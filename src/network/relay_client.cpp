#include "network/relay_client.hpp"
#include "network/relay_protocol.hpp"
#include <atomic>
#include <spdlog/spdlog.h>
#include <deque>
#include <condition_variable>
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
    TrafficSnapshot traffic;
    ClientVideoSnapshot video;
    bool keyframe_pending{};
    std::uint64_t keyframe_generation{};
    std::optional<CaptureSample> latest;
    std::deque<ControlEvent> events;
    std::optional<MouseMode> mode;
    std::uint64_t session{}, min_epoch{}, last_sequence{}, video_sequence{};
    bool release_pending{}, active{}, gui_seen{}, recapture_required{};
    Clock::time_point progress{}, consumed{}, status_at{};

    // Called under mutex, including before accepting a returning GUI tick.
    void expire_gui(Clock::time_point now) {
        if (stopped || now - progress < 250ms) return;
        events.clear();
        if (!active) return;
        active = false;
        recapture_required = true;
        if (!release_pending && min_epoch <= control.epoch) {
            min_epoch = control.epoch + 1;
            release_pending = true;
        }
        control.state = ControlConnectionState::clearing;
        control.release_confirmed = false;
        spdlog::debug("Relay client: GUI input lease expired; release to Preview");
    }
    void fail(std::string error) {
        std::lock_guard lock(mutex);
        if (stopped) return;
        stopped = true;
        spdlog::debug("Relay client: disconnected: {}", error);
        control.state = ControlConnectionState::disconnected;
        control.release_confirmed = false;
        control.target_usb_ready = false;
        control.error = error;
        capture.state = CaptureState::fault;
        capture.error = std::move(error);
        latest.reset(); events.clear(); active = false;
    }
    void publish(CaptureSample sample, VideoCodec codec) {
        std::lock_guard lock(mutex);
        if (stopped) return;
        if (latest) ++capture.overwritten_samples;
        capture.state = CaptureState::streaming;
        capture.actual_mode = {"relay", sample.width, sample.height, {0,1},
            codec == VideoCodec::mjpeg ? PixelFormat::mjpeg : PixelFormat::nv12,
            codec == VideoCodec::mjpeg ? PixelFormat::mjpeg : PixelFormat::nv12,
            codec == VideoCodec::mjpeg ? "MJPEG" : "HEVC"};
        ++capture.received_samples;
        latest = std::move(sample);
    }
    void video_loop(Hello hello) {
        std::string error;
        auto socket = tcp::connect(options.host, options.video_port, 500ms, error);
        if (!socket || !send_packet(*socket, PacketType::hello, encode_hello(hello))) {
            fail("Video connection failed: " + error); return;
        }
        // Only complete decoded pictures enter the latest-value stage. Compressed
        // HEVC stays ordered until decoded, with explicit reference-chain recovery.
        struct Ingress {
            std::mutex mutex;
            std::condition_variable changed;
            std::deque<EncodedAccessUnit> queue;
            std::size_t bytes{};
            std::uint64_t reset{}, sequence{};
            bool waiting{true}, done{};
        } ingress;
        auto recover = [&](const char* reason) {
            // Caller holds ingress.mutex. The marker also fences in-flight output.
            ingress.queue.clear(); ingress.bytes = 0; ++ingress.reset;
            ingress.waiting = true; ingress.sequence = 0;
            std::lock_guard lock(mutex);
            latest.reset();
            keyframe_pending = true; keyframe_generation = hello.session;
            ++video.recoveries; video.error = reason;
        };
        std::jthread decoder_worker;
        if (hello.codec == VideoCodec::hevc) {
            decoder_worker = std::jthread([&] {
                std::string failure;
                auto decoder = create_video_decoder(options.decoder_backend, failure);
                if (!decoder) { fail("HEVC decoder: " + failure); return; }
                std::uint64_t marker = 0;
                bool configured = false;
                std::uint32_t width = 0, height = 0;
                while (!stopped) {
                    EncodedAccessUnit au;
                    {
                        std::unique_lock lock(ingress.mutex);
                        ingress.changed.wait_for(lock, 5ms, [&] { return ingress.done || stopped || !ingress.queue.empty(); });
                        if (ingress.done || stopped) break;
                        if (ingress.queue.empty()) continue;
                        au = std::move(ingress.queue.front()); ingress.queue.pop_front();
                        ingress.bytes -= au.bytes.size();
                        if (Clock::now() - au.arrival > 250ms) { recover("HEVC ingress age exceeded"); continue; }
                        if (marker != ingress.reset) { configured = false; marker = ingress.reset; }
                    }
                    if (!configured || au.width != width || au.height != height) {
                        if (!au.idr) {
                            std::lock_guard lock(ingress.mutex); recover("HEVC dimensions changed without IDR"); continue;
                        }
                        CodecConfig config; config.width = au.width; config.height = au.height;
                        config.generation = au.generation;
                        auto result = decoder->configure(config);
                        if (!result.ok()) { fail("HEVC decoder configuration: " + result.message); break; }
                        configured = true; width = au.width; height = au.height;
                        std::lock_guard lock(mutex); video.decoder_backend = decoder->backend();
                    }
                    auto drain = [&]() {
                        for (unsigned output = 0; output < 32 && !stopped; ++output) {
                            VideoFrame frame;
                            auto result = decoder->poll(frame);
                            if (result.status == CodecStatus::again) return true;
                            if (!result.ok() || !frame.frame) return false;
                            std::lock_guard lock(ingress.mutex);
                            if (marker != ingress.reset) return false;
                            if (Clock::now() - frame.arrival > 250ms) return false;
                            CaptureSample sample;
                            sample.generation = capture_snapshot_generation(); sample.sequence = frame.sequence;
                            sample.arrival = frame.arrival;
                            sample.width = static_cast<std::uint32_t>(frame.frame->width);
                            sample.height = static_cast<std::uint32_t>(frame.frame->height);
                            sample.device_timestamp = frame.device_timestamp;
                            sample.device_time_base_numerator = frame.device_time_base_numerator;
                            sample.device_time_base_denominator = frame.device_time_base_denominator;
                            sample.sample_aspect_ratio_numerator = frame.sample_aspect_ratio_numerator;
                            sample.sample_aspect_ratio_denominator = frame.sample_aspect_ratio_denominator;
                            sample.color_range = frame.color_range; sample.color_matrix = frame.color_matrix;
                            sample.decoded = std::move(frame.frame);
                            publish(std::move(sample), VideoCodec::hevc);
                            const auto diagnostic = decoder->diagnostic();
                            std::lock_guard state_lock(mutex);
                            video.hardware_active = diagnostic.hardware_active;
                            video.hardware_verified = diagnostic.hardware_verified;
                            video.decoder_diagnostic = diagnostic.detail;
                            video.error.clear();
                        }
                        return false;
                    };
                    auto result = decoder->submit(au);
                    bool good = true;
                    while (result.status == CodecStatus::again && Clock::now() - au.arrival <= 250ms && !stopped) {
                        if (!drain()) { good = false; break; }
                        { std::lock_guard lock(ingress.mutex); if (marker != ingress.reset) { good = false; break; } }
                        std::this_thread::sleep_for(1ms);
                        result = decoder->submit(au);
                    }
                    good = good && result.ok() && drain();
                    if (!good) {
                        decoder->reset(); configured = false;
                        std::lock_guard lock(ingress.mutex);
                        if (marker == ingress.reset) recover("HEVC decoder lost progress or rejected access unit");
                    }
                }
                decoder->shutdown();
            });
        }
        std::uint64_t sequence = 0;
        while (!stopped) {
            auto packet = receive_packet(*socket, 600ms);
            if (packet) { std::lock_guard lock(mutex); traffic.video_received_bytes += 12U + packet->payload.size(); }
            if (!packet) break;
            if (hello.codec == VideoCodec::mjpeg) {
                if (packet->type != PacketType::video_mjpeg) break;
                auto sample = decode_mjpeg(packet->payload, capture_snapshot_generation());
                if (!sample || sample->sequence <= sequence) break;
                sequence = sample->sequence;
                publish(std::move(*sample), hello.codec);
                continue;
            }
            if (packet->type != PacketType::video_hevc) break;
            auto au = decode_hevc(packet->payload);
            if (!au) break;
            au->arrival = Clock::now(); // Remote steady-clock epochs are unrelated.
            std::lock_guard lock(ingress.mutex);
            if (au->generation != hello.session) { recover("HEVC generation mismatch"); continue; }
            if (!ingress.waiting && au->encoded_sequence != ingress.sequence + 1)
                recover("HEVC encoded sequence gap");
            if (ingress.queue.size() >= 8 || ingress.bytes + au->bytes.size() > 32U * 1024U * 1024U ||
                (!ingress.queue.empty() && Clock::now() - ingress.queue.front().arrival > 250ms))
                recover("HEVC ordered ingress limit exceeded");
            if (ingress.waiting && !au->idr) {
                std::lock_guard state_lock(mutex); keyframe_pending = true; keyframe_generation = hello.session;
                continue;
            }
            ingress.waiting = false; ingress.sequence = au->encoded_sequence;
            ingress.bytes += au->bytes.size(); ingress.queue.push_back(std::move(*au));
            ingress.changed.notify_one();
        }
        { std::lock_guard lock(ingress.mutex); ingress.done = true; ingress.changed.notify_all(); }
        if (!stopped) fail("Video connection lost or stale");
    }
    std::uint64_t capture_snapshot_generation() { std::lock_guard lock(mutex); return capture.generation; }
    void control_loop() {
        std::string error;
        spdlog::debug("Relay client: connecting control to {}:{}", options.host, options.control_port);
        auto socket = tcp::connect(options.host, options.control_port, 500ms, error);
        if (!socket) { fail("Control connection failed: " + error); return; }
        auto hello = receive_packet(*socket, 500ms);
        if (hello) { std::lock_guard lock(mutex); traffic.control_received_bytes += 12U + hello->payload.size(); }
        if (!hello) { fail("Control handshake read failed or timed out"); return; }
        auto greeting = hello->type == PacketType::hello ? decode_hello(hello->payload) : std::nullopt;
        auto token = greeting ? std::optional(greeting->session) : std::nullopt;
        if (!token || !*token || stopped) { fail("Invalid relay handshake"); return; }
        spdlog::debug("Relay client: control handshake complete, session={}", *token);
        { std::lock_guard lock(mutex); session = *token; video.codec = greeting->codec; }
        video_worker = std::thread([this, greeting = *greeting] { video_loop(greeting); });
        auto initial = receive_packet(*socket, 350ms);
        if (initial) { std::lock_guard lock(mutex); traffic.control_received_bytes += 12U + initial->payload.size(); }
        if (!initial) { fail("Initial control status read failed or timed out"); return; }
        if (initial->type != PacketType::status) { fail("Unexpected initial control packet"); return; }
        auto initial_status = decode_status(initial->payload);
        if (!initial_status || initial_status->session != *token) { fail("Invalid initial status"); return; }
        { std::lock_guard lock(mutex); control = initial_status->control; status_at = Clock::now(); }
        spdlog::debug("Relay client: initial control state={}, epoch={}", static_cast<int>(initial_status->control.state), initial_status->control.epoch);
        // Reading a status must not prevent fresh GUI progress from reaching
        // the server's 250ms heartbeat lease. One reader and one writer own the
        // two TCP directions; this scoped worker joins before socket destruction.
        std::jthread status_reader([&] {
            while (!stopped) {
                auto packet = receive_packet(*socket, 350ms);
                if (packet) { std::lock_guard lock(mutex); traffic.control_received_bytes += 12U + packet->payload.size(); }
                if (!packet) { if (!stopped) fail("Control status read failed or timed out"); return; }
                if (packet->type != PacketType::status) { fail("Unexpected control packet"); return; }
                auto status = decode_status(packet->payload);
                if (!status || status->session != *token) { fail("Invalid control status or session"); return; }
                std::lock_guard lock(mutex);
                if (stopped) return;
                if (status->control.epoch < control.epoch || status->control.epoch < min_epoch) continue;
                if (control.state != status->control.state || control.epoch != status->control.epoch ||
                    control.target_usb_ready != status->control.target_usb_ready || control.release_confirmed != status->control.release_confirmed)
                    spdlog::debug("Relay client: control state {} -> {}, epoch={}, USB ready={}, release confirmed={}",
                        static_cast<int>(control.state), static_cast<int>(status->control.state), status->control.epoch,
                        status->control.target_usb_ready, status->control.release_confirmed);
                control = std::move(status->control);
                status_at = Clock::now();
            }
        });
        auto heartbeat_at = Clock::time_point{};
        auto keyframe_at = Clock::time_point{};
        bool last_active = false;
        std::string failure;
        while (!stopped) {
            PacketType type = PacketType::heartbeat;
            std::vector<std::uint8_t> payload;
            {
                std::lock_guard lock(mutex);
                const auto now = Clock::now();
                expire_gui(now);
                if (now - status_at >= 250ms) {
                    // A recovered status restores Preview readiness, not an old
                    // capture authorization or queued input from before the gap.
                    if (active) recapture_required = true;
                    active = false;
                    events.clear();
                }
                if (release_pending) {
                    type = PacketType::release; payload = encode_session(session);
                    release_pending = false;
                } else if (mode) {
                    type = PacketType::mouse_mode; payload = encode_mouse_mode({session, *mode});
                    min_epoch = control.epoch + 1; control.release_confirmed = false;
                    control.state = ControlConnectionState::clearing; mode.reset();
                } else if ((now - heartbeat_at >= 50ms || active != last_active) && (active || events.empty())) {
                    payload = encode_heartbeat({session, control.epoch, active && gui_seen && now - progress < 250ms && now - status_at < 250ms &&
                        control.state == ControlConnectionState::ready && control.target_usb_ready && control.release_confirmed,
                        consumed != Clock::time_point{} && now - consumed < 500ms, video_sequence});
                    heartbeat_at = now; last_active = active;
                } else if (keyframe_pending && now - keyframe_at >= 100ms) {
                    type = PacketType::keyframe_request;
                    payload = encode_keyframe_request({session, keyframe_generation});
                    keyframe_pending = false; keyframe_at = now;
                } else if (!events.empty()) {
                    auto event = std::move(events.front()); events.pop_front();
                    if (event.epoch == control.epoch && event.epoch >= min_epoch &&
                        now - status_at < 250ms && control.state == ControlConnectionState::ready &&
                        control.target_usb_ready && control.release_confirmed) {
                        type = PacketType::control; payload = encode_session_control({session, event});
                    }
                }
            }
            if (payload.empty()) {
                std::this_thread::sleep_for(2ms); continue;
            }
            if (!send_packet(*socket, type, payload, 50ms)) { failure = "Control send failed or timed out"; break; }
            { std::lock_guard lock(mutex); traffic.control_sent_bytes += 12U + payload.size(); }
        }
        if (!stopped) fail(std::move(failure));
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
        p.video = {}; p.keyframe_pending = false; p.keyframe_generation = 0;
        p.control = {}; p.control.state = ControlConnectionState::opening;
        const auto generation = p.capture.generation + 1;
        p.capture = {}; p.capture.generation = generation; p.capture.state = CaptureState::starting;
        p.latest.reset(); p.events.clear(); p.session = p.min_epoch = p.last_sequence = p.video_sequence = 0;
        p.release_pending = p.active = p.gui_seen = p.recapture_required = false; p.consumed = {}; p.status_at = {};
        // Allow the first GUI tick to follow the asynchronous network handshake.
        // This startup grace is not authorization to forward input.
        p.progress = Clock::now();
        p.stopped = false;
    }
    p.control_worker = std::thread([&p] { p.control_loop(); });
}
void RelayClient::stop() noexcept {
    if (!impl_->stopped) spdlog::debug("Relay client: stop requested");
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
    impl_->expire_gui(Clock::now());
    if (!active) impl_->recapture_required = false;
    if (active && (impl_->recapture_required || !impl_->gui_seen || Clock::now() - impl_->progress >= 250ms)) return;
    // The final special-key release edges still need the GUI-authorized lease.
    // A later GUI tick deactivates after these bounded queued edges drain.
    if (active || impl_->events.empty()) impl_->active = active;
}
void RelayClient::video_presented(std::uint64_t sequence) noexcept {
    std::lock_guard lock(impl_->mutex);
    if (sequence > impl_->video_sequence) { impl_->video_sequence = sequence; impl_->consumed = Clock::now(); }
}
void RelayClient::gui_progress() noexcept {
    std::lock_guard lock(impl_->mutex);
    const auto now = Clock::now();
    impl_->expire_gui(now);
    impl_->progress = now; impl_->gui_seen = true;
}
SubmitResult RelayClient::submit(ControlEvent event) {
    std::lock_guard lock(impl_->mutex);
    auto& p = *impl_;
    if (p.stopped || p.recapture_required || !p.gui_seen || p.control.state != ControlConnectionState::ready || !p.control.target_usb_ready || !p.control.release_confirmed ||
        event.epoch != p.control.epoch || event.epoch < p.min_epoch || event.sequence <= p.last_sequence ||
        Clock::now() - p.status_at >= 250ms || Clock::now() - p.progress >= 250ms ||
        p.consumed == Clock::time_point{} || Clock::now() - p.consumed >= 500ms) return SubmitResult::not_ready;
    if (p.events.size() >= 128) return SubmitResult::overloaded;
    p.last_sequence = event.sequence; p.events.push_back(std::move(event)); return SubmitResult::accepted;
}
ControlSnapshot RelayClient::control_snapshot() const {
    std::lock_guard lock(impl_->mutex);
    auto result = impl_->control;
    if (result.state == ControlConnectionState::ready && (impl_->recapture_required || Clock::now() - impl_->status_at >= 250ms)) {
        result.state = ControlConnectionState::stalled; result.release_confirmed = false;
    }
    return result;
}
CaptureSnapshot RelayClient::capture_snapshot() const { std::lock_guard lock(impl_->mutex); return impl_->capture; }
ClientVideoSnapshot RelayClient::video_snapshot() const { std::lock_guard lock(impl_->mutex); return impl_->video; }
TrafficSnapshot RelayClient::traffic_snapshot() const { std::lock_guard lock(impl_->mutex); return impl_->traffic; }
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
