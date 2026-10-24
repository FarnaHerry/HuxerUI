#include <huxerui/huxerui.h>

#include <desktop_pet_resources.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <stdexcept>

#include "pet.h"

using namespace huxerui;

namespace {

WindowOptions PetOptions() {
  return {
      .title = "HuxerUI Desktop Pet",
      .key = "pet",
      .initial_size = {200.0F, 200.0F},
      .presentation = WindowPresentation::Utility,
      .transparent_surface = true,
      .show_without_activation = true,
      .topmost = true,
  };
}

// The startup pet requests only the widely supported floating core. Passive display and keep-above
// are creation policies that Wayland compositors own by protocol, so requesting them there throws
// std::logic_error; the mounted pet applies keep-above afterwards where the attachment accepts it.
WindowOptions StartupPetOptions() {
  WindowOptions options = PetOptions();
  options.show_without_activation = false;
  options.topmost = false;
  return options;
}

struct PetModel {
  std::optional<WindowHandle> pet;
  std::optional<WindowHandle> settings;
  State<bool> energized{false};
  State<bool> animate{true};
  State<bool> topmost{true};

  void Show(const ApplicationHandle& application) {
    if (!pet) {
      auto options = PetOptions();
      options.topmost = topmost.Get();
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

  void RecoverPosition() const {
    if (!pet) return;
    if (const auto work = pet->WorkArea()) {
      static_cast<void>(pet->SetPosition({work->x + std::max(0.0F, work->width - 220.0F),
                                        work->y + std::max(0.0F, work->height - 220.0F)}));
    }
  }

  void OpenSettings(const ApplicationHandle& application) {
    if (!settings) {
      settings = application.OpenWindow(
          {.title = "Desktop Pet Settings", .key = "settings", .initial_size = {380.0F, 340.0F}},
          {.owner = pet});
    }
    settings->Activate();
  }
};

void InstallPet(ApplicationContext& context) {
  const auto model = std::make_shared<PetModel>();
  context.Provide(model);
  const auto application = UseApplication();
  const auto tray = application.SystemTray();
  tray.OnActivate([model, application] { model->Show(application); });
  tray.Show(desktop_pet::images::tray, {
      .tooltip = "HuxerUI Desktop Pet",
      .menu = {
          MenuItem("Show pet", [model, application] { model->Show(application); }),
          MenuItem("Hide pet", [model] { if (model->pet) model->pet->Hide(); }),
          MenuItem("Recover position", [model] { model->RecoverPosition(); }),
          MenuItem("Settings", [model, application] { model->OpenSettings(application); }),
          MenuSection{},
          MenuItem("Quit", [application] { application.Quit(); }),
      },
  });
}

[[huxerui::composable]]
View PetContent() {
  const auto window = UseWindow();
  const auto application = UseApplication();
  const auto model = UseService<PetModel>();
  const auto menu = UseMenu();
  const bool visible = window.LifecycleState() != WindowLifecycleState::Background;
  const bool tray_available = application.SystemTray().IsAvailable();
  Lifecycle([window, tray_available] {
    if (!tray_available) window.Show();
  }, tray_available);
  Lifecycle([model, window] {
    model->pet = window;
    // Keep-above is a post-creation request; attachments without the operation keep their stacking.
    if (window.SetTopmost(true) != WindowRequestResult::Submitted) model->topmost = false;
    model->RecoverPosition();
    return [model] { model->pet.reset(); };
  });
  window.OnCloseRequest([model, window, tray_available] {
    if (!tray_available) return false;
    window.Hide();
    return true;
  }, tray_available);
  return Stack {}.With(
      Indication{},
      desktop_pet::Orb{.energized = model->energized.Get(), .animate = visible && model->animate.Get()},
      DragGesture{},
      Semantics{.role = SemanticRole::Button,
                .label = "Assistant orb. Click to energize; drag to move; right-click for menu."}
  ).OnClick([model] { model->energized = !model->energized.Get(); })
      .On<DragEvents::Started>([window](const DragEvent&) { static_cast<void>(window.BeginMove()); })
      .On<ViewEvents::ContextMenuRequested>([model, application, menu, window, tray_available](Point position) {
        menu.ShowAt(position, {
            MenuItem("Settings", [model, application] { model->OpenSettings(application); }),
            MenuItem("Hide pet", [window] { window.Hide(); }).Enabled(tray_available),
            MenuSection{},
            MenuItem("Quit", [application] { application.Quit(); }),
        });
      });
}

[[huxerui::composable]]
View SettingsContent() {
  const auto window = UseWindow();
  const auto model = UseService<PetModel>();
  const auto application = UseApplication();
  const bool tray_available = application.SystemTray().IsAvailable();
  Lifecycle([model, window] {
    model->settings = window;
    return [model] { model->settings.reset(); };
  });
  return ScrollView {
    Column {
      Text("Assistant orb", TextRole::Title),
      Text("Click the orb to brighten its waves. Drag to move it."),
      Checkbox("Animate waves", model->animate.Get())
          .OnChanged([model](bool value) { model->animate = value; }),
      Checkbox("Stay on top", model->topmost.Get()).OnChanged([model](bool value) {
        if (model->pet && model->pet->SetTopmost(value) == WindowRequestResult::Submitted) model->topmost = value;
      }),
      Button("Recover pet position").OnClick([model] { model->RecoverPosition(); }),
      Button("Retire pet").OnClick([model] { if (model->pet) model->pet->Retire(); })
          .With(Enabled(tray_available)),
      Button("Show or recreate pet").OnClick([model, application] { model->Show(application); }),
      Button("Quit").OnClick([application] { application.Quit(); }),
    }.With(Padding(20.0F), Spacing(10.0F), CrossAlign(CrossAxisAlignment::Stretch)),
  };
}

} // namespace

View App() {
  return MaterialTheme {
    UseEnvironment<WindowIdentity>().key == "settings" ? SettingsContent() : PetContent(),
  };
}

const Application application{App, {.window = StartupPetOptions(), .application_hooks = {InstallPet}}};
