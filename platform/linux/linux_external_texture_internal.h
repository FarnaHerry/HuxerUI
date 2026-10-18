#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <gdk/gdk.h>

#include <huxerui/linux/external_texture.h>

namespace huxerui::detail {

[[nodiscard]] bool SupportsLinuxGlVersion(GdkGLAPI api, int major, int minor) noexcept;

// Borrowed immutable snapshot metadata; the enclosing GDK texture owns the GL resources and context.
struct LinuxGlSnapshot {
  ::GdkGLContext* context = nullptr;
  std::uint32_t texture = 0;
  bool premultiplied = false;
};

class LinuxGdkTextureFrame final {
public:
  explicit LinuxGdkTextureFrame(
      ::GdkTexture* texture, linux::GlTexture::Origin origin = linux::GlTexture::Origin::TopLeft,
      LinuxGlSnapshot snapshot = {}
  )
      : texture_(GDK_TEXTURE(g_object_ref(texture))), origin_(origin), snapshot_(snapshot) {}

  ~LinuxGdkTextureFrame() {
    g_object_unref(texture_);
  }

  LinuxGdkTextureFrame(const LinuxGdkTextureFrame&) = delete;
  LinuxGdkTextureFrame& operator=(const LinuxGdkTextureFrame&) = delete;

  [[nodiscard]] ::GdkTexture* Texture() const noexcept {
    return texture_;
  }

  [[nodiscard]] linux::GlTexture::Origin TextureOrigin() const noexcept {
    return origin_;
  }

  [[nodiscard]] const LinuxGlSnapshot& GlSnapshot() const noexcept { return snapshot_; }

private:
  ::GdkTexture* texture_ = nullptr;
  linux::GlTexture::Origin origin_ = linux::GlTexture::Origin::TopLeft;
  LinuxGlSnapshot snapshot_;
};

class LinuxPixelFrame final {
public:
  LinuxPixelFrame() noexcept = default;
  LinuxPixelFrame(int pixel_width, int pixel_height, std::vector<std::byte> pixels)
      : pixel_width_(pixel_width), pixel_height_(pixel_height), pixels_(std::move(pixels)) {}

  [[nodiscard]] int PixelWidth() const noexcept {
    return pixel_width_;
  }

  [[nodiscard]] int PixelHeight() const noexcept {
    return pixel_height_;
  }

  [[nodiscard]] std::size_t BytesPerRow() const noexcept {
    return static_cast<std::size_t>(pixel_width_) * 4U;
  }

  [[nodiscard]] std::span<const std::byte> Pixels() const noexcept {
    return pixels_;
  }

private:
  int pixel_width_ = 0;
  int pixel_height_ = 0;
  std::vector<std::byte> pixels_;
};

} // namespace huxerui::detail
