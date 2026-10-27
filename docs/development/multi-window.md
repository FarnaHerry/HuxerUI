# Developing Multi-Window

This guide covers host support, validation, and the platform porting checklist for runtime multi-window creation.
It complements the ownership contract in [Multi-Window Design](../design/multi-window.md), which remains the normative reference for the API and invariants.

## Current status

| Platform | `ApplicationHandle::OpenWindow` | Status |
|---|:---:|---|
| Windows | Supported | Win32 backend; native compilation and execution require a Windows host. |
| Linux | Supported | GTK backend with native lifecycle, retirement, floating-window, and coordinate checks on Linux. |
| macOS | Preview | Runtime-owned AppKit loop, per-window routing, ownership, modality, and retirement; requires macOS CI and interactive validation. |
| Web | Throws `std::runtime_error` | Sessions remain host-driven; no runtime creation entry exists yet. |
| Android, iOS | Throws `std::runtime_error` | Platform-driven attachments by design; no runtime creation is planned. |

Unsupported hosts report false from `SupportsMultipleWindows()` and throw `std::runtime_error` from `OpenWindow`; desktop hosts implement the Runtime `CreateWindow` factory.

## Windows validation

Use the existing MSVC Release build with examples and tests enabled.

| Check | Coverage |
|---|---|
| Common CTest label | Unit, Runtime, UI, and Library suites, including window configuration and coordinate-range validation |
| `huxerui_windows_tests` | Win32 platform behavior |
| `huxerui_windows_tests [win32-native]` | Native lifecycle, client-origin round trips with both chrome modes, and native handle release |
| `huxerui_windows_tests [win32-native-startup]` | Default and windowless startup, independent extra windows, timers without a UI, delayed window creation and retirement, and windowless exit |
| `huxerui_header_checks` | Standalone public headers |
| `example_multi_window`, `example_window_relationship` | Public application API and window relationships |

The focused multi-window filter is `"MultiWindow*,RuntimeOpenWindow*,RetiringOneWindow*,ApplicationHandleOpenWindow*"` against `huxerui_runtime_tests`.
A real-HWND lifecycle test with the hidden `[win32-native]` tag opens and closes a runtime window twice, checks requested client origins with both system and custom chrome, and verifies that GDI/USER handle counts return near baseline; run it explicitly with `huxerui_windows_tests.exe [win32-native]` because it creates real top-level windows and Direct3D devices.
The Linux port has the mirror-image `[linux-native]` test (`LinuxRuntimeWindowsOpenAndCloseIndependently`): it opens and closes a runtime window twice against a real display, asserts the requested `initial_position` on platforms that honor placement, retires the runtime windows through the close path while the initial window stays alive, and finishes by closing the initial window to exercise whole-application shutdown; run it explicitly with `huxerui_linux_tests [linux-native]` because it creates real GTK toplevels.
The interactive demo is `example_multi_window`: the primary window opens a secondary one at runtime, both windows share one counter service, and each window keeps its own local state.

## Linux validation

Use a native Linux session with distribution-owned GTK dependencies for CMake; WSL results do not establish Linux compatibility.
Run each hidden native tag in a separate process, particularly `[linux-native]`, whose application loop uses a worker thread.
Retirement and independent-window lifecycle checks run on both X11 and Wayland; floating-window geometry and physical pointer input require the backend capabilities described in [Platform Interfaces](../guide/platforms.md).
Use an isolated X11 display for synthetic pointer sequences, since desktop focus, stacking, and window-manager animations can prevent deterministic delivery.
CMake links Xext when XShape is available; the mcpp payload configuration declares XShape support and probes `xext` explicitly so its consumers receive the same link dependency.

The complete `[linux-native-floating]` X11 scenario remains unqualified: the current desktop and pre-integration binary both time out at stage 6, and an isolated Xwayland run times out at stage 3.
Independent X11 lifecycle, geometry, retirement, and isolated cross-window drag checks pass; those results do not establish the complete floating-window input sequence.
A combined hidden-tag invocation also encountered a GDK/SIGSEGV after the floating scenario timed out; its cause remains unresolved, so independent-process results are the supported validation record.
For the deferred bidirectional-caret failure and hidden profiling failure, see [Known Linux test failures](building.md#known-linux-test-failures).

## macOS preview validation

macOS multi-window and floating-window support remain previews accepted through CI; no physical macOS device is available for interactive validation.
The `macOS Multi-Window Preview` workflow builds on GitHub-hosted macOS runners: arm64 with static linkage and Intel with shared linkage, both retaining the macOS 12 deployment target.
It builds common and codegen suites, AppKit platform suites, standalone headers, `example_multi_window`, `example_window_relationship`, and `example_desktop_pet`.
The separate native test bundle creates real NSWindows on the main thread and exercises startup extras, runtime creation, state isolation, shared services, close interception, deferred keyboard closes, native close, failure cleanup, owner cascade, reparenting, modal stacking, and reopening after retiring every window.
Each CTest scenario runs in a fresh process because AppKit owns a process-wide NSApplication; weak native references check that retired windows and their content views are released after bounded native autorelease cleanup.
The native Quit scenario invokes AppKit termination from the main queue and verifies that queued Runtime shutdown completes and returns to the caller.

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

- Close retires only the target window and its owned descendants. Closing the last framework-owned window requests application shutdown by default; `quit_on_last_window_closed = false` keeps the Runtime running. Hidden windows still count, and the initial window follows the same policy.
- Windows receives forwarded URL, file, and notification activation through the Runtime-owned hidden facilities HWND. Application activation handlers choose whether to activate an existing window or create content; delivery does not require a visible UI window.
- Tray hibernation is an application policy, not a framework switch: applications intercept minimize through `WindowHandle::OnMinimizeRequest`, then either `Hide()` (window stays alive) or `Retire()` (releases the mounted tree, window services, and native resources while the Runtime keeps running), and reopen it from `SystemTrayHandle::OnActivate` with application services surviving on the Runtime; a tray menu Quit calls `ApplicationHandle::Quit()`. Retiring the initial window keeps the application running, so the reopen path uses the same `OpenWindow` entry as any other window; on hosts that do not support runtime window creation, `Retire()` reports the same capability gap.
- Each window receives explicit content through `OpenWindow(content, options)` or `UiWindow::InitializeWindow(runtime, content, options, configuration)`. Applications retain handles and pass their own business metadata; no framework window ID is injected into Environment.
- Window retirement releases the mounted tree, window services, text/pointer/file-drop/semantics subsystems, the last semantic frame, the committed damage snapshot, and the native resources of hosts that implement `CreateWindow`. On Windows, Direct3D/D2D/DWrite/DirectComposition resources are released through the renderer's `Discard`. Before any of that runs, `RetireWindow` verifies through `UiWindow::CanDiscardHost()` that the window and every initialized window in its owned chain support native discard, and rejects the whole retirement with the capability error when any of them does not; the rejection leaves every window fully intact.
- Working-set readings in Task Manager do not establish whether window objects have been destroyed; compare repeated create/retire cycles with object-lifetime checks and committed-memory measurements.
- Applying modal blocking on Windows calls `EnableWindow` on the covered windows. When a modal window opens from a click, the press still holds mouse capture, and disabling the capturing window synchronously delivers `WM_CANCELMODE` or `WM_CAPTURECHANGED`; the adapter defers the nested pointer cancellation until the in-flight pointer dispatch returns (see `WindowsUiWindow::SendPointer`). Delivering that cancellation immediately re-enters the shared pointer interaction and ends the active session while the outer dispatch still holds it, which corrupts the session map. Host ports whose grab-loss notifications arrive synchronously during input dispatch need an equivalent guard. The Linux port applies modality through `gtk_window_set_modal` and `gtk_window_set_transient_for`, which is window-manager mediated and produced no synchronous grab-loss during validation, so no deferred-cancellation guard was needed; revisit if a compositor is found to deliver one.
- GTK4 defers native disposal. Accepted Close and explicit Retire use Runtime maintenance after active callbacks return; native facility release detaches IM, PlatformView, drag, canvas, and controller callbacks while the widget tree remains usable. Release runs at most once per attachment. Run `G_DEBUG=fatal-criticals huxerui_linux_tests '[linux-native-retire]'` on X11 and Wayland to verify release, surviving application services, and reopening.
- On glibc Linux, explicit retirement of the last GTK toplevel also returns unused allocator pages with `malloc_trim(0)`, so a long-lived tray-only Runtime does not retain the UI's freed heap until later allocation pressure. GTK, font, and graphics-driver caches and loaded library pages may remain resident; retirement does not promise the same process footprint across platforms.
- The single Runtime window collection releases retired C++ attachments after the original native callback returns; retained old handles remain disconnected. Linux file-picker parent lookup uses a weak GTK reference. `RuntimeOpenWindowReclaimsRetiredHostsBeforeApplicationShutdown` covers 1,000 cycles; native retirement tests cover frame release and retained GTK callback safety.
- `WindowHandle::Retire()` may run on any thread: it posts weak application/service references through the synchronized application queue, then resolves the Runtime only on its owning thread. `RuntimeOpenWindowRetirementMayRaceApplicationShutdown` exercises submissions concurrent with shutdown.
- Window placement on Linux uses `XMoveWindow` after the native surface maps (`WindowOptions::initial_position`); GTK4 removed `gtk_window_move`. `WindowHandle::Position()` reports the client-area origin in logical screen coordinates through `XTranslateCoordinates` on X11 and `ClientToScreen` on Windows, and reports `std::nullopt` on Wayland where compositors own placement by protocol.
- When several modal windows overlap in coverage, the most recently established relationship wins: each window is blocked only by modals newer than every modal whose own owned chain contains it. Sibling modals of one owner therefore stack newest-on-top instead of disabling each other, application modals stack the same way, and retiring the top modal unblocks the previous one. Re-affirming an unchanged relationship keeps its place in the stack; windows opened through the ordinary `OpenWindow` overload join the blocking set on the common creation path.
- Running the test executables leaves an untracked `testing/Temporary/` directory next to the sources; it is a test artifact and is never committed.

## Porting checklist for additional hosts

Each port follows the Windows shape: keep one application event loop at the Runtime entry, let `CreateApplicationWindow` construct an initialized hidden attachment, and let Runtime own presentation and safe retirement.

1. Hoist the per-window main loop to the Runtime owner and iterate until application shutdown, including when every window is hidden or explicitly retired.
2. Implement `SupportsMultipleWindows()` and `CreateApplicationWindow(ViewFactory, const WindowOptions&)` on the platform Runtime subclass: prepare native facilities, call `InitializeWindow(runtime, content, options, configuration)`, and return a hidden attachment without entering a nested loop.
3. Open the initial window and `AppOptions::extra_windows` through `ApplicationHandle::OpenWindow(content, options)` in the platform entry so every window shares one path.
4. Route native callbacks from the handle or object that originated them instead of a single stored window pointer.
5. Route accepted native closure to `UiWindow::CloseWindow`, honor the shared last-window policy, and preserve explicit retirement without application shutdown.
6. Build and run the common suites plus the platform tests on the host, then record the result in this document.

Host implementation anchors:

- Linux: `LinuxRuntime::RunApplication` owns the GLib loop, `LinuxRuntime::CreateApplicationWindow` initializes hidden GTK attachments, and `LinuxUiWindow::CloseRequested` queues shared retirement. `CanDiscardHost` and `DiscardHost` support explicit retirement. GTK callbacks route per instance and retain their execution context across shared dispatch. X11 placement uses the optional `x11` pkg-config dependency (`HUXERUI_LINUX_X11`).
- Web: `WebUiWindow` in `platform/web/web_adapter.cpp` already mounts one composition root per JavaScript session; runtime creation would expose a second session rather than a second in-page root.

## Rebuild and retest

Windows:

```powershell
cmake --build build\check --config Release --parallel --target example_multi_window huxerui_runtime_tests
build\check\bin\Release\huxerui_runtime_tests.exe "MultiWindow*,RuntimeOpenWindow*"
```

Linux and macOS hosts use the same targets after a normal configure; see [Building HuxerUI](building.md) for toolchain selection.

The separate hidden `[linux-native-drag]` case drives actual X11 pointer input through dynamically loaded XTest.
It covers a cross-window typed drop, source retirement during native transport, and a subsequent ordinary click without synthetic target Down events.
Run it with an X11 display and libXtst available:

```bash
GDK_BACKEND=x11 build/bin/huxerui_linux_tests "[linux-native-drag]"
```

Use `G_DEBUG=fatal-criticals` to catch native teardown failures, including IM client detachment after widget disposal.
It skips on Wayland and when XTest is unavailable; a skip does not validate native Wayland dragging.
Use an isolated X11 display without window-manager animations for deterministic XTest placement and clicks; a rootful nested Xwayland server is suitable.
For example, start `Xwayland -displayfd 1 -geometry 1280x800 -shm -ac -nolisten tcp`, then use its reported display number in DISPLAY for the native test process and stop that server afterward.
The shared `[window-drag]` Runtime cases exercise transport-independent scopes, target geometry, snapshots, callback context, Runtime isolation, modality, and lifecycle cleanup.
`example_multi_window` includes opted-in sources and targets for manual backend validation.
