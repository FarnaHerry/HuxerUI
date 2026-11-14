# Downstream integration

`FarnaHerry/HuxerUI` uses `farna/main` as the downstream integration branch. The initial integration was taken from
acgu's framework checkout on 2026-10-06, without switching or modifying that application's working tree.
The dedicated local maintenance clone is `/home/farna/dev/cpp/mcpp/HuxerUI-fork`.
Repository-wide maintenance rules live in [AGENTS.md](../../AGENTS.md).
Before treating a fork commit as an upstream contribution, check the [Upstream Coverage Registry](upstream-coverage.md); it records behavior already present in `upstream/main` and items confirmed to remain fork-only.

## Upstream anchor direction

Upstream `1b227315` unified `FocusTarget` and presentation anchors through `ViewHandle`, so anchor lifetime is owned by that handle instead of per-node mount identity.
The fork's `bd8fb056` layer-anchor transfer fix was removed during merge `c2f099c1`; it should not be proposed separately because PR [#163](https://github.com/HuxerUI/HuxerUI/pull/163) was closed unmerged.
`TestAnchoredPresentationFollowsViewHandleAcrossReplacement` covers popup replacement against the unified handle.

## Closed upstream pull requests

The maintainer closed eight fork PRs; almost every one landed as an independent implementation, usually without a review comment.
The fork tracks those upstream commits, so the corresponding preparation branches are retired.

| PR | Disposition | Upstream evidence |
|---|---|---|
| #170 | Partial: custom-chrome corner rounding and PlatformView input region adopted; icon resource not adopted; activation target already existed | `bdd0987b`, `38eb34dc` |
| #164 | Fixed through the profiling default and the mcpp source exclusion | `9b8f5d05`, `3abaa77b` |
| #163 | Superseded by the unified mounted view target | `1b227315` |
| #154 | Superseded by the public `TextLayout` API | `1949e84e` |
| #153 | Closed without an equivalent implementation; the fork removed `TextField::TextStyles` | none |
| #151 | Fixed with the `CreateUiWindow` name instead of the proposed `CreateApplicationWindow` | `e613b515` |
| #150 | Pager, drag-preview, and virtual-subtree fixes landed before closing; the remaining items were already covered | `784f99a6`, `711a9eae`, `9f90e005` |
| #135 | Replaced by per-window system appearance | `5523172a` |

PRs #149 and #148 were closed by the fork after #150 absorbed them; #141 was later implemented upstream as `e0f49ceb`.
PRs #139, #140, #138, and #118 were closed by the fork without an upstream equivalent.

## Upstreamed fork pull requests

| PR | Upstream commit | Result |
|---|---|---|
| #174 | `d407f566` | Disabled containers accept programmatic controller scrolls, including `ScrollToItem`, while user-driven sources and focus reveal stay blocked. |
| #175 | `df5f4ba0` | `ScrollView::FillViewport()` exposes the existing fill-viewport measurement policy; the public header and guide document bounded and unbounded axes. |
| #182 | `acfb154b` | Disabling direct drag during an active Pager gesture settles on the controlled page instead of the outgoing displayed page, for both `Dragging` and `AwaitingCommit`. |

The coverage registry records all three as covered. The #174 and #175 commits are merged into `farna/main`; the merge commit is `d48d36d0`. PR #182 (`acfb154b`) is not merged yet: the next upstream merge drops the fork's duplicate `dbc28f3c` change.

## Upstreamed installer work

Upstream `adea3ba8` (authored by FarnaHerry) supersedes the fork's installer upgrade and unattended-run work.
It is a single-parent squash commit, and none of the fork commits (`929ebde9`, `8c2eae59`, `884b8a59`, `c0949cc5`, `9333c98b`, `abd07a3f`, and the installer hunk of `42f0069d`) are ancestors, so this is an upstream rewrite rather than a merged branch.
The rewrite inlines the command policy and hidden owner window, tightens preemption to headless, passive, and failed-upgrade embedded runs, keeps full-display `/uninstall` and `/repair` behind confirmation, and preserves restart exit codes; `b2ae8e8f` adds the restart prompt.
Merge `8eb3adfc` therefore removed `installer_command_policy.h`, `installer_native_window.h`, their platform test, the SDK header installs, and the test-only WiX wiring.
The fork keeps one residual policy: engine error prompts never offer `Ignore` (`3517ea29`).

## Initial inventory

The upstream baseline is `0c5126235d43c2b703166bcc00781b850f2d1c39`. The starting acgu HEAD is
`dce2a08bcecd9d9da254891944979aa21cbab02e`, which retains these five downstream commits:

| Commit | Behavior |
| --- | --- |
| `f38d5ec` | Linux custom-chrome resize targets and directional cursors |
| `16323d5` | Pull refresh can keep content in place |
| `0b6400c` | Cancel pull translation when content stays in place |
| `794c038` | Reuse unchanged paragraph measurements |
| `dce2a08` | Skip redundant loose measurement for grow children |

`329d75e` preserves acgu's 25 modified and three new framework files as one snapshot commit. These cover native
desktop fullscreen, the GTK4 PlatformView host, immutable shared HTTP request bodies, windowless lifecycle
control, and Pager distance/velocity thresholds. It is a preservation commit, not a claim that each file is an
independent fix. The follow-up integration adds focused public-header, HTTP ownership and lifecycle checks,
and makes the Linux native test target link libepoxy explicitly for shared-only builds.

Pager release thresholds remain the only deliberate interaction tuning. The upstream author plans to expose configuration for them; the [Upstream Coverage Registry](upstream-coverage.md) records the current fork defaults and the replacement point.

Android's whole-host default focus highlight is disabled in `HuxerUIView` on API 26 and later. Keyboard and touch-mode
focus remain enabled. This moves the split-screen brightness correction into the framework for all consumers.
The application-side workaround can be removed after a consumer adopts and verifies this revision.

Clash-Flux's five framework patches are integrated after review against the same official baseline:

- Drag previews follow the original grab point without popover flipping or viewport clamping.
- Pager retargeting preserves the departing peer when a non-adjacent transition returns to its source and clears stale drag targets.
- Hidden virtual pages retain pending measurement without invalidating visible ancestors on every frame.
- Linux window Show and Activate request a frame, Hide releases pending paint, and frame callbacks avoid destroyed drawing widgets.
- WindowTitleBar uses brace initialization for Objective-C++ compiler compatibility.

The Linux changes retain the integration's PlatformView commit and exception handling rather than replacing the adapter with the older application snapshot.
Historical feature branches have not been merged wholesale. Lib-MediaPlayer, Lib-WebView, Lib-Camera, Lib-Charts and Lib-SQLite remain separate repositories with their own revisions and application patches.

apitab's three framework patches were audited against this integration: the WindowTitleBar brace initialization and the Linux resize hit areas were already covered.
The remaining host-tools patch is carried here as the `HUXERUI_HOST_TOOLS_STATIC_RUNTIME` option in both standalone tool projects; it defaults to ON and preserves upstream's static libstdc++/libgcc linking, and is set to OFF on development hosts without a static libstdc++.
apitab's SweetEditor and SweetLine patches belong to those separate repositories and are not part of this fork.
They are integrated in the `FarnaHerry/Lib-CodeEditor` fork instead (upstream renamed `huxerui-sweetedit` to `Lib-CodeEditor`), whose `farna/main` carries the CodeEditor component changes and the two vendored-core patches under `3dparty/patches/`; the local maintenance clone is `/home/farna/dev/cpp/mcpp/HuxerUI-libs/Lib-CodeEditor`.

## Validation

The Clash-Flux integration passes the incremental Linux Debug runtime and standalone-header builds, the Linux Release framework build, and all four common CTest suites, including 953 runtime cases.
Focused regressions cover non-adjacent Pager reversal without a geometry jump, preview grab offsets beyond viewport edges, and hidden virtual-page measurement resuming on selection.

Linux Debug and Release compilation, standalone public-header checks and focused shared HTTP/lifecycle tests passed.
The full Debug CTest run passed 30 of 32 suites, with two existing failures reproduced in a clean checkout of the same official
upstream baseline on this host:

- `HuxerUILinuxTests`: `LinuxTextLayoutCaretHitTestingPreservesAlignedBidiGeometry`, caret Y `0` versus `5`.
- `HuxerUIRuntimeDependencyTests`: the relocated dependency fixture segfaults during dynamic-library initialization.

These assertions are retained. They are not waived or weakened by the integration.
On 2026-10-07, direct ADB instrumentation passed **126 assertions** on OnePlus PJE110 / Android 16.
The actual host focus check requires native HuxerUI because constructing HuxerUIView loads JNI. The test APK used
matching ARM64 libhuxerui.so and libc++_shared.so from an application built against revision `b12080c`;
see [Building](building.md) for `huxeruiRuntimeTestJniDirectory`. Main-thread setup errors now report through
Instrumentation instead of terminating its process. Gradle's connected test task still requires uncached UTP plugins;
the device package was installed and its Instrumentation runner invoked directly.
An isolated acgu Android build against this core revision and pinned extension forks also installed successfully.
Video playback, landscape fullscreen, database/session preservation and camera preview were checked. Four split-screen
focus switches kept the application background at #F1F1F3 and display brightness at 2454; the independent host assertion
confirms the framework fix even though the application's previous focus workaround remains for its old dependency pin.
Native Windows, macOS, iOS and Web execution has not been verified on this Linux host.

## Application adoption

Use an independent framework clone for each application and pin a verified full commit SHA in both local build
configuration and CI. Do not point multiple builds at a mutable maintenance checkout. Check any existing automatic
patch application before migrating; a patch already covered by the integration must not be applied twice.
Use the same source revision for Android Java and native code, with an explicit `--source` argument in SDK CLI builds.

Upstream `main` and the downstream integration can move independently. A new upstream release does not automatically
change applications' pins; advance them after building and testing the application against the new integration.

## Branch cleanup

On 2026-10-07, the remote branches `codex/mcpp-cli-support` and `feat/linux-resize-hitarea` were deleted.
The first tracks upstream PR #120, which is merged; the second's commits are all ancestors of `farna/main`.
The remote default reference now points to `main`. The open performance branches, the open system-color-scheme
branch, and the window-caption-controls WIP remain available for their ongoing work.

On 2026-11-11, the merge of upstream `9ae9cb0d` retired the remaining upstream-preparation branches whose content upstream now owns.
Deleted remote branches and their coverage:

- `fix/patchelf-runtime-safety`: upstream `b51f7139` (#165) has the same rejection and pinned archives.
- `perf/layout-loose-measure`: upstream measures bounded grow children only once.
- `fix/presentation-layer-anchor-transfer`: superseded by upstream `1b227315`; PR [#163](https://github.com/HuxerUI/HuxerUI/pull/163) was closed unmerged.
- `fix/windows-native-window-polish`: the corner preference and input-overlay region are covered by `bdd0987b` and `38eb34dc`, and the activation target was restored from upstream in `af8b6462`.
- `codex/paragraph-layout` and `codex/upstream-paragraph-layout`: upstream `1949e84e` owns the paragraph layout surface.
- `codex/upstream-text-field-styles`: already merged into `farna/main`.
- `fix/mcpp-disabled-profiling`: upstream `3abaa77b` excludes the profiling source from the mcpp glob.
- `codex/fix-win32-window-factory`: upstream `e613b515` renamed the factory to `CreateUiWindow`.
- `feat/system-color-scheme`: upstream `5523172a` provides per-window system appearance.

The local branches `fix/patchelf-runtime-safety`, `fix/presentation-layer-anchor-transfer`, and `feat/linux-resize-hitarea` were deleted with their prunable worktrees.
`fix/windows-native-window-polish` remains checked out in `/home/farna/dev/cpp/mcpp/HuxerUI-upstream-win32`; removing that worktree drops the branch.
`codex/fix-win32-window-factory` also remains checked out in its Codex worktree while the remote branch is gone.
The performance, window-caption-controls, and multi-window feature branches remain available for ongoing work.

On 2026-11-13, after upstream `adea3ba8` covered the installer work, the following branches and clean preparation worktrees were deleted:

- `fix/windows-installer-runs` (remote tip `9def0481`; upstream rewrite `adea3ba8` and `b2ae8e8f`) with `/home/farna/dev/cpp/mcpp/HuxerUI-upstream-installer`.
- `codex/fix-win32-window-factory` (`c81042d7`) with its Codex worktree; upstream `e613b515` renamed the factory.
- `fix/windows-native-window-polish` (`cd7d44a7`) with `/home/farna/dev/cpp/mcpp/HuxerUI-upstream-win32`; covered by `bdd0987b`, `38eb34dc`, and `af8b6462`.
- `codex/linux-glass-native` (`c1cce090`) and `codex/macos-multi-window-preview` (`ad73d2b7`) with their clean Codex worktrees.
- `codex/application-scoped-drag`, `codex/linux-floating-windows`, `codex/linux-liquid-glass-opt`, `codex/macos-floating-windows`, `feat/disabled-scroll-programmatic`, and `feat/multi-window`, all merged into `farna/main`.

The remote branches `codex/macos-floating-windows`, `feat/disabled-scroll-programmatic`, and `fix/windows-installer-runs` were deleted.
The remote `fix/windows-installer-runs` tip `9def0481` carried `4f53e7f4`, `6d854e48`, and `9def0481` beyond the local preparation branch; upstream's squash `adea3ba8` owns that behavior.

`feat/scroll-fill-viewport` keeps its worktree at `/home/farna/dev/cpp/mcpp/HuxerUI-upstream-fill-viewport` because it has uncommitted work.
`farna/upstream-2026-10`, `farna/worktree-snapshot-20261018`, `probe/ui-perf`, `origin/perf/text-measurement-memo`, and `origin/wip/window-caption-controls` remain available.

## llm-switch patch intake

On 2026-10-07, llm-switch's CI-applied checkout patches were folded into the fork:

- `bcead3f` carries `cmake/patches/huxerui-windows-icon.patch`: the Win32 window class and `WM_SETICON` load the
  icon resource named by `HUXERUI_WINDOWS_APPLICATION_ICON_RESOURCE_ID` (default `0` keeps upstream behavior).
  Consumers pass the macro at compile time instead of patching the checkout.
- The Objective-C++ P0960 compatibility fix (`huxerui-macos-p0960.patch`) was already covered by the Clash-Flux
  WindowTitleBar brace-initialization change and needed no further action.
- The llm-switch UI performance probe (`huxerui-ui-perf-probe.patch`) lives on the `probe/ui-perf` branch rather
  than `farna/main`, keeping production builds free of the instrumentation. It applies against the same revision
  and stays inert unless `LLMSWITCH_PROBE_CLICK` is set.

The Windows icon change is compiled only by the Windows adapter and could not be built or run on this Linux host;
it is a straight port of the patch llm-switch CI already applies and validates on Windows.

## Multi-window development

The multi-window API follows upstream's explicit content factories: `ApplicationHandle::OpenWindow(content, options)` creates a Runtime-owned window through the platform `CreateUiWindow` factory.
Applications retain `WindowHandle` values and attach business metadata to their own models or factory captures.
[Multi-Window Design](../design/multi-window.md) defines the ownership, per-window state, windowless startup, and last-window close policy.
The downstream extensions retain windowless startup, floating utility presentation, window relationships, and cross-window drag support.

[Window Relationship Design](../design/window-relationship.md) defines ownership, modality, and child-first cascade through the same Runtime maintenance path.
Windows and Linux implement native window creation; macOS remains a preview requiring its own CI and interactive validation.
Android, iOS, and Web retain platform-driven attachments without programmatic top-level creation.
[Developing Multi-Window](multi-window.md) lists host validation commands, behavioral notes, and the porting checklist.

## Deferred upstream ownership

The upstream author will manage transparency and Gaussian blur centrally.
The fork's backdrop and liquid-glass materials (`include/huxerui/effects.h`, [Backdrop effects](../design/backdrop-effects.md), and [Glass](../guide/glass.md)) remain fork-only capability for local use, but they are not prepared for upstream PRs and need no fork assistance (2026-11-13).
