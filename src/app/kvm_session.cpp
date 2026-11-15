#include "app/kvm_session.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace kvmux {
namespace {
constexpr auto kCapturePoll = std::chrono::milliseconds(25);
constexpr auto kStaleAfter = std::chrono::milliseconds(500);
constexpr auto kKeepAliveInterval = std::chrono::seconds(60);
constexpr std::array kReconnectBackoff{
    std::chrono::milliseconds(500), std::chrono::milliseconds(1000),
    std::chrono::milliseconds(2000), std::chrono::milliseconds(5000)};

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
      return "permission-denied";
    case CaptureState::fault:
      return "fault";
  }
  return "unknown";
}

const char* device_name(const DeviceInfo& device) {
  return device.display_name.empty() ? "(unnamed)"
                                     : device.display_name.c_str();
}

std::string capture_mode_label(const CaptureMode& mode) {
  return std::to_string(mode.width) + "x" + std::to_string(mode.height) +
         " @ " + std::to_string(mode.frame_rate.numerator) + "/" +
         std::to_string(mode.frame_rate.denominator) + " " +
         (mode.device_format_name.empty() ? "?" : mode.device_format_name);
}

void log_capture_devices(const std::vector<DeviceInfo>& devices) {
  if (!spdlog::should_log(spdlog::level::debug)) return;
  std::string names;
  for (const auto& device : devices) {
    if (!names.empty()) names += "; ";
    names += device_name(device);
  }
  spdlog::debug("Capture devices: count={} result={} names=[{}]",
                devices.size(), devices.empty() ? "empty" : "ok", names);
}

void log_capture_modes(const std::vector<CaptureMode>& modes) {
  if (!spdlog::should_log(spdlog::level::debug)) return;
  std::string listed;
  for (const auto& mode : modes) {
    if (!listed.empty()) listed += "; ";
    listed += capture_mode_label(mode);
  }
  spdlog::debug("Capture modes: count={} result={} modes=[{}]", modes.size(),
                modes.empty() ? "empty" : "ok", listed);
}

}  // namespace

KvmSession::KvmSession(const std::shared_ptr<Diagnostics>& diagnostics)
    : KvmSession(create_platform_capture_source(),
                 std::make_unique<Ch9329ControlSink>(diagnostics),
                 diagnostics) {}

KvmSession::KvmSession(std::unique_ptr<CaptureSource> capture,
                       std::unique_ptr<ControlSink> control,
                       std::shared_ptr<Diagnostics> diagnostics)
    : capture_(std::move(capture)),
      control_(std::move(control)),
      video_(*capture_),
      diagnostics_(std::move(diagnostics)),
      input_(*control_),
      capture_worker_([this] { capture_loop(); }),
      mouse_worker_([this] { mouse_loop(); }) {}

KvmSession::~KvmSession() { shutdown(); }

InputState KvmSession::input_state() const noexcept {
  std::lock_guard lock(input_mutex_);
  return input_.state();
}

bool KvmSession::text_paste_active() const noexcept {
  std::lock_guard lock(input_mutex_);
  return input_.text_active();
}

bool KvmSession::preview_only() const noexcept {
  std::lock_guard lock(input_mutex_);
  return input_.state() == InputState::preview;
}

bool KvmSession::control_blocked() const noexcept {
  return capture_only_.load(std::memory_order_acquire);
}

void KvmSession::set_capture_only(bool enabled) noexcept {
  if (capture_only_.exchange(enabled, std::memory_order_acq_rel) == enabled)
    return;
  // Toggling on must walk the same release path as focus loss etc., so a
  // previously connected session cannot keep half its HID state on target.
  if (enabled) {
    request_release(PasteCancelReason::link_lost);
    control_->disconnect();
  }
}

void KvmSession::note_session_error(std::string error) {
  std::lock_guard lock(mutex_);
  session_error_ = std::move(error);
}

std::future<std::vector<DeviceInfo>> KvmSession::enumerate_capture_devices() {
  auto task =
      std::make_shared<std::packaged_task<std::vector<DeviceInfo>()>>([this] {
        try {
          auto devices = capture_->enumerate_devices();
          log_capture_devices(devices);
          return devices;
        } catch (const std::exception& error) {
          spdlog::debug("Capture devices: count=0 result=failed error={}",
                        error.what());
          throw;
        }
      });
  auto future = task->get_future();
  {
    std::lock_guard lock(mutex_);
    capture_tasks_.push_back([task] { (*task)(); });
  }
  wake_.notify_one();
  return future;
}

std::future<std::vector<CaptureMode>> KvmSession::enumerate_capture_modes(
    std::string stable_id) {
  auto task = std::make_shared<std::packaged_task<std::vector<CaptureMode>()>>(
      [this, stable_id = std::move(stable_id)] {
        try {
          auto modes = capture_->enumerate_modes(stable_id);
          log_capture_modes(modes);
          return modes;
        } catch (const std::exception& error) {
          spdlog::debug("Capture modes: count=0 result=failed error={}",
                        error.what());
          throw;
        }
      });
  auto future = task->get_future();
  {
    std::lock_guard lock(mutex_);
    capture_tasks_.push_back([task] { (*task)(); });
  }
  wake_.notify_one();
  return future;
}

bool KvmSession::select_capture(const DeviceInfo& device,
                                const CaptureMode& mode) {
  if (!preview_only() || mode.device_id != device.stable_id ||
      device.stable_id.empty()) {
    const char* reason = "id-mismatch";
    if (!preview_only())
      reason = "not-preview";
    else if (device.stable_id.empty())
      reason = "empty-id";
    spdlog::debug("Capture start: device={} mode={} result=rejected reason={}",
                  device_name(device), capture_mode_label(mode), reason);
    return false;
  }
  request_release(PasteCancelReason::video_stale);
  bool rejected = false;
  {
    std::lock_guard lock(mutex_);
    if (shutting_down_) {
      rejected = true;
    } else {
      selected_device_ = device;
      selected_mode_ = mode;
      requested_mode_ = mode;
      command_ = CaptureCommand::start;
      video_state_ = SessionVideoState::stopping;
      session_error_.clear();
      video_fresh_ = false;
      last_sample_arrival_ = {};
    }
  }
  if (rejected) {
    spdlog::debug(
        "Capture start: device={} mode={} result=rejected "
        "reason=shutting-down",
        device_name(device), capture_mode_label(mode));
    return false;
  }
  {
    std::lock_guard lock(input_mutex_);
    input_.set_video_fresh(false);
  }
  wake_.notify_one();
  spdlog::debug("Capture start: device={} mode={} result=accepted",
                device_name(device), capture_mode_label(mode));
  return true;
}

bool KvmSession::stop_capture() {
  if (!preview_only()) return false;
  request_release(PasteCancelReason::video_stale);
  {
    std::lock_guard lock(mutex_);
    if (shutting_down_) return false;
    command_ = CaptureCommand::stop;
    requested_mode_.reset();
    selected_mode_.reset();
    selected_device_.reset();
    video_state_ = SessionVideoState::stopping;
    video_fresh_ = false;
    last_sample_arrival_ = {};
  }
  {
    std::lock_guard lock(input_mutex_);
    input_.set_video_fresh(false);
  }
  wake_.notify_one();
  return true;
}

bool KvmSession::connect_control(std::string port, int baud_rate,
                                 std::uint8_t address) {
  if (!preview_only() || control_blocked() || port.empty() || baud_rate <= 0)
    return false;
  {
    std::lock_guard lock(mutex_);
    if (shutting_down_) return false;
  }
  control_->connect(std::move(port), baud_rate, address);
  return true;
}

bool KvmSession::disconnect_control() {
  if (!preview_only()) return false;
  request_release(PasteCancelReason::link_lost);
  control_->disconnect();
  return true;
}

void KvmSession::set_host_key(std::uint16_t usage) noexcept {
  std::lock_guard lock(input_mutex_);
  input_.set_host_key(usage);
}
void KvmSession::set_relative_gain(double gain) noexcept {
  std::lock_guard lock(input_mutex_);
  drain_motion_locked();  // Pending motion keeps the gain it arrived under.
  input_.set_relative_gain(gain);
}

void KvmSession::set_keep_alive(bool enabled) noexcept {
  keep_alive_enabled_ = enabled;
  last_control_activity_ = Clock::now();
}

// Caller holds input_mutex_.
void KvmSession::note_keep_alive(Clock::time_point now) noexcept {
  if (input_.capture_intended() || input_.injected_active()) {
    last_control_activity_ = now;
  } else if (keep_alive_enabled_ && !control_blocked() &&
             input_.state() == InputState::preview &&
             now - last_control_activity_ >= kKeepAliveInterval) {
    last_control_activity_ = now;
    (void)input_.send_keep_alive(now);
  }
}
bool KvmSession::send_special(SpecialKeys keys) {
  std::lock_guard lock(input_mutex_);
  if (control_blocked()) return false;
  drain_motion_locked();
  return video_fresh_ && input_.send_special(keys);
}
TextMappingResult KvmSession::start_text_paste(const std::string_view text) {
  std::lock_guard lock(input_mutex_);
  if (control_blocked()) {
    auto blocked = map_us_ascii_text(text);
    if (blocked) {
      blocked.error = TextPasteError::canceled;
      blocked.cancel_reason = PasteCancelReason::not_ready;
    }
    return blocked;
  }
  drain_motion_locked();
  control_->set_control_active(true);
  auto result = input_.start_text(text);
  if (!result) control_->set_control_active(false);
  return result;
}
TextMappingResult KvmSession::start_windows_hex_paste(
    const std::string_view text) {
  std::lock_guard lock(input_mutex_);
  if (control_blocked()) {
    const auto plan = map_windows_hex_paste(text);
    TextMappingResult blocked;
    blocked.error = plan.error;
    blocked.source_bytes = plan.source_bytes;
    blocked.normalized_characters = plan.normalized_characters;
    if (blocked) {
      blocked.error = TextPasteError::canceled;
      blocked.cancel_reason = PasteCancelReason::not_ready;
    }
    log_windows_hex_paste("refused",
                          windows_hex_paste_error_name(blocked.error),
                          paste_cancel_reason_name(blocked.cancel_reason),
                          blocked.normalized_characters, 0, 0, plan.rejected);
    return blocked;
  }
  drain_motion_locked();
  control_->set_control_active(true);
  auto result = input_.start_windows_hex_text(text);
  if (!result) control_->set_control_active(false);
  return result;
}
void KvmSession::cancel_text_paste() noexcept {
  std::lock_guard lock(input_mutex_);
  input_.cancel_text(PasteCancelReason::user);
  control_->set_control_active(false);
}
TextMappingResult KvmSession::text_paste_snapshot() const {
  std::lock_guard lock(input_mutex_);
  return input_.text_snapshot();
}
TextPasteSnapshot KvmSession::text_paste_progress() const noexcept {
  std::lock_guard lock(input_mutex_);
  return input_.text_paste_snapshot();
}

bool KvmSession::set_mouse_mode(MouseMode mode) {
  std::lock_guard lock(input_mutex_);
  if (input_.state() != InputState::preview) return false;
  if (input_.text_active())
    log_unknown_paste_cancel("KvmSession::set_mouse_mode");
  request_release_locked(PasteCancelReason::unknown);
  input_.set_mouse_mode(mode);
  control_->set_mouse_mode(mode);
  return true;
}

void KvmSession::set_video_rect(Rect rect) noexcept {
  std::lock_guard lock(input_mutex_);
  // Pending motion keeps the rectangle it was received under.
  drain_motion_locked();
  input_.set_video_rect(rect);
}
void KvmSession::activate_control() noexcept {
  std::lock_guard lock(input_mutex_);
  if (control_blocked()) return;
  drain_motion_locked();
  input_.activate();
}

void KvmSession::apply_input_locked(const InputEvent& event) {
  control_->set_control_active(input_.capture_intended() ||
                               input_.injected_active());
  input_.handle(event);
  if (diagnostics_ && event.received_at != Clock::time_point{})
    diagnostics_->record_input_processing(Clock::now() - event.received_at);
}

void KvmSession::handle_input(const InputEvent& event) {
  if (control_blocked()) return;
  if (std::holds_alternative<InputRelativeMotion>(event.payload) ||
      std::holds_alternative<InputPointerMotion>(event.payload)) {
    queue_motion(event);
    return;
  }
  // Edges never overtake earlier motion: apply it first, under the same lock.
  std::lock_guard lock(input_mutex_);
  drain_motion_locked();
  apply_input_locked(event);
}

void KvmSession::queue_motion(const InputEvent& event) {
  {
    std::lock_guard lock(motion_mutex_);
    if (motion_stop_) return;
    if (!pending_motion_) {
      pending_motion_ = PendingMotion{event, Clock::now()};
      motion_wake_.notify_one();
      return;
    }
    // Only adjacent motion of the same kind folds; relative deltas add and
    // absolute motion keeps the last position. The slot keeps the oldest
    // enqueue and receipt times so age and latency are not understated.
    auto& held = *pending_motion_;
    if (auto* total = std::get_if<InputRelativeMotion>(&held.event.payload)) {
      if (const auto* next = std::get_if<InputRelativeMotion>(&event.payload)) {
        total->dx += next->dx;
        total->dy += next->dy;
        if (diagnostics_) diagnostics_->record_mouse_motion_folded();
        return;
      }
    } else if (std::holds_alternative<InputPointerMotion>(event.payload)) {
      held.event.payload = event.payload;
      if (diagnostics_) diagnostics_->record_mouse_motion_folded();
      return;
    }
  }
  // Motion kind changed: apply the older one before holding the new one.
  std::lock_guard lock(input_mutex_);
  drain_motion_locked();
  std::lock_guard motion_lock(motion_mutex_);
  if (motion_stop_) return;
  pending_motion_ = PendingMotion{event, Clock::now()};
  motion_wake_.notify_one();
}

void KvmSession::discard_motion() noexcept {
  std::lock_guard lock(motion_mutex_);
  pending_motion_.reset();
}

void KvmSession::drain_motion_locked() {
  std::optional<PendingMotion> motion;
  {
    std::lock_guard lock(motion_mutex_);
    motion = pending_motion_;
    pending_motion_.reset();
  }
  if (!motion) return;
  if (Clock::now() - motion->queued_at > ControlQueue::maximum_age) {
    // Same rule as the control queue: input older than 100 ms is never
    // replayed; a controlled session releases instead.
    if (input_.capture_intended() || input_.injected_active())
      request_release_locked(PasteCancelReason::ui_stall);
    return;
  }
  apply_input_locked(motion->event);
}

void KvmSession::mouse_loop() {
  for (;;) {
    {
      std::unique_lock lock(motion_mutex_);
      motion_wake_.wait(lock,
                        [this] { return motion_stop_ || pending_motion_; });
      if (motion_stop_) return;
    }
    std::lock_guard lock(input_mutex_);
    drain_motion_locked();
  }
}

void KvmSession::request_release_locked(
    const PasteCancelReason reason) noexcept {
  {
    std::lock_guard lock(motion_mutex_);
    pending_motion_.reset();  // A release must never be followed by old motion.
  }
  input_.release(reason);
  control_->set_control_active(false);
  control_->release_all();
}

void KvmSession::request_release(const PasteCancelReason reason) noexcept {
  discard_motion();
  std::lock_guard lock(input_mutex_);
  request_release_locked(reason);
}

void KvmSession::focus_lost() noexcept {
  discard_motion();
  std::lock_guard lock(input_mutex_);
  // request_release_locked() drops control and ReleaseAll. That cancels a
  // paste and releases Alt. Leave the paste's control lease and keyboard
  // report alone; focus_lost() releases only keys the router had forwarded.
  if (input_.text_active()) {
    input_.focus_lost();
    return;
  }
  request_release_locked(PasteCancelReason::focus_lost);
  input_.focus_lost();
}
void KvmSession::minimized() noexcept {
  discard_motion();
  std::lock_guard lock(input_mutex_);
  if (input_.text_active()) {
    input_.minimized();
    return;
  }
  request_release_locked(PasteCancelReason::focus_lost);
  input_.minimized();
}
void KvmSession::focus_returned() noexcept {
  std::lock_guard lock(input_mutex_);
  input_.focus_returned();
}
void KvmSession::suspended() noexcept {
  discard_motion();
  std::lock_guard lock(input_mutex_);
  request_release_locked(PasteCancelReason::focus_lost);
  input_.focus_lost();
}
void KvmSession::release_control(const PasteCancelReason reason) noexcept {
  request_release(reason);
}

void KvmSession::update_ui_heartbeat() noexcept {
  control_->update_ui_heartbeat();
}

void KvmSession::tick(Clock::time_point now) {
  control_->update_ui_heartbeat();
  const auto capture = capture_->snapshot();
  bool release = false;
  bool fresh = false;
  {
    std::lock_guard lock(mutex_);
    if (capture.generation != observed_generation_) {
      release = observed_generation_ != 0;
      observed_generation_ = capture.generation;
      observed_samples_ = capture.received_samples;
      last_sample_arrival_ =
          capture.state == CaptureState::streaming ? now : Clock::time_point{};
      received_current_generation_ = false;
      video_fresh_ = false;
      video_.set_generation(capture.generation);
    }
    if (capture.state == CaptureState::streaming &&
        observed_capture_state_ != CaptureState::streaming) {
      last_sample_arrival_ = now;
    }
    observed_capture_state_ = capture.state;
    if (capture.state == CaptureState::streaming &&
        (!observed_actual_mode_ ||
         *observed_actual_mode_ != capture.actual_mode)) {
      release = observed_actual_mode_.has_value();
      observed_actual_mode_ = capture.actual_mode;
      video_.set_generation(capture.generation);
      video_fresh_ = false;
      received_current_generation_ = false;
      last_sample_arrival_ = now;
    }
    if (capture.received_samples != observed_samples_) {
      observed_samples_ = capture.received_samples;
      last_sample_arrival_ = now;
      received_current_generation_ = true;
      video_fresh_ = true;
    }
    fresh = received_current_generation_ &&
            last_sample_arrival_ != Clock::time_point{} &&
            now - last_sample_arrival_ < kStaleAfter &&
            capture.state == CaptureState::streaming;
    if (video_fresh_ && !fresh) release = true;
    video_fresh_ = fresh;
    if (capture.state == CaptureState::permission_denied) {
      video_state_ = SessionVideoState::permission_denied;
    } else if (capture.state == CaptureState::fault) {
      video_state_ = SessionVideoState::fault;
    } else if (capture.state == CaptureState::streaming) {
      const bool timed_out = last_sample_arrival_ != Clock::time_point{} &&
                             now - last_sample_arrival_ >= kStaleAfter;
      video_state_ = fresh ? SessionVideoState::streaming
                           : (timed_out ? SessionVideoState::stale
                                        : SessionVideoState::connecting);
    }
  }
  const auto decoded = video_.snapshot();
  const bool video_ok = fresh && decoded.processed_frames > 0 &&
                        decoded.latest_arrival != Clock::time_point{} &&
                        now - decoded.latest_arrival < kStaleAfter;
  // The verdict is already known: drop motion now so the mouse thread cannot
  // drain it while this call waits for input_mutex_.
  if (!video_ok || release) discard_motion();
  std::lock_guard input_lock(input_mutex_);
  if ((input_.capture_intended() || input_.injected_active()) &&
      control_->snapshot().state != ControlConnectionState::ready)
    release = true;
  // Decide only after the freshness/release verdict: motion queued while the
  // video was being found invalid must not be sent across that boundary.
  if (!video_ok || release)
    discard_motion();
  else
    drain_motion_locked();
  input_.set_video_fresh(video_ok);
  if (release) {
    const auto paste = control_->ascii_paste_snapshot();
    auto reason = PasteCancelReason::link_lost;
    if (paste.cancel_reason != PasteCancelReason::none)
      reason = paste.cancel_reason;
    else if (!video_ok)
      reason = PasteCancelReason::video_stale;
    if (control_->snapshot().recoverable_transport)
      input_.video_stale(reason);
    else
      request_release_locked(reason);
  }
  control_->set_control_active(input_.capture_intended() ||
                               input_.injected_active());
  input_.clear_fault();
  note_keep_alive(now);
  input_.tick(now);
  control_->set_control_active(input_.capture_intended() ||
                               input_.injected_active());
}

std::optional<VideoFrame> KvmSession::take_latest_frame() {
  auto frame = video_.take_latest_frame();
  // A relay decoder reset advances local capture generation and fences already
  // processed output as well as compressed ingress.
  if (frame && frame->generation != capture_->snapshot().generation) return {};
  return frame;
}

void KvmSession::video_presented(std::uint64_t generation,
                                 std::uint64_t sequence) noexcept {
  control_->video_presented(generation, sequence);
}

KvmSessionSnapshot KvmSession::snapshot() const {
  KvmSessionSnapshot value;
  {
    std::lock_guard lock(mutex_);
    value.video_state = video_state_;
    value.video_fresh = video_fresh_;
    value.shutting_down = shutting_down_;
    value.serial_shutdown_timed_out = serial_shutdown_timed_out_;
    value.session_error = session_error_;
  }
  value.capture = capture_->snapshot();
  value.video = video_.snapshot();
  value.control = control_->snapshot();
  {
    std::lock_guard lock(input_mutex_);
    value.input_state = input_.state();
    value.pointer = input_.pointer_snapshot();
    value.text_paste_active = input_.text_active();
  }
  return value;
}

void KvmSession::capture_loop() {
  std::size_t retry_index = 0;
  auto retry_at = Clock::time_point::max();
  for (;;) {
    CaptureCommand command;
    std::optional<CaptureMode> requested;
    std::function<void()> task;
    {
      std::unique_lock lock(mutex_);
      wake_.wait_for(lock, kCapturePoll, [this] {
        return command_ != CaptureCommand::none || !capture_tasks_.empty();
      });
      command = command_;
      requested = requested_mode_;
      command_ = CaptureCommand::none;
      if (!capture_tasks_.empty()) {
        task = std::move(capture_tasks_.front());
        capture_tasks_.pop_front();
      }
    }
    if (task) task();
    if (command == CaptureCommand::shutdown) {
      capture_->stop();
      return;
    }
    if (command == CaptureCommand::stop || command == CaptureCommand::start) {
      capture_->stop();  // stops callback publication before the video
                         // generation changes
      video_.set_generation(capture_->snapshot().generation + 1);
      if (command == CaptureCommand::stop) {
        std::lock_guard lock(mutex_);
        video_state_ = SessionVideoState::stopped;
        retry_at = Clock::time_point::max();
        continue;
      }
      if (requested) {
        {
          std::lock_guard lock(mutex_);
          video_state_ = SessionVideoState::connecting;
        }
        capture_->start(*requested);
        const auto started = capture_->snapshot();
        spdlog::debug("Capture start issued: mode={} state={} error={}",
                      capture_mode_label(*requested),
                      capture_state_name(started.state),
                      started.error.empty() ? "none" : started.error);
        const auto generation = started.generation;
        video_.set_generation(generation);
        video_.start(generation);
        retry_index = 0;
        retry_at = Clock::time_point::max();
      }
    }

    const auto status = capture_->snapshot();
    if (status.state != CaptureState::fault) continue;
    std::optional<DeviceInfo> device;
    std::optional<CaptureMode> mode;
    {
      std::lock_guard lock(mutex_);
      device = selected_device_;
      mode = selected_mode_;
    }
    if (!device || !mode || device->weak_match) continue;
    const auto now = Clock::now();
    if (retry_at == Clock::time_point::max()) {
      retry_at = now + kReconnectBackoff[retry_index];
      retry_index = std::min(retry_index + 1, kReconnectBackoff.size() - 1);
      continue;
    }
    if (now < retry_at) continue;

    const auto devices = capture_->enumerate_devices();
    log_capture_devices(devices);
    std::size_t exact_matches = 0;
    for (const auto& candidate : devices) {
      if (!candidate.weak_match && candidate.stable_id == device->stable_id)
        ++exact_matches;
    }
    if (exact_matches != 1) {
      retry_at = now + kReconnectBackoff[retry_index];
      retry_index = std::min(retry_index + 1, kReconnectBackoff.size() - 1);
      continue;
    }
    capture_->stop();
    capture_->start(*mode);
    const auto restarted = capture_->snapshot();
    spdlog::debug("Capture start issued: mode={} state={} error={}",
                  capture_mode_label(*mode),
                  capture_state_name(restarted.state),
                  restarted.error.empty() ? "none" : restarted.error);
    const auto generation = restarted.generation;
    video_.set_generation(generation);
    retry_at = Clock::time_point::max();
  }
}

void KvmSession::shutdown(std::chrono::milliseconds serial_timeout) noexcept {
  {
    std::lock_guard lock(mutex_);
    if (shutting_down_) return;
    shutting_down_ = true;
    video_state_ = SessionVideoState::stopping;
    video_fresh_ = false;
  }
  {
    std::lock_guard lock(input_mutex_);
    request_release_locked(PasteCancelReason::shutdown);
    input_.set_video_fresh(false);
  }
  {
    std::lock_guard lock(motion_mutex_);
    motion_stop_ = true;
    pending_motion_.reset();
  }
  motion_wake_.notify_one();
  if (mouse_worker_.joinable()) mouse_worker_.join();
  {
    std::lock_guard lock(mutex_);
    command_ = CaptureCommand::shutdown;
  }
  wake_.notify_one();
  if (capture_worker_.joinable()) capture_worker_.join();
  video_.stop();

  control_->disconnect();
  const auto deadline = Clock::now() + serial_timeout;
  while (Clock::now() < deadline) {
    if (control_->snapshot().state == ControlConnectionState::disconnected)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const bool timed_out =
      control_->snapshot().state != ControlConnectionState::disconnected;
  std::lock_guard lock(mutex_);
  serial_shutdown_timed_out_ = timed_out;
  video_state_ = SessionVideoState::stopped;
}

}  // namespace kvmux
