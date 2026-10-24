# Desktop pet

A small desktop companion styled as a glass assistant orb, drawn entirely with existing HuxerUI paint commands.
Windows and Linux are supported; macOS is a CI-qualified preview.
Layered cyan, blue, and violet wave ribbons flow inside a dark translucent sphere; click to brighten and energize the waves, drag to move, and right-click for settings or exit.
Wave motion uses retained paint state capped at 30 frames per second without recomposing the UI; it pauses while hidden or disabled and honors reduced motion.
The visual is a demonstration and does not acquire microphone audio.
The tray icon reuses the repository-owned Window Chrome example fixture; no character assets, network accounts, or model services are needed.

## Build and run

Use the repository's configured Windows MSVC build:

```powershell
cmake --build build/check --config Release --parallel 4 --target example_desktop_pet
& ./build/check/bin/Release/example_desktop_pet.exe
```

On Linux, use a configured GTK build:

```sh
cmake --build build/linux-multi-window-clean --parallel 3 --target example_desktop_pet
./build/linux-multi-window-clean/bin/example_desktop_pet
```

The same target builds on macOS through the macOS Multi-Window Preview CI workflow; no physical macOS device has been used to qualify it.

The tray menu provides show, hide, position recovery, settings, and quit.
The ordinary settings window can receive keyboard focus and change wave-animation, topmost, and lightweight-mode preferences.
Retiring the pet also retires its owned settings window; the Runtime and tray remain alive, and Show pet creates a new attachment.
If a tray host is unavailable, hiding and explicit retirement are disabled in the example, a mounted hidden pet is shown again, and right-click Quit remains available.

### Lightweight tray mode

Lightweight mode is off by default and can be enabled in Settings when a tray host is available.
Ordinary hiding keeps the native window and rendering resources for fast restoration.
With lightweight mode enabled, Hide pet and intercepted close requests retire both the pet and settings windows through WindowHandle::Retire, releasing the UI trees and native rendering resources while the Runtime and tray continue running.
Enabling the mode for an already hidden pet also retires its windows.
Tray activation recreates the orb with its last in-process position, energized state, animation preference, and topmost preference; wave phase restarts.
The settings window can be opened from the tray while the pet is retired, including changing the next window's topmost preference.
Lightweight hiding also closes any such settings window.
The process still retains application state, the tray, loaded libraries, and allocator/driver caches; retirement does not promise zero memory use or an immediate matching drop in the operating system's working-set counter.
Preferences and position are not persisted across process exit.

## Native behavior and limits

The pet uses a transparent, borderless utility window and requests topmost and passive display when it is created.
Application setup queues the first pet through the same creation path as tray recovery, after application services freeze.
On Wayland, unsupported passive display and topmost policies fall back to compositor-controlled activation and stacking; Windows, macOS, and X11 retain the requested policies.
Windows layered publication preserves premultiplied alpha, and zero-alpha pixels let native mouse input reach windows belonging to other processes.
The translucent outer rim remains interactive.
Whole-window SetInputPassthrough is available separately; the sample keeps it off so the body and context menu remain usable.
Arbitrary input regions independent of the painted silhouette are not implemented by this example.

The layered path reuses the existing GPU scene renderer and performs a complete CPU readback on each presented frame for native alpha hit testing.
This is a bounded small-surface implementation, not a claim of transfer-free rendering: surfaces are limited to 4096 pixels per dimension and 16 million pixels in total.
PlatformView hosting is unavailable on this presentation path; immutable ExternalTexture frames can still be drawn.
Ordinary opaque windows retain the swap-chain and DirectComposition paths.
The example target is registered on desktop Windows, Linux, and macOS only.
Linux uses GTK scene composition without the Windows readback path; X11 exposes logical client coordinates, native stacking, and whole-window input passthrough.
X11 position recovery uses the current monitor's work area, excluding desktop panels; Wayland does not expose global placement or a work-area query, so position recovery is unavailable there.
macOS uses transparent AppKit panels and remains a preview accepted through CI; physical-device dragging, focus, Spaces, fullscreen, and multiple displays remain unqualified.
Monitor-change subscriptions, persistent placement, arbitrary independent input regions, and character decoders remain follow-up scopes.

## Validation

The common Window tests cover invalid configuration, unavailable operations, disconnected handles, retained wave animation, hidden/reduced-motion scheduling, lightweight retirement/recreation, preference restoration, absent-tray recovery, and replacement safety during queued retirement.
The Windows transparent-surface renderer fixture checks premultiplied pixels, silhouette clearing, resize, and resource recreation.
The hidden native test creates its own second-process target to verify actual desktop alpha, passive focus, and excluded mouse input:

```powershell
& ./build/check/bin/Release/huxerui_runtime_tests.exe "FloatingWindow*,DesktopPet*"
& ./build/check/bin/Release/huxerui_windows_tests.exe "[transparent-surface]"
& ./build/check/bin/Release/huxerui_windows_tests.exe "[win32-native-pet]"
```

The Windows native case temporarily moves the pointer over its own test windows and restores the previous pointer position when it finishes.
Linux geometry scenarios can run without driving the pointer; run them on Wayland and X11, including a scaled X11 surface:

```sh
GDK_BACKEND=wayland ./build-farna-debug/bin/huxerui_linux_tests '[linux-native-floating-geometry]'
GDK_BACKEND=x11 GDK_SCALE=2 ./build-farna-debug/bin/huxerui_linux_tests '[linux-native-floating-geometry]'
```

The separate `[linux-native-floating]` case drives pointer input and should run in an isolated desktop session.
Mixed-DPI physical monitors, touch/pen, accessibility, fullscreen interactions, and actual Windows 7 execution require their own native qualification.
