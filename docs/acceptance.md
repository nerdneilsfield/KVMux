# KVMux v1 acceptance record

This record distinguishes source/build evidence from hardware evidence. A
software build, CI result, fake transport, camera device or UVC-like device is
not evidence that a CH9329 KVM hardware combination passed.

## Software evidence

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
