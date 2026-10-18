package org.huxerui;

import android.app.Activity;
import android.app.Instrumentation;
import android.os.Bundle;
import android.os.Build;
import android.graphics.Paint;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Typeface;
import android.text.Layout;
import android.text.TextPaint;

import java.lang.reflect.Method;

/** Android-runtime regression checks; the actual host View requires a matching native HuxerUI library. */
public final class HuxerUIRuntimeTest extends Instrumentation {
    @Override
    public void onCreate(Bundle arguments) {
        super.onCreate(arguments);
        start();
    }

    @Override
    public void onStart() {
        assertions = 0;
        Bundle status = new Bundle();
        status.putString("class", getClass().getName());
        status.putString("test", "androidRuntime");
        status.putInt("numtests", 1);
        status.putInt("current", 1);
        sendStatus(1, status);
        Bundle result = new Bundle();
        try {
            verifyHostFocusHighlight();
            verifyBackdropCanvases();
            verifyGradientBrushes();
            verifyGeometry();
            int notificationAssertions = HuxerUILocalNotificationTest.verify(getTargetContext());
            sendStatus(0, status);
            result.putString("stream",
                    "HuxerUI Android runtime: " + (assertions + notificationAssertions) + " assertions passed\n");
        } catch (Throwable error) {
            String stack = android.util.Log.getStackTraceString(error);
            status.putString("stack", stack);
            sendStatus(-2, status);
            result.putString("stream", stack);
        }
        finish(Activity.RESULT_OK, result);
    }

    private static int assertions;

    private void verifyHostFocusHighlight() {
        final boolean[] state = new boolean[3];
        final Throwable[] failure = new Throwable[1];
        runOnMainSync(() -> {
            try {
                HuxerUIView host = new HuxerUIView(getTargetContext());
                state[0] = host.isFocusable();
                state[1] = host.isFocusableInTouchMode();
                state[2] = Build.VERSION.SDK_INT < Build.VERSION_CODES.O
                        || !host.getDefaultFocusHighlightEnabled();
            } catch (Throwable error) {
                // Report main-thread setup errors through Instrumentation rather than terminating its process.
                failure[0] = error;
            }
        });
        if (failure[0] != null) {
            throw new AssertionError("Host focus checks require matching libhuxerui.so and its runtime dependencies", failure[0]);
        }
        check(state[0], "The host remains focusable for keyboard input");
        check(state[1], "The host remains focusable in touch mode for IME input");
        check(state[2], "Android default focus highlighting must not tint the whole host");
    }

    private static void check(boolean value, String message) {
        ++assertions;
        if (!value) {
            throw new AssertionError(message);
        }
    }

    private static Method hostMethod(String name, Class<?>... parameters) throws Exception {
        Method method = HuxerUIView.class.getDeclaredMethod(name, parameters);
        method.setAccessible(true);
        return method;
    }

    private void verifyGradientBrushes() {
        final Throwable[] failure = new Throwable[1];
        runOnMainSync(() -> {
            Bitmap target = Bitmap.createBitmap(8, 8, Bitmap.Config.ARGB_8888);
            try {
                HuxerUIView host = new HuxerUIView(getTargetContext());
                Method draw = hostMethod("drawBrushRect", Canvas.class, float.class, float.class, float.class,
                        float.class, int.class, int.class, float[].class, float[].class, int[].class, float.class);
                Canvas canvas = new Canvas(target);
                float[] geometry = {0.0F, 0.0F, 1.0F, 0.0F, 8.0F, 0.0F, 0.0F, 8.0F, 0.0F, 0.0F};
                float[] stops = {0.0F, 1.0F};
                draw.invoke(host, canvas, 0.0F, 0.0F, 8.0F, 8.0F, 2, 0, geometry, stops,
                        new int[] {0xFFFF0000, 0xFF0000FF}, 0.0F);
                check((target.getPixel(4, 4) >>> 24) == 255, "Gradient alpha comes from its stops");
                check(((target.getPixel(0, 4) >>> 16) & 255) > (target.getPixel(0, 4) & 255),
                        "Linear gradient retains its starting color");
                check((target.getPixel(7, 4) & 255) > ((target.getPixel(7, 4) >>> 16) & 255),
                        "Linear gradient retains its ending color");
                target.eraseColor(0);
                geometry[0] = 0.5F;
                geometry[1] = 0.5F;
                geometry[2] = 0.5F;
                geometry[3] = 0.5F;
                draw.invoke(host, canvas, 0.0F, 0.0F, 8.0F, 8.0F, 3, 0, geometry, stops,
                        new int[] {0x40FF0000, 0x400000FF}, 0.0F);
                check((target.getPixel(4, 4) >>> 24) == 64, "Radial gradient preserves translucent stop alpha");
            } catch (Throwable error) {
                failure[0] = error;
            } finally {
                target.recycle();
            }
        });
        if (failure[0] != null) {
            throw new AssertionError("Android gradient brush checks failed", failure[0]);
        }
    }

    private void verifyBackdropCanvases() {
        final Throwable[] failure = new Throwable[1];
        runOnMainSync(() -> {
            Bitmap target = Bitmap.createBitmap(8, 8, Bitmap.Config.ARGB_8888);
            HuxerUIView host = new HuxerUIView(getTargetContext());
            try {
                Method begin = hostMethod("beginBackdropCanvas", Canvas.class);
                Method end = hostMethod("endBackdropCanvas", Canvas.class, Canvas.class, float.class);
                Method capture = hostMethod("backdropPixels", Canvas.class, int.class, int.class, int.class, int.class);
                Method replace = hostMethod(
                        "drawBackdropPixels", Canvas.class, int[].class, int.class, int.class, int.class, int.class);
                Canvas parent = new Canvas(target);
                Canvas root = (Canvas) begin.invoke(host, parent);
                check(((int[]) capture.invoke(host, root, 0, 0, 8, 8))[0] == 0xFFF7F8FA,
                        "Glass capture starts with this frame's background");
                hostMethod("pushClip", Canvas.class, float.class, float.class, float.class, float.class, float.class)
                        .invoke(host, root, 0.0F, 0.0F, 8.0F, 8.0F, 4.0F);
                Canvas group = (Canvas) begin.invoke(host, root);
                check(((int[]) capture.invoke(host, group, 0, 0, 8, 8))[4 * 8 + 4] == 0,
                        "An opacity group captures its own transparent prefix");
                int[] translucent = new int[64];
                java.util.Arrays.fill(translucent, 0x40FF0000);
                replace.invoke(host, group, translucent, 8, 8, 0, 0);
                replace.invoke(host, group, translucent, 8, 8, 0, 0);
                int[] pixels = (int[]) capture.invoke(host, group, 0, 0, 8, 8);
                check((pixels[4 * 8 + 4] >>> 24) == 64, "Replacing the optical prefix preserves its alpha");
                check((pixels[0] >>> 24) == 64, "An isolated prefix defers inherited clips until composition");
                end.invoke(host, root, group, 0.5F);
                int composed = ((int[]) capture.invoke(host, root, 0, 0, 8, 8))[4 * 8 + 4];
                check((composed >>> 24) == 255, "Group opacity composites into the opaque background once");
                check(((composed >>> 8) & 255) > 200 && ((composed >>> 8) & 255) < 225,
                        "Glass alpha and group opacity multiply only at group composition");
                hostMethod("popClip", Canvas.class).invoke(host, root);
                end.invoke(host, parent, root, 1.0F);
                check(target.getPixel(0, 0) == 0xFFF7F8FA,
                        "The glass clip leaves the surrounding background unchanged");
                Canvas next = (Canvas) begin.invoke(host, parent);
                check(((int[]) capture.invoke(host, next, 0, 0, 8, 8))[4 * 8 + 4] == 0xFFF7F8FA,
                        "A new glass frame excludes the previous frame's optical output");
                int[] crop = {0xFF0000FF, 0xFF0000FF, 0xFF0000FF, 0xFF0000FF};
                replace.invoke(host, next, crop, 2, 2, 3, 3);
                check(((int[]) capture.invoke(host, next, 3, 3, 2, 2))[3] == 0xFF0000FF,
                        "A cropped optical output uses its capture origin");
                check(((int[]) capture.invoke(host, next, 0, 0, 8, 8))[0] == 0xFFF7F8FA,
                        "Replacing a crop preserves pixels outside its bounds");
                next.scale(2.0F, 2.0F);
                float[] inverse = (float[]) hostMethod("backdropTransform", Canvas.class).invoke(host, next);
                check(inverse[2] == 0.5F && inverse[5] == 0.5F,
                        "Capture converts pixel edges into local logical coordinates");
            } catch (Throwable error) {
                failure[0] = error;
            } finally {
                try {
                    hostMethod("releaseBackdropCanvases").invoke(host);
                } catch (Throwable error) {
                    failure[0] = error;
                }
                target.recycle();
            }
        });
        if (failure[0] != null) {
            throw new AssertionError("Android backdrop Canvas checks failed", failure[0]);
        }
    }

    private static HuxerUITextLayout layout(String text, float width, Layout.Alignment alignment, boolean wrap,
                                            int direction) {
        Typeface typeface = Typeface.MONOSPACE;
        TextPaint paint = new TextPaint(Paint.ANTI_ALIAS_FLAG);
        paint.setTypeface(typeface);
        paint.setTextSize(20.25F);
        return new HuxerUITextLayout(text, paint, width, alignment, wrap, direction);
    }

    private static float[] caret(HuxerUITextLayout layout, int offset, boolean upstream) throws Exception {
        Method method = HuxerUITextLayout.class.getDeclaredMethod("caret", long.class, boolean.class);
        method.setAccessible(true);
        return (float[]) method.invoke(layout, (long) offset, upstream);
    }

    private static long hit(HuxerUITextLayout layout, float[] caret) throws Exception {
        Method method = HuxerUITextLayout.class.getDeclaredMethod("hitTest", float.class, float.class);
        method.setAccessible(true);
        return (long) method.invoke(layout, caret[0], caret[1] + caret[3] * 0.5F);
    }

    private static float[] range(HuxerUITextLayout layout, int start, int end) throws Exception {
        Method method = HuxerUITextLayout.class.getDeclaredMethod("range", long.class, long.class);
        method.setAccessible(true);
        return (float[]) method.invoke(layout, (long) start, (long) end);
    }

    private static void verifyGeometry() throws Exception {
        for (int direction : new int[] {1, 2}) {
            String text = direction == 1 ? "abcdefghijklm" : "אבגדהוזחטיכלמ";
            for (Layout.Alignment alignment :
                 new Layout.Alignment[] {Layout.Alignment.ALIGN_NORMAL, Layout.Alignment.ALIGN_CENTER,
                                         Layout.Alignment.ALIGN_OPPOSITE}) {
                HuxerUITextLayout layout = layout(text, 62.0F, alignment, true, direction);
                int boundary = 1;
                while (boundary < text.length() && caret(layout, boundary, false)[1] == caret(layout, 0, false)[1]) {
                    ++boundary;
                }
                check(boundary < text.length(), "Expected automatic wrapping");
                float[] before = caret(layout, boundary, true);
                float[] after = caret(layout, boundary, false);
                check(before[1] < after[1], "Upstream must stay on the preceding visual line");
                long encoded = hit(layout, before);
                check(encoded == -(long) boundary - 1L,
                      "Soft-wrap hit must preserve upstream affinity: " + direction + " / " + boundary + " / " + encoded
                              + " / " + before[0]);
            }
        }
        for (String separator : new String[] {"\n", "\r\n"}) {
            HuxerUITextLayout layout =
                    layout("abc" + separator + "xyz", 200.0F, Layout.Alignment.ALIGN_NORMAL, true, 1);
            int offset = 3 + separator.length();
            float[] before = caret(layout, offset, true);
            float[] after = caret(layout, offset, false);
            check(before[0] == after[0] && before[1] == after[1], "Hard breaks must not move the caret backwards");
        }
        String bidi = "abc אבג xyz";
        for (Layout.Alignment alignment : new Layout.Alignment[] {
                     Layout.Alignment.ALIGN_NORMAL, Layout.Alignment.ALIGN_CENTER, Layout.Alignment.ALIGN_OPPOSITE}) {
            HuxerUITextLayout layout = layout(bidi, 300.0F, alignment, false, 1);
            for (int offset = 0; offset <= bidi.length(); ++offset) {
                for (boolean upstream : new boolean[] {false, true}) {
                    float[] position = caret(layout, offset, upstream);
                    long encoded = hit(layout, position);
                    float[] resolved = caret(layout, (int) (encoded < 0 ? -encoded - 1 : encoded), encoded < 0);
                    check(Math.abs(position[0] - resolved[0]) < 0.1F && position[1] == resolved[1],
                          "Bidi caret geometry must round-trip at offset " + offset + " / " + alignment);
                }
            }
            check(range(layout, 2, 5).length >= 8, "Disjoint bidi selection fragments must remain separate");
        }
        HuxerUITextLayout emoji = layout("a\u0301👩‍💻b", 300.0F, Layout.Alignment.ALIGN_NORMAL, false, 1);
        float[] selected = range(emoji, 2, 7);
        check(selected.length >= 4 && selected[2] > 0.0F, "Emoji selection must retain nonempty geometry");
        HuxerUITextLayout fractional = layout("abc", 300.0F, Layout.Alignment.ALIGN_NORMAL, false, 1);
        float[] fragment = range(fractional, 0, 1);
        float[] leading = caret(fractional, 0, false);
        float[] trailing = caret(fractional, 1, true);
        check(fragment.length == 4 && Math.abs(fragment[0] - leading[0]) <= 1.0F / 64.0F
                      && Math.abs(fragment[0] + fragment[2] - trailing[0]) <= 1.0F / 64.0F,
              "Region conversion must preserve subpixel selection edges");
        StringBuilder longText = new StringBuilder();
        for (int line = 0; line < 1000; ++line) {
            longText.append("abc\n");
        }
        HuxerUITextLayout longLayout = layout(longText.toString(), 300.0F, Layout.Alignment.ALIGN_NORMAL, false, 1);
        float[] tail = range(longLayout, longText.length() - 4, longText.length() - 1);
        check(tail.length >= 4 && tail[1] > 10000.0F, "Selection must preserve the origin of distant lines");
    }
}
