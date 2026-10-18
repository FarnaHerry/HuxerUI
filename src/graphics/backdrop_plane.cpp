#include "backdrop_plane_internal.h"

#include <algorithm>
#include <cmath>

#include "effects_internal.h"
#include "geometry_internal.h"

namespace huxerui::detail {
namespace {

Rect UnionRect(Rect first, Rect second) noexcept {
  if (first.IsEmpty()) { return second; }
  if (second.IsEmpty()) { return first; }
  const float right = std::max(first.x + first.width, second.x + second.width);
  const float bottom = std::max(first.y + first.height, second.y + second.height);
  first.x = std::min(first.x, second.x);
  first.y = std::min(first.y, second.y);
  first.width = right - first.x;
  first.height = bottom - first.y;
  return first;
}

Rect OutsetRect(Rect rect, float outset) noexcept {
  return Rect{rect.x - outset, rect.y - outset, rect.width + 2 * outset, rect.height + 2 * outset};
}

float StrokeReach(const StrokeStyle& style) noexcept {
  return style.width * std::max(1.0F, style.miter_limit) * 0.5F;
}

float ShadowReach(const Point& offset, float blur, float spread) noexcept {
  return blur + spread + std::max(std::abs(offset.x), std::abs(offset.y));
}

Rect AntialiasedOutputBounds(const BackdropPlaneCommand& member) noexcept {
  Rect bounds = BackdropBounds(*member.command);
  if (bounds.IsEmpty()) { return {}; }
  if (const auto inverse = InverseTransform(member.local_to_device)) {
    // Coverage extends half a capture pixel in the processor's local-space distance metric.
    const float pixel_size = std::max(std::hypot(inverse->m11, inverse->m12),
                                     std::hypot(inverse->m21, inverse->m22));
    bounds = OutsetRect(bounds, pixel_size * 0.5F);
  }
  return TransformBounds(member.local_to_device, bounds);
}

} // namespace

bool BackdropPlaneCollector::TryAdd(const DrawBackdropCommand& command, Transform2D local_to_device,
                                    float device_margin) {
  if (!batch_.commands.empty() &&
      (batch_.commands.size() >= maximum_commands || shapes_ + command.shapes.size() > maximum_shapes)) {
    return false;
  }
  const Rect sampling = OutsetRect(TransformBounds(local_to_device, BackdropSamplingBounds(command)),
                                   device_margin);
  const bool samples_previous_output = std::any_of(batch_.commands.begin(), batch_.commands.end(),
      [&](const BackdropPlaneCommand& member) { return sampling.Intersects(AntialiasedOutputBounds(member)); });
  if (samples_previous_output) {
    return false;
  }
  const BackdropPlaneCommand member{&command, local_to_device};
  const Rect output = TransformBounds(local_to_device, BackdropBounds(command));
  batch_.commands.push_back(member);
  batch_.sampling_bounds = UnionRect(batch_.sampling_bounds, sampling);
  batch_.output_bounds = UnionRect(batch_.output_bounds, output);
  shapes_ += command.shapes.size();
  return true;
}

bool BackdropPlaneCollector::BlockedBy(Rect device_bounds) const noexcept {
  return !batch_.commands.empty() && batch_.sampling_bounds.Intersects(device_bounds);
}

void BackdropPlaneCollector::Clear() noexcept {
  batch_.commands.clear();
  batch_.sampling_bounds = Rect{};
  batch_.output_bounds = Rect{};
  shapes_ = 0;
}

std::optional<Rect> ConservativeRasterBounds(const PaintCommand& command) {
  return std::visit([](const auto& value) -> std::optional<Rect> {
    using Command = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<Command, DrawRectCommand>) {
      return value.rect;
    } else if constexpr (std::is_same_v<Command, DrawTextCommand>) {
      return value.rect;
    } else if constexpr (std::is_same_v<Command, DrawTextRunsCommand>) {
      Rect bounds;
      for (const TextRun& run : value.runs) {
        bounds = bounds.IsEmpty() ? run.bounds : UnionRect(bounds, run.bounds);
      }
      return bounds;
    } else if constexpr (std::is_same_v<Command, DrawImageCommand> ||
                         std::is_same_v<Command, DrawExternalTextureCommand>) {
      return value.destination;
    } else if constexpr (std::is_same_v<Command, DrawCircleCommand>) {
      return Rect{value.center.x - value.radius, value.center.y - value.radius, 2 * value.radius,
                  2 * value.radius};
    } else if constexpr (std::is_same_v<Command, DrawLineCommand>) {
      const float reach = StrokeReach(value.style);
      return OutsetRect(Rect{std::min(value.start.x, value.end.x), std::min(value.start.y, value.end.y),
                             std::abs(value.end.x - value.start.x), std::abs(value.end.y - value.start.y)},
                        reach);
    } else if constexpr (std::is_same_v<Command, DrawArcCommand>) {
      const float reach = value.radius + StrokeReach(value.style);
      return Rect{value.center.x - reach, value.center.y - reach, 2 * reach, 2 * reach};
    } else if constexpr (std::is_same_v<Command, DrawBorderCommand>) {
      return OutsetRect(value.rect, value.style.width);
    } else if constexpr (std::is_same_v<Command, DrawShadowCommand>) {
      return OutsetRect(value.rect, ShadowReach(value.offset, value.blur_radius, value.spread));
    } else if constexpr (std::is_same_v<Command, FillPathCommand>) {
      return value.path.Bounds();
    } else if constexpr (std::is_same_v<Command, StrokePathCommand>) {
      return OutsetRect(value.path.Bounds(), StrokeReach(value.style));
    } else if constexpr (std::is_same_v<Command, DrawPathShadowCommand>) {
      return OutsetRect(value.path.Bounds(), ShadowReach(value.offset, value.blur_radius, 0.0F));
    } else {
      // Backdrop, clip, transform, and platform placement are structural during replay; callers
      // flush pending batches around them instead of deriving raster bounds.
      return std::nullopt;
    }
  }, command);
}

} // namespace huxerui::detail
