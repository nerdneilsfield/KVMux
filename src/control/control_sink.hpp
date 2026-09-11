#pragma once

#include "control/control_event.hpp"

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
    std::string error;
};

class ControlSink {
public:
    virtual ~ControlSink() = default;
    virtual void connect(std::string, int, std::uint8_t = 0) {}
    virtual void disconnect() noexcept {}
    virtual void set_mouse_mode(MouseMode) {}
    virtual void set_control_active(bool) noexcept {}
    virtual void update_ui_heartbeat() noexcept {}
    virtual void video_presented(std::uint64_t) noexcept {}
    [[nodiscard]] virtual SubmitResult submit(ControlEvent event) = 0;
    virtual void release_all() noexcept = 0;
    [[nodiscard]] virtual ControlSnapshot snapshot() const = 0;
};

[[nodiscard]] std::vector<SerialPortInfo> enumerate_serial_ports();

}  // namespace kvmux
