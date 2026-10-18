#include <huxerui/effects.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

#include <huxerui/render_scene.h>

#include "effects_internal.h"
#include "geometry_internal.h"
#include "paint_internal.h"

namespace huxerui::detail {
namespace {

void RequireNonNegative(float value) {
  if (!std::isfinite(value) || value < 0.0F) {
    throw std::invalid_argument("HuxerUI backdrop distances and color multipliers must be finite and non-negative");
  }
}

float ShapeDistance(Point point, const BackdropShape& shape) {
  const Rect rect = shape.rect;
  const Point center{rect.x + rect.width * 0.5F, rect.y + rect.height * 0.5F};
  const CornerRadii radii = NormalizeCornerRadii(rect, shape.corner_radii);
  const float x = std::abs(point.x - center.x) - rect.width * 0.5F;
  const float y = std::abs(point.y - center.y) - rect.height * 0.5F;
  float distance = std::hypot(std::max(x, 0.0F), std::max(y, 0.0F)) + std::min(std::max(x, y), 0.0F);
  const auto corner = [&](float radius, Point arc_center, bool in_corner) {
    if (radius > 0.0F && in_corner) {
      distance = std::max(distance, std::hypot(point.x - arc_center.x, point.y - arc_center.y) - radius);
    }
  };
  // Asymmetric normalized radii may exceed half an edge, so corner cells follow their arc tangencies rather than
  // the rectangle's center quadrants.
  corner(radii.top_left, {rect.x + radii.top_left, rect.y + radii.top_left},
         point.x < rect.x + radii.top_left && point.y < rect.y + radii.top_left);
  corner(radii.top_right, {rect.x + rect.width - radii.top_right, rect.y + radii.top_right},
         point.x > rect.x + rect.width - radii.top_right && point.y < rect.y + radii.top_right);
  corner(radii.bottom_right, {rect.x + rect.width - radii.bottom_right, rect.y + rect.height - radii.bottom_right},
         point.x > rect.x + rect.width - radii.bottom_right && point.y > rect.y + rect.height - radii.bottom_right);
  corner(radii.bottom_left, {rect.x + radii.bottom_left, rect.y + rect.height - radii.bottom_left},
         point.x < rect.x + radii.bottom_left && point.y > rect.y + rect.height - radii.bottom_left);
  return distance;
}

float SurfaceDistance(Point point, const DrawBackdropCommand& command) {
  float distance = std::numeric_limits<float>::infinity();
  for (const auto& shape : command.shapes) {
    if (shape.rect.IsEmpty()) { continue; }
    const float next = ShapeDistance(point, shape);
    if (command.effect.fusion_radius > 0.0F && std::isfinite(distance)) {
      const float h = std::max(command.effect.fusion_radius - std::abs(distance - next), 0.0F) /
                      command.effect.fusion_radius;
      distance = std::min(distance, next) - h * h * command.effect.fusion_radius * 0.25F;
    } else {
      distance = std::min(distance, next);
    }
  }
  return distance;
}

void BlurPass(const std::vector<std::uint8_t>& source, std::vector<std::uint8_t>& destination,
              std::size_t width, std::size_t height, std::size_t radius, bool horizontal) {
  const std::size_t length = horizontal ? width : height;
  const std::size_t lines = horizontal ? height : width;
  radius = std::min(radius, length);
  const auto index = [&](std::size_t line, std::size_t coordinate) {
    return (horizontal ? line * width + coordinate : coordinate * width + line) * 4;
  };
  const std::uint64_t divisor = 2 * radius + 1;
  for (std::size_t line = 0; line < lines; ++line) {
    std::array<std::uint64_t, 4> sum{};
    for (std::size_t sample = 0; sample <= 2 * radius; ++sample) {
      const std::size_t coordinate = sample < radius ? 0 : std::min(sample - radius, length - 1);
      const std::size_t offset = index(line, coordinate);
      for (std::size_t channel = 0; channel < 4; ++channel) { sum[channel] += source[offset + channel]; }
    }
    for (std::size_t coordinate = 0; coordinate < length; ++coordinate) {
      const std::size_t offset = index(line, coordinate);
      for (std::size_t channel = 0; channel < 4; ++channel) {
        destination[offset + channel] = static_cast<std::uint8_t>((sum[channel] + divisor / 2) / divisor);
      }
      const std::size_t remove = index(line, coordinate > radius ? coordinate - radius : 0);
      const std::size_t add = index(line, std::min(length - 1, coordinate + radius + 1));
      for (std::size_t channel = 0; channel < 4; ++channel) {
        sum[channel] += source[add + channel];
        sum[channel] -= source[remove + channel];
      }
    }
  }
}

std::array<float, 4> Sample(const std::vector<std::uint8_t>& source, std::size_t width, std::size_t height,
                          Point pixel) {
  const float x = std::clamp(pixel.x - 0.5F, 0.0F, static_cast<float>(width - 1));
  const float y = std::clamp(pixel.y - 0.5F, 0.0F, static_cast<float>(height - 1));
  const std::size_t left = static_cast<std::size_t>(x);
  const std::size_t top = static_cast<std::size_t>(y);
  const std::size_t right = std::min(left + 1, width - 1);
  const std::size_t bottom = std::min(top + 1, height - 1);
  const float fx = x - static_cast<float>(left);
  const float fy = y - static_cast<float>(top);
  std::array<float, 4> result{};
  for (std::size_t channel = 0; channel < 4; ++channel) {
    const float a = source[(top * width + left) * 4 + channel];
    const float b = source[(top * width + right) * 4 + channel];
    const float c = source[(bottom * width + left) * 4 + channel];
    const float d = source[(bottom * width + right) * 4 + channel];
    result[channel] = std::lerp(std::lerp(a, b, fx), std::lerp(c, d, fx), fy) / 255.0F;
  }
  return result;
}

bool HasNative(const RenderNode* node) {
  if (!node) { return false; }
  const auto has_native = [](const PaintSequence& sequence) {
    return std::any_of(sequence.Commands().begin(), sequence.Commands().end(), [](const auto& command) {
      return std::holds_alternative<PlacePlatformViewCommand>(command);
    });
  };
  return has_native(node->content) || has_native(node->foreground) ||
      std::any_of(node->children.begin(), node->children.end(), HasNative);
}

std::size_t RasterCommandCount(const RenderNode* node) {
  if (!node || !node->visible || node->opacity <= 0.0F) { return 0; }
  std::size_t count = 0;
  const auto add = [&](std::size_t amount) {
    if (amount > std::numeric_limits<std::size_t>::max() - count) {
      throw std::logic_error("HuxerUI backdrop scene command count exceeds its supported range");
    }
    count += amount;
  };
  for (const auto* sequence : {&node->content, &node->foreground}) {
    add(static_cast<std::size_t>(std::count_if(sequence->Commands().begin(), sequence->Commands().end(),
        [](const PaintCommand& command) { return !std::holds_alternative<PlacePlatformViewCommand>(command); })));
  }
  for (const auto* child : node->children) { add(RasterCommandCount(child)); }
  return count;
}

} // namespace

void ValidateBackdropEffect(const BackdropEffect& effect) {
  if (effect.blur_quality != BackdropBlurQuality::Precise && effect.blur_quality != BackdropBlurQuality::Fast) {
    throw std::invalid_argument("HuxerUI backdrop blur quality must be valid");
  }
  RequireNonNegative(effect.blur_radius);
  RequireNonNegative(effect.refraction);
  RequireNonNegative(effect.edge_width);
  RequireNonNegative(effect.saturation);
  RequireNonNegative(effect.brightness);
  RequireNonNegative(effect.fusion_radius);
  const float light_length = std::hypot(effect.light_direction.x, effect.light_direction.y);
  if (effect.edge_width == 0.0F || !std::isfinite(effect.highlight) || effect.highlight < 0.0F ||
      effect.highlight > 1.0F || !std::isfinite(light_length) || light_length <= 0.0F) {
    throw std::invalid_argument("HuxerUI backdrop edge width, highlight, and light direction must be valid");
  }
}

void ValidateBackdropShapes(std::span<const BackdropShape> shapes) {
  if (shapes.empty() || shapes.size() > 64) {
    throw std::invalid_argument("HuxerUI backdrop surfaces require between one and 64 shapes");
  }
  for (const auto& shape : shapes) {
    const Rect rect = shape.rect;
    if (!std::isfinite(rect.x) || !std::isfinite(rect.y) || !std::isfinite(rect.width) ||
        !std::isfinite(rect.height) || rect.width < 0.0F || rect.height < 0.0F ||
        !std::isfinite(rect.x + rect.width) || !std::isfinite(rect.y + rect.height)) {
      throw std::invalid_argument("HuxerUI backdrop rectangles must be finite with non-negative dimensions");
    }
    ValidateCornerRadii(shape.corner_radii, "HuxerUI backdrop corner radii must be finite and non-negative");
  }
}

Rect BackdropBounds(const DrawBackdropCommand& command) noexcept {
  Rect bounds;
  bool first = true;
  for (const auto& shape : command.shapes) {
    if (shape.rect.IsEmpty()) { continue; }
    if (first) { bounds = shape.rect; first = false; continue; }
    const float right = std::max(bounds.x + bounds.width, shape.rect.x + shape.rect.width);
    const float bottom = std::max(bounds.y + bounds.height, shape.rect.y + shape.rect.height);
    bounds.x = std::min(bounds.x, shape.rect.x);
    bounds.y = std::min(bounds.y, shape.rect.y);
    bounds.width = right - bounds.x;
    bounds.height = bottom - bounds.y;
  }
  const float outset = command.effect.fusion_radius;
  return first ? Rect{} : Rect{bounds.x - outset, bounds.y - outset, bounds.width + 2 * outset,
                              bounds.height + 2 * outset};
}

float BackdropSamplingOutset(const BackdropEffect& effect) noexcept {
  // Fast filtering includes its downsampling footprint, Gaussian tail and reconstruction footprint.
  const float margin = effect.blur_quality == BackdropBlurQuality::Fast && effect.blur_radius > 0
      ? 12.0F : 3.0F;
  return effect.blur_radius + effect.refraction + margin;
}

Rect BackdropSamplingBounds(const DrawBackdropCommand& command) noexcept {
  const Rect bounds = BackdropBounds(command);
  const float outset = BackdropSamplingOutset(command.effect);
  return bounds.IsEmpty() ? bounds : Rect{bounds.x - outset, bounds.y - outset, bounds.width + 2 * outset,
                                        bounds.height + 2 * outset};
}

void ValidateBackdropCommand(const DrawBackdropCommand& command) {
  ValidateBackdropEffect(command.effect);
  ValidateBackdropShapes(command.shapes);
  const Rect bounds = BackdropSamplingBounds(command);
  if (!std::isfinite(bounds.x) || !std::isfinite(bounds.y) || !std::isfinite(bounds.width) ||
      !std::isfinite(bounds.height) || !std::isfinite(bounds.x + bounds.width) ||
      !std::isfinite(bounds.y + bounds.height)) {
    throw std::invalid_argument("HuxerUI backdrop sampling bounds must remain finite");
  }
}

bool RenderSceneHasBackdropEffects(const RenderNode* node) {
  return node && (node->content.HasBackdropCommands() || node->foreground.HasBackdropCommands() ||
                 std::any_of(node->children.begin(), node->children.end(), RenderSceneHasBackdropEffects));
}

void ValidateBackdropScene(const RenderScene& scene) {
  if (RenderSceneHasBackdropEffects(scene.root) && HasNative(scene.root)) {
    throw std::logic_error("HuxerUI backdrop effects cannot share a scene with native PlatformViews");
  }
}

void ValidateBackdropSceneSlice(const RenderScene& scene, std::size_t first_command, std::size_t command_count) {
  ValidateBackdropScene(scene);
  if (RenderSceneHasBackdropEffects(scene.root) &&
      (first_command != 0 || command_count < RasterCommandCount(scene.root))) {
    throw std::logic_error("HuxerUI backdrop replay requires the complete scene drawing prefix and foreground");
  }
}

BackdropPixels ApplyBackdropEffect(std::span<const std::uint8_t> bgra, std::size_t width, std::size_t height,
                                  std::size_t stride, Transform2D pixel_to_local,
                                  const DrawBackdropCommand& command) {
  ValidateBackdropCommand(command);
  if (width == 0 || height == 0) { return {}; }
  if (width > maximum_backdrop_capture_pixels / height ||
      width > std::numeric_limits<std::size_t>::max() / 4 ||
      stride < width * 4 || height > std::numeric_limits<std::size_t>::max() / stride ||
      bgra.size() < height * stride) {
    throw std::invalid_argument("HuxerUI backdrop capture dimensions, stride, or byte extent exceed supported bounds");
  }
  const auto local_to_pixel = InverseTransform(pixel_to_local);
  const auto finite_transform = [](Transform2D transform) {
    return std::isfinite(transform.m11) && std::isfinite(transform.m12) && std::isfinite(transform.m21) &&
           std::isfinite(transform.m22) && std::isfinite(transform.translate_x) && std::isfinite(transform.translate_y);
  };
  if (!finite_transform(pixel_to_local) || !local_to_pixel || !finite_transform(*local_to_pixel)) {
    throw std::invalid_argument("HuxerUI backdrop capture transform must be finite and invertible");
  }
  const float pixel_size = std::max(std::hypot(pixel_to_local.m11, pixel_to_local.m12),
                                    std::hypot(pixel_to_local.m21, pixel_to_local.m22));
  if (!std::isfinite(pixel_size) || pixel_size <= 0.0F) {
    throw std::invalid_argument("HuxerUI backdrop capture scale must be finite and positive");
  }
  const std::size_t compact_stride = width * 4;
  std::vector<std::uint8_t> filtered(width * height * 4);
  for (std::size_t row = 0; row < height; ++row) {
    std::copy_n(bgra.begin() + static_cast<std::ptrdiff_t>(row * stride), compact_stride,
                filtered.begin() + static_cast<std::ptrdiff_t>(row * compact_stride));
  }
  if (command.effect.blur_radius > 0.0F) {
    const auto& transform = *local_to_pixel;
    const double scale_x = std::hypot(transform.m11, transform.m21);
    const double scale_y = std::hypot(transform.m12, transform.m22);
    const double tolerance = 0.00001 * std::max(scale_x, scale_y);
    const bool aligned =
        (std::abs(transform.m12) <= tolerance && std::abs(transform.m21) <= tolerance) ||
        (std::abs(transform.m11) <= tolerance && std::abs(transform.m22) <= tolerance);
    const double column_dot = static_cast<double>(transform.m11) * transform.m21 +
                              static_cast<double>(transform.m12) * transform.m22;
    const bool uniform = std::abs(scale_x - scale_y) <= tolerance &&
                         std::abs(column_dot) <= tolerance * std::max(scale_x, scale_y);
    if (!aligned && !uniform) {
      throw std::logic_error("HuxerUI backdrop blur requires axis-aligned scaling or a uniform orthogonal transform");
    }
    std::vector<std::uint8_t> temporary(filtered.size());
    const auto radius = [&](double scale, std::size_t extent) {
      const double scaled = std::ceil(static_cast<double>(command.effect.blur_radius) * scale / 3.0);
      return static_cast<std::size_t>(std::min(scaled, static_cast<double>(extent)));
    };
    const std::size_t horizontal_radius = radius(scale_x, width);
    const std::size_t vertical_radius = radius(scale_y, height);
    for (int pass = 0; pass < 3; ++pass) {
      BlurPass(filtered, temporary, width, height, horizontal_radius, true);
      BlurPass(temporary, filtered, width, height, vertical_radius, false);
    }
  }
  BackdropPixels result{std::vector<std::uint8_t>(filtered.size()), width, height, compact_stride};
  for (std::size_t row = 0; row < height; ++row) {
    std::copy_n(bgra.begin() + static_cast<std::ptrdiff_t>(row * stride), compact_stride,
                result.bgra.begin() + static_cast<std::ptrdiff_t>(row * compact_stride));
  }
  const auto& effect = command.effect;
  const float light_length = std::hypot(effect.light_direction.x, effect.light_direction.y);
  const Point light = effect.light_direction / light_length;
  const float epsilon = std::max(pixel_size * 0.5F, 0.01F);
  const Rect output = TransformBounds(*local_to_pixel, BackdropBounds(command));
  if (!std::isfinite(output.x) || !std::isfinite(output.y) || !std::isfinite(output.width) ||
      !std::isfinite(output.height) || !std::isfinite(output.x + output.width) ||
      !std::isfinite(output.y + output.height)) {
    throw std::invalid_argument("HuxerUI backdrop output bounds exceed finite capture geometry");
  }
  const auto lower_bound = [](float value, std::size_t extent) {
    return static_cast<std::size_t>(std::clamp(std::floor(static_cast<double>(value) - 1.0), 0.0,
                                             static_cast<double>(extent)));
  };
  const auto upper_bound = [](float value, std::size_t extent) {
    return static_cast<std::size_t>(std::clamp(std::ceil(static_cast<double>(value) + 1.0), 0.0,
                                             static_cast<double>(extent)));
  };
  const std::size_t left = lower_bound(output.x, width);
  const std::size_t right = upper_bound(output.x + output.width, width);
  const std::size_t top = lower_bound(output.y, height);
  const std::size_t bottom = upper_bound(output.y + output.height, height);
  for (std::size_t y = top; y < bottom; ++y) {
    for (std::size_t x = left; x < right; ++x) {
      const Point pixel{static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F};
      const Point local = pixel_to_local.Apply(pixel);
      const float distance = SurfaceDistance(local, command);
      const float coverage = std::clamp(0.5F - distance / pixel_size, 0.0F, 1.0F);
      if (coverage == 0.0F) { continue; }
      const float edge = std::clamp(1.0F + std::min(distance, 0.0F) / effect.edge_width, 0.0F, 1.0F);
      const float lens = edge * edge;
      Point normal{};
      if (lens > 0.0F && (effect.refraction > 0.0F || effect.highlight > 0.0F)) {
        normal = {SurfaceDistance({local.x + epsilon, local.y}, command) -
                      SurfaceDistance({local.x - epsilon, local.y}, command),
                  SurfaceDistance({local.x, local.y + epsilon}, command) -
                      SurfaceDistance({local.x, local.y - epsilon}, command)};
        const float length = std::hypot(normal.x, normal.y);
        // A central difference near a medial axis can contain only coordinate-rounding noise.
        const float normal_floor = std::max(0.00001F,
            std::max({std::abs(local.x), std::abs(local.y), std::abs(distance), 1.0F}) *
                (8.0F * std::numeric_limits<float>::epsilon()));
        if (length > normal_floor) { normal /= length; } else { normal = {}; }
      }
      const Point sample = lens > 0.0F && effect.refraction > 0.0F
                               ? local_to_pixel->Apply(local - normal * (effect.refraction * lens))
                               : pixel;
      auto color = Sample(filtered, width, height, sample);
      const float luminance = color[2] * 0.2126F + color[1] * 0.7152F + color[0] * 0.0722F;
      const float lighting = (normal.x * light.x + normal.y * light.y) * effect.highlight * lens;
      for (std::size_t channel = 0; channel < 3; ++channel) {
        color[channel] = std::clamp((luminance + (color[channel] - luminance) * effect.saturation) *
                                      effect.brightness, 0.0F, color[3]);
        color[channel] = lighting >= 0.0F ? std::lerp(color[channel], color[3], lighting)
                                         : color[channel] * (1.0F + lighting);
        color[channel] = std::clamp(color[channel], 0.0F, color[3]);
      }
      const std::size_t offset = (y * width + x) * 4;
      for (std::size_t channel = 0; channel < 4; ++channel) {
        const float previous = static_cast<float>(result.bgra[offset + channel]);
        result.bgra[offset + channel] = static_cast<std::uint8_t>(
            std::lround(std::lerp(previous, color[channel] * 255.0F, coverage)));
      }
    }
  }
  return result;
}

} // namespace huxerui::detail
