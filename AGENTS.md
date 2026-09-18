# Counterweight

Use $counterweight by default for coding implementation and modification tasks.

Counterweight is the sole task workflow in this repository. Do not invoke or
combine it with engineering-change, planning, orchestration, or other workflow
skills unless the user explicitly names that additional skill.

## Project requirements

Implement KVMux according to `docs/design/kvm-technical-design-v1.md`. Treat that
document as the source of truth for product scope, data contracts, protocol
behavior, platform behavior, and acceptance criteria. If repository guidance
and the design differ, follow explicit newer user instructions and record the
decision in the relevant documentation.

### Build and dependencies

- Use C++20, CMake Presets, and Ninja. Keep platform selection in CMake and the
  platform adapters instead of scattering conditionals through business code.
- Do not use Git submodules, Conan, vcpkg, or configure-time network downloads.
- Vendor third-party libraries when practical under `third_party/`. Keep only
  the source and build integration needed by KVMux, pin the upstream version,
  and preserve each upstream license and required notices.
- For large runtime libraries that are not practical to vendor, such as
  FFmpeg or libserialport, use explicit CMake discovery and document supported
  versions, build options, runtime packaging, and license obligations.
- Keep Dear ImGui core and its SDL3/OpenGL3 backends at the same pinned version.
  Do not introduce Qt, Electron, Tauri, WebView, or another UI framework.

### Architecture and safety invariants

- Keep the module boundaries defined in the design: application/session,
  capture, video processing, rendering, input routing, CH9329 control, and
  support. Only `CaptureSource` and `ControlSink` are replaceable interfaces.
- Keep SDL events, ImGui, and all OpenGL work on the main thread. Keep capture,
  video processing, and serial work on their designated threads. Native capture
  callbacks may validate, copy, publish, and re-arm only.
- Driver-owned frame memory must be copied or retained by an explicit safe
  reference before it leaves a callback or dequeued buffer. Enforce the design
  limits for dimensions, payload sizes, strides, planes, and integer overflow.
- Use capacity-one latest-value mailboxes for samples and frames. Use a bounded,
  ordered control queue. Do not add hidden unbounded queues or per-frame tasks.
- Preserve keyboard and button edges. Merge only adjacent pure motion allowed
  by the design. `ReleaseAll` must bypass ordinary queue saturation and stale
  control epochs must never replay.
- The UI must never wait for capture or serial transactions. ACK timeout,
  overload, stale video, focus loss, device changes, sleep, and shutdown all
  use the common release path. Never report release as confirmed after the
  communication link is lost.
- Use SDL scancodes as physical USB keyboard usages. Do not route SDL text input
  as remote keystrokes. The only exception is an explicit opt-in, controlled
  ASCII simulated typing engine: it accepts US-layout printable ASCII plus Tab
  and Enter, starts only on an explicit user action, and sends paced, bounded
  events through the existing `InputRouter`/`ControlSink`. It must use the
  common cancel/release path and must not persist or log clipboard/text data.
  Unicode and Wubi input are out of scope. The Host key is local-only and must
  never reach the target.
- Keep platform capture code in the three native adapters: Media Foundation on
  Windows, V4L2 on Linux, and AVFoundation in Objective-C++ files on macOS.
  Shared video, input, control, and UI code must not depend on native handles.
- Do not implement excluded first-version features or speculative networking,
  plugin, media-graph, zero-copy, hardware-decoding, or second-renderer layers.

### Verification and delivery

- Put reusable non-UI logic in the internal static library and exercise it with
  CTest. Cover the protocol parser/codec, control ordering and recovery, input
  mapping/state, latest-frame behavior, video ownership, and lifecycle faults
  specified by the design.
- Provide Debug, Release, and supported sanitizer presets. A preset must use the
  Ninja generator and must not mutate or download dependencies at configure time.

- Use the root `Makefile` as the normal configure, build, test, format, and lint
  entry point. Let it select the host CMake preset; use `HEADLESS=1` only when a
  headless build is intended.
- Format first-party C, C++, and Objective-C++ with the repository
  `.clang-format`, which is based on Google style. After C++ changes, run
  `make lint` and the affected build/tests. Lint covers format checking,
  Cppcheck, and clang-tidy; fix first-party defects rather than hiding them with
  broad suppressions or changes under `third_party/`.
- Treat every remote source workspace as read-only. Without fresh authorization
  naming the exact remote mutation, do not upload files or patches and do not
  run commands that alter remote source, configuration, build outputs, the
  working tree, index, refs, or Git history. Remote inspection does not authorize
  synchronization; the user owns synchronization by default.
- Keep hardware-dependent checks separate from software simulation. Record the
  exact OS, machine, capture mode and format, serial rate, USB topology, and
  CH9329 configuration. Mark missing hardware evidence as unverified; never
  present fake-source or internal timestamp results as real KVM performance.
- Keep third-party notices and source/build-offer information with release
  artifacts. Do not claim a platform, package signing, notarization, latency,
  or hardware combination is accepted without recorded evidence.
