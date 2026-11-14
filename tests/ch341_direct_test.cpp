#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "control/ch341_usb.hpp"

namespace {
void expect(bool ok, const std::string& message) {
  if (!ok) {
    throw std::runtime_error(message);
  }
}
}  // namespace

int main() {
  struct Sample {
    int baud;
    std::uint16_t version27;
    std::uint16_t version30;
  };
  // Hand-calculated from Linux ch341_get_divisor with no prescaler quirk.
  // version 0x30 sets bit 7; version 0x27 does not.
  constexpr Sample samples[] = {
      {1200, 0xB201, 0xB281},   {2400, 0xD901, 0xD981},
      {4800, 0x6402, 0x6482},   {9600, 0xB202, 0xB282},
      {14400, 0xCC02, 0xCC82},  {19200, 0xD902, 0xD982},
      {38400, 0x6403, 0x6483},  {57600, 0x9803, 0x9883},
      {115200, 0xCC03, 0xCC83},
  };
  for (const auto& sample : samples) {
    const auto older = kvmux::ch341_divisor_register(sample.baud, 0x27);
    const auto newer = kvmux::ch341_divisor_register(sample.baud, 0x30);
    if (!older.has_value() || !newer.has_value()) {
      throw std::runtime_error("missing divisor baud " +
                               std::to_string(sample.baud));
    }
    const std::uint16_t older_value = *older;
    const std::uint16_t newer_value = *newer;
    expect(older_value == sample.version27,
           "divisor version 0x27 baud " + std::to_string(sample.baud));
    expect(newer_value == sample.version30,
           "divisor version 0x30 baud " + std::to_string(sample.baud));
    expect((older_value & 0x0080u) == 0 && (newer_value & 0x0080u) != 0 &&
               (older_value | 0x0080u) == newer_value,
           "bit 7 differs only by version");
  }
  for (const int baud : {0, 300, 50, 9601, 230400, -9600, 1000000}) {
    expect(!kvmux::ch341_divisor_register(baud, 0x27) &&
               !kvmux::ch341_divisor_register(baud, 0x30),
           "rejected baud " + std::to_string(baud));
  }

  const auto address = kvmux::parse_ch341_usb_name("ch341-usb:1-2.3");
  if (!address.has_value()) {
    throw std::runtime_error("parse ch341-usb:1-2.3");
  }
  expect(address->bus == 1 && address->port_count == 2 &&
             address->ports[0] == 2 && address->ports[1] == 3,
         "path 1-2.3");
  expect(kvmux::format_ch341_usb_name(
             address->bus, std::span<const std::uint8_t>(
                               address->ports, address->port_count)) ==
             "ch341-usb:1-2.3",
         "round trip");
  const auto deep = kvmux::parse_ch341_usb_name("ch341-usb:255-1.2.3.4.5.6.7");
  if (!deep.has_value()) {
    throw std::runtime_error("seven ports");
  }
  expect(deep->bus == 255 && deep->port_count == 7 && deep->ports[6] == 7,
         "seven ports");
  expect(kvmux::parse_ch341_usb_name("ch341-usb:1-255").has_value(),
         "port 255");

  constexpr std::string_view illegal[] = {
      "",
      "ttyUSB0",
      "CH341-usb:1-2",
      "ch341-usb:",
      "ch341-usb:1",
      "ch341-usb:1-",
      "ch341-usb:1-2.",
      "ch341-usb:01-2",
      "ch341-usb:1-02",
      "ch341-usb:0-1",
      "ch341-usb:1-0",
      "ch341-usb:1-2.3 ",
      "ch341-usb:256-1",
      "ch341-usb:1-256",
      "ch341-usb:1-2..3",
      "ch341-usb:1-2.3x",
      "ch341-usb:1-2.3.4.5.6.7.8.9",
  };
  for (const auto name : illegal) {
    expect(!kvmux::parse_ch341_usb_name(name),
           std::string("accepted illegal name ") + std::string(name));
  }

  const auto ignored = kvmux::classify_wch_device({0x0403, 0x6001, 0, 0xff});
  expect(ignored.decision == kvmux::WchPortDecision::ignore,
         "other vendor is ignored");

  struct Listed {
    std::uint16_t product_id = 0;
    kvmux::WchLinkKind link = kvmux::WchLinkKind::none;
    std::string_view chip;
  };
  constexpr Listed listed[] = {
      {0x7523, kvmux::WchLinkKind::classic, "CH340"},
      {0x7522, kvmux::WchLinkKind::classic, "CH340K"},
      {0x5523, kvmux::WchLinkKind::classic, "CH341"},
      {0x55d3, kvmux::WchLinkKind::ch343, "CH343"},
      {0x55d4, kvmux::WchLinkKind::ch343, "CH9102"},
      {0x55d8, kvmux::WchLinkKind::ch343, "CH9101"},
      {0x55e9, kvmux::WchLinkKind::ch343, "CH9111"},
      {0x55ea, kvmux::WchLinkKind::ch343, "CH9111"},
  };
  for (const auto& row : listed) {
    const auto choice =
        kvmux::classify_wch_device({0x1a86, row.product_id, 0x0263, 0xff});
    expect(choice.decision == kvmux::WchPortDecision::list &&
               choice.link == row.link && choice.chip == row.chip &&
               choice.reason.find("请使用系统驱动") == std::string_view::npos,
           "listed pid " + std::to_string(row.product_id));
  }

  struct Skipped {
    std::uint16_t product_id = 0;
    std::string_view chip;
  };
  constexpr Skipped skipped[] = {
      {0x55d2, "CH342"}, {0x55d5, "CH344"},  {0x55d7, "CH9103"},
      {0x55db, "CH347"}, {0x5610, "CH9433"}, {0x7584, "CH340S"},
      {0x1111, "WCH"},
  };
  for (const auto& row : skipped) {
    const auto low =
        kvmux::classify_wch_device({0x1a86, row.product_id, 0x0100, -1});
    const auto high =
        kvmux::classify_wch_device({0x1a86, row.product_id, 0x0445, 0x02});
    expect(low.decision == kvmux::WchPortDecision::skip &&
               high.decision == kvmux::WchPortDecision::skip &&
               low.chip == row.chip && high.chip == row.chip &&
               low.link == kvmux::WchLinkKind::none &&
               low.reason.find("请使用系统驱动") != std::string_view::npos &&
               low.reason == high.reason,
           "skipped pid " + std::to_string(row.product_id));
  }

  struct Line {
    int baud = 0;
    std::uint16_t index = 0;
  };
  constexpr Line lines[] = {
      {1200, 0xB201},  {2400, 0xD901},  {4800, 0x6402},
      {9600, 0xB202},  {14400, 0xCC02}, {19200, 0xD902},
      {38400, 0x6403}, {57600, 0x9803}, {115200, 0xCC03},
  };
  expect(kvmux::ch343_line_value_8n1() == 0xC38C, "8N1 line value");
  expect(kvmux::ch343_modem_dtr_rts() == 0xFF9F, "DTR RTS modem value");
  for (const auto& line : lines) {
    const auto index = kvmux::ch343_line_index(line.baud);
    expect(index.has_value() && *index == line.index,
           "ch343 line baud " + std::to_string(line.baud));
  }
  for (const int baud : {0, 300, 50, 9601, 230400, -9600, 1000000}) {
    expect(!kvmux::ch343_line_index(baud),
           "ch343 rejected baud " + std::to_string(baud));
  }
  return 0;
}
