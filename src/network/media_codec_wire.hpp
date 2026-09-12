#pragma once

#include "video/capture_sample.hpp"
#include "video/codec/video_codec.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace kvmux::relay {

[[nodiscard]] std::vector<std::uint8_t> encode_mjpeg(const CaptureSample& sample);
[[nodiscard]] std::optional<CaptureSample> decode_mjpeg(std::span<const std::uint8_t> bytes,
                                                         std::uint64_t generation);

// HEVC payloads contain one complete Annex B access unit, not individual NALs.
// encoded_sequence tracks the ordered reference chain; capture_sequence may skip.
// Big-endian payload: generation/u64, encoded_sequence/u64, capture_sequence/u64,
// pts_ns/i64, width/u32, height/u32, SAR numerator/u32 and denominator/u32,
// H.273 range/space/primaries/transfer (one byte each), IDR/u8, reserved/u8=0,
// Annex B byte length/u32, then bytes (at most 16 MiB). Arrival is receiver-local.
[[nodiscard]] std::vector<std::uint8_t> encode_hevc(const EncodedAccessUnit&);
[[nodiscard]] std::optional<EncodedAccessUnit> decode_hevc(std::span<const std::uint8_t>);

}  // namespace kvmux::relay
