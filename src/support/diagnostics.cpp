#include "support/diagnostics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <spdlog/logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

namespace kvmux {
namespace {

constexpr std::size_t kWindowCapacity = 256;
constexpr std::size_t kLogFileBytes = 10U * 1024U * 1024U;
constexpr std::size_t kRotatedLogFiles = 3;

template <typename T, std::size_t Capacity>
class SlidingWindow {
public:
    void push(T value) noexcept {
        values_[next_] = std::move(value);
        next_ = (next_ + 1U) % Capacity;
        size_ = std::min(size_ + 1U, Capacity);
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    [[nodiscard]] const T& ordered(std::size_t index) const noexcept {
        const std::size_t first = size_ == Capacity ? next_ : 0U;
        return values_[(first + index) % Capacity];
    }

private:
    std::array<T, Capacity> values_{};
    std::size_t next_{};
    std::size_t size_{};
};

using TimeWindow = SlidingWindow<Diagnostics::Clock::time_point, kWindowCapacity>;
using DurationWindow = SlidingWindow<Diagnostics::Duration, kWindowCapacity>;

double fps(const TimeWindow& window) noexcept {
    if (window.size() < 2U) return 0.0;
    const auto elapsed = window.ordered(window.size() - 1U) - window.ordered(0U);
    const double seconds = std::chrono::duration<double>(elapsed).count();
    return seconds > 0.0 ? static_cast<double>(window.size() - 1U) / seconds : 0.0;
}

LatencySnapshot latency_snapshot(const DurationWindow& window) {
    LatencySnapshot result;
    result.samples = window.size();
    if (window.size() == 0U) return result;

    std::array<double, kWindowCapacity> milliseconds{};
    double total = 0.0;
    for (std::size_t i = 0; i < window.size(); ++i) {
        milliseconds[i] = std::chrono::duration<double, std::milli>(window.ordered(i)).count();
        total += milliseconds[i];
    }
    result.latest_ms = milliseconds[window.size() - 1U];
    result.mean_ms = total / static_cast<double>(window.size());

    const std::size_t rank = static_cast<std::size_t>(
        std::ceil(0.95 * static_cast<double>(window.size())));
    std::nth_element(milliseconds.begin(), milliseconds.begin() + (rank - 1U),
                     milliseconds.begin() + static_cast<std::ptrdiff_t>(window.size()));
    result.p95_ms = milliseconds[rank - 1U];
    return result;
}

Diagnostics::Duration nonnegative(Diagnostics::Duration duration) noexcept {
    return std::max(duration, Diagnostics::Duration::zero());
}

}  // namespace

class Diagnostics::Impl {
public:
    mutable std::mutex mutex;
    TimeWindow capture;
    TimeWindow decode;
    TimeWindow present;
    DurationWindow sample_to_gpu_submit;
    DurationWindow sdl_to_control_dequeue;
    DurationWindow input_queue_age;
    DurationWindow ack_rtt;
    DurationWindow present_blocking;
    std::optional<std::pair<std::uint64_t, std::uint64_t>> last_present;
    std::uint64_t sample_mailbox_overwrites{};
    std::uint64_t frame_mailbox_overwrites{};
    std::uint64_t timeouts{};
    std::string capture_mode;
    std::string pixel_path;
    std::string recent_error;
};

Diagnostics::Diagnostics() : impl_(std::make_unique<Impl>()) {}
Diagnostics::~Diagnostics() = default;

void Diagnostics::record_capture(const Clock::time_point at) noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->capture.push(at);
}

void Diagnostics::record_decode(const Clock::time_point at) noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->decode.push(at);
}

void Diagnostics::record_present(const std::uint64_t generation,
                                 const std::uint64_t capture_sequence,
                                 const Clock::time_point at) noexcept {
    std::lock_guard lock(impl_->mutex);
    const auto identity = std::pair{generation, capture_sequence};
    if (!impl_->last_present || *impl_->last_present != identity) {
        impl_->present.push(at);
        impl_->last_present = identity;
    }
}

void Diagnostics::record_sample_to_gpu_submit(const Duration duration) noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->sample_to_gpu_submit.push(nonnegative(duration));
}
void Diagnostics::record_sdl_to_control_dequeue(const Duration duration) noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->sdl_to_control_dequeue.push(nonnegative(duration));
}
void Diagnostics::record_input_queue_age(const Duration duration) noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->input_queue_age.push(nonnegative(duration));
}
void Diagnostics::record_ack_rtt(const Duration duration) noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->ack_rtt.push(nonnegative(duration));
}
void Diagnostics::record_present_blocking(const Duration duration) noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->present_blocking.push(nonnegative(duration));
}

void Diagnostics::set_mailbox_overwrites(const std::uint64_t sample_mailbox,
                                         const std::uint64_t frame_mailbox) noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->sample_mailbox_overwrites = sample_mailbox;
    impl_->frame_mailbox_overwrites = frame_mailbox;
}

void Diagnostics::record_timeout() noexcept {
    std::lock_guard lock(impl_->mutex);
    ++impl_->timeouts;
}

void Diagnostics::set_capture_mode(const std::string_view mode) {
    std::lock_guard lock(impl_->mutex);
    impl_->capture_mode.assign(mode);
}
void Diagnostics::set_pixel_path(const std::string_view path) {
    std::lock_guard lock(impl_->mutex);
    impl_->pixel_path.assign(path);
}
void Diagnostics::set_recent_error(const std::string_view error) {
    std::lock_guard lock(impl_->mutex);
    impl_->recent_error.assign(error);
}
void Diagnostics::clear_recent_error() {
    std::lock_guard lock(impl_->mutex);
    impl_->recent_error.clear();
}

DiagnosticsSnapshot Diagnostics::snapshot() const {
    std::lock_guard lock(impl_->mutex);
    DiagnosticsSnapshot result;
    result.capture_fps = fps(impl_->capture);
    result.decode_fps = fps(impl_->decode);
    result.unique_present_fps = fps(impl_->present);
    result.sample_to_gpu_submit = latency_snapshot(impl_->sample_to_gpu_submit);
    result.sdl_to_control_dequeue = latency_snapshot(impl_->sdl_to_control_dequeue);
    result.input_queue_age = latency_snapshot(impl_->input_queue_age);
    result.ack_rtt = latency_snapshot(impl_->ack_rtt);
    result.present_blocking = latency_snapshot(impl_->present_blocking);
    result.sample_mailbox_overwrites = impl_->sample_mailbox_overwrites;
    result.frame_mailbox_overwrites = impl_->frame_mailbox_overwrites;
    result.timeouts = impl_->timeouts;
    result.capture_mode = impl_->capture_mode;
    result.pixel_path = impl_->pixel_path;
    result.recent_error = impl_->recent_error;
    return result;
}

std::shared_ptr<spdlog::logger> initialize_logging(const std::string& log_file) {
    auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        log_file, kLogFileBytes, kRotatedLogFiles);
    auto logger = std::make_shared<spdlog::logger>("kvmux", std::move(sink));
    logger->set_level(spdlog::level::info);
    logger->flush_on(spdlog::level::warn);
    logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%l] %v");
    spdlog::set_default_logger(logger);
    return logger;
}

}  // namespace kvmux
