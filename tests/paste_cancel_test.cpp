#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "app/kvm_session.hpp"
#include "control/ch9329_protocol.hpp"
#include "control/serial_worker.hpp"
#include "support/diagnostics.hpp"

namespace {
using namespace std::chrono_literals;
using namespace kvmux;

void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

template <typename Condition>
bool eventually(Condition condition, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (condition()) return true;
    std::this_thread::sleep_for(1ms);
  }
  return false;
}

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

struct FakeSerial {
  std::mutex mutex;
  bool opened{};
  std::size_t write_limit{4096};
  bool hold_keyboard_ack{};
  std::chrono::milliseconds keyboard_ack_delay{};
  std::deque<std::uint8_t> incoming;
  std::deque<std::uint8_t> held_ack;
  struct DelayedBytes {
    std::chrono::steady_clock::time_point ready;
    std::vector<std::uint8_t> bytes;
  };
  std::deque<DelayedBytes> delayed;
  ch9329::Parser requests;
  std::vector<ch9329::Frame> received;

  SerialIo io() {
    return {
        [this](const std::string&, int, std::string&) {
          std::lock_guard lock(mutex);
          opened = true;
          return true;
        },
        [this] {
          std::lock_guard lock(mutex);
          opened = false;
        },
        [this](std::span<const std::uint8_t> bytes) -> std::ptrdiff_t {
          std::lock_guard lock(mutex);
          if (!opened) return -1;
          const auto count = std::min(write_limit, bytes.size());
          auto frames = requests.feed(bytes.first(count));
          for (const auto& request : frames) {
            received.push_back(request);
            ch9329::Frame response{
                request.address,
                static_cast<std::uint8_t>(request.command | 0x80U),
                {0}};
            if (request.command == 0x01U) {
              response.data = {0x31, 1, 0, 0, 0, 0, 0, 0};
            } else if (request.command == 0x08U) {
              response.data.assign(50, 0);
              response.data[0] = 0x80;
              response.data[1] = 0x80;
            }
            const auto encoded = ch9329::encode(response);
            if (keyboard_ack_delay.count() > 0 && request.command == 0x02U &&
                !hold_keyboard_ack) {
              delayed.push_back(
                  {std::chrono::steady_clock::now() + keyboard_ack_delay,
                   encoded});
            } else if (hold_keyboard_ack && request.command == 0x02U) {
              held_ack.insert(held_ack.end(), encoded.begin(), encoded.end());
            } else {
              incoming.insert(incoming.end(), encoded.begin(), encoded.end());
            }
          }
          return static_cast<std::ptrdiff_t>(count);
        },
        [this](std::span<std::uint8_t> bytes) -> std::ptrdiff_t {
          std::lock_guard lock(mutex);
          if (!opened) return -1;
          const auto now = std::chrono::steady_clock::now();
          while (!delayed.empty() && delayed.front().ready <= now) {
            incoming.insert(incoming.end(), delayed.front().bytes.begin(),
                            delayed.front().bytes.end());
            delayed.pop_front();
          }
          const auto count = std::min(bytes.size(), incoming.size());
          for (std::size_t i = 0; i < count; ++i) {
            bytes[i] = incoming.front();
            incoming.pop_front();
          }
          return static_cast<std::ptrdiff_t>(count);
        },
    };
  }
};

struct Fixture {
  FakeSerial serial;
  StreamingCapture* capture{};
  std::unique_ptr<KvmSession> session;

  Fixture() {
    auto sink = std::make_unique<Ch9329ControlSink>(serial.io());
    sink->connect("fake", 57600);
    require(eventually(
                [&] {
                  return sink->snapshot().state ==
                             ControlConnectionState::ready &&
                         sink->snapshot().release_confirmed;
                },
                3000ms),
            "serial handshake reaches ready");
    auto source = std::make_unique<StreamingCapture>();
    capture = source.get();
    session = std::make_unique<KvmSession>(std::move(source), std::move(sink),
                                           std::make_shared<Diagnostics>());
    DeviceInfo device;
    device.stable_id = "fake";
    const CaptureMode mode{
        "fake", 2, 2, {30, 1}, PixelFormat::yuy2, PixelFormat::yuy2, "YUY2"};
    require(session->select_capture(device, mode), "capture starts");
    session->set_video_rect({0, 0, 100, 100});
    require(eventually(
                [&] {
                  session->tick();
                  const auto state = session->snapshot();
                  return state.video_fresh &&
                         state.control.state == ControlConnectionState::ready &&
                         state.control.release_confirmed &&
                         state.input_state == InputState::preview;
                },
                3000ms),
            "preview is fresh and the control link is ready");
  }
};

void stale_video_cancels_paste() {
  Fixture fixture;
  {
    std::lock_guard lock(fixture.serial.mutex);
    fixture.serial.keyboard_ack_delay = 40ms;
  }
  require(static_cast<bool>(
              fixture.session->start_text_paste(std::string(40, 'a'))),
          "paste starts");
  require(fixture.session->text_paste_active(), "paste is active");
  const auto epoch = fixture.session->snapshot().control.epoch;
  fixture.capture->pause();
  require(eventually(
              [&] {
                fixture.session->tick();
                const auto progress = fixture.session->text_paste_progress();
                return !progress.active &&
                       progress.error == TextPasteError::canceled &&
                       progress.cancel_reason == PasteCancelReason::video_stale;
              },
              3000ms),
          "stale video cancels the paste");
  require(eventually(
              [&] {
                fixture.session->tick();
                const auto control = fixture.session->snapshot().control;
                return control.epoch != epoch && control.release_confirmed;
              },
              3000ms),
          "the common release path completes while the link is up");
}

void held_ack_cancels_paste() {
  Fixture fixture;
  {
    std::lock_guard lock(fixture.serial.mutex);
    fixture.serial.hold_keyboard_ack = true;
  }
  require(static_cast<bool>(fixture.session->start_text_paste("ab")),
          "paste starts");
  const auto began = std::chrono::steady_clock::now();
  require(eventually(
              [&] {
                fixture.session->tick();
                const auto progress = fixture.session->text_paste_progress();
                const auto control = fixture.session->snapshot().control;
                return std::chrono::steady_clock::now() - began >= 700ms &&
                       !progress.active &&
                       progress.error == TextPasteError::canceled &&
                       progress.cancel_reason ==
                           PasteCancelReason::ack_timeout &&
                       !control.release_confirmed;
              },
              3000ms),
          "a held keyboard ACK ends as ack_timeout without release confirmed");
}

void fresh_link_completes_paste() {
  Fixture fixture;
  constexpr std::size_t bytes = 4096;
  require(static_cast<bool>(
              fixture.session->start_text_paste(std::string(bytes, 'a'))),
          "4 KB paste starts");
  require(eventually(
              [&] {
                fixture.session->tick();
                const auto progress = fixture.session->text_paste_progress();
                return !progress.active &&
                       progress.error == TextPasteError::none &&
                       progress.cancel_reason == PasteCancelReason::none &&
                       progress.planned_gestures == bytes &&
                       progress.scheduled_gestures == bytes;
              },
              20000ms),
          "a fresh FakeSerial paste completes");
}

}  // namespace

int main() {
  stale_video_cancels_paste();
  held_ack_cancels_paste();
  fresh_link_completes_paste();
  return 0;
}
