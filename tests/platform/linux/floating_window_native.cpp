#include "linux_internal.h"

#include <gtk/gtk.h>
#include <catch2/catch_amalgamated.hpp>
#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#ifdef None
#undef None
#endif
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <dlfcn.h>
#endif

#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <huxerui/huxerui.h>

namespace huxerui::test {
namespace {

#ifdef GDK_WINDOWING_X11

// Xlib, Xext, and XTest stay behind dlopen so the suite keeps its GTK-only link surface; the probes
// read and drive the same X11 state the adapter writes through GDK's display for that window.
class X11Probe {
public:
  X11Probe() {
    display_ = gdk_display_get_default();
    if (display_ == nullptr || !GDK_IS_X11_DISPLAY(display_)) {
      return;
    }
    xlib_ = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    xext_ = dlopen("libXext.so.6", RTLD_NOW | RTLD_LOCAL);
    xtest_ = dlopen("libXtst.so.6", RTLD_NOW | RTLD_LOCAL);
    if (xlib_ == nullptr) {
      return;
    }
    Resolve(xlib_, "XInternAtom", intern_);
    Resolve(xlib_, "XGetWindowProperty", property_);
    Resolve(xlib_, "XGetWindowAttributes", attributes_);
    Resolve(xlib_, "XGetWMHints", wm_hints_);
    Resolve(xlib_, "XTranslateCoordinates", translate_);
    Resolve(xlib_, "XDefaultRootWindow", root_);
    Resolve(xlib_, "XMoveWindow", move_);
    Resolve(xlib_, "XRaiseWindow", raise_);
    Resolve(xlib_, "XQueryPointer", query_pointer_);
    Resolve(xlib_, "XFree", free_);
    Resolve(xlib_, "XFlush", flush_);
    if (xext_ != nullptr) {
      Resolve(xext_, "XShapeQueryExtension", shape_query_);
      Resolve(xext_, "XShapeGetRectangles", shape_rectangles_);
    }
    if (xtest_ != nullptr) {
      Resolve(xtest_, "XTestFakeMotionEvent", motion_);
      Resolve(xtest_, "XTestFakeButtonEvent", button_);
    }
  }

  ~X11Probe() {
    if (xtest_ != nullptr) dlclose(xtest_);
    if (xext_ != nullptr) dlclose(xext_);
    if (xlib_ != nullptr) dlclose(xlib_);
  }

  X11Probe(const X11Probe&) = delete;
  X11Probe& operator=(const X11Probe&) = delete;

  bool OnX11() const { return xlib_ != nullptr; }

  bool HasShape() const {
    if (shape_query_ == nullptr || shape_rectangles_ == nullptr) {
      return false;
    }
    int event = 0;
    int error = 0;
    return shape_query_(XDisplay(), &event, &error) != 0;
  }

  bool HasXTest() const { return motion_ != nullptr && button_ != nullptr; }

  /// Xwayland on scaled outputs applies the output scale to synthetic pointer motion a second time,
  /// so the physical pointer ends up at a multiple of the requested root coordinates. Measuring that
  /// ratio once keeps synthetic clicks on their windows across plain X11 and scaled XWayland hosts.
  double MotionScale() const {
    if (!HasXTest() || query_pointer_ == nullptr) {
      return 1.0;
    }
    motion_(XDisplay(), -1, 200, 200, 0);
    ::Window root = 0;
    ::Window child = 0;
    int root_x = 0;
    int root_y = 0;
    int win_x = 0;
    int win_y = 0;
    unsigned int mask = 0;
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    const Bool queried = query_pointer_(XDisplay(), root_(XDisplay()), &root, &child, &root_x, &root_y,
                                        &win_x, &win_y, &mask);
    G_GNUC_END_IGNORE_DEPRECATIONS
    if (queried == 0 || root_x <= 0 || root_y <= 0) {
      return 1.0;
    }
    return std::max(1.0, std::min(4.0, (root_x + root_y) / 400.0));
  }

  bool DepthIs32(GtkWindow* window) const {
    XWindowAttributes attributes{};
    return attributes_(XDisplay(), Xid(window), &attributes) != 0 && attributes.depth == 32;
  }

  bool WindowTypeIsUtility(GtkWindow* window) const {
    ::Display* display = XDisplay();
    const Atom type_atom = intern_(display, "_NET_WM_WINDOW_TYPE", False);
    const Atom utility_atom = intern_(display, "_NET_WM_WINDOW_TYPE_UTILITY", False);
    Atom type = 0;
    int format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char* data = nullptr;
    if (property_(display, Xid(window), type_atom, 0, 64, False, XA_ATOM, &type, &format, &count,
                  &remaining, &data) != Success) {
      return false;
    }
    bool utility = false;
    const auto* atoms = reinterpret_cast<const Atom*>(data);
    for (unsigned long index = 0; index < count; ++index) {
      utility = utility || atoms[index] == utility_atom;
    }
    if (data != nullptr) free_(data);
    return utility;
  }

  bool InputHintIsPassive(GtkWindow* window) const {
    XWMHints* hints = wm_hints_(XDisplay(), Xid(window));
    if (hints == nullptr) {
      return false;
    }
    const bool passive = (hints->flags & InputHint) != 0 && hints->input == False;
    free_(hints);
    return passive;
  }

  int InputRegionRectangles(GtkWindow* window) const {
    int count = -1;
    int ordering = 0;
    XRectangle* rectangles = shape_rectangles_(XDisplay(), Xid(window), ShapeInput, &count, &ordering);
    if (rectangles != nullptr) free_(rectangles);
    return count;
  }

  /// Raises the window at a known root-pixel position so synthetic input reaches it deterministically.
  void Place(GtkWindow* window, int root_x, int root_y) const {
    move_(XDisplay(), Xid(window), root_x, root_y);
    raise_(XDisplay(), Xid(window));
    flush_(XDisplay());
  }

  /// Translates a window-relative logical point into root pixels for the current surface geometry.
  void RootPoint(GtkWindow* window, double x, double y, int* root_x, int* root_y) const {
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window));
    double offset_x = 0.0;
    double offset_y = 0.0;
    gtk_native_get_surface_transform(GTK_NATIVE(window), &offset_x, &offset_y);
    const int scale = gdk_surface_get_scale_factor(surface);
    ::Window child = 0;
    translate_(XDisplay(), Xid(window), root_(XDisplay()),
               static_cast<int>(std::lround((x - offset_x) * scale)),
               static_cast<int>(std::lround((y - offset_y) * scale)), root_x, root_y, &child);
    flush_(XDisplay());
  }

  void Motion(int root_x, int root_y, double motion_scale) const {
    motion_(XDisplay(), -1, static_cast<int>(std::lround(root_x / motion_scale)),
            static_cast<int>(std::lround(root_y / motion_scale)), 0);
    flush_(XDisplay());
  }

  void Button(bool down) const {
    button_(XDisplay(), 1, down ? True : False, 0);
    flush_(XDisplay());
  }

private:
  ::Display* XDisplay() const {
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    return gdk_x11_display_get_xdisplay(display_);
    G_GNUC_END_IGNORE_DEPRECATIONS
  }

  ::Window Xid(GtkWindow* window) const {
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window));
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    return gdk_x11_surface_get_xid(surface);
    G_GNUC_END_IGNORE_DEPRECATIONS
  }

  template <typename Signature>
  void Resolve(void* library, const char* name, Signature& target) const {
    target = reinterpret_cast<Signature>(dlsym(library, name));
  }

  GdkDisplay* display_ = nullptr;
  void* xlib_ = nullptr;
  void* xext_ = nullptr;
  void* xtest_ = nullptr;
  Atom (*intern_)(::Display*, const char*, Bool) = nullptr;
  int (*property_)(::Display*, ::Window, Atom, long, long, Bool, Atom, Atom*, int*, unsigned long*,
                   unsigned long*, unsigned char**) = nullptr;
  int (*attributes_)(::Display*, ::Window, XWindowAttributes*) = nullptr;
  XWMHints* (*wm_hints_)(::Display*, ::Window) = nullptr;
  Bool (*translate_)(::Display*, ::Window, ::Window, int, int, int*, int*, ::Window*) = nullptr;
  ::Window (*root_)(::Display*) = nullptr;
  int (*move_)(::Display*, ::Window, int, int) = nullptr;
  int (*raise_)(::Display*, ::Window) = nullptr;
  Bool (*query_pointer_)(::Display*, ::Window, ::Window*, ::Window*, int*, int*, int*, int*,
                         unsigned int*) = nullptr;
  int (*free_)(void*) = nullptr;
  int (*flush_)(::Display*) = nullptr;
  Bool (*shape_query_)(::Display*, int*, int*) = nullptr;
  XRectangle* (*shape_rectangles_)(::Display*, ::Window, int, int*, int*) = nullptr;
  int (*motion_)(::Display*, int, int, int, unsigned long) = nullptr;
  int (*button_)(::Display*, unsigned int, Bool, unsigned long) = nullptr;
};

#else

// GTK built without X11 keeps the scenario compiling; every probe reports an absent X11 backend.
class X11Probe {
public:
  bool OnX11() const { return false; }
  bool HasShape() const { return false; }
  bool HasXTest() const { return false; }
  double MotionScale() const { return 1.0; }
  bool DepthIs32(GtkWindow*) const { return false; }
  bool WindowTypeIsUtility(GtkWindow*) const { return false; }
  bool InputHintIsPassive(GtkWindow*) const { return false; }
  int InputRegionRectangles(GtkWindow*) const { return -1; }
  void Place(GtkWindow*, int, int) const {}
  void RootPoint(GtkWindow*, double, double, int* root_x, int* root_y) const {
    *root_x = 0;
    *root_y = 0;
  }
  void Motion(int, int, double) const {}
  void Button(bool) const {}
};

#endif

struct FloatingScenario {
  std::optional<WindowHandle> primary;
  std::optional<ApplicationHandle> application;
  std::function<bool()> advance;
  std::exception_ptr failure;
  std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(60);
  int presses = 0;
  int cancels = 0;
  int settle = 0;
  int stage = 0;
  bool completed = false;

  void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string("HuxerUI ") + message);
  }

  /// Borrows the live GTK toplevel with this title for the current main-loop iteration.
  GtkWindow* Native(const std::string& title) const {
    GListModel* windows = gtk_window_get_toplevels();
    for (guint index = 0; index < g_list_model_get_n_items(windows); ++index) {
      auto* window = GTK_WINDOW(g_list_model_get_item(windows, index));
      const bool match = title == gtk_window_get_title(window);
      g_object_unref(window);
      if (match) return window;
    }
    return nullptr;
  }
};

FloatingScenario* current_scenario = nullptr;

View FloatingContentApp() {
  auto& scenario = *current_scenario;
  if (!scenario.primary.has_value()) {
    scenario.primary = UseWindow();
    scenario.application = UseApplication();
  }
  return Canvas([](PaintContext& paint, Size) {
    paint.DrawRect({40.0F, 40.0F, 160.0F, 160.0F}, Color::Rgb(240, 60, 30));
    paint.DrawRect({8.0F, 40.0F, 24.0F, 160.0F}, Color::Rgb(240, 60, 30, 0.5F));
  })
      .With(Frame{200.0F, 200.0F})
      .On<ViewEvents::Pointer>([](const PointerEvent& event) {
        auto& scenario = *current_scenario;
        if (event.type == PointerEventType::Down) ++scenario.presses;
        if (event.type == PointerEventType::Cancel) ++scenario.cancels;
      });
}

void RunFloatingScenario(FloatingScenario& scenario) {
  current_scenario = &scenario;
  const guint timer = g_timeout_add(10, [](gpointer data) -> gboolean {
    auto& scenario = *static_cast<FloatingScenario*>(data);
    try {
      scenario.Check(std::chrono::steady_clock::now() < scenario.deadline,
                     ("floating-window native test timed out at stage " + std::to_string(scenario.stage)).c_str());
      if (!scenario.advance()) return G_SOURCE_CONTINUE;
      scenario.completed = true;
    } catch (...) {
      scenario.failure = std::current_exception();
    }
    if (scenario.application.has_value()) {
      scenario.application->Quit();
    } else if (scenario.primary.has_value()) {
      scenario.primary->Close();
    }
    return G_SOURCE_REMOVE;
  }, &scenario);
  std::exception_ptr runner_failure;
  try {
    const Application application{
        FloatingContentApp,
        {
            .window = {.title = "HuxerUI floating probe", .key = "primary",
                       .initial_size = {240.0F, 240.0F}},
            .show_debug_overlay = false,
        },
    };
    static_cast<void>(detail::RunPlatformApplication(application));
  } catch (...) {
    runner_failure = std::current_exception();
  }
  if (g_main_context_find_source_by_id(nullptr, timer) != nullptr) g_source_remove(timer);
  current_scenario = nullptr;
  if (scenario.failure) std::rethrow_exception(scenario.failure);
  if (runner_failure) std::rethrow_exception(runner_failure);
  REQUIRE(scenario.completed);
}

bool HasTransparentCssClass(GtkWindow* window) {
  const char* const* classes = gtk_widget_get_css_classes(GTK_WIDGET(window));
  for (const char* const* name = classes; name != nullptr && *name != nullptr; ++name) {
    if (std::string_view(*name) == "huxerui-transparent") {
      return true;
    }
  }
  return false;
}

} // namespace

// Hidden: creates real GTK toplevels and drives the physical pointer on the developer's display, so it
// runs only on demand through the [linux-native-floating] tag. Run it under both GDK backends: the
// default session (Wayland here) and GDK_BACKEND=x11 through XWayland.
TEST_CASE("LinuxNativeFloatingWindowHonorsBackendCapabilitiesAndRequests", "[.][linux-native-floating]") {
  X11Probe probe;
  const bool x11 = probe.OnX11();
  const bool shape = x11 && probe.HasShape();
  const bool xtest = x11 && probe.HasXTest();
  const double motion_scale = probe.MotionScale();

  FloatingScenario scenario;
  WindowCapabilities capabilities{};
  std::optional<WindowHandle> floating;
  std::optional<WindowHandle> reopened;
  std::optional<WindowHandle> plain;
  std::optional<Rect> work_area;
  int& stage = scenario.stage;
  int press_baseline = 0;
  int move_attempts = 0;
  int reopen_cycle = 0;
  bool invalid_argument_rejected = false;
  int logic_error_rejections = 0;

  scenario.advance = [&]() -> bool {
    auto& self = scenario;
    if (stage == 0) {
      GtkWindow* primary = self.Native("HuxerUI floating probe");
      if (!self.primary.has_value() || primary == nullptr || !gtk_widget_get_mapped(GTK_WIDGET(primary))) {
        return false;
      }
      capabilities = self.primary->Capabilities();
      // Wayland owns activation, stacking, and placement by protocol; both backends share the
      // transparent utility surface, sizing, and interactive moves through GDK.
      self.Check(capabilities.transparent_surface, "transparent surfaces were unavailable");
      self.Check(capabilities.utility_presentation, "utility presentation was unavailable");
      self.Check(capabilities.native_move, "native moves were unavailable");
      self.Check(capabilities.passive_display == x11, "passive display capability mismatched the backend");
      self.Check(capabilities.topmost == x11, "topmost capability mismatched the backend");
      self.Check(capabilities.geometry == x11, "geometry capability mismatched the backend");
      self.Check(!capabilities.input_passthrough || x11, "input passthrough was offered without X11");
      if (shape) {
        self.Check(capabilities.input_passthrough, "input passthrough missed the X11 shape extension");
      }
      work_area = self.primary->WorkArea();
      if (x11) {
        self.Check(work_area.has_value() && work_area->width > 0.0F && work_area->height > 0.0F,
                   "X11 work area was not reported");
      } else {
        self.Check(!work_area.has_value(), "Wayland exposed monitor geometry as a work area");
      }
      self.Check(self.primary->BeginMove() == WindowRequestResult::Rejected,
                 "native move began without an active pointer press");
      ++stage;
    } else if (stage == 1) {
      // Shared validation rejects invalid floating configuration on every backend.
      try {
        static_cast<void>(self.application->OpenWindow({
            .title = "HuxerUI floating invalid",
            .initial_size = {120.0F, 90.0F},
            .presentation = WindowPresentation::Standard,
            .transparent_surface = true,
        }));
      } catch (const std::invalid_argument&) {
        invalid_argument_rejected = true;
      }
      self.Check(invalid_argument_rejected, "transparent standard windows were accepted");
      if (!x11) {
        // Wayland compositors own activation and stacking, so requesting them must fail creation
        // instead of silently creating an ordinary window.
        for (int option = 0; option < 2; ++option) {
          try {
            WindowOptions options{
                .title = "HuxerUI floating unavailable",
                .initial_size = {120.0F, 90.0F},
                .presentation = WindowPresentation::Utility,
            };
            if (option == 0) {
              options.show_without_activation = true;
            } else {
              options.topmost = true;
            }
            static_cast<void>(self.application->OpenWindow(options));
          } catch (const std::logic_error&) {
            ++logic_error_rejections;
          }
        }
        self.Check(logic_error_rejections == 2, "unavailable Wayland policies were accepted");
      }
      ++stage;
    } else if (stage == 2) {
      if (!floating.has_value()) {
        WindowOptions options{
            .title = "HuxerUI floating pet",
            .initial_size = {200.0F, 200.0F},
            .presentation = WindowPresentation::Utility,
            .transparent_surface = true,
        };
        if (x11) {
          options.initial_position = Point{120.0F, 90.0F};
          options.show_without_activation = true;
          options.topmost = true;
        }
        floating = self.application->OpenWindow(options);
        return false;
      }
      GtkWindow* pet = self.Native("HuxerUI floating pet");
      if (pet == nullptr || !gtk_widget_get_mapped(GTK_WIDGET(pet))) {
        return false;
      }
      // Let the window manager settle its focus decision for the freshly mapped surface.
      if (++self.settle < 40) {
        return false;
      }
      self.settle = 0;
      self.Check(!gtk_window_get_decorated(pet), "utility window kept its decoration");
      self.Check(!gtk_window_get_resizable(pet), "utility window stayed resizable");
      self.Check(HasTransparentCssClass(pet), "transparent surface did not switch the window background");
      if (x11) {
        self.Check(probe.DepthIs32(pet), "transparent window did not use a 32-bit visual");
        self.Check(probe.WindowTypeIsUtility(pet), "X11 window type missed UTILITY");
        self.Check(probe.InputHintIsPassive(pet), "passive window kept its X11 input hint");
        self.Check(!gtk_window_is_active(pet), "passive utility window held keyboard activation");
      }
      ++stage;
    } else if (stage == 3) {
      GtkWindow* pet = self.Native("HuxerUI floating pet");
      self.Check(pet != nullptr, "floating window vanished before sizing");
      self.Check(floating->SetClientSize({220.0F, 180.0F}) == WindowRequestResult::Submitted,
                 "client size request was not submitted");
      int width = 0;
      int height = 0;
      gtk_window_get_default_size(pet, &width, &height);
      self.Check(width == 220 && height == 180, "client size request did not adopt the default size");
      // Resizing re-configures the native surface and window managers re-place such windows; let
      // that settle before reading or requesting positions.
      if (++self.settle < 80) {
        return false;
      }
      self.settle = 0;
      const WindowRequestResult placement = floating->SetPosition({210.0F, 150.0F});
      if (x11) {
        self.Check(placement == WindowRequestResult::Submitted, "X11 position request was not submitted");
        const std::optional<Point> origin = floating->Position();
        if (!origin.has_value() || std::abs(origin->x - 210.0F) > 2.0F ||
            std::abs(origin->y - 150.0F) > 2.0F) {
          return false;
        }
        self.Check(origin->x >= work_area->x && origin->x <= work_area->x + work_area->width &&
                       origin->y >= work_area->y && origin->y <= work_area->y + work_area->height,
                   "reported origin fell outside the reported work area");
      } else {
        self.Check(placement == WindowRequestResult::Unavailable,
                   "Wayland position request was not unavailable");
      }
      ++stage;
    } else if (stage == 4) {
      const WindowRequestResult passthrough = floating->SetInputPassthrough(true);
      if (shape) {
        self.Check(passthrough == WindowRequestResult::Submitted, "passthrough request was not submitted");
        self.Check(probe.InputRegionRectangles(self.Native("HuxerUI floating pet")) == 0,
                   "passthrough left X11 input rectangles behind");
        self.Check(floating->SetInputPassthrough(false) == WindowRequestResult::Submitted,
                   "restoring input was not submitted");
        self.Check(probe.InputRegionRectangles(self.Native("HuxerUI floating pet")) > 0,
                   "restoring input did not bring back X11 input rectangles");
      } else {
        self.Check(passthrough == WindowRequestResult::Unavailable,
                   "passthrough was offered without the X11 shape extension");
      }
      self.Check(floating->SetTopmost(false) ==
                     (x11 ? WindowRequestResult::Submitted : WindowRequestResult::Unavailable),
                 "topmost request mismatched the backend");
      press_baseline = self.presses;
      ++stage;
    } else if (stage == 5) {
      // Synthetic clicks can land on stacked foreign windows, so delivery retries with a raised and
      // freshly placed window until the shared content observes the press.
      if (xtest) {
        GtkWindow* pet = self.Native("HuxerUI floating pet");
        if (pet == nullptr || !gtk_widget_get_mapped(GTK_WIDGET(pet))) {
          return false;
        }
        if (self.presses <= press_baseline) {
          if (++self.settle % 30 == 1) {
            probe.Place(pet, 240, 200);
            int root_x = 0;
            int root_y = 0;
            probe.RootPoint(pet, 100.0, 100.0, &root_x, &root_y);
            probe.Motion(root_x, root_y, motion_scale);
            probe.Button(true);
            probe.Button(false);
          }
          return false;
        }
      }
      ++stage;
    } else if (stage == 6) {
      // Hold a fresh press for the passthrough-during-press and native-move stages.
      if (xtest) {
        GtkWindow* pet = self.Native("HuxerUI floating pet");
        if (pet == nullptr) {
          return false;
        }
        if (self.presses <= press_baseline + 1) {
          if (++self.settle % 30 == 1) {
            probe.Place(pet, 240, 200);
            int root_x = 0;
            int root_y = 0;
            probe.RootPoint(pet, 100.0, 100.0, &root_x, &root_y);
            probe.Motion(root_x, root_y, motion_scale);
            probe.Button(true);
          }
          return false;
        }
        if (shape) {
          // Enabling passthrough mid-press cancels the shared pointer sequence and releases capture.
          self.Check(floating->SetInputPassthrough(true) == WindowRequestResult::Submitted,
                     "passthrough during a press was not submitted");
          if (self.cancels == 0) {
            return false;
          }
          self.Check(floating->SetInputPassthrough(false) == WindowRequestResult::Submitted,
                     "restoring input during a press was not submitted");
          probe.Button(false);
        }
        press_baseline = self.presses;
      }
      ++stage;
    } else if (stage == 7) {
      // Start one interactive-move attempt: re-establish a held press after the passthrough cancel,
      // submit the native move, and drag the pointer before releasing.
      if (xtest) {
        GtkWindow* pet = self.Native("HuxerUI floating pet");
        if (pet == nullptr) {
          return false;
        }
        if (self.presses <= press_baseline) {
          if (++self.settle % 30 == 1) {
            probe.Place(pet, 240, 200);
            int root_x = 0;
            int root_y = 0;
            probe.RootPoint(pet, 100.0, 100.0, &root_x, &root_y);
            probe.Motion(root_x, root_y, motion_scale);
            probe.Button(true);
          }
          return false;
        }
        self.Check(floating->BeginMove() == WindowRequestResult::Submitted,
                   "native move with an active press was rejected");
        int root_x = 0;
        int root_y = 0;
        probe.RootPoint(pet, 100.0, 100.0, &root_x, &root_y);
        probe.Motion(root_x + 120, root_y + 60, motion_scale);
        probe.Button(false);
        press_baseline = self.presses;
        self.settle = 0;
      }
      ++stage;
    } else if (stage == 8) {
      // The compositor owns the gesture, so an attempt that never tracks the synthetic pointer is
      // retried a bounded number of times before the relocation outcome is required.
      if (xtest) {
        const std::optional<Point> origin = floating->Position();
        GtkWindow* pet = self.Native("HuxerUI floating pet");
        const float scale = static_cast<float>(gtk_widget_get_scale_factor(GTK_WIDGET(pet)));
        const bool relocated = origin.has_value() && origin->x * scale > 340.0F;
        if (!relocated && ++self.settle < 250 && move_attempts < 3) {
          return false;
        }
        if (!relocated) {
          if (++move_attempts < 3) {
            self.settle = 0;
            --stage;  // Re-establish the press and submit another move attempt.
            return false;
          }
          self.Check(false, "interactive native move did not relocate the window");
        }
      }
      floating->Retire();
      ++stage;
    } else if (stage == 9) {
      if (floating->Capabilities() != WindowCapabilities{}) {
        return false;
      }
      self.Check(floating->SetInputPassthrough(true) == WindowRequestResult::Disconnected,
                 "retired floating window accepted passthrough");
      self.Check(floating->SetClientSize({120.0F, 90.0F}) == WindowRequestResult::Disconnected,
                 "retired floating window accepted a resize");
      ++stage;
    } else if (stage == 10) {
      // Repeated retirement and reopening must keep honoring the floating policies.
      if (!reopened.has_value()) {
        if (self.Native("HuxerUI floating pet") != nullptr) {
          return false;  // The previous cycle's window is still tearing down.
        }
        reopened = self.application->OpenWindow({
            .title = "HuxerUI floating pet",
            .initial_size = {200.0F, 200.0F},
            .presentation = WindowPresentation::Utility,
            .transparent_surface = true,
        });
        return false;
      }
      GtkWindow* pet = self.Native("HuxerUI floating pet");
      if (pet == nullptr || !gtk_widget_get_mapped(GTK_WIDGET(pet))) {
        return false;
      }
      self.Check(!gtk_window_get_decorated(pet), "reopened utility window was decorated");
      if (++reopen_cycle < 2) {
        reopened->Retire();
        reopened.reset();
      } else {
        self.Check(reopened->Capabilities().transparent_surface,
                   "reopened floating window lost transparency");
        ++stage;
      }
    } else if (stage == 11) {
      // The transparent background override is window-scoped: an ordinary window opened afterwards
      // keeps the theme background and decoration.
      if (!plain.has_value()) {
        plain = self.application->OpenWindow({
            .title = "HuxerUI floating plain",
            .initial_size = {160.0F, 120.0F},
        });
        return false;
      }
      GtkWindow* pet_plain = self.Native("HuxerUI floating plain");
      if (pet_plain == nullptr || !gtk_widget_get_mapped(GTK_WIDGET(pet_plain))) {
        return false;
      }
      self.Check(gtk_window_get_decorated(pet_plain), "ordinary window lost its decoration");
      self.Check(!HasTransparentCssClass(pet_plain), "transparent background leaked across windows");
      plain->Retire();
      return true;
    }
    return false;
  };

  RunFloatingScenario(scenario);

  if (xtest) {
    REQUIRE(scenario.presses >= 3);
  }
  if (xtest && shape) {
    REQUIRE(scenario.cancels >= 1);
  }
}

TEST_CASE("LinuxNativeFloatingGeometryUsesLogicalClientCoordinates", "[.][linux-native-floating-geometry]") {
  X11Probe probe;
  FloatingScenario scenario;
  std::optional<WindowHandle> utility;
  std::optional<WindowHandle> decorated;
  int& stage = scenario.stage;
  scenario.advance = [&]() {
    if (!scenario.application || !scenario.primary) return false;
    if (stage == 0) {
      utility = scenario.application->OpenWindow({
          .title = "HuxerUI logical utility", .initial_size = {120.0F, 100.0F},
          .initial_position = Point{80.0F, 70.0F}, .presentation = WindowPresentation::Utility,
      });
      decorated = scenario.application->OpenWindow({
          .title = "HuxerUI logical decorated", .initial_size = {140.0F, 110.0F},
          .initial_position = Point{120.0F, 100.0F},
      });
      ++stage;
      return false;
    }
    GtkWindow* native_utility = scenario.Native("HuxerUI logical utility");
    GtkWindow* native_decorated = scenario.Native("HuxerUI logical decorated");
    if (native_utility == nullptr || native_decorated == nullptr ||
        !gtk_widget_get_mapped(GTK_WIDGET(native_utility)) ||
        !gtk_widget_get_mapped(GTK_WIDGET(native_decorated))) return false;
    if (++scenario.settle < 20) return false;
    scenario.settle = 0;
    if (!probe.OnX11()) {
      scenario.Check(!utility->Position() && !utility->WorkArea(), "Wayland returned global geometry");
      scenario.Check(utility->SetPosition({80.0F, 70.0F}) == WindowRequestResult::Unavailable,
                     "Wayland accepted global placement");
      return true;
    }
#ifdef GDK_WINDOWING_X11
    const auto verify_origin = [&](const WindowHandle& handle, GtkWindow* window, Point expected) {
      GtkWidget* canvas = gtk_window_get_child(window);
      const graphene_point_t zero{0.0F, 0.0F};
      graphene_point_t offset{};
      scenario.Check(gtk_widget_compute_point(canvas, GTK_WIDGET(window), &zero, &offset),
                     "canvas origin was unavailable");
      int root_x = 0;
      int root_y = 0;
      probe.RootPoint(window, offset.x, offset.y, &root_x, &root_y);
      const float scale = static_cast<float>(gtk_widget_get_scale_factor(GTK_WIDGET(window)));
      const auto origin = handle.Position();
      scenario.Check(origin && std::abs(origin->x * scale - root_x) < 1.1F &&
                         std::abs(origin->y * scale - root_y) < 1.1F,
                     "public origin did not describe the logical canvas origin");
      return std::abs(origin->x - expected.x) < 1.1F && std::abs(origin->y - expected.y) < 1.1F;
    };
    if (stage == 1) {
      if (!verify_origin(*utility, native_utility, {80.0F, 70.0F}) ||
          !verify_origin(*decorated, native_decorated, {120.0F, 100.0F})) return false;
      scenario.Check(decorated->SetPosition({160.0F, 120.0F}) == WindowRequestResult::Submitted,
                     "decorated placement was refused");
      scenario.Check(utility->SetPosition({200.0F, 160.0F}) == WindowRequestResult::Submitted,
                     "utility placement was refused");
      ++stage;
      return false;
    }
    if (!verify_origin(*utility, native_utility, {200.0F, 160.0F}) ||
        !verify_origin(*decorated, native_decorated, {160.0F, 120.0F})) return false;
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(native_utility));
    GdkMonitor* monitor = gdk_display_get_monitor_at_surface(gdk_surface_get_display(surface), surface);
    GdkRectangle native_work{};
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gdk_x11_monitor_get_workarea(monitor, &native_work);
    G_GNUC_END_IGNORE_DEPRECATIONS
    const auto work = utility->WorkArea();
    scenario.Check(work && work->x == native_work.x && work->y == native_work.y &&
                       work->width == native_work.width && work->height == native_work.height,
                   "work area included reserved space or physical scaling");
    bool rejected = false;
    try {
      static_cast<void>(utility->SetPosition({std::numeric_limits<float>::max(), 0.0F}));
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    scenario.Check(rejected, "unrepresentable native placement was not rejected");
#endif
    return true;
  };
  RunFloatingScenario(scenario);
}

} // namespace huxerui::test
