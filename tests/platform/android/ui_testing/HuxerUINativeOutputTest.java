package org.huxerui;

import android.app.Instrumentation;
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

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;

/** Records old-backend premultiplied bytes before a patch and compares the same inputs after rebuilding. */
final class HuxerUINativeOutputTest {
    private static final int WIDTH = 320;
    private static final int HEIGHT = 240;

    static void verify(Instrumentation instrumentation, String mode) {
        if (android.os.Build.VERSION.SDK_INT >= 35 && mode.equals("benchmark")) {
            benchmark(instrumentation);
            return;
        }
        if (android.os.Build.VERSION.SDK_INT < 35 || (!mode.equals("record") && !mode.equals("compare")))
            throw new IllegalArgumentException(
                    "HuxerUI native output fixture requires API 35 and record, compare or benchmark mode");
        File directory = new File(instrumentation.getContext().getExternalFilesDir(null), "native-output-reference");
        if (!directory.isDirectory() && !directory.mkdirs())
            throw new AssertionError("HuxerUI native output reference directory is unavailable");
        String property = "org.huxerui.experimental.nativeBlur";
        String previous = System.getProperty(property);
        float[][] mappings = {
                {1, 0, 0, 0, 1, 0, 0, 0, 1},
                {1, 0, 0.25F, 0, 1, 0.75F, 0, 0, 1},
                {1.1F, 0, 0, 0, 0.75F, 0, 0, 0, 1},
                {0, -1, 280.5F, 1, 0, 0.25F, 0, 0, 1},
                {-1, 0, 300.25F, 0, 1, 0.75F, 0, 0, 1},
                {0.96F, -0.28F, 55.25F, 0.28F, 0.96F, 0.75F, 0, 0, 1},
        };
        float[][] shapes = {
                {40.25F, 30.5F, 210, 150, 8, 16, 24, 12},
                {15, 35, 120, 160, 20, 24, 12, 8, 145, 35, 120, 160, 16, 8, 24, 12},
                {-60, -35, 110, 100, 12, 8, 16, 4, 150, 150, 200, 130, 24, 12, 8, 16},
                {80.25F, 70.5F, 2.25F, 3.5F, 1, 1, 1, 1},
                scatteredShapes(),
                {20.25F, 35.5F, 76, 150, 12, 24, 6, 18,
                        110.25F, 35.5F, 76, 150, 16, 8, 20, 12,
                        200.25F, 35.5F, 76, 150, 6, 18, 12, 24},
        };
        int cases = 0;
        try (HuxerUIBackdropRaster raster = new HuxerUIBackdropRaster(WIDTH, HEIGHT);
                HuxerUINativeBlur backend = new HuxerUINativeBlur();
                FileOutputStream metrics = new FileOutputStream(new File(directory, mode + ".tsv"))) {
            for (String gaussian : new String[]{"matched", "nominal"}) {
                System.setProperty(property, gaussian);
                for (int radius : new int[]{0, 18}) {
                    for (int mapping = 0; mapping < mappings.length; ++mapping) {
                        for (int geometry = 0; geometry < shapes.length; ++geometry) {
                            for (int sample : new int[]{0, 7}) {
                                String name = gaussian + "-" + radius + "-" + mapping + "-" + geometry + "-" + sample;
                                float[] effect = {radius, geometry == 2 ? 64 : 5, 12, 0.24F, -0.5F, -1,
                                        1.1F, 0.95F,
                                        geometry == 1 ? 32 : geometry == 2 || geometry == 5 ? 16
                                                : geometry == 4 ? 12 : 0};
                                Bitmap source = source(sample);
                                Bitmap actual;
                                try {
                                    actual = render(backend, raster, source, mappings[mapping], effect,
                                            shapes[geometry], geometry);
                                } finally {
                                    source.recycle();
                                }
                                try {
                                    ByteBuffer buffer = ByteBuffer.allocate(actual.getByteCount());
                                    actual.copyPixelsToBuffer(buffer);
                                    byte[] bytes = buffer.array();
                                    File reference = new File(directory, name + ".rgba");
                                    int maximum = 0;
                                    long total = 0;
                                    if (mode.equals("record")) {
                                        try (FileOutputStream output = new FileOutputStream(reference)) {
                                            output.write(bytes);
                                        }
                                    } else {
                                        byte[] expected = read(reference, bytes.length);
                                        for (int index = 0; index < bytes.length; ++index) {
                                            int error = Math.abs((bytes[index] & 255) - (expected[index] & 255));
                                            maximum = Math.max(maximum, error);
                                            total += error;
                                        }
                                        if (maximum > 3 || (double) total / bytes.length > 0.5) {
                                            writeDiagnostic(directory, name, expected, actual);
                                            throw new AssertionError("HuxerUI native output pixels changed: " + name
                                                    + " max=" + maximum + " mean=" + (double) total / bytes.length);
                                        }
                                    }
                                    String row = name + "\t" + maximum + "\t" + (double) total / bytes.length + "\n";
                                    metrics.write(row.getBytes(java.nio.charset.StandardCharsets.UTF_8));
                                    ++cases;
                                } finally {
                                    actual.recycle();
                                }
                            }
                        }
                    }
                }
            }
        } catch (IOException failure) {
            throw new AssertionError("HuxerUI native output reference I/O failed", failure);
        } finally {
            if (previous == null) System.clearProperty(property);
            else System.setProperty(property, previous);
        }
        Log.i("HuxerUI", "Native output " + mode + ": " + cases + " cases passed, directory=" + directory);
    }

    private static float[] scatteredShapes() {
        float[] shapes = new float[16 * 8];
        for (int index = 0; index < 16; ++index) {
            int offset = index * 8;
            shapes[offset] = 6.25F + index % 4 * 76;
            shapes[offset + 1] = 6.5F + index / 4 * 56;
            shapes[offset + 2] = 24; shapes[offset + 3] = 28;
            shapes[offset + 4] = 4; shapes[offset + 5] = 7;
            shapes[offset + 6] = 5; shapes[offset + 7] = 3;
        }
        return shapes;
    }

    private static Bitmap source(int sample) {
        Bitmap bitmap = Bitmap.createBitmap(WIDTH, HEIGHT, Bitmap.Config.ARGB_8888);
        int[] colors = new int[WIDTH * HEIGHT];
        for (int y = 0; y < HEIGHT; ++y) {
            for (int x = 0; x < WIDTH; ++x) {
                colors[y * WIDTH + x] = Color.argb(128 + (x + sample) % 128,
                        x < WIDTH / 2 ? 230 : 20, (y * 3 + sample) % 256, (x / 3 + y / 5) % 2 == 0 ? 240 : 15);
            }
        }
        bitmap.setPixels(colors, 0, WIDTH, 0, 0, WIDTH, HEIGHT);
        return bitmap;
    }

    private static Bitmap render(HuxerUINativeBlur backend, HuxerUIBackdropRaster raster, Bitmap source,
            float[] values, float[] effect, float[] shapes, int geometry) {
        return render(backend, raster, source, values, effect, shapes, geometry, WIDTH, HEIGHT);
    }

    private static Bitmap render(HuxerUINativeBlur backend, HuxerUIBackdropRaster raster, Bitmap source,
            float[] values, float[] effect, float[] shapes, int geometry, int width, int height) {
        RenderNode root = new RenderNode("HuxerUI native output fixture");
        root.setPosition(0, 0, width, height);
        Canvas output = root.beginRecording(width, height);
        backend.beginFrame(width, height);
        try {
            Canvas parent = backend.begin(output);
            Canvas proxy = geometry == 3 ? backend.begin(parent) : parent;
            Paint replace = new Paint();
            replace.setXfermode(new PorterDuffXfermode(PorterDuff.Mode.SRC));
            if (width == WIDTH && height == HEIGHT) backend.drawing(proxy).drawBitmap(source, 0, 0, replace);
            else backend.drawing(proxy).drawBitmap(source, null,
                    new android.graphics.Rect(0, 0, width, height), replace);
            Matrix matrix = new Matrix();
            matrix.setValues(values);
            if (width != WIDTH || height != HEIGHT) matrix.postScale((float) width / WIDTH, (float) height / HEIGHT);
            proxy.save(); proxy.concat(matrix); backend.pushTransform(proxy, matrix);
            if (geometry == 1) {
                Path clip = new Path();
                clip.addRoundRect(20.5F, 10.25F, 280, 220, 16, 16, Path.Direction.CW);
                backend.pushClip(proxy, clip);
            }
            backend.filter(proxy, effect, shapes);
            if (geometry == 1) backend.pop(proxy);
            backend.pop(proxy); proxy.restore();
            Paint foreground = new Paint();
            foreground.setColor(Color.YELLOW);
            backend.drawing(proxy).drawRect(155, 110, 165, 120, foreground);
            if (geometry == 3) backend.end(parent, proxy, 0.65F);
            backend.end(output, parent, 1);
        } finally {
            backend.endFrame();
            root.endRecording();
        }
        try (Image image = raster.draw(root, 0, 0); HardwareBuffer buffer = image.getHardwareBuffer()) {
            Bitmap hardware = Bitmap.wrapHardwareBuffer(buffer, ColorSpace.get(ColorSpace.Named.SRGB));
            if (hardware == null) throw new AssertionError("HuxerUI native output hardware pixels are unavailable");
            try {
                return hardware.copy(Bitmap.Config.ARGB_8888, false);
            } finally {
                hardware.recycle();
            }
        } finally {
            root.discardDisplayList();
        }
    }

    private static void benchmark(Instrumentation instrumentation) {
        File directory = new File(instrumentation.getContext().getExternalFilesDir(null), "native-member-benchmark");
        if (!directory.isDirectory() && !directory.mkdirs())
            throw new AssertionError("HuxerUI native member benchmark directory is unavailable");
        int width = 960, height = 720;
        Bitmap source = source(7);
        float[] mapping = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        float[] effect = {0, 8, 12, 0.24F, -0.5F, -1, 1.1F, 0.95F, 12};
        try (HuxerUIBackdropRaster raster = new HuxerUIBackdropRaster(width, height);
                FileOutputStream metrics = new FileOutputStream(new File(directory, "frames.tsv"))) {
            for (int count : new int[]{1, 2, 4, 16, 64}) {
                int columns = count == 1 ? 1 : count == 2 || count == 4 ? 2 : count == 16 ? 4 : 8;
                int rows = count / columns;
                float[] shapes = new float[count * 8];
                for (int index = 0; index < count; ++index) {
                    int offset = index * 8;
                    shapes[offset] = 8.25F + index % columns * (300.0F / columns);
                    shapes[offset + 1] = 8.5F + index / columns * (220.0F / rows);
                    shapes[offset + 2] = 300.0F / columns - 10;
                    shapes[offset + 3] = 220.0F / rows - 10;
                    shapes[offset + 4] = 4; shapes[offset + 5] = 8;
                    shapes[offset + 6] = 6; shapes[offset + 7] = 2;
                }
                try (HuxerUINativeBlur backend = new HuxerUINativeBlur()) {
                    for (int sample = -4; sample < 20; ++sample) {
                        long start = System.nanoTime();
                        Bitmap pixels = render(backend, raster, source, mapping, effect, shapes, 0, width, height);
                        long elapsed = System.nanoTime() - start;
                        try {
                            if (sample >= 0) {
                                String row = count + "\t" + sample + "\t" + elapsed + "\n";
                                metrics.write(row.getBytes(java.nio.charset.StandardCharsets.UTF_8));
                            }
                            if (sample == 0) {
                                ByteBuffer bytes = ByteBuffer.allocate(pixels.getByteCount());
                                pixels.copyPixelsToBuffer(bytes);
                                try (FileOutputStream output = new FileOutputStream(
                                        new File(directory, "shapes-" + count + ".rgba"))) {
                                    output.write(bytes.array());
                                }
                            }
                        } finally {
                            pixels.recycle();
                        }
                    }
                }
            }
        } catch (IOException failure) {
            throw new AssertionError("HuxerUI native member benchmark I/O failed", failure);
        } finally {
            source.recycle();
        }
        Log.i("HuxerUI", "Native member benchmark: 960x720, four warmups and 20 measured frames per count;"
                + " includes replay, submission, synchronization and pixel readback, directory=" + directory);
    }

    private static byte[] read(File path, int size) throws IOException {
        byte[] bytes = new byte[size];
        try (FileInputStream input = new FileInputStream(path)) {
            int offset = 0;
            while (offset < size) {
                int count = input.read(bytes, offset, size - offset);
                if (count < 0) throw new IOException("HuxerUI native output reference is incomplete");
                offset += count;
            }
            if (input.read() != -1) throw new IOException("HuxerUI native output reference has a different extent");
        }
        return bytes;
    }

    private static void writeDiagnostic(File directory, String name, byte[] expected, Bitmap actual)
            throws IOException {
        Bitmap reference = Bitmap.createBitmap(WIDTH, HEIGHT, Bitmap.Config.ARGB_8888);
        try {
            reference.copyPixelsFromBuffer(ByteBuffer.wrap(expected));
            try (FileOutputStream output = new FileOutputStream(new File(directory, name + "-reference.png"))) {
                reference.compress(Bitmap.CompressFormat.PNG, 100, output);
            }
            try (FileOutputStream output = new FileOutputStream(new File(directory, name + "-candidate.png"))) {
                actual.compress(Bitmap.CompressFormat.PNG, 100, output);
            }
        } finally {
            reference.recycle();
        }
    }
}
