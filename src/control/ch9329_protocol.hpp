#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace kvmux::ch9329 {

inline constexpr std::uint8_t kHeader0 = 0x57;
inline constexpr std::uint8_t kHeader1 = 0xAB;
inline constexpr std::size_t kMaxDataLength = 64;
inline constexpr std::size_t kMaxFrameLength = 70;

enum class Command : std::uint8_t {
  get_info = 0x01,
  keyboard = 0x02,
  absolute_mouse = 0x04,
  relative_mouse = 0x05,
  get_config = 0x08,
};

struct Frame {
  std::uint8_t address{};
  std::uint8_t command{};
  std::vector<std::uint8_t> data;

  friend bool operator==(const Frame&, const Frame&) = default;
};

[[nodiscard]] std::vector<std::uint8_t> encode(const Frame& frame);
[[nodiscard]] Frame keyboard_report(std::uint8_t address,
                                    std::uint8_t modifiers,
                                    std::span<const std::uint8_t, 6> usages);
[[nodiscard]] Frame absolute_mouse_report(std::uint8_t address,
                                          std::uint8_t buttons, std::uint16_t x,
                                          std::uint16_t y, std::int8_t wheel);
[[nodiscard]] Frame relative_mouse_report(std::uint8_t address,
                                          std::uint8_t buttons, std::int8_t dx,
                                          std::int8_t dy, std::int8_t wheel);

class Parser {
 public:
  [[nodiscard]] std::vector<Frame> feed(std::span<const std::uint8_t> bytes);
  [[nodiscard]] std::size_t buffered_bytes() const noexcept {
    return buffer_.size();
  }
  void reset() noexcept { buffer_.clear(); }

 private:
  std::vector<std::uint8_t> buffer_;
};

}  // namespace kvmux::ch9329
