#pragma once
// Bounded XML preparation before TinyXML2 allocates or recurses. No DTD loading.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <tinyxml2.h>
#include "xhtml_entities.h"

namespace epub { namespace xml_input {
constexpr size_t MAX_BYTES = 1024 * 1024;
constexpr unsigned MAX_DEPTH = 32;
constexpr size_t MAX_DOM_BYTES = 2 * 1024 * 1024;
constexpr size_t POOL_RESERVE = 24 * 1024;
// TinyXML2 allocates nodes in ~4 KiB pools, not one heap allocation per node.
// Account for this target's actual sizes (host pointers are larger), rounded
// up to allocator alignment; reserve covers pool slack and transient end tags.
constexpr size_t alignedCost(size_t size) { return (size + 15) & ~size_t(15); }
constexpr size_t ELEMENT_COST = alignedCost(sizeof(tinyxml2::XMLElement));
constexpr size_t TEXT_COST = alignedCost(sizeof(tinyxml2::XMLText));
constexpr size_t COMMENT_COST = alignedCost(sizeof(tinyxml2::XMLComment));
constexpr size_t ATTRIBUTE_COST = alignedCost(sizeof(tinyxml2::XMLAttribute));
using Buffer = std::unique_ptr<char, decltype(&free)>;
struct Budget { size_t domBytes = POOL_RESERVE; size_t outputBytes = 0; bool rewrite = false; };
inline bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
inline bool xmlChar(uint32_t c) {
  return c == 9 || c == 10 || c == 13 || (c >= 0x20 && c <= 0xd7ff) ||
         (c >= 0xe000 && c <= 0xfffd) || (c >= 0x10000 && c <= 0x10ffff);
}
inline bool fail(std::string& error, const char* message) { error = message; return false; }
inline bool utf8(const char* text, size_t size, std::string& error) {
  size_t at = 0;
  while (at < size) {
    uint8_t first = uint8_t(text[at++]); uint32_t code = first, minimum = 0; unsigned extra = 0;
    if (first >= 0xc2 && first <= 0xdf) { code &= 31; extra = 1; minimum = 0x80; }
    else if (first >= 0xe0 && first <= 0xef) { code &= 15; extra = 2; minimum = 0x800; }
    else if (first >= 0xf0 && first <= 0xf4) { code &= 7; extra = 3; minimum = 0x10000; }
    else if (first >= 0x80) return fail(error, "Invalid XML UTF-8");
    if (extra > size - at) return fail(error, "Invalid XML UTF-8");
    while (extra--) { uint8_t next = uint8_t(text[at++]); if ((next & 0xc0) != 0x80) return fail(error, "Invalid XML UTF-8"); code = (code << 6) | (next & 63); }
    if (code < minimum || !xmlChar(code)) return fail(error, "Invalid XML Unicode character");
  }
  return true;
}
inline size_t encodedSize(uint32_t code) { return code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4; }
inline void encode(char* out, size_t& at, uint32_t code) {
  if (code < 0x80) out[at++] = char(code);
  else if (code < 0x800) { out[at++] = char(0xc0 | (code >> 6)); out[at++] = char(0x80 | (code & 63)); }
  else if (code < 0x10000) { out[at++] = char(0xe0 | (code >> 12)); out[at++] = char(0x80 | ((code >> 6) & 63)); out[at++] = char(0x80 | (code & 63)); }
  else { out[at++] = char(0xf0 | (code >> 18)); out[at++] = char(0x80 | ((code >> 12) & 63)); out[at++] = char(0x80 | ((code >> 6) & 63)); out[at++] = char(0x80 | (code & 63)); }
}
inline bool decode(Buffer& input, size_t& size, const std::function<void*(size_t)>& alloc, std::string& error) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(input.get());
  bool little = size >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe;
  bool big = size >= 2 && bytes[0] == 0xfe && bytes[1] == 0xff;
  size_t start = little || big ? 2 : 0;
  if (!little && !big && size >= 4) { little = bytes[0] == '<' && bytes[1] == 0; big = bytes[0] == 0 && bytes[1] == '<'; }
  if (!little && !big) return utf8(input.get(), size, error);
  if ((size - start) % 2) return fail(error, "Invalid XML UTF-16");
  auto unit = [&](size_t at) -> uint16_t { return little ? bytes[at] | (uint16_t(bytes[at + 1]) << 8) : (uint16_t(bytes[at]) << 8) | bytes[at + 1]; };
  size_t needed = 0;
  for (unsigned pass = 0; pass < 2; ++pass) {
    Buffer decoded(nullptr, free);
    if (pass) { decoded.reset(static_cast<char*>(alloc(needed + 1))); if (!decoded) return fail(error, "Not enough PSRAM for XML"); }
    size_t written = 0;
    for (size_t at = start; at < size; at += 2) {
      uint32_t code = unit(at);
      if (code >= 0xd800 && code <= 0xdbff) {
        if (at + 3 >= size) return fail(error, "Invalid XML UTF-16 surrogate");
        uint16_t low = unit(at + 2); if (low < 0xdc00 || low > 0xdfff) return fail(error, "Invalid XML UTF-16 surrogate");
        code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00; at += 2;
      }
      if (!xmlChar(code)) return fail(error, "Invalid XML Unicode character");
      if (pass) encode(decoded.get(), written, code); else written += encodedSize(code);
      if (written > MAX_BYTES) return fail(error, "Decoded XML exceeds 1 MiB");
    }
    if (!pass) needed = written;
    else { decoded.get()[written] = 0; input = std::move(decoded); size = written; }
  }
  return true;
}

// This is a lexical resource preflight, not a second XML parser. TinyXML2 still
// validates tag matching and XML syntax after every allocation/recursion bound
// has been checked. Quotes, comments, CDATA and declarations are separate tokens.
class Scanner {
 public:
  Scanner(const char* input, size_t length, char* output, std::string& failure)
      : s(input), n(length), out(output), error(failure) {}
  Budget budget;
  bool run() {
    unsigned depth = 0;
    while (at < n) {
      if (s[at] != '<') {
        size_t end = at; while (end < n && s[end] != '<') ++end;
        if (!charge(TEXT_COST)) return false;
        bool onlyWhitespace = depth > 0;
        for (size_t i = at; i < end && onlyWhitespace; ++i) onlyWhitespace = space(s[i]);
        if (onlyWhitespace) {
          // TinyXML2 10's PEDANTIC mode still drops whitespace between sibling
          // elements. A numeric reference keeps this text node alive without
          // adding visible characters or reinterpreting user entity text.
          unsigned code = uint8_t(s[at]);
          size_t consumed = 1;
          if (code == '\r') {
            code = '\n';
            if (at + 1 < end && s[at + 1] == '\n') consumed = 2;
          }
          char numeric[8];int count = std::snprintf(numeric,sizeof(numeric),"&#%u;",code);
          if (!emit(numeric,size_t(count)) || !copy(at + consumed,end)) return false;
          budget.rewrite = true;
        } else if (!entities(at, end)) return false;
        at = end; continue;
      }
      if (starts("<!--")) { if (!opaque("-->", 4, COMMENT_COST)) return false; continue; }
      if (starts("<![CDATA[")) { if (!opaque("]]>", 9, TEXT_COST)) return false; continue; }
      if (starts("<?")) { if (!opaque("?>", 2, COMMENT_COST)) return false; continue; }
      if (starts("<!DOCTYPE")) {
        if (at + 9 == n || !space(s[at + 9])) return fail(error, "Invalid XML DOCTYPE");
        size_t end = at + 9; char quote = 0; unsigned subset = 0;
        for (; end < n; ++end) {
          char c = s[end];
          if (quote) { if (c == quote) quote = 0; continue; }
          if (end + 4 <= n && !std::memcmp(s + end, "<!--", 4)) {
            size_t finish = end + 4;
            while (finish + 3 <= n && std::memcmp(s + finish, "-->", 3)) ++finish;
            if (finish + 3 > n) return fail(error, "Unterminated XML DOCTYPE comment");
            end = finish + 2; continue;
          }
          if (c == '\'' || c == '"') { quote = c; continue; }
          if (c == '[') { ++subset; continue; }
          if (c == ']') { if (!subset) return fail(error, "Invalid XML DOCTYPE"); --subset; continue; }
          if (c == '>' && !subset) break;
          // Custom entity declarations are intentionally unsupported. A fixed
          // XHTML entity table supplies named characters without external I/O.
          if (c == '<' && end + 8 <= n && !std::memcmp(s + end, "<!ENTITY", 8)) return fail(error, "Custom XML entities unsupported");
        }
        if (end == n || quote || subset) return fail(error, "Invalid XML DOCTYPE");
        if (!charge(COMMENT_COST)) return false;
        // TinyXML2 does not understand an internal subset; remove DOCTYPE as
        // one complete token after checking it. It is never loaded or fetched.
        budget.rewrite = true; at = end + 1; continue;
      }
      if (starts("<!")) return fail(error, "Unsupported XML declaration");
      bool closing = at + 1 < n && s[at + 1] == '/';
      size_t begin = at; at += closing ? 2 : 1;
      if (!name()) return false;
      if (closing) {
        while (at < n && space(s[at])) ++at;
        if (at == n || s[at++] != '>' || !depth) return fail(error, "Invalid XML closing tag");
        --depth; if (!copy(begin, at)) return false; continue;
      }
      if (!charge(ELEMENT_COST)) return false;
      if (depth >= MAX_DEPTH) return fail(error, "XML depth exceeds 32 levels");
      bool selfClosing = false;
      size_t copied = begin;
      while (at < n) {
        bool separated = false; while (at < n && space(s[at])) { separated = true; ++at; }
        if (at == n) return fail(error, "Unterminated XML tag");
        if (s[at] == '>') { ++at; break; }
        if (s[at] == '/' && at + 1 < n && s[at + 1] == '>') { at += 2; selfClosing = true; break; }
        if (!separated) return fail(error, "Invalid XML attribute");
        if (!name() || !charge(ATTRIBUTE_COST)) return false;
        while (at < n && space(s[at])) ++at;
        if (at == n || s[at++] != '=') return fail(error, "Invalid XML attribute");
        while (at < n && space(s[at])) ++at;
        if (at == n || (s[at] != '\'' && s[at] != '"')) return fail(error, "Unquoted XML attribute");
        char quote = s[at++]; size_t value = at;
        while (at < n && s[at] != quote) { if (s[at] == '<') return fail(error, "Invalid XML attribute"); ++at; }
        if (at == n) return fail(error, "Unterminated XML attribute");
        if (!copy(copied, value) || !entities(value, at)) return false;
        copied = at; ++at;
      }
      if (at > n || !at || s[at - 1] != '>') return fail(error, "Unterminated XML tag");
      if (!copy(copied, at)) return false;
      if (!selfClosing) ++depth;
    }
    if (depth) return fail(error, "Unclosed XML element");
    return true;
  }
 private:
  const char* s; size_t n, at = 0; char* out; std::string& error;
  bool starts(const char* token) const { size_t len = std::strlen(token); return len <= n - at && !std::memcmp(s + at, token, len); }
  bool charge(size_t bytes) { if (bytes > MAX_DOM_BYTES - budget.domBytes) return fail(error, "XML DOM resource budget exceeded"); budget.domBytes += bytes; return true; }
  bool emit(const char* value, size_t count) {
    if (count > MAX_BYTES - budget.outputBytes) return fail(error, "Decoded XML exceeds 1 MiB");
    if (out) std::memcpy(out + budget.outputBytes, value, count);
    budget.outputBytes += count; return true;
  }
  bool copy(size_t begin, size_t end) { return emit(s + begin, end - begin); }
  bool name() {
    size_t begin = at;
    while (at < n && !space(s[at]) && s[at] != '/' && s[at] != '>' && s[at] != '=') {
      if (s[at] == '<' || s[at] == '\'' || s[at] == '"' || s[at] == '&') return fail(error, "Invalid XML name");
      ++at;
    }
    return at != begin || fail(error, "Missing XML name");
  }
  bool opaque(const char* ending, size_t skip, size_t cost) {
    size_t len = std::strlen(ending), end = at + skip;
    while (end + len <= n && std::memcmp(s + end, ending, len)) ++end;
    if (end + len > n) return fail(error, "Unterminated XML token");
    if (!charge(cost) || !copy(at, end + len)) return false;
    at = end + len; return true;
  }
  bool entities(size_t begin, size_t end) {
    for (size_t i = begin; i < end;) {
      if (s[i] != '&') { size_t next = i + 1; while (next < end && s[next] != '&') ++next; if (!copy(i, next)) return false; i = next; continue; }
      size_t close = i + 1; while (close < end && s[close] != ';' && close - i <= 32) ++close;
      if (close == end || s[close] != ';') return fail(error, "Invalid XML entity");
      size_t length = close - i - 1; const char* name = s + i + 1;
      if (length && name[0] == '#') {
        size_t digit = 1; unsigned base = 10; uint32_t code = 0;
        if (digit < length && name[digit] == 'x') { base = 16; ++digit; }
        if (digit == length) return fail(error, "Invalid numeric XML entity");
        for (; digit < length; ++digit) {
          char c = name[digit]; unsigned v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
          if (v >= base || code > (0x10ffffu - v) / base) return fail(error, "Invalid numeric XML entity");
          code = code * base + v;
        }
        if (!xmlChar(code)) return fail(error, "Invalid XML Unicode character");
        if (!copy(i, close + 1)) return false;
      } else if ((length == 3 && (!std::memcmp(name, "amp", 3))) || (length == 2 && (!std::memcmp(name, "lt", 2) || !std::memcmp(name, "gt", 2))) || (length == 4 && (!std::memcmp(name, "apos", 4) || !std::memcmp(name, "quot", 4)))) {
        if (!copy(i, close + 1)) return false;
      } else {
        uint32_t code = xhtmlEntity(name, length);
        if (!code) return fail(error, "Unknown XML entity");
        char numeric[16]; int count = std::snprintf(numeric, sizeof(numeric), "&#%u;", unsigned(code));
        if (count < 0 || !emit(numeric, size_t(count))) return false;
        budget.rewrite = true;
      }
      i = close + 1;
    }
    return true;
  }
};
inline bool prepare(Buffer& input, size_t& size, const std::function<void*(size_t)>& alloc, Budget& budget, std::string& error) {
  if (!decode(input, size, alloc, error)) return false;
  Scanner scan(input.get(), size, nullptr, error);
  if (!scan.run()) return false;
  budget = scan.budget;
  if (!budget.rewrite) return true;
  Buffer normalized(static_cast<char*>(alloc(budget.outputBytes + 1)), free);
  if (!normalized) return fail(error, "Not enough PSRAM for XML");
  Scanner rewrite(input.get(), size, normalized.get(), error);
  if (!rewrite.run()) return false;
  normalized.get()[budget.outputBytes] = 0;
  input = std::move(normalized); size = budget.outputBytes; return true;
}
} }
