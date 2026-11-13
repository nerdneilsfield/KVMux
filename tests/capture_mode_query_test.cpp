#include "app/capture_mode_query.hpp"

int main() {
  using kvmux::should_query_capture_modes;
  // The default device is selected and its modes have never been requested.
  if (!should_query_capture_modes(0, 2, true, false, "", "cam-a")) return 1;
  // That device was already queried, including when the list came back empty.
  if (should_query_capture_modes(0, 2, true, false, "cam-a", "cam-a")) return 1;
  if (should_query_capture_modes(0, 2, true, true, "", "cam-a")) return 1;
  if (should_query_capture_modes(0, 2, false, false, "", "cam-a")) return 1;
  if (should_query_capture_modes(-1, 2, true, false, "", "cam-a")) return 1;
  // Choosing another device asks for its modes.
  if (!should_query_capture_modes(1, 2, true, false, "cam-a", "cam-b"))
    return 1;
  if (should_query_capture_modes(0, 1, true, false, "", "")) return 1;
  return 0;
}
