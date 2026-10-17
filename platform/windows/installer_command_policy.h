#pragma once

#include <windows.h>
#include <msiquery.h>

#include <optional>

#include <BootstrapperApplicationTypes.h>
#include <BootstrapperEngineTypes.h>

#include <huxerui/windows/installer.h>

namespace huxerui::detail {

// Returns true when the engine command leaves no user available to answer interactive pages:
// related-bundle runs embedded by a parent bundle (a previous version asked to uninstall itself
// during an upgrade), quiet or passive displays, or an explicit action switch on the command
// line. Commanded runs must execute the engine's action on their own and never park on a page.
[[nodiscard]] inline bool InstallerCommandPreemptsInteraction(BOOTSTRAPPER_ACTION action, BOOTSTRAPPER_DISPLAY display,
                                                              BOOTSTRAPPER_RELATION_TYPE relation) {
  if (display == BOOTSTRAPPER_DISPLAY_EMBEDDED || display == BOOTSTRAPPER_DISPLAY_NONE ||
      display == BOOTSTRAPPER_DISPLAY_PASSIVE || relation != BOOTSTRAPPER_RELATION_NONE) {
    return true;
  }
  return action == BOOTSTRAPPER_ACTION_UNINSTALL || action == BOOTSTRAPPER_ACTION_UNSAFE_UNINSTALL ||
         action == BOOTSTRAPPER_ACTION_REPAIR;
}

// Maps a preempting engine command to the session action executed without user input. Commands
// that carry no matching session action (for example LAYOUT) produce no automatic action.
[[nodiscard]] inline std::optional<windows::InstallerAction>
InstallerCommandedAction(BOOTSTRAPPER_ACTION action, BOOTSTRAPPER_DISPLAY display,
                         BOOTSTRAPPER_RELATION_TYPE relation) {
  if (!InstallerCommandPreemptsInteraction(action, display, relation)) {
    return std::nullopt;
  }
  switch (action) {
  case BOOTSTRAPPER_ACTION_INSTALL:
    return windows::InstallerAction::Install;
  case BOOTSTRAPPER_ACTION_UNINSTALL:
  case BOOTSTRAPPER_ACTION_UNSAFE_UNINSTALL:
    return windows::InstallerAction::Uninstall;
  case BOOTSTRAPPER_ACTION_REPAIR:
    return windows::InstallerAction::Repair;
  default:
    return std::nullopt;
  }
}

// Maps a detected package state to the observable product state. A superseded package during a
// commanded run still owns a live registration that must be removed — the related bundle asked to
// uninstall itself detects its own MSI as superseded while the newer MSI is being installed — so
// only interactive runs are gated behind NewerVersion.
[[nodiscard]] inline windows::InstallerProductState
InstallerProductStateForPackage(BOOTSTRAPPER_PACKAGE_STATE package_state, bool preempts) {
  switch (package_state) {
  case BOOTSTRAPPER_PACKAGE_STATE_PRESENT:
    return windows::InstallerProductState::Present;
  case BOOTSTRAPPER_PACKAGE_STATE_ABSENT:
    return windows::InstallerProductState::Absent;
  case BOOTSTRAPPER_PACKAGE_STATE_OBSOLETE:
  case BOOTSTRAPPER_PACKAGE_STATE_SUPERSEDED:
    return preempts ? windows::InstallerProductState::Present : windows::InstallerProductState::NewerVersion;
  default:
    return windows::InstallerProductState::Unknown;
  }
}

// Engine questions (errors, files-in-use) are answered by the local interface only when a user is
// available to answer them; commanded runs return the engine or parent resolution instead.
[[nodiscard]] inline bool InstallerPromptAnswersLocally(bool preempts) {
  return !preempts;
}

} // namespace huxerui::detail
