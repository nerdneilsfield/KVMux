#pragma once
#include "control/control_sink.hpp"
#include "video/capture/capture_source.hpp"
#include <span>
#include "video/codec/video_codec.hpp"

namespace kvmux::relay {
// Explicit choices are validated, never replaced by an automatic fallback.
std::string select_device(std::span<const DeviceInfo> devices,
                          const std::optional<std::string>& requested = {});
std::size_t select_mode(std::span<const CaptureMode> modes,
                        std::optional<std::size_t> requested = {}, VideoCodec codec = VideoCodec::mjpeg);
std::string select_serial(std::span<const SerialPortInfo> ports,
                          const std::optional<std::string>& requested = {});
}  // namespace kvmux::relay
