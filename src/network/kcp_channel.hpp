#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

struct IKCPCB;
namespace kvmux::relay {
enum class SubmitResult { accepted, full, invalid };

// Single owner thread; update every 10 ms, including when the application is idle.
// Transport ACKs do not confirm execution by the serial device.
class KcpChannel {
public:
    explicit KcpChannel(std::uint32_t conversation);
    ~KcpChannel();
    KcpChannel(const KcpChannel&) = delete;
    KcpChannel& operator=(const KcpChannel&) = delete;
    [[nodiscard]] SubmitResult submit(std::span<const std::uint8_t> message);
    [[nodiscard]] bool input(std::span<const std::uint8_t> packet);
    void update(std::uint32_t monotonic_ms);
    [[nodiscard]] std::vector<std::vector<std::uint8_t>> take_datagrams();
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> receive();
    [[nodiscard]] bool failed() const noexcept { return failed_; }
private:
    static int output(const char*, int, IKCPCB*, void*);
    IKCPCB* kcp_{};
    bool failed_{};
    std::vector<std::vector<std::uint8_t>> output_;
};
}  // namespace kvmux::relay
