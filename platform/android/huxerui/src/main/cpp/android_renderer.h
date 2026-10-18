#pragma once

#include <jni.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <huxerui/android/jni.h>
#include <huxerui/render_scene.h>
#include <huxerui/text.h>

#include "android_external_texture_internal.h"
#include "android_gl_effects_internal.h"

namespace huxerui::detail {

android::LocalRef<jbyteArray>
AndroidTextAttributes(JNIEnv* environment, const AttributedText& text, const TextStyle& base);

class AndroidTextureLayers;

struct AndroidBackdropStats {
  BackdropGlStats gpu;
  std::size_t plane_count = 0;
  std::size_t command_count = 0;
  bool completed_frame_hit = false;
  int native_fallback_reason = 0;
  bool native_rendered = false;
  std::size_t rectangular_clip_planes = 0;
  std::size_t mask_clip_planes = 0;
  std::uint64_t clip_fallback_reasons = 0;
};

class AndroidRenderer final {
public:
  [[nodiscard]] AndroidBackdropStats BackdropStats() const noexcept {
    return backdrop_stats_;
  }
  void Initialize(JNIEnv* environment, jclass view_class);
  void SetTextureLayers(AndroidTextureLayers* texture_layers) noexcept;
  void BeginDraw();
  void DrawSlice(JNIEnv* environment, jobject view, jobject canvas, const RenderFrame& frame, std::size_t first_command,
                 std::size_t command_count);

private:
  void DrawSliceAttempt(JNIEnv* environment, jobject view, jobject canvas, const RenderFrame& frame,
                        std::size_t first_command, std::size_t command_count, bool allow_native);
  struct CommandRange;
  struct DeferredRaster {
    const PaintCommand* command;
    Transform2D transform;
    Rect bounds;
  };
  Transform2D BackdropTransform(JNIEnv* environment, jobject view, jobject canvas);
  void FlushBackdropPlane(JNIEnv* environment, jobject view, jobject canvas);
  void FlushBackdropRaster(JNIEnv* environment, jobject view, jobject canvas);
  std::optional<Rect> RectangularBackdropClip();
  Rect BackdropVisibleBounds() const;
  void DrawRasterCommand(JNIEnv* environment, jobject view, jobject canvas, const PaintCommand& command);

  struct RasterClip {
    PaintCommand command;
    Transform2D transform;
    bool operator==(const RasterClip&) const = default;
  };
  android::LocalRef<jobject> BackdropMask(JNIEnv* environment, jobject view, jobject canvas,
                                        const std::vector<RasterClip>& clips);
  struct RasterOperation {
    PaintCommand command;
    Transform2D transform;
    std::vector<RasterClip> clips;
    bool operator==(const RasterOperation&) const = default;
  };
  struct RasterProgram {
    std::vector<RasterOperation> operations;
    std::string environment;
    std::uint64_t identity;
  };
  void RecordRaster(JNIEnv* environment, jobject view, jobject canvas, const PaintCommand& command);
  std::uint64_t RasterIdentity();
  std::vector<RasterClip> raster_clips_;
  std::vector<RasterOperation> raster_operations_;
  std::vector<RasterProgram> raster_programs_;
  std::size_t raster_program_index_ = 0;
  std::size_t raster_program_bytes_ = 0;
  std::string raster_environment_;
  std::uint64_t next_raster_identity_ = 1;
  bool track_raster_ = false;
  bool raster_program_reused_ = false;
  const RenderNode* cached_root_ = nullptr;
  int cached_native_fallback_ = 0;
  std::uint64_t cached_revision_ = 0;
  Transform2D cached_mapping_;
  float cached_width_ = 0;
  float cached_height_ = 0;

  struct CachedExternalTexture {
    std::weak_ptr<ExternalTexture> texture;
    std::shared_ptr<const AndroidBitmapFrame> bitmap_frame;
    std::shared_ptr<const AndroidGpuFrame> gpu_frame;
    std::uint64_t draw_epoch = 0;
  };

  bool RenderSequence(JNIEnv* environment, jobject view, jobject canvas, const PaintSequence& sequence,
    std::uint64_t node_identity, bool foreground, CommandRange* range);
  bool RenderSceneNode(JNIEnv* environment, jobject view, jobject canvas, const RenderNode& node, CommandRange* range);
  [[nodiscard]] const AndroidBitmapFrame* BitmapFrameFor(const std::shared_ptr<ExternalTexture>& texture);
  [[nodiscard]] std::shared_ptr<const AndroidGpuFrame> GpuFrameFor(const std::shared_ptr<ExternalTexture>& texture);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawRectCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawTextCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawTextRunsCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawImageCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawExternalTextureCommand& command, const AndroidTextureLayerKey& key);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawCircleCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawLineCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawArcCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawBorderCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawShadowCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawBackdropCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const FillPathCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const StrokePathCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawPathShadowCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const PushClipCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const PushPathClipCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const PopClipCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const PushTransformCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const PopTransformCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const PlacePlatformViewCommand& command);

  jmethodID draw_brush_rect_ = nullptr;
  jmethodID draw_text_ = nullptr;
  jmethodID draw_text_runs_ = nullptr;
  jmethodID draw_image_ = nullptr;
  jmethodID draw_external_texture_ = nullptr;
  jmethodID draw_circle_ = nullptr;
  jmethodID draw_line_ = nullptr;
  jmethodID draw_arc_ = nullptr;
  jmethodID draw_border_ = nullptr;
  jmethodID draw_shadow_ = nullptr;
  jmethodID fill_brush_path_ = nullptr;
  jmethodID stroke_brush_path_ = nullptr;
  jmethodID draw_path_shadow_ = nullptr;
  jmethodID push_clip_ = nullptr;
  jmethodID push_path_clip_ = nullptr;
  jmethodID pop_clip_ = nullptr;
  jmethodID push_opacity_ = nullptr;
  jmethodID pop_opacity_ = nullptr;
  jmethodID push_transform_ = nullptr;
  jmethodID pop_transform_ = nullptr;
  jmethodID native_backdrop_available_ = nullptr;
  jmethodID native_backdrop_fallback_ = nullptr;
  jmethodID begin_native_backdrop_canvas_ = nullptr;
  jmethodID draw_native_backdrop_ = nullptr;
  bool native_backdrop_drawing_ = false;
  jmethodID begin_backdrop_canvas_ = nullptr;
  jmethodID end_backdrop_canvas_ = nullptr;
  jmethodID release_backdrop_canvases_ = nullptr;
  jmethodID backdrop_pixels_ = nullptr;
  jmethodID backdrop_transform_ = nullptr;
  jmethodID draw_backdrop_pixels_ = nullptr;
  jmethodID backdrop_gpu_available_ = nullptr;
  jmethodID raster_environment_method_ = nullptr;
  jmethodID redraw_hardware_method_ = nullptr;
  jmethodID begin_backdrop_gpu_canvas_ = nullptr;
  jmethodID backdrop_bitmap_ = nullptr;
  jmethodID backdrop_mask_ = nullptr;
  jmethodID clear_backdrop_raster_ = nullptr;
  jmethodID discard_backdrop_canvas_ = nullptr;
  jmethodID present_backdrop_hardware_ = nullptr;
  jmethodID set_backdrop_raster_transform_ = nullptr;
  std::unique_ptr<AndroidBackdropGl> backdrop_gl_;
  bool backdrop_gl_unavailable_ = false;
  bool backdrop_gpu_drawing_ = false;
  bool backdrop_raster_dirty_ = false;
  Rect backdrop_raster_bounds_;
  Rect backdrop_frame_bounds_;
  BackdropPlaneCollector backdrop_planes_;
  std::vector<DeferredRaster> deferred_raster_;
  AndroidBackdropStats backdrop_stats_;
  std::vector<CachedExternalTexture> external_textures_;
  AndroidTextureLayers* texture_layers_ = nullptr;
  std::uint64_t draw_epoch_ = 0;
  std::uint64_t backdrop_frame_number_ = 0;
  bool backdrop_drawing_ = false;
};

} // namespace huxerui::detail
