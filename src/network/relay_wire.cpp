#include "network/relay_wire.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <type_traits>

namespace kvmux::relay::wire {
namespace {
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
constexpr EnvelopeKind kinds[]{EnvelopeKind::hello, EnvelopeKind::welcome, EnvelopeKind::confirm,
    EnvelopeKind::ready, EnvelopeKind::busy, EnvelopeKind::kcp, EnvelopeKind::media,
    EnvelopeKind::challenge, EnvelopeKind::proof, EnvelopeKind::cancel, EnvelopeKind::cancel_ack};
constexpr ControlConnectionState connections[]{ControlConnectionState::disconnected,
    ControlConnectionState::opening, ControlConnectionState::monitoring, ControlConnectionState::clearing,
    ControlConnectionState::ready, ControlConnectionState::stalled, ControlConnectionState::reconnecting,
    ControlConnectionState::fault, ControlConnectionState::stopping};
constexpr MediaReason reasons[]{MediaReason::none, MediaReason::malformed, MediaReason::conflict,
    MediaReason::invalid_body, MediaReason::age, MediaReason::capacity, MediaReason::gap,
    MediaReason::ingress_overflow, MediaReason::decoder_failure, MediaReason::sender_abort,
    MediaReason::refresh_point, MediaReason::sender_deadline, MediaReason::source_stale,
    MediaReason::frame_exceeds_rate_budget, MediaReason::skipped_access_unit};
constexpr CancelReason cancels[]{CancelReason::focus, CancelReason::host, CancelReason::release, CancelReason::disconnect};
template<class T, std::size_t N> int index(const T (&table)[N], T value) {
    auto p = std::find(std::begin(table), std::end(table), value);
    return p == std::end(table) ? -1 : static_cast<int>(p - std::begin(table));
}
struct Writer {
    std::vector<std::uint8_t> bytes;
    bool ok{true};
    void integer(std::uint64_t v, unsigned n) {
        for (unsigned i = n; i > 0; --i) bytes.push_back(static_cast<std::uint8_t>(v >> ((i-1)*8)));
    }
    void zeros(unsigned n) { integer(0,n); }
    void number(double v) { ok &= std::isfinite(v); integer(std::bit_cast<std::uint64_t>(v),8); }
    void mapped(int v) { if (v < 0) ok=false; integer(static_cast<std::uint64_t>(v),1); }
};
struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t pos{};
    bool ok{true};
    std::uint64_t integer(unsigned n) {
        if (n > bytes.size()-pos) { ok=false; return 0; }
        std::uint64_t v{};
        for (unsigned i=0;i<n;++i) v=(v<<8)|bytes[pos++];
        return v;
    }
    std::uint8_t u8() { return static_cast<std::uint8_t>(integer(1)); }
    std::uint16_t u16() { return static_cast<std::uint16_t>(integer(2)); }
    void zeros(unsigned n) { if(integer(n)!=0) ok=false; }
    bool boolean() { auto v=u8(); if(v>1) ok=false; return v==1; }
    double number() { auto v=std::bit_cast<double>(integer(8)); if(!std::isfinite(v)) ok=false; return v; }
    bool done() const { return ok && pos==bytes.size(); }
};
bool key(std::uint8_t k) { return (k>=0x04 && k<=0x73) || (k>=0x7f && k<=0x82) ||
    (k>=0x85 && k<=0x87) || (k>=0x89 && k<=0x8f); }
bool coordinate(double v) { return std::isfinite(v) && v>=0 && v<=1; }
void state(Writer& w,const DesiredInputState& s) {
    w.ok &= valid_state(s); w.integer(s.modifiers,1);
    for(auto k:s.keys) w.integer(k,1);
    w.integer(s.buttons,1); w.mapped(s.mode==MouseMode::absolute ? 0 : s.mode==MouseMode::relative ? 1 : -1);
    w.integer(s.absolute_x,2); w.integer(s.absolute_y,2);
}
DesiredInputState state(Reader& r) {
    DesiredInputState s; s.modifiers=r.u8(); for(auto& k:s.keys) k=r.u8();
    s.buttons=r.u8(); auto m=r.u8(); if(m>1) r.ok=false;
    s.mode=m==0?MouseMode::absolute:MouseMode::relative;
    s.absolute_x=r.u16(); s.absolute_y=r.u16(); r.ok &= valid_state(s); return s;
}
void cancel_body(Writer& w,const Cancel& c) { w.ok &= c.intent!=0; w.integer(c.intent,8); auto i=index(cancels,c.reason); w.mapped(i<0?-1:i+1); w.zeros(7); }
Cancel cancel_body(Reader& r) {
    Cancel c; c.intent=r.integer(8); auto reason=r.u8();
    if(reason<1 || reason>4 || !c.intent) r.ok=false; else c.reason=cancels[reason-1];
    r.zeros(7); return c;
}
void edge_body(Writer& w,const ControlPayload& p) {
    std::visit([&](const auto& e) {
        using T=std::decay_t<decltype(e)>;
        if constexpr(std::is_same_v<T,KeyEdge>) { w.integer(1,1); w.integer(e.usage,1); w.integer(e.pressed,1); w.ok &= key(e.usage)||(e.usage>=0xe0 && e.usage<=0xe7); }
        else if constexpr(std::is_same_v<T,AbsoluteMotion>) { w.integer(2,1); w.number(e.x); w.number(e.y); w.ok &= coordinate(e.x)&&coordinate(e.y); }
        else if constexpr(std::is_same_v<T,RelativeMotion>) { w.integer(3,1); w.number(e.dx); w.number(e.dy); }
        else if constexpr(std::is_same_v<T,ButtonEdge>) { w.integer(4,1); w.integer(e.button,1); w.integer(e.pressed,1); w.number(e.x); w.number(e.y); w.ok &= e.button<=2&&coordinate(e.x)&&coordinate(e.y); }
        else { w.integer(5,1); w.number(e.steps); w.number(e.x); w.number(e.y); w.ok &= coordinate(e.x)&&coordinate(e.y); }
    },p);
}
ControlPayload edge_body(Reader& r) {
    switch(r.u8()) {
    case 1: { KeyEdge e; e.usage=r.u8(); e.pressed=r.boolean(); r.ok &= key(e.usage)||(e.usage>=0xe0&&e.usage<=0xe7); return e; }
    case 2: { AbsoluteMotion e{r.number(),r.number()}; r.ok &= coordinate(e.x)&&coordinate(e.y); return e; }
    case 3: return RelativeMotion{r.number(),r.number()};
    case 4: { ButtonEdge e; e.button=r.u8(); e.pressed=r.boolean(); e.x=r.number(); e.y=r.number(); r.ok &= e.button<=2&&coordinate(e.x)&&coordinate(e.y); return e; }
    case 5: { VerticalWheel e{r.number(),r.number(),r.number()}; r.ok &= coordinate(e.x)&&coordinate(e.y); return e; }
    default: r.ok=false; return KeyEdge{};
    }
}
bool direction(unsigned type,Direction d) { return (type==1||type==3||type==12||type==17) ? d==Direction::server_to_client : d==Direction::client_to_server; }
}
bool valid_state(const DesiredInputState& s) {
    if(s.buttons>7 || s.absolute_x>4095 || s.absolute_y>4095 || (s.mode!=MouseMode::absolute&&s.mode!=MouseMode::relative)) return false;
    for(std::size_t i=0;i<s.keys.size();++i) if(s.keys[i]) {
        if(!key(s.keys[i])) return false;
        for(std::size_t j=0;j<i;++j) if(s.keys[j]==s.keys[i]) return false;
    }
    return true;
}
bool same_state(const DesiredInputState& a,const DesiredInputState& b) {
    return a.modifiers==b.modifiers&&a.keys==b.keys&&a.buttons==b.buttons&&a.mode==b.mode&&a.absolute_x==b.absolute_x&&a.absolute_y==b.absolute_y;
}
std::optional<std::vector<std::uint8_t>> encode_raw(const RawBody& body) {
    Writer w;
    std::visit([&](const auto& b) {
        using T=std::decay_t<decltype(b)>;
        if constexpr(std::is_same_v<T,Hello>) { w.ok &= b.codecs>0&&b.codecs<=3; w.integer(b.codecs,1); w.zeros(3); }
        else if constexpr(std::is_same_v<T,Welcome>) { w.mapped(b.codec==VideoCodec::mjpeg?0:b.codec==VideoCodec::hevc?1:-1); w.zeros(3); w.integer(b.generation,8); w.ok &= b.generation!=0; }
        else if constexpr(std::is_same_v<T,Busy>) w.mapped(b.reason==BusyReason::busy?0:b.reason==BusyReason::no_common_codec?1:-1);
        else if constexpr(std::is_same_v<T,Challenge>) { w.integer(b.id,8); w.ok &= b.id!=0; }
        else if constexpr(std::is_same_v<T,Proof>) {
            w.integer(b.challenge,8); w.integer(b.intent,8); w.integer((b.active?1:0)|(b.video_fresh?2:0),1); w.zeros(7); w.integer(b.presented_sequence,8);
            w.ok &= b.challenge!=0&&(!b.active||b.intent!=0)&&(!b.video_fresh||b.presented_sequence!=0);
        } else cancel_body(w,b);
    },body);
    if(!w.ok) return {}; return w.bytes;
}
std::optional<RawBody> decode_raw(EnvelopeKind kind,std::span<const std::uint8_t> bytes) {
    Reader r{bytes}; RawBody b;
    switch(kind) {
    case EnvelopeKind::hello: { Hello h{r.u8()}; r.zeros(3); r.ok &= h.codecs>0&&h.codecs<=3; b=h; break; }
    case EnvelopeKind::welcome: case EnvelopeKind::confirm: case EnvelopeKind::ready: {
        auto c=r.u8(); r.zeros(3); Welcome v{c==0?VideoCodec::mjpeg:VideoCodec::hevc,r.integer(8)}; r.ok &= c<=1&&v.generation!=0; b=v; break;
    }
    case EnvelopeKind::busy: { auto v=r.u8(); r.ok &= v<=1; b=Busy{v==0?BusyReason::busy:BusyReason::no_common_codec}; break; }
    case EnvelopeKind::challenge: { Challenge c{r.integer(8)}; r.ok &= c.id!=0; b=c; break; }
    case EnvelopeKind::proof: { Proof p; p.challenge=r.integer(8); p.intent=r.integer(8); auto f=r.u8(); p.active=(f&1)!=0; p.video_fresh=(f&2)!=0; r.zeros(7); p.presented_sequence=r.integer(8); r.ok &= f<=3&&p.challenge!=0&&(!p.active||p.intent!=0)&&(!p.video_fresh||p.presented_sequence!=0); b=p; break; }
    case EnvelopeKind::cancel: case EnvelopeKind::cancel_ack: b=cancel_body(r); break;
    default: return {};
    }
    if(!r.done()) return {}; return b;
}
std::optional<Envelope> decode_envelope(std::span<const std::uint8_t> bytes) {
    if(bytes.size()<32||bytes.size()>1200) return {};
    Reader r{bytes.first(32)};
    if(r.integer(4)!=0x4b564d58||r.u8()!=4) return {};
    auto k=r.u8(); if(k<1||k>11) return {};
    auto n=r.u16(); Tuple t; t.session=r.integer(8); t.nonce=r.integer(8); t.conversation=static_cast<std::uint32_t>(r.integer(4)); r.zeros(4);
    if(!r.done()||bytes.size()!=32U+n||!t.nonce) return {};
    auto kind=kinds[k-1];
    if(kind==EnvelopeKind::hello||kind==EnvelopeKind::busy) { if(t.session||t.conversation) return {}; }
    else if(!t.session||!t.conversation) return {};
    auto body=bytes.subspan(32);
    if(kind==EnvelopeKind::kcp) { if(n<1||n>1168) return {}; }
    else if(kind==EnvelopeKind::media) { if(n<40||n>1168) return {}; }
    else if(!decode_raw(kind,body)) return {};
    return Envelope{kind,t,body};
}
std::optional<std::vector<std::uint8_t>> encode_envelope(const Envelope& e) {
    if(e.body.size()>1168) return {};
    auto k=index(kinds,e.kind); if(k<0) return {};
    Writer w; w.integer(0x4b564d58,4); w.integer(4,1); w.integer(static_cast<unsigned>(k+1),1); w.integer(e.body.size(),2);
    w.integer(e.tuple.session,8); w.integer(e.tuple.nonce,8); w.integer(e.tuple.conversation,4); w.zeros(4);
    w.bytes.insert(w.bytes.end(),e.body.begin(),e.body.end());
    if(!decode_envelope(w.bytes)) return {}; return w.bytes;
}
std::optional<std::vector<std::uint8_t>> encode_control(const Control& c,Direction d) {
    Writer w; unsigned type{};
    std::visit([&](const auto& b) {
        using T=std::decay_t<decltype(b)>;
        if constexpr(std::is_same_v<T,Status>) { type=1; w.ok &= b.epoch!=0; w.integer(b.epoch,8); w.mapped(index(connections,b.connection)); w.integer((b.usb_ready?1:0)|(b.release_confirmed?2:0)|(b.ordinary_input_pending?4:0),1); w.zeros(2); w.integer(b.canceled_through,8); w.integer(b.completed_ordinary_sequence,8); }
        else if constexpr(std::is_same_v<T,Sync>||std::is_same_v<T,StateAck>) {
            type=std::is_same_v<T,Sync>?2:3; w.ok &= b.epoch&&b.intent&&b.revision; w.integer(b.epoch,8); w.integer(b.intent,8); w.integer(b.revision,8);
            if constexpr(std::is_same_v<T,Sync>) { w.ok &= b.challenge!=0; w.integer(b.challenge,8); }
            w.integer(b.edge_floor,8); state(w,b.state);
        } else if constexpr(std::is_same_v<T,Edge>) { type=4; w.ok &= b.epoch&&b.intent&&b.sequence&&b.challenge; w.integer(b.epoch,8); w.integer(b.intent,8); w.integer(b.sequence,8); w.integer(b.source_sequence,8); w.integer(b.challenge,8); edge_body(w,b.payload); }
        else if constexpr(std::is_same_v<T,Cancel>) { type=5; cancel_body(w,b); }
        else if constexpr(std::is_same_v<T,RefreshRequest>) { type=6; w.ok &= b.generation!=0; w.integer(b.generation,8); w.mapped(index(reasons,b.reason)); w.zeros(7); }
        else if constexpr(std::is_same_v<T,PasteBegin>) { type=8; w.ok &= b.transaction_id && b.normalized_bytes>=1 && b.normalized_bytes<=65536; w.integer(b.transaction_id,8); w.integer(b.normalized_bytes,4); w.integer(b.crc32,4); }
        else if constexpr(std::is_same_v<T,PasteChunk>) { type=9; w.ok &= b.transaction_id && !b.payload.empty() && b.payload.size()<=960; w.integer(b.transaction_id,8); w.integer(b.chunk_index,4); w.integer(b.payload.size(),2); w.zeros(2); w.bytes.insert(w.bytes.end(),b.payload.begin(),b.payload.end()); }
        else if constexpr(std::is_same_v<T,PasteCommit>) { type=10; w.ok &= b.transaction_id && b.authorization_token; w.integer(b.transaction_id,8); w.integer(b.authorization_token,8); }
        else if constexpr(std::is_same_v<T,PasteAuthorize>) { type=16; w.ok &= b.transaction_id && b.challenge && b.request_id; w.integer(b.transaction_id,8); w.integer(b.challenge,8); w.integer(b.request_id,8); }
        else if constexpr(std::is_same_v<T,PasteAuthorized>) { type=17; w.ok &= b.transaction_id && b.request_id && b.token; w.integer(b.transaction_id,8); w.integer(b.request_id,8); w.integer(b.token,8); }
        else if constexpr(std::is_same_v<T,PasteCancel>) { type=11; auto reason=static_cast<unsigned>(b.reason); w.ok &= b.transaction_id && reason>=1 && reason<=5; w.integer(b.transaction_id,8); w.integer(reason,1); w.zeros(7); }
        else if constexpr(std::is_same_v<T,PasteKeepalive>) { type=13; w.ok &= b.transaction_id; w.integer(b.transaction_id,8); }
        else if constexpr(std::is_same_v<T,PasteStatus>) { type=12; auto state=static_cast<unsigned>(b.state), reason=static_cast<unsigned>(b.reason); w.ok &= b.transaction_id && state>=1 && state<=7 && reason<=12; w.integer(b.transaction_id,8); w.integer(state,1); w.zeros(3); w.integer(b.next_chunk,4); w.integer(b.accepted_bytes,4); w.integer(b.completed_bytes,4); w.integer(reason,1); w.zeros(3); }
        else { type=7; w.ok &= b.generation!=0; w.integer(b.generation,8); const auto& s=b.stats;
            for(auto v:{s.received_frames,s.recovered_fragments,s.recovered_frames,s.lost_frames,s.last_completed,s.age_losses,s.capacity_losses,s.gap_losses,s.unrecoverable}) w.integer(v,8);
            w.integer(s.waiting_idr,1); w.zeros(7);
        }
    },c);
    if(!w.ok||!direction(type,d)||w.bytes.size()>1020) return {};
    Writer head; head.integer(type,1); head.zeros(1); head.integer(w.bytes.size(),2);
    head.bytes.insert(head.bytes.end(),w.bytes.begin(),w.bytes.end()); return head.bytes;
}
std::optional<Control> decode_control(std::span<const std::uint8_t> bytes,Direction d) {
    if(bytes.size()<4||bytes.size()>1024) return {};
    Reader h{bytes.first(4)}; auto type=h.u8(); h.zeros(1); auto n=h.u16();
    if(!h.done()||n!=bytes.size()-4||type<1||type>17||!direction(type,d)) return {};
    Reader r{bytes.subspan(4)}; Control c;
    switch(type) {
    case 1: { Status b; b.epoch=r.integer(8); auto conn=r.u8(),flags=r.u8(); r.zeros(2); b.canceled_through=r.integer(8); b.completed_ordinary_sequence=r.integer(8); r.ok &= b.epoch!=0&&conn<=8&&flags<=7; if(conn<=8)b.connection=connections[conn]; b.usb_ready=(flags&1)!=0; b.release_confirmed=(flags&2)!=0; b.ordinary_input_pending=(flags&4)!=0; c=b; break; }
    case 2: { Sync b; b.epoch=r.integer(8); b.intent=r.integer(8); b.revision=r.integer(8); b.challenge=r.integer(8); b.edge_floor=r.integer(8); b.state=state(r); r.ok &= b.epoch&&b.intent&&b.revision&&b.challenge; c=b; break; }
    case 3: { StateAck b; b.epoch=r.integer(8); b.intent=r.integer(8); b.revision=r.integer(8); b.edge_floor=r.integer(8); b.state=state(r); r.ok &= b.epoch&&b.intent&&b.revision; c=b; break; }
    case 4: { Edge b; b.epoch=r.integer(8); b.intent=r.integer(8); b.sequence=r.integer(8); b.source_sequence=r.integer(8); b.challenge=r.integer(8); b.payload=edge_body(r); r.ok &= b.epoch&&b.intent&&b.sequence&&b.source_sequence&&b.challenge; c=b; break; }
    case 5: c=cancel_body(r); break;
    case 6: { RefreshRequest b; b.generation=r.integer(8); auto reason=r.u8(); r.zeros(7); r.ok &= b.generation!=0&&reason<std::size(reasons); if(reason<std::size(reasons)) b.reason=reasons[reason]; c=b; break; }
    case 7: { MediaFeedback b; b.generation=r.integer(8); auto& s=b.stats; s.received_frames=r.integer(8); s.recovered_fragments=r.integer(8); s.recovered_frames=r.integer(8); s.lost_frames=r.integer(8); s.last_completed=r.integer(8); s.age_losses=r.integer(8); s.capacity_losses=r.integer(8); s.gap_losses=r.integer(8); s.unrecoverable=r.integer(8); s.waiting_idr=r.boolean(); r.zeros(7); r.ok &= b.generation!=0; c=b; break; }
    case 8: { PasteBegin b{r.integer(8), static_cast<std::uint32_t>(r.integer(4)), static_cast<std::uint32_t>(r.integer(4))}; r.ok &= b.transaction_id && b.normalized_bytes>=1 && b.normalized_bytes<=65536; c=b; break; }
    case 9: { PasteChunk b; b.transaction_id=r.integer(8); b.chunk_index=static_cast<std::uint32_t>(r.integer(4)); auto n=r.u16(); r.zeros(2); r.ok &= b.transaction_id && n>=1 && n<=960 && n<=r.bytes.size()-r.pos; if(r.ok) b.payload.assign(r.bytes.begin()+static_cast<std::ptrdiff_t>(r.pos),r.bytes.begin()+static_cast<std::ptrdiff_t>(r.pos+n)), r.pos+=n; c=std::move(b); break; }
    case 10: { PasteCommit b{r.integer(8),r.integer(8)}; r.ok &= b.transaction_id&&b.authorization_token; c=b; break; }
    case 16: { PasteAuthorize b{r.integer(8),r.integer(8),r.integer(8)}; r.ok &= b.transaction_id&&b.challenge&&b.request_id; c=b; break; }
    case 17: { PasteAuthorized b{r.integer(8),r.integer(8),r.integer(8)}; r.ok &= b.transaction_id&&b.request_id&&b.token; c=b; break; }
    case 11: { PasteCancel b; b.transaction_id=r.integer(8); auto reason=r.u8(); r.zeros(7); r.ok &= b.transaction_id && reason>=1 && reason<=5; if(reason>=1&&reason<=5) b.reason=static_cast<PasteCancelReason>(reason); c=b; break; }
    case 13: { PasteKeepalive b{r.integer(8)}; r.ok &= b.transaction_id; c=b; break; }
    case 12: { PasteStatus b; b.transaction_id=r.integer(8); auto state=r.u8(); r.zeros(3); b.next_chunk=static_cast<std::uint32_t>(r.integer(4)); b.accepted_bytes=static_cast<std::uint32_t>(r.integer(4)); b.completed_bytes=static_cast<std::uint32_t>(r.integer(4)); auto reason=r.u8(); r.zeros(3); r.ok &= b.transaction_id && state>=1 && state<=7 && reason<=12; if(state>=1&&state<=7) b.state=static_cast<PasteState>(state); if(reason<=12) b.reason=static_cast<PasteStatusReason>(reason); c=b; break; }
    }
    if(!r.done()) return {}; return c;
}
} // namespace kvmux::relay::wire
