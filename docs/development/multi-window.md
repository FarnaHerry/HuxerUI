# Developing Multi-Window

This guide covers the development state, validation results, and platform porting checklist for the runtime multi-window extension introduced on `feat/multi-window`.
It complements the ownership contract in [Multi-Window Design](../design/multi-window.md), which remains the normative reference for the API and invariants.

## Current status

| Platform | Runtime `OpenWindow` | Status |
|---|:---:|---|
| Windows | Supported | Implemented, built, and tested on the development host. |
| Linux | Throws `std::logic_error` | Port planned; adapter still owns the window and the `GMainContext` loop. |
| macOS | Throws `std::logic_error` | Port planned; the application delegate still routes through one window pointer. |
| Web | Throws `std::logic_error` | Sessions remain host-driven; no runtime creation entry exists yet. |
| Android, iOS | Throws `std::logic_error` | Platform-driven attachments by design; no runtime creation is planned. |

The unsupported-host error is the intended contract, not a regression: the shared `Runtime::CreateNativeWindow` default throws, and only hosts that override it gain the capability.

## Validated on the Windows host

MSVC Release, full build with examples and tests enabled.

| Suite | Result |
|---|---|
| `huxerui_runtime_tests` (includes the six multi-window cases) | 977 cases, 30718 assertions |
| `huxerui_unit_tests` | 177 cases, 11083 assertions |
| `huxerui_windows_tests` | 55 cases, 651 assertions |
| `huxerui_ui_tests` | 35 cases, 204 assertions |
| Codegen suites and header checks | Built clean |

The focused multi-window filter is `"MultiWindow*,RuntimeOpenWindow*,RetiringOneWindow*,ApplicationHandleOpenWindow*"` against `huxerui_runtime_tests`.
A real-HWND lifecycle test with the hidden `[win32-native]` tag opens and closes a runtime window twice and checks that GDI/USER handle counts return near baseline; run it explicitly with `huxerui_windows_tests.exe [win32-native]` because it creates real top-level windows and Direct3D devices.
The interactive demo is `example_multi_window`: the primary window opens a secondary one at runtime, both windows share one counter service, and each window keeps its own local state.

## Behavioral notes

- Closing the initial window requests whole-application shutdown, preserving historical single-window behavior on every platform. Closing a runtime-opened window retires only that window.
- A forwarded single-instance activation (URL or file) targets the visible initial window first, then the foreground window of the application class, then any visible window, so the surfaced target stays predictable while extra windows are open.
- Tray hibernation is an application policy, not a framework switch: applications intercept minimize through `WindowHandle::OnMinimizeRequest`, then either `Hide()` (window stays alive) or `Retire()` (releases the mounted tree, window services, and native resources while the Runtime keeps running), and reopen it from `SystemTrayHandle::OnActivate` with application services surviving on the Runtime; a tray menu Quit calls `ApplicationHandle::Quit()`. Retiring the initial window keeps the application running, so the reopen path uses the same `OpenWindow` entry as any other window; on hosts that do not support runtime window creation, `Retire()` reports the same capability gap.
- `WindowIdentity::ordinal` is assigned by `Runtime::OpenWindow`; callers that attach windows directly through `UiWindow::InitializeWindow` (embedded hosts and windowless tests) supply their own ordinal and are responsible for keeping it distinct.
- Window retirement releases the mounted tree, window services, text/pointer/file-drop/semantics subsystems, the committed damage snapshot, and the native resources of hosts that override `CreateNativeWindow`. On Windows, Direct3D/D2D/DWrite/DirectComposition resources are released through the renderer's `Discard`.
- Closing a Windows window frees object memory immediately; the graphics driver returns the remaining working set asynchronously. This looks like a partial drop in Task Manager and is expected. If retention is ever suspected again, compare "Commit size" across repeated open/close cycles and use `EmptyWorkingSet` to prove that no object pins the memory before investigating further.
- Applying modal blocking on Windows calls `EnableWindow` on the covered windows. When a modal window opens from a click, the press still holds mouse capture, and disabling the capturing window synchronously delivers `WM_CANCELMODE` or `WM_CAPTURECHANGED`; the adapter defers the nested pointer cancellation until the in-flight pointer dispatch returns (see `WindowsUiWindow::SendPointer`). Delivering that cancellation immediately re-enters the shared pointer interaction and ends the active session while the outer dispatch still holds it, which corrupts the session map. Host ports whose grab-loss notifications arrive synchronously during input dispatch need an equivalent guard.
- When several modal windows overlap in coverage, the most recently established relationship wins: each window is blocked only by modals newer than every modal whose own owned chain contains it. Sibling modals of one owner therefore stack newest-on-top instead of disabling each other, application modals stack the same way, and retiring the top modal unblocks the previous one. Re-affirming an unchanged relationship keeps its place in the stack; windows opened through the ordinary `OpenWindow` overload join the blocking set on the common creation path.
- Running the test executables leaves an untracked `testing/Temporary/` directory next to the sources; it is a test artifact and is never committed.

## Porting checklist for remaining desktop hosts

Each port follows the Windows shape: keep one application event loop at the Runtime entry, let `CreateNativeWindow` construct and initialize the platform window, and retire runtime-opened windows independently of the initial one.

1. Hoist the per-window main loop to the Runtime owner and iterate while any owned window is running.
2. Implement `CreateNativeWindow(const WindowOptions&, std::uint32_t)` on the platform Runtime subclass: construct the platform `UiWindow`, create the native window, call `InitializeWindow(runtime, options, ordinal, configuration)`, and return it without entering a nested loop.
3. Open the initial window and `AppOptions::extra_windows` through `Runtime::OpenWindow` in the platform entry so every window shares one path.
4. Route native callbacks from the handle or object that originated them instead of a single stored window pointer.
5. Define close behavior: the initial window keeps the platform's historical shutdown behavior; runtime-opened windows destroy only their own native window.
6. Build and run the common suites plus the platform tests on the host, then record the result in this document.

Concrete anchors on the unported adapters:

- Linux: `LinuxUiWindow::Run` in `platform/linux/linux_adapter.cpp` still creates its `GtkWindow` and runs `g_main_context_iteration` inline; the loop condition is the single window's `running_` flag. Ownership and modality virtuals are implemented through `gtk_window_set_transient_for` and `gtk_window_set_modal`, but the close path still shuts the application down until runtime window creation lands.
- macOS: `MacUiWindow::Run` in `platform/macos/appkit_adapter.mm` stores itself on the application delegate's single `huxeruiWindow` pointer, and delegate callbacks such as `windowShouldClose:` assume one window. Ownership uses `addChildWindow:ordered:`, and blocked windows refuse key status through the `canBecomeKeyWindow` override on `HuxerUIWindow`; both are unverified on a macOS host.
- Web: `WebUiWindow` in `platform/web/web_adapter.cpp` already mounts one composition root per JavaScript session; runtime creation would expose a second session rather than a second in-page root.

## Rebuild and retest

Windows:

```powershell
cmake --build build\check --config Release --parallel --target example_multi_window huxerui_runtime_tests
build\check\bin\Release\huxerui_runtime_tests.exe "MultiWindow*,RuntimeOpenWindow*"
```

Linux and macOS hosts use the same targets after a normal configure; see [Building HuxerUI](building.md) for toolchain selection.
