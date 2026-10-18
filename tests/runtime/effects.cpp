#include "runtime_test_support.h"
#include "external_texture_test_support.h"

#include <algorithm>
#include <stdexcept>

#include <huxerui/effects.h>

#include "graphics/effects_internal.h"
#include "graphics/external_texture_internal.h"
#include "runtime/transition_internal.h"

namespace huxerui::test {
namespace {

int glass_group_clicks = 0;

State<int> glass_lifecycle_mode;
State<bool> glass_source_changed;

View GlassLifecycleApp() {
  auto mode = UseState(0);
  glass_lifecycle_mode = mode;
  View glass = Button("Glass").With(Frame{80, 40}, LiquidGlass{}).Key(mode.Get() == 2 ? "replacement" : "glass");
  View plain = Button("Plain").With(Frame{80, 40}).Key("plain");
  if (mode.Get() == 3) {
    return Row {
      std::move(plain),
    };
  }
  if (mode.Get() == 1) {
    return Row {
      std::move(plain),
      std::move(glass),
    };
  }
  return Row {
    std::move(glass),
    std::move(plain),
  };
}

const RenderNode* FindEffectRenderNode(const RenderNode& node, std::uint64_t id) {
  if (node.id == id) { return &node; }
  for (const auto* child : node.children) {
    if (child) {
      if (const auto* found = FindEffectRenderNode(*child, id)) { return found; }
    }
  }
  return nullptr;
}

DrawBackdropCommand RecordedGlass(NodeExtension& extension, const ViewNode& node) {
  PaintSequence sequence;
  PaintContext context(sequence, node.Bounds());
  extension.PaintBackdrop(node, context);
  context.Finish();
  return std::get<DrawBackdropCommand>(sequence.Commands().front());
}

const DrawBackdropCommand& FindBackdrop(const RenderNode& root) {
  for (const auto& command : root.content.Commands()) {
    if (const auto* effect = std::get_if<DrawBackdropCommand>(&command)) { return *effect; }
  }
  for (const auto* child : root.children) {
    if (detail::RenderSceneHasBackdropEffects(child)) { return FindBackdrop(*child); }
  }
  throw std::logic_error("HuxerUI test expected a backdrop command");
}

} // namespace

TEST_CASE("BackdropSurfaceRecordsBeforeItsOwnShadowBackgroundAndText") {
  TestPlatform platform;
  UiWindow runtime{[]() -> View {
    return Text("Glass").With(
        Frame{100.0F, 40.0F}, BackdropBlur{8.0F}, CornerRadius{12.0F},
        Shadow{.color = Color::Black(), .blur_radius = 4.0F}, Background{Color::Rgb(255, 255, 255, 0.2F)}
    );
  }, platform};
  runtime.SetWindowMetrics({.viewport = {160, 100}});
  const auto& frame = runtime.BuildRenderFrame();
  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const auto* rendered = FindEffectRenderNode(*frame.scene.root, root->identity);
  REQUIRE(rendered != nullptr);
  const auto& commands = rendered->content.Commands();
  REQUIRE(std::holds_alternative<DrawBackdropCommand>(commands[0]));
  REQUIRE(std::holds_alternative<DrawShadowCommand>(commands[1]));
  REQUIRE(std::holds_alternative<DrawRectCommand>(commands[2]));
  REQUIRE(std::holds_alternative<DrawTextCommand>(commands[3]));
  const auto& backdrop = std::get<DrawBackdropCommand>(commands[0]);
  REQUIRE(backdrop.effect.blur_radius == 8);
  REQUIRE(backdrop.shapes.front().corner_radii == CornerRadii{12});
  const auto revision = rendered->content.Revision();
  const auto& unchanged = runtime.BuildRenderFrame();
  REQUIRE(unchanged.damage.rects.empty());
  REQUIRE(FindEffectRenderNode(*unchanged.scene.root, root->identity)->content.Revision() == revision);
}

TEST_CASE("LiquidGlassRespondsToPressCancelDisabledAndReducedMotionWithoutChangingLayout") {
  detail::MountedNode node;
  node.bounds = {0, 0, 100, 40};
  node.resolved_corner_radii = CornerRadii{12};
  LiquidGlass modifier;
  const auto& descriptor = LiquidGlass::Descriptor();
  auto extension = descriptor.create_extension(node, &modifier);
  const auto resting = RecordedGlass(*extension, node);
  node.interaction.pressed = true;
  extension->OnInteraction(node, node.interaction,
                           InteractionEvent{.type = InteractionEvent::Type::Press, .position = {}});
  extension->OnFrame(node, FrameInfo{.timestamp = 1, .delta_time = 0.016, .reduced_motion = true});
  const auto pressed = RecordedGlass(*extension, node);
  REQUIRE(pressed.effect.refraction > resting.effect.refraction);
  REQUIRE(pressed.effect.highlight > resting.effect.highlight);
  REQUIRE(pressed.shapes == resting.shapes);
  node.interaction.pressed = false;
  extension->OnInteraction(node, node.interaction,
                           InteractionEvent{.type = InteractionEvent::Type::Cancel, .position = {}});
  extension->OnFrame(node, FrameInfo{.timestamp = 2, .delta_time = 0.016, .reduced_motion = true});
  REQUIRE(RecordedGlass(*extension, node).effect == resting.effect);
  node.interaction.pressed = true;
  extension->OnInteraction(node, node.interaction,
                           InteractionEvent{.type = InteractionEvent::Type::Press, .position = {}});
  extension->OnFrame(node, FrameInfo{.timestamp = 3, .delta_time = 0.016, .reduced_motion = true});
  node.interaction.enabled = false;
  extension->OnInteraction(node, node.interaction,
                           InteractionEvent{.type = InteractionEvent::Type::Cancel, .position = {}});
  const auto result = extension->OnFrame(node, FrameInfo{.timestamp = 4, .delta_time = 0.016});
  REQUIRE_FALSE(result.needs_frame);
  REQUIRE(RecordedGlass(*extension, node).effect == resting.effect);
}

TEST_CASE("LiquidGlassCompatibleUpdatePreservesRetainedInteractionProgress") {
  detail::MountedNode node;
  node.bounds = {0, 0, 100, 40};
  LiquidGlass modifier;
  const auto& descriptor = LiquidGlass::Descriptor();
  auto extension = descriptor.create_extension(node, &modifier);
  node.interaction.pressed = true;
  extension->OnInteraction(node, node.interaction, InteractionEvent{});
  extension->OnFrame(node, FrameInfo{.timestamp = 1, .delta_time = 0.016, .reduced_motion = true});
  modifier.effect.blur_radius = 24;
  modifier.effect.blur_quality = BackdropBlurQuality::Fast;
  descriptor.update_extension(*extension, node, &modifier);
  const auto updated = RecordedGlass(*extension, node);
  REQUIRE(updated.effect.blur_radius == 24);
  REQUIRE(updated.effect.blur_quality == BackdropBlurQuality::Fast);
  REQUIRE(updated.effect.refraction > modifier.effect.refraction);
}

TEST_CASE("LiquidGlassSpringReversalKeepsOpticalParametersInTheirPublicRanges") {
  detail::MountedNode node;
  node.bounds = {0, 0, 100, 40};
  LiquidGlass modifier;
  modifier.effect.highlight = 0.0F;
  auto extension = LiquidGlass::Descriptor().create_extension(node, &modifier);
  node.interaction.pressed = true;
  extension->OnInteraction(node, node.interaction, InteractionEvent{});
  for (int frame = 0; frame < 120; ++frame) {
    if (frame == 30) {
      node.interaction.pressed = false;
      extension->OnInteraction(node, node.interaction, InteractionEvent{});
    }
    extension->OnFrame(node, FrameInfo{.timestamp = frame / 60.0, .delta_time = 1.0 / 60.0});
    const auto command = RecordedGlass(*extension, node);
    REQUIRE(command.effect.highlight >= 0.0F);
    REQUIRE(command.effect.highlight <= 1.0F);
    REQUIRE(command.effect.refraction >= modifier.effect.refraction);
    REQUIRE(command.effect.refraction <= modifier.effect.refraction * 1.45F);
  }
  REQUIRE(RecordedGlass(*extension, node).effect == modifier.effect);
}

TEST_CASE("GlassGroupRecordsCurrentDirectChildUnionAndPreservesChildActions") {
  glass_group_clicks = 0;
  TestPlatform platform;
  UiWindow runtime{[]() -> View {
    return Row {
      Button("First").OnClick([] { ++glass_group_clicks; }).With(Frame{60, 40}, CornerRadius{20}),
      Button("Second").With(Frame{60, 40}, CornerRadius{20}),
    }.With(Spacing{4}, GlassGroup{});
  }, platform};
  runtime.SetWindowMetrics({.viewport = {160, 80}});
  const auto& frame = runtime.BuildRenderFrame();
  const auto& command = FindBackdrop(*frame.scene.root);
  REQUIRE(command.shapes.size() == 2);
  REQUIRE(command.shapes[0].rect.width == 60);
  REQUIRE(command.shapes[1].rect.x - command.shapes[0].rect.x == 64);
  REQUIRE(command.effect.fusion_radius == 16);
  ClickAt(runtime, {30, 20}, 1);
  REQUIRE(glass_group_clicks == 1);
}

TEST_CASE("BackdropDamagePropagatesBackgroundChangesAndFrozenScenesRejectLiveSampling") {
  RenderNode background;
  background.id = 1;
  PaintContext first(background.content, {0, 0, 100, 80});
  first.DrawRect({0, 0, 100, 80}, Color::White());
  first.Finish();
  RenderNode glass;
  glass.id = 2;
  PaintContext second(glass.content, {0, 0, 100, 80});
  second.DrawBackdrop({20, 20, 60, 40}, {});
  second.Finish();
  RenderNode root;
  root.id = 3;
  root.children = {&background, &glass};
  detail::RenderDamageSnapshot committed;
  Size committed_viewport;
  bool has_committed = false;
  detail::ComputeDamageRegion(&root, {100, 80}, committed, committed_viewport, has_committed, {});
  PaintContext changed(background.content, {0, 0, 100, 80});
  changed.DrawRect({0, 0, 100, 80}, Color::Black());
  changed.Finish();
  const auto damage = detail::ComputeDamageRegion(&root, {100, 80}, committed, committed_viewport, has_committed, {});
  REQUIRE_FALSE(damage.rects.empty());
  REQUIRE(std::any_of(damage.rects.begin(), damage.rects.end(), [](Rect rect) {
    return rect.Contains({20, 20}) && rect.Contains({80, 60});
  }));
  REQUIRE_THROWS_AS(detail::FreezeRenderScene(&root), std::logic_error);
  REQUIRE(detail::ComputeDamageRegion(&root, {100, 80}, committed, committed_viewport, has_committed, {})
              .rects.empty());
}

TEST_CASE("LiquidGlassKeyedMovementReplacementAndUnmountPreserveExpectedLifetimes") {
  TestPlatform platform;
  UiWindow runtime{GlassLifecycleApp, platform};
  runtime.SetWindowMetrics({.viewport = {160, 80}});
  runtime.BuildRenderFrame();
  const auto* mounted = FindMountedText(*runtime.RootNode(), "Glass");
  REQUIRE(mounted != nullptr);
  const auto identity = mounted->identity;
  const auto find_extension = [](const detail::MountedNode& node) -> const NodeExtension* {
    for (const auto& entry : node.extensions) {
      if (entry.descriptor == &LiquidGlass::Descriptor()) { return entry.extension.get(); }
    }
    return nullptr;
  };
  const auto* extension = find_extension(*mounted);
  REQUIRE(extension != nullptr);
  glass_lifecycle_mode = 1;
  runtime.BuildRenderFrame();
  mounted = FindMountedText(*runtime.RootNode(), "Glass");
  REQUIRE(mounted->identity == identity);
  REQUIRE(find_extension(*mounted) == extension);
  glass_lifecycle_mode = 2;
  runtime.BuildRenderFrame();
  mounted = FindMountedText(*runtime.RootNode(), "Glass");
  REQUIRE(mounted->identity != identity);
  REQUIRE(find_extension(*mounted) != nullptr);
  glass_lifecycle_mode = 3;
  const auto& removed = runtime.BuildRenderFrame();
  REQUIRE(FindMountedText(*runtime.RootNode(), "Glass") == nullptr);
  REQUIRE_FALSE(detail::RenderSceneHasBackdropEffects(removed.scene.root));
}

TEST_CASE("ExplicitBackdropDeclarationsOverrideThemeAndRejectAmbiguousRetainedSurfaces") {
  TestPlatform platform;
  UiWindow runtime{[]() -> View {
    auto definition = FlatThemeDefinition();
    auto button = ButtonStyle::Default();
    button.backdrop_effect = BackdropEffect{.blur_radius = 40};
    definition.Set(button);
    return Theme{definition, Button("Glass").With(LiquidGlass{})};
  }, platform};
  runtime.SetWindowMetrics({.viewport = {160, 80}});
  const auto& frame = runtime.BuildRenderFrame();
  const auto* mounted = FindMountedText(*runtime.RootNode(), "Glass");
  REQUIRE(mounted->properties.backdrop_override);
  REQUIRE_FALSE(mounted->properties.backdrop_effect.has_value());
  const auto* rendered = FindEffectRenderNode(*frame.scene.root, mounted->identity);
  REQUIRE(std::count_if(rendered->content.Commands().begin(), rendered->content.Commands().end(),
                       [](const auto& command) {
    return std::holds_alternative<DrawBackdropCommand>(command);
  }) == 1);
  TestPlatform duplicate_platform;
  UiWindow duplicate{[]() -> View { return Text("Invalid").With(LiquidGlass{}, GlassGroup{}); }, duplicate_platform};
  duplicate.SetWindowMetrics({.viewport = {160, 80}});
  REQUIRE_THROWS_AS(duplicate.BuildRenderFrame(), std::invalid_argument);
  TestPlatform later_platform;
  UiWindow later{[]() -> View { return Text("Invalid").With(LiquidGlass{}, BackdropBlur{}); }, later_platform};
  later.SetWindowMetrics({.viewport = {160, 80}});
  REQUIRE_THROWS_AS(later.BuildRenderFrame(), std::invalid_argument);
}

TEST_CASE("BackdropDamageStaysLocalWhenUnrelatedBackgroundChanges") {
  RenderNode background;
  background.id = 1;
  PaintContext first(background.content, {0, 0, 20, 20});
  first.DrawRect({0, 0, 20, 20}, Color::White());
  first.Finish();
  RenderNode glass;
  glass.id = 2;
  PaintContext second(glass.content, {0, 0, 300, 80});
  second.DrawBackdrop({200, 20, 60, 40}, BackdropEffect{.blur_radius = 8});
  second.Finish();
  RenderNode root;
  root.id = 3;
  root.children = {&background, &glass};
  detail::RenderDamageSnapshot committed;
  Size viewport;
  bool has_committed = false;
  detail::ComputeDamageRegion(&root, {300, 80}, committed, viewport, has_committed, {});
  PaintContext changed(background.content, {0, 0, 20, 20});
  changed.DrawRect({0, 0, 20, 20}, Color::Black());
  changed.Finish();
  const auto damage = detail::ComputeDamageRegion(&root, {300, 80}, committed, viewport, has_committed, {});
  REQUIRE_FALSE(damage.full);
  REQUIRE(damage.rects.size() == 1);
  REQUIRE(damage.rects.front() == Rect{0, 0, 20, 20});
}

TEST_CASE("BackdropSourceDamageInsideOpacityGroupSurvivesDeferredInheritedClip") {
  RenderNode background;
  background.id = 1;
  PaintContext first(background.content, {8, 10, 2, 20});
  first.DrawRect({8, 10, 2, 20}, Color::White());
  first.Finish();
  RenderNode glass;
  glass.id = 2;
  PaintContext effect(glass.content, {10, 10, 20, 20});
  effect.DrawBackdrop({10, 10, 20, 20}, BackdropEffect{.blur_radius = 6});
  effect.Finish();
  RenderNode group;
  group.id = 3;
  group.opacity = 0.5F;
  group.children = {&background, &glass};
  RenderNode root;
  root.id = 4;
  root.child_clips = {PushClipCommand{{10, 10, 20, 20}}};
  root.children = {&group};
  detail::RenderDamageSnapshot committed;
  Size viewport;
  bool has_committed = false;
  detail::ComputeDamageRegion(&root, {40, 40}, committed, viewport, has_committed, {});
  PaintContext changed(background.content, {8, 10, 2, 20});
  changed.DrawRect({8, 10, 2, 20}, Color::Black());
  changed.Finish();
  const auto damage = detail::ComputeDamageRegion(&root, {40, 40}, committed, viewport, has_committed, {});
  REQUIRE(std::any_of(damage.rects.begin(), damage.rects.end(), [](Rect rect) {
    return rect.Contains({10, 10}) && rect.Contains({30, 30});
  }));
  REQUIRE(detail::ComputeDamageRegion(&root, {40, 40}, committed, viewport, has_committed, {}).rects.empty());
}

TEST_CASE("BackdropTracksTexturePublicationOutsideDeferredOpacityGroupClip") {
  TestPlatform platform;
  UiWindow runtime{[]() -> View { return Text("Host"); }, platform};
  auto requester = std::make_shared<detail::ExternalTextureFrameRequester>(runtime, [](auto callback) { callback(); });
  const auto texture = std::make_shared<ExternalTextureTestTexture>(Size{2, 20});
  RenderNode background;
  background.id = 1;
  PaintContext source(background.content, {8, 10, 2, 20});
  source.DrawImage(texture, {8, 10, 2, 20});
  source.Finish();
  RenderNode glass;
  glass.id = 2;
  PaintContext effect(glass.content, {10, 10, 20, 20});
  effect.DrawBackdrop({10, 10, 20, 20}, BackdropEffect{.blur_radius = 6});
  effect.Finish();
  RenderNode group;
  group.id = 3;
  group.opacity = 0.5F;
  group.children = {&background, &glass};
  RenderNode root;
  root.id = 4;
  root.child_clips = {PushClipCommand{{10, 10, 20, 20}}};
  root.children = {&group};
  detail::RenderDamageSnapshot committed;
  Size viewport;
  bool has_committed = false;
  detail::ComputeDamageRegion(&root, {40, 40}, committed, viewport, has_committed, requester);
  REQUIRE(committed.at(background.id).external_textures.size() == 1);
  const auto revision = background.content.Revision();
  texture->PublishFrame();
  const auto damage = detail::ComputeDamageRegion(&root, {40, 40}, committed, viewport, has_committed, requester);
  REQUIRE(background.content.Revision() == revision);
  REQUIRE(std::any_of(damage.rects.begin(), damage.rects.end(), [](Rect rect) {
    return rect.Contains({10, 10}) && rect.Contains({30, 30});
  }));
  detail::DeactivateExternalTextures(committed, requester);
  requester->Close();
}

TEST_CASE("BackdropRuntimePreservesPrefixChildrenOutsideAnOpacityGroupsDeferredClip") {
  TestPlatform platform;
  UiWindow runtime{[]() -> View {
    auto changed = UseState(false);
    glass_source_changed = changed;
    return Stack {
      Stack {
        Canvas([changed = changed.Get()](PaintContext& paint, Size) {
          paint.DrawRect({0, 0, 16, 16}, changed ? Color::Black() : Color::White());
        }).With(Frame{16, 16}, Offset{Point{16, 0}}),
        Spacer().With(Frame{16, 16}, BackdropBlur{6}),
      }.With(Frame{32, 16}, Opacity{0.5F}),
    }.With(Frame{16, 16}, ClipChildren{});
  }, platform};
  runtime.SetWindowMetrics({.viewport = {40, 20}});
  const auto& first = runtime.BuildRenderFrame();
  const auto* source = FindMountedKind(*runtime.RootNode(), detail::NodeKind::Canvas);
  REQUIRE(source != nullptr);
  const auto* rendered = FindEffectRenderNode(*first.scene.root, source->identity);
  REQUIRE(rendered != nullptr);
  REQUIRE(rendered->visible);
  REQUIRE(rendered->transform.translate_x == 16);
  glass_source_changed = true;
  const auto& updated = runtime.BuildRenderFrame();
  REQUIRE(std::any_of(updated.damage.rects.begin(), updated.damage.rects.end(), [](Rect rect) {
    return rect.Contains({0, 0}) && rect.Contains({16, 16});
  }));
  REQUIRE(runtime.BuildRenderFrame().damage.rects.empty());
}

TEST_CASE("BackdropDamagePropagatesThroughEarlierEffectsOutsideAnOpacityGroupsDeferredClip") {
  RenderNode background;
  background.id = 1;
  PaintContext source(background.content, {26, 10, 2, 20});
  source.DrawRect({26, 10, 2, 20}, Color::White());
  source.Finish();
  RenderNode earlier;
  earlier.id = 2;
  PaintContext outside(earlier.content, {16, 10, 8, 20});
  outside.DrawBackdrop({16, 10, 8, 20}, BackdropEffect{.blur_radius = 8});
  outside.Finish();
  RenderNode later;
  later.id = 3;
  PaintContext inside(later.content, {0, 10, 16, 20});
  inside.DrawBackdrop({0, 10, 16, 20}, BackdropEffect{.blur_radius = 2});
  inside.Finish();
  RenderNode group;
  group.id = 4;
  group.opacity = 0.5F;
  group.children = {&background, &earlier, &later};
  RenderNode root;
  root.id = 5;
  root.child_clips = {PushClipCommand{{0, 10, 16, 20}}};
  root.children = {&group};
  detail::RenderDamageSnapshot committed;
  Size viewport;
  bool has_committed = false;
  detail::ComputeDamageRegion(&root, {40, 40}, committed, viewport, has_committed, {});
  PaintContext changed(background.content, {26, 10, 2, 20});
  changed.DrawRect({26, 10, 2, 20}, Color::Black());
  changed.Finish();
  const auto damage = detail::ComputeDamageRegion(&root, {40, 40}, committed, viewport, has_committed, {});
  REQUIRE(std::any_of(damage.rects.begin(), damage.rects.end(), [](Rect rect) {
    return rect.Contains({0, 10}) && rect.Contains({16, 30});
  }));
  REQUIRE(detail::ComputeDamageRegion(&root, {40, 40}, committed, viewport, has_committed, {}).rects.empty());
}

} // namespace huxerui::test
