#pragma once
// Small, read-only EPUB text importer. No filesystem writes or external resources.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <tinyxml2.h>
#include <lgfx/utility/lgfx_miniz.h>
#include "xml_input.h"

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
  char* output;size_t capacity,used=0;
  bool ok=true;
  bool hasBodyText=false;
  bool fail(const char* s){error=s;ok=false;return false;}
  bool exact(uint32_t at,void* p,size_t n){return at<=fileSize&&n<=fileSize-at&&read(at,p,n)==n;}
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
      if(!exact(start,compressed.get(),e->compressed)||lgfx_tinfl_decompress_mem_to_mem(out.get(),e->size,compressed.get(),e->compressed,0)!=e->size){fail("EPUB decompression failed");out.reset();return out;}
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
    if (!xml_input::prepare(source, size, alloc, budget, error)) { ok = false; return false; }
    size_t required = size + 1 + budget.domBytes + 64 * 1024;
    if (xmlMemoryAvailable && !xmlMemoryAvailable(required, std::max(size + 1, size_t(4096))))
      return fail("Not enough memory for XML document");
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
    if (!used) return;
    unsigned existing = 0;
    while (existing < used && output[used - existing - 1] == '\n') ++existing;
    while (existing++ < count) append('\n');
  }
  void text(const char* value, bool preserve = false) {
    if (!value) return;
    while (*value && ok) {
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
  void walk(tinyxml2::XMLNode* node, unsigned depth = 0, bool preserve = false) {
    if (!node || !ok) return;
    if (depth > xml_input::MAX_DEPTH) { fail("XML depth exceeds 32 levels"); return; }
    if (auto textNode = node->ToText()) { text(textNode->Value(), preserve); return; }
    auto element = node->ToElement();
    const char* name = element ? local(element->Name()) : "";
    if (!strcmp(name,"head") || !strcmp(name,"script") || !strcmp(name,"style") ||
        !strcmp(name,"svg") || !strcmp(name,"img")) return;
    if (!strcmp(name, "br")) { if (used) append('\n'); return; }
    preserve = preserve || !strcmp(name, "pre") || !strcmp(attr(element, "xml:space"), "preserve");
    bool block = !strcmp(name,"p") || !strcmp(name,"div") || !strcmp(name,"li") ||
      !strcmp(name,"section") || !strcmp(name,"article") || !strcmp(name,"tr") ||
      !strcmp(name,"blockquote") || !strcmp(name,"pre") || !strcmp(name,"dt") ||
      !strcmp(name,"dd") || (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]);
    if (block) newline();
    for (auto next = node->FirstChild(); next && ok; next = next->NextSibling()) walk(next, depth + 1, preserve);
    if (block) newline();
  }
  bool englishHyphenation = false;
  bool run() {
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
    struct Chapter { std::string path; bool auxiliary; };
    std::vector<Chapter> chapters;
    size_t pathBytes = 0;
    {
      tinyxml2::XMLDocument opf;
      if (!parseXML(root, 256 * 1024, opf, "Invalid package XML")) return false;
      auto pkg = child(&opf,"package"), manifest = child(pkg,"manifest"), spine = child(pkg,"spine");
      if (!manifest || !spine) return fail("Missing EPUB spine");
      auto language = child(child(pkg,"metadata"),"language");
      if (language && language->GetText()) {
        std::string tag = language->GetText();
        for (char& c : tag) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        englishHyphenation = tag == "en" || tag.compare(0,3,"en-") == 0;
      }
      auto title = child(child(pkg,"metadata"),"title");
      if (title) { text(title->GetText()); newline(); }
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
        chapters.push_back({std::move(path), !strcmp(attr(ref,"linear"),"no")});
      }
    }
    hasBodyText = false;
    // Preserve the primary reading order. Auxiliary notes remain accessible at
    // the end until the UI has hyperlink navigation.
    for (bool auxiliary : {false, true}) {
      for (const auto& chapter : chapters) {
        if (chapter.auxiliary != auxiliary) continue;
        tinyxml2::XMLDocument doc(true, tinyxml2::PEDANTIC_WHITESPACE);
        if (!parseXML(chapter.path, xml_input::MAX_BYTES, doc, "Chapter is not valid XHTML")) return false;
        auto html = child(&doc, "html");
        auto body = child(html, "body");
        if (!body) return fail("Chapter has no XHTML body");
        walk(body); newline();
        if (!ok) return false;
        if (tick) tick();
      }
    }
    if (!ok) return false;
    if (chapters.empty() || !hasBodyText) return fail("No readable EPUB text");
    output[used] = 0;
    return true;
  }
};
}
