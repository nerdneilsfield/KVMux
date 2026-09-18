# Region and scrolling screenshot implementation plan

Plan readiness: ready.

## Outcome and scope

KVMux lets the user drag a rectangle over the displayed video and save that source-video region. In scrolling mode, the user selects the region, activates remote input, and scrolls; KVMux uses wheel direction plus image overlap matching to append newly exposed rows into one long JPEG. Menus, overlays, pointer, and black bars remain excluded. Horizontal/panorama stitching and automatic scrolling are out of scope.

Existing changes: none. Authorization: implementation requested.

## Design and constraints

The UI owns a small local selection/capture state machine. Selection mouse edges are consumed before `InputRouter`, and are available only in Preview, so drawing a rectangle can never click the target. The displayed fit rectangle maps the normalized selection to integer source-frame bounds.

`Recording` remains the background media writer. A crop contract (`x`, `y`, `width`, `height`) is validated against each CPU `VideoFrame`; ordinary region snapshots convert only that crop and encode it. Scrolling capture retains a bounded current job rather than an unbounded frame queue. The main loop supplies the initial frame and then at most one fresh frame after each remote wheel event. Wheel sign provides expected vertical direction; image matching determines actual displacement.

The stitcher converts selected regions to packed RGB, compares several horizontal grayscale bands over candidate vertical overlaps, and accepts only consensus with low normalized error, negligible horizontal drift by construction, and a minimum overlap. Accepted downward movement appends only the new bottom rows. Unchanged/settling frames are ignored. Reverse movement is rejected for v1 rather than destructively editing already collected output. Width and region must stay stable. Output has explicit pixel/byte limits and fails cleanly instead of growing without bound. Worker file I/O and image processing never block the UI; GL remains main-thread-only and no GPU readback is added.

The UI states are idle -> selecting region -> ready/scrolling -> finishing -> idle. Escape cancels local selection/capture. Starting scrolling takes the initial frame. Each wheel event is still routed through the existing control path and marks the next fresh decoded frame for capture. Stop finalizes the accumulated JPEG. Focus/video/session loss cancels and uses the existing control release path.

Reference algorithms were studied from `Brkgng/ScrollSnap` (MIT, multi-band translation consensus) and `ericleong/scrollshot` (MIT, scroll-guided overlap/RMSE). No source is copied and no new third-party dependency is added.

## Acceptance map

| ID | Behavior or invariant | Task | Check and expected result |
| --- | --- | --- | --- |
| A1 | Drag-selected video region saves exact source dimensions without UI | T1 | `kvmux_recording_test` decodes output and verifies crop dimensions/pixels; manual overlay selects only inside fitted video |
| A2 | Selection input is local-only and invalid/tiny selections do not save | T1 | focused UI/helper tests and manual preview check |
| A3 | Downward wheel-driven frames become one correctly ordered long image | T2 | deterministic synthetic-frame stitch test verifies dimensions and row content |
| A4 | Unchanged, bad-match, reverse, stale, and oversized inputs do not corrupt/unbound output | T2 | stitch tests verify rejection/status and configured bounds |
| A5 | UI stays responsive and remote input safety is preserved | T2 | build/tests plus manual flow: select, activate, scroll, stop/cancel; worker owns processing and existing release path handles loss |

## Execution

Read this plan and `docs/design/kvm-technical-design-v1.md`; use counterweight Deep execution. Run tasks in order. Run/check, inspect, commit each coherent unit, and update this plan before the next task.

### T1: Region screenshot

Status: done
Depends on: none
Acceptance: A1, A2
Targets: `src/app/recording.hpp`, `src/app/recording.cpp`, `src/app/main.cpp`, `tests/recording_test.cpp`, `README.md`, `CMakeLists.txt` if a small reusable geometry helper is needed.
Contracts: crop bounds use source-video pixels and exclude display/UI coordinates.

- [x] Add validated crop-aware background JPEG encoding while preserving full-frame screenshot behavior where useful.
- [x] Add a Preview-only drag overlay mapped from fitted video coordinates to source pixels; consume its pointer edges locally and expose save/cancel actions.
- [x] Verify from repository root with `cmake --build --preset macos-debug --target kvmux_recording_test kvmux && ctest --test-dir build/macos-debug -R recording --output-on-failure`; expected crop dimensions/content and existing recording behavior pass.
- [x] Inspect and commit the coherent region screenshot slice.

Evidence: macOS Debug `kvmux` and `kvmux_recording_test` built; recording and input_router CTests passed. Crop JPEG decoded at 24x20; invalid crop was rejected. Manual GUI/hardware selection remains unverified.

### T2: Wheel-guided scrolling stitch

Status: done
Depends on: T1
Acceptance: A3, A4, A5
Targets: new internal stitcher files under `src/app/`, `src/app/recording.*`, `src/app/main.cpp`, focused tests, `README.md`, `CMakeLists.txt`.
Contracts: bounded latest requested frame; packed RGB crop; accepted append reports displacement; output width fixed and byte/pixel cap enforced.

- [x] Implement deterministic vertical overlap matching and bounded append/finalize behavior on the media worker.
- [x] Wire scrolling UI lifecycle to selected region, remote wheel events, fresh decoded frames, stop/cancel, and lifecycle loss without consuming remote wheel input.
- [x] Verify focused synthetic stitch tests, recording tests, build, and the existing input-router tests. Manually run the UI when hardware/video is available; otherwise record hardware behavior as unverified.
- [x] Inspect and commit the scrolling screenshot slice.

Evidence: macOS Debug stitcher, recording, input-router tests and `kvmux` build passed. Synthetic tests cover ordered append, unchanged/reverse/stale/bad-match rejection, fixed width, and pixel bounds. Physical target scrolling remains unverified.

## Final acceptance

From repository root, build `kvmux`, run the focused screenshot/stitch/recording/input tests, and inspect generated JPEG dimensions/content. Manual physical target scrolling remains explicitly unverified if no capture/control hardware is attached.

## Progress and handoff

Reference inspection and repository integration tracing are complete. T1 is next. No design blocker remains.
