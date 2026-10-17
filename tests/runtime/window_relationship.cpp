#include "runtime_test_support.h"

#include <algorithm>
#include <utility>

namespace huxerui::test {

namespace {

View RelationshipApp() {
  return Spacer();
}

using ModalState = std::pair<bool, bool>;

[[nodiscard]] bool ReceivedCommand(const TestPlatform& platform, WindowCommand command) {
  return std::find(platform.window_commands.begin(), platform.window_commands.end(), command) !=
         platform.window_commands.end();
}

} // namespace

TEST_CASE("OwnedWindowAppliesOwnershipAndRetiresWithOwner") {
  TestPlatform owner;
  Application application{RelationshipApp, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  const WindowHandle primary = owner.OpenWindow(application.options.window);
  const WindowHandle child = owner.OpenWindow({.title = "child"}, {.owner = primary, .modal = false});
  static_cast<void>(child);
  REQUIRE(owner.created_windows.size() == 2);
  REQUIRE(owner.created_windows[1]->applied_owners.size() == 1);
  REQUIRE(owner.created_windows[1]->applied_owners.back() == owner.created_windows[0]);

  owner.RetireOwned(*owner.created_windows[0]);
  REQUIRE(!owner.created_windows[1]->Initialized());
  REQUIRE(owner.created_windows[1]->host_discarded);
}

TEST_CASE("ModalWindowBlocksItsOwner") {
  TestPlatform owner;
  Application application{RelationshipApp, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  const WindowHandle primary = owner.OpenWindow(application.options.window);
  const WindowHandle settings =
      owner.OpenWindow({.title = "settings"}, {.owner = primary, .modal = true});
  static_cast<void>(settings);
  REQUIRE(owner.created_windows.size() == 2);
  REQUIRE(owner.created_windows[0]->applied_modal_states.back() == ModalState{false, true});
  REQUIRE(owner.created_windows[1]->applied_modal_states.back() == ModalState{true, false});

  owner.RetireOwned(*owner.created_windows[1]);
  REQUIRE(owner.created_windows[0]->applied_modal_states.back() == ModalState{false, false});
  REQUIRE(ReceivedCommand(*owner.created_windows[0], WindowCommand::Activate));
}

TEST_CASE("ApplicationModalBlocksEveryWindow") {
  TestPlatform owner;
  Application application{RelationshipApp, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  const WindowHandle primary = owner.OpenWindow(application.options.window);
  const WindowHandle dialog = owner.OpenWindow({.title = "dialog"}, {.owner = std::nullopt, .modal = true});
  static_cast<void>(dialog);
  REQUIRE(owner.created_windows.size() == 2);
  REQUIRE(owner.created_windows[0]->applied_modal_states.back() == ModalState{false, true});
  REQUIRE(owner.created_windows[1]->applied_modal_states.back() == ModalState{true, false});
}

TEST_CASE("RelationshipRejectsSelfOwnershipAndCycles") {
  TestPlatform owner;
  Application application{RelationshipApp, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  const WindowHandle primary = owner.OpenWindow(application.options.window);
  const WindowHandle first = owner.OpenWindow({.title = "first"}, {.owner = primary, .modal = false});
  const WindowHandle second = owner.OpenWindow({.title = "second"}, {.owner = first, .modal = false});

  REQUIRE_THROWS_AS(SetWindowRelationship(first, {.owner = first, .modal = false}), std::invalid_argument);
  REQUIRE_THROWS_AS(SetWindowRelationship(primary, {.owner = second, .modal = false}), std::invalid_argument);
}

TEST_CASE("RelationshipRejectsRetiredWindows") {
  TestPlatform owner;
  Application application{RelationshipApp, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  const WindowHandle primary = owner.OpenWindow(application.options.window);
  const WindowHandle retired = owner.OpenWindow({.title = "retired"});
  detail::InternalAccess::RetireWindow(*owner.created_windows[1]);
  const WindowHandle live = owner.OpenWindow({.title = "live"});
  static_cast<void>(primary);

  REQUIRE_THROWS_AS(SetWindowRelationship(live, {.owner = retired, .modal = false}), std::logic_error);
  REQUIRE_THROWS_AS(SetWindowRelationship(retired, {.owner = live, .modal = false}), std::logic_error);
}

TEST_CASE("OwnedModalExclusionsKeepOwnerInteractiveForOwnChain") {
  TestPlatform owner;
  Application application{RelationshipApp, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  const WindowHandle primary = owner.OpenWindow(application.options.window);
  const WindowHandle settings =
      owner.OpenWindow({.title = "settings"}, {.owner = primary, .modal = true});
  const WindowHandle helper =
      owner.OpenWindow({.title = "helper"}, {.owner = settings, .modal = false});
  static_cast<void>(helper);
  REQUIRE(owner.created_windows.size() == 3);
  // The modal's own owned chain stays interactive; the owner and unrelated windows stay blocked.
  REQUIRE(owner.created_windows[0]->applied_modal_states.back() == ModalState{false, true});
  REQUIRE(owner.created_windows[1]->applied_modal_states.back() == ModalState{true, false});
  REQUIRE(owner.created_windows[2]->applied_modal_states.back() == ModalState{false, false});
}

TEST_CASE("WindowHandleRetireRetiresOwnedChainAndKeepsApplicationRunning") {
  TestPlatform owner;
  Application application{RelationshipApp, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  const WindowHandle primary = owner.OpenWindow(application.options.window);
  const WindowHandle settings = owner.OpenWindow({.title = "settings"}, {.owner = primary, .modal = true});
  const WindowHandle helper = owner.OpenWindow({.title = "helper"}, {.owner = settings, .modal = false});
  static_cast<void>(helper);
  REQUIRE(owner.created_windows.size() == 3);

  primary.Retire();
  owner.RunPlatformModuleTasks();

  for (const TestPlatform* window : owner.created_windows) {
    REQUIRE(!window->Initialized());
    REQUIRE(window->host_discarded);
  }
  // Explicit retirement never requests application shutdown; the Runtime keeps running and can reopen windows.
  REQUIRE(owner.application_quit_requests == 0);
  const WindowHandle again = owner.OpenWindow({.title = "again"});
  static_cast<void>(again);
  REQUIRE(owner.created_windows.size() == 4);
  REQUIRE(owner.created_windows[3]->Initialized());
}

TEST_CASE("WindowHandleRetireUnblocksOwnerAndReactivatesIt") {
  TestPlatform owner;
  Application application{RelationshipApp, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  const WindowHandle primary = owner.OpenWindow(application.options.window);
  const WindowHandle settings = owner.OpenWindow({.title = "settings"}, {.owner = primary, .modal = true});
  REQUIRE(owner.created_windows[0]->applied_modal_states.back() == ModalState{false, true});

  settings.Retire();
  owner.RunPlatformModuleTasks();

  REQUIRE(owner.created_windows[0]->applied_modal_states.back() == ModalState{false, false});
  REQUIRE(ReceivedCommand(*owner.created_windows[0], WindowCommand::Activate));
  REQUIRE(owner.created_windows[1]->host_discarded);
}

TEST_CASE("WindowHandleRetireIsHarmlessAfterRetirement") {
  TestPlatform owner;
  Application application{RelationshipApp, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  const WindowHandle primary = owner.OpenWindow(application.options.window);
  static_cast<void>(primary);
  const WindowHandle child = owner.OpenWindow({.title = "child"});

  child.Retire();
  owner.RunPlatformModuleTasks();
  REQUIRE(!owner.created_windows[1]->Initialized());
  REQUIRE_NOTHROW(child.Retire());
  owner.RunPlatformModuleTasks();

  const WindowHandle another = owner.OpenWindow({.title = "another"});
  static_cast<void>(another);
  REQUIRE(owner.created_windows[2]->Initialized());
}

} // namespace huxerui::test
