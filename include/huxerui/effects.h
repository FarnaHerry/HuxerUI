#pragma once

#include <huxerui/geometry.h>

namespace huxerui {

namespace detail {
struct ModifierDescriptor;
}

/// Fast permits a renderer approximation; unsupported configurations retain precise processing.
enum class BackdropBlurQuality {
  Precise,
  Fast,
};

/// Immutable optical parameters applied to the already-painted prefix of the active compositing group.
///
/// Distances use logical pixels. Processing uses premultiplied sRGB colors; capture edges extend their nearest pixel.
/// Blur supports axis-aligned nonuniform scaling and uniform orthogonal transforms. Other affine transforms require
/// zero blur; refractive sampling and shape geometry still follow the complete affine transform.
/// This effect does not change layout, input, child content, or the window's native background.
struct BackdropEffect {
  /// Finite, non-negative blur radius: Precise support or Fast strength; zero preserves background detail.
  float blur_radius = 16.0F;
  /// Finite, non-negative edge displacement producing a lens-like refraction.
  float refraction = 0.0F;
  /// Positive thickness of the optical edge band in logical pixels.
  float edge_width = 12.0F;
  /// Strength of directional edge lighting, in the inclusive range [0, 1].
  float highlight = 0.0F;
  /// Non-zero direction toward the light, normalized during processing.
  Point light_direction{-0.5F, -1.0F};
  /// Non-negative saturation multiplier, where one preserves captured saturation.
  float saturation = 1.0F;
  /// Non-negative brightness multiplier, where one preserves captured brightness.
  float brightness = 1.0F;
  /// Non-negative smoothing radius for a union of glass shapes; zero creates a geometric union.
  float fusion_radius = 0.0F;
  /// Opt-in blur approximation. Geometry, clipping, optics and foreground remain at capture resolution.
  BackdropBlurQuality blur_quality = BackdropBlurQuality::Precise;

  bool operator==(const BackdropEffect&) const = default;
};

/// One rounded rectangle in a glass surface; a command may combine several shapes into one optical surface.
struct BackdropShape {
  Rect rect;
  CornerRadii corner_radii;

  bool operator==(const BackdropShape&) const = default;
};

/// Blurs the background behind a View, before its own shadow, fill, border, and content.
struct BackdropBlur {
  static const detail::ModifierDescriptor& Descriptor();
  float radius = 16.0F;

  bool operator==(const BackdropBlur&) const = default;
};

/// Applies an immutable optical material to a View's background before its shadow, fill, and content.
struct BackdropFilter {
  static const detail::ModifierDescriptor& Descriptor();
  BackdropEffect effect;

  bool operator==(const BackdropFilter&) const = default;
};

/// Adds retained optical press and hover feedback without transforming children or changing controlled values.
///
/// Compatible recomposition preserves motion. Disabled input resolves to the resting material; reduced motion resolves
/// immediately. Apply transparent fills separately with Background or the component's Theme style.
struct LiquidGlass {
  static const detail::ModifierDescriptor& Descriptor();
  BackdropEffect effect{.refraction = 6.0F, .highlight = 0.2F};

  bool operator==(const LiquidGlass&) const = default;
};

/// Draws one fused optical material behind a container's direct children while preserving each child's identity.
///
/// Child geometry must map to axis-aligned rounded rectangles in the container's local space. Rotated, reflected, and
/// skewed child shapes are rejected. Child fills should be transparent when the shared material is intended to show.
struct GlassGroup {
  static const detail::ModifierDescriptor& Descriptor();
  BackdropEffect effect{.refraction = 6.0F, .highlight = 0.2F, .fusion_radius = 16.0F};

  bool operator==(const GlassGroup&) const = default;
};

} // namespace huxerui
