#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace kvmux {

template <typename T>
class LatestValue {
 public:
  void publish(T value) {
    std::optional<T> replaced;
    {
      std::lock_guard lock(mutex_);
      if (value_) {
        replaced = std::move(value_);
        ++overwritten_;
      }
      value_ = std::move(value);
    }
  }

  [[nodiscard]] std::optional<T> take() {
    std::lock_guard lock(mutex_);
    auto value = std::move(value_);
    value_.reset();
    return value;
  }

  void clear() {
    std::optional<T> removed;
    {
      std::lock_guard lock(mutex_);
      removed = std::move(value_);
      value_.reset();
    }
  }

  [[nodiscard]] std::uint64_t overwritten() const {
    std::lock_guard lock(mutex_);
    return overwritten_;
  }

 private:
  mutable std::mutex mutex_;
  std::optional<T> value_;
  std::uint64_t overwritten_{};
};

}  // namespace kvmux
