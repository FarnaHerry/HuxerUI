# Window Relationship Design

This document defines window-level ownership and modality on top of the [Multi-Window](multi-window.md) creation contract: which window owns another, which window blocks input, and what happens when an owner retires.
It does not change the Layer or dialog model; presentation inside one window stays on the existing focus-trap and semantic-modal machinery.

## Goals

- Let an application declare that one window is owned by another: the owned window stays ordered above its owner, follows owner minimization, and retires when the owner retires.
- Let an application declare a window modal: while the modal window is alive, user input to the windows it blocks is rejected by the platform, without a nested event loop.
- Support application-modal windows: a modal window with no owner blocks every other window of the application.
- Allow relationships to change at runtime, including reparenting and promoting or demoting modality.

## Non-goals

- Cross-process ownership or modality.
- System-modal windows that block other applications.
- Nested event loops or a second UI thread; the one application thread keeps dispatching every window.
- Window placement, geometry, or z-order APIs beyond what ownership implies.
- Docking, tabbing, or merging windows.
- Document-modal or task-modal variants with different input scopes.

## Relationship model

One value describes both aspects:

```cpp
struct WindowRelationship {
  /// The window that owns the target. Empty selects no owner; a modal window with no owner is application-modal.
  std::optional<WindowHandle> owner;
  /// Whether the window blocks user input to the windows its relationship covers.
  bool modal = false;
};
```

An owned window that is not modal is a transient helper such as an inspector or tool window: it stays above its owner, minimizes with the owner, and closes when the owner closes, but it never blocks input.
A modal window blocks user input to its owner's whole owned subtree except windows the modal itself owns; with no owner, it blocks every other window of the application.
An owned modal window, such as a settings dialog, combines both.

`Runtime::OpenWindow` accepts an optional relationship, and `SetWindowRelationship(window, relationship)` changes it later.
Validation runs before any native change: a window cannot own itself, no owner chain may contain the target, and an owner that has already retired is rejected.
`std::invalid_argument` reports caller input; a retired owner reports through the owning service contract.

## Ownership registry and cascade

`Runtime` owns one relationship registry beside the window table: an owned-edge map from owner to owned windows and a modal-edge set.
The registry updates only from `OpenWindow` and `SetRelationship` on the application thread.

Retiring a window retires its owned windows first, deepest chain first, before the window itself finishes retirement.
The platform adapter calls `Runtime::RetireOwned` before destroying a window through a user close, so a transient inspector or owned dialog disappears with its owner; application shutdown retires every window through the existing path and needs no separate cascade.
Reparenting an alive window moves its registry edges and restores or applies modality bookkeeping as needed; retiring a window drops it from every registry edge that names it.

## Modality without a nested loop

Modality is a property of the window set, not of the event loop.
While a modal window is alive, the platform disables native input delivery to every blocked window, and the one application thread keeps dispatching frames, timers, and tasks for all windows.
Modal windows may therefore open further windows, run animations, and process I/O exactly like ordinary windows.

When the last modal window over a blocked window retires, the platform re-enables that window and returns activation to the owner if the owner is still alive and visible.
Nested modality follows the edge set: closing an inner modal restores its own scope, and closing an outer modal restores what remains.

## Platform mapping

- Windows marks the initial window with the ownership property, stores the owner through `GWLP_HWNDPARENT`, and applies modality by enabling or disabling the affected HWNDs; disabling the owner also disables its owned chain, matching the blocked-subtree rule. Focus returns to the owner on modal retirement.
- Linux sets `GtkWindow` transient-for ownership and uses GTK modal grabbing, which already blocks the transient subtree without a nested main loop.
- The macOS preview orders owned windows through `addChildWindow:ordered:` and removes the old parent when reparenting. Runtime computes the exact blocked set, including sibling and application-modal stacks; blocked NSWindows reject event delivery, mouse events, key status, and main status, covering embedded native views as well as shared controls. Pending pointer cancellation waits until in-flight pointer dispatch returns, and text input pauses while blocked. Native close queues retirement after the AppKit callback returns; owner cascade releases owned native windows first without a nested run loop.
- Web, Android, and iOS do not support runtime window creation, so relationships stay unused there; their platform-driven presentations keep their native controller semantics.

## Interaction with the rest of the framework

- The initial window keeps its historical shutdown behavior; cascade retirement never changes application lifetime by itself.
- `WindowHandle` commands keep working on blocked windows; modality blocks user input, not programmatic control.
- Layers and dialogs inside one window are unchanged; a window-level modal and an in-window dialog may coexist, and the window-level rule wins for input crossing windows.
- Accessibility sees disabled windows through the platform state; no semantic role is added.
