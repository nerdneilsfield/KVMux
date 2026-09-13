#include "network/relay_client.hpp"
#include "network/relay_session.hpp"
#include "network/kcp_channel.hpp"
#include "network/paste_execute_retry.hpp"
#include "network/media_codec_wire.hpp"
#include "support/crc32.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cmath>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <utility>

namespace kvmux::relay {
PasteUploadSnapshot merge_paste_status(PasteUploadSnapshot current, const wire::PasteStatus& status) noexcept {
    if (current.terminal()) return current;
    const auto phase = [](PasteUploadState state) noexcept {
        switch (state) {
        case PasteUploadState::uploading: return 1U;
        case PasteUploadState::uploaded: return 2U;
        case PasteUploadState::preparing: return 3U;
        case PasteUploadState::executing: return 4U;
        default: return 0U;
        }
    };
    const auto received_phase = static_cast<unsigned>(status.state);
    if (status.state != wire::PasteState::finished && received_phase < phase(current.state)) return current;

    current.accepted_bytes = std::max(current.accepted_bytes, std::min(status.accepted_bytes, current.total_bytes));
    current.completed_bytes = std::max(current.completed_bytes, std::min(status.completed_bytes, current.total_bytes));
    current.reason = status.reason;
    switch (status.state) {
    case wire::PasteState::uploading: current.state = PasteUploadState::uploading; break;
    case wire::PasteState::uploaded: current.state = PasteUploadState::uploaded; break;
    case wire::PasteState::preparing: current.state = PasteUploadState::preparing; break;
    case wire::PasteState::executing: current.state = PasteUploadState::executing; break;
    case wire::PasteState::finished:
        switch (status.outcome) {
        case wire::PasteOutcome::completed: current.state = PasteUploadState::completed; break;
        case wire::PasteOutcome::canceled: current.state = PasteUploadState::canceled; break;
        case wire::PasteOutcome::expired: current.state = PasteUploadState::expired; break;
        default: current.state = PasteUploadState::rejected; break;
        }
        break;
    }
    return current;
}
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
struct RelayClient::Impl {
    explicit Impl(ClientOptions value) : options(std::move(value)) { control.recoverable_transport = true; }
    ClientOptions options;
    // Serializes start/stop callers. The worker never takes this lock, so a
    // completed worker can be joined here without blocking its state updates.
    std::mutex lifecycle_mutex;
    mutable std::mutex mutex;
    std::atomic<bool> stopped{true};
    std::thread worker, decoder_worker;
    ControlSnapshot control;
    CaptureSnapshot capture;
    TrafficSnapshot traffic;
    ClientVideoSnapshot video;
    std::optional<CaptureSample> latest;
    std::deque<ControlEvent> events;
    struct PasteUpload {
        std::vector<std::uint8_t> bytes;
        PasteUploadSnapshot snapshot;
        std::uint64_t epoch{}, intent{};
        std::uint32_t next_chunk{};
        bool begin_sent{}, cancel_pending{};
        // A full KCP window retries without a new proof, but never spins.
        PasteExecuteRetry execute_retry;
    };
    std::optional<PasteUpload> paste;
    std::uint64_t next_paste_id{};
    std::optional<InputSync> requested_sync;
    std::optional<wire::Sync> pending_sync;
    std::uint64_t intent{}, wire_sequence{}, last_source_sequence{};
    bool active{}, barrier{}, release_pending{}, disconnect_pending{}, gui_seen{}, intent_canceled{}, cancel_inflight{};
    MouseMode mode{MouseMode::absolute};
    Clock::time_point progress{}, challenge_at{}, consumed{}, status_at{}, sync_requested_at{};
    std::uint64_t presented_media{}, presented_capture{};
    struct Presentation {
        std::uint64_t capture{}, media{}, generation{}, marker{};
        Clock::time_point arrival{};
        bool published{};
    };
    std::deque<Presentation> presentations;
    std::uint64_t marker{}, generation{};
    // Network owns marker/recovery. Decoder posts only a coalesced reason.
    std::condition_variable decode_wake;
    struct Compressed { EncodedAccessUnit au; std::uint64_t marker{}; };
    std::deque<Compressed> ingress;
    std::size_t ingress_bytes{};
    std::optional<MediaReason> decode_recovery;
    bool decode_done{};

    bool fresh(Clock::time_point now) const {
        return gui_seen && now - progress < 250ms && challenge_at != Clock::time_point{} &&
            now - challenge_at < 250ms && consumed != Clock::time_point{} && now - consumed < 500ms;
    }
    bool ready(Clock::time_point now) const {
        return !stopped && fresh(now) && control.state == ControlConnectionState::ready &&
            control.target_usb_ready && control.release_confirmed;
    }
    void suspend() {
        barrier = false; control.applied.known = false;
        events.clear(); requested_sync.reset(); pending_sync.reset();
    }
    void apply_paste_status(const wire::PasteStatus& value) {
        if (!paste || value.transaction_id != paste->snapshot.transaction_id ||
            paste->epoch != control.epoch || paste->intent != intent) return;
        const auto previous = paste->snapshot;
        paste->snapshot = merge_paste_status(paste->snapshot, value);
        if (!previous.terminal() && paste->snapshot.terminal()) paste->bytes.clear();
        if (paste->snapshot.state == PasteUploadState::executing) paste->bytes.clear();
    }
    void abandon_paste(PasteUploadState state, wire::PasteStatusReason reason) {
        if (!paste) return;
        paste->snapshot.state = state; paste->snapshot.reason = reason;
        paste->snapshot.accepted_bytes = 0; paste->snapshot.completed_bytes = 0;
        paste->bytes.clear(); paste->cancel_pending = false;
    }
    void fail(std::string error) {
        std::lock_guard lock(mutex);
        stopped = true; suspend(); active = false;
        control.state = ControlConnectionState::disconnected;
        control.target_usb_ready = control.release_confirmed = false; control.error = error;
        capture.state = CaptureState::fault; capture.error = std::move(error);
        latest.reset(); decode_wake.notify_all();
    }
    void publish(CaptureSample sample, VideoCodec codec) {
        // Caller holds mutex; presentation identity was registered at ingress.
        if (stopped || sample.generation != capture.generation) return;
        if (latest) ++capture.overwritten_samples;
        capture.state = CaptureState::streaming;
        capture.actual_mode = {"relay", sample.width, sample.height, {0,1},
            codec == VideoCodec::mjpeg ? PixelFormat::mjpeg : PixelFormat::nv12,
            codec == VideoCodec::mjpeg ? PixelFormat::mjpeg : PixelFormat::nv12,
            codec == VideoCodec::mjpeg ? "MJPEG" : "HEVC"};
        for (auto& identity : presentations) {
            if (identity.capture == sample.sequence && identity.generation == sample.generation && identity.marker == marker)
                identity.published = true;
        }
        ++capture.received_samples; latest = std::move(sample);
    }
    void decoder_loop() {
        std::string error;
        auto decoder = create_video_decoder(options.decoder_backend, error);
        if (!decoder) { fail("HEVC decoder: " + error); return; }
        bool configured{};
        std::uint64_t current_marker{};
        std::uint32_t width{}, height{};
        while (!stopped) {
            Compressed value;
            {
                std::unique_lock lock(mutex);
                decode_wake.wait_for(lock, 5ms, [&] { return stopped || decode_done || !ingress.empty() || marker != current_marker; });
                if (stopped || decode_done) break;
                if (marker != current_marker) {
                    current_marker = marker; configured = false;
                    lock.unlock(); decoder->reset(); continue;
                }
                if (ingress.empty()) continue;
                value = std::move(ingress.front()); ingress.pop_front(); ingress_bytes -= value.au.bytes.size();
            }
            auto& au = value.au;
            auto recover = [&] {
                std::lock_guard lock(mutex);
                if (value.marker == marker) decode_recovery = MediaReason::decoder_failure;
            };
            if (Clock::now() - au.arrival >= 250ms) { recover(); continue; }
            if (!configured || width != au.width || height != au.height) {
                if (!au.idr) { recover(); continue; }
                CodecConfig config; config.width = au.width; config.height = au.height; config.generation = au.generation;
                const auto result = decoder->configure(config);
                if (!result.ok()) { recover(); continue; }
                configured = true; width = au.width; height = au.height;
                std::lock_guard lock(mutex); video.decoder_backend = decoder->backend();
            }
            auto drain = [&] {
                for (unsigned i = 0; i < 32 && !stopped; ++i) {
                    VideoFrame frame;
                    auto result = decoder->poll(frame);
                    if (result.status == CodecStatus::again) return true;
                    if (!result.ok() || !frame.frame) return false;
                    std::lock_guard lock(mutex);
                    if (value.marker != marker || au.generation != generation || Clock::now() - frame.arrival >= 250ms) return false;
                    auto identity = std::find_if(presentations.begin(), presentations.end(), [&](const Presentation& p) {
                        return p.capture == frame.sequence && p.generation == capture.generation && p.marker == marker;
                    });
                    if (identity == presentations.end()) return false;
                    CaptureSample sample;
                    sample.generation = identity->generation; sample.sequence = frame.sequence; sample.arrival = identity->arrival;
                    sample.width = static_cast<std::uint32_t>(frame.frame->width); sample.height = static_cast<std::uint32_t>(frame.frame->height);
                    sample.device_timestamp = frame.device_timestamp;
                    sample.device_time_base_numerator = frame.device_time_base_numerator;
                    sample.device_time_base_denominator = frame.device_time_base_denominator;
                    sample.sample_aspect_ratio_numerator = frame.sample_aspect_ratio_numerator;
                    sample.sample_aspect_ratio_denominator = frame.sample_aspect_ratio_denominator;
                    sample.color_range = frame.color_range; sample.color_matrix = frame.color_matrix;
                    sample.decoded = std::move(frame.frame);
                    publish(std::move(sample), VideoCodec::hevc);
                    const auto diagnostic = decoder->diagnostic();
                    video.hardware_active = diagnostic.hardware_active; video.hardware_verified = diagnostic.hardware_verified;
                    video.decoder_diagnostic = diagnostic.detail; video.error.clear();
                }
                return false;
            };
            auto result = decoder->submit(au);
            bool good = true;
            while (result.status == CodecStatus::again && Clock::now() - au.arrival < 250ms && !stopped) {
                if (!drain()) { good = false; break; }
                { std::lock_guard lock(mutex); if (value.marker != marker) { good = false; break; } }
                std::this_thread::sleep_for(1ms);
                result = decoder->submit(au);
            }
            if (!good || !result.ok() || !drain()) { recover(); configured = false; decoder->reset(); }
        }
        decoder->shutdown();
    }
    void run() {
        std::string error;
        auto control_peer = udp::resolve(options.host, options.control_port, error);
        auto media_peer = udp::resolve(options.host, options.video_port, error);
        auto socket = udp::Socket::bind("0.0.0.0", 0, error);
        if (!control_peer || !media_peer || !socket) { fail(error); return; }
        ClientSession session(*control_peer);
        std::unique_ptr<KcpChannel> kcp;
        std::unique_ptr<MediaReceiver> receiver;
        std::optional<wire::RefreshRequest> refresh;
        std::optional<wire::MediaFeedback> feedback;
        Clock::time_point paste_keepalive_at{};
        std::vector<std::vector<std::uint8_t>> output;
        std::size_t output_index{};
        std::uint64_t reliable_cancel{};
        bool closing{};
        auto actions = [&](const SessionActions& batch) {
            for (const auto& action : batch) {
                if (action.kind == SessionAction::Kind::send_raw) {
                    auto result = socket->send_to(action.peer, action.datagram);
                    if (result == udp::SendStatus::sent) { std::lock_guard lock(mutex); traffic.control_sent_bytes += action.datagram.size(); }
                    else if (result == udp::SendStatus::error) fail("UDP control send failed");
                } else if (action.kind == SessionAction::Kind::established) {
                    kcp = std::make_unique<KcpChannel>(session.tuple().conversation);
                    receiver = std::make_unique<MediaReceiver>(session.welcome().codec, session.welcome().generation);
                    { std::lock_guard lock(mutex); generation = session.welcome().generation; video.codec = session.welcome().codec; video.media = {}; video.last_recovery_reason = MediaReason::none; }
                    if (session.welcome().codec == VideoCodec::hevc) decoder_worker = std::thread([this] { decoder_loop(); });
                } else if (action.kind == SessionAction::Kind::expired) {
                    if (closing) stopped = true;
                    else if (!stopped) fail("UDP session expired");
                } else if (action.kind == SessionAction::Kind::rejected) {
                    fail(action.rejection == wire::BusyReason::no_common_codec ? "No common relay codec" : "Relay already controlled");
                }
            }
        };
        auto media_events = [&](std::vector<MediaEvent> batch) {
            for (auto& event : batch) {
                { std::lock_guard lock(mutex);
                    video.media = receiver->stats();
                    if (event.reason != MediaReason::none) video.last_recovery_reason = event.reason;
                }
                if (event.kind == MediaEvent::Kind::reset) {
                    std::lock_guard lock(mutex);
                    ++marker; ++capture.generation;
                    ingress.clear(); ingress_bytes = 0; latest.reset(); presentations.clear();
                    consumed = {}; presented_media = presented_capture = 0; ++video.recoveries;
                    decode_wake.notify_one();
                } else if (event.kind == MediaEvent::Kind::refresh) {
                    refresh = wire::RefreshRequest{session.welcome().generation, event.reason};
                } else if (event.kind == MediaEvent::Kind::feedback) {
                    feedback = wire::MediaFeedback{session.welcome().generation, event.stats};
                } else if (event.kind == MediaEvent::Kind::frame && event.frame) {
                    auto& frame = *event.frame;
                    std::lock_guard lock(mutex);
                    if (frame.codec == VideoCodec::mjpeg) {
                        auto sample = decode_mjpeg(frame.bytes, capture.generation);
                        if (!sample) continue;
                        sample->arrival = frame.first_arrival;
                        presentations.push_back({sample->sequence, frame.sequence, capture.generation, marker, frame.first_arrival});
                        publish(std::move(*sample), VideoCodec::mjpeg);
                    } else {
                        auto au = decode_hevc(frame.bytes);
                        if (!au) { decode_recovery = MediaReason::decoder_failure; continue; }
                        au->arrival = frame.first_arrival;
                        if (ingress.size() >= 8 || ingress_bytes + au->bytes.size() > kMediaAllocationLimit ||
                            Clock::now() - au->arrival >= 250ms ||
                            (!ingress.empty() && Clock::now() - ingress.front().au.arrival >= 250ms)) {
                            decode_recovery = MediaReason::ingress_overflow; continue;
                        }
                        presentations.push_back({au->capture_sequence, frame.sequence, capture.generation, marker, frame.first_arrival});
                        ingress_bytes += au->bytes.size(); ingress.push_back({std::move(*au), marker}); decode_wake.notify_one();
                    }
                    while (presentations.size() > 512) presentations.pop_front();
                }
            }
        };
        std::random_device random;
        std::uint64_t nonce{};
        while (!nonce) nonce = (std::uint64_t(random()) << 32U) ^ random();
        actions(session.start(nonce, Clock::now()));
        while (!stopped) {
            auto now = Clock::now();
            bool cancel{}, disconnect{};
            {
                std::lock_guard lock(mutex);
                if (!fresh(now)) suspend();
                // A GUI stall is an intent revocation, not automatic recapture.
                if (active && gui_seen && now - progress >= 250ms) { active = false; intent_canceled = true; release_pending = true; }
                cancel = std::exchange(release_pending, false);
                disconnect = std::exchange(disconnect_pending, false);
                if (disconnect && !intent) intent = 1;
                session.set_intent(intent, active, fresh(now), presented_media);
            }
            if (cancel || disconnect) {
                actions(session.cancel(disconnect ? wire::CancelReason::disconnect : wire::CancelReason::release, now));
                if (disconnect) { closing = true; if (session.phase() != SessionPhase::established) stopped = true; }
            }
            actions(session.tick(now));
            for (unsigned i = 0; i < 32 && !stopped; ++i) {
                auto packet = socket->receive(i == 0 ? 1ms : 0ms);
                if (packet.status == udp::ReceiveStatus::idle) break;
                if (packet.status == udp::ReceiveStatus::error) { fail("UDP receive failed"); break; }
                if (packet.status != udp::ReceiveStatus::datagram) continue;
                auto envelope = wire::decode_envelope(packet.datagram.bytes);
                if (!envelope) continue;
                now = Clock::now();
                if (envelope->kind == wire::EnvelopeKind::media) {
                    if (receiver && packet.datagram.source == *media_peer && session.phase() == SessionPhase::established && envelope->tuple == session.tuple()) {
                        { std::lock_guard lock(mutex); traffic.video_received_bytes += packet.datagram.bytes.size(); }
                        media_events(receiver->input(envelope->body, now));
                    }
                    continue;
                }
                if (!(packet.datagram.source == *control_peer)) continue;
                if (envelope->kind == wire::EnvelopeKind::kcp) {
                    if (kcp && session.phase() == SessionPhase::established && envelope->tuple == session.tuple() && kcp->input(envelope->body)) {
                        std::lock_guard lock(mutex); traffic.control_received_bytes += packet.datagram.bytes.size();
                    }
                    continue;
                }
                const auto challenge = session.latest_challenge();
                const auto cancellation = session.pending_cancel();
                // Proof reflects current consumption. Receiving the challenge is not video progress.
                { std::lock_guard lock(mutex);
                    const bool video_fresh = gui_seen && now - progress < 250ms && consumed != Clock::time_point{} && now - consumed < 500ms;
                    session.set_intent(intent, active, video_fresh, presented_media);
                }
                actions(session.on_datagram(packet.datagram.source, packet.datagram.bytes, now));
                { std::lock_guard lock(mutex);
                    traffic.control_received_bytes += packet.datagram.bytes.size();
                    if (session.latest_challenge() > challenge) challenge_at = now;
                    if (cancellation && !session.pending_cancel()) cancel_inflight = false;
                }
            }
            now = Clock::now();
            actions(session.tick(now));
            if (receiver) {
                std::optional<MediaReason> recovery;
                { std::lock_guard lock(mutex); recovery = std::exchange(decode_recovery, std::nullopt); }
                if (recovery) media_events(receiver->recover(*recovery, now));
                media_events(receiver->poll(now));
            }
            if (!kcp || stopped) continue;
            for (unsigned i = 0; i < 32; ++i) {
                auto bytes = kcp->receive(); if (!bytes) break;
                auto message = wire::decode_control(*bytes, wire::Direction::server_to_client);
                if (!message) continue;
                std::lock_guard lock(mutex);
                if (auto value = std::get_if<wire::Status>(&*message)) {
                    if (value->epoch < control.epoch) continue;
                    if (value->epoch != control.epoch || value->connection != ControlConnectionState::ready ||
                        !value->usb_ready || !value->release_confirmed || intent <= value->canceled_through) suspend();
                    if (cancel_inflight && value->canceled_through >= intent) cancel_inflight = false;
                    const bool same_epoch = value->epoch == control.epoch;
                    control.epoch = value->epoch; control.state = value->connection;
                    control.target_usb_ready = value->usb_ready; control.release_confirmed = value->release_confirmed;
                    control.ordinary_input_pending = value->ordinary_input_pending;
                    control.completed_ordinary_sequence = same_epoch ? value->completed_ordinary_sequence : 0;
                    status_at = now;
                } else if (auto value = std::get_if<wire::PasteStatus>(&*message)) {
                    apply_paste_status(*value);
                } else if (auto value = std::get_if<wire::StateAck>(&*message)) {
                    if (pending_sync && ready(now) && active && value->epoch == control.epoch && value->intent == intent &&
                        value->revision == pending_sync->revision && value->edge_floor == pending_sync->edge_floor &&
                        wire::same_state(value->state, pending_sync->state)) {
                        control.applied = {true, value->epoch, value->intent, value->revision, value->state};
                        barrier = true; pending_sync.reset();
                    }
                }
            }
            auto submit = [&](const wire::Control& value, unsigned reserve_slots = 0) {
                auto bytes = wire::encode_control(value, wire::Direction::client_to_server);
                return bytes && kcp->submit(*bytes, reserve_slots) == SubmitResult::accepted;
            };
            bool paste_lifecycle_pending = false;
            {
                std::lock_guard lock(mutex);
                paste_lifecycle_pending = paste && paste->epoch == control.epoch && paste->intent == intent &&
                    (paste->cancel_pending || !paste->snapshot.terminal() || paste->snapshot.state == PasteUploadState::executing);
            }
            // Cancellation ends the transaction in KCP order, so it may take the
            // one bounded transaction-control slot. No later keepalive can overtake it.
            if (auto cancellation = session.pending_cancel(); cancellation && cancellation->intent != reliable_cancel) {
                if (submit(*cancellation)) reliable_cancel = cancellation->intent;
            }
            {
                std::lock_guard lock(mutex);
                if (requested_sync && ready(now) && active && challenge_at > sync_requested_at) {
                    const auto& request = *requested_sync;
                    wire::Sync value{request.epoch, request.intent_generation, request.revision, session.latest_challenge(), wire_sequence, request.state};
                    if (submit(value, paste_lifecycle_pending ? 1 : 0)) { pending_sync = value; requested_sync.reset(); }
                }
                if (paste && paste->epoch == control.epoch && paste->intent == intent) {
                    auto& job = *paste;
                    if (job.cancel_pending) {
                        // Cancel is ordered before any later keepalive, so it can consume
                        // the reserved slot and stop the server-side transaction.
                        if (submit(wire::PasteCancel{job.snapshot.transaction_id, wire::PasteCancelReason::user})) job.cancel_pending = false;
                    } else if (job.snapshot.state == PasteUploadState::executing) {
                        // A completed transaction keeps its own control lease. Video may recover independently.
                        // Status is optional progress feedback, not a transport ACK. Send one
                        // coalesced renewal per interval, with KCP's bounded window as the
                        // backpressure limit, so dropped server statuses cannot end the lease.
                        if (now - paste_keepalive_at >= 100ms &&
                            submit(wire::PasteKeepalive{job.snapshot.transaction_id})) {
                            paste_keepalive_at = now;
                        }
                    } else if (!job.snapshot.terminal() && ready(now) && active) {
                        if (!job.begin_sent) {
                            if (submit(wire::PasteBegin{job.snapshot.transaction_id, job.snapshot.total_bytes,
                                    support::crc32_ieee(job.bytes)}, 1)) job.begin_sent = true;
                        } else if (job.next_chunk * 960U < job.bytes.size()) {
                            const auto offset = static_cast<std::size_t>(job.next_chunk) * 960U;
                            const auto count = std::min<std::size_t>(960, job.bytes.size() - offset);
                            wire::PasteChunk chunk{job.snapshot.transaction_id, job.next_chunk,
                                std::vector<std::uint8_t>(job.bytes.begin() + static_cast<std::ptrdiff_t>(offset), job.bytes.begin() + static_cast<std::ptrdiff_t>(offset + count))};
                            if (submit(chunk, 1)) ++job.next_chunk;
                        } else if (const auto challenge = session.latest_challenge();
                            should_retry_paste_execute(job.execute_retry, challenge, now)) {
                            // Execute consumes the final lifecycle slot. A full window retries at
                            // a bounded cadence until it drains, rather than waiting for another proof.
                            if (submit(wire::PasteExecute{job.snapshot.transaction_id}))
                                accept_paste_execute(job.execute_retry, challenge);
                        }
                    }
                }
                for (unsigned i = 0; i < 32 && barrier && ready(now) && active && !events.empty(); ++i) {
                    auto& event = events.front();
                    wire::Edge edge{control.epoch, intent, wire_sequence + 1, event.sequence, session.latest_challenge(), event.payload};
                    if (!submit(edge, paste_lifecycle_pending ? 1 : 0)) break;
                    ++wire_sequence; events.pop_front();
                }
            }
            // Feedback is optional and coalesced. Do not let it consume the lifecycle slot.
            if (!paste_lifecycle_pending && refresh && submit(*refresh, 1)) refresh.reset();
            if (!paste_lifecycle_pending && feedback && submit(*feedback, 1)) feedback.reset();
            // Do not call ikcp_update while UDP still owns an unsent output batch.
            // Its output callback is bounded, so advancing KCP here would turn a
            // recoverable socket would-block into a transport failure.
            auto drain_output = [&] {
                for (unsigned i = 0; i < 32 && output_index < output.size(); ++i) {
                    auto bytes = wire::encode_envelope({wire::EnvelopeKind::kcp, session.tuple(), output[output_index]});
                    if (!bytes) { fail("Invalid KCP envelope"); return false; }
                    auto result = socket->send_to(*control_peer, *bytes);
                    if (result == udp::SendStatus::would_block) return false;
                    if (result != udp::SendStatus::sent) { fail("UDP KCP send failed"); return false; }
                    { std::lock_guard lock(mutex); traffic.control_sent_bytes += bytes->size(); }
                    ++output_index;
                }
                return output_index == output.size();
            };
            if (!drain_output()) continue;
            kcp->update(static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()));
            if (kcp->failed()) { fail("KCP queue or transport failure"); break; }
            output = kcp->take_datagrams(); output_index = 0;
            (void)drain_output();
        }
        { std::lock_guard lock(mutex); decode_done = true; decode_wake.notify_all(); }
    }
};
RelayClient::RelayClient(ClientOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
RelayClient::~RelayClient() {
    stop();
    if (impl_->worker.joinable()) impl_->worker.join();
    if (impl_->decoder_worker.joinable()) impl_->decoder_worker.join();
}
void RelayClient::start() {
    auto& p = *impl_;
    std::lock_guard lifecycle_lock(p.lifecycle_mutex);
    if (!p.stopped) return;
    // Do not hold p.mutex while joining: a worker may need it to finish.
    if (p.worker.joinable()) p.worker.join();
    if (p.decoder_worker.joinable()) p.decoder_worker.join();
    {
        std::lock_guard lock(p.mutex);
        p.control = {}; p.control.recoverable_transport = true; p.control.state = ControlConnectionState::opening;
        const auto generation = p.capture.generation + 1;
        p.capture = {}; p.capture.generation = generation; p.capture.state = CaptureState::starting;
        p.video = {}; p.latest.reset(); p.events.clear(); p.paste.reset(); p.next_paste_id = 0; p.requested_sync.reset(); p.pending_sync.reset();
        p.ingress.clear(); p.ingress_bytes = 0; p.presentations.clear(); p.decode_recovery.reset();
        p.intent = p.wire_sequence = p.last_source_sequence = p.presented_media = p.presented_capture = p.marker = p.generation = 0;
        p.active = p.barrier = p.release_pending = p.disconnect_pending = p.gui_seen = p.decode_done = p.intent_canceled = p.cancel_inflight = false;
        p.progress = Clock::now(); p.challenge_at = p.consumed = p.status_at = {}; p.stopped = false;
    }
    p.worker = std::thread([&p] { p.run(); });
}
void RelayClient::stop() noexcept {
    auto& p = *impl_;
    std::lock_guard lifecycle_lock(p.lifecycle_mutex);
    std::lock_guard lock(p.mutex);
    p.active = false; p.suspend(); p.abandon_paste(PasteUploadState::canceled, wire::PasteStatusReason::session); p.latest.reset();
    if (!p.stopped) p.disconnect_pending = true;
    p.control.state = ControlConnectionState::disconnected;
    p.control.release_confirmed = p.control.target_usb_ready = false;
    p.capture.state = CaptureState::stopped;
}
void RelayClient::release() noexcept {
    auto& p = *impl_; std::lock_guard lock(p.mutex);
    p.suspend(); p.active = false;
    if (p.paste && !p.paste->snapshot.terminal()) { p.paste->cancel_pending = true; p.paste->snapshot.state = PasteUploadState::canceled; p.paste->snapshot.reason = wire::PasteStatusReason::canceled; p.paste->bytes.clear(); }
    if (!p.stopped && p.intent) { p.release_pending = true; p.cancel_inflight = true; }
}
void RelayClient::mouse_mode(MouseMode mode) {
    auto& p = *impl_; std::lock_guard lock(p.mutex); p.mode = mode; p.suspend();
}
void RelayClient::active(bool active) noexcept {
    std::lock_guard lock(impl_->mutex);
    // A later router intent may reacquire temporary control after a confirmed
    // release. `intent_canceled` fences the old generation at synchronization;
    // it must not permanently suppress this local liveness request.
    if (active || impl_->events.empty()) impl_->active = active;
}
void RelayClient::gui_progress() noexcept {
    std::lock_guard lock(impl_->mutex); impl_->progress = Clock::now(); impl_->gui_seen = true;
}
void RelayClient::video_presented(std::uint64_t generation, std::uint64_t sequence) noexcept {
    auto& p = *impl_; std::lock_guard lock(p.mutex);
    if (generation != p.capture.generation || sequence <= p.presented_capture) return;
    auto found = std::find_if(p.presentations.begin(), p.presentations.end(), [&](const Impl::Presentation& entry) {
        return entry.published && entry.generation == generation && entry.capture == sequence && entry.marker == p.marker;
    });
    if (found == p.presentations.end() || Clock::now() - found->arrival >= 500ms) return;
    p.presented_capture = sequence; p.presented_media = found->media; p.consumed = found->arrival;
}
kvmux::SubmitResult RelayClient::synchronize(InputSync value) {
    auto& p = *impl_; std::lock_guard lock(p.mutex);
    if (p.cancel_inflight || !p.ready(Clock::now()) || value.epoch != p.control.epoch || !value.intent_generation || !value.revision || !wire::valid_state(value.state))
        return kvmux::SubmitResult::not_ready;
    if ((p.requested_sync || p.pending_sync) && Clock::now() - p.sync_requested_at >= 500ms) p.suspend();
    if (p.requested_sync || p.pending_sync) return kvmux::SubmitResult::overloaded;
    if (value.intent_generation != p.intent) {
        if (value.intent_generation < p.intent) return kvmux::SubmitResult::not_ready;
        p.wire_sequence = 0; p.intent = value.intent_generation; p.intent_canceled = false;
    }
    p.active = true; p.barrier = false; p.control.applied.known = false; p.events.clear();
    p.requested_sync = value; p.sync_requested_at = Clock::now();
    return kvmux::SubmitResult::accepted;
}
kvmux::SubmitResult RelayClient::submit(ControlEvent event) {
    auto& p = *impl_; std::lock_guard lock(p.mutex);
    if (!p.ready(Clock::now()) || !p.barrier || !p.active || event.epoch != p.control.epoch || event.sequence <= p.last_source_sequence)
        return kvmux::SubmitResult::not_ready;
    if (p.events.size() >= 128) { p.suspend(); return kvmux::SubmitResult::overloaded; }
    if (!wire::encode_control(wire::Edge{event.epoch, p.intent, 1, event.sequence, 1, event.payload}, wire::Direction::client_to_server))
        return kvmux::SubmitResult::not_ready;
    p.last_source_sequence = event.sequence;
    // Source IDs are diagnostic only; wire IDs are assigned after adjacent merging.
    if (!p.events.empty()) {
        if (auto value = std::get_if<AbsoluteMotion>(&event.payload); value && std::holds_alternative<AbsoluteMotion>(p.events.back().payload)) {
            p.events.back() = std::move(event); return kvmux::SubmitResult::accepted;
        }
        if (auto value = std::get_if<RelativeMotion>(&event.payload)) {
            if (auto previous = std::get_if<RelativeMotion>(&p.events.back().payload)) {
                const auto dx = previous->dx + value->dx, dy = previous->dy + value->dy;
                if (std::isfinite(dx) && std::isfinite(dy)) {
                    previous->dx = dx; previous->dy = dy; return kvmux::SubmitResult::accepted;
                }
            }
        }
    }
    p.control.applied.known = false; p.control.ordinary_input_pending = true; p.events.push_back(std::move(event)); return kvmux::SubmitResult::accepted;
}
kvmux::SubmitResult RelayClient::start_ascii_paste_text(std::vector<std::uint8_t> normalized) {
    auto& p = *impl_; std::lock_guard lock(p.mutex);
    if (normalized.empty() || normalized.size() > 65536 || !p.ready(Clock::now()) || !p.barrier || !p.active || (p.paste && !p.paste->snapshot.terminal())) return kvmux::SubmitResult::not_ready;
    for (std::size_t i = 0; i < normalized.size(); ++i) {
        const auto c = normalized[i];
        if (c >= 0x80 || (c == '\r' && (i + 1 == normalized.size() || normalized[++i] != '\n')) ||
            !(c == '\t' || c == '\n' || (c >= 0x20 && c <= 0x7e))) return kvmux::SubmitResult::not_ready;
    }
    std::random_device random;
    std::uint64_t id{};
    while (!id) id = (std::uint64_t(random()) << 32U) ^ random();
    Impl::PasteUpload job; job.bytes = std::move(normalized); job.epoch = p.control.epoch; job.intent = p.intent;
    job.snapshot = {PasteUploadState::uploading, id, static_cast<std::uint32_t>(job.bytes.size()), 0, 0, wire::PasteStatusReason::none};
    p.paste = std::move(job);
    return kvmux::SubmitResult::accepted;
}
void RelayClient::cancel_ascii_paste_text() noexcept {
    auto& p = *impl_; std::lock_guard lock(p.mutex);
    if (!p.paste || p.paste->snapshot.terminal()) return;
    p.paste->cancel_pending = true; p.paste->snapshot.state = PasteUploadState::canceled; p.paste->snapshot.reason = wire::PasteStatusReason::canceled; p.paste->bytes.clear();
}
PasteUploadSnapshot RelayClient::ascii_paste_text_snapshot() const { std::lock_guard lock(impl_->mutex); return impl_->paste ? impl_->paste->snapshot : PasteUploadSnapshot{}; }
AsciiPasteSnapshot NetworkControlSink::ascii_paste_snapshot() const {
    const auto paste = client_->ascii_paste_text_snapshot();
    AsciiPasteState state = AsciiPasteState::idle;
    // `complete` means the server has all uploaded bytes. Authorization and
    // commit still have to run, so every nonterminal transaction owns the
    // input lease until the server reports its terminal outcome.
    if (paste.state != PasteUploadState::idle && !paste.terminal()) state = AsciiPasteState::active;
    else if (paste.state == PasteUploadState::completed) state = AsciiPasteState::completed;
    else if (paste.state == PasteUploadState::canceled || paste.state == PasteUploadState::expired) state = AsciiPasteState::canceled;
    else if (paste.state == PasteUploadState::rejected) state = AsciiPasteState::failed;
    return {state, paste.total_bytes, paste.completed_bytes};
}
ControlSnapshot RelayClient::control_snapshot() const {
    auto& p = *impl_; std::lock_guard lock(p.mutex); auto result = p.control;
    if (p.cancel_inflight) { result.state = ControlConnectionState::clearing; result.release_confirmed = false; result.applied.known = false; }
    if (result.state == ControlConnectionState::ready && !p.fresh(Clock::now())) {
        result.state = ControlConnectionState::stalled; result.applied.known = false;
    }
    return result;
}
CaptureSnapshot RelayClient::capture_snapshot() const { std::lock_guard lock(impl_->mutex); return impl_->capture; }
ClientVideoSnapshot RelayClient::video_snapshot() const { std::lock_guard lock(impl_->mutex); return impl_->video; }
TrafficSnapshot RelayClient::traffic_snapshot() const { std::lock_guard lock(impl_->mutex); return impl_->traffic; }
std::optional<CaptureSample> RelayClient::take_sample() { std::lock_guard lock(impl_->mutex); return std::exchange(impl_->latest, std::nullopt); }
std::vector<DeviceInfo> NetworkCaptureSource::enumerate_devices() {
    DeviceInfo device; device.stable_id = "relay"; device.display_name = "LAN relay"; device.weak_match = true;
    return {device};
}
std::vector<CaptureMode> NetworkCaptureSource::enumerate_modes(const std::string&) {
    return {{"relay", 0, 0, {0,1}, PixelFormat::mjpeg, PixelFormat::mjpeg, "MJPEG"}};
}
} // namespace kvmux::relay
