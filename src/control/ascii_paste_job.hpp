#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "control/control_event.hpp"

namespace kvmux {

// A mapped ASCII paste contains HID edges only. It deliberately retains no
// source text.
struct AsciiPasteJob {
  std::vector<std::vector<KeyEdge>> gestures;
};

// A relay submits an immutable, correlated request. The serial worker alone
// decides when its current serial state can start it.
struct AsciiPasteRequest {
  std::uint64_t job_id{};
  std::uint64_t owner_epoch{};
  std::uint64_t owner_intent{};
  AsciiPasteJob job;
};

enum class AsciiPasteState { idle, active, completed, canceled, failed };

struct AsciiPasteSnapshot {
  AsciiPasteState state{AsciiPasteState::idle};
  std::size_t total_gestures{};
  std::size_t completed_gestures{};
  std::uint64_t job_id{};
  [[nodiscard]] bool active() const noexcept {
    return state == AsciiPasteState::active;
  }
};

}  // namespace kvmux
