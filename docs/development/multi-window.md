# Developing Multi-Window

This guide covers host support, validation, and the platform porting checklist for runtime multi-window creation.
It complements the ownership contract in [Multi-Window Design](../design/multi-window.md), which remains the normative reference for the API and invariants.

## Current status

| Platform | Runtime `OpenWindow` | Status |
|---|:---:|---|
| Windows | Supported | Implemented, built, and tested on the development host. |
| Linux | Implemented | Native coordinate and lifecycle validation belongs to Linux maintainers on supported Linux hosts. |
| macOS | Preview | Runtime-owned AppKit loop, per-window callback routing, ownership, modality, and retirement; validated by the macOS CI workflow, with interactive device validation still required. |
| Web | Throws `std::logic_error` | Sessions remain host-driven; no runtime creation entry exists yet. |
| Android, iOS | Throws `std::logic_error` | Platform-driven attachments by design; no runtime creation is planned. |

The unsupported-host error is the intended contract, not a regression: the shared `Runtime::CreateNativeWindow` default throws, and only hosts that override it gain the capability.

## Validated on the Windows host

MSVC Release, full build with examples and tests enabled.

| Suite | Result |
|---|---|
| `huxerui_runtime_tests` (includes the six multi-window cases) | 979 cases, 30731 assertions |
| `huxerui_unit_tests` | 177 cases, 11083 assertions |
| `huxerui_windows_tests` | 63 cases, 725 assertions |
| `huxerui_ui_tests` | 35 cases, 204 assertions |
| Codegen suites and header checks | Built clean |

The focused multi-window filter is `"MultiWindow*,RuntimeOpenWindow*,RetiringOneWindow*,ApplicationHandleOpenWindow*"` against `huxerui_runtime_tests`.
A real-HWND lifecycle test with the hidden `[win32-native]` tag opens and closes a runtime window twice, checks requested client origins with both system and custom chrome, and verifies that GDI/USER handle counts return near baseline; run it explicitly with `huxerui_windows_tests.exe [win32-native]` because it creates real top-level windows and Direct3D devices.
The Linux port has the mirror-image `[linux-native]` test (`LinuxRuntimeWindowsOpenAndCloseIndependently`): it opens and closes a runtime window twice against a real display, asserts the requested `initial_position` on platforms that honor placement, retires the runtime windows through the close path while the initial window stays alive, and finishes by closing the initial window to exercise whole-application shutdown; run it explicitly with `huxerui_linux_tests [linux-native]` because it creates real GTK toplevels.
The interactive demo is `example_multi_window`: the primary window opens a secondary one at runtime, both windows share one counter service, and each window keeps its own local state.

## Linux validation ownership

Linux maintainers validate the GTK adapter on supported Linux hosts; WSL is not a supported HuxerUI validation host and its results do not establish Linux compatibility.
The X11 placement origin, DPI conversion, and close-from-input lifecycle need Linux follow-up; the native placement test and its failure cleanup also need review.
The Linux changes in the macOS preview PR address CMake/mcpp X11 build dependencies, without claiming those native runtime issues are resolved.

## macOS preview validation

The `macOS Multi-Window Preview` workflow builds on GitHub-hosted macOS runners: arm64 with static linkage and Intel with shared linkage, both retaining the macOS 12 deployment target.
It builds common and codegen suites, AppKit platform suites, standalone headers, `example_multi_window`, and `example_window_relationship`.
The separate native test bundle creates real NSWindows on the main thread and exercises startup extras, runtime creation, state isolation, shared services, close interception, deferred keyboard closes, native close, failure cleanup, owner cascade, reparenting, modal stacking, and reopening after retiring every window.
Each CTest scenario runs in a fresh process because AppKit owns a process-wide NSApplication; weak native references check that retired windows and their content views are released after bounded native autorelease cleanup.

Run the same checks on a Mac with a normal CMake configure:

```bash
cmake --build build --parallel 3 --target huxerui_macos_tests huxerui_macos_native_tests example_multi_window example_window_relationship
ctest --test-dir build --output-on-failure -R '^HuxerUIMacOS(Tests|Native)'
```

CI results establish compilation and automated lifecycle behavior on the runner, not interactive validation on a physical Mac.
The native macOS workflow also runs on integration pushes to `farna/main`.
Traffic lights, native embedded controls, IME, accessibility, focus restoration, dragging, fullscreen, minimization, and multiple displays still need an interactive macOS pass before removing the preview qualification.
The Linux development host cannot build or execute AppKit, and no local macOS result is claimed.

## Behavioral notes

- Closing the initial window requests whole-application shutdown, preserving historical single-window behavior on every platform. Closing a runtime-opened window retires only that window.
- On Windows, a forwarded single-instance activation (URL or file) targets the visible initial window first, then the foreground window of the application class, then any visible window, so the surfaced target stays predictable while extra windows are open.
- Tray hibernation is an application policy, not a framework switch: applications intercept minimize through `WindowHandle::OnMinimizeRequest`, then either `Hide()` (window stays alive) or `Retire()` (releases the mounted tree, window services, and native resources while the Runtime keeps running), and reopen it from `SystemTrayHandle::OnActivate` with application services surviving on the Runtime; a tray menu Quit calls `ApplicationHandle::Quit()`. Retiring the initial window keeps the application running, so the reopen path uses the same `OpenWindow` entry as any other window; on hosts that do not support runtime window creation, `Retire()` reports the same capability gap.
- `WindowIdentity::ordinal` is assigned by `Runtime::OpenWindow`; callers that attach windows directly through `UiWindow::InitializeWindow` (embedded hosts and windowless tests) supply their own ordinal and are responsible for keeping it distinct.
- Window retirement releases the mounted tree, window services, text/pointer/file-drop/semantics subsystems, the committed damage snapshot, and the native resources of hosts that override `CreateNativeWindow`. On Windows, Direct3D/D2D/DWrite/DirectComposition resources are released through the renderer's `Discard`. Before any of that runs, `RetireWindow` verifies through `UiWindow::CanDiscardHost()` that the window and every initialized window in its owned chain support native discard, and rejects the whole retirement with the capability error when any of them does not; the rejection leaves every window fully intact.
- Closing a Windows window frees object memory immediately; the graphics driver returns the remaining working set asynchronously. This looks like a partial drop in Task Manager and is expected. If retention is ever suspected again, compare "Commit size" across repeated open/close cycles and use `EmptyWorkingSet` to prove that no object pins the memory before investigating further.
- Applying modal blocking on Windows calls `EnableWindow` on the covered windows. When a modal window opens from a click, the press still holds mouse capture, and disabling the capturing window synchronously delivers `WM_CANCELMODE` or `WM_CAPTURECHANGED`; the adapter defers the nested pointer cancellation until the in-flight pointer dispatch returns (see `WindowsUiWindow::SendPointer`). Delivering that cancellation immediately re-enters the shared pointer interaction and ends the active session while the outer dispatch still holds it, which corrupts the session map. Host ports whose grab-loss notifications arrive synchronously during input dispatch need an equivalent guard. The Linux port applies modality through `gtk_window_set_modal` and `gtk_window_set_transient_for`, which is window-manager mediated and produced no synchronous grab-loss during validation, so no deferred-cancellation guard was needed; revisit if a compositor is found to deliver one.
- GTK4 defers the native dispose of `gtk_window_destroy`, so the Linux close path for runtime-opened windows runs the shared teardown (owned chain, `Retire()`, native facility release) synchronously inside `CloseRequested` before calling `gtk_window_destroy`; the later `::destroy` emission only releases the remaining native references. Native facility release is guarded to run at most once per window because the IM client and platform views must detach while the widget tree is still alive.
- Window placement on Linux uses `XMoveWindow` after the native surface maps (`WindowOptions::initial_position`); GTK4 removed `gtk_window_move`. `WindowHandle::Position()` reports the client-area origin in logical screen coordinates through `XTranslateCoordinates` on X11 and `ClientToScreen` on Windows, and reports `std::nullopt` on Wayland where compositors own placement by protocol.
- When several modal windows overlap in coverage, the most recently established relationship wins: each window is blocked only by modals newer than every modal whose own owned chain contains it. Sibling modals of one owner therefore stack newest-on-top instead of disabling each other, application modals stack the same way, and retiring the top modal unblocks the previous one. Re-affirming an unchanged relationship keeps its place in the stack; windows opened through the ordinary `OpenWindow` overload join the blocking set on the common creation path.
- Running the test executables leaves an untracked `testing/Temporary/` directory next to the sources; it is a test artifact and is never committed.

## Porting checklist for additional hosts

Each port follows the Windows shape: keep one application event loop at the Runtime entry, let `CreateNativeWindow` construct and initialize the platform window, and retire runtime-opened windows independently of the initial one.

1. Hoist the per-window main loop to the Runtime owner and iterate until application shutdown, including when every window is hidden or explicitly retired.
2. Implement `CreateNativeWindow(const WindowOptions&, std::uint32_t)` on the platform Runtime subclass: construct the platform `UiWindow`, create the native window, call `InitializeWindow(runtime, options, ordinal, configuration)`, and return it without entering a nested loop.
3. Open the initial window and `AppOptions::extra_windows` through `Runtime::OpenWindow` in the platform entry so every window shares one path.
4. Route native callbacks from the handle or object that originated them instead of a single stored window pointer.
5. Define close behavior: the initial window keeps the platform's historical shutdown behavior; runtime-opened windows destroy only their own native window.
6. Build and run the common suites plus the platform tests on the host, then record the result in this document.

Host implementation anchors:

- Linux (ported): the per-window `GMainContext` iteration moved to `LinuxRuntime::RunApplication`, `LinuxRuntime::CreateNativeWindow` constructs `LinuxUiWindow` through `Attach` (which no longer enters a nested loop), the initial window opens through `Runtime::OpenWindow`, `CloseRequested` splits on the ordinal (initial window keeps whole-application shutdown, runtime windows retire independently with a synchronous shared teardown before the deferred GTK dispose), and `CanDiscardHost`/`DiscardHost` are overridden so `WindowHandle::Retire()` works. GTK signal handlers were already routed per instance through user data. X11 placement and screen-origin queries use the optional `x11` pkg-config dependency (`HUXERUI_LINUX_X11`).
- Web: `WebUiWindow` in `platform/web/web_adapter.cpp` already mounts one composition root per JavaScript session; runtime creation would expose a second session rather than a second in-page root.

## Rebuild and retest

Windows:

```powershell
cmake --build build\check --config Release --parallel --target example_multi_window huxerui_runtime_tests
build\check\bin\Release\huxerui_runtime_tests.exe "MultiWindow*,RuntimeOpenWindow*"
```

Linux and macOS hosts use the same targets after a normal configure; see [Building HuxerUI](building.md) for toolchain selection.
