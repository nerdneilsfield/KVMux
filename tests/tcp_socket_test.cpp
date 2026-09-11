#include "network/tcp_socket.hpp"
#include "network/relay_protocol.hpp"

#include <array>
#include <cassert>
#include <thread>
#include <sstream>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/ostream_sink.h>

using namespace std::chrono_literals;
using kvmux::tcp::Listener;
using kvmux::tcp::Socket;
using Clock = std::chrono::steady_clock;

int main() {
    std::ostringstream diagnostics;
    auto log_sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(diagnostics);
    auto logger = std::make_shared<spdlog::logger>("tcp-test", log_sink);
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);
    std::string error = "stale error";
    auto listener = Listener::bind("127.0.0.1", 0, error);
    assert(listener && error.empty());
    const auto port = listener->local_port();
    assert(port && *port != 0);
    assert(!listener->accept(20ms));

    auto client = kvmux::tcp::connect("127.0.0.1", *port, 1s, error);
    assert(client && error.empty());
    auto server = listener->accept(1s);
    assert(server);
    assert(client->send_all({}, 0ms));
    assert(server->receive_exact(0, 0ms)->empty());

    // Transfer more than one typical socket buffer in both directions.
    std::vector<std::uint8_t> payload(1024 * 1024);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::uint8_t>(i % 251);
    std::jthread echo([&] {
        const auto received = server->receive_exact(payload.size(), 2s);
        assert(received && *received == payload);
        assert(server->send_all(*received));
    });
    assert(client->send_all(payload));
    const auto response = client->receive_exact(payload.size(), 2s);
    assert(response && *response == payload);
    echo.join();
    assert(diagnostics.str().empty()); // Successful traffic does not log per frame.

    // A trickle must not restart receive_exact's total timeout per byte.
    std::jthread trickle([&](std::stop_token stop) {
        const std::array<std::uint8_t, 1> byte{42};
        while (!stop.stop_requested()) {
            if (!server->send_all(byte, 100ms)) break;
            std::this_thread::sleep_for(40ms);
        }
    });
    auto started = Clock::now();
    assert(!client->receive_exact(20, 150ms));
    auto elapsed = Clock::now() - started;
    assert(elapsed >= 100ms && elapsed < 500ms);
    assert(diagnostics.str().find("receive failed: reason=deadline error=0 bytes=") != std::string::npos);
    diagnostics.str("");
    trickle.request_stop();
    trickle.join();

    // A peer that stops reading must not hold send_all past its deadline.
    std::vector<std::uint8_t> blocked_payload(32 * 1024 * 1024, 7);
    started = Clock::now();
    const auto partial_send=client->send_all(blocked_payload, 150ms);
    assert(partial_send.status==kvmux::tcp::SendStatus::deadline);
    assert(partial_send.bytes_sent>0 && partial_send.bytes_sent<blocked_payload.size());
    assert(!partial_send.unsent_deadline()); // A partial packet must close, not skip bytes.
    elapsed = Clock::now() - started;
    assert(elapsed >= 100ms && elapsed < 500ms);
    assert(diagnostics.str().find("send failed: reason=deadline error=0 bytes=") != std::string::npos);
    assert(diagnostics.str().find("/33554432 elapsed_us=") != std::string::npos);
    assert(diagnostics.str().find("timeout_ms=150") != std::string::npos);
    diagnostics.str("");
    client->close();
    server->close();

    // Saturate a real socket, then drop only packets for which not even the
    // header was written. Draining the queued bytes must restore clean framing.
    client = kvmux::tcp::connect("127.0.0.1", *port, 1s, error);
    server = listener->accept(1s);
    assert(client && server);
    std::size_t queued{};
    bool blocked=false;
    for(int attempt=0;attempt<20&&!blocked;++attempt) {
        const auto result=client->send_all(blocked_payload,100ms);
        queued+=result.bytes_sent;
        assert(result.status==kvmux::tcp::SendStatus::deadline);
        blocked=result.unsent_deadline();
    }
    assert(blocked && queued>0);
    diagnostics.str("");
    const auto skipped=kvmux::relay::encode_hello({41,kvmux::VideoCodec::mjpeg});
    const auto dropped=kvmux::relay::send_packet(*client,kvmux::relay::PacketType::hello,skipped,100ms);
    assert(dropped.unsent_deadline());
    assert(diagnostics.str().empty()); // Recoverable drops do not flood the socket log.
    assert(server->receive_exact(queued,2s));
    const auto latest=kvmux::relay::encode_hello({42,kvmux::VideoCodec::mjpeg});
    assert(kvmux::relay::send_packet(*client,kvmux::relay::PacketType::hello,latest,1s));
    const auto resumed=kvmux::relay::receive_packet(*server,1s);
    assert(resumed && resumed->type==kvmux::relay::PacketType::hello && resumed->payload==latest);
    client->close();server->close();

    // FIN before the requested byte count returns promptly, rather than waiting.
    client = kvmux::tcp::connect("127.0.0.1", *port, 1s, error);
    server = listener->accept(1s);
    assert(client && server);
    const std::array<std::uint8_t, 3> partial{1, 2, 3};
    assert(server->send_all(partial));
    server->close();
    started = Clock::now();
    assert(!client->receive_exact(4, 1s));
    assert(Clock::now() - started < 500ms);
    assert(diagnostics.str().find("receive failed: reason=peer closed error=0 bytes=3/4") != std::string::npos);
    // A closed peer must eventually reject writes without raising SIGPIPE.
    bool rejected = false;
    for (int i = 0; i < 10 && !rejected; ++i) rejected = !client->send_all(payload, 100ms);
    assert(rejected);
    client->close();
    assert(!client->send_all(partial));
    assert(!client->receive_exact(1, 10ms));

    listener.reset();
    assert(!kvmux::tcp::connect("127.0.0.1", *port, 200ms, error));
    assert(!error.empty());
}
