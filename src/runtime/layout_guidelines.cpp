#include "runtime_internal.h"
#include "mounted_node_internal.h"

#include <cstdint>
#include <limits>

#include <huxerui/paint.h>

namespace huxerui::detail {

namespace {

// Mounted node identities count upward from 1 and the text selection overlay owns 0.
constexpr std::uint64_t kLayoutGuidelinesOverlayId = std::numeric_limits<std::uint64_t>::max();

void PaintNodeGuides(PaintContext& context, const MountedNode& node) {
  const Rect bounds = node.LocalToWindowBounds(node.Bounds());
  if (!bounds.IsEmpty()) {
    context.DrawBorder(bounds, Color::Rgb(0, 188, 212), StrokeStyle{.width = 1.0F});
    const Rect content = node.LocalToWindowBounds(node.ContentBounds());
    if (content != bounds && !content.IsEmpty()) {
      context.DrawBorder(content, Color::Rgb(255, 179, 0), StrokeStyle{.width = 1.0F, .dash_pattern = {3.0F, 2.0F}});
    }
  }
  for (const auto& child : node.children) {
    PaintNodeGuides(context, *child);
  }
}

} // namespace

void PaintLayoutGuidelinesOverlay(
    LayoutGuidelinesOverlay& overlay,
    const MountedNode* mounted_root,
    Size viewport
) {
  RenderNode& render_node = overlay.render_node;
  render_node.id = kLayoutGuidelinesOverlayId;
  render_node.offset = {};
  render_node.transform = {};
  render_node.opacity = 1.0F;
  render_node.child_clips.clear();
  render_node.children.clear();
  ++render_node.revision;
  const Rect bounds{0.0F, 0.0F, viewport.width, viewport.height};
  PaintContext context{render_node.content, bounds};
  if (mounted_root != nullptr) {
    PaintNodeGuides(context, *mounted_root);
  }
  context.Finish();
  render_node.visible = !render_node.content.Bounds().IsEmpty();
}

} // namespace huxerui::detail
