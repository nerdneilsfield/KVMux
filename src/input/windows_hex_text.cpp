#include "input/windows_hex_text.hpp"

namespace kvmux {
namespace {

constexpr std::uint8_t kLeftAlt = 0xe2;
constexpr std::uint8_t kKeypadPlus = 0x57;

std::uint8_t hex_digit_usage(unsigned digit) noexcept {
  // Keypad 1 is 0x59. Keypad 0 is 0x62, not the slot before 1.
  if (digit == 0) return 0x62;
  if (digit < 10) return static_cast<std::uint8_t>(0x58U + digit);
  return static_cast<std::uint8_t>(0x04U + (digit - 10U));
}

void push_tap(std::vector<KeyEdge>& edges, std::uint8_t usage) {
  edges.push_back(KeyEdge{usage, true});
  edges.push_back(KeyEdge{usage, false});
}

}  // namespace

WindowsHexError windows_hex_scalar_error(const std::uint32_t scalar) noexcept {
  if (scalar > 0x10FFFFU) return WindowsHexError::out_of_range;
  if (scalar >= 0xD800U && scalar <= 0xDFFFU) return WindowsHexError::surrogate;
  return WindowsHexError::none;
}

WindowsHexSequence map_windows_hex_scalar(const std::uint32_t scalar) {
  WindowsHexSequence result;
  result.error = windows_hex_scalar_error(scalar);
  if (result.error != WindowsHexError::none) return result;

  unsigned digits[6]{};
  int count = 0;
  auto value = scalar;
  if (value == 0) {
    digits[0] = 0;
    count = 1;
  } else {
    while (value != 0) {
      digits[count] = value & 0xFU;
      ++count;
      value >>= 4U;
    }
  }

  result.edges.reserve(static_cast<std::size_t>(count) * 2U + 4U);
  result.edges.push_back(KeyEdge{kLeftAlt, true});
  push_tap(result.edges, kKeypadPlus);
  for (int index = count - 1; index >= 0; --index)
    push_tap(result.edges, hex_digit_usage(digits[index]));
  result.edges.push_back(KeyEdge{kLeftAlt, false});
  return result;
}

}  // namespace kvmux
