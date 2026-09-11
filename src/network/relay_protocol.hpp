#pragma once

#include "control/control_event.hpp"
#include "control/control_sink.hpp"
#include "network/tcp_socket.hpp"
#include "video/capture_sample.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace kvmux::relay {

inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::size_t kMaxPacketBytes = kMaxCompressedSampleBytes + 128U;

enum class PacketType : std::uint8_t { hello = 1, video_mjpeg = 2, control = 3, heartbeat = 4, status = 5, release = 6, mouse_mode = 7 };

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

// Session payloads are big-endian. IDs are pairing tokens, not authentication.
struct SessionControl { std::uint64_t session{}; ControlEvent event; };
struct Heartbeat {
    std::uint64_t session{}, epoch{};
    bool gui_active{}, video_fresh{};
    std::uint64_t video_sequence{};
};
struct Status { std::uint64_t session{}; ControlSnapshot control; };
struct SessionMouseMode { std::uint64_t session{}; MouseMode mode{}; };
[[nodiscard]] std::vector<std::uint8_t> encode_session(std::uint64_t session);
[[nodiscard]] std::optional<std::uint64_t> decode_session(std::span<const std::uint8_t>);
[[nodiscard]] std::vector<std::uint8_t> encode_session_control(const SessionControl&);
[[nodiscard]] std::optional<SessionControl> decode_session_control(std::span<const std::uint8_t>);
[[nodiscard]] std::vector<std::uint8_t> encode_heartbeat(const Heartbeat&);
[[nodiscard]] std::optional<Heartbeat> decode_heartbeat(std::span<const std::uint8_t>);
[[nodiscard]] std::vector<std::uint8_t> encode_status(const Status&);
[[nodiscard]] std::optional<Status> decode_status(std::span<const std::uint8_t>);
[[nodiscard]] std::vector<std::uint8_t> encode_mouse_mode(const SessionMouseMode&);
[[nodiscard]] std::optional<SessionMouseMode> decode_mouse_mode(std::span<const std::uint8_t>);
// Whole-packet deadline, validated header and type-specific bounds before allocation.
// Any failure (including partial packet timeout) requires closing the socket.
[[nodiscard]] std::optional<Packet> receive_packet(const tcp::Socket&, std::chrono::milliseconds);
// Only a zero-byte deadline leaves a packet boundary safe for dropping/retrying.
[[nodiscard]] tcp::SendResult send_packet(const tcp::Socket&, PacketType, std::span<const std::uint8_t>,
                               std::chrono::milliseconds timeout = std::chrono::milliseconds(100));
}  // namespace kvmux::relay
