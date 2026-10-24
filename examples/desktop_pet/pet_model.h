#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>

#include <huxerui/huxerui.h>

namespace desktop_pet {

inline huxerui::WindowOptions PetOptions() {
  return {
      .title = "HuxerUI Desktop Pet",
      .key = "pet",
      .initial_size = {200.0F, 200.0F},
      .presentation = huxerui::WindowPresentation::Utility,
      .transparent_surface = true,
      .show_without_activation = true,
      .topmost = true,
  };
}

struct PetModel {
  std::optional<huxerui::WindowHandle> pet;
  std::optional<huxerui::WindowHandle> settings;
  huxerui::State<bool> energized{false};
  huxerui::State<bool> animate{true};
  huxerui::State<bool> topmost{true};
  huxerui::State<bool> lightweight{false};
  std::optional<huxerui::Point> saved_position;
  std::uint64_t pet_generation = 0;
  std::uint64_t settings_generation = 0;

  void Show(const huxerui::ApplicationHandle& application) {
    if (!pet) {
      auto options = PetOptions();
      options.topmost = topmost.Get();
      options.initial_position = saved_position;
      try {
        pet = application.OpenWindow(options);
      } catch (const std::logic_error&) {
        // Wayland rejects passive display and keep-above at creation; fall back to the base policy.
        options.show_without_activation = false;
        options.topmost = false;
        pet = application.OpenWindow(options);
        topmost = false;
      }
    }
    pet->Show();
  }

  void Retire() {
    if (pet) saved_position = pet->Position();
    // Retirement is queued; an old attachment's cleanup must not clear a newly opened replacement.
    ++pet_generation;
    ++settings_generation;
    if (settings) settings->Retire();
    if (pet) pet->Retire();
    settings.reset();
    pet.reset();
  }

  void Hide(bool tray_available) {
    if (!tray_available) return;
    if (lightweight.Get()) Retire();
    else if (pet) pet->Hide();
  }

  void SetLightweight(bool value, bool tray_available) {
    if (!tray_available) return;
    lightweight = value;
    if (value && pet && pet->LifecycleState() == huxerui::WindowLifecycleState::Background) Hide(true);
  }

  void SetTopmost(bool value) {
    if (!pet || pet->SetTopmost(value) == huxerui::WindowRequestResult::Submitted) topmost = value;
  }

  void RecoverPosition() {
    if (!pet) {
      saved_position.reset();
      return;
    }
    if (const auto work = pet->WorkArea()) {
      const huxerui::Point position{work->x + std::max(0.0F, work->width - 220.0F),
                                   work->y + std::max(0.0F, work->height - 220.0F)};
      if (pet->SetPosition(position) == huxerui::WindowRequestResult::Submitted) saved_position = position;
    }
  }

  void OpenSettings(const huxerui::ApplicationHandle& application) {
    if (!settings) {
      settings = application.OpenWindow(
          {.title = "Desktop Pet Settings", .key = "settings", .initial_size = {380.0F, 440.0F}},
          {.owner = pet});
    }
    settings->Activate();
  }
};

inline void UsePetAttachment(const std::shared_ptr<PetModel>& model) {
  const auto window = huxerui::UseWindow();
  huxerui::Lifecycle([model, window] {
    const auto generation = ++model->pet_generation;
    model->pet = window;
    // An attachment without keep-above support uses its compositor's stacking policy.
    if (model->topmost.Get() && window.SetTopmost(true) != huxerui::WindowRequestResult::Submitted) {
      model->topmost = false;
    }
    if (!model->saved_position) model->RecoverPosition();
    return [model, generation] {
      if (model->pet_generation == generation) model->pet.reset();
    };
  });
}

inline void UseSettingsAttachment(const std::shared_ptr<PetModel>& model) {
  const auto window = huxerui::UseWindow();
  huxerui::Lifecycle([model, window] {
    const auto generation = ++model->settings_generation;
    model->settings = window;
    return [model, generation] {
      if (model->settings_generation == generation) model->settings.reset();
    };
  });
}

} // namespace desktop_pet
