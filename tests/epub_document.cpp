#include "epub_document.h"
#include "prepared_epub.h"
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include <cstdio>
#include <new>

// Fault only the checked semantic array allocation; leave collector, fixture
// and STL allocations intact so the failure is deterministic under sanitizers.
static bool rejectSemanticAllocation=false;
void* operator new[](std::size_t bytes,const std::nothrow_t&) noexcept {
  if(rejectSemanticAllocation)return nullptr;
  try{return ::operator new[](bytes);}catch(...){return nullptr;}
}

namespace {
using Collector = epub::structure::Collector;
using Kind = epub::structure::Kind;
constexpr uint32_t NONE = epub::structure::NONE;
void put(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
  for (unsigned i=0;i<4;++i) bytes[at+i]=uint8_t(value>>(8*i));
}
Collector& populate(Collector& c, const std::string& text) {
  c.begin();
  const uint8_t us=c.internLanguage("en-US"), gb=c.internLanguage("en-GB"), ja=c.internLanguage("ja");
  uint32_t heading=c.open(Kind::Heading,0,NONE,NONE,0,0,us,2);
  c.text(0,5,heading,NONE,epub::structure::Strong,1,us);c.close(heading,5);
  const size_t body=text.find("Body"), emphasis=text.find("emphasis"), end=text.find(".\n",body)+1;
  uint32_t paragraph=c.open(Kind::Paragraph,body,NONE,NONE,0,0,us);
  c.text(body,emphasis,paragraph,NONE,0,1,us);
  c.text(emphasis,end,paragraph,NONE,epub::structure::Emphasis,1,gb);c.close(paragraph,end);
  const size_t quote=text.find("Quote"), quoteEnd=quote+6;
  uint32_t outer=c.open(Kind::Quote,quote,NONE,NONE,0,0,us);
  uint32_t inner=c.open(Kind::Quote,quote,outer,NONE,0,1,us);
  c.text(quote,quoteEnd,inner,NONE,0,2,us);c.close(inner,quoteEnd);c.close(outer,quoteEnd);
  const size_t code=text.find("code");
  uint32_t pre=c.open(Kind::Preformatted,code,NONE,NONE,0,0,us);
  c.text(code,code+4,pre,NONE,0,1,us);c.close(pre,code+4);
  c.text(code+5,text.size(),NONE,NONE,0,0,ja);
  c.finish(text.data(),text.size());assert(c.ready());return c;
}
}
int main() {
  static_assert(prepared_epub::MAX_METADATA_BYTES == epub_document::Document::MAX_BYTES,"interchange semantic limit must match decoder");
  const std::string text=u8"Title\n\nBody emphasis.\n\nQuote.\n\ncode\n日本語";
  const auto untouched=text;
  Collector collector;populate(collector,text);
  epub_document::Document doc;assert(!doc.ready()&&doc.runCount()==0);
  assert(doc.assign(collector,text.data(),text.size())&&doc.ready());
  assert(doc.text()==text.data()&&doc.source()==text.data()&&doc.size()==text.size());
  assert(text==untouched); // semantic conversion cannot insert/change source bytes
  uint32_t cursor=0;
  for(size_t i=0;i<doc.runCount();++i){const auto& r=doc.run(i);
    assert(r.begin==cursor&&r.end>r.begin&&r.sourceBegin==r.begin&&r.sourceEnd==r.end);
    assert(doc.runAt(r.begin)==i);cursor=r.end;
  }
  assert(cursor==text.size()&&doc.runAt(text.size())==doc.runCount());
  const auto& heading=doc.run(doc.runAt(0));
  assert((heading.flags&markdown::Heading)&&(heading.flags&markdown::Strong)&&heading.headingLevel==2);
  const auto& emphasis=doc.run(doc.runAt(text.find("emphasis")));
  assert((emphasis.flags&markdown::Emphasis)&&emphasis.language==text_language::Language::EnglishGB);
  const auto& quote=doc.run(doc.runAt(text.find("Quote")));
  assert((quote.flags&markdown::Quote)&&quote.indent==2);
  const auto& code=doc.run(doc.runAt(text.find("code")));
  assert((code.flags&markdown::Code)&&(code.flags&markdown::Preformatted));
  assert(doc.run(doc.runAt(text.find(u8"日"))).language==text_language::Language::None);
  for(size_t at=0;at<=text.size()+2;++at){assert(doc.sourceOffset(at)==std::min(at,text.size()));assert(doc.renderOffset(at)==std::min(at,text.size()));}
  std::vector<uint8_t> bytes;assert(doc.encode(bytes)&&bytes.size()==doc.encodedSize());
  std::vector<uint8_t> direct(bytes.size());assert(doc.encodeTo(direct.data(),direct.size())&&direct==bytes);
  assert(!doc.encodeTo(direct.data(),direct.size()-1)&&!doc.encodeTo(nullptr,direct.size()));
  epub_document::Document decoded;assert(decoded.decode(bytes.data(),bytes.size(),text.data(),text.size()));
  assert(decoded.runCount()==doc.runCount());
  for(size_t i=0;i<doc.runCount();++i){const auto&a=doc.run(i);const auto&b=decoded.run(i);
    assert(a.begin==b.begin&&a.end==b.end&&a.flags==b.flags&&a.indent==b.indent&&a.headingLevel==b.headingLevel&&a.language==b.language);
  }
  auto reject=[&](std::vector<uint8_t> broken){
    assert(decoded.decode(bytes.data(),bytes.size(),text.data(),text.size()));
    assert(!decoded.decode(broken.data(),broken.size(),text.data(),text.size()));
    assert(!decoded.ready()&&decoded.runCount()==0&&decoded.text()==nullptr&&decoded.size()==0);
    assert(decoded.error()==epub_document::Document::Error::Invalid&&!decoded.allocationFailed());
  };
  auto bad=bytes;bad[0]='X';reject(bad);
  bad=bytes;put(bad,4,2);reject(bad);
  bad=bytes;put(bad,8,0);reject(bad);
  bad=bytes;put(bad,8,epub_document::Document::MAX_RUNS+1);reject(bad);
  bad=bytes;put(bad,12,uint32_t(text.size()+1));reject(bad);
  bad=bytes;bad.pop_back();reject(bad);bad=bytes;bad.push_back(0);reject(bad);
  bad=bytes;put(bad,16,1);reject(bad);
  bad=bytes;put(bad,20,0);reject(bad);
  bad=bytes;put(bad,20,uint32_t(text.size()+1));reject(bad);
  bad=bytes;bad[25]|=0x80;reject(bad);
  bad=bytes;bad[26]=9;reject(bad);
  bad=bytes;bad[27]=0;reject(bad); // heading flag requires nonzero level
  bad=bytes;bad[27]=7;reject(bad);
  bad=bytes;bad[28]=3;reject(bad);
  for(size_t at:{29u,30u,31u}){bad=bytes;bad[at]=1;reject(bad);}
  // A valid byte envelope cannot split a UTF-8 sequence at a run boundary.
  const std::string multibyte=u8"日a";
  std::vector<uint8_t> split(48);memcpy(split.data(),"PDS1",4);put(split,4,1);put(split,8,2);put(split,12,multibyte.size());
  put(split,16,0);put(split,20,1);put(split,32,1);put(split,36,multibyte.size());
  assert(!decoded.decode(split.data(),split.size(),multibyte.data(),multibyte.size()));
  put(split,20,3);put(split,32,3);assert(decoded.decode(split.data(),split.size(),multibyte.data(),multibyte.size()));
  const std::vector<std::string> invalid={std::string("x\0y",3),std::string("\xc0\x80",2),std::string("\xed\xa0\x80",3),std::string("\xf4\x90\x80\x80",4),std::string("\xe4\xb8",2)};
  for(const auto& s:invalid)assert(!epub_document::Document::validText(s.data(),s.size()));
  assert(!epub_document::Document::validText(nullptr,1)&&!epub_document::Document::validText("",0));
  assert(epub_document::Document::validText(u8"日本語 é 😀",std::strlen(u8"日本語 é 😀")));
  rejectSemanticAllocation=true;
  assert(!doc.assign(collector,text.data(),text.size())&&!doc.ready()&&doc.allocationFailed());
  assert(!decoded.decode(bytes.data(),bytes.size(),text.data(),text.size())&&!decoded.ready()&&decoded.allocationFailed());
  rejectSemanticAllocation=false;
  assert(doc.assign(collector,text.data(),text.size())&&doc.error()==epub_document::Document::Error::None);
  assert(decoded.decode(bytes.data(),bytes.size(),text.data(),text.size())&&!decoded.allocationFailed());
  epub::structure::Allocator failing;
  failing.allocate=[](size_t,void*)->void*{return nullptr;};failing.release=[](void*,void*){};
  Collector allocation(epub::structure::Limits(),failing);allocation.begin();
  assert(!doc.assign(allocation,text.data(),text.size())&&doc.allocationFailed());
  epub::structure::Limits limits;limits.totalBytes=1;Collector budget(limits);budget.begin();
  assert(!doc.assign(budget,text.data(),text.size())&&doc.error()==epub_document::Document::Error::Budget&&!doc.allocationFailed());
  Collector incomplete;assert(!doc.assign(incomplete,text.data(),text.size())&&!doc.ready());
  assert(doc.error()==epub_document::Document::Error::Unavailable&&!doc.allocationFailed());
  std::vector<uint8_t> discarded{1,2,3};assert(!doc.encode(discarded)&&discarded.empty());
  doc.clear();assert(!doc.ready()&&doc.runAt(0)==0&&doc.error()==epub_document::Document::Error::None);
  std::puts("EPUB semantic source identity, ranges, language and malformed wire checks passed");
}
