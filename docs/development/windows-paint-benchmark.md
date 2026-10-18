# Windows paint benchmark

This source-only benchmark compares the Windows native paint replay before and after resource-cache changes.
It is independent of the application frame scheduler and does not create another production rendering path.

## Run locally

Use PowerShell 7 and an existing ordinary Windows MSVC CMake build containing `lib/Release/huxerui_static.lib`.
The runner reads that build's generator and Visual Studio instance instead of choosing another toolchain.
It configures a separate small project under the existing build directory, importing the already-built framework archive and the native link interface from `cmake/platform/Windows.cmake`.
The project's targets are separate from the framework, SDK packaging, and ordinary CTest runs.

```powershell
cmake --build build/check --config Release --target huxerui_static --parallel 4
./scripts/benchmark_windows_paint.ps1 -BuildDirectory build/check -CleanCheck
```

`-CleanCheck` additionally configures and builds a fresh benchmark project, then checks the incremental build.
It does not delete existing build output.
The default baseline is commit `c1cce090f7fc59f88c8b7087e7a81e47ae68657a`, which handles backdrop commands but predates the Windows paint-resource caches; pass `-BaselineRef` to select another compatible baseline explicitly.
The baseline renderer must handle the current PaintCommand variant; older renderers cannot compile against newly added commands.
The runner extracts that commit's renderer as exact bytes, snapshots the working-tree renderer, and compiles the two snapshots separately using the same harness and Release flags.
The private renderer and text-layout classes are renamed in these executables to avoid collisions with the imported archive.
The framework's public API and normal renderer compilation are unchanged.

Defaults are five rounds, 64 warmup frames and 160 measured frames per process, at 96 and 144 DPI.
The logical viewport is 1280 × 720 DIPs, producing 1280 × 720 and 1920 × 1080 pixel targets respectively.
Each variant/scenario/DPI/round uses a fresh process, and baseline/optimized order alternates between rounds.
Run on a quiet machine with a stable power source and power scheme; avoid compiling, running graphics tests, or interacting with GPU-heavy applications during measurement.
The runner does not change CPU affinity, clock frequencies, or the power scheme.
The harness does not create a Runtime or start its profiling recorder.

## Scenarios and measurements

| Scenario | Workload |
| --- | --- |
| `empty` | Clear and background fill, measuring the fixed native replay cost |
| `gradients_static` | 96 Path fills alternating linear and radial gradients with shared stops |
| `gradients_animated` | Same fills with changing endpoints and centers, retaining shared stops |
| `gradients_churn` | 96 distinct stop sets per frame with changing paths, exceeding cache capacity |
| `shadows_static` | 96 rounded Path shadows sharing geometry and blur configuration |
| `shadows_animated` | Same Path shadows with changing color and offset |
| `shadows_churn` | 96 distinct, changing shadow geometries per frame, exceeding cache capacity |
| `mixed` | 96 gradient-filled surfaces with Path shadows and changing paint parameters |

Scenes are recorded before timing and their 64 deterministic phases are reused.
The timed region calls the actual native scene traversal into an offscreen Direct2D target, with no swap chain or `Present()` call.

- `submit_us` is wall time around `BeginDraw()`, native replay, and `EndDraw()`, including resource creation, driver submission, and any wait inside those calls; it is not a CPU-utilization percentage.
- `gpu_us` is the difference between D3D11 GPU timestamps, using their disjoint query's frequency; disjoint samples are rejected with bounded retries, and intervals can include GPU idle time between submitted commands rather than measuring utilization.
- `completed_us` includes GPU completion polling after submission, with a 1 ms polling sleep; it is diagnostic latency rather than a presentation-frame or FPS estimate.
- `process_cpu_us` is process CPU time divided by sample count and includes native worker activity and query polling; it is separate from submission wall time.
- `private_before`, `private_after`, and `working_set` are process memory measurements around the timed batch, excluding pixel readback allocation; they do not measure GPU VRAM.

Every query wait has a timeout.
An end-of-run pixel checksum is calculated outside the timed region, and the runner rejects a baseline/optimized pixel or adapter mismatch.
Functional pixel tests separately verify sampling, shadow interiors, transforms, clipping, opacity, DPI, resize, cache eviction, oversized resources, and device recreation.

## Results and interpretation

Each run writes `metadata.json`, renderer snapshots, per-process device logs and raw CSV files, combined `samples.csv`, per-round `rounds.csv`, and `summary.csv` under `build/check/paint-benchmark-runs/<timestamp>/`.
Metadata identifies the exact baseline commit, both renderer source hashes, dirty worktree, compiler, CPU, OS, installed display drivers, power scheme, and sample configuration.
Device logs identify the adapter actually chosen by D3D11, including a software adapter when the hardware path is unavailable.
Source hashes are necessary because the optimized renderer may contain uncommitted changes.

Recalculate summaries without executing or rebuilding the benchmark:

```powershell
./scripts/benchmark_windows_paint.ps1 -SummarizeDirectory build/check/paint-benchmark-runs/<timestamp>
```

`summary.csv` contains pooled sample medians and p95 values, process CPU time, private memory, and the minimum/maximum paired round changes in submission time.
Negative changes mean lower time; a paired range spanning zero does not demonstrate a consistent improvement across those rounds.

Compare medians and p95 values together, inspect round-to-round variation, and retain the empty control and cache-churn cases when evaluating a change.
A cache-hit improvement alone does not establish a universal frame-rate improvement or a favorable memory tradeoff.
Treat these results as a local paint-replay baseline; they exclude composition, layout, scene recording, swap-chain presentation, desktop composition, and complete application workloads.

## Local reference: 2026-10-18

The [saved summary](../../tests/platform/windows/paint_benchmark/baseline_2026_10_18.csv) and [machine/source metadata](../../tests/platform/windows/paint_benchmark/baseline_2026_10_18.json) establish the first reference for this change.
That historical run used `fa04cb45bd1f405e0c6a06073b2be1db539b8661` before backdrop commands were introduced; its saved results are separate from subsequent runs using the current compatible default.
The host used an i5-13500H, Intel Iris Xe Graphics driver `32.0.101.5972`, Windows 11 build 26300, and MSVC 19.51.36252.0 x64 Release `/O2 /Ob2 /DNDEBUG`, with the Balanced power scheme and no affinity or clock lock.
Both variants selected the Intel adapter rather than WARP.
Five rounds produced 800 measured samples per variant/scenario/DPI, 25600 samples in total, 80 matching end-of-run pixel pairs, and zero rejected GPU timing samples.
The raw run is retained locally at `build/check/paint-benchmark-runs/20261018-193104`; source snapshots and hashes identify the uncommitted optimized implementation.

Submission p50 below is in milliseconds, shown as baseline → optimized:

| Scenario | 96 DPI | 144 DPI |
| --- | --- | --- |
| Empty control | 0.232 → 0.226 | 0.225 → 0.222 |
| Static gradients | 14.043 → 0.957 | 13.932 → 0.977 |
| Animated gradients | 14.573 → 0.924 | 13.781 → 0.902 |
| Gradient cache churn | 14.752 → 11.320 | 14.213 → 11.113 |
| Static shadows | 5.402 → 4.532 | 4.490 → 4.488 |
| Animated shadows | 5.418 → 5.206 | 5.182 → 4.967 |
| Shadow cache churn | 5.988 → 5.491 | 5.182 → 5.068 |
| Mixed surfaces | 18.487 → 4.962 | 18.815 → 5.330 |

Static/animated gradient submission improved by about 93%, consistently across paired rounds; mixed submission improved by 72–73%.
At 96 DPI, static-gradient submission p95 fell from 19.530 ms to 2.012 ms, and mixed p95 fell from 25.935 ms to 10.618 ms.
The shadow GPU intervals were essentially unchanged: static shadows measured approximately 1.912 ms at 96 DPI and 2.51 ms at 144 DPI in both variants.
Static shadows improved consistently in the five 96-DPI pairs, but the 144-DPI static-shadow paired changes ranged from -17.2% to +34.2%; animated and churn shadow ranges also crossed zero.
Those shadow results do not establish a consistent general improvement, and 144-DPI static/animated shadow process CPU medians increased even though their pooled submission medians were flat or slightly lower.

At 96 DPI, static-gradient private memory fell from 108.20 MiB to 58.54 MiB, while gradient cache churn increased it from 108.41 MiB to 119.42 MiB.
The churn tradeoff is therefore approximately 11 MiB of additional private memory for 22–23% lower submission time on this host.
Shadow and mixed batches showed about 4–4.6 MiB of private-memory growth after the ordinary 64-frame warmup.
A separate static-shadow probe with 512 warmup and 512 measured frames ended at 134.70 MiB baseline and 117.04 MiB optimized, down from 147.15 MiB and 129.19 MiB respectively; its pixel checksums matched.
These finite process-memory observations do not measure VRAM or establish an absence of leaks, and short-batch growth alone should not be interpreted as a leak.

The six focused pixel tests passed 148 assertions, covering ancestor transforms, clipping, opacity, resize, blur changes, transparent and zero-blur paths, resource eviction, DPI changes, and device recreation.
This reference supports the gradient resource-reuse improvement; further shadow optimization needs a more repeatable gain in the shadow-specific scenarios.
