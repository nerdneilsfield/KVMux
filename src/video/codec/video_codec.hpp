#pragma once

#include "video/video_frame.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace kvmux {

enum class VideoCodec { mjpeg = 0, hevc = 1, h264 = 2 };
enum class EncodingPriority { quality, size };
enum class CodecBackend { automatic, jetson_gstreamer, videotoolbox, ffmpeg_software };
enum class CodecStatus { ok, again, invalid_input, unsupported, failed, end_of_stream };

struct EncodingProfile {
    std::uint32_t bitrate;
    const char* ffmpeg_preset;
    unsigned jetson_preset_level;
};

[[nodiscard]] constexpr EncodingProfile encoding_profile(
    EncodingPriority priority, std::uint32_t requested_bitrate) noexcept {
    return priority == EncodingPriority::quality
        ? EncodingProfile{requested_bitrate, "medium", 3}
        : EncodingProfile{std::max<std::uint32_t>(250'000, requested_bitrate / 2), "slow", 4};
}

struct CodecResult {
    CodecStatus status{CodecStatus::ok};
    std::string message;
    [[nodiscard]] bool ok() const noexcept { return status == CodecStatus::ok; }
};

struct CodecDiagnostic {
    CodecBackend backend{CodecBackend::automatic};
    bool hardware_active{};
    bool hardware_verified{};
    std::string detail;
};

struct CodecConfig {
    VideoCodec codec{VideoCodec::hevc};
    EncodingPriority priority{EncodingPriority::quality};
    std::uint32_t width{}, height{};
    std::uint32_t fps_numerator{60}, fps_denominator{1};
    std::uint32_t bitrate{40'000'000}, keyframe_interval{60};
    std::uint64_t generation{};
};

struct EncoderInput {
    // Owned CPU frame. Do not mutate while submit executes. Backends copy or retain
    // an AVBufferRef before returning; the caller may release its frame afterwards.
    AvFramePtr frame;
    std::int64_t pts_ns{};
    std::uint64_t capture_sequence{}, generation{};
    std::chrono::steady_clock::time_point arrival{};
};

struct EncodedAccessUnit {
    VideoCodec codec{VideoCodec::hevc};
    // Exactly one complete Annex B AU. IDRs include SPS/PPS (and VPS for HEVC).
    std::vector<std::uint8_t> bytes;
    std::uint32_t width{}, height{};
    std::int64_t pts_ns{};
    std::uint64_t capture_sequence{}, generation{};
    std::chrono::steady_clock::time_point arrival{};
    // Consecutive encoder outputs; configure starts at 1, reset preserves counter.
    std::uint64_t encoded_sequence{};
    bool idr{};
    AVColorRange color_range{AVCOL_RANGE_UNSPECIFIED};
    AVColorSpace color_space{AVCOL_SPC_UNSPECIFIED};
    AVColorPrimaries color_primaries{AVCOL_PRI_UNSPECIFIED};
    AVColorTransferCharacteristic color_transfer{AVCOL_TRC_UNSPECIFIED};
    AVRational sample_aspect_ratio{1, 1};
};

// Single owner thread. submit/poll are bounded and never wait for codec progress.
// again: input was NOT accepted, or no output is ready. All other non-ok results
// require caller recovery; accepted compressed AUs must never be silently dropped.
// configure/reset/shutdown are lifecycle operations and may wait. reset discards
// pending work, retains configuration, and starts a fresh reference chain.
class VideoEncoder {
public:
    virtual ~VideoEncoder() = default;
    virtual CodecResult configure(const CodecConfig&) = 0;
    virtual CodecResult submit(const EncoderInput&) = 0;
    virtual CodecResult poll(EncodedAccessUnit&) = 0;
    virtual CodecResult request_keyframe() = 0;
    // Signal EOS; poll until end_of_stream. No submit until reset/configure.
    virtual CodecResult finish() = 0;
    virtual CodecResult reset() = 0;
    virtual void shutdown() noexcept = 0;
    [[nodiscard]] virtual CodecBackend backend() const noexcept = 0;
    [[nodiscard]] virtual CodecDiagnostic diagnostic() const = 0;
};

class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;
    virtual CodecResult configure(const CodecConfig&) = 0;
    virtual CodecResult submit(const EncodedAccessUnit&) = 0;
    // Output owns its AVFrame. Hardware backends explicitly read back CPU pixels.
    virtual CodecResult poll(VideoFrame&) = 0;
    // Signal EOS; poll until end_of_stream. No submit until reset/configure.
    virtual CodecResult finish() = 0;
    virtual CodecResult reset() = 0;
    virtual void shutdown() noexcept = 0;
    [[nodiscard]] virtual CodecBackend backend() const noexcept = 0;
    [[nodiscard]] virtual CodecDiagnostic diagnostic() const = 0;
};

// Explicit unavailable backends fail. Automatic tries hardware, then software
// during initialization and reports the fallback reason in diagnostics.
[[nodiscard]] std::unique_ptr<VideoEncoder> create_video_encoder(
    CodecBackend backend, std::string& error);
[[nodiscard]] std::unique_ptr<VideoDecoder> create_video_decoder(
    CodecBackend backend, std::string& error);

}  // namespace kvmux
