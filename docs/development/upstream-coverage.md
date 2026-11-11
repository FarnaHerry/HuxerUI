# Upstream Coverage Registry

This registry records fork work that has been compared against `upstream/main`.
Its purpose is to stop repeated audits and accidental re-submission of behavior that upstream already has.

## Baseline

- Upstream remote: `upstream` (`https://github.com/HuxerUI/HuxerUI.git`).
- Fork integration branch: `farna/main`.
- Last merged upstream commit: `9ae9cb0d` (`fix(clipboard): make HTML font size conversion portable`).
- Fork merge commit: `ff1815e7`.
- Last audit date: 2026-11-11.

Line numbers and commit SHAs in this document are evidence at the baseline.
Prefer symbol names and focused diff checks when the baseline moves.

## Classification

- **Covered**: upstream contains equivalent code and tests, or a superseding API.
  Remove the item from upstream PR candidates.
- **Partial**: upstream contains part of the behavior.
  Record exactly which residual remains; do not submit the old fork commit wholesale.
- **Fork-only**: upstream lacks the behavior at the baseline.
  It remains an upstream PR candidate unless the fork decides not to upstream it.

A fork commit remaining in `git log upstream/main..farna/main` does not prove its content is absent.
Upstream may have fixed the same bug independently, or a later upstream refactor may have superseded it.
Check the residual diff and the upstream source, not only the commit list.

## Covered by upstream

| Item | Fork evidence | Upstream evidence | Action |
|---|---|---|---|
| `ImageFit::Cover` source rectangle clamp | `0e3e89bb` | `src/runtime/render.cpp` uses `std::min(intrinsic.{width,height}, content / scale)`; the clamp hunk has no residual diff | Drop the fix. Optional: contribute `TestImageCoverFractionalFramesKeepSourceInsideIntrinsicBounds`, which upstream does not have. |
| TextField trailing-caret clipping | `c6ecefc0` | Upstream `src/components/text_field.cpp` has the same caret-width clamp and `TestTextFieldPaintsTheTrailingCaretInsideTheEditorClip` | Drop the fix and its test. |
| Linux custom-chrome resize hit area and directional cursor | `f38d5ec0` | Upstream `platform/linux/linux_adapter.cpp` has `HitTestResizeEdge`, `LinuxResizePointerCursor`, and the GTK resize request | Drop the patch. |
| Android whole-host default focus highlight | `97609cd8` | Upstream `platform/android/huxerui/src/main/java/org/huxerui/HuxerUIView.java` calls `setDefaultFocusHighlightEnabled(false)` on API 26 and later | Drop the patch. |
| Redundant loose measurement of bounded grow children | `dce2a08b`, `c40a2da2` | Upstream `src/components/containers.cpp` already skips a grow child when `main` is bounded and `(!stretch || tight_cross)` | Drop both patches. |
| Pager reverse-transition peer retention | `3e16bf4c` (pager hunk) | Upstream `784f99a6` keeps the previous destination visible when reversing an unfinished transition; the residual fork pager diff is touchpad paging only | Drop the shared fix. |
| Hidden virtual-subtree measurement invalidation | `3e16bf4c` (virtual-layout hunk) | Upstream `9f90e005` retains pending work inside unplaced subtrees; current `PropagateVirtualLayoutInvalidation` matches | Drop the shared fix. |
| Drag preview grab-offset beyond viewport edges | `3e16bf4c` (drag-preview hunk) | Upstream `711a9eae` uses `LayerPlacementKind::Positioned` for preview placement; current fork has no residual `DragPreviewPlacement` difference | Drop the shared fix. |
| Public paragraph layout | `554d9a54`, `TextParagraphLayout`, `TextMeasurer::LayoutText` | Upstream `1949e84e` added `TextLayout` and `TextMeasurer::CreateTextLayout`; the fork API and its module export were removed during merge `a559c788` | Do not reintroduce. The fork's `TextField::TextStyles` was removed separately; `TextField` uses the plain paragraph layout. |
| Runtime multi-window API and platform creation hooks | `d6c13b6c`, `5c6198e8`, `eab74b07`, `0b289005` overlap conceptually | Upstream already declares `ApplicationHandle::OpenWindow`, `Runtime::OpenWindow`, `SupportsMultipleWindows`, and Windows/Linux/macOS `CreateUiWindow` overrides; `examples/multi_window` also exists | Do not propose "add multi-window". Propose only the remaining capabilities and fixes: relationships, windowless startup, utility/floating windows, native window requests, and cross-window drag. |
| patchelf 0.19 minimum for runtime deployment | `738fe88b` | Upstream `b51f7139` (#165) rejects older patchelf before rewriting staged binaries and pins SDK CI to the verified 0.19.2 archives | Drop the fork-only proposal; keep the merged upstream implementation. |
| Windows DWM corner preference for custom chrome | `85a8ea5e` | Upstream `bdd0987b` requests `DWMWCP_ROUND` through `DWMWA_WINDOW_CORNER_PREFERENCE` after the custom frame is applied | Drop the fork patch; the merged `win32_adapter.cpp` keeps the upstream call with the fork's numeric attribute values so pre-Windows-11 SDK toolchains still compile. |
| Windows PlatformView input-overlay window region | `e6b4ba83` | Upstream `38eb34dc` builds the overlay shield from mounted hit candidates, keeps covering controls shielded, honors custom input bounds, and covers `tests/platform/windows/platform_view.cpp` | Drop the fork patch; upstream's `BuildOverlayRegion` supersedes the container-rectangle holes. |
| Windows single-instance activation target | `c9e3b63f` | Upstream discovers the target through the hidden facilities window with `EnumWindows` in `win32_adapter.cpp`; `af8b6462` restored that path and removed the fork's primary-window marker | Drop the fork patch. |
| Win32 `CreateWindow` macro collision | `c81042d7`, PR #151 | Upstream `e613b515` renamed the Runtime factory to `CreateUiWindow`; the Windows-local `CreateApplicationWindow` helper keeps the macro away | The fork now matches upstream exactly; no separate patch. |
| mcpp compile failure with profiling disabled | `368f3f57`, PR #164 | Upstream `9b8f5d05` defaults source profiling off and `3abaa77b` excludes `src/runtime/profiling.cpp` from the mcpp glob | Drop the fork patch; the merge brings the exclusion and the option default. |
| Host system appearance preference | PR #135 | Upstream `5523172a` adds per-window `UseSystemThemeBrightness()` and `ThemeBrightness`, with native observation on every platform | Use only the upstream API; the fork branch was deleted. |
| Pager, drag-preview, and hidden virtual-subtree fixes from PR #150 | `3e16bf4c` | Upstream `784f99a6`, `711a9eae`, and `9f90e005` landed the same behavior before closing #150 | Drop the shared hunks; only the pager cancel target remains fork-only. |

## Partially covered

| Item | Upstream state | Residual fork work |
|---|---|---|
| macOS per-key-window text input | Upstream lifecycle uses `window_.keyWindow`, but `ApplicationActiveChanged(bool active)` still passes only application-level active state to text input | `373ebdad` adds `active && window_.keyWindow && !input_blocked_` and blocked drag/drop checks. Still fork-only. |
| iOS app-target link closure | Upstream `cmake/HuxerUIApp.cmake` walks `INTERFACE_LINK_LIBRARIES` and emits some `$<TARGET_FILE:>` entries | `8fd9d0e1` still adds the app target's `LINK_LIBRARIES` transitive closure, alias handling, `file(GENERATE)`, and `-framework CoreMedia`. Still fork-only. |

## Superseded upstream

These items should not be resubmitted because an upstream design change owns the problem.

| Area | Fork evidence | Upstream direction | Action |
|---|---|---|---|
| Layer anchor transfer across structural replacement | `bd8fb056` | Upstream `1b227315` unified `FocusTarget` and presentation anchors through `ViewHandle`; anchor lifetime is owned by that handle instead of per-node mount identity. PR [#163](https://github.com/HuxerUI/HuxerUI/pull/163) was closed unmerged. | Do not resubmit. The fork fix was removed during merge `c2f099c1`; `TestAnchoredPresentationFollowsViewHandleAcrossReplacement` covers popup replacement with the unified handle. |

## Checked and still fork-only

These items were checked at the baseline and remain absent from upstream.
The list is a re-audit shortcut, not a roadmap or a promise to upstream every item.

| Area | Fork evidence | Status at 2026-11-11 |
|---|---|---|
| Programmatic controller scroll on disabled containers | `db693dcd` | Fork-only |
| RefreshBox `move_content` and overscroll cancellation | `16323d5a`, `0b6400c0` | Fork-only |
| GTK retired-window reclaim and callback detachment | `99fd7ba2` | Fork-only |
| macOS native-input task and frame-commit deferral | `785eb892`, `7f0ffc9f` | Fork-only |
| macOS native quit without `NSTerminateLater` | `ca6d413d` | Fork-only |
| Windows application icon resource and handle lifetime | `bcead3fe`, `446a5a90` | Fork-only |
| Windows fullscreen covering the taskbar | `6ebc6213` | Fork-only |
| Window ownership and modality relationships | `6b34925e` and follow-up fixes | Fork-only capability |
| Transparent utility windows and native window requests | `8e2adda1`, `3aef230b`, `317328c4` | Fork-only capability |
| Application-scoped cross-window drag | `3ef0a74b` | Fork-only capability |
| Windowless startup (`AppOptions::create_initial_window`) | `a954965f`, `83617e66` | Fork-only capability |
| Backdrop and liquid-glass materials | `96a0b988` and follow-up fixes | Fork-only capability |
| `ScrollView::FillViewport` | `12cd2215` | Fork-only public API; upstream already has the internal `ScrollFillViewport` layout value used by Tabs, but no `ScrollView` method |
| Desktop touchpad paging | `00caf9a0` | Fork-only capability |
| Immutable shared HTTP request body | `329d75e2` | Fork-only capability |
| Pager release-distance and velocity thresholds | `329d75e2` | Fork-only interaction choice |
| Staged-versus-host runtime dependency conflicts | `cdffec57`, `9e67114e`, `03b438c6` | Fork-only fix |
| Host-tool static runtime option | `32666c44` | Fork-only build portability |
| Optional X11 linkage across CMake and mcpp | `ad73d2b7` | Fork-only build fix |
| `WindowTitleBar` Objective-C++ brace initialization | `3e16bf4c` | Fork-only compiler compatibility |

## Remaining upstream bug-fix candidates

Verified absent from `upstream/main` at `9ae9cb0d`. These are the bug fixes still worth a focused upstream PR; capability work stays in the table above.

| Value | Item | Fork evidence | Upstream gap |
|---|---|---|---|
| High | macOS quit enters a modal loop | `ca6d413d` | `appkit_adapter.mm` returns `NSTerminateLater` while shutdown is queued; the fork returns `NSTerminateCancel` after `RequestShutdown()` |
| High | macOS native-input reentrancy | `785eb892`, `7f0ffc9f` | No `MacNativeInputGuard` or `IsNativeInputActive`; native tasks and frame commits run during AppKit input delivery |
| High | GTK retired windows and callbacks | `99fd7ba2` | No weak window reference or canvas/toplevel callback detachment; the file picker can retain a destroyed window |
| High | Windows fullscreen stays under the taskbar | `6ebc6213` | Fullscreen still uses `HWND_TOP`; no `HWND_TOPMOST` span or `HWND_NOTOPMOST` restore |
| High | Pager cancel returns to the displayed page | `3e16bf4c` (pager hunk) | Cancel calls `BeginAnimation(mounted, displayed_index_)` instead of the committed `selected_index` |
| High | Programmatic scroll on disabled containers | `db693dcd` | `ScrollNodeBy` rejects every source when `interaction.enabled` is false |
| Medium | Windows application icon resource | `bcead3fe`, `446a5a90` | No `HUXERUI_WINDOWS_APPLICATION_ICON_RESOURCE_ID` registration or `WM_SETICON` handle lifetime |
| Medium | RefreshBox `move_content=false` overscroll | `16323d5a`, `0b6400c0` | No `move_content` style; the overscroll offset still translates the pulled content |
| Medium | macOS per-key-window text input | `373ebdad` | `ApplicationActiveChanged(active)` ignores `window_.keyWindow` and `input_blocked_` |
| Medium | iOS app-target link closure | `8fd9d0e1` | `HuxerUIApp.cmake` reads only `INTERFACE_LINK_LIBRARIES`; no `LINK_LIBRARIES` closure, alias handling, `file(GENERATE)`, or `CoreMedia` |
| Medium | Runtime dependency conflict classification | `cdffec57`, `9e67114e`, `03b438c6` | No explicit `STAGING_ROOT` or `REAL_PATH`-normalized staged-versus-host comparison; packaging aborts on mixed resolutions |
| Low | Optional X11/Xext linkage | `ad73d2b7` | `cmake/platform/Linux.cmake` always links without optional `x11`/`xext` discovery, and mcpp has no matching switch |
| Low | `WindowTitleBar` Objective-C++ braces | `3e16bf4c` (window.h hunk) | `window.h` still uses `CrossAlign(...)` instead of `CrossAlign{...}` |

`TextField::TextStyles` (PR #153) was closed without an equivalent upstream implementation and is now removed from the fork rather than carried as a candidate.

## Procedure for the next upstream merge

1. Fetch and merge upstream.
   ```bash
   git fetch upstream --prune
   git merge upstream/main
   ```
2. Update the baseline section with the merged upstream commit and date.
3. Recheck the **Partial** and **Fork-only** rows before proposing a PR:
   ```bash
   git diff --stat upstream/main..farna/main
   git diff upstream/main..farna/main -- <relevant paths>
   git grep -n '<symbol>' upstream/main -- <relevant paths>
   ```
4. Mark newly covered rows as **Covered**, add the upstream evidence, and remove the item from the upstream PR list.
5. If upstream covered only part of an item, move the exact residual into **Partially covered**.
6. Leave **Fork-only** rows in place until upstream lands the behavior, the fork drops it, or the fork decides not to upstream it.

When adding an entry, record the fork commit and a code-level upstream symbol or test path.
Do not use a same-named upstream commit as proof by itself; compare behavior and tests.
