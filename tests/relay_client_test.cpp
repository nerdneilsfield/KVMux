#include "network/relay_client.hpp"
#include "network/relay_protocol.hpp"
#include "video/video_pipeline.hpp"
#include "relay_test_fakes.hpp"
#include <atomic>
#include <fstream>
#include <stdexcept>
#include <thread>
using namespace kvmux;
using namespace kvmux::relay;
using namespace std::chrono_literals;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void integrated_test(const std::vector<std::uint8_t>& jpeg);
void startup_grace_test(const std::vector<std::uint8_t>& jpeg);
int main(int argc, char** argv) {
    require(argc == 2, "JPEG fixture required");
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
        if (!send_packet(*socket, PacketType::hello, encode_session(42)) ||
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
        if (!hello || decode_session(hello->payload) != 42) return;
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
    // Keep decoder/video thread running but stop the GUI progress updates.
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (sink.snapshot().state != ControlConnectionState::disconnected && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    require(sink.snapshot().state == ControlConnectionState::disconnected && !sink.snapshot().release_confirmed,
        "network thread cannot renew stopped GUI lease");
    require(sink.snapshot().error == "GUI heartbeat expired", "GUI expiry diagnostic");
    source.stop(); pipeline.stop(); done = true;
    server.join(); video.join();
    integrated_test(jpeg);
    startup_grace_test(jpeg);
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
        if (!send_packet(*socket, PacketType::hello, encode_session(42)) ||
            !send_packet(*socket, PacketType::status, encode_status(status))) return;
        control_sent_bytes = 12U + encode_session(42).size() + 12U + encode_status(status).size();
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
    while (client.control_snapshot().state != ControlConnectionState::disconnected &&
           std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    require(client.control_snapshot().error == "GUI heartbeat expired", "startup grace expires without real GUI progress");
    require(std::chrono::steady_clock::now() - started >= 250ms, "startup gets the existing bounded grace");
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
