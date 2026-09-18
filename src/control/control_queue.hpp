#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

#include "control/control_event.hpp"

namespace kvmux {

class ControlQueue {
 public:
  static constexpr std::size_t capacity = 128;
  static constexpr auto maximum_age = std::chrono::milliseconds(100);

  [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }
  [[nodiscard]] std::size_t size() const noexcept { return events_.size(); }
  [[nodiscard]] bool release_pending() const noexcept {
    return release_pending_;
  }

  void set_ready(bool ready) noexcept { ready_ = ready; }
  [[nodiscard]] SubmitResult submit(ControlEvent event,
                                    std::chrono::steady_clock::time_point now);
  [[nodiscard]] std::optional<ControlEvent> pop();
  void request_release() noexcept;
  [[nodiscard]] bool take_release_request() noexcept;

 private:
  void invalidate() noexcept;

  std::deque<ControlEvent> events_;
  std::uint64_t epoch_{1};
  bool ready_{false};
  bool release_pending_{false};
};

}  // namespace kvmux
