#pragma once
#include "console.h"
#include "cache_format.h"
#include "document_format.h"
#include "reader_typography.h"
#include "storage_validation.h"
#include "epub_reader.h"
#include "prepared_epub.h"
#include "sd_content.h"
#include "managed_progress.h"
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
  bool directory,enabled=true,ioFailed=false;
  void readFailure(){ioFailed=true;storage_validation::invalidate();}
  uint8_t sourceHash[32];
  bool languageKnown=false;
  uint8_t preparedDigest[32]={};
  struct Work {uint64_t sourceBytes=0,cacheReadBytes=0,cacheWriteBytes=0;uint32_t sourceMs=0,reusedFingerprints=0,reusedGraphs=0;} work;
  void logWork() const {
    Console.printf("CACHE_WORK source_bytes=%llu cache_read_bytes=%llu cache_write_bytes=%llu source_ms=%u verified_reuse=%u graph_reuse=%u\n",
      (unsigned long long)work.sourceBytes,(unsigned long long)work.cacheReadBytes,
      (unsigned long long)work.cacheWriteBytes,unsigned(work.sourceMs),unsigned(work.reusedFingerprints),unsigned(work.reusedGraphs));
  }

  BookCache(const String& s,bool d):source(s),directory(d){
    enabled=!managed_progress::isPushPath(s.c_str());
    mbedtls_sha256_ret((const uint8_t*)s.c_str(),s.length(),sourceHash,0);
    char hex[65];
    for(int i=0;i<32;++i)snprintf(hex+i*2,3,"%02x",sourceHash[i]);
    path=String("/.paperdiy-cache/")+hex+".bin";
  }

  bool stamp(const String& name,uint32_t& size,uint32_t& modified){
    String full=String("/sd")+source+(name.length()?"/"+name:"");
    struct stat st;
    if(stat(full.c_str(),&st)!=0){size=UINT32_MAX;modified=0;if(errno==ENOENT)return true;readFailure();return false;}
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
    const String full=source+(dep.name.length()?"/"+dep.name:"");
    uint8_t identity[32];mbedtls_sha256_ret((const uint8_t*)full.c_str(),full.length(),identity,0);
    if(storage_validation::find(identity,dep.size,dep.mtime,dep.digest)){++work.reusedFingerprints;return true;}
    const uint64_t leaseId=storage_validation::lease();const uint32_t started=millis();
    File f=SD.open(full,"r");
    if(!f){
      // A deleted source is a content change, not evidence of bus failure.
      uint32_t present,modified;
      if(stamp(dep.name,present,modified)&&present!=UINT32_MAX)readFailure();
      storage_validation::invalidate();return false;
    }
    if(f.size()!=dep.size){storage_validation::invalidate();return false;}
    Buffer bytes=buffer();
    if(!bytes)return false;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);mbedtls_sha256_starts_ret(&sha,0);
    bool ok=true;
    for(size_t at=0;at<dep.size;){
      size_t count=std::min(chunkSize(),size_t(dep.size)-at);
      size_t read=f.read(bytes.get(),count);work.sourceBytes+=read;
      if(read!=count){readFailure();ok=false;break;}
      mbedtls_sha256_update_ret(&sha,bytes.get(),count);at+=count;cooperate(count);
    }
    mbedtls_sha256_finish_ret(&sha,dep.digest);mbedtls_sha256_free(&sha);f.close();
    uint32_t size,mtime;
    work.sourceMs+=uint32_t(millis()-started);
    ok=ok&&stamp(dep.name,size,mtime)&&size==dep.size&&mtime==dep.mtime&&
      (!leaseId||leaseId==storage_validation::lease());
    if(ok)storage_validation::remember(leaseId,identity,dep.size,dep.mtime,dep.digest);
    else storage_validation::invalidate();
    return ok;
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
    uint8_t graphDigest[32];const uint64_t leaseId=storage_validation::lease();
    if(!legacy&&leaseId){
      mbedtls_sha256_context sha;mbedtls_sha256_init(&sha);mbedtls_sha256_starts_ret(&sha,0);
      for(const auto& dep:dependencies){
        uint32_t fields[3]={uint32_t(dep.name.length()),dep.size,dep.mtime};
        mbedtls_sha256_update_ret(&sha,(const uint8_t*)fields,sizeof(fields));
        mbedtls_sha256_update_ret(&sha,(const uint8_t*)dep.name.c_str(),dep.name.length());
        mbedtls_sha256_update_ret(&sha,dep.digest,32);
      }
      mbedtls_sha256_finish_ret(&sha,graphDigest);mbedtls_sha256_free(&sha);
      if(storage_validation::verifiedGraph(sourceHash,graphDigest)){++work.reusedGraphs;return true;}
    }
    for(const auto& dep:dependencies){
      // A changed stamp rejects cheaply. Matching stamps require a full hash
      // unless this exact source was already verified under an exclusive lease.
      uint32_t size,mtime;
      if(!stamp(dep.name,size,mtime)||size!=dep.size||mtime!=dep.mtime)return false;
      if(!legacy){
        CacheDependency current;current.name=dep.name;
        if(!fingerprint(current)||!same(dep,current))return false;
      }
    }
    if(!legacy&&leaseId){
      if(leaseId!=storage_validation::lease()){storage_validation::invalidate();return false;}
      storage_validation::rememberGraph(leaseId,sourceHash,graphDigest);
    }
    return true;
  }

  bool transfer(File& f,void* data,size_t size,mbedtls_sha256_context& sha,bool write){
    uint8_t* p=(uint8_t*)data;
    for(size_t at=0;at<size;){
      size_t count=std::min(chunkSize(),size-at);
      size_t done=write?f.write(p+at,count):f.read(p+at,count);
      (write?work.cacheWriteBytes:work.cacheReadBytes)+=done;
      if(done!=count){if(write)storage_validation::invalidate();else readFailure();return false;}
      mbedtls_sha256_update_ret(&sha,p+at,count);at+=count;cooperate(count);
    }
    return true;
  }

  bool load(){
    if(!enabled)return false;
    recoveryNeeded=SD.exists(path)||SD.exists(path+".bak");
    uint32_t offset=0;
    bool loaded=loadFile(path,false,offset)||loadFile(path+".bak",false,offset);
    if(loaded)recoveryNeeded=false;
    return loaded;
  }

  // Reuse authenticated extraction bytes after a geometry-only change. The
  // caller must rebuild page offsets before display; the old cache is retained
  // until the exact saved anchor is persisted and the replacement is verified.
  bool loadTextForReflow(){
    if(!enabled)return false;
    recoveryNeeded=SD.exists(path)||SD.exists(path+".bak");
    uint32_t offset=0;
    return loadFile(path,false,offset,true)||loadFile(path+".bak",false,offset,true);
  }

  bool loadPrepared(){
    if(!enabled)return false;
    if(directory||document_format::detect(source.c_str())!=document_format::Kind::Epub||
       dependencies.size()!=1||dependencies[0].name.length())return false;
    return loadPreparedFile(source+".pdt")||loadPreparedFile(source+".pdt.bak");
  }

  bool preparedChanged(){
    if(!enabled)return false;
    if(directory||document_format::detect(source.c_str())!=document_format::Kind::Epub||dependencies.size()!=1)return false;
    File f=SD.open(source+".pdt","r");if(!f)return false;
    uint8_t bytes[prepared_epub::HEADER_BYTES];prepared_epub::Header h;
    if(f.size()<sizeof(bytes))return false;
    if(f.read(bytes,sizeof(bytes))!=sizeof(bytes)){readFailure();return false;}
    if(!prepared_epub::decode(bytes,sizeof(bytes),f.size(),h)||
       h.sourceBytes!=dependencies[0].size||memcmp(h.sourceDigest,dependencies[0].digest,32))return false;
    uint8_t digest[32];mbedtls_sha256_ret(bytes,sizeof(bytes),digest,0);
    return memcmp(digest,preparedDigest,32)!=0;
  }

  // An unreadable cache may be the only copy of text needed to translate an
  // old bookmark. Never infer "new book" from a failed checksum or short header.
  bool recoveryRequired() const {return recoveryNeeded;}

  // Migration is deliberately read-only. The caller must retain its context
  // before re-importing, persist the mapped bookmark, then call save().
  // A false return may have overwritten scratch book bytes but publishes no
  // bookLength/pageOffsets/key. Legacy fingerprints are only size and mtime.
  bool loadLegacyForMigration(uint32_t& savedOffset){
    if(!enabled)return false;
    recoveryNeeded=SD.exists(path)||SD.exists(path+".bak");
    return loadFile(path,true,savedOffset)||loadFile(path+".bak",true,savedOffset);
  }

  bool save(){
    if(!enabled||bookEpubTransientFallback)return false;
    if(reader_content::active()){
      if(!reader_content::pin(0,bookLength)||(bookDocument.ready()&&!bookDocument.validateSpan(0,bookLength)))return false;
      reader_content::clear();
    }
    bool success=false;
    struct InvalidateOnFailure {bool& success;~InvalidateOnFailure(){if(!success)storage_validation::invalidate();}} guard{success};
    if(!enabled||dependencies.empty()||dependencies.size()>reader_cache::MAX_DEPS||
       !bookLength||bookLength>MAX_BOOK||pageOffsets.size()>reader_cache::MAX_PAGES||
       !reader_cache::validOffsets(pageOffsets.data(),pageOffsets.size(),bookLength)||!unchanged())return false;
    reader_cache::Header h{};
    const bool markdown=document_format::detect(source.c_str())==document_format::Kind::Markdown;
    const auto language=markdown?text_language::Language::EnglishUS:bookEnglishHyphens?bookLanguage:text_language::Language::None;
    const size_t documentBytes=bookDocument.ready()?bookDocument.encodedSize():0;
    Buffer document(documentBytes?(uint8_t*)ps_malloc(documentBytes):nullptr,free);
    if(documentBytes&&(!document||!bookDocument.encodeTo(document.get(),documentBytes)))return false;
    const bool epub=directory||document_format::detect(source.c_str())==document_format::Kind::Epub;
    reader_cache::Extension extension{reader_cache::TEXT_REVISION,uint32_t(documentBytes),uint32_t(language),epub?reader_cache::EPUB_SEMANTIC_REVISION:0};
    memcpy(extension.preparedDigest,preparedDigest,32);
    h.magic=reader_cache::MAGIC;h.version=reader_cache::VERSION;h.layout=reader_cache::layoutFor(bookEnglishHyphens,markdown,reader_typography::currentStyle().flags(),language==text_language::Language::EnglishGB,documentBytes!=0);
    h.directory=directory;h.textBytes=bookLength;h.pages=pageOffsets.size();h.deps=dependencies.size();
    h.metaBytes=sizeof(extension)+documentBytes;
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
    ok=ok&&transfer(f,&extension,sizeof(extension),sha,true);
    for(auto& dep:dependencies){
      uint32_t fields[3]={(uint32_t)dep.name.length(),dep.size,dep.mtime};
      ok=ok&&transfer(f,fields,sizeof(fields),sha,true)&&transfer(f,dep.digest,sizeof(dep.digest),sha,true)&&
        transfer(f,(void*)dep.name.c_str(),dep.name.length(),sha,true);
    }
    ok=ok&&transfer(f,document.get(),documentBytes,sha,true);
    ok=ok&&transfer(f,pageOffsets.data(),pageOffsets.size()*4,sha,true)&&transfer(f,book,bookLength,sha,true);
    mbedtls_sha256_finish_ret(&sha,h.digest);mbedtls_sha256_free(&sha);
    ok=ok&&f.seek(0)&&f.write((uint8_t*)&h,sizeof(h))==sizeof(h);f.flush();f.close();
    // Arduino File::flush has no error result: reread and verify before rotation.
    if(!ok||!storedIntact(tmp,&h)){SD.remove(tmp);return false;}
    if(!commit(tmp)){SD.remove(tmp);return false;}
    success=true;Console.printf("CACHE_SAVED bytes=%u pages=%u\n",h.textBytes,h.pages);return true;
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

  bool loadPreparedFile(const String& candidate){
    File f=SD.open(candidate,"r");if(!f)return false;
    uint8_t bytes[prepared_epub::HEADER_BYTES];prepared_epub::Header header;
    if(f.size()<sizeof(bytes))return false;
    if(f.read(bytes,sizeof(bytes))!=sizeof(bytes)){readFailure();return false;}
    if(!prepared_epub::decode(bytes,sizeof(bytes),f.size(),header))return false;
    const auto& dep=dependencies[0];
    if(header.sourceBytes!=dep.size||memcmp(header.sourceDigest,dep.digest,32))return false;
    const uint64_t leaseId=storage_validation::lease();
    Buffer meta(header.metadataBytes?(uint8_t*)ps_malloc(header.metadataBytes):nullptr,free);
    if(header.metadataBytes&&!meta)return false;
    mbedtls_sha256_context sha;mbedtls_sha256_init(&sha);mbedtls_sha256_starts_ret(&sha,0);
    bool ok=transfer(f,book,header.textBytes,sha,false)&&transfer(f,meta.get(),header.metadataBytes,sha,false);
    uint8_t digest[32];mbedtls_sha256_finish_ret(&sha,digest);mbedtls_sha256_free(&sha);f.close();
    epub_document::Document decoded;
    if(!ok||memcmp(digest,header.payloadDigest,32)||!epub_document::Document::validText(book,header.textBytes)||
       (header.metadataBytes&&!decoded.decode(meta.get(),header.metadataBytes,book,header.textBytes))||
       (leaseId&&leaseId!=storage_validation::lease())||!unchanged())return false;
    reader_content::clear();bookLength=header.textBytes;book[bookLength]=0;pageOffsets.clear();
    bookLanguage=text_language::Language(header.flags);bookEnglishHyphens=bookLanguage!=text_language::Language::None;
    bookDocument=std::move(decoded);bookEpubTransientFallback=false;languageKnown=true;
    mbedtls_sha256_ret(bytes,sizeof(bytes),preparedDigest,0);
    Console.printf("PREPARED_EPUB bytes=%u metadata=%u language=%u\n",header.textBytes,header.metadataBytes,header.flags);
    return true;
  }

  bool loadFile(const String& candidate,bool legacy,uint32_t& savedOffset,bool textOnly=false,bool allowWindow=true){
    File f=SD.open(candidate,"r");reader_cache::Header h{};
    if(!f)return false;
    if(f.size()<sizeof(h))return false;
    if(f.read((uint8_t*)&h,sizeof(h))!=sizeof(h)){readFailure();return false;}
    bool valid=(legacy||textOnly)?reader_cache::validMigrationHeader(h,f.size(),MAX_BOOK,sourceHash,directory):
      reader_cache::validHeader(h,f.size(),MAX_BOOK,sourceHash,directory);
    if(!valid)return false;
    const bool markdown=document_format::detect(source.c_str())==document_format::Kind::Markdown;
    const bool epub=directory||document_format::detect(source.c_str())==document_format::Kind::Epub;
    if(textOnly){
      if((h.version!=2&&h.version!=3)||!reader_cache::compatibleText(h.layout,markdown))return false;
    }else if(!legacy&&!reader_cache::matchesLayout(h.layout,markdown,reader_typography::currentStyle().flags()))return false;
    Buffer meta((uint8_t*)ps_malloc(h.metaBytes),free);
    if(!meta)return false;
    std::vector<uint32_t> offsets(h.pages);
    std::unique_ptr<SdContentSource> content;
    if(allowWindow&&!legacy&&!textOnly&&document_format::detect(source.c_str())==document_format::Kind::Epub){
      content.reset(new(std::nothrow) SdContentSource(candidate,book));
      if(content&&!content->load(h)){
        const bool failed=content->ioFailed();work.cacheReadBytes+=content->readBytes();content.reset();
        if(failed){readFailure();return false;}
      }
    }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);mbedtls_sha256_starts_ret(&sha,0);
    mbedtls_sha256_update_ret(&sha,(uint8_t*)&h,content?sizeof(h):offsetof(reader_cache::Header,digest));
    bool ok=transfer(f,meta.get(),h.metaBytes,sha,false)&&transfer(f,offsets.data(),offsets.size()*4,sha,false)&&
      (content||transfer(f,book,h.textBytes,sha,false));
    uint8_t digest[32];mbedtls_sha256_finish_ret(&sha,digest);mbedtls_sha256_free(&sha);f.close();
    if(!ok)return false;
    if(memcmp(digest,content?content->manifest().prefixDigest:h.digest,32))
      return content?loadFile(candidate,legacy,savedOffset,textOnly,false):false;
    if(!reader_cache::validOffsets(offsets.data(),offsets.size(),h.textBytes))return false;
    if(!content){
      if(!epub_document::Document::validText(book,h.textBytes))return false;
      for(const auto at:offsets)if((uint8_t(book[at])&0xc0)==0x80)return false;
    }
    dependencies.clear();size_t pos=0;
    reader_cache::Extension extension{reader_cache::textRevision(h.layout),0,uint32_t((h.layout&12u)!=4u?text_language::Language::EnglishUS:text_language::Language::None),0};
    if(h.version==3){
      memcpy(&extension,meta.get(),sizeof(extension));pos=sizeof(extension);
      if(extension.semanticRevision>reader_cache::EPUB_SEMANTIC_REVISION||(!epub&&extension.semanticRevision)||
         extension.documentBytes>epub_document::Document::MAX_BYTES||
         extension.documentBytes>h.metaBytes-pos||extension.language>uint32_t(text_language::Language::EnglishGB))return false;
      if(!legacy&&extension.textRevision!=reader_cache::TEXT_REVISION)return false;
      if(currentLayoutMetadataMismatch(h,extension))return false;
    }
    const bool currentSemantics=extension.semanticRevision==(epub?reader_cache::EPUB_SEMANTIC_REVISION:0);
    if(!legacy&&!currentSemantics)return false;
    // Current geometry with old semantics still supplies read-only anchors.
    // The semantic version must be checked before excluding a matching layout.
    if(legacy&&reader_cache::currentLayout(h.layout)&&
       (reader_cache::markdownLayout(h.layout)!=markdown||
        (currentSemantics&&reader_cache::matchesLayout(h.layout,markdown,reader_typography::currentStyle().flags()))))return false;
    const bool v1=h.version==1;
    const size_t recordBytes=v1?reader_cache::LEGACY_DEP_BYTES:reader_cache::DEP_BYTES;
    for(uint32_t i=0;i<h.deps;++i){
      if(h.metaBytes-pos<recordBytes)return false;
      uint32_t fields[3];memcpy(fields,meta.get()+pos,sizeof(fields));pos+=sizeof(fields);
      CacheDependency dep;dep.size=fields[1];dep.mtime=fields[2];
      if(!v1){memcpy(dep.digest,meta.get()+pos,sizeof(dep.digest));pos+=sizeof(dep.digest);}
      if(fields[0]>h.metaBytes-pos||!reader_cache::validResource((char*)meta.get()+pos,fields[0])||(!directory&&fields[0]))return false;
      dep.name=String((char*)meta.get()+pos,fields[0]);
      if(dep.name.length()!=fields[0])return false;
      pos+=fields[0];dependencies.push_back(dep);
    }
    epub_document::Document decoded;
    if(extension.documentBytes!=h.metaBytes-pos||
       (extension.documentBytes&&!decoded.decode(meta.get()+pos,extension.documentBytes,book,h.textBytes,!content))||
       !unchanged(v1)){dependencies.clear();return false;}
    uint32_t offset=0;
    if(prefs.isKey(h.bookmark)){
      offset=prefs.getUInt(h.bookmark,UINT32_MAX);
      if(offset==UINT32_MAX)return false;
    }
    if(offset>=h.textBytes)return false; // Never replace an invalid saved position with page zero.
    if(content){
      content->validateSource();
      const auto at=std::upper_bound(offsets.begin(),offsets.end(),offset);
      const size_t selected=at==offsets.begin()?0:size_t(at-offsets.begin()-1);
      const auto range=reader_content::pageRange(offsets,selected,h.textBytes);
      const bool pinned=content->pin(range.first,range.second)&&
        (uint8_t(book[offsets[selected]])&0xc0)!=0x80&&
        (selected+1==offsets.size()||(uint8_t(book[offsets[selected+1]])&0xc0)!=0x80)&&
        (!decoded.ready()||decoded.validateSpan(content->begin(),content->end()));
      work.cacheReadBytes+=content->readBytes();
      if(!pinned){
        if(content->ioFailed()){readFailure();return false;}
        // A pathological page/span or damaged optional block index may still
        // use the independently authenticated complete-cache path.
        return loadFile(candidate,legacy,savedOffset,textOnly,false);
      }
    }
    reader_content::clear();if(content)reader_content::install(std::move(content));
    bookLength=h.textBytes;book[bookLength]=0;pageOffsets.swap(offsets);
    bookmarkKey=h.bookmark;bookIdentity=source;savedOffset=offset;
    if(!legacy){
      bookLanguage=text_language::Language(extension.language);languageKnown=h.version==3;
      memcpy(preparedDigest,extension.preparedDigest,32);
      bookEnglishHyphens=bookLanguage!=text_language::Language::None;
      bookDocument=std::move(decoded);bookEpubTransientFallback=false;prepareReaderFont();
    }
    auto page=std::upper_bound(pageOffsets.begin(),pageOffsets.end(),offset);
    pageNumber=page==pageOffsets.begin()?0:page-pageOffsets.begin()-1;
    retainReadingAnchor(offset);
    Console.printf(legacy?"CACHE_LEGACY bytes=%u pages=%u\n":textOnly?"CACHE_TEXT_HIT bytes=%u old_pages=%u\n":"CACHE_HIT bytes=%u pages=%u\n",h.textBytes,h.pages);
    if(legacy)Console.printf("CACHE_LEGACY_LAYOUT value=%08x\n",h.layout);
    return true;
  }

  static bool currentLayoutMetadataMismatch(const reader_cache::Header& h,const reader_cache::Extension& extension){
    if(!reader_cache::knownV3Layout(h.layout))return false;
    return bool(h.layout&1024u)!=(extension.documentBytes!=0)||
      bool(h.layout&512u)!=(extension.language==uint32_t(text_language::Language::EnglishGB))||
      ((h.layout&12u)==4u)!=(extension.language==uint32_t(text_language::Language::None));
  }

  bool storedIntact(const String& candidate,const reader_cache::Header* expected=nullptr){
    File f=SD.open(candidate,"r");reader_cache::Header h{};
    if(!f||f.size()<sizeof(h))return false;
    if(f.read((uint8_t*)&h,sizeof(h))!=sizeof(h)){readFailure();return false;}
    if(expected&&memcmp(&h,expected,sizeof(h)))return false;
    if(!reader_cache::validHeader(h,f.size(),MAX_BOOK,sourceHash,directory)&&
       !reader_cache::validMigrationHeader(h,f.size(),MAX_BOOK,sourceHash,directory))return false;
    Buffer bytes=buffer();if(!bytes)return false;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);mbedtls_sha256_starts_ret(&sha,0);
    mbedtls_sha256_update_ret(&sha,(uint8_t*)&h,offsetof(reader_cache::Header,digest));
    size_t remaining=f.size()-sizeof(h);bool ok=true;
    while(remaining){
      size_t count=std::min(chunkSize(),remaining);
      size_t read=f.read(bytes.get(),count);work.cacheReadBytes+=read;
      if(read!=count){readFailure();ok=false;break;}
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
