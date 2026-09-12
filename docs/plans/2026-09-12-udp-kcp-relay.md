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
Status: pending. Depends on: D1. Acceptance: A1.
Deliver a real loopback-capable transport with owned datagrams, pinned KCP,
message/queue bounds and typed receive/liveness outcomes. Include tests and
inventory/build registration in the coherent commit. Exact files/contracts/check
commands will be specified before readiness, not delegated as an open-ended layer.

### T2: UDP media delivery and reference recovery
Status: pending. Depends on: T1. Acceptance: A2, A5.
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
