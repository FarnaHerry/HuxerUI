# HuxerUI Documentation

## User guide

- Use [Installation](guide/installation.md) to install an SDK and switch versions.
- Use [Getting Started](guide/getting-started.md) to create a project, prepare its toolchain, and run it.
- Check [Platform Support](guide/platforms.md) for host requirements, backend capabilities, and native window placement.
- Use [Packaging Applications](guide/packaging.md) to choose target ABIs, build distributable artifacts, and configure signing and runtime dependencies.
- [Core Concepts](guide/core-concepts.md): Views, composition, state, identity, events, and Environment.
- [Layout and Scrolling](guide/layout.md): constraints, responsive layout, scrolling, and virtualization.
- [Components and Input](guide/components.md): controls, navigation, gestures, and text editing.
- [Themes and Presentation](guide/themes-and-presentation.md): themes, indication, animation, and layers.
- [Frosted and Liquid Glass](guide/glass.md): backdrop blur, optical materials, themed controls, fusion, and capture boundaries.
- [Resources and Images](guide/resources.md): packaged assets, localized strings, and image editing.
- [Files and Storage](guide/files.md): application storage, external files, and pickers.
- [HTTP Client](guide/http.md): buffered and streaming responses, transfer progress, Tasks, and errors.
- [System Tasks](guide/system-tasks.md) explains task selection, control, UI observation, and platform setup.
- [Extending HuxerUI](guide/extending.md): custom components, layouts, modifiers, and platform services.
- [Windowless UI Testing](guide/testing.md): real-Runtime queries, input, virtual frames, and structural captures.
- [C++20/23 modules and mcpp](../skills/huxerui-app-development/references/cpp-modules-and-mcpp.md): `import huxerui;` and `import std;`, module units, applications with no headers, and building with mcpp instead of CMake.

## Framework development

- Use [Building HuxerUI](development/building.md) to configure the source tree, run tests, and build examples.
- [Windows Paint Benchmark](development/windows-paint-benchmark.md): reproduce native renderer A/B timing, pixel checks, and memory measurements.
- [Downstream integration](development/farna-integration.md): the `farna/main` patch inventory, validation, and adoption.
- [Upstream Coverage Registry](development/upstream-coverage.md): fork items already covered by upstream, partial overlaps, fork-only re-audit status, and the merge procedure that keeps the registry current.
- [Developing Multi-Window](development/multi-window.md): the multi-window extension status, validation, behavioral notes, and desktop porting checklist.
- [Desktop Pet Example](../examples/desktop_pet/README.md): a Windows assistant orb with retained wave animation, transparent utility presentation, passive display, native input passthrough, and tray recovery.
- [Farna graphics TODO](development/farna-graphics-todo.md): downstream-only future work for compositing, glass primitives, and backend qualification; not an upstream roadmap.
- [Farna floating-window and desktop-assistant TODO](development/farna-desktop-assistant-todo.md): delivered Windows baseline, validation limits, remaining foundations, and optional native services; not an upstream roadmap.
- [Farna Linux accessibility TODO](development/farna-linux-accessibility-todo.md): downstream plans to bridge existing semantics to Linux accessibility, with GTK compatibility decisions and interactive Linux validation gates.
- Use [SDK Packaging](development/sdk-packaging.md) to assemble, validate, and publish SDK archives and maintain host tools.
- Find the private vulnerability reporting channel in the [Security Policy](../SECURITY.md).

## Design reference

[Architecture](design/architecture.md) describes the application, window, and host boundaries. [Layout and Virtualization](design/layout.md) defines measurement, placement, and virtual item lifetime. [Design Documents](design/README.md) indexes the owning contracts and drafts. The [Roadmap](roadmap.md) records future capability areas.

Downstream design contracts also cover [Backdrop Effects and Glass](design/backdrop-effects.md), [Multi-Window](design/multi-window.md), and [Window Relationship](design/window-relationship.md).
