#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace spdlog {
class logger;
}

namespace kvmux {

// A compact description of one fixed-capacity latency window.
struct LatencySnapshot {
    std::size_t samples{};
    double latest_ms{};
    double mean_ms{};
    double p95_ms{};
};

struct DiagnosticsSnapshot {
    double capture_fps{};
    double decode_fps{};
    double unique_present_fps{};

    LatencySnapshot sample_to_gpu_submit;
    LatencySnapshot sdl_to_control_dequeue;
    LatencySnapshot input_queue_age;
    LatencySnapshot ack_rtt;
    LatencySnapshot present_blocking;

    std::uint64_t sample_mailbox_overwrites{};
    std::uint64_t frame_mailbox_overwrites{};
    std::uint64_t timeouts{};
    std::string capture_mode;
    std::string pixel_path;
    std::string recent_error;
};

// Thread-safe, allocation-free on the metric recording path. All timestamps
// must come from std::chrono::steady_clock; device PTS values are not accepted.
class Diagnostics {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;

    Diagnostics();
    ~Diagnostics();
    Diagnostics(const Diagnostics&) = delete;
    Diagnostics& operator=(const Diagnostics&) = delete;

    void record_capture(Clock::time_point at = Clock::now()) noexcept;
    void record_decode(Clock::time_point at = Clock::now()) noexcept;
    void record_present(std::uint64_t generation, std::uint64_t capture_sequence,
                        Clock::time_point at = Clock::now()) noexcept;

    void record_sample_to_gpu_submit(Duration duration) noexcept;
    void record_sdl_to_control_dequeue(Duration duration) noexcept;
    void record_input_queue_age(Duration duration) noexcept;
    void record_ack_rtt(Duration duration) noexcept;
    void record_present_blocking(Duration duration) noexcept;

    void set_mailbox_overwrites(std::uint64_t sample_mailbox,
                                std::uint64_t frame_mailbox) noexcept;
    void record_timeout() noexcept;
    void set_capture_mode(std::string_view mode);
    void set_pixel_path(std::string_view path);
    void set_recent_error(std::string_view error);
    void clear_recent_error();

    [[nodiscard]] DiagnosticsSnapshot snapshot() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Initializes the process default logger for low-frequency state changes and
// aggregate diagnostics. The active log and rotated files are capped at
// 10 MiB each, with three rotated files retained. Metric hot paths do not log.
[[nodiscard]] std::shared_ptr<spdlog::logger> initialize_logging(
    const std::string& log_file);

}  // namespace kvmux
