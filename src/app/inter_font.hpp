#pragma once

#include <cstddef>
#include <cstdint>

namespace kvmux::font {

// Inter Regular 4.1, embedded by tools/embed_font.py. The atlas does not own
// these bytes.
extern const std::uint8_t kInterRegular[];
extern const std::size_t kInterRegularSize;

}  // namespace kvmux::font
