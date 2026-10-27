#include <huxerui/huxerui.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

using namespace huxerui;

struct SharedCounter {
  State<int> value{0};
};

[[huxerui::composable]]
View WindowTransfer(std::string label) {
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
View CounterPanel(std::shared_ptr<SharedCounter> model, std::string label) {
  auto local = UseState(0);
  auto window = UseWindow();
  return Column {
    WindowTransfer(std::move(label)),
    Text("Shared: " + std::to_string(model->value.Get())),
    Button("Increment shared").OnClick([model] { model->value += 1; }),
    Text("This window: " + std::to_string(local.Get())),
    Button("Increment local").OnClick([local] { local += 1; }),
    Button("Close this window").OnClick([window] { window.Close(); }),
  }.With(Padding(24.0F), Spacing(12.0F));
}

View App() {
  auto application = UseApplication();
  auto model = UseService<SharedCounter>();
  auto secondary = UseState(std::optional<WindowHandle>{});
  const bool open = secondary.Get().has_value() && secondary.Get()->IsOpen();
  return MaterialTheme {
    Column {
      Text("Multiple windows"),
      CounterPanel(model, "primary"),
      Text(open ? "Secondary window is open" : "Secondary window is closed"),
      Button(open ? "Activate secondary" : "Open secondary").OnClick([application, model, secondary] {
        if (secondary.Get().has_value() && secondary.Get()->IsOpen()) {
          secondary.Get()->Activate();
        } else {
          secondary = application.OpenWindow([model] { return MaterialTheme {CounterPanel(model, "secondary")}; }, {
              .title = "Independent counter",
              .initial_size = {380.0F, 320.0F},
          });
        }
      }).With(Enabled(application.SupportsMultipleWindows())),
      Button("Quit application").OnClick([application] { application.Quit(); }),
    }.With(Padding(24.0F), Spacing(12.0F)),
  };
}

const Application application{
    App,
    {
        .window = {.title = "HuxerUI Multiple Windows", .initial_size = {480.0F, 520.0F}},
        .application_hooks = {[](ApplicationContext& context) {
          context.Provide(std::make_shared<SharedCounter>());
        }},
    },
};
