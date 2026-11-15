#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "control/control_event.hpp"

namespace kvmux {

enum class WindowsHexError { none, surrogate, out_of_range };

struct WindowsHexSequence {
  WindowsHexError error{WindowsHexError::none};
  std::vector<KeyEdge> edges;
  [[nodiscard]] explicit operator bool() const noexcept {
    return error == WindowsHexError::none;
  }
};

// `surrogate` is U+D800..U+DFFF. `out_of_range` is above U+10FFFF.
[[nodiscard]] WindowsHexError windows_hex_scalar_error(
    std::uint32_t scalar) noexcept;

// Left Alt down, keypad '+', minimal hex digits, Left Alt up.
// Digits 0-9 use the keypad. A-F use letter keys. No leading zeros.
// The scalar is not logged.
[[nodiscard]] WindowsHexSequence map_windows_hex_scalar(std::uint32_t scalar);

// One HID report per edge, starting from a clear keyboard.
struct WindowsHexReport {
  std::uint8_t modifiers{};
  std::array<std::uint8_t, 6> keys{};
};

[[nodiscard]] std::vector<WindowsHexReport> windows_hex_reports(
    std::span<const KeyEdge> edges);

}  // namespace kvmux
