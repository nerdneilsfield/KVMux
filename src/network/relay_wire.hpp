#pragma once

#include "control/control_sink.hpp"
#include "network/udp_media.hpp"
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace kvmux::relay::wire {
inline constexpr std::size_t envelope_bytes = 32, max_datagram_bytes = 1200;
enum class EnvelopeKind { hello, welcome, confirm, ready, busy, kcp, media, challenge, proof, cancel, cancel_ack };
struct Tuple {
    std::uint64_t session{}, nonce{};
    std::uint32_t conversation{};
    bool operator==(const Tuple&) const = default;
};
struct Envelope { EnvelopeKind kind{}; Tuple tuple; std::span<const std::uint8_t> body; };
struct Hello { std::uint8_t codecs{3}; };
struct Welcome { VideoCodec codec{VideoCodec::mjpeg}; std::uint64_t generation{}; bool operator==(const Welcome&) const = default; };
enum class BusyReason { busy, no_common_codec };
struct Busy { BusyReason reason{BusyReason::busy}; };
struct Challenge { std::uint64_t id{}; };
struct Proof { std::uint64_t challenge{}, intent{}; bool active{}, video_fresh{}; std::uint64_t presented_sequence{}; };
enum class CancelReason { focus, host, release, disconnect };
struct Cancel { std::uint64_t intent{}; CancelReason reason{CancelReason::release}; };
using RawBody = std::variant<Hello, Welcome, Busy, Challenge, Proof, Cancel>;
struct Status { std::uint64_t epoch{}; ControlConnectionState connection{}; bool usb_ready{}, release_confirmed{}; std::uint64_t canceled_through{}; bool ordinary_input_pending{}; std::uint64_t completed_ordinary_sequence{}; };
struct Sync { std::uint64_t epoch{}, intent{}, revision{}, challenge{}, edge_floor{}; DesiredInputState state; };
struct StateAck { std::uint64_t epoch{}, intent{}, revision{}, edge_floor{}; DesiredInputState state; };
struct Edge { std::uint64_t epoch{}, intent{}, sequence{}, source_sequence{}, challenge{}; ControlPayload payload; };
struct RefreshRequest { std::uint64_t generation{}; MediaReason reason{MediaReason::none}; };
struct MediaFeedback { std::uint64_t generation{}; MediaStats stats; };
struct PasteBegin { std::uint64_t transaction_id{}; std::uint32_t normalized_bytes{}, crc32{}; };
struct PasteChunk { std::uint64_t transaction_id{}; std::uint32_t chunk_index{}; std::vector<std::uint8_t> payload; };
struct PasteExecute { std::uint64_t transaction_id{}; };
// Renews an executing transaction lease. It asserts the local GUI/control loop is alive; it is not a video proof.
struct PasteKeepalive { std::uint64_t transaction_id{}; };
enum class PasteCancelReason { user=1, host=2, focus=3, release=4, disconnect=5 };
struct PasteCancel { std::uint64_t transaction_id{}; PasteCancelReason reason{PasteCancelReason::user}; };
enum class PasteState { uploading=1, uploaded=2, preparing=3, executing=4, finished=5 };
enum class PasteOutcome { none=0, completed=1, canceled=2, rejected=3, expired=4 };
enum class PasteStatusReason { none=0, busy=1, invalid=2, conflict=3, checksum=4, deadline=5, session=6, proof=7, serial=8, canceled=9 };
struct PasteStatus { std::uint64_t transaction_id{}; PasteState state{PasteState::uploading}; std::uint32_t next_chunk{}, accepted_bytes{}, completed_bytes{}; PasteOutcome outcome{PasteOutcome::none}; PasteStatusReason reason{PasteStatusReason::none}; };
using Control = std::variant<Status, Sync, StateAck, Edge, Cancel, RefreshRequest, MediaFeedback,
    PasteBegin, PasteChunk, PasteExecute, PasteCancel, PasteStatus, PasteKeepalive>;
enum class Direction { client_to_server, server_to_client };
[[nodiscard]] bool valid_state(const DesiredInputState&);
[[nodiscard]] bool same_state(const DesiredInputState&, const DesiredInputState&);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_raw(const RawBody&);
[[nodiscard]] std::optional<RawBody> decode_raw(EnvelopeKind, std::span<const std::uint8_t>);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_envelope(const Envelope&);
// Body borrows the input; consume synchronously before releasing the datagram.
[[nodiscard]] std::optional<Envelope> decode_envelope(std::span<const std::uint8_t>);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_control(const Control&, Direction);
[[nodiscard]] std::optional<Control> decode_control(std::span<const std::uint8_t>, Direction);
} // namespace kvmux::relay::wire
