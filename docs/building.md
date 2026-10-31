# Building KVMux

KVMux uses C++20, CMake Presets, and Ninja. Configure does not download or
modify dependencies. Run these commands from the repository root:

```sh
cmake --preset macos-debug
cmake --build --preset macos-debug
ctest --preset macos-debug
open build/macos-debug/kvmux.app
```

Use `macos-release` for an optimized macOS build. On macOS, use `macos-sanitizers`
to enable AddressSanitizer and UndefinedBehaviorSanitizer.

## Dependency policy

KVMux does not use Git submodules, Conan, or vcpkg. Source dependencies that
are practical to embed are pinned under `third_party/` with only the required
source, build integration, upstream version record, and license notices. CMake
does not fetch from the network.

Large runtime libraries that are impractical to embed are discovered explicitly
from the build host. FFmpeg is currently required with `libavcodec`, `libavutil`,
and `libswscale`; the development build is verified with FFmpeg 9.0.1. Their
supported versions, feature configuration, runtime packaging, and license
obligations must be recorded when introduced. A release must ship the notices
and source/build-offer information required by each dependency.

## HEVC codec backends

FFmpeg remains required on every platform. Its software H.264/H.265 encoder and
decoder wrappers are always compiled. CPU encoding requires the runtime FFmpeg build to
provide `libx265`; no separate x265 development dependency is added by KVMux.
Automatic selection tries hardware first, then CPU if backend creation or initial
configuration fails, including a rejected first input before any input was accepted. Diagnostics report the selected backend and fallback reason.
Explicit hardware selections remain strict and never fall back. A running codec
is not replaced midstream; stream recovery must establish a new reference chain.
A hardware error reported by polling after input was accepted also requires stream
recovery, even if the decoder has not produced its first frame.

| CMake cache option | Default | AUTO behavior |
| --- | --- | --- |
| `KVMUX_JETSON_ENCODER` | `AUTO` | On Linux, compile when pkg-config finds GStreamer core, app, and video development packages; otherwise disable. |
| `KVMUX_VIDEOTOOLBOX` | `AUTO` | On Apple platforms, compile when VideoToolbox and CoreFoundation frameworks are found; otherwise disable. |

Both options accept only `AUTO`, `ON`, or `OFF`. `OFF` skips backend dependency
discovery. `ON` requires the supported platform and development dependencies and
fails configure with an error if they are missing. Jetson encoding requires Linux;
VideoToolbox decoding requires Apple frameworks. No separate preset is needed:

```sh
cmake --preset linux-debug-headless -DKVMUX_JETSON_ENCODER=ON
cmake --preset macos-debug -DKVMUX_VIDEOTOOLBOX=OFF
```

Configure checks compile dependencies only. It does not inspect device nodes,
probe hardware, or run GStreamer plugins. A normal Linux host with GStreamer core,
app, and video headers can compile the Jetson backend without NVIDIA runtime plugins.
An explicit Jetson encoder selection on that host fails with a runtime error.
Automatic selection uses CPU encoding instead if FFmpeg provides the requested
`libx264` or `libx265` encoder. A Jetson deployment needs the NVIDIA conversion
and matching `nvv4l2h264enc` or `nvv4l2h265enc` plugin provided
by its JetPack installation, plus the GStreamer runtime. Hardware backend support
must be verified on the deployed hardware; a successful build is not hardware
acceptance.

Software fixture decoding runs in CTest as `ffmpeg_decoder`. The `ffmpeg_encoder`
round-trip test covers both priorities for each available `libx264`/`libx265` encoder.
`codec_factory` checks automatic selection and strict explicit requests. Hardware checks are
manual and are not required by ordinary CI. On a Jetson build with tests enabled,
run `build/linux-debug-headless/kvmux_jetson_encoder_test output.h265` to exercise
the encoder. On a VideoToolbox-enabled macOS build, run:

```sh
build/macos-debug/kvmux_ffmpeg_decoder_test tests/data/hevc_1080p60.h265 videotoolbox
```

## Preset names

Use `<platform>-<configuration>`, optionally followed by `-headless`:

| Platform | Configurations | Example |
| --- | --- | --- |
| macos | debug, release, sanitizers | `macos-debug` |
| linux | debug, release, sanitizers | `linux-release-headless` |
| windows | debug, release | `windows-debug-headless` |

`-headless` excludes the GUI dependencies and builds the relay. Configure presets
are enabled only on their matching host OS. Each preset has its own build directory.
Debug and sanitizer presets build tests; release presets do not. Sanitizers means
ASan plus UBSan, which the current MSVC build does not support. Windows builds
require an MSVC developer shell. No cross-compilation toolchain is implied.

Use the same name for configure, build and (where available) test:

```sh
cmake --preset linux-debug-headless
cmake --build --preset linux-debug-headless
ctest --preset linux-debug-headless
```

The former `dev`, `headless`, `release` and `sanitizers` names are removed.
Existing build directories are not migrated; configure the new preset once.

## Windows GUI and relay

Use an x64 MSVC developer shell, CMake 3.25 or newer, and Ninja. Install an
FFmpeg **shared development SDK**, not just `ffmpeg.exe`: its root must contain
`include/libavcodec/avcodec.h`, MSVC-compatible import libraries in `lib/`, and
runtime DLLs in `bin/`. CI pins Gyan FFmpeg 8.0.1 full shared with a SHA-256 check.
Set `CMAKE_PREFIX_PATH` to that SDK root before configuring:

```powershell
$env:CMAKE_PREFIX_PATH = 'C:/deps/ffmpeg-8.0.1-full_build-shared'
cmake --preset windows-debug
cmake --build --preset windows-debug --parallel 2
ctest --preset windows-debug --output-on-failure
./build/windows-debug/kvmux.exe
```

The normal build produces both `kvmux.exe` (Local and Remote GUI) and
`kvmux-relay.exe`. Use `windows-debug-headless` to build only the relay and
non-GUI tests. The build copies FFmpeg, libserialport and, for GUI builds, SDL
DLLs alongside the executables. `cmake --install build/windows-release --prefix
stage` installs the executables, DLLs and branding assets; build the release
preset first. Keep the installed `bin` directory together when deploying.

Native capture uses Media Foundation; Windows N editions need their Media
Feature Pack, and camera privacy settings must allow desktop applications.
Windows Server installations also require the Media Foundation feature; a
headless relay build still links the native capture adapter.
The relay needs access to the selected COM port and inbound UDP 17000/17001.
These implementation paths still require Windows hardware acceptance.

## Linux GUI and relay

On Ubuntu 24.04, install the compiler, CMake, Ninja and GUI development libraries:

```sh
sudo apt-get install build-essential cmake ninja-build pkg-config \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
  libgl1-mesa-dev libegl1-mesa-dev libx11-dev libxext-dev libxcursor-dev \
  libxi-dev libxfixes-dev libxrandr-dev libxss-dev libxtst-dev \
  libwayland-dev wayland-protocols libxkbcommon-dev libdecor-0-dev
make dev JOBS=2
make test JOBS=2
build/linux-debug/kvmux
```

SDL selects X11 or Wayland for the current desktop session. To exercise each
backend explicitly, start with `SDL_VIDEO_DRIVER=x11` or
`SDL_VIDEO_DRIVER=wayland` in the corresponding desktop session. Both need an
OpenGL 3.2-capable driver. Headless relay builds use `make dev HEADLESS=1` and do
not need these display development packages. Local capture uses V4L2; the user
must have access to the selected video and serial devices.

After `make release`, install with `cmake --install build/linux-release --prefix
stage`. Installation includes the vendored SDL shared library and a relative
runtime search path, so the GUI does not depend on SDL being installed globally.
FFmpeg and system graphics libraries remain distribution runtime dependencies.
X11/Wayland input behavior and real capture/control still need platform acceptance.

Windows SDK DLL redistribution must retain the SDK's license notices and matching
source/build information; the full shared FFmpeg build includes GPL components.
The install tree is a development staging tree, not a signed release package or
a complete FFmpeg source offer. See `third_party/INVENTORY.md` before distribution.
Debug staging includes non-redistributable MSVC debug runtimes for local checks;
only use Release builds when preparing a distribution.

## UDP/KCP relay builds

Build both relay and GUI from the current source. Protocol v3 does not negotiate
with the former TCP relay. KCP is pinned under `third_party/kcp`; configuring the
project does not download it. The relay uses two server UDP ports, not TCP ports.
See [LAN relay usage](lan-relay.md) for firewall rules, `--transport-rate`, and
recovery diagnostics. Encoder `--bitrate` uses bits/s; the transport cap uses
bytes/s including the application envelope and parity, excluding UDP/IP headers.

`ctest --preset macos-debug --output-on-failure` includes the UDP/KCP components,
state synchronization and real loopback relay recovery with synthetic capture and
serial I/O. These tests do not open the user's capture card or CH9329 device.
Platform-specific and cross-host results are recorded in [acceptance.md](acceptance.md).
