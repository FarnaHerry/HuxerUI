#include <catch2/catch_amalgamated.hpp>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <huxerui/huxerui.h>

using namespace huxerui;

namespace huxerui::test {

namespace {

std::mutex test_state_mutex;
std::optional<ApplicationHandle> captured_application;
std::function<void(std::function<void()>)> app_thread_post;
std::optional<WindowHandle> primary_window;
std::optional<WindowHandle> secondary_window;
std::atomic<int> live_secondary_windows{0};
std::atomic<bool> runner_finished{false};
std::exception_ptr runner_failure;

View NativeWindowLifecycleApp() {
  const WindowIdentity identity = UseEnvironment<WindowIdentity>();
  const WindowHandle window = UseWindow();
  if (identity.ordinal == 0) {
    const std::lock_guard lock(test_state_mutex);
    primary_window = window;
  } else {
    {
      const std::lock_guard lock(test_state_mutex);
      secondary_window = window;
    }
    live_secondary_windows.fetch_add(1, std::memory_order_release);
    huxerui::Lifecycle([] {
      return [] { live_secondary_windows.fetch_sub(1, std::memory_order_release); };
    });
  }
  {
    const std::lock_guard lock(test_state_mutex);
    if (!captured_application.has_value()) {
      captured_application = UseApplication();
    }
    if (!app_thread_post) {
      auto tasks = UseTaskScope();
      app_thread_post = [tasks](std::function<void()> task) { tasks.Post(std::move(task)); };
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

} // namespace

// Hidden: creates real GTK toplevels on the developer's display, so it runs only on demand through the
// [linux-native] tag instead of the default platform test pass.
TEST_CASE("LinuxRuntimeWindowsOpenAndCloseIndependently", "[.][linux-native]") {
  {
    std::lock_guard lock(test_state_mutex);
    captured_application.reset();
    app_thread_post = nullptr;
    primary_window.reset();
    secondary_window.reset();
  }
  live_secondary_windows.store(0, std::memory_order_release);
  runner_failure = nullptr;
  runner_finished.store(false);

  const std::string process = std::to_string(static_cast<long long>(::getpid()));
  const Application application{
      NativeWindowLifecycleApp,
      {
          .window =
              {
                  .title = "HuxerUI MW Native Primary " + process,
                  .initial_size = {360.0F, 240.0F},
              },
      },
  };

  std::thread runner{[&application] {
    try {
      static_cast<void>(detail::RunPlatformApplication(application));
    } catch (...) {
      runner_failure = std::current_exception();
    }
    runner_finished.store(true);
  }};

  PumpUntil([&] {
    std::lock_guard lock(test_state_mutex);
    return primary_window.has_value();
  }, "the initial window");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  for (int cycle = 0; cycle < 2; ++cycle) {
    std::function<void(std::function<void()>)> post;
    {
      std::lock_guard lock(test_state_mutex);
      REQUIRE(captured_application.has_value());
      REQUIRE(app_thread_post != nullptr);
      post = app_thread_post;
    }
    const Point preferred_origin{80.0F + static_cast<float>(cycle) * 40.0F, 60.0F};
    std::optional<ApplicationHandle> application_handle;
    {
      std::lock_guard lock(test_state_mutex);
      application_handle = captured_application;
    }
    post([handle = *application_handle, process, preferred_origin] {
      static_cast<void>(handle.OpenWindow({
          .title = "HuxerUI MW Native Secondary " + process,
          .initial_size = {320.0F, 220.0F},
          .initial_position = preferred_origin,
      }));
    });
    PumpUntil([&] { return live_secondary_windows.load(std::memory_order_acquire) > 0; },
              "the secondary window composition");
    std::optional<WindowHandle> secondary;
    {
      std::lock_guard lock(test_state_mutex);
      REQUIRE(secondary_window.has_value());
      secondary = secondary_window;
    }

    // Placement is best-effort: X11 honors the request, Wayland compositors ignore it. GTK calls must run
    // on the application thread, so the origin query posts there and the result comes back through a promise.
    std::optional<Point> origin;
    for (int attempt = 0; attempt < 200 && !origin.has_value(); ++attempt) {
      auto placement = std::make_shared<std::promise<std::optional<Point>>>();
      std::future<std::optional<Point>> placement_result = placement->get_future();
      {
        std::lock_guard lock(test_state_mutex);
        post([secondary, placement]() mutable {
          placement->set_value(secondary->Position());
        });
      }
      REQUIRE(placement_result.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
      origin = placement_result.get();
      if (!origin.has_value()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
    if (origin.has_value()) {
      REQUIRE(std::abs(origin->x - preferred_origin.x) <= 2.0F);
      REQUIRE(std::abs(origin->y - preferred_origin.y) <= 2.0F);
    }

    post([secondary] { secondary->Close(); });
    PumpUntil([&] { return live_secondary_windows.load(std::memory_order_acquire) == 0; },
              "secondary window retirement");
  }

  {
    std::function<void(std::function<void()>)> finish_post;
    std::optional<WindowHandle> primary;
    {
      std::lock_guard lock(test_state_mutex);
      REQUIRE(primary_window.has_value());
      REQUIRE(app_thread_post != nullptr);
      finish_post = app_thread_post;
      primary = primary_window;
    }
    finish_post([window = *primary] { window.Close(); });
  }
  runner.join();

  REQUIRE(runner_finished.load());
  if (runner_failure != nullptr) {
    std::rethrow_exception(runner_failure);
  }
}

} // namespace huxerui::test
