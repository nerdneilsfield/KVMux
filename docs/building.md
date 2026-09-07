# Building KVMux

KVMux uses C++20, CMake Presets, and Ninja. Configure does not download or
modify dependencies. Run these commands from the repository root:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/kvmux
```

Use `release` for an optimized build. On Clang and GCC hosts, use `sanitizers`
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
