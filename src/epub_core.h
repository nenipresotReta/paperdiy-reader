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
  // Optional unpacked EPUB source; SIZE_MAX means missing resource.
  std::function<size_t(const std::string&)> resourceSize;
  std::function<bool(const std::string&,void*,size_t)> resourceRead;
  uint32_t fileSize=0;
  std::vector<Entry> entries;
  std::vector<std::string> unusedObfuscatedFonts;
  std::string error;
  char* output;size_t capacity,used=0;
  bool ok=true;
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
    uint32_t cursor=start;
    for(uint16_t i=0;i<count;++i){
      uint8_t h[46];if(cursor-start>bytes||bytes-(cursor-start)<46||!exact(cursor,h,46)||u32(h)!=0x02014b50)return fail("Bad ZIP entry");
      uint16_t len=u16(h+28);uint32_t step=46u+len+u16(h+30)+u16(h+32);
      if(!len||len>1024||step>bytes-(cursor-start))return fail("Bad ZIP filename");
      Entry e;e.name.resize(len);if(!exact(cursor+46,&e.name[0],len))return fail("ZIP read failed");
      e.flags=u16(h+8);e.method=u16(h+10);e.crc=u32(h+16);e.compressed=u32(h+20);e.size=u32(h+24);e.offset=u32(h+42);
      entries.push_back(e);cursor+=step;if(tick)tick();
    }
    return true;
  }
  const Entry* find(const std::string& name){for(auto& e:entries)if(e.name==name)return &e;return nullptr;}
  std::unique_ptr<char,decltype(&free)> extract(const std::string& name,size_t max=1024*1024){
    std::unique_ptr<char,decltype(&free)> out(nullptr,free);
    for(const auto& path:unusedObfuscatedFonts)if(path==name){fail("Obfuscated resource cannot be read as text");return out;}
    if(resourceRead){
      if(name.empty()||name[0]=='/'||name.find('\\')!=std::string::npos){fail("Invalid EPUB resource path");return out;}
      size_t at=0;while(at<=name.size()){size_t end=name.find('/',at);if(end==std::string::npos)end=name.size();if(name.substr(at,end-at)==".."){fail("EPUB path escapes directory");return out;}at=end+1;}
      size_t size=resourceSize(name);
      if(size==SIZE_MAX){fail("EPUB resource missing");return out;}
      if(size>max){fail("Chapter too large");return out;}
      out.reset((char*)alloc(size+1));if(!out){fail("Not enough PSRAM");return out;}
      if(!resourceRead(name,out.get(),size)){fail("EPUB resource read failed");out.reset();return out;}
      out.get()[size]=0;if(tick)tick();return out;
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
      if(!compressed||!exact(start,compressed.get(),e->compressed)||lgfx_tinfl_decompress_mem_to_mem(out.get(),e->size,compressed.get(),e->compressed,0)!=e->size){fail("EPUB decompression failed");out.reset();return out;}
    }
    if(lgfx_mz_crc32(0,(const unsigned char*)out.get(),e->size)!=e->crc){fail("EPUB CRC mismatch");out.reset();return out;}
    out.get()[e->size]=0;if(tick)tick();return out;
  }
  std::string resolve(const std::string& base,const std::string& href){
    std::string path=href.substr(0,href.find('#')),decoded;
    for(size_t i=0;i<path.size();++i){if(path[i]=='%'&&i+2<path.size()){char hex[3]={path[i+1],path[i+2],0};char* end;long v=strtol(hex,&end,16);if(*end==0&&v){decoded+=char(v);i+=2;continue;}}decoded+=path[i];}
    if(decoded.empty()||decoded[0]=='/'||decoded.find(':')!=std::string::npos||decoded.find('\\')!=std::string::npos){fail("Invalid EPUB resource path");return "";}
    std::string combined=base.substr(0,base.find_last_of('/')==std::string::npos?0:base.find_last_of('/')+1)+decoded;
    std::vector<std::string> parts;size_t from=0;
    while(from<=combined.size()){size_t end=combined.find('/',from);if(end==std::string::npos)end=combined.size();auto s=combined.substr(from,end-from);if(s==".."){if(parts.empty()){fail("EPUB path escapes archive");return "";}parts.pop_back();}else if(!s.empty()&&s!=".")parts.push_back(s);from=end+1;}
    std::string result;for(auto& s:parts){if(!result.empty())result+='/';result+=s;}return result;
  }
  void append(char c){if(!ok)return;if(used>=capacity){fail("Book text exceeds reader capacity");return;}output[used++]=c;}
  void newline(){while(used&&output[used-1]==' ')--used;if(!used||output[used-1]!='\n')append('\n');else if(used>1&&output[used-2]!='\n')append('\n');}
  void text(const char* s){
    if(!s)return;
    while(*s&&ok){
      if(!strncmp(s,"&nbsp;",6)){if(used&&output[used-1]!=' '&&output[used-1]!='\n')append(' ');s+=6;continue;}
      char c=*s++;if(c=='\r'||c=='\n'||c=='\t'||c==' '){if(used&&output[used-1]!=' '&&output[used-1]!='\n')append(' ');}else append(c);
    }
  }
  void walk(tinyxml2::XMLNode* node,unsigned depth=0){
    if(!node||!ok)return;if(depth>100){fail("EPUB markup too deeply nested");return;}
    if(auto t=node->ToText()){text(t->Value());return;}
    auto e=node->ToElement();const char* name=e?local(e->Name()):"";
    if(!strcmp(name,"head")||!strcmp(name,"script")||!strcmp(name,"style")||!strcmp(name,"svg")||!strcmp(name,"img"))return;
    bool block=!strcmp(name,"p")||!strcmp(name,"div")||!strcmp(name,"br")||!strcmp(name,"li")||!strcmp(name,"section")||!strcmp(name,"article")||!strcmp(name,"tr")||!strcmp(name,"blockquote")||(name[0]=='h'&&name[1]>='1'&&name[1]<='6'&&!name[2]);
    if(block)newline();for(auto c=node->FirstChild();c;c=c->NextSibling())walk(c,depth+1);if(block)newline();
  }
  bool run(){
    if(!index())return false;
    if(resourceRead?resourceSize("META-INF/encryption.xml")!=SIZE_MAX:find("META-INF/encryption.xml")!=nullptr){
      auto xml=extract("META-INF/encryption.xml",65536);if(!xml)return false;
      tinyxml2::XMLDocument doc;if(doc.Parse(xml.get())!=tinyxml2::XML_SUCCESS)return fail("Invalid encryption XML");
      auto root=child(&doc,"encryption");if(!root)return fail("Invalid encryption XML");
      for(auto item=root->FirstChildElement();item;item=item->NextSiblingElement()){
        if(strcmp(local(item->Name()),"EncryptedData"))return fail("Encrypted EPUB resources unsupported");
        std::string algorithm=attr(child(item,"EncryptionMethod"),"Algorithm");
        std::string path=resolve("",attr(child(child(item,"CipherData"),"CipherReference"),"URI"));if(!ok)return false;
        auto ext=path.substr(path.find_last_of('.')==std::string::npos?path.size():path.find_last_of('.'));
        // Font obfuscation does not encrypt the book text. The reader uses its
        // own fonts; never load these resources, even if a spine points to one.
        if((algorithm!="http://www.idpf.org/2008/embedding"&&algorithm!="http://ns.adobe.com/pdf/enc#RC")||(ext!=".ttf"&&ext!=".otf"))return fail("Encrypted EPUB resources unsupported");
        unusedObfuscatedFonts.push_back(path);
      }
    }
    auto container=extract("META-INF/container.xml",65536);if(!container)return false;
    tinyxml2::XMLDocument cd;if(cd.Parse(container.get())!=tinyxml2::XML_SUCCESS)return fail("Invalid container XML");
    auto rootfile=child(child(child(&cd,"container"),"rootfiles"),"rootfile");std::string root=attr(rootfile,"full-path");if(root.empty())return fail("Missing EPUB rootfile");
    auto package=extract(root,256*1024);if(!package)return false;
    tinyxml2::XMLDocument opf;if(opf.Parse(package.get())!=tinyxml2::XML_SUCCESS)return fail("Invalid package XML");
    auto pkg=child(&opf,"package"),manifest=child(pkg,"manifest"),spine=child(pkg,"spine");if(!manifest||!spine)return fail("Missing EPUB spine");
    auto title=child(child(pkg,"metadata"),"title");if(title){text(title->GetText());newline();}
    unsigned chapters=0;
    for(auto ref=spine->FirstChildElement();ref&&ok;ref=ref->NextSiblingElement()){
      if(strcmp(local(ref->Name()),"itemref")||!strcmp(attr(ref,"linear"),"no"))continue;
      std::string href;for(auto item=manifest->FirstChildElement();item;item=item->NextSiblingElement())if(!strcmp(attr(item,"id"),attr(ref,"idref"))){href=attr(item,"href");break;}
      if(href.empty())return fail("Chapter ID missing");auto path=resolve(root,href);if(!ok)return false;
      auto chapter=extract(path);if(!chapter)return false;
      tinyxml2::XMLDocument doc;if(doc.Parse(chapter.get())!=tinyxml2::XML_SUCCESS)return fail("Chapter is not valid XHTML");
      walk(&doc);newline();++chapters;if(tick)tick();
    }
    if(!ok)return false;if(!chapters||!used)return fail("No readable EPUB text");output[used]=0;return true;
  }
};
}
