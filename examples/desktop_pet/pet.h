#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

#include <huxerui/huxerui.h>

namespace desktop_pet {

inline void PaintOrb(huxerui::PaintContext& paint, huxerui::Size size, float phase, bool energized) {
  using namespace huxerui;
  const Point center{size.width * 0.5F, size.height * 0.5F};
  const float radius = std::min(size.width, size.height) * 0.37F;
  if (radius <= 0.0F) return;
  const Rect bounds{center.x - radius, center.y - radius, radius * 2.0F, radius * 2.0F};
  const Path circle = Path::RoundedRect(bounds, CornerRadii{radius});
  const float intensity = energized ? 1.0F : 0.65F;

  paint.DrawRect({center.x - radius * 1.24F, center.y - radius * 1.24F, radius * 2.48F, radius * 2.48F},
      RadialGradient{.stops = {
          {0.0F, Color::Rgb(66, 110, 255, 0.12F * intensity)},
          {0.70F, Color::Rgb(97, 79, 255, 0.065F * intensity)},
          {1.0F, Color::Transparent()},
      }});
  paint.FillPath(circle, RadialGradient{
      .center = {0.30F, 0.16F},
      .radius = {0.95F, 0.95F},
      .stops = {
          {0.0F, Color::Rgb(39, 59, 92, 0.98F)},
          {0.48F, Color::Rgb(13, 22, 47, 0.98F)},
          {1.0F, Color::Rgb(6, 10, 28, 0.96F)},
      },
  }, bounds);

  paint.PushPathClip(circle);
  paint.DrawRect(bounds, RadialGradient{
      .center = {0.48F, 0.57F},
      .radius = {0.65F, 0.43F},
      .stops = {
          {0.0F, Color::Rgb(70, 146, 255, 0.28F * intensity)},
          {0.65F, Color::Rgb(102, 70, 241, 0.10F * intensity)},
          {1.0F, Color::Transparent()},
      },
  });

  const auto ribbon = [&](float offset, float amplitude, float thickness, float baseline) {
    const auto point = [&](float x, float side) {
      const float envelope = std::sqrt(std::max(0.0F, 1.0F - x * x));
      const float wave = std::sin(x * 3.4F + phase + offset) * amplitude +
                         std::sin(x * 5.2F - phase * 0.7F + offset) * 0.035F;
      return Point{center.x + x * radius,
                   center.y + radius * (baseline + (wave + side * thickness) * envelope)};
    };
    Path path;
    path.MoveTo(point(-1.0F, -1.0F));
    constexpr int samples = 64;
    for (int index = 1; index <= samples; ++index) {
      path.LineTo(point(-1.0F + 2.0F * static_cast<float>(index) / samples, -1.0F));
    }
    for (int index = samples; index >= 0; --index) {
      path.LineTo(point(-1.0F + 2.0F * static_cast<float>(index) / samples, 1.0F));
    }
    return path.Close();
  };

  const float amplitude = energized ? 0.30F : 0.18F;
  paint.FillPath(ribbon(1.7F, amplitude, 0.22F, 0.03F), LinearGradient{
      .start = {0.0F, 0.15F}, .end = {1.0F, 0.85F},
      .stops = {
          {0.0F, Color::Rgb(26, 214, 224, 0.08F)},
          {0.34F, Color::Rgb(35, 174, 255, 0.46F * intensity)},
          {0.74F, Color::Rgb(127, 89, 255, 0.52F * intensity)},
          {1.0F, Color::Rgb(222, 121, 251, 0.10F)},
      },
  }, bounds);
  paint.FillPath(ribbon(-1.1F, amplitude * 0.85F, 0.13F, 0.10F), LinearGradient{
      .stops = {
          {0.0F, Color::Rgb(74, 240, 218, 0.12F)},
          {0.30F, Color::Rgb(62, 224, 247, 0.64F * intensity)},
          {0.68F, Color::Rgb(99, 123, 255, 0.62F * intensity)},
          {1.0F, Color::Rgb(179, 100, 255, 0.14F)},
      },
  }, bounds);
  paint.FillPath(ribbon(0.0F, amplitude * 0.9F, 0.045F, 0.0F), LinearGradient{
      .stops = {
          {0.0F, Color::Rgb(81, 246, 230, 0.08F)},
          {0.27F, Color::Rgb(134, 255, 243, 0.92F * intensity)},
          {0.53F, Color::Rgb(140, 205, 255, 0.90F * intensity)},
          {0.80F, Color::Rgb(176, 143, 255, 0.82F * intensity)},
          {1.0F, Color::Rgb(226, 159, 255, 0.06F)},
      },
  }, bounds);
  paint.DrawRect(bounds, RadialGradient{
      .center = {0.32F, 0.06F}, .radius = {0.68F, 0.48F},
      .stops = {{0.0F, Color::Rgb(218, 237, 255, 0.16F)}, {1.0F, Color::Transparent()}},
  });
  paint.PopClip();

  paint.StrokePath(circle, LinearGradient{
      .start = {0.15F, 0.0F}, .end = {0.85F, 1.0F},
      .stops = {
          {0.0F, Color::Rgb(204, 235, 255, 0.52F)},
          {0.35F, Color::Rgb(94, 191, 241, 0.20F)},
          {0.72F, Color::Rgb(135, 108, 236, 0.12F)},
          {1.0F, Color::Rgb(172, 157, 247, 0.32F)},
      },
  }, bounds, StrokeStyle{.width = 0.8F});
  paint.DrawArc(center, radius * 0.94F, 3.80F, 0.86F,
      Color::Rgb(209, 236, 255, 0.28F), StrokeStyle{.width = 0.8F, .cap = StrokeCap::Round});
}

struct Orb {
  class Extension;
  bool energized = false;
  bool animate = true;
  bool operator==(const Orb&) const = default;
};

class Orb::Extension final : public huxerui::NodeExtension {
public:
  Extension(huxerui::ViewNode& node, const Orb& orb) { Update(node, orb); }

  void Update(huxerui::ViewNode&, const Orb& orb) {
    if (orb_ != orb) {
      orb_ = orb;
      InvalidatePaint();
    }
  }

  FrameResult OnFrame(huxerui::ViewNode&, const huxerui::FrameInfo& frame) override {
    if (frame.reduced_motion || !orb_.animate) return {};
    const float delta = static_cast<float>(std::clamp(frame.delta_time, 0.0, 0.1));
    if (delta > 0.0F) {
      constexpr float wave_cycle = 20.0F * std::numbers::pi_v<float>;
      phase_ = std::fmod(phase_ + delta * (orb_.energized ? 1.6F : 0.7F), wave_cycle);
      InvalidatePaint();
    }
    return {.wake_after = 1.0 / 30.0};
  }

  void PaintAboveContent(const huxerui::ViewNode& node, huxerui::PaintContext& paint) const override {
    PaintOrb(paint, node.LayoutSize(), phase_, orb_.energized);
  }

private:
  Orb orb_;
  float phase_ = 0.0F;
};

} // namespace desktop_pet
