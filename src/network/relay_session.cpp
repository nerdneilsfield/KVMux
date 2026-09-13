#include "network/relay_session.hpp"
#include "support/crc32.hpp"
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
bool valid_paste_text(std::span<const std::uint8_t> text) {
    if(text.empty()) return false;
    for(std::size_t i=0;i<text.size();++i) {
        const auto c=text[i];
        if(c>=0x80) return false;
        if(c=='\r') { if(i+1>=text.size()||text[++i]!='\n') return false; continue; }
        if(!(c=='\t'||c=='\n'||(c>=0x20&&c<=0x7e))) return false;
    }
    return true;
}
void paste_action(SessionActions& out, const wire::PasteStatus& status) {
    action(out, SessionAction::Kind::paste_ready);
    out.items[out.size-1].paste_status=status;
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
    struct PasteUpload {
        wire::PasteBegin begin;
        std::vector<std::uint8_t> bytes;
        std::uint32_t accepted_bytes{};
        std::uint32_t next{};
        SessionTime deadline{};
        wire::PasteState state{wire::PasteState::uploading};
        std::optional<SessionTime> lease;
        std::uint64_t completion_proof{};
        // Bound at Begin. A paste must never outlive the control owner that created it.
        std::uint64_t owner_epoch{}, owner_intent{}, job_id{};
    };
    std::optional<PasteUpload> paste;
    std::optional<wire::PasteStatus> terminal_paste;
    ControlSnapshot snapshot;
    std::optional<wire::Sync> pending_sync, completed_sync;
    bool barrier{};
    explicit Impl(VideoCodec c):codec(c) {
        if(c!=VideoCodec::mjpeg&&c!=VideoCodec::hevc) throw std::invalid_argument("session codec");
    }
    wire::PasteStatus status(wire::PasteStatusReason reason=wire::PasteStatusReason::none) const {
        if(!paste) return {};
        const auto& p=*paste;
        return {p.begin.transaction_id,p.state,p.next,p.accepted_bytes,0,wire::PasteOutcome::none,reason};
    }
    void clear_paste() { paste.reset(); }
    bool paste_owner_valid() const {
        return paste && ready(snapshot) && paste->owner_epoch == snapshot.epoch &&
            paste->owner_intent == intent && (!paste->owner_intent || paste->owner_intent > canceled);
    }
    void finish_paste(SessionActions& out, wire::PasteStatus status) {
        // Keep exactly one terminal result so delayed controls can only observe it.
        if (status.state == wire::PasteState::finished && status.outcome == wire::PasteOutcome::none) {
            status.outcome = status.reason == wire::PasteStatusReason::canceled ? wire::PasteOutcome::canceled :
                status.reason == wire::PasteStatusReason::deadline ? wire::PasteOutcome::expired : wire::PasteOutcome::rejected;
        }
        terminal_paste = status;
        clear_paste();
        paste_action(out, status);
    }
    bool invalidate_paste(SessionActions& out) {
        if (!paste || paste_owner_valid()) return false;
        auto st=status(wire::PasteStatusReason::canceled); st.state=wire::PasteState::finished;
        finish_paste(out, st); return true;
    }
    void cancel_paste(SessionActions& out, wire::PasteStatusReason reason) {
        if(!paste) return;
        auto st=status(reason); st.state=wire::PasteState::finished;
        finish_paste(out,st);
    }
    void revoke(SessionActions& out) {
        if(paste) cancel_paste(out,wire::PasteStatusReason::canceled);
        if(execution||barrier||pending_sync) action(out,SessionAction::Kind::revoke_input);
        execution.reset(); barrier=false; pending_sync.reset(); completed_sync.reset();
    }
    void expire(SessionActions& out) {
        revoke(out); phase=SessionPhase::idle; action(out,SessionAction::Kind::expired);
        clear_paste(); tuple={}; count=head=0;
    }
    void deadlines(SessionActions& out,SessionTime now) {
        if(paste && paste->state==wire::PasteState::uploading && now>=paste->deadline) { auto st=status(wire::PasteStatusReason::deadline); st.state=wire::PasteState::finished; finish_paste(out,st); }
        if(phase==SessionPhase::pending&&now>=pending_deadline) expire(out);
        else if(phase==SessionPhase::established) {
            if(now>=session_deadline) expire(out);
            else if (paste && (paste->state == wire::PasteState::executing || paste->state == wire::PasteState::preparing) && paste->lease && now >= *paste->lease) {
                if (paste->state == wire::PasteState::executing) cancel_paste(out, wire::PasteStatusReason::deadline);
                else cancel_paste(out, wire::PasteStatusReason::deadline);
            }
            else if(execution&&now>=*execution && (!paste || (paste->state != wire::PasteState::executing && paste->state != wire::PasteState::preparing))) revoke(out);
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
    void proof(const wire::Proof& p, SessionTime now, SessionTime received_at, bool video_valid, SessionActions& out) {
        auto time=issued(p.challenge);
        // Validate a raw proof at packet receipt.  Its effects start at actual
        // processing time because video completion can legitimately defer it.
        if(!time || received_at > now || received_at < *time || received_at >= *time+250ms || p.challenge<=latest_proof) return;
        const bool deferred = received_at != now;
        latest_proof=p.challenge;
        session_deadline=std::max(session_deadline, deferred ? now+10s : *time+10s);
        if(p.intent<=canceled||p.intent<intent) return;
        if(p.intent>intent) {
            // An upload may begin before its first proof. Bind that provisional
            // owner to the first intent, but never transfer an established owner.
            const bool bind_provisional_paste = paste && paste->owner_intent == 0;
            if (!bind_provisional_paste) revoke(out);
            else { execution.reset(); barrier=false; pending_sync.reset(); completed_sync.reset(); }
            intent=p.intent;
            if (bind_provisional_paste) paste->owner_intent=intent;
            revision=last_edge=0;
        }
        if(!p.active||!p.video_fresh||!video_valid||!ready(snapshot)) { revoke(out); return; }
        auto deadline=deferred ? now+250ms : *time+250ms;
        if(!execution||deadline>*execution) {
            execution=deadline; action(out,SessionAction::Kind::lease_changed);
        }
    }
    void cancellation(const wire::Cancel& c,SessionTime now,SessionActions& out) {
        deadlines(out,now);
        if(phase!=SessionPhase::established||!wire::encode_raw(c)) return;
        // An old cancel acknowledges itself, but cannot cancel a newer paste.
        if (paste && c.intent == paste->owner_intent) cancel_paste(out,wire::PasteStatusReason::canceled);
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
                                         SessionTime now,SessionIds ids,bool video_valid,
                                         std::optional<SessionTime> proof_received_at) {
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
        s.clear_paste(); s.terminal_paste.reset(); s.execution.reset(); s.barrier=false; s.pending_sync.reset(); s.completed_sync.reset();
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
    if(e->kind==EnvelopeKind::proof) s.proof(std::get<wire::Proof>(*wire::decode_raw(e->kind,e->body)),now,proof_received_at.value_or(now),video_valid,out);
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
SessionActions ServerSession::paste_begin(const wire::PasteBegin& begin, SessionTime now) {
    auto& s=*impl_; SessionActions out; s.deadlines(out,now);
    if(s.phase!=SessionPhase::established || !begin.transaction_id || begin.normalized_bytes==0 || begin.normalized_bytes>65536) return out;
    s.invalidate_paste(out);
    if (!s.paste && s.terminal_paste && s.terminal_paste->transaction_id == begin.transaction_id) {
        paste_action(out, *s.terminal_paste); return out;
    }
    if(s.paste) {
        const auto& p=*s.paste;
        if(p.begin.transaction_id==begin.transaction_id && p.begin.normalized_bytes==begin.normalized_bytes && p.begin.crc32==begin.crc32) paste_action(out,s.status());
        else { auto st=s.status(wire::PasteStatusReason::conflict); st.state=wire::PasteState::finished; paste_action(out,st); }
        return out;
    }
    ServerSession::Impl::PasteUpload p; p.begin=begin; p.bytes.reserve(begin.normalized_bytes); p.deadline=now+30s;
    p.owner_epoch=s.snapshot.epoch; p.owner_intent=s.intent;
    s.terminal_paste.reset(); s.paste=std::move(p); paste_action(out,s.status()); return out;
}
SessionActions ServerSession::paste_chunk(const wire::PasteChunk& chunk, SessionTime now) {
    auto& s=*impl_; SessionActions out; s.deadlines(out,now);
    if (s.invalidate_paste(out)) return out;
    if (!s.paste && s.terminal_paste && chunk.transaction_id == s.terminal_paste->transaction_id) { paste_action(out,*s.terminal_paste); return out; }
    if(s.phase!=SessionPhase::established || !s.paste || chunk.transaction_id!=s.paste->begin.transaction_id) return out;
    auto& p=*s.paste;
    const auto fail=[&](wire::PasteStatusReason reason) { auto st=s.status(reason); st.state=wire::PasteState::finished; s.finish_paste(out,st); };
    if(chunk.payload.empty() || chunk.payload.size()>960) { fail(wire::PasteStatusReason::invalid); return out; }
    if(chunk.chunk_index<p.next) {
        const auto off=static_cast<std::size_t>(chunk.chunk_index)*960;
        if(off+chunk.payload.size()<=p.bytes.size() && std::equal(chunk.payload.begin(),chunk.payload.end(),p.bytes.begin()+static_cast<std::ptrdiff_t>(off))) paste_action(out,s.status());
        else fail(wire::PasteStatusReason::conflict);
        return out;
    }
    if(chunk.chunk_index!=p.next || p.bytes.size()+chunk.payload.size()>p.begin.normalized_bytes ||
       (p.bytes.size()+chunk.payload.size()<p.begin.normalized_bytes && chunk.payload.size()!=960)) { fail(wire::PasteStatusReason::invalid); return out; }
    p.bytes.insert(p.bytes.end(),chunk.payload.begin(),chunk.payload.end());
    p.accepted_bytes=static_cast<std::uint32_t>(p.bytes.size()); ++p.next;
    p.state=p.bytes.size()==p.begin.normalized_bytes ? wire::PasteState::uploaded : wire::PasteState::uploading;
    if (p.state == wire::PasteState::uploaded) p.completion_proof=s.latest_proof;
    paste_action(out,s.status()); return out;
}
SessionActions ServerSession::paste_execute(const wire::PasteExecute& execute, SessionTime now) {
    auto& s=*impl_; SessionActions out; s.deadlines(out,now);
    if (s.invalidate_paste(out)) return out;
    if (!s.paste && s.terminal_paste && execute.transaction_id == s.terminal_paste->transaction_id) { paste_action(out,*s.terminal_paste); return out; }
    if(s.phase!=SessionPhase::established || !s.paste || execute.transaction_id!=s.paste->begin.transaction_id) return out;
    auto& p=*s.paste;
    if (p.state == wire::PasteState::preparing || p.state == wire::PasteState::executing) { paste_action(out, s.status()); return out; }
    if(p.state!=wire::PasteState::uploaded || p.bytes.size()!=p.begin.normalized_bytes || support::crc32_ieee(p.bytes)!=p.begin.crc32 || !valid_paste_text(p.bytes)) { paste_action(out,s.status(wire::PasteStatusReason::invalid)); return out; }
    // Execute is intent. A proof must be newer than completion and currently leased.
    if(!s.latest_proof || s.latest_proof <= p.completion_proof || !s.execution || now>=*s.execution) { paste_action(out,s.status(wire::PasteStatusReason::proof)); return out; }
    p.state=wire::PasteState::preparing; p.lease=now+500ms;
    paste_action(out,s.status()); return out;
}
SessionActions ServerSession::paste_cancel(const wire::PasteCancel& cancel, SessionTime now) {
    auto& s=*impl_; SessionActions out; s.deadlines(out,now);
    if (s.invalidate_paste(out)) return out;
    if(s.paste && s.paste->begin.transaction_id==cancel.transaction_id) s.cancel_paste(out,wire::PasteStatusReason::canceled);
    else if (!s.paste && s.terminal_paste && s.terminal_paste->transaction_id==cancel.transaction_id) paste_action(out,*s.terminal_paste);
    return out;
}
SessionActions ServerSession::paste_keepalive(const wire::PasteKeepalive& keepalive, SessionTime now) {
    // The relay drains reliable controls before its deadline pass. This lets a
    // keepalive received in that pass renew an executing transaction at `now`.
    auto& s=*impl_; SessionActions out;
    if (s.phase != SessionPhase::established || !s.paste || s.paste->state != wire::PasteState::executing ||
        s.paste->begin.transaction_id != keepalive.transaction_id || !s.paste->lease) return out;
    // This lease follows the reliable control connection, not transient video presentation.
    s.paste->lease = now + 500ms;
    // Acknowledging receipt lets the client replace its one coalesced keepalive.
    // RelayServer coalesces this status to 50 ms, so progress traffic stays bounded.
    paste_action(out, s.status());
    return out;
}
SessionActions ServerSession::paste_started(std::uint64_t job_id, SessionTime now) {
    auto& s=*impl_; SessionActions out; s.deadlines(out,now);
    if (!s.paste || s.paste->state != wire::PasteState::preparing) return out;
    s.paste->state = wire::PasteState::executing; s.paste->job_id = job_id;
    paste_action(out, s.status());
    // The serial job owns mapped gestures. Do not retain upload source after handoff.
    s.paste->bytes.clear(); s.paste->bytes.shrink_to_fit();
    return out;
}
SessionActions ServerSession::paste_started(SessionTime now) {
    return paste_started(0, now);
}
SessionActions ServerSession::paste_start_failed(SessionTime now) {
    auto& s=*impl_; SessionActions out; s.deadlines(out,now);
    if (!s.paste || s.paste->state != wire::PasteState::preparing) return out;
    auto st=s.status(wire::PasteStatusReason::serial); st.state=wire::PasteState::finished;
    s.finish_paste(out,st); return out;
}
SessionActions ServerSession::update_ascii_paste(const AsciiPasteSnapshot& snapshot, SessionTime now) {
    // The relay samples terminal serial state before its deadline pass, so a
    // completed job observed at `now` cannot be canceled by that same pass.
    (void)now;
    auto& s=*impl_; SessionActions out;
    if (!s.paste || s.paste->state != wire::PasteState::executing ||
        snapshot.job_id != s.paste->job_id) return out;
    auto st=s.status(); st.completed_bytes=static_cast<std::uint32_t>(
        st.accepted_bytes * (snapshot.total_gestures ? snapshot.completed_gestures : 0) /
        (snapshot.total_gestures ? snapshot.total_gestures : 1));
    if (snapshot.state == AsciiPasteState::completed) { st.state=wire::PasteState::finished; st.outcome=wire::PasteOutcome::completed; }
    else if (snapshot.state == AsciiPasteState::canceled) { st.state=wire::PasteState::finished; st.reason=wire::PasteStatusReason::canceled; }
    else if (snapshot.state == AsciiPasteState::failed) { st.state=wire::PasteState::finished; st.reason=wire::PasteStatusReason::serial; }
    if (st.state != wire::PasteState::executing) { s.finish_paste(out,st); }
    else if (st.completed_bytes != 0) paste_action(out,st);
    return out;
}
std::optional<wire::PasteStatus> ServerSession::paste_status() const {
    return impl_->paste ? std::optional{impl_->status()} : impl_->terminal_paste;
}
std::optional<std::span<const std::uint8_t>> ServerSession::pending_paste_bytes() const {
    const auto& s=*impl_; if(!s.paste || ((s.paste->state!=wire::PasteState::executing && s.paste->state!=wire::PasteState::preparing) || s.paste->bytes.empty())) return {}; return std::span<const std::uint8_t>(s.paste->bytes);
}
std::optional<std::pair<std::uint64_t, std::uint64_t>> ServerSession::pending_paste_owner() const {
    const auto& s=*impl_; if (!s.paste || (s.paste->state != wire::PasteState::executing && s.paste->state != wire::PasteState::preparing) || s.paste->bytes.empty()) return {};
    return std::pair{s.paste->owner_epoch, s.paste->owner_intent};
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
    if(!wire::encode_control(edge,wire::Direction::client_to_server)||!s.eligible(edge.epoch,edge.intent,edge.challenge,now)||!s.barrier || (s.paste && (s.paste->state==wire::PasteState::executing || s.paste->state==wire::PasteState::preparing))) return InputGate::rejected;
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
std::optional<SessionTime> ServerSession::control_deadline() const {
    const auto& s=*impl_;
    if (s.paste && (s.paste->state == wire::PasteState::executing || s.paste->state==wire::PasteState::preparing)) return s.paste->lease;
    return s.execution;
}
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
