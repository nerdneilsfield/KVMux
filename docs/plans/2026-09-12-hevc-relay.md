# HEVC relay implementation plan

## Outcome and scope

Implement the authorized hardware HEVC relay alongside existing MJPEG. Follow
`docs/design/kvm-technical-design-v1.md` except the newer explicit request permits
common codec contracts and hardware backends. Raw capture already exists in
`src/video/capture_sample.hpp`; do not reimplement capture. No camera/HID use in
synthetic qualification, no plugin framework, no software fallback disguised as
hardware, and no copied Sunshine implementation. Initial Git worktree was clean.
Authorization: implementation, carried forward from the user. Track: Deep.

## Decisions and constraints

The owned CPU `AvFramePtr` is encoder input. Submission copies into owned backend
storage before returning. One complete owned Annex B access unit is output, with
HEVC codec, dimensions, nanosecond PTS, capture sequence, generation, arrival,
and IDR flag. Encoded sequence starts at1 on configure and survives reset. Parameter sets accompany IDRs. Shared contracts are in new
`src/video/codec/video_codec.hpp`. All calls have one owner thread; submit/poll
are bounded and nonblocking. `again` means unaccepted input or absent output.
No accepted compressed frame is dropped. Latest-value dropping is legal only
before encoding and after ordered decoding. Lifecycle calls can block.
Explicit backend selection fails clearly when unavailable. Per the later user
request, automatic selection tries hardware first and falls back to CPU encoding
or decoding with a reported reason. CPU HEVC encoding uses FFmpeg libx265 when
available; queue and latency bounds remain unchanged.
An explicitly selected ffmpeg_software decoder is allowed. finish signals EOS;
poll returns end_of_stream after all delayed outputs.
Jetson uses appsrc/nvvidconv/NVENC/appsink. Mac uses FFmpeg VideoToolbox and
explicit CPU readback. Reset discards a generation and restarts with IDR.
Evidence in `/tmp/kvmux-hevc-spike.md` proves synthetic CLI NVENC to native
VideoToolbox, not native encoder, capture latency, or application streaming.

## Acceptance map

| ID | Behavior | Task | Check and expected result |
|---|---|---|---|
| A1 | Bounded owned native encoder, metadata, dynamic IDR | T1 | Build standalone native test on jetson-hy; 60 synthetic AUs, monotonic matching PTS/sequence, requested IDR plus parameter sets, no camera/HID |
| A2 | Hardware decode and explicit CPU frames | T2 | Decode T1 stream on Mac; 60 frames, no B frames, retained CPU frames and matching metadata |
| A3 | Unsupported backend fails clearly | T1/T2/T3 | Explicit unavailable backend produces named error, never software fallback |
| A4 | HEVC ordered relay and recovery | T3 | Transport AU framing preserves metadata; stale progress/gap recovery starts new generation with IDR rather than dropping dependent AUs |
| A5 | MJPEG unchanged, selectable HEVC UI | T4 | Existing tests and MJPEG run pass; UI chooses supported backend, actual synthetic relay decodes |

## Execution

Run ready tasks in dependency order. Each coherent unit is checked, diff-inspected,
and locally committed. Parent owns integration; codec workers own disjoint files.

### T1: Native Jetson encoder and shared contracts
Status: done. Depends on: none. Acceptance: A1, A3.
Files: new `src/video/codec/video_codec.hpp`,
`src/video/codec/jetson_encoder.hpp`, `src/video/codec/jetson_encoder.cpp`,
`tests/jetson_encoder_test.cpp`.
- Define common configure/submit/poll/request_keyframe/reset/shutdown/backend contracts.
- Use capacity-bounded ordered pending metadata and appsrc/appsink; reject admission
  before overflow; validate raw CPU NV12/YUV420P planes and dimensions.
- Preserve PTS through GstBuffer and match output to admitted metadata.
- Request NVIDIA force-IDR and inspect complete byte-stream/alignment=au samples.
- Check using temporary independent CMake project referencing owned files and
  installed `pkg-config gstreamer-app-1.0 gstreamer-video-1.0 libavutil` on Jetson.
  Run finite 60-frame synthetic test under `timeout 30`; exact commands and output
  recorded with evidence. Do not edit shared CMake in this task.
- Inspect scoped diff and commit codec implementation with behavioral test.
Evidence: Native standalone C++20 GCC 11.4 build passed with `-Wall -Wextra
-Werror`, installed GStreamer app/video 1.20.1 and libavutil 56.70.100 on Jetson
Orin NX (aarch64 Linux 5.15.148-tegra, L4T R36.4.7). Commands, cwd local repo:
`scp /tmp/kvmux-native-encoder.tar jetson-hy:/tmp/kvmux-native-encoder.tar`, then
SSH to extract into `/tmp/kvmux-native-encoder`, run
`cmake -S /tmp/kvmux-native-encoder -B /tmp/kvmux-native-encoder/build -G Ninja`,
`cmake --build /tmp/kvmux-native-encoder/build`, and
`timeout 30 /tmp/kvmux-native-encoder/build/probe /tmp/kvmux-native-encoder/output.h265`.
Observed `frames=60 IDRs=2 pts=matched sequence=matched force_IDR=passed
reset=passed backend=jetson_gstreamer`. GOP600 and drained request at frame30
prove dynamic IDR; both IDRs contain VPS/SPS/PPS. Encoded sequence1..60 and
post-reset61 passed. Output copied to `/tmp/kvmux-native-encoder-output.h265`
for independent decoder check. No capture/camera/HID or running app touched.
This proves finite synthetic codec operation, not real capture latency.

### T2: Mac hardware decoder
Status: implemented; standalone software and VideoToolbox decode/reset passed. Depends on: T1 contract. Acceptance: A2, A3.
Create common-contract VideoToolbox backend. Details readiness-gated on finalized
header and owned test AU output. Parent assigns files and exact native test.

### T3: Codec selection and ordered relay
Status: in_progress. Depends on: T1/T2 contracts. Acceptance: A3, A4.
Server worker owns raw processing, NV12 conversion and encoder submit/poll. CLI
selects mjpeg (default) or hevc, auto/jetson encoder and bitrate in bits/s. HEVC
requires supported native/delivered raw capture, even dimensions; explicit modes
never fall back to MJPEG transcoding. Startup probes encoder availability and
configuration before opening listeners. Each session creates a new encoder and
uses its session ID as wire generation. Pairing echoes actual codec. Ordered AUs
carry encoded and capture sequences. A zero-byte send timeout requests IDR and
suppresses dependent AUs until IDR; partial packet send closes the session.
Same-generation keyframe requests trigger IDR on the video worker. Capture
freshness and existing CH9329 control leases remain independent of codec progress.
Check raw selection and rejection, MJPEG regression, synthetic raw/fake-encoder
server transport and recovery where deterministic. No production fake capture.
Build discovery remains parent-owned and platform-specific.

### T4: UI and application acceptance
Status: integration verification. Depends on: T3. Acceptance: A5.
Parent defines exact UI/config entry points after working relay contract. Preserve
MJPEG and existing safety release behavior. No unverified real latency claims.

## Final acceptance and handoff

Reuse native 60-frame encoder/decode evidence, then test actual relay framing and
existing MJPEG tests. Hardware camera/display acceptance is separate from synthetic
codec evidence. T1 native backend is verified; parent resumes T2/T3 integration; later details remain blocked until contracts and
runtime results are available, not on another permission request.

## Integration checkpoint

Common contracts, NVIDIA encoder, FFmpeg decoder, v2 codec handshake/AU framing,
server raw-to-HEVC path and client ordered decode are implemented. CMake provides
AUTO/ON/OFF backend switches. Software decoder and protocol fixtures are checked
in. GUI/config build passed; full macos-debug CTest passed 13/13 and Release built. Cross-machine
synthetic acceptance is in progress using temporary probes, separate test ports
and no hardware capture or input. Do not interpret this checkpoint as completion.

Remaining acceptance: complete project tests, real Jetson backend-enabled and
backend-disabled project builds, actual relay TCP to Mac decoded frames with
recovery, final Release build and user-facing startup/dependency instructions.
Real camera mode availability and physical input remain distinct hardware checks.

### Added requirement: CPU fallback

The user explicitly requested CPU encoder and decoder fallback. Add a common
FFmpeg libx265 encoder backend and automatic wrappers that try hardware, then
software during configuration. Explicit backend selection remains strict.
Verify force-IDR, drain/reset, bounded metadata, software round-trip and
hardware-disabled auto selection. Report the actual backend and fallback reason.
Document FFmpeg/libx265 availability and licensing. This task is required before
completion, not an optional follow-up.

User authorized local commit/push followed by Jetson fast-forward pull/build.
Do not edit remote production source or overwrite a dirty remote worktree.
Temporary isolated synthetic tests remain permitted and must not use camera/HID.

Latest local acceptance: macos-debug full CTest 15/15 passed after CPU encoder
and Auto fallback integration; macos-release build passed. Backend AUTO/OFF
factory tests verify software selection and a fallback reason when hardware is
unavailable. Jetson native backend-OFF build passed. Backend-ON qualification
found and fixed an old FFmpeg AVFrame API use and the missing GStreamer video
link dependency; native relink and cross-machine synthetic streaming remain in
progress. GitHub pull from Jetson timed out; local commits have been pushed, but
remote formal checkout has not yet been confirmed updated. No remote source edits.
