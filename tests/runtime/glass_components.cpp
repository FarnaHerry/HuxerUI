#include "runtime_test_support.h"

#include <huxerui/effects.h>

namespace huxerui::test {
namespace {

int theme_glass_clicks = 0;
State<TextEditingValue> glass_editing;
State<int> glass_theme_mode;
State<bool> glass_supporting_message;
int invalid_glass_style = 0;
int invalid_glass_presentation = 0;
std::optional<DialogHandle> invalid_glass_dialog;
std::optional<MenuHandle> invalid_glass_menu;

std::vector<const DrawBackdropCommand*> SurfaceCommands(const RenderNode& node) {
  std::vector<const DrawBackdropCommand*> result;
  for (const PaintSequence* sequence : {&node.content, &node.foreground}) {
    for (const PaintCommand& command : sequence->Commands()) {
      if (const auto* backdrop = std::get_if<DrawBackdropCommand>(&command)) {
        result.push_back(backdrop);
      }
    }
  }
  for (const RenderNode* child : node.children) {
    if (child) {
      auto commands = SurfaceCommands(*child);
      result.insert(result.end(), commands.begin(), commands.end());
    }
  }
  return result;
}

} // namespace

TEST_CASE("GlassThemeSurfacesShareTheTypedEffectWithoutChangingControlActions", "[glass]") {
  theme_glass_clicks = 0;
  TestPlatform platform;
  UiWindow runtime{[]() -> View {
    ButtonStyle button = ButtonStyle::Default();
    button.background = Color::Rgb(255, 255, 255, 0.2F);
    button.backdrop_effect = BackdropEffect{.blur_radius = 7.0F};
    ChipStyle chip = ChipStyle::Default();
    chip.background = Color::Rgb(255, 255, 255, 0.2F);
    chip.backdrop_effect = BackdropEffect{.blur_radius = 11.0F};
    ThemeDefinition definition;
    definition.Set(button);
    definition.Set(chip);
    return Theme {
      definition,
      Column {
        Button("Action").OnClick([] { ++theme_glass_clicks; }),
        Chip("Choice"),
      },
    };
  }, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  const auto& frame = runtime.BuildRenderFrame();
  const auto commands = SurfaceCommands(*frame.scene.root);
  REQUIRE(commands.size() == 2);
  REQUIRE(commands[0]->effect.blur_radius == 7.0F);
  REQUIRE(commands[1]->effect.blur_radius == 11.0F);
  const auto* action = FindMountedText(*runtime.RootNode(), "Action");
  REQUIRE(action != nullptr);
  InvokeClick(*action);
  REQUIRE(theme_glass_clicks == 1);
}

TEST_CASE("TextFieldGlassThemeTargetsOnlyItsEditorAndPreservesControlledSelection", "[glass]") {
  TestPlatform platform;
  UiWindow runtime{[]() -> View {
    glass_editing = UseState(TextEditingValue::FromText("Content"));
    TextFieldStyle style = TextFieldStyle::Default();
    style.standard.background = Color::Rgb(255, 255, 255, 0.2F);
    style.standard.backdrop_effect = BackdropEffect{.blur_radius = 9.0F};
    ThemeDefinition definition;
    definition.Set(style);
    return Theme {
      definition,
      TextField(glass_editing)
          .Label("Label")
          .Validation(ValidationResult::Invalid("Supporting text"))
          .With(Frame{.width = 240.0F})
          .OnChanged([](TextEditingValue value) { glass_editing = std::move(value); }),
    };
  }, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  const auto& frame = runtime.BuildRenderFrame();
  const auto commands = SurfaceCommands(*frame.scene.root);
  REQUIRE(commands.size() == 1);
  const auto* field = FindMountedKind(*runtime.RootNode(), detail::NodeKind::TextField);
  REQUIRE(field != nullptr);
  REQUIRE(commands.front()->effect.blur_radius == 9.0F);
  REQUIRE(commands.front()->shapes.front().rect.height < field->bounds.height);
  TextEditingValue next = glass_editing.Get();
  next.selection = {1, 4};
  glass_editing = next;
  runtime.BuildRenderFrame();
  REQUIRE(glass_editing.Get().text == "Content");
  REQUIRE(glass_editing.Get().selection == TextSelection{1, 4});
}

TEST_CASE("ExplicitTextFieldGlassOverridesItsInheritedEditorMaterial", "[glass]") {
  TestPlatform platform;
  UiWindow runtime{[]() -> View {
    TextFieldStyle style = TextFieldStyle::Default();
    style.standard.backdrop_effect = BackdropEffect{.blur_radius = 9.0F};
    ThemeDefinition definition;
    definition.Set(style);
    return Theme {
      definition,
      TextField(TextEditingValue::FromText("Content"))
          .With(Frame{.width = 240.0F}, BackdropBlur{3.0F}),
    };
  }, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  const auto& frame = runtime.BuildRenderFrame();
  const auto commands = SurfaceCommands(*frame.scene.root);
  REQUIRE(commands.size() == 1);
  REQUIRE(commands.front()->effect.blur_radius == 3.0F);
}

TEST_CASE("TextFieldGlassThemeChangesAndRemovalPreserveMountedEditingState", "[glass]") {
  TestPlatform platform;
  UiWindow runtime{[]() -> View {
    glass_theme_mode = UseState(0);
    glass_editing = UseState(TextEditingValue::FromText("Content"));
    TextFieldStyle style = TextFieldStyle::Default();
    style.standard.background = Color::Rgb(255, 255, 255, 0.2F);
    if (glass_theme_mode.Get() != 2) {
      style.standard.backdrop_effect = BackdropEffect{.blur_radius = glass_theme_mode.Get() == 1 ? 3.0F : 9.0F};
    }
    ThemeDefinition definition;
    definition.Set(style);
    return Theme {
      definition,
      TextField(glass_editing)
          .With(Frame{.width = 240.0F, .height = 100.0F})
          .OnChanged([](TextEditingValue value) { glass_editing = std::move(value); }),
    };
  }, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildRenderFrame();
  const auto* field = FindMountedKind(*runtime.RootNode(), detail::NodeKind::TextField);
  REQUIRE(field != nullptr);
  const std::uint64_t identity = field->identity;
  const auto initial_commands = SurfaceCommands(field->render_node);
  REQUIRE(initial_commands.size() == 1);
  REQUIRE(initial_commands.front()->effect.blur_radius == 9.0F);
  const std::uint64_t initial_content_revision = field->render_node.content.Revision();

  TextEditingValue editing = glass_editing.Get();
  editing.selection = {1, 4};
  glass_editing = editing;
  runtime.BuildRenderFrame();
  REQUIRE(field->render_node.content.Revision() == initial_content_revision);

  glass_theme_mode = 1;
  runtime.BuildRenderFrame();
  const auto changed_commands = SurfaceCommands(field->render_node);
  REQUIRE(changed_commands.size() == 1);
  REQUIRE(changed_commands.front()->effect.blur_radius == 3.0F);
  REQUIRE(field->render_node.content.Revision() > initial_content_revision);
  REQUIRE(field->identity == identity);
  REQUIRE(glass_editing.Get() == editing);

  glass_theme_mode = 2;
  runtime.BuildRenderFrame();
  REQUIRE(SurfaceCommands(field->render_node).empty());
  REQUIRE(field->identity == identity);
  REQUIRE(glass_editing.Get() == editing);

  glass_theme_mode = 0;
  runtime.BuildRenderFrame();
  const auto restored_commands = SurfaceCommands(field->render_node);
  REQUIRE(restored_commands.size() == 1);
  REQUIRE(restored_commands.front()->effect.blur_radius == 9.0F);
  REQUIRE(field->identity == identity);
  REQUIRE(glass_editing.Get() == editing);
}

TEST_CASE("TextFieldGlassTracksEditorGeometryWithinAnUnchangedOuterFrame", "[glass]") {
  TestPlatform platform;
  UiWindow runtime{[]() -> View {
    glass_supporting_message = UseState(false);
    glass_editing = UseState(TextEditingValue::FromText("Content"));
    TextFieldStyle style = TextFieldStyle::Default();
    style.outlined.background = Color::Rgb(255, 255, 255, 0.2F);
    style.outlined.backdrop_effect = BackdropEffect{.blur_radius = 9.0F};
    ThemeDefinition definition;
    definition.Set(style);
    return Theme {
      definition,
      TextField(glass_editing)
          .Variant(TextFieldVariant::Outlined)
          .Label("Label")
          .Validation(glass_supporting_message.Get() ? ValidationResult::Invalid("Supporting text")
                                                     : ValidationResult{})
          .With(Frame{.width = 240.0F, .height = 100.0F}),
    };
  }, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildRenderFrame();
  const auto* field = FindMountedKind(*runtime.RootNode(), detail::NodeKind::TextField);
  REQUIRE(field != nullptr);
  const Rect initial_bounds = field->bounds;
  const auto initial_commands = SurfaceCommands(field->render_node);
  REQUIRE(initial_commands.size() == 1);
  const Rect initial_editor = initial_commands.front()->shapes.front().rect;
  const std::uint64_t initial_content_revision = field->render_node.content.Revision();

  glass_supporting_message = true;
  runtime.BuildRenderFrame();
  const auto changed_commands = SurfaceCommands(field->render_node);
  REQUIRE(changed_commands.size() == 1);
  REQUIRE(field->bounds == initial_bounds);
  REQUIRE(changed_commands.front()->shapes.front().rect.height < initial_editor.height);
  REQUIRE(field->render_node.content.Revision() > initial_content_revision);
  REQUIRE(glass_editing.Get().text == "Content");

  glass_supporting_message = false;
  runtime.BuildRenderFrame();
  const auto restored_commands = SurfaceCommands(field->render_node);
  REQUIRE(restored_commands.size() == 1);
  REQUIRE(restored_commands.front()->shapes.front().rect == initial_editor);
  REQUIRE(field->bounds == initial_bounds);
}

TEST_CASE("GlassPopupStylesRejectInvalidEffectsBeforeOpening", "[glass]") {
  SECTION("Select trigger") {
    invalid_glass_style = 0;
  }
  SECTION("Select popup") {
    invalid_glass_style = 1;
  }
  SECTION("ComboBox popup") {
    invalid_glass_style = 2;
  }
  TestPlatform platform;
  UiWindow runtime{[]() -> View {
    ThemeDefinition definition;
    if (invalid_glass_style == 2) {
      ComboBoxStyle style = ComboBoxStyle::Default();
      style.popup_backdrop_effect = BackdropEffect{.blur_radius = -1.0F};
      definition.Set(style);
      return Theme {
        definition,
        ComboBox(TextEditingValue::FromText("Content"), {"First", "Second"}),
      };
    }
    SelectStyle style = SelectStyle::Default();
    if (invalid_glass_style == 0) {
      style.backdrop_effect = BackdropEffect{.blur_radius = -1.0F};
    } else {
      style.popup_backdrop_effect = BackdropEffect{.blur_radius = -1.0F};
    }
    definition.Set(style);
    return Theme {
      definition,
      Select(std::vector<std::string>{"First", "Second"}, 0, [](const std::string& label) { return Text(label); }),
    };
  }, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  REQUIRE_THROWS_AS(runtime.BuildRenderFrame(), std::invalid_argument);
}

TEST_CASE("GlassPresentationStylesRejectInvalidEffectsBeforeAttachingLayers", "[glass]") {
  invalid_glass_dialog.reset();
  invalid_glass_menu.reset();
  SECTION("Dialog") {
    invalid_glass_presentation = 0;
  }
  SECTION("Menu") {
    invalid_glass_presentation = 1;
  }
  SECTION("Tooltip") {
    invalid_glass_presentation = 2;
  }
  TestPlatform platform;
  UiWindow runtime{[]() -> View {
    ThemeDefinition definition;
    const BackdropEffect invalid_effect{.blur_radius = -1.0F};
    if (invalid_glass_presentation == 0) {
      DialogStyle style = DialogStyle::Default();
      style.backdrop_effect = invalid_effect;
      definition.Set(style);
    } else if (invalid_glass_presentation == 1) {
      MenuStyle style = MenuStyle::Default();
      style.backdrop_effect = invalid_effect;
      definition.Set(style);
    } else {
      TooltipStyle style = TooltipStyle::Default();
      style.backdrop_effect = invalid_effect;
      definition.Set(style);
    }
    return Theme {
      definition,
      Scope([]() -> View {
        if (invalid_glass_presentation == 0) {
          invalid_glass_dialog = UseDialog();
        } else if (invalid_glass_presentation == 1) {
          invalid_glass_menu = UseMenu();
        } else {
          return Text("Target").With(Tooltip("Invalid tooltip"));
        }
        return Text("Target");
      }),
    };
  }, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  if (invalid_glass_presentation == 2) {
    REQUIRE_THROWS_AS(runtime.BuildRenderFrame(), std::invalid_argument);
    return;
  }
  runtime.BuildRenderFrame();
  if (invalid_glass_presentation == 0) {
    REQUIRE(invalid_glass_dialog.has_value());
    REQUIRE_THROWS_AS(invalid_glass_dialog->Show("Invalid dialog", "Message", "OK"), std::invalid_argument);
  } else {
    REQUIRE(invalid_glass_menu.has_value());
    REQUIRE_THROWS_AS(invalid_glass_menu->ShowAt({10.0F, 10.0F}, {MenuItem("Invalid menu", [] {})}),
        std::invalid_argument);
  }
  const auto& frame = runtime.BuildRenderFrame();
  REQUIRE(SurfaceCommands(*frame.scene.root).empty());
  REQUIRE(FindMountedText(*runtime.RootNode(), "Invalid dialog") == nullptr);
  REQUIRE(FindMountedText(*runtime.RootNode(), "Invalid menu") == nullptr);
}

} // namespace huxerui::test
