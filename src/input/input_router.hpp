#pragma once

#include "control/control_sink.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <unordered_set>
#include <variant>
#include <vector>

namespace kvmux {

enum class InputState { preview, arming, captured, releasing, fault };
enum class InputMouseButton : std::uint8_t { left = 1, right = 2, middle = 3 };
enum class SpecialKeys { control_alt_delete, alt_tab, host_key };

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
[[nodiscard]] bool supported_usb_keyboard_usage(std::uint16_t usage) noexcept;

class InputRouter {
public:
    using Clock = std::chrono::steady_clock;

    explicit InputRouter(ControlSink& sink, std::uint16_t host_usage = 0xe4);

    [[nodiscard]] InputState state() const noexcept { return state_; }
    [[nodiscard]] MouseMode mouse_mode() const noexcept { return mouse_mode_; }
    [[nodiscard]] bool captured() const noexcept { return state_ == InputState::captured; }

    [[nodiscard]] bool special_active() const noexcept { return !special_steps_.empty(); }

    void set_video_rect(Rect rect) noexcept { video_rect_ = rect; }
    void set_video_fresh(bool fresh) noexcept { video_fresh_ = fresh; }
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
    void send_relative_integral(int dx, int dy, int wheel);
    [[nodiscard]] std::pair<std::uint16_t, std::uint16_t> absolute(double x,
                                                                  double y) const noexcept;

    ControlSink& sink_;
    InputState state_{InputState::preview};
    MouseMode mouse_mode_{MouseMode::absolute};
    Rect video_rect_{};
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
};

}  // namespace kvmux
