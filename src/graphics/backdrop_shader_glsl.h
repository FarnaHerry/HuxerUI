#pragma once

// GLSL sources for GPU backdrop plane execution. One body serves both flavors: the caller prepends
// the version and precision prefix matching its GL context (OpenGL 3.3 core or OpenGL ES 3.0).
// The math mirrors detail::ApplyBackdropEffect exactly: premultiplied BGRA rows are uploaded as
// GL_RGBA, so sampler channel x is logical blue, y green, z red, and w alpha.

namespace huxerui::detail::backdrop_glsl {

inline constexpr const char* vertex_body = R"GLSL(
layout(location = 0) in vec2 position;
uniform vec2 texture_size;
out vec2 device_pixel;
void main() {
  gl_Position = vec4(position, 0.0, 1.0);
  device_pixel = (position * 0.5 + 0.5) * texture_size;
}
)GLSL";

inline constexpr const char* blur_body = R"GLSL(
#ifdef HUXERUI_INTEGER_SOURCE
uniform highp usampler2D source;
#else
uniform sampler2D source;
#endif
uniform vec2 texture_size;
uniform ivec2 direction;
uniform int radius;
in vec2 device_pixel;
#ifdef HUXERUI_NORMALIZED_OUTPUT
out vec4 out_color;
#else
out uvec4 out_color;
#endif
void main() {
  ivec2 pixel = ivec2(device_pixel);
  ivec2 last_pixel = textureSize(source, 0) - ivec2(1);
  uvec4 sum = uvec4(0);
  for (int offset = -radius; offset <= radius; ++offset) {
#ifdef HUXERUI_INTEGER_SOURCE
    sum += texelFetch(source, clamp(pixel + direction * offset, ivec2(0), last_pixel), 0);
#else
    vec4 sample_color = texelFetch(source, clamp(pixel + direction * offset, ivec2(0), last_pixel), 0);
    sum += uvec4(floor(sample_color * 255.0 + 0.5));
#endif
  }
  uint divisor = uint(2 * radius + 1);
  uvec4 value = (sum + divisor / 2u) / divisor;
#ifdef HUXERUI_NORMALIZED_OUTPUT
  out_color = vec4(value) / 255.0;
#else
  out_color = value;
#endif
}
)GLSL";

// Independent scanline tiles preserve integer sliding-window sums while exposing enough parallel work.
// Reads are independent of the radius after initialization, avoiding a neighbor loop per output pixel.
inline constexpr const char* compute_blur_body = R"GLSL(
layout(local_size_x = 64) in;
#ifdef HUXERUI_NORMALIZED_OUTPUT
layout(binding = 0, rgba8) writeonly uniform highp image2D destination;
#else
layout(binding = 0, rgba8ui) writeonly uniform highp uimage2D destination;
#endif
#ifdef HUXERUI_INTEGER_SOURCE
uniform highp usampler2D source;
#else
uniform sampler2D source;
#endif
uniform ivec2 direction;
uniform int radius;
uniform uint reciprocal;
uvec4 read_color(int position, int line, int extent) {
  ivec2 pixel = direction.x == 1 ? ivec2(clamp(position, 0, extent - 1), line)
                               : ivec2(line, clamp(position, 0, extent - 1));
#ifdef HUXERUI_INTEGER_SOURCE
  return texelFetch(source, pixel, 0);
#else
  return uvec4(floor(texelFetch(source, pixel, 0) * 255.0 + 0.5));
#endif
}
void main() {
  ivec2 size = textureSize(source, 0);
  int line = int(gl_GlobalInvocationID.x);
  int lines = direction.x == 1 ? size.y : size.x;
  int extent = direction.x == 1 ? size.x : size.y;
  if (line >= lines) return;
  int begin = int(gl_GlobalInvocationID.y) * 128;
  int end = min(begin + 128, extent);
  if (begin >= end) return;
  uvec4 sum = uvec4(0);
  for (int offset = -radius; offset <= radius; ++offset) sum += read_color(begin + offset, line, extent);
  uint divisor = uint(2 * radius + 1);
  for (int position = begin; position < end; ++position) {
    ivec2 pixel = direction.x == 1 ? ivec2(position, line) : ivec2(line, position);
    uvec4 numerator = sum + divisor / 2u;
    uvec4 quotient = numerator;
    if (divisor != 1u) {
      // For divisors up to 255 the rounded byte sum is below 2^16. A 16-bit reciprocal
      // needs one ordinary multiply, and its ceiling can overestimate by at most one.
      if (divisor <= 255u) {
        quotient = (numerator * reciprocal) >> 16;
      } else {
        uvec4 low;
        umulExtended(numerator, uvec4(reciprocal), quotient, low);
      }
      quotient -= uvec4(greaterThan(quotient * divisor, numerator));
    }
#ifdef HUXERUI_NORMALIZED_OUTPUT
    imageStore(destination, pixel, vec4(quotient) / 255.0);
#else
    imageStore(destination, pixel, quotient);
#endif
    sum -= read_color(position - radius, line, extent);
    sum += read_color(position + radius + 1, line, extent);
  }
}
)GLSL";

inline constexpr const char* composite_body = R"GLSL(
uniform sampler2D blurred;
uniform vec2 blurred_origin;
uniform vec2 blurred_size;
uniform sampler2D original;
uniform vec2 texture_size;
uniform mat3 pixel_to_local;
uniform mat3 local_to_pixel;
uniform float pixel_size;
uniform float refraction;
uniform float edge_width;
uniform float highlight;
uniform float saturation;
uniform float brightness;
uniform vec2 light;
uniform float fusion_radius;
uniform int shape_count;
uniform vec4 shape_rect[64];
uniform vec4 shape_radii[64];
in vec2 device_pixel;
out vec4 out_color;

float shape_distance(vec2 point, int index) {
  vec4 rect = shape_rect[index];
  vec4 radii = shape_radii[index];
  vec2 half_size = rect.zw * 0.5;
  vec2 center = rect.xy + half_size;
  vec2 quadrant = abs(point - center) - half_size;
  float separation = length(max(quadrant, vec2(0.0))) + min(max(quadrant.x, quadrant.y), 0.0);
  // Asymmetric normalized radii may exceed half an edge, so corner cells follow their arc
  // tangencies rather than the rectangle's center quadrants.
  if (radii.x > 0.0 && point.x < rect.x + radii.x && point.y < rect.y + radii.x) {
    separation = max(separation, distance(point, vec2(rect.x + radii.x, rect.y + radii.x)) - radii.x);
  }
  if (radii.y > 0.0 && point.x > rect.x + rect.z - radii.y && point.y < rect.y + radii.y) {
    separation = max(separation, distance(point, vec2(rect.x + rect.z - radii.y, rect.y + radii.y)) - radii.y);
  }
  if (radii.z > 0.0 && point.x > rect.x + rect.z - radii.z && point.y > rect.y + rect.w - radii.z) {
    separation = max(separation, distance(point, vec2(rect.x + rect.z - radii.z, rect.y + rect.w - radii.z)) - radii.z);
  }
  if (radii.w > 0.0 && point.x < rect.x + radii.w && point.y > rect.y + rect.w - radii.w) {
    separation = max(separation, distance(point, vec2(rect.x + radii.w, rect.y + rect.w - radii.w)) - radii.w);
  }
  return separation;
}

float surface_distance(vec2 point) {
  float separation = 1.0e20;
  for (int index = 0; index < shape_count; ++index) {
    if (shape_rect[index].z <= 0.0 || shape_rect[index].w <= 0.0) {
      continue;
    }
    float next = shape_distance(point, index);
    if (fusion_radius > 0.0 && separation < 1.0e19) {
      float blend = max(fusion_radius - abs(separation - next), 0.0) / fusion_radius;
      separation = min(separation, next) - blend * blend * fusion_radius * 0.25;
    } else {
      separation = min(separation, next);
    }
  }
  return separation;
}

void main() {
  // Interpolated coordinates can straddle a symmetric surface's zero gradient.
  vec2 pixel = floor(device_pixel) + vec2(0.5);
  vec2 local = (pixel_to_local * vec3(pixel, 1.0)).xy;
  float separation = surface_distance(local);
  float coverage = clamp(0.5 - separation / pixel_size, 0.0, 1.0);
  if (coverage <= 0.0) {
    discard;
  }
  float edge = clamp(1.0 + min(separation, 0.0) / edge_width, 0.0, 1.0);
  float lens = edge * edge;
  vec2 normal = vec2(0.0);
  if (lens > 0.0 && (refraction > 0.0 || highlight > 0.0)) {
    float epsilon = max(pixel_size * 0.5, 0.01);
    normal = vec2(surface_distance(local + vec2(epsilon, 0.0)) - surface_distance(local - vec2(epsilon, 0.0)),
                  surface_distance(local + vec2(0.0, epsilon)) - surface_distance(local - vec2(0.0, epsilon)));
    float normal_length = length(normal);
    // Match the portable processor's coordinate-relative eight-epsilon floor.
    float normal_floor = max(0.00001,
        max(max(abs(local.x), abs(local.y)), max(abs(separation), 1.0)) * 0.00000095367431640625);
    normal = normal_length > normal_floor ? normal / normal_length : vec2(0.0);
  }
  vec2 sample_position = pixel;
  if (lens > 0.0 && refraction > 0.0) {
    sample_position = (local_to_pixel * vec3(local - normal * (refraction * lens), 1.0)).xy;
  }
  vec4 color = texture(blurred, (sample_position - blurred_origin) / blurred_size);
  vec4 previous = texelFetch(original, ivec2(pixel), 0);
  float luminance = color.z * 0.2126 + color.y * 0.7152 + color.x * 0.0722;
  float lighting = dot(normal, light) * highlight * lens;
  vec3 rgb = clamp((luminance + (color.rgb - luminance) * saturation) * brightness, vec3(0.0), vec3(color.w));
  rgb = lighting >= 0.0 ? mix(rgb, vec3(color.w), lighting) : rgb * (1.0 + lighting);
  rgb = clamp(rgb, vec3(0.0), vec3(color.w));
  out_color = vec4(mix(previous.rgb, rgb, coverage), mix(previous.w, color.w, coverage));
}
)GLSL";

inline constexpr const char* transfer_body = R"GLSL(
uniform sampler2D source;
uniform sampler2D original;
uniform sampler2D clip_mask;
uniform vec2 texture_size;
uniform ivec2 source_origin;
uniform float opacity;
uniform int operation;
uniform bool has_mask;
in vec2 device_pixel;
out vec4 out_color;
void main() {
  ivec2 pixel = ivec2(device_pixel);
  vec4 color = texelFetch(source, pixel, 0);
  float coverage = has_mask ? texelFetch(clip_mask, pixel + source_origin, 0).w : 1.0;
  if (operation == 1) {
    out_color = mix(texelFetch(original, pixel, 0), color, coverage);
  } else if (operation == 2) {
    out_color = color.bgra;
  } else {
    out_color = color * (opacity * coverage);
  }
}
)GLSL";

inline constexpr const char* fast_resample_body = R"GLSL(
uniform sampler2D source;
uniform vec2 texture_size;
uniform vec2 source_size;
uniform vec2 source_origin;
uniform float source_scale;
uniform int sample_pairs;
in vec2 device_pixel;
out vec4 out_color;
void main() {
  vec2 p = source_origin + device_pixel * source_scale;
  if (sample_pairs == 2) {
    out_color = (texture(source, (p + vec2(-1, -1)) / source_size)
               + texture(source, (p + vec2(1, -1)) / source_size)
               + texture(source, (p + vec2(-1, 1)) / source_size)
               + texture(source, (p + vec2(1, 1)) / source_size)) * 0.25;
  } else {
    out_color = texture(source, p / source_size);
  }
}
)GLSL";
inline constexpr const char* fast_gaussian_body = R"GLSL(
uniform sampler2D source;
uniform vec2 texture_size;
uniform vec2 direction;
uniform int sample_count;
uniform vec2 samples[65];
in vec2 device_pixel;
out vec4 out_color;
void main() {
  vec2 uv = device_pixel / texture_size;
  vec2 step_size = direction / texture_size;
  vec4 sum = texture(source, uv) * samples[0].x;
  for (int index = 1; index < 65; ++index) {
    if (index >= sample_count) break;
    vec2 delta = step_size * samples[index].y;
    sum += (texture(source, uv + delta) + texture(source, uv - delta)) * samples[index].x;
  }
  out_color = sum;
}
)GLSL";
// Returns the version and precision prefix for the given flavor.
inline constexpr const char* FlavorPrefix(bool gles) {
  return gles ? "#version 300 es\nprecision highp float;\nprecision highp int;\n"
              : "#version 330 core\n";
}

} // namespace huxerui::detail::backdrop_glsl
