#include "linux_internal.h"
#include "internal_access.h"

#include "linux_gl_effects_internal.h"

#include <epoxy/gl.h>
#include <gdk/gdk.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "graphics/backdrop_gl_impl.inc"
#include "linux_external_texture_internal.h"
#include "graphics/effects_internal.h"
#include "graphics/geometry_internal.h"

namespace huxerui::detail {
namespace {

class CurrentContextScope final {
public:
  explicit CurrentContextScope(::GdkGLContext* context) : previous_(gdk_gl_context_get_current()) {
    if (previous_ != nullptr) {
      g_object_ref(previous_);
    }
    gdk_gl_context_make_current(context);
  }

  ~CurrentContextScope() {
    if (previous_ != nullptr) {
      gdk_gl_context_make_current(previous_);
      g_object_unref(previous_);
    } else {
      gdk_gl_context_clear_current();
    }
  }

  CurrentContextScope(const CurrentContextScope&) = delete;
  CurrentContextScope& operator=(const CurrentContextScope&) = delete;

private:
  ::GdkGLContext* previous_ = nullptr;
};

struct PublishedReleaseContext {
  explicit PublishedReleaseContext(::GdkGLContext* value) : context(GDK_GL_CONTEXT(g_object_ref(value))) {}
  ~PublishedReleaseContext() {
    g_object_unref(context);
  }

  std::mutex mutex;
  ::GdkGLContext* context = nullptr;
};

std::shared_ptr<PublishedReleaseContext> CreatePublishedReleaseContext(::GdkGLContext* producer) {
  GError* error = nullptr;
  ::GdkGLContext* context = gdk_display_create_gl_context(gdk_gl_context_get_display(producer), &error);
  if (context != nullptr) {
    const GdkGLAPI api = gdk_gl_context_get_api(producer);
    gdk_gl_context_set_allowed_apis(context, api);
    gdk_gl_context_set_required_version(context, 3, api == GDK_GL_API_GLES ? 0 : 3);
  }
  const bool realized = context != nullptr && gdk_gl_context_realize(context, &error) != FALSE;
  const std::string message = error != nullptr ? error->message : "unknown GDK GL context error";
  if (error != nullptr) {
    g_error_free(error);
  }
  if (!realized || gdk_gl_context_is_shared(context, producer) == FALSE) {
    if (context != nullptr) {
      g_object_unref(context);
    }
    throw std::runtime_error("HuxerUI Linux backdrop texture release context is unavailable: " + message);
  }
  try {
    auto result = std::make_shared<PublishedReleaseContext>(context);
    g_object_unref(context);
    return result;
  } catch (...) {
    g_object_unref(context);
    throw;
  }
}

struct PublishedTexture {
  std::shared_ptr<PublishedReleaseContext> release_context;
  GLuint texture = 0;
  GLsync sync = nullptr;
};

void ReleasePublishedTexture(gpointer data) noexcept {
  std::unique_ptr<PublishedTexture> texture(static_cast<PublishedTexture*>(data));
  std::lock_guard lock(texture->release_context->mutex);
  CurrentContextScope current(texture->release_context->context);
  if (gdk_gl_context_get_current() != texture->release_context->context) {
    return;
  }
  glDeleteSync(texture->sync);
  glDeleteTextures(1, &texture->texture);
}

} // namespace

struct LinuxBackdropGl::Resources {
  explicit Resources(bool gles) : engine(gles) {}
  ~Resources() {
    if (external_frame != nullptr) g_object_unref(external_frame);
    glDeleteTextures(1, &external_texture);
    glDeleteFramebuffers(2, external_framebuffers);
  }
  BackdropGl engine;
  std::shared_ptr<PublishedReleaseContext> release_context;
  GLuint external_texture = 0;
  GLuint external_framebuffers[2]{};
  int external_width = 0;
  int external_height = 0;
  ::GdkTexture* external_frame = nullptr;
  Rect external_source;
  Rect external_destination;
  std::uint64_t external_identity = 0;
  std::uint64_t external_raster_prefix_bytes = 0;
};

LinuxBackdropGl::LinuxBackdropGl() noexcept = default;

LinuxBackdropGl::~LinuxBackdropGl() {
  if (context_ != nullptr) {
    if (resources_ != nullptr) {
      CurrentContextScope current(context_);
      delete resources_;
    }
    g_object_unref(context_);
  }
}

bool LinuxBackdropGl::TryInitialize(::GdkDisplay* display, GdkGLAPI allowed_apis) {
  if (resources_ != nullptr) {
    return true;
  }
  if (display == nullptr || context_ != nullptr) {
    return false;
  }
  GError* error = nullptr;
  ::GdkGLContext* context = gdk_display_create_gl_context(display, &error);
  if (context != nullptr) {
    gdk_gl_context_set_allowed_apis(context, allowed_apis);
  }
  if (context == nullptr || gdk_gl_context_realize(context, &error) == FALSE) {
    if (error != nullptr) {
      g_error_free(error);
    }
    if (context != nullptr) {
      g_object_unref(context);
    }
    return false;
  }
  int major = 0;
  int minor = 0;
  gdk_gl_context_get_version(context, &major, &minor);
  const bool gles = gdk_gl_context_get_use_es(context) == TRUE;
  if (major < 3 || (!gles && major == 3 && minor < 3)) {
    g_object_unref(context);
    return false;
  }
  context_ = context;
  try {
    CurrentContextScope current(context_);
    resources_ = new Resources(gles);
  } catch (...) {
    g_object_unref(context_);
    context_ = nullptr;
    throw;
  }
  return true;
}

bool LinuxBackdropGl::Apply(const BackdropPlaneBatch& batch, std::uint8_t* bgra, std::size_t width, std::size_t height,
                            float device_left, float device_top) {
  if (bgra == nullptr) {
    throw std::invalid_argument("HuxerUI backdrop CPU transfer requires pixel data");
  }
  if (resources_ == nullptr) {
    return false;
  }
  CurrentContextScope current(context_);
  return resources_->engine.Apply(batch, bgra, width, height, device_left, device_top);
}

bool LinuxBackdropGl::BeginFrame(std::size_t width, std::size_t height) {
  if (resources_ == nullptr) {
    return false;
  }
  CurrentContextScope current(context_);
  return resources_->engine.BeginFrame(width, height);
}

void LinuxBackdropGl::CompositeRaster(const std::uint8_t* bgra, std::size_t stride) {
  if (resources_ == nullptr) {
    throw std::logic_error("HuxerUI Linux backdrop requires initialization");
  }
  CurrentContextScope current(context_);
  resources_->engine.CompositeRaster(bgra, stride);
}

void LinuxBackdropGl::CompositeRaster(const std::uint8_t* bgra, std::size_t stride, Rect bounds) {
  if (resources_ == nullptr) {
    throw std::logic_error("HuxerUI Linux backdrop requires initialization");
  }
  CurrentContextScope current(context_);
  resources_->engine.CompositeRaster(bgra, stride, bounds);
}

bool LinuxBackdropGl::CompositeSnapshot(const LinuxGdkTextureFrame& frame, Rect source, Rect destination) {
  const LinuxGlSnapshot& snapshot = frame.GlSnapshot();
  if (resources_ == nullptr || snapshot.context == nullptr || !snapshot.premultiplied ||
      gdk_gl_context_is_shared(context_, snapshot.context) == FALSE) {
    return false;
  }
  CurrentContextScope current(context_);
  const int width = static_cast<int>(source.width);
  const int height = static_cast<int>(source.height);
  if (resources_->external_width != width || resources_->external_height != height) {
    glDeleteTextures(1, &resources_->external_texture);
    resources_->external_texture = 0;
    resources_->external_width = resources_->external_height = 0;
    resources_->external_texture = CreateTexture(width, height);
    resources_->external_width = width;
    resources_->external_height = height;
  }
  if (resources_->external_framebuffers[0] == 0) glGenFramebuffers(2, resources_->external_framebuffers);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, resources_->external_framebuffers[0]);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, snapshot.texture, 0);
  RequireFramebuffer(GL_READ_FRAMEBUFFER);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resources_->external_framebuffers[1]);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resources_->external_texture, 0);
  RequireFramebuffer(GL_DRAW_FRAMEBUFFER);
  int top = static_cast<int>(source.y);
  int bottom = top + height;
  if (frame.TextureOrigin() == linux::GlTexture::Origin::BottomLeft) {
    const int texture_height = gdk_texture_get_height(frame.Texture());
    top = texture_height - top;
    bottom = top - height;
  }
  glDisable(GL_SCISSOR_TEST);
  glBlitFramebuffer(static_cast<int>(source.x), top, static_cast<int>(source.x) + width, bottom,
                    0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  RequireGlSuccess();
  // Retain the exact immutable source so pointer reuse cannot make a later publication reuse an old blur.
  // The preceding raster byte count distinguishes native/raster interleaving; raster bytes themselves are compared
  // by the executor. One retained source and one crop are bounded by the renderer's 16-Mpixel admission limit.
  const auto raster_prefix_bytes = resources_->engine.FrameStats().raster_upload_bytes;
  if (resources_->external_frame != frame.Texture() || resources_->external_source != source ||
      resources_->external_destination != destination ||
      resources_->external_raster_prefix_bytes != raster_prefix_bytes) {
    if (resources_->external_frame != nullptr) g_object_unref(resources_->external_frame);
    resources_->external_frame = GDK_TEXTURE(g_object_ref(frame.Texture()));
    resources_->external_source = source;
    resources_->external_destination = destination;
    resources_->external_raster_prefix_bytes = raster_prefix_bytes;
    if (++resources_->external_identity == 0) {
      throw std::logic_error("HuxerUI Linux backdrop external source identity exhausted");
    }
  }
  resources_->engine.CompositeTexture(resources_->external_texture, resources_->external_identity, destination, false);
  // Detach borrowed storage before the frame's last GDK reference can release its texture name.
  glBindFramebuffer(GL_READ_FRAMEBUFFER, resources_->external_framebuffers[0]);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
  return true;
}

void LinuxBackdropGl::PushGroup() {
  if (resources_ == nullptr) {
    throw std::logic_error("HuxerUI Linux backdrop requires initialization");
  }
  CurrentContextScope current(context_);
  resources_->engine.PushGroup();
}

void LinuxBackdropGl::PopGroup(const std::uint8_t* mask_bgra, std::size_t mask_stride, float opacity,
                               bool mask_changed) {
  if (resources_ == nullptr) {
    throw std::logic_error("HuxerUI Linux backdrop requires initialization");
  }
  CurrentContextScope current(context_);
  resources_->engine.PopGroup(mask_bgra, mask_stride, opacity, mask_changed);
}

bool LinuxBackdropGl::ApplyToFrame(const BackdropPlaneBatch& batch, const std::uint8_t* mask_bgra,
                                   std::size_t mask_stride, bool mask_changed) {
  if (resources_ == nullptr) {
    return false;
  }
  CurrentContextScope current(context_);
  return resources_->engine.ApplyToFrame(batch, mask_bgra, mask_stride, mask_changed);
}

::GdkTexture* LinuxBackdropGl::FinishFrame() {
  if (resources_ == nullptr) {
    throw std::logic_error("HuxerUI Linux backdrop requires initialization");
  }
  CurrentContextScope current(context_);
  if (!resources_->release_context) {
    resources_->release_context = CreatePublishedReleaseContext(context_);
  }
  struct PendingExport {
    GLuint texture = 0;
    GLsync sync = nullptr;
    ~PendingExport() {
      if (sync != nullptr) {
        glDeleteSync(sync);
      }
      glDeleteTextures(1, &texture);
    }
  } pending;
  pending.texture = CreateTexture(resources_->engine.Width(), resources_->engine.Height());
  resources_->engine.ExportToTexture(pending.texture);
  pending.sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  if (pending.sync == nullptr) {
    throw std::runtime_error("HuxerUI Linux backdrop publication could not create a GPU fence");
  }
  glFlush();
  RequireGlSuccess();
  auto published = std::make_unique<PublishedTexture>();
  published->release_context = resources_->release_context;
  published->texture = pending.texture;
  published->sync = pending.sync;
  GdkGLTextureBuilder* builder = gdk_gl_texture_builder_new();
  gdk_gl_texture_builder_set_context(builder, resources_->release_context->context);
  gdk_gl_texture_builder_set_id(builder, pending.texture);
  gdk_gl_texture_builder_set_width(builder, static_cast<int>(resources_->engine.Width()));
  gdk_gl_texture_builder_set_height(builder, static_cast<int>(resources_->engine.Height()));
  gdk_gl_texture_builder_set_format(builder, GDK_MEMORY_R8G8B8A8_PREMULTIPLIED);
  gdk_gl_texture_builder_set_sync(builder, pending.sync);
  ::GdkTexture* texture = gdk_gl_texture_builder_build(builder, ReleasePublishedTexture, published.get());
  g_object_unref(builder);
  if (texture == nullptr) {
    throw std::runtime_error("HuxerUI Linux backdrop publication could not build an immutable GL texture");
  }
  static_cast<void>(published.release());
  pending.texture = 0;
  pending.sync = nullptr;
  CancelFrame();
  return texture;
}

void LinuxBackdropGl::CancelFrame() noexcept {
  if (resources_ != nullptr) {
    resources_->engine.CancelFrame();
  }
}

LinuxBackdropGlStats LinuxBackdropGl::FrameStats() const noexcept {
  return resources_ != nullptr ? resources_->engine.FrameStats() : LinuxBackdropGlStats{};
}

} // namespace huxerui::detail
