#include "network/relay_client.hpp"
#include "network/relay_protocol.hpp"
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace kvmux::relay {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
struct RelayClient::Impl {
    explicit Impl(ClientOptions value) : options(std::move(value)) {}
    ClientOptions options;
    mutable std::mutex mutex;
    std::atomic<bool> stopped{true};
    std::thread control_worker, video_worker;
    ControlSnapshot control;
    CaptureSnapshot capture;
    std::optional<CaptureSample> latest;
    std::deque<ControlEvent> events;
    std::optional<MouseMode> mode;
    std::uint64_t session{}, min_epoch{}, last_sequence{}, video_sequence{};
    bool release_pending{}, active{};
    Clock::time_point progress{}, consumed{}, status_at{};

    void fail(std::string error) {
        stopped = true;
        std::lock_guard lock(mutex);
        control.state = ControlConnectionState::disconnected;
        control.release_confirmed = false;
        control.target_usb_ready = false;
        control.error = error;
        capture.state = CaptureState::fault;
        capture.error = std::move(error);
        latest.reset(); events.clear(); active = false;
    }
    void video_loop(std::uint64_t token) {
        std::string error;
        auto socket = tcp::connect(options.host, options.video_port, 500ms, error);
        if (!socket || !send_packet(*socket, PacketType::hello, encode_session(token))) {
            fail("Video connection failed: " + error); return;
        }
        std::uint64_t sequence = 0;
        while (!stopped) {
            auto packet = receive_packet(*socket, 600ms);
            if (!packet || packet->type != PacketType::video_mjpeg) break;
            auto sample = decode_mjpeg(packet->payload, capture_snapshot_generation());
            if (!sample || sample->sequence <= sequence) break;
            sequence = sample->sequence;
            std::lock_guard lock(mutex);
            if (stopped) return;
            if (latest) ++capture.overwritten_samples;
            capture.state = CaptureState::streaming;
            capture.actual_mode = {"relay", sample->width, sample->height, {0,1}, PixelFormat::mjpeg, PixelFormat::mjpeg, "MJPEG"};
            ++capture.received_samples;
            latest = std::move(sample);
        }
        if (!stopped) fail("Video connection lost or stale");
    }
    std::uint64_t capture_snapshot_generation() { std::lock_guard lock(mutex); return capture.generation; }
    void control_loop() {
        std::string error;
        auto socket = tcp::connect(options.host, options.control_port, 500ms, error);
        if (!socket) { fail("Control connection failed: " + error); return; }
        auto hello = receive_packet(*socket, 500ms);
        auto token = hello && hello->type == PacketType::hello ? decode_session(hello->payload) : std::nullopt;
        if (!token || !*token || stopped) { fail("Invalid relay handshake"); return; }
        { std::lock_guard lock(mutex); session = *token; }
        video_worker = std::thread([this, token = *token] { video_loop(token); });
        auto initial = receive_packet(*socket, 350ms);
        if (!initial || initial->type != PacketType::status) { fail("Missing initial status"); return; }
        auto initial_status = decode_status(initial->payload);
        if (!initial_status || initial_status->session != *token) { fail("Invalid initial status"); return; }
        { std::lock_guard lock(mutex); control = initial_status->control; status_at = Clock::now(); }
        auto last_progress = Clock::time_point{};
        auto heartbeat_at = Clock::time_point{};
        bool last_active = false;
        while (!stopped) {
            { std::lock_guard lock(mutex); if (Clock::now() - progress >= 250ms) break; }
            PacketType type = PacketType::heartbeat;
            std::vector<std::uint8_t> payload;
            {
                std::lock_guard lock(mutex);
                const auto now = Clock::now();
                if (release_pending) {
                    type = PacketType::release; payload = encode_session(session);
                    release_pending = false;
                } else if (mode) {
                    type = PacketType::mouse_mode; payload = encode_mouse_mode({session, *mode});
                    min_epoch = control.epoch + 1; control.release_confirmed = false;
                    control.state = ControlConnectionState::clearing; mode.reset();
                } else if ((progress != last_progress || active != last_active) && now - progress < 250ms && (now - heartbeat_at >= 50ms || active != last_active) && (active || events.empty())) {
                    payload = encode_heartbeat({session, control.epoch, active,
                        consumed != Clock::time_point{} && now - consumed < 500ms, video_sequence});
                    last_progress = progress; heartbeat_at = now; last_active = active;
                } else if (!events.empty()) {
                    auto event = std::move(events.front()); events.pop_front();
                    if (event.epoch == control.epoch && event.epoch >= min_epoch) {
                        type = PacketType::control; payload = encode_session_control({session, event});
                    }
                }
            }
            if (payload.empty()) {
                bool expired;
                { std::lock_guard lock(mutex); expired = Clock::now() - progress >= 250ms; }
                if (expired) break;
                std::this_thread::sleep_for(2ms); continue;
            }
            if (!send_packet(*socket, type, payload, 50ms)) break;
            auto packet = receive_packet(*socket, 350ms);
            if (!packet || packet->type != PacketType::status) break;
            auto status = decode_status(packet->payload);
            if (!status || status->session != *token) break;
            std::lock_guard lock(mutex);
            if (status->control.epoch < control.epoch || status->control.epoch < min_epoch) continue;
            control = std::move(status->control);
            status_at = Clock::now();
        }
        if (!stopped) fail("Control connection lost");
    }
};
RelayClient::RelayClient(ClientOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
RelayClient::~RelayClient() {
    stop();
    if (impl_->control_worker.joinable()) impl_->control_worker.join();
    if (impl_->video_worker.joinable()) impl_->video_worker.join();
}
void RelayClient::start() {
    auto& p = *impl_;
    if (!p.stopped) return;
    if (p.control_worker.joinable()) p.control_worker.join();
    if (p.video_worker.joinable()) p.video_worker.join();
    {
        std::lock_guard lock(p.mutex);
        p.control = {}; p.control.state = ControlConnectionState::opening;
        const auto generation = p.capture.generation + 1;
        p.capture = {}; p.capture.generation = generation; p.capture.state = CaptureState::starting;
        p.latest.reset(); p.events.clear(); p.session = p.min_epoch = p.last_sequence = p.video_sequence = 0;
        p.release_pending = p.active = false; p.consumed = {}; p.status_at = {};
        p.stopped = false;
    }
    p.control_worker = std::thread([&p] { p.control_loop(); });
}
void RelayClient::stop() noexcept {
    impl_->stopped = true;
    std::lock_guard lock(impl_->mutex);
    impl_->events.clear(); impl_->latest.reset(); impl_->active = false;
    impl_->control.state = ControlConnectionState::disconnected;
    impl_->control.release_confirmed = false; impl_->control.target_usb_ready = false;
    impl_->capture.state = CaptureState::stopped;
}
void RelayClient::release() noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->events.clear(); impl_->active = false;
    if (!impl_->stopped && impl_->session) {
        if (!impl_->release_pending && impl_->min_epoch <= impl_->control.epoch) {
            impl_->min_epoch = impl_->control.epoch + 1;
            impl_->release_pending = true;
        }
        impl_->control.state = ControlConnectionState::clearing;
    }
    impl_->control.release_confirmed = false;
}
void RelayClient::mouse_mode(MouseMode mode) {
    release(); std::lock_guard lock(impl_->mutex); impl_->mode = mode;
}
void RelayClient::active(bool active) noexcept {
    std::lock_guard lock(impl_->mutex);
    // The final special-key release edges still need the GUI-authorized lease.
    // A later GUI tick deactivates after these bounded queued edges drain.
    if (active || impl_->events.empty()) impl_->active = active;
}
void RelayClient::video_presented(std::uint64_t sequence) noexcept {
    std::lock_guard lock(impl_->mutex);
    if (sequence > impl_->video_sequence) { impl_->video_sequence = sequence; impl_->consumed = Clock::now(); }
}
void RelayClient::gui_progress() noexcept { std::lock_guard lock(impl_->mutex); impl_->progress = Clock::now(); }
SubmitResult RelayClient::submit(ControlEvent event) {
    std::lock_guard lock(impl_->mutex);
    auto& p = *impl_;
    if (p.stopped || p.control.state != ControlConnectionState::ready || !p.control.target_usb_ready || !p.control.release_confirmed ||
        event.epoch != p.control.epoch || event.epoch < p.min_epoch || event.sequence <= p.last_sequence ||
        Clock::now() - p.status_at >= 250ms || Clock::now() - p.progress >= 250ms ||
        p.consumed == Clock::time_point{} || Clock::now() - p.consumed >= 500ms) return SubmitResult::not_ready;
    if (p.events.size() >= 128) return SubmitResult::overloaded;
    p.last_sequence = event.sequence; p.events.push_back(std::move(event)); return SubmitResult::accepted;
}
ControlSnapshot RelayClient::control_snapshot() const {
    std::lock_guard lock(impl_->mutex);
    auto result = impl_->control;
    if (result.state == ControlConnectionState::ready && Clock::now() - impl_->status_at >= 250ms) {
        result.state = ControlConnectionState::stalled; result.release_confirmed = false;
    }
    return result;
}
CaptureSnapshot RelayClient::capture_snapshot() const { std::lock_guard lock(impl_->mutex); return impl_->capture; }
std::optional<CaptureSample> RelayClient::take_sample() {
    std::lock_guard lock(impl_->mutex);
    auto sample = std::exchange(impl_->latest, std::nullopt);
    return sample;
}
std::vector<DeviceInfo> NetworkCaptureSource::enumerate_devices() {
    DeviceInfo device; device.stable_id = "relay"; device.display_name = "LAN relay"; device.weak_match = true;
    return {device};
}
std::vector<CaptureMode> NetworkCaptureSource::enumerate_modes(const std::string&) {
    return {{"relay", 0, 0, {0,1}, PixelFormat::mjpeg, PixelFormat::mjpeg, "MJPEG"}};
}
} // namespace kvmux::relay
