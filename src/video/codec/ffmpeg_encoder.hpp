#pragma once
#include "video/codec/video_codec.hpp"

namespace kvmux {
[[nodiscard]] std::unique_ptr<VideoEncoder> create_ffmpeg_encoder(
    CodecBackend backend, std::string& error);
}  // namespace kvmux
