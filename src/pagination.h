#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "english_hyphenation.h"

namespace pagination {
struct Geometry {
  int left = 24, right = 516, top = 24, bottom = 886, lineHeight = 40;
  bool hyphenate = true, justify = true;
  // Maximum display-only overhang beyond `right`; 0 keeps it unbounded.
  int hangLimit = 0;
};
inline size_t utf8Bytes(const char* text, size_t remaining) {
  if (!remaining) return 0;
  uint8_t c = text[0];
  size_t n = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 :
             (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1;
  if (n > remaining) return 1;
  for (size_t i = 1; i < n; ++i)
    if ((uint8_t(text[i]) & 0xc0) != 0x80) return 1;
  return n;
}
inline uint32_t codepoint(const char* text, size_t n) {
  if (n == 1) return uint8_t(text[0]);
  uint32_t cp = uint8_t(text[0]) & (n == 2 ? 31 : n == 3 ? 15 : 7);
  for (size_t i = 1; i < n; ++i) cp = (cp << 6) | (uint8_t(text[i]) & 63);
  return cp;
}
inline bool space(uint32_t c) {
  return c == ' ' || c == '\t' || (c >= 0x2000 && c <= 0x200a && c != 0x2007);
}
inline bool noBreakSpace(uint32_t c) { return c == 0xa0 || c == 0x2007 || c == 0x202f; }
inline bool invisible(uint32_t c) {
  return c == '\r' || c == 0xad || c == 0x200b || c == 0x2060 || c == 0xfeff ||
         (c >= 0xfe00 && c <= 0xfe0f);
}
inline bool combining(uint32_t c) {
  return (c >= 0x300 && c <= 0x36f) || (c >= 0x1ab0 && c <= 0x1aff) ||
         (c >= 0x1dc0 && c <= 0x1dff) || (c >= 0xfe00 && c <= 0xfe0f) ||
         (c >= 0x1f3fb && c <= 0x1f3ff);
}
inline bool cjk(uint32_t c) {
  return (c >= 0x2e80 && c <= 0x9fff) || (c >= 0xac00 && c <= 0xd7af) ||
         (c >= 0xff00 && c <= 0xffef) || (c >= 0x20000 && c <= 0x3134f);
}
inline bool opening(uint32_t c) {
  switch (c) {
    case '(': case '[': case '{': case 0x2018: case 0x201c:
    case 0x3008: case 0x300a: case 0x300c: case 0x300e: case 0x3010:
    case 0x3014: case 0xff08: case 0xff3b: case 0xff5b: return true;
    default: return false;
  }
}
inline bool closing(uint32_t c) {
  switch (c) {
    case ')': case ']': case '}': case ',': case '.': case '!': case '?': case ':': case ';':
    case 0x2019: case 0x201d: case 0x3001: case 0x3002: case 0x3009: case 0x300b:
    case 0x300d: case 0x300f: case 0x3011: case 0x3015: case 0xff09: case 0xff0c:
    case 0xff0e: case 0xff01: case 0xff1f: case 0xff1a: case 0xff1b:
    case 0xff3d: case 0xff5d: return true;
    default: return false;
  }
}
inline bool latin(uint32_t c) {
  return english_hyphenation::letter(c < 128 ? uint8_t(c) : 0) ||
         (c >= 0xc0 && c <= 0x2af && c != 0xd7 && c != 0xf7);
}
inline bool latinCompatible(uint32_t c) {
  return c < 128 || latin(c) || combining(c) || noBreakSpace(c) ||
         (c >= 0x2000 && c <= 0x206f);
}
// Source positions remain byte offsets. A discretionary hyphen is display-only.
struct Line {
  size_t begin = 0, end = 0, next = 0;
  int advance = 0, spaces = 0, hang = 0;
  bool hyphen = false, paragraphEnd = false, latinText = false;
  bool compatible = true, formatted = false, emergency = false;
};
template<class Width>
int advance(const char* text, size_t n, uint32_t cp, Width width) {
  if (invisible(cp)) return 0;
  int w = (cp == '\t' || noBreakSpace(cp)) ? width(" ", 1) : width(text, n);
  return std::max(0, w);
}
inline bool wordGuard(uint32_t c) {
  return latin(c) || (c >= '0' && c <= '9') || noBreakSpace(c) || c == 0x2060 ||
         c == 0xad || c == 0x2019 || c == '\'' || c == '_' || c == '@' ||
         c == '/' || c == '.' || c == '\\' || c == ':' || c == '=' || c == '+' || c == '#';
}
inline uint32_t previous(const char* text, size_t at) {
  if (!at) return 0;
  size_t p = at - 1;
  while (p && at - p < 4 && (uint8_t(text[p]) & 0xc0) == 0x80) --p;
  return codepoint(text + p, at - p);
}
template<class Width>
bool automatic(const char* text, size_t length, size_t overflow,
               int available, Width width, const Line& scanned, Line& result) {
  if (overflow >= length) return false;
  size_t probe = overflow;
  if (!english_hyphenation::letter(uint8_t(text[probe]))) {
    uint32_t cp = codepoint(text + probe, utf8Bytes(text + probe, length - probe));
    if (!closing(cp) || !probe || !english_hyphenation::letter(uint8_t(text[probe-1]))) return false;
    --probe;
  }
  size_t start = probe, end = probe;
  while (start && overflow - start <= english_hyphenation::MAX_WORD &&
         english_hyphenation::letter(uint8_t(text[start - 1]))) --start;
  while (end < length && end - start <= english_hyphenation::MAX_WORD &&
         english_hyphenation::letter(uint8_t(text[end]))) ++end;
  if (end - start > english_hyphenation::MAX_WORD ||
      (start && wordGuard(previous(text, start)))) return false;
  if (end < length) {
    auto cp = codepoint(text + end, utf8Bytes(text + end, length - end));
    // A sentence-final period may travel with a break inside the word; a
    // period inside a domain/identifier (example.com, 3.14) must not.
    if ((cp != '.' && wordGuard(cp)) || combining(cp) ||
        (cp == '.' && end + 1 < length &&
         (latin(uint8_t(text[end + 1])) || (text[end + 1] >= '0' && text[end + 1] <= '9')))) return false;
  }
  uint64_t points = english_hyphenation::points(text + start, end - start);
  if (!points) return false;
  int base = scanned.advance;
  size_t visibleStart = std::max(start, scanned.begin);
  for (size_t p = visibleStart; p < overflow; ++p) base -= std::max(0, width(text + p, 1));
  int w = base, hyphenWidth = std::max(0, width("-", 1));
  bool found = false;
  for (size_t p = visibleStart; p < end; ++p) {
    w += std::max(0, width(text + p, 1));
    size_t cut = p + 1;
    if (cut >= visibleStart + 2 && (points & (uint64_t(1) << (cut - start))) &&
        w + hyphenWidth <= available && cut > result.end) {
      result = scanned; result.end = result.next = cut;
      result.advance = w + hyphenWidth; result.hyphen = true;
      found = true;
    }
    if (w + hyphenWidth > available) break;
  }
  return found;
}
inline bool softPoint(const char* text, size_t length, size_t at) {
  unsigned before = 0, after = 0;
  size_t p = at, budget = 0;
  while (p && before < 2 && budget++ < 64) {
    uint32_t cp = previous(text, p);
    size_t begin = p - 1;
    while (begin && p - begin < 4 && (uint8_t(text[begin]) & 0xc0) == 0x80) --begin;
    if (latin(cp)) ++before;
    else if (cp != 0xad && !combining(cp)) break;
    p = begin;
  }
  p = at + 2; budget = 0;
  while (p < length && after < 3 && budget++ < 64) {
    size_t n = utf8Bytes(text + p, length - p);
    uint32_t cp = codepoint(text + p, n);
    if (latin(cp)) ++after;
    else if (cp != 0xad && !combining(cp)) break;
    p += n;
  }
  return before >= 2 && after >= 3;
}

template<class Width, class Tick>
Line line(const char* text, size_t length, size_t from, Geometry g,
          Width width, Tick tick, bool allowHyphen = true) {
  Line current; current.begin = current.end = current.next = from;
  Line best = current;
  int available = g.right - g.left;
  size_t p = from, inkEnd = from;
  int inkWidth = 0, inkSpaces = 0;
  uint32_t prev = 0;
  bool prevSpace = false;
  unsigned units = 0;
  while (p < length) {
    size_t n = utf8Bytes(text + p, length - p);
    uint32_t cp = codepoint(text + p, n);
    if (cp == '\n') {
      current.end = inkEnd; current.next = p + n;
      current.advance = inkWidth; current.spaces = inkSpaces;
      current.paragraphEnd = true; return current;
    }
    if (cp == '\r') { p += n; continue; }
    // Legal CJK boundaries are independent of English word boundaries.
    if (p > from && !space(cp) && !invisible(cp) && !space(prev) &&
        (cjk(prev) || cjk(cp) || prev == 0x2013 || prev == 0x2014) &&
        !opening(prev) && !closing(cp) && !combining(cp) &&
        !noBreakSpace(prev) && !noBreakSpace(cp) && prev != 0x2060 &&
        current.advance <= available) {
      best = current; best.end = best.next = p;
    }
    if (space(cp)) {
      if (inkEnd > from) {
        best = current; best.end = inkEnd; best.next = p + n;
        best.advance = inkWidth; best.spaces = inkSpaces; best.hyphen = false;
      }
      if (prevSpace || cp != ' ') current.formatted = true;
    } else if (cp == 0x200b) {
      if (inkEnd > from) { best = current; best.end = inkEnd; best.next = p + n; }
    } else if (cp == 0xad) {
      if (allowHyphen && softPoint(text, length, p) &&
          current.advance + std::max(0, width("-", 1)) <= available) {
        best = current; best.end = best.next = p + n;
        best.advance += std::max(0, width("-", 1)); best.hyphen = true;
      }
    }
    int w = advance(text + p, n, cp, width);
    if (p > from && current.advance + w > available && !space(cp) && !invisible(cp) &&
        !combining(cp) && cp != 0x200d && prev != 0x200d) {
      // A closing mark that is the only thing over the edge hangs into the
      // margin instead of forcing the preceding word onto the next line.
      if (closing(cp) && !prevSpace && current.advance <= available) {
        int hang = w;
        size_t runEnd = p + n;
        size_t q = p + n;
        while (q < length) {
          size_t qn = utf8Bytes(text + q, length - q);
          uint32_t qc = codepoint(text + q, qn);
          if (!closing(qc)) break;
          hang += advance(text + q, qn, qc, width);
          runEnd = q + qn;
          q += qn;
        }
        if (g.hangLimit <= 0 || hang <= g.hangLimit) {
          current.end = current.next = runEnd;
          current.hang = hang;
          size_t next = runEnd;
          while (next < length) {
            size_t nn = utf8Bytes(text + next, length - next);
            uint32_t nc = codepoint(text + next, nn);
            if (space(nc) || nc == '\r') { next += nn; continue; }
            break;
          }
          if (next >= length) { current.next = length; current.paragraphEnd = true; }
          else if (text[next] == '\n') { current.next = next + 1; current.paragraphEnd = true; }
          else current.next = next;
          return current;
        }
      }
      if (g.hyphenate && allowHyphen) automatic(text, length, p, available, width, current, best);
      if (best.end > from) return best;
      current.end = current.next = p; current.emergency = true; return current;
    }
    current.advance += w;
    if (space(cp)) {
      if (inkEnd > from) ++current.spaces;
    } else if (!invisible(cp)) {
      current.latinText |= latin(cp);
      current.compatible &= latinCompatible(cp);
      inkEnd = p + n; inkWidth = current.advance; inkSpaces = current.spaces;
    }
    p += n;
    current.end = current.next = p;
    if (cp == '-' || cp == 0x2010 || cp == 0x2013 || cp == 0x2014 || cp == '/') {
      if (current.advance <= available) { best = current; best.end = best.next = p; }
    }
    prevSpace = space(cp); prev = cp;
    if ((++units & 255) == 0) tick();
    // A malicious stream of zero-width marks cannot monopolize one line.
    if (units == 8192) { current.emergency = true; return current; }
  }
  current.end = inkEnd; current.next = length;
  current.advance = inkWidth; current.spaces = inkSpaces;
  current.paragraphEnd = true; return current;
}

template<class Width, class Draw, class Tick>
size_t page(const char* text, size_t length, size_t from, Geometry g,
            Width width, Draw draw, Tick tick) {
  if (g.right <= g.left || g.lineHeight <= 0 || g.bottom < g.top) return from;
  size_t p = from;
  unsigned hyphenLines = 0;
  bool continuation = from && from < length &&
      (latin(previous(text, from)) || previous(text, from) == 0xad) &&
      latin(codepoint(text + from, utf8Bytes(text + from, length - from)));
  bool multiLine = g.top + g.lineHeight <= g.bottom;
  for (int y = g.top; y <= g.bottom && p < length; y += g.lineHeight) {
    bool lastLine = multiLine && y + g.lineHeight > g.bottom;
    bool allowHyphen = hyphenLines < 2 && !(y == g.top && continuation) && !lastLine;
    Line row = line(text, length, p, g, width, tick, allowHyphen);
    if (row.next <= p) return p;
    // A long word that cannot fit without a hyphen is moved to the next page
    // rather than split on the page's last line.
    if (lastLine && row.emergency) return p;
    int spare = g.right - g.left - row.advance;
    int spaceWidth = std::max(0, width(" ", 1));
    bool justify = g.justify && !row.paragraphEnd && row.latinText && row.compatible &&
        !row.formatted && !row.emergency && row.spaces > 0 && spare >= 0 &&
        spare <= row.spaces * std::max(1, spaceWidth * 3 / 4);
    int x = g.left, usedSpaces = 0;
    for (size_t at = row.begin; at < row.end;) {
      size_t n = utf8Bytes(text + at, row.end - at);
      uint32_t cp = codepoint(text + at, n);
      if (!invisible(cp)) {
        int w = advance(text + at, n, cp, width);
        if (space(cp) || noBreakSpace(cp)) draw(" ", 1, x, y);
        else draw(text + at, n, x, y);
        x += w;
        if (justify && space(cp) && at > row.begin) {
          // Integer distribution reaches the right edge without changing glyphs.
          int before = spare * usedSpaces / row.spaces;
          ++usedSpaces; x += spare * usedSpaces / row.spaces - before;
        }
      }
      at += n;
    }
    if (row.hyphen) draw("-", 1, x, y);
    hyphenLines = row.hyphen ? hyphenLines + 1 : 0;
    p = row.next; tick();
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
} // namespace pagination
