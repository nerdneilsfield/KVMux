#pragma once

#include "control/ascii_paste_job.hpp"
#include "control/control_event.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kvmux {

enum class ControlConnectionState {
    disconnected,
    opening,
    monitoring,
    clearing,
    ready,
    stalled,
    reconnecting,
    fault,
    stopping,
};

enum class MouseMode { absolute, relative };

struct DesiredInputState {
    std::uint8_t modifiers{};
    std::array<std::uint8_t, 6> keys{};
    std::uint8_t buttons{};
    MouseMode mode{MouseMode::absolute};
    std::uint16_t absolute_x{}, absolute_y{};
};

struct InputSync {
    std::uint64_t epoch{}, intent_generation{}, revision{};
    DesiredInputState state;
};

struct AppliedInputState {
    bool known{};
    std::uint64_t epoch{}, intent_generation{}, revision{};
    DesiredInputState state;
};

struct SerialPortInfo {
    std::string name;
    std::string description;
    std::optional<std::uint16_t> usb_vendor_id;
    std::optional<std::uint16_t> usb_product_id;
};

struct ControlSnapshot {
    ControlConnectionState state{ControlConnectionState::disconnected};
    std::uint64_t epoch{1};
    bool target_usb_ready{};
    bool release_confirmed{};
    std::uint8_t chip_version{};
    std::uint8_t keyboard_leds{};
    std::chrono::microseconds last_ack_rtt{};
    std::uint64_t timeout_count{};
    std::uint64_t rejected_events{};
    // Ordinary input remains pending until the serial HID report is ACKed.
    bool ordinary_input_pending{};
    std::uint64_t completed_ordinary_sequence{};
    std::string error;
    AppliedInputState applied;
    // Network-only interruption can suspend execution without revoking UI intent.
    bool recoverable_transport{};
};

class ControlSink {
public:
    virtual ~ControlSink() = default;
    virtual void connect(std::string, int, std::uint8_t = 0) {}
    virtual void disconnect() noexcept {}
    virtual void set_mouse_mode(MouseMode) {}
    virtual void set_control_active(bool) noexcept {}
    virtual void update_ui_heartbeat() noexcept {}
    virtual void video_presented(std::uint64_t generation, std::uint64_t sequence) noexcept { (void)generation; (void)sequence; }
    [[nodiscard]] virtual SubmitResult submit(ControlEvent event) = 0;
    [[nodiscard]] virtual SubmitResult synchronize(InputSync) { return SubmitResult::not_ready; }
    // Starts one fully mapped HID-only job. Unsupported sinks return not_ready.
    [[nodiscard]] virtual SubmitResult start_ascii_paste(AsciiPasteJob) { return SubmitResult::not_ready; }
    // Bounded relay path. Admission only queues an immutable request; the serial
    // worker validates readiness and ownership before it starts HID reports.
    [[nodiscard]] virtual SubmitResult prepare_ascii_paste(AsciiPasteRequest) { return SubmitResult::not_ready; }
    // Remote sinks receive normalized transaction bytes; local sinks receive HID gestures.
    [[nodiscard]] virtual SubmitResult start_ascii_paste_text(std::vector<std::uint8_t>) { return SubmitResult::not_ready; }
    virtual void cancel_ascii_paste() noexcept {}
    [[nodiscard]] virtual AsciiPasteSnapshot ascii_paste_snapshot() const { return {}; }
    virtual void release_all() noexcept = 0;
    [[nodiscard]] virtual ControlSnapshot snapshot() const = 0;
};

[[nodiscard]] std::vector<SerialPortInfo> enumerate_serial_ports();

}  // namespace kvmux
