#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace kvmux {

// A compact description of one fixed-capacity latency window.
struct LatencySnapshot {
  std::size_t samples{};
  double latest_ms{};
  double mean_ms{};
  double p95_ms{};
};

struct DiagnosticsSnapshot {
  double capture_fps{};
  double decode_fps{};
  double unique_present_fps{};

  // Mouse motion events seen by the UI thread per second over the recent
  // window; 0 once the pointer has been idle for a second.
  double mouse_events_per_second{};

  LatencySnapshot sample_to_gpu_submit;
  // SDL event timestamp to the UI thread polling it (SDL's own clock).
  LatencySnapshot sdl_event_wait;
  // UI thread receipt to the input handler returning (includes waiting for
  // the router lock and the router's own submits; a handler call need not
  // submit anything, e.g. outside Captured).
  LatencySnapshot input_processing;
  // UI thread receipt (oldest merged event) to the serial thread dequeuing it.
  LatencySnapshot sdl_to_control_dequeue;
  // ControlSink submit (oldest merged event) to the serial thread dequeuing it.
  LatencySnapshot input_queue_age;
  // Ordinary input report written to ACK read. Not target response time.
  LatencySnapshot ack_rtt;
  // SDL_GL_SwapWindow only; excludes the frame-rate limiter delay.
  LatencySnapshot present_blocking;

  std::uint64_t mouse_motion_events{};
  // Motion events folded into a neighbour before reaching the router.
  std::uint64_t mouse_motion_folded{};

  std::uint64_t sample_mailbox_overwrites{};
  std::uint64_t frame_mailbox_overwrites{};
  std::uint64_t timeouts{};
  std::string capture_mode;
  std::string pixel_path;
  std::string recent_error;
};

// Thread-safe, allocation-free on the metric recording path. All timestamps
// must come from std::chrono::steady_clock; device PTS values are not accepted.
class Diagnostics {
 public:
  using Clock = std::chrono::steady_clock;
  using Duration = Clock::duration;

  Diagnostics();
  ~Diagnostics();
  Diagnostics(const Diagnostics&) = delete;
  Diagnostics& operator=(const Diagnostics&) = delete;

  void record_capture(Clock::time_point at = Clock::now()) noexcept;
  void record_decode(Clock::time_point at = Clock::now()) noexcept;
  void record_present(std::uint64_t generation, std::uint64_t capture_sequence,
                      Clock::time_point at = Clock::now()) noexcept;

  void record_mouse_motion(Clock::time_point at = Clock::now()) noexcept;
  void record_mouse_motion_folded() noexcept;
  void record_sample_to_gpu_submit(Duration duration) noexcept;
  void record_sdl_event_wait(Duration duration) noexcept;
  void record_input_processing(Duration duration) noexcept;
  void record_sdl_to_control_dequeue(Duration duration) noexcept;
  void record_input_queue_age(Duration duration) noexcept;
  void record_ack_rtt(Duration duration) noexcept;
  void record_present_blocking(Duration duration) noexcept;

  void set_mailbox_overwrites(std::uint64_t sample_mailbox,
                              std::uint64_t frame_mailbox) noexcept;
  void record_timeout() noexcept;
  void set_capture_mode(std::string_view mode);
  void set_pixel_path(std::string_view path);
  void set_recent_error(std::string_view error);
  void clear_recent_error();

  [[nodiscard]] DiagnosticsSnapshot snapshot() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

// Stderr, and when `log_file` is set the same logger also appends to a
// rotating file (10 MiB, three retained files). Both sinks use one level and
// one pattern. Debug is opt-in. Warnings flush immediately. Debug is also
// flushed every second and by shutdown_console_logging(). Returns false when
// the file cannot be opened; the process then exits 2. `serial_port` and
// `baud` are argument text, or empty when those arguments are absent. An info
// line records version, OS, and those arguments. The repository does not
// embed a commit id.
bool configure_console_logging(bool debug, std::string_view log_file = {},
                               std::string_view program = {},
                               std::string_view serial_port = {},
                               std::string_view baud = {});

// Flushes the default logger, then spdlog::shutdown(). Safe to call twice.
void shutdown_console_logging() noexcept;

// Destructor calls shutdown_console_logging(). Construct after a successful
// configure_console_logging() so every return path flushes.
class ConsoleLoggingSession {
 public:
  ConsoleLoggingSession() = default;
  ~ConsoleLoggingSession();
  ConsoleLoggingSession(const ConsoleLoggingSession&) = delete;
  ConsoleLoggingSession& operator=(const ConsoleLoggingSession&) = delete;
  ConsoleLoggingSession(ConsoleLoggingSession&&) = delete;
  ConsoleLoggingSession& operator=(ConsoleLoggingSession&&) = delete;
};

}  // namespace kvmux
