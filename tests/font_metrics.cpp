#include <algorithm>
#include <cassert>
#include <cstdio>
#include <set>
#include <utility>
#include "font_metrics.h"
#include "native_font_data.h"

using Pixels = std::set<std::pair<int, int>>;

static Pixels render(const reader_font::Font& font, uint32_t codepoint,
                     int x = 0, int y = 0) {
  Pixels result;
  int advance = reader_font::drawGlyphRuns(font, codepoint, x, y,
      [&](int left, int top, int width) {
        assert(width > 0);
        assert(top >= y && top < y + font.height);
        for (int column = 0; column < width; ++column) {
          result.emplace(left + column, top);
        }
      });
  assert(advance == reader_font::glyphAdvance(font, codepoint));
  return result;
}

static void checkFont(const reader_font::Font& font) {
  assert(font.height < font.lineAdvance);
  for (size_t i = 0; i < font.glyphCount; ++i) {
    const auto& glyph = font.glyphs[i];
    if (i) assert(glyph.codepoint > font.glyphs[i - 1].codepoint);
    assert(reader_font::findGlyph(font, glyph.codepoint) == &glyph);
    const size_t stride = (glyph.width + 7) / 8;
    const size_t end = glyph.bitmapOffset + stride * glyph.height;
    assert(end <= font.bitmapSize);
    if (glyph.height) {
      assert(font.baseline + glyph.top >= 0);
      assert(font.baseline + glyph.top + glyph.height <= font.height);
    }
  }

  // Missing BMP, supplementary Unicode and the M5GFX sentinel must all draw
  // the same visible box, with the width pagination reserves for them.
  const auto placeholder = render(font, 0);
  assert(!placeholder.empty());
  for (uint32_t codepoint : {0x9f98u, 0x20bb7u, 0x1f600u, 0xfffdu, 0x3031u}) {
    assert(!reader_font::hasGlyph(font, codepoint));
    assert(render(font, codepoint) == placeholder);
    assert(reader_font::glyphAdvance(font, codepoint) == font.pixels);
  }
  assert(render(font, 0x200b).empty());
  assert(reader_font::glyphAdvance(font, 0x200b) == 0);
  assert(render(font, ' ').empty());
  assert(reader_font::glyphAdvance(font, ' ') > 0);

  // Letters with a common x-height must share a baseline despite descenders.
  const auto* a = reader_font::findGlyph(font, 'a');
  const auto* g = reader_font::findGlyph(font, 'g');
  const auto* y = reader_font::findGlyph(font, 'y');
  assert(a && g && y);
  assert(a->top == g->top && a->top == y->top);
  assert(a->top + a->height == 0);
  assert(g->top + g->height > 0 && y->top + y->height > 0);
  assert(render(font, 'a').size() > 0);
  assert(render(font, 'g').size() > 0);

  // A font's bearing must survive both bitmap packing and drawing offsets.
  const auto* j = reader_font::findGlyph(font, 'j');
  assert(j && j->left < 0);
  auto origin = render(font, 'j');
  Pixels translated;
  for (const auto& pixel : origin) translated.emplace(pixel.first + 17, pixel.second + 23);
  assert(render(font, 'j', 17, 23) == translated);
  assert(origin.begin()->first < 0);
}

int main() {
  using namespace reader_font;
  assert(FONT_28.pixels == 28 && FONT_24.pixels == 24);
  assert(FONT_28.baseline == BODY_BASELINE && FONT_28.height == BODY_HEIGHT);
  assert(FONT_24.baseline == LIBRARY_BASELINE && FONT_24.height == LIBRARY_HEIGHT);
  assert(FONT_28.glyphCount == FONT_24.glyphCount);
  checkFont(FONT_28);
  checkFont(FONT_24);

  // Traditional Chinese supplement: common Big5 characters and the arrow used
  // by the library must have real 24/28px glyphs rather than the missing box.
  for (uint32_t codepoint : {0x8aaau, 0x5167u, 0x773eu, 0x2934u}) {
    assert(hasGlyph(FONT_28, codepoint));
    assert(hasGlyph(FONT_24, codepoint));
  }

  // j used to be shrunk from 28 to 26px (advance 8, height 27).
  const auto* j = findGlyph(FONT_28, 'j');
  assert(j->advance == 9 && j->height == 30);
  assert(findGlyph(FONT_24, 'j')->height == 25);
  assert(decodeCodepoint("a", 1) == 'a');
  assert(decodeCodepoint("\xe6\xb1\x89", 3) == 0x6c49);
  assert(decodeCodepoint("\xf0\xa0\xae\xb7", 4) == 0x20bb7);
  for (auto invalid : {"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80",
                       "\xe6\xa1", "\x80", "\xf8\x80\x80\x80"}) {
    size_t length = 0;
    while (invalid[length]) ++length;
    assert(decodeCodepoint(invalid, length) == 0xfffd);
  }
  assert(decodeCodepoint(nullptr, 0) == 0xfffd);
  std::printf("native font metrics, baseline, bitmap bounds, missing glyphs: PASS "
              "(%zu glyphs, %zu bitmap bytes)\n",
              FONT_28.glyphCount, FONT_28.bitmapSize + FONT_24.bitmapSize);
}
