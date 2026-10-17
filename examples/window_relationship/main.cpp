#include <huxerui/huxerui.h>

#include <memory>
#include <string>

using namespace huxerui;

namespace {

[[huxerui::composable]]
View SettingsContent() {
  const auto window = UseWindow();
  return Column {
    Text("Settings").With(FontSize(24.0F)),
    Text("This window is owned by the primary window and blocks its input while open."),
    Text("Close this window to return focus to the primary window."),
    Button("Close").OnClick([window] {
      window.Close();
    }),
  }.With(
      Padding(24.0F),
      Spacing(12.0F)
  );
}

[[huxerui::composable]]
View PrimaryContent() {
  const auto application = UseApplication();
  const auto owner = UseWindow();

  return Column {
    Text("Window Relationship").With(FontSize(24.0F)),
    Text("Open the owned modal settings window, or an independent inspector that stays above but never blocks."),
    Button("Open settings (owned modal)").OnClick([application, owner] {
      static_cast<void>(application.OpenWindow(
          {.title = "HuxerUI Settings", .key = "settings", .initial_size = {440.0F, 320.0F}},
          {.owner = owner, .modal = true}
      ));
    }),
    Button("Open inspector (owned)").OnClick([application, owner] {
      static_cast<void>(application.OpenWindow(
          {.title = "HuxerUI Inspector", .key = "inspector", .initial_size = {360.0F, 240.0F}},
          {.owner = owner, .modal = false}
      ));
    }),
  }.With(
      Padding(24.0F),
      Spacing(12.0F)
  );
}

} // namespace

View App() {
  const WindowIdentity identity = UseEnvironment<WindowIdentity>();
  return MaterialTheme {
    identity.key.empty() ? PrimaryContent() : SettingsContent(),
  };
}

const Application application{
    App,
    {
        .window = {
            .title = "HuxerUI Window Relationship",
            .initial_size = {560.0F, 360.0F},
        },
    }
};
