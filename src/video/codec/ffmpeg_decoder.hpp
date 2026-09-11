#pragma once
#include "video/codec/video_codec.hpp"

namespace kvmux {
// Only explicit VideoToolbox or software choices; automatic selection is the factory's job.
[[nodiscard]] std::unique_ptr<VideoDecoder> create_ffmpeg_decoder(
    CodecBackend backend, std::string& error);
}  // namespace kvmux
