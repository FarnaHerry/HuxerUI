#include <huxerui/huxerui.h>

#include <memory>
#include <string>

using namespace huxerui;

namespace {

struct SharedCounter {
  State<int> value{0};
};

void InstallSharedCounter(ApplicationContext& context) {
  context.Provide(std::make_shared<SharedCounter>());
}

[[huxerui::composable]]
View SecondaryContent() {
  auto local = UseState(0);
  const auto shared = UseService<SharedCounter>();

  return Column {
    Text("Secondary window").With(FontSize(24.0F)),
    Text::Format("Local count: {}", local),
    Text::Format("Shared count: {}", shared->value.Get()),
    Button("+1 local").OnClick([local] { local += 1; }),
    Button("+1 shared").OnClick([shared] { shared->value += 1; }),
  }.With(
      Padding(24.0F),
      Spacing(12.0F)
  );
}

[[huxerui::composable]]
View PrimaryContent() {
  auto local = UseState(0);
  const auto shared = UseService<SharedCounter>();
  const auto application = UseApplication();

  return Column {
    Text("Primary window").With(FontSize(24.0F)),
    Text::Format("Local count: {}", local),
    Text::Format("Shared count: {}", shared->value.Get()),
    Button("+1 local").OnClick([local] { local += 1; }),
    Button("+1 shared").OnClick([shared] { shared->value += 1; }),
    Button("Open secondary window").OnClick([application] {
      static_cast<void>(application.OpenWindow({.title = "HuxerUI Secondary", .key = "secondary"}));
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
    identity.key == "secondary" ? SecondaryContent() : PrimaryContent(),
  };
}

const Application application{
    App,
    {
        .window = {
            .title = "HuxerUI Multi-Window",
            .initial_size = {520.0F, 420.0F},
        },
        .application_hooks = {InstallSharedCounter},
    }
};
