# Chunked ASCII paste transaction implementation plan

Plan readiness: ready. The user authorized implementation of transaction chunks. This is protocol v4: replace the current unreleased edge-by-edge paste path directly. Do not retain v3 text-paste wire messages, fallback behavior, or compatibility negotiation. Local commits only; no push is authorized.

## Outcome and scope

A local or relay GUI can upload one validated US-ASCII paste as a bounded transaction and execute it only after a current input proof and synchronization fence. The input appears at the CH9329 as the same key reports in both paths. The UI can show upload/execution progress, cancel a pending transaction, and distinguish an upload failure from a denied or interrupted execution.

The source limit is **65,536 normalized source bytes**. Normalization is the existing complete-input US-ASCII validation: CRLF becomes one LF; TAB, LF, and printable ASCII are accepted; bare CR, other controls, and non-ASCII are rejected. The source text is not retained for diagnostics or logged. The current 1,024-character mapper limit is replaced as part of this feature; the new bound applies after normalization. A transaction maps each normalized byte to its existing US keyboard gesture when it becomes executable, rather than storing an unbounded edge list.

Out of scope: clipboard integration, keyboard-layout selection, Unicode/input methods, persistence or resuming across reconnect, multiple simultaneous uploads, encryption/authentication, changing the existing real-time keyboard/mouse protocol, and compatibility with protocol v3.

Repository evidence before this plan:

- `src/input/us_ascii_text.*` currently validates and maps all text before it creates gesture vectors, with a 1,024 normalized-character limit.
- `InputRouter` currently submits one synthetic edge only after `ControlSnapshot::completed_ordinary_sequence` reaches the submitted source sequence. The historical edge mode nevertheless produced reported paste scheduling/ACK problems. The current remediation commits are `fix(input): retain lease for ASCII paste`, `fix(input): diagnose ASCII paste scheduling`, `fix(input): drain ASCII paste edges on ACK`, and `fix(input): pace text edges by source ACK`; `fix(control): add safe stalled transaction diagnostics` is also current. These are observations and current commits, not proof that edge pacing is sufficient.
- `Ch9329ControlSink` owns the serial worker and reports completion after a HID ACK. `RelayServer` currently owns a concrete `Ch9329ControlSink`, while the relay client translates input through `wire::Edge`, a session challenge, a completed sync barrier, and current serial state.
- The present relay envelope rejects a wire version other than 3 in `src/network/relay_wire.cpp`. This plan changes it to v4 everywhere in this unreleased repository.

## Design and constraints

### Ownership and one transaction

`Ch9329ControlSink` gains one worker-owned composite paste job. It owns the normalized bytes, byte cursor, current gesture edges, and the normal serial transaction/ACK boundary. It never exposes text in a snapshot, error, or log. Only one composite job can exist; `Begin` replaces no existing job: it returns busy while a job exists. `Cancel`, `release_all`, disconnect, stale heartbeat, serial stall/hard failure, epoch change, or loss of execution authorization discard the job and follow the existing neutral-clear/recovery rules. A completed job releases its temporary input intent without adding an extra HID report.

Local `InputRouter` sends the source as a composite job to the sink. It does not expand it into `TextGesture` vectors and does not use the old synthetic text edge state. Existing special-key gestures and ordinary physical input remain real-time events. Starting a paste still requires Preview, fresh video, and a ready/released sink; physical key activity and the Host/focus/release paths cancel it as they do current synthetic text.

The relay client is an upload owner, not an executor. It may upload only while its v4 session is established. The relay server owns the assembled normalized bytes and is the only relay-side caller that starts the serial composite job. Upload is not execution permission. `Commit` is admitted only while the exact current server session tuple, current challenge/proof lease, current serial-ready snapshot, and completed `Sync` barrier are all valid. The server gives the serial worker the fence identity (serial epoch, intent, and completed sync revision); the worker checks it before each next HID report and after each ACK. Losing any fence cancels/revokes rather than continuing text.

This deliberately makes upload tolerant of a momentary lack of proof but makes execution conservative. A client must obtain a new proof and synchronization after a denied/expired commit; it does not replay a past commit automatically.

### Wire v4 composite contract

Add these reliable KCP control variants in `src/network/relay_wire.*`; encoded integer fields are big-endian. All message bodies use the existing four-byte control header (type, reserved zero byte, u16 body size). They are direction-specific and rejected on the wrong direction, malformed reserved bytes, trailing bytes, invalid enum values, zero IDs, invalid sizes, invalid session association, or an impossible state transition. `transaction_id` is a nonzero client-chosen u64, unique among that client session's live transaction; it is reset on a new session.

| Message | Direction | Exact body and contract |
| --- | --- | --- |
| `PasteBegin` | client → server | `transaction_id:u64`, `normalized_bytes:u32`, `crc32:u32`. `1 <= normalized_bytes <= 65536`. CRC is standard CRC-32/IEEE of the normalized source bytes, used only to detect accidental corruption; it is not authentication or a security boundary. Requires an established tuple/session, but no challenge or execution lease. On acceptance allocate the sole server upload, set `next_chunk=0`, and start a 30-second upload deadline. |
| `PasteChunk` | client → server | `transaction_id:u64`, `chunk_index:u32`, `payload_bytes:u16`, `reserved:u16=0`, `payload[payload_bytes]`. `1 <= payload_bytes <= 960`. Chunks are strict contiguous indexes starting at zero. A chunk whose index equals `next_chunk` must fit exactly within the declared source length and is appended, then advances `next_chunk`. A duplicate of an already accepted index is idempotent only if its stored byte range and payload are byte-for-byte equal; it returns the same status and does not append. A future index, a conflicting duplicate, an overrun, or a chunk after all declared bytes is rejected and cancels the upload. |
| `PasteCommit` | client → server | `transaction_id:u64`. It has no source payload. It succeeds only if exactly `normalized_bytes` were assembled, CRC32 matches, and the current tuple/session, proof lease, serial-ready state, epoch/intent, and completed sync barrier are valid at admission. The server starts the serial job with that fence and changes the transaction to executing. Otherwise it leaves a complete upload available only until its normal deadline and returns the precise status; it never queues execution for later proof. |
| `PasteCancel` | client → server | `transaction_id:u64`, `reason:u8`, `reserved[7]=0`. Reasons are `user=1`, `host=2`, `focus=3`, `release=4`, `disconnect=5`. It is idempotent: an unknown, expired, canceled, completed, or previously canceled ID receives the terminal status if still known, otherwise no state is created. A cancel for an executing ID makes the server revoke its serial job and neutralize through the existing release path. |
| `PasteStatus` | server → client | `transaction_id:u64`, `state:u8`, `reserved[3]=0`, `next_chunk:u32`, `accepted_bytes:u32`, `completed_bytes:u32`, `reason:u8`, `reserved[3]=0`. States: `uploading=1`, `complete=2`, `executing=3`, `completed=4`, `canceled=5`, `rejected=6`, `expired=7`. Reasons: `none=0`, `busy=1`, `invalid=2`, `conflict=3`, `checksum=4`, `deadline=5`, `session=6`, `proof=7`, `fence=8`, `serial=9`, `canceled=10`. `next_chunk` and `accepted_bytes` describe the accepted prefix; `completed_bytes` is only the ACK-confirmed source-byte count while executing/completed. Status is sent after every accepted Begin/Chunk/Commit/Cancel transition and whenever execution makes progress or reaches a terminal state. It never contains source bytes. |

The client divides normalized bytes into 960-byte maximum payloads and may retransmit a chunk after a status/reliable-delivery uncertainty. It advances its upload cursor only from `PasteStatus`, not send completion. The server retains the accepted byte prefix for idempotence until terminal removal. All nonterminal uploads expire 30 seconds after accepted Begin, including complete-but-uncommitted uploads. `tick()` drives expiry; expiry frees bytes, marks the terminal result, and emits `PasteStatus{expired, deadline}` while the session can receive it. The bounded retained source is at most 65,536 bytes plus small metadata.

### Real-time and composite scheduling

The serial worker classifies `KeyEdge`, pointer/button, relative movement, wheel, sync, clear, information, and configuration reports as **real-time** work. A paste byte/gesture is **composite** work. Real-time work has priority at every safe report boundary. The composite job submits at most one CH9329 report at a time and advances its source-byte cursor only after the final ACK for that byte's mapped gesture. A real-time event admitted while composite execution is active does not interleave a different held-key state into a paste gesture: the worker completes the current single report, processes the real-time item, then resumes from the same composite edge. Release/cancellation wins over both and clears the job.

Direct and relay use this one scheduler and this one ACK definition. The relay must not recreate a second edge loop, synthesize completion from KCP delivery, or treat KCP ordering as serial completion. `ordinary_input_pending` and `completed_ordinary_sequence` remain the real-time edge diagnostic contract; composite progress uses the paste status contract instead.

### Session and fence invariants

The existing relay acceptance gates remain authoritative for real-time `Sync` and `Edge`: tuple match, issued current proof, proof age/lease, current intent, cancellation floor, serial ready snapshot, and sync barrier. A composite upload requires only session ownership because it does not actuate input. `PasteCommit` additionally requires `ServerSession` to expose/check the same current proof and barrier state used for `Edge`, plus identity of the accepted epoch/intent/revision. The server calls the serial job only after that check. The job receives immutable fence values and stops if `ControlSnapshot` no longer has the same ready epoch, active intent, and applied sync state. A fresh proof cannot revive an already canceled job; a new transaction is required.

No retry is added for unknown CH9329 ACK state. Serial failure changes the transaction terminal reason to `serial`, follows the existing release/reconnect behavior, and requires a new upload after recovery. The deadline, CRC, and IDs are correctness/bounds mechanisms, not access control.

## Acceptance map

| ID | Behavior or invariant | Task | Check and expected result |
| --- | --- | --- | --- |
| A1 | Normalized source accepts current allowed ASCII/CRLF rules and rejects invalid input; exactly 65,536 normalized bytes is accepted and 65,537 is rejected without retaining/logging text. | T1 | Focused text/control test maps boundary data lazily and observes no source in snapshots/errors. |
| A2 | Direct serial composite job emits mapped HID reports in order, advances completed bytes only after relevant ACKs, and preserves real-time priority without corrupting held state. | T1 | Fake serial test delays ACKs, injects a real-time key/button between composite reports, and observes ordered reports and ACK-derived progress. |
| A3 | Release, focus/host cancellation, heartbeat/epoch fence loss, and serial stall cancel composite execution and issue existing neutral behavior; no later source byte is emitted. | T1/T3 | Fake serial timeout/fence tests inspect reports after cancellation and terminal status. |
| A4 | v4 wire has exact Begin/Chunk/Commit/Cancel/Status validation, 960-byte chunks, strict contiguous acceptance, safe idempotent duplicates, and no v3 decode. | T2 | `relay_wire` vectors round-trip valid values and reject wrong direction, malformed fields, >960 payload, gaps, conflicts, and v3 envelope. |
| A5 | Upload needs an established session but may finish without a proof; it has one 30-second bounded deadline and CRC failure never executes. | T2/T3 | Deterministic session/server tests advance time and inspect statuses, retained-prefix behavior, deadline expiry, and no serial admission on bad CRC. |
| A6 | Commit executes only with current tuple/proof/serial-ready/sync fences; stale or lost proof/fence cancels rather than later replaying text. | T3 | Session and relay fake tests invalidate each fence before/during commit and observe rejected/canceled status, release, and no post-fence report. |
| A7 | Local and relay UI use the transaction path and expose non-secret progress/cancel/terminal state; old edge text scheduling is removed. | T4 | Input router local fake and relay loopback acceptance paste multi-chunk text, observe progress to completed, and confirm physical key cancels. |
| A8 | The assembled v4 feature builds and the current focused test set remains green. | T4 | Run the stated CMake build and focused CTest command from repository root; all selected tests pass. |

## Execution

Read this plan, `docs/plans/2026-09-12-udp-kcp-relay.md`, current Git state, and Counterweight Deep execution guidance before changing a task state. Execute in order. For each task: mark in progress, implement only its scope, run its acceptance, inspect the diff, commit the passing coherent unit, and record actual evidence below. Do not push.

### T1: Add the shared serial composite paste job

Status: pending
Depends on: none
Acceptance: A1, A2, A3
Targets: `src/input/us_ascii_text.{hpp,cpp}`, `src/control/control_sink.hpp`, `src/control/serial_worker.{hpp,cpp}`, `tests/us_ascii_text_test.cpp`, `tests/serial_worker_test.cpp`, and any directly required local InputRouter declarations.
Contracts: Own the normalized source in the sink worker; expose metadata-only job progress/result to the caller. Preserve current `ControlSink` real-time event and synchronization semantics.

- [ ] Replace eager text-gesture storage for paste with bounded normalized source validation/mapping support. Keep existing per-byte US mapping semantics and define the 65,536-byte post-normalization limit.
- [ ] Add a single composite job API and metadata-only result/progress shape to the control boundary. Implement it in `Ch9329ControlSink` using the worker's existing ACK, clear, epoch, heartbeat, and error paths.
- [ ] Add real-time/composite priority at report boundaries. A release/cancel and all existing fault paths discard the composite job; no source content reaches diagnostics.
- [ ] Verify from repository root: `cmake --build --preset macos-debug --target kvmux_us_ascii_text_test kvmux_serial_worker_test && ctest --preset macos-debug -R '^(us_ascii_text|serial_worker)$' --output-on-failure`. Extend the named tests with the boundary, delayed-ACK, priority, and cancellation cases above. Expected: both tests pass and report only metadata.
- [ ] Inspect the diff for retained old text-edge plumbing in the direct path and unintended changes to real-time input.
- [ ] Commit the shared mapper/serial job and focused tests as one coherent unit, proposed subject: `feat(control): add bounded composite ASCII paste job`.

Evidence: not run.

### T2: Define protocol v4 and upload state machine

Status: pending
Depends on: T1
Acceptance: A4, A5
Targets: `src/network/relay_wire.{hpp,cpp}`, `src/network/relay_session.{hpp,cpp}`, `tests/relay_wire_test.cpp`, `tests/relay_session_test.cpp`.
Contracts: Replace envelope version 3 with 4 and add the exact tabled control messages/statuses. `ServerSession` owns no source payload; it owns only the session/proof/fence admission decisions. The relay server will own the bounded bytes in T3.

- [ ] Encode/decode the exact v4 messages and enums, enforce 960 payload bytes and direction rules, and reject v3 directly.
- [ ] Add a deterministic single-upload transaction state machine with Begin deadline, contiguous index/prefix accounting inputs, status transitions, and Commit fence query. Keep byte storage out of `ServerSession` if a separate relay-server upload owner is simpler; expose only the checks/state needed by T3.
- [ ] Verify from repository root: `cmake --build --preset macos-debug --target kvmux_relay_wire_test kvmux_relay_session_test && ctest --preset macos-debug -R '^(relay_wire|relay_session)$' --output-on-failure`. Expected: valid vectors pass; protocol v3, invalid fields, wrong directions, expired upload and unavailable fence cases are rejected deterministically.
- [ ] Inspect the diff for compatibility decoding or a hidden execution path on upload acceptance.
- [ ] Commit protocol/state-machine change and tests, proposed subject: `feat(relay): add v4 paste upload protocol`.

Evidence: not run.

### T3: Wire upload/commit to the relay serial owner

Status: pending
Depends on: T1, T2
Acceptance: A3, A5, A6
Targets: `src/network/relay_server.{hpp,cpp}`, `src/network/relay_client.{hpp,cpp}`, `src/network/relay_session.{hpp,cpp}` only if T2 needs its concrete admission API, `tests/relay_server_test.cpp`, `tests/relay_recovery_test.cpp`, and targeted fakes.
Contracts: Server retains at most one 65,536-byte upload for 30 seconds; client uploads 960-byte chunks and uses status as its cursor/progress authority. Server alone starts/cancels the serial job with a current proof/barrier fence.

- [ ] Route KCP PasteBegin/Chunk/Commit/Cancel/Status. Implement source retention, byte-for-byte duplicate comparison, CRC32/length completion, expiry, terminal status, and cleanup in the server owner thread.
- [ ] Make client upload state replace its old relay synthetic text edge queue. It may retry status-safe chunks but must not auto-commit or auto-replay after a failed proof/fence.
- [ ] Couple server commit and each serial job progress/cancel callback to the current `ServerSession` lease and completed sync identity. Revoke paths, disconnect, and serial failure send safe terminal status when the tuple remains live.
- [ ] Verify from repository root: `cmake --build --preset macos-debug --target kvmux_relay_server_test kvmux_relay_recovery_test kvmux_relay_session_test && ctest --preset macos-debug -R '^(relay_server|relay_recovery|relay_session)$' --output-on-failure`. Add deterministic fake/loopback cases for multi-chunk completion, duplicate retransmission, gap/conflict, checksum/deadline, denied commit, and proof/fence loss mid-job. Expected: only valid committed source reaches fake serial; each loss ends neutral and terminal.
- [ ] Inspect the diff for concurrent access to source bytes, KCP-delivery-as-ACK assumptions, and a second relay scheduler.
- [ ] Commit server/client transaction integration and focused tests, proposed subject: `feat(relay): execute fenced chunked ASCII paste`.

Evidence: not run.

### T4: Replace UI/router edge paste and run assembled acceptance

Status: pending
Depends on: T1, T2, T3
Acceptance: A7, A8
Targets: `src/input/input_router.{hpp,cpp}`, `src/app/main.cpp` and existing paste UI/status code found there, `src/network/relay_client.{hpp,cpp}`, `tests/input_router_test.cpp`, relevant relay tests, and only necessary user-facing documentation.
Contracts: UI metadata must use `PasteStatus`/local job progress, not source text or per-edge ACK sequence. Existing special key behavior remains separate. Cancel is available while uploading/executing and physical keyboard input cancels a paste.

- [ ] Replace the current `TextGesture`/`text_edge_inflight_` scheduling path with local/relay transaction submission and progress polling. Remove obsolete v3 edge-paste behavior rather than retaining fallback branches.
- [ ] Update the existing paste controls/status wording to show accepted bytes, completed bytes, active/terminal reason, and cancel. Do not expose typed content.
- [ ] Verify from repository root: `cmake --build --preset macos-debug && ctest --preset macos-debug -R '^(us_ascii_text|serial_worker|input_router|relay_wire|relay_session|relay_server|relay_recovery|relay_client)$' --output-on-failure`. Expected: build succeeds; focused tests pass; local and loopback relay multi-chunk paste reaches completed, cancellation stops it, and no old protocol path is accepted.
- [ ] Manually run `open build/macos-debug/kvmux.app` after the build when the GUI environment is available. Connect only to existing fake/controlled hardware; start a short allowed ASCII paste, observe metadata progress/completion, then cancel a second paste. Record unavailable hardware as unavailable rather than inferred.
- [ ] Inspect the final diff for source logging, retained v3 handling, incompatible UI claims, and scope creep.
- [ ] Commit UI/router migration and acceptance updates, proposed subject: `feat(input): use chunked ASCII paste transactions`.

Evidence: not run.

## Final acceptance

From repository root, after T4, run:

```sh
cmake --build --preset macos-debug
ctest --preset macos-debug -R '^(us_ascii_text|serial_worker|input_router|relay_wire|relay_session|relay_server|relay_recovery|relay_client)$' --output-on-failure
```

The result must show all selected tests passing. The test evidence must cover a 960-byte chunk boundary, a 65,536-byte normalized source boundary, contiguous/idempotent upload behavior, the 30-second deadline, CRC rejection, commit fence denial/loss, serial ACK progress, direct/relay parity, cancellation, and no post-cancel HID input. Native CH9329 and two-host GUI proof remains hardware-dependent and must not be claimed from fake serial/loopback checks.

## Progress and handoff

Last completed task: planning only.
Next ready task: T1.
Blockers: none.
The worktree contains pre-existing non-plan changes in `src/control/control_sink.hpp`, `src/control/serial_worker.{hpp,cpp}`, `tests/serial_worker_test.cpp`, and new `src/control/ascii_paste_job.hpp`; they are outside this plan-only task and must not be staged with this document. This document is the only change made by this task; its commit records the ready plan, not feature implementation.
