#include "android_gl_effects_internal.h"
#include "android_backdrop_trace_internal.h"

#include <android/bitmap.h>
#include <android/hardware_buffer.h>
#include <android/hardware_buffer_jni.h>
#include <dlfcn.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>

#include <cstring>
#include <array>
#include <stdexcept>
#include <vector>

#include "graphics/backdrop_gl_impl.inc"

namespace huxerui::detail {
namespace {

class GpuTimings final {
public:
  void SetFrame(std::uint64_t frame) {
#if defined(HUXERUI_ENABLE_PROFILING) && HUXERUI_ENABLE_PROFILING
    frame_ = frame;
#else
    static_cast<void>(frame);
#endif
  }
  void Poll() {
#if defined(HUXERUI_ENABLE_PROFILING) && HUXERUI_ENABLE_PROFILING
    if (!initialized_ || !result_)
      return;
    GLint disjoint = 0;
    glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
    for (auto& slot : slots_) {
      if (!slot.pending)
        continue;
      GLuint ready = 0;
      glGetQueryObjectuiv(slot.query, GL_QUERY_RESULT_AVAILABLE, &ready);
      if (!ready)
        continue;
      GLuint64 nanoseconds = 0;
      result_(slot.query, GL_QUERY_RESULT, &nanoseconds);
      if (!disjoint) {
        __android_log_print(ANDROID_LOG_INFO, "HuxerUI", "Backdrop GPU stage=%s frame=%llu ns=%llu", slot.name,
                            static_cast<unsigned long long>(slot.frame),
                            static_cast<unsigned long long>(nanoseconds));
      }
      slot.pending = false;
    }
    if (disjoint) {
      // Results spanning a clock discontinuity cannot be compared, including still-pending queries.
      Reset();
      __android_log_print(ANDROID_LOG_INFO, "HuxerUI", "Backdrop GPU timing disjoint; samples discarded");
    }
#endif
  }

  void Reset() {
#if defined(HUXERUI_ENABLE_PROFILING) && HUXERUI_ENABLE_PROFILING
    for (auto& slot : slots_) {
      if (slot.query)
        glDeleteQueries(1, &slot.query);
      slot = {};
    }
#endif
  }

  class Scope final {
  public:
    Scope(GpuTimings& owner, const char* name) : owner_(owner), active_(owner.Begin(name)) {}
    ~Scope() {
      if (active_)
        owner_.End();
    }

  private:
    GpuTimings& owner_;
    bool active_;
  };

private:
  bool Begin(const char* name) {
#if defined(HUXERUI_ENABLE_PROFILING) && HUXERUI_ENABLE_PROFILING
    if (!ATrace_isEnabled())
      return false;
    if (!initialized_) {
      initialized_ = true;
      const auto* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
      GLint counter_bits = 0;
      if (extensions && std::strstr(extensions, "GL_EXT_disjoint_timer_query")) {
        result_ = reinterpret_cast<PFNGLGETQUERYOBJECTUI64VEXTPROC>(eglGetProcAddress("glGetQueryObjectui64vEXT"));
        const auto query_info = reinterpret_cast<PFNGLGETQUERYIVEXTPROC>(eglGetProcAddress("glGetQueryivEXT"));
        if (query_info)
          query_info(GL_TIME_ELAPSED_EXT, GL_QUERY_COUNTER_BITS_EXT, &counter_bits);
        if (counter_bits == 0)
          result_ = nullptr;
      }
      __android_log_print(ANDROID_LOG_INFO, "HuxerUI", "Backdrop GPU timers available=%d counter_bits=%d",
                          result_ != nullptr, counter_bits);
    }
    if (!result_)
      return false;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      auto& slot = slots_[i];
      if (slot.pending)
        continue;
      if (!slot.query)
        glGenQueries(1, &slot.query);
      if (!slot.query)
        return false;
      slot.name = name;
      slot.frame = frame_;
      active_ = i;
      glBeginQuery(GL_TIME_ELAPSED_EXT, slot.query);
      return true;
    }
    return false;
#else
    static_cast<void>(name);
    return false;
#endif
  }
  void End() {
#if defined(HUXERUI_ENABLE_PROFILING) && HUXERUI_ENABLE_PROFILING
    glEndQuery(GL_TIME_ELAPSED_EXT);
    slots_[active_].pending = true;
#endif
  }
#if defined(HUXERUI_ENABLE_PROFILING) && HUXERUI_ENABLE_PROFILING
  struct Slot {
    GLuint query = 0;
    const char* name = nullptr;
    std::uint64_t frame = 0;
    bool pending = false;
  };
  std::array<Slot, 64> slots_{};
  std::size_t active_ = 0;
  std::uint64_t frame_ = 0;
  bool initialized_ = false;
  PFNGLGETQUERYOBJECTUI64VEXTPROC result_ = nullptr;
#endif
};

class CurrentEgl final {
public:
  CurrentEgl(EGLDisplay display, EGLSurface surface, EGLContext context)
      : previous_display_(eglGetCurrentDisplay()), previous_context_(eglGetCurrentContext()),
        previous_read_(eglGetCurrentSurface(EGL_READ)), previous_draw_(eglGetCurrentSurface(EGL_DRAW)),
        display_(display) {
    changed_ = previous_display_ != display || previous_context_ != context || previous_read_ != surface ||
               previous_draw_ != surface;
    if (changed_ && eglMakeCurrent(display, surface, surface, context) != EGL_TRUE) {
      throw std::runtime_error("HuxerUI Android backdrop could not make its EGL context current");
    }
  }
  ~CurrentEgl() {
    if (!changed_) {
      return;
    }
    if (previous_context_ != EGL_NO_CONTEXT) {
      eglMakeCurrent(previous_display_, previous_draw_, previous_read_, previous_context_);
    } else {
      eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
  }

private:
  EGLDisplay previous_display_;
  EGLContext previous_context_;
  EGLSurface previous_read_;
  EGLSurface previous_draw_;
  EGLDisplay display_;
  bool changed_ = false;
};

class BitmapLock final {
public:
  BitmapLock(JNIEnv* environment, jobject bitmap, std::size_t width, std::size_t height)
      : environment_(environment), bitmap_(bitmap) {
    if (bitmap == nullptr || AndroidBitmap_getInfo(environment, bitmap, &info_) != ANDROID_BITMAP_RESULT_SUCCESS ||
        info_.format != ANDROID_BITMAP_FORMAT_RGBA_8888 || info_.width != width || info_.height != height ||
        info_.stride < width * 4) {
      throw std::logic_error("HuxerUI Android backdrop requires matching premultiplied RGBA bitmap rows");
    }
    if (AndroidBitmap_lockPixels(environment, bitmap, &pixels_) != ANDROID_BITMAP_RESULT_SUCCESS) {
      throw std::runtime_error("HuxerUI Android backdrop could not lock raster pixels");
    }
  }
  ~BitmapLock() {
    AndroidBitmap_unlockPixels(environment_, bitmap_);
  }
  const std::uint8_t* Data() const noexcept {
    return static_cast<const std::uint8_t*>(pixels_);
  }
  std::size_t Stride() const noexcept {
    return info_.stride;
  }

private:
  JNIEnv* environment_;
  jobject bitmap_;
  AndroidBitmapInfo info_{};
  void* pixels_ = nullptr;
};

} // namespace

struct AndroidBackdropGl::State {
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLContext context = EGL_NO_CONTEXT;
  EGLSurface surface = EGL_NO_SURFACE;
  std::unique_ptr<BackdropGl> engine;
  GpuTimings timings;
  // Segment scopes borrow this binding instead of flushing and rebinding the context at every paint boundary.
  std::unique_ptr<CurrentEgl> frame_context;
  std::vector<std::uint8_t> raster_rows;
  void* android_library = nullptr;
  int (*allocate)(const AHardwareBuffer_Desc*, AHardwareBuffer**) = nullptr;
  void (*release)(AHardwareBuffer*) = nullptr;
  jobject (*to_java)(JNIEnv*, AHardwareBuffer*) = nullptr;
  AHardwareBuffer* (*from_java)(JNIEnv*, jobject) = nullptr;
  JNIEnv* environment = nullptr;
  jmethodID close_image = nullptr;
  jmethodID image_buffer = nullptr;
  jmethodID image_width = nullptr;
  jmethodID image_height = nullptr;
  jmethodID close_buffer = nullptr;
  jmethodID generation_id = nullptr;
  jobject cached_mask = nullptr;
  jint cached_mask_generation = 0;
  struct RasterImage {
    jobject image;
    EGLImageKHR egl_image;
    GLuint texture;
    GLsync fence;
  };
  std::vector<RasterImage> images;

  void RetireImages(bool all) {
    for (auto it = images.begin(); it != images.end();) {
      const auto result = glClientWaitSync(it->fence, all ? GL_SYNC_FLUSH_COMMANDS_BIT : 0, all ? 1000000000ULL : 0);
      if (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED) {
        glDeleteSync(it->fence);
        glDeleteTextures(1, &it->texture);
        destroy_image(display, it->egl_image);
        environment->CallVoidMethod(it->image, close_image);
        environment->DeleteGlobalRef(it->image);
        it = images.erase(it);
      } else if (all) {
        throw std::runtime_error("HuxerUI Android raster sampling fence did not signal");
      } else {
        ++it;
      }
    }
  }

  jint MaskGeneration(JNIEnv* env, jobject mask) {
    environment = env;
    if (generation_id == nullptr) {
      android::LocalRef<jclass> type{env, env->GetObjectClass(mask)};
      if (!type) return 0;
      generation_id = env->GetMethodID(type.Get(), "getGenerationId", "()I");
      if (generation_id == nullptr) return 0;
    }
    return env->CallIntMethod(mask, generation_id);
  }

  bool MaskChanged(JNIEnv* env, jobject mask) {
    const jint generation = MaskGeneration(env, mask);
    if (env->ExceptionCheck()) return true;
    return cached_mask == nullptr || !env->IsSameObject(cached_mask, mask) || generation != cached_mask_generation;
  }

  void ForgetMask(JNIEnv* env) noexcept {
    if (cached_mask != nullptr) {
      env->DeleteGlobalRef(cached_mask);
      cached_mask = nullptr;
    }
  }

  void RememberMask(JNIEnv* env, jobject mask) {
    const jint generation = MaskGeneration(env, mask);
    if (env->ExceptionCheck()) {
      ForgetMask(env);
      return;
    }
    if (cached_mask == nullptr || !env->IsSameObject(cached_mask, mask)) {
      if (cached_mask != nullptr) {
        env->DeleteGlobalRef(cached_mask);
      }
      cached_mask = env->NewGlobalRef(mask);
    }
    cached_mask_generation = generation;
  }
  PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC client_buffer = nullptr;
  PFNEGLCREATEIMAGEKHRPROC create_image = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC destroy_image = nullptr;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bind_image = nullptr;

  ~State() {
    if (context != EGL_NO_CONTEXT) {
      frame_context.reset();
      try {
        CurrentEgl current(display, surface, context);
        RetireImages(true);
        timings.Reset();
        engine.reset();
      } catch (...) {
        if (engine) {
          engine->Abandon();
        }
        engine.reset();
      }
      eglDestroyContext(display, context);
      for (const auto& image : images) {
        destroy_image(display, image.egl_image);
        environment->CallVoidMethod(image.image, close_image);
        environment->DeleteGlobalRef(image.image);
      }
      images.clear();
    }
    if (cached_mask != nullptr) {
      environment->DeleteGlobalRef(cached_mask);
    }
    if (surface != EGL_NO_SURFACE) {
      eglDestroySurface(display, surface);
    }
    // EGL_DEFAULT_DISPLAY is shared with Android's other native GPU consumers. Do not terminate it.
    if (android_library != nullptr) {
      dlclose(android_library);
    }
  }
};

AndroidBackdropGl::AndroidBackdropGl() = default;
AndroidBackdropGl::~AndroidBackdropGl() = default;

bool AndroidBackdropGl::TryInitialize() {
  if (state_) {
    return true;
  }
  auto state = std::make_unique<State>();
  state->android_library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
  if (state->android_library == nullptr) {
    return false;
  }
  // Resolve API 26 functions dynamically so the library continues to load on API 23.
  state->allocate =
      reinterpret_cast<decltype(state->allocate)>(dlsym(state->android_library, "AHardwareBuffer_allocate"));
  state->release = reinterpret_cast<decltype(state->release)>(dlsym(state->android_library, "AHardwareBuffer_release"));
  state->to_java =
      reinterpret_cast<decltype(state->to_java)>(dlsym(state->android_library, "AHardwareBuffer_toHardwareBuffer"));
  state->from_java =
      reinterpret_cast<decltype(state->from_java)>(dlsym(state->android_library, "AHardwareBuffer_fromHardwareBuffer"));
  state->client_buffer =
      reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(eglGetProcAddress("eglGetNativeClientBufferANDROID"));
  state->create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
  state->destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
  state->bind_image =
      reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  if (!state->allocate || !state->release || !state->to_java || !state->from_java || !state->client_buffer ||
      !state->create_image || !state->destroy_image || !state->bind_image) {
    return false;
  }
  state->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (state->display == EGL_NO_DISPLAY || eglInitialize(state->display, nullptr, nullptr) != EGL_TRUE) {
    return false;
  }
  const EGLint attributes[] = {EGL_SURFACE_TYPE,
                               EGL_PBUFFER_BIT,
                               EGL_RENDERABLE_TYPE,
                               EGL_OPENGL_ES3_BIT_KHR,
                               EGL_RED_SIZE,
                               8,
                               EGL_GREEN_SIZE,
                               8,
                               EGL_BLUE_SIZE,
                               8,
                               EGL_ALPHA_SIZE,
                               8,
                               EGL_NONE};
  EGLConfig config = nullptr;
  EGLint count = 0;
  if (eglChooseConfig(state->display, attributes, &config, 1, &count) != EGL_TRUE || count != 1) {
    return false;
  }
  const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  state->context = eglCreateContext(state->display, config, EGL_NO_CONTEXT, context_attributes);
  if (state->context == EGL_NO_CONTEXT) {
    return false;
  }
  const EGLint surface_attributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  state->surface = eglCreatePbufferSurface(state->display, config, surface_attributes);
  if (state->surface == EGL_NO_SURFACE) {
    return false;
  }
  {
    CurrentEgl current(state->display, state->surface, state->context);
    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    const char* egl_extensions = eglQueryString(state->display, EGL_EXTENSIONS);
    if (extensions == nullptr || std::strstr(extensions, "GL_OES_EGL_image") == nullptr || egl_extensions == nullptr ||
        std::strstr(egl_extensions, "EGL_ANDROID_image_native_buffer") == nullptr) {
      return false;
    }
    state->engine = std::make_unique<BackdropGl>(true);
  }
  state_ = std::move(state);
  return true;
}

bool AndroidBackdropGl::BeginFrame(std::size_t width, std::size_t height, std::uint64_t frame) {
  if (!state_) {
    return false;
  }
  state_->frame_context.reset();
  state_->frame_context = std::make_unique<CurrentEgl>(state_->display, state_->surface, state_->context);
  state_->timings.Poll();
  state_->timings.SetFrame(frame);
  if (width != state_->engine->Width() || height != state_->engine->Height()) {
    if (state_->cached_mask != nullptr) {
      state_->environment->DeleteGlobalRef(state_->cached_mask);
      state_->cached_mask = nullptr;
    }
  }
  const bool active = state_->engine->BeginFrame(width, height);
  if (!active) {
    state_->frame_context.reset();
  }
  return active;
}

bool AndroidBackdropGl::CompositeCachedRaster(std::uint64_t identity) {
  CurrentEgl current(state_->display, state_->surface, state_->context);
  GpuTimings::Scope timing(state_->timings, "raster-cache");
  return state_->engine->CompositeCachedTexture(identity);
}

void AndroidBackdropGl::PrepareHardwareRaster(JNIEnv* environment) {
  AndroidBackdropTrace trace("HuxerUI Raster Retirement");
  state_->environment = environment;
  CurrentEgl current(state_->display, state_->surface, state_->context);
  state_->RetireImages(state_->images.size() >= 3);
  // HWUI may acquire a recycled producer buffer while this context is retained. Submit its pending native accesses.
  glFlush();
}

void AndroidBackdropGl::CompositeHardwareRaster(JNIEnv* environment, jobject image, std::uint64_t identity, Rect bounds,
                                                bool cache_texture) {
  AndroidBackdropTrace trace("HuxerUI Raster Import Composite");
  state_->environment = environment;
  CurrentEgl current(state_->display, state_->surface, state_->context);
  GpuTimings::Scope timing(state_->timings, "raster-import-composite");
  android::LocalRef<jclass> bitmap_type{environment, environment->FindClass("android/graphics/Bitmap")};
  if (environment->IsInstanceOf(image, bitmap_type.Get())) {
    const auto width = state_->engine->Width();
    const auto height = state_->engine->Height();
    BitmapLock rows(environment, image, width, height);
    state_->raster_rows.resize(width * height * 4);
    for (std::size_t y = 0; y < height; ++y) {
      for (std::size_t x = 0; x < width; ++x) {
        const auto* source = rows.Data() + y * rows.Stride() + x * 4;
        auto* target = state_->raster_rows.data() + (y * width + x) * 4;
        target[0] = source[2];
        target[1] = source[1];
        target[2] = source[0];
        target[3] = source[3];
      }
    }
    state_->engine->CompositeRaster(state_->raster_rows.data(), width * 4);
    return;
  }
  if (state_->image_buffer == nullptr) {
    android::LocalRef<jclass> type{environment, environment->GetObjectClass(image)};
    state_->image_buffer =
        environment->GetMethodID(type.Get(), "getHardwareBuffer", "()Landroid/hardware/HardwareBuffer;");
    state_->image_width = environment->GetMethodID(type.Get(), "getWidth", "()I");
    state_->image_height = environment->GetMethodID(type.Get(), "getHeight", "()I");
    state_->close_image = environment->GetMethodID(type.Get(), "close", "()V");
  }
  if (environment->ExceptionCheck()) {
    return;
  }
  const jint width = environment->CallIntMethod(image, state_->image_width);
  const jint height = environment->CallIntMethod(image, state_->image_height);
  if (environment->ExceptionCheck()) {
    return;
  }
  if (width == static_cast<jint>(state_->engine->Width()) && height == static_cast<jint>(state_->engine->Height())) {
    bounds = {0, 0, static_cast<float>(width), static_cast<float>(height)};
  } else if (width != bounds.width || height != bounds.height) {
    environment->CallVoidMethod(image, state_->close_image);
    throw std::logic_error("HuxerUI Android raster image does not match its capture bounds");
  }
  android::LocalRef<jobject> buffer{environment, environment->CallObjectMethod(image, state_->image_buffer)};
  if (!buffer || environment->ExceptionCheck()) {
    return;
  }
  if (state_->close_buffer == nullptr) {
    android::LocalRef<jclass> type{environment, environment->GetObjectClass(buffer.Get())};
    state_->close_buffer = environment->GetMethodID(type.Get(), "close", "()V");
  }
  const EGLint attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
  state_->images.reserve(state_->images.size() + 1);
  State::RasterImage imported{environment->NewGlobalRef(image), EGL_NO_IMAGE_KHR, 0, nullptr};
  if (imported.image == nullptr) {
    environment->CallVoidMethod(buffer.Get(), state_->close_buffer);
    return;
  }
  try {
    imported.egl_image =
        state_->create_image(state_->display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                             state_->client_buffer(state_->from_java(environment, buffer.Get())), attributes);
    if (imported.egl_image == EGL_NO_IMAGE_KHR) {
      throw std::runtime_error("HuxerUI Android raster import failed");
    }
    glGenTextures(1, &imported.texture);
    glBindTexture(GL_TEXTURE_2D, imported.texture);
    state_->bind_image(GL_TEXTURE_2D, imported.egl_image);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    state_->engine->CompositeTexture(imported.texture, identity, bounds, cache_texture);
    imported.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (!imported.fence) {
      throw std::runtime_error("HuxerUI Android raster sampling fence is unavailable");
    }
    glFlush();
    state_->images.push_back(imported);
  } catch (...) {
    // Only the exceptional path drains the queue: an imported producer image cannot be returned
    // while any earlier sampling or cache-copy command may still read it.
    glFinish();
    if (imported.fence) {
      glDeleteSync(imported.fence);
    }
    glDeleteTextures(1, &imported.texture);
    if (imported.egl_image != EGL_NO_IMAGE_KHR) {
      state_->destroy_image(state_->display, imported.egl_image);
    }
    environment->CallVoidMethod(image, state_->close_image);
    environment->DeleteGlobalRef(imported.image);
    environment->CallVoidMethod(buffer.Get(), state_->close_buffer);
    throw;
  }
  environment->CallVoidMethod(buffer.Get(), state_->close_buffer);
}

void AndroidBackdropGl::PushGroup() {
  CurrentEgl current(state_->display, state_->surface, state_->context);
  state_->engine->PushGroup();
}

void AndroidBackdropGl::PopGroup(JNIEnv* environment, jobject mask, float opacity) {
  CurrentEgl current(state_->display, state_->surface, state_->context);
  if (mask == nullptr) {
    state_->engine->PopGroup(nullptr, 0, opacity);
    return;
  }
  const bool changed = state_->MaskChanged(environment, mask);
  if (environment->ExceptionCheck()) return;
  try {
    BitmapLock rows(environment, mask, state_->engine->Width(), state_->engine->Height());
    state_->engine->PopGroup(rows.Data(), rows.Stride(), opacity, changed);
  } catch (...) {
    state_->ForgetMask(environment);
    throw;
  }
  state_->RememberMask(environment, mask);
}

bool AndroidBackdropGl::ApplyToFrame(JNIEnv* environment, const BackdropPlaneBatch& batch, jobject mask,
                                     std::optional<Rect> replacement_clip, std::optional<Rect> visible_bounds) {
  AndroidBackdropTrace trace("HuxerUI Optical Plane");
  CurrentEgl current(state_->display, state_->surface, state_->context);
  GpuTimings::Scope timing(state_->timings, "optical");
  if (mask == nullptr) {
    return state_->engine->ApplyToFrame(batch, nullptr, 0, true, replacement_clip, visible_bounds);
  }
  AndroidBackdropTrace mask_trace("HuxerUI Mask Lock Replay");
  const bool changed = state_->MaskChanged(environment, mask);
  if (environment->ExceptionCheck()) return false;
  bool applied;
  try {
    BitmapLock rows(environment, mask, state_->engine->Width(), state_->engine->Height());
    applied = state_->engine->ApplyToFrame(batch, rows.Data(), rows.Stride(), changed, replacement_clip,
                                          visible_bounds);
  } catch (...) {
    state_->ForgetMask(environment);
    throw;
  }
  // Pixel access may advance Bitmap generation even though this lock only reads. Commit after unlock and success.
  if (applied) state_->RememberMask(environment, mask);
  return applied;
}

android::LocalRef<jobject> AndroidBackdropGl::FinishFrame(JNIEnv* environment) {
  AndroidBackdropTrace trace("HuxerUI Export");
  CurrentEgl current(state_->display, state_->surface, state_->context);
  struct PendingBuffer {
    State& state;
    AHardwareBuffer* buffer = nullptr;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
    GLsync sync = nullptr;
    ~PendingBuffer() {
      if (sync) {
        glDeleteSync(sync);
      }
      glDeleteTextures(1, &texture);
      if (image != EGL_NO_IMAGE_KHR) {
        state.destroy_image(state.display, image);
      }
      if (buffer) {
        state.release(buffer);
      }
    }
  } pending{*state_};
  AHardwareBuffer_Desc description{};
  description.width = static_cast<std::uint32_t>(state_->engine->Width());
  description.height = static_cast<std::uint32_t>(state_->engine->Height());
  description.layers = 1;
  description.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
  description.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
  if (state_->allocate(&description, &pending.buffer) != 0) {
    throw std::runtime_error("HuxerUI Android backdrop could not allocate its immutable hardware buffer");
  }
  const EGLint attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
  pending.image = state_->create_image(state_->display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                       state_->client_buffer(pending.buffer), attributes);
  if (pending.image == EGL_NO_IMAGE_KHR) {
    throw std::runtime_error("HuxerUI Android backdrop could not import its hardware buffer");
  }
  glGenTextures(1, &pending.texture);
  glBindTexture(GL_TEXTURE_2D, pending.texture);
  state_->bind_image(GL_TEXTURE_2D, pending.image);
  {
    GpuTimings::Scope timing(state_->timings, "export-copy");
    state_->engine->ExportToTexture(pending.texture);
  }
  pending.sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  if (!pending.sync) {
    throw std::runtime_error("HuxerUI Android backdrop could not create its publication fence");
  }
  // Bitmap.wrapHardwareBuffer has no acquire-fence parameter. Wait for this immutable export
  // before handing it to RenderThread; no pixel data crosses back to the CPU.
  GLenum result;
  {
    AndroidBackdropTrace wait("HuxerUI Export Fence");
    result = glClientWaitSync(pending.sync, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ULL);
  }
  if (result != GL_ALREADY_SIGNALED && result != GL_CONDITION_SATISFIED) {
    throw std::runtime_error("HuxerUI Android backdrop publication fence did not signal");
  }
  android::LocalRef<jobject> buffer{environment, state_->to_java(environment, pending.buffer)};
  if (!buffer || environment->ExceptionCheck()) {
    return {};
  }
  state_->RetireImages(true);
  state_->timings.Poll();
  state_->engine->CancelFrame();
  return buffer;
}

void AndroidBackdropGl::CancelFrame() noexcept {
  if (state_) {
    try {
      CurrentEgl current(state_->display, state_->surface, state_->context);
      state_->RetireImages(true);
    } catch (...) {
    }
    state_->engine->CancelFrame();
    state_->frame_context.reset();
  }
}
BackdropGlStats AndroidBackdropGl::FrameStats() const noexcept {
  return state_ ? state_->engine->FrameStats() : BackdropGlStats{};
}
bool AndroidBackdropGl::Apply(const BackdropPlaneBatch& batch, std::uint8_t* bgra, std::size_t width,
                              std::size_t height, float device_left, float device_top) {
  if (!state_) {
    return false;
  }
  CurrentEgl current(state_->display, state_->surface, state_->context);
  return state_->engine->Apply(batch, bgra, width, height, device_left, device_top);
}

} // namespace huxerui::detail
