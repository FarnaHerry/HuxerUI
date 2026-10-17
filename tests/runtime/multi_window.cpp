#include "runtime_test_support.h"

#include <optional>
#include <string>
#include <thread>

namespace huxerui::test {

namespace {

WindowIdentity primary_identity;
WindowIdentity secondary_identity;
int primary_compositions = 0;
int secondary_compositions = 0;
std::optional<State<int>> primary_state;
std::optional<State<int>> secondary_state;
std::optional<WindowHandle> primary_handle;
std::optional<WindowHandle> secondary_handle;
std::optional<ApplicationHandle> captured_application;

View IdentityApp() {
  const WindowIdentity identity = UseEnvironment<WindowIdentity>();
  if (identity.ordinal == 0) {
    primary_identity = identity;
    ++primary_compositions;
  } else {
    secondary_identity = identity;
    ++secondary_compositions;
  }
  return Text(identity.key.empty() ? std::string{"primary"} : "secondary: " + identity.key);
}

View IsolatedCounterApp() {
  const WindowIdentity identity = UseEnvironment<WindowIdentity>();
  auto count = UseState(0);
  if (identity.ordinal == 0) {
    ++primary_compositions;
    primary_state = count;
    primary_handle = UseWindow();
  } else {
    ++secondary_compositions;
    secondary_state = count;
    secondary_handle = UseWindow();
  }
  return Text::Format("{}", count.Get());
}

View CaptureApplicationApp() {
  captured_application = UseApplication();
  return Spacer();
}

void ResetMultiWindowGlobals() {
  primary_identity = WindowIdentity{};
  secondary_identity = WindowIdentity{};
  primary_compositions = 0;
  secondary_compositions = 0;
  primary_state.reset();
  secondary_state.reset();
  primary_handle.reset();
  secondary_handle.reset();
  captured_application.reset();
}

} // namespace

TEST_CASE("MultiWindowAttachmentsReceiveDistinctIdentities") {
  ResetMultiWindowGlobals();
  TestPlatform owner;
  Application application{IdentityApp, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  UiWindow primary(owner, owner, application.options.window, 0);
  const WindowOptions secondary_options{
      .title = "secondary",
      .key = "secondary",
      .initial_size = {320.0F, 240.0F},
  };
  TestPlatform secondary_platform;
  UiWindow secondary(owner, secondary_platform, secondary_options, 1);
  primary.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
  secondary.SetWindowMetrics({.viewport = {200.0F, 120.0F}});

  REQUIRE(FirstText(primary.BuildFrame()) == "primary");
  REQUIRE(FirstText(secondary.BuildFrame()) == "secondary: secondary");
  REQUIRE(primary_compositions == 1);
  REQUIRE(secondary_compositions == 1);
  REQUIRE(primary_identity.key.empty());
  REQUIRE(primary_identity.ordinal == 0);
  REQUIRE(secondary_identity.key == "secondary");
  REQUIRE(secondary_identity.ordinal == 1);
}

TEST_CASE("RuntimeOpenWindowCreatesIndependentAttachments") {
  ResetMultiWindowGlobals();
  TestPlatform owner;
  Application application{IdentityApp, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  const WindowHandle first = owner.OpenWindow(application.options.window);
  const WindowHandle second =
      owner.OpenWindow({.title = "secondary", .key = "secondary", .initial_size = {320.0F, 240.0F}});
  REQUIRE(owner.created_windows.size() == 2);
  owner.created_windows[0]->SetWindowMetrics({.viewport = {200.0F, 120.0F}});
  owner.created_windows[1]->SetWindowMetrics({.viewport = {200.0F, 120.0F}});

  const FlattenedScene first_scene = FlattenedFrame(owner.created_windows[0]->BuildFrame());
  const FlattenedScene second_scene = FlattenedFrame(owner.created_windows[1]->BuildFrame());
  REQUIRE(FirstText(first_scene) == "primary");
  REQUIRE(FirstText(second_scene) == "secondary: secondary");
  REQUIRE(primary_identity.ordinal == 0);
  REQUIRE(secondary_identity.ordinal == 1);
  REQUIRE(first.LifecycleState() == WindowLifecycleState::Background);
  REQUIRE(second.LifecycleState() == WindowLifecycleState::Background);
}

TEST_CASE("MultiWindowStateDoesNotRecomposeOtherWindows") {
  ResetMultiWindowGlobals();
  TestPlatform owner;
  Application application{IsolatedCounterApp, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  UiWindow primary(owner, owner, application.options.window, 0);
  TestPlatform secondary_platform;
  UiWindow secondary(owner, secondary_platform, WindowOptions{.title = "secondary"}, 1);
  primary.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
  secondary.SetWindowMetrics({.viewport = {200.0F, 120.0F}});

  REQUIRE(FirstText(primary.BuildFrame()) == "0");
  REQUIRE(FirstText(secondary.BuildFrame()) == "0");
  REQUIRE(primary_compositions == 1);
  REQUIRE(secondary_compositions == 1);

  REQUIRE(primary_state.has_value());
  *primary_state += 1;
  REQUIRE(FirstText(primary.BuildFrame()) == "1");
  REQUIRE(primary_compositions == 2);
  REQUIRE(FirstText(secondary.BuildFrame()) == "0");
  REQUIRE(secondary_compositions == 1);
}

TEST_CASE("RetiringOneWindowLeavesOtherWindowsAlive") {
  ResetMultiWindowGlobals();
  TestPlatform owner;
  Application application{IsolatedCounterApp, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  UiWindow primary(owner, owner, application.options.window, 0);
  {
    TestPlatform secondary_platform;
    UiWindow secondary(owner, secondary_platform, WindowOptions{.title = "secondary"}, 1);
    secondary.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
    REQUIRE(FirstText(secondary.BuildFrame()) == "0");
    REQUIRE(secondary_compositions == 1);
    REQUIRE(secondary_handle.has_value());
  }

  primary.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
  REQUIRE(FirstText(primary.BuildFrame()) == "0");
  REQUIRE(primary_compositions == 1);
  if (secondary_handle.has_value()) {
    secondary_handle->Hide();
    secondary_handle->Close();
  }
}

TEST_CASE("RuntimeOpenWindowValidatesPhaseThreadAndOptions") {
  ResetMultiWindowGlobals();
  TestPlatform owner;
  REQUIRE_THROWS_AS(owner.OpenWindow({}), std::logic_error);

  Application application{IsolatedCounterApp, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  static_cast<void>(owner.OpenWindow(application.options.window));
  REQUIRE_THROWS_AS(owner.OpenWindow({.initial_size = {0.0F, 0.0F}}), std::invalid_argument);

  std::exception_ptr thread_failure;
  std::thread worker([&owner, &thread_failure] {
    try {
      static_cast<void>(owner.OpenWindow({}));
    } catch (...) {
      thread_failure = std::current_exception();
    }
  });
  worker.join();
  REQUIRE(thread_failure != nullptr);
  REQUIRE_THROWS_AS(std::rethrow_exception(thread_failure), std::logic_error);
}

TEST_CASE("ApplicationHandleOpenWindowReportsDisconnectedService") {
  ResetMultiWindowGlobals();
  {
    TestPlatform owner;
    Application application{CaptureApplicationApp, {.show_debug_overlay = false}};
    RuntimeLifetime lifetime(application, owner);
    UiWindow primary(owner, owner, application.options.window, 0);
    primary.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
    primary.BuildFrame();
    REQUIRE(captured_application.has_value());
  }
  REQUIRE_THROWS_AS(captured_application->OpenWindow({}), std::logic_error);
}

} // namespace huxerui::test
