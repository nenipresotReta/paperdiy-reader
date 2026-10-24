#pragma once
// Optional EPUB semantics over the EXISTING extracted UTF-8 text. Offsets are
// never XHTML byte offsets; the importer still owns all text normalization.
// No generated bullets, cache writes or layout policy.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include "epub_objects.h"

namespace epub { namespace structure {
constexpr uint32_t NONE = UINT32_MAX;
enum class Kind : uint8_t {
  Text, Title, Paragraph, Heading, Quote, ListItem, Preformatted, Container,
  OrderedList, UnorderedList, DefinitionTerm, DefinitionDetail, TableRow, Break, Ruby, RubyText, RubyFallback, Math, Table, TableCell
};
enum Flag : uint16_t { Strong = 1, Emphasis = 2, Code = 4, Preserve = 8,
                       Superscript = 16, Subscript = 32, VerticalRL = 64, Combine = 128, TableHeader = 256, UnsupportedLayout = 512 };
enum class Status : uint8_t { Idle, Collecting, Complete, Budget, Allocation, ImportFailed, Invalid };

// Records are in XML preorder. Nested block ranges intentionally overlap;
// Text/Break ranges are disjoint. Separating newlines need not belong to a Text
// record. parent is an index in records, resource is an index in resources.
struct Record {
  uint32_t begin, end, parent, resource;
  uint16_t flags;
  Kind kind;
  uint8_t headingLevel, depth, language;
  uint16_t reserved;
};
struct Resource {
  uint32_t begin, end, pathOffset;
  uint16_t spineIndex; // original OPF itemref order, even for deferred notes
  bool auxiliary;
  uint8_t reserved;
};
struct Limits {
  size_t records = 8192;
  size_t resources = 2048;
  size_t pathBytes = 128 * 1024;
  size_t totalBytes = 512 * 1024;
  size_t formulas=epub_objects::MAX_MATH,mathBytes=epub_objects::MAX_TEX_BYTES;
};
struct Allocator {
  void* (*allocate)(size_t, void*) = nullptr;
  void (*release)(void*, void*) = nullptr;
  void* context = nullptr;
};

// Any sidecar failure discards ALL observable records. Importing flat text
// continues, and clients must check ready() before choosing semantic layout.
// Allocation is a single checked block, never std::vector/string or throwing
// new. Custom allocate/release must be paired and satisfy malloc alignment.
class Collector {
 public:
  // Extraction checkpoints are not PDS render metadata. Keep the complete
  // collector tree and budget accounting so later chapters behave exactly as
  // an uninterrupted import. Only call between closed chapters (or at Complete).
  static constexpr size_t SNAPSHOT_HEADER_BYTES = 96;
  static constexpr size_t SNAPSHOT_MAX_BYTES = 512 * 1024 + SNAPSHOT_HEADER_BYTES + 32 * 36;
  explicit Collector(const Limits& limits = Limits(), const Allocator& allocator = Allocator())
      : limits_(limits), allocator_(allocator) {}
  ~Collector() { release(); }
  Collector(const Collector&) = delete;
  Collector& operator=(const Collector&) = delete;
  bool ready() const { return state_ == Status::Complete; }
  Status status() const { return state_; }
  size_t recordCount() const { return ready() ? recordCount_ : 0; }
  size_t resourceCount() const { return ready() ? resourceCount_ : 0; }
  size_t mathCount()const{return ready()?mathCount_:0;}
  const epub_objects::Math& math(size_t i)const{return maths_[i];}
  const char* mathText(size_t i)const{return mathText_+maths_[i].texOffset;}
  const Record* records() const { return ready() ? records_ : nullptr; }
  const Resource* resources() const { return ready() ? resources_ : nullptr; }
  const char* path(uint32_t resource) const {
    return ready() && resource < resourceCount_ ? paths_ + resources_[resource].pathOffset : "";
  }
  const char* language(uint8_t id) const { return id < languageCount_ ? languages_[id] : ""; }
  size_t allocatedBytes() const { return storage_ ? allocatedBytes_ : 0; }

  size_t snapshotBytes() const {
    if (languageCount_ == 0 || languageCount_ > 32 ||
        recordCount_ > UINT32_MAX || resourceCount_ > UINT16_MAX ||
        pathUsed_ > UINT32_MAX || mathCount_ > epub_objects::MAX_MATH ||
        mathUsed_ > epub_objects::MAX_TEX_BYTES || allocatedBytes_ > 512 * 1024) return 0;
    size_t bytes = SNAPSHOT_HEADER_BYTES;
    if (!addBudget(bytes, languageCount_, 36) || !addBudget(bytes, recordCount_, 24) ||
        !addBudget(bytes, resourceCount_, 16) || !addBudget(bytes, mathCount_, 16) ||
        !addBudget(bytes, pathUsed_, 1) || !addBudget(bytes, mathUsed_, 1) ||
        bytes > SNAPSHOT_MAX_BYTES) return 0;
    return bytes;
  }
  bool encodeSnapshot(uint8_t* out, size_t capacity, const char* text, size_t bytes) const {
    const size_t size = snapshotBytes();
    // The importer already validated the live UTF-8. Avoid rescanning the whole
    // growing book at every chapter; restore performs the full prefix check.
    if (!out || !size || capacity < size || !snapshotValid(text, bytes, false)) return false;
    std::memset(out, 0, SNAPSHOT_HEADER_BYTES);
    std::memcpy(out, "ECS1", 4); write16(out + 4, 1); write16(out + 6, SNAPSHOT_HEADER_BYTES);
    write32(out + 8, uint32_t(size)); write32(out + 12, uint32_t(bytes));
    out[16] = uint8_t(state_); out[17] = languageCount_;
    write32(out + 20, uint32_t(recordCount_)); write32(out + 24, uint32_t(resourceCount_));
    write32(out + 28, uint32_t(pathUsed_)); write32(out + 32, uint32_t(mathCount_));
    write32(out + 36, uint32_t(mathUsed_)); write32(out + 40, lastText_);
    const size_t limits[] = {limits_.records, limits_.resources, limits_.pathBytes,
                            limits_.totalBytes, limits_.formulas, limits_.mathBytes};
    for (unsigned i = 0; i < 6; ++i) write64(out + 48 + i * 8, uint64_t(limits[i]));
    size_t at = SNAPSHOT_HEADER_BYTES;
    for (uint8_t i = 0; i < languageCount_; ++i) {
      std::memset(out + at, 0, 36);
      std::memcpy(out + at, languages_[i], std::strlen(languages_[i])); at += 36;
    }
    for (size_t i = 0; i < recordCount_; ++i, at += 24) {
      const auto& r = records_[i]; write32(out + at, r.begin); write32(out + at + 4, r.end);
      write32(out + at + 8, r.parent); write32(out + at + 12, r.resource);
      write16(out + at + 16, r.flags); out[at + 18] = uint8_t(r.kind);
      out[at + 19] = r.headingLevel; out[at + 20] = r.depth; out[at + 21] = r.language;
      write16(out + at + 22, 0);
    }
    for (size_t i = 0; i < resourceCount_; ++i, at += 16) {
      const auto& r = resources_[i]; write32(out + at, r.begin); write32(out + at + 4, r.end);
      write32(out + at + 8, r.pathOffset); write16(out + at + 12, r.spineIndex);
      out[at + 14] = r.auxiliary ? 1 : 0; out[at + 15] = 0;
    }
    for (size_t i = 0; i < mathCount_; ++i, at += 16) {
      const auto& m = maths_[i]; write32(out + at, m.begin); write32(out + at + 4, m.end);
      write32(out + at + 8, m.texOffset); write16(out + at + 12, m.texBytes);
      write16(out + at + 14, m.flags);
    }
    if (pathUsed_) std::memcpy(out + at, paths_, pathUsed_); at += pathUsed_;
    if (mathUsed_) std::memcpy(out + at, mathText_, mathUsed_);
    return true;
  }
  bool restoreSnapshot(const uint8_t* data, size_t size, const char* text, size_t bytes) {
    // Publish no partial state, including when a newer slot is corrupt. The
    // caller can try its older committed slot or start a fresh import.
    release(); recordCount_ = resourceCount_ = pathUsed_ = mathCount_ = mathUsed_ = 0;
    lastText_ = NONE; languageCount_ = 1; languages_[0][0] = 0; state_ = Status::Invalid;
    if (!data || size < SNAPSHOT_HEADER_BYTES || size > SNAPSHOT_MAX_BYTES ||
        std::memcmp(data, "ECS1", 4) || read16(data + 4) != 1 ||
        read16(data + 6) != SNAPSHOT_HEADER_BYTES || read32(data + 8) != size ||
        read32(data + 12) != bytes || data[16] > uint8_t(Status::Invalid) ||
        !data[17] || data[17] > 32 || read16(data + 18) || read32(data + 44)) return false;
    const size_t limits[] = {limits_.records, limits_.resources, limits_.pathBytes,
                            limits_.totalBytes, limits_.formulas, limits_.mathBytes};
    for (unsigned i = 0; i < 6; ++i) if (read64(data + 48 + i * 8) != uint64_t(limits[i])) return false;
    const size_t records = read32(data + 20), resources = read32(data + 24), paths = read32(data + 28);
    const size_t maths = read32(data + 32), tex = read32(data + 36);
    size_t expected = SNAPSHOT_HEADER_BYTES;
    if (records > limits_.records || resources > limits_.resources || resources > UINT16_MAX ||
        paths > limits_.pathBytes || maths > limits_.formulas || maths > epub_objects::MAX_MATH ||
        tex > limits_.mathBytes || tex > epub_objects::MAX_TEX_BYTES ||
        !addBudget(expected, data[17], 36) || !addBudget(expected, records, 24) ||
        !addBudget(expected, resources, 16) || !addBudget(expected, maths, 16) ||
        !addBudget(expected, paths, 1) || !addBudget(expected, tex, 1) || expected != size) return false;
    if (!snapshotUtf8(text, bytes)) return false;
    const Status state = Status(data[16]);
    const bool active = state == Status::Collecting || state == Status::Complete;
    if (!active && (records || resources || paths || maths || tex || read32(data + 40) != NONE)) return false;
    if (active) {
      begin();
      if (state_ != Status::Collecting) return false;
      if (allocatedBytes_ > 512 * 1024) { discard(Status::Budget); return false; }
    }
    state_ = state; languageCount_ = data[17]; lastText_ = read32(data + 40);
    size_t at = SNAPSHOT_HEADER_BYTES;
    for (uint8_t i = 0; i < languageCount_; ++i, at += 36) {
      const void* zero = std::memchr(data + at, 0, 36);
      if (!zero) return snapshotFailure();
      const size_t length = static_cast<const uint8_t*>(zero) - (data + at);
      for (size_t j = length; j < 36; ++j) if (data[at + j]) return snapshotFailure();
      std::memcpy(languages_[i], data + at, 36);
    }
    for (size_t i = 0; i < records; ++i, at += 24) {
      Record r = {read32(data + at), read32(data + at + 4), read32(data + at + 8),
                  read32(data + at + 12), read16(data + at + 16), Kind(data[at + 18]),
                  data[at + 19], data[at + 20], data[at + 21], read16(data + at + 22)};
      records_[recordCount_++] = r;
    }
    for (size_t i = 0; i < resources; ++i, at += 16) {
      if (data[at + 14] > 1) return snapshotFailure();
      Resource r = {read32(data + at), read32(data + at + 4), read32(data + at + 8),
                    read16(data + at + 12), data[at + 14] != 0, data[at + 15]};
      resources_[resourceCount_++] = r;
    }
    for (size_t i = 0; i < maths; ++i, at += 16) {
      epub_objects::Math m = {read32(data + at), read32(data + at + 4), read32(data + at + 8),
                             read16(data + at + 12), read16(data + at + 14)};
      maths_[mathCount_++] = m;
    }
    if (paths) std::memcpy(paths_, data + at, paths); pathUsed_ = paths; at += paths;
    if (tex) std::memcpy(mathText_, data + at, tex); mathUsed_ = tex;
    if (!snapshotValid(text, bytes, false)) return snapshotFailure();
    return true;
  }

  void begin() {
    release();
    recordCount_ = resourceCount_ = pathUsed_ = mathCount_ = mathUsed_ = 0;
    lastText_ = NONE; languageCount_ = 1; languages_[0][0] = 0;
    state_ = Status::Collecting;
    if ((allocator_.allocate == nullptr) != (allocator_.release == nullptr)) {
      discard(Status::Invalid); return;
    }
    if (limits_.records > UINT32_MAX || limits_.resources > UINT16_MAX ||
        limits_.pathBytes > UINT32_MAX || limits_.formulas>epub_objects::MAX_MATH || limits_.mathBytes>epub_objects::MAX_TEX_BYTES) { discard(Status::Budget); return; }
    size_t bytes = 0;
    if (!addBudget(bytes, limits_.records, sizeof(Record)) ||
        !addBudget(bytes, limits_.resources, sizeof(Resource)) ||
        !addBudget(bytes, limits_.formulas, sizeof(epub_objects::Math)) ||
        !addBudget(bytes, limits_.mathBytes, 1) || !addBudget(bytes, limits_.pathBytes, 1) || bytes > limits_.totalBytes || !bytes) {
      discard(Status::Budget); return;
    }
    storage_ = allocator_.allocate ? allocator_.allocate(bytes, allocator_.context) : std::malloc(bytes);
    if (!storage_) { discard(Status::Allocation); return; }
    allocatedBytes_ = bytes;
    records_ = static_cast<Record*>(storage_);
    resources_ = reinterpret_cast<Resource*>(records_ + limits_.records);
    maths_ = reinterpret_cast<epub_objects::Math*>(resources_ + limits_.resources);
    paths_ = reinterpret_cast<char*>(maths_ + limits_.formulas);mathText_=paths_+limits_.pathBytes;
  }
  void discard(Status reason = Status::ImportFailed) {
    // Preserve the first failure for diagnostics; no partial sidecar is usable.
    if (state_ == Status::Collecting || state_ == Status::Complete || state_ == Status::Idle) state_ = reason;
    recordCount_ = resourceCount_ = pathUsed_ = mathCount_ = mathUsed_ = 0;
    lastText_ = NONE;
    release(); // Free the optional budget promptly so flat import can continue.
  }
  bool addMath(size_t begin,size_t end,const char* tex,size_t bytes,bool display){
    if(state_!=Status::Collecting||begin>=end||end>UINT32_MAX||end-begin>epub_objects::MAX_MATH_SOURCE||!tex||!bytes||bytes>4096||mathCount_>=limits_.formulas||bytes>limits_.mathBytes-mathUsed_)return false;
    maths_[mathCount_++]={uint32_t(begin),uint32_t(end),uint32_t(mathUsed_),uint16_t(bytes),uint16_t(display?epub_objects::MATH_DISPLAY:0)};
    std::memcpy(mathText_+mathUsed_,tex,bytes);mathUsed_+=bytes;return true;
  }
  uint8_t internLanguage(const char* tag) {
    if (state_ != Status::Collecting || !tag) return 0;
    while (space(*tag)) ++tag;
    size_t length = std::strlen(tag);
    while (length && space(tag[length - 1])) --length;
    // Unknown/invalid or long tags stay explicitly unknown, never truncated.
    if (!length || length >= sizeof(languages_[0])) return 0;
    char normalized[36];
    for (size_t i = 0; i < length; ++i) {
      char c = tag[i]; if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return 0;
      normalized[i] = c;
    }
    normalized[length] = 0;
    for (uint8_t i = 1; i < languageCount_; ++i)
      if (!std::strcmp(normalized, languages_[i])) return i;
    if (languageCount_ == 32) { discard(Status::Budget); return 0; }
    std::memcpy(languages_[languageCount_], normalized, length + 1);
    return languageCount_++;
  }
  uint32_t openResource(const char* path, uint16_t spineIndex, bool auxiliary, size_t offset) {
    if (state_ != Status::Collecting) return NONE;
    const size_t length = path ? std::strlen(path) + 1 : 0;
    if (!length || offset > UINT32_MAX || resourceCount_ >= limits_.resources ||
        length > limits_.pathBytes - pathUsed_) { discard(Status::Budget); return NONE; }
    Resource& resource = resources_[resourceCount_];
    resource.begin = resource.end = uint32_t(offset);
    resource.pathOffset = uint32_t(pathUsed_); resource.spineIndex = spineIndex;
    resource.auxiliary = auxiliary; resource.reserved = 0;
    std::memcpy(paths_ + pathUsed_, path, length); pathUsed_ += length;
    return uint32_t(resourceCount_++);
  }
  void closeResource(uint32_t id, size_t offset) {
    if (state_ != Status::Collecting || id == NONE) return;
    if (id >= resourceCount_ || offset > UINT32_MAX || offset < resources_[id].begin) {
      discard(Status::Invalid); return;
    }
    resources_[id].end = uint32_t(offset);
  }
  uint32_t open(Kind kind, size_t begin, uint32_t parent, uint32_t resource,
                uint16_t flags, uint8_t depth, uint8_t language, uint8_t headingLevel = 0) {
    if (state_ != Status::Collecting) return NONE;
    if (begin > UINT32_MAX || recordCount_ >= limits_.records) { discard(Status::Budget); return NONE; }
    if ((parent != NONE && parent >= recordCount_) ||
        (resource != NONE && resource >= resourceCount_) || language >= languageCount_) {
      discard(Status::Invalid); return NONE;
    }
    Record& r = records_[recordCount_];
    r.begin = r.end = uint32_t(begin); r.parent = parent; r.resource = resource;
    r.kind = kind; r.flags = flags; r.depth = depth; r.language = language;
    r.headingLevel = headingLevel; r.reserved = 0;
    return uint32_t(recordCount_++);
  }
  void close(uint32_t id, size_t end) {
    if (state_ != Status::Collecting || id == NONE) return;
    if (id >= recordCount_ || end > UINT32_MAX || end < records_[id].begin) {
      discard(Status::Invalid); return;
    }
    records_[id].end = uint32_t(end);
  }
  void text(size_t begin, size_t end, uint32_t parent, uint32_t resource,
            uint16_t flags, uint8_t depth, uint8_t language, Kind kind = Kind::Text) {
    if (state_ != Status::Collecting || begin == end) return;
    // Merge adjacent equal-style text nodes without spending a record per span.
    if (kind == Kind::Text && lastText_ != NONE && lastText_ + 1 == recordCount_) {
      Record& previous = records_[lastText_];
      if (previous.kind == kind && previous.end == begin && previous.parent == parent &&
          previous.resource == resource && previous.flags == flags && previous.language == language) {
        close(lastText_, end); return;
      }
    }
    const uint32_t id = open(kind, begin, parent, resource, flags, depth, language);
    close(id, end); if (state_ == Status::Collecting) lastText_ = id;
  }
  void trimTail(size_t end) {
    // newline() may rewind the last text node's spaces before appending '\n'.
    // This never scans old paragraphs, so dense books remain linear in size.
    if (state_ != Status::Collecting) return;
    // A non-spacing wrapper (ul/ol/body) may open after preserved spaces and
    // before a child block rewinds them. Rewind those empty/open boundaries too.
    size_t boundary = recordCount_;
    while (boundary && records_[boundary - 1].begin > end) {
      Record& r = records_[--boundary];
      r.begin = uint32_t(end);
      if (r.end > end) r.end = uint32_t(end);
    }
    if (lastText_ == NONE) return;
    size_t cursor = size_t(lastText_) + 1;
    while (cursor) {
      Record& r = records_[--cursor];
      if (r.kind != Kind::Text && r.kind != Kind::Break) continue;
      if (r.end > end) {
        r.end = uint32_t(end);
        if (r.begin > end) r.begin = uint32_t(end);
        // A closed non-spacing ancestor can still include these spaces. Its
        // end must move before later UTF-8 text reuses the trimmed byte slots.
        uint32_t parent = r.parent;
        while (parent != NONE) {
          Record& ancestor = records_[parent];
          if (ancestor.end <= end) break; // unclosed or entirely before rewind
          ancestor.end = uint32_t(end);
          if (ancestor.begin > end) ancestor.begin = uint32_t(end);
          parent = ancestor.parent;
        }
      }
      if (r.begin < r.end) { lastText_ = uint32_t(cursor); return; }
    }
    lastText_ = NONE;
  }
  void finish(const char* text, size_t bytes) {
    if (state_ != Status::Collecting) return;
    if (!text || bytes > UINT32_MAX) { discard(Status::Invalid); return; }
    for (size_t i = 0; i < resourceCount_; ++i) {
      const Resource& r = resources_[i];
      if (r.begin > r.end || r.end > bytes || (i && r.begin < resources_[i - 1].end)) {
        discard(Status::Invalid); return;
      }
    }
    for (size_t i = 0; i < recordCount_; ++i) {
      Record& r = records_[i];
      // Parent boundary cleanup can trim a child trailing space or an empty
      // nested block. Clip consistently in preorder before checking boundaries.
      uint32_t begin = 0, end = uint32_t(bytes);
      if (r.resource != NONE) { begin = resources_[r.resource].begin; end = resources_[r.resource].end; }
      if (r.parent != NONE) { begin = records_[r.parent].begin; end = records_[r.parent].end; }
      if (r.begin < begin) r.begin = begin;
      if (r.begin > end) r.begin = end;
      if (r.end < r.begin) r.end = r.begin;
      if (r.end > end) r.end = end;
      if ((r.begin < bytes && continuation(text[r.begin])) ||
          (r.end < bytes && continuation(text[r.end]))) { discard(Status::Invalid); return; }
    }
    state_ = Status::Complete;
  }
 private:
  static uint16_t read16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(uint16_t(p[1]) << 8); }
  static uint32_t read32(const uint8_t* p) { return uint32_t(read16(p)) | (uint32_t(read16(p + 2)) << 16); }
  static uint64_t read64(const uint8_t* p) { return uint64_t(read32(p)) | (uint64_t(read32(p + 4)) << 32); }
  static void write16(uint8_t* p, uint16_t value) { p[0] = uint8_t(value); p[1] = uint8_t(value >> 8); }
  static void write32(uint8_t* p, uint32_t value) { for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (i * 8)); }
  static void write64(uint8_t* p, uint64_t value) { for (unsigned i = 0; i < 8; ++i) p[i] = uint8_t(value >> (i * 8)); }
  bool snapshotFailure() {
    // discard() preserves prior failure reasons, but malformed restore must be
    // explicitly Invalid rather than inherit a serialized Allocation/Budget.
    state_ = Status::Collecting; discard(Status::Invalid); return false;
  }
  static bool snapshotUtf8(const char* text, size_t bytes) {
    if ((!text && bytes) || bytes > 4u * 1024 * 1024) return false;
    for (size_t at = 0; at < bytes;) {
      uint8_t c = uint8_t(text[at++]); if (c < 128) { if (!c) return false; continue; }
      unsigned count; uint32_t cp, minimum;
      if (c >= 0xc2 && c <= 0xdf) { count = 1; cp = c & 31; minimum = 128; }
      else if (c >= 0xe0 && c <= 0xef) { count = 2; cp = c & 15; minimum = 2048; }
      else if (c >= 0xf0 && c <= 0xf4) { count = 3; cp = c & 7; minimum = 65536; }
      else return false;
      if (count > bytes - at) return false;
      while (count--) { c = uint8_t(text[at++]); if ((c & 0xc0) != 0x80) return false; cp = (cp << 6) | (c & 63); }
      if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    }
    return true;
  }
  bool snapshotValid(const char* text, size_t bytes, bool verifyText) const {
    if ((!text && bytes) || bytes > 4u * 1024 * 1024 ||
        uint8_t(state_) > uint8_t(Status::Invalid) || !snapshotBytes() ||
        (verifyText && !snapshotUtf8(text, bytes))) return false;
    auto boundary = [&](uint32_t p) { return p <= bytes && (p == bytes || !continuation(text[p])); };
    for (uint8_t i = 0; i < languageCount_; ++i) {
      const char* zero = static_cast<const char*>(std::memchr(languages_[i], 0, 36));
      if (!zero || (i == 0 ? zero != languages_[i] : zero == languages_[i])) return false;
      for (const char* p = languages_[i]; p != zero; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')) return false;
      for (uint8_t j = 0; j < i; ++j) if (!std::strcmp(languages_[i], languages_[j])) return false;
    }
    const bool active = state_ == Status::Collecting || state_ == Status::Complete;
    if (!active) return !recordCount_ && !resourceCount_ && !pathUsed_ && !mathCount_ && !mathUsed_ && lastText_ == NONE;
    if (!storage_ || (state_ == Status::Collecting && bytes &&
        (bytes < 2 || text[bytes - 1] != '\n' || text[bytes - 2] != '\n'))) return false;
    size_t path = 0;
    for (size_t i = 0; i < resourceCount_; ++i) {
      const auto& r = resources_[i];
      if (r.begin > r.end || !boundary(r.begin) || !boundary(r.end) || r.reserved ||
          (i && r.begin < resources_[i - 1].end) || r.pathOffset != path || path >= pathUsed_) return false;
      const char* zero = static_cast<const char*>(std::memchr(paths_ + path, 0, pathUsed_ - path));
      if (!zero || zero == paths_ + path) return false;
      path = size_t(zero - paths_) + 1;
    }
    if (path != pathUsed_) return false;
    for (size_t i = 0; i < recordCount_; ++i) {
      const auto& r = records_[i];
      if (r.begin > r.end || !boundary(r.begin) || !boundary(r.end) || r.reserved ||
          uint8_t(r.kind) > uint8_t(Kind::TableCell) || r.flags & ~uint16_t(1023) ||
          r.headingLevel > 6 || r.depth > 32 || r.language >= languageCount_ ||
          (r.parent != NONE && r.parent >= i) || (r.resource != NONE && r.resource >= resourceCount_)) return false;
      if (r.parent != NONE && records_[r.parent].resource != r.resource) return false;
      if (r.resource != NONE && (r.begin < resources_[r.resource].begin ||
          r.end > resources_[r.resource].end)) return false;
      // Raw pre-finish ranges intentionally need not fit their closed parent:
      // finish() clips nested trailing spaces/empty blocks in XML preorder.
      if (state_ == Status::Complete) {
        uint32_t begin = 0, end = uint32_t(bytes);
        if (r.resource != NONE) { begin = resources_[r.resource].begin; end = resources_[r.resource].end; }
        if (r.parent != NONE) { begin = records_[r.parent].begin; end = records_[r.parent].end; }
        if (r.begin < begin || r.end > end) return false;
      }
    }
    if (lastText_ != NONE && (lastText_ >= recordCount_ ||
        (records_[lastText_].kind != Kind::Text && records_[lastText_].kind != Kind::Break))) return false;
    size_t tex = 0;
    for (size_t i = 0; i < mathCount_; ++i) {
      const auto& m = maths_[i];
      if (m.begin >= m.end || !boundary(m.begin) || !boundary(m.end) ||
          m.end - m.begin > epub_objects::MAX_MATH_SOURCE || m.texOffset != tex ||
          !m.texBytes || m.texBytes > 4096 || m.texBytes > mathUsed_ - tex ||
          m.flags & ~uint16_t(epub_objects::MATH_DISPLAY) ||
          !snapshotUtf8(mathText_ + tex, m.texBytes)) return false;
      tex += m.texBytes;
    }
    return tex == mathUsed_;
  }
  static bool continuation(char c) { return (static_cast<unsigned char>(c) & 0xc0) == 0x80; }
  static bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
  static bool addBudget(size_t& total, size_t count, size_t width) {
    if (count > (std::numeric_limits<size_t>::max() - total) / width) return false;
    total += count * width; return true;
  }
  void release() {
    if (storage_) {
      if (allocator_.release) allocator_.release(storage_, allocator_.context);
      else std::free(storage_);
    }
    storage_ = nullptr; records_ = nullptr; resources_ = nullptr; paths_ = nullptr; maths_=nullptr;mathText_=nullptr;allocatedBytes_ = 0;
  }
  Limits limits_; Allocator allocator_;
  Status state_ = Status::Idle;
  void* storage_ = nullptr;
  Record* records_ = nullptr; Resource* resources_ = nullptr; char* paths_ = nullptr;
  size_t recordCount_ = 0, resourceCount_ = 0, pathUsed_ = 0, allocatedBytes_ = 0;
  epub_objects::Math* maths_=nullptr;char* mathText_=nullptr;size_t mathCount_=0,mathUsed_=0;
  uint32_t lastText_ = NONE;
  uint8_t languageCount_ = 1;
  char languages_[32][36] = {{0}};
};
}} // namespace epub::structure
