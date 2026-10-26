# Farna TODO: Linux Accessibility

Owner: Farna.
Status: downstream future work, to be undertaken when an interactive Linux validation environment is available.

This backlog records the Farna fork's intent to maintain Linux accessibility using HuxerUI's existing shared semantics.
It does not declare an implemented Linux accessibility bridge, change upstream's roadmap, or authorize implementation or dependency installation by itself.
Keep this document and its index entry out of upstream contribution patches unless upstream requests them.
Follow the [downstream integration policy](farna-integration.md) when carrying the implementation and preparing independently useful upstream patches.

## Existing foundation and missing boundary

[Semantics and Accessibility](../design/semantics.md) owns the current shared contract and platform support declarations.
The public [semantics API](../../include/huxerui/semantics.h) already supplies roles, names, values, states, stable node identities, hierarchy, bounds, focus, actions, collections, virtual descendants, and secure-text redaction.
UiWindow publishes an immutable `SemanticFrame` and accepts validated requests through `PerformSemanticAction()`.
The existing Windows, Android, AppKit, and UIKit adapters provide implementation references, with different coverage and validation limits documented in the design.

The [Linux adapter](../../platform/linux/linux_adapter.cpp) does not consume `FrameCommit::semantic_frame` when committing its native frame.
Unless the environment explicitly selects another backend, initialization sets `GTK_A11Y=none` because the host does not project HuxerUI controls into GTK accessibility.
Removing that setting alone would expose the GTK host without constructing the missing HuxerUI semantic subtree.
The current design explicitly excludes Linux AT-SPI support; implementation must update that declaration to describe the delivered subset and its remaining limits.

## Maintenance and validation responsibilities

An interactive Linux environment is a major maintenance requirement, but it is not the only source of complexity.
Stable object lifetime, action routing, text offsets, event ordering, GTK version compatibility, and native subtree integration require implementation work even when a device is available.
AI assistance can perform much of the source audit, bridge implementation, automated test authoring, build integration, failure analysis, and regression maintenance.
It cannot establish successful screen-reader use or desktop compatibility without observing those environments, and compilation alone is not sufficient evidence.

| Work | Expected approach |
| --- | --- |
| Shared semantics and bridge logic | Reuse executable tests and existing adapters; automate deterministic checks and lifecycle regressions. |
| AT-SPI discovery and operations | Query the real accessibility bus with an inspector and client tests, including action results and change events. |
| Screen-reader behavior | Exercise navigation, announcements, focus, editing, and recovery with Orca; include human review of practical usability. |
| Desktop and coordinate behavior | Validate native KDE Wayland, X11 or Xwayland, scaling, and multiple monitors; keep each result scoped to the tested environment. |
| Ongoing maintenance | Re-run bridge and desktop checks after relevant GTK, shared semantics, input, window-lifecycle, or PlatformView changes. |

A headless Linux VM, container, or WSL shell can support compilation and some bus-level tests; this alone does not establish compatibility with KDE Wayland on the target desktop.
An interactive Linux VM can validate its actual desktop session, while hardware-dependent input, scaling, monitor, and graphics behavior still needs suitable target coverage.
The initial target is the owner's KDE Wayland desktop, with an X11 or Xwayland comparison; broader desktop support requires its own evidence.

## Implementation boundaries

- Keep the public `Semantics`, `SemanticBuilder`, `SemanticFrame`, and `SemanticAction` model authoritative rather than adding Linux-specific component APIs.
- Place the bridge in private Linux implementation files, owned by the existing platform-derived UiWindow, and attach its hierarchy beneath the real GTK host.
- Commit PlatformViews before publishing accessibility for the same frame; use GTK's application thread and main context rather than creating another UI thread or event loop.
- Retain immutable frames and cache platform objects by SemanticNodeId; never retain MountedNode or NodeExtension pointers.
- Preserve compatible identity, invalidate removed objects safely, and account for GTK's immutable accessible roles when replacing platform objects.
- Route advertised actions through `UiWindow::PerformSemanticAction()` so controlled state, disabled checks, stale-target rejection, and existing typed events remain intact.
- Keep screen-reader focus separate from UiWindow input focus and IME ownership; preserve secure-field redaction without reconstructing protected text or length.
- Publish only meaningful changes, reuse unchanged semantic revisions, and avoid forcing realization of an entire virtual collection.
- Bridge native PlatformView descendants once at the existing semantic anchor; do not publish a duplicate HuxerUI subtree.
- Respect explicit accessibility environment settings and support accessibility activation during the application's lifetime; do not simply replace the current default with another unconditional setting.

## GTK compatibility and bridge choice

Preserve the existing GTK 4.14 minimum unless a separate compatibility decision approves a change.
Prefer public GTK accessibility interfaces so GTK can own AT-SPI transport and native integration, but verify their suitability with an executable prototype before committing to this route.
Do not depend on GTK private APIs or replace each rendered control with a native GTK control to obtain accessibility implicitly.

The prototype must resolve these constraints:

- [GtkAccessible](https://docs.gtk.org/gtk4/iface.Accessible.html) supports custom accessible hierarchies and bounds, and accessible roles cannot change after object creation.
- GTK 4.14.5's [AT-SPI Action implementation](https://github.com/GNOME/gtk/blob/4.14.5/gtk/a11y/gtkatspiaction.c) selects action handlers for GtkWidget implementations, not arbitrary non-widget GtkAccessible objects; readable semantic objects alone do not prove actionable controls.
- [GtkAccessibleText](https://docs.gtk.org/gtk4/iface.AccessibleText.html) starts at 4.14, text-range geometry callbacks start at 4.16, and selection-writing callbacks start at 4.22; use only capabilities supported by the declared build and runtime boundaries.
- HuxerUI selection ranges use UTF-16 units, while GTK accessible-text offsets use characters and content is UTF-8; test conversion with Chinese, supplementary characters, and combining sequences.
- The shared snapshot supplies whole-node bounds, not complete text-range geometry; do not fabricate character rectangles or silently add a parallel text-layout path.
- Verify parent-, window-, and screen-relative bounds and point queries on the actual GTK and compositor combination; unavailable global coordinates must remain explicit rather than being fabricated.

If public GTK interfaces cannot express the agreed scope, document the gap and compare a direct AT-SPI implementation through existing GIO facilities against a narrower initial feature set.
A direct transport adds ownership of bus registration, application and object identities, interfaces, events, retained references, shutdown, and native subtree embedding; it is a separate maintenance decision, not an assumed shortcut.

## Phased work and acceptance

- [ ] **TODO(farna) FA-01: Establish a reproducible Linux validation environment.** Record the desktop, compositor, session type, GTK version, backend, scale, monitor arrangement, accessibility bus, inspector, and Orca availability; identify missing dependencies before changing them and preserve existing build directories and toolchains.
- [ ] **TODO(farna) FA-02: Prove the bridge path.** Use a small fixture containing a Button, Checkbox, Slider, and TextField to demonstrate discovery, names, roles, bounds, focus queries, activation, and value changes through an external AT-SPI client; explicitly resolve non-widget actions, the GTK 4.14 boundary, and frame ownership before expanding coverage.
- [ ] **TODO(farna) FA-03: Publish the semantic hierarchy.** Cover committed properties and states, parent and sibling order, keyed movement, role replacement, clipping and transforms, offscreen nodes, removal, and window retirement; verify safe behavior for retained references after their targets disappear.
- [ ] **TODO(farna) FA-04: Connect actions and notifications.** Qualify activation, selection, expansion, numeric values, scrolling, ShowOnScreen, focus, dismissal, custom actions, and property/structure/focus/live-region events; reject disabled, stale, unsupported, and invalid requests through existing shared behavior.
- [ ] **TODO(farna) FA-05: Qualify accessible text.** Define and test the supported reading, caret, selection, and editing subset for each supported GTK boundary, including controlled writeback, active IME synchronization, read-only fields, secure fields, Unicode offsets, and content/selection notifications.
- [ ] **TODO(farna) FA-06: Qualify complex content.** Exercise modal and exiting layers, VirtualList, VirtualGrid, TreeView, native PlatformView substitution, dynamic accessibility activation, multiple windows, and repeated attachment and retirement without duplicate nodes or retained-object leaks.
- [ ] **TODO(farna) FA-07: Validate native desktop usability.** Run Orca and inspector scenarios in KDE Wayland and the X11/Xwayland comparison, including fractional scale, multiple monitors, geometry after movement, and keyboard versus screen-reader focus; add other desktop environments only with explicit validation results.
- [ ] **TODO(farna) FA-08: Integrate and document the delivered subset.** Update Linux CMake sources and any required source/link declarations in `mcpp.toml`, run build-parity checks, add focused Linux tests, and update the owning design and public platform documentation with actual support and remaining limitations.

Initial discovery or action support is an incremental milestone, not a claim of complete Linux accessibility.
Use existing tests under [tests/runtime](../../tests/runtime), [tests/platform/windows](../../tests/platform/windows), and [tests/platform/android](../../tests/platform/android) as behavioral references, and place Linux integration fixtures under [tests/platform/linux](../../tests/platform/linux).
Validate the framework, standalone headers, affected example, common suites, Linux bridge tests, and both build systems where affected; report unavailable GTK versions, desktop sessions, and assistive tools explicitly.
Keep implementation evidence with its patch or review rather than recording an accumulating historical test log here.

## Screenshot compatibility is a separate check

WeChat screenshot auto-selection motivated this investigation, but it is not the acceptance contract for accessibility.
The user observed better automatic selection when the same application used X11/Xwayland instead of native Wayland under KDE; this does not prove that WeChat consumes AT-SPI or identify its internal detection algorithm.
After the bridge works with independent accessibility tools, test WeChat separately and record its version, package, session, application backend, scaling, and whether selection targets a whole window or an internal control.
Do not promise that an AT-SPI bridge will resolve native Wayland screenshot selection, and do not change the framework's default display backend to satisfy one screenshot client.
