#include "linux_internal.h"

#include <catch2/catch_amalgamated.hpp>
#include <epoxy/gl.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <huxerui/effects.h>
#include <huxerui/geometry.h>
#include <huxerui/paint.h>

#include "graphics/backdrop_plane_internal.h"
#include "graphics/backdrop_gl_internal.h"
#include "graphics/effects_internal.h"
#include "graphics/geometry_internal.h"
#include "linux_gl_effects_internal.h"

namespace huxerui::test {
namespace {

class GlEnvironment final {
public:
  explicit GlEnvironment(GdkGLAPI allowed_apis = static_cast<GdkGLAPI>(GDK_GL_API_GL | GDK_GL_API_GLES)) {
    display_ = gdk_display_get_default();
    if (display_ == nullptr) {
      SKIP("HuxerUI Linux backdrop GPU tests require a GTK display");
    }
    const std::array<std::pair<GdkGLAPI, int>, 2> candidates{{{GDK_GL_API_GL, 3}, {GDK_GL_API_GLES, 0}}};
    std::string reason;
    for (const auto& [api, minor] : candidates) {
      if ((allowed_apis & api) == 0) {
        continue;
      }
      GError* error = nullptr;
      GdkGLContext* candidate = gdk_display_create_gl_context(display_, &error);
      if (candidate != nullptr) {
        gdk_gl_context_set_allowed_apis(candidate, api);
        gdk_gl_context_set_required_version(candidate, 3, minor);
      }
      const bool realized = candidate != nullptr && gdk_gl_context_realize(candidate, &error) != FALSE;
      if (error != nullptr) {
        reason = error->message;
        g_error_free(error);
      }
      if (realized) {
        context_ = candidate;
        break;
      }
      if (candidate != nullptr) {
        g_object_unref(candidate);
      }
    }
    if (context_ == nullptr) {
      SKIP("HuxerUI Linux backdrop GPU tests require OpenGL 3.3 or OpenGL ES 3.0: " << reason);
    }
    previous_ = gdk_gl_context_get_current();
    if (previous_ != nullptr) {
      g_object_ref(previous_);
    }
    gdk_gl_context_make_current(context_);
  }

  ~GlEnvironment() {
    if (previous_ != nullptr) {
      gdk_gl_context_make_current(previous_);
      g_object_unref(previous_);
    } else {
      gdk_gl_context_clear_current();
    }
    g_object_unref(context_);
  }

  GlEnvironment(const GlEnvironment&) = delete;
  GlEnvironment& operator=(const GlEnvironment&) = delete;

  GdkDisplay* RequireDisplay() const {
    REQUIRE(gdk_gl_context_get_current() == context_);
    const auto* renderer = glGetString(GL_RENDERER);
    const auto* version = glGetString(GL_VERSION);
    REQUIRE(renderer != nullptr);
    REQUIRE(version != nullptr);
    WARN("HuxerUI Linux backdrop GPU renderer=" << reinterpret_cast<const char*>(renderer)
                                               << " version=" << reinterpret_cast<const char*>(version));
    return display_;
  }

private:
  GdkDisplay* display_ = nullptr;
  GdkGLContext* context_ = nullptr;
  GdkGLContext* previous_ = nullptr;
};

std::vector<std::uint8_t> PatternPixels(std::size_t width, std::size_t height, bool translucent = false) {
  std::vector<std::uint8_t> pixels(width * height * 4);
  for (std::size_t y = 0; y < height; ++y) {
    for (std::size_t x = 0; x < width; ++x) {
      const std::size_t offset = (y * width + x) * 4;
      const std::uint32_t alpha = translucent ? 96 + (x * 3 + y * 5) % 128 : 255;
      const std::uint32_t checker = (x / 4 + y / 5) % 2 == 0 ? 40 : 180;
      pixels[offset] = static_cast<std::uint8_t>(checker * alpha / 255);
      pixels[offset + 1] = static_cast<std::uint8_t>(((x * 9 + y * 3) % 256) * alpha / 255);
      pixels[offset + 2] = static_cast<std::uint8_t>(((x * 7 + y * 11) % 256) * alpha / 255);
      pixels[offset + 3] = static_cast<std::uint8_t>(alpha);
    }
  }
  return pixels;
}

DrawBackdropCommand GlassRect(Rect rect, float blur) {
  return {{{rect, CornerRadii{6.0F}}}, BackdropEffect{.blur_radius = blur}};
}

std::vector<std::uint8_t> ReferencePixels(const detail::BackdropPlaneBatch& batch,
                                        const std::vector<std::uint8_t>& input, std::size_t width,
                                        std::size_t height, Point origin) {
  std::vector<std::uint8_t> output = input;
  for (const detail::BackdropPlaneCommand& member : batch.commands) {
    const auto inverse = detail::InverseTransform(member.local_to_device);
    REQUIRE(inverse.has_value());
    const Transform2D pixel_to_local = detail::ComposeTransform(*inverse, detail::TranslationTransform(origin));
    output = detail::ApplyBackdropEffect(output, width, height, width * 4, pixel_to_local, *member.command).bgra;
  }
  return output;
}

void RequireReferenceMatch(const detail::BackdropPlaneBatch& batch, const std::vector<std::uint8_t>& input,
                           const std::vector<std::uint8_t>& actual, std::size_t width, std::size_t height,
                           Point origin = {}) {
  const auto expected = ReferencePixels(batch, input, width, height, origin);
  REQUIRE(actual.size() == expected.size());
  int maximum_error = 0;
  std::size_t maximum_index = 0;
  std::uint64_t total_error = 0;
  for (std::size_t index = 0; index < actual.size(); ++index) {
    const int error = std::abs(static_cast<int>(actual[index]) - static_cast<int>(expected[index]));
    if (error > maximum_error) {
      maximum_error = error;
      maximum_index = index;
    }
    total_error += static_cast<std::uint64_t>(error);
  }
  const double mean_error = static_cast<double>(total_error) / static_cast<double>(actual.size());
  CAPTURE(maximum_error, mean_error, maximum_index / 4 % width, maximum_index / 4 / width,
          maximum_index % 4, int(expected[maximum_index]), int(actual[maximum_index]));
  REQUIRE(maximum_error <= 3);
  REQUIRE(mean_error <= 0.5);

  const Transform2D capture_offset = detail::TranslationTransform({-origin.x, -origin.y});
  for (std::size_t y = 0; y < height; ++y) {
    for (std::size_t x = 0; x < width; ++x) {
      const Point pixel{static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F};
      bool inside = false;
      for (const detail::BackdropPlaneCommand& member : batch.commands) {
        const Transform2D transform = detail::ComposeTransform(capture_offset, member.local_to_device);
        const Rect bounds = detail::TransformBounds(transform, detail::BackdropBounds(*member.command));
        inside = inside || Rect{bounds.x - 1.0F, bounds.y - 1.0F, bounds.width + 2.0F,
                                bounds.height + 2.0F}.Contains(pixel);
      }
      if (!inside) {
        const std::size_t offset = (y * width + x) * 4;
        REQUIRE(std::equal(input.begin() + static_cast<std::ptrdiff_t>(offset),
                           input.begin() + static_cast<std::ptrdiff_t>(offset + 4),
                           actual.begin() + static_cast<std::ptrdiff_t>(offset)));
      }
    }
  }
}

} // namespace

TEST_CASE("LinuxBackdropGlPreservesIdentityAndUniformCapturesExactly", "[linux][backdrop][gpu]") {
  GlEnvironment environment;
  detail::LinuxBackdropGl gpu;
  REQUIRE(gpu.TryInitialize(environment.RequireDisplay()));
  constexpr std::size_t width = 48;
  constexpr std::size_t height = 40;
  DrawBackdropCommand command = GlassRect({6.0F, 5.0F, 36.0F, 30.0F}, 0.0F);
  const detail::BackdropPlaneBatch batch{.commands = {{&command, {}}}, .sampling_bounds = {}, .output_bounds = {}};
  for (const bool translucent : {false, true}) {
    const auto input = PatternPixels(width, height, translucent);
    auto actual = input;
    REQUIRE(gpu.Apply(batch, actual.data(), width, height, 0.0F, 0.0F));
    REQUIRE(actual == input);
  }
  for (const std::array<std::uint8_t, 4> color :
       {std::array<std::uint8_t, 4>{28, 79, 116, 128}, std::array<std::uint8_t, 4>{0, 0, 0, 0}}) {
    std::vector<std::uint8_t> input(width * height * 4);
    for (std::size_t offset = 0; offset < input.size(); offset += 4) {
      std::copy(color.begin(), color.end(), input.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    command.effect.blur_radius = 9.0F;
    auto actual = input;
    REQUIRE(gpu.Apply(batch, actual.data(), width, height, 0.0F, 0.0F));
    REQUIRE(actual == input);
  }
}

TEST_CASE("LinuxBackdropGlMatchesCpuBlurAtSupportedDeviceTransforms", "[linux][backdrop][gpu]") {
  GlEnvironment environment;
  detail::LinuxBackdropGl gpu;
  REQUIRE(gpu.TryInitialize(environment.RequireDisplay()));
  constexpr std::size_t width = 128;
  constexpr std::size_t height = 112;
  const Point origin{13.0F, 9.0F};
  const DrawBackdropCommand command = GlassRect({0.0F, 0.0F, 30.0F, 24.0F}, 9.0F);
  struct Case {
    const char* name;
    Transform2D transform;
  };
  constexpr float diagonal = 0.70710678F;
  const std::array<Case, 6> cases{{
      {"scale 1", {1.0F, 0.0F, 0.0F, 1.0F, 37.0F, 28.0F}},
      {"scale 2", {2.0F, 0.0F, 0.0F, 2.0F, 37.0F, 28.0F}},
      {"axis-aligned nonuniform", {2.0F, 0.0F, 0.0F, 1.0F, 37.0F, 28.0F}},
      {"quarter-turn nonuniform", {0.0F, 2.0F, -1.0F, 0.0F, 86.0F, 22.0F}},
      {"uniform rotation", {diagonal, diagonal, -diagonal, diagonal, 64.0F, 30.0F}},
      {"uniform reflection", {-2.0F, 0.0F, 0.0F, 2.0F, 98.0F, 28.0F}},
  }};
  const auto input = PatternPixels(width, height);
  for (const Case& test_case : cases) {
    CAPTURE(test_case.name);
    const detail::BackdropPlaneBatch batch{
        .commands = {{&command, test_case.transform}}, .sampling_bounds = {}, .output_bounds = {}};
    auto actual = input;
    REQUIRE(gpu.Apply(batch, actual.data(), width, height, origin.x, origin.y));
    RequireReferenceMatch(batch, input, actual, width, height, origin);
  }

  std::vector<std::uint8_t> impulse(width * height * 4, 0);
  for (std::size_t offset = 3; offset < impulse.size(); offset += 4) {
    impulse[offset] = 255;
  }
  impulse[(36 * width + 38) * 4 + 2] = 255;
  const detail::BackdropPlaneBatch batch{
      .commands = {{&command, cases[1].transform}}, .sampling_bounds = {}, .output_bounds = {}};
  auto actual = impulse;
  REQUIRE(gpu.Apply(batch, actual.data(), width, height, origin.x, origin.y));
  RequireReferenceMatch(batch, impulse, actual, width, height, origin);
}

TEST_CASE("LinuxBackdropGlKeepsMixedAndRepeatedBlurKeysIndependent", "[linux][backdrop][gpu]") {
  GlEnvironment environment;
  detail::LinuxBackdropGl gpu;
  REQUIRE(gpu.TryInitialize(environment.RequireDisplay()));
  constexpr std::size_t width = 248;
  constexpr std::size_t height = 80;
  const std::array<DrawBackdropCommand, 4> commands{{
      GlassRect({12.0F, 20.0F, 32.0F, 28.0F}, 3.0F),
      GlassRect({72.0F, 20.0F, 32.0F, 28.0F}, 9.0F),
      GlassRect({132.0F, 20.0F, 32.0F, 28.0F}, 0.0F),
      GlassRect({192.0F, 20.0F, 32.0F, 28.0F}, 3.0F),
  }};
  detail::BackdropPlaneCollector collector;
  for (const DrawBackdropCommand& command : commands) {
    REQUIRE(collector.TryAdd(command, {}, 3.0F));
  }
  REQUIRE(collector.batch().commands.size() == commands.size());
  const auto input = PatternPixels(width, height);
  auto actual = input;
  REQUIRE(gpu.Apply(collector.batch(), actual.data(), width, height, 0.0F, 0.0F));
  RequireReferenceMatch(collector.batch(), input, actual, width, height);
}

TEST_CASE("LinuxBackdropGlMatchesTranslucentRefractionHighlightAndFusion", "[linux][backdrop][gpu]") {
  GlEnvironment environment;
  detail::LinuxBackdropGl gpu;
  REQUIRE(gpu.TryInitialize(environment.RequireDisplay()));
  constexpr std::size_t width = 80;
  constexpr std::size_t height = 64;
  const DrawBackdropCommand command{
      {{Rect{12.0F, 12.0F, 20.0F, 24.0F}, CornerRadii{7.0F, 12.0F, 5.0F, 10.0F}},
       {Rect{36.0F, 15.0F, 22.0F, 20.0F}, CornerRadii{8.0F}}},
      BackdropEffect{.blur_radius = 6.0F, .refraction = 2.0F, .edge_width = 8.0F, .highlight = 0.3F,
                     .saturation = 1.1F, .brightness = 0.95F, .fusion_radius = 8.0F},
  };
  const detail::BackdropPlaneBatch batch{.commands = {{&command, {}}}, .sampling_bounds = {}, .output_bounds = {}};
  const auto input = PatternPixels(width, height, true);
  auto actual = input;
  REQUIRE(gpu.Apply(batch, actual.data(), width, height, 0.0F, 0.0F));
  RequireReferenceMatch(batch, input, actual, width, height);
  for (std::size_t offset = 0; offset < actual.size(); offset += 4) {
    REQUIRE(actual[offset] <= actual[offset + 3]);
    REQUIRE(actual[offset + 1] <= actual[offset + 3]);
    REQUIRE(actual[offset + 2] <= actual[offset + 3]);
  }
}

TEST_CASE("LinuxBackdropGlReportsUnavailableAndUnsupportedExecutionWithoutChangingPixels",
          "[linux][backdrop][gpu]") {
  constexpr std::size_t width = 64;
  constexpr std::size_t height = 56;
  DrawBackdropCommand command = GlassRect({0.0F, 0.0F, 26.0F, 24.0F}, 6.0F);
  const Transform2D shear{1.0F, 0.0F, 0.35F, 1.0F, 16.0F, 12.0F};
  const detail::BackdropPlaneBatch batch{.commands = {{&command, shear}}, .sampling_bounds = {}, .output_bounds = {}};
  const auto input = PatternPixels(width, height, true);
  auto actual = input;
  detail::LinuxBackdropGl gpu;
  REQUIRE_FALSE(gpu.TryInitialize(nullptr));
  REQUIRE_FALSE(gpu.Apply(batch, actual.data(), width, height, 0.0F, 0.0F));
  REQUIRE(actual == input);

  GlEnvironment environment;
  REQUIRE(gpu.TryInitialize(environment.RequireDisplay()));
  REQUIRE_FALSE(gpu.Apply(batch, actual.data(), width, height, 0.0F, 0.0F));
  REQUIRE(actual == input);
  REQUIRE_THROWS_AS(ReferencePixels(batch, input, width, height, {}), std::logic_error);

  command.effect.blur_radius = 0.0F;
  command.effect.refraction = 2.0F;
  REQUIRE(gpu.Apply(batch, actual.data(), width, height, 0.0F, 0.0F));
  RequireReferenceMatch(batch, input, actual, width, height);
}

TEST_CASE("LinuxBackdropGlPublishesImmutableRowsAndChannelsBeyondRendererLifetime", "[linux][backdrop][gpu]") {
  GlEnvironment environment;
  constexpr std::size_t width = 48;
  constexpr std::size_t height = 40;
  const auto input = PatternPixels(width, height, true);
  const auto replacement = PatternPixels(width, height);
  const DrawBackdropCommand command = GlassRect({6.0F, 5.0F, 36.0F, 30.0F}, 0.0F);
  const detail::BackdropPlaneBatch batch{.commands = {{&command, {}}}, .sampling_bounds = {}, .output_bounds = {}};
  const auto release = [](::GdkTexture* texture) { g_object_unref(texture); };
  std::unique_ptr<::GdkTexture, decltype(release)> first(nullptr, release);
  std::unique_ptr<::GdkTexture, decltype(release)> second(nullptr, release);
  {
    detail::LinuxBackdropGl gpu;
    REQUIRE(gpu.TryInitialize(environment.RequireDisplay()));
    REQUIRE(gpu.BeginFrame(width, height));
    gpu.CompositeRaster(input.data(), width * 4);
    REQUIRE_FALSE(gpu.BeginFrame(detail::maximum_backdrop_capture_pixels + 1, 1));
    REQUIRE(gpu.ApplyToFrame(batch, nullptr, 0));
    first.reset(gpu.FinishFrame());
    REQUIRE(first != nullptr);
    REQUIRE(gpu.BeginFrame(width, height));
    gpu.CompositeRaster(replacement.data(), width * 4);
    second.reset(gpu.FinishFrame());
    REQUIRE(second != nullptr);
    gpu.CancelFrame();
  }
  const auto download = [](GdkTexture* texture) {
    std::vector<std::uint8_t> pixels(width * height * 4);
    GdkTextureDownloader* downloader = gdk_texture_downloader_new(texture);
    gdk_texture_downloader_set_format(downloader, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED);
    gdk_texture_downloader_download_into(downloader, pixels.data(), width * 4);
    gdk_texture_downloader_free(downloader);
    return pixels;
  };
  REQUIRE(download(first.get()) == input);
  REQUIRE(download(second.get()) == replacement);
}

TEST_CASE("LinuxBackdropGlReusesOnlyAnUnchangedFirstRootPlaneSource", "[linux][backdrop][gpu]") {
  const auto api = GENERATE(GDK_GL_API_GL, GDK_GL_API_GLES);
  GlEnvironment environment(api);
  detail::LinuxBackdropGl gpu;
  REQUIRE(gpu.TryInitialize(environment.RequireDisplay(), api));
  constexpr std::size_t width = 128;
  constexpr std::size_t height = 96;
  auto input = PatternPixels(width, height, true);
  DrawBackdropCommand command = GlassRect({24.0F, 24.0F, 32.0F, 32.0F}, 6.0F);
  command.effect.refraction = 2.0F;
  const detail::BackdropPlaneBatch batch{.commands = {{&command, {}}}, .sampling_bounds = {}, .output_bounds = {}};
  const auto render = [&](bool isolated = false) {
    REQUIRE(gpu.BeginFrame(width, height));
    if (isolated) {
      gpu.PushGroup();
    }
    gpu.CompositeRaster(input.data(), width * 4);
    REQUIRE(gpu.ApplyToFrame(batch, nullptr, 0));
    if (isolated) {
      gpu.PopGroup(nullptr, 0, 1.0F);
    }
    const auto release = [](::GdkTexture* value) { g_object_unref(value); };
    const std::unique_ptr<::GdkTexture, decltype(release)> texture(gpu.FinishFrame(), release);
    REQUIRE(texture != nullptr);
    std::vector<std::uint8_t> pixels(input.size());
    GdkTextureDownloader* downloader = gdk_texture_downloader_new(texture.get());
    gdk_texture_downloader_set_format(downloader, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED);
    gdk_texture_downloader_download_into(downloader, pixels.data(), width * 4);
    gdk_texture_downloader_free(downloader);
    RequireReferenceMatch(batch, input, pixels, width, height);
    REQUIRE(gpu.FrameStats().readback_bytes == 0);
  };

  render();
  REQUIRE(gpu.FrameStats().source_captures == 1);
  REQUIRE(gpu.FrameStats().blur_passes == 6);
  REQUIRE(gpu.FrameStats().blur_cache_hits == 0);
  REQUIRE(gpu.FrameStats().source_miss_reasons != 0);
  REQUIRE(gpu.FrameStats().kernel_source_copy_pixels == 0);
  REQUIRE(gpu.FrameStats().kernel_source_reuses == 1);
  REQUIRE(gpu.FrameStats().blur_result_adoptions == 1);
  command.effect.highlight = 0.3F;
  command.effect.refraction = 3.0F;
  command.effect.brightness = 0.95F;
  render();
  REQUIRE(gpu.FrameStats().source_captures == 0);
  REQUIRE(gpu.FrameStats().blur_passes == 0);
  REQUIRE(gpu.FrameStats().blur_cache_hits == 1);
  REQUIRE(gpu.FrameStats().source_miss_reasons == 0);
  REQUIRE(gpu.FrameStats().blur_miss_reasons == 0);

  command.effect.blur_radius = 9.0F;
  render();
  REQUIRE(gpu.FrameStats().source_captures == 0);
  REQUIRE(gpu.FrameStats().blur_passes == 6);
  REQUIRE(gpu.FrameStats().blur_cache_hits == 0);
  REQUIRE(gpu.FrameStats().blur_miss_reasons ==
          static_cast<std::uint64_t>(detail::BackdropCacheMiss::KernelRadius));
  render();
  REQUIRE(gpu.FrameStats().blur_passes == 0);
  REQUIRE(gpu.FrameStats().blur_cache_hits == 1);

  command.effect.blur_radius = 6.0F;
  render();
  REQUIRE(gpu.FrameStats().source_captures == 0);
  REQUIRE(gpu.FrameStats().blur_cache_hits == 1);
  REQUIRE(gpu.FrameStats().blur_result_adoptions == 0);
  command.effect.blur_radius = 9.0F;
  render();
  REQUIRE(gpu.FrameStats().blur_cache_hits == 1);

  input = PatternPixels(width, height);
  render();
  REQUIRE(gpu.FrameStats().source_captures == 1);
  REQUIRE(gpu.FrameStats().blur_passes == 6);
  REQUIRE(gpu.FrameStats().cache_texture_allocations == 0);
  REQUIRE(gpu.FrameStats().cache_texture_reuses == 2);
  for (int frame = 0; frame < 6; ++frame) {
    for (std::size_t offset = 0; offset < input.size(); offset += 4) {
      input[offset] = static_cast<std::uint8_t>((frame * 41) % (input[offset + 3] + 1U));
    }
    command.effect.blur_radius = frame % 2 == 0 ? 6.0F : 9.0F;
    render();
    REQUIRE(gpu.FrameStats().source_captures == 1);
    REQUIRE(gpu.FrameStats().blur_passes == 6);
    REQUIRE(gpu.FrameStats().blur_cache_hits == 0);
    REQUIRE(gpu.FrameStats().cache_texture_allocations == 0);
    REQUIRE(gpu.FrameStats().cache_texture_reuses == 2);
    render();
    REQUIRE(gpu.FrameStats().source_captures == 0);
    REQUIRE(gpu.FrameStats().blur_cache_hits == 1);
  }
  command.shapes.front().rect.x = 88.0F;
  render();
  REQUIRE(gpu.FrameStats().source_captures == 1);
  REQUIRE(gpu.FrameStats().blur_passes == 6);
  render(true);
  REQUIRE(gpu.FrameStats().source_captures == 1);
  REQUIRE(gpu.FrameStats().blur_passes == 6);
  REQUIRE(gpu.FrameStats().blur_cache_hits == 0);
}

TEST_CASE("LinuxBackdropGlAdoptsIndependentCroppedKernelsWithoutChangingPixels", "[linux][backdrop][gpu]") {
  const auto api = GENERATE(GDK_GL_API_GL, GDK_GL_API_GLES);
  const bool large = GENERATE(false, true);
  GlEnvironment environment(api);
  detail::LinuxBackdropGl gpu;
  REQUIRE(gpu.TryInitialize(environment.RequireDisplay(), api));
  const std::size_t width = large ? 2400 : 256, height = large ? 512 : 160;
  auto source = PatternPixels(width, height, true);
  std::array<DrawBackdropCommand, 2> commands{{
      large ? GlassRect({0, 0, 1050, 512}, 36) : GlassRect({32, 40, 80, 50}, 6),
      large ? GlassRect({1250, 0, 1100, 512}, 60) : GlassRect({180, 80, 72, 58}, 15),
  }};
  const detail::BackdropPlaneBatch batch{.commands = {{&commands[0], {}}, {&commands[1], {}}},
                                        .sampling_bounds = {}, .output_bounds = {}};
  const auto render = [&] {
    REQUIRE(gpu.BeginFrame(width, height));
    gpu.CompositeRaster(source.data(), width * 4);
    REQUIRE(gpu.ApplyToFrame(batch, nullptr, 0));
    const auto release = [](::GdkTexture* value) { g_object_unref(value); };
    const std::unique_ptr<::GdkTexture, decltype(release)> texture(gpu.FinishFrame(), release);
    REQUIRE(texture != nullptr);
    std::vector<std::uint8_t> pixels(source.size());
    GdkTextureDownloader* downloader = gdk_texture_downloader_new(texture.get());
    gdk_texture_downloader_set_format(downloader, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED);
    gdk_texture_downloader_download_into(downloader, pixels.data(), width * 4);
    gdk_texture_downloader_free(downloader);
    RequireReferenceMatch(batch, source, pixels, width, height);
    REQUIRE(gpu.FrameStats().readback_bytes == 0);
  };
  render();
  REQUIRE(gpu.FrameStats().blur_passes == 12);
  REQUIRE(gpu.FrameStats().kernel_source_copy_pixels > 0);
  REQUIRE(gpu.FrameStats().blur_result_adoptions == 2);
  render();
  REQUIRE(gpu.FrameStats().blur_cache_hits == 2);
  REQUIRE(gpu.FrameStats().blur_passes == 0);
  commands[0].effect.blur_radius = large ? 42 : 9;
  render();
  REQUIRE(gpu.FrameStats().blur_cache_hits == 1);
  REQUIRE(gpu.FrameStats().blur_result_adoptions == 1);
  commands[0].effect.blur_radius = large ? 36 : 6;
  render();
  REQUIRE(gpu.FrameStats().blur_cache_hits == 2);
  for (int frame = 0; frame < 4; ++frame) {
    for (std::size_t offset = 0; offset < source.size(); offset += 4) {
      source[offset] = static_cast<std::uint8_t>((frame * 41 + 7) % (source[offset + 3] + 1U));
    }
    render();
    REQUIRE(gpu.FrameStats().source_captures == 1);
    REQUIRE(gpu.FrameStats().blur_passes == 12);
    REQUIRE(gpu.FrameStats().blur_result_adoptions == 2);
  }
}

TEST_CASE("LinuxBackdropGlKeepsAsymmetricSamplingHalosAcrossBlurGroups", "[linux][backdrop][gpu]") {
  GlEnvironment environment;
  detail::LinuxBackdropGl gpu;
  REQUIRE(gpu.TryInitialize(environment.RequireDisplay()));
  constexpr std::size_t width = 184;
  constexpr std::size_t height = 80;
  const std::array<DrawBackdropCommand, 3> commands{{
      GlassRect({8.0F, 20.0F, 20.0F, 30.0F}, 0.0F),
      GlassRect({100.0F, 20.0F, 20.0F, 30.0F}, 20.0F),
      GlassRect({131.0F, 20.0F, 20.0F, 30.0F}, 0.0F),
  }};
  detail::BackdropPlaneCollector collector;
  for (const DrawBackdropCommand& command : commands) {
    REQUIRE(collector.TryAdd(command, {}, 3.0F));
  }
  const auto input = PatternPixels(width, height);
  auto actual = input;
  REQUIRE(gpu.Apply(collector.batch(), actual.data(), width, height, 0.0F, 0.0F));
  RequireReferenceMatch(collector.batch(), input, actual, width, height);
}

TEST_CASE("LinuxBackdropGlExecutesTheGlesShaderFlavor", "[linux][backdrop][gpu]") {
  GlEnvironment environment(GDK_GL_API_GLES);
  detail::LinuxBackdropGl gpu;
  REQUIRE(gpu.TryInitialize(environment.RequireDisplay(), GDK_GL_API_GLES));
  constexpr std::size_t width = 80;
  constexpr std::size_t height = 64;
  DrawBackdropCommand command = GlassRect({12.0F, 12.0F, 48.0F, 36.0F}, 6.0F);
  const detail::BackdropPlaneBatch batch{.commands = {{&command, {}}}, .sampling_bounds = {}, .output_bounds = {}};
  const auto input = PatternPixels(width, height, true);
  auto actual = input;
  REQUIRE(gpu.Apply(batch, actual.data(), width, height, 0.0F, 0.0F));
  RequireReferenceMatch(batch, input, actual, width, height);
  command.effect.blur_radius = 0.0F;
  REQUIRE(gpu.Apply(batch, actual.data(), width, height, 0.0F, 0.0F));
  actual = input;
  REQUIRE(gpu.Apply(batch, actual.data(), width, height, 0.0F, 0.0F));
  REQUIRE(actual == input);
}

TEST_CASE("LinuxBackdropGlPreservesLargeKernelPixels", "[linux][backdrop][gpu]") {
  GlEnvironment environment;
  detail::LinuxBackdropGl gpu;
  REQUIRE(gpu.TryInitialize(environment.RequireDisplay()));
  constexpr std::size_t width = 1024;
  constexpr std::size_t height = 512;
  auto command = GlassRect({20.0F, 20.0F, 950.0F, 470.0F}, 36.0F);
  command.effect.refraction = 4.0F;
  command.effect.edge_width = 16.0F;
  const detail::BackdropPlaneBatch batch{.commands = {{&command, {}}}, .sampling_bounds = {}, .output_bounds = {}};
  const auto input = PatternPixels(width, height, true);
  auto actual = input;
  REQUIRE(gpu.Apply(batch, actual.data(), width, height, 0.0F, 0.0F));
  RequireReferenceMatch(batch, input, actual, width, height);
  REQUIRE(gpu.FrameStats().blur_passes == 6);
  REQUIRE((gpu.FrameStats().compute_blur_passes == 0 || gpu.FrameStats().compute_blur_passes == 6));
}

TEST_CASE("LinuxBackdropGlPreservesPartialScanlineTilesAndClampedHalos", "[linux][backdrop][gpu]") {
  const auto api = GENERATE(GDK_GL_API_GL, GDK_GL_API_GLES);
  const bool vertical = GENERATE(false, true);
  const float radius = GENERATE(36.0F, 180.0F, 420.0F);
  GlEnvironment environment(api);
  detail::BackdropGl gpu(api == GDK_GL_API_GLES);
  environment.RequireDisplay();
  const std::size_t width = vertical ? 73 : 1031;
  const std::size_t height = vertical ? 1031 : 73;
  auto command = GlassRect({0, 0, static_cast<float>(width), static_cast<float>(height)}, radius);
  command.effect.refraction = 4.0F;
  command.effect.edge_width = 16.0F;
  const detail::BackdropPlaneBatch batch{.commands = {{&command, {}}}, .sampling_bounds = {}, .output_bounds = {}};
  const auto input = PatternPixels(width, height, true);
  auto actual = input;
  REQUIRE(gpu.Apply(batch, actual.data(), width, height, 0, 0));
  RequireReferenceMatch(batch, input, actual, width, height);
  REQUIRE(gpu.FrameStats().blur_passes == 6);
}

TEST_CASE("LinuxBackdropGlPreservesEveryUniformByteAcrossReciprocalWidths", "[linux][backdrop][gpu]") {
  const auto api = GENERATE(GDK_GL_API_GL, GDK_GL_API_GLES);
  GlEnvironment environment(api);
  detail::BackdropGl gpu(api == GDK_GL_API_GLES);
  environment.RequireDisplay();
  constexpr std::size_t width = 1031;
  for (float radius : {36.0F, 381.0F, 384.0F}) {
    auto command = GlassRect({0, -1, static_cast<float>(width), 3}, radius);
    const detail::BackdropPlaneBatch batch{.commands = {{&command, {}}}, .sampling_bounds = {}, .output_bounds = {}};
    for (unsigned value = 0; value <= 255; ++value) {
      CAPTURE(api, radius, value);
      std::vector<std::uint8_t> expected(width * 4, static_cast<std::uint8_t>(value));
      auto actual = expected;
      REQUIRE(gpu.Apply(batch, actual.data(), width, 1, 0, 0));
      REQUIRE(actual == expected);
    }
  }
}

TEST_CASE("LinuxBackdropGlCroppedRastersPreservePixelsAndInvalidateMovedSources", "[linux][backdrop][gpu]") {
  const auto api = GENERATE(GDK_GL_API_GL, GDK_GL_API_GLES);
  GlEnvironment environment(api);
  detail::LinuxBackdropGl gpu;
  REQUIRE(gpu.TryInitialize(environment.RequireDisplay(), api));
  constexpr std::size_t width = 128;
  constexpr std::size_t height = 96;
  auto command = GlassRect({16, 16, 80, 64}, 6);
  const detail::BackdropPlaneBatch batch{.commands = {{&command, {}}}, .sampling_bounds = {}, .output_bounds = {}};
  const auto render = [&](Rect bounds, bool cropped) {
    std::vector<std::uint8_t> pixels(width * height * 4);
    for (std::size_t y = static_cast<std::size_t>(bounds.y); y < bounds.y + bounds.height; ++y) {
      for (std::size_t x = static_cast<std::size_t>(bounds.x); x < bounds.x + bounds.width; ++x) {
        const std::size_t offset = (y * width + x) * 4;
        pixels[offset] = 20;
        pixels[offset + 1] = 60;
        pixels[offset + 2] = 100;
        pixels[offset + 3] = 128;
      }
    }
    REQUIRE(gpu.BeginFrame(width, height));
    REQUIRE_THROWS_AS(gpu.CompositeRaster(pixels.data(), width * 4, {-1, 0, 10, 10}), std::invalid_argument);
    REQUIRE_THROWS_AS(gpu.CompositeRaster(pixels.data(), width * 4, {0.5F, 0, 10, 10}), std::invalid_argument);
    REQUIRE_THROWS_AS(gpu.CompositeRaster(pixels.data(), width * 4, {0, 0, 129, 10}), std::invalid_argument);
    if (cropped) {
      gpu.CompositeRaster(pixels.data(), width * 4, bounds);
    } else {
      gpu.CompositeRaster(pixels.data(), width * 4);
    }
    REQUIRE(gpu.FrameStats().raster_upload_bytes ==
            (cropped ? static_cast<std::size_t>(bounds.width * bounds.height) * 4 : width * height * 4));
    REQUIRE(gpu.ApplyToFrame(batch, nullptr, 0));
    std::unique_ptr<::GdkTexture, void (*)(::GdkTexture*)> texture(gpu.FinishFrame(),
                                                                   [](::GdkTexture* value) { g_object_unref(value); });
    std::vector<std::uint8_t> actual(width * height * 4);
    gdk_texture_download(texture.get(), actual.data(), width * 4);
    RequireReferenceMatch(batch, pixels, actual, width, height);
    return actual;
  };
  const auto first = render({16, 16, 32, 24}, true);
  REQUIRE(gpu.FrameStats().source_captures == 1);
  REQUIRE(render({16, 16, 32, 24}, true) == first);
  REQUIRE(gpu.FrameStats().blur_cache_hits == 1);
  // Equal compact bytes at a different origin must invalidate the cached optical source.
  REQUIRE(render({48, 32, 32, 24}, true) != first);
  REQUIRE(gpu.FrameStats().source_captures == 1);
  const auto moved = render({48, 32, 32, 24}, false);
  REQUIRE(render({48, 32, 32, 24}, true) == moved);
  REQUIRE(render({8, 8, 112, 80}, true) != moved);
  REQUIRE(render({16, 16, 32, 24}, true) == first);
  REQUIRE(render({0, 0, 0, 0}, true) == std::vector<std::uint8_t>(width * height * 4));
}

TEST_CASE("LinuxBackdropGlGrowsResidentMaskCoverageWithoutReuploadingOverlaps", "[linux][backdrop][gpu]") {
  const auto api = GENERATE(GDK_GL_API_GL, GDK_GL_API_GLES);
  GlEnvironment environment(api);
  detail::LinuxBackdropGl gpu;
  REQUIRE(gpu.TryInitialize(environment.RequireDisplay(), api));
  constexpr std::size_t width = 128;
  constexpr std::size_t height = 96;
  const auto input = PatternPixels(width, height, true);
  const auto render = [&](bool reuse) {
    auto mask = PatternPixels(width, height, true);
    REQUIRE(gpu.BeginFrame(width, height));
    gpu.PushGroup();
    gpu.CompositeRaster(input.data(), width * 4);
    const std::array<Point, 5> origins{{{30, 25}, {40, 35}, {20, 15}, {35, 20}, {30, 25}}};
    for (std::size_t index = 0; index < origins.size(); ++index) {
      const auto origin = origins[index];
      auto command = GlassRect({origin.x, origin.y, 50, 40}, 2);
      command.effect.brightness = 0.7F;
      if (index == origins.size() - 1) {
        std::fill(mask.begin(), mask.end(), 0);
      }
      const detail::BackdropPlaneBatch batch{.commands = {{&command, {}}}, .sampling_bounds = {}, .output_bounds = {}};
      REQUIRE(gpu.ApplyToFrame(batch, mask.data(), width * 4, !reuse || index == 0 || index == origins.size() - 1));
    }
    gpu.PopGroup(nullptr, 0, 0.8F);
    std::unique_ptr<::GdkTexture, void (*)(::GdkTexture*)> texture(gpu.FinishFrame(),
                                                                   [](::GdkTexture* value) { g_object_unref(value); });
    std::vector<std::uint8_t> pixels(width * height * 4);
    gdk_texture_download(texture.get(), pixels.data(), width * 4);
    return pixels;
  };
  const auto expected = render(false);
  const auto forced_upload_bytes = gpu.FrameStats().mask_upload_bytes;
  REQUIRE(render(true) == expected);
  REQUIRE(gpu.FrameStats().mask_upload_bytes < forced_upload_bytes);
  REQUIRE(gpu.FrameStats().mask_upload_bytes < 2 * width * height * 4);
}

TEST_CASE("LinuxBackdropGlPlacesCompactRgbaTexturesAndRetainsTheirCachedPixels", "[linux][backdrop][gpu]") {
  const auto api = GENERATE(GDK_GL_API_GL, GDK_GL_API_GLES);
  GlEnvironment environment(api);
  struct Textures {
    GLuint source = 0;
    GLuint output = 0;
    GLuint framebuffer = 0;
    ~Textures() {
      glDeleteTextures(1, &source);
      glDeleteTextures(1, &output);
      glDeleteFramebuffers(1, &framebuffer);
    }
  } textures;
  glGenTextures(1, &textures.source);
  glGenTextures(1, &textures.output);
  glGenFramebuffers(1, &textures.framebuffer);
  glBindTexture(GL_TEXTURE_2D, textures.source);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, textures.output);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 128, 96, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  detail::BackdropGl gpu(api == GDK_GL_API_GLES);
  const auto render = [&](std::uint64_t identity, Rect bounds, bool cached, bool admit = true) {
    REQUIRE(gpu.BeginFrame(128, 96));
    if (cached) {
      REQUIRE(gpu.CompositeCachedTexture(identity));
    } else {
      std::vector<std::uint8_t> pixels(static_cast<std::size_t>(bounds.width * bounds.height) * 4);
      for (std::size_t offset = 0; offset < pixels.size(); offset += 4) {
        pixels[offset] = 20;
        pixels[offset + 1] = 60;
        pixels[offset + 2] = 100;
        pixels[offset + 3] = 128;
      }
      glBindTexture(GL_TEXTURE_2D, textures.source);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(bounds.width), static_cast<GLsizei>(bounds.height),
                   0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
      gpu.CompositeTexture(textures.source, identity, bounds, admit);
      REQUIRE(gpu.FrameStats().raster_cache_copy_pixels ==
              (admit ? static_cast<std::uint64_t>(bounds.width * bounds.height) : 0));
    }
    REQUIRE(gpu.FrameStats().raster_composite_pixels == static_cast<std::uint64_t>(bounds.width * bounds.height));
    gpu.ExportToTexture(textures.output);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, textures.framebuffer);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures.output, 0);
    std::vector<std::uint8_t> actual(128 * 96 * 4);
    glReadPixels(0, 0, 128, 96, GL_RGBA, GL_UNSIGNED_BYTE, actual.data());
    REQUIRE(glGetError() == GL_NO_ERROR);
    for (int y = 0; y < 96; ++y) {
      for (int x = 0; x < 128; ++x) {
        const bool inside = bounds.Contains({x + 0.5F, y + 0.5F});
        const std::size_t offset = static_cast<std::size_t>(y * 128 + x) * 4;
        REQUIRE(actual[offset] == (inside ? 20 : 0));
        REQUIRE(actual[offset + 1] == (inside ? 60 : 0));
        REQUIRE(actual[offset + 2] == (inside ? 100 : 0));
        REQUIRE(actual[offset + 3] == (inside ? 128 : 0));
      }
    }
    gpu.CancelFrame();
    return actual;
  };
  const auto first = render(1, {16, 8, 32, 16}, false);
  render(2, {100, 80, 16, 8}, false);
  REQUIRE(render(1, {16, 8, 32, 16}, true) == first);
  render(3, {0, 0, 128, 96}, false);
  REQUIRE(render(1, {16, 8, 32, 16}, true) == first);
  const auto uncached = render(4, {48, 48, 32, 16}, false, false);
  REQUIRE(gpu.BeginFrame(128, 96));
  REQUIRE_FALSE(gpu.CompositeCachedTexture(4));
  gpu.CancelFrame();
  REQUIRE(render(4, {48, 48, 32, 16}, false) == uncached);
  REQUIRE(render(4, {48, 48, 32, 16}, true) == uncached);
}

TEST_CASE("LinuxBackdropGlVisibleWorkBoundsKeepTheCompleteFilterHalo", "[linux][backdrop][gpu]") {
  const auto api = GENERATE(GDK_GL_API_GL, GDK_GL_API_GLES);
  const float radius = GENERATE(0.0F, 36.0F);
  const auto quality = GENERATE(BackdropBlurQuality::Precise, BackdropBlurQuality::Fast);
  const auto mapping = GENERATE(Transform2D{}, Transform2D{1.5F, 0, 0, 0.75F, -20, 3},
                                Transform2D{0, 1, -1, 0, 1031, 0});
  GlEnvironment environment(api);
  detail::BackdropGl full(api == GDK_GL_API_GLES);
  detail::BackdropGl cropped(api == GDK_GL_API_GLES);
  environment.RequireDisplay();
  constexpr std::size_t width = 1031, height = 385;
  auto source = PatternPixels(width, height, true);
  auto command = GlassRect({-50, -50, 1500, 1500}, radius);
  command.effect.blur_quality = quality;
  command.effect.refraction = 4;
  command.effect.edge_width = 1000;
  const detail::BackdropPlaneBatch batch{.commands = {{&command, mapping}}, .sampling_bounds = {}, .output_bounds = {}};
  GLuint texture = 0, framebuffer = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glGenFramebuffers(1, &framebuffer);
  for (Rect clip : {Rect{0, 0, 100, 50}, Rect{400, 100, 150, 125}, Rect{900, 300, 131, 85}, Rect{}}) {
    ++source[0];
    std::vector<std::uint8_t> mask(source.size(), 0);
    for (std::size_t y = 0; y < height; ++y) {
      for (std::size_t x = 0; x < width; ++x) {
        if (clip.Contains({static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F})) {
          mask[(y * width + x) * 4 + 3] = x % 3 == 0 ? 128 : 255;
        }
      }
    }
    const auto render = [&](detail::BackdropGl& engine, std::optional<Rect> visible) {
      REQUIRE(engine.BeginFrame(width, height));
      engine.CompositeRaster(source.data(), width * 4);
      REQUIRE(engine.ApplyToFrame(batch, mask.data(), width * 4, true, std::nullopt, visible));
      engine.ExportToTexture(texture);
      glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
      std::vector<std::uint8_t> result(source.size());
      glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, result.data());
      REQUIRE(glGetError() == GL_NO_ERROR);
      engine.CancelFrame();
      return result;
    };
    const auto expected = render(full, std::nullopt);
    const auto actual = render(cropped, clip);
    if (radius > 0) {
      REQUIRE(cropped.FrameStats().blur_pixels < full.FrameStats().blur_pixels / 2);
    } else {
      REQUIRE(cropped.FrameStats().blur_pixels == 0);
    }
    unsigned maximum = 0;
    std::uint64_t total = 0;
    bool outside_preserved = true;
    for (std::size_t i = 0; i < actual.size(); ++i) {
      const auto error = static_cast<unsigned>(std::abs(static_cast<int>(actual[i]) - expected[i]));
      maximum = std::max(maximum, error);
      total += error;
      if (mask[i / 4 * 4 + 3] == 0) {
        outside_preserved = outside_preserved && actual[i] == expected[i];
      }
    }
    CAPTURE(api, radius, clip, maximum);
    REQUIRE(outside_preserved);
    REQUIRE(maximum <= 3);
    REQUIRE(static_cast<double>(total) / actual.size() <= 0.5);
  }
  glDeleteFramebuffers(1, &framebuffer);
  glDeleteTextures(1, &texture);
}

TEST_CASE("LinuxBackdropGlRectangleReplacementMatchesCoverageMasks", "[linux][backdrop][gpu]") {
  for (GdkGLAPI api : {GDK_GL_API_GL, GDK_GL_API_GLES}) {
    GlEnvironment environment(api);
    detail::BackdropGl engine(api == GDK_GL_API_GLES);
    constexpr std::size_t width = 96;
    constexpr std::size_t height = 64;
    const auto source = PatternPixels(width, height, true);
    auto command = GlassRect({8, 6, 75, 48}, 12);
    command.effect.refraction = 5;
    const detail::BackdropPlaneBatch batch{.commands = {{&command, {}}}, .sampling_bounds = {}, .output_bounds = {}};
    GLuint texture = 0;
    GLuint framebuffer = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &framebuffer);
    const auto render = [&](std::optional<Rect> clip, const std::uint8_t* mask, bool optical) {
      REQUIRE(engine.BeginFrame(width, height));
      engine.CompositeRaster(source.data(), width * 4);
      if (optical) {
        REQUIRE(engine.ApplyToFrame(batch, mask, width * 4, true, clip));
        REQUIRE((engine.FrameStats().mask_upload_bytes == 0) == (mask == nullptr));
      }
      engine.ExportToTexture(texture);
      glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
      REQUIRE(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
      std::vector<std::uint8_t> result(source.size());
      glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, result.data());
      REQUIRE(glGetError() == GL_NO_ERROR);
      engine.CancelFrame();
      return result;
    };
    const auto unchanged = render(std::nullopt, nullptr, false);
    for (Rect clip : {Rect{24, 18, 35, 20}, Rect{}, Rect{0, 0, width, height}}) {
      std::vector<std::uint8_t> mask(source.size(), 0);
      for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
          if (x >= clip.x && x < clip.x + clip.width && y >= clip.y && y < clip.y + clip.height) {
            mask[(y * width + x) * 4 + 3] = 255;
          }
        }
      }
      const auto masked = render(std::nullopt, mask.data(), true);
      const auto scissored = render(clip, nullptr, true);
      REQUIRE(scissored == masked);
      REQUIRE(render(clip, mask.data(), true) == scissored);
      if (clip.IsEmpty()) {
        REQUIRE(scissored == unchanged);
      } else {
        REQUIRE(scissored != unchanged);
      }
    }
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
  }
}

TEST_CASE("LinuxBackdropGlMaskedReplacementPreservesPartialAlphaAndCapturePlacement", "[linux][backdrop][gpu]") {
  const auto api = GENERATE(GDK_GL_API_GL, GDK_GL_API_GLES);
  const bool isolated = GENERATE(false, true);
  GlEnvironment environment(api);
  detail::BackdropGl engine(api == GDK_GL_API_GLES);
  constexpr std::size_t width = 383, height = 257;
  const auto source = PatternPixels(width, height, true);
  auto command = GlassRect({180, 100, 80, 60}, 6);
  command.effect.refraction = 3;
  command.effect.brightness = 0.6F;
  const detail::BackdropPlaneBatch batch{.commands = {{&command, {}}}, .sampling_bounds = {}, .output_bounds = {}};
  GLuint texture = 0, framebuffer = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glGenFramebuffers(1, &framebuffer);
  const auto render = [&](bool optical, const std::uint8_t* mask, std::optional<Rect> clip) {
    REQUIRE(engine.BeginFrame(width, height));
    if (isolated) engine.PushGroup();
    engine.CompositeRaster(source.data(), width * 4);
    if (optical) REQUIRE(engine.ApplyToFrame(batch, mask, width * 4, true, clip));
    if (isolated) engine.PopGroup(nullptr, 0, 1.0F);
    engine.ExportToTexture(texture);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    REQUIRE(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    std::vector<std::uint8_t> result(source.size());
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, result.data());
    REQUIRE(glGetError() == GL_NO_ERROR);
    engine.CancelFrame();
    return result;
  };
  const auto unchanged = render(false, nullptr, std::nullopt);
  const auto filtered = render(true, nullptr, std::nullopt);
  std::vector<std::uint8_t> mask(source.size(), 0);
  constexpr std::array<unsigned, 4> coverage{0, 17, 128, 255};
  for (std::size_t y = 0; y < height; ++y) {
    for (std::size_t x = 0; x < width; ++x) {
      mask[(y * width + x) * 4 + 3] = coverage[(x / 3 + y / 5) % coverage.size()];
    }
  }
  for (std::optional<Rect> clip : {std::optional<Rect>{}, std::optional<Rect>{{193, 113, 45, 29}},
                                  std::optional<Rect>{Rect{}}, std::optional<Rect>{{0, 0, 12, 12}}}) {
    const auto actual = render(true, mask.data(), clip);
    unsigned maximum = 0;
    bool outside_preserved = true;
    bool changed = false;
    for (std::size_t y = 0; y < height; ++y) {
      for (std::size_t x = 0; x < width; ++x) {
        const std::size_t offset = (y * width + x) * 4;
        const bool inside = !clip || clip->Contains({static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F});
        const unsigned alpha = inside ? mask[offset + 3] : 0;
        for (std::size_t channel = 0; channel < 4; ++channel) {
          const auto i = offset + channel;
          const auto expected = (unchanged[i] * (255U - alpha) + filtered[i] * alpha + 127U) / 255U;
          maximum = std::max(maximum, static_cast<unsigned>(std::abs(static_cast<int>(actual[i]) -
                                                                   static_cast<int>(expected))));
          if (alpha == 0) outside_preserved = outside_preserved && actual[i] == unchanged[i];
          changed = changed || actual[i] != unchanged[i];
        }
      }
    }
    CAPTURE(api, isolated, clip, maximum);
    REQUIRE(maximum <= 1);
    REQUIRE(outside_preserved);
    REQUIRE(changed == (!clip || clip->Intersects({180, 100, 80, 60})));
  }
  glDeleteFramebuffers(1, &framebuffer);
  glDeleteTextures(1, &texture);
}

TEST_CASE("LinuxBackdropGlStableSourceReusesMovingWindowsWithoutStalePixels", "[linux][backdrop][gpu]") {
  GlEnvironment environment;
  detail::LinuxBackdropGl gpu;
  REQUIRE(gpu.TryInitialize(environment.RequireDisplay()));
  constexpr std::size_t width = 512;
  constexpr std::size_t height = 384;
  auto source = PatternPixels(width, height, true);
  auto command = GlassRect({24, 24, 40, 32}, 12);
  command.effect.refraction = 3;
  const detail::BackdropPlaneBatch batch{.commands = {{&command, {}}}, .sampling_bounds = {}, .output_bounds = {}};
  const auto render = [&] {
    REQUIRE(gpu.BeginFrame(width, height));
    gpu.CompositeRaster(source.data(), width * 4);
    REQUIRE(gpu.ApplyToFrame(batch, nullptr, 0));
    const auto release = [](::GdkTexture* value) { g_object_unref(value); };
    const std::unique_ptr<::GdkTexture, decltype(release)> texture(gpu.FinishFrame(), release);
    REQUIRE(texture != nullptr);
    std::vector<std::uint8_t> pixels(source.size());
    GdkTextureDownloader* downloader = gdk_texture_downloader_new(texture.get());
    gdk_texture_downloader_set_format(downloader, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED);
    gdk_texture_downloader_download_into(downloader, pixels.data(), width * 4);
    gdk_texture_downloader_free(downloader);
    RequireReferenceMatch(batch, source, pixels, width, height);
  };
  for (int frame = 0; frame < 5; ++frame) {
    render();
    REQUIRE(gpu.FrameStats().stable_full_frame_planes == 0);
  }
  command.shapes.front().rect = {280, 230, 40, 32};
  render();
  REQUIRE(gpu.FrameStats().stable_full_frame_planes == 1);
  command.shapes.front().rect = {420, 300, 40, 32};
  render();
  REQUIRE(gpu.FrameStats().source_captures == 0);
  REQUIRE(gpu.FrameStats().blur_passes == 0);
  REQUIRE(gpu.FrameStats().blur_cache_hits == 1);
  source = PatternPixels(width, height);
  render();
  REQUIRE(gpu.FrameStats().stable_full_frame_planes == 0);
  REQUIRE(gpu.FrameStats().source_captures == 1);
  REQUIRE(gpu.FrameStats().blur_passes == 6);
}

TEST_CASE("LinuxBackdropFastBlurMatchesIndependentGaussianAtFullResolution", "[linux][backdrop][gpu][fast]") {
  for (GdkGLAPI api : {GDK_GL_API_GL, GDK_GL_API_GLES}) {
    GlEnvironment environment(api);
    detail::BackdropGl gpu(api == GDK_GL_API_GLES);
    constexpr std::size_t width = 129, height = 97;
    auto command = GlassRect({0, 0, width, height}, 6);
    command.shapes.front().corner_radii = {};
    command.effect.blur_quality = BackdropBlurQuality::Fast;
    const detail::BackdropPlaneBatch batch{.commands = {{&command, {}}}, .sampling_bounds = {}, .output_bounds = {}};
    const auto input = PatternPixels(width, height, true);
    auto expected = input;
    const double sigma = std::sqrt(6.0);
    const int support = static_cast<int>(std::ceil(3 * sigma));
    std::vector<double> weights(2 * support + 1);
    double sum = 0;
    for (int distance = -support; distance <= support; ++distance) {
      weights[distance + support] = std::exp(-0.5 * distance * distance / (sigma * sigma));
      sum += weights[distance + support];
    }
    for (double& weight : weights) { weight /= sum; }
    for (bool horizontal : {true, false}) {
      auto filtered = expected;
      for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
          for (std::size_t channel = 0; channel < 4; ++channel) {
            double value = 0;
            for (int distance = -support; distance <= support; ++distance) {
              const int sx = std::clamp(static_cast<int>(x) + (horizontal ? distance : 0), 0, int(width) - 1);
              const int sy = std::clamp(static_cast<int>(y) + (horizontal ? 0 : distance), 0, int(height) - 1);
              value += expected[(sy * width + sx) * 4 + channel] * weights[distance + support];
            }
            filtered[(y * width + x) * 4 + channel] = static_cast<std::uint8_t>(std::lround(value));
          }
        }
      }
      expected = std::move(filtered);
    }
    auto actual = input;
    REQUIRE(gpu.Apply(batch, actual.data(), width, height, 0, 0));
    std::uint64_t total = 0;
    unsigned maximum = 0;
    for (std::size_t index = 0; index < actual.size(); ++index) {
      const auto error = static_cast<unsigned>(std::abs(int(actual[index]) - int(expected[index])));
      maximum = std::max(maximum, error);
      total += error;
    }
    CAPTURE(api, maximum);
    REQUIRE(maximum <= 1);
    REQUIRE(static_cast<double>(total) / actual.size() <= 0.5);
    REQUIRE(gpu.FrameStats().blur_passes == 4);
    REQUIRE(gpu.FrameStats().integer_blur_passes == 0);
  }
}

TEST_CASE("LinuxBackdropFastBlurPreservesCropPhaseAlphaAndExterior", "[linux][backdrop][gpu][fast]") {
  for (GdkGLAPI api : {GDK_GL_API_GL, GDK_GL_API_GLES}) {
    GlEnvironment environment(api);
    detail::BackdropGl gpu(api == GDK_GL_API_GLES);
    constexpr std::size_t width = 384, height = 288, world_width = 400;
    const auto world = PatternPixels(world_width, 304, true);
    auto command = GlassRect({120, 96, 128, 96}, 18);
    command.effect.blur_quality = BackdropBlurQuality::Fast;
    const detail::BackdropPlaneBatch batch{.commands = {{&command, {}}}, .sampling_bounds = {}, .output_bounds = {}};
    for (float radius : {0.0F, 3.0F, 18.0F, 48.0F}) {
      command.effect.blur_radius = radius;
      std::vector<std::uint8_t> reference;
      for (std::size_t origin = 0; origin < 8; ++origin) {
        std::vector<std::uint8_t> input(width * height * 4);
        for (std::size_t y = 0; y < height; ++y) {
          std::copy_n(world.begin() + ((y + origin) * world_width + origin) * 4, width * 4,
                      input.begin() + y * width * 4);
        }
        auto actual = input;
        REQUIRE(gpu.Apply(batch, actual.data(), width, height, origin, origin));
        if (origin == 0) { reference = actual; }
        for (std::size_t y = 0; y < height; ++y) {
          for (std::size_t x = 0; x < width; ++x) {
            const auto offset = (y * width + x) * 4;
            for (std::size_t channel = 0; channel < 3; ++channel) {
              REQUIRE(actual[offset + channel] <= actual[offset + 3]);
            }
            const bool inside = command.shapes.front().rect.Contains(
                {float(x + origin) + 0.5F, float(y + origin) + 0.5F});
            if (!inside || radius == 0) {
              REQUIRE(std::equal(input.begin() + offset, input.begin() + offset + 4, actual.begin() + offset));
            }
            if (x + origin >= 124 && x + origin < 240 && y + origin >= 100 && y + origin < 184) {
              for (std::size_t channel = 0; channel < 4; ++channel) {
                const int error = std::abs(int(actual[offset + channel])
                    - int(reference[((y + origin) * width + x + origin) * 4 + channel]));
                REQUIRE(error <= 1);
              }
            }
          }
        }
      }
    }
  }
}

TEST_CASE("LinuxBackdropFastAndPreciseKernelsDoNotAliasInSharedPlanesOrCaches", "[linux][backdrop][gpu][fast]") {
  for (GdkGLAPI api : {GDK_GL_API_GL, GDK_GL_API_GLES}) {
    GlEnvironment environment(api);
    detail::BackdropGl gpu(api == GDK_GL_API_GLES);
    constexpr std::size_t width = 192, height = 96;
    const auto input = PatternPixels(width, height, true);
    auto left = GlassRect({8, 8, 64, 72}, 18);
    auto right = GlassRect({120, 8, 64, 72}, 18);
    right.effect.blur_quality = BackdropBlurQuality::Fast;
    const detail::BackdropPlaneBatch mixed{
        .commands = {{&left, {}}, {&right, {}}}, .sampling_bounds = {}, .output_bounds = {}};
    auto actual = input;
    REQUIRE(gpu.Apply(mixed, actual.data(), width, height, 0, 0));
    REQUIRE(gpu.FrameStats().blur_passes == 10);
    auto expected = input;
    detail::BackdropGl fresh(api == GDK_GL_API_GLES);
    REQUIRE(fresh.Apply({.commands = {{&left, {}}}, .sampling_bounds = {}, .output_bounds = {}},
                        expected.data(), width, height, 0, 0));
    REQUIRE(fresh.Apply({.commands = {{&right, {}}}, .sampling_bounds = {}, .output_bounds = {}},
                        expected.data(), width, height, 0, 0));
    REQUIRE(actual == expected);

    const auto prior_passes = gpu.FrameStats().blur_passes;
    left.effect.blur_quality = BackdropBlurQuality::Fast;
    actual = input;
    REQUIRE(gpu.Apply(mixed, actual.data(), width, height, 0, 0));
    REQUIRE(gpu.FrameStats().blur_passes - prior_passes == 4);

    left.shapes.front().rect = {0, 0, width, height};
    const detail::BackdropPlaneBatch batch{.commands = {{&left, {}}}, .sampling_bounds = {}, .output_bounds = {}};
    GLuint texture = 0, framebuffer = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &framebuffer);
    std::vector<std::uint8_t> precise_pixels, fast_pixels;
    int frame = 0;
    for (BackdropBlurQuality quality : {BackdropBlurQuality::Precise, BackdropBlurQuality::Fast,
                                        BackdropBlurQuality::Precise, BackdropBlurQuality::Fast}) {
      left.effect.blur_quality = quality;
      REQUIRE(gpu.BeginFrame(width, height));
      gpu.CompositeRaster(input.data(), width * 4);
      REQUIRE(gpu.ApplyToFrame(batch, nullptr, 0));
      REQUIRE(gpu.FrameStats().blur_passes == (frame >= 2 ? 0 : quality == BackdropBlurQuality::Fast ? 4 : 6));
      REQUIRE(gpu.FrameStats().blur_cache_hits == (frame >= 2 ? 1 : 0));
      gpu.ExportToTexture(texture);
      glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
      REQUIRE(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
      std::vector<std::uint8_t> pixels(input.size());
      glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
      REQUIRE(glGetError() == GL_NO_ERROR);
      auto& reference = quality == BackdropBlurQuality::Fast ? fast_pixels : precise_pixels;
      if (reference.empty()) { reference = pixels; }
      else { REQUIRE(pixels == reference); }
      gpu.CancelFrame();
      ++frame;
    }
    REQUIRE(precise_pixels != fast_pixels);
    left.effect.blur_quality = BackdropBlurQuality::Precise;
    right.effect.blur_quality = BackdropBlurQuality::Fast;
    const auto large_input = PatternPixels(512, 256, true);
    for (int step = 0; step < 5; ++step) {
      left.shapes.front().rect = {40.0F + step * 17, 60, 40, 32};
      right.shapes.front().rect = {200.0F + step * 17, 60, 40, 32};
      REQUIRE(gpu.BeginFrame(512, 256));
      gpu.CompositeRaster(large_input.data(), 512 * 4);
      REQUIRE(gpu.ApplyToFrame(mixed, nullptr, 0));
      REQUIRE(gpu.FrameStats().stable_full_frame_planes == 0);
      gpu.CancelFrame();
    }
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
  }
}

TEST_CASE("LinuxBackdropFastQualityRetainsPreciseForMinificationAndOversizedKernels", "[linux][backdrop][gpu][fast]") {
  for (GdkGLAPI api : {GDK_GL_API_GL, GDK_GL_API_GLES}) {
    GlEnvironment environment(api);
    detail::BackdropGl gpu(api == GDK_GL_API_GLES);
    constexpr std::size_t width = 1031, height = 73;
    const auto input = PatternPixels(width, height, true);
    for (Transform2D mapping : {Transform2D{}, Transform2D{0.5F, 0, 0, 0.75F, 0, 0}}) {
      auto command = GlassRect({0, 0, 2062, 146}, mapping.IsIdentity() ? 2048.0F : 18.0F);
      const detail::BackdropPlaneBatch batch{
          .commands = {{&command, mapping}}, .sampling_bounds = {}, .output_bounds = {}};
      auto expected = input;
      REQUIRE(gpu.Apply(batch, expected.data(), width, height, 0, 0));
      command.effect.blur_quality = BackdropBlurQuality::Fast;
      auto actual = input;
      const auto previous = gpu.FrameStats().integer_blur_passes;
      REQUIRE(gpu.Apply(batch, actual.data(), width, height, 0, 0));
      REQUIRE(gpu.FrameStats().integer_blur_passes - previous == 6);
      REQUIRE(actual == expected);
    }
  }
}

TEST_CASE("LinuxBackdropFastBlurKeepsOpacityIsolationAndLaterForeground", "[linux][backdrop][gpu][fast]") {
  for (GdkGLAPI api : {GDK_GL_API_GL, GDK_GL_API_GLES}) {
    GlEnvironment environment(api);
    detail::LinuxBackdropGl gpu;
    REQUIRE(gpu.TryInitialize(environment.RequireDisplay(), api));
    constexpr std::size_t width = 128, height = 96;
    const auto input = PatternPixels(width, height, true);
    auto command = GlassRect({24, 24, 32, 32}, 18);
    command.effect.refraction = 2;
    command.effect.highlight = 0.2F;
    command.effect.blur_quality = BackdropBlurQuality::Fast;
    const detail::BackdropPlaneBatch batch{.commands = {{&command, {}}}, .sampling_bounds = {}, .output_bounds = {}};
    auto filtered = input;
    detail::BackdropGl reference(api == GDK_GL_API_GLES);
    REQUIRE(reference.Apply(batch, filtered.data(), width, height, 0, 0));
    std::vector<std::uint8_t> background(input.size()), foreground(input.size(), 0);
    for (std::size_t offset = 0; offset < background.size(); offset += 4) {
      background[offset] = 80;
      background[offset + 1] = 20;
      background[offset + 2] = 10;
      background[offset + 3] = 255;
    }
    for (std::size_t y = 60; y < 63; ++y) {
      for (std::size_t x = 60; x < 63; ++x) {
        std::fill_n(foreground.begin() + (y * width + x) * 4, 4, 255);
        std::fill_n(filtered.begin() + (y * width + x) * 4, 4, 255);
      }
    }
    REQUIRE(gpu.BeginFrame(width, height));
    gpu.CompositeRaster(background.data(), width * 4);
    gpu.PushGroup();
    gpu.CompositeRaster(input.data(), width * 4);
    REQUIRE(gpu.ApplyToFrame(batch, nullptr, 0));
    gpu.CompositeRaster(foreground.data(), width * 4);
    gpu.PopGroup(nullptr, 0, 0.5F);
    const auto release = [](::GdkTexture* value) { g_object_unref(value); };
    const std::unique_ptr<::GdkTexture, decltype(release)> texture(gpu.FinishFrame(), release);
    REQUIRE(texture != nullptr);
    std::vector<std::uint8_t> actual(input.size());
    GdkTextureDownloader* downloader = gdk_texture_downloader_new(texture.get());
    gdk_texture_downloader_set_format(downloader, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED);
    gdk_texture_downloader_download_into(downloader, actual.data(), width * 4);
    gdk_texture_downloader_free(downloader);
    for (std::size_t offset = 0; offset < actual.size(); offset += 4) {
      const double alpha = filtered[offset + 3] / 510.0;
      for (std::size_t channel = 0; channel < 4; ++channel) {
        const int expected = static_cast<int>(std::lround(filtered[offset + channel] * 0.5
            + background[offset + channel] * (1 - alpha)));
        REQUIRE(std::abs(int(actual[offset + channel]) - expected) <= 2);
      }
    }
    REQUIRE(gpu.FrameStats().readback_bytes == 0);
  }
}

TEST_CASE("LinuxBackdropGlPreservesAllShapesAcrossArrayGrowthAndShrink", "[linux][backdrop][gpu]") {
  const auto api = GENERATE(GDK_GL_API_GL, GDK_GL_API_GLES);
  GlEnvironment environment(api);
  detail::LinuxBackdropGl gpu;
  REQUIRE(gpu.TryInitialize(environment.RequireDisplay(), api));
  constexpr std::size_t width = 256, height = 192;
  const auto input = PatternPixels(width, height, true);
  for (const auto quality : {BackdropBlurQuality::Precise, BackdropBlurQuality::Fast}) {
    for (const int count : {1, 2, 3, 4, 16, 17, 32, 33, 64, 17, 3, 1}) {
      DrawBackdropCommand command{.shapes = {},
                                  .effect = {.blur_radius = 0, .brightness = 0.65F, .blur_quality = quality}};
      for (int index = 0; index < count; ++index) {
        command.shapes.push_back({{8.25F + index % 8 * 30, 8.5F + index / 8 * 22,
                                   index % 11 == 5 ? 0.0F : 26.0F, 18}, CornerRadii{2, 4, 3, 1}});
      }
      for (const float fusion : {0.0F, 6.0F}) {
        CAPTURE(api, quality, count, fusion);
        command.effect.fusion_radius = fusion;
        const detail::BackdropPlaneBatch batch{.commands = {{&command, {}}},
                                               .sampling_bounds = {}, .output_bounds = {}};
        auto actual = input;
        REQUIRE(gpu.Apply(batch, actual.data(), width, height, 0, 0));
        RequireReferenceMatch(batch, input, actual, width, height);
      }
    }
  }
}

TEST_CASE("LinuxBackdropGlKeepsRefractiveNormalsStableAcrossPixelMappings", "[linux][backdrop][gpu]") {
  const auto api = GENERATE(GDK_GL_API_GL, GDK_GL_API_GLES);
  GlEnvironment environment(api);
  detail::LinuxBackdropGl gpu;
  REQUIRE(gpu.TryInitialize(environment.RequireDisplay(), api));
  constexpr std::size_t width = 256, height = 192;
  const auto input = PatternPixels(width, height, true);
  for (const Transform2D mapping : {Transform2D{1, 0, 0, 1, 3, 5}, Transform2D{1.2F, 0.35F, 0.2F, 0.8F}}) {
    for (const int count : {1, 2, 3, 4, 16, 17, 32, 33, 64, 17, 3, 1}) {
      DrawBackdropCommand command{.shapes = {},
                                  .effect = {.blur_radius = 0, .refraction = 3, .highlight = 0.2F,
                                             .brightness = 0.65F}};
      for (int index = 0; index < count; ++index) {
        command.shapes.push_back({{8.25F + index % 8 * 30, 8.5F + index / 8 * 22,
                                   index % 11 == 5 ? 0.0F : 26.0F, 18}, CornerRadii{2, 4, 3, 1}});
      }
      for (const float fusion : {0.0F, 6.0F}) {
        CAPTURE(api, count, fusion, mapping.m11, mapping.translate_x);
        command.effect.fusion_radius = fusion;
        const detail::BackdropPlaneBatch batch{.commands = {{&command, mapping}},
                                               .sampling_bounds = {}, .output_bounds = {}};
        auto actual = input;
        REQUIRE(gpu.Apply(batch, actual.data(), width, height, 0, 0));
        RequireReferenceMatch(batch, input, actual, width, height);
      }
    }
  }
}

TEST_CASE("LinuxBackdropGlRetiresUncreatedProgramsWithoutErrors", "[linux][backdrop][gpu]") {
  const auto api = GENERATE(GDK_GL_API_GL, GDK_GL_API_GLES);
  GlEnvironment environment(api);
  environment.RequireDisplay();
  for (int iteration = 0; iteration < 4; ++iteration) {
    {
      detail::BackdropGl gpu(api == GDK_GL_API_GLES);
    }
    REQUIRE(glGetError() == GL_NO_ERROR);
  }
}

} // namespace huxerui::test
