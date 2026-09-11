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
