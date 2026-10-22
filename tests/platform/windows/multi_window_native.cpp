#include <catch2/catch_amalgamated.hpp>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <huxerui/huxerui.h>

#include "win32_application_runner.h"

using namespace huxerui;

namespace huxerui::test {

namespace {

std::mutex test_state_mutex;
std::optional<ApplicationHandle> captured_application;
UiThreadDispatcher application_dispatch;
std::atomic<HWND> primary_window{nullptr};
std::exception_ptr runner_failure;
std::atomic<bool> runner_finished{false};

View NativeWindowLifecycleApp() {
  {
    std::lock_guard lock(test_state_mutex);
    if (!captured_application.has_value()) {
      captured_application = UseApplication();
    }
  }
  return Spacer();
}

void PumpUntil(const std::function<bool()>& condition, const char* description) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!condition()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      FAIL(std::string{"Timed out waiting for "} + description);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

HWND FindWindowByTitle(const std::wstring& title) {
  struct Search {
    const std::wstring* title;
    HWND result = nullptr;
  };
  Search search{&title, nullptr};
  const BOOL enumerated = EnumWindows(
      [](HWND window, LPARAM parameter) -> BOOL {
        auto& search = *reinterpret_cast<Search*>(parameter);
        wchar_t text[256] = {};
        if (GetWindowTextW(window, text, static_cast<int>(std::size(text))) == 0) {
          return TRUE;
        }
        if (*search.title == text) {
          search.result = window;
          return FALSE;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&search)
  );
  static_cast<void>(enumerated);
  return search.result;
}

struct RunnerGuard {
  std::thread runner;
  ~RunnerGuard() {
    if (runner.joinable()) {
      std::optional<ApplicationHandle> application;
      UiThreadDispatcher post;
      {
        std::lock_guard lock(test_state_mutex);
        application = captured_application;
        post = application_dispatch;
      }
      if (application && post) post([application = *application] { application.Quit(); });
      const HWND primary = primary_window.load();
      if (primary != nullptr && IsWindow(primary)) {
        PostMessageW(primary, WM_CLOSE, 0, 0);
      }
      runner.join();
    }
  }
};

struct StartupWindowState {
  std::atomic<int> windows{0};
  std::atomic<int> compositions{0};
  std::atomic<bool> timer_completed{false};
  std::optional<WindowHandle> delayed;
};

View StartupWindowApp() {
  ++UseService<StartupWindowState>()->compositions;
  return Spacer();
}

} // namespace

TEST_CASE("Win32StartupCanDeferTheInitialWindowWithoutStoppingApplicationDispatch", "[.][win32-native-startup]") {
  AppOptions options;
  int expected_startup_windows = 0;
  bool open_later = true;
  SECTION("Default startup still opens the initial window") { expected_startup_windows = 1; }
  SECTION("Application starts without a UI") { options.create_initial_window = false; }
  SECTION("Explicit extra windows are independent of the initial window") {
    options.create_initial_window = false;
    options.extra_windows.push_back({.title = "HuxerUI explicit extra", .initial_size = {240.0F, 160.0F}});
    expected_startup_windows = 1;
  }
  SECTION("Application can quit without ever creating a UI") {
    options.create_initial_window = false;
    open_later = false;
  }
  {
    std::lock_guard lock(test_state_mutex);
    captured_application.reset();
    application_dispatch = {};
  }
  primary_window.store(nullptr);
  runner_failure = nullptr;
  runner_finished.store(false);
  auto state = std::make_shared<StartupWindowState>();
  const std::string initial_title = "HuxerUI startup initial " + std::to_string(GetCurrentProcessId());
  const std::string delayed_title = "HuxerUI startup delayed " + std::to_string(GetCurrentProcessId());
  const std::wstring delayed_title_wide(delayed_title.begin(), delayed_title.end());
  options.window = {.title = initial_title, .initial_size = {240.0F, 160.0F}};
  options.show_debug_overlay = false;
  options.application_hooks.push_back([state](ApplicationContext& context) {
    context.Provide(state);
    const auto tasks = UseApplicationTaskScope();
    {
      std::lock_guard lock(test_state_mutex);
      captured_application = UseApplication();
      application_dispatch = [tasks](std::function<void()> callback) { tasks.Post(std::move(callback)); };
    }
    tasks.Launch([state]() -> Task<void> {
      co_await Delay(std::chrono::milliseconds(100));
      state->timer_completed.store(true);
    });
  });
  options.window_hooks.push_back([state](WindowContext&) { ++state->windows; });
  const Application application{StartupWindowApp, std::move(options)};
  RunnerGuard guard;
  guard.runner = std::thread([&application] {
    try {
      static_cast<void>(detail::RunWin32PlatformApplication(application));
    } catch (...) {
      runner_failure = std::current_exception();
    }
    runner_finished.store(true);
  });
  PumpUntil([&] { return state->timer_completed.load() || runner_finished.load(); }, "windowless application timer");
  if (runner_finished.load()) {
    guard.runner.join();
    if (runner_failure) std::rethrow_exception(runner_failure);
  }
  REQUIRE_FALSE(runner_finished.load());
  REQUIRE(state->windows.load() == expected_startup_windows);
  const std::wstring initial_title_wide(initial_title.begin(), initial_title.end());
  REQUIRE((FindWindowByTitle(initial_title_wide) != nullptr) == application.options.create_initial_window);
  if (expected_startup_windows == 0) REQUIRE(state->compositions.load() == 0);

  ApplicationHandle handle = [&] {
    std::lock_guard lock(test_state_mutex);
    return *captured_application;
  }();
  UiThreadDispatcher post = [&] {
    std::lock_guard lock(test_state_mutex);
    return application_dispatch;
  }();
  if (open_later) {
    for (int cycle = 0; cycle != 2; ++cycle) {
      post([handle, state, delayed_title] {
        state->delayed = handle.OpenWindow({.title = delayed_title, .initial_size = {240.0F, 160.0F}});
      });
      PumpUntil([&] { return FindWindowByTitle(delayed_title_wide) != nullptr; }, "the delayed window");
      const HWND native = FindWindowByTitle(delayed_title_wide);
      PumpUntil([&] { return state->windows.load() == expected_startup_windows + cycle + 1; }, "window initialization");
      post([state] { state->delayed->Retire(); });
      PumpUntil([native] { return !IsWindow(native); }, "delayed window retirement");
      REQUIRE_FALSE(runner_finished.load());
    }
  }
  post([handle] { handle.Quit(); });
  PumpUntil([] { return runner_finished.load(); }, "explicit application shutdown");
  guard.runner.join();
  if (runner_failure) std::rethrow_exception(runner_failure);
  REQUIRE(state->windows.load() == expected_startup_windows + (open_later ? 2 : 0));
}

// Hidden: creates real top-level windows and Direct3D devices, so it runs only on demand through the
// [win32-native] tag instead of the default platform test pass.
TEST_CASE("Win32RuntimeWindowsOpenAndCloseWithoutRetainingHandles", "[.][win32-native]") {
  {
    std::lock_guard lock(test_state_mutex);
    captured_application.reset();
    application_dispatch = {};
  }
  primary_window.store(nullptr);
  runner_failure = nullptr;
  runner_finished.store(false);

  const std::wstring secondary_title =
      L"HuxerUI MW Native Secondary " + std::to_wstring(GetCurrentProcessId());
  const std::string secondary_title_narrow =
      "HuxerUI MW Native Secondary " + std::to_string(GetCurrentProcessId());
  const Application application{
      NativeWindowLifecycleApp,
      {
          .window =
              {
                  .title = "HuxerUI MW Native Primary " + std::to_string(GetCurrentProcessId()),
                  .initial_size = {360.0F, 240.0F},
              },
      },
  };

  RunnerGuard guard;
  guard.runner = std::thread([&application] {
    try {
      static_cast<void>(detail::RunWin32PlatformApplication(
          application,
          [](UiThreadDispatcher dispatch, HWND window) {
            {
              std::lock_guard lock(test_state_mutex);
              application_dispatch = std::move(dispatch);
            }
            primary_window.store(window);
          }
      ));
    } catch (...) {
      runner_failure = std::current_exception();
    }
    runner_finished.store(true);
  });

  PumpUntil([] { return primary_window.load() != nullptr; }, "the initial window");
  PumpUntil(
      [] {
        std::lock_guard lock(test_state_mutex);
        return captured_application.has_value();
      },
      "the first composition"
  );
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  const DWORD baseline_gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
  const DWORD baseline_user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);

  for (int cycle = 0; cycle < 2; ++cycle) {
    INFO("cycle " << cycle);
    const Point preferred_origin{120.0F + static_cast<float>(cycle) * 40.0F, 160.0F};
    auto placement = std::make_shared<std::promise<std::optional<Point>>>();
    auto placement_result = placement->get_future();
    {
      std::lock_guard lock(test_state_mutex);
      const ApplicationHandle application = *captured_application;
      application_dispatch([application, secondary_title_narrow, preferred_origin, placement, cycle] {
        const WindowHandle window = application.OpenWindow({
            .title = secondary_title_narrow,
            .initial_size = {320.0F, 220.0F},
            .initial_position = preferred_origin,
            .chrome_mode = cycle == 0 ? WindowChromeMode::System : WindowChromeMode::Custom,
        });
        placement->set_value(window.Position());
      });
    }
    REQUIRE(placement_result.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    const auto origin = placement_result.get();
    REQUIRE(origin.has_value());
    REQUIRE(std::abs(origin->x - preferred_origin.x) <= 2.0F);
    REQUIRE(std::abs(origin->y - preferred_origin.y) <= 2.0F);
    PumpUntil([&] { return FindWindowByTitle(secondary_title) != nullptr; }, "the secondary window");
    const HWND secondary = FindWindowByTitle(secondary_title);
    if (secondary == nullptr) {
      PumpUntil([] { return runner_finished.load(); }, "creation failure propagation from the native runner");
      guard.runner.join();
      if (runner_failure) REQUIRE_NOTHROW(std::rethrow_exception(runner_failure));
    }
    REQUIRE(secondary != nullptr);
    REQUIRE(IsWindow(secondary));
    REQUIRE(secondary != primary_window.load());

    PostMessageW(secondary, WM_CLOSE, 0, 0);
    PumpUntil([&secondary] { return !IsWindow(secondary); }, "secondary window destruction");
    if (!IsWindow(primary_window.load())) {
      PumpUntil([] { return runner_finished.load(); }, "failure propagation from the native runner");
      guard.runner.join();
      if (runner_failure) REQUIRE_NOTHROW(std::rethrow_exception(runner_failure));
    }
    REQUIRE(IsWindow(primary_window.load()));
  }

  const DWORD final_gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
  const DWORD final_user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
  REQUIRE(final_gdi <= baseline_gdi + 8);
  REQUIRE(final_user <= baseline_user + 4);

  const HWND primary = primary_window.load();
  REQUIRE(primary != nullptr);
  PostMessageW(primary, WM_CLOSE, 0, 0);
  guard.runner.join();

  if (runner_failure != nullptr) {
    std::rethrow_exception(runner_failure);
  }
  REQUIRE(runner_finished.load());
}

} // namespace huxerui::test
