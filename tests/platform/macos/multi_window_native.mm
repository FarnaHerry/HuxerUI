#import <AppKit/AppKit.h>
#include <catch2/catch_amalgamated.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <huxerui/huxerui.h>

namespace huxerui::test {
namespace {

struct SharedCounter {
  State<int> value{0};
};

struct WindowRecord {
  WindowIdentity identity;
  std::optional<WindowHandle> handle;
  std::optional<State<int>> local;
  int compositions = 0;
  int unmounts = 0;
  int shared_value = 0;
};

struct NativeWatch {
  __weak NSWindow* window;
  __weak NSView* canvas;

  explicit NativeWatch(NSWindow* native) : window(native), canvas(native.contentView) {}

  NSWindow* Window() { return window; }
  bool Released() { return window == nil && canvas == nil; }

  bool SendEscape() {
    NSView* view = canvas;
    if (view == nil) return false;
    NSEvent* event = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                    location:NSZeroPoint
                               modifierFlags:0
                                   timestamp:0
                                windowNumber:view.window.windowNumber
                                     context:nil
                                  characters:@"\033"
                 charactersIgnoringModifiers:@"\033"
                                   isARepeat:NO
                                     keyCode:53];
    [view keyDown:event];
    return true;
  }
};

bool IsInteractive(NSWindow* window) {
  return window != nil && !window.ignoresMouseEvents && window.canBecomeKeyWindow && window.canBecomeMainWindow;
}

struct NativeScenario {
  std::map<std::uint32_t, WindowRecord> records;
  std::optional<ApplicationHandle> application;
  std::shared_ptr<SharedCounter> shared;
  std::vector<std::unique_ptr<NativeWatch>> watches;
  std::function<bool()> advance;
  std::exception_ptr failure;
  std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  bool block_extra_close = true;
  int close_requests = 0;
  bool completed = false;
  bool native_primary_close = false;
  bool destroy_primary = false;
  bool key_close_returned = false;

  void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string("HuxerUI ") + message);
  }

  void Watch(const std::string& title) {
    for (NSWindow* window in NSApplication.sharedApplication.windows) {
      if (title == window.title.UTF8String) {
        watches.push_back(std::make_unique<NativeWatch>(window));
        return;
      }
    }
    throw std::runtime_error("HuxerUI native test window was not found");
  }
};

NativeScenario* current_scenario = nullptr;

View NativeMultiWindowRoot() {
  auto& scenario = *current_scenario;
  const auto identity = UseEnvironment<WindowIdentity>();
  if (identity.key == "throw") throw std::runtime_error("HuxerUI native test composition failure");
  auto& record = scenario.records[identity.ordinal];
  record.identity = identity;
  record.handle = UseWindow();
  record.local = UseState(0);
  record.shared_value = UseService<SharedCounter>()->value.Get();
  ++record.compositions;
  scenario.application = UseApplication();
  Lifecycle([&record] { return [&record] { ++record.unmounts; }; });
  if (identity.ordinal == 1) {
    record.handle->OnCloseRequest([&scenario] {
      ++scenario.close_requests;
      return scenario.block_extra_close;
    });
  }
  return Text::Format("{}: {} / {}", identity.key, record.local->Get(), record.shared_value)
      .On<ViewEvents::KeyIntercept>([&scenario, &record, window = *record.handle](const KeyEvent& event) {
        if (record.identity.key != "key-close" || event.type != KeyEventType::Down || event.key != Key::Escape) {
          return false;
        }
        window.Close();
        scenario.key_close_returned = record.unmounts == 0;
        return true;
      });
}

void RunScenario(NativeScenario& scenario, const std::string& expected_failure = {}) {
  current_scenario = &scenario;
  REQUIRE(NSThread.isMainThread);
  NSTimer* timer = [NSTimer scheduledTimerWithTimeInterval:0.01 repeats:YES block:^(NSTimer* sender) {
    @autoreleasepool {
      auto& scenario = *current_scenario;
      try {
        scenario.Check(std::chrono::steady_clock::now() < scenario.deadline, "Native multi-window test timed out");
        if (!scenario.advance()) return;
        scenario.completed = true;
      } catch (...) {
        scenario.failure = std::current_exception();
      }
      [sender invalidate];
      if (scenario.completed && scenario.native_primary_close) {
        NSWindow* native = scenario.watches.front()->Window();
        if (scenario.destroy_primary) [native close];
        else [native performClose:nil];
      } else if (scenario.records.contains(0) && scenario.records.at(0).unmounts == 0) {
        scenario.records.at(0).handle->Close();
      } else if (scenario.application) {
        scenario.application->Quit();
      }
    }
  }];
  std::exception_ptr runner_failure;
  try {
    const Application application{
        NativeMultiWindowRoot,
        {
            .window = {.title = "HuxerUI native primary", .key = "primary", .initial_size = {320.0F, 240.0F}},
            .extra_windows = {{
                .title = "HuxerUI native extra", .key = "extra", .initial_size = {200.0F, 160.0F},
                .minimum_size = Size{360.0F, 220.0F}, .chrome_mode = WindowChromeMode::Custom,
            }},
            .show_debug_overlay = false,
            .application_hooks = {[&scenario](ApplicationContext& context) {
              scenario.shared = std::make_shared<SharedCounter>();
              context.Provide(scenario.shared);
            }},
        },
    };
    @autoreleasepool {
      scenario.Check(detail::RunPlatformApplication(application) == 0, "Native application returned a failure code");
    }
  } catch (...) {
    runner_failure = std::current_exception();
  }
  [timer invalidate];
  current_scenario = nullptr;
  if (scenario.failure) std::rethrow_exception(scenario.failure);
  if (expected_failure.empty()) {
    if (runner_failure) std::rethrow_exception(runner_failure);
    REQUIRE(scenario.completed);
  } else {
    REQUIRE(runner_failure != nullptr);
    REQUIRE_THROWS_WITH(std::rethrow_exception(runner_failure), expected_failure);
  }
  for (const auto& [ordinal, record] : scenario.records) {
    INFO(ordinal);
    REQUIRE(record.unmounts == 1);
    record.handle->Show();
    record.handle->Close();
  }
  for (const auto& watch : scenario.watches) REQUIRE(watch->Released());
}

} // namespace

TEST_CASE("MacNativeWindowsIsolateStateAndReleaseClosedAttachments", "[macos-native]") {
  NativeScenario scenario;
  SECTION("The initial handle requests application shutdown") {}
  SECTION("Native initial-window close requests application shutdown") { scenario.native_primary_close = true; }
  SECTION("Native initial-window destruction requests application shutdown") {
    scenario.native_primary_close = true;
    scenario.destroy_primary = true;
  }
  int phase = 0;
  int primary_compositions = 0;
  int extra_compositions = 0;
  std::uint32_t cycle_ordinal = 2;
  scenario.advance = [&] {
    auto& records = scenario.records;
    if (!records.contains(0) || !records.contains(1)) return false;
    auto& primary = records.at(0);
    auto& extra = records.at(1);
    switch (phase) {
    case 0: {
      scenario.Check(primary.identity.key == "primary" && extra.identity.key == "extra", "Window keys mismatch");
      scenario.Watch("HuxerUI native primary");
      scenario.Watch("HuxerUI native extra");
      NSWindow* native = scenario.watches.back()->Window();
      const NSSize size = native.contentView.bounds.size;
      const bool valid_options = size.width >= 360 && size.height >= 220 &&
          (native.styleMask & NSWindowStyleMaskFullSizeContentView) != 0;
      scenario.Check(valid_options, "Extra window did not apply its own size or chrome");
      primary_compositions = primary.compositions;
      extra_compositions = extra.compositions;
      *primary.local = 1;
      ++phase;
      return false;
    }
    case 1:
      if (primary.compositions == primary_compositions) return false;
      scenario.Check(extra.compositions == extra_compositions, "Local state recomposed another window");
      scenario.shared->value = 1;
      ++phase;
      return false;
    case 2:
      if (primary.shared_value != 1 || extra.shared_value != 1) return false;
      extra.handle->Close();
      scenario.Check(scenario.close_requests == 1 && extra.unmounts == 0, "Close interception failed");
      scenario.block_extra_close = false;
      {
        [scenario.watches.back()->Window() performClose:nil];
      }
      ++phase;
      return false;
    case 3:
      if (!scenario.watches.back()->Released() || extra.unmounts != 1) return false;
      extra.handle->Activate();
      extra.handle->Close();
      scenario.Check(primary.unmounts == 0, "Closing an extra window retired the primary");
      {
        const NSUInteger baseline = NSApplication.sharedApplication.windows.count;
        bool rejected = false;
        try {
          static_cast<void>(scenario.application->OpenWindow({.title = "Invalid", .title_bar_height = -1.0F}));
        } catch (const std::invalid_argument&) {
          rejected = true;
        }
        scenario.Check(rejected, "Invalid window options were accepted");
        scenario.Check(NSApplication.sharedApplication.windows.count == baseline,
                       "Failed window creation retained an AppKit window");
        // Failed creation consumes an ordinal; successful windows must not depend on contiguous identities.
        ++cycle_ordinal;
      }
      ++phase;
      return false;
    case 4:
      static_cast<void>(scenario.application->OpenWindow({.title = "HuxerUI native cycle", .key = "cycle"}));
      ++phase;
      return false;
    case 5:
      if (!records.contains(cycle_ordinal)) return false;
      scenario.Watch("HuxerUI native cycle");
      scenario.Check(records.at(cycle_ordinal).local->Get() == 0, "New window reused retired state");
      if (cycle_ordinal == 6) primary.handle->Hide();
      if (cycle_ordinal % 2 == 0) {
        records.at(cycle_ordinal).handle->Close();
      }
      if (cycle_ordinal % 2 != 0 || cycle_ordinal == 4) {
        [scenario.watches.back()->Window() close];
      }
      ++phase;
      return false;
    case 6:
      if (!scenario.watches.back()->Released() || records.at(cycle_ordinal).unmounts != 1) return false;
      scenario.Check(primary.unmounts == 0, "Runtime close stopped the application");
      if (++cycle_ordinal <= 6) {
        phase = 4;
      } else {
        primary.handle->Show();
        static_cast<void>(scenario.application->OpenWindow({.title = "HuxerUI native survivor", .key = "survivor"}));
        ++phase;
      }
      return false;
    default:
      if (!records.contains(cycle_ordinal)) return false;
      scenario.Watch("HuxerUI native survivor");
      return true;
    }
  };
  REQUIRE_NOTHROW(RunScenario(scenario));
}

TEST_CASE("MacNativeWindowFrameFailuresPropagateFromTheSharedLoop", "[macos-native]") {
  NativeScenario scenario;
  bool opened = false;
  scenario.advance = [&] {
    if (!opened && scenario.application && scenario.records.contains(1)) {
      opened = true;
      scenario.Watch("HuxerUI native primary");
      scenario.Watch("HuxerUI native extra");
      static_cast<void>(scenario.application->OpenWindow({.title = "HuxerUI native failure", .key = "throw"}));
      scenario.Watch("HuxerUI native failure");
    }
    return false;
  };
  REQUIRE_NOTHROW(RunScenario(scenario, "HuxerUI native test composition failure"));
}

TEST_CASE("MacNativeWindowCloseWaitsForKeyDispatchToReturn", "[macos-native]") {
  NativeScenario scenario;
  int phase = 0;
  scenario.advance = [&] {
    if (!scenario.records.contains(0) || !scenario.records.contains(1)) return false;
    switch (phase) {
    case 0:
      scenario.Watch("HuxerUI native primary");
      scenario.Watch("HuxerUI native extra");
      static_cast<void>(scenario.application->OpenWindow({.title = "HuxerUI native key close", .key = "key-close"}));
      ++phase;
      return false;
    case 1:
      if (!scenario.records.contains(2)) return false;
      scenario.Watch("HuxerUI native key close");
      scenario.Check(scenario.watches.back()->SendEscape(), "Native close shortcut was not handled");
      scenario.Check(scenario.key_close_returned, "Closing a window retired its in-flight key handler");
      ++phase;
      return false;
    default:
      if (!scenario.watches.back()->Released()) return false;
      scenario.Check(scenario.records.at(2).unmounts == 1, "Closed window was not unmounted exactly once");
      scenario.Check(scenario.records.at(0).unmounts == 0, "Close shortcut retired the primary window");
      return true;
    }
  };
  REQUIRE_NOTHROW(RunScenario(scenario));
}

TEST_CASE("MacNativeOwnedModalsPreserveIndependentWindowsAndCascade", "[macos-native]") {
  NativeScenario scenario;
  bool destroy_owner = false;
  SECTION("Close retires the owned chain") {}
  SECTION("Native destruction retires the owned chain") { destroy_owner = true; }
  int phase = 0;
  scenario.advance = [&] {
    auto& records = scenario.records;
    if (!records.contains(0) || !records.contains(1)) return false;
    switch (phase) {
    case 0:
      scenario.Watch("HuxerUI native primary");
      scenario.Watch("HuxerUI native extra");
      static_cast<void>(scenario.application->OpenWindow(
          {.title = "HuxerUI native modal", .key = "modal"}, {.owner = *records.at(1).handle, .modal = true}));
      ++phase;
      return false;
    case 1: {
      if (!records.contains(2)) return false;
      scenario.Watch("HuxerUI native modal");
      NSWindow* primary = scenario.watches[0]->Window();
      NSWindow* owner = scenario.watches[1]->Window();
      NSWindow* modal = scenario.watches[2]->Window();
      scenario.Check(modal.parentWindow == owner, "Native owner was not applied");
      scenario.Check(IsInteractive(modal), "Modal window was blocked");
      scenario.Check(!IsInteractive(owner), "Modal owner remained interactive");
      scenario.Check(IsInteractive(primary), "Independent window was blocked");
      static_cast<void>(scenario.application->OpenWindow(
          {.title = "HuxerUI native helper"}, {.owner = *records.at(2).handle}));
      ++phase;
      return false;
    }
    case 2:
      if (!records.contains(3)) return false;
      scenario.Watch("HuxerUI native helper");
      scenario.Check(IsInteractive(scenario.watches[3]->Window()),
                     "Modal-owned helper was blocked");
      static_cast<void>(scenario.application->OpenWindow(
          {.title = "HuxerUI native newer modal"}, {.owner = *records.at(1).handle, .modal = true}));
      ++phase;
      return false;
    case 3:
      if (!records.contains(4)) return false;
      scenario.Watch("HuxerUI native newer modal");
      scenario.Check(!IsInteractive(scenario.watches[2]->Window()),
                     "Older sibling modal remained interactive");
      scenario.Check(!IsInteractive(scenario.watches[3]->Window()),
                     "Older modal's helper remained interactive");
      scenario.Check(IsInteractive(scenario.watches[4]->Window()),
                     "Newest modal was blocked");
      records.at(4).handle->Retire();
      ++phase;
      return false;
    case 4:
      if (!scenario.watches[4]->Released()) return false;
      scenario.Check(IsInteractive(scenario.watches[2]->Window()),
                     "Previous modal was not restored");
      scenario.Check(IsInteractive(scenario.watches[3]->Window()),
                     "Previous modal's helper was not restored");
      SetWindowRelationship(*records.at(2).handle, {.owner = *records.at(0).handle});
      scenario.Check(IsInteractive(scenario.watches[1]->Window()),
                     "Reparenting left the old owner blocked");
      scenario.Check(scenario.watches[2]->Window().parentWindow == scenario.watches[0]->Window(), "Native reparenting was not applied");
      SetWindowRelationship(*records.at(2).handle, {.owner = *records.at(1).handle, .modal = true});
      scenario.block_extra_close = false;
      if (destroy_owner) {
        [scenario.watches[1]->Window() close];
      } else {
        records.at(1).handle->Close();
      }
      ++phase;
      return false;
    default:
      if (!scenario.watches[1]->Released() || !scenario.watches[2]->Released() ||
          !scenario.watches[3]->Released()) return false;
      scenario.Check(records.at(0).unmounts == 0, "Owned cascade retired an independent window");
      scenario.Check(IsInteractive(scenario.watches[0]->Window()),
                     "Owned cascade left an independent window blocked");
      return true;
    }
  };
  REQUIRE_NOTHROW(RunScenario(scenario));
}

TEST_CASE("MacNativeApplicationModalsStackAndCoverNewWindows", "[macos-native]") {
  NativeScenario scenario;
  int phase = 0;
  scenario.advance = [&] {
    auto& records = scenario.records;
    if (!records.contains(0) || !records.contains(1)) return false;
    switch (phase) {
    case 0:
      scenario.Watch("HuxerUI native primary");
      scenario.Watch("HuxerUI native extra");
      static_cast<void>(scenario.application->OpenWindow(
          {.title = "HuxerUI native app modal"}, {.modal = true}));
      ++phase;
      return false;
    case 1:
      if (!records.contains(2)) return false;
      scenario.Watch("HuxerUI native app modal");
      static_cast<void>(scenario.application->OpenWindow({.title = "HuxerUI native late window", .key = "key-close"}));
      ++phase;
      return false;
    case 2:
      if (!records.contains(3)) return false;
      scenario.Watch("HuxerUI native late window");
      for (const std::size_t index : {0U, 1U, 3U}) {
        scenario.Check(!IsInteractive(scenario.watches[index]->Window()),
                       "Application modal did not cover every other window");
      }
      static_cast<void>(scenario.watches[3]->SendEscape());
      scenario.Check(!scenario.key_close_returned, "Blocked window dispatched a queued key event");
      static_cast<void>(scenario.application->OpenWindow(
          {.title = "HuxerUI native top app modal"}, {.modal = true}));
      ++phase;
      return false;
    case 3:
      if (!records.contains(4)) return false;
      scenario.Watch("HuxerUI native top app modal");
      scenario.Check(!IsInteractive(scenario.watches[2]->Window()),
                     "Previous application modal remained interactive");
      scenario.Check(IsInteractive(scenario.watches[4]->Window()),
                     "Top application modal was blocked");
      records.at(4).handle->Retire();
      ++phase;
      return false;
    case 4:
      if (!scenario.watches[4]->Released()) return false;
      scenario.Check(IsInteractive(scenario.watches[2]->Window()),
                     "Previous application modal did not resume");
      SetWindowRelationship(*records.at(2).handle, {});
      for (std::size_t index = 0; index != 4; ++index) {
        scenario.Check(IsInteractive(scenario.watches[index]->Window()),
                       "Removing modality did not restore input");
      }
      scenario.Check(scenario.watches[3]->SendEscape(), "Unblocked window did not accept keyboard input");
      scenario.Check(scenario.key_close_returned, "Unblocked close handler retired synchronously");
      ++phase;
      return false;
    default:
      return scenario.watches[3]->Released();
    }
    return false;
  };
  REQUIRE_NOTHROW(RunScenario(scenario));
}

TEST_CASE("MacNativeRetirementKeepsWindowlessRuntimeAliveAndCanReopen", "[macos-native]") {
  NativeScenario scenario;
  int phase = 0;
  scenario.advance = [&] {
    auto& records = scenario.records;
    if (!records.contains(0) || !records.contains(1)) return false;
    switch (phase) {
    case 0:
      scenario.Watch("HuxerUI native primary");
      scenario.Watch("HuxerUI native extra");
      SetWindowRelationship(*records.at(1).handle, {.owner = *records.at(0).handle, .modal = true});
      records.at(0).handle->Retire();
      scenario.Check(records.at(0).unmounts == 0 && records.at(1).unmounts == 0,
                     "Retirement did not wait for the current callback");
      ++phase;
      return false;
    case 1:
      if (!scenario.watches[0]->Released() || !scenario.watches[1]->Released()) return false;
      scenario.Check(records.at(0).unmounts == 1 && records.at(1).unmounts == 1,
                     "Retirement did not unmount the owned chain exactly once");
      scenario.Check(scenario.close_requests == 0, "Retirement invoked close handlers");
      scenario.shared->value = 42;
      records.at(0).handle->Retire();
      records.at(1).handle->Retire();
      static_cast<void>(scenario.application->OpenWindow({.title = "HuxerUI native reopened"}));
      ++phase;
      return false;
    default:
      if (!records.contains(2)) return false;
      scenario.Watch("HuxerUI native reopened");
      scenario.Check(records.at(2).shared_value == 42, "Reopened UI lost its application service");
      scenario.Check(records.at(2).local->Get() == 0, "Reopened UI reused retired local state");
      return true;
    }
  };
  REQUIRE_NOTHROW(RunScenario(scenario));
}

} // namespace huxerui::test
