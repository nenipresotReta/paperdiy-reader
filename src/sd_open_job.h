#pragma once
#include "sd_extraction.h"
#include "sd_open_state.h"
// Main-thread cold EPUB lifecycle, shared by the SD adapter and offline tests.
// Firmware bindings (progress, reader globals and SD health) precede this header.
namespace sd_open {
struct Job {
 bool (*probe)();
 const uint64_t openingLease;
 BookCache cache;size_t index;bool paginate,forceReflow,cancelled=false,ioFailed=false,initialized=false;
 bool restoreFinished=false,checkpointDisabled=false;
 bookmarks::Anchor anchor;uint32_t started,lastYield;
 File input;
 epub::structure::Allocator allocator;
 epub::structure::Collector structure;
 epub::Importer importer;
 std::unique_ptr<ExtractionCheckpoint> checkpoint;
 static epub::structure::Allocator allocation(){
  epub::structure::Allocator a;a.allocate=[](size_t n,void*)->void*{return ps_malloc(n);};a.release=[](void* p,void*){free(p);};return a;
 }
 Job(BookCache&& c,size_t i,bool p,bool f,const bookmarks::Anchor& a,uint32_t at,bool (*inputProbe)())
  :probe(inputProbe),openingLease(storage_validation::lease()),cache(std::move(c)),index(i),paginate(p),forceReflow(f),anchor(a),started(at),lastYield(at),allocator(allocation()),structure(epub::structure::Limits(),allocator){}
 ~Job(){checkpoint.reset();input.close();cache.logWork();}
 bool prepare(){
  importer=epub::Importer{};
  importer.structure=&structure;importer.output=book;importer.capacity=MAX_BOOK;
  importer.alloc=[](size_t n)->void*{return ps_malloc(n);};
  importer.tick=[this](){if(uint32_t(millis()-lastYield)>=50){
   if(probe&&probe())cancelled=true;
   delay(1);lastYield=millis();
  }};
  importer.xmlMemoryAvailable=[](size_t required,size_t largest){return heap_caps_get_free_size(MALLOC_CAP_SPIRAM)>=required&&heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)>=largest;};
  if(cache.directory){
   importer.resourceSize=[this](const std::string& name)->size_t{return cache.observe(name);};
   importer.resourceRead=[this](const std::string& name,void* data,size_t n)->bool{
    File f=SD.open(cache.source+"/"+name.c_str());bool ok=f&&!f.isDirectory();
    for(size_t at=0;ok&&at<n;){const size_t count=std::min(size_t(8192),n-at);
     ok=f.read((uint8_t*)data+at,count)==count;at+=count;if(importer.tick)importer.tick();
    }
    if(!ok)ioFailed=true;return ok;
   };
  }else{
   if(!input)input=SD.open(cache.source,"r");if(!input){ioFailed=true;return false;}
   importer.fileSize=input.size();
   importer.read=[this](uint32_t at,void* data,size_t n)->size_t{
    if(!input.seek(at)){ioFailed=true;return 0;}size_t got=input.read((uint8_t*)data,n);if(got!=n)ioFailed=true;return got;
   };
  }
  return importer.begin();
 }
 bool initialize(){
  if(!prepare())return false;initialized=true;
  if(!importer.flattenedForMemory&&structure.status()!=epub::structure::Status::Allocation&&cache.enabled)
   checkpoint.reset(new(std::nothrow) ExtractionCheckpoint(cache,importer,structure));
  if(!checkpoint){checkpointDisabled=true;restoreFinished=true;}
  return true;
 }
 SDOpenResult failure(const char* message){
  sdError=message;bookLength=0;pageOffsets.clear();bookDocument.clear();
  if(ioFailed||cache.ioFailed)sdReadFailure();
  return SDOpenResult::Failed;
 }
 SDOpenResult publish(){
  input.close();
  if(!openingLease||openingLease!=storage_validation::lease())return failure("SD ownership changed while opening");
  if(!cache.unchanged()||openingLease!=storage_validation::lease())return failure("EPUB changed while opening");
  bookLength=importer.used;book[bookLength]=0;bookEnglishHyphens=importer.englishHyphenation;bookLanguage=importer.language;
  bookDocument.assign(structure,book,bookLength);
  bookEpubTransientFallback=importer.flattenedForMemory||structure.status()==epub::structure::Status::Allocation||bookDocument.allocationFailed();
  if(!bookDocument.ready()&&importer.unsafeFlatLanguage){bookLanguage=text_language::Language::None;bookEnglishHyphens=false;}
  Console.printf("BOOK_STRUCTURE ready=%u status=%u runs=%u retry_flat=%u transient=%u\n",unsigned(bookDocument.ready()),unsigned(structure.status()),unsigned(bookDocument.runCount()),unsigned(importer.flattenedForMemory),unsigned(bookEpubTransientFallback));
  if(!paginate){selectedBook=index;return SDOpenResult::Ready;}
  uint32_t hash=crcUpdate(0xffffffff,(const uint8_t*)cache.source.c_str(),cache.source.length())^0xffffffff;
  char key[12];snprintf(key,sizeof(key),"s%08lx",(unsigned long)hash);bookmarkKey=key;bookIdentity=cache.source;
  Console.printf("BOOK_PARSE_DONE bytes=%u ms=%u free_psram=%u\n",unsigned(bookLength),unsigned(millis()-started),ESP.getFreePsram());
  if(!indexBook(false,true,anchor.valid?&anchor:nullptr,cache.recoveryRequired(),true))return failure("Cannot restore saved position");
  const bool saved=saveReadingProgress();
  // The extraction journal no longer accesses the cache before ownership moves
  // to progressive pagination/complete-cache publication.
  checkpoint.reset();queueBackground(cache,!forceReflow);
  if(!saved)Console.println("CACHE_BACKGROUND_SKIPPED");
  rememberResident();selectedBook=index;
  Console.printf("SD_OPENED %s ms=%u\n",sdBooks[index].c_str(),unsigned(millis()-started));
  Console.printf("BOOK_TIMING result=import total_ms=%u\n",unsigned(millis()-started));
  return SDOpenResult::Ready;
 }
 SDOpenResult journalFailure(){
  if(checkpoint&&checkpoint->failure()==ExtractionCheckpoint::Failure::Unavailable){
   checkpoint.reset();checkpointDisabled=true;
   // A restore can fail allocation after writing scratch text/Collector. A
   // fresh package state is mandatory before continuing without the journal.
   if(!restoreFinished){if(!prepare())return failure(importer.error.c_str());restoreFinished=true;}
   Console.println("EXTRACTION_VOLATILE journal_unavailable=1");return SDOpenResult::Pending;
  }
  return failure("Cannot verify EPUB extraction checkpoint");
 }
 SDOpenResult poll(){
  if(!openingLease||openingLease!=storage_validation::lease())return failure("SD ownership changed while opening");
  if(cancelled&&!initialized){bookLength=0;pageOffsets.clear();bookDocument.clear();sdError="";Console.println("BOOK_OPEN_CANCELLED");return SDOpenResult::Cancelled;}
  if(!initialized&&!initialize())return failure(importer.error.empty()?"Cannot begin EPUB extraction":importer.error.c_str());
  if(ioFailed||cache.ioFailed)return failure("Cannot read EPUB source");
  if(checkpoint&&checkpoint->failed())return journalFailure();
  if(checkpoint&&checkpoint->busy()){
   checkpoint->step();
   if(checkpoint->failed())return journalFailure();
   return SDOpenResult::Pending;
  }
  if(!restoreFinished){
   restoreFinished=true;
   if(checkpoint&&!checkpoint->recovered()){
    // Invalid slots may have overwritten scratch text/Collector. Regenerate the
    // small package/title state before continuing a fresh import.
    if(!prepare())return failure(importer.error.c_str());
   }
  }
  if(importer.flattenedForMemory||structure.status()==epub::structure::Status::Allocation){checkpoint.reset();checkpointDisabled=true;}
  if(cancelled){
   if(checkpoint&&importer.chapterCursor&&importer.used>checkpoint->committedBytes()){
    if(!checkpoint->beginCommit())return journalFailure();
    return SDOpenResult::Pending;
   }
   bookLength=0;pageOffsets.clear();bookDocument.clear();sdError="";
   Console.println("BOOK_OPEN_CANCELLED");return SDOpenResult::Cancelled;
  }
  if(importer.progress==epub::Importer::Progress::Complete){
   if(checkpoint&&!checkpoint->complete()){
    if(!checkpoint->beginCommit())return journalFailure();
    return SDOpenResult::Pending;
   }
   if(checkpointDisabled)Console.println("EXTRACTION_VOLATILE transient_or_unavailable=1");
   return publish();
  }
  if(importer.stepChapter()==epub::Importer::Progress::Failed)return failure(importer.error.c_str());
  if(ioFailed||cache.ioFailed)return failure("Cannot read EPUB source");
  if(importer.flattenedForMemory||structure.status()==epub::structure::Status::Allocation){checkpoint.reset();checkpointDisabled=true;}
  if(checkpoint&&checkpoint->due()&&!checkpoint->beginCommit())return journalFailure();
  return SDOpenResult::Pending;
 }
};
}
