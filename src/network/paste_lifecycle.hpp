#pragma once

#include <cstdint>

namespace kvmux::relay {

// A paste is owned by the exact control epoch and intent that started it.
// Only that owner can send its ordered cancellation. A later tuple must not
// accidentally cancel a newer owner; it only abandons the local old snapshot.
enum class PasteLifecycleAction { none, abandon_owner, cancel_current };

[[nodiscard]] inline PasteLifecycleAction paste_lifecycle_action(
    bool pending, std::uint64_t paste_epoch, std::uint64_t paste_intent,
    std::uint64_t control_epoch, std::uint64_t control_intent, bool active,
    bool ready, bool executing = false) noexcept {
  if (!pending) return PasteLifecycleAction::none;
  if (paste_epoch != control_epoch || paste_intent != control_intent)
    return PasteLifecycleAction::abandon_owner;
  // `ready` includes a 500 ms presented-frame window. Once Execute is in
  // flight the transaction is renewed by PasteKeepalive, so a stalled frame
  // must not cancel keystrokes that are already on the serial worker.
  return active && (ready || executing) ? PasteLifecycleAction::none
                                        : PasteLifecycleAction::cancel_current;
}

}  // namespace kvmux::relay
