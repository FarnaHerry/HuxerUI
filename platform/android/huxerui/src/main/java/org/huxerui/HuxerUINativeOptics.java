package org.huxerui;

import android.graphics.Canvas;
import android.graphics.Matrix;
import android.graphics.RenderEffect;
import android.graphics.RuntimeShader;

/** One retained API 35 optical slot; uniforms use logical geometry and device-pixel sampling. */
final class HuxerUINativeOptics {
    private static final String GEOMETRY =
            "uniform float4 rectangles[HUXERUI_CAPACITY];\n"
            + "uniform float4 corners[HUXERUI_CAPACITY];\n"
            + "uniform int shapeCount;\n"
            + "uniform float fusionRadius;\n"
            + "uniform float3 inverseX;\n"
            + "uniform float3 inverseY;\n"
            + "uniform float pixelSize;\n"
            + "float2 localPoint(float2 p) {\n"
            + "  return float2(dot(inverseX, float3(p, 1)), dot(inverseY, float3(p, 1)));\n"
            + "}\n"
            + "float shapeDistance(float2 p, float4 rect, float4 radii) {\n"
            + "  float2 halfSize = rect.zw * 0.5;\n"
            + "  float2 center = rect.xy + halfSize;\n"
            + "  float2 q = abs(p - center) - halfSize;\n"
            + "  float d = length(max(q, float2(0))) + min(max(q.x, q.y), 0);\n"
            + "  if (radii.x > 0 && p.x < rect.x + radii.x && p.y < rect.y + radii.x)\n"
            + "    d = max(d, distance(p, rect.xy + radii.xx) - radii.x);\n"
            + "  if (radii.y > 0 && p.x > rect.x + rect.z - radii.y && p.y < rect.y + radii.y)\n"
            + "    d = max(d, distance(p, rect.xy + float2(rect.z - radii.y, radii.y)) - radii.y);\n"
            + "  if (radii.z > 0 && p.x > rect.x + rect.z - radii.z && p.y > rect.y + rect.w - radii.z)\n"
            + "    d = max(d, distance(p, rect.xy + rect.zw - radii.zz) - radii.z);\n"
            + "  if (radii.w > 0 && p.x < rect.x + radii.w && p.y > rect.y + rect.w - radii.w)\n"
            + "    d = max(d, distance(p, rect.xy + float2(radii.w, rect.w - radii.w)) - radii.w);\n"
            + "  return d;\n"
            + "}\n"
            + "float mergeDistance(float separation, float next) {\n"
            + "  if (fusionRadius > 0 && separation < 1.0e19) {\n"
            + "    float h = max(fusionRadius - abs(separation - next), 0) / fusionRadius;\n"
            + "    return min(separation, next) - h * h * fusionRadius * 0.25;\n"
            + "  }\n"
            + "  return min(separation, next);\n"
            + "}\n"
            + "float surfaceDistance(float2 p) {\n"
            + "  float separation = 1.0e20;\n"
            + "HUXERUI_UNION_BODY"
            + "  return separation;\n"
            + "}\n";
    private static final String OPTICS = GEOMETRY
            + "uniform shader content;\n"
            + "uniform float2 origin;\n"
            + "uniform float4 sourceBounds;\n"
            + "uniform float3 forwardX;\n"
            + "uniform float3 forwardY;\n"
            + "uniform float4 material;\n"
            + "uniform float2 light;\n"
            + "uniform float2 colorScale;\n"
            + "half4 main(float2 p) {\n"
            + "  // The effect target is device-aligned; stabilize normals at exact pixel centers.\n"
            + "  float2 device = floor(p + origin) + 0.5;\n"
            + "  float2 raster = device - origin;\n"
            + "  float2 local = localPoint(device);\n"
            + "  float d = surfaceDistance(local);\n"
            + "HUXERUI_OUTSIDE_CULL"
            + "  float edge = clamp(1 + min(d, 0) / material.y, 0, 1);\n"
            + "  float lens = edge * edge;\n"
            + "  float2 normal = float2(0);\n"
            + "  if (lens > 0 && (material.x > 0 || material.z > 0)) {\n"
            + "    float e = max(pixelSize * 0.5, 0.01);\n"
            + "    normal = float2(surfaceDistance(local + float2(e, 0)) - surfaceDistance(local - float2(e, 0)),\n"
            + "        surfaceDistance(local + float2(0, e)) - surfaceDistance(local - float2(0, e)));\n"
            + "    float size = length(normal);\n"
            + "    // Match the portable processor's coordinate-relative eight-epsilon floor.\n"
            + "    float normalFloor = max(0.00001,\n"
            + "        max(max(abs(local.x), abs(local.y)), max(abs(d), 1)) * 0.00000095367431640625);\n"
            + "    normal = size > normalFloor ? normal / size : float2(0);\n"
            + "  }\n"
            + "  float2 displaced = local - normal * (material.x * lens);\n"
            + "  float2 samplePoint = material.x > 0 ? float2(dot(forwardX, float3(displaced, 1)),\n"
            + "      dot(forwardY, float3(displaced, 1))) - origin : raster;\n"
            + "  float4 color = float4(content.eval(clamp(samplePoint, sourceBounds.xy, sourceBounds.zw)));\n"
            + "  float luminance = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));\n"
            + "  float3 rgb = clamp((luminance + (color.rgb - luminance) * colorScale.x) * colorScale.y,\n"
            + "      float3(0), float3(color.a));\n"
            + "  float lighting = dot(normal, light) * material.z * lens;\n"
            + "  rgb = lighting >= 0 ? mix(rgb, float3(color.a), lighting) : rgb * (1 + lighting);\n"
            + "  return half4(clamp(rgb, float3(0), float3(color.a)), color.a);\n"
            + "}\n";
    private static final String MASK = GEOMETRY
            + "half4 main(float2 p) {\n"
            + "  return half4(0, 0, 0, clamp(0.5 - surfaceDistance(localPoint(p)) / pixelSize, 0, 1));\n"
            + "}\n";
    private static final String SCALAR_UNION =
            "  for (int index = 0; index < HUXERUI_CAPACITY; ++index) {\n"
            + "    if (index >= shapeCount) break;\n"
            + "    float4 rect = rectangles[index];\n"
            + "    if (rect.z <= 0 || rect.w <= 0) continue;\n"
            + "    separation = mergeDistance(separation, shapeDistance(p, rect, corners[index]));\n"
            + "  }\n";

    private final RuntimeShader optics;
    private final RuntimeShader mask;
    final int capacity;
    final boolean zero_blur;
    private final float[] rectangles;
    private final float[] corners;

    HuxerUINativeOptics(int count, boolean zero_blur) {
        this.zero_blur = zero_blur;
        int size = 1;
        while (size < count) size *= 2;
        capacity = size;
        String bound = Integer.toString(capacity);
        String union_body = SCALAR_UNION;
        if (zero_blur) {
            // Bounded groups avoid scalar-loop normal discrepancies without full-unroll compilation growth.
            int stride = Math.min(capacity, 4);
            StringBuilder union = new StringBuilder("  for (int start = 0; start < " + capacity
                    + "; start += " + stride + ") {\n");
            for (int index = 0; index < stride; ++index) {
                String at = "start + " + index;
                union.append("    if (").append(at).append(" < shapeCount) {\n")
                        .append("      float4 rect = rectangles[").append(at).append("];\n")
                        .append("      if (rect.z > 0 && rect.w > 0)\n")
                        .append("        separation = mergeDistance(separation, shapeDistance(p, rect, corners[")
                        .append(at).append("]));\n    }\n");
            }
            union.append("  }\n");
            union_body = union.toString();
        }
        // Keep single-shape outside culling unchanged; adding that branch regressed device measurements.
        String outsideCull = capacity > 1 ? "  if (d > pixelSize * 2) return half4(0);\n" : "";
        optics = new RuntimeShader(OPTICS.replace("HUXERUI_UNION_BODY", union_body).replace("HUXERUI_CAPACITY", bound)
                .replace("HUXERUI_OUTSIDE_CULL", outsideCull));
        mask = new RuntimeShader(MASK.replace("HUXERUI_UNION_BODY", union_body).replace("HUXERUI_CAPACITY", bound));
        rectangles = new float[capacity * 4];
        corners = new float[capacity * 4];
    }

    RenderEffect configure(Matrix matrix, float[] effect, float[] shape, int left, int top, int width, int height,
            RenderEffect blur) {
        Matrix inverse = new Matrix();
        if (!matrix.invert(inverse))
            throw new HuxerUINativeBlur.Unavailable(4, "HuxerUI native optics requires an invertible mapping");
        float[] forward = new float[9], backward = new float[9];
        matrix.getValues(forward);
        inverse.getValues(backward);
        for (int index = 0; index < 9; ++index) {
            if (!Float.isFinite(forward[index]) || !Float.isFinite(backward[index]))
                throw new HuxerUINativeBlur.Unavailable(4, "HuxerUI native optical mapping is unavailable");
        }
        float pixelSize = Math.max((float) Math.hypot(backward[0], backward[3]),
                (float) Math.hypot(backward[1], backward[4]));
        if (!(pixelSize > 0) || !Float.isFinite(pixelSize))
            throw new HuxerUINativeBlur.Unavailable(4, "HuxerUI native optics scale is unavailable");
        int count = shape.length / 8;
        for (int index = 0; index < count; ++index) {
            System.arraycopy(shape, index * 8, rectangles, index * 4, 4);
            System.arraycopy(shape, index * 8 + 4, corners, index * 4, 4);
        }
        for (RuntimeShader shader : new RuntimeShader[]{optics, mask}) {
            shader.setFloatUniform("rectangles", rectangles);
            shader.setFloatUniform("corners", corners);
            shader.setIntUniform("shapeCount", count);
            shader.setFloatUniform("fusionRadius", effect[8]);
            shader.setFloatUniform("inverseX", backward[0], backward[1], backward[2]);
            shader.setFloatUniform("inverseY", backward[3], backward[4], backward[5]);
            shader.setFloatUniform("pixelSize", pixelSize);
        }
        float lightSize = (float) Math.hypot(effect[4], effect[5]);
        optics.setFloatUniform("origin", left, top);
        optics.setFloatUniform("sourceBounds", 0.5F, 0.5F, width - 0.5F, height - 0.5F);
        optics.setFloatUniform("forwardX", forward[0], forward[1], forward[2]);
        optics.setFloatUniform("forwardY", forward[3], forward[4], forward[5]);
        optics.setFloatUniform("material", effect[1], effect[2], effect[3], 0);
        optics.setFloatUniform("light", effect[4] / lightSize, effect[5] / lightSize);
        optics.setFloatUniform("colorScale", effect[6], effect[7]);
        RenderEffect result = RenderEffect.createRuntimeShaderEffect(optics, "content");
        return blur == null ? result : RenderEffect.createChainEffect(result, blur);
    }

    void clip(Canvas canvas) { canvas.clipShader(mask); }
}
