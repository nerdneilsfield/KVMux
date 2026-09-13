#include "network/kcp_channel.hpp"

#include <ikcp.h>
#include <utility>

namespace kvmux::relay {
namespace {
constexpr std::size_t mtu = 1168;
constexpr std::size_t max_message = 1024;
constexpr unsigned window = 128;
constexpr std::size_t max_output = 256;
constexpr unsigned max_pending_acks = 256;
std::uint32_t read32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return std::uint32_t{bytes[offset]} | (std::uint32_t{bytes[offset + 1]} << 8) |
           (std::uint32_t{bytes[offset + 2]} << 16) | (std::uint32_t{bytes[offset + 3]} << 24);
}
}  // namespace
KcpChannel::KcpChannel(std::uint32_t conversation) : kcp_(ikcp_create(conversation, this)) {
    if (!kcp_) { failed_ = true; return; }
    ikcp_setoutput(kcp_, &KcpChannel::output);
    failed_ = ikcp_setmtu(kcp_, static_cast<int>(mtu)) != 0;
    ikcp_wndsize(kcp_, window, window);
    ikcp_nodelay(kcp_, 1, 10, 2, 1);
}
KcpChannel::~KcpChannel() { if (kcp_) ikcp_release(kcp_); }
SubmitResult KcpChannel::submit(std::span<const std::uint8_t> message, unsigned reserve_slots) {
    if (failed_ || message.empty() || message.size() > max_message || reserve_slots >= window) return SubmitResult::invalid;
    if (ikcp_waitsnd(kcp_) >= static_cast<int>(window - reserve_slots)) return SubmitResult::full;
    if (ikcp_send(kcp_, reinterpret_cast<const char*>(message.data()), static_cast<int>(message.size())) < 0) {
        failed_ = true;
        return SubmitResult::invalid;
    }
    return SubmitResult::accepted;
}
bool KcpChannel::input(std::span<const std::uint8_t> packet) {
    if (failed_ || packet.empty() || packet.size() > mtu) return false;
    unsigned pushes = 0;
    // Validate the WHOLE packet before KCP can mutate state or allocate segments.
    for (std::size_t offset = 0; offset < packet.size();) {
        if (packet.size() - offset < 24 || read32(packet, offset) != kcp_->conv) return false;
        const auto command = packet[offset + 4];
        const auto fragment = packet[offset + 5];
        const auto peer_window = unsigned{packet[offset + 6]} | (unsigned{packet[offset + 7]} << 8);
        const auto length = read32(packet, offset + 20);
        // Our 1024-byte messages fit one segment. ACK/probes have no payload.
        if (fragment != 0 || peer_window > window || length > packet.size() - offset - 24) return false;
        if (command == 81) {
            if (length == 0 || length > max_message) return false;
            ++pushes;
        } else if (command < 82 || command > 84 || length != 0) {
            return false;
        }
        offset += 24 + length;
    }
    // Upstream ACK storage otherwise grows without bound on duplicate PUSH floods.
    // Dropped input is retried by the peer after the next update flushes our ACKs.
    if (kcp_->ackcount + pushes > max_pending_acks) return false;
    return ikcp_input(kcp_, reinterpret_cast<const char*>(packet.data()), static_cast<long>(packet.size())) == 0;
}
void KcpChannel::update(std::uint32_t monotonic_ms) {
    if (failed_) return;
    ikcp_update(kcp_, monotonic_ms);
    if (kcp_->state == static_cast<IUINT32>(-1)) failed_ = true;
}
int KcpChannel::output(const char* data, int size, IKCPCB*, void* user) {
    auto& self = *static_cast<KcpChannel*>(user);
    if (self.failed_) return -1;
    if (size <= 0 || static_cast<std::size_t>(size) > mtu || self.output_.size() >= max_output) {
        self.failed_ = true;
        return -1;
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data);
    self.output_.emplace_back(bytes, bytes + size);
    return 0;
}
std::vector<std::vector<std::uint8_t>> KcpChannel::take_datagrams() {
    return std::exchange(output_, {});
}
std::optional<std::vector<std::uint8_t>> KcpChannel::receive() {
    if (failed_) return std::nullopt;
    const int size = ikcp_peeksize(kcp_);
    if (size < 0) return std::nullopt;
    if (size == 0 || size > static_cast<int>(max_message)) { failed_ = true; return std::nullopt; }
    std::vector<std::uint8_t> message(static_cast<std::size_t>(size));
    if (ikcp_recv(kcp_, reinterpret_cast<char*>(message.data()), size) != size) {
        failed_ = true;
        return std::nullopt;
    }
    return message;
}
}  // namespace kvmux::relay
