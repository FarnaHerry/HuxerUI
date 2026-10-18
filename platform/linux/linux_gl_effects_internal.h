#pragma once

#include <cstddef>
#include <cstdint>

#include <gdk/gdk.h>

#include "graphics/backdrop_gl_internal.h"

namespace huxerui::detail {

class LinuxGdkTextureFrame;

using LinuxBackdropGlStats = BackdropGlStats;

// GPU execution of one backdrop plane through a private GdkGLContext. Initialization is explicit and
// permanent: a display without a usable GL context keeps the renderer on the portable CPU processor.
// All methods must run on the GTK main thread.
class LinuxBackdropGl final {
public:
  LinuxBackdropGl() noexcept;
  ~LinuxBackdropGl();
  LinuxBackdropGl(const LinuxBackdropGl&) = delete;
  LinuxBackdropGl& operator=(const LinuxBackdropGl&) = delete;

  // Realizes a private context on the display and compiles the shader suite. Returns false without
  // throwing when the display cannot provide a usable context; shader and resource failures throw.
  [[nodiscard]] bool TryInitialize(::GdkDisplay* display,
      GdkGLAPI allowed_apis = static_cast<GdkGLAPI>(GDK_GL_API_GL | GDK_GL_API_GLES));

  // Applies every batch member to the capture rows in place. Members' local_to_device mappings are
  // folded with the capture origin (device_left, device_top) inside. Returns false when a member
  // uses a blur transform the GPU path does not express so the caller can replay the batch on CPU.
  [[nodiscard]] bool Apply(const BackdropPlaneBatch& batch, std::uint8_t* bgra, std::size_t width,
                           std::size_t height, float device_left, float device_top);

  // Starts a transparent device-pixel prefix. Unsupported dimensions return false before replacing
  // an existing frame. Raster rows and masks use premultiplied BGRA8; masks contribute only alpha.
  [[nodiscard]] bool BeginFrame(std::size_t width, std::size_t height);
  void CompositeRaster(const std::uint8_t* bgra, std::size_t stride);
  void CompositeRaster(const std::uint8_t* bgra, std::size_t stride, Rect bounds);
  // Copies an integral one-to-one crop from a compatible immutable GL snapshot without CPU pixel transfer.
  // The caller admits unscaled, unclipped commands with unit opacity; straight-alpha sources fall back.
  [[nodiscard]] bool CompositeSnapshot(const LinuxGdkTextureFrame& frame, Rect source, Rect destination);
  void PushGroup();
  void PopGroup(const std::uint8_t* mask_bgra, std::size_t mask_stride, float opacity, bool mask_changed = true);

  // Captures and updates the active prefix entirely on GPU. Coordinates are relative to the frame's
  // device-pixel origin. A null mask applies no clip. Unsupported mappings leave the prefix intact.
  [[nodiscard]] bool ApplyToFrame(const BackdropPlaneBatch& batch, const std::uint8_t* mask_bgra,
                                  std::size_t mask_stride, bool mask_changed = true);

  // Returns an owned immutable RGBA texture, with independent release lifetime and GPU fence.
  // The caller releases its reference. Working targets remain reusable for later frames.
  [[nodiscard]] ::GdkTexture* FinishFrame();
  void CancelFrame() noexcept;
  [[nodiscard]] LinuxBackdropGlStats FrameStats() const noexcept;

private:
  struct Resources;
  Resources* resources_ = nullptr;
  ::GdkGLContext* context_ = nullptr;
};

} // namespace huxerui::detail
