#include "network/relay_wire.hpp"
#include <bit>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <source_location>
using namespace kvmux;
namespace w=kvmux::relay::wire;
using Bytes=std::vector<std::uint8_t>;
void check(bool ok,std::source_location loc=std::source_location::current()) {
    if(!ok) { std::cerr<<"wire check failed at "<<loc.line()<<'\n'; std::abort(); }
}
void control(const w::Control& c,w::Direction d,std::uint8_t type,std::size_t payload) {
    auto b=w::encode_control(c,d); check(b.has_value());
    check(b->size()==payload+4&&(*b)[0]==type&&(*b)[1]==0&&(*b)[2]==payload/256&&(*b)[3]==payload%256);
    auto decoded=w::decode_control(*b,d); check(decoded.has_value()); check(w::encode_control(*decoded,d)==b);
    auto other=d==w::Direction::client_to_server?w::Direction::server_to_client:w::Direction::client_to_server;
    check(!w::decode_control(*b,other)); check(!w::encode_control(c,other));
    for(std::size_t n=0;n<b->size();++n) check(!w::decode_control(std::span(*b).first(n),d));
    auto bad=*b; bad.push_back(0); check(!w::decode_control(bad,d));
    bad=*b; bad[1]=1; check(!w::decode_control(bad,d));
    bad=*b; bad[0]=14; check(!w::decode_control(bad,d));
}
void envelope_golden_offsets_and_1200_ceiling() {
    Bytes body(1168,0x5a); w::Tuple tuple{0x0102030405060708,0x1112131415161718,0x21222324};
    auto b=w::encode_envelope({w::EnvelopeKind::kcp,tuple,body}); check(b&&b->size()==1200);
    Bytes header{0x4b,0x56,0x4d,0x58,4,6,4,0x90,1,2,3,4,5,6,7,8,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x21,0x22,0x23,0x24,0,0,0,0};
    check(std::equal(header.begin(),header.end(),b->begin()));
    check(w::encode_envelope({w::EnvelopeKind::media,tuple,body}).has_value());
    body.push_back(0); check(!w::encode_envelope({w::EnvelopeKind::kcp,tuple,body}));
    for(std::size_t n=0;n<b->size();++n) check(!w::decode_envelope(std::span(*b).first(n)));
    for(auto offset:{0,4,5,28,29,30,31}) { auto bad=*b; bad[static_cast<std::size_t>(offset)]=255; check(!w::decode_envelope(bad)); }
    for(auto offset:{8,16,24}) { auto bad=*b; auto width=offset==24?4:8; std::fill(bad.begin()+offset,bad.begin()+offset+width,0); check(!w::decode_envelope(bad)); }
    auto extra=*b; extra.push_back(0); check(!w::decode_envelope(extra));
    check(!w::encode_envelope({w::EnvelopeKind::media,tuple,Bytes(39)}));
    check(!w::encode_envelope({w::EnvelopeKind::kcp,tuple,{}}));
}
void control_golden_vectors_and_exact_lengths() {
    using D=w::Direction; auto c=D::client_to_server,s=D::server_to_client;
    DesiredInputState state; state.keys={4,0,5,0,0,0}; state.absolute_x=4095;
    control(w::Status{1,ControlConnectionState::ready,true,true,2,false,9},s,1,28);
    control(w::Sync{1,2,3,4,5,state},c,2,53); control(w::StateAck{1,2,3,5,state},s,3,45);
    control(w::Edge{1,2,3,4,5,KeyEdge{4,true}},c,4,43);
    control(w::Cancel{2,w::CancelReason::host},c,5,16);
    control(w::RefreshRequest{1,relay::MediaReason::skipped_access_unit},c,6,16);
    control(w::MediaFeedback{1,{}},c,7,88);
    auto sync=*w::encode_control(w::Sync{1,2,3,4,5,state},c);
    check(sync[11]==1&&sync[19]==2&&sync[27]==3&&sync[35]==4&&sync[43]==5&&sync[45]==4&&sync[47]==5&&sync[53]==0x0f&&sync[54]==0xff);
    auto status=*w::encode_control(w::Status{1,ControlConnectionState::ready,true,true,2,false,9},s);
    check(status==Bytes({1,0,0,28,0,0,0,0,0,0,0,1,4,3,0,0,0,0,0,0,0,0,0,2,0,0,0,0,0,0,0,9}));
    for(auto offset:{12,13,14,15}) { auto bad=status; bad[static_cast<std::size_t>(offset)]=255; check(!w::decode_control(bad,s)); }
    auto refresh=*w::encode_control(w::RefreshRequest{1,relay::MediaReason::none},c);
    for(auto reason:{15,16,255}) { refresh[12]=static_cast<std::uint8_t>(reason); check(!w::decode_control(refresh,c)); }
    check(!w::decode_control(Bytes(1025),c));
    std::vector<std::pair<w::EnvelopeKind,w::RawBody>> raws{
        {w::EnvelopeKind::hello,w::Hello{3}}, {w::EnvelopeKind::welcome,w::Welcome{VideoCodec::hevc,9}},
        {w::EnvelopeKind::confirm,w::Welcome{VideoCodec::hevc,9}}, {w::EnvelopeKind::ready,w::Welcome{VideoCodec::hevc,9}},
        {w::EnvelopeKind::busy,w::Busy{w::BusyReason::no_common_codec}}, {w::EnvelopeKind::challenge,w::Challenge{8}},
        {w::EnvelopeKind::proof,w::Proof{8,2,true,true,9}}, {w::EnvelopeKind::cancel,w::Cancel{2,w::CancelReason::focus}},
        {w::EnvelopeKind::cancel_ack,w::Cancel{2,w::CancelReason::focus}}};
    for(const auto& [kind,raw]:raws) {
        auto b=w::encode_raw(raw); check(b.has_value()); auto v=w::decode_raw(kind,*b); check(v&&w::encode_raw(*v)==b);
        for(std::size_t n=0;n<b->size();++n) check(!w::decode_raw(kind,std::span(*b).first(n)));
        auto extra=*b; extra.push_back(0); check(!w::decode_raw(kind,extra));
        w::Tuple tuple{1,2,3}; if(kind==w::EnvelopeKind::hello||kind==w::EnvelopeKind::busy) tuple={0,2,0};
        check(w::encode_envelope({kind,tuple,*b}).has_value());
    }
    check(*w::encode_raw(w::Hello{3})==Bytes({3,0,0,0}));
    check(!w::encode_raw(w::Hello{0})); check(!w::encode_raw(w::Hello{4}));
    auto proof=*w::encode_raw(w::Proof{1,2,true,true,3});
    for(auto offset:{16,17,23}) { auto bad=proof; bad[static_cast<std::size_t>(offset)]=255; check(!w::decode_raw(w::EnvelopeKind::proof,bad)); }
    check(!w::encode_raw(w::Proof{1,0,true,true,3})); check(!w::encode_raw(w::Proof{1,2,true,true,0}));
}

void paste_control_contract() {
    using D=w::Direction; auto c=D::client_to_server, s=D::server_to_client;
    control(w::PasteBegin{0x0102030405060708,65536,0x10203040},c,8,16);
    control(w::PasteChunk{1,2,{0xaa,0xbb}},c,9,18);
    control(w::PasteCommit{1},c,10,8);
    control(w::PasteCancel{1,w::PasteCancelReason::disconnect},c,11,16);
    control(w::PasteStatus{1,w::PasteState::executing,2,960,10,w::PasteStatusReason::proof},s,12,28);
    control(w::PasteKeepalive{1},c,13,8);
    auto begin=*w::encode_control(w::PasteBegin{0x0102030405060708,65536,0x10203040},c);
    check(begin==Bytes({8,0,0,16,1,2,3,4,5,6,7,8,0,1,0,0,0x10,0x20,0x30,0x40}));
    auto status=*w::encode_control(w::PasteStatus{1,w::PasteState::completed,2,960,960,w::PasteStatusReason::none},s);
    check(status==Bytes({12,0,0,28,0,0,0,0,0,0,0,1,4,0,0,0,0,0,0,2,0,0,3,0xc0,0,0,3,0xc0,0,0,0,0}));
    Bytes max(960,0x5a); auto chunk=w::encode_control(w::PasteChunk{1,0,max},c); check(chunk&&chunk->size()==980);
    check(!w::encode_control(w::PasteBegin{0,1,0},c)); check(!w::encode_control(w::PasteBegin{1,0,0},c)); check(!w::encode_control(w::PasteBegin{1,65537,0},c));
    check(!w::encode_control(w::PasteChunk{1,0,{}},c)); check(!w::encode_control(w::PasteChunk{1,0,Bytes(961)},c));
    check(!w::encode_control(w::PasteCommit{0},c)); check(!w::encode_control(w::PasteCancel{0,w::PasteCancelReason::user},c));
    auto bad=*chunk; bad[18]=1; check(!w::decode_control(bad,c)); // chunk reserved
    bad=*chunk; bad[2]=3; bad[3]=0xbf; check(!w::decode_control(bad,c)); // payload size mismatch
    bad=*w::encode_control(w::PasteStatus{1,w::PasteState::uploading,0,0,0,w::PasteStatusReason::none},s); bad[13]=1; check(!w::decode_control(bad,s));
    for(auto n:{std::size_t(0),std::size_t(1),std::size_t(7),std::size_t(15),std::size_t(16),std::size_t(17),chunk->size()-1}) check(!w::decode_control(std::span(*chunk).first(n),c));
    Bytes oversized(1025); check(!w::decode_control(oversized,c));
}
void v3_envelopes_are_rejected() {
    Bytes body(1,0); w::Tuple tuple{1,2,3};
    auto b=*w::encode_envelope({w::EnvelopeKind::kcp,tuple,body}); b[4]=3; check(!w::decode_envelope(b));
}

void desired_state_matches_serial_domain() {
    DesiredInputState s; s.keys={4,0,0x73,0x7f,0x85,0x8f}; s.buttons=7; s.absolute_x=s.absolute_y=4095;
    check(w::valid_state(s)); s.keys[1]=4; check(!w::valid_state(s)); s.keys[1]=0xe0; check(!w::valid_state(s));
    s.keys[1]=0; s.absolute_x=4096; check(!w::valid_state(s)); s.absolute_x=4095; s.buttons=8; check(!w::valid_state(s));
    for(unsigned usage=1;usage<256;++usage) { s={}; s.keys[0]=static_cast<std::uint8_t>(usage); bool supported=(usage>=4&&usage<=0x73)||(usage>=0x7f&&usage<=0x82)||(usage>=0x85&&usage<=0x87)||(usage>=0x89&&usage<=0x8f); check(w::valid_state(s)==supported); }
}
void edge_binary64_roundtrip() {
    using D=w::Direction; auto d=D::client_to_server;
    double precise=0.123456789012345; std::vector<ControlPayload> payloads{AbsoluteMotion{precise,-0.0},RelativeMotion{-0.125,precise},ButtonEdge{2,true,precise,1},VerticalWheel{-0.5,precise,0}};
    for(auto& payload:payloads) { w::Edge e{1,2,3,4,5,payload}; auto b=w::encode_control(e,d); check(b.has_value()); auto v=w::decode_control(*b,d); check(v&&w::encode_control(*v,d)==b); }
    auto encoded=*w::encode_control(w::Edge{1,2,3,3,4,AbsoluteMotion{precise,-0.0}},d);
    auto decoded=std::get<AbsoluteMotion>(std::get<w::Edge>(*w::decode_control(encoded,d)).payload);
    check(std::bit_cast<std::uint64_t>(decoded.x)==std::bit_cast<std::uint64_t>(precise)); check(std::signbit(decoded.y));
    for(auto bad:{std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) check(!w::encode_control(w::Edge{1,2,3,3,4,RelativeMotion{bad,0}},d));
    check(!w::encode_control(w::Edge{1,2,3,3,4,AbsoluteMotion{1.1,0}},d));
    check(!w::encode_control(w::Edge{1,2,3,3,4,KeyEdge{0,true}},d)); check(!w::encode_control(w::Edge{1,2,3,3,4,ButtonEdge{3,true,0,0}},d));
    auto key=*w::encode_control(w::Edge{1,2,3,3,4,KeyEdge{0xe7,true}},d); key.back()=2; check(!w::decode_control(key,d));
    encoded[37]=0x7f; encoded[38]=0xf0; std::fill(encoded.begin()+39,encoded.begin()+45,0); check(!w::decode_control(encoded,d));
}
int main() {
    envelope_golden_offsets_and_1200_ceiling(); control_golden_vectors_and_exact_lengths();
    desired_state_matches_serial_domain(); edge_binary64_roundtrip(); paste_control_contract(); v3_envelopes_are_rejected();
    std::cout<<"relay_wire: all binary/domain checks passed\n";
}
