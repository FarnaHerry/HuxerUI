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
View WindowTransfer() {
  const auto identity = UseEnvironment<WindowIdentity>();
  const std::string label = identity.key.empty() ? "primary" : identity.key;
  auto received = UseState(std::string{"Drop a window label here"});
  auto hovered = UseState(false);
  const auto& theme = UseTheme();

  return Column {
    Text("Drag label: " + label).With(
        Padding(12.0F),
        Background(theme.colors.surface),
        DragSource(label, [label] { return Text(label).With(Padding(8.0F)); })
            .Scope(DragDropScope::Application)
    ),
    Text(received.Get())
        .With(
            Padding(12.0F),
            Background(hovered.Get() ? theme.colors.primary : theme.colors.surface),
            DropTarget::Accepts<std::string>().Scope(DragDropScope::Application)
        )
        .On<DropEvents<std::string>::Entered>([hovered](const std::string&, const DropEvent&) { hovered = true; })
        .On<DropEvents<std::string>::Exited>([hovered](const std::string&, const DropEvent&) { hovered = false; })
        .On<DropEvents<std::string>::Dropped>([received, hovered](const std::string& value, const DropEvent&) {
          received = "Received from: " + value;
          hovered = false;
        }),
  }.With(Spacing(8.0F));
}

[[huxerui::composable]]
View SecondaryContent() {
  auto local = UseState(0);
  const auto shared = UseService<SharedCounter>();

  return Column {
    Text("Secondary window").With(FontSize(24.0F)),
    WindowTransfer(),
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
    WindowTransfer(),
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
