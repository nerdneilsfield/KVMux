#include "input/input_router.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace kvmux {

bool Rect::contains(const double px, const double py) const noexcept {
    return width > 0 && height > 0 && px >= x && py >= y &&
           px < x + width && py < y + height;
}

Rect fit_video_rect(const Rect area, const int video_width,
                    const int video_height) noexcept {
    if (area.width <= 0 || area.height <= 0 || video_width <= 0 || video_height <= 0) {
        return {};
    }
    const double scale = std::min(area.width / static_cast<double>(video_width),
                                  area.height / static_cast<double>(video_height));
    const double width = static_cast<double>(video_width) * scale;
    const double height = static_cast<double>(video_height) * scale;
    return {area.x + (area.width - width) / 2.0,
            area.y + (area.height - height) / 2.0, width, height};
}

Rect target_input_rect(const Rect display, const TargetAspect aspect) noexcept {
    switch (aspect) {
    case TargetAspect::full_frame: return display;
    case TargetAspect::ratio_16_9: return fit_video_rect(display, 16, 9);
    case TargetAspect::ratio_16_10: return fit_video_rect(display, 16, 10);
    case TargetAspect::ratio_4_3: return fit_video_rect(display, 4, 3);
    }
    return display;
}

bool supported_usb_keyboard_usage(const std::uint16_t usage) noexcept {
    // SDL's keyboard page mappings used by the first version: ordinary keys,
    // locking keys, navigation/keypad keys, F13-F24, and eight modifiers.
    return (usage >= 0x04U && usage <= 0x73U) ||
           (usage >= 0x7fU && usage <= 0x82U) ||
           (usage >= 0x85U && usage <= 0x87U) ||
           (usage >= 0x89U && usage <= 0x8fU) ||
           (usage >= 0xe0U && usage <= 0xe7U);
}

InputRouter::InputRouter(ControlSink& sink, const std::uint16_t host_usage)
    : sink_(sink), host_usage_(host_usage) {}

void InputRouter::set_video_rect(const Rect rect) noexcept {
    if (rect.x != video_rect_.x || rect.y != video_rect_.y ||
        rect.width != video_rect_.width || rect.height != video_rect_.height) {
        pointer_ = {};
    }
    video_rect_ = rect;
}

void InputRouter::set_host_key(const std::uint16_t usage) noexcept {
    if (state_ == InputState::preview && supported_usb_keyboard_usage(usage)) {
        host_usage_ = usage;
    }
}

void InputRouter::set_relative_gain(const double gain) noexcept {
    if (state_ == InputState::preview && std::isfinite(gain) && gain > 0) {
        relative_gain_ = gain;
    }
}

void InputRouter::set_mouse_mode(const MouseMode mode) {
    if (mode == mouse_mode_) { return; }
    if (state_ != InputState::preview) {
        begin_release();
        return;
    }
    mouse_mode_ = mode;
    pointer_ = {};
    residual_x_ = residual_y_ = wheel_residual_ = 0;
}

bool InputRouter::sink_ready_released() const {
    const auto value = sink_.snapshot();
    return value.state == ControlConnectionState::ready && value.target_usb_ready &&
           value.release_confirmed;
}

bool InputRouter::submit(ControlPayload payload) {
    const auto snapshot = sink_.snapshot();
    const auto result = sink_.submit(
        {snapshot.epoch, ++sequence_, Clock::now(), payload});
    if (result != SubmitResult::accepted) { if (snapshot.recoverable_transport) recover(); else fail(); return false; }
    std::visit([this](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, AbsoluteMotion> ||
                      std::is_same_v<T, ButtonEdge> || std::is_same_v<T, VerticalWheel>) {
            if (mouse_mode_ == MouseMode::absolute) {
                // These events contain the router's integer coordinate / 4095.
                // SerialWorker's floor(4096 * value), clamped to 4095, recovers it.
                pointer_.submitted_absolute = {
                    static_cast<std::uint16_t>(std::lround(value.x * 4095.0)),
                    static_cast<std::uint16_t>(std::lround(value.y * 4095.0))};
            }
        } else if constexpr (std::is_same_v<T, RelativeMotion>) {
            pointer_.submitted_relative = {static_cast<int>(value.dx), static_cast<int>(value.dy)};
        }
    }, payload);
    return true;
}

void InputRouter::handle(const InputEvent& event) {
    // A physical key edge cancels synthetic text before it can inject another character.
    if (text_active_ && std::holds_alternative<InputKey>(event.payload)) { cancel_text(); }
    std::visit([this](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, InputPointerMotion> ||
                      std::is_same_v<T, InputButton> || std::is_same_v<T, InputWheel>) {
            if (captured() && video_fresh_ && video_rect_.width > 0 && video_rect_.height > 0) {
                pointer_.video_rect = video_rect_;
                pointer_.video_local = {value.x - video_rect_.x, value.y - video_rect_.y};
            }
        }
        if constexpr (std::is_same_v<T, InputKey>) { handle_key(value); }
        else if constexpr (std::is_same_v<T, InputPointerMotion>) { handle_pointer(value); }
        else if constexpr (std::is_same_v<T, InputRelativeMotion>) { handle_relative(value); }
        else if constexpr (std::is_same_v<T, InputButton>) { handle_button(value); }
        else { handle_wheel(value); }
    }, event.payload);
}

void InputRouter::handle_key(const InputKey& key) {
    if (!supported_usb_keyboard_usage(key.usb_usage)) { return; }
    if (key.pressed) {
        physical_keys_.insert(key.usb_usage);
        if (state_ == InputState::arming) { isolated_keys_.insert(key.usb_usage); }
    } else {
        physical_keys_.erase(key.usb_usage);
    }

    if (key.usb_usage == host_usage_) {
        if (key.pressed && (capture_intended() || injected_active())) {
            swallowed_host_releases_.insert(key.usb_usage);
            begin_release();
        } else if (!key.pressed) {
            swallowed_host_releases_.erase(key.usb_usage);
        }
        return;
    }
    if (isolated_keys_.contains(key.usb_usage)) {
        if (!key.pressed) { isolated_keys_.erase(key.usb_usage); }
        return;
    }
    if (key.repeat) return;
    if (capture_intended()) {
        if (key.pressed) (void)held_.press(static_cast<std::uint8_t>(key.usb_usage));
        else held_.release(static_cast<std::uint8_t>(key.usb_usage));
    }
    if (state_ != InputState::captured) return;
    (void)submit(KeyEdge{static_cast<std::uint8_t>(key.usb_usage), key.pressed});
}

void InputRouter::handle_pointer(const InputPointerMotion& motion) {
    if (!capture_intended() || mouse_mode_ != MouseMode::absolute) return;
    const auto [x, y] = absolute(motion.x, motion.y);
    desired_x_ = x; desired_y_ = y;
    if (!captured()) return;
    (void)submit(AbsoluteMotion{static_cast<double>(x) / 4095.0,
                                static_cast<double>(y) / 4095.0});
}

void InputRouter::handle_relative(const InputRelativeMotion& motion) {
    if (state_ != InputState::captured || mouse_mode_ != MouseMode::relative) { return; }
    residual_x_ += motion.dx * relative_gain_;
    residual_y_ += motion.dy * relative_gain_;
    const int dx = static_cast<int>(std::trunc(residual_x_));
    const int dy = static_cast<int>(std::trunc(residual_y_));
    residual_x_ -= static_cast<double>(dx);
    residual_y_ -= static_cast<double>(dy);
    send_relative_integral(dx, dy, 0);
}

void InputRouter::handle_button(const InputButton& button) {
    const auto number = static_cast<std::uint8_t>(button.button);
    const auto bit = static_cast<std::uint8_t>(1U << (number - 1U));
    if (state_ == InputState::preview && button.pressed && video_fresh_ &&
        sink_ready_released() && video_rect_.contains(button.x, button.y)) {
        state_ = InputState::arming;
        activation_button_ = number;
        activation_released_ = false;
        isolated_keys_ = physical_keys_; held_.clear();
        return;
    }
    if (state_ == InputState::arming) {
        if (number == activation_button_ && !button.pressed) { activation_released_ = true; }
        return;
    }
    if (!capture_intended()) return;
    if (button.pressed) {
        if (!video_rect_.contains(button.x, button.y) && buttons_ == 0) { return; }
        buttons_ = static_cast<std::uint8_t>(buttons_ | bit);
    } else {
        if ((buttons_ & bit) == 0) { return; }
        buttons_ = static_cast<std::uint8_t>(buttons_ & ~bit);
    }
    const auto [x, y] = absolute(button.x, button.y);
    desired_x_ = x; desired_y_ = y;
    if (!captured()) return;
    (void)submit(ButtonEdge{static_cast<std::uint8_t>(number - 1U), button.pressed,
                            static_cast<double>(x) / 4095.0,
                            static_cast<double>(y) / 4095.0});
}

void InputRouter::handle_wheel(const InputWheel& wheel) {
    if (state_ != InputState::captured ||
        (!video_rect_.contains(wheel.x, wheel.y) && buttons_ == 0)) { return; }
    wheel_residual_ += wheel.vertical_steps;
    const int steps = static_cast<int>(std::trunc(wheel_residual_));
    wheel_residual_ -= static_cast<double>(steps);
    if (steps == 0) { return; }
    if (mouse_mode_ == MouseMode::relative) {
        send_relative_integral(0, 0, steps);
        return;
    }
    const auto [x, y] = absolute(wheel.x, wheel.y);
    int left = steps;
    while (left != 0 && state_ == InputState::captured) {
        const int part = std::clamp(left, -127, 127);
        if (!submit(VerticalWheel{static_cast<double>(part),
                                  static_cast<double>(x) / 4095.0,
                                  static_cast<double>(y) / 4095.0})) { return; }
        left -= part;
    }
}

void InputRouter::send_relative_integral(int dx, int dy, int wheel) {
    while ((dx != 0 || dy != 0 || wheel != 0) && state_ == InputState::captured) {
        const int sx = std::clamp(dx, -127, 127);
        const int sy = std::clamp(dy, -127, 127);
        const int sw = std::clamp(wheel, -127, 127);
        ControlPayload payload = sw != 0
            ? ControlPayload{VerticalWheel{static_cast<double>(sw), 0, 0}}
            : ControlPayload{RelativeMotion{static_cast<double>(sx), static_cast<double>(sy)}};
        if (!submit(std::move(payload))) { return; }
        dx -= sx;
        dy -= sy;
        wheel -= sw;
    }
}

std::pair<std::uint16_t, std::uint16_t> InputRouter::absolute(
    const double x, const double y) const noexcept {
    if (video_rect_.width <= 0 || video_rect_.height <= 0) { return {0, 0}; }
    // WCH protocol 2.2.4 uses 4096 * position / screen extent, not extent-1.
    // Full-frame capture scaling cancels in this ratio, even when the target
    // desktop and transmitted video have different pixel dimensions.
    const double u = std::clamp((x - video_rect_.x) / video_rect_.width, 0.0, 1.0);
    const double v = std::clamp((y - video_rect_.y) / video_rect_.height, 0.0, 1.0);
    return {static_cast<std::uint16_t>(std::min(4095.0, std::floor(4096.0 * u))),
            static_cast<std::uint16_t>(std::min(4095.0, std::floor(4096.0 * v)))};
}

DesiredInputState InputRouter::desired() const {
    DesiredInputState value;
    value.modifiers = held_.modifiers(); value.keys = held_.keys();
    // Canonical reports without changing the serial rollover/ignored-key policy.
    std::sort(value.keys.begin(), value.keys.end());
    value.buttons = buttons_; value.mode = mouse_mode_;
    value.absolute_x = desired_x_; value.absolute_y = desired_y_;
    return value;
}
void InputRouter::recover() noexcept {
    if (temporary_intent_) { begin_release(); return; }
    if (!capture_intended()) return;
    state_ = InputState::recovering; sync_.reset(); barrier_epoch_ = 0;
    residual_x_ = residual_y_ = wheel_residual_ = 0;
    special_steps_.clear(); pending_special_.reset(); text_active_ = false; text_completion_pending_ = false; text_gestures_.clear(); next_text_gesture_ = 0; pointer_ = {};
}
void InputRouter::synchronize(Clock::time_point now) {
    const auto snapshot = sink_.snapshot();
    if (!video_fresh_ || !sink_ready_released()) { sync_.reset(); return; }
    const auto state = desired();
    auto equal = [](const DesiredInputState& a, const DesiredInputState& b) {
        return a.modifiers == b.modifiers && a.keys == b.keys && a.buttons == b.buttons &&
            a.mode == b.mode && a.absolute_x == b.absolute_x && a.absolute_y == b.absolute_y;
    };
    if (sync_) {
        const auto& ack = snapshot.applied;
        if (ack.known && ack.epoch == sync_->epoch && ack.intent_generation == sync_->intent_generation &&
            ack.revision == sync_->revision && equal(ack.state, sync_->state)) {
            const bool unchanged = equal(state, sync_->state);
            sync_.reset();
            if (unchanged) {
                barrier_epoch_ = snapshot.epoch;
                if (pending_special_) {
                    auto keys = *pending_special_; pending_special_.reset(); schedule_special(keys, now);
                } else if (!text_active_) {
                    state_ = InputState::captured;
                }
                return;
            }
        } else if (snapshot.epoch != sync_->epoch || now - sync_at_ >= std::chrono::milliseconds(500)) sync_.reset();
        else return;
    }
    InputSync value{snapshot.epoch, intent_, ++revision_, state};
    if (sink_.synchronize(value) == SubmitResult::accepted) { sync_ = value; sync_at_ = now; }
}
void InputRouter::tick(const Clock::time_point now) {
    const auto snapshot = sink_.snapshot();
    if (snapshot.recoverable_transport && state_ == InputState::arming && activation_released_) {
        ++intent_; revision_ = 0; activation_button_ = 0; state_ = InputState::recovering;
    }
    if (snapshot.recoverable_transport && capture_intended() &&
        (!video_fresh_ || !sink_ready_released() || (barrier_epoch_ && barrier_epoch_ != snapshot.epoch))) recover();
    if (temporary_intent_ && (!video_fresh_ || !sink_ready_released())) { begin_release(); return; }
    if (state_ == InputState::arming && activation_released_ && video_fresh_ && sink_ready_released()) {
        activation_button_ = 0;
        state_ = InputState::captured;
    } else if (state_ == InputState::releasing && snapshot.release_confirmed) {
        state_ = InputState::preview; release_requested_ = false;
    }
    if (text_completion_pending_) {
        text_completion_pending_ = false;
        text_active_ = false;
        temporary_intent_ = false;
    }
    if (snapshot.recoverable_transport &&
        (state_ == InputState::recovering || pending_special_ ||
         (text_active_ && barrier_epoch_ != snapshot.epoch))) {
        synchronize(now);
    }
    if (text_active_ && special_steps_.empty() && next_text_gesture_ < text_gestures_.size() &&
        (!snapshot.recoverable_transport || barrier_epoch_ == snapshot.epoch)) {
        schedule_text(now);
    }
    while (!special_steps_.empty() && special_steps_.front().due <= now) {
        auto step = std::move(special_steps_.front()); special_steps_.erase(special_steps_.begin());
        for (const auto edge : step.edges) {
            if (!submit(edge)) { special_steps_.clear(); return; }
        }
    }
    if (text_active_ && special_steps_.empty() && next_text_gesture_ == text_gestures_.size()) {
        // Keep the temporary lease through this tick so the final up edges can drain.
        text_completion_pending_ = true;
    }
    if (temporary_intent_ && !pending_special_ && special_steps_.empty() && !text_active_) {
        // Keep the final up edges inside the active lease until the next UI turn.
        temporary_intent_ = false;
    }
}

void InputRouter::begin_release() noexcept {
    pointer_ = {};
    special_steps_.clear(); pending_special_.reset(); text_active_ = false; text_completion_pending_ = false; text_gestures_.clear(); next_text_gesture_ = 0; temporary_intent_ = false;
    sync_.reset(); barrier_epoch_ = 0; held_.clear();
    buttons_ = 0;
    residual_x_ = residual_y_ = wheel_residual_ = 0;
    isolated_keys_.clear();
    if (!release_requested_) { sink_.release_all(); release_requested_ = true; }
    state_ = InputState::releasing;
}

void InputRouter::release() noexcept { if (state_ != InputState::preview || injected_active()) begin_release(); }
void InputRouter::focus_lost() noexcept { begin_release(); }
void InputRouter::minimized() noexcept { begin_release(); }
void InputRouter::video_stale() noexcept { video_fresh_ = false; if (sink_.snapshot().recoverable_transport) recover(); else begin_release(); }
void InputRouter::fail() noexcept { pointer_ = {}; sink_.release_all(); release_requested_ = true; state_ = InputState::fault; }
void InputRouter::clear_fault() noexcept {
    if (state_ == InputState::fault && sink_ready_released()) {
        release_requested_ = false;
        state_ = InputState::preview;
    }
}

bool InputRouter::send_special(const SpecialKeys keys, const Clock::time_point now) {
    if (state_ != InputState::preview || special_active() || !sink_ready_released()) return false;
    if (sink_.snapshot().recoverable_transport) {
        if (!video_fresh_) return false;
        ++intent_; revision_ = 0; temporary_intent_ = true; pending_special_ = keys;
        held_.clear(); buttons_ = 0; synchronize(now); return true;
    }
    schedule_special(keys, now); return !special_steps_.empty();
}
TextMappingResult InputRouter::start_text(const std::string_view text, const Clock::time_point now) {
    text_result_ = map_us_ascii_text(text);
    if (!text_result_ || state_ != InputState::preview || special_active() || text_active_ || !sink_ready_released()) {
        if (text_result_) text_result_.error = TextPasteError::unsupported;
        return text_result_;
    }
    text_gestures_ = text_result_.gestures;
    next_text_gesture_ = 0;
    text_completion_pending_ = false;
    text_active_ = true;
    temporary_intent_ = true;
    if (sink_.snapshot().recoverable_transport) {
        if (!video_fresh_) { cancel_text(); return text_result_; }
        ++intent_; revision_ = 0; synchronize(now);
    } else {
        schedule_text(now);
    }
    return text_result_;
}

void InputRouter::cancel_text() noexcept {
    if (text_active_) begin_release();
}

void InputRouter::schedule_text(const Clock::time_point now) {
    if (next_text_gesture_ == text_gestures_.size()) return;
    special_steps_.push_back({now, text_gestures_[next_text_gesture_++].edges});
    // A full gesture is submitted together; the next character begins only after 50 ms.
    if (next_text_gesture_ < text_gestures_.size()) {
        special_steps_.push_back({now + std::chrono::milliseconds(50), {}});
    }
}

void InputRouter::schedule_special(const SpecialKeys keys, const Clock::time_point now) {
    std::vector<std::uint8_t> usages;
    if (keys == SpecialKeys::control_alt_delete) usages = {0xe0, 0xe2, 0x4c};
    else if (keys == SpecialKeys::alt_tab) usages = {0xe2, 0x2b};
    else if (supported_usb_keyboard_usage(host_usage_)) usages = {static_cast<std::uint8_t>(host_usage_)};
    else return;
    std::vector<KeyEdge> down, up;
    for (const auto usage : usages) down.push_back({usage, true});
    for (auto it = usages.rbegin(); it != usages.rend(); ++it) up.push_back({*it, false});
    special_steps_.push_back({now, std::move(down)});
    special_steps_.push_back({now + std::chrono::milliseconds(20), std::move(up)});
}

}  // namespace kvmux
