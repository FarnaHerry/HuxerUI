#include <catch2/catch_amalgamated.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>

#include <emscripten/val.h>

#include <huxerui/paint.h>

#include "web_renderer.h"

namespace huxerui::test {
namespace {

emscripten::val RendererCanvas() {
  const auto document = emscripten::val::global("document");
  if (document.isUndefined() || document.isNull()) {
    SKIP("Web renderer pixel tests require a browser Canvas 2D context");
  }
  auto canvas = document.call<emscripten::val>("createElement", std::string("canvas"));
  canvas.set("width", 16);
  canvas.set("height", 16);
  return canvas;
}

std::uint32_t Pixel(const emscripten::val& canvas, int x, int y) {
  const auto data = canvas.call<emscripten::val>("getContext", std::string("2d"))
                        .call<emscripten::val>("getImageData", x, y, 1, 1)["data"];
  return data[0].as<unsigned>() << 24U | data[1].as<unsigned>() << 16U | data[2].as<unsigned>() << 8U |
         data[3].as<unsigned>();
}

TEST_CASE("WebBackdropSamplesCurrentFrameBeforeForeground") {
  const auto canvas = RendererCanvas();
  detail::WebRenderer renderer(0, canvas);
  renderer.SetViewport({16.0F, 16.0F}, 1.0F);
  RenderNode root;
  const auto record = [&](bool reverse) {
    PaintContext paint(root.content, {0.0F, 0.0F, 16.0F, 16.0F});
    paint.DrawRect({0.0F, 0.0F, 8.0F, 16.0F}, reverse ? Color::White() : Color::Black());
    paint.DrawRect({8.0F, 0.0F, 8.0F, 16.0F}, reverse ? Color::Black() : Color::White());
    paint.DrawBackdrop({2.0F, 2.0F, 12.0F, 12.0F}, {.blur_radius = 3.0F});
    paint.DrawRect({7.0F, 7.0F, 2.0F, 2.0F}, Color::Rgb(255, 0, 0));
    paint.Finish();
  };
  record(false);
  renderer.Draw({.scene = {.root = &root}, .damage = {.full = true}});
  const auto first = Pixel(canvas, 7, 5);
  REQUIRE((first >> 24U) > 0);
  REQUIRE((first >> 24U) < 255);
  REQUIRE(Pixel(canvas, 7, 7) == 0xFF0000FFU);
  record(true);
  renderer.Draw({.scene = {.root = &root}, .damage = {.rects = {{0.0F, 0.0F, 1.0F, 1.0F}}}});
  REQUIRE((Pixel(canvas, 7, 5) >> 24U) > (first >> 24U));
  REQUIRE(Pixel(canvas, 0, 0) == 0xFFFFFFFFU);
}

TEST_CASE("WebBackdropKeepsOpacityGroupsIsolatedAndPreservesAlpha") {
  const auto canvas = RendererCanvas();
  detail::WebRenderer renderer(0, canvas);
  renderer.SetViewport({16.0F, 16.0F}, 1.0F);
  RenderNode root;
  PaintContext background(root.content, {0.0F, 0.0F, 16.0F, 16.0F});
  background.DrawRect({0.0F, 0.0F, 16.0F, 16.0F}, Color::Black());
  background.Finish();
  RenderNode group;
  group.opacity = 0.5F;
  PaintContext paint(group.content, {0.0F, 0.0F, 16.0F, 16.0F});
  paint.DrawRect({0.0F, 0.0F, 16.0F, 16.0F}, Color::Rgb(255, 0, 0, 0.25F));
  paint.DrawBackdrop({0.0F, 0.0F, 16.0F, 16.0F}, {.blur_radius = 0.0F});
  paint.DrawBackdrop({0.0F, 0.0F, 16.0F, 16.0F}, {.blur_radius = 0.0F});
  paint.Finish();
  root.children = {&group};
  renderer.Draw({.scene = {.root = &root}, .damage = {.full = true}});
  const unsigned red = Pixel(canvas, 8, 8) >> 24U;
  REQUIRE(red >= 30);
  REQUIRE(red <= 33);
}

TEST_CASE("WebBackdropReportsUnreadableCanvasInsteadOfUsingStalePixels") {
  const auto canvas = RendererCanvas();
  detail::WebRenderer renderer(0, canvas);
  renderer.SetViewport({16.0F, 16.0F}, 1.0F);
  RenderNode root;
  PaintContext paint(root.content, {0.0F, 0.0F, 16.0F, 16.0F});
  paint.DrawBackdrop({0.0F, 0.0F, 16.0F, 16.0F}, {.blur_radius = 1.0F});
  paint.Finish();
  const auto context = canvas.call<emscripten::val>("getContext", std::string("2d"));
  const auto original = context["getImageData"];
  context.set("getImageData",
              emscripten::val::global("Function")
                  .new_(std::string("throw new DOMException('Origin is not clean', 'SecurityError');")));
  REQUIRE_THROWS_AS(renderer.Draw({.scene = {.root = &root}, .damage = {.full = true}}), std::runtime_error);
  context.set("getImageData", original);
  REQUIRE_NOTHROW(renderer.Draw({.scene = {.root = &root}, .damage = {.full = true}}));
}

TEST_CASE("WebBackdropOpacityGroupsDeferInheritedClipsUntilComposition") {
  const auto canvas = RendererCanvas();
  detail::WebRenderer renderer(0, canvas);
  renderer.SetViewport({16.0F, 16.0F}, 1.0F);
  RenderNode root;
  PaintContext background(root.content, {0.0F, 0.0F, 16.0F, 16.0F});
  background.DrawRect({0.0F, 0.0F, 16.0F, 16.0F}, Color::Black());
  background.Finish();
  root.child_clips = {PushClipCommand{{8.0F, 0.0F, 8.0F, 16.0F}, 0.0F}};
  RenderNode group;
  group.opacity = 0.5F;
  PaintContext paint(group.content, {0.0F, 0.0F, 16.0F, 16.0F});
  paint.DrawRect({0.0F, 0.0F, 8.0F, 16.0F}, Color::White());
  paint.DrawRect({8.0F, 0.0F, 8.0F, 16.0F}, Color::Black());
  paint.DrawBackdrop({4.0F, 0.0F, 8.0F, 16.0F}, {.blur_radius = 3.0F});
  paint.Finish();
  root.children = {&group};
  renderer.Draw({.scene = {.root = &root}, .damage = {.full = true}});
  REQUIRE(Pixel(canvas, 7, 8) == 0x000000FFU);
  REQUIRE((Pixel(canvas, 8, 8) >> 24U) > 0);
}

TEST_CASE("WebBackdropCropPreservesPixelsOutsideItsReplacementBounds") {
  const auto canvas = RendererCanvas();
  canvas.set("width", 64);
  canvas.set("height", 64);
  detail::WebRenderer renderer(0, canvas);
  renderer.SetViewport({64.0F, 64.0F}, 1.0F);
  RenderNode root;
  PaintContext paint(root.content, {0.0F, 0.0F, 64.0F, 64.0F});
  paint.DrawRect({0.0F, 0.0F, 64.0F, 64.0F}, Color::White());
  paint.DrawRect({0.0F, 0.0F, 8.0F, 8.0F}, Color::Rgb(0, 0, 255));
  paint.DrawBackdrop({24.0F, 24.0F, 16.0F, 16.0F}, {.blur_radius = 0.0F, .brightness = 0.5F});
  paint.Finish();
  renderer.Draw({.scene = {.root = &root}, .damage = {.full = true}});
  REQUIRE(Pixel(canvas, 0, 0) == 0x0000FFFFU);
  REQUIRE(Pixel(canvas, 63, 63) == 0xFFFFFFFFU);
  const unsigned center = Pixel(canvas, 32, 32) >> 24U;
  REQUIRE(center >= 127);
  REQUIRE(center <= 128);
}

TEST_CASE("WebBackdropRejectsEveryIncompleteSceneSlice") {
  const auto canvas = RendererCanvas();
  detail::WebRenderer renderer(0, canvas);
  renderer.SetViewport({16.0F, 16.0F}, 1.0F);
  RenderNode root;
  PaintContext paint(root.content, {0.0F, 0.0F, 16.0F, 16.0F});
  paint.DrawRect({0.0F, 0.0F, 16.0F, 16.0F}, Color::White());
  paint.DrawBackdrop({2.0F, 2.0F, 12.0F, 12.0F}, {.blur_radius = 2.0F});
  paint.DrawRect({7.0F, 7.0F, 2.0F, 2.0F}, Color::Black());
  paint.Finish();
  RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}};
  REQUIRE_THROWS_AS(renderer.DrawSlice(canvas, frame, 0, 0, true, true), std::logic_error);
  REQUIRE_THROWS_AS(renderer.DrawSlice(canvas, frame, 0, 1, true, true), std::logic_error);
  REQUIRE_THROWS_AS(renderer.DrawSlice(canvas, frame, 1, 3, true, true), std::logic_error);
  REQUIRE_NOTHROW(renderer.DrawSlice(canvas, frame, 0, 3, true, true));
}

} // namespace
} // namespace huxerui::test
