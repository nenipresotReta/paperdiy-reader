#include "epub_core.h"
#include "epub_reader.h"
#include "markdown_reader.h"
#include "reading_font.h"
#include "reader_font.h"
#include "native_font_data.h"
#include "content_source.h"
#include "reader_typography.h"
#include "math_layout.h"
#include "math_font.h"
#include <tuple>
#include <set>
#include <M5Unified.h>
#include <cassert>
#include <map>
#include <string>
#include <vector>
#include <cstdio>
#include <sys/mman.h>
#include <unistd.h>
#include <new>
static int objectFailAfter=-1;static size_t objectArrayCalls=0;
void* operator new[](std::size_t bytes,const std::nothrow_t&) noexcept{++objectArrayCalls;if(objectFailAfter==0)return nullptr;if(objectFailAfter>0)--objectFailAfter;try{return ::operator new[](bytes);}catch(...){return nullptr;}}
namespace reader_typography {const Style style;}
const reader_font::Font& bodyNativeFont(){return reader_settings::geometry().pixels==24?reader_font::FONT_24:reader_font::FONT_28;}
const lgfx::IFont* bodyFont(){static lgfx::IFont font;return &font;}
const lgfx::IFont* readerFont(){return bodyFont();}
int bodyGlyphAdvance(uint32_t cp){return reader_font::glyphAdvance(bodyNativeFont(),cp);}
const reader_font::Glyph* bodyGlyph(uint32_t cp){return reader_font::findGlyph(bodyNativeFont(),cp);}
bool bodyFontHasGlyph(uint32_t cp){return reader_font::hasGlyph(bodyNativeFont(),cp);}
int readerGlyphWidth(uint32_t cp){return reader_font::glyphAdvance(reader_font::FONT_24,cp);}
bool readerFontHasGlyph(uint32_t cp){return reader_font::hasGlyph(reader_font::FONT_24,cp);}
uint32_t readerCodepoint(const char* text,size_t bytes){return reader_font::decodeCodepoint(text,bytes);}
static size_t bodyDrawCalls=0;static bool drawingBody=false;
static const char* tracedSource=nullptr;static size_t tracedBytes=0;static std::map<size_t,unsigned> bodySourceCounts;
using Primitive=std::tuple<int,int,int,int>;static std::vector<Primitive> mathPrimitives;
static void observe(int x,int y,int w,int h,uint32_t color){if(!drawingBody&&color==TFT_BLACK)mathPrimitives.emplace_back(x,y,w,h);}

void drawBodyGlyph(const char* text,size_t bytes,int x,int y,uint16_t color){++bodyDrawCalls;const uintptr_t at=reinterpret_cast<uintptr_t>(text),base=reinterpret_cast<uintptr_t>(tracedSource);if(tracedSource&&at>=base&&at<base+tracedBytes)++bodySourceCounts[size_t(at-base)];drawingBody=true;reader_font::drawGlyphRuns(bodyNativeFont(),readerCodepoint(text,bytes),x,y,[&](int a,int b,int w){M5.Display.drawFastHLine(a,b,w,color);});drawingBody=false;}
void drawReaderGlyph(const char* text,size_t bytes,int x,int y,uint16_t color){reader_font::drawGlyphRuns(reader_font::FONT_24,readerCodepoint(text,bytes),x,y,[&](int a,int b,int w){M5.Display.drawFastHLine(a,b,w,color);});}
namespace {
struct Fixture {
  std::map<std::string,std::string> files;std::vector<char> source;epub::structure::Collector collector;epub_document::Document doc;
  explicit Fixture(const std::string& body){files["META-INF/container.xml"]="<container><rootfiles><rootfile full-path='book.opf'/></rootfiles></container>";files["book.opf"]="<package><manifest><item id='c' href='c.xhtml'/></manifest><spine><itemref idref='c'/></spine></package>";files["c.xhtml"]="<html><body>"+body+"</body></html>";source.resize(131072);
    epub::Importer importer;importer.output=source.data();importer.capacity=source.size()-1;importer.structure=&collector;importer.alloc=[](size_t n){return std::malloc(n);};importer.resourceSize=[&](const std::string& p){auto i=files.find(p);return i==files.end()?SIZE_MAX:i->second.size();};importer.resourceRead=[&](const std::string& p,void* out,size_t bytes){auto i=files.find(p);if(i==files.end()||i->second.size()!=bytes)return false;std::memcpy(out,i->second.data(),bytes);return true;};assert(importer.run());source.resize(importer.used+1);assert(doc.assign(collector,source.data(),importer.used));
    std::vector<char> flat(source.size());importer.structure=nullptr;importer.used=0;importer.output=flat.data();assert(importer.run());assert(importer.used==doc.size()&&!std::memcmp(flat.data(),source.data(),doc.size()));
  }
};
std::vector<Primitive> nativeFormula(const std::string& source,bool display){math_layout::Options options;options.displayStyle=display;options.maxHeight=840;auto result=math_layout::layout(source.data(),source.size(),options);assert(result.ok());std::vector<Primitive> output;for(const auto& command:result.commands){if(command.kind==math_layout::CommandKind::Rule)output.emplace_back(command.x,command.y,command.width,command.height);else math_font::drawGlyphRuns(command.codepoint,command.pixels,command.x,command.y,[&](int x,int y,int width){output.emplace_back(x,y,width,1);});}return output;}
std::vector<Primitive> normalized(std::vector<Primitive> input){assert(!input.empty());int x=INT32_MAX,y=INT32_MAX;for(const auto& p:input){x=std::min(x,std::get<0>(p));y=std::min(y,std::get<1>(p));}for(auto& p:input){std::get<0>(p)-=x;std::get<1>(p)-=y;}return input;}
void writePage(const char* prefix,const char* suffix){if(!prefix)return;const std::string path=std::string(prefix)+suffix;FILE* file=std::fopen(path.c_str(),"wb");assert(file);std::fputs("P5\n540 960\n255\n",file);assert(std::fwrite(M5.Display.fb.data(),1,M5.Display.fb.size(),file)==M5.Display.fb.size());assert(!std::fclose(file));}
void put(std::vector<uint8_t>& data,size_t at,uint32_t value){for(unsigned i=0;i<4;++i)data[at+i]=uint8_t(value>>(8*i));}
void activate(Fixture& f){bookDocument.clear();std::vector<uint8_t> bytes;assert(f.doc.encode(bytes));assert(bookDocument.decode(bytes.data(),bytes.size(),f.source.data(),f.doc.size()));bookEpubTransientFallback=false;prepareEpubMetrics();}
size_t render(Fixture& f){activate(f);size_t at=0,pages=0;const size_t allocationCalls=objectArrayCalls;const int previousFailure=objectFailAfter;objectFailAfter=0;while(at<f.doc.size()){
    const size_t measured=layoutEpub(at,false);M5.Display.fillScreen(TFT_WHITE);const size_t drawn=layoutEpub(at,true);assert(drawn==measured&&drawn>at&&drawn<=f.doc.size());
    for(int y=916;y<960;++y)for(int x=0;x<540;++x)assert(M5.Display.fb[y*540+x]==uint8_t(TFT_WHITE));
    assert(!bookDocument.rubyAt(drawn)||bookDocument.rubyAt(drawn)->begin==drawn);
    const size_t math=bookDocument.mathAt(drawn);assert(math==bookDocument.mathCount()||bookDocument.math(math).begin==drawn);at=drawn;assert(++pages<500);
  }assert(objectArrayCalls==allocationCalls);objectFailAfter=previousFailure;bookDocument.clear();return pages;
}
}
int main(int argc,char** argv){
  const std::string math="<math display='block'><mfrac><msup><mi>x</mi><mn>2</mn></msup><msqrt><mi>y</mi></msqrt></mfrac></math>";
  Fixture formulaOnly(math);activate(formulaOnly);bodyDrawCalls=0;M5.Display.fillScreen(TFT_WHITE);assert(layoutEpub(0,true)==formulaOnly.doc.size());assert(bodyDrawCalls==0);int firstInk=960,lastInk=0;for(int y=0;y<916;++y)for(int x=0;x<540;++x)if(M5.Display.fb[y*540+x]==0){firstInk=std::min(firstInk,y);lastInk=std::max(lastInk,y);}assert(lastInk-firstInk>35);bookDocument.clear();
  // Production draw primitive counts must equal exactly one native formula,
  // even when canonical MathML contains multiple ASCII/UTF-8 source scalars.
  for(bool display:{false,true}){Fixture exact(std::string("<p>ab<math")+(display?" display='block'":"")+"><mfrac><msup><mi>x</mi><mn>2</mn></msup><msqrt><mi>y</mi></msqrt></mfrac></math>cd</p>");activate(exact);mathPrimitives.clear();M5.Display.fillScreen(TFT_WHITE);M5.Display.observe=observe;assert(layoutEpub(0,true)==exact.doc.size());M5.Display.observe=nullptr;assert(normalized(mathPrimitives)==normalized(nativeFormula("\\frac{{x}^{2}}{\\sqrt{y}}",display)));}
  const std::string nested="<math display='block'><msqrt><msqrt><msqrt><msqrt><mi>x</mi></msqrt></msqrt></msqrt></msqrt></math>";
  Fixture displays(math+nested);activate(displays);mathPrimitives.clear();M5.Display.fillScreen(TFT_WHITE);M5.Display.observe=observe;assert(layoutEpub(0,true)==displays.doc.size());M5.Display.observe=nullptr;
  const auto first=nativeFormula("\\frac{{x}^{2}}{\\sqrt{y}}",true),second=nativeFormula("\\sqrt{\\sqrt{\\sqrt{\\sqrt{x}}}}",true);assert(mathPrimitives.size()==first.size()+second.size());
  std::vector<Primitive> firstActual(mathPrimitives.begin(),mathPrimitives.begin()+first.size()),secondActual(mathPrimitives.begin()+first.size(),mathPrimitives.end());assert(normalized(firstActual)==normalized(first)&&normalized(secondActual)==normalized(second));
  int bottom=0,top=960;for(const auto& p:firstActual)bottom=std::max(bottom,std::get<1>(p)+std::get<3>(p));for(const auto& p:secondActual)top=std::min(top,std::get<1>(p));assert(top>bottom);writePage(argc==2?argv[1]:nullptr,"-math.pgm");
  // Match the real acceptance fixture and require every table page to paint.
  std::string actualRows="<table><tr><th>Item</th><th>Value</th></tr>";for(unsigned i=0;i<50;++i)actualRows+="<tr><td>Row "+std::to_string(i+1)+" 日本語</td><td>Bounded native text</td></tr>";actualRows+="</table>";
  Fixture device("<p>MathML and tables</p>"+math+nested+actualRows);activate(device);bodySourceCounts.clear();tracedSource=device.doc.text();tracedBytes=device.doc.size();M5.Display.fillScreen(TFT_WHITE);const size_t firstPage=layoutEpub(0,true);assert(firstPage>device.doc.table(0).begin&&firstPage<device.doc.table(0).end);writePage(argc==2?argv[1]:nullptr,"-device-first.pgm");
  size_t tableAt=firstPage,tablePages=0;
  while(tableAt<device.doc.size()){const size_t measured=layoutEpub(tableAt,false);M5.Display.fillScreen(TFT_WHITE);const size_t drawn=layoutEpub(tableAt,true);assert(drawn==measured&&drawn>tableAt);size_t ink=0;for(int y=24;y<916;++y)for(int x=0;x<540;++x)ink+=M5.Display.fb[y*540+x]==0;assert(ink>100);if(!tablePages)writePage(argc==2?argv[1]:nullptr,"-table.pgm");tableAt=drawn;assert(++tablePages<20);}assert(tablePages>1);
  for(size_t r=device.doc.table(0).firstRow;r<device.doc.table(0).firstRow+device.doc.table(0).rows;++r){const auto& row=device.doc.row(r);for(size_t c=row.firstCell;c<row.firstCell+row.cells;++c){const auto& cell=device.doc.cell(c);for(size_t at=cell.begin;at<cell.end;){const size_t bytes=pagination::utf8Bytes(device.doc.text()+at,cell.end-at);const uint32_t cp=pagination::codepoint(device.doc.text()+at,bytes);if(!pagination::space(cp)&&!pagination::invisible(cp)&&cp!='\n')assert(bodySourceCounts[at]==unsigned(row.flags&1?2:1));at+=bytes;}}}tracedSource=nullptr;bookDocument.clear();
  Fixture one("<p>Before日本 "+math+" after.</p><table><tr><th>Name</th><th>Value</th></tr><tr><td>alpha β</td><td>10</td></tr><tr><td>日本語</td><td>A&#160;B</td></tr></table><p>End.</p>");
  assert(one.doc.mathCount()==1&&one.doc.tableCount()==1);const auto& m=one.doc.math(0);assert(std::string(one.doc.text()+m.begin,m.end-m.begin)=="x2y");assert(std::string(one.doc.mathText(0),m.texBytes)=="\\frac{{x}^{2}}{\\sqrt{y}}");
  const auto& t=one.doc.table(0);assert(t.rows==3&&t.columns==2);assert(one.doc.row(t.firstRow).flags==1);
  std::vector<uint8_t> encoded;assert(one.doc.encode(encoded)&&!std::memcmp(encoded.data(),"PDS3",4));epub_document::Document decoded;assert(decoded.decode(encoded.data(),encoded.size(),one.doc.text(),one.doc.size()));assert(decoded.mathCount()==1&&decoded.tableCount()==1&&decoded.validateSpan(0,decoded.size()));
  for(int fail=0;fail<7;++fail){objectFailAfter=fail;assert(!decoded.decode(encoded.data(),encoded.size(),one.doc.text(),one.doc.size()));assert(decoded.allocationFailed()&&!decoded.ready());}objectFailAfter=-1;
  auto reject=[&](std::vector<uint8_t> data){assert(!decoded.decode(data.data(),data.size(),one.doc.text(),one.doc.size()));assert(!decoded.ready());};
  auto bad=encoded;bad.pop_back();reject(bad);bad=encoded;bad.push_back(0);reject(bad);bad=encoded;put(bad,20,257);reject(bad);bad=encoded;put(bad,28,257);reject(bad);bad=encoded;put(bad,36,32769);reject(bad);
  const size_t objects=40+one.doc.runCount()*16+one.doc.rubyCount()*20;bad=encoded;put(bad,objects+8,1);reject(bad);bad=encoded;put(bad,objects+16+8,1);reject(bad);bad=encoded;bad.back()=0;reject(bad);
  // Numeric-only metadata admission cannot touch a single source byte.
  const long systemPage=sysconf(_SC_PAGESIZE);void* inaccessible=mmap(nullptr,size_t(systemPage),PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);assert(inaccessible!=MAP_FAILED);
  assert(decoded.decode(encoded.data(),encoded.size(),static_cast<const char*>(inaccessible),one.doc.size(),false));assert(decoded.validateSpan(0,0));assert(!munmap(inaccessible,size_t(systemPage)));
  assert(decoded.decode(encoded.data(),encoded.size(),one.doc.text(),one.doc.size()));assert(!decoded.validateSpan(m.begin,m.end));assert(decoded.validateSpan(m.begin,m.end+1));
  activate(one);M5.Display.fillScreen(TFT_WHITE);assert(layoutEpub(t.begin,true)>t.begin);unsigned border=0;for(int y=24;y<916;++y)border+=M5.Display.fb[y*540+reader_settings::geometry().left]==0;assert(border>60);bookDocument.clear();
  for(unsigned preset=0;preset<3;++preset){reader_settings::current().preset=preset;assert(render(one)==1);}
  activate(one);clearMarkdown();objectFailAfter=0;prepareEpubMetrics();objectFailAfter=-1;assert(bookEpubTransientFallback);size_t fallbackAt=0;while(fallbackAt<one.doc.size()){const size_t next=layoutEpub(fallbackAt,false);assert(next>fallbackAt);fallbackAt=next;}bookDocument.clear();
  std::string repeated;for(unsigned i=0;i<20;++i)repeated+="<p>Equation "+std::to_string(i)+math+" complete.</p>";Fixture many(repeated);assert(many.doc.mathCount()==20&&render(many)>1);
  std::string rows="<table>";for(unsigned i=0;i<70;++i)rows+="<tr><td>row "+std::to_string(i)+" 日本語</td><td>Value plus wrapping English words.</td></tr>";rows+="</table>";Fixture table(rows);assert(table.doc.tableCount()==1&&render(table)>3);
  Fixture tall("<table><tr><td>"+std::string(3000,'x')+"</td><td>complete</td></tr><tr><td>tail</td><td>safe</td></tr></table>");assert(tall.doc.tableCount()==1&&render(tall)>2);
  Fixture unsupported("<math><menclose><mi>UNSUPPORTED</mi></menclose></math><table><tr><td colspan='2'>Keep全部</td><td>text</td></tr></table>");assert(unsupported.doc.mathCount()==0&&unsupported.doc.tableCount()==0);assert(std::string(unsupported.doc.text()).find(u8"Keep全部text")!=std::string::npos);assert(render(unsupported));
  reader_settings::current().preset=1;std::puts("EPUB MathML/table canonical offsets, PDS3/torn metadata, no-read decode, native parity and footer bounds passed");
}
