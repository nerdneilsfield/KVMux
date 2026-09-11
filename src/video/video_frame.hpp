#pragma once

#include "video/capture_sample.hpp"

#include <chrono>
#include <cstdint>
#include <memory>

extern "C" {
#include <libavutil/frame.h>
}

namespace kvmux {

using AvFramePtr = std::shared_ptr<AVFrame>;

[[nodiscard]] inline ColorRange frame_color_range(const AVFrame& frame) noexcept {
    if (frame.color_range == AVCOL_RANGE_JPEG) return ColorRange::full;
    if (frame.color_range == AVCOL_RANGE_MPEG) return ColorRange::limited;
    // Legacy JPEG pixel formats encode full range even without separate metadata.
    switch (static_cast<AVPixelFormat>(frame.format)) {
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_YUVJ444P: return ColorRange::full;
    default: return ColorRange::unknown;
    }
}

struct VideoFrame {
    std::uint64_t generation{};
    std::uint64_t sequence{};
    std::chrono::steady_clock::time_point arrival{};
    std::chrono::steady_clock::time_point decoded{};
    std::optional<std::int64_t> device_timestamp;
    std::int32_t device_time_base_numerator{};
    std::int32_t device_time_base_denominator{1};
    std::uint32_t sample_aspect_ratio_numerator{1};
    std::uint32_t sample_aspect_ratio_denominator{1};
    ColorRange color_range{ColorRange::unknown};
    ColorMatrix color_matrix{ColorMatrix::unknown};
    AvFramePtr frame;
};

}  // namespace kvmux
