#include "network/kcp_channel.hpp"
#include "network/udp_socket.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace std::chrono_literals;
using kvmux::relay::KcpChannel;
using kvmux::relay::SubmitResult;
using kvmux::udp::ReceiveStatus;
using kvmux::udp::SendStatus;
using kvmux::udp::Socket;
using Bytes = std::vector<std::uint8_t>;
void check(bool ok) {
    if (!ok) { std::cerr << "UDP/KCP check failed\n"; std::abort(); }
}
void put32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}
Bytes segment(std::uint8_t command, std::uint32_t sequence = 0, std::size_t size = 1) {
    Bytes bytes(24 + size);
    put32(bytes, 0, 42);
    bytes[4] = command;
    bytes[6] = 128;
    put32(bytes, 12, sequence);
    put32(bytes, 20, static_cast<std::uint32_t>(size));
    return bytes;
}
void bounds() {
    KcpChannel sender(42);
    check(sender.submit({}) == SubmitResult::invalid);
    check(sender.submit(Bytes(1025)) == SubmitResult::invalid);
    for (int i = 0; i < 128; ++i) check(sender.submit(Bytes(1024, 7)) == SubmitResult::accepted);
    check(sender.submit(Bytes{8}) == SubmitResult::full);

    // Optional traffic may fill only 127 slots while a paste lifecycle message
    // needs the final bounded slot. Its Commit can then enter immediately.
    KcpChannel reserved(42);
    for (int i = 0; i < 127; ++i)
        check(reserved.submit(Bytes{7}, 1) == SubmitResult::accepted);
    check(reserved.submit(Bytes{7}, 1) == SubmitResult::full);
    check(reserved.submit(Bytes{8}) == SubmitResult::accepted);

    // Paste chunks and ordinary traffic reserve one bounded lifecycle slot.
    // A coalesced executing keepalive therefore still enters when all ordinary
    // capacity is occupied; no retry queue is needed.
    KcpChannel executing(42);
    for (int i = 0; i < 127; ++i)
        check(executing.submit(Bytes{7}, 1) == SubmitResult::accepted);
    check(executing.submit(Bytes{13}) == SubmitResult::accepted); // PasteKeepalive
    check(executing.submit(Bytes{9}) == SubmitResult::full);

    sender.update(0);
    auto packets = sender.take_datagrams();
    check(packets.size() == 128);
    for (const auto& packet : packets) check(packet.size() <= 1168);
    check(sender.submit(Bytes{8}) == SubmitResult::full);

    KcpChannel receiver(42);
    check(!receiver.input({}));
    check(!receiver.input(Bytes(1169)));
    const auto valid = segment(81);
    for (auto offset : {0U, 4U, 5U, 6U, 20U}) {
        auto malformed = valid;
        malformed[offset] = 255;
        check(!receiver.input(malformed));
    }
    auto suffix = valid;
    suffix.push_back(0); // Whole datagram rejection, not a partially accepted prefix.
    check(!receiver.input(suffix));
    check(!receiver.receive());
    check(!receiver.input(segment(81, 0, 0)));
    check(!receiver.input(segment(81, 0, 1025)));
    for (std::uint8_t command : {82, 83, 84}) {
        check(!receiver.input(segment(command)));
        check(receiver.input(segment(command, 0, 0)));
    }
    for (int i = 0; i < 256; ++i) check(receiver.input(valid));
    check(!receiver.input(valid)); // ACK allocation cap is recoverable datagram loss.
    check(!receiver.failed());
    receiver.update(0);
    check(receiver.input(valid));
    check(receiver.receive() == std::optional<Bytes>(Bytes{0}));
    check(!receiver.receive());
    check(receiver.take_datagrams().size() <= 256);

    // Stop draining native output: its hard cap fails rather than growing forever.
    KcpChannel overflow(42);
    for (int i = 0; i < 128; ++i) check(overflow.submit(Bytes(1024)) == SubmitResult::accepted);
    for (std::uint32_t t = 0; t <= 2000 && !overflow.failed(); t += 10) overflow.update(t);
    check(overflow.failed());
    check(overflow.take_datagrams().size() == 256);

    // A relay must retain a taken batch while its UDP send would-blocks and not
    // call update again. Once the batch drains, normal updates remain healthy.
    KcpChannel held(42);
    for (int i = 0; i < 128; ++i) check(held.submit(Bytes(1024)) == SubmitResult::accepted);
    held.update(0);
    auto held_batch = held.take_datagrams();
    check(held_batch.size() == 128);
    // Simulated would-block: relay holds held_batch and deliberately skips update.
    check(!held.failed());
    held_batch.clear(); // Socket becomes writable and the retained batch drains.
    held.update(10);
    check(!held.failed());
    check(held.take_datagrams().size() <= 128);

    // A stopped application reader advertises zero window; ingress remains bounded.
    KcpChannel slow(42);
    for (std::uint32_t sn = 0; sn < 400; ++sn) {
        check(slow.input(segment(81, sn)));
        slow.update(sn * 10);
        (void)slow.take_datagrams();
    }
    unsigned count = 0;
    while (slow.receive()) ++count;
    check(count == 256); // 128 ready + at most 128 in KCP's reorder window.
}
void impairment() {
    KcpChannel sender(42), receiver(42);
    for (unsigned i = 0; i < 100; ++i)
        check(sender.submit(Bytes(64, static_cast<std::uint8_t>(i))) == SubmitResult::accepted);
    struct Flight { std::uint32_t due; bool to_receiver; Bytes bytes; };
    std::vector<Flight> flights;
    unsigned ordinal = 0, received = 0;
    bool prefix_withheld = false;
    for (std::uint32_t t = 0; t <= 15000; t += 10) {
        sender.update(t);
        receiver.update(t);
        auto enqueue = [&](KcpChannel& from, bool to_receiver) {
            auto packets = from.take_datagrams();
            check(packets.size() <= 256);
            for (auto& packet : packets) {
                check(packet.size() <= 1168);
                const auto n = ++ordinal;
                // Lose first prefix and all retransmissions until the blackout ends.
                if ((to_receiver && t < 1000 && packet[4] == 81 && packet[12] == 0) ||
                    (t >= 200 && t < 1000) || n % 7 == 0) continue;
                const auto delay = (n % 3 == 0) ? 70U : 10U;
                if (n % 11 == 0) flights.push_back({t + delay + 20, to_receiver, packet});
                flights.push_back({t + delay, to_receiver, std::move(packet)});
            }
        };
        enqueue(sender, true);
        enqueue(receiver, false);
        check(flights.size() < 1024);
        for (auto it = flights.begin(); it != flights.end();) {
            if (it->due <= t) {
                check((it->to_receiver ? receiver : sender).input(it->bytes));
                it = flights.erase(it);
            } else ++it;
        }
        while (auto message = receiver.receive()) {
            check(t >= 1000); // Never publish later messages across the missing prefix.
            check(*message == Bytes(64, static_cast<std::uint8_t>(received)));
            ++received;
        }
        if (t == 900) { check(received == 0); prefix_withheld = true; }
        check(!sender.failed() && !receiver.failed());
        if (received == 100 && t >= 1200) break;
    }
    check(prefix_withheld && received == 100);
}
void native_roundtrip() {
    std::string error;
    auto left = Socket::bind("127.0.0.1", 0, error);
    auto right = Socket::bind("127.0.0.1", 0, error);
    check(left && right && error.empty());
    check(left->local_port() && right->local_port());
    auto a = kvmux::udp::resolve("localhost", *left->local_port(), error);
    auto b = kvmux::udp::resolve("127.0.0.1", *right->local_port(), error);
    check(a && b && *a != *b);
    check(*a == *kvmux::udp::resolve("127.0.0.1", *left->local_port(), error));
    const auto started = std::chrono::steady_clock::now();
    check(right->receive(20ms).status == ReceiveStatus::idle);
    check(std::chrono::steady_clock::now() - started < 500ms);
    check(left->send_to(*b, Bytes(1201)) == SendStatus::invalid);
    check(left->send_to(*b, Bytes(1200, 1)) == SendStatus::sent);
    check(left->send_to(*b, Bytes{2, 3}) == SendStatus::sent);
    auto datagram = right->receive(100ms);
    check(datagram.status == ReceiveStatus::datagram && datagram.datagram.bytes == Bytes(1200, 1));
    check(datagram.datagram.source == *a);
    datagram = right->receive(100ms);
    check(datagram.status == ReceiveStatus::datagram && datagram.datagram.bytes == Bytes({2, 3}));

    // External peer can exceed our send API's bound. Never expose a truncated prefix.
    const auto raw = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(*right->local_port());
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const Bytes oversized(2000, 9);
    check(::sendto(raw, reinterpret_cast<const char*>(oversized.data()), 2000, 0,
        reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 2000);
#ifdef _WIN32
    closesocket(raw);
#else
    ::close(raw);
#endif
    check(right->receive(100ms).status == ReceiveStatus::oversized);
    check(right->receive(0ms).status == ReceiveStatus::idle);

    KcpChannel client(42), server(42);
    for (unsigned i = 0; i < 16; ++i)
        check(client.submit(Bytes(32, static_cast<std::uint8_t>(i))) == SubmitResult::accepted);
    unsigned echoed = 0, received = 0;
    for (std::uint32_t t = 0; t <= 2000 && received < 16; t += 10) {
        client.update(t);
        server.update(t);
        auto transfer = [](KcpChannel& from, Socket& source, const kvmux::udp::Endpoint& to,
                           Socket& destination, KcpChannel& target) {
            for (auto& packet : from.take_datagrams()) {
                check(source.send_to(to, packet) == SendStatus::sent);
                auto result = destination.receive(100ms);
                check(result.status == ReceiveStatus::datagram);
                check(target.input(result.datagram.bytes));
            }
        };
        transfer(client, *left, *b, *right, server);
        transfer(server, *right, *a, *left, client);
        while (auto message = server.receive()) {
            check(*message == Bytes(32, static_cast<std::uint8_t>(echoed++)));
            check(server.submit(*message) == SubmitResult::accepted);
        }
        while (auto message = client.receive())
            check(*message == Bytes(32, static_cast<std::uint8_t>(received++)));
    }
    check(echoed == 16 && received == 16);
    const auto closing = std::chrono::steady_clock::now();
    left->close(); right->close();
    check(left->receive(1s).status == ReceiveStatus::error);
    check(!left->local_port());
    check(std::chrono::steady_clock::now() - closing < 100ms);
}
int main() {
    bounds();
    impairment();
    native_roundtrip();
    std::cout << "UDP atomicity, bounded KCP, impairment recovery and native roundtrip passed\n";
}
