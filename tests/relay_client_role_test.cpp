#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <source_location>
#include <string>
#include <thread>
#include <vector>

#include "relay_integration_fakes.hpp"

using namespace kvmux;
using namespace kvmux::relay;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

void check(bool ok, const char* detail = "",
           std::source_location loc = std::source_location::current()) {
  if (!ok) {
    std::cerr << "relay client role check failed at " << loc.line();
    if (detail && detail[0]) std::cerr << ": " << detail;
    std::cerr << '\n';
    std::abort();
  }
}
ServerOptions server_options(std::uint16_t video_port) {
  ServerOptions options;
  options.bind_address = "127.0.0.1";
  options.control_port = 0;
  options.video_port = video_port;
  options.codec = VideoCodec::mjpeg;
  return options;
}
void start_server(RelayServer& server, std::uint16_t video_port) {
  std::string error;
  check(server.start(server_options(video_port), error), error.c_str());
}
bool keyboard_usage(const ch9329::Frame& frame, std::uint8_t usage) {
  if (frame.command != 0x02 || frame.data.size() < 8) return false;
  return std::any_of(frame.data.begin() + 2, frame.data.begin() + 8,
                     [&](std::uint8_t key) { return key == usage; });
}
bool keyboard_clear(const ch9329::Frame& frame) {
  return frame.command == 0x02 && frame.data.size() >= 8 &&
         frame.data[0] == 0 && frame.data[2] == 0;
}
bool serial_has(FakeSerial& serial, std::size_t begin,
                const std::function<bool(const ch9329::Frame&)>& pred) {
  std::lock_guard lock(serial.mutex);
  for (std::size_t i = begin; i < serial.received.size(); ++i)
    if (pred(serial.received[i])) return true;
  return false;
}

void expect_role_mismatch(RelayServer& server, wire::RelayRole role) {
  ClientOptions options;
  options.role = role;
  options.control_port = server.control_port();
  options.video_port = server.video_port() == 0 ? 9 : server.video_port();
  RelayClient client(options);
  client.start();
  check(until([&] {
    const auto control = client.control_snapshot();
    const auto capture = client.capture_snapshot();
    return control.error == "角色不匹配" && capture.error == "角色不匹配" &&
           control.state == ControlConnectionState::disconnected &&
           capture.state == CaptureState::fault &&
           capture.received_samples == 0;
  }));
}
void role_mismatch_reports_unsupported(const char* jpeg) {
  {
    FakeCapture capture;
    capture.jpeg = fixture_bytes(jpeg);
    RelayServer server(&capture, nullptr);
    start_server(server, 0);
    expect_role_mismatch(server, wire::RelayRole::control);
  }
  {
    FakeCapture capture;
    capture.jpeg = fixture_bytes(jpeg);
    FakeSerial serial;
    Ch9329ControlSink sink(serial.io());
    sink.connect("fake", 115200);
    check(until([&] { return sink.snapshot().release_confirmed; }));
    RelayServer server(&capture, &sink);
    start_server(server, 0);
    expect_role_mismatch(server, wire::RelayRole::video);
  }
}

struct ControlLink {
  FakeSerial serial;
  Ch9329ControlSink sink;
  RelayServer server;
  std::unique_ptr<UdpProxy> proxy;
  std::shared_ptr<RelayClient> client;

  ControlLink() : sink(serial.io()), server(nullptr, &sink) {
    sink.connect("fake", 115200);
    check(until([&] { return sink.snapshot().release_confirmed; }));
    start_server(server, 0);
    ClientOptions upstream;
    upstream.control_port = server.control_port();
    std::string error;
    auto spare = udp::Socket::bind("127.0.0.1", 0, error);
    check(spare.has_value(), error.c_str());
    upstream.video_port = *spare->local_port();
    proxy = std::make_unique<UdpProxy>(upstream);
    auto options = proxy->options();
    options.role = wire::RelayRole::control;
    client = std::make_shared<RelayClient>(options);
    client->start();
  }
  void keep_picture(std::uint64_t generation, std::uint64_t sequence) {
    client->gui_progress();
    client->video_presented(generation, sequence);
  }
};
void release_confirmed_tracks_challenge_not_picture() {
  ControlLink link;
  check(until([&] {
    link.keep_picture(1, 1);
    const auto snap = link.client->control_snapshot();
    return snap.state == ControlConnectionState::ready &&
           snap.target_usb_ready && snap.release_confirmed &&
           snap.error.empty();
  }));
  const auto stale = Clock::now();
  while (Clock::now() - stale < 600ms) {
    link.keep_picture(1, 1);
    std::this_thread::sleep_for(5ms);
  }
  const auto held = link.client->control_snapshot();
  check(held.release_confirmed);
  check(held.error.empty());

  link.proxy->blackout = true;
  const auto dropped = Clock::now();
  while (Clock::now() - dropped < 120ms) {
    const auto snap = link.client->control_snapshot();
    check(snap.release_confirmed);
    check(snap.error.empty());
    std::this_thread::sleep_for(5ms);
  }
  check(until([&] {
    const auto snap = link.client->control_snapshot();
    return !snap.release_confirmed && snap.error.empty() &&
           link.client->capture_snapshot().state != CaptureState::fault;
  }));
  check(Clock::now() - dropped < 2s);
}

// Local frames plus a control-only relay. Presentation identity is what the
// control client treats as the picture the operator is watching.
struct StreamingCapture final : public CaptureSource {
  std::atomic<bool> paused{false};
  std::vector<DeviceInfo> enumerate_devices() override { return {}; }
  std::vector<CaptureMode> enumerate_modes(const std::string&) override {
    return {};
  }
  void start(const CaptureMode& mode) override {
    std::lock_guard lock(mutex_);
    ++status_.generation;
    status_.state = CaptureState::streaming;
    status_.actual_mode = mode;
  }
  void stop() noexcept override {
    std::lock_guard lock(mutex_);
    status_.state = CaptureState::stopped;
  }
  std::optional<CaptureSample> take_latest_sample() override {
    std::this_thread::sleep_for(3ms);
    if (paused) return std::nullopt;
    std::lock_guard lock(mutex_);
    if (status_.state != CaptureState::streaming) return std::nullopt;
    static constexpr std::array<std::uint8_t, 8> yuy2{16, 128, 235, 128,
                                                      81, 90,  145, 240};
    const std::array planes{PlaneLayout{0, 4, 4, 2}};
    ++status_.received_samples;
    return CaptureSample::make_raw(status_.generation, status_.received_samples,
                                   Clock::now(), 2, 2, PixelFormat::yuy2,
                                   planes, yuy2);
  }
  CaptureSnapshot snapshot() const override {
    std::lock_guard lock(mutex_);
    return status_;
  }

  mutable std::mutex mutex_;
  CaptureSnapshot status_;
};

struct ControlSession {
  FakeSerial serial;
  Ch9329ControlSink sink;
  RelayServer server;
  StreamingCapture* capture{};
  std::shared_ptr<RelayClient> client;
  std::unique_ptr<KvmSession> session;
  std::uint64_t generation{1};
  std::uint64_t sequence{1};
  bool advance{true};

  ControlSession() : sink(serial.io()), server(nullptr, &sink) {
    sink.connect("fake", 115200);
    check(until([&] { return sink.snapshot().release_confirmed; }));
    start_server(server, 0);
    ClientOptions options;
    options.role = wire::RelayRole::control;
    options.control_port = server.control_port();
    options.video_port = 9;
    client = std::make_shared<RelayClient>(std::move(options));
    client->start();
    auto source = std::make_unique<StreamingCapture>();
    capture = source.get();
    session = std::make_unique<KvmSession>(
        std::move(source), std::make_unique<NetworkControlSink>(client));
    DeviceInfo device;
    device.stable_id = "local";
    const CaptureMode mode{
        "local", 2, 2, {30, 1}, PixelFormat::yuy2, PixelFormat::yuy2, "YUY2"};
    check(session->select_capture(device, mode));
    session->set_video_rect({0, 0, 100, 100});
  }
  void present() {
    session->tick();
    if (advance) {
      if (auto frame = session->take_latest_frame()) {
        generation = frame->generation;
        sequence = frame->sequence;
      } else {
        ++sequence;
      }
    } else {
      (void)session->take_latest_frame();
    }
    session->video_presented(generation, sequence);
  }
  void note(const char* step) {
    const auto state = session->snapshot();
    std::cerr << step << " input=" << int(state.input_state)
              << " control=" << int(state.control.state)
              << " usb=" << state.control.target_usb_ready
              << " rel=" << state.control.release_confirmed
              << " fresh=" << state.video_fresh
              << " video=" << int(state.video_state)
              << " err=" << state.control.error << '\n';
  }
  void arm_and_press() {
    const bool ready = until([&] {
      present();
      const auto state = session->snapshot();
      return state.video_fresh &&
             state.control.state == ControlConnectionState::ready &&
             state.control.target_usb_ready && state.control.release_confirmed;
    });
    if (!ready) note("ready");
    check(ready);
    session->handle_input({InputButton{InputMouseButton::left, true, 50, 50}});
    session->handle_input({InputButton{InputMouseButton::left, false, 50, 50}});
    const bool captured = until([&] {
      present();
      return session->snapshot().input_state == InputState::captured;
    });
    if (!captured) note("captured");
    check(captured);
    session->handle_input({InputKey{0x04, true, false}});
    const bool down = until([&] {
      present();
      return serial_has(serial, 0, [](const ch9329::Frame& frame) {
        return keyboard_usage(frame, 0x04);
      });
    });
    if (!down) note("key");
    check(down);
  }
  std::size_t last_usage(std::uint8_t usage) {
    std::lock_guard lock(serial.mutex);
    for (std::size_t i = serial.received.size(); i-- > 0;)
      if (keyboard_usage(serial.received[i], usage)) return i;
    return 0;
  }
};

void control_session_releases_when_capture_stops() {
  ControlSession session;
  session.arm_and_press();
  const auto mark = session.last_usage(0x04) + 1;
  session.advance = false;
  session.capture->paused = true;
  const auto began = Clock::now();
  const bool released = until(
      [&] {
        session.present();
        return serial_has(session.serial, mark, keyboard_clear);
      },
      2000ms);
  check(released);
  check(Clock::now() - began >= 200ms);
}
void control_session_releases_when_presentation_repeats() {
  ControlSession session;
  session.arm_and_press();
  const auto mark = session.last_usage(0x04) + 1;
  session.advance = false;
  const auto began = Clock::now();
  const bool released = until(
      [&] {
        session.present();
        return serial_has(session.serial, mark, keyboard_clear);
      },
      2000ms);
  check(released);
  check(Clock::now() - began >= 200ms);
}
void control_session_accepts_restarted_sequence() {
  ControlSession session;
  session.arm_and_press();
  const auto mark = session.last_usage(0x04) + 1;
  const auto generation = session.generation + 1;
  std::uint64_t sequence = 1;
  const auto began = Clock::now();
  while (Clock::now() - began < 700ms) {
    session.advance = false;
    session.generation = generation;
    session.sequence = sequence++;
    session.present();
    check(!serial_has(session.serial, mark, keyboard_clear));
    std::this_thread::sleep_for(5ms);
  }
  session.session->handle_input({InputKey{0x05, true, false}});
  check(until([&] {
    session.generation = generation;
    session.sequence = sequence++;
    session.present();
    return serial_has(session.serial, mark, [](const ch9329::Frame& frame) {
      return keyboard_usage(frame, 0x05);
    });
  }));
  check(!serial_has(session.serial, mark, keyboard_clear));
}

void video_client_stays_up_without_sending_control(const char* jpeg) {
  FakeSerial serial;
  FakeCapture capture;
  capture.jpeg = fixture_bytes(jpeg);
  RelayServer server(&capture, nullptr);
  start_server(server, 0);
  check(server.video_port() != 0);
  ClientOptions options;
  options.role = wire::RelayRole::video;
  options.control_port = server.control_port();
  options.video_port = server.video_port();
  RelayClient client(options);
  client.start();
  check(until([&] {
    const auto snap = client.capture_snapshot();
    return snap.state == CaptureState::streaming && snap.received_samples > 0 &&
           snap.error.empty();
  }));
  const auto before = client.capture_snapshot().received_samples;
  const auto end = Clock::now() + 11s;
  while (Clock::now() < end) std::this_thread::sleep_for(20ms);
  const auto after = client.capture_snapshot();
  check(after.state == CaptureState::streaming);
  check(after.received_samples > before);
  check(after.error.empty());
  check(client.control_snapshot().error.empty());
  InputSync sync;
  sync.epoch = 1;
  sync.intent_generation = 1;
  sync.revision = 1;
  sync.state.mode = MouseMode::relative;
  check(client.synchronize(sync) == SubmitResult::not_ready);
  check(client.submit({1, 1, Clock::now(), KeyEdge{4, true}}) ==
        SubmitResult::not_ready);
  check(client.start_ascii_paste_text(std::vector<std::uint8_t>{'a'}) ==
        SubmitResult::not_ready);
  check(!serial.opened);
  check(serial.received.empty());
}

int main(int argc, char** argv) {
  check(argc >= 2);
  role_mismatch_reports_unsupported(argv[1]);
  release_confirmed_tracks_challenge_not_picture();
  control_session_releases_when_capture_stops();
  control_session_releases_when_presentation_repeats();
  control_session_accepts_restarted_sequence();
  video_client_stays_up_without_sending_control(argv[1]);
  std::cout << "relay_client_role: passed\n";
}
