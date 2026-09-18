#include "app/scroll_stitcher.hpp"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {
std::vector<std::uint8_t> image(std::uint32_t width, std::uint32_t first,
                                std::uint32_t rows) {
  std::vector<std::uint8_t> result;
  for (std::uint32_t y = first; y < first + rows; ++y)
    for (std::uint32_t x = 0; x < width; ++x) {
      result.push_back(static_cast<std::uint8_t>((y * 17 + x * 29) & 255));
      result.push_back(static_cast<std::uint8_t>((y * 41 + x * 7) & 255));
      result.push_back(static_cast<std::uint8_t>((y * 11 + x * 53) & 255));
    }
  return result;
}
bool check(bool value, const char* text) {
  if (!value) std::cerr << text << '\n';
  return value;
}
}  // namespace
int main() {
  using namespace kvmux;
  bool ok = true;
  ScrollStitcher stitch({.minimum_overlap = 3,
                         .maximum_normalized_error = 0.0,
                         .maximum_pixels = 1000,
                         .maximum_bytes = 3000});
  auto a = image(5, 0, 8), b = image(5, 3, 8), reverse = image(5, 1, 8),
       bad = image(5, 30, 8);
  ok &= check(stitch.add(a, 5, 8, 1).result == StitchResult::started, "start");
  ok &= check(stitch.add(a, 5, 8, 2).result == StitchResult::unchanged,
              "unchanged");
  auto update = stitch.add(b, 5, 8, 3);
  ok &=
      check(update.result == StitchResult::appended && update.displacement == 3,
            "append displacement");
  ok &= check(
      stitch.width() == 5 && stitch.height() == 11 &&
          std::vector<std::uint8_t>(stitch.pixels().begin(),
                                    stitch.pixels().end()) == image(5, 0, 11),
      "ordered pixels");
  ok &= check(stitch.add(reverse, 5, 8, 4).result == StitchResult::reverse,
              "reverse");
  ok &= check(stitch.height() == 11, "reverse preserves output");
  ok &= check(stitch.add(b, 5, 8, 3).result == StitchResult::stale, "stale");
  ok &= check(stitch.add(bad, 5, 8, 5).result == StitchResult::bad_match,
              "bad match");
  ok &=
      check(stitch.add(image(4, 0, 8), 4, 8, 6).result == StitchResult::invalid,
            "width stability");

  ScrollStitcher bounded({.minimum_overlap = 3,
                          .maximum_normalized_error = 0.0,
                          .maximum_pixels = 50,
                          .maximum_bytes = 1000});
  ok &= check(bounded.add(a, 5, 8, 1).result == StitchResult::started,
              "bounded start");
  ok &= check(bounded.add(b, 5, 8, 2).result == StitchResult::size_limit,
              "pixel cap");
  ok &= check(bounded.height() == 8, "cap preserves output");
  return ok ? 0 : 1;
}
