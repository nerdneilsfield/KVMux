#pragma once

#include "video/capture_sample.hpp"
#include "video/video_frame.hpp"

#include <memory>
#include <optional>
#include <string>

struct AVCodecContext;

namespace kvmux {

class VideoProcessor {
public:
    VideoProcessor();
    ~VideoProcessor();
    VideoProcessor(const VideoProcessor&) = delete;
    VideoProcessor& operator=(const VideoProcessor&) = delete;

    [[nodiscard]] std::optional<VideoFrame> process(const CaptureSample& sample);
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

private:
    [[nodiscard]] std::optional<VideoFrame> process_raw(const CaptureSample& sample);
    [[nodiscard]] std::optional<VideoFrame> process_mjpeg(const CaptureSample& sample);
    bool ensure_mjpeg_decoder();
    void fail(std::string message);

    AVCodecContext* decoder_{};
    std::string last_error_;
};

}  // namespace kvmux
