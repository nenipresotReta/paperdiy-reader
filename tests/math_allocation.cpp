#include "math_layout.h"
#include <cassert>
#include <array>
#include <cstring>
#include <new>
#include <utility>

static int failAfter = -1, calls = 0;
static size_t largest = 0;
// Replace only the nothrow array allocator used by the production renderer.
// Forward successful calls to the standard throwing allocator so ASan tracks
// ordinary new[]/delete[] pairs. Tests never request an actual huge allocation.
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
  ++calls;
  if (bytes > largest) largest = bytes;
  if (failAfter == 0) return nullptr;
  if (failAfter > 0) --failAfter;
  return ::operator new[](bytes);
}
int main() {
  const char* source = "\\frac{x^2+1}{\\sqrt{y}}";
  for (int failure = 0; failure < 2; ++failure) {
    failAfter = failure; calls = 0;
    auto r = math_layout::layout(source, std::strlen(source));
    assert(r.status == math_layout::Status::Limit);
    assert(r.commands.empty() && !r.width && !r.ascent && !r.descent);
    assert(calls == failure + 1);
  }
  failAfter = -1; calls = 0;
  auto good = math_layout::layout(source, std::strlen(source));
  assert(good.ok() && calls == 2 && largest == math_layout::MAX_SCRATCH_BYTES);
  const size_t n = good.commands.size();
  math_layout::CommandList moved(std::move(good.commands));
  assert(good.commands.empty() && good.commands.begin() == good.commands.end());
  assert(moved.size() == n);
  good.commands = std::move(moved);
  assert(moved.empty() && good.commands.size() == n);
  failAfter = 0;
  math_layout::Command c{};
  assert(!good.commands.assign(&c,1));
  assert(good.commands.size() == n);  // Failed replacement preserves old output.

  math_layout::Workspace workspace;
  std::array<math_layout::Command, math_layout::MAX_COMMANDS> output{};
  auto into = [&]() { return math_layout::layoutInto(source,std::strlen(source),math_layout::Options(),workspace,output.data(),output.size()); };
  assert(!workspace.prepare());  // OOM is explicit at admission time.
  assert(into().status == math_layout::Status::Limit);
  failAfter = -1;
  assert(workspace.prepare() && workspace.ready());
  failAfter = 0; calls = 0;
  assert(workspace.prepare());  // Already prepared: no allocation.
  for (unsigned repeat = 0; repeat < 100; ++repeat) {
    auto r = into();
    assert(r.ok() && r.commands.size() == n && r.commands.begin() == output.data());
    for (size_t i=0;i<n;++i) {
      const auto& a=good.commands[i]; const auto& b=r.commands[i];
      assert(a.kind==b.kind && a.pixels==b.pixels && a.codepoint==b.codepoint &&
             a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height);
    }
  }
  assert(calls == 0);  // The entire runtime layout path is allocation-free.
  std::array<unsigned char,sizeof(output)> old;
  std::memcpy(old.data(),output.data(),sizeof(output));
  auto small = math_layout::layoutInto(source,std::strlen(source),math_layout::Options(),workspace,output.data(),1);
  assert(small.status == math_layout::Status::Limit && small.commands.empty());
  assert(std::memcmp(old.data(),output.data(),sizeof(output)) == 0);
  workspace.clear();
  assert(!workspace.ready() && into().status == math_layout::Status::Limit);
}
