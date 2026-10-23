#include "math_layout.h"
#include "math_font.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace math_layout {
namespace {
using math_font::Style;
enum class Atom { Ordinary, Operator, Binary, Relation, Open, Close, Punctuation, Inner };
enum class Stop { End, Brace, Bracket, Right, Matrix };
class Scratch {
 public:
  bool ready(Workspace* workspace) {
    if (workspace) memory_ = workspace->data();
    else {
      storage_.reset(new (std::nothrow) uint8_t[MAX_SCRATCH_BYTES]);
      memory_ = storage_.get();
    }
    return memory_ != nullptr;
  }
  template<class T> T* allocate(size_t count) {
    static_assert(std::is_trivially_destructible<T>::value, "Arena values must not require destruction");
    const size_t aligned = (used_ + alignof(T) - 1) & ~(alignof(T) - 1);
    if (count > MAX_SCRATCH_BYTES / sizeof(T) || aligned > MAX_SCRATCH_BYTES - count * sizeof(T)) return nullptr;
    T* p = reinterpret_cast<T*>(memory_ + aligned);
    for (size_t i = 0; i < count; ++i) new (p + i) T();
    used_ = aligned + count * sizeof(T);
    return p;
  }
 private:
  std::unique_ptr<uint8_t[]> storage_;
  uint8_t* memory_ = nullptr;
  size_t used_ = 0;
};
struct Commands {
  Command* data = nullptr;
  size_t count = 0, capacity = 0;
  bool append(const Command& command, Scratch& scratch) {
    if (count == capacity) {
      const size_t next = capacity ? capacity * 2 : 4;
      if (next > MAX_COMMANDS) return false;
      Command* fresh = scratch.allocate<Command>(next);
      if (!fresh) return false;
      if (count) std::copy(data, data + count, fresh);
      data = fresh; capacity = next;
    }
    data[count++] = command; return true;
  }
  Command* begin() const { return data; }
  Command* end() const { return count ? data + count : data; }
  size_t size() const { return count; }
  bool empty() const { return count == 0; }
};
struct Token {
  const char* data = nullptr;
  size_t length = 0;
  Token() = default;
  Token(const char* p, size_t n) : data(p), length(n) {}
  bool operator==(const char* s) const { return length == std::strlen(s) && std::memcmp(data, s, length) == 0; }
  bool operator!=(const char* s) const { return !(*this == s); }
  bool operator==(const Token& t) const { return length == t.length && (!length || std::memcmp(data,t.data,length) == 0); }
  bool operator!=(const Token& t) const { return !(*this == t); }
  size_t size() const { return length; }
  char operator[](size_t i) const { return i < length ? data[i] : 0; }
};
struct Box {
  int width = 0, ascent = 0, descent = 0, italic = 0;
  Atom atom = Atom::Ordinary;
  bool limits = false;
  Commands commands;
};
struct Matrix {
  Box cells[MAX_MATRIX_ROWS][MAX_MATRIX_COLUMNS];
  unsigned columns[MAX_MATRIX_ROWS] = {};
};
struct Symbol { const char* name; uint32_t cp; Atom atom; };
const Symbol symbols[] = {
  {"alpha",0x3B1,Atom::Ordinary},{"beta",0x3B2,Atom::Ordinary},{"gamma",0x3B3,Atom::Ordinary},
  {"delta",0x3B4,Atom::Ordinary},{"epsilon",0x3F5,Atom::Ordinary},{"varepsilon",0x3B5,Atom::Ordinary},
  {"zeta",0x3B6,Atom::Ordinary},{"eta",0x3B7,Atom::Ordinary},{"theta",0x3B8,Atom::Ordinary},
  {"vartheta",0x3D1,Atom::Ordinary},{"iota",0x3B9,Atom::Ordinary},{"kappa",0x3BA,Atom::Ordinary},
  {"lambda",0x3BB,Atom::Ordinary},{"mu",0x3BC,Atom::Ordinary},{"nu",0x3BD,Atom::Ordinary},
  {"xi",0x3BE,Atom::Ordinary},{"pi",0x3C0,Atom::Ordinary},{"varpi",0x3D6,Atom::Ordinary},
  {"rho",0x3C1,Atom::Ordinary},{"varrho",0x3F1,Atom::Ordinary},{"sigma",0x3C3,Atom::Ordinary},
  {"varsigma",0x3C2,Atom::Ordinary},{"tau",0x3C4,Atom::Ordinary},{"upsilon",0x3C5,Atom::Ordinary},
  {"phi",0x3D5,Atom::Ordinary},{"varphi",0x3C6,Atom::Ordinary},{"chi",0x3C7,Atom::Ordinary},
  {"psi",0x3C8,Atom::Ordinary},{"omega",0x3C9,Atom::Ordinary},
  {"Gamma",0x393,Atom::Ordinary},{"Delta",0x394,Atom::Ordinary},{"Theta",0x398,Atom::Ordinary},
  {"Lambda",0x39B,Atom::Ordinary},{"Xi",0x39E,Atom::Ordinary},{"Pi",0x3A0,Atom::Ordinary},
  {"Sigma",0x3A3,Atom::Ordinary},{"Upsilon",0x3A5,Atom::Ordinary},{"Phi",0x3A6,Atom::Ordinary},
  {"Psi",0x3A8,Atom::Ordinary},{"Omega",0x3A9,Atom::Ordinary},
  {"times",0xD7,Atom::Binary},{"div",0xF7,Atom::Binary},{"cdot",0x22C5,Atom::Binary},
  {"pm",0xB1,Atom::Binary},{"mp",0x2213,Atom::Binary},{"circ",0x2218,Atom::Binary},
  {"ast",0x2217,Atom::Binary},{"star",0x22C6,Atom::Binary},{"oplus",0x2295,Atom::Binary},
  {"otimes",0x2297,Atom::Binary},{"cup",0x222A,Atom::Binary},{"cap",0x2229,Atom::Binary},
  {"wedge",0x2227,Atom::Binary},{"vee",0x2228,Atom::Binary},{"land",0x2227,Atom::Binary},
  {"lor",0x2228,Atom::Binary},{"setminus",0x2216,Atom::Binary},
  {"le",0x2264,Atom::Relation},{"leq",0x2264,Atom::Relation},{"ge",0x2265,Atom::Relation},
  {"geq",0x2265,Atom::Relation},{"ne",0x2260,Atom::Relation},{"neq",0x2260,Atom::Relation},
  {"approx",0x2248,Atom::Relation},{"equiv",0x2261,Atom::Relation},{"sim",0x223C,Atom::Relation},
  {"simeq",0x2243,Atom::Relation},{"cong",0x2245,Atom::Relation},{"propto",0x221D,Atom::Relation},
  {"ll",0x226A,Atom::Relation},{"gg",0x226B,Atom::Relation},{"in",0x2208,Atom::Relation},
  {"notin",0x2209,Atom::Relation},{"ni",0x220B,Atom::Relation},{"subset",0x2282,Atom::Relation},
  {"supset",0x2283,Atom::Relation},{"subseteq",0x2286,Atom::Relation},{"supseteq",0x2287,Atom::Relation},
  {"perp",0x22A5,Atom::Relation},{"parallel",0x2225,Atom::Relation},
  {"to",0x2192,Atom::Relation},{"rightarrow",0x2192,Atom::Relation},{"leftarrow",0x2190,Atom::Relation},
  {"leftrightarrow",0x2194,Atom::Relation},{"mapsto",0x21A6,Atom::Relation},
  {"Rightarrow",0x21D2,Atom::Relation},{"Leftarrow",0x21D0,Atom::Relation},{"Leftrightarrow",0x21D4,Atom::Relation},
  {"implies",0x21D2,Atom::Relation},{"iff",0x21D4,Atom::Relation},
  {"infty",0x221E,Atom::Ordinary},{"partial",0x2202,Atom::Ordinary},{"nabla",0x2207,Atom::Ordinary},
  {"forall",0x2200,Atom::Ordinary},{"exists",0x2203,Atom::Ordinary},{"neg",0xAC,Atom::Ordinary},
  {"emptyset",0x2205,Atom::Ordinary},{"varnothing",0x2205,Atom::Ordinary},
  {"ell",0x2113,Atom::Ordinary},{"hbar",0x210F,Atom::Ordinary},{"Re",0x211C,Atom::Ordinary},
  {"Im",0x2111,Atom::Ordinary},{"aleph",0x2135,Atom::Ordinary},{"angle",0x2220,Atom::Ordinary},
  {"prime",0x2032,Atom::Ordinary},{"ldots",0x2026,Atom::Inner},{"dots",0x2026,Atom::Inner},
  {"cdots",0x22EF,Atom::Inner},{"vdots",0x22EE,Atom::Inner},{"ddots",0x22F1,Atom::Inner},
  {"langle",0x27E8,Atom::Open},{"rangle",0x27E9,Atom::Close},
  {"lceil",0x2308,Atom::Open},{"rceil",0x2309,Atom::Close},
  {"lfloor",0x230A,Atom::Open},{"rfloor",0x230B,Atom::Close},
  {"lbrace",'{',Atom::Open},{"rbrace",'}',Atom::Close},{"vert",'|',Atom::Ordinary},{"Vert",0x2016,Atom::Ordinary}
};
bool letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
uint8_t smaller(uint8_t size) { return size == 28 ? 20 : 14; }

class Parser {
 public:
  Parser(const char* source, size_t length, const Options& options,
         Workspace* workspace = nullptr, Command* output = nullptr, size_t capacity = 0)
      : source_(source), length_(length), options_(options), workspace_(workspace), output_(output), capacity_(capacity) {}
  Result run() {
    Result result;
    if (!source_ || !length_ || options_.maxWidth <= 0 || options_.maxHeight <= 0) return result;
    if (length_ > MAX_SOURCE_BYTES) { result.status = Status::Limit; return result; }
    if ((workspace_ && (!output_ || !capacity_)) || !scratch_.ready(workspace_)) { result.status = Status::Limit; return result; }
    if (!tick(true)) { result.status = status_; return result; }
    Box box = row(28, Style::Normal, options_.displayStyle, 0, Stop::End);
    skip();
    // Negative explicit spacing may reduce advance, but cannot reduce ink bounds.
    for (const auto& c : box.commands) {
      const auto* g = c.kind == CommandKind::Glyph ? math_font::find(c.codepoint, c.pixels) : nullptr;
      box.width = std::max(box.width, int(c.x) + (g ? g->left + g->width : c.width));
    }
    if (status_ == Status::Ok && (position_ != length_ || box.commands.empty())) fail(Status::Invalid);
    if (status_ == Status::Ok && (box.width > options_.maxWidth || box.ascent + box.descent > options_.maxHeight)) fail(Status::Limit);
    tick(true);
    result.status = status_;
    if (status_ != Status::Ok) return result;
    result.width = box.width;
    result.ascent = box.ascent;
    result.descent = box.descent;
    for (auto& command : box.commands) command.y += box.ascent;
    if (workspace_) {
      if (box.commands.count > capacity_) return failure(Status::Limit);
      std::copy(box.commands.begin(), box.commands.end(), output_);
      result.commands.borrow(output_, box.commands.count);
    } else if (!result.commands.assign(box.commands.data, box.commands.count)) return failure(Status::Limit);
    return result;
  }
 private:
  const char* source_;
  size_t length_, position_ = 0, nodes_ = 0, work_ = 0;
  const Options& options_;
  Status status_ = Status::Ok;
  Scratch scratch_;
  Workspace* workspace_;
  Command* output_;
  size_t capacity_;
  static Result failure(Status s) { Result r; r.status = s; return r; }
  void fail(Status s) { if (status_ == Status::Ok) status_ = s; }
  bool tick(bool now = false) {
    if ((now || (++work_ % 32) == 0) && options_.keepGoing && !options_.keepGoing(options_.context)) fail(Status::Cancelled);
    return status_ == Status::Ok;
  }
  void skip() {
    while (position_ < length_ && status_ == Status::Ok) {
      if (space(source_[position_])) { ++position_; tick(); }
      else if (source_[position_] == '%') {
        while (position_ < length_ && source_[position_] != '\n' && source_[position_] != '\r') { ++position_; if (!tick()) break; }
      } else break;
    }
  }
  bool startsCommand(const char* name) const {
    const size_t n = std::strlen(name);
    return position_ < length_ && source_[position_] == '\\' && n < length_ - position_ &&
        std::memcmp(source_ + position_ + 1, name, n) == 0 &&
        (position_ + 1 + n == length_ || !letter(source_[position_ + 1 + n]));
  }
  Token command() {
    if (position_ >= length_ || source_[position_++] != '\\') { fail(Status::Invalid); return {}; }
    const size_t start = position_;
    if (position_ < length_ && letter(source_[position_])) {
      while (position_ < length_ && letter(source_[position_])) { ++position_; if (!tick()) break; }
    } else if (position_ < length_) ++position_;
    if (position_ == start) fail(Status::Invalid);
    return Token(source_ + start, position_ - start);
  }
  uint32_t scalar() {
    if (position_ >= length_) { fail(Status::Invalid); return 0; }
    const unsigned char lead = static_cast<unsigned char>(source_[position_++]);
    if (lead < 0x80) { if (lead < 0x20 && !space(static_cast<char>(lead))) fail(Status::Invalid); return lead; }
    unsigned remaining = lead >= 0xC2 && lead <= 0xDF ? 1 : lead >= 0xE0 && lead <= 0xEF ? 2 : lead >= 0xF0 && lead <= 0xF4 ? 3 : 0;
    if (!remaining) { fail(Status::Invalid); return 0; }
    const unsigned count = remaining;
    uint32_t cp = lead & (0x7F >> (count + 1));
    while (remaining--) {
      if (position_ >= length_) { fail(Status::Invalid); return 0; }
      const unsigned char byte = static_cast<unsigned char>(source_[position_++]);
      if ((byte & 0xC0) != 0x80) { fail(Status::Invalid); return 0; }
      cp = (cp << 6) | (byte & 0x3F);
    }
    if ((count == 1 && cp < 0x80) || (count == 2 && cp < 0x800) || (count == 3 && cp < 0x10000) ||
        (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) fail(Status::Invalid);
    // Internal PUA glyph identifiers must never be accepted from book content.
    if (cp >= 0xE000 && cp <= 0xF8FF) fail(Status::Unsupported);
    return cp;
  }
  Box glyph(uint32_t cp, uint8_t size, Style style = Style::Roman) {
    Box b;
    cp = math_font::styled(cp, style);
    const auto* g = cp ? math_font::find(cp, size) : nullptr;
    if (!g) { fail(Status::Unsupported); return b; }
    const int x = std::max(0, -int(g->left));
    b.width = std::max(int(g->advance), int(g->left + g->width)) + x;
    b.ascent = std::max(0, -int(g->top));
    b.descent = std::max(0, int(g->top + g->height));
    b.italic = g->italicCorrection;
    if (g->width && g->height && !b.commands.append({CommandKind::Glyph, size, cp, static_cast<int16_t>(x), 0, 0, 0}, scratch_)) fail(Status::Limit);
    return b;
  }
  void add(Box& out, const Box& child, int x, int baseline) {
    if (status_ != Status::Ok) return;
    if (out.commands.size() + child.commands.size() > MAX_COMMANDS || std::abs(x) > 8192 || std::abs(baseline) > 8192) { fail(Status::Limit); return; }
    out.ascent = std::max(out.ascent, child.ascent - baseline);
    out.descent = std::max(out.descent, child.descent + baseline);
    for (auto c : child.commands) {
      const int cx = c.x + x, cy = c.y + baseline;
      if (std::abs(cx) > 8192 || std::abs(cy) > 8192) { fail(Status::Limit); return; }
      c.x = static_cast<int16_t>(cx); c.y = static_cast<int16_t>(cy);
      if (!out.commands.append(c, scratch_)) { fail(Status::Limit); return; }
    }
  }
  void rule(Box& b, int x, int y, int width, int height) {
    if (width <= 0 || height <= 0 || status_ != Status::Ok) return;
    if (width > 8192 || height > 8192 || b.commands.size() >= MAX_COMMANDS) { fail(Status::Limit); return; }
    if (!b.commands.append({CommandKind::Rule, 0, 0, static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(width), static_cast<int16_t>(height)}, scratch_)) { fail(Status::Limit); return; }
    b.ascent = std::max(b.ascent, -y); b.descent = std::max(b.descent, y + height);
  }
  bool stop(Stop kind) const {
    if (position_ == length_) return true;
    const char c = source_[position_];
    return (kind == Stop::Brace && c == '}') || (kind == Stop::Bracket && c == ']') ||
        (kind == Stop::Right && startsCommand("right")) ||
        (kind == Stop::Matrix && (c == '&' || startsCommand("end") ||
          (c == '\\' && position_ + 1 < length_ && source_[position_ + 1] == '\\')));
  }
  int spacing(Atom left, Atom right, uint8_t size) const {
    if (left == Atom::Open || right == Atom::Close || right == Atom::Punctuation) return 0;
    if (left == Atom::Relation || right == Atom::Relation) return std::max(2, int(size) / 4);
    if (left == Atom::Binary || right == Atom::Binary) return std::max(2, int(size) / 6);
    if (left == Atom::Operator || right == Atom::Operator || left == Atom::Punctuation) return std::max(2, int(size) / 7);
    return 0;
  }
  Box row(uint8_t size, Style style, bool display, unsigned depth, Stop ending) {
    Box out; bool first = true; Atom previous = Atom::Ordinary;
    if (depth > MAX_DEPTH) { fail(Status::Limit); return out; }
    while (tick()) {
      skip(); if (stop(ending)) break;
      if (++nodes_ > MAX_NODES) { fail(Status::Limit); break; }
      const size_t before = position_;
      Box child = scripted(size, style, display, depth + 1);
      if (position_ <= before) { fail(Status::Invalid); break; }
      if (child.atom == Atom::Binary && (first || previous == Atom::Binary || previous == Atom::Relation || previous == Atom::Open)) child.atom = Atom::Ordinary;
      if (!first && !child.commands.empty()) out.width += spacing(previous, child.atom, size);
      add(out, child, out.width, 0);
      out.width = std::max(0, out.width + child.width);
      out.italic = child.italic;
      if (!child.commands.empty()) { previous = child.atom; first = false; }
      if (out.width > 8192 || out.ascent + out.descent > 8192) { fail(Status::Limit); break; }
    }
    return out;
  }
  Box argument(uint8_t size, Style style, bool display, unsigned depth) {
    skip();
    if (depth > MAX_DEPTH) { fail(Status::Limit); return {}; }
    if (position_ >= length_) { fail(Status::Invalid); return {}; }
    if (source_[position_] == '{') {
      ++position_;
      Box b = row(size, style, display, depth + 1, Stop::Brace);
      if (position_ >= length_ || source_[position_] != '}') fail(Status::Invalid); else ++position_;
      return b;
    }
    if (++nodes_ > MAX_NODES) { fail(Status::Limit); return {}; }
    return primary(size, style, display, depth + 1);
  }
  Box scripted(uint8_t size, Style style, bool display, unsigned depth) {
    Box base = primary(size, style, display, depth);
    Box upper, lower; bool hasUpper = false, hasLower = false;
    skip();
    if (startsCommand("limits") || startsCommand("nolimits")) {
      if (base.atom != Atom::Operator) { fail(Status::Invalid); return {}; }
      base.limits = command() == "limits";
      skip();
    }
    while (position_ < length_ && status_ == Status::Ok) {
      const char c = source_[position_];
      if (c != '^' && c != '_' && c != '\'') break;
      ++position_;
      if (c == '\'') {
        if (hasUpper) { fail(Status::Invalid); break; }
        upper = glyph(0x2032, smaller(size)); hasUpper = true;
        while (position_ < length_ && source_[position_] == '\'') {
          if (++nodes_ > MAX_NODES) { fail(Status::Limit); break; }
          if (!tick()) break;
          ++position_; Box prime = glyph(0x2032, smaller(size)); add(upper, prime, upper.width, 0); upper.width += prime.width;
        }
      } else {
        if ((c == '^' && hasUpper) || (c == '_' && hasLower)) { fail(Status::Invalid); break; }
        Box script = argument(smaller(size), style, false, depth + 1);
        if (c == '^') { upper = std::move(script); hasUpper = true; }
        else { lower = std::move(script); hasLower = true; }
      }
      skip();
    }
    if (!hasUpper && !hasLower) return base;
    const auto& c = math_font::constants(size);
    Box out; out.atom = base.atom;
    if (base.limits) {
      out.width = std::max(base.width, std::max(upper.width, lower.width));
      add(out, base, (out.width - base.width) / 2, 0);
      if (hasUpper) add(out, upper, (out.width - upper.width) / 2, -base.ascent - c.limitGap - upper.descent);
      if (hasLower) add(out, lower, (out.width - lower.width) / 2, base.descent + c.limitGap + lower.ascent);
    } else {
      add(out, base, 0, 0);
      int up = std::max(int(c.supShift), base.ascent - int(size) / 4);
      up = std::max(up, upper.descent + int(size) / 8);
      int down = std::max(int(c.subShift), base.descent + lower.ascent - int(size) / 3);
      if (hasUpper && hasLower) down = std::max(down, lower.ascent + upper.descent - up + c.scriptGap);
      if (hasUpper) add(out, upper, base.width + base.italic, -up);
      if (hasLower) add(out, lower, base.width, down);
      out.width = base.width + std::max(hasUpper ? upper.width + base.italic : 0, lower.width) + c.scriptSpace;
    }
    return out;
  }
  Box fraction(Box numerator, Box denominator, uint8_t size, bool display, bool withRule) {
    Box b; b.atom = Atom::Inner;
    const auto& c = math_font::constants(size);
    const int padding = std::max(2, int(size) / 10), gap = display ? c.displayFractionGap : c.fractionGap;
    const int thickness = withRule ? c.rule : 0;
    b.width = std::max(numerator.width, denominator.width) + 2 * padding;
    const int y = -c.axis - thickness / 2;
    add(b, numerator, (b.width - numerator.width) / 2, y - gap - numerator.descent);
    add(b, denominator, (b.width - denominator.width) / 2, y + thickness + gap + denominator.ascent);
    if (withRule) rule(b, 0, y, b.width, thickness);
    return b;
  }
  uint32_t delimiter() {
    skip();
    if (position_ >= length_) { fail(Status::Invalid); return 0; }
    if (source_[position_] != '\\') {
      const uint32_t cp = scalar();
      if (cp == '.') return 0;
      if (cp == '(' || cp == ')' || cp == '[' || cp == ']' || cp == '|' || cp == '/') return cp;
      fail(Status::Unsupported); return 0;
    }
    const Token name = command();
    if (name == "{" || name == "}") return name[0];
    for (const auto& s : symbols) if (name == s.name && (s.atom == Atom::Open || s.atom == Atom::Close || name == "vert" || name == "Vert")) return s.cp;
    fail(Status::Unsupported); return 0;
  }
  Box stretch(uint32_t cp, uint8_t size, int height) {
    if (!cp) return {};
    const auto* g = math_font::variant(cp, size, height);
    if (!g) { fail(Status::Limit); return {}; }
    Box b = glyph(g->codepoint, size);
    const int baseline = -math_font::constants(size).axis - g->height / 2 - g->top;
    Box out; out.width = b.width; add(out, b, 0, baseline); return out;
  }
  Box fenced(Box content, uint32_t left, uint32_t right, uint8_t size) {
    const int height = std::max(int(size) * 3 / 4, content.ascent + content.descent);
    Box l = stretch(left, size, height), r = stretch(right, size, height), out;
    const int gap = std::max(1, int(size) / 14);
    add(out, l, 0, 0);
    add(out, content, l.width + (left ? gap : 0), 0);
    add(out, r, l.width + (left ? gap : 0) + content.width + (right ? gap : 0), 0);
    out.width = l.width + content.width + r.width + (left ? gap : 0) + (right ? gap : 0);
    out.atom = Atom::Inner; return out;
  }
  Box radical(Box content, Box degree, uint8_t size, bool display) {
    const auto& c = math_font::constants(size);
    const int gap = display ? c.displayRadicalGap : c.radicalGap;
    const int top = -content.ascent - gap - c.rule;
    const auto* g = math_font::variant(0x221A, size, content.ascent + content.descent + gap + c.rule);
    if (!g) { fail(Status::Limit); return {}; }
    Box root = glyph(g->codepoint, size), out;
    const int degreeSpace = degree.width ? std::max(0, degree.width - root.width / 2) : 0;
    const int bodyX = degreeSpace + root.width;
    add(out, root, degreeSpace, top - g->top);
    add(out, content, bodyX, 0);
    rule(out, bodyX - 2, top, content.width + 4, c.rule);
    if (!degree.commands.empty()) add(out, degree, 0, top + g->height / 2 - degree.descent);
    out.width = bodyX + content.width + 2; return out;
  }
  Token environment() {
    skip();
    if (position_ >= length_ || source_[position_++] != '{') { fail(Status::Invalid); return {}; }
    const size_t start = position_;
    while (position_ < length_ && letter(source_[position_])) { ++position_; if (!tick()) break; }
    if (position_ - start > 20 || position_ >= length_ || source_[position_] != '}') { fail(Status::Invalid); return {}; }
    const Token value(source_ + start, position_ - start); ++position_; return value;
  }
  Box matrix(uint8_t size, Style style, bool display, unsigned depth) {
    const Token env = environment();
    const bool aligned = env == "aligned";
    if (env != "matrix" && env != "pmatrix" && env != "bmatrix" && env != "Bmatrix" && env != "vmatrix" && env != "Vmatrix" && env != "cases" && !aligned) { fail(Status::Unsupported); return {}; }
    Matrix* matrix = scratch_.allocate<Matrix>(1);
    if (!matrix) { fail(Status::Limit); return {}; }
    unsigned rowCount = 1;
    while (tick()) {
      if (rowCount > MAX_MATRIX_ROWS || matrix->columns[rowCount-1] >= MAX_MATRIX_COLUMNS) { fail(Status::Limit); return {}; }
      const unsigned column = matrix->columns[rowCount-1]++;
      matrix->cells[rowCount-1][column] = row(size, style, display, depth + 1, Stop::Matrix);
      if (position_ >= length_) { fail(Status::Invalid); return {}; }
      if (source_[position_] == '&') { ++position_; continue; }
      if (startsCommand("end")) { command(); if (environment() != env) fail(Status::Invalid); break; }
      if (source_[position_] == '\\' && position_ + 1 < length_ && source_[position_ + 1] == '\\') {
        position_ += 2; skip();
        if (startsCommand("end")) { command(); if (environment() != env) fail(Status::Invalid); break; }
        ++rowCount; continue;
      }
      fail(Status::Invalid); return {};
    }
    if (status_ != Status::Ok) return {};
    size_t columns = matrix->columns[0];
    if (aligned) for (unsigned r = 1; r < rowCount; ++r) columns = std::max(columns, size_t(matrix->columns[r]));
    int widths[MAX_MATRIX_COLUMNS] = {}, ascents[MAX_MATRIX_ROWS] = {}, descents[MAX_MATRIX_ROWS] = {};
    for (size_t r = 0; r < rowCount; ++r) {
      if ((!aligned && matrix->columns[r] != columns) || (env == "cases" && columns != 2)) { fail(Status::Invalid); return {}; }
      for (size_t col = 0; col < columns; ++col) {
        const auto& b = matrix->cells[r][col]; widths[col] = std::max(widths[col], b.width);
        ascents[r] = std::max(ascents[r], b.ascent); descents[r] = std::max(descents[r], b.descent);
      }
    }
    const int rowGap = std::max(3, int(size) / 3), colGap = std::max(4, int(size) / 2);
    int height = 0; for (size_t r = 0; r < rowCount; ++r) height += ascents[r] + descents[r] + (r ? rowGap : 0);
    Box out; int y = -height / 2 - math_font::constants(size).axis;
    for (size_t r = 0; r < rowCount; ++r) {
      int x = 0; y += ascents[r];
      for (size_t col = 0; col < columns; ++col) {
        const int spare = widths[col] - matrix->cells[r][col].width;
        const int inset = aligned ? (col % 2 == 0 ? spare : 0) : env == "cases" ? 0 : spare / 2;
        add(out, matrix->cells[r][col], x + inset, y);
        x += widths[col];
        if (col + 1 < columns) x += aligned ? (col % 2 == 0 ? std::max(2, int(size) / 4) : size) : colGap;
      }
      out.width = x; y += descents[r] + rowGap;
    }
    if (env == "pmatrix") return fenced(std::move(out), '(', ')', size);
    if (env == "bmatrix") return fenced(std::move(out), '[', ']', size);
    if (env == "Bmatrix") return fenced(std::move(out), '{', '}', size);
    if (env == "vmatrix") return fenced(std::move(out), '|', '|', size);
    if (env == "Vmatrix") return fenced(std::move(out), 0x2016, 0x2016, size);
    if (env == "cases") return fenced(std::move(out), '{', 0, size);
    out.atom = Atom::Inner; return out;
  }
  Box text(uint8_t size) {
    Box out; skip();
    if (position_ >= length_ || source_[position_++] != '{') { fail(Status::Invalid); return out; }
    while (position_ < length_ && tick()) {
      if (source_[position_] == '}') { ++position_; return out; }
      uint32_t cp;
      if (source_[position_] == '\\') {
        const Token name = command();
        if (name.size() != 1 || std::strchr("{}%$#&_ ", name[0]) == nullptr) { fail(Status::Unsupported); return {}; }
        cp = static_cast<unsigned char>(name[0]);
      } else {
        if (source_[position_] == '{') { fail(Status::Unsupported); return {}; }
        cp = scalar();
      }
      if (++nodes_ > MAX_NODES) { fail(Status::Limit); return {}; }
      if (cp == '\t' || cp == '\r' || cp == '\n') cp = ' ';
      Box b = glyph(cp, size); add(out, b, out.width, 0); out.width += b.width;
    }
    fail(Status::Invalid); return {};
  }
  Box primary(uint8_t size, Style style, bool display, unsigned depth) {
    skip();
    if (depth > MAX_DEPTH) { fail(Status::Limit); return {}; }
    if (position_ >= length_) { fail(Status::Invalid); return {}; }
    if (source_[position_] == '{') return argument(size, style, display, depth);
    if (source_[position_] != '\\') {
      uint32_t cp = scalar();
      if (cp == '}' || cp == '^' || cp == '_' || cp == '&' || cp == '$' || cp == '#' || cp == '~') { fail(Status::Invalid); return {}; }
      if (cp == '-') cp = 0x2212;
      Box b = glyph(cp, size, style);
      if (cp == '+' || cp == 0x2212 || cp == '*') b.atom = Atom::Binary;
      else if (cp == '=' || cp == '<' || cp == '>') b.atom = Atom::Relation;
      else if (cp == '(' || cp == '[') b.atom = Atom::Open;
      else if (cp == ')' || cp == ']') b.atom = Atom::Close;
      else if (cp == ',' || cp == ';' || cp == ':') b.atom = Atom::Punctuation;
      return b;
    }
    const Token name = command();
    if (name == "right" || name == "end" || name == "limits" || name == "nolimits") { fail(Status::Invalid); return {}; }
    if (name == "frac" || name == "dfrac" || name == "tfrac" || name == "binom" || name == "dbinom" || name == "tbinom") {
      const bool full = name[0] == 'd' || (name[0] != 't' && display);
      const bool binomial = name == "binom" || name == "dbinom" || name == "tbinom";
      const uint8_t childSize = full ? size : smaller(size);
      Box top = argument(childSize, style, false, depth + 1);
      Box bottom = argument(childSize, style, false, depth + 1);
      Box b = fraction(std::move(top), std::move(bottom), size, full, !binomial);
      return binomial ? fenced(std::move(b), '(', ')', size) : b;
    }
    if (name == "sqrt") {
      Box degree; skip();
      if (position_ < length_ && source_[position_] == '[') {
        ++position_; degree = row(14, style, false, depth + 1, Stop::Bracket);
        if (position_ >= length_ || source_[position_] != ']') fail(Status::Invalid); else ++position_;
      }
      Box content = argument(size, style, display, depth + 1);
      return radical(std::move(content), std::move(degree), size, display);
    }
    if (name == "left") {
      const uint32_t left = delimiter();
      Box content = row(size, style, display, depth + 1, Stop::Right);
      if (!startsCommand("right")) { fail(Status::Invalid); return {}; }
      command(); const uint32_t right = delimiter(); return fenced(std::move(content), left, right, size);
    }
    if (name == "begin") return matrix(size, style, display, depth + 1);
    if (name == "text" || name == "operatorname") {
      Box b = text(size); if (name == "operatorname") b.atom = Atom::Operator; return b;
    }
    if (name == "mathrm" || name == "mathit" || name == "mathbf" || name == "mathbb" || name == "mathcal") {
      const Style selected = name == "mathrm" ? Style::Roman : name == "mathit" ? Style::Italic : name == "mathbf" ? Style::Bold : name == "mathbb" ? Style::Blackboard : Style::Calligraphic;
      return argument(size, selected, display, depth + 1);
    }
    if (name == "overline" || name == "bar" || name == "underline" || name == "hat" || name == "vec") {
      Box b = argument(size, style, display, depth + 1);
      const auto& c = math_font::constants(size);
      if (name == "hat" || name == "vec") {
        Box accent = glyph(name == "hat" ? '^' : 0x2192, size);
        if (accent.width > b.width) {
          Box centered; centered.width = accent.width; add(centered, b, (accent.width - b.width) / 2, 0); b = std::move(centered);
        }
        const auto* accentGlyph = math_font::find(name == "hat" ? '^' : 0x2192, size);
        const int inkBottom = accentGlyph->top + accentGlyph->height;
        const int y = -b.ascent - 2 - inkBottom;
        add(b, accent, (b.width - accent.width) / 2, y);
      } else rule(b, 0, name == "underline" ? b.descent + 2 : -b.ascent - 2 - c.rule, b.width, c.rule);
      return b;
    }
    if (name == "sum" || name == "prod" || name == "int" || name == "oint") {
      const uint32_t cp = name == "sum" ? 0x2211 : name == "prod" ? 0x220F : name == "int" ? 0x222B : 0x222E;
      const auto* g = display ? math_font::variant(cp, size, size + size / 5) : math_font::find(cp, size);
      if (!g) { fail(Status::Unsupported); return {}; }
      Box b = glyph(g->codepoint, size); b.atom = Atom::Operator;
      b.limits = display && (name == "sum" || name == "prod"); return b;
    }
    const char* functions[] = {"sin","cos","tan","cot","sec","csc","sinh","cosh","tanh","arcsin","arccos","arctan","log","ln","exp","lim","max","min","sup","inf","det","gcd","Pr"};
    for (const char* fn : functions) if (name == fn) {
      Box b; for (const char* p = fn; *p; ++p) { Box ch = glyph(*p, size); add(b, ch, b.width, 0); b.width += ch.width; }
      b.atom = Atom::Operator;
      b.limits = display && (name == "lim" || name == "max" || name == "min" || name == "sup" || name == "inf"); return b;
    }
    if (name == "," || name == ":" || name == ";" || name == " " || name == "quad" || name == "qquad" || name == "!") {
      Box b; b.width = name == "qquad" ? size * 2 : name == "quad" ? size : name == " " ? size / 3 : name == ";" ? size * 5 / 18 : name == ":" ? size * 2 / 9 : name == "!" ? -int(size) / 6 : size / 6; return b;
    }
    if (name.size() == 1 && std::strchr("{}%$#&_", name[0])) return glyph(name[0], size);
    for (const auto& s : symbols) if (name == s.name) { Box b = glyph(s.cp, size, s.atom == Atom::Ordinary ? style : Style::Roman); b.atom = s.atom; return b; }
    fail(Status::Unsupported); return {};
  }
};
}  // namespace
CommandList::CommandList(CommandList&& other) noexcept
    : storage_(std::move(other.storage_)), view_(other.view_), size_(other.size_) { other.view_ = nullptr; other.size_ = 0; }
CommandList& CommandList::operator=(CommandList&& other) noexcept {
  if (this != &other) { storage_ = std::move(other.storage_); view_ = other.view_; size_ = other.size_; other.view_ = nullptr; other.size_ = 0; }
  return *this;
}
bool CommandList::assign(const Command* source, size_t count) {
  if (count > MAX_COMMANDS || (count && !source)) return false;
  std::unique_ptr<Command[]> fresh(count ? new (std::nothrow) Command[count] : nullptr);
  if (count && !fresh) return false;
  if (count) std::copy(source, source + count, fresh.get());
  storage_ = std::move(fresh); view_ = storage_.get(); size_ = count; return true;
}
void CommandList::borrow(const Command* source, size_t count) {
  storage_.reset(); view_ = source; size_ = count;
}
bool Workspace::prepare() {
  if (!storage_) storage_.reset(new (std::nothrow) uint8_t[MAX_SCRATCH_BYTES]);
  return bool(storage_);
}
Result layout(const char* source, size_t length, const Options& options) { return Parser(source, length, options).run(); }
Result layoutInto(const char* source, size_t length, const Options& options,
                  Workspace& workspace, Command* output, size_t capacity) {
  return Parser(source, length, options, &workspace, output, capacity).run();
}
const char* statusName(Status status) {
  switch (status) {
    case Status::Ok: return "ok";
    case Status::Invalid: return "invalid";
    case Status::Unsupported: return "unsupported";
    case Status::Limit: return "limit";
    case Status::Cancelled: return "cancelled";
  }
  return "invalid";
}
}  // namespace math_layout
