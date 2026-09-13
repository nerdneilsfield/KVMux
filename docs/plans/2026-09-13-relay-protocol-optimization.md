# Relay protocol optimization plan

Plan readiness: draft. This plan follows the protocol review in `docs/design/protocol-simplify-design.md`. It describes the intended implementation order. It does not change the current protocol by itself.

## Goal

Keep the existing relay transport split: raw UDP for media/proof/cancel and KCP for reliable control. Clarify ownership and remove duplicated paste control flow without weakening tuple, media generation, serial epoch, input intent, serial ACK, or input freshness boundaries.

The target outcome is one deterministic lifecycle for each session, control intent, media proof and paste transaction.

## Fixed boundaries

- `tuple` identifies a relay connection.
- Media `generation` fences old media work.
- Serial `epoch` fences serial reset, release and recovery.
- Input `intent` fences user control ownership.
- Normal input remains `Sync → serial ACK-derived StateAck → Edge`.
- The serial worker owns HID report pacing and every CH9329 ACK.
- Video stale, media generation or actual-mode changes, Host, focus loss, minimize, release, disconnect, serial fault and owner changes cancel an active paste.
- KCP ACK does not prove serial execution.
- All queues and retained upload state remain bounded.

## Target ownership

| Area | Owner |
| --- | --- |
| Session tuple, intent cancellation, session expiry | `ServerSession` |
| Media packet pacing, held media datagram, sent-frame record | media sender / relay server |
| Proof validation | relay server with session state |
| Normal Sync, StateAck, Edge and Cancel | control session |
| Upload, transaction owner, terminal result | paste transaction state in relay session |
| Paste preparation, job start, HID ACK progress and release | serial worker |
| Text source, validation, user lifecycle | GUI / `InputRouter` |

## Work items

### 1. Stabilize current protocol semantics

1. Bind every paste transaction to its tuple, serial epoch and input intent.
2. Invalidate all nonterminal paste phases when that owner becomes invalid.
3. Make old raw or KCP Cancel affect only its own intent.
4. Make a transaction terminal result monotonic. Later progress, authorization replies or commit messages for the same transaction cannot overwrite or restart it.
5. Define one loop ordering for received controls, serial snapshots, cancellation, completion, lease renewal and deadline expiry. Stop old-session output after session destruction.
6. Keep held KCP output bounded without pausing raw cancel, proof processing, media pacing, media deadlines or refresh requests.
7. Move serial job admission decisions behind a worker-owned request/result boundary. Do not read worker-private transaction state from relay code.

Acceptance:

- Old intent Commit, Cancel and serial callback cannot affect a newer intent.
- One transaction produces at most one serial job and one monotonic terminal result.
- A `would_block` control socket does not stop media recovery or raw cancellation.
- Session expiry cannot access or publish through a destroyed KCP channel.

### 2. Simplify the paste transaction

Replace the public authorization token round trip with one execution-intent message:

```text
PasteBegin(tx, byte_count, crc32)
PasteChunk(tx, index, payload)*
PasteExecute(tx)
PasteCancel(tx, reason)
PasteKeepalive(tx)
PasteStatus(tx, phase, progress, outcome, reason)
```

The relay accepts `PasteExecute` as intent only. It starts a job only after upload validation, current owner/proof checks and worker preparation succeed.

Server phases become:

```text
Uploading → Uploaded → Preparing → Executing → Finished
```

`Finished` represents completed, canceled or failed results. `Uploaded` is distinct from final completion.

Remove `PasteAuthorize`, `PasteAuthorized`, token storage, authorization request retry state and the private high-bit revision fence after the worker-owned preparation path replaces them.

Acceptance:

- Duplicate Begin, Chunk, Execute and Cancel are idempotent within one transaction.
- A transaction cannot execute before validation, proof and worker preparation.
- No retry depends on an authorization response sequence number.
- GUI text uses no per-edge text pacing path.

### 3. Clarify media/proof and health projection

1. Put a media frame cursor, held datagram, send result and proof-eligible frame record under one sender owner.
2. Preserve the original receipt time of a deferred proof. Validate the receipt time only after its frame is eligible.
3. Keep GUI progress, presentation freshness, raw proof lease, paste execution lease and serial readiness as separate facts.
4. Derive control admission and paste start admission from those facts at their call sites; do not overload one `fresh()` or `stalled` value.

Acceptance:

- A proof received in time for a still-pacing frame is accepted only after the frame becomes eligible.
- A late proof remains invalid.
- HEVC refresh remains sendable while a paste transaction exists.
- A media-mode/generation transition cancels paste exactly once.

## Deterministic test seams

Add narrow seams for a monotonic fake clock, controlled UDP send result, KCP input/output and serial job events. Keep thread/socket integration tests as secondary coverage.

Required cases:

- session expiry with held output and pending paste status;
- owner change in every paste phase;
- old Cancel after a new paste begins;
- duplicate Execute and delayed terminal messages;
- terminal result followed by stale progress or failure status;
- keepalive and serial completion before, at and after deadline;
- `would_block` while the media socket remains writable;
- HEVC waiting-IDR during upload, preparation and execution;
- proof around final media send and proof deadline;
- partial serial preparation ACK, cancellation and delayed job snapshot;
- 960, 961 and 65,536 byte input, CRC/ASCII/chunk conflicts;
- exact HID report and ACK counts for a two-chunk 961-byte paste;
- paste completion followed by relative mouse input.

## Commit boundaries

1. Current-protocol ownership, terminal and loop-order fixes.
2. Worker-owned paste preparation and result correlation.
3. Paste wire simplification and deletion of token/fence flow.
4. Media sender/proof-health ownership cleanup.
5. Deterministic test seams and integration regression coverage.

Each unit requires focused tests before its local commit. Do not push without explicit instruction.
