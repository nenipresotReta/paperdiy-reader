#pragma once
#include "console.h"
#include "sd_cache.h"
#include <new>

// Application-task-owned cache publication. Each step transfers at most 8 KiB;
// readback and old-generation validation are incremental too. The caller must
// destroy this object before replacing the book buffer or handing SD elsewhere.
class DeferredCacheWrite {
  using Buffer=std::unique_ptr<uint8_t,decltype(&free)>;
  enum class Stage {Start,Write,Seal,OpenTemp,VerifyTemp,OpenPrimary,VerifyPrimary,
                    OpenBackup,VerifyBackup,Commit,OpenManifest,WriteManifest,SealManifest,
                    OpenManifestCheck,VerifyManifest,CommitManifest,Done,Failed};
  BookCache& cache;
  reader_cache::Header header{},checking{};
  Buffer prefix{nullptr,free},scratch{nullptr,free},manifest{nullptr,free};
  size_t manifestBytes=0;
  content_manifest::Header blockHeader{};
  size_t prefixBytes=0,position=0,remaining=0;
  const char* text=nullptr;
  uint64_t leaseId=0;
  File file;
  mbedtls_sha256_context sha;
  Stage stage=Stage::Failed;
  bool primaryGood=false,backupGood=false;
  String temporary;

  bool trusted()const{return leaseId&&storage_validation::lease()==leaseId;}
  void fail(){file.close();stage=Stage::Failed;}
  bool beginCheck(const String& name,bool exact){
    file=SD.open(name,"r");
    if(!file||file.size()<sizeof(checking))return false;
    if(file.read((uint8_t*)&checking,sizeof(checking))!=sizeof(checking)){cache.readFailure();return false;}
    if(exact&&memcmp(&checking,&header,sizeof(header)))return false;
    if(!reader_cache::validHeader(checking,file.size(),MAX_BOOK,cache.sourceHash,cache.directory)&&
       !reader_cache::validMigrationHeader(checking,file.size(),MAX_BOOK,cache.sourceHash,cache.directory))return false;
    remaining=file.size()-sizeof(checking);
    mbedtls_sha256_starts_ret(&sha,0);
    mbedtls_sha256_update_ret(&sha,(const uint8_t*)&checking,offsetof(reader_cache::Header,digest));
    return true;
  }
  // -1: invalid/read failure, 0: more bytes, 1: complete and checked.
  int checkStep(){
    const size_t count=std::min(size_t(8192),remaining);
    const size_t got=file.read(scratch.get(),count);cache.work.cacheReadBytes+=got;
    if(got!=count){cache.readFailure();file.close();return -1;}
    mbedtls_sha256_update_ret(&sha,scratch.get(),count);remaining-=count;
    if(remaining)return 0;
    uint8_t digest[32];mbedtls_sha256_finish_ret(&sha,digest);file.close();
    return memcmp(digest,checking.digest,32)?-1:1;
  }
 public:
  explicit DeferredCacheWrite(BookCache& owner):cache(owner),temporary(owner.path+".tmp"){
    mbedtls_sha256_init(&sha);
  }
  ~DeferredCacheWrite(){
    file.close();mbedtls_sha256_free(&sha);
    // Never touch a different/remounted card while cleaning an interrupted job.
    if(stage!=Stage::Done&&trusted()){
      if(SD.exists(temporary))SD.remove(temporary);
      if(SD.exists(cache.path+".chunks.tmp"))SD.remove(cache.path+".chunks.tmp");
    }
  }
  DeferredCacheWrite(const DeferredCacheWrite&)=delete;
  DeferredCacheWrite& operator=(const DeferredCacheWrite&)=delete;
  bool begin(const std::vector<uint32_t>& pages){
    if(stage!=Stage::Failed||bookEpubTransientFallback||!cache.enabled||cache.dependencies.empty()||
       cache.dependencies.size()>reader_cache::MAX_DEPS||!bookLength||bookLength>MAX_BOOK||
       pages.size()>reader_cache::MAX_PAGES||!reader_cache::validOffsets(pages.data(),pages.size(),bookLength))return false;
    if(reader_content::active()){
      reader_content::cancelPrefetch();
      if(!reader_content::pin(0,bookLength)||(bookDocument.ready()&&!bookDocument.validateSpan(0,bookLength)))return false;
      reader_content::clear();
    }
    leaseId=storage_validation::lease();if(!leaseId)return false;
    const bool markdown=document_format::detect(cache.source.c_str())==document_format::Kind::Markdown;
    const auto language=markdown?text_language::Language::EnglishUS:bookEnglishHyphens?bookLanguage:text_language::Language::None;
    const size_t documentBytes=bookDocument.ready()?bookDocument.encodedSize():0;
    const bool epub=cache.directory||document_format::detect(cache.source.c_str())==document_format::Kind::Epub;
    reader_cache::Extension ext{reader_cache::TEXT_REVISION,uint32_t(documentBytes),uint32_t(language),epub?reader_cache::EPUB_SEMANTIC_REVISION:0};
    memcpy(ext.preparedDigest,cache.preparedDigest,32);
    header.magic=reader_cache::MAGIC;header.version=reader_cache::VERSION;
    header.layout=reader_cache::layoutFor(bookEnglishHyphens,markdown,reader_typography::currentStyle().flags(),language==text_language::Language::EnglishGB,documentBytes!=0);
    header.directory=cache.directory;header.textBytes=bookLength;header.pages=pages.size();header.deps=cache.dependencies.size();
    header.metaBytes=sizeof(ext)+documentBytes;
    for(const auto& dep:cache.dependencies)header.metaBytes+=reader_cache::DEP_BYTES+dep.name.length();
    if(header.metaBytes>reader_cache::MAX_META)return false;
    memcpy(header.source,cache.sourceHash,32);strncpy(header.bookmark,bookmarkKey.c_str(),15);
    const uint64_t size=sizeof(header)+uint64_t(header.metaBytes)+uint64_t(header.pages)*4+header.textBytes;
    if(!reader_cache::validHeader(header,size,MAX_BOOK,cache.sourceHash,cache.directory))return false;
    prefixBytes=header.metaBytes+pages.size()*4;
    prefix.reset((uint8_t*)ps_malloc(prefixBytes));scratch.reset((uint8_t*)ps_malloc(8192));
    if(!prefix||!scratch)return false;
    if(document_format::detect(cache.source.c_str())==document_format::Kind::Epub){
      blockHeader.textBytes=header.textBytes;blockHeader.prefixBytes=uint32_t(sizeof(header)+prefixBytes);
      blockHeader.blocks=uint32_t(content_manifest::blocks(header.textBytes));
      manifestBytes=sizeof(blockHeader)+size_t(blockHeader.blocks)*32;
      manifest.reset((uint8_t*)ps_malloc(manifestBytes)); // Optional acceleration only.
    }
    uint8_t* at=prefix.get();memcpy(at,&ext,sizeof(ext));at+=sizeof(ext);
    for(const auto& dep:cache.dependencies){
      const uint32_t fields[3]={uint32_t(dep.name.length()),dep.size,dep.mtime};
      memcpy(at,fields,sizeof(fields));at+=sizeof(fields);memcpy(at,dep.digest,32);at+=32;
      memcpy(at,dep.name.c_str(),dep.name.length());at+=dep.name.length();
    }
    if(documentBytes&&!bookDocument.encodeTo(at,documentBytes))return false;
    at+=documentBytes;memcpy(at,pages.data(),pages.size()*4);
    text=book;position=0;stage=Stage::Start;return true;
  }
  bool done()const{return stage==Stage::Done;}
  bool failed()const{return stage==Stage::Failed;}
  void step(){
    if(done()||failed())return;
    if(!trusted()||book!=text||bookLength!=header.textBytes||bookmarkKey!=header.bookmark){fail();return;}
    switch(stage){
    case Stage::Start:
      if((!SD.exists("/.paperdiy-cache")&&!SD.mkdir("/.paperdiy-cache"))||
         (SD.exists(temporary)&&!SD.remove(temporary))){fail();return;}
      file=SD.open(temporary,"w");
      if(!file||file.write((const uint8_t*)&header,sizeof(header))!=sizeof(header)){fail();return;}
      mbedtls_sha256_starts_ret(&sha,0);
      mbedtls_sha256_update_ret(&sha,(const uint8_t*)&header,offsetof(reader_cache::Header,digest));
      stage=Stage::Write;return;
    case Stage::Write:{
      const bool metadata=position<prefixBytes;
      const size_t offset=metadata?position:position-prefixBytes;
      const size_t count=std::min(size_t(8192),(metadata?prefixBytes:size_t(header.textBytes))-offset);
      const uint8_t* bytes=metadata?prefix.get()+offset:(const uint8_t*)text+offset;
      const size_t wrote=file.write(bytes,count);cache.work.cacheWriteBytes+=wrote;
      if(wrote!=count){storage_validation::invalidate();fail();return;}
      mbedtls_sha256_update_ret(&sha,bytes,count);
      if(manifest&&!metadata)mbedtls_sha256_ret(bytes,count,
        manifest.get()+sizeof(blockHeader)+(offset/content_manifest::BLOCK_BYTES)*32,0);
      position+=count;
      if(position==prefixBytes+header.textBytes)stage=Stage::Seal;
      return;
    }
    case Stage::Seal:
      mbedtls_sha256_finish_ret(&sha,header.digest);
      if(!file.seek(0)||file.write((const uint8_t*)&header,sizeof(header))!=sizeof(header)){fail();return;}
      file.flush();file.close();
      if(manifest){
        memcpy(blockHeader.cacheDigest,header.digest,32);
        mbedtls_sha256_starts_ret(&sha,0);
        mbedtls_sha256_update_ret(&sha,(const uint8_t*)&header,sizeof(header));
        mbedtls_sha256_update_ret(&sha,prefix.get(),prefixBytes);
        mbedtls_sha256_finish_ret(&sha,blockHeader.prefixDigest);
        mbedtls_sha256_starts_ret(&sha,0);
        mbedtls_sha256_update_ret(&sha,(const uint8_t*)&blockHeader,offsetof(content_manifest::Header,digest));
        mbedtls_sha256_update_ret(&sha,manifest.get()+sizeof(blockHeader),manifestBytes-sizeof(blockHeader));
        mbedtls_sha256_finish_ret(&sha,blockHeader.digest);memcpy(manifest.get(),&blockHeader,sizeof(blockHeader));
      }
      prefix.reset();stage=Stage::OpenTemp;return;
    case Stage::OpenTemp:
      if(!beginCheck(temporary,true)){fail();return;}stage=Stage::VerifyTemp;return;
    case Stage::VerifyTemp:{
      const int result=checkStep();if(result<0){fail();return;}
      if(result>0)stage=Stage::OpenPrimary;return;
    }
    case Stage::OpenPrimary:
      if(!(SD.exists(cache.path)&&SD.exists(cache.path+".bak"))){stage=Stage::Commit;return;}
      if(beginCheck(cache.path,false))stage=Stage::VerifyPrimary;
      else {file.close();stage=Stage::OpenBackup;}return;
    case Stage::VerifyPrimary:{
      const int result=checkStep();if(result){primaryGood=result>0;stage=primaryGood?Stage::Commit:Stage::OpenBackup;}return;
    }
    case Stage::OpenBackup:
      if(beginCheck(cache.path+".bak",false))stage=Stage::VerifyBackup;
      else {file.close();stage=Stage::Commit;}return;
    case Stage::VerifyBackup:{
      const int result=checkStep();if(result){backupGood=result>0;stage=Stage::Commit;}return;
    }
    case Stage::Commit:{
      if(!trusted()||cache.ioFailed){fail();return;}
      const String backup=cache.path+".bak";
      if(SD.exists(cache.path)&&SD.exists(backup)){
        if(!SD.remove(primaryGood||!backupGood?backup:cache.path)){fail();return;}
      }
      if(SD.exists(cache.path)&&!SD.rename(cache.path,backup)){fail();return;}
      if(!SD.rename(temporary,cache.path)){
        if(!SD.exists(cache.path)&&SD.exists(backup))SD.rename(backup,cache.path);
        fail();return;
      }
      stage=manifest?Stage::OpenManifest:Stage::Done;
      Console.printf("CACHE_BACKGROUND_SAVED bytes=%u pages=%u\n",header.textBytes,header.pages);return;
    }
    case Stage::OpenManifest:
      if(SD.exists(cache.path+".chunks.tmp")&&!SD.remove(cache.path+".chunks.tmp")){fail();return;}
      file=SD.open(cache.path+".chunks.tmp","w");if(!file){fail();return;}
      position=0;stage=Stage::WriteManifest;return;
    case Stage::WriteManifest:{
      const size_t count=std::min(size_t(8192),manifestBytes-position);
      const size_t wrote=file.write(manifest.get()+position,count);cache.work.cacheWriteBytes+=wrote;
      if(wrote!=count){storage_validation::invalidate();fail();return;}
      position+=count;if(position==manifestBytes)stage=Stage::SealManifest;return;
    }
    case Stage::SealManifest:
      file.flush();file.close();stage=Stage::OpenManifestCheck;return;
    case Stage::OpenManifestCheck:
      file=SD.open(cache.path+".chunks.tmp","r");
      if(!file||file.size()!=manifestBytes){fail();return;}
      position=0;stage=Stage::VerifyManifest;return;
    case Stage::VerifyManifest:{
      const size_t count=std::min(size_t(8192),manifestBytes-position);
      const size_t got=file.read(scratch.get(),count);cache.work.cacheReadBytes+=got;
      if(got!=count){cache.readFailure();fail();return;}
      if(memcmp(scratch.get(),manifest.get()+position,count)){fail();return;}
      position+=count;if(position==manifestBytes){file.close();stage=Stage::CommitManifest;}return;
    }
    case Stage::CommitManifest:
      if((SD.exists(cache.path+".chunks")&&!SD.remove(cache.path+".chunks"))||
         !SD.rename(cache.path+".chunks.tmp",cache.path+".chunks")){fail();return;}
      stage=Stage::Done;return;
    default:return;
    }
  }
};
