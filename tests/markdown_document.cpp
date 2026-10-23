#include "markdown_document.h"
#include "markdown_pagination.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using markdown::Document;
using markdown::Kind;
using markdown::Run;

void invariant(const Document& d) {
  assert(d.size() <= Document::MaxTextBytes);
  size_t next = 0, priorSource = 0;
  for (size_t i = 0; i < d.runCount(); ++i) {
    const auto& r = d.run(i);
    assert(r.begin == next && r.end > r.begin && r.end <= d.size());
    assert(r.sourceBegin >= priorSource && r.sourceEnd >= r.sourceBegin && r.sourceEnd <= d.sourceSize());
    for (size_t p = r.begin; p < r.end; ++p) assert(d.runAt(p) == i);
    if (r.kind == Kind::MathInline || r.kind == Kind::MathBlock || r.kind == Kind::Rule) {
      assert(r.end - r.begin == 3 && std::memcmp(d.text() + r.begin, "\xef\xbf\xbc", 3) == 0);
      if (r.kind != Kind::Rule)
        assert(r.sourceBegin < r.formulaBegin && r.formulaBegin <= r.formulaEnd && r.formulaEnd < r.sourceEnd);
      if (r.kind == Kind::MathBlock || r.kind == Kind::Rule) {
        assert(!r.begin || d.text()[r.begin - 1] == '\n');
        assert(r.end == d.size() || d.text()[r.end] == '\n');
      }
    }
    next = r.end; priorSource = r.sourceEnd;
  }
  assert(next == d.size() && d.runAt(d.size()) == d.runCount());
  priorSource = 0;
  for (size_t p = 0; p <= d.size(); ++p) {
    const size_t source = d.sourceOffset(p);
    assert(source >= priorSource && source <= d.sourceSize());
    priorSource = source;
  }
  size_t priorRender = 0;
  for (size_t p = 0; p <= d.sourceSize(); ++p) {
    const size_t render = d.renderOffset(p);
    assert(render >= priorRender && render <= d.size()); priorRender = render;
  }
  assert(d.sourceOffset(d.size()) == d.sourceSize());
  assert(d.renderOffset(d.sourceSize()) == d.size());
}
void parsed(Document& d, const std::string& source) {
  if (!d.parse(source.data(), source.size())) {
    std::fprintf(stderr, "parse error %u: %s\n", unsigned(d.error()), source.c_str()); std::abort();
  }
  invariant(d);
}
const Run& atText(const Document& d, const char* needle) {
  const char* found = std::strstr(d.text(), needle); assert(found);
  return d.run(d.runAt(size_t(found - d.text())));
}
void formatting() {
  const std::string raw = "# 标题\n\nplain **bold *inside*** and `code` &amp; &#x1F600; \\*literal\\*\nsoft  \nhard\n\n"
                          "> quote **strong**\n\n- first\n- second\n  - child\n\n3. three\n4. four\n\n"
                          "- [x] done\n- [ ] todo\n\n[label](https://example.invalid/a) ![image alt](pic.png)\n\n"
                          "```cpp\n  if (a < 3) {}\n```\n\n~~gone~~";
  Document d; parsed(d, raw);
  assert(std::strstr(d.text(), "标题\n\nplain bold inside and code & 😀 *literal* soft\nhard"));
  assert(atText(d, "标题").flags & markdown::Heading);
  assert(atText(d, "标题").headingLevel == 1);
  assert((atText(d, "inside").flags & (markdown::Strong | markdown::Emphasis)) == (markdown::Strong | markdown::Emphasis));
  assert(atText(d, "quote").flags & markdown::Quote);
  assert(atText(d, "code").flags & markdown::Code);
  assert(atText(d, "label").flags & markdown::Link);
  assert(atText(d, "image alt").flags & markdown::Image);
  assert(!std::strstr(d.text(), "example.invalid") && !std::strstr(d.text(), "pic.png"));
  assert(std::strstr(d.text(), "• first\n• second\n• child"));
  assert(std::strstr(d.text(), "3. three\n4. four"));
  assert(std::strstr(d.text(), "[x] done\n[ ] todo"));
  assert(atText(d, "child").indent > atText(d, "first").indent);
  assert(atText(d, "if (a").flags & markdown::Preformatted);
  assert(atText(d, "gone").flags & markdown::Strike);
  // Bookmarks in the middle of a styled UTF-8 text run remain source byte positions.
  const std::string source = "prefix **中文 words continued** tail";
  parsed(d, source);
  for (const char* word : {"中文", "words", "continued", "tail"}) {
    const size_t r = size_t(std::strstr(d.text(), word) - d.text());
    const size_t s = source.find(word);
    for (size_t p = 0; p < std::strlen(word); ++p) {
      assert(d.sourceOffset(r + p) == s + p);
      assert(d.renderOffset(s + p) == r + p);
    }
  }
}
void math() {
  Document d;
  const std::string source = "前 $a+b$ 后\n\n$$\nx + 1 % preserve comment\n+ y\n$$\n\nend";
  parsed(d, source);
  unsigned inlineCount = 0, blockCount = 0;
  for (size_t i = 0; i < d.runCount(); ++i) {
    const auto& r = d.run(i);
    if (r.kind == Kind::MathInline || r.kind == Kind::MathBlock) {
      const std::string formula(source.data() + r.formulaBegin, r.formulaEnd - r.formulaBegin);
      if (r.kind == Kind::MathInline) { ++inlineCount; assert(formula == "a+b"); }
      else { ++blockCount; assert(formula == "\nx + 1 % preserve comment\n+ y\n"); }
      for (size_t p = r.sourceBegin; p < r.sourceEnd; ++p) assert(d.renderOffset(p) == r.begin);
      assert(d.sourceOffset(r.begin) == r.sourceBegin);
      assert(d.sourceOffset(r.end) >= r.sourceEnd);
    }
  }
  assert(inlineCount == 1 && blockCount == 1);
  const std::string rejected = "before $$a % comment\n\\unknown{z}$$ after $bad$";
  assert(d.parse(rejected.data(), rejected.size(), nullptr, nullptr,
                 [](const char*,size_t,bool,void*) { return false; }));
  invariant(d);
  assert(std::strstr(d.text(), "$$a % comment\n\\unknown{z}$$"));
  assert(std::strstr(d.text(), "$bad$"));
  for (size_t i = 0; i < d.runCount(); ++i) assert(d.run(i).kind == Kind::Text);
  assert(atText(d, "\\unknown").flags & markdown::Preformatted);
  const std::string escape = "\\$literal$ and `$code$` and $$a$$ tail\n\n$unfinished";
  parsed(d, escape); assert(std::strstr(d.text(), "$code$")); assert(std::strstr(d.text(), "$unfinished"));
  // MD4C can normalize code span newlines, but must never normalize formula raw bytes.
  const std::string multiline = "$a\r\nb$ and `c\nd`";
  parsed(d, multiline);
  assert(std::string(multiline.data() + d.run(0).formulaBegin,
                     d.run(0).formulaEnd - d.run(0).formulaBegin) == "a\r\nb");
}
void texDelimiters() {
  Document d;
  const std::string source = "中文 \\(a+1\\) 继续\n\n\\[\nx % comment\n\n+ y\n\\]\n\n结束";
  parsed(d, source);
  unsigned formulas = 0;
  for (size_t i = 0; i < d.runCount(); ++i) {
    const auto& r = d.run(i);
    if (r.kind == Kind::MathInline || r.kind == Kind::MathBlock) {
      ++formulas;
      if (r.kind == Kind::MathInline)
        assert(source.substr(r.formulaBegin,r.formulaEnd-r.formulaBegin) == "a+1");
      else assert(source.substr(r.formulaBegin,r.formulaEnd-r.formulaBegin) == "\nx % comment\n\n+ y\n");
      assert(r.formulaBegin == r.sourceBegin + 2 && r.formulaEnd == r.sourceEnd - 2);
    }
  }
  assert(formulas == 2);
  const std::string literal = "`\\(x\\)` and `\\[y\\]`\n\n```tex\n\\(a\\)\n\\[b\\]\n```\n\n\\\\(escaped\\\\) and \\(unfinished";
  parsed(d,literal);
  for (size_t i = 0; i < d.runCount(); ++i) assert(d.run(i).kind == Kind::Text);
  assert(std::strstr(d.text(),"\\(x\\)") && std::strstr(d.text(),"\\[b\\]"));
  assert(std::strstr(d.text(),"\\(escaped\\)") && std::strstr(d.text(),"\\(unfinished"));
  const std::string inlineDisplay = "前\\[a+b\\]后";
  parsed(d,inlineDisplay);
  assert(d.run(1).kind == Kind::MathBlock || d.run(2).kind == Kind::MathBlock);
  const std::string reject = "x \\(\\unknown{a}\\) y";
  assert(d.parse(reject.data(),reject.size(),nullptr,nullptr,[](const char*,size_t,bool,void*){return false;}));
  invariant(d); assert(std::strstr(d.text(),"\\(\\unknown{a}\\)"));
  // Repeated unmatched openers must not rescan the rest of the paragraph.
  size_t ticks = 0; std::string unmatched;
  for (unsigned i = 0; i < 3000; ++i) unmatched += "\\( x ";
  unmatched += "end";
  assert(d.parse(unmatched.data(),unmatched.size(),[](void* p){++*static_cast<size_t*>(p);},&ticks));
  assert(ticks < unmatched.size()*3 && std::string(d.text(),d.size()) == unmatched);
  ticks = 0; unmatched.clear();
  for (unsigned i = 0; i < 1500; ++i) unmatched += "\\[ x\n";
  assert(d.parse(unmatched.data(),unmatched.size(),[](void* p){++*static_cast<size_t*>(p);},&ticks));
  assert(ticks < unmatched.size()*5);
}
void blocksAndAnchors() {
  Document d;
  const std::string source = "before\n\n---\n\nafter\n\n| h | v |\n|---|---|\n| **raw** | $x$ |\n\nfinal";
  parsed(d, source);
  assert(std::strstr(d.text(), "| h | v |\n|---|---|\n| **raw** | $x$ |"));
  assert(atText(d, "| h").flags & markdown::Preformatted);
  unsigned rules = 0;
  for (size_t i = 0; i < d.runCount(); ++i) if (d.run(i).kind == Kind::Rule) {
    ++rules; const auto& r = d.run(i);
    assert(source.substr(r.sourceBegin, r.sourceEnd-r.sourceBegin) == "---");
  }
  assert(rules == 1);
  // Generated markers and block boundaries still lead to advancing source positions
  // after each visible line; no bookmark relies on pointers into generated literals.
  const std::string lists = "- alpha\n- 中文\n- gamma\n\nlast";
  parsed(d, lists);
  size_t previous = 0;
  for (size_t p = 0; p <= d.size(); ++p) if (p == d.size() || d.text()[p] == '\n') {
    if (p && d.text()[p-1] != '\n') {
      const size_t original = d.sourceOffset(std::min(p+1,d.size()));
      assert(original > previous); previous = original;
    }
  }
  const std::string entities = "&#0; &#xD800; &#1114112; &unknown; &lt; &copy;";
  parsed(d, entities); assert(std::strstr(d.text(), "� � � &unknown; < ©"));
  const std::string signature = "\xef\xbb\xbf# 中文\n\ncontent";
  parsed(d, signature);
  assert(std::string(d.text()).find("中文") == 0 && (d.run(0).flags & markdown::Heading));
  assert(d.sourceOffset(0) == 5 && d.renderOffset(5) == 0);
  const std::string refs = "[label][id]\n\n[id]: https://example.invalid \"some &amp; title\"\n";
  parsed(d, refs); assert(std::string(d.text(), d.size()) == "label");
  assert(d.sourceOffset(d.size()) == refs.size());
  parsed(d, std::string()); assert(d.size() == 0);
  assert(d.parse(nullptr,0)); assert(!d.parse(nullptr,1));
  const char bad[] = {char(0xf0),char(0x80),char(0x80),char(0x80)};
  assert(!d.parse(bad,sizeof(bad)) && d.error() == markdown::Error::InvalidUtf8);
  const char zero[] = {'a',0,'b'}; assert(!d.parse(zero,sizeof(zero)));
}

struct FaultAllocator {
  std::unordered_map<void*,size_t> live;
  size_t calls = 0, failOn = SIZE_MAX, current = 0, peak = 0;
  static void* resize(void* old, size_t bytes, void* context) {
    auto& a = *static_cast<FaultAllocator*>(context);
    if (++a.calls == a.failOn) return nullptr;
    const size_t oldSize = old ? a.live.at(old) : 0;
    // Always move successful allocations: catches stale pointers after partial realloc.
    void* p = std::malloc(bytes); assert(p);
    if (old) { std::memcpy(p,old,std::min(oldSize,bytes)); a.live.erase(old); std::free(old); }
    a.live[p] = bytes; a.current = a.current - oldSize + bytes; a.peak = std::max(a.peak,a.current);
    return p;
  }
  static void release(void* memory,void* context) {
    auto& a = *static_cast<FaultAllocator*>(context);
    a.current -= a.live.at(memory); a.live.erase(memory); std::free(memory);
  }
  markdown::Allocator allocator() { return {resize,release,this}; }
};
void failuresAndBounds() {
  const std::string source = "# Head\n\n[label](https://e.invalid/\\(x\\) \"&amp;&lt;&gt;&quot;&apos;&copy;&AElig;&aacute;&eacute;&oacute;&uacute;&hellip;\")\n\n"
                             "[ref][id] **strong** $x$\n\n```x&amp;y title &amp;&lt;&gt;\n    code\n```\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\n[id]: /x \"&lt;&gt;&amp;&quot;&apos;&copy;&aacute;&eacute;&oacute;&uacute;\"\n";
  size_t calls = 0;
  { FaultAllocator a; { Document d(a.allocator()); parsed(d,source); calls = a.calls; }
    assert(a.live.empty() && a.current == 0); }
  assert(calls > 10);
  for (size_t fail = 1; fail <= calls; ++fail) {
    FaultAllocator a; a.failOn = fail;
    { Document d(a.allocator()); assert(!d.parse(source.data(),source.size()));
      assert(d.error() == markdown::Error::Allocation && d.size() == 0 && d.runCount() == 0);
      assert(a.live.empty()); a.failOn = SIZE_MAX; parsed(d, source); }
    assert(a.live.empty());
  }
  Document d; size_t ticks = 0;
  const std::string large(Document::MaxSourceBytes,'a');
  assert(d.parse(large.data(),large.size(),[](void* p) { ++*static_cast<size_t*>(p); },&ticks));
  assert(d.size() == large.size() && ticks > 250 && d.runCount() == 1);
  assert(!d.parse(large.data(),large.size()+1) && d.error() == markdown::Error::SourceLimit);
  std::string styles;
  for (unsigned i = 0; i < 2500; ++i) styles += "a *b* ";
  assert(!d.parse(styles.data(),styles.size()) && d.error() == markdown::Error::RunLimit);
  std::string mappings;
  for (unsigned i = 0; i < 9000; ++i) mappings += "&amp;";
  assert(!d.parse(mappings.data(),mappings.size()) && d.error() == markdown::Error::MappingLimit);
  // Many syntax marks / huge block tables are allowed to hit explicit budgets.
  std::string heavy;
  for (unsigned i = 0; i < 30000; ++i) heavy += "[x](a) ";
  assert(!d.parse(heavy.data(),heavy.size()));
  assert(d.error() == markdown::Error::ParserBudget || d.error() == markdown::Error::RunLimit || d.error() == markdown::Error::MappingLimit);
  assert(d.size() == 0);
  std::string nesting(140,'>'); nesting += " text";
  assert(!d.parse(nesting.data(),nesting.size()) && d.error() == markdown::Error::NestingLimit);
  parsed(d,"reusable");
}
struct PageRenderer {
  std::string drawn;
  markdown_page::Box glyph(const Run&,const char*,size_t n) {
    markdown_page::Box box; box.width = n == 1 ? 5 : 10; box.ascent = 31; box.descent = 8; return box;
  }
  markdown_page::Box math(const Run&) {
    markdown_page::Box box; box.width = 20; box.ascent = 20; box.descent = 8; return box;
  }
  void rule(int,int,int,int) {}
  void drawMath(const Run&,int,int) { drawn += "MATH"; }
  void drawGlyph(const Run&,const char* ch,size_t n,int,int) { drawn.append(ch,n); }
};
void listPageRestoration() {
  Document d; const std::string source = "- alpha\n- bravo\n- charlie\n- delta";
  parsed(d,source);
  markdown_page::Geometry geometry; geometry.left = 0; geometry.right = 300;
  geometry.top = 0; geometry.bottom = 40; geometry.lineHeight = 40;
  size_t offset = 0;
  for (const char* expected : {"• alpha", "• bravo", "• charlie", "• delta"}) {
    PageRenderer renderer;
    const size_t next = markdown_page::page(d,offset,true,geometry,renderer,[](){});
    assert(renderer.drawn == expected && next > offset);
    PageRenderer restored;
    const size_t repeat = markdown_page::page(d,offset,true,geometry,restored,[](){});
    assert(restored.drawn == expected && repeat == next); offset = next;
  }
  assert(offset == source.size());
  const std::string code = "- ```\n\n  code\n  ```\n- next";
  parsed(d,code); assert(std::strstr(d.text(),"• ") && std::strstr(d.text(),"code"));
  const std::string math = "- $$x$$\n- next";
  parsed(d,math);
  const size_t marker = size_t(std::strstr(d.text(),"•") - d.text());
  assert(d.renderOffset(d.sourceOffset(marker)) == marker);
}
void boundedFuzz() {
  Document d; uint32_t random = 0x3456789;
  const char alphabet[] = "abc XYZ0123\n\r\t*_$`[]()<>!&;#|~\\.:-";
  for (unsigned iteration = 0; iteration < 1800; ++iteration) {
    std::string source;
    for (unsigned n = 0; n < (iteration % 350); ++n) {
      random = random * 1664525 + 1013904223;
      source += alphabet[random % (sizeof(alphabet)-1)];
    }
    if (d.parse(source.data(),source.size())) invariant(d);
    else assert(d.size() == 0 && d.runCount() == 0 && d.error() == markdown::Error::Parser);
  }
}
int main() {
  formatting(); math(); texDelimiters(); blocksAndAnchors(); failuresAndBounds(); listPageRestoration(); boundedFuzz();
  std::puts("markdown document: raw math, source anchors, styles, allocation failures and bounded fuzz passed");
}
