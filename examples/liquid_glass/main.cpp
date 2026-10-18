#include <algorithm>
#include <utility>
#include <vector>

#include <huxerui/huxerui.h>

namespace {

using namespace huxerui;

constexpr Color secondary_text = Color::Rgb(224, 232, 247);
constexpr Color outline = Color::Rgb(255, 255, 255, 0.25F);
constexpr BackdropEffect surface_effect{.blur_radius = 18.0F, .refraction = 5.0F, .highlight = 0.24F};

ButtonStyle GlassButtonStyle() {
  ButtonStyle style = ButtonStyle::Default();
  style.background = Color::Rgb(255, 255, 255, 0.12F);
  style.disabled_background = Color::Rgb(255, 255, 255, 0.06F);
  style.label_style = TextStyle{Font::System(14.0F).WithWeight(FontWeight::SemiBold), Color::White()};
  style.disabled_label = Color::Rgb(255, 255, 255, 0.45F);
  style.minimum_height = 48.0F;
  style.corner_radii = CornerRadii{24.0F};
  style.backdrop_effect = surface_effect;
  return style;
}

ThemeDefinition GlassTheme() {
  ThemeDefinition definition = FlatDarkThemeDefinition();
  definition.Set(GlassButtonStyle());
  TextFieldStyle field = TextFieldStyle::Default();
  field.variant = TextFieldVariant::Outlined;
  field.text_style = TextStyle{Font::System(16.0F), Color::White()};
  field.label_style = TextStyle{Font::System(14.0F), secondary_text};
  field.floating_label_style = TextStyle{Font::System(12.0F), secondary_text};
  field.placeholder_style = TextStyle{Font::System(16.0F), Color::Rgb(255, 255, 255, 0.55F)};
  field.caret = Color::White();
  field.selection = Color::Rgb(141, 190, 255, 0.4F);
  field.outlined.background = Color::Rgb(255, 255, 255, 0.08F);
  field.outlined.border = outline;
  field.outlined.hovered_border = Color::Rgb(255, 255, 255, 0.5F);
  field.outlined.focused_border = Color::Rgb(188, 217, 255);
  field.outlined.corner_radii = CornerRadii{14.0F};
  field.outlined.minimum_height = 52.0F;
  field.outlined.backdrop_effect = surface_effect;
  definition.Set(field);
  DialogStyle dialog = DialogStyle::Default();
  dialog.background = Color::Rgb(24, 32, 61, 0.55F);
  dialog.title_style = TextStyle{Font::System(20.0F).WithWeight(FontWeight::Bold), Color::White()};
  dialog.message_style = TextStyle{Font::System(14.0F), secondary_text};
  dialog.corner_radii = CornerRadii{24.0F};
  dialog.backdrop_effect = surface_effect;
  definition.Set(dialog);
  return definition;
}

View ColorfulBackground(bool moved) {
  return Stack {
    Canvas([](PaintContext& paint, Size size) {
      paint.DrawRect(
          {0.0F, 0.0F, size.width, size.height},
          LinearGradient{
              .start = {0.0F, 0.0F},
              .end = {1.0F, 1.0F},
              .stops = {{0.0F, Color::Rgb(20, 38, 80)}, {1.0F, Color::Rgb(44, 22, 75)}},
          }
      );
    }),
    Canvas([](PaintContext& paint, Size size) {
      const float radius = std::min(size.width, size.height) * 0.3F;
      paint.DrawCircle({size.width * 0.15F, size.height * 0.24F}, radius, Color::Rgb(48, 149, 206));
      paint.DrawCircle({size.width * 0.8F, size.height * 0.42F}, radius, Color::Rgb(181, 83, 158));
      paint.DrawCircle({size.width * 0.5F, size.height * 0.9F}, radius, Color::Rgb(63, 150, 139));
      for (int index = 0; index < 12; ++index) {
        const float y = size.height * static_cast<float>(index) / 11.0F;
        paint.DrawLine({0.0F, y}, {size.width, y + 70.0F}, Color::Rgb(255, 255, 255, 0.3F),
                       StrokeStyle{.width = 2.0F});
      }
    }).With(Offset{AnimateTo(moved ? Point{48.0F, -32.0F} : Point{}, TweenSpec{0.7, Easing::EaseInOut})}),
  }.With(Align(HorizontalAlignment::Stretch, VerticalAlignment::Stretch), Semantics{.hidden = true});
}

View FrostedPanel() {
  return Column {
    Text("Frosted glass", TextRole::Title),
    Text("The backdrop is blurred while text and controls stay sharp.").With(Foreground(secondary_text)),
    Text("Scroll this page or move the background to see the material update.")
        .With(Foreground(secondary_text)),
  }.With(
      Spacing(12.0F),
      Padding(24.0F),
      BackdropBlur{18.0F},
      Background{Color::Rgb(255, 255, 255, 0.1F)},
      Border{.color = outline},
      CornerRadius{24.0F},
      CrossAlign(CrossAxisAlignment::Stretch)
  );
}

View GlassPopup(PopupContext popup) {
  return Column {
    Text("Floating glass", TextRole::Title),
    Text("This anchored surface samples the page beneath it.").With(Foreground(secondary_text)),
    Button("Close").OnClick([popup] { popup.Dismiss(); }).With(LiquidGlass{}),
  }.With(
      Frame{.max_width = 280.0F},
      Padding(20.0F),
      Spacing(14.0F),
      BackdropFilter{surface_effect},
      Background{Color::Rgb(24, 32, 61, 0.35F)},
      Border{.color = outline},
      CornerRadius{20.0F},
      CrossAlign(CrossAxisAlignment::Stretch)
  );
}

View FusingButtons(bool fused, State<int> activations, bool compact) {
  ButtonStyle buttons = GlassButtonStyle();
  buttons.background = Color::Transparent();
  buttons.backdrop_effect.reset();
  ThemeDefinition definition;
  definition.Set(buttons);
  const float width = compact ? 92.0F : 124.0F;
  return Theme {
    std::move(definition),
    Row {
      Button("Left")
          .OnClick([activations] { activations += 1; })
          .With(
              Frame{.width = width},
              Offset{AnimateTo(fused ? Point{24.0F, 0.0F} : Point{}, TweenSpec{0.5, Easing::EaseInOut})}
          )
          .Key("left-glass-button"),
      Button("Right")
          .OnClick([activations] { activations += 1; })
          .With(
              Frame{.width = width},
              Offset{AnimateTo(fused ? Point{-24.0F, 0.0F} : Point{}, TweenSpec{0.5, Easing::EaseInOut})}
          )
          .Key("right-glass-button"),
    }.With(
        Spacing(48.0F),
        Frame{.height = 88.0F},
        MainAlign(MainAxisAlignment::Center),
        CrossAlign(CrossAxisAlignment::Center),
        GlassGroup{}
    ),
  };
}

[[huxerui::composable]]
View GlassPage(State<bool> diagnostics) {
  const bool compact = UseViewportClass() == ViewportClass::Compact;
  const auto moved = UseState(false);
  const auto fused = UseState(false);
  const auto activations = UseState(0);
  const auto note = UseState(TextEditingValue::FromText("Glass keeps editing state intact."));
  const auto popup = UsePopup();
  const auto dialog = UseDialog();
  return Stack {
    ColorfulBackground(moved.Get()),
    ScrollView {
      Stack {
        Column {
          Text("Liquid glass").With(FontSize(compact ? 32.0F : 44.0F)),
          Text("Background blur, optical edges, and independent interactive surfaces.")
              .With(Foreground(secondary_text)),
          Flow {
            Button("Performance levels").OnClick([diagnostics] { diagnostics = true; }),
            Button("Move background").OnClick([moved] { moved = !moved.Get(); }).With(LiquidGlass{}),
            Button(fused.Get() ? "Separate glass" : "Fuse glass")
                .OnClick([fused] { fused = !fused.Get(); })
                .With(LiquidGlass{}),
            Button("Open popup").OnClick([popup] { popup.Show(GlassPopup); }).With(popup.Anchor(), LiquidGlass{}),
            Button("Open dialog").OnClick([dialog] {
              dialog.Show("Glass dialog", "The dialog material comes from its Theme style.", "Done");
            }).With(LiquidGlass{}),
          }.With(Spacing(12.0F)),
          FrostedPanel(),
          Text("Liquid surfaces", TextRole::Title),
          Text("Hover or press the controls to change their edge lighting and refraction.")
              .With(Foreground(secondary_text)),
          FusingButtons(fused.Get(), activations, compact),
          Text::Format("Independent button activations: {}", activations.Get()).With(Foreground(secondary_text)),
          TextField(note)
              .Label("Glass note")
              .Placeholder("Type on the glass surface")
              .Variant(TextFieldVariant::Outlined)
              .OnChanged([note](const TextEditingValue& value) { note = value; })
              .Key("glass-editor"),
          Text("The editor material belongs to the field's outlined variant. Its label remains outside the effect.")
              .With(Foreground(secondary_text)),
          Spacer().With(Frame{.height = 96.0F}),
        }.With(Spacing(20.0F), Frame{.max_width = 760.0F}, CrossAlign(CrossAxisAlignment::Stretch)),
      }.With(Padding(compact ? 16.0F : 36.0F), Align(HorizontalAlignment::Center, VerticalAlignment::Start)),
    }.With(ScrollBar()),
  }.With(Align(HorizontalAlignment::Stretch, VerticalAlignment::Stretch));
}

View BenchmarkPanel(int level, int index, BackdropBlurQuality quality) {
  View panel = Column {
    Text::Format("Surface {}", index + 1).With(FontSize(22.0F)),
    Text("The same geometry, foreground and fill are used at every level."),
    Text("Only the background processing changes.").With(Foreground(secondary_text)),
  }.With(
      Frame{.height = 180.0F},
      Padding(24.0F),
      Spacing(12.0F),
      Background{Color::Rgb(255, 255, 255, 0.12F)},
      Border{.color = outline},
      CornerRadius{24.0F},
      CrossAlign(CrossAxisAlignment::Stretch)
  );
  if (level == 4) {
    panel = Stack {
      Canvas([quality](PaintContext& paint, Size size) {
        const float half_width = std::max(0.0F, (size.width - 12.0F) * 0.5F);
        paint.DrawBackdrop(std::vector<BackdropShape>{
          {{0, 0, half_width, size.height}, CornerRadii{24}},
          {{half_width + 12, 0, half_width, size.height}, CornerRadii{24}},
        }, BackdropEffect{
            .blur_radius = 18,
            .refraction = 5,
            .highlight = 0.24F,
            .saturation = 1.1F,
            .brightness = 0.95F,
            .fusion_radius = 32,
            .blur_quality = quality,
        });
      }),
      std::move(panel),
    }.With(Frame{.height = 180.0F}, Align(HorizontalAlignment::Stretch, VerticalAlignment::Stretch));
  } else if (level != 0) {
    panel = std::move(panel).With(BackdropFilter{BackdropEffect{
        .blur_radius = level >= 2 ? 18.0F : 0.0F,
        .refraction = level == 3 ? 5.0F : 0.0F,
        .highlight = level == 3 ? 0.24F : 0.0F,
        .saturation = level == 3 ? 1.1F : 1.0F,
        .brightness = level == 3 ? 0.95F : 1.0F,
        .blur_quality = quality,
    }});
  }
  return std::move(panel).Key(index);
}

[[huxerui::composable]]
View BenchmarkPage(State<bool> diagnostics) {
  const auto level = UseState(0);
  const auto fast = UseState(false);
  const auto quality = fast.Get() ? BackdropBlurQuality::Fast : BackdropBlurQuality::Precise;
  const auto moving_source = UseState(false);
  const auto scroll = UseScrollController();
  View background = ColorfulBackground(false);
  View surfaces = Column {
    BenchmarkPanel(level.Get(), 0, quality),
    BenchmarkPanel(level.Get(), 1, quality),
    BenchmarkPanel(level.Get(), 2, quality),
    BenchmarkPanel(level.Get(), 3, quality),
    BenchmarkPanel(level.Get(), 4, quality),
    BenchmarkPanel(level.Get(), 5, quality),
  }.With(Padding(20.0F), Spacing(40.0F), CrossAlign(CrossAxisAlignment::Stretch));
  View content = moving_source.Get()
      ? Stack {background, surfaces}.With(Align(HorizontalAlignment::Stretch, VerticalAlignment::Stretch))
      : surfaces;
  return Column {
    Column {
      Text("Glass performance levels", TextRole::Title),
      Flow {
        Button("L0 Plain").OnClick([level] { level = 0; }),
        Button("L1 Transfer").OnClick([level] { level = 1; }),
        Button("L2 Blur").OnClick([level] { level = 2; }),
        Button("L3 Optics").OnClick([level] { level = 3; }),
        Button("L4 Fusion").OnClick([level] { level = 4; }),
        Button(moving_source.Get() ? "Source: scrolling" : "Source: fixed")
            .OnClick([moving_source, scroll] { scroll.ScrollTo(0); moving_source = !moving_source.Get(); })
            .With(Frame{.width = 180.0F}),
        Button(fast.Get() ? "Quality: fast" : "Quality: precise")
            .OnClick([fast, scroll] { scroll.ScrollTo(0); fast = !fast.Get(); })
            .With(Frame{.width = 180.0F}),
        Button("Reset scroll").OnClick([scroll] { scroll.ScrollTo(0); }),
        Button("Glass demo").OnClick([diagnostics] { diagnostics = false; }),
      }.With(Spacing(8.0F)),
      Text::Format("Level: {}", level.Get()),
    }.With(Padding(12.0F), Spacing(8.0F), Background{Color::Rgb(24, 32, 61)}),
    Stack {
      moving_source.Get() ? Spacer() : background,
      ScrollView {content}.Controller(scroll).With(ScrollBar()),
    }.With(Grow(), Align(HorizontalAlignment::Stretch, VerticalAlignment::Stretch)),
  }.With(CrossAlign(CrossAxisAlignment::Stretch));
}

View App() {
  const auto diagnostics = UseState(false);
  if (diagnostics.Get()) {
    return Theme {FlatDarkThemeDefinition(), BenchmarkPage(diagnostics)};
  }
  return Theme {GlassTheme(), GlassPage(diagnostics)};
}

} // namespace

const huxerui::Application application{
    App,
    {
        .window = {
            .title = "HuxerUI Liquid Glass",
            .initial_size = {900.0F, 760.0F},
            .minimum_size = huxerui::Size{340.0F, 480.0F},
            .content_mode = huxerui::WindowContentMode::SafeArea,
        },
    }
};
