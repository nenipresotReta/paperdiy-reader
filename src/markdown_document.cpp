#include "markdown_document.h"
#include "xhtml_entities.h"
#include <md4c.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

namespace markdown {
namespace {
void* defaultResize(void* p, size_t n, void*) {
#if defined(ARDUINO_ARCH_ESP32)
  return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  return std::realloc(p, n);
#endif
}
void defaultRelease(void* p, void*) {
#if defined(ARDUINO_ARCH_ESP32)
  heap_caps_free(p);
#else
  std::free(p);
#endif
}
size_t encode(uint32_t cp, char* out) {
  if (!cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
  if (cp < 0x80) { out[0] = char(cp); return 1; }
  if (cp < 0x800) { out[0] = char(0xc0 | (cp >> 6)); out[1] = char(0x80 | (cp & 63)); return 2; }
  if (cp < 0x10000) {
    out[0] = char(0xe0 | (cp >> 12)); out[1] = char(0x80 | ((cp >> 6) & 63));
    out[2] = char(0x80 | (cp & 63)); return 3;
  }
  out[0] = char(0xf0 | (cp >> 18)); out[1] = char(0x80 | ((cp >> 12) & 63));
  out[2] = char(0x80 | ((cp >> 6) & 63)); out[3] = char(0x80 | (cp & 63)); return 4;
}
constexpr char atom[] = "\xef\xbf\xbc";
}

constexpr size_t Document::MaxSourceBytes;
constexpr size_t Document::MaxTextBytes;
constexpr size_t Document::MaxRuns;
constexpr size_t Document::MaxMappings;
constexpr size_t Document::ParserBudget;

Document::Document(Allocator allocator) : allocator_(allocator) {
  if (!allocator.reallocate && !allocator.release)
    allocator_ = {defaultResize, defaultRelease, nullptr};
}
Document::~Document() { discard(); }
void* Document::resize(void* p, size_t n) {
  return allocator_.reallocate ? allocator_.reallocate(p, n, allocator_.context) : nullptr;
}
void Document::release(void* p) {
  if (p && allocator_.release) allocator_.release(p, allocator_.context);
}
void Document::discard() {
  release(text_); release(runs_); release(mappings_);
  text_ = nullptr; runs_ = nullptr; mappings_ = nullptr;
  size_ = textCapacity_ = runCount_ = runCapacity_ = mappingCount_ = mappingCapacity_ = 0;
}
void Document::clear() {
  discard(); source_ = nullptr; sourceSize_ = 0; error_ = Error::None;
}
bool Document::fail(Error error) {
  if (error_ == Error::None) error_ = error;
  return false;
}
void* Document::reserve(void* memory, size_t& capacity, size_t need,
                       size_t maximum, size_t itemSize, Error limit) {
  if (need > maximum) { fail(limit); return nullptr; }
  if (need <= capacity) return memory;
  size_t next = capacity ? capacity : 64;
  while (next < need) next = std::min(maximum, next + next / 2);
  void* p = resize(memory, next * itemSize);
  if (!p) { fail(Error::Allocation); return nullptr; }
  capacity = next;
  return p;
}
bool Document::append(const char* bytes, size_t count, uint32_t sb,
                      uint32_t se, bool linear, Run style) {
  if (!count) return true;
  if (sb > se || se > sourceSize_ ||
      (mappingCount_ && sb < mappings_[mappingCount_ - 1].sourceEnd)) return fail(Error::Parser);
  if (count > MaxTextBytes - size_) return fail(Error::TextLimit);
  void* allocation = reserve(text_, textCapacity_, size_ + count + 1,
                             MaxTextBytes + 1, sizeof(char), Error::TextLimit);
  if (!allocation) return false;
  text_ = static_cast<char*>(allocation);
  const bool mergeRun = runCount_ && style.kind == Kind::Text &&
      runs_[runCount_ - 1].kind == Kind::Text &&
      runs_[runCount_ - 1].flags == style.flags &&
      runs_[runCount_ - 1].indent == style.indent &&
      runs_[runCount_ - 1].headingLevel == style.headingLevel;
  if (!mergeRun) {
    allocation = reserve(runs_, runCapacity_, runCount_ + 1, MaxRuns, sizeof(Run), Error::RunLimit);
    if (!allocation) return false;
    runs_ = static_cast<Run*>(allocation);
  }
  const bool mergeMap = mappingCount_ && linear && mappings_[mappingCount_ - 1].linear &&
      mappings_[mappingCount_ - 1].sourceEnd == sb;
  if (!mergeMap) {
    allocation = reserve(mappings_, mappingCapacity_, mappingCount_ + 1,
                         MaxMappings, sizeof(Mapping), Error::MappingLimit);
    if (!allocation) return false;
    mappings_ = static_cast<Mapping*>(allocation);
  }
  std::memcpy(text_ + size_, bytes, count);
  const uint32_t end = uint32_t(size_ + count);
  if (mergeRun) {
    runs_[runCount_ - 1].end = end;
    runs_[runCount_ - 1].sourceEnd = se;
  } else {
    style.begin = uint32_t(size_); style.end = end;
    style.sourceBegin = sb; style.sourceEnd = se;
    runs_[runCount_++] = style;
  }
  if (mergeMap) {
    mappings_[mappingCount_ - 1].end = end;
    mappings_[mappingCount_ - 1].sourceEnd = se;
  } else mappings_[mappingCount_++] = {uint32_t(size_), end, sb, se, linear};
  size_ = end; text_[size_] = 0;
  return true;
}
const Run& Document::run(size_t index) const {
  static const Run empty;
  return index < runCount_ ? runs_[index] : empty;
}
size_t Document::runAt(size_t rendered) const {
  size_t lo = 0, hi = runCount_;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (runs_[mid].end <= rendered) lo = mid + 1; else hi = mid;
  }
  return lo;
}
size_t Document::sourceOffset(size_t rendered) const {
  if (rendered >= size_) return sourceSize_;
  size_t lo = 0, hi = mappingCount_;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (mappings_[mid].end <= rendered) lo = mid + 1; else hi = mid;
  }
  const Mapping& m = mappings_[lo];
  return m.linear ? m.sourceBegin + rendered - m.begin : m.sourceBegin;
}
size_t Document::renderOffset(size_t original) const {
  if (original >= sourceSize_) return size_;
  size_t lo = 0, hi = mappingCount_;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (mappings_[mid].sourceEnd <= original) lo = mid + 1; else hi = mid;
  }
  if (lo == mappingCount_) return size_;
  const Mapping& m = mappings_[lo];
  return m.linear && original >= m.sourceBegin ? m.begin + original - m.sourceBegin : m.begin;
}

class Builder {
 public:
  Builder(Document& document, Tick tick, void* context, MathAccept accept, void* mathContext)
      : d(document), tick_(tick), context_(context), accept_(accept), mathContext_(mathContext) {}

  bool parse() {
    MD_PARSER parser{};
    parser.flags = MD_FLAG_NOHTML | MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH |
                   MD_FLAG_TASKLISTS | MD_FLAG_LATEXMATHSPANS;
    parser.enter_block = enterBlock; parser.leave_block = leaveBlock;
    parser.enter_span = enterSpan; parser.leave_span = leaveSpan; parser.text = text;
    MD_PAPERDIY_OPTIONS options{allocateParser, resizeParser, releaseParser, parserTick, this};
    const int result = md_parse_ex(d.source_, MD_SIZE(d.sourceSize_), &parser, this, &options);
    if (result != 0 || d.error_ != Error::None) return d.fail(Error::Parser);
    return true;
  }

  bool validUtf8() {
    size_t nextTick = 0;
    for (size_t p = 0; p < d.sourceSize_;) {
      if (p >= nextTick) { tick(); nextTick = p + 4096; }
      const auto c = uint8_t(d.source_[p++]);
      if (!c) return d.fail(Error::InvalidInput);
      if (c < 128) continue;
      unsigned remaining; uint32_t cp, minimum;
      if (c >= 0xc2 && c <= 0xdf) { remaining = 1; cp = c & 31; minimum = 0x80; }
      else if (c >= 0xe0 && c <= 0xef) { remaining = 2; cp = c & 15; minimum = 0x800; }
      else if (c >= 0xf0 && c <= 0xf4) { remaining = 3; cp = c & 7; minimum = 0x10000; }
      else return d.fail(Error::InvalidUtf8);
      if (remaining > d.sourceSize_ - p) return d.fail(Error::InvalidUtf8);
      while (remaining--) {
        const auto continuation = uint8_t(d.source_[p++]);
        if ((continuation & 0xc0) != 0x80) return d.fail(Error::InvalidUtf8);
        cp = (cp << 6) | (continuation & 63);
      }
      if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
        return d.fail(Error::InvalidUtf8);
    }
    return true;
  }

 private:
  Document& d;
  Tick tick_; void* context_;
  MathAccept accept_; void* mathContext_;
  Run style_{};
  struct Frame { uint16_t flags; uint8_t indent, heading; unsigned listDepth; } frames_[128]{};
  unsigned depth_ = 0, listDepth_ = 0;
  struct List { uint32_t number; bool ordered; } lists_[64]{};
  unsigned tableDepth_ = 0, mathDepth_ = 0, pendingBreaks_ = 0;
  char marker_[32]{};
  size_t markerSize_ = 0, leadingBreaks_ = 0, leadingSpaces_ = 0;
  uint32_t cursor_ = 0;
  size_t parserBytes_ = 0;
  struct alignas(std::max_align_t) AllocationHeader { size_t bytes; };

  void tick() { if (tick_) tick_(context_); }
  bool save() {
    if (depth_ == 128) return d.fail(Error::NestingLimit);
    frames_[depth_++] = {style_.flags, style_.indent, style_.headingLevel, listDepth_}; return true;
  }
  bool restore() {
    if (!depth_) return d.fail(Error::Parser);
    const Frame& frame = frames_[--depth_];
    style_.flags = frame.flags; style_.indent = frame.indent;
    style_.headingLevel = frame.heading; listDepth_ = frame.listDepth; return true;
  }
  void breakBefore(unsigned n) { pendingBreaks_ = std::max(n, pendingBreaks_); }
  bool flush(uint32_t anchor) {
    if (anchor < cursor_ || anchor > d.sourceSize_) return d.fail(Error::Parser);
    // A marker must own at least one byte of skipped source syntax. Otherwise
    // restoring its source anchor would skip the zero-length marker interval.
    if (markerSize_ && anchor == cursor_) return d.fail(Error::Parser);
    const uint32_t breakEnd = markerSize_ ? anchor - 1 : anchor;
    if (d.size_ && pendingBreaks_) {
      unsigned trailing = 0;
      for (size_t p = d.size_; p && d.text_[p - 1] == '\n' && trailing < pendingBreaks_; --p) ++trailing;
      while (trailing++ < pendingBreaks_) {
        if (!d.append("\n", 1, cursor_, breakEnd, false, Run{})) return false;
        cursor_ = breakEnd;
      }
    }
    pendingBreaks_ = 0;
    if (markerSize_) {
      if (!d.append(marker_, markerSize_, breakEnd, anchor, false, style_)) return false;
      markerSize_ = 0; cursor_ = anchor;
      // MD4C can emit synthetic indentation/blank lines before the first raw
      // byte of a code item. Defer them until a real source anchor is known.
      while (leadingBreaks_) {
        if (!d.append("\n", 1, anchor, anchor, false, style_)) return false;
        --leadingBreaks_;
      }
      static const char spaces[] = "                ";
      while (leadingSpaces_) {
        const size_t n = std::min(size_t(16), leadingSpaces_);
        if (!d.append(spaces, n, anchor, anchor, false, style_)) return false;
        leadingSpaces_ -= n;
      }
    }
    return true;
  }
  bool emit(const char* text, size_t bytes, uint32_t begin, uint32_t end, bool linear, Run style) {
    if (!flush(begin) || !d.append(text, bytes, begin, end, linear, style)) return false;
    cursor_ = end; return true;
  }
  bool pointerRange(const char* p, size_t bytes, uint32_t& begin, uint32_t& end) const {
    // Callback text can be an MD4C-generated literal. Compare integer addresses
    // before any offset arithmetic; subtracting unrelated pointers is undefined.
    const uintptr_t address = reinterpret_cast<uintptr_t>(p);
    const uintptr_t base = reinterpret_cast<uintptr_t>(d.source_);
    if (address < base || address - base > d.sourceSize_) return false;
    const size_t offset = address - base;
    if (bytes > d.sourceSize_ - offset) return false;
    begin = uint32_t(offset); end = uint32_t(offset + bytes); return true;
  }
  uint32_t consumeBreak() const {
    size_t p = cursor_;
    // Hard breaks may have two trailing spaces or an escaped newline.
    while (p < d.sourceSize_ && (d.source_[p] == ' ' || d.source_[p] == '\t' || d.source_[p] == '\\')) ++p;
    if (p < d.sourceSize_ && d.source_[p] == '\r') ++p;
    if (p < d.sourceSize_ && d.source_[p] == '\n') ++p;
    return uint32_t(p);
  }
  bool entity(const char* value, size_t bytes, uint32_t begin, uint32_t end) {
    uint32_t cp = 0;
    if (bytes >= 3 && value[0] == '&' && value[bytes - 1] == ';') {
      if (value[1] == '#') {
        size_t p = 2; unsigned base = 10;
        if (p < bytes - 1 && (value[p] == 'x' || value[p] == 'X')) { base = 16; ++p; }
        const size_t start = p;
        for (; p < bytes - 1; ++p) {
          const unsigned char ch = value[p];
          const unsigned digit = ch >= '0' && ch <= '9' ? ch - '0' :
              ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : 99;
          if (digit >= base || cp > (0x10ffffu - digit) / base) { cp = 0xfffd; break; }
          cp = cp * base + digit;
        }
        if (p == start) cp = 0xfffd;
        if (!cp) cp = 0xfffd;
      } else cp = epub::xml_input::xhtmlEntity(value + 1, bytes - 2);
    }
    if (!cp) return emit(value, bytes, begin, end, true, style_);
    char encoded[4]; const size_t n = encode(cp, encoded);
    return emit(encoded, n, begin, end, false, style_);
  }
  static int text(MD_TEXTTYPE type, const char* value, MD_SIZE bytes, void* userdata) {
    auto& b = *static_cast<Builder*>(userdata);
    b.tick();
    if (b.tableDepth_ || b.mathDepth_ || !bytes) return 0;
    uint32_t begin = b.cursor_, end = b.cursor_;
    const bool raw = b.pointerRange(value, bytes, begin, end);
    if (!raw && b.markerSize_ && type == MD_TEXT_CODE) {
      for (size_t i = 0; i < bytes; ++i) {
        if (value[i] == '\n') { ++b.leadingBreaks_; b.leadingSpaces_ = 0; }
        else if (value[i] == ' ') ++b.leadingSpaces_;
        else return b.d.fail(Error::Parser) ? 0 : 1;
        if (b.leadingBreaks_ + b.leadingSpaces_ > Document::MaxTextBytes)
          return b.d.fail(Error::TextLimit) ? 0 : 1;
      }
      return 0;
    }
    if (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR) {
      begin = b.cursor_; end = b.consumeBreak();
      return b.emit(type == MD_TEXT_BR ? "\n" : " ", 1, begin, end, false, b.style_) ? 0 : 1;
    }
    if (type == MD_TEXT_NULLCHAR) {
      if (!raw) end = std::min(uint32_t(b.d.sourceSize_), b.cursor_ + 1);
      return b.emit("\xef\xbf\xbd", 3, begin, end, false, b.style_) ? 0 : 1;
    }
    if (!raw && bytes == 1 && (value[0] == '\n' || value[0] == ' ')) end = b.consumeBreak();
    if (type == MD_TEXT_ENTITY && raw) return b.entity(value, bytes, begin, end) ? 0 : 1;
    return b.emit(value, bytes, begin, end, raw, b.style_) ? 0 : 1;
  }
  static int enterBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
    auto& b = *static_cast<Builder*>(userdata);
    if (b.tableDepth_) { ++b.tableDepth_; return 0; }
    if (!b.save()) return 1;
    switch (type) {
      case MD_BLOCK_QUOTE:
        b.style_.flags |= Quote;
        if (b.style_.indent == 64) return b.d.fail(Error::NestingLimit) ? 0 : 1;
        ++b.style_.indent; b.breakBefore(1); break;
      case MD_BLOCK_UL: case MD_BLOCK_OL:
        if (b.listDepth_ == 64) return b.d.fail(Error::NestingLimit) ? 0 : 1;
        b.lists_[b.listDepth_++] = {type == MD_BLOCK_OL ? static_cast<MD_BLOCK_OL_DETAIL*>(detail)->start : 0,
                                   type == MD_BLOCK_OL};
        b.breakBefore(1); break;
      case MD_BLOCK_LI: {
        b.breakBefore(1);
        b.style_.indent = uint8_t(std::min(64u, unsigned(b.style_.indent) + 1));
        const auto* li = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
        if (li->is_task) {
          const char* marker = li->task_mark == ' ' ? "[ ] " : "[x] ";
          std::memcpy(b.marker_, marker, 4); b.markerSize_ = 4;
        } else if (b.listDepth_ && b.lists_[b.listDepth_ - 1].ordered) {
          auto& list = b.lists_[b.listDepth_ - 1];
          b.markerSize_ = size_t(std::snprintf(b.marker_, sizeof(b.marker_), "%lu. ",
                                             static_cast<unsigned long>(list.number)));
          if (list.number < UINT32_MAX) ++list.number;
        } else { std::memcpy(b.marker_, "\xe2\x80\xa2 ", 4); b.markerSize_ = 4; }
        break;
      }
      case MD_BLOCK_H:
        b.breakBefore(2); b.style_.flags |= Heading | Strong;
        b.style_.headingLevel = uint8_t(static_cast<MD_BLOCK_H_DETAIL*>(detail)->level); break;
      case MD_BLOCK_CODE:
        b.breakBefore(2); b.style_.flags |= Code | Preformatted; break;
      case MD_BLOCK_P: b.breakBefore(2); break;
      case MD_BLOCK_LATEXMATH:
        // Share math acceptance/atom handling with inline display spans.
        if (enterSpan(MD_SPAN_LATEXMATH_DISPLAY, detail, userdata)) return 1;
        if (leaveSpan(MD_SPAN_LATEXMATH_DISPLAY, detail, userdata)) return 1;
        break;
      case MD_BLOCK_HR: {
        const auto& range = *static_cast<MD_SOURCE_RANGE_DETAIL*>(detail);
        b.breakBefore(1); Run style = b.style_; style.kind = Kind::Rule;
        if (!b.emit(atom, 3, range.source_begin, range.source_end, false, style)) return 1;
        b.breakBefore(1); break;
      }
      case MD_BLOCK_TABLE: {
        const auto& table = *static_cast<MD_BLOCK_TABLE_DETAIL*>(detail);
        if (table.source_begin > table.source_end || table.source_end > b.d.sourceSize_) return 1;
        b.breakBefore(2); Run style = b.style_; style.flags |= Code | Preformatted;
        if (!b.emit(b.d.source_ + table.source_begin, table.source_end - table.source_begin,
                    table.source_begin, table.source_end, true, style)) return 1;
        b.tableDepth_ = 1; break;
      }
      default: break;
    }
    return 0;
  }
  static int leaveBlock(MD_BLOCKTYPE type, void*, void* userdata) {
    auto& b = *static_cast<Builder*>(userdata);
    if (b.tableDepth_ && --b.tableDepth_) return 0;
    switch (type) {
      case MD_BLOCK_P: case MD_BLOCK_H: case MD_BLOCK_CODE: case MD_BLOCK_TABLE: b.breakBefore(2); break;
      case MD_BLOCK_LI:
        b.markerSize_ = b.leadingBreaks_ = b.leadingSpaces_ = 0; b.breakBefore(1); break;
      case MD_BLOCK_UL: case MD_BLOCK_OL: case MD_BLOCK_QUOTE: b.breakBefore(1); break;
      default: break;
    }
    return b.restore() ? 0 : 1;
  }
  static int enterSpan(MD_SPANTYPE type, void* detail, void* userdata) {
    auto& b = *static_cast<Builder*>(userdata);
    if (b.tableDepth_) return 0;
    if (b.mathDepth_) { ++b.mathDepth_; return 0; }
    if (!b.save()) return 1;
    switch (type) {
      case MD_SPAN_EM: b.style_.flags |= Emphasis; break;
      case MD_SPAN_STRONG: b.style_.flags |= Strong; break;
      case MD_SPAN_CODE: b.style_.flags |= Code; break;
      case MD_SPAN_DEL: b.style_.flags |= Strike; break;
      case MD_SPAN_A: b.style_.flags |= Link; break;
      case MD_SPAN_IMG: b.style_.flags |= Image; break;
      case MD_SPAN_LATEXMATH: case MD_SPAN_LATEXMATH_DISPLAY: {
        const auto& math = *static_cast<MD_SPAN_LATEXMATH_DETAIL*>(detail);
        if (math.source_begin > math.formula_begin || math.formula_begin > math.formula_end ||
            math.formula_end > math.source_end || math.source_end > b.d.sourceSize_)
          return b.d.fail(Error::Parser) ? 0 : 1;
        const bool display = type == MD_SPAN_LATEXMATH_DISPLAY;
        if (display) {
          b.breakBefore(1);
          if (b.markerSize_) { if (!b.flush(math.source_begin)) return 1; b.breakBefore(1); }
        }
        Run style = b.style_;
        if (b.accept_ && !b.accept_(b.d.source_ + math.formula_begin,
                                   math.formula_end - math.formula_begin, display, b.mathContext_)) {
          style.flags |= Code | Preformatted;
          if (!b.emit(b.d.source_ + math.source_begin, math.source_end - math.source_begin,
                      math.source_begin, math.source_end, true, style)) return 1;
        } else {
          style.kind = display ? Kind::MathBlock : Kind::MathInline;
          style.formulaBegin = math.formula_begin; style.formulaEnd = math.formula_end;
          if (!b.emit(atom, 3, math.source_begin, math.source_end, false, style)) return 1;
        }
        if (display) b.breakBefore(1);
        b.mathDepth_ = 1; break;
      }
      default: break;
    }
    return 0;
  }
  static int leaveSpan(MD_SPANTYPE, void*, void* userdata) {
    auto& b = *static_cast<Builder*>(userdata);
    if (b.tableDepth_) return 0;
    if (b.mathDepth_ && --b.mathDepth_) return 0;
    return b.restore() ? 0 : 1;
  }
  static void* allocateParser(size_t bytes, void* context) { return resizeParser(nullptr, bytes, context); }
  static void* resizeParser(void* memory, size_t bytes, void* context) {
    auto& b = *static_cast<Builder*>(context);
    const size_t old = memory ? (static_cast<AllocationHeader*>(memory) - 1)->bytes : 0;
    if (bytes > Document::ParserBudget - sizeof(AllocationHeader) ||
        bytes + sizeof(AllocationHeader) > Document::ParserBudget - (b.parserBytes_ - old)) {
      b.d.fail(Error::ParserBudget); return nullptr;
    }
    const size_t total = bytes + sizeof(AllocationHeader);
    auto* allocation = static_cast<AllocationHeader*>(b.d.resize(
        memory ? static_cast<AllocationHeader*>(memory) - 1 : nullptr, total));
    if (!allocation) { b.d.fail(Error::Allocation); return nullptr; }
    allocation->bytes = total; b.parserBytes_ = b.parserBytes_ - old + total;
    return allocation + 1;
  }
  static void releaseParser(void* memory, void* context) {
    if (!memory) return;
    auto& b = *static_cast<Builder*>(context);
    auto* allocation = static_cast<AllocationHeader*>(memory) - 1;
    b.parserBytes_ -= allocation->bytes; b.d.release(allocation);
  }
  static void parserTick(void* context) { static_cast<Builder*>(context)->tick(); }
};

bool Document::parse(const char* original, size_t bytes, Tick tick, void* context,
                     MathAccept accept, void* mathContext) {
  clear(); source_ = original; sourceSize_ = bytes;
  if ((!original && bytes) || !allocator_.reallocate || !allocator_.release) return fail(Error::InvalidInput);
  if (bytes > MaxSourceBytes) return fail(Error::SourceLimit);
  if (!bytes) return true;
  Builder builder(*this, tick, context, accept, mathContext);
  if (!builder.validUtf8() || !builder.parse()) { discard(); return false; }
  return true;
}
}  // namespace markdown
