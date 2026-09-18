#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

#include "video/latest_value.hpp"

namespace kvmux {

template <typename T>
class GenerationMailbox {
 public:
  void set_generation(std::uint64_t generation) {
    {
      std::lock_guard lock(generation_mutex_);
      generation_ = generation;
    }
    latest_.clear();
  }

  void publish(T value) {
    std::lock_guard lock(generation_mutex_);
    if (value.generation == generation_) {
      latest_.publish(std::move(value));
    }
  }

  [[nodiscard]] std::optional<T> take() {
    auto value = latest_.take();
    std::lock_guard lock(generation_mutex_);
    if (value && value->generation != generation_) {
      return std::nullopt;
    }
    return value;
  }

  [[nodiscard]] std::uint64_t overwritten() const {
    return latest_.overwritten();
  }

 private:
  mutable std::mutex generation_mutex_;
  std::uint64_t generation_{};
  LatestValue<T> latest_;
};

}  // namespace kvmux
