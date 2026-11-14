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

WchPortChoice classify_wch_device(WchObservedDevice device) {
  if (device.vendor_id != kCh341DirectVendorId) {
    return {};
  }
  struct Row {
    std::uint16_t product_id = 0;
    WchLinkKind link = WchLinkKind::none;
    WchPortDecision decision = WchPortDecision::ignore;
    std::string_view chip;
    std::string_view reason;
  };
  // Product IDs are the public USB IDs. Multi-port and interface-qualified
  // IDs stay in the table so a log can name them, and they are not opened.
  static constexpr Row kRows[] = {
      {0x7523, WchLinkKind::classic, WchPortDecision::list, "CH340",
       "CH340 与 CH341 使用同一套厂商控制请求"},
      {0x7522, WchLinkKind::classic, WchPortDecision::list, "CH340K",
       "CH340K 与 CH341 使用同一套厂商控制请求"},
      {0x5523, WchLinkKind::classic, WchPortDecision::list, "CH341",
       "CH341 串口模式"},
      {0x55d3, WchLinkKind::ch343, WchPortDecision::list, "CH343",
       "CH343 单串口，依据 WCH ch343 驱动确认的协议事实"},
      {0x55d4, WchLinkKind::ch343, WchPortDecision::list, "CH9102",
       "CH9102 单串口，依据 WCH ch343 驱动确认的协议事实"},
      {0x55d8, WchLinkKind::ch343, WchPortDecision::list, "CH9101",
       "CH9101 单串口，依据 WCH ch343 驱动确认的协议事实"},
      {0x55e9, WchLinkKind::ch343, WchPortDecision::list, "CH9111",
       "CH9111 单串口，115200 及以下走通用分频"},
      {0x55ea, WchLinkKind::ch343, WchPortDecision::list, "CH9111",
       "CH9111 单串口模式 1，115200 及以下走通用分频"},
      {0x55d2, WchLinkKind::none, WchPortDecision::skip, "CH342",
       "CH342 多串口，未支持，请使用系统驱动"},
      {0x55d5, WchLinkKind::none, WchPortDecision::skip, "CH344",
       "CH344 多串口，未支持，请使用系统驱动"},
      {0x55d6, WchLinkKind::none, WchPortDecision::skip, "CH9143",
       "CH9143 未实现直连，请使用系统驱动"},
      {0x55d7, WchLinkKind::none, WchPortDecision::skip, "CH9103",
       "CH9103 多串口，未支持，请使用系统驱动"},
      {0x55da, WchLinkKind::none, WchPortDecision::skip, "CH347",
       "CH347 多串口，未支持，请使用系统驱动"},
      {0x55db, WchLinkKind::none, WchPortDecision::skip, "CH347",
       "CH347 需要指定接口号，未支持，请使用系统驱动"},
      {0x55dd, WchLinkKind::none, WchPortDecision::skip, "CH347",
       "CH347 需要指定接口号，未支持，请使用系统驱动"},
      {0x55de, WchLinkKind::none, WchPortDecision::skip, "CH347",
       "CH347 需要指定接口号，未支持，请使用系统驱动"},
      {0x55df, WchLinkKind::none, WchPortDecision::skip, "CH9104",
       "CH9104 多串口，未支持，请使用系统驱动"},
      {0x55e7, WchLinkKind::none, WchPortDecision::skip, "CH339",
       "CH339 需要指定接口号，未支持，请使用系统驱动"},
      {0x55e8, WchLinkKind::none, WchPortDecision::skip, "CH9114",
       "CH9114 多串口，未支持，请使用系统驱动"},
      {0x55eb, WchLinkKind::none, WchPortDecision::skip, "CH346",
       "CH346 需要指定接口号，未支持，请使用系统驱动"},
      {0x55ec, WchLinkKind::none, WchPortDecision::skip, "CH346",
       "CH346 多串口，未支持，请使用系统驱动"},
      {0x55ef, WchLinkKind::none, WchPortDecision::skip, "CH9105",
       "CH9105 多串口，未支持，请使用系统驱动"},
      {0x5610, WchLinkKind::none, WchPortDecision::skip, "CH9433",
       "CH9433 需要指定接口号，未支持，请使用系统驱动"},
      {0x5512, WchLinkKind::none, WchPortDecision::skip, "CH341",
       "CH341 EPP 模式，不支持，请使用系统驱动"},
      {0x5584, WchLinkKind::none, WchPortDecision::skip, "CH341",
       "CH341 并口模式，不支持，请使用系统驱动"},
      {0x752d, WchLinkKind::none, WchPortDecision::skip, "CH345",
       "CH345 MIDI，不支持，请使用系统驱动"},
      {0x7584, WchLinkKind::none, WchPortDecision::skip, "CH340S",
       "CH340S 协议未确认，不支持，请使用系统驱动"},
      {0xe008, WchLinkKind::none, WchPortDecision::skip, "HID serial",
       "HID 串口，不支持，请使用系统驱动"},
      {0xe523, WchLinkKind::none, WchPortDecision::skip, "CH330",
       "CH330 协议未确认，不支持，请使用系统驱动"},
  };
  for (const Row& row : kRows) {
    if (row.product_id == device.product_id) {
      return {row.decision, row.link, row.chip, row.reason};
    }
  }
  return {WchPortDecision::skip, WchLinkKind::none, "WCH",
          "PID 未确认，不支持，请使用系统驱动"};
}

std::optional<std::uint16_t> ch343_line_index(int baud) {
  // Below 2 Mbaud the third-generation UART picks one of four base clocks
  // and an 8-bit divider. The nine CH9329 rates all fall in that range.
  if (!ch9329_baud(baud)) {
    return std::nullopt;
  }
  const auto rate = static_cast<std::uint32_t>(baud);
  std::uint32_t clock = 11719;
  unsigned code = 0;
  if (rate > 6000000u / 255u) {
    clock = 6000000;
    code = 3;
  } else if (rate > 750000u / 255u) {
    clock = 750000;
    code = 2;
  } else if (rate > 93750u / 255u) {
    clock = 93750;
    code = 1;
  }
  std::uint32_t divider = clock / rate;
  if (divider == 0 || divider == 0xffu) {
    return std::nullopt;
  }
  const auto high = static_cast<std::int64_t>(clock / divider) -
                    static_cast<std::int64_t>(rate);
  const auto low = static_cast<std::int64_t>(rate) -
                   static_cast<std::int64_t>(clock / (divider + 1));
  if (high > low) {
    ++divider;
  }
  if (divider == 0 || divider > 0xffu) {
    return std::nullopt;
  }
  const unsigned factor = 256u - divider;
  return static_cast<std::uint16_t>((factor << 8u) | code);
}

}  // namespace kvmux
