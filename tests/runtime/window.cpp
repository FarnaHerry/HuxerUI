#include "runtime_test_support.h"

#include <deque>
#include <limits>
#include <thread>

#include "application/window_internal.h"
#include "../../examples/desktop_pet/pet.h"
#include "../../examples/desktop_pet/pet_model.h"

namespace huxerui::test {

namespace {

constexpr Color status_color = Color::Rgb(18, 24, 32);
constexpr Color navigation_color = Color::Rgb(246, 248, 252);
int window_compositions = 0;
int title_bar_compositions = 0;
bool use_light_status_bar = false;
Color title_bar_background = Color::Rgb(48, 32, 96);
std::optional<WindowHandle> window_handle;
bool handle_minimize_request = false;
bool handle_close_request = false;
int minimize_requests = 0;
int close_requests = 0;

View WindowContentApp() {
  ++window_compositions;
  return Column {
    Text("window content"),
  }.With(CrossAlign(CrossAxisAlignment::Stretch));
}

View SelectiveSafeAreaApp() {
  return Column {
    Text("selective content").With(huxerui::SafeAreaPadding{}),
  }.With(
      Padding{EdgeInsets{.top = 1.0F, .right = 2.0F, .bottom = 3.0F, .left = 4.0F}},
      huxerui::SafeAreaPadding{.bottom = false},
      CrossAlign(CrossAxisAlignment::Stretch)
  );
}

View WindowAppearanceApp() {
  return Stack {}.With(
      Background(status_color),
      huxerui::SystemBarsAppearance{
          .status_bar_background = status_color,
          .navigation_bar_background = navigation_color,
      }
  );
}

View MutableWindowAppearanceApp() {
  return Stack {}.With(
      Background(status_color),
      huxerui::SystemBarsAppearance{
          .status_bar_background = use_light_status_bar ? Color::White() : status_color,
          .navigation_bar_background = navigation_color,
      }
  );
}

View WindowTitleBarApp() {
  ++title_bar_compositions;
  return Column {
    WindowTitleBar {
      Text("Title").With(Frame{.width = 40.0F, .height = 20.0F}),
      Button("Action").OnClick([] {}).With(Frame{.width = 60.0F, .height = 24.0F}),
    }.With(Padding(4.0F), Spacing(10.0F)),
    Spacer().With(Grow(1.0F)),
  }.With(CrossAlign(CrossAxisAlignment::Stretch));
}

View WindowCommandsApp() {
  window_handle = UseWindow();
  return Spacer();
}

View WindowRequestsApp() {
  window_handle = UseWindow();
  window_handle->OnMinimizeRequest([] {
    ++minimize_requests;
    return handle_minimize_request;
  });
  window_handle->OnCloseRequest([] {
    ++close_requests;
    return handle_close_request;
  });
  return Spacer();
}

View MutableTitleBarBackgroundApp() {
  return Column {
    WindowTitleBar {Text("Title")}.With(Background(title_bar_background)),
    Spacer().With(Grow(1.0F)),
  }.With(CrossAlign(CrossAxisAlignment::Stretch));
}

} // namespace

TEST_CASE("WindowSafeAreaModeConstrainsApplicationContent") {
  window_compositions = 0;
  TestPlatform platform;
  UiWindow runtime(WindowContentApp, platform);
  runtime.SetWindowMetrics({
      .viewport = {200.0F, 120.0F},
      .safe_area = {.top = 20.0F, .right = 12.0F, .bottom = 10.0F, .left = 8.0F},
  });
  runtime.BuildFrame();

  const detail::MountedNode* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->PresentationBounds().x == 8.0F);
  REQUIRE(root->PresentationBounds().y == 20.0F);
  REQUIRE((root->LayoutSize() == Size{180.0F, 90.0F}));
  REQUIRE(window_compositions == 1);

  runtime.SetWindowMetrics({
      .viewport = {200.0F, 120.0F},
      .safe_area = {.top = 24.0F, .right = 12.0F, .bottom = 10.0F, .left = 8.0F},
  });
  runtime.BuildFrame();
  REQUIRE(window_compositions == 1);
  REQUIRE(root->PresentationBounds().y == 24.0F);
  REQUIRE((root->LayoutSize() == Size{180.0F, 86.0F}));
}

TEST_CASE("WindowEdgeToEdgeModeLetsViewsConsumeSelectedInsets") {
  TestPlatform platform;
  AppOptions options;
  options.show_debug_overlay = false;
  options.window.content_mode = WindowContentMode::EdgeToEdge;
  UiWindow runtime(SelectiveSafeAreaApp, platform, options);
  runtime.SetWindowMetrics({
      .viewport = {200.0F, 120.0F},
      .safe_area = {.top = 20.0F, .right = 12.0F, .bottom = 10.0F, .left = 8.0F},
  });
  runtime.BuildFrame();

  const detail::MountedNode* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->LayoutOffset() == Point{});
  REQUIRE((root->LayoutSize() == Size{200.0F, 120.0F}));
  REQUIRE((root->resolved_padding == EdgeInsets{.top = 21.0F, .right = 14.0F, .bottom = 3.0F, .left = 12.0F}));
  REQUIRE((root->ChildAt(0).LayoutOffset() == Point{12.0F, 21.0F}));
  REQUIRE(
      (static_cast<const detail::MountedNode&>(root->ChildAt(0)).resolved_padding ==
       EdgeInsets{.bottom = 10.0F})
  );
}

TEST_CASE("WindowAppearanceResolvesAutomaticSystemBarBrightnessOnce") {
  TestPlatform platform;
  UiWindow runtime(WindowAppearanceApp, platform);
  runtime.SetWindowMetrics({
      .viewport = {200.0F, 120.0F},
      .safe_area = {.top = 20.0F, .bottom = 10.0F},
  });
  const FlattenedScene& scene = runtime.BuildFrame();

  REQUIRE(platform.system_bars_updates == 1);
  REQUIRE(
      (platform.system_bar_brightness == std::pair{
                                             SystemBarContentBrightness::Light,
                                             SystemBarContentBrightness::Dark,
                                         })
  );
  REQUIRE(FindRect(scene, Rect{0.0F, 0.0F, 200.0F, 20.0F}) != nullptr);
  REQUIRE(FindRect(scene, Rect{0.0F, 110.0F, 200.0F, 10.0F}) != nullptr);

  runtime.BuildFrame();
  REQUIRE(platform.system_bars_updates == 1);
}

TEST_CASE("WindowAppearanceChangesDoNotRerecordApplicationPaint") {
  use_light_status_bar = false;
  TestPlatform platform;
  UiWindow runtime(MutableWindowAppearanceApp, platform);
  runtime.SetWindowMetrics({
      .viewport = {200.0F, 120.0F},
      .safe_area = {.top = 20.0F, .bottom = 10.0F},
  });
  runtime.BuildFrame();

  const detail::MountedNode* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const std::uint64_t content_revision = root->render_node.content.Revision();

  use_light_status_bar = true;
  runtime.InvalidateRoot();
  runtime.BuildFrame();

  REQUIRE(runtime.RootNode() == root);
  REQUIRE(root->render_node.content.Revision() == content_revision);
  REQUIRE(platform.system_bars_updates == 2);
  REQUIRE(platform.system_bar_brightness->first == SystemBarContentBrightness::Dark);
}

TEST_CASE("WindowTitleBarReservesSystemControlsWithoutRecomposition") {
  title_bar_compositions = 0;
  TestPlatform platform;
  UiWindow runtime(WindowTitleBarApp, platform);
  runtime.SetWindowMetrics({
      .viewport = {300.0F, 100.0F},
      .title_bar = WindowTitleBarMetrics{.height = 40.0F, .left_inset = 20.0F, .right_inset = 60.0F},
  });
  runtime.BuildFrame();

  const detail::MountedNode* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const auto& title_bar = static_cast<const detail::MountedNode&>(root->ChildAt(0));
  REQUIRE((title_bar.LayoutSize() == Size{300.0F, 40.0F}));
  REQUIRE((title_bar.ChildAt(0).LayoutOffset() == Point{24.0F, 10.0F}));
  REQUIRE((title_bar.ChildAt(1).LayoutOffset() == Point{74.0F, 8.0F}));
  REQUIRE(title_bar_compositions == 1);

  runtime.SetWindowMetrics({
      .viewport = {300.0F, 100.0F},
      .title_bar = WindowTitleBarMetrics{.height = 48.0F, .left_inset = 30.0F, .right_inset = 60.0F},
  });
  runtime.BuildFrame();

  REQUIRE(runtime.RootNode() == root);
  REQUIRE((title_bar.LayoutSize() == Size{300.0F, 48.0F}));
  REQUIRE((title_bar.ChildAt(0).LayoutOffset() == Point{34.0F, 14.0F}));
  REQUIRE(title_bar_compositions == 1);
}

TEST_CASE("WindowTitleBarDragRegionDefersToInteractiveChildren") {
  TestPlatform platform;
  UiWindow runtime(WindowTitleBarApp, platform);
  runtime.SetWindowMetrics({
      .viewport = {300.0F, 100.0F},
      .title_bar = WindowTitleBarMetrics{.height = 40.0F, .left_inset = 20.0F, .right_inset = 60.0F},
  });
  runtime.BuildFrame();

  REQUIRE(runtime.IsWindowDragRegion({30.0F, 20.0F}));
  REQUIRE_FALSE(runtime.IsWindowDragRegion({90.0F, 20.0F}));
  REQUIRE(runtime.IsWindowDragRegion({180.0F, 20.0F}));

  runtime.SetWindowMetrics({.viewport = {300.0F, 100.0F}});
  runtime.BuildFrame();
  REQUIRE_FALSE(runtime.IsWindowDragRegion({30.0F, 20.0F}));
}

TEST_CASE("WindowMetricsRejectInvalidValues") {
  TestPlatform platform;
  UiWindow runtime(WindowContentApp, platform);
  REQUIRE_THROWS_AS(runtime.SetWindowMetrics({.viewport = {-1.0F, 100.0F}}), std::invalid_argument);
  REQUIRE_THROWS_AS(
      runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}, .safe_area = {.top = -1.0F}}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      runtime.SetWindowMetrics({
          .viewport = {100.0F, 100.0F},
          .title_bar = WindowTitleBarMetrics{.height = 101.0F},
      }),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      runtime.SetWindowMetrics({
          .viewport = {100.0F, 100.0F},
          .title_bar = WindowTitleBarMetrics{.left_inset = 60.0F, .right_inset = 50.0F},
      }),
      std::invalid_argument
  );

  AppOptions invalid_options;
  const auto construct_window = [&] {
    TestPlatform invalid_platform;
    UiWindow window(WindowContentApp, invalid_platform, invalid_options);
  };
  invalid_options.show_debug_overlay = false;
  invalid_options.window.content_mode = static_cast<WindowContentMode>(99);
  REQUIRE_THROWS_AS(construct_window(), std::invalid_argument);
  invalid_options.window.content_mode = WindowContentMode::SafeArea;
  invalid_options.window.chrome_mode = static_cast<WindowChromeMode>(99);
  REQUIRE_THROWS_AS(construct_window(), std::invalid_argument);
  invalid_options.window.chrome_mode = WindowChromeMode::System;
  invalid_options.window.title_bar_height = 0.0F;
  REQUIRE_THROWS_AS(construct_window(), std::invalid_argument);
  invalid_options.window.title_bar_height = 40.0F;
  invalid_options.window.initial_size.width = 0.0F;
  REQUIRE_THROWS_AS(construct_window(), std::invalid_argument);
  invalid_options.window.initial_size = {520.0F, std::numeric_limits<float>::infinity()};
  REQUIRE_THROWS_AS(construct_window(), std::invalid_argument);
  invalid_options.window.initial_size = {520.0F, 360.0F};
  invalid_options.window.minimum_size = Size{0.0F, 240.0F};
  REQUIRE_THROWS_AS(construct_window(), std::invalid_argument);
  invalid_options.window.minimum_size = Size{320.0F, -1.0F};
  REQUIRE_THROWS_AS(construct_window(), std::invalid_argument);
  invalid_options.window.minimum_size = Size{std::numeric_limits<float>::quiet_NaN(), 240.0F};
  REQUIRE_THROWS_AS(construct_window(), std::invalid_argument);
  invalid_options.window.minimum_size = Size{320.0F, std::numeric_limits<float>::infinity()};
  REQUIRE_THROWS_AS(construct_window(), std::invalid_argument);
  invalid_options.window.minimum_size = Size{640.0F, 480.0F};
  REQUIRE_NOTHROW(construct_window());
  invalid_options.window.initial_position = Point{std::numeric_limits<float>::quiet_NaN(), 0.0F};
  REQUIRE_THROWS_AS(construct_window(), std::invalid_argument);
  invalid_options.window.initial_position = Point{0.0F, std::numeric_limits<float>::infinity()};
  REQUIRE_THROWS_AS(construct_window(), std::invalid_argument);
  invalid_options.window.initial_position = Point{-320.0F, -200.0F};
  REQUIRE_NOTHROW(construct_window());
  TestPlatform invalid_appearance_platform;
  UiWindow invalid_appearance{
      +[]() -> View {
        return Stack {}.With(huxerui::SystemBarsAppearance{
            .status_bar_content = static_cast<SystemBarContentBrightness>(99),
        });
      },
      invalid_appearance_platform,
  };
  invalid_appearance.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  REQUIRE_THROWS_AS(invalid_appearance.BuildFrame(), std::invalid_argument);
}

TEST_CASE("WindowHandleForwardsCommandsToThePlatform") {
  window_handle.reset();
  TestPlatform platform;
  UiWindow runtime(WindowCommandsApp, platform);
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  REQUIRE(window_handle.has_value());
  window_handle->Show();
  window_handle->Hide();
  window_handle->Activate();
  window_handle->Minimize();
  window_handle->Maximize();
  window_handle->Restore();
  window_handle->ToggleMaximize();
  window_handle->SetFullscreen(true);
  window_handle->SetFullscreen(false);
  window_handle->Close();

  const std::vector expected{
      WindowCommand::Show,
      WindowCommand::Hide,
      WindowCommand::Activate,
      WindowCommand::Minimize,
      WindowCommand::Maximize,
      WindowCommand::Restore,
      WindowCommand::ToggleMaximize,
      WindowCommand::EnterFullscreen,
      WindowCommand::ExitFullscreen,
      WindowCommand::Close,
  };
  REQUIRE(platform.window_commands == expected);
}

TEST_CASE("WindowInitialSizeRespectsEachConfiguredMinimumDimension") {
  WindowOptions options;
  options.initial_size = {800.0F, 360.0F};
  options.minimum_size = Size{640.0F, 480.0F};
  REQUIRE((detail::ResolveInitialWindowSize(options) == Size{800.0F, 480.0F}));

  options.minimum_size.reset();
  REQUIRE(detail::ResolveInitialWindowSize(options) == options.initial_size);
}

TEST_CASE("WindowPositionConversionRejectsOverflowAndPreservesNegativeCoordinates") {
  const int minimum = std::numeric_limits<int>::min();
  const int maximum = std::numeric_limits<int>::max();
  REQUIRE(detail::ResolveWindowPositionCoordinate(-320.5, minimum, maximum) == -321);
  REQUIRE(detail::ResolveWindowPositionCoordinate(minimum, minimum, maximum) == minimum);
  REQUIRE(detail::ResolveWindowPositionCoordinate(maximum, minimum, maximum) == maximum);
  REQUIRE_THROWS_AS(detail::ResolveWindowPositionCoordinate(static_cast<double>(maximum) + 1, minimum, maximum),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(detail::ResolveWindowPositionCoordinate(static_cast<double>(minimum) - 1, minimum, maximum),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(detail::ResolveWindowPositionCoordinate(std::numeric_limits<double>::infinity(), minimum, maximum),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(detail::ResolveWindowPositionCoordinate(32768, -32768, 32767), std::invalid_argument);
  REQUIRE(detail::ResolveWindowPositionCoordinate(-32768, -32768, 32767) == -32768);
}

TEST_CASE("Window request handlers independently suppress platform and public defaults") {
  window_handle.reset();
  handle_minimize_request = true;
  handle_close_request = false;
  minimize_requests = 0;
  close_requests = 0;
  TestPlatform platform;
  UiWindow runtime(WindowRequestsApp, platform);
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  REQUIRE(runtime.HandleWindowRequest(WindowCommand::Minimize));
  REQUIRE_FALSE(runtime.HandleWindowRequest(WindowCommand::Close));
  REQUIRE(minimize_requests == 1);
  REQUIRE(close_requests == 1);

  window_handle->Minimize();
  window_handle->Close();
  REQUIRE(minimize_requests == 2);
  REQUIRE(close_requests == 2);
  REQUIRE(platform.window_commands == std::vector<WindowCommand>{WindowCommand::Close});
}

TEST_CASE("CustomWindowChromeProvidesStandardCaptionControls") {
  TestPlatform platform;
  AppOptions options;
  options.show_debug_overlay = false;
  options.window.chrome_mode = WindowChromeMode::Custom;
  options.window.caption_labels = {"Minimize", "Maximize or restore", "Close"};
  UiWindow runtime(WindowCommandsApp, platform, options);
  runtime.SetWindowMetrics({
      .viewport = {300.0F, 100.0F},
      .title_bar = WindowTitleBarMetrics{.height = 32.0F, .right_inset = 138.0F},
  });
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 0, {277.0F, 16.0F}, huxerui::PointerDeviceKind::Mouse});
  runtime.HandlePointerEvent({PointerEventType::Up, 0, {277.0F, 16.0F}, huxerui::PointerDeviceKind::Mouse});

  REQUIRE(platform.window_commands == std::vector<WindowCommand>{WindowCommand::Close});
  REQUIRE_FALSE(runtime.IsWindowDragRegion({277.0F, 16.0F}));
}

TEST_CASE("CustomWindowChromeCollapsesControlsWhenPlatformMetricsDisappear") {
  TestPlatform platform;
  AppOptions options;
  options.show_debug_overlay = false;
  options.window.chrome_mode = WindowChromeMode::Custom;
  options.window.caption_labels = {"Minimize", "Maximize or restore", "Close"};
  UiWindow runtime(WindowCommandsApp, platform, options);
  runtime.SetWindowMetrics({
      .viewport = {300.0F, 100.0F},
      .title_bar = WindowTitleBarMetrics{.height = 32.0F, .right_inset = 138.0F},
  });
  runtime.BuildFrame();

  runtime.SetWindowMetrics({.viewport = {300.0F, 100.0F}});
  const FlattenedScene& collapsed = runtime.BuildFrame();
  runtime.HandlePointerEvent({PointerEventType::Down, 0, {277.0F, 16.0F}, PointerDeviceKind::Mouse});
  runtime.HandlePointerEvent({PointerEventType::Up, 0, {277.0F, 16.0F}, PointerDeviceKind::Mouse});

  REQUIRE(platform.window_commands.empty());
  REQUIRE(std::ranges::none_of(collapsed.Commands(), [](const PaintCommand& command) {
    return std::holds_alternative<StrokePathCommand>(command);
  }));
  REQUIRE(std::ranges::none_of(runtime.LastCommit().semantic_frame->nodes, [](const SemanticNode& node) {
    return node.label == "Minimize" || node.label == "Maximize or restore" || node.label == "Close";
  }));
}

TEST_CASE("CustomWindowChromeRerecordsCaptionGlyphsWhenTitleBarBackgroundChanges") {
  title_bar_background = Color::Rgb(48, 32, 96);
  TestPlatform platform;
  AppOptions options;
  options.show_debug_overlay = false;
  options.window.chrome_mode = WindowChromeMode::Custom;
  options.window.caption_labels = {"Minimize", "Maximize or restore", "Close"};
  UiWindow runtime(MutableTitleBarBackgroundApp, platform, options);
  runtime.SetWindowMetrics({
      .viewport = {300.0F, 100.0F},
      .title_bar = WindowTitleBarMetrics{.height = 32.0F, .right_inset = 138.0F},
  });
  const FlattenedScene& dark = runtime.BuildFrame();
  REQUIRE(std::ranges::count_if(dark.Commands(), [](const PaintCommand& command) {
            const auto* path = std::get_if<StrokePathCommand>(&command);
            return path != nullptr && BrushIsColor(path->brush, Color::Rgb(245, 245, 245));
          }) == 3);

  title_bar_background = Color::White();
  runtime.InvalidateRoot();
  const FlattenedScene& light = runtime.BuildFrame();
  REQUIRE(std::ranges::count_if(light.Commands(), [](const PaintCommand& command) {
            const auto* path = std::get_if<StrokePathCommand>(&command);
            return path != nullptr && BrushIsColor(path->brush, Color::Rgb(32, 32, 32));
          }) == 3);
}

TEST_CASE("CustomWindowChromeUpdatesTheMaximizeGlyphFromPlatformState") {
  TestPlatform platform;
  AppOptions options;
  options.show_debug_overlay = false;
  options.window.chrome_mode = WindowChromeMode::Custom;
  options.window.caption_labels = {"Minimize", "Maximize or restore", "Close"};
  UiWindow runtime(WindowCommandsApp, platform, options);
  runtime.SetWindowMetrics({
      .viewport = {300.0F, 100.0F},
      .title_bar = WindowTitleBarMetrics{.height = 32.0F, .right_inset = 138.0F},
  });
  const FlattenedScene& restored = runtime.BuildFrame();
  Path maximize_path;
  maximize_path.MoveTo({18.0F, 11.0F})
      .LineTo({28.0F, 11.0F})
      .LineTo({28.0F, 21.0F})
      .LineTo({18.0F, 21.0F})
      .Close();
  REQUIRE(std::ranges::any_of(restored.Commands(), [&maximize_path](const PaintCommand& command) {
    const auto* path = std::get_if<StrokePathCommand>(&command);
    return path != nullptr && path->path == maximize_path;
  }));

  runtime.SetWindowMetrics({
      .viewport = {300.0F, 100.0F},
      .title_bar = WindowTitleBarMetrics{.height = 32.0F, .right_inset = 138.0F, .maximized = true},
  });
  const FlattenedScene& maximized = runtime.BuildFrame();
  Path restore_path;
  restore_path.MoveTo({20.0F, 11.0F})
      .LineTo({28.0F, 11.0F})
      .LineTo({28.0F, 19.0F})
      .MoveTo({18.0F, 13.0F})
      .LineTo({26.0F, 13.0F})
      .LineTo({26.0F, 21.0F})
      .LineTo({18.0F, 21.0F})
      .Close();
  REQUIRE(std::ranges::any_of(maximized.Commands(), [&restore_path](const PaintCommand& command) {
    const auto* path = std::get_if<StrokePathCommand>(&command);
    return path != nullptr && path->path == restore_path;
  }));
}

TEST_CASE("CustomWindowChromeUsesConfiguredCaptionLabels") {
  TestPlatform platform;
  AppOptions options;
  options.show_debug_overlay = false;
  options.window.chrome_mode = WindowChromeMode::Custom;
  options.window.caption_labels = {
      .minimize = "Minimize window",
      .toggle_maximize = "Toggle window size",
      .close = "Close window",
  };
  UiWindow runtime(WindowCommandsApp, platform, options);
  runtime.SetWindowMetrics({
      .viewport = {300.0F, 100.0F},
      .title_bar = WindowTitleBarMetrics{.height = 32.0F, .right_inset = 138.0F},
  });
  const FrameCommit& frame = runtime.BuildCommit();
  REQUIRE(frame.semantic_frame != nullptr);

  REQUIRE(std::ranges::any_of(frame.semantic_frame->nodes, [](const SemanticNode& node) {
    return node.label == "Minimize window";
  }));
  REQUIRE(std::ranges::any_of(frame.semantic_frame->nodes, [](const SemanticNode& node) {
    return node.label == "Toggle window size";
  }));
  REQUIRE(std::ranges::any_of(frame.semantic_frame->nodes, [](const SemanticNode& node) {
    return node.label == "Close window";
  }));
}

TEST_CASE("CustomWindowChromePaintsStateLayerBehindCaptionGlyph") {
  TestPlatform platform;
  AppOptions options;
  options.show_debug_overlay = false;
  options.window.chrome_mode = WindowChromeMode::Custom;
  options.window.caption_labels = {"Minimize", "Maximize or restore", "Close"};
  UiWindow runtime(WindowCommandsApp, platform, options);
  runtime.SetWindowMetrics({
      .viewport = {300.0F, 100.0F},
      .title_bar = WindowTitleBarMetrics{.height = 32.0F, .right_inset = 138.0F},
  });
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Move, 0, {277.0F, 16.0F}, huxerui::PointerDeviceKind::Mouse});
  runtime.BuildFrame();
  platform.AdvanceTime(0.1);
  const FlattenedScene& hovered = runtime.BuildFrame();

  const auto state_layer = std::find_if(hovered.Commands().begin(), hovered.Commands().end(), [](const auto& command) {
    const auto* rect = std::get_if<DrawRectCommand>(&command);
    return rect != nullptr && BrushIsColor(rect->brush, Color::Rgb(196, 43, 28, 0.9F));
  });
  REQUIRE(state_layer != hovered.Commands().end());
  REQUIRE(std::find_if(std::next(state_layer), hovered.Commands().end(), [](const auto& command) {
            return std::holds_alternative<StrokePathCommand>(command);
          }) != hovered.Commands().end());
}

TEST_CASE("WindowHandleBecomesInactiveAfterRuntimeDestruction") {
  window_handle.reset();
  TestPlatform platform;
  {
    UiWindow runtime(WindowCommandsApp, platform);
    runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
    runtime.BuildFrame();
  }

  REQUIRE(window_handle.has_value());
  window_handle->Close();
  REQUIRE(platform.window_commands.empty());
}

TEST_CASE("FloatingWindowRequestsReportUnavailableAndDisconnectWithoutRebinding") {
  window_handle.reset();
  TestPlatform platform;
  {
    UiWindow runtime(WindowCommandsApp, platform);
    runtime.SetWindowMetrics({.viewport = {200.0F, 200.0F}});
    runtime.BuildFrame();
    REQUIRE(window_handle.has_value());
    REQUIRE(window_handle->Capabilities() == WindowCapabilities{});
    REQUIRE(window_handle->SetTopmost(true) == WindowRequestResult::Unavailable);
    REQUIRE(window_handle->SetPosition({-100.0F, 20.0F}) == WindowRequestResult::Unavailable);
    REQUIRE(window_handle->SetClientSize({120.0F, 80.0F}) == WindowRequestResult::Unavailable);
    REQUIRE(window_handle->SetInputPassthrough(true) == WindowRequestResult::Unavailable);
    REQUIRE(window_handle->BeginMove() == WindowRequestResult::Unavailable);
    REQUIRE_FALSE(window_handle->WorkArea());
    REQUIRE_THROWS_AS(
        window_handle->SetPosition({std::numeric_limits<float>::infinity(), 0.0F}), std::invalid_argument
    );
    REQUIRE_THROWS_AS(window_handle->SetClientSize({0.0F, 80.0F}), std::invalid_argument);
    REQUIRE_THROWS_AS(window_handle->SetClientSize({120.0F, std::numeric_limits<float>::quiet_NaN()}),
        std::invalid_argument);
  }
  REQUIRE(window_handle->Capabilities() == WindowCapabilities{});
  REQUIRE(window_handle->SetTopmost(true) == WindowRequestResult::Disconnected);
  REQUIRE(window_handle->SetPosition({10.0F, 20.0F}) == WindowRequestResult::Disconnected);
  REQUIRE(window_handle->SetClientSize({100.0F, 80.0F}) == WindowRequestResult::Disconnected);
  REQUIRE(window_handle->SetInputPassthrough(false) == WindowRequestResult::Disconnected);
  REQUIRE(window_handle->BeginMove() == WindowRequestResult::Disconnected);
  REQUIRE_FALSE(window_handle->WorkArea());
}

TEST_CASE("FloatingWindowCreationRejectsUnavailablePoliciesAndInvalidConfiguration") {
  AppOptions options;
  SECTION("Utility presentation is unavailable") { options.window.presentation = WindowPresentation::Utility; }
  SECTION("Passive display is unavailable") { options.window.show_without_activation = true; }
  SECTION("Topmost is unavailable") { options.window.topmost = true; }
  SECTION("Transparent utility surfaces are unavailable") {
    options.window.presentation = WindowPresentation::Utility;
    options.window.transparent_surface = true;
  }
  TestPlatform platform;
  REQUIRE_THROWS_AS(UiWindow(WindowCommandsApp, platform, options), std::logic_error);

  WindowOptions invalid;
  invalid.transparent_surface = true;
  REQUIRE_THROWS_AS(detail::ValidateWindowOptions(invalid), std::invalid_argument);
  invalid.transparent_surface = false;
  invalid.presentation = static_cast<WindowPresentation>(999);
  REQUIRE_THROWS_AS(detail::ValidateWindowOptions(invalid), std::invalid_argument);
}

TEST_CASE("FloatingWindowRequestsPreserveNativeResultsAndRequireTheApplicationThread") {
  class NativePlatform final : public TestPlatform {
  public:
    int requests = 0;
    WindowCapabilities NativeWindowCapabilities() const override { return {.geometry = true}; }
    WindowRequestResult RequestWindowPosition(Point position) override {
      ++requests;
      REQUIRE((position == Point{-120.0F, 40.0F}));
      return WindowRequestResult::Rejected;
    }
  } platform;
  UiWindow window(WindowCommandsApp, platform);
  window.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  window.BuildFrame();
  REQUIRE(window_handle->Capabilities().geometry);
  REQUIRE(window_handle->SetPosition({-120.0F, 40.0F}) == WindowRequestResult::Rejected);
  REQUIRE(platform.requests == 1);
  bool rejected = false;
  std::thread worker([&] {
    try { static_cast<void>(window_handle->SetPosition({-120.0F, 40.0F})); }
    catch (const std::logic_error&) { rejected = true; }
  });
  worker.join();
  REQUIRE(rejected);
  REQUIRE(platform.requests == 1);
}

namespace {
bool pet_animate = true;
bool pet_reduced_motion = false;
int pet_compositions = 0;

View PetAnimationApp() {
  ++pet_compositions;
  ThemeSpec theme;
  theme.motion.reduced_motion = pet_reduced_motion;
  return MaterialTheme{theme, Stack {}.With(desktop_pet::Orb{.animate = pet_animate})};
}
}

TEST_CASE("DesktopPetOrbAnimatesPaintWithoutRecompositionAndStopsWhenHiddenOrMotionIsReduced") {
  pet_animate = true;
  pet_reduced_motion = false;
  pet_compositions = 0;
  TestPlatform platform;
  UiWindow window(PetAnimationApp, platform);
  window.SetWindowMetrics({.viewport = {200.0F, 200.0F}});
  const auto first = window.BuildCommit().next_frame_deadline;
  REQUIRE(first.has_value());
  REQUIRE(*first == Catch::Approx(1.0 / 30.0));
  const auto initial_commands = window.BuildFrame().Commands();
  platform.current_time = 1.0 / 30.0;
  REQUIRE(window.BuildCommit().next_frame_deadline.value() == Catch::Approx(2.0 / 30.0));
  REQUIRE(window.BuildFrame().Commands() != initial_commands);
  platform.current_time = 2.0 / 30.0;
  REQUIRE(window.BuildCommit().next_frame_deadline.value() == Catch::Approx(3.0 / 30.0));
  REQUIRE(pet_compositions == 1);

  SECTION("Hidden or manually stopped") { pet_animate = false; }
  SECTION("Reduced motion") { pet_reduced_motion = true; }
  window.InvalidateRoot();
  REQUIRE_FALSE(window.BuildCommit().next_frame_deadline);
  const auto paused_commands = window.BuildFrame().Commands();
  platform.current_time = 1.0;
  REQUIRE(window.BuildFrame().Commands() == paused_commands);
}

namespace {

View PetModelApp() {
  const auto model = UseService<desktop_pet::PetModel>();
  desktop_pet::UsePetAttachment(model);
  return Spacer();
}

View PetSettingsApp() {
  desktop_pet::UseSettingsAttachment(UseService<desktop_pet::PetModel>());
  return Spacer();
}

class PetModelWindow final : public TestPlatform {
public:
  PetModelWindow(const WindowOptions& options, int& destroyed, bool passive_display_available)
      : position_(options.initial_position.value_or(Point{})), destroyed_(destroyed),
        passive_display_available_(passive_display_available) {
    owned_native_window = true;
  }
  ~PetModelWindow() override { ++destroyed_; }

  WindowCapabilities NativeWindowCapabilities() const override {
    return {.transparent_surface = true, .utility_presentation = true,
            .passive_display = passive_display_available_, .topmost = true, .geometry = true};
  }
  WindowRequestResult RequestWindowTopmost(bool) override { return WindowRequestResult::Submitted; }
  WindowRequestResult RequestWindowPosition(Point position) override {
    position_ = position;
    return WindowRequestResult::Submitted;
  }
  std::optional<Point> WindowScreenOrigin() const override { return position_; }
  std::optional<Rect> WindowWorkArea() const override { return Rect{0.0F, 0.0F, 1000.0F, 800.0F}; }
  void RequestWindowCommand(WindowCommand command) override {
    TestPlatform::RequestWindowCommand(command);
    if (command == WindowCommand::Show || command == WindowCommand::Activate) {
      UpdateWindowLifecycleState(WindowLifecycleState::Inactive);
    } else if (command == WindowCommand::Hide) {
      UpdateWindowLifecycleState(WindowLifecycleState::Background);
    }
  }
  void DiscardHost() override {
    TestPlatform::DiscardHost();
  }

private:
  Point position_;
  int& destroyed_;
  bool passive_display_available_;
};

class PetModelRuntime final : public TestPlatform {
public:
  ~PetModelRuntime() override { huxerui::Runtime::Retire(); }
  int destroyed = 0;
  bool passive_display_available = true;
  std::vector<WindowOptions> attempted_options;
  std::vector<WindowOptions> opened_options;

  std::unique_ptr<huxerui::UiWindow> CreateUiWindow(ViewFactory content, const WindowOptions& options) override {
    attempted_options.push_back(options);
    auto window = std::make_unique<PetModelWindow>(options, destroyed, passive_display_available);
    detail::InternalAccess::InitializeWindow(*window, *this, std::move(content), options);
    window->SetWindowMetrics({.viewport = options.initial_size});
    window->BuildFrame();
    opened_options.push_back(options);
    return window;
  }
};

}

TEST_CASE("DesktopPetStartupPreservesPassiveDisplayAndFallsBackWhenUnavailable") {
  PetModelRuntime owner;
  SECTION("A backend with passive display retains its creation policy") {}
  SECTION("A backend without passive display falls back during creation") {
    owner.passive_display_available = false;
  }
  std::shared_ptr<desktop_pet::PetModel> model;
  Application application{
      PetModelApp,
      {
          .create_initial_window = false,
          .show_debug_overlay = false,
          .application_hooks = {
              [&](ApplicationContext& context) {
                model = std::make_shared<desktop_pet::PetModel>(PetModelApp, PetSettingsApp);
                context.Provide(model);
                const auto application = UseApplication();
                UseApplicationTaskScope().Post([model, application] { model->Show(application); });
              },
          },
      },
  };
  RuntimeLifetime lifetime(application, owner);
  REQUIRE(owner.opened_options.empty());
  owner.RunPlatformModuleTasks();
  REQUIRE(model->pet.has_value());
  REQUIRE(owner.opened_options.size() == 1);
  REQUIRE(owner.attempted_options.front().show_without_activation);
  REQUIRE(owner.attempted_options.front().topmost);
  REQUIRE(owner.opened_options.back().show_without_activation == owner.passive_display_available);
  REQUIRE(owner.opened_options.back().topmost == owner.passive_display_available);
  REQUIRE(owner.attempted_options.size() == (owner.passive_display_available ? 1 : 2));
}

TEST_CASE("DesktopPetLightweightHideReleasesWindowsAndRestoresApplicationPreferences") {
  std::shared_ptr<desktop_pet::PetModel> model;
  std::optional<ApplicationHandle> application_handle;
  PetModelRuntime owner;
  auto tray = std::make_shared<TestSystemTrayTransport>();
  tray->available = true;
  owner.system_tray_transport = tray;
  Application application{
      PetModelApp,
      {
          .show_debug_overlay = false,
          .quit_on_last_window_closed = false,
          .application_hooks = {
              [&model, &application_handle](ApplicationContext& context) {
                model = std::make_shared<desktop_pet::PetModel>(PetModelApp, PetSettingsApp);
                context.Provide(model);
                application_handle = UseApplication();
              },
          },
      },
  };
  RuntimeLifetime lifetime(application, owner);
  model->Show(*application_handle);
  model->OpenSettings(*application_handle);
  const auto old_pet = *model->pet;
  const auto old_settings = *model->settings;
  REQUIRE_FALSE(model->lightweight.Get());
  old_pet.Close();
  REQUIRE(old_pet.IsOpen());
  owner.RunPlatformModuleTasks();
  REQUIRE(owner.destroyed == 0);
  REQUIRE(model->pet.has_value());
  REQUIRE(model->pet->LifecycleState() == WindowLifecycleState::Background);
  model->Show(*application_handle);
  REQUIRE(model->pet->SetPosition({-120.0F, 64.0F}) == WindowRequestResult::Submitted);
  model->energized = true;
  model->animate = false;
  model->SetTopmost(false);
  model->SetLightweight(true, true);
  REQUIRE(model->pet.has_value());
  SECTION("Lightweight hide requests closure") { model->Hide(true); }
  SECTION("Lightweight close allows the native close path") { old_pet.Close(); }
  owner.RunPlatformModuleTasks();
  REQUIRE(owner.destroyed == 2);
  REQUIRE_FALSE(model->pet);
  REQUIRE_FALSE(model->settings);
  REQUIRE(old_pet.SetTopmost(true) == WindowRequestResult::Disconnected);
  REQUIRE(old_settings.SetTopmost(true) == WindowRequestResult::Disconnected);
  REQUIRE(owner.application_quit_requests == 0);

  model->Show(*application_handle);
  REQUIRE(model->pet.has_value());
  REQUIRE((model->pet->Position() == Point{-120.0F, 64.0F}));
  REQUIRE_FALSE(owner.opened_options.back().topmost);
  REQUIRE(model->energized.Get());
  REQUIRE_FALSE(model->animate.Get());
  REQUIRE(model->lightweight.Get());
  model->Hide(true);
  owner.RunPlatformModuleTasks();
  model->OpenSettings(*application_handle);
  REQUIRE_FALSE(model->pet);
  model->SetTopmost(true);
  model->Show(*application_handle);
  REQUIRE(owner.opened_options.back().topmost);
  model->Hide(true);
  owner.RunPlatformModuleTasks();
  REQUIRE(owner.destroyed == 5);
}

TEST_CASE("DesktopPetLightweightModeProtectsReplacementsAndRequiresTrayRecovery") {
  std::shared_ptr<desktop_pet::PetModel> model;
  std::optional<ApplicationHandle> application_handle;
  PetModelRuntime owner;
  Application application{
      PetModelApp,
      {
          .show_debug_overlay = false,
          .quit_on_last_window_closed = false,
          .application_hooks = {
              [&model, &application_handle](ApplicationContext& context) {
                model = std::make_shared<desktop_pet::PetModel>(PetModelApp, PetSettingsApp);
                context.Provide(model);
                application_handle = UseApplication();
              },
          },
      },
  };
  RuntimeLifetime lifetime(application, owner);
  model->Show(*application_handle);
  model->SetLightweight(true, false);
  model->Hide(false);
  REQUIRE_FALSE(model->lightweight.Get());
  REQUIRE(model->pet->LifecycleState() != WindowLifecycleState::Background);

  SECTION("Enabling while hidden releases the retained window") {
    model->Hide(true);
    model->SetLightweight(true, true);
    owner.RunPlatformModuleTasks();
    REQUIRE(owner.destroyed == 1);
    REQUIRE_FALSE(model->pet);
  }
  SECTION("Old lifecycle cleanup preserves newly mounted windows") {
    model->SetLightweight(true, true);
    model->OpenSettings(*application_handle);
    model->Hide(true);
    model->Show(*application_handle);
    model->OpenSettings(*application_handle);
    owner.RunPlatformModuleTasks();
    REQUIRE(owner.destroyed == 2);
    REQUIRE(model->pet.has_value());
    REQUIRE(model->settings.has_value());
    REQUIRE(model->pet->SetTopmost(true) == WindowRequestResult::Submitted);
    REQUIRE(model->settings->SetTopmost(true) == WindowRequestResult::Submitted);
    model->Hide(true);
    owner.RunPlatformModuleTasks();
    REQUIRE(owner.destroyed == 4);
    REQUIRE_FALSE(model->pet);
    REQUIRE_FALSE(model->settings);
  }
}

TEST_CASE("WindowRetirementReleasesItsLastSemanticFrame") {
  TestPlatform platform;
  UiWindow window(WindowContentApp, platform);
  window.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  std::weak_ptr<const SemanticFrame> frame = window.BuildCommit().semantic_frame;
  REQUIRE(!frame.expired());

  detail::InternalAccess::RetireWindow(platform);

  REQUIRE(frame.expired());
}

namespace {
std::size_t lifecycle_window_slot = 0;
std::vector<WindowHandle> lifecycle_windows;
std::vector<std::vector<WindowLifecycleState>> lifecycle_transitions;
int lifecycle_secondary_calls = 0;

View WindowLifecycleApp() {
  const auto window = UseWindow();
  const auto slot = lifecycle_window_slot;
  lifecycle_windows.push_back(window);
  window.OnLifecycleChanged([slot](WindowLifecycleState state) {
    lifecycle_transitions.at(slot).push_back(state);
    REQUIRE(UseWindow().LifecycleState() == lifecycle_windows.at(slot).LifecycleState());
  });
  window.OnLifecycleChanged([](WindowLifecycleState) { ++lifecycle_secondary_calls; });
  return {};
}
}

TEST_CASE("Window lifecycle remains local and delivers each live observer without frames") {
  lifecycle_windows.clear();
  lifecycle_transitions = {{}, {}};
  lifecycle_secondary_calls = 0;
  TestPlatform application_platform;
  TestPlatform first_platform;
  TestPlatform second_platform;
  Application application(WindowLifecycleApp, {.show_debug_overlay = false});
  RuntimeLifetime runtime(application, application_platform, LaunchActivation{}, ApplicationLifecycleState::Active);
  UiWindow first(runtime, first_platform);
  auto second = std::make_unique<UiWindow>(runtime, second_platform);
  first.SetWindowMetrics({.viewport = {320, 240}});
  second->SetWindowMetrics({.viewport = {320, 240}});
  lifecycle_window_slot = 0;
  first.BuildFrame();
  lifecycle_window_slot = 1;
  second->BuildFrame();
  application_platform.RunPlatformModuleTasks();
  REQUIRE(lifecycle_secondary_calls == 0);
  first.UpdateWindowLifecycleState(WindowLifecycleState::Active);
  first.UpdateWindowLifecycleState(WindowLifecycleState::Inactive);
  first.UpdateWindowLifecycleState(WindowLifecycleState::Background);
  first.UpdateWindowLifecycleState(WindowLifecycleState::Background);
  second->UpdateWindowLifecycleState(WindowLifecycleState::Active);
  REQUIRE(lifecycle_windows[0].LifecycleState() == WindowLifecycleState::Background);
  REQUIRE(lifecycle_windows[1].LifecycleState() == WindowLifecycleState::Active);
  REQUIRE(UseApplication().LifecycleState() == ApplicationLifecycleState::Active);
  REQUIRE(lifecycle_secondary_calls == 0);
  application_platform.RunPlatformModuleTasks();
  REQUIRE(lifecycle_transitions[0] == std::vector<WindowLifecycleState>{WindowLifecycleState::Active,
      WindowLifecycleState::Inactive, WindowLifecycleState::Background});
  REQUIRE(lifecycle_transitions[1] == std::vector<WindowLifecycleState>{WindowLifecycleState::Active});
  REQUIRE(lifecycle_secondary_calls == 4);
  second->UpdateWindowLifecycleState(WindowLifecycleState::Background);
  second.reset();
  application_platform.RunPlatformModuleTasks();
  REQUIRE(lifecycle_secondary_calls == 4);
}

namespace {

class OwnedTestWindow final : public TestPlatform {
public:
  explicit OwnedTestWindow(int& destroyed) : destroyed_(destroyed) {}
  ~OwnedTestWindow() override {
    huxerui::UiWindow::Retire();
    ++destroyed_;
  }
  void Initialize(huxerui::Runtime& runtime, ViewFactory content, const WindowOptions& options) {
    InitializeWindow(runtime, std::move(content), options,
                     ResourceConfiguration{Locale::FromLanguageTag("fr-FR"), 2.0F});
    SetWindowMetrics({.viewport = options.initial_size});
  }
  void RequestWindowCommand(WindowCommand command) override {
    TestPlatform::RequestWindowCommand(command);
    if (command == WindowCommand::Close) huxerui::UiWindow::CloseWindow();
  }
  void Frame() { huxerui::UiWindow::BuildFrame(); }
private:
  int& destroyed_;
};

class OwnedWindowPlatform final : public TestPlatform {
public:
  using TestPlatform::TestPlatform;
  ~OwnedWindowPlatform() override { huxerui::Runtime::Retire(); }
  bool SupportsMultipleWindows() const noexcept override { return true; }
  std::unique_ptr<huxerui::UiWindow> CreateUiWindow(ViewFactory content, const WindowOptions& options) override {
    ++creations;
    if (before_create) before_create();
    auto window = std::make_unique<OwnedTestWindow>(destroyed);
    window->Initialize(*this, std::move(content), options);
    windows.push_back(window.get());
    return window;
  }
  int creations = 0;
  int destroyed = 0;
  std::function<void()> before_create;
  std::vector<OwnedTestWindow*> windows;
};

View EmptyOwnedWindow() { return {}; }

}

TEST_CASE("Owned windows have independent content and local State while sharing application State") {
  OwnedWindowPlatform platform;
  State<int> shared;
  State<int> first_local;
  State<int> second_local;
  int hooks = 0;
  int first_builds = 0;
  int second_builds = 0;
  int first_value = -1;
  int second_value = -1;
  Application application(EmptyOwnedWindow, {
      .show_debug_overlay = false,
      .window_hooks = {[&](WindowContext&) {
        ++hooks;
        REQUIRE(UseService<Resources>()->Configuration().display_scale == 2.0F);
        REQUIRE(UseService<Resources>()->Configuration().locale.LanguageTag() == "fr-FR");
      }},
  });
  RuntimeLifetime runtime(application, platform);
  auto app = UseApplication();
  REQUIRE(app.SupportsMultipleWindows());
  shared = State<int>{0};
  auto first = app.OpenWindow([&] {
    ++first_builds;
    first_local = UseState(10);
    first_value = first_local.Get() + shared.Get();
    return Text(std::to_string(first_value));
  }, {.initial_size = {200, 100}});
  auto second = app.OpenWindow([&] {
    ++second_builds;
    second_local = UseState(20);
    second_value = second_local.Get() + shared.Get();
    return Text(std::to_string(second_value));
  }, {.initial_size = {400, 300}});
  REQUIRE(hooks == 2);
  REQUIRE(first.IsOpen());
  REQUIRE(second.IsOpen());
  platform.windows[0]->Frame();
  platform.windows[1]->Frame();
  REQUIRE(first_value == 10);
  REQUIRE(second_value == 20);
  first_local = 11;
  platform.windows[0]->Frame();
  platform.windows[1]->Frame();
  REQUIRE(first_builds == 2);
  REQUIRE(second_builds == 1);
  shared = 3;
  platform.windows[0]->Frame();
  platform.windows[1]->Frame();
  REQUIRE(first_value == 14);
  REQUIRE(second_value == 23);
  first.Close();
  REQUIRE_FALSE(first.IsOpen());
  REQUIRE(platform.destroyed == 0);
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.destroyed == 1);
  REQUIRE(second.IsOpen());
  REQUIRE(platform.application_quit_requests == 0);
  second.Hide();
  platform.RunPlatformModuleTasks();
  REQUIRE(second.IsOpen());
  REQUIRE(platform.application_quit_requests == 0);
  second.Close();
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.destroyed == 2);
  REQUIRE(platform.application_quit_requests == 1);
  REQUIRE_FALSE(app.SupportsMultipleWindows());
  REQUIRE_THROWS_AS(app.OpenWindow(EmptyOwnedWindow), std::logic_error);
  REQUIRE_NOTHROW(first.Close());
}

TEST_CASE("Owned window closure is observable and waits for the outer UI callback") {
  OwnedWindowPlatform platform;
  bool open = false;
  int builds = 0;
  Application application(EmptyOwnedWindow, {.show_debug_overlay = false, .quit_on_last_window_closed = false});
  RuntimeLifetime runtime(application, platform);
  auto app = UseApplication();
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.application_quit_requests == 0);
  auto first = app.OpenWindow(EmptyOwnedWindow);
  auto observer = app.OpenWindow([&] {
    ++builds;
    open = first.IsOpen();
    return Text(open ? "Open" : "Closed");
  });
  platform.windows[1]->Frame();
  REQUIRE(open);
  {
    detail::ExecutionGuard callback(detail::InternalAccess::WindowExecution(*platform.windows[0]));
    first.Close();
    platform.RunPlatformModuleTasks();
    REQUIRE_FALSE(first.IsOpen());
    REQUIRE(platform.destroyed == 0);
  }
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.destroyed == 1);
  platform.windows[1]->Frame();
  REQUIRE_FALSE(open);
  REQUIRE(builds == 2);
  observer.Close();
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.application_quit_requests == 0);
  auto reopened = app.OpenWindow(EmptyOwnedWindow);
  REQUIRE(reopened.IsOpen());
  app.Quit();
  platform.RunPlatformModuleTasks();
  REQUIRE_FALSE(reopened.IsOpen());
  REQUIRE(platform.application_quit_requests == 1);
}

TEST_CASE("Rejected maintenance wakes do not escape UI callback cleanup") {
  std::deque<std::function<void()>> callbacks;
  bool reject_wake = false;
  int rejected = 0;
  OwnedWindowPlatform platform([&](std::function<void()> callback) {
    if (reject_wake) {
      ++rejected;
      throw std::runtime_error("native wake rejected");
    }
    callbacks.push_back(std::move(callback));
  });
  const auto drain = [&] {
    while (!callbacks.empty()) {
      auto callback = std::move(callbacks.front());
      callbacks.pop_front();
      callback();
    }
  };
  Application application(EmptyOwnedWindow, {.show_debug_overlay = false});
  RuntimeLifetime runtime(application, platform);
  auto window = UseApplication().OpenWindow(EmptyOwnedWindow);
  const auto source = detail::InternalAccess::WindowExecution(*platform.windows.front());
  const auto close_in_callback = [&](bool fail) {
    detail::ExecutionGuard callback(source);
    window.Close();
    drain();
    REQUIRE(platform.destroyed == 0);
    reject_wake = true;
    if (fail) throw std::logic_error("original callback failure");
  };

  SECTION("Normal return") { REQUIRE_NOTHROW(close_in_callback(false)); }
  SECTION("Exception unwinding") { REQUIRE_THROWS_WITH(close_in_callback(true), "original callback failure"); }

  REQUIRE(rejected == 1);
  REQUIRE(callbacks.empty());
  REQUIRE_FALSE(window.IsOpen());
  REQUIRE(platform.destroyed == 0);
  reject_wake = false;
  { detail::ExecutionGuard callback(source); }
  drain();
  REQUIRE(platform.destroyed == 1);
  REQUIRE(platform.application_quit_requests == 1);
}

TEST_CASE("Failed native window creation resumes previously deferred closure") {
  std::deque<std::function<void()>> callbacks;
  bool reject_wake = false;
  bool reject_retry = false;
  OwnedWindowPlatform platform([&](std::function<void()> callback) {
    if (reject_wake) throw std::runtime_error("native wake rejected");
    callbacks.push_back(std::move(callback));
  });
  const auto drain = [&] {
    while (!callbacks.empty()) {
      auto callback = std::move(callbacks.front());
      callbacks.pop_front();
      callback();
    }
  };
  Application application(EmptyOwnedWindow, {.show_debug_overlay = false});
  RuntimeLifetime runtime(application, platform);
  auto app = UseApplication();
  auto window = app.OpenWindow(EmptyOwnedWindow);
  const auto source = detail::InternalAccess::WindowExecution(*platform.windows.front());
  window.Close();
  platform.before_create = [&] {
    drain();
    REQUIRE(platform.destroyed == 0);
    reject_wake = reject_retry;
    throw std::logic_error("native window creation failed");
  };

  SECTION("Maintenance wake accepted") {}
  SECTION("Maintenance wake rejected") { reject_retry = true; }

  REQUIRE_THROWS_WITH(app.OpenWindow(EmptyOwnedWindow), "native window creation failed");
  REQUIRE_FALSE(window.IsOpen());
  REQUIRE(platform.destroyed == 0);
  if (reject_retry) {
    REQUIRE(callbacks.empty());
    reject_wake = false;
    { detail::ExecutionGuard callback(source); }
  }
  drain();
  REQUIRE(platform.destroyed == 1);
  REQUIRE(platform.application_quit_requests == 1);
}

TEST_CASE("Failed external window installation resumes previously deferred closure") {
  OwnedWindowPlatform platform;
  bool fail = false;
  Application application(EmptyOwnedWindow, {
      .show_debug_overlay = false,
      .window_hooks = {[&](WindowContext&) {
        if (fail) {
          platform.RunPlatformModuleTasks();
          REQUIRE(platform.destroyed == 0);
          throw std::logic_error("external window installation failed");
        }
      }},
  });
  RuntimeLifetime runtime(application, platform);
  auto window = UseApplication().OpenWindow(EmptyOwnedWindow);
  window.Close();
  fail = true;
  int external_destroyed = 0;
  {
    OwnedTestWindow attachment(external_destroyed);
    REQUIRE_THROWS_WITH(attachment.Initialize(platform, EmptyOwnedWindow, {}), "external window installation failed");
  }
  REQUIRE(external_destroyed == 1);
  REQUIRE_FALSE(window.IsOpen());
  REQUIRE(platform.destroyed == 0);
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.destroyed == 1);
  REQUIRE(platform.application_quit_requests == 1);
}

TEST_CASE("Owned window creation rejects invalid timing and rolls back failed hooks") {
  OwnedWindowPlatform platform;
  bool fail = true;
  std::optional<WindowHandle> failed_handle;
  Application application(EmptyOwnedWindow, {
      .show_debug_overlay = false,
      .application_hooks = {[](ApplicationContext&) {
        REQUIRE_THROWS_AS(UseApplication().OpenWindow(EmptyOwnedWindow), std::logic_error);
      }},
      .window_hooks = {[&](WindowContext&) {
        REQUIRE_THROWS_AS(UseApplication().OpenWindow(EmptyOwnedWindow), std::logic_error);
        REQUIRE_THROWS_AS(UseApplication().Quit(), std::logic_error);
        REQUIRE_THROWS_AS(UseWindow().Close(), std::logic_error);
        if (fail) {
          failed_handle = UseWindow();
          throw std::runtime_error("failed window");
        }
      }},
  });
  RuntimeLifetime runtime(application, platform);
  auto app = UseApplication();
  REQUIRE_THROWS_AS(app.OpenWindow({}), std::invalid_argument);
  REQUIRE_THROWS_AS(app.OpenWindow(EmptyOwnedWindow, {.initial_size = {-1, 100}}), std::invalid_argument);
  REQUIRE(platform.creations == 0);
  REQUIRE_THROWS_AS(app.OpenWindow(EmptyOwnedWindow), std::runtime_error);
  REQUIRE_FALSE(failed_handle->IsOpen());
  REQUIRE(platform.destroyed == 1);
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.application_quit_requests == 0);
  fail = false;
  auto window = app.OpenWindow([&] {
    REQUIRE_THROWS_AS(app.OpenWindow(EmptyOwnedWindow), std::logic_error);
    return View{};
  });
  platform.windows.back()->Frame();
#if !defined(__EMSCRIPTEN__)
  bool rejected = false;
  std::thread worker([&] {
    try { app.OpenWindow(EmptyOwnedWindow); } catch (const std::logic_error&) { rejected = true; }
  });
  worker.join();
  REQUIRE(rejected);
#endif
  REQUIRE(window.IsOpen());
}

TEST_CASE("Quit bypasses per-window interception and waits for active UI execution") {
  OwnedWindowPlatform platform;
  int close_requests = 0;
  bool intercept = true;
  Application application(EmptyOwnedWindow, {.show_debug_overlay = false});
  RuntimeLifetime runtime(application, platform);
  auto app = UseApplication();
  auto window = app.OpenWindow([&] {
    UseWindow().OnCloseRequest([&] { ++close_requests; return intercept; });
    return View{};
  });
  platform.windows[0]->Frame();
  window.Close();
  REQUIRE(window.IsOpen());
  REQUIRE(close_requests == 1);
  {
    detail::ExecutionGuard callback(detail::InternalAccess::WindowExecution(*platform.windows[0]));
    app.Quit();
    platform.RunPlatformModuleTasks();
    REQUIRE(platform.destroyed == 0);
    REQUIRE_THROWS_AS(app.OpenWindow(EmptyOwnedWindow), std::logic_error);
  }
  platform.RunPlatformModuleTasks();
  REQUIRE_FALSE(window.IsOpen());
  REQUIRE(platform.destroyed == 1);
  REQUIRE(close_requests == 1);
  REQUIRE(platform.application_quit_requests == 1);
}

TEST_CASE("Window request handlers allow cross-command calls and stop recursive cycles") {
  OwnedWindowPlatform platform;
  int closes = 0;
  int minimizes = 0;
  Application application(EmptyOwnedWindow, {.show_debug_overlay = false});
  RuntimeLifetime runtime(application, platform);
  auto window = UseApplication().OpenWindow([&] {
    const auto current = UseWindow();
    current.OnCloseRequest([&, current] {
      REQUIRE(++closes == 1);
      current.Minimize();
      return true;
    });
    current.OnMinimizeRequest([&, current] {
      REQUIRE(++minimizes == 1);
      current.Close();
      return false;
    });
    return View{};
  });
  auto* native = platform.windows.front();
  native->Frame();
  native->window_commands.clear();

  SECTION("Public close request") { window.Close(); }
  SECTION("Public minimize request") { window.Minimize(); }
  SECTION("Native close request") { REQUIRE(native->HandleWindowRequest(WindowCommand::Close)); }

  REQUIRE(closes == 1);
  REQUIRE(minimizes == 1);
  REQUIRE(native->window_commands == std::vector<WindowCommand>{WindowCommand::Minimize});
  REQUIRE(window.IsOpen());
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.destroyed == 0);
}

TEST_CASE("Window request guards recover after exceptions and consume same-command recursion") {
  OwnedWindowPlatform platform;
  int closes = 0;
  int minimizes = 0;
  bool fail = true;
  Application application(EmptyOwnedWindow, {.show_debug_overlay = false});
  RuntimeLifetime runtime(application, platform);
  auto window = UseApplication().OpenWindow([&] {
    const auto current = UseWindow();
    current.OnCloseRequest([&, current] {
      REQUIRE(++closes <= 2);
      current.Close();
      if (fail) throw std::runtime_error("close failed");
      return false;
    });
    current.OnMinimizeRequest([&, current] {
      REQUIRE(++minimizes <= 2);
      current.Minimize();
      if (fail) throw std::runtime_error("minimize failed");
      return false;
    });
    return View{};
  });
  auto* native = platform.windows.front();
  native->Frame();
  native->window_commands.clear();

  REQUIRE_THROWS_AS(window.Close(), std::runtime_error);
  REQUIRE_THROWS_AS(window.Minimize(), std::runtime_error);
  REQUIRE(closes == 1);
  REQUIRE(minimizes == 1);
  REQUIRE(native->window_commands.empty());
  REQUIRE(window.IsOpen());

  fail = false;
  window.Minimize();
  window.Close();
  REQUIRE(closes == 2);
  REQUIRE(minimizes == 2);
  REQUIRE(native->window_commands == std::vector<WindowCommand>{WindowCommand::Minimize, WindowCommand::Close});
  REQUIRE_FALSE(window.IsOpen());
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.destroyed == 1);
}

TEST_CASE("Closing inside a minimize handler suppresses the stale native minimize") {
  OwnedWindowPlatform platform;
  Application application(EmptyOwnedWindow, {.show_debug_overlay = false});
  RuntimeLifetime runtime(application, platform);
  auto window = UseApplication().OpenWindow([&] {
    const auto current = UseWindow();
    current.OnMinimizeRequest([&, current] {
      current.Close();
      REQUIRE_FALSE(current.IsOpen());
      platform.RunPlatformModuleTasks();
      REQUIRE(platform.destroyed == 0);
      return false;
    });
    return View{};
  });
  auto* native = platform.windows.front();
  native->Frame();
  native->window_commands.clear();

  SECTION("Public minimize request") { window.Minimize(); }
  SECTION("Native minimize request") { REQUIRE(native->HandleWindowRequest(WindowCommand::Minimize)); }

  REQUIRE(native->window_commands == std::vector<WindowCommand>{WindowCommand::Close});
  REQUIRE_FALSE(window.IsOpen());
  REQUIRE(platform.destroyed == 0);
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.destroyed == 1);
  REQUIRE(platform.application_quit_requests == 1);
}

TEST_CASE("Replacing a close handler during its callback preserves reentrancy protection") {
  OwnedWindowPlatform platform;
  State<int> revision;
  int replacements = 0;
  Application application(EmptyOwnedWindow, {.show_debug_overlay = false});
  RuntimeLifetime runtime(application, platform);
  auto window = UseApplication().OpenWindow([&] {
    revision = UseState(0);
    const auto generation = revision.Get();
    const auto current = UseWindow();
    current.OnCloseRequest([&, current, generation] {
      if (generation == 0) {
        revision = 1;
        platform.windows.front()->Frame();
        current.Close();
        return true;
      }
      ++replacements;
      return false;
    }, generation);
    return View{};
  });
  auto* native = platform.windows.front();
  native->Frame();
  native->window_commands.clear();

  window.Close();
  REQUIRE(replacements == 0);
  REQUIRE(native->window_commands.empty());
  REQUIRE(window.IsOpen());
  window.Close();
  REQUIRE(replacements == 1);
  REQUIRE(native->window_commands == std::vector<WindowCommand>{WindowCommand::Close});
  REQUIRE_FALSE(window.IsOpen());
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.destroyed == 1);
}

TEST_CASE("Hosts without top-level creation keep externally owned attachments independent") {
  TestPlatform platform;
  platform.multiple_windows_supported = false;
  TestPlatform attachment;
  Application application(EmptyOwnedWindow, {.show_debug_overlay = false});
  RuntimeLifetime runtime(application, platform);
  auto app = UseApplication();
  REQUIRE_FALSE(app.SupportsMultipleWindows());
  REQUIRE_THROWS_AS(app.OpenWindow(EmptyOwnedWindow), std::runtime_error);
  { UiWindow window(runtime, attachment); }
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.application_quit_requests == 0);
}

TEST_CASE("Closing an owner retires descendants before native destruction and exits only after the last window") {
  TestPlatform platform;
  Application application(EmptyOwnedWindow, {.show_debug_overlay = false});
  bool all_closed = true;
  std::vector<int> order;
  RuntimeLifetime runtime(application, platform);
  const auto app = UseApplication();
  const auto owner = app.OpenWindow(EmptyOwnedWindow);
  const auto child = app.OpenWindow(EmptyOwnedWindow, {}, {.owner = owner, .modal = true});
  const auto grandchild = app.OpenWindow(EmptyOwnedWindow, {}, {.owner = child});
  const auto independent = app.OpenWindow(EmptyOwnedWindow);
  for (int index = 0; index != 3; ++index) {
    platform.created_windows[index]->on_destroy = [&, index, owner, child, grandchild] {
      order.push_back(index);
      all_closed &= !owner.IsOpen() && !child.IsOpen() && !grandchild.IsOpen();
    };
  }
  owner.Close();
  REQUIRE(order.empty());
  platform.RunPlatformModuleTasks();
  REQUIRE(order == std::vector<int>{2, 1, 0});
  REQUIRE(all_closed);
  REQUIRE(independent.IsOpen());
  REQUIRE(platform.application_quit_requests == 0);
  independent.Close();
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.application_quit_requests == 1);
}

TEST_CASE("Relationships are installed before presentation and passive windows are shown without activation") {
  class PresentationWindow final : public TestPlatform {
  public:
    bool presented_with_relationship = false;
    WindowCapabilities NativeWindowCapabilities() const override { return {.passive_display = true}; }
    void RequestWindowCommand(WindowCommand command) override {
      if (command == WindowCommand::Show || command == WindowCommand::Activate) {
        presented_with_relationship = !applied_owners.empty() && !applied_modal_states.empty();
      }
      TestPlatform::RequestWindowCommand(command);
    }
  };
  class PresentationRuntime final : public TestPlatform {
  public:
    std::vector<PresentationWindow*> windows;
    std::unique_ptr<huxerui::UiWindow> CreateUiWindow(ViewFactory content, const WindowOptions& options) override {
      auto window = std::make_unique<PresentationWindow>();
      detail::InternalAccess::InitializeWindow(*window, *this, std::move(content), options);
      windows.push_back(window.get());
      return window;
    }
  } platform;
  Application application(EmptyOwnedWindow, {.show_debug_overlay = false});
  RuntimeLifetime runtime(application, platform);
  const auto app = UseApplication();
  const auto owner = app.OpenWindow(EmptyOwnedWindow);
  const auto modal = app.OpenWindow(EmptyOwnedWindow, {}, {.owner = owner, .modal = true});
  const auto passive = app.OpenWindow(EmptyOwnedWindow, {.show_without_activation = true});
  REQUIRE(platform.windows[1]->presented_with_relationship);
  REQUIRE(platform.windows[1]->applied_owners.back() == platform.windows[0]);
  REQUIRE(platform.windows[0]->applied_modal_states.back() == std::pair<bool, bool>{false, true});
  REQUIRE(platform.windows[2]->window_commands == std::vector<WindowCommand>{WindowCommand::Show});
  REQUIRE(modal.IsOpen());
  REQUIRE(passive.IsOpen());
}

TEST_CASE("Relationship changes defer native destruction through reentrant callbacks") {
  class RelationshipWindow final : public TestPlatform {
  public:
    std::function<void()> on_ownership;
    void ApplyWindowOwnership(huxerui::UiWindow* owner) override {
      TestPlatform::ApplyWindowOwnership(owner);
      if (on_ownership) on_ownership();
    }
  };
  class RelationshipRuntime final : public TestPlatform {
  public:
    std::vector<RelationshipWindow*> windows;
    std::unique_ptr<huxerui::UiWindow> CreateUiWindow(ViewFactory content, const WindowOptions& options) override {
      auto window = std::make_unique<RelationshipWindow>();
      window->owned_native_window = true;
      window->on_destroy = [this] { ++destroyed_windows; };
      detail::InternalAccess::InitializeWindow(*window, *this, std::move(content), options);
      windows.push_back(window.get());
      return window;
    }
  } platform;
  bool alive_in_callback = false;
  Application application(EmptyOwnedWindow, {.show_debug_overlay = false});
  RuntimeLifetime runtime(application, platform);
  const auto app = UseApplication();
  const auto owner = app.OpenWindow(EmptyOwnedWindow);
  const auto child = app.OpenWindow(EmptyOwnedWindow);
  platform.windows[1]->on_ownership = [&] {
    child.Close();
    platform.RunPlatformModuleTasks();
    alive_in_callback = platform.destroyed_windows == 0;
  };
  SetWindowRelationship(child, {.owner = owner});
  REQUIRE(alive_in_callback);
  REQUIRE(platform.destroyed_windows == 0);
  REQUIRE_FALSE(child.IsOpen());
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.destroyed_windows == 1);
  REQUIRE(owner.IsOpen());
}

} // namespace huxerui::test
