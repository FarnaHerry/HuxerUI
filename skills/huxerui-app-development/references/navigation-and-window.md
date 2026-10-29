# Navigation and Window

## Navigation surfaces

`NavigationBar` and `NavigationPane` are destination selectors, not page routers. Keep their selected index controlled. `DrawerLayout` combines app content with optional `StartDrawer` and `EndDrawer`; each drawer's open state is controlled through `.Open` and `.OnOpenChanged`.

## Navigation stack

`NavigationStack(root_factory)` provides imperative factory-based navigation. Routed navigation uses:

- copyable, equality-comparable route values;
- `State<NavigationPath<Route>>` as authoritative path state;
- a root factory;
- a resolver from `Route` to `View`;
- `UseNavigation<Route>()` for the nearest compatible stack;
- `UseRootNavigation<Route>()` for the root compatible stack.

The controller supports `Push`, `Replace`, `Pop`, `SetPath`, `CanPop`, and `Depth`. Keep route parameters in the route value instead of side-channel global state. Nested stacks may own local flows while root navigation handles application-level destinations.

On Web, include `<huxerui/web/navigation.h>` and use `BrowserNavigationStack` with a codec whose `Decode(location)` returns an optional path and whose `Encode(path)` returns a same-document location. The codec owns URL policy. One browser navigation stack owns URL synchronization per document.

## Window content

`WindowOptions` configures the title, initial size, optional minimum size, content mode, chrome mode, preferred custom title-bar height, and caption labels exposed by the active SDK. `minimum_size` is a minimum logical client size for framework-owned resizable desktop windows; when present, initialize it with `Size{width, height}` and keep both dimensions finite and positive. An initial size below it is raised independently on each axis. Android, iOS, and Web viewports remain host-owned and may be smaller, so do not use this option as a layout constraint or clamp reported viewport metrics. `WindowChromeMode::System` leaves chrome to the platform; `WindowChromeMode::Custom` lets application content occupy the title-bar area while retaining HuxerUI's platform-specific window behavior. Use `WindowContentMode::SafeArea` for automatically inset content and `WindowContentMode::EdgeToEdge` only when application content deliberately handles insets.

`SafeAreaPadding` consumes selected safe-area edges without hardcoded platform values. Ancestor consumption affects what descendants see.

`WindowTitleBar` lays out application-defined title-bar content, consumes the platform-resolved caption insets, and marks its non-interactive area as draggable. Interactive descendants such as buttons, tabs, and text fields remain interactive inside it and need no exclusion rectangles. Do not duplicate framework- or platform-managed caption controls. Use `WindowDragRegion` only when another mounted region should drag the window.

`UseWindow()` returns the current window's handle during composition or a captured window callback. It exposes `Show`, `Hide`, `Activate`, `Minimize`, `Maximize`, `Restore`, `ToggleMaximize`, and `Close`. `Activate` restores a minimized window and requests foreground activation, subject to platform focus policy. `IsOpen()` and `LifecycleState()` observe changes when read during composition; `OnLifecycleChanged()` installs a composition-lifetime observer. Hidden and minimized windows remain open. A stale handle does not target another window.

`OnMinimizeRequest` and `OnCloseRequest` register lifecycle-bound synchronous handlers. Return `true` to consume the request; `false` allows the platform action only while the window remains open. Recursive requests for the same command are consumed, while the other command remains independent. Use dependency arguments when captured values should replace a committed handler. For an asynchronous close decision, consume the request now and issue a new close after updating the application's decision state.

System bar background and foreground brightness come from the resolved `SystemBarsAppearance`, which is both a Theme value and a View modifier. Its `Light` and `Dark` foreground choices describe icons and text; `ThemeBrightness` describes theme appearance. See [system appearance](theme-animation-presentation.md#system-appearance). Desktop custom chrome and mobile safe areas share a public window boundary but have different platform behavior.

## Multiple windows

Check `ApplicationHandle::SupportsMultipleWindows()` before presenting a new-window action. Desktop hosts support framework-owned top-level windows; Android, iOS, and Web UI attachments remain host-owned and do not imply this capability.

```cpp
auto application = UseApplication();
return Button("New window")
    .With(Enabled(application.SupportsMultipleWindows()))
    .OnClick([application] {
      application.OpenWindow([] { return MaterialTheme { Text("New window") }; },
                             {.title = "New window", .initial_size = {640.0F, 480.0F}});
    });
```

`OpenWindow(ViewFactory, WindowOptions)` runs on the original application thread outside composition and application/window installation. Its retained factory is evaluated later in the new window's root scope. Windows share application services and State but have fresh root Environments, independent composition lifetimes, and their own window hooks. Explicitly supply shared models or theme policy; window options and the opener's local Theme/Locale do not transfer implicitly. Discarding the returned handle leaves the window open.

Invalid options throw `std::invalid_argument`, invalid thread/lifetime throws `std::logic_error`, and unsupported or failed native creation throws `std::runtime_error`. Success means the window was created and presented, not that its first frame has rendered.

`AppOptions::quit_on_last_window_closed` defaults to true for framework-owned desktop windows. Hidden windows still count; external attachments and an initially windowless Runtime do not trigger this policy. Set it to false when the application must remain alive after closing its last window, and use `ApplicationHandle::Quit()` for explicit termination. Closing a window cancels its composition tasks and presentation services; application tasks remain until shutdown.

## Activation

Read the optional `UseApplication().StartupActivation()` once during application installation or derive initial UI from it without side effects. Inspect its `LaunchActivation`, `UrlActivation`, `FileActivation`, or `NotificationActivation` alternative with `std::visit` or `std::get_if`. `UrlActivation::url` is a validated `Uri`, `FileActivation` contains `FileReference` capabilities, and `NotificationActivation` carries an identifier and data. Validate application-level routing and authorization before acting on external input.

Register the single subsequent-activation handler with `ApplicationContext::OnActivation()` in `AppOptions::application_hooks`. It receives queued activations in FIFO order on the application thread, survives UI retirement, and does not replay startup. Update application-owned state or navigation from this handler; it has no implicit current window. Open a window or address a retained live handle when presentation is needed.

For local-notification submission, templates, scheduling, and activation data, read [local-notifications.md](local-notifications.md).

On Windows, register an application-owned custom URL scheme with `windows::RegisterUrlScheme(scheme, display_name)` from `<huxerui/system.h>` in explicit entry code, normally before `RunApplication()`. Prefer this public API over hand-written registry helpers or static-initialization side effects. It targets the current executable and current user, including the Windows 7 compatibility backend; no notification identity, COM setup, or CMake registration metadata is required. Use a bare ASCII scheme without ':', with a letter followed by letters, digits, '+', '-', or '.', and 2 to 255 characters. Registration persists after exit. Call `windows::UnregisterUrlScheme(scheme)` only for explicit removal or relocation, not from normal lifecycle cleanup. Repeated registration by the same executable is allowed; another executable or a machine-wide registration is rejected. Report conflicts rather than deleting another application's keys. This API does not override user default-app choices, register file associations, or manage all-user installation; other platforms still use their native manifest or bundle declarations. Incoming URLs continue through `UrlActivation` and require application-level validation and authorization.

## System tray

`UseApplication().SystemTray()` returns the application-level tray handle, and `UseApplication().Quit()` requests orderly application termination. Check `IsAvailable()` before hiding the last visible window because unsupported hosts and temporarily unavailable Linux tray hosts report `false`.

Install application-wide tray behavior once, normally in an `ApplicationHook`. `SystemTrayHandle::Show(icon, options)` creates or replaces the shared presentation, and `Hide()` removes it. `SystemTrayOptions` reuses `MenuEntry`, `MenuItem`, and `MenuSection`; tray and menu icons are `ImageVariant` values that must resolve to raster `ImageAsset` values.

Register primary activation through `SystemTrayHandle::OnActivate(handler)` on the application's thread. It retains one nonempty handler until Runtime shutdown, rejects duplicate registration, and survives presentation changes and window retirement. It is not a composition-lifetime hook and does not capture the current window context. Capture a specific `WindowHandle`, or an application-owned window controller that binds and unbinds the intended window, rather than calling `UseWindow()` inside the tray callback.

Compose minimize-to-tray behavior from the tray handle and window request handlers. When the tray is unavailable, return `false` so minimize and close retain their normal platform behavior. Do not create a second menu model, a `PlatformModule`, or platform-specific tray code for ordinary application use.
