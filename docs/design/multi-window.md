# Multi-Window Design

This document defines runtime creation of additional native windows, per-window content selection, and the ownership rules that keep one application thread across every attached `UiWindow`.
It extends the [Application Activation and Lifecycle](application.md) ownership model and the [Window Chrome](window-chrome.md) command surface without changing their contracts.

## Goals

- Let an application open additional native windows after startup through one application-thread API.
- Give every window its own composition tree, state scopes, window services, layers, and lifecycle while sharing one Runtime and its application services.
- Route each window to distinct root content without changing the `RootFactory` signature.
- Keep the initial window on the exact code path used by later windows.
- Preserve the existing shutdown order: windows retire before application tasks and services.

## Non-goals

- Multiple UI threads or per-window threads; all attached UIs stay on the one application thread.
- Window geometry, dragging, or platform handle APIs beyond the existing `WindowHandle` commands, with one deliberate exception: the best-effort placement surface (`WindowOptions::initial_position` and `WindowHandle::Position()`) for tear-off workflows. It is honored on X11 and Windows and reports unavailable on Wayland, where compositors own placement by protocol.
- Cross-window layers, shared `NavigationStack` state, or cross-window focus transfer.
- Window relationships such as owner, transient parent, or modality are specified separately in [Window Relationship Design](window-relationship.md).
- Mobile multi-window: Android and iOS windows remain platform-driven attachments, and this document does not add runtime window creation there.
- Saveable or restorable window layout.

## Ownership model

`Runtime` owns the window table.
Each entry is one platform-derived `UiWindow` attached to that Runtime, already tracked through the existing weak registry that `Runtime::Retire()` walks before closing application tasks and services.
The table is append-only from the application side: windows leave the table by retiring, never by replacement.

```cpp
class Runtime {
 public:
  [[nodiscard]] WindowHandle OpenWindow(const WindowOptions& options);
 protected:
  virtual std::unique_ptr<UiWindow> CreateNativeWindow(const WindowOptions& options, std::uint32_t window_ordinal);
};
```

`Runtime::OpenWindow` is the single public creation path.
It validates on the application thread that the Runtime is running, assigns the next window ordinal, asks the platform subclass for one native window, registers it, and returns its `WindowHandle`.
The platform virtual constructs the platform `UiWindow`, creates the native window, and calls `UiWindow::InitializeWindow` with the per-window options; creation failures retire the partial window and propagate.

`ApplicationHandle::OpenWindow(const WindowOptions&)` forwards to `Runtime::OpenWindow` on the application thread and exists so a captured `ApplicationHandle` can open windows outside composition, matching the existing handle contract.

The platform application entry creates the initial window by calling `Runtime::OpenWindow` with `AppOptions::window` instead of constructing its platform window inline, so startup and runtime windows share one path.
`AppOptions::extra_windows` lists additional windows opened in declaration order immediately after the Runtime starts; an empty list preserves single-window startup.

## Per-window options and content selection

`UiWindow::InitializeWindow` takes the window's own `WindowOptions`; it no longer reads `AppOptions::window`, which remains only the initial-window default.
Every `WindowOptions` carries a stable application-defined `key` that is empty for the initial window unless the application sets it.

Each root composition receives its identity as one typed Environment value seeded by `UiWindow` beside the existing viewport environment:

```cpp
struct WindowIdentity {
  std::string key;
  std::uint32_t ordinal;
};
```

The root factory selects content by reading `UseEnvironment<WindowIdentity>()` during composition, the same inherited-value mechanism used for themes.
`UseWindow()` keeps resolving the handle of the window whose composition is running, so existing window commands and observers bind to the correct window without a new lookup API.

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
- Application services remain the only supported cross-window shared state; a window must not retain another window's handle beyond that window's retirement.
- Window services and layers stay per window exactly as today; `window_hooks` run once per attached window in declaration order.
- One window's frame production never recomposes, measures, or repaints another window; each `UiWindow` keeps its own damage and scheduling.

## Platform mapping

- Windows creates one top-level HWND per `WindowsUiWindow` through one shared message loop owned by `WindowsRuntime`; the window procedure routes by per-window instance data. The initial window requests application shutdown when closed, matching historical single-window behavior; windows opened later retire independently.
- Linux shares one `GMainContext` iteration loop owned by `LinuxRuntime`; `LinuxRuntime::CreateNativeWindow` attaches one `GtkWindow` per `LinuxUiWindow`, and GTK signal handlers route by per-instance user data. Runtime-opened windows run their shared teardown synchronously on the close path because GTK defers the native dispose of `gtk_window_destroy`. Window placement and screen-origin queries exist on X11 and degrade to platform placement on Wayland, where compositors own positions by protocol.
- The macOS preview creates one NSWindow per `MacUiWindow` through a single `NSApplication` loop owned by `MacRuntime`; window delegate callbacks resolve the originating NSWindow, while application lifecycle changes fan out to all live attachments. Native input scopes defer Runtime task delivery across reentrant AppKit main-queue servicing so close, retirement, and shutdown cannot dismantle an in-flight shared input dispatch. Closing the initial window requests application shutdown; later windows retire independently.
- Web, Android, and iOS do not override `CreateNativeWindow`, so `OpenWindow` reports that the host does not support runtime window creation; their platform-driven attachments are unchanged.
- Android and iOS keep platform-driven attachments permanently; their multi-window surface remains scene- and activity-based.

## Shutdown and failure

Closing the initial window requests application shutdown, preserving the historical single-window behavior; closing a later window retires only that window. Retiring every window explicitly does not terminate the application; `Runtime::RequestShutdown()` remains the whole-application stop.
The macOS preview cancels AppKit's process termination request and queues Runtime shutdown, allowing `RunApplication` to return after teardown without entering AppKit's termination modal loop.
`WindowHandle::Retire()` requests retirement of one window, bypassing the platform close path and its request handlers; the retirement runs on the application thread after the current callback returns. Owned windows retire first, deepest chain first, and the native attachment is discarded only after shared teardown, so retiring the initial window keeps the application running and close-path shutdown semantics stay unchanged. Hosts that cannot retire a window at runtime report the capability gap, matching `OpenWindow`.
`Runtime::Retire()` retires every attached window through the existing registry before closing tasks, HTTP operations, and services, so an application that never closes its extra windows still shuts down cleanly.
A window that fails native creation after registration is retired immediately and its creation throws.
