#pragma once
#include "cache_format.h"
#include <algorithm>
#include <cerrno>
#include <memory>
#include <sys/stat.h>

struct CacheDependency {
  String name;
  uint32_t size=UINT32_MAX,mtime=0;
  uint8_t digest[32]={};
};

struct BookCache {
  std::vector<CacheDependency> dependencies;
  String source,path;
  bool directory,enabled=true;
  uint8_t sourceHash[32];

  BookCache(const String& s,bool d):source(s),directory(d){
    mbedtls_sha256_ret((const uint8_t*)s.c_str(),s.length(),sourceHash,0);
    char hex[65];
    for(int i=0;i<32;++i)snprintf(hex+i*2,3,"%02x",sourceHash[i]);
    path=String("/.paperdiy-cache/")+hex+".bin";
  }

  bool stamp(const String& name,uint32_t& size,uint32_t& modified){
    String full=String("/sd")+source+(name.length()?"/"+name:"");
    struct stat st;
    if(stat(full.c_str(),&st)!=0){size=UINT32_MAX;modified=0;return errno==ENOENT;}
    if(!S_ISREG(st.st_mode)||st.st_size<0||uint64_t(st.st_size)>=UINT32_MAX)return false;
    size=st.st_size;modified=st.st_mtime;return true;
  }

  // Hash source bytes, not the extracted text. One bounded PSRAM buffer also
  // keeps large archives and directory resources off the task's small stack.
  bool fingerprint(CacheDependency& dep){
    if(!stamp(dep.name,dep.size,dep.mtime))return false;
    memset(dep.digest,0,sizeof(dep.digest));
    if(dep.size==UINT32_MAX)return true; // Remember absent optional resources.
    if(dep.size>64u*1024*1024)return false;
    File f=SD.open(source+(dep.name.length()?"/"+dep.name:""),"r");
    if(!f||f.size()!=dep.size)return false;
    Buffer bytes=buffer();
    if(!bytes)return false;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);mbedtls_sha256_starts_ret(&sha,0);
    bool ok=true;
    for(size_t at=0;at<dep.size;){
      size_t count=std::min(chunkSize(),size_t(dep.size)-at);
      if(f.read(bytes.get(),count)!=count){ok=false;break;}
      mbedtls_sha256_update_ret(&sha,bytes.get(),count);at+=count;cooperate(count);
    }
    mbedtls_sha256_finish_ret(&sha,dep.digest);mbedtls_sha256_free(&sha);f.close();
    uint32_t size,mtime;
    return ok&&stamp(dep.name,size,mtime)&&size==dep.size&&mtime==dep.mtime;
  }

  size_t observe(const std::string& name){
    if(!reader_cache::validResource(name.c_str(),name.size())){enabled=false;return SIZE_MAX;}
    CacheDependency current;current.name=name.c_str();
    if(current.name.length()!=name.size()){enabled=false;return SIZE_MAX;}
    if(!fingerprint(current)){enabled=false;return SIZE_MAX;}
    for(auto& dep:dependencies){
      if(dep.name!=current.name)continue;
      if(!same(dep,current))enabled=false;
      return current.size==UINT32_MAX?SIZE_MAX:current.size;
    }
    if(dependencies.size()>=reader_cache::MAX_DEPS)enabled=false;
    else dependencies.push_back(current);
    return current.size==UINT32_MAX?SIZE_MAX:current.size;
  }

  bool unchanged(bool legacy=false){
    for(const auto& dep:dependencies){
      if(legacy){
        uint32_t size,mtime;
        if(!stamp(dep.name,size,mtime)||size!=dep.size||mtime!=dep.mtime)return false;
      }else{
        CacheDependency current;current.name=dep.name;
        if(!fingerprint(current)||!same(dep,current))return false;
      }
    }
    return true;
  }

  bool transfer(File& f,void* data,size_t size,mbedtls_sha256_context& sha,bool write){
    uint8_t* p=(uint8_t*)data;
    for(size_t at=0;at<size;){
      size_t count=std::min(chunkSize(),size-at);
      if((write?f.write(p+at,count):f.read(p+at,count))!=count)return false;
      mbedtls_sha256_update_ret(&sha,p+at,count);at+=count;cooperate(count);
    }
    return true;
  }

  bool load(){
    recoveryNeeded=SD.exists(path)||SD.exists(path+".bak");
    uint32_t offset=0;
    bool loaded=loadFile(path,false,offset)||loadFile(path+".bak",false,offset);
    if(loaded)recoveryNeeded=false;
    return loaded;
  }

  // An unreadable cache may be the only copy of text needed to translate an
  // old bookmark. Never infer "new book" from a failed checksum or short header.
  bool recoveryRequired() const {return recoveryNeeded;}

  // Migration is deliberately read-only. The caller must retain its context
  // before re-importing, persist the mapped bookmark, then call save().
  // A false return may have overwritten scratch book bytes but publishes no
  // bookLength/pageOffsets/key. Legacy fingerprints are only size and mtime.
  bool loadLegacyForMigration(uint32_t& savedOffset){
    recoveryNeeded=SD.exists(path)||SD.exists(path+".bak");
    return loadFile(path,true,savedOffset)||loadFile(path+".bak",true,savedOffset);
  }

  bool save(){
    if(!enabled||dependencies.empty()||dependencies.size()>reader_cache::MAX_DEPS||
       !bookLength||bookLength>MAX_BOOK||pageOffsets.size()>reader_cache::MAX_PAGES||
       !reader_cache::validOffsets(pageOffsets.data(),pageOffsets.size(),bookLength)||!unchanged())return false;
    reader_cache::Header h{};
    h.magic=reader_cache::MAGIC;h.version=reader_cache::VERSION;h.layout=reader_cache::LAYOUT;
    h.directory=directory;h.textBytes=bookLength;h.pages=pageOffsets.size();h.deps=dependencies.size();
    for(const auto& dep:dependencies)h.metaBytes+=reader_cache::DEP_BYTES+dep.name.length();
    if(h.metaBytes>reader_cache::MAX_META)return false;
    memcpy(h.source,sourceHash,32);strncpy(h.bookmark,bookmarkKey.c_str(),15);
    uint64_t size=sizeof(h)+uint64_t(h.metaBytes)+uint64_t(h.pages)*4+h.textBytes;
    if(!reader_cache::validHeader(h,size,MAX_BOOK,sourceHash,directory))return false;
    if(!SD.exists("/.paperdiy-cache")&&!SD.mkdir("/.paperdiy-cache"))return false;
    String tmp=path+".tmp";
    if(SD.exists(tmp)&&!SD.remove(tmp))return false;
    File f=SD.open(tmp,"w");if(!f)return false;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);mbedtls_sha256_starts_ret(&sha,0);
    mbedtls_sha256_update_ret(&sha,(uint8_t*)&h,offsetof(reader_cache::Header,digest));
    bool ok=f.write((uint8_t*)&h,sizeof(h))==sizeof(h);
    for(auto& dep:dependencies){
      uint32_t fields[3]={(uint32_t)dep.name.length(),dep.size,dep.mtime};
      ok=ok&&transfer(f,fields,sizeof(fields),sha,true)&&transfer(f,dep.digest,sizeof(dep.digest),sha,true)&&
        transfer(f,(void*)dep.name.c_str(),dep.name.length(),sha,true);
    }
    ok=ok&&transfer(f,pageOffsets.data(),pageOffsets.size()*4,sha,true)&&transfer(f,book,bookLength,sha,true);
    mbedtls_sha256_finish_ret(&sha,h.digest);mbedtls_sha256_free(&sha);
    ok=ok&&f.seek(0)&&f.write((uint8_t*)&h,sizeof(h))==sizeof(h);f.flush();f.close();
    // Arduino File::flush has no error result: reread and verify before rotation.
    if(!ok||!storedIntact(tmp,&h)){SD.remove(tmp);return false;}
    if(!commit(tmp)){SD.remove(tmp);return false;}
    Serial.printf("CACHE_SAVED bytes=%u pages=%u\n",h.textBytes,h.pages);return true;
  }

private:
  bool recoveryNeeded=false;
  uint32_t lastYield=millis();
  size_t bytesSinceYield=0;
  void cooperate(size_t bytes){
    // Share the budget across small metadata writes and resource reads too.
    // delay(1) may cost a whole RTOS tick: avoid paying it for every 8 KiB.
    bytesSinceYield+=bytes;
    if(bytesSinceYield<128u*1024&&uint32_t(millis()-lastYield)<50)return;
    delay(1);lastYield=millis();bytesSinceYield=0;
  }
  static constexpr size_t chunkSize(){return 8192;}
  using Buffer=std::unique_ptr<uint8_t,decltype(&free)>;
  static Buffer buffer(){return Buffer((uint8_t*)ps_malloc(chunkSize()),free);}
  static bool same(const CacheDependency& a,const CacheDependency& b){
    return a.size==b.size&&a.mtime==b.mtime&&!memcmp(a.digest,b.digest,32);
  }

  bool loadFile(const String& candidate,bool legacy,uint32_t& savedOffset){
    File f=SD.open(candidate,"r");reader_cache::Header h{};
    if(!f||f.read((uint8_t*)&h,sizeof(h))!=sizeof(h))return false;
    bool valid=legacy?reader_cache::validLegacyHeader(h,f.size(),MAX_BOOK,sourceHash,directory):
      reader_cache::validHeader(h,f.size(),MAX_BOOK,sourceHash,directory);
    if(!valid)return false;
    Buffer meta((uint8_t*)ps_malloc(h.metaBytes),free);
    if(!meta)return false;
    std::vector<uint32_t> offsets(h.pages);
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);mbedtls_sha256_starts_ret(&sha,0);
    mbedtls_sha256_update_ret(&sha,(uint8_t*)&h,offsetof(reader_cache::Header,digest));
    bool ok=transfer(f,meta.get(),h.metaBytes,sha,false)&&transfer(f,offsets.data(),offsets.size()*4,sha,false)&&transfer(f,book,h.textBytes,sha,false);
    uint8_t digest[32];mbedtls_sha256_finish_ret(&sha,digest);mbedtls_sha256_free(&sha);f.close();
    if(!ok||memcmp(digest,h.digest,32)||!reader_cache::validOffsets(offsets.data(),offsets.size(),h.textBytes))return false;
    dependencies.clear();size_t pos=0;
    const size_t recordBytes=legacy?reader_cache::LEGACY_DEP_BYTES:reader_cache::DEP_BYTES;
    for(uint32_t i=0;i<h.deps;++i){
      if(h.metaBytes-pos<recordBytes)return false;
      uint32_t fields[3];memcpy(fields,meta.get()+pos,sizeof(fields));pos+=sizeof(fields);
      CacheDependency dep;dep.size=fields[1];dep.mtime=fields[2];
      if(!legacy){memcpy(dep.digest,meta.get()+pos,sizeof(dep.digest));pos+=sizeof(dep.digest);}
      if(fields[0]>h.metaBytes-pos||!reader_cache::validResource((char*)meta.get()+pos,fields[0])||(!directory&&fields[0]))return false;
      dep.name=String((char*)meta.get()+pos,fields[0]);
      if(dep.name.length()!=fields[0])return false;
      pos+=fields[0];dependencies.push_back(dep);
    }
    if(pos!=h.metaBytes||!unchanged(legacy)){dependencies.clear();return false;}
    uint32_t offset=0;
    if(prefs.isKey(h.bookmark)){
      offset=prefs.getUInt(h.bookmark,UINT32_MAX);
      if(offset==UINT32_MAX)return false;
    }
    if(offset>=h.textBytes){if(legacy)return false;offset=0;}
    bookLength=h.textBytes;book[bookLength]=0;pageOffsets.swap(offsets);
    bookmarkKey=h.bookmark;bookIdentity=source;savedOffset=offset;
    if(!legacy)prepareReaderFont();
    auto page=std::upper_bound(pageOffsets.begin(),pageOffsets.end(),offset);
    pageNumber=page==pageOffsets.begin()?0:page-pageOffsets.begin()-1;
    Serial.printf(legacy?"CACHE_LEGACY bytes=%u pages=%u\n":"CACHE_HIT bytes=%u pages=%u\n",h.textBytes,h.pages);
    if(legacy)Serial.printf("CACHE_LEGACY_LAYOUT value=%08x\n",h.layout);
    return true;
  }

  bool storedIntact(const String& candidate,const reader_cache::Header* expected=nullptr){
    File f=SD.open(candidate,"r");reader_cache::Header h{};
    if(!f||f.read((uint8_t*)&h,sizeof(h))!=sizeof(h))return false;
    if(expected&&memcmp(&h,expected,sizeof(h)))return false;
    if(!reader_cache::validHeader(h,f.size(),MAX_BOOK,sourceHash,directory)&&
       !reader_cache::validLegacyHeader(h,f.size(),MAX_BOOK,sourceHash,directory))return false;
    Buffer bytes=buffer();if(!bytes)return false;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);mbedtls_sha256_starts_ret(&sha,0);
    mbedtls_sha256_update_ret(&sha,(uint8_t*)&h,offsetof(reader_cache::Header,digest));
    size_t remaining=f.size()-sizeof(h);bool ok=true;
    while(remaining){
      size_t count=std::min(chunkSize(),remaining);
      if(f.read(bytes.get(),count)!=count){ok=false;break;}
      mbedtls_sha256_update_ret(&sha,bytes.get(),count);remaining-=count;cooperate(count);
    }
    uint8_t digest[32];mbedtls_sha256_finish_ret(&sha,digest);mbedtls_sha256_free(&sha);
    f.close();return ok&&!memcmp(digest,h.digest,32);
  }

  bool commit(const String& tmp){
    String backup=path+".bak";
    if(SD.exists(path)&&SD.exists(backup)){
      // An interrupted earlier commit can leave both. Keep whichever old
      // image has an intact checksum until the new image has its final name.
      if(storedIntact(path)){if(!SD.remove(backup))return false;}
      else if(storedIntact(backup)){if(!SD.remove(path))return false;}
      else if(!SD.remove(backup))return false; // Neither copy is usable.
    }
    if(SD.exists(path)&&!SD.rename(path,backup))return false;
    if(!SD.rename(tmp,path)){
      if(!SD.exists(path)&&SD.exists(backup))SD.rename(backup,path);
      return false; // A failed rollback leaves the readable .bak in place.
    }
    // Keep the previous generation for firmware rollback and migration recovery.
    // It is rotated only after the next replacement has passed verification.
    return true;
  }
};
