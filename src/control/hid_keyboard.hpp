#pragma once

#include <array>
#include <cstdint>
#include <unordered_set>

namespace kvmux {

class HidKeyboardState {
public:
    [[nodiscard]] bool press(std::uint8_t usage);
    void release(std::uint8_t usage) noexcept;
    void clear() noexcept;

    [[nodiscard]] std::uint8_t modifiers() const noexcept { return modifiers_; }
    [[nodiscard]] const std::array<std::uint8_t, 6>& keys() const noexcept { return keys_; }
    [[nodiscard]] bool ignored(std::uint8_t usage) const { return ignored_.contains(usage); }

private:
    static bool is_modifier(std::uint8_t usage) noexcept;

    std::uint8_t modifiers_{};
    std::array<std::uint8_t, 6> keys_{};
    std::unordered_set<std::uint8_t> ignored_;
};

}  // namespace kvmux
