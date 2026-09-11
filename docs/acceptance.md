# KVMux v1 acceptance record

This record distinguishes source/build evidence from hardware evidence. A
software build, CI result, fake transport, camera device or UVC-like device is
not evidence that a CH9329 KVM hardware combination passed.

## Earlier local-KVM software evidence

The table below records earlier runs. Generic preset names in those historical
commands have since been replaced; use [current build instructions](building.md).

| Item | Status | Evidence |
|---|---|---|
| Debug configure/build | passed on macOS Apple Silicon | `cmake --preset dev && cmake --build --preset dev` |
| Debug CTest | passed on macOS Apple Silicon | 5/5: build info, CH9329 codec/queue, fake serial worker, InputRouter, video contract |
| Release bundle/install | passed on macOS Apple Silicon | Release install produced a staged `kvmux.app`; `fixup_bundle` verified it, `otool -L` found no Homebrew/user absolute references, and an ad-hoc `codesign --verify --deep --strict` passed |
| GUI startup/layout | passed on macOS Apple Silicon | SDL3/ImGui/OpenGL window started; device/serial controls, video area and status bar rendered |
| Linux adapter syntax | passed | `zig c++ -target x86_64-linux-gnu ... capture_v4l2.cpp` |
| Windows build | not run locally | Media Foundation source and CI job exist; no Windows toolchain evidence in this record |
| Linux build | not run locally | V4L2 source and CI job exist; no Ubuntu host evidence in this record |
| CI | configured, not yet observed | `.github/workflows/ci.yml` has macOS, Ubuntu and Windows jobs plus Linux sanitizers |

## Hardware and performance evidence

| Required item | Status |
|---|---|
| Selected LCC2003B capture device under KVMux | not run in this build record |
| CH343/CH9329 `GET_INFO`, USB-ready and clear reports | not run; CH343 is not enumerated on this Mac |
| CH9329 keyboard/mouse control | not run |
| Windows 11 x64 hardware matrix | not run |
| Ubuntu 24.04 X11 hardware matrix | not run |
| Ubuntu 24.04 Wayland hardware matrix | not run |
| macOS first grant/deny/permission recovery | not run |
| 1080p60 performance / p95 latency | not measured |
| one-hour / 100 connect-disconnect stability | not run |
| Developer ID signing and notarization | not performed; local staged bundle is ad-hoc signed only, and no release credentials are stored in this repository |

The current macOS device list can contain an iPhone Continuity Camera. That is
not a substitute for the UVC capture-card acceptance target.

## LAN relay follow-up evidence

- The user built and ran `linux-release-headless` on Jetson (`5.15.148-tegra`,
  aarch64). SSH inspection confirmed the relay listener and LCC2003B enumeration.
- The user supplied a remote GUI image containing target video, but also
  `VIDEO UNAVAILABLE`, `Capture: Fault` and a control-connection error. This
  demonstrates received video, not sustained streaming or working input.
- SSH inspection confirmed a `root:dialout` serial node and a running relay
  process without that group. CH9329 control was not accepted by this evidence.
- Automatic-selection and connection-fix checks passed 10/10 CTests under
  `macos-debug-headless`. The Mac GUI target built successfully.
- A local offscreen OpenGL probe checked all three YUVJ planar formats with
  repeated uploads: GPU paths, full-range fallback semantics, and texture-row
  orientation. These checks do not replace a visual retest of the remote GUI.
- End-to-end hardware control after the permission/render/connection fixes,
  sustained performance and Windows relay hardware remain unverified.

See [user-facing relay troubleshooting](lan-relay.md) for recovery steps.

## Subsequent user control check and renderer regression

The user reported successful control after restarting the updated applications.
The supplied Mac log shows Captured at 00:12:23.329 and release confirmed followed
by Preview at 00:12:41.496, with capture still Streaming. Jetson logs show normal
GET_INFO responses and USB-ready state. This establishes a reported successful
short hardware control session, not long-duration stability or a complete input
matrix. Earlier TCP send failures remain unexplained.

The reconnect green-screen defect was reproduced with real offscreen OpenGL:
the old renderer failed 28 of 60 uploads; the corrected renderer passed all 60,
including ImGui interleaving and texture reallocation cases. The macos-debug
build and 11/11 CTests passed. Real GUI reconnect still needs user confirmation.
The Windows Caps Lock initial-state question remains unresolved; the user
confirmed macOS Caps Lock input-source switching is enabled.

## HEVC relay and CPU fallback verification

The implementation passed 16/16 macos-debug CTests and a macos-release build.
Tests include codec framing, explicit software decode, libx265 encode/decode,
automatic fallback, IDR recovery and existing input/relay safety regressions.

On Jetson Orin NX (L4T R36.4.7, GCC 11.4, libavcodec 58.134.100 and libavutil
56.70.100), a temporary synthetic NV12 source drove the actual RelayServer and
NVIDIA encoder over LAN TCP to the Mac RelayClient, VideoToolbox decoder and
VideoPipeline. The same client object stopped and restarted between sessions;
each session produced 120 decoded 1920×1080 frames, with no decoder/pipeline
error or recovery request. The finite server exited normally. This was not a
capture-card test, a measured 60 fps throughput test or a latency benchmark.

The Mac produced `videotoolbox_vld` frames with `hw_frames_ctx=yes` and explicit
NV12 CPU transfer. The Apple session hardware property could not be verified;
Diagnostics correctly reports that uncertainty. This is not zero-copy evidence.

A native Jetson GUI-OFF, Jetson-backend-OFF, VideoToolbox-OFF build also passed.
The CPU path encoded and decoded 12 synthetic 64×64 frames with libx265, including
NV12/YUV420P input, metadata, EAGAIN, forced IDR, reset and EOS checks. It does not
establish CPU real-time 1080p60 performance. No camera, serial device, physical
input or user application was used by these probes. Other platform hardware
backends and real capture-to-display acceptance remain unverified.
