#pragma once
// Small, read-only EPUB text importer. No filesystem writes or external resources.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include <memory>
#include <tinyxml2.h>
#include <lgfx/utility/lgfx_miniz.h>
#include "xml_input.h"
#include "epub_structure.h"
#include "text_language.h"
#include "epub_vertical_style.h"
#include "epub_mathml.h"

namespace epub {
inline uint16_t u16(const uint8_t* p){return p[0]|(uint16_t(p[1])<<8);}
inline uint32_t u32(const uint8_t* p){return u16(p)|(uint32_t(u16(p+2))<<16);}
inline const char* local(const char* n){const char* p=strchr(n,':');return p?p+1:n;}
inline tinyxml2::XMLElement* child(tinyxml2::XMLNode* p,const char* name){if(!p)return nullptr;for(auto e=p->FirstChildElement();e;e=e->NextSiblingElement())if(!strcmp(local(e->Name()),name))return e;return nullptr;}
inline const char* attr(tinyxml2::XMLElement* e,const char* name){const char* s=e?e->Attribute(name):nullptr;return s?s:"";}
struct Entry{std::string name;uint32_t compressed,size,offset,crc;uint16_t method,flags;};
struct Importer {
  std::function<size_t(uint32_t,void*,size_t)> read;
  std::function<void*(size_t)> alloc;
  std::function<void()> tick;
  // Called after checked input allocation and before TinyXML2 heap allocations.
  std::function<bool(size_t, size_t)> xmlMemoryAvailable;
  // Optional unpacked EPUB source; SIZE_MAX means missing resource.
  std::function<size_t(const std::string&)> resourceSize;
  std::function<bool(const std::string&,void*,size_t)> resourceRead;
  uint32_t fileSize=0;
  std::vector<Entry> entries;
  std::vector<std::string> unusedObfuscatedFonts;
  std::string error;
  char* output=nullptr;size_t capacity=0,used=0;
  bool ok=true;
  bool hasBodyText=false;
  // Opt-in sidecar only: null keeps the original flat importer and allocations.
  // All offsets refer to output bytes, preserving book identity and bookmarks.
  structure::Collector* structure = nullptr;
  uint32_t structureResource = structure::NONE;
  uint8_t structureLanguage = 0;
  vertical_style::Sheet verticalStyles;
  std::unique_ptr<char[]> mathScratch;
  bool fail(const char* s){error=s;ok=false;if(structure)structure->discard();return false;}
  bool exact(uint32_t at,void* p,size_t n){
    if(at>fileSize||n>fileSize-at)return false;
    for(size_t done=0;done<n;){const size_t count=std::min(size_t(8192),n-done);
      if(read(at+done,(uint8_t*)p+done,count)!=count)return false;
      done+=count;if(tick)tick();
    }return true;
  }
  bool index(){
    if(resourceRead)return true;
    if(fileSize<22)return fail("Invalid ZIP");
    size_t n=fileSize<65557?fileSize:65557;
    std::unique_ptr<uint8_t,decltype(&free)> tail((uint8_t*)alloc(n),free);
    if(!tail||!exact(fileSize-n,tail.get(),n))return fail("Cannot read ZIP directory");
    int pos=-1;for(int i=(int)n-22;i>=0;--i)if(u32(tail.get()+i)==0x06054b50&&size_t(i+22+u16(tail.get()+i+20))==n){pos=i;break;}
    if(pos<0)return fail("ZIP directory missing");auto p=tail.get()+pos;
    uint32_t start=u32(p+16),bytes=u32(p+12);uint16_t count=u16(p+10);
    if(u16(p+4)||u16(p+6)||count!=u16(p+8)||count==65535||count>2048||start>fileSize||bytes>fileSize-start)return fail("Unsupported ZIP64/multidisk/large ZIP");
    uint32_t cursor=start;size_t nameBytes=0;
    entries.reserve(count);
    for(uint16_t i=0;i<count;++i){
      uint8_t h[46];if(cursor-start>bytes||bytes-(cursor-start)<46||!exact(cursor,h,46)||u32(h)!=0x02014b50)return fail("Bad ZIP entry");
      uint16_t len=u16(h+28);uint32_t step=46u+len+u16(h+30)+u16(h+32);
      if(!len||len>1024||step>bytes-(cursor-start))return fail("Bad ZIP filename");
      if(nameBytes+len>128*1024)return fail("ZIP filename budget exceeded");nameBytes+=len;
      Entry e;e.name.resize(len);if(!exact(cursor+46,&e.name[0],len))return fail("ZIP read failed");
      e.flags=u16(h+8);e.method=u16(h+10);e.crc=u32(h+16);e.compressed=u32(h+20);e.size=u32(h+24);e.offset=u32(h+42);
      entries.push_back(e);cursor+=step;if(tick)tick();
    }
    return true;
  }
  const Entry* find(const std::string& name){for(auto& e:entries)if(e.name==name)return &e;return nullptr;}
  std::unique_ptr<char,decltype(&free)> extract(const std::string& name,size_t max=1024*1024,size_t* actualSize=nullptr){
    std::unique_ptr<char,decltype(&free)> out(nullptr,free);
    if(name.size()>1024){fail("EPUB resource path too long");return out;}
    for(const auto& path:unusedObfuscatedFonts)if(path==name){fail("Obfuscated resource cannot be read as text");return out;}
    {
      if(name.empty()||name[0]=='/'||name.find('\\')!=std::string::npos){fail("Invalid EPUB resource path");return out;}
      size_t at=0;while(at<=name.size()){size_t end=name.find('/',at);if(end==std::string::npos)end=name.size();if(name.substr(at,end-at)==".."){fail("EPUB path escapes directory");return out;}at=end+1;}
    }
    if(resourceRead){
      size_t size=resourceSize(name);
      if(size==SIZE_MAX){fail("EPUB resource missing");return out;}
      if(size>max){fail("Chapter too large");return out;}
      out.reset((char*)alloc(size+1));if(!out){fail("Not enough PSRAM");return out;}
      if(!resourceRead(name,out.get(),size)){fail("EPUB resource read failed");out.reset();return out;}
      out.get()[size]=0;if(actualSize)*actualSize=size;if(tick)tick();return out;
    }
    const Entry* e=find(name);
    if(!e){fail("EPUB resource missing");return out;}
    if(e->flags&1){fail("Encrypted EPUB unsupported");return out;}
    if(e->size>max||e->compressed>1024*1024||(e->method!=0&&e->method!=8)){fail("Chapter too large or unsupported ZIP method");return out;}
    uint8_t h[30];if(!exact(e->offset,h,30)||u32(h)!=0x04034b50){fail("Bad ZIP local header");return out;}
    uint64_t start=uint64_t(e->offset)+30+u16(h+26)+u16(h+28);if(start>fileSize||e->compressed>fileSize-start){fail("ZIP data out of bounds");return out;}
    out.reset((char*)alloc(e->size+1));if(!out){fail("Not enough PSRAM");return out;}
    if(e->method==0){if(e->size!=e->compressed||!exact(start,out.get(),e->size)){fail("Stored ZIP length mismatch");out.reset();return out;}}
    else {
      std::unique_ptr<uint8_t,decltype(&free)> compressed((uint8_t*)alloc(e->compressed),free);
      if(!compressed){fail("Not enough PSRAM");out.reset();return out;}
      if(!exact(start,compressed.get(),e->compressed)){fail("EPUB decompression failed");out.reset();return out;}
      // The convenience mem_to_mem helper puts its ~11 KiB Huffman state on
      // the task stack. Keep it in the same checked allocator as chapter data
      // (PSRAM on the device), and release it before constructing the XML DOM.
      std::unique_ptr<lgfx_tinfl_decompressor,decltype(&free)> state(
        (lgfx_tinfl_decompressor*)alloc(sizeof(lgfx_tinfl_decompressor)),free);
      if(!state){fail("Not enough PSRAM for EPUB decompressor");out.reset();return out;}
      memset(state.get(),0,sizeof(*state));lgfx_tinfl_init(state.get());
      size_t consumed=0,produced=0;
      lgfx_tinfl_status status=TINFL_STATUS_HAS_MORE_OUTPUT;
      // Keep the full, non-wrapping output dictionary, but return to the input
      // observer every 8 KiB. This does not expose a partial chapter or retain
      // decompressor state beyond this call. HAS_MORE_INPUT rejects truncation.
      do {
        size_t inputBytes=e->compressed-consumed;
        size_t outputBytes=std::min(size_t(8192),size_t(e->size)-produced);
        status=lgfx_tinfl_decompress(state.get(),compressed.get()+consumed,&inputBytes,
          (lgfx_mz_uint8*)out.get(),(lgfx_mz_uint8*)out.get()+produced,&outputBytes,
          TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF|TINFL_FLAG_HAS_MORE_INPUT);
        consumed+=inputBytes;produced+=outputBytes;
        if(tick)tick();
        if(status!=TINFL_STATUS_HAS_MORE_OUTPUT)break;
        if(!outputBytes)break;
      } while(true);
      if(status!=TINFL_STATUS_DONE||consumed!=e->compressed||produced!=e->size){
        fail("EPUB decompression failed");out.reset();return out;
      }
    }
    if(lgfx_mz_crc32(0,(const unsigned char*)out.get(),e->size)!=e->crc){fail("EPUB CRC mismatch");out.reset();return out;}
    out.get()[e->size]=0;if(actualSize)*actualSize=e->size;if(tick)tick();return out;
  }
  std::string resolve(const std::string& base,const std::string& href){
    // Bound inputs before substrings or path components allocate memory.
    if(base.size()>1024||href.size()>1024){fail("EPUB resource path too long");return "";}
    std::string path=href.substr(0,href.find('#')),decoded;
    for(size_t i=0;i<path.size();++i){if(path[i]=='%'&&i+2<path.size()){char hex[3]={path[i+1],path[i+2],0};char* end;long v=strtol(hex,&end,16);if(*end==0&&v){decoded+=char(v);i+=2;continue;}}decoded+=path[i];}
    if(decoded.empty()||decoded[0]=='/'||decoded.find(':')!=std::string::npos||decoded.find('\\')!=std::string::npos){fail("Invalid EPUB resource path");return "";}
    std::string combined=base.substr(0,base.find_last_of('/')==std::string::npos?0:base.find_last_of('/')+1)+decoded;
    std::vector<std::string> parts;size_t from=0;
    while(from<=combined.size()){size_t end=combined.find('/',from);if(end==std::string::npos)end=combined.size();auto s=combined.substr(from,end-from);if(s==".."){if(parts.empty()){fail("EPUB path escapes archive");return "";}parts.pop_back();}else if(!s.empty()&&s!=".")parts.push_back(s);from=end+1;}
    std::string result;for(auto& s:parts){if(!result.empty())result+='/';result+=s;}
    if(result.size()>1024){fail("EPUB resource path too long");return "";}
    return result;
  }
  bool parseXML(const std::string& path, size_t limit, tinyxml2::XMLDocument& document, const char* invalid) {
    size_t size = 0;
    auto source = extract(path, limit, &size);
    if (!source) return false;
    xml_input::Budget budget;
    if (!xml_input::prepare(source, size, alloc, budget, error)) { ok = false; if (structure) structure->discard(); return false; }
    size_t required = size + 1 + budget.domBytes + 64 * 1024;
    if (xmlMemoryAvailable && !xmlMemoryAvailable(required, std::max(size + 1, size_t(4096))))
      return fail("Not enough memory for XML document");
    document.SetProgressCallback([](void* context){auto* importer=static_cast<Importer*>(context);if(importer->tick)importer->tick();},this);
    if (document.Parse(source.get(), size) != tinyxml2::XML_SUCCESS) return fail(invalid);
    if (tick) tick();
    return true;
  }
  void append(char c) {
    if (!ok) return;
    if (used >= capacity) { fail("Book text exceeds reader capacity"); return; }
    output[used++] = c;
  }
  void newline(unsigned count = 2) {
    while (used && output[used - 1] == ' ') --used;
    if (structure) structure->trimTail(used);
    if (!used) return;
    unsigned existing = 0;
    while (existing < used && output[used - existing - 1] == '\n') ++existing;
    while (existing++ < count) append('\n');
  }
  void text(const char* value, bool preserve = false) {
    if (!value) return;
    size_t observed=0;
    while (*value && ok) {
      if(++observed==8192){observed=0;if(tick)tick();}
      char c = *value++;
      if (xml_input::space(c)) {
        if (preserve) {
          if (c == '\t') { for (unsigned i = 0; i < 4; ++i) append(' '); }
          else append(c == '\r' ? '\n' : c);
        } else if (used && output[used - 1] != ' ' && output[used - 1] != '\n') append(' ');
      } else {
        append(c); hasBodyText = true;
      }
    }
  }
  uint8_t nodeLanguage(tinyxml2::XMLElement* element, uint8_t inherited) {
    if (!element) return inherited;
    const char* tag = element->Attribute("xml:lang");
    if (!tag) tag = element->Attribute("lang");
    if(tag){
      const auto localLanguage=text_language::fromTag(tag);
      if(localLanguage==text_language::Language::None||
         (language!=text_language::Language::None&&localLanguage!=language))unsafeFlatLanguage=true;
    }
    if(!structure)return inherited;
    return tag ? structure->internLanguage(tag) : inherited;
  }
  static structure::Kind blockKind(const char* name) {
    using structure::Kind;
    if (!strcmp(name,"math")) return Kind::Math;
    if (!strcmp(name,"table")) return Kind::Table;
    if (!strcmp(name,"td")||!strcmp(name,"th")) return Kind::TableCell;
    if (!strcmp(name,"ruby")) return Kind::Ruby;
    if (!strcmp(name,"rt")) return Kind::RubyText;
    if (!strcmp(name,"rp")) return Kind::RubyFallback;
    if (!strcmp(name,"p")) return Kind::Paragraph;
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) return Kind::Heading;
    if (!strcmp(name,"blockquote")) return Kind::Quote;
    if (!strcmp(name,"li")) return Kind::ListItem;
    if (!strcmp(name,"pre")) return Kind::Preformatted;
    if (!strcmp(name,"ol")) return Kind::OrderedList;
    if (!strcmp(name,"ul")) return Kind::UnorderedList;
    if (!strcmp(name,"dt")) return Kind::DefinitionTerm;
    if (!strcmp(name,"dd")) return Kind::DefinitionDetail;
    if (!strcmp(name,"tr")) return Kind::TableRow;
    if (!strcmp(name,"div") || !strcmp(name,"section") || !strcmp(name,"article") ||
        !strcmp(name,"body")) return Kind::Container;
    return Kind::Text; // no semantic block; descendants retain their parent
  }
  void walk(tinyxml2::XMLNode* node, unsigned depth = 0, bool preserve = false,
            uint32_t parent = structure::NONE, uint16_t flags = 0, uint8_t language = 0) {
    if (!node || !ok) return;
    if(tick)tick();
    if (depth > xml_input::MAX_DEPTH) { fail("XML depth exceeds 32 levels"); return; }
    if (auto textNode = node->ToText()) {
      const size_t begin = used;
      text(textNode->Value(), preserve);
      if (structure) structure->text(begin, used, parent, structureResource, flags, uint8_t(depth), language);
      return;
    }
    auto element = node->ToElement();
    const char* name = element ? local(element->Name()) : "";
    if (!strcmp(name,"head") || !strcmp(name,"script") || !strcmp(name,"style") ||
        !strcmp(name,"svg") || !strcmp(name,"img")) return;
    language = nodeLanguage(element, language);
    if(structure){const auto style=verticalStyles.resolve(element);
      if(style.mode>=0){flags&=~structure::VerticalRL;if(style.mode)flags|=structure::VerticalRL;}
      if(style.combine>=0){flags&=~structure::Combine;if(style.combine)flags|=structure::Combine;}
    }
    if (!strcmp(name, "br")) {
      const size_t begin = used;
      if (used) append('\n');
      if (structure) structure->text(begin, used, parent, structureResource, flags,
                                     uint8_t(depth), language, structure::Kind::Break);
      return;
    }
    preserve = preserve || !strcmp(name, "pre") || !strcmp(attr(element, "xml:space"), "preserve");
    if (structure) {
      if (preserve) flags |= structure::Preserve;
      if(!strcmp(name,"th"))flags|=structure::TableHeader;
      if((!strcmp(name,"td")||!strcmp(name,"th"))&&
         ((*attr(element,"rowspan")&&strcmp(attr(element,"rowspan"),"1"))||(*attr(element,"colspan")&&strcmp(attr(element,"colspan"),"1"))))flags|=structure::UnsupportedLayout;
      if (!strcmp(name,"strong") || !strcmp(name,"b")) flags |= structure::Strong;
      if (!strcmp(name,"em") || !strcmp(name,"i")) flags |= structure::Emphasis;
      if (!strcmp(name,"code") || !strcmp(name,"kbd") || !strcmp(name,"samp")) flags |= structure::Code;
      if (!strcmp(name,"sup")) flags |= structure::Superscript;
      if (!strcmp(name,"sub")) flags |= structure::Subscript;
    }
    bool block = !strcmp(name,"p") || !strcmp(name,"div") || !strcmp(name,"li") ||
      !strcmp(name,"section") || !strcmp(name,"article") || !strcmp(name,"tr") ||
      !strcmp(name,"blockquote") || !strcmp(name,"pre") || !strcmp(name,"dt") ||
      !strcmp(name,"dd") || (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]);
    if (block) newline();
    uint32_t record = structure::NONE;
    if (structure) {
      const structure::Kind kind = blockKind(name);
      if (kind != structure::Kind::Text) {
        record = structure->open(kind, used, parent, structureResource, flags, uint8_t(depth),
                                 language, kind == structure::Kind::Heading ? name[1] - '0' : 0);
        parent = record;
      }
    }
    const size_t objectBegin=used;
    for (auto next = node->FirstChild(); next && ok; next = next->NextSibling())
      walk(next, depth + 1, preserve, parent, flags, language);
    // Match newline()'s trimming without modifying output. Parent ranges are
    // clipped together at finish(), including empty nested blocks and spans.
    if (structure) {
      size_t end = used;
      if (block) while (end && output[end - 1] == ' ') --end;
      structure->close(record, end);
    }
    if(structure&&!strcmp(name,"math")&&used>objectBegin){
      if(!mathScratch)mathScratch.reset(new(std::nothrow) char[4096]);
      if(!mathScratch)structure->discard(structure::Status::Allocation);
      else {epub_mathml::Converter converter(mathScratch.get(),4096);if(converter.convert(element)){size_t objectEnd=used;while(objectEnd>objectBegin&&output[objectEnd-1]==' ')--objectEnd;structure->addMath(objectBegin,objectEnd,mathScratch.get(),converter.size(),!strcmp(attr(element,"display"),"block"));}}
    }
    if (block) newline();
  }
  void chapterStyles(tinyxml2::XMLElement* html,const std::string& path){
    verticalStyles.clear();if(!structure)return;
    auto head=child(html,"head");size_t total=0;unsigned links=0;
    for(auto e=head?head->FirstChildElement():nullptr;e;e=e->NextSiblingElement()){
      if(!strcmp(local(e->Name()),"style")){const char* css=e->GetText();if(css){size_t n=strlen(css);if(n<=65536&&total+n<=131072){verticalStyles.add(css,n);total+=n;}}}
      else if(!strcmp(local(e->Name()),"link")&&!strcmp(attr(e,"rel"),"stylesheet")&&links++<8){
        const char* href=attr(e,"href");if(!*href||strchr(href,':')||href[0]=='/')continue;
        const auto resolved=resolve(path,href);if(!ok)return;
        size_t bytes=resourceRead?resourceSize(resolved):(find(resolved)?find(resolved)->size:SIZE_MAX);
        if(bytes==SIZE_MAX||bytes>65536||total+bytes>131072)continue;
        auto css=extract(resolved,65536);if(!css)return;verticalStyles.add(css.get(),bytes);total+=bytes;
      }
    }
    if(verticalStyles.failed())structure->discard(structure::Status::Allocation);
  }
  bool englishHyphenation = false;
  text_language::Language language=text_language::Language::None;
  // Inspect container/package metadata without expanding any spine resource.
  // This upgrades older flat-text caches which did not retain the English
  // regional variant; output may be null when inspectOnly is true.
  bool inspectOnly=false;
  bool flattenedForMemory=false;
  bool unsafeFlatLanguage=false;
  void constrainFlatLanguage(){
    // A flat fallback cannot retain span-level language boundaries. Disable
    // automatic rules for the whole book rather than applying the package's
    // English pattern set to an explicitly foreign/unknown span.
    if(unsafeFlatLanguage&&(!structure||!structure->ready())){
      language=text_language::Language::None;englishHyphenation=false;
    }
  }
  struct Chapter { std::string path; bool auxiliary; uint16_t spineIndex; };
  enum class Progress : uint8_t { Pending, Complete, Failed };
  std::vector<Chapter> chapters;
  size_t chapterCursor=0;
  Progress progress=Progress::Failed;
  // Chapter boundaries are stable canonical prefixes. XML/deflate remain
  // indivisible, and no DOM or allocation address is retained between steps.
  bool begin() {
    restoreCollector();entries.clear();unusedObfuscatedFonts.clear();
    used=0;ok=true;hasBodyText=false;error.clear();englishHyphenation=false;
    language=text_language::Language::None;structureResource=structure::NONE;structureLanguage=0;
    unsafeFlatLanguage=false;flattenedForMemory=false;mathScratch.reset();verticalStyles=vertical_style::Sheet{};
    chapters.clear();chapterCursor=0;
    progress=prepare()? (inspectOnly?Progress::Complete:Progress::Pending):Progress::Failed;
    if(progress==Progress::Failed)retryFlat();
    return progress!=Progress::Failed;
  }
  Progress stepChapter() {
    if(progress!=Progress::Pending)return progress;
    if(chapterCursor<chapters.size()){
      if(!importChapter(chapters[chapterCursor])){
        progress=Progress::Failed;retryFlat();return progress;
      }
      mathScratch.reset();verticalStyles=vertical_style::Sheet{};
      ++chapterCursor;
    }
    if(chapterCursor==chapters.size()){
      if(chapters.empty()||!hasBodyText){fail("No readable EPUB text");progress=Progress::Failed;}
      else{
        output[used]=0;if(structure)structure->finish(output,used);
        constrainFlatLanguage();progress=Progress::Complete;
      }
      restoreCollector();
    }
    return progress;
  }
  bool run() {
    if(!begin())return false;
    while(stepChapter()==Progress::Pending){}
    return progress==Progress::Complete;
  }
  // Restore only after a freshly prepared, source-validated matching plan and
  // separately validated Collector/prefix. The persistence layer hashes every
  // field and binds the plan, source graph and extractor revision.
  bool restoreProgress(size_t cursor,size_t bytes,bool body,bool unsafe,
                       uint8_t packageLanguage,uint8_t collectorLanguage,bool complete) {
    if(progress!=Progress::Pending||flattenedForMemory||cursor>chapters.size()||
       bytes>capacity||collectorLanguage!=structureLanguage||
       (packageLanguage!=uint8_t(language)&&!(complete&&unsafe&&(!structure||!structure->ready())&&packageLanguage==uint8_t(text_language::Language::None)))||
       (complete&&(cursor!=chapters.size()||!body))||(!complete&&cursor==chapters.size()))return false;
    chapterCursor=cursor;used=bytes;hasBodyText=body;unsafeFlatLanguage=unsafe;
    language=text_language::Language(packageLanguage);englishHyphenation=language!=text_language::Language::None;
    structureLanguage=collectorLanguage;structureResource=structure::NONE;
    output[used]=0;progress=complete?Progress::Complete:Progress::Pending;
    if(complete)constrainFlatLanguage();return true;
  }
 private:
  structure::Collector* retryCollector=nullptr;
  void restoreCollector(){if(retryCollector){structure=retryCollector;retryCollector=nullptr;}}
  bool retryFlat(){
    if(!structure||flattenedForMemory||(error!="Not enough PSRAM"&&error!="Not enough PSRAM for EPUB decompressor"&&
       error!="Not enough PSRAM for XML"&&error!="Not enough memory for XML document")){
      restoreCollector();return false;
    }
    retryCollector=structure;structure->discard();structure=nullptr;
    mathScratch.reset();verticalStyles=vertical_style::Sheet{};
    std::vector<Entry>().swap(entries);std::vector<std::string>().swap(unusedObfuscatedFonts);
    chapters.clear();chapterCursor=0;used=0;ok=true;hasBodyText=false;error.clear();englishHyphenation=false;
    language=text_language::Language::None;structureResource=structure::NONE;structureLanguage=0;unsafeFlatLanguage=false;
    flattenedForMemory=true;
    progress=prepare()?(inspectOnly?Progress::Complete:Progress::Pending):Progress::Failed;
    if(progress!=Progress::Pending)restoreCollector();
    return progress!=Progress::Failed;
  }
  bool prepare() {
    if (structure) structure->begin();
    if (!index()) return false;
    if (resourceRead ? resourceSize("META-INF/encryption.xml") != SIZE_MAX : find("META-INF/encryption.xml") != nullptr) {
      tinyxml2::XMLDocument doc;
      if (!parseXML("META-INF/encryption.xml", 65536, doc, "Invalid encryption XML")) return false;
      auto root = child(&doc, "encryption");
      if (!root) return fail("Invalid encryption XML");
      for (auto item = root->FirstChildElement(); item; item = item->NextSiblingElement()) {
        if (strcmp(local(item->Name()), "EncryptedData")) return fail("Encrypted EPUB resources unsupported");
        std::string algorithm = attr(child(item, "EncryptionMethod"), "Algorithm");
        std::string path = resolve("", attr(child(child(item,"CipherData"),"CipherReference"),"URI"));
        if (!ok) return false;
        auto ext = path.substr(path.find_last_of('.') == std::string::npos ? path.size() : path.find_last_of('.'));
        if ((algorithm != "http://www.idpf.org/2008/embedding" && algorithm != "http://ns.adobe.com/pdf/enc#RC") || (ext != ".ttf" && ext != ".otf"))
          return fail("Encrypted EPUB resources unsupported");
        unusedObfuscatedFonts.push_back(path);
      }
    }
    std::string root;
    {
      tinyxml2::XMLDocument doc;
      if (!parseXML("META-INF/container.xml", 65536, doc, "Invalid container XML")) return false;
      root = attr(child(child(child(&doc,"container"),"rootfiles"),"rootfile"), "full-path");
      if (root.empty()) return fail("Missing EPUB rootfile");
    }
    size_t pathBytes = 0;
    {
      tinyxml2::XMLDocument opf;
      if (!parseXML(root, 256 * 1024, opf, "Invalid package XML")) return false;
      auto pkg = child(&opf,"package"), manifest = child(pkg,"manifest"), spine = child(pkg,"spine");
      if (!manifest || !spine) return fail("Missing EPUB spine");
      auto language = child(child(pkg,"metadata"),"language");
      if (language && language->GetText()) {
        this->language=text_language::fromTag(language->GetText());
        englishHyphenation=this->language!=text_language::Language::None;
        if (structure) structureLanguage = structure->internLanguage(language->GetText());
      }
      auto title = child(child(pkg,"metadata"),"title");
      if (title&&!inspectOnly) {
        const size_t begin = used;
        text(title->GetText());
        if (structure) {
          const uint32_t id = structure->open(structure::Kind::Title, begin, structure::NONE,
            structure::NONE, 0, 0, nodeLanguage(title, structureLanguage));
          size_t end = used; while (end && output[end - 1] == ' ') --end;
          structure->close(id, end);
        }
        newline();
      }
      for (auto ref = spine->FirstChildElement(); ref; ref = ref->NextSiblingElement()) {
        if (strcmp(local(ref->Name()), "itemref")) continue;
        std::string href;
        for (auto item = manifest->FirstChildElement(); item; item = item->NextSiblingElement()) {
          if (!strcmp(attr(item,"id"),attr(ref,"idref"))) { href = attr(item,"href"); break; }
        }
        if (href.empty()) return fail("Chapter ID missing");
        auto path = resolve(root, href); if (!ok) return false;
        pathBytes += path.size();
        if (chapters.size() >= 2048 || pathBytes > 128 * 1024) return fail("EPUB spine resource budget exceeded");
        chapters.push_back({std::move(path), !strcmp(attr(ref,"linear"),"no"), uint16_t(chapters.size())});
      }
    }
    if(inspectOnly)return true;
    hasBodyText = false;
    // Stable partition preserves the prior two-pass primary/auxiliary order.
    std::stable_partition(chapters.begin(),chapters.end(),[](const Chapter& c){return !c.auxiliary;});
    return true;
  }
  bool importChapter(const Chapter& chapter) {
    tinyxml2::XMLDocument doc(true, tinyxml2::PEDANTIC_WHITESPACE);
    if (!parseXML(chapter.path, xml_input::MAX_BYTES, doc, "Chapter is not valid XHTML")) return false;
    auto html = child(&doc, "html");
    auto body = child(html, "body");
    if (!body) return fail("Chapter has no XHTML body");
    if (structure) structureResource = structure->openResource(chapter.path.c_str(), chapter.spineIndex,
                                                               chapter.auxiliary, used);
    chapterStyles(html,chapter.path);if(!ok)return false;
    uint16_t initialFlags=0;const auto htmlStyle=verticalStyles.resolve(html);
    if(htmlStyle.mode==1)initialFlags|=structure::VerticalRL;
    if(htmlStyle.combine==1)initialFlags|=structure::Combine;
    walk(body, 0, false, structure::NONE, initialFlags, nodeLanguage(html, structureLanguage)); newline();
    if (structure) structure->closeResource(structureResource, used);
    if (!ok) return false;
    if (tick) tick();
    return true;
  }
};
}
