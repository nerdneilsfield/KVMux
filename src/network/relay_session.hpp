#pragma once

#include <array>
#include <memory>

#include "network/relay_wire.hpp"
#include "network/udp_socket.hpp"

namespace kvmux::relay {
using SessionTime = std::chrono::steady_clock::time_point;
enum class SessionPhase { idle, pending, established };
struct SessionAction {
  enum class Kind {
    send_raw,
    established,
    expired,
    revoke_input,
    lease_changed,
    state_ack,
    rejected,
    paste_ready
  };
  Kind kind{};
  udp::Endpoint peer;
  std::vector<std::uint8_t> datagram;
  std::optional<wire::StateAck> ack;
  std::optional<wire::BusyReason> rejection;
  std::optional<wire::PasteStatus> paste_status;
};
// Fixed-capacity synchronous results. No internal output queue; consume each
// batch.
struct SessionActions {
  std::array<SessionAction, 8> items{};
  std::size_t size{};
  const SessionAction* begin() const { return items.data(); }
  const SessionAction* end() const { return items.data() + size; }
};
struct SessionIds {
  std::uint64_t session{}, generation{};
  std::uint32_t conversation{};
};
enum class InputGate { allowed, duplicate, rejected, recovery_required };
class ServerSession {
 public:
  explicit ServerSession(VideoCodec);
  ~ServerSession();
  ServerSession(const ServerSession&) = delete;
  ServerSession& operator=(const ServerSession&) = delete;
  // Caller supplies newly random nonzero IDs when accepting a free hello.
  // video_valid defaults false: caller must validate actual current-generation
  // presentation against bounded sender history, not trust the sequence alone.
  // `now` is processing time.  A relay may defer a raw proof until its video
  // frame is fully sent; in that case it supplies the packet receipt time so
  // challenge freshness is checked at receipt without backdating leases.
  SessionActions on_datagram(const udp::Endpoint&,
                             std::span<const std::uint8_t>, SessionTime now,
                             SessionIds = {}, bool video_valid = false,
                             std::optional<SessionTime> proof_received_at = {});
  SessionActions tick(SessionTime);
  SessionActions update_control_snapshot(const ControlSnapshot&, SessionTime);
  SessionActions cancel(const wire::Cancel&,
                        SessionTime);  // Already tuple-validated KCP input.
  // These reliable-control calls require the caller to have matched the current
  // tuple. They retain no source outside the one bounded session upload.
  SessionActions paste_begin(const wire::PasteBegin&, SessionTime);
  SessionActions paste_chunk(const wire::PasteChunk&, SessionTime);
  SessionActions paste_execute(const wire::PasteExecute&, SessionTime);
  SessionActions paste_cancel(const wire::PasteCancel&, SessionTime);
  // Call for an already decoded KCP control before tick(now), so a keepalive
  // received at its deadline renews the executing transaction.
  SessionActions paste_keepalive(const wire::PasteKeepalive&, SessionTime);
  // Transfers the validated upload to the serial owner, then reports
  // ACK-derived progress. Call before tick(now), so a terminal serial
  // observation wins at that instant.
  SessionActions paste_started(std::uint64_t job_id, SessionTime);
  // Legacy direct local path has no correlation identifier.
  SessionActions paste_started(SessionTime);
  SessionActions paste_start_failed(SessionTime);
  SessionActions update_ascii_paste(const AsciiPasteSnapshot&, SessionTime);
  [[nodiscard]] std::optional<wire::PasteStatus> paste_status() const;
  // Available after accepted execution intent until the relay server submits it
  // to the serial owner.
  [[nodiscard]] std::optional<std::span<const std::uint8_t>>
  pending_paste_bytes() const;
  [[nodiscard]] std::optional<std::pair<std::uint64_t, std::uint64_t>>
  pending_paste_owner() const;
  [[nodiscard]] bool matches(const udp::Endpoint&, const wire::Tuple&) const;
  [[nodiscard]] InputGate check_sync(const wire::Sync&, SessionTime) const;
  // Call after synchronous sink admission; rejection must not create an ACK.
  SessionActions sync_submitted(const wire::Sync&, kvmux::SubmitResult,
                                SessionTime);
  [[nodiscard]] InputGate check_edge(const wire::Edge&, SessionTime) const;
  // For recovery_required call with not_ready WITHOUT submitting to the sink;
  // this records the gap/revocation. Duplicate/rejected edges are never sent.
  SessionActions edge_submitted(const wire::Edge&, kvmux::SubmitResult,
                                SessionTime);
  [[nodiscard]] SessionPhase phase() const;
  [[nodiscard]] wire::Tuple tuple() const;
  [[nodiscard]] wire::Welcome welcome() const;
  [[nodiscard]] bool barrier_complete() const;
  [[nodiscard]] std::uint64_t canceled_through() const;
  // Provisional lease enables sink activation/heartbeat for synchronization.
  // Only barrier_complete additionally permits healthy edges. Caller must tick
  // and enforce this deadline, not replace it with a receipt-local heartbeat.
  [[nodiscard]] std::optional<SessionTime> execution_deadline() const;
  // During an executing paste this is the transaction lease; otherwise it is
  // the proof lease.
  [[nodiscard]] std::optional<SessionTime> control_deadline() const;
  [[nodiscard]] std::size_t challenge_count() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
class ClientSession {
 public:
  explicit ClientSession(udp::Endpoint server,
                         std::uint8_t supported_codecs = 7);
  ~ClientSession();
  ClientSession(const ClientSession&) = delete;
  ClientSession& operator=(const ClientSession&) = delete;
  SessionActions start(std::uint64_t new_nonce, SessionTime);
  SessionActions on_datagram(const udp::Endpoint&,
                             std::span<const std::uint8_t>, SessionTime);
  SessionActions tick(SessionTime);
  void set_intent(std::uint64_t generation, bool active, bool video_fresh,
                  std::uint64_t presented_sequence);
  SessionActions cancel(wire::CancelReason, SessionTime);
  // Caller coalesces this one outstanding cancellation into reliable KCP.
  [[nodiscard]] std::optional<wire::Cancel> pending_cancel() const;
  [[nodiscard]] SessionPhase phase() const;
  [[nodiscard]] wire::Tuple tuple() const;
  [[nodiscard]] wire::Welcome welcome() const;
  [[nodiscard]] std::uint64_t latest_challenge() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace kvmux::relay
