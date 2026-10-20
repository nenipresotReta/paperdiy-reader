#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace bookmarks {
// Whitespace is deliberately ignored: the old XHTML reader joined inline
// words and collapsed preformatted spaces. Offsets always refer to original
// UTF-8 bytes, never to this comparison representation.
inline size_t whitespace(const char* text, size_t size, size_t at) {
  const auto* p = reinterpret_cast<const unsigned char*>(text + at);
  if (p[0] == ' ' || (p[0] >= 9 && p[0] <= 13)) return 1;
  if (size - at >= 2 && p[0] == 0xc2 && p[1] == 0xa0) return 2;
  if (size - at >= 3 && p[0] == 0xe3 && p[1] == 0x80 && p[2] == 0x80) return 3;
  return 0;
}
struct Anchor {
  std::string context;
  size_t cursor = 0;
  uint32_t offset = 0, length = 0;
  bool valid = false;
};
inline Anchor capture(const char* text, size_t size, uint32_t offset) {
  Anchor a;
  if (offset >= size) return a;
  a.offset = offset; a.length = size;
  // Only inspect a bounded window; the match below scans with bounded memory.
  size_t begin = offset > 512 ? offset - 512 : 0;
  while (begin < offset && (uint8_t(text[begin]) & 0xc0) == 0x80) ++begin;
  std::string before;
  for (size_t p = begin; p < offset;) {
    size_t n = whitespace(text, size, p);
    if (n) p += n; else before += text[p++];
  }
  if (before.size() > 128) before.erase(0, before.size() - 128);
  a.context = before; a.cursor = before.size();
  for (size_t p = offset; p < size && a.context.size() < a.cursor + 256;) {
    size_t n = whitespace(text, size, p);
    if (n) p += n; else a.context += text[p++];
  }
  a.valid = !a.context.empty();
  return a;
}

// KMP avoids quadratic searches on repetitive books. A ring holds only the
// original byte positions of one context. Repeated passages prefer the match
// nearest the old relative position.
template<class Tick>
bool relocate(const Anchor& a, const char* text, size_t size, uint32_t& offset, Tick tick) {
  if (!a.valid || !size) return false;
  if (a.offset == 0) { offset = 0; return true; }
  const size_t n = a.context.size();
  std::vector<size_t> prefix(n), positions(n);
  for (size_t i = 1, j = 0; i < n; ++i) {
    while (j && a.context[i] != a.context[j]) j = prefix[j - 1];
    if (a.context[i] == a.context[j]) ++j;
    prefix[i] = j;
  }
  const uint64_t expected = uint64_t(a.offset) * size / a.length;
  uint64_t distance = UINT64_MAX;
  size_t seen = 0, matched = 0;
  for (size_t p = 0; p < size;) {
    if ((p & 4095) == 0) tick();
    size_t skip = whitespace(text, size, p);
    if (skip) { p += skip; continue; }
    positions[seen % n] = p;
    while (matched && text[p] != a.context[matched]) matched = prefix[matched - 1];
    if (text[p] == a.context[matched]) ++matched;
    ++seen; ++p;
    if (matched != n) continue;
    size_t candidate = a.cursor < n
      ? positions[(seen - n + a.cursor) % n] : p;
    uint64_t delta = candidate > expected ? candidate - expected : expected - candidate;
    if (delta < distance && candidate < size) {
      distance = delta; offset = candidate;
    }
    matched = prefix[matched - 1];
  }
  return distance != UINT64_MAX;
}
}
