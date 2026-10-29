#include <catch2/catch_amalgamated.hpp>

#include "async_test_support.h"

TEST_CASE("WebTestWaitYieldsToCallbacksAndKeepsItsTimeout") {
  struct Pending {
    std::mutex mutex;
    std::condition_variable condition;
    bool complete = false;
  } pending;
  emscripten_async_call([](void* context) {
    auto& pending = *static_cast<Pending*>(context);
    std::scoped_lock lock(pending.mutex);
    pending.complete = true;
    pending.condition.notify_one();
  }, &pending, 0);
  std::unique_lock lock(pending.mutex);
  const bool completed = huxerui::test::WaitForTestCondition(
      pending.condition, lock, std::chrono::steady_clock::now() + std::chrono::seconds(5),
      [&] { return pending.complete; });
  REQUIRE(completed);
  REQUIRE(lock.owns_lock());
  const bool ready = huxerui::test::WaitForTestCondition(
      pending.condition, lock, std::chrono::steady_clock::now() + std::chrono::milliseconds(5),
      [] { return false; });
  REQUIRE_FALSE(ready);
  REQUIRE(lock.owns_lock());
}

// CTest invokes this hidden case separately and requires a failing process exit status.
TEST_CASE("WebTestRunnerPropagatesFailure", "[.test-entry]") {
  emscripten_sleep(0);
  FAIL("HuxerUI Web test runner must propagate assertion failures");
}
