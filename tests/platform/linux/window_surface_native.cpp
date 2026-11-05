#include "linux_internal.h"

#include <gtk/gtk.h>
#include <catch2/catch_amalgamated.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

#include <huxerui/huxerui.h>
#include <huxerui/linux/window.h>

namespace huxerui::test {
namespace {

struct SurfaceScenario {
  UiWindow* window = nullptr;
  std::optional<ApplicationHandle> application;
  std::exception_ptr failure;
  bool completed = false;
  int stage = 0;
  int settle = 0;
  std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

  static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
  }

  static std::uint32_t Pixel(GtkWidget* canvas, int x, int y) {
    GdkPaintable* paintable = gtk_widget_paintable_new(canvas);
    GtkSnapshot* snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot),
        gtk_widget_get_width(canvas), gtk_widget_get_height(canvas));
    g_object_unref(paintable);
    GskRenderNode* node = gtk_snapshot_free_to_node(snapshot);
    Check(node != nullptr, "native canvas did not produce a snapshot");
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 200, 160);
    cairo_t* context = cairo_create(surface);
    gsk_render_node_draw(node, context);
    cairo_destroy(context);
    gsk_render_node_unref(node);
    cairo_surface_flush(surface);
    const auto* row = cairo_image_surface_get_data(surface) + y * cairo_image_surface_get_stride(surface);
    const std::uint32_t pixel = reinterpret_cast<const std::uint32_t*>(row)[x];
    cairo_surface_destroy(surface);
    return pixel;
  }

  bool Advance() {
    Check(std::chrono::steady_clock::now() < deadline, "native canvas surface test timed out");
    if (!window || !application) return false;
    const auto geometry = linux::NativeGeometry(*window);
    if (!geometry.surface) return false;
    GtkNative* native = gtk_native_get_for_surface(geometry.surface);
    GtkWidget* canvas = gtk_window_get_child(GTK_WINDOW(native));
    if (!canvas || !gtk_widget_get_mapped(canvas) || ++settle < 5) return false;
    settle = 0;
    Check(geometry.scale > 0.0, "native surface did not report its scale");
    if (stage == 0) {
      Check((Pixel(canvas, 30, 30) >> 24U) == 255, "initial canvas was not opaque");
      Check(linux::SetWindowAperture(*window, Rect{20, 20, 40, 40}), "aperture request was rejected");
      Check(linux::SetWindowMarker(*window, Rect{80, 20, 30, 10}), "locator request was rejected");
      bool rejected = false;
      try {
        linux::SetWindowAperture(*window, Rect{0, 0, std::numeric_limits<float>::infinity(), 10});
      } catch (const std::invalid_argument&) {
        rejected = true;
      }
      Check(rejected, "nonfinite aperture dimensions were accepted");
    } else if (stage == 1) {
      Check(gtk_widget_has_css_class(GTK_WIDGET(native), "huxerui-transparent"), "window background stayed opaque");
      Check((Pixel(canvas, 30, 30) >> 24U) == 0, "aperture did not omit the selected canvas pixels");
      Check((Pixel(canvas, 5, 5) >> 24U) == 255, "aperture removed pixels outside its rectangle");
      Check(Pixel(canvas, 85, 25) == 0xFF01A0FDU, "locator did not use the documented opaque color");
      Check(linux::SetWindowAperture(*window, {}), "aperture restoration was rejected");
      Check(linux::SetWindowMarker(*window, {}), "locator removal was rejected");
    } else {
      Check(!gtk_widget_has_css_class(GTK_WIDGET(native), "huxerui-transparent"), "original background was not restored");
      Check((Pixel(canvas, 30, 30) >> 24U) == 255, "canvas did not repaint after aperture removal");
      Check(Pixel(canvas, 85, 25) == 0xFFCC3311U, "locator pixels survived removal");
      return true;
    }
    ++stage;
    return false;
  }
};

SurfaceScenario* surface_scenario = nullptr;

View SurfaceProbeContent() {
  Lifecycle([] {
    surface_scenario->window = OpenPlatformModule<UiWindow*>("test/native-surface");
    surface_scenario->application = UseApplication();
    return [] {};
  });
  return Canvas([](PaintContext& paint, Size size) {
    paint.DrawRect({0, 0, size.width, size.height}, Color::Rgb(204, 51, 17));
  });
}

} // namespace

TEST_CASE("LinuxNativeCanvasApertureAndLocatorRestoreDrawing", "[.][linux-native][window-surface]") {
  SurfaceScenario scenario;
  surface_scenario = &scenario;
  const guint timer = g_timeout_add(20, [](gpointer data) -> gboolean {
    auto& state = *static_cast<SurfaceScenario*>(data);
    try {
      if (!state.Advance()) return G_SOURCE_CONTINUE;
      state.completed = true;
    } catch (...) {
      state.failure = std::current_exception();
    }
    if (state.application) state.application->Quit();
    return G_SOURCE_REMOVE;
  }, &scenario);
  const Application application{
      SurfaceProbeContent,
      {
          .window = {.title = "HuxerUI canvas surface probe", .initial_size = {200, 160}},
          .show_debug_overlay = false,
          .application_hooks = {[](ApplicationContext& context) {
            context.RegisterPlatformModule<UiWindow*>("test/native-surface", [](UiWindow& window) { return &window; });
          }},
      },
  };
  try {
    static_cast<void>(detail::RunPlatformApplication(application));
  } catch (...) {
    if (!scenario.failure) scenario.failure = std::current_exception();
  }
  if (g_main_context_find_source_by_id(nullptr, timer)) g_source_remove(timer);
  surface_scenario = nullptr;
  if (scenario.failure) std::rethrow_exception(scenario.failure);
  REQUIRE(scenario.completed);
}

} // namespace huxerui::test
