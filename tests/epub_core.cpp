#include <fstream>
#include <filesystem>
#include <iterator>
#include <iostream>
#include <cassert>
#include "epub_core.h"

static void quote(const char* text) {
  std::cerr << '"';
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
    if (*p == '"' || *p == '\\') std::cerr << '\\' << char(*p);
    else if (*p < 32) { const char* hex = "0123456789abcdef"; std::cerr << "\\u00" << hex[*p >> 4] << hex[*p & 15]; }
    else std::cerr << char(*p);
  }
  std::cerr << '"';
}
static const char* kind(epub::structure::Kind value) {
  using K = epub::structure::Kind;
  switch(value) {
    case K::Text: return "text"; case K::Title: return "title";
    case K::Paragraph: return "paragraph"; case K::Heading: return "heading";
    case K::Quote: return "quote"; case K::ListItem: return "list-item";
    case K::Preformatted: return "pre"; case K::Container: return "container";
    case K::OrderedList: return "ol"; case K::UnorderedList: return "ul";
    case K::DefinitionTerm: return "dt"; case K::DefinitionDetail: return "dd";
    case K::TableRow: return "tr"; case K::Break: return "break";
    case K::Ruby: return "ruby";case K::RubyText: return "rt";case K::RubyFallback: return "rp";
    case K::Math: return "math";case K::Table: return "table";case K::TableCell: return "cell";
  }
  return "invalid";
}
static void dump(const epub::structure::Collector& sidecar) {
  std::cerr << "{\"ready\":" << sidecar.ready() << ",\"status\":" << int(sidecar.status())
            << ",\"bytes\":" << sidecar.allocatedBytes() << ",\"records\":[";
  for (size_t i = 0; i < sidecar.recordCount(); ++i) {
    const auto& r = sidecar.records()[i];
    if (i) std::cerr << ',';
    std::cerr << "{\"begin\":" << r.begin << ",\"end\":" << r.end << ",\"parent\":" << r.parent
              << ",\"resource\":" << r.resource << ",\"flags\":" << r.flags
              << ",\"depth\":" << int(r.depth) << ",\"level\":" << int(r.headingLevel) << ",\"kind\":";
    quote(kind(r.kind)); std::cerr << ",\"language\":"; quote(sidecar.language(r.language)); std::cerr << '}';
  }
  std::cerr << "],\"resources\":[";
  for (size_t i = 0; i < sidecar.resourceCount(); ++i) {
    const auto& r = sidecar.resources()[i];
    if (i) std::cerr << ',';
    std::cerr << "{\"begin\":" << r.begin << ",\"end\":" << r.end << ",\"spine\":" << r.spineIndex
              << ",\"auxiliary\":" << r.auxiliary << ",\"path\":";
    quote(sidecar.path(i)); std::cerr << '}';
  }
  std::cerr << "]}\n";
}
static void collectorContracts() {
  using namespace epub::structure;
  size_t allocations = 0, releases = 0;
  struct Counts { size_t* allocations; size_t* releases; } counts{&allocations, &releases};
  Allocator allocator;
  allocator.context = &counts;
  allocator.allocate = [](size_t bytes, void* context)->void* {
    ++*static_cast<Counts*>(context)->allocations; return std::malloc(bytes);
  };
  allocator.release = [](void* p, void* context) { ++*static_cast<Counts*>(context)->releases; std::free(p); };
  Limits limits; limits.records = 1; limits.resources = 1; limits.pathBytes = 2;
  {
    Collector c(limits, allocator);
    assert(!c.ready() && !c.records() && c.recordCount() == 0);
    c.begin(); assert(allocations == 1 && releases == 0);
    uint32_t resource = c.openResource("x", 9, false, 0);
    uint32_t record = c.open(Kind::Paragraph, 0, NONE, resource, 0, 0, 0);
    c.close(record, 1); c.closeResource(resource, 1); c.finish("x", 1);
    assert(c.ready() && c.recordCount() == 1 && c.resourceCount() == 1);
    assert(!std::strcmp(c.path(0), "x") && c.resources()[0].spineIndex == 9);
    assert(c.path(NONE)[0] == 0 && c.language(255)[0] == 0);
    c.begin(); assert(allocations == 2 && releases == 1);
    c.open(Kind::Paragraph, 0, NONE, NONE, 0, 0, 0);
    c.open(Kind::Paragraph, 0, NONE, NONE, 0, 0, 0);
    c.finish("x", 1); assert(c.status() == Status::Budget && !c.records() && !c.resourceCount());
    c.begin(); c.openResource("xx", 0, false, 0); assert(c.status() == Status::Budget);
    c.begin(); c.openResource("x", 0, false, 0); c.openResource("x", 0, false, 0);
    assert(c.status() == Status::Budget);
    c.begin(); c.open(Kind::Paragraph, 0, 7, NONE, 0, 0, 0); assert(c.status() == Status::Invalid);
    c.begin(); record = c.open(Kind::Text, 1, NONE, NONE, 0, 0, 0); c.close(record, 2);
    c.finish("\xc3\xa9", 2); assert(c.status() == Status::Invalid);
    c.begin(); c.discard(); c.finish("x", 1); assert(c.status() == Status::ImportFailed && !c.ready());
    c.begin(); uint8_t language = c.internLanguage(" EN-gb \t");
    assert(language != 0 && !std::strcmp(c.language(language), "en-gb"));
    assert(c.internLanguage("en-GB") == language && c.internLanguage("") == 0);
    assert(c.internLanguage("x<script>") == 0);
    assert(c.internLanguage("abcdefghijklmnopqrstuvwxyzabcdefghijklmnop") == 0);
    for (unsigned i = 0; i < 32; ++i) { const std::string tag = "x-" + std::to_string(i); c.internLanguage(tag.c_str()); }
    assert(c.status() == Status::Budget && !c.ready());
  }
  assert(allocations == releases);
  limits.records = std::numeric_limits<size_t>::max();
  Collector overflow(limits, allocator); overflow.begin(); assert(overflow.status() == Status::Budget);
  assert(allocations == releases);
  limits = Limits(); limits.totalBytes = 1;
  Collector total(limits, allocator); total.begin(); assert(total.status() == Status::Budget);
  Allocator wrong; wrong.allocate = allocator.allocate;
  Collector invalid(Limits(), wrong); invalid.begin(); assert(invalid.status() == Status::Invalid);
}

int main(int argc,char** argv){
 if(argc<2||argc>3)return 2;
 collectorContracts();
 std::string mode=argc==3?argv[2]:"";
 size_t allocation=0,failAllocation=mode.rfind("--fail-alloc=",0)==0?std::stoul(mode.substr(13)):0;
 size_t inflateAllocation=0,failInflate=mode.rfind("--fail-inflate=",0)==0?std::stoul(mode.substr(15)):0;
 std::ifstream file;if(!std::filesystem::is_directory(argv[1]))file.open(argv[1],std::ios::binary);std::vector<char> bytes((std::istreambuf_iterator<char>(file)),{}),out(4*1024*1024+1);
 epub::Importer e;e.fileSize=bytes.size();e.output=out.data();e.capacity=out.size()-1;e.alloc=[&](size_t n)->void*{
  if(n==sizeof(lgfx_tinfl_decompressor)){
   ++inflateAllocation;
   if(inflateAllocation==failInflate||(mode=="--structure-inflate-memory-retry"&&e.structure)||mode=="--structure-inflate-oom")return nullptr;
  }
  return ++allocation==failAllocation?nullptr:malloc(n);
 };
 e.inspectOnly=mode=="--inspect-language";
 e.xmlMemoryAvailable=[&](size_t required,size_t largest){return mode!="--deny-xml-memory"&&!(mode=="--structure-memory-retry"&&e.structure)&&required>=largest&&largest>=4096;};
 e.read=[&](uint32_t at,void* p,size_t n)->size_t{if(at>bytes.size()||n>bytes.size()-at)return 0;memcpy(p,bytes.data()+at,n);return n;};
 if(std::filesystem::is_directory(argv[1])){
  std::filesystem::path root(argv[1]);
  e.resourceSize=[root](const std::string& name)->size_t{auto p=root/name;return std::filesystem::is_regular_file(p)?std::filesystem::file_size(p):SIZE_MAX;};
  e.resourceRead=[root](const std::string& name,void* data,size_t n)->bool{std::ifstream f(root/name,std::ios::binary);return bool(f.read((char*)data,n));};
 }
 epub::structure::Limits limits;
 if(mode=="--structure-record-budget")limits.records=1;
 if(mode=="--structure-path-budget")limits.pathBytes=1;
 if(mode=="--structure-resource-budget")limits.resources=0;
 epub::structure::Allocator allocator;
 if(mode=="--structure-oom"){
  allocator.allocate=[](size_t,void*)->void*{return nullptr;};
  allocator.release=[](void*,void*){};
 }
 epub::structure::Collector sidecar(limits,allocator);
 if(mode.rfind("--structure",0)==0)e.structure=&sidecar;
 if(!e.run()){
  if(e.structure)assert(!sidecar.ready() && sidecar.recordCount()==0 && sidecar.resourceCount()==0);
  if(mode=="--structure-inflate-oom")assert(e.flattenedForMemory&&inflateAllocation==2);
  std::cerr<<e.error<<'\n';return 1;
 }
 if(e.structure)dump(sidecar);
 if(mode=="--language"||mode=="--inspect-language")std::cerr<<"HYPHEN_LANGUAGE english="<<e.englishHyphenation<<" region="<<unsigned(e.language)<<"\n";
 if(mode=="--structure-memory-retry")assert(!e.englishHyphenation||!e.unsafeFlatLanguage);
 if(mode=="--structure-inflate-memory-retry")assert(e.flattenedForMemory&&!sidecar.ready()&&inflateAllocation==4);
 std::cout.write(out.data(),e.used);return 0;
}
