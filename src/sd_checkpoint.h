#pragma once
#include "console.h"
#include "sd_cache.h"
#include "pagination_checkpoint.h"
#include "epub_metadata_limits.h"

// Bind partial layout work to freshly validated source bytes, the complete
// extraction and the serialized semantic document. Source leases never persist
// in this file: a new session must validate its own source before comparing it.
inline bool checkpointIdentity(BookCache& cache,uint32_t layout,pagination_checkpoint::Identity& identity){
  using Buffer=std::unique_ptr<uint8_t,decltype(&free)>;
  const uint64_t lease=storage_validation::lease();
  if(!lease||!cache.enabled||cache.dependencies.empty()||cache.dependencies.size()>reader_cache::MAX_DEPS||
     !bookLength||bookLength>MAX_BOOK||bookEpubTransientFallback||!cache.unchanged())return false;
  const size_t metadataBytes=bookDocument.ready()?bookDocument.encodedSize():0;
  if(metadataBytes>epub_metadata_limits::MAX_BYTES)return false;
  Buffer metadata(metadataBytes?(uint8_t*)ps_malloc(metadataBytes):nullptr,free);
  if(metadataBytes&&(!metadata||!bookDocument.encodeTo(metadata.get(),metadataBytes)))return false;
  pagination_checkpoint::Identity result;result.layout=layout;
  result.textRevision=reader_cache::TEXT_REVISION;result.textBytes=uint32_t(bookLength);
  memcpy(result.source,cache.sourceHash,32);
  mbedtls_sha256_context sha;mbedtls_sha256_init(&sha);mbedtls_sha256_starts_ret(&sha,0);
  for(const auto& dep:cache.dependencies){
    const uint32_t fields[3]={uint32_t(dep.name.length()),dep.size,dep.mtime};
    mbedtls_sha256_update_ret(&sha,(const uint8_t*)fields,sizeof(fields));
    mbedtls_sha256_update_ret(&sha,(const uint8_t*)dep.name.c_str(),dep.name.length());
    mbedtls_sha256_update_ret(&sha,dep.digest,32);
  }
  mbedtls_sha256_finish_ret(&sha,result.dependencies);mbedtls_sha256_starts_ret(&sha,0);
  const bool epub=cache.directory||document_format::detect(cache.source.c_str())==document_format::Kind::Epub;
  const uint32_t framing[3]={uint32_t(bookLength),uint32_t(metadataBytes),epub?reader_cache::EPUB_SEMANTIC_REVISION:0};
  mbedtls_sha256_update_ret(&sha,(const uint8_t*)framing,sizeof(framing));
  uint32_t yielded=millis();size_t work=0;
  auto hash=[&](const uint8_t* bytes,size_t size){
    for(size_t at=0;at<size;){
      const size_t count=std::min(size_t(8192),size-at);
      mbedtls_sha256_update_ret(&sha,bytes+at,count);at+=count;work+=count;
      if(work>=128u*1024||uint32_t(millis()-yielded)>=50){delay(1);work=0;yielded=millis();}
    }
  };
  hash((const uint8_t*)book,bookLength);hash(metadata.get(),metadataBytes);
  mbedtls_sha256_finish_ret(&sha,result.content);mbedtls_sha256_free(&sha);
  if(lease!=storage_validation::lease())return false;
  identity=result;return true;
}

// Application-task-owned two-slot journal. Reads and writes each transfer at
// most 8 KiB per step. Opening, sealing and flush remain synchronous SD calls.
// Cancellation only closes a handle: it neither deletes the last valid slot nor
// writes to a card whose lease has ended. Complete-cache generations are never
// opened here. A failed/torn optional slot is simply unusable.
class PaginationCheckpoint {
  using Header=pagination_checkpoint::Header;
  using Identity=pagination_checkpoint::Identity;
  using Buffer=std::unique_ptr<uint8_t,decltype(&free)>;
  enum class Stage {OpenSlot,ReadSlot,Ready,OpenWrite,Write,Seal,OpenVerify,Verify,Failed};
  BookCache& cache;
  Identity identity;
  Header header{},candidate{};
  Buffer bytes{nullptr,free},scratch{nullptr,free},recovered{nullptr,free};
  File file;
  mbedtls_sha256_context sha;
  Stage stage=Stage::OpenSlot;
  uint64_t leaseId;
  const char* text;
  String key;
  size_t position=0,recoveredPages=0,savedPages_=0;
  uint32_t recoveredCursor=0,generation=0,lastSaved=0;
  int readingSlot=0,latestSlot=-1,writingSlot=0;
  bool allowRestore=true;
  String slot(int index)const{return cache.path+(index?".idx1":".idx0");}
  bool trusted()const{return leaseId&&leaseId==storage_validation::lease()&&
    text==book&&identity.textBytes==bookLength&&key==bookmarkKey;}
  void hashHeader(const Header& h){
    mbedtls_sha256_starts_ret(&sha,0);
    mbedtls_sha256_update_ret(&sha,(const uint8_t*)&h,offsetof(Header,digest));
  }
  void fail(){file.close();bytes.reset();scratch.reset();stage=Stage::Failed;}
  void nextSlot(){file.close();bytes.reset();position=0;++readingSlot;stage=readingSlot<2?Stage::OpenSlot:Stage::Ready;}
  bool openCheck(int index,bool exact){
    file=SD.open(slot(index),"r");
    if(!file||file.size()<sizeof(candidate))return false;
    const size_t got=file.read((uint8_t*)&candidate,sizeof(candidate));cache.work.cacheReadBytes+=got;
    if(got!=sizeof(candidate)){cache.readFailure();return false;}
    if(!pagination_checkpoint::validHeader(candidate,file.size(),identity)||
       (exact&&memcmp(&candidate,&header,sizeof(header))))return false;
    if(!exact){bytes.reset((uint8_t*)ps_malloc(size_t(candidate.pages)*4));if(!bytes)return false;}
    position=0;hashHeader(candidate);return true;
  }
  int readStep(bool verify){
    const size_t total=size_t(candidate.pages)*4;
    const size_t count=std::min(pagination_checkpoint::IO_BYTES,total-position);
    uint8_t* destination=verify?scratch.get():bytes.get()+position;
    const size_t got=file.read(destination,count);cache.work.cacheReadBytes+=got;
    if(got!=count){cache.readFailure();return -1;}
    mbedtls_sha256_update_ret(&sha,destination,count);position+=count;
    if(position<total)return 0;
    uint8_t digest[32];mbedtls_sha256_finish_ret(&sha,digest);file.close();
    if(memcmp(digest,candidate.digest,32))return -1;
    // Readback must match the snapshot, including canonical coverage. The hash
    // checks all payload bytes; a partial cursor can never denote end-of-book.
    return verify||pagination_checkpoint::validPrefix((uint32_t*)bytes.get(),candidate.pages,candidate.cursor,identity.textBytes)?1:-1;
  }
 public:
  PaginationCheckpoint(BookCache& owner,const Identity& id,bool restore=true):
    cache(owner),identity(id),leaseId(storage_validation::lease()),text(book),key(bookmarkKey),allowRestore(restore){
    mbedtls_sha256_init(&sha);
  }
  ~PaginationCheckpoint(){file.close();mbedtls_sha256_free(&sha);}
  PaginationCheckpoint(const PaginationCheckpoint&)=delete;
  PaginationCheckpoint& operator=(const PaginationCheckpoint&)=delete;
  bool ready()const{return stage==Stage::Ready;}
  bool failed()const{return stage==Stage::Failed;}
  size_t savedPages()const{return savedPages_;}
  bool due(size_t pages,uint32_t now)const{
    return ready()&&pages>=pagination_checkpoint::FIRST_PAGES&&pages>savedPages_&&
      (!savedPages_||(pages-savedPages_>=pagination_checkpoint::PAGE_INTERVAL&&
                     uint32_t(now-lastSaved)>=pagination_checkpoint::WRITE_INTERVAL_MS));
  }
  // Call once after incremental loading. The callback must preserve the visible
  // window and exact anchor. It can reject a prefix already overtaken in RAM.
  template<class Restore> bool restore(Restore apply){
    if(!ready()||!trusted()||!recovered)return false;
    const bool accepted=allowRestore&&apply((uint32_t*)recovered.get(),recoveredPages,recoveredCursor);
    recovered.reset();recoveredPages=0;return accepted;
  }
  bool begin(const std::vector<uint32_t>& pages,uint32_t cursor){
    if(!ready()||!trusted()||generation==UINT32_MAX||
       !pagination_checkpoint::validPrefix(pages.data(),pages.size(),cursor,identity.textBytes))return false;
    bytes.reset((uint8_t*)ps_malloc(pages.size()*4));scratch.reset((uint8_t*)ps_malloc(pagination_checkpoint::IO_BYTES));
    if(!bytes||!scratch){fail();return false;}
    memcpy(bytes.get(),pages.data(),pages.size()*4);
    header=Header();header.identity=identity;header.generation=generation+1;
    header.pages=uint32_t(pages.size());header.cursor=cursor;
    writingSlot=latestSlot==0?1:0;position=0;stage=Stage::OpenWrite;return true;
  }
  void step(){
    if(failed())return;
    if(!trusted()){fail();return;}
    switch(stage){
    case Stage::OpenSlot:
      if(openCheck(readingSlot,false))stage=Stage::ReadSlot;else nextSlot();return;
    case Stage::ReadSlot:{
      const int result=readStep(false);if(!result)return;
      if(result>0&&candidate.generation>generation){
        generation=candidate.generation;latestSlot=readingSlot;savedPages_=candidate.pages;
        recovered=std::move(bytes);recoveredPages=candidate.pages;recoveredCursor=candidate.cursor;
      }
      nextSlot();return;
    }
    case Stage::Ready:return;
    case Stage::OpenWrite:
      if((!SD.exists("/.paperdiy-cache")&&!SD.mkdir("/.paperdiy-cache"))){fail();return;}
      file=SD.open(slot(writingSlot),"w");
      if(!file||file.write((const uint8_t*)&header,sizeof(header))!=sizeof(header)){storage_validation::invalidate();fail();return;}
      cache.work.cacheWriteBytes+=sizeof(header);hashHeader(header);stage=Stage::Write;return;
    case Stage::Write:{
      const size_t count=std::min(pagination_checkpoint::IO_BYTES,size_t(header.pages)*4-position);
      const size_t wrote=file.write(bytes.get()+position,count);cache.work.cacheWriteBytes+=wrote;
      if(wrote!=count){storage_validation::invalidate();fail();return;}
      mbedtls_sha256_update_ret(&sha,bytes.get()+position,count);position+=count;
      if(position==size_t(header.pages)*4)stage=Stage::Seal;return;
    }
    case Stage::Seal:
      mbedtls_sha256_finish_ret(&sha,header.digest);
      if(!file.seek(0)||file.write((const uint8_t*)&header,sizeof(header))!=sizeof(header)){storage_validation::invalidate();fail();return;}
      cache.work.cacheWriteBytes+=sizeof(header);file.flush();file.close();bytes.reset();stage=Stage::OpenVerify;return;
    case Stage::OpenVerify:
      if(!openCheck(writingSlot,true)){fail();return;}stage=Stage::Verify;return;
    case Stage::Verify:{
      const int result=readStep(true);if(!result)return;
      if(result<0){fail();return;}
      generation=header.generation;latestSlot=writingSlot;savedPages_=header.pages;lastSaved=millis();
      scratch.reset();stage=Stage::Ready;
      Console.printf("INDEX_CHECKPOINT_SAVED pages=%u\n",header.pages);return;
    }
    default:return;
    }
  }
};
