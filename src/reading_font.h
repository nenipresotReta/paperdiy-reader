#pragma once
#include <M5GFX.h>
#include "font_metrics.h"
#include "reader_settings.h"
inline int bodyFontHeight(){return reader_settings::geometry().pixels==24?34:reader_font::BODY_HEIGHT;}
inline int bodyFontBaseline(){return reader_settings::geometry().pixels==24?26:reader_font::BODY_BASELINE;}
const lgfx::IFont* bodyFont();
int bodyGlyphAdvance(uint32_t codepoint);
const reader_font::Glyph* bodyGlyph(uint32_t codepoint);
bool bodyFontHasGlyph(uint32_t codepoint);
void drawBodyGlyph(const char* utf8,size_t bytes,int x,int y,uint32_t foreground=TFT_BLACK);

// Fixed 28 px interface glyphs are independent of the reading preset.
int interfaceGlyphAdvance(uint32_t codepoint);
void drawInterfaceGlyph(const char* utf8,size_t bytes,int x,int y,uint32_t foreground=TFT_BLACK);
