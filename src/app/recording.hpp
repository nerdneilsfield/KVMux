#pragma once

#include <algorithm>
#include <cmath>
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

// Maps a window-pixel drag onto source pixels. Near edges keep the pixel
// under the pointer; far edges keep a pixel the pointer only partly covers.
// nullopt when the mapped rectangle is empty. Recording still requires even
// coordinates, including when the crop is the whole frame.
[[nodiscard]] inline std::optional<FrameCrop> crop_from_display(
    double video_x, double video_y, double video_width, double video_height,
    double x0, double y0, double x1, double y1, unsigned source_width,
    unsigned source_height) {
  if (!(video_width > 0.0) || !(video_height > 0.0) || source_width == 0 ||
      source_height == 0) {
    return std::nullopt;
  }
  const double left = std::min(x0, x1);
  const double top = std::min(y0, y1);
  const double right = std::max(x0, x1);
  const double bottom = std::max(y0, y1);
  const double limit_x = static_cast<double>(source_width);
  const double limit_y = static_cast<double>(source_height);
  const auto edge = [](double along, double limit, bool far) {
    const double scaled = std::clamp(along, 0.0, limit);
    const double snapped = far ? std::ceil(scaled) : std::floor(scaled);
    if (snapped <= 0.0) return 0U;
    if (snapped >= limit) return static_cast<unsigned>(limit);
    return static_cast<unsigned>(snapped);
  };
  const unsigned x =
      edge((left - video_x) * limit_x / video_width, limit_x, false);
  const unsigned y =
      edge((top - video_y) * limit_y / video_height, limit_y, false);
  const unsigned right_px =
      edge((right - video_x) * limit_x / video_width, limit_x, true);
  const unsigned bottom_px =
      edge((bottom - video_y) * limit_y / video_height, limit_y, true);
  if (right_px <= x || bottom_px <= y) return std::nullopt;
  return FrameCrop{x, y, right_px - x, bottom_px - y};
}

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
