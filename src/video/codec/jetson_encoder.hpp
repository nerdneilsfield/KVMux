#pragma once
#include "video/codec/video_codec.hpp"
namespace kvmux {
[[nodiscard]] std::unique_ptr<VideoEncoder> create_jetson_encoder(std::string& error);
}  // namespace kvmux
