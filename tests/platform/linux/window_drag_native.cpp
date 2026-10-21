#include "linux_internal.h"

#include <gtk/gtk.h>
#include <catch2/catch_amalgamated.hpp>
#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#ifdef None
#undef None
#endif
#endif
#include <dlfcn.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <huxerui/huxerui.h>

namespace huxerui::test {
namespace {

struct SharedCounter {
  State<int> value{0};
};

struct WindowRecord {
  WindowIdentity identity;
  std::optional<WindowHandle> handle;
  std::optional<State<int>> local;
  int compositions = 0;
  int unmounts = 0;
  int shared_value = 0;
};

struct NativeWatch {
  GWeakRef window;
  GWeakRef canvas;

  explicit NativeWatch(GtkWindow* native) {
    g_weak_ref_init(&window, native);
    g_weak_ref_init(&canvas, gtk_window_get_child(native));
  }
  ~NativeWatch() {
    g_weak_ref_clear(&window);
    g_weak_ref_clear(&canvas);
  }
  std::unique_ptr<GtkWindow, decltype(&g_object_unref)> Window() {
    return {GTK_WINDOW(g_weak_ref_get(&window)), g_object_unref};
  }
  bool Released() {
    GObject* native = static_cast<GObject*>(g_weak_ref_get(&window));
    GObject* child = static_cast<GObject*>(g_weak_ref_get(&canvas));
    const bool released = native == nullptr && child == nullptr;
    if (native) g_object_unref(native);
    if (child) g_object_unref(child);
    return released;
  }
  bool SendEscape() {
    auto* widget = GTK_WIDGET(g_weak_ref_get(&canvas));
    if (widget == nullptr) return false;
    GListModel* controllers = gtk_widget_observe_controllers(widget);
    gboolean handled = FALSE;
    for (guint index = 0; index < g_list_model_get_n_items(controllers); ++index) {
      auto* controller = G_OBJECT(g_list_model_get_item(controllers, index));
      const bool key_controller = GTK_IS_EVENT_CONTROLLER_KEY(controller);
      if (key_controller) {
        g_signal_emit_by_name(controller, "key-pressed", GDK_KEY_Escape, 9U, GdkModifierType{}, &handled);
      }
      g_object_unref(controller);
      if (key_controller) break;
    }
    g_object_unref(controllers);
    g_object_unref(widget);
    return handled != FALSE;
  }
};

struct NativeScenario {
  std::map<std::uint32_t, WindowRecord> records;
  std::optional<ApplicationHandle> application;
  std::shared_ptr<SharedCounter> shared;
  std::vector<std::unique_ptr<NativeWatch>> watches;
  std::function<bool()> advance;
  std::function<View(const WindowIdentity&)> content;
  std::exception_ptr failure;
  std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  bool block_extra_close = true;
  int close_requests = 0;
  bool completed = false;
  bool native_primary_close = false;
  bool destroy_primary = false;
  bool key_close_returned = false;

  void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string("HuxerUI ") + message);
  }

  void Watch(const std::string& title) {
    GListModel* windows = gtk_window_get_toplevels();
    for (guint index = 0; index < g_list_model_get_n_items(windows); ++index) {
      auto* window = GTK_WINDOW(g_list_model_get_item(windows, index));
      const bool match = title == gtk_window_get_title(window);
      if (match) watches.push_back(std::make_unique<NativeWatch>(window));
      g_object_unref(window);
      if (match) return;
    }
    throw std::runtime_error("HuxerUI native test window was not found");
  }
};

NativeScenario* current_scenario = nullptr;

View NativeMultiWindowRoot() {
  auto& scenario = *current_scenario;
  const auto identity = UseEnvironment<WindowIdentity>();
  if (identity.key == "throw") throw std::runtime_error("HuxerUI native test composition failure");
  auto& record = scenario.records[identity.ordinal];
  record.identity = identity;
  record.handle = UseWindow();
  record.local = UseState(0);
  record.shared_value = UseService<SharedCounter>()->value.Get();
  ++record.compositions;
  scenario.application = UseApplication();
  Lifecycle([&record] { return [&record] { ++record.unmounts; }; });
  if (identity.ordinal == 1) {
    record.handle->OnCloseRequest([&scenario] {
      ++scenario.close_requests;
      return scenario.block_extra_close;
    });
  }
  if (scenario.content) return scenario.content(identity);
  return Text::Format("{}: {} / {}", identity.key, record.local->Get(), record.shared_value)
      .On<ViewEvents::KeyIntercept>([&scenario, &record, window = *record.handle](const KeyEvent& event) {
        if (record.identity.key != "key-close" || event.type != KeyEventType::Down || event.key != Key::Escape) {
          return false;
        }
        window.Close();
        scenario.key_close_returned = record.unmounts == 0;
        return true;
      });
}

void RunScenario(NativeScenario& scenario, const std::string& expected_failure = {}) {
  current_scenario = &scenario;
  const guint timer = g_timeout_add(10, [](gpointer data) -> gboolean {
    auto& scenario = *static_cast<NativeScenario*>(data);
    try {
      scenario.Check(std::chrono::steady_clock::now() < scenario.deadline, "Native multi-window test timed out");
      if (!scenario.advance()) return G_SOURCE_CONTINUE;
      scenario.completed = true;
    } catch (...) {
      scenario.failure = std::current_exception();
    }
    if (scenario.completed && scenario.native_primary_close) {
      auto* native = GTK_WINDOW(g_weak_ref_get(&scenario.watches.front()->window));
      if (scenario.destroy_primary) {
        gtk_window_destroy(native);
      } else {
        gtk_window_close(native);
      }
      g_object_unref(native);
    } else if (scenario.records.contains(0) && scenario.records.at(0).unmounts == 0) {
      scenario.records.at(0).handle->Close();
    } else if (scenario.application) {
      scenario.application->Quit();
    }
    return G_SOURCE_REMOVE;
  }, &scenario);
  std::exception_ptr runner_failure;
  try {
    const Application application{
        NativeMultiWindowRoot,
        {
            .window = {.title = "HuxerUI native primary", .key = "primary", .initial_size = {320.0F, 240.0F}},
            .extra_windows = {{
                .title = "HuxerUI native extra", .key = "extra", .initial_size = {200.0F, 160.0F},

            }},
            .show_debug_overlay = false,
            .application_hooks = {[&scenario](ApplicationContext& context) {
              scenario.shared = std::make_shared<SharedCounter>();
              context.Provide(scenario.shared);
            }},
        },
    };
    scenario.Check(detail::RunPlatformApplication(application) == 0, "Native application returned a failure code");
  } catch (...) {
    runner_failure = std::current_exception();
  }
  if (g_main_context_find_source_by_id(nullptr, timer) != nullptr) g_source_remove(timer);
  current_scenario = nullptr;
  if (scenario.failure) std::rethrow_exception(scenario.failure);
  if (expected_failure.empty()) {
    if (runner_failure) std::rethrow_exception(runner_failure);
    REQUIRE(scenario.completed);
  } else {
    REQUIRE(runner_failure != nullptr);
    REQUIRE_THROWS_WITH(std::rethrow_exception(runner_failure), expected_failure);
  }
  for (const auto& [ordinal, record] : scenario.records) {
    INFO(ordinal);
    REQUIRE(record.unmounts == 1);
    record.handle->Show();
    record.handle->Close();
  }
  for (const auto& watch : scenario.watches) REQUIRE(watch->Released());
}

} // namespace

#ifdef GDK_WINDOWING_X11
namespace {

// XTest drives the real GTK input path without making X11 a framework or test link dependency.
class NativeDragInput {
public:
  NativeDragInput() {
    display_ = gdk_display_get_default();
    if (!display_ || !GDK_IS_X11_DISPLAY(display_)) return;
    xlib_ = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    xtest_ = dlopen("libXtst.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!xlib_ || !xtest_) return;
    move_ = reinterpret_cast<decltype(move_)>(dlsym(xtest_, "XTestFakeMotionEvent"));
    button_ = reinterpret_cast<decltype(button_)>(dlsym(xtest_, "XTestFakeButtonEvent"));
    place_ = reinterpret_cast<decltype(place_)>(dlsym(xlib_, "XMoveWindow"));
    raise_ = reinterpret_cast<decltype(raise_)>(dlsym(xlib_, "XRaiseWindow"));
    translate_ = reinterpret_cast<decltype(translate_)>(dlsym(xlib_, "XTranslateCoordinates"));
    root_ = reinterpret_cast<decltype(root_)>(dlsym(xlib_, "XDefaultRootWindow"));
  }
  ~NativeDragInput() {
    if (Available()) Button(false);
    if (xtest_) dlclose(xtest_);
    if (xlib_) dlclose(xlib_);
  }
  bool Available() const { return move_ && button_ && place_ && raise_ && translate_ && root_; }
  void Place(GtkWindow* window, int x, int y) {
    const Window xid = gdk_x11_surface_get_xid(gtk_native_get_surface(GTK_NATIVE(window)));
    place_(Display(), xid, x, y);
    raise_(Display(), xid);
    gdk_display_flush(display_);
  }
  void Move(GtkWindow* window, int x, int y) {
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window));
    const Window xid = gdk_x11_surface_get_xid(surface);
    double offset_x = 0.0;
    double offset_y = 0.0;
    gtk_native_get_surface_transform(GTK_NATIVE(window), &offset_x, &offset_y);
    const int scale = gdk_surface_get_scale_factor(surface);
    int root_x = 0;
    int root_y = 0;
    Window child = 0;
    translate_(Display(), xid, root_(Display()), static_cast<int>(std::lround((x - offset_x) * scale)),
               static_cast<int>(std::lround((y - offset_y) * scale)), &root_x, &root_y, &child);
    move_(Display(), -1, root_x, root_y, 0);
    gdk_display_flush(display_);
  }
  void Button(bool down) {
    button_(Display(), 1, down ? True : False, 0);
    gdk_display_flush(display_);
  }

private:
  ::Display* Display() const { return gdk_x11_display_get_xdisplay(display_); }
  GdkDisplay* display_ = nullptr;
  void* xlib_ = nullptr;
  void* xtest_ = nullptr;
  int (*move_)(::Display*, int, int, int, unsigned long) = nullptr;
  int (*button_)(::Display*, unsigned int, Bool, unsigned long) = nullptr;
  int (*place_)(::Display*, Window, int, int) = nullptr;
  int (*raise_)(::Display*, Window) = nullptr;
  Bool (*translate_)(::Display*, Window, Window, int, int, int*, int*, Window*) = nullptr;
  Window (*root_)(::Display*) = nullptr;
};

} // namespace
#endif

// Retiring the source while GTK 4 X11 transport is still live leaves the window's IM multicontext delegate dangling,
// so teardown prints a GLib g_object_unref critical after the assertions pass. The warning is internal to GTK; run
// this case without G_DEBUG=fatal-warnings.
TEST_CASE("LinuxNativeApplicationDragUsesOnePhysicalSequence", "[.][linux-native-drag]") {
#ifdef GDK_WINDOWING_X11
  NativeDragInput input;
  if (!input.Available()) SKIP("X11 display and XTest are required for native pointer delivery");
  NativeScenario scenario;
  std::vector<std::string> events;
  int clicks = 0;
  int raw_downs = 0;
  int stage = 0;
  bool dropped = false;
  bool cancel_source = false;
  bool retire_on_drop = false;
  SECTION("Successful cross-window drop") {}
  SECTION("Source retirement cancels native transport") { cancel_source = true; }
  SECTION("Dropped handler retires its source") { retire_on_drop = true; }
  scenario.content = [&](const WindowIdentity& identity) -> View {
    if (identity.ordinal == 0) {
      return Text("source")
          .With(Frame{160.0F, 80.0F}, DragSource(7).Scope(DragDropScope::Application))
          .On<DragSourceEvents::Started>([&](const DragEvent&) { events.emplace_back("started"); })
          .On<DragSourceEvents::Ended>([&](const DragDropResult& result) {
            dropped = result.dropped;
            events.emplace_back("ended");
          });
    }
    return Button("target")
        .With(Frame{160.0F, 80.0F}, DropTarget::Accepts<int>().Scope(DragDropScope::Application))
        .On<DropEvents<int>::Entered>([&](int, const DropEvent&) { events.emplace_back("entered"); })
        .On<DropEvents<int>::Exited>([&](int, const DropEvent&) { events.emplace_back("exited"); })
        .On<DropEvents<int>::Dropped>([&](int, const DropEvent&) {
          events.emplace_back("dropped");
          if (retire_on_drop) scenario.records.at(0).handle->Retire();
        })
        .On<ViewEvents::Pointer>([&](const PointerEvent& event) {
          if (event.type == PointerEventType::Down) ++raw_downs;
        })
        .OnClick([&] { ++clicks; });
  };
  scenario.advance = [&] {
    if (scenario.records.size() < 2) return false;
    if (stage == 0) {
      scenario.Watch("HuxerUI native primary");
      scenario.Watch("HuxerUI native extra");
      auto source = scenario.watches[0]->Window();
      auto target = scenario.watches[1]->Window();
      if (!gtk_widget_get_mapped(GTK_WIDGET(source.get())) || !gtk_widget_get_mapped(GTK_WIDGET(target.get()))) {
        scenario.watches.clear();
        return false;
      }
      input.Place(source.get(), 80, 120);
      input.Place(target.get(), 600, 120);
      ++stage;
    } else if (stage == 1) {
      auto source = scenario.watches[0]->Window();
      input.Move(source.get(), 50, 45);
      ++stage;
    } else if (stage == 2) {
      input.Button(true);
      ++stage;
    } else if (stage == 3) {
      auto source = scenario.watches[0]->Window();
      input.Move(source.get(), 80, 45);
      ++stage;
    } else if (stage == 4) {
      if (std::ranges::find(events, "started") == events.end()) return false;
      auto target = scenario.watches[1]->Window();
      input.Move(target.get(), 50, 50);
      ++stage;
    } else if (stage == 5) {
      if (std::ranges::find(events, "entered") == events.end()) return false;
      if (cancel_source) scenario.records.at(0).handle->Retire();
      else input.Button(false);
      ++stage;
    } else if (stage == 6) {
      if (cancel_source) {
        if (scenario.records.at(0).unmounts == 0) return false;
        input.Button(false);
        scenario.Check(!dropped && std::ranges::find(events, "exited") != events.end(),
                       "Retirement left a live target");
      } else if (retire_on_drop) {
        if (scenario.records.at(0).unmounts == 0) return false;
        scenario.Check(std::ranges::count(events, "dropped") == 1, "Source retirement duplicated the accepted drop");
      } else {
        if (std::ranges::find(events, "ended") == events.end()) return false;
        scenario.Check(dropped && std::ranges::count(events, "dropped") == 1, "Native drop was not delivered once");
      }
      scenario.Check(clicks == 0 && raw_downs == 0, "Drag fabricated target pointer input");
      scenario.records.at(1).handle->Activate();
      ++stage;
    } else if (stage == 7) {
      auto target = scenario.watches[1]->Window();
      input.Move(target.get(), 60, 50);
      input.Button(true);
      ++stage;
    } else if (stage == 8) {
      input.Button(false);
      ++stage;
    } else if (stage == 9) {
      if (clicks == 0) return false;
      scenario.Check(clicks == 1 && raw_downs == 1, "Native completion left stuck pointer state");
      return true;
    }
    return false;
  };
  std::exception_ptr failure;
  try { RunScenario(scenario); } catch (...) { failure = std::current_exception(); }
  CAPTURE(stage, events);
  if (failure) REQUIRE_NOTHROW(std::rethrow_exception(failure));
#else
  SKIP("GTK was built without X11 support");
#endif
}

} // namespace huxerui::test
