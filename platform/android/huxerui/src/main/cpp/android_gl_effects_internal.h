#pragma once

#include <jni.h>

#include <memory>

#include <huxerui/android/jni.h>

#include "graphics/backdrop_gl_internal.h"

namespace huxerui::detail {

// Owned by AndroidRenderer and used on the application UI thread. Published HardwareBuffers
// have independent Java lifetimes and are never reused as mutable working textures.
class AndroidBackdropGl final {
public:
  AndroidBackdropGl();
  ~AndroidBackdropGl();
  AndroidBackdropGl(const AndroidBackdropGl&) = delete;
  AndroidBackdropGl& operator=(const AndroidBackdropGl&) = delete;

  bool TryInitialize();
  bool BeginFrame(std::size_t width, std::size_t height, std::uint64_t frame = 0);
  void PrepareHardwareRaster(JNIEnv* environment);
  bool CompositeCachedRaster(std::uint64_t identity);
  void CompositeHardwareRaster(JNIEnv* environment, jobject image, std::uint64_t identity, Rect bounds,
                               bool cache_texture);
  void PushGroup();
  void PopGroup(JNIEnv* environment, jobject mask, float opacity);
  bool ApplyToFrame(JNIEnv* environment, const BackdropPlaneBatch& batch, jobject mask,
                    std::optional<Rect> replacement_clip = std::nullopt,
                    std::optional<Rect> visible_bounds = std::nullopt);
  android::LocalRef<jobject> FinishFrame(JNIEnv* environment);
  void CancelFrame() noexcept;
  BackdropGlStats FrameStats() const noexcept;
  bool Apply(const BackdropPlaneBatch& batch, std::uint8_t* bgra, std::size_t width, std::size_t height,
             float device_left, float device_top);

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace huxerui::detail
