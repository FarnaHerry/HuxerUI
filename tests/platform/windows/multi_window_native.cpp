#include <catch2/catch_amalgamated.hpp>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
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
      const HWND primary = primary_window.load();
      if (primary != nullptr && IsWindow(primary)) {
        PostMessageW(primary, WM_CLOSE, 0, 0);
      }
      runner.join();
    }
  }
};

} // namespace

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
    {
      std::lock_guard lock(test_state_mutex);
      const ApplicationHandle application = *captured_application;
      application_dispatch([application, secondary_title_narrow] {
        static_cast<void>(application.OpenWindow({
            .title = secondary_title_narrow,
            .initial_size = {320.0F, 220.0F},
        }));
      });
    }
    PumpUntil([&] { return FindWindowByTitle(secondary_title) != nullptr; }, "the secondary window");
    const HWND secondary = FindWindowByTitle(secondary_title);
    REQUIRE(secondary != nullptr);
    REQUIRE(IsWindow(secondary));
    REQUIRE(secondary != primary_window.load());

    PostMessageW(secondary, WM_CLOSE, 0, 0);
    PumpUntil([&secondary] { return !IsWindow(secondary); }, "secondary window destruction");
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
