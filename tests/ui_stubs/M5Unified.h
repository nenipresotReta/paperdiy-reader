#pragma once
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "M5GFX.h"

// Host-side framebuffer with a real clip rectangle and a panel that only
// receives pixels inside the refresh rectangle. This mirrors the two display
// boundaries that matter for the page-margin regression.
int readerGlyphWidth(uint32_t codepoint);
uint32_t readerCodepoint(const char* utf8, size_t bytes);
void drawReaderGlyph(const char* utf8, size_t bytes, int x, int y,
                     uint32_t foreground);

class SimDisplay {
 public:
  static constexpr int W = 540, H = 960;
  std::vector<uint8_t> fb, panel;
  int clipX0 = 0, clipY0 = 0, clipX1 = W, clipY1 = H;
  int lastClipX = 0, lastClipY = 0, lastClipW = W, lastClipH = H;
  int lastRefreshX = -1, lastRefreshY = -1, lastRefreshW = 0, lastRefreshH = 0;
  bool lastFull = false;
  unsigned refreshCount = 0;
  int cursorX = 0, cursorY = 0;
  int rotation = 0;
  uint32_t textFg = TFT_BLACK;

  SimDisplay() : fb(W * H, 255), panel(W * H, 255) {}

  void setFont(const lgfx::IFont*) {}
  void setTextSize(int) {}
  void setTextColor(uint32_t fg, uint32_t) { textFg = fg; }
  void setCursor(int x, int y) { cursorX = x; cursorY = y; }

  void setClipRect(int x, int y, int w, int h) {
    lastClipX = x;
    lastClipY = y;
    lastClipW = w;
    lastClipH = h;
    clipX0 = std::max(0, x);
    clipY0 = std::max(0, y);
    clipX1 = std::min(W, x + w);
    clipY1 = std::min(H, y + h);
  }
  void clearClipRect() {
    clipX0 = 0;
    clipY0 = 0;
    clipX1 = W;
    clipY1 = H;
  }

  void fillScreen(uint32_t color) {
    std::fill(fb.begin(), fb.end(), uint8_t(color));
  }

  void fillRect(int x, int y, int w, int h, uint32_t color) {
    int x0 = std::max({0, x, clipX0}), x1 = std::min({W, x + w, clipX1});
    int y0 = std::max({0, y, clipY0}), y1 = std::min({H, y + h, clipY1});
    if (x0 >= x1 || y0 >= y1) return;
    const uint8_t value = uint8_t(color);
    for (int row = y0; row < y1; ++row)
      std::fill(fb.begin() + row * W + x0, fb.begin() + row * W + x1, value);
  }

  void drawPixel(int x, int y, uint32_t color) {
    if (x < clipX0 || x >= clipX1 || y < clipY0 || y >= clipY1) return;
    fb[y * W + x] = uint8_t(color);
  }

  void drawFastHLine(int x, int y, int w, uint32_t color) {
    if (w <= 0) return;
    int x0 = std::max({0, x, clipX0}), x1 = std::min({W, x + w, clipX1});
    if (x0 >= x1 || y < clipY0 || y >= clipY1) return;
    std::fill(fb.begin() + y * W + x0, fb.begin() + y * W + x1, uint8_t(color));
  }

  void drawFastVLine(int x, int y, int h, uint32_t color) {
    if (h <= 0 || x < clipX0 || x >= clipX1) return;
    int y0 = std::max({0, y, clipY0}), y1 = std::min({H, y + h, clipY1});
    for (int row = y0; row < y1; ++row) fb[row * W + x] = uint8_t(color);
  }

  void drawLine(int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
      drawPixel(x0, y0, color);
      if (x0 == x1 && y0 == y1) break;
      int e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
    }
  }

  void drawRect(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    drawFastHLine(x, y, w, color);
    drawFastHLine(x, y + h - 1, w, color);
    drawFastVLine(x, y, h, color);
    drawFastVLine(x + w - 1, y, h, color);
  }

  void drawCircle(int cx, int cy, int r, uint32_t color) {
    for (int y = -r; y <= r; ++y)
      for (int x = -r; x <= r; ++x) {
        int d = x * x + y * y;
        if (d <= r * r && d > (r - 1) * (r - 1)) drawPixel(cx + x, cy + y, color);
      }
  }

  void fillCircle(int cx, int cy, int r, uint32_t color) {
    for (int y = -r; y <= r; ++y)
      for (int x = -r; x <= r; ++x)
        if (x * x + y * y <= r * r) drawPixel(cx + x, cy + y, color);
  }

  static size_t utf8Length(const char* s) {
    const unsigned char c = uint8_t(s[0]);
    size_t n = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 :
               (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1;
    for (size_t i = 1; i < n; ++i)
      if ((uint8_t(s[i]) & 0xc0) != 0x80) return 1;
    return n;
  }

  void print(const char* s) {
    while (s && *s) {
      const size_t n = utf8Length(s);
      const uint32_t cp = readerCodepoint(s, n);
      drawReaderGlyph(s, n, cursorX, cursorY, textFg);
      cursorX += readerGlyphWidth(cp);
      s += n;
    }
  }
  void print(const std::string& s) { print(s.c_str()); }

  int textWidth(const char* s) const {
    int width = 0;
    while (s && *s) {
      const size_t n = utf8Length(s);
      width += readerGlyphWidth(readerCodepoint(s, n));
      s += n;
    }
    return width;
  }
  int textWidth(const std::string& s) const { return textWidth(s.c_str()); }

  int width() const { return W; }
  int height() const { return H; }
  void setRotation(int value) {
    // This portrait-only fake models the two orientations used by the reader.
    assert(value == 0 || value == 2);
    if (value != rotation) std::reverse(fb.begin(), fb.end());
    rotation = value;
  }
  void setAutoDisplay(bool) {}
  void waitDisplay() {}
  void setEpdMode(epd_mode_t) {}
  void startWrite() {}
  void endWrite() {}

  void display(int x, int y, int w, int h) { refresh(x, y, w, h); }
  void display() { refresh(0, 0, W, H); }

  void refresh(int x, int y, int w, int h) {
    lastRefreshX = x;
    lastRefreshY = y;
    lastRefreshW = w;
    lastRefreshH = h;
    lastFull = x == 0 && y == 0 && w == W && h == H;
    ++refreshCount;
    const int x0 = std::max(0, x), x1 = std::min(W, x + w);
    const int y0 = std::max(0, y), y1 = std::min(H, y + h);
    for (int row = y0; row < y1; ++row)
      for (int column = x0; column < x1; ++column) {
        const int px = rotation == 2 ? W - 1 - column : column;
        const int py = rotation == 2 ? H - 1 - row : row;
        panel[py * W + px] = fb[row * W + column];
      }
  }
};

struct SimPower {
  int battery = 87;
  int getBatteryLevel() const { return battery; }
};

struct SimM5 {
  SimDisplay Display;
  SimPower Power;
};
inline SimM5 M5;
