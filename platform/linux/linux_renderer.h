#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

#include <gtk/gtk.h>

#include <huxerui/render_scene.h>
#include <huxerui/text.h>

namespace huxerui::detail {

class TextLayout;

struct LinuxBackdropStats {
  std::size_t plane_count = 0;
  std::size_t command_count = 0;
  std::size_t gpu_plane_count = 0;
  std::size_t source_capture_count = 0;
  std::size_t blur_pass_count = 0;
  std::size_t blur_cache_hits = 0;
  std::size_t raster_upload_bytes = 0;
  std::size_t readback_bytes = 0;
  std::size_t mask_upload_bytes = 0;
  std::size_t mask_rasterizations = 0;
  std::size_t external_texture_imports = 0;
  std::size_t texture_download_bytes = 0;
};

// Records the platform-neutral scene into GTK Snapshot nodes, using Cairo nodes
// for ordinary drawing and texture nodes for GPU-backed external frames.
class LinuxRenderer final {
public:
  LinuxRenderer();
  ~LinuxRenderer();

  LinuxRenderer(const LinuxRenderer&) = delete;
  LinuxRenderer& operator=(const LinuxRenderer&) = delete;

  void Initialize();
  void Discard() noexcept;
  void SetDeviceScale(float scale);
  void SetDisplay(::GdkDisplay* display);
  void SetViewport(Size viewport);

  [[nodiscard]] FontMetrics Metrics(const Font& font);
  [[nodiscard]] TextRunMetrics
  MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options);
  [[nodiscard]] TextLayoutMetrics
  MeasureText(std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options = {});
  [[nodiscard]] std::unique_ptr<TextLayout> CreateTextLayout(std::string_view text, const TextStyle& style,
      float max_width, const TextLayoutOptions& options = {});
  [[nodiscard]] TextLayoutMetrics
  MeasureText(const AttributedText& text, const TextStyle& style, float max_width, const TextLayoutOptions& options);
  [[nodiscard]] std::unique_ptr<TextLayout> CreateTextLayout(const AttributedText& text, const TextStyle& style,
      float max_width, const TextLayoutOptions& options);

  void Snapshot(GtkSnapshot* snapshot, const RenderFrame& frame);
  void SnapshotSlice(GtkSnapshot* snapshot, const RenderFrame& frame, std::size_t first_command,
                     std::size_t command_count);
  [[nodiscard]] LinuxBackdropStats BackdropStats() const noexcept;

public:
  struct State;

private:
  std::unique_ptr<State> state_;
};

} // namespace huxerui::detail
