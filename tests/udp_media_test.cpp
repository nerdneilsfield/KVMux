#include "network/udp_media.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <random>

#include "network/media_codec_wire.hpp"
#include "network/udp_socket.hpp"

using namespace kvmux;
using namespace kvmux::relay;
using namespace std::chrono_literals;
namespace {
const MediaTime epoch{};
MediaFrame make_frame(VideoCodec codec, std::uint64_t seq,
                      std::size_t compressed, bool idr = false) {
  std::vector<std::uint8_t> data(compressed, 0x55);
  if (codec == VideoCodec::mjpeg) {
    auto sample = CaptureSample::make_mjpeg(7, seq, epoch, 16, 16, data);
    assert(sample);
    return {codec, 7, seq, false, encode_mjpeg(*sample), epoch};
  }
  assert(compressed >= 7);
  data[0] = 0;
  data[1] = 0;
  data[2] = 0;
  data[3] = 1;
  data[4] = idr ? 0x26 : 0x02;
  data[5] = 1;
  EncodedAccessUnit au;
  au.bytes = std::move(data);
  au.width = 16;
  au.height = 16;
  au.generation = 7;
  au.encoded_sequence = seq;
  au.capture_sequence = seq;
  au.idr = idr;
  return {codec, 7, seq, idr, encode_annex_b(au), epoch};
}
struct Events {
  std::vector<MediaFrame> frames;
  std::vector<MediaEvent::Kind> order;
  std::size_t resets{}, refreshes{}, losses{};
  std::uint64_t marker{};
  void add(std::vector<MediaEvent> batch) {
    std::size_t bytes = 0;
    for (auto& e : batch) {
      order.push_back(e.kind);
      if (e.kind == MediaEvent::Kind::frame) {
        bytes += e.frame->bytes.size();
        frames.push_back(std::move(*e.frame));
        marker = e.recovery_marker;
      }
      if (e.kind == MediaEvent::Kind::reset) ++resets;
      if (e.kind == MediaEvent::Kind::refresh) ++refreshes;
      if (e.kind == MediaEvent::Kind::loss) ++losses;
    }
    assert(batch.size() <= 32 && bytes <= kMediaAllocationLimit);
  }
};
Events send(MediaReceiver& receiver, const MediaFrame& f, MediaTime now = epoch,
            std::vector<std::size_t> missing = {}, bool reverse = false,
            bool duplicate = false) {
  Events result;
  const auto n = media_packet_count(f);
  for (std::size_t i = 0; i < n; ++i) {
    const auto ordinal = reverse ? n - 1 - i : i;
    if (std::find(missing.begin(), missing.end(), ordinal) != missing.end())
      continue;
    const auto packet = media_packet(f, ordinal);
    assert(packet && packet->size() + 32 <= 1200);
    result.add(receiver.input(*packet, now));
    if (duplicate) result.add(receiver.input(*packet, now));
    assert(receiver.stats().charged_bytes <= kMediaAllocationLimit &&
           receiver.stats().resident_frames <= 8);
  }
  return result;
}
void roundtrips() {
  for (auto codec : {VideoCodec::mjpeg, VideoCodec::hevc, VideoCodec::h264}) {
    const std::size_t overhead = codec == VideoCodec::mjpeg ? 24 : 58;
    for (auto size : {std::size_t(100), 1128 - overhead, std::size_t(1129),
                      std::size_t(70000), kMaxCompressedSampleBytes}) {
      const auto frame = make_frame(codec, 1, size, codec != VideoCodec::mjpeg);
      assert(frame.bytes.size() == size + overhead);
      MediaReceiver receiver(codec, 7);
      auto result = send(receiver, frame, epoch, {}, true, true);
      assert(result.frames.size() == 1 &&
             result.frames[0].bytes == frame.bytes);
      assert(result.frames[0].first_arrival == epoch &&
             receiver.stats().charged_bytes == 0);
    }
    const auto f = make_frame(codec, 1, 20000, codec != VideoCodec::mjpeg);
    for (const auto missing : {std::vector<std::size_t>{0},
                               {4},
                               {media_packet_count(f) - 2},
                               {8, 17}}) {
      MediaReceiver receiver(codec, 7);
      auto result = send(receiver, f, epoch, missing, true, true);
      assert(result.frames.size() == 1 && result.frames[0].bytes == f.bytes);
    }
    MediaReceiver failed(codec, 7);
    auto bad = send(failed, f, epoch, {0, 1});
    assert(bad.frames.empty());
    bad.add(failed.poll(epoch + 150ms));
    assert(bad.frames.empty() && bad.losses == 1);
    assert(failed.stats().charged_bytes == 0);
  }
}
void malformed_and_bounds() {
  auto f = make_frame(VideoCodec::mjpeg, 1, 20000);
  const auto p = *media_packet(f, 0);
  for (auto offset : {0U, 1U, 2U, 3U, 4U, 20U, 24U, 26U, 28U, 30U, 32U}) {
    auto bad = p;
    bad[offset] = 255;
    MediaReceiver receiver(VideoCodec::mjpeg, 7);
    receiver.input(bad, epoch);
    assert(receiver.stats().resident_frames == 0);
  }
  for (auto size : {std::size_t(0), std::size_t(39), std::size_t(1169)}) {
    auto bad = p;
    bad.resize(size);
    MediaReceiver receiver(VideoCodec::mjpeg, 7);
    receiver.input(bad, epoch);
    assert(receiver.stats().resident_frames == 0);
  }
  MediaReceiver conflict(VideoCodec::mjpeg, 7);
  conflict.input(p, epoch);
  auto changed = p;
  changed[40] ^= 1;
  Events result;
  result.add(conflict.input(changed, epoch));
  assert(result.losses == 1);
  assert(send(conflict, f).frames.empty());
  MediaReceiver aged(VideoCodec::mjpeg, 7);
  aged.input(p, epoch);
  aged.input(p, epoch + 149ms);
  aged.poll(epoch + 150ms);
  assert(aged.stats().resident_frames == 0 && aged.stats().age_losses == 1);
  MediaReceiver flood(VideoCodec::mjpeg, 7);
  for (std::uint64_t seq = 1; seq <= 20; ++seq) {
    auto candidate = make_frame(VideoCodec::mjpeg, (1ULL << 60) + seq, 20000);
    flood.input(*media_packet(candidate, 0),
                epoch + std::chrono::milliseconds(seq));
    assert(flood.stats().resident_frames <= 8 &&
           flood.stats().charged_bytes <= kMediaAllocationLimit);
  }
  assert(flood.stats().capacity_losses == 12);
  MediaReceiver bytes(VideoCodec::mjpeg, 7);
  auto huge = make_frame(VideoCodec::mjpeg, 1, kMaxCompressedSampleBytes);
  bytes.input(*media_packet(huge, 0), epoch);
  huge.sequence = 2;
  bytes.input(*media_packet(huge, 0), epoch + 1ms);
  assert(bytes.stats().resident_frames == 1 &&
         bytes.stats().capacity_losses == 1);
  // Valid header with an invalid serialized body must never publish.
  auto invalid = make_frame(VideoCodec::mjpeg, 1, 100);
  invalid.bytes[0] ^= 1;
  MediaReceiver body(VideoCodec::mjpeg, 7);
  auto body_result = send(body, invalid);
  assert(body_result.frames.empty() && body_result.losses == 1);
  // Parity for a different frame cannot repair the current frame.
  MediaReceiver cross(VideoCodec::mjpeg, 7);
  auto other = make_frame(VideoCodec::mjpeg, 2, 20000);
  auto partial = send(cross, f, epoch, {0, 8, 17, 20});
  cross.input(*media_packet(other, 8), epoch);
  assert(partial.frames.empty() && cross.stats().received_frames == 0);
}
void hevc_order() {
  MediaReceiver r(VideoCodec::hevc, 7);
  auto idr = make_frame(VideoCodec::hevc, 1, 20000, true);
  auto first = send(r, idr);
  assert(first.frames.size() == 1 && first.resets == 1);
  const auto reset = std::find(first.order.begin(), first.order.end(),
                               MediaEvent::Kind::reset);
  const auto frame = std::find(first.order.begin(), first.order.end(),
                               MediaEvent::Kind::frame);
  assert(reset < frame && first.marker == 1);
  auto p2 = make_frame(VideoCodec::hevc, 2, 20000);
  auto p3 = make_frame(VideoCodec::hevc, 3, 20000);
  assert(send(r, p3, epoch + 1ms, {}, true).frames.empty());
  r.poll(epoch + 40ms);
  assert(!r.stats().waiting_idr);
  Events gap;
  gap.add(r.poll(epoch + 41ms));
  assert(gap.resets == 1 && gap.refreshes == 0 && r.stats().waiting_idr);
  assert(send(r, p2, epoch + 42ms).frames.empty());
  assert(send(r, p3, epoch + 42ms).frames.empty());
  auto damaged_idr = make_frame(VideoCodec::hevc, 5, 20000, true);
  assert(send(r, damaged_idr, epoch + 50ms, {0, 1, 8}).frames.empty());
  Events refreshes;
  for (int ms = 42; ms <= 350; ++ms)
    refreshes.add(r.poll(epoch + std::chrono::milliseconds(ms)));
  assert(refreshes.refreshes == 3 && r.stats().waiting_idr);
  auto recovery =
      send(r, make_frame(VideoCodec::hevc, 10, 20000, true), epoch + 351ms);
  assert(recovery.frames.size() == 1 && recovery.marker > first.marker);
  auto resumed =
      send(r, make_frame(VideoCodec::hevc, 11, 20000), epoch + 352ms);
  assert(resumed.frames.size() == 1 && resumed.frames[0].sequence == 11);
  assert(send(r, p3, epoch + 353ms).frames.empty());
  Events reset_failure;
  reset_failure.add(r.recover(MediaReason::decoder_failure, epoch + 354ms));
  assert(reset_failure.resets == 1 && r.stats().waiting_idr);
  MediaReceiver generation(VideoCodec::hevc, 8);
  assert(send(generation, idr).frames.empty() &&
         generation.stats().resident_frames == 0);
  // Reordered P frames flush strictly in order when the missing prefix arrives.
  MediaReceiver ordered(VideoCodec::hevc, 7);
  send(ordered, idr);
  assert(send(ordered, p3, epoch + 1ms).frames.empty());
  auto pair = send(ordered, p2, epoch + 2ms);
  assert(pair.frames.size() == 2 && pair.frames[0].sequence == 2 &&
         pair.frames[1].sequence == 3);
  MediaReceiver bypass(VideoCodec::hevc, 7);
  send(bypass, idr);
  send(bypass, p3, epoch + 1ms);
  auto fresh =
      send(bypass, make_frame(VideoCodec::hevc, 10, 100, true), epoch + 2ms);
  assert(fresh.frames.size() == 1 && fresh.resets == 1 &&
         fresh.frames[0].sequence == 10);
  // A reordered future AU must not retire the intervening new IDR's chain.
  MediaReceiver future(VideoCodec::hevc, 7);
  const auto old_chain = send(future, idr);
  const auto p12 = make_frame(VideoCodec::hevc, 12, 100);
  const auto idr10 = make_frame(VideoCodec::hevc, 10, 100, true);
  send(future, p12, epoch + 1ms);
  const auto replacement = send(future, idr10, epoch + 2ms);
  assert(replacement.frames.size() == 1 &&
         replacement.frames[0].sequence == 10);
  assert(replacement.marker >
         old_chain.marker);  // T3 must fence old decoder output.
  assert(std::find(replacement.order.begin(), replacement.order.end(),
                   MediaEvent::Kind::reset) <
         std::find(replacement.order.begin(), replacement.order.end(),
                   MediaEvent::Kind::frame));
  const auto eleven =
      send(future, make_frame(VideoCodec::hevc, 11, 100), epoch + 3ms);
  const auto twelve = send(future, p12, epoch + 4ms);
  assert(eleven.frames.size() == 1 && eleven.frames[0].sequence == 11);
  assert(twelve.frames.size() == 1 && twelve.frames[0].sequence == 12);
  assert(send(future, idr10, epoch + 5ms)
             .frames.empty());  // Delivered watermark cannot rewind.
  // Both a required incomplete AU and explicit ingress failure break
  // references.
  MediaReceiver age(VideoCodec::hevc, 7);
  send(age, idr);
  age.input(*media_packet(p2, 0), epoch + 1ms);
  Events expired;
  expired.add(age.poll(epoch + 151ms));
  assert(expired.resets == 1 && age.stats().age_losses == 1);
  Events overflow;
  overflow.add(age.recover(MediaReason::ingress_overflow, epoch + 152ms));
  assert(overflow.resets == 1 && age.stats().waiting_idr);
  MediaReceiver capacity(VideoCodec::hevc, 7);
  send(capacity, idr);
  for (std::uint64_t seq = 2; seq <= 10; ++seq) {
    auto pending = make_frame(VideoCodec::hevc, seq, 20000);
    capacity.input(*media_packet(pending, 0), epoch + 1ms);
  }
  assert(capacity.stats().capacity_losses == 1 && capacity.stats().waiting_idr);
}
void pacing() {
  constexpr std::uint64_t rate = 120000;
  MediaPacer pacer(VideoCodec::mjpeg, 7, rate, epoch);
  MediaReceiver receiver(VideoCodec::mjpeg, 7);
  std::optional<MediaFrame> latest;
  std::uint64_t sequence = 0, replaced = 0, bytes = 0, delivered = 0;
  for (int ms = 0; ms <= 1000; ++ms) {
    const auto now = epoch + std::chrono::milliseconds(ms);
    if (ms % 5 == 0) {
      if (latest) ++replaced;
      latest = make_frame(VideoCodec::mjpeg, ++sequence, 4000);
      latest->first_arrival = now;
    }
    pacer.poll(now);
    if (latest && pacer.can_start(now)) {
      assert(pacer.submit(std::move(*latest), now).accepted);
      latest.reset();
    }
    for (int pull = 0; pull < 2; ++pull) {
      auto packet = pacer.next_datagram(now);
      if (!packet) break;
      bytes += packet->size() + 32;
      for (const auto& e : receiver.input(*packet, now))
        if (e.kind == MediaEvent::Kind::frame) ++delivered;
    }
    assert(bytes <= rate * static_cast<std::uint64_t>(ms) / 1000 + 2400);
  }
  assert(delivered > 10 && replaced > 100 && bytes == pacer.stats().sent_bytes);
  auto oversized = make_frame(VideoCodec::hevc, 1, 20000, true);
  MediaPacer hevc(VideoCodec::hevc, 7, rate, epoch);
  auto budget = hevc.submit(oversized, epoch);
  assert(!budget.accepted && budget.needs_idr &&
         budget.reason == MediaReason::frame_exceeds_rate_budget);
  auto small = make_frame(VideoCodec::hevc, 2, 100, true);
  assert(hevc.submit(small, epoch).accepted);
  // Final datagram EAGAIN: saved deadline survives releasing the active frame.
  std::optional<MediaTime> saved;
  for (int ms = 0; ms < 100; ++ms) {
    auto now = epoch + std::chrono::milliseconds(ms);
    if (hevc.active_deadline()) saved = hevc.active_deadline();
    auto packet = hevc.next_datagram(now);
    if (packet && !hevc.active_deadline()) break;
  }
  assert(saved && *saved == epoch + 100ms && !hevc.active_deadline());
  auto expired_pending = hevc.discard(MediaReason::sender_deadline);
  assert(expired_pending.needs_idr);
  auto dependent = make_frame(VideoCodec::hevc, 3, 100);
  assert(hevc.submit(dependent, epoch).needs_idr);
  auto fresh = make_frame(VideoCodec::hevc, 4, 100, true);
  assert(hevc.submit(fresh, epoch).accepted);
  // Source age shortens the active lifetime independently of packetization.
  MediaPacer stale(VideoCodec::mjpeg, 7, 1000000, epoch);
  auto old_source = make_frame(VideoCodec::mjpeg, 1, 100);
  assert(stale.submit(old_source, epoch + 240ms).accepted);
  assert(stale.active_deadline() == epoch + 250ms);
  assert(!stale.poll(epoch + 250ms).accepted);
  old_source = make_frame(VideoCodec::mjpeg, 2, 100);
  assert(stale.submit(old_source, epoch + 250ms).reason ==
         MediaReason::source_stale);
  auto deadline = hevc.next_deadline(epoch);
  assert(deadline && *deadline <= epoch + 100ms);
  assert(hevc.poll(epoch + 100ms).needs_idr);
  // Long idle never accumulates more than 2400 total-envelope bytes.
  MediaPacer burst(VideoCodec::mjpeg, 7, 1000000, epoch);
  auto f = make_frame(VideoCodec::mjpeg, 1, 10000);
  f.first_arrival = epoch + 10s;
  assert(burst.submit(f, epoch + 10s).accepted);
  std::size_t burst_bytes = 0;
  while (auto p = burst.next_datagram(epoch + 10s))
    burst_bytes += p->size() + 32;
  assert(burst_bytes == 2400);
  // Every blackout exceeds or reaches the active lifetime: no old backlog.
  for (auto blackout : {100ms, 300ms, 800ms, 2000ms})
    for (auto codec : {VideoCodec::mjpeg, VideoCodec::hevc, VideoCodec::h264}) {
      MediaPacer paused(codec, 7, 1000000, epoch);
      assert(paused
                 .submit(make_frame(codec, 1, 4000, codec != VideoCodec::mjpeg),
                         epoch)
                 .accepted);
      assert(paused.next_datagram(epoch));
      auto expiry = paused.poll(epoch + blackout);
      assert(!expiry.accepted && !paused.next_datagram(epoch + blackout));
      auto next = make_frame(codec, 10, 4000, codec != VideoCodec::mjpeg);
      next.first_arrival = epoch + blackout;
      assert(paused.submit(next, epoch + blackout).accepted);
      MediaReceiver recovered(codec, 7);
      Events result;
      for (int ms = 0; ms < 100; ++ms) {
        const auto now = epoch + blackout + std::chrono::milliseconds(ms);
        for (int i = 0; i < 2; ++i)
          if (auto packet = paused.next_datagram(now))
            result.add(recovered.input(*packet, now));
      }
      assert(result.frames.size() == 1 && result.frames[0].sequence == 10);
    }
  std::cout << "pacer: " << bytes << " charged bytes/1000ms, " << replaced
            << " pre-encode source replacements, " << delivered
            << " delivered frames\n";
}
void random_loss() {
  std::mt19937 random(0x4b564d58);
  std::uniform_int_distribution<int> loss(0, 9999);
  std::size_t fec = 0, plain = 0;
  for (std::uint64_t seq = 1; seq <= 1000; ++seq) {
    auto f = make_frame(VideoCodec::mjpeg, seq, 60000);
    MediaReceiver a(VideoCodec::mjpeg, 7), b(VideoCodec::mjpeg, 7);
    for (std::size_t i = 0; i < media_packet_count(f); ++i) {
      const auto p = *media_packet(f, i);
      if (loss(random) < 100) continue;
      for (auto& e : a.input(p, epoch))
        if (e.kind == MediaEvent::Kind::frame) ++fec;
      if (p[2] == 0)
        for (auto& e : b.input(p, epoch))
          if (e.kind == MediaEvent::Kind::frame) ++plain;
    }
  }
  assert(fec > plain + 250 && fec > 900);
  std::cout << "seeded 1% independent loss/1000 frames: XOR=" << fec
            << ", no parity=" << plain << "\n";
}
void native_udp() {
  std::string error;
  auto a = udp::Socket::bind("127.0.0.1", 0, error),
       b = udp::Socket::bind("127.0.0.1", 0, error);
  assert(a && b);
  auto destination = udp::resolve("127.0.0.1", *b->local_port(), error);
  assert(destination);
  const auto f = make_frame(VideoCodec::mjpeg, 1, 2000);
  MediaReceiver receiver(VideoCodec::mjpeg, 7);
  Events result;
  for (std::size_t i = 0; i < media_packet_count(f); ++i) {
    auto p = *media_packet(f, i);
    p.insert(p.begin(), 32, 0);
    assert(a->send_to(*destination, p) == udp::SendStatus::sent);
    auto received = b->receive(100ms);
    assert(received.status == udp::ReceiveStatus::datagram);
    result.add(
        receiver.input(std::span(received.datagram.bytes).subspan(32), epoch));
  }
  assert(result.frames.size() == 1 && result.frames[0].bytes == f.bytes);
}
}  // namespace
int main() {
  roundtrips();
  malformed_and_bounds();
  hevc_order();
  pacing();
  random_loss();
  native_udp();
  std::cout << "UDP media boundaries, ordering, burst recovery, blackouts and "
               "native roundtrip passed\n";
}
