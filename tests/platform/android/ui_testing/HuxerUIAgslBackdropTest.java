package org.huxerui;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorSpace;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffXfermode;
import android.graphics.RenderEffect;
import android.graphics.RenderNode;
import android.graphics.RuntimeShader;
import android.graphics.Shader;
import android.hardware.HardwareBuffer;
import android.media.Image;
import android.util.Log;

/** GPU capability probe; the two-circle material is not a production optical implementation. */
final class HuxerUIAgslBackdropTest {
    private static final String DISTANCE =
            "uniform float fusion;\n"
            + "float distanceToSurface(float2 p) {\n"
            + "  float a = length(p - float2(300, 300)) - 90;\n"
            + "  float b = length(p - float2(500, 300)) - 90;\n"
            + "  float h = fusion > 0 ? max(fusion - abs(a - b), 0) / fusion : 0;\n"
            + "  return min(a, b) - h * h * fusion * 0.25;\n"
            + "}\n";
    private static final String OPTICS = DISTANCE
            + "uniform shader content;\n"
            + "uniform float2 strength;\n"
            + "half4 main(float2 p) {\n"
            + "  float d = distanceToSurface(p);\n"
            + "  float edge = clamp(1 + min(d, 0) / 20, 0, 1);\n"
            + "  float lens = edge * edge;\n"
            + "  float2 n = float2(distanceToSurface(p + float2(0.5, 0))\n"
            + "      - distanceToSurface(p - float2(0.5, 0)),\n"
            + "      distanceToSurface(p + float2(0, 0.5)) - distanceToSurface(p - float2(0, 0.5)));\n"
            + "  n = length(n) > 0.00001 ? normalize(n) : float2(0);\n"
            + "  half4 color = content.eval(p - n * strength.x * lens);\n"
            + "  float light = dot(n, normalize(float2(-0.5, -1))) * strength.y * lens;\n"
            + "  color.rgb = light >= 0 ? mix(color.rgb, half3(color.a), light) : color.rgb * (1 + light);\n"
            + "  return color;\n"
            + "}\n";
    private static final String MASK = DISTANCE
            + "half4 main(float2 p) { return half4(0, 0, 0, clamp(0.5 - distanceToSurface(p), 0, 1)); }";

    static void verify(HuxerUIBackdropRaster raster) {
        RuntimeShader optics = new RuntimeShader(OPTICS);
        RuntimeShader mask = new RuntimeShader(MASK);
        RenderNode source = new RenderNode("HuxerUI AGSL prefix probe");
        RenderNode filtered = new RenderNode("HuxerUI AGSL optical probe");
        source.setPosition(0, 0, 800, 600);
        filtered.setPosition(0, 0, 800, 600);
        Canvas prefix = source.beginRecording(800, 600);
        Paint gradient = new Paint();
        gradient.setShader(new LinearGradient(0, 0, 800, 0, Color.rgb(30, 60, 90),
                Color.rgb(200, 150, 120), Shader.TileMode.CLAMP));
        gradient.setAlpha(128);
        prefix.drawRect(0, 0, 800, 600, gradient);
        Paint stripe = new Paint(); stripe.setColor(Color.argb(128, 255, 255, 255));
        stripe.setXfermode(new PorterDuffXfermode(PorterDuff.Mode.SRC));
        prefix.drawRect(398, 0, 402, 600, stripe);
        source.endRecording();
        Canvas layer = filtered.beginRecording(800, 600);
        layer.drawRenderNode(source);
        filtered.endRecording();
        Paint replace = new Paint(); replace.setXfermode(new PorterDuffXfermode(PorterDuff.Mode.SRC));
        filtered.setUseCompositingLayer(true, replace);
        Bitmap baseline = null;
        int checks = 0;
        try {
            for (float[] material : new float[][]{{0, 0, 0}, {16, 0, 0}, {0, 0.3F, 0}, {16, 0.3F, 64}}) {
                optics.setFloatUniform("fusion", material[2]); mask.setFloatUniform("fusion", material[2]);
                optics.setFloatUniform("strength", material[0], material[1]);
                filtered.setRenderEffect(RenderEffect.createChainEffect(
                        RenderEffect.createRuntimeShaderEffect(optics, "content"),
                        RenderEffect.createBlurEffect(6, 6, Shader.TileMode.CLAMP)));
                RenderNode root = new RenderNode("HuxerUI AGSL masked replacement probe");
                root.setPosition(0, 0, 800, 600);
                Canvas result = root.beginRecording(800, 600);
                result.drawRenderNode(source);
                int saved = result.save();
                result.clipShader(mask);
                result.drawRenderNode(filtered);
                result.restoreToCount(saved);
                Paint foreground = new Paint(); foreground.setColor(Color.YELLOW);
                result.drawRect(290, 290, 310, 310, foreground);
                root.endRecording();
                try (Image image = raster.draw(root, 0, 0); HardwareBuffer buffer = image.getHardwareBuffer()) {
                    Bitmap hardware = Bitmap.wrapHardwareBuffer(buffer, ColorSpace.get(ColorSpace.Named.SRGB));
                    Bitmap pixels = hardware.copy(Bitmap.Config.ARGB_8888, false);
                    hardware.recycle();
                    check(pixels.getPixel(300, 300) == Color.YELLOW, "AGSL leaves foreground sharp"); ++checks;
                    check(Math.abs(Color.alpha(pixels.getPixel(212, 300)) - 128) <= 1,
                            "Shader clip and source replacement preserve partial alpha"); ++checks;
                    if (baseline == null) baseline = pixels.copy(Bitmap.Config.ARGB_8888, false);
                    else {
                        check(pixels.getPixel(50, 50) == baseline.getPixel(50, 50),
                                "SDF clipping preserves outside source pixels"); ++checks;
                        check(pixels.getPixel(214, 300) != baseline.getPixel(214, 300),
                                "AGSL refraction and lighting affect the sampled optical edge"); ++checks;
                    }
                    if (material[2] > 0) {
                        check(pixels.getPixel(400, 300) != baseline.getPixel(400, 300),
                                "Smooth-union shader coverage exposes a fused bridge"); ++checks;
                    }
                    pixels.recycle();
                } finally {
                    root.discardDisplayList();
                }
            }
        } finally {
            if (baseline != null) baseline.recycle();
            filtered.discardDisplayList(); source.discardDisplayList();
        }
        Log.i("HuxerUI", "AGSL blur/refraction/highlight/fusion capability probe: " + checks + " assertions passed");
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError("HuxerUI " + message);
    }
}
