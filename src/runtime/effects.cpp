#include <huxerui/effects.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#include <huxerui/animation.h>
#include <huxerui/modifier.h>

#include "view_internal.h"
#include "graphics/effects_internal.h"

namespace huxerui {
namespace {

void RequireNoRetainedBackdrop(const detail::ViewSpec& spec) {
  const bool retained = std::any_of(spec.modifiers.begin(), spec.modifiers.end(), [](const auto& modifier) {
    return modifier.descriptor == &LiquidGlass::Descriptor() || modifier.descriptor == &GlassGroup::Descriptor();
  });
  if (retained) {
    throw std::invalid_argument("HuxerUI a node supports one retained glass material and no later static backdrop");
  }
}

class LiquidGlassExtension final : public NodeExtension {
public:
  LiquidGlassExtension(ViewNode& node, const LiquidGlass& value) {
    Update(node, value);
  }

  void Update(ViewNode& node, const LiquidGlass& value) {
    detail::ValidateBackdropEffect(value.effect);
    effect_ = value.effect;
    Retarget(node.Interaction());
    InvalidatePaint(PaintInvalidation::Content);
  }

  void OnInteraction(ViewNode&, const InteractionState& state, const std::optional<InteractionEvent>&) override {
    Retarget(state);
    InvalidatePaint(PaintInvalidation::Content);
  }

  FrameResult OnFrame(ViewNode& node, const FrameInfo& frame) override {
    Retarget(node.Interaction());
    const float previous = interaction_.Value();
    const auto result = interaction_.Advance(frame);
    if (interaction_.Value() != previous) { InvalidatePaint(PaintInvalidation::Content); }
    return {result.needs_frame, result.wake_after};
  }

  void PaintBackdrop(const ViewNode& node, PaintContext& context) const override {
    BackdropEffect effect = effect_;
    const float progress = std::clamp(interaction_.Value(), 0.0F, 1.0F);
    effect.refraction *= 1.0F + 0.45F * progress;
    effect.edge_width *= 1.0F + 0.2F * progress;
    effect.highlight += (1.0F - effect.highlight) * 0.15F * progress;
    context.DrawBackdrop(node.Bounds(), effect, node.ResolvedCornerRadii());
  }

private:
  void Retarget(const InteractionState& state) {
    const float target = !state.enabled ? 0.0F : state.pressed ? 1.0F : state.hovered ? 0.35F : 0.0F;
    if (!state.enabled) {
      interaction_.Set(0.0F);
    } else if (target != target_) {
      interaction_.AnimateTo(target, SpringSpec{});
    }
    target_ = target;
  }

  BackdropEffect effect_;
  MotionController interaction_;
  float target_ = 0.0F;
};

class GlassGroupExtension final : public NodeExtension {
public:
  GlassGroupExtension(ViewNode& node, const GlassGroup& value) { Update(node, value); }

  void Update(ViewNode&, const GlassGroup& value) {
    detail::ValidateBackdropEffect(value.effect);
    effect_ = value.effect;
    InvalidatePaint(PaintInvalidation::Content);
  }

  PaintInvalidation PrepareGeometry(ViewNode& node, TextMeasurer&) override {
    std::vector<BackdropShape> shapes;
    for (std::size_t index = 0; index < node.ChildCount(); ++index) {
      const auto& child = node.ChildAt(index);
      if (child.PresentationOpacity() <= 0.0F || child.Bounds().IsEmpty()) { continue; }
      const Rect bounds = child.Bounds();
      const auto origin = node.WindowToLocal(child.LocalToWindow({bounds.x, bounds.y}));
      const auto right = node.WindowToLocal(child.LocalToWindow({bounds.x + bounds.width, bounds.y}));
      const auto bottom = node.WindowToLocal(child.LocalToWindow({bounds.x, bounds.y + bounds.height}));
      if (!origin || !right || !bottom) {
        throw std::logic_error("HuxerUI glass groups require invertible child presentation geometry");
      }
      constexpr float tolerance = 0.001F;
      if (std::abs(right->y - origin->y) > tolerance || std::abs(bottom->x - origin->x) > tolerance ||
          right->x <= origin->x || bottom->y <= origin->y) {
        throw std::logic_error("HuxerUI glass groups require axis-aligned, non-reflected child shapes");
      }
      const float scale_x = (right->x - origin->x) / bounds.width;
      const float scale_y = (bottom->y - origin->y) / bounds.height;
      if (std::abs(scale_x - scale_y) > tolerance) {
        throw std::logic_error("HuxerUI glass groups require uniform child scaling for rounded shapes");
      }
      auto radii = child.ResolvedCornerRadii();
      radii.top_left *= scale_x;
      radii.top_right *= scale_x;
      radii.bottom_right *= scale_x;
      radii.bottom_left *= scale_x;
      shapes.push_back({{origin->x, origin->y, right->x - origin->x, bottom->y - origin->y}, radii});
    }
    if (shapes == shapes_) { return PaintInvalidation::None; }
    shapes_ = std::move(shapes);
    return PaintInvalidation::Content;
  }

  void PaintBackdrop(const ViewNode&, PaintContext& context) const override {
    if (!shapes_.empty()) { context.DrawBackdrop(shapes_, effect_); }
  }

private:
  BackdropEffect effect_;
  std::vector<BackdropShape> shapes_;
};

} // namespace

const detail::ModifierDescriptor& BackdropBlur::Descriptor() {
  static const detail::ModifierDescriptor descriptor{
      [](detail::ViewSpec& spec, detail::ModifierSpec& modifier, const std::shared_ptr<const Environment>&,
         detail::AppResources&) {
        const auto& value = *static_cast<const BackdropBlur*>(modifier.value.get());
        BackdropEffect effect{.blur_radius = value.radius};
        detail::ValidateBackdropEffect(effect);
        RequireNoRetainedBackdrop(spec);
        spec.properties.backdrop_effect = effect;
        spec.properties.backdrop_override = true;
      },
      nullptr,
      nullptr,
      false,
      detail::ErasedEqualsFor<BackdropBlur>(),
  };
  return descriptor;
}

const detail::ModifierDescriptor& BackdropFilter::Descriptor() {
  static const detail::ModifierDescriptor descriptor{
      [](detail::ViewSpec& spec, detail::ModifierSpec& modifier, const std::shared_ptr<const Environment>&,
         detail::AppResources&) {
        const auto& value = *static_cast<const BackdropFilter*>(modifier.value.get());
        detail::ValidateBackdropEffect(value.effect);
        RequireNoRetainedBackdrop(spec);
        spec.properties.backdrop_effect = value.effect;
        spec.properties.backdrop_override = true;
      },
      nullptr,
      nullptr,
      false,
      detail::ErasedEqualsFor<BackdropFilter>(),
  };
  return descriptor;
}

const detail::ModifierDescriptor& LiquidGlass::Descriptor() {
  static const detail::ModifierDescriptor descriptor{
      [](detail::ViewSpec& spec, detail::ModifierSpec& modifier, const std::shared_ptr<const Environment>&,
         detail::AppResources&) {
        detail::ValidateBackdropEffect(static_cast<const LiquidGlass*>(modifier.value.get())->effect);
        RequireNoRetainedBackdrop(spec);
        spec.properties.backdrop_effect.reset();
        spec.properties.backdrop_override = true;
      },
      [](ViewNode& node, const void* value) -> std::unique_ptr<NodeExtension> {
        return std::make_unique<LiquidGlassExtension>(node, *static_cast<const LiquidGlass*>(value));
      },
      [](NodeExtension& extension, ViewNode& node, const void* value) {
        static_cast<LiquidGlassExtension&>(extension).Update(node, *static_cast<const LiquidGlass*>(value));
      },
      false,
      detail::ErasedEqualsFor<LiquidGlass>(),
  };
  return descriptor;
}

const detail::ModifierDescriptor& GlassGroup::Descriptor() {
  static const detail::ModifierDescriptor descriptor{
      [](detail::ViewSpec& spec, detail::ModifierSpec& modifier, const std::shared_ptr<const Environment>&,
         detail::AppResources&) {
        detail::ValidateBackdropEffect(static_cast<const GlassGroup*>(modifier.value.get())->effect);
        RequireNoRetainedBackdrop(spec);
        spec.properties.backdrop_effect.reset();
        spec.properties.backdrop_override = true;
      },
      [](ViewNode& node, const void* value) -> std::unique_ptr<NodeExtension> {
        return std::make_unique<GlassGroupExtension>(node, *static_cast<const GlassGroup*>(value));
      },
      [](NodeExtension& extension, ViewNode& node, const void* value) {
        static_cast<GlassGroupExtension&>(extension).Update(node, *static_cast<const GlassGroup*>(value));
      },
      false,
      detail::ErasedEqualsFor<GlassGroup>(),
  };
  return descriptor;
}

} // namespace huxerui
