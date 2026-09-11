#include "input/input_router.hpp"
#include "control/ch9329_protocol.hpp"
#include "control/hid_keyboard.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

void require(bool value, const char* message) {
    if (!value) { throw std::runtime_error(message); }
}

class FakeSink final : public kvmux::ControlSink {
public:
    kvmux::SubmitResult submit(kvmux::ControlEvent event) override {
        events.push_back(std::move(event));
        return result;
    }
    void release_all() noexcept override { ++releases; snapshot_value.release_confirmed = false; }
    kvmux::ControlSnapshot snapshot() const override { return snapshot_value; }

    kvmux::ControlSnapshot snapshot_value = [] {
        kvmux::ControlSnapshot value;
        value.state = kvmux::ControlConnectionState::ready;
        value.epoch = 7;
        value.target_usb_ready = true;
        value.release_confirmed = true;
        return value;
    }();
    kvmux::SubmitResult result{kvmux::SubmitResult::accepted};
    std::vector<kvmux::ControlEvent> events;
    int releases{};
};

void capture(kvmux::InputRouter& router, FakeSink& sink, double x = 50, double y = 50) {
    router.handle({kvmux::InputButton{kvmux::InputMouseButton::left, true, x, y}});
    require(router.state() == kvmux::InputState::arming, "activation starts arming");
    require(sink.events.empty(), "activation down is swallowed");
    router.handle({kvmux::InputButton{kvmux::InputMouseButton::left, false, x, y}});
    router.tick();
    require(router.captured(), "activation release captures");
    require(sink.events.empty(), "activation up is swallowed");
}

}  // namespace

int main() {
    using namespace kvmux;

    {
        FakeSink sink;
        InputRouter router(sink);
        router.set_video_rect({0, 0, 200, 200});
        const auto click = [&] {
            router.handle({InputButton{InputMouseButton::left, true, 50, 50}});
            router.handle({InputButton{InputMouseButton::left, false, 50, 50}});
            router.tick();
            require(router.state() == InputState::preview && sink.events.empty(),
                    "unavailable control/video cannot arm or defer activation");
        };
        click(); // Stale video, including a texture retained after capture failure.
        router.set_video_fresh(true);
        sink.snapshot_value.state = ControlConnectionState::disconnected;
        click();
        sink.snapshot_value.state = ControlConnectionState::ready;
        sink.snapshot_value.target_usb_ready = false;
        click();
        sink.snapshot_value.target_usb_ready = true;
        sink.snapshot_value.release_confirmed = false;
        click();
        sink.snapshot_value.release_confirmed = true;
        router.tick();
        require(router.state() == InputState::preview, "recovery needs a fresh click");
        capture(router, sink);
    }

    {
        FakeSink sink;
        InputRouter router(sink);
        router.set_video_rect({0, 0, 200, 200});
        router.set_video_fresh(true);
        capture(router, sink);

        router.handle({InputKey{0xe4, true, false}});
        require(router.state() == InputState::releasing && sink.events.empty() &&
                    sink.releases == 1, "Host key releases without leaking");
        router.handle({InputKey{0xe4, false, false}});
        sink.snapshot_value.release_confirmed = true;
        router.tick();
        require(router.state() == InputState::preview, "confirmed release returns preview");
    }

    {
        FakeSink sink;
        InputRouter router(sink);
        router.set_video_rect({0, 0, 200, 200});
        router.set_video_fresh(true);
        router.handle({InputKey{0xe0, true, false}});
        capture(router, sink);
        router.handle({InputKey{0xe0, true, false}});
        router.handle({InputKey{0xe0, false, false}});
        require(sink.events.empty(), "pre-held key is isolated through release");
        router.handle({InputKey{0xe0, true, false}});
        router.handle({InputKey{0xe0, false, false}});
        require(sink.events.size() == 2 && std::get<KeyEdge>(sink.events[0].payload).pressed &&
                    !std::get<KeyEdge>(sink.events[1].payload).pressed,
                "new press after isolation keeps quick edges");
    }

    {
        FakeSink sink;
        // Target Caps Lock LED is feedback, not a request to press Caps Lock.
        sink.snapshot_value.keyboard_leds = 0x02;
        InputRouter router(sink);
        router.set_video_rect({0, 0, 200, 200});
        router.set_video_fresh(true);
        router.handle({InputKey{0x39, true, false}});
        capture(router, sink);
        router.handle({InputKey{0x39, true, true}});
        router.handle({InputKey{0x39, false, false}});
        require(sink.events.empty(), "pre-held Caps Lock and activation send no keyboard input");
        router.handle({InputKey{0xe1, true, false}});
        router.handle({InputKey{0x04, true, false}});
        HidKeyboardState keyboard;
        for (const auto& event : sink.events) {
            const auto edge = std::get<KeyEdge>(event.payload);
            require(edge.usage != 0x39, "Shift and A do not synthesize Caps Lock");
            require(keyboard.press(edge.usage), "keyboard edge admitted");
        }
        auto report = ch9329::keyboard_report(0, keyboard.modifiers(), keyboard.keys());
        require(report.data == std::vector<std::uint8_t>{0x02, 0, 0x04, 0, 0, 0, 0, 0},
                "Shift is modifier bit, not Caps Lock usage");
        keyboard.clear();
        report = ch9329::keyboard_report(0, keyboard.modifiers(), keyboard.keys());
        require(report.data == std::vector<std::uint8_t>(8, 0),
                "clear report releases modifiers and keys without toggling Caps Lock");
        require(keyboard.press(0x39), "Caps Lock is an ordinary HID usage");
        report = ch9329::keyboard_report(0, keyboard.modifiers(), keyboard.keys());
        require(report.data == std::vector<std::uint8_t>{0, 0, 0x39, 0, 0, 0, 0, 0},
                "Caps Lock occupies a key slot, not the modifier or reserved byte");
        router.handle({InputKey{0x39, true, false}});
        router.handle({InputKey{0x39, false, false}});
        require(sink.events.size() == 4 &&
                    std::get<KeyEdge>(sink.events[2].payload).usage == 0x39 &&
                    std::get<KeyEdge>(sink.events[2].payload).pressed &&
                    !std::get<KeyEdge>(sink.events[3].payload).pressed,
                "only a fresh Caps Lock press forwards its edges");
    }

    {
        const auto wide = fit_video_rect({0, 0, 1000, 1000}, 1920, 1080);
        const auto classic = fit_video_rect({0, 0, 1200, 600}, 640, 480);
        const auto ultra = fit_video_rect({0, 0, 1600, 600}, 2560, 1080);
        require(std::abs(wide.width - 1000.0) < 1e-9 &&
                    std::abs(wide.y - 218.75) < 1e-9 &&
                    std::abs(wide.height - 562.5) < 1e-9,
                "16:9 letterbox");
        require(classic.width == 800 && classic.x == 200, "4:3 pillarbox");
        require(ultra.width > 1400 && ultra.height == 600, "ultrawide fit");

        FakeSink sink;
        InputRouter router(sink);
        router.set_video_rect(wide);
        router.set_video_fresh(true);
        router.handle({InputButton{InputMouseButton::left, true, 500, 10}});
        require(router.state() == InputState::preview, "black-bar click rejected");
        capture(router, sink, wide.x, wide.y);
        router.handle({InputPointerMotion{wide.x, wide.y}});
        router.handle({InputPointerMotion{wide.x + wide.width, wide.y + wide.height}});
        const auto first = std::get<AbsoluteMotion>(sink.events[0].payload);
        const auto last = std::get<AbsoluteMotion>(sink.events[1].payload);
        require(first.x == 0 && first.y == 0 && last.x == 1 && last.y == 1,
                "absolute corners cover full range");
        router.handle({InputButton{InputMouseButton::left, true, 500, 500}});
        router.handle({InputButton{InputMouseButton::left, false, 2000, -10}});
        const auto release = std::get<ButtonEdge>(sink.events.back().payload);
        require(!release.pressed && release.x == 1 && release.y == 0,
                "drag release outside is sent and clipped");
    }

    {
        FakeSink sink;
        InputRouter router(sink);
        router.set_video_rect({0, 0, 100, 100});
        router.set_video_fresh(true);
        router.set_mouse_mode(MouseMode::relative);
        capture(router, sink);
        router.handle({InputRelativeMotion{0.4, -0.4}});
        router.handle({InputRelativeMotion{300.8, -260.8}});
        int x = 0, y = 0;
        for (const auto& event : sink.events) {
            const auto motion = std::get<RelativeMotion>(event.payload);
            require(motion.dx >= -127 && motion.dx <= 127 &&
                        motion.dy >= -127 && motion.dy <= 127,
                    "relative reports split to signed-byte range");
            x += static_cast<int>(motion.dx);
            y += static_cast<int>(motion.dy);
        }
        require(x == 301 && y == -261, "fractional and split relative totals conserved");

        router.release();
        sink.snapshot_value.release_confirmed = true;
        router.tick();
        router.set_mouse_mode(MouseMode::absolute);
        router.set_mouse_mode(MouseMode::relative);
        sink.events.clear();
        capture(router, sink);
        router.handle({InputRelativeMotion{0.6, 0.6}});
        require(sink.events.empty(), "mode switch discards old fractional motion");
    }

    {
        // SDL and ImGui use logical window coordinates. The GL framebuffer may
        // be 1x, 1.5x or 2x larger; it must not scale only one side of the map.
        // A complete desktop scaled into MJPEG needs no target-resolution knob.
        for (const auto source : {std::pair{1920, 1080}, std::pair{1280, 720},
                                  std::pair{640, 480}, std::pair{2560, 1080}}) {
            for (const double dpi : {1.0, 1.5, 2.0}) {
                const Rect area{31.25, 87.5, 1000, 700};
                const auto logical = fit_video_rect(area, source.first, source.second);
                const auto pixels = fit_video_rect(
                    {area.x * dpi, area.y * dpi, area.width * dpi, area.height * dpi},
                    source.first, source.second);
                for (const bool framebuffer_coordinates : {false, true}) {
                    FakeSink sink;
                    InputRouter router(sink);
                    const Rect rect = framebuffer_coordinates ? pixels : logical;
                    router.set_video_rect(rect);
                    router.set_video_fresh(true);
                    capture(router, sink, rect.x, rect.y);
                    // Official WCH section 2.2.4: (100,100) on 1280x768
                    // maps to (320,533), using width/height, NOT width-1.
                    const double u = 100.0 / 1280.0;
                    const double v = 100.0 / 768.0;
                    const double x = rect.x + rect.width * u;
                    const double y = rect.y + rect.height * v;
                    router.handle({InputPointerMotion{x, y}});
                    const auto motion = std::get<AbsoluteMotion>(sink.events.back().payload);
                    require(std::lround(motion.x * 4095) == 320 &&
                                std::lround(motion.y * 4095) == 533,
                            "scaled video, letterbox and HiDPI preserve WCH coordinates");
                    router.handle({InputButton{InputMouseButton::left, true, x, y}});
                    const auto button = std::get<ButtonEdge>(sink.events.back().payload);
                    router.handle({InputWheel{1, x, y}});
                    const auto wheel = std::get<VerticalWheel>(sink.events.back().payload);
                    require(button.x == motion.x && button.y == motion.y &&
                                wheel.x == motion.x && wheel.y == motion.y,
                            "motion, button and wheel share the same absolute mapping");
                }
            }
        }
    }

    {
        FakeSink sink;
        InputRouter router(sink);
        router.set_video_rect({20, 30, 200, 100});
        router.set_video_fresh(true);
        capture(router, sink, 50, 50);
        require(!router.pointer_snapshot().submitted_absolute, "activation sends no coordinates");
        router.handle({InputPointerMotion{70, 80}});
        auto pointer = router.pointer_snapshot();
        require(pointer.video_local == std::pair{50.0, 50.0} &&
                    pointer.submitted_absolute == std::pair<std::uint16_t, std::uint16_t>{1024, 2048},
                "diagnostics show video-local units and accepted HID coordinates");
        router.handle({InputButton{InputMouseButton::left, true, 120, 55}});
        require(router.pointer_snapshot().submitted_absolute ==
                    std::pair<std::uint16_t, std::uint16_t>{2048, 1024},
                "button updates submitted coordinates");
        sink.result = SubmitResult::overloaded;
        router.handle({InputPointerMotion{220, 130}});
        pointer = router.pointer_snapshot();
        require(!pointer.video_local && !pointer.submitted_absolute && !pointer.submitted_relative,
                "rejected submission clears diagnostics instead of reporting predicted coordinates");
    }
    {
        FakeSink sink;
        InputRouter router(sink);
        router.set_video_rect({0, 0, 200, 200});
        router.set_video_fresh(true);
        router.set_mouse_mode(MouseMode::relative);
        capture(router, sink);
        router.handle({InputRelativeMotion{0.4, 0.4}});
        require(!router.pointer_snapshot().submitted_relative, "fractional unsent delta stays absent");
        router.handle({InputRelativeMotion{2.0, -3.0}});
        require(router.pointer_snapshot().submitted_relative == std::pair{2, -2} &&
                    !router.pointer_snapshot().submitted_absolute,
                "relative diagnostics report accepted integral delta, not absolute coordinates");
        router.set_video_fresh(false);
        require(!router.pointer_snapshot().submitted_relative, "stale video clears diagnostics");
        router.set_video_fresh(true);
        router.handle({InputRelativeMotion{1, 1}});
        router.release();
        require(!router.pointer_snapshot().submitted_relative, "release clears diagnostics");
    }

    return EXIT_SUCCESS;
}
