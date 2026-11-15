#include "input/windows_hex_text.hpp"

#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <stdexcept>

#include "control/control_sink.hpp"

using kvmux::KeyEdge;
using kvmux::map_windows_hex_scalar;
using kvmux::TracedKeyReport;
using kvmux::windows_hex_reports;
using kvmux::WindowsHexError;

void check(bool ok) {
  if (!ok) throw std::runtime_error("failed");
}

void expect(std::uint32_t scalar, std::initializer_list<KeyEdge> edges) {
  const auto got = map_windows_hex_scalar(scalar);
  check(static_cast<bool>(got));
  check(got.edges.size() == edges.size());
  std::size_t index = 0;
  for (const KeyEdge& edge : edges) {
    check(got.edges[index].usage == edge.usage);
    check(got.edges[index].pressed == edge.pressed);
    ++index;
  }
}

struct ReportExpect {
  std::uint8_t modifiers;
  std::uint8_t key;
};

void expect_reports(std::uint32_t scalar,
                    std::initializer_list<ReportExpect> rows) {
  const auto mapped = map_windows_hex_scalar(scalar);
  check(static_cast<bool>(mapped));
  const auto reports = windows_hex_reports(mapped.edges);
  check(reports.size() == rows.size());
  std::size_t index = 0;
  for (const ReportExpect& row : rows) {
    check(reports[index].modifiers == row.modifiers);
    check(reports[index].keys[0] == row.key);
    for (std::size_t slot = 1; slot < reports[index].keys.size(); ++slot)
      check(reports[index].keys[slot] == 0);
    check((reports[index].modifiers & static_cast<std::uint8_t>(~0x04U)) == 0);
    check(row.key != 0x2e && row.key != 0x0e);
    ++index;
  }
}

int main() {
  // U+4E2D, digits 4 e 2 d. Keypad 4, letter E, keypad 2, letter D.
  expect(0x4e2d, {{0xe2, true},
                  {0x57, true},
                  {0x57, false},
                  {0x5c, true},
                  {0x5c, false},
                  {0x08, true},
                  {0x08, false},
                  {0x5a, true},
                  {0x5a, false},
                  {0x07, true},
                  {0x07, false},
                  {0xe2, false}});
  // U+7684, digits 7 6 8 4. All four are keypad keys.
  expect(0x7684, {{0xe2, true},
                  {0x57, true},
                  {0x57, false},
                  {0x5f, true},
                  {0x5f, false},
                  {0x5e, true},
                  {0x5e, false},
                  {0x60, true},
                  {0x60, false},
                  {0x5c, true},
                  {0x5c, false},
                  {0xe2, false}});
  // U+1F600, digits 1 f 6 0 0. No surrogate split.
  expect(0x1f600, {{0xe2, true},
                   {0x57, true},
                   {0x57, false},
                   {0x59, true},
                   {0x59, false},
                   {0x09, true},
                   {0x09, false},
                   {0x5e, true},
                   {0x5e, false},
                   {0x62, true},
                   {0x62, false},
                   {0x62, true},
                   {0x62, false},
                   {0xe2, false}});
  // U+0041 is sent as 41, not 0041.
  expect(0x0041, {{0xe2, true},
                  {0x57, true},
                  {0x57, false},
                  {0x5c, true},
                  {0x5c, false},
                  {0x59, true},
                  {0x59, false},
                  {0xe2, false}});

  const auto surrogate = map_windows_hex_scalar(0xD800);
  check(surrogate.error == WindowsHexError::surrogate &&
        surrogate.edges.empty());
  check(map_windows_hex_scalar(0xDFFF).error == WindowsHexError::surrogate);
  const auto too_big = map_windows_hex_scalar(0x110000);
  check(too_big.error == WindowsHexError::out_of_range &&
        too_big.edges.empty());

  // Frame 0 is Left Alt alone. Frame 1 is Left Alt and keypad '+' (0x57).
  // The last frame releases Alt by itself. 0x2e would be the main '+' key.
  expect_reports(0x4e2d, {{0x04, 0x00},
                          {0x04, 0x57},
                          {0x04, 0x00},
                          {0x04, 0x5c},
                          {0x04, 0x00},
                          {0x04, 0x08},
                          {0x04, 0x00},
                          {0x04, 0x5a},
                          {0x04, 0x00},
                          {0x04, 0x07},
                          {0x04, 0x00},
                          {0x00, 0x00}});
  expect_reports(0x7684, {{0x04, 0x00},
                          {0x04, 0x57},
                          {0x04, 0x00},
                          {0x04, 0x5f},
                          {0x04, 0x00},
                          {0x04, 0x5e},
                          {0x04, 0x00},
                          {0x04, 0x60},
                          {0x04, 0x00},
                          {0x04, 0x5c},
                          {0x04, 0x00},
                          {0x00, 0x00}});
  const auto first = map_windows_hex_scalar(0x4e2d);
  const auto second = map_windows_hex_scalar(0x7684);
  std::vector<KeyEdge> both = first.edges;
  both.insert(both.end(), second.edges.begin(), second.edges.end());
  const auto joined = windows_hex_reports(both);
  check(joined.size() == 24);
  check(joined[11].modifiers == 0x00 && joined[11].keys[0] == 0);
  check(joined[12].modifiers == 0x04 && joined[12].keys[0] == 0);

  TracedKeyReport down;
  down.seq = 0;
  down.t_ms = 0;
  down.modifiers = 0x04;
  check(kvmux::format_traced_key_report(down) == "seq=0 t_ms=0 mods=04 keys=");
  TracedKeyReport plus;
  plus.seq = 1;
  plus.t_ms = 27;
  plus.modifiers = 0x04;
  plus.keys[0] = 0x57;
  check(kvmux::format_traced_key_report(plus) ==
        "seq=1 t_ms=27 mods=04 keys=57");
  return EXIT_SUCCESS;
}
