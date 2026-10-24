# Platform Support

HuxerUI application code and the shared Runtime support Windows, macOS, Linux, Web, Android, and iOS.
Each backend uses platform lifecycle, input, text, accessibility, file, network, and rendering services where available.

## Capability overview

| Platform | Rendering and text | HTTP and files | Accessibility | PlatformView | ExternalTexture | Permissions | Local notifications | System tray |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| Windows | Direct2D and DirectWrite | Yes | UI Automation | Yes | Yes | AppCapability | Registered Win32 toast identity | Yes |
| macOS | Core Graphics and Core Text | Yes | AppKit accessibility | Yes | Yes | Camera and microphone | User Notifications | Yes |
| Linux | GSK, Cairo, and Pango | Yes | Not supported | No | Yes | Unavailable | Unavailable | StatusNotifierItem host |
| Web | Canvas 2D and browser text metrics | Yes | Not supported | Yes | Yes | Query only | Unavailable | No |
| Android | Android Canvas and StaticLayout | Yes | AccessibilityNodeInfo | Yes | Yes | Camera and microphone | Configured NotificationManager | No |
| iOS | Core Graphics and Core Text | Yes | UIKit accessibility | Yes | Yes | Camera and microphone | User Notifications | No |

Capabilities not listed as implemented are not implied by the shared API.
OHOS does not currently have a repository-owned backend.

The shared local-notification API is available on every listed platform.
Registered Windows applications, configured Android hosts, and the iOS and macOS adapters install native transports; Linux and Web currently report unavailable capabilities and operations.
See [Local Notifications](../design/local-notifications.md) for authorization, activation, and platform mapping details.

## Native window placement

`WindowOptions::initial_position` requests the initial client-area origin in logical screen coordinates: X increases to the right and Y downwards, with negative coordinates allowed for monitors left of or above the primary display.
The client origin is the point corresponding to `{0, 0}` in the window's content coordinates; native title bars, borders, and shadows are excluded.
Windows applies this preference using the native display scale; the window manager may constrain placement.
macOS applies this preference through AppKit and reports positions in the same logical coordinates.
Linux/X11 has a placement implementation, but its origin consistency, scaling, and native lifecycle still require separate Linux maintainer validation.
Wayland and host-managed mobile and Web surfaces use platform placement instead.

```cpp
const WindowHandle secondary = application.OpenWindow({
    .title = "Inspector",
    .initial_size = {400.0F, 300.0F},
    .initial_position = Point{120.0F, 160.0F},
});
const std::optional<Point> origin = secondary.Position();
```

`WindowHandle::Position()` reads that same client origin synchronously on the application thread.
It is a snapshot: reading it during composition does not subscribe to window moves or trigger recomposition when the user moves the window.
It returns `std::nullopt` after retirement and on hosts that do not expose positions, including Wayland.
Initial positions must be finite; NaN and infinity throw `std::invalid_argument`, including on hosts that ignore placement.
On Windows, conversion to native coordinates checks the signed integer range before narrowing and reserves the default-placement sentinel.
Positions that become unrepresentable after DPI and decoration adjustment throw `std::invalid_argument`; ordinary negative positions remain valid.

## Floating utility windows

Windows, Linux (GTK), and macOS support `WindowPresentation::Utility` for a borderless, non-resizable window excluded from task or window switching.
Set `WindowOptions::transparent_surface` for per-pixel transparency, `show_without_activation` for passive display, and `topmost` for an initial above-normal stacking preference.
These options default to ordinary window behavior; transparency requires Utility presentation.
The utility, transparency, and passive-display policies are fixed when the native window is created.
Hosts reject nondefault presentation policies their active backend cannot honor with `std::logic_error`, so query `WindowHandle::Capabilities()` or branch on availability instead of assuming one desktop contract.
Other backends retain that explicit rejection behavior.

```cpp
const auto pet = application.OpenWindow({
    .title = "Companion",
    .initial_size = {200.0F, 200.0F},
    .presentation = WindowPresentation::Utility,
    .transparent_surface = true,
    .show_without_activation = true,
    .topmost = true,
});
```

`WindowHandle::Capabilities()` describes native operations available to that attachment.
`SetTopmost`, `SetPosition`, `SetClientSize`, `SetInputPassthrough`, and `BeginMove` return `WindowRequestResult`: Submitted accepts the request without promising native application, Unavailable reports an unsupported operation, Rejected reports native refusal, and Disconnected reports a retired attachment.
Call these operations and `WorkArea()` on the original application thread; nonfinite positions and nonpositive or nonfinite sizes throw `std::invalid_argument`.
`WorkArea()` returns the current monitor's logical screen work area, or `std::nullopt`; monitor selection follows the native window rather than a global primary-monitor assumption.
Use `BeginMove()` from an accepted drag gesture to let the host move the window, and reserve programmatic positioning for explicit placement or recovery.

Windows transparent utility windows use a bounded layered-surface path with complete CPU readback per presented frame, limited to 4096 physical pixels per dimension and 16 million pixels in total.
Zero-alpha pixels naturally pass native mouse input through; translucent pixels remain interactive.
`SetInputPassthrough(true)` excludes the entire surface without changing its appearance, and restoring input preserves native modal blocking.
Independent arbitrary shaped input regions remain unavailable; View hit testing and visual alpha are not substitutes for that capability.
PlatformView hosting is unavailable on the transparent path; ExternalTexture rendering remains usable.

macOS utility windows are borderless non-activating `NSPanel` surfaces excluded from window cycling and able to share a Space with a full-screen window.
Transparent surfaces make the panel non-opaque with a clear background and no shadow, and the renderer erases stale content with per-pixel alpha instead of the opaque fallback fill.
Passive display orders the panel front without taking key status, and its non-activating style keeps pointer input from implicitly activating the application.
`SetInputPassthrough` uses whole-window input exclusion and is available on every attachment; `BeginMove` needs the pointer-down event of the current drag gesture and is rejected without one.
Open an ordinary settings window and explicitly activate it for keyboard/IME editing.

Linux transparent utility windows draw through the regular GTK scene graph: the window background becomes a per-pixel alpha surface, and no CPU readback budget applies.
Both GDK backends support Utility presentation, transparency, client sizing, `WorkArea()`, and interactive `BeginMove()`.
On X11 they additionally support passive display, topmost, programmatic positioning, and whole-window input passthrough through the shape extension; Wayland owns activation, stacking, and placement by protocol, so those operations report Unavailable there and `Position()` returns `std::nullopt`.
Treat backend availability as a runtime property of each attachment: a Wayland session and an X11 or XWayland session on the same machine report different capabilities.
See the [desktop pet example](../../examples/desktop_pet/README.md) and [floating-window design](../design/window-chrome.md#floating-utility-windows) for scope and native validation requirements.

## Android host focus

The native HuxerUI host remains focusable for keyboard and IME input.
On Android API 26 and later it disables the system's default View focus highlight because that drawable would tint the entire application surface when its window receives focus, including in split-screen mode.
HuxerUI components keep their own focus indication; embedded native controls keep their native focus behavior.

## Shared CPU buffer references

Include `<huxerui/data.h>` for `BufferReference`: a retained, read-only view of address-stable CPU memory.
Unlike `Bytes`, it does not own a byte snapshot or copy memory when passed through a supported PlatformPayload bridge.
Use it for image analysis or other large in-process buffers; dimensions, format, row strides, and frame sequence belong in your own typed frame structure.
The runnable [`buffer_reference` example](../../examples/buffer_reference/main.cpp) demonstrates native allocation, zero-copy analysis, and sequential reuse, including a histogram that excludes row padding.

```cpp
auto storage = std::make_shared<huxerui::Bytes>(4096);
huxerui::BufferReference frame(*storage, storage);
FillFrame(*storage);
Analyze(frame.AsBytes());
auto region = frame.Slice(128, 512);
```

The owner must keep the address valid without resizing or explicitly closing the resource.
Finish producer writes before reading, and wait for every reader to complete before filling the next frame.
For asynchronous analysis, retain a BufferReference in the task and signal completion before reusing storage; retaining it alone does not prevent a data race.
A borrowed span must not outlive its owner.
Equality compares shared backing identity and range, so changing memory contents does not publish a new UI value; send an event or update a sequence number separately.
Notifications and other durable data must use owned values, not retained buffers.

Android libraries use `HuxerUIBufferReference.wrap(directBuffer)`, or `wrap(directBuffer, onRelease)` when external storage needs a final-release callback.
Only direct buffers are accepted, and wrapping freezes the current position-to-limit range without changing the original cursor.
`withBuffer(reader)` supplies a read-only ByteBuffer for that synchronous callback only; do not store the borrowed view.
`PlatformPayload.bufferReference(reference)` retains its own share, and `requireBufferReference()` returns a new wrapper that callers should close.
Closing the original wrapper does not invalidate payloads, slices, or active readers.
The optional callback runs once after the last backing share is released, on that thread; dispatch if the resource requires a particular thread.
Payload-owned Java shares may remain alive until garbage collection, so do not rely on the release callback for prompt per-frame scheduling; use explicit reader completion.
Retaining a DirectByteBuffer from an Android image does not prevent an independent `Image.close()`; coordinate that lifetime yourself.

```java
ByteBuffer storage = ByteBuffer.allocateDirect(4096);
try (HuxerUIBufferReference reference = HuxerUIBufferReference.wrap(storage)) {
    fillFrame(storage);
    reference.withBuffer(bytes -> analyze(bytes));
}
```

Apple libraries import `BufferReference` from `HuxerUIPlatform` (`HUXBufferReference` in Objective-C).
The initializer accepts a stable pointer, length, and an owning object; `withUnsafeBytes` borrows it only during the callback, and `slice(offset:length:)` retains a subrange.
ARC keeps the storage owner alive through copies and payload round trips; the owner can be released on the last reader's thread.

```swift
let storage = NSMutableData(length: 4096)!
let reference = BufferReference(bytes: storage.bytes, length: UInt(storage.length), owner: storage)
let payload = PlatformPayload.bufferReference(reference)
payload.bufferReference().withUnsafeBytes { bytes, length in
  analyze(bytes, length)
}
```

Do not resize that NSMutableData or construct a retained reference from a temporary Swift `Data.withUnsafeBytes` pointer.
Windows and Linux libraries use the shared C++ type directly; Web's JavaScript bridge explicitly rejects BufferReference without silently copying it.

## Windows

The default backend targets Windows 10 version 1607 or later and uses Win32, D3D11, Direct2D, DirectWrite, DXGI, and IMM32.
Build with MSVC and a supported Visual Studio installation.
Application packaging deploys the required Release VC++ runtime beside the application and installer; see [Packaging Applications](packaging.md#desktop-system-requirements).

The optional `HUXERUI_WINDOWS_7_COMPAT=ON` configuration targets Windows 7 SP1 with Platform Update by using capability-based fallbacks.
PlatformView composition requires DirectComposition and is unavailable when that capability is missing.
Windows 7 without Platform Update is unsupported.

Custom chrome keeps Win32 window behavior while HuxerUI draws the title-bar content and caption controls.
Windows 11 Snap Layout is available through the shared maximize-button geometry on the default backend.
System tray presentation uses the Windows notification area and restores its item after Explorer restarts.
Runtime camera and microphone permissions use AppCapability when present; package capability declarations remain application-owned.
Custom application URL schemes use `windows::RegisterUrlScheme()` and `UnregisterUrlScheme()` from `<huxerui/system.h>` for explicit current-user registration and cleanup, including in the Windows 7 compatibility backend. See [Windows URL scheme registration](packaging.md#windows-url-scheme-registration) for ownership, persistent registration, and default-app limitations.
Local notifications use the Windows system toast service without MSIX or Windows App SDK dependencies. Before `RunApplication()`, pass an application-owned App ID, display name, and fixed CLSID to `windows::RegisterLocalNotifications()` from `<huxerui/system.h>`; no CMake notification metadata is required. See [Windows notification registration](packaging.md#windows-notification-registration) for registration and explicit cleanup. Default presentation, scheduling, cancellation, and data-bearing activation are supported. An optional `windows::LocalNotificationTemplateProvider` passed during registration enables application-owned ToastGeneric XML templates; custom actions and in-place progress updates are not supported. The Windows 7 compatibility backend remains unavailable. Notification clicks enter `OnActivation()` even when they launch the process.

Native libraries include `<huxerui/windows/external_texture.h>` for `windows::PixelTexture` and `windows::D3D11Texture`.
`PixelTexture` copies straight-alpha RGBA8888 or BGRA8888 rows into premultiplied CPU storage.
`D3D11Texture` accepts one-mip, one-slice, single-sampled `DXGI_FORMAT_B8G8R8A8_UNORM` `D3D11_USAGE_DEFAULT` textures without CPU access.
It synchronously performs one GPU copy into an immutable shared snapshot, so the producer may reuse or release its source after `Publish()` returns.

```cpp
auto texture = std::make_shared<huxerui::windows::D3D11Texture>(huxerui::Size{320.0F, 180.0F});
texture->Publish({producer_texture, huxerui::windows::D3D11Texture::Alpha::Opaque});
```

Submit producer writes before publication and externally serialize use of the source device's immediate context.
Pixel row zero is interpreted as the logical top edge.
The producer and every renderer displaying the texture must use the same graphics adapter.
Renderers open the immutable shared snapshot directly, coordinate read access with its keyed mutex, and do not make a second GPU copy or silently read pixels back to CPU memory.
Renderer-local resource recreation can reopen the snapshot while its producer device remains valid.
If the producer device is removed, publish a replacement texture from a valid device before the next render.
`D3D11Texture` is unavailable when `HUXERUI_WINDOWS_7_COMPAT=ON`.

## macOS

The macOS backend requires macOS 12 or later.
The macOS backend uses AppKit, Core Graphics, Core Text, and `NSTextInputClient`.
Runtime-created windows, startup `extra_windows`, ownership, modality, and explicit retirement are available as a preview; see [Developing Multi-Window](../development/multi-window.md#macos-preview-validation) for CI coverage and the remaining interactive validation.
macOS applies `WindowOptions::initial_position` through AppKit, and `WindowHandle::Position()` reports the client-area origin in logical screen coordinates with the contract's top-left origin.
Transparent utility windows, passive display, topmost stacking, geometry requests, whole-window input passthrough, and drag-initiated native moves are available as described under [Floating utility windows](#floating-utility-windows).
Build with Xcode 26 or later and the macOS SDK; release CI uses Xcode 26.2. This toolchain requirement does not raise the macOS 12 deployment target.

Custom chrome extends application content into the title bar while preserving AppKit traffic lights and window behavior.
External file references preserve security-scoped access when required.
System tray presentation uses an AppKit status item and platform menu.
Camera and microphone permissions use AVFoundation and require the corresponding bundle usage descriptions.
Local notifications use User Notifications for authorization, immediate presentation, durable one-shot scheduling, cancellation, and primary-action activation.
Notification interactions enter `OnActivation()` even when they launched the process.
macOS supports only `DefaultNotificationPresentation`; template requests return `Unavailable`.
The installed SDK exposes AppKit PlatformModule, PlatformView, PlatformPayload, FileReference, BufferReference, and ExternalTexture contracts to Objective-C and Swift through `HuxerUIPlatform`.
FileReference payloads retain the shared grant and expose its path-backed `NSURL` as `fileURL` for native libraries.

Native libraries include `<huxerui/macos/external_texture.h>` for `macos::PixelBufferTexture` and `macos::MetalTexture`.
`PixelBufferTexture` retains an immutable `CVPixelBufferRef`; publish a different buffer before mutating later content.
`MetalTexture` synchronously copies level zero of a completed, non-framebuffer-only 2D BGRA8 or RGBA8 texture, so the producer may reuse or release its source after `Publish()` returns.

```objective-c++
auto texture = std::make_shared<huxerui::macos::MetalTexture>(huxerui::Size{320.0F, 180.0F});
texture->Publish({producer_texture, huxerui::macos::MetalTexture::Origin::TopLeft,
                  huxerui::macos::MetalTexture::Alpha::Premultiplied});
```

Finish producer command buffers before publication.
Use `Origin::BottomLeft` only when row zero represents the logical bottom edge, `Alpha::Opaque` when stored alpha must be ignored, and `Alpha::Straight` when RGB has not been multiplied by alpha.
Metal publication performs a GPU snapshot and is not a zero-copy handoff; rendering remains in the ordinary Image/Core Graphics path so transforms, clipping, opacity, overlays, scrolling, and multiple windows keep their normal semantics.

## Linux

The Linux backend requires GTK 4.14 or later and uses GTK for the window, event loop, GSK presentation, `GtkIMContext`, and backend selection.
Libepoxy provides OpenGL dispatch, Pango provides text layout, Cairo records ordinary drawing nodes, GIO provides platform services, and libsoup 3 provides HTTP.

Install the corresponding development packages before configuring CMake.
The SDK archive does not bundle distribution-owned GTK, libepoxy, Pango, Cairo, GIO, or libsoup libraries.
Official Linux SDK binaries require glibc 2.38 or later.
Release CI builds and tests on Ubuntu 24.04; meeting the glibc baseline alone does not establish validation on another distribution.
Application packages retain the distribution-owned runtime stack and may require newer system ABI versions according to their binaries; see [Packaging Applications](packaging.md#desktop-system-requirements).

Linux builds are provided for x86_64 and aarch64 hosts.
PlatformView is not implemented, and a platform accessibility bridge is not supported.
System tray presentation requires an active StatusNotifierItem watcher and host; `IsAvailable()` tracks hosts appearing or disappearing at runtime.

Native libraries include `<huxerui/linux/external_texture.h>` for `linux::PixelTexture`, `linux::GlTexture`, and `linux::GdkTexture`.
`PixelTexture` copies straight-alpha RGBA8888 or BGRA8888 rows and remains available to CPU producers.
`GlTexture` is the normal OpenGL publication path.
The producer keeps a `GL_TEXTURE_2D` with level-zero `GL_RGBA8` storage and makes its `GdkGLContext` current before publishing:

```cpp
#include <huxerui/linux/external_texture.h>

auto texture = std::make_shared<huxerui::linux::GlTexture>(huxerui::Size{320.0F, 180.0F});
texture->PublishCurrent({
    .texture_name = source_texture,
    .pixel_width = 320,
    .pixel_height = 180,
    .origin = huxerui::linux::GlTexture::Origin::BottomLeft,
    .alpha = huxerui::linux::GlTexture::Alpha::Opaque,
});
```

`PublishCurrent()` completes a GPU copy in the current context into an immutable texture managed through a private HuxerUI context in the producer's GL share group.
It constructs the GDK texture, submits the consumer fence, and owns final GPU cleanup; after the call returns, the producer may immediately update or delete `source_texture`.
Calls for one `GlTexture` are serialized by the producer, and the current context must share resources with the context used for its first publication.
Use `Origin::BottomLeft` for conventional OpenGL render output and `Origin::TopLeft` when the producer already stores logical row zero at texture coordinate zero.

`GdkTexture` is the advanced direct-publication path for a producer that already owns a complete immutable `::GdkTexture`, such as a `GdkDmabufTexture` or a texture supplied by another GTK subsystem.
`Publish()` retains that texture without downloading it and may invoke its final callback on the replacing thread, so the producer remains responsible for native synchronization, immutability, and callback-safe resource release.
DMA-BUF producers keep every plane fd valid for the `GdkDmabufTexture` lifetime and use formats advertised by the target `GdkDisplay`.
When an explicit producer fence is not directly carried by the texture builder, import its sync file into the DMA-BUF reservation before publication so GDK's implicit consumer synchronization observes completed writes.

The GTK Snapshot renderer inserts GDK texture nodes at the exact `DrawExternalTextureCommand` position among Cairo nodes.
Source crop, destination mapping, nearest or linear sampling, transforms, rectangular and path clips, node opacity, foreground drawing, and multiple windows retain the ordinary Image semantics.
HuxerUI performs no CPU readback on these GPU paths, but the active GSK renderer and graphics driver decide whether a concrete texture is imported directly or converted.
The `example_external_texture` Linux target creates and updates only the reusable producer GL texture; `GlTexture` hides the GDK builder, fence, immutable snapshot, and release callback.

## Web

The Web backend uses Emscripten, WebAssembly, Canvas 2D, browser input events, hidden text controls for IME, Fetch for HTTP, and browser-managed file storage.

Generated projects use the Emscripten version pinned by the installed HuxerUI SDK.
Run the generated output through `huxerui run web` or another HTTP server; loading the files directly with a `file:` URL is unsupported.
On Termux, `huxerui run web` starts a Python standard-library server on an available loopback port and passes the generated entry URL to `termux-open`; it does not use ADB or Emscripten's Android-device mode.
The pinned Emscripten tools, Python, and `termux-open` must be available on `PATH`.

Web libraries include `<huxerui/web/external_texture.h>` and publish open WebCodecs `VideoFrame` objects through `web::VideoFrameTexture`.
Pass the same shared texture to `Image`; `Publish()` clones the frame, so the caller may close its original immediately, and `Finish()` preserves the last published image while rejecting later publication.
Construction, publication, finish, and destruction run on the browser main thread.
Canvas2D and WebGL2 producers can both create frames with `new VideoFrame(canvas, {timestamp})`; capture a WebGL canvas in the drawing callback before its drawing buffer may be discarded.
Raw `WebGLTexture` and `GPUTexture` objects are not accepted: render their content into a supported Canvas image source first.
The framework does not require application-side pixel readback, but browser snapshotting, import, and color conversion may copy, so this path does not promise end-to-end zero-copy.
The `example_external_texture` Web build demonstrates Canvas2D and WebGL2 producers using the same texture API, overlay, clipping, and pause controls; it retains the Canvas2D example when WebGL2 is unavailable and reports missing `VideoFrame` support.

JavaScript platform libraries receive a retained `HuxerUI.FileReference` from `PlatformPayload.requireFileReference()`.
Call `await reference.getFile()` when a browser media or document API requires a `File`, and call `close()` when the library no longer needs its retained C++ capability share.
The public `PlatformPayload.encode()` remains a capability-free byte API; the framework's private bridge envelope carries FileReference companion entries and Web continues to reject ExternalTexture and BufferReference payloads.

Typed routed navigation can bind the authoritative `NavigationPath` to browser URL and history state.
Browser restrictions still govern clipboard, file pickers, autoplay, cross-origin requests, and storage persistence.
Camera and microphone permission state is queried through the Permissions API when supported; requesting access remains coupled to browser media acquisition and is not emulated by the shared permission API.
HuxerUI-rendered content does not have a semantic DOM accessibility bridge; DOM PlatformViews retain their browser-provided accessibility.

## Android

The Android backend requires API 23 or later and uses an Android View host, Canvas, StaticLayout, InputConnection, JNI, and platform accessibility APIs.
The generated Gradle project links the SDK-provided Android shared library and application C++ library for each configured ABI.

Build and run require an Android SDK, NDK, Java, Gradle wrapper dependencies, and a compatible emulator or device.
Insets, system-bar appearance, lifecycle, activation, file pickers, HTTP, PlatformView, FileReference payloads, and ExternalTexture are translated at the Android host boundary.
Camera and microphone requests use the Activity launcher and require manifest declarations owned by the application.
Local notifications use `NotificationManager`, an application-created channel on Android 8 or later, and `POST_NOTIFICATIONS` on Android 13 or later.
The application manifest supplies `org.huxerui.local_notification.channel_id`, `org.huxerui.local_notification.small_icon`, and an explicit non-exported `org.huxerui.HuxerUILocalNotificationReceiver` declaration when durable scheduling is required.
Create the configured channel before constructing `HuxerUIView`; HuxerUI does not inject declarations or create channels from C++ options.
Immediate presentation replaces a pending schedule and delivered notification with the same stable identifier, while durable one-shot scheduling uses an inexact alarm and does not survive device restart by contract.
A tap delivered to a new Activity becomes `StartupActivation()`; `onNewIntent()` delivers it through `OnActivation()`.
On API 24 or later, an application's `Application` class may implement `HuxerUILocalNotificationLayoutProvider` to map stable template identifiers to fresh compact, expanded, and heads-up `RemoteViews`.
HuxerUI retains the builder, channel, icon, tag, and activation `PendingIntent`, and scheduled alarms resolve the provider again after process restart.
Android still owns the surrounding notification chrome and may constrain the custom area.
`can_use_templates` is false on API 23 and when no provider is installed; an unsupported identifier returns `Unavailable` without system-layout fallback.
See [Local Notifications](../design/local-notifications.md#android) for the complete configuration and capability rules.

Generated applications keep the same Activity, `HuxerUIView`, and Runtime across orientation and window-size changes.
An application that hosts `HuxerUIView` in a custom Activity must declare the same handled changes on that Activity:

```xml
<activity
    android:name=".MainActivity"
    android:configChanges="orientation|screenSize|smallestScreenSize|screenLayout" />
```

This declaration does not preserve the Runtime across process death or configuration changes that are not listed, such as locale, UI mode, font scale, layout direction, or display density.
A custom host remains responsible for updating any other Android Views, Fragments, or alternative Android resources that depend on the handled configuration values.

Android libraries include `<huxerui/android/external_texture.h>` and choose the producer that matches their source.
`BitmapTexture` retains immutable `android.graphics.Bitmap` objects and remains on the Canvas path.
`GlTexture` synchronously copies `GL_TEXTURE_2D` content from the EGL context current during `PublishCurrent()`; supply a native acquire-fence fd when producer work is asynchronous, or publication waits for current GL work.
The producer may reuse or delete the source texture after `PublishCurrent()` returns.

```cpp
auto texture = std::make_shared<android::GlTexture>(Size{320.0F, 180.0F});
texture->PublishCurrent({
    .texture_name = texture_name,
    .pixel_width = 1280,
    .pixel_height = 720,
    .acquire_fence_fd = fence_fd,
    .origin = android::GlTexture::Origin::BottomLeft,
    .alpha = android::GlTexture::Alpha::Opaque,
});
```

`SurfaceStreamTexture` owns the SurfaceTexture/OES consumer and returns a local reference to a producer-facing `android.view.Surface` suitable for Camera or MediaCodec.
Its logical intrinsic size remains fixed while `SetDefaultBufferSize()` may change the requested physical buffer size.
Surface buffers follow Android's premultiplied-alpha convention, and `Finish()` releases the producer Surface and consumer while preserving the last latched frame.

```cpp
auto texture = android::SurfaceStreamTexture::Create(environment, Size{320.0F, 180.0F}, 1280, 720);
auto producer_surface = texture->Surface(environment);
camera->SetPreviewSurface(producer_surface.Get());
```

Applications pass all three concrete types to `Image` as `std::shared_ptr<ExternalTexture>`.
GPU-backed textures require a hardware-accelerated host window; HuxerUI does not silently read them back to CPU memory.

Java and Kotlin platform libraries receive a retained `HuxerUIFileReference` from `PlatformPayload.requireFileReference()` and call `uri()` for APIs that accept Android document resources.
Closing the wrapper releases only that Java share of the C++ capability; the payload bridge does not add broad storage permission or expose a parallel writability flag.

An Android arm64-v8a host SDK provides the `huxerui` CLI, `hcg`, and `hrc` as native Bionic executables for Termux.
Termux Android builds target the local `arm64-v8a` ABI, use the Termux `aapt2` executable, and still require an Android SDK platform and NDK layout compatible with Gradle `externalNativeBuild` on Termux.
The SDK installer does not install Java, Gradle dependencies, `aapt2`, the Android SDK, or the Android NDK; use `huxerui doctor android` to inspect those application-build prerequisites.
Termux diagnosis and setup do not require `sdkmanager`, platform-tools, or ADB because they are not part of the local-device path.
`huxerui run android` opens the generated APK in the Android system installer through `termux-open`; complete the confirmation and choose Open in the installer to start the application.

## iOS

The iOS backend requires iOS 15 or later and uses UIKit, Core Graphics, Core Text, `UITextInput`, and UIKit accessibility.
Build on macOS with Xcode 26 or later and an installed simulator runtime or paired device; release CI uses Xcode 26.2. This toolchain requirement does not raise the iOS 15 deployment target.

```bash
huxerui devices ios
huxerui open ios
huxerui run ios --device <id>
```

Physical-device builds use Xcode signing settings owned by the generated project and local developer configuration.
External files preserve security-scoped access when required.
Camera and microphone permissions use AVFoundation, require native usage descriptions, and can open the application settings page through UIKit.
Local notifications use User Notifications for authorization, immediate presentation, durable one-shot scheduling, cancellation, and primary-action activation.
Notification interactions enter `OnActivation()` even when they launched the process.
Embedded Notification Content Extensions declare stable `UNNotificationExtensionCategory` values; discovered categories enable template presentation and receive matching requests through `categoryIdentifier`.
The extension owns the expanded interface, while the ordinary banner/list UI remains system-controlled and HuxerUI retains notification identity, scheduling, cancellation, and activation semantics.
The iOS XCFramework exposes UIKit PlatformModule, PlatformView, PlatformPayload, FileReference, BufferReference, and ExternalTexture contracts to Objective-C and Swift through `HuxerUIPlatform`.
FileReference payloads retain the shared grant and expose its path-backed `NSURL` as `fileURL` for native libraries.

Native libraries include `<huxerui/ios/external_texture.h>` for the iOS forms of `PixelBufferTexture` and `MetalTexture`.
They have the same ownership, format, origin, alpha, synchronization, and Image rendering contract as their macOS counterparts; the C++ types remain in `huxerui::ios` rather than a shared Apple namespace.
Objective-C and Swift import the concrete objects as `HUXPixelBufferTexture`/`PixelBufferTexture` and `HUXMetalTexture`/`MetalTexture`.

## Shared behavior

Composition, state, reconciliation, layout, navigation, scrolling, gestures, text editing behavior, animation, resources, semantics generation, and retained scene construction live in shared C++.
Platform differences belong at explicit platform capability boundaries rather than in application components.

For internal mapping details, use the relevant document in the [Design Index](../design/README.md).
