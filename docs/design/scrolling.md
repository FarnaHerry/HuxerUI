# Scrolling

This document defines mounted offset ownership, input consumption, and motion. See [Layout and Scrolling](../guide/layout.md#scrollview) for application usage and [Layout and Virtualization](layout.md#virtual-geometry-and-offset-corrections) for realization and offset commit.

## Ownership

A mounted scroll container owns one clamped content offset per enabled axis. Two axes share the viewport and clip, with independent controller connections, momentum, and overscroll. `ScrollMetrics` observes offset and extents. A disabled container still accepts requests from its connected controller and rejects user scrolling and focus reveal.

Ordinary `ScrollView` measures its child with an unbounded maximum on enabled axes. `FillViewport()` also raises each of those minima to the bounded viewport extent, or preserves the incoming minimum on an unbounded axis. The Tabs strip shares this measurement policy. Ordinary offset changes update the retained children transform. Virtual offsets instead change the realized range and placements. Scrollbars retain only their own interaction, visibility, and paint state and derive thumb geometry from the owner's metrics.

## Platform scroll input

Adapters normalize wheel and trackpad data into `ScrollInputEvent` with a window position, two signed content-offset deltas in DIPs, `ScrollInputKind` (`Wheel` or `Touchpad`), and `ScrollInputPhase` (`Begin`, `Update`, `End`, or `Cancel`). `Wheel` retains independent-update behavior, while `Touchpad` reports gesture phases where only `Update` carries displacement. `UiWindow::HandleScrollInput()` returns the consumed components; `End` and `Cancel` return zero after notifying mounted consumers, and the host can use an unconsumed wheel update for its own default scrolling.

Dispatch uses the committed hit route. A hit on a PlatformView leaves the update to the host. Otherwise the deepest enabled node with `ViewEvents::ScrollInput` receives the complete update once. A `true` result consumes both axes for that update. A `false` result invokes the default scroll transaction without contacting another raw handler. Direct dragging uses pointer recognition.

For continuous `Touchpad` gestures, Runtime retains the hit route, delivers `Begin`/`Update`/`End` activity to each mounted scroll container that consumes an axis, and derives release velocity from recent consumed offsets. Both axes remain independent so vertical updates cannot start horizontal paging, and `Cancel` never submits a page proposal.

GTK uses continuous scroll begin/end signals and preserves surface-pixel deltas while wheel-unit deltas keep the 40-DIP step. AppKit uses the gesture phase for direct input and leaves the separate momentum phase as ordinary scroll input so momentum cannot request another Pager page. Win32 wheel messages lack reliable device identity and an end phase, so sub-notch deltas start a gesture that ends after 120 milliseconds idle while ordinary notch messages outside that gesture retain wheel behavior.

## Nested scroll transaction

Each nonzero axis selects the eligible scroll containers along the route. On direct touch, reversing direction can consume existing overscroll first. Runtime then offers the remaining signed delta in this order.

- Pre-scroll callbacks run from outer to inner, in modifier declaration order.
- Clamped offsets consume from inner to outer.
- Post-scroll callbacks run from inner to outer, in reverse modifier order. They receive the amount already consumed and what remains.
- On a direct-touch update, the outermost eligible container can accept the remaining delta as overscroll for that direction.

Callbacks on retained extensions of participating scroll containers return the actual amount consumed. It must be finite, point in the available direction, and stay within the available magnitude. An invalid return is a framework invariant failure. A two-axis transaction can reach a boundary on one axis while the other continues consuming.

## Direct drag and fling

The [pointer session](gestures.md) arbitrates direct scrolling after movement crosses slop. A single-axis recognizer competes in the dominant direction. A two-axis container can accept on either axis and dispatch both components along its committed route. Touch may accept at a boundary when overscroll is enabled for that direction. Mouse and pen need scroll capacity.

After a fling-eligible release, pre-fling callbacks offer velocity from outer to inner before starting momentum. The momentum owner integrates its velocity through the same clamped offset path. At a boundary it offers the remaining velocity to its post-fling callbacks before the nearest compatible ancestor can continue it. Velocity consumption follows the same direction and magnitude rules as delta consumption and is independent on each axis.

`ScrollPhysics` supplies thresholds, decay, resistance, and settlement. A modifier overrides the platform's default for that container. Theme resolves scroll bar visuals. Reduced motion stops momentum and clears settling overscroll on the next frame.

## Overscroll and activity

Overscroll is a separate presentation translation for clipped content, including PlatformViews. It preserves layout, realization range, clamped metrics, and semantic scroll position. Leading and trailing eligibility are tracked separately. Release and cancellation settle the displacement. Disablement, lost layout participation, or unmount clears active motion.

`ScrollActivity` notifies mounted extensions of the axis, source, phase, delta, and current metrics. Settlement reports `ScrollSource::Overscroll` so geometry-dependent observers follow the visual motion. At a terminal direct notification, a retained behavior can take over the displacement and apply an equivalent descendant transform in the same commit. [RefreshBox](refresh-box.md#scroll-ownership-and-handoff) uses that handoff.

## Programmatic and semantic scrolling

Controller updates, focus and text-input reveal, scrollbar dragging, drag-and-drop auto-scroll, and semantic actions use the clamped offset mutation path. A direct offset request targets one container. Reveal can adjust several ancestors independently using the target's current geometry. In a two-axis container it uses the nearest offset exposing as much as fits on each axis. A target already covering the viewport leaves that axis in place. Drag auto-scroll selects compatible ancestors using their own edge geometry.

A controller can queue an initial request before its first connection. A detached controller never replays requests from its previous connection. Single-axis virtual item jumps can refine an estimated target until the measured placement agrees with it. A newer offset request or intervening scroll cancels pending alignment. A two-axis policy supplies `ItemBounds()` in content coordinates. Each controller aligns the same rectangle along its bound axis, and layout settles the requests together with the viewport.

## Validation

[Scrolling tests](../../tests/runtime/scrolling.cpp) cover raw-input consumption, nested callback order, independent axes, overscroll settlement, controller requests, disabled interaction, and focus reveal.
