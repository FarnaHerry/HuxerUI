#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "graphics/backdrop_plane_internal.h"

namespace huxerui::detail {

enum class BackdropCacheMiss : std::uint64_t {
  None = 0,
  MissingSource = 1,
  FrameSize = 2,
  SourceInputs = 4,
  CaptureGeometry = 8,
  Budget = 16,
  Ineligible = 32,
  MissingKernel = 64,
  KernelRadius = 128,
  KernelGeometry = 256,
};

struct BackdropGlStats {
  std::uint64_t source_captures = 0;
  std::uint64_t blur_passes = 0;
  std::uint64_t blur_cache_hits = 0;
  std::uint64_t raster_upload_bytes = 0;
  std::uint64_t readback_bytes = 0;
  std::uint64_t raster_imports = 0;
  std::uint64_t mask_upload_bytes = 0;
  std::uint64_t blur_pixels = 0;
  std::uint64_t raster_cache_hits = 0;
  std::uint64_t compute_blur_passes = 0;
  std::uint64_t raster_composite_pixels = 0;
  std::uint64_t raster_cache_copy_pixels = 0;
  std::uint64_t capture_resize_cpu_ns = 0;
  std::uint64_t source_prepare_cpu_ns = 0;
  std::uint64_t kernel_resize_cpu_ns = 0;
  std::uint64_t blur_submit_cpu_ns = 0;
  std::uint64_t mask_upload_cpu_ns = 0;
  // Bit sets of BackdropCacheMiss; several independent conditions can invalidate one frame.
  std::uint64_t source_miss_reasons = 0;
  std::uint64_t blur_miss_reasons = 0;
  std::uint64_t stable_full_frame_planes = 0;
  std::uint64_t cache_texture_allocations = 0;
  std::uint64_t cache_texture_reuses = 0;
  std::uint64_t kernel_source_copy_pixels = 0;
  std::uint64_t kernel_source_reuses = 0;
  std::uint64_t blur_result_adoptions = 0;
  std::uint64_t integer_blur_passes = 0;
};

// Private GL replay shared by native backends. The owner keeps a dedicated GL 3.3 or GLES 3
// context current for every operation, including construction and destruction.
class BackdropGl final {
public:
  explicit BackdropGl(bool gles);
  ~BackdropGl();
  BackdropGl(const BackdropGl&) = delete;
  BackdropGl& operator=(const BackdropGl&) = delete;

  bool Apply(const BackdropPlaneBatch& batch, std::uint8_t* bgra, std::size_t width, std::size_t height,
             float device_left, float device_top);
  bool BeginFrame(std::size_t width, std::size_t height);
  void CompositeRaster(const std::uint8_t* bgra, std::size_t stride);
  // Rows still describe the full frame; pixels outside the integral device rectangle must be transparent.
  void CompositeRaster(const std::uint8_t* bgra, std::size_t stride, Rect bounds);
  // Nonzero identities require exact equality of all immutable raster inputs, established by the owner.
  void CompositeTexture(unsigned int rgba_texture, std::uint64_t identity);
  // The compact texture covers bounds in the frame. Identity includes its complete device placement.
  void CompositeTexture(unsigned int rgba_texture, std::uint64_t identity, Rect bounds, bool cache_texture = true);
  bool CompositeCachedTexture(std::uint64_t identity);
  void PushGroup();
  void PopGroup(const std::uint8_t* mask_bgra, std::size_t mask_stride, float opacity, bool mask_changed = true);
  // Visible bounds contain all nonzero final clip coverage; they limit work without replacing that clip.
  bool ApplyToFrame(const BackdropPlaneBatch& batch, const std::uint8_t* mask_bgra, std::size_t mask_stride,
                    bool mask_changed = true, std::optional<Rect> replacement_clip = std::nullopt,
                    std::optional<Rect> visible_bounds = std::nullopt);
  void ExportToTexture(unsigned int texture);
  void CancelFrame() noexcept;
  // On context loss, retire CPU ownership without deleting names in an unrelated current context.
  void Abandon() noexcept;
  BackdropGlStats FrameStats() const noexcept;
  std::size_t Width() const noexcept;
  std::size_t Height() const noexcept;

private:
  bool ExecutePlane(const BackdropPlaneBatch& batch, std::uint8_t* bgra, std::size_t width, std::size_t height,
                    float device_left, float device_top);
  void EnsureCaptureSize(std::size_t width, std::size_t height);
  struct Resources;
  Resources* resources_ = nullptr;
};

} // namespace huxerui::detail
