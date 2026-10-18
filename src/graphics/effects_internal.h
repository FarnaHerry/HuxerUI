#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <huxerui/effects.h>
#include <huxerui/paint.h>

namespace huxerui {
struct RenderNode;
struct RenderScene;
}

namespace huxerui::detail {

inline constexpr std::size_t maximum_backdrop_capture_pixels = 16U * 1024U * 1024U;
inline constexpr std::size_t maximum_backdrop_group_bytes = 256U * 1024U * 1024U;

struct BackdropPixels {
  std::vector<std::uint8_t> bgra;
  std::size_t width = 0;
  std::size_t height = 0;
  std::size_t stride = 0;
};

bool RenderSceneHasBackdropEffects(const RenderNode* node);
void ValidateBackdropScene(const RenderScene& scene);
void ValidateBackdropSceneSlice(const RenderScene& scene, std::size_t first_command, std::size_t command_count);

void ValidateBackdropEffect(const BackdropEffect& effect);
void ValidateBackdropShapes(std::span<const BackdropShape> shapes);
void ValidateBackdropCommand(const DrawBackdropCommand& command);
Rect BackdropBounds(const DrawBackdropCommand& command) noexcept;
float BackdropSamplingOutset(const BackdropEffect& effect) noexcept;
Rect BackdropSamplingBounds(const DrawBackdropCommand& command) noexcept;

// Pixel coordinates identify pixel edges. Output contains the full updated prefix, unchanged outside the surface.
// Captures include surrounding support. The caller replaces the active target under its current clip; source-over
// against the original capture would apply its alpha twice. Capture and compositing isolation remain caller-owned.
BackdropPixels ApplyBackdropEffect(std::span<const std::uint8_t> bgra, std::size_t width, std::size_t height,
                                  std::size_t stride, Transform2D pixel_to_local,
                                  const DrawBackdropCommand& command);

} // namespace huxerui::detail
