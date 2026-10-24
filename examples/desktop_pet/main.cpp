#include <huxerui/huxerui.h>

#include <desktop_pet_resources.h>

#include <memory>

#include "pet.h"
#include "pet_model.h"

using namespace huxerui;
using desktop_pet::PetModel;
using desktop_pet::StartupPetOptions;

namespace {

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
          MenuItem("Hide pet", [model, tray] { model->Hide(tray.IsAvailable()); }),
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
  desktop_pet::UsePetAttachment(model);
  window.OnCloseRequest([model, tray_available] {
    if (!tray_available) return false;
    model->Hide(tray_available);
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
      .On<ViewEvents::ContextMenuRequested>([model, application, menu, tray_available](Point position) {
        menu.ShowAt(position, {
            MenuItem("Settings", [model, application] { model->OpenSettings(application); }),
            MenuItem("Hide pet", [model, tray_available] { model->Hide(tray_available); }).Enabled(tray_available),
            MenuSection{},
            MenuItem("Quit", [application] { application.Quit(); }),
        });
      });
}

[[huxerui::composable]]
View SettingsContent() {
  const auto model = UseService<PetModel>();
  const auto application = UseApplication();
  const bool tray_available = application.SystemTray().IsAvailable();
  desktop_pet::UseSettingsAttachment(model);
  return ScrollView {
    Column {
      Text("Assistant orb", TextRole::Title),
      Text("Click the orb to brighten its waves. Drag to move it."),
      Checkbox("Animate waves", model->animate.Get())
          .OnChanged([model](bool value) { model->animate = value; }),
      Checkbox("Stay on top", model->topmost.Get())
          .OnChanged([model](bool value) { model->SetTopmost(value); }),
      Checkbox("Lightweight mode", model->lightweight.Get())
          .OnChanged([model, tray_available](bool value) { model->SetLightweight(value, tray_available); })
          .With(Enabled(tray_available)),
      Text("Release pet and settings windows when hidden. Click the tray to recreate the orb."),
      Button("Recover pet position").OnClick([model] { model->RecoverPosition(); }),
      Button("Hide pet").OnClick([model, tray_available] { model->Hide(tray_available); })
          .With(Enabled(tray_available)),
      Button("Retire pet").OnClick([model] { model->Retire(); })
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
