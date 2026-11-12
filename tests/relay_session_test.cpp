#include "network/relay_session.hpp"

#include <cstdlib>
#include <iostream>
#include <source_location>

#include "input/us_ascii_text.hpp"
#include "support/crc32.hpp"
using namespace std::chrono_literals;
using namespace kvmux;
using namespace kvmux::relay;
namespace w = kvmux::relay::wire;
using Kind = SessionAction::Kind;
void check(bool ok,
           std::source_location loc = std::source_location::current()) {
  if (!ok) {
    std::cerr << "session check failed at " << loc.line() << '\n';
    std::abort();
  }
}
SessionTime at(int ms) { return SessionTime{} + std::chrono::milliseconds(ms); }
std::size_t count(const SessionActions& a, Kind k) {
  std::size_t n = 0;
  check(a.size <= 8);
  for (auto& v : a)
    if (v.kind == k) ++n;
  return n;
}
std::vector<std::uint8_t> packet(const SessionActions& a,
                                 w::EnvelopeKind kind) {
  for (auto& v : a)
    if (v.kind == Kind::send_raw &&
        w::decode_envelope(v.datagram)->kind == kind)
      return v.datagram;
  check(false);
  return {};
}
std::vector<std::uint8_t> raw(w::EnvelopeKind kind, w::Tuple tuple,
                              w::RawBody body) {
  auto b = w::encode_raw(body);
  check(b.has_value());
  auto e = w::encode_envelope({kind, tuple, *b});
  check(e.has_value());
  return *e;
}
struct Fixture {
  udp::Endpoint client, server, foreign;
  ServerSession s;
  ClientSession c;
  ControlSnapshot snapshot;
  static udp::Endpoint endpoint(std::uint16_t port) {
    std::string error;
    auto p = udp::resolve("127.0.0.1", port, error);
    check(p.has_value());
    return *p;
  }
  explicit Fixture(w::RelayRole role = w::RelayRole::both,
                   VideoCodec codec = VideoCodec::hevc, std::uint8_t codecs = 7)
      : client(endpoint(23001)),
        server(endpoint(23002)),
        foreign(endpoint(23003)),
        s(codec, role),
        c(server, codecs, role) {
    snapshot.state = ControlConnectionState::ready;
    snapshot.epoch = 7;
    snapshot.target_usb_ready = true;
    snapshot.release_confirmed = true;
    s.update_control_snapshot(snapshot, at(0));
  }
  void establish() {
    auto hello = packet(c.start(11, at(0)), w::EnvelopeKind::hello);
    auto welcome = packet(s.on_datagram(client, hello, at(0), {22, 33, 44}),
                          w::EnvelopeKind::welcome);
    auto confirm =
        packet(c.on_datagram(server, welcome, at(0)), w::EnvelopeKind::confirm);
    auto ready = s.on_datagram(client, confirm, at(0));
    check(count(ready, Kind::established) == 1);
    check(count(c.on_datagram(server, packet(ready, w::EnvelopeKind::ready),
                              at(0)),
                Kind::established) == 1);
    check(c.tuple() == s.tuple() && c.welcome().generation == 33 &&
          c.welcome().codec == VideoCodec::hevc);
    c.set_intent(1, true, true, 100);
  }
  SessionActions proof(int issued, int received, bool valid = true) {
    auto challenge = packet(s.tick(at(issued)), w::EnvelopeKind::challenge);
    auto p = packet(c.on_datagram(server, challenge, at(received)),
                    w::EnvelopeKind::proof);
    return s.on_datagram(client, p, at(received), {}, valid);
  }
  w::Sync sync(std::uint64_t rev = 1, std::uint64_t floor = 0) {
    DesiredInputState state;
    state.keys = {4, 0, 5, 0, 0, 0};
    state.buttons = 1;
    state.mode = MouseMode::relative;
    return {snapshot.epoch, 1, rev, c.latest_challenge(), floor, state};
  }
  void apply(const w::Sync& request, int now) {
    snapshot.applied = {true, request.epoch, request.intent, request.revision,
                        request.state};
    auto actions = s.update_control_snapshot(snapshot, at(now));
    check(count(actions, Kind::state_ack) == 1);
    for (const auto& a : actions)
      if (a.ack)
        check(a.ack->revision == request.revision &&
              a.ack->edge_floor == request.edge_floor &&
              w::same_state(a.ack->state, request.state));
  }
  void barrier(int now = 0) {
    auto request = sync();
    check(s.check_sync(request, at(now)) == InputGate::allowed);
    check(s.sync_submitted(request, kvmux::SubmitResult::accepted, at(now))
              .size == 0);
    apply(request, now);
    check(s.barrier_complete());
  }
};
void execute_upload_contract() {
  Fixture f;
  f.establish();
  const std::vector<std::uint8_t> text = {'a', 'b', 'c', 'd'};
  const auto crc = support::crc32_ieee(text);
  check(f.s.paste_begin({9, 4, crc}, at(0)).items[0].paste_status->state ==
        w::PasteState::uploading);
  auto conflict = f.s.paste_begin({10, 4, crc}, at(1));
  check(conflict.items[0].paste_status->transaction_id == 10 &&
        conflict.items[0].paste_status->state == w::PasteState::finished &&
        conflict.items[0].paste_status->reason ==
            w::PasteStatusReason::conflict);
  check(f.s.paste_status()->transaction_id == 9 &&
        f.s.paste_status()->state == w::PasteState::uploading);
  check(f.s.paste_chunk({9, 0, {'a', 'b', 'c', 'd'}}, at(3))
            .items[0]
            .paste_status->state == w::PasteState::uploaded);
  // An expired history proof is not an Execute authorization.
  Fixture expired;
  expired.establish();
  expired.s.paste_begin({18, 4, crc}, at(0));
  expired.s.paste_chunk({18, 0, text}, at(1));
  expired.proof(50, 301);  // challenge-window expiry: never creates a lease.
  auto rejected = expired.s.paste_execute({18}, at(302));
  check(rejected.items[0].paste_status->state != w::PasteState::preparing);
  Fixture malformed;
  malformed.establish();
  malformed.s.paste_begin({19, 4, crc}, at(0));
  auto bad_chunk = malformed.s.paste_chunk({19, 1, text}, at(1));
  check(bad_chunk.items[0].paste_status->state == w::PasteState::finished &&
        bad_chunk.items[0].paste_status->outcome == w::PasteOutcome::rejected);
  Fixture corrupt;
  corrupt.establish();
  corrupt.s.paste_begin({20, 4, crc ^ 1U}, at(0));
  corrupt.s.paste_chunk({20, 0, text}, at(1));
  corrupt.proof(50, 51);
  auto checksum = corrupt.s.paste_execute({20}, at(52));
  check(checksum.items[0].paste_status->state == w::PasteState::finished &&
        checksum.items[0].paste_status->reason ==
            w::PasteStatusReason::checksum &&
        checksum.items[0].paste_status->outcome == w::PasteOutcome::rejected);
  check(!corrupt.s.paste_status() ||
        corrupt.s.paste_status()->state == w::PasteState::finished);
  // A fresh proof for a recently sent frame remains an authorization lease
  // while the sender starts pacing a newer frame and this upload completes.
  f.proof(50, 51);
  auto preparing = f.s.paste_execute({9}, at(52));
  check(preparing.items[0].paste_status->state == w::PasteState::preparing);
  check(f.s.pending_paste_bytes()->size() == 4 &&
        f.s.pending_paste_owner()->second == 1);
  check(f.s.paste_execute({9}, at(53)).items[0].paste_status->state ==
        w::PasteState::preparing);
  check(f.s.paste_started(77, at(54)).items[0].paste_status->state ==
        w::PasteState::executing);
  auto done =
      f.s.update_ascii_paste({AsciiPasteState::completed, 2, 2, 77}, at(55));
  check(done.items[0].paste_status->state == w::PasteState::finished &&
        done.items[0].paste_status->outcome == w::PasteOutcome::completed);
  check(f.s.paste_execute({9}, at(56)).items[0].paste_status->outcome ==
        w::PasteOutcome::completed);
}
void ownership_cancel_expiry_and_job_id() {
  Fixture f;
  f.establish();
  const std::vector<std::uint8_t> text = {'a'};
  auto crc = support::crc32_ieee(text);
  f.s.paste_begin({21, 1, crc}, at(0));
  f.s.paste_chunk({21, 0, text}, at(1));
  f.proof(50, 51);
  f.s.paste_execute({21}, at(52));
  f.s.paste_started(4, at(53));
  check(f.s.update_ascii_paste({AsciiPasteState::completed, 1, 1, 5}, at(54))
            .size == 0);  // foreign job cannot finish
  auto cancel = f.s.paste_cancel({21, w::PasteCancelReason::user}, at(55));
  check(cancel.items[0].paste_status->outcome == w::PasteOutcome::canceled);
  check(f.s.paste_cancel({21, w::PasteCancelReason::user}, at(56))
            .items[0]
            .paste_status->outcome == w::PasteOutcome::canceled);
  Fixture expiry;
  expiry.establish();
  expiry.s.paste_begin({22, 1, crc}, at(0));
  check(expiry.s.tick(at(30000)).items[0].paste_status->outcome ==
        w::PasteOutcome::expired);
  Fixture owner;
  owner.establish();
  owner.s.paste_begin({23, 1, crc}, at(0));
  owner.snapshot.epoch = 8;
  owner.s.update_control_snapshot(owner.snapshot, at(1));
  check(owner.s.paste_status()->outcome == w::PasteOutcome::canceled);
}
// This models the RelayServer handoff boundary without its UDP/KCP loop or GUI.
// The worker only starts the immutable mapped request, and completion is fed
// back as the serial worker's ACK-derived snapshot.
struct PasteWorker {
  std::optional<AsciiPasteRequest> prepared;
  std::uint64_t next_job{41};

  SubmitResult prepare(AsciiPasteRequest request) {
    if (prepared || request.job.gestures.empty())
      return SubmitResult::not_ready;
    prepared = std::move(request);
    return SubmitResult::accepted;
  }
  AsciiPasteSnapshot complete() const {
    check(prepared.has_value());
    return {AsciiPasteState::completed, prepared->job.gestures.size(),
            prepared->job.gestures.size(), prepared->job_id};
  }
};

void execute_two_chunk_relay_transaction() {
  Fixture f;
  f.establish();
  std::vector<std::uint8_t> text(961, 'a');
  const auto crc = support::crc32_ieee(text);

  // The proof is current before any reliable upload. It grants the owner lease,
  // but does not itself start a serial job.
  f.proof(50, 51);
  check(f.s.paste_begin({101, static_cast<std::uint32_t>(text.size()), crc},
                        at(52))
            .items[0]
            .paste_status->state == w::PasteState::uploading);
  std::vector<std::uint8_t> first(text.begin(), text.begin() + 960);
  std::vector<std::uint8_t> second(text.begin() + 960, text.end());
  check(f.s.paste_chunk({101, 0, first}, at(53))
            .items[0]
            .paste_status->accepted_bytes == 960);
  auto uploaded = f.s.paste_chunk({101, 1, second}, at(54));
  check(uploaded.items[0].paste_status->state == w::PasteState::uploaded &&
        uploaded.items[0].paste_status->next_chunk == 2);

  auto preparing = f.s.paste_execute({101}, at(55));
  check(preparing.items[0].paste_status->state == w::PasteState::preparing);
  PasteWorker worker;
  auto bytes = f.s.pending_paste_bytes();
  auto owner = f.s.pending_paste_owner();
  check(bytes && owner && bytes->size() == text.size() &&
        std::equal(bytes->begin(), bytes->end(), text.begin()));
  auto mapped = map_us_ascii_text(std::string_view(
      reinterpret_cast<const char*>(bytes->data()), bytes->size()));
  check(mapped.normalized_characters == 961 && mapped.gestures.size() == 961);
  check(std::ranges::all_of(mapped.gestures, [](const auto& gesture) {
    return gesture.edges.size() == 2;
  }));  // 1,922 ACKed HID reports
  AsciiPasteJob job;
  for (auto& gesture : mapped.gestures)
    job.gestures.push_back(std::move(gesture.edges));
  check(worker.prepare({worker.next_job, owner->first, owner->second,
                        std::move(job)}) == SubmitResult::accepted);
  check(
      f.s.paste_started(worker.next_job, at(56)).items[0].paste_status->state ==
      w::PasteState::executing);
  auto finished = f.s.update_ascii_paste(worker.complete(), at(57));
  check(finished.items[0].paste_status->state == w::PasteState::finished &&
        finished.items[0].paste_status->outcome == w::PasteOutcome::completed &&
        finished.items[0].paste_status->completed_bytes == 961);
  // Delayed Execute and Cancel cannot alter a retained terminal result.
  check(f.s.paste_execute({101}, at(58)).items[0].paste_status->outcome ==
        w::PasteOutcome::completed);
  check(f.s.paste_cancel({101, w::PasteCancelReason::user}, at(59))
            .items[0]
            .paste_status->outcome == w::PasteOutcome::completed);

  // A newer proof establishes a new owner. An old cancellation must not cancel
  // it.
  f.c.set_intent(2, true, true, 101);
  f.proof(100, 101);
  check(f.s.cancel({1, w::CancelReason::release}, at(102)).size == 1);
  check(f.s.paste_begin({102, 1,
                         support::crc32_ieee(
                             std::span<const std::uint8_t>(text.data(), 1))},
                        at(103))
            .items[0]
            .paste_status->state == w::PasteState::uploading);
  f.s.paste_chunk({102, 0, {'a'}}, at(104));
  check(f.s.paste_execute({102}, at(105)).items[0].paste_status->state ==
        w::PasteState::preparing);
  auto released = f.snapshot;
  released.release_confirmed = false;
  auto canceled = f.s.update_control_snapshot(released, at(106));
  check(canceled.items[0].paste_status->outcome == w::PasteOutcome::canceled);
  check(f.s.paste_execute({102}, at(107)).items[0].paste_status->outcome ==
        w::PasteOutcome::canceled);
}

void prepare_one_byte_paste(Fixture& f, std::uint64_t id) {
  const std::vector<std::uint8_t> text = {'a'};
  const auto crc = support::crc32_ieee(text);
  f.proof(50, 51);
  check(f.s.paste_begin({id, 1, crc}, at(52)).items[0].paste_status->state ==
        w::PasteState::uploading);
  check(f.s.paste_chunk({id, 0, text}, at(53)).items[0].paste_status->state ==
        w::PasteState::uploaded);
  check(f.s.paste_execute({id}, at(60)).items[0].paste_status->state ==
        w::PasteState::preparing);
}
w::BusyReason busy_reason(const SessionActions& actions) {
  auto bytes = packet(actions, w::EnvelopeKind::busy);
  return std::get<w::Busy>(*w::decode_raw(w::EnvelopeKind::busy,
                                          w::decode_envelope(bytes)->body))
      .reason;
}
void role_checked_before_busy_and_codec() {
  const auto client = Fixture::endpoint(23201),
             server = Fixture::endpoint(23202);
  ServerSession video{VideoCodec::hevc, w::RelayRole::video};
  ClientSession control_client{server, 1, w::RelayRole::control};
  auto mismatched = video.on_datagram(
      client, packet(control_client.start(14, at(0)), w::EnvelopeKind::hello),
      at(0), {25, 36, 47});
  check(busy_reason(mismatched) == w::BusyReason::role_unsupported);
  check(video.phase() == SessionPhase::idle);
  ClientSession video_mjpeg{server, 1, w::RelayRole::video};
  auto codec = video.on_datagram(
      client, packet(video_mjpeg.start(15, at(1)), w::EnvelopeKind::hello),
      at(1), {26, 37, 48});
  check(busy_reason(codec) == w::BusyReason::no_common_codec);
  check(video.phase() == SessionPhase::idle);

  ServerSession both{VideoCodec::hevc};
  ClientSession video_client{server, 7, w::RelayRole::video};
  auto rejected = both.on_datagram(
      client, packet(video_client.start(16, at(0)), w::EnvelopeKind::hello),
      at(0), {27, 38, 49});
  check(busy_reason(rejected) == w::BusyReason::role_unsupported);
  check(both.phase() == SessionPhase::idle);

  ServerSession control{VideoCodec::hevc, w::RelayRole::control};
  ClientSession mjpeg_only{server, 1, w::RelayRole::control};
  auto welcome = packet(
      control.on_datagram(
          client, packet(mjpeg_only.start(17, at(0)), w::EnvelopeKind::hello),
          at(0), {28, 39, 50}),
      w::EnvelopeKind::welcome);
  check(std::get<w::Welcome>(*w::decode_raw(w::EnvelopeKind::welcome,
                                            w::decode_envelope(welcome)->body))
            .codec == VideoCodec::hevc);
  check(count(mjpeg_only.on_datagram(server, welcome, at(0)), Kind::send_raw) ==
        1);
  check(control.phase() == SessionPhase::pending);

  Fixture occupied;
  occupied.establish();
  ClientSession other{occupied.server, 7, w::RelayRole::video};
  auto busy = occupied.s.on_datagram(
      occupied.foreign, packet(other.start(18, at(20)), w::EnvelopeKind::hello),
      at(20));
  check(busy_reason(busy) == w::BusyReason::role_unsupported);
  check(occupied.s.phase() == SessionPhase::established);
}
void control_proof_lease_revokes_when_freshness_stops() {
  Fixture f(w::RelayRole::control);
  f.establish();
  f.proof(50, 51);
  check(count(f.s.tick(at(299)), Kind::revoke_input) == 0);
  check(f.s.execution_deadline().has_value());
  check(count(f.s.tick(at(300)), Kind::revoke_input) == 1);
  check(!f.s.execution_deadline());
  check(f.s.phase() == SessionPhase::established);
}
void control_stale_proof_revokes_preparing_paste() {
  Fixture f(w::RelayRole::control);
  f.establish();
  prepare_one_byte_paste(f, 31);
  // Absence is not a stale picture. The paste keepalive lease outlives the
  // 250 ms proof lease.
  check(count(f.s.tick(at(260)), Kind::revoke_input) == 0);
  check(f.s.paste_status()->state == w::PasteState::preparing);
  check(count(f.s.tick(at(400)), Kind::revoke_input) == 0);
  check(f.s.paste_status()->state == w::PasteState::preparing);
  f.c.set_intent(1, true, false, 100);
  auto stale = f.proof(450, 451);
  check(f.s.paste_status()->state == w::PasteState::finished);
  check(f.s.paste_status()->outcome == w::PasteOutcome::canceled);
  check(count(stale, Kind::revoke_input) == 1);
}
void both_stale_proof_keeps_paste_exemption() {
  Fixture f;
  f.establish();
  prepare_one_byte_paste(f, 32);
  check(count(f.s.tick(at(400)), Kind::revoke_input) == 0);
  f.c.set_intent(1, true, false, 100);
  auto stale = f.proof(450, 451);
  check(f.s.paste_status()->state == w::PasteState::preparing);
  check(count(stale, Kind::revoke_input) == 0);
}
void video_role_refuses_paste_without_allocation() {
  Fixture f(w::RelayRole::video);
  f.establish();
  const std::vector<std::uint8_t> text = {'a'};
  const auto crc = support::crc32_ieee(text);
  check(f.s.paste_begin({41, 1, crc}, at(1)).size == 0);
  check(f.s.paste_chunk({41, 0, text}, at(2)).size == 0);
  check(f.s.paste_execute({41}, at(3)).size == 0);
  check(f.s.paste_cancel({41, w::PasteCancelReason::user}, at(4)).size == 0);
  check(f.s.paste_keepalive({41}, at(5)).size == 0);
  check(!f.s.paste_status());
  check(!f.s.pending_paste_bytes());
}
void h264_negotiation_and_no_common_codec() {
  const auto client = Fixture::endpoint(23101),
             server = Fixture::endpoint(23102);
  ServerSession h264{VideoCodec::h264};
  ClientSession supports_h264{server, 4};
  auto hello = packet(supports_h264.start(12, at(0)), w::EnvelopeKind::hello);
  auto welcome = packet(h264.on_datagram(client, hello, at(0), {23, 34, 45}),
                        w::EnvelopeKind::welcome);
  check(std::get<w::Welcome>(*w::decode_raw(w::EnvelopeKind::welcome,
                                            w::decode_envelope(welcome)->body))
            .codec == VideoCodec::h264);
  ServerSession hevc{VideoCodec::hevc};
  ClientSession h264_only{server, 4};
  auto rejected = hevc.on_datagram(
      client, packet(h264_only.start(13, at(0)), w::EnvelopeKind::hello), at(0),
      {24, 35, 46});
  auto busy = packet(rejected, w::EnvelopeKind::busy);
  check(std::get<w::Busy>(*w::decode_raw(w::EnvelopeKind::busy,
                                         w::decode_envelope(busy)->body))
            .reason == w::BusyReason::no_common_codec);
}
int main() {
  execute_upload_contract();
  ownership_cancel_expiry_and_job_id();
  execute_two_chunk_relay_transaction();
  h264_negotiation_and_no_common_codec();
  role_checked_before_busy_and_codec();
  control_proof_lease_revokes_when_freshness_stops();
  control_stale_proof_revokes_preparing_paste();
  both_stale_proof_keeps_paste_exemption();
  video_role_refuses_paste_without_allocation();
}
