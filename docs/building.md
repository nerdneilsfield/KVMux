# Building KVMux

KVMux uses C++20, CMake Presets, and Ninja. Configure does not download or
modify dependencies; the only exception is the windows presets, which build
FFmpeg from the vcpkg manifest (`vcpkg.json`) at configure time. Run these
commands from the repository root:

```sh
cmake --preset macos-debug
cmake --build --preset macos-debug
ctest --preset macos-debug
open build/macos-debug/kvmux.app
```

Use `macos-release` for an optimized macOS build. On macOS, use `macos-sanitizers`
to enable AddressSanitizer and UndefinedBehaviorSanitizer.

## Dependency policy

KVMux does not use Git submodules or Conan, and CMake does not fetch from the
network, with one Windows-only exception: the windows presets consume the root
vcpkg manifest (`vcpkg.json`) and build its declared dependencies at configure
time. This supersedes the design document's earlier rejection of vcpkg for
Windows only (decision recorded 2026-11-22). Source dependencies that are
practical to embed are pinned under `third_party/` with only the required
source, build integration, upstream version record, and license notices. CMake
does not fetch from the network anywhere else.

Large runtime libraries that are impractical to embed are discovered explicitly
from the build host. FFmpeg is currently required with `libavcodec`, `libavutil`,
and `libswscale`; the development build is verified with FFmpeg 9.0.1. On
Windows the FFmpeg version instead follows the manifest's pinned
`builtin-baseline`; record the version with each release. Their
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
ASan plus UBSan, which the current MSVC build does not support. The windows
presets pin `clang-cl` from LLVM found on `PATH` (for example `scoop install llvm`),
so configure never falls back to a bare `clang.exe` or GCC picked up from `PATH`.
`clang-cl` still compiles against the MSVC v143 toolset and a Windows SDK, and
linking still uses their libraries, so both must be installed and the build still
runs from an x64 MSVC developer shell. Delete any `build/windows-*` directory
created before this pinning; CMake caches the previously detected compiler.
No cross-compilation toolchain is implied.

Use the same name for configure, build and (where available) test:

```sh
cmake --preset linux-debug-headless
cmake --build --preset linux-debug-headless
ctest --preset linux-debug-headless
```

The former `dev`, `headless`, `release` and `sanitizers` names are removed.
Existing build directories are not migrated; configure the new preset once.

## Windows GUI and relay

Use an x64 MSVC developer shell, CMake 3.25 or newer, and Ninja; the presets
select `clang-cl`, so an LLVM build must be on `PATH` (for example
`scoop install llvm`). The windows presets also point the vcpkg toolchain at
`$env{VCPKG_ROOT}`: clone vcpkg from GitHub, run its `bootstrap-vcpkg` script,
and set `VCPKG_ROOT` to that checkout. Use a standalone git clone; the vcpkg
copy bundled under `VC\vcpkg` in Visual Studio may fail to resolve the
manifest's `builtin-baseline`. The manifest pins a vcpkg baseline, so the
FFmpeg port version is reproducible from `vcpkg.json`; record the FFmpeg
version shipped with each Windows release. Set `VCPKG_ROOT` before configuring:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug --parallel 2
ctest --preset windows-debug --output-on-failure
./build/windows-debug/kvmux.exe
```

The normal build produces both `kvmux.exe` (Local and Remote GUI) and
`kvmux-relay.exe`. Use `windows-debug-headless` to build only the relay and
non-GUI tests. The build copies FFmpeg, libserialport and, for GUI builds, SDL
DLLs alongside the executables. When the CH341 direct backend is compiled it
also copies `libusb-1.0.dll`, and a `COPYING` or `LICENSE` file from
`LIBUSB_ROOT` when that file is present. `cmake --install build/windows-release --prefix
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
  libusb-1.0-0-dev \
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
source/build information. The manifest enables the `gpl`, `x264` and `x265`
features of the vcpkg `ffmpeg` port, so the Windows FFmpeg DLLs are GPL builds;
see `third_party/INVENTORY.md` for what a distribution must carry.
The install tree is a development staging tree, not a signed release package or
a complete FFmpeg source offer. See `third_party/INVENTORY.md` before distribution.
Debug staging includes non-redistributable MSVC debug runtimes for local checks;
only use Release builds when preparing a distribution.

## CH341 direct serial

`KVMUX_CH341_USB` selects a libusb backend for a fixed set of WCH `1A86`
UART chips. A port name `ch341-usb:<bus>-<port path>` (for example
`ch341-usb:1-2.3`) uses that backend. Every other name stays on
libserialport. The option accepts only `AUTO`, `ON`, or `OFF`.

The GUI serial combo and `kvmux-relay --list-serial` both call
`enumerate_serial_ports()`. That function appends the libusb list to the
system ports. A debug log names every `1A86` device when that set or a
list/skip decision changes: VID, PID, `bcdDevice`, the first interface class,
and the reason. An unchanged scan is not logged again. No clipboard or typed
text is included.

| PID | Chip | Direct backend |
| --- | --- | --- |
| `7523` | CH340 | Listed. Same vendor-register sequence as CH341. |
| `7522` | CH340K | Listed. Same sequence as CH341. |
| `5523` | CH341 serial | Listed. Original sequence. |
| `55d3` | CH343 | Listed. Single UART, third-generation line request. |
| `55d4` | CH9102 | Listed. Same third-generation sequence. |
| `55d8` | CH9101 | Listed. Same third-generation sequence. |
| `55e9`, `55ea` | CH9111 | Listed. Same sequence for the nine CH9329 baud rates. |
| `55d2`, `55d5`, `55d7`, `55da`, `55df`, `55e8`, `55ec`, `55ef` | CH342, CH344, CH9103, CH347, CH9104, CH9114, CH346, CH9105 | Skipped. More than one UART. Use the system driver. `55d5` is CH344 and `55d7` is CH9103. |
| `55d6` | CH9143 | Skipped. Present in the WCH driver table and not opened here. Use the system driver. |
| `55db`, `55dd`, `55de`, `55e7`, `55eb`, `5610` | CH347, CH339, CH346, CH9433 | Skipped. The WCH driver binds a specific interface number. |
| any other `1A86` PID | — | Skipped. Protocol was not confirmed. |

CH343, CH9102, CH9101, and CH9111 follow protocol facts read from the WCH
`ch343` driver: 依据 WCH ch343 驱动确认的协议事实. Those four are
按官方驱动源码实现，未经硬件验证. CH340, CH340K, and CH341 reuse the
existing vendor-register sequence and are also unverified on hardware.

A skipped chip is not given a `ch341-usb:` name. Its system serial node, when
the OS created one, remains in the list. Opening a direct name for a skipped
chip fails with that chip's skip reason. None of these rows has a hardware
pass recorded in `docs/hardware-validation.md`.

Linux and Windows auto-select still ignore `ch341-usb:` names. macOS
auto-select is unchanged and still considers only `1a86:7523`, `1a86:5523`,
and `1a86:55d3`. A direct name for another PID is used only when it is
selected explicitly.

| Host | Default | When libusb is missing |
| --- | --- | --- |
| macOS | `ON` | Configure fails. |
| Linux, Windows | `AUTO` | Configure prints that the backend is not compiled and continues. |

`OFF` does not compile the backend and lists no `ch341-usb:` ports. Opening
that name then fails with `未编译 CH341 direct 支持`. `ON` fails configure
when the development files are missing. KVMux does not download or vendor
libusb.

Linux and macOS discover pkg-config module `libusb-1.0`. Use 1.0.27 or newer.
This tree was developed against 1.0.27. Ubuntu 24.04's `libusb-1.0-0-dev` is
1.0.27. On macOS, `brew install libusb` may install a newer release; install
`pkg-config` as well so CMake can see the module. Windows reads `LIBUSB_ROOT`
(a CMake variable or the same environment variable) as the official prebuilt
tree from
`libusb-1.0.27.7z`
(<https://github.com/libusb/libusb/releases/download/v1.0.27/libusb-1.0.27.7z>,
SHA-256 `19835e290f46fab6bd8ce4be6ab7dc5209f1c04bad177065df485e51dc4118c8`).
That tree must contain `include/libusb.h` or `include/libusb-1.0/libusb.h`,
and for an x64 MSVC build `VS2022/MS64/dll/libusb-1.0.dll` together with
`libusb-1.0.lib`. The official `.7z` does not contain the license text. Copy
`third_party/libusb-1.0.27/COPYING` (taken unchanged from the 1.0.27 source
tarball) to the root of `LIBUSB_ROOT`. The build then installs it beside the
DLL.

libusb is LGPL-2.1-or-later and is linked dynamically. KVMux does not modify
it. A binary distribution keeps that license notice and a corresponding-source
offer for the libusb version that was linked. See `third_party/INVENTORY.md`.

These platform prerequisites are documented here. They are not hardware
acceptance:

- Linux: opening `ch341-usb:` detaches the kernel driver bound to that device
  and does not fall back to a `/dev/tty*` node. The process needs permission
  to open the USB device. One udev rule that grants it for every WCH id is
  `SUBSYSTEM=="usb", ATTR{idVendor}=="1a86", MODE="0660", GROUP="plugdev"`.
- Windows: bind the interface to WinUSB with Zadig. A device left on another
  driver fails the open with `设备没有绑定 WinUSB，需用 Zadig 绑定`.
- macOS: leave the WCH vendor driver `CH34xVCPDriver` uninstalled. If that
  driver owns the interface, open fails with the busy or permission text and
  does not fall back to `/dev/cu.*`. Auto-select on macOS can choose a
  `ch341-usb:` entry. Linux and Windows auto-select keep the system serial
  port unless the `ch341-usb:` name is requested explicitly.

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
