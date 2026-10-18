# Farna TODO: Graphics and Glass Primitives

Owner: Farna.
Status: downstream future work; no implementation is authorized by this checklist.

This backlog belongs to the Farna fork and its `farna/main` integration policy.
It is not an upstream roadmap, an SDK support declaration, or a commitment to new public APIs or release dates.
Keep this document and its index entry out of upstream contribution patches unless upstream explicitly requests them.
Implementation proposals should start from an agreed downstream scope; independently useful upstream contributions remain separate patches based on `upstream/main`.
See [Downstream integration](farna-integration.md) for branch ownership and contribution boundaries.

## Existing contract and scope

The current [glass guide](../guide/glass.md) and [backdrop design](../design/backdrop-effects.md) own implemented behavior.
The existing public surface includes `BackdropEffect`, `BackdropShape`, `PaintContext::DrawBackdrop`, `BackdropBlur`, `BackdropFilter`, `LiquidGlass`, and `GlassGroup`.
This backlog records qualification and extension work; an unchecked qualification task does not mean its underlying mechanism is absent.
It does not change those contracts, the default Precise quality, platform minimum versions, or any renderer selection policy.

Prefer strengthening existing PaintCommands, RenderScene ownership, NodeExtension animation, and renderer resources before introducing another abstraction.
Expose a new public primitive only when a concrete application needs to compose it independently and its semantics, resource limits, and fallback behavior can be specified across backends.
Names in the tasks below describe capabilities rather than approved C++ API declarations.

## Foundation and backend qualification

These tasks take precedence over adding more material parameters.

Use the [native rendering architecture baseline](../design/backdrop-effects.md#native-rendering-architecture-baseline) as an acceptance gate for backend optimization: preserve platform drawing and compositing ownership, prefer compatible native operations, and account for the complete frame's transfers and synchronization.
The Linux audit in that section distinguishes resident optical processing from remaining Cairo uploads and external-texture downloads.

- [ ] **TODO(farna) FG-01: Qualify compositing semantics.** Audit drawing order, transparent isolation groups, premultiplied alpha, ordinary source-over composition, and coverage-controlled replacement across available renderers; accept with partial-alpha, nested-group, earlier-sibling, and sharp-foreground pixel fixtures without applying alpha twice.
- [ ] **TODO(farna) FG-02: Make the color contract explicit.** Verify texture imports, intermediate targets, filtering, and publication preserve the declared premultiplied sRGB contract; specify conversion and rounding boundaries and add transparent-color and gradient fixtures before considering another working color space.
- [ ] **TODO(farna) FG-03: Qualify offscreen resource ownership.** Audit renderer-owned targets, immutable published frames, synchronization, pooling, cancellation, resize, device/context loss, and retirement; accept with bounded allocation and retained-frame tests before deciding whether applications need a public offscreen-target API.
- [ ] **TODO(farna) FG-04: Qualify backdrop source dependencies.** Test the complete already-painted prefix within the active isolation group, including earlier siblings and earlier optical output; invalidate capture and kernel reuse on every relevant source, transform, clip, scale, viewport, and texture-revision change without sampling a previous finished frame.
- [ ] **TODO(farna) FG-05: Qualify geometry and coverage.** Extend cross-backend fixtures for asymmetric rounded rectangles, distance-field unions, fractional scale, transformed and path clips, empty coverage, and antialiased edges; retain the complete blur/refraction sampling halo and unchanged exterior pixels when reducing visible work.
- [ ] **TODO(farna) FG-06: Qualify animation and paint invalidation.** Exercise geometry changes, press/hover feedback, parameter animation, keyed movement, disabling, cancellation, unmount, and reduced motion through existing MotionController and NodeExtension mechanisms; verify background dependencies and conservative damage propagation, and ensure resting materials stop requesting frames.
- [ ] **TODO(farna) FG-07: Establish resource and frame-cost gates.** Account separately for live targets, masks, caches, exports, and producer storage; measure replay, transfer, filtering, synchronization, and completed-window presentation with fixed/changing-source controls and reversed A/B order, preserving missing feedback as unavailable and excluding contaminated runs from acceptance.
- [ ] **TODO(farna) FG-08: Improve backend execution coverage.** Evaluate GPU optical processing for Windows, Apple platforms, and Web while retaining the shared effect contract; qualify Linux and Android capability admission, native compositor/texture interoperability, and CPU fallback with equivalent focused fixtures on available target devices.

GPU work should reuse an existing executor or native operation when it owns the required behavior.
Linux work should remain within GTK/GDK ownership, Android work should retain scene-wide quality admission and matching Java/native sources, and other backends should preserve their native lifecycle and device-loss rules.
A supported fallback must preserve the declared quality contract or an explicitly selected approximation; budget exclusion must not silently change material quality.

## Focused additions with concrete consumers

Implement each addition independently after identifying an application example that exercises its composition and fallback behavior.

- [ ] **TODO(farna) FG-09: Evaluate a color-matrix primitive.** Use a real image-adjustment, grayscale, disabled-state, or transition example to define channel order, alpha handling, color space, filter ordering, clamping, and identity behavior; require a CPU reference and affected-renderer comparisons before public API approval.
- [ ] **TODO(farna) FG-10: Evaluate a general mask primitive.** Start from a fade, cutout, or texture-mask consumer; define alpha versus luminance interpretation, mapping, sampling, nested isolation, immutable-source identity, and invalidation rather than exposing a backend's temporary clip-mask storage directly.
- [ ] **TODO(farna) FG-11: Evaluate a bounded set of blend modes.** Select only modes required by real drawing or mask examples, specify premultiplied equations and group scope, and verify transparent inputs, nested composition, device fallback, and interactions with backdrop capture.
- [ ] **TODO(farna) FG-12: Evaluate deterministic grain/noise.** Use a glass or gradient example to define spatial coordinates, scale, seed, amplitude, alpha treatment, filter placement, and temporal behavior; require reproducible captures and no continuing frame scheduling for static noise.

Existing saturation and brightness parameters remain valid without a general color matrix.
General masks and blend modes should extend the narrowest existing drawing contract rather than introduce a parallel scene, property system, or layer host.

## Deferred capabilities

These remain recorded Farna TODOs and are not prerequisites for the existing UI glass material.
Promote a task only when a concrete consumer and a bounded cross-platform contract justify its implementation and validation cost.

- [ ] **TODO(farna) FG-13: Explore external displacement and normal maps.** Specify coordinate spaces, texture lifetime and revision, sample reach, edge behavior, normal decoding, and CPU/low-capability execution for an identified custom-material consumer.
- [ ] **TODO(farna) FG-14: Explore environment reflection and Fresnel terms.** Define the environment source, view convention, material parameters, compositing, color space, and resource limits; keep directional edge lighting distinct from actual environment reflection.
- [ ] **TODO(farna) FG-15: Explore arbitrary distance-field geometry and shape morphing.** Establish bounded geometry representation, stable shape identity, antialiasing, conservative sampling bounds, and deterministic animation before exposing programmable SDFs or unrestricted shape interpolation.
- [ ] **TODO(farna) FG-16: Reassess a general filter graph or programmable shader API.** Require multiple implemented filters and real consumers that need ordering, branching, or intermediate-result reuse; define capability admission, intermediate formats, budgets, cache invalidation, failure semantics, and portable execution before introducing a graph abstraction.
- [ ] **TODO(farna) FG-17: Explore native window/desktop backdrop capabilities separately.** Assess platform compositor support, permissions, unavailable-content behavior, synchronization, and lifetime; do not change application-internal DrawBackdrop sampling to imply access to another native window or the desktop.
- [ ] **TODO(farna) FG-18: Explore opt-in adaptive quality and broader color workflows.** Treat frame-budget adaptation and linear-light, wide-gamut, or HDR processing as separate contracts with explicit application policy, measurable transitions, and matching fallback semantics; do not silently modify existing Precise or sRGB behavior.
- [ ] **TODO(farna) FG-19: Qualify motion-driven Precise/Fast switching.** Keep switching opt-in, use hysteresis and a stable return to Precise when motion settles, and verify visual continuity, filter-resource reuse and completed-window frame cost before introducing automatic material selection.

## Completion and upstream boundary

A task is complete only when its agreed scope has implementation evidence, focused correctness checks, appropriate available-platform validation, documentation, and explicit remaining limitations.
Keep existing Precise and Fast pixel gates distinct, account for platform rasterization differences through an appropriate reference, and report unexecuted platforms as unavailable validation.
Preserve source ordering, immutable publication, bounded resource ownership, and cancellation/retirement behavior through every optimization.
Public drawing changes must audit every renderer; public declarations must remain consistent between headers and the generated module shell, and framework source/link changes must respect both CMake and mcpp.

Record implementation evidence in the owning patch or review rather than turning this future-work document into a historical test log.
Any accepted SDK behavior belongs in the owning guide and design document; this Farna checklist must not become the authoritative public contract.
Upstream review and adoption are separate decisions, and this checklist alone changes no upstream roadmap, API, build configuration, or runtime behavior.
