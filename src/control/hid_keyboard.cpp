#include "control/hid_keyboard.hpp"

#include <algorithm>

namespace kvmux {

bool HidKeyboardState::is_modifier(const std::uint8_t usage) noexcept {
    return usage >= 0xe0U && usage <= 0xe7U;
}

bool HidKeyboardState::press(const std::uint8_t usage) {
    if (is_modifier(usage)) {
        modifiers_ = static_cast<std::uint8_t>(modifiers_ | (1U << (usage - 0xe0U)));
        return true;
    }
    if (ignored_.contains(usage) ||
        std::find(keys_.begin(), keys_.end(), usage) != keys_.end()) {
        return !ignored_.contains(usage);
    }
    const auto slot = std::find(keys_.begin(), keys_.end(), std::uint8_t{0});
    if (slot == keys_.end()) {
        ignored_.insert(usage);
        return false;
    }
    *slot = usage;
    return true;
}

void HidKeyboardState::release(const std::uint8_t usage) noexcept {
    if (is_modifier(usage)) {
        modifiers_ = static_cast<std::uint8_t>(modifiers_ & ~(1U << (usage - 0xe0U)));
        return;
    }
    ignored_.erase(usage);
    const auto slot = std::find(keys_.begin(), keys_.end(), usage);
    if (slot != keys_.end()) {
        *slot = 0;
    }
}

void HidKeyboardState::clear() noexcept {
    modifiers_ = 0;
    keys_.fill(0);
    ignored_.clear();
}

}  // namespace kvmux
