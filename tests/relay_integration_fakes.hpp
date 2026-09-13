#pragma once
#include "relay_test_fakes.hpp"
#include "network/relay_client.hpp"
#include "network/relay_session.hpp"
#include "network/udp_socket.hpp"
#include "app/kvm_session.hpp"
#include <cassert>
#include <fstream>
#include <thread>
#include <functional>
#include <cstdio>

inline std::vector<std::uint8_t> fixture_bytes(const char* path) {
    std::ifstream input(path, std::ios::binary); assert(input);
    return {std::istreambuf_iterator<char>(input), {}};
}
inline bool until(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do { if (condition()) return true; std::this_thread::sleep_for(2ms); } while (std::chrono::steady_clock::now() < deadline);
    return condition();
}
struct RelayFixture {
    FakeSerial serial;
    FakeCapture capture;
    Ch9329ControlSink sink{serial.io()};
    kvmux::relay::RelayServer server{capture, sink};
    explicit RelayFixture(const char* jpeg, std::uint64_t rate = 12000000) {
        capture.jpeg = fixture_bytes(jpeg); sink.connect("fake", 115200);
        assert(until([&] { return sink.snapshot().release_confirmed; }));
        kvmux::relay::ServerOptions options; options.bind_address = "127.0.0.1"; options.control_port = options.video_port = 0; options.transport_bytes_per_second = rate;
        std::string error; assert(server.start(options, error));
    }
    kvmux::relay::ClientOptions options() const {
        kvmux::relay::ClientOptions options; options.control_port = server.control_port(); options.video_port = server.video_port(); return options;
    }
};
// Native two-front-port proxy. A single backend socket preserves the client's
// endpoint identity across both server sockets. Impairment drops whole datagrams.
struct UdpProxy {
    kvmux::udp::Socket control, video, backend;
    kvmux::udp::Endpoint server_control, server_video;
    std::optional<kvmux::udp::Endpoint> client;
    std::atomic<bool> blackout{}, drop_server_kcp{}, done{};
    std::atomic<unsigned> drop_welcome{}, drop_ready{};
    std::atomic<bool> wrong_source_only{}, wrong_tuple_only{};
    std::atomic<std::uint64_t> session_id{}, challenges{}, proofs{}, media{}, last_presented{}, drop_media_sequence{}, last_media_sequence{};
    std::thread worker;
    explicit UdpProxy(kvmux::relay::ClientOptions options) {
        std::string error;
        control = std::move(*kvmux::udp::Socket::bind("127.0.0.1", 0, error));
        video = std::move(*kvmux::udp::Socket::bind("127.0.0.1", 0, error));
        backend = std::move(*kvmux::udp::Socket::bind("127.0.0.1", 0, error));
        server_control = *kvmux::udp::resolve("127.0.0.1", options.control_port, error);
        server_video = *kvmux::udp::resolve("127.0.0.1", options.video_port, error);
        worker = std::thread([this] {
            while (!done) {
                for (unsigned i = 0; i < 32; ++i) {
                    auto packet = control.receive(i ? 0ms : 1ms);
                    if (packet.status != kvmux::udp::ReceiveStatus::datagram) break;
                    client = packet.datagram.source;
                    const auto envelope = kvmux::relay::wire::decode_envelope(packet.datagram.bytes);
                    if (envelope && envelope->kind == kvmux::relay::wire::EnvelopeKind::proof) {
                        ++proofs;
                        last_presented = std::get<kvmux::relay::wire::Proof>(*kvmux::relay::wire::decode_raw(envelope->kind, envelope->body)).presented_sequence;
                    }
                    if (!blackout) (void)backend.send_to(server_control, packet.datagram.bytes);
                }
                for (unsigned i = 0; i < 64; ++i) {
                    auto packet = backend.receive(0ms);
                    if (packet.status != kvmux::udp::ReceiveStatus::datagram) break;
                    const auto envelope = kvmux::relay::wire::decode_envelope(packet.datagram.bytes);
                    if (!envelope || !client) continue;
                    using Kind = kvmux::relay::wire::EnvelopeKind;
                    if (envelope->kind == Kind::ready) { session_id = envelope->tuple.session; if (drop_ready) { --drop_ready; continue; } }
                    if (envelope->kind == Kind::welcome && drop_welcome) { --drop_welcome; continue; }
                    if (envelope->kind == Kind::challenge) ++challenges;
                    if (envelope->kind == Kind::media) {
                        ++media;
                        std::uint64_t sequence{};
                        for (unsigned n = 12; n < 20; ++n) sequence = (sequence << 8U) | envelope->body[n];
                        last_media_sequence = sequence;
                        if (sequence == drop_media_sequence) continue;
                    }
                    if (blackout || (drop_server_kcp && envelope->kind == Kind::kcp)) continue;
                    if (packet.datagram.source == server_control) (void)control.send_to(*client, packet.datagram.bytes);
                    else if (packet.datagram.source == server_video) {
                        if (wrong_tuple_only) packet.datagram.bytes[15] ^= 0x01;
                        if (wrong_source_only) (void)control.send_to(*client, packet.datagram.bytes);
                        else (void)video.send_to(*client, packet.datagram.bytes);
                    }
                }
            }
        });
    }
    ~UdpProxy() { done = true; if (worker.joinable()) worker.join(); }
    kvmux::relay::ClientOptions options() const {
        kvmux::relay::ClientOptions options; options.control_port = *control.local_port(); options.video_port = *video.local_port(); return options;
    }
};
struct GatedNetworkCapture final : CaptureSource {
    std::shared_ptr<kvmux::relay::RelayClient> client;
    std::atomic<bool> hold{false};
    explicit GatedNetworkCapture(std::shared_ptr<kvmux::relay::RelayClient> value) : client(std::move(value)) {}
    std::vector<DeviceInfo> enumerate_devices() override { return {}; }
    std::vector<CaptureMode> enumerate_modes(const std::string&) override { return {}; }
    void start(const CaptureMode&) override { client->start(); }
    void stop() noexcept override { client->stop(); }
    CaptureSnapshot snapshot() const override { return client->capture_snapshot(); }
    std::optional<CaptureSample> take_latest_sample() override { return hold ? std::nullopt : client->take_sample(); }
};
struct GuiFixture {
    std::shared_ptr<kvmux::relay::RelayClient> client;
    GatedNetworkCapture* capture{};
    KvmSession session;
    std::uint64_t presented_generation{};
    std::chrono::steady_clock::time_point presented_arrival{};
    std::function<void()> after_session_tick;
    explicit GuiFixture(kvmux::relay::ClientOptions options, bool hold_samples = false)
        : client(std::make_shared<kvmux::relay::RelayClient>(options)),
          session([&] {
              auto source = std::make_unique<GatedNetworkCapture>(client);
              source->hold = hold_samples; capture = source.get(); return source;
          }(), std::make_unique<kvmux::relay::NetworkControlSink>(client)) {
        DeviceInfo device; device.stable_id = "relay"; device.weak_match = true;
        CaptureMode mode{"relay",0,0,{0,1},PixelFormat::mjpeg,PixelFormat::mjpeg,"MJPEG"};
        assert(session.select_capture(device, mode)); session.set_video_rect({0,0,100,100});
    }
    void tick() {
        session.tick();
        if (after_session_tick) after_session_tick();
        if (auto frame = session.take_latest_frame()) {
            session.video_presented(frame->generation, frame->sequence);
            presented_generation = frame->generation;
            presented_arrival = frame->arrival;
        }
    }
    bool wait(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 3000ms) {
        return until([&] { tick(); return condition(); }, timeout);
    }
    void run_for(std::chrono::milliseconds duration) {
        const auto end = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < end) { tick(); std::this_thread::sleep_for(2ms); }
    }
    void activate() {
        // Capture freshness can precede decode/presentation. Observe a displayed
        // frame before the tick that updates InputRouter's cached video gate.
        assert(until([&] {
            const auto before = session.snapshot();
            const bool displayed = presented_generation == before.capture.generation &&
                presented_arrival != std::chrono::steady_clock::time_point{} &&
                std::chrono::steady_clock::now() - presented_arrival < 500ms &&
                before.video.processed_frames > 0;
            tick();
            const auto state = session.snapshot();
            return displayed && presented_generation == state.capture.generation &&
                state.control.state == ControlConnectionState::ready &&
                state.control.target_usb_ready && state.control.release_confirmed && state.video_fresh;
        }));
        session.handle_input({InputButton{InputMouseButton::left, true, 50,50}});
        session.handle_input({InputButton{InputMouseButton::left, false, 50,50}});
        const bool captured = wait([&] { return session.snapshot().input_state == InputState::captured; });
        if (!captured) { const auto state = session.snapshot(); std::fprintf(stderr,"activation input=%d control=%d epoch=%llu applied=%d rev=%llu fresh=%d error=%s\n",int(state.input_state),int(state.control.state),(unsigned long long)state.control.epoch,state.control.applied.known,(unsigned long long)state.control.applied.revision,state.video_fresh,state.control.error.c_str()); }
        assert(captured);
    }
};

namespace kvmux { std::unique_ptr<CaptureSource> create_platform_capture_source() { return std::make_unique<FakeCapture>(); } }
