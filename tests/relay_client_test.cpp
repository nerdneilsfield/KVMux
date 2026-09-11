#include "network/relay_client.hpp"
#include "app/kvm_session.hpp"
#include "network/relay_protocol.hpp"
#include "video/video_pipeline.hpp"
#include "video/video_processor.hpp"
#include "relay_test_fakes.hpp"
#include <atomic>
#include <fstream>
#include <stdexcept>
#include <thread>
extern "C" {
#include <libavcodec/avcodec.h>
}
using namespace kvmux;
using namespace kvmux::relay;
using namespace std::chrono_literals;
namespace kvmux {
// This test always injects its relay source. Never open a platform device.
std::unique_ptr<CaptureSource> create_platform_capture_source() {
    throw std::runtime_error("platform capture is forbidden in relay client tests");
}
}
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::vector<kvmux::EncodedAccessUnit> read_units(const char* path) {
    using namespace kvmux;
    std::ifstream in(path, std::ios::binary);
    require(in.good(), "open HEVC test fixture");
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
    const auto size=bytes.size();
    bytes.resize(size+AV_INPUT_BUFFER_PADDING_SIZE);
    auto* parser=av_parser_init(AV_CODEC_ID_HEVC);
    auto* context=avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_HEVC));
    require(parser && context, "HEVC parser allocation");
    std::vector<EncodedAccessUnit> units;
    auto parse=[&](const std::uint8_t* data, int count) {
        std::uint8_t* packet=nullptr; int packet_size=0;
        const int used=av_parser_parse2(parser, context, &packet, &packet_size, data, count,
                                       AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        require(used>=0 && (used || packet_size || !count), "HEVC parse progress");
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
void hevc_test(const char* fixture);
void integrated_test(const std::vector<std::uint8_t>& jpeg);
void session_stall_test(const std::vector<std::uint8_t>& jpeg);
void startup_grace_test(const std::vector<std::uint8_t>& jpeg);
void delayed_status_test(const std::vector<std::uint8_t>& jpeg, bool active = false);
int main(int argc, char** argv) {
    require(argc == 3, "JPEG and HEVC fixtures required");
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<std::uint8_t> jpeg((std::istreambuf_iterator<char>(input)), {});
    std::string error;
    auto controls = tcp::Listener::bind("127.0.0.1", 0, error);
    auto videos = tcp::Listener::bind("127.0.0.1", 0, error);
    require(controls && videos, "bind");
    std::atomic<bool> done{}, release_seen{}, stale_sent{}, cleared{}, input_seen{};
    std::jthread server([&] {
        auto socket = controls->accept(2s);
        if (!socket) return;
        Status status; status.session = 42; status.control.epoch = 5;
        status.control.state = ControlConnectionState::ready;
        status.control.target_usb_ready = status.control.release_confirmed = true;
        if (!send_packet(*socket, PacketType::hello, encode_hello({42, VideoCodec::mjpeg})) ||
            !send_packet(*socket, PacketType::status, encode_status(status))) return;
        while (!done) {
            auto packet = receive_packet(*socket, 400ms);
            if (!packet) break;
            if (packet->type == PacketType::release) {
                release_seen = true;
                // Deliberately send old ready state before acknowledging the new epoch.
                stale_sent = true;
            } else if (release_seen && cleared) { status.control.epoch = 6; }
            if (packet->type == PacketType::control) {
                auto event = decode_session_control(packet->payload);
                input_seen = event && event->session == 42 && event->event.epoch == 6;
            }
            if (!send_packet(*socket, PacketType::status, encode_status(status))) break;
        }
        done = true;
    });
    std::jthread video([&] {
        auto socket = videos->accept(2s);
        if (!socket) return;
        auto hello = receive_packet(*socket, 500ms);
        if (!hello || !decode_hello(hello->payload) || decode_hello(hello->payload)->session != 42) return;
        std::uint64_t sequence = 0;
        while (!done) {
            auto sample = CaptureSample::make_mjpeg(1, ++sequence, std::chrono::steady_clock::now(), 16, 16, jpeg);
            if (!sample || !send_packet(*socket, PacketType::video_mjpeg, encode_mjpeg(*sample))) break;
            std::this_thread::sleep_for(10ms);
        }
    });
    auto client = std::make_shared<RelayClient>(ClientOptions{"127.0.0.1", *controls->local_port(), *videos->local_port()});
    NetworkCaptureSource source(client);
    NetworkControlSink sink(client);
    client->gui_progress(); source.start({});
    VideoPipeline pipeline(source); pipeline.start(source.snapshot().generation);
    auto pump = [&](auto predicate) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!predicate() && std::chrono::steady_clock::now() < deadline) {
            if (auto frame = pipeline.take_latest_frame()) sink.video_presented(frame->sequence);
            sink.update_ui_heartbeat(); std::this_thread::sleep_for(5ms);
        }
        return predicate();
    };
    require(pump([&] { return pipeline.snapshot().processed_frames > 0 && sink.snapshot().state == ControlConnectionState::ready; }), "network JPEG decode and ready");
    sink.release_all();
    require(!sink.snapshot().release_confirmed, "release immediate readiness fence");
    require(pump([&] { return stale_sent.load(); }), "release transmitted");
    std::this_thread::sleep_for(30ms);
    require(!sink.snapshot().release_confirmed && sink.snapshot().state != ControlConnectionState::ready, "old ready status cannot acknowledge release");
    cleared = true;
    require(pump([&] { return sink.snapshot().epoch == 6 && sink.snapshot().release_confirmed; }), "new epoch ready");
    sink.set_control_active(true); sink.update_ui_heartbeat();
    require(sink.submit({5,1,{},KeyEdge{4,true}}) == SubmitResult::not_ready, "old epoch rejected");
    require(sink.submit({6,2,{},KeyEdge{4,true}}) == SubmitResult::accepted, "current input accepted");
    require(sink.submit({6,2,{},KeyEdge{4,true}}) == SubmitResult::not_ready, "duplicate sequence rejected");
    require(pump([&] { return input_seen.load(); }), "input crosses actual socket");
    // GUI loss revokes input, not the background Preview connection.
    std::this_thread::sleep_for(650ms);
    require(sink.snapshot().state != ControlConnectionState::disconnected,
        "GUI expiry preserves network connection");
    sink.update_ui_heartbeat(); sink.set_control_active(true);
    require(sink.submit({6,3,{},KeyEdge{5,true}}) == SubmitResult::not_ready,
        "returning GUI and repeated active cannot restore old capture");
    source.stop(); pipeline.stop(); done = true;
    server.join(); video.join();
    integrated_test(jpeg);
    session_stall_test(jpeg);
    startup_grace_test(jpeg);
    delayed_status_test(jpeg);
    delayed_status_test(jpeg, true);
    hevc_test(argv[2]);
}

void integrated_test(const std::vector<std::uint8_t>& jpeg) {
    FakeSerial serial; Ch9329ControlSink hardware(serial.io()); hardware.connect("fake", 57600);
    FakeCapture capture; capture.jpeg = jpeg;
    RelayServer server(capture, hardware); std::string error;
    require(server.start({"127.0.0.1",0,0}, error), "actual relay listen");
    auto client = std::make_shared<RelayClient>(ClientOptions{"127.0.0.1",server.control_port(),server.video_port()});
    NetworkCaptureSource source(client); NetworkControlSink sink(client);
    client->gui_progress(); source.start({});
    VideoPipeline pipeline(source); pipeline.start(source.snapshot().generation);
    auto pump = [&](auto predicate) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!predicate() && std::chrono::steady_clock::now() < deadline) {
            if (auto frame = pipeline.take_latest_frame()) sink.video_presented(frame->sequence);
            sink.update_ui_heartbeat(); std::this_thread::sleep_for(5ms);
        }
        return predicate();
    };
    require(pump([&] {return sink.snapshot().state == ControlConnectionState::ready && pipeline.snapshot().processed_frames > 0;}), "actual client/server ready");
    // Activate, then let the GUI lease reach the server before emitting a key.
    sink.set_control_active(true);
    auto activated = std::chrono::steady_clock::now() + 80ms;
    require(pump([&] {return std::chrono::steady_clock::now() >= activated;}), "active heartbeat");
    require(sink.submit({sink.snapshot().epoch,1,{},KeyEdge{4,true}}) == SubmitResult::accepted, "actual key queued");
    require(pump([&] {std::lock_guard lock(serial.mutex); return std::ranges::any_of(serial.received, [](const auto& f){return f.command==2 && f.data.size()==8 && f.data[2]==4;});}), "actual client/server emits HID");
    const auto epoch = sink.snapshot().epoch; sink.release_all();
    require(!sink.snapshot().release_confirmed, "actual release fenced");
    require(pump([&] {return sink.snapshot().epoch > epoch && sink.snapshot().release_confirmed;}), "actual release confirmed new epoch");
    source.stop(); pipeline.stop(); server.stop(); hardware.disconnect();
}

void startup_grace_test(const std::vector<std::uint8_t>& jpeg) {
    std::string error;
    auto controls = tcp::Listener::bind("127.0.0.1", 0, error);
    auto videos = tcp::Listener::bind("127.0.0.1", 0, error);
    require(controls && videos, "startup listeners");
    std::atomic<bool> done{}, active_seen{};
    std::atomic<std::uint64_t> control_received_bytes{}, control_sent_bytes{};
    std::jthread server([&] {
        auto socket = controls->accept(2s);
        if (!socket) return;
        Status status; status.session = 42; status.control.epoch = 1;
        status.control.state = ControlConnectionState::ready;
        status.control.target_usb_ready = status.control.release_confirmed = true;
        if (!send_packet(*socket, PacketType::hello, encode_hello({42, VideoCodec::mjpeg})) ||
            !send_packet(*socket, PacketType::status, encode_status(status))) return;
        control_sent_bytes = 12U + encode_hello({42, VideoCodec::mjpeg}).size() + 12U + encode_status(status).size();
        while (!done) {
            auto packet = receive_packet(*socket, 400ms);
            if (!packet) break;
            control_received_bytes += 12U + packet->payload.size();
            if (packet->type == PacketType::heartbeat) {
                const auto heartbeat = decode_heartbeat(packet->payload);
                if (heartbeat && heartbeat->gui_active) active_seen = true;
            }
            if (!send_packet(*socket, PacketType::status, encode_status(status))) break;
            control_sent_bytes += 12U + encode_status(status).size();
        }
    });
    std::jthread video([&] {
        auto socket = videos->accept(2s);
        if (!socket || !receive_packet(*socket, 500ms)) return;
        std::uint64_t sequence{};
        while (!done) {
            auto sample = CaptureSample::make_mjpeg(1, ++sequence, std::chrono::steady_clock::now(), 16, 16, jpeg);
            if (!sample || !send_packet(*socket, PacketType::video_mjpeg, encode_mjpeg(*sample))) break;
            std::this_thread::sleep_for(10ms);
        }
    });
    RelayClient client({"127.0.0.1", *controls->local_port(), *videos->local_port()});
    const auto empty = client.traffic_snapshot();
    require(empty.video_received_bytes == 0 && empty.control_received_bytes == 0 && empty.control_sent_bytes == 0,
        "new client traffic starts at zero");
    const auto started = std::chrono::steady_clock::now();
    client.start(); // No GUI progress before the handshake, as in the real GUI.
    client.active(true);
    client.video_presented(1);
    const auto deadline = started + 2s;
    while (client.control_snapshot().state == ControlConnectionState::opening &&
           std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    require(client.control_snapshot().state == ControlConnectionState::ready, "handshake survives before first GUI tick");
    require(client.submit({1,1,{},KeyEdge{4,true}}) == SubmitResult::not_ready, "startup grace cannot authorize input");
    std::this_thread::sleep_for(650ms);
    require(client.control_snapshot().state == ControlConnectionState::ready,
        "Preview survives without GUI progress");
    require(!active_seen, "startup grace never grants an active lease");
    client.stop(); done = true;
    server.join(); video.join();
    const auto traffic = client.traffic_snapshot();
    const auto sample = CaptureSample::make_mjpeg(1, 1, std::chrono::steady_clock::now(), 16, 16, jpeg);
    require(sample.has_value(), "traffic sample");
    const auto video_packet_bytes = 12U + encode_mjpeg(*sample).size();
    require(traffic.video_received_bytes > 0 && traffic.video_received_bytes % video_packet_bytes == 0,
        "video traffic counts complete packets including headers");
    require(traffic.control_sent_bytes > 0 && traffic.control_sent_bytes == control_received_bytes,
        "control sent traffic matches peer received packets");
    require(traffic.control_received_bytes == control_sent_bytes,
        "control received traffic includes handshake and status headers");
    client.stop();
    const auto stopped = client.traffic_snapshot();
    require(stopped.video_received_bytes == traffic.video_received_bytes &&
        stopped.control_received_bytes == traffic.control_received_bytes && stopped.control_sent_bytes == traffic.control_sent_bytes,
        "stop preserves cumulative traffic");
}


// A delayed/fragmented return path must not stop Preview heartbeats. Keep
// actual TCP video and GUI ticks alive; no device or hardware connection.
void delayed_status_test(const std::vector<std::uint8_t>& jpeg, bool active) {
    std::string error;
    auto controls = tcp::Listener::bind("127.0.0.1", 0, error);
    auto videos = tcp::Listener::bind("127.0.0.1", 0, error);
    require(controls && videos, "delayed status listeners");
    std::atomic<bool> done{}, delayed_sent{}, expired{}, inactive_only{true}, reactivated{};
    std::atomic<unsigned> heartbeats{};
    std::jthread server([&] {
        auto socket = controls->accept(1s);
        if (!socket) return;
        Status status; status.session = 42; status.control.epoch = 1;
        status.control.state = ControlConnectionState::ready;
        status.control.target_usb_ready = status.control.release_confirmed = true;
        if (!send_packet(*socket, PacketType::hello, encode_hello({42, VideoCodec::mjpeg})) ||
            !send_packet(*socket, PacketType::status, encode_status(status))) return;
        auto first = receive_packet(*socket, 250ms);
        if (!first || first->type != PacketType::heartbeat) return;
        ++heartbeats;
        auto last = std::chrono::steady_clock::now();
        std::jthread delayed([&] {
            // Split the header as well as the packet, so the reader must retain
            // partial input while the writer continues renewing the lease.
            const auto bytes = encode_packet(PacketType::status, encode_status(status));
            if (!socket->send_all(std::span(bytes).first(5), 50ms)) return;
            std::this_thread::sleep_for(280ms);
            if (!socket->send_all(std::span(bytes).subspan(5), 50ms)) return;
            delayed_sent = true;
        });
        while (!done) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(last + 250ms - std::chrono::steady_clock::now());
            auto packet = left > 0ms ? receive_packet(*socket, left) : std::nullopt;
            if (!packet) { expired = true; break; }
            if (packet->type == PacketType::heartbeat) {
                auto heartbeat = decode_heartbeat(packet->payload);
                if (!heartbeat || heartbeat->gui_active) inactive_only = false;
                if (delayed_sent && heartbeat && heartbeat->gui_active) reactivated = true;
                ++heartbeats; last = std::chrono::steady_clock::now();
            }
            // After the delayed write completes, resume ordinary responses.
            if (delayed_sent && !send_packet(*socket, PacketType::status, encode_status(status))) break;
        }
    });
    std::jthread video([&] {
        auto socket = videos->accept(1s);
        if (!socket || !receive_packet(*socket, 500ms)) return;
        std::uint64_t sequence{};
        while (!done) {
            auto sample = CaptureSample::make_mjpeg(1, ++sequence, std::chrono::steady_clock::now(), 16, 16, jpeg);
            if (!sample || !send_packet(*socket, PacketType::video_mjpeg, encode_mjpeg(*sample))) break;
            std::this_thread::sleep_for(10ms);
        }
    });
    RelayClient client({"127.0.0.1", *controls->local_port(), *videos->local_port()});
    client.start();
    client.gui_progress();
    client.active(active);
    const auto started = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - started < 450ms) {
        client.gui_progress();
        std::this_thread::sleep_for(5ms);
    }
    const auto expected_state = active ? ControlConnectionState::stalled : ControlConnectionState::ready;
    const bool survived = delayed_sent && !expired && heartbeats >= 6 &&
        client.control_snapshot().state == expected_state;
    // Inactive transport heartbeats continue without granting an input lease.
    std::this_thread::sleep_for(650ms);
    const auto snapshot = client.control_snapshot();
    const bool transport_alive = !expired;
    client.stop(); done = true; server.join(); video.join();
    require(survived, "280ms fragmented status must not starve 250ms Preview heartbeat lease");
    require(active || inactive_only, "Preview heartbeats never authorize control");
    require(!reactivated, "status recovery must not restore old active authorization");
    require(transport_alive && snapshot.state == expected_state,
        "GUI expiry preserves inactive Preview transport");
}

// Exercise the real per-tick active(true) caller, input router, TCP relay and
// serial safety worker. Only capture frames and serial I/O are simulated.
void session_stall_test(const std::vector<std::uint8_t>& jpeg) {
    FakeSerial serial; Ch9329ControlSink hardware(serial.io()); hardware.connect("fake", 57600);
    FakeCapture capture; capture.jpeg = jpeg;
    RelayServer server(capture, hardware); std::string error;
    require(server.start({"127.0.0.1",0,0}, error), "session relay listen");
    auto client = std::make_shared<RelayClient>(ClientOptions{"127.0.0.1",server.control_port(),server.video_port()});
    KvmSession session(std::make_unique<NetworkCaptureSource>(client), std::make_unique<NetworkControlSink>(client));
    DeviceInfo device; device.stable_id = "relay"; device.weak_match = true;
    CaptureMode mode; mode.device_id = "relay";
    require(session.select_capture(device, mode), "session select relay");
    session.set_video_rect({0,0,100,100});
    auto pump = [&](auto predicate) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        do {
            (void)session.take_latest_frame(); session.tick();
            if (predicate()) return true;
            std::this_thread::sleep_for(5ms);
        } while (std::chrono::steady_clock::now() < deadline);
        return predicate();
    };
    auto ready = [&] { const auto s = session.snapshot(); return s.input_state == InputState::preview &&
        s.control.state == ControlConnectionState::ready && s.video_fresh && s.video.processed_frames > 0; };
    require(pump(ready), "session Preview ready");
    const auto frames = client->capture_snapshot().received_samples;
    std::this_thread::sleep_for(650ms);
    require(client->control_snapshot().state == ControlConnectionState::ready &&
        client->capture_snapshot().received_samples > frames, "stale Preview retains live video connection");
    require(pump(ready), "Preview GUI resumes");
    auto capture_input = [&] {
        session.handle_input({InputButton{InputMouseButton::left,true,50,50}});
        session.handle_input({InputButton{InputMouseButton::left,false,50,50}});
        require(pump([&] {return session.snapshot().input_state == InputState::captured;}), "explicit capture click");
    };
    auto key_count = [&] {
        std::lock_guard lock(serial.mutex);
        return std::ranges::count_if(serial.received, [](const auto& f) {return f.command == 2 && f.data.size() == 8 && f.data[2] == 4;});
    };
    capture_input();
    session.handle_input({InputKey{4,true,false}});
    require(pump([&] {return key_count() > 0;}), "captured key reaches simulated HID");
    const auto epoch = hardware.snapshot().epoch;
    const auto count = key_count();
    // No GUI ticks, but video/network workers continue. Release must not wait
    // for the GUI to wake or for a later input event.
    std::this_thread::sleep_for(650ms);
    require(hardware.snapshot().epoch > epoch && hardware.snapshot().release_confirmed,
        "captured GUI stall releases through real serial worker");
    require(client->control_snapshot().state == ControlConnectionState::stalled,
        "old capture stays fenced after server release confirmation");
    client->gui_progress(); client->active(true);
    require(client->submit({hardware.snapshot().epoch,999,{},KeyEdge{4,true}}) == SubmitResult::not_ready,
        "fresh GUI and repeated true cannot bypass recapture latch");
    require(pump(ready), "real session tick returns captured router to Preview");
    session.handle_input({InputKey{4,false,false}});
    const auto until = std::chrono::steady_clock::now() + 150ms;
    require(pump([&] {return std::chrono::steady_clock::now() >= until;}), "resume observation");
    require(key_count() == count, "GUI resume never replays held key");
    capture_input();
    session.handle_input({InputKey{4,true,false}});
    require(pump([&] {return key_count() > count;}), "explicit recapture restores input");
    const auto stop_at = std::chrono::steady_clock::now();
    session.shutdown(); client.reset();
    require(std::chrono::steady_clock::now() - stop_at < 1s, "session stop remains bounded");
    server.stop(); hardware.disconnect();
}

void hevc_test(const char* fixture) {
    auto units = read_units(fixture);
    require(units.size() == 60 && units[0].idr && units[30].idr, "HEVC fixture IDR boundaries");
    std::string error;
    auto controls = tcp::Listener::bind("127.0.0.1", 0, error);
    auto videos = tcp::Listener::bind("127.0.0.1", 0, error);
    require(controls && videos, "HEVC listeners");
    std::atomic<bool> done{}, request_seen{}, initial_seen{}, recovered_seen{};
    std::atomic<int> requests{};
    std::jthread server([&] {
        auto socket = controls->accept(2s);
        if (!socket) return;
        Status status; status.session = 77; status.control.epoch = 1;
        status.control.state = ControlConnectionState::ready;
        status.control.target_usb_ready = status.control.release_confirmed = true;
        if (!send_packet(*socket, PacketType::hello, encode_hello({77, VideoCodec::hevc})) ||
            !send_packet(*socket, PacketType::status, encode_status(status))) return;
        while (!done) {
            auto packet = receive_packet(*socket, 400ms);
            if (!packet) break;
            if (packet->type == PacketType::keyframe_request) {
                const auto request = decode_keyframe_request(packet->payload);
                if (request && request->session == 77 && request->generation == 77) {
                    request_seen = true; ++requests;
                }
            }
            if (!send_packet(*socket, PacketType::status, encode_status(status))) break;
        }
    });
    std::jthread video([&] {
        auto socket = videos->accept(2s);
        if (!socket) return;
        auto packet = receive_packet(*socket, 500ms);
        const auto hello = packet ? decode_hello(packet->payload) : std::nullopt;
        if (!hello || hello->session != 77 || hello->codec != VideoCodec::hevc) return;
        auto send = [&](std::size_t index, std::uint64_t encoded_sequence) {
            auto au = units[index]; au.generation = 77; au.encoded_sequence = encoded_sequence;
            au.capture_sequence = 100 + index;
            // Deliberately foreign steady-clock epoch: only receive time is meaningful.
            au.arrival = std::chrono::steady_clock::time_point{};
            return bool(send_packet(*socket, PacketType::video_hevc, encode_hevc(au)));
        };
        if (!send(0, 1)) return;
        const auto initial_deadline = std::chrono::steady_clock::now() + 2s;
        while (!initial_seen && !done && std::chrono::steady_clock::now() < initial_deadline)
            std::this_thread::sleep_for(2ms);
        // Skip encoded sequence 2. Dependent pictures must not enter the mailbox.
        if (!send(2, 3) || !send(3, 4)) return;
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (!request_seen && !done && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(2ms);
        if (!request_seen || !send(30, 5)) return;
        for (std::size_t i = 31; i < units.size() && !done; ++i) {
            if (!send(i, i - 25)) break;
            std::this_thread::sleep_for(10ms);
        }
        while (!done) std::this_thread::sleep_for(2ms);
    });
    RelayClient client({"127.0.0.1", *controls->local_port(), *videos->local_port(), CodecBackend::ffmpeg_software});
    client.start();
    VideoProcessor processor;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    bool invalid_sequence = false;
    while (!recovered_seen && std::chrono::steady_clock::now() < deadline) {
        client.gui_progress();
        if (auto sample = client.take_sample()) {
            auto frame = processor.process(*sample);
            require(frame && frame->frame->data[0], "TCP HEVC owned CPU picture processes");
            if (frame->sequence == 100) initial_seen = true;
            if (frame->sequence == 102 || frame->sequence == 103) invalid_sequence = true;
            if (frame->sequence >= 130) recovered_seen = true;
            client.video_presented(frame->sequence);
        }
        std::this_thread::sleep_for(2ms);
    }
    const auto diagnostic = client.video_snapshot();
    const auto traffic = client.traffic_snapshot();
    client.stop(); done = true; server.join(); video.join();
    require(initial_seen && recovered_seen && request_seen && !invalid_sequence,
            "TCP HEVC sequence gap requests IDR and recovers without publishing dependents");
    require(diagnostic.codec == VideoCodec::hevc && diagnostic.decoder_backend == CodecBackend::ffmpeg_software &&
            !diagnostic.hardware_active && diagnostic.recoveries >= 1 && requests <= 3,
            "actual software decoder diagnostics and bounded recovery requests");
    require(traffic.video_received_bytes > 0 && traffic.control_sent_bytes > 0,
            "HEVC traffic counters preserved");
}
