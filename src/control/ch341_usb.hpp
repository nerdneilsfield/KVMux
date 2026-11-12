#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace kvmux {

inline constexpr std::uint16_t kCh341DirectVendorId = 0x1a86;
inline constexpr std::uint16_t kCh341DirectProductId = 0x5523;
inline constexpr std::string_view kCh341UsbPrefix = "ch341-usb:";

// Bus and port numbers from the root hub down to the device. Depth is at
// most 7, which is the USB topology limit libusb reports.
struct Ch341UsbAddress {
  std::uint8_t bus = 0;
  std::uint8_t ports[7]{};
  std::uint8_t port_count = 0;
};

// Register value written to CH341 divisor/prescaler (wIndex of request 0x9a,
// wValue 0x1312). Empty when `baud` is outside the CH9329 whitelist.
[[nodiscard]] std::optional<std::uint16_t> ch341_divisor_register(
    int baud, std::uint8_t version);

[[nodiscard]] std::optional<Ch341UsbAddress> parse_ch341_usb_name(
    std::string_view name);

// Empty when `bus` or `ports` cannot appear in a name enumeration emits.
[[nodiscard]] std::string format_ch341_usb_name(
    std::uint8_t bus, std::span<const std::uint8_t> ports);

}  // namespace kvmux
