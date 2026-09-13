#include "input/us_ascii_text.hpp"

#include <array>

namespace kvmux {
namespace {
constexpr std::uint8_t kLeftShift = 0xe1;
struct Entry { std::uint8_t usage{}; bool shift{}; };

Entry printable(const unsigned char c) {
    if (c >= 'a' && c <= 'z') return {static_cast<std::uint8_t>(0x04 + c - 'a'), false};
    if (c >= 'A' && c <= 'Z') return {static_cast<std::uint8_t>(0x04 + c - 'A'), true};
    if (c >= '1' && c <= '9') return {static_cast<std::uint8_t>(0x1e + c - '1'), false};
    if (c == '0') return {0x27, false};
    switch (c) {
    case ' ': return {0x2c, false}; case '-': return {0x2d, false}; case '_': return {0x2d, true};
    case '=': return {0x2e, false}; case '+': return {0x2e, true}; case '[': return {0x2f, false};
    case '{': return {0x2f, true}; case ']': return {0x30, false}; case '}': return {0x30, true};
    case '\\': return {0x31, false}; case '|': return {0x31, true}; case ';': return {0x33, false};
    case ':': return {0x33, true}; case '\'': return {0x34, false}; case '"': return {0x34, true};
    case '`': return {0x35, false}; case '~': return {0x35, true}; case ',': return {0x36, false};
    case '<': return {0x36, true}; case '.': return {0x37, false}; case '>': return {0x37, true};
    case '/': return {0x38, false}; case '?': return {0x38, true}; case '!': return {0x1e, true};
    case '@': return {0x1f, true}; case '#': return {0x20, true}; case '$': return {0x21, true};
    case '%': return {0x22, true}; case '^': return {0x23, true}; case '&': return {0x24, true};
    case '*': return {0x25, true}; case '(': return {0x26, true}; case ')': return {0x27, true};
    default: return {};
    }
}
}

TextFilterResult filter_us_ascii_text(const std::string_view text) {
    TextFilterResult result;
    result.text.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            result.text.push_back('\n'); ++i; continue;
        }
        if (c == '\t' || c == '\n' || (c >= 0x20 && c <= 0x7e)) result.text.push_back(static_cast<char>(c));
        else ++result.removed;
    }
    result.normalized_characters = result.text.size();
    return result;
}

TextMappingResult map_us_ascii_text(const std::string_view text) {
    TextMappingResult result;
    result.source_bytes = text.size();
    std::vector<unsigned char> normalized;
    normalized.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c >= 0x80) { result.error = TextPasteError::non_ascii; return result; }
        if (c == '\r') {
            if (i + 1 >= text.size() || text[i + 1] != '\n') {
                result.error = TextPasteError::bare_carriage_return; return result;
            }
            ++i; normalized.push_back('\n'); continue;
        }
        if (!(c == '\t' || c == '\n' || (c >= 0x20 && c <= 0x7e))) {
            result.error = TextPasteError::unsupported; return result;
        }
        normalized.push_back(c);
    }
    result.normalized_characters = normalized.size();
    if (normalized.empty()) { result.error = TextPasteError::empty; return result; }
    if (normalized.size() > 1024) { result.error = TextPasteError::too_long; return result; }
    result.gestures.reserve(normalized.size());
    for (const auto c : normalized) {
        Entry entry = c == '\t' ? Entry{0x2b, false} : c == '\n' ? Entry{0x28, false} : printable(c);
        if (entry.usage == 0) { result.error = TextPasteError::unsupported; result.gestures.clear(); return result; }
        TextGesture gesture;
        if (entry.shift) gesture.edges.push_back({kLeftShift, true});
        gesture.edges.push_back({entry.usage, true});
        gesture.edges.push_back({entry.usage, false});
        if (entry.shift) gesture.edges.push_back({kLeftShift, false});
        result.gestures.push_back(std::move(gesture));
    }
    return result;
}

}  // namespace kvmux
