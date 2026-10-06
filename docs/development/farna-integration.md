# Downstream integration

`FarnaHerry/HuxerUI` uses `farna/main` as the downstream integration branch. The initial integration was taken from
acgu's framework checkout on 2026-10-06, without switching or modifying that application's working tree.
The dedicated local maintenance clone is `/home/farna/dev/cpp/mcpp/HuxerUI-fork`.
Repository-wide maintenance rules live in [AGENTS.md](../../AGENTS.md).

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

Android's whole-host default focus highlight is disabled in `HuxerUIView` on API 26 and later. Keyboard and touch-mode
focus remain enabled. This moves the split-screen brightness correction into the framework for all consumers.
The application-side workaround can be removed after a consumer adopts and verifies this revision.

Sibling applications' patches and historical feature branches have not been merged wholesale. They require
semantic review and deduplication before adoption. Lib-MediaPlayer, Lib-WebView, Lib-Camera and other extensions
remain separate repositories with their own revisions.

## Validation

Linux Debug and Release compilation, standalone public-header checks and focused shared HTTP/lifecycle tests passed.
The full Debug CTest run passed 30 of 32 suites, with two existing failures reproduced in a clean checkout of the same official
upstream baseline on this host:

- `HuxerUILinuxTests`: `LinuxTextLayoutCaretHitTestingPreservesAlignedBidiGeometry`, caret Y `0` versus `5`.
- `HuxerUIRuntimeDependencyTests`: the relocated dependency fixture segfaults during dynamic-library initialization.

These assertions are retained. They are not waived or weakened by the integration.
The Android library and instrumentation APK compile successfully. Gradle's connected test task could not run offline
because its UTP host plugins were not cached. A direct ADB test install then waited at the locked device's package
installer; the client was cancelled. The framework instrumentation has not run on the device. The equivalent
application-side focus fix had already passed four split-screen focus switches on the same OnePlus PJE110 / Android 16.
Native Windows, macOS, iOS and Web execution has not been verified on this Linux host.

## Application adoption

Use an independent framework clone for each application and pin a verified full commit SHA in both local build
configuration and CI. Do not point multiple builds at a mutable maintenance checkout. Check any existing automatic
patch application before migrating; a patch already covered by the integration must not be applied twice.
Use the same source revision for Android Java and native code, with an explicit `--source` argument in SDK CLI builds.

Upstream `main` and the downstream integration can move independently. A new upstream release does not automatically
change applications' pins; advance them after building and testing the application against the new integration.
