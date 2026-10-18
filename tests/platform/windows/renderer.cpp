#include <catch2/catch_amalgamated.hpp>

#include <objbase.h>

#include <limits>
#include <vector>

#include <huxerui/paint.h>
#include <huxerui/render_scene.h>
#include <huxerui/text.h>

#include "win32_renderer.h"
#include "text/text_internal.h"

namespace huxerui::detail {

struct Win32RendererTestAccess {
  static std::vector<std::uint32_t> ReadPixels(Win32Renderer& renderer) {
    return renderer.ReadPixels();
  }
};

} // namespace huxerui::detail

namespace huxerui::test {

namespace {

struct RendererComScope {
  RendererComScope() {
    REQUIRE(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)));
  }
  ~RendererComScope() {
    CoUninitialize();
  }
};

struct RendererWindow {
  HWND handle = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 128, 128, nullptr, nullptr, nullptr, nullptr);
  ~RendererWindow() {
    if (handle) {
      DestroyWindow(handle);
    }
  }
};

struct PaintFixture {
  RendererComScope com;
  RendererWindow window;
  detail::Win32Renderer renderer;
  float dpi = 96.0F;

  PaintFixture() {
    REQUIRE(window.handle != nullptr);
    renderer.Initialize();
    renderer.Resize(window.handle, dpi);
  }

  std::vector<std::uint32_t> Draw(const RenderNode& root) {
    RECT bounds{};
    REQUIRE(GetClientRect(window.handle, &bounds));
    const RenderFrame frame{.scene = {.root = &root}, .damage = {.full = true}, .revision = 1};
    REQUIRE(renderer.Render(window.handle, dpi, frame, bounds) == detail::Win32RenderResult::Presented);
    auto pixels = detail::Win32RendererTestAccess::ReadPixels(renderer);
    REQUIRE(pixels.size() == static_cast<std::size_t>(bounds.right) * bounds.bottom);
    return pixels;
  }
};

std::vector<GradientStop> RedBlueStops() {
  return {{0.0F, Color::Rgb(255, 0, 0)}, {1.0F, Color::Rgb(0, 0, 255)}};
}

void RecordGradients(RenderNode& root, bool reversed) {
  PaintContext paint(root.content, {0.0F, 0.0F, 128.0F, 128.0F});
  const auto stops = RedBlueStops();
  paint.DrawRect({0.0F, 0.0F, 64.0F, 32.0F}, LinearGradient{.start = reversed ? Point{1.0F, 0.0F} : Point{0.0F, 0.0F},
                                                            .end = reversed ? Point{0.0F, 0.0F} : Point{1.0F, 0.0F},
                                                            .stops = stops});
  paint.DrawRect({64.0F, 0.0F, 64.0F, 32.0F},
                 LinearGradient{.start = {1.0F, 0.0F}, .end = {0.0F, 0.0F}, .stops = stops});
  paint.DrawRect({0.0F, 32.0F, 64.0F, 64.0F}, RadialGradient{.center = {0.25F, 0.5F},
                                                             .radius = reversed ? Size{0.25F, 0.5F} : Size{0.5F, 0.25F},
                                                             .stops = stops});
  paint.DrawRect({64.0F, 32.0F, 64.0F, 64.0F}, RadialGradient{.center = {0.75F, 0.5F},
                                                              .radius = {0.25F, 0.5F},
                                                              .stops = stops,
                                                              .transform = {.translate_x = reversed ? -0.25F : 0.0F}});
  paint.Finish();
}

void RecordShadow(RenderNode& root, PathFillRule fill_rule, Color color, Point offset, float blur_radius) {
  Path path = Path::RoundedRect({24.0F, 24.0F, 48.0F, 48.0F}, CornerRadii{4.0F});
  // Identical contour direction distinguishes non-zero filling from an even-odd hole.
  path.MoveTo({36.0F, 36.0F}).LineTo({60.0F, 36.0F}).LineTo({60.0F, 60.0F}).LineTo({36.0F, 60.0F}).Close();
  PaintContext paint(root.content, {0.0F, 0.0F, 128.0F, 128.0F});
  paint.DrawRect({0.0F, 0.0F, 128.0F, 128.0F}, Color::White());
  paint.DrawPathShadow(path, color, offset, blur_radius, fill_rule);
  paint.Finish();
}

} // namespace

TEST_CASE("Win32GradientReusePreservesIndependentSamplingAndDeviceRecreation", "[paint-resources]") {
  PaintFixture fixture;
  RenderNode root;
  RecordGradients(root, false);
  const auto initial = fixture.Draw(root);
  REQUIRE(fixture.Draw(root) == initial);
  const auto red = [](std::uint32_t pixel) { return (pixel >> 16U) & 255U; };
  const auto blue = [](std::uint32_t pixel) { return pixel & 255U; };
  REQUIRE(red(initial[8 * 128 + 8]) > blue(initial[8 * 128 + 8]));
  REQUIRE(blue(initial[8 * 128 + 56]) > red(initial[8 * 128 + 56]));
  REQUIRE(blue(initial[8 * 128 + 72]) > red(initial[8 * 128 + 72]));
  REQUIRE(red(initial[64 * 128 + 16]) > blue(initial[64 * 128 + 16]));
  REQUIRE(red(initial[64 * 128 + 112]) > blue(initial[64 * 128 + 112]));

  RecordGradients(root, true);
  const auto changed = fixture.Draw(root);
  REQUIRE(changed != initial);
  fixture.renderer.ResetDeviceResources();
  REQUIRE(fixture.Draw(root) == changed);
  fixture.dpi = 144.0F;
  fixture.renderer.DpiChanged(fixture.window.handle, fixture.dpi);
  const auto scaled = fixture.Draw(root);
  fixture.renderer.ResetDeviceResources();
  REQUIRE(fixture.Draw(root) == scaled);
}

TEST_CASE("Win32PathShadowReusePreservesColorOffsetFillRuleAndDpi", "[paint-resources]") {
  PaintFixture fixture;
  RenderNode root;
  RecordShadow(root, PathFillRule::NonZero, Color::Black(), {}, 12.0F);
  const auto initial = fixture.Draw(root);
  REQUIRE(fixture.Draw(root) == initial);
  REQUIRE(initial[48 * 128 + 48] == 0xFFFFFFFFU);
  REQUIRE(initial[48 * 128 + 22] != 0xFFFFFFFFU);
  REQUIRE(initial[0] == 0xFFFFFFFFU);

  RecordShadow(root, PathFillRule::NonZero, Color::Rgb(255, 0, 0, 0.5F), {12.0F, 8.0F}, 12.0F);
  const auto moved = fixture.Draw(root);
  REQUIRE(moved != initial);
  fixture.renderer.ResetDeviceResources();
  REQUIRE(fixture.Draw(root) == moved);
  RecordShadow(root, PathFillRule::EvenOdd, Color::Black(), {}, 12.0F);
  const auto with_hole = fixture.Draw(root);
  REQUIRE(with_hole != initial);
  REQUIRE(with_hole[38 * 128 + 48] != 0xFFFFFFFFU);
  fixture.renderer.ResetDeviceResources();
  REQUIRE(fixture.Draw(root) == with_hole);
  fixture.dpi = 144.0F;
  fixture.renderer.DpiChanged(fixture.window.handle, fixture.dpi);
  const auto scaled = fixture.Draw(root);
  fixture.renderer.ResetDeviceResources();
  REQUIRE(fixture.Draw(root) == scaled);
}

TEST_CASE("Win32PaintResourceEvictionPreservesTheOriginalScene", "[paint-resources]") {
  PaintFixture fixture;
  RenderNode original;
  RecordGradients(original, false);
  const auto gradients = fixture.Draw(original);
  RenderNode shadow;
  RecordShadow(shadow, PathFillRule::EvenOdd, Color::Black(), {}, 12.0F);
  const auto shadows = fixture.Draw(shadow);
  RenderNode churn;
  PaintContext paint(churn.content, {0.0F, 0.0F, 128.0F, 128.0F});
  for (int index = 0; index < 80; ++index) {
    paint.DrawRect({0.0F, 0.0F, 16.0F, 16.0F},
                   LinearGradient{.stops = {{0.0F, Color::Rgb(index * 3, 0, 0)}, {1.0F, Color::White()}}});
    const auto path = Path::RoundedRect({24.0F, 24.0F, 32.0F, 32.0F}, CornerRadii{index * 0.1F});
    paint.DrawPathShadow(path, Color::Black(), {}, 8.0F);
  }
  paint.Finish();
  fixture.Draw(churn);
  REQUIRE(fixture.Draw(original) == gradients);
  REQUIRE(fixture.Draw(shadow) == shadows);
}

TEST_CASE("Win32OversizedPaintResourcesStillRender", "[paint-resources]") {
  PaintFixture fixture;
  RenderNode root;
  LinearGradient gradient;
  for (int index = 0; index <= 4096; ++index) {
    gradient.stops.push_back({index / 4096.0F, Color::Rgb(255, 0, 0)});
  }
  Path path;
  path.MoveTo({24.0F, 24.0F});
  for (int index = 0; index < 32768; ++index) {
    path.LineTo({72.0F, 24.0F});
  }
  path.LineTo({72.0F, 72.0F}).LineTo({24.0F, 72.0F}).Close();
  PaintContext paint(root.content, {0.0F, 0.0F, 128.0F, 128.0F});
  paint.DrawRect({0.0F, 0.0F, 128.0F, 128.0F}, Color::White());
  paint.DrawRect({0.0F, 0.0F, 16.0F, 16.0F}, gradient);
  paint.DrawPathShadow(path, Color::Black(), {}, 12.0F);
  paint.Finish();
  const auto pixels = fixture.Draw(root);
  REQUIRE(pixels[8 * 128 + 8] == 0xFFFF0000U);
  REQUIRE(pixels[48 * 128 + 22] != 0xFFFFFFFFU);
  REQUIRE(pixels[48 * 128 + 48] == 0xFFFFFFFFU);
  REQUIRE(fixture.Draw(root) == pixels);
}

TEST_CASE("Win32CachedGradientsPreserveAncestorClippingOpacityAndResize", "[paint-resources]") {
  PaintFixture fixture;
  RenderNode child;
  RecordGradients(child, false);
  RenderNode parent;
  parent.children = {&child};
  parent.opacity = 0.6F;
  parent.transform = {.m11 = 0.8F, .m22 = 1.1F, .translate_x = 5.0F, .translate_y = 7.0F};
  parent.child_clips.push_back(PushClipCommand{{8.0F, 12.0F, 100.0F, 80.0F}, 6.0F});
  const auto initial = fixture.Draw(parent);
  REQUIRE(fixture.Draw(parent) == initial);
  fixture.renderer.ResetDeviceResources();
  REQUIRE(fixture.Draw(parent) == initial);
  parent.children_transform = {.translate_x = 12.0F, .translate_y = -4.0F};
  const auto moved = fixture.Draw(parent);
  REQUIRE(moved != initial);
  fixture.renderer.ResetDeviceResources();
  REQUIRE(fixture.Draw(parent) == moved);
  REQUIRE(SetWindowPos(fixture.window.handle, nullptr, 0, 0, 160, 96, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
  fixture.renderer.Resize(fixture.window.handle, fixture.dpi);
  const auto resized = fixture.Draw(parent);
  REQUIRE(resized.size() == 160 * 96);
  fixture.renderer.ResetDeviceResources();
  REQUIRE(fixture.Draw(parent) == resized);
}

TEST_CASE("Win32PathShadowBlurChangesDoNotReuseAnIncompatibleMask", "[paint-resources]") {
  PaintFixture fixture;
  RenderNode root;
  RecordShadow(root, PathFillRule::NonZero, Color::Black(), {}, 12.0F);
  const auto wide = fixture.Draw(root);
  RecordShadow(root, PathFillRule::NonZero, Color::Black(), {}, 6.0F);
  const auto narrow = fixture.Draw(root);
  REQUIRE(narrow != wide);
  fixture.renderer.ResetDeviceResources();
  REQUIRE(fixture.Draw(root) == narrow);
  RecordShadow(root, PathFillRule::NonZero, Color::Black(), {}, 0.0F);
  const auto solid = fixture.Draw(root);
  REQUIRE(solid[48 * 128 + 48] == 0xFF000000U);
  RecordShadow(root, PathFillRule::NonZero, Color::Transparent(), {}, 12.0F);
  const auto transparent = fixture.Draw(root);
  REQUIRE(transparent[48 * 128 + 22] == 0xFFFFFFFFU);
  REQUIRE(transparent[48 * 128 + 48] == 0xFFFFFFFFU);
}

TEST_CASE("Win32UnboundedTextMeasurementIgnoresParagraphAlignment") {
  const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  REQUIRE(SUCCEEDED(com_result));
  detail::Win32Renderer renderer;
  renderer.Initialize();

  const TextStyle style{Font::System(14.0F), Color::Black()};
  const float unbounded = std::numeric_limits<float>::infinity();
  const TextLayoutMetrics leading =
      renderer.MeasureText("Button", style, unbounded, {.align = TextAlign::Leading, .wrap = TextWrap::NoWrap});
  const TextLayoutMetrics centered =
      renderer.MeasureText("Button", style, unbounded, {.align = TextAlign::Center, .wrap = TextWrap::NoWrap});
  const TextLayoutMetrics trailing =
      renderer.MeasureText("Button", style, unbounded, {.align = TextAlign::Trailing, .wrap = TextWrap::NoWrap});

  REQUIRE(leading.size.width > 0.0F);
  REQUIRE(centered.size == leading.size);
  REQUIRE(trailing.size == leading.size);
  renderer.Discard();
  CoUninitialize();
}

TEST_CASE("Win32AttributedParagraphSharesNativeMeasurementAndSelectionMetrics") {
  const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  REQUIRE(SUCCEEDED(com_result));
  detail::Win32Renderer renderer;
  renderer.Initialize();
  const TextStyle style{Font::System(14.0F), Color::Black()};
  const AttributedText text{
    TextSpan("small "),
    TextSpan("LARGE").Style({.font_size = 36.0F, .font_weight = FontWeight::Bold}),
    TextSpan(" small"),
  };
  const auto metrics = renderer.MeasureText(text, style, 240.0F, {});
  const auto plain = renderer.MeasureText(text.PlainText(), style, 240.0F, {});
  const auto layout = renderer.CreateTextLayout(text, style, 240.0F, {});
  REQUIRE(metrics.size.height > plain.size.height);
  REQUIRE(layout->Measure().height == metrics.size.height);
  const auto ranges = layout->RangeRects({6, 11});
  REQUIRE_FALSE(ranges.empty());
  REQUIRE(ranges.front().width > 70.0F);
  const auto caret = layout->CaretRect(8, TextAffinity::Downstream);
  REQUIRE(layout->HitTest({caret.x, caret.y + caret.height * 0.5F}).offset == 8);
  const auto wrapped = renderer.MeasureText(text, style, 85.0F, {.wrap = TextWrap::Word});
  REQUIRE(wrapped.line_count > metrics.line_count);
  const auto combining = renderer.CreateTextLayout(
      AttributedText::FromRanges("a\u0301b", {{{1, 2}, {.foreground = Color::White()}}}), style, 200.0F, {});
  REQUIRE(combining->NextCaretOffset(0) == 2);
  REQUIRE(combining->PreviousCaretOffset(2) == 0);
  const auto styled_combining = renderer.CreateTextLayout(
      AttributedText::FromRanges("a\u0301b", {{{1, 2}, {.font_size = 24.0F}}}), style, 200.0F, {});
  REQUIRE(styled_combining->NextCaretOffset(0) == 2);
  REQUIRE(styled_combining->PreviousCaretOffset(2) == 0);
  const auto emoji = renderer.CreateTextLayout(
      AttributedText::FromRanges("\U0001F469\u200D\U0001F4BB!", {{{2, 3}, {.font_size = 24.0F}}}), style, 200.0F, {});
  REQUIRE(emoji->NextCaretOffset(0) == 5);
  REQUIRE(emoji->PreviousCaretOffset(5) == 0);
  renderer.Discard();
  CoUninitialize();
}

} // namespace huxerui::test
