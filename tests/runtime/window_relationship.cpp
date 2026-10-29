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
  const WindowHandle primary = UseApplication().OpenWindow([] { return Spacer(); }, application.options.window);
  const WindowHandle child = UseApplication().OpenWindow(
      [] { return Spacer(); },
      {.title = "child"},
      {.owner = primary, .modal = false}
  );
  static_cast<void>(child);
  REQUIRE(owner.created_windows.size() == 2);
  REQUIRE(owner.created_windows[1]->applied_owners.size() == 1);
  REQUIRE(owner.created_windows[1]->applied_owners.back() == owner.created_windows[0]);

  primary.Close();
  owner.RunPlatformModuleTasks();
  REQUIRE_FALSE(primary.IsOpen());
  REQUIRE_FALSE(child.IsOpen());
  REQUIRE(owner.destroyed_windows == 2);
  REQUIRE(owner.discarded_windows == 2);
}

TEST_CASE("ModalWindowBlocksItsOwner") {
  TestPlatform owner;
  Application application{RelationshipApp, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  const WindowHandle primary = UseApplication().OpenWindow([] { return Spacer(); }, application.options.window);
  const WindowHandle settings =
      UseApplication().OpenWindow([] { return Spacer(); }, {.title = "settings"}, {.owner = primary, .modal = true});
  static_cast<void>(settings);
  REQUIRE(owner.created_windows.size() == 2);
  REQUIRE(owner.created_windows[0]->applied_modal_states.back() == ModalState{false, true});
  REQUIRE(owner.created_windows[1]->applied_modal_states.back() == ModalState{true, false});

  settings.Close();
  owner.RunPlatformModuleTasks();
  REQUIRE(owner.created_windows[0]->applied_modal_states.back() == ModalState{false, false});
  REQUIRE(ReceivedCommand(*owner.created_windows[0], WindowCommand::Activate));
}

TEST_CASE("ApplicationModalBlocksEveryWindow") {
  TestPlatform owner;
  Application application{RelationshipApp, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  const WindowHandle primary = UseApplication().OpenWindow([] { return Spacer(); }, application.options.window);
  const WindowHandle dialog = UseApplication().OpenWindow(
      [] { return Spacer(); },
      {.title = "dialog"},
      {.owner = std::nullopt, .modal = true}
  );
  static_cast<void>(dialog);
  REQUIRE(owner.created_windows.size() == 2);
  REQUIRE(owner.created_windows[0]->applied_modal_states.back() == ModalState{false, true});
  REQUIRE(owner.created_windows[1]->applied_modal_states.back() == ModalState{true, false});
}

TEST_CASE("RelationshipRejectsSelfOwnershipAndCycles") {
  TestPlatform owner;
  Application application{RelationshipApp, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  const WindowHandle primary = UseApplication().OpenWindow([] { return Spacer(); }, application.options.window);
  const WindowHandle first = UseApplication().OpenWindow(
      [] { return Spacer(); },
      {.title = "first"},
      {.owner = primary, .modal = false}
  );
  const WindowHandle second = UseApplication().OpenWindow(
      [] { return Spacer(); },
      {.title = "second"},
      {.owner = first, .modal = false}
  );

  REQUIRE_THROWS_AS(SetWindowRelationship(first, {.owner = first, .modal = false}), std::invalid_argument);
  REQUIRE_THROWS_AS(SetWindowRelationship(primary, {.owner = second, .modal = false}), std::invalid_argument);
}

TEST_CASE("RelationshipRejectsRetiredWindows") {
  TestPlatform owner;
  Application application{RelationshipApp, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  const WindowHandle primary = UseApplication().OpenWindow([] { return Spacer(); }, application.options.window);
  const WindowHandle retired = UseApplication().OpenWindow([] { return Spacer(); }, {.title = "retired"});
  detail::InternalAccess::RetireWindow(*owner.created_windows[1]);
  const WindowHandle live = UseApplication().OpenWindow([] { return Spacer(); }, {.title = "live"});
  static_cast<void>(primary);

  REQUIRE_THROWS_AS(SetWindowRelationship(live, {.owner = retired, .modal = false}), std::logic_error);
  REQUIRE_THROWS_AS(SetWindowRelationship(retired, {.owner = live, .modal = false}), std::logic_error);
}

TEST_CASE("OwnedModalExclusionsKeepOwnerInteractiveForOwnChain") {
  TestPlatform owner;
  Application application{RelationshipApp, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  const WindowHandle primary = UseApplication().OpenWindow([] { return Spacer(); }, application.options.window);
  const WindowHandle settings =
      UseApplication().OpenWindow([] { return Spacer(); }, {.title = "settings"}, {.owner = primary, .modal = true});
  const WindowHandle helper =
      UseApplication().OpenWindow([] { return Spacer(); }, {.title = "helper"}, {.owner = settings, .modal = false});
  static_cast<void>(helper);
  REQUIRE(owner.created_windows.size() == 3);
  // The modal's own owned chain stays interactive; the owner and unrelated windows stay blocked.
  REQUIRE(owner.created_windows[0]->applied_modal_states.back() == ModalState{false, true});
  REQUIRE(owner.created_windows[1]->applied_modal_states.back() == ModalState{true, false});
  REQUIRE(owner.created_windows[2]->applied_modal_states.back() == ModalState{false, false});
}

TEST_CASE("WindowHandleCloseRetiresOwnedChainAndKeepsApplicationRunningWhenConfigured") {
  TestPlatform owner;
  Application application{RelationshipApp, {.show_debug_overlay = false, .quit_on_last_window_closed = false}};
  RuntimeLifetime lifetime(application, owner);
  const WindowHandle primary = UseApplication().OpenWindow([] { return Spacer(); }, application.options.window);
  const WindowHandle settings = UseApplication().OpenWindow(
      [] { return Spacer(); },
      {.title = "settings"},
      {.owner = primary, .modal = true}
  );
  const WindowHandle helper = UseApplication().OpenWindow(
      [] { return Spacer(); },
      {.title = "helper"},
      {.owner = settings, .modal = false}
  );
  static_cast<void>(helper);
  REQUIRE(owner.created_windows.size() == 3);

  primary.Close();
  owner.RunPlatformModuleTasks();

  REQUIRE_FALSE(primary.IsOpen());
  REQUIRE_FALSE(settings.IsOpen());
  REQUIRE_FALSE(helper.IsOpen());
  REQUIRE(owner.destroyed_windows == 3);
  REQUIRE(owner.discarded_windows == 3);
  // The configured close policy keeps the Runtime running so it can reopen windows.
  REQUIRE(owner.application_quit_requests == 0);
  const WindowHandle again = UseApplication().OpenWindow([] { return Spacer(); }, {.title = "again"});
  static_cast<void>(again);
  REQUIRE(owner.created_windows.size() == 4);
  REQUIRE(owner.created_windows[3]->Initialized());
}

TEST_CASE("WindowHandleCloseUnblocksOwnerAndReactivatesIt") {
  TestPlatform owner;
  Application application{RelationshipApp, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  const WindowHandle primary = UseApplication().OpenWindow([] { return Spacer(); }, application.options.window);
  const WindowHandle settings = UseApplication().OpenWindow(
      [] { return Spacer(); },
      {.title = "settings"},
      {.owner = primary, .modal = true}
  );
  REQUIRE(owner.created_windows[0]->applied_modal_states.back() == ModalState{false, true});

  settings.Close();
  owner.RunPlatformModuleTasks();

  REQUIRE(owner.created_windows[0]->applied_modal_states.back() == ModalState{false, false});
  REQUIRE(ReceivedCommand(*owner.created_windows[0], WindowCommand::Activate));
  REQUIRE(owner.discarded_windows == 1);
  REQUIRE(owner.destroyed_windows == 1);
}

TEST_CASE("WindowHandleCloseIsHarmlessAfterRetirement") {
  TestPlatform owner;
  Application application{RelationshipApp, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  const WindowHandle primary = UseApplication().OpenWindow([] { return Spacer(); }, application.options.window);
  static_cast<void>(primary);
  const WindowHandle child = UseApplication().OpenWindow([] { return Spacer(); }, {.title = "child"});

  child.Close();
  owner.RunPlatformModuleTasks();
  REQUIRE_FALSE(child.IsOpen());
  REQUIRE(owner.destroyed_windows == 1);
  REQUIRE_NOTHROW(child.Close());
  owner.RunPlatformModuleTasks();

  const WindowHandle another = UseApplication().OpenWindow([] { return Spacer(); }, {.title = "another"});
  static_cast<void>(another);
  REQUIRE(owner.created_windows[2]->Initialized());
}

TEST_CASE("SiblingModalWindowsStackSoNewestStaysInteractive") {
  TestPlatform owner;
  Application application{RelationshipApp, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  const WindowHandle primary = UseApplication().OpenWindow([] { return Spacer(); }, application.options.window);
  const WindowHandle first = UseApplication().OpenWindow(
      [] { return Spacer(); },
      {.title = "first"},
      {.owner = primary, .modal = true}
  );
  const WindowHandle second = UseApplication().OpenWindow(
      [] { return Spacer(); },
      {.title = "second"},
      {.owner = primary, .modal = true}
  );
  static_cast<void>(first);
  // The owner stays blocked and the older sibling yields to the newer one, which stays interactive.
  REQUIRE(owner.created_windows.size() == 3);
  REQUIRE(owner.created_windows[0]->applied_modal_states.back() == ModalState{false, true});
  REQUIRE(owner.created_windows[1]->applied_modal_states.back() == ModalState{true, true});
  REQUIRE(owner.created_windows[2]->applied_modal_states.back() == ModalState{true, false});

  second.Close();
  owner.RunPlatformModuleTasks();
  REQUIRE(owner.created_windows[0]->applied_modal_states.back() == ModalState{false, true});
  REQUIRE(owner.created_windows[1]->applied_modal_states.back() == ModalState{true, false});
  REQUIRE(ReceivedCommand(*owner.created_windows[0], WindowCommand::Activate));
}

TEST_CASE("ApplicationModalWindowsStackSoNewestStaysInteractive") {
  TestPlatform owner;
  Application application{RelationshipApp, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  const WindowHandle primary = UseApplication().OpenWindow([] { return Spacer(); }, application.options.window);
  const WindowHandle first = UseApplication().OpenWindow(
      [] { return Spacer(); },
      {.title = "first"},
      {.owner = std::nullopt, .modal = true}
  );
  const WindowHandle second = UseApplication().OpenWindow(
      [] { return Spacer(); },
      {.title = "second"},
      {.owner = std::nullopt, .modal = true}
  );
  static_cast<void>(first);
  REQUIRE(owner.created_windows.size() == 3);
  REQUIRE(owner.created_windows[0]->applied_modal_states.back() == ModalState{false, true});
  REQUIRE(owner.created_windows[1]->applied_modal_states.back() == ModalState{true, true});
  REQUIRE(owner.created_windows[2]->applied_modal_states.back() == ModalState{true, false});
}

TEST_CASE("PlainOpenWindowJoinsExistingModalBlocking") {
  TestPlatform owner;
  Application application{RelationshipApp, {.show_debug_overlay = false}};
  RuntimeLifetime lifetime(application, owner);
  const WindowHandle primary = UseApplication().OpenWindow([] { return Spacer(); }, application.options.window);
  const WindowHandle dialog = UseApplication().OpenWindow(
      [] { return Spacer(); },
      {.title = "dialog"},
      {.owner = std::nullopt, .modal = true}
  );
  static_cast<void>(dialog);
  const WindowHandle later = UseApplication().OpenWindow([] { return Spacer(); }, {.title = "later"});
  static_cast<void>(later);
  REQUIRE(owner.created_windows.size() == 3);
  // Windows opened after a modal already exists join the blocking set on the common creation path.
  REQUIRE(!owner.created_windows[2]->applied_modal_states.empty());
  REQUIRE(owner.created_windows[2]->applied_modal_states.back() == ModalState{false, true});
}

} // namespace huxerui::test
