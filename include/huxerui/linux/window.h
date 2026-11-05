#pragma once

#include <optional>

#include <huxerui/color.h>
#include <huxerui/geometry.h>

typedef struct _GdkSurface GdkSurface;

namespace huxerui {
class UiWindow;

namespace linux {

/// Borrowed surface and canvas origin in surface-local logical coordinates.
/// Use only on the original application thread; the surface is invalid after native retirement or unrealization.
struct WindowGeometry {
  GdkSurface* surface = nullptr;
  double content_x = 0.0;
  double content_y = 0.0;
  double scale = 1.0;
};

/// Reads the original GTK attachment; an unavailable or retired attachment returns empty geometry.
[[nodiscard]] WindowGeometry NativeGeometry(UiWindow& window);

/// Omits a rectangular canvas region from native drawing and makes the window background transparent.
/// Coordinates are canvas-local logical units. An empty value restores drawing and the original background policy.
/// This controls drawing only; native input and X11 window shapes remain the extension's responsibility.
/// Call on the original application thread. Returns false for an unavailable or retired attachment.
/// Nonempty rectangles must be finite with positive dimensions, otherwise std::invalid_argument is thrown.
bool SetWindowAperture(UiWindow& window, std::optional<Rect> region);

inline constexpr Color MarkerColor = Color::Rgb(1, 160, 253);

/// Paints an opaque locator in MarkerColor above canvas content, without starting a frame timer.
/// Coordinates, validation, thread and lifetime rules match SetWindowAperture. An empty value removes the locator.
bool SetWindowMarker(UiWindow& window, std::optional<Rect> region);

} // namespace linux
} // namespace huxerui
