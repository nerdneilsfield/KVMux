#include "network/udp_media.hpp"
#include "network/media_codec_wire.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>

namespace kvmux::relay {
const char* media_reason_name(MediaReason reason) noexcept {
    switch (reason) {
    case MediaReason::none: return "none";
    case MediaReason::malformed: return "malformed";
    case MediaReason::conflict: return "conflict";
    case MediaReason::invalid_body: return "invalid_body";
    case MediaReason::age: return "age";
    case MediaReason::capacity: return "capacity";
    case MediaReason::gap: return "gap";
    case MediaReason::ingress_overflow: return "ingress_overflow";
    case MediaReason::decoder_failure: return "decoder_failure";
    case MediaReason::sender_abort: return "sender_abort";
    case MediaReason::refresh_point: return "refresh_point";
    case MediaReason::sender_deadline: return "sender_deadline";
    case MediaReason::source_stale: return "source_stale";
    case MediaReason::frame_exceeds_rate_budget: return "frame_exceeds_rate_budget";
    case MediaReason::skipped_access_unit: return "skipped_access_unit";
    }
    return "unknown";
}
namespace {
using namespace std::chrono_literals;
constexpr std::size_t payload = kMediaPayloadBytes;
constexpr std::size_t metadata_charge = 512; // Includes map node and vector owners.
std::size_t fragments(std::size_t n) { return (n + payload - 1) / payload; }
std::size_t groups(std::size_t n) { return (n + 7) / 8; }
bool valid_shape(const MediaFrame& f) {
    return (f.codec == VideoCodec::mjpeg || f.codec == VideoCodec::hevc ||
            f.codec == VideoCodec::h264) &&
        f.sequence != 0 && !f.bytes.empty() &&
        f.bytes.size() <= kMaxCompressedSampleBytes + (f.codec == VideoCodec::mjpeg ? 24 : 58) &&
        (f.codec != VideoCodec::mjpeg || !f.idr);
}
bool valid_body(const MediaFrame& f) {
    if (f.codec == VideoCodec::mjpeg) {
        const auto decoded = decode_mjpeg(f.bytes, f.generation);
        return decoded && decoded->sequence == f.sequence && !f.idr;
    }
    const auto decoded = decode_annex_b(f.bytes, f.codec);
    return decoded && decoded->generation == f.generation &&
        decoded->encoded_sequence == f.sequence && decoded->idr == f.idr;
}
void put(std::vector<std::uint8_t>& b, std::size_t offset, std::uint64_t n, std::size_t size) {
    for (std::size_t i = size; i; --i) { b[offset + i - 1] = static_cast<std::uint8_t>(n); n >>= 8; }
}
std::uint64_t get(std::span<const std::uint8_t> b, std::size_t offset, std::size_t size) {
    std::uint64_t n = 0;
    for (std::size_t i = 0; i < size; ++i) n = (n << 8) | b[offset + i];
    return n;
}
struct Header {
    VideoCodec codec{};
    bool idr{}, parity{};
    std::uint64_t generation{}, sequence{};
    std::size_t size{}, count{}, index{}, length{};
};
std::optional<Header> parse(std::span<const std::uint8_t> b) {
    if (b.size() < 40 || b.size() > 1168 || b[0] != 1 || b[1] > 1 || b[2] > 1 ||
        b[3] > 1 || (b[1] == 0 && b[3]) || get(b, 30, 2) || get(b, 32, 8)) return {};
    Header h{static_cast<VideoCodec>(b[1]), b[3] != 0, b[2] != 0,
        get(b, 4, 8), get(b, 12, 8), static_cast<std::size_t>(get(b, 20, 4)),
        static_cast<std::size_t>(get(b, 24, 2)), static_cast<std::size_t>(get(b, 26, 2)),
        static_cast<std::size_t>(get(b, 28, 2))};
    if (!h.sequence || !h.size || h.size > kMaxCompressedSampleBytes + (b[1] == 0 ? 24 : 58) ||
        h.count != fragments(h.size) || b.size() != 40 + h.length) return {};
    if (h.parity) {
        if (h.index >= groups(h.count) || h.length != payload) return {};
    } else if (h.index >= h.count || h.length != std::min(payload, h.size - h.index * payload)) return {};
    return h;
}
} // namespace

std::size_t media_packet_count(const MediaFrame& f) {
    if (!valid_shape(f)) return 0;
    const auto count = fragments(f.bytes.size());
    return count + groups(count);
}
std::size_t media_wire_bytes(const MediaFrame& f) {
    const auto packets = media_packet_count(f);
    return packets ? f.bytes.size() + groups(fragments(f.bytes.size())) * payload + packets * 72 : 0;
}
std::optional<std::vector<std::uint8_t>> media_packet(const MediaFrame& f, std::size_t ordinal) {
    if (ordinal >= media_packet_count(f)) return {};
    const auto count = fragments(f.bytes.size());
    const auto group = ordinal / 9;
    const auto within = ordinal % 9;
    const auto group_size = std::min<std::size_t>(8, count - group * 8);
    const bool parity = within == group_size;
    const auto index = parity ? group : group * 8 + within;
    const auto length = parity ? payload : std::min(payload, f.bytes.size() - index * payload);
    std::vector<std::uint8_t> b(40 + length);
    b[0] = 1; b[1] = static_cast<std::uint8_t>(f.codec); b[2] = parity; b[3] = f.idr;
    put(b, 4, f.generation, 8); put(b, 12, f.sequence, 8); put(b, 20, f.bytes.size(), 4);
    put(b, 24, count, 2); put(b, 26, index, 2); put(b, 28, length, 2);
    if (!parity) std::copy_n(f.bytes.data() + index * payload, length, b.begin() + 40);
    else for (std::size_t i = group * 8 * payload; i < std::min(f.bytes.size(), (group + 1) * 8 * payload); ++i)
        b[40 + i % payload] ^= f.bytes[i];
    return b;
}

struct MediaReceiver::Impl {
    struct Entry {
        MediaFrame frame;
        std::vector<std::uint8_t> parity, data_seen, parity_seen;
        std::size_t count{}, present{}, charge{};
        bool complete{}, repaired{};
    };
    VideoCodec codec;
    std::uint64_t generation, retired{}, expected{};
    MediaStats counters;
    std::map<std::uint64_t, Entry> frames;
    std::optional<MediaTime> gap_since, last_refresh, last_feedback;
    explicit Impl(VideoCodec c, std::uint64_t g) : codec(c), generation(g) {
        counters.waiting_idr = c != VideoCodec::mjpeg;
    }
    MediaStats stats() const {
        auto s = counters; s.resident_frames = frames.size(); return s;
    }
    void event(std::vector<MediaEvent>& out, MediaEvent::Kind kind, MediaReason reason = MediaReason::none) {
        out.push_back({kind, reason, counters.recovery_marker, {}, stats()});
    }
    void erase(std::map<std::uint64_t, Entry>::iterator it) {
        counters.charged_bytes -= it->second.charge; frames.erase(it);
    }
    void loss(MediaReason reason, std::vector<MediaEvent>& out) {
        ++counters.lost_frames; ++counters.unrecoverable;
        if (reason == MediaReason::age) ++counters.age_losses;
        if (reason == MediaReason::capacity) ++counters.capacity_losses;
        if (reason == MediaReason::gap) ++counters.gap_losses;
        event(out, MediaEvent::Kind::loss, reason);
    }
    void deliver(std::map<std::uint64_t, Entry>::iterator it, std::vector<MediaEvent>& out) {
        auto f = std::move(it->second.frame);
        if (it->second.repaired) ++counters.recovered_frames;
        erase(it); ++counters.received_frames; counters.last_completed = f.sequence;
        retired = std::max(retired, f.sequence);
        expected = f.sequence == std::numeric_limits<std::uint64_t>::max() ? 0 : f.sequence + 1;
        out.push_back({MediaEvent::Kind::frame, MediaReason::none, counters.recovery_marker, std::move(f), stats()});
    }
    void reset(MediaReason reason, std::vector<MediaEvent>& out) {
        // Only a complete validated refresh can survive a broken dependency chain.
        auto keep = frames.end();
        for (auto it = frames.begin(); it != frames.end(); ++it)
            if (it->second.complete && it->second.frame.idr && it->first > retired) keep = it;
        std::optional<Entry> refresh;
        std::uint64_t refresh_seq = 0;
        if (keep != frames.end()) { refresh_seq = keep->first; refresh = std::move(keep->second); }
        for (const auto& [seq, unused] : frames) retired = std::max(retired, seq);
        frames.clear(); counters.charged_bytes = 0; gap_since.reset(); expected = 0;
        ++counters.recovery_marker; counters.waiting_idr = codec != VideoCodec::mjpeg;
        event(out, MediaEvent::Kind::reset, reason);
        if (refresh) {
            // This IDR supersedes all older references. Future packets may be
            // readmitted against its new chain; only its prefix is retired.
            retired = refresh_seq;
            counters.charged_bytes = refresh->charge;
            auto it = frames.emplace(refresh_seq, std::move(*refresh)).first;
            counters.waiting_idr = false; deliver(it, out);
        }
    }
    void drain(MediaTime now, std::vector<MediaEvent>& out) {
        if (codec == VideoCodec::mjpeg) return;
        // A fresh IDR is an immediate recovery point, not hostage to a lost P AU.
        auto refresh = frames.end();
        for (auto it = frames.begin(); it != frames.end(); ++it)
            if (it->second.complete && it->second.frame.idr &&
                (counters.waiting_idr || it->first > expected)) refresh = it;
        if (refresh != frames.end()) reset(MediaReason::refresh_point, out);
        while (!counters.waiting_idr && expected) {
            auto it = frames.find(expected);
            if (it == frames.end() || !it->second.complete) break;
            deliver(it, out); gap_since.reset();
        }
        if (!counters.waiting_idr && expected && frames.upper_bound(expected) != frames.end()) {
            if (!gap_since) gap_since = now;
        } else gap_since.reset();
    }
    void periodic(MediaTime now, std::vector<MediaEvent>& out) {
        if (counters.waiting_idr && (!last_refresh || now - *last_refresh >= 100ms)) {
            last_refresh = now; event(out, MediaEvent::Kind::refresh);
        }
        if (!last_feedback || now - *last_feedback >= 100ms) {
            last_feedback = now; event(out, MediaEvent::Kind::feedback);
        }
    }
    void expire(MediaTime now, std::vector<MediaEvent>& out) {
        for (auto it = frames.begin(); it != frames.end();) {
            if (now - it->second.frame.first_arrival < 150ms) { ++it; continue; }
            loss(MediaReason::age, out);
            if (codec != VideoCodec::mjpeg) { reset(MediaReason::age, out); break; }
            retired = std::max(retired, it->first); auto old = it++; erase(old);
        }
        if (gap_since && now - *gap_since >= 40ms) {
            loss(MediaReason::gap, out); reset(MediaReason::gap, out);
        }
    }
};
MediaReceiver::MediaReceiver(VideoCodec c, std::uint64_t g) : impl_(std::make_unique<Impl>(c, g)) {}
MediaReceiver::~MediaReceiver() = default;
MediaStats MediaReceiver::stats() const { return impl_->stats(); }
std::vector<MediaEvent> MediaReceiver::poll(MediaTime now) {
    std::vector<MediaEvent> out; impl_->expire(now, out); impl_->drain(now, out); impl_->periodic(now, out); return out;
}
std::vector<MediaEvent> MediaReceiver::recover(MediaReason reason, MediaTime now) {
    std::vector<MediaEvent> out; impl_->loss(reason, out); impl_->reset(reason, out); impl_->periodic(now, out); return out;
}
std::vector<MediaEvent> MediaReceiver::input(std::span<const std::uint8_t> bytes, MediaTime now) {
    auto& s = *impl_;
    std::vector<MediaEvent> out;
    s.expire(now, out);
    const auto h = parse(bytes);
    if (!h || h->codec != s.codec || h->generation != s.generation || h->sequence <= s.retired ||
        (s.counters.waiting_idr && !h->idr)) { s.periodic(now, out); return out; }
    auto it = s.frames.find(h->sequence);
    if (it == s.frames.end()) {
        const auto charge = h->size + groups(h->count) * payload + h->count + groups(h->count) + metadata_charge;
        while (s.frames.size() >= 8 || s.counters.charged_bytes + charge > kMediaAllocationLimit) {
            auto oldest = std::min_element(s.frames.begin(), s.frames.end(), [](const auto& a, const auto& b) {
                return a.second.frame.first_arrival < b.second.frame.first_arrival;
            });
            s.loss(MediaReason::capacity, out);
            if (s.codec != VideoCodec::mjpeg) { s.reset(MediaReason::capacity, out); break; }
            s.retired = std::max(s.retired, oldest->first); s.erase(oldest);
        }
        if (h->sequence <= s.retired || (s.counters.waiting_idr && !h->idr)) { s.periodic(now, out); return out; }
        Impl::Entry e;
        e.frame = {h->codec, h->generation, h->sequence, h->idr, std::vector<std::uint8_t>(h->size), now};
        e.count = h->count; e.charge = charge;
        e.parity.resize(groups(h->count) * payload); e.data_seen.resize(h->count); e.parity_seen.resize(groups(h->count));
        s.counters.charged_bytes += charge;
        it = s.frames.emplace(h->sequence, std::move(e)).first;
    }
    auto& e = it->second;
    const auto incoming = bytes.subspan(40);
    bool conflict = e.frame.bytes.size() != h->size || e.frame.idr != h->idr;
    if (!conflict) {
        auto& seen = h->parity ? e.parity_seen : e.data_seen;
        auto& storage = h->parity ? e.parity : e.frame.bytes;
        const auto offset = h->index * payload;
        if (seen[h->index]) conflict = !std::equal(incoming.begin(), incoming.end(), storage.data() + offset);
        else {
            std::copy(incoming.begin(), incoming.end(), storage.data() + offset);
            seen[h->index] = 1;
            if (!h->parity) ++e.present;
        }
    }
    if (conflict) {
        s.loss(MediaReason::conflict, out);
        if (s.codec != VideoCodec::mjpeg) s.reset(MediaReason::conflict, out);
        else { s.retired = std::max(s.retired, h->sequence); s.erase(it); }
        s.periodic(now, out); return out;
    }
    const auto group = h->parity ? h->index : h->index / 8;
    if (e.parity_seen[group]) {
        std::size_t missing = 0, missing_index = 0;
        for (std::size_t i = group * 8; i < std::min(e.count, (group + 1) * 8); ++i)
            if (!e.data_seen[i]) { ++missing; missing_index = i; }
        if (missing == 1) {
            std::array<std::uint8_t, payload> repaired{};
            std::copy_n(e.parity.data() + group * payload, payload, repaired.begin());
            for (std::size_t i = group * 8; i < std::min(e.count, (group + 1) * 8); ++i) {
                if (i == missing_index) continue;
                for (std::size_t j = 0; j < std::min(payload, e.frame.bytes.size() - i * payload); ++j)
                    repaired[j] ^= e.frame.bytes[i * payload + j];
            }
            std::copy_n(repaired.begin(), std::min(payload, e.frame.bytes.size() - missing_index * payload),
                        e.frame.bytes.data() + missing_index * payload);
            e.data_seen[missing_index] = 1; ++e.present; e.repaired = true; ++s.counters.recovered_fragments;
        }
    }
    if (e.present == e.count && !e.complete) {
        if (!valid_body(e.frame)) {
            s.loss(MediaReason::invalid_body, out);
            if (s.codec != VideoCodec::mjpeg) s.reset(MediaReason::invalid_body, out);
            else { s.retired = std::max(s.retired, h->sequence); s.erase(it); }
        } else {
            e.complete = true;
            if (s.codec == VideoCodec::mjpeg) {
                s.deliver(it, out);
                for (auto old = s.frames.begin(); old != s.frames.end() && old->first <= s.retired;) {
                    auto next = old++; s.erase(next);
                }
            }
        }
    }
    s.drain(now, out); s.periodic(now, out); return out;
}

struct MediaPacer::Impl {
    VideoCodec codec;
    std::uint64_t generation, rate, last_sequence{};
    double tokens{1200};
    MediaTime updated;
    std::optional<MediaFrame> active;
    MediaTime deadline{};
    std::size_t ordinal{};
    MediaPacerStats counters;
    Impl(VideoCodec c, std::uint64_t g, std::uint64_t r, MediaTime now)
        : codec(c), generation(g), rate(r), updated(now) { counters.waiting_idr = c != VideoCodec::mjpeg; }
    double credit(MediaTime now) const {
        return std::min(2400.0, tokens + std::max(0.0, std::chrono::duration<double>(now - updated).count()) * static_cast<double>(rate));
    }
    MediaAdmission drop(MediaReason reason) {
        active.reset(); ordinal = 0;
        if (reason == MediaReason::sender_deadline || reason == MediaReason::source_stale) ++counters.sender_deadlines;
        if (codec != VideoCodec::mjpeg) { counters.waiting_idr = true; ++counters.skipped_access_units; }
        return {false, reason, counters.waiting_idr};
    }
};
MediaPacer::MediaPacer(VideoCodec c, std::uint64_t g, std::uint64_t rate, MediaTime now)
    : impl_(std::make_unique<Impl>(c, g, rate, now)) {}
MediaPacer::~MediaPacer() = default;
bool MediaPacer::can_start(MediaTime now) const {
    return !impl_->active || now >= impl_->deadline;
}
MediaAdmission MediaPacer::poll(MediaTime now) {
    if (impl_->active && now >= impl_->deadline) return impl_->drop(MediaReason::sender_deadline);
    return {true, MediaReason::none, impl_->counters.waiting_idr};
}
MediaAdmission MediaPacer::discard(MediaReason reason) { return impl_->drop(reason); }
MediaAdmission MediaPacer::submit(MediaFrame f, MediaTime now) {
    auto& s = *impl_;
    poll(now);
    auto reject = [&](MediaReason reason) { ++s.counters.rejected_frames; return s.drop(reason); };
    if (s.active) return reject(MediaReason::skipped_access_unit);
    if (!valid_shape(f) || f.codec != s.codec || f.generation != s.generation || !valid_body(f) || f.sequence <= s.last_sequence)
        return reject(MediaReason::invalid_body);
    if (f.first_arrival > now || now - f.first_arrival >= 250ms) return reject(MediaReason::source_stale);
    if (s.codec != VideoCodec::mjpeg && (s.counters.waiting_idr || (s.last_sequence && f.sequence != s.last_sequence + 1)) && !f.idr)
        return reject(MediaReason::skipped_access_unit);
    const auto deadline = std::min(now + 100ms, f.first_arrival + 250ms);
    const auto budget = s.credit(now) + static_cast<double>(s.rate) * std::chrono::duration<double>(deadline - now).count();
    // The deadline is exclusive: a final token available only at expiry is late.
    if (!s.rate || static_cast<double>(media_wire_bytes(f)) >= budget) return reject(MediaReason::frame_exceeds_rate_budget);
    s.last_sequence = f.sequence; s.counters.waiting_idr = false;
    s.active = std::move(f); s.deadline = deadline; s.ordinal = 0;
    return {true, MediaReason::none, false};
}
std::optional<std::vector<std::uint8_t>> MediaPacer::next_datagram(MediaTime now) {
    poll(now);
    auto& s = *impl_;
    if (!s.active) return {};
    auto packet = media_packet(*s.active, s.ordinal);
    const auto charge = packet->size() + 32;
    s.tokens = s.credit(now); s.updated = std::max(s.updated, now);
    if (s.tokens < static_cast<double>(charge)) return {};
    s.tokens -= static_cast<double>(charge); s.counters.sent_bytes += charge;
    if (++s.ordinal == media_packet_count(*s.active)) { s.active.reset(); s.ordinal = 0; }
    return packet;
}
std::optional<MediaTime> MediaPacer::next_deadline(MediaTime now) const {
    const auto& s = *impl_;
    if (!s.active) return {};
    const auto packet = media_packet(*s.active, s.ordinal);
    const double need = static_cast<double>(packet->size() + 32) - s.credit(now);
    if (need <= 0 || now >= s.deadline) return now;
    if (!s.rate) return s.deadline;
    const auto delay = std::chrono::nanoseconds(static_cast<std::int64_t>(std::ceil(need / static_cast<double>(s.rate) * 1e9)));
    return std::min(s.deadline, now + delay);
}
std::optional<MediaTime> MediaPacer::active_deadline() const { return impl_->active ? std::optional(impl_->deadline) : std::nullopt; }
MediaPacerStats MediaPacer::stats() const { return impl_->counters; }
} // namespace kvmux::relay
