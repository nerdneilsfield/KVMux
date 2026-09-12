# UDP video and KCP control transport replacement

## Outcome and scope

Implement the user's requested replacement of the dual-TCP relay with UDP video
and KCP control. Main conditions are intermittent loss, bursts, reordering and
brief interruption, not ISP-specific UDP policy. Keep MJPEG/HEVC and public codec
backends, real platform capture and CH9329 input. Preserve control intent through
transient impairment without requiring a click solely because the network paused.
Authorization: implementation carried forward from the explicit rewrite request;
Counterweight Deep explicitly requested. Local commits; no new push authorization
assumed. Existing worktree was clean. No real-device interruption or network-wide
impairment is authorized. Use isolated synthetic loopback/cross-host probes.

## Decisions and constraints

- Replace the unreleased wire protocol directly; no v2 compatibility transport.
- KCP carries bounded reliable control messages, not the video byte stream.
  UDP video carries bounded, sequenced frame fragments; never rely on IP fragmentation.
- Vendor the minimal pinned upstream MIT KCP C implementation and license. No
  RustDesk/Sunshine GPL/AGPL implementation copying or configure-time downloads.
- Separate session liveness, capture intent, input execution permission and video
  freshness. A short input freshness expiry must not itself destroy transport.
- Focus loss, Host exit and explicit disconnect revoke intent even during outage.
  No blind replay of uncertain relative motion/wheel or historical clicks.
- Session identity is a pairing/fencing token, not authentication; trusted LAN only.
  NAT traversal, relay discovery, encryption and automatic TCP fallback are outside
  this requested replacement. User configuration must retain device selections.
- All ingress, KCP queues and frame reassembly have explicit byte/count/age limits.
  HEVC loss recovery resets references and waits for a decodable refresh point.

Blocking design details are deliberately not marked ready: handshake/token wire
layout, fresh-state reconciliation/serial acknowledgement boundary, UDP fragment
layout/reassembly limits and media feedback/pacing rules. Resolve these in this
plan before their implementation task starts; current phase is SHAPE.

## Acceptance map

| ID | Behavior | Task | Check and expected result |
| --- | --- | --- | --- |
| A1 | Bounded reliable control under loss/reorder | T1 | Deterministic datagram-loss fixture plus native UDP loopback delivers accepted control messages in order with bounded queues; idle poll is not disconnection. |
| A2 | Video does not become an ordered reliable backlog | T2 | Synthetic MJPEG and HEVC fragment loss/reorder/duplicates; complete frames recovered, incomplete frames expire within declared bounds; no dependent HEVC frame published after missing reference. |
| A3 | Session survives brief network interruption | T3 | Same session survives 100/300/800/2000 ms simulated interruption; unavailable input suspended without global Fault; reconnect only after separately declared liveness expiry. |
| A4 | Input converges without stale actions | T3 | Held-key interruption, lost release, delayed old epoch, Host/focus exit: acknowledged neutral/reconciled state, no stale relative/wheel or click replay, no reclick for network-only pause. |
| A5 | Sender does not perpetually outrun receiver | T2/T4 | Rate cap below offered video load; bounded oldest-frame age/bytes, recovery to fresh decodable picture, feedback and recovery reasons observable. |
| A6 | Usable replacement on supported native paths | T4 | Existing Mac debug CTest and Release, native Linux headless build, isolated Jetson-to-Mac synthetic MJPEG/HEVC with reconnect; no hardware-performance claims from synthetic tests. |

## Ordered execution

### D1: Resolve executable wire and state contracts
Status: in_progress. Depends on: none. Read-only discovery; no implementation.
Read src/network/relay_{protocol,client,server}.{hpp,cpp}, tcp_socket.{hpp,cpp},
control event/sink/serial worker and input router/session callers. Inspect CMake
and pinned KCP source. Record exact new types, bounds, ownership and test commands
here. Stop discovery once T1 is executable; later task details may remain blocked.

### T1: Bounded KCP control over native UDP
Status: done. Depends on: D1. Acceptance: A1.
Deliver a real loopback-capable transport with owned datagrams, pinned KCP,
message/queue bounds and typed receive/liveness outcomes. Include tests and
inventory/build registration in the coherent commit. Exact files/contracts/check
commands will be specified before readiness, not delegated as an open-ended layer.

### T2: UDP media delivery and reference recovery
Status: done. Depends on: T1. Acceptance: A2, A5.
Deliver bounded frame packetization/reassembly with pacing and refresh feedback,
using existing owned MJPEG/HEVC payloads and codec interfaces. Resolve loss/FEC
tradeoffs from the concrete burst-loss acceptance before implementation.

### T3: Replace relay sessions and reconcile input
Status: pending. Depends on: T1, T2. Acceptance: A3, A4.
Replace client/server transport and lifecycle together. Implement explicit
session negotiation, input revision/epoch recovery and captured-intent handling.
Remove old TCP-only paths/tests where superseded, not by compatibility wrappers.

### T4: Diagnostics, usage and native integration acceptance
Status: pending. Depends on: T3. Acceptance: A5, A6.
Update CLI/GUI transport status and configuration without expanding the single-line
bar. Update LAN/build/design documentation alongside actual behavior. Verify real
loopback plus isolated cross-host synthetic paths; record untested hardware limits.

## Execution and progress

Follow Counterweight Deep: one implementation task at a time, run/check, inspect
scope and commit each passing coherent unit promptly. No routine reapproval after
plan readiness. Parent owns this plan and integration. Current next action D1;
no source changes or passing implementation claims yet.

## Final acceptance

Working directory: /Users/dengqi/Source/langs/cpp/KVMux. Existing build/check entry:
`cmake --preset macos-debug`, `cmake --build --preset macos-debug`,
`ctest --preset macos-debug --output-on-failure`; Release via
`cmake --build --preset macos-release`. New targeted test names and impairment
fixture commands are specified with their owning task before it becomes ready.
Native cross-host checks must use reserved test ports and synthetic CaptureSource/
SerialIo, not open the user's camera/serial or stop their relay.

## T1 executable contract (ready)

New `src/network/udp_socket.{hpp,cpp}` in namespace kvmux::udp:
move-only Socket, static bind(host, port, error), local_port(), send_to(endpoint,
span), receive(timeout), close(); resolve(host,port,error) returns owned Endpoint.
Endpoint stores owned sockaddr storage/length behind portable representation,
supports equality. Datagram has source and owned bytes. ReceiveResult explicitly
separates datagram/idle/error; datagrams are atomic, truncated/oversized packets
are errors or discarded with a named result, never partial accepted packets.
Use POSIX/Winsock nonblocking UDP and bounded waits, existing platform patterns.
Maximum accepted UDP payload 1200 bytes; idle receives preserve the socket.

New `src/network/kcp_channel.{hpp,cpp}` in namespace kvmux::relay:
KcpChannel(uint32_t conversation), noncopyable owner-thread object;
submit(span)-> enum accepted/full/invalid; input(span)->bool;
update(uint32_t monotonic_ms); take_datagrams()->vector<vector<uint8_t>>;
receive()->optional<vector<uint8_t>>; failed()->bool.
Message mode; message size 1..1024 bytes, at most128 pending send segments.
KCP MTU=1168 leaves32 bytes for the later outer session envelope within1200;
windows128/128, nodelay(1,10,2,1). No blanket use for video. This small reliable
control workload is bounded; pacing/adaptation of bulk media belongs to T2.
Reject submit when ikcp_waitsnd>=128 before ikcp_send. Internal output bound256
packets; overflow fails the channel rather than growing memory. Received peer
segments must be completely validated before ikcp_input: correct conversation,
legal command/frg, payload length, declared window <=128. Bound delivered receive
queue using KCP's window and pull messages directly (no second unbounded queue).
No lease logic or automatic reconnect inside this primitive. Its caller must
invoke update each10ms even during application idle and never equate KCP ACK with
serial execution. Later handshake supplies conversation and session envelope.

Vendor only ikcp.c, ikcp.h, LICENSE from upstream KCP commit
32da082e529a26730aea3eb19922f80634ee6dea in new third_party/kcp. Record source/pin
and fileSHA256 in third_party/INVENTORY.md; build local C static library in
third_party/CMakeLists.txt. No Git submodule or configure downloads in project.
Register new source files in root CMakeLists.txt linking internal kvmux_core.

New tests/udp_kcp_test.cpp, CTest name udp_kcp, target kvmux_udp_kcp_test:
1. Two real UDP localhost sockets, ephemeral ports; endpoint resolution, datagram
   atomicity, idle timeout then successful subsequent receive, bounded shutdown.
2. Two KcpChannel objects connected by a deterministic simulated datagram queue,
   update clock10ms, bounded loop <=15s simulated. Deliver numbered small messages
   through fixed periodic drops, duplicate/reordered packets and800ms blackout;
   received sequence exactly once/in order, no output before missing prefix.
3. Submit saturation rejects extra messages; oversized message rejected; malformed
   and wrong-conversation input rejected, no oversized output/backlog growth.
4. Drive KCP through the real UDP sockets for a small ordered message roundtrip.
Check in repo: `cmake --preset macos-debug`,
`cmake --build --preset macos-debug --target kvmux_udp_kcp_test -j 8`,
`ctest --preset macos-debug -R '^udp_kcp$' --output-on-failure`.
Diff inspection and local commit scope: bounded UDP/KCP transport with its tests,
vendor inventory/license and CMake registration. Existing TCP remains only until
T3 updates its callers; not a retained compatibility mode.

Input recovery design discovered: keep intent separate from execution permission,
state snapshot plus healthy ordered edges, immutable serial-ACKed revisions.
Reliable delayed heartbeats must not extend input permission: use server-issued
bounded fresh challenges anchored to server issue time, not receipt time. Exact
wire/state integration details remain T3-blocking until specified here. Research
notes: /tmp/kvmux-v3-input-contract.md. No claim of target USB readback.

Progress: D1 established T1 contracts; T1 in_progress. Later handshake, video and
input integration details still pending and must not be implemented by guessing.

T1 acceptance: native macos-debug configure and targeted build passed;
`ctest --preset macos-debug -R '^udp_kcp$' --output-on-failure` passed 1/1.
100 ordered messages survived deterministic loss, reorder, duplication and800ms
blackout; native UDP and16-message KCP echo passed. Bounds and malformed input
checks passed, including pendingACK cap256 and output cap256. Minimal pinned
vendor bytes preserved, hashes recorded in INVENTORY. Implementation committed.
No relay integration, hardware test or push performed. T2 media-contract discovery
is active; no second implementation task starts until its contract is ready.

## T2 executable contract

## Exact packet format

Outer session envelope is owned by T3, reserves 32 bytes, and validates session before media input. Media body max 1168 bytes; complete UDP payload max 1200. No IP fragmentation.

40-byte big-endian media header, followed by <=1128 payload bytes:

- u8 version=1, u8 codec (existing VideoCodec values), u8 kind (0=data, 1=XOR), u8 flags (bit0 IDR, otherwise zero; MJPEG requires zero).
- u64 generation (agreed session generation for both codecs).
- u64 frame_sequence (MJPEG capture sequence, HEVC encoded_sequence).
- u32 serialized_size (1..16MiB+24 or +58 depending codec; codec decoder supplies actual lower-bound validation).
- u16 data_fragment_count, u16 index (data index or XOR group index).
- u16 payload_size, u16 reserved=0.
- u64 reserved=0.

Data count exactly ceil(serialized_size/1128), no zero frames. Data offsets are implicit index*1128. Every nonfinal data payload is 1128; final length derived exactly from total. XOR index ranges 0..ceil(data_count/8)-1; XOR payload is always 1128. Flags, size, count, codec and generation must match every packet for a frame. Never trust a packet offset or allocate from unvalidated count. Reject unknown flags/version/kind, wrong lengths, bad counts, codec mismatch and session/generation mismatch before allocation. Completed decode metadata must match fragment frame_sequence, codec, generation (HEVC), and IDR flag. No remote monotonic timestamp is used as a local deadline. Sequence zero reserved; do not wrap a session sequence counter.

## Minimal FEC: include 8+1 XOR now, with honest boundaries

Send eight consecutive data fragments then one parity fragment; last group may be shorter. XOR zero-padded payloads to 1128 bytes. Grouping and final lengths derive from header: no length table, retransmission protocol or Reed-Solomon library. Reconstruct only when exactly one data fragment is missing and parity is present; trim final fragment from serialized_size. Missing parity alone does not delay complete data. Duplicate identical data/parity is ignored, never counted twice. Conflicting duplicates invalidate that frame, not the session.

This is useful for independent random erasures: an 8-data group succeeds with probability (1-p)^8 + 8*p*(1-p)^8 when its parity is also independently lost with probability p. At 1% loss this is about 0.9966 per full group vs 0.9227 without parity. For ~500 data packets, whole-frame survival rises from about 0.0066 to about 0.81 (independence assumption). Overhead is approximately 12.5%, explicitly included in pacing. This is NOT burst protection: any two missing data in a group remain unrecoverable, and consecutive data+parity loss can also defeat recovery. No interleaving buffer or burst-repair claims. Tests must show unrecoverable bursts triggering refresh, not silently passing them.

## Bounded reassembly and ordering

Single-owner `MediaReceiver(codec,generation)` with `input(span, now)` and `poll(now)`; clock injected in tests. Return owned completed-frame events and typed recovery/feedback events, never call GUI/codec from transport. Process/deliver callbacks synchronously or return one bounded batch; do not hide another output queue.

- Max 8 resident frames, **32 MiB total charged allocation**, including parity, bitmaps and frame buffers. Pre-charge worst-case allocation at admission (checked arithmetic); parity cost is ceil(count/8)*1128. Metadata is bounded too. No allocation for frame-ID-sized sparse arrays.
- Age deadline: 150 ms from first accepted packet for a frame; duplicate packets never renew it. Explicit poll expires frames even if socket is idle. Use the same first-arrival time on completed output; decode_mjpeg/hevc currently overwrite arrival, so T3 must restore transport first-arrival after decoding.
- On capacity pressure evict oldest admitted frame; report capacity-loss. For HEVC loss of required/future AU triggers chain recovery. While waiting for IDR, reject non-IDR frames before large allocation.
- MJPEG: publish newest complete frame immediately, purge incomplete frames <= published sequence, ignore all later arrivals for those sequences. Capture gaps are normal; no gap wait.
- HEVC: start waiting for IDR. Admit reordered future AUs only within bounded frame/byte limits. Once synchronized, deliver strictly expected encoded_sequence. Seeing future sequence starts a **40 ms gap timer**, anchored to first evidence, not renewed by duplicates/new future packets. This covers a frame whose every packet was lost (there is no reassembly entry to expire). poll must check gap even when traffic stops.
- Gap timeout, required-frame expiration, invalid completed codec body, ingress overflow, decoder failure, or sender abort -> `recover(reason)`: clear dependent reassembly and queued completed AUs, increment recovery marker, emit reset/refresh event, enter waiting-IDR. Old packets <= retired high-water mark are discarded. At recovery boundary, retain/admit a newer complete valid IDR if available; drop older generations permanently. A valid newer IDR may bypass an unresolved gap immediately, but must emit reset before delivery. Subsequent AUs remain ordered from that IDR+1.
- Refresh requests are level-triggered while waiting, coalesced and rate limited to once/100 ms; T3 sends them in KCP control, scoped to current generation. Recovery of a missing P frame never waits for an old P retransmission. Decoder reset marker fences already in-flight output.

## Sender and pacing API

`MediaPacer` is not a socket/thread: configured byte/s rate, monotonic clock, max burst **2400 total UDP bytes**. Token bucket starts with one 1200-byte credit, caps at 2400; idle time cannot accumulate a giant burst. Charge actual 32-byte envelope + media body for data AND parity. `next_deadline(now)` and `next_datagram(now)` return at most one owned body, not all fragments of a frame. T3 sends control first on each event-loop turn, services KCP every 10 ms, then at most two video datagrams before checking control again. Do not place all media packets in kernel/output queues. Nonblocking send EAGAIN retains at most the current datagram until frame deadline; no sleeping send loop.

Keep **one active serialized frame**, one latest-source slot, and at most one pending datagram; no vector of every packet. Active frame lifetime <=100 ms from packetization start AND source age <=250 ms. Expose `can_start(now)` and `offer_latest_source(owned source)`/`take_latest_source(now)` so the caller replaces raw HEVC sources BEFORE encode, and MJPEG sources BEFORE serialization/fragmentation. The primitive can expose this as a small latest-slot helper; it must not own VideoEncoder. Admission computes data+parity wire cost and rejects a frame that cannot fit its remaining 100ms deadline at current configured rate; receiver legal maximum is not a promise that a 16MiB frame is deliverable at every cap.

HEVC encoded output is a dependency chain, not a latest-value slot. If an already encoded AU is skipped, active frame expires, or submission cannot be admitted: stop forwarding dependents, signal encoder request_keyframe, drain/discard non-IDR outputs until fresh IDR, then resume. Do not renumber dropped encoded AUs to hide gaps. Source replacement before encoding does not break references. For MJPEG drop expired active frame and take latest source at the next slot. Expose dropped-source, sender-deadline, receiver-gap/age/capacity, XOR-recovered, unrecoverable and waiting-IDR counters.

Use configurable fixed transport rate cap in T2, with receiver feedback event every 100ms containing cumulative received/recovered/lost frames, last completed encoded/capture sequence, waiting-IDR and capacity/age pressure. These are current-session counters, not delivery ACKs. T3 wire integration and T4 adaptive bitrate/source-rate tuning must consume this. A fixed cap plus pre-encode admission proves bounded offered-load behavior; it does NOT promise useful video if every IDR exceeds the deadline budget. Expose that explicit `frame_exceeds_rate_budget` reason. Lower encoder bitrate/resolution or raise cap is necessary in that case; do not disguise permanent recovery as success. The unit need not invent an unverified congestion controller. User-visible complete acceptance remains T3/T4, not T2 alone.

## Runnable T2 acceptance

New CTest `udp_media`, target `kvmux_udp_media_test`, using virtual clock and deterministic datagram fixture; use real native UDP pair from T1 for a short packet roundtrip, no RelayClient/Server required.

1. Existing serialized MJPEG/HEVC body roundtrip byte-identically under out-of-order/duplicates; single-erasure recovery for first/middle/final fragment, short final group, parity loss alone, two erasures unrecoverable. Validate codec metadata with actual decode helpers, not arbitrary bytes only.
2. Boundary sizes: 1 fragment, 1128 boundary, >65535-byte body, maximum 16MiB compressed data with codec overhead; 1200-byte full envelope ceiling; reject malformed count/index/length, unknown fields, wrong generation, conflicts, cross-frame parity.
3. Flood admissions and high sequence IDs; allocation charge never exceeds 32MiB and resident count never exceeds eight; duplicate arrival cannot extend age. No-traffic poll expires an incomplete frame and a wholly missing HEVC hole.
4. HEVC deterministic trace IDR1/P2/P3, lose P2, reorder P3: no P3 emitted; <=40ms after gap evidence reset+refresh. Burst removes multiple data/group including IDR; no false repair, periodic <=10Hz refresh, then fresh complete IDR10/P11 resumes strictly ordered. Test reset marker integration seam, generation switch and no replay of retired packets. MJPEG resumes with next complete frame without IDR.
5. Pacer offered source every 5ms at capped link: max two packets/turn, <=rate*elapsed+2400 bytes, bounded active age and memory, latest unsent source replaces older one before packetization; HEVC skipped AU requests IDR. A control sentinel queued between data packets is handled before next video batch. Source suitable for cap eventually delivers; intentionally oversized IDR returns explicit budget failure.
6. Seeded independent 1% loss comparison, >=1000 moderately sized frames, verifies parity recovers substantially more whole frames than no parity; no assertion of guaranteed delivery. Fixed burst test verifies recovery contract instead. Virtual 100/300/800/2000ms blackouts retain no stale frame backlog and resume from new MJPEG/IDR after return; no session liveness claim at this layer.

Commands: cmake --preset macos-debug; cmake --build --preset macos-debug --target kvmux_udp_media_test -j 8; ctest --preset macos-debug -R '^(udp_media|udp_kcp|relay_protocol)$' --output-on-failure. Parent to verify actual existing relay_protocol CTest spelling. Commit only after targeted checks and diff review; no concurrent implementation with T1.

### T2 ownership refinements

New src/network/udp_media.hpp and .cpp define MediaFrame (codec, generation,
sequence, idr, owned serialized bytes, first-arrival), MediaReceiver and MediaPacer.
Use steady_clock time_point as explicit API input. Completed media body validation
calls existing decode_mjpeg/decode_hevc; old packet TCP framing is not used.
Receiver returns bounded ordered event batches carrying reset-before-frame and
refresh events; expose stats including charged bytes and resident frame count.
Declare public signatures in the header before implementation and send parent the
header for integration. Do not add a generic callback/plugin infrastructure.
Pacer accepts one MediaFrame, exposes admission result, one next datagram and
next wake time, discard/expiry result. Latest raw CaptureSample already has a
mailbox in caller: do not create a second source abstraction in this primitive.
Control-first scheduling remains a T3 acceptance; T2 can prove only one-datagram
pull and burst/rate bounds, not actual relay event-loop priority. Sender skip
returns typed needs-IDR information for caller, not encoder ownership.
Pacing rate includes outer32 bytes but excludes UDP/IP headers; document exact
unit so no physical-wire bandwidth guarantee is claimed. No FEC dependency is
needed for XOR; parity implemented independently, not copied upstream.

T2 readiness checked against actual relay_protocol CTest and codec serialization.
T1 committed and no other implementation worker active. Run commands above;
commit only owned udp_media sources/test, root CMake registration and this plan.
Full relay/input recovery remains blocked until T3 exact wire contract is written.

### T2 implementation and acceptance report

Implemented src/network/udp_media.hpp/.cpp, tests/udp_media_test.cpp and root
CMake registration. Public header was supplied before implementation. Bodies use
existing decode_mjpeg/decode_hevc validation, never TCP packet framing. Stateless
indexed packetization and single-active-frame pacing allocate no packet vector.

Receiver enforces 8 resident frames and 32 MiB charged resident allocation:
serialized body, XOR parity, byte bitmaps and 512 bytes of metadata per frame.
This is not a process-RSS claim. One codec validation temporary is bounded by
16 MiB + codec header/padding; returned frame bodies move without copying and
sum to <=32 MiB per synchronous batch, with <=32 event records. Caller must not
accumulate batches. Source mailbox/drop-source accounting remains caller-owned.

Final-packet EAGAIN seam: caller saves active_deadline before pulling each body,
retains only {body, deadline}, and does not submit/pull again until sent or
expired. A final pull releases the active frame, so can_start alone does not
permit overwriting that pending body. Expiry calls discard(sender_deadline),
including an unsent final packet; HEVC reports needs_idr. Actual control-first
scheduling and the control sentinel belong to T3, not this standalone primitive.
Rate counts the outer 32-byte envelope and media body, excluding UDP/IP headers.

Loss recovery without an available valid IDR retires all observed sequence IDs.
A complete validated newer IDR bypasses the gap with reset-before-frame, retires
its prefix, clears other buffered AUs, then accepts its consecutive chain. Thus
IDR10 with previously observed P12 can still accept P11; otherwise retiring P12
would make the recovered chain impossible. New generations require a new receiver;
old-generation packets are rejected before resident allocation.

Checks passed on native macOS Debug:
- cmake --preset macos-debug
- cmake --build --preset macos-debug --target kvmux_udp_media_test -j 8
- ctest --preset macos-debug -R '^(udp_media|udp_kcp|relay_protocol)$' --output-on-failure
  Result: 3/3 passed (udp_media, udp_kcp, relay_protocol).
- Byte-identical MJPEG/HEVC at one fragment, 1128-byte serialized boundary,
  >65535 bytes and maximum 16 MiB compressed data plus codec overhead.
- Reordering/duplicates, first/middle/final erasure, short-group recovery,
  parity-only loss, unrecoverable two-data erasure, malformed headers/lengths,
  metadata conflicts, cross-frame parity and generation rejection.
- Count/byte floods, high sequence IDs, duplicate-proof age expiry, idle poll,
  missing whole HEVC AU gap at 40ms, burst IDR failure, <=10Hz refresh, decoder
  and ingress failure, capacity recovery, ordered P2/P3 and fresh IDR10/P11.
- Saved-deadline final-packet EAGAIN simulation and 100/300/800/2000ms blackouts
  leave no stale active backlog, resume with fresh MJPEG/IDR.
- At 120000 bytes/s with source offered every 5ms: 120064 charged bytes over
  1000ms, 178 caller-side pre-encode replacements, 22 delivered frames. Burst
  credit after long idle is exactly 2400 bytes. Oversized IDR reports explicit
  frame_exceeds_rate_budget and needs_idr, not false recovery success.
- Seeded independent 1% erasures, 1000 frames: XOR delivered 973 versus 602
  without parity. This is not burst protection or a delivery guarantee.
- Real ephemeral-port native UDP pair roundtrip with 32-byte reserved envelope.

No relay/source integration, hardware performance, cross-host, adaptive bitrate,
or session-liveness acceptance is claimed. Native linker retains the existing
missing /Users/dengqi/.local/lib search-path warning; new media source compiles
without warnings. No push performed.

## T3 subdivision and next executable unit

T2 is committed and accepted. Split T3 into sequential independently verified
units: T3a serial-ACKed state synchronization; T3b v3 envelope/control wire and
session freshness; T3c relay replacement plus router intent recovery. No parallel
implementation. T3b/T3c exact protocol remains blocked until specified below.

### T3a: Immutable serial state synchronization
Status: done. Depends on: T2. Acceptance: A4 (serial subset).
Modify src/control/control_sink.hpp, serial_worker.hpp/.cpp and tests/serial_worker_test.cpp.
New DesiredInputState: uint8 modifiers, array<uint8_t,6> keys, uint8 buttons
(bits0..2), MouseMode mode, uint16 absolute_x/y (0..4095). No deltas/wheel.
New InputSync: uint64 epoch,intent_generation,revision plus DesiredInputState.
New AppliedInputState: bool known; uint64 epoch,intent_generation,revision;
DesiredInputState state. Append AppliedInputState applied to ControlSnapshot.
ControlSink::synchronize(InputSync)->SubmitResult default not_ready, real
Ch9329ControlSink override. Default is unsupported semantics for current fakes,
not old-wire compatibility. Normal ControlEvent unchanged in this unit.

Only ready/USB-ready/release-confirmed current epoch accepts synchronization;
nonzero intent/revision, unique supported usages excluding modifier usages in
keys array, buttons<=7, valid mode andcoordinates. Require control_active and
fresh existing UI heartbeat. Do not hardcode user-configurable Host exclusion
inside serial layer; router provides eligible state in T3c.
At most one pending/inflight snapshot; second submission returns overloaded
(no invisible overwrite). Fence ordinary queued input: synchronization admitted
only when action queue empty, no outstanding delta residuals; pause ordinary
submit until its snapshot completes. Worker existing info transaction may finish
first; snapshot must not interleave ordinary reports. Execute keyboard report,
then mouse report (zero relative deltas andwheel) from an immutable snapshot.
Publish applied.known only after BOTH successful matching serial ACKs and current
epoch, intent and revision. Never interpret GET_INFO as state readback. Existing
release/fault/disconnect/stall invalidates applied immediately and purges pending
sync; never publish a late canceled snapshot. Internal keyboard/button/absolute
state must match synchronization for subsequent healthy edges. On accepting
ordinary input invalidate snapshot-known conservatively until next synchronization;
healthy event application acknowledgements are a later T3b/T3c contract, not
silently inferred. Snapshot application does not mean target USB/application ACK.

Tests use existing SerialIo fake: keyboard-onlyACK insufficient; mouseACK publishes
exact immutable revision; second snapshot cannot relabel first; release/epoch
change betweenACKs prevents publication and starts neutral clear; relative sync
hasbuttonsbutzerodeltas/wheel; fault/timeout invalidates state. Existing serial
release andrelative uncertain-action tests must still pass.
Commands repo cwd: cmake --build --preset macos-debug --target
kvmux_serial_worker_test kvmux_ch9329_control_test -j8;
ctest --preset macos-debug -R '^(serial_worker|ch9329_control)$' --output-on-failure.
Inspect diff and commit this coherent control capability+tests+plan, no push.

T3a public types now match the declared contract in control_sink.hpp.
T3c must distinguish a completed synchronization barrier from the current
applied.known snapshot: ordinary healthy input invalidates snapshot knowledge but
must NOT re-enter recovery on every key/mouse action. Only actual freshness,
epoch/readiness failure or explicit cancellation resets the synchronization barrier.
A narrow serial-worker prerequisite moves thread launch to the constructor body,
after all members initialize; this fixes observed construction-order UB, not a
proven cause of the historical network timeout.

T3a accepted and committed: targeted native build and serial_worker/ch9329_control
CTest passed 2/2. Immutable two-report ACK, cancellation/epoch fencing, relative
zero-delta synchronization, sparse keyboard slots, heartbeat expiry and serial
stall/hard-timeout invalidation verified. Constructor thread launch now occurs
after member initialization. Only chip command acknowledgement is established,
not USB target readback. T3b exact wire contract is next; no push/hardware use.

## T3b executable protocol and session unit

## Files and transition

Add src/network/relay_wire.hpp/.cpp (namespace kvmux::relay::wire) and relay_session.hpp/.cpp (namespace kvmux::relay), tests/relay_wire_test.cpp and relay_session_test.cpp; register core sources and CTests relay_wire, relay_session. Keep old relay_protocol untouched and unused by new primitives: its current TCP callers and T2 codec serialization still build. This is staged source replacement, NOT v2 negotiation or runtime fallback. T3c moves reusable encode/decode_mjpeg/hevc into media_codec_wire.hpp/.cpp (or other clearly media-only file), changes udp_media and relay callers, then deletes old TCP framing, old session/control codecs and superseded tests. Do not change kProtocolVersion in old file mid-transition. New wire accepts version3 only.

## Exact outer datagram header

All integers big endian; serialize fieldwise, no sizeof(struct), packing or C++ enum casts. Exactly32 bytes:
 offset0 magic[4]='KVMX'; 4 version/u8=3; 5 kind/u8; 6 body_bytes/u16;
 8 session/u64; 16 client_nonce/u64; 24 conversation/u32; 28 reserved/u32=0.
Datagram length must equal32+body_bytes and be<=1200. Kinds:1 hello,2 welcome,3 confirm,4 ready,5 busy,6 kcp,7 media,8 challenge,9 proof,10 cancel,11 cancel_ack. Unknown kind/version/reserved, extra/truncated bytes, bad per-kind lengths rejected before allocation/KCP/media dispatch. kcp bodies1..1168 additionally validated by existing KcpChannel (including inner conversation); media bodies40..1168 additionally validated by MediaReceiver. All post-welcome packets carry exact nonzero tuple(session,nonce,conversation) and must match pinned peer Endpoint. No endpoint migration. Hello/busy rules below are the only zero-session exception. IDs are pairing/fencing tokens, not authentication.

## Handshake and one controller

Client creates nonzero random64 nonce per attempt. Hello: session=conversation=0, nonce nonzero; body codec/u8 (0 MJPEG,1 HEVC), reserved[3]=0. One server pending OR established slot. On free hello server allocates random nonzero session/u64, conversation/u32 and generation/u64; body welcome: codec/u8,reserved[3]=0,generation/u64 (12 bytes). Pin source endpoint and nonce. Same pending hello repeats identical welcome; other hello receives busy with zero session/conv, echoed nonce and empty body, without allocation or lease refresh. Pending slot expires at exactly2s since first hello; duplicates never extend it.

Confirm echoes full tuple and same12-byte welcome body. Server establishes on exact confirm, emits ready with same body; client establishes only on exact ready from configured server. Repeated confirm for established tuple repeats ready but does not extend liveness. Client retries hello/confirm each100ms, attempt deadline2s; a new attempt uses a new nonce. Client must not submit KCP/media before ready; server must not emit application traffic before confirm. Generation is negotiated independently of serial epoch, nonzero and constant for media receiver lifetime. Unknown tuple or prior attempt welcome/ready ignored. Established slot is never preempted by hello; clear it on explicit disconnect or10s freshness-probe expiry. Allocate KCP only for selected tuple (server on confirm/client on ready). Loss of ready is repaired by repeated confirm. Pending slot is one bounded object, no source-address map.

## Session liveness vs execution freshness: server clock only

Use explicit steady_clock::time_point in every primitive call, not system clock or transmitted monotonic timestamps. Server emits raw challenge every50ms after establishment: body challenge_id/u64 (nonzero increasing). Keep bounded ring of200 {id,issued_at}; IDs never wrap within session. Proof is raw/unordered to bypass KCP backlog: challenge_id/u64,intent_generation/u64,flags/u8,reserved[7]=0,presented_video_sequence/u64 (32 bytes). Flags bit0 capture-intent active, bit1 locally fresh presented video, all others zero. Active requires nonzero intent and nonzero presented sequence when fresh; client builds flags from CURRENT UI intent at reply time, never echoes cached active state.

Server accepts proof only for issued ID, exact peer/tuple, age<10s and ID newer than latest accepted proof. Session deadline=max(existing, issued_at+10s), NOT receipt+10s. Initial session deadline=confirm_time+10s. Client liveness advances only on strictly newer valid challenge IDs, deadline=local_receive+10s (a bounded delayed burst is not input authorization). KCP ACK, video, old proof and duplicate handshake do not renew server liveness. At deadline equality expire. Never treat an idle UDP poll or input expiry as session destruction.

Execution deadline can advance only with active+fresh proof, current noncanceled intent, ready/release-confirmed current serial epoch and proof age<250ms. Deadline=issued_at+250ms, not receipt+250ms. Active proof does not by itself complete synchronization. Every sync/edge also names a challenge ID and must arrive before that challenge's issue+250ms AND current execution deadline. This prevents an old KCP edge executing after a new proof reopens the lease. Expiry produces revoke-input once, invalidates barrier, discards unsent/queued uncertain actions; intent remains desired for network-only interruption. T3c calls release_all/set_control_active(false), then synchronizes latest eligible held state after fresh proof, neutral release and new epoch. No historical edge replay. Server loop must revoke explicitly at server deadline; calling sink.update_ui_heartbeat on delayed proof alone is insufficient because sink's own250ms clock would otherwise extend permission past server deadline.

Cancellation raw body intent_generation/u64,reason/u8,reserved[7]=0 (16 bytes); reason1 focus,2 Host,3 explicit release,4 disconnect. Accept for current or newer generation immediately, without freshness proof, before KCP dispatch. Track maximum canceled generation; delayed proof/sync/edge at or below it cannot reenable input. Duplicate cancel idempotently repeats cancel_ack with same body; never repeatedly increments serial epoch. Cancellation ACK confirms revocation recorded, NOT physical neutral state. New capture requires strictly newer intent generation. Client sends cancellation immediately and retries each50ms until ACK/session closes; also queues same cancel payload as reliable control, bounded/coalesced one outstanding cancellation. Disconnect closes slot after revoke; raw disconnect ACK loss need not keep slot alive (client bounded local timeout). No-cancel delivery still expires execution by250ms. Server emits neutral-complete status only after actual release_confirmed of resulting epoch.

## Reliable control binary messages

Each KCP message begins type/u8, flags/u8=0, payload_bytes/u16 then exact payload. Total4..1024, no text/native struct serialization; reject unknown fields, noncanonical bool, unknown enum, trailing bytes. Direction validated. Session tuple belongs to envelope, not repeated inside messages. Types and payloads:

1 Status (S->C): serial_epoch/u64, connection/u8 (explicit0..8 matching named disconnected/opening/monitoring/clearing/ready/stalled/reconnecting/fault/stopping), flags/u8(bit0 USB-ready,bit1 release-confirmed), reserved/u16=0, canceled_through/u64 (20). Diagnostics strings remain local/T4; no unbounded remote error string.
2 Sync (C->S): epoch/u64,intent/u64,revision/u64,challenge/u64,edge_floor/u64,state[13] (53).
3 StateAck (S->C): epoch/u64,intent/u64,revision/u64,edge_floor/u64,state[13] (45). ONLY emitted when AppliedInputState.known and all identifiers/state match the immutable admitted sync after both serial ACKs. edge_floor belongs to the server-held sync request because T3a AppliedInputState lacks it. Unknown state is expressed by Status/recovery, not fabricated StateAck. Do not ACK synchronize(accepted), KCP delivery or GET_INFO as application.
4 Edge (C->S): epoch/u64,intent/u64,sequence/u64,challenge/u64,kind/u8,variant bytes below.
5 Cancel (C->S): same16-byte body as raw cancel, same tombstone rules.
6 RefreshRequest (C->S): generation/u64,reason/u8,reserved[7]=0 (16); reason explicit MediaReason mapping0..14 in header order; use named validation table, not implicit enum serialization.
7 MediaFeedback (C->S): generation/u64,received_frames/u64,recovered_fragments/u64,recovered_frames/u64,lost_frames/u64,last_completed/u64,age_losses/u64,capacity_losses/u64,gap_losses/u64,unrecoverable/u64,flags/u8(bit0 waiting_idr),reserved[7]=0 (88). Cumulative current-generation values, not ACK; sender ignores wrong generation. Coalesce to latest unsent sample, <=10Hz. No adaptive controller in T3b.

State[13]: modifiers/u8,keys[6]/u8,buttons/u8,mode/u8(0 absolute,1 relative),absolute_x/u16,absolute_y/u16. Coordinates0..4095 even in relative mode; relative synchronization emits zero delta/wheel. Fixed12-bit coordinates are deliberate: this is exactly T3a/chip absolute desired-state domain, not reduced edge precision. Six sparse slots allowed; zeros allowed repeatedly; nonzero unique supported usages exactly04..73,7f..82,85..87,89..8f hex; modifiers never in keys; buttons0..7.

Edge variants:1 key usage/u8,pressed/u8;2 absolute x/f64,y/f64;3 relative dx/f64,dy/f64;4 button button/u8,pressed/u8,x/f64,y/f64;5 wheel steps/f64,x/f64,y/f64. f64 is IEEE754 binary64 bit pattern big endian; preserve finite doubles bit-for-bit (including signed zero), not float32, text or fixed point. Reject NaN/infinity; absolute/button/wheel coordinates0..1; button explicit0..2 (verified InputRouter emits number-1 and serial_worker uses 1<<button); pressed0/1. Key usages supported state set plus modifier usages e0..e7; no zero key edge. Relative/wheel finite doubles preserved; bounds/aggregation at existing serial queue, do not silently clamp wire. Event.timestamp is receipt-local, never transmit a host steady_clock representation.

All epoch/intent/revision/sequence/challenge/generation nonzero; edge_floor may be0. Revisions and sequence increase without wrapping per intent. Sequence<=floor/last accepted is duplicate and never resubmitted. Following a successful sync, first healthy edge must equal floor+1; a sequence gap or sink overload revokes barrier and requires new sync, not retry of possibly applied relative/wheel/click events. StateAck confirms snapshot only, never an ordinary edge's physical execution. Ordinary accepted input invalidates T3a applied.known but does NOT invalidate the previously completed sync barrier. A sync establishes desired state and retires all historical sequences<=floor. Client serializes one outstanding snapshot at a time; later desired changes remain a latest local state, never relabel in-flight revision. On epoch, freshness or cancellation transition late StateAck is ignored.

## Primitive ownership/API

wire: typed EnvelopeKind/Envelope, typed control variant and functions encode_envelope/decode_envelope, encode_control/decode_control plus typed raw-body codecs. Decode returns optional/error without side effects; enforce all byte/domain bounds in both encode and decode. Borrowed body span is allowed only for synchronous envelope dispatch; control values owned.
relay_session: deterministic ServerSession and ClientSession, explicit peer endpoint, supplied random nonzero identifiers and explicit time. API on_datagram(peer,bytes,now), tick(now), server update_control_snapshot(snapshot,now), client set_intent(generation,active,video_fresh,presented_sequence). Return bounded actions (send raw datagram, established, expired, revoke_input, lease_changed); no threads, sockets, callbacks, serial calls or KCP queue inside. Expose validation gate for sync/edge using tuple/current challenge/epoch/intent/barrier. Fixed one slot,200 challenges, one cancellation tombstone; per-call <=8 actions; no accumulating internal action queue. T3c owns consuming actions and maps actual sink applied snapshot to wire StateAck. If this API is split into handshake and FreshnessGate classes, keep the same observable tests; no generic transport framework.

## Named focused tests and commands

CTest relay_wire:
- envelope_golden_offsets_and_1200_ceiling: exact32 header; KCP1168/body media1168; malformed sizes, magic/version/reserved/kinds, tuple zeros.
- control_golden_vectors_and_exact_lengths: all seven types/raw bodies, all truncation points, extra byte, max1024 guard, bad enums/bools/reserved and direction.
- desired_state_matches_serial_domain: sparse keys, duplicates/modifier rejection,4095/4096, buttons7/8.
- edge_binary64_roundtrip: values distinguishable from float32, fractional deltas/wheel, negative/signed zero; NaN/inf rejected, coordinate/key/button bounds.
CTest relay_session:
- handshake_loss_duplicate_and_single_controller: dropped welcome/ready, retries, foreign endpoint/nonce/conv, busy cannot preempt, pending2s hard bound, old-attempt fencing.
- server_issue_time_not_receipt_lease: proof arriving at249ms expires at250, at250 rejected; old reliable edges rejected after a newer proof; stale/duplicate proof cannot renew.
- session_10s_input_250ms_separation: virtual100/300/800/2000ms blackouts preserve tuple, input suspends when necessary;10s boundary expires; KCP/video do not count as proof.
- cancellation_overtakes_kcp_and_tombstones: raw cancel before delayed active/sync/edge, duplicate no repeated release, lower generation fenced; new generation needs fresh proof and sync.
- immutable_state_ack_and_barrier: accepted sync alone no ACK; one-report ACK insufficient (feed snapshot fixture), exact AppliedInputState required; late epoch/revision ignored; ordinary applied.known=false does not force recovery.
- edge_floor_gap_and_no_uncertain_replay: duplicates retired, gap/overload recovery, new snapshot floor discards old motion/wheel/clicks.
- challenge_ring_and_actions_bounded: long virtual run, invalid floods cannot allocate peer map or extend time; exact boundary deadlines and bounded outputs.
Use no hardware, real clocks or sleeps. T3b does not claim end-to-end serial/network acceptance. Run cmake --preset macos-debug; cmake --build --preset macos-debug --target kvmux_relay_wire_test kvmux_relay_session_test -j8; ctest --preset macos-debug -R '^(relay_wire|relay_session|udp_kcp|udp_media|relay_protocol|serial_worker|ch9329_control)$' --output-on-failure. Existing relay_protocol remains passing until T3c removes it. Parent merges decisions into plan before implementation. Stop discovery here.

### T3b readiness refinements

Status: in_progress. T3a committed and accepted. Parent approved the concrete
handshake/probe exception to reliable payload transport: application input,
synchronization and status use KCP; raw challenge/proof avoids stale reliable
heartbeats extending permission; raw cancel is repeated and also sent reliably.
No second generic transport or protocol fallback. All controls remain bound to
one exact peer/session tuple, without claiming authentication.

Codec selection follows existing CLI authority: server configured codec is
selected in Welcome; Hello advertises a supported codec bitmask (bit0 MJPEG,
bit1 HEVC), not a mandatory client codec. Reject zero/unknown bits and report
no-common-codec via a typed rejection (busy body reason/u8 with bounded known
values busy/no_common_codec), never silently change server codec. Update golden
vectors accordingly. Client GUI does not need a new encoder/codec selector.

Fix cancellation semantics: a cancel generation below the currently active newer
intent advances only the tombstone through that older generation; it must NOT
revoke the newer intent. Duplicates must not repeatedly request release. Higher
intent during active prior intent revokes old barrier before synchronizing new.
A proof with active=false for the current intent revokes permission immediately;
video_fresh=false likewise suspends permission, preserving intent. Neither waits
for250ms when the signal is already received.

Do not claim a presented sequence is sufficient proof of video freshness here.
T3c must gate against bounded sender frame history and current-generation actual
client consumption/presentation. Expose an explicit caller-provided video-valid
boolean when applying proof; missing validation defaults false. Raw probe can
renew session liveness without authorizing input. The250ms permission gate is
anchored to server-issued challenge times. This LAN-specific policy is not a
promise of usable input at arbitrarily high RTT.

Input event gap/recovery rules must match existing adjacent motion merging:
wire sequence is assigned after merging immediately before reliable admission;
never reuse router source sequence directly if coalescing skipped it. Keep source
sequence diagnostic separate if needed. Only edges actually admitted to KCP get
consecutive wire IDs. On expiry retire old sequence floor through next snapshot.

Single-owner session primitive may own admission/barrier metadata but not serial
I/O. Successful sync admission is not AppliedInputState; only exact immutable
applied identity+state can open barrier and emit StateAck. Serial request rejection
must not advance applied state. Preserve a completed barrier across ordinary
snapshot-known invalidation. Bound externally dispatched reliable message results
like raw packet results. Encode/decode strictly named enum mappings, static_assert
IEC559 double/sizeof8 where binary64 used. No extra host timestamp wire field.

Files owned by this unit: new relay_wire.hpp/.cpp, relay_session.hpp/.cpp,
new tests/relay_wire_test.cpp and relay_session_test.cpp, root CMake registration.
Parent owns plan, authorizes include it unchanged in passing unit commit. Existing
relay_protocol survives only until T3c replaces callers; no new code calls its
old control/TCP messages. Test commands above are the T3b acceptance.

T3b acceptance: declared relay_wire/relay_session/udp_kcp/udp_media/
relay_protocol/serial_worker/ch9329_control CTests passed 7/7 (8.34s),
with native configure and targeted build successful. Explicit binary envelope and
control validation, 200-entry challenge history, server-issue-time permission,
cancellation generation fences and immutable state-ACK barrier implemented.
Client cancellation retries every50ms; lost disconnect ACK ends locally within2s.
Diff inspection and coherent local commit pending. No actual relay replacement,
router integration, hardware or weak-network end-to-end claim at this checkpoint.

T3b committed and accepted: final native targeted CTests7/7 passed. Exact wire,
handshake and freshness contracts live in relay_wire.hpp and relay_session.hpp.
Slow serial completion is allowed only while newer valid proofs continuously
maintain permission; expiry or cancellation permanently invalidates that pending
sync. T3c discovery now resolves actual endpoint pairing, media presentation IDs,
worker scheduling and router intent integration before implementation. No source
compatibility commitment to v2; removal occurs with all affected callers/tests.

## T3c executable integration contract

## Recommended task boundary

Use one coherent T3c replacement commit, including client/server, router/session/GUI callers, codec extraction and test migration. Splitting network replacement from router intent leaves a buildable but behaviorally unsafe intermediate: current KvmSession forcibly releases on stale video/non-ready control, current InputRouter faults on submit rejection, and NetworkControlSink has no synchronize override. A small media-only extraction can be a prior independently checked commit if useful, but is not required. Do not split a second transport implementation or keep a TCP compatibility path.

## 1. Two configured server ports; one client endpoint

Keep existing ClientOptions/ServerOptions control_port and video_port and configuration selections. Server binds two UDP sockets; client binds ONE ephemeral UDP socket. Client sends hello/confirm/proof/cancel/KCP from this socket to the configured server control endpoint. Server sends media FROM its configured video socket TO the exact endpoint pinned by ServerSession's control handshake. Client receives both on its one socket, accepting raw/session/KCP only from the resolved control endpoint and media only from the separately resolved video endpoint plus the established exact tuple. No media hello, inferred client port+1, peer-address-only match, or new pairing packet is needed. Server ignores inbound media-port traffic; nothing received there grants ownership or freshness. Endpoint currently exposes equality only; this design needs no new Endpoint mutation/get-port API. Resolve the server host once consistently for both ports (adapter is currently IPv4-only). Bind failures unwind both sockets. Reject equal nonzero server control/video ports; port 0 tests bind independently and read actual ports.

Do not pass media-source endpoints to ClientSession::on_datagram (it deliberately pins the control endpoint). Decode and validate media envelope explicitly against ClientSession::tuple(), phase and expected media endpoint before MediaReceiver::input. KCP path validates peer/tuple before KcpChannel::input. New random nonzero IDs go through existing SessionIds and ClientSession::start; do not retain increment-only old session IDs.

## 2. Owner loops and codec scheduling

One network owner per client/server owns sockets, session primitive, KCP, and receiver/pacer. Every turn: bounded raw/control receives (cancel before reliable input when both are available), session tick + deadline revocation, latest sink snapshot/session actions, KCP update at <=10ms scheduling intervals, bounded reliable dispatch/output, then at most two media sends before returning to control. Flood ingress must not prevent tick. Idle receive is not disconnection. Socket waits are bounded by nearest session/KCP/pacer deadline, never old 350/500/600ms TCP receive timeouts.

Server conversion, VideoProcessor, swscale, encoder configure/reset/shutdown and serialization stay on a dedicated media worker. Never move old hevc_video work directly into the network loop: even lifecycle codec calls may wait, and conversion/serialization have real CPU cost. Worker owns codec throughout; network sends coalesced keyframe requests and admission credit, not cross-thread codec calls. Keep capture's latest raw sample replaceable BEFORE encode. Grant new encode work only when pacer has no active/pending datagram and ordered output slot has capacity. Bound worker-to-network ordered HEVC output explicitly (one output slot is sufficient if worker stops polling/admitting input when occupied). Never overwrite that slot as a latest-value mailbox. Codec-internal delayed output is still drained in order; any AU intentionally skipped gets an encoded sequence and forces IDR recovery. Reuse encoder output encoded_sequence if it satisfies the codec contract; old server redundantly assigned its own sequence. Never renumber around drops. MJPEG has one latest source and one serialized offer, with stale admission rejected.

Network owns MediaPacer. Save active_deadline BEFORE next_datagram. On would_block retain ONLY body+saved deadline, block further submit/pull; expire with discard(sender_deadline), including final packet. poll even without traffic. Every needs_idr result coalesces a worker request; drain/discard dependents until fresh IDR. Keep generation fixed for session. Capture absence/media loss suspends freshness, not 10s session liveness. Fatal codec/device errors are distinct from ordinary media gaps.

Specify a new ServerOptions transport byte/s cap independently of encoder bitrate; proposed default 12,000,000 bytes/s (envelope+payload+FEC, excluding IP/UDP). This is an explicit initial cap, not adaptive control or evidence that every source fits. Expose frame_exceeds_rate_budget rather than failing the session or reporting successful recovery. CLI exposure/adaptive tuning may remain T4, but integration tests set the cap directly.

KCP output would_block handling must also be bounded: retain at most the existing bounded output batch, service it before collecting more output; unrecoverable local send errors close transport. Do not let repeated take_datagrams append an unbounded secondary queue. Status/feedback/refresh have latest-unsent slots; synchronization/ACK are immutable, not replaceable. Keep at most one pending synchronization and its required ACK; if KCP full, retry admission without changing identifiers. Healthy edges receive consecutive wire sequence only after adjacent motion merge and successful KCP admission.

## 3. MediaReceiver, decoder and presentation

Consume every bounded MediaEvent batch synchronously. Reset clears ordered compressed ingress and latest unpublished decoded output and advances a marker before any subsequent frame enqueue. Decoder worker observes marker, calls reset/reconfigure on its own thread, and fences every in-flight output by both marker and generation. Ingress retains existing explicit 8 AU/32MiB/250ms limits; overflow or stale AU reports to the NETWORK owner, which calls receiver.recover(ingress_overflow/decoder_failure, now). Do not call single-owner MediaReceiver from decoder thread. A bounded coalesced recovery flag is sufficient. Network converts refresh/feedback events to current-generation KCP controls. Preserve reset-before-frame ordering when a new IDR bypasses a gap. Decoder failure is media recovery, not immediate global disconnect.

For MJPEG decode_mjpeg(frame.bytes, local_capture_generation); for HEVC decode_hevc(frame.bytes). Restore MediaFrame.first_arrival onto decoded sample/AU after these helpers, because they currently stamp a fresh local time. Preserve it through VideoPipeline and decoded output; don't rejuvenate stale data on dequeuing.

Actual sequence facts: MediaFrame.sequence is MJPEG capture sequence but HEVC encoded_sequence. ffmpeg_decoder.cpp sets output VideoFrame.sequence from au.capture_sequence. Existing RelayServer sent_sequence also used capture_sequence. Therefore a raw comparison to encoded sequence is wrong.

Minimal mapping: retain capture sequence as the public VideoFrame/GUI ID. Client maintains a bounded current-generation table of published capture sequence -> {media sequence, first_arrival, marker}. On actual GUI consumption/presentation, look up this exact entry, reject wrong generation/retired marker and translate to media sequence for Proof.presented_video_sequence. Server keeps a bounded sent-frame history keyed by media sequence with source arrival and completion time. Admit only entries whose entire data/parity send completed successfully (conservative if parity is lost locally), still current-generation and source age <500ms. Proof video_valid requires exact history membership, not sequence <= high-water. Proposed bound 512 entries, age-pruned at 500ms on server; client bound 512 with generation/reset clearing. Client fresh flag requires a newly consumed valid entry and its first-arrival age <500ms, plus recent GUI progress; duplicates do not renew consumption time. The number is a fixed memory bound, not an assumption about FPS.

Current KvmSession::take_latest_frame calls video_presented BEFORE renderer.upload. Either explicitly define this as actual GUI consumption (allowed by the prior contract), or preferably move acknowledgement to a new KvmSession::video_presented(generation,sequence) call after successful renderer.upload in main.cpp. The latter is this recommendation; add a generation-aware ControlSink/NetworkControlSink hook and update all callers/fakes. It proves upload/GUI consumption, not physical display scanout. Never acknowledge only because take_sample or capture.received_samples advanced.

## 4. Server input and ACK dispatch

On established: allocate KCP and media generation state; deactivate and neutralize serial once. Feed sink.snapshot into ServerSession::update_control_snapshot on changes and every relevant turn; consume returned state_ack only via reliable wire::StateAck. Session revoke_input means set_control_active(false), release_all ONCE for the transition, discard queued application work. Session tick is the authoritative 250ms expiry; receipt-local sink heartbeat is not its replacement.

Provisional execution_deadline permits sink activation + heartbeat only while now < deadline and ready/USB/release conditions still hold. This is needed BEFORE synchronize, otherwise the sink rejects the barrier. For wire::Sync, call check_sync, then sink.synchronize(InputSync{epoch,intent,revision,state}), then sync_submitted with actual result. edge_floor remains in admitted wire::Sync held by ServerSession. ACK only arises from exact immutable applied snapshot after both serial reports ACK; acceptance and GET_INFO are not ACK. For Edge, allowed -> submit receipt-local ControlEvent and edge_submitted; recovery_required -> edge_submitted(not_ready) WITHOUT sink submission; duplicate/rejected -> discard. Do not re-enter recovery merely because ordinary accepted edges set applied.known=false.

wire has no mouse_mode command: mode travels in DesiredInputState during synchronization. NetworkControlSink::set_mouse_mode updates next local desired mode; do not invent a new wire message or silently call server set_mouse_mode without a synchronization boundary. Verify serial synchronize's mode handling in implementation tests. Raw/KCP cancel share session.cancel tombstone behavior; cancel_ack is not neutral-complete status. Session expiry clears media, KCP, pending sync/ACK and history, and releases serial; brief impairment does none of the connection teardown.

## 5. Router intent and new Recovering state

Add InputState::recovering plus a capture_intended() accessor distinct from captured() execution. Intent spans arming/captured/recovering as appropriate after activation click release; preserve OS relative mouse capture in recovering. main.cpp currently uses equality captured to gate SDL and controls, so inspect/update every use and the state-name switch. Explicit release/focus/minimize/Host/disconnect revokes intent in recovering too. Network-only stale video, freshness expiry, changed remote epoch/readiness and queue admission failure suspend to recovering without requiring another activation click. Irrecoverable local faults remain fault/releasing. KvmSession's current request_release on video stale/non-ready must not erase recoverable intent.

Minimal explicit distinction proposed: add a ControlSnapshot flag for network recovery capability/state (name chosen in implementation, default false) so local serial behavior is not silently altered. No RTT heuristic or dynamic_cast. NetworkControlSink overrides synchronize and delegates to a new RelayClient synchronization admission method; the default ControlSink::synchronize(not_ready) is NOT sufficient. Client snapshot.applied is populated only from exact current StateAck; Status cannot fabricate applied state. Client provisional network readiness and completed input barrier must be represented separately; don't overload ready to mean both, which deadlocks synchronization.

Router owns intent generation, desired state and revision. Initial captured admission and every recovery require one immutable synchronization snapshot. Track latest eligible physical held keys (exclude Host, activation-isolated keys, and unsupported usages), modifiers, held buttons and latest absolute position. Keep changes made while waiting in latest desired state, never mutate in-flight snapshot. After exact ACK, if desired changed, submit a NEW revision before healthy edges; do not replay stale edges. Canonical key order, <=6 ordinary keys; define rollover deterministically using existing serial policy rather than creating invalid wire states. Tracking key releases must also remove isolation while recovering (current handle_key returns too early). Track button releases while recovering; relative/wheel residuals and pending special macro steps are discarded on recovery. No historical click/down+up or uncertain delta replay. A still-held eligible key/button may reappear via the current snapshot, as state reconciliation, not replay. New presses/releases completed entirely during outage vanish from latest state.

Router synchronization barrier remains complete across ordinary snapshot-known invalidation; only actual recovery/epoch/cancel clears it. Client adds wire challenge and edge_floor when admitting immutable InputSync; client owns wire sequence independent of router source sequence. Client rejects late StateAck after epoch, local freshness or intent changes. UI thread must not wait for network or serial ACKs.

Existing special keys run from Preview without persistent capture and include an explicit Host-key macro. Preserve the explicit macro feature separately from the physical local-only Host exit, but require the same fresh proof/synchronization barrier before scheduling macro edges. Cancel the rest of a macro on impairment; never restart it automatically. This caller cannot be ignored or special_keys will stop working after mandatory barriers are added.

## 6. Remove old TCP without losing media tests

Extract only encode/decode_mjpeg and encode/decode_hevc plus their private validation/byte helpers into media_codec_wire.hpp/.cpp. Update udp_media.cpp, tests/udp_media_test.cpp and new client/server includes. Migrate codec payload tests from relay_protocol_test to a media_codec_wire target. Delete old relay_protocol.hpp/.cpp, tcp_socket.hpp/.cpp, tcp_socket_test and obsolete framing/control tests after replacing relay_client_test and relay_server_test fixtures with v3 UDP behavior. CMake must remove old sources/targets and register new codec test. Do not delete entire protocol test coverage merely because framing changed. Preserve existing encoder fallback, software decoder and codec tests. Update stale 12-byte/TCP traffic comments/counters now: count actual accepted UDP datagram bytes including 32-byte envelope; define whether retries are included (recommended actual sent/received bytes). Broad docs/diagnostics tuning remains T4.

## 7. Focused integrated regression and acceptance

Keep relay_wire/relay_session/udp_media/udp_kcp primitive tests. Add one end-to-end regression using REAL RelayClient, RelayServer, InputRouter/KvmSession and Ch9329ControlSink with tests/relay_test_fakes.hpp SerialIo + synthetic capture. A test-only UDP proxy uses two front sockets: client sends to front control; proxy forwards all control to backend from ONE stable backend socket, and forwards backend video via front VIDEO socket. This preserves the exact one-peer/two-port contract. Proxy delays/drops/reorders whole datagrams with bounded queue; no generic production transport injection layer. Test production loops under short real timeouts, not only virtual session primitive actions.

Required trace for 100/300/800/2000ms blackouts: establish video and neutral; activation click; sync both ACKs; key down; blackout; key release while unavailable; resume fresh picture and same tuple; exact current state sync ACK; no stale relative/wheel/completed click; router returns Captured without new activation click. 100ms may avoid revocation if deadlines never expired; larger gaps must revoke input but not session. Include Host/focus during blackout -> Preview intent and no delayed reactivation; separately a >10s primitive expiry already exists, so no need for every integrated test to wait ten seconds.

Additional focused cases: lost welcome/ready; wrong media source/tuple ignored; two-controller busy; one serial ACK held back cannot unlock edges; late ACK after cancel cannot restore barrier; HEVC missing P -> reset before fresh IDR, no old marker publication; capture sequence gaps with consecutive encoded IDs; actual presentation mapping rejects unsent/stale ID; media worker deliberately slow configure/conversion with challenge/control sentinel progressing independently; cap below offered load has bounded buffers and fresh recovery. Use existing EncoderFactory fake for slow encoder; adding a concrete DecoderFactory seam is optional only if the existing real software HEVC fixture cannot exercise marker fencing deterministically.

Run native Debug configure and full build (all callers), focused CTests including relay_client, relay_server, relay_wire, relay_session, udp_kcp, udp_media, input_router, serial_worker, ch9329_control, new media codec and integrated test; then full Debug CTest. Full GUI target compile is required even though no hardware is used. Release/native Linux/cross-host/performance remain T4 unless parent elects earlier checks. Report synthetic evidence as such; no hardware claims.

## Remaining explicit choices, not invented existing APIs

Parent should record proposed cap, snapshot recovery flag, generation-aware presentation hook and special-macro admission before implementation readiness. Current headers do not provide these. The inspected APIs are sufficient for two-port pairing without ANY wire change. Unknown until focused implementation checks: exact serial mode transition behavior during synchronize, native GUI tests available for presentation callback, and whether codec's internal bounded output can yield more than one AU per accepted input on every supported backend. These require direct code/test confirmation, not new protocol design or external research. Discovery can stop here.

### T3c selected decisions and readiness

Status: in_progress. Depends on T3a/T3b/T2, all committed and verified.
Select the one coherent all-callers replacement boundary described above. Retain
server ports17000/17001 as UDP; one client endpoint, no protocol extensions.
Set ServerOptions::transport_bytes_per_second default12,000,000 (envelope+FEC,
not IP headers). This is pacing, not advertised achievable network capacity.
Adaptive behavior/user-facing rate diagnostics remain required in T4.

Add ControlSnapshot::recoverable_transport bool defaultfalse; NetworkControlSink
sets true even during transient control unavailability. Local serial semantics
remain unchanged except explicit supported synchronize calls. InputRouter uses
this typed capability for Recovering; no dynamic_cast. Add capture_intended()
to distinguish retained UI capture from execution readiness. Store state in router
using existing HidKeyboardState to preserve rollover and isolation semantics.
Network initial capture and recovery synchronize; local old activation behavior
may continue, since local serial has no network freshness barrier.

Presentation hook: add ControlSink::video_presented(generation,sequence) replacing
the old one-argument hook at all callers; add KvmSession::video_presented with
samearguments. Remove automatic notification in take_latest_frame. main calls
notification only after successful renderer upload. Synthetic tests explicitly
consume/upload-confirm frames via that method. This proves GUI acceptance, not
physical display scanout. Maintain a bounded mapping from GUIcapturesequence to
mediaencodedsequence, including marker/generation; no equality assumption.

Special macros from Preview must first acquire a temporary intent and exact
sync barrier under existing freshvideo conditions; schedule steps only after ACK.
Physical Host exit remains local, explicit Host macro remains supported. Cancel
macro on any interruption and returnPreview; never automatically replay it.
No background automatic capture: only retained focused capture intent resumes.

Worker owns all actual source/caller/test edits for this one sequential unit;
parent owns plan. May commit an independently passing media-codec extraction
before replacement if useful, but no commits with knowingly broken callers.
Mandatory actual integrated impairment cases are given above; primitive tests
alone cannot mark T3c complete. Network/session loops mustremain active during
100/300/800/2000ms interruptions; no manualreclick afternetwork-only pause.
No real devices, remote production changes or push. Sourcepaths and existing
CTest names verified in discovery. New integrated test name relay_recovery,
new media codec test media_codec_wire, both registered under existing BUILD_TESTING.
Run cmake --preset macos-debug; cmake --build --preset macos-debug -j8;
ctest --preset macos-debug --output-on-failure. If testtimeouts need increasing for
actual2s blackout traces set boundedpertestlimits, never remove behavior checks.
No new uncertain protocol decision blocks this task; actual implementation bugs
must be fixed and verified before marking it done. T4 native cross-host and
loadadaptation acceptance are not implied by T3c success.

T3c integration currently verifying full Debug suite. Real UDP interruption cases,
HEVC reference recovery, slow encoder/control independence, delayed serial ACK,
wrong peer/tuple rejection and limited-rate operation passed focused checks.
Full-suite execution exposed a previous-cancel/new-macro intent race; implementation
worker fixed generation/admission ordering and added actual serial Host-macro and
relative-mode synchronization assertions. Final full-suite result/commit pending;
do not classify this task complete from earlier targeted passes.

T3c completed and locally committed. Final exact-tree native macos-debug full
build and20/20 CTest passed (33.63s). Real UDP proxy blackout100/300/800/2000ms,
same-session/no-reclick held-state convergence, stale action fencing, focus/Host
cancellation, delayed serial ACK, HEVC reference recovery/capture-ID mapping,
slow encoder/control independence, wrong-source/tuple rejection, handshake loss,
limited-rate video and native OpenGL/ImGui tests passed. Old TCP runtime and wire
removed. Synthetic CaptureSource/SerialIo are not real target input acceptance.
T4 discovery pending: final parameters/diagnostics/docs, required load behavior,
Linux native and isolated cross-host evidence. No push performed.

## T4 remaining implementation and acceptance

## Scope decision

The existing plan requires feedback consumption and T4 adaptive bitrate/source-rate tuning (T2 Sender and pacing API), not a new congestion-control framework or mandatory dynamic codec API. Existing pre-encode credit and fixed MediaPacer satisfy bounded offered-load admission, but **do not finish T4 adaptation**: they depend only on local pacing/output occupancy and do not react to receiver feedback. They cannot make an oversized HEVC IDR fit the fixed 100ms budget.

Smallest compliant choice: receiver-feedback-driven source admission rate, before encoding/serialization, preserving all VideoEncoder methods. Apply bounded reduction after new loss/age/capacity/gap pressure and gradual recovery on sustained healthy samples; never treat cumulative counters as per-sample losses. Keep fixed transport cap as ceiling. Tune source rate only, do not claim codec bitrate changed or bandwidth estimation. Explicitly report persistent frame_exceeds_rate_budget and advice to lower configured HEVC bitrate or raise cap. MJPEG admission cannot shrink an individual JPEG. Plan explicitly acknowledges this limitation. Exact sample window, step sizes, minimum/maximum interval and recovery hysteresis remain design choices to record before readiness; no numbers are silently authorized by this report.

Actual codec bitrate feedback is absent. It is not necessary if parent selects the source-rate option already in the plan. If parent instead chooses bitrate reconfiguration: configure resets encoded_sequence to 1, while reset preserves it, and current negotiated media generation is fixed for session. Reconfigure under unchanged generation can retire fresh frames as old. Therefore do NOT casually call configure with a new bitrate, add a backend setter, or renumber dropped AUs. This would be a larger contract change than source admission tuning.

## Current wiring and exact edits needed

- src/network/relay_server.hpp: ServerOptions.transport_bytes_per_second exists, uint64, default 12,000,000. Only nonzero checked in RelayServer::start. No server snapshot API.
- src/app/relay_main.cpp: --bitrate exists (bits/s); --transport-rate does not. Numeric parser currently uses int. Add a named transport-cap option with explicit bytes/s unit, nonzero/overflow validation before device enumeration, help and startup output. Distinguish encoder bitrate from UDP media budget. Existing help incorrectly says LAN TCP.
- src/network/relay_server.cpp: constructs MediaPacer from cap at establishment; grants credit only with empty pacer/no pending datagram/no offer; media worker polls encoder and pulls latest capture before conversion. One ordered HEVC output slot. This is correct place for a tiny next-source-admission deadline; keep codec polling/draining independent so delayed AUs are not stranded. Network-owner receives feedback and publishes only bounded/latest admission settings to worker under existing mutex, not codec calls from network thread.
- src/network/relay_client.cpp: MediaReceiver feedback events become latest optional wire::MediaFeedback, reliably submitted through KCP. RefreshRequest is likewise wired. Server reliable dispatch handles Cancel/Sync/Edge/RefreshRequest but has NO MediaFeedback branch: feedback is decoded and discarded. Add current-generation gate and delta tracking; first sample establishes baseline, reset at session establishment/expiry, ignore regressed counters rather than unsigned underflow. No per-peer map or new queue.
- src/network/relay_wire.hpp/.cpp: MediaFeedback already carries generation, cumulative frame/loss/recovery counters and waiting_idr. No wire expansion needed for source tuning.
- src/network/udp_media.hpp/.cpp: MediaStats and MediaPacerStats exist, but sender reason is only numeric debug output and receiver stats are not exposed in GUI. MediaPacerStats.sent_bytes charges pulled packets, not verified successful socket delivery: label accurately, especially EAGAIN expiry.
- src/network/relay_client.hpp/.cpp: ClientVideoSnapshot contains only codec/backend/hardware fields, recoveries and error. Add bounded current-session media stats/last named recovery reason for diagnostics using existing mutex snapshot mechanism.
- src/network/relay_server.hpp/.cpp and src/app/relay_main.cpp: expose or log configured cap, effective source admission, received feedback deltas/counters and named sender rejection/recovery reason. Prefer change/periodic logging over per-datagram logs. A small snapshot fits existing ownership; no metrics subsystem.
- src/app/main.cpp Diagnostics window near current Decoder recoveries field: add UDP-v3/KCP identity, receiver loss/XOR/age/capacity/gap/waiting-IDR and last recovery reason. Preserve single-line status-bar layout. Server tuning state need not be invented as new remote wire fields: show server-local state in relay logs, receiver-local state in GUI.
- src/support/config.hpp/.cpp, tests/config_test.cpp: retain saved capture/serial/host/ports/decoder selections; transport cap belongs to server CLI, which currently has no persisted server configuration. No new protocol selector, TCP fallback, old-schema migration, or unnecessary GUI cap knob. Verify existing round-trip tests, rather than invent a config version requirement.

## Tests and smallest sequential units

Start ONLY after T3c passing commit.

1. T4a: feedback-driven pre-encode source admission + CLI cap + diagnostics, with focused tests and current usage documentation in the same coherent commit. Use tests/relay_server_test.cpp and tests/relay_recovery_test.cpp, current tests/relay_integration_fakes.hpp. Add deterministic clock-driven policy tests only if needed for stable timing (a small internal helper, not a framework). Cover loss deltas slowing raw admission, healthy recovery, wrong generation/regressed counters ignored, no repeated reduction from same cumulative sample, bounded latest source/output, low-cap oversized reason, HEVC sequence/reference recovery unchanged. The actual relay test must show feedback reaching and changing sender admission, not just a pure helper test. CLI invalid zero/overflow and --help should run without opening hardware. CMakeLists.txt only if registering new tests.

Commands:
```
cmake --preset macos-debug
cmake --build --preset macos-debug -j 8
ctest --preset macos-debug -R '^(relay_server|relay_client|relay_recovery|relay_wire|relay_session|udp_media|udp_kcp|config|input_router)$' --output-on-failure
ctest --preset macos-debug --output-on-failure
build/macos-debug/kvmux-relay --help
cmake --preset macos-release
cmake --build --preset macos-release -j 8
```
CLI added-option checks need final spelling before exact commands. No --serve test that can reach real capture/serial enumeration without a guaranteed early parse error.

2. T4b: native Linux/headless and isolated cross-host synthetic acceptance; update acceptance/build/design documentation and plan with actual results, then commit. Do not defer the passing T4a commit while waiting for remote availability. Native Linux commands (on Linux, not a forced host setting on macOS):
```
cmake --preset linux-debug-headless
cmake --build --preset linux-debug-headless -j 8
ctest --preset linux-debug-headless --output-on-failure
```
Backend ON/OFF choices must match real installed dependencies and be recorded. Current presets exist for these exact names; GUI OFF avoids SDL/ImGui/glad/OpenGL. No native Linux execution was performed in discovery.

## Cross-host synthetic reuse

/tmp/kvmux-hevc-e2e/probe.cpp and /tmp/kvmux-hevc-e2e-report.md exist. Old probe uses synthetic NV12 1920x1080, FakeSerial, actual RelayServer/RelayClient/VideoPipeline, two 120-frame sessions, server bounded 60s/client 15s per session. Old source/CMakeLists.txt appended kvmux_hevc_e2e_probe linked to kvmux::core. Report records jetson-hy / 192.168.14.32 and ports18700/18701, but availability and free ports are UNKNOWN now. Do not assume old builds/source trees contain UDP. Never execute old binaries as v3 evidence.

Rebuild against a fresh archive of final current tree; keep temporary probe source/CMake additions in an isolated /tmp source copy. Update video_presented(last) to video_presented(frame.generation, frame.sequence), as current API requires. Current server/client API handles UDP internally, so no second transport probe is needed. Inspect pipeline generation startup/restart against current relay_client_test before reuse. Add MJPEG mode using current FakeCapture and red16.jpg fixture, rather than claiming HEVC qualifies both. If using current integration fake, do not accidentally start an embedded loopback server in cross-host client mode. Reserve/check UDP ports, not TCP only; record fresh-generation reconnect and increasing local traffic totals (totals persist across restart). Keep synthetic capture/serial and bounded deadlines. No hardware capture, actual CH9329, user relay stop, tc/netem or network-wide interruption. Old /tmp/kvmux-reconnect-probe.cpp is a GL upload test, NOT a network reconnect fixture despite its filename.

## Stale documentation inventory

- README.md line13: unencrypted TCP claim.
- docs/lan-relay.md: lines7 dual TCP,124 TCP firewall,155 protocol v2,205-213 TCP queues/test claims,383 listening TCP wording,487 TCP framing/deadline explanation,514 traffic excludes retransmissions/TCP headers. Replace current-behavior claims with v3 two-server-UDP-port/one-client-socket, reliable KCP controls/raw freshness exceptions, bounded media/FEC, 250ms input vs10s session, recovery intent, transport cap units, current counters. Old historical failures around463 can remain explicitly historical, not portrayed as current.
- docs/acceptance.md: line14 obsolete dev preset, line84 old LAN TCP hardware/synthetic evidence. Preserve as dated prior evidence and add new UDP results; do not relabel old TCP logs as v3 acceptance.
- docs/building.md: presets already current; update relevant test/backend/native qualification sections only.
- docs/design/kvm-technical-design-v1.md: historical design rather than silently rewriting all v1. Link current v3 transport/state contract or clearly mark superseded transport sections if any; no need to rewrite unrelated capture/render design.
- docs/hardware-validation.md: preserve real-device limitations, add no unperformed hardware claims.
- Parent-owned docs/plans/2026-09-12-udp-kcp-relay.md: reconcile stale summary statuses and record precise T4 policy before implementation, then acceptance outcomes.

Unknowns: T3c final diff/test outcome, desired source-rate policy constants, current remote availability/dependencies/free UDP ports, effectiveness on real moving high-detail video, Apple hardware-session verification. Source tuning cannot fix permanently oversized frames; document rather than falsely assert useful video at arbitrary caps.

### T4a selected executable policy

Status: in_progress. T3c committed, final20/20 accepted. No parallel implementation.
Server source-admission interval starts at the capture mode's nominal interval.
Use one current-session feedback baseline, accept only currentgeneration and
nonregressing cumulative counters. Evaluate deltas at most once per500ms; pressure
is any positive lost/age/capacity/gap delta or waiting-IDR with no newly completed
frames. Repeated identical cumulative feedback cannot repeatedly reduce the rate.
On new pressure multiply interval by1.25 capped at max(nominal,200ms). After2s of
healthy new completions with no new pressure, reduce interval by10 percent no more
than once persecond, floored at nominal. Missing feedback for1s after initial
feedback: one reduction peroutage, no perpetual compounding without evidence;
reset outage latch on newer progress. This controls source admission only, never
codec output polling, raw challenges, KCP updates or user input. No bandwidth
estimator or claim of dynamic encoded bitrate is introduced.

Expose --transport-rate BYTES_PER_SECOND, positive uint64 in bounded practical
range1..1,000,000,000; rejectzero/overflow/malformed at parse before devices open.
Default remains12,000,000. Startup/help distinguish this UDP envelope+payload+FEC
cap from --bitrate encoder bits/s and excludeIP/UDP headers. Add current sender
snapshot for feedback/admission interval/rejectionreason, and receivermedia stats
in existingDiagnostics only; single-linebarunchanged. Use namedMediaReason labels.

Persistent frame_exceeds_rate_budget reports blocked reason, not restoredvideo.
Document lower --bitrate or higher --transport-rate followed by a new session;
no in-session encoderconfigure, sequence reset or hidden renumbering. Verify a
newsessionwithsuitablecap restoresfreshIDR after an intentionally impossiblecap.
This is not a promise to sustain arbitrary resolution/motion at arbitrarycap.

T4a owns relayserver/client snapshot+feedback/admission, relaymain CLI, mainGUI
Diagnostics, tests and relevant README/lan docs. Parent owns thisplan. May add a
small deterministic policy helper only if required for time-basedchecks. Existing
realrelaytest must show feedback actuallychanges admission; helpertestaloneisnot
acceptance. Run discoverycommands fullDebug/Release and CLI invalidchecks
`build/macos-debug/kvmux-relay --transport-rate 0` and overflow (expectnonzero
beforehardware). No realdevice/remote/push. Inspectcommitpassingunit immediately,
then T4b isolatednative/crosshost qualification. Include parentplanunchanged.

T4a completed and locally committed. Exact-tree macos-debug fullCTest20/20 and
macos-release all-target build passed. ActualHEVC/KCP feedback changes admission;
impossiblecap reportsblocked andnewsessionwithsuitablecap restoresfreshIDR.
CLI range/error handling andDiagnostics verified withoutopeninghardware. T4b now
in_progress: freshcommittedsnapshot isolatednativeLinux andcrosshostMJPEG/HEVC;
no remoteproduction changes, nohardwarecapture/serial, no push. Finalacceptance
andbuilding/design docs will recordactualresults aftertheprobe, not oldTCPevidence.

### T4b native failures and sequential correction

Baseline Linux ON/OFF builds passed; both CTest runs had18/19, solely FFmpeg58
HEVC drain failure. MJPEG cross-host two sessions120+120 passed. HEVC server
crashed in libtegrav4l2 during force-IDR while encoder negotiation was active.
These are separate failures; native acceptance remains incomplete.

Decoder correction is currently the sole implementation unit: require polling to
EAGAIN after accepted input before sending flush NULL; do not require pending
metadata empty and do not weaken EOF consistency. Original native decoder test
passed60+30 with this fix. Necessary test callers obey existing finish-again
contract; final related tests/commit pending.

Next correction, not yet executing: coalesce early Jetson keyframe requests until
hardware output establishes encoder readiness. Never emit force-IDR during initial
negotiation; first valid IDR can satisfy a pending startup refresh. If first output
is not IDR, defer force signal until readiness and preserve pending recovery request.
Reset/shutdown clear readiness and pending state. Native encoder regression requests
IDR immediately after configure and reset, preserves dynamic post-start IDR/GOP
checks, and actual cross-host HEVC must pass before accepting the fix. No sleeps or
arbitrary startup delay. Keep explicit backend failures observable.

## Final functional acceptance

All implementation units are committed. macOS20/20 andRelease passed; native
Linux ON/OFF19/19 passed after the FFmpeg58 drain correction. Jetson startup
force-IDR fix passed native60frames/dynamicIDR/reset. ActualMJPEG andHEVC UDP/KCP
Jetson-to-Mac each delivered120+120frames acrosssame-processreconnect. HEVC had
2/1 recovery events andno pipeline errors; probe followscapturegeneration on
recovery. Hardwareuse propertyforApple remainsunverified. See docs/acceptance.md
for measuredscope andlimits; /tmp/kvmux-v3-e2e-report.md contains temporarylogs.
No push or remoteproductionupdate performed. Only isolatedsynthetic hardware
encoder/decoder use, no camera/serial/userprocessinterruption. Final test processes exited normally; reserved UDP ports are free and no probe
process remains. No additional implementation is planned.

Completion: T1, T2, T3a, T3b, T3c, T4a and T4b are done. Earlier pending and
in_progress entries above are historical execution notes, superseded here.
Final native source verification found no mismatches in1766 committed files;
the isolated test CMake append was the only intentional difference. Both final
cross-host servers exited0; UDP18700/18701 and probe processes are clear.
