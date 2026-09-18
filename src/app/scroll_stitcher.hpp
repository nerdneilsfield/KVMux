#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace kvmux {

enum class StitchResult {
  started,
  appended,
  unchanged,
  bad_match,
  reverse,
  stale,
  size_limit,
  invalid
};

struct StitchUpdate {
  StitchResult result{StitchResult::invalid};
  std::uint32_t displacement{};
};

struct ScrollStitcherConfig {
  std::uint32_t minimum_overlap{16};
  double maximum_normalized_error{0.01};
  std::uint64_t maximum_pixels{100'000'000};
  std::uint64_t maximum_bytes{300'000'000};
};

class ScrollStitcher {
 public:
  explicit ScrollStitcher(ScrollStitcherConfig config = {});
  StitchUpdate add(std::span<const std::uint8_t> packed_rgb,
                   std::uint32_t width, std::uint32_t height,
                   std::uint64_t sequence);
  void reset() noexcept;

  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept;
  [[nodiscard]] std::span<const std::uint8_t> pixels() const noexcept {
    return output_;
  }

 private:
  double error(std::span<const std::uint8_t> next, std::uint32_t displacement,
               bool reverse) const;

  ScrollStitcherConfig config_;
  std::uint32_t width_{};
  std::uint32_t frame_height_{};
  std::uint64_t sequence_{};
  std::vector<std::uint8_t> previous_;
  std::vector<std::uint8_t> output_;
};

}  // namespace kvmux
