package org.huxerui;

import android.graphics.Canvas;
import android.graphics.HardwareRenderer;
import android.graphics.Matrix;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.RenderNode;
import android.hardware.HardwareBuffer;
import android.hardware.SyncFence;
import android.media.Image;
import android.media.ImageReader;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Trace;

import java.util.ArrayList;
import java.time.Duration;
import java.io.IOException;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;

/** API 33 hardware raster targets. Images stay acquired until the native sampling fence retires. */
final class HuxerUIBackdropRaster implements AutoCloseable {
    private final ImageReader reader;
    private final HardwareRenderer renderer;
    private final HandlerThread delivery = new HandlerThread("HuxerUI raster delivery");
    private final Semaphore available = new Semaphore(0);
    final int width;
    final int height;
    private final RenderNode crop = new RenderNode("HuxerUI raster crop");

    HuxerUIBackdropRaster(int width, int height) {
        this.width = width;
        this.height = height;
        crop.setPosition(0, 0, width, height);
        reader = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 4,
                HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE | HardwareBuffer.USAGE_GPU_COLOR_OUTPUT);
        renderer = new HardwareRenderer();
        try {
            delivery.start();
            reader.setOnImageAvailableListener(ignored -> available.release(), new Handler(delivery.getLooper()));
            renderer.setSurface(reader.getSurface());
            renderer.setOpaque(false);
        } catch (RuntimeException exception) {
            renderer.destroy();
            reader.close();
            delivery.quitSafely();
            throw exception;
        }
    }

    Image draw(RenderNode node, int left, int top) {
        Canvas canvas = crop.beginRecording(width, height);
        canvas.translate(-left, -top);
        canvas.drawRenderNode(node);
        crop.endRecording();
        renderer.setContentRoot(crop);
        available.drainPermits();
        int result;
        Trace.beginSection("HuxerUI HWUI Submit");
        try {
            result = renderer.createRenderRequest()
                             .setVsyncTime(System.nanoTime())
                             .setWaitForPresent(false)
                             .syncAndDraw();
        } finally {
            Trace.endSection();
        }
        if ((result & (HardwareRenderer.SYNC_LOST_SURFACE_REWARD_IF_FOUND | HardwareRenderer.SYNC_CONTEXT_IS_STOPPED))
                != 0) {
            throw new IllegalStateException("HuxerUI Android hardware raster target is unavailable");
        }
        Image image;
        Trace.beginSection("HuxerUI Image Delivery");
        try {
            image = reader.acquireNextImage();
            if (image == null) {
                // FRAME_DROPPED still syncs this content and schedules a later vsync; do not submit duplicates.
                // Delivery runs independently of the blocked UI thread, including deferred frames.
                try {
                    long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(1);
                    while (image == null && System.nanoTime() < deadline) {
                        if (!available.tryAcquire(Math.max(0, deadline - System.nanoTime()), TimeUnit.NANOSECONDS))
                            break;
                        image = reader.acquireNextImage();
                    }
                } catch (InterruptedException exception) {
                    Thread.currentThread().interrupt();
                    throw new IllegalStateException("HuxerUI Android raster delivery was interrupted", exception);
                }
            }
            if (image == null)
                throw new IllegalStateException("HuxerUI Android hardware raster frame is missing: " + result);
        } finally {
            Trace.endSection();
        }
        // Submission does not imply completion. Sampling must wait for the producer's acquire fence.
        Trace.beginSection("HuxerUI Producer Fence");
        try (SyncFence fence = image.getFence()) {
            if (fence.isValid() && !fence.await(Duration.ofSeconds(1))) {
                throw new IllegalStateException("HuxerUI Android raster producer fence did not signal");
            }
        } catch (RuntimeException exception) {
            image.close();
            throw exception;
        } catch (IOException exception) {
            image.close();
            throw new IllegalStateException("HuxerUI Android raster producer fence is unavailable", exception);
        } finally {
            Trace.endSection();
        }
        return image;
    }

    @Override
    public void close() {
        renderer.destroy();
        crop.discardDisplayList();
        reader.close();
        delivery.quitSafely();
    }

    static final class Recording {
        final RenderNode node = new RenderNode("HuxerUI raster segment");
        final ArrayList<Operation> operations = new ArrayList<>();
        final Matrix base = new Matrix();
        Canvas canvas;
        final int width;
        final int height;

        Recording(int width, int height) {
            this.width = width;
            this.height = height;
            node.setPosition(0, 0, width, height);
        }

        void begin(Matrix matrix) {
            base.set(matrix);
            operations.clear();
            restart(matrix);
        }

        void restart(Matrix matrix) {
            canvas = node.beginRecording(width, height);
            canvas.setMatrix(base);
            for (Operation operation : operations) operation.apply(canvas);
            canvas.setMatrix(matrix);
        }

        void pushTransform(Matrix matrix) {
            Operation operation = new Operation(new Matrix(matrix), null);
            operations.add(operation);
            operation.apply(canvas);
        }

        void pushClip(Path path) {
            Operation operation = new Operation(null, new Path(path));
            operations.add(operation);
            operation.apply(canvas);
        }

        void pop() {
            if (operations.isEmpty())
                throw new IllegalStateException("HuxerUI Android raster state is unbalanced");
            operations.remove(operations.size() - 1);
            canvas.restore();
        }

        void finish() {
            canvas.restoreToCount(1);
            node.endRecording();
            canvas = null;
        }

        void discard() {
            if (canvas != null)
                finish();
            node.discardDisplayList();
            operations.clear();
        }
    }

    private static final class Operation {
        final Matrix transform;
        final Path clip;
        Operation(Matrix transform, Path clip) {
            this.transform = transform;
            this.clip = clip;
        }
        void apply(Canvas canvas) {
            canvas.save();
            if (transform != null)
                canvas.concat(transform);
            else
                canvas.clipPath(clip);
        }
    }
}
