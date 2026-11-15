#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "control/control_event.hpp"
#include "input/us_ascii_text.hpp"

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

// Counts only. The paste text and its scalar values are not retained.
struct WindowsHexPasteCounts {
  std::size_t invalid_utf8{};
  std::size_t surrogate{};
  std::size_t above_bmp{};
  std::size_t control{};
  std::size_t bare_carriage_return{};
  [[nodiscard]] std::size_t rejected() const noexcept {
    return invalid_utf8 + surrogate + above_bmp + control +
           bare_carriage_return;
  }
};

// One gesture per accepted scalar. ASCII, Tab, and LF use the US-ASCII
// mapper. Other BMP scalars are one Windows hex sequence. Any rejection
// drops the whole paste.
struct WindowsHexPasteResult {
  TextPasteError error{TextPasteError::none};
  std::size_t source_bytes{};
  std::size_t normalized_characters{};
  WindowsHexPasteCounts rejected{};
  std::vector<TextGesture> gestures;
  [[nodiscard]] explicit operator bool() const noexcept {
    return error == TextPasteError::none;
  }
};

[[nodiscard]] WindowsHexPasteResult map_windows_hex_paste(
    std::string_view text);

// About 12 frames per character times the latest ACK time. A zero duration
// means the ACK time is unknown. This is an estimate, not a measurement.
struct WindowsHexPasteEstimate {
  std::size_t frames{};
  std::int64_t microseconds{};
};

[[nodiscard]] inline WindowsHexPasteEstimate estimate_windows_hex_paste(
    const std::size_t characters,
    const std::chrono::microseconds ack) noexcept {
  WindowsHexPasteEstimate estimate;
  if (characters > static_cast<std::size_t>(-1) / 12U) return estimate;
  estimate.frames = characters * 12U;
  if (ack.count() <= 0 || estimate.frames == 0) return estimate;
  const auto frames = static_cast<std::int64_t>(estimate.frames);
  if (ack.count() > (std::numeric_limits<std::int64_t>::max)() / frames)
    return estimate;
  estimate.microseconds = ack.count() * frames;
  return estimate;
}

// The formatted line has counts, phase, and reason only.
[[nodiscard]] std::string format_windows_hex_paste_log(
    const char* phase, const char* result, const char* reason,
    std::size_t code_points, std::size_t sent, std::int64_t elapsed_ms,
    const WindowsHexPasteCounts& rejected);

[[nodiscard]] const char* windows_hex_paste_error_name(
    TextPasteError error) noexcept;

void log_windows_hex_paste(const char* phase, const char* result,
                           const char* reason, std::size_t code_points,
                           std::size_t sent, std::int64_t elapsed_ms,
                           const WindowsHexPasteCounts& rejected) noexcept;

}  // namespace kvmux
