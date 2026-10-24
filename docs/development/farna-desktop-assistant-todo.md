# Farna TODO: Floating Windows and Desktop Assistant Capabilities

Owner: Farna.
Status: Windows and Linux floating-window implementations and the assistant-orb example delivered; macOS remains a CI-qualified preview; broader qualification and optional services remain future work.

This backlog extracts reusable framework requirements from animated desktop companions and character-based assistants.
It belongs to the Farna fork and its [downstream integration policy](farna-integration.md), not to an upstream release plan.
The current implementation follows the separately approved Windows desktop-pet scope and the Linux/macOS port of its window contract; unchecked work below still requires its own scope.
Future capability descriptions are planning terms, not approved C++ declarations or platform support promises.

## Current delivery and remaining work

The Windows baseline provides an interactive transparent utility surface and the [desktop-pet example](../../examples/desktop_pet/README.md), styled as an assistant orb with animated wave ribbons.
It uses the existing Runtime, WindowHandle, retained NodeExtension animation, ordinary settings window, and system tray.
The example does not acquire audio or connect to a model service.

| Work area | Delivered Windows baseline | Remaining acceptance scope |
| --- | --- | --- |
| FD-01: window contract | WindowCapabilities, WindowRequestResult, creation-policy validation, application-thread requests, unsupported-host defaults, and disconnected-handle results | Qualify additional hosts and any future dynamic capability changes |
| FD-02: transparency | Premultiplied layered presentation, transparent clearing, native alpha hit testing, resize, and renderer resource recreation | Physical mixed-DPI displays, broader ExternalTexture combinations, and other desktop renderers |
| FD-03: utility and activation | Borderless non-resizable utility windows excluded from task switching, passive show, and explicit activation of an ordinary settings window | Accessibility, editing/IME scenarios, and additional desktop hosts |
| FD-04: stacking | Initial and mutable topmost requests with native failure results | Fullscreen/workspace interactions and broader owner/modal stacking qualification |
| FD-05: geometry | Runtime position and client-size requests, observed position, work-area snapshots, native drag, and explicit position recovery | Display-change subscriptions, persistent placement, monitor removal, and mixed-DPI recovery |
| FD-06: input | Zero-alpha passthrough to another process, independent whole-window passthrough, capture cancellation, and preservation of modal blocking | Arbitrary input regions independent of visual alpha, touch/pen, and other desktop hosts |
| FD-07: example | Geometric assistant orb, retained 30 fps wave animation, hidden/reduced-motion pausing, click/drag/context menu, settings, tray recovery, and opt-in lightweight hiding with window retirement and in-process preference restoration | Background decoder/producer qualification if introduced; broader device and compositor coverage |

The layered renderer reads back the complete small surface on every presented frame; it is limited to 4096 pixels per dimension and 16 million pixels in total.
PlatformView hosting is unavailable on this path; ordinary opaque windows retain their existing presentation paths.
The utility policies are implemented on Windows, on Linux through the GTK adapter with its capability set split truthfully between the X11 and Wayland backends, and as a CI-qualified macOS preview through AppKit, without physical-device validation; mobile and Web hosts retain explicit unavailable behavior.
The P0 checklist below retains the broader acceptance scope and must not be interpreted as a claim that every desktop backend or device has passed it.

### Validation and distribution

- Windows MSVC x64 Release framework and example builds, standalone public-header checks, and a separate clean configure/build pass.
- All four common CTest suites and the Windows native suite pass, including transparent pixels, silhouette clearing, cross-process input, passive focus, native drag, capture cancellation, modal blocking, and repeated retirement/recreation.
- The module-shell freshness check and CMake/mcpp build-parity check cover the shared declaration and build boundaries; gdi32 was already present in the mcpp Windows link interface.
- A temporary Windows 10/11 x64 portable ZIP contains the executable, resources, licenses, and matching app-local Release CRT; profiling is disabled, extracted file hashes match, and startup succeeds from an unrelated working directory with a system-only PATH.
- The ZIP remains a local build artifact; clean-machine execution, physical mixed-DPI displays, touch/pen, accessibility, fullscreen interaction, and actual Windows 7 execution are not qualified by that startup check.
- Linux validation ran locally on Fedora (GTK 4.22, Wayland session with XWayland): full Ninja build with tests and examples, all four common suites, the Linux platform suite including the new per-backend native floating-window tests, and a desktop-pet smoke run; one pre-existing bidi caret test failure and one pre-existing runtime-dependency segfault were verified against a pristine base checkout.
- macOS is a preview accepted through CI because no physical macOS device is available. Previous validation ran through the `macos-multi-window` workflow on both runners (macos-15 static, macos-15-intel shared): native suite including the two new floating-window scenarios, common and codegen suites, and the desktop-pet example build all passed; interactive behavior beyond CI (dragging behind other applications, Space and full-screen transitions, multiple displays) remains unqualified.
- Mobile and Web builds remain unavailable on these hosts; no equivalent native support is claimed for them.

### Next priorities

1. Qualify mixed-DPI movement, display removal, saved-position recovery, and fullscreen/topmost behavior on real Windows desktops.
2. Add display-change observation and independently shaped input regions through the existing window owners when their contracts are agreed.
3. Extend real-desktop qualification to the Linux and macOS ports: mixed-DPI movement, display changes, and the interactive macOS scenarios beyond automated CI.
4. Choose optional animation decoding, audio, capture, or model integration only from concrete application demand; these remain separate from the visual demo.

## Research basis and boundaries

The repository audit was performed on 2026-10-23 against the current checkout's public headers, shared implementation, desktop adapters, focused test sources, and owning documentation.
That initial audit inspected tests as contract evidence; the implemented Windows baseline is validated separately as described above.
Recheck the relevant files before implementing a task because this inventory does not freeze the SDK.

The application references reveal three useful groups of requirements:

| Reference | Requirements extracted for HuxerUI |
| --- | --- |
| [White Dragon desktop companion](https://github.com/Wunsch2048/codex-bailong-desktop-pet) and its [Linux adaptation](https://github.com/shiki-sl/bailong-linux) | Transparent floating windows, independent input regions, placement, quiet presentation, persistent tray recovery, bounded animated-frame playback, and separate character/status surfaces |
| [AIRI](https://github.com/moeru-ai/airi) and [Open-LLM-VTuber](https://github.com/Open-LLM-VTuber/Open-LLM-VTuber) | External character renderers, voice input/output, interruptible sessions, image acquisition, and lifecycle-bound native resources |
| [Super Agent Party](https://github.com/heshengtao/super-agent-party) | Optional native services for desktop integration and tool-backed applications; model selection, task execution policy, and multi-agent coordination remain application responsibilities |

These projects are requirements references, not dependencies or specifications to reproduce.
Their artwork, model files, licenses, service credentials, and product behavior are outside this backlog.
Keep future examples independent of pet assets, model providers, accounts, and running agents.

## Existing capabilities to reuse

An application-level feature is not evidence that HuxerUI needs another built-in component or service.
The following mechanisms already own substantial parts of the required behavior:

| Capability | Current evidence | Planning consequence |
| --- | --- | --- |
| Runtime-created windows, ownership, modality, and retirement | [app.h](../../include/huxerui/app.h), [window.h](../../include/huxerui/window.h), [multi-window design](../design/multi-window.md), [relationship design](../design/window-relationship.md) | Extend the existing window contract; do not introduce a companion host or a second event loop |
| Custom chrome, utility presentation, and native dragging | [window.h](../../include/huxerui/window.h), [window-chrome design](../design/window-chrome.md), [window tests](../../tests/runtime/window.cpp) | Windows, Linux, and macOS implement transparent utility surfaces with backend-honest capability reporting; further hosts require their own implementations and qualification |
| Placement, runtime geometry, and work areas | [window.h](../../include/huxerui/window.h), [platform placement guide](../guide/platforms.md#native-window-placement) | Windows, macOS, and Linux/X11 expose native geometry requests and work-area snapshots; Wayland owns placement and activation by protocol and reports them unavailable; display-change observation remains future work |
| Tray, visibility commands, close interception, and application quit | [app.h](../../include/huxerui/app.h), [tray design](../design/system-tray.md), [tray tests](../../tests/runtime/system_tray.cpp) | Reuse the tray; qualify availability and recovery rather than build another tray system |
| Retained animation, delayed frame requests, gestures, and reduced motion | [modifier.h](../../include/huxerui/modifier.h), [animation.h](../../include/huxerui/animation.h), [animation design](../design/animation.md), [gesture design](../design/gestures.md) | Character behavior belongs in a library or application using existing scheduling and interaction |
| Immutable external frames and pixel publication | [external_texture.h](../../include/huxerui/external_texture.h), [implementation](../../src/graphics/external_texture.cpp), [Windows](../../include/huxerui/windows/external_texture.h), [Linux](../../include/huxerui/linux/external_texture.h), [macOS](../../include/huxerui/macos/external_texture.h) | Feed decoded or rendered frames through ExternalTexture; publication does not require per-frame recomposition |
| Native extension and embedded-content mechanisms | [platform_registry.h](../../include/huxerui/platform_registry.h), [PlatformModule guidance](../../skills/huxerui-app-development/references/platform-modules.md), [module tests](../../tests/runtime/platform_module.cpp) | Use a typed PlatformModule for a missing native service, PlatformView for embedded native content, and ExternalTexture for frame production |
| Tasks, workers, HTTP streaming, files, storage, and clipboard | [task.h](../../include/huxerui/task.h), [http.h](../../include/huxerui/http.h), [file.h](../../include/huxerui/file.h), [clipboard.h](../../include/huxerui/clipboard.h) | Reuse asynchronous delivery and I/O; a model client or log reader does not justify another framework task system |
| Permissions and local notifications | [system.h](../../include/huxerui/system.h), [permissions design](../design/permissions.md), [notification design](../design/local-notifications.md) | Shared contracts exist; camera/microphone authorization is not media acquisition, and Linux notification delivery is currently unavailable |

### Documentation discrepancy requiring a separate decision

[The Linux platform guide](../guide/platforms.md) currently says PlatformView is not implemented, while [linux_platform_view.h](../../platform/linux/linux_platform_view.h), [linux_platform_view.cpp](../../platform/linux/linux_platform_view.cpp), and [linux_adapter.cpp](../../platform/linux/linux_adapter.cpp) contain its implementation and integration.
Resolve the intended support and qualification status with the owner before changing that declaration or promising embedded-renderer compatibility.
This backlog records the discrepancy without treating the mechanism as wholly missing or declaring the implementation supported.
The macOS multi-window preview and the outstanding Linux placement validation in [Developing Multi-Window](multi-window.md) likewise remain explicit qualification boundaries.

## Ownership and public API rules

- One platform-derived Runtime owns application services, dispatch, and ordinary timers; each native attachment remains one platform-derived UiWindow.
- Extend WindowOptions, WindowHandle, the existing WindowService, and genuine native virtual operations for window behavior; keep operating-system objects in their platform implementations.
- Keep requested configuration distinct from observed native state; successful request submission must not imply that the window manager applied it.
- Decide creation-only versus mutable policies, validation, failure results, thread requirements, and stale-handle behavior before approving API names.
- Use capability-specific availability and operation results, following existing service patterns; do not introduce a general capability registry, platform enum, property bag, or parallel callback convention.
- Install native service factories through application hooks and expose typed services; use window hooks only when the capability actually belongs to a window.
- Retained capabilities remain bound to their original Runtime, and late callbacks must not reach retired windows or a replacement Runtime.
- Future accepted behavior belongs in the owning guide and design document; this checklist remains a downstream planning artifact.

## P0: Floating-window foundations

These tasks form the smallest reusable framework increment for floating palettes, overlays, inspectors, status surfaces, and desktop companions.
They do not require an AI backend, audio stack, character engine, or new image codec.

### FD-01: Specify window capability and result semantics

- [ ] Define availability for transparency, utility presentation, activation policy, stacking, geometry control, and native input regions at the narrowest window boundary.
- [ ] Distinguish unsupported behavior, host-denied or unapplied requests, runtime failures, and invalid caller configuration; preserve existing exception categories and command behavior.
- [ ] Specify dynamic capability changes, application-thread access, shutdown behavior, and requested-versus-applied observation without mirroring the native window manager in shared state.
- [ ] Update the owning [window-chrome](../design/window-chrome.md), [multi-window](../design/multi-window.md), and [relationship](../design/window-relationship.md) contracts before implementation; their current non-goals do not authorize these additions.

Primary files: [window.h](../../include/huxerui/window.h), [app.h](../../include/huxerui/app.h), [window_internal.h](../../src/application/window_internal.h), [window.cpp](../../src/application/window.cpp).
Accept with focused validation, unsupported-host, disconnected-handle, and request-result tests in [window.cpp](../../tests/runtime/window.cpp) and [multi_window.cpp](../../tests/runtime/multi_window.cpp).

### FD-02: Add opt-in per-pixel window transparency

- [ ] Specify a transparent native surface independently of View opacity, rounded clipping, custom chrome, and application-internal backdrop effects.
- [ ] Audit the complete presentation path: shared root/background painting, renderer targets, alpha representation, native backing surface, compositor submission, resizing, and device recreation.
- [ ] Reuse a compatible native composition path where available; ordinary Windows swap chains currently select ignored alpha, while the PlatformView composition path already uses premultiplied alpha in [win32_renderer.cpp](../../platform/windows/win32_renderer.cpp).
- [ ] Qualify GTK window styling and opaque regions, AppKit window/view opacity, and Windows composition or layered presentation separately; preserve the Windows 7 compatibility capability policy and existing platform minimums.

Accept with transparent and opaque controls, soft partial-alpha edges, changing silhouettes without trails, external textures, nested clipping, resize, mixed DPI, and device-loss recovery.
Include a real desktop behind the native window; an offscreen screenshot of transparent pixels does not establish native transparency.
Coordinate compositing checks with [FG-01 and FG-02](farna-graphics-todo.md#foundation-and-backend-qualification); desktop backdrop capture remains a separate capability.

### FD-03: Add utility-window and activation policies

- [ ] Define undecorated utility presentation, taskbar/application-switcher participation, resize affordances, and show-without-activation as explicit native policies.
- [ ] Keep ordinary Custom chrome behavior intact; a zero-height title bar is not a substitute for a utility-window contract.
- [ ] Specify whether a utility surface can later accept keyboard/IME focus, or whether editing opens an ordinary owned window; retain an explicit user action for activation.
- [ ] Preserve owner relationships, modality, accessibility exposure, and application shutdown when a window's task-switcher presence changes.

Primary adapters: [Win32](../../platform/windows/win32_adapter.cpp), [AppKit](../../platform/macos/appkit_adapter.mm), and [GTK](../../platform/linux/linux_adapter.cpp).
Accept with focus remaining in an unrelated editor during passive display, deliberate activation and text entry, owner minimization, close interception, and recovery from the existing tray.

### FD-04: Add runtime stacking and qualify workspace behavior

- [ ] Specify mutable above-normal presentation independently of the existing rule that an owned window stays above its owner.
- [ ] Keep normal visibility, activation, fullscreen, ownership, and input blocking distinct; define restoration after native minimization or fullscreen transitions.
- [ ] Investigate workspace/fullscreen participation only where a concrete native policy exists; expose unsupported results rather than infer support from the operating-system name.
- [ ] Make any automatic yield-to-video behavior application-owned; an explicit application preference must remain authoritative.

Accept with owner changes, multiple ordinary applications, native fullscreen, repeated toggles, modal windows, native surface recreation, and retirement.
Foreground application inspection and video recognition are optional services under FD-15, not part of the stacking setter.

### FD-05: Extend geometry and display information

- [ ] Define runtime client-size and position requests, observed geometry changes, display bounds/work areas, display scale, and display-change observation through native owners.
- [ ] Keep screen coordinates, client coordinates, outer decoration geometry, physical pixels, and DIPs explicit; preserve negative origins and fractional scaling.
- [ ] Specify compositor-controlled placement, rejected requests, asynchronous completion, minimum sizes, and reentrant move/resize callbacks.
- [ ] Reuse native drag initiation for user drags; reserve programmatic movement for hosts that support it and keep wandering, throwing, anchoring, and collision rules application-owned.

Accept with mixed-DPI displays, monitor removal, resolution/work-area changes, coordinates outside native integer range, saved-position recovery, and close or retirement during a drag.
Extend the existing [Windows](../../tests/platform/windows/multi_window_native.cpp), [Linux](../../tests/platform/linux/multi_window_native.cpp), and [macOS](../../tests/platform/macos/multi_window_native.mm) native tests; do not infer equivalent geometry support from a common API.

### FD-06: Add native input regions and whole-window passthrough

- [ ] Specify full, empty, and shaped native input regions independently of visual transparency, View hit testing, gesture eligibility, and modal input blocking.
- [ ] Define coordinate conversion, rounding, resize behavior, region updates, restoration, and the interaction between whole-window passthrough and a supplied region.
- [ ] Deliver pointer Cancel and release capture consistently when input policy changes mid-gesture; compose the policy with modality instead of accidentally re-enabling a blocked window.
- [ ] Let applications derive a bounded region from already available CPU image data when needed; avoid compulsory GPU readback or per-frame native window resizing.

Accept with clicks reaching an unrelated process through excluded regions, opaque and translucent pixels remaining visually unchanged, transformed content, active capture, touch/pen paths where available, disabling, hide/show, and unmount.
A View ignoring a pointer event or a Win32 hit-test-only fixture does not prove native passthrough to other applications.
AppKit whole-window ignoresMouseEvents and GTK input regions are implementation candidates, not proof of equivalent arbitrary-region support on every host.

### FD-07: Qualify a reusable floating-window example

- [ ] After FD-01 through FD-06, add an asset-independent example using existing composition, owned windows, tray recovery, pointer handling, and retained frame scheduling.
- [ ] Use generated geometric artwork or a small repository-owned fixture; demonstrate an interactive shape, excluded input area, passive status surface, and explicit activation of a normal settings window.
- [ ] Verify hidden/inactive producer throttling, coalesced frame publication, bounded memory, and shutdown while background work is pending through the existing ExternalTexture and Task lifetimes.
- [ ] Register an example such as example_floating_window using the existing examples structure and huxerui_add_example; describe actual backend availability in the example.

This is an integration and qualification task, not a new character framework.
Extend [animation tests](../../tests/runtime/animation.cpp), [texture tests](../../tests/unit/external_texture.cpp), [tray tests](../../tests/runtime/system_tray.cpp), and native fixtures where failures identify a real contract gap.

## P1: Optional reusable desktop services

These tasks may be developed independently after a consumer and a bounded native contract are agreed.
Start through existing extension mechanisms when the SDK does not already own the behavior; promote a service into the built-in application surface only when its reuse and ownership justify that decision.

### FD-08: Evaluate an animated-raster library

- [ ] Provide an optional decoder/player prototype for transparent WebP, GIF, or APNG using an explicitly selected external library and existing ExternalTexture publication.
- [ ] Specify timestamps, loops, frame disposal/composition, seeking, cancellation, bounded decode queues, cache budgets, resize, hidden-state pausing, and reduced motion.
- [ ] Reuse Windows/Linux PixelTexture and macOS PixelBufferTexture behind the consuming library; evaluate a shared publication convenience only after repeated consumers establish a need.
- [ ] Keep ImageAsset's current PNG/JPEG contract unchanged unless a separate public resource-format proposal also updates tooling, metadata validation, renderer decoding, packaging, and tests.

Evidence: [resource.h](../../include/huxerui/resource.h), [resource implementation](../../src/resources/resource.cpp), and [resource design non-goals](../design/resources.md#non-goals).
Accept with malformed inputs, oversized animations, variable frame durations, stale producer cancellation, repeated replacement, and no UI-thread decoder joins.

### FD-09: Evaluate global shortcut registration

- [ ] Define application-owned shortcut registration, conflict/unavailable results, activation delivery, unregister-on-retirement, and native rebinding or revocation.
- [ ] Reuse Task and dispatcher delivery; keep focused-window keyboard events distinct from global shortcuts and general keyboard monitoring.
- [ ] Evaluate supported Win32/AppKit mechanisms and Linux's GlobalShortcuts portal where present; do not require a compositor-wide input hook to provide a shortcut.

Accept with duplicate bindings, user cancellation, service disappearance, late events, windowless operation, and cleanup after failed initialization.

### FD-10: Evaluate login launch and outgoing URI activation separately

- [ ] Investigate opt-in login-launch status, registration, update, and removal using a stable installed application identity; preserve external user choices and keep installer/package declarations authoritative.
- [ ] Specify a separate outgoing URI opener with native-handler availability, asynchronous result semantics, and application-owned URI policy; incoming ApplicationActivation and navigation are already present.
- [ ] Keep launch-on-another-process, watched application names, full command construction, and product autostart preferences in the consuming application.

Accept with moved or upgraded installations, failed registration, disabled entries, cancellation, missing URI handlers, and no writes outside the application's own managed entry.
These two capabilities do not require a general background-agent host or a new application lifecycle.

### FD-11: Evaluate a native credential-store service

- [ ] Define application-scoped secret identity, read/write/delete, locked-store and unavailable results, cancellable prompts, and Runtime-bound callbacks.
- [ ] Evaluate native Windows credential/protection facilities, Apple Keychain, and Linux Secret Service without treating their storage and prompt semantics as identical.
- [ ] Keep ordinary settings in existing storage; do not silently persist secrets as plaintext when the native store is unavailable.

Accept with locked or missing stores, user cancellation, duplicate identities, interrupted updates, application isolation, and redacted diagnostics.
Model provider accounts and credential migration rules remain application-owned.

### FD-12: Complete desktop notification qualification

- [ ] Evaluate a Linux transport through the existing LocalNotification contract, with capability reporting for presentation, activation, and scheduling rather than a second notification API.
- [ ] Qualify tray watcher loss/recovery and hidden-window recovery using the already implemented StatusNotifierItem transport.
- [ ] Define application fallback when notifications or a tray host are unavailable; an in-window reminder is application UI, not proof of native notification delivery.

Evidence: [system.h](../../include/huxerui/system.h), [notification tests](../../tests/runtime/local_notification.cpp), [tray implementation](../../platform/linux/linux_system_tray.cpp), and [tray design](../design/system-tray.md).
Accept with no desktop notification service, notification dismissal, duplicate identifiers, activation after UI retirement, and runtime shutdown.
Persistent scheduled delivery must meet the existing contract; a process timer must not pretend to survive application exit.

## P2: Media and desktop-observation extensions

These are independent native capabilities with distinct sessions, grants, and resource lifetimes.
Their absence does not block floating UI, and camera/microphone permission status alone does not provide any of them.

### FD-13: Evaluate audio playback and acquisition modules

- [ ] Prototype separate typed playback and microphone-input capabilities through PlatformModule; define stream formats, clock domains, device selection, interruption, cancellation, and bounded buffering.
- [ ] Keep device callbacks off UI work, resume application Tasks through the original dispatcher, and retire streams before native audio facilities disappear.
- [ ] Support stopping speech promptly and switching devices; keep voice activity detection, speech recognition, synthesis providers, dialogue policy, and lip-sync interpretation in the consuming library/application.

Accept with device removal, denied or revoked access, no device, underrun/overrun, stop during streaming, and late callbacks after retirement.
Do not add an audio component or generic media host merely to expose platform acquisition.

### FD-14: Evaluate camera, screenshot, and screen-stream acquisition

- [ ] Specify independent session-based acquisition capabilities, with actual native authorization and selection rather than an inferred global permission flag.
- [ ] Define source selection, crop/scale coordinates, frame timestamps, cancellation, revocation, producer/consumer synchronization, and shutdown for each media source.
- [ ] Reuse ExternalTexture for previews and BufferReference or owned bytes for analysis with explicit completion lifetimes; retained memory does not itself prevent producer overwrite.
- [ ] Evaluate Linux portals/PipeWire and appropriate Windows/Apple acquisition APIs behind platform modules; distinguish a one-shot screenshot from a continuing stream.

Accept with cancellation before the first frame, resize, display/source removal, revoked grants, hidden preview, format/stride changes, and no late frames after shutdown.
Desktop screenshots and external-window capture are separate from application-internal DrawBackdrop; coordinate with [FG-17](farna-graphics-todo.md#deferred-capabilities).

### FD-15: Evaluate desktop-state observation only for concrete consumers

- [ ] Separate foreground application identity, fullscreen state, workspace changes, and global pointer position; document exactly which observation a native host can provide.
- [ ] Define event delivery, scope, unavailable results, revocation, and bounded polling only when a native subscription is unavailable and polling is acceptable.
- [ ] Keep browser video detection, process-name rules, application matching, attention priorities, and automatic yield decisions outside shared Runtime.

Accept with application switching, permission loss, multiple monitors, minimized windows, subscriber retirement, and hosts that intentionally do not expose global state.
Generic control of other applications through accessibility or synthetic input is not implied by HuxerUI's own semantic accessibility bridge; assess such automation as an application/tool library separately.

### FD-16: Evaluate process and IPC helpers outside UI ownership

- [ ] Establish whether consumers need a portable dependency or a reusable optional library for subprocess pipes, exit observation, timeouts, local sockets/named pipes, and bounded message framing.
- [ ] Define argv/environment handling, cancellation, backpressure, process-tree cleanup, inherited-handle policy, endpoint identity, and final cleanup before selecting a public abstraction.
- [ ] Adapt completion to existing TaskScope and dispatch; PlatformChannel's language bridge is not an arbitrary cross-process transport, and HTTP streaming is not a subprocess API.

Accept with partial reads/writes, EOF, oversized messages, cancellation during startup, child failure, shutdown, and unavailable transports.
Codex/Claude protocols, Python-worker messages, MCP, model routing, and agent permissions remain client libraries or application logic.

## P3: Third-party character-renderer qualification

### FD-17: Demonstrate existing renderer extension paths before adding API

- [ ] Integrate one bounded third-party or synthetic renderer through ExternalTexture; use PlatformView only when native embedded content must own focus/input and the backend is qualified.
- [ ] Verify immutable frame publication, device/context loss, alpha, scale, visibility, capture synchronization, input ownership, and release ordering against existing texture/view contracts.
- [ ] Document native dependencies and CMake/mcpp application packaging; engine licenses and model assets belong to the integration library and application.

Live2D, VRM, WebGL engines, avatar rigs, facial parameters, and character state machines are not new built-in View types by default.
Add a framework primitive only when a real integration exposes a missing generic contract that cannot be implemented through existing extension points.

## Desktop backend implementation directions

This matrix covers the desktop backends reviewed here, not a closed list of platforms or a delivered support matrix.
Every entry is an investigation direction; shared contracts must also define honest unsupported behavior for other hosts.

| Area | Windows direction | macOS direction | Linux direction |
| --- | --- | --- | --- |
| Transparent presentation | Audit DirectComposition or layered-window presentation and renderer fallback | Coordinate NSWindow, content-view, and backing opacity | Coordinate GTK/GSK/Cairo output, window CSS, and native opaque-region claims |
| Utility role and activation | Appropriate window styles and show/activation operations | Appropriate NSWindow/NSPanel presentation and key-window policy | GTK/GDK capabilities and backend-specific native requests where available |
| Stacking and placement | Extend current native geometry path with checked DIP conversion | Extend AppKit operations while retaining the current preview qualification | Reuse GTK-managed windows; keep X11-specific work in the Linux adapter and report compositor-owned restrictions |
| Native input exclusion | Validate real cross-process delivery, including capture and native child windows | Whole-window exclusion is a candidate; arbitrary regions require separate proof | Probe GDK input-shape support; distinguish full/default and empty regions |
| Global shortcuts and capture | Capability-specific native services | Capability-specific native services and grants | Probe the installed portal/backend and preserve session/grant lifetime |

GTK4 removed several backend-independent placement and stacking APIs; a common setter cannot manufacture equivalent window-manager authority.
For local Linux development, choose a concrete X11 or XWayland configuration and qualify it first; broader Wayland parity is a separate optional scope.
Do not silently force another backend or mutate desktop settings, and do not add compositor protocols to shared Runtime.
Mobile and Web capability mappings may remain unavailable where they do not own a native desktop window; adding desktop features does not raise existing platform minimum versions.

## Suggested delivery order

1. Agree FD-01 semantics and resolve the PlatformView documentation discrepancy; specify one initial host/backend and explicit unavailable results.
2. Implement FD-02, FD-03, and FD-06 together far enough to validate a transparent, interactive utility surface against another real application.
3. Add FD-04 and FD-05, then finish FD-07 with tray recovery, lifecycle, and available-desktop qualification.
4. Select FD-08 through FD-12 individually from actual application demand; do not couple a basic floating-window release to all optional services.
5. Keep FD-13 through FD-17 as independent extension/library projects until consumers establish a shared primitive requirement.

## Acceptance and contribution gates

- Each implementation patch identifies its public behavior, affected platforms, owner, fallback or unavailable behavior, and focused tests; the unchecked tasks above are not blanket implementation approval.
- Public header changes require self-contained header checks, affected runtime/common tests and examples, the current-host build, and every affected platform build available locally; inspect tests/runtime, tests/unit, and the relevant tests/platform directory before selecting commands.
- Native presentation, focus, cross-process input, display topology, and compositor behavior require real-host checks; windowless tests, offscreen alpha, and successful compilation cannot establish them.
- Exercise replacement, compatible recomposition where relevant, unmount/retirement, pointer Cancel, disabled paths, deterministic animation time, stale handles, asynchronous cancellation, and resource bounds.
- Keep new shared declarations platform-neutral, use native Runtime/UiWindow subclasses for native operations, and retain English HuxerUI diagnostics and existing validation categories.
- A new public header updates the umbrella header, public-header checks, applicable packaging, and owning documentation; regenerate the module shell through huxerui-module-gen rather than editing generated output.
- Framework source, define, and platform-link changes update both CMake and mcpp; build-system or packaging changes require incremental validation plus a separate clean configure/build using an appropriate toolchain.
- Report unavailable platforms and preview limitations explicitly, finish with git diff --check and git status --short, and keep Farna-only planning out of upstream contribution patches unless requested.

Model providers, persona/memory policy, ASR/TTS models, multi-agent orchestration, Codex status parsing, quota arithmetic, avatar animation selection, and self-update policy remain application or integration-library concerns.
These can consume HuxerUI's native capabilities without becoming framework-owned business logic.

## Native research references

- [Win32 window features](https://learn.microsoft.com/en-us/windows/win32/winmsg/window-features): window types, layered presentation, activation, ownership, and geometry.
- [GTK4 migration guide](https://docs.gtk.org/gtk4/migrating-3to4.html): removed placement/stacking operations and backend boundaries.
- [GDK input regions](https://docs.gtk.org/gdk4/method.Surface.set_input_region.html): native clickability independent of visual alpha and input-shape capability probing.
- [GlobalShortcuts portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.GlobalShortcuts.html): session-based shortcut registration.
- [ScreenCast portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.ScreenCast.html): capture sessions and native frame-source acquisition.
- [Desktop Application Autostart](https://specifications.freedesktop.org/autostart/latest/): desktop-entry registration and user overrides.
- [Secret Service](https://specifications.freedesktop.org/secret-service/latest/): secret identity, storage, locking, and prompts; review the deployed service/interface version before implementation.
