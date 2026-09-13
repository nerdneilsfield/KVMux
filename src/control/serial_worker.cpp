#include "control/serial_worker.hpp"

#include "control/ch9329_protocol.hpp"
#include "control/hid_keyboard.hpp"

#include <spdlog/spdlog.h>

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
    enum class Purpose { info, config, keyboard, absolute, relative, sync_keyboard, sync_mouse };
    enum class ClearStep { none, keyboard, absolute, relative };

    struct Transaction {
        ch9329::Frame frame;
        Purpose purpose{};
        std::vector<std::uint8_t> bytes;
        std::size_t written{};
        Clock::time_point started{};
        bool stalled{};
        std::uint64_t source_sequence{};
        std::uint64_t epoch{};
        const char* kind{};
        bool ordinary{};
        bool ascii_paste{};
    };

    explicit Impl(SerialIo value) : io(std::move(value)) {
        // run() uses members declared after worker; start only after initialization.
        worker = std::thread([this] { run(); });
    }

    // Caller holds mutex. A report already written must still drain its ACK.
    void cancel_paste_locked(AsciiPasteState state) {
        if (paste.state == AsciiPasteState::active) { paste.state = state; }
        paste_job.reset();
        paste_gesture = paste_edge = 0;
    }

    // Caller holds mutex. A canceled in-flight report still drains its ACK,
    // but its snapshot can never be published or continue with a held mouse.
    void invalidate_sync() {
        status.applied.known = false;
        input_sync.reset();
    }

    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            invalidate_sync();
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
        if (transaction) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - transaction->started).count();
            spdlog::debug("CH9329 transaction failure: kind={} source_sequence={} epoch={} "
                          "elapsed_ms={} command=0x{:02x} written={} total={} reason={} reconnect={}",
                          transaction->kind, transaction->source_sequence, transaction->epoch, elapsed,
                          transaction->frame.command, transaction->written, transaction->bytes.size(),
                          message, reconnect);
        } else {
            spdlog::debug("CH9329 failure: reason={} reconnect={}", message, reconnect);
        }
        io.close();
        open = false;
        transaction.reset();
        parser.reset();
        clear_step = ClearStep::none;
        {
            std::lock_guard lock(mutex);
            cancel_paste_locked(AsciiPasteState::failed);
            keyboard.clear();
            buttons = 0;
            relative_x = relative_y = wheel = 0.0;
            ordinary_inflight = false;
            ordinary_sequence = 0;
            status.ordinary_input_pending = false;
            sync_inflight = false;
            invalidate_sync();
            queue.request_release();
            status.epoch = queue.epoch();
            status.state = reconnect ? ControlConnectionState::reconnecting : ControlConnectionState::fault;
            status.target_usb_ready = false;
            status.release_confirmed = false;
            status.error = std::move(message);
        }
        reconnect_at = Clock::now() + kReconnectInterval;
    }

    void begin(ch9329::Frame frame, Purpose purpose, Clock::time_point now,
               std::uint64_t source_sequence = 0, std::uint64_t epoch = 0,
               bool ordinary = false, bool ascii_paste = false) {
        const char* kind = purpose == Purpose::info ? "info" :
                           purpose == Purpose::config ? "config" :
                           (purpose == Purpose::sync_keyboard || purpose == Purpose::sync_mouse) ? "sync" :
                           clear_step != ClearStep::none ? "clear" : "keyboard";
        Transaction next{std::move(frame), purpose, {}, 0, {}, false, source_sequence, epoch, kind, ordinary, ascii_paste};
        next.bytes = ch9329::encode(next.frame);
        next.started = now;
        if (purpose == Purpose::info || purpose == Purpose::config) {
            spdlog::debug("CH9329 {} request: address=0x{:02x} command=0x{:02x}",
                          purpose == Purpose::info ? "GET_INFO" : "GET_CONFIG",
                          next.frame.address, next.frame.command);
        } else if (ordinary) {
            // Do not log HID report bytes: they can reveal simulated typed text.
            spdlog::debug("CH9329 keyboard transaction begin: source_sequence={} epoch={}",
                          source_sequence, epoch);
        }
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
        {
            std::lock_guard lock(mutex);
            invalidate_sync();
            cancel_paste_locked(AsciiPasteState::canceled);
            ordinary_inflight = false;
            ordinary_sequence = 0;
            status.ordinary_input_pending = false;
            keyboard.clear(); buttons = 0; relative_x = relative_y = wheel = 0.0;
            status.state = ControlConnectionState::clearing;
            status.release_confirmed = false;
        }
        std::array<std::uint8_t, 6> empty{};
        clear_both_mouse_modes = both_mouse_modes;
        clear_step = ClearStep::keyboard;
        begin(ch9329::keyboard_report(address, 0, empty), Purpose::keyboard, now);
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
                status.ordinary_input_pending = false;
                status.error.clear();
            }
        }
    }

    bool accept_ack(const ch9329::Frame& reply, Clock::time_point now) {
        if (!transaction || reply.address != address) { return false; }
        const auto request = transaction->frame.command;
        const bool diagnostic = transaction->purpose == Purpose::info || transaction->purpose == Purpose::config;
        const bool error_ack = reply.command == static_cast<std::uint8_t>(request | 0xc0U);
        if (diagnostic || error_ack) {
            spdlog::debug("CH9329 ACK: request=0x{:02x} reply=0x{:02x} type={} response_length={}",
                          request, reply.command, error_ack ? "error" :
                          reply.command == static_cast<std::uint8_t>(request | 0x80U) ? "normal" : "unexpected",
                          reply.data.size());
        }
        if (error_ack) {
            spdlog::debug("CH9329 error ACK: command=0x{:02x} status={}", request,
                          reply.data.empty() ? -1 : static_cast<int>(reply.data.front()));
        }
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
            spdlog::debug("CH9329 HID ACK failed: command=0x{:02x} response_length={} status={}",
                          request, reply.data.size(), reply.data.empty() ? -1 : static_cast<int>(reply.data.front()));
            fail("CH9329 HID command failed", true);
            return true;
        }

        const bool was_stalled = transaction->stalled;
        const bool was_ascii_paste = transaction->ascii_paste;
        if (transaction->ordinary) {
            spdlog::debug("CH9329 keyboard ACK: source_sequence={} epoch={}",
                          transaction->source_sequence, transaction->epoch);
        }
        const auto rtt = std::chrono::duration_cast<std::chrono::microseconds>(now - transaction->started);
        transaction.reset();
        update_snapshot([&](auto& value) {
            value.last_ack_rtt = rtt;
            if (ordinary_inflight) {
                status.completed_ordinary_sequence = ordinary_sequence;
                ordinary_sequence = 0;
            }
            ordinary_inflight = false;
            status.ordinary_input_pending = queue.size() != 0;
        });

        if (was_ascii_paste) {
            std::lock_guard lock(mutex);
            if (paste.state == AsciiPasteState::active && !was_stalled && !release_requested) {
                ++paste_edge;
                if (paste_job && paste_edge == paste_job->gestures[paste_gesture].size()) {
                    paste_edge = 0;
                    ++paste_gesture;
                    ++paste.completed_gestures;
                    if (paste_gesture == paste.total_gestures) {
                        paste.state = AsciiPasteState::completed;
                        paste_job.reset();
                    }
                }
            }
        }

        if (purpose == Purpose::info) {
            const bool usb_ready = reply.data[1] == 1U;
            spdlog::debug("CH9329 GET_INFO: response_length={} chip_version=0x{:02x} usb_status={} usb_ready={}",
                          reply.data.size(), reply.data[0], reply.data[1], usb_ready);
            if (!last_keyboard_leds || *last_keyboard_leds != reply.data[2]) {
                spdlog::debug("CH9329 target keyboard LEDs: bits=0x{:02x} caps_lock={} num_lock={} scroll_lock={}",
                              reply.data[2], (reply.data[2] & 2U) != 0,
                              (reply.data[2] & 1U) != 0, (reply.data[2] & 4U) != 0);
                last_keyboard_leds = reply.data[2];
            }
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
                invalidate_sync();
                cancel_paste_locked(AsciiPasteState::canceled);
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
            spdlog::debug("CH9329 GET_CONFIG: response_length={} chip_mode=0x{:02x} protocol_mode=0x{:02x} valid={}",
                          reply.data.size(), reply.data[0], reply.data[1], valid);
            if (!valid) {
                fail("CH9329 must expose keyboard+mouse in binary protocol mode", false);
            } else {
                configuration_validated = true;
                handshake_needs_clear = false;
                begin_clear(now, true);
            }
        } else if (purpose == Purpose::sync_keyboard || purpose == Purpose::sync_mouse) {
            bool canceled{};
            {
                std::lock_guard lock(mutex);
                canceled = !input_sync || was_stalled || release_requested ||
                    queue.release_pending() || input_sync->epoch != status.epoch ||
                    !control_active || now - heartbeat > kHeartbeatTimeout;
                if (canceled) {
                    invalidate_sync();
                    (void)queue.take_release_request();
                    sync_inflight = false;
                } else if (purpose == Purpose::sync_keyboard) {
                    const auto& state = input_sync->state;
                    begin(state.mode == MouseMode::absolute ?
                        ch9329::absolute_mouse_report(address, state.buttons,
                            state.absolute_x, state.absolute_y, 0) :
                        ch9329::relative_mouse_report(address, state.buttons, 0, 0, 0),
                        Purpose::sync_mouse, now);
                } else {
                    const auto& sync = *input_sync;
                    keyboard.restore(sync.state.modifiers, sync.state.keys);
                    buttons = sync.state.buttons;
                    mouse_mode = sync.state.mode;
                    absolute_x = sync.state.absolute_x;
                    absolute_y = sync.state.absolute_y;
                    status.applied = {true, sync.epoch, sync.intent_generation,
                                      sync.revision, sync.state};
                    input_sync.reset();
                    sync_inflight = false;
                }
            }
            if (canceled) {
                release_requested = false;
                begin_clear(now);
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
                invalidate_sync();
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
            spdlog::debug("CH9329 opening: path={} baud={} address=0x{:02x}", selected_port, selected_baud, address);
            if (!io.open(selected_port, selected_baud)) {
                spdlog::debug("CH9329 open failed: path={} baud={}", selected_port, selected_baud);
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

        if (transaction && (transaction->purpose == Purpose::sync_keyboard ||
                            transaction->purpose == Purpose::sync_mouse) && transaction->written == 0) {
            bool canceled{};
            {
                std::lock_guard lock(mutex);
                canceled = !input_sync || queue.release_pending() ||
                    input_sync->epoch != status.epoch || !control_active ||
                    now - heartbeat > kHeartbeatTimeout;
                if (canceled) {
                    invalidate_sync();
                    sync_inflight = false;
                    (void)queue.take_release_request();
                }
            }
            if (canceled) {
                transaction.reset();
                release_requested = false;
                begin_clear(now);
            }
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
                spdlog::debug("CH9329 transaction timeout: kind={} source_sequence={} epoch={} "
                              "command=0x{:02x} elapsed_ms={} written={} total={}",
                              transaction->kind, transaction->source_sequence, transaction->epoch,
                              transaction->frame.command,
                              std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
                              transaction->written, transaction->bytes.size());
                update_snapshot([](auto& value) { ++value.timeout_count; });
                fail("CH9329 transaction timed out; release is unconfirmed", true);
            } else if (elapsed >= kStallTimeout && !transaction->stalled) {
                spdlog::debug("CH9329 transaction stalled: kind={} source_sequence={} epoch={} "
                              "command=0x{:02x} elapsed_ms={} written={} total={}",
                              transaction->kind, transaction->source_sequence, transaction->epoch,
                              transaction->frame.command,
                              std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
                              transaction->written, transaction->bytes.size());
                transaction->stalled = true;
                std::lock_guard lock(mutex);
                invalidate_sync();
                queue.request_release();
                status.epoch = queue.epoch();
                status.state = ControlConnectionState::stalled;
                status.error = "CH9329 transaction stalled";
            }
            return;
        }

        if (release_requested) {
            release_requested = false;
            begin_clear(now);
            return;
        }

        {
            std::lock_guard lock(mutex);
            if (input_sync) {
                if (!sync_started) {
                    sync_started = true;
                    sync_inflight = true;
                    // Cancellation must clear the mode this immutable sync will use.
                    mouse_mode = input_sync->state.mode;
                    begin(ch9329::keyboard_report(address, input_sync->state.modifiers,
                        input_sync->state.keys), Purpose::sync_keyboard, now);
                }
                return;
            }
            if (paste.state == AsciiPasteState::active && paste_job) {
                const auto& edges = paste_job->gestures[paste_gesture];
                const auto& edge = edges[paste_edge];
                if (edge.pressed) { (void)keyboard.press(edge.usage); }
                else { keyboard.release(edge.usage); }
                begin(ch9329::keyboard_report(address, keyboard.modifiers(), keyboard.keys()),
                      Purpose::keyboard, now, 0, status.epoch, false, true);
                return;
            }
            if (auto event = queue.pop()) {
                ordinary_inflight = true;
                ordinary_sequence = event->sequence;
                status.ordinary_input_pending = true;
                if (auto frame = event_frame(std::move(*event))) {
                    begin(std::move(frame->first), frame->second, now, event->sequence, event->epoch, true);
                }
                return;
            }
        }
        if (now - last_info >= kInfoInterval) { begin_info(now); }
    }

    void run() {
        while (true) {
            {
                std::unique_lock lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(1), [this] { return stopping.load(); });
                if (stopping.load()) {
                    invalidate_sync();
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
    std::optional<std::uint8_t> last_keyboard_leds;
    std::thread worker;
    ch9329::Parser parser;
    HidKeyboardState keyboard;
    std::optional<Transaction> transaction;
    // Shared with admission under mutex; transaction itself is worker-owned.
    std::optional<InputSync> input_sync;
    bool sync_started{};
    bool sync_inflight{};
    bool ordinary_inflight{};
    std::uint64_t ordinary_sequence{};
    AsciiPasteSnapshot paste;
    std::optional<AsciiPasteJob> paste_job;
    std::size_t paste_gesture{};
    std::size_t paste_edge{};
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
    impl_->invalidate_sync();
    impl_->status.state = ControlConnectionState::opening;
    impl_->wake.notify_one();
}

void Ch9329ControlSink::disconnect() noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->control_active = false;
    impl_->invalidate_sync();
    impl_->cancel_paste_locked(AsciiPasteState::canceled);
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
        impl_->invalidate_sync();
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
    if (!active) impl_->invalidate_sync();
    impl_->heartbeat = Clock::now();
    impl_->wake.notify_one();
}

void Ch9329ControlSink::update_ui_heartbeat() noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->heartbeat = Clock::now();
}

SubmitResult Ch9329ControlSink::submit(ControlEvent event) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->paste.state == AsciiPasteState::active || impl_->input_sync || impl_->sync_inflight) {
        ++impl_->status.rejected_events;
        return SubmitResult::overloaded;
    }
    const auto result = impl_->queue.submit(std::move(event), Clock::now());
    if (result == SubmitResult::accepted || result == SubmitResult::overloaded)
        impl_->invalidate_sync();
    if (result == SubmitResult::accepted) impl_->status.ordinary_input_pending = true;
    if (result != SubmitResult::accepted) { ++impl_->status.rejected_events; }
    impl_->status.epoch = impl_->queue.epoch();
    impl_->wake.notify_one();
    return result;
}

SubmitResult Ch9329ControlSink::synchronize(InputSync sync) {
    std::lock_guard lock(impl_->mutex);
    const auto& state = sync.state;
    bool valid = sync.intent_generation != 0 && sync.revision != 0 &&
        state.buttons <= 7 && state.absolute_x <= 4095 && state.absolute_y <= 4095 &&
        (state.mode == MouseMode::absolute || state.mode == MouseMode::relative);
    std::array<bool, 256> seen{};
    for (const auto key : state.keys) {
        if (key == 0) continue;
        const bool supported = (key >= 0x04U && key <= 0x73U) ||
            (key >= 0x7fU && key <= 0x82U) || (key >= 0x85U && key <= 0x87U) ||
            (key >= 0x89U && key <= 0x8fU);
        valid = valid && supported && !seen[key];
        seen[key] = true;
    }
    if (!valid || impl_->status.state != ControlConnectionState::ready ||
        !impl_->status.target_usb_ready || !impl_->status.release_confirmed ||
        sync.epoch != impl_->status.epoch || impl_->queue.release_pending() ||
        !impl_->control_active || Clock::now() - impl_->heartbeat > kHeartbeatTimeout) {
        ++impl_->status.rejected_events;
        return SubmitResult::not_ready;
    }
    if (impl_->paste.state == AsciiPasteState::active || impl_->input_sync || impl_->sync_inflight || impl_->ordinary_inflight || impl_->queue.size() != 0 ||
        impl_->relative_x != 0 || impl_->relative_y != 0 || impl_->wheel != 0) {
        ++impl_->status.rejected_events;
        return SubmitResult::overloaded;
    }
    impl_->status.applied.known = false;
    impl_->input_sync = std::move(sync);
    impl_->sync_started = false;
    impl_->wake.notify_one();
    return SubmitResult::accepted;
}

SubmitResult Ch9329ControlSink::start_ascii_paste(AsciiPasteJob job) {
    std::lock_guard lock(impl_->mutex);
    if (job.gestures.empty() || std::ranges::any_of(job.gestures, [](const auto& edges) { return edges.empty(); }) ||
        impl_->status.state != ControlConnectionState::ready || !impl_->status.target_usb_ready ||
        !impl_->status.release_confirmed || !impl_->control_active || Clock::now() - impl_->heartbeat > kHeartbeatTimeout ||
        impl_->input_sync || impl_->sync_inflight || impl_->ordinary_inflight || impl_->queue.size() != 0 ||
        impl_->transaction || impl_->paste.state == AsciiPasteState::active) {
        ++impl_->status.rejected_events;
        return SubmitResult::not_ready;
    }
    impl_->paste = {AsciiPasteState::active, job.gestures.size(), 0};
    impl_->paste_job = std::move(job);
    impl_->paste_gesture = impl_->paste_edge = 0;
    impl_->wake.notify_one();
    return SubmitResult::accepted;
}

void Ch9329ControlSink::cancel_ascii_paste() noexcept {
    std::lock_guard lock(impl_->mutex);
    if (impl_->paste.state == AsciiPasteState::active) {
        impl_->cancel_paste_locked(AsciiPasteState::canceled);
        impl_->queue.request_release();
        impl_->status.epoch = impl_->queue.epoch();
        impl_->status.release_confirmed = false;
        impl_->release_requested = true;
        impl_->wake.notify_one();
    }
}

AsciiPasteSnapshot Ch9329ControlSink::ascii_paste_snapshot() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->paste;
}

void Ch9329ControlSink::release_all() noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->control_active = false;
    impl_->invalidate_sync();
    impl_->cancel_paste_locked(AsciiPasteState::canceled);
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
