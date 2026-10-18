package org.huxerui;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffXfermode;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.RenderEffect;
import android.graphics.RenderNode;
import android.graphics.Shader;
import android.os.Trace;

import java.util.ArrayList;

/** Bounded API 33 Fast blur backend; recordings publish only after the complete scene succeeds. */
final class HuxerUINativeBlur implements AutoCloseable {
    private final ArrayList<RenderNode> nodes = new ArrayList<>();
    private final ArrayList<Surface> surfaces = new ArrayList<>();
    private final ArrayList<Surface> pool = new ArrayList<>();
    private final ArrayList<HuxerUINativeOptics> opticalSlots = new ArrayList<>();
    private int nextOptics;
    static final long FILTER_BUDGET_BYTES = 64L * 1024 * 1024;
    private static final long TARGET_BUDGET_BYTES = 256L * 1024 * 1024;
    private long filterBytes;
    private boolean published;
    private int nextNode;

    static final class Unavailable extends RuntimeException {
        final int reason;
        Unavailable(int reason, String message) {
            super(message, null, false, false);
            this.reason = reason;
        }
    }

    long retainedFilterBytes() { return filterBytes; }
    int retainedNodeCount() { return nodes.size(); }
    int retainedOpticalSlots() { return opticalSlots.size(); }
    int width;
    int height;
    private boolean active;

    boolean active() { return active; }

    void beginFrame(int width, int height) {
        if (active) throw new IllegalStateException("HuxerUI native blur frame is already active");
        this.width = width;
        this.height = height;
        if (width <= 0 || height <= 0 || (long) width * height > 16L * 1024 * 1024)
            throw new Unavailable(5, "HuxerUI native blur viewport exceeds the capture budget");
        nextNode = 0;
        nextOptics = 0;
        filterBytes = 0;
        published = false;
        active = true;
    }

    private RenderNode node(String name, int left, int top, int width, int height) {
        if (nextNode >= 512) throw new Unavailable(2, "HuxerUI native blur recording budget exceeded");
        if (nextNode == nodes.size()) nodes.add(new RenderNode(name));
        RenderNode node = nodes.get(nextNode++);
        node.setPosition(left, top, left + width, top + height);
        node.setClipToBounds(true);
        node.setRenderEffect(null);
        node.setUseCompositingLayer(false, null);
        return node;
    }

    Canvas begin(Canvas parent) {
        if ((surfaces.size() + 1L) * width * height * 4 > TARGET_BUDGET_BYTES)
            throw new Unavailable(3, "HuxerUI native blur isolation targets exceed the capture budget");
        Surface surface = pool.isEmpty() ? new Surface() : pool.remove(pool.size() - 1);
        surface.proxy.restoreToCount(1);
        surface.proxy.setMatrix(new Matrix());
        parent.getMatrix(surface.base);
        surface.proxy.setMatrix(surface.base);
        surface.operations.clear();
        surface.prefix = null;
        surfaces.add(surface);
        surface.restart();
        if (surfaces.size() == 1) surface.canvas.drawColor(0xFFF7F8FA);
        return surface.proxy;
    }

    private Surface surface(Canvas proxy) {
        for (Surface surface : surfaces) if (surface.proxy == proxy) return surface;
        throw new IllegalStateException("HuxerUI native blur recording is missing");
    }

    Canvas drawing(Canvas proxy) { return surface(proxy).canvas; }

    void pushClip(Canvas proxy, Path path) {
        Surface surface = surface(proxy);
        if (surface.operations.size() >= 64)
            throw new Unavailable(2, "HuxerUI native blur state budget exceeded");
        Operation operation = new Operation(null, new Path(path));
        surface.operations.add(operation);
        operation.apply(surface.canvas);
    }

    void pushTransform(Canvas proxy, Matrix transform) {
        Surface surface = surface(proxy);
        if (surface.operations.size() >= 64)
            throw new Unavailable(2, "HuxerUI native blur state budget exceeded");
        Operation operation = new Operation(new Matrix(transform), null);
        surface.operations.add(operation);
        operation.apply(surface.canvas);
    }

    void pop(Canvas proxy) {
        Surface surface = surface(proxy);
        if (surface.operations.isEmpty()) throw new IllegalStateException("HuxerUI native blur state is unbalanced");
        surface.operations.remove(surface.operations.size() - 1);
        surface.canvas.restore();
    }

    void blur(Canvas proxy, float radius, float[] shapes) {
        filter(proxy, new float[]{radius, 0, 12, 0, -0.5F, -1, 1, 1, 0}, shapes);
    }

    void filter(Canvas proxy, float[] effect, float[] shapes) {
        boolean optical = effect[1] != 0 || effect[3] != 0 || effect[6] != 1 || effect[7] != 1 || effect[8] != 0;
        int count = shapes.length / 8;
        if (count < 1 || count > 64 || shapes.length % 8 != 0
                || (optical && android.os.Build.VERSION.SDK_INT < 35))
            throw new Unavailable(6, "HuxerUI native optical material is unavailable");
        float radius = effect[0];
        if (radius == 0 && effect[1] == 0 && effect[3] == 0 && effect[6] == 1 && effect[7] == 1) return;
        Trace.beginSection(optical ? "HuxerUI Native Optics Record" : "HuxerUI Native Blur Record");
        try {
            Surface surface = surface(proxy);
            Path path = new Path();
            for (int index = 0; index < shapes.length; index += 8) {
                path.addRoundRect(new RectF(shapes[index], shapes[index + 1], shapes[index] + shapes[index + 2],
                        shapes[index + 1] + shapes[index + 3]), new float[] {
                        shapes[index + 4], shapes[index + 4], shapes[index + 5], shapes[index + 5],
                        shapes[index + 6], shapes[index + 6], shapes[index + 7], shapes[index + 7]}, Path.Direction.CW);
            }
            RectF bounds = new RectF();
            path.computeBounds(bounds, true);
            if (bounds.isEmpty() || (radius == 0 && !optical)) return;
            Matrix matrix = new Matrix();
            proxy.getMatrix(matrix);
            float[] values = new float[9];
            matrix.getValues(values);
            // Device-axis support must swap under a quarter turn with nonuniform scale.
            float sx = (float) Math.hypot(values[0], values[1]);
            float sy = (float) Math.hypot(values[3], values[4]);
            float tolerance = 0.00001F * Math.max(sx, sy);
            if (!(sx > 0 && sy > 0) || !Float.isFinite(sx) || !Float.isFinite(sy))
                throw new Unavailable(4, "HuxerUI native blur mapping is unavailable");
            boolean axis = (Math.abs(values[1]) <= tolerance && Math.abs(values[3]) <= tolerance)
                    || (Math.abs(values[0]) <= tolerance && Math.abs(values[4]) <= tolerance);
            boolean orthogonal = Math.abs(values[0] * values[3] + values[1] * values[4]) <= tolerance * Math.max(sx, sy)
                    && Math.abs(sx - sy) <= tolerance;
            if (radius > 0 && !axis && !orthogonal)
                throw new Unavailable(4, "HuxerUI native blur requires axis-aligned or uniform orthogonal mapping");
            if (optical) {
                bounds.inset(-effect[8], -effect[8]);
                Matrix inverse = new Matrix();
                if (!matrix.invert(inverse))
                    throw new Unavailable(4, "HuxerUI native optics requires an invertible mapping");
                float[] backward = new float[9];
                inverse.getValues(backward);
                float pixelSize = Math.max((float) Math.hypot(backward[0], backward[3]),
                        (float) Math.hypot(backward[1], backward[4]));
                bounds.inset(-pixelSize * 0.5F, -pixelSize * 0.5F);
            }
            matrix.mapRect(bounds);
            if (!Float.isFinite(bounds.left) || !Float.isFinite(bounds.top)
                    || !Float.isFinite(bounds.right) || !Float.isFinite(bounds.bottom))
                throw new Unavailable(4, "HuxerUI native blur bounds are unavailable");
            Rect clip = new Rect();
            if (!surface.canvas.getClipBounds(clip)) return;
            RectF visible = new RectF(clip);
            matrix.mapRect(visible);
            visible.inset(-2, -2);
            if (!bounds.intersect(visible) || !bounds.intersect(0, 0, width, height)) return;
            float rx = radius * sx;
            float ry = radius * sy;
            if (!Float.isFinite(rx) || !Float.isFinite(ry))
                throw new Unavailable(4, "HuxerUI native blur support is unavailable");
            if (radius > 0 && !System.getProperty("org.huxerui.experimental.nativeBlur", "matched").equals("nominal")) {
                // Three integer box kernels have variance r * (r + 1); Android converts radius to sigma.
                rx = matchedRadius(rx);
                ry = matchedRadius(ry);
            }
            float haloX = effect[1] * sx + Math.max(radius * sx + 3, radius > 0 ? 3 * (rx * 0.577350269F + 0.5F) + 3 : 3);
            float haloY = effect[1] * sy + Math.max(radius * sy + 3, radius > 0 ? 3 * (ry * 0.577350269F + 0.5F) + 3 : 3);
            if (!Float.isFinite(haloX) || !Float.isFinite(haloY))
                throw new Unavailable(4, "HuxerUI native optical support is unavailable");
            int left = Math.max(0, (int) Math.floor(bounds.left - haloX));
            int top = Math.max(0, (int) Math.floor(bounds.top - haloY));
            int right = Math.min(width, (int) Math.ceil(bounds.right + haloX));
            int bottom = Math.min(height, (int) Math.ceil(bounds.bottom + haloY));
            if (right <= left || bottom <= top) return;
            long bytes = (long) (right - left) * (bottom - top) * 4;
            if (bytes > FILTER_BUDGET_BYTES - filterBytes)
                throw new Unavailable(1, "HuxerUI native blur filters exceed the capture budget");
            filterBytes += bytes;
            RenderNode prefix = surface.snapshot();
            RenderNode filtered = node("HuxerUI native blur crop", left, top, right - left, bottom - top);
            Canvas crop = filtered.beginRecording(right - left, bottom - top);
            crop.translate(-left, -top);
            crop.drawRenderNode(prefix);
            filtered.endRecording();
            RenderEffect filter = radius > 0 ? RenderEffect.createBlurEffect(rx, ry, Shader.TileMode.CLAMP) : null;
            HuxerUINativeOptics optics = null;
            if (optical) {
                boolean zero_blur = radius == 0;
                if (nextOptics == opticalSlots.size()) opticalSlots.add(new HuxerUINativeOptics(count, zero_blur));
                else {
                    HuxerUINativeOptics previous = opticalSlots.get(nextOptics);
                    if (previous.capacity < count || previous.zero_blur != zero_blur) {
                        opticalSlots.set(nextOptics,
                                new HuxerUINativeOptics(Math.max(count, previous.capacity), zero_blur));
                    }
                }
                optics = opticalSlots.get(nextOptics++);
                filter = optics.configure(matrix, effect, shapes, left, top, right - left, bottom - top, filter);
            }
            filtered.setRenderEffect(filter);
            Paint replace = new Paint();
            replace.setXfermode(new PorterDuffXfermode(PorterDuff.Mode.SRC));
            filtered.setUseCompositingLayer(true, replace);
            RenderNode output = node("HuxerUI native blur replacement", 0, 0, width, height);
            Canvas result = output.beginRecording(width, height);
            result.drawRenderNode(prefix);
            surface.applyState(result);
            if (optics == null) result.clipPath(path);
            result.setMatrix(new Matrix());
            if (optics != null) optics.clip(result);
            result.drawRenderNode(filtered);
            result.restoreToCount(1);
            output.endRecording();
            surface.prefix = output;
            surface.restart();
        } finally {
            Trace.endSection();
        }
    }

    private static float matchedRadius(float support) {
        double boxRadius = Math.ceil(support / 3.0);
        return (float) ((Math.sqrt(boxRadius * (boxRadius + 1)) - 0.5) / 0.577350269);
    }

    void end(Canvas parent, Canvas proxy, float opacity) {
        Surface child = surface(proxy);
        RenderNode result = child.snapshot();
        surfaces.remove(child);
        Canvas destination = surfaces.isEmpty() ? parent : surface(parent).canvas;
        int saved = destination.save();
        destination.setMatrix(new Matrix());
        if (opacity < 1) destination.saveLayerAlpha(null, Math.round(opacity * 255));
        try {
            destination.drawRenderNode(result);
        } finally {
            destination.restoreToCount(saved);
        }
        if (surfaces.isEmpty()) published = true;
        recycleSurface(child);
    }

    private void recycleSurface(Surface surface) {
        surface.operations.clear();
        surface.prefix = null;
        surface.segment = null;
        if (pool.size() < 4) pool.add(surface);
        else surface.bitmap.recycle();
    }

    private void retireNodes(int first) {
        for (int index = first; index < nodes.size(); ++index) {
            RenderNode node = nodes.get(index);
            node.setRenderEffect(null);
            node.setUseCompositingLayer(false, null);
            node.discardDisplayList();
        }
        nodes.subList(first, nodes.size()).clear();
    }

    void endFrame() {
        for (Surface surface : surfaces) {
            surface.finish();
            recycleSurface(surface);
        }
        surfaces.clear();
        // Unsubmitted attempts and unused slots cannot retain an old frame's filter layers.
        retireNodes(active && published ? nextNode : 0);
        opticalSlots.subList(active && published ? nextOptics : 0, opticalSlots.size()).clear();
        if (!active || !published) filterBytes = 0;
        active = false;
    }

    @Override
    public void close() {
        endFrame();
        retireNodes(0);
        opticalSlots.clear();
        filterBytes = 0;
        for (Surface surface : pool) surface.bitmap.recycle();
        pool.clear();
    }

    private final class Surface {
        // The proxy carries JNI's matrix/save stack; pixels are recorded into full-size RenderNodes.
        final Bitmap bitmap = Bitmap.createBitmap(1, 1, Bitmap.Config.ARGB_8888);
        final Canvas proxy = new Canvas(bitmap);
        final Matrix base = new Matrix();
        final ArrayList<Operation> operations = new ArrayList<>();
        Canvas canvas;
        RenderNode segment;
        RenderNode prefix;

        void applyState(Canvas canvas) {
            canvas.setMatrix(base);
            for (Operation operation : operations) operation.apply(canvas);
        }

        void restart() {
            segment = node("HuxerUI native ordinary segment", 0, 0, width, height);
            canvas = segment.beginRecording(width, height);
            applyState(canvas);
        }

        void finish() {
            if (canvas == null) return;
            canvas.restoreToCount(1);
            segment.endRecording();
            canvas = null;
        }

        RenderNode snapshot() {
            finish();
            RenderNode result = node("HuxerUI native prefix", 0, 0, width, height);
            Canvas target = result.beginRecording(width, height);
            if (prefix != null) target.drawRenderNode(prefix);
            target.drawRenderNode(segment);
            result.endRecording();
            return result;
        }
    }

    private static final class Operation {
        final Matrix transform;
        final Path clip;
        Operation(Matrix transform, Path clip) { this.transform = transform; this.clip = clip; }
        void apply(Canvas canvas) {
            canvas.save();
            if (transform != null) canvas.concat(transform);
            else canvas.clipPath(clip);
        }
    }
}
