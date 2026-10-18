#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include <huxerui/effects.h>
#include <huxerui/geometry.h>
#include <huxerui/paint.h>

namespace huxerui::detail {

// One deferred glass command together with the mapping from its local coordinate space to device
// pixels at its recording position. The mapping includes the capture surface origin, so transformed
// command bounds are directly comparable with capture rectangles.
struct BackdropPlaneCommand {
  const DrawBackdropCommand* command = nullptr;
  Transform2D local_to_device;
};

// A batch of glass commands that share one source capture. Batch members never sample another
// member's optical output and no interleaved raster work intersects the sampling union, so processing
// every member from a single capture taken at the first member's paint position reproduces immediate
// per-command replay exactly.
struct BackdropPlaneBatch {
  std::vector<BackdropPlaneCommand> commands;
  Rect sampling_bounds;  // Device-pixel union of member sampling bounds, including the caller margin.
  Rect output_bounds;    // Device-pixel union of member optical outputs.
};

// Collects consecutive glass commands into shared-capture batches during renderer replay.
// Batches keep pointers to the added commands; callers keep the owning PaintSequences alive until
// the batch is flushed.
class BackdropPlaneCollector {
public:
  static constexpr std::size_t maximum_commands = 16;
  static constexpr std::size_t maximum_shapes = 256;

  // Returns false when the command cannot join the pending batch because it would sample a member's
  // optical output or a budget is exhausted; the caller flushes the batch and adds the command again.
  // A command always joins an empty batch. The margin enlarges stored sampling bounds beyond the
  // declared blur and refraction support so renderer capture policy stays conservative.
  bool TryAdd(const DrawBackdropCommand& command, Transform2D local_to_device, float device_margin);
  // Returns true when raster work intersecting the given device bounds must flush the pending batch
  // first, because a deferred member would otherwise capture that work as its source.
  bool BlockedBy(Rect device_bounds) const noexcept;
  [[nodiscard]] bool empty() const noexcept { return batch_.commands.empty(); }
  [[nodiscard]] const BackdropPlaneBatch& batch() const noexcept { return batch_; }
  void Clear() noexcept;

private:
  BackdropPlaneBatch batch_;
  std::size_t shapes_ = 0;
};

// Conservative local-space paint bounds of one command, used to decide whether interleaved drawing
// can affect a pending capture. Structural commands report no bounds and must flush conservatively.
[[nodiscard]] std::optional<Rect> ConservativeRasterBounds(const PaintCommand& command);

} // namespace huxerui::detail
