#include "network/relay_server.hpp"
#include "network/relay_session.hpp"
#include "network/source_admission.hpp"
#include "network/kcp_channel.hpp"
#include "network/media_codec_wire.hpp"
#include "network/relay_selection.hpp"
#include "input/us_ascii_text.hpp"
#include "video/video_processor.hpp"
extern "C" {
#include <libswscale/swscale.h>
}
#include <algorithm>
#include <utility>
#include <stdexcept>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <spdlog/spdlog.h>

namespace kvmux::relay {
namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
std::uint64_t random_id() {
    std::random_device random;
    std::uint64_t value{};
    while (!value) value = (std::uint64_t(random()) << 32U) ^ random();
    return value;
}
std::uint32_t milliseconds(Clock::time_point now) {
    return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}
}
struct RelayServer::Impl {
    CaptureSource& capture;
    Ch9329ControlSink& sink;
    EncoderFactory encoder_factory;
    ServerOptions options;
    std::optional<udp::Socket> control_socket, video_socket;
    std::atomic<bool> stopping{false};
    std::thread network_worker, media_worker;
    std::mutex media_mutex;
    std::condition_variable media_wake;
    // One ordered output slot. Credit is granted only when the pacer is empty.
    std::optional<MediaFrame> offer;
    std::uint64_t media_generation{};
    bool credit{}, keyframe{};
    std::string media_error;
    ServerSnapshot snapshot;

    Impl(CaptureSource& c, Ch9329ControlSink& s, EncoderFactory f)
        : capture(c), sink(s), encoder_factory(std::move(f)) {}

    void media_loop() {
        std::unique_ptr<VideoEncoder> codec;
        VideoProcessor processor;
        std::unique_ptr<SwsContext, decltype(&sws_freeContext)> scaler(nullptr, sws_freeContext);
        std::uint64_t generation{};
        bool waiting_idr = true;
        Clock::time_point next_source{};
        auto reset_codec = [&] { if (codec) { codec->shutdown(); codec.reset(); } };
        try {
            while (!stopping) {
                std::uint64_t wanted{};
                bool work{}, refresh{};
                std::chrono::microseconds interval;
                {
                    std::unique_lock lock(media_mutex);
                    media_wake.wait_for(lock, 2ms, [&] { return stopping || generation != media_generation || keyframe; });
                    if (stopping) break;
                    wanted = media_generation;
                    refresh = std::exchange(keyframe, false);
                    work = credit && !offer;
                    interval = snapshot.admission_interval;
                }
                if (generation != wanted) {
                    reset_codec(); generation = wanted; waiting_idr = true; next_source = {};
                    if (generation && options.codec == VideoCodec::hevc) {
                        std::string error;
                        codec = encoder_factory(options.encoder_backend, error);
                        if (!codec) throw std::runtime_error("HEVC encoder: " + error);
                        const auto mode = capture.snapshot().actual_mode;
                        CodecConfig settings;
                        settings.width = mode.width; settings.height = mode.height;
                        settings.fps_numerator = static_cast<std::uint32_t>(mode.frame_rate.numerator);
                        settings.fps_denominator = static_cast<std::uint32_t>(mode.frame_rate.denominator);
                        settings.bitrate = options.bitrate; settings.generation = generation;
                        auto result = codec->configure(settings);
                        if (!result.ok()) throw std::runtime_error(result.message);
                    }
                }
                if (!generation) continue;
                if (refresh && codec) {
                    waiting_idr = true;
                    const auto result = codec->request_keyframe();
                    if (!result.ok()) throw std::runtime_error(result.message);
                }
                if (!work) continue;
                auto publish = [&](MediaFrame frame) {
                    std::lock_guard lock(media_mutex);
                    if (media_generation == generation && !offer && credit) {
                        offer = std::move(frame); credit = false;
                    }
                };
                if (codec) {
                    EncodedAccessUnit unit;
                    const auto result = codec->poll(unit);
                    if (result.ok()) {
                        if (!unit.encoded_sequence || unit.generation != generation)
                            throw std::runtime_error("Invalid encoder sequence/generation");
                        if (waiting_idr && !unit.idr) continue;
                        auto bytes = encode_hevc(unit);
                        if (bytes.empty()) throw std::runtime_error("Invalid HEVC access unit");
                        waiting_idr = false;
                        publish({VideoCodec::hevc, generation, unit.encoded_sequence, unit.idr, std::move(bytes), unit.arrival});
                        continue; // Never overwrite an ordered output slot.
                    }
                    if (result.status != CodecStatus::again) throw std::runtime_error(result.message);
                }
                // Drain delayed codec output above even when source admission is slow.
                if (Clock::now() < next_source) continue;
                auto sample = capture.take_latest_sample();
                if (!sample || Clock::now() - sample->arrival >= 250ms) { std::this_thread::sleep_for(2ms); continue; }
                next_source = Clock::now() + interval;
                { std::lock_guard lock(media_mutex); ++snapshot.source_admissions; }
                if (!codec) {
                    auto bytes = encode_mjpeg(*sample);
                    if (bytes.empty()) throw std::runtime_error("Invalid MJPEG capture");
                    publish({VideoCodec::mjpeg, generation, sample->sequence, false, std::move(bytes), sample->arrival});
                    continue;
                }
                if (!sample->raw || sample->mjpeg) throw std::runtime_error("HEVC capture is not raw");
                auto frame = processor.process(*sample);
                if (!frame) throw std::runtime_error(processor.last_error());
                auto input = frame->frame;
                if (input->format != AV_PIX_FMT_NV12 && input->format != AV_PIX_FMT_YUV420P) {
                    AvFramePtr converted(av_frame_alloc(), [](AVFrame* f) { av_frame_free(&f); });
                    if (!converted) throw std::runtime_error("HEVC frame allocation failed");
                    converted->format = AV_PIX_FMT_NV12; converted->width = input->width; converted->height = input->height;
                    if (av_frame_get_buffer(converted.get(), 32) < 0 || av_frame_copy_props(converted.get(), input.get()) < 0)
                        throw std::runtime_error("HEVC frame storage allocation failed");
                    scaler.reset(sws_getCachedContext(scaler.release(), input->width, input->height,
                        static_cast<AVPixelFormat>(input->format), input->width, input->height, AV_PIX_FMT_NV12,
                        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr));
                    const int matrix = input->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_ITU601;
                    const bool rgb = input->format == AV_PIX_FMT_BGRA || input->format == AV_PIX_FMT_RGBA;
                    const int full = rgb || input->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
                    converted->color_range = full ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
                    converted->colorspace = matrix == SWS_CS_ITU709 ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
                    if (!scaler || sws_setColorspaceDetails(scaler.get(), sws_getCoefficients(matrix), full,
                        sws_getCoefficients(matrix), full, 0, 1<<16, 1<<16) < 0 ||
                        sws_scale(scaler.get(), input->data, input->linesize, 0, input->height,
                            converted->data, converted->linesize) != input->height)
                        throw std::runtime_error("HEVC conversion failed");
                    input = std::move(converted);
                }
                EncoderInput value;
                value.frame = std::move(input); value.generation = generation; value.capture_sequence = sample->sequence;
                value.arrival = sample->arrival;
                value.pts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(sample->arrival.time_since_epoch()).count();
                const auto result = codec->submit(value);
                if (!result.ok() && result.status != CodecStatus::again) throw std::runtime_error(result.message);
            }
        } catch (const std::exception& error) {
            std::lock_guard lock(media_mutex); media_error = error.what();
        }
        reset_codec();
    }

    void run() {
        ServerSession session(options.codec);
        std::unique_ptr<KcpChannel> kcp;
        std::unique_ptr<MediaPacer> pacer;
        std::optional<SourceAdmission> admission;
        udp::Endpoint peer;
        std::optional<wire::StateAck> ack;
        std::optional<wire::Status> status;
        std::optional<wire::PasteStatus> paste_status;
        std::vector<std::vector<std::uint8_t>> control_output;
        std::size_t control_index{};
        struct Pending { std::vector<std::uint8_t> body; Clock::time_point deadline; };
        std::optional<Pending> pending;
        struct Sent { std::uint64_t sequence{}; Clock::time_point arrival{}, completed{}; };
        std::deque<Sent> history;
        std::optional<Sent> sending;
        std::optional<udp::Datagram> pending_proof;
        Clock::time_point pending_proof_at{};
        auto status_at = Clock::time_point{};
        bool fatal{};
        auto request_refresh = [&] { std::lock_guard lock(media_mutex); keyframe = true; media_wake.notify_one(); };
        auto revoke = [&] { sink.cancel_ascii_paste(); sink.set_control_active(false); sink.release_all(); ack.reset(); };
        auto actions = [&](const SessionActions& batch) {
            for (const auto& action : batch) {
                switch (action.kind) {
                case SessionAction::Kind::send_raw:
                    if (control_socket->send_to(action.peer, action.datagram) == udp::SendStatus::error) fatal = true;
                    break;
                case SessionAction::Kind::established:
                    kcp = std::make_unique<KcpChannel>(session.tuple().conversation);
                    pacer = std::make_unique<MediaPacer>(options.codec, session.welcome().generation, options.transport_bytes_per_second, Clock::now());
                    { std::lock_guard lock(media_mutex);
                        const auto nominal = snapshot.nominal_interval;
                        snapshot = {}; snapshot.nominal_interval = snapshot.admission_interval = nominal;
                        snapshot.generation = session.welcome().generation;
                        snapshot.transport_bytes_per_second = options.transport_bytes_per_second;
                        admission.emplace(snapshot.generation, nominal);
                    }
                    history.clear(); sending.reset(); pending.reset(); pending_proof.reset(); ack.reset(); status_at = {};
                    control_output.clear(); control_index = 0;
                    revoke();
                    { std::lock_guard lock(media_mutex); media_generation = session.welcome().generation; offer.reset(); credit = true; keyframe = true; }
                    media_wake.notify_one();
                    break;
                case SessionAction::Kind::revoke_input: revoke(); break;
                case SessionAction::Kind::state_ack: ack = action.ack; break;
                case SessionAction::Kind::paste_ready:
                    paste_status = action.paste_status;
                    if (action.paste_status && (action.paste_status->state == wire::PasteState::canceled ||
                        action.paste_status->state == wire::PasteState::rejected || action.paste_status->state == wire::PasteState::expired)) sink.cancel_ascii_paste();
                    break;
                case SessionAction::Kind::expired:
                    admission.reset();
                    revoke(); kcp.reset(); pacer.reset(); pending.reset(); pending_proof.reset(); sending.reset(); history.clear(); ack.reset(); status.reset();
                    control_output.clear(); control_index = 0;
                    { std::lock_guard lock(media_mutex);
                        media_generation = 0; offer.reset(); credit = false;
                        const auto nominal = snapshot.nominal_interval;
                        snapshot = {}; snapshot.nominal_interval = snapshot.admission_interval = nominal;
                        snapshot.transport_bytes_per_second = options.transport_bytes_per_second;
                    }
                    media_wake.notify_one();
                    break;
                default: break;
                }
            }
        };
        while (!stopping && !fatal) {
            auto now = Clock::now();
            actions(session.tick(now));
            actions(session.update_control_snapshot(sink.snapshot(), now));
            std::vector<udp::Datagram> received;
            received.reserve(32);
            for (unsigned i = 0; i < 32; ++i) {
                auto input = control_socket->receive(i == 0 ? 1ms : 0ms);
                if (input.status == udp::ReceiveStatus::idle) break;
                if (input.status == udp::ReceiveStatus::error) { fatal = true; break; }
                if (input.status == udp::ReceiveStatus::datagram) received.push_back(std::move(input.datagram));
            }
            // Raw cancellation overtakes reliable edges already in this bounded batch.
            std::stable_sort(received.begin(), received.end(), [](const auto& a, const auto& b) {
                auto ea = wire::decode_envelope(a.bytes), eb = wire::decode_envelope(b.bytes);
                return (ea && ea->kind == wire::EnvelopeKind::cancel) > (eb && eb->kind == wire::EnvelopeKind::cancel);
            });
            for (const auto& packet : received) {
                now = Clock::now();
                auto envelope = wire::decode_envelope(packet.bytes);
                if (!envelope) continue;
                if (envelope->kind == wire::EnvelopeKind::kcp) {
                    if (kcp && session.matches(packet.source, envelope->tuple)) (void)kcp->input(envelope->body);
                    continue;
                }
                bool valid_video = false;
                if (envelope->kind == wire::EnvelopeKind::proof && session.matches(packet.source, envelope->tuple)) {
                    const auto proof = std::get<wire::Proof>(*wire::decode_raw(envelope->kind, envelope->body));
                    if (proof.active && proof.video_fresh && sending && sending->sequence == proof.presented_sequence) {
                        // Data can complete at the receiver before paced parity is
                        // locally sent. Defer one proof, never authorize an unsent
                        // frame; ServerSession still checks original issue time.
                        bool newer = true;
                        if (pending_proof) {
                            const auto previous = wire::decode_envelope(pending_proof->bytes);
                            const auto value = std::get<wire::Proof>(*wire::decode_raw(previous->kind, previous->body));
                            newer = proof.challenge > value.challenge;
                        }
                        if (newer) { pending_proof = packet; pending_proof_at = now; }
                        continue;
                    }
                    valid_video = std::any_of(history.begin(), history.end(), [&](const Sent& frame) {
                        return frame.sequence == proof.presented_sequence && now - frame.arrival < 500ms;
                    });
                }
                SessionIds ids;
                if (envelope->kind == wire::EnvelopeKind::hello && session.phase() == SessionPhase::idle) {
                    ids = {random_id(), random_id(), static_cast<std::uint32_t>(random_id())};
                    if (!ids.conversation) ids.conversation = 1;
                }
                actions(session.on_datagram(packet.source, packet.bytes, now, ids, valid_video));
                if (session.matches(packet.source, envelope->tuple)) peer = packet.source;
            }
            now = Clock::now();
            if (pending_proof && now - pending_proof_at >= 250ms) pending_proof.reset();
            if (pending_proof) {
                const auto envelope = wire::decode_envelope(pending_proof->bytes);
                const auto proof = std::get<wire::Proof>(*wire::decode_raw(envelope->kind, envelope->body));
                const bool complete = std::any_of(history.begin(), history.end(), [&](const Sent& frame) {
                    return frame.sequence == proof.presented_sequence && now - frame.arrival < 500ms;
                });
                if (complete || !sending || sending->sequence != proof.presented_sequence) {
                    auto packet = std::move(*pending_proof); pending_proof.reset();
                    actions(session.on_datagram(packet.source, packet.bytes, now, {}, complete));
                }
            }
            actions(session.tick(now));
            actions(session.update_control_snapshot(sink.snapshot(), now));
            const auto lease = session.control_deadline();
            if (lease && now < *lease) { sink.set_control_active(true); sink.update_ui_heartbeat(); }
            else sink.set_control_active(false);
            if (!kcp) continue;
            for (unsigned i = 0; i < 32; ++i) {
                auto bytes = kcp->receive(); if (!bytes) break;
                auto message = wire::decode_control(*bytes, wire::Direction::client_to_server);
                if (!message) continue;
                now = Clock::now();
                actions(session.tick(now));
                if (!kcp) break;
                if (auto value = std::get_if<wire::Cancel>(&*message)) actions(session.cancel(*value, now));
                else if (auto value = std::get_if<wire::Sync>(&*message)) {
                    if (session.check_sync(*value, now) == InputGate::allowed)
                        actions(session.sync_submitted(*value, sink.synchronize({value->epoch, value->intent, value->revision, value->state}), now));
                } else if (auto value = std::get_if<wire::Edge>(&*message)) {
                    auto gate = session.check_edge(*value, now);
                    if (gate == InputGate::allowed) actions(session.edge_submitted(*value,
                        sink.submit({value->epoch, value->source_sequence, now, value->payload}), now));
                    else if (gate == InputGate::recovery_required) actions(session.edge_submitted(*value, kvmux::SubmitResult::not_ready, now));
                } else if (auto value = std::get_if<wire::PasteBegin>(&*message)) {
                    actions(session.paste_begin(*value, now));
                } else if (auto value = std::get_if<wire::PasteChunk>(&*message)) {
                    actions(session.paste_chunk(*value, now));
                } else if (auto value = std::get_if<wire::PasteCommit>(&*message)) {
                    actions(session.paste_commit(*value, now));
                    if (auto bytes = session.pending_paste_bytes()) {
                        const auto mapped = map_us_ascii_text(std::string_view(
                            reinterpret_cast<const char*>(bytes->data()), bytes->size()));
                        if (!mapped) {
                            sink.cancel_ascii_paste(); actions(session.paste_start_failed(now));
                        } else {
                            AsciiPasteJob job;
                            job.gestures.reserve(mapped.gestures.size());
                            for (auto& gesture : mapped.gestures) job.gestures.push_back(std::move(gesture.edges));
                            if (sink.start_ascii_paste(std::move(job)) == kvmux::SubmitResult::accepted)
                                actions(session.paste_started(now));
                            else { sink.cancel_ascii_paste(); actions(session.paste_start_failed(now)); }
                        }
                    }
                } else if (auto value = std::get_if<wire::PasteCancel>(&*message)) {
                    actions(session.paste_cancel(*value, now));
                } else if (auto value = std::get_if<wire::PasteKeepalive>(&*message)) {
                    actions(session.paste_keepalive(*value, now));
                } else if (auto value = std::get_if<wire::MediaFeedback>(&*message)) {
                    if (admission && admission->feedback(value->generation, value->stats, now)) {
                        std::lock_guard lock(media_mutex); ++snapshot.feedback_samples;
                    }
                } else if (auto value = std::get_if<wire::RefreshRequest>(&*message)) {
                    if (value->generation == session.welcome().generation) request_refresh();
                }
                if (!kcp) break;
            }
            if (!kcp) continue;
            now = Clock::now();
            // Serial progress advances only after each HID report ACK. Poll it at
            // the same bounded cadence as the normal control status.
            if (now - status_at >= 50ms) actions(session.update_ascii_paste(sink.ascii_paste_snapshot(), now));
            if (now - status_at >= 50ms) {
                const auto value = sink.snapshot();
                status = wire::Status{value.epoch, value.state, value.target_usb_ready, value.release_confirmed, session.canceled_through(), value.ordinary_input_pending, value.completed_ordinary_sequence};
                status_at = now;
            }
            auto submit = [&](const wire::Control& value) {
                auto bytes = wire::encode_control(value, wire::Direction::server_to_client);
                return bytes && kcp->submit(*bytes) == SubmitResult::accepted;
            };
            if (ack && submit(*ack)) ack.reset();
            if (paste_status && submit(*paste_status)) paste_status.reset();
            if (status && submit(*status)) status.reset();
            kcp->update(milliseconds(now));
            if (kcp->failed()) { fatal = true; continue; }
            if (control_index == control_output.size()) { control_output = kcp->take_datagrams(); control_index = 0; }
            for (unsigned i = 0; i < 32 && control_index < control_output.size(); ++i) {
                auto bytes = wire::encode_envelope({wire::EnvelopeKind::kcp, session.tuple(), control_output[control_index]});
                auto result = control_socket->send_to(peer, *bytes);
                if (result == udp::SendStatus::would_block) break;
                if (result != udp::SendStatus::sent) { fatal = true; break; }
                ++control_index;
            }
            if (!pacer) continue;
            now = Clock::now();
            admission->poll(now);
            { std::lock_guard lock(media_mutex);
                snapshot.admission_interval = admission->interval();
                if (admission->latest()) snapshot.feedback = *admission->latest();
                snapshot.feedback_delta = admission->delta();
                snapshot.pacer = pacer->stats();
            }
            auto record_reason = [&](MediaReason reason) {
                std::lock_guard lock(media_mutex);
                if (snapshot.last_reason != reason)
                    spdlog::debug("Relay media reason={}", media_reason_name(reason));
                snapshot.last_reason = reason;
            };
            while (!history.empty() && (history.size() > 512 || now - history.front().arrival >= 500ms)) history.pop_front();
            auto expired = pacer->poll(now);
            if (expired.reason != MediaReason::none) { record_reason(expired.reason); sending.reset(); if (expired.needs_idr) request_refresh(); }
            if (pending && now >= pending->deadline) {
                auto result = pacer->discard(MediaReason::sender_deadline);
                record_reason(result.reason);
                pending.reset(); sending.reset(); if (result.needs_idr) request_refresh();
            }
            if (!pending && pacer->can_start(now)) {
                std::optional<MediaFrame> frame;
                { std::lock_guard lock(media_mutex); frame = std::exchange(offer, std::nullopt); }
                if (frame) {
                    Sent candidate{frame->sequence, frame->first_arrival, {}};
                    auto result = pacer->submit(std::move(*frame), now);
                    if (result.accepted) { sending = candidate; record_reason(MediaReason::none); }
                    else {
                        record_reason(result.reason);
                        if (result.needs_idr) request_refresh();
                    }
                }
            }
            // At most two media datagrams before returning to challenge/control service.
            for (unsigned i = 0; i < 2; ++i) {
                now = Clock::now();
                if (!pending) {
                    auto deadline = pacer->active_deadline();
                    auto body = pacer->next_datagram(now);
                    if (!body) break;
                    pending = Pending{std::move(*body), *deadline};
                }
                auto bytes = wire::encode_envelope({wire::EnvelopeKind::media, session.tuple(), pending->body});
                auto result = video_socket->send_to(peer, *bytes);
                if (result == udp::SendStatus::would_block) break;
                if (result != udp::SendStatus::sent) { fatal = true; break; }
                pending.reset();
                if (!pacer->active_deadline() && sending) {
                    sending->completed = Clock::now(); history.push_back(*sending); sending.reset();
                    if (history.size() > 512) history.pop_front();
                }
            }
            { std::lock_guard lock(media_mutex);
                if (!pending && pacer->can_start(Clock::now()) && !offer) { credit = true; media_wake.notify_one(); }
                if (!media_error.empty()) { spdlog::error("Relay media: {}", media_error); fatal = true; }
            }
        }
        revoke();
        { std::lock_guard lock(media_mutex); media_generation = 0; credit = false; offer.reset(); }
        media_wake.notify_one();
    }
};
RelayServer::RelayServer(CaptureSource& c, Ch9329ControlSink& s, EncoderFactory f)
    : impl_(std::make_unique<Impl>(c, s, std::move(f))) {}
RelayServer::~RelayServer() { stop(); }
bool RelayServer::start(const ServerOptions& options, std::string& error) {
    auto& p = *impl_;
    if (p.network_worker.joinable()) { error = "Relay already running"; return false; }
    error.clear();
    if ((options.control_port && options.control_port == options.video_port) || !options.transport_bytes_per_second || options.transport_bytes_per_second > 1'000'000'000) {
        error = "UDP ports must differ and transport rate must be 1..1000000000 bytes/s"; return false;
    }
    const auto mode = p.capture.snapshot().actual_mode;
    try { select_mode(std::span<const CaptureMode>(&mode, 1), 0, options.codec); }
    catch (const std::exception& e) { error = e.what(); return false; }
    if (options.codec == VideoCodec::hevc && (!options.bitrate || options.bitrate > 100'000'000)) {
        error = "HEVC bitrate must be 1..100000000 bits/s"; return false;
    }
    p.options = options;
    { std::lock_guard lock(p.media_mutex);
        p.snapshot = {};
        p.snapshot.transport_bytes_per_second = options.transport_bytes_per_second;
        p.snapshot.nominal_interval = p.snapshot.admission_interval = std::chrono::microseconds(
            std::max<std::int64_t>(1, 1'000'000LL * mode.frame_rate.denominator / mode.frame_rate.numerator));
    }
    p.control_socket = udp::Socket::bind(options.bind_address, options.control_port, error);
    if (!p.control_socket) return false;
    p.video_socket = udp::Socket::bind(options.bind_address, options.video_port, error);
    if (!p.video_socket) { p.control_socket.reset(); return false; }
    p.stopping = false; p.media_error.clear();
    p.media_worker = std::thread([&p] { p.media_loop(); });
    p.network_worker = std::thread([&p] { p.run(); });
    return true;
}
void RelayServer::stop() noexcept {
    auto& p = *impl_;
    p.stopping = true; p.media_wake.notify_all();
    if (p.network_worker.joinable()) p.network_worker.join();
    if (p.media_worker.joinable()) p.media_worker.join();
    p.sink.set_control_active(false); p.sink.release_all();
    p.control_socket.reset(); p.video_socket.reset();
}
ServerSnapshot RelayServer::snapshot() const { std::lock_guard lock(impl_->media_mutex); return impl_->snapshot; }
std::uint16_t RelayServer::control_port() const { return impl_->control_socket ? impl_->control_socket->local_port().value_or(0) : 0; }
std::uint16_t RelayServer::video_port() const { return impl_->video_socket ? impl_->video_socket->local_port().value_or(0) : 0; }
} // namespace kvmux::relay
