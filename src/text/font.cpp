#include <huxerui/text.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <span>
#include <stdexcept>
#include <tuple>
#include <utility>

#include <huxerui/app.h>
#include <huxerui/file.h>
#include <huxerui/resource.h>

#include "font_internal.h"
#include "internal_access.h"
#include "resources/resource_internal.h"

namespace huxerui {
namespace {

[[noreturn]] void InvalidFont() {
  throw std::invalid_argument("HuxerUI font must contain valid static, single-face OpenType or TrueType data");
}

std::span<const std::byte> Slice(std::span<const std::byte> bytes, std::size_t offset, std::size_t size) {
  if (offset > bytes.size() || size > bytes.size() - offset)
    InvalidFont();
  return bytes.subspan(offset, size);
}

std::uint16_t U16(std::span<const std::byte> bytes, std::size_t offset) {
  const auto value = Slice(bytes, offset, 2);
  return (std::to_integer<unsigned>(value[0]) << 8U) | std::to_integer<unsigned>(value[1]);
}

std::uint32_t U32(std::span<const std::byte> bytes, std::size_t offset) {
  return (static_cast<std::uint32_t>(U16(bytes, offset)) << 16U) | U16(bytes, offset + 2);
}

constexpr std::uint32_t Tag(const char (&name)[5]) {
  return (static_cast<std::uint32_t>(name[0]) << 24U) | (static_cast<std::uint32_t>(name[1]) << 16U) |
         (static_cast<std::uint32_t>(name[2]) << 8U) | static_cast<std::uint32_t>(name[3]);
}

std::string Utf16Name(std::span<const std::byte> bytes) {
  if (bytes.size() % 2 != 0)
    InvalidFont();
  std::string value;
  for (std::size_t index = 0; index < bytes.size(); index += 2) {
    std::uint32_t code = U16(bytes, index);
    if (code >= 0xd800 && code <= 0xdbff) {
      index += 2;
      const auto tail = U16(bytes, index);
      if (tail < 0xdc00 || tail > 0xdfff)
        InvalidFont();
      code = 0x10000 + ((code - 0xd800) << 10U) + tail - 0xdc00;
    } else if (code >= 0xdc00 && code <= 0xdfff) {
      InvalidFont();
    }
    if (code == 0)
      InvalidFont();
    if (code < 0x80) {
      value.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      value.push_back(static_cast<char>(0xc0 | (code >> 6U)));
      value.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    } else if (code < 0x10000) {
      value.push_back(static_cast<char>(0xe0 | (code >> 12U)));
      value.push_back(static_cast<char>(0x80 | ((code >> 6U) & 0x3f)));
      value.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    } else {
      value.push_back(static_cast<char>(0xf0 | (code >> 18U)));
      value.push_back(static_cast<char>(0x80 | ((code >> 12U) & 0x3f)));
      value.push_back(static_cast<char>(0x80 | ((code >> 6U) & 0x3f)));
      value.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    }
  }
  return value;
}

std::string FontName(std::span<const std::byte> table, std::uint16_t name_id) {
  if (U16(table, 0) > 1)
    InvalidFont();
  const auto count = U16(table, 2);
  const auto strings = U16(table, 4);
  Slice(table, 6, static_cast<std::size_t>(count) * 12);
  if (strings < 6 + count * 12U || strings > table.size())
    InvalidFont();
  std::string result;
  auto best = std::tuple{4U, 0xffffU};
  for (unsigned index = 0; index < count; ++index) {
    const auto offset = 6 + index * 12;
    const auto platform = U16(table, offset);
    const auto encoding = U16(table, offset + 2);
    const auto language = U16(table, offset + 4);
    const auto id = U16(table, offset + 6);
    const auto value = Slice(table, strings + U16(table, offset + 10), U16(table, offset + 8));
    if (id != name_id)
      continue;
    const bool unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
    const bool ascii_mac = platform == 1 && encoding == 0 && std::ranges::all_of(value, [](std::byte b) {
                             return b > std::byte{0} && b < std::byte{128};
                           });
    if (!unicode && !ascii_mac)
      continue;
    const auto rank = std::tuple{platform == 3 && language == 0x0409 ? 0U
                                 : unicode                           ? 1U
                                                                     : 2U,
                                 static_cast<unsigned>(language)};
    if (rank >= best || value.empty())
      continue;
    result = unicode ? Utf16Name(value) : std::string(reinterpret_cast<const char*>(value.data()), value.size());
    best = rank;
  }
  return result;
}

void CombineHash(std::size_t& result, std::size_t value) noexcept {
  result ^= value + 0x9e3779b9U + (result << 6U) + (result >> 2U);
}

// Shared parsing validates family metadata and table bounds; native engines still validate glyph data.
std::shared_ptr<const detail::FontFaceData> ParseFace(Bytes bytes) {
  const std::span<const std::byte> input(bytes);
  const auto signature = U32(input, 0);
  if (signature != 0x00010000 && signature != Tag("OTTO"))
    InvalidFont();
  const auto count = U16(input, 4);
  if (count == 0)
    InvalidFont();
  const std::size_t directory_end = 12 + static_cast<std::size_t>(count) * 16;
  Slice(input, 0, directory_end);
  std::map<std::uint32_t, std::span<const std::byte>> tables;
  for (unsigned index = 0; index < count; ++index) {
    const auto record = 12 + index * 16;
    const auto offset = U32(input, record + 8);
    const auto length = U32(input, record + 12);
    if (offset < directory_end || !tables.emplace(U32(input, record), Slice(input, offset, length)).second) {
      InvalidFont();
    }
  }
  if (tables.contains(Tag("fvar")) || tables.contains(Tag("CFF2")))
    InvalidFont();
  const auto table = [&](std::uint32_t tag) -> std::span<const std::byte> {
    const auto found = tables.find(tag);
    if (found == tables.end())
      InvalidFont();
    return found->second;
  };
  const auto head = table(Tag("head"));
  Slice(head, 0, 54);
  if (U32(head, 12) != 0x5f0f3cf5 || U16(head, 18) < 16 || U16(head, 18) > 16384)
    InvalidFont();
  if (U16(table(Tag("maxp")), 4) == 0)
    InvalidFont();
  Slice(table(Tag("cmap")), 0, 4);
  if (signature == Tag("OTTO")) {
    Slice(table(Tag("CFF ")), 0, 4);
  } else {
    table(Tag("glyf"));
    table(Tag("loca"));
  }
  const auto os2 = table(Tag("OS/2"));
  Slice(os2, 0, 68);
  const auto weight = U16(os2, 4);
  const auto width = U16(os2, 6);
  if (weight == 0 || weight > 1000 || width == 0 || width > 9)
    InvalidFont();
  const auto names = table(Tag("name"));
  // Typographic family names keep styles together when legacy family names split them into separate families.
  auto family = FontName(names, 16);
  auto native_family = FontName(names, 1);
  if (family.empty())
    family = native_family;
  if (family.empty() || native_family.empty())
    InvalidFont();
  static std::atomic<std::uint64_t> next_identity{1};
  auto face = std::make_shared<detail::FontFaceData>();
  face->family_name = std::move(family);
  face->weight = weight;
  face->width = width;
  face->slant = (U16(os2, 62) & 0x201U) != 0 ? FontSlant::Italic : FontSlant::Normal;
  face->identity = next_identity.fetch_add(1, std::memory_order_relaxed);
  face->hash =
      std::hash<std::string_view>{}(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  face->bytes = std::move(bytes);
  return face;
}

// CSS weight matching gives 400..500 a special search order instead of choosing the nearest numeric weight.
std::pair<unsigned, unsigned> WeightRank(unsigned requested, unsigned available) {
  if (requested < 400)
    return available <= requested ? std::pair{0U, requested - available} : std::pair{1U, available - requested};
  if (requested > 500)
    return available >= requested ? std::pair{0U, available - requested} : std::pair{1U, requested - available};
  if (available >= requested && available <= 500)
    return {0U, available - requested};
  if (available < requested)
    return {1U, requested - available};
  return {2U, available - 500};
}

} // namespace

FontFamily FontFamily::System() {
  return {};
}

FontFamily FontFamily::Monospace() {
  FontFamily result;
  result.kind_ = FontFamilyKind::Monospace;
  return result;
}

FontFamily FontFamily::Named(std::string name) {
  if (name.empty())
    throw std::invalid_argument("HuxerUI named font family must not be empty");
  FontFamily result;
  result.kind_ = FontFamilyKind::Named;
  result.name_ = std::move(name);
  return result;
}

FontFamily FontFamily::FromResources(std::vector<RawResource> resources) {
  if (resources.empty())
    throw std::invalid_argument("HuxerUI custom font family must contain at least one file");
  const auto service = UseService<Resources>();
  service->RequireLifetime();
  return service->resources_->ResolveFontFamily(std::move(resources));
}

FontFamily FontFamily::FromFiles(std::vector<File> files) {
  std::vector<Bytes> bytes;
  bytes.reserve(files.size());
  for (const auto& file : files) {
    auto read = file.ReadBytes();
    if (!read.Succeeded()) {
      throw std::runtime_error("HuxerUI font file could not be read: " + file.Path() + ": " + read.Error().message);
    }
    bytes.push_back(std::move(read).Value());
  }
  return FromBytes(std::move(bytes));
}

FontFamily FontFamily::FromBytes(std::vector<Bytes> files) {
  if (files.empty())
    throw std::invalid_argument("HuxerUI custom font family must contain at least one file");
  auto data = std::make_shared<detail::FontFamilyData>();
  for (auto& bytes : files)
    data->faces.push_back(ParseFace(std::move(bytes)));
  // Canonical style order makes equality and hashing independent of source-file order.
  std::ranges::sort(data->faces, {}, [](const auto& face) { return std::pair{face->slant, face->weight}; });
  const detail::FontFaceData* previous = nullptr;
  for (const auto& face : data->faces) {
    if (previous && (face->family_name != previous->family_name || face->width != previous->width ||
                     (face->weight == previous->weight && face->slant == previous->slant))) {
      throw std::invalid_argument(
          "HuxerUI font family must have matching family names and widths with distinct styles");
    }
    CombineHash(data->hash, face->hash);
    previous = face.get();
  }
  FontFamily result;
  result.kind_ = FontFamilyKind::Custom;
  result.data_ = std::move(data);
  return result;
}

bool FontFamily::operator==(const FontFamily& other) const noexcept {
  if (kind_ != other.kind_ || name_ != other.name_)
    return false;
  if (data_ == other.data_)
    return true;
  if (!data_ || !other.data_ || data_->hash != other.data_->hash || data_->faces.size() != other.data_->faces.size()) {
    return false;
  }
  for (std::size_t index = 0; index < data_->faces.size(); ++index) {
    if (data_->faces[index]->bytes != other.data_->faces[index]->bytes)
      return false;
  }
  return true;
}

namespace detail {

const std::shared_ptr<const FontFamilyData>& InternalAccess::FontData(const FontFamily& family) noexcept {
  return family.data_;
}

std::size_t HashFontFamily(const FontFamily& family) noexcept {
  auto result = std::hash<FontFamilyKind>{}(family.Kind());
  CombineHash(result, std::hash<std::string_view>{}(family.Name()));
  if (const auto& data = InternalAccess::FontData(family))
    CombineHash(result, data->hash);
  return result;
}

std::shared_ptr<const FontFaceData> ResolveFontFace(const Font& font) {
  const auto& data = InternalAccess::FontData(font.Family());
  if (!data)
    return {};
  return *std::ranges::min_element(data->faces, {}, [&](const auto& face) {
    return std::tuple{face->slant != font.Slant(), WeightRank(static_cast<unsigned>(font.Weight()), face->weight)};
  });
}

FontFamily AppResources::ResolveFontFamily(std::vector<RawResource> resources) {
  std::ranges::sort(resources, {}, [](const auto& resource) { return resource.ToString(); });
  std::string key;
  // Length prefixes keep resource sets distinct even when identifiers contain separators.
  for (const auto& resource : resources) {
    const auto id = resource.ToString();
    key += std::to_string(id.size()) + ':' + id;
  }
  if (const auto cached = font_cache_.find(key); cached != font_cache_.end())
    return cached->second;
  std::vector<Bytes> files;
  files.reserve(resources.size());
  for (auto& resource : resources)
    files.push_back(Resolve(std::move(resource)).ReadBytes(true));
  auto family = FontFamily::FromBytes(std::move(files));
  font_cache_.emplace(std::move(key), family);
  return family;
}

} // namespace detail
} // namespace huxerui
