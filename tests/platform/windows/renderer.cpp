#include <catch2/catch_amalgamated.hpp>

#include <objbase.h>

#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>

#include <huxerui/paint.h>
#include <huxerui/render_scene.h>
#include <huxerui/text.h>

#include "win32_renderer.h"
#include "text/text_internal.h"

namespace huxerui::test {

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

TEST_CASE("Win32BackdropRendersNestedClipsAndOpacityGroupsAfterDeviceReset") {
  struct ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComScope() {
      if (SUCCEEDED(result)) {
        CoUninitialize();
      }
    }
  } com;
  REQUIRE((SUCCEEDED(com.result) || com.result == RPC_E_CHANGED_MODE));
  const auto destroy_window = [](HWND window) { DestroyWindow(window); };
  const std::unique_ptr<std::remove_pointer_t<HWND>, decltype(destroy_window)> window(
      CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr), destroy_window
  );
  REQUIRE(window != nullptr);

  RenderNode root;
  PaintContext background(root.content, {0.0F, 0.0F, 64.0F, 64.0F});
  background.DrawRect({0.0F, 0.0F, 32.0F, 64.0F}, Color::Rgb(255, 0, 0));
  background.DrawRect({32.0F, 0.0F, 32.0F, 64.0F}, Color::Rgb(0, 0, 255));
  background.DrawBackdrop({8.0F, 8.0F, 48.0F, 48.0F}, {.blur_radius = 6.0F, .refraction = 2.0F}, {8.0F});
  background.Finish();
  root.child_clips = {PushClipCommand{{0.0F, 0.0F, 60.0F, 60.0F}, 6.0F}};
  RenderNode group;
  group.opacity = 0.5F;
  group.offset = {2.0F, 3.0F};
  PaintContext group_paint(group.content, {0.0F, 0.0F, 60.0F, 60.0F});
  group_paint.DrawRect({0.0F, 0.0F, 60.0F, 60.0F}, Color::Rgb(255, 255, 255, 0.4F));
  group_paint.PushClip({4.0F, 4.0F, 48.0F, 48.0F});
  Path path;
  path.MoveTo({8.0F, 8.0F}).LineTo({52.0F, 8.0F}).LineTo({30.0F, 52.0F}).Close();
  group_paint.PushPathClip(path);
  group_paint.DrawBackdrop({4.0F, 4.0F, 48.0F, 48.0F},
      {.blur_radius = 4.0F, .refraction = 1.0F, .highlight = 0.2F}, {8.0F});
  group_paint.PopClip();
  group_paint.PopClip();
  group_paint.DrawRect({20.0F, 20.0F, 12.0F, 12.0F}, Color::Rgb(0, 255, 0));
  group_paint.Finish();
  root.children = {&group};
  const RenderFrame frame{.scene = {&root}, .damage = {.full = true}};
  const RECT paint_rect{0, 0, 64, 64};
  detail::Win32Renderer renderer;
  renderer.Initialize();
  REQUIRE(renderer.Render(window.get(), 96.0F, frame, paint_rect) == detail::Win32RenderResult::Presented);
  renderer.ResetDeviceResources();
  REQUIRE(renderer.Render(window.get(), 96.0F, frame, paint_rect) == detail::Win32RenderResult::Presented);
  const RECT partial_rect{20, 20, 21, 21};
  REQUIRE(renderer.Render(window.get(), 96.0F, frame, partial_rect) == detail::Win32RenderResult::Presented);
  renderer.Discard();
}

TEST_CASE("Win32BackdropRejectsOversizedViewportBeforeAllocatingRenderTargets") {
  const auto destroy_window = [](HWND window) { DestroyWindow(window); };
  const std::unique_ptr<std::remove_pointer_t<HWND>, decltype(destroy_window)> window(
      CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 5000, 5000, nullptr, nullptr, nullptr, nullptr), destroy_window
  );
  REQUIRE(window != nullptr);
  RenderNode root;
  PaintContext paint(root.content, {0.0F, 0.0F, 5000.0F, 5000.0F});
  paint.DrawBackdrop({0.0F, 0.0F, 20.0F, 20.0F}, {.blur_radius = 0.0F});
  paint.Finish();
  const RenderFrame frame{.scene = {&root}, .damage = {.full = true}};
  detail::Win32Renderer renderer;
  REQUIRE_THROWS_AS(renderer.Render(window.get(), 96.0F, frame, {0, 0, 5000, 5000}), std::runtime_error);
}

} // namespace huxerui::test
