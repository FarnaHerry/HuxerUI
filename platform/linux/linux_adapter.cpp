#include "linux_internal.h"

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#ifdef None
#undef None
#endif
#if defined(HUXERUI_LINUX_X11)
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#endif
#if defined(HUXERUI_LINUX_X11) && defined(HUXERUI_LINUX_X11_SHAPE)
#include <X11/extensions/shape.h>
#endif
#endif

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/clipboard.h>
#include <huxerui/file.h>
#include <huxerui/resource.h>
#include <huxerui/window.h>

#include "internal_access.h"
#include "io/file_internal.h"
#include "internal_access.h"
#include "io/http_internal.h"
#include "linux_file_internal.h"
#include "linux_file_picker_internal.h"
#include "linux_http_internal.h"
#include "linux_platform_view.h"
#include "linux_renderer.h"
#include "linux_system_tray.h"
#include "linux_text_input.h"
#include "linux_ui_dispatcher.h"
#include "application/platform_frame_internal.h"
#include "application/window_internal.h"
#include "resources/resource_internal.h"
#include "text/text_internal.h"
#include "runtime/window_drag_internal.h"
#include "application/window_internal.h"

namespace huxerui::detail {
namespace {

constexpr float kDipsPerScrollStep = 40.0F;

struct LinuxWindowReference {
  GWeakRef window;
  explicit LinuxWindowReference(GtkWindow* native) { g_weak_ref_init(&window, native); }
  ~LinuxWindowReference() { g_weak_ref_clear(&window); }
};

unsigned long X11WindowId(GtkWindow* window) noexcept {
#ifdef GDK_WINDOWING_X11
  if (window != nullptr) {
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window));
    if (surface != nullptr && GDK_IS_X11_SURFACE(surface)) {
      G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      return gdk_x11_surface_get_xid(surface);
      G_GNUC_END_IGNORE_DEPRECATIONS
    }
  }
#endif
  return 0;
}

#if defined(GDK_WINDOWING_X11) && defined(HUXERUI_LINUX_X11)
constexpr int kNetWmStateRemove = 0;
constexpr int kNetWmStateAdd = 1;

/// EWMH state transitions are requests to the window manager: the client message is sent after the
/// window maps because GTK rewrites _NET_WM_STATE from its own surface state during mapping.
void SendNetWmStateMessage(::Display* xdisplay, ::Window window, const char* state_name, int action) {
  XEvent message{};
  message.xclient.type = ClientMessage;
  message.xclient.window = window;
  message.xclient.message_type = XInternAtom(xdisplay, "_NET_WM_STATE", False);
  message.xclient.format = 32;
  message.xclient.data.l[0] = action;
  message.xclient.data.l[1] = static_cast<long>(XInternAtom(xdisplay, state_name, False));
  message.xclient.data.l[2] = 0;
  XSendEvent(xdisplay, DefaultRootWindow(xdisplay), False, SubstructureRedirectMask | SubstructureNotifyMask, &message);
}

/// Resolves the X11 connection behind a realized GTK window for direct protocol requests.
struct X11SurfaceRequest {
  ::Display* display = nullptr;
  ::Window window = 0;

  explicit X11SurfaceRequest(GtkWindow* gtk_window) {
    GdkSurface* surface = gtk_window != nullptr ? gtk_native_get_surface(GTK_NATIVE(gtk_window)) : nullptr;
    if (surface == nullptr || !GDK_IS_X11_SURFACE(surface)) {
      return;
    }
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    display = gdk_x11_display_get_xdisplay(gdk_surface_get_display(surface));
    window = gdk_x11_surface_get_xid(surface);
    G_GNUC_END_IGNORE_DEPRECATIONS
  }

  explicit operator bool() const { return display != nullptr && window != 0; }
};
#endif

/// The theme paints the GtkWindow background node opaquely; transparent utility surfaces override
/// that node only for windows carrying this class, so ordinary windows keep their theme background.
constexpr const char* kTransparentWindowCss = "window.huxerui-transparent { background: transparent; }";

/// Displays whose style cascade already carries the transparent-background rule. One
/// display-lifetime registration avoids re-invalidating every window's style each time a
/// transparent window retires, mirrors how theme providers live with their display, and lets
/// windows opt in and out through the CSS class alone.
std::vector<GdkDisplay*> transparent_background_displays;

void EnsureTransparentBackgroundProvider(GdkDisplay* display) {
  if (std::find(transparent_background_displays.begin(), transparent_background_displays.end(), display) !=
      transparent_background_displays.end()) {
    return;
  }
  GtkCssProvider* provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider, kTransparentWindowCss);
  gtk_style_context_add_provider_for_display(
      display, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
  );
  // The display's style cascade owns the registration from here on, matching theme providers.
  g_object_unref(provider);
  transparent_background_displays.push_back(display);
}

const char* LinuxPointerCursorName(PointerCursorKind kind) noexcept {
  switch (kind) {
  case PointerCursorKind::Default:
    return "default";
  case PointerCursorKind::Text:
    return "text";
  case PointerCursorKind::Hand:
    return "pointer";
  case PointerCursorKind::Crosshair:
    return "crosshair";
  case PointerCursorKind::Move:
    return "move";
  case PointerCursorKind::Grab:
    return "grab";
  case PointerCursorKind::Grabbing:
    return "grabbing";
  case PointerCursorKind::ResizeHorizontal:
    return "ew-resize";
  case PointerCursorKind::ResizeVertical:
    return "ns-resize";
  case PointerCursorKind::ResizeNorthEastSouthWest:
    return "nesw-resize";
  case PointerCursorKind::ResizeNorthWestSouthEast:
    return "nwse-resize";
  case PointerCursorKind::NotAllowed:
    return "not-allowed";
  case PointerCursorKind::Wait:
    return "wait";
  }
  return "default";
}

Key TranslateKey(guint key_value) noexcept {
  if ((key_value >= GDK_KEY_a && key_value <= GDK_KEY_z) ||
      (key_value >= GDK_KEY_A && key_value <= GDK_KEY_Z)) {
    const guint letter = key_value >= GDK_KEY_a ? key_value - GDK_KEY_a : key_value - GDK_KEY_A;
    return static_cast<Key>(static_cast<int>(Key::A) + static_cast<int>(letter));
  }
  if (key_value >= GDK_KEY_0 && key_value <= GDK_KEY_9) {
    return static_cast<Key>(static_cast<int>(Key::Digit0) + static_cast<int>(key_value - GDK_KEY_0));
  }
  if (key_value >= GDK_KEY_F1 && key_value <= GDK_KEY_F24) {
    return static_cast<Key>(static_cast<int>(Key::F1) + static_cast<int>(key_value - GDK_KEY_F1));
  }
  if (key_value >= GDK_KEY_KP_0 && key_value <= GDK_KEY_KP_9) {
    return static_cast<Key>(static_cast<int>(Key::Numpad0) + static_cast<int>(key_value - GDK_KEY_KP_0));
  }
  switch (key_value) {
  case GDK_KEY_Shift_L:
    return Key::ShiftLeft;
  case GDK_KEY_Shift_R:
    return Key::ShiftRight;
  case GDK_KEY_Control_L:
    return Key::ControlLeft;
  case GDK_KEY_Control_R:
    return Key::ControlRight;
  case GDK_KEY_Alt_L:
    return Key::AltLeft;
  case GDK_KEY_Alt_R:
    return Key::AltRight;
  case GDK_KEY_Meta_L:
  case GDK_KEY_Super_L:
    return Key::MetaLeft;
  case GDK_KEY_Meta_R:
  case GDK_KEY_Super_R:
    return Key::MetaRight;
  case GDK_KEY_BackSpace:
    return Key::Backspace;
  case GDK_KEY_Tab:
  case GDK_KEY_ISO_Left_Tab:
    return Key::Tab;
  case GDK_KEY_Return:
    return Key::Enter;
  case GDK_KEY_Escape:
    return Key::Escape;
  case GDK_KEY_space:
    return Key::Space;
  case GDK_KEY_Insert:
    return Key::Insert;
  case GDK_KEY_Delete:
    return Key::Delete;
  case GDK_KEY_Left:
    return Key::ArrowLeft;
  case GDK_KEY_Right:
    return Key::ArrowRight;
  case GDK_KEY_Up:
    return Key::ArrowUp;
  case GDK_KEY_Down:
    return Key::ArrowDown;
  case GDK_KEY_Home:
    return Key::Home;
  case GDK_KEY_End:
    return Key::End;
  case GDK_KEY_Page_Up:
    return Key::PageUp;
  case GDK_KEY_Page_Down:
    return Key::PageDown;
  case GDK_KEY_grave:
    return Key::Backquote;
  case GDK_KEY_minus:
    return Key::Minus;
  case GDK_KEY_equal:
    return Key::Equal;
  case GDK_KEY_bracketleft:
    return Key::BracketLeft;
  case GDK_KEY_bracketright:
    return Key::BracketRight;
  case GDK_KEY_backslash:
    return Key::Backslash;
  case GDK_KEY_semicolon:
    return Key::Semicolon;
  case GDK_KEY_apostrophe:
    return Key::Quote;
  case GDK_KEY_comma:
    return Key::Comma;
  case GDK_KEY_period:
    return Key::Period;
  case GDK_KEY_slash:
    return Key::Slash;
  case GDK_KEY_yen:
    return Key::IntlYen;
  case GDK_KEY_Caps_Lock:
    return Key::CapsLock;
  case GDK_KEY_Num_Lock:
    return Key::NumLock;
  case GDK_KEY_Scroll_Lock:
    return Key::ScrollLock;
  case GDK_KEY_Print:
    return Key::PrintScreen;
  case GDK_KEY_Pause:
    return Key::Pause;
  case GDK_KEY_Menu:
    return Key::ContextMenu;
  case GDK_KEY_Help:
    return Key::Help;
  case GDK_KEY_KP_Insert:
    return Key::Numpad0;
  case GDK_KEY_KP_End:
    return Key::Numpad1;
  case GDK_KEY_KP_Down:
    return Key::Numpad2;
  case GDK_KEY_KP_Page_Down:
    return Key::Numpad3;
  case GDK_KEY_KP_Left:
    return Key::Numpad4;
  case GDK_KEY_KP_Begin:
    return Key::Numpad5;
  case GDK_KEY_KP_Right:
    return Key::Numpad6;
  case GDK_KEY_KP_Home:
    return Key::Numpad7;
  case GDK_KEY_KP_Up:
    return Key::Numpad8;
  case GDK_KEY_KP_Page_Up:
    return Key::Numpad9;
  case GDK_KEY_KP_Delete:
  case GDK_KEY_KP_Decimal:
    return Key::NumpadDecimal;
  case GDK_KEY_KP_Divide:
    return Key::NumpadDivide;
  case GDK_KEY_KP_Multiply:
    return Key::NumpadMultiply;
  case GDK_KEY_KP_Subtract:
    return Key::NumpadSubtract;
  case GDK_KEY_KP_Add:
    return Key::NumpadAdd;
  case GDK_KEY_KP_Enter:
    return Key::NumpadEnter;
  case GDK_KEY_KP_Equal:
    return Key::NumpadEqual;
  case GDK_KEY_KP_Separator:
    return Key::NumpadComma;
  case GDK_KEY_Clear:
    return Key::NumpadClear;
  default:
    return Key::Unknown;
  }
}

guint UnmodifiedKeyValue(GdkEvent* event, guint fallback) noexcept {
  if (event == nullptr) {
    return fallback;
  }
  guint key_value = 0;
  const gboolean translated = gdk_display_translate_key(
      gdk_event_get_display(event), gdk_key_event_get_keycode(event), static_cast<GdkModifierType>(0),
      gdk_key_event_get_layout(event), &key_value, nullptr, nullptr, nullptr
  );
  return translated ? key_value : fallback;
}

PointerButton TranslatePointerButton(guint button) noexcept {
  switch (button) {
  case GDK_BUTTON_PRIMARY:
    return PointerButton::Primary;
  case GDK_BUTTON_SECONDARY:
    return PointerButton::Secondary;
  case GDK_BUTTON_MIDDLE:
    return PointerButton::Middle;
  case 8:
    return PointerButton::Back;
  case 9:
    return PointerButton::Forward;
  default:
    return PointerButton::None;
  }
}

PointerButton RemovePointerButton(PointerButton buttons, PointerButton button) noexcept {
  return static_cast<PointerButton>(static_cast<std::uint32_t>(buttons) & ~static_cast<std::uint32_t>(button));
}

KeyModifiers TranslateModifiers(GdkModifierType state) noexcept {
  return {
      (state & GDK_SHIFT_MASK) != 0,
      (state & GDK_CONTROL_MASK) != 0,
      (state & GDK_ALT_MASK) != 0,
      (state & GDK_SUPER_MASK) != 0 || (state & GDK_META_MASK) != 0,
  };
}

std::string KeyText(guint key_value, GdkModifierType state) {
  if ((state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK | GDK_META_MASK)) != 0) {
    return {};
  }
  const gunichar character = gdk_keyval_to_unicode(key_value);
  if (character == 0 || !g_unichar_isprint(character)) {
    return {};
  }
  char buffer[7]{};
  const int length = g_unichar_to_utf8(character, buffer);
  return std::string(buffer, static_cast<std::size_t>(length));
}

GdkSurfaceEdge GdkResizeEdge(LinuxResizeEdge edge) noexcept {
  switch (edge) {
  case LinuxResizeEdge::NorthWest: return GDK_SURFACE_EDGE_NORTH_WEST;
  case LinuxResizeEdge::North: return GDK_SURFACE_EDGE_NORTH;
  case LinuxResizeEdge::NorthEast: return GDK_SURFACE_EDGE_NORTH_EAST;
  case LinuxResizeEdge::East: return GDK_SURFACE_EDGE_EAST;
  case LinuxResizeEdge::SouthEast: return GDK_SURFACE_EDGE_SOUTH_EAST;
  case LinuxResizeEdge::South: return GDK_SURFACE_EDGE_SOUTH;
  case LinuxResizeEdge::SouthWest: return GDK_SURFACE_EDGE_SOUTH_WEST;
  case LinuxResizeEdge::West: return GDK_SURFACE_EDGE_WEST;
  }
  return GDK_SURFACE_EDGE_NORTH;
}

const char* LinuxResizeCursorName(LinuxResizeEdge edge) noexcept {
  switch (edge) {
  case LinuxResizeEdge::North:
  case LinuxResizeEdge::South: return "ns-resize";
  case LinuxResizeEdge::East:
  case LinuxResizeEdge::West: return "ew-resize";
  case LinuxResizeEdge::NorthEast:
  case LinuxResizeEdge::SouthWest: return "nesw-resize";
  case LinuxResizeEdge::NorthWest:
  case LinuxResizeEdge::SouthEast: return "nwse-resize";
  }
  return "default";
}

std::shared_ptr<LinuxUiThreadDispatcher> InitializeGtk() {
  if (g_getenv("GTK_A11Y") == nullptr) {
    // The Linux adapter does not yet publish UiWindow semantics through GTK. Avoid exposing a misleading host-only
    // accessibility tree, while preserving an explicit backend selected by the application or its environment.
    static_cast<void>(g_setenv("GTK_A11Y", "none", FALSE));
  }
  if (gtk_init_check() == FALSE) {
    throw std::runtime_error("HuxerUI Linux could not initialize GTK");
  }
  return std::make_shared<LinuxUiThreadDispatcher>();
}

using HuxerUICanvasSnapshot = void (*)(GtkSnapshot* snapshot, int width, int height, gpointer data);
using HuxerUICanvasAllocate = void (*)(GtkWidget* widget, int width, int height, gpointer data);

struct HuxerUICanvas {
  GtkWidget parent_instance;
  HuxerUICanvasSnapshot snapshot = nullptr;
  HuxerUICanvasAllocate allocate = nullptr;
  gpointer data = nullptr;
};

struct HuxerUICanvasClass {
  GtkWidgetClass parent_class;
};

G_DEFINE_TYPE(HuxerUICanvas, huxerui_canvas, GTK_TYPE_WIDGET)

void HuxerUICanvasSnapshotFrame(GtkWidget* widget, GtkSnapshot* snapshot) {
  auto* canvas = reinterpret_cast<HuxerUICanvas*>(widget);
  if (canvas->snapshot != nullptr) {
    canvas->snapshot(snapshot, gtk_widget_get_width(widget), gtk_widget_get_height(widget), canvas->data);
  }
}

void HuxerUICanvasAllocateChildren(GtkWidget* widget, int width, int height, int) {
  auto* canvas = reinterpret_cast<HuxerUICanvas*>(widget);
  if (canvas->allocate != nullptr) {
    canvas->allocate(widget, width, height, canvas->data);
  }
}

void HuxerUICanvasDispose(GObject* object) {
  GtkWidget* widget = GTK_WIDGET(object);
  while (GtkWidget* child = gtk_widget_get_first_child(widget)) {
    gtk_widget_unparent(child);
  }
  G_OBJECT_CLASS(huxerui_canvas_parent_class)->dispose(object);
}

void huxerui_canvas_class_init(HuxerUICanvasClass* canvas_class) {
  GTK_WIDGET_CLASS(canvas_class)->snapshot = HuxerUICanvasSnapshotFrame;
  GTK_WIDGET_CLASS(canvas_class)->size_allocate = HuxerUICanvasAllocateChildren;
  G_OBJECT_CLASS(canvas_class)->dispose = HuxerUICanvasDispose;
}

void huxerui_canvas_init(HuxerUICanvas* canvas) {
  gtk_widget_set_overflow(GTK_WIDGET(canvas), GTK_OVERFLOW_HIDDEN);
}

GtkWidget* CreateHuxerUICanvas(HuxerUICanvasSnapshot snapshot, HuxerUICanvasAllocate allocate, gpointer data) {
  auto* canvas = static_cast<HuxerUICanvas*>(g_object_new(huxerui_canvas_get_type(), nullptr));
  canvas->snapshot = snapshot;
  canvas->allocate = allocate;
  canvas->data = data;
  return GTK_WIDGET(canvas);
}

} // namespace

/// Owns application services and observes GTK toplevel state on the retained main-context dispatcher.
/// Native facilities are prepared before shared startup and remain alive until Runtime::Retire completes.
class LinuxRuntime final : public Runtime, public PlatformClipboard, public PlatformResources {
public:
  explicit LinuxRuntime(std::shared_ptr<LinuxUiThreadDispatcher> dispatcher)
      : Runtime(dispatcher->Bind()), dispatcher_(std::move(dispatcher)) {}
  ~LinuxRuntime() override {
    Retire();
    StopObservingLifecycle();
    dispatcher_->Shutdown();
  }
  bool SupportsMultipleWindows() const noexcept override { return true; }
  std::unique_ptr<UiWindow> CreateWindow(ViewFactory content, const WindowOptions& options) override;

  void ReportFailure(std::exception_ptr failure) {
    if (!failure_) failure_ = std::move(failure);
    RequestShutdown();
  }

  void RethrowFailure() const {
    if (failure_) std::rethrow_exception(failure_);
  }

  /// Reports whether shared shutdown has completed on the GTK application thread.
  /// @return True after OnRuntimeStopped marks the native loop ready to exit.
  bool Stopped() const noexcept { return stopped_; }

  /// Initializes services and begins observing GTK toplevel additions/removals.
  /// @param application Declaration that outlives this Runtime, accessed on the GTK application thread.
  void Start(const Application& application) {
    InitializeApplication(application);
    windows_ = gtk_window_get_toplevels();
    g_signal_connect(windows_, "items-changed", G_CALLBACK(WindowsChanged), this);
    ObserveWindows();
  }

  /// Creates the configured startup windows through OpenWindow, then runs the GTK main loop even without windows
  /// until shutdown. Rethrows the first recorded window failure after the loop.
  int RunApplication(const Application& application) {
    if (application.options.create_initial_window) {
      static_cast<void>(UseApplication().OpenWindow(application.root_factory, application.options.window));
    }
    for (const WindowOptions& extra : application.options.extra_windows) {
      static_cast<void>(UseApplication().OpenWindow(application.root_factory, extra));
    }
    while (!Stopped()) {
      g_main_context_iteration(nullptr, TRUE);
    }
    RethrowFailure();
    return 0;
  }

  /// Aggregates mapped, non-minimized GTK toplevels into application activity on the main thread.
  /// Native GTK windows participate even when they do not contain a HuxerUI attachment.
  void UpdateLifecycleState() {
    auto lifecycle = ApplicationLifecycleState::Background;
    GListModel* windows = gtk_window_get_toplevels();
    for (guint index = 0; index < g_list_model_get_n_items(windows); ++index) {
      auto* window = GTK_WINDOW(g_list_model_get_item(windows, index));
      GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window));
      const bool minimized = surface && GDK_IS_TOPLEVEL(surface) &&
          (gdk_toplevel_get_state(GDK_TOPLEVEL(surface)) & GDK_TOPLEVEL_STATE_MINIMIZED);
      if (gtk_widget_get_mapped(GTK_WIDGET(window)) && !minimized) {
        lifecycle = gtk_window_is_active(window) ? ApplicationLifecycleState::Active : ApplicationLifecycleState::Inactive;
      }
      g_object_unref(window);
      if (lifecycle == ApplicationLifecycleState::Active) break;
    }
    Runtime::UpdateApplicationLifecycleState(lifecycle);
  }
  PlatformClipboard* Clipboard() noexcept override {
    return this;
  }

public:

  PlatformResources* Resources() noexcept override {
    return this;
  }

  std::optional<AppDirectories> CreateAppDirectories() override {
    return CreateLinuxAppDirectories();
  }

  std::shared_ptr<HttpTransport> CreateHttpTransport() override {
    return CreateLinuxHttpTransport();
  }

  std::shared_ptr<SystemTrayTransport> CreateSystemTrayTransport() override {
    return std::make_shared<LinuxSystemTrayTransport>();
  }

  std::optional<ProcessMetrics> QueryProcessMetrics() noexcept override {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
      return std::nullopt;
    }
    std::ifstream statm("/proc/self/statm");
    std::uint64_t total_pages = 0;
    std::uint64_t resident_pages = 0;
    if (!(statm >> total_pages >> resident_pages)) {
      return std::nullopt;
    }
    static_cast<void>(total_pages);
    const long page_size = sysconf(_SC_PAGESIZE);
    const long processor_count = sysconf(_SC_NPROCESSORS_ONLN);
    if (page_size <= 0) {
      return std::nullopt;
    }
    const auto seconds = [](const timeval& value) {
      return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_usec) / 1'000'000.0;
    };
    return ProcessMetrics{
        .cpu_time_seconds = seconds(usage.ru_utime) + seconds(usage.ru_stime),
        .memory_usage_bytes = resident_pages * static_cast<std::uint64_t>(page_size),
        .processor_count = static_cast<std::uint32_t>(std::max(1L, processor_count)),
    };
  }

  ResourceConfiguration Configuration() const override {
    const char* const* languages = g_get_language_names();
    std::string language = languages != nullptr && languages[0] != nullptr ? languages[0] : "en";
    if (const std::size_t dot = language.find('.'); dot != std::string::npos) {
      language.resize(dot);
    }
    std::replace(language.begin(), language.end(), '_', '-');
    return {Locale::FromLanguageTag(std::move(language)), 1.0F};
  }

  std::optional<InputStream> OpenRead(std::string_view package_path) override {
    if (!IsValidResourcePackagePath(package_path)) {
      throw std::logic_error("HuxerUI Linux resource path is invalid");
    }
    const std::filesystem::path path = ResourceRoot() / std::filesystem::path(package_path);
    return OpenPackageFile(path);
  }

  std::optional<std::string> ReadText() override {
    if (gdk_display_get_default() == nullptr || clipboard_read_active_) {
      return std::nullopt;
    }
    GdkDisplay* display = gdk_display_get_default();
    GdkClipboard* clipboard = gdk_display_get_clipboard(display);
    struct ReadState {
      ~ReadState() {
        g_object_unref(cancellable);
        g_main_loop_unref(loop);
      }

      GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
      GCancellable* cancellable = g_cancellable_new();
      std::optional<std::string> value;
      bool finished = false;
      bool timed_out = false;
    };
    auto state = std::make_shared<ReadState>();
    clipboard_read_active_ = true;
    gdk_clipboard_read_text_async(
        clipboard,
        state->cancellable,
        [](GObject* source, GAsyncResult* result, gpointer data) {
          std::unique_ptr<std::shared_ptr<ReadState>> owner(
              static_cast<std::shared_ptr<ReadState>*>(data)
          );
          const std::shared_ptr<ReadState>& read = *owner;
          GError* error = nullptr;
          char* text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, &error);
          if (text != nullptr) {
            if (!read->timed_out) {
              read->value = text;
            }
            g_free(text);
          }
          if (error != nullptr) {
            g_error_free(error);
          }
          read->finished = true;
          g_main_loop_quit(read->loop);
        },
        new std::shared_ptr<ReadState>(state)
    );
    const guint timeout = g_timeout_add_full(
        G_PRIORITY_DEFAULT,
        1000,
        [](gpointer data) -> gboolean {
          auto& read = *static_cast<ReadState*>(data);
          if (!read.finished) {
            read.timed_out = true;
            g_cancellable_cancel(read.cancellable);
            g_main_loop_quit(read.loop);
          }
          return G_SOURCE_REMOVE;
        },
        state.get(),
        nullptr
    );
    g_main_loop_run(state->loop);
    if (!state->timed_out) {
      g_source_remove(timeout);
    }
    clipboard_read_active_ = false;
    return std::move(state->value);
  }

  bool WriteText(std::string_view text) override {
    if (gdk_display_get_default() == nullptr) {
      return false;
    }
    GdkDisplay* display = gdk_display_get_default();
    gdk_clipboard_set_text(gdk_display_get_clipboard(display), std::string(text).c_str());
    return true;
  }

  std::filesystem::path ResourceRoot() const {
    if (const char* override_directory = std::getenv("HUXERUI_RESOURCES_DIR")) {
      return std::filesystem::path(override_directory);
    }
    std::filesystem::path root(ResolveLinuxExecutablePath());
    root.replace_extension(".resources");
    return root;
  }

private:
  /// Rebuilds lifecycle observation when the GTK toplevel model changes.
  /// @param data Original LinuxRuntime supplied when registering the GTK signal.
  static void WindowsChanged(GListModel*, guint, guint, guint, gpointer data) {
    static_cast<LinuxRuntime*>(data)->ObserveWindows();
  }
  /// Adds native surface-state observation when a GTK window is realized.
  /// @param data Original LinuxRuntime supplied when registering the GTK signal.
  static void WindowRealized(GtkWidget*, gpointer data) {
    static_cast<LinuxRuntime*>(data)->ObserveWindows();
  }
  /// Refreshes aggregate lifecycle when a native window is mapped or unmapped.
  /// @param data Original LinuxRuntime supplied when registering the GTK signal.
  static void WindowMappingChanged(GtkWidget*, gpointer data) {
    auto& self = *static_cast<LinuxRuntime*>(data);
    if (self.IsInitialized()) self.UpdateLifecycleState();
  }
  /// Forwards GTK focus/minimize property changes to aggregate lifecycle updates.
  /// @param data Original LinuxRuntime supplied when registering the GTK signal.
  static void WindowStateChanged(GObject*, GParamSpec*, gpointer data) {
    WindowMappingChanged(nullptr, data);
  }
  /// Disconnects window/surface signals and releases their retained GObjects on the GTK application thread.
  void ClearWindowObservers() {
    for (auto* object : observed_windows_) {
      g_signal_handlers_disconnect_by_data(object, this);
      g_object_unref(object);
    }
    observed_windows_.clear();
  }
  /// Rebuilds observers for current GTK toplevels and immediately refreshes aggregate application lifecycle.
  void ObserveWindows() {
    ClearWindowObservers();
    for (guint index = 0; index < g_list_model_get_n_items(windows_); ++index) {
      auto* window = GTK_WINDOW(g_list_model_get_item(windows_, index));
      observed_windows_.push_back(G_OBJECT(window));
      g_signal_connect(window, "realize", G_CALLBACK(WindowRealized), this);
      g_signal_connect(window, "map", G_CALLBACK(WindowMappingChanged), this);
      g_signal_connect(window, "unmap", G_CALLBACK(WindowMappingChanged), this);
      g_signal_connect(window, "notify::is-active", G_CALLBACK(WindowStateChanged), this);
      GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window));
      if (surface && GDK_IS_TOPLEVEL(surface)) {
        observed_windows_.push_back(G_OBJECT(g_object_ref(surface)));
        g_signal_connect(surface, "notify::state", G_CALLBACK(WindowStateChanged), this);
      }
    }
    if (IsInitialized()) UpdateLifecycleState();
  }
  /// Stops toplevel and native surface observation before Runtime retirement releases native services.
  void StopObservingLifecycle() {
    if (windows_) g_signal_handlers_disconnect_by_data(windows_, this);
    windows_ = nullptr;
    ClearWindowObservers();
  }
  void OnRuntimeStopped() override {
    StopObservingLifecycle();
    stopped_ = true;
  }
  std::shared_ptr<LinuxUiThreadDispatcher> dispatcher_;
  GListModel* windows_ = nullptr;
  std::vector<GObject*> observed_windows_;
  bool stopped_ = false;
  bool clipboard_read_active_ = false;
  std::exception_ptr failure_;
};

/// Owns one GTK window and its rendering/input facilities while sharing its application Runtime.
class LinuxUiWindow final : public UiWindow {
public:
  explicit LinuxUiWindow(std::shared_ptr<LinuxUiThreadDispatcher> dispatcher)
      : ui_dispatcher_(std::move(dispatcher)) {}
  ~LinuxUiWindow() override { Retire(); Cleanup(); }

  ResourceConfiguration Configuration() const {
    auto configuration = static_cast<LinuxRuntime&>(ApplicationRuntime()).Configuration();
    if (drawing_area_) configuration.display_scale = static_cast<float>(gtk_widget_get_scale_factor(drawing_area_));
    return configuration;
  }

  void Initialize(LinuxRuntime& runtime, ViewFactory content, const WindowOptions& options) {
    runtime_ = &runtime;
    renderer_.Initialize();
    const Size initial_size = ResolveInitialWindowSize(options);
    CreateWindow(options, initial_size);
    auto configuration = runtime.Resources()->Configuration();
    configuration.display_scale = static_cast<float>(gtk_widget_get_scale_factor(drawing_area_));
    text_input_.SetUiWindow(this);
    platform_views_ = std::make_unique<LinuxPlatformViews>(
        drawing_area_, renderer_, InternalAccess::ApplicationRegistry(runtime), *this);
    platform_views_->SetFocusChangedHandler([this](bool focused) {
      if (focused) {
        text_input_.SetFocus(false);
        key_tracker_.Reset();
      } else if (drawing_area_ && gtk_widget_has_focus(drawing_area_)) {
        text_input_.SetFocus(true);
      }
    });
    file_drop_ = std::make_unique<LinuxFileDrop>(drawing_area_, *this, ui_dispatcher_->Bind());
    running_ = true;
    InitializeWindow(runtime, std::move(content), options, configuration);
    UpdateRuntimeViewport(initial_size);
    gtk_widget_grab_focus(drawing_area_);
    RequestFrameAt(Now());
  }

  void RequestFrameAt(double deadline) override {
    if (const std::optional<double> scheduled =
            frame_state_.Request(deadline, Now(), drawing_area_ != nullptr && running_)) {
      ScheduleFrame(*scheduled);
    }
  }

  double Now() const noexcept override {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
  }

  void SetPointerCursor(PointerCursorKind kind) override {
    // 应用请求的光标先记下来：命中自定义边框的缩放区时，悬停光标要盖过它。
    requested_pointer_cursor_ = kind;
    if (drawing_area_ != nullptr) {
      ApplyPointerCursor();
    }
  }

  FontMetrics Metrics(const Font& font) override {
    return renderer_.Metrics(font);
  }

  TextRunMetrics MeasureRun(
      std::string_view text, const TextStyle& style, const TextShapingOptions& options
  ) override {
    return renderer_.MeasureRun(text, style, options);
  }

  TextLayoutMetrics MeasureText(const huxerui::AttributedText& text, const TextStyle& style, float max_width,
      const TextLayoutOptions& options) override {
    return renderer_.MeasureText(text, style, max_width, options);
  }

  std::unique_ptr<TextLayout> CreateTextLayout(const huxerui::AttributedText& text, const TextStyle& style,
      float max_width, const TextLayoutOptions& options) override {
    return renderer_.CreateTextLayout(text, style, max_width, options);
  }

  PlatformTextInput* TextInput() noexcept override {
    return &text_input_;
  }

  std::shared_ptr<FilePickerTransport> CreateFilePickerTransport() override {
    auto reference = std::make_shared<LinuxWindowReference>(window_);
    return CreateLinuxFilePickerTransport([reference] {
      std::unique_ptr<GtkWindow, decltype(&g_object_unref)> window{
          GTK_WINDOW(g_weak_ref_get(&reference->window)), g_object_unref};
      return X11WindowId(window.get());
    });
  }

  bool CanDiscardHost() const override {
    return window_ != nullptr;
  }

  /// Detaches native facilities while the widget tree is alive, before GTK disposes the window.
  void DiscardHost() override {
    Cleanup();
#if defined(__GLIBC__)
    // A headless application can stay alive indefinitely; return the retired UI's free heap pages now.
    if (g_list_model_get_n_items(gtk_window_get_toplevels()) == 0) {
      static_cast<void>(malloc_trim(0));
    }
#endif
  }

  /// Reports the window origin in logical screen coordinates on X11. Wayland compositors own placement by
  /// protocol, so positions are reported as unavailable there.
  std::optional<Point> WindowScreenOrigin() const override {
#ifdef GDK_WINDOWING_X11
#ifdef HUXERUI_LINUX_X11
    if (window_ == nullptr) {
      return std::nullopt;
    }
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window_));
    if (surface == nullptr || !GDK_IS_X11_SURFACE(surface)) {
      return std::nullopt;
    }
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    Display* const xdisplay = gdk_x11_display_get_xdisplay(gdk_surface_get_display(surface));
    const Window xid = gdk_x11_surface_get_xid(surface);
    Window root = 0;
    Window child = 0;
    int x = 0;
    int y = 0;
    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int border = 0;
    unsigned int depth = 0;
    if (XGetGeometry(xdisplay, xid, &root, &x, &y, &width, &height, &border, &depth) == 0 ||
        XTranslateCoordinates(xdisplay, xid, root, 0, 0, &x, &y, &child) == False) {
      return std::nullopt;
    }
    G_GNUC_END_IGNORE_DEPRECATIONS
    const float scale = static_cast<float>(gdk_surface_get_scale_factor(surface));
    const Point offset = ClientSurfaceOffset();
    return Point{static_cast<float>(x) / scale + offset.x, static_cast<float>(y) / scale + offset.y};
#endif
#endif
    return std::nullopt;
  }

  void ApplyWindowOwnership(UiWindow* owner) override {
    if (window_ == nullptr) {
      return;
    }
    GtkWindow* owner_window = owner != nullptr ? static_cast<LinuxUiWindow*>(owner)->window_ : nullptr;
    gtk_window_set_transient_for(window_, owner_window);
  }

  void ApplyWindowModalState(bool modal, bool blocked) override {
    input_blocked_ = blocked;
    if (window_ == nullptr) {
      return;
    }
    gtk_window_set_modal(window_, modal ? TRUE : FALSE);
  }

  WindowCapabilities NativeWindowCapabilities() const override {
    // Wayland owns focus-on-map, stacking, and placement by protocol, so those operations stay
    // unavailable there; transparency, utility presentation, sizing, and interactive moves work
    // through GDK on both backends.
    const bool x11 = UsingX11SurfaceBackend();
    WindowCapabilities capabilities;
    capabilities.transparent_surface = true;
    capabilities.utility_presentation = true;
    capabilities.passive_display = x11;
    capabilities.topmost = x11;
    capabilities.geometry = x11;
    capabilities.native_move = true;
#if defined(HUXERUI_LINUX_X11) && defined(HUXERUI_LINUX_X11_SHAPE)
    capabilities.input_passthrough = x11;
#endif
    return capabilities;
  }

  WindowRequestResult RequestWindowTopmost(bool topmost) override {
#if defined(GDK_WINDOWING_X11) && defined(HUXERUI_LINUX_X11)
    if (!UsingX11SurfaceBackend()) {
      return WindowRequestResult::Unavailable;
    }
    if (window_ == nullptr) {
      return WindowRequestResult::Rejected;
    }
    topmost_ = topmost;
    if (!gtk_widget_get_mapped(GTK_WIDGET(window_))) {
      // Unmapped windows adopt the preference the next time they map.
      return WindowRequestResult::Submitted;
    }
    X11SurfaceRequest request{window_};
    if (!request) {
      return WindowRequestResult::Rejected;
    }
    SendNetWmStateMessage(request.display, request.window, "_NET_WM_STATE_ABOVE",
                          topmost ? kNetWmStateAdd : kNetWmStateRemove);
    XFlush(request.display);
    return WindowRequestResult::Submitted;
#else
    static_cast<void>(topmost);
    return WindowRequestResult::Unavailable;
#endif
  }

  WindowRequestResult RequestWindowPosition(Point position) override {
#if defined(GDK_WINDOWING_X11) && defined(HUXERUI_LINUX_X11)
    if (!UsingX11SurfaceBackend()) {
      return WindowRequestResult::Unavailable;
    }
    if (window_ == nullptr || gtk_window_is_fullscreen(window_) || !IsInitialized()) {
      return WindowRequestResult::Rejected;
    }
    X11SurfaceRequest request{window_};
    if (!request) {
      return WindowRequestResult::Rejected;
    }
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window_));
    const double scale = gdk_surface_get_scale_factor(surface);
    const Point offset = ClientSurfaceOffset();
    const int x = ResolveWindowPositionCoordinate((static_cast<double>(position.x) - offset.x) * scale,
        std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
    const int y = ResolveWindowPositionCoordinate((static_cast<double>(position.y) - offset.y) * scale,
        std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
    // StaticGravity addresses the client surface rather than the window manager's decoration frame.
    XEvent message{};
    message.xclient.type = ClientMessage;
    message.xclient.window = request.window;
    message.xclient.message_type = XInternAtom(request.display, "_NET_MOVERESIZE_WINDOW", False);
    message.xclient.format = 32;
    message.xclient.data.l[0] = StaticGravity | (1 << 8) | (1 << 9) | (1 << 12);
    message.xclient.data.l[1] = x;
    message.xclient.data.l[2] = y;
    const int submitted = XSendEvent(request.display, DefaultRootWindow(request.display), False,
        SubstructureRedirectMask | SubstructureNotifyMask, &message);
    XFlush(request.display);
    return submitted != 0 ? WindowRequestResult::Submitted : WindowRequestResult::Rejected;
#else
    static_cast<void>(position);
    return WindowRequestResult::Unavailable;
#endif
  }

  WindowRequestResult RequestWindowSize(Size size) override {
    if (window_ == nullptr || !IsInitialized() || gtk_window_is_fullscreen(window_)) {
      return WindowRequestResult::Rejected;
    }
    if (minimum_size_.has_value()) {
      size.width = std::max(size.width, minimum_size_->width);
      size.height = std::max(size.height, minimum_size_->height);
    }
    // GTK owns logical sizes, so the DIP request maps one to one to the widget-space default size;
    // the mapped surface adopts it even while non-resizable, as probe-verified on both backends.
    const int width = ResolveWindowPositionCoordinate(std::max(1.0F, size.width), 1, std::numeric_limits<int>::max());
    const int height = ResolveWindowPositionCoordinate(std::max(1.0F, size.height), 1, std::numeric_limits<int>::max());
    gtk_window_set_default_size(window_, width, height);
    return WindowRequestResult::Submitted;
  }

  WindowRequestResult RequestWindowInputPassthrough(bool passthrough) override {
#if defined(GDK_WINDOWING_X11) && defined(HUXERUI_LINUX_X11_SHAPE)
    if (!UsingX11SurfaceBackend()) {
      return WindowRequestResult::Unavailable;
    }
    if (window_ == nullptr) {
      return WindowRequestResult::Rejected;
    }
    if (passthrough == input_passthrough_) {
      return WindowRequestResult::Submitted;
    }
    X11SurfaceRequest request{window_};
    if (!request || !gtk_widget_get_mapped(GTK_WIDGET(window_))) {
      return WindowRequestResult::Rejected;
    }
    if (passthrough) {
      // An empty input region excludes the whole surface from native hit testing.
      XShapeCombineRectangles(request.display, request.window, ShapeInput, 0, 0, nullptr, 0, ShapeSet, 0);
    } else {
      // Restoring the input region from the bounding shape tracks the current window size.
      XShapeCombineShape(request.display, request.window, ShapeInput, 0, 0, request.window, ShapeBounding, ShapeSet);
    }
    XFlush(request.display);
    input_passthrough_ = passthrough;
    CancelPointer();
    return WindowRequestResult::Submitted;
#else
    static_cast<void>(passthrough);
    return WindowRequestResult::Unavailable;
#endif
  }

  WindowRequestResult RequestWindowMove() override {
    if (!IsInitialized() || input_blocked_ || native_drag_ || window_ == nullptr ||
        gtk_window_is_fullscreen(window_)) {
      return WindowRequestResult::Rejected;
    }
    // Interactive moves need the seat, serial, and surface-relative origin of the active press;
    // GDK forwards them to the compositor or window manager, which then owns the gesture.
    if (pointer_device_ == nullptr || pressed_buttons_ != PointerButton::Primary) {
      return WindowRequestResult::Rejected;
    }
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window_));
    if (surface == nullptr || !GDK_IS_TOPLEVEL(surface)) {
      return WindowRequestResult::Rejected;
    }
    gdk_toplevel_begin_move(GDK_TOPLEVEL(surface), pointer_device_, GDK_BUTTON_PRIMARY,
                            last_pointer_position_.x, last_pointer_position_.y, pointer_press_time_);
    CancelPointer();
    // The compositor consumes the implicit grab, so the matching release never reaches the canvas.
    suppress_pointer_release_ = true;
    return WindowRequestResult::Submitted;
  }

  std::optional<Rect> WindowWorkArea() const override {
#if defined(GDK_WINDOWING_X11) && defined(HUXERUI_LINUX_X11)
    if (window_ == nullptr) return std::nullopt;
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window_));
    if (surface == nullptr || !GDK_IS_X11_SURFACE(surface)) return std::nullopt;
    GdkMonitor* monitor = gdk_display_get_monitor_at_surface(gdk_surface_get_display(surface), surface);
    if (monitor == nullptr) return std::nullopt;
    // GDK resolves the current desktop's reserved areas and returns logical coordinates.
    GdkRectangle work_area{};
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gdk_x11_monitor_get_workarea(monitor, &work_area);
    G_GNUC_END_IGNORE_DEPRECATIONS
    return Rect{static_cast<float>(work_area.x), static_cast<float>(work_area.y),
                static_cast<float>(work_area.width), static_cast<float>(work_area.height)};
#else
    return std::nullopt;
#endif
  }

  bool BeginWindowDrag(std::shared_ptr<WindowDrag> transfer) override {
    if (!IsInitialized() || input_blocked_ || native_drag_ || !pointer_device_ ||
        pressed_buttons_ != PointerButton::Primary) return false;
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window_));
    if (!surface) return false;
    GBytes* bytes = g_bytes_new_static("", 0);
    GdkContentProvider* content = gdk_content_provider_new_for_bytes("application/x-huxerui-window-drag", bytes);
    g_bytes_unref(bytes);
    native_drag_ = gdk_drag_begin(surface, pointer_device_, content, GDK_ACTION_COPY, 0.0, 0.0);
    g_object_unref(content);
    if (!native_drag_) return false;
    native_transfer_ = std::move(transfer);
    g_object_set_data_full(
        G_OBJECT(native_drag_), "huxerui-window-drag", new std::shared_ptr<WindowDrag>(native_transfer_),
        [](gpointer value) { delete static_cast<std::shared_ptr<WindowDrag>*>(value); }
    );
    g_signal_connect(native_drag_, "cancel", G_CALLBACK(NativeDragCanceled), this);
    g_signal_connect(native_drag_, "dnd-finished", G_CALLBACK(NativeDragFinished), this);
    GtkWidget* icon = gtk_drag_icon_get_for_drag(native_drag_);
    GtkWidget* empty = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(empty, 1, 1);
    gtk_drag_icon_set_child(GTK_DRAG_ICON(icon), empty);
    native_transfer_->SetNativeCancel([this] {
      if (native_drag_) g_signal_emit_by_name(native_drag_, "cancel", GDK_DRAG_CANCEL_ERROR);
    });
    // Native capture replaces GTK's widget sequence, as it does for GtkDragSource.
    GListModel* controllers = gtk_widget_observe_controllers(drawing_area_);
    for (guint index = 0; index < g_list_model_get_n_items(controllers); ++index) {
      auto* controller = GTK_EVENT_CONTROLLER(g_list_model_get_item(controllers, index));
      gtk_event_controller_reset(controller);
      g_object_unref(controller);
    }
    g_object_unref(controllers);
    return true;
  }

  void RequestWindowCommand(WindowCommand command) override {
    if (window_ == nullptr || !IsInitialized()) {
      return;
    }
    switch (command) {
    case WindowCommand::Minimize:
      performing_minimize_ = true;
      gtk_window_minimize(window_);
      break;
    case WindowCommand::Maximize:
      gtk_window_maximize(window_);
      break;
    case WindowCommand::Restore:
      gtk_window_unminimize(window_);
      gtk_window_unmaximize(window_);
      break;
    case WindowCommand::ToggleMaximize:
      gtk_window_is_maximized(window_) ? gtk_window_unmaximize(window_) : gtk_window_maximize(window_);
      break;
    case WindowCommand::EnterFullscreen:
      // Leave monitor selection to the current toplevel's placement. Forcing a monitor first moves
      // the restored Xwayland window to that monitor's origin on some window managers.
      gtk_window_fullscreen(window_);
      break;
    case WindowCommand::ExitFullscreen:
      gtk_window_unfullscreen(window_);
      break;
    case WindowCommand::Close:
      performing_close_ = true;
      gtk_window_close(window_);
      break;
    case WindowCommand::Show:
      gtk_widget_set_visible(GTK_WIDGET(window_), TRUE);
      RequestFrameAt(Now());
      break;
    case WindowCommand::Hide:
      gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
      if (frame_state_.PaintPending()) {
        if (const auto deadline = frame_state_.EndPaint(drawing_area_ != nullptr && running_)) {
          ScheduleFrame(*deadline);
        }
      }
      break;
    case WindowCommand::Activate:
      gtk_widget_set_visible(GTK_WIDGET(window_), TRUE);
      gtk_window_present(window_);
      RequestFrameAt(Now());
      break;
    }
  }

  bool DispatchWindowRequest(WindowCommand command) noexcept {
    try {
      return IsInitialized() && UiWindow::HandleWindowRequest(command);
    } catch (...) {
      static_cast<LinuxRuntime&>(ApplicationRuntime()).ReportFailure(std::current_exception());
      return true;
    }
  }

private:

  void EndNativeDrag(bool canceled, bool success) noexcept {
    GdkDrag* drag = std::exchange(native_drag_, nullptr);
    auto transfer = std::move(native_transfer_);
    if (!drag) return;
    pressed_buttons_ = PointerButton::None;
    suppress_pointer_release_ = false;
    g_signal_handlers_disconnect_by_data(drag, this);
    if (transfer) {
      transfer->SetNativeCancel({});
      try {
        transfer->Finish(canceled);
      } catch (...) {
        runtime_->ReportFailure(std::current_exception());
      }
    }
    gdk_drag_drop_done(drag, success);
    g_object_unref(drag);
  }

  static std::shared_ptr<WindowDrag> NativeTransfer(GdkDrop* drop) {
    GdkDrag* drag = gdk_drop_get_drag(drop);
    if (!drag) return {};
    auto* transfer = static_cast<std::shared_ptr<WindowDrag>*>(
        g_object_get_data(G_OBJECT(drag), "huxerui-window-drag")
    );
    return transfer ? *transfer : nullptr;
  }

  static gboolean NativeDragAccept(GtkDropTargetAsync*, GdkDrop* drop, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    const auto transfer = NativeTransfer(drop);
    return self.IsInitialized() && !self.input_blocked_ && transfer && transfer->Active();
  }

  static GdkDragAction NativeDragMoved(GtkDropTargetAsync*, GdkDrop* drop, double x, double y, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    const auto transfer = NativeTransfer(drop);
    try {
      if (self.IsInitialized() && !self.input_blocked_ && transfer &&
          transfer->Move(self, {static_cast<float>(x), static_cast<float>(y)})) return GDK_ACTION_COPY;
    } catch (...) {
      self.runtime_->ReportFailure(std::current_exception());
    }
    return static_cast<GdkDragAction>(0);
  }

  static void NativeDragLeft(GtkDropTargetAsync*, GdkDrop* drop, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    try {
      if (const auto transfer = NativeTransfer(drop)) transfer->Leave(self);
    } catch (...) {
      self.runtime_->ReportFailure(std::current_exception());
    }
  }

  static gboolean NativeDragDropped(GtkDropTargetAsync*, GdkDrop* drop, double x, double y, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    const auto transfer = NativeTransfer(drop);
    try {
      if (!self.IsInitialized() || self.input_blocked_ || !transfer ||
          !transfer->Drop(self, {static_cast<float>(x), static_cast<float>(y)})) return FALSE;
      // Let GTK retire its drop dispatch and release the native grab before acknowledging completion.
      g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer value) -> gboolean {
        gdk_drop_finish(GDK_DROP(value), GDK_ACTION_COPY);
        return G_SOURCE_REMOVE;
      }, g_object_ref(drop), g_object_unref);
      return TRUE;
    } catch (...) {
      self.runtime_->ReportFailure(std::current_exception());
      return FALSE;
    }
  }

  static void NativeDragCanceled(GdkDrag*, GdkDragCancelReason reason, gpointer data) {
    static_cast<LinuxUiWindow*>(data)->EndNativeDrag(reason != GDK_DRAG_CANCEL_NO_TARGET, false);
  }

  static void NativeDragFinished(GdkDrag*, gpointer data) {
    static_cast<LinuxUiWindow*>(data)->EndNativeDrag(false, true);
  }

  void CreateWindow(const WindowOptions& options, Size initial_size) {
    utility_window_ = options.presentation == WindowPresentation::Utility;
    transparent_surface_ = options.transparent_surface;
    passive_display_ = options.show_without_activation;
    topmost_ = options.topmost;
    // Utility presentation is borderless by definition, so custom chrome has nothing to replace.
    custom_chrome_ = !utility_window_ && options.chrome_mode == WindowChromeMode::Custom;
    custom_title_bar_height_ = options.title_bar_height;
    minimum_size_ = options.minimum_size;
    window_ = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(window_, options.title.c_str());
    gtk_window_set_default_size(
        window_,
        std::max(1, static_cast<int>(std::lround(initial_size.width))),
        std::max(1, static_cast<int>(std::lround(initial_size.height)))
    );
    gtk_window_set_decorated(window_, !custom_chrome_ && !utility_window_);
    if (utility_window_) {
      gtk_window_set_resizable(window_, FALSE);
    }
    if (transparent_surface_) {
      ApplyTransparentBackground();
    }
    if (options.initial_position.has_value()) {
      ApplyInitialPosition(*options.initial_position);
    }

    drawing_area_ = CreateHuxerUICanvas(Snapshot, AllocatePlatformViews, this);
    if (options.minimum_size.has_value()) {
      gtk_widget_set_size_request(
          GTK_WIDGET(drawing_area_),
          std::max(1, static_cast<int>(std::ceil(options.minimum_size->width))),
          std::max(1, static_cast<int>(std::ceil(options.minimum_size->height)))
      );
    }
    gtk_widget_set_focusable(GTK_WIDGET(drawing_area_), TRUE);
    gtk_window_set_child(window_, GTK_WIDGET(drawing_area_));
    g_signal_connect(drawing_area_, "destroy", G_CALLBACK(ClientWidgetDestroyed), this);
    text_input_.SetClientWidget(GTK_WIDGET(drawing_area_));

    g_signal_connect(window_, "close-request", G_CALLBACK(CloseRequested), this);
    g_signal_connect(window_, "destroy", G_CALLBACK(Destroyed), this);
    g_signal_connect(window_, "realize", G_CALLBACK(WindowRealizedForChrome), this);
    g_signal_connect(window_, "unrealize", G_CALLBACK(WindowUnrealized), this);
    g_signal_connect(window_, "map", G_CALLBACK(WindowMapped), this);
    g_signal_connect(window_, "unmap", G_CALLBACK(WindowUnmapped), this);
    g_signal_connect(window_, "notify::is-active", G_CALLBACK(WindowActiveChanged), this);
    g_signal_connect(window_, "notify::maximized", G_CALLBACK(WindowMaximizedChanged), this);
    g_signal_connect(window_, "notify::fullscreened", G_CALLBACK(WindowMaximizedChanged), this);
    g_signal_connect(drawing_area_, "notify::scale-factor", G_CALLBACK(ScaleChanged), this);

    GtkEventController* pointer = gtk_event_controller_legacy_new();
    gtk_event_controller_set_propagation_phase(pointer, GTK_PHASE_CAPTURE);
    g_signal_connect(pointer, "event", G_CALLBACK(PointerEventReceived), this);
    gtk_widget_add_controller(GTK_WIDGET(drawing_area_), pointer);

    GtkGesture* touch = gtk_gesture_click_new();
    gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(touch), TRUE);
    g_signal_connect(touch, "pressed", G_CALLBACK(TouchPressed), this);
    g_signal_connect(touch, "released", G_CALLBACK(TouchReleased), this);
    g_signal_connect(touch, "cancel", G_CALLBACK(PointerCanceled), this);
    gtk_widget_add_controller(GTK_WIDGET(drawing_area_), GTK_EVENT_CONTROLLER(touch));

    GtkEventController* motion = gtk_event_controller_motion_new();
    gtk_event_controller_set_propagation_phase(motion, GTK_PHASE_CAPTURE);
    g_signal_connect(motion, "enter", G_CALLBACK(PointerEntered), this);
    g_signal_connect(motion, "motion", G_CALLBACK(PointerMoved), this);
    g_signal_connect(motion, "leave", G_CALLBACK(PointerLeft), this);
    gtk_widget_add_controller(GTK_WIDGET(drawing_area_), motion);

    const char* drag_formats[] = {"application/x-huxerui-window-drag"};
    GtkDropTargetAsync* drop = gtk_drop_target_async_new(gdk_content_formats_new(drag_formats, 1), GDK_ACTION_COPY);
    g_signal_connect(drop, "accept", G_CALLBACK(NativeDragAccept), this);
    g_signal_connect(drop, "drag-enter", G_CALLBACK(NativeDragMoved), this);
    g_signal_connect(drop, "drag-motion", G_CALLBACK(NativeDragMoved), this);
    g_signal_connect(drop, "drag-leave", G_CALLBACK(NativeDragLeft), this);
    g_signal_connect(drop, "drop", G_CALLBACK(NativeDragDropped), this);
    gtk_widget_add_controller(GTK_WIDGET(drawing_area_), GTK_EVENT_CONTROLLER(drop));

    GtkEventController* scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    gtk_event_controller_set_propagation_phase(scroll, GTK_PHASE_CAPTURE);
    g_signal_connect(scroll, "scroll-begin", G_CALLBACK(ScrollBegan), this);
    g_signal_connect(scroll, "scroll", G_CALLBACK(Scrolled), this);
    g_signal_connect(scroll, "scroll-end", G_CALLBACK(ScrollEnded), this);
    gtk_widget_add_controller(GTK_WIDGET(drawing_area_), scroll);

    GtkEventController* key = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key, GTK_PHASE_CAPTURE);
    g_signal_connect(key, "key-pressed", G_CALLBACK(KeyPressed), this);
    g_signal_connect(key, "key-released", G_CALLBACK(KeyReleased), this);
    gtk_widget_add_controller(GTK_WIDGET(drawing_area_), key);

    GtkEventController* focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "enter", G_CALLBACK(FocusEntered), this);
    g_signal_connect(focus, "leave", G_CALLBACK(FocusLeft), this);
    gtk_widget_add_controller(GTK_WIDGET(drawing_area_), focus);
  }

  bool UsingX11SurfaceBackend() const noexcept {
#if defined(GDK_WINDOWING_X11) && defined(HUXERUI_LINUX_X11)
    return window_ != nullptr && GDK_IS_X11_DISPLAY(gtk_widget_get_display(GTK_WIDGET(window_)));
#else
    return false;
#endif
  }

  /// Locates the canvas origin within the native surface, excluding GTK decorations and shadows.
  Point ClientSurfaceOffset() const {
    const graphene_point_t zero{0.0F, 0.0F};
    graphene_point_t origin{};
    if (!gtk_widget_compute_point(drawing_area_, GTK_WIDGET(window_), &zero, &origin)) return {};
    double x = 0.0;
    double y = 0.0;
    gtk_native_get_surface_transform(GTK_NATIVE(window_), &x, &y);
    return Point{origin.x - static_cast<float>(x), origin.y - static_cast<float>(y)};
  }

  /// Points the theme's window background at per-pixel alpha for this window only.
  void ApplyTransparentBackground() {
    EnsureTransparentBackgroundProvider(gtk_widget_get_display(GTK_WIDGET(window_)));
    gtk_widget_add_css_class(GTK_WIDGET(window_), "huxerui-transparent");
  }

  /// Applies creation-time X11 protocol state that must exist before the first map: the utility
  /// window type and the passive input hint GTK rewrites after mapping.
  void ApplyX11InitialChrome() {
#if defined(GDK_WINDOWING_X11) && defined(HUXERUI_LINUX_X11)
    X11SurfaceRequest request{window_};
    if (!request) {
      return;
    }
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    if (utility_window_) {
      const Atom utility_type = XInternAtom(request.display, "_NET_WM_WINDOW_TYPE_UTILITY", False);
      XChangeProperty(request.display, request.window, XInternAtom(request.display, "_NET_WM_WINDOW_TYPE", False),
          XA_ATOM, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(&utility_type), 1);
    }
    if (passive_display_) {
      XWMHints hints;
      hints.flags = InputHint;
      hints.input = False;
      XSetWMHints(request.display, request.window, &hints);
    }
    G_GNUC_END_IGNORE_DEPRECATIONS
#endif
  }

  /// Applies post-map X11 state: taskbar and pager exclusion, the initial topmost preference, and
  /// the passive input hint again because GTK restores its own hints while mapping.
  void ApplyFloatingWindowMapState() {
#if defined(GDK_WINDOWING_X11) && defined(HUXERUI_LINUX_X11)
    X11SurfaceRequest request{window_};
    if (!request) {
      return;
    }
    if (utility_window_) {
      SendNetWmStateMessage(request.display, request.window, "_NET_WM_STATE_SKIP_TASKBAR", kNetWmStateAdd);
      SendNetWmStateMessage(request.display, request.window, "_NET_WM_STATE_SKIP_PAGER", kNetWmStateAdd);
    }
    if (topmost_) {
      SendNetWmStateMessage(request.display, request.window, "_NET_WM_STATE_ABOVE", kNetWmStateAdd);
    }
    if (passive_display_) {
      G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      XWMHints hints;
      hints.flags = InputHint;
      hints.input = False;
      XSetWMHints(request.display, request.window, &hints);
      G_GNUC_END_IGNORE_DEPRECATIONS
    }
    XFlush(request.display);
#endif
  }

  void ScheduleFrame(double deadline) {
    if (frame_source_ != 0) {
      g_source_remove(frame_source_);
      frame_source_ = 0;
    }
    const double milliseconds = std::ceil(std::max(0.0, deadline - Now()) * 1000.0);
    const guint delay = static_cast<guint>(
        std::clamp(milliseconds, 0.0, static_cast<double>(std::numeric_limits<guint>::max()))
    );
    frame_source_ = delay == 0
                        ? g_idle_add_full(G_PRIORITY_HIGH_IDLE, FrameReady, this, nullptr)
                        : g_timeout_add_full(G_PRIORITY_HIGH, std::max(1U, delay), FrameReady, this, nullptr);
  }

  void CommitFrame() {
    ExecutionGuard guard(InternalAccess::WindowExecution(*this));
    frame_source_ = 0;
    // A ready GLib frame source can run after a close request destroys the drawing widget.
    if (!IsInitialized() || drawing_area_ == nullptr || !running_ ||
        !GTK_IS_WIDGET(drawing_area_) || !frame_state_.BeginCommit()) {
      return;
    }
    try {
      const FrameCommit& commit = UiWindow::BuildFrame();
      if (drawing_area_ == nullptr || !running_ || !GTK_IS_WIDGET(drawing_area_)) {
        return;
      }
      committed_frame_ = &commit.render_frame;
      if (platform_views_) {
        platform_views_->Commit(*committed_frame_);
      }
      if (gtk_widget_is_visible(GTK_WIDGET(drawing_area_))) {
        frame_state_.MarkPaintPending();
        gtk_widget_queue_draw(GTK_WIDGET(drawing_area_));
      }
      if (commit.next_frame_deadline.has_value()) {
        RequestFrameAt(*commit.next_frame_deadline);
      }
    } catch (...) {
      runtime_->ReportFailure(std::current_exception());
    }
    FlushDeferredFrame();
  }

  void FlushDeferredFrame() {
    if (const std::optional<double> deadline = frame_state_.TakeDeferred(drawing_area_ != nullptr && running_)) {
      ScheduleFrame(*deadline);
    }
  }

  void DrawFrame(GtkSnapshot* snapshot) {
    if (!IsInitialized() || committed_frame_ == nullptr) {
      return;
    }
    frame_state_.BeginPaint();
    try {
      renderer_.SetDeviceScale(static_cast<float>(gtk_widget_get_scale_factor(drawing_area_)));
      renderer_.SetDisplay(gtk_widget_get_display(drawing_area_));
      renderer_.SetViewport({static_cast<float>(gtk_widget_get_width(drawing_area_)),
                             static_cast<float>(gtk_widget_get_height(drawing_area_))});
      if (platform_views_) {
        platform_views_->Snapshot(snapshot, *committed_frame_);
      } else {
        renderer_.Snapshot(snapshot, *committed_frame_);
      }
    } catch (...) {
      runtime_->ReportFailure(std::current_exception());
    }
    if (const std::optional<double> deadline = frame_state_.EndPaint(drawing_area_ != nullptr && running_)) {
      ScheduleFrame(*deadline);
    }
  }

  void UpdateRuntimeViewport(Size viewport) {
    if (!IsInitialized() || viewport.width <= 0.0F || viewport.height <= 0.0F) {
      return;
    }
    WindowMetrics metrics{.viewport = viewport, .safe_area = {}, .title_bar = std::nullopt};
    if (custom_chrome_ && window_ != nullptr && !gtk_window_is_fullscreen(window_)) {
      metrics.title_bar = ResolveLinuxTitleBarMetrics(
          custom_title_bar_height_, viewport, window_ != nullptr && gtk_window_is_maximized(window_)
      );
    }
    UiWindow::SetWindowMetrics(metrics);
  }

  void UpdateRuntimeViewport() {
    if (drawing_area_ == nullptr) {
      return;
    }
    UpdateRuntimeViewport({
        static_cast<float>(gtk_widget_get_width(GTK_WIDGET(drawing_area_))),
        static_cast<float>(gtk_widget_get_height(GTK_WIDGET(drawing_area_))),
    });
  }

  void AttachToplevelState() {
    if (window_ == nullptr) {
      return;
    }
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window_));
    GdkToplevel* toplevel = surface != nullptr && GDK_IS_TOPLEVEL(surface) ? GDK_TOPLEVEL(surface) : nullptr;
    if (toplevel == toplevel_) {
      return;
    }
    DetachToplevelState();
    toplevel_ = toplevel;
    if (toplevel_ != nullptr) {
      minimized_ = (gdk_toplevel_get_state(toplevel_) & GDK_TOPLEVEL_STATE_MINIMIZED) != 0;
      toplevel_state_handler_ =
          g_signal_connect(toplevel_, "notify::state", G_CALLBACK(ToplevelStateChanged), this);
    }
  }

  void DetachToplevelState() noexcept {
    if (toplevel_ != nullptr && toplevel_state_handler_ != 0) {
      g_signal_handler_disconnect(toplevel_, toplevel_state_handler_);
    }
    toplevel_ = nullptr;
    toplevel_state_handler_ = 0;
    minimized_ = false;
  }

  void UpdateLifecycleState() {
    if (!IsInitialized() || window_ == nullptr) {
      return;
    }
    const bool mapped = gtk_widget_get_mapped(GTK_WIDGET(window_)) != FALSE;
    const bool active = gtk_window_is_active(window_) != FALSE;
    const bool minimized = toplevel_ != nullptr &&
                           (gdk_toplevel_get_state(toplevel_) & GDK_TOPLEVEL_STATE_MINIMIZED) != 0;
    UiWindow::UpdateWindowLifecycleState(!mapped || minimized ? WindowLifecycleState::Background
        : active ? WindowLifecycleState::Active : WindowLifecycleState::Inactive);
    static_cast<LinuxRuntime&>(ApplicationRuntime()).UpdateLifecycleState();
  }

  bool BeginWindowOperation(GdkEvent* event, guint button, Point point) {
    if (!custom_chrome_ || window_ == nullptr || gtk_window_is_fullscreen(window_) || !IsInitialized() || event == nullptr) {
      return false;
    }
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window_));
    if (surface == nullptr || !GDK_IS_TOPLEVEL(surface)) {
      return false;
    }
    GdkDevice* device = gdk_event_get_device(event);
    if (device == nullptr) {
      return false;
    }
    const guint32 time = gdk_event_get_time(event);
    const Size viewport{
        static_cast<float>(gtk_widget_get_width(GTK_WIDGET(drawing_area_))),
        static_cast<float>(gtk_widget_get_height(GTK_WIDGET(drawing_area_))),
    };
    if (const std::optional<LinuxResizeEdge> edge = ResolveLinuxResizeEdge(
            point, viewport, gtk_window_is_maximized(window_) != FALSE)) {
      gdk_toplevel_begin_resize(GDK_TOPLEVEL(surface), GdkResizeEdge(*edge), device, static_cast<int>(button), point.x, point.y, time);
      return true;
    }
    if (UiWindow::IsWindowDragRegion(point)) {
      gdk_toplevel_begin_move(GDK_TOPLEVEL(surface), device, static_cast<int>(button), point.x, point.y, time);
      return true;
    }
    return false;
  }

  bool RoutePointerToHuxerUI(Point position, PointerEventType type) {
    return !platform_views_ ||
           platform_views_->RouteToHuxerUI(position, type, pressed_buttons_ != PointerButton::None);
  }

  void SendPointer(PointerEventType type, Point position, PointerButton changed_button = PointerButton::None,
      PointerButton pressed_buttons = PointerButton::None, KeyModifiers modifiers = {}) {
    if (native_drag_ && type != PointerEventType::Cancel) return;
    if (!IsInitialized()) {
      return;
    }
    last_pointer_position_ = position;
    UiWindow::HandlePointerEvent({
        type,
        0,
        position,
        PointerDeviceKind::Mouse,
        changed_button,
        pressed_buttons,
        modifiers,
    });
    UpdateResizeCursor(position);
  }

  void ApplyPointerCursor() noexcept {
    if (drawing_area_ == nullptr) return;
    const char* name = hovered_resize_edge_.has_value()
                           ? LinuxResizeCursorName(*hovered_resize_edge_)
                           : LinuxPointerCursorName(requested_pointer_cursor_);
    gtk_widget_set_cursor_from_name(GTK_WIDGET(drawing_area_), name);
  }

  void UpdateResizeCursor(Point position) noexcept {
    std::optional<LinuxResizeEdge> edge;
    if (custom_chrome_ && drawing_area_ != nullptr && window_ != nullptr && !gtk_window_is_fullscreen(window_) &&
        pressed_buttons_ == PointerButton::None) {
      edge = ResolveLinuxResizeEdge(
          position,
          {static_cast<float>(gtk_widget_get_width(GTK_WIDGET(drawing_area_))),
           static_cast<float>(gtk_widget_get_height(GTK_WIDGET(drawing_area_)))},
          gtk_window_is_maximized(window_) != FALSE
      );
    }
    if (edge != hovered_resize_edge_) {
      hovered_resize_edge_ = edge;
      ApplyPointerCursor();
    }
  }

  void CancelPointer() {
    suppress_pointer_release_ = false;
    if (pressed_buttons_ == PointerButton::None) {
      return;
    }
    pressed_buttons_ = PointerButton::None;
    SendPointer(PointerEventType::Cancel, last_pointer_position_);
  }

  bool SendKey(KeyEventType type, guint key_value, GdkModifierType state, GdkEvent* event, bool repeat = false) {
    if (!IsInitialized()) {
      return false;
    }
    return UiWindow::HandleKeyEvent({
        type,
        TranslateKey(UnmodifiedKeyValue(event, key_value)),
        type == KeyEventType::Down ? KeyText(key_value, state) : std::string{},
        TranslateModifiers(state),
        repeat,
    });
  }

  void DetachCanvasCallbacks() noexcept {
    if (drawing_area_ == nullptr) return;
    auto* canvas = reinterpret_cast<HuxerUICanvas*>(drawing_area_);
    canvas->snapshot = nullptr;
    canvas->allocate = nullptr;
    canvas->data = nullptr;
    g_signal_handlers_disconnect_by_data(drawing_area_, this);
    GListModel* controllers = gtk_widget_observe_controllers(drawing_area_);
    for (guint index = 0; index < g_list_model_get_n_items(controllers); ++index) {
      GObject* controller = G_OBJECT(g_list_model_get_item(controllers, index));
      g_signal_handlers_disconnect_by_data(controller, this);
      g_object_unref(controller);
    }
    g_object_unref(controllers);
  }



  void Cleanup() noexcept {
    Retire();
    // Native facilities must release while the widget tree is still alive: the IM client and platform
    // views detach through live widgets, so a post-destroy release would touch finalized objects.
    ReleaseNativeFacilities();
    DetachCanvasCallbacks();
    if (window_ != nullptr) {
      GtkWindow* window = window_;
      g_signal_handlers_disconnect_by_data(window, this);
      window_ = nullptr;
      drawing_area_ = nullptr;
      gtk_window_destroy(window);
    }
  }

  /// Releases the per-window native subsystems while the widget tree can still serve detach calls.
  /// Runs at most once; later callers (including the deferred "destroy" emission) are no-ops.
  void ReleaseNativeFacilities() noexcept {
    if (native_facilities_released_) {
      return;
    }
    native_facilities_released_ = true;
    running_ = false;
    EndNativeDrag(true, false);
    g_clear_object(&pointer_device_);
    if (platform_views_) {
      platform_views_->Shutdown();
      platform_views_.reset();
    }
    file_drop_.reset();
    if (frame_source_ != 0) {
      g_source_remove(frame_source_);
      frame_source_ = 0;
    }

    text_input_.Reset();
    DetachToplevelState();
    key_tracker_.Reset();
    committed_frame_ = nullptr;
    renderer_.Discard();
  }

  static gboolean FrameReady(gpointer data) {
    static_cast<LinuxUiWindow*>(data)->CommitFrame();
    return G_SOURCE_REMOVE;
  }

  static void Snapshot(GtkSnapshot* snapshot, int width, int height, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    self.UpdateRuntimeViewport({static_cast<float>(width), static_cast<float>(height)});
    self.DrawFrame(snapshot);
  }

  static void AllocatePlatformViews(GtkWidget*, int width, int height, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    if (self.platform_views_) {
      self.platform_views_->Allocate(width, height);
    }
  }

  /// Stashes the preferred initial position on X11; GTK4 has no placement API, so the request moves the
  /// client surface through the window manager once it maps. Wayland ignores client positions by protocol.
  void ApplyInitialPosition(const Point& position) {
#ifdef GDK_WINDOWING_X11
#ifdef HUXERUI_LINUX_X11
    GdkDisplay* display = gdk_display_get_default();
    if (display == nullptr || !GDK_IS_X11_DISPLAY(display)) {
      return;
    }
    pending_initial_position_ = position;
#else
    static_cast<void>(position);
#endif
#else
    static_cast<void>(position);
#endif
  }

  /// Applies a pending initial position request after the native surface exists.
  void ApplyPendingInitialPosition() {
#ifdef GDK_WINDOWING_X11
#ifdef HUXERUI_LINUX_X11
    if (!pending_initial_position_.has_value() || window_ == nullptr) {
      return;
    }
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window_));
    if (surface == nullptr || !GDK_IS_X11_SURFACE(surface)) {
      return;
    }
    const Point position = *pending_initial_position_;
    pending_initial_position_.reset();
    static_cast<void>(RequestWindowPosition(position));
#endif
#endif
  }

  static gboolean CloseRequested(GtkWindow*, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    if (!self.performing_close_ && self.DispatchWindowRequest(WindowCommand::Close)) {
      return TRUE;
    }
    self.performing_close_ = false;
    if (self.IsInitialized()) self.CloseWindow();
    return TRUE;
  }

  static void ClientWidgetDestroyed(GtkWidget* widget, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    self.text_input_.Reset();
    if (self.drawing_area_ != nullptr && GTK_WIDGET(self.drawing_area_) == widget) {
      self.DetachCanvasCallbacks();
      self.drawing_area_ = nullptr;
    }
  }

  /// Runs when GTK finally disposes the native window. Every shutdown decision already happened in
  /// CloseRequested or RetireWindow, so this only releases window resources idempotently.
  static void Destroyed(GtkWidget*, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    self.CancelPointer();
    self.key_tracker_.Reset();
    if (self.IsInitialized()) self.CloseWindow();
    self.ReleaseNativeFacilities();
    self.DetachCanvasCallbacks();
    if (self.window_) g_signal_handlers_disconnect_by_data(self.window_, &self);
    self.window_ = nullptr;
    self.drawing_area_ = nullptr;
  }

  static void WindowMapped(GtkWidget*, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    self.text_input_.SetClientWidget(self.drawing_area_);
    self.text_input_.SetFocus(gtk_widget_has_focus(self.drawing_area_));
    self.AttachToplevelState();
    self.ApplyPendingInitialPosition();
    self.ApplyFloatingWindowMapState();
    self.UpdateLifecycleState();
  }

  static void WindowRealizedForChrome(GtkWidget*, gpointer data) {
    static_cast<LinuxUiWindow*>(data)->ApplyX11InitialChrome();
  }

  static void WindowUnrealized(GtkWidget*, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    // GTK's destroy emission follows native disposal; detach IM delegates before unrealize destroys them.
    self.text_input_.SetFocus(false);
    self.text_input_.SetClientWidget(nullptr);
    self.DetachToplevelState();
  }

  static void WindowUnmapped(GtkWidget*, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    self.CancelPointer();
    self.key_tracker_.Reset();
    self.UpdateLifecycleState();
  }

  static void WindowActiveChanged(GObject*, GParamSpec*, gpointer data) {
    static_cast<LinuxUiWindow*>(data)->UpdateLifecycleState();
  }

  static void ToplevelStateChanged(GObject*, GParamSpec*, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    const bool minimized = self.toplevel_ != nullptr &&
                           (gdk_toplevel_get_state(self.toplevel_) & GDK_TOPLEVEL_STATE_MINIMIZED) != 0;
    if (minimized && !self.minimized_) {
      if (self.performing_minimize_) {
        self.performing_minimize_ = false;
      } else if (self.DispatchWindowRequest(WindowCommand::Minimize) && self.IsInitialized()) {
        gtk_window_unminimize(self.window_);
      }
    }
    self.minimized_ = minimized;
    self.UpdateLifecycleState();
    self.UpdateRuntimeViewport();
  }

  static void WindowMaximizedChanged(GObject*, GParamSpec*, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    self.UpdateRuntimeViewport();
    self.UpdateResizeCursor(self.last_pointer_position_);
  }

  static void ScaleChanged(GObject*, GParamSpec*, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    if (self.IsInitialized()) {
      self.UiWindow::UpdateResourceConfiguration(self.Configuration());
      self.UpdateRuntimeViewport();
      self.RequestFrameAt(self.Now());
    }
  }

  void PressPointer(GdkEvent* event, guint platform_button, Point position) {
    suppress_pointer_release_ = false;
    const PointerButton button = TranslatePointerButton(platform_button);
    if (button == PointerButton::None) {
      return;
    }
    if (button == PointerButton::Primary && BeginWindowOperation(event, platform_button, position)) {
      suppress_pointer_release_ = true;
      return;
    }
    pressed_buttons_ |= button;
    if (event) {
      g_set_object(&pointer_device_, gdk_event_get_device(event));
      pointer_press_time_ = gdk_event_get_time(event);
    }
    SendPointer(PointerEventType::Down, position, button, pressed_buttons_,
        event ? TranslateModifiers(gdk_event_get_modifier_state(event)) : KeyModifiers{});
  }

  void ReleasePointer(GdkEvent* event, guint platform_button, Point position) {
    if (suppress_pointer_release_) {
      suppress_pointer_release_ = false;
      return;
    }
    if (pressed_buttons_ == PointerButton::None) {
      return;
    }
    const PointerButton button = TranslatePointerButton(platform_button);
    pressed_buttons_ = RemovePointerButton(pressed_buttons_, button);
    SendPointer(PointerEventType::Up, position, button, pressed_buttons_,
        event ? TranslateModifiers(gdk_event_get_modifier_state(event)) : KeyModifiers{});
  }

  static gboolean PointerEventReceived(GtkEventControllerLegacy*, GdkEvent* event, gpointer data) {
    const GdkEventType type = gdk_event_get_event_type(event);
    if ((type != GDK_BUTTON_PRESS && type != GDK_BUTTON_RELEASE) ||
        gdk_event_get_pointer_emulated(event)) {
      return FALSE;
    }
    double x = 0.0;
    double y = 0.0;
    if (!gdk_event_get_position(event, &x, &y)) {
      return FALSE;
    }
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    if (self.window_ != nullptr) {
      double offset_x = 0.0;
      double offset_y = 0.0;
      gtk_native_get_surface_transform(GTK_NATIVE(self.window_), &offset_x, &offset_y);
      x += offset_x;
      y += offset_y;
    }
    const Point position{static_cast<float>(x), static_cast<float>(y)};
    const guint platform_button = gdk_button_event_get_button(event);
    const PointerButton button = TranslatePointerButton(platform_button);
    if (button == PointerButton::None) {
      return FALSE;
    }
    if (type == GDK_BUTTON_PRESS) {
      if (!self.RoutePointerToHuxerUI(position, PointerEventType::Down)) {
        return FALSE;
      }
      self.PressPointer(event, platform_button, position);
    } else {
      if (!self.RoutePointerToHuxerUI(position, PointerEventType::Up)) {
        return FALSE;
      }
      self.ReleasePointer(event, platform_button, position);
    }
    return FALSE;
  }

  static void TouchPressed(GtkGestureClick* gesture, int, double x, double y, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
    const guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    const Point position{static_cast<float>(x), static_cast<float>(y)};
    if (!self.RoutePointerToHuxerUI(position, PointerEventType::Down)) {
      static_cast<void>(gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_DENIED));
      return;
    }
    self.PressPointer(event, button, position);
  }

  static void TouchReleased(GtkGestureClick* gesture, int, double x, double y, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    const guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
    const Point position{static_cast<float>(x), static_cast<float>(y)};
    if (self.RoutePointerToHuxerUI(position, PointerEventType::Up)) {
      self.ReleasePointer(event, button, position);
    }
  }

  static void PointerCanceled(GtkGesture*, GdkEventSequence*, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    if (self.native_drag_) return;
    if (self.RoutePointerToHuxerUI(self.last_pointer_position_, PointerEventType::Cancel)) {
      self.CancelPointer();
    }
  }

  static void PointerEntered(GtkEventControllerMotion* controller, double x, double y, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    const Point position{static_cast<float>(x), static_cast<float>(y)};
    if (!self.RoutePointerToHuxerUI(position, PointerEventType::Move)) {
      return;
    }
    self.SendPointer(PointerEventType::Move, position, PointerButton::None,
        self.pressed_buttons_,
        TranslateModifiers(gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller))));
  }

  static void PointerMoved(GtkEventControllerMotion* controller, double x, double y, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    const Point position{static_cast<float>(x), static_cast<float>(y)};
    if (!self.RoutePointerToHuxerUI(position, PointerEventType::Move)) {
      return;
    }
    self.SendPointer(PointerEventType::Move, position, PointerButton::None,
        self.pressed_buttons_,
        TranslateModifiers(gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller))));
  }

  static void PointerLeft(GtkEventControllerMotion*, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    if (self.pressed_buttons_ == PointerButton::None) {
      self.SendPointer(PointerEventType::Cancel, self.last_pointer_position_);
    }
    self.hovered_resize_edge_.reset();
    self.ApplyPointerCursor();
  }

  static bool IsTouchpadScroll(GtkEventControllerScroll* controller) {
    GdkDevice* device = gtk_event_controller_get_current_event_device(GTK_EVENT_CONTROLLER(controller));
    return device != nullptr && gdk_device_get_source(device) == GDK_SOURCE_TOUCHPAD;
  }

  static void ScrollBegan(GtkEventControllerScroll* controller, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    self.touchpad_scroll_active_ = self.IsInitialized() && IsTouchpadScroll(controller);
    if (self.touchpad_scroll_active_) {
      static_cast<void>(self.UiWindow::HandleScrollInput({
          self.last_pointer_position_, 0.0F, 0.0F, {}, ScrollInputKind::Touchpad, ScrollInputPhase::Begin,
      }));
    }
  }

  static void ScrollEnded(GtkEventControllerScroll*, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    if (self.touchpad_scroll_active_ && self.IsInitialized()) {
      static_cast<void>(self.UiWindow::HandleScrollInput({
          self.last_pointer_position_, 0.0F, 0.0F, {}, ScrollInputKind::Touchpad, ScrollInputPhase::End,
      }));
    }
    self.touchpad_scroll_active_ = false;
  }

  static gboolean Scrolled(GtkEventControllerScroll* controller, double dx, double dy, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    if (self.IsInitialized()) {
      if (!self.RoutePointerToHuxerUI(self.last_pointer_position_, PointerEventType::Move)) {
        return FALSE;
      }
      GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
      const GdkModifierType state = event ? gdk_event_get_modifier_state(event) : GdkModifierType{};
      const bool touchpad = IsTouchpadScroll(controller) ||
                            (event != nullptr && gdk_scroll_event_get_unit(event) == GDK_SCROLL_UNIT_SURFACE);
      const float scale = gtk_event_controller_scroll_get_unit(controller) == GDK_SCROLL_UNIT_WHEEL
                              ? kDipsPerScrollStep : 1.0F;
      const Point consumed = self.UiWindow::HandleScrollInput({
          self.last_pointer_position_,
          static_cast<float>(dx) * scale,
          static_cast<float>(dy) * scale,
          TranslateModifiers(state),
          touchpad ? ScrollInputKind::Touchpad : ScrollInputKind::Wheel,
          ScrollInputPhase::Update,
      });
      self.touchpad_scroll_active_ = touchpad;
      return consumed.x != 0.0F || consumed.y != 0.0F;
    }
    return FALSE;
  }

  static gboolean KeyPressed(
      GtkEventControllerKey* controller, guint key_value, guint key_code, GdkModifierType state, gpointer data
  ) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    if (self.platform_views_ && self.platform_views_->HasPlatformFocus()) {
      return FALSE;
    }
    GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
    const LinuxKeyPressResult press =
        self.key_tracker_.Press(key_code, self.text_input_.FilterKeyEvent(event));
    if (!press.dispatch) {
      return TRUE;
    }
    return self.SendKey(KeyEventType::Down, key_value, state, event, press.repeat);
  }

  static void KeyReleased(
      GtkEventControllerKey* controller, guint key_value, guint key_code, GdkModifierType state, gpointer data
  ) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    if (self.platform_views_ && self.platform_views_->HasPlatformFocus()) {
      return;
    }
    GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
    if (self.key_tracker_.Release(key_code, self.text_input_.FilterKeyEvent(event))) {
      static_cast<void>(self.SendKey(KeyEventType::Up, key_value, state, event));
    }
  }

  static void FocusEntered(GtkEventControllerFocus*, gpointer data) {
    static_cast<LinuxUiWindow*>(data)->text_input_.SetFocus(true);
  }

  static void FocusLeft(GtkEventControllerFocus*, gpointer data) {
    auto& self = *static_cast<LinuxUiWindow*>(data);
    ExecutionGuard guard(InternalAccess::WindowExecution(self));
    self.text_input_.SetFocus(false);
    self.key_tracker_.Reset();
  }

  std::shared_ptr<LinuxUiThreadDispatcher> ui_dispatcher_;

  GtkWindow* window_ = nullptr;
  GtkWidget* drawing_area_ = nullptr;
  bool touchpad_scroll_active_ = false;
  GdkToplevel* toplevel_ = nullptr;
  gulong toplevel_state_handler_ = 0;
  LinuxRenderer renderer_;
  LinuxTextInput text_input_;
  std::unique_ptr<LinuxPlatformViews> platform_views_;
  std::unique_ptr<LinuxFileDrop> file_drop_;
  GdkDevice* pointer_device_ = nullptr;
  GdkDrag* native_drag_ = nullptr;
  std::shared_ptr<WindowDrag> native_transfer_;
  PlatformFrameState frame_state_;
  const RenderFrame* committed_frame_ = nullptr;
  guint frame_source_ = 0;
  bool running_ = false;
  LinuxRuntime* runtime_ = nullptr;
  bool input_blocked_ = false;
  bool minimized_ = false;
  bool native_facilities_released_ = false;
  std::optional<Point> pending_initial_position_;
  bool performing_minimize_ = false;
  bool performing_close_ = false;
  bool custom_chrome_ = false;
  bool utility_window_ = false;
  bool transparent_surface_ = false;
  bool passive_display_ = false;
  bool topmost_ = false;
  bool input_passthrough_ = false;
  std::optional<Size> minimum_size_;
  guint32 pointer_press_time_ = 0;
  float custom_title_bar_height_ = 0.0F;
  PointerButton pressed_buttons_ = PointerButton::None;
  bool suppress_pointer_release_ = false;
  Point last_pointer_position_;
  PointerCursorKind requested_pointer_cursor_ = PointerCursorKind::Default;
  std::optional<LinuxResizeEdge> hovered_resize_edge_;
  LinuxKeyTracker key_tracker_;
};

std::unique_ptr<UiWindow> LinuxRuntime::CreateWindow(ViewFactory content, const WindowOptions& options) {
  auto window = std::make_unique<LinuxUiWindow>(dispatcher_);
  window->Initialize(*this, std::move(content), options);
  return window;
}

int RunPlatformApplication(const Application& application) {
  LinuxRuntime runtime(InitializeGtk());
  runtime.Start(application);
  return runtime.RunApplication(application);
}

} // namespace huxerui::detail
