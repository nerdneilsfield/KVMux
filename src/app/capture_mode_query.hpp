#pragma once

#include <string_view>

namespace kvmux {

// A selected capture device with an empty mode list needs one mode query.
// Repeating it after an empty result keeps the UI thread busy, so the caller
// records the device id it already asked about.
inline bool should_query_capture_modes(int selected_device, int device_count,
                                       bool modes_empty, bool query_in_flight,
                                       std::string_view queried_id,
                                       std::string_view device_id) {
  if (query_in_flight || !modes_empty) return false;
  if (selected_device < 0 || selected_device >= device_count) return false;
  if (device_id.empty() || device_id == queried_id) return false;
  return true;
}

}  // namespace kvmux
