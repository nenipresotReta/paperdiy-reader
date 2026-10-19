#pragma once
#include "cache_format.h"
#include <memory>
#include <sys/stat.h>
struct CacheDependency {String name;uint32_t size,mtime;};
struct BookCache {
  std::vector<CacheDependency> dependencies;
  String source,path;bool directory,enabled=true;
  uint8_t sourceHash[32];
  BookCache(const String& s,bool d):source(s),directory(d){
    mbedtls_sha256_ret((const uint8_t*)s.c_str(),s.length(),sourceHash,0);
    char hex[65];for(int i=0;i<32;++i)snprintf(hex+i*2,3,"%02x",sourceHash[i]);
    path=String("/.paperdiy-cache/")+hex+".bin";
  }
  bool stamp(const String& name,uint32_t& size,uint32_t& modified){
    String full=String("/sd")+source+(name.length()?"/"+name:"");struct stat st;
    if(stat(full.c_str(),&st)!=0){size=UINT32_MAX;modified=0;return errno==ENOENT;}
    if(!S_ISREG(st.st_mode)||st.st_size<0||uint64_t(st.st_size)>=UINT32_MAX)return false;
    size=st.st_size;modified=st.st_mtime;return true;
  }
  size_t observe(const std::string& name){
    if(!reader_cache::validResource(name.c_str(),name.size())){enabled=false;return SIZE_MAX;}
    uint32_t size,mtime;if(!stamp(name.c_str(),size,mtime)){enabled=false;return SIZE_MAX;}
    bool found=false;for(auto& dep:dependencies)if(dep.name==name.c_str()){found=true;if(dep.size!=size||dep.mtime!=mtime)enabled=false;break;}
    if(!found){if(dependencies.size()>=reader_cache::MAX_DEPS)enabled=false;else dependencies.push_back({String(name.c_str()),size,mtime});}
    return size==UINT32_MAX?SIZE_MAX:size;
  }
  bool unchanged(){for(auto& dep:dependencies){uint32_t size,mtime;if(!stamp(dep.name,size,mtime)||size!=dep.size||mtime!=dep.mtime)return false;}return true;}
  bool transfer(File& f,void* data,size_t size,mbedtls_sha256_context& sha,bool write){
    uint8_t* p=(uint8_t*)data;
    for(size_t at=0;at<size;){size_t n=std::min(size_t(8192),size-at);
      if((write?f.write(p+at,n):f.read(p+at,n))!=n)return false;
      mbedtls_sha256_update_ret(&sha,p+at,n);at+=n;delay(1);
    }return true;
  }
  bool load(){
    if(!SD.exists(path))return false;
    File f=SD.open(path,"r");reader_cache::Header h{};
    if(!f||f.read((uint8_t*)&h,sizeof(h))!=sizeof(h)||!reader_cache::validHeader(h,f.size(),MAX_BOOK,sourceHash,directory))return false;
    std::unique_ptr<uint8_t,decltype(&free)> meta((uint8_t*)ps_malloc(h.metaBytes+1),free);
    if(!meta)return false;
    std::vector<uint32_t> offsets(h.pages);
    mbedtls_sha256_context sha;mbedtls_sha256_init(&sha);mbedtls_sha256_starts_ret(&sha,0);
    mbedtls_sha256_update_ret(&sha,(uint8_t*)&h,offsetof(reader_cache::Header,digest));
    bool ok=transfer(f,meta.get(),h.metaBytes,sha,false)&&transfer(f,offsets.data(),offsets.size()*4,sha,false)&&transfer(f,book,h.textBytes,sha,false);
    uint8_t digest[32];mbedtls_sha256_finish_ret(&sha,digest);mbedtls_sha256_free(&sha);f.close();
    if(!ok||memcmp(digest,h.digest,32)||!reader_cache::validOffsets(offsets.data(),offsets.size(),h.textBytes))return false;
    dependencies.clear();size_t pos=0;
    for(uint32_t i=0;i<h.deps;++i){
      if(h.metaBytes-pos<12)return false;uint32_t fields[3];memcpy(fields,meta.get()+pos,12);pos+=12;
      if(fields[0]>h.metaBytes-pos||!reader_cache::validResource((char*)meta.get()+pos,fields[0])||(!directory&&fields[0]))return false;
      String name;name.reserve(fields[0]);for(uint32_t k=0;k<fields[0];++k)name+=char(meta.get()[pos+k]);pos+=fields[0];
      dependencies.push_back({name,fields[1],fields[2]});
    }
    if(pos!=h.metaBytes||!unchanged()){dependencies.clear();return false;}
    bookLength=h.textBytes;book[bookLength]=0;pageOffsets.swap(offsets);bookmarkKey=h.bookmark;bookIdentity=source;prepareReaderFont();
    uint32_t offset=prefs.getUInt(bookmarkKey.c_str(),0);if(offset>=bookLength)offset=0;
    auto page=std::upper_bound(pageOffsets.begin(),pageOffsets.end(),offset);pageNumber=page==pageOffsets.begin()?0:page-pageOffsets.begin()-1;
    Serial.printf("CACHE_HIT bytes=%u pages=%u\n",h.textBytes,h.pages);return true;
  }
  bool save(){
    if(!enabled||dependencies.empty()||!unchanged()||!reader_cache::validOffsets(pageOffsets.data(),pageOffsets.size(),bookLength))return false;
    reader_cache::Header h{};h.magic=reader_cache::MAGIC;h.version=reader_cache::VERSION;h.layout=reader_cache::LAYOUT;h.directory=directory;h.textBytes=bookLength;h.pages=pageOffsets.size();h.deps=dependencies.size();
    for(auto& dep:dependencies)h.metaBytes+=12+dep.name.length();
    if(h.metaBytes>reader_cache::MAX_META||h.pages>reader_cache::MAX_PAGES)return false;
    memcpy(h.source,sourceHash,32);strncpy(h.bookmark,bookmarkKey.c_str(),15);
    if(!SD.exists("/.paperdiy-cache")&&!SD.mkdir("/.paperdiy-cache"))return false;
    String tmp=path+".tmp";if(SD.exists(tmp)&&!SD.remove(tmp))return false;File f=SD.open(tmp,"w");if(!f)return false;
    mbedtls_sha256_context sha;mbedtls_sha256_init(&sha);mbedtls_sha256_starts_ret(&sha,0);mbedtls_sha256_update_ret(&sha,(uint8_t*)&h,offsetof(reader_cache::Header,digest));
    bool ok=f.write((uint8_t*)&h,sizeof(h))==sizeof(h);
    for(auto& dep:dependencies){uint32_t fields[3]={(uint32_t)dep.name.length(),dep.size,dep.mtime};ok=ok&&transfer(f,fields,12,sha,true)&&transfer(f,(void*)dep.name.c_str(),dep.name.length(),sha,true);}
    ok=ok&&transfer(f,pageOffsets.data(),pageOffsets.size()*4,sha,true)&&transfer(f,book,bookLength,sha,true);
    mbedtls_sha256_finish_ret(&sha,h.digest);mbedtls_sha256_free(&sha);
    ok=ok&&f.seek(0)&&f.write((uint8_t*)&h,sizeof(h))==sizeof(h);f.flush();f.close();
    if(!ok){SD.remove(tmp);return false;}
    if(SD.exists(path)&&!SD.remove(path)){SD.remove(tmp);return false;}
    if(!SD.rename(tmp,path)){SD.remove(tmp);return false;}
    Serial.printf("CACHE_SAVED bytes=%u pages=%u\n",h.textBytes,h.pages);return true;
  }
};
