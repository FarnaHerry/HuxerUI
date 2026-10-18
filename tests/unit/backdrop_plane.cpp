#include <catch2/catch_amalgamated.hpp>

#include <cstdint>
#include <vector>

#include <huxerui/effects.h>
#include <huxerui/geometry.h>
#include <huxerui/paint.h>

#include "graphics/backdrop_plane_internal.h"
#include "graphics/effects_internal.h"
#include "graphics/geometry_internal.h"

namespace huxerui::test {
namespace {

std::vector<std::uint8_t> GradientPixels(std::size_t width, std::size_t height) {
  std::vector<std::uint8_t> pixels(width * height * 4);
  for (std::size_t y = 0; y < height; ++y) {
    for (std::size_t x = 0; x < width; ++x) {
      const auto offset = (y * width + x) * 4;
      pixels[offset] = static_cast<std::uint8_t>(x * 255 / width);
      pixels[offset + 1] = static_cast<std::uint8_t>(y * 255 / height);
      pixels[offset + 2] = static_cast<std::uint8_t>(x * 255 / (width + height));
      pixels[offset + 3] = 255;
    }
  }
  return pixels;
}

DrawBackdropCommand GlassRect(Rect rect, float blur, float refraction) {
  return DrawBackdropCommand{{{BackdropShape{rect, CornerRadii{6.0F}}}},
                             BackdropEffect{.blur_radius = blur, .refraction = refraction,
                                            .edge_width = 6.0F, .highlight = 0.4F}};
}

Rect SamplingOf(const DrawBackdropCommand& command) {
  return detail::BackdropSamplingBounds(command);
}

} // namespace

TEST_CASE("BackdropPlaneCollectorBatchesDisjointGlassAndRejectsOverlappingSources") {
  detail::BackdropPlaneCollector collector;
  const DrawBackdropCommand left = GlassRect({4.0F, 8.0F, 36.0F, 28.0F}, 5.0F, 3.0F);
  const DrawBackdropCommand right = GlassRect({56.0F, 12.0F, 32.0F, 24.0F}, 5.0F, 3.0F);
  REQUIRE(collector.empty());
  REQUIRE(collector.TryAdd(left, {}, 3.0F));
  REQUIRE(collector.TryAdd(right, {}, 3.0F));
  REQUIRE(collector.batch().commands.size() == 2);
  REQUIRE(collector.batch().output_bounds == Rect{4.0F, 8.0F, 84.0F, 28.0F});

  // A command whose sampling halo reaches another member's optical output must start a new batch.
  const DrawBackdropCommand overlapping = GlassRect({30.0F, 12.0F, 20.0F, 20.0F}, 8.0F, 4.0F);
  REQUIRE(SamplingOf(overlapping).Intersects(detail::BackdropBounds(left)));
  REQUIRE_FALSE(collector.TryAdd(overlapping, {}, 3.0F));
  collector.Clear();
  REQUIRE(collector.empty());
  REQUIRE(collector.TryAdd(overlapping, {}, 3.0F));
}

TEST_CASE("BackdropPlaneCollectorEnforcesCommandAndShapeBudgets") {
  detail::BackdropPlaneCollector collector;
  std::vector<DrawBackdropCommand> commands;
  commands.reserve(detail::BackdropPlaneCollector::maximum_commands + 1);
  for (std::size_t index = 0; index != detail::BackdropPlaneCollector::maximum_commands; ++index) {
    commands.push_back(GlassRect({static_cast<float>(index) * 60.0F, 0.0F, 40.0F, 30.0F}, 0.0F, 0.0F));
    REQUIRE(collector.TryAdd(commands.back(), {}, 0.0F));
  }
  REQUIRE(collector.batch().commands.size() == detail::BackdropPlaneCollector::maximum_commands);
  commands.push_back(GlassRect({5000.0F, 0.0F, 40.0F, 30.0F}, 0.0F, 0.0F));
  REQUIRE_FALSE(collector.TryAdd(commands.back(), {}, 0.0F));
  collector.Clear();
  commands.clear();

  const auto fused_at = [](float offset) {
    DrawBackdropCommand command;
    for (std::size_t index = 0; index != 64; ++index) {
      command.shapes.push_back(
          BackdropShape{{offset + static_cast<float>(index) * 60.0F, 0.0F, 40.0F, 30.0F}, CornerRadii{}});
    }
    return command;
  };
  for (std::size_t added = 0; added != detail::BackdropPlaneCollector::maximum_shapes / 64; ++added) {
    commands.push_back(fused_at(static_cast<float>(added) * 4000.0F));
    REQUIRE(collector.TryAdd(commands.back(), {}, 0.0F));
  }
  commands.push_back(fused_at(4000.0F * detail::BackdropPlaneCollector::maximum_shapes / 64));
  REQUIRE_FALSE(collector.TryAdd(commands.back(), {}, 0.0F));
  collector.Clear();
  REQUIRE(collector.TryAdd(commands.front(), {}, 0.0F));
}

TEST_CASE("BackdropPlaneCollectorBatchesDisjointGridWithoutTreatingTheUnionGapAsOutput") {
  detail::BackdropPlaneCollector collector;
  std::vector<DrawBackdropCommand> commands;
  commands.reserve(6);
  for (std::size_t index = 0; index != 6; ++index) {
    commands.push_back(GlassRect({30.0F + static_cast<float>(index % 3) * 130.0F,
                                 40.0F + static_cast<float>(index / 3) * 130.0F, 100.0F, 60.0F}, 14.0F, 6.0F));
    if (index == 4) {
      REQUIRE(SamplingOf(commands.back()).Intersects(collector.batch().output_bounds));
    }
    REQUIRE(collector.TryAdd(commands.back(), {}, 3.0F));
  }
  REQUIRE(collector.batch().commands.size() == 6);
}

TEST_CASE("BackdropPlaneCollectorRejectsSamplingOfAnEarlierAntialiasedFringe") {
  detail::BackdropPlaneCollector collector;
  const DrawBackdropCommand first = GlassRect({0.2F, 0.2F, 40.0F, 30.0F}, 0.0F, 0.0F);
  const DrawBackdropCommand second = GlassRect({43.45F, 8.0F, 20.0F, 20.0F}, 0.0F, 0.0F);
  REQUIRE_FALSE(SamplingOf(second).Intersects(detail::BackdropBounds(first)));
  REQUIRE(collector.TryAdd(first, {}, 0.0F));
  REQUIRE_FALSE(collector.TryAdd(second, {}, 0.0F));
}

TEST_CASE("BackdropPlaneCollectorBlocksInterleavedRasterWorkInsideTheSamplingUnion") {
  detail::BackdropPlaneCollector collector;
  REQUIRE_FALSE(collector.BlockedBy(Rect{0.0F, 0.0F, 1000.0F, 1000.0F}));
  const DrawBackdropCommand command = GlassRect({100.0F, 100.0F, 60.0F, 40.0F}, 8.0F, 4.0F);
  REQUIRE(collector.TryAdd(command, {}, 3.0F));
  const Rect halo = collector.batch().sampling_bounds;
  REQUIRE(collector.BlockedBy(halo));
  REQUIRE(collector.BlockedBy(Rect{halo.x - 1.0F, halo.y - 1.0F, 2.0F, 2.0F}));
  REQUIRE_FALSE(collector.BlockedBy(Rect{halo.x + halo.width + 4.0F, halo.y, 4.0F, 4.0F}));
  REQUIRE_FALSE(collector.BlockedBy(Rect{halo.x, halo.y + halo.height + 4.0F, 4.0F, 4.0F}));
}

TEST_CASE("ConservativeRasterBoundsCoversRasterCommandsAndDefersStructuralOnes") {
  const PaintCommand rect = DrawRectCommand{.rect = {2.0F, 3.0F, 8.0F, 4.0F}, .brush = Brush{Color::Black()}};
  REQUIRE(detail::ConservativeRasterBounds(rect) == Rect{2.0F, 3.0F, 8.0F, 4.0F});

  DrawTextRunsCommand runs;
  runs.runs = {TextRun{.bounds = {4.0F, 6.0F, 10.0F, 8.0F}}, TextRun{.bounds = {20.0F, 2.0F, 6.0F, 8.0F}}};
  REQUIRE(detail::ConservativeRasterBounds(PaintCommand{runs}) == Rect{4.0F, 2.0F, 22.0F, 12.0F});

  DrawLineCommand line;
  line.start = {0.0F, 0.0F};
  line.end = {10.0F, 4.0F};
  line.style.width = 2.0F;
  const Rect line_bounds = *detail::ConservativeRasterBounds(PaintCommand{line});
  REQUIRE(line_bounds.x < 0.0F);
  REQUIRE(line_bounds.x + line_bounds.width > 10.0F);
  REQUIRE(line_bounds.y < 0.0F);
  REQUIRE(line_bounds.y + line_bounds.height > 4.0F);

  DrawShadowCommand shadow;
  shadow.rect = {10.0F, 10.0F, 20.0F, 8.0F};
  shadow.offset = {3.0F, 4.0F};
  shadow.blur_radius = 5.0F;
  shadow.spread = 2.0F;
  const Rect shadow_bounds = *detail::ConservativeRasterBounds(PaintCommand{shadow});
  REQUIRE(shadow_bounds.x <= 10.0F - 11.0F);
  REQUIRE(shadow_bounds.y + shadow_bounds.height >= 18.0F + 11.0F);

  Path path;
  path.MoveTo({1.0F, 1.0F}).LineTo({30.0F, 1.0F}).LineTo({30.0F, 20.0F});
  const PaintCommand fill = FillPathCommand{path, Brush{Color::Black()}, {2.0F, 2.0F, 28.0F, 18.0F}};
  const Rect path_bounds = *detail::ConservativeRasterBounds(fill);
  REQUIRE(path_bounds.x <= 1.0F);
  REQUIRE(path_bounds.y + path_bounds.height >= 20.0F);

  REQUIRE_FALSE(detail::ConservativeRasterBounds(PaintCommand{PopClipCommand{}}).has_value());
  const PushTransformCommand transform{.transform = detail::TranslationTransform({4.0F, 4.0F})};
  REQUIRE_FALSE(detail::ConservativeRasterBounds(PaintCommand{transform}).has_value());
}

TEST_CASE("BackdropPlaneExecutionOnASharedCaptureMatchesImmediateReplay") {
  const std::size_t width = 96;
  const std::size_t height = 48;
  const std::vector<std::uint8_t> capture = GradientPixels(width, height);
  const DrawBackdropCommand first = GlassRect({4.0F, 8.0F, 36.0F, 28.0F}, 5.0F, 3.0F);
  const DrawBackdropCommand second = GlassRect({56.0F, 12.0F, 32.0F, 24.0F}, 5.0F, 3.0F);

  detail::BackdropPlaneCollector collector;
  REQUIRE(collector.TryAdd(first, {}, 3.0F));
  REQUIRE(collector.TryAdd(second, {}, 3.0F));

  // Immediate replay crops each command's sampling region from the active surface, applies the
  // effect, and replaces that region, mirroring per-command renderer semantics exactly.
  std::vector<std::uint8_t> immediate = capture;
  for (const DrawBackdropCommand* command : {&first, &second}) {
    const Rect sampling = detail::BackdropSamplingBounds(*command);
    const std::size_t left = static_cast<std::size_t>(sampling.x);
    const std::size_t top = static_cast<std::size_t>(sampling.y);
    const std::size_t right = static_cast<std::size_t>(sampling.x + sampling.width);
    const std::size_t bottom = static_cast<std::size_t>(sampling.y + sampling.height);
    std::vector<std::uint8_t> region((right - left) * (bottom - top) * 4);
    for (std::size_t y = top; y < bottom; ++y) {
      std::copy_n(immediate.begin() + static_cast<std::ptrdiff_t>((y * width + left) * 4), (right - left) * 4,
                  region.begin() + static_cast<std::ptrdiff_t>((y - top) * (right - left) * 4));
    }
    const auto output = detail::ApplyBackdropEffect(region, right - left, bottom - top, (right - left) * 4,
                                                    detail::TranslationTransform(
                                                        {static_cast<float>(left), static_cast<float>(top)}),
                                                    *command);
    for (std::size_t y = top; y < bottom; ++y) {
      std::copy_n(output.bgra.begin() + static_cast<std::ptrdiff_t>((y - top) * output.stride), (right - left) * 4,
                  immediate.begin() + static_cast<std::ptrdiff_t>((y * width + left) * 4));
    }
  }

  // Plane execution runs every member against the shared capture taken at the first member's
  // position, then writes each member's sampling region back in command order.
  std::vector<std::uint8_t> planar = capture;
  for (const auto& member : collector.batch().commands) {
    const Rect sampling = detail::BackdropSamplingBounds(*member.command);
    const std::size_t left = static_cast<std::size_t>(sampling.x);
    const std::size_t top = static_cast<std::size_t>(sampling.y);
    const std::size_t right = static_cast<std::size_t>(sampling.x + sampling.width);
    const std::size_t bottom = static_cast<std::size_t>(sampling.y + sampling.height);
    std::vector<std::uint8_t> region((right - left) * (bottom - top) * 4);
    for (std::size_t y = top; y < bottom; ++y) {
      std::copy_n(capture.begin() + static_cast<std::ptrdiff_t>((y * width + left) * 4), (right - left) * 4,
                  region.begin() + static_cast<std::ptrdiff_t>((y - top) * (right - left) * 4));
    }
    const auto output = detail::ApplyBackdropEffect(region, right - left, bottom - top, (right - left) * 4,
                                                    detail::TranslationTransform(
                                                        {static_cast<float>(left), static_cast<float>(top)}),
                                                    *member.command);
    for (std::size_t y = top; y < bottom; ++y) {
      std::copy_n(output.bgra.begin() + static_cast<std::ptrdiff_t>((y - top) * output.stride), (right - left) * 4,
                  planar.begin() + static_cast<std::ptrdiff_t>((y * width + left) * 4));
    }
  }

  REQUIRE(planar == immediate);
}

} // namespace huxerui::test
