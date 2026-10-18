# Liquid Glass

This example demonstrates a moving background, frosted panels, refractive button edges, retained press feedback, direct-child fusion, controlled text editing, and anchored or modal glass surfaces.
Use **Move background** to change the source, **Fuse glass** to join the decorative button surfaces, and **Left** or **Right** to verify that their actions remain independent.

## Desktop

Build `example_liquid_glass` in a source configuration with `HUXERUI_BUILD_EXAMPLES=ON`.

```sh
cmake --build <build-directory> --target example_liquid_glass
```

## Android

Use the repository's existing Android example runner with a configured Android SDK, NDK, Java and Gradle wrapper.
The minimum Android API remains 23; select the ABI supported by the connected device.
The example's manifest sets its display name to **Liquid Glass**.

From `platform/android`:

```sh
sh ./gradlew :example_runner:assembleDebug \
    -PhuxeruiExample=liquid_glass \
    -PhuxeruiAbis=arm64-v8a
adb install --no-incremental -r example_runner/build/outputs/apk/debug/example_runner-debug.apk
adb shell am start -n org.huxerui.examples/.MainActivity
```

Use `assembleRelease` for optimized native code; its unsigned APK requires a development signature before installation.
The example runner uses the `org.huxerui.examples` application package, so selecting a different example changes the content of that runner application.

Precise hardware Canvas rendering at API 29 or later uses GLES 3 shader planes and immutable hardware Bitmap publication when the required GPU capabilities are available.
Eligible Fast scenes can use HWUI directly from API 33, with optical blur requiring API 35.
API 23–28 and software Canvas rendering retain the portable CPU path.
The portable CPU backdrop path can be expensive on dense phone screens, especially during background movement, scrolling or multiple simultaneous materials.
An optimized build helps, but this example does not establish a 60 FPS guarantee.
See [Frosted and Liquid Glass](../../docs/guide/glass.md) for source isolation, capture limits and supported geometry.

## Layered performance measurements

Open **Performance levels** to compare the same six surfaces, foreground content, fills and scrolling geometry.
**L0 Plain** omits backdrop commands, **L1 Transfer** retains zero-blur, zero-refraction and zero-highlight backdrop commands, **L2 Blur** applies only an 18 DIP blur, and **L3 Optics** adds 5 DIP refraction, 0.24 edge lighting, 1.1 saturation and 0.95 brightness to the same blur.
With Precise, L1 retains capture, identity optical composition, clipping and publication; it is a baseline for the complete backdrop pipeline, not an isolated memory-copy benchmark.
The Fast HWUI path can omit neutral zero-blur filters, so L1 work also depends on the selected material policy and backend.
The controls themselves use an ordinary theme without backdrop effects.
**Source: fixed** keeps the background stationary while surfaces scroll; **Source: scrolling** moves the background with the content.
**Quality: precise** is the default; toggle it to **Quality: fast** to compare the explicitly approximate GPU blur while preserving the same surface geometry and foreground.
Changing quality resets scroll before measurement.
Changing source mode and **Reset scroll** return the content to its initial offset.
These source modes are separate workloads: a fixed background can still require new captures and kernels when scrolling changes their device rectangles.

After installing the matching signed Release APK, run the ADB harness from the repository root:

```sh
python3 tests/platform/android/glass_levels.py \
    --adb "$ANDROID_HOME/platform-tools/adb" \
    --apk /absolute/path/to/installed-release.apk \
    --profiling OFF \
    --output /tmp/glass-levels-release
```

The output directory must be new, and `--serial` selects a device when several are connected.
The harness restarts only the example application, selects each level, warms four alternating swipes, resets the scroll position and records four identical one-second swipes.
It runs two repetitions by default, reversing level order in the second, and retains APK identity, device configuration, UI state, ATrace, process-filtered logs, graphics statistics, memory snapshots and thermal state.
Use `--levels 2` for a focused L2 run or `--levels 2,1` to compare blur and transfer overhead.
Pass `--blur-quality precise` or `--blur-quality fast` to select and verify the material before each measured sequence in this example.
Omitting it retains compatibility with older benchmark APKs; the metadata records whether quality was explicitly selected.
Use `--levels 3` for the optical workload or `--levels 4` for fusion; the default retains L0–L2.
L4 retains the panel geometry, foreground and fill while using two rounded backdrop shapes separated by 12 DIP and fused with a 32 DIP radius in one command.
The Android runner defaults to `-PhuxeruiNativeBlur=auto`: select `Quality: fast` to permit HWUI for a scene whose backdrop commands all select Fast.
Neutral blur requires API 33; optical blur and fusion require API 35 and support up to 64 shapes per command.
Precise, mixed-quality and zero-blur optical scenes retain the existing renderer; unsupported mappings or resource admission also fall back for the complete scene.
For a private backend comparison, build otherwise identical APKs with `-PhuxeruiNativeBlur=off` and `auto`, and explicitly select Fast in both.
`matched` uses the same radius policy as `auto`; `nominal` passes radius directly to Android for diagnostics, and neither mode overrides quality admission.
Platform Gaussian output is approximate and is not byte-equivalent to the default Precise box filter.
It does not change device-wide graphics or refresh-rate settings or clear device logs.
No layout or material selection occurs during a measured swipe sequence.

Use `-PhuxeruiExampleProfiling=ON` for a separate diagnostic APK and pass `--profiling ON` to label its run.
Native CPU sections, per-frame work counters and optional `GL_EXT_disjoint_timer_query` stage timings are active during ATrace capture; query collection is bounded and checks availability without adding GPU waits or pixel readback.
Unsupported timer capabilities are reported, and disjoint query samples are discarded.
The timing stages are raster cache composition, raster import/composition, complete optical-plane processing and final export copy; optical timing includes source capture and clip composition, not just blur.
Profiling builds also report per-frame CPU wall time for capture resizing, source preparation, kernel resizing, blur submission and mask upload.
Frame IDs associate work counters, asynchronously collected GPU queries and native frame wall/thread CPU times in `frame_records`.
Wall time minus thread CPU time includes blocking and descheduling; it does not identify which GPU command caused a wait.
The harness requests scheduler events when the device lists the category; native frame clocks remain available when scheduler tracing is unavailable.
Source and blur cache miss reasons are bit sets: missing source `1`, frame-size invalidation `2`, changed source inputs `4`, changed capture geometry `8`, budget exclusion `16`, ineligible replay `32`, missing cached kernel `64`, changed kernel radius or blur quality `128`, and changed kernel geometry `256`.
Several causes can coexist; input changes include complete paint programs and their device placements, rather than just application data changes.
`rect_clips` counts plane replacements using exact integral rectangular clips; `mask_clips` counts planes retaining coverage masks, while group-composition masks are accounted for in uploaded bytes.
`clip_fallback` is a bit set identifying paths `1`, rounded rectangles `2`, non-axis-aligned mappings `4`, and nonintegral or nonfinite device edges `8`.
`stable_full_frame` identifies planes admitted to the budgeted stable-background cache after repeated source equality and capture-window movement; changing sources retain cropped kernels.
These measure driver calls and any implicit blocking within them; GPU stage durations are separate measurements and cannot be subtracted mechanically from the CPU timings.
Detailed GPU timing and logging can perturb a run, so use the profiling-disabled APK for the primary performance comparison.
Java hardware-submission, image-delivery and producer-fence sections remain available through ATrace.

`View#draw()` spans measure CPU recording and synchronous renderer work, not physical display presentation.
`process_trace` additionally summarizes process-owned HWUI sections on the UI and RenderThread threads, while `trace` retains its UI-thread scope.
Window frame records include UI recording, RenderThread queue/prepare and issue-to-GPU-completion intervals under `pipeline_stages_ms`, with correlated samples in `pipeline_frame_records`.
The last interval includes submission and waits; it is not an isolated GPU shader time, and missing or invalid stage timestamps produce no sample.
Per-stage percentiles need not describe the same frame or add up to the complete-frame percentile.
Where accessible, SurfaceFlinger layer history provides a separate recent presentation-interval sample; this bounded history does not cover every frame in the trace, and intervals include application scheduling and gesture gaps.
The harness retains all intervals and also reports intervals below 250 ms separately, counting longer gaps explicitly.
`gfxinfo` summaries report HWUI completion and deadline misses and can include offscreen HardwareRenderer contexts, so their counts and percentiles do not necessarily describe only window presentation.
Unavailable detailed frame records remain empty rather than being treated as successful frames.
Windowless Android fixtures additionally exercise L0–L2 at 800×600 and 1264×2780 with fixed and changing source pixels, checking that L1 retains its plane and L2 invalidates blur according to source changes.
Their recording/publication times exclude on-screen display and must be reported separately from application scrolling.
