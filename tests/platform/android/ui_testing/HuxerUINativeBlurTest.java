package org.huxerui;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorSpace;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.RenderNode;
import android.hardware.HardwareBuffer;
import android.media.Image;
import android.util.Log;

/** Checks backdrop ordering, foreground sharpness, clipping, isolation and reused node lifetimes. */
final class HuxerUINativeBlurTest {
    static void verify(HuxerUIBackdropRaster raster) {
        int assertions = 0;
        try (HuxerUINativeBlur backend = new HuxerUINativeBlur()) {
            for (int sample = 0; sample < 6; ++sample) {
                RenderNode root = new RenderNode("HuxerUI official blur invariant fixture");
                root.setPosition(0, 0, 800, 600);
                Canvas output = root.beginRecording(800, 600);
                backend.beginFrame(800, 600);
                Canvas proxy = backend.begin(output);
                Paint paint = new Paint();
                backend.drawing(proxy).drawColor(Color.BLUE);
                paint.setColor(sample % 2 == 0 ? Color.RED : Color.GREEN);
                backend.drawing(proxy).drawRect(0, 0, 400, 600, paint);
                Path clip = new Path();
                clip.addRect(new RectF(100, 100, 700, 500), Path.Direction.CW);
                backend.pushClip(proxy, clip);
                backend.blur(proxy, 24, new float[]{50, 50, 700, 500, 20, 20, 20, 20});
                backend.pop(proxy);
                paint.setColor(Color.GREEN);
                backend.drawing(proxy).drawRect(498, 320, 502, 420, paint);
                backend.blur(proxy, 18, new float[]{470, 320, 90, 100, 0, 0, 0, 0});
                Canvas group = backend.begin(proxy);
                Paint translucent = new Paint();
                translucent.setColor(0x80FF0000);
                backend.drawing(group).drawRect(100, 440, 160, 500, translucent);
                backend.blur(group, 12, new float[]{100, 440, 100, 100, 0, 0, 0, 0});
                backend.end(proxy, group, 0.5F);
                paint.setColor(Color.YELLOW);
                backend.drawing(proxy).drawRect(385, 260, 415, 300, paint);
                backend.end(output, proxy, 1);
                backend.endFrame();
                root.endRecording();
                try (Image image = raster.draw(root, 0, 0); HardwareBuffer buffer = image.getHardwareBuffer()) {
                    Bitmap hardware = Bitmap.wrapHardwareBuffer(buffer, ColorSpace.get(ColorSpace.Named.SRGB));
                    Bitmap pixels = hardware.copy(Bitmap.Config.ARGB_8888, false);
                    hardware.recycle();
                    check(pixels.getPixel(399, 80) == (sample % 2 == 0 ? Color.RED : Color.GREEN),
                            "Clipping preserves the unfiltered prefix"); ++assertions;
                    check(pixels.getPixel(400, 280) == Color.YELLOW, "Foreground stays sharp"); ++assertions;
                    int edge = pixels.getPixel(399, 180);
                    check(Color.blue(edge) > 30 && Color.blue(edge) < 225,
                            "Blur samples both sides of the current background edge"); ++assertions;
                    check(pixels.getPixel(720, 180) == Color.BLUE, "Outside replacement is unchanged"); ++assertions;
                    check(Color.green(pixels.getPixel(493, 360)) > 5,
                            "Later blur samples earlier foreground in the same frame"); ++assertions;
                    int isolated = pixels.getPixel(125, 475);
                    if (sample % 2 == 1) {
                        check(Math.abs(Color.red(isolated) - 64) <= 2
                                && Math.abs(Color.green(isolated) - 191) <= 2,
                                "Isolated blur keeps transparent alpha before group opacity"); ++assertions;
                    }
                    pixels.recycle();
                } finally {
                    root.discardDisplayList();
                }
            }
        }
        verifyAdmissionAndResize(raster);
        verifyDeviceAxes(raster);
        verifyLifecycleStress(raster);
        if (android.os.Build.VERSION.SDK_INT >= 35) {
            HuxerUIAgslBackdropTest.verify(raster);
            // The unblurred production path uses GL; nativeOutputMode=optics retains its independent AGSL gate.
        }
        Log.i("HuxerUI", "Official blur invariants: " + assertions + " assertions passed");
    }

    private static void verifyLifecycleStress(HuxerUIBackdropRaster raster) {
        int checks = 0;
        for (int epoch = 0; epoch < 4; ++epoch) {
            HuxerUINativeBlur backend = new HuxerUINativeBlur();
            try {
                for (int frame = 0; frame < 24; ++frame) {
                    int width = frame % 3 == 0 ? 320 : frame % 3 == 1 ? 640 : 800;
                    int height = width * 3 / 4;
                    boolean abort = frame % 8 == 7;
                    int color = Color.rgb(20 + frame * 7, 30 + epoch * 40, 160);
                    RenderNode root = new RenderNode("HuxerUI native lifecycle stress");
                    root.setPosition(0, 0, width, height);
                    Canvas output = root.beginRecording(width, height);
                    output.drawColor(Color.MAGENTA);
                    backend.beginFrame(width, height);
                    int reason = 0;
                    try {
                        Canvas proxy = backend.begin(output);
                        backend.drawing(proxy).drawColor(color);
                        if (abort) {
                            Path clip = new Path();
                            clip.addRect(0, 0, width, height, Path.Direction.CW);
                            for (int index = 0; index < 65; ++index) backend.pushClip(proxy, clip);
                        } else {
                            float[] effect = {18, android.os.Build.VERSION.SDK_INT >= 35 ? 3 : 0, 12,
                                    0, -0.5F, -1, 1, 1, 0};
                            backend.filter(proxy, effect, new float[]{40, 40, width - 80, height - 80, 12, 12, 12, 12});
                            Paint ink = new Paint(); ink.setColor(Color.YELLOW);
                            backend.drawing(proxy).drawRect(55, 65, 75, 85, ink);
                            backend.end(output, proxy, 1);
                        }
                    } catch (HuxerUINativeBlur.Unavailable unavailable) {
                        reason = unavailable.reason;
                    } finally {
                        backend.endFrame();
                        root.endRecording();
                    }
                    check(reason == (abort ? 2 : 0), "Lifecycle stress preserves admission reasons"); ++checks;
                    check(!backend.active(), "Every stress frame retires its active recording"); ++checks;
                    check(backend.retainedNodeCount() < 16 && backend.retainedOpticalSlots() <= 1
                                    && backend.retainedFilterBytes() <= HuxerUINativeBlur.FILTER_BUDGET_BYTES,
                            "Changing stress frames retain bounded recording resources"); ++checks;
                    Bitmap pixels = read(raster, root);
                    try {
                        check(pixels.getPixel(5, 5) == (abort ? Color.MAGENTA : color),
                                "Stress frames expose only their current published prefix"); ++checks;
                        if (!abort) {
                            check(pixels.getPixel(60, 70) == Color.YELLOW,
                                    "Stress frames preserve sharp later foreground"); ++checks;
                        }
                        if (width < 800) {
                            check(pixels.getPixel(width + 1, 10) == Color.TRANSPARENT,
                                    "Shrinking stress frames do not expose a larger frame"); ++checks;
                        }
                    } finally {
                        pixels.recycle();
                        root.discardDisplayList();
                    }
                    if (abort || frame % 8 == 6) {
                        backend.endFrame();
                        check(backend.retainedNodeCount() == 0 && backend.retainedFilterBytes() == 0
                                        && backend.retainedOpticalSlots() == 0,
                                "Aborted and inactive recordings release their complete graph"); ++checks;
                    }
                }
            } finally {
                backend.close();
            }
            check(backend.retainedNodeCount() == 0 && backend.retainedFilterBytes() == 0
                            && backend.retainedOpticalSlots() == 0 && !backend.active(),
                    "Closing a stress backend releases its recording graph"); ++checks;
        }
        Log.i("HuxerUI", "Native lifecycle stress: 96 frames, 12 aborted, " + checks + " assertions passed");
    }

    private static Bitmap read(HuxerUIBackdropRaster raster, RenderNode root) {
        try (Image image = raster.draw(root, 0, 0); HardwareBuffer buffer = image.getHardwareBuffer()) {
            Bitmap hardware = Bitmap.wrapHardwareBuffer(buffer, ColorSpace.get(ColorSpace.Named.SRGB));
            Bitmap result = hardware.copy(Bitmap.Config.ARGB_8888, false);
            hardware.recycle();
            return result;
        }
    }

    private static void verifyAdmissionAndResize(HuxerUIBackdropRaster raster) {
        int checks = 0;
        try (HuxerUINativeBlur backend = new HuxerUINativeBlur()) {
            for (int kind : new int[]{1, 2, 3}) {
                RenderNode root = new RenderNode("HuxerUI unsubmitted native admission fixture");
                int width = kind == 3 ? 4096 : 800;
                int height = kind == 3 ? 4096 : 600;
                root.setPosition(0, 0, width, height);
                Canvas output = root.beginRecording(width, height);
                output.drawColor(Color.MAGENTA);
                backend.beginFrame(width, height);
                Canvas proxy = backend.begin(output);
                int reason = 0;
                try {
                    if (kind == 3) {
                        for (int i = 0; i < 8; ++i) proxy = backend.begin(proxy);
                    } else {
                        for (int i = 0; i < 200; ++i) {
                            backend.blur(proxy, kind == 1 ? 18 : 1,
                                    kind == 1 ? new float[]{0, 0, 800, 600, 0, 0, 0, 0}
                                              : new float[]{20, 20, 2, 2, 0, 0, 0, 0});
                        }
                    }
                } catch (HuxerUINativeBlur.Unavailable exception) {
                    reason = exception.reason;
                } finally {
                    check(backend.retainedFilterBytes() <= HuxerUINativeBlur.FILTER_BUDGET_BYTES,
                            "Filter reservation never exceeds the byte budget"); ++checks;
                    backend.endFrame();
                    root.endRecording();
                }
                check(reason == kind, "Admission rejects the appropriate bounded resource"); ++checks;
                check(backend.retainedNodeCount() == 0 && backend.retainedFilterBytes() == 0,
                        "An aborted frame retires all candidate layers"); ++checks;
                Bitmap pixels = read(raster, root);
                check(pixels.getPixel(100, 100) == Color.MAGENTA,
                        "Unsubmitted candidate drawing never reaches the destination"); ++checks;
                pixels.recycle(); root.discardDisplayList();
            }
            for (int[] size : new int[][]{{640, 480}, {800, 600}, {320, 240}, {800, 600}}) {
                RenderNode root = new RenderNode("HuxerUI resized native fixture");
                root.setPosition(0, 0, size[0], size[1]);
                Canvas output = root.beginRecording(size[0], size[1]);
                backend.beginFrame(size[0], size[1]);
                Canvas proxy = backend.begin(output);
                backend.drawing(proxy).drawColor(Color.BLUE);
                backend.end(output, proxy, 1);
                backend.endFrame();
                root.endRecording();
                Bitmap pixels = read(raster, root);
                check(pixels.getPixel(size[0] - 10, size[1] - 10) == Color.BLUE,
                        "Resized recordings preserve their current contents"); ++checks;
                if (size[0] < 800) {
                    check(pixels.getPixel(size[0] + 1, 20) == Color.TRANSPARENT,
                            "Resizing does not expose stale pixels from a larger frame"); ++checks;
                }
                check(backend.retainedFilterBytes() == 0 && backend.retainedNodeCount() < 8,
                        "Small frames release unused filter layers and node slots"); ++checks;
                pixels.recycle(); root.discardDisplayList();
            }
        }
        Log.i("HuxerUI", "Official blur admission and resize: " + checks + " assertions passed");
    }

    private static void verifyDeviceAxes(HuxerUIBackdropRaster raster) {
        Bitmap reference = null;
        try (HuxerUINativeBlur backend = new HuxerUINativeBlur()) {
            for (float[] transform : new float[][]{
                    {1.5F, 0, 0, 0, 0.5F, 0, 0, 0, 1},
                    {0, -1.5F, 600, 0.5F, 0, 0, 0, 0, 1},
                    {-1.5F, 0, 600, 0, 0.5F, 0, 0, 0, 1}}) {
                RenderNode root = new RenderNode("HuxerUI device-axis native fixture");
                root.setPosition(0, 0, 800, 600);
                Canvas output = root.beginRecording(800, 600);
                backend.beginFrame(800, 600);
                Canvas proxy = backend.begin(output);
                Paint paint = new Paint();
                for (int y = 0; y < 600; y += 8) {
                    paint.setColor((y / 8) % 2 == 0 ? Color.RED : Color.GREEN);
                    backend.drawing(proxy).drawRect(0, y, 800, y + 8, paint);
                }
                paint.setColor(Color.BLUE);
                backend.drawing(proxy).drawRect(300, 0, 400, 600, paint);
                Matrix matrix = new Matrix(); matrix.setValues(transform);
                proxy.save(); proxy.concat(matrix); backend.pushTransform(proxy, matrix);
                backend.blur(proxy, 24, new float[]{100, 100, 200, 200, 0, 0, 0, 0});
                backend.pop(proxy); proxy.restore();
                backend.end(output, proxy, 1); backend.endFrame(); root.endRecording();
                Bitmap actual = read(raster, root);
                if (reference == null) reference = actual;
                else {
                    check(reference.sameAs(actual),
                            "Equivalent device-axis blur survives quarter turns and reflections exactly");
                    actual.recycle();
                }
                root.discardDisplayList();
            }
        } finally {
            if (reference != null) reference.recycle();
        }
        Log.i("HuxerUI", "Official blur device axes: 2 equivalence assertions passed");
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError("HuxerUI " + message);
    }
}
