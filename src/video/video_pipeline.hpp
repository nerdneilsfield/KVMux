#pragma once

#include "video/capture/capture_source.hpp"
#include "video/video_frame.hpp"
#include "video/video_mailbox.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace kvmux {

struct VideoPipelineSnapshot {
    std::uint64_t processed_frames{};
    std::uint64_t rejected_samples{};
    std::uint64_t overwritten_frames{};
    std::chrono::steady_clock::time_point latest_arrival{};
    std::string error;
};

class VideoPipeline {
public:
    explicit VideoPipeline(CaptureSource& source);
    ~VideoPipeline();
    VideoPipeline(const VideoPipeline&) = delete;
    VideoPipeline& operator=(const VideoPipeline&) = delete;

    void start(std::uint64_t generation);
    void set_generation(std::uint64_t generation);
    void stop() noexcept;
    [[nodiscard]] std::optional<VideoFrame> take_latest_frame();
    [[nodiscard]] VideoPipelineSnapshot snapshot() const;

private:
    void run();

    CaptureSource& source_;
    GenerationMailbox<VideoFrame> frames_;
    std::thread worker_;
    std::atomic_bool stopping_{true};
    mutable std::mutex mutex_;
    VideoPipelineSnapshot snapshot_;
};

}  // namespace kvmux
