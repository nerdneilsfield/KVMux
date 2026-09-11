# LAN relay implementation plan

## Outcome and authorization

Implement the user's requested split KVM: headless Windows or Linux relay owns
capture and CH9329; the existing desktop GUI receives video and sends input.
The user explicitly superseded design v1 section 12: use direct LAN TCP,
without SSH, TLS or authentication for this controlled network. Do not expose
this service to an untrusted network. No new networking dependency is needed.
Existing uncommitted work: tcp_socket.cpp/.hpp and their CMake source entry.
Protocol encoding already exists; a running relay/client does not yet exist.

## Decisions and contracts

- Separate video and control TCP connections. Default relay bind 0.0.0.0;
  default ports 17000 control and 17001 video. Client takes an IPv4 address.
- One controller. Pair video with the control session, reject unrelated video
  connections and do not silently transfer ownership to a second controller.
- Retain the KVMX versioned big-endian framing and bounded payloads. Never send
  CH9329 serial bytes over the network. Validate semantic input before submission.
- First runnable video path uses native MJPEG capture. Unsupported raw-only
  modes must report a clear error, not silently send raw frames as JPEG.
- Keep only the latest unsent video sample. TCP bytes already sent cannot be
  retracted: a bounded write deadline closes a slow connection rather than
  promising arbitrary queued-frame dropping. Separate TCP streams do not
  guarantee freedom from shared network congestion.
- Control messages carry session/epoch and increasing sequence. Reject stale
  events. Release bypasses queued input; client reconnection starts in Preview.
- A client heartbeat must reflect GUI progress and video freshness, not merely
  a running network thread. Relay timeout, video failure, control disconnect,
  overload and shutdown request ReleaseAll. Never claim confirmed release if
  the serial link failed. Local and remote clocks are not subtracted.
- Relay build must not require SDL, ImGui, OpenGL or a desktop session.

## Acceptance and ordered tasks

| ID | Requirement | Task | Check and expected result |
|---|---|---|---|
| R1 | Portable bounded TCP I/O | T1 | loopback CTest sends/receives actual bytes, detects EOF and expires a whole-operation deadline |
| R2 | Headless relay | T2 | headless configure/build; --help and device listing run without display libraries; paired fake client receives a JPEG |
| R3 | Safe remote input | T2/T3 | fake serial records release on heartbeat loss/disconnect; stale epoch does not emit HID input |
| R4 | GUI remote path | T3 | loopback test source appears through existing decoder/renderer; InputRouter sends events through network sink |
| R5 | Real two-host use | T4 | documented Windows/Linux relay and macOS client commands; record user-run hardware evidence separately |

### T1: TCP transport
Status: done (macOS software evidence; Windows/Linux runtime unverified)
Files: src/network/tcp_socket.hpp/.cpp, tests/tcp_socket_test.cpp (new), CMakeLists.txt.
Fix header self-containment, POSIX EINPROGRESS handling, Winsock types/linking,
whole-operation deadlines, EOF/zero writes. Keep socket ownership move-only.
Run from repository root: cmake --build --preset dev and ctest --preset dev -R tcp.
Commit this transport unit once actual loopback tests pass.

### T2: Headless relay
Status: done (macOS software verification); depends on T1.
Add src/app/relay_main.cpp and a small relay server implementation. Extend
packet types for session handshake, status, release and mouse mode. Split GUI
CMake dependencies behind KVMUX_BUILD_GUI; preserve the existing default GUI.
Enumerate native devices/modes and serial ports from CLI. Use existing native
CaptureSource and Ch9329ControlSink, not simulated production implementations.
Before execution, detail handshake/state packets and the paired-client test.

### T3: GUI network source and sink
Status: done (macOS build and loopback integration); depends on T2.
Implement network CaptureSource and ControlSink, adapt KvmSession's concrete
serial dependency only as required, and add remote connection controls to main.
Reuse VideoPipeline, renderer and InputRouter. Verify loopback JPEG display and
input/release using the same server protocol and a fake serial boundary.

### T4: Run instructions and evidence
Status: blocked on real two-host hardware evidence; instructions written; depends on T3.
Document actual relay CLI, GUI connection and firewall ports. Run available
local software checks and commit useful passing units promptly. Windows native
build and real capture/CH9329 tests require platform/hardware evidence; do not
mark them passed based on macOS tests or source inspection.

## Progress

T1 committed with actual loopback echo, EOF, refused connection and whole-operation
receive/send timeout checks. macOS dev CTest passed 7/7. T2 headless build and native device-list CLI are committed. macOS headless
build and CTest passed 7/7; build graph has no GUI dependencies. Real native
device/mode/serial listing ran on macOS. Windows/Linux are not yet verified.
T2 TCP server is now delegated; this is not yet a streaming server. Existing framing unit passed its focused test, but has
not yet been exercised across sockets. No network streaming completion claim.

## T2/T3 wire-session contract

Before server/client implementation, extend the existing packet enum with
status, release and mouse_mode. Keep encoding explicit, not native struct dumps.

- Control accept allocates a nonzero monotonically increasing session ID for
  this relay process. Server sends hello containing that u64 ID. This ID pairs
  channels; it is not authentication. Video client echoes hello with the ID.
- Each controller owns both channels. Video pairing has a bounded deadline.
  A second client cannot replace either channel of the active session.
- Control packet payload: session u64 followed by existing encoded ControlEvent
  (epoch, sequence, semantic payload). Epoch is the relay serial snapshot epoch;
  server rejects a mismatched session, epoch or non-increasing event sequence.
- Status returns session ID and serial snapshot: epoch, connection state,
  target USB ready, release confirmation and bounded error text. Client only
  arms when a current status and new decoded video are both available.
- Heartbeat (every 50 ms) contains session ID, epoch, client GUI-active flag,
  video-fresh flag and latest video sequence consumed. Relay expires the lease
  after 250 ms without a complete heartbeat. A live network thread must not
  fabricate heartbeat progress after GUI failure.
- Release contains session ID and is independent of the ordinary input queue.
  It immediately clears client readiness, invalidates server epoch and requests
  serial ReleaseAll. Server status acknowledges the new epoch and actual serial
  release result; a disconnected client reports release as unconfirmed.
- Mouse mode contains session ID and an absolute/relative byte. It uses the same
  release-before-change path; no input is accepted until new ready status.
- Client disconnect, heartbeat expiry, video socket failure or source stale
  for 500 ms ends the control lease, closes the paired channels and requests
  ReleaseAll. Reconnection creates a new session and never restores held input.
- Read header under a whole-packet deadline; check magic/version/type/length
  before allocating the payload. Any partial read failure discards the connection,
  not just the partial packet. Input packets have a small type-specific limit.
- Video worker owns its socket. It reads only the latest MJPEG source sample
  after each completed write; write failure/timeout invalidates the session.
  Control I/O never waits on the video write. Hardware capture keeps its existing
  owned capacity-one mailbox.

Implementation verification: paired loopback clients with a fake CaptureSource
and fake SerialIo must observe a complete JPEG; control disconnect and heartbeat
loss must produce serial clear reports; previous epoch input must not produce a
key report. These fakes are test boundaries, never production CLI substitutes.


## Latest integration evidence

The headless server and serial release-state fix are committed. Headless CTest
passed 8/8. Server checks cover real loopback JPEG/HID transfer with fake hardware,
stale epochs, disconnect, heartbeat loss, stale capture and rejected pairing.
The GUI/client compiles; the first combined dev CTest run passed 9/9. Client
review fixes and the actual client/server interoperation check are still pending
final validation. Do not count GUI visual or real two-host hardware as verified.


Final local software check: dev and headless builds succeeded, each CTest run
passed 9/9. relay_client includes actual RelayClient + RelayServer + VideoPipeline
with fake capture and serial hardware, not only a mocked TCP peer. Documentation
is in docs/lan-relay.md. GUI interaction/visual behavior and Windows/Linux native
relay hardware remain unverified. No push was performed.
