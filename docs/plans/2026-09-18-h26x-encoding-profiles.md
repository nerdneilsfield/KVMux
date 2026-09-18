# H.264/H.265 encoding profiles implementation plan

Plan readiness: ready.

## Outcome and scope

For native RAW capture, the relay can encode and stream four explicit choices: H.264 quality-first, H.264 size-first, H.265 quality-first, and H.265 size-first. The client negotiates the selected codec and decodes either H.264 or H.265 using the chosen automatic/software/VideoToolbox backend. MJPEG pass-through remains available. The four profiles apply to relay streaming, not local MP4 recording.

Existing changes: none. Authorization: implementation requested.

## Design and constraints

Add `VideoCodec::h264` beside MJPEG and HEVC, and an encoder `EncodingPriority { quality, size }` in `CodecConfig`. A relay-side product selection maps to codec plus priority. Quality versus size is testable: quality uses the user bitrate and a faster low-delay preset; size uses a lower bounded target bitrate and a more compression-efficient low-delay preset. Both retain no B-frames, zero lookahead, one frame thread, closed GOP, repeated parameter sets, Annex B, and forced IDR. Pure CRF is excluded because unbounded access-unit size conflicts with transport pacing.

The software backend selects `libx264` or `libx265` from `CodecConfig::codec`. On Jetson, the NVIDIA hardware backend covers both codecs through JetPack NVENC GStreamer elements: `nvv4l2h264enc` and `nvv4l2h265enc`, with `nvvidconv`/NVMM NV12 upload. Auto tries this backend first for both H.264 and H.265, then falls back to FFmpeg only if backend creation or initial configuration fails. Explicit Jetson selection is strict and never falls back. The decoder selects `AV_CODEC_ID_H264` or `AV_CODEC_ID_HEVC` and validates codec-specific Annex-B recovery: H.264 IDR with SPS/PPS; HEVC IDR with VPS/SPS/PPS.

Generalize the existing 58-byte compressed AU body without changing its metadata layout. Negotiation allocates codec mask bit 4 and welcome value 2 for H.264. Protocol version moves from 4 to 5 because existing v4 peers reject these values; this unreleased protocol has no compatibility shim. UDP ordered recovery treats both inter-frame codecs identically and waits for a codec-valid IDR after loss.

CLI replaces the RAW choice with `--encoding h264-quality|h264-size|h265-quality|h265-size`; `--codec mjpeg` remains the pass-through choice. `--bitrate` is the quality-first target; size-first derives a documented lower target. Avoid conflicting codec/priority combinations. Diagnostics show negotiated codec, priority and effective bitrate. Decoder backend remains client-selected and codec is negotiated.

## Acceptance map

| ID | Behavior | Task | Check |
| --- | --- | --- | --- |
| C1 | H.264/H.265 and priority are explicit shared contracts | T1 | contract/wire tests pass for all codec values and profiles |
| C2 | RAW frames encode as H.264 or H.265 under both priorities | T2 | parameterized FFmpeg encode tests observe codec-valid IDR and diagnostics/options |
| C3 | Both codecs decode and recover after reset/loss | T2 | encode→decode round trips and malformed/recovery tests pass |
| C4 | Relay negotiates/routes H.264 safely | T3 | session, wire, UDP and relay focused tests pass |
| C5 | Operator can select four choices and see effective configuration | T3 | CLI parsing/help/diagnostics tests or direct command checks; docs updated |

## Execution

Follow counterweight Deep execution. Complete tasks in order, verify and commit each coherent slice.

### T1: Codec/profile and wire contracts

Status: done
Depends on: none
Acceptance: C1
Targets: codec contracts, relay wire/session, media AU wire, focused tests.

- [x] Add H.264 and priority types, generic Annex-B payload naming, protocol v5 negotiation.
- [x] Update contract/wire tests including malformed values and no-common-codec.
- [x] Build and run focused tests; inspect and commit.

Evidence: macOS Debug headless build passed; media_codec_wire, relay_wire, relay_session and udp_media passed 4/4.

### T2: Software encoding and decoding

Status: pending
Depends on: T1
Acceptance: C2, C3
Targets: FFmpeg encoder/decoder/factory and tests.

- [ ] Generalize libx264/libx265 configuration while preserving bounded low-latency invariants.
- [ ] Generalize Jetson NVENC (`nvv4l2h264enc`/`nvv4l2h265enc`) for both codecs and priorities with strict hardware diagnostics.
- [ ] Implement codec-specific IDR/parameter-set validation and decoder selection.
- [ ] Parameterize round-trip/recovery tests for available libraries and both priorities; inspect and commit.

Evidence: pending.

### T3: Relay integration and operator interface

Status: pending
Depends on: T2
Acceptance: C4, C5
Targets: relay selection/server/client/UDP, relay CLI, UI diagnostics, docs and focused tests.

- [ ] Route both inter-frame codecs through encoder, generic AU wire, recovery and decoder.
- [ ] Expose exactly four RAW encoding choices with effective bounded bitrate/preset diagnostics.
- [ ] Run the focused codec/network suite and a macOS build; record hardware validation as unverified unless real evidence exists; inspect and commit.

Evidence: pending.

## Final acceptance

Build the macOS GUI/headless codec targets and run codec factory, FFmpeg encoder/decoder, media wire, relay wire/session/selection/client, and UDP media tests. Software encode→decode proves both available codecs. Physical Jetson/VideoToolbox/network quality and size comparisons remain unverified without recorded hardware runs.
