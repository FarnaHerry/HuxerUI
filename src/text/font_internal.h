#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <huxerui/text.h>

namespace huxerui::detail {

struct FontFaceData {
  Bytes bytes;
  std::string family_name;
  std::uint16_t weight = 400;
  std::uint16_t width = 5;
  FontSlant slant = FontSlant::Normal;
  // Native cache and registration key only; public equality and hashing use the owned contents.
  std::uint64_t identity = 0;
  std::size_t hash = 0;
};

struct FontFamilyData {
  std::vector<std::shared_ptr<const FontFaceData>> faces;
  std::size_t hash = 0;
};

[[nodiscard]] std::shared_ptr<const FontFaceData> ResolveFontFace(const Font& font);
[[nodiscard]] std::size_t HashFontFamily(const FontFamily& family) noexcept;

} // namespace huxerui::detail
