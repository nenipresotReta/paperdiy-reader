#pragma once
#include <M5GFX.h>
const lgfx::IFont* bodyFont();
int bodyGlyphAdvance(uint32_t codepoint);
bool bodyFontHasGlyph(uint32_t codepoint);
void drawBodyGlyph(const char* utf8,size_t bytes,int x,int y,uint32_t foreground=TFT_BLACK);
