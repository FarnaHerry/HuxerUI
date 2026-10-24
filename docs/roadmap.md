# Roadmap

This roadmap records intended capability areas, not committed release dates.
Current behavior is documented in the user guides, while detailed constraints live in the design reference.

## Accessibility

- Expand platform text-editing and collection mappings where the shared semantic contract already provides the required data.
- Keep accessibility behavior derived from one shared `SemanticFrame` rather than platform-specific component logic.

## Platform integration

- Add Linux `PlatformView` hosting while preserving shared composition ordering.
- Complete iOS archive export, distribution signing, and embeddable host integration.
- Add an OHOS backend through the existing platform boundaries.

## Desktop windows

- Extend the existing window contract with per-pixel transparency, utility-window presentation, task-switcher visibility, activation policy, and runtime stacking control through native platform operations.
- Add runtime window geometry control and display work-area queries for floating UI, with explicit capability reporting where the host controls placement or stacking.
- Add native pointer-input regions and whole-window input passthrough independently of visual alpha, preserving shared pointer cancellation and capture behavior when the input policy changes.
- Reuse ExternalTexture and retained frame scheduling for decoded animation frames, keeping character assets, animation selection, and application integration in the consuming application.

The [Farna floating-window and desktop-assistant backlog](development/farna-desktop-assistant-todo.md) records downstream investigation, optional native-service candidates, and validation gates; it does not commit upstream API or implementation scope.

## Interaction and animation

- Add ordinary View lifecycle transitions without introducing per-component Runtime branches.
- Expand paint primitives only when a production component or application requires them.

## Navigation and restoration

- Define saveable application and navigation state without making factory-only routes serializable by implication.
- Add typed navigation results and richer adaptive navigation only when they preserve the existing single path of truth.
- Extend platform activation and deep-link integration while keeping URL and route policy application-owned.

## Resources and distribution

- Add localized image selection, inherited locale-aware text shaping, and explicit resource preload policy.
- Publish signed platform packages where the platform ecosystem requires them.
- Extend SDK packaging and signing without duplicating build policy in generated projects.

Implementation proposals must update the relevant design document before changing public API or ownership boundaries.
