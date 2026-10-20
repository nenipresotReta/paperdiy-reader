#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pagination {
struct Geometry { int left = 24, right = 516, top = 24, bottom = 886, lineHeight = 40; };
inline size_t utf8Bytes(const char* text, size_t remaining) {
  if (!remaining) return 0;
  uint8_t c = text[0];
  size_t n = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 :
             (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1;
  return n <= remaining ? n : 1;
}
template<class Width, class Draw, class Tick>
size_t page(const char* text, size_t length, size_t from, Geometry g,
            Width width, Draw draw, Tick tick) {
  if (g.right <= g.left || g.lineHeight <= 0 || g.bottom < g.top) return from;
  int x = g.left, y = g.top;
  size_t p = from;
  while (p < length) {
    const size_t n = utf8Bytes(text + p, length - p);
    const char c = text[p];
    if (c == '\r') { p += n; continue; }
    if (c == '\n') {
      p += n; x = g.left; y += g.lineHeight;
      if (y > g.bottom) break;
      continue;
    }
    const char* glyph = c == '\t' ? " " : text + p;
    int w = width(glyph, n);
    if (w < 0) w = 0;
    // An over-wide glyph still consumes input on an otherwise empty line.
    if (x != g.left && x + w > g.right) {
      x = g.left; y += g.lineHeight;
      if (y > g.bottom) break;
    }
    draw(glyph, n, x, y);
    x += w; p += n;
    if ((p & 1023) < 4) tick();
  }
  return p;
}
template<class Next>
bool index(size_t length, std::vector<uint32_t>& offsets, Next next, size_t maxPages = 65536) {
  offsets.clear();
  if (!length || length > UINT32_MAX) return false;
  for (size_t p = 0; p < length;) {
    if (offsets.size() == maxPages) { offsets.clear(); return false; }
    offsets.push_back(p);
    size_t end = next(p);
    if (end <= p || end > length) { offsets.clear(); return false; }
    p = end;
  }
  return true;
}
}
