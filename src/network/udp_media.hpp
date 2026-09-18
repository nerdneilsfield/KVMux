#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "video/codec/video_codec.hpp"

namespace kvmux::relay {
using MediaTime = std::chrono::steady_clock::time_point;
inline constexpr std::size_t kMediaPayloadBytes = 1128;
inline constexpr std::size_t kMediaHeaderBytes = 40;
inline constexpr std::size_t kMediaAllocationLimit = 32U * 1024U * 1024U;
struct MediaFrame {
  VideoCodec codec{VideoCodec::mjpeg};
  std::uint64_t generation{}, sequence{};
  bool idr{};
  std::vector<std::uint8_t> bytes;  // Serialized codec body, never TCP framing.
  MediaTime
      first_arrival{};  // Sender: local source arrival. Receiver: first packet.
};
enum class MediaReason {
  none,
  malformed,
  conflict,
  invalid_body,
  age,
  capacity,
  gap,
  ingress_overflow,
  decoder_failure,
  sender_abort,
  refresh_point,
  sender_deadline,
  source_stale,
  frame_exceeds_rate_budget,
  skipped_access_unit
};
[[nodiscard]] const char* media_reason_name(MediaReason) noexcept;
struct MediaStats {
  std::uint64_t received_frames{}, recovered_fragments{}, recovered_frames{},
      lost_frames{};
  std::uint64_t age_losses{}, capacity_losses{}, gap_losses{}, unrecoverable{};
  std::uint64_t last_completed{}, recovery_marker{};
  std::size_t charged_bytes{}, resident_frames{};
  bool waiting_idr{};
};
struct MediaEvent {
  enum class Kind { frame, reset, refresh, feedback, loss };
  Kind kind{};
  MediaReason reason{MediaReason::none};
  std::uint64_t recovery_marker{};
  std::optional<MediaFrame> frame;
  MediaStats stats{};
};
// Single owner. Each call returns a bounded batch; caller consumes it
// synchronously. 32 MiB is resident reassembly charge (body, parity, bitmaps,
// 512 bytes/entry), not process RSS. Codec validation temporarily owns at most
// one extra 16 MiB body + 58 bytes/padding. Returned bodies are moved, total
// <=32 MiB/call; at most 32 event records. Do not accumulate batches in another
// queue. Construct a new receiver for a new negotiated generation. Reset
// markers fence decoder output; caller restores first_arrival after decode_*
// validation/copy.
class MediaReceiver {
 public:
  MediaReceiver(VideoCodec, std::uint64_t generation);
  ~MediaReceiver();
  MediaReceiver(const MediaReceiver&) = delete;
  MediaReceiver& operator=(const MediaReceiver&) = delete;
  std::vector<MediaEvent> input(std::span<const std::uint8_t>, MediaTime);
  std::vector<MediaEvent> poll(MediaTime);
  std::vector<MediaEvent> recover(MediaReason, MediaTime);
  [[nodiscard]] MediaStats stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
// Stateless indexed packet generation for tests and the incremental pacer.
// Packet order is 8 data, XOR parity, then the next group. Invalid frames/index
// yield nullopt. No full-frame packet vector is retained.
[[nodiscard]] std::size_t media_packet_count(const MediaFrame&);
[[nodiscard]] std::size_t media_wire_bytes(const MediaFrame&);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> media_packet(
    const MediaFrame&, std::size_t ordinal);

struct MediaAdmission {
  bool accepted{};
  MediaReason reason{MediaReason::none};
  bool needs_idr{};
};
struct MediaPacerStats {
  std::uint64_t sent_bytes{}, sender_deadlines{}, rejected_frames{},
      skipped_access_units{};
  bool waiting_idr{};
};
// Rate counts outer 32-byte envelope + body, NOT UDP/IP headers. Latest raw
// source stays in the caller's existing mailbox. Caller services control before
// pulling at most two packets and checking control again (2400-byte burst cap).
class MediaPacer {
 public:
  MediaPacer(VideoCodec, std::uint64_t generation,
             std::uint64_t bytes_per_second, MediaTime);
  ~MediaPacer();
  MediaPacer(const MediaPacer&) = delete;
  MediaPacer& operator=(const MediaPacer&) = delete;
  [[nodiscard]] bool can_start(MediaTime) const;
  MediaAdmission submit(MediaFrame, MediaTime);
  MediaAdmission poll(MediaTime);  // Expire even during socket blackout.
  MediaAdmission discard(MediaReason = MediaReason::sender_abort);
  // Capture active_deadline BEFORE each pull and keep it with the datagram:
  // final pull releases the active frame. On EAGAIN caller retains ONLY that
  // datagram until its saved deadline; no submit/pull until sent or discarded.
  // Expiring an unsent final packet requires discard(sender_deadline), too.
  std::optional<std::vector<std::uint8_t>> next_datagram(MediaTime);
  [[nodiscard]] std::optional<MediaTime> next_deadline(MediaTime) const;
  [[nodiscard]] std::optional<MediaTime> active_deadline() const;
  [[nodiscard]] MediaPacerStats stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace kvmux::relay
