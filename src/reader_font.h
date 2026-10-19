#pragma once
#include <M5GFX.h>
// Native 24px library font; body font uses the same repertoire at 28px.
const lgfx::IFont* readerFont();
bool readerFontHasGlyph(uint32_t codepoint);
uint32_t readerCodepoint(const char* utf8,size_t bytes);
int readerGlyphWidth(uint32_t codepoint);
void drawReaderGlyph(const char* utf8,size_t bytes,int x,int y,uint32_t foreground=TFT_BLACK);
