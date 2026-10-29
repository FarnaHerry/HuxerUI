# Multi-Window Design

This document defines runtime creation of additional native windows, per-window content selection, and the ownership rules that keep one application thread across every attached `UiWindow`.
It extends the [Application Activation and Lifecycle](application.md) ownership model and the [Window Chrome](window-chrome.md) command surface without changing their contracts.

## Goals

- Let an application open additional native windows after startup through one application-thread API.
- Give every window its own composition tree, state scopes, window services, layers, and lifecycle while sharing one Runtime and its application services.
- Give each window an explicit retained content factory while keeping the application root signature unchanged.
- Keep the initial window on the exact code path used by later windows.
- Preserve the existing shutdown order: windows retire before application tasks and services.

## Non-goals

- Multiple UI threads or per-window threads; all attached UIs stay on the one application thread.
- A general window-manager or platform-handle API. Initial placement remains best effort, with native floating-window geometry and move requests specified separately in [Window Chrome](window-chrome.md#floating-utility-windows); compositor-controlled operations remain explicitly unavailable.
- Cross-window layers, shared `NavigationStack` state, or cross-window focus transfer.
- Window relationships such as owner, transient parent, or modality are specified separately in [Window Relationship Design](window-relationship.md).
- Mobile multi-window: Android and iOS windows remain platform-driven attachments, and this document does not add runtime window creation there.
- Saveable or restorable window layout. Floating utility presentation and explicit native geometry requests extend the window boundary as specified in [Window Chrome](window-chrome.md#floating-utility-windows); applications still own saved-position policy.

## Ownership model

`Runtime` uniquely owns framework-created windows in one collection.
The attached-UI weak registry also tracks externally owned host attachments without acquiring native ownership.
Accepted closure marks a window closed immediately; maintenance retires its shared state and releases its native attachment after active UI callbacks and window installation have returned.

```cpp
class ApplicationHandle {
 public:
  WindowHandle OpenWindow(ViewFactory content, WindowOptions options = {}) const;
};
```

`ApplicationHandle::OpenWindow(content, options)` creates and presents independently composed content on the original application's thread.
Runtime validates the content and options, invokes native creation, and retains the window independently of its returned handle.
Each native attachment receives its own content and options through `UiWindow::InitializeWindow`.
Creation failures retire the partial window and propagate.

The desktop application entry creates the initial window by calling `ApplicationHandle::OpenWindow` with the application root factory and `AppOptions::window` when `AppOptions::create_initial_window` is true, its default value, so startup and runtime windows share one path.
Setting the flag to false skips native window creation, renderer initialization, window hooks, and root composition for that window.
`AppOptions::extra_windows` lists independent startup window options; these windows use the application root factory, including when initial-window creation is disabled.
For distinct startup content, enqueue explicit `OpenWindow(content, options)` calls through the application task scope during an application hook.
With initial-window creation disabled and no extra windows, application hooks, services, tasks, timers, activation dispatch, and the native event loop still run without a UiWindow.
Later application work may open a window through the existing `ApplicationHandle::OpenWindow` API; work launched in application hooks remains queued until installation has finished.
Skipping the initial window does not create an attachment; every framework-created window follows the same close policy.
Set `AppOptions::quit_on_last_window_closed = false` to keep the Runtime running after every window closes, including after windowless startup.
Mobile, Web, and embedded attachments are platform-owned and do not consult this desktop startup flag.

## Per-window content and options

`UiWindow::InitializeWindow` takes a retained `ViewFactory` and the window's own `WindowOptions`.
The factory executes in that window's root scope and may capture application models or ordinary business metadata with the required lifetime.
Each window has a fresh root Environment; an opening window's Environment and options are not implicitly inherited.

```cpp
const WindowHandle settings = application.OpenWindow([model] {
  return SettingsView(model);
}, {.title = "Settings"});
```

Retain the returned `WindowHandle` to address that window later; discarding it leaves the window open.
Inside window content, `UseWindow()` resolves the current window through its composition or captured callback context.
Business identifiers, document names, and content labels belong to application models or explicit arguments.
The framework does not seed a window identity into Environment or select content from a window configuration key.

## Window placement

`WindowOptions::initial_position` and `WindowHandle::Position()` use the same client-area origin in logical screen coordinates, with X increasing right and Y downwards.
Native decorations are excluded; negative positions are valid for displays above or left of the primary display.
Shared option validation rejects nonfinite positions before native creation; Windows additionally validates its integer coordinate range after applying display scale and decoration offsets.
The existing Linux/X11 placement and origin implementation requires separate coordinate, scaling, and input-lifecycle validation by Linux maintainers.
Hosts that do not support placement ignore finite preferences and return `std::nullopt` from the query.
`Position()` is an application-thread snapshot without composition subscriptions or movement notifications; it returns `std::nullopt` after retirement.
The application-facing contract and platform availability are documented in [Native window placement](../guide/platforms.md#native-window-placement).

## State and service rules

- State created by `UseState` inside one window's content belongs to that window's recomposition scopes and is destroyed with the window.
- Content factories may capture shared application models; application services are also available across windows. Retained handles remain bound to their original window lifetime.
- Window services and layers stay per window exactly as today; `window_hooks` run once per attached window in declaration order.
- One window's frame production never recomposes, measures, or repaints another window; each `UiWindow` keeps its own damage and scheduling.

## Platform mapping

- Windows creates one HWND per `WindowsUiWindow` through `WindowsRuntime::CreateUiWindow` and a shared message loop. A separate hidden facilities HWND receives application activation and tray messages even when no UI window exists.
- Linux creates one GtkWindow per `LinuxUiWindow` through `LinuxRuntime::CreateUiWindow` and shares one GLib event loop. PlatformView, IME, drag, frame, and canvas callbacks detach while their native widgets remain alive; Runtime maintenance releases the C++ attachment after active callbacks return.
- The macOS preview creates NSWindow or utility NSPanel attachments through `MacRuntime::CreateUiWindow` and a single NSApplication loop. Each attachment has a window delegate; the application delegate owns activation and process lifecycle. Native input scopes also defer application tasks during reentrant AppKit input delivery.
- Android, iOS, Web, and embedded UI attachments remain host-owned. They report no programmatic top-level window support and receive the declaration's initial content and options through shared initialization.

## Shutdown and failure

Closing the last framework-owned window requests application shutdown when `AppOptions::quit_on_last_window_closed` is true, its default.
Hidden and minimized windows still count; initially windowless startup, rejected creation, and external attachment retirement do not trigger this policy.
Set the option to false to keep application services, timers, and activation available after the last Close and open windows later.

`WindowHandle::Close()` consults the target window's close handler before accepting closure and queuing cleanup at the safe maintenance boundary.
The application's last-window policy determines whether closing every window also requests shutdown.
Owned descendants retire before their owners; relationship removal restores the surviving modal stack and owner activation.
Framework-created windows use the single Runtime ownership collection; external attachments retain native host ownership.

Execution guards retain an application-level active UI call count, including across nested native loops.
Maintenance waits for that count and window installation to reach zero, and rejected wakeups are retried at subsequent safe boundaries without replacing an existing exception.
Native facilities release after shared retirement while their native members can still perform teardown.

`ApplicationHandle::Quit()` bypasses per-window close interception and retires windows before application tasks and services.
The macOS preview cancels AppKit process termination and queues Runtime shutdown so `RunApplication()` returns without entering a termination modal loop.
Creation failures unwind the partial attachment and preserve the original exception; unrelated windows and services remain available.
