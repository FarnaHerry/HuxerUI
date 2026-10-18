#include <catch2/catch_amalgamated.hpp>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

#include <huxerui/effects.h>
#include <huxerui/paint.h>
#include <huxerui/render_scene.h>

#include "graphics/effects_internal.h"

namespace huxerui::test {
namespace {

std::vector<std::uint8_t> GradientPixels(std::size_t width, std::size_t height, std::uint8_t alpha = 255) {
  std::vector<std::uint8_t> pixels(width * height * 4);
  for (std::size_t y = 0; y < height; ++y) {
    for (std::size_t x = 0; x < width; ++x) {
      const auto offset = (y * width + x) * 4;
      pixels[offset] = static_cast<std::uint8_t>(x * alpha / width);
      pixels[offset + 1] = static_cast<std::uint8_t>(y * alpha / height);
      pixels[offset + 2] = static_cast<std::uint8_t>(alpha / 2);
      pixels[offset + 3] = alpha;
    }
  }
  return pixels;
}

DrawBackdropCommand WholeSurface(float width = 32, float height = 24) {
  return {{{{0, 0, width, height}, {}}}, BackdropEffect{.blur_radius = 0}};
}

} // namespace

TEST_CASE("BackdropIdentityPreservesPremultipliedAlphaAndExteriorPixels") {
  const auto input = GradientPixels(32, 24, 128);
  auto command = WholeSurface();
  auto output = detail::ApplyBackdropEffect(input, 32, 24, 128, {}, command);
  REQUIRE(output.bgra == input);
  REQUIRE(output.stride == 128);
  command.shapes = {{{8, 8, 16, 8}, CornerRadii{4}}};
  command.effect.brightness = 0.5F;
  output = detail::ApplyBackdropEffect(input, 32, 24, 128, {}, command);
  REQUIRE(output.bgra[3] == 128);
  REQUIRE(output.bgra[(12 * 32 + 16) * 4] < input[(12 * 32 + 16) * 4]);
  REQUIRE(output.bgra[(12 * 32 + 16) * 4 + 3] == 128);
  REQUIRE(std::equal(input.begin(), input.begin() + 32 * 4, output.bgra.begin()));
}

TEST_CASE("BackdropBlurSpreadsAnImpulseWithoutDarkeningUniformCaptureEdges") {
  std::vector<std::uint8_t> input(32 * 24 * 4, 0);
  for (std::size_t offset = 3; offset < input.size(); offset += 4) { input[offset] = 255; }
  input[(12 * 32 + 16) * 4 + 2] = 255;
  auto command = WholeSurface();
  command.effect.blur_radius = 6;
  const auto output = detail::ApplyBackdropEffect(input, 32, 24, 128, {}, command);
  REQUIRE(output.bgra[(12 * 32 + 16) * 4 + 2] < 255);
  REQUIRE(output.bgra[(12 * 32 + 15) * 4 + 2] > 0);
  std::fill(input.begin(), input.end(), 128);
  const auto uniform = detail::ApplyBackdropEffect(input, 32, 24, 128, {}, command);
  REQUIRE(uniform.bgra == input);
}

TEST_CASE("BackdropAsymmetricCornersFollowArcTangenciesBeyondCenterQuadrants") {
  const auto input = GradientPixels(100, 100);
  auto command = WholeSurface(100, 100);
  command.shapes.front().corner_radii = CornerRadii{80, 0, 0, 0};
  command.effect.brightness = 0.5F;
  const auto output = detail::ApplyBackdropEffect(input, 100, 100, 400, {}, command);
  const auto outside_arc = (0 * 100 + 55) * 4;
  REQUIRE(output.bgra[outside_arc] == input[outside_arc]);
  const auto inside_arc = (20 * 100 + 60) * 4;
  REQUIRE(output.bgra[inside_arc] < input[inside_arc]);
}

TEST_CASE("LiquidGlassRefractionAndHighlightChangeBackgroundWithinRoundedShape") {
  const auto input = GradientPixels(32, 24, 128);
  auto command = WholeSurface();
  command.shapes.front().corner_radii = CornerRadii{8};
  command.effect.refraction = 5;
  command.effect.highlight = 0.8F;
  const auto output = detail::ApplyBackdropEffect(input, 32, 24, 128, {}, command);
  REQUIRE(output.bgra != input);
  REQUIRE(std::equal(input.begin(), input.begin() + 4, output.bgra.begin()));
  for (std::size_t offset = 0; offset < output.bgra.size(); offset += 4) {
    REQUIRE(output.bgra[offset] <= output.bgra[offset + 3]);
    REQUIRE(output.bgra[offset + 1] <= output.bgra[offset + 3]);
    REQUIRE(output.bgra[offset + 2] <= output.bgra[offset + 3]);
    REQUIRE(output.bgra[offset + 3] == 128);
  }
}

TEST_CASE("BackdropMedialAxisDoesNotAmplifyCoordinateRoundingIntoRefraction") {
  const auto input = GradientPixels(32, 24);
  const DrawBackdropCommand command{{{{4.5F, 0.5F, 24, 24}, {}}},
                                    BackdropEffect{.blur_radius = 0, .refraction = 4,
                                                   .edge_width = 32, .highlight = 0.3F}};
  const std::size_t center = (12 * 32 + 16) * 4;
  for (const float offset : {-0.00000762939453125F, 0.0F, 0.00000762939453125F}) {
    CAPTURE(offset);
    const auto output = detail::ApplyBackdropEffect(input, 32, 24, 128, {1, 0, 0, 1, 0, offset}, command);
    for (std::size_t channel = 0; channel < 4; ++channel) {
      REQUIRE(output.bgra[center + channel] == input[center + channel]);
    }
  }
  const auto displaced = detail::ApplyBackdropEffect(input, 32, 24, 128, {1, 0, 0, 1, 0, 0.001F}, command);
  REQUIRE(!std::equal(input.begin() + center, input.begin() + center + 3, displaced.bgra.begin() + center));
}

TEST_CASE("GlassUnionFusionConnectsAdjacentShapesAndAppliesMaterialOnce") {
  const auto input = GradientPixels(32, 24);
  DrawBackdropCommand command{{{{2, 2, 12, 20}, {}}, {{16, 2, 12, 20}, {}}},
                              BackdropEffect{.blur_radius = 0, .brightness = 0.5F}};
  const auto separate = detail::ApplyBackdropEffect(input, 32, 24, 128, {}, command);
  command.effect.fusion_radius = 16;
  const auto fused = detail::ApplyBackdropEffect(input, 32, 24, 128, {}, command);
  const auto gap = (12 * 32 + 15) * 4;
  REQUIRE(separate.bgra[gap] == input[gap]);
  REQUIRE(fused.bgra[gap] < input[gap]);
  command.shapes[1] = command.shapes[0];
  command.effect.fusion_radius = 0;
  const auto overlap = detail::ApplyBackdropEffect(input, 32, 24, 128, {}, command);
  const auto center = (12 * 32 + 8) * 4;
  REQUIRE(overlap.bgra[center] == static_cast<std::uint8_t>(std::lround(input[center] * 0.5F)));
}

TEST_CASE("BackdropCaptureGeometrySupportsDpiAndNonCompactStride") {
  const auto input = GradientPixels(32, 24);
  auto command = WholeSurface(16, 12);
  Transform2D pixel_to_local{0.5F, 0, 0, 0.5F, 0, 0};
  const auto output = detail::ApplyBackdropEffect(input, 32, 24, 128, pixel_to_local, command);
  REQUIRE(output.bgra == input);
  std::vector<std::uint8_t> padded(24 * 132, 42);
  for (std::size_t row = 0; row < 24; ++row) {
    std::copy_n(input.begin() + row * 128, 128, padded.begin() + row * 132);
  }
  const auto compact = detail::ApplyBackdropEffect(padded, 32, 24, 132, pixel_to_local, command);
  REQUIRE(compact.bgra == input);
}

TEST_CASE("BackdropValidationRejectsInvalidParametersShapesAndCaptures") {
  auto command = WholeSurface();
  const auto input = GradientPixels(32, 24);
  command.effect.blur_radius = -1;
  REQUIRE_THROWS_AS(detail::ApplyBackdropEffect(input, 32, 24, 128, {}, command), std::invalid_argument);
  command = WholeSurface();
  command.effect.blur_quality = static_cast<BackdropBlurQuality>(-1);
  REQUIRE_THROWS_AS(detail::ValidateBackdropEffect(command.effect), std::invalid_argument);
  command = WholeSurface();
  command.effect.light_direction = {};
  REQUIRE_THROWS_AS(detail::ValidateBackdropEffect(command.effect), std::invalid_argument);
  command = WholeSurface();
  command.shapes.front().rect.width = std::numeric_limits<float>::infinity();
  REQUIRE_THROWS_AS(detail::ValidateBackdropShapes(command.shapes), std::invalid_argument);
  command = WholeSurface();
  REQUIRE_THROWS_AS(detail::ApplyBackdropEffect(input, 32, 24, 1, {}, command), std::invalid_argument);
  REQUIRE_THROWS_AS(detail::ApplyBackdropEffect({}, 8192, 8192, 32768, {}, command), std::invalid_argument);
  REQUIRE_THROWS_AS(detail::ApplyBackdropEffect(input, 32, 24, 128, Transform2D{0, 0, 0, 0}, command),
                    std::invalid_argument);
  REQUIRE(detail::ApplyBackdropEffect({}, 0, 0, 0, {}, command).bgra.empty());
}

TEST_CASE("BackdropFastQualityRetainsPortableFallbackAndConservativeSampling") {
  const auto input = GradientPixels(32, 24, 128);
  auto command = WholeSurface();
  command.effect.blur_radius = 18;
  const auto precise = detail::ApplyBackdropEffect(input, 32, 24, 128, {}, command);
  const Rect precise_bounds = detail::BackdropSamplingBounds(command);
  command.effect.blur_quality = BackdropBlurQuality::Fast;
  const auto fast = detail::ApplyBackdropEffect(input, 32, 24, 128, {}, command);
  const Rect fast_bounds = detail::BackdropSamplingBounds(command);
  REQUIRE(fast.bgra == precise.bgra);
  REQUIRE(fast_bounds.x < precise_bounds.x);
  REQUIRE(fast_bounds.y < precise_bounds.y);
  REQUIRE(fast_bounds.width > precise_bounds.width);
  REQUIRE(fast_bounds.height > precise_bounds.height);
}

TEST_CASE("BackdropBlurMapsNonuniformAxisScalesAndRejectsUnsupportedDirectionalKernels") {
  constexpr std::size_t extent = 64;
  std::vector<std::uint8_t> input(extent * extent * 4, 0);
  for (std::size_t offset = 3; offset < input.size(); offset += 4) { input[offset] = 255; }
  for (std::size_t y = 0; y < extent; ++y) { input[(y * extent + 32) * 4 + 2] = 255; }
  auto command = WholeSurface(32, 64);
  command.effect.blur_radius = 6;
  const auto stretched = detail::ApplyBackdropEffect(input, extent, extent, extent * 4,
                                                     Transform2D{0.5F, 0, 0, 1}, command);
  REQUIRE(stretched.bgra[(32 * extent + 41) * 4 + 2] > 0);
  REQUIRE(stretched.bgra[(32 * extent + 45) * 4 + 2] == 0);
  const Transform2D uniform_rotation{0, 0.5F, -0.5F, 0, 32, 0};
  REQUIRE_NOTHROW(detail::ApplyBackdropEffect(input, extent, extent, extent * 4, uniform_rotation, command));
  const Transform2D shear{1, 0, 0.5F, 1};
  REQUIRE_THROWS_AS(detail::ApplyBackdropEffect(input, extent, extent, extent * 4, shear, command), std::logic_error);
  command.effect.blur_radius = 0;
  REQUIRE_NOTHROW(detail::ApplyBackdropEffect(input, extent, extent, extent * 4, shear, command));
}

TEST_CASE("PaintContextRecordsImmutableBackdropUnionAndNormalizesCorners") {
  PaintSequence sequence;
  PaintContext context(sequence, {0, 0, 100, 80});
  context.DrawBackdrop({10, 20, 30, 10}, {}, CornerRadii{50});
  context.Finish();
  REQUIRE(sequence.HasBackdropCommands());
  REQUIRE(sequence.Bounds() == Rect{10, 20, 30, 10});
  const auto& command = std::get<DrawBackdropCommand>(sequence.Commands().front());
  REQUIRE(command.shapes.front().corner_radii == CornerRadii{5});
  REQUIRE_THROWS_AS(context.DrawBackdrop(Rect{}, {}), std::logic_error);
  PaintContext rerecord(sequence, {0, 0, 100, 80});
  REQUIRE_THROWS_AS(rerecord.DrawBackdrop(std::vector<BackdropShape>{}, {}), std::invalid_argument);
  rerecord.Finish();
  REQUIRE_FALSE(sequence.HasBackdropCommands());
}

TEST_CASE("BackdropSceneSlicesRequireAllVisibleCommandsIncludingForeground") {
  RenderNode root;
  PaintContext content(root.content, {0, 0, 32, 24});
  content.DrawRect({0, 0, 32, 24}, Color::White());
  content.DrawBackdrop({4, 4, 24, 16}, {});
  content.Finish();
  PaintContext foreground(root.foreground, {0, 0, 32, 24});
  foreground.DrawRect({0, 0, 4, 4}, Color::Black());
  foreground.Finish();
  RenderNode hidden;
  hidden.visible = false;
  hidden.content = root.content;
  RenderNode transparent;
  transparent.opacity = 0.0F;
  transparent.content = root.content;
  root.children = {&hidden, &transparent};
  REQUIRE_NOTHROW(detail::ValidateBackdropSceneSlice({&root}, 0, 3));
  REQUIRE_NOTHROW(detail::ValidateBackdropSceneSlice({&root}, 0, std::numeric_limits<std::size_t>::max()));
  REQUIRE_THROWS_AS(detail::ValidateBackdropSceneSlice({&root}, 0, 2), std::logic_error);
  REQUIRE_THROWS_AS(detail::ValidateBackdropSceneSlice({&root}, 1, 3), std::logic_error);
}

} // namespace huxerui::test
