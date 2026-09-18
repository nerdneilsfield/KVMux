#pragma once

#include "video/video_frame.hpp"

#include <filesystem>
#include <string>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace kvmux {

enum class RecordingState { idle, starting, recording, paused, stopping, failed };
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
};

// Thread-safe local writer. Submission retains only a bounded newest owned VideoFrame.
class Recording final {
public:
    explicit Recording(std::filesystem::path output_directory = {});
    ~Recording();
    Recording(const Recording&) = delete;
    Recording& operator=(const Recording&) = delete;

    [[nodiscard]] std::filesystem::path downloads_directory() const;
    [[nodiscard]] bool snapshot(const VideoFrame& frame, std::optional<FrameCrop> crop = std::nullopt);
    [[nodiscard]] bool start(const VideoFrame& first_frame);
    [[nodiscard]] bool append(const VideoFrame& frame);
    [[nodiscard]] bool pause();
    [[nodiscard]] bool resume();
    [[nodiscard]] bool stop();
    void shutdown() noexcept;

    [[nodiscard]] RecordingStatus status() const;

private:
    struct Impl;
    [[nodiscard]] bool start_now(const VideoFrame& frame);
    [[nodiscard]] bool snapshot_now(const VideoFrame& frame, std::optional<FrameCrop> crop);
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
    std::optional<VideoFrame> start_;
    bool stop_requested_{};
    std::jthread worker_;
};

}  // namespace kvmux
