#include "app/probe.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "control/ch341_usb.hpp"
#include "control/control_event.hpp"
#include "control/control_sink.hpp"
#include "input/us_ascii_text.hpp"
#include "video/capture/capture_source.hpp"

namespace kvmux {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kPulse = std::chrono::milliseconds(30);
constexpr std::uint8_t kLeftShift = 0xE1;
constexpr std::string_view kPasteProbe = "kvmux paste 0123456789\n";

const char* state_name(ControlConnectionState state) {
  switch (state) {
    case ControlConnectionState::disconnected:
      return "disconnected";
    case ControlConnectionState::opening:
      return "opening";
    case ControlConnectionState::monitoring:
      return "monitoring";
    case ControlConnectionState::clearing:
      return "clearing";
    case ControlConnectionState::ready:
      return "ready";
    case ControlConnectionState::stalled:
      return "stalled";
    case ControlConnectionState::reconnecting:
      return "reconnecting";
    case ControlConnectionState::fault:
      return "fault";
    case ControlConnectionState::stopping:
      return "stopping";
  }
  return "unknown";
}

void step_line(std::ostream& out, std::string_view status,
               std::string_view name, std::string_view detail) {
  std::string label(name);
  if (label.size() < 9) label.append(9 - label.size(), ' ');
  out << '[' << status << "] " << label << detail << '\n';
  out.flush();
}

std::string hex_byte(std::uint8_t value) {
  std::ostringstream text;
  text << "0x" << std::hex << std::setw(2) << std::setfill('0')
       << static_cast<unsigned>(value);
  return text.str();
}

bool open_succeeded(const ControlSnapshot& snap) {
  switch (snap.state) {
    case ControlConnectionState::monitoring:
    case ControlConnectionState::clearing:
    case ControlConnectionState::ready:
    case ControlConnectionState::stalled:
      return true;
    default:
      return false;
  }
}

// `connect` publishes `opening` before the port open returns. That state is
// not success: a missing device moves on to `reconnecting` with `error`.
bool open_failed(const ControlSnapshot& snap) {
  if (snap.state == ControlConnectionState::reconnecting ||
      snap.state == ControlConnectionState::fault ||
      snap.state == ControlConnectionState::stopping)
    return true;
  return snap.state == ControlConnectionState::disconnected &&
         !snap.error.empty();
}

bool session_broken(const ControlSnapshot& snap) {
  return snap.state == ControlConnectionState::fault ||
         snap.state == ControlConnectionState::reconnecting ||
         snap.state == ControlConnectionState::disconnected ||
         snap.state == ControlConnectionState::stopping;
}

std::string failure_text(const ControlSnapshot& snap, bool timed_out) {
  std::string text;
  if (snap.state == ControlConnectionState::fault ||
      snap.state == ControlConnectionState::reconnecting ||
      snap.state == ControlConnectionState::disconnected) {
    text = "故障 ";
  }
  text += "state=";
  text += state_name(snap.state);
  text += " release_confirmed=";
  text += snap.release_confirmed ? "true" : "false";
  if (!snap.error.empty()) {
    text += " error=";
    text += snap.error;
  } else if (timed_out) {
    text += " timed out";
  }
  return text;
}

struct SnapshotTrace {
  bool have{false};
  ControlConnectionState state{ControlConnectionState::disconnected};
  bool usb_ready{false};
  bool release_confirmed{false};
  std::uint64_t epoch{0};
  std::string error;

  void observe(const ControlSnapshot& snap) {
    if (!spdlog::should_log(spdlog::level::debug)) return;
    if (have && state == snap.state && usb_ready == snap.target_usb_ready &&
        release_confirmed == snap.release_confirmed && epoch == snap.epoch &&
        error == snap.error) {
      return;
    }
    have = true;
    state = snap.state;
    usb_ready = snap.target_usb_ready;
    release_confirmed = snap.release_confirmed;
    epoch = snap.epoch;
    error = snap.error;
    spdlog::debug(
        "probe control state={} usb_ready={} release_confirmed={} epoch={} "
        "error={}",
        state_name(state), usb_ready ? "true" : "false",
        release_confirmed ? "true" : "false", epoch,
        error.empty() ? "-" : error);
  }
};

const char* capture_state_name(CaptureState state) {
  switch (state) {
    case CaptureState::stopped:
      return "stopped";
    case CaptureState::starting:
      return "starting";
    case CaptureState::streaming:
      return "streaming";
    case CaptureState::stopping:
      return "stopping";
    case CaptureState::permission_denied:
      return "permission_denied";
    case CaptureState::fault:
      return "fault";
  }
  return "unknown";
}

struct Pulse {
  ControlSink& sink;
  bool declare_freshness{false};
  std::uint64_t sequence{0};

  void operator()() {
    sink.update_ui_heartbeat();
    if (!declare_freshness) return;
    // Same values do not refresh the control-role freshness deadline.
    ++sequence;
    sink.video_presented(1, sequence);
  }
};

enum class Poll { ok, timeout, fault };

struct PollResult {
  Poll status{Poll::timeout};
  ControlSnapshot snap;
};

template <typename Done>
PollResult poll_until(ControlSink& sink, Pulse& pulse, SnapshotTrace& trace,
                      int timeout_ms, Done done) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    pulse();
    auto snap = sink.snapshot();
    trace.observe(snap);
    if (session_broken(snap)) return {Poll::fault, std::move(snap)};
    if (done(snap)) return {Poll::ok, std::move(snap)};
    if (Clock::now() >= deadline) return {Poll::timeout, std::move(snap)};
    std::this_thread::sleep_for(kPulse);
  }
}

PollResult poll_open(ControlSink& sink, Pulse& pulse, SnapshotTrace& trace,
                     int timeout_ms) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    pulse();
    auto snap = sink.snapshot();
    trace.observe(snap);
    if (open_failed(snap)) return {Poll::fault, std::move(snap)};
    if (open_succeeded(snap)) return {Poll::ok, std::move(snap)};
    if (Clock::now() >= deadline) return {Poll::timeout, std::move(snap)};
    std::this_thread::sleep_for(kPulse);
  }
}

void release_and_hint(std::ostream& out, ControlSink& sink) {
  sink.release_all();
  sink.disconnect();
  out << "如需测试拔出：重新运行并在 ready 后拔掉设备\n";
  out.flush();
}

int fail_step(std::ostream& out, ControlSink& sink, std::string_view name,
              std::string_view detail) {
  step_line(out, "FAIL", name, detail);
  release_and_hint(out, sink);
  return 1;
}

std::string open_detail(const ProbeControlOptions& options) {
  if (options.serial) {
    const bool direct = options.target.starts_with(kCh341UsbPrefix);
    return std::string("backend=") + (direct ? "ch341-usb" : "system") +
           " port=" + options.target + " baud=" + std::to_string(options.baud);
  }
  return "backend=relay host=" + options.target +
         " port=" + std::to_string(options.control_port);
}

// Empty absolute sync. Relative motion still emits a relative report.
bool sync_matches(const ControlSnapshot& snap) {
  return snap.applied.known && snap.applied.intent_generation == 1 &&
         snap.applied.revision == 1 && snap.applied.epoch == snap.epoch &&
         snap.state == ControlConnectionState::ready && snap.target_usb_ready &&
         snap.release_confirmed;
}

PollResult synchronize_idle(ControlSink& sink, Pulse& pulse,
                            SnapshotTrace& trace, int timeout_ms) {
  sink.set_control_active(true);
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  bool in_flight = false;
  std::uint64_t sent_epoch = 0;
  for (;;) {
    pulse();
    auto snap = sink.snapshot();
    trace.observe(snap);
    if (session_broken(snap)) return {Poll::fault, std::move(snap)};
    if (sync_matches(snap)) return {Poll::ok, std::move(snap)};
    const bool epoch_changed = in_flight && snap.epoch != sent_epoch;
    if (!in_flight || epoch_changed) {
      InputSync sync;
      sync.epoch = snap.epoch;
      sync.intent_generation = 1;
      sync.revision = 1;
      sync.state.mode = MouseMode::absolute;
      const auto result = sink.synchronize(sync);
      if (result == SubmitResult::accepted) {
        in_flight = true;
        sent_epoch = sync.epoch;
      } else if (result == SubmitResult::overloaded && !epoch_changed) {
        in_flight = true;
      }
    }
    if (Clock::now() >= deadline) return {Poll::timeout, std::move(snap)};
    std::this_thread::sleep_for(kPulse);
  }
}

bool input_acknowledged(const ControlSnapshot& snap, std::uint64_t sequence) {
  return snap.completed_ordinary_sequence >= sequence &&
         !snap.ordinary_input_pending && snap.release_confirmed &&
         snap.target_usb_ready &&
         snap.state != ControlConnectionState::clearing &&
         !session_broken(snap);
}

PollResult send_and_wait(ControlSink& sink, Pulse& pulse, SnapshotTrace& trace,
                         int timeout_ms, ControlPayload payload,
                         std::uint64_t& sequence) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  std::uint64_t sent = 0;
  bool accepted = false;
  for (;;) {
    pulse();
    auto snap = sink.snapshot();
    trace.observe(snap);
    if (session_broken(snap)) return {Poll::fault, std::move(snap)};
    if (accepted && input_acknowledged(snap, sent))
      return {Poll::ok, std::move(snap)};
    if (!accepted && snap.state == ControlConnectionState::ready &&
        snap.target_usb_ready && snap.release_confirmed) {
      ControlEvent event;
      event.epoch = snap.epoch;
      event.sequence = sequence;
      event.timestamp = Clock::now();
      event.payload = payload;
      if (sink.submit(event) == SubmitResult::accepted) {
        sent = sequence;
        ++sequence;
        accepted = true;
      }
    }
    if (Clock::now() >= deadline) return {Poll::timeout, std::move(snap)};
    std::this_thread::sleep_for(kPulse);
  }
}

const char* paste_state_name(AsciiPasteState state) {
  switch (state) {
    case AsciiPasteState::idle:
      return "idle";
    case AsciiPasteState::active:
      return "active";
    case AsciiPasteState::completed:
      return "completed";
    case AsciiPasteState::canceled:
      return "canceled";
    case AsciiPasteState::failed:
      return "failed";
  }
  return "unknown";
}

std::string step_failure(const PollResult& result) {
  if (result.status == Poll::fault || result.snap.error.empty())
    return failure_text(result.snap, result.status != Poll::fault);
  return result.snap.error;
}

}  // namespace

int probe_control(std::ostream& out, ControlSink& sink,
                  const ProbeControlOptions& options) {
  Pulse pulse{sink, options.declare_freshness, 0};
  SnapshotTrace trace;
  sink.connect(options.target, options.baud);
  const auto opened = poll_open(sink, pulse, trace, options.timeout_ms);
  if (opened.status != Poll::ok) {
    const std::string detail = !opened.snap.error.empty()
                                   ? opened.snap.error
                                   : failure_text(opened.snap, true);
    return fail_step(out, sink, "open", detail);
  }
  step_line(out, "PASS", "open", open_detail(options));

  if (options.declare_freshness) {
    out << "无画面，CLI 自行声明新鲜，仅用于自检\n";
    out.flush();
  }
  const auto ready =
      poll_until(sink, pulse, trace, options.timeout_ms, [](const auto& snap) {
        return snap.state == ControlConnectionState::ready &&
               snap.target_usb_ready;
      });
  if (ready.status != Poll::ok)
    return fail_step(out, sink, "ready", step_failure(ready));
  step_line(out, "PASS", "ready",
            "usb=ready version=" + hex_byte(ready.snap.chip_version));

  bool synced = false;
  auto require_sync = [&](std::string_view step) -> int {
    if (synced) return 0;
    const auto result =
        synchronize_idle(sink, pulse, trace, options.timeout_ms);
    if (result.status != Poll::ok)
      return fail_step(out, sink, step, step_failure(result));
    synced = true;
    return 0;
  };

  std::uint64_t sequence = 1;
  if (!options.keys) {
    step_line(out, "SKIP", "keys", "(pass --keys to send Left Shift)");
  } else {
    if (const int failed = require_sync("keys")) return failed;
    const auto pressed = send_and_wait(sink, pulse, trace, options.timeout_ms,
                                       KeyEdge{kLeftShift, true}, sequence);
    if (pressed.status != Poll::ok)
      return fail_step(out, sink, "keys", step_failure(pressed));
    const auto released = send_and_wait(sink, pulse, trace, options.timeout_ms,
                                        KeyEdge{kLeftShift, false}, sequence);
    if (released.status != Poll::ok)
      return fail_step(out, sink, "keys", step_failure(released));
    step_line(out, "PASS", "keys", "Left Shift acknowledged");
  }

  if (!options.mouse) {
    step_line(out, "SKIP", "mouse", "(pass --mouse to move the pointer)");
  } else {
    if (const int failed = require_sync("mouse")) return failed;
    const auto right = send_and_wait(sink, pulse, trace, options.timeout_ms,
                                     RelativeMotion{20, 0}, sequence);
    if (right.status != Poll::ok)
      return fail_step(out, sink, "mouse", step_failure(right));
    const auto left = send_and_wait(sink, pulse, trace, options.timeout_ms,
                                    RelativeMotion{-20, 0}, sequence);
    if (left.status != Poll::ok)
      return fail_step(out, sink, "mouse", step_failure(left));
    step_line(out, "PASS", "mouse", "relative +20,-20 acknowledged");
  }

  if (options.paste) {
    if (const int failed = require_sync("paste")) return failed;
    std::vector<std::uint8_t> normalized;
    auto mapped = map_us_ascii_text(kPasteProbe, &normalized);
    if (!mapped) return fail_step(out, sink, "paste", "mapping failed");
    const auto paste_started = Clock::now();
    SubmitResult submitted = SubmitResult::not_ready;
    if (options.serial) {
      AsciiPasteJob job;
      job.gestures.reserve(mapped.gestures.size());
      for (auto& gesture : mapped.gestures)
        job.gestures.push_back(std::move(gesture.edges));
      submitted = sink.start_ascii_paste(std::move(job));
    } else {
      submitted = sink.start_ascii_paste_text(std::move(normalized));
    }
    if (submitted != SubmitResult::accepted)
      return fail_step(out, sink, "paste", "not started");
    const auto deadline =
        Clock::now() + std::chrono::milliseconds(options.timeout_ms);
    AsciiPasteSnapshot last;
    AsciiPasteState seen = AsciiPasteState::idle;
    std::size_t seen_done = static_cast<std::size_t>(-1);
    for (;;) {
      pulse();
      last = sink.ascii_paste_snapshot();
      if (last.state != seen || last.completed_gestures != seen_done) {
        seen = last.state;
        seen_done = last.completed_gestures;
        out << "paste stage=" << paste_state_name(last.state)
            << " sent=" << last.completed_gestures << "/" << last.total_gestures
            << " reason=" << paste_cancel_reason_name(last.cancel_reason)
            << '\n';
        out.flush();
      }
      if (last.state == AsciiPasteState::completed ||
          last.state == AsciiPasteState::canceled ||
          last.state == AsciiPasteState::failed)
        break;
      if (Clock::now() >= deadline)
        return fail_step(out, sink, "paste", "timed out");
      std::this_thread::sleep_for(kPulse);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - paste_started);
    const bool ok = last.state == AsciiPasteState::completed &&
                    last.cancel_reason == PasteCancelReason::none;
    spdlog::info(
        "paste finished result={} reason={} sent_bytes={} total_bytes={} "
        "elapsed_ms={}",
        paste_state_name(last.state),
        paste_cancel_reason_name(last.cancel_reason), last.completed_gestures,
        last.total_gestures, elapsed.count());
    const std::string detail =
        std::string("result=") + paste_state_name(last.state) +
        " reason=" + paste_cancel_reason_name(last.cancel_reason) +
        " sent=" + std::to_string(last.completed_gestures) + "/" +
        std::to_string(last.total_gestures);
    if (!ok) return fail_step(out, sink, "paste", detail);
    step_line(out, "PASS", "paste", detail);
  }

  const auto started = Clock::now();
  sink.release_all();
  const auto released =
      poll_until(sink, pulse, trace, options.timeout_ms, [](const auto& snap) {
        return snap.release_confirmed &&
               snap.state != ControlConnectionState::clearing;
      });
  if (released.status != Poll::ok)
    return fail_step(out, sink, "release", step_failure(released));
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      Clock::now() - started);
  step_line(out, "PASS", "release",
            "confirmed in " + std::to_string(elapsed.count()) + " ms");
  sink.disconnect();
  out << "如需测试拔出：重新运行并在 ready 后拔掉设备\n";
  out.flush();
  return 0;
}

int probe_video(std::ostream& out, CaptureSource& source, int seconds) {
  out << "帧数与最长间隔只是接收计数，不是延迟或性能结论\n";
  out.flush();
  source.start({});
  const auto deadline = Clock::now() + std::chrono::seconds(seconds);
  auto next_stats = Clock::now() + std::chrono::seconds(1);
  std::uint64_t observed = 0;
  std::string codec;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  bool announced = false;
  Clock::time_point previous{};
  bool have_previous = false;
  auto longest = std::chrono::milliseconds::zero();
  const auto log_stats = [&](const CaptureSnapshot& snap) {
    spdlog::debug(
        "probe video state={} observed={} received={} codec={} size={}x{} "
        "longest_gap_ms={} error={}",
        capture_state_name(snap.state), observed, snap.received_samples,
        codec.empty() ? std::string_view("-") : std::string_view(codec), width,
        height, longest.count(),
        snap.error.empty() ? std::string_view("-")
                           : std::string_view(snap.error));
  };
  while (Clock::now() < deadline) {
    const auto snap = source.snapshot();
    if (Clock::now() >= next_stats) {
      log_stats(snap);
      next_stats += std::chrono::seconds(1);
    }
    if (snap.state == CaptureState::fault ||
        snap.state == CaptureState::permission_denied) {
      log_stats(snap);
      const std::string detail =
          snap.error.empty() ? std::string("capture fault") : snap.error;
      step_line(out, "FAIL", announced ? "video" : "handshake", detail);
      source.stop();
      return 1;
    }
    if (codec.empty() && !snap.actual_mode.device_format_name.empty()) {
      codec = snap.actual_mode.device_format_name;
      width = snap.actual_mode.width;
      height = snap.actual_mode.height;
    }
    if (auto sample = source.take_latest_sample()) {
      if (width == 0) {
        width = sample->width;
        height = sample->height;
      }
      if (have_previous && sample->arrival > previous) {
        const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(
            sample->arrival - previous);
        if (gap > longest) longest = gap;
      }
      previous = sample->arrival;
      have_previous = true;
      ++observed;
    }
    if (!announced && (!codec.empty() || observed > 0)) {
      step_line(out, "PASS", "handshake",
                "codec=" + (codec.empty() ? std::string("unknown") : codec) +
                    " size=" + std::to_string(width) + "x" +
                    std::to_string(height));
      announced = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  source.stop();
  const auto snap = source.snapshot();
  const auto received = snap.received_samples;
  const auto frames = received > observed ? received : observed;
  if (snap.state == CaptureState::fault ||
      snap.state == CaptureState::permission_denied || frames == 0) {
    std::string detail = "frames=" + std::to_string(frames);
    if (!snap.error.empty()) detail += " error=" + snap.error;
    step_line(out, "FAIL", "video", detail);
    return 1;
  }
  step_line(out, "PASS", "video",
            "codec=" + (codec.empty() ? std::string("unknown") : codec) +
                " size=" + std::to_string(width) + "x" +
                std::to_string(height) + " frames=" + std::to_string(frames) +
                " longest_gap_ms=" + std::to_string(longest.count()));
  return 0;
}

}  // namespace kvmux
