#include "network/relay_session.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace kvmux::relay {
namespace {
using namespace std::chrono_literals;
using wire::EnvelopeKind;
void action(SessionActions& out, SessionAction::Kind kind) {
    if(out.size==out.items.size()) throw std::logic_error("session action bound");
    out.items[out.size++].kind=kind;
}
void send(SessionActions& out,const udp::Endpoint& peer,EnvelopeKind kind,
          wire::Tuple tuple,const wire::RawBody& raw) {
    auto body=wire::encode_raw(raw);
    if(!body) return;
    auto bytes=wire::encode_envelope({kind,tuple,*body});
    if(!bytes) return;
    action(out,SessionAction::Kind::send_raw);
    auto& a=out.items[out.size-1]; a.peer=peer; a.datagram=std::move(*bytes);
}
bool ready(const ControlSnapshot& s) {
    return s.epoch!=0&&s.state==ControlConnectionState::ready&&s.target_usb_ready&&s.release_confirmed;
}
bool same_sync(const wire::Sync& a,const wire::Sync& b) {
    return a.epoch==b.epoch&&a.intent==b.intent&&a.revision==b.revision&&a.edge_floor==b.edge_floor&&wire::same_state(a.state,b.state);
}
}
struct ServerSession::Impl {
    VideoCodec codec;
    SessionPhase phase{SessionPhase::idle};
    udp::Endpoint peer;
    wire::Tuple tuple;
    wire::Welcome welcome;
    SessionTime pending_deadline{}, session_deadline{}, next_challenge{};
    struct Issued { std::uint64_t id{}; SessionTime at{}; };
    std::array<Issued,200> challenges{};
    std::size_t count{}, head{};
    std::uint64_t next_id{}, latest_proof{}, intent{}, canceled{}, revision{}, last_edge{};
    std::optional<SessionTime> execution;
    ControlSnapshot snapshot;
    std::optional<wire::Sync> pending_sync, completed_sync;
    bool barrier{};
    explicit Impl(VideoCodec c):codec(c) {
        if(c!=VideoCodec::mjpeg&&c!=VideoCodec::hevc) throw std::invalid_argument("session codec");
    }
    void revoke(SessionActions& out) {
        if(execution||barrier||pending_sync) action(out,SessionAction::Kind::revoke_input);
        execution.reset(); barrier=false; pending_sync.reset(); completed_sync.reset();
    }
    void expire(SessionActions& out) {
        revoke(out); phase=SessionPhase::idle; action(out,SessionAction::Kind::expired);
        tuple={}; count=head=0;
    }
    void deadlines(SessionActions& out,SessionTime now) {
        if(phase==SessionPhase::pending&&now>=pending_deadline) expire(out);
        else if(phase==SessionPhase::established) {
            if(now>=session_deadline) expire(out);
            else if(execution&&now>=*execution) revoke(out);
        }
    }
    std::optional<SessionTime> issued(std::uint64_t id) const {
        for(std::size_t i=0;i<count;++i) if(challenges[i].id==id) return challenges[i].at;
        return {};
    }
    bool eligible(std::uint64_t epoch,std::uint64_t gen,std::uint64_t challenge,SessionTime now) const {
        auto time=issued(challenge);
        return phase==SessionPhase::established&&now<session_deadline&&execution&&now<*execution&&
            ready(snapshot)&&epoch==snapshot.epoch&&gen==intent&&gen>canceled&&time&&now>=*time&&now<*time+250ms;
    }
    void proof(const wire::Proof& p,SessionTime now,bool video_valid,SessionActions& out) {
        auto time=issued(p.challenge);
        if(!time||now<*time||now>=*time+10s||p.challenge<=latest_proof) return;
        latest_proof=p.challenge;
        session_deadline=std::max(session_deadline,*time+10s);
        if(p.intent<=canceled||p.intent<intent) return;
        if(p.intent>intent) {
            revoke(out); intent=p.intent; revision=last_edge=0;
        }
        if(!p.active||!p.video_fresh||!video_valid) { revoke(out); return; }
        if(!ready(snapshot)||now>=*time+250ms) return;
        auto deadline=*time+250ms;
        if(!execution||deadline>*execution) {
            execution=deadline; action(out,SessionAction::Kind::lease_changed);
        }
    }
    void cancellation(const wire::Cancel& c,SessionTime now,SessionActions& out) {
        deadlines(out,now);
        if(phase!=SessionPhase::established||!wire::encode_raw(c)) return;
        if(c.intent>canceled) {
            canceled=c.intent;
            if(c.intent>=intent) revoke(out);
        }
        send(out,peer,EnvelopeKind::cancel_ack,tuple,c);
        // An old-generation disconnect cannot close a newer active controller.
        if(c.reason==wire::CancelReason::disconnect&&c.intent>=intent) expire(out);
    }
};
ServerSession::ServerSession(VideoCodec c):impl_(std::make_unique<Impl>(c)) {}
ServerSession::~ServerSession()=default;
SessionActions ServerSession::on_datagram(const udp::Endpoint& peer,std::span<const std::uint8_t> bytes,
                                         SessionTime now,SessionIds ids,bool video_valid) {
    auto& s=*impl_; SessionActions out; s.deadlines(out,now);
    auto e=wire::decode_envelope(bytes); if(!e) return out;
    if(e->kind==EnvelopeKind::hello) {
        auto hello=std::get<wire::Hello>(*wire::decode_raw(e->kind,e->body));
        if(s.phase!=SessionPhase::idle) {
            if(s.phase==SessionPhase::pending&&peer==s.peer&&e->tuple.nonce==s.tuple.nonce)
                send(out,s.peer,EnvelopeKind::welcome,s.tuple,s.welcome);
            else send(out,peer,EnvelopeKind::busy,e->tuple,wire::Busy{});
            return out;
        }
        auto bit=s.codec==VideoCodec::mjpeg?1:2;
        if(!(hello.codecs&bit)) { send(out,peer,EnvelopeKind::busy,e->tuple,wire::Busy{wire::BusyReason::no_common_codec}); return out; }
        if(!ids.session||!ids.conversation||!ids.generation) return out;
        s.phase=SessionPhase::pending; s.peer=peer; s.tuple={ids.session,e->tuple.nonce,ids.conversation};
        s.welcome={s.codec,ids.generation}; s.pending_deadline=now+2s;
        s.count=s.head=0; s.next_id=s.latest_proof=s.intent=s.canceled=s.revision=s.last_edge=0;
        s.execution.reset(); s.barrier=false; s.pending_sync.reset(); s.completed_sync.reset();
        send(out,peer,EnvelopeKind::welcome,s.tuple,s.welcome); return out;
    }
    if(s.phase==SessionPhase::idle||!(s.peer==peer)||!(s.tuple==e->tuple)) return out;
    if(e->kind==EnvelopeKind::confirm) {
        if(std::get<wire::Welcome>(*wire::decode_raw(e->kind,e->body))!=s.welcome) return out;
        if(s.phase==SessionPhase::pending) {
            s.phase=SessionPhase::established; s.session_deadline=now+10s; s.next_challenge=now;
            action(out,SessionAction::Kind::established);
        }
        send(out,peer,EnvelopeKind::ready,s.tuple,s.welcome); return out;
    }
    if(s.phase!=SessionPhase::established) return out;
    if(e->kind==EnvelopeKind::proof) s.proof(std::get<wire::Proof>(*wire::decode_raw(e->kind,e->body)),now,video_valid,out);
    else if(e->kind==EnvelopeKind::cancel) s.cancellation(std::get<wire::Cancel>(*wire::decode_raw(e->kind,e->body)),now,out);
    return out;
}
SessionActions ServerSession::tick(SessionTime now) {
    auto& s=*impl_; SessionActions out; s.deadlines(out,now);
    if(s.phase!=SessionPhase::established||now<s.next_challenge) return out;
    if(s.next_id==std::numeric_limits<std::uint64_t>::max()) { s.expire(out); return out; }
    auto id=++s.next_id; s.challenges[s.head]={id,now}; s.head=(s.head+1)%s.challenges.size(); s.count=std::min(s.count+1,s.challenges.size());
    s.next_challenge=now+50ms; send(out,s.peer,EnvelopeKind::challenge,s.tuple,wire::Challenge{id}); return out;
}
SessionActions ServerSession::update_control_snapshot(const ControlSnapshot& snapshot,SessionTime now) {
    auto& s=*impl_; SessionActions out; s.deadlines(out,now);
    if(snapshot.epoch!=s.snapshot.epoch||!ready(snapshot)) s.revoke(out);
    s.snapshot=snapshot;
    if(s.phase!=SessionPhase::established||!s.pending_sync) return out;
    const auto sync=*s.pending_sync; const auto& a=snapshot.applied;
    // Challenge age gates admission, not serial completion. A newer valid proof
    // may keep this immutable transaction eligible while both chip ACKs finish.
    if(!s.execution||now>=*s.execution||!ready(snapshot)||snapshot.epoch!=sync.epoch||
       sync.intent!=s.intent||sync.intent<=s.canceled||!a.known||a.epoch!=sync.epoch||
       a.intent_generation!=sync.intent||a.revision!=sync.revision||!wire::same_state(a.state,sync.state)) return out;
    s.barrier=true; s.completed_sync=sync; s.pending_sync.reset(); s.last_edge=sync.edge_floor;
    action(out,SessionAction::Kind::state_ack);
    out.items[out.size-1].ack=wire::StateAck{sync.epoch,sync.intent,sync.revision,sync.edge_floor,sync.state};
    return out;
}
SessionActions ServerSession::cancel(const wire::Cancel& c,SessionTime now) {
    SessionActions out; impl_->cancellation(c,now,out); return out;
}
bool ServerSession::matches(const udp::Endpoint& peer,const wire::Tuple& tuple) const {
    return impl_->phase==SessionPhase::established&&impl_->peer==peer&&impl_->tuple==tuple;
}
InputGate ServerSession::check_sync(const wire::Sync& sync,SessionTime now) const {
    const auto& s=*impl_;
    if(!wire::encode_control(sync,wire::Direction::client_to_server)||!s.eligible(sync.epoch,sync.intent,sync.challenge,now)) return InputGate::rejected;
    if((s.pending_sync&&same_sync(*s.pending_sync,sync))||(s.completed_sync&&same_sync(*s.completed_sync,sync))) return InputGate::duplicate;
    if(s.pending_sync||sync.revision<=s.revision||sync.edge_floor<s.last_edge) return InputGate::rejected;
    return InputGate::allowed;
}
SessionActions ServerSession::sync_submitted(const wire::Sync& sync,kvmux::SubmitResult result,SessionTime now) {
    auto& s=*impl_; SessionActions out; s.deadlines(out,now);
    if(check_sync(sync,now)!=InputGate::allowed) return out;
    if(result!=kvmux::SubmitResult::accepted) { s.revoke(out); return out; }
    s.pending_sync=sync; s.completed_sync.reset(); s.barrier=false; s.revision=sync.revision;
    return out;
}
InputGate ServerSession::check_edge(const wire::Edge& edge,SessionTime now) const {
    const auto& s=*impl_;
    if(!wire::encode_control(edge,wire::Direction::client_to_server)||!s.eligible(edge.epoch,edge.intent,edge.challenge,now)||!s.barrier) return InputGate::rejected;
    if(edge.sequence<=s.last_edge) return InputGate::duplicate;
    if(s.last_edge==std::numeric_limits<std::uint64_t>::max()||edge.sequence!=s.last_edge+1) return InputGate::recovery_required;
    return InputGate::allowed;
}
SessionActions ServerSession::edge_submitted(const wire::Edge& edge,kvmux::SubmitResult result,SessionTime now) {
    auto& s=*impl_; SessionActions out; s.deadlines(out,now); auto gate=check_edge(edge,now);
    if(gate==InputGate::recovery_required||(gate==InputGate::allowed&&result!=kvmux::SubmitResult::accepted)) s.revoke(out);
    else if(gate==InputGate::allowed) s.last_edge=edge.sequence;
    return out;
}
SessionPhase ServerSession::phase() const { return impl_->phase; }
wire::Tuple ServerSession::tuple() const { return impl_->tuple; }
wire::Welcome ServerSession::welcome() const { return impl_->welcome; }
bool ServerSession::barrier_complete() const { return impl_->barrier; }
std::uint64_t ServerSession::canceled_through() const { return impl_->canceled; }
std::optional<SessionTime> ServerSession::execution_deadline() const { return impl_->execution; }
std::size_t ServerSession::challenge_count() const { return impl_->count; }

struct ClientSession::Impl {
    udp::Endpoint peer;
    std::uint8_t codecs;
    SessionPhase phase{SessionPhase::idle};
    wire::Tuple tuple;
    wire::Welcome welcome;
    SessionTime attempt_deadline{}, session_deadline{}, retry{}, cancel_retry{}, cancel_deadline{};
    std::uint64_t challenge{}, last_nonce{}, intent{}, canceled{};
    bool active{}, video_fresh{};
    std::uint64_t presented{};
    std::optional<wire::Cancel> cancellation;
    Impl(udp::Endpoint p,std::uint8_t c):peer(std::move(p)),codecs(c) {
        if(!c||c>3) throw std::invalid_argument("supported codecs");
    }
    void close(SessionActions& out) {
        phase=SessionPhase::idle; active=video_fresh=false; cancellation.reset(); tuple={}; challenge=0;
        action(out,SessionAction::Kind::expired);
    }
    void deadlines(SessionActions& out,SessionTime now) {
        if((phase==SessionPhase::pending&&now>=attempt_deadline)||
           (phase==SessionPhase::established&&now>=session_deadline)||
           (cancellation&&cancellation->reason==wire::CancelReason::disconnect&&now>=cancel_deadline)) close(out);
    }
    void handshake(SessionActions& out) {
        if(tuple.session) send(out,peer,EnvelopeKind::confirm,tuple,welcome);
        else send(out,peer,EnvelopeKind::hello,tuple,wire::Hello{codecs});
    }
};
ClientSession::ClientSession(udp::Endpoint peer,std::uint8_t codecs):impl_(std::make_unique<Impl>(std::move(peer),codecs)) {}
ClientSession::~ClientSession()=default;
SessionActions ClientSession::start(std::uint64_t nonce,SessionTime now) {
    auto& s=*impl_; SessionActions out;
    if(!nonce||nonce==s.last_nonce||s.phase!=SessionPhase::idle) return out;
    s.last_nonce=nonce; s.tuple={0,nonce,0}; s.welcome={}; s.challenge=0; s.phase=SessionPhase::pending;
    s.intent=s.canceled=0; s.active=s.video_fresh=false; s.presented=0; s.cancellation.reset();
    s.attempt_deadline=now+2s; s.retry=now+100ms; s.handshake(out); return out;
}
SessionActions ClientSession::on_datagram(const udp::Endpoint& peer,std::span<const std::uint8_t> bytes,SessionTime now) {
    auto& s=*impl_; SessionActions out; s.deadlines(out,now);
    auto e=wire::decode_envelope(bytes);
    if(!e||s.phase==SessionPhase::idle||!(peer==s.peer)||e->tuple.nonce!=s.tuple.nonce) return out;
    if(e->kind==EnvelopeKind::busy&&s.phase==SessionPhase::pending&&!s.tuple.session) {
        auto reason=std::get<wire::Busy>(*wire::decode_raw(e->kind,e->body)).reason;
        action(out,SessionAction::Kind::rejected); out.items[out.size-1].rejection=reason; s.close(out); return out;
    }
    if(e->kind==EnvelopeKind::welcome&&s.phase==SessionPhase::pending) {
        auto welcome=std::get<wire::Welcome>(*wire::decode_raw(e->kind,e->body));
        auto bit=welcome.codec==VideoCodec::mjpeg?1:2;
        if(!(s.codecs&bit)||(s.tuple.session&&(s.tuple!=e->tuple||s.welcome!=welcome))) return out;
        s.tuple=e->tuple; s.welcome=welcome; s.retry=now+100ms; s.handshake(out); return out;
    }
    if(e->tuple!=s.tuple||!s.tuple.session) return out;
    if(e->kind==EnvelopeKind::ready&&s.phase==SessionPhase::pending) {
        if(std::get<wire::Welcome>(*wire::decode_raw(e->kind,e->body))!=s.welcome) return out;
        s.phase=SessionPhase::established; s.session_deadline=now+10s; action(out,SessionAction::Kind::established); return out;
    }
    if(s.phase!=SessionPhase::established) return out;
    if(e->kind==EnvelopeKind::challenge) {
        auto id=std::get<wire::Challenge>(*wire::decode_raw(e->kind,e->body)).id;
        if(id<=s.challenge) return out;
        s.challenge=id; s.session_deadline=now+10s;
        send(out,s.peer,EnvelopeKind::proof,s.tuple,wire::Proof{id,s.intent,s.active,s.video_fresh,s.presented});
    } else if(e->kind==EnvelopeKind::cancel_ack&&s.cancellation) {
        auto c=std::get<wire::Cancel>(*wire::decode_raw(e->kind,e->body));
        if(c.intent==s.cancellation->intent&&c.reason==s.cancellation->reason) {
            s.cancellation.reset();
            if(c.reason==wire::CancelReason::disconnect) s.close(out);
        }
    }
    return out;
}
SessionActions ClientSession::tick(SessionTime now) {
    auto& s=*impl_; SessionActions out; s.deadlines(out,now);
    if(s.phase==SessionPhase::pending&&now>=s.retry) { s.handshake(out); s.retry=now+100ms; }
    if(s.phase==SessionPhase::established&&s.cancellation&&now>=s.cancel_retry) {
        send(out,s.peer,EnvelopeKind::cancel,s.tuple,*s.cancellation); s.cancel_retry=now+50ms;
    }
    return out;
}
void ClientSession::set_intent(std::uint64_t generation,bool active,bool video_fresh,std::uint64_t sequence) {
    auto& s=*impl_; if(generation<s.intent||(active&&(!generation||generation<=s.canceled))) return;
    s.intent=generation; s.active=active; s.video_fresh=video_fresh&&sequence!=0; s.presented=sequence;
}
SessionActions ClientSession::cancel(wire::CancelReason reason,SessionTime now) {
    auto& s=*impl_; SessionActions out; s.deadlines(out,now);
    wire::Cancel c{s.intent,reason};
    if(s.phase!=SessionPhase::established||!wire::encode_raw(c)) return out;
    s.active=false; s.video_fresh=false; s.canceled=std::max(s.canceled,s.intent);
    s.cancellation=c; s.cancel_retry=now+50ms; s.cancel_deadline=now+2s;
    send(out,s.peer,EnvelopeKind::cancel,s.tuple,c); return out;
}
std::optional<wire::Cancel> ClientSession::pending_cancel() const { return impl_->cancellation; }
SessionPhase ClientSession::phase() const { return impl_->phase; }
wire::Tuple ClientSession::tuple() const { return impl_->tuple; }
wire::Welcome ClientSession::welcome() const { return impl_->welcome; }
std::uint64_t ClientSession::latest_challenge() const { return impl_->challenge; }
} // namespace kvmux::relay
