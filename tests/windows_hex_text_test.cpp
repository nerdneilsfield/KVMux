#include "input/windows_hex_text.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

#include "control/control_sink.hpp"
#include "input/us_ascii_text.hpp"

using kvmux::KeyEdge;
using kvmux::map_us_ascii_text;
using kvmux::map_windows_hex_paste;
using kvmux::map_windows_hex_scalar;
using kvmux::TracedKeyReport;
using kvmux::windows_hex_reports;
using kvmux::WindowsHexError;
using kvmux::WindowsHexPasteCounts;

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

  const auto expect_edges = [](const std::vector<KeyEdge>& got,
                               const std::vector<KeyEdge>& edges) {
    check(got.size() == edges.size());
    for (std::size_t index = 0; index < edges.size(); ++index) {
      check(got[index].usage == edges[index].usage);
      check(got[index].pressed == edges[index].pressed);
    }
  };
  const std::vector<KeyEdge> zhong_edges = {
      {0xe2, true},  {0x57, true}, {0x57, false}, {0x5c, true},
      {0x5c, false}, {0x08, true}, {0x08, false}, {0x5a, true},
      {0x5a, false}, {0x07, true}, {0x07, false}, {0xe2, false}};
  const std::vector<KeyEdge> de_edges = {
      {0xe2, true},  {0x57, true}, {0x57, false}, {0x5f, true},
      {0x5f, false}, {0x5e, true}, {0x5e, false}, {0x60, true},
      {0x60, false}, {0x5c, true}, {0x5c, false}, {0xe2, false}};
  const std::vector<KeyEdge> nobreak_edges = {
      {0xe2, true},  {0x57, true}, {0x57, false}, {0x04, true},
      {0x04, false}, {0x62, true}, {0x62, false}, {0xe2, false}};

  const auto ascii = map_windows_hex_paste("a\t\n");
  check(ascii && ascii.normalized_characters == 3 &&
        ascii.gestures.size() == 3);
  expect_edges(ascii.gestures[0].edges, {{0x04, true}, {0x04, false}});
  expect_edges(ascii.gestures[1].edges, {{0x2b, true}, {0x2b, false}});
  expect_edges(ascii.gestures[2].edges, {{0x28, true}, {0x28, false}});
  const auto shifted = map_windows_hex_paste("A");
  check(shifted && shifted.gestures.size() == 1);
  expect_edges(shifted.gestures[0].edges,
               {{0xe1, true}, {0x04, true}, {0x04, false}, {0xe1, false}});

  const auto han = map_windows_hex_paste("\xE4\xB8\xAD\xE7\x9A\x84");
  check(han && han.normalized_characters == 2 && han.gestures.size() == 2);
  expect_edges(han.gestures[0].edges, zhong_edges);
  expect_edges(han.gestures[1].edges, de_edges);
  check(map_us_ascii_text("\xE4\xB8\xAD\xE7\x9A\x84").error ==
        kvmux::TextPasteError::non_ascii);

  const auto mixed = map_windows_hex_paste("a\r\n\xE4\xB8\xAD");
  check(mixed && mixed.normalized_characters == 3 &&
        mixed.gestures.size() == 3);
  expect_edges(mixed.gestures[0].edges, {{0x04, true}, {0x04, false}});
  expect_edges(mixed.gestures[1].edges, {{0x28, true}, {0x28, false}});
  expect_edges(mixed.gestures[2].edges, zhong_edges);

  const auto nbsp = map_windows_hex_paste("\xC2\xA0");
  check(nbsp && nbsp.gestures.size() == 1);
  expect_edges(nbsp.gestures[0].edges, nobreak_edges);

  const auto above = map_windows_hex_paste("\xF0\x9F\x98\x80");
  check(!above && above.error == kvmux::TextPasteError::above_bmp &&
        above.rejected.above_bmp == 1 && above.gestures.empty());
  const auto surrogate_text = map_windows_hex_paste("\xED\xA0\x80");
  check(!surrogate_text &&
        surrogate_text.error == kvmux::TextPasteError::surrogate &&
        surrogate_text.rejected.surrogate == 1 &&
        surrogate_text.gestures.empty());
  const auto control = map_windows_hex_paste("ab\x01c");
  check(!control && control.error == kvmux::TextPasteError::control &&
        control.rejected.control == 1 && control.gestures.empty());
  const auto invalid = map_windows_hex_paste("\xff");
  check(!invalid && invalid.error == kvmux::TextPasteError::invalid_utf8 &&
        invalid.rejected.invalid_utf8 == 1 && invalid.gestures.empty());
  const auto bare = map_windows_hex_paste("\r\xE4\xB8\xAD");
  check(!bare && bare.error == kvmux::TextPasteError::bare_carriage_return &&
        bare.rejected.bare_carriage_return == 1 && bare.gestures.empty());
  const auto several = map_windows_hex_paste("\x01\xF0\x9F\x98\x80");
  check(!several && several.error == kvmux::TextPasteError::above_bmp &&
        several.rejected.control == 1 && several.rejected.above_bmp == 1 &&
        several.gestures.empty());
  check(map_windows_hex_paste("").error == kvmux::TextPasteError::empty);
  check(map_windows_hex_paste(std::string(65537, 'a')).error ==
        kvmux::TextPasteError::too_long);

  const auto estimate =
      kvmux::estimate_windows_hex_paste(2, std::chrono::microseconds(1000));
  check(estimate.frames == 24 && estimate.microseconds == 24000);
  const auto unknown =
      kvmux::estimate_windows_hex_paste(1, std::chrono::microseconds(0));
  check(unknown.frames == 12 && unknown.microseconds == 0);

  WindowsHexPasteCounts counts;
  check(kvmux::format_windows_hex_paste_log("refused", "num_lock", "none", 1, 0,
                                            0, counts) ==
        "paste phase=refused result=num_lock reason=none code_points=1 "
        "sent=0 elapsed_ms=0 rejected=0 invalid_utf8=0 surrogate=0 "
        "above_bmp=0 control=0 bare_cr=0");
  const auto logged = kvmux::format_windows_hex_paste_log(
      "finished", "canceled", "user", 2, 0, 0, counts);
  check(logged.find("4e2d") == std::string::npos);
  check(logged.find("7684") == std::string::npos);
  check(logged.find("\xE4\xB8\xAD") == std::string::npos);
  return EXIT_SUCCESS;
}
