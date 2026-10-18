#include "linux_internal.h"
#include "internal_access.h"

#include "linux_renderer.h"

#include <gtk/gtk.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "graphics/external_texture_internal.h"
#include "graphics/backdrop_plane_internal.h"
#include "graphics/effects_internal.h"
#include "graphics/geometry_internal.h"
#include "linux_external_texture_internal.h"
#include "linux_gl_effects_internal.h"
#include "graphics/path_internal.h"
#include "graphics/paint_internal.h"
#include "resources/resource_internal.h"
#include "graphics/shadow_internal.h"
#include "text/text_internal.h"

namespace huxerui::detail {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTau = 2.0 * kPi;

float PangoUnits(int value) noexcept {
  return static_cast<float>(value) / static_cast<float>(PANGO_SCALE);
}

PangoDirection PangoTextDirection(TextDirection direction) noexcept {
  switch (direction) {
  case TextDirection::LeftToRight:
    return PANGO_DIRECTION_LTR;
  case TextDirection::RightToLeft:
    return PANGO_DIRECTION_RTL;
  case TextDirection::Auto:
    return PANGO_DIRECTION_NEUTRAL;
  }
  return PANGO_DIRECTION_NEUTRAL;
}

PangoFontDescription* CreateFontDescription(const Font& font) {
  PangoFontDescription* description = pango_font_description_new();
  switch (font.FamilyKind()) {
  case FontFamilyKind::System:
    pango_font_description_set_family(description, "sans-serif");
    break;
  case FontFamilyKind::Monospace:
    pango_font_description_set_family(description, "monospace");
    break;
  case FontFamilyKind::Named:
    pango_font_description_set_family(description, std::string(font.FamilyName()).c_str());
    break;
  }
  pango_font_description_set_absolute_size(
      description,
      std::max(0.0F, font.Size()) * static_cast<float>(PANGO_SCALE)
  );
  pango_font_description_set_weight(description, static_cast<PangoWeight>(font.Weight()));
  pango_font_description_set_style(
      description,
      font.Slant() == FontSlant::Italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL
  );
  return description;
}

PangoAttrList* CreateTextAttributes(const TextStyle& style, const TextShapingOptions& shaping) {
  PangoAttrList* attributes = pango_attr_list_new();
  if (HasTextDecoration(style.decoration, TextDecoration::Underline)) {
    pango_attr_list_insert(attributes, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
  }
  if (HasTextDecoration(style.decoration, TextDecoration::StrikeThrough)) {
    pango_attr_list_insert(attributes, pango_attr_strikethrough_new(TRUE));
  }
  if (!shaping.locale.empty()) {
    pango_attr_list_insert(attributes, pango_attr_language_new(pango_language_from_string(shaping.locale.c_str())));
  }
  return attributes;
}

void ConfigureLayout(
    PangoLayout* layout,
    std::string_view text,
    const TextStyle& style,
    float max_width,
    const TextLayoutOptions& options
) {
  if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument("HuxerUI Linux text is too large");
  }
  pango_layout_set_text(layout, text.data(), static_cast<int>(text.size()));
  PangoFontDescription* description = CreateFontDescription(style.font);
  pango_layout_set_font_description(layout, description);
  pango_font_description_free(description);
  PangoAttrList* attributes = CreateTextAttributes(style, options.shaping);
  pango_layout_set_attributes(layout, attributes);
  pango_attr_list_unref(attributes);

  const PangoDirection direction = PangoTextDirection(options.shaping.direction);
  pango_layout_set_auto_dir(layout, direction == PANGO_DIRECTION_NEUTRAL);
  PangoContext* context = pango_layout_get_context(layout);
  pango_context_set_base_dir(context, direction == PANGO_DIRECTION_NEUTRAL ? PANGO_DIRECTION_LTR : direction);

  const bool constrained = std::isfinite(max_width) && max_width > 0.0F;
  if (constrained && options.wrap == TextWrap::Word) {
    pango_layout_set_width(layout, static_cast<int>(std::ceil(max_width * PANGO_SCALE)));
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
  } else {
    pango_layout_set_width(layout, -1);
    pango_layout_set_single_paragraph_mode(layout, FALSE);
  }

  const bool rtl = direction == PANGO_DIRECTION_RTL;
  switch (options.align) {
  case TextAlign::Leading:
    pango_layout_set_alignment(layout, rtl ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
    break;
  case TextAlign::Center:
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    break;
  case TextAlign::Trailing:
    pango_layout_set_alignment(layout, rtl ? PANGO_ALIGN_LEFT : PANGO_ALIGN_RIGHT);
    break;
  }
}

void ConfigureLayout(PangoLayout* layout, const AttributedText& text, const TextStyle& style, float max_width,
    const TextLayoutOptions& options) {
  ConfigureLayout(layout, text.PlainText(), style, max_width, options);
  // Copy the paragraph attributes before adding resolved overrides; range indices below are UTF-8 byte offsets.
  PangoAttrList* attributes = pango_attr_list_copy(pango_layout_get_attributes(layout));
  for (const auto& run : ResolveTextRuns(text, style)) {
    const auto insert = [&](PangoAttribute* attribute) {
      attribute->start_index = static_cast<guint>(run.byte_start);
      attribute->end_index = static_cast<guint>(run.byte_end);
      pango_attr_list_change(attributes, attribute);
    };
    PangoFontDescription* font = CreateFontDescription(run.style.font);
    insert(pango_attr_font_desc_new(font));
    pango_font_description_free(font);
    insert(pango_attr_underline_new(
        HasTextDecoration(run.style.decoration, TextDecoration::Underline) ? PANGO_UNDERLINE_SINGLE
                                                                         : PANGO_UNDERLINE_NONE
    ));
    insert(pango_attr_strikethrough_new(HasTextDecoration(run.style.decoration, TextDecoration::StrikeThrough)));
    const auto channel = [](float value) { return static_cast<guint16>(std::clamp(value, 0.0F, 1.0F) * 65535.0F); };
    const Color foreground = run.style.foreground;
    insert(pango_attr_foreground_new(channel(foreground.red), channel(foreground.green), channel(foreground.blue)));
    insert(pango_attr_foreground_alpha_new(channel(foreground.alpha)));
    if (run.background.alpha > 0.0F) {
      const Color background = run.background;
      insert(pango_attr_background_new(channel(background.red), channel(background.green), channel(background.blue)));
      insert(pango_attr_background_alpha_new(channel(background.alpha)));
    }
  }
  pango_layout_set_attributes(layout, attributes);
  pango_attr_list_unref(attributes);
}

std::optional<TextOffset> Utf8ByteToUtf16(std::string_view text, int byte_offset) noexcept {
  if (byte_offset < 0 || static_cast<std::size_t>(byte_offset) > text.size() ||
      !g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) {
    return std::nullopt;
  }
  TextOffset result = 0;
  const char* current = text.data();
  const char* end = text.data() + byte_offset;
  while (current < end) {
    const gunichar code_point = g_utf8_get_char(current);
    current = g_utf8_next_char(current);
    if (current > end) {
      return std::nullopt;
    }
    result += code_point > 0xFFFFU ? 2 : 1;
  }
  return result;
}

std::optional<int> Utf16ToUtf8Byte(std::string_view text, TextOffset offset) noexcept {
  if (offset < 0 || !g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) {
    return std::nullopt;
  }
  TextOffset utf16 = 0;
  const char* current = text.data();
  const char* end = text.data() + text.size();
  while (current < end && utf16 < offset) {
    const gunichar code_point = g_utf8_get_char(current);
    const TextOffset units = code_point > 0xFFFFU ? 2 : 1;
    if (utf16 + units > offset) {
      return std::nullopt;
    }
    utf16 += units;
    current = g_utf8_next_char(current);
  }
  if (utf16 != offset) {
    return std::nullopt;
  }
  return static_cast<int>(current - text.data());
}

TextLayoutMetrics LayoutMetrics(PangoLayout* layout) {
  PangoRectangle logical{};
  pango_layout_get_extents(layout, nullptr, &logical);
  TextLayoutMetrics result{
      .size = {std::ceil(PangoUnits(logical.width)), std::ceil(PangoUnits(logical.height))},
      .line_count = static_cast<std::size_t>(std::max(1, pango_layout_get_line_count(layout))),
  };
  PangoLayoutIter* iterator = pango_layout_get_iter(layout);
  if (iterator != nullptr) {
    result.first_baseline = PangoUnits(pango_layout_iter_get_baseline(iterator));
    result.last_baseline = result.first_baseline;
    while (pango_layout_iter_next_line(iterator)) {
      result.last_baseline = PangoUnits(pango_layout_iter_get_baseline(iterator));
    }
    pango_layout_iter_free(iterator);
  }
  return result;
}

class PangoTextLayout final : public TextLayout {
public:
  PangoTextLayout(PangoContext* context, const AttributedText& text, const TextStyle& style, float max_width,
      const TextLayoutOptions& options)
      : text_(text.PlainText()) {
    PangoFontMap* font_map = pango_context_get_font_map(context);
    // Retained interaction geometry must not inherit later direction or locale changes on the renderer's context.
    PangoContext* layout_context = pango_font_map_create_context(font_map);
    pango_context_set_language(layout_context, pango_context_get_language(context));
    pango_cairo_context_set_resolution(layout_context, pango_cairo_context_get_resolution(context));
    layout_ = pango_layout_new(layout_context);
    g_object_unref(layout_context);
    ConfigureLayout(layout_, text, style, max_width, options);
    metrics_ = LayoutMetrics(layout_);
    if (options.wrap == TextWrap::NoWrap && std::isfinite(max_width) && metrics_.size.width < max_width) {
      pango_layout_set_width(layout_, static_cast<int>(std::ceil(max_width * PANGO_SCALE)));
    }
    BuildCaretOffsets();
  }

  ~PangoTextLayout() override {
    g_object_unref(layout_);
  }

  Size Measure() const override {
    return metrics_.size;
  }

  const TextLayoutMetrics& MetricsValue() const noexcept {
    return metrics_;
  }

  TextPosition HitTest(Point point) const override {
    int byte_index = 0;
    int trailing = 0;
    pango_layout_xy_to_index(layout_, static_cast<int>(std::lround(point.x * PANGO_SCALE)),
        static_cast<int>(std::lround(point.y * PANGO_SCALE)), &byte_index, &trailing);
    int line_index = 0;
    int ignored_x = 0;
    pango_layout_index_to_line_x(layout_, byte_index, FALSE, &line_index, &ignored_x);
    const char* current = text_.data() + std::clamp(byte_index, 0, static_cast<int>(text_.size()));
    const char* end = text_.data() + text_.size();
    // Pango's trailing count is in Unicode characters, whereas the shared selection boundary uses UTF-16 units.
    while (trailing-- > 0 && current < end) {
      current = g_utf8_next_char(current);
    }
    const TextOffset offset = Utf8ByteToUtf16(text_, static_cast<int>(current - text_.data())).value_or(0);
    const auto distance = [&](const Rect& caret) {
      return point.y >= caret.y && point.y < caret.y + caret.height
          ? std::abs(point.x - caret.x) : std::numeric_limits<float>::infinity();
    };
    // Pango's inside/outside result is not affinity; choose between visual caret candidates on the hit line.
    const float before = distance(CaretRect(offset, TextAffinity::Upstream));
    const float after = distance(CaretRect(offset, TextAffinity::Downstream));
    PangoLayoutLine* line = pango_layout_get_line_readonly(layout_, line_index);
    const int line_end = line->start_index + line->length;
    // A hit at a soft line end may name its last character rather than the boundary after it.
    if (line_end > line->start_index && line_end < static_cast<int>(text_.size()) &&
        text_[line_end - 1] != '\n' && text_[line_end - 1] != '\r') {
      const TextOffset boundary = Utf8ByteToUtf16(text_, line_end).value_or(offset);
      const float boundary_distance = distance(CaretRect(boundary, TextAffinity::Upstream));
      if (std::isfinite(boundary_distance) && boundary_distance <= std::min(before, after)) {
        return {boundary, TextAffinity::Upstream};
      }
    }
    return {offset, before < after ? TextAffinity::Upstream : TextAffinity::Downstream};
  }

  Rect CaretRect(TextOffset offset, TextAffinity affinity) const override {
    offset = std::clamp<TextOffset>(offset, 0, caret_offsets_.back());
    const int requested_byte = Utf16ToUtf8Byte(text_, offset).value_or(static_cast<int>(text_.size()));
    int byte_index = requested_byte;
    bool trailing = affinity == TextAffinity::Upstream && offset > 0;
    if (trailing) {
      const int previous = Utf16ToUtf8Byte(text_, PreviousCaretOffset(offset)).value_or(0);
      // An explicit separator advances to the next line regardless of affinity.
      trailing = text_[previous] != '\n' && text_[previous] != '\r';
      if (trailing) {
        byte_index = previous;
      }
    }
    int line_index = 0;
    int x = 0;
    pango_layout_index_to_line_x(layout_, byte_index, trailing, &line_index, &x);
    PangoLayoutLine* line = pango_layout_get_line_readonly(layout_, line_index);
    // Pango strong/weak positions share a line; a soft-wrap upstream caret needs the preceding line's edge.
    if (!trailing || requested_byte != line->start_index + line->length ||
        line_index + 1 == pango_layout_get_line_count(layout_)) {
      PangoRectangle strong{};
      PangoRectangle weak{};
      pango_layout_get_cursor_pos(layout_, requested_byte, &strong, &weak);
      const PangoRectangle& selected = trailing ? weak : strong;
      return {PangoUnits(selected.x), PangoUnits(selected.y), 1.0F, std::ceil(PangoUnits(selected.height))};
    }
    PangoLayoutIter* iterator = pango_layout_get_iter(layout_);
    for (int index = 0; index < line_index; ++index) {
      pango_layout_iter_next_line(iterator);
    }
    PangoRectangle logical{};
    pango_layout_iter_get_line_extents(iterator, nullptr, &logical);
    pango_layout_iter_free(iterator);
    return {
        PangoUnits(logical.x + x),
        PangoUnits(logical.y),
        1.0F,
        std::ceil(PangoUnits(logical.height)),
    };
  }

  std::vector<Rect> RangeRects(TextRange range) const override {
    const int start = Utf16ToUtf8Byte(text_, std::max<TextOffset>(0, range.start)).value_or(0);
    const int end = Utf16ToUtf8Byte(text_, std::max(range.start, range.end)).value_or(static_cast<int>(text_.size()));
    if (start >= end) {
      return {};
    }
    std::vector<Rect> result;
    PangoLayoutIter* iterator = pango_layout_get_iter(layout_);
    if (iterator == nullptr) {
      return result;
    }
    do {
      PangoLayoutLine* line = pango_layout_iter_get_line_readonly(iterator);
      if (line == nullptr) {
        continue;
      }
      const int line_start = line->start_index;
      const int line_end = line_start + line->length;
      const int selected_start = std::max(start, line_start);
      const int selected_end = std::min(end, line_end);
      if (selected_start >= selected_end) {
        continue;
      }
      int* ranges = nullptr;
      int range_count = 0;
      pango_layout_line_get_x_ranges(line, selected_start, selected_end, &ranges, &range_count);
      PangoRectangle logical{};
      pango_layout_iter_get_line_extents(iterator, nullptr, &logical);
      std::vector<Rect> line_rects;
      line_rects.reserve(static_cast<std::size_t>(range_count));
      // Pango's x ranges already include line alignment; adding logical.x again would shift the selection twice.
      for (int range_index = 0; range_index < range_count; ++range_index) {
        const int first_x = ranges[range_index * 2];
        const int last_x = ranges[range_index * 2 + 1];
        line_rects.push_back({
            PangoUnits(std::min(first_x, last_x)),
            PangoUnits(logical.y),
            PangoUnits(std::abs(last_x - first_x)),
            std::ceil(PangoUnits(logical.height)),
        });
      }
      g_free(ranges);
      std::ranges::sort(line_rects, {}, &Rect::x);
      for (const Rect& rect : line_rects) {
        if (!result.empty() && result.back().y == rect.y &&
            rect.x <= result.back().x + result.back().width + 1.0F / PANGO_SCALE) {
          result.back().width = std::max(result.back().x + result.back().width, rect.x + rect.width) - result.back().x;
        } else {
          result.push_back(rect);
        }
      }
    } while (pango_layout_iter_next_line(iterator));
    pango_layout_iter_free(iterator);
    return result;
  }

  TextOffset PreviousCaretOffset(TextOffset offset) const override {
    const auto iterator = std::lower_bound(caret_offsets_.begin(), caret_offsets_.end(), offset);
    return iterator == caret_offsets_.begin() ? 0 : *std::prev(iterator);
  }

  TextOffset NextCaretOffset(TextOffset offset) const override {
    const auto iterator = std::upper_bound(caret_offsets_.begin(), caret_offsets_.end(), offset);
    return iterator == caret_offsets_.end() ? caret_offsets_.back() : *iterator;
  }

private:
  void BuildCaretOffsets() {
    caret_offsets_.push_back(0);
    int attribute_count = 0;
    const PangoLogAttr* attributes = pango_layout_get_log_attrs_readonly(layout_, &attribute_count);
    const char* current = text_.data();
    const char* end = text_.data() + text_.size();
    // Walk scalar positions once while accumulating UTF-16 offsets; visual style splits are not extra caret stops.
    TextOffset offset = 0;
    for (int index = 1; index < attribute_count && current < end; ++index) {
      offset += g_utf8_get_char(current) > 0xFFFFU ? 2 : 1;
      current = g_utf8_next_char(current);
      if (attributes[index].is_cursor_position != 0) {
        caret_offsets_.push_back(offset);
      }
    }
    const TextOffset length = Utf8ByteToUtf16(text_, static_cast<int>(text_.size())).value_or(0);
    if (caret_offsets_.back() != length) {
      caret_offsets_.push_back(length);
    }
  }

  std::string text_;
  PangoLayout* layout_ = nullptr;
  TextLayoutMetrics metrics_;
  std::vector<TextOffset> caret_offsets_;
};

void SetSourceColor(cairo_t* context, const Color& color) {
  cairo_set_source_rgba(context, color.red, color.green, color.blue, color.alpha);
}

cairo_line_cap_t CairoLineCap(StrokeCap cap) noexcept {
  switch (cap) {
  case StrokeCap::Butt:
    return CAIRO_LINE_CAP_BUTT;
  case StrokeCap::Round:
    return CAIRO_LINE_CAP_ROUND;
  case StrokeCap::Square:
    return CAIRO_LINE_CAP_SQUARE;
  }
  return CAIRO_LINE_CAP_BUTT;
}

cairo_line_join_t CairoLineJoin(StrokeJoin join) noexcept {
  switch (join) {
  case StrokeJoin::Miter:
    return CAIRO_LINE_JOIN_MITER;
  case StrokeJoin::Round:
    return CAIRO_LINE_JOIN_ROUND;
  case StrokeJoin::Bevel:
    return CAIRO_LINE_JOIN_BEVEL;
  }
  return CAIRO_LINE_JOIN_MITER;
}

void ApplyStrokeStyle(cairo_t* context, const StrokeStyle& style) {
  std::vector<double> dashes;
  dashes.reserve(style.dash_pattern.size());
  for (const float length : style.dash_pattern) {
    dashes.push_back(length);
  }
  cairo_set_line_width(context, style.width);
  cairo_set_line_cap(context, CairoLineCap(style.cap));
  cairo_set_line_join(context, CairoLineJoin(style.join));
  cairo_set_miter_limit(context, style.miter_limit);
  cairo_set_dash(context, dashes.empty() ? nullptr : dashes.data(), static_cast<int>(dashes.size()), style.dash_offset);
}

void AddRoundedRect(cairo_t* context, Rect rect, float corner_radius) {
  const float radius = std::clamp(corner_radius, 0.0F, std::min(rect.width, rect.height) * 0.5F);
  if (radius <= 0.0F) {
    cairo_rectangle(context, rect.x, rect.y, rect.width, rect.height);
    return;
  }
  const double right = rect.x + rect.width;
  const double bottom = rect.y + rect.height;
  cairo_new_sub_path(context);
  cairo_arc(context, right - radius, rect.y + radius, radius, -kPi / 2.0, 0.0);
  cairo_arc(context, right - radius, bottom - radius, radius, 0.0, kPi / 2.0);
  cairo_arc(context, rect.x + radius, bottom - radius, radius, kPi / 2.0, kPi);
  cairo_arc(context, rect.x + radius, rect.y + radius, radius, kPi, 3.0 * kPi / 2.0);
  cairo_close_path(context);
}

void AppendPathContour(cairo_t* context, const Path& path) {
  Point previous;
  for (const PathElement& element : InternalAccess::Elements(path)) {
    switch (element.verb) {
    case PathVerb::MoveTo:
      cairo_move_to(context, element.points[0].x, element.points[0].y);
      previous = element.points[0];
      break;
    case PathVerb::LineTo:
      cairo_line_to(context, element.points[0].x, element.points[0].y);
      previous = element.points[0];
      break;
    case PathVerb::QuadraticTo: {
      const Point control = element.points[0];
      const Point end = element.points[1];
      cairo_curve_to(
          context,
          previous.x + 2.0 / 3.0 * (control.x - previous.x),
          previous.y + 2.0 / 3.0 * (control.y - previous.y),
          end.x + 2.0 / 3.0 * (control.x - end.x),
          end.y + 2.0 / 3.0 * (control.y - end.y),
          end.x,
          end.y
      );
      previous = end;
      break;
    }
    case PathVerb::CubicTo:
      cairo_curve_to(
          context,
          element.points[0].x,
          element.points[0].y,
          element.points[1].x,
          element.points[1].y,
          element.points[2].x,
          element.points[2].y
      );
      previous = element.points[2];
      break;
    case PathVerb::Close:
      cairo_close_path(context);
      break;
    }
  }
}

void AppendPath(cairo_t* context, const Path& path) {
  cairo_new_path(context);
  AppendPathContour(context, path);
}

struct CairoSurfaceHandle {
  explicit CairoSurfaceHandle(cairo_surface_t* value) : surface(value) {}

  ~CairoSurfaceHandle() {
    if (surface != nullptr) {
      cairo_surface_destroy(surface);
    }
  }

  CairoSurfaceHandle(const CairoSurfaceHandle&) = delete;
  CairoSurfaceHandle& operator=(const CairoSurfaceHandle&) = delete;

  cairo_surface_t* surface = nullptr;
};

} // namespace

struct LinuxRenderer::State {
  struct SnapshotTexture {
    ::GdkTexture* texture = nullptr;
    bool flip_y = false;
  };

  struct CachedImage {
    std::uint64_t identity = 0;
    GdkPixbuf* pixbuf = nullptr;
    std::uint64_t bytes = 0;
  };

  struct CachedExternalTexture {
    std::weak_ptr<linux::PixelTexture> texture;
    std::shared_ptr<const LinuxPixelFrame> frame;
    ::GdkTexture* gdk_texture = nullptr;
  };

  struct ShadowMaskKey {
    float width = 0.0F;
    float height = 0.0F;
    float corner_radius = 0.0F;
    float blur_radius = 0.0F;
    float scale = 1.0F;

    bool operator==(const ShadowMaskKey&) const = default;
  };

  struct ShadowMaskEntry {
    ShadowMaskKey key;
    cairo_surface_t* surface = nullptr;
    std::uint64_t bytes = 0;
  };

  struct PathShadowMaskKey {
    Path path;
    PathFillRule fill_rule = PathFillRule::NonZero;
    float blur_radius = 0.0F;
    float scale = 1.0F;

    bool operator==(const PathShadowMaskKey&) const = default;
  };

  struct PathShadowMaskEntry {
    PathShadowMaskKey key;
    cairo_surface_t* surface = nullptr;
    std::uint64_t bytes = 0;
  };

  // Text layouts are re-requested for every measure and paint of every visible
  // row (VirtualList re-runs item factories each frame), and Pango shaping is
  // far too expensive for that cadence. Cache shaped layouts keyed by the full
  // measure inputs so repeated frames reuse the shaped layout instead.
  struct TextLayoutCacheKey {
    AttributedText text;
    TextStyle style;
    float max_width = 0.0F;
    TextLayoutOptions options;

    bool operator==(const TextLayoutCacheKey&) const = default;
  };

  struct TextLayoutCacheEntry {
    TextLayoutCacheKey key;
    PangoLayout* layout = nullptr;
    TextLayoutMetrics metrics;
    std::uint64_t bytes = 0;
  };

  State() {
    PangoFontMap* font_map = pango_cairo_font_map_get_default();
    context = pango_font_map_create_context(font_map);
    pango_cairo_context_set_resolution(context, 96.0);
  }

  ~State() {
    if (backdrop_frame != nullptr) {
      g_object_unref(backdrop_frame);
    }
    for (CachedImage& image : images) {
      g_object_unref(image.pixbuf);
    }
    for (CachedExternalTexture& texture : external_textures) {
      if (texture.gdk_texture != nullptr) {
        g_object_unref(texture.gdk_texture);
      }
    }
    for (ShadowMaskEntry& shadow : shadow_masks) {
      cairo_surface_destroy(shadow.surface);
    }
    for (PathShadowMaskEntry& shadow : path_shadow_masks) {
      cairo_surface_destroy(shadow.surface);
    }
    for (TextLayoutCacheEntry& entry : text_layouts) {
      if (entry.layout != nullptr) {
        g_object_unref(entry.layout);
      }
    }
    g_object_unref(context);
  }

  ::GdkTexture* backdrop_frame = nullptr;
  const RenderNode* backdrop_root = nullptr;
  std::uint64_t backdrop_revision = 0;
  Rect backdrop_bounds;
  float device_scale = 1.0F;
  float backdrop_scale = 0.0F;
  std::optional<Size> viewport;
  // GPU backdrop execution is created on first use on this display; once initialization fails the
  // renderer stays on the portable CPU processor for its lifetime.
  ::GdkDisplay* display = nullptr;
  std::unique_ptr<LinuxBackdropGl> backdrop_gl;
  bool backdrop_gl_unavailable = false;
  LinuxBackdropStats backdrop_stats;

  GdkPixbuf* ImageFor(const ImageAsset& image) {
    if (!image.HasValue()) {
      return nullptr;
    }
    const std::uint64_t identity = InternalAccess::ImageIdentity(image);
    const auto existing = std::find_if(images.begin(), images.end(), [identity](const CachedImage& candidate) {
      return candidate.identity == identity;
    });
    if (existing != images.end()) {
      std::rotate(existing, existing + 1, images.end());
      return images.back().pixbuf;
    }
    GdkPixbufLoader* loader = gdk_pixbuf_loader_new();
    GError* error = nullptr;
    const std::span<const std::byte> bytes = image.EncodedBytes();
    const gboolean wrote = gdk_pixbuf_loader_write(
        loader,
        reinterpret_cast<const guchar*>(bytes.data()),
        bytes.size(),
        &error
    );
    const gboolean closed = wrote != FALSE && gdk_pixbuf_loader_close(loader, &error);
    GdkPixbuf* pixbuf = closed != FALSE ? gdk_pixbuf_loader_get_pixbuf(loader) : nullptr;
    if (pixbuf != nullptr) {
      g_object_ref(pixbuf);
      const std::uint64_t decoded_bytes = gdk_pixbuf_get_byte_length(pixbuf);
      while (!images.empty() &&
             (images.size() >= kMaxImages || image_cache_bytes + decoded_bytes > kImageCacheBudget)) {
        image_cache_bytes -= images.front().bytes;
        g_object_unref(images.front().pixbuf);
        images.erase(images.begin());
      }
      images.push_back({identity, pixbuf, decoded_bytes});
      image_cache_bytes += decoded_bytes;
    }
    if (error != nullptr) {
      g_error_free(error);
    }
    g_object_unref(loader);
    return pixbuf;
  }

  SnapshotTexture AcquireSnapshotTexture(const std::shared_ptr<ExternalTexture>& texture) {
    const std::shared_ptr<linux::GdkTexture> gdk_texture = std::dynamic_pointer_cast<linux::GdkTexture>(texture);
    if (gdk_texture) {
      const std::shared_ptr<const LinuxGdkTextureFrame> frame = GetGdkTextureFrame(*gdk_texture);
      return {frame ? GDK_TEXTURE(g_object_ref(frame->Texture())) : nullptr, false};
    }

    const std::shared_ptr<linux::GlTexture> gl_texture = std::dynamic_pointer_cast<linux::GlTexture>(texture);
    if (gl_texture) {
      const std::shared_ptr<const LinuxGdkTextureFrame> frame = GetGlFrame(*gl_texture);
      return {
          frame ? GDK_TEXTURE(g_object_ref(frame->Texture())) : nullptr,
          frame && frame->TextureOrigin() == linux::GlTexture::Origin::BottomLeft,
      };
    }

    const std::shared_ptr<linux::PixelTexture> pixel_texture = std::dynamic_pointer_cast<linux::PixelTexture>(texture);
    if (!pixel_texture) {
      throw std::logic_error("HuxerUI external texture is incompatible with the Linux renderer");
    }
    for (auto iterator = external_textures.begin(); iterator != external_textures.end();) {
      const std::shared_ptr<linux::PixelTexture> retained = iterator->texture.lock();
      if (retained && retained->IsActive()) {
        ++iterator;
        continue;
      }
      if (iterator->gdk_texture != nullptr) {
        g_object_unref(iterator->gdk_texture);
      }
      iterator = external_textures.erase(iterator);
    }
    auto entry =
        std::find_if(external_textures.begin(), external_textures.end(), [&pixel_texture](const auto& candidate) {
          return candidate.texture.lock() == pixel_texture;
        });
    if (entry == external_textures.end()) {
      entry = external_textures.insert(
          external_textures.end(),
          CachedExternalTexture{
              .texture = pixel_texture,
              .frame = {},
              .gdk_texture = nullptr,
          }
      );
    }
    const std::shared_ptr<const LinuxPixelFrame> frame = GetPixelFrame(*pixel_texture);
    if (frame && frame != entry->frame) {
      entry->frame = frame;
      if (entry->gdk_texture != nullptr) {
        g_object_unref(entry->gdk_texture);
      }
      auto* retained_frame = new std::shared_ptr<const LinuxPixelFrame>(frame);
      GBytes* bytes = g_bytes_new_with_free_func(
          frame->Pixels().data(),
          frame->Pixels().size(),
          [](gpointer data) { delete static_cast<std::shared_ptr<const LinuxPixelFrame>*>(data); },
          retained_frame
      );
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
      constexpr GdkMemoryFormat format = GDK_MEMORY_B8G8R8A8_PREMULTIPLIED;
#else
      constexpr GdkMemoryFormat format = GDK_MEMORY_A8R8G8B8_PREMULTIPLIED;
#endif
      entry->gdk_texture =
          gdk_memory_texture_new(frame->PixelWidth(), frame->PixelHeight(), format, bytes, frame->BytesPerRow());
      g_bytes_unref(bytes);
    }
    return {entry->gdk_texture != nullptr ? GDK_TEXTURE(g_object_ref(entry->gdk_texture)) : nullptr, false};
  }

  // Shaped layout handed to callers. Cache hits borrow the retained layout;
  // oversized paragraphs bypass the cache and come back as an owning handle
  // that releases its Pango resources when the measure or paint scope ends.
  class ScopedTextLayout {
  public:
    ScopedTextLayout() = default;
    ScopedTextLayout(const ScopedTextLayout&) = delete;
    ScopedTextLayout& operator=(const ScopedTextLayout&) = delete;
    ScopedTextLayout(ScopedTextLayout&& other) noexcept
        : layout_(other.layout_), metrics_(other.metrics_), owned_(other.owned_) {
      other.layout_ = nullptr;
      other.owned_ = false;
    }
    ScopedTextLayout& operator=(ScopedTextLayout&& other) noexcept {
      if (this != &other) {
        Release();
        layout_ = std::exchange(other.layout_, nullptr);
        metrics_ = other.metrics_;
        owned_ = std::exchange(other.owned_, false);
      }
      return *this;
    }
    ~ScopedTextLayout() {
      Release();
    }

    static ScopedTextLayout Cached(const TextLayoutCacheEntry& entry) {
      ScopedTextLayout handle;
      handle.layout_ = entry.layout;
      handle.metrics_ = entry.metrics;
      return handle;
    }
    static ScopedTextLayout Owning(PangoLayout* layout, const TextLayoutMetrics& metrics) {
      ScopedTextLayout handle;
      handle.layout_ = layout;
      handle.metrics_ = metrics;
      handle.owned_ = true;
      return handle;
    }

    [[nodiscard]] PangoLayout* layout() const {
      return layout_;
    }
    [[nodiscard]] const TextLayoutMetrics& metrics() const {
      return metrics_;
    }

  private:
    void Release() {
      if (owned_ && layout_ != nullptr) {
        g_object_unref(layout_);
      }
      layout_ = nullptr;
      owned_ = false;
    }

    PangoLayout* layout_ = nullptr;
    TextLayoutMetrics metrics_{};
    bool owned_ = false;
  };

  // Returns the shaped layout for these exact measure inputs, creating it once
  // and reusing it across frames; measurement and painting both go through
  // this cache because virtual lists re-run their item factories every frame.
  ScopedTextLayout TextLayoutFor(const AttributedText& text, const TextStyle& style, float max_width,
      const TextLayoutOptions& options) {
    const auto matches = [&](const TextLayoutCacheEntry& entry) {
      return entry.key.max_width == max_width && entry.key.style == style && entry.key.options == options &&
          entry.key.text == text;
    };
    for (std::size_t position = 0; position < text_layouts.size(); ++position) {
      if (matches(text_layouts[position])) {
        std::rotate(text_layouts.begin() + static_cast<std::ptrdiff_t>(position),
            text_layouts.begin() + static_cast<std::ptrdiff_t>(position) + 1, text_layouts.end());
        return ScopedTextLayout::Cached(text_layouts.back());
      }
    }
    PangoFontMap* font_map = pango_context_get_font_map(context);
    // A private context freezes language, direction, and resolution for the
    // cached layout independently of later renderer-context changes.
    PangoContext* layout_context = pango_font_map_create_context(font_map);
    pango_context_set_language(layout_context, pango_context_get_language(context));
    pango_cairo_context_set_resolution(layout_context, pango_cairo_context_get_resolution(context));
    PangoLayout* layout = pango_layout_new(layout_context);
    g_object_unref(layout_context);
    ConfigureLayout(layout, text, style, max_width, options);

    // Cost accounting matches the Windows, UIKit, and AppKit paragraph caches:
    // ParagraphCacheCost() conservatively charges the text, its style and link
    // metadata, and the platform's shaped-glyph bookkeeping.
    TextLayoutCacheEntry entry;
    entry.key = {text, style, max_width, options};
    entry.layout = layout;
    entry.metrics = LayoutMetrics(layout);
    entry.bytes = ParagraphCacheCost(text);
    // ParagraphCacheCost() marks oversized paragraphs with a cost above the
    // shared budget; hand those to the caller as an owning handle so their
    // memory is released after this measure or paint instead of pinning the
    // layout outside the budget, and skip eviction so one oversized entry
    // cannot displace the entire reusable working set.
    if (entry.bytes > paragraph_cache_budget) {
      return ScopedTextLayout::Owning(layout, entry.metrics);
    }
    while (!text_layouts.empty() &&
           (text_layout_bytes + entry.bytes > paragraph_cache_budget ||
               text_layouts.size() >= kMaxTextLayouts)) {
      text_layout_bytes -= text_layouts.front().bytes;
      if (text_layouts.front().layout != nullptr) {
        g_object_unref(text_layouts.front().layout);
      }
      text_layouts.erase(text_layouts.begin());
    }
    text_layout_bytes += entry.bytes;
    text_layouts.push_back(std::move(entry));
    return ScopedTextLayout::Cached(text_layouts.back());
  }

  PangoContext* context = nullptr;
  std::vector<CachedImage> images;
  std::vector<CachedExternalTexture> external_textures;
  std::vector<ShadowMaskEntry> shadow_masks;
  std::vector<PathShadowMaskEntry> path_shadow_masks;
  std::vector<unsigned char> blur_scratch;
  std::vector<TextLayoutCacheEntry> text_layouts;
  std::uint64_t shadow_mask_bytes = 0;
  std::uint64_t path_shadow_mask_bytes = 0;
  std::uint64_t image_cache_bytes = 0;
  std::uint64_t text_layout_bytes = 0;
  static constexpr std::uint64_t kImageCacheBudget = 32 * 1024 * 1024;
  static constexpr std::size_t kMaxImages = 64;
  static constexpr std::uint64_t kShadowMaskBudget = 32 * 1024 * 1024;
  static constexpr std::uint64_t kPathShadowMaskBudget = 32 * 1024 * 1024;
  static constexpr std::size_t kMaxShadowMasks = 64;
  static constexpr std::size_t kMaxPathShadowMasks = 32;
  static constexpr std::size_t kMaxTextLayouts = 128;
};

namespace {

template <class Command>
concept CairoPaintCommand = std::same_as<Command, DrawRectCommand> || std::same_as<Command, DrawTextCommand> ||
                            std::same_as<Command, DrawTextRunsCommand> || std::same_as<Command, DrawImageCommand> ||
                            std::same_as<Command, DrawCircleCommand> || std::same_as<Command, DrawLineCommand> ||
                            std::same_as<Command, DrawArcCommand> || std::same_as<Command, DrawBorderCommand> ||
                            std::same_as<Command, DrawShadowCommand> || std::same_as<Command, FillPathCommand> ||
                            std::same_as<Command, StrokePathCommand> || std::same_as<Command, DrawPathShadowCommand>;

class CairoCommandPainter final {
public:
  CairoCommandPainter(LinuxRenderer::State& state, cairo_t* context) : state_(state), context_(context) {
    double horizontal_x = 1.0;
    double horizontal_y = 0.0;
    double vertical_x = 0.0;
    double vertical_y = 1.0;
    cairo_user_to_device_distance(context_, &horizontal_x, &horizontal_y);
    cairo_user_to_device_distance(context_, &vertical_x, &vertical_y);
    device_scale_ = static_cast<float>(std::max(
        std::hypot(horizontal_x, horizontal_y),
        std::hypot(vertical_x, vertical_y)
    ));
    device_scale_ = std::max(device_scale_, 0.001F);
  }

  template <CairoPaintCommand Command> void Draw(const Command& command) {
    DrawCommand(command);
  }

private:
  int MaskDimension(float length) const noexcept {
    const double pixels = std::ceil(static_cast<double>(length) * device_scale_);
    if (!std::isfinite(pixels) || pixels <= 0.0 || pixels > std::numeric_limits<int>::max()) {
      return 0;
    }
    return std::max(1, static_cast<int>(pixels));
  }

  static void AddStops(cairo_pattern_t* pattern, const std::vector<GradientStop>& stops) {
    for (const GradientStop& stop : stops) {
      cairo_pattern_add_color_stop_rgba(
          pattern, stop.offset, stop.color.red, stop.color.green, stop.color.blue, stop.color.alpha
      );
    }
    cairo_pattern_set_extend(pattern, CAIRO_EXTEND_PAD);
  }

  template <class Gradient>
  static void ApplyGradientTransform(cairo_pattern_t* pattern, Rect rect, const Gradient& gradient) {
    const Transform2D transform = ResolveGradientTransform(rect, gradient);
    cairo_matrix_t matrix{};
    cairo_matrix_init(&matrix, transform.m11, transform.m12, transform.m21, transform.m22, transform.translate_x,
                      transform.translate_y);
    static_cast<void>(cairo_matrix_invert(&matrix));
    cairo_pattern_set_matrix(pattern, &matrix);
  }

  void ApplyBrush(Rect bounds, const Brush& brush) {
    std::visit(
        [this, bounds](const auto& value) {
          using Value = std::decay_t<decltype(value)>;
          if constexpr (std::same_as<Value, Color>) {
            SetSourceColor(context_, value);
          } else {
            cairo_pattern_t* pattern = nullptr;
            if constexpr (std::same_as<Value, LinearGradient>) {
              pattern = cairo_pattern_create_linear(value.start.x, value.start.y, value.end.x, value.end.y);
            } else {
              pattern = cairo_pattern_create_radial(value.center.x, value.center.y, 0.0, value.center.x,
                                                    value.center.y, value.radius.width);
            }
            AddStops(pattern, value.stops);
            ApplyGradientTransform(pattern, bounds, value);
            cairo_set_source(context_, pattern);
            cairo_pattern_destroy(pattern);
          }
        },
        brush.Get()
    );
  }

  void DrawCommand(const DrawRectCommand& command) {
    if (command.rect.IsEmpty()) {
      return;
    }
    ApplyBrush(command.rect, command.brush);
    cairo_new_path(context_);
    AddRoundedRect(context_, command.rect, command.corner_radius);
    cairo_fill(context_);
  }

  void DrawCommand(const DrawTextCommand& command) {
    if (command.text.PlainText().empty() || command.rect.IsEmpty()) {
      return;
    }
    auto shaped = state_.TextLayoutFor(command.text, command.style, command.rect.width, command.options);
    PangoLayout* layout = shaped.layout();
    PangoRectangle logical{};
    pango_layout_get_extents(layout, nullptr, &logical);
    if (command.options.wrap == TextWrap::NoWrap && PangoUnits(logical.width) < command.rect.width) {
      pango_layout_set_width(layout, static_cast<int>(std::ceil(command.rect.width * PANGO_SCALE)));
    }
    const float remaining_height = std::max(0.0F, command.rect.height - PangoUnits(logical.height));
    float vertical_offset = 0.0F;
    if (command.options.vertical_align == TextVerticalAlign::Center) {
      vertical_offset = remaining_height * 0.5F;
    } else if (command.options.vertical_align == TextVerticalAlign::Bottom) {
      vertical_offset = remaining_height;
    }
    cairo_save(context_);
    cairo_new_path(context_);
    cairo_rectangle(context_, command.rect.x, command.rect.y, command.rect.width, command.rect.height);
    cairo_clip(context_);
    SetSourceColor(context_, command.style.foreground);
    cairo_move_to(
        context_,
        command.rect.x + command.paragraph_offset.x,
        command.rect.y + vertical_offset + command.paragraph_offset.y
    );
    pango_cairo_show_layout(context_, layout);
    cairo_restore(context_);
    // Cached layouts stay owned by the text layout cache; oversized layouts are
    // released when the handle above leaves scope, so nothing is unref'd here.
  }

  void DrawCommand(const DrawTextRunsCommand& command) {
    for (const TextRun& run : command.runs) {
      if (run.text.empty() || run.bounds.IsEmpty() || run.style.foreground.alpha <= 0.0F) {
        continue;
      }
      PangoLayout* layout = pango_layout_new(state_.context);
      ConfigureLayout(
          layout,
          run.text,
          run.style,
          std::numeric_limits<float>::infinity(),
          {.shaping = run.shaping, .wrap = TextWrap::NoWrap}
      );
      SetSourceColor(context_, run.style.foreground);
      const float baseline = PangoUnits(pango_layout_get_baseline(layout));
      cairo_move_to(context_, run.baseline_origin.x, run.baseline_origin.y - baseline);
      pango_cairo_show_layout(context_, layout);
      g_object_unref(layout);
    }
  }

  void DrawCommand(const DrawImageCommand& command) {
    if (command.destination.IsEmpty() || command.source.IsEmpty() || command.opacity <= 0.0F) {
      return;
    }
    GdkPixbuf* pixbuf = state_.ImageFor(command.image);
    if (pixbuf == nullptr) {
      return;
    }
    cairo_save(context_);
    cairo_new_path(context_);
    cairo_rectangle(
        context_, command.destination.x, command.destination.y, command.destination.width, command.destination.height
    );
    cairo_clip(context_);
    const float image_scale = command.image.Scale();
    const double scale_x = command.destination.width / (command.source.width * image_scale);
    const double scale_y = command.destination.height / (command.source.height * image_scale);
    cairo_translate(context_, command.destination.x, command.destination.y);
    cairo_scale(context_, scale_x, scale_y);
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gdk_cairo_set_source_pixbuf(
        context_, pixbuf, -command.source.x * image_scale, -command.source.y * image_scale
    );
    G_GNUC_END_IGNORE_DEPRECATIONS
    cairo_pattern_set_filter(
        cairo_get_source(context_),
        command.sampling == ImageSampling::Nearest ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_BILINEAR
    );
    cairo_paint_with_alpha(context_, std::clamp(command.opacity, 0.0F, 1.0F));
    cairo_restore(context_);
  }

  void DrawCommand(const DrawCircleCommand& command) {
    if (command.radius <= 0.0F || command.color.alpha <= 0.0F) {
      return;
    }
    SetSourceColor(context_, command.color);
    cairo_new_path(context_);
    cairo_arc(context_, command.center.x, command.center.y, command.radius, 0.0, kTau);
    cairo_fill(context_);
  }

  void DrawCommand(const DrawLineCommand& command) {
    if (command.start == command.end || command.style.width <= 0.0F || command.color.alpha <= 0.0F) {
      return;
    }
    SetSourceColor(context_, command.color);
    ApplyStrokeStyle(context_, command.style);
    cairo_new_path(context_);
    cairo_move_to(context_, command.start.x, command.start.y);
    cairo_line_to(context_, command.end.x, command.end.y);
    cairo_stroke(context_);
  }

  void DrawCommand(const DrawArcCommand& command) {
    if (command.radius <= 0.0F || command.style.width <= 0.0F || command.color.alpha <= 0.0F ||
        !std::isfinite(command.start_angle) || !std::isfinite(command.sweep_angle) || command.sweep_angle == 0.0F) {
      return;
    }
    SetSourceColor(context_, command.color);
    ApplyStrokeStyle(context_, command.style);
    cairo_new_path(context_);
    if (std::abs(command.sweep_angle) >= kTau - 0.0001) {
      const double end_angle = command.start_angle + std::copysign(kTau, command.sweep_angle);
      if (command.sweep_angle > 0.0F) {
        cairo_arc(context_, command.center.x, command.center.y, command.radius, command.start_angle, end_angle);
      } else {
        cairo_arc_negative(
            context_, command.center.x, command.center.y, command.radius, command.start_angle, end_angle
        );
      }
      cairo_close_path(context_);
    } else if (command.sweep_angle > 0.0F) {
      cairo_arc(
          context_, command.center.x, command.center.y, command.radius, command.start_angle,
          command.start_angle + command.sweep_angle
      );
    } else {
      cairo_arc_negative(
          context_, command.center.x, command.center.y, command.radius, command.start_angle,
          command.start_angle + command.sweep_angle
      );
    }
    cairo_stroke(context_);
  }

  void DrawCommand(const DrawBorderCommand& command) {
    if (command.rect.IsEmpty() || command.style.width <= 0.0F || command.color.alpha <= 0.0F) {
      return;
    }
    if (!command.style.dash_pattern.empty() || command.style.join != StrokeJoin::Miter ||
        command.style.miter_limit != 4.0F) {
      DrawCommand(StrokePathCommand{
          CreateBorderStrokePath(command.rect, CornerRadii{command.corner_radius}, command.style.width),
          command.color,
          command.rect,
          command.style,
      });
      return;
    }
    const float inset = command.style.width * 0.5F;
    const Rect inner{
        command.rect.x + inset,
        command.rect.y + inset,
        std::max(0.0F, command.rect.width - command.style.width),
        std::max(0.0F, command.rect.height - command.style.width),
    };
    SetSourceColor(context_, command.color);
    ApplyStrokeStyle(context_, command.style);
    cairo_new_path(context_);
    AddRoundedRect(context_, inner, std::max(0.0F, command.corner_radius - inset));
    cairo_stroke(context_);
  }

  void DrawCommand(const DrawShadowCommand& command) {
    const ResolvedShadow resolved = ResolveShadow(command);
    if (resolved.IsEmpty() || command.color.alpha <= 0.0F) {
      return;
    }
    if (command.blur_radius <= 0.0F) {
      DrawCommand(DrawRectCommand{resolved.caster, command.color, resolved.corner_radius});
      return;
    }
    const int mask_width = MaskDimension(resolved.bounds.width);
    const int mask_height = MaskDimension(resolved.bounds.height);
    if (mask_width == 0 || mask_height == 0) {
      return;
    }
    cairo_surface_t* blurred = BlurredRectMaskFor(resolved, command.blur_radius, mask_width, mask_height);
    if (blurred == nullptr) {
      return;
    }

    SetSourceColor(context_, command.color);
    cairo_save(context_);
    cairo_new_path(context_);
    cairo_rectangle(
        context_, resolved.bounds.x, resolved.bounds.y, mask_width / device_scale_, mask_height / device_scale_
    );
    AddRoundedRect(context_, resolved.caster, resolved.corner_radius);
    cairo_set_fill_rule(context_, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_clip(context_);
    cairo_translate(context_, resolved.bounds.x, resolved.bounds.y);
    cairo_scale(context_, 1.0 / device_scale_, 1.0 / device_scale_);
    cairo_pattern_t* pattern = cairo_pattern_create_for_surface(blurred);
    cairo_pattern_set_extend(pattern, CAIRO_EXTEND_NONE);
    cairo_mask(context_, pattern);
    cairo_pattern_destroy(pattern);
    cairo_restore(context_);
  }

  cairo_surface_t*
  BlurredRectMaskFor(const ResolvedShadow& shadow, float blur_radius, int width, int height) {
    const LinuxRenderer::State::ShadowMaskKey key{
        shadow.caster.width,
        shadow.caster.height,
        shadow.corner_radius,
        blur_radius,
        device_scale_,
    };
    const auto cached = std::find_if(
        state_.shadow_masks.begin(),
        state_.shadow_masks.end(),
        [&key](const LinuxRenderer::State::ShadowMaskEntry& entry) { return entry.key == key; }
    );
    if (cached != state_.shadow_masks.end()) {
      std::rotate(cached, cached + 1, state_.shadow_masks.end());
      return state_.shadow_masks.back().surface;
    }

    CairoSurfaceHandle mask{cairo_image_surface_create(CAIRO_FORMAT_A8, width, height)};
    if (cairo_surface_status(mask.surface) != CAIRO_STATUS_SUCCESS) {
      return nullptr;
    }
    cairo_t* mask_context = cairo_create(mask.surface);
    cairo_scale(mask_context, device_scale_, device_scale_);
    cairo_translate(mask_context, blur_radius - shadow.caster.x, blur_radius - shadow.caster.y);
    cairo_set_source_rgb(mask_context, 1.0, 1.0, 1.0);
    AddRoundedRect(mask_context, shadow.caster, shadow.corner_radius);
    cairo_fill(mask_context);
    cairo_destroy(mask_context);

    const int radius = std::max(1, static_cast<int>(std::ceil(blur_radius * device_scale_ * 0.57735F)));
    cairo_surface_t* blurred = BoxBlurMask(mask.surface, radius);
    if (blurred == nullptr) {
      return nullptr;
    }
    const std::uint64_t bytes =
        static_cast<std::uint64_t>(cairo_image_surface_get_stride(blurred)) * static_cast<std::uint64_t>(height);
    while (!state_.shadow_masks.empty() &&
           (state_.shadow_masks.size() >= LinuxRenderer::State::kMaxShadowMasks ||
            state_.shadow_mask_bytes + bytes > LinuxRenderer::State::kShadowMaskBudget)) {
      state_.shadow_mask_bytes -= state_.shadow_masks.front().bytes;
      cairo_surface_destroy(state_.shadow_masks.front().surface);
      state_.shadow_masks.erase(state_.shadow_masks.begin());
    }
    state_.shadow_masks.push_back({key, blurred, bytes});
    state_.shadow_mask_bytes += bytes;
    return blurred;
  }

  cairo_surface_t* BoxBlurMask(cairo_surface_t* mask, int radius) {
    cairo_surface_flush(mask);
    const int width = cairo_image_surface_get_width(mask);
    const int height = cairo_image_surface_get_height(mask);
    const unsigned char* source = cairo_image_surface_get_data(mask);
    const int source_stride = cairo_image_surface_get_stride(mask);
    state_.blur_scratch.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    const int window = radius * 2 + 1;
    for (int y = 0; y < height; ++y) {
      int sum = 0;
      for (int x = -radius; x <= radius; ++x) {
        sum += source[y * source_stride + std::clamp(x, 0, width - 1)];
      }
      for (int x = 0; x < width; ++x) {
        state_.blur_scratch[static_cast<std::size_t>(y) * width + x] = static_cast<unsigned char>(sum / window);
        sum += source[y * source_stride + std::clamp(x + radius + 1, 0, width - 1)] -
               source[y * source_stride + std::clamp(x - radius, 0, width - 1)];
      }
    }
    cairo_surface_t* result = cairo_image_surface_create(CAIRO_FORMAT_A8, width, height);
    if (cairo_surface_status(result) != CAIRO_STATUS_SUCCESS) {
      cairo_surface_destroy(result);
      return nullptr;
    }
    unsigned char* destination = cairo_image_surface_get_data(result);
    const int destination_stride = cairo_image_surface_get_stride(result);
    for (int x = 0; x < width; ++x) {
      int sum = 0;
      for (int y = -radius; y <= radius; ++y) {
        sum += state_.blur_scratch[static_cast<std::size_t>(std::clamp(y, 0, height - 1)) * width + x];
      }
      for (int y = 0; y < height; ++y) {
        destination[y * destination_stride + x] = static_cast<unsigned char>(sum / window);
        sum += state_.blur_scratch[static_cast<std::size_t>(std::clamp(y + radius + 1, 0, height - 1)) * width + x] -
               state_.blur_scratch[static_cast<std::size_t>(std::clamp(y - radius, 0, height - 1)) * width + x];
      }
    }
    cairo_surface_mark_dirty(result);
    return result;
  }

  void DrawCommand(const FillPathCommand& command) {
    if (command.path.IsEmpty()) {
      return;
    }
    ApplyBrush(command.brush_bounds, command.brush);
    AppendPath(context_, command.path);
    cairo_set_fill_rule(
        context_, command.fill_rule == PathFillRule::EvenOdd ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING
    );
    cairo_fill(context_);
  }

  void DrawCommand(const StrokePathCommand& command) {
    if (command.path.IsEmpty() || command.style.width <= 0.0F) {
      return;
    }
    ApplyBrush(command.brush_bounds, command.brush);
    AppendPath(context_, command.path);
    ApplyStrokeStyle(context_, command.style);
    cairo_stroke(context_);
  }

  void DrawCommand(const DrawPathShadowCommand& command) {
    if (command.path.IsEmpty() || command.color.alpha <= 0.0F) {
      return;
    }
    if (command.blur_radius <= 0.0F) {
      SetSourceColor(context_, command.color);
      cairo_save(context_);
      cairo_translate(context_, command.offset.x, command.offset.y);
      AppendPath(context_, command.path);
      cairo_set_fill_rule(
          context_, command.fill_rule == PathFillRule::EvenOdd ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING
      );
      cairo_fill(context_);
      cairo_restore(context_);
      return;
    }

    const Rect bounds = command.path.Bounds();
    const Rect shadow_bounds{
        bounds.x + command.offset.x - command.blur_radius,
        bounds.y + command.offset.y - command.blur_radius,
        bounds.width + command.blur_radius * 2.0F,
        bounds.height + command.blur_radius * 2.0F,
    };
    const int mask_width = MaskDimension(shadow_bounds.width);
    const int mask_height = MaskDimension(shadow_bounds.height);
    if (mask_width == 0 || mask_height == 0) {
      return;
    }
    cairo_surface_t* blurred = BlurredPathMaskFor(command, bounds, mask_width, mask_height);
    if (blurred == nullptr) {
      return;
    }

    SetSourceColor(context_, command.color);
    cairo_save(context_);
    cairo_new_path(context_);
    cairo_rectangle(
        context_, shadow_bounds.x, shadow_bounds.y, mask_width / device_scale_, mask_height / device_scale_
    );
    cairo_matrix_t previous{};
    cairo_get_matrix(context_, &previous);
    cairo_translate(context_, command.offset.x, command.offset.y);
    AppendPathContour(context_, command.path);
    cairo_set_matrix(context_, &previous);
    cairo_set_fill_rule(context_, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_clip(context_);
    cairo_translate(context_, shadow_bounds.x, shadow_bounds.y);
    cairo_scale(context_, 1.0 / device_scale_, 1.0 / device_scale_);
    cairo_pattern_t* pattern = cairo_pattern_create_for_surface(blurred);
    cairo_pattern_set_extend(pattern, CAIRO_EXTEND_NONE);
    cairo_mask(context_, pattern);
    cairo_pattern_destroy(pattern);
    cairo_restore(context_);
  }

  cairo_surface_t*
  BlurredPathMaskFor(const DrawPathShadowCommand& command, const Rect& bounds, int width, int height) {
    const LinuxRenderer::State::PathShadowMaskKey key{
        command.path,
        command.fill_rule,
        command.blur_radius,
        device_scale_,
    };
    const auto cached = std::find_if(
        state_.path_shadow_masks.begin(),
        state_.path_shadow_masks.end(),
        [&key](const LinuxRenderer::State::PathShadowMaskEntry& entry) { return entry.key == key; }
    );
    if (cached != state_.path_shadow_masks.end()) {
      std::rotate(cached, cached + 1, state_.path_shadow_masks.end());
      return state_.path_shadow_masks.back().surface;
    }

    CairoSurfaceHandle mask{cairo_image_surface_create(CAIRO_FORMAT_A8, width, height)};
    if (cairo_surface_status(mask.surface) != CAIRO_STATUS_SUCCESS) {
      return nullptr;
    }
    cairo_t* mask_context = cairo_create(mask.surface);
    cairo_scale(mask_context, device_scale_, device_scale_);
    cairo_translate(mask_context, command.blur_radius - bounds.x, command.blur_radius - bounds.y);
    cairo_set_source_rgb(mask_context, 1.0, 1.0, 1.0);
    AppendPath(mask_context, command.path);
    cairo_set_fill_rule(
        mask_context,
        command.fill_rule == PathFillRule::EvenOdd ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING
    );
    cairo_fill(mask_context);
    cairo_destroy(mask_context);

    const int radius =
        std::max(1, static_cast<int>(std::ceil(command.blur_radius * device_scale_ * 0.57735F)));
    cairo_surface_t* blurred = BoxBlurMask(mask.surface, radius);
    if (blurred == nullptr) {
      return nullptr;
    }
    const std::uint64_t bytes =
        static_cast<std::uint64_t>(cairo_image_surface_get_stride(blurred)) * static_cast<std::uint64_t>(height);
    while (!state_.path_shadow_masks.empty() &&
           (state_.path_shadow_masks.size() >= LinuxRenderer::State::kMaxPathShadowMasks ||
            state_.path_shadow_mask_bytes + bytes > LinuxRenderer::State::kPathShadowMaskBudget)) {
      state_.path_shadow_mask_bytes -= state_.path_shadow_masks.front().bytes;
      cairo_surface_destroy(state_.path_shadow_masks.front().surface);
      state_.path_shadow_masks.erase(state_.path_shadow_masks.begin());
    }
    state_.path_shadow_masks.push_back({key, blurred, bytes});
    state_.path_shadow_mask_bytes += bytes;
    return blurred;
  }

  LinuxRenderer::State& state_;
  cairo_t* context_;
  float device_scale_ = 1.0F;
};

graphene_rect_t GrapheneRect(Rect rect) noexcept {
  graphene_rect_t result;
  graphene_rect_init(&result, rect.x, rect.y, rect.width, rect.height);
  return result;
}

Rect ExpandRect(Rect rect, float amount) noexcept {
  return {
      rect.x - amount,
      rect.y - amount,
      rect.width + amount * 2.0F,
      rect.height + amount * 2.0F,
  };
}

Rect UnionRect(Rect first, Rect second) noexcept {
  if (first.IsEmpty()) {
    return second;
  }
  if (second.IsEmpty()) {
    return first;
  }
  const float left = std::min(first.x, second.x);
  const float top = std::min(first.y, second.y);
  const float right = std::max(first.x + first.width, second.x + second.width);
  const float bottom = std::max(first.y + first.height, second.y + second.height);
  return {left, top, right - left, bottom - top};
}

template <CairoPaintCommand Command> Rect CairoCommandBounds(const Command& command) noexcept {
  if constexpr (std::same_as<Command, DrawRectCommand>) {
    return command.rect;
  } else if constexpr (std::same_as<Command, DrawTextCommand>) {
    return command.rect;
  } else if constexpr (std::same_as<Command, DrawTextRunsCommand>) {
    Rect result;
    for (const TextRun& run : command.runs) {
      result = UnionRect(result, run.bounds);
    }
    return result;
  } else if constexpr (std::same_as<Command, DrawImageCommand>) {
    return command.destination;
  } else if constexpr (std::same_as<Command, DrawCircleCommand>) {
    return {
        command.center.x - command.radius,
        command.center.y - command.radius,
        command.radius * 2.0F,
        command.radius * 2.0F
    };
  } else if constexpr (std::same_as<Command, DrawLineCommand>) {
    const float amount = command.style.width;
    return ExpandRect(
        {std::min(command.start.x, command.end.x),
         std::min(command.start.y, command.end.y),
         std::abs(command.end.x - command.start.x),
         std::abs(command.end.y - command.start.y)},
        amount
    );
  } else if constexpr (std::same_as<Command, DrawArcCommand>) {
    return ExpandRect(
        {command.center.x - command.radius,
         command.center.y - command.radius,
         command.radius * 2.0F,
         command.radius * 2.0F},
        command.style.width
    );
  } else if constexpr (std::same_as<Command, DrawBorderCommand>) {
    return command.rect;
  } else if constexpr (std::same_as<Command, DrawShadowCommand>) {
    return ResolveShadow(command).bounds;
  } else if constexpr (std::same_as<Command, FillPathCommand>) {
    return command.path.Bounds();
  } else if constexpr (std::same_as<Command, StrokePathCommand>) {
    return ExpandRect(command.path.Bounds(), command.style.width * std::max(1.0F, command.style.miter_limit));
  } else {
    static_assert(std::same_as<Command, DrawPathShadowCommand>);
    const Rect bounds = command.path.Bounds();
    return {
        bounds.x + command.offset.x - command.blur_radius,
        bounds.y + command.offset.y - command.blur_radius,
        bounds.width + command.blur_radius * 2.0F,
        bounds.height + command.blur_radius * 2.0F,
    };
  }
}

void ApplySnapshotTransform(GtkSnapshot* snapshot, const Transform2D& transform) {
  if (transform.IsIdentity()) {
    return;
  }
  graphene_matrix_t matrix;
  graphene_matrix_init_from_2d(
      &matrix,
      transform.m11,
      transform.m12,
      transform.m21,
      transform.m22,
      transform.translate_x,
      transform.translate_y
  );
  gtk_snapshot_transform_matrix(snapshot, &matrix);
}

GskPath* CreateGskPath(const Path& path) {
  GskPathBuilder* builder = gsk_path_builder_new();
  for (const PathElement& element : InternalAccess::Elements(path)) {
    switch (element.verb) {
    case PathVerb::MoveTo:
      gsk_path_builder_move_to(builder, element.points[0].x, element.points[0].y);
      break;
    case PathVerb::LineTo:
      gsk_path_builder_line_to(builder, element.points[0].x, element.points[0].y);
      break;
    case PathVerb::QuadraticTo:
      gsk_path_builder_quad_to(
          builder,
          element.points[0].x,
          element.points[0].y,
          element.points[1].x,
          element.points[1].y
      );
      break;
    case PathVerb::CubicTo:
      gsk_path_builder_cubic_to(
          builder,
          element.points[0].x,
          element.points[0].y,
          element.points[1].x,
          element.points[1].y,
          element.points[2].x,
          element.points[2].y
      );
      break;
    case PathVerb::Close:
      gsk_path_builder_close(builder);
      break;
    }
  }
  return gsk_path_builder_free_to_path(builder);
}

Rect BackdropSceneBounds(const RenderNode& node, Transform2D parent = {}) {
  if (!node.visible || node.opacity <= 0.0F) {
    return {};
  }
  const Transform2D own = ComposeTransform(parent, ComposeTransform(TranslationTransform(node.offset), node.transform));
  Rect bounds = UnionRect(TransformBounds(own, node.content.Bounds()), TransformBounds(own, node.foreground.Bounds()));
  const Transform2D children = ComposeTransform(own, node.children_transform);
  for (const RenderNode* child : node.children) {
    if (child) {
      bounds = UnionRect(bounds, BackdropSceneBounds(*child, children));
    }
  }
  return bounds;
}

void CairoTransform(cairo_t* context, const Transform2D& transform) {
  cairo_matrix_t matrix;
  cairo_matrix_init(&matrix, transform.m11, transform.m12, transform.m21, transform.m22,
                   transform.translate_x, transform.translate_y);
  cairo_transform(context, &matrix);
}

class BackdropScenePainter final {
public:
  BackdropScenePainter(LinuxRenderer::State& state, cairo_t* context, bool gpu_frame)
      : state_(state), context_(context), gpu_frame_(gpu_frame) {
    cairo_surface_t* target = cairo_get_target(context_);
    live_group_bytes_ = static_cast<std::size_t>(cairo_image_surface_get_stride(target)) *
                        static_cast<std::size_t>(cairo_image_surface_get_height(target)) * (gpu_frame_ ? 2 : 1);
  }

  void Draw(const RenderNode& node) {
    if (!node.visible || node.opacity <= 0.0F) {
      return;
    }
    cairo_save(context_);
    cairo_translate(context_, node.offset.x, node.offset.y);
    CairoTransform(context_, node.transform);
    const bool isolated = node.opacity < 1.0F;
    const std::size_t parent_clip_depth = clip_depth_;
    cairo_t* parent_context = context_;
    std::unique_ptr<cairo_surface_t, void (*)(cairo_surface_t*)> group_surface(nullptr, cairo_surface_destroy);
    std::unique_ptr<cairo_t, void (*)(cairo_t*)> group_context(nullptr, cairo_destroy);
    std::size_t group_bytes = 0;
    if (isolated) {
      FlushBackdropPlane();
      FlushRaster();
      cairo_surface_t* parent_target = cairo_get_target(parent_context);
      const int width = cairo_image_surface_get_width(parent_target);
      const int height = cairo_image_surface_get_height(parent_target);
      group_bytes = static_cast<std::size_t>(cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width)) *
                    static_cast<std::size_t>(height) * (gpu_frame_ ? 2 : 1);
      if (group_bytes > maximum_backdrop_group_bytes - live_group_bytes_) {
        throw std::runtime_error("HuxerUI Linux backdrop opacity groups exceed the capture budget");
      }
      group_surface.reset(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height));
      if (cairo_surface_status(group_surface.get()) != CAIRO_STATUS_SUCCESS) {
        throw std::runtime_error("HuxerUI Linux backdrop opacity allocation failed");
      }
      double offset_x = 0.0;
      double offset_y = 0.0;
      cairo_surface_get_device_offset(parent_target, &offset_x, &offset_y);
      cairo_surface_set_device_offset(group_surface.get(), offset_x, offset_y);
      group_context.reset(cairo_create(group_surface.get()));
      cairo_matrix_t matrix;
      cairo_get_matrix(parent_context, &matrix);
      cairo_set_matrix(group_context.get(), &matrix);
      // Inherited clips apply at final composition; they must not truncate this group's sampling source.
      context_ = group_context.get();
      clip_depth_ = 0;
      InvalidateClipMask();
      live_group_bytes_ += group_bytes;
      if (gpu_frame_) {
        state_.backdrop_gl->PushGroup();
      }
    }
    Sequence(node.content);
    cairo_save(context_);
    for (const RenderClip& clip : node.child_clips) {
      std::visit([this](const auto& command) { Command(command); }, clip);
    }
    CairoTransform(context_, node.children_transform);
    for (const RenderNode* child : node.children) {
      if (child) {
        Draw(*child);
      }
    }
    if (!node.child_clips.empty()) {
      FlushBackdropPlane();
      FlushRaster();
    }
    for (std::size_t index = 0; index < node.child_clips.size(); ++index) {
      cairo_restore(context_);
      --clip_depth_;
    }
    if (!node.child_clips.empty()) {
      InvalidateClipMask();
    }
    cairo_restore(context_);
    Sequence(node.foreground);
    if (isolated) {
      // Deferred optical output belongs to the group surface and must be written before the group
      // composites onto its parent.
      FlushBackdropPlane();
      FlushRaster();
      if (cairo_status(context_) != CAIRO_STATUS_SUCCESS) {
        throw std::runtime_error("HuxerUI Linux backdrop opacity replay failed");
      }
      context_ = parent_context;
      clip_depth_ = parent_clip_depth;
      InvalidateClipMask();
      live_group_bytes_ -= group_bytes;
      if (gpu_frame_) {
        WithBackdropRows(ClipMask(), [&](const std::uint8_t* pixels, std::size_t stride) {
          state_.backdrop_gl->PopGroup(pixels, stride, std::clamp(node.opacity, 0.0F, 1.0F), mask_changed_);
        });
        mask_changed_ = false;
        cairo_restore(context_);
        return;
      }
      double offset_x = 0.0;
      double offset_y = 0.0;
      cairo_surface_get_device_offset(group_surface.get(), &offset_x, &offset_y);
      cairo_save(context_);
      cairo_identity_matrix(context_);
      cairo_set_source_surface(context_, group_surface.get(), -offset_x, -offset_y);
      cairo_paint_with_alpha(context_, std::clamp(node.opacity, 0.0F, 1.0F));
      cairo_restore(context_);
    }
    cairo_restore(context_);
  }

  // Writes any still-deferred optical output; call once after the root node has been replayed.
  void Finish() {
    FlushBackdropPlane();
    FlushRaster();
  }

private:
  struct DeferredRaster {
    const PaintCommand* command;
    cairo_matrix_t matrix;
    Rect device_bounds;
  };

  static constexpr std::size_t maximum_deferred_commands = 4096;

  template <typename Submit>
  auto WithBackdropRows(cairo_surface_t* surface, Submit submit)
      -> decltype(submit(static_cast<const std::uint8_t*>(nullptr), std::size_t{})) {
    if (surface == nullptr) {
      return submit(nullptr, 0);
    }
    cairo_surface_flush(surface);
    const auto* pixels = cairo_image_surface_get_data(surface);
    const std::size_t stride = static_cast<std::size_t>(cairo_image_surface_get_stride(surface));
#if G_BYTE_ORDER == G_BIG_ENDIAN
    const std::size_t height = static_cast<std::size_t>(cairo_image_surface_get_height(surface));
    const std::size_t width = static_cast<std::size_t>(cairo_image_surface_get_width(surface));
    std::vector<std::uint8_t> bgra(stride * height);
    for (std::size_t row = 0; row < height; ++row) {
      for (std::size_t column = 0; column < width; ++column) {
        const std::size_t offset = row * stride + column * 4;
        bgra[offset] = pixels[offset + 3];
        bgra[offset + 1] = pixels[offset + 2];
        bgra[offset + 2] = pixels[offset + 1];
        bgra[offset + 3] = pixels[offset];
      }
    }
    return submit(bgra.data(), stride);
#else
    return submit(pixels, stride);
#endif
  }

  void InvalidateClipMask() {
    clip_mask_.reset();
    mask_changed_ = true;
  }

  cairo_surface_t* ClipMask() {
    if (clip_depth_ == 0) {
      return nullptr;
    }
    if (clip_mask_) {
      return clip_mask_.get();
    }
    cairo_surface_t* target = cairo_get_target(context_);
    const int width = cairo_image_surface_get_width(target);
    const int height = cairo_image_surface_get_height(target);
    double offset_x = 0.0;
    double offset_y = 0.0;
    cairo_surface_get_device_offset(target, &offset_x, &offset_y);
    cairo_matrix_t matrix;
    cairo_get_matrix(context_, &matrix);
    cairo_save(context_);
    cairo_push_group_with_content(context_, CAIRO_CONTENT_ALPHA);
    cairo_set_operator(context_, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(context_, 1.0, 1.0, 1.0, 1.0);
    cairo_paint(context_);
    std::unique_ptr<cairo_pattern_t, void (*)(cairo_pattern_t*)> pattern(
        cairo_pop_group(context_), cairo_pattern_destroy);
    cairo_restore(context_);
    std::unique_ptr<cairo_surface_t, void (*)(cairo_surface_t*)> mask(
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height), cairo_surface_destroy);
    if (cairo_surface_status(mask.get()) != CAIRO_STATUS_SUCCESS) {
      throw std::runtime_error("HuxerUI Linux backdrop clip allocation failed");
    }
    cairo_surface_set_device_offset(mask.get(), offset_x, offset_y);
    std::unique_ptr<cairo_t, void (*)(cairo_t*)> context(cairo_create(mask.get()), cairo_destroy);
    cairo_set_matrix(context.get(), &matrix);
    cairo_set_source(context.get(), pattern.get());
    cairo_paint(context.get());
    if (cairo_status(context.get()) != CAIRO_STATUS_SUCCESS) {
      throw std::runtime_error("HuxerUI Linux backdrop clip rasterization failed");
    }
    cairo_surface_flush(mask.get());
    clip_mask_ = std::move(mask);
    ++state_.backdrop_stats.mask_rasterizations;
    return clip_mask_.get();
  }

  void FlushRaster() {
    if (!gpu_frame_ || raster_bounds_.IsEmpty()) {
      return;
    }
    cairo_surface_t* target = cairo_get_target(context_);
    WithBackdropRows(target, [&](const std::uint8_t* pixels, std::size_t stride) {
      state_.backdrop_gl->CompositeRaster(pixels, stride, raster_bounds_);
    });
    // Clearing the upload segment must not depend on the current node's clip or transform.
    std::unique_ptr<cairo_t, void (*)(cairo_t*)> clear(cairo_create(target), cairo_destroy);
    double offset_x = 0;
    double offset_y = 0;
    cairo_surface_get_device_offset(target, &offset_x, &offset_y);
    cairo_set_operator(clear.get(), CAIRO_OPERATOR_CLEAR);
    cairo_rectangle(clear.get(), raster_bounds_.x - offset_x, raster_bounds_.y - offset_y, raster_bounds_.width,
                    raster_bounds_.height);
    cairo_fill(clear.get());
    raster_bounds_ = {};
  }

  void AccumulateRasterBounds(Rect local_bounds, bool use_clip = false) {
    if (!gpu_frame_) {
      return;
    }
    if (use_clip) {
      double left, top, right, bottom;
      cairo_clip_extents(context_, &left, &top, &right, &bottom);
      local_bounds = {static_cast<float>(left), static_cast<float>(top), static_cast<float>(right - left),
                      static_cast<float>(bottom - top)};
    }
    const Rect bounds = ExpandRect(TransformBounds(CurrentDeviceTransform(), local_bounds), 3.0F);
    cairo_surface_t* target = cairo_get_target(context_);
    const float width = static_cast<float>(cairo_image_surface_get_width(target));
    const float height = static_cast<float>(cairo_image_surface_get_height(target));
    if (!std::isfinite(bounds.x) || !std::isfinite(bounds.y) || !std::isfinite(bounds.width) ||
        !std::isfinite(bounds.height) || !std::isfinite(bounds.x + bounds.width) ||
        !std::isfinite(bounds.y + bounds.height)) {
      raster_bounds_ = {0, 0, width, height};
      return;
    }
    const float left = std::clamp(std::floor(bounds.x), 0.0F, width);
    const float top = std::clamp(std::floor(bounds.y), 0.0F, height);
    const float right = std::clamp(std::ceil(bounds.x + bounds.width), 0.0F, width);
    const float bottom = std::clamp(std::ceil(bounds.y + bounds.height), 0.0F, height);
    raster_bounds_ = UnionRect(raster_bounds_, {left, top, right - left, bottom - top});
  }

  template <CairoPaintCommand Command> void DrawRaster(const Command& command) {
    CairoCommandPainter(state_, context_).Draw(command);
    Rect bounds = CairoCommandBounds(command);
    if constexpr (std::same_as<Command, DrawBorderCommand>) {
      bounds = ExpandRect(bounds, command.style.width * std::max(1.0F, command.style.miter_limit));
    }
    // Run bounds describe layout, not necessarily glyph ink. Retain the complete clip for this command.
    AccumulateRasterBounds(bounds, std::same_as<Command, DrawTextRunsCommand>);
  }

  void Sequence(const PaintSequence& sequence) {
    for (const PaintCommand& command : sequence.Commands()) {
      std::visit([this, &command](const auto& value) {
        if constexpr (CairoPaintCommand<std::decay_t<decltype(value)>>) {
          if (deferred_raster_.size() == maximum_deferred_commands) {
            FlushBackdropPlane();
          }
          if (planes_.empty()) {
            DrawRaster(value);
          } else {
            Rect bounds = CairoCommandBounds(value);
            if constexpr (std::same_as<std::decay_t<decltype(value)>, DrawBorderCommand>) {
              bounds = ExpandRect(bounds, value.style.width * std::max(1.0F, value.style.miter_limit));
            }
            cairo_matrix_t matrix;
            cairo_get_matrix(context_, &matrix);
            const Rect device_bounds = bounds.IsEmpty()
                ? Rect{} : ExpandRect(TransformBounds(CurrentDeviceTransform(), bounds), 3.0F);
            deferred_raster_.push_back({&command, matrix, device_bounds});
          }
        } else {
          Command(value);
        }
      }, command);
    }
  }

  void Command(const DrawBackdropCommand& command) {
    const Transform2D local_to_pixel = CurrentDeviceTransform();
    if (!InverseTransform(local_to_pixel)) {
      return;
    }
    const Rect sampling = ExpandRect(TransformBounds(local_to_pixel, BackdropSamplingBounds(command)), 3.0F);
    if (!std::isfinite(sampling.x) || !std::isfinite(sampling.y) || !std::isfinite(sampling.width) ||
        !std::isfinite(sampling.height) || !std::isfinite(sampling.x + sampling.width) ||
        !std::isfinite(sampling.y + sampling.height)) {
      throw std::invalid_argument("HuxerUI Linux backdrop sampling bounds must remain finite");
    }
    // Moving earlier raster work behind this effect preserves its source only when that work lies
    // outside the complete sampling halo. Deferred raster commands retain their original order.
    const bool needs_raster_prefix = std::any_of(deferred_raster_.begin(), deferred_raster_.end(),
        [&](const DeferredRaster& raster) { return sampling.Intersects(raster.device_bounds); });
    if (needs_raster_prefix || !planes_.TryAdd(command, local_to_pixel, 3.0F)) {
      FlushBackdropPlane();
      const bool added = planes_.TryAdd(command, local_to_pixel, 3.0F);
      (void)added; // An empty batch always accepts a command that passed record-time validation.
    }
  }

  Transform2D CurrentDeviceTransform() const {
    double offset_x = 0.0;
    double offset_y = 0.0;
    cairo_surface_t* target = cairo_get_group_target(context_);
    cairo_surface_get_device_offset(target, &offset_x, &offset_y);
    cairo_matrix_t matrix;
    cairo_get_matrix(context_, &matrix);
    return Transform2D{
        static_cast<float>(matrix.xx), static_cast<float>(matrix.yx),
        static_cast<float>(matrix.xy), static_cast<float>(matrix.yy),
        static_cast<float>(matrix.x0 + offset_x), static_cast<float>(matrix.y0 + offset_y)};
  }

  void FlushBackdropPlane() {
    if (planes_.empty()) {
      return;
    }
    const detail::BackdropPlaneBatch batch = planes_.batch();
    planes_.Clear();
    ApplyBackdropPlane(batch);
    cairo_save(context_);
    for (const DeferredRaster& raster : deferred_raster_) {
      cairo_set_matrix(context_, &raster.matrix);
      std::visit([this](const auto& command) {
        if constexpr (CairoPaintCommand<std::decay_t<decltype(command)>>) {
          DrawRaster(command);
        }
      }, *raster.command);
    }
    cairo_restore(context_);
    deferred_raster_.clear();
  }

  void ApplyBackdropPlane(const detail::BackdropPlaneBatch& batch) {
    cairo_surface_t* target = cairo_get_group_target(context_);
    if (cairo_surface_get_type(target) != CAIRO_SURFACE_TYPE_IMAGE ||
        cairo_image_surface_get_format(target) != CAIRO_FORMAT_ARGB32) {
      throw std::logic_error("HuxerUI Linux backdrop requires an ARGB32 capture target");
    }
    cairo_surface_flush(target);
    const int width = cairo_image_surface_get_width(target);
    const int height = cairo_image_surface_get_height(target);
    const int stride = cairo_image_surface_get_stride(target);
    double offset_x = 0.0;
    double offset_y = 0.0;
    cairo_surface_get_device_offset(target, &offset_x, &offset_y);
    const auto clamp_start = [](float value, int extent) {
      return static_cast<int>(std::clamp(std::floor(value), 0.0F, static_cast<float>(extent)));
    };
    const auto clamp_end = [](float value, int extent) {
      return static_cast<int>(std::clamp(std::ceil(value), 0.0F, static_cast<float>(extent)));
    };
    const int left = clamp_start(batch.sampling_bounds.x, width);
    const int top = clamp_start(batch.sampling_bounds.y, height);
    const int right = clamp_end(batch.sampling_bounds.x + batch.sampling_bounds.width, width);
    const int bottom = clamp_end(batch.sampling_bounds.y + batch.sampling_bounds.height, height);
    if (right <= left || bottom <= top) {
      return;
    }
    ++state_.backdrop_stats.plane_count;
    state_.backdrop_stats.command_count += batch.commands.size();
    if (gpu_frame_) {
      FlushRaster();
      const bool applied = WithBackdropRows(ClipMask(), [&](const std::uint8_t* pixels, std::size_t stride) {
        return state_.backdrop_gl->ApplyToFrame(batch, pixels, stride, mask_changed_);
      });
      mask_changed_ = false;
      if (!applied) {
        throw std::logic_error("HuxerUI backdrop blur requires axis-aligned scaling or a uniform orthogonal transform");
      }
      ++state_.backdrop_stats.gpu_plane_count;
      return;
    }
    const std::size_t capture_width = static_cast<std::size_t>(right - left);
    const std::size_t capture_stride = capture_width * 4;
    std::vector<std::uint8_t> capture(capture_stride * static_cast<std::size_t>(bottom - top));
    const auto* source = cairo_image_surface_get_data(target);
    for (int row = top; row < bottom; ++row) {
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
      std::memcpy(capture.data() + static_cast<std::size_t>(row - top) * capture_stride,
                  source + static_cast<std::size_t>(row) * static_cast<std::size_t>(stride) + left * 4,
                  capture_stride);
#else
      const auto* input = source + static_cast<std::size_t>(row) * static_cast<std::size_t>(stride) + left * 4;
      auto* output = capture.data() + static_cast<std::size_t>(row - top) * capture_stride;
      for (std::size_t column = 0; column < capture_width; ++column) {
        output[column * 4] = input[column * 4 + 3];
        output[column * 4 + 1] = input[column * 4 + 2];
        output[column * 4 + 2] = input[column * 4 + 1];
        output[column * 4 + 3] = input[column * 4];
      }
#endif
    }
    for (const auto& member : batch.commands) {
      const Rect sampling = ExpandRect(
          TransformBounds(member.local_to_device, BackdropSamplingBounds(*member.command)), 3.0F);
      const int region_left = std::max(left, clamp_start(sampling.x, width));
      const int region_top = std::max(top, clamp_start(sampling.y, height));
      const int region_right = std::min(right, clamp_end(sampling.x + sampling.width, width));
      const int region_bottom = std::min(bottom, clamp_end(sampling.y + sampling.height, height));
      if (region_right <= region_left || region_bottom <= region_top) {
        continue;
      }
      const std::size_t region_width = static_cast<std::size_t>(region_right - region_left);
      const std::size_t region_height = static_cast<std::size_t>(region_bottom - region_top);
      std::vector<std::uint8_t> region(region_width * region_height * 4);
      for (std::size_t row = 0; row < region_height; ++row) {
        std::memcpy(region.data() + row * region_width * 4,
                    capture.data() +
                        (static_cast<std::size_t>(region_top - top) + row) * capture_stride +
                        static_cast<std::size_t>(region_left - left) * 4,
                    region_width * 4);
      }
      const auto inverse = InverseTransform(member.local_to_device);
      if (!inverse) {
        continue;
      }
      const Transform2D pixel_to_local = ComposeTransform(*inverse, TranslationTransform(
          {static_cast<float>(region_left), static_cast<float>(region_top)}));
      BackdropPixels pixels = ApplyBackdropEffect(region, region_width, region_height, region_width * 4,
          pixel_to_local, *member.command);
      // Members never write into another member's optical output, so command-order region writes
      // reproduce immediate per-command replay on the shared capture.
      for (std::size_t row = 0; row < region_height; ++row) {
        std::memcpy(capture.data() +
                        (static_cast<std::size_t>(region_top - top) + row) * capture_stride +
                        static_cast<std::size_t>(region_left - left) * 4,
                    pixels.bgra.data() + row * pixels.stride,
                    region_width * 4);
      }
    }
#if G_BYTE_ORDER == G_BIG_ENDIAN
    for (std::size_t offset = 0; offset < capture.size(); offset += 4) {
      std::swap(capture[offset], capture[offset + 3]);
      std::swap(capture[offset + 1], capture[offset + 2]);
    }
#endif
    CairoSurfaceHandle surface{cairo_image_surface_create_for_data(capture.data(), CAIRO_FORMAT_ARGB32,
        right - left, bottom - top, static_cast<int>(capture_stride))};
    if (cairo_surface_status(surface.surface) != CAIRO_STATUS_SUCCESS) {
      throw std::runtime_error("HuxerUI Linux backdrop output allocation failed");
    }
    cairo_save(context_);
    cairo_identity_matrix(context_);
    cairo_set_operator(context_, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(context_, surface.surface, left - offset_x, top - offset_y);
    cairo_rectangle(context_, left - offset_x, top - offset_y, right - left, bottom - top);
    cairo_fill(context_);
    cairo_restore(context_);
  }

  void Command(const DrawExternalTextureCommand& command) {
    FlushBackdropPlane();
    auto acquired = state_.AcquireSnapshotTexture(command.texture);
    if (!acquired.texture) {
      return;
    }
    std::unique_ptr<::GdkTexture, void (*)(::GdkTexture*)> texture(acquired.texture,
        [](::GdkTexture* value) { g_object_unref(value); });
    const int width = gdk_texture_get_width(texture.get());
    const int height = gdk_texture_get_height(texture.get());
    if (width <= 0 || height <= 0 || static_cast<std::uint64_t>(width) * height > 16777216) {
      throw std::runtime_error("HuxerUI Linux backdrop texture exceeds the capture budget");
    }
    AccumulateRasterBounds(command.destination);
    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
    std::vector<unsigned char> pixels(static_cast<std::size_t>(stride) * static_cast<std::size_t>(height));
    gdk_texture_download(texture.get(), pixels.data(), static_cast<std::size_t>(stride));
    CairoSurfaceHandle surface{cairo_image_surface_create_for_data(pixels.data(), CAIRO_FORMAT_ARGB32,
        width, height, stride)};
    cairo_save(context_);
    cairo_rectangle(context_, command.destination.x, command.destination.y,
                    command.destination.width, command.destination.height);
    cairo_clip(context_);
    const Size intrinsic = command.texture->IntrinsicSize();
    cairo_translate(context_, command.destination.x, command.destination.y);
    cairo_scale(context_, command.destination.width / command.source.width,
                command.destination.height / command.source.height);
    cairo_translate(context_, -command.source.x, -command.source.y);
    if (acquired.flip_y) {
      cairo_translate(context_, 0.0, intrinsic.height);
      cairo_scale(context_, 1.0, -1.0);
    }
    cairo_scale(context_, intrinsic.width / width, intrinsic.height / height);
    cairo_set_source_surface(context_, surface.surface, 0.0, 0.0);
    cairo_pattern_set_filter(cairo_get_source(context_), command.sampling == ImageSampling::Nearest
        ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_BILINEAR);
    cairo_paint_with_alpha(context_, command.opacity);
    cairo_restore(context_);
  }

  void Command(const PushClipCommand& command) {
    // Deferred optical output must be written under the clip active at its recording position.
    FlushBackdropPlane();
    cairo_save(context_);
    cairo_new_path(context_);
    AddRoundedRect(context_, command.rect, command.corner_radius);
    cairo_clip(context_);
    ++clip_depth_;
    InvalidateClipMask();
  }

  void Command(const PushPathClipCommand& command) {
    FlushBackdropPlane();
    cairo_save(context_);
    cairo_new_path(context_);
    AppendPathContour(context_, command.path);
    cairo_set_fill_rule(context_, command.fill_rule == PathFillRule::EvenOdd ? CAIRO_FILL_RULE_EVEN_ODD
                                                                           : CAIRO_FILL_RULE_WINDING);
    cairo_clip(context_);
    ++clip_depth_;
    InvalidateClipMask();
  }

  void Command(const PopClipCommand&) {
    FlushBackdropPlane();
    cairo_restore(context_);
    --clip_depth_;
    InvalidateClipMask();
  }
  void Command(const PushTransformCommand& command) {
    cairo_save(context_);
    CairoTransform(context_, command.transform);
  }
  void Command(const PopTransformCommand&) { cairo_restore(context_); }
  void Command(const PlacePlatformViewCommand&) {
    throw std::logic_error("HuxerUI backdrop effects cannot sample native PlatformView content");
  }

  LinuxRenderer::State& state_;
  cairo_t* context_;
  bool gpu_frame_ = false;
  Rect raster_bounds_;
  std::size_t clip_depth_ = 0;
  std::unique_ptr<cairo_surface_t, void (*)(cairo_surface_t*)> clip_mask_{nullptr, cairo_surface_destroy};
  bool mask_changed_ = true;
  std::size_t live_group_bytes_ = 0;
  detail::BackdropPlaneCollector planes_;
  std::vector<DeferredRaster> deferred_raster_;
};

void SnapshotBackdropScene(LinuxRenderer::State& state, GtkSnapshot* snapshot, const RenderFrame& frame) {
  Rect bounds = state.viewport
      ? Rect{0.0F, 0.0F, state.viewport->width, state.viewport->height}
      : BackdropSceneBounds(*frame.scene.root);
  if (bounds.IsEmpty()) {
    return;
  }
  const double pixel_left = std::floor(static_cast<double>(bounds.x) * state.device_scale);
  const double pixel_top = std::floor(static_cast<double>(bounds.y) * state.device_scale);
  const double pixel_width = std::ceil(static_cast<double>(bounds.x + bounds.width) * state.device_scale) - pixel_left;
  const double pixel_height = std::ceil(static_cast<double>(bounds.y + bounds.height) * state.device_scale) - pixel_top;
  if (!std::isfinite(pixel_width) || !std::isfinite(pixel_height) || pixel_width < 1 || pixel_height < 1 ||
      pixel_width > 16384 || pixel_height > 16384 || pixel_width * pixel_height > 16777216) {
    throw std::runtime_error("HuxerUI Linux backdrop scene exceeds the capture budget");
  }
  const int width = static_cast<int>(pixel_width);
  const int height = static_cast<int>(pixel_height);
  bounds.x = static_cast<float>(pixel_left / state.device_scale);
  bounds.y = static_cast<float>(pixel_top / state.device_scale);
  bounds.width = width / state.device_scale;
  bounds.height = height / state.device_scale;
  if (state.backdrop_frame == nullptr || state.backdrop_root != frame.scene.root ||
      state.backdrop_revision != frame.revision || state.backdrop_bounds != bounds ||
      state.backdrop_scale != state.device_scale) {
    CairoSurfaceHandle surface{cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height)};
    if (cairo_surface_status(surface.surface) != CAIRO_STATUS_SUCCESS) {
      throw std::runtime_error("HuxerUI Linux backdrop scene allocation failed");
    }
    std::unique_ptr<cairo_t, void (*)(cairo_t*)> context(cairo_create(surface.surface), cairo_destroy);
    cairo_scale(context.get(), state.device_scale, state.device_scale);
    cairo_translate(context.get(), -bounds.x, -bounds.y);
    bool gpu_frame = false;
    if (!state.backdrop_gl_unavailable && state.display != nullptr) {
      if (!state.backdrop_gl) {
        state.backdrop_gl = std::make_unique<LinuxBackdropGl>();
      }
      if (!state.backdrop_gl->TryInitialize(state.display)) {
        state.backdrop_gl_unavailable = true;
        state.backdrop_gl.reset();
      } else {
        gpu_frame = state.backdrop_gl->BeginFrame(static_cast<std::size_t>(width), static_cast<std::size_t>(height));
      }
    }
    BackdropScenePainter painter(state, context.get(), gpu_frame);
    try {
      painter.Draw(*frame.scene.root);
      painter.Finish();
    } catch (...) {
      if (gpu_frame) {
        state.backdrop_gl->CancelFrame();
      }
      throw;
    }
    if (cairo_status(context.get()) != CAIRO_STATUS_SUCCESS) {
      throw std::runtime_error("HuxerUI Linux backdrop scene replay failed");
    }
    cairo_surface_flush(surface.surface);
    const std::size_t stride = static_cast<std::size_t>(cairo_image_surface_get_stride(surface.surface));
    ::GdkTexture* texture = nullptr;
    if (gpu_frame) {
      texture = state.backdrop_gl->FinishFrame();
      const LinuxBackdropGlStats stats = state.backdrop_gl->FrameStats();
      state.backdrop_stats.source_capture_count = stats.source_captures;
      state.backdrop_stats.blur_pass_count = stats.blur_passes;
      state.backdrop_stats.blur_cache_hits = stats.blur_cache_hits;
      state.backdrop_stats.raster_upload_bytes = stats.raster_upload_bytes;
      state.backdrop_stats.readback_bytes = stats.readback_bytes;
      state.backdrop_stats.mask_upload_bytes = stats.mask_upload_bytes;
    } else {
      GBytes* bytes = g_bytes_new(cairo_image_surface_get_data(surface.surface), stride * height);
      texture = gdk_memory_texture_new(width, height, GDK_MEMORY_DEFAULT, bytes, stride);
      g_bytes_unref(bytes);
    }
    if (state.backdrop_frame) {
      g_object_unref(state.backdrop_frame);
    }
    state.backdrop_frame = texture;
    state.backdrop_root = frame.scene.root;
    state.backdrop_revision = frame.revision;
    state.backdrop_bounds = bounds;
    state.backdrop_scale = state.device_scale;
  }
  const graphene_rect_t output = GrapheneRect(bounds);
  gtk_snapshot_append_texture(snapshot, state.backdrop_frame, &output);
}

class SnapshotScenePainter final {
public:
  SnapshotScenePainter(LinuxRenderer::State& state, GtkSnapshot* snapshot,
                       std::optional<std::pair<std::size_t, std::size_t>> command_range = std::nullopt)
      : state_(state), snapshot_(snapshot), command_range_(command_range) {}

  void Draw(const RenderScene& scene) {
    if (scene.root != nullptr) {
      DrawNode(*scene.root);
    }
  }

private:
  struct CairoBatch {
    std::vector<const PaintCommand*> commands;
    Rect bounds;
  };

  void DrawNode(const RenderNode& node) {
    const float opacity = std::clamp(node.opacity, 0.0F, 1.0F);
    if (!node.visible || opacity <= 0.0F) {
      return;
    }
    gtk_snapshot_save(snapshot_);
    graphene_point_t offset;
    graphene_point_init(&offset, node.offset.x, node.offset.y);
    gtk_snapshot_translate(snapshot_, &offset);
    ApplySnapshotTransform(snapshot_, node.transform);
    if (opacity < 1.0F) {
      gtk_snapshot_push_opacity(snapshot_, opacity);
    }
    DrawSequence(node.content);
    for (const RenderClip& clip : node.child_clips) {
      std::visit([this](const auto& value) { PushClip(value); }, clip);
    }
    gtk_snapshot_save(snapshot_);
    ApplySnapshotTransform(snapshot_, node.children_transform);
    for (const RenderNode* child : node.children) {
      if (child != nullptr) {
        DrawNode(*child);
      }
    }
    gtk_snapshot_restore(snapshot_);
    for (std::size_t index = 0; index < node.child_clips.size(); ++index) {
      gtk_snapshot_pop(snapshot_);
    }
    DrawSequence(node.foreground);
    if (opacity < 1.0F) {
      gtk_snapshot_pop(snapshot_);
    }
    gtk_snapshot_restore(snapshot_);
  }

  void DrawSequence(const PaintSequence& sequence) {
    CairoBatch batch;
    batch.commands.reserve(sequence.Commands().size());
    for (const PaintCommand& command : sequence.Commands()) {
      if (!std::holds_alternative<PlacePlatformViewCommand>(command) && command_range_.has_value()) {
        const std::size_t index = command_cursor_++;
        const std::size_t end = command_range_->first + command_range_->second;
        if (index < command_range_->first || index >= end) {
          continue;
        }
      }
      std::visit([this, &batch, &command](const auto& value) { DrawCommand(batch, command, value); }, command);
    }
    Flush(batch);
  }

  template <CairoPaintCommand Command>
  void DrawCommand(CairoBatch& batch, const PaintCommand& command, const Command& value) {
    const Rect bounds = CairoCommandBounds(value);
    if (bounds.IsEmpty()) {
      return;
    }
    batch.bounds = UnionRect(batch.bounds, bounds);
    batch.commands.push_back(&command);
  }

  void DrawCommand(CairoBatch& batch, const PaintCommand&, const DrawExternalTextureCommand& command) {
    Flush(batch);
    if (command.destination.IsEmpty() || command.source.IsEmpty() || command.opacity <= 0.0F) {
      return;
    }
    LinuxRenderer::State::SnapshotTexture acquired = state_.AcquireSnapshotTexture(command.texture);
    if (acquired.texture == nullptr) {
      return;
    }
    const Size intrinsic = command.texture->IntrinsicSize();
    const float scale_x = command.destination.width / command.source.width;
    const float scale_y = command.destination.height / command.source.height;
    const Rect texture_bounds{
        command.destination.x - command.source.x * scale_x,
        command.destination.y - command.source.y * scale_y,
        intrinsic.width * scale_x,
        intrinsic.height * scale_y,
    };
    const graphene_rect_t clip = GrapheneRect(command.destination);
    const graphene_rect_t bounds = GrapheneRect(texture_bounds);
    gtk_snapshot_push_clip(snapshot_, &clip);
    if (command.opacity < 1.0F) {
      gtk_snapshot_push_opacity(snapshot_, std::clamp(command.opacity, 0.0F, 1.0F));
    }
    if (acquired.flip_y) {
      gtk_snapshot_save(snapshot_);
      graphene_point_t flip_offset;
      graphene_point_init(&flip_offset, 0.0F, texture_bounds.y * 2.0F + texture_bounds.height);
      gtk_snapshot_translate(snapshot_, &flip_offset);
      gtk_snapshot_scale(snapshot_, 1.0F, -1.0F);
    }
    gtk_snapshot_append_scaled_texture(
        snapshot_,
        acquired.texture,
        command.sampling == ImageSampling::Nearest ? GSK_SCALING_FILTER_NEAREST : GSK_SCALING_FILTER_LINEAR,
        &bounds
    );
    if (acquired.flip_y) {
      gtk_snapshot_restore(snapshot_);
    }
    if (command.opacity < 1.0F) {
      gtk_snapshot_pop(snapshot_);
    }
    gtk_snapshot_pop(snapshot_);
    g_object_unref(acquired.texture);
  }

  void DrawCommand(CairoBatch& batch, const PaintCommand&, const PushClipCommand& command) {
    Flush(batch);
    PushClip(command);
  }

  void DrawCommand(CairoBatch& batch, const PaintCommand&, const PushPathClipCommand& command) {
    Flush(batch);
    PushClip(command);
  }

  void DrawCommand(CairoBatch& batch, const PaintCommand&, const PopClipCommand&) {
    Flush(batch);
    gtk_snapshot_pop(snapshot_);
  }

  void DrawCommand(CairoBatch& batch, const PaintCommand&, const PushTransformCommand& command) {
    Flush(batch);
    gtk_snapshot_save(snapshot_);
    ApplySnapshotTransform(snapshot_, command.transform);
  }

  void DrawCommand(CairoBatch& batch, const PaintCommand&, const PopTransformCommand&) {
    Flush(batch);
    gtk_snapshot_restore(snapshot_);
  }

  void DrawCommand(CairoBatch& batch, const PaintCommand&, const PlacePlatformViewCommand&) {
    Flush(batch);
  }

  void DrawCommand(CairoBatch&, const PaintCommand&, const DrawBackdropCommand&) {
    throw std::logic_error("HuxerUI Linux backdrop requires complete scene capture");
  }

  void Flush(CairoBatch& batch) {
    if (batch.commands.empty()) {
      return;
    }
    const graphene_rect_t cairo_bounds = GrapheneRect(batch.bounds);
    cairo_t* context = gtk_snapshot_append_cairo(snapshot_, &cairo_bounds);
    CairoCommandPainter painter(state_, context);
    for (const PaintCommand* command : batch.commands) {
      std::visit(
          [&painter](const auto& value) {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (CairoPaintCommand<Value>) {
              painter.Draw(value);
            } else {
              throw std::logic_error("HuxerUI GTK snapshot contains a non-Cairo command in a Cairo batch");
            }
          },
          *command
      );
    }
    cairo_destroy(context);
    batch.commands.clear();
    batch.bounds = {};
  }

  void PushClip(const PushClipCommand& command) {
    const graphene_rect_t rect = GrapheneRect(command.rect);
    if (command.corner_radius <= 0.0F) {
      gtk_snapshot_push_clip(snapshot_, &rect);
      return;
    }
    GskRoundedRect rounded;
    gsk_rounded_rect_init_from_rect(&rounded, &rect, command.corner_radius);
    gtk_snapshot_push_rounded_clip(snapshot_, &rounded);
  }

  void PushClip(const PushPathClipCommand& command) {
    GskPath* path = CreateGskPath(command.path);
    gtk_snapshot_push_fill(
        snapshot_,
        path,
        command.fill_rule == PathFillRule::EvenOdd ? GSK_FILL_RULE_EVEN_ODD : GSK_FILL_RULE_WINDING
    );
    gsk_path_unref(path);
  }

  LinuxRenderer::State& state_;
  GtkSnapshot* snapshot_ = nullptr;
  std::optional<std::pair<std::size_t, std::size_t>> command_range_;
  std::size_t command_cursor_ = 0;
};

} // namespace

LinuxRenderer::LinuxRenderer() : state_(std::make_unique<State>()) {}

LinuxRenderer::~LinuxRenderer() = default;

void LinuxRenderer::Initialize() {
  if (!state_) {
    state_ = std::make_unique<State>();
  }
}

void LinuxRenderer::Discard() noexcept {
  state_.reset();
}

FontMetrics LinuxRenderer::Metrics(const Font& font) {
  PangoFontDescription* description = CreateFontDescription(font);
  PangoFontMetrics* metrics = pango_context_get_metrics(state_->context, description, nullptr);
  pango_font_description_free(description);
  const float ascent = PangoUnits(pango_font_metrics_get_ascent(metrics));
  const float descent = PangoUnits(pango_font_metrics_get_descent(metrics));
  const float height = PangoUnits(pango_font_metrics_get_height(metrics));
  FontMetrics result{
      .ascent = ascent,
      .descent = descent,
      .leading = std::max(0.0F, height - ascent - descent),
      .underline_position = std::abs(PangoUnits(pango_font_metrics_get_underline_position(metrics))),
      .underline_thickness = std::max(1.0F, PangoUnits(pango_font_metrics_get_underline_thickness(metrics))),
      .strike_through_position = PangoUnits(pango_font_metrics_get_strikethrough_position(metrics)),
      .strike_through_thickness =
          std::max(1.0F, PangoUnits(pango_font_metrics_get_strikethrough_thickness(metrics))),
  };
  pango_font_metrics_unref(metrics);
  return result;
}

TextRunMetrics LinuxRenderer::MeasureRun(
    std::string_view text, const TextStyle& style, const TextShapingOptions& options
) {
  if (text.find_first_of("\r\n") != std::string_view::npos) {
    throw std::invalid_argument("HuxerUI text runs must not contain line breaks");
  }
  PangoLayout* layout = pango_layout_new(state_->context);
  ConfigureLayout(
      layout,
      text,
      style,
      std::numeric_limits<float>::infinity(),
      {.shaping = options, .wrap = TextWrap::NoWrap}
  );
  PangoRectangle ink{};
  PangoRectangle logical{};
  pango_layout_get_extents(layout, &ink, &logical);
  const float baseline = PangoUnits(pango_layout_get_baseline(layout));
  const FontMetrics font_metrics = Metrics(style.font);
  const TextRunMetrics result{
      .advance = PangoUnits(logical.width),
      .visual_bounds = {PangoUnits(ink.x), PangoUnits(ink.y) - baseline, PangoUnits(ink.width), PangoUnits(ink.height)},
      .font_metrics = font_metrics,
  };
  g_object_unref(layout);
  return result;
}

TextLayoutMetrics LinuxRenderer::MeasureText(std::string_view text, const TextStyle& style, float max_width,
    const TextLayoutOptions& options) {
  return MeasureText(AttributedText(std::string(text)), style, max_width, options);
}

std::unique_ptr<TextLayout> LinuxRenderer::CreateTextLayout(std::string_view text, const TextStyle& style,
    float max_width, const TextLayoutOptions& options) {
  return CreateTextLayout(AttributedText(std::string(text)), style, max_width, options);
}

TextLayoutMetrics LinuxRenderer::MeasureText(const AttributedText& text, const TextStyle& style, float max_width,
    const TextLayoutOptions& options) {
  if (std::isfinite(max_width) && max_width <= 0.0F) {
    return {};
  }
  TextLayoutMetrics metrics = state_->TextLayoutFor(text, style, max_width, options).metrics();
  if (std::isfinite(max_width)) {
    metrics.size.width = std::min(metrics.size.width, max_width);
  }
  return metrics;
}

std::unique_ptr<TextLayout> LinuxRenderer::CreateTextLayout(const AttributedText& text, const TextStyle& style,
    float max_width, const TextLayoutOptions& options) {
  return std::make_unique<PangoTextLayout>(state_->context, text, style, max_width, options);
}

void LinuxRenderer::Snapshot(GtkSnapshot* snapshot, const RenderFrame& frame) {
  if (snapshot == nullptr) {
    throw std::invalid_argument("HuxerUI Linux renderer requires a GTK snapshot");
  }
  state_->backdrop_stats = {};
  ValidateBackdropScene(frame.scene);
  if (RenderSceneHasBackdropEffects(frame.scene.root)) {
    SnapshotBackdropScene(*state_, snapshot, frame);
  } else {
    if (state_->backdrop_frame) {
      g_object_unref(state_->backdrop_frame);
      state_->backdrop_frame = nullptr;
      state_->backdrop_root = nullptr;
    }
    state_->backdrop_gl.reset();
    SnapshotScenePainter(*state_, snapshot).Draw(frame.scene);
  }
}

LinuxBackdropStats LinuxRenderer::BackdropStats() const noexcept {
  return state_ ? state_->backdrop_stats : LinuxBackdropStats{};
}

void LinuxRenderer::SetDeviceScale(float scale) {
  if (!std::isfinite(scale) || scale <= 0.0F) {
    throw std::invalid_argument("HuxerUI Linux renderer scale must be finite and positive");
  }
  state_->device_scale = scale;
}

void LinuxRenderer::SetViewport(Size viewport) {
  if (!std::isfinite(viewport.width) || !std::isfinite(viewport.height) ||
      viewport.width < 0.0F || viewport.height < 0.0F) {
    throw std::invalid_argument("HuxerUI Linux renderer viewport must be finite and non-negative");
  }
  state_->viewport = viewport;
}

void LinuxRenderer::SetDisplay(::GdkDisplay* display) {
  if (state_->display != display) {
    state_->backdrop_gl.reset();
    state_->backdrop_gl_unavailable = false;
    if (state_->backdrop_frame != nullptr) {
      g_object_unref(state_->backdrop_frame);
      state_->backdrop_frame = nullptr;
      state_->backdrop_root = nullptr;
    }
  }
  state_->display = display;
}

void LinuxRenderer::SnapshotSlice(GtkSnapshot* snapshot, const RenderFrame& frame, std::size_t first_command,
                                 std::size_t command_count) {
  if (snapshot == nullptr) {
    throw std::invalid_argument("HuxerUI Linux renderer requires a GTK snapshot");
  }
  ValidateBackdropSceneSlice(frame.scene, first_command, command_count);
  if (command_count == 0) {
    return;
  }
  if (RenderSceneHasBackdropEffects(frame.scene.root)) {
    if (first_command != 0) {
      throw std::logic_error("HuxerUI backdrop effects cannot sample native drawing slices");
    }
    Snapshot(snapshot, frame);
    return;
  }
  SnapshotScenePainter(*state_, snapshot, std::pair{first_command, command_count}).Draw(frame.scene);
}

} // namespace huxerui::detail
