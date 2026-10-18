package org.huxerui;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorSpace;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffXfermode;
import android.graphics.RenderNode;
import android.hardware.HardwareBuffer;
import android.media.Image;
import android.util.Log;

import java.nio.ByteBuffer;

/** Compares the production single-surface optical shader with the shared CPU algorithm on identical input bytes. */
final class HuxerUINativeOpticsTest {
    private static final int WIDTH = 320;
    private static final int HEIGHT = 240;
    private static int assertions;

    static void verify() {
        String property = "org.huxerui.experimental.nativeBlur";
        String previous = System.getProperty(property);
        System.setProperty(property, "matched");
        Bitmap source = Bitmap.createBitmap(WIDTH, HEIGHT, Bitmap.Config.ARGB_8888);
        int[] colors = new int[WIDTH * HEIGHT];
        for (int y = 0; y < HEIGHT; ++y) {
            for (int x = 0; x < WIDTH; ++x) {
                colors[y * WIDTH + x] = Color.argb(128 + x % 96,
                        20 + x / 2, 30 + y / 2, 210 - x / 3);
            }
        }
        source.setPixels(colors, 0, WIDTH, 0, 0, WIDTH, HEIGHT);
        byte[] input = bytes(source);
        float[][] transforms = {
                {1, 0, 0, 0, 1, 0, 0, 0, 1},
                {1, 0, 0.25F, 0, 1, 0.75F, 0, 0, 1},
                {1.5F, 0, 0, 0, 0.7F, 0, 0, 0, 1},
                {0, -1.2F, 260, 0.8F, 0, 0, 0, 0, 1},
                {-1, 0, 300, 0, 1, 0, 0, 0, 1},
                {0.96F, -0.28F, 60, 0.28F, 0.96F, 0, 0, 0, 1},
                {1.2F, 0.2F, 0, 0.35F, 0.8F, 0, 0, 0, 1},
                {1, 0, -80, 0, 1, -60, 0, 0, 1},
        };
        try (HuxerUIBackdropRaster raster = new HuxerUIBackdropRaster(WIDTH, HEIGHT);
                HuxerUINativeBlur backend = new HuxerUINativeBlur()) {
            for (int material = 0; material < 7; ++material) {
                float[] effect = {0, material == 0 || material >= 4 ? (material == 6 ? 64 : 4) : 0, 12,
                        material == 1 || material >= 4 ? 0.3F : 0, -0.5F, -1,
                        material == 2 || material >= 4 ? 0.4F : 1,
                        material == 3 || material >= 4 ? 0.85F : 1, 0};
                float[] shape = {45.25F, 35.5F, 180, 140,
                        material == 5 ? 120 : 8, 20, 12, 4};
                for (int mapping = 0; mapping < transforms.length; ++mapping) {
                    Bitmap actual = render(backend, raster, source, transforms[mapping], effect, shape, false);
                    byte[] expected = reference(input, WIDTH, HEIGHT, transforms[mapping], effect, shape);
                    compare(expected, bytes(actual), "material=" + material + " mapping=" + mapping);
                    actual.recycle();
                }
            }
            verifyFusion(backend, raster, source, input, transforms);
            verifyFusionBounds(backend, raster, source, input, transforms);
            verifyFreshAndRetainedNormals(raster, source, input, transforms);
            verifyBlurProgramTransitions(raster, source, input, transforms);
            float[] effect = {18, 4, 12, 0.3F, -0.5F, -1, 1.1F, 0.95F, 0};
            float[] shape = {20, 20, 260, 180, 16, 16, 16, 16};
            Bitmap first = render(backend, raster, source, transforms[0], effect, shape, true);
            check(first.getPixel(155, 115) == Color.YELLOW, "Later foreground stays sharp");
            check(first.getPixel(30, 110) == source.getPixel(30, 110), "Optical replacement obeys the current clip");
            check(first.getPixel(310, 230) == source.getPixel(310, 230), "Outside pixels retain partial alpha");
            check(Color.alpha(first.getPixel(120, 110)) >= 128, "SRC replacement preserves captured alpha");
            Bitmap second = render(backend, raster, source, transforms[0], effect, shape, true);
            check(first.sameAs(second), "Retained optical slots reproduce a fresh current-frame prefix");
            first.recycle(); second.recycle();
            RenderNode aborted = new RenderNode("HuxerUI optical admission abort");
            aborted.setPosition(0, 0, WIDTH, HEIGHT);
            Canvas destination = aborted.beginRecording(WIDTH, HEIGHT);
            destination.drawColor(Color.MAGENTA);
            backend.beginFrame(WIDTH, HEIGHT);
            int reason = 0;
            try {
                Canvas proxy = backend.begin(destination);
                for (int i = 0; i < 200; ++i) backend.filter(proxy, effect, shape);
            } catch (HuxerUINativeBlur.Unavailable failure) {
                reason = failure.reason;
            } finally {
                backend.endFrame();
                aborted.endRecording();
            }
            check(reason == 2, "Optical graphs retain the existing recording-node admission limit");
            try (Image image = raster.draw(aborted, 0, 0); HardwareBuffer buffer = image.getHardwareBuffer()) {
                Bitmap hardware = Bitmap.wrapHardwareBuffer(buffer, ColorSpace.get(ColorSpace.Named.SRGB));
                Bitmap pixels = hardware.copy(Bitmap.Config.ARGB_8888, false);
                check(pixels.getPixel(150, 110) == Color.MAGENTA,
                        "An aborted optical graph never publishes a partial frame");
                pixels.recycle(); hardware.recycle();
            } finally {
                aborted.discardDisplayList();
            }
            check(backend.retainedNodeCount() == 0 && backend.retainedFilterBytes() == 0
                            && backend.retainedOpticalSlots() == 0,
                    "Aborted optical recordings retire their graph and reservations");
        } finally {
            source.recycle();
            if (previous == null) System.clearProperty(property);
            else System.setProperty(property, previous);
        }
        Log.i("HuxerUI", "Production AGSL optics: " + assertions + " assertions passed");
    }

    private static void verifyFusion(HuxerUINativeBlur backend, HuxerUIBackdropRaster raster, Bitmap source,
            byte[] input, float[][] transforms) {
        for (int count : new int[]{1, 2, 3, 4, 5, 8, 9, 16, 17, 32, 33, 64, 17, 3, 1}) {
            float[] shapes = new float[count * 8];
            for (int index = 0; index < count; ++index) {
                int at = index * 8;
                shapes[at] = 15.25F + index % 8 * 36;
                shapes[at + 1] = 15.5F + index / 8 * 26;
                shapes[at + 2] = index % 11 == 5 ? 0 : 32;
                shapes[at + 3] = 26;
                shapes[at + 4] = 8; shapes[at + 5] = 12;
                shapes[at + 6] = 3; shapes[at + 7] = 6;
            }
            for (float fusion : new float[]{0, 12}) {
                float[] effect = {0, 4, 12, 0.3F, 0.8F, -0.25F, 0.7F, 0.9F, fusion};
                for (int mapping : new int[]{0, 3, 6}) {
                    Bitmap actual = render(backend, raster, source, transforms[mapping], effect, shapes, false);
                    byte[] expected = reference(input, WIDTH, HEIGHT, transforms[mapping], effect, shapes);
                    compare(expected, bytes(actual), "shapes=" + count + " fusion=" + fusion + " mapping=" + mapping);
                    check(backend.retainedOpticalSlots() == 1,
                            "Shapes in one command share one optical recording slot");
                    actual.recycle();
                }
            }
        }
        float[] neutral = {0, 0, 12, 0, -0.5F, -1, 1, 1, 32};
        float[] pair = {25, 25, 90, 140, 24, 24, 24, 24, 125, 25, 90, 140, 24, 24, 24, 24};
        Bitmap identity = render(backend, raster, source, transforms[0], neutral, pair, false);
        neutral[8] = 0;
        Bitmap ordinary = render(backend, raster, source, transforms[0], neutral, pair, false);
        check(identity.sameAs(ordinary), "Zero-blur neutral fusion preserves the ordinary raster pixels exactly");
        identity.recycle(); ordinary.recycle();
    }

    private static void verifyFusionBounds(HuxerUINativeBlur backend, HuxerUIBackdropRaster raster, Bitmap source,
            byte[] input, float[][] transforms) {
        for (float fusion : new float[]{0, 0.5F, 12, 64}) {
            for (float delta : new float[]{-0.25F, 0, 0.25F}) {
                float[] effect = {0, 8, 12, 0.3F, 0.8F, -0.25F, 0.7F, 0.9F, fusion};
                float[] shapes = {
                        20.25F, 45.5F, 80, 140, 8, 20, 12, 4,
                        100.25F + fusion + delta, 45.5F, 80, 140, 16, 8, 20, 12,
                        250.25F, 10.5F, 25, 25, 4, 8, 6, 2,
                };
                for (int mapping : new int[]{0, 1, 4, 6}) {
                    Bitmap actual = render(backend, raster, source, transforms[mapping], effect, shapes, false);
                    try {
                        byte[] expected = reference(input, WIDTH, HEIGHT, transforms[mapping], effect, shapes);
                        compare(expected, bytes(actual),
                                "fusion-boundary=" + fusion + " delta=" + delta + " mapping=" + mapping);
                    } finally {
                        actual.recycle();
                    }
                }
            }
        }
    }

    private static void verifyFreshAndRetainedNormals(HuxerUIBackdropRaster raster, Bitmap source,
            byte[] input, float[][] transforms) {
        float[] warm_shapes = scatteredShapes(64);
        for (int fixture = 0; fixture < 2; ++fixture) {
            float[] shapes = fixture == 0 ? scatteredShapes(17) : new float[]{
                    20.25F, 45.5F, 80, 140, 8, 20, 12, 4,
                    164, 45.5F, 80, 140, 16, 8, 20, 12,
                    250.25F, 10.5F, 25, 25, 4, 8, 6, 2,
            };
            float[] effect = {0, fixture == 0 ? 4 : 8, 12, 0.3F, 0.8F, -0.25F, 0.7F, 0.9F,
                    fixture == 0 ? 0 : 64};
            for (boolean warm : new boolean[]{false, true}) {
                for (int mapping : new int[]{0, 1, 6}) {
                    try (HuxerUINativeBlur backend = new HuxerUINativeBlur()) {
                        if (warm) {
                            Bitmap previous = render(backend, raster, source, transforms[0],
                                    effect, warm_shapes, false);
                            previous.recycle();
                        }
                        Bitmap actual = render(backend, raster, source, transforms[mapping], effect, shapes, false);
                        try {
                            compare(reference(input, WIDTH, HEIGHT, transforms[mapping], effect, shapes), bytes(actual),
                                    "normal-state=" + fixture + " warm=" + warm + " mapping=" + mapping);
                        } finally {
                            actual.recycle();
                        }
                    }
                }
            }
        }
    }

    private static void verifyBlurProgramTransitions(HuxerUIBackdropRaster raster, Bitmap source,
            byte[] input, float[][] transforms) {
        float[] shapes = {
                20.25F, 45.5F, 80, 140, 8, 20, 12, 4,
                164, 45.5F, 80, 140, 16, 8, 20, 12,
                250.25F, 10.5F, 25, 25, 4, 8, 6, 2,
        };
        float[] zero = {0, 8, 12, 0.3F, 0.8F, -0.25F, 0.7F, 0.9F, 64};
        float[] blurred = zero.clone(); blurred[0] = 18;
        for (int mapping : new int[]{0, 3}) {
            try (HuxerUINativeBlur backend = new HuxerUINativeBlur()) {
                Bitmap warm = render(backend, raster, source, transforms[0], zero, scatteredShapes(64), false);
                warm.recycle();
                Bitmap first_zero = render(backend, raster, source, transforms[mapping], zero, shapes, false);
                Bitmap first_blur = render(backend, raster, source, transforms[mapping], blurred, shapes, false);
                Bitmap next_zero = render(backend, raster, source, transforms[mapping], zero, shapes, false);
                Bitmap next_blur = render(backend, raster, source, transforms[mapping], blurred, shapes, false);
                try {
                    byte[] expected = reference(input, WIDTH, HEIGHT, transforms[mapping], zero, shapes);
                    compare(expected, bytes(first_zero), "program-zero-first mapping=" + mapping);
                    compare(expected, bytes(next_zero), "program-zero-restored mapping=" + mapping);
                    check(first_zero.sameAs(next_zero), "Zero-blur program switching restores identical source pixels");
                    check(first_blur.sameAs(next_blur),
                            "Positive-blur program switching restores identical source pixels");
                    check(backend.retainedOpticalSlots() == 1, "Program switching preserves one bounded optical slot");
                } finally {
                    first_zero.recycle(); first_blur.recycle(); next_zero.recycle(); next_blur.recycle();
                }
            }
        }
    }

    private static float[] scatteredShapes(int count) {
        float[] shapes = new float[count * 8];
        for (int index = 0; index < count; ++index) {
            int at = index * 8;
            shapes[at] = 15.25F + index % 8 * 36;
            shapes[at + 1] = 15.5F + index / 8 * 26;
            shapes[at + 2] = index % 11 == 5 ? 0 : 32;
            shapes[at + 3] = 26;
            shapes[at + 4] = 8; shapes[at + 5] = 12;
            shapes[at + 6] = 3; shapes[at + 7] = 6;
        }
        return shapes;
    }

    private static Bitmap render(HuxerUINativeBlur backend, HuxerUIBackdropRaster raster, Bitmap source,
            float[] values, float[] effect, float[] shape, boolean clippedForeground) {
        RenderNode root = new RenderNode("HuxerUI production optical fixture");
        root.setPosition(0, 0, WIDTH, HEIGHT);
        Canvas output = root.beginRecording(WIDTH, HEIGHT);
        backend.beginFrame(WIDTH, HEIGHT);
        try {
            Canvas proxy = backend.begin(output);
            Paint replace = new Paint(); replace.setXfermode(new PorterDuffXfermode(PorterDuff.Mode.SRC));
            backend.drawing(proxy).drawBitmap(source, 0, 0, replace);
            Matrix matrix = new Matrix(); matrix.setValues(values);
            proxy.save(); proxy.concat(matrix); backend.pushTransform(proxy, matrix);
            if (clippedForeground) {
                Path clip = new Path(); clip.addRect(80, 60, 240, 180, Path.Direction.CW);
                backend.pushClip(proxy, clip);
            }
            backend.filter(proxy, effect, shape);
            if (clippedForeground) backend.pop(proxy);
            backend.pop(proxy); proxy.restore();
            if (clippedForeground) {
                Paint foreground = new Paint(); foreground.setColor(Color.YELLOW);
                backend.drawing(proxy).drawRect(150, 110, 170, 130, foreground);
            }
            backend.end(output, proxy, 1);
        } finally {
            backend.endFrame();
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

    private static byte[] bytes(Bitmap pixels) {
        ByteBuffer buffer = ByteBuffer.allocate(pixels.getByteCount());
        pixels.copyPixelsToBuffer(buffer);
        return buffer.array();
    }

    private static void compare(byte[] expected, byte[] actual, String fixture) {
        int maximum = 0;
        int maximumIndex = 0;
        long total = 0;
        for (int i = 0; i < expected.length; ++i) {
            int error = Math.abs((expected[i] & 255) - (actual[i] & 255));
            if (error > maximum) { maximum = error; maximumIndex = i; }
            total += error;
        }
        double mean = (double) total / expected.length;
        Log.i("HuxerUI", "Production AGSL " + fixture + " max=" + maximum + " mean=" + mean);
        if (maximum > 3) {
            Log.i("HuxerUI", "AGSL largest error pixel=" + maximumIndex / 4 % WIDTH + ","
                    + maximumIndex / 4 / WIDTH + " channel=" + maximumIndex % 4
                    + " expected=" + (expected[maximumIndex] & 255) + " actual=" + (actual[maximumIndex] & 255));
        }
        check(maximum <= 3 && mean <= 0.5, "Optical pixels retain the shared tolerance: " + fixture
                + " max=" + maximum + " mean=" + mean);
    }

    private static void check(boolean condition, String message) {
        ++assertions;
        if (!condition) throw new AssertionError("HuxerUI " + message);
    }

    private static byte[] reference(byte[] rgba, int width, int height, float[] matrix,
            float[] effect, float[] shape) {
        Matrix forward = new Matrix(); forward.setValues(matrix);
        Matrix inverse = new Matrix();
        check(forward.invert(inverse), "Reference mapping is invertible");
        float[] values = new float[9]; inverse.getValues(values);
        return referencePixels(rgba, width, height, values, effect, shape);
    }

    private static native byte[] referencePixels(byte[] rgba, int width, int height, float[] pixelToLocal,
            float[] effect, float[] shape);
}
