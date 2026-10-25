#pragma once
#include "content_source.h"
#include "content_manifest.h"
#include "epub_reader.h"
#include "storage_validation.h"
#include <algorithm>
#include <memory>

class SdContentSource:public reader_content::Source {
  using Buffer=std::unique_ptr<uint8_t,decltype(&free)>;
  enum class Stage:uint8_t {None,Pending,Reading,Utf8,Semantic,Complete,Failed,Cancelled};
  String path;
  char* text;
  content_manifest::Header header{};
  Buffer hashes{nullptr,free},scratch{nullptr,free};
  uint8_t loaded[content_manifest::MAX_BLOCKS/8]={};
  uint64_t leaseId=0;
  size_t first_=0,last_=0;
  bool ioFailed_=false;
  bool leaseInvalidated_=false;
  uint64_t readBytes_=0;

  Stage stage_=Stage::None;
  reader_content::PrefetchFailure failure_=reader_content::PrefetchFailure::None;
  size_t cursor_=0,loadedCount_=0;
  size_t utf8At_=0;
  uint8_t utf8Need_=0;
  uint32_t utf8Cp_=0,utf8Min_=0;
  uint64_t prefetchReadBytes_=0;
  uint64_t token_=0;
  File prefetchFile;
  epub_document::Document::FullValidation semanticValidation_{};

  static uint64_t nextToken(){static uint64_t value=0;return ++value;}
  bool has(size_t block)const{return loaded[block/8]&(1u<<(block%8));}
  size_t countLoaded()const{
    size_t count=0;
    for(size_t block=0;block<header.blocks;++block)if(has(block))++count;
    return count;
  }
  void closePrefetch(){prefetchFile.close();prefetchFile=File();}
  bool failIO(){ioFailed_=true;leaseInvalidated_=true;stage_=Stage::Failed;
    failure_=reader_content::PrefetchFailure::Read;closePrefetch();
    storage_validation::invalidate();return false;}
  bool failPrefetch(reader_content::PrefetchFailure failure){
    if(stage_==Stage::Complete)return false;
    failure_=failure;stage_=Stage::Failed;closePrefetch();
    if(failure==reader_content::PrefetchFailure::Read||
       failure==reader_content::PrefetchFailure::Hash){
      ioFailed_=true;leaseInvalidated_=true;storage_validation::invalidate();
    }else if(failure==reader_content::PrefetchFailure::Lease){
      leaseInvalidated_=true;
    }
    // UTF-8/semantic failures are content errors, not storage errors. Keep
    // the lease so a read-only diagnostic can still inspect the source.
    return false;
  }
  bool stepRead();
  bool stepUtf8();
  bool stepSemantic();
 public:
  SdContentSource(const String& name,char* destination)
    :path(name),text(destination),token_(nextToken()){}
  const content_manifest::Header& manifest()const{return header;}
  bool ioFailed()const{return ioFailed_;}
  uint64_t readBytes()const{return readBytes_;}
  bool load(const reader_cache::Header& cache){
    stage_=Stage::None;failure_=reader_content::PrefetchFailure::None;
    cursor_=0;loadedCount_=0;utf8At_=0;utf8Need_=0;
    prefetchReadBytes_=0;closePrefetch();memset(loaded,0,sizeof(loaded));
    leaseInvalidated_=false;
    semanticValidation_=epub_document::Document::FullValidation{};
    File file=SD.open(path+".chunks","r");
    if(!file||file.size()<sizeof(header)||file.size()>content_manifest::MAX_BYTES)return false;
    const size_t got=file.read((uint8_t*)&header,sizeof(header));readBytes_+=got;
    if(got!=sizeof(header))return failIO();
    if(!content_manifest::valid(header,file.size(),cache))return false;
    hashes.reset((uint8_t*)ps_malloc(size_t(header.blocks)*32));scratch.reset((uint8_t*)ps_malloc(content_manifest::BLOCK_BYTES));
    if(!hashes||!scratch)return false;
    mbedtls_sha256_context sha;mbedtls_sha256_init(&sha);mbedtls_sha256_starts_ret(&sha,0);
    mbedtls_sha256_update_ret(&sha,(const uint8_t*)&header,offsetof(content_manifest::Header,digest));
    bool ok=true;
    for(size_t at=0;at<size_t(header.blocks)*32;){
      const size_t count=std::min(size_t(content_manifest::BLOCK_BYTES),size_t(header.blocks)*32-at);
      const size_t read=file.read(hashes.get()+at,count);readBytes_+=read;
      if(read!=count){failIO();ok=false;break;}
      mbedtls_sha256_update_ret(&sha,hashes.get()+at,count);at+=count;
    }
    uint8_t digest[32];mbedtls_sha256_finish_ret(&sha,digest);mbedtls_sha256_free(&sha);
    return ok&&!memcmp(digest,header.digest,32);
  }
  // Called only after the metadata/pages and source dependency graph match.
  void validateSource(){leaseId=storage_validation::lease();}
  size_t begin()const override{return first_;}
  size_t end()const override{return last_;}
  size_t length()const override{return header.textBytes;}
  void restoreSpan(size_t first,size_t last)override{
    if(first>last||last>header.textBytes)return;
    if(first<last)for(size_t block=first/content_manifest::BLOCK_BYTES;block<=(last-1)/content_manifest::BLOCK_BYTES;++block)if(!has(block))return;
    first_=first;last_=last;
  }
  bool pin(size_t first,size_t last)override{
    if(first>=last||last>header.textBytes||!hashes||!scratch)return false;
    const size_t firstBlock=first/content_manifest::BLOCK_BYTES?first/content_manifest::BLOCK_BYTES-1:0;
    const size_t lastBlock=std::min(size_t(header.blocks-1),(last-1)/content_manifest::BLOCK_BYTES+1);
    // A failed background fill may still redraw the already authenticated
    // visible span, but it must not perform another on-demand read.
    if(stage_==Stage::Failed){
      for(size_t block=firstBlock;block<=lastBlock;++block)if(!has(block)){
        if(failure_==reader_content::PrefetchFailure::Lease||
           leaseId!=storage_validation::lease())leaseInvalidated_=true;
        return false;
      }
    }
    File file;size_t work=0;uint32_t yielded=millis();
    for(size_t block=firstBlock;block<=lastBlock;++block){
      if(has(block))continue;
      const uint64_t lease=storage_validation::lease();
      if(!leaseId||leaseId!=lease){leaseInvalidated_=true;return false;}
      if(!file){file=SD.open(path,"r");if(!file)return failIO();}
      const size_t at=block*content_manifest::BLOCK_BYTES;
      const size_t count=std::min(size_t(content_manifest::BLOCK_BYTES),size_t(header.textBytes)-at);
      if(!file.seek(size_t(header.prefixBytes)+at))return failIO();
      const size_t got=file.read(scratch.get(),count);readBytes_+=got;
      if(got!=count)return failIO();
      uint8_t digest[32];mbedtls_sha256_ret(scratch.get(),count,digest,0);
      if(memcmp(digest,hashes.get()+block*32,32))return false;
      if(leaseId!=storage_validation::lease()){leaseInvalidated_=true;return false;}
      // Never overwrite a visible byte with an unverified read.
      memcpy(text+at,scratch.get(),count);loaded[block/8]|=uint8_t(1u<<(block%8));++loadedCount_;work+=count;
      if(work>=128u*1024||uint32_t(millis()-yielded)>=50){delay(1);work=0;yielded=millis();}
    }
    // Trim only boundary fragments belonging to neighboring unloaded blocks.
    // Every requested byte must remain inside the published complete-codepoint
    // span; UTF-8 validation never consults a byte outside loaded blocks.
    size_t begin=firstBlock*content_manifest::BLOCK_BYTES;
    const size_t end=std::min(size_t(header.textBytes),(lastBlock+1)*content_manifest::BLOCK_BYTES);
    if(begin){unsigned skipped=0;while(begin<end&&(uint8_t(text[begin])&0xc0)==0x80&&skipped<3){++begin;++skipped;}}
    size_t at=begin,validEnd=begin;
    while(at<end){
      const size_t start=at;const uint8_t c=uint8_t(text[at++]);unsigned n=0;uint32_t cp=0,min=0;
      if(c<128){if(!c)return false;validEnd=at;continue;}
      if(c>=0xc2&&c<=0xdf){n=1;cp=c&31;min=128;}else if(c>=0xe0&&c<=0xef){n=2;cp=c&15;min=2048;}else if(c>=0xf0&&c<=0xf4){n=3;cp=c&7;min=65536;}else return false;
      if(n>end-at){if(end!=header.textBytes){validEnd=start;break;}return false;}
      while(n--){const uint8_t b=uint8_t(text[at++]);if((b&0xc0)!=0x80)return false;cp=(cp<<6)|(b&63);}
      if(cp<min||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return false;validEnd=at;
    }
    if(begin>first||validEnd<last)return false;
    first_=begin;last_=validEnd;return true;
  }

  // ---- optional authenticated background fill -------------------------
  bool prefetchSupported()const override{
    return header.blocks&&header.textBytes&&text&&hashes&&scratch;
  }
  void beginPrefetch()override{
    if(stage_!=Stage::None&&stage_!=Stage::Cancelled)return;
    leaseInvalidated_=false;
    const bool leaseBad=!leaseId||leaseId!=storage_validation::lease();
    if(!prefetchSupported()||leaseBad){
      failure_=reader_content::PrefetchFailure::Lease;stage_=Stage::Failed;
      if(leaseBad)leaseInvalidated_=true;
      return;
    }
    failure_=reader_content::PrefetchFailure::None;stage_=Stage::Pending;
    cursor_=0;loadedCount_=countLoaded();utf8At_=0;utf8Need_=0;
    prefetchReadBytes_=0;semanticValidation_=epub_document::Document::FullValidation{};
  }
  void abortPrefetch(reader_content::PrefetchFailure failure)override{
    if(stage_==Stage::Complete)return;
    failPrefetch(failure);
  }
  void cancelPrefetch()override{
    if(stage_==Stage::Complete||stage_==Stage::Failed)return;
    closePrefetch();stage_=Stage::Cancelled;
  }
  bool prefetchStep()override{
    if(stage_==Stage::Pending)stage_=Stage::Reading;
    if(stage_==Stage::Reading)return stepRead();
    if(stage_==Stage::Utf8)return stepUtf8();
    if(stage_==Stage::Semantic)return stepSemantic();
    return false;
  }
  bool prefetchPending()const override{
    return stage_==Stage::Pending||stage_==Stage::Reading||
      stage_==Stage::Utf8||stage_==Stage::Semantic;
  }
  bool prefetchFailed()const override{return stage_==Stage::Failed;}
  bool prefetchComplete()const override{return stage_==Stage::Complete;}
  bool leaseInvalidated()const override{return leaseInvalidated_;}
  reader_content::PrefetchState prefetchState()const override{
    switch(stage_){
    case Stage::Pending:return reader_content::PrefetchState::Pending;
    case Stage::Reading:return reader_content::PrefetchState::Reading;
    case Stage::Utf8:return reader_content::PrefetchState::VerifyingUtf8;
    case Stage::Semantic:return reader_content::PrefetchState::VerifyingSemantics;
    case Stage::Complete:return reader_content::PrefetchState::Complete;
    case Stage::Failed:return reader_content::PrefetchState::Failed;
    case Stage::Cancelled:return reader_content::PrefetchState::Cancelled;
    default:return reader_content::PrefetchState::NotNeeded;
    }
  }
  reader_content::PrefetchFailure prefetchFailure()const override{return failure_;}
  uint64_t prefetchToken()const override{return token_;}
  size_t prefetchLoadedBlocks()const override{return loadedCount_;}
  size_t prefetchTotalBlocks()const override{return header.blocks;}
  uint64_t prefetchReadBytes()const override{return prefetchReadBytes_;}

};

inline bool SdContentSource::stepRead(){
  if(loadedCount_>=header.blocks){
    closePrefetch();stage_=Stage::Utf8;utf8At_=0;utf8Need_=0;return true;
  }
  if(!leaseId||leaseId!=storage_validation::lease())
    return failPrefetch(reader_content::PrefetchFailure::Lease);

  // Skip already loaded blocks, but cap one bitmap scan so a foreground pin
  // cannot turn the background step into an unbounded walk.
  size_t block=cursor_;unsigned scanned=0;
  while(block<header.blocks&&scanned<32&&has(block)){++block;++scanned;}
  if(block<header.blocks&&has(block)){cursor_=block;return true;}
  if(block>=header.blocks){
    if(loadedCount_>=header.blocks){
      closePrefetch();stage_=Stage::Utf8;utf8At_=0;utf8Need_=0;return true;
    }
    return failPrefetch(reader_content::PrefetchFailure::Hash);
  }
  cursor_=block;

  if(!prefetchFile){
    prefetchFile=SD.open(path,"r");
    if(!prefetchFile)return failPrefetch(reader_content::PrefetchFailure::Read);
  }
  const size_t at=block*content_manifest::BLOCK_BYTES;
  const size_t count=std::min(size_t(content_manifest::BLOCK_BYTES),
    size_t(header.textBytes)-at);
  if(!prefetchFile.seek(size_t(header.prefixBytes)+at))
    return failPrefetch(reader_content::PrefetchFailure::Read);
  const size_t got=prefetchFile.read(scratch.get(),count);
  readBytes_+=got;prefetchReadBytes_+=got;
  if(got!=count)return failPrefetch(reader_content::PrefetchFailure::Read);
  uint8_t digest[32];mbedtls_sha256_ret(scratch.get(),count,digest,0);
  if(memcmp(digest,hashes.get()+block*32,32))
    return failPrefetch(reader_content::PrefetchFailure::Hash);
  if(leaseId!=storage_validation::lease())
    return failPrefetch(reader_content::PrefetchFailure::Lease);
  // Only verified bytes are published; the visible begin/end span is untouched.
  memcpy(text+at,scratch.get(),count);
  loaded[block/8]|=uint8_t(1u<<(block%8));++loadedCount_;cursor_=block+1;
  if(loadedCount_>=header.blocks){
    closePrefetch();stage_=Stage::Utf8;utf8At_=0;utf8Need_=0;
  }
  return true;
}

inline bool SdContentSource::stepUtf8(){
  if(utf8At_>=header.textBytes){
    if(utf8Need_)return failPrefetch(reader_content::PrefetchFailure::Utf8);
    stage_=Stage::Semantic;return true;
  }
  const size_t end=std::min(size_t(header.textBytes),utf8At_+65536);
  while(utf8At_<end){
    const uint8_t c=uint8_t(text[utf8At_++]);
    if(utf8Need_){
      if((c&0xc0)!=0x80)return failPrefetch(reader_content::PrefetchFailure::Utf8);
      utf8Cp_=(utf8Cp_<<6)|(c&63);
      if(--utf8Need_==0&&(utf8Cp_<utf8Min_||utf8Cp_>0x10ffff||
          (utf8Cp_>=0xd800&&utf8Cp_<=0xdfff)))
        return failPrefetch(reader_content::PrefetchFailure::Utf8);
      continue;
    }
    if(c<0x80){if(!c)return failPrefetch(reader_content::PrefetchFailure::Utf8);continue;}
    if(c>=0xc2&&c<=0xdf){utf8Need_=1;utf8Cp_=c&31;utf8Min_=128;}
    else if(c>=0xe0&&c<=0xef){utf8Need_=2;utf8Cp_=c&15;utf8Min_=2048;}
    else if(c>=0xf0&&c<=0xf4){utf8Need_=3;utf8Cp_=c&7;utf8Min_=65536;}
    else return failPrefetch(reader_content::PrefetchFailure::Utf8);
  }
  if(utf8At_>=header.textBytes){
    if(utf8Need_)return failPrefetch(reader_content::PrefetchFailure::Utf8);
    stage_=Stage::Semantic;
  }
  return true;
}

inline bool SdContentSource::stepSemantic(){
  if(!bookDocument.ready()||bookDocument.size()!=header.textBytes)
    return failPrefetch(reader_content::PrefetchFailure::Semantic);
  if(!bookDocument.validateFullStep(semanticValidation_,1024))
    return failPrefetch(reader_content::PrefetchFailure::Semantic);
  if(!semanticValidation_.done)return true;
  closePrefetch();stage_=Stage::Complete;return true;
}
