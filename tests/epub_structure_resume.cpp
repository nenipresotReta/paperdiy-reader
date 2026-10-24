#include "epub_core.h"
#include "epub_document.h"
#include <cassert>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

using Collector = epub::structure::Collector;
using Status = epub::structure::Status;
using Progress = epub::Importer::Progress;
struct Chapter {std::string html; bool auxiliary;Chapter(const std::string& value,bool aux=false):html(value),auxiliary(aux){}};
struct Fixture {
  std::map<std::string,std::string> files;
  Fixture(const std::vector<Chapter>& chapters,const std::string& title="<title>Title é</title>") {
    files["META-INF/container.xml"]="<container><rootfiles><rootfile full-path='O/package.opf'/></rootfiles></container>";
    std::string manifest,spine;
    for(size_t i=0;i<chapters.size();++i){const auto id=std::to_string(i);files["O/c"+id+".xhtml"]=chapters[i].html;
      manifest+="<item id='c"+id+"' href='c"+id+".xhtml'/>";
      spine+="<itemref idref='c"+id+"'"+(chapters[i].auxiliary?std::string(" linear='no'"):std::string())+"/>";
    }
    files["O/package.opf"]="<package><metadata><language>en-US</language>"+title+"</metadata><manifest>"+manifest+"</manifest><spine>"+spine+"</spine></package>";
    files["O/jp.css"]=".v{writing-mode:vertical-rl}.tcy{text-combine-upright:all}";
  }
};
static std::string html(const std::string& body,const std::string& head="",const std::string& attributes="") {
  return "<html "+attributes+"><head>"+head+"</head><body>"+body+"</body></html>";
}
struct Session {
  std::vector<char> text=std::vector<char>(4*1024*1024+1);
  Collector collector;
  epub::Importer importer;
  std::map<std::string,size_t> reads;
  Session(const Fixture& fixture,const epub::structure::Limits& limits=epub::structure::Limits(),
          const epub::structure::Allocator& allocator=epub::structure::Allocator()):collector(limits,allocator) {
    importer.output=text.data();importer.capacity=text.size()-1;importer.structure=&collector;
    importer.alloc=[](size_t bytes)->void*{return std::malloc(bytes);};
    importer.resourceSize=[&fixture](const std::string& path){auto i=fixture.files.find(path);return i==fixture.files.end()?SIZE_MAX:i->second.size();};
    importer.resourceRead=[this,&fixture](const std::string& path,void* out,size_t size){auto i=fixture.files.find(path);if(i==fixture.files.end()||i->second.size()!=size)return false;std::memcpy(out,i->second.data(),size);++reads[path];return true;};
  }
};
struct Result {std::string text;std::vector<uint8_t> pds;Status status;bool unsafe,english;uint8_t language;};
static Result result(Session& session) {
  assert(session.importer.progress==Progress::Complete);
  Result r;r.text.assign(session.text.data(),session.importer.used);r.status=session.collector.status();
  r.unsafe=session.importer.unsafeFlatLanguage;r.english=session.importer.englishHyphenation;r.language=uint8_t(session.importer.language);
  epub_document::Document doc;if(session.collector.ready()){assert(doc.assign(session.collector,session.text.data(),session.importer.used));assert(doc.encode(r.pds));}
  return r;
}
static void equal(const Result& a,const Result& b){assert(a.text==b.text&&a.pds==b.pds&&a.status==b.status&&a.unsafe==b.unsafe&&a.english==b.english&&a.language==b.language);}
struct Saved {std::vector<uint8_t> wire;std::string text;size_t cursor;bool body,unsafe,complete;uint8_t language,collectorLanguage;Status status;};
static Saved save(Session& s) {
  Saved v;v.text.assign(s.text.data(),s.importer.used);v.cursor=s.importer.chapterCursor;v.body=s.importer.hasBodyText;
  v.unsafe=s.importer.unsafeFlatLanguage;v.complete=s.importer.progress==Progress::Complete;
  v.language=uint8_t(s.importer.language);v.collectorLanguage=s.importer.structureLanguage;v.status=s.collector.status();
  v.wire.resize(s.collector.snapshotBytes());assert(!v.wire.empty());
  assert(s.collector.encodeSnapshot(v.wire.data(),v.wire.size(),v.text.data(),v.text.size()));return v;
}
static void restore(Session& s,const Saved& v) {
  assert(s.importer.begin());std::memcpy(s.text.data(),v.text.data(),v.text.size());
  assert(s.collector.restoreSnapshot(v.wire.data(),v.wire.size(),s.text.data(),v.text.size()));assert(s.collector.status()==v.status);
  std::vector<uint8_t> roundtrip(s.collector.snapshotBytes());
  assert(s.collector.encodeSnapshot(roundtrip.data(),roundtrip.size(),s.text.data(),v.text.size())&&roundtrip==v.wire);
  assert(s.importer.restoreProgress(v.cursor,v.text.size(),v.body,v.unsafe,v.language,v.collectorLanguage,v.complete));
}
static void finish(Session& s) {
  while(s.importer.progress==Progress::Pending){const std::string prefix(s.text.data(),s.importer.used);
    assert(s.importer.stepChapter()!=Progress::Failed);
    assert(s.importer.used>=prefix.size()&&!std::memcmp(s.text.data(),prefix.data(),prefix.size()));
  }
}
static Result parity(const Fixture& fixture,const char* magic,
                     const epub::structure::Limits& limits=epub::structure::Limits(),
                     const epub::structure::Allocator& allocator=epub::structure::Allocator()) {
  Session baseline(fixture,limits,allocator);assert(baseline.importer.run());const auto wanted=result(baseline);
  if(magic)assert(wanted.pds.size()>=4&&!std::memcmp(wanted.pds.data(),magic,4));else assert(wanted.pds.empty());
  const size_t chapters=baseline.importer.chapters.size();
  // Restart after each possible chapter, including the completed extraction.
  for(size_t stop=1;stop<=chapters;++stop){Session before(fixture,limits,allocator);assert(before.importer.begin());
    for(size_t i=0;i<stop;++i)assert(before.importer.stepChapter()!=Progress::Failed);
    const auto saved=save(before);Session after(fixture,limits,allocator);restore(after,saved);
    for(size_t i=0;i<stop;++i)assert(!after.reads.count(after.importer.chapters[i].path));
    finish(after);equal(wanted,result(after));
  }
  // Reboot repeatedly: every chapter is reached through a restored collector.
  Saved saved;bool first=true;
  for(size_t i=0;i<chapters;++i){Session current(fixture,limits,allocator);if(first){assert(current.importer.begin());first=false;}else restore(current,saved);
    assert(current.importer.stepChapter()!=Progress::Failed);saved=save(current);
    if(i+1==chapters)equal(wanted,result(current));
  }
  return wanted;
}
static uint32_t get(const uint8_t* p){return epub::u32(p);}
static void put(uint8_t* p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(i*8));}
static void corruptions(const Fixture& fixture) {
  Session s(fixture);assert(s.importer.begin());assert(s.importer.stepChapter()==Progress::Pending);const auto saved=save(s);
  for(size_t bytes=0;bytes<saved.wire.size();++bytes){Collector c;assert(!c.restoreSnapshot(saved.wire.data(),bytes,saved.text.data(),saved.text.size()));assert(!c.ready()&&!c.records());}
  auto rejects=[&](std::vector<uint8_t> wire){Collector c;assert(!c.restoreSnapshot(wire.data(),wire.size(),saved.text.data(),saved.text.size()));assert(!c.ready()&&!c.records()&&!c.resourceCount());};
  for(size_t offset:{size_t(0),size_t(4),size_t(6),size_t(8),size_t(12),size_t(16),size_t(17),size_t(18),size_t(44),size_t(48)}){auto wire=saved.wire;wire[offset]^=0x80;rejects(wire);}
  {auto wire=saved.wire;put(wire.data()+20,UINT32_MAX);rejects(wire);}
  {auto wire=saved.wire;put(wire.data()+40,UINT32_MAX-1);rejects(wire);}
  const size_t records=Collector::SNAPSHOT_HEADER_BYTES+saved.wire[17]*36;
  {auto wire=saved.wire;put(wire.data()+records+8,0);rejects(wire);} // parent cannot point to itself
  {auto wire=saved.wire;put(wire.data()+records+12,UINT32_MAX-1);rejects(wire);}
  {auto wire=saved.wire;wire[records+18]=255;rejects(wire);}
  {auto wire=saved.wire;wire[records+21]=255;rejects(wire);}
  {auto wire=saved.wire;wire[records+22]=1;rejects(wire);}
  {auto wire=saved.wire;wire[Collector::SNAPSHOT_HEADER_BYTES]=1;rejects(wire);} // language0 must be empty
  {auto wire=saved.wire;wire[Collector::SNAPSHOT_HEADER_BYTES+1]=1;rejects(wire);} // zero padding
  {auto wire=saved.wire;const auto accent=saved.text.find("é");assert(accent!=std::string::npos);put(wire.data()+records+4,uint32_t(accent+1));rejects(wire);}
  const size_t resources=records+get(saved.wire.data()+20)*24;
  {auto wire=saved.wire;put(wire.data()+resources+8,UINT32_MAX);rejects(wire);}
  {auto wire=saved.wire;wire[resources+14]=2;rejects(wire);}
  const size_t maths=resources+get(saved.wire.data()+24)*16;
  assert(get(saved.wire.data()+32));
  {auto wire=saved.wire;put(wire.data()+maths+8,1);rejects(wire);}
  {auto wire=saved.wire;wire[maths+14]=0x80;rejects(wire);}
  const size_t paths=maths+get(saved.wire.data()+32)*16;
  {auto wire=saved.wire;std::memset(wire.data()+paths,'x',get(saved.wire.data()+28));rejects(wire);}
  {auto wire=saved.wire;wire[paths+get(saved.wire.data()+28)]=0x80;rejects(wire);}
  {auto wire=saved.wire;wire.push_back(0);rejects(wire);}
  {auto broken=saved.text;broken[0]=char(0x80);Collector c;assert(!c.restoreSnapshot(saved.wire.data(),saved.wire.size(),broken.data(),broken.size()));}
  {Collector c;assert(!c.restoreSnapshot(saved.wire.data(),saved.wire.size(),saved.text.data(),saved.text.size()-1));}
  // An offset equal to the half-open prefix end must not inspect text[end].
  const size_t page=size_t(sysconf(_SC_PAGESIZE));assert(saved.text.size()<page);
  void* mapping=mmap(nullptr,page*2,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);assert(mapping!=MAP_FAILED);
  char* guarded=static_cast<char*>(mapping)+page-saved.text.size();std::memcpy(guarded,saved.text.data(),saved.text.size());
  assert(mprotect(static_cast<char*>(mapping)+page,page,PROT_NONE)==0);
  {Collector c;assert(c.restoreSnapshot(saved.wire.data(),saved.wire.size(),guarded,saved.text.size()));}
  assert(munmap(mapping,page*2)==0);
  // The full source/prefix digest belongs to the outer transaction; ECS only
  // checks content-dependent bounds and UTF-8, never claims authentication.
  {epub::structure::Limits other;--other.records;Collector c(other);assert(!c.restoreSnapshot(saved.wire.data(),saved.wire.size(),saved.text.data(),saved.text.size()));}
  {auto small=saved.wire;assert(!s.collector.encodeSnapshot(small.data(),small.size()-1,saved.text.data(),saved.text.size()));}
  {auto noBoundary=saved.text;noBoundary.back()='x';auto wire=saved.wire;assert(!s.collector.encodeSnapshot(wire.data(),wire.size(),noBoundary.data(),noBoundary.size()));}
  struct Allocation {bool deny=false;size_t allocations=0,releases=0;} allocation;
  epub::structure::Allocator allocator;allocator.context=&allocation;
  allocator.allocate=[](size_t n,void* p)->void*{auto& a=*static_cast<Allocation*>(p);++a.allocations;return a.deny?nullptr:std::malloc(n);};
  allocator.release=[](void* p,void* c){++static_cast<Allocation*>(c)->releases;std::free(p);};
  {Collector c(epub::structure::Limits(),allocator);allocation.deny=true;assert(!c.restoreSnapshot(saved.wire.data(),saved.wire.size(),saved.text.data(),saved.text.size()));assert(c.status()==Status::Allocation);
    allocation.deny=false;assert(c.restoreSnapshot(saved.wire.data(),saved.wire.size(),saved.text.data(),saved.text.size()));}
  assert(allocation.allocations==allocation.releases+1); // one denied request
}
static void rawClipping() {
  using K=epub::structure::Kind;const std::string text="a \n\n";Collector c;c.begin();auto resource=c.openResource("c",0,false,0);
  auto p=c.open(K::Paragraph,0,epub::structure::NONE,resource,0,0,0);
  c.text(0,2,p,resource,0,1,0);c.close(p,1);c.closeResource(resource,text.size());
  std::vector<uint8_t> wire(c.snapshotBytes());assert(c.encodeSnapshot(wire.data(),wire.size(),text.data(),text.size()));
  Collector restored;assert(restored.restoreSnapshot(wire.data(),wire.size(),text.data(),text.size()));
  c.finish(text.data(),text.size());restored.finish(text.data(),text.size());
  epub_document::Document a,b;assert(a.assign(c,text.data(),text.size())&&b.assign(restored,text.data(),text.size()));
  std::vector<uint8_t> aw,bw;assert(a.encode(aw)&&b.encode(bw)&&aw==bw);
}
int main() {
  const Fixture horizontal({{html(""),false},{html("<p>Auxiliary note é</p>"),true},
    {html("<blockquote><p>Words <strong>bold </strong><i>italic</i> 中文</p><p xml:space='preserve'>   </p></blockquote><pre>  preserved\t end  </pre>"),false},
    {html("<p lang='en-GB'>Colour</p><div><section><p>Tail </p></section></div>"),false}});
  auto h=parity(horizontal,"PDS1");assert(h.text.find("Auxiliary")>h.text.find("Tail"));
  parity(Fixture({{html(""),false},{html("<p>Body</p>"),false}},""),"PDS1"); // zero-length first prefix
  {Fixture empty({{html(""),false},{html(""),false}});Session before(empty);assert(before.importer.begin());
    assert(before.importer.stepChapter()==Progress::Pending&&!before.importer.hasBodyText&&before.importer.used);
    const auto saved=save(before);Session after(empty);restore(after,saved);
    assert(after.importer.stepChapter()==Progress::Failed&&after.importer.error=="No readable EPUB text");}
  const std::string linked="<link rel='stylesheet' href='jp.css'/>";
  const Fixture vertical({{html("<ruby>日本<rp>(</rp><rt>にほん</rt><rp>)</rp></ruby><span class='tcy'>12</span>、。 gjpqyW|",linked,"class='v' lang='ja'"),false},
    {html("<p>Horizontal after vertical</p>"),false},{html("<p>後書き</p>",linked,"class='v'"),true}});
  parity(vertical,"PDS2");
  const Fixture objects({{html("<p>Before <math><mfrac><mi>x</mi><mn>2</mn></mfrac></math> after</p><math display='block'><msqrt><mi>y</mi></msqrt></math>"),false},
    {html("<table><tr><th>A</th><th>B</th></tr><tr><td>日本語</td><td>Cell</td></tr></table><p>After</p>"),false},
    {html("<ruby>東京<rt>とうきょう</rt></ruby>",linked,"class='v'"),false}});
  parity(objects,"PDS3");corruptions(objects);rawClipping();
  epub::structure::Limits tiny;tiny.records=4;parity(horizontal,nullptr,tiny);
  std::vector<Chapter> languages;
  for(unsigned i=0;i<34;++i)languages.push_back({html("<p lang='x-"+std::to_string(i)+"'>Word</p>"),false});
  auto budget=parity(Fixture(languages),nullptr);assert(budget.status==Status::Budget&&!budget.english);
  epub::structure::Allocator deny;deny.allocate=[](size_t,void*)->void*{return nullptr;};deny.release=[](void*,void*){};
  auto allocation=parity(horizontal,nullptr,epub::structure::Limits(),deny);assert(allocation.status==Status::Allocation&&!allocation.english);
  // A rich prefix followed by optional XML-memory failure must restart from
  // zero in flat mode; no partial rich snapshot can survive that transition.
  Session oom(objects);size_t chapters=0;oom.importer.xmlMemoryAvailable=[&](size_t,size_t){return !(oom.importer.structure&&oom.importer.chapterCursor==1&&++chapters);};
  assert(oom.importer.run()&&oom.importer.flattenedForMemory&&!oom.collector.ready());
  Session reference(objects);assert(reference.importer.run());
  assert(oom.importer.used&&std::string(oom.text.data(),oom.importer.used)==result(reference).text);
  std::cout<<"Collector ECS1 portable restart, torn/bounded restore, status/OOM and canonical PDS1/2/3 parity passed\n";
}
