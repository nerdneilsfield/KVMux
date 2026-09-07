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

struct VideoFrame {
    std::uint64_t generation{};
    std::uint64_t sequence{};
    std::chrono::steady_clock::time_point arrival{};
    std::chrono::steady_clock::time_point decoded{};
    ColorRange color_range{ColorRange::unknown};
    ColorMatrix color_matrix{ColorMatrix::unknown};
    AvFramePtr frame;
};

}  // namespace kvmux
