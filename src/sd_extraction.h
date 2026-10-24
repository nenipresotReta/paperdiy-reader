#pragma once
#include "sd_cache.h"
#include "epub_core.h"
#include "extraction_checkpoint.h"

// A canonical text spool and two independently authenticated commits. Only the
// suffix after the last committed length is writable, including after a torn
// append. Pages, ordinary bookmark keys and NVS are deliberately absent.
class ExtractionCheckpoint {
 public:
 enum class Failure { None, Unavailable, SourceChanged, IO };
 private:
 Failure failure_=Failure::None;
 using H=extraction_checkpoint::Header;
 using Buffer=std::unique_ptr<uint8_t,decltype(&free)>;
 enum class Stage { ReadHeader,ReadMetadata,ReadText,Ready,Append,CheckAppend,OpenSlot,WriteSlot,SealSlot,CheckSlot,Failed };
 BookCache& cache;epub::Importer& importer;epub::structure::Collector& collector;
 uint64_t leaseId;Stage stage=Stage::ReadHeader;
 Buffer metadata{nullptr,free},scratch{nullptr,free};
 File file;H header{},saved{};uint8_t encoded[extraction_checkpoint::HEADER_BYTES]={},plan[32]={};
 mbedtls_sha256_context sha,textSha;
 size_t position=0,metadataBytes=0,committed=0,appendEnd=0;
 uint32_t generation=0,lastCursor=0,lastSave=0;
 int order[2]={0,1},attempt=0,latest=-1,target=0;
 bool recovered_=false,committing_=false,restoring_=true;
 std::vector<CacheDependency> startupDependencies;
 String spool()const{return cache.path+".exttext";}
 String slot(int n)const{return cache.path+(n?".ext1":".ext0");}
 bool trusted()const{return leaseId&&leaseId==storage_validation::lease();}
 void close(){file.close();}
 void fail(Failure reason=Failure::IO){failure_=reason;if(reason==Failure::IO)cache.readFailure();close();metadata.reset();stage=Stage::Failed;restoring_=committing_=false;}
 bool unavailable(){fail(Failure::Unavailable);return false;}
 static bool persistentStatus(epub::structure::Status status){using S=epub::structure::Status;return status==S::Collecting||status==S::Complete||status==S::Budget||status==S::Invalid;}
 void next(){close();metadata.reset();++attempt;stage=Stage::ReadHeader;}
 void hashStart(const uint8_t* h){mbedtls_sha256_starts_ret(&sha,0);mbedtls_sha256_update_ret(&sha,h,160);}
 bool readHeader(int n,H& h,uint8_t* bytes){
  File f=SD.open(slot(n),"r");if(!f)return false;
  if(f.size()<sizeof(encoded)||f.size()>sizeof(encoded)+extraction_checkpoint::MAX_METADATA)return false;
  const size_t got=f.read(bytes,sizeof(encoded));cache.work.cacheReadBytes+=got;
  if(got!=sizeof(encoded)){cache.readFailure();return false;}
  return extraction_checkpoint::decode(bytes,f.size(),importer.capacity,
   reader_cache::TEXT_REVISION,reader_cache::EPUB_SEMANTIC_REVISION,cache.sourceHash,plan,h)&&h.chapters==importer.chapters.size();
 }
 bool decodeDependencies(){
  std::vector<CacheDependency> result;result.reserve(header.deps);
  const uint8_t* p=metadata.get();size_t at=0;
  for(uint32_t i=0;i<header.deps;++i){
   if(header.depBytes-at<44)return false;
   const uint32_t length=extraction_checkpoint::get(p+at);
   CacheDependency d;d.size=extraction_checkpoint::get(p+at+4);d.mtime=extraction_checkpoint::get(p+at+8);
   memcpy(d.digest,p+at+12,32);at+=44;
   if(length>1024||length>header.depBytes-at||!reader_cache::validResource((const char*)p+at,length))return false;
   for(size_t j=0;j<length;++j)if(!p[at+j])return false;
   std::string name((const char*)p+at,length);d.name=name.c_str();at+=length;
   if(!cache.directory&&length)return false;
   for(const auto& prior:result)if(prior.name==d.name)return false;
   result.push_back(std::move(d));
  }
  if(at!=header.depBytes||(!cache.directory&&result.size()!=1))return false;
  // The freshly parsed package/setup dependencies must also occur unchanged in
  // the saved graph. Later chapters discover additional dependencies normally.
  for(const auto& current:startupDependencies){
   bool found=false;for(const auto& prior:result)if(prior.name==current.name&&prior.size==current.size&&prior.mtime==current.mtime&&!memcmp(prior.digest,current.digest,32)){found=true;break;}
   if(!found)return false;
  }
  auto old=std::move(cache.dependencies);cache.dependencies=std::move(result);
  const bool valid=cache.unchanged();
  if(!valid)cache.dependencies=std::move(old);
  return valid;
 }
 void encodeDependencies(){
  size_t at=0;for(const auto& d:cache.dependencies){
   extraction_checkpoint::put(metadata.get()+at,uint32_t(d.name.length()));
   extraction_checkpoint::put(metadata.get()+at+4,d.size);extraction_checkpoint::put(metadata.get()+at+8,d.mtime);
   memcpy(metadata.get()+at+12,d.digest,32);at+=44;
   memcpy(metadata.get()+at,d.name.c_str(),d.name.length());at+=d.name.length();
  }
 }
 bool checkDigest(const uint8_t* expected){uint8_t actual[32];mbedtls_sha256_finish_ret(&sha,actual);return !memcmp(actual,expected,32);}
 void finishCommit(){
  close();committed=appendEnd;generation=header.generation;latest=target;lastCursor=header.cursor;lastSave=millis();
  saved=header;metadata.reset();stage=Stage::Ready;committing_=false;
  Console.printf("EXTRACTION_COMMITTED chapter=%u chapters=%u bytes=%u complete=%u\n",unsigned(header.cursor),unsigned(header.chapters),unsigned(committed),unsigned(bool(header.flags&extraction_checkpoint::COMPLETE)));
 }
 public:
 ExtractionCheckpoint(BookCache& c,epub::Importer& i,epub::structure::Collector& s):cache(c),importer(i),collector(s),leaseId(storage_validation::lease()),startupDependencies(c.dependencies){
  mbedtls_sha256_init(&sha);mbedtls_sha256_init(&textSha);mbedtls_sha256_starts_ret(&textSha,0);
  scratch.reset((uint8_t*)ps_malloc(extraction_checkpoint::IO_BYTES));
  mbedtls_sha256_starts_ret(&sha,0);
  // Length framing and original spine ordinals avoid plan concatenation ambiguity.
  for(const auto& chapter:importer.chapters){
   uint8_t fields[12];extraction_checkpoint::put(fields,uint32_t(chapter.path.size()));
   extraction_checkpoint::put(fields+4,chapter.spineIndex);extraction_checkpoint::put(fields+8,chapter.auxiliary?1:0);
   mbedtls_sha256_update_ret(&sha,fields,sizeof(fields));mbedtls_sha256_update_ret(&sha,(const uint8_t*)chapter.path.data(),chapter.path.size());
  }
  mbedtls_sha256_finish_ret(&sha,plan);
  H a,b;uint8_t bytes[extraction_checkpoint::HEADER_BYTES];
  const bool va=readHeader(0,a,bytes),vb=readHeader(1,b,bytes);
  if(vb&&(!va||b.generation>a.generation)){order[0]=1;order[1]=0;}
  if(cache.ioFailed)fail();
  else if(!scratch)fail(Failure::Unavailable);
  else if(!trusted())fail(Failure::SourceChanged);
 }
 ~ExtractionCheckpoint(){close();mbedtls_sha256_free(&sha);mbedtls_sha256_free(&textSha);}
 bool restoring()const{return restoring_;}
 bool busy()const{return restoring_||committing_;}
 bool failed()const{return stage==Stage::Failed;}
 Failure failure()const{return failure_;}
 bool recovered()const{return recovered_;}
 bool complete()const{return committed&&bool(saved.flags&extraction_checkpoint::COMPLETE);}
 size_t committedBytes()const{return committed;}
 // First chapter, then bounded growth/chapter cadence; cancellation/completion
 // force a commit. Text is appended once even when semantic metadata rotates.
 bool due()const{return importer.used&&(committed==0||
  ((importer.used-std::min(importer.used,committed)>=256u*1024||importer.chapterCursor-lastCursor>=64)&&uint32_t(millis()-lastSave)>=1000));}
 bool beginCommit(){
  if(stage!=Stage::Ready)return false;
  if(!cache.enabled)return unavailable();
  if(!trusted()){fail(Failure::SourceChanged);return false;}
  if(!importer.ok||importer.flattenedForMemory||!persistentStatus(collector.status())||!importer.used||importer.used>importer.capacity||
     importer.used<committed||(committed&&(committed<2||importer.output[committed-1]!='\n'||importer.output[committed-2]!='\n'))||
     importer.output[importer.used-1]!='\n'||importer.used<2||importer.output[importer.used-2]!='\n'||
     cache.dependencies.empty()||cache.dependencies.size()>reader_cache::MAX_DEPS||generation==UINT32_MAX)return unavailable();
  if(!cache.unchanged()){fail(cache.ioFailed?Failure::IO:Failure::SourceChanged);return false;}
  header=H{};header.generation=generation+1;header.textRevision=reader_cache::TEXT_REVISION;header.semanticRevision=reader_cache::EPUB_SEMANTIC_REVISION;
  header.flags=(importer.progress==epub::Importer::Progress::Complete?extraction_checkpoint::COMPLETE:0)|
   (importer.hasBodyText?extraction_checkpoint::BODY:0)|(importer.unsafeFlatLanguage?extraction_checkpoint::UNSAFE_LANGUAGE:0);
  header.textBytes=importer.used;header.cursor=importer.chapterCursor;header.chapters=importer.chapters.size();
  header.language=uint8_t(importer.language);header.collectorLanguage=importer.structureLanguage;
  header.collectorBytes=collector.snapshotBytes();header.deps=cache.dependencies.size();
  size_t depBytes=0;for(const auto& d:cache.dependencies)depBytes+=44+d.name.length();
  if(!header.collectorBytes||depBytes>extraction_checkpoint::MAX_METADATA||header.collectorBytes>extraction_checkpoint::MAX_METADATA-depBytes)return unavailable();
  header.depBytes=depBytes;metadataBytes=depBytes+header.collectorBytes;
  metadata.reset((uint8_t*)ps_malloc(metadataBytes));if(!metadata)return unavailable();
  encodeDependencies();
  if(!collector.encodeSnapshot(metadata.get()+depBytes,header.collectorBytes,importer.output,importer.used))return unavailable();
  memcpy(header.source,cache.sourceHash,32);memcpy(header.plan,plan,32);
  if(!SD.exists("/.paperdiy-cache")&&!SD.mkdir("/.paperdiy-cache"))return unavailable();
  // Never truncate a committed prefix. Orphan tails are overwritten from the
  // verified length; old slots remain usable until the new slot verifies.
  file=SD.open(spool(),committed?"r+":"w");
  if(!file)return unavailable();
  if(!file.seek(committed)){fail();return false;}
  appendEnd=importer.used;position=committed;target=latest==0?1:0;stage=Stage::Append;committing_=true;return true;
 }
 void step(){
  using namespace extraction_checkpoint;
  if(stage==Stage::Failed||stage==Stage::Ready)return;
  if(!trusted()){fail(Failure::SourceChanged);return;}
  if(stage==Stage::ReadHeader){
   if(attempt==2){
    cache.dependencies=startupDependencies;restoring_=false;stage=Stage::Ready;
    mbedtls_sha256_starts_ret(&textSha,0);
    // No valid commit survived: invalidate both headers before a new spool can
    // be truncated. A power cut during cleanup cannot resurrect a stale prefix.
    for(int n=0;n<2;++n)if(SD.exists(slot(n))&&!SD.remove(slot(n))){fail(Failure::Unavailable);return;}
    if(SD.exists(spool())&&!SD.remove(spool())){fail(Failure::Unavailable);return;}
    return;
   }
   if(!readHeader(order[attempt],header,encoded)){if(cache.ioFailed)fail();else next();return;}
   metadataBytes=header.depBytes+header.collectorBytes;metadata.reset((uint8_t*)ps_malloc(metadataBytes));
   if(!metadata){fail(Failure::Unavailable);return;}
   file=SD.open(slot(order[attempt]),"r");if(!file||!file.seek(HEADER_BYTES)){next();return;}
   hashStart(encoded);position=0;stage=Stage::ReadMetadata;return;
  }
  if(stage==Stage::ReadMetadata){
   const size_t n=std::min(IO_BYTES,metadataBytes-position),got=file.read(metadata.get()+position,n);cache.work.cacheReadBytes+=got;
   if(got!=n){fail();return;}mbedtls_sha256_update_ret(&sha,metadata.get()+position,n);position+=n;
   if(position<metadataBytes)return;
   close();if(!checkDigest(header.digest)||!decodeDependencies()){next();return;}
   file=SD.open(spool(),"r");if(!file||file.size()<header.textBytes||file.size()>importer.capacity){next();return;}
   position=0;mbedtls_sha256_starts_ret(&textSha,0);stage=Stage::ReadText;return;
  }
  if(stage==Stage::ReadText){
   const size_t n=std::min(IO_BYTES,size_t(header.textBytes)-position),got=file.read((uint8_t*)importer.output+position,n);cache.work.cacheReadBytes+=got;
   if(got!=n){fail();return;}mbedtls_sha256_update_ret(&textSha,(uint8_t*)importer.output+position,n);position+=n;
   if(position<header.textBytes)return;
   close();mbedtls_sha256_clone(&sha,&textSha);
   if(!checkDigest(header.textDigest)||header.textBytes<2||importer.output[header.textBytes-1]!='\n'||importer.output[header.textBytes-2]!='\n'){next();return;}
   if(!collector.restoreSnapshot(metadata.get()+header.depBytes,header.collectorBytes,importer.output,header.textBytes)){
    if(collector.status()==epub::structure::Status::Allocation)fail(Failure::Unavailable);else next();return;
   }
   if(!persistentStatus(collector.status())||(header.collectorLanguage&&!collector.language(header.collectorLanguage)[0])||
      ((header.flags&COMPLETE)?collector.status()==epub::structure::Status::Collecting:collector.status()==epub::structure::Status::Complete)||
      !importer.restoreProgress(header.cursor,header.textBytes,header.flags&BODY,header.flags&UNSAFE_LANGUAGE,
       header.language,header.collectorLanguage,header.flags&COMPLETE)){next();return;}

   saved=header;committed=header.textBytes;generation=header.generation;latest=order[attempt];lastCursor=header.cursor;lastSave=millis();
   metadata.reset();recovered_=true;restoring_=false;stage=Stage::Ready;
   Console.printf("EXTRACTION_RESTORED chapter=%u chapters=%u bytes=%u complete=%u\n",unsigned(header.cursor),unsigned(header.chapters),unsigned(committed),unsigned(bool(header.flags&COMPLETE)));return;
  }
  if(stage==Stage::Append){
   const size_t n=std::min(IO_BYTES,appendEnd-position),put=file.write((const uint8_t*)importer.output+position,n);cache.work.cacheWriteBytes+=put;
   if(put!=n){fail();return;}position+=n;if(position<appendEnd)return;
   file.flush();close();file=SD.open(spool(),"r");if(!file||!file.seek(committed)){fail();return;}
   position=committed;stage=Stage::CheckAppend;return;
  }
  if(stage==Stage::CheckAppend){
   const size_t n=std::min(IO_BYTES,appendEnd-position),got=file.read(scratch.get(),n);cache.work.cacheReadBytes+=got;
   if(got!=n||memcmp(scratch.get(),importer.output+position,n)){fail();return;}
   mbedtls_sha256_update_ret(&textSha,scratch.get(),n);position+=n;if(position<appendEnd)return;
   close();mbedtls_sha256_clone(&sha,&textSha);mbedtls_sha256_finish_ret(&sha,header.textDigest);
   encode(header,encoded);hashStart(encoded);mbedtls_sha256_update_ret(&sha,metadata.get(),metadataBytes);mbedtls_sha256_finish_ret(&sha,header.digest);encode(header,encoded);
   stage=Stage::OpenSlot;return;
  }
  if(stage==Stage::OpenSlot){
   file=SD.open(slot(target),"w");if(!file){fail(Failure::Unavailable);return;}
   const size_t put=file.write(encoded,sizeof(encoded));cache.work.cacheWriteBytes+=put;if(put!=sizeof(encoded)){fail();return;}
   position=0;stage=Stage::WriteSlot;return;
  }
  if(stage==Stage::WriteSlot){
   const size_t n=std::min(IO_BYTES,metadataBytes-position),put=file.write(metadata.get()+position,n);cache.work.cacheWriteBytes+=put;
   if(put!=n){fail();return;}position+=n;if(position<metadataBytes)return;
   file.flush();close();stage=Stage::SealSlot;return;
  }
  if(stage==Stage::SealSlot){
   H verify;uint8_t check[HEADER_BYTES];
   if(!readHeader(target,verify,check)||memcmp(check,encoded,sizeof(encoded))){fail();return;}
   file=SD.open(slot(target),"r");if(!file||!file.seek(HEADER_BYTES)){fail();return;}
   position=0;hashStart(encoded);stage=Stage::CheckSlot;return;
  }
  if(stage==Stage::CheckSlot){
   const size_t n=std::min(IO_BYTES,metadataBytes-position),got=file.read(scratch.get(),n);cache.work.cacheReadBytes+=got;
   if(got!=n||memcmp(scratch.get(),metadata.get()+position,n)){fail();return;}
   mbedtls_sha256_update_ret(&sha,scratch.get(),n);position+=n;if(position<metadataBytes)return;
   if(!checkDigest(header.digest)){fail();return;}finishCommit();
  }
 }
};
