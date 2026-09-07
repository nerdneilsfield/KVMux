#pragma once

#include "video/latest_value.hpp"

#include <cstdint>
#include <optional>
#include <utility>

namespace kvmux {

template <typename T>
class GenerationMailbox {
public:
    void set_generation(std::uint64_t generation) {
        generation_ = generation;
        latest_.clear();
    }

    void publish(T value) {
        if (value.generation == generation_) {
            latest_.publish(std::move(value));
        }
    }

    [[nodiscard]] std::optional<T> take() {
        auto value = latest_.take();
        if (value && value->generation != generation_) {
            return std::nullopt;
        }
        return value;
    }

    [[nodiscard]] std::uint64_t overwritten() const { return latest_.overwritten(); }

private:
    std::uint64_t generation_{};
    LatestValue<T> latest_;
};

}  // namespace kvmux
