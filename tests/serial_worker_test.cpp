#include "control/serial_worker.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "control/ch9329_protocol.hpp"
#include "input/windows_hex_text.hpp"
#include "support/diagnostics.hpp"

namespace {
using namespace std::chrono_literals;

void require(bool value, const char* message) {
  if (!value) {
    throw std::runtime_error(message);
  }
}

struct FakeSerial {
  std::mutex mutex;
  bool opened{};
  std::size_t write_limit{3};
  bool answer{true};
  // Negative means every keyboard frame is ACKed. Zero holds the next one.
  int keyboard_acks_remaining{-1};
  std::deque<std::uint8_t> incoming;
  kvmux::ch9329::Parser requests;
  std::vector<kvmux::ch9329::Frame> received;

  void ack(std::uint8_t command) {
    std::lock_guard lock(mutex);
    const auto bytes = kvmux::ch9329::encode(
        {0, static_cast<std::uint8_t>(command | 0x80U), {0}});
    incoming.insert(incoming.end(), bytes.begin(), bytes.end());
  }

  kvmux::SerialIo io() {
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
          if (!opened) {
            return -1;
          }
          const auto count = std::min(write_limit, bytes.size());
          auto frames = requests.feed(bytes.first(count));
          for (const auto& request : frames) {
            received.push_back(request);
            bool ack_frame = answer;
            if (ack_frame && request.command == 0x02U &&
                keyboard_acks_remaining >= 0) {
              if (keyboard_acks_remaining == 0) {
                ack_frame = false;
              } else {
                --keyboard_acks_remaining;
              }
            }
            if (!ack_frame) {
              continue;
            }
            kvmux::ch9329::Frame response{
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
            const auto encoded = kvmux::ch9329::encode(response);
            incoming.insert(incoming.end(), encoded.begin(), encoded.end());
          }
          return static_cast<std::ptrdiff_t>(count);
        },
        [this](std::span<std::uint8_t> bytes) -> std::ptrdiff_t {
          std::lock_guard lock(mutex);
          if (!opened) {
            return -1;
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

template <class Predicate>
bool eventually(Predicate predicate,
                std::chrono::milliseconds timeout = 1000ms) {
  const auto until = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < until) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return false;
}

}  // namespace

int main() {
  using namespace kvmux;
  FakeSerial fake;
  Ch9329ControlSink sink(fake.io());
  sink.connect("fake", 57600);
  require(eventually([&] {
            return sink.snapshot().state == ControlConnectionState::ready;
          }),
          "handshake and initial release reach ready");
  {
    std::lock_guard lock(fake.mutex);
    require(fake.received.size() >= 5 && fake.received[0].command == 0x01 &&
                fake.received[1].command == 0x08 &&
                fake.received[2].command == 0x02 &&
                fake.received[3].command == 0x04 &&
                fake.received[4].command == 0x05,
            "partial writes preserve handshake and full release order");
  }

  auto epoch = sink.snapshot().epoch;
  sink.set_control_active(true);
  require(sink.submit({epoch, 1, std::chrono::steady_clock::now(),
                       KeyEdge{0xe0, true}}) == SubmitResult::accepted,
          "control event accepted");
  require(eventually([&] {
            std::lock_guard lock(fake.mutex);
            return std::ranges::any_of(fake.received, [](const auto& frame) {
              return frame.command == 0x02 && !frame.data.empty() &&
                     frame.data[0] == 1;
            });
          }),
          "key down sent");
  require(eventually(
              [&] { return sink.snapshot().completed_ordinary_sequence == 1; }),
          "ordinary completion reports the ACKed ControlEvent sequence");

  const std::size_t paste_start = [&] {
    std::lock_guard lock(fake.mutex);
    return fake.received.size();
  }();
  AsciiPasteJob paste{{{{4, true}},
                       {{4, false}},
                       {{0xe1, true}, {30, true}, {30, false}, {0xe1, false}}}};
  require(sink.start_ascii_paste(std::move(paste)) == SubmitResult::accepted,
          "paste starts outside control queue");
  require(eventually([&] {
            return sink.ascii_paste_snapshot().state ==
                   AsciiPasteState::completed;
          }),
          "paste completes only after all HID ACKs");
  const auto paste_status = sink.ascii_paste_snapshot();
  require(
      paste_status.completed_gestures == 3 && paste_status.total_gestures == 3,
      "paste progress advances after final gesture edge ACK");
  {
    std::lock_guard lock(fake.mutex);
    require(fake.received.size() == paste_start + 6,
            "one report is sent per paste edge at ACK boundary");
  }

  // Release while a keyboard transaction awaits its ACK. The next epoch
  // must not rebuild a report from the previous held modifiers or keys.
  sink.release_all();
  require(!sink.snapshot().release_confirmed && sink.snapshot().epoch != epoch,
          "release immediately invalidates confirmation and epoch");
  require(eventually([&] {
            return sink.snapshot().state == ControlConnectionState::ready &&
                   sink.snapshot().release_confirmed;
          }),
          "release completed");
  epoch = sink.snapshot().epoch;
  require(sink.submit({epoch, 2, std::chrono::steady_clock::now(),
                       KeyEdge{4, true}}) == SubmitResult::accepted,
          "new epoch key accepted");
  require(eventually([&] {
            std::lock_guard lock(fake.mutex);
            return std::ranges::any_of(fake.received, [](const auto& frame) {
              return frame.command == 0x02 && frame.data.size() == 8 &&
                     frame.data[0] == 0 && frame.data[2] == 4;
            });
          }),
          "new epoch key does not retain old modifier");

  {
    std::lock_guard lock(fake.mutex);
    fake.answer = false;
  }
  require(sink.submit({epoch, 3, std::chrono::steady_clock::now(),
                       RelativeMotion{19, -7}}) == SubmitResult::accepted,
          "relative move accepted");
  require(eventually(
              [&] {
                return sink.snapshot().state == ControlConnectionState::stalled;
              },
              300ms),
          "late ack stalls and invalidates session");
  require(sink.snapshot().epoch != epoch, "stalled transaction advances epoch");
  require(eventually([&] { return sink.snapshot().timeout_count == 1; }, 800ms),
          "missing ack reaches hard timeout");

  std::size_t motion_count{};
  {
    std::lock_guard lock(fake.mutex);
    motion_count = std::ranges::count_if(fake.received, [](const auto& frame) {
      return frame.command == 0x05 && frame.data.size() == 5 &&
             frame.data[2] == 19;
    });
    fake.answer = true;
  }
  require(eventually(
              [&] {
                return sink.snapshot().state == ControlConnectionState::ready;
              },
              1200ms),
          "reconnect handshakes and clears before ready");
  {
    std::lock_guard lock(fake.mutex);
    require(std::ranges::count_if(
                fake.received,
                [](const auto& frame) {
                  return frame.command == 0x05 && frame.data.size() == 5 &&
                         frame.data[2] == 19;
                }) == static_cast<std::ptrdiff_t>(motion_count),
            "relative motion not replayed");
    require(std::ranges::any_of(fake.received,
                                [](const auto& frame) {
                                  return frame.command == 0x05 &&
                                         frame.data.size() == 5 &&
                                         frame.data[1] == 0 &&
                                         frame.data[2] == 0 &&
                                         frame.data[3] == 0;
                                }),
            "reconnect includes a clear report, not old input");
  }

  sink.disconnect();

  FakeSerial sync_fake;
  Ch9329ControlSink synced(sync_fake.io());
  synced.connect("sync", 57600);
  require(eventually([&] {
            return synced.snapshot().state == ControlConnectionState::ready;
          }),
          "sync handshake ready");
  synced.set_control_active(true);
  InputSync desired{
      synced.snapshot().epoch,
      7,
      11,
      {3, {4, 0, 5, 0, 0, 0}, 5, MouseMode::relative, 1234, 2345}};
  auto invalid = desired;
  invalid.state.keys[1] = 4;
  require(synced.synchronize(invalid) == SubmitResult::not_ready,
          "duplicate key rejected");
  invalid = desired;
  invalid.state.keys[0] = 0xe0;
  require(synced.synchronize(invalid) == SubmitResult::not_ready,
          "modifier usage rejected");
  invalid = desired;
  invalid.state.absolute_x = 4096;
  require(synced.synchronize(invalid) == SubmitResult::not_ready,
          "coordinate rejected");
  std::size_t start{};
  {
    std::lock_guard lock(sync_fake.mutex);
    sync_fake.answer = false;
    start = sync_fake.received.size();
  }
  auto sent = [&](std::size_t index, std::uint8_t command) {
    std::lock_guard lock(sync_fake.mutex);
    return sync_fake.received.size() > index &&
           sync_fake.received[index].command == command;
  };
  require(synced.synchronize(desired) == SubmitResult::accepted,
          "sync accepted");
  desired.revision = 12;
  desired.state.buttons = 2;
  require(synced.synchronize(desired) == SubmitResult::overloaded,
          "second sync cannot relabel");
  require(synced.submit({desired.epoch, 1, std::chrono::steady_clock::now(),
                         KeyEdge{6, true}}) == SubmitResult::overloaded,
          "ordinary input fenced by sync");
  require(eventually([&] { return sent(start, 0x02); }), "sync keyboard sent");
  require(!synced.snapshot().applied.known, "no ACK is not applied");
  sync_fake.ack(0x02);
  require(eventually([&] { return sent(start + 1, 0x05); }),
          "sync mouse follows keyboard ACK");
  require(!synced.snapshot().applied.known, "keyboard ACK alone insufficient");
  {
    std::lock_guard lock(sync_fake.mutex);
    require(sync_fake.received[start + 1].data ==
                std::vector<std::uint8_t>({1, 5, 0, 0, 0}),
            "relative sync has immutable buttons and zero deltas/wheel");
  }
  sync_fake.ack(0x05);
  require(eventually([&] { return synced.snapshot().applied.known; }),
          "two ACKs publish");
  const auto applied = synced.snapshot().applied;
  require(applied.epoch == desired.epoch && applied.intent_generation == 7 &&
              applied.revision == 11 && applied.state.modifiers == 3 &&
              applied.state.keys[0] == 4 && applied.state.keys[1] == 0 &&
              applied.state.keys[2] == 5 && applied.state.buttons == 5 &&
              applied.state.mode == MouseMode::relative &&
              applied.state.absolute_x == 1234 &&
              applied.state.absolute_y == 2345,
          "exact immutable applied state");
  require(synced.submit({desired.epoch, 2, std::chrono::steady_clock::now(),
                         KeyEdge{4, false}}) == SubmitResult::accepted,
          "healthy edge after sync");
  require(!synced.snapshot().applied.known, "ordinary event invalidates known");
  require(eventually([&] { return sent(start + 2, 0x02); }),
          "post-sync key report sent");
  {
    std::lock_guard lock(sync_fake.mutex);
    const auto& data = sync_fake.received[start + 2].data;
    require(data[0] == 3 && data[2] == 0 && data[3] == 0 && data[4] == 5,
            "internal held keys match sync");
  }
  sync_fake.ack(0x02);
  require(eventually([&] {
            synced.update_ui_heartbeat();
            return synced.synchronize(desired) == SubmitResult::accepted;
          }),
          "next sync after ordinary ACK");
  require(eventually([&] { return sent(start + 3, 0x02); }),
          "second keyboard sent");
  sync_fake.ack(0x02);
  require(eventually([&] { return sent(start + 4, 0x05); }),
          "second mouse sent");
  synced.release_all();
  require(!synced.snapshot().applied.known &&
              synced.snapshot().epoch != desired.epoch,
          "release fences revision immediately");
  sync_fake.ack(0x05);
  require(eventually([&] { return sent(start + 5, 0x02); }),
          "late mouse ACK starts neutral clear");
  require(!synced.snapshot().applied.known,
          "canceled two ACK snapshot never published");
  {
    std::lock_guard lock(sync_fake.mutex);
    require(
        sync_fake.received[start + 5].data == std::vector<std::uint8_t>(8, 0),
        "release clears held keys");
    sync_fake.answer = true;
  }
  sync_fake.ack(0x02);
  require(eventually([&] {
            return synced.snapshot().state == ControlConnectionState::ready;
          }),
          "canceled sync release completes");
  desired.epoch = synced.snapshot().epoch;
  require(synced.synchronize(desired) == SubmitResult::not_ready,
          "inactive control rejects sync");
  synced.set_control_active(true);
  desired.state.mode = MouseMode::absolute;
  require(synced.synchronize(desired) == SubmitResult::accepted,
          "fresh active sync accepted");
  require(eventually([&] { return synced.snapshot().applied.known; }),
          "sync known before timeout");
  {
    std::lock_guard lock(sync_fake.mutex);
    require(sync_fake.received.back().data ==
                ch9329::absolute_mouse_report(0, 2, 1234, 2345, 0).data,
            "absolute synchronization uses exact coordinates and zero wheel");
  }
  require(eventually([&] { return !synced.snapshot().applied.known; }, 400ms),
          "expired UI heartbeat invalidates applied state");
  require(synced.synchronize(desired) == SubmitResult::not_ready,
          "stale heartbeat rejects sync");
  require(eventually([&] {
            return synced.snapshot().state == ControlConnectionState::ready;
          }),
          "heartbeat expiry completes neutral clear");
  desired.epoch = synced.snapshot().epoch;
  synced.set_control_active(true);
  require(synced.synchronize(desired) == SubmitResult::accepted,
          "fresh epoch resumes sync");
  require(eventually([&] { return synced.snapshot().applied.known; }),
          "fresh epoch applies");
  {
    std::lock_guard lock(sync_fake.mutex);
    sync_fake.answer = false;
  }
  require(synced.synchronize(desired) == SubmitResult::accepted,
          "timeout sync accepted");
  require(eventually(
              [&] {
                return synced.snapshot().state ==
                       ControlConnectionState::stalled;
              },
              300ms),
          "sync ACK timeout stalls");
  require(!synced.snapshot().applied.known, "stall invalidates applied state");
  require(
      eventually([&] { return synced.snapshot().timeout_count == 1; }, 800ms),
      "sync hard timeout faults link");
  require(!synced.snapshot().applied.known, "fault cannot publish sync");
  synced.disconnect();

  // Serial-thread timings reach the shared Diagnostics only for ordinary input.
  FakeSerial timed;
  auto diagnostics = std::make_shared<Diagnostics>();
  Ch9329ControlSink measured(timed.io(), diagnostics);
  measured.connect("fake", 57600);
  require(eventually([&] {
            return measured.snapshot().state == ControlConnectionState::ready;
          }),
          "measured sink ready");
  require(diagnostics->snapshot().ack_rtt.samples == 0 &&
              diagnostics->snapshot().input_queue_age.samples == 0,
          "handshake and release are not ordinary input timings");
  measured.set_control_active(true);
  const auto stamped = std::chrono::steady_clock::now();
  require(measured.submit({measured.snapshot().epoch, 1, stamped,
                           RelativeMotion{3, 4}, stamped}) ==
              SubmitResult::accepted,
          "measured motion accepted");
  require(eventually([&] {
            return measured.snapshot().completed_ordinary_sequence == 1;
          }),
          "measured motion ACKed");
  const auto timing = diagnostics->snapshot();
  require(timing.ack_rtt.samples == 1 && timing.input_queue_age.samples == 1 &&
              timing.sdl_to_control_dequeue.samples == 1 &&
              timing.timeouts == 0,
          "ordinary input records queue age, receipt-to-dequeue and ACK RTT");
  const auto unstamped = std::chrono::steady_clock::now();
  require(measured.submit({measured.snapshot().epoch, 2, unstamped,
                           RelativeMotion{1, 1}}) == SubmitResult::accepted,
          "unstamped motion accepted");
  require(eventually([&] {
            return measured.snapshot().completed_ordinary_sequence == 2;
          }),
          "unstamped motion ACKed");
  require(diagnostics->snapshot().input_queue_age.samples == 2 &&
              diagnostics->snapshot().sdl_to_control_dequeue.samples == 1,
          "events without UI receipt time skip receipt-to-dequeue");
  measured.disconnect();

  FakeSerial dropping;
  Ch9329ControlSink dropped(dropping.io());
  dropped.connect("fake", 57600);
  require(eventually([&] {
            return dropped.snapshot().state == ControlConnectionState::ready;
          }),
          "disconnect fixture reaches ready");
  dropped.set_control_active(true);
  dropped.update_ui_heartbeat();
  {
    std::lock_guard lock(dropping.mutex);
    dropping.answer = false;
  }
  require(dropped.start_ascii_paste({{{{4, true}}, {{4, false}}}}) ==
              SubmitResult::accepted,
          "paste is active before disconnect");
  dropped.disconnect();
  const auto dropped_paste = dropped.ascii_paste_snapshot();
  require(dropped_paste.cancel_reason != PasteCancelReason::user &&
              dropped_paste.cancel_reason == PasteCancelReason::link_lost,
          "disconnect cancels paste as link_lost");

  // Cancel inside a Windows hex sequence. The first edge is ACKed. The next
  // edge stays in flight, then cancel takes the common clear path.
  FakeSerial hex_serial;
  Ch9329ControlSink hex_sink(hex_serial.io());
  hex_sink.connect("fake", 57600);
  require(eventually([&] {
            return hex_sink.snapshot().state == ControlConnectionState::ready;
          }),
          "hex paste sink ready");
  hex_sink.set_control_active(true);
  {
    std::lock_guard lock(hex_serial.mutex);
    hex_serial.keyboard_acks_remaining = 1;
  }
  const auto hex_edges = map_windows_hex_scalar(0x4e2d);
  require(static_cast<bool>(hex_edges) && hex_edges.edges.size() == 12,
          "hex fixture has the twelve-edge sequence");
  AsciiPasteJob hex_job;
  hex_job.gestures.push_back(hex_edges.edges);
  require(
      hex_sink.start_ascii_paste(std::move(hex_job)) == SubmitResult::accepted,
      "hex paste accepted");
  require(eventually([&] {
            std::lock_guard lock(hex_serial.mutex);
            std::size_t keyboards = 0;
            for (const auto& frame : hex_serial.received) {
              if (frame.command == 0x02U) ++keyboards;
            }
            // Handshake clear plus Alt down and the held keypad-plus frame.
            return keyboards >= 3;
          }),
          "hex paste holds the second keyboard frame");
  hex_sink.cancel_ascii_paste();
  {
    std::lock_guard lock(hex_serial.mutex);
    hex_serial.keyboard_acks_remaining = -1;
  }
  hex_serial.ack(0x02);
  require(eventually([&] {
            return hex_sink.snapshot().release_confirmed &&
                   hex_sink.snapshot().state == ControlConnectionState::ready;
          }),
          "cancel releases Alt through the common clear path");
  {
    std::lock_guard lock(hex_serial.mutex);
    std::vector<ch9329::Frame> keys;
    for (const auto& frame : hex_serial.received) {
      if (frame.command == 0x02U) keys.push_back(frame);
    }
    require(keys.size() >= 4, "clear adds a zero keyboard report");
    require(keys[1].data.size() == 8 && keys[1].data[0] == 0x04 &&
                keys[1].data[2] == 0,
            "first paste frame is Left Alt alone");
    require(keys[2].data.size() == 8 && keys[2].data[0] == 0x04 &&
                keys[2].data[2] == 0x57,
            "second paste frame is Left Alt and keypad plus");
    const auto zero = [](const ch9329::Frame& frame) {
      return frame.data.size() == 8 &&
             std::ranges::all_of(frame.data,
                                 [](const auto byte) { return byte == 0; });
    };
    require(zero(keys.back()), "last keyboard report releases every key");
    bool saw_plus = false;
    bool saw_clear = false;
    for (std::size_t index = 1; index < keys.size(); ++index) {
      if (!saw_plus) {
        saw_plus = keys[index].data.size() == 8 && keys[index].data[2] == 0x57;
        continue;
      }
      if (!saw_clear) {
        saw_clear = zero(keys[index]);
        require(saw_clear, "Alt is released before another paste edge");
        continue;
      }
      require(zero(keys[index]), "no hex edge follows the release report");
    }
    require(saw_clear, "release report follows the in-flight Alt chord");
    for (const auto& frame : keys) {
      require(frame.data.size() < 3 || frame.data[2] != 0x5c,
              "cancel does not continue the hex digits");
    }
  }
  require(hex_sink.ascii_paste_snapshot().state == AsciiPasteState::canceled &&
              hex_sink.ascii_paste_snapshot().completed_gestures == 0,
          "canceled hex paste does not finish the scalar");

  FakeSerial stalled_hex;
  Ch9329ControlSink stalled_sink(stalled_hex.io());
  stalled_sink.connect("fake", 57600);
  require(eventually([&] {
            return stalled_sink.snapshot().state ==
                   ControlConnectionState::ready;
          }),
          "stalled hex sink ready");
  stalled_sink.set_control_active(true);
  {
    std::lock_guard lock(stalled_hex.mutex);
    stalled_hex.keyboard_acks_remaining = 1;
  }
  AsciiPasteJob stalled_job;
  stalled_job.gestures.push_back(map_windows_hex_scalar(0x4e2d).edges);
  require(stalled_sink.start_ascii_paste(std::move(stalled_job)) ==
              SubmitResult::accepted,
          "stalled hex paste accepted");
  require(eventually([&] {
            std::lock_guard lock(stalled_hex.mutex);
            return stalled_hex.keyboard_acks_remaining == 0;
          }),
          "stalled hex paste is waiting on an ACK");
  require(eventually(
              [&] {
                const auto state = stalled_sink.snapshot();
                return !state.release_confirmed &&
                       (state.state == ControlConnectionState::fault ||
                        state.state == ControlConnectionState::reconnecting);
              },
              1500ms),
          "ACK timeout does not confirm release");
  require(stalled_sink.ascii_paste_snapshot().cancel_reason ==
              PasteCancelReason::ack_timeout,
          "ACK timeout is the paste cancel reason");
  return EXIT_SUCCESS;
}
