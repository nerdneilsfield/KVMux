#include "relay_integration_fakes.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
}
std::vector<kvmux::EncodedAccessUnit> read_units(const char* path) {
    using namespace kvmux;
    std::ifstream in(path, std::ios::binary);
    assert(in.good());
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
    const auto size=bytes.size();
    bytes.resize(size+AV_INPUT_BUFFER_PADDING_SIZE);
    auto* parser=av_parser_init(AV_CODEC_ID_HEVC);
    auto* context=avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_HEVC));
    assert(parser && context);
    std::vector<EncodedAccessUnit> units;
    auto parse=[&](const std::uint8_t* data, int count) {
        std::uint8_t* packet=nullptr; int packet_size=0;
        const int used=av_parser_parse2(parser, context, &packet, &packet_size, data, count,
                                       AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        assert(used>=0 && (used || packet_size || !count));
        if (packet_size) {
            EncodedAccessUnit au;
            au.bytes.assign(packet, packet+packet_size);
            au.width=1920; au.height=1080; au.generation=7;
            au.capture_sequence=100+units.size(); au.pts_ns=static_cast<std::int64_t>(units.size())*16'666'667;
            au.idr=parser->key_frame==1;
            au.color_range=AVCOL_RANGE_MPEG; au.color_space=AVCOL_SPC_BT709;
            au.sample_aspect_ratio={4,3};
            units.push_back(std::move(au));
        }
        return used;
    };
    std::size_t offset=0;
    while (offset<size) offset+=parse(bytes.data()+offset, static_cast<int>(size-offset));
    parse(nullptr,0);
    av_parser_close(parser); avcodec_free_context(&context);
    return units;
}
struct RawCapture final : CaptureSource {
    std::atomic<std::uint64_t> sequence{};
    std::chrono::steady_clock::time_point previous{};
    std::vector<DeviceInfo> enumerate_devices() override{return {};}
    std::vector<CaptureMode> enumerate_modes(const std::string&) override{return {};}
    void start(const CaptureMode&) override{}
    void stop() noexcept override{}
    CaptureSnapshot snapshot() const override {
        CaptureSnapshot value;value.state=CaptureState::streaming;
        value.actual_mode={"synthetic",1920,1080,{50,1},PixelFormat::yuy2,PixelFormat::yuy2,"YUY2"};return value;
    }
    std::optional<CaptureSample> take_latest_sample() override {
        const auto now=std::chrono::steady_clock::now();
        if(now-previous<20ms)return {};previous=now;
        const std::vector<std::uint8_t> pixels(1920*1080*2,128);
        const PlaneLayout plane{0,3840,3840,1080};
        return CaptureSample::make_raw(7,(++sequence)*10,now,1920,1080,PixelFormat::yuy2,{&plane,1},pixels);
    }
};

struct FixtureEncoder final : VideoEncoder {
    std::shared_ptr<const std::vector<EncodedAccessUnit>> units;
    CodecConfig config;
    std::optional<EncoderInput> input;
    std::size_t index{};
    std::uint64_t sequence{};
    bool refresh{};
    explicit FixtureEncoder(std::shared_ptr<const std::vector<EncodedAccessUnit>> value) : units(std::move(value)) {}
    CodecResult configure(const CodecConfig& value) override {
        std::this_thread::sleep_for(350ms); config = value; index = 0; sequence = 0; return {};
    }
    CodecResult submit(const EncoderInput& value) override {
        assert(value.frame && value.frame->format == AV_PIX_FMT_NV12);
        if (input) return {CodecStatus::again,{}};
        input = value; return {};
    }
    CodecResult poll(EncodedAccessUnit& unit) override {
        if (!input) return {CodecStatus::again,{}};
        if (refresh) { index = 30; refresh = false; }
        unit = (*units)[index++]; if (index == units->size()) index = 0;
        unit.generation = config.generation; unit.encoded_sequence = ++sequence;
        unit.capture_sequence = input->capture_sequence; unit.arrival = input->arrival; unit.pts_ns = input->pts_ns;
        input.reset(); return {};
    }
    CodecResult request_keyframe() override { refresh = true; return {}; }
    CodecResult finish() override { return {}; }
    CodecResult reset() override { input.reset(); refresh = true; return {}; }
    void shutdown() noexcept override { input.reset(); }
    CodecBackend backend() const noexcept override { return CodecBackend::ffmpeg_software; }
    CodecDiagnostic diagnostic() const override { return {backend(),false,false,"Synthetic pre-encoded HEVC"}; }
};
void hevc_test(const char* path) {
    auto units = std::make_shared<const std::vector<EncodedAccessUnit>>(read_units(path));
    assert(units->size() == 60);
    RawCapture capture; FakeSerial serial; Ch9329ControlSink sink(serial.io()); sink.connect("fake",115200);
    assert(until([&] { return sink.snapshot().release_confirmed; }));
    kvmux::relay::RelayServer server(capture, sink, [units](CodecBackend, std::string&) { return std::make_unique<FixtureEncoder>(units); });
    kvmux::relay::ServerOptions options; options.bind_address = "127.0.0.1"; options.control_port = options.video_port = 0;
    options.codec = VideoCodec::hevc;
    std::string error;
    options.transport_bytes_per_second = 1;
    assert(server.start(options,error));
    {
        kvmux::relay::ClientOptions blocked_options;
        blocked_options.control_port = server.control_port(); blocked_options.video_port = server.video_port();
        GuiFixture blocked(blocked_options);
        assert(blocked.wait([&] { return server.snapshot().last_reason == kvmux::relay::MediaReason::frame_exceeds_rate_budget; }));
        blocked.run_for(200ms);
        assert(!blocked.session.snapshot().video_fresh);
        assert(server.snapshot().pacer.rejected_frames > 0);
        assert(std::string(kvmux::relay::media_reason_name(server.snapshot().last_reason)) == "frame_exceeds_rate_budget");
    }
    server.stop();
    options.transport_bytes_per_second = 12'000'000;
    assert(server.start(options,error)); // New generation and fresh IDR, no in-session configure.

    kvmux::relay::ClientOptions client_options; client_options.control_port = server.control_port(); client_options.video_port = server.video_port();
    UdpProxy proxy(client_options);
    client_options = proxy.options(); client_options.decoder_backend = CodecBackend::ffmpeg_software;
    GuiFixture gui(client_options);
    gui.run_for(250ms);
    assert(proxy.challenges >= 3); // Slow configure did not stop the network owner.
    gui.activate();
    assert(gui.client->video_snapshot().codec == VideoCodec::hevc);
    assert(proxy.last_presented > 0 && proxy.last_presented < capture.sequence * 10);
    assert(gui.wait([&] { return server.snapshot().feedback_samples >= 2; }));
    const auto before_loss = server.snapshot();
    const auto recovered = gui.client->video_snapshot().recoveries;
    proxy.drop_media_sequence = proxy.last_media_sequence + 3;
    assert(gui.wait([&] { return gui.client->video_snapshot().recoveries > recovered; }));
    assert(gui.wait([&] { return gui.session.snapshot().input_state == InputState::captured; }));
    assert(gui.wait([&] {
        const auto sender = server.snapshot();
        return sender.feedback.lost_frames > before_loss.feedback.lost_frames &&
            sender.admission_interval > sender.nominal_interval;
    })); // Actual receiver -> KCP -> server policy, not a policy-only fixture.
    assert(gui.client->video_snapshot().media.lost_frames > 0);
    const auto slowed = server.snapshot();
    gui.run_for(600ms);
    const auto admitted = server.snapshot().source_admissions - slowed.source_admissions;
    assert(admitted > 0 && admitted <= static_cast<std::uint64_t>(600000 / slowed.admission_interval.count()) + 2);
    // A forged/stale GUI ID must not move the proof high-water to that value.
    gui.client->video_presented(gui.client->capture_snapshot().generation, 99999999);
    gui.run_for(80ms); assert(proxy.last_presented < 99999999);
    gui.session.release_control();
}

void chunked_ascii_paste_loopback_test(const char* jpeg) {
    // Exercise the GUI path: KvmSession enables temporary intent, waits for the
    // sync barrier, then sends the normalized text through NetworkControlSink.
    RelayFixture relay(jpeg);
    { std::lock_guard lock(relay.serial.mutex); relay.serial.write_limit = 4096; }
    GuiFixture gui(relay.options());
    gui.activate();
    gui.session.release_control();
    assert(gui.wait([&] {
        const auto state = gui.session.snapshot();
        // A real user can start text only after the released preview has a
        // negotiated mode and fresh displayed video. Do not race the relay's
        // initial empty-to-actual-mode transition with the text intent.
        return state.input_state == InputState::preview && state.control.release_confirmed &&
            state.video_fresh && state.capture.state == CaptureState::streaming &&
            state.capture.actual_mode.width != 0 && state.capture.actual_mode.height != 0;
    }));

    constexpr std::size_t text_bytes = 961;
    const auto keyboard_reports_before = [&] {
        std::lock_guard lock(relay.serial.mutex);
        return static_cast<std::size_t>(std::count_if(relay.serial.received.begin(), relay.serial.received.end(),
            [](const auto& frame) { return frame.command == 0x02U; }));
    }();
    assert(gui.session.start_text_paste(std::string(text_bytes, 'a')));

    bool executing = false;
    bool active_before_serial = false;
    assert(gui.wait([&] {
        const auto paste = gui.client->ascii_paste_text_snapshot();
        executing = executing || paste.state == kvmux::relay::PasteUploadState::executing;
        // Authorization is after upload but before the server starts serial I/O.
        if (paste.state == kvmux::relay::PasteUploadState::authorizing ||
            paste.state == kvmux::relay::PasteUploadState::authorized) {
            active_before_serial = active_before_serial || gui.session.snapshot().text_paste_active;
        }
        return paste.state == kvmux::relay::PasteUploadState::completed;
    }, 20000ms));
    assert(active_before_serial);
    const auto paste = gui.client->ascii_paste_text_snapshot();
    assert(executing);
    assert(paste.total_bytes == text_bytes && paste.accepted_bytes == text_bytes &&
        paste.completed_bytes == text_bytes && paste.reason == kvmux::relay::wire::PasteStatusReason::none);
    {
        std::lock_guard lock(relay.serial.mutex);
        const auto keyboard_reports_after = static_cast<std::size_t>(std::count_if(
            relay.serial.received.begin(), relay.serial.received.end(),
            [](const auto& frame) { return frame.command == 0x02U; }));
        // 961 bytes cross the normalized 960-byte upload chunk boundary.
        // The temporary remote intent adds one synchronized empty keyboard report
        // before the job and one release report after it. The remaining reports
        // are exactly the two edges for each normalized character.
        assert(keyboard_reports_after >= keyboard_reports_before + 2 + text_bytes * 2);
    }
    // The relay-private authorization revision must not collide with the GUI revision stream.
    assert(gui.session.set_mouse_mode(MouseMode::relative));
    gui.activate();
    assert(gui.wait([&] {
        const auto applied = relay.sink.snapshot().applied;
        return applied.known && applied.state.mode == MouseMode::relative;
    }));
    gui.session.release_control();
}

void paste_keepalive_survives_blackholed_server_status_test(const char* jpeg) {
    RelayFixture relay(jpeg);
    { std::lock_guard lock(relay.serial.mutex); relay.serial.write_limit = 4096; relay.serial.hold_keyboard_ack = true; }
    UdpProxy proxy(relay.options());
    GuiFixture gui(proxy.options());
    gui.activate();
    assert(gui.session.start_text_paste("keepalive"));
    assert(gui.wait([&] {
        return gui.client->ascii_paste_text_snapshot().state == kvmux::relay::PasteUploadState::executing;
    }));

    // Drop every server KCP packet after execution begins, while client-to-server
    // KCP remains live. The server must keep the serial job alive past its 500 ms
    // lease using periodic K1 renewals, not reverse PasteStatus acknowledgements.
    proxy.drop_server_kcp = true;
    gui.run_for(750ms);
    assert(relay.sink.ascii_paste_snapshot().state == AsciiPasteState::active);

    proxy.drop_server_kcp = false;
    { std::lock_guard lock(relay.serial.mutex);
        relay.serial.hold_keyboard_ack = false;
        relay.serial.incoming.insert(relay.serial.incoming.end(), relay.serial.held_ack.begin(), relay.serial.held_ack.end());
        relay.serial.held_ack.clear();
    }
    assert(gui.wait([&] {
        return gui.client->ascii_paste_text_snapshot().state == kvmux::relay::PasteUploadState::completed;
    }, 5000ms));
    gui.session.release_control();
}

void first_frame_activation_test(const char* jpeg) {
    RelayFixture relay(jpeg);
    GuiFixture gui(relay.options(), true);
    bool injected = false;
    gui.after_session_tick = [&] {
        if (injected || !gui.session.snapshot().video_fresh) return;
        injected = true;
        assert(gui.session.snapshot().video.processed_frames == 0);
        // Complete decode/presentation after the tick cached a false video gate,
        // but before activate() checks readiness and sends its only click.
        gui.capture->hold = false;
        assert(until([&] { return gui.session.snapshot().video.processed_frames > 0; }));
        auto frame = gui.session.take_latest_frame(); assert(frame);
        gui.session.video_presented(frame->generation, frame->sequence);
        gui.presented_generation = frame->generation;
        gui.presented_arrival = frame->arrival;
        assert(until([&] {
            gui.client->gui_progress();
            const auto state = gui.session.snapshot();
            return state.control.state == ControlConnectionState::ready &&
                state.control.target_usb_ready && state.control.release_confirmed && state.video_fresh;
        }));
    };
    gui.activate();
    assert(injected);
    gui.session.release_control();
}

int main(int argc, char** argv) {
    assert(argc > 2);
    first_frame_activation_test(argv[1]);
    chunked_ascii_paste_loopback_test(argv[1]);
    paste_keepalive_survives_blackholed_server_status_test(argv[1]);
    hevc_test(argv[2]);
    RelayFixture relay(argv[1]); GuiFixture gui(relay.options()); gui.activate();
    gui.session.release_control();
    assert(gui.wait([&] { return gui.session.snapshot().input_state == InputState::preview; }));
    { std::lock_guard lock(relay.serial.mutex); relay.serial.hold_mouse_ack = true; }
    gui.session.handle_input({InputButton{InputMouseButton::left,true,50,50}});
    gui.session.handle_input({InputButton{InputMouseButton::left,false,50,50}});
    assert(gui.wait([&] { std::lock_guard lock(relay.serial.mutex); return !relay.serial.held_ack.empty(); }));
    assert(gui.session.snapshot().input_state == InputState::recovering);
    assert(!relay.sink.snapshot().applied.known);
    gui.session.handle_input({InputKey{0x05,true,false}});
    gui.session.focus_lost();
    { std::lock_guard lock(relay.serial.mutex);
        relay.serial.hold_mouse_ack = false;
        relay.serial.incoming.insert(relay.serial.incoming.end(), relay.serial.held_ack.begin(), relay.serial.held_ack.end());
        relay.serial.held_ack.clear();
    }
    assert(gui.wait([&] { return gui.session.snapshot().input_state == InputState::preview; }));
    gui.run_for(100ms); assert(gui.session.snapshot().input_state == InputState::preview);
    gui.session.handle_input({InputKey{0x05,false,false}});
    gui.activate();
    gui.session.handle_input({InputKey{0x04,true,false}});
    assert(gui.wait([&] {
        std::lock_guard lock(relay.serial.mutex);
        return std::any_of(relay.serial.received.begin(), relay.serial.received.end(), [](const auto& frame) {
            return frame.command == 2 && std::find(frame.data.begin(), frame.data.end(), 4) != frame.data.end();
        });
    }));
    const auto traffic = gui.client->traffic_snapshot();
    assert(traffic.video_received_bytes && traffic.control_received_bytes && traffic.control_sent_bytes);
    gui.session.handle_input({InputKey{0x04,false,false}});
    relay.capture.stalled = true; gui.run_for(650ms);
    assert(gui.session.snapshot().input_state == InputState::recovering);
    relay.capture.stalled = false;
    assert(gui.wait([&] { return gui.session.snapshot().input_state == InputState::captured; }));
    gui.session.handle_input({InputKey{0xe4,true,false}});
    assert(gui.wait([&] { return gui.session.snapshot().input_state == InputState::preview; }));
    assert(gui.session.send_special(SpecialKeys::host_key)); gui.run_for(300ms);
    assert(gui.session.snapshot().input_state == InputState::preview);
    {
        std::lock_guard lock(relay.serial.mutex);
        assert(std::any_of(relay.serial.received.begin(), relay.serial.received.end(), [](const auto& report) {
            return report.command == 2 && report.data.size() >= 2 && (report.data[0] & 0x10);
        }));
    }
    assert(gui.session.set_mouse_mode(MouseMode::relative));
    gui.activate();
    assert(relay.sink.snapshot().applied.known && relay.sink.snapshot().applied.state.mode == MouseMode::relative);
    gui.session.release_control();
}
