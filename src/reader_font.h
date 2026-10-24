#pragma once
#include <M5GFX.h>
#include "font_metrics.h"
// Native 24px library font; body font uses the same repertoire at 28px.
constexpr int READER_FONT_HEIGHT = reader_font::LIBRARY_HEIGHT;
constexpr int READER_FONT_BASELINE = reader_font::LIBRARY_BASELINE;
const lgfx::IFont* readerFont();
bool readerFontHasGlyph(uint32_t codepoint);
uint32_t readerCodepoint(const char* utf8,size_t bytes);
int readerGlyphWidth(uint32_t codepoint);
// TFT_* colors use RGB565. Keep the 16-bit type through the M5GFX draw call;
// its uint32_t overload interprets 0xFFFF as RGB888 cyan, not white.
void drawReaderGlyph(const char* utf8,size_t bytes,int x,int y,uint16_t foreground=TFT_BLACK);
