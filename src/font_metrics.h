#pragma once
#include <cstddef>
#include <cstdint>

namespace reader_font {

constexpr int BODY_PIXELS = 28;
constexpr int BODY_BASELINE = 31;
constexpr int BODY_HEIGHT = 39;
constexpr int BODY_LINE_ADVANCE = 40;
constexpr int LIBRARY_PIXELS = 24;
constexpr int LIBRARY_BASELINE = 26;
constexpr int LIBRARY_HEIGHT = 34;
constexpr int LIBRARY_LINE_ADVANCE = 39;

// Bearings locate the bitmap relative to the pen on the common baseline.
// Rows are tightly packed, most significant bit first; each row starts a byte.
struct Glyph {
  uint32_t codepoint;
  uint32_t bitmapOffset;
  uint8_t advance;
  int8_t left;
  int8_t top;
  uint8_t width;
  uint8_t height;
};

struct Font {
  const Glyph* glyphs;
  size_t glyphCount;
  const uint8_t* bitmap;
  size_t bitmapSize;
  uint8_t pixels;
  uint8_t baseline;
  uint8_t height;
  uint8_t lineAdvance;
};

inline const Glyph* findGlyph(const Font& font, uint32_t codepoint) {
  size_t first = 0, last = font.glyphCount;
  while (first < last) {
    size_t middle = first + (last - first) / 2;
    if (font.glyphs[middle].codepoint < codepoint) first = middle + 1;
    else last = middle;
  }
  return first < font.glyphCount && font.glyphs[first].codepoint == codepoint
      ? &font.glyphs[first] : nullptr;
}

inline int blankAdvance(uint32_t codepoint, int pixels) {
  // U+0000 is the M5GFX missing-glyph sentinel and must remain visible.
  if ((codepoint > 0 && codepoint < 32) ||
      codepoint == 0xfeff || codepoint == 0x200b || codepoint == 0x200c ||
      codepoint == 0x200d || codepoint == 0x2060 || codepoint == 0xad ||
      (codepoint >= 0x202a && codepoint <= 0x202e) ||
      (codepoint >= 0x2066 && codepoint <= 0x2069) ||
      (codepoint >= 0xfe00 && codepoint <= 0xfe0f)) return 0;
  if (codepoint == 0xa0 || codepoint == 0x2007) return pixels / 2;
  if (codepoint == 0x202f || codepoint == 0x2009) return pixels / 4;
  if (codepoint == 0x2003) return pixels;
  return -1;
}

inline int glyphAdvance(const Font& font, uint32_t codepoint) {
  int blank = blankAdvance(codepoint, font.pixels);
  if (blank >= 0) return blank;
  const auto* glyph = findGlyph(font, codepoint);
  return glyph ? glyph->advance : font.pixels;
}

inline bool hasGlyph(const Font& font, uint32_t codepoint) {
  return blankAdvance(codepoint, font.pixels) >= 0 ||
      findGlyph(font, codepoint) != nullptr;
}

inline bool inkAt(const Font& font, const Glyph& glyph, int x, int y) {
  const size_t stride = (glyph.width + 7) / 8;
  return (font.bitmap[glyph.bitmapOffset + y * stride + x / 8] &
      (0x80u >> (x & 7))) != 0;
}

// Emit black horizontal runs. Layout, the device and host tests share the same
// placement and fallback path; x/y are the top-left of the font's line box.
template<class EmitRun>
int drawGlyphRuns(const Font& font, uint32_t codepoint,
                  int x, int y, EmitRun emit) {
  int blank = blankAdvance(codepoint, font.pixels);
  if (blank >= 0) return blank;
  if (const auto* glyph = findGlyph(font, codepoint)) {
    for (int row = 0; row < glyph->height; ++row) {
      int column = 0;
      while (column < glyph->width) {
        while (column < glyph->width && !inkAt(font, *glyph, column, row)) ++column;
        const int first = column;
        while (column < glyph->width && inkAt(font, *glyph, column, row)) ++column;
        if (column > first) {
          emit(x + glyph->left + first, y + font.baseline + glyph->top + row,
               column - first);
        }
      }
    }
    return glyph->advance;
  }

  // Missing BMP/non-BMP glyphs and the SDK's U+0000 sentinel use the same box.
  const int left = x + 2;
  const int top = y + font.baseline - font.pixels + 2;
  const int width = font.pixels - 4;
  const int height = font.pixels - 4;
  emit(left, top, width);
  for (int row = 1; row < height - 1; ++row) {
    emit(left, top + row, 1);
    emit(left + width - 1, top + row, 1);
  }
  emit(left, top + height - 1, width);
  return font.pixels;
}

inline uint32_t decodeCodepoint(const char* text, size_t bytes) {
  constexpr uint32_t replacement = 0xfffd;
  if (text == nullptr || bytes == 0 || bytes > 4) return replacement;
  const auto first = static_cast<uint8_t>(text[0]);
  if (bytes == 1) return first < 0x80 ? first : replacement;
  const size_t expected = first >= 0xc2 && first <= 0xdf ? 2 :
      first >= 0xe0 && first <= 0xef ? 3 :
      first >= 0xf0 && first <= 0xf4 ? 4 : 0;
  if (bytes != expected) return replacement;
  uint32_t codepoint = first & ((1u << (7 - bytes)) - 1);
  for (size_t i = 1; i < bytes; ++i) {
    const auto next = static_cast<uint8_t>(text[i]);
    if ((next & 0xc0) != 0x80) return replacement;
    codepoint = (codepoint << 6) | (next & 0x3f);
  }
  if ((bytes == 2 && codepoint < 0x80) ||
      (bytes == 3 && codepoint < 0x800) ||
      (bytes == 4 && codepoint < 0x10000) ||
      (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff) {
    return replacement;
  }
  return codepoint;
}

} // namespace reader_font
