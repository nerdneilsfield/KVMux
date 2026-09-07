#include "video/video_pipeline.hpp"

#include "video/video_processor.hpp"

namespace kvmux {

VideoPipeline::VideoPipeline(CaptureSource& source) : source_(source) {}
VideoPipeline::~VideoPipeline() { stop(); }

void VideoPipeline::start(std::uint64_t generation) {
    stop();
    frames_.set_generation(generation);
    {
        std::lock_guard lock(mutex_);
        snapshot_ = {};
    }
    stopping_.store(false, std::memory_order_release);
    worker_ = std::thread([this] { run(); });
}

void VideoPipeline::set_generation(std::uint64_t generation) {
    frames_.set_generation(generation);
}

void VideoPipeline::stop() noexcept {
    stopping_.store(true, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
}

std::optional<VideoFrame> VideoPipeline::take_latest_frame() { return frames_.take(); }

VideoPipelineSnapshot VideoPipeline::snapshot() const {
    std::lock_guard lock(mutex_);
    auto value = snapshot_;
    value.overwritten_frames = frames_.overwritten();
    return value;
}

void VideoPipeline::run() {
    VideoProcessor processor;
    while (!stopping_.load(std::memory_order_acquire)) {
        auto sample = source_.take_latest_sample();
        if (!sample) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        auto frame = processor.process(*sample);
        if (!frame) {
            std::lock_guard lock(mutex_);
            ++snapshot_.rejected_samples;
            snapshot_.error = processor.last_error();
            continue;
        }
        const auto arrival = frame->arrival;
        frames_.publish(std::move(*frame));
        std::lock_guard lock(mutex_);
        ++snapshot_.processed_frames;
        snapshot_.latest_arrival = arrival;
        snapshot_.error.clear();
    }
}

}  // namespace kvmux
