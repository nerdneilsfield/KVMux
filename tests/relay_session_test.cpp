#include "network/relay_session.hpp"
#include "support/crc32.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
using namespace std::chrono_literals;
using namespace kvmux;
using namespace kvmux::relay;
namespace w=kvmux::relay::wire;
using Kind=SessionAction::Kind;
void check(bool ok,std::source_location loc=std::source_location::current()) {
    if(!ok) { std::cerr<<"session check failed at "<<loc.line()<<'\n'; std::abort(); }
}
SessionTime at(int ms) { return SessionTime{}+std::chrono::milliseconds(ms); }
std::size_t count(const SessionActions& a,Kind k) { std::size_t n=0; check(a.size<=8); for(auto& v:a) if(v.kind==k)++n; return n; }
std::vector<std::uint8_t> packet(const SessionActions& a,w::EnvelopeKind kind) {
    for(auto& v:a) if(v.kind==Kind::send_raw&&w::decode_envelope(v.datagram)->kind==kind) return v.datagram;
    check(false); return {};
}
std::vector<std::uint8_t> raw(w::EnvelopeKind kind,w::Tuple tuple,w::RawBody body) {
    auto b=w::encode_raw(body); check(b.has_value()); auto e=w::encode_envelope({kind,tuple,*b}); check(e.has_value()); return *e;
}
struct Fixture {
    udp::Endpoint client,server,foreign;
    ServerSession s{VideoCodec::hevc};
    ClientSession c;
    ControlSnapshot snapshot;
    static udp::Endpoint endpoint(std::uint16_t port) { std::string error; auto p=udp::resolve("127.0.0.1",port,error); check(p.has_value()); return *p; }
    Fixture():client(endpoint(23001)),server(endpoint(23002)),foreign(endpoint(23003)),c(server) {
        snapshot.state=ControlConnectionState::ready; snapshot.epoch=7; snapshot.target_usb_ready=true; snapshot.release_confirmed=true;
        s.update_control_snapshot(snapshot,at(0));
    }
    void establish() {
        auto hello=packet(c.start(11,at(0)),w::EnvelopeKind::hello);
        auto welcome=packet(s.on_datagram(client,hello,at(0),{22,33,44}),w::EnvelopeKind::welcome);
        auto confirm=packet(c.on_datagram(server,welcome,at(0)),w::EnvelopeKind::confirm);
        auto ready=s.on_datagram(client,confirm,at(0)); check(count(ready,Kind::established)==1);
        check(count(c.on_datagram(server,packet(ready,w::EnvelopeKind::ready),at(0)),Kind::established)==1);
        check(c.tuple()==s.tuple()&&c.welcome().generation==33&&c.welcome().codec==VideoCodec::hevc);
        c.set_intent(1,true,true,100);
    }
    SessionActions proof(int issued,int received,bool valid=true) {
        auto challenge=packet(s.tick(at(issued)),w::EnvelopeKind::challenge);
        auto p=packet(c.on_datagram(server,challenge,at(received)),w::EnvelopeKind::proof);
        return s.on_datagram(client,p,at(received),{},valid);
    }
    w::Sync sync(std::uint64_t rev=1,std::uint64_t floor=0) {
        DesiredInputState state; state.keys={4,0,5,0,0,0}; state.buttons=1; state.mode=MouseMode::relative;
        return {snapshot.epoch,1,rev,c.latest_challenge(),floor,state};
    }
    void apply(const w::Sync& request,int now) {
        snapshot.applied={true,request.epoch,request.intent,request.revision,request.state};
        auto actions=s.update_control_snapshot(snapshot,at(now)); check(count(actions,Kind::state_ack)==1);
        for(const auto& a:actions) if(a.ack) check(a.ack->revision==request.revision&&a.ack->edge_floor==request.edge_floor&&w::same_state(a.ack->state,request.state));
    }
    void barrier(int now=0) {
        auto request=sync(); check(s.check_sync(request,at(now))==InputGate::allowed);
        check(s.sync_submitted(request,kvmux::SubmitResult::accepted,at(now)).size==0);
        apply(request,now); check(s.barrier_complete());
    }
};
void handshake_loss_duplicate_and_single_controller() {
    Fixture f;
    auto hello=packet(f.c.start(11,at(0)),w::EnvelopeKind::hello);
    auto welcome=packet(f.s.on_datagram(f.client,hello,at(0),{22,33,44}),w::EnvelopeKind::welcome);
    check(f.s.phase()==SessionPhase::pending);
    check(packet(f.c.tick(at(100)),w::EnvelopeKind::hello)==hello);
    check(packet(f.s.on_datagram(f.client,hello,at(100),{99,99,99}),w::EnvelopeKind::welcome)==welcome);
    auto busy=f.s.on_datagram(f.foreign,hello,at(100)); check(w::decode_envelope(packet(busy,w::EnvelopeKind::busy))->tuple.session==0);
    check(f.c.on_datagram(f.foreign,welcome,at(100)).size==0);
    auto wrong=welcome; wrong[23]=12; check(f.c.on_datagram(f.server,wrong,at(100)).size==0);
    auto confirm=packet(f.c.on_datagram(f.server,welcome,at(100)),w::EnvelopeKind::confirm);
    auto ready=packet(f.s.on_datagram(f.client,confirm,at(100)),w::EnvelopeKind::ready);
    check(f.c.phase()==SessionPhase::pending); // Lost ready: only confirm retries.
    check(packet(f.c.tick(at(200)),w::EnvelopeKind::confirm)==confirm);
    check(packet(f.s.on_datagram(f.client,confirm,at(200)),w::EnvelopeKind::ready)==ready);
    f.c.on_datagram(f.server,ready,at(200)); check(f.c.phase()==SessionPhase::established);
    check(count(f.s.on_datagram(f.client,hello,at(200)),Kind::send_raw)==1); check(f.s.tuple().session==22);
    check(f.s.on_datagram(f.foreign,confirm,at(200)).size==0);
    check(count(f.s.tick(at(10100)),Kind::expired)==1); // Duplicate confirm did not renew.
    Fixture pending; auto h=packet(pending.c.start(41,at(0)),w::EnvelopeKind::hello);
    auto old=packet(pending.s.on_datagram(pending.client,h,at(0),{1,2,3}),w::EnvelopeKind::welcome);
    pending.s.on_datagram(pending.client,h,at(1999)); check(count(pending.s.tick(at(2000)),Kind::expired)==1);
    check(count(pending.c.tick(at(2000)),Kind::expired)==1); check(pending.c.start(41,at(2000)).size==0);
    pending.c.start(42,at(2000)); check(pending.c.on_datagram(pending.server,old,at(2000)).size==0);
    ServerSession mismatch(VideoCodec::hevc); auto single=raw(w::EnvelopeKind::hello,{0,1,0},w::Hello{1});
    auto reject=packet(mismatch.on_datagram(f.client,single,at(0),{1,2,3}),w::EnvelopeKind::busy);
    check(std::get<w::Busy>(*w::decode_raw(w::EnvelopeKind::busy,w::decode_envelope(reject)->body)).reason==w::BusyReason::no_common_codec);
    check(mismatch.phase()==SessionPhase::idle);
}
void server_issue_time_not_receipt_lease() {
    Fixture f; f.establish(); check(count(f.proof(0,249),Kind::lease_changed)==1);
    check(f.s.execution_deadline()==at(250)); f.barrier(249);
    check(count(f.s.tick(at(250)),Kind::revoke_input)==1); check(!f.s.barrier_complete());
    check(count(f.s.tick(at(251)),Kind::revoke_input)==0);
    auto old=w::Edge{7,1,1,1,1,RelativeMotion{0.25,-0.5}};
    f.proof(300,300); check(f.s.check_edge(old,at(300))==InputGate::rejected);
    auto request=f.sync(2); f.s.sync_submitted(request,kvmux::SubmitResult::accepted,at(300)); f.apply(request,300);
    check(f.s.check_edge(old,at(300))==InputGate::rejected);
    Fixture boundary; boundary.establish(); check(count(boundary.proof(0,250),Kind::lease_changed)==0); check(!boundary.s.execution_deadline());
    Fixture unvalidated; unvalidated.establish(); unvalidated.proof(0,0,false); check(!unvalidated.s.execution_deadline());
    auto proof=raw(w::EnvelopeKind::proof,f.s.tuple(),w::Proof{1,1,true,true,100});
    check(f.s.on_datagram(f.client,proof,at(301),{},true).size==0);
    check(f.s.execution_deadline()==at(550));
}
void session_10s_input_250ms_separation() {
    for(int blackout:{100,300,800,2000}) {
        Fixture f; f.establish(); f.proof(0,0); f.barrier(); auto tuple=f.s.tuple();
        auto a=f.s.tick(at(blackout)); check(f.s.phase()==SessionPhase::established&&f.s.tuple()==tuple);
        check(count(a,Kind::revoke_input)==(blackout>=250?1U:0U));
        auto challenge=packet(a,w::EnvelopeKind::challenge); auto p=packet(f.c.on_datagram(f.server,challenge,at(blackout)),w::EnvelopeKind::proof);
        f.s.on_datagram(f.client,p,at(blackout),{},true); check(f.s.execution_deadline()==at(blackout+250));
    }
    Fixture f; f.establish(); f.proof(0,0);
    auto tuple=f.s.tuple(); std::vector<std::uint8_t> body(40,0);
    auto media=*w::encode_envelope({w::EnvelopeKind::media,tuple,body});
    for(int t:{1000,5000,9999}) { f.s.on_datagram(f.client,media,at(t)); f.c.on_datagram(f.server,media,at(t)); }
    check(count(f.s.tick(at(10000)),Kind::expired)==1); check(count(f.c.tick(at(10000)),Kind::expired)==1);
}
void cancellation_overtakes_kcp_and_tombstones() {
    Fixture f; f.establish(); f.proof(0,0); f.barrier(); auto old=f.sync(2);
    auto cancel=f.c.cancel(w::CancelReason::focus,at(10)); check(f.c.pending_cancel().has_value());
    auto body=packet(cancel,w::EnvelopeKind::cancel);
    auto first=f.s.on_datagram(f.client,body,at(10)); check(count(first,Kind::revoke_input)==1&&f.s.canceled_through()==1);
    check(count(f.s.on_datagram(f.client,body,at(11)),Kind::revoke_input)==0);
    check(f.s.check_sync(old,at(11))==InputGate::rejected);
    check(f.s.check_edge({7,1,1,1,1,ButtonEdge{0,true,0,0}},at(11))==InputGate::rejected);
    check(packet(f.c.tick(at(60)),w::EnvelopeKind::cancel)==body);
    f.c.on_datagram(f.server,packet(first,w::EnvelopeKind::cancel_ack),at(60)); check(!f.c.pending_cancel());
    f.c.set_intent(1,true,true,100); f.proof(100,100); check(!f.s.execution_deadline());
    f.c.set_intent(2,true,true,101); f.proof(150,150); check(f.s.execution_deadline()&&!f.s.barrier_complete());
    auto fresh=f.sync(); fresh.intent=2; f.s.sync_submitted(fresh,kvmux::SubmitResult::accepted,at(150)); f.apply(fresh,150);
    check(count(f.s.cancel({1,w::CancelReason::host},at(151)),Kind::revoke_input)==0&&f.s.barrier_complete());
    f.c.set_intent(3,true,true,102); check(count(f.proof(200,200),Kind::revoke_input)==1); check(!f.s.barrier_complete());
    f.c.set_intent(3,true,false,102); check(count(f.proof(250,250),Kind::revoke_input)==1); check(!f.s.execution_deadline());
    f.c.set_intent(3,true,true,103); f.proof(300,300);
    f.c.set_intent(3,false,true,103); check(count(f.proof(350,350),Kind::revoke_input)==1);
    auto disconnect=f.c.cancel(w::CancelReason::disconnect,at(400));
    f.s.on_datagram(f.client,packet(disconnect,w::EnvelopeKind::cancel),at(400)); check(f.s.phase()==SessionPhase::idle);
    check(count(f.c.tick(at(2400)),Kind::expired)==1); // Lost disconnect ACK bounded locally.
}
void immutable_state_ack_and_barrier() {
    Fixture f; f.establish(); f.proof(0,0); auto request=f.sync();
    check(f.s.check_sync(request,at(0))==InputGate::allowed); check(!f.s.barrier_complete()&&f.s.execution_deadline());
    check(count(f.s.sync_submitted(request,kvmux::SubmitResult::accepted,at(0)),Kind::state_ack)==0);
    check(f.s.check_sync(request,at(1))==InputGate::duplicate);
    auto changed=request; changed.revision=2; check(f.s.check_sync(changed,at(1))==InputGate::rejected);
    f.snapshot.applied={false,7,1,1,request.state}; check(count(f.s.update_control_snapshot(f.snapshot,at(1)),Kind::state_ack)==0);
    f.snapshot.applied.known=true; f.snapshot.applied.revision=2; check(count(f.s.update_control_snapshot(f.snapshot,at(2)),Kind::state_ack)==0);
    f.snapshot.applied.revision=1; f.snapshot.applied.state.buttons=2; check(count(f.s.update_control_snapshot(f.snapshot,at(3)),Kind::state_ack)==0);
    f.apply(request,4); check(f.s.check_sync(request,at(4))==InputGate::duplicate);
    w::Edge edge{7,1,1,1,1,KeyEdge{4,false}};
    check(f.s.check_edge(edge,at(5))==InputGate::allowed); f.s.edge_submitted(edge,kvmux::SubmitResult::accepted,at(5));
    f.snapshot.applied.known=false; f.s.update_control_snapshot(f.snapshot,at(6)); check(f.s.barrier_complete());
    f.snapshot.epoch=8; check(count(f.s.update_control_snapshot(f.snapshot,at(7)),Kind::revoke_input)==1);
    f.snapshot.applied={true,7,1,1,request.state}; check(count(f.s.update_control_snapshot(f.snapshot,at(8)),Kind::state_ack)==0);
    Fixture slow; slow.establish(); slow.proof(0,0); auto pending=slow.sync();
    slow.s.sync_submitted(pending,kvmux::SubmitResult::accepted,at(0));
    slow.proof(200,200); slow.apply(pending,300); // Fresh lease, original admission challenge aged out.
    Fixture expired; expired.establish(); expired.proof(0,0); auto abandoned=expired.sync();
    expired.s.sync_submitted(abandoned,kvmux::SubmitResult::accepted,at(0));
    check(count(expired.s.tick(at(250)),Kind::revoke_input)==1);
    expired.proof(300,300);
    expired.snapshot.applied={true,7,1,1,abandoned.state};
    check(count(expired.s.update_control_snapshot(expired.snapshot,at(301)),Kind::state_ack)==0);
    check(!expired.s.barrier_complete()); // New proof cannot resurrect expired sync.
    Fixture late; late.establish(); late.proof(0,0); auto canceled=late.sync();
    late.s.sync_submitted(canceled,kvmux::SubmitResult::accepted,at(0));
    late.s.cancel({1,w::CancelReason::focus},at(1));
    late.snapshot.applied={true,7,1,1,canceled.state};
    check(count(late.s.update_control_snapshot(late.snapshot,at(2)),Kind::state_ack)==0);
    Fixture rejected; rejected.establish(); rejected.proof(0,0); auto r=rejected.sync();
    rejected.s.sync_submitted(r,kvmux::SubmitResult::not_ready,at(0)); rejected.snapshot.applied={true,7,1,1,r.state};
    check(count(rejected.s.update_control_snapshot(rejected.snapshot,at(1)),Kind::state_ack)==0);
}
void edge_floor_gap_and_no_uncertain_replay() {
    Fixture f; f.establish(); f.proof(0,0); auto sync=f.sync(1,10); f.s.sync_submitted(sync,kvmux::SubmitResult::accepted,at(0)); f.apply(sync,0);
    w::Edge e{7,1,10,10,1,RelativeMotion{0.5,-0.25}}; check(f.s.check_edge(e,at(1))==InputGate::duplicate);
    e.sequence=12; check(f.s.check_edge(e,at(1))==InputGate::recovery_required); check(count(f.s.edge_submitted(e,kvmux::SubmitResult::not_ready,at(1)),Kind::revoke_input)==1);
    f.proof(50,50); sync=f.sync(2,12); f.s.sync_submitted(sync,kvmux::SubmitResult::accepted,at(50)); f.apply(sync,50);
    e.challenge=2; check(f.s.check_edge(e,at(51))==InputGate::duplicate);
    e.sequence=13; e.payload=VerticalWheel{-0.25,0,0}; check(f.s.check_edge(e,at(51))==InputGate::allowed);
    check(count(f.s.edge_submitted(e,kvmux::SubmitResult::overloaded,at(51)),Kind::revoke_input)==1);
    f.proof(100,100); sync=f.sync(3,13); f.s.sync_submitted(sync,kvmux::SubmitResult::accepted,at(100)); f.apply(sync,100);
    e.challenge=3; check(f.s.check_edge(e,at(101))==InputGate::duplicate);
    e.sequence=14; e.payload=ButtonEdge{0,true,0,0}; f.s.edge_submitted(e,kvmux::SubmitResult::accepted,at(101));
    check(f.s.check_edge(e,at(102))==InputGate::duplicate&&f.s.barrier_complete());
}
void challenge_ring_and_actions_bounded() {
    Fixture f; f.establish();
    for(int t=0;t<30000;t+=50) {
        auto a=f.proof(t,t); check(a.size<=8); check(f.s.challenge_count()<=200);
        auto unknown=raw(w::EnvelopeKind::proof,{99,11,44},w::Proof{1,1,true,true,100});
        for(int i=0;i<10;++i) check(f.s.on_datagram(f.foreign,unknown,at(t),{},true).size==0);
    }
    check(f.s.challenge_count()==200);
    auto old=raw(w::EnvelopeKind::proof,f.s.tuple(),w::Proof{1,1,true,true,100});
    check(f.s.on_datagram(f.client,old,at(30000),{},true).size<=1);
    check(f.s.phase()==SessionPhase::established); check(count(f.s.tick(at(39950)),Kind::expired)==1);
}

void paste_upload_contract() {
    Fixture f; f.establish();
    const std::vector<std::uint8_t> text{'a','\r','\n','b'};
    const auto crc=support::crc32_ieee(text); check(crc==0xf35534d7U);
    auto a=f.s.paste_begin({9,4,crc},at(0)); check(a.items[0].paste_status->state==w::PasteState::uploading);
    check(f.s.paste_begin({9,4,crc},at(1)).items[0].paste_status->next_chunk==0);
    check(f.s.paste_begin({10,4,crc},at(1)).items[0].paste_status->reason==w::PasteStatusReason::conflict);
    check(f.s.paste_chunk({9,0,text},at(2)).items[0].paste_status->state==w::PasteState::complete);
    check(f.s.paste_chunk({9,0,text},at(3)).items[0].paste_status->accepted_bytes==4);
    check(f.s.paste_commit({9},at(4)).items[0].paste_status->reason==w::PasteStatusReason::proof);
    f.proof(50,50); f.barrier(50);
    auto done=f.s.paste_commit({9},at(51)); check(done.items[0].paste_status->state==w::PasteState::executing);
    check(f.s.pending_paste_bytes()->size()==4 && f.s.pending_paste_fence()->intent==1);
    check(f.s.check_edge({1,1,1,1,1,KeyEdge{4,true}},at(51))==InputGate::rejected);
    f.s.paste_started(at(51)); check(!f.s.pending_paste_bytes());
    auto progress=f.s.update_ascii_paste({AsciiPasteState::active,2,1},at(52));
    check(progress.items[0].paste_status->state==w::PasteState::executing && progress.items[0].paste_status->completed_bytes==2);
    auto terminal=f.s.update_ascii_paste({AsciiPasteState::completed,2,2},at(53));
    check(terminal.items[0].paste_status->state==w::PasteState::completed && terminal.items[0].paste_status->completed_bytes==4);
    Fixture bad; bad.establish();
    auto b=bad.s.paste_begin({3,2,0},at(0)); check(b.size==1);
    auto rejected=bad.s.paste_chunk({3,0,{'x','\r'}},at(1)); check(rejected.items[0].paste_status->state==w::PasteState::complete);
    auto r=bad.s.paste_commit({3},at(2)); check(r.items[0].paste_status->state==w::PasteState::rejected);
    Fixture expiry; expiry.establish(); expiry.s.paste_begin({4,1,0},at(0)); auto x=expiry.s.tick(at(30000)); check(x.items[0].paste_status->state==w::PasteState::expired);
}

void paste_execution_lease_survives_video_gap_and_expires_without_heartbeat() {
    Fixture f; f.establish();
    const std::vector<std::uint8_t> text{'a'};
    check(f.s.paste_begin({19,1,support::crc32_ieee(text)},at(0)).size==1);
    check(f.s.paste_chunk({19,0,text},at(1)).size==1);
    f.proof(0,0); f.barrier(0);
    check(f.s.paste_commit({19},at(1)).items[0].paste_status->state==w::PasteState::executing);
    f.s.paste_started(at(1));
    // Presentation proof expires at 250ms. The transaction continues while the
    // client GUI/control loop renews its explicit lease.
    check(!f.s.tick(at(251)).items[0].paste_status.has_value());
    check(f.s.paste_keepalive({19},at(400)).size==0);
    check(!f.s.tick(at(800)).items[0].paste_status.has_value());
    auto expired=f.s.tick(at(900));
    check(count(expired, Kind::paste_ready)==1);
    bool canceled = false; for (const auto& action : expired) if (action.paste_status) canceled = action.paste_status->state==w::PasteState::canceled && action.paste_status->reason==w::PasteStatusReason::deadline;
    check(canceled);
    // A delayed renewal cannot revive the canceled transaction or permit input.
    check(!f.s.paste_keepalive({19},at(901)).items[0].paste_status.has_value());
    check(f.s.check_edge({7,1,1,1,1,KeyEdge{4,true}},at(901))==InputGate::rejected);
}

int main() {
    handshake_loss_duplicate_and_single_controller(); server_issue_time_not_receipt_lease();
    session_10s_input_250ms_separation(); cancellation_overtakes_kcp_and_tombstones();
    immutable_state_ack_and_barrier(); edge_floor_gap_and_no_uncertain_replay(); challenge_ring_and_actions_bounded(); paste_upload_contract(); paste_execution_lease_survives_video_gap_and_expires_without_heartbeat();
    std::cout<<"relay_session: deterministic handshake/freshness/barrier checks passed\n";
}
