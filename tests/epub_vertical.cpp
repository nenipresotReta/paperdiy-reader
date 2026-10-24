#include "epub_core.h"
#include "epub_document.h"
#include "epub_vertical.h"
#include "native_font_data.h"
#include <cassert>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include <cstdio>
namespace {
struct Fixture {
  std::map<std::string,std::string> files;std::vector<char> output;epub::structure::Collector collector;epub_document::Document doc;
  Fixture(const std::string& html,const std::string& css="",const std::string& progression=""){
    files["META-INF/container.xml"]="<container><rootfiles><rootfile full-path='book.opf'/></rootfiles></container>";
    files["book.opf"]="<package><metadata><language>ja</language></metadata><manifest><item id='c' href='c.xhtml'/></manifest><spine "+progression+"><itemref idref='c'/></spine></package>";
    files["c.xhtml"]=html;files["book.css"]=css;output.resize(65536);
    epub::Importer importer;importer.output=output.data();importer.capacity=output.size()-1;importer.alloc=[](size_t n){return std::malloc(n);};importer.structure=&collector;
    importer.resourceSize=[&](const std::string& p){auto i=files.find(p);return i==files.end()?SIZE_MAX:i->second.size();};
    importer.resourceRead=[&](const std::string& p,void* out,size_t n){auto i=files.find(p);if(i==files.end()||i->second.size()!=n)return false;std::memcpy(out,i->second.data(),n);return true;};
    assert(importer.run());output.resize(importer.used+1);assert(doc.assign(collector,output.data(),importer.used));
  }
};
struct Renderer {
  using Mark=std::tuple<uint32_t,unsigned,int,int,bool,size_t,size_t>;std::vector<Mark> marks;std::vector<uint8_t> pixels=std::vector<uint8_t>(540*960);bool rotate=false,inspectInk=false,rejectCollisions=false;epub_vertical::Geometry geometry;
  std::map<size_t,std::set<std::pair<int,int>>> inkBySource;
  const reader_font::Font& face()const{return geometry.pixels==24?reader_font::FONT_24:reader_font::FONT_28;}
  int advance(uint32_t cp){const auto* g=reader_font::findGlyph(face(),cp);return std::max(reader_font::glyphAdvance(face(),cp),g?int(g->width)+std::max(0,int(g->left)):0);}
  void glyph(uint32_t cp,unsigned size,int x,int y,bool sideways,size_t begin,size_t end){
    marks.emplace_back(cp,size,x,y,sideways,begin,end);
    vertical_font::draw(face(),cp,size,x,y,sideways,[&](int left,int top,int w,int h){
      assert(left>=geometry.left&&top>=geometry.top&&left+w<=geometry.right&&top+h<=geometry.bottom);
      for(int row=top;row<top+h;++row)for(int col=left;col<left+w;++col){int px=rotate?539-col:col,py=rotate?959-row:row;if(rejectCollisions)assert(!pixels[py*540+px]);pixels[py*540+px]=1;if(inspectInk)inkBySource[begin].emplace(col,row);}
    });
  }
};
}
int main(int argc,char** argv){
  Fixture horizontal("<html><body><p>日本 ABC 12。</p></body></html>","","page-progression-direction='rtl'");assert(!horizontal.doc.hasVertical());
  std::vector<uint8_t> metadata;assert(horizontal.doc.encode(metadata)&&!std::memcmp(metadata.data(),"PDS1",4));
  Fixture f("<html><head><link rel='stylesheet' href='book.css'/></head><body><p>日本「かな」、<span class='tcy'>12</span> ABC。<ruby>漢字<rp>（</rp><rt>かんじ</rt><rp>）</rp></ruby>終。</p><p style='writing-mode:horizontal-tb'>English.</p></body></html>","/* declaration */ body { writing-mode: vertical-rl; } .tcy { text-combine-upright: all; }");
  assert(f.doc.hasVertical()&&f.doc.verticalAt(0)&&f.doc.rubyCount()==1);
  const std::string text(f.doc.text(),f.doc.size());assert(text==u8"日本「かな」、12 ABC。漢字（かんじ）終。\n\nEnglish.\n\n");
  const auto& ruby=f.doc.ruby(0);assert(text.substr(ruby.begin,ruby.baseEnd-ruby.begin)==u8"漢字");assert(text.substr(ruby.noteBegin,ruby.noteEnd-ruby.noteBegin)==u8"かんじ");
  assert(f.doc.rubyAt(ruby.noteBegin)==&ruby&&f.doc.rubyAt(ruby.end)==nullptr);
  assert(f.doc.combineAt(text.find("12")));assert(!f.doc.verticalAt(text.find("English")));assert(f.doc.modeEnd(0)==text.find("English"));
  assert(f.doc.encode(metadata)&&!std::memcmp(metadata.data(),"PDS2",4));epub_document::Document decoded;
  assert(decoded.decode(metadata.data(),metadata.size(),f.doc.text(),f.doc.size())&&decoded.rubyCount()==1&&decoded.hasVertical());
  auto broken=metadata;broken[20]=1;assert(!decoded.decode(broken.data(),broken.size(),f.doc.text(),f.doc.size()));
  broken=metadata;broken[24+13]=4;assert(!decoded.decode(broken.data(),broken.size(),f.doc.text(),f.doc.size()));
  broken=metadata;broken.back()=255;assert(!decoded.decode(broken.data(),broken.size(),f.doc.text(),f.doc.size()));
  assert(vertical_font::sideways('"',24)&&vertical_font::sideways('"',28)&&vertical_font::sideways('\'',24));
  assert(!vertical_font::sideways(0x65e5)&&vertical_font::sideways('A')&&vertical_font::sideways(0xe9));
  for(uint32_t cp:{0x3001u,0x3002u,0x300cu,0x300du,0x30fcu,0x3041u})assert(vertical_font::has(cp,28)&&!vertical_font::sideways(cp));
  Renderer a,b;b.rotate=true;epub_vertical::Geometry g;a.geometry=b.geometry=g;
  const size_t measured=epub_vertical::page(f.doc,0,false,g,a,[](){});assert(a.marks.empty());
  assert(epub_vertical::page(f.doc,0,true,g,a,[](){})==measured&&measured==f.doc.modeEnd(0));
  assert(epub_vertical::page(f.doc,0,true,g,b,[](){})==measured&&a.marks==b.marks);
  for(int y=0;y<960;++y)for(int x=0;x<540;++x)assert(a.pixels[y*540+x]==b.pixels[(959-y)*540+(539-x)]);
  bool tcy=false,sidewaysLatin=false,rubySmall=false;for(const auto& mark:a.marks){uint32_t cp;unsigned px;int x,y;bool side;size_t begin,end;std::tie(cp,px,x,y,side,begin,end)=mark;assert(begin<end&&end<=f.doc.size());tcy|=cp=='1'&&px==14&&!side;sidewaysLatin|=cp=='A'&&px==28&&side;rubySmall|=cp==0x304bu&&px==14;}assert(tcy&&sidewaysLatin&&rubySmall);
  if(argc==2){FILE* file=std::fopen(argv[1],"wb");assert(file);std::fputs("P5\n540 960\n255\n",file);for(auto pixel:a.pixels)std::fputc(pixel?0:255,file);assert(!std::fclose(file));}
  // Both native ruby sizes retain full g/j/p/q/y descenders. Compare the actual
  // composed gutter pixels with every source-font bitmap pixel, not a box/count.
  for(int body:{24,28}){
    Fixture latinRuby("<html style='writing-mode:vertical-rl'><body><ruby>漢字<rt>gjpqy</rt></ruby></body></html>");
    assert(latinRuby.doc.rubyCount()==1);epub_vertical::Geometry shape;shape.pixels=body;Renderer actual;actual.geometry=shape;
    const auto& group=latinRuby.doc.ruby(0);assert(epub_vertical::rubyAvailable(latinRuby.doc,group,unsigned(body/2)));
    const size_t stop=epub_vertical::page(latinRuby.doc,0,false,shape,actual,[](){});assert(actual.marks.empty());
    assert(epub_vertical::page(latinRuby.doc,0,true,shape,actual,[](){})==stop&&stop==latinRuby.doc.size());
    unsigned readings=0;size_t note=group.noteBegin;
    for(const auto& mark:actual.marks){uint32_t cp;unsigned px;int x,y;bool side;size_t begin,end;std::tie(cp,px,x,y,side,begin,end)=mark;
      if(px!=unsigned(body/2))continue;++readings;assert(!side&&begin==note&&end==begin+1);note=end;
      const auto& font=vertical_font::font(px);const auto* glyph=reader_font::findGlyph(font,cp);assert(glyph&&glyph->height&&glyph->advance<=px+2);
      if(cp=='j')assert(glyph->height>px); // native descender extends the small cell
      unsigned ink=0;for(int row=0;row<glyph->advance;++row)for(unsigned col=0;col<px;++col){
        const int gx=int(col)-glyph->left,gy=row-glyph->top;const bool expected=gx>=0&&gy>=0&&gx<glyph->width&&gy<glyph->height&&reader_font::inkAt(font,*glyph,gx,gy);
        assert(bool(actual.pixels[(y+row)*540+x+col])==expected);ink+=expected;
      }assert(ink>4);
    }assert(readings==5&&note==group.noteEnd);
    // An unavailable small-font character must expose the complete canonical
    // group at body size instead of rendering a missing annotation box.
    Fixture unavailable("<html style='writing-mode:vertical-rl'><body><ruby>漢<rt>g&#x85;y</rt></ruby></body></html>");
    assert(unavailable.doc.rubyCount()==1&&!epub_vertical::rubyAvailable(unavailable.doc,unavailable.doc.ruby(0),unsigned(body/2)));
    Renderer fallback;fallback.geometry=shape;assert(epub_vertical::page(unavailable.doc,0,true,shape,fallback,[](){})==unavailable.doc.size());
    size_t covered=0;for(const auto& mark:fallback.marks){assert(std::get<1>(mark)==unsigned(body)&&std::get<5>(mark)==covered);covered=std::get<6>(mark);}assert(covered==unavailable.doc.ruby(0).end);
    // A full-length reading whose final native descender meets the body edge
    // must neither clip that last pixel row nor intrude into the footer.
    Fixture edge("<html style='writing-mode:vertical-rl'><body><ruby>漢<rt>"+std::string(16,'j')+"</rt></ruby></body></html>");
    auto edgeShape=shape;edgeShape.top=edgeShape.bottom-16*int(epub_vertical::rubyAdvance('j',unsigned(body/2)));Renderer edgeInk;edgeInk.geometry=edgeShape;
    assert(epub_vertical::page(edge.doc,0,false,edgeShape,edgeInk,[](){})==edge.doc.size());
    assert(epub_vertical::page(edge.doc,0,true,edgeShape,edgeInk,[](){})==edge.doc.size());
    bool finalRow=false;for(int x=0;x<540;++x)finalRow|=bool(edgeInk.pixels[(edgeShape.bottom-1)*540+x]);assert(finalRow);
    for(size_t i=size_t(edgeShape.bottom)*540;i<edgeInk.pixels.size();++i)assert(!edgeInk.pixels[i]);
    const auto& face=body==24?reader_font::FONT_24:reader_font::FONT_28;
    for(uint32_t cp:{0x9ad9u,0x20bb7u}){assert(reader_font::hasGlyph(face,cp));unsigned ink=0;vertical_font::draw(face,cp,body,0,0,false,[&](int x,int y,int w,int h){assert(x>=0&&y>=0&&x+w<=body&&y+h<=body);ink+=unsigned(w*h);});assert(ink>unsigned(4*(body-4)));}
  }
  // Sideways body j/W/pipe keep every native pixel. Reserve the three-pixel
  // overhang in the column geometry and the wide W in its vertical advance.
  auto normalized=[](const std::set<std::pair<int,int>>& ink){assert(!ink.empty());int left=INT32_MAX,top=INT32_MAX;for(const auto& p:ink){left=std::min(left,p.first);top=std::min(top,p.second);}std::set<std::pair<int,int>> result;for(const auto& p:ink)result.emplace(p.first-left,p.second-top);return result;};
  for(int body:{24,28}){
    Fixture latinBody("<html style='writing-mode:vertical-rl'><body><p>jWgjpqy|</p></body></html>");
    epub_vertical::Geometry shape;shape.pixels=body;Renderer actual;actual.geometry=shape;actual.inspectInk=actual.rejectCollisions=true;
    const size_t stop=epub_vertical::page(latinBody.doc,0,false,shape,actual,[](){});assert(actual.marks.empty());
    assert(epub_vertical::page(latinBody.doc,0,true,shape,actual,[](){})==stop&&stop==latinBody.doc.size());
    const auto& font=body==24?reader_font::FONT_24:reader_font::FONT_28;
    for(size_t at=0;at<8;++at){const uint32_t cp=uint8_t(latinBody.doc.text()[at]);const auto* glyph=reader_font::findGlyph(font,cp);assert(glyph);
      std::set<std::pair<int,int>> rotatedNative;for(int row=0;row<glyph->height;++row)for(int col=0;col<glyph->width;++col)if(reader_font::inkAt(font,*glyph,col,row))rotatedNative.emplace(-row,col);
      assert(normalized(actual.inkBySource.at(at))==normalized(rotatedNative));
    }
    // Exact height exercises j's sideways width and W's >1em vertical extent
    // at the last body row, with no footer ink and equal paint/page boundaries.
    std::string edgeText;for(unsigned i=0;i<16;++i)edgeText+="j|W";
    Fixture edge("<html style='writing-mode:vertical-rl'><body>"+edgeText+"</body></html>");
    Renderer edgeInk;auto edgeShape=shape;edgeShape.top=edgeShape.bottom-16*int(epub_vertical::bodyAdvance('j',body,actual)+epub_vertical::bodyAdvance('|',body,actual)+epub_vertical::bodyAdvance('W',body,actual));edgeInk.geometry=edgeShape;edgeInk.rejectCollisions=true;
    assert(epub_vertical::page(edge.doc,0,false,edgeShape,edgeInk,[](){})==edge.doc.size());
    assert(epub_vertical::page(edge.doc,0,true,edgeShape,edgeInk,[](){})==edge.doc.size());
    bool last=false;for(int x=0;x<540;++x)last|=bool(edgeInk.pixels[(edgeShape.bottom-1)*540+x]);assert(last);for(size_t i=size_t(edgeShape.bottom)*540;i<edgeInk.pixels.size();++i)assert(!edgeInk.pixels[i]);
    // Force three adjacent columns, including the left edge reservation. Ink
    // for body j/W and native ruby may never overlap between any two glyphs.
    std::string mixed;for(unsigned i=0;i<30;++i)mixed+="<ruby>漢字<rt>gjpqy</rt></ruby>j|W";
    Fixture neighbors("<html style='writing-mode:vertical-rl'><body>"+mixed+"</body></html>");
    auto narrow=shape;narrow.right=narrow.left+3*(body+body/2+2+vertical_font::SIDEWAYS_EXTRA);narrow.bottom=160;
    size_t at=0,paintedPages=0;while(at<neighbors.doc.size()){Renderer ink;ink.geometry=narrow;ink.rejectCollisions=true;const size_t measured=epub_vertical::page(neighbors.doc,at,false,narrow,ink,[](){});assert(measured>at&&ink.marks.empty());assert(epub_vertical::page(neighbors.doc,at,true,narrow,ink,[](){})==measured);at=measured;assert(++paintedPages<100);}assert(paintedPages>2);
  }
  // Tight columns exercise legal backtracking, stable page starts and all ink.
  std::string longText;for(unsigned i=0;i<40;++i)longText+=u8"日本語「小さいゃ」、ABC12。<ruby>漢字<rt>かんじ</rt></ruby>";
  Fixture many("<html style='writing-mode:vertical-rl'><body>"+longText+"</body></html>");g.right=150;g.bottom=180;Renderer c;c.geometry=g;
  size_t at=0,pages=0;while(at<many.doc.size()){size_t next=epub_vertical::page(many.doc,at,false,g,c,[](){});assert(next>at);c.marks.clear();assert(epub_vertical::page(many.doc,at,true,g,c,[](){})==next);assert(!many.doc.rubyAt(next)||many.doc.rubyAt(next)->begin==next);at=next;assert(++pages<500);}assert(pages>10);
  Fixture inlineOverride("<html style='writing-mode:vertical-rl'><body style='writing-mode:horizontal-tb'>日本</body></html>");assert(!inlineOverride.doc.hasVertical());
  Fixture nested("<html style='writing-mode:vertical-rl'><body><ruby>漢<ruby>字<rt>じ</rt></ruby><rt>かんじ</rt></ruby></body></html>");assert(nested.doc.rubyCount()==0);
  Fixture trailing("<html style='writing-mode:vertical-rl'><body><ruby>漢<rt>かん</rt>字</ruby></body></html>");assert(trailing.doc.rubyCount()==0);
  Fixture unsupported("<html style='writing-mode:vertical-rl'><body><ruby>漢<rt>かん</rt>字<rt>じ</rt></ruby></body></html>");assert(unsupported.doc.rubyCount()==0); // whole-source readable fallback
  std::puts("Vertical declaration, canonical mappings, PDS2, native alternates, ruby/TCY, parity and rotation passed");
}
