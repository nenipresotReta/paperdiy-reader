#pragma once
#include <M5GFX.h>
#include "font_metrics.h"
constexpr int BODY_FONT_HEIGHT = reader_font::BODY_HEIGHT;
constexpr int BODY_FONT_BASELINE = reader_font::BODY_BASELINE;
const lgfx::IFont* bodyFont();
int bodyGlyphAdvance(uint32_t codepoint);
const reader_font::Glyph* bodyGlyph(uint32_t codepoint);
bool bodyFontHasGlyph(uint32_t codepoint);
void drawBodyGlyph(const char* utf8,size_t bytes,int x,int y,uint32_t foreground=TFT_BLACK);
