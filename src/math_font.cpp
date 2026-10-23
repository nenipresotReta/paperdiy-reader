#include "math_font.h"
#include "math_font_data.h"

namespace math_font {
const Glyph* find(uint32_t codepoint, uint8_t pixels) {
  size_t lo = 0, hi = sizeof(data::glyphs) / sizeof(data::glyphs[0]);
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const Glyph& glyph = data::glyphs[mid];
    if (glyph.pixels < pixels || (glyph.pixels == pixels && glyph.codepoint < codepoint)) lo = mid + 1;
    else hi = mid;
  }
  return lo < glyphCount() && data::glyphs[lo].pixels == pixels &&
      data::glyphs[lo].codepoint == codepoint ? &data::glyphs[lo] : nullptr;
}
const Glyph* variant(uint32_t codepoint, uint8_t pixels, int minimumHeight) {
  const Glyph* best = find(codepoint, pixels);
  if (!best || best->height < minimumHeight) best = nullptr;
  for (const auto& v : data::variants) {
    if (v.base == codepoint && v.pixels == pixels && v.height >= minimumHeight &&
        (!best || v.height < best->height)) best = find(v.codepoint, pixels);
  }
  return best;
}
const Constants& constants(uint8_t pixels) {
  return data::constants[pixels == 14 ? 0 : pixels == 20 ? 1 : 2];
}
uint32_t styled(uint32_t cp, Style style) {
  if (style == Style::Normal) {
    style = ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
             (cp >= 0x3B1 && cp <= 0x3C9) || cp == 0x3F5 || cp == 0x3D1 ||
             cp == 0x3F0 || cp == 0x3D5 || cp == 0x3F1 || cp == 0x3D6) ? Style::Italic : Style::Roman;
  }
  if (style == Style::Roman) return cp;
  // Nonalphabetic math symbols retain their own native glyph under a font switch.
  const bool alphabet = (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
      (cp >= '0' && cp <= '9') || (cp >= 0x391 && cp <= 0x3C9);
  for (const auto& s : data::styles) {
    if (s.base == cp && s.style == static_cast<uint8_t>(style)) return s.codepoint;
  }
  // Digits remain upright in math italic, as in conventional mathematical type.
  if (style == Style::Italic && cp >= '0' && cp <= '9') return cp;
  return alphabet ? 0 : cp;
}
uint8_t bitmapByte(size_t offset) { return offset < sizeof(data::bitmap) ? data::bitmap[offset] : 0; }
size_t glyphCount() { return sizeof(data::glyphs) / sizeof(data::glyphs[0]); }
size_t bitmapSize() { return sizeof(data::bitmap); }
}  // namespace math_font
