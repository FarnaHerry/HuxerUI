#include "android_renderer.h"
#include "android_backdrop_trace_internal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <limits>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/android/jni.h>

#include "graphics/effects_internal.h"
#include "graphics/geometry_internal.h"
#include "internal_access.h"
#include "text/font_internal.h"
#include "graphics/path_internal.h"
#include "graphics/paint_internal.h"
#include "resources/resource_internal.h"
#include "graphics/shadow_internal.h"
#include "text/text_internal.h"

namespace huxerui::detail {

namespace {
std::mutex font_sources_mutex;
std::unordered_map<std::uint64_t, std::weak_ptr<const FontFaceData>> font_sources;
}

std::string AndroidFontName(const Font& font) {
  const auto face = ResolveFontFace(font);
  if (!face) return std::string(font.FamilyName());
  std::scoped_lock lock(font_sources_mutex);
  std::erase_if(font_sources, [](const auto& entry) { return entry.second.expired(); });
  font_sources.insert_or_assign(face->identity, face);
  return std::to_string(face->identity);
}

std::shared_ptr<const FontFaceData> AndroidFontSource(std::uint64_t identity) {
  std::scoped_lock lock(font_sources_mutex);
  const auto found = font_sources.find(identity);
  return found == font_sources.end() ? nullptr : found->second.lock();
}

namespace {

bool NativeBlurSceneEligible(const RenderNode& node, bool optics_available) {
  for (const PaintSequence* sequence : {&node.content, &node.foreground}) {
    for (const auto& command : sequence->Commands()) {
      if (const auto* backdrop = std::get_if<DrawBackdropCommand>(&command)) {
        const auto& effect = backdrop->effect;
        if (effect.blur_quality != BackdropBlurQuality::Fast) {
          return false;
        }
        const bool optical = effect.refraction != 0 || effect.highlight != 0 || effect.fusion_radius != 0 ||
                             effect.saturation != 1 || effect.brightness != 1;
        // Unblurred source discontinuities exceed the native optical sampling tolerance on some drivers.
        if (optical && (!optics_available || effect.blur_radius == 0)) {
          return false;
        }
      }
    }
  }
  for (const auto* child : node.children) {
    if (child && !NativeBlurSceneEligible(*child, optics_available)) return false;
  }
  return true;
}

bool HasMutableRasterSource(const RenderNode& node) {
  for (const PaintSequence* sequence : {&node.content, &node.foreground}) {
    for (const auto& command : sequence->Commands()) {
      if (std::holds_alternative<DrawImageCommand>(command) ||
          std::holds_alternative<DrawExternalTextureCommand>(command)) {
        return true;
      }
    }
  }
  for (const auto* child : node.children) {
    if (child && HasMutableRasterSource(*child)) {
      return true;
    }
  }
  return false;
}

constexpr float kRadiansToDegrees = 57.2957795130823208768F;

jint PackColor(Color color) {
  const auto channel = [](float value) {
    return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
  };
  return static_cast<jint>(
      channel(color.alpha) << 24U | channel(color.red) << 16U | channel(color.green) << 8U | channel(color.blue)
  );
}

std::span<const std::byte> ByteSpan(std::string_view value) {
  return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

std::span<const std::byte> ByteSpan(const std::vector<jbyte>& value) {
  return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

jintArray ToIntArray(JNIEnv* environment, const std::vector<jint>& values) {
  jintArray result = environment->NewIntArray(static_cast<jsize>(values.size()));
  if (result != nullptr && !values.empty()) {
    environment->SetIntArrayRegion(result, 0, static_cast<jsize>(values.size()), values.data());
  }
  return result;
}

jfloatArray ToFloatArray(JNIEnv* environment, const std::vector<jfloat>& values) {
  jfloatArray result = environment->NewFloatArray(static_cast<jsize>(values.size()));
  if (result != nullptr && !values.empty()) {
    environment->SetFloatArrayRegion(result, 0, static_cast<jsize>(values.size()), values.data());
  }
  return result;
}

jfloatArray ToPathArray(JNIEnv* environment, const Path& path) {
  std::vector<jfloat> data;
  const std::span<const PathElement> elements = InternalAccess::Elements(path);
  data.reserve(elements.size() * 7);
  for (const PathElement& element : elements) {
    data.push_back(static_cast<jfloat>(element.verb));
    const auto append = [&data](Point point) {
      data.push_back(point.x);
      data.push_back(point.y);
    };
    switch (element.verb) {
    case PathVerb::MoveTo:
    case PathVerb::LineTo:
      append(element.points[0]);
      break;
    case PathVerb::QuadraticTo:
      append(element.points[0]);
      append(element.points[1]);
      break;
    case PathVerb::CubicTo:
      append(element.points[0]);
      append(element.points[1]);
      append(element.points[2]);
      break;
    case PathVerb::Close:
      break;
    }
  }

  jfloatArray result = environment->NewFloatArray(static_cast<jsize>(data.size()));
  if (result != nullptr && !data.empty()) {
    environment->SetFloatArrayRegion(result, 0, static_cast<jsize>(data.size()), data.data());
  }
  return result;
}

struct JavaBrush final {
  jint kind = 0;
  jint color = 0;
  android::LocalRef<jfloatArray> geometry;
  android::LocalRef<jfloatArray> stops;
  android::LocalRef<jintArray> colors;

  [[nodiscard]] bool IsValid() const noexcept {
    return kind == 1 || (geometry && stops && colors);
  }
};

JavaBrush ToJavaBrush(JNIEnv* environment, Rect bounds, const Brush& brush) {
  JavaBrush result;
  std::visit(
      [&](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::same_as<Value, Color>) {
          result.kind = 1;
          result.color = PackColor(value);
        } else {
          result.kind = std::same_as<Value, LinearGradient> ? 2 : 3;
          const Transform2D transform = ResolveGradientTransform(bounds, value);
          std::vector<jfloat> geometry;
          if constexpr (std::same_as<Value, LinearGradient>) {
            geometry = {value.start.x, value.start.y, value.end.x, value.end.y};
          } else {
            geometry = {value.center.x, value.center.y, value.radius.width, value.radius.height};
          }
          geometry.insert(geometry.end(),
                          {transform.m11, transform.m12, transform.m21, transform.m22, transform.translate_x,
                           transform.translate_y});
          std::vector<jfloat> offsets;
          std::vector<jint> colors;
          offsets.reserve(value.stops.size());
          colors.reserve(value.stops.size());
          for (const GradientStop& stop : value.stops) {
            offsets.push_back(stop.offset);
            colors.push_back(PackColor(stop.color));
          }
          result.geometry = {environment, ToFloatArray(environment, geometry)};
          result.stops = {environment, ToFloatArray(environment, offsets)};
          result.colors = {environment, ToIntArray(environment, colors)};
        }
      },
      brush.Get()
  );
  return result;
}

} // namespace

void AndroidRenderer::Initialize(JNIEnv* environment, jclass view_class) {
  draw_brush_rect_ =
      environment->GetMethodID(view_class, "drawBrushRect", "(Landroid/graphics/Canvas;FFFFII[F[F[IF)V");
  draw_text_ =
      environment->GetMethodID(view_class, "drawText", "(Landroid/graphics/Canvas;[BFFFFFFIFI[BIIIIIII[B[B)V");
  draw_text_runs_ =
      environment->GetMethodID(view_class, "drawTextRuns", "(Landroid/graphics/Canvas;[B[I[F[I[F[I[B[I)V");
  draw_image_ = environment->GetMethodID(view_class, "drawImage", "(Landroid/graphics/Canvas;J[BFFFFFFFFFI)Z");
  draw_external_texture_ = environment->GetMethodID(
      view_class,
      "drawExternalTexture",
      "(Landroid/graphics/Canvas;Landroid/graphics/Bitmap;FFFFFFFFFII)V"
  );
  draw_circle_ = environment->GetMethodID(view_class, "drawCircle", "(Landroid/graphics/Canvas;FFFI)V");
  draw_line_ = environment->GetMethodID(view_class, "drawLine", "(Landroid/graphics/Canvas;FFFFIFIIF[FF)V");
  draw_arc_ = environment->GetMethodID(view_class, "drawArc", "(Landroid/graphics/Canvas;FFFFFIFIIF[FF)V");
  draw_border_ = environment->GetMethodID(view_class, "drawBorder", "(Landroid/graphics/Canvas;FFFFIFF)V");
  draw_shadow_ = environment->GetMethodID(view_class, "drawShadow", "(Landroid/graphics/Canvas;FFFFIFF)V");
  fill_brush_path_ =
      environment->GetMethodID(view_class, "fillBrushPath", "(Landroid/graphics/Canvas;[FIII[F[F[I)V");
  stroke_brush_path_ =
      environment->GetMethodID(view_class, "strokeBrushPath", "(Landroid/graphics/Canvas;[FII[F[F[IFIIF[FF)V");
  draw_path_shadow_ = environment->GetMethodID(view_class, "drawPathShadow", "(Landroid/graphics/Canvas;[FFFFFIFFFI)V");
  push_clip_ = environment->GetMethodID(view_class, "pushClip", "(Landroid/graphics/Canvas;FFFFF)V");
  push_path_clip_ = environment->GetMethodID(view_class, "pushPathClip", "(Landroid/graphics/Canvas;[FI)V");
  pop_clip_ = environment->GetMethodID(view_class, "popClip", "(Landroid/graphics/Canvas;)V");
  push_opacity_ = environment->GetMethodID(view_class, "pushOpacity", "(Landroid/graphics/Canvas;F)V");
  pop_opacity_ = environment->GetMethodID(view_class, "popOpacity", "(Landroid/graphics/Canvas;)V");
  push_transform_ = environment->GetMethodID(view_class, "pushTransform", "(Landroid/graphics/Canvas;FFFFFF)V");
  pop_transform_ = environment->GetMethodID(view_class, "popTransform", "(Landroid/graphics/Canvas;)V");
  native_backdrop_available_ = environment->GetMethodID(view_class, "nativeBackdropAvailable",
                                                       "(Landroid/graphics/Canvas;)I");
  native_backdrop_fallback_ =
      environment->GetMethodID(view_class, "nativeBackdropFallback", "(Ljava/lang/Throwable;)I");
  begin_native_backdrop_canvas_ = environment->GetMethodID(view_class, "beginNativeBackdropCanvas",
                                                         "(Landroid/graphics/Canvas;)Landroid/graphics/Canvas;");
  draw_native_backdrop_ = environment->GetMethodID(view_class, "drawNativeBackdrop", "(Landroid/graphics/Canvas;[F[F)V");
  begin_backdrop_canvas_ = environment->GetMethodID(view_class, "beginBackdropCanvas",
                                                    "(Landroid/graphics/Canvas;)Landroid/graphics/Canvas;");
  end_backdrop_canvas_ = environment->GetMethodID(view_class, "endBackdropCanvas",
                                                  "(Landroid/graphics/Canvas;Landroid/graphics/Canvas;F)V");
  release_backdrop_canvases_ = environment->GetMethodID(view_class, "releaseBackdropCanvases", "()V");
  backdrop_pixels_ = environment->GetMethodID(view_class, "backdropPixels", "(Landroid/graphics/Canvas;IIII)[I");
  backdrop_transform_ = environment->GetMethodID(view_class, "backdropTransform", "(Landroid/graphics/Canvas;)[F");
  draw_backdrop_pixels_ =
      environment->GetMethodID(view_class, "drawBackdropPixels", "(Landroid/graphics/Canvas;[IIIII)V");

  backdrop_gpu_available_ =
      environment->GetMethodID(view_class, "backdropGpuAvailable", "(Landroid/graphics/Canvas;)Z");
  begin_backdrop_gpu_canvas_ = environment->GetMethodID(view_class, "beginBackdropGpuCanvas",
                                                        "(Landroid/graphics/Canvas;)Landroid/graphics/Canvas;");
  backdrop_bitmap_ = environment->GetMethodID(view_class, "backdropHardwareRaster",
                                              "(Landroid/graphics/Canvas;IIII)Ljava/lang/Object;");
  backdrop_mask_ =
      environment->GetMethodID(view_class, "backdropMask", "(Landroid/graphics/Canvas;[F)Landroid/graphics/Bitmap;");
  clear_backdrop_raster_ = environment->GetMethodID(view_class, "clearBackdropRaster", "(Landroid/graphics/Canvas;)V");
  discard_backdrop_canvas_ =
      environment->GetMethodID(view_class, "discardBackdropCanvas", "(Landroid/graphics/Canvas;)V");
  present_backdrop_hardware_ =
      environment->GetMethodID(view_class, "presentBackdropHardware", "(Landroid/graphics/Canvas;Ljava/lang/Object;)V");
  set_backdrop_raster_transform_ =
      environment->GetMethodID(view_class, "setBackdropRasterTransform", "(Landroid/graphics/Canvas;FFFFFF)V");

  raster_environment_method_ = environment->GetMethodID(view_class, "backdropRasterEnvironment", "()Ljava/lang/String;");
  redraw_hardware_method_ = environment->GetMethodID(view_class, "redrawBackdropHardware", "(Landroid/graphics/Canvas;)V");
  if (native_backdrop_available_ == nullptr || native_backdrop_fallback_ == nullptr ||
      begin_native_backdrop_canvas_ == nullptr ||
      draw_native_backdrop_ == nullptr || draw_brush_rect_ == nullptr || draw_text_ == nullptr ||
      draw_text_runs_ == nullptr || draw_image_ == nullptr ||
      draw_external_texture_ == nullptr || draw_circle_ == nullptr || draw_line_ == nullptr || draw_arc_ == nullptr ||
      draw_border_ == nullptr || draw_shadow_ == nullptr || fill_brush_path_ == nullptr ||
      stroke_brush_path_ == nullptr || draw_path_shadow_ == nullptr || push_clip_ == nullptr ||
      push_path_clip_ == nullptr || pop_clip_ == nullptr || push_opacity_ == nullptr || pop_opacity_ == nullptr ||
      push_transform_ == nullptr || pop_transform_ == nullptr || begin_backdrop_canvas_ == nullptr ||
      end_backdrop_canvas_ == nullptr || release_backdrop_canvases_ == nullptr || backdrop_pixels_ == nullptr ||
      backdrop_transform_ == nullptr || draw_backdrop_pixels_ == nullptr || raster_environment_method_ == nullptr ||
      redraw_hardware_method_ == nullptr || backdrop_gpu_available_ == nullptr ||
      begin_backdrop_gpu_canvas_ == nullptr || backdrop_bitmap_ == nullptr || backdrop_mask_ == nullptr ||
      clear_backdrop_raster_ == nullptr || discard_backdrop_canvas_ == nullptr ||
      present_backdrop_hardware_ == nullptr || set_backdrop_raster_transform_ == nullptr) {
    throw std::runtime_error("HuxerUI Android renderer methods do not match the platform backend");
  }
}

void AndroidRenderer::SetTextureLayers(AndroidTextureLayers* texture_layers) noexcept {
  texture_layers_ = texture_layers;
}

void AndroidRenderer::BeginDraw() {
  if (draw_epoch_ == std::numeric_limits<std::uint64_t>::max()) {
    draw_epoch_ = 0;
    for (CachedExternalTexture& cached : external_textures_) {
      cached.draw_epoch = std::numeric_limits<std::uint64_t>::max();
    }
  }
  ++draw_epoch_;
  std::erase_if(external_textures_, [](const CachedExternalTexture& cached) {
    const std::shared_ptr<ExternalTexture> texture = cached.texture.lock();
    return !texture || !texture->IsActive();
  });
}

struct AndroidRenderer::CommandRange {
  std::size_t first = 0;
  std::size_t end = 0;
  std::size_t cursor = 0;
};

bool AndroidRenderer::RenderSequence(
    JNIEnv* environment, jobject view, jobject canvas, const PaintSequence& sequence, std::uint64_t node_identity,
    bool foreground, CommandRange* range
) {
  std::size_t texture_ordinal = 0;
  for (const PaintCommand& command : sequence.Commands()) {
    if (std::holds_alternative<PlacePlatformViewCommand>(command)) {
      continue;
    }
    const auto* texture = std::get_if<DrawExternalTextureCommand>(&command);
    const std::size_t current_texture_ordinal = texture_ordinal;
    if (texture != nullptr) {
      ++texture_ordinal;
    }
    const bool selected = range == nullptr || (range->cursor >= range->first && range->cursor < range->end);
    if (range != nullptr) {
      ++range->cursor;
    }
    if (!selected) {
      continue;
    }
    if (backdrop_gpu_drawing_) {
      if (std::holds_alternative<DrawBackdropCommand>(command)) {
        RenderCommand(environment, view, canvas, std::get<DrawBackdropCommand>(command));
        if (environment->ExceptionCheck()) {
          return false;
        }
        continue;
      }
      const auto bounds = ConservativeRasterBounds(command);
      if (texture != nullptr || (!bounds && !std::holds_alternative<PushTransformCommand>(command) &&
                                 !std::holds_alternative<PopTransformCommand>(command))) {
        FlushBackdropPlane(environment, view, canvas);
      } else if (bounds) {
        if (deferred_raster_.size() == 4096) {
          FlushBackdropPlane(environment, view, canvas);
        }
        if (!backdrop_planes_.empty()) {
          const Transform2D transform = BackdropTransform(environment, view, canvas);
          Rect device_bounds = TransformBounds(transform, *bounds);
          if (!device_bounds.IsEmpty()) {
            device_bounds = {device_bounds.x - 3.0F, device_bounds.y - 3.0F, device_bounds.width + 6.0F,
                             device_bounds.height + 6.0F};
          }
          deferred_raster_.push_back({&command, transform, device_bounds});
          continue;
        }
        backdrop_raster_dirty_ = true;
      }
      if (environment->ExceptionCheck()) {
        return false;
      }
    }
    if (backdrop_gpu_drawing_ && ConservativeRasterBounds(command)) {
      RecordRaster(environment, view, canvas, command);
    }
    if (texture != nullptr) {
      RenderCommand(environment, view, canvas, *texture,
                    AndroidTextureLayerKey{
                        .node_identity = node_identity,
                        .texture_ordinal = current_texture_ordinal,
                        .foreground = foreground,
                    });
      if (backdrop_gpu_drawing_) {
        backdrop_raster_dirty_ = true;
      }
    } else {
      std::visit(
          [this, environment, view, canvas](const auto& value) {
            using Command = std::remove_cvref_t<decltype(value)>;
            if constexpr (!std::same_as<Command, DrawExternalTextureCommand>) {
              RenderCommand(environment, view, canvas, value);
            }
          },
          command);
    }
    if (environment->ExceptionCheck()) {
      return false;
    }
  }
  return true;
}

bool AndroidRenderer::RenderSceneNode(
    JNIEnv* environment, jobject view, jobject canvas, const RenderNode& node, CommandRange* range
) {
  const float opacity = std::clamp(node.opacity, 0.0F, 1.0F);
  if (!node.visible || opacity <= 0.0F || environment->ExceptionCheck()) {
    return !environment->ExceptionCheck();
  }

  Transform2D transform = node.transform;
  transform.translate_x += node.offset.x;
  transform.translate_y += node.offset.y;
  const bool transformed = !transform.IsIdentity();
  if (transformed) {
    RenderCommand(environment, view, canvas, PushTransformCommand{transform});
    if (environment->ExceptionCheck()) {
      return false;
    }
  }

  std::vector<RasterClip> parent_clips;
  const bool translucent = opacity < 1.0F;
  const jobject parent_canvas = canvas;
  android::LocalRef<jobject> group_canvas;
  if (translucent) {
    if (backdrop_drawing_) {
      if (backdrop_gpu_drawing_) {
        FlushBackdropPlane(environment, view, canvas);
        FlushBackdropRaster(environment, view, canvas);
        if (environment->ExceptionCheck()) {
          return false;
        }
        track_raster_ = false;
        raster_operations_.clear();
        parent_clips = std::move(raster_clips_);
        raster_clips_.clear();
        backdrop_gl_->PushGroup();
      }
      group_canvas = {environment, environment->CallObjectMethod(view, begin_backdrop_canvas_, canvas)};
      canvas = group_canvas.Get();
    } else {
      environment->CallVoidMethod(view, push_opacity_, canvas, opacity);
    }
    if (environment->ExceptionCheck()) {
      return false;
    }
  }

  if (!RenderSequence(environment, view, canvas, node.content, node.id, false, range)) {
    return false;
  }
  if (backdrop_gpu_drawing_ && !node.child_clips.empty()) {
    FlushBackdropPlane(environment, view, canvas);
  }
  for (const RenderClip& clip : node.child_clips) {
    std::visit([&](const auto& command) { RenderCommand(environment, view, canvas, command); }, clip);
    if (environment->ExceptionCheck()) {
      return false;
    }
  }
  const bool children_transformed = !node.children_transform.IsIdentity();
  if (children_transformed) {
    RenderCommand(environment, view, canvas, PushTransformCommand{node.children_transform});
    if (environment->ExceptionCheck()) {
      return false;
    }
  }
  for (const RenderNode* child : node.children) {
    if (child != nullptr && !RenderSceneNode(environment, view, canvas, *child, range)) {
      return false;
    }
  }
  if (children_transformed) {
    RenderCommand(environment, view, canvas, PopTransformCommand{});
    if (environment->ExceptionCheck()) {
      return false;
    }
  }
  if (backdrop_gpu_drawing_ && !node.child_clips.empty()) {
    FlushBackdropPlane(environment, view, canvas);
  }
  for (std::size_t index = 0; index < node.child_clips.size(); ++index) {
    RenderCommand(environment, view, canvas, PopClipCommand{});
    if (environment->ExceptionCheck()) {
      return false;
    }
  }
  if (!RenderSequence(environment, view, canvas, node.foreground, node.id, true, range)) {
    return false;
  }
  if (translucent) {
    if (backdrop_drawing_) {
      if (backdrop_gpu_drawing_) {
        FlushBackdropPlane(environment, view, canvas);
        FlushBackdropRaster(environment, view, canvas);
        if (environment->ExceptionCheck()) {
          return false;
        }
        auto mask = BackdropMask(environment, view, parent_canvas, parent_clips);
        if (environment->ExceptionCheck()) {
          return false;
        }
        backdrop_gl_->PopGroup(environment, mask.Get(), opacity);
        if (environment->ExceptionCheck()) {
          return false;
        }
        environment->CallVoidMethod(view, discard_backdrop_canvas_, canvas);
        raster_clips_ = std::move(parent_clips);
      } else {
        environment->CallVoidMethod(view, end_backdrop_canvas_, parent_canvas, canvas, opacity);
      }
      canvas = parent_canvas;
    } else {
      environment->CallVoidMethod(view, pop_opacity_, canvas);
    }
    if (environment->ExceptionCheck()) {
      return false;
    }
  }
  if (transformed) {
    RenderCommand(environment, view, canvas, PopTransformCommand{});
    if (environment->ExceptionCheck()) {
      return false;
    }
  }
  return true;
}

void AndroidRenderer::RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawRectCommand& command) {
  JavaBrush brush = ToJavaBrush(environment, command.rect, command.brush);
  if (!brush.IsValid()) {
    return;
  }
  environment->CallVoidMethod(
      view,
      draw_brush_rect_,
      canvas,
      command.rect.x,
      command.rect.y,
      command.rect.width,
      command.rect.height,
      brush.kind,
      brush.color,
      brush.geometry.Get(),
      brush.stops.Get(),
      brush.colors.Get(),
      command.corner_radius
  );
}

void AndroidRenderer::RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawTextCommand& command) {
  auto attributes = AndroidTextAttributes(environment, command.text, command.style);
  if (!attributes) {
    return;
  }
  auto bytes = android::BytesToJavaByteArray(environment, ByteSpan(command.text.PlainText()));
  auto family = android::BytesToJavaByteArray(environment, ByteSpan(AndroidFontName(command.style.font)));
  auto locale = android::BytesToJavaByteArray(environment, ByteSpan(command.options.shaping.locale));
  if (!bytes || !family || !locale) {
    return;
  }
  environment->CallVoidMethod(view, draw_text_, canvas, bytes.Get(),
      command.rect.x, command.rect.y, command.rect.width, command.rect.height,
      command.paragraph_offset.x, command.paragraph_offset.y, PackColor(command.style.foreground),
      command.style.font.Size(), static_cast<jint>(command.style.font.FamilyKind()), family.Get(),
      static_cast<jint>(command.style.font.Weight()), static_cast<jint>(command.style.font.Slant()),
      static_cast<jint>(command.style.decoration), static_cast<jint>(command.options.align),
      static_cast<jint>(command.options.vertical_align), static_cast<jint>(command.options.wrap),
      static_cast<jint>(command.options.shaping.direction), locale.Get(), attributes.Get());
}

void AndroidRenderer::RenderCommand(
    JNIEnv* environment, jobject view, jobject canvas, const DrawTextRunsCommand& command
) {
  std::vector<jbyte> text_data;
  std::vector<jint> text_ranges;
  std::vector<jfloat> baselines;
  std::vector<jint> colors;
  std::vector<jfloat> font_sizes;
  std::vector<jint> styles;
  std::vector<jbyte> metadata;
  std::vector<jint> metadata_ranges;
  text_ranges.reserve(command.runs.size() * 2);
  baselines.reserve(command.runs.size() * 2);
  colors.reserve(command.runs.size());
  font_sizes.reserve(command.runs.size());
  styles.reserve(command.runs.size() * 5);
  metadata_ranges.reserve(command.runs.size() * 4);

  const auto append = [](std::vector<jbyte>& destination, std::string_view value) {
    const jint offset = static_cast<jint>(destination.size());
    destination.insert(destination.end(), value.begin(), value.end());
    return std::pair{offset, static_cast<jint>(value.size())};
  };
  for (const TextRun& run : command.runs) {
    const auto text_range = append(text_data, run.text);
    const auto family_range = append(metadata, AndroidFontName(run.style.font));
    const auto locale_range = append(metadata, run.shaping.locale);
    text_ranges.insert(text_ranges.end(), {text_range.first, text_range.second});
    baselines.insert(baselines.end(), {run.baseline_origin.x, run.baseline_origin.y});
    colors.push_back(PackColor(run.style.foreground));
    font_sizes.push_back(run.style.font.Size());
    styles.insert(
        styles.end(),
        {
            static_cast<jint>(run.style.font.FamilyKind()),
            static_cast<jint>(run.style.font.Weight()),
            static_cast<jint>(run.style.font.Slant()),
            static_cast<jint>(run.style.decoration),
            static_cast<jint>(run.shaping.direction),
        }
    );
    metadata_ranges.insert(
        metadata_ranges.end(),
        {family_range.first, family_range.second, locale_range.first, locale_range.second}
    );
  }

  auto text_array = android::BytesToJavaByteArray(environment, ByteSpan(text_data));
  jintArray text_range_array = ToIntArray(environment, text_ranges);
  jfloatArray baseline_array = ToFloatArray(environment, baselines);
  jintArray color_array = ToIntArray(environment, colors);
  jfloatArray font_size_array = ToFloatArray(environment, font_sizes);
  jintArray style_array = ToIntArray(environment, styles);
  auto metadata_array = android::BytesToJavaByteArray(environment, ByteSpan(metadata));
  jintArray metadata_range_array = ToIntArray(environment, metadata_ranges);
  if (text_array && text_range_array != nullptr && baseline_array != nullptr && color_array != nullptr &&
      font_size_array != nullptr && style_array != nullptr && metadata_array && metadata_range_array != nullptr) {
    environment->CallVoidMethod(
        view,
        draw_text_runs_,
        canvas,
        text_array.Get(),
        text_range_array,
        baseline_array,
        color_array,
        font_size_array,
        style_array,
        metadata_array.Get(),
        metadata_range_array
    );
  }
  const jobject references[] = {
      text_range_array,
      baseline_array,
      color_array,
      font_size_array,
      style_array,
      metadata_range_array,
  };
  for (jobject reference : references) {
    if (reference != nullptr) {
      environment->DeleteLocalRef(reference);
    }
  }
}

void AndroidRenderer::RenderCommand(
    JNIEnv* environment, jobject view, jobject canvas, const DrawImageCommand& command
) {
  const std::uint64_t identity = InternalAccess::ImageIdentity(command.image);
  const float scale = command.image.Scale();
  const auto draw = [&](jbyteArray encoded) {
    return environment->CallBooleanMethod(
               view,
               draw_image_,
               canvas,
               static_cast<jlong>(identity),
               encoded,
               command.source.x * scale,
               command.source.y * scale,
               command.source.width * scale,
               command.source.height * scale,
               command.destination.x,
               command.destination.y,
               command.destination.width,
               command.destination.height,
               command.opacity,
               static_cast<jint>(command.sampling)
           ) == JNI_TRUE;
  };
  // Probe the Java Bitmap cache without allocating or copying encoded bytes; transfer the payload only on a miss.
  if (draw(nullptr) || environment->ExceptionCheck()) {
    return;
  }
  auto encoded = android::BytesToJavaByteArray(environment, command.image.EncodedBytes());
  if (encoded) {
    static_cast<void>(draw(encoded.Get()));
  }
}

void AndroidRenderer::RenderCommand(
    JNIEnv* environment, jobject view, jobject canvas, const DrawExternalTextureCommand& command,
    const AndroidTextureLayerKey& key
) {
  if (std::dynamic_pointer_cast<android::BitmapTexture>(command.texture)) {
    const AndroidBitmapFrame* frame = BitmapFrameFor(command.texture);
    if (frame == nullptr) {
      return;
    }
    const Size intrinsic_size = command.texture->IntrinsicSize();
    const float scale_x = static_cast<float>(frame->PixelWidth()) / intrinsic_size.width;
    const float scale_y = static_cast<float>(frame->PixelHeight()) / intrinsic_size.height;
    environment->CallVoidMethod(
        view, draw_external_texture_, canvas, frame->Bitmap(), command.source.x * scale_x, command.source.y * scale_y,
        command.source.width * scale_x, command.source.height * scale_y, command.destination.x, command.destination.y,
        command.destination.width, command.destination.height, command.opacity, frame->Generation(),
        static_cast<jint>(command.sampling)
    );
    return;
  }
  if (backdrop_drawing_) {
    throw std::logic_error("HuxerUI Android backdrop effects cannot sample GPU texture layers");
  }
  std::shared_ptr<const AndroidGpuFrame> frame = GpuFrameFor(command.texture);
  if (texture_layers_ == nullptr) {
    throw std::logic_error("HuxerUI Android GPU texture layer host is unavailable");
  }
  texture_layers_->Draw(environment, canvas, key, command, frame);
}

const AndroidBitmapFrame* AndroidRenderer::BitmapFrameFor(const std::shared_ptr<ExternalTexture>& texture) {
  const std::shared_ptr<android::BitmapTexture> bitmap_texture =
      std::dynamic_pointer_cast<android::BitmapTexture>(texture);
  if (!bitmap_texture) {
    throw std::logic_error("HuxerUI external texture is incompatible with the Android Canvas renderer");
  }
  auto cached = std::ranges::find_if(external_textures_, [&bitmap_texture](const CachedExternalTexture& entry) {
    return entry.texture.lock() == bitmap_texture;
  });
  if (cached == external_textures_.end()) {
    external_textures_.push_back(
        CachedExternalTexture{bitmap_texture, {}, {}, std::numeric_limits<std::uint64_t>::max()}
    );
    cached = external_textures_.end() - 1;
  }
  if (cached->draw_epoch != draw_epoch_) {
    const std::shared_ptr<const AndroidBitmapFrame> frame = bitmap_texture->AcquireFrame();
    if (frame) {
      cached->bitmap_frame = frame;
    }
    cached->draw_epoch = draw_epoch_;
  }
  return cached->bitmap_frame.get();
}

std::shared_ptr<const AndroidGpuFrame> AndroidRenderer::GpuFrameFor(const std::shared_ptr<ExternalTexture>& texture) {
  auto cached = std::ranges::find_if(external_textures_, [&texture](const CachedExternalTexture& entry) {
    return entry.texture.lock() == texture;
  });
  if (cached == external_textures_.end()) {
    external_textures_.push_back(CachedExternalTexture{texture, {}, {}, std::numeric_limits<std::uint64_t>::max()});
    cached = external_textures_.end() - 1;
  }
  if (cached->draw_epoch != draw_epoch_) {
    std::shared_ptr<const AndroidGpuFrame> frame;
    if (const auto gl_texture = std::dynamic_pointer_cast<android::GlTexture>(texture)) {
      frame = gl_texture->AcquireFrame();
    } else if (const auto stream_texture = std::dynamic_pointer_cast<android::SurfaceStreamTexture>(texture)) {
      frame = stream_texture->AcquireFrame();
    } else {
      throw std::logic_error("HuxerUI external texture is incompatible with the Android renderer");
    }
    if (frame) {
      cached->gpu_frame = std::move(frame);
    }
    cached->draw_epoch = draw_epoch_;
  }
  return cached->gpu_frame;
}

void AndroidRenderer::RenderCommand(
    JNIEnv* environment, jobject view, jobject canvas, const DrawCircleCommand& command
) {
  environment->CallVoidMethod(
      view,
      draw_circle_,
      canvas,
      command.center.x,
      command.center.y,
      command.radius,
      PackColor(command.color)
  );
}

void AndroidRenderer::RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawLineCommand& command) {
  jfloatArray dashes =
      command.style.dash_pattern.empty() ? nullptr : ToFloatArray(environment, command.style.dash_pattern);
  if (!command.style.dash_pattern.empty() && dashes == nullptr) {
    return;
  }
  environment->CallVoidMethod(
      view,
      draw_line_,
      canvas,
      command.start.x,
      command.start.y,
      command.end.x,
      command.end.y,
      PackColor(command.color),
      command.style.width,
      static_cast<jint>(command.style.cap),
      static_cast<jint>(command.style.join),
      command.style.miter_limit,
      dashes,
      command.style.dash_offset
  );
  if (dashes != nullptr) {
    environment->DeleteLocalRef(dashes);
  }
}

void AndroidRenderer::RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const FillPathCommand& command) {
  android::LocalRef<jfloatArray> path(environment, ToPathArray(environment, command.path));
  JavaBrush brush = ToJavaBrush(environment, command.brush_bounds, command.brush);
  if (!path || !brush.IsValid()) {
    return;
  }
  environment->CallVoidMethod(
      view, fill_brush_path_, canvas, path.Get(), static_cast<jint>(command.fill_rule), brush.kind, brush.color,
      brush.geometry.Get(), brush.stops.Get(), brush.colors.Get()
  );
}

void AndroidRenderer::RenderCommand(
    JNIEnv* environment, jobject view, jobject canvas, const StrokePathCommand& command
) {
  android::LocalRef<jfloatArray> path(environment, ToPathArray(environment, command.path));
  JavaBrush brush = ToJavaBrush(environment, command.brush_bounds, command.brush);
  android::LocalRef<jfloatArray> dashes(
      environment,
      command.style.dash_pattern.empty() ? nullptr : ToFloatArray(environment, command.style.dash_pattern)
  );
  if (!path || !brush.IsValid() || (!command.style.dash_pattern.empty() && !dashes)) {
    return;
  }
  environment->CallVoidMethod(
      view, stroke_brush_path_, canvas, path.Get(), brush.kind, brush.color, brush.geometry.Get(), brush.stops.Get(),
      brush.colors.Get(), command.style.width,
      static_cast<jint>(command.style.cap), static_cast<jint>(command.style.join), command.style.miter_limit,
      dashes.Get(), command.style.dash_offset
  );
}

void AndroidRenderer::RenderCommand(
    JNIEnv* environment, jobject view, jobject canvas, const DrawPathShadowCommand& command
) {
  jfloatArray path = ToPathArray(environment, command.path);
  if (path == nullptr) {
    return;
  }
  const Rect bounds = command.path.Bounds();
  environment->CallVoidMethod(
      view,
      draw_path_shadow_,
      canvas,
      path,
      bounds.x,
      bounds.y,
      bounds.width,
      bounds.height,
      PackColor(command.color),
      command.offset.x,
      command.offset.y,
      command.blur_radius,
      static_cast<jint>(command.fill_rule)
  );
  environment->DeleteLocalRef(path);
}

void AndroidRenderer::RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawArcCommand& command) {
  jfloatArray dashes =
      command.style.dash_pattern.empty() ? nullptr : ToFloatArray(environment, command.style.dash_pattern);
  if (!command.style.dash_pattern.empty() && dashes == nullptr) {
    return;
  }
  environment->CallVoidMethod(
      view,
      draw_arc_,
      canvas,
      command.center.x,
      command.center.y,
      command.radius,
      command.start_angle * kRadiansToDegrees,
      command.sweep_angle * kRadiansToDegrees,
      PackColor(command.color),
      command.style.width,
      static_cast<jint>(command.style.cap),
      static_cast<jint>(command.style.join),
      command.style.miter_limit,
      dashes,
      command.style.dash_offset
  );
  if (dashes != nullptr) {
    environment->DeleteLocalRef(dashes);
  }
}

void AndroidRenderer::RenderCommand(
    JNIEnv* environment, jobject view, jobject canvas, const DrawBorderCommand& command
) {
  if (!command.style.dash_pattern.empty() || command.style.join != StrokeJoin::Miter ||
      command.style.miter_limit != 4.0F) {
    RenderCommand(environment, view, canvas,
                  StrokePathCommand{
                      CreateBorderStrokePath(command.rect, CornerRadii{command.corner_radius}, command.style.width),
                      command.color,
                      command.rect,
                      command.style,
                  });
    return;
  }
  environment->CallVoidMethod(
      view,
      draw_border_,
      canvas,
      command.rect.x,
      command.rect.y,
      command.rect.width,
      command.rect.height,
      PackColor(command.color),
      command.style.width,
      command.corner_radius
  );
}

void AndroidRenderer::RenderCommand(
    JNIEnv* environment, jobject view, jobject canvas, const DrawShadowCommand& command
) {
  const ResolvedShadow resolved = ResolveShadow(command);
  if (resolved.IsEmpty() || command.color.alpha <= 0.0F) {
    return;
  }
  environment->CallVoidMethod(
      view,
      draw_shadow_,
      canvas,
      resolved.caster.x,
      resolved.caster.y,
      resolved.caster.width,
      resolved.caster.height,
      PackColor(command.color),
      command.blur_radius,
      resolved.corner_radius
  );
}

void AndroidRenderer::RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const PushClipCommand& command) {
  if (backdrop_gpu_drawing_) { raster_clips_.push_back({command, BackdropTransform(environment, view, canvas)}); }

  environment->CallVoidMethod(
      view,
      push_clip_,
      canvas,
      command.rect.x,
      command.rect.y,
      command.rect.width,
      command.rect.height,
      command.corner_radius
  );
}

void AndroidRenderer::RenderCommand(
    JNIEnv* environment, jobject view, jobject canvas, const PushPathClipCommand& command
) {
  if (backdrop_gpu_drawing_) { raster_clips_.push_back({command, BackdropTransform(environment, view, canvas)}); }

  jfloatArray path = ToPathArray(environment, command.path);
  if (path == nullptr) {
    return;
  }
  environment->CallVoidMethod(view, push_path_clip_, canvas, path, static_cast<jint>(command.fill_rule));
  environment->DeleteLocalRef(path);
}

void AndroidRenderer::RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const PopClipCommand&) {
  if (backdrop_gpu_drawing_ && !raster_clips_.empty()) { raster_clips_.pop_back(); }
  environment->CallVoidMethod(view, pop_clip_, canvas);
}

void AndroidRenderer::RenderCommand(JNIEnv* environment, jobject view, jobject canvas,
                                    const DrawBackdropCommand& command) {
  if (native_backdrop_drawing_) {
    std::vector<jfloat> shapes;
    shapes.reserve(command.shapes.size() * 8);
    for (const auto& shape : command.shapes) {
      const auto& rect = shape.rect;
      const auto radii = NormalizeCornerRadii(rect, shape.corner_radii);
      shapes.insert(shapes.end(), {rect.x, rect.y, rect.width, rect.height, radii.top_left, radii.top_right,
                                   radii.bottom_right, radii.bottom_left});
    }
    android::LocalRef<jfloatArray> data{environment, environment->NewFloatArray(static_cast<jsize>(shapes.size()))};
    if (data && !environment->ExceptionCheck()) {
      environment->SetFloatArrayRegion(data.Get(), 0, static_cast<jsize>(shapes.size()), shapes.data());
      if (!environment->ExceptionCheck()) {
        const auto& effect = command.effect;
        const jfloat values[] = {effect.blur_radius, effect.refraction, effect.edge_width, effect.highlight,
                                 effect.light_direction.x, effect.light_direction.y, effect.saturation,
                                 effect.brightness, effect.fusion_radius};
        android::LocalRef<jfloatArray> material{environment, environment->NewFloatArray(9)};
        if (material && !environment->ExceptionCheck()) {
          environment->SetFloatArrayRegion(material.Get(), 0, 9, values);
          if (!environment->ExceptionCheck()) {
            environment->CallVoidMethod(view, draw_native_backdrop_, canvas, material.Get(), data.Get());
          }
        }
      }
    }
    return;
  }
  if (backdrop_gpu_drawing_) {
    const Transform2D transform = BackdropTransform(environment, view, canvas);
    if (environment->ExceptionCheck() || !InverseTransform(transform)) {
      return;
    }
    const Rect bounds = TransformBounds(transform, BackdropSamplingBounds(command));
    const Rect sampling{bounds.x - 3.0F, bounds.y - 3.0F, bounds.width + 6.0F, bounds.height + 6.0F};
    const bool needs_raster =
        std::any_of(deferred_raster_.begin(), deferred_raster_.end(),
                    [&](const DeferredRaster& raster) { return sampling.Intersects(raster.bounds); });
    if (needs_raster || !backdrop_planes_.TryAdd(command, transform, 3.0F)) {
      FlushBackdropPlane(environment, view, canvas);
      if (environment->ExceptionCheck()) {
        return;
      }
      backdrop_planes_.TryAdd(command, transform, 3.0F);
    }
    return;
  }
  android::LocalRef<jfloatArray> state{
      environment, static_cast<jfloatArray>(environment->CallObjectMethod(view, backdrop_transform_, canvas))};
  if (!state || environment->ExceptionCheck()) {
    return;
  }
  jfloat values[8];
  environment->GetFloatArrayRegion(state.Get(), 0, 8, values);
  if (environment->ExceptionCheck() || values[0] <= 0.0F || values[1] <= 0.0F) {
    return;
  }
  Transform2D pixel_to_local{values[2], values[3], values[4], values[5], values[6], values[7]};
  const Rect sampling = BackdropSamplingBounds(command);
  const Point corners[] = {{sampling.x, sampling.y},
                           {sampling.x + sampling.width, sampling.y},
                           {sampling.x, sampling.y + sampling.height},
                           {sampling.x + sampling.width, sampling.y + sampling.height}};
  float left = values[0];
  float top = values[1];
  float right = 0.0F;
  float bottom = 0.0F;
  for (const Point corner : corners) {
    const auto pixel = pixel_to_local.Inverse(corner);
    if (!pixel) {
      return;
    }
    left = std::min(left, pixel->x);
    top = std::min(top, pixel->y);
    right = std::max(right, pixel->x);
    bottom = std::max(bottom, pixel->y);
  }
  const jint capture_left = static_cast<jint>(std::floor(std::clamp(left - 3.0F, 0.0F, values[0])));
  const jint capture_top = static_cast<jint>(std::floor(std::clamp(top - 3.0F, 0.0F, values[1])));
  const jint capture_width = static_cast<jint>(std::ceil(std::clamp(right + 3.0F, 0.0F, values[0]))) - capture_left;
  const jint capture_height = static_cast<jint>(std::ceil(std::clamp(bottom + 3.0F, 0.0F, values[1]))) - capture_top;
  if (capture_width <= 0 || capture_height <= 0) {
    return;
  }
  const Point capture_origin =
      pixel_to_local.Apply({static_cast<float>(capture_left), static_cast<float>(capture_top)});
  pixel_to_local.translate_x = capture_origin.x;
  pixel_to_local.translate_y = capture_origin.y;
  android::LocalRef<jintArray> pixels{
      environment, static_cast<jintArray>(environment->CallObjectMethod(view, backdrop_pixels_, canvas, capture_left,
                                                                        capture_top, capture_width, capture_height))};
  if (!pixels || environment->ExceptionCheck()) {
    return;
  }
  const std::size_t width = static_cast<std::size_t>(capture_width);
  const std::size_t height = static_cast<std::size_t>(capture_height);
  const std::size_t count = width * height;
  if (static_cast<std::size_t>(environment->GetArrayLength(pixels.Get())) != count) {
    throw std::logic_error("HuxerUI Android backdrop capture dimensions do not match its pixels");
  }
  std::vector<jint> argb(count);
  environment->GetIntArrayRegion(pixels.Get(), 0, static_cast<jsize>(count), argb.data());
  if (environment->ExceptionCheck()) {
    return;
  }
  std::vector<std::uint8_t> bgra(count * 4);
  for (std::size_t index = 0; index < count; ++index) {
    const auto color = static_cast<std::uint32_t>(argb[index]);
    const unsigned alpha = color >> 24U;
    for (unsigned channel = 0; channel < 3; ++channel) {
      bgra[index * 4 + channel] = static_cast<std::uint8_t>(((color >> (channel * 8U)) & 255U) * alpha / 255U);
    }
    bgra[index * 4 + 3] = static_cast<std::uint8_t>(alpha);
  }
  const BackdropPixels filtered = ApplyBackdropEffect(bgra, width, height, width * 4, pixel_to_local, command);
  for (std::size_t index = 0; index < count; ++index) {
    const unsigned alpha = filtered.bgra[index * 4 + 3];
    std::uint32_t color = alpha << 24U;
    for (unsigned channel = 0; channel < 3; ++channel) {
      const unsigned straight = alpha == 0 ? 0 : std::min(255U, filtered.bgra[index * 4 + channel] * 255U / alpha);
      color |= straight << (channel * 8U);
    }
    argb[index] = static_cast<jint>(color);
  }
  environment->SetIntArrayRegion(pixels.Get(), 0, static_cast<jsize>(count), argb.data());
  if (!environment->ExceptionCheck()) {
    environment->CallVoidMethod(view, draw_backdrop_pixels_, canvas, pixels.Get(), static_cast<jint>(width),
                                static_cast<jint>(height), capture_left, capture_top);
  }
}

void AndroidRenderer::RenderCommand(
    JNIEnv* environment, jobject view, jobject canvas, const PushTransformCommand& command
) {
  environment->CallVoidMethod(
      view,
      push_transform_,
      canvas,
      command.transform.m11,
      command.transform.m12,
      command.transform.m21,
      command.transform.m22,
      command.transform.translate_x,
      command.transform.translate_y
  );
}

void AndroidRenderer::RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const PopTransformCommand&) {
  environment->CallVoidMethod(view, pop_transform_, canvas);
}

void AndroidRenderer::RenderCommand(
    JNIEnv* environment, jobject view, jobject canvas, const PlacePlatformViewCommand& command
) {
  static_cast<void>(environment);
  static_cast<void>(view);
  static_cast<void>(canvas);
  static_cast<void>(command);
}

void AndroidRenderer::DrawSlice(JNIEnv* environment, jobject view, jobject canvas, const RenderFrame& frame,
                                std::size_t first_command, std::size_t command_count) {
  if (environment->ExceptionCheck()) return;
  DrawSliceAttempt(environment, view, canvas, frame, first_command, command_count, true);
  android::LocalRef<jthrowable> pending{environment, environment->ExceptionOccurred()};
  if (!pending) return;
  environment->ExceptionClear();
  const jint reason = environment->CallIntMethod(view, native_backdrop_fallback_, pending.Get());
  if (environment->ExceptionCheck()) {
    environment->ExceptionClear();
    environment->Throw(pending.Get());
    return;
  }
  if (reason == 0) {
    environment->Throw(pending.Get());
    return;
  }
  // The failed native attempt never published its root, so the original Canvas is still untouched.
  DrawSliceAttempt(environment, view, canvas, frame, first_command, command_count, false);
  backdrop_stats_.native_fallback_reason = reason;
  if (!environment->ExceptionCheck() && cached_root_ == frame.scene.root && cached_revision_ == frame.revision) {
    cached_native_fallback_ = reason;
  }
}

void AndroidRenderer::DrawSliceAttempt(JNIEnv* environment, jobject view, jobject canvas, const RenderFrame& frame,
                                       std::size_t first_command, std::size_t command_count, bool allow_native) {
  const std::uint64_t frame_number = ++backdrop_frame_number_;
  AndroidBackdropTrace trace("HuxerUI Backdrop Frame", frame_number);
  backdrop_stats_ = {};
  ValidateBackdropSceneSlice(frame.scene, first_command, command_count);
  if (frame.scene.root != nullptr && command_count > 0) {
    backdrop_drawing_ = RenderSceneHasBackdropEffects(frame.scene.root);
    if (!backdrop_drawing_) {
      cached_root_ = nullptr;
      raster_programs_.clear();
      backdrop_gl_.reset();
    }
    const std::size_t end = command_count > std::numeric_limits<std::size_t>::max() - first_command
                                ? std::numeric_limits<std::size_t>::max()
                                : first_command + command_count;
    CommandRange range{first_command, end, 0};
    android::LocalRef<jobject> backdrop_canvas;
    const auto release = [&] {
      android::LocalRef<jthrowable> pending{environment, environment->ExceptionOccurred()};
      if (pending) {
        environment->ExceptionClear();
      }
      if (backdrop_gl_) {
        backdrop_gl_->CancelFrame();
      }
      environment->CallVoidMethod(view, release_backdrop_canvases_);
      if (pending) {
        if (environment->ExceptionCheck()) {
          environment->ExceptionClear();
        }
        environment->Throw(pending.Get());
      }
      backdrop_planes_.Clear();
      deferred_raster_.clear();
      raster_operations_.clear();
      raster_clips_.clear();
      track_raster_ = false;
      backdrop_gpu_drawing_ = false;
      backdrop_raster_dirty_ = false;
      backdrop_drawing_ = false;
      native_backdrop_drawing_ = false;
    };
    try {
      jobject target = canvas;
      if (backdrop_drawing_) {
        const jint native_capability = allow_native
            ? environment->CallIntMethod(view, native_backdrop_available_, canvas) : 0;
        native_backdrop_drawing_ = native_capability >= 33 && !environment->ExceptionCheck() &&
                                  NativeBlurSceneEligible(*frame.scene.root, native_capability >= 35);
        const bool eligible = !native_backdrop_drawing_ && !backdrop_gl_unavailable_ &&
                              environment->CallBooleanMethod(view, backdrop_gpu_available_, canvas) == JNI_TRUE;
        const std::string previous_environment = raster_environment_;
        android::LocalRef<jstring> configuration{
            environment, static_cast<jstring>(environment->CallObjectMethod(view, raster_environment_method_))};
        if (configuration && !environment->ExceptionCheck()) {
          const char* text = environment->GetStringUTFChars(configuration.Get(), nullptr);
          if (text) {
            raster_environment_ = text;
            environment->ReleaseStringUTFChars(configuration.Get(), text);
          }
        }
        if ((eligible || (native_backdrop_drawing_ && cached_native_fallback_ != 0)) &&
            !environment->ExceptionCheck() && cached_root_ == frame.scene.root &&
            cached_revision_ == frame.revision && previous_environment == raster_environment_ &&
            !HasMutableRasterSource(*frame.scene.root)) {
          android::LocalRef<jfloatArray> state{
              environment, static_cast<jfloatArray>(environment->CallObjectMethod(view, backdrop_transform_, canvas))};
          jfloat values[8]{};
          if (state && !environment->ExceptionCheck()) {
            environment->GetFloatArrayRegion(state.Get(), 0, 8, values);
            const Transform2D mapping{values[2], values[3], values[4], values[5], values[6], values[7]};
            if (!environment->ExceptionCheck() && values[0] == cached_width_ && values[1] == cached_height_ &&
                mapping == cached_mapping_) {
              environment->CallVoidMethod(view, redraw_hardware_method_, canvas);
              backdrop_stats_.completed_frame_hit = !environment->ExceptionCheck();
              backdrop_stats_.native_fallback_reason = cached_native_fallback_;
              backdrop_drawing_ = false;
              native_backdrop_drawing_ = false;
              return;
            }
          }
        }
        cached_root_ = nullptr;
        cached_native_fallback_ = 0;
        if (eligible && !environment->ExceptionCheck()) {
          if (!backdrop_gl_) {
            backdrop_gl_ = std::make_unique<AndroidBackdropGl>();
          }
          if (!backdrop_gl_->TryInitialize()) {
            backdrop_gl_unavailable_ = true;
            backdrop_gl_.reset();
          } else {
            android::LocalRef<jfloatArray> dimensions{
                environment,
                static_cast<jfloatArray>(environment->CallObjectMethod(view, backdrop_transform_, canvas))};
            jfloat size[2]{};
            if (dimensions && !environment->ExceptionCheck()) {
              environment->GetFloatArrayRegion(dimensions.Get(), 0, 2, size);
              if (!environment->ExceptionCheck() && size[0] > 0 && size[1] > 0) {
                backdrop_gpu_drawing_ =
                    backdrop_gl_->BeginFrame(static_cast<std::size_t>(size[0]), static_cast<std::size_t>(size[1]),
                                              frame_number);
                backdrop_frame_bounds_ = {0, 0, size[0], size[1]};
              }
            }
          }
        }
        if (environment->ExceptionCheck()) {
          release();
          return;
        }
        backdrop_canvas = {environment, environment->CallObjectMethod(view,
                                                                      native_backdrop_drawing_
                                                                          ? begin_native_backdrop_canvas_ :
                                                                      backdrop_gpu_drawing_ ? begin_backdrop_gpu_canvas_
                                                                                            : begin_backdrop_canvas_,
                                                                      canvas)};
        backdrop_raster_dirty_ = backdrop_gpu_drawing_;
        backdrop_raster_bounds_ = backdrop_frame_bounds_;
        track_raster_ = backdrop_gpu_drawing_;
        raster_program_index_ = 0;
        raster_program_bytes_ = 0;
        target = backdrop_canvas.Get();
      }
      bool rendered = false;
      if (!environment->ExceptionCheck()) {
        AndroidBackdropTrace replay("HuxerUI Scene Replay");
        rendered = RenderSceneNode(environment, view, target, *frame.scene.root, &range);
      }
      if (rendered && backdrop_drawing_) {
        if (backdrop_gpu_drawing_) {
          FlushBackdropPlane(environment, view, target);
          FlushBackdropRaster(environment, view, target);
          raster_programs_.resize(raster_program_index_);
          if (!environment->ExceptionCheck()) {
            auto buffer = backdrop_gl_->FinishFrame(environment);
            backdrop_stats_.gpu = backdrop_gl_->FrameStats();
#if defined(HUXERUI_ENABLE_PROFILING) && HUXERUI_ENABLE_PROFILING
            if (ATrace_isEnabled()) {
              const auto& stats = backdrop_stats_.gpu;
              __android_log_print(
                  ANDROID_LOG_INFO, "HuxerUI",
                  "Backdrop work frame=%llu planes=%zu commands=%zu captures=%llu blur_passes=%llu blur_pixels=%llu "
                  "imports=%llu raster_hits=%llu composite_pixels=%llu cache_copy_pixels=%llu "
                  "upload_bytes=%llu mask_bytes=%llu readback_bytes=%llu blur_hits=%llu",
                  static_cast<unsigned long long>(frame_number),
                  backdrop_stats_.plane_count, backdrop_stats_.command_count,
                  static_cast<unsigned long long>(stats.source_captures),
                  static_cast<unsigned long long>(stats.blur_passes),
                  static_cast<unsigned long long>(stats.blur_pixels),
                  static_cast<unsigned long long>(stats.raster_imports),
                  static_cast<unsigned long long>(stats.raster_cache_hits),
                  static_cast<unsigned long long>(stats.raster_composite_pixels),
                  static_cast<unsigned long long>(stats.raster_cache_copy_pixels),
                  static_cast<unsigned long long>(stats.raster_upload_bytes),
                  static_cast<unsigned long long>(stats.mask_upload_bytes),
                  static_cast<unsigned long long>(stats.readback_bytes),
                  static_cast<unsigned long long>(stats.blur_cache_hits));
              __android_log_print(ANDROID_LOG_INFO, "HuxerUI",
                                  "Backdrop work frame=%llu capture_resize_ns=%llu source_prepare_ns=%llu "
                                  "kernel_resize_ns=%llu blur_submit_ns=%llu mask_upload_ns=%llu",
                                  static_cast<unsigned long long>(frame_number),
                                  static_cast<unsigned long long>(stats.capture_resize_cpu_ns),
                                  static_cast<unsigned long long>(stats.source_prepare_cpu_ns),
                                  static_cast<unsigned long long>(stats.kernel_resize_cpu_ns),
                                  static_cast<unsigned long long>(stats.blur_submit_cpu_ns),
                                  static_cast<unsigned long long>(stats.mask_upload_cpu_ns));
              __android_log_print(ANDROID_LOG_INFO, "HuxerUI",
                                  "Backdrop work frame=%llu source_miss_reasons=%llu blur_miss_reasons=%llu "
                                  "rect_clips=%zu mask_clips=%zu compute_passes=%llu "
                                  "stable_full_frame=%llu clip_fallback=%llu cache_allocations=%llu cache_reuses=%llu",
                                  static_cast<unsigned long long>(frame_number),
                                  static_cast<unsigned long long>(stats.source_miss_reasons),
                                  static_cast<unsigned long long>(stats.blur_miss_reasons),
                                  backdrop_stats_.rectangular_clip_planes, backdrop_stats_.mask_clip_planes,
                                  static_cast<unsigned long long>(stats.compute_blur_passes),
                                  static_cast<unsigned long long>(stats.stable_full_frame_planes),
                                  static_cast<unsigned long long>(backdrop_stats_.clip_fallback_reasons),
                                  static_cast<unsigned long long>(stats.cache_texture_allocations),
                                  static_cast<unsigned long long>(stats.cache_texture_reuses));
              __android_log_print(ANDROID_LOG_INFO, "HuxerUI",
                                  "Backdrop work frame=%llu kernel_source_copy_pixels=%llu "
                                  "kernel_source_reuses=%llu blur_result_adoptions=%llu integer_blur_passes=%llu",
                                  static_cast<unsigned long long>(frame_number),
                                  static_cast<unsigned long long>(stats.kernel_source_copy_pixels),
                                  static_cast<unsigned long long>(stats.kernel_source_reuses),
                                  static_cast<unsigned long long>(stats.blur_result_adoptions),
                                  static_cast<unsigned long long>(stats.integer_blur_passes));
            }
#endif
            if (buffer && !environment->ExceptionCheck()) {
              AndroidBackdropTrace publication("HuxerUI Bitmap Publication");
              environment->CallVoidMethod(view, present_backdrop_hardware_, canvas, buffer.Get());
              if (!environment->ExceptionCheck()) {
                android::LocalRef<jfloatArray> state{
                    environment,
                    static_cast<jfloatArray>(environment->CallObjectMethod(view, backdrop_transform_, canvas))};
                jfloat values[8]{};
                if (state && !environment->ExceptionCheck()) {
                  environment->GetFloatArrayRegion(state.Get(), 0, 8, values);
                  cached_root_ = frame.scene.root;
                  cached_revision_ = frame.revision;
                  cached_width_ = values[0];
                  cached_height_ = values[1];
                  cached_mapping_ = {values[2], values[3], values[4], values[5], values[6], values[7]};
                }
              }
            }
          }
        } else {
          environment->CallVoidMethod(view, end_backdrop_canvas_, canvas, target, 1.0F);
        }
      }
      backdrop_stats_.native_rendered = rendered && native_backdrop_drawing_ && !environment->ExceptionCheck();
    } catch (...) {
      release();
      throw;
    }
    release();
  }
}

Transform2D AndroidRenderer::BackdropTransform(JNIEnv* environment, jobject view, jobject canvas) {
  android::LocalRef<jfloatArray> state{
      environment, static_cast<jfloatArray>(environment->CallObjectMethod(view, backdrop_transform_, canvas))};
  if (!state || environment->ExceptionCheck()) {
    return Transform2D{0, 0, 0, 0, 0, 0};
  }
  jfloat values[8]{};
  environment->GetFloatArrayRegion(state.Get(), 0, 8, values);
  const auto transform =
      InverseTransform(Transform2D{values[2], values[3], values[4], values[5], values[6], values[7]});
  return transform.value_or(Transform2D{0, 0, 0, 0, 0, 0});
}

void AndroidRenderer::DrawRasterCommand(JNIEnv* environment, jobject view, jobject canvas,
                                        const PaintCommand& command) {
  RecordRaster(environment, view, canvas, command);
  std::visit(
      [&](const auto& value) {
        if constexpr (!std::same_as<std::remove_cvref_t<decltype(value)>, DrawExternalTextureCommand>) {
          RenderCommand(environment, view, canvas, value);
        }
      },
      command);
}

void AndroidRenderer::RecordRaster(JNIEnv* environment, jobject view, jobject canvas, const PaintCommand& command) {
  const Transform2D mapping = BackdropTransform(environment, view, canvas);
  if (std::holds_alternative<DrawTextRunsCommand>(command)) {
    // Positioned run bounds need not contain every glyph's ink.
    backdrop_raster_bounds_ = backdrop_frame_bounds_;
  } else if (const auto local = ConservativeRasterBounds(command)) {
    const Rect output = TransformBounds(mapping, *local);
    if (!std::isfinite(output.x) || !std::isfinite(output.y) || !std::isfinite(output.width) ||
        !std::isfinite(output.height) || !std::isfinite(output.x + output.width) ||
        !std::isfinite(output.y + output.height)) {
      backdrop_raster_bounds_ = backdrop_frame_bounds_;
    } else {
      const float left = std::clamp(std::floor(output.x - 3), 0.0F, backdrop_frame_bounds_.width);
      const float top = std::clamp(std::floor(output.y - 3), 0.0F, backdrop_frame_bounds_.height);
      const float right = std::clamp(std::ceil(output.x + output.width + 3), 0.0F, backdrop_frame_bounds_.width);
      const float bottom = std::clamp(std::ceil(output.y + output.height + 3), 0.0F, backdrop_frame_bounds_.height);
      if (right > left && bottom > top) {
        if (backdrop_raster_bounds_.IsEmpty()) {
          backdrop_raster_bounds_ = {left, top, right - left, bottom - top};
        } else {
          const float union_left = std::min(left, backdrop_raster_bounds_.x);
          const float union_top = std::min(top, backdrop_raster_bounds_.y);
          backdrop_raster_bounds_ = {
              union_left, union_top,
              std::max(right, backdrop_raster_bounds_.x + backdrop_raster_bounds_.width) - union_left,
              std::max(bottom, backdrop_raster_bounds_.y + backdrop_raster_bounds_.height) - union_top};
        }
      }
    }
  }
  if (!track_raster_) {
    return;
  }
  std::size_t bytes = sizeof(RasterOperation) + raster_clips_.size() * sizeof(RasterClip);
  bool immutable = std::visit(
      [&](const auto& value) {
        using Command = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::same_as<Command, DrawImageCommand> || std::same_as<Command, DrawExternalTextureCommand>) {
          return false;
        }
        if constexpr (requires { value.path; }) {
          return false;
        }
        if constexpr (requires { value.style.dash_pattern; }) {
          bytes += value.style.dash_pattern.size() * sizeof(float);
        }
        if constexpr (requires { value.brush; }) {
          std::visit(
              [&](const auto& brush) {
                if constexpr (requires { brush.stops; }) {
                  bytes += brush.stops.size() * sizeof(GradientStop);
                }
              },
              value.brush.Get());
        }
        if constexpr (std::same_as<Command, DrawTextCommand>) {
          bytes += value.text.PlainText().size() + value.text.StyleRanges().size() * sizeof(TextStyleRange) +
                   value.style.font.FamilyName().size() + value.text.LinkRanges().size() * sizeof(TextLinkRange);
          for (const auto& range : value.text.StyleRanges()) {
            if (range.style.font) {
              bytes += range.style.font->FamilyName().size();
            }
          }
          for (const auto& link : value.text.LinkRanges()) {
            bytes += link.target.ToString().size();
          }
        }
        if constexpr (std::same_as<Command, DrawTextRunsCommand>) {
          for (const auto& run : value.runs) {
            bytes += sizeof(TextRun) + run.text.size() + run.style.font.FamilyName().size();
          }
        }
        return true;
      },
      command);
  for (const auto& clip : raster_clips_) {
    if (std::holds_alternative<PushPathClipCommand>(clip.command)) {
      immutable = false;
    }
  }
  if (!immutable || raster_operations_.size() >= 256 || raster_program_index_ >= 16 ||
      bytes > 4U * 1024U * 1024U - raster_program_bytes_) {
    track_raster_ = false;
    raster_operations_.clear();
    return;
  }
  raster_program_bytes_ += bytes;
  raster_operations_.push_back({command, mapping, raster_clips_});
}

std::uint64_t AndroidRenderer::RasterIdentity() {
  raster_program_reused_ = false;
  if (!track_raster_ || next_raster_identity_ == std::numeric_limits<std::uint64_t>::max()) {
    raster_operations_.clear();
    return 0;
  }
  const std::size_t index = raster_program_index_++;
  if (index < raster_programs_.size() && raster_programs_[index].operations == raster_operations_ &&
      raster_programs_[index].environment == raster_environment_) {
    raster_program_reused_ = true;
    raster_operations_.clear();
    return raster_programs_[index].identity;
  }
  if (index >= raster_programs_.size()) {
    raster_programs_.resize(index + 1);
  }
  auto& program = raster_programs_[index];
  program = {std::move(raster_operations_), raster_environment_, next_raster_identity_++};
  raster_operations_.clear();
  return program.identity;
}

void AndroidRenderer::FlushBackdropRaster(JNIEnv* environment, jobject view, jobject canvas) {
  if (!backdrop_gpu_drawing_ || !backdrop_raster_dirty_ || environment->ExceptionCheck()) {
    return;
  }
  const auto clear = [&] {
    environment->CallVoidMethod(view, clear_backdrop_raster_, canvas);
    backdrop_raster_dirty_ = false;
    backdrop_raster_bounds_ = {};
  };
  if (backdrop_raster_bounds_.IsEmpty()) {
    clear();
    return;
  }
  const std::uint64_t identity = RasterIdentity();
  if (backdrop_gl_->CompositeCachedRaster(identity)) {
    clear();
    return;
  }
  AndroidBackdropTrace raster_trace("HuxerUI Raster Capture");
  // Stable size classes absorb small scroll translations without creating a reader for every pixel extent.
  const float width = std::min(backdrop_frame_bounds_.width, std::ceil((backdrop_raster_bounds_.width + 63) / 64) * 64);
  const float height =
      std::min(backdrop_frame_bounds_.height, std::ceil((backdrop_raster_bounds_.height + 63) / 64) * 64);
  const Rect bounds{
      std::clamp(std::floor(backdrop_raster_bounds_.x / 64) * 64, 0.0F, backdrop_frame_bounds_.width - width),
      std::clamp(std::floor(backdrop_raster_bounds_.y / 64) * 64, 0.0F, backdrop_frame_bounds_.height - height), width,
      height};
  backdrop_gl_->PrepareHardwareRaster(environment);
  android::LocalRef<jobject> bitmap{
      environment, environment->CallObjectMethod(view, backdrop_bitmap_, canvas, static_cast<jint>(bounds.x),
                                                 static_cast<jint>(bounds.y), static_cast<jint>(bounds.width),
                                                 static_cast<jint>(bounds.height))};
  if (!bitmap || environment->ExceptionCheck()) {
    return;
  }
  backdrop_gl_->CompositeHardwareRaster(environment, bitmap.Get(), identity, bounds, raster_program_reused_);
  clear();
}

std::optional<Rect> AndroidRenderer::RectangularBackdropClip() {
  Rect result = backdrop_frame_bounds_;
  for (const RasterClip& entry : raster_clips_) {
    const auto* clip = std::get_if<PushClipCommand>(&entry.command);
    if (clip == nullptr || clip->corner_radius != 0.0F || entry.transform.m12 != 0.0F || entry.transform.m21 != 0.0F) {
      backdrop_stats_.clip_fallback_reasons |= clip == nullptr ? 1 : clip->corner_radius != 0.0F ? 2 : 4;
      return std::nullopt;
    }
    const Rect bounds = TransformBounds(entry.transform, clip->rect);
    for (float value : {bounds.x, bounds.y, bounds.x + bounds.width, bounds.y + bounds.height}) {
      if (!std::isfinite(value) || std::floor(value) != value) {
        backdrop_stats_.clip_fallback_reasons |= 8;
        return std::nullopt;
      }
    }
    result = result.Intersection(bounds);
  }
  return result.IsEmpty() ? Rect{} : result;
}

Rect AndroidRenderer::BackdropVisibleBounds() const {
  Rect result = backdrop_frame_bounds_;
  for (const RasterClip& entry : raster_clips_) {
    Rect local;
    if (const auto* clip = std::get_if<PushClipCommand>(&entry.command)) {
      local = clip->rect;
    } else if (const auto* clip = std::get_if<PushPathClipCommand>(&entry.command)) {
      local = clip->path.Bounds();
    } else {
      return backdrop_frame_bounds_;
    }
    const Rect bounds = TransformBounds(entry.transform, local);
    for (float edge : {bounds.x, bounds.y, bounds.x + bounds.width, bounds.y + bounds.height}) {
      if (!std::isfinite(edge)) {
        return backdrop_frame_bounds_;
      }
    }
    // This bounds work only; the original mask still determines fractional and curved coverage.
    const float left = std::floor(bounds.x) - 3;
    const float top = std::floor(bounds.y) - 3;
    const float right = std::ceil(bounds.x + bounds.width) + 3;
    const float bottom = std::ceil(bounds.y + bounds.height) + 3;
    result = result.Intersection({left, top, right - left, bottom - top});
  }
  return result.IsEmpty() ? Rect{} : result;
}

android::LocalRef<jobject> AndroidRenderer::BackdropMask(JNIEnv* environment, jobject view, jobject canvas,
                                                       const std::vector<RasterClip>& clips) {
  android::LocalRef<jfloatArray> key;
  if (!clips.empty() && clips.size() <= 64 &&
      std::all_of(clips.begin(), clips.end(), [](const auto& clip) {
        return std::holds_alternative<PushClipCommand>(clip.command);
      })) {
    std::vector<jfloat> values;
    values.reserve(clips.size() * 11);
    for (const auto& clip : clips) {
      const auto& command = std::get<PushClipCommand>(clip.command);
      const auto& transform = clip.transform;
      values.insert(values.end(), {command.rect.x, command.rect.y, command.rect.width, command.rect.height,
                                    command.corner_radius, transform.m11, transform.m12, transform.m21, transform.m22,
                                    transform.translate_x, transform.translate_y});
    }
    key = {environment, environment->NewFloatArray(static_cast<jsize>(values.size()))};
    if (key && !environment->ExceptionCheck()) {
      environment->SetFloatArrayRegion(key.Get(), 0, static_cast<jsize>(values.size()), values.data());
    }
    if (environment->ExceptionCheck()) {
      return {};
    }
  }
  return {environment, environment->CallObjectMethod(view, backdrop_mask_, canvas, key.Get())};
}

void AndroidRenderer::FlushBackdropPlane(JNIEnv* environment, jobject view, jobject canvas) {
  if (backdrop_planes_.empty() || environment->ExceptionCheck()) {
    return;
  }
  const BackdropPlaneBatch batch = backdrop_planes_.batch();
  backdrop_planes_.Clear();
  FlushBackdropRaster(environment, view, canvas);
  if (environment->ExceptionCheck()) {
    return;
  }
  const auto clip = RectangularBackdropClip();
  android::LocalRef<jobject> mask;
  if (!clip) {
    mask = BackdropMask(environment, view, canvas, raster_clips_);
    ++backdrop_stats_.mask_clip_planes;
  } else if (!raster_clips_.empty()) {
    ++backdrop_stats_.rectangular_clip_planes;
  }
  if (environment->ExceptionCheck()) {
    return;
  }
  const bool applied = backdrop_gl_->ApplyToFrame(environment, batch, mask.Get(), clip, BackdropVisibleBounds());
  if (environment->ExceptionCheck()) {
    return;
  }
  if (!applied) {
    throw std::logic_error("HuxerUI backdrop blur requires axis-aligned scaling or a uniform orthogonal transform");
  }
  ++backdrop_stats_.plane_count;
  backdrop_stats_.command_count += batch.commands.size();
  const Transform2D original = BackdropTransform(environment, view, canvas);
  const auto set_transform = [&](const Transform2D& transform) {
    environment->CallVoidMethod(view, set_backdrop_raster_transform_, canvas, transform.m11, transform.m12,
                                transform.m21, transform.m22, transform.translate_x, transform.translate_y);
  };
  for (const DeferredRaster& raster : deferred_raster_) {
    set_transform(raster.transform);
    if (environment->ExceptionCheck()) {
      break;
    }
    DrawRasterCommand(environment, view, canvas, *raster.command);
    backdrop_raster_dirty_ = true;
    if (environment->ExceptionCheck()) {
      break;
    }
  }
  if (!environment->ExceptionCheck()) {
    set_transform(original);
  }
  deferred_raster_.clear();
}

// Wire format shared with HuxerUIView.attributedText: ten little-endian 32-bit fields, then UTF-8 font-family bytes.
// Text ranges use UTF-16 offsets, matching Java String indices; the font-family length counts bytes instead.
android::LocalRef<jbyteArray> AndroidTextAttributes(JNIEnv* environment, const AttributedText& text,
                                                    const TextStyle& base) {
  if (text.Length() > std::numeric_limits<jint>::max()) {
    throw std::invalid_argument("HuxerUI Android paragraph exceeds the supported length");
  }
  std::vector<std::byte> bytes;
  const auto append = [&](std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
      bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
    }
  };
  if (!text.StyleRanges().empty()) {
    for (const auto& run : ResolveTextRuns(text, base)) {
      const auto& font = run.style.font;
      append(static_cast<std::uint32_t>(run.range.start));
      append(static_cast<std::uint32_t>(run.range.end));
      append(std::bit_cast<std::uint32_t>(font.Size()));
      append(static_cast<std::uint32_t>(font.FamilyKind()));
      append(static_cast<std::uint32_t>(font.Weight()));
      append(static_cast<std::uint32_t>(font.Slant()));
      append(static_cast<std::uint32_t>(PackColor(run.style.foreground)));
      append(static_cast<std::uint32_t>(PackColor(run.background)));
      append(static_cast<std::uint32_t>(run.style.decoration));
      const auto family = AndroidFontName(font);
      if (family.size() > static_cast<std::size_t>(std::numeric_limits<jint>::max())) {
        throw std::invalid_argument("HuxerUI Android font family exceeds the supported length");
      }
      append(static_cast<std::uint32_t>(family.size()));
      for (const char byte : family) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
      }
    }
  }
  return android::BytesToJavaByteArray(environment, bytes);
}

} // namespace huxerui::detail

extern "C" JNIEXPORT jbyteArray JNICALL
Java_org_huxerui_HuxerUIView_nativeFontBytes(JNIEnv* environment, jclass, jlong identity) {
  if (identity <= 0) return nullptr;
  try {
    const auto face = huxerui::detail::AndroidFontSource(static_cast<std::uint64_t>(identity));
    if (!face) return nullptr;
    return huxerui::android::BytesToJavaByteArray(environment, face->bytes).Release();
  } catch (const std::exception&) {
    return nullptr;
  }
}
