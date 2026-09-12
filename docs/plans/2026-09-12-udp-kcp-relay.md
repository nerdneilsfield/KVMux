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
Status: in_progress (acceptance passed; diff inspection and commit pending). Depends on: T1. Acceptance: A2, A5.
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
