#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "app/kvm_session.hpp"

namespace {
using namespace std::chrono_literals;
using namespace kvmux;

void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

template <typename Condition>
bool eventually(Condition condition,
                std::chrono::milliseconds timeout = 3000ms) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (condition()) return true;
    std::this_thread::sleep_for(1ms);
  }
  return false;
}

// Streams a tiny raw frame every few milliseconds so the session sees fresh
// video; pause() stops new samples so the session can go stale.
class StreamingCapture final : public CaptureSource {
 public:
  void pause() { paused_ = true; }
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
    if (paused_) return std::nullopt;
    std::lock_guard lock(mutex_);
    if (status_.state != CaptureState::streaming) return std::nullopt;
    static constexpr std::array<std::uint8_t, 8> yuy2{16, 128, 235, 128,
                                                      81, 90,  145, 240};
    const std::array planes{PlaneLayout{0, 4, 4, 2}};
    ++status_.received_samples;
    return CaptureSample::make_raw(status_.generation, status_.received_samples,
                                   std::chrono::steady_clock::now(), 2, 2,
                                   PixelFormat::yuy2, planes, yuy2);
  }
  CaptureSnapshot snapshot() const override {
    std::lock_guard lock(mutex_);
    return status_;
  }

 private:
  mutable std::mutex mutex_;
  CaptureSnapshot status_;
  std::atomic_bool paused_{false};
};

struct Recorded {
  ControlEvent event;
  std::thread::id thread;
};

// State outlives the sink so the test can inspect it after session shutdown.
struct SinkState {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<Recorded> events;
  int releases{};
  bool hold{};  // While set, submit() records the event and then blocks.

  std::size_t count() {
    std::lock_guard lock(mutex);
    return events.size();
  }
  void set_hold(bool value) {
    {
      std::lock_guard lock(mutex);
      hold = value;
    }
    cv.notify_all();
  }
};

class RecordingSink final : public ControlSink {
 public:
  explicit RecordingSink(std::shared_ptr<SinkState> state)
      : state_(std::move(state)) {}
  SubmitResult submit(ControlEvent event) override {
    std::unique_lock lock(state_->mutex);
    state_->events.push_back({event, std::this_thread::get_id()});
    state_->cv.wait(lock, [&] { return !state_->hold; });
    return SubmitResult::accepted;
  }
  void release_all() noexcept override {
    std::lock_guard lock(state_->mutex);
    ++state_->releases;
  }
  ControlSnapshot snapshot() const override {
    ControlSnapshot value;
    value.state = ControlConnectionState::ready;
    value.epoch = 7;
    value.target_usb_ready = true;
    value.release_confirmed = true;
    return value;
  }

 private:
  std::shared_ptr<SinkState> state_;
};

InputEvent left_down() {
  return {InputButton{InputMouseButton::left, true, 50, 50}};
}
InputEvent left_up() {
  return {InputButton{InputMouseButton::left, false, 50, 50}};
}

struct Fixture {
  std::shared_ptr<SinkState> sink = std::make_shared<SinkState>();
  std::shared_ptr<Diagnostics> diagnostics = std::make_shared<Diagnostics>();
  StreamingCapture* capture{};
  std::unique_ptr<KvmSession> session;

  Fixture() {
    auto source = std::make_unique<StreamingCapture>();
    capture = source.get();
    session = std::make_unique<KvmSession>(
        std::move(source), std::make_unique<RecordingSink>(sink), diagnostics);
    DeviceInfo device;
    device.stable_id = "fake";
    const CaptureMode mode{
        "fake", 2, 2, {30, 1}, PixelFormat::yuy2, PixelFormat::yuy2, "YUY2"};
    require(session->select_capture(device, mode), "capture starts");
    require(session->set_mouse_mode(MouseMode::relative), "relative mode");
    session->set_video_rect({0, 0, 100, 100});
    require(eventually([&] {
              session->tick();
              return session->snapshot().video_fresh;
            }),
            "video becomes fresh");
    // The router only arms once its own video-fresh flag is set by tick().
    require(eventually([&] {
              session->tick();
              session->handle_input(left_down());
              if (session->input_state() == InputState::arming) return true;
              session->handle_input(left_up());
              return false;
            }),
            "activation press arms");
    session->handle_input(left_up());
    require(eventually([&] {
              session->tick();
              return session->input_state() == InputState::captured;
            }),
            "activation click captures");
    require(sink->count() == 0, "activation is swallowed");
  }

  // Leaves the mouse thread blocked inside the sink holding the router lock,
  // so everything sent afterwards waits in the one-slot mailbox.
  void block_mouse_thread() {
    sink->set_hold(true);
    session->handle_input({InputRelativeMotion{1, 0}});
    require(eventually([&] { return sink->count() == 1; }),
            "mouse thread submitted the first motion");
  }
  int releases() {
    std::lock_guard lock(sink->mutex);
    return sink->releases;
  }
};

}  // namespace

int main() {
  const auto caller = std::this_thread::get_id();

  {
    // Motion with no tick or edge is delivered by a thread other than the
    // caller; edges are applied on the caller's thread.
    Fixture f;
    f.session->handle_input({InputRelativeMotion{2, 0}});
    require(eventually([&] { return f.sink->count() == 1; }),
            "independent motion reaches the sink");
    f.session->handle_input(left_down());
    std::lock_guard lock(f.sink->mutex);
    require(f.sink->events.size() == 2 && f.sink->events[0].thread != caller &&
                std::holds_alternative<RelativeMotion>(
                    f.sink->events[0].event.payload),
            "motion is submitted off the UI thread");
    require(
        f.sink->events[1].thread == caller &&
            std::holds_alternative<ButtonEdge>(f.sink->events[1].event.payload),
        "edges are submitted on the calling thread");
  }

  {
    // Motion that arrives while the router is busy folds into one report, and
    // keeps its total.
    Fixture f;
    f.block_mouse_thread();
    for (const double dx : {1.0, 2.0, 3.0})
      f.session->handle_input({InputRelativeMotion{dx, 0}});
    f.sink->set_hold(false);
    require(eventually([&] { return f.sink->count() == 2; }),
            "folded motion delivered as one event");
    std::this_thread::sleep_for(30ms);
    std::lock_guard lock(f.sink->mutex);
    require(
        f.sink->events.size() == 2 &&
            std::get<RelativeMotion>(f.sink->events[1].event.payload).dx == 6.0,
        "folded relative motion keeps its total");
    require(f.diagnostics->snapshot().mouse_motion_folded >= 2,
            "folding is counted");
  }

  {
    // Saturating motion never crosses an edge and none is lost.
    Fixture f;
    constexpr int per_phase = 500;
    const auto motion = [&] {
      for (int i = 0; i < per_phase; ++i)
        f.session->handle_input({InputRelativeMotion{1, 0}});
    };
    motion();
    f.session->handle_input(left_down());
    motion();
    f.session->handle_input({InputKey{0x04, true, false}});
    motion();
    f.session->handle_input(left_up());
    require(eventually([&] {
              std::lock_guard lock(f.sink->mutex);
              return !f.sink->events.empty() &&
                     std::holds_alternative<ButtonEdge>(
                         f.sink->events.back().event.payload);
            }),
            "last edge reaches the sink");
    std::lock_guard lock(f.sink->mutex);
    std::vector<double> segments(1, 0.0);
    std::vector<int> edges;
    for (const auto& recorded : f.sink->events) {
      const auto& payload = recorded.event.payload;
      if (const auto* move = std::get_if<RelativeMotion>(&payload)) {
        segments.back() += move->dx;
      } else if (const auto* button = std::get_if<ButtonEdge>(&payload)) {
        edges.push_back(button->pressed ? 1 : 2);
        segments.push_back(0.0);
      } else if (std::holds_alternative<KeyEdge>(payload)) {
        edges.push_back(3);
        segments.push_back(0.0);
      }
    }
    require(edges == std::vector<int>({1, 3, 2}),
            "button/key edges keep their order");
    require(segments.size() == 4 && segments[0] == per_phase &&
                segments[1] == per_phase && segments[2] == per_phase &&
                segments[3] == 0.0,
            "motion never crosses an edge and no motion is lost");
  }

  {
    // Video found invalid by tick(): motion still waiting is not sent.
    Fixture f;
    f.capture->pause();
    std::this_thread::sleep_for(650ms);  // Past the 500 ms stale threshold.
    f.block_mouse_thread();
    f.session->handle_input({InputRelativeMotion{5, 0}});
    std::thread ticker([&] { f.session->tick(); });
    std::this_thread::sleep_for(50ms);
    f.sink->set_hold(false);
    ticker.join();
    require(eventually([&] { return f.releases() >= 1; }),
            "stale video releases control");
    std::this_thread::sleep_for(50ms);
    require(f.sink->count() == 1, "pending motion is dropped, not replayed");
    require(f.session->input_state() != InputState::captured,
            "stale video leaves Captured");
  }

  {
    // Pending motion older than the 100 ms control-queue limit is not
    // replayed; the session releases instead.
    Fixture f;
    f.block_mouse_thread();
    f.session->handle_input({InputRelativeMotion{5, 0}});
    std::this_thread::sleep_for(150ms);
    f.sink->set_hold(false);
    require(eventually([&] { return f.releases() >= 1; }),
            "stale motion releases control");
    std::this_thread::sleep_for(30ms);
    require(f.sink->count() == 1, "stale motion is not sent");
    require(f.session->input_state() != InputState::captured,
            "stale motion leaves Captured");
  }

  {
    // Unbounded relative accumulation is treated like a full queue.
    Fixture f;
    f.block_mouse_thread();
    for (int i = 0; i < 20000; ++i)
      f.session->handle_input({InputRelativeMotion{1, 0}});
    f.sink->set_hold(false);
    require(eventually([&] { return f.releases() >= 1; }),
            "oversized motion releases control");
    std::this_thread::sleep_for(30ms);
    require(f.sink->count() == 1, "oversized motion is not split and sent");
  }

  {
    // Shutdown joins the mouse thread; nothing is delivered afterwards.
    Fixture f;
    for (int i = 0; i < 200; ++i)
      f.session->handle_input({InputRelativeMotion{1, 0}});
    f.session.reset();
    const auto delivered = f.sink->count();
    std::this_thread::sleep_for(50ms);
    require(f.sink->count() == delivered, "no input after shutdown");
  }
  return EXIT_SUCCESS;
}
