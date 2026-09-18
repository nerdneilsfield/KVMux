#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

struct AVFrame;
#include <span>
#include <vector>

namespace kvmux {

inline constexpr std::size_t kMaxCompressedSampleBytes =
    std::size_t{16} * 1024U * 1024U;
inline constexpr std::size_t kMaxRawSampleBytes =
    std::size_t{64} * 1024U * 1024U;
inline constexpr std::size_t kInputPaddingBytes = 64U;
inline constexpr std::uint32_t kMaxCaptureWidth = 1920U;
inline constexpr std::uint32_t kMaxCaptureHeight = 1200U;

enum class PixelFormat {
  yuy2,
  uyvy,
  nv12,
  yuv420p,
  yuv422p,
  yuv444p,
  bgra,
  rgba,
  mjpeg,
  unknown
};
enum class ColorRange { limited, full, unknown };
enum class ColorMatrix { bt601, bt709, unknown };

struct PlaneLayout {
  std::size_t offset{};
  std::ptrdiff_t stride{};
  std::size_t row_bytes{};
  std::size_t rows{};
};

struct RawPayload {
  PixelFormat format{PixelFormat::unknown};
  std::vector<PlaneLayout> planes;
  std::vector<std::uint8_t> bytes;
};

struct MjpegPayload {
  std::size_t payload_size{};
  std::vector<std::uint8_t> bytes;
};

struct CaptureSample {
  std::uint64_t generation{};
  std::uint64_t sequence{};
  std::chrono::steady_clock::time_point arrival{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::optional<std::int64_t> device_timestamp;
  std::int32_t device_time_base_numerator{};
  std::int32_t device_time_base_denominator{1};
  std::uint32_t sample_aspect_ratio_numerator{1};
  std::uint32_t sample_aspect_ratio_denominator{1};
  ColorRange color_range{ColorRange::unknown};
  ColorMatrix color_matrix{ColorMatrix::unknown};
  std::optional<RawPayload> raw;
  std::optional<MjpegPayload> mjpeg;
  // Owned CPU decoder output; no compressed dependencies cross this mailbox.
  std::shared_ptr<AVFrame> decoded;

  [[nodiscard]] static std::optional<CaptureSample> make_raw(
      std::uint64_t generation, std::uint64_t sequence,
      std::chrono::steady_clock::time_point arrival, std::uint32_t width,
      std::uint32_t height, PixelFormat format,
      std::span<const PlaneLayout> planes, std::span<const std::uint8_t> bytes);

  [[nodiscard]] static std::optional<CaptureSample> make_mjpeg(
      std::uint64_t generation, std::uint64_t sequence,
      std::chrono::steady_clock::time_point arrival, std::uint32_t width,
      std::uint32_t height, std::span<const std::uint8_t> bytes);
};

[[nodiscard]] bool valid_dimensions(std::uint32_t width,
                                    std::uint32_t height) noexcept;

}  // namespace kvmux
