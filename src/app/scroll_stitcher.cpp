#include "app/scroll_stitcher.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace kvmux {
namespace {
bool byte_count(std::uint32_t width, std::uint32_t height,
                std::uint64_t& result) {
  result = static_cast<std::uint64_t>(width) * height * 3U;
  return width != 0 && height != 0 &&
         result <= std::numeric_limits<std::size_t>::max();
}
}  // namespace

ScrollStitcher::ScrollStitcher(ScrollStitcherConfig config) : config_(config) {}

void ScrollStitcher::reset() noexcept {
  width_ = frame_height_ = 0;
  sequence_ = 0;
  previous_.clear();
  output_.clear();
}

double ScrollStitcher::error(std::span<const std::uint8_t> next,
                             std::uint32_t displacement, bool reverse) const {
  const auto rows = frame_height_ - displacement;
  const auto stride = static_cast<std::size_t>(width_) * 3;
  const auto previous_row = reverse ? 0U : displacement;
  const auto next_row = reverse ? displacement : 0U;
  long double sum = 0;
  const auto count = static_cast<std::size_t>(rows) * stride;
  for (std::size_t i = 0; i < count; ++i) {
    const int delta =
        static_cast<int>(
            previous_[static_cast<std::size_t>(previous_row) * stride + i]) -
        static_cast<int>(next[static_cast<std::size_t>(next_row) * stride + i]);
    sum += static_cast<long double>(delta * delta);
  }
  return static_cast<double>(
      sum / (static_cast<long double>(count) * 255.0L * 255.0L));
}

StitchUpdate ScrollStitcher::add(std::span<const std::uint8_t> rgb,
                                 std::uint32_t width, std::uint32_t height,
                                 std::uint64_t sequence) {
  std::uint64_t bytes = 0;
  if (!byte_count(width, height, bytes) || rgb.size() != bytes ||
      config_.minimum_overlap == 0 || config_.minimum_overlap >= height)
    return {StitchResult::invalid, 0};
  if (!previous_.empty() && sequence <= sequence_)
    return {StitchResult::stale, 0};
  if (previous_.empty()) {
    if (static_cast<std::uint64_t>(width) * height > config_.maximum_pixels ||
        bytes > config_.maximum_bytes)
      return {StitchResult::size_limit, 0};
    width_ = width;
    frame_height_ = height;
    sequence_ = sequence;
    previous_.assign(rgb.begin(), rgb.end());
    output_ = previous_;
    return {StitchResult::started, 0};
  }
  if (width != width_ || height != frame_height_)
    return {StitchResult::invalid, 0};

  double best = error(rgb, 0, false);
  if (best <= config_.maximum_normalized_error) {
    sequence_ = sequence;
    previous_.assign(rgb.begin(), rgb.end());
    return {StitchResult::unchanged, 0};
  }
  double forward_error = std::numeric_limits<double>::infinity();
  double reverse_error = forward_error;
  std::uint32_t forward = 0, reverse = 0;
  const auto maximum_displacement = height - config_.minimum_overlap;
  for (std::uint32_t d = 1; d <= maximum_displacement; ++d) {
    const auto f = error(rgb, d, false);
    if (f < forward_error) {
      forward_error = f;
      forward = d;
    }
    const auto r = error(rgb, d, true);
    if (r < reverse_error) {
      reverse_error = r;
      reverse = d;
    }
  }
  if (reverse_error <= config_.maximum_normalized_error &&
      reverse_error < forward_error)
    return {StitchResult::reverse, reverse};
  if (forward_error > config_.maximum_normalized_error)
    return {StitchResult::bad_match, 0};

  const auto new_height =
      static_cast<std::uint64_t>(output_.size() /
                                 (static_cast<std::size_t>(width_) * 3)) +
      forward;
  const auto new_bytes = new_height * width_ * 3U;
  if (new_height * width_ > config_.maximum_pixels ||
      new_bytes > config_.maximum_bytes ||
      new_bytes > std::numeric_limits<std::size_t>::max())
    return {StitchResult::size_limit, 0};
  const auto stride = static_cast<std::size_t>(width_) * 3;
  output_.insert(output_.end(),
                 rgb.end() - static_cast<std::ptrdiff_t>(forward * stride),
                 rgb.end());
  previous_.assign(rgb.begin(), rgb.end());
  sequence_ = sequence;
  return {StitchResult::appended, forward};
}

std::uint32_t ScrollStitcher::height() const noexcept {
  return width_ == 0
             ? 0
             : static_cast<std::uint32_t>(
                   output_.size() / (static_cast<std::size_t>(width_) * 3));
}

}  // namespace kvmux
