#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "control/serial_worker.hpp"
#include "input/input_router.hpp"
#include "video/video_pipeline.hpp"

namespace kvmux {

enum class SessionVideoState {
  stopped,
  connecting,
  streaming,
  stopping,
  stale,
  permission_denied,
  fault,
};

struct KvmSessionSnapshot {
  SessionVideoState video_state{SessionVideoState::stopped};
  CaptureSnapshot capture;
  VideoPipelineSnapshot video;
  ControlSnapshot control;
  InputState input_state{InputState::preview};
  InputPointerSnapshot pointer;
  bool text_paste_active{};
  bool video_fresh{};
  bool shutting_down{};
  bool serial_shutdown_timed_out{};
  std::string session_error;
};

// Main-thread coordinator. Its public event and tick functions are intended to
// be called by the UI thread; capture lifecycle work is performed by its
// worker. Pointer motion is handed to a dedicated mouse thread through a
// one-slot mailbox; every other input, release and state change stays on the
// caller's thread and first applies the pending motion, so ordering is kept.
// All InputRouter access is serialized by input_mutex_.
class KvmSession {
 public:
  using Clock = std::chrono::steady_clock;

  // `diagnostics` (optional) receives serial-thread input timings.
  explicit KvmSession(const std::shared_ptr<Diagnostics>& diagnostics = {});
  KvmSession(std::unique_ptr<CaptureSource> capture,
             std::unique_ptr<ControlSink> control,
             std::shared_ptr<Diagnostics> diagnostics = {});
  ~KvmSession();
  KvmSession(const KvmSession&) = delete;
  KvmSession& operator=(const KvmSession&) = delete;

  // Device and mode changes are allowed only from Preview. Weak identities are
  // never accepted for automatic recovery.
  [[nodiscard]] std::future<std::vector<DeviceInfo>>
  enumerate_capture_devices();
  [[nodiscard]] std::future<std::vector<CaptureMode>> enumerate_capture_modes(
      std::string stable_id);
  [[nodiscard]] bool select_capture(const DeviceInfo& device,
                                    const CaptureMode& mode);
  [[nodiscard]] bool stop_capture();
  [[nodiscard]] bool connect_control(std::string port, int baud_rate,
                                     std::uint8_t address = 0);
  [[nodiscard]] bool disconnect_control();
  [[nodiscard]] bool set_mouse_mode(MouseMode mode);
  void set_host_key(std::uint16_t usage) noexcept;
  void set_relative_gain(double gain) noexcept;
  // While enabled, an idle preview session wiggles the target pointer every
  // kKeepAliveInterval so an unattended target does not sleep.
  void set_keep_alive(bool enabled) noexcept;
  [[nodiscard]] bool send_special(SpecialKeys keys);
  [[nodiscard]] TextMappingResult start_text_paste(std::string_view text);
  void cancel_text_paste() noexcept;
  [[nodiscard]] TextMappingResult text_paste_snapshot() const;
  [[nodiscard]] TextPasteSnapshot text_paste_progress() const noexcept;

  void set_video_rect(Rect rect) noexcept;
  void activate_control() noexcept;
  void handle_input(const InputEvent& event);
  void tick(Clock::time_point now = Clock::now());
  void update_ui_heartbeat() noexcept;
  void focus_lost() noexcept;
  void minimized() noexcept;
  void suspended() noexcept;
  void release_control() noexcept;

  [[nodiscard]] std::optional<VideoFrame> take_latest_frame();
  void video_presented(std::uint64_t generation,
                       std::uint64_t sequence) noexcept;
  [[nodiscard]] KvmSessionSnapshot snapshot() const;
  // UI-thread reads that avoid copying the full snapshot per input event.
  [[nodiscard]] InputState input_state() const noexcept;
  [[nodiscard]] bool text_paste_active() const noexcept;

  // Idempotent. Serial release/close is observed for at most serial_timeout.
  void shutdown(std::chrono::milliseconds serial_timeout =
                    std::chrono::milliseconds(500)) noexcept;

 private:
  enum class CaptureCommand { none, start, stop, shutdown };

  void capture_loop();
  void mouse_loop();
  void queue_motion(const InputEvent& event);
  // Caller holds input_mutex_.
  void drain_motion_locked();
  void discard_motion() noexcept;
  void apply_input_locked(const InputEvent& event);
  void request_release_locked() noexcept;
  void request_release() noexcept;
  void note_session_error(std::string error);
  void note_keep_alive(Clock::time_point now) noexcept;
  [[nodiscard]] bool preview_only() const noexcept;

  std::unique_ptr<CaptureSource> capture_;
  std::unique_ptr<ControlSink> control_;
  VideoPipeline video_;
  std::shared_ptr<Diagnostics> diagnostics_;
  mutable std::mutex
      input_mutex_;  // Guards input_; taken before motion_mutex_.
  InputRouter input_;
  std::mutex motion_mutex_;  // Guards only the one-slot motion mailbox.
  std::condition_variable motion_wake_;
  // One slot. queued_at is the enqueue time of the oldest folded motion and
  // drives the 100 ms staleness rule, independent of InputEvent::received_at.
  struct PendingMotion {
    InputEvent event;
    Clock::time_point queued_at;
  };
  std::optional<PendingMotion> pending_motion_;
  bool motion_stop_{};

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  CaptureCommand command_{CaptureCommand::none};
  std::deque<std::function<void()>> capture_tasks_;
  std::optional<CaptureMode> requested_mode_;
  std::optional<DeviceInfo> selected_device_;
  std::optional<CaptureMode> selected_mode_;
  SessionVideoState video_state_{SessionVideoState::stopped};
  std::string session_error_;
  bool shutting_down_{};
  bool serial_shutdown_timed_out_{};
  std::thread capture_worker_;

  std::uint64_t observed_generation_{};
  std::uint64_t observed_samples_{};
  std::optional<CaptureMode> observed_actual_mode_;
  CaptureState observed_capture_state_{CaptureState::stopped};
  bool received_current_generation_{};
  Clock::time_point last_sample_arrival_{};
  bool video_fresh_{};

  bool keep_alive_enabled_{};
  Clock::time_point last_control_activity_{};

  std::thread mouse_worker_;  // Last member: starts after all state exists.
};

}  // namespace kvmux
