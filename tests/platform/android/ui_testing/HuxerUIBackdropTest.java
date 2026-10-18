package org.huxerui;

import android.app.Instrumentation;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorSpace;
import android.graphics.RenderNode;
import android.hardware.HardwareBuffer;
import android.media.Image;
import android.os.Build;
import android.util.Log;

import java.lang.reflect.Field;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.Arrays;

/** Native renderer fixtures exercise both the software fallback and actual hardware Canvas publication. */
final class HuxerUIBackdropTest {
    private static int assertions;

    static void verify(Instrumentation instrumentation) throws Throwable {
        if (Build.VERSION.SDK_INT < 29) {
            Log.i("HuxerUI", "Backdrop GPU fixtures unavailable below API 29");
            return;
        }
        final Throwable[] failure = new Throwable[1];
        instrumentation.runOnMainSync(() -> {
            try {
                verifyShaders();
                Log.i("HuxerUI", "Fast blur fixtures: 32 crop/identity/alpha cases and 7 uniform cases passed");
                if (Build.VERSION.SDK_INT >= 33) verifyRapidRasters();
                HuxerUIView host = new HuxerUIView(instrumentation.getContext());
                HuxerUIView cpuHost = new HuxerUIView(instrumentation.getContext());
                long gpu = createRenderer(host);
                long cpu = createRenderer(host);
                try {
                    Bitmap retained = null;
                    for (int scenario : new int[]{0, 1, 2, 3, 4, 5, 6, 8, 9}) {
                        Bitmap expected = Bitmap.createBitmap(800, 600, Bitmap.Config.ARGB_8888);
                        Canvas software = new Canvas(expected);
                        software.scale(2.0F, 2.0F);
                        drawScenario(cpu, cpuHost, software, scenario, 0, true);
                        long[] initial = drawHardware(gpu, host, scenario, 0, true);
                        if (scenario == 0) {
                            check(initial[20] == 0, "Full-source blur avoids a cropped source copy");
                            check(initial[21] > 0, "Full-source kernels sample the captured source directly");
                            check(initial[22] > 0, "The blur cache adopts completed working results");
                        }
                        if (scenario == 0 && Build.VERSION.SDK_INT >= 33) {
                            check(initial[14] == 0, "New raster programs avoid speculative texture copies");
                        }
                        Bitmap actual = presented(host).copy(Bitmap.Config.ARGB_8888, false);
                        if (scenario == 1) {
                            long[] admitted = drawHardware(gpu, host, scenario, 0, true);
                            if (Build.VERSION.SDK_INT >= 33) {
                                check(admitted[14] > 0, "Repeated raster programs become eligible for texture reuse");
                            }
                            long[] replay = drawHardware(gpu, host, scenario, 0, true);
                            Bitmap repeated = presented(host).copy(Bitmap.Config.ARGB_8888, false);
                            check(replay[0] > 1 && actual.sameAs(repeated), "Multi-plane replay preserves complete pixels");
                            if (Build.VERSION.SDK_INT >= 33) {
                                check(replay[10] > 1, "Immutable ordinary segments reuse textures across optical planes");
                                check(replay[13] > 0 && replay[13] < 800L * 600L * (replay[7] + replay[10]),
                                        "Cached ordinary segments compose their cropped pixel regions");
                            }
                            repeated.recycle();
                        }
                        Bitmap baselineCpu = Bitmap.createBitmap(800, 600, Bitmap.Config.ARGB_8888);
                        Canvas baselineCanvas = new Canvas(baselineCpu);
                        baselineCanvas.scale(2.0F, 2.0F);
                        drawScenario(cpu, cpuHost, baselineCanvas, -scenario - 1, 0, true);
                        drawHardware(gpu, host, -scenario - 1, 0, true);
                        Bitmap baselineGpu = presented(host).copy(Bitmap.Config.ARGB_8888, false);
                        compare(expected, actual, baselineCpu, baselineGpu);
                        baselineCpu.recycle(); baselineGpu.recycle();
                        expected.recycle();
                        actual.recycle();
                        if (retained == null) retained = presented(host);
                    }
                    for (int sample : new int[]{3, 6}) {
                        Bitmap expected = Bitmap.createBitmap(800, 600, Bitmap.Config.ARGB_8888);
                        Canvas software = new Canvas(expected); software.scale(2, 2);
                        drawScenario(cpu, cpuHost, software, 8, sample, true);
                        drawHardware(gpu, host, 8, sample, true);
                        Bitmap actual = presented(host).copy(Bitmap.Config.ARGB_8888, false);
                        Bitmap identityCpu = Bitmap.createBitmap(800, 600, Bitmap.Config.ARGB_8888);
                        Canvas identityCanvas = new Canvas(identityCpu); identityCanvas.scale(2, 2);
                        drawScenario(cpu, cpuHost, identityCanvas, -9, sample, true);
                        drawHardware(gpu, host, -9, sample, true);
                        Bitmap identityGpu = presented(host).copy(Bitmap.Config.ARGB_8888, false);
                        compare(expected, actual, identityCpu, identityGpu);
                        expected.recycle(); actual.recycle(); identityCpu.recycle(); identityGpu.recycle();
                    }
                    Bitmap first = retained.copy(Bitmap.Config.ARGB_8888, false);
                    drawHardware(gpu, host, 0, 19, true);
                    Bitmap unchanged = retained.copy(Bitmap.Config.ARGB_8888, false);
                    check(first.sameAs(unchanged), "Published hardware textures remain immutable across frames");
                    first.recycle();
                    unchanged.recycle();
                    boolean rejected = false;
                    try {
                        drawHardware(gpu, host, 7, 20, true);
                    } catch (AssertionError expected) {
                        rejected = expected.getMessage().contains("blur requires");
                    }
                    check(rejected, "Unsupported blur mapping is rejected explicitly");
                    drawHardware(gpu, host, 0, 21, true);
                    check(presented(host).getWidth() == 800, "Rendering recovers after an aborted clipped frame");
                    RenderNode resized = new RenderNode("HuxerUI resized backdrop fixture");
                    Canvas smaller = resized.beginRecording(640, 480);
                    try {
                        smaller.scale(1.6F, 1.6F);
                        long[] resizeStats = drawScenario(gpu, host, smaller, 0, 22, true);
                        check(resizeStats[2] == 1 && resizeStats[4] == 0, "Resize invalidates the source cache");
                    } finally {
                        resized.endRecording();
                        resized.discardDisplayList();
                    }
                    check(presented(host).getWidth() == 640 && presented(host).getHeight() == 480,
                            "Hardware publication follows the resized pixel dimensions");
                    drawHardware(gpu, host, 0, 23, true);
                    check(presented(host).getWidth() == 800, "Raster targets recover after viewport growth");
                    long[] cached = drawHardware(gpu, host, 99, 0, true);
                    check(cached[11] == 1 && cached[7] == 0 && cached[2] == 0 && cached[3] == 0,
                            "An unchanged committed frame reuses its completed immutable Bitmap");
                    verifyLevels(cpu, gpu, cpuHost, host);
                    verifyClips(host);
                    verifyMaskReuse(host);
                    verifyStableSource(gpu, host);
                    if (Build.VERSION.SDK_INT >= 33) verifyOfficialApi(gpu, host);
                    benchmark(cpu, gpu, cpuHost, host, false, 800, 600);
                    benchmark(cpu, gpu, cpuHost, host, true, 800, 600);
                    benchmark(cpu, gpu, cpuHost, host, false, 1264, 2780);
                    benchmark(cpu, gpu, cpuHost, host, true, 1264, 2780);
                    for (int width : new int[]{800, 1264}) {
                        int height = width == 800 ? 600 : 2780;
                        for (boolean changes : new boolean[]{false, true}) {
                            benchmarkLevels(gpu, host, changes, width, height);
                        }
                    }
                } finally {
                    destroyRenderer(cpu);
                    destroyRenderer(gpu);
                }
                // The Bitmap's buffer outlives the native EGL renderer and working targets.
                Bitmap retired = presented(host).copy(Bitmap.Config.ARGB_8888, false);
                check(retired.getPixel(20, 20) != 0, "Published hardware textures outlive renderer destruction");
                retired.recycle();
                Log.i("HuxerUI", "Backdrop GPU: 23 baseline shader fixtures, "
                        + "48 shape-array/zero-blur color fixtures, "
                        + "12 refractive normal fixtures and uniform-byte sweeps, "
                        + "9 renderer scenarios plus L0-L2 and rectangular/fallback clips and abort/resize recovery, "
                        + assertions + " assertions passed");
            } catch (Throwable error) {
                failure[0] = error;
            }
        });
        if (failure[0] != null) throw failure[0];
    }

    private static void verifyOfficialApi(long renderer, HuxerUIView host) throws ReflectiveOperationException {
        String property = "org.huxerui.experimental.nativeBlur";
        String previous = System.getProperty(property);
        try (HuxerUIBackdropRaster raster = new HuxerUIBackdropRaster(800, 600)) {
            for (int scenario : new int[]{11, 12}) {
                int fastScenario = 200 + scenario;
                System.setProperty(property, "off");
                drawHardware(renderer, host, fastScenario, 0, true);
                Bitmap reference = presented(host).copy(Bitmap.Config.ARGB_8888, false);
                System.setProperty(property, "matched");
                Bitmap first = null;
                for (int sample : new int[]{0, 1, 0}) {
                    RenderNode root = new RenderNode("HuxerUI official blur renderer fixture");
                    root.setPosition(0, 0, 800, 600);
                    Canvas canvas = root.beginRecording(800, 600);
                    canvas.scale(2, 2);
                    long[] stats;
                    try {
                        stats = drawScenario(renderer, host, canvas, fastScenario, sample, true);
                    } finally {
                        root.endRecording();
                    }
                    check(stats[7] == 0 && stats[3] == 0 && stats[11] == 0,
                            "Official blur bypasses native imports, box passes and completed Bitmap cache");
                    try (Image image = raster.draw(root, 0, 0);
                            HardwareBuffer buffer = image.getHardwareBuffer()) {
                        Bitmap hardware = Bitmap.wrapHardwareBuffer(buffer, ColorSpace.get(ColorSpace.Named.SRGB));
                        Bitmap actual = hardware.copy(Bitmap.Config.ARGB_8888, false);
                        hardware.recycle();
                        if (sample == 0) {
                            int[] expected = new int[800 * 600];
                            int[] pixels = new int[expected.length];
                            reference.getPixels(expected, 0, 800, 0, 0, 800, 600);
                            actual.getPixels(pixels, 0, 800, 0, 0, 800, 600);
                            long total = 0;
                            int max = 0;
                            for (int index = 0; index < pixels.length; ++index) {
                                for (int shift : new int[]{0, 8, 16, 24}) {
                                    int error = Math.abs((pixels[index] >>> shift & 255) -
                                            (expected[index] >>> shift & 255));
                                    total += error;
                                    max = Math.max(max, error);
                                }
                            }
                            Log.i("HuxerUI", "Official blur scenario=" + scenario + " max_error=" + max
                                    + " mean_error=" + (double) total / (pixels.length * 4));
                            if (scenario == 11) check(max <= 3, "Official identity preserves complete renderer pixels");
                            if (first == null) first = actual.copy(Bitmap.Config.ARGB_8888, false);
                            else check(first.sameAs(actual), "Official prefix returns to the current source without feedback");
                        } else {
                            check(!first.sameAs(actual), "Official blur observes changed source pixels");
                        }
                        check(actual.getPixel(790, 590) == Color.rgb(29 + sample, 70, 100),
                                "Official blur leaves pixels outside its surfaces unchanged");
                        actual.recycle();
                    } finally {
                        root.discardDisplayList();
                    }
                }
                first.recycle();
                reference.recycle();
            }
            verifyNativeAdmission(renderer, host, raster);
            verifyNativeQualityAdmission(renderer, host, raster);
            if (Build.VERSION.SDK_INT >= 35) verifyNativeOpticalScenes(renderer, host, raster);
            HuxerUINativeBlurTest.verify(raster);
        } finally {
            if (previous == null) System.clearProperty(property);
            else System.setProperty(property, previous);
        }
    }

    static void verifyNativeFast(Instrumentation instrumentation) throws ReflectiveOperationException {
        if (Build.VERSION.SDK_INT < 33) throw new AssertionError("HuxerUI native Fast validation requires API 33");
        HuxerUIView host = new HuxerUIView(instrumentation.getContext());
        long renderer = createRenderer(host);
        String property = "org.huxerui.experimental.nativeBlur";
        String previous = System.getProperty(property);
        File directory = new File(instrumentation.getContext().getExternalFilesDir(null), "native-fast-validation");
        check(directory.isDirectory() || directory.mkdirs(), "Native Fast diagnostics directory is available");
        int[] scenarios = Build.VERSION.SDK_INT >= 35
                ? new int[]{0, 1, 2, 3, 4, 5, 6, 8, 9, 12, 27, 30} : new int[]{12};
        try (HuxerUIBackdropRaster raster = new HuxerUIBackdropRaster(800, 600);
                FileOutputStream metrics = new FileOutputStream(new File(directory, "scenes.tsv"))) {
            for (int scenario : scenarios) {
                int fastScenario = 200 + scenario;
                System.setProperty(property, "off");
                Bitmap custom = renderNativeAdmission(renderer, host, raster, fastScenario, false, 0, 0);
                System.clearProperty(property);
                Bitmap candidate = renderNativeAdmission(renderer, host, raster, fastScenario, true, 0, 0);
                Bitmap changed = renderNativeAdmission(renderer, host, raster, fastScenario, true, 0, 7);
                Bitmap restored = renderNativeAdmission(renderer, host, raster, fastScenario, true, 0, 0);
                try {
                    double[] error = pixelDifference(custom, candidate);
                    Log.i("HuxerUI", "Native Fast scene=" + scenario + " max=" + error[0] + " mean=" + error[1]);
                    metrics.write((scenario + "\t" + error[0] + "\t" + error[1] + "\t" + error[2] + "\n")
                            .getBytes(java.nio.charset.StandardCharsets.UTF_8));
                    check(!candidate.sameAs(changed), "Native Fast observes changed current-frame pixels");
                    check(candidate.sameAs(restored), "Native Fast restores its source without temporal feedback");
                    int foreground = 0;
                    for (int y = 0; y < 600; ++y) {
                        // The moved lower-right panel reaches the right edge below this exterior strip.
                        if (y < 500) {
                            for (int x = 780; x < 800; ++x) {
                                check(candidate.getPixel(x, y) == custom.getPixel(x, y),
                                        "Native Fast preserves the complete exterior strip");
                            }
                        }
                        for (int x = 0; x < 760; ++x) {
                            if (custom.getPixel(x, y) == Color.WHITE) {
                                check(candidate.getPixel(x, y) == Color.WHITE,
                                        "Native Fast preserves opaque foreground text cores");
                                ++foreground;
                            }
                        }
                    }
                    check(foreground > 0, "Fast comparison includes opaque foreground ink");
                    if (scenario == 0 || scenario == 2 || scenario == 27) {
                        savePixels(directory, "scene-" + scenario + "-gl.png", custom);
                        savePixels(directory, "scene-" + scenario + "-hwui.png", candidate);
                    }
                } finally {
                    custom.recycle(); candidate.recycle(); changed.recycle(); restored.recycle();
                }
            }
            System.setProperty(property, "off");
            Bitmap expected = renderNativeAdmission(renderer, host, raster, 224, false, 0);
            System.setProperty(property, "matched");
            Bitmap fallback = renderNativeAdmission(renderer, host, raster, 224, false, 1);
            Bitmap cached = renderNativeAdmission(renderer, host, raster, 99, false, 1);
            check(expected.sameAs(fallback) && fallback.sameAs(cached),
                    "Fast budget fallback publishes only the complete original-renderer frame");
            expected.recycle(); fallback.recycle(); cached.recycle();
            verifyNativeQualityAdmission(renderer, host, raster);
            HuxerUINativeBlurTest.verify(raster);
        } catch (IOException error) {
            throw new AssertionError("HuxerUI native Fast diagnostics failed", error);
        } finally {
            destroyRenderer(renderer);
            host.releaseWindow();
            if (previous == null) System.clearProperty(property);
            else System.setProperty(property, previous);
        }
        Log.i("HuxerUI", "Native Fast validation: " + scenarios.length
                + " scene comparisons, source restoration and atomic fallback passed");
    }

    private static void savePixels(File directory, String name, Bitmap pixels) throws IOException {
        try (FileOutputStream stream = new FileOutputStream(new File(directory, name))) {
            if (!pixels.compress(Bitmap.CompressFormat.PNG, 100, stream))
                throw new IOException("HuxerUI native Fast PNG encoding failed");
        }
    }

    private static Bitmap renderNativeAdmission(long renderer, HuxerUIView host, HuxerUIBackdropRaster raster,
            int scenario, boolean nativeExpected, int fallbackReason) {
        return renderNativeAdmission(renderer, host, raster, scenario, nativeExpected, fallbackReason, 3);
    }

    private static Bitmap renderNativeAdmission(long renderer, HuxerUIView host, HuxerUIBackdropRaster raster,
            int scenario, boolean nativeExpected, int fallbackReason, int sample) {
        RenderNode root = new RenderNode("HuxerUI native admission fixture");
        root.setPosition(0, 0, 800, 600);
        Canvas canvas = root.beginRecording(800, 600);
        canvas.scale(2, 2);
        try {
            long[] stats = drawScenario(renderer, host, canvas, scenario, sample, true);
            check(stats[24] == (nativeExpected ? 1 : 0), "Native backend reports the committed path");
            check(stats[23] == fallbackReason, "Native backend reports admission fallback without hiding errors");
            if (scenario == 99) {
                check(stats[11] == 1 && stats[3] == 0,
                        "Unchanged rejected scenes reuse their committed fallback without retrying native recording");
            }
        } finally {
            root.endRecording();
        }
        try (Image image = raster.draw(root, 0, 0); HardwareBuffer buffer = image.getHardwareBuffer()) {
            Bitmap hardware = Bitmap.wrapHardwareBuffer(buffer, ColorSpace.get(ColorSpace.Named.SRGB));
            Bitmap result = hardware.copy(Bitmap.Config.ARGB_8888, false);
            hardware.recycle();
            return result;
        } finally {
            root.discardDisplayList();
        }
    }

    private static void verifyNativeQualityAdmission(long renderer, HuxerUIView host, HuxerUIBackdropRaster raster) {
        String property = "org.huxerui.experimental.nativeBlur";
        String previous = System.getProperty(property);
        try {
            for (int scenario : new int[]{12, 312, 324, 226, 229, 231}) {
                System.setProperty(property, "off");
                Bitmap expected = renderNativeAdmission(renderer, host, raster, scenario, false, 0);
                try {
                    for (String mode : new String[]{null, "auto", "matched", "nominal"}) {
                        if (mode == null) System.clearProperty(property);
                        else System.setProperty(property, mode);
                        Bitmap actual = renderNativeAdmission(renderer, host, raster, scenario, false, 0);
                        try {
                            check(expected.sameAs(actual),
                                    "Ineligible quality and unblurred optics preserve complete original-renderer pixels");
                        } finally {
                            actual.recycle();
                        }
                    }
                } finally {
                    expected.recycle();
                }
            }
            System.clearProperty(property);
            Bitmap fast = renderNativeAdmission(renderer, host, raster, 212, true, 0);
            try {
                for (int scenario : new int[]{12, 212, 312, 212, 324, 212, 226, 212, 229, 212, 231, 212}) {
                    Bitmap actual = renderNativeAdmission(renderer, host, raster, scenario, scenario == 212, 0);
                    try {
                        if (scenario == 212) check(fast.sameAs(actual),
                                "Returning to Fast after a backend change never reuses another material's frame");
                    } finally {
                        actual.recycle();
                    }
                }
            } finally {
                fast.recycle();
            }
            if (Build.VERSION.SDK_INT < 35) {
                System.setProperty(property, "off");
                Bitmap expected = renderNativeAdmission(renderer, host, raster, 206, false, 0);
                System.clearProperty(property);
                Bitmap actual = renderNativeAdmission(renderer, host, raster, 206, false, 0);
                try {
                    check(expected.sameAs(actual),
                            "Unavailable optics reject the whole scene before its earlier neutral Fast command records");
                } finally {
                    expected.recycle(); actual.recycle();
                }
            }
        } finally {
            if (previous == null) System.clearProperty(property);
            else System.setProperty(property, previous);
        }
        Log.i("HuxerUI", "Native Fast quality admission, capability fallback and backend-switch pixels passed");
    }

    private static void verifyNativeAdmission(long renderer, HuxerUIView host, HuxerUIBackdropRaster raster) {
        String property = "org.huxerui.experimental.nativeBlur";
        System.setProperty(property, "off");
        Bitmap expected = renderNativeAdmission(renderer, host, raster, 224, false, 0);
        System.setProperty(property, "matched");
        Bitmap fallback = renderNativeAdmission(renderer, host, raster, 224, false, 1);
        check(expected.sameAs(fallback),
                "Budget fallback preserves complete pixels without partial native publication");
        Bitmap cachedFallback = renderNativeAdmission(renderer, host, raster, 99, false, 1);
        check(fallback.sameAs(cachedFallback), "Cached fallback preserves its original completed pixels");
        cachedFallback.recycle();
        expected.recycle(); fallback.recycle();
        Bitmap rotated = renderNativeAdmission(renderer, host, raster, 223, true, 0);
        check(rotated.getPixel(790, 590) == Color.rgb(32, 70, 100),
                "Quarter-turn nonuniform blur preserves outside pixels");
        rotated.recycle();
        boolean rejected = false;
        RenderNode invalid = new RenderNode("HuxerUI invalid native blur mapping");
        Canvas target = invalid.beginRecording(800, 600);
        target.scale(2, 2);
        try {
            drawScenario(renderer, host, target, 225, 3, true);
        } catch (AssertionError error) {
            rejected = error.getMessage().contains("blur requires");
        } finally {
            invalid.endRecording();
            invalid.discardDisplayList();
        }
        check(rejected, "Fallback preserves the original unsupported-mapping error");
        Bitmap recovered = renderNativeAdmission(renderer, host, raster, 212, true, 0);
        recovered.recycle();
    }

    private static void verifyNativeOpticalScenes(long renderer, HuxerUIView host, HuxerUIBackdropRaster raster) {
        String property = "org.huxerui.experimental.nativeBlur";
        for (int scenario : new int[]{0, 1, 2, 3, 4, 5, 6, 8, 9, 26, 27, 28, 29, 30, 31}) {
            System.setProperty(property, "off");
            Bitmap custom = renderNativeAdmission(renderer, host, raster, 200 + scenario, false, 0);
            System.setProperty(property, "matched");
            boolean nativeExpected = scenario != 26 && scenario != 29 && scenario != 31;
            Bitmap candidate = renderNativeAdmission(renderer, host, raster, 200 + scenario, nativeExpected, 0);
            double[] error = pixelDifference(custom, candidate);
            Log.i("HuxerUI", "Native optical scene=" + scenario + " max=" + error[0] + " mean=" + error[1]);
            if (scenario == 26 || scenario == 29 || scenario == 31) {
                check(error[0] <= 3 && error[1] <= 0.5,
                        "Complete zero-blur optical scenes retain the existing pixel tolerance");
            }
            check(candidate.getPixel(790, 590) == custom.getPixel(790, 590),
                    "Native optics preserves pixels outside the glass");
            Bitmap repeated = renderNativeAdmission(renderer, host, raster, 200 + scenario, nativeExpected, 0);
            check(candidate.sameAs(repeated), "Retained native optical scenes never sample a previous frame");
            repeated.recycle();
            Bitmap changed = renderNativeAdmission(renderer, host, raster, 200 + scenario, nativeExpected, 0, 7);
            check(!candidate.sameAs(changed), "Native optical scenes observe changed current-frame source pixels");
            Bitmap restored = renderNativeAdmission(renderer, host, raster, 200 + scenario, nativeExpected, 0);
            check(candidate.sameAs(restored), "Native optical scenes restore the source without temporal feedback");
            changed.recycle(); restored.recycle();
            custom.recycle(); candidate.recycle();
        }
    }

    private static void verifyRapidRasters() {
        RenderNode node = new RenderNode("HuxerUI rapid raster fixture");
        node.setPosition(0, 0, 64, 64);
        try (HuxerUIBackdropRaster raster = new HuxerUIBackdropRaster(64, 64)) {
            for (int sample = 0; sample < 128; ++sample) {
                int expected = Color.rgb(sample * 37 % 256, sample * 71 % 256, sample * 113 % 256);
                Canvas canvas = node.beginRecording(64, 64);
                canvas.drawColor(expected);
                node.endRecording();
                try (Image image = raster.draw(node, 0, 0); HardwareBuffer buffer = image.getHardwareBuffer()) {
                    Bitmap hardware = Bitmap.wrapHardwareBuffer(buffer, ColorSpace.get(ColorSpace.Named.SRGB));
                    check(hardware != null, "Rapid raster publishes a hardware buffer");
                    Bitmap pixels = hardware.copy(Bitmap.Config.ARGB_8888, false);
                    try {
                        check(pixels.getPixel(32, 32) == expected,
                                "Rapid raster delivery preserves the current submitted content");
                    } finally {
                        pixels.recycle();
                        hardware.recycle();
                    }
                }
            }
        } finally {
            node.discardDisplayList();
        }
    }

    private static Bitmap presented(HuxerUIView host) throws ReflectiveOperationException {
        Field field = HuxerUIView.class.getDeclaredField("presentedBackdropBitmap");
        field.setAccessible(true);
        Bitmap bitmap = (Bitmap) field.get(host);
        check(bitmap != null && bitmap.getConfig() == Bitmap.Config.HARDWARE, "GPU result is a hardware Bitmap");
        return bitmap;
    }

    private static long[] drawHardware(long renderer, HuxerUIView host, int scenario, int sample, boolean sourceChanges) {
        return drawHardware(renderer, host, scenario, sample, sourceChanges, 800, 600);
    }

    private static long[] drawHardware(long renderer, HuxerUIView host, int scenario, int sample,
            boolean sourceChanges, int width, int height) {
        RenderNode node = new RenderNode("HuxerUI backdrop fixture");
        Canvas canvas = node.beginRecording(width, height);
        try {
            check(canvas.isHardwareAccelerated(), "GPU fixture uses a hardware Canvas");
            canvas.scale(width / 400.0F, width / 400.0F);
            return drawScenario(renderer, host, canvas, scenario, sample, sourceChanges);
        } finally {
            node.endRecording();
            node.discardDisplayList();
        }
    }

    private static void compare(Bitmap expected, Bitmap actual, Bitmap baselineCpu, Bitmap baselineGpu) {
        int[] left = new int[800 * 600];
        int[] right = new int[left.length];
        expected.getPixels(left, 0, 800, 0, 0, 800, 600);
        actual.getPixels(right, 0, 800, 0, 0, 800, 600);
        int[] baselineLeft = new int[left.length], baselineRight = new int[left.length];
        baselineCpu.getPixels(baselineLeft, 0, 800, 0, 0, 800, 600);
        baselineGpu.getPixels(baselineRight, 0, 800, 0, 0, 800, 600);
        int maximum = 0;
        long total = 0;
        for (int i = 0; i < left.length; ++i) {
            for (int channel = 0; channel < 4; ++channel) {
                int difference = Math.abs(((left[i] >>> (channel * 8)) & 255) - ((right[i] >>> (channel * 8)) & 255));
                int rasterDifference = Math.abs(((baselineLeft[i] >>> (channel * 8)) & 255)
                        - ((baselineRight[i] >>> (channel * 8)) & 255));
                int excess = Math.max(0, difference - rasterDifference);
                maximum = Math.max(maximum, excess);
                total += excess;
            }
        }
        double mean = (double) total / (left.length * 4L);
        Log.i("HuxerUI", "Backdrop renderer optical error beyond identity raster baseline: maximum=" + maximum + " mean=" + mean);
        check(maximum <= 3 && mean <= 0.5, "Renderer pixels exceed CPU fixture tolerance: " + maximum + ", " + mean);
    }

    private static void benchmark(long cpu, long gpu, HuxerUIView cpuHost, HuxerUIView host,
            boolean sourceChanges, int width, int height) {
        double[] cpuTimes = new double[20];
        double[] gpuTimes = new double[20];
        Bitmap target = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        Canvas software = new Canvas(target);
        software.scale(width / 400.0F, width / 400.0F);
        long[] stats = null;
        for (int i = -3; i < 20; ++i) {
            int sample = i + 4;
            long begin = System.nanoTime();
            drawScenario(cpu, cpuHost, software, 0, sample, sourceChanges);
            long middle = System.nanoTime();
            stats = drawHardware(gpu, host, 0, sample, sourceChanges, width, height);
            long end = System.nanoTime();
            check(stats[0] == 1 && stats[1] == 6, "Six filled and labeled controls share one plane");
            check(stats[6] == 0, "Optical GPU processing performs no pixel readback");
            check(Build.VERSION.SDK_INT < 33 ? stats[5] > 0 : stats[5] == 0 && stats[7] + stats[10] > 0,
                    "Ordinary raster uses the device's synchronized import or Bitmap upload path");
            if (i >= 0) {
                cpuTimes[i] = (middle - begin) / 1000000.0;
                gpuTimes[i] = (end - middle) / 1000000.0;
                check(sourceChanges ? stats[2] == 1 && stats[3] == 6 && stats[4] == 0
                                    : stats[2] == 0 && stats[3] == 0 && stats[4] == 1,
                        "Source and kernel cache follows actual source pixels");
            }
        }
        Arrays.sort(cpuTimes);
        Arrays.sort(gpuTimes);
        Log.i("HuxerUI", "Backdrop benchmark " + width + "x" + height + " source_changes=" + sourceChanges
                + " CPU p50=" + cpuTimes[10] + " p95=" + cpuTimes[18]
                + " GPU p50=" + gpuTimes[10] + " p95=" + gpuTimes[18]
                + " captures=" + stats[2] + " blur_passes=" + stats[3] + " cache_hits=" + stats[4]
                + " upload_bytes=" + stats[5] + " readback_bytes=" + stats[6]
                + " imports=" + stats[7] + " mask_bytes=" + stats[8] + " blur_pixels=" + stats[9]
                + " raster_hits=" + stats[10] + " compute_passes=" + stats[12]
                + " raster_composite_pixels=" + stats[13] + " raster_cache_copy_pixels=" + stats[14]);
        target.recycle();
    }

    private static void verifyLevels(long cpu, long gpu, HuxerUIView cpuHost, HuxerUIView host) throws Exception {
        Bitmap plain = Bitmap.createBitmap(800, 600, Bitmap.Config.ARGB_8888);
        Canvas plainCanvas = new Canvas(plain);
        plainCanvas.scale(2, 2);
        drawScenario(cpu, cpuHost, plainCanvas, 10, 0, false);
        Bitmap identity = Bitmap.createBitmap(800, 600, Bitmap.Config.ARGB_8888);
        Canvas identityCanvas = new Canvas(identity);
        identityCanvas.scale(2, 2);
        drawScenario(cpu, cpuHost, identityCanvas, -13, 0, false);
        check(plain.sameAs(identity), "L1 preserves the ordinary scene pixels with zero optical parameters");
        long[] transfer = drawHardware(gpu, host, 11, 0, false);
        check(transfer[0] == 1 && transfer[1] == 6 && transfer[3] == 0,
                "L1 retains the optical plane and publication path without blur");
        Bitmap identityGpu = presented(host).copy(Bitmap.Config.ARGB_8888, false);
        Bitmap blurred = Bitmap.createBitmap(800, 600, Bitmap.Config.ARGB_8888);
        Canvas blurredCanvas = new Canvas(blurred);
        blurredCanvas.scale(2, 2);
        drawScenario(cpu, cpuHost, blurredCanvas, 12, 0, false);
        drawHardware(gpu, host, 12, 0, false);
        Bitmap blurredGpu = presented(host).copy(Bitmap.Config.ARGB_8888, false);
        // Full-renderer raster differences can spread through the blur support; direct shader fixtures
        // compare identical input pixels at these radii with the existing strict optical tolerance.
        check(!blurredGpu.sameAs(identityGpu), "L2 GPU publication contains blurred background pixels");
        check(!blurred.sameAs(plain), "L2 applies visible blur to the same geometry and source");
        plain.recycle();
        identity.recycle();
        identityGpu.recycle();
        blurred.recycle();
        blurredGpu.recycle();
    }

    private static void verifyClips(HuxerUIView host) throws Exception {
        long direct = createRenderer(host), coverage = createRenderer(host);
        try {
            for (int scenario : new int[]{13, 14, 15, 16, 17, 18, 19, 20}) {
                for (int sample : new int[]{0, 3}) {
                    long[] stats = drawHardware(direct, host, scenario, sample, true);
                    Bitmap actual = presented(host).copy(Bitmap.Config.ARGB_8888, false);
                    boolean rectangular = scenario != 14 && scenario != 16 && scenario != 17;
                    check(rectangular ? stats[15] > 0 : stats[16] > 0,
                            "Clip classification selects an exact rectangle or coverage-mask fallback: " + scenario);
                    if (rectangular && scenario != 19) {
                        check(stats[8] == 0, "Integral rectangular clipping needs no mask upload");
                    }
                    long[] reference = drawHardware(coverage, host,
                            rectangular ? scenario + 100 : scenario, sample, true);
                    Bitmap masked = presented(host).copy(Bitmap.Config.ARGB_8888, false);
                    check(stats[19] == reference[19], "Clip comparison uses equivalent source-cache admission");
                    if (!actual.sameAs(masked)) {
                        Log.i("HuxerUI", "Clip " + scenario + " sample=" + sample + " "
                                + Arrays.toString(pixelDifference(actual, masked)));
                    }
                    check(actual.sameAs(masked),
                            "Rectangle replacement preserves mask pixels and sampling halos: " + scenario);
                    actual.recycle();
                    masked.recycle();
                }
            }
        } finally {
            destroyRenderer(direct);
            destroyRenderer(coverage);
        }
    }

    private static void verifyMaskReuse(HuxerUIView host) throws Exception {
        long renderer = createRenderer(host);
        try {
            for (int scenario : new int[]{14, 16, 17, 14, 2, 3}) {
                long[] initial = drawHardware(renderer, host, scenario, 0, true);
                check(initial[8] > 0, "Changed clip geometry publishes fresh mask coverage: " + scenario);
                long[] repeated = drawHardware(renderer, host, scenario, 3, true);
                if (scenario == 14 || scenario == 16 || scenario == 17) {
                    check(repeated[8] == 0, "Identical rectangular clip programs reuse resident coverage: " + scenario);
                } else if (scenario == 3) {
                    check(repeated[8] > 0, "Path clip coverage retains conservative rebuilding");
                }
                Bitmap actual = presented(host).copy(Bitmap.Config.ARGB_8888, false);
                long cold = createRenderer(host);
                try {
                    drawHardware(cold, host, scenario, 3, true);
                    Bitmap expected = presented(host).copy(Bitmap.Config.ARGB_8888, false);
                    check(actual.sameAs(expected), "Reused clip masks preserve cold renderer pixels: " + scenario);
                    expected.recycle();
                } finally {
                    destroyRenderer(cold);
                    actual.recycle();
                }
            }
            drawHardware(renderer, host, 22, 0, true);
            long[] moved = drawHardware(renderer, host, 22, 3, true);
            check(moved[8] > 0, "Clip transform changes invalidate retained mask coverage");
            Bitmap movedPixels = presented(host).copy(Bitmap.Config.ARGB_8888, false);
            check(drawHardware(renderer, host, 22, 3, true)[8] == 0,
                    "Identical transformed clips reuse the updated coverage");
            long cold = createRenderer(host);
            try {
                drawHardware(cold, host, 22, 3, true);
                Bitmap expected = presented(host).copy(Bitmap.Config.ARGB_8888, false);
                check(movedPixels.sameAs(expected), "Changed clip transforms preserve cold renderer pixels");
                expected.recycle();
            } finally {
                destroyRenderer(cold);
                movedPixels.recycle();
            }
            for (int width : new int[]{640, 800}) {
                long[] resized = drawHardware(renderer, host, 14, 4, true, width, width * 3 / 4);
                check(resized[8] > 0, "Viewport changes rebuild mask coverage");
                long[] repeated = drawHardware(renderer, host, 14, 5, true, width, width * 3 / 4);
                check(repeated[8] == 0, "Resized targets retain only their matching clip coverage");
            }
        } finally {
            destroyRenderer(renderer);
        }
    }

    private static double[] pixelDifference(Bitmap left, Bitmap right) {
        int maximum = 0;
        long total = 0;
        int different = 0;
        int[] a = new int[left.getWidth() * left.getHeight()], b = new int[a.length];
        left.getPixels(a, 0, left.getWidth(), 0, 0, left.getWidth(), left.getHeight());
        right.getPixels(b, 0, right.getWidth(), 0, 0, right.getWidth(), right.getHeight());
        for (int i = 0; i < a.length; ++i) {
            if (a[i] != b[i]) ++different;
            for (int c = 0; c < 4; ++c) {
                int delta = Math.abs(((a[i] >>> (c * 8)) & 255) - ((b[i] >>> (c * 8)) & 255));
                maximum = Math.max(maximum, delta);
                total += delta;
            }
        }
        return new double[]{maximum, (double) total / (a.length * 4L), different};
    }

    private static void verifyStableSource(long gpu, HuxerUIView host) throws Exception {
        for (int width : new int[]{800, 1264}) {
            int height = width == 800 ? 600 : 2780;
            long[] stats = null;
            for (int sample = 0; sample < 5; ++sample) {
                stats = drawHardware(gpu, host, 21, sample, false, width, height);
            }
            check(stats[19] == 1, "Repeated identical source inputs admit a budgeted stable full-frame kernel");
            stats = drawHardware(gpu, host, 21, 100, false, width, height);
            check(stats[2] == 0 && stats[3] == 0 && stats[4] == 1,
                    "Moving glass reuses the stable source and kernel across capture tiles");
            Bitmap warm = presented(host).copy(Bitmap.Config.ARGB_8888, false);
            long cold = createRenderer(host);
            try {
                drawHardware(cold, host, 21, 100, false, width, height);
                Bitmap fresh = presented(host).copy(Bitmap.Config.ARGB_8888, false);
                double[] error = pixelDifference(warm, fresh);
                Log.i("HuxerUI", "Stable source maximum=" + error[0] + " mean=" + error[1] + " pixels=" + error[2]);
                check(error[0] <= 3 && error[1] <= 0.5,
                        "Stable full-frame kernels retain the existing direct optical pixel tolerance");
                fresh.recycle();
            } finally {
                destroyRenderer(cold);
            }
            stats = drawHardware(gpu, host, 21, 100, true, width, height);
            check(stats[19] == 0 && stats[2] == 1 && stats[3] == 6,
                    "Changed source inputs immediately retire stable full-frame reuse");
            Bitmap changed = presented(host).copy(Bitmap.Config.ARGB_8888, false);
            check(!warm.sameAs(changed), "Source updates reach moving glass without a stale cached frame");
            warm.recycle();
            changed.recycle();
        }
    }

    private static void benchmarkLevels(long gpu, HuxerUIView host, boolean sourceChanges, int width, int height) {
        for (int level = 0; level <= 2; ++level) {
            double[] times = new double[24];
            long[] totals = new long[20];
            double cold = 0;
            for (int sample = 0; sample < 28; ++sample) {
                long begin = System.nanoTime();
                long[] stats = drawHardware(gpu, host, 10 + level, sample, sourceChanges, width, height);
                double elapsed = (System.nanoTime() - begin) / 1000000.0;
                if (sample == 0) cold = elapsed;
                check(stats[0] == (level == 0 ? 0 : 1) && stats[1] == (level == 0 ? 0 : 6),
                        "Level fixtures preserve the intended ordinary or six-surface plane path");
                check(stats[6] == 0, "Level timing performs no optical readback");
                if (level < 2) check(stats[3] == 0, "L0 and L1 do not execute blur passes");
                if (sample >= 4) {
                    if (level == 2) {
                        check(stats[3] == (sourceChanges ? 6 : 0), "L2 blur follows source invalidation");
                    }
                    times[sample - 4] = elapsed;
                    for (int i = 0; i < totals.length; ++i) totals[i] += stats[i];
                }
            }
            Arrays.sort(times);
            Log.i("HuxerUI", "Backdrop level=" + level + " size=" + width + "x" + height
                    + " source_changes=" + sourceChanges + " samples=" + times.length
                    + " first_ms=" + cold + " p50_ms=" + times[12] + " p95_ms=" + times[22]
                    + " work_totals=" + Arrays.toString(totals));
        }
    }

    private static void check(boolean condition, String message) {
        ++assertions;
        if (!condition) throw new AssertionError(message);
    }

    private static native void verifyShaders();
    private static native long createRenderer(HuxerUIView host);
    private static native void destroyRenderer(long renderer);
    private static native long[] drawScenario(long renderer, HuxerUIView host, Canvas canvas,
            int scenario, int sample, boolean sourceChanges);
}
