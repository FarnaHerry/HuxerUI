#include "runtime_test_support.h"

namespace huxerui::test {
namespace {

const Color kNodeGuideColor = Color::Rgb(0, 188, 212);
const Color kPaddingGuideColor = Color::Rgb(255, 179, 0);

View GuidelinesApp() {
  return Column {
    Text("first"),
    Text("second").With(Padding(12.0F)),
  };
}

TEST_CASE("LayoutGuidelinesDrawNodeBoundsAndDashedPadding") {
  TestPlatform platform;
  Runtime runtime{GuidelinesApp, platform, {.show_debug_overlay = false, .show_layout_guidelines = true}};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();

  const DrawBorderCommand* padding_guide = FindBorderWithColor(scene, kPaddingGuideColor);
  REQUIRE(padding_guide != nullptr);
  REQUIRE(!padding_guide->style.dash_pattern.empty());

  const Rect content = padding_guide->rect;
  bool found_enclosing_node_guide = false;
  for (const auto& command : scene.Commands()) {
    const auto* node_guide = std::get_if<DrawBorderCommand>(&command);
    if (node_guide == nullptr || node_guide->color != kNodeGuideColor) {
      continue;
    }
    const Rect& bounds = node_guide->rect;
    if (bounds.x + 12.0F == content.x && bounds.y + 12.0F == content.y &&
        bounds.width - 24.0F == content.width && bounds.height - 24.0F == content.height) {
      found_enclosing_node_guide = true;
      break;
    }
  }
  REQUIRE(found_enclosing_node_guide);
}

TEST_CASE("LayoutGuidelinesStayDisabledByDefault") {
  TestPlatform platform;
  Runtime runtime{GuidelinesApp, platform, {.show_debug_overlay = false}};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();
  REQUIRE(FindBorderWithColor(scene, kNodeGuideColor) == nullptr);
  REQUIRE(FindBorderWithColor(scene, kPaddingGuideColor) == nullptr);
}

} // namespace
} // namespace huxerui::test
