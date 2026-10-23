#include "markdown_pagination.h"
#include "math_layout.h"
#include "native_font_data.h"
#include <tuple>
#include <array>
#include "markdown_samples.h"
#include <cassert>
#include <map>
#include <string>
#include <vector>
#include <cstdio>

static bool accept(const char* source,size_t size,bool display,void*){
  math_layout::Options o;o.displayStyle=display;o.maxHeight=150;
  return math_layout::layout(source,size,o).ok();
}
static std::string visible(const std::string& text){
  std::string out;
  for(size_t p=0;p<text.size();){
    size_t n=pagination::utf8Bytes(text.data()+p,text.size()-p);
    uint32_t cp=pagination::codepoint(text.data()+p,n);
    if(!pagination::space(cp)&&cp!='\n'&&cp!='\r'&&cp!='-'&&cp!=0xfffc)out.append(text,p,n);
    p+=n;
  }
  return out;
}
struct Renderer {
  const markdown::Document& doc;
  std::string drawn;
  std::map<uint32_t,unsigned> formulas;
  int bottom=230,maxMathHeight=150;
  markdown_page::Box glyph(const markdown::Run&,const char* text,size_t bytes){
    markdown_page::Box b;b.width=bytes==1?9:24;b.ascent=31;b.descent=8;return b;
  }
  markdown_page::Box math(const markdown::Run& r){
    math_layout::Options o;o.displayStyle=r.kind==markdown::Kind::MathBlock;o.maxHeight=maxMathHeight;
    auto m=math_layout::layout(doc.source()+r.formulaBegin,r.formulaEnd-r.formulaBegin,o);
    assert(m.ok());markdown_page::Box b;b.width=m.width;b.ascent=m.ascent;b.descent=m.descent;return b;
  }
  void drawGlyph(const markdown::Run& r,const char* text,size_t bytes,int x,int baseline){
    auto b=glyph(r,text,bytes);assert(x>=24&&x+b.width<=540);
    assert(baseline-b.ascent>=24&&baseline+b.descent<bottom);
    drawn.append(text,bytes);
  }
  void drawMath(const markdown::Run& r,int x,int baseline){
    auto b=math(r);assert(x>=24&&x+b.width<=510);
    assert(baseline-b.ascent>=24&&baseline+b.descent<bottom);++formulas[r.sourceBegin];
  }
  void rule(int x,int y,int width,int height){assert(x>=16&&x+width<=510&&y>=24&&y+height<=bottom);}
};
using Mark=std::tuple<uint32_t,int,int>;
struct TypographyRenderer {
  const markdown::Document& doc;std::vector<Mark> marks;
  markdown_page::Box glyph(const markdown::Run& run,const char* text,size_t bytes){
    const uint32_t cp=pagination::codepoint(text,bytes);markdown_page::Box box;
    const auto& font=(run.flags&markdown::Code)?reader_font::FONT_24:reader_font::FONT_28;
    box.width=reader_font::glyphAdvance(font,cp);box.ascent=31;box.descent=8;
    if(const auto* g=reader_font::findGlyph(font,cp)){
      box.ink=g->width&&g->height;box.inkLeft=g->left;box.inkRight=g->left+g->width;
      if(run.flags&(markdown::Strong|markdown::Heading))++box.inkRight;
    }
    return box;
  }
  markdown_page::Box math(const markdown::Run& run){
    auto value=math_layout::layout(doc.source()+run.formulaBegin,run.formulaEnd-run.formulaBegin);
    assert(value.ok());markdown_page::Box box;box.width=value.width;box.ascent=value.ascent;box.descent=value.descent;return box;
  }
  void drawGlyph(const markdown::Run&,const char* text,size_t bytes,int x,int baseline){marks.emplace_back(pagination::codepoint(text,bytes),x,baseline);}
  void drawMath(const markdown::Run& run,int x,int baseline){marks.emplace_back(0xfffc,x,baseline);marks.emplace_back(0,math(run).width,0);}
  void rule(int,int,int,int){}
};
void indentationRegression(){
  const std::vector<std::string> samples={
    "```\n  page_index = 0\n```\n",
    "before\n\n```\n  page_index = 0\n```\n",
    "before\n\n```\n\n  page_index = 0\n\n    next = 1\n```\n",
    "before\n\n- ```\n    page_index = 0\n  ```\n",
    "before\n\n```\n\tpage_index = 0\n```\n",
    "```\n"+std::string(60,'\n')+"  x\n```\n",
    "first **prose**\n\nnext\n",
    "- first\n- second\n",
    "- [x] first\n- [ ] second\n"
  };
  for(const auto& source:samples){
    markdown::Document doc;assert(doc.parse(source.data(),source.size()));
    auto paint=[&](int bottom,unsigned flags){
      markdown_page::Geometry geometry;geometry.bottom=bottom;TypographyRenderer renderer{doc};
      size_t from=0;unsigned pages=0;
      while(from<source.size()){
        const size_t measured=markdown_page::page(doc,from,false,geometry,renderer,[](){},reader_typography::Style::fromFlags(flags));
        const size_t drawn=markdown_page::page(doc,from,true,geometry,renderer,[](){},reader_typography::Style::fromFlags(flags));
        assert(measured==drawn&&drawn>from&&drawn<=source.size());
        from=drawn;assert(++pages<100);
      }
      // Page height may change y, never code indentation or visible content.
      for(auto& mark:renderer.marks)std::get<2>(mark)=0;
      return renderer.marks;
    };
    const auto baseline=paint(916,0);
    for(unsigned flags=0;flags<4;++flags)assert(paint(84,flags)==baseline);
    // Source-zero leading blanks can share an empty source interval. They
    // must not fill a page that returns zero again; every visible byte stays.
    std::vector<uint32_t> expected,actual;
    for(size_t at=0;at<doc.size();){
      const size_t count=pagination::utf8Bytes(doc.text()+at,doc.size()-at);
      const uint32_t cp=pagination::codepoint(doc.text()+at,count);
      if(!pagination::space(cp)&&!pagination::invisible(cp)&&cp!='\n')expected.push_back(cp);
      at+=count;
    }
    for(const auto& mark:baseline){const uint32_t cp=std::get<0>(mark);if(!pagination::space(cp)&&!pagination::invisible(cp)&&cp!='\n')actual.push_back(cp);}
    assert(actual==expected);
    assert(markdown_page::pageStart(doc,source.size(),[](){})==doc.size());
    // Linear source whitespace must not be replayed when resuming within it.
    for(size_t at=0;at<doc.size();++at)if(doc.text()[at]==' '){
      const size_t anchor=doc.sourceOffset(at);
      if(anchor&&doc.renderOffset(anchor)==at)
        assert(markdown_page::pageStart(doc,anchor,[](){})==at);
    }
  }
  // Exact reported case: two generated newlines and two generated spaces all
  // share source anchor 6. Restore the spaces, never those preceding newlines.
  const std::string source="before\n\n```\n  page_index = 0\n```\n";
  markdown::Document doc;assert(doc.parse(source.data(),source.size()));
  assert(doc.renderOffset(6)==10&&markdown_page::pageStart(doc,6,[](){})==8);
  markdown_page::Geometry geometry;geometry.bottom=84;TypographyRenderer renderer{doc};
  assert(markdown_page::page(doc,0,false,geometry,renderer,[](){})==6);
  const size_t next=markdown_page::page(doc,6,true,geometry,renderer,[](){});assert(next>6);
  unsigned found=0;
  for(const auto& mark:renderer.marks)if(std::get<0>(mark)=='p'){
    assert(std::get<1>(mark)==24+2*reader_font::glyphAdvance(reader_font::FONT_24,' '));++found;
  }
  assert(found==1);
}
void typographyRegression(){
 markdown::Document doc;markdown_page::Geometry geometry;
 auto render=[&](const std::string& source,unsigned flags){
   assert(doc.parse(source.data(),source.size(),nullptr,nullptr,accept));TypographyRenderer renderer{doc};
   const auto style=reader_typography::Style::fromFlags(flags);
   size_t from=0;unsigned pages=0;
   while(from<source.size()){
     const size_t measured=markdown_page::page(doc,from,false,geometry,renderer,[](){},style);
     const size_t drawn=markdown_page::page(doc,from,true,geometry,renderer,[](){},style);
     assert(measured==drawn&&drawn>from&&drawn<=source.size());from=drawn;assert(++pages<200);
   }
   return renderer.marks;
 };
 // Each barrier is on both sides. Formula width and baseline/placement remain
 // byte-for-byte identical, including a fraction immediately adjacent to Han.
 for(const std::string source:{"中 A 文", "中`A`文", "中$x$文", "中$\\frac{x+1}{y}$文", "```\n中A。》\n```\n", "中文（A）中文"}){
   const auto baseline=render(source,0);
   for(unsigned flags=1;flags<4;++flags)assert(render(source,flags)==baseline);
 }
 const auto plain=render("中A文",0),gap=render("中A文",2);
 assert(plain.size()==3&&gap.size()==3);
 assert(std::get<1>(gap[1])==std::get<1>(plain[1])+4&&std::get<1>(gap[2])==std::get<1>(plain[2])+8);
 assert(render("中**A**文",2)==gap); // Emphasis/strong text is still prose.
 const auto spaced=render("。”",0),compact=render("。”",1);
 assert(std::get<1>(compact[1])<std::get<1>(spaced[1]));
 std::string many;for(unsigned i=0;i<100;++i)many+="她说：“《中文》很好。”中EPUB3文 `中A` $x_1$。\n\n";
 for(unsigned flags=0;flags<4;++flags){const auto first=render(many,flags);assert(render(many,flags)==first);}
}
int main(){
  indentationRegression();
  typographyRegression();
  markdown::Document d;
  std::string source=markdown_samples::mixed;
  assert(d.parse(source.data(),source.size(),nullptr,nullptr,accept));
  assert(std::string(d.text(),d.size()).find("$\\unsupported{x}$")!=std::string::npos);
  for(int bottom : {230,400,916}){
    Renderer r{d};r.bottom=bottom;markdown_page::Geometry g;g.bottom=bottom;
    size_t position=0;unsigned pages=0;
    while(position<source.size()){
      size_t measured=markdown_page::page(d,position,false,g,r,[](){});
      size_t drawn=markdown_page::page(d,position,true,g,r,[](){});
      assert(measured==drawn&&drawn>position&&drawn<=source.size());
      assert(drawn==source.size()||(uint8_t(source[drawn])&0xc0)!=0x80);
      position=drawn;assert(++pages<100);
    }
    assert(visible(r.drawn)==visible(std::string(d.text(),d.size())));
    unsigned expected=0;
    for(size_t i=0;i<d.runCount();++i)if(markdown_page::formula(d.run(i))){
      ++expected;assert(r.formulas[d.run(i).sourceBegin]==1);
    }
    assert(r.formulas.size()==expected&&expected>=5);
  }
  // Repeated generated markers and block gaps must not stall source offsets.
  source.clear();for(unsigned i=0;i<100;++i)source+="- **word** and 中文 $x_i^2$\n\n";
  assert(d.parse(source.data(),source.size(),nullptr,nullptr,accept));
  Renderer r{d};markdown_page::Geometry g;g.bottom=r.bottom;
  size_t position=0;unsigned pages=0;
  while(position<source.size()){
    size_t next=markdown_page::page(d,position,true,g,r,[](){});
    assert(next>position&&next<=source.size());position=next;assert(++pages<200);
  }
  assert(visible(r.drawn)==visible(std::string(d.text(),d.size())));
  // Each formula fits a page, but their opposing baseline extents do not fit
  // on one line. This formerly returned source offset zero and rejected a book.
  auto matrix=[](unsigned rows,const std::string& body){
    std::string result="\\begin{matrix}";
    for(unsigned i=0;i<rows;++i){if(i)result+="\\\\";result+=body;}
    return result+"\\end{matrix}";
  };
  std::string tall=matrix(4,matrix(8,"x"));
  source="$\\frac{"+tall+"}{x}$ $\\frac{x}{"+tall+"}$";
  assert(d.parse(source.data(),source.size()));
  Renderer high{d};high.bottom=916;high.maxMathHeight=840;
  g.bottom=916;position=0;pages=0;
  while(position<source.size()){
    size_t next=markdown_page::page(d,position,true,g,high,[](){});
    assert(next>position);position=next;assert(++pages<=3);
  }
  assert(high.formulas.size()==2&&pages==2);
  // A code word after normal prose must never gain an automatic hyphen.
  // With this width the old first-run-only policy painted "a representa-".
  source="a `representation` end";
  assert(d.parse(source.data(),source.size()));
  Renderer code{d};markdown_page::Geometry narrow;narrow.right=150;narrow.bottom=code.bottom;
  position=0;
  while(position<source.size()){
    size_t next=markdown_page::page(d,position,true,narrow,code,[](){});
    assert(next>position);position=next;
  }
  assert(code.drawn.find('-')==std::string::npos);
  assert(visible(code.drawn)==visible(std::string(d.text(),d.size())));
  puts("Markdown source bookmarks, variable formula heights, no clipped or repeated content: PASS");
}
