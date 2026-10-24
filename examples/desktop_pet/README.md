# Desktop pet

A small Windows desktop companion styled as a glass assistant orb, drawn entirely with existing HuxerUI paint commands.
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

The tray menu provides show, hide, position recovery, settings, and quit.
The ordinary settings window can receive keyboard focus and change wave-animation and topmost preferences.
Retiring the pet also retires its owned settings window; the Runtime and tray remain alive, and Show pet creates a new attachment.
If a tray host is unavailable, hiding and explicit retirement are disabled in the example, a mounted hidden pet is shown again, and right-click Quit remains available.

## Native behavior and limits

The pet uses a transparent, borderless utility window, initially topmost and shown without activation.
Windows layered publication preserves premultiplied alpha, and zero-alpha pixels let native mouse input reach windows belonging to other processes.
The translucent outer rim remains interactive.
Whole-window SetInputPassthrough is available separately; the sample keeps it off so the body and context menu remain usable.
Arbitrary input regions independent of the painted silhouette are not implemented by this example.

The layered path reuses the existing GPU scene renderer and performs a complete CPU readback on each presented frame for native alpha hit testing.
This is a bounded small-surface implementation, not a claim of transfer-free rendering: surfaces are limited to 4096 pixels per dimension and 16 million pixels in total.
PlatformView hosting is unavailable on this presentation path; immutable ExternalTexture frames can still be drawn.
Ordinary opaque windows retain the swap-chain and DirectComposition paths.
Linux and macOS do not yet implement the new utility policies, and this example target is registered only on Windows.
Monitor-change subscriptions, persistent placement, arbitrary independent input regions, and character decoders remain follow-up scopes.

## Validation

The common Window tests cover invalid configuration, unavailable operations, disconnected handles, retained wave animation, and hidden/reduced-motion scheduling.
The Windows transparent-surface renderer fixture checks premultiplied pixels, silhouette clearing, resize, and resource recreation.
The hidden native test creates its own second-process target to verify actual desktop alpha, passive focus, and excluded mouse input:

```powershell
& ./build/check/bin/Release/huxerui_runtime_tests.exe "FloatingWindow*,DesktopPet*"
& ./build/check/bin/Release/huxerui_windows_tests.exe "[transparent-surface]"
& ./build/check/bin/Release/huxerui_windows_tests.exe "[win32-native-pet]"
```

The native case temporarily moves the pointer over its own test windows and restores the previous pointer position when it finishes.
Mixed-DPI physical monitors, touch/pen, accessibility, fullscreen interactions, and actual Windows 7 execution require their own native qualification.
