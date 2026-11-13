#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "control/control_event.hpp"

namespace kvmux {

// Why an in-progress paste stopped. `none` is not a cancellation.
// `unsupported` stays a character-mapping failure and does not use this.
enum class PasteCancelReason {
  none,
  user,
  key_press,
  focus_lost,
  video_stale,
  ui_stall,
  link_lost,
  ack_timeout,
  device_fault,
  shutdown
};

[[nodiscard]] inline const char* paste_cancel_reason_name(
    PasteCancelReason reason) noexcept {
  switch (reason) {
    case PasteCancelReason::none:
      return "none";
    case PasteCancelReason::user:
      return "user";
    case PasteCancelReason::key_press:
      return "key_press";
    case PasteCancelReason::focus_lost:
      return "focus_lost";
    case PasteCancelReason::video_stale:
      return "video_stale";
    case PasteCancelReason::ui_stall:
      return "ui_stall";
    case PasteCancelReason::link_lost:
      return "link_lost";
    case PasteCancelReason::ack_timeout:
      return "ack_timeout";
    case PasteCancelReason::device_fault:
      return "device_fault";
    case PasteCancelReason::shutdown:
      return "shutdown";
  }
  return "none";
}

enum class TextPasteError {
  none,
  empty,
  bare_carriage_return,
  non_ascii,
  unsupported,
  too_long,
  canceled
};

struct TextFilterResult {
  std::string text;
  std::size_t removed{};
  std::size_t normalized_characters{};
};

// Keeps accepted source bytes only and changes CRLF to LF. It never logs text.
[[nodiscard]] TextFilterResult filter_us_ascii_text(std::string_view text);

struct TextGesture {
  std::vector<KeyEdge> edges;
};
struct TextMappingResult {
  TextPasteError error{TextPasteError::none};
  // Metadata only; source text is never retained for diagnostics.
  std::size_t source_bytes{};
  std::size_t normalized_characters{};
  std::vector<TextGesture> gestures;
  PasteCancelReason cancel_reason{PasteCancelReason::none};
  [[nodiscard]] explicit operator bool() const noexcept {
    return error == TextPasteError::none;
  }
};

// Validates the entire byte string before returning any HID gestures. This
// first version accepts only US ASCII printable bytes, TAB, and LF; CRLF
// becomes LF.
[[nodiscard]] TextMappingResult map_us_ascii_text(
    std::string_view text, std::vector<std::uint8_t>* normalized = nullptr);

// True when every byte is TAB, LF, or a printable US-ASCII character. A bare
// CR is rejected. CRLF is not accepted here; callers normalize it first.
[[nodiscard]] bool accepted_us_ascii_bytes(
    std::span<const std::uint8_t> text) noexcept;

}  // namespace kvmux
