#include "graphics/backdrop_gl_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(HUXERUI_ENABLE_PROFILING) && HUXERUI_ENABLE_PROFILING
#include <chrono>
#endif

#include "graphics/backdrop_shader_glsl.h"
#include "graphics/effects_internal.h"
#include "graphics/geometry_internal.h"

namespace huxerui::detail {
namespace {

constexpr std::size_t maximum_backdrop_cache_bytes = 64U * 1024U * 1024U;
constexpr GLint backdrop_capture_tile = 64;

class BackdropCpuTimer final {
public:
  explicit BackdropCpuTimer(std::uint64_t& elapsed) : elapsed_(elapsed) {}
  ~BackdropCpuTimer() {
#if defined(HUXERUI_ENABLE_PROFILING) && HUXERUI_ENABLE_PROFILING
    elapsed_ += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start_).count());
#endif
  }

private:
  [[maybe_unused]] std::uint64_t& elapsed_;
#if defined(HUXERUI_ENABLE_PROFILING) && HUXERUI_ENABLE_PROFILING
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
#endif
};

std::string InfoLog(GLuint handle, bool shader) {
  GLint length = 0;
  if (shader) {
    glGetShaderiv(handle, GL_INFO_LOG_LENGTH, &length);
  } else {
    glGetProgramiv(handle, GL_INFO_LOG_LENGTH, &length);
  }
  std::string log(static_cast<std::size_t>(std::max(length, 0)), ' ');
  if (length > 0) {
    if (shader) {
      glGetShaderInfoLog(handle, length, nullptr, log.data());
    } else {
      glGetProgramInfoLog(handle, length, nullptr, log.data());
    }
  }
  return log;
}

GLuint CompileShader(GLenum kind, const std::string& source) {
  GLuint shader = glCreateShader(kind);
  const char* text = source.c_str();
  glShaderSource(shader, 1, &text, nullptr);
  glCompileShader(shader);
  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled != GL_TRUE) {
    const std::string log = InfoLog(shader, true);
    glDeleteShader(shader);
    throw std::runtime_error("HuxerUI backdrop shader compilation failed: " + log);
  }
  return shader;
}

GLuint LinkProgram(const std::string& vertex_source, const std::string& fragment_source) {
  GLuint vertex = CompileShader(GL_VERTEX_SHADER, vertex_source);
  GLuint fragment = 0;
  try {
    fragment = CompileShader(GL_FRAGMENT_SHADER, fragment_source);
  } catch (...) {
    glDeleteShader(vertex);
    throw;
  }
  GLuint program = glCreateProgram();
  glAttachShader(program, vertex);
  glAttachShader(program, fragment);
  glLinkProgram(program);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  GLint linked = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked != GL_TRUE) {
    const std::string log = InfoLog(program, false);
    glDeleteProgram(program);
    throw std::runtime_error("HuxerUI backdrop shader link failed: " + log);
  }
  return program;
}

GLuint LinkComputeProgram(bool gles, bool integer_source = false, bool normalized_output = false) {
  const std::string prefix =
      gles ? "#version 310 es\nprecision highp float;\nprecision highp int;\n" : "#version 430 core\n";
  const std::string input = integer_source ? "#define HUXERUI_INTEGER_SOURCE\n" : "";
  const std::string output = normalized_output ? "#define HUXERUI_NORMALIZED_OUTPUT\n" : "";
  const GLuint shader = CompileShader(GL_COMPUTE_SHADER, prefix + input + output + backdrop_glsl::compute_blur_body);
  const GLuint program = glCreateProgram();
  glAttachShader(program, shader);
  glLinkProgram(program);
  glDeleteShader(shader);
  GLint linked = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked != GL_TRUE) {
    const std::string log = InfoLog(program, false);
    glDeleteProgram(program);
    throw std::runtime_error("HuxerUI backdrop compute shader link failed: " + log);
  }
  return program;
}

// Mirrors the blur-transform validation of ApplyBackdropEffect: blur needs axis-aligned scaling or a
// uniform orthogonal mapping. Physical radii are derived per axis the same way.
bool BlurRadii(const Transform2D& local_to_capture, float blur_radius, std::size_t width, std::size_t height,
               std::size_t& radius_x, std::size_t& radius_y) {
  if (blur_radius <= 0.0F) {
    radius_x = 0;
    radius_y = 0;
    return true;
  }
  const double scale_x = std::hypot(local_to_capture.m11, local_to_capture.m21);
  const double scale_y = std::hypot(local_to_capture.m12, local_to_capture.m22);
  const double tolerance = 0.00001 * std::max(scale_x, scale_y);
  const bool aligned = (std::abs(local_to_capture.m12) <= tolerance && std::abs(local_to_capture.m21) <= tolerance) ||
                       (std::abs(local_to_capture.m11) <= tolerance && std::abs(local_to_capture.m22) <= tolerance);
  const double column_dot = static_cast<double>(local_to_capture.m11) * local_to_capture.m21 +
                            static_cast<double>(local_to_capture.m12) * local_to_capture.m22;
  const bool uniform =
      std::abs(scale_x - scale_y) <= tolerance && std::abs(column_dot) <= tolerance * std::max(scale_x, scale_y);
  if (!aligned && !uniform) {
    return false;
  }
  const auto radius = [blur_radius](double scale, std::size_t extent) {
    const double scaled = std::ceil(static_cast<double>(blur_radius) * scale / 3.0);
    return static_cast<std::size_t>(std::min(scaled, static_cast<double>(extent)));
  };
  radius_x = radius(scale_x, width);
  radius_y = radius(scale_y, height);
  return true;
}

std::array<float, 9> ColumnMajor(const Transform2D& transform) noexcept {
  return {transform.m11,         transform.m12,         0.0F, transform.m21, transform.m22, 0.0F,
          transform.translate_x, transform.translate_y, 1.0F};
}

void RequireFramebuffer(GLenum target) {
  if (glCheckFramebufferStatus(target) != GL_FRAMEBUFFER_COMPLETE) {
    throw std::runtime_error("HuxerUI backdrop framebuffer is incomplete");
  }
}

void RequireGlSuccess() {
  const auto error = glGetError();
  if (error != GL_NO_ERROR) {
    throw std::runtime_error("HuxerUI backdrop GL replay failed (" + std::to_string(error) + ")");
  }
}

GLuint CreateTexture(std::size_t width, std::size_t height, bool immutable = false, bool integer = false) {
  GLuint texture = 0;
  glGenTextures(1, &texture);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, integer ? GL_NEAREST : GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, integer ? GL_NEAREST : GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  if (immutable) {
    glTexStorage2D(GL_TEXTURE_2D, 1, integer ? GL_RGBA8UI : GL_RGBA8, static_cast<GLsizei>(width),
                   static_cast<GLsizei>(height));
  } else {
    glTexImage2D(GL_TEXTURE_2D, 0, integer ? GL_RGBA8UI : GL_RGBA8, static_cast<GLsizei>(width),
                 static_cast<GLsizei>(height), 0, integer ? GL_RGBA_INTEGER : GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  }
  if (glGetError() != GL_NO_ERROR) {
    glDeleteTextures(1, &texture);
    throw std::runtime_error("HuxerUI backdrop texture allocation failed");
  }
  return texture;
}

void UploadRows(GLuint texture, const std::uint8_t* bgra, std::size_t width, std::size_t height, std::size_t stride,
                GLint left = 0, GLint top = 0) {
  if (bgra == nullptr || stride < width * 4 || stride % 4 != 0 ||
      stride / 4 > static_cast<std::size_t>(std::numeric_limits<GLint>::max()) ||
      height > std::numeric_limits<std::size_t>::max() / stride) {
    throw std::invalid_argument("HuxerUI backdrop raster rows require valid BGRA data and stride");
  }
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(stride / 4));
  glTexSubImage2D(GL_TEXTURE_2D, 0, left, top, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA,
                  GL_UNSIGNED_BYTE, bgra);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

} // namespace

struct BackdropGl::Resources {
  bool release_gl_objects = true;
  bool gles = false;
  GLuint fast_gaussian_program = 0;
  GLuint fast_resample_program = 0;
  unsigned kernel_fast_divisor = 0;
  std::size_t small_width = 0;
  std::size_t small_height = 0;
  GLuint blur_program = 0;
  GLuint compute_program = 0;
  GLuint integer_blur_program = 0;
  GLuint integer_compute_program = 0;
  GLuint resolve_blur_program = 0;
  GLuint resolve_compute_program = 0;
  GLuint composite_program = 0;
  GLuint transfer_program = 0;
  GLuint vertex_array = 0;
  GLuint vertex_buffer = 0;
  GLuint read_framebuffer = 0;
  GLuint draw_framebuffer = 0;
  GLuint source_texture = 0;
  GLuint output_texture = 0;
  GLuint ping_textures[2] = {0, 0};
  GLuint kernel_source = 0;
  std::size_t kernel_width = 0;
  std::size_t kernel_height = 0;
  GLuint raster_texture = 0;
  GLuint mask_texture = 0;
  Rect mask_uploaded_bounds;
  GLint texture_width = 0;
  GLint texture_height = 0;
  GLint maximum_texture_size = 0;
  std::size_t frame_width = 0;
  std::size_t frame_height = 0;
  std::size_t group_depth = 0;
  std::vector<GLuint> group_textures;
  BackdropGlStats frame_stats;
  struct CachedRaster {
    std::uint64_t identity;
    GLuint texture;
    std::uint64_t last_frame;
    Rect bounds;
  };
  std::vector<CachedRaster> cached_textures;
  std::size_t cached_texture_bytes = 0;
  std::uint64_t frame_number = 0;
  bool cache_eligible = false;
  bool cache_execution = false;
  unsigned stable_source_frames = 0;
  bool stable_full_frame = false;
  bool moving_source_window = false;
  Rect last_sampling_bounds;
  std::size_t raster_snapshot_bytes = 0;
  struct RasterSnapshot {
    Rect bounds;
    std::vector<std::uint8_t> bgra;
  };
  std::vector<std::shared_ptr<const RasterSnapshot>> raster_snapshots;
  std::vector<std::shared_ptr<const RasterSnapshot>> cached_rasters;
  std::vector<std::uint64_t> raster_identities;
  std::vector<std::uint64_t> cached_identities;
  GLuint cached_source = 0;
  bool cached_source_valid = false;
  GLint cached_left = 0;
  GLint cached_top = 0;
  std::size_t cached_width = 0;
  std::size_t cached_height = 0;
  std::size_t cache_bytes = 0;
  struct CachedBlur {
    std::size_t radius_x;
    std::size_t radius_y;
    GLuint texture;
    std::size_t left, top, width, height;
    bool fast = false;
    bool valid = true;
  };
  std::vector<CachedBlur> cached_blurs;

  void ClearCache(BackdropCacheMiss reason = BackdropCacheMiss::None) noexcept {
    if (reason == BackdropCacheMiss::SourceInputs || reason == BackdropCacheMiss::FrameSize ||
        reason == BackdropCacheMiss::Budget) {
      stable_source_frames = 0;
      moving_source_window = false;
    }
    if (cached_source_valid) {
      frame_stats.source_miss_reasons |= static_cast<std::uint64_t>(reason);
    }
    glDeleteTextures(1, &cached_source);
    cached_source = 0;
    cached_source_valid = false;
    for (const CachedBlur& blur : cached_blurs) {
      glDeleteTextures(1, &blur.texture);
    }
    cached_blurs.clear();
    cached_rasters.clear();
    cached_identities.clear();
    cached_width = 0;
    cached_height = 0;
    cache_bytes = 0;
    cache_execution = false;
  }

  void InvalidateSource(bool changed_inputs = true) noexcept {
    if (changed_inputs && cached_source_valid) {
      frame_stats.source_miss_reasons |= static_cast<std::uint64_t>(BackdropCacheMiss::SourceInputs);
    }
    if (changed_inputs) {
      stable_source_frames = 0;
      moving_source_window = false;
    }
    cached_source_valid = false;
    cache_execution = false;
    cached_rasters.clear();
    cached_identities.clear();
    // Retain bounded storage, but no kernel from an earlier source may satisfy a cache lookup.
    for (auto& blur : cached_blurs) {
      blur.valid = false;
    }
  }

  void RememberRaster(const std::uint8_t* bgra, std::size_t stride, Rect bounds) {
    if (!cache_eligible) {
      return;
    }
    const auto width = static_cast<std::size_t>(bounds.width);
    const auto height = static_cast<std::size_t>(bounds.height);
    const std::size_t bytes = width * height * 4;
    if (bytes > maximum_backdrop_cache_bytes - raster_snapshot_bytes) {
      ClearCache(BackdropCacheMiss::Budget);
      raster_snapshots.clear();
      raster_snapshot_bytes = 0;
      cache_eligible = false;
      return;
    }
    const std::size_t index = raster_snapshots.size();
    bool matches = index < cached_rasters.size() && cached_rasters[index]->bounds == bounds;
    if (matches) {
      for (std::size_t row = 0; row < height; ++row) {
        if (std::memcmp(bgra + row * stride, cached_rasters[index]->bgra.data() + row * width * 4, width * 4) != 0) {
          matches = false;
          break;
        }
      }
    }
    if (matches) {
      raster_snapshots.push_back(cached_rasters[index]);
    } else {
      InvalidateSource();
      auto snapshot = std::make_shared<RasterSnapshot>();
      snapshot->bounds = bounds;
      snapshot->bgra.resize(bytes);
      for (std::size_t row = 0; row < height; ++row) {
        std::memcpy(snapshot->bgra.data() + row * width * 4, bgra + row * stride, width * 4);
      }
      raster_snapshots.push_back(std::move(snapshot));
    }
    raster_snapshot_bytes += bytes;
  }

  GLuint RememberBlur(std::size_t radius_x, std::size_t radius_y, GLuint input, std::size_t width, std::size_t height,
                      std::size_t left, std::size_t top, bool fast = false) {
    const std::size_t bytes = width * height * 4;
    if (bytes > maximum_backdrop_cache_bytes - cached_width * cached_height * 4) {
      frame_stats.blur_miss_reasons |= static_cast<std::uint64_t>(BackdropCacheMiss::Budget);
      return input;
    }
    auto reusable = std::find_if(cached_blurs.begin(), cached_blurs.end(), [&](const auto& blur) {
      return !blur.valid && blur.width == width && blur.height == height;
    });
    const bool reuse = reusable != cached_blurs.end();
    if (!reuse) {
      while (bytes > maximum_backdrop_cache_bytes - cache_bytes && !cached_blurs.empty()) {
        cache_bytes -= cached_blurs.front().width * cached_blurs.front().height * 4;
        glDeleteTextures(1, &cached_blurs.front().texture);
        cached_blurs.erase(cached_blurs.begin());
      }
    }
    if (input != kernel_source) {
      throw std::logic_error("HuxerUI cached blur result must belong to the normalized working target");
    }
    if (!reuse) {
      cached_blurs.reserve(cached_blurs.size() + 1);
    }
    // Adopt the completed result; its replacement must remain valid for GLES image-store writes.
    const GLuint replacement = reuse ? reusable->texture : CreateTexture(width, height, compute_program != 0);
    if (reuse) {
      *reusable = {radius_x, radius_y, input, left, top, width, height, fast};
      ++frame_stats.cache_texture_reuses;
    } else {
      cached_blurs.push_back({radius_x, radius_y, input, left, top, width, height, fast});
      cache_bytes += bytes;
      ++frame_stats.cache_texture_allocations;
    }
    kernel_source = replacement;
    ++frame_stats.blur_result_adoptions;
    return input;
  }

  void Transfer(GLuint source, GLuint destination, std::size_t width, std::size_t height, int operation, float opacity,
                bool masked, GLint origin_x = 0, GLint origin_y = 0, bool blend = false, GLuint original = 0,
                Rect bounds = {}, Point destination_origin = {}) {
    glUseProgram(transfer_program);
    glBindVertexArray(vertex_array);
    glViewport(static_cast<GLint>(destination_origin.x), static_cast<GLint>(destination_origin.y),
               static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    glDisable(GL_SCISSOR_TEST);
    if (!bounds.IsEmpty()) {
      glEnable(GL_SCISSOR_TEST);
      glScissor(static_cast<GLint>(bounds.x), static_cast<GLint>(bounds.y), static_cast<GLsizei>(bounds.width),
                static_cast<GLsizei>(bounds.height));
    }
    glDisable(GL_DITHER);
    if (blend) {
      glEnable(GL_BLEND);
      glBlendEquation(GL_FUNC_ADD);
      glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    } else {
      glDisable(GL_BLEND);
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_framebuffer);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, destination, 0);
    RequireFramebuffer(GL_DRAW_FRAMEBUFFER);
    glUniform2f(glGetUniformLocation(transfer_program, "texture_size"), static_cast<float>(width),
                static_cast<float>(height));
    glUniform2i(glGetUniformLocation(transfer_program, "source_origin"), origin_x, origin_y);
    glUniform1i(glGetUniformLocation(transfer_program, "operation"), operation);
    glUniform1f(glGetUniformLocation(transfer_program, "opacity"), opacity);
    glUniform1i(glGetUniformLocation(transfer_program, "has_mask"), masked ? 1 : 0);
    glUniform1i(glGetUniformLocation(transfer_program, "source"), 0);
    glUniform1i(glGetUniformLocation(transfer_program, "original"), 1);
    glUniform1i(glGetUniformLocation(transfer_program, "clip_mask"), 2);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, original != 0 ? original : source_texture);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, mask_texture);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    RequireGlSuccess();
  }

  ~Resources() {
    if (!release_gl_objects) {
      return;
    }
    ClearCache();
    for (const auto& raster : cached_textures) {
      glDeleteTextures(1, &raster.texture);
    }
    for (const GLuint program : {fast_gaussian_program, fast_resample_program, transfer_program, composite_program,
                                 blur_program, compute_program, integer_blur_program, integer_compute_program,
                                 resolve_blur_program, resolve_compute_program}) {
      if (program != 0) {
        glDeleteProgram(program);
      }
    }
    glDeleteVertexArrays(1, &vertex_array);
    glDeleteBuffers(1, &vertex_buffer);
    glDeleteFramebuffers(1, &read_framebuffer);
    glDeleteFramebuffers(1, &draw_framebuffer);
    glDeleteTextures(1, &source_texture);
    glDeleteTextures(1, &output_texture);
    glDeleteTextures(2, ping_textures);
    glDeleteTextures(1, &kernel_source);
    glDeleteTextures(1, &raster_texture);
    glDeleteTextures(1, &mask_texture);
    for (GLuint texture : group_textures) {
      glDeleteTextures(1, &texture);
    }
  }
};

BackdropGl::BackdropGl(bool gles) {
  auto* resources = new Resources();
  resources->gles = gles;
  try {
    const std::string prefix = backdrop_glsl::FlavorPrefix(gles);
    resources->blur_program = LinkProgram(prefix + backdrop_glsl::vertex_body, prefix + backdrop_glsl::blur_body);
    resources->integer_blur_program = LinkProgram(prefix + backdrop_glsl::vertex_body,
        prefix + "#define HUXERUI_INTEGER_SOURCE\n" + backdrop_glsl::blur_body);
    resources->resolve_blur_program = LinkProgram(prefix + backdrop_glsl::vertex_body,
        prefix + "#define HUXERUI_INTEGER_SOURCE\n#define HUXERUI_NORMALIZED_OUTPUT\n" + backdrop_glsl::blur_body);
    resources->composite_program =
        LinkProgram(prefix + backdrop_glsl::vertex_body, prefix + backdrop_glsl::composite_body);
    resources->transfer_program =
        LinkProgram(prefix + backdrop_glsl::vertex_body, prefix + backdrop_glsl::transfer_body);
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    if ((gles && (major > 3 || (major == 3 && minor >= 1))) || (!gles && (major > 4 || (major == 4 && minor >= 3)))) {
      resources->compute_program = LinkComputeProgram(gles);
      resources->integer_compute_program = LinkComputeProgram(gles, true);
      resources->resolve_compute_program = LinkComputeProgram(gles, true, true);
    }
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &resources->maximum_texture_size);
    glGenVertexArrays(1, &resources->vertex_array);
    glGenBuffers(1, &resources->vertex_buffer);
    glBindVertexArray(resources->vertex_array);
    glBindBuffer(GL_ARRAY_BUFFER, resources->vertex_buffer);
    const float quad[] = {-1.0F, -1.0F, 1.0F, -1.0F, -1.0F, 1.0F, 1.0F, 1.0F};
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    const GLint position = glGetAttribLocation(resources->blur_program, "position");
    if (position >= 0) {
      glVertexAttribPointer(static_cast<GLuint>(position), 2, GL_FLOAT, GL_FALSE, 0, nullptr);
      glEnableVertexAttribArray(static_cast<GLuint>(position));
    }
    glGenFramebuffers(1, &resources->read_framebuffer);
    glGenFramebuffers(1, &resources->draw_framebuffer);
    glGenTextures(1, &resources->source_texture);
    glGenTextures(1, &resources->output_texture);
    glGenTextures(2, resources->ping_textures);
    glGenTextures(1, &resources->kernel_source);
    for (const GLuint texture : {resources->source_texture, resources->output_texture, resources->ping_textures[0],
                                 resources->ping_textures[1], resources->kernel_source}) {
      glBindTexture(GL_TEXTURE_2D, texture);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    if (glGetError() != GL_NO_ERROR) {
      throw std::runtime_error("HuxerUI backdrop GL initialization left errors behind");
    }
  } catch (...) {
    delete resources;
    throw;
  }
  resources_ = resources;
}

BackdropGl::~BackdropGl() {
  delete resources_;
}

void BackdropGl::Abandon() noexcept {
  if (resources_ != nullptr) {
    resources_->release_gl_objects = false;
    delete resources_;
    resources_ = nullptr;
  }
}

bool BackdropGl::Apply(const BackdropPlaneBatch& batch, std::uint8_t* bgra, std::size_t width, std::size_t height,
                       float device_left, float device_top) {
  if (bgra == nullptr) {
    throw std::invalid_argument("HuxerUI backdrop CPU transfer requires pixel data");
  }
  if (resources_ != nullptr) {
    resources_->cache_execution = false;
  }
  return ExecutePlane(batch, bgra, width, height, device_left, device_top);
}

void BackdropGl::EnsureCaptureSize(std::size_t width, std::size_t height) {
  if (resources_->texture_width == static_cast<GLint>(width) &&
      resources_->texture_height == static_cast<GLint>(height)) {
    return;
  }
  BackdropCpuTimer timer(resources_->frame_stats.capture_resize_cpu_ns);
  glActiveTexture(GL_TEXTURE0);
  for (const GLuint texture : {resources_->source_texture, resources_->output_texture}) {
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
  }
  RequireGlSuccess();
  resources_->texture_width = static_cast<GLint>(width);
  resources_->texture_height = static_cast<GLint>(height);
}

bool BackdropGl::ExecutePlane(const BackdropPlaneBatch& batch, std::uint8_t* bgra, std::size_t width,
                              std::size_t height, float device_left, float device_top) {
  if (resources_ == nullptr || batch.commands.empty() || width == 0 || height == 0 ||
      width > maximum_backdrop_capture_pixels / height ||
      width > static_cast<std::size_t>(resources_->maximum_texture_size) ||
      height > static_cast<std::size_t>(resources_->maximum_texture_size)) {
    return false;
  }
  struct MemberPlan {
    const BackdropPlaneCommand* member = nullptr;
    Transform2D local_to_capture;
    Transform2D capture_to_local;
    float pixel_size = 1.0F;
    std::size_t radius_x = 0;
    std::size_t radius_y = 0;
    unsigned fast_divisor = 0;
    std::size_t left = 0;
    std::size_t top = 0;
    std::size_t right = 0;
    std::size_t bottom = 0;
  };
  const Transform2D capture_offset = TranslationTransform({-device_left, -device_top});
  std::vector<MemberPlan> plans;
  plans.reserve(batch.commands.size());
  for (const BackdropPlaneCommand& member : batch.commands) {
    MemberPlan plan;
    plan.member = &member;
    plan.local_to_capture = ComposeTransform(capture_offset, member.local_to_device);
    const auto inverse = InverseTransform(plan.local_to_capture);
    if (!inverse) {
      return false;
    }
    plan.capture_to_local = *inverse;
    plan.pixel_size = std::max(std::hypot(plan.capture_to_local.m11, plan.capture_to_local.m12),
                               std::hypot(plan.capture_to_local.m21, plan.capture_to_local.m22));
    if (!std::isfinite(plan.pixel_size) || plan.pixel_size <= 0.0F) {
      return false;
    }
    if (!BlurRadii(plan.local_to_capture, member.command->effect.blur_radius, width, height, plan.radius_x,
                   plan.radius_y)) {
      return false;
    }
    if (member.command->effect.blur_quality == BackdropBlurQuality::Fast && plan.pixel_size <= 1.0F &&
        (plan.radius_x != 0 || plan.radius_y != 0)) {
      const auto sigma = [](std::size_t radius) {
        return std::sqrt(static_cast<double>(radius) * (radius + 1));
      };
      const double minimum_sigma = sigma(std::min(plan.radius_x, plan.radius_y));
      const unsigned divisor = minimum_sigma <= 4 ? 1U : minimum_sigma <= 8 ? 2U : 4U;
      if (std::ceil(3 * sigma(std::max(plan.radius_x, plan.radius_y)) / divisor) <= 128) {
        plan.fast_divisor = divisor;
      }
    }
    Rect sampling = TransformBounds(plan.local_to_capture, BackdropSamplingBounds(*member.command));
    sampling = Rect{sampling.x - 3.0F, sampling.y - 3.0F, sampling.width + 6.0F, sampling.height + 6.0F};
    const auto clamp_start = [](float value, std::size_t extent) {
      return static_cast<std::size_t>(std::clamp(std::floor(value), 0.0F, static_cast<float>(extent)));
    };
    const auto clamp_end = [](float value, std::size_t extent) {
      return static_cast<std::size_t>(std::clamp(std::ceil(value), 0.0F, static_cast<float>(extent)));
    };
    plan.left = clamp_start(sampling.x, width);
    plan.top = clamp_start(sampling.y, height);
    plan.right = clamp_end(sampling.x + sampling.width, width);
    plan.bottom = clamp_end(sampling.y + sampling.height, height);
    if (plan.right <= plan.left || plan.bottom <= plan.top) {
      continue;
    }
    plans.push_back(plan);
  }
  if (plans.empty()) {
    return true;
  }

  EnsureCaptureSize(width, height);
  if (bgra != nullptr) {
    UploadRows(resources_->source_texture, bgra, width, height, width * 4);
  }
  const GLuint source_texture = resources_->cache_execution ? resources_->cached_source : resources_->source_texture;

  // Start the output texture from the unfiltered source so scissored member regions leave the rest
  // of the plane untouched.
  glBindFramebuffer(GL_READ_FRAMEBUFFER, resources_->read_framebuffer);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, source_texture, 0);
  RequireFramebuffer(GL_READ_FRAMEBUFFER);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resources_->draw_framebuffer);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resources_->output_texture, 0);
  RequireFramebuffer(GL_DRAW_FRAMEBUFFER);
  glBlitFramebuffer(0, 0, static_cast<GLint>(width), static_cast<GLint>(height), 0, 0, static_cast<GLint>(width),
                    static_cast<GLint>(height), GL_COLOR_BUFFER_BIT, GL_NEAREST);

  struct BlurKey {
    std::size_t x;
    std::size_t y;
    bool fast;
    bool operator==(const BlurKey&) const = default;
  };
  std::vector<BlurKey> completed_blurs;
  struct FilteredRegion {
    GLuint texture;
    std::size_t left, top, width, height;
  };
  const auto blurred_texture = [&](const MemberPlan& plan) -> FilteredRegion {
    if (plan.radius_x == 0 && plan.radius_y == 0) {
      return {source_texture, 0, 0, width, height};
    }
    std::size_t left = width, top = height, right = 0, bottom = 0;
    for (const auto& member : plans) {
      if (member.radius_x == plan.radius_x && member.radius_y == plan.radius_y &&
          (member.fast_divisor != 0) == (plan.fast_divisor != 0)) {
        left = std::min(left, member.left);
        top = std::min(top, member.top);
        right = std::max(right, member.right);
        bottom = std::max(bottom, member.bottom);
      }
    }
    if (resources_->cache_execution) {
      left = left / backdrop_capture_tile * backdrop_capture_tile;
      top = top / backdrop_capture_tile * backdrop_capture_tile;
      right = std::min(width, (right + backdrop_capture_tile - 1) / backdrop_capture_tile * backdrop_capture_tile);
      bottom = std::min(height, (bottom + backdrop_capture_tile - 1) / backdrop_capture_tile * backdrop_capture_tile);
    }
    if (resources_->stable_full_frame) {
      left = top = 0;
      right = width;
      bottom = height;
    }
    const std::size_t kernel_width = right - left, kernel_height = bottom - top;
    if (resources_->cache_execution) {
      for (const Resources::CachedBlur& blur : resources_->cached_blurs) {
        if (blur.valid && blur.radius_x == plan.radius_x && blur.radius_y == plan.radius_y &&
            blur.fast == (plan.fast_divisor != 0) && blur.left == left &&
            blur.top == top && blur.width == kernel_width && blur.height == kernel_height) {
          ++resources_->frame_stats.blur_cache_hits;
          return {blur.texture, left, top, kernel_width, kernel_height};
        }
      }
    }
    const bool radius_cached = std::any_of(resources_->cached_blurs.begin(), resources_->cached_blurs.end(),
                                           [&](const auto& blur) {
                                             return blur.valid && blur.radius_x == plan.radius_x &&
                                                    blur.radius_y == plan.radius_y &&
                                                    blur.fast == (plan.fast_divisor != 0);
                                           });
    const auto miss = !resources_->cache_execution ? BackdropCacheMiss::Ineligible
                      : resources_->cached_blurs.empty() ? BackdropCacheMiss::MissingKernel
                      : radius_cached ? BackdropCacheMiss::KernelGeometry : BackdropCacheMiss::KernelRadius;
    resources_->frame_stats.blur_miss_reasons |= static_cast<std::uint64_t>(miss);
    const unsigned divisor = plan.fast_divisor;
    // Anchor the downsampling lattice in device space so capture movement does not change its phase.
    const float grid_left = divisor ? std::floor((device_left + left) / divisor) * divisor - device_left : 0;
    const float grid_top = divisor ? std::floor((device_top + top) / divisor) * divisor - device_top : 0;
    const auto small_width = divisor ? static_cast<std::size_t>(std::ceil((right - grid_left) / divisor))
                                     : kernel_width;
    const auto small_height = divisor ? static_cast<std::size_t>(std::ceil((bottom - grid_top) / divisor))
                                      : kernel_height;
    if (resources_->kernel_width != kernel_width || resources_->kernel_height != kernel_height ||
        resources_->kernel_fast_divisor != divisor || resources_->small_width != small_width ||
        resources_->small_height != small_height) {
      BackdropCpuTimer timer(resources_->frame_stats.kernel_resize_cpu_ns);
      resources_->kernel_width = resources_->kernel_height = 0;
      glDeleteTextures(1, &resources_->kernel_source);
      resources_->kernel_source = 0;
      glDeleteTextures(2, resources_->ping_textures);
      resources_->ping_textures[0] = resources_->ping_textures[1] = 0;
      resources_->kernel_source = CreateTexture(kernel_width, kernel_height, resources_->compute_program != 0);
      for (auto& texture : resources_->ping_textures) {
        texture = divisor ? CreateTexture(small_width, small_height)
                          : CreateTexture(kernel_width, kernel_height, resources_->compute_program != 0, true);
      }
      RequireGlSuccess();
      resources_->kernel_width = kernel_width;
      resources_->kernel_height = kernel_height;
      resources_->kernel_fast_divisor = divisor;
      resources_->small_width = small_width;
      resources_->small_height = small_height;
    }
    if (divisor != 0) {
      const std::string prefix = backdrop_glsl::FlavorPrefix(resources_->gles);
      if (resources_->fast_gaussian_program == 0) {
        resources_->fast_gaussian_program = LinkProgram(prefix + backdrop_glsl::vertex_body,
                                                       prefix + backdrop_glsl::fast_gaussian_body);
      }
      if (resources_->fast_resample_program == 0) {
        resources_->fast_resample_program = LinkProgram(prefix + backdrop_glsl::vertex_body,
                                                       prefix + backdrop_glsl::fast_resample_body);
      }
      BackdropCpuTimer timer(resources_->frame_stats.blur_submit_cpu_ns);
      glBindVertexArray(resources_->vertex_array);
      glDisable(GL_SCISSOR_TEST);
      glDisable(GL_BLEND);
      glActiveTexture(GL_TEXTURE0);
      const auto target = [&](GLuint texture, std::size_t w, std::size_t h) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resources_->draw_framebuffer);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        RequireFramebuffer(GL_DRAW_FRAMEBUFFER);
        glViewport(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h));
        ++resources_->frame_stats.blur_passes;
        resources_->frame_stats.blur_pixels += w * h;
      };
      const auto resample = [&](GLuint source, std::size_t sw, std::size_t sh, GLuint destination,
                                std::size_t dw, std::size_t dh, Point origin, float scale, int pairs) {
        const GLuint program = resources_->fast_resample_program;
        glUseProgram(program);
        target(destination, dw, dh);
        glBindTexture(GL_TEXTURE_2D, source);
        glUniform1i(glGetUniformLocation(program, "source"), 0);
        glUniform2f(glGetUniformLocation(program, "texture_size"), dw, dh);
        glUniform2f(glGetUniformLocation(program, "source_size"), sw, sh);
        glUniform2f(glGetUniformLocation(program, "source_origin"), origin.x, origin.y);
        glUniform1f(glGetUniformLocation(program, "source_scale"), scale);
        glUniform1i(glGetUniformLocation(program, "sample_pairs"), pairs);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      };
      resample(source_texture, width, height, resources_->ping_textures[0], small_width, small_height,
               {grid_left, grid_top}, divisor, divisor == 4 ? 2 : 1);
      const auto gaussian = [&](GLuint source, GLuint destination, std::size_t radius, bool horizontal) {
        const double sigma = std::sqrt(static_cast<double>(radius) * (radius + 1)) / divisor;
        const int support = static_cast<int>(std::ceil(3 * sigma));

        std::array<float, 130> samples{};
        const auto weight = [&](int distance) {
          return sigma > 0 ? std::exp(-0.5 * distance * distance / (sigma * sigma)) : 1.0;
        };
        double sum = 1;
        for (int index = 1; index <= support; ++index) sum += 2 * weight(index);
        samples[0] = static_cast<float>(1 / sum);
        int count = 1;
        for (int index = 1; index <= support; index += 2) {
          const double a = weight(index), b = index < support ? weight(index + 1) : 0;
          samples[count * 2] = static_cast<float>((a + b) / sum);
          samples[count * 2 + 1] = static_cast<float>(index + b / (a + b));
          ++count;
        }
        const GLuint program = resources_->fast_gaussian_program;
        glUseProgram(program);
        target(destination, small_width, small_height);
        glBindTexture(GL_TEXTURE_2D, source);
        glUniform1i(glGetUniformLocation(program, "source"), 0);
        glUniform2f(glGetUniformLocation(program, "texture_size"), small_width, small_height);
        glUniform2f(glGetUniformLocation(program, "direction"), horizontal ? 1 : 0, horizontal ? 0 : 1);
        glUniform1i(glGetUniformLocation(program, "sample_count"), count);
        glUniform2fv(glGetUniformLocation(program, "samples"), count, samples.data());
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      };
      gaussian(resources_->ping_textures[0], resources_->ping_textures[1], plan.radius_x, true);
      gaussian(resources_->ping_textures[1], resources_->ping_textures[0], plan.radius_y, false);
      resample(resources_->ping_textures[0], small_width, small_height, resources_->kernel_source,
               kernel_width, kernel_height, {(left - grid_left) / divisor, (top - grid_top) / divisor},
               1.0F / divisor, 1);
      const GLuint result = resources_->cache_execution ? resources_->RememberBlur(plan.radius_x, plan.radius_y,
          resources_->kernel_source, kernel_width, kernel_height, left, top, true) : resources_->kernel_source;
      return {result, left, top, kernel_width, kernel_height};
    }
    GLuint input = source_texture;
    if (left != 0 || top != 0 || kernel_width != width || kernel_height != height) {
      // A cropped kernel needs its own edge-clamping domain; a complete source already has that domain.
      glBindFramebuffer(GL_READ_FRAMEBUFFER, resources_->read_framebuffer);
      glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, source_texture, 0);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resources_->draw_framebuffer);
      glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resources_->kernel_source, 0);
      RequireFramebuffer(GL_READ_FRAMEBUFFER);
      RequireFramebuffer(GL_DRAW_FRAMEBUFFER);
      glDisable(GL_SCISSOR_TEST);
      glBlitFramebuffer(static_cast<GLint>(left), static_cast<GLint>(top), static_cast<GLint>(right),
                        static_cast<GLint>(bottom), 0, 0, static_cast<GLint>(kernel_width),
                        static_cast<GLint>(kernel_height), GL_COLOR_BUFFER_BIT, GL_NEAREST);
      resources_->frame_stats.kernel_source_copy_pixels += kernel_width * kernel_height;
      input = resources_->kernel_source;
    } else {
      ++resources_->frame_stats.kernel_source_reuses;
    }
    GLuint write = resources_->ping_textures[0];
    const bool compute = resources_->compute_program != 0 && std::max(kernel_width, kernel_height) >= 1024 &&
                         std::max(plan.radius_x, plan.radius_y) >= 12;
    glBindVertexArray(resources_->vertex_array);
    glViewport(0, 0, static_cast<GLsizei>(kernel_width), static_cast<GLsizei>(kernel_height));
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glActiveTexture(GL_TEXTURE0);
    bool integer_input = false;
    GLuint current_program = 0;
    GLint direction = 0, radius_location = 0;
    {
      BackdropCpuTimer timer(resources_->frame_stats.blur_submit_cpu_ns);
      for (int pass = 0; pass != 3; ++pass) {
        const std::pair<bool, std::size_t> steps[] = {{true, plan.radius_x}, {false, plan.radius_y}};
        for (const auto& [horizontal, radius] : steps) {
          const bool normalized_output = pass == 2 && !horizontal;
          const GLuint destination = normalized_output ? resources_->kernel_source : write;
          const GLuint program = compute
              ? (normalized_output ? resources_->resolve_compute_program
                 : integer_input ? resources_->integer_compute_program : resources_->compute_program)
              : (normalized_output ? resources_->resolve_blur_program
                 : integer_input ? resources_->integer_blur_program : resources_->blur_program);
          if (program != current_program) {
            glUseProgram(program);
            glUniform1i(glGetUniformLocation(program, "source"), 0);
            if (!compute) {
              glUniform2f(glGetUniformLocation(program, "texture_size"), static_cast<float>(kernel_width),
                          static_cast<float>(kernel_height));
            }
            direction = glGetUniformLocation(program, "direction");
            radius_location = glGetUniformLocation(program, "radius");
            current_program = program;
          }
          glBindTexture(GL_TEXTURE_2D, input);
          glUniform1i(radius_location, static_cast<GLint>(radius));
          if (compute) {
            const std::uint64_t divisor = 2 * radius + 1;
            const unsigned shift = divisor <= 255 ? 16 : 32;
            const auto reciprocal = divisor == 1 ? 0 : ((1ULL << shift) + divisor - 1) / divisor;
            glUniform1ui(glGetUniformLocation(program, "reciprocal"), static_cast<GLuint>(reciprocal));
          }
          glUniform2i(direction, horizontal ? 1 : 0, horizontal ? 0 : 1);
          if (compute) {
            glBindImageTexture(0, destination, 0, GL_FALSE, 0, GL_WRITE_ONLY,
                               normalized_output ? GL_RGBA8 : GL_RGBA8UI);
            const auto lines = horizontal ? kernel_height : kernel_width;
            const auto extent = horizontal ? kernel_width : kernel_height;
            glDispatchCompute(static_cast<GLuint>((lines + 63) / 64), static_cast<GLuint>((extent + 127) / 128), 1);
            glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                            GL_FRAMEBUFFER_BARRIER_BIT);
            ++resources_->frame_stats.compute_blur_passes;
          } else {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resources_->draw_framebuffer);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, destination, 0);
            RequireFramebuffer(GL_DRAW_FRAMEBUFFER);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
          }
          ++resources_->frame_stats.blur_passes;
          ++resources_->frame_stats.integer_blur_passes;
          integer_input = true;
          resources_->frame_stats.blur_pixels += kernel_width * kernel_height;
          input = destination;
          write = write == resources_->ping_textures[0] ? resources_->ping_textures[1] : resources_->ping_textures[0];
        }
      }
    }
    if (compute) {
      glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    }
    const GLuint result = resources_->cache_execution ? resources_->RememberBlur(plan.radius_x, plan.radius_y, input,
                                                                                 kernel_width, kernel_height, left, top)
                                                      : input;
    return {result, left, top, kernel_width, kernel_height};
  };
  // A blur result is consumed before another radius reuses the ping-pong textures. Batch outputs
  // are independent, so members sharing a physical radius may composite together in any order.
  for (const MemberPlan& blur_plan : plans) {
    const BlurKey key{blur_plan.radius_x, blur_plan.radius_y, blur_plan.fast_divisor != 0};
    if (std::find(completed_blurs.begin(), completed_blurs.end(), key) != completed_blurs.end()) {
      continue;
    }
    const auto filtered = blurred_texture(blur_plan);
    completed_blurs.push_back(key);
    const GLint pixel_to_local = glGetUniformLocation(resources_->composite_program, "pixel_to_local");
    const GLint local_to_pixel = glGetUniformLocation(resources_->composite_program, "local_to_pixel");
    const GLint pixel_size = glGetUniformLocation(resources_->composite_program, "pixel_size");
    const GLint refraction = glGetUniformLocation(resources_->composite_program, "refraction");
    const GLint edge_width = glGetUniformLocation(resources_->composite_program, "edge_width");
    const GLint highlight = glGetUniformLocation(resources_->composite_program, "highlight");
    const GLint saturation = glGetUniformLocation(resources_->composite_program, "saturation");
    const GLint brightness = glGetUniformLocation(resources_->composite_program, "brightness");
    const GLint fusion_radius = glGetUniformLocation(resources_->composite_program, "fusion_radius");
    const GLint light = glGetUniformLocation(resources_->composite_program, "light");
    const GLint shape_rect = glGetUniformLocation(resources_->composite_program, "shape_rect");
    const GLint shape_radii = glGetUniformLocation(resources_->composite_program, "shape_radii");
    const GLint shape_count = glGetUniformLocation(resources_->composite_program, "shape_count");
    glUseProgram(resources_->composite_program);
    glBindVertexArray(resources_->vertex_array);
    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    glDisable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resources_->draw_framebuffer);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resources_->output_texture, 0);
    glUniform1i(glGetUniformLocation(resources_->composite_program, "blurred"), 0);
    glUniform1i(glGetUniformLocation(resources_->composite_program, "original"), 1);
    glUniform2f(glGetUniformLocation(resources_->composite_program, "texture_size"), static_cast<float>(width),
                static_cast<float>(height));
    for (const MemberPlan& plan : plans) {
      if (plan.radius_x != key.x || plan.radius_y != key.y || (plan.fast_divisor != 0) != key.fast) {
        continue;
      }
      const DrawBackdropCommand& command = *plan.member->command;
      const auto& effect = command.effect;
      if (effect.blur_quality == BackdropBlurQuality::Fast && effect.blur_radius == 0 && effect.refraction == 0 &&
          effect.highlight == 0 && effect.saturation == 1 && effect.brightness == 1) {
        // The output already contains the source; an identity material needs no floating-point resampling.
        continue;
      }
      glScissor(static_cast<GLint>(plan.left), static_cast<GLint>(plan.top),
                static_cast<GLsizei>(plan.right - plan.left), static_cast<GLsizei>(plan.bottom - plan.top));
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, filtered.texture);
      glUniform2f(glGetUniformLocation(resources_->composite_program, "blurred_origin"),
                  static_cast<float>(filtered.left), static_cast<float>(filtered.top));
      glUniform2f(glGetUniformLocation(resources_->composite_program, "blurred_size"),
                  static_cast<float>(filtered.width), static_cast<float>(filtered.height));
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, source_texture);
      const std::array<float, 9> capture_to_local = ColumnMajor(plan.capture_to_local);
      const std::array<float, 9> local_to_capture = ColumnMajor(plan.local_to_capture);
      glUniformMatrix3fv(pixel_to_local, 1, GL_FALSE, capture_to_local.data());
      glUniformMatrix3fv(local_to_pixel, 1, GL_FALSE, local_to_capture.data());
      glUniform1f(pixel_size, plan.pixel_size);
      glUniform1f(refraction, command.effect.refraction);
      glUniform1f(edge_width, command.effect.edge_width);
      glUniform1f(highlight, command.effect.highlight);
      glUniform1f(saturation, command.effect.saturation);
      glUniform1f(brightness, command.effect.brightness);
      glUniform1f(fusion_radius, command.effect.fusion_radius);
      const float light_length = std::hypot(command.effect.light_direction.x, command.effect.light_direction.y);
      glUniform2f(light, command.effect.light_direction.x / light_length,
                  command.effect.light_direction.y / light_length);
      std::array<float, 64 * 4> rectangles;
      std::array<float, 64 * 4> corners;
      GLint index = 0;
      for (const BackdropShape& shape : command.shapes) {
        if (shape.rect.IsEmpty()) {
          continue;
        }
        const CornerRadii radii = NormalizeCornerRadii(shape.rect, shape.corner_radii);
        const float rect_uniform[4] = {shape.rect.x, shape.rect.y, shape.rect.width, shape.rect.height};
        const float radii_uniform[4] = {radii.top_left, radii.top_right, radii.bottom_right, radii.bottom_left};
        std::copy_n(rect_uniform, 4, rectangles.data() + index * 4);
        std::copy_n(radii_uniform, 4, corners.data() + index * 4);
        ++index;
      }
      // Submit each array together so the driver owns element addressing.
      glUniform4fv(shape_rect, index, rectangles.data());
      glUniform4fv(shape_radii, index, corners.data());
      glUniform1i(shape_count, index);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
  }
  glDisable(GL_SCISSOR_TEST);
  glBindVertexArray(0);
  glUseProgram(0);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, 0);

  if (bgra != nullptr) {
    glBindFramebuffer(GL_FRAMEBUFFER, resources_->draw_framebuffer);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, bgra);
    resources_->frame_stats.readback_bytes += width * height * 4;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  RequireGlSuccess();
  return true;
}

bool BackdropGl::BeginFrame(std::size_t width, std::size_t height) {
  if (resources_ == nullptr || width == 0 || height == 0 || width > maximum_backdrop_capture_pixels / height ||
      width > static_cast<std::size_t>(resources_->maximum_texture_size) ||
      height > static_cast<std::size_t>(resources_->maximum_texture_size)) {
    return false;
  }
  CancelFrame();
  resources_->frame_stats = {};
  ++resources_->frame_number;
  resources_->cache_eligible = true;
  if (resources_->frame_width != width || resources_->frame_height != height) {
    resources_->ClearCache(BackdropCacheMiss::FrameSize);
    for (const auto& raster : resources_->cached_textures) {
      glDeleteTextures(1, &raster.texture);
    }
    resources_->cached_textures.clear();
    resources_->cached_texture_bytes = 0;
    for (GLuint texture : resources_->group_textures) {
      glDeleteTextures(1, &texture);
    }
    resources_->group_textures.clear();
    glDeleteTextures(1, &resources_->raster_texture);
    glDeleteTextures(1, &resources_->mask_texture);
    resources_->raster_texture = 0;
    resources_->mask_texture = 0;
    resources_->frame_width = 0;
    resources_->frame_height = 0;
    resources_->raster_texture = CreateTexture(width, height);
    resources_->mask_texture = CreateTexture(width, height);
    resources_->mask_uploaded_bounds = {};
    resources_->frame_width = width;
    resources_->frame_height = height;
  }
  if (resources_->group_textures.empty()) {
    const GLuint texture = CreateTexture(width, height);
    try {
      resources_->group_textures.push_back(texture);
    } catch (...) {
      glDeleteTextures(1, &texture);
      throw;
    }
  }
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resources_->draw_framebuffer);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resources_->group_textures.front(),
                         0);
  RequireFramebuffer(GL_DRAW_FRAMEBUFFER);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
  glClear(GL_COLOR_BUFFER_BIT);
  RequireGlSuccess();
  resources_->group_depth = 1;
  return true;
}

void BackdropGl::CompositeRaster(const std::uint8_t* bgra, std::size_t stride) {
  if (resources_ == nullptr) {
    throw std::logic_error("HuxerUI backdrop raster composition requires an active frame");
  }
  CompositeRaster(bgra, stride, {0, 0, static_cast<float>(Width()), static_cast<float>(Height())});
}

void BackdropGl::CompositeRaster(const std::uint8_t* bgra, std::size_t stride, Rect bounds) {
  if (resources_ == nullptr || resources_->group_depth == 0) {
    throw std::logic_error("HuxerUI backdrop raster composition requires an active frame");
  }
  if (!std::isfinite(bounds.x) || !std::isfinite(bounds.y) || !std::isfinite(bounds.width) ||
      !std::isfinite(bounds.height) || bounds.x < 0 || bounds.y < 0 || bounds.width < 0 || bounds.height < 0 ||
      bounds.x + bounds.width > resources_->frame_width || bounds.y + bounds.height > resources_->frame_height ||
      std::floor(bounds.x) != bounds.x || std::floor(bounds.y) != bounds.y ||
      std::floor(bounds.width) != bounds.width || std::floor(bounds.height) != bounds.height) {
    throw std::invalid_argument("HuxerUI backdrop raster bounds must be integral and inside the frame");
  }
  if (bounds.IsEmpty()) {
    return;
  }
  if (bgra == nullptr || stride < resources_->frame_width * 4 || stride % 4 != 0 ||
      stride / 4 > static_cast<std::size_t>(std::numeric_limits<GLint>::max()) ||
      resources_->frame_height > std::numeric_limits<std::size_t>::max() / stride) {
    throw std::invalid_argument("HuxerUI backdrop raster rows require valid BGRA data and stride");
  }
  const auto width = static_cast<std::size_t>(bounds.width);
  const auto height = static_cast<std::size_t>(bounds.height);
  const auto* pixels = bgra + static_cast<std::size_t>(bounds.y) * stride + static_cast<std::size_t>(bounds.x) * 4;
  UploadRows(resources_->raster_texture, pixels, width, height, stride, static_cast<GLint>(bounds.x),
             static_cast<GLint>(bounds.y));
  resources_->Transfer(resources_->raster_texture, resources_->group_textures[resources_->group_depth - 1],
                       resources_->frame_width, resources_->frame_height, 0, 1.0F, false, 0, 0, true, 0, bounds);
  resources_->frame_stats.raster_upload_bytes += width * height * 4;
  resources_->frame_stats.raster_composite_pixels += width * height;
  resources_->RememberRaster(pixels, stride, bounds);
}

bool BackdropGl::CompositeCachedTexture(std::uint64_t identity) {
  if (identity == 0 || resources_ == nullptr || resources_->group_depth == 0) {
    return false;
  }
  for (auto& raster : resources_->cached_textures) {
    if (raster.identity == identity) {
      raster.last_frame = resources_->frame_number;
      const auto width = static_cast<std::size_t>(raster.bounds.width);
      const auto height = static_cast<std::size_t>(raster.bounds.height);
      resources_->Transfer(raster.texture, resources_->group_textures[resources_->group_depth - 1], width, height, 2,
                           1.0F, false, 0, 0, true, 0, {}, {raster.bounds.x, raster.bounds.y});
      resources_->frame_stats.raster_composite_pixels += width * height;
      ++resources_->frame_stats.raster_cache_hits;
      if (resources_->cache_eligible) {
        const std::size_t index = resources_->raster_identities.size();
        if (index >= resources_->cached_identities.size() || resources_->cached_identities[index] != identity) {
          resources_->InvalidateSource();
        }
        resources_->raster_identities.push_back(identity);
      }
      return true;
    }
  }
  return false;
}

void BackdropGl::CompositeTexture(unsigned int rgba_texture, std::uint64_t identity) {
  if (resources_ == nullptr) {
    throw std::logic_error("HuxerUI backdrop raster import requires an active frame");
  }
  CompositeTexture(rgba_texture, identity, {0, 0, static_cast<float>(Width()), static_cast<float>(Height())});
}

void BackdropGl::CompositeTexture(unsigned int rgba_texture, std::uint64_t identity, Rect bounds, bool cache_texture) {
  if (resources_ == nullptr || resources_->group_depth == 0) {
    throw std::logic_error("HuxerUI backdrop raster import requires an active frame");
  }
  if (!std::isfinite(bounds.x) || !std::isfinite(bounds.y) || !std::isfinite(bounds.width) ||
      !std::isfinite(bounds.height) || bounds.x < 0 || bounds.y < 0 || bounds.width <= 0 || bounds.height <= 0 ||
      bounds.x + bounds.width > resources_->frame_width || bounds.y + bounds.height > resources_->frame_height ||
      std::floor(bounds.x) != bounds.x || std::floor(bounds.y) != bounds.y ||
      std::floor(bounds.width) != bounds.width || std::floor(bounds.height) != bounds.height) {
    throw std::invalid_argument("HuxerUI backdrop texture bounds must be integral and inside the frame");
  }
  const auto width = static_cast<std::size_t>(bounds.width);
  const auto height = static_cast<std::size_t>(bounds.height);
  resources_->Transfer(rgba_texture, resources_->group_textures[resources_->group_depth - 1], width, height, 2, 1.0F,
                       false, 0, 0, true, 0, {}, {bounds.x, bounds.y});
  resources_->frame_stats.raster_composite_pixels += width * height;
  ++resources_->frame_stats.raster_imports;
  if (resources_->cache_eligible) {
    if (identity == 0) {
      resources_->InvalidateSource();
      resources_->cache_eligible = false;
    } else {
      const std::size_t index = resources_->raster_identities.size();
      if (index >= resources_->cached_identities.size() || resources_->cached_identities[index] != identity) {
        resources_->InvalidateSource();
      }
      resources_->raster_identities.push_back(identity);
    }
  }
  const std::size_t bytes = width * height * 4;
  if (cache_texture && identity != 0 && bytes <= maximum_backdrop_cache_bytes) {
    while ((bytes > maximum_backdrop_cache_bytes - resources_->cached_texture_bytes ||
            resources_->cached_textures.size() >= 64) &&
           !resources_->cached_textures.empty()) {
      const auto stale =
          std::find_if(resources_->cached_textures.begin(), resources_->cached_textures.end(),
                       [&](const auto& entry) { return resources_->frame_number - entry.last_frame > 1; });
      // Keep the preceding frame's working set so a fifth large segment does not evict four reusable segments.
      if (stale == resources_->cached_textures.end()) {
        return;
      }
      glDeleteTextures(1, &stale->texture);
      resources_->cached_texture_bytes -=
          static_cast<std::size_t>(stale->bounds.width) * static_cast<std::size_t>(stale->bounds.height) * 4;
      resources_->cached_textures.erase(stale);
    }
    const GLuint immutable = CreateTexture(width, height);
    try {
      glBindFramebuffer(GL_READ_FRAMEBUFFER, resources_->read_framebuffer);
      glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rgba_texture, 0);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resources_->draw_framebuffer);
      glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, immutable, 0);
      RequireFramebuffer(GL_READ_FRAMEBUFFER);
      RequireFramebuffer(GL_DRAW_FRAMEBUFFER);
      glDisable(GL_SCISSOR_TEST);
      glBlitFramebuffer(0, 0, static_cast<GLint>(width), static_cast<GLint>(height), 0, 0, static_cast<GLint>(width),
                        static_cast<GLint>(height), GL_COLOR_BUFFER_BIT, GL_NEAREST);
      RequireGlSuccess();
      resources_->cached_textures.push_back({identity, immutable, resources_->frame_number, bounds});
      resources_->cached_texture_bytes += bytes;
      resources_->frame_stats.raster_cache_copy_pixels += width * height;
    } catch (...) {
      glDeleteTextures(1, &immutable);
      throw;
    }
  }
}

void BackdropGl::PushGroup() {
  if (resources_ == nullptr || resources_->group_depth == 0) {
    throw std::logic_error("HuxerUI backdrop opacity group requires an active frame");
  }
  const std::size_t bytes = resources_->frame_width * resources_->frame_height * 4;
  if (resources_->group_depth >= maximum_backdrop_group_bytes / bytes) {
    throw std::runtime_error("HuxerUI backdrop GPU opacity groups exceed the capture budget");
  }
  resources_->cache_eligible = false;
  resources_->raster_snapshots.clear();
  resources_->raster_snapshot_bytes = 0;
  if (resources_->group_depth == resources_->group_textures.size()) {
    const GLuint texture = CreateTexture(resources_->frame_width, resources_->frame_height);
    try {
      resources_->group_textures.push_back(texture);
    } catch (...) {
      glDeleteTextures(1, &texture);
      throw;
    }
  }
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resources_->draw_framebuffer);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         resources_->group_textures[resources_->group_depth], 0);
  RequireFramebuffer(GL_DRAW_FRAMEBUFFER);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
  glClear(GL_COLOR_BUFFER_BIT);
  RequireGlSuccess();
  ++resources_->group_depth;
}

void BackdropGl::PopGroup(const std::uint8_t* mask_bgra, std::size_t mask_stride, float opacity, bool mask_changed) {
  if (resources_ == nullptr || resources_->group_depth <= 1) {
    throw std::logic_error("HuxerUI backdrop opacity group stack is unbalanced");
  }
  if (!std::isfinite(opacity) || opacity < 0.0F || opacity > 1.0F) {
    throw std::invalid_argument("HuxerUI backdrop group opacity must be in the range [0, 1]");
  }
  if (mask_bgra != nullptr) {
    if (mask_changed || resources_->mask_uploaded_bounds != Rect{0, 0, static_cast<float>(resources_->frame_width),
                                                                 static_cast<float>(resources_->frame_height)}) {
      UploadRows(resources_->mask_texture, mask_bgra, resources_->frame_width, resources_->frame_height, mask_stride);
      resources_->frame_stats.mask_upload_bytes += resources_->frame_width * resources_->frame_height * 4;
      resources_->mask_uploaded_bounds = {0, 0, static_cast<float>(resources_->frame_width),
                                          static_cast<float>(resources_->frame_height)};
    }
  }
  resources_->Transfer(resources_->group_textures[resources_->group_depth - 1],
                       resources_->group_textures[resources_->group_depth - 2], resources_->frame_width,
                       resources_->frame_height, 0, opacity, mask_bgra != nullptr, 0, 0, true);
  --resources_->group_depth;
}

bool BackdropGl::ApplyToFrame(const BackdropPlaneBatch& batch, const std::uint8_t* mask_bgra, std::size_t mask_stride,
                              bool mask_changed, std::optional<Rect> replacement_clip,
                              std::optional<Rect> visible_bounds) {
  if (resources_ == nullptr || resources_->group_depth == 0) {
    throw std::logic_error("HuxerUI backdrop plane execution requires an active frame");
  }
  if (batch.commands.empty()) {
    return true;
  }
  if (visible_bounds) {
    const Rect bounds = *visible_bounds;
    for (float value : {bounds.x, bounds.y, bounds.width, bounds.height, bounds.x + bounds.width,
                        bounds.y + bounds.height}) {
      if (!std::isfinite(value)) {
        throw std::logic_error("HuxerUI backdrop visible bounds must remain finite");
      }
    }
    if (bounds.x < 0 || bounds.y < 0 || bounds.width < 0 || bounds.height < 0 ||
        bounds.x + bounds.width > resources_->frame_width || bounds.y + bounds.height > resources_->frame_height) {
      throw std::logic_error("HuxerUI backdrop visible bounds must be inside the frame");
    }
  }
  if (replacement_clip) {
    const Rect clip = *replacement_clip;
    for (float value : {clip.x, clip.y, clip.width, clip.height, clip.x + clip.width, clip.y + clip.height}) {
      if (!std::isfinite(value) || std::floor(value) != value) {
        throw std::logic_error("HuxerUI backdrop replacement clip must have integral finite edges");
      }
    }
    if (clip.x < 0 || clip.y < 0 || clip.width < 0 || clip.height < 0 ||
        clip.x + clip.width > resources_->frame_width || clip.y + clip.height > resources_->frame_height) {
      throw std::logic_error("HuxerUI backdrop replacement clip must be inside the frame");
    }
  }
  Rect sampling_bounds;
  for (const BackdropPlaneCommand& member : batch.commands) {
    if (member.command == nullptr) {
      throw std::logic_error("HuxerUI backdrop plane contains an invalid command");
    }
    ValidateBackdropCommand(*member.command);
    const Rect bounds = TransformBounds(member.local_to_device, BackdropSamplingBounds(*member.command));
    Rect sampling{bounds.x - 3.0F, bounds.y - 3.0F, bounds.width + 6.0F, bounds.height + 6.0F};
    if (!std::isfinite(sampling.x) || !std::isfinite(sampling.y) || !std::isfinite(sampling.width) ||
        !std::isfinite(sampling.height) || !std::isfinite(sampling.x + sampling.width) ||
        !std::isfinite(sampling.y + sampling.height)) {
      throw std::invalid_argument("HuxerUI backdrop sampling bounds must remain finite");
    }
    if (visible_bounds) {
      const auto& transform = member.local_to_device;
      const auto& effect = member.command->effect;
      const float support = BackdropSamplingOutset(effect);
      const float halo_x = (std::abs(transform.m11) + std::abs(transform.m21)) * support + 3.0F;
      const float halo_y = (std::abs(transform.m12) + std::abs(transform.m22)) * support + 3.0F;
      if (visible_bounds->IsEmpty()) {
        continue;
      }
      // Clipped output still depends on the complete filter and reconstruction footprint beyond the clip.
      const Rect required{visible_bounds->x - halo_x, visible_bounds->y - halo_y,
                          visible_bounds->width + 2 * halo_x, visible_bounds->height + 2 * halo_y};
      if (std::isfinite(required.x) && std::isfinite(required.y) && std::isfinite(required.width) &&
          std::isfinite(required.height)) {
        sampling = sampling.Intersection(required);
      }
      if (sampling.IsEmpty()) {
        continue;
      }
    }
    if (sampling_bounds.IsEmpty()) {
      sampling_bounds = sampling;
    } else {
      const float left = std::min(sampling_bounds.x, sampling.x);
      const float top = std::min(sampling_bounds.y, sampling.y);
      const float right = std::max(sampling_bounds.x + sampling_bounds.width, sampling.x + sampling.width);
      const float bottom = std::max(sampling_bounds.y + sampling_bounds.height, sampling.y + sampling.height);
      sampling_bounds = {left, top, right - left, bottom - top};
    }
  }
  const auto clamp = [](float value, std::size_t extent, bool upper) {
    return static_cast<GLint>(
        std::clamp(upper ? std::ceil(value) : std::floor(value), 0.0F, static_cast<float>(extent)));
  };
  GLint left = clamp(sampling_bounds.x, resources_->frame_width, false);
  GLint top = clamp(sampling_bounds.y, resources_->frame_height, false);
  GLint right = clamp(sampling_bounds.x + sampling_bounds.width, resources_->frame_width, true);
  GLint bottom = clamp(sampling_bounds.y + sampling_bounds.height, resources_->frame_height, true);
  if (right <= left || bottom <= top) {
    return true;
  }
  const bool same_inputs = resources_->cache_eligible && resources_->cached_source_valid &&
                           resources_->cached_rasters == resources_->raster_snapshots &&
                           resources_->cached_identities == resources_->raster_identities;
  const Rect sampling_window{static_cast<float>(left), static_cast<float>(top), static_cast<float>(right - left),
                              static_cast<float>(bottom - top)};
  resources_->moving_source_window = same_inputs &&
      (resources_->moving_source_window || resources_->last_sampling_bounds != sampling_window);
  resources_->last_sampling_bounds = sampling_window;
  resources_->stable_source_frames = same_inputs ? std::min(resources_->stable_source_frames + 1, 3U) : 0;
  const auto& first = batch.commands.front();
  const bool one_kernel = first.command->effect.blur_radius > 0 &&
      std::all_of(batch.commands.begin(), batch.commands.end(), [&](const auto& member) {
        return member.command->effect.blur_radius == first.command->effect.blur_radius &&
               member.command->effect.blur_quality == first.command->effect.blur_quality &&
               member.local_to_device.m11 == first.local_to_device.m11 &&
               member.local_to_device.m12 == first.local_to_device.m12 &&
               member.local_to_device.m21 == first.local_to_device.m21 &&
               member.local_to_device.m22 == first.local_to_device.m22;
      });
  resources_->stable_full_frame = resources_->stable_source_frames >= 3 && resources_->moving_source_window &&
      one_kernel && resources_->frame_width * resources_->frame_height * 4 <= maximum_backdrop_cache_bytes / 2;
  if (resources_->stable_full_frame) {
    // A proven stable prefix amortizes one full kernel across moving glass without reusing stale source pixels.
    left = top = 0;
    right = static_cast<GLint>(resources_->frame_width);
    bottom = static_cast<GLint>(resources_->frame_height);
    ++resources_->frame_stats.stable_full_frame_planes;
  }
  if (resources_->cache_eligible) {
    // Stable capture tiles let small optical displacements reuse the same background support.
    left = left / backdrop_capture_tile * backdrop_capture_tile;
    top = top / backdrop_capture_tile * backdrop_capture_tile;
    right = std::min((right + backdrop_capture_tile - 1) / backdrop_capture_tile * backdrop_capture_tile,
                     static_cast<GLint>(resources_->frame_width));
    bottom = std::min((bottom + backdrop_capture_tile - 1) / backdrop_capture_tile * backdrop_capture_tile,
                      static_cast<GLint>(resources_->frame_height));
  }
  const std::size_t width = static_cast<std::size_t>(right - left);
  const std::size_t height = static_cast<std::size_t>(bottom - top);
  EnsureCaptureSize(width, height);
  const bool reuse_source = resources_->cache_eligible && resources_->cached_source_valid &&
                            resources_->cached_left == left && resources_->cached_top == top &&
                            resources_->cached_width == width && resources_->cached_height == height &&
                            resources_->cached_rasters == resources_->raster_snapshots &&
                            resources_->cached_identities == resources_->raster_identities;
  resources_->cache_execution = false;
  if (!reuse_source) {
    auto& reasons = resources_->frame_stats.source_miss_reasons;
    if (!resources_->cache_eligible) {
      reasons |= static_cast<std::uint64_t>(BackdropCacheMiss::Ineligible);
    } else if (resources_->cached_source_valid) {
      if (resources_->cached_left != left || resources_->cached_top != top ||
          resources_->cached_width != width || resources_->cached_height != height) {
        reasons |= static_cast<std::uint64_t>(BackdropCacheMiss::CaptureGeometry);
      }
      if (resources_->cached_rasters != resources_->raster_snapshots ||
          resources_->cached_identities != resources_->raster_identities) {
        reasons |= static_cast<std::uint64_t>(BackdropCacheMiss::SourceInputs);
      }
    } else if (reasons == 0) {
      reasons |= static_cast<std::uint64_t>(BackdropCacheMiss::MissingSource);
    }
    BackdropCpuTimer timer(resources_->frame_stats.source_prepare_cpu_ns);
    if (resources_->cache_eligible) {
      if (resources_->cached_width != width || resources_->cached_height != height) {
        resources_->ClearCache();
      } else {
        resources_->InvalidateSource(false);
      }
      if (width * height * 4 <= maximum_backdrop_cache_bytes / 2) {
        if (resources_->cached_source == 0) {
          resources_->cached_source = CreateTexture(width, height);
          resources_->cache_bytes = width * height * 4;
          ++resources_->frame_stats.cache_texture_allocations;
        } else {
          ++resources_->frame_stats.cache_texture_reuses;
        }
        resources_->cached_left = left;
        resources_->cached_top = top;
        resources_->cached_width = width;
        resources_->cached_height = height;
        resources_->cached_rasters = resources_->raster_snapshots;
        resources_->cached_identities = resources_->raster_identities;
        resources_->cache_execution = true;
      } else {
        reasons |= static_cast<std::uint64_t>(BackdropCacheMiss::Budget);
      }
    }
    const GLuint destination = resources_->cache_execution ? resources_->cached_source : resources_->source_texture;
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, resources_->read_framebuffer);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           resources_->group_textures[resources_->group_depth - 1], 0);
    RequireFramebuffer(GL_READ_FRAMEBUFFER);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resources_->draw_framebuffer);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, destination, 0);
    RequireFramebuffer(GL_DRAW_FRAMEBUFFER);
    glBlitFramebuffer(left, top, right, bottom, 0, 0, right - left, bottom - top, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    RequireGlSuccess();
    if (resources_->cache_execution) {
      resources_->cached_source_valid = true;
    }
    ++resources_->frame_stats.source_captures;
  } else {
    resources_->cache_execution = true;
  }
  if (!ExecutePlane(batch, nullptr, width, height, static_cast<float>(left), static_cast<float>(top))) {
    return false;
  }

  GLuint output = resources_->output_texture;
  if (mask_bgra != nullptr) {
    const Rect uploaded = resources_->mask_uploaded_bounds;
    if (mask_changed || uploaded.IsEmpty() || left < uploaded.x || top < uploaded.y ||
        right > uploaded.x + uploaded.width || bottom > uploaded.y + uploaded.height) {
      BackdropCpuTimer timer(resources_->frame_stats.mask_upload_cpu_ns);
      if (mask_stride < resources_->frame_width * 4 || mask_stride % 4 != 0) {
        throw std::logic_error("HuxerUI backdrop mask rows are invalid");
      }
      glActiveTexture(GL_TEXTURE0);
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
      glBindTexture(GL_TEXTURE_2D, resources_->mask_texture);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(mask_stride / 4));
      const auto upload = [&](GLint x, GLint y, GLint w, GLint h) {
        if (w <= 0 || h <= 0) {
          return;
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE,
                        mask_bgra + static_cast<std::size_t>(y) * mask_stride + static_cast<std::size_t>(x) * 4);
        resources_->frame_stats.mask_upload_bytes += static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4;
      };
      Rect resident{static_cast<float>(left), static_cast<float>(top), static_cast<float>(right - left),
                    static_cast<float>(bottom - top)};
      if (mask_changed || uploaded.IsEmpty()) {
        upload(left, top, right - left, bottom - top);
      } else {
        const auto old_left = static_cast<GLint>(uploaded.x);
        const auto old_top = static_cast<GLint>(uploaded.y);
        const auto old_right = static_cast<GLint>(uploaded.x + uploaded.width);
        const auto old_bottom = static_cast<GLint>(uploaded.y + uploaded.height);
        const GLint union_left = std::min(left, old_left);
        const GLint union_top = std::min(top, old_top);
        const GLint union_right = std::max(right, old_right);
        const GLint union_bottom = std::max(bottom, old_bottom);
        // Grow the known rectangle through disjoint strips; overlapping planes never upload its pixels twice.
        upload(union_left, union_top, union_right - union_left, old_top - union_top);
        upload(union_left, old_bottom, union_right - union_left, union_bottom - old_bottom);
        upload(union_left, old_top, old_left - union_left, old_bottom - old_top);
        upload(old_right, old_top, union_right - old_right, old_bottom - old_top);
        resident = {static_cast<float>(union_left), static_cast<float>(union_top),
                    static_cast<float>(union_right - union_left), static_cast<float>(union_bottom - union_top)};
      }
      glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
      RequireGlSuccess();
      resources_->mask_uploaded_bounds = resident;
    }
    if (!replacement_clip || !replacement_clip->IsEmpty()) {
      // Both sampled textures are independent captures; replacing the active target never samples its attachment.
      resources_->Transfer(output, resources_->group_textures[resources_->group_depth - 1], width, height, 1, 1.0F,
                           true, left, top, false,
                           resources_->cache_execution ? resources_->cached_source : resources_->source_texture,
                           replacement_clip.value_or(Rect{}),
                           {static_cast<float>(left), static_cast<float>(top)});
    }
  } else {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, resources_->read_framebuffer);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output, 0);
    RequireFramebuffer(GL_READ_FRAMEBUFFER);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resources_->draw_framebuffer);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           resources_->group_textures[resources_->group_depth - 1], 0);
    RequireFramebuffer(GL_DRAW_FRAMEBUFFER);
    glDisable(GL_SCISSOR_TEST);
    // Only replacement is clipped: pixels outside the output still supply the complete filter halo.
    if (replacement_clip) {
      glEnable(GL_SCISSOR_TEST);
      glScissor(static_cast<GLint>(replacement_clip->x), static_cast<GLint>(replacement_clip->y),
                static_cast<GLsizei>(replacement_clip->width), static_cast<GLsizei>(replacement_clip->height));
    }
    glBlitFramebuffer(0, 0, right - left, bottom - top, left, top, right, bottom, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glDisable(GL_SCISSOR_TEST);
    RequireGlSuccess();
  }
  resources_->cache_eligible = false;
  resources_->cache_execution = false;
  resources_->raster_snapshots.clear();
  resources_->raster_snapshot_bytes = 0;
  return true;
}

void BackdropGl::ExportToTexture(unsigned int texture) {
  if (resources_ == nullptr || resources_->group_depth != 1) {
    throw std::logic_error("HuxerUI backdrop publication requires one balanced root group");
  }
  resources_->Transfer(resources_->group_textures.front(), texture, resources_->frame_width, resources_->frame_height,
                       2, 1.0F, false);
}

std::size_t BackdropGl::Width() const noexcept {
  return resources_->frame_width;
}
std::size_t BackdropGl::Height() const noexcept {
  return resources_->frame_height;
}

void BackdropGl::CancelFrame() noexcept {
  if (resources_ != nullptr) {
    resources_->group_depth = 0;
    resources_->cache_eligible = false;
    resources_->cache_execution = false;
    resources_->stable_full_frame = false;
    resources_->raster_snapshots.clear();
    resources_->raster_identities.clear();
    resources_->raster_snapshot_bytes = 0;
  }
}

BackdropGlStats BackdropGl::FrameStats() const noexcept {
  return resources_ != nullptr ? resources_->frame_stats : BackdropGlStats{};
}

} // namespace huxerui::detail
