#pragma once
#include <cstddef>
#include <cstdint>

// Native, one-bit math glyphs. Coordinates are relative to the glyph baseline.
// The generated subset is derived from STIX Two Math under SIL OFL 1.1.
namespace math_font {
struct Glyph {
  uint32_t codepoint, bitmapOffset;
  int16_t advance, left, top, width, height, italicCorrection;
  uint8_t pixels;
};
struct Constants {
  int16_t axis, rule, fractionGap, displayFractionGap;
  int16_t supShift, subShift, scriptGap, scriptSpace;
  int16_t limitGap, radicalGap, displayRadicalGap;
};
enum class Style : uint8_t { Normal, Roman, Italic, Bold, Blackboard, Calligraphic };
const Glyph* find(uint32_t codepoint, uint8_t pixels);
const Glyph* variant(uint32_t codepoint, uint8_t pixels, int minimumHeight);
const Constants& constants(uint8_t pixels);
uint32_t styled(uint32_t codepoint, Style style);
uint8_t bitmapByte(size_t offset);
size_t glyphCount();
size_t bitmapSize();

// emit(x, y, length) receives horizontal runs of black pixels only.
template<class Emit>
bool drawGlyphRuns(uint32_t codepoint, uint8_t pixels, int x, int baseline, Emit emit) {
  const Glyph* glyph = find(codepoint, pixels);
  if (!glyph) return false;
  const size_t stride = (static_cast<size_t>(glyph->width) + 7) / 8;
  for (int row = 0; row < glyph->height; ++row) {
    int run = -1;
    for (int col = 0; col <= glyph->width; ++col) {
      const bool ink = col < glyph->width &&
          (bitmapByte(glyph->bitmapOffset + row * stride + col / 8) & (0x80 >> (col % 8)));
      if (ink && run < 0) run = col;
      if (!ink && run >= 0) {
        emit(x + glyph->left + run, baseline + glyph->top + row, col - run);
        run = -1;
      }
    }
  }
  return true;
}
}  // namespace math_font
