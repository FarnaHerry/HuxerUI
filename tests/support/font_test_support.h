#pragma once

#include <cstring>
#include <span>

#include <huxerui/text.h>

#include "font_test_data.h"

namespace huxerui::test {

inline Bytes TestFontBytes(FontWeight weight = FontWeight::Regular, FontSlant slant = FontSlant::Normal) {
  using Span = std::span<const unsigned char>;
  const Span source = slant == FontSlant::Italic   ? Span(test_font_italic)
                      : weight == FontWeight::Bold ? Span(test_font_bold)
                                                   : Span(test_font_regular);
  Bytes result(source.size());
  std::memcpy(result.data(), source.data(), source.size());
  return result;
}

inline FontFamily TestFontFamily() {
  return FontFamily::FromBytes(
      {TestFontBytes(), TestFontBytes(FontWeight::Bold), TestFontBytes(FontWeight::Regular, FontSlant::Italic)});
}

} // namespace huxerui::test
