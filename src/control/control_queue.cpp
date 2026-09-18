#include "control/control_queue.hpp"

#include <utility>

namespace kvmux {

SubmitResult ControlQueue::submit(
    ControlEvent event, const std::chrono::steady_clock::time_point now) {
  if (!ready_ || event.epoch != epoch_) {
    return SubmitResult::not_ready;
  }
  if (events_.size() >= capacity ||
      (!events_.empty() && now - events_.front().timestamp > maximum_age)) {
    invalidate();
    return SubmitResult::overloaded;
  }

  if (!events_.empty()) {
    auto& previous = events_.back();
    if (previous.epoch == event.epoch) {
      if (auto* old_absolute = std::get_if<AbsoluteMotion>(&previous.payload)) {
        if (const auto* next = std::get_if<AbsoluteMotion>(&event.payload)) {
          old_absolute->x = next->x;
          old_absolute->y = next->y;
          previous.sequence = event.sequence;
          return SubmitResult::accepted;
        }
      }
      if (auto* old_relative = std::get_if<RelativeMotion>(&previous.payload)) {
        if (const auto* next = std::get_if<RelativeMotion>(&event.payload)) {
          old_relative->dx += next->dx;
          old_relative->dy += next->dy;
          previous.sequence = event.sequence;
          return SubmitResult::accepted;
        }
      }
    }
  }

  events_.push_back(std::move(event));
  return SubmitResult::accepted;
}

std::optional<ControlEvent> ControlQueue::pop() {
  if (events_.empty()) {
    return std::nullopt;
  }
  auto event = std::move(events_.front());
  events_.pop_front();
  return event;
}

void ControlQueue::request_release() noexcept { invalidate(); }

bool ControlQueue::take_release_request() noexcept {
  const bool pending = release_pending_;
  release_pending_ = false;
  return pending;
}

void ControlQueue::invalidate() noexcept {
  events_.clear();
  ++epoch_;
  ready_ = false;
  release_pending_ = true;
}

}  // namespace kvmux
