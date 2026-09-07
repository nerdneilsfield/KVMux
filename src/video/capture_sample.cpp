#include "video/capture_sample.hpp"

#include <algorithm>
#include <limits>

namespace kvmux {
namespace {

bool plane_fits(const PlaneLayout& plane, const std::size_t payload_size) {
    if (plane.stride == std::numeric_limits<std::ptrdiff_t>::min() ||
        plane.row_bytes == 0 || plane.rows == 0) {
        return false;
    }
    const auto stride = static_cast<std::size_t>(
        plane.stride < 0 ? -plane.stride : plane.stride);
    if (stride < plane.row_bytes) {
        return false;
    }
    if (plane.rows - 1U >
        (std::numeric_limits<std::size_t>::max() - plane.row_bytes) / stride) {
        return false;
    }
    const auto extent = (plane.rows - 1U) * stride + plane.row_bytes;
    return plane.offset <= payload_size && extent <= payload_size - plane.offset;
}

}  // namespace

bool valid_dimensions(const std::uint32_t width,
                      const std::uint32_t height) noexcept {
    return width > 0 && height > 0 && width <= kMaxCaptureWidth &&
           height <= kMaxCaptureHeight;
}

std::optional<CaptureSample> CaptureSample::make_raw(
    const std::uint64_t generation, const std::uint64_t sequence,
    const std::chrono::steady_clock::time_point arrival,
    const std::uint32_t width, const std::uint32_t height,
    const PixelFormat format, const std::span<const PlaneLayout> planes,
    const std::span<const std::uint8_t> bytes) {
    if (!valid_dimensions(width, height) || format == PixelFormat::unknown ||
        planes.empty() || bytes.empty() || bytes.size() > kMaxRawSampleBytes ||
        !std::all_of(planes.begin(), planes.end(),
                     [size = bytes.size()](const auto& plane) {
                         return plane_fits(plane, size);
                     })) {
        return std::nullopt;
    }

    CaptureSample sample{};
    sample.generation = generation;
    sample.sequence = sequence;
    sample.arrival = arrival;
    sample.width = width;
    sample.height = height;
    sample.raw = RawPayload{format, std::vector<PlaneLayout>(planes.begin(), planes.end()),
                            std::vector<std::uint8_t>(bytes.begin(), bytes.end())};
    return sample;
}

std::optional<CaptureSample> CaptureSample::make_mjpeg(
    const std::uint64_t generation, const std::uint64_t sequence,
    const std::chrono::steady_clock::time_point arrival,
    const std::uint32_t width, const std::uint32_t height,
    const std::span<const std::uint8_t> bytes) {
    if (!valid_dimensions(width, height) || bytes.empty() ||
        bytes.size() > kMaxCompressedSampleBytes) {
        return std::nullopt;
    }
    if (bytes.size() > std::numeric_limits<std::size_t>::max() - kInputPaddingBytes) {
        return std::nullopt;
    }

    CaptureSample sample{};
    sample.generation = generation;
    sample.sequence = sequence;
    sample.arrival = arrival;
    sample.width = width;
    sample.height = height;
    MjpegPayload payload;
    payload.payload_size = bytes.size();
    payload.bytes.resize(bytes.size() + kInputPaddingBytes, 0);
    std::copy(bytes.begin(), bytes.end(), payload.bytes.begin());
    sample.mjpeg = std::move(payload);
    return sample;
}

}  // namespace kvmux
