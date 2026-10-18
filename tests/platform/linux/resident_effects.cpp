#include "linux_internal.h"

#include <catch2/catch_amalgamated.hpp>
#include <gtk/gtk.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <huxerui/effects.h>
#include <huxerui/geometry.h>
#include <huxerui/paint.h>
#include <huxerui/render_scene.h>
#include <huxerui/text.h>

#include "linux_renderer.h"

namespace huxerui::test {
namespace {

using TextureHandle = std::unique_ptr<::GdkTexture, void (*)(::GdkTexture*)>;
using RenderNodeHandle = std::unique_ptr<GskRenderNode, void (*)(GskRenderNode*)>;

GdkDisplay* RequireGpuDisplay() {
  GdkDisplay* display = gdk_display_get_default();
  if (display == nullptr) {
    SKIP("HuxerUI Linux resident backdrop tests require a GTK display");
  }
  const std::array<std::pair<GdkGLAPI, int>, 2> candidates{{{GDK_GL_API_GL, 3}, {GDK_GL_API_GLES, 0}}};
  std::string reason;
  for (const auto& [api, minor] : candidates) {
    GError* error = nullptr;
    GdkGLContext* context = gdk_display_create_gl_context(display, &error);
    if (context != nullptr) {
      gdk_gl_context_set_allowed_apis(context, api);
      gdk_gl_context_set_required_version(context, 3, minor);
    }
    const bool realized = context != nullptr && gdk_gl_context_realize(context, &error) != FALSE;
    if (error != nullptr) {
      reason = error->message;
      g_error_free(error);
    }
    if (context != nullptr) {
      g_object_unref(context);
    }
    if (realized) {
      return display;
    }
  }
  SKIP("HuxerUI Linux resident backdrop tests require OpenGL 3.3 or OpenGL ES 3.0: " << reason);
  return nullptr;
}

RenderNodeHandle SnapshotNode(detail::LinuxRenderer& renderer, const RenderFrame& frame) {
  GtkSnapshot* snapshot = gtk_snapshot_new();
  renderer.Snapshot(snapshot, frame);
  RenderNodeHandle node(gtk_snapshot_free_to_node(snapshot), gsk_render_node_unref);
  REQUIRE(node != nullptr);
  return node;
}

TextureHandle SnapshotTexture(detail::LinuxRenderer& renderer, const RenderFrame& frame) {
  const auto node = SnapshotNode(renderer, frame);
  REQUIRE(gsk_render_node_get_node_type(node.get()) == GSK_TEXTURE_NODE);
  auto* texture = gsk_texture_node_get_texture(node.get());
  g_object_ref(texture);
  return TextureHandle(texture, [](::GdkTexture* value) { g_object_unref(value); });
}

std::vector<std::uint8_t> Download(::GdkTexture* texture) {
  const int width = gdk_texture_get_width(texture);
  const int height = gdk_texture_get_height(texture);
  REQUIRE(width > 0);
  REQUIRE(height > 0);
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
  gdk_texture_download(texture, pixels.data(), static_cast<std::size_t>(width) * 4);
  return pixels;
}

std::vector<std::uint8_t> DrawNode(const RenderNodeHandle& node, int width, int height) {
  std::unique_ptr<cairo_surface_t, void (*)(cairo_surface_t*)> surface(
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height), cairo_surface_destroy);
  REQUIRE(cairo_surface_status(surface.get()) == CAIRO_STATUS_SUCCESS);
  cairo_t* context = cairo_create(surface.get());
  REQUIRE(cairo_status(context) == CAIRO_STATUS_SUCCESS);
  gsk_render_node_draw(node.get(), context);
  cairo_destroy(context);
  cairo_surface_flush(surface.get());
  const auto* pixels = cairo_image_surface_get_data(surface.get());
  return std::vector<std::uint8_t>(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
}

void RequireMatchingPixels(const std::vector<std::uint8_t>& actual, const std::vector<std::uint8_t>& expected) {
  REQUIRE(actual.size() == expected.size());
  REQUIRE_FALSE(actual.empty());
  int maximum_error = 0;
  std::uint64_t total_error = 0;
  for (std::size_t index = 0; index != actual.size(); ++index) {
    const int error = std::abs(static_cast<int>(actual[index]) - static_cast<int>(expected[index]));
    maximum_error = std::max(maximum_error, error);
    total_error += static_cast<std::uint64_t>(error);
  }
  const double mean_error = static_cast<double>(total_error) / static_cast<double>(actual.size());
  CAPTURE(maximum_error, mean_error);
  REQUIRE(maximum_error <= 3);
  REQUIRE(mean_error <= 0.5);
}

void PaintBackground(RenderNode& root, bool changed = false) {
  PaintContext paint(root.content, {0.0F, 0.0F, 220.0F, 100.0F});
  for (int column = 0; column != 11; ++column) {
    paint.DrawRect({column * 20.0F, 0.0F, 20.0F, 100.0F},
                   changed ? Color::Rgb(30, 110, static_cast<std::uint8_t>(column * 20), 0.65F)
                           : Color::Rgb(static_cast<std::uint8_t>(column * 20), 50, 140, 0.65F));
  }
  paint.DrawCircle({100.0F, 48.0F}, 28.0F, Color::Rgb(40, 200, 100, 0.4F));
  paint.Finish();
}

void PaintControl(RenderNode& node, std::string label) {
  PaintContext paint(node.content, {0.0F, 0.0F, 60.0F, 36.0F});
  paint.DrawBackdrop({0.0F, 0.0F, 60.0F, 36.0F},
                     BackdropEffect{.blur_radius = 5.0F, .refraction = 3.0F, .highlight = 0.2F}, CornerRadii{6.0F});
  paint.DrawRect({0.0F, 0.0F, 60.0F, 36.0F}, Color::Rgb(255, 255, 255, 0.2F), CornerRadii{6.0F});
  paint.DrawText({6.0F, 8.0F, 48.0F, 20.0F}, std::move(label), TextStyle{Font::System(13.0F), Color::White()});
  paint.Finish();
}

void Configure(detail::LinuxRenderer& renderer, GdkDisplay* display = nullptr) {
  renderer.Initialize();
  renderer.SetViewport({220.0F, 100.0F});
  renderer.SetDeviceScale(2.0F);
  renderer.SetDisplay(display);
}

} // namespace

TEST_CASE("LinuxResidentBackdropMatchesCpuForSiblingControlsAndOverlappingLayers", "[backdrop][gpu]") {
  GdkDisplay* display = RequireGpuDisplay();
  RenderNode first;
  first.offset = {12.0F, 12.0F};
  PaintControl(first, "First");
  RenderNode second;
  second.offset = {142.0F, 16.0F};
  second.transform = {.m11 = 0.98F, .m12 = 0.14F, .m21 = -0.14F, .m22 = 0.98F};
  PaintControl(second, "Second");
  RenderNode later;
  later.offset = {44.0F, 28.0F};
  PaintControl(later, "Later");
  RenderNode root;
  PaintBackground(root);
  root.children = {&first, &second};
  RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};
  detail::LinuxRenderer gpu;
  detail::LinuxRenderer cpu;
  Configure(gpu, display);
  Configure(cpu);
  auto actual = SnapshotTexture(gpu, frame);
  REQUIRE(gpu.BackdropStats().plane_count == 1);
  REQUIRE(gpu.BackdropStats().gpu_plane_count == 1);
  REQUIRE(gpu.BackdropStats().command_count == 2);
  REQUIRE(gpu.BackdropStats().readback_bytes == 0);
  auto expected = SnapshotTexture(cpu, frame);
  RequireMatchingPixels(Download(actual.get()), Download(expected.get()));
  root.children.push_back(&later);
  ++frame.revision;
  actual = SnapshotTexture(gpu, frame);
  REQUIRE(gpu.BackdropStats().plane_count == 2);
  REQUIRE(gpu.BackdropStats().gpu_plane_count == 2);
  REQUIRE(gpu.BackdropStats().command_count == 3);
  REQUIRE(gpu.BackdropStats().readback_bytes == 0);
  expected = SnapshotTexture(cpu, frame);
  RequireMatchingPixels(Download(actual.get()), Download(expected.get()));
  gpu.Discard();
  cpu.Discard();
}

TEST_CASE("LinuxResidentBackdropMatchesCpuForNestedOpacityAndInheritedPathClips", "[backdrop][gpu]") {
  GdkDisplay* display = RequireGpuDisplay();
  RenderNode first;
  first.offset = {12.0F, 12.0F};
  PaintControl(first, "Outer");
  RenderNode second;
  second.offset = {142.0F, 16.0F};
  PaintControl(second, "Inner");
  RenderNode inner;
  inner.opacity = 0.5F;
  PaintContext inner_background(inner.content, {0.0F, 0.0F, 220.0F, 100.0F});
  inner_background.DrawRect({130.0F, 0.0F, 50.0F, 100.0F}, Color::Rgb(40, 70, 220, 0.55F));
  inner_background.Finish();
  inner.children = {&second};
  RenderNode outer;
  outer.opacity = 0.65F;
  PaintBackground(outer, true);
  outer.children = {&first, &inner};
  RenderNode clipped;
  clipped.child_clips = {
    PushClipCommand{.rect = {10.0F, 8.0F, 164.0F, 76.0F}},
    PushPathClipCommand{.path = Path::RoundedRect({8.0F, 4.0F, 170.0F, 86.0F},
                                                CornerRadii{11.0F, 19.0F, 7.0F, 5.0F})},
  };
  clipped.children = {&outer};
  RenderNode root;
  PaintBackground(root);
  root.children = {&clipped};
  RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};
  detail::LinuxRenderer gpu;
  detail::LinuxRenderer cpu;
  Configure(gpu, display);
  Configure(cpu);
  const auto actual = SnapshotTexture(gpu, frame);
  REQUIRE(gpu.BackdropStats().gpu_plane_count == 2);
  REQUIRE(gpu.BackdropStats().command_count == 2);
  REQUIRE(gpu.BackdropStats().readback_bytes == 0);
  const auto expected = SnapshotTexture(cpu, frame);
  RequireMatchingPixels(Download(actual.get()), Download(expected.get()));
  gpu.Discard();
  cpu.Discard();
}

TEST_CASE("LinuxResidentBackdropPublishesImmutableTexturesAcrossSourceChangesAndRetirement", "[backdrop][gpu]") {
  GdkDisplay* display = RequireGpuDisplay();
  RenderNode control;
  control.offset = {12.0F, 12.0F};
  PaintControl(control, "Glass");
  RenderNode root;
  PaintBackground(root);
  root.children = {&control};
  RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};
  detail::LinuxRenderer gpu;
  detail::LinuxRenderer cpu;
  Configure(gpu, display);
  Configure(cpu);
  const auto first = SnapshotTexture(gpu, frame);
  REQUIRE(gpu.BackdropStats().gpu_plane_count == 1);
  REQUIRE(gpu.BackdropStats().readback_bytes == 0);
  const auto first_pixels = Download(first.get());
  const auto cached = SnapshotTexture(gpu, frame);
  REQUIRE(cached.get() == first.get());
  REQUIRE(gpu.BackdropStats().plane_count == 0);
  PaintBackground(root, true);
  ++frame.revision;
  const auto second = SnapshotTexture(gpu, frame);
  REQUIRE(second.get() != first.get());
  REQUIRE(gpu.BackdropStats().gpu_plane_count == 1);
  REQUIRE(gpu.BackdropStats().readback_bytes == 0);
  const auto second_pixels = Download(second.get());
  REQUIRE(second_pixels != first_pixels);
  const auto reference = SnapshotTexture(cpu, frame);
  RequireMatchingPixels(second_pixels, Download(reference.get()));
  REQUIRE(Download(first.get()) == first_pixels);
  root.children.clear();
  ++frame.revision;
  const auto ordinary = SnapshotNode(gpu, frame);
  REQUIRE(gpu.BackdropStats().plane_count == 0);
  REQUIRE(gsk_render_node_get_node_type(ordinary.get()) != GSK_TEXTURE_NODE);
  const auto ordinary_reference = SnapshotNode(cpu, frame);
  RequireMatchingPixels(DrawNode(ordinary, 220, 100), DrawNode(ordinary_reference, 220, 100));
  gpu.Discard();
  cpu.Discard();
  REQUIRE(Download(first.get()) == first_pixels);
  REQUIRE(Download(second.get()) == second_pixels);
}

TEST_CASE("LinuxResidentBackdropMatchesCpuUnderDirectRoundedAndPathClips", "[backdrop][gpu]") {
  GdkDisplay* display = RequireGpuDisplay();
  RenderNode control;
  control.offset = {12.0F, 12.0F};
  control.transform = {.m11 = 0.98F, .m12 = 0.14F, .m21 = -0.14F, .m22 = 0.98F};
  PaintControl(control, "Clipped");
  RenderNode clipped;
  clipped.offset = {5.0F, 3.0F};
  clipped.child_clips = {
    PushClipCommand{.rect = {14.0F, 10.0F, 40.0F, 32.0F}, .corner_radius = 5.0F},
    PushPathClipCommand{.path = Path::RoundedRect({10.0F, 6.0F, 46.0F, 38.0F},
                                                CornerRadii{7.0F, 12.0F, 4.0F, 8.0F})},
  };
  clipped.children = {&control};
  RenderNode root;
  PaintBackground(root);
  root.children = {&clipped};
  RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};
  detail::LinuxRenderer gpu;
  detail::LinuxRenderer cpu;
  Configure(gpu, display);
  Configure(cpu);
  const auto actual = SnapshotTexture(gpu, frame);
  REQUIRE(gpu.BackdropStats().gpu_plane_count == 1);
  REQUIRE(gpu.BackdropStats().readback_bytes == 0);
  const auto expected = SnapshotTexture(cpu, frame);
  RequireMatchingPixels(Download(actual.get()), Download(expected.get()));
  gpu.Discard();
  cpu.Discard();
}

TEST_CASE("LinuxResidentBackdropMatchesCpuWithANegativeCaptureOriginWithoutAViewport", "[backdrop][gpu]") {
  GdkDisplay* display = RequireGpuDisplay();
  RenderNode root;
  PaintContext background(root.content, {-28.0F, -10.0F, 120.0F, 60.0F});
  background.DrawRect({-28.0F, -10.0F, 60.0F, 60.0F}, Color::Rgb(180, 40, 80, 0.65F));
  background.DrawRect({32.0F, -10.0F, 60.0F, 60.0F}, Color::Rgb(30, 100, 210, 0.65F));
  background.Finish();
  RenderNode control;
  control.offset = {-12.0F, 0.0F};
  PaintControl(control, "Negative");
  root.children = {&control};
  RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};
  detail::LinuxRenderer gpu;
  detail::LinuxRenderer cpu;
  gpu.Initialize();
  gpu.SetDisplay(display);
  gpu.SetDeviceScale(2.0F);
  cpu.Initialize();
  cpu.SetDeviceScale(2.0F);
  const auto node = SnapshotNode(gpu, frame);
  REQUIRE(gpu.BackdropStats().gpu_plane_count == 1);
  REQUIRE(gpu.BackdropStats().readback_bytes == 0);
  REQUIRE(gsk_render_node_get_node_type(node.get()) == GSK_TEXTURE_NODE);
  graphene_rect_t bounds;
  gsk_render_node_get_bounds(node.get(), &bounds);
  REQUIRE(bounds.origin.x == -28.0F);
  REQUIRE(bounds.origin.y == -10.0F);
  auto* actual = gsk_texture_node_get_texture(node.get());
  REQUIRE(gdk_texture_get_width(actual) == 240);
  REQUIRE(gdk_texture_get_height(actual) == 120);
  const auto expected = SnapshotTexture(cpu, frame);
  REQUIRE(gdk_texture_get_width(expected.get()) == 240);
  REQUIRE(gdk_texture_get_height(expected.get()) == 120);
  RequireMatchingPixels(Download(actual), Download(expected.get()));
  gpu.Discard();
  cpu.Discard();
}

TEST_CASE("LinuxResidentBackdropSamplesCompletedOpacityGroupsBeforeTheFirstGlassPlane", "[backdrop][gpu]") {
  GdkDisplay* display = RequireGpuDisplay();
  RenderNode source;
  source.offset = {10.0F, 10.0F};
  source.opacity = 0.4F;
  const auto paint_source = [&](Color color) {
    PaintContext paint(source.content, {0.0F, 0.0F, 80.0F, 60.0F});
    paint.DrawRect({0.0F, 0.0F, 80.0F, 60.0F}, color);
    paint.DrawText({8.0F, 10.0F, 64.0F, 20.0F}, "Source", TextStyle{Font::System(13.0F), Color::White()});
    paint.Finish();
  };
  paint_source(Color::Rgb(220, 30, 40, 0.7F));
  RenderNode control;
  control.offset = {20.0F, 20.0F};
  PaintControl(control, "Glass");
  RenderNode root;
  PaintBackground(root);
  root.children = {&source, &control};
  RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};
  detail::LinuxRenderer gpu;
  detail::LinuxRenderer cpu;
  Configure(gpu, display);
  Configure(cpu);
  const auto first = SnapshotTexture(gpu, frame);
  REQUIRE(gpu.BackdropStats().gpu_plane_count == 1);
  REQUIRE(gpu.BackdropStats().readback_bytes == 0);
  const auto first_pixels = Download(first.get());
  const auto first_reference = SnapshotTexture(cpu, frame);
  RequireMatchingPixels(first_pixels, Download(first_reference.get()));
  paint_source(Color::Rgb(30, 220, 70, 0.7F));
  ++frame.revision;
  const auto second = SnapshotTexture(gpu, frame);
  REQUIRE(gpu.BackdropStats().gpu_plane_count == 1);
  REQUIRE(gpu.BackdropStats().readback_bytes == 0);
  REQUIRE(second.get() != first.get());
  const auto second_pixels = Download(second.get());
  REQUIRE(second_pixels != first_pixels);
  const auto second_reference = SnapshotTexture(cpu, frame);
  RequireMatchingPixels(second_pixels, Download(second_reference.get()));
  gpu.Discard();
  cpu.Discard();
}

TEST_CASE("LinuxResidentBackdropReusesMasksAndCropsRasterWithoutLosingTransformedInk", "[backdrop][gpu]") {
  GdkDisplay* display = RequireGpuDisplay();
  RenderNode root;
  PaintBackground(root);
  RenderNode first;
  first.offset = {20, 12};
  PaintControl(first, "First");
  RenderNode second;
  second.offset = {40, 24};
  second.transform = {.m11 = 0.98F, .m12 = 0.14F, .m21 = -0.14F, .m22 = 0.98F};
  PaintControl(second, "Second");
  RenderNode foreground;
  PaintContext ink(foreground.content, {0, 0, 220, 100});
  ink.DrawTextRun({180, 80, 1, 1}, {120, 74}, "Overhang", TextStyle{Font::System(15), Color::White()});
  Path path;
  path.MoveTo({110, 20}).LineTo({150, 60}).LineTo({180, 20});
  ink.StrokePath(path, Color::White(), StrokeStyle{.width = 4, .miter_limit = 10});
  ink.Finish();
  RenderNode clipped;
  clipped.children = {&first, &second, &foreground};
  root.children = {&clipped};
  RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true, .rects = {}}, .revision = 1};
  detail::LinuxRenderer gpu;
  detail::LinuxRenderer cpu;
  Configure(gpu, display);
  Configure(cpu);
  for (int phase = 0; phase < 4; ++phase) {
    if (phase == 1) {
      clipped.child_clips = {PushPathClipCommand{.path = Path::RoundedRect({10, 8, 190, 80}, CornerRadii{12})}};
    } else if (phase == 2) {
      clipped.child_clips = {PushPathClipCommand{.path = Path::RoundedRect({30, 15, 160, 70}, CornerRadii{20})}};
      second.opacity = 0.7F;
    } else if (phase == 3) {
      clipped.child_clips.clear();
      second.opacity = 1;
      first.offset = {-20, -6};
    }
    ++frame.revision;
    const auto actual = SnapshotTexture(gpu, frame);
    const auto stats = gpu.BackdropStats();
    REQUIRE(stats.gpu_plane_count == 2);
    REQUIRE(stats.readback_bytes == 0);
    if (phase == 0 || phase == 3) {
      REQUIRE(stats.mask_rasterizations == 0);
      REQUIRE(stats.mask_upload_bytes == 0);
    } else if (phase == 1) {
      REQUIRE(stats.mask_rasterizations == 1);
      REQUIRE(stats.mask_upload_bytes > 0);
    }
    const auto expected = SnapshotTexture(cpu, frame);
    RequireMatchingPixels(Download(actual.get()), Download(expected.get()));
  }
}

TEST_CASE("LinuxResidentBackdropCroppedSegmentsDoNotLeavePixelsAcrossPlanesOrFrames", "[backdrop][gpu]") {
  RenderNode root;
  RenderNode first;
  first.offset = {12, 12};
  PaintControl(first, "First");
  RenderNode second;
  second.offset = {40, 24};
  PaintControl(second, "Second");
  root.children = {&first, &second};
  detail::LinuxRenderer gpu;
  detail::LinuxRenderer cpu;
  Configure(gpu, RequireGpuDisplay());
  Configure(cpu);
  RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true, .rects = {}}, .revision = 1};
  for (int phase = 0; phase < 3; ++phase) {
    PaintContext paint(root.content, {0, 0, 220, 100});
    paint.DrawRect({10.0F + phase * 15, 8, 80, 50}, Color::Rgb(40, 160, 200, 0.7F));
    paint.Finish();
    ++frame.revision;
    const auto actual = SnapshotTexture(gpu, frame);
    const auto expected = SnapshotTexture(cpu, frame);
    const auto stats = gpu.BackdropStats();
    REQUIRE(stats.gpu_plane_count == 2);
    REQUIRE(stats.raster_upload_bytes < 3 * 440 * 200 * 4);
    RequireMatchingPixels(Download(actual.get()), Download(expected.get()));
  }
}

TEST_CASE("LinuxResidentBackdropReportsScrollingMultiPlaneTiming", "[backdrop][gpu][.benchmark]") {
  const int width = GENERATE(800, 1200);
  const int height = width * 3 / 4;
  const bool source_changes = GENERATE(false, true);
  detail::LinuxRenderer renderer;
  renderer.Initialize();
  renderer.SetDisplay(RequireGpuDisplay());
  renderer.SetViewport({static_cast<float>(width), static_cast<float>(height)});
  renderer.SetDeviceScale(2.0F);
  RenderNode root;
  const auto background = [&](int phase) {
    PaintContext paint(root.content, {0, 0, static_cast<float>(width), static_cast<float>(height)});
    for (int index = 0; index < 12; ++index) {
      paint.DrawRect({index * width / 12.0F, 0, width / 12.0F, static_cast<float>(height)},
                     Color::Rgb(static_cast<std::uint8_t>(index * 20), static_cast<std::uint8_t>(200 - index * 12),
                                static_cast<std::uint8_t>(80 + phase)));
    }
    paint.Finish();
  };
  background(0);
  std::vector<RenderNode> controls(8);
  RenderNode scrolling;
  for (std::size_t index = 0; index < controls.size(); ++index) {
    auto& control = controls[index];
    control.offset = {40.0F + static_cast<float>(index % 2) * 180, 32.0F + static_cast<float>(index) * 70};
    PaintContext paint(control.content, {0, 0, 280, 110});
    paint.DrawBackdrop({0, 0, 280, 110}, BackdropEffect{.blur_radius = 18, .refraction = 6, .highlight = 0.2F},
                       CornerRadii{18});
    paint.DrawRect({0, 0, 280, 110}, Color::Rgb(255, 255, 255, 0.15F), CornerRadii{18});
    paint.DrawText({16, 18, 240, 30}, "Scrolling glass " + std::to_string(index),
                   TextStyle{Font::System(18), Color::White()});
    paint.Finish();
    scrolling.children.push_back(&control);
  }
  scrolling.child_clips = {PushClipCommand{
      .rect = {20, 20, static_cast<float>(width - 40), static_cast<float>(height - 40)}, .corner_radius = 12}};
  root.children = {&scrolling};
  RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true, .rects = {}}, .revision = 0};
  std::vector<double> milliseconds;
  for (int index = 0; index < 25; ++index) {
    if (source_changes) {
      background(index % 2);
    }
    scrolling.children_transform.translate_y = -static_cast<float>(index % 8) * 3;
    ++frame.revision;
    const auto start = std::chrono::steady_clock::now();
    const auto texture = SnapshotTexture(renderer, frame);
    // Download makes the GPU finish before the next sample. This includes readback, not display presentation.
    const auto pixels = Download(texture.get());
    const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    REQUIRE_FALSE(pixels.empty());
    REQUIRE(renderer.BackdropStats().gpu_plane_count > 1);
    if (index >= 5) {
      milliseconds.push_back(elapsed);
    }
  }
  std::sort(milliseconds.begin(), milliseconds.end());
  const auto stats = renderer.BackdropStats();
  WARN("LinuxResidentBackdropScroll pixels="
       << width * 2 << "x" << height * 2 << " source_changes=" << source_changes << " p50=" << milliseconds[10]
       << "ms p95=" << milliseconds[18] << "ms planes=" << stats.gpu_plane_count
       << " upload_bytes=" << stats.raster_upload_bytes << " mask_rasterizations=" << stats.mask_rasterizations
       << " mask_upload_bytes=" << stats.mask_upload_bytes);
}

} // namespace huxerui::test
