#pragma once

#include "control/control_event.hpp"
#include "video/capture_sample.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace kvmux::relay {

inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::size_t kMaxPacketBytes = kMaxCompressedSampleBytes + 128U;

enum class PacketType : std::uint8_t { hello = 1, video_mjpeg = 2, control = 3, heartbeat = 4 };

struct Packet {
    PacketType type{};
    std::vector<std::uint8_t> payload;
};

// All packets use a fixed big-endian header: "KVMX", version, type, reserved,
// payload length. The caller provides one complete packet boundary from TCP.
[[nodiscard]] std::vector<std::uint8_t> encode_packet(PacketType type,
                                                        std::span<const std::uint8_t> payload);
[[nodiscard]] std::optional<Packet> decode_packet(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::vector<std::uint8_t> encode_control(const ControlEvent& event);
[[nodiscard]] std::optional<ControlEvent> decode_control(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::vector<std::uint8_t> encode_mjpeg(const CaptureSample& sample);
[[nodiscard]] std::optional<CaptureSample> decode_mjpeg(std::span<const std::uint8_t> bytes,
                                                         std::uint64_t generation);

}  // namespace kvmux::relay
