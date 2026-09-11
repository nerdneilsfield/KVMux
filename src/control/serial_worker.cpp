#include "control/serial_worker.hpp"

#include "control/ch9329_protocol.hpp"
#include "control/hid_keyboard.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace kvmux {

SerialIo make_libserialport_io();

namespace {
using Clock = std::chrono::steady_clock;
constexpr auto kStallTimeout = std::chrono::milliseconds(100);
constexpr auto kHardTimeout = std::chrono::milliseconds(500);
constexpr auto kInfoInterval = std::chrono::seconds(1);
constexpr auto kReconnectInterval = std::chrono::milliseconds(500);
constexpr auto kHeartbeatTimeout = std::chrono::milliseconds(250);

std::uint16_t absolute_coordinate(double value) {
    return static_cast<std::uint16_t>(std::min(4095.0, std::floor(4096.0 * std::clamp(value, 0.0, 1.0))));
}

bool keyboard_and_mouse_mode(std::uint8_t mode) { return mode == 0x00U || mode == 0x80U; }
bool protocol_mode(std::uint8_t mode) { return mode == 0x00U || mode == 0x80U; }

}  // namespace

struct Ch9329ControlSink::Impl {
    enum class Purpose { info, config, keyboard, absolute, relative };
    enum class ClearStep { none, keyboard, absolute, relative };

    struct Transaction {
        ch9329::Frame frame;
        Purpose purpose{};
        std::vector<std::uint8_t> bytes;
        std::size_t written{};
        Clock::time_point started{};
        bool stalled{};
    };

    explicit Impl(SerialIo value) : io(std::move(value)), worker([this] { run(); }) {}

    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            queue.request_release();
        }
        wake.notify_one();
        if (worker.joinable()) { worker.join(); }
    }

    void update_snapshot(std::function<void(ControlSnapshot&)> fn) {
        std::lock_guard lock(mutex);
        fn(status);
    }

    void fail(std::string message, bool reconnect) {
        io.close();
        open = false;
        transaction.reset();
        parser.reset();
        clear_step = ClearStep::none;
        keyboard.clear();
        buttons = 0;
        relative_x = relative_y = wheel = 0.0;
        {
            std::lock_guard lock(mutex);
            queue.request_release();
            status.epoch = queue.epoch();
            status.state = reconnect ? ControlConnectionState::reconnecting : ControlConnectionState::fault;
            status.target_usb_ready = false;
            status.release_confirmed = false;
            status.error = std::move(message);
        }
        reconnect_at = Clock::now() + kReconnectInterval;
    }

    void begin(ch9329::Frame frame, Purpose purpose, Clock::time_point now) {
        Transaction next{std::move(frame), purpose, {}, 0, {}, false};
        next.bytes = ch9329::encode(next.frame);
        next.started = now;
        transaction = std::move(next);
    }

    void begin_info(Clock::time_point now) {
        begin({address, static_cast<std::uint8_t>(ch9329::Command::get_info), {}}, Purpose::info, now);
        last_info = now;
    }

    void begin_config(Clock::time_point now) {
        begin({address, static_cast<std::uint8_t>(ch9329::Command::get_config), {}}, Purpose::config, now);
    }

    void begin_clear(Clock::time_point now, bool both_mouse_modes = false) {
        // Both idle and in-flight ACK release paths must forget held input.
        keyboard.clear(); buttons = 0; relative_x = relative_y = wheel = 0.0;
        std::array<std::uint8_t, 6> empty{};
        clear_both_mouse_modes = both_mouse_modes;
        clear_step = ClearStep::keyboard;
        begin(ch9329::keyboard_report(address, 0, empty), Purpose::keyboard, now);
        update_snapshot([](auto& value) {
            value.state = ControlConnectionState::clearing;
            value.release_confirmed = false;
        });
    }

    void finish_clear_step(Clock::time_point now) {
        if (clear_step == ClearStep::keyboard) {
            if (clear_both_mouse_modes || mouse_mode == MouseMode::absolute) {
                clear_step = ClearStep::absolute;
                begin(ch9329::absolute_mouse_report(address, 0, absolute_x, absolute_y, 0), Purpose::absolute, now);
            } else {
                clear_step = ClearStep::relative;
                begin(ch9329::relative_mouse_report(address, 0, 0, 0, 0), Purpose::relative, now);
            }
        } else if (clear_step == ClearStep::absolute && clear_both_mouse_modes) {
            clear_step = ClearStep::relative;
            begin(ch9329::relative_mouse_report(address, 0, 0, 0, 0), Purpose::relative, now);
        } else {
            clear_step = ClearStep::none;
            if (disconnect_after_clear) {
                io.close();
                open = false;
                disconnect_after_clear = false;
                std::lock_guard lock(mutex);
                connect_requested = false;
                status.state = ControlConnectionState::disconnected;
                status.release_confirmed = true;
                status.target_usb_ready = false;
                status.error.clear();
            } else {
                std::lock_guard lock(mutex);
                queue.set_ready(true);
                status.state = ControlConnectionState::ready;
                status.release_confirmed = true;
                status.error.clear();
            }
        }
    }

    bool accept_ack(const ch9329::Frame& reply, Clock::time_point now) {
        if (!transaction || reply.address != address) { return false; }
        const auto request = transaction->frame.command;
        if (reply.command == static_cast<std::uint8_t>(request | 0xc0U)) {
            fail("CH9329 returned error status " +
                 std::to_string(reply.data.empty() ? -1 : reply.data.front()), true);
            return true;
        }
        if (reply.command != static_cast<std::uint8_t>(request | 0x80U)) { return false; }

        const auto purpose = transaction->purpose;
        if (purpose == Purpose::info) {
            if (reply.data.size() != 8U) {
                fail("Malformed GET_INFO response", true);
                return true;
            }
        } else if (purpose == Purpose::config) {
            if (reply.data.size() != 50U) {
                fail("Malformed GET_CONFIG response", true);
                return true;
            }
        } else if (reply.data.size() != 1U || reply.data.front() != 0U) {
            fail("CH9329 HID command failed", true);
            return true;
        }

        const bool was_stalled = transaction->stalled;
        const auto rtt = std::chrono::duration_cast<std::chrono::microseconds>(now - transaction->started);
        transaction.reset();
        update_snapshot([&](auto& value) { value.last_ack_rtt = rtt; });

        if (purpose == Purpose::info) {
            const bool usb_ready = reply.data[1] == 1U;
            update_snapshot([&](auto& value) {
                value.chip_version = reply.data[0];
                value.target_usb_ready = usb_ready;
                value.keyboard_leds = reply.data[2];
                // A routine successful probe must not revoke an active relay lease
                // through a transient clearing snapshot between mutex acquisitions.
                value.state = !usb_ready ? ControlConnectionState::monitoring :
                    (configuration_validated && !handshake_needs_clear ?
                        ControlConnectionState::ready : ControlConnectionState::clearing);
                if (usb_ready && configuration_validated && !handshake_needs_clear) queue.set_ready(true);
            });
            if (!usb_ready) {
                std::lock_guard lock(mutex);
                queue.request_release();
                status.epoch = queue.epoch();
                return true;
            }
            if (!configuration_validated) {
                begin_config(now);
            } else if (handshake_needs_clear) {
                handshake_needs_clear = false;
                begin_clear(now, true);
            }
        } else if (purpose == Purpose::config) {
            const bool valid = keyboard_and_mouse_mode(reply.data[0]) && protocol_mode(reply.data[1]);
            if (!valid) {
                fail("CH9329 must expose keyboard+mouse in binary protocol mode", false);
            } else {
                configuration_validated = true;
                handshake_needs_clear = false;
                begin_clear(now, true);
            }
        } else if (clear_step != ClearStep::none) {
            finish_clear_step(now);
        } else if (was_stalled || release_requested) {
            release_requested = false;
            begin_clear(now);
        }
        return true;
    }

    std::optional<std::pair<ch9329::Frame, Purpose>> event_frame(ControlEvent event) {
        if (const auto* edge = std::get_if<KeyEdge>(&event.payload)) {
            if (edge->pressed) { (void)keyboard.press(edge->usage); }
            else { keyboard.release(edge->usage); }
            return std::pair{ch9329::keyboard_report(address, keyboard.modifiers(), keyboard.keys()), Purpose::keyboard};
        }
        if (const auto* motion = std::get_if<AbsoluteMotion>(&event.payload)) {
            absolute_x = absolute_coordinate(motion->x);
            absolute_y = absolute_coordinate(motion->y);
            return std::pair{ch9329::absolute_mouse_report(address, buttons, absolute_x, absolute_y, 0), Purpose::absolute};
        }
        if (const auto* edge = std::get_if<ButtonEdge>(&event.payload)) {
            const std::uint8_t bit = edge->button < 3U ? static_cast<std::uint8_t>(1U << edge->button) : 0U;
            if (edge->pressed) { buttons = static_cast<std::uint8_t>(buttons | bit); }
            else { buttons = static_cast<std::uint8_t>(buttons & ~bit); }
            absolute_x = absolute_coordinate(edge->x);
            absolute_y = absolute_coordinate(edge->y);
            if (mouse_mode == MouseMode::absolute) {
                return std::pair{ch9329::absolute_mouse_report(address, buttons, absolute_x, absolute_y, 0), Purpose::absolute};
            }
            return std::pair{ch9329::relative_mouse_report(address, buttons, 0, 0, 0), Purpose::relative};
        }
        if (const auto* motion = std::get_if<RelativeMotion>(&event.payload)) {
            relative_x += motion->dx;
            relative_y += motion->dy;
        } else if (const auto* value = std::get_if<VerticalWheel>(&event.payload)) {
            wheel += value->steps;
            absolute_x = absolute_coordinate(value->x);
            absolute_y = absolute_coordinate(value->y);
        }
        const auto dx = std::clamp(static_cast<int>(std::trunc(relative_x)), -127, 127);
        const auto dy = std::clamp(static_cast<int>(std::trunc(relative_y)), -127, 127);
        const auto dw = std::clamp(static_cast<int>(std::trunc(wheel)), -127, 127);
        relative_x -= dx;
        relative_y -= dy;
        wheel -= dw;
        if (mouse_mode == MouseMode::absolute && dx == 0 && dy == 0) {
            return std::pair{ch9329::absolute_mouse_report(address, buttons, absolute_x, absolute_y,
                static_cast<std::int8_t>(dw)), Purpose::absolute};
        }
        return std::pair{ch9329::relative_mouse_report(address, buttons,
            static_cast<std::int8_t>(dx), static_cast<std::int8_t>(dy),
            static_cast<std::int8_t>(dw)), Purpose::relative};
    }

    void service(Clock::time_point now) {
        std::string selected_port;
        int selected_baud{};
        bool should_connect{};
        {
            std::lock_guard lock(mutex);
            should_connect = connect_requested;
            selected_port = port;
            selected_baud = baud;
            if (queue.take_release_request()) { release_requested = true; }
            if (control_active && now - heartbeat > kHeartbeatTimeout) {
                control_active = false;
                queue.request_release();
                status.epoch = queue.epoch();
                release_requested = true;
                status.state = ControlConnectionState::clearing;
                status.error = "UI heartbeat expired";
            }
        }

        if (!should_connect) {
            if (open) { io.close(); open = false; transaction.reset(); }
            return;
        }
        if (!open) {
            if (now < reconnect_at) { return; }
            if (!io.open(selected_port, selected_baud)) {
                reconnect_at = now + kReconnectInterval;
                update_snapshot([](auto& value) {
                    value.state = ControlConnectionState::reconnecting;
                    value.error = "Could not open selected serial port";
                });
                return;
            }
            open = true;
            configuration_validated = false;
            handshake_needs_clear = true;
            parser.reset();
            update_snapshot([](auto& value) {
                value.state = ControlConnectionState::monitoring;
                value.error.clear();
            });
            begin_info(now);
        }

        std::array<std::uint8_t, 128> input{};
        const auto count = io.read(input);
        if (count < 0) { fail("Serial read failed; release is unconfirmed", true); return; }
        if (count > 0) {
            const auto replies = parser.feed(std::span(input).first(static_cast<std::size_t>(count)));
            for (const auto& reply : replies) { if (accept_ack(reply, now)) { break; } }
        }

        if (transaction) {
            if (transaction->written < transaction->bytes.size()) {
                const auto remaining = std::span(transaction->bytes).subspan(transaction->written);
                const auto written = io.write(remaining);
                if (written < 0) { fail("Serial write failed; release is unconfirmed", true); return; }
                transaction->written += static_cast<std::size_t>(written);
            }
            const auto elapsed = now - transaction->started;
            if (elapsed >= kHardTimeout) {
                update_snapshot([](auto& value) { ++value.timeout_count; });
                fail("CH9329 transaction timed out; release is unconfirmed", true);
            } else if (elapsed >= kStallTimeout && !transaction->stalled) {
                transaction->stalled = true;
                std::lock_guard lock(mutex);
                queue.request_release();
                status.epoch = queue.epoch();
                status.state = ControlConnectionState::stalled;
                status.error = "CH9329 transaction stalled";
            }
            return;
        }

        if (release_requested) {
            release_requested = false;
            keyboard.clear(); buttons = 0; relative_x = relative_y = wheel = 0.0;
            begin_clear(now);
            return;
        }

        std::optional<ControlEvent> event;
        {
            std::lock_guard lock(mutex);
            event = queue.pop();
        }
        if (event) {
            if (auto frame = event_frame(std::move(*event))) { begin(std::move(frame->first), frame->second, now); }
        } else if (now - last_info >= kInfoInterval) {
            begin_info(now);
        }
    }

    void run() {
        while (true) {
            {
                std::unique_lock lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(1), [this] { return stopping.load(); });
                if (stopping.load()) {
                    queue.request_release();
                    release_requested = true;
                }
            }
            service(Clock::now());
            if (stopping.load()) {
                const auto deadline = Clock::now() + std::chrono::milliseconds(50);
                while (open && Clock::now() < deadline && (transaction || release_requested || clear_step != ClearStep::none)) {
                    service(Clock::now());
                    std::this_thread::yield();
                }
                io.close();
                return;
            }
        }
    }

    SerialIo io;
    mutable std::mutex mutex;
    std::condition_variable wake;
    ControlQueue queue;
    ControlSnapshot status;
    std::thread worker;
    ch9329::Parser parser;
    HidKeyboardState keyboard;
    std::optional<Transaction> transaction;
    std::string port;
    int baud{9600};
    std::uint8_t address{};
    MouseMode mouse_mode{MouseMode::absolute};
    std::uint8_t buttons{};
    std::uint16_t absolute_x{}, absolute_y{};
    double relative_x{}, relative_y{}, wheel{};
    ClearStep clear_step{ClearStep::none};
    bool clear_both_mouse_modes{};
    bool disconnect_after_clear{};
    Clock::time_point last_info{};
    Clock::time_point reconnect_at{};
    Clock::time_point heartbeat{Clock::now()};
    bool connect_requested{};
    bool open{};
    bool configuration_validated{};
    bool handshake_needs_clear{};
    bool release_requested{};
    bool control_active{};
    std::atomic_bool stopping{};
};

Ch9329ControlSink::Ch9329ControlSink() : Ch9329ControlSink(make_libserialport_io()) {}
Ch9329ControlSink::Ch9329ControlSink(SerialIo io) : impl_(std::make_unique<Impl>(std::move(io))) {}
Ch9329ControlSink::~Ch9329ControlSink() = default;

void Ch9329ControlSink::connect(std::string port, int baud_rate, std::uint8_t address) {
    std::lock_guard lock(impl_->mutex);
    impl_->port = std::move(port);
    impl_->baud = baud_rate;
    impl_->address = address == 0xffU ? 0U : address;
    impl_->connect_requested = true;
    impl_->reconnect_at = {};
    impl_->status.state = ControlConnectionState::opening;
    impl_->wake.notify_one();
}

void Ch9329ControlSink::disconnect() noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->control_active = false;
    impl_->queue.request_release();
    impl_->status.epoch = impl_->queue.epoch();
    if (impl_->open) {
        impl_->disconnect_after_clear = true;
        impl_->status.state = ControlConnectionState::stopping;
    } else {
        impl_->connect_requested = false;
        impl_->status.state = ControlConnectionState::disconnected;
    }
    impl_->wake.notify_one();
}

void Ch9329ControlSink::set_mouse_mode(MouseMode mode) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->mouse_mode != mode) {
        impl_->queue.request_release();
        impl_->status.epoch = impl_->queue.epoch();
        impl_->status.release_confirmed = false;
        if (impl_->status.state == ControlConnectionState::ready)
            impl_->status.state = ControlConnectionState::clearing;
        impl_->mouse_mode = mode;
    }
    impl_->wake.notify_one();
}

void Ch9329ControlSink::set_control_active(bool active) noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->control_active = active;
    impl_->heartbeat = Clock::now();
    impl_->wake.notify_one();
}

void Ch9329ControlSink::update_ui_heartbeat() noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->heartbeat = Clock::now();
}

SubmitResult Ch9329ControlSink::submit(ControlEvent event) {
    std::lock_guard lock(impl_->mutex);
    const auto result = impl_->queue.submit(std::move(event), Clock::now());
    if (result != SubmitResult::accepted) { ++impl_->status.rejected_events; }
    impl_->status.epoch = impl_->queue.epoch();
    impl_->wake.notify_one();
    return result;
}

void Ch9329ControlSink::release_all() noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->control_active = false;
    impl_->queue.request_release();
    impl_->status.epoch = impl_->queue.epoch();
    impl_->status.release_confirmed = false;
    if (impl_->status.state == ControlConnectionState::ready)
        impl_->status.state = ControlConnectionState::clearing;
    impl_->wake.notify_one();
}

ControlSnapshot Ch9329ControlSink::snapshot() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->status;
}

}  // namespace kvmux
