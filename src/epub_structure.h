#pragma once
// Optional EPUB semantics over the EXISTING extracted UTF-8 text. Offsets are
// never XHTML byte offsets; the importer still owns all text normalization.
// No CSS, generated bullets, font selection, cache writes or layout policy.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace epub { namespace structure {
constexpr uint32_t NONE = UINT32_MAX;
enum class Kind : uint8_t {
  Text, Title, Paragraph, Heading, Quote, ListItem, Preformatted, Container,
  OrderedList, UnorderedList, DefinitionTerm, DefinitionDetail, TableRow, Break
};
enum Flag : uint16_t { Strong = 1, Emphasis = 2, Code = 4, Preserve = 8,
                       Superscript = 16, Subscript = 32 };
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
  explicit Collector(const Limits& limits = Limits(), const Allocator& allocator = Allocator())
      : limits_(limits), allocator_(allocator) {}
  ~Collector() { release(); }
  Collector(const Collector&) = delete;
  Collector& operator=(const Collector&) = delete;
  bool ready() const { return state_ == Status::Complete; }
  Status status() const { return state_; }
  size_t recordCount() const { return ready() ? recordCount_ : 0; }
  size_t resourceCount() const { return ready() ? resourceCount_ : 0; }
  const Record* records() const { return ready() ? records_ : nullptr; }
  const Resource* resources() const { return ready() ? resources_ : nullptr; }
  const char* path(uint32_t resource) const {
    return ready() && resource < resourceCount_ ? paths_ + resources_[resource].pathOffset : "";
  }
  const char* language(uint8_t id) const { return id < languageCount_ ? languages_[id] : ""; }
  size_t allocatedBytes() const { return storage_ ? allocatedBytes_ : 0; }

  void begin() {
    release();
    recordCount_ = resourceCount_ = pathUsed_ = 0;
    lastText_ = NONE; languageCount_ = 1; languages_[0][0] = 0;
    state_ = Status::Collecting;
    if ((allocator_.allocate == nullptr) != (allocator_.release == nullptr)) {
      discard(Status::Invalid); return;
    }
    if (limits_.records > UINT32_MAX || limits_.resources > UINT16_MAX ||
        limits_.pathBytes > UINT32_MAX) { discard(Status::Budget); return; }
    size_t bytes = 0;
    if (!addBudget(bytes, limits_.records, sizeof(Record)) ||
        !addBudget(bytes, limits_.resources, sizeof(Resource)) ||
        !addBudget(bytes, limits_.pathBytes, 1) || bytes > limits_.totalBytes || !bytes) {
      discard(Status::Budget); return;
    }
    storage_ = allocator_.allocate ? allocator_.allocate(bytes, allocator_.context) : std::malloc(bytes);
    if (!storage_) { discard(Status::Allocation); return; }
    allocatedBytes_ = bytes;
    records_ = static_cast<Record*>(storage_);
    resources_ = reinterpret_cast<Resource*>(records_ + limits_.records);
    paths_ = reinterpret_cast<char*>(resources_ + limits_.resources);
  }
  void discard(Status reason = Status::ImportFailed) {
    // Preserve the first failure for diagnostics; no partial sidecar is usable.
    if (state_ == Status::Collecting || state_ == Status::Complete || state_ == Status::Idle) state_ = reason;
    recordCount_ = resourceCount_ = pathUsed_ = 0;
    lastText_ = NONE;
    release(); // Free the optional budget promptly so flat import can continue.
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
    storage_ = nullptr; records_ = nullptr; resources_ = nullptr; paths_ = nullptr; allocatedBytes_ = 0;
  }
  Limits limits_; Allocator allocator_;
  Status state_ = Status::Idle;
  void* storage_ = nullptr;
  Record* records_ = nullptr; Resource* resources_ = nullptr; char* paths_ = nullptr;
  size_t recordCount_ = 0, resourceCount_ = 0, pathUsed_ = 0, allocatedBytes_ = 0;
  uint32_t lastText_ = NONE;
  uint8_t languageCount_ = 1;
  char languages_[32][36] = {{0}};
};
}} // namespace epub::structure
