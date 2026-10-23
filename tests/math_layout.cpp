#include "math_layout.h"
#include "math_font.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace math_layout;
static Result formula(const char* text, bool display = false) {
  Options o; o.displayStyle = display;
  return layout(text, std::strlen(text), o);
}
static void boundedInk(const Result& r) {
  assert(r.ok() && !r.commands.empty());
  assert(r.width > 0 && r.width <= 486 && r.ascent + r.descent <= 862);
  for (const auto& c : r.commands) {
    if (c.kind == CommandKind::Rule) {
      assert(c.x >= 0 && c.y >= 0 && c.width > 0 && c.height > 0);
      assert(c.x + c.width <= r.width && c.y + c.height <= r.ascent + r.descent);
    } else {
      assert(math_font::drawGlyphRuns(c.codepoint, c.pixels, c.x, c.y, [&](int x, int y, int n) {
        assert(x >= 0 && y >= 0 && n > 0);
        assert(x + n <= r.width && y < r.ascent + r.descent);
      }));
    }
  }
}
static void rejected(const std::string& s, Status expected) {
  const auto r = layout(s.data(), s.size());
  if (r.status != expected) std::fprintf(stderr, "Expected %s, got %s for %s\n", statusName(expected), statusName(r.status), s.c_str());
  assert(r.status == expected && r.commands.empty());
  assert(!r.width && !r.ascent && !r.descent);
}
static void render(const char* path) {
  const int w = 540, h = 960;
  std::vector<unsigned char> image(w * h, 255);
  const char* samples[] = {
    "E = mc^2",
    "x = \\frac{-b \\pm \\sqrt{b^2-4ac}}{2a}",
    "\\sum_{n=1}^{\\infty} \\frac{1}{n^2} = \\frac{\\pi^2}{6}",
    "\\int_0^1 x^2\\,\\mathrm{d}x = \\frac{1}{3}",
    "\\mathbf{A}=\\begin{pmatrix}a&b\\\\c&d\\end{pmatrix}",
    "\\mathbb{R} \\supset \\mathbb{Q},\\quad \\alpha \\notin \\emptyset",
    "\\lim_{x\\to0}\\frac{\\sin x}{x}=1",
    "\\sqrt[3]{x_i^2 + y_i^2} \\quad \\text{where } i \\ge 0",
    "\\left\\langle \\frac{a}{b}, \\frac{c}{d} \\right\\rangle",
    "\\hat{x}+\\bar{y}+\\vec{v}"
  };
  int y = 18;
  for (const char* s : samples) {
    auto r = formula(s, true);
    if (!r.ok()) { std::fprintf(stderr, "Render failed %s: %s\n", statusName(r.status), s); assert(false); }
    boundedInk(r);
    const int x = (w - r.width) / 2;
    auto ink = [&](int xx, int yy, int len) {
      assert(xx >= 0 && xx + len <= w && yy >= 0 && yy < h);
      std::fill(image.begin() + yy * w + xx, image.begin() + yy * w + xx + len, 0);
    };
    for (const auto& c : r.commands) {
      if (c.kind == CommandKind::Glyph) math_font::drawGlyphRuns(c.codepoint, c.pixels, x + c.x, y + c.y, ink);
      else for (int row = 0; row < c.height; ++row) ink(x + c.x, y + c.y + row, c.width);
    }
    y += r.ascent + r.descent + 24;
  }
  std::ofstream f(path, std::ios::binary); f << "P5\n" << w << " " << h << "\n255\n";
  f.write(reinterpret_cast<const char*>(image.data()), image.size());
}
int main(int argc, char** argv) {
  const char* valid[] = {
    "x", "x^2", "a_{i_j}^{n+1}", "\\frac{1}{2}", "\\dfrac{a+b}{c-d}",
    "\\sqrt{x}", "\\sqrt[3]{x+1}", "\\frac{1}{\\frac{x}{y}+1}",
    "\\left(\\frac{a}{b}\\right)", "\\left. x^2 \\right|", "\\binom{n}{k}",
    "\\sum_{i=1}^n i", "\\prod_{i=1}^n i", "\\int_0^1 x\\,dx", "\\oint_{C} f(z)dz",
    "\\sum\\limits_{a}^{b}x", "\\sum\\nolimits_{a}^{b}x", "\\lim_{x\\to0}x",
    "\\begin{matrix}1&2\\\\3&4\\end{matrix}", "\\begin{bmatrix}a&b\\\\c&d\\end{bmatrix}",
    "\\begin{cases}x & x>0\\\\-x & x<0\\end{cases}",
    "\\begin{aligned}a &= b+c\\\\long &= d\\end{aligned}",
    "\\begin{aligned}a &= b & c &= d\\\\x &= y & z &= w\\end{aligned}",
    "\\mathbb{R} \\supset \\mathbb{Q}", "\\mathbf{v}=\\mathcal{F}(x)",
    "\\text{if } x \\in \\mathbb{Z}", "\\operatorname{rank}(A)",
    "\\epsilon+\\varepsilon+\\varrho+\\phi+\\varphi", "\\alpha+\\beta+\\Gamma",
    "f''(x)+\\hat{x}+\\bar{y}+\\vec{v}", "a\\!+b\\!", "x % comment\n + y",
    "\\underline{x} + \\overline{abc}", "\\text{cost \\$5, \\{ok\\}}", "\\mathrm{d}x",
    "∀x∈ℝ:x²≥0"
  };
  for (const char* s : valid) for (bool display : {false, true}) {
    auto r = formula(s, display);
    if (!r.ok()) std::fprintf(stderr, "Valid failed %s: %s\n", statusName(r.status), s);
    boundedInk(r);
  }
  const auto plain = formula("x"), power = formula("x^2"), sub = formula("x_2");
  assert(power.ascent > plain.ascent && sub.descent > plain.descent);
  const auto frac = formula("\\frac{1}{2}");
  assert(frac.ascent + frac.descent > plain.ascent + plain.descent);
  assert(std::count_if(frac.commands.begin(), frac.commands.end(), [](const Command& c) { return c.kind == CommandKind::Rule; }) == 1);
  const auto root = formula("\\sqrt{x}");
  assert(root.width > plain.width && root.ascent > plain.ascent);
  const auto sumInline = formula("\\sum_{i=1}^n x"), sumDisplay = formula("\\sum_{i=1}^n x", true);
  assert(sumDisplay.ascent + sumDisplay.descent > sumInline.ascent + sumInline.descent);
  const auto italic = formula("x"), roman = formula("\\mathrm{x}");
  assert(italic.commands[0].codepoint != roman.commands[0].codepoint);
  assert(formula("\\frac{x^2}{y_1}").commands.size() >= 5);
  const auto aligned = formula("\\begin{aligned}a &= b\\\\abcd &= c\\end{aligned}",true);
  int equalsX = -1, countEquals = 0;
  for (const auto& c : aligned.commands) if (c.kind == CommandKind::Glyph && c.codepoint == '=') {
    if (equalsX < 0) equalsX = c.x; else assert(equalsX == c.x);
    ++countEquals;
  }
  assert(countEquals == 2);
  for (const char* s : {"", "x_", "x^^2", "x_1_2", "{x", "x}", "\\frac{1}", "\\left(x", "\\right)",
      "\\begin{matrix}1&2\\\\3\\end{matrix}", "\\begin{matrix}x\\end{pmatrix}"}) rejected(s, Status::Invalid);
  for (const char* s : {"a+\\unknown{x}", "\\text{中文}", "\\begin{align}a&=b\\end{align}", "\\input{a}", "\\color{red}{x}"}) rejected(s, Status::Unsupported);
  rejected(std::string("x\0y", 3), Status::Invalid);
  rejected(std::string("\xC0\xAF", 2), Status::Invalid);
  rejected(std::string("\xED\xA0\x80", 3), Status::Invalid);
  rejected(std::string("\xF4\x90\x80\x80", 4), Status::Invalid);
  rejected(std::string(MAX_SOURCE_BYTES + 1, ' '), Status::Limit);
  rejected(std::string(MAX_NODES + 1, 'x'), Status::Limit);
  rejected(std::string(MAX_DEPTH + 1, '{') + "x" + std::string(MAX_DEPTH + 1, '}'), Status::Limit);
  std::string tooManyRows = "\\begin{matrix}x"; for (unsigned i=0;i<MAX_MATRIX_ROWS;++i) tooManyRows += "\\\\x"; tooManyRows += "\\end{matrix}";
  rejected(tooManyRows, Status::Limit);
  Options o; o.maxWidth = 1;
  assert(layout("x",1,o).status == Status::Limit);
  o.maxWidth = 486; o.maxHeight = 1;
  assert(layout("x",1,o).status == Status::Limit);
  int polls = 0; o = Options(); o.context = &polls;
  o.keepGoing = [](void* p) { return ++*static_cast<int*>(p) < 3; };
  const std::string work(200, 'x');
  const auto cancelled = layout(work.data(), work.size(), o);
  assert(cancelled.status == Status::Cancelled && cancelled.commands.empty() && polls >= 3);
  for (uint8_t size : {14,20,28}) for (uint32_t cp : {0x78,0x1D465,0x211D,0x2209,0x2211,0x222B,0x27E8,0x3B1}) {
    const auto* g = math_font::find(cp,size); assert(g && g->width && g->height);
    assert(g->bitmapOffset + ((g->width+7)/8)*g->height <= math_font::bitmapSize());
  }
  // Deterministic hostile inputs exercise parser failure and bounds paths.
  uint32_t state = 0x34B2A17;
  const char fuzzChars[] = "abc012{}[]_^\\%&$+ !\n";
  for (unsigned trial = 0; trial < 2000; ++trial) {
    std::string input;
    for (unsigned i = 0; i < trial % 90; ++i) { state = state*1664525u+1013904223u; input += fuzzChars[state % (sizeof(fuzzChars)-1)]; }
    const auto r = layout(input.data(),input.size());
    if (r.ok()) boundedInk(r); else assert(r.commands.empty() && !r.width && !r.ascent && !r.descent);
  }
  if (argc == 2) render(argv[1]);
  std::printf("math layout: passed; %zu glyphs, %zu bitmap bytes\n", math_font::glyphCount(), math_font::bitmapSize());
}
