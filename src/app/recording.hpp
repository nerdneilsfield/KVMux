#pragma once

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "app/scroll_stitcher.hpp"
#include "video/video_frame.hpp"

namespace kvmux {

enum class RecordingState {
  idle,
  starting,
  recording,
  paused,
  stopping,
  failed
};
enum class ScrollCaptureState { idle, starting, capturing, finishing, failed };
struct FrameCrop {
  unsigned x{}, y{}, width{}, height{};
};

struct RecordingStatus {
  RecordingState state{RecordingState::idle};
  std::filesystem::path output_path;
  std::string error;
  std::filesystem::path last_snapshot_path;
  std::string snapshot_error;
  unsigned width{}, height{};
  ScrollCaptureState scroll_state{ScrollCaptureState::idle};
  std::filesystem::path last_scroll_path;
  std::string scroll_error;
  unsigned scroll_width{}, scroll_height{};
};

// Thread-safe local writer. Submission retains only a bounded newest owned
// VideoFrame.
class Recording final {
 public:
  explicit Recording(std::filesystem::path output_directory = {});
  ~Recording();
  Recording(const Recording&) = delete;
  Recording& operator=(const Recording&) = delete;

  [[nodiscard]] std::filesystem::path downloads_directory() const;
  [[nodiscard]] bool snapshot(const VideoFrame& frame,
                              std::optional<FrameCrop> crop = std::nullopt);
  [[nodiscard]] bool start_scroll(const VideoFrame& first_frame,
                                  FrameCrop crop);
  [[nodiscard]] bool sample_scroll(const VideoFrame& frame);
  [[nodiscard]] bool finish_scroll();
  void cancel_scroll();
  [[nodiscard]] bool start(const VideoFrame& first_frame,
                           std::optional<FrameCrop> crop = std::nullopt);
  [[nodiscard]] bool append(const VideoFrame& frame);
  [[nodiscard]] bool pause();
  [[nodiscard]] bool resume();
  [[nodiscard]] bool stop();
  void shutdown() noexcept;

  [[nodiscard]] RecordingStatus status() const;

 private:
  struct Impl;
  [[nodiscard]] bool start_now(const VideoFrame& frame,
                               std::optional<FrameCrop> crop);
  [[nodiscard]] bool snapshot_now(const VideoFrame& frame,
                                  std::optional<FrameCrop> crop);
  [[nodiscard]] bool write_frame(const VideoFrame& frame);
  void worker(std::stop_token stop_token);
  void ensure_worker();
  void fail(std::string message) noexcept;

  Impl* impl_{};
  mutable std::mutex mutex_;
  RecordingStatus status_;
  std::filesystem::path output_directory_;
  std::chrono::steady_clock::time_point paused_at_{};
  std::chrono::steady_clock::duration paused_duration_{};
  std::condition_variable_any wake_;
  std::optional<VideoFrame> latest_;
  std::optional<std::pair<VideoFrame, std::optional<FrameCrop>>> snapshot_;
  struct ScrollRequest {
    VideoFrame frame;
    FrameCrop crop;
  };
  std::optional<ScrollStitcher> scroll_stitcher_;
  FrameCrop scroll_crop_{};
  std::uint64_t scroll_sequence_{};
  std::optional<ScrollRequest> scroll_start_;
  std::optional<VideoFrame> scroll_latest_;
  bool scroll_finish_{};
  bool scroll_cancel_{};
  std::optional<std::pair<VideoFrame, std::optional<FrameCrop>>> start_;
  bool stop_requested_{};
  std::jthread worker_;
};

}  // namespace kvmux
