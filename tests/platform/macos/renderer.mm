#include <catch2/catch_amalgamated.hpp>

#include <CoreGraphics/CoreGraphics.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include <huxerui/paint.h>
#include <huxerui/render_scene.h>

#include "appkit_renderer.h"

namespace huxerui::test {
namespace {

class BackdropTestBitmap {
public:
  explicit BackdropTestBitmap(std::size_t scale = 1)
      : width_(16 * scale), height_(8 * scale), pixels_(width_ * height_ * 4) {
    CGColorSpaceRef color_space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    context_ = CGBitmapContextCreate(pixels_.data(), width_, height_, 8, width_ * 4, color_space,
        static_cast<CGBitmapInfo>(static_cast<std::uint32_t>(kCGImageAlphaPremultipliedFirst) |
                                 static_cast<std::uint32_t>(kCGBitmapByteOrder32Little)));
    CGColorSpaceRelease(color_space);
    if (context_ == nullptr) {
      throw std::runtime_error("HuxerUI test could not allocate a backdrop bitmap");
    }
    CGContextTranslateCTM(context_, 0.0, height_);
    CGContextScaleCTM(context_, scale, -static_cast<CGFloat>(scale));
  }

  ~BackdropTestBitmap() {
    CGContextRelease(context_);
  }

  BackdropTestBitmap(const BackdropTestBitmap&) = delete;
  BackdropTestBitmap& operator=(const BackdropTestBitmap&) = delete;

  void Draw(const RenderNode& root, CGRect dirty_rect = CGRectMake(0.0, 0.0, 16.0, 8.0),
      CGRect viewport = CGRectMake(0.0, 0.0, 16.0, 8.0)) {
    detail::AppKitRenderer renderer;
    const RenderFrame frame{.scene = {&root}, .damage = {.full = true}};
    renderer.Draw(context_, dirty_rect, &frame, viewport);
    CGContextFlush(context_);
  }

  void DrawSlice(const RenderNode& root, std::size_t first, std::size_t count) {
    detail::AppKitRenderer renderer;
    const RenderFrame frame{.scene = {&root}, .damage = {.full = true}};
    renderer.DrawSlice(context_, CGRectMake(0.0, 0.0, 16.0, 8.0), &frame, first, count, true,
        CGRectMake(0.0, 0.0, 16.0, 8.0));
  }

  std::array<std::uint8_t, 4> Pixel(std::size_t x, std::size_t y) const {
    const std::size_t offset = (y * width_ + x) * 4;
    return {pixels_[offset], pixels_[offset + 1], pixels_[offset + 2], pixels_[offset + 3]};
  }

private:
  std::size_t width_;
  std::size_t height_;
  std::vector<std::uint8_t> pixels_;
  CGContextRef context_ = nullptr;
};

void RecordSplitBackground(PaintSequence& sequence) {
  PaintContext paint(sequence, {0.0F, 0.0F, 16.0F, 8.0F});
  paint.DrawRect({0.0F, 0.0F, 8.0F, 8.0F}, Color::Rgb(255, 0, 0));
  paint.DrawRect({8.0F, 0.0F, 8.0F, 8.0F}, Color::Rgb(0, 0, 255));
  paint.DrawBackdrop({4.0F, 0.0F, 8.0F, 8.0F}, {.blur_radius = 6.0F});
  paint.DrawRect({7.0F, 3.0F, 2.0F, 2.0F}, Color::Rgb(0, 255, 0));
  paint.Finish();
}

} // namespace

TEST_CASE("MacBackdropSamplesOnlyItsPrefixAndLeavesLaterContentSharp") {
  RenderNode root;
  RecordSplitBackground(root.content);
  BackdropTestBitmap bitmap;
  bitmap.Draw(root);
  REQUIRE(bitmap.Pixel(2, 4) == std::array<std::uint8_t, 4>{0, 0, 255, 255});
  const auto blurred = bitmap.Pixel(6, 4);
  REQUIRE(blurred[0] > 0);
  REQUIRE(blurred[2] > 0);
  REQUIRE(blurred[1] == 0);
  REQUIRE(bitmap.Pixel(8, 4) == std::array<std::uint8_t, 4>{0, 255, 0, 255});
}

TEST_CASE("MacBackdropPartialRedrawCapturesTheCompleteViewportAtRetinaScale") {
  RenderNode root;
  RecordSplitBackground(root.content);
  BackdropTestBitmap full(2);
  BackdropTestBitmap partial(2);
  full.Draw(root);
  partial.Draw(root, CGRectMake(6.0, 4.0, 1.0, 1.0));
  REQUIRE(partial.Pixel(12, 8) == full.Pixel(12, 8));
  REQUIRE(partial.Pixel(0, 0) == std::array<std::uint8_t, 4>{0, 0, 0, 0});
}

TEST_CASE("MacBackdropReplacesTransparentGroupPixelsWithoutApplyingAlphaTwice") {
  RenderNode root;
  PaintContext root_paint(root.content, {0.0F, 0.0F, 16.0F, 8.0F});
  root_paint.DrawRect({0.0F, 0.0F, 16.0F, 8.0F}, Color::Rgb(0, 0, 255));
  root_paint.Finish();
  RenderNode group;
  group.opacity = 0.5F;
  PaintContext group_paint(group.content, {0.0F, 0.0F, 16.0F, 8.0F});
  group_paint.DrawRect({0.0F, 0.0F, 16.0F, 8.0F}, Color::Rgb(255, 0, 0, 0.4F));
  group_paint.DrawBackdrop({0.0F, 0.0F, 16.0F, 8.0F}, {.blur_radius = 0.0F});
  group_paint.Finish();
  root.children = {&group};
  BackdropTestBitmap bitmap;
  bitmap.Draw(root);
  const auto pixel = bitmap.Pixel(8, 4);
  REQUIRE(pixel[0] >= 202);
  REQUIRE(pixel[0] <= 206);
  REQUIRE(pixel[2] >= 49);
  REQUIRE(pixel[2] <= 53);
  REQUIRE(pixel[3] == 255);
}

TEST_CASE("MacBackdropRespectsTransformedClipsAndEmptyGroupIsolation") {
  RenderNode root;
  PaintContext root_paint(root.content, {0.0F, 0.0F, 16.0F, 8.0F});
  root_paint.DrawRect({0.0F, 0.0F, 8.0F, 8.0F}, Color::Rgb(255, 0, 0));
  root_paint.DrawRect({8.0F, 0.0F, 8.0F, 8.0F}, Color::Rgb(0, 0, 255));
  root_paint.Finish();
  RenderNode glass;
  glass.offset = {4.0F, 0.0F};
  PaintContext glass_paint(glass.content, {0.0F, 0.0F, 12.0F, 8.0F});
  glass_paint.PushClip({0.0F, 0.0F, 4.0F, 8.0F});
  glass_paint.DrawBackdrop({0.0F, 0.0F, 12.0F, 8.0F}, {.blur_radius = 6.0F, .refraction = 2.0F});
  glass_paint.PopClip();
  glass_paint.Finish();
  root.children = {&glass};
  BackdropTestBitmap bitmap;
  bitmap.Draw(root);
  REQUIRE(bitmap.Pixel(9, 4) == std::array<std::uint8_t, 4>{255, 0, 0, 255});
  REQUIRE(bitmap.Pixel(6, 4)[0] > 0);

  glass.opacity = 0.5F;
  BackdropTestBitmap isolated;
  isolated.Draw(root);
  REQUIRE(isolated.Pixel(6, 4) == std::array<std::uint8_t, 4>{0, 0, 255, 255});
}

TEST_CASE("MacBackdropOpacityDefersInheritedClipsUntilGroupComposite") {
  RenderNode root;
  PaintContext root_paint(root.content, {0.0F, 0.0F, 16.0F, 8.0F});
  root_paint.DrawRect({0.0F, 0.0F, 16.0F, 8.0F}, Color::Rgb(0, 0, 255));
  root_paint.Finish();
  RenderNode group;
  group.opacity = 0.5F;
  PaintContext group_paint(group.content, {0.0F, 0.0F, 16.0F, 8.0F});
  group_paint.DrawRect({0.0F, 0.0F, 8.0F, 8.0F}, Color::Rgb(255, 0, 0));
  group_paint.DrawRect({8.0F, 0.0F, 8.0F, 8.0F}, Color::Black());
  group_paint.DrawBackdrop({4.0F, 0.0F, 8.0F, 8.0F}, {.blur_radius = 6.0F});
  group_paint.Finish();
  root.children = {&group};
  BackdropTestBitmap full;
  full.Draw(root);
  root.child_clips = {PushClipCommand{{8.0F, 0.0F, 4.0F, 8.0F}, {}}};
  BackdropTestBitmap clipped;
  clipped.Draw(root);
  REQUIRE(clipped.Pixel(8, 4) == full.Pixel(8, 4));
  REQUIRE(clipped.Pixel(8, 4)[2] > 0);
  REQUIRE(clipped.Pixel(6, 4) == std::array<std::uint8_t, 4>{255, 0, 0, 255});
}

TEST_CASE("MacBackdropRejectsMissingPrefixAndOversizedCaptureBeforeAllocation") {
  RenderNode root;
  RecordSplitBackground(root.content);
  BackdropTestBitmap bitmap;
  REQUIRE_THROWS_AS(bitmap.DrawSlice(root, 1, 1), std::logic_error);
  REQUIRE_THROWS_AS(bitmap.DrawSlice(root, 0, 2), std::logic_error);
  REQUIRE_NOTHROW(bitmap.DrawSlice(root, 0, root.content.Commands().size()));
  REQUIRE_THROWS_AS(bitmap.Draw(root, CGRectMake(0.0, 0.0, 16.0, 8.0),
      CGRectMake(0.0, 0.0, 5000.0, 5000.0)), std::runtime_error);
}

} // namespace huxerui::test
