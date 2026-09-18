#include "network/relay_selection.hpp"

#include <algorithm>
#include <stdexcept>
#include <tuple>

namespace kvmux::relay {
namespace {
bool raw(PixelFormat format) {
  return format >= PixelFormat::yuy2 && format <= PixelFormat::rgba;
}
bool usable(const CaptureMode& mode, VideoCodec codec) {
  const bool format = codec == VideoCodec::mjpeg
                          ? mode.device_format == PixelFormat::mjpeg &&
                                mode.delivered_format == PixelFormat::mjpeg
                          : raw(mode.device_format) &&
                                raw(mode.delivered_format) &&
                                mode.width % 2 == 0 && mode.height % 2 == 0;
  return format && valid_dimensions(mode.width, mode.height) &&
         mode.frame_rate.numerator > 0 && mode.frame_rate.denominator > 0;
}
int priority(const CaptureMode& mode) {
  const auto n = static_cast<std::int64_t>(mode.frame_rate.numerator);
  const auto d = static_cast<std::int64_t>(mode.frame_rate.denominator);
  // Include both exact NTSC rates and devices advertising decimal 59.94/29.97.
  const bool sixty = n * 100 >= d * 5994 && n <= d * 60;
  const bool thirty = n * 100 >= d * 2997 && n <= d * 30;
  if (mode.width == 1920 && mode.height == 1080) {
    if (sixty) return 0;
    if (thirty) return 2;
  }
  if (mode.width == 1280 && mode.height == 720) {
    if (sixty) return 1;
    if (thirty) return 3;
  }
  return 4;
}
bool better(const CaptureMode& a, const CaptureMode& b) {
  if (priority(a) != priority(b)) return priority(a) < priority(b);
  const auto size_a = std::tuple{static_cast<std::uint64_t>(a.width) * a.height,
                                 a.width, a.height};
  const auto size_b = std::tuple{static_cast<std::uint64_t>(b.width) * b.height,
                                 b.width, b.height};
  if (size_a != size_b) return size_a > size_b;
  return static_cast<std::int64_t>(a.frame_rate.numerator) *
             b.frame_rate.denominator >
         static_cast<std::int64_t>(b.frame_rate.numerator) *
             a.frame_rate.denominator;
}
bool known_adapter(const SerialPortInfo& port) {
  if (port.usb_vendor_id != 0x1a86 || !port.usb_product_id) return false;
  // CH340/CH341 UART and CH343 UART. WCH VID alone is not enough.
  const auto pid = *port.usb_product_id;
  return pid == 0x7523 || pid == 0x5523 || pid == 0x55d3;
}
}  // namespace
std::string select_device(std::span<const DeviceInfo> devices,
                          const std::optional<std::string>& requested) {
  if (requested) {
    for (const auto& device : devices)
      if (device.stable_id == *requested) return *requested;
    throw std::runtime_error("Capture device not found: " + *requested);
  }
  if (devices.size() == 1) return devices.front().stable_id;
  std::string error =
      devices.empty()
          ? "No capture devices found; specify --device after connecting a "
            "device."
          : "Multiple capture devices; specify --device. Candidates:";
  for (const auto& device : devices)
    error += "\n  " + device.stable_id + " (" + device.display_name + ")";
  throw std::runtime_error(error);
}
std::size_t select_mode(std::span<const CaptureMode> modes,
                        std::optional<std::size_t> requested,
                        VideoCodec codec) {
  if (requested) {
    if (*requested >= modes.size())
      throw std::runtime_error("Capture mode index out of range");
    if (!usable(modes[*requested], codec))
      throw std::runtime_error(
          codec != VideoCodec::mjpeg
              ? "H.264/H.265 requires supported native and delivered raw video "
                "with even dimensions; MJPEG transcoding is not supported"
              : "LAN relay requires usable native and delivered MJPEG; raw "
                "modes are unsupported");
    return *requested;
  }
  std::optional<std::size_t> best;
  for (std::size_t i = 0; i < modes.size(); ++i)
    if (usable(modes[i], codec) && (!best || better(modes[i], modes[*best])))
      best = i;
  if (!best)
    throw std::runtime_error(codec != VideoCodec::mjpeg
                                 ? "No supported native and delivered raw "
                                   "modes for H.264/H.265 found"
                                 : "No usable native and delivered MJPEG modes "
                                   "found; raw modes are unsupported");
  return *best;
}
std::string select_serial(std::span<const SerialPortInfo> ports,
                          const std::optional<std::string>& requested) {
  // Explicit names may not be enumerated by a driver; opening reports their
  // errors.
  if (requested) {
    if (requested->empty()) throw std::runtime_error("Empty --serial port");
    return *requested;
  }
  std::vector<const SerialPortInfo*> candidates;
  for (const auto& port : ports)
    if (known_adapter(port)) candidates.push_back(&port);
  if (candidates.size() == 1) return candidates.front()->name;
  std::string error = candidates.empty()
                          ? "No known CH340/CH341/CH343 USB serial adapter "
                            "found; specify --serial."
                          : "Multiple CH340/CH341/CH343 USB serial adapters; "
                            "specify --serial. Candidates:";
  for (const auto* port : candidates)
    error += "\n  " + port->name + " (" + port->description + ")";
  throw std::runtime_error(error);
}
}  // namespace kvmux::relay
