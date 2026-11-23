#include "network/source_admission.hpp"
#include "relay_integration_fakes.hpp"
void admission_policy_test() {
  using namespace kvmux::relay;
  const auto start = MediaTime{} + 10s;
  SourceAdmission policy(7, 20ms);
  MediaStats stats;
  assert(!policy.feedback(8, stats, start));
  assert(policy.feedback(7, stats, start));
  stats.received_frames = 10;
  assert(policy.feedback(7, stats, start + 100ms));
  stats.lost_frames = stats.age_losses = 1;
  assert(policy.feedback(7, stats, start + 200ms));
  policy.poll(start + 499ms);
  assert(policy.interval() == 20ms);
  policy.poll(start + 500ms);
  assert(policy.interval() == 25ms);
  assert(policy.delta().lost_frames == 1);
  assert(!policy.feedback(7, stats, start + 600ms));
  auto regressed = stats;
  regressed.received_frames = 9;
  assert(!policy.feedback(7, regressed, start + 600ms));
  assert(!policy.feedback(8, stats, start + 600ms));
  policy.poll(start + 1000ms);
  assert(policy.interval() == 25ms);
  // Missing feedback causes one reduction per outage, never repeated
  // compounding.
  policy.poll(start + 1200ms);
  assert(policy.interval() == 31250us);
  policy.poll(start + 4000ms);
  assert(policy.interval() == 31250us);
  for (int n = 0; n <= 4; ++n) {
    ++stats.received_frames;
    assert(policy.feedback(7, stats, start + 4500ms + n * 500ms));
    policy.poll(start + 4500ms + n * 500ms);
  }
  assert(policy.interval() ==
         28125us);  // Two seconds healthy, then ten percent.
  ++stats.received_frames;
  policy.feedback(7, stats, start + 7000ms);
  policy.poll(start + 7000ms);
  assert(policy.interval() == 28125us);
  ++stats.received_frames;
  policy.feedback(7, stats, start + 7500ms);
  policy.poll(start + 7500ms);
  assert(policy.interval() == 25312us);
  for (int n = 0; n < 30; ++n) {
    ++stats.lost_frames;
    policy.feedback(7, stats, start + 8000ms + n * 500ms);
    policy.poll(start + 8000ms + n * 500ms);
  }
  assert(policy.interval() == 200ms);
  for (int n = 0; n < 70; ++n) {
    ++stats.received_frames;
    policy.feedback(7, stats, start + 23000ms + n * 500ms);
    policy.poll(start + 23000ms + n * 500ms);
  }
  assert(policy.interval() == 20ms);
  SourceAdmission slow(9, 300ms);
  MediaStats waiting;
  waiting.waiting_idr = true;
  slow.feedback(9, {}, start);
  slow.feedback(9, waiting, start + 500ms);
  slow.poll(start + 500ms);
  assert(slow.interval() == 300ms);
  SourceAdmission reset(10, 20ms);
  assert(reset.interval() == 20ms && !reset.latest());
}

// An encoder that cannot be created on the first client stops the workers; the
// server must report that instead of staying bound and silent.
struct RawModeCapture final : CaptureSource {
  std::vector<DeviceInfo> enumerate_devices() override { return {}; }
  std::vector<CaptureMode> enumerate_modes(const std::string&) override {
    return {};
  }
  void start(const CaptureMode&) override {}
  void stop() noexcept override {}
  std::optional<CaptureSample> take_latest_sample() override { return {}; }
  CaptureSnapshot snapshot() const override {
    CaptureSnapshot out;
    out.state = CaptureState::streaming;
    out.actual_mode = {
        "fake",   16, 16, {50, 1}, PixelFormat::yuv420p, PixelFormat::yuv420p,
        "YUV420P"};
    return out;
  }
};
void encoder_failure_test() {
  FakeSerial serial;
  RawModeCapture capture;
  Ch9329ControlSink sink{serial.io()};
  sink.connect("fake", 115200);
  assert(until([&] { return sink.snapshot().release_confirmed; }));
  kvmux::relay::RelayServer server(
      &capture, &sink,
      [](kvmux::CodecBackend,
         std::string& error) -> std::unique_ptr<kvmux::VideoEncoder> {
        error = "no encoder";
        return nullptr;
      });
  kvmux::relay::ServerOptions options;
  options.bind_address = "127.0.0.1";
  options.control_port = options.video_port = 0;
  options.codec = kvmux::VideoCodec::h264;
  std::string error;
  assert(server.start(options, error));
  assert(!server.snapshot().failed);
  kvmux::relay::ClientOptions client;
  client.control_port = server.control_port();
  client.video_port = server.video_port();
  GuiFixture gui(client);
  assert(until(
      [&] {
        gui.run_for(20ms);
        return server.snapshot().failed;
      },
      5000ms));
  assert(server.snapshot().failure.find("no encoder") != std::string::npos);
}

int main(int argc, char** argv) {
  assert(argc > 1);
  admission_policy_test();
  encoder_failure_test();
  {
    RelayFixture capped(argv[1], 24000);
    GuiFixture gui(capped.options());
    gui.activate();
    const auto initial = gui.client->traffic_snapshot().video_received_bytes;
    gui.run_for(1000ms);
    const auto bytes =
        gui.client->traffic_snapshot().video_received_bytes - initial;
    assert(bytes <= 24000 + 4800 && bytes > 0);
    assert(gui.session.snapshot().video_fresh);
  }
  RelayFixture relay(argv[1]);
  assert(relay.server.control_port() != relay.server.video_port());
  UdpProxy proxy(relay.options());
  proxy.drop_welcome = 1;
  proxy.drop_ready = 1;
  proxy.wrong_source_only = true;
  GuiFixture gui(proxy.options());
  gui.run_for(500ms);
  assert(gui.client->capture_snapshot().received_samples == 0);
  proxy.wrong_source_only = false;
  proxy.wrong_tuple_only = true;
  gui.run_for(100ms);
  assert(gui.client->capture_snapshot().received_samples == 0);
  proxy.wrong_tuple_only = false;
  gui.activate();
  const auto id = proxy.session_id.load();
  assert(id);
  auto second = std::make_shared<kvmux::relay::RelayClient>(relay.options());
  second->start();
  assert(until([&] {
    return second->control_snapshot().state ==
           ControlConnectionState::disconnected;
  }));
  assert(second->control_snapshot().error == "Relay already controlled");
  gui.run_for(100ms);
  assert(proxy.session_id == id);
  assert(proxy.challenges > 2 && proxy.proofs > 2);
  gui.session.release_control(PasteCancelReason::shutdown);
  assert(gui.wait([&] {
    return gui.session.snapshot().input_state == InputState::preview;
  }));
}
