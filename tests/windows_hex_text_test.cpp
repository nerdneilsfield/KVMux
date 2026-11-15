#include "input/windows_hex_text.hpp"

#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <stdexcept>

using kvmux::KeyEdge;
using kvmux::map_windows_hex_scalar;
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
  return EXIT_SUCCESS;
}
