#pragma once

#include "control/control_event.hpp"

#include <cstddef>
#include <vector>

namespace kvmux {

// A mapped ASCII paste contains HID edges only. It deliberately retains no source text.
struct AsciiPasteJob {
    std::vector<std::vector<KeyEdge>> gestures;
};

enum class AsciiPasteState { idle, active, completed, canceled, failed };

struct AsciiPasteSnapshot {
    AsciiPasteState state{AsciiPasteState::idle};
    std::size_t total_gestures{};
    std::size_t completed_gestures{};
    [[nodiscard]] bool active() const noexcept { return state == AsciiPasteState::active; }
};

}  // namespace kvmux
