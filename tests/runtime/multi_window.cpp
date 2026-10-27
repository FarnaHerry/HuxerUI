#include "runtime_test_support.h"

#include <optional>
#include <limits>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>

#include "application/window_internal.h"

namespace huxerui::test {

namespace {

int primary_compositions = 0;
int secondary_compositions = 0;
std::optional<State<int>> primary_state;
std::optional<State<int>> secondary_state;
std::optional<WindowHandle> primary_handle;
std::optional<WindowHandle> secondary_handle;
std::optional<ApplicationHandle> captured_application;

View PrimaryContent() {
  ++primary_compositions;
  return Text("primary");
}

View SecondaryContent(std::string label) {
  ++secondary_compositions;
  return Text("secondary: " + label);
}

View CounterContent(bool primary) {
  auto count = UseState(0);
  if (primary) {
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

View PrimaryCounterApp() { return CounterContent(true); }
View SecondaryCounterApp() { return CounterContent(false); }

View CaptureApplicationApp() {
  captured_application = UseApplication();
  return Spacer();
}

void ResetMultiWindowGlobals() {
  primary_compositions = 0;
  secondary_compositions = 0;
  primary_state.reset();
  secondary_state.reset();
  primary_handle.reset();
  secondary_handle.reset();
  captured_application.reset();
}

struct RetirementCounts {
  int destroyed = 0;
  bool inside_discard = false;
  bool destroyed_inside_discard = false;
};

class ReclaimingWindow final : public TestPlatform {
public:
  explicit ReclaimingWindow(RetirementCounts& counts) : counts_(counts) {}
  ~ReclaimingWindow() override {
    ++counts_.destroyed;
    counts_.destroyed_inside_discard |= counts_.inside_discard;
  }
  void DiscardHost() override {
    counts_.inside_discard = true;
    TestPlatform::DiscardHost();
    counts_.inside_discard = false;
  }
private:
  RetirementCounts& counts_;
};

class ReclaimingRuntime final : public TestPlatform {
public:
  explicit ReclaimingRuntime(RetirementCounts& counts) : counts_(counts) {}
  std::unique_ptr<huxerui::UiWindow> CreateWindow(ViewFactory content, const WindowOptions& options) override {
    auto window = std::make_unique<ReclaimingWindow>(counts_);
    detail::InternalAccess::InitializeWindow(*window, *this, std::move(content), options);
    return window;
  }
private:
  RetirementCounts& counts_;
};

} // namespace

TEST_CASE("MultiWindowAttachmentsComposeExplicitContent") {
  ResetMultiWindowGlobals();
  TestPlatform owner;
  Application application{PrimaryContent, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  UiWindow primary(owner, owner);
  const WindowOptions secondary_options{
      .title = "secondary",
      .initial_size = {320.0F, 240.0F},
  };
  TestPlatform secondary_platform;
  const std::string label = "secondary";
  UiWindow secondary(owner, secondary_platform, [label] { return SecondaryContent(label); }, secondary_options);
  primary.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
  secondary.SetWindowMetrics({.viewport = {200.0F, 120.0F}});

  REQUIRE(FirstText(primary.BuildFrame()) == "primary");
  REQUIRE(FirstText(secondary.BuildFrame()) == "secondary: secondary");
  REQUIRE(primary_compositions == 1);
  REQUIRE(secondary_compositions == 1);
}

TEST_CASE("RuntimeOpenWindowCreatesIndependentAttachments") {
  ResetMultiWindowGlobals();
  TestPlatform owner;
  Application application{PrimaryContent, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  const WindowHandle first = UseApplication().OpenWindow(PrimaryContent, application.options.window);
  const WindowHandle second =
      UseApplication().OpenWindow(
          [] { return SecondaryContent("secondary"); },
          {.title = "secondary", .initial_size = {320.0F, 240.0F}}
      );
  REQUIRE(owner.created_windows.size() == 2);
  owner.created_windows[0]->SetWindowMetrics({.viewport = {200.0F, 120.0F}});
  owner.created_windows[1]->SetWindowMetrics({.viewport = {200.0F, 120.0F}});

  const FlattenedScene first_scene = FlattenedFrame(owner.created_windows[0]->BuildFrame());
  const FlattenedScene second_scene = FlattenedFrame(owner.created_windows[1]->BuildFrame());
  REQUIRE(FirstText(first_scene) == "primary");
  REQUIRE(FirstText(second_scene) == "secondary: secondary");
  REQUIRE(first.LifecycleState() == WindowLifecycleState::Background);
  REQUIRE(second.LifecycleState() == WindowLifecycleState::Background);
}

TEST_CASE("MultiWindowStateDoesNotRecomposeOtherWindows") {
  ResetMultiWindowGlobals();
  TestPlatform owner;
  Application application{PrimaryCounterApp, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  UiWindow primary(owner, owner);
  TestPlatform secondary_platform;
  UiWindow secondary(owner, secondary_platform, SecondaryCounterApp, WindowOptions{.title = "secondary"});
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
  Application application{PrimaryCounterApp, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  UiWindow primary(owner, owner);
  {
    TestPlatform secondary_platform;
    UiWindow secondary(owner, secondary_platform, SecondaryCounterApp, WindowOptions{.title = "secondary"});
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
  REQUIRE_THROWS_AS(UseApplication().OpenWindow(PrimaryCounterApp, {}), std::logic_error);

  Application application{PrimaryCounterApp, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  static_cast<void>(UseApplication().OpenWindow(PrimaryCounterApp, application.options.window));
  REQUIRE_THROWS_AS(
      UseApplication().OpenWindow(PrimaryCounterApp, {.initial_size = {0.0F, 0.0F}}), std::invalid_argument);
  REQUIRE_THROWS_AS(UseApplication().OpenWindow(
      PrimaryCounterApp,
      {.initial_position = Point{std::numeric_limits<float>::infinity(), 0.0F}}
  ),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(UseApplication().OpenWindow(
      PrimaryCounterApp,
      {.initial_position = Point{0.0F, std::numeric_limits<float>::quiet_NaN()}}
  ),
                    std::invalid_argument);
  REQUIRE_NOTHROW(UseApplication().OpenWindow(PrimaryCounterApp, {.initial_position = Point{-320.0F, -200.0F}}));

  std::exception_ptr thread_failure;
  std::thread worker([application = UseApplication(), &thread_failure] {
    try {
      static_cast<void>(application.OpenWindow(PrimaryCounterApp));
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
    UiWindow primary(owner, owner);
    primary.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
    primary.BuildFrame();
    REQUIRE(captured_application.has_value());
  }
  REQUIRE_THROWS_AS(captured_application->OpenWindow(PrimaryCounterApp, {}), std::logic_error);
}

TEST_CASE("RuntimeOpenWindowReclaimsRetiredHostsBeforeApplicationShutdown") {
  RetirementCounts counts;
  ReclaimingRuntime owner(counts);
  Application application{[]() -> View { return Spacer(); }, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  std::vector<WindowHandle> retired;
  for (int index = 0; index < 1000; ++index) {
    auto window = UseApplication().OpenWindow([] { return Spacer(); }, {});
    window.Retire();
    REQUIRE(counts.destroyed == index);
    owner.RunPlatformModuleTasks();
    REQUIRE(counts.destroyed == index + 1);
    retired.push_back(window);
  }
  auto replacement = UseApplication().OpenWindow([] { return Spacer(); }, {});
  for (const auto& window : retired) {
    window.Show();
    window.Close();
    window.Retire();
    REQUIRE_FALSE(window.Position().has_value());
  }
  owner.RunPlatformModuleTasks();
  REQUIRE(counts.destroyed == 1000);
  replacement.Retire();
  owner.RunPlatformModuleTasks();
  REQUIRE(counts.destroyed == 1001);
  auto parent = UseApplication().OpenWindow([] { return Spacer(); }, {});
  auto child = UseApplication().OpenWindow([] { return Spacer(); }, {}, {.owner = parent});
  auto grandchild = UseApplication().OpenWindow([] { return Spacer(); }, {}, {.owner = child});
  parent.Retire();
  owner.RunPlatformModuleTasks();
  REQUIRE(counts.destroyed == 1004);
  REQUIRE_FALSE(child.Position().has_value());
  REQUIRE_FALSE(grandchild.Position().has_value());
  REQUIRE_FALSE(counts.destroyed_inside_discard);
}

TEST_CASE("RuntimeOpenWindowRetirementMayRaceApplicationShutdown") {
  std::mutex mutex;
  std::vector<std::function<void()>> tasks;
  TestPlatform owner([&](std::function<void()> callback) {
    std::lock_guard lock(mutex);
    tasks.push_back(std::move(callback));
  });
  Application application{[]() -> View { return Spacer(); }, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  auto window = UseApplication().OpenWindow([] { return Spacer(); }, {});
  std::atomic<int> submissions{0};
  std::atomic<bool> stopped{false};
  std::thread worker([&] {
    while (!stopped.load()) {
      window.Retire();
      ++submissions;
    }
    window.Retire();
  });
  while (submissions.load() < 100) std::this_thread::yield();
  detail::InternalAccess::RetireRuntime(owner);
  stopped = true;
  worker.join();
  REQUIRE(submissions.load() >= 100);
  for (auto& task : tasks) REQUIRE_NOTHROW(task());
  REQUIRE_NOTHROW(window.Retire());
}

} // namespace huxerui::test
