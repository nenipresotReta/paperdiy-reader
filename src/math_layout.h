#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>

namespace math_layout {
enum class Status : uint8_t { Ok, Invalid, Unsupported, Limit, Cancelled };
enum class CommandKind : uint8_t { Glyph, Rule };
struct Command {
  CommandKind kind;
  uint8_t pixels;
  uint32_t codepoint;
  int16_t x, y, width, height;
};
// Move-only owning list. Allocation is explicitly nothrow; unlike std::vector,
// failure is recoverable on the firmware's -fno-exceptions build.
class CommandList {
 public:
  CommandList() = default;
  CommandList(CommandList&& other) noexcept;
  CommandList& operator=(CommandList&& other) noexcept;
  bool assign(const Command* source, size_t count);
  // Borrowed storage must outlive this view and is invalidated by its next use.
  void borrow(const Command* source, size_t count);
  const Command* begin() const { return view_; }
  const Command* end() const { return size_ ? view_ + size_ : view_; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  const Command& operator[](size_t i) const { return view_[i]; }
 private:
  std::unique_ptr<Command[]> storage_;
  const Command* view_ = nullptr;
  size_t size_ = 0;
};
struct Options {
  bool displayStyle = false;
  int maxWidth = 486;
  int maxHeight = 862;
  // Called periodically during parsing/layout. False cancels without partial output.
  bool (*keepGoing)(void*) = nullptr;
  void* context = nullptr;
};
struct Result {
  Status status = Status::Invalid;
  int width = 0, ascent = 0, descent = 0;
  // Origin is the top-left of the formula. Glyph.y is its baseline;
  // Rule.y is the rectangle top. No command paints outside result bounds.
  CommandList commands;
  bool ok() const { return status == Status::Ok; }
};
constexpr size_t MAX_SOURCE_BYTES = 4096;
constexpr size_t MAX_NODES = 256;
constexpr size_t MAX_COMMANDS = 1024;
constexpr unsigned MAX_DEPTH = 16;
constexpr unsigned MAX_MATRIX_ROWS = 8;
constexpr unsigned MAX_MATRIX_COLUMNS = 8;
constexpr size_t MAX_SCRATCH_BYTES = 128 * 1024;

// Prepare once before document admission. Reuse sequentially; not thread-safe.
// Large allocation can live in target PSRAM via the caller's allocation policy.
class Workspace {
 public:
  bool prepare();
  bool ready() const { return bool(storage_); }
  void clear() { storage_.reset(); }
  uint8_t* data() const { return storage_.get(); }
 private:
  std::unique_ptr<uint8_t[]> storage_;
};

// Accepts the formula body, without its Markdown $ delimiters. This is an
// intentionally bounded TeX subset, not a TeX interpreter. On every failure the
// draw list is empty: callers must preserve/render the whole original source.
Result layout(const char* source, size_t length, const Options& options = Options());
// No allocation occurs in this overload. Allocate MAX_COMMANDS Command objects
// for each retained result (16 KiB per buffer on the target). Output is written
// only on success. Result.commands borrows output, so each live cached result
// needs its own buffer. Workspace may be reused immediately after return.
Result layoutInto(const char* source, size_t length, const Options& options,
                  Workspace& workspace, Command* output, size_t capacity);
const char* statusName(Status status);
}  // namespace math_layout
