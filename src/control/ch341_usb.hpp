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

// classic: CH340/CH340K/CH341 vendor-register sequence already in this tree.
// ch343: third-generation single UART. The request numbers were checked
// against WCH's ch343 driver and rewritten here; that driver is not vendored.
enum class WchLinkKind { none, classic, ch343 };

enum class WchPortDecision { ignore, list, skip };

// One device seen on the bus. interface_class is -1 when the first interface
// descriptor could not be read. Enumeration logs every 0x1a86 device.
struct WchObservedDevice {
  std::uint16_t vendor_id = 0;
  std::uint16_t product_id = 0;
  std::uint16_t bcd_device = 0;
  int interface_class = -1;
};

struct WchPortChoice {
  WchPortDecision decision = WchPortDecision::ignore;
  WchLinkKind link = WchLinkKind::none;
  std::string_view chip;
  std::string_view reason;
};

[[nodiscard]] WchPortChoice classify_wch_device(WchObservedDevice device);

// wIndex of vendor OUT 0xA1 for 8N1 on interface 0. Empty outside the
// CH9329 baud whitelist. High-rate encodings above 115200 are not produced.
[[nodiscard]] std::optional<std::uint16_t> ch343_line_index(int baud);

// wValue of that 0xA1 transfer: 8 data bits, TX and RX enabled.
[[nodiscard]] constexpr std::uint16_t ch343_line_value_8n1() { return 0xC38C; }

// wValue of vendor OUT 0xA4: 16-bit inverse of the DTR and RTS bits.
[[nodiscard]] constexpr std::uint16_t ch343_modem_dtr_rts() { return 0xFF9F; }

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
