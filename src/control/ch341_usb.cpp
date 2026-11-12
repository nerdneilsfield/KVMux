#include "control/ch341_usb.hpp"

#include <string>

namespace kvmux {
namespace {

constexpr std::uint32_t kClockHz = 48000000;

constexpr std::uint32_t clock_divider(int prescaler, int factor) {
  const auto shift = static_cast<unsigned>(12 - 3 * prescaler - factor);
  return 1u << shift;
}

bool ch9329_baud(int baud) {
  switch (baud) {
    case 1200:
    case 2400:
    case 4800:
    case 9600:
    case 14400:
    case 19200:
    case 38400:
    case 57600:
    case 115200:
      return true;
    default:
      return false;
  }
}

// Decimal component in 1..255 with no leading zero and no extra characters.
std::optional<int> parse_component(std::string_view text) {
  if (text.empty() || text.size() > 3) {
    return std::nullopt;
  }
  if (text.size() > 1 && text.front() == '0') {
    return std::nullopt;
  }
  int value = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return std::nullopt;
    }
    value = value * 10 + (character - '0');
  }
  if (value < 1 || value > 255) {
    return std::nullopt;
  }
  return value;
}

}  // namespace

std::optional<std::uint16_t> ch341_divisor_register(int baud,
                                                    std::uint8_t version) {
  // Linux drivers/usb/serial/ch341.c ch341_get_divisor, without the
  // limited-prescaler quirk. Bit 7 follows ch341_set_baudrate_lcr.
  if (!ch9329_baud(baud)) {
    return std::nullopt;
  }
  const std::uint32_t min_rates[4] = {
      kClockHz / (clock_divider(0, 1) * 512),
      kClockHz / (clock_divider(1, 1) * 512),
      kClockHz / (clock_divider(2, 1) * 512),
      kClockHz / (clock_divider(3, 1) * 512),
  };
  int factor = 1;
  int prescaler = -1;
  for (int candidate = 3; candidate >= 0; --candidate) {
    if (static_cast<std::uint32_t>(baud) > min_rates[candidate]) {
      prescaler = candidate;
      break;
    }
  }
  if (prescaler < 0) {
    return std::nullopt;
  }
  std::uint32_t divider = clock_divider(prescaler, factor);
  std::uint32_t divisor =
      kClockHz / (divider * static_cast<std::uint32_t>(baud));
  if (divisor < 9 || divisor > 255) {
    divisor /= 2;
    divider *= 2;
    factor = 0;
  }
  if (divisor < 2) {
    return std::nullopt;
  }
  const auto scale = static_cast<std::int64_t>(16);
  const auto left =
      scale * kClockHz / (static_cast<std::int64_t>(divider) * divisor) -
      scale * baud;
  const auto right =
      scale * baud -
      scale * kClockHz / (static_cast<std::int64_t>(divider) * (divisor + 1));
  if (left >= right) {
    ++divisor;
  }
  if (factor == 1 && divisor % 2 == 0) {
    divisor /= 2;
    factor = 0;
  }
  auto value = static_cast<std::uint16_t>(
      ((0x100u - divisor) << 8u) | (static_cast<unsigned>(factor) << 2u) |
      static_cast<unsigned>(prescaler));
  if (version > 0x27) {
    value = static_cast<std::uint16_t>(value | 0x80u);
  }
  return value;
}

std::optional<Ch341UsbAddress> parse_ch341_usb_name(std::string_view name) {
  if (!name.starts_with(kCh341UsbPrefix)) {
    return std::nullopt;
  }
  name.remove_prefix(kCh341UsbPrefix.size());
  const auto dash = name.find('-');
  if (dash == std::string_view::npos) {
    return std::nullopt;
  }
  const auto bus = parse_component(name.substr(0, dash));
  if (!bus) {
    return std::nullopt;
  }
  name.remove_prefix(dash + 1);
  if (name.empty()) {
    return std::nullopt;
  }
  Ch341UsbAddress address;
  address.bus = static_cast<std::uint8_t>(*bus);
  while (!name.empty()) {
    if (address.port_count == 7) {
      return std::nullopt;
    }
    const auto dot = name.find('.');
    const auto token =
        dot == std::string_view::npos ? name : name.substr(0, dot);
    const auto port = parse_component(token);
    if (!port) {
      return std::nullopt;
    }
    address.ports[address.port_count++] = static_cast<std::uint8_t>(*port);
    if (dot == std::string_view::npos) {
      return address;
    }
    name.remove_prefix(dot + 1);
    if (name.empty()) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

std::string format_ch341_usb_name(std::uint8_t bus,
                                  std::span<const std::uint8_t> ports) {
  if (bus == 0 || ports.empty() || ports.size() > 7) {
    return {};
  }
  std::string name(kCh341UsbPrefix);
  name += std::to_string(bus);
  for (std::size_t index = 0; index < ports.size(); ++index) {
    if (ports[index] == 0) {
      return {};
    }
    name += index == 0 ? '-' : '.';
    name += std::to_string(ports[index]);
  }
  return name;
}

}  // namespace kvmux
