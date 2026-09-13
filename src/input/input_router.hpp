#pragma once

#include "control/control_sink.hpp"
#include "control/hid_keyboard.hpp"
#include "input/us_ascii_text.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace kvmux {

enum class InputState { preview, arming, captured, recovering, releasing, fault };
enum class InputMouseButton : std::uint8_t { left = 1, right = 2, middle = 3 };
enum class SpecialKeys { control_alt_delete, alt_tab, host_key };
enum class TargetAspect { full_frame, ratio_16_9, ratio_16_10, ratio_4_3 };

struct InputKey {
    std::uint16_t usb_usage{};
    bool pressed{};
    bool repeat{};
};
struct InputPointerMotion { double x{}; double y{}; };
struct InputRelativeMotion { double dx{}; double dy{}; };
struct InputButton {
    InputMouseButton button{InputMouseButton::left};
    bool pressed{};
    double x{};
    double y{};
};
struct InputWheel { double vertical_steps{}; double x{}; double y{}; };
using InputPayload = std::variant<InputKey, InputPointerMotion, InputRelativeMotion,
                                  InputButton, InputWheel>;
struct InputEvent { InputPayload payload; };

struct Rect {
    double x{};
    double y{};
    double width{};
    double height{};
    [[nodiscard]] bool contains(double px, double py) const noexcept;
};

[[nodiscard]] Rect fit_video_rect(Rect area, int video_width, int video_height) noexcept;
// Centered target desktop inside the rendered capture; does not alter rendering.
[[nodiscard]] Rect target_input_rect(Rect display, TargetAspect aspect) noexcept;
[[nodiscard]] bool supported_usb_keyboard_usage(std::uint16_t usage) noexcept;

// UI-thread diagnostics. Submission means queue acceptance, not a device ACK.
// Counts are safe metadata only. "Scheduled" means the router accepted all
// edges of a gesture into the control sink; it does not mean target delivery.
struct TextPasteSnapshot {
    TextPasteError error{TextPasteError::none};
    std::size_t source_bytes{};
    std::size_t normalized_characters{};
    std::size_t planned_gestures{};
    std::size_t scheduled_gestures{};
    bool active{};
    bool pending{};
};

struct InputPointerSnapshot {
    std::optional<Rect> video_rect; // Event-time active desktop rectangle in logical window units.
    std::optional<std::pair<double, double>> video_local; // Logical window units.
    std::optional<std::pair<std::uint16_t, std::uint16_t>> submitted_absolute; // 0..4095.
    std::optional<std::pair<int, int>> submitted_relative; // Last report delta.
};

class InputRouter {
public:
    using Clock = std::chrono::steady_clock;

    explicit InputRouter(ControlSink& sink, std::uint16_t host_usage = 0xe4);

    [[nodiscard]] InputState state() const noexcept { return state_; }
    [[nodiscard]] MouseMode mouse_mode() const noexcept { return mouse_mode_; }
    [[nodiscard]] bool captured() const noexcept { return state_ == InputState::captured; }

    [[nodiscard]] bool capture_intended() const noexcept { return state_ == InputState::captured || state_ == InputState::recovering || (state_ == InputState::arming && activation_released_); }

    [[nodiscard]] bool special_active() const noexcept { return pending_special_.has_value() || !special_steps_.empty(); }
    // Synthetic input needs the same temporary control lease as special-key gestures.
    [[nodiscard]] bool injected_active() const noexcept { return special_active() || text_active_; }
    [[nodiscard]] bool text_active() const noexcept { return text_active_; }
    [[nodiscard]] TextMappingResult start_text(std::string_view text, Clock::time_point now = Clock::now());
    void cancel_text() noexcept;
    [[nodiscard]] TextMappingResult text_snapshot() const { return text_result_; }
    [[nodiscard]] TextPasteSnapshot text_paste_snapshot() const noexcept;

    // Set the active desktop region, which may exclude bars inside the capture.
    // Use the same coordinate space as pointer/button/wheel events (SDL/ImGui
    // logical window coordinates, not GL framebuffer pixels).
    void set_video_rect(Rect rect) noexcept;
    void set_video_fresh(bool fresh) noexcept {
        video_fresh_ = fresh;
        if (!fresh) { pointer_ = {}; }
    }
    [[nodiscard]] InputPointerSnapshot pointer_snapshot() const noexcept { return pointer_; }
    void set_host_key(std::uint16_t usage) noexcept;
    void set_relative_gain(double gain) noexcept;
    void set_mouse_mode(MouseMode mode);

    void handle(const InputEvent& event);
    void tick(Clock::time_point now = Clock::now());
    void focus_lost() noexcept;
    void minimized() noexcept;
    void video_stale() noexcept;
    void release() noexcept;
    void clear_fault() noexcept;

    [[nodiscard]] bool send_special(SpecialKeys keys,
                                    Clock::time_point now = Clock::now());

private:
    struct SpecialStep {
        Clock::time_point due;
        std::vector<KeyEdge> edges;
        bool text_gesture{};
    };

    [[nodiscard]] bool sink_ready_released() const;
    bool submit(ControlPayload payload);
    void handle_key(const InputKey& key);
    void handle_pointer(const InputPointerMotion& motion);
    void handle_relative(const InputRelativeMotion& motion);
    void handle_button(const InputButton& button);
    void handle_wheel(const InputWheel& wheel);
    void begin_release() noexcept;
    void fail() noexcept;
    void recover() noexcept;
    DesiredInputState desired() const;
    void synchronize(Clock::time_point now);
    void schedule_special(SpecialKeys, Clock::time_point);
    void schedule_text(Clock::time_point);
    void send_relative_integral(int dx, int dy, int wheel);
    [[nodiscard]] std::pair<std::uint16_t, std::uint16_t> absolute(double x,
                                                                  double y) const noexcept;

    HidKeyboardState held_;
    std::uint64_t intent_{}, revision_{}, barrier_epoch_{};
    std::optional<InputSync> sync_;
    Clock::time_point sync_at_{};
    std::uint16_t desired_x_{}, desired_y_{};
    std::optional<SpecialKeys> pending_special_;
    bool temporary_intent_{};
    ControlSink& sink_;
    InputState state_{InputState::preview};
    MouseMode mouse_mode_{MouseMode::absolute};
    Rect video_rect_{};
    InputPointerSnapshot pointer_;
    std::uint16_t host_usage_;
    double relative_gain_{1.0};
    double residual_x_{};
    double residual_y_{};
    double wheel_residual_{};
    std::uint8_t buttons_{};
    std::uint8_t activation_button_{};
    bool activation_released_{};
    bool video_fresh_{};
    bool release_requested_{};
    std::uint64_t sequence_{};
    std::unordered_set<std::uint16_t> physical_keys_;
    std::unordered_set<std::uint16_t> isolated_keys_;
    std::unordered_set<std::uint16_t> swallowed_host_releases_;
    std::vector<SpecialStep> special_steps_;
    std::vector<TextGesture> text_gestures_;
    std::size_t next_text_gesture_{};
    std::size_t scheduled_text_gestures_{};
    bool text_active_{};
    bool text_completion_pending_{};
    TextMappingResult text_result_;
};

}  // namespace kvmux
