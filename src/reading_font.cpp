#include <M5Unified.h>
#include "reading_font.h"
#include "reader_font.h"
#include "native_font_data.h"
#include "reader_settings.h"

namespace {

void paint(lgfx::LGFXBase* display, const reader_font::Font& font,
           uint32_t codepoint, int x, int y, uint32_t color) {
  reader_font::drawGlyphRuns(font, codepoint, x, y,
      [display, color](int left, int top, int width) {
        display->drawFastHLine(left, top, width, color);
      });
}

class NativeFont : public lgfx::IFont {
  const reader_font::Font& font;
public:
  explicit NativeFont(const reader_font::Font& data) : font(data) {}

  void getDefaultMetric(lgfx::FontMetrics* metrics) const override {
    metrics->height = font.height;
    metrics->y_advance = font.lineAdvance;
    metrics->baseline = font.baseline;
    metrics->y_offset = -font.baseline;
    metrics->x_offset = 0;
    metrics->width = metrics->x_advance = font.pixels;
  }

  bool updateFontMetric(lgfx::FontMetrics* metrics, uint16_t codepoint) const override {
    metrics->x_advance = reader_font::glyphAdvance(font, codepoint);
    metrics->x_offset = 0;
    metrics->width = metrics->x_advance;
    if (const auto* glyph = reader_font::findGlyph(font, codepoint)) {
      metrics->width = glyph->width;
      metrics->x_offset = glyph->left;
    }
    // U+0000 is the explicit missing-glyph box requested by M5GFX.
    return codepoint == 0 || reader_font::hasGlyph(font, codepoint);
  }

  size_t drawChar(lgfx::LGFXBase* display, int32_t x, int32_t y,
                  uint16_t codepoint, const lgfx::TextStyle* style,
                  lgfx::FontMetrics*, int32_t& filled) const override {
    const int32_t scaleX = 65536 * style->size_x;
    const int32_t scaleY = 65536 * style->size_y;
    const int top = y - ((font.baseline * scaleY) >> 16);
    const int advance = reader_font::drawGlyphRuns(font, codepoint, 0, 0,
        [display, x, top, scaleX, scaleY, style](int left, int row, int width) {
          const int firstX = (left * scaleX) >> 16;
          const int lastX = ((left + width) * scaleX) >> 16;
          const int firstY = (row * scaleY) >> 16;
          const int lastY = ((row + 1) * scaleY) >> 16;
          if (lastX > firstX && lastY > firstY) {
            display->fillRect(x + firstX, top + firstY, lastX - firstX,
                              lastY - firstY, style->fore_rgb888);
          }
        });
    const int scaledAdvance = (advance * scaleX) >> 16;
    filled = x + scaledAdvance;
    return scaledAdvance;
  }
};

NativeFont body(reader_font::FONT_28), small(reader_font::FONT_24);

} // namespace

static const reader_font::Font& activeBody(){return reader_settings::geometry().pixels==24?reader_font::FONT_24:reader_font::FONT_28;}
const lgfx::IFont* bodyFont() { return reader_settings::geometry().pixels==24?&small:&body; }
const reader_font::Glyph* bodyGlyph(uint32_t codepoint) {
  return reader_font::findGlyph(activeBody(), codepoint);
}
int bodyGlyphAdvance(uint32_t codepoint) {
  return reader_font::glyphAdvance(activeBody(), codepoint);
}
bool bodyFontHasGlyph(uint32_t codepoint) {
  return reader_font::hasGlyph(activeBody(), codepoint);
}
void drawBodyGlyph(const char* text, size_t bytes, int x, int y, uint32_t foreground) {
  paint(&M5.Display, activeBody(), readerCodepoint(text, bytes), x, y, foreground);
}
const lgfx::IFont* readerFont() { return &small; }
int readerGlyphWidth(uint32_t codepoint) {
  return reader_font::glyphAdvance(reader_font::FONT_24, codepoint);
}
bool readerFontHasGlyph(uint32_t codepoint) {
  return reader_font::hasGlyph(reader_font::FONT_24, codepoint);
}
uint32_t readerCodepoint(const char* text, size_t bytes) {
  return reader_font::decodeCodepoint(text, bytes);
}
void drawReaderGlyph(const char* text, size_t bytes, int x, int y, uint32_t foreground) {
  paint(&M5.Display, reader_font::FONT_24, readerCodepoint(text, bytes), x, y, foreground);
}

int interfaceGlyphAdvance(uint32_t cp){return reader_font::glyphAdvance(reader_font::FONT_28,cp);}
void drawInterfaceGlyph(const char* text,size_t bytes,int x,int y,uint32_t foreground){paint(&M5.Display,reader_font::FONT_28,readerCodepoint(text,bytes),x,y,foreground);}
