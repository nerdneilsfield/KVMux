#include "input/windows_hex_text.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>

#include "control/hid_keyboard.hpp"

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

namespace {

constexpr std::size_t kMaxScalars = 65536;

enum class UnitKind { scalar, invalid_utf8, surrogate, above_bmp };

struct Unit {
  UnitKind kind{UnitKind::invalid_utf8};
  char32_t scalar{};
  std::size_t size{1};
};

Unit decode_utf8(const std::string_view text, const std::size_t index) {
  const auto lead = static_cast<unsigned char>(text[index]);
  if (lead <= 0x7FU) return {UnitKind::scalar, lead, 1};
  if (lead < 0xC2U || lead > 0xF4U) return {UnitKind::invalid_utf8, 0, 1};
  const int need = lead < 0xE0U ? 2 : lead < 0xF0U ? 3 : 4;
  std::size_t taken = 1;
  for (int extra = 1; extra < need; ++extra) {
    if (index + static_cast<std::size_t>(extra) >= text.size()) break;
    const auto next = static_cast<unsigned char>(
        text[index + static_cast<std::size_t>(extra)]);
    if ((next & 0xC0U) != 0x80U) break;
    ++taken;
  }
  if (taken != static_cast<std::size_t>(need))
    return {UnitKind::invalid_utf8, 0, taken};
  const auto b1 = static_cast<unsigned char>(text[index + 1]);
  if (need == 2) {
    const char32_t scalar =
        (static_cast<char32_t>(lead & 0x1FU) << 6U) | (b1 & 0x3FU);
    return {UnitKind::scalar, scalar, 2};
  }
  const auto b2 = static_cast<unsigned char>(text[index + 2]);
  if (need == 3) {
    if (lead == 0xE0U && b1 < 0xA0U) return {UnitKind::invalid_utf8, 0, 3};
    if (lead == 0xEDU && b1 >= 0xA0U) return {UnitKind::surrogate, 0, 3};
    const char32_t scalar = (static_cast<char32_t>(lead & 0x0FU) << 12U) |
                            (static_cast<char32_t>(b1 & 0x3FU) << 6U) |
                            (b2 & 0x3FU);
    return {UnitKind::scalar, scalar, 3};
  }
  if ((lead == 0xF0U && b1 < 0x90U) || (lead == 0xF4U && b1 > 0x8FU))
    return {UnitKind::invalid_utf8, 0, 4};
  return {UnitKind::above_bmp, 0, 4};
}

bool is_paste_control(const char32_t scalar) noexcept {
  if (scalar == '\t' || scalar == '\n') return false;
  if (scalar < 0x20U || scalar == 0x7FU) return true;
  return scalar >= 0x80U && scalar <= 0x9FU;
}

bool is_ascii_paste_scalar(const char32_t scalar) noexcept {
  return scalar == '\t' || scalar == '\n' ||
         (scalar >= 0x20U && scalar <= 0x7EU);
}

TextPasteError primary_rejection(const WindowsHexPasteCounts& counts) {
  if (counts.invalid_utf8 != 0) return TextPasteError::invalid_utf8;
  if (counts.surrogate != 0) return TextPasteError::surrogate;
  if (counts.above_bmp != 0) return TextPasteError::above_bmp;
  if (counts.control != 0) return TextPasteError::control;
  if (counts.bare_carriage_return != 0)
    return TextPasteError::bare_carriage_return;
  return TextPasteError::none;
}

const char* text_or_none(const char* value) noexcept {
  return value != nullptr && value[0] != '\0' ? value : "none";
}

}  // namespace

std::vector<WindowsHexReport> windows_hex_reports(
    const std::span<const KeyEdge> edges) {
  HidKeyboardState keyboard;
  std::vector<WindowsHexReport> reports;
  reports.reserve(edges.size());
  for (const KeyEdge& edge : edges) {
    if (edge.pressed) {
      (void)keyboard.press(edge.usage);
    } else {
      keyboard.release(edge.usage);
    }
    WindowsHexReport report;
    report.modifiers = keyboard.modifiers();
    report.keys = keyboard.keys();
    reports.push_back(report);
  }
  return reports;
}

namespace {

constexpr int kMaxChord = 5;

bool modifier_usage(const std::uint8_t usage) noexcept {
  return usage >= 0xe0U && usage <= 0xe7U;
}

std::uint8_t modifier_bit(const std::uint8_t usage) noexcept {
  return static_cast<std::uint8_t>(1U << (usage - 0xe0U));
}

int filled_keys(const std::array<std::uint8_t, 6>& keys) noexcept {
  int count = 0;
  while (count < 6 && keys[static_cast<std::size_t>(count)] != 0) ++count;
  return count;
}

bool has_key(const std::array<std::uint8_t, 6>& keys,
             const std::uint8_t usage) noexcept {
  return std::find(keys.begin(), keys.end(), usage) != keys.end();
}

void append_key(std::array<std::uint8_t, 6>& keys,
                const std::uint8_t usage) noexcept {
  for (auto& slot : keys) {
    if (slot == 0) {
      slot = usage;
      return;
    }
  }
}

void remove_key(std::array<std::uint8_t, 6>& keys,
                const std::uint8_t usage) noexcept {
  std::array<std::uint8_t, 6> packed{};
  std::size_t count = 0;
  for (const auto slot : keys) {
    if (slot != 0 && slot != usage) packed[count++] = slot;
  }
  keys = packed;
}

void push_report(std::vector<PasteKeyboardReport>& out,
                 const std::uint8_t modifiers,
                 const std::array<std::uint8_t, 6>& keys,
                 const std::size_t completes = kPasteReportContinues) {
  PasteKeyboardReport report;
  report.modifiers = modifiers;
  report.keys = keys;
  report.completes_gesture = completes;
  out.push_back(report);
}

bool hex_gesture(const std::vector<KeyEdge>& edges) noexcept {
  return std::any_of(edges.begin(), edges.end(), [](const KeyEdge& edge) {
    return edge.usage == kLeftAlt;
  });
}

void apply_edge(const KeyEdge& edge, std::uint8_t& modifiers,
                std::array<std::uint8_t, 6>& keys) noexcept {
  if (modifier_usage(edge.usage)) {
    const auto bit = modifier_bit(edge.usage);
    if (edge.pressed) {
      modifiers = static_cast<std::uint8_t>(modifiers | bit);
    } else {
      modifiers = static_cast<std::uint8_t>(modifiers &
                                            static_cast<std::uint8_t>(~bit));
    }
    return;
  }
  if (edge.pressed) {
    if (!has_key(keys, edge.usage)) append_key(keys, edge.usage);
  } else {
    remove_key(keys, edge.usage);
  }
}

// Ordinary releases stay down until Alt goes up, except a repeated usage,
// which gets its own release report before it is pressed again. A sixth
// distinct key releases the chord first. Those keys share one report, so
// their release order is not defined. Alt is the next report.
void fold_hex_gesture(const std::vector<KeyEdge>& edges,
                      const std::size_t gesture,
                      std::vector<PasteKeyboardReport>& out) {
  std::uint8_t modifiers = 0;
  std::array<std::uint8_t, 6> keys{};
  bool completed = false;
  for (const KeyEdge& edge : edges) {
    if (modifier_usage(edge.usage)) {
      const auto bit = modifier_bit(edge.usage);
      if (edge.pressed) {
        modifiers = static_cast<std::uint8_t>(modifiers | bit);
        push_report(out, modifiers, keys);
      } else {
        if (filled_keys(keys) > 0) {
          keys.fill(0);
          push_report(out, modifiers, keys);
        }
        modifiers = static_cast<std::uint8_t>(modifiers &
                                              static_cast<std::uint8_t>(~bit));
        push_report(out, modifiers, keys, gesture);
        completed = true;
      }
      continue;
    }
    if (!edge.pressed) continue;
    if (has_key(keys, edge.usage)) {
      remove_key(keys, edge.usage);
      push_report(out, modifiers, keys);
    } else if (filled_keys(keys) >= kMaxChord) {
      keys.fill(0);
      push_report(out, modifiers, keys);
    }
    append_key(keys, edge.usage);
    push_report(out, modifiers, keys);
  }
  if (!completed && !out.empty()) out.back().completes_gesture = gesture;
}

struct MarkedEdge {
  KeyEdge edge;
  std::size_t gesture{};
  bool last{};
};

// A key-up followed by a different key-down is one report. A repeated key,
// or a modifier edge on either side, stays separate so the edge is visible.
void fold_ascii_gestures(const std::span<const std::vector<KeyEdge>> gestures,
                         const std::size_t origin,
                         std::vector<PasteKeyboardReport>& out) {
  std::vector<MarkedEdge> marked;
  for (std::size_t index = 0; index < gestures.size(); ++index) {
    const auto& edges = gestures[index];
    for (std::size_t edge = 0; edge < edges.size(); ++edge) {
      marked.push_back(
          MarkedEdge{edges[edge], origin + index, edge + 1 == edges.size()});
    }
  }
  std::uint8_t modifiers = 0;
  std::array<std::uint8_t, 6> keys{};
  for (std::size_t index = 0; index < marked.size(); ++index) {
    const auto& current = marked[index];
    apply_edge(current.edge, modifiers, keys);
    auto completes = current.last ? current.gesture : kPasteReportContinues;
    auto also = kPasteReportContinues;
    if (!current.edge.pressed && !modifier_usage(current.edge.usage) &&
        index + 1 < marked.size()) {
      const auto& next = marked[index + 1];
      if (next.edge.pressed && !modifier_usage(next.edge.usage) &&
          next.edge.usage != current.edge.usage) {
        apply_edge(next.edge, modifiers, keys);
        if (next.last) {
          if (completes == kPasteReportContinues) {
            completes = next.gesture;
          } else {
            also = next.gesture;
          }
        }
        ++index;
      }
    }
    push_report(out, modifiers, keys, completes);
    out.back().also_completes_gesture = also;
  }
}

}  // namespace

std::vector<PasteKeyboardReport> fold_paste_reports(
    const std::span<const std::vector<KeyEdge>> gestures) {
  std::vector<PasteKeyboardReport> reports;
  std::size_t index = 0;
  while (index < gestures.size()) {
    if (hex_gesture(gestures[index])) {
      fold_hex_gesture(gestures[index], index, reports);
      ++index;
      continue;
    }
    std::size_t end = index + 1;
    while (end < gestures.size() && !hex_gesture(gestures[end])) ++end;
    fold_ascii_gestures(gestures.subspan(index, end - index), index, reports);
    index = end;
  }
  return reports;
}

WindowsHexPasteResult map_windows_hex_paste(const std::string_view text) {
  WindowsHexPasteResult result;
  result.source_bytes = text.size();
  std::vector<char32_t> scalars;
  bool too_long = false;
  std::size_t index = 0;
  while (index < text.size()) {
    const Unit unit = decode_utf8(text, index);
    index += unit.size;
    if (unit.kind == UnitKind::invalid_utf8) {
      ++result.rejected.invalid_utf8;
      scalars.clear();
      continue;
    }
    if (unit.kind == UnitKind::surrogate) {
      ++result.rejected.surrogate;
      scalars.clear();
      continue;
    }
    if (unit.kind == UnitKind::above_bmp) {
      ++result.rejected.above_bmp;
      scalars.clear();
      continue;
    }
    if (unit.scalar == '\r') {
      if (index < text.size()) {
        const Unit next = decode_utf8(text, index);
        if (next.kind == UnitKind::scalar && next.scalar == '\n') {
          index += next.size;
          if (result.rejected.rejected() == 0 && !too_long) {
            if (scalars.size() >= kMaxScalars) {
              too_long = true;
              scalars.clear();
            } else {
              scalars.push_back('\n');
            }
          }
          continue;
        }
      }
      ++result.rejected.bare_carriage_return;
      scalars.clear();
      continue;
    }
    if (is_paste_control(unit.scalar)) {
      ++result.rejected.control;
      scalars.clear();
      continue;
    }
    if (result.rejected.rejected() != 0 || too_long) continue;
    if (scalars.size() >= kMaxScalars) {
      too_long = true;
      scalars.clear();
      continue;
    }
    scalars.push_back(unit.scalar);
  }

  if (const auto reason = primary_rejection(result.rejected);
      reason != TextPasteError::none) {
    result.error = reason;
    return result;
  }
  if (too_long) {
    result.error = TextPasteError::too_long;
    return result;
  }
  if (scalars.empty()) {
    result.error = TextPasteError::empty;
    return result;
  }

  std::string ascii_run;
  const auto flush_ascii = [&] {
    if (ascii_run.empty()) return true;
    const auto mapped = map_us_ascii_text(ascii_run);
    ascii_run.clear();
    if (!mapped) return false;
    result.gestures.insert(result.gestures.end(), mapped.gestures.begin(),
                           mapped.gestures.end());
    return true;
  };
  for (const char32_t scalar : scalars) {
    if (is_ascii_paste_scalar(scalar)) {
      ascii_run.push_back(static_cast<char>(scalar));
      continue;
    }
    if (!flush_ascii()) {
      result.gestures.clear();
      result.error = TextPasteError::unsupported;
      return result;
    }
    auto sequence = map_windows_hex_scalar(static_cast<std::uint32_t>(scalar));
    if (!sequence || sequence.edges.empty()) {
      result.gestures.clear();
      result.error = TextPasteError::unsupported;
      return result;
    }
    TextGesture gesture;
    gesture.edges = std::move(sequence.edges);
    result.gestures.push_back(std::move(gesture));
  }
  if (!flush_ascii()) {
    result.gestures.clear();
    result.error = TextPasteError::unsupported;
    return result;
  }
  result.normalized_characters = scalars.size();
  return result;
}

const char* windows_hex_paste_error_name(const TextPasteError error) noexcept {
  switch (error) {
    case TextPasteError::none:
      return "none";
    case TextPasteError::empty:
      return "empty";
    case TextPasteError::bare_carriage_return:
      return "bare_carriage_return";
    case TextPasteError::non_ascii:
      return "non_ascii";
    case TextPasteError::unsupported:
      return "unsupported";
    case TextPasteError::invalid_utf8:
      return "invalid_utf8";
    case TextPasteError::surrogate:
      return "surrogate";
    case TextPasteError::above_bmp:
      return "above_bmp";
    case TextPasteError::control:
      return "control";
    case TextPasteError::num_lock:
      return "num_lock";
    case TextPasteError::too_long:
      return "too_long";
    case TextPasteError::canceled:
      return "canceled";
  }
  return "unknown";
}

std::string format_windows_hex_paste_log(
    const char* const phase, const char* const result, const char* const reason,
    const std::size_t code_points, const std::size_t sent,
    const std::int64_t elapsed_ms, const WindowsHexPasteCounts& rejected) {
  return std::string("paste phase=") + text_or_none(phase) +
         " result=" + text_or_none(result) + " reason=" + text_or_none(reason) +
         " code_points=" + std::to_string(code_points) +
         " sent=" + std::to_string(sent) +
         " elapsed_ms=" + std::to_string(elapsed_ms) +
         " rejected=" + std::to_string(rejected.rejected()) +
         " invalid_utf8=" + std::to_string(rejected.invalid_utf8) +
         " surrogate=" + std::to_string(rejected.surrogate) +
         " above_bmp=" + std::to_string(rejected.above_bmp) +
         " control=" + std::to_string(rejected.control) +
         " bare_cr=" + std::to_string(rejected.bare_carriage_return);
}

void log_windows_hex_paste(const char* const phase, const char* const result,
                           const char* const reason,
                           const std::size_t code_points,
                           const std::size_t sent,
                           const std::int64_t elapsed_ms,
                           const WindowsHexPasteCounts& rejected) noexcept {
  try {
    spdlog::info(
        "{}", format_windows_hex_paste_log(phase, result, reason, code_points,
                                           sent, elapsed_ms, rejected));
  } catch (const std::exception&) {
  }
}

}  // namespace kvmux
