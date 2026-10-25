#include "console.h"
#include "sd_reader.h"
#include "reader.h"
#include <M5Unified.h>
#include <mbedtls/sha256.h>
#include <SD.h>
#include <SPI.h>
#include <algorithm>
#include "epub_core.h"
#include "sd_cache.h"
#include "document_format.h"
#include "markdown_reader.h"
#include "resident_book.h"
#include "sd_speed.h"
#include "deferred_cache.h"
#include "sd_checkpoint.h"
#include "sd_extraction.h"
#include <esp_heap_caps.h>
static sd_speed::Policy sdSpeed;
void resetSDSpeed(){sdSpeed.newCard();}
bool sdOK=false;
std::vector<String> sdBooks;
size_t selectedBook=0;
String sdError;
static resident_book::Snapshot resident;
static std::unique_ptr<BookCache> backgroundCache;
static std::unique_ptr<DeferredCacheWrite> backgroundWrite;
static std::unique_ptr<PaginationCheckpoint> backgroundCheckpoint;
static bool checkpointAttempted=false,checkpointRestore=true;
static bool usbStorageHandoff=false;
static uint64_t backgroundLease=0;
static uint32_t backgroundLayout=0;
static bool backgroundIndex=false;
bool sdReadFailure();
void cancelContentPrefetch();
void cancelSDBackground(){
  cancelContentPrefetch();
  // Writer owns a reference to the cache; release it first.
  backgroundWrite.reset();backgroundCheckpoint.reset();backgroundCache.reset();backgroundIndex=false;checkpointAttempted=false;
}
bool sdBackgroundPending(){return backgroundIndex||bool(backgroundCache);}
uint32_t activeLayout(){return reader_cache::layoutFor(bookEnglishHyphens,bookMarkdown,reader_typography::currentStyle().flags(),bookLanguage==text_language::Language::EnglishGB,bookDocument.ready());}
namespace {
struct ContentPrefetchTask {
  bool armed=false,firstPaint=false,done=true,failed=false;
  const reader_content::Source* source=nullptr;
  uint64_t token=0,lease=0;
  size_t bookLength=0;
  String identity;
  uint32_t layout=0,armMs=0,firstPaintMs=0,maxStepMs=0,verifyMs=0;
};
ContentPrefetchTask prefetchTask;
const char* prefetchStateName(reader_content::PrefetchState state){
  switch(state){
  case reader_content::PrefetchState::Pending:return "pending";
  case reader_content::PrefetchState::Reading:return "reading";
  case reader_content::PrefetchState::VerifyingUtf8:return "verifying_utf8";
  case reader_content::PrefetchState::VerifyingSemantics:return "verifying_semantics";
  case reader_content::PrefetchState::Complete:return "complete";
  case reader_content::PrefetchState::Failed:return "failed";
  case reader_content::PrefetchState::Cancelled:return "cancelled";
  default:return "not_needed";
  }
}
const char* prefetchFailureName(reader_content::PrefetchFailure failure){
  switch(failure){
  case reader_content::PrefetchFailure::Lease:return "lease";
  case reader_content::PrefetchFailure::Read:return "read";
  case reader_content::PrefetchFailure::Hash:return "hash";
  case reader_content::PrefetchFailure::Utf8:return "utf8";
  case reader_content::PrefetchFailure::Semantic:return "semantic";
  default:return "none";
  }
}
void failContentPrefetch(reader_content::PrefetchFailure failure){
  if(prefetchTask.done)return;
  auto* source=reader_content::active()?reader_content::source().get():nullptr;
  if(source&&source==prefetchTask.source&&source->prefetchToken()==prefetchTask.token)
    source->abortPrefetch(failure);
  prefetchTask.failed=true;prefetchTask.armed=false;prefetchTask.done=true;
  // A failed fill must not remain a fast-reuse candidate. Reopening the same
  // book then takes the normal source/dependency validation path.
  resident.clear();
  const unsigned aux=unsigned(prefetchTask.source?
    (prefetchTask.source->prefetchTotalBlocks()*32+8192+64):0);
  Console.printf("CACHE_PREFETCH failed reason=%s loaded=%u/%u read=%llu max_step_ms=%u verify_ms=%u aux_bytes=%u\n",
    prefetchFailureName(failure),unsigned(prefetchTask.source?prefetchTask.source->prefetchLoadedBlocks():0),
    unsigned(prefetchTask.source?prefetchTask.source->prefetchTotalBlocks():0),
    (unsigned long long)(prefetchTask.source?prefetchTask.source->prefetchReadBytes():0),
    unsigned(prefetchTask.maxStepMs),unsigned(prefetchTask.verifyMs),aux);
}
void completeContentPrefetch(){
  auto* source=prefetchTask.source;
  const size_t loaded=source?source->prefetchLoadedBlocks():0;
  const uint64_t bytes=source?source->prefetchReadBytes():0;
  const unsigned aux=unsigned(source?source->prefetchTotalBlocks()*32+8192+64:0);
  Console.printf("CACHE_PREFETCH done blocks=%u read=%llu total_ms=%u first_paint_ms=%u verify_ms=%u max_step_ms=%u aux_bytes=%u\n",
    unsigned(loaded),(unsigned long long)bytes,unsigned(millis()-prefetchTask.armMs),
    unsigned(prefetchTask.firstPaintMs-prefetchTask.armMs),unsigned(prefetchTask.verifyMs),
    unsigned(prefetchTask.maxStepMs),aux);
  prefetchTask.armed=false;prefetchTask.done=true;prefetchTask.failed=false;
  // All bytes and semantic boundaries have been authenticated. The on-demand
  // source can now be released; the book buffer is complete in RAM.
  reader_content::clear();
  prefetchTask.source=nullptr;prefetchTask.token=0;
}
}
void contentPrefetchArm(){
  cancelContentPrefetch();
  if(!reader_content::active()||!bookLength||!bookDocument.ready())return;
  auto* source=reader_content::source().get();
  if(!source->prefetchSupported())return;
  const uint64_t lease=storage_validation::lease();
  if(!lease)return;
  source->beginPrefetch();
  if(!source->prefetchPending()){
    if(source->prefetchFailed())resident.clear();
    return;
  }
  prefetchTask.armed=true;prefetchTask.firstPaint=false;prefetchTask.done=false;prefetchTask.failed=false;
  prefetchTask.source=source;prefetchTask.token=source->prefetchToken();
  prefetchTask.lease=lease;prefetchTask.bookLength=bookLength;prefetchTask.identity=bookIdentity;
  prefetchTask.layout=activeLayout();prefetchTask.armMs=millis();
  prefetchTask.maxStepMs=0;prefetchTask.verifyMs=0;
  Console.printf("CACHE_PREFETCH arm blocks=%u bytes=%u loaded=%u aux_bytes=%u\n",
    unsigned(source->prefetchTotalBlocks()),unsigned(bookLength),unsigned(source->prefetchLoadedBlocks()),
    unsigned(source->prefetchTotalBlocks()*32+8192+64));
}
void contentPrefetchFirstPaint(){
  if(!prefetchTask.armed||prefetchTask.firstPaint)return;
  prefetchTask.firstPaint=true;prefetchTask.firstPaintMs=millis();
  Console.printf("CACHE_PREFETCH first_paint_ms=%u blocks=%u\n",
    unsigned(prefetchTask.firstPaintMs-prefetchTask.armMs),
    unsigned(prefetchTask.source?prefetchTask.source->prefetchTotalBlocks():0));
}
bool contentPrefetchPending(){
  if(!prefetchTask.armed||prefetchTask.done)return false;
  if(!reader_content::active()){cancelContentPrefetch();return false;}
  auto* source=reader_content::source().get();
  if(source!=prefetchTask.source||source->prefetchToken()!=prefetchTask.token||
     bookLength!=prefetchTask.bookLength||bookIdentity!=prefetchTask.identity||
     activeLayout()!=prefetchTask.layout){
    cancelContentPrefetch();return false;
  }
  // A missing first paint must not hold sleep indefinitely if a caller never
  // reaches the reading scene. Normal reading arms and paints synchronously.
  if(!prefetchTask.firstPaint&&uint32_t(millis()-prefetchTask.armMs)>10000){
    cancelContentPrefetch();return false;
  }
  return true;
}
bool contentPrefetchStep(){
  if(!contentPrefetchPending()||!prefetchTask.firstPaint)return false;
  auto* source=reader_content::source().get();
  if(storage_validation::lease()!=prefetchTask.lease){
    failContentPrefetch(reader_content::PrefetchFailure::Lease);return false;
  }
  const auto beforeState=source->prefetchState();
  const uint32_t started=millis();
  const bool ok=source->prefetchStep();
  const uint32_t elapsed=uint32_t(millis()-started);
  if(elapsed>prefetchTask.maxStepMs)prefetchTask.maxStepMs=elapsed;
  if(beforeState==reader_content::PrefetchState::VerifyingUtf8||
     beforeState==reader_content::PrefetchState::VerifyingSemantics)
    prefetchTask.verifyMs+=elapsed;
  if(source->prefetchFailed()){
    failContentPrefetch(source->prefetchFailure());return false;
  }
  if(source->prefetchComplete()){completeContentPrefetch();return true;}
  if(!source->prefetchPending()){cancelContentPrefetch();return false;}
  return ok;
}
void cancelContentPrefetch(){
  if(prefetchTask.armed&&reader_content::active()){
    auto* source=reader_content::source().get();
    if(source==prefetchTask.source&&source->prefetchToken()==prefetchTask.token)source->cancelPrefetch();
  }
  prefetchTask=ContentPrefetchTask{};
}
bool contentPrefetchFailed(){return prefetchTask.failed;}
const char* contentPrefetchStateName(){
  if(prefetchTask.failed)return "failed";
  if(!prefetchTask.armed)return "none";
  if(!prefetchTask.firstPaint)return "pending_first_paint";
  return prefetchStateName(reader_content::prefetchState());
}
void queueBackground(BookCache& cache,bool restoreCheckpoint=true){
  cancelSDBackground();backgroundLease=storage_validation::lease();backgroundLayout=activeLayout();
  backgroundIndex=readerIndexPending();checkpointRestore=restoreCheckpoint;
  if(!bookMarkupFallback&&!bookEpubTransientFallback&&!progressSaveFailed&&backgroundLease)
    backgroundCache.reset(new(std::nothrow) BookCache(std::move(cache)));
}
void pollSDBackground(){
  if(!sdBackgroundPending())return;
  // A changed layout cannot inherit this job. Revoking SD trust (including a
  // light sleep) cancels publication, but pagination of the owned RAM document
  // may still finish. Actual book/card replacement cancels the job explicitly.
  if(backgroundLayout!=activeLayout()){
    cancelSDBackground();return;
  }
  if(backgroundLease!=storage_validation::lease()){
    backgroundWrite.reset();backgroundCheckpoint.reset();backgroundCache.reset();backgroundLease=0;
  }
  if(backgroundIndex){
    // Identity and slot I/O run only after the provisional first page has been
    // presented. The source was validated on open; unchanged() checks that
    // proof under this same exclusive lease before checkpoint admission.
    if(backgroundCache&&!checkpointAttempted){
      checkpointAttempted=true;pagination_checkpoint::Identity identity;
      if(checkpointIdentity(*backgroundCache,backgroundLayout,identity))
        backgroundCheckpoint.reset(new(std::nothrow) PaginationCheckpoint(*backgroundCache,identity,checkpointRestore));
    }
    if(backgroundCheckpoint){
      backgroundCheckpoint->step();
      if(backgroundCheckpoint->failed())backgroundCheckpoint.reset();
      else if(backgroundCheckpoint->restore([](const uint32_t* pages,size_t count,uint32_t cursor){
          return restoreReaderIndex(pages,count,cursor);
        }))Console.println("INDEX_CHECKPOINT_RESTORED");
    }
    if(backgroundCache&&backgroundCache->ioFailed){
      backgroundCheckpoint.reset();backgroundCache.reset();backgroundLease=0;
      sdReadFailure(); // The owned RAM page/index remains available.
    }
    advanceReaderIndex(2,12);
    backgroundIndex=readerIndexPending();
    if(backgroundIndex){
      const auto* prefix=partialReaderPages();
      if(backgroundCheckpoint&&prefix&&backgroundCheckpoint->due(prefix->size(),millis()))
        backgroundCheckpoint->begin(*prefix,partialReaderCursor());
      return;
    }
    backgroundCheckpoint.reset();
    if(readerIndexFailed()){cancelSDBackground();return;}
  }
  if(!backgroundCache)return;
  if(!backgroundWrite){
    const auto* pages=canonicalReaderPages();
    // The exact bookmark must be durable before its migration cache rotates.
    if(!pages||!saveReadingProgress()){cancelSDBackground();return;}
    backgroundWrite.reset(new(std::nothrow) DeferredCacheWrite(*backgroundCache));
    if(!backgroundWrite||!backgroundWrite->begin(*pages)){
      Console.println("CACHE_BACKGROUND_SKIPPED");cancelSDBackground();return;
    }
  }
  backgroundWrite->step();
  if(backgroundWrite->done()||backgroundWrite->failed()){
    if(backgroundWrite->failed())Console.println("CACHE_BACKGROUND_SKIPPED");
    const bool ioFailed=backgroundCache->ioFailed;cancelSDBackground();
    if(ioFailed)sdReadFailure();
  }
}
void rememberResident(){
  if(managed_progress::isPushPath(bookIdentity.c_str())||bookMarkupFallback||bookEpubTransientFallback||progressSaveFailed||pageOffsets.empty()){resident.clear();return;}
  resident.remember(bookIdentity.c_str(),bookmarkKey.c_str(),storage_validation::lease(),activeLayout(),bookLength);
}
bool sdReadFailure() {
  sdSpeed.readFailed();
  Console.printf("SD_SPEED_FALLBACK ceiling_hz=%u\n",sdSpeed.ceiling);
  storage_validation::invalidate();
  sdError="Cannot read SD file";
  // Force a fresh mount/scan on the next attempt after removal or bus failure.
  sdOK=false;SD.end();
  return false;
}
uint32_t sdStorageFrequency(){return sdSpeed.active?sdSpeed.active:4000000u;}
bool beginUsbStorageHandoff(){
  if(usbStorageHandoff)return true;
  // Saving an unchanged exact anchor is harmless; failure must leave the book
  // and local SD ownership available for retry, before any handle is closed.
  if(!pageOffsets.empty()&&!saveReadingProgress())return false;
  cancelOpenSD();while(openSDPending())pollOpenSD();
  cancelSDBackground();cancelReaderIndex();reader_content::clear();resident.clear();
  storage_validation::invalidate();usbStorageHandoff=true;sdOK=false;SD.end();
  return true;
}
bool endUsbStorageHandoff(){
  if(!usbStorageHandoff)return sdOK;
  // Caller has already synchronized/uninitialized raw USB access. Clear RAM
  // content because host writes can invalidate text, semantics and anchors.
  usbStorageHandoff=false;bookLength=0;pageOffsets.clear();bookDocument.clear();
  clearMarkdown();bookIdentity="";resident.clear();storage_validation::invalidate();
  return scanSD();
}
bool sdOwnedByUsb(){return usbStorageHandoff;}
uint32_t crcUpdate(uint32_t c,const uint8_t* p,size_t n){
  while(n--){c^=*p++;for(int i=0;i<8;i++)c=(c>>1)^((c&1)?0xedb88320:0);}
  return c;
}

void scanDirectory(const String& path,unsigned depth){
  Console.printf("SD_DIR %s\n",path.c_str());
  File dir=SD.open(path);if(!dir||!dir.isDirectory())return;
  while(sdBooks.size()<256){
    File f=dir.openNextFile();if(!f)break;String name=f.name();
    if(!name.startsWith(".")){
      String full=path=="/"?"/"+name:path+"/"+name;
      String lower=name;lower.toLowerCase();
      if(f.isDirectory()&&lower.endsWith(".epub")){sdBooks.push_back(full);}
      else if(f.isDirectory()&&depth<3){f.close();scanDirectory(full,depth+1);}
      else if(!f.isDirectory()&&document_format::detect(name.c_str())!=document_format::Kind::Unsupported)sdBooks.push_back(full);
    }
    f.close();delay(1);
  }
}
bool scanSD(){
  if(usbStorageHandoff){sdError="SD card is owned by USB";return false;}
  cancelOpenSD();while(openSDPending())pollOpenSD();
  cancelSDBackground();
  storage_validation::invalidate();
  String previous=selectedBook<sdBooks.size()?sdBooks[selectedBook]:prefs.getString("sdLast","");
  sdBooks.clear();selectedBook=0;sdError="";
  pinMode(48,INPUT_PULLUP);
  Console.printf("SD_DETECT level=%d (active low)\n",digitalRead(48));
  SD.end();SPI.begin(39,40,38,47);
  sdOK=false;
  for(uint32_t frequency : sd_speed::FREQUENCIES){
    if(!sdSpeed.accepts(frequency))continue;
    SD.end();
    pinMode(47,OUTPUT);digitalWrite(47,HIGH);
    pinMode(40,INPUT_PULLUP);
    delay(100);SPI.begin(39,40,38,47);
    Console.printf("SD_TRY frequency=%u\n",frequency);
    sdOK=SD.begin(47,SPI,frequency);
    Console.printf("SD_TRY_DONE ok=%d\n",sdOK);
    if(sdOK&&SD.cardType()!=CARD_NONE){
      File root=SD.open("/");const bool ready=root&&root.isDirectory();root.close();
      if(ready){sdSpeed.mounted(frequency);break;}sdOK=false;
    }
  }
  if(!sdOK||SD.cardType()==CARD_NONE){sdOK=false;sdError="No SD card / mount failed (FAT32)";Console.println("SD_NOT_MOUNTED");return false;}
  storage_validation::beginExclusive();
  Console.println("SD_SCAN_BEGIN");scanDirectory("/",0);Console.println("SD_SCAN_DONE");std::sort(sdBooks.begin(),sdBooks.end(),[](const String& a,const String& b){return a.compareTo(b)<0;});
  for(size_t i=0;i<sdBooks.size();++i)if(sdBooks[i]==previous){selectedBook=i;break;}
  Console.printf("SD_MOUNTED size_mb=%llu books=%u\n",SD.cardSize()/1024/1024,(unsigned)sdBooks.size());
  for(size_t i=0;i<sdBooks.size();i++)Console.printf("SD_FILE %u %s\n",(unsigned)i,sdBooks[i].c_str());return true;
}
bool validUtf8(const char* data,size_t size){
  size_t p=0;while(p<size){uint8_t c=data[p++];if(c<128){if(!c)return false;continue;}int n;uint32_t code,min;
    if(c>=0xc2&&c<=0xdf){n=1;code=c&31;min=128;}else if(c>=0xe0&&c<=0xef){n=2;code=c&15;min=2048;}else if(c>=0xf0&&c<=0xf4){n=3;code=c&7;min=65536;}else return false;
    if(size-p<(size_t)n)return false;while(n--){uint8_t b=data[p++];if((b&0xc0)!=0x80)return false;code=(code<<6)|(b&63);}if(code<min||code>0x10ffff||(code>=0xd800&&code<=0xdfff))return false;
  }return true;
}
bool inspectCachedLanguage(const String& source,bool directory,BookCache& cache){
  epub::Importer importer;bool ioFailed=false;
  File input=directory?File():SD.open(source);
  if(!directory&&!input)return sdReadFailure();
  importer.inspectOnly=true;importer.output=nullptr;importer.capacity=0;
  importer.fileSize=directory?0:input.size();
  importer.alloc=[](size_t bytes)->void*{return ps_malloc(bytes);};
  uint32_t yielded=millis();importer.tick=[&](){if(uint32_t(millis()-yielded)>=50){delay(1);yielded=millis();}};
  importer.xmlMemoryAvailable=[](size_t required,size_t largest){
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM)>=required&&heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)>=largest;
  };
  importer.read=[&](uint32_t at,void* data,size_t bytes)->size_t{
    if(!input.seek(at)){ioFailed=true;return 0;}
    const size_t count=input.read((uint8_t*)data,bytes);if(count!=bytes)ioFailed=true;return count;
  };
  if(directory){
    importer.resourceSize=[&](const std::string& name){return cache.observe(name);};
    importer.resourceRead=[&](const std::string& name,void* data,size_t bytes){
      File resource=SD.open(source+"/"+name.c_str());
      bool ok=resource&&!resource.isDirectory()&&resource.read((uint8_t*)data,bytes)==bytes;
      if(!ok)ioFailed=true;return ok;
    };
  }
  const bool ok=importer.run();input.close();
  if(ioFailed||cache.ioFailed)return sdReadFailure();
  bookLanguage=ok?importer.language:text_language::Language::None;
  bookEnglishHyphens=bookLanguage!=text_language::Language::None;
  Console.printf("BOOK_LANGUAGE inspected=%u language=%u\n",unsigned(ok),unsigned(bookLanguage));
  return true;
}
static bool startExtraction(BookCache&& cache,size_t index,bool paginate,bool forceReflow,const bookmarks::Anchor& anchor,uint32_t started);
static bool openSDImmediate(size_t index,bool paginate,bool forceReflow){
  if(usbStorageHandoff){sdError="SD card is owned by USB";return false;}
  if(!forceReflow&&index<sdBooks.size()&&!managed_progress::isPushPath(sdBooks[index].c_str())&&book&&sdOK&&!bookMarkupFallback&&!bookEpubTransientFallback&&!progressSaveFailed&&bookIdentity==sdBooks[index]&&
     resident.matches(sdBooks[index].c_str(),bookmarkKey.c_str(),storage_validation::lease(),activeLayout(),bookLength)&&
     (readerHasProvisionalPages()||reader_cache::validOffsets(pageOffsets.data(),pageOffsets.size(),bookLength))){
    uint32_t offset=prefs.isKey(bookmarkKey.c_str())?prefs.getUInt(bookmarkKey.c_str(),UINT32_MAX):0;
    if(offset>=bookLength){resident.clear();sdError="Cannot restore saved position";return false;}
    if(!readerHasProvisionalPages()||currentReadingOffset()==offset){
      if(!readerHasProvisionalPages()){
        auto page=std::upper_bound(pageOffsets.begin(),pageOffsets.end(),offset);
        const uint32_t selected=page==pageOffsets.begin()?0:page-pageOffsets.begin()-1;
        if(!pinReaderPage(selected)){resident.clear();sdError="Cannot read cached page";return false;}
        pageNumber=selected;
      }
      retainReadingAnchor(offset);prepareReaderFont();selectedBook=index;sdError="";
      Console.println("BOOK_TIMING result=resident cache_ms=0 import_ms=0 index_ms=0 save_ms=0 source_bytes=0 cache_read_bytes=0");
      return true;
    }
  }
  cancelSDBackground();cancelReaderIndex();reader_content::clear();
  resident.clear();
  uint32_t started=millis();pageOffsets.clear();bookLength=0;bookEnglishHyphens=false;
  bookDocument.clear();bookEpubTransientFallback=false;bookLanguage=text_language::Language::None;
  struct Timings {
    uint32_t started,cacheMs=0,importMs=0,indexMs=0,saveMs=0;
    const char* result="failed";
    explicit Timings(uint32_t at):started(at){}
    ~Timings(){Console.printf("BOOK_TIMING result=%s cache_ms=%u import_ms=%u index_ms=%u save_ms=%u total_ms=%u\n",
      result,unsigned(cacheMs),unsigned(importMs),unsigned(indexMs),unsigned(saveMs),unsigned(millis()-started));}
  } timing{started};
  clearMarkdown();bookMarkdown=false;bookMarkupFallback=false;
  sdError="";if(index>=sdBooks.size()||!book){sdError="Book unavailable";return false;}
  const auto format=document_format::detect(sdBooks[index].c_str());
  const bool managedPush=managed_progress::isPushPath(sdBooks[index].c_str());
  bookMarkdown=format==document_format::Kind::Markdown;
  File f=SD.open(sdBooks[index]);if(!f)return sdReadFailure();
  bool directory=f.isDirectory();f.close();
  BookCache cache(sdBooks[index],directory);
  struct CacheReport {BookCache& cache;bool active=true;explicit CacheReport(BookCache& c):cache(c){}~CacheReport(){if(active)cache.logWork();}} report{cache};
  const uint32_t cacheStarted=millis();
  bool pageHit=cache.load();
  const bool textHit=pageHit||cache.loadTextForReflow();
  if(forceReflow&&pageHit)pageHit=false;
  if(cache.ioFailed)return sdReadFailure();
  bool preparedUpgrade=false;bookmarks::Anchor preparedAnchor;
  if(textHit&&cache.preparedChanged()){
    if(reader_content::active()){
      if(!reader_content::pin(0,bookLength)||(bookDocument.ready()&&!bookDocument.validateSpan(0,bookLength))){
        sdError="Cannot recover cached position";return false;
      }
      reader_content::clear();
    }
    preparedAnchor=bookmarks::capture(book,bookLength,currentReadingOffset());
    if(cache.loadPrepared()){preparedUpgrade=true;pageHit=false;}
    else if(!(pageHit?cache.load():cache.loadTextForReflow())){
      sdError="Cannot restore cached book";return false;
    }
  }
  if(cache.ioFailed)return sdReadFailure();
  timing.cacheMs=millis()-cacheStarted;
  if(pageHit){
    if(bookMarkdown&&!prepareMarkdown(book,bookLength)){
      bookMarkdown=false;bookMarkupFallback=true;
      pageOffsets.clear();
      if(!indexBook()){sdError="Cannot restore saved position";return false;}
      Console.println("MARKDOWN_FALLBACK original_source=1");
    }
    timing.result="pages";
    rememberResident();
    if(paginate)contentPrefetchArm();
    selectedBook=index;Console.printf("SD_OPENED %s ms=%u cached=1\n",sdBooks[index].c_str(),(unsigned)(millis()-started));return true;
  }
  if(textHit){
    if(!cache.languageKnown&&format==document_format::Kind::Epub&&
       !inspectCachedLanguage(sdBooks[index],directory,cache))return false;
    timing.cacheMs=millis()-cacheStarted;
    if(!paginate){timing.result="text";selectedBook=index;return true;}
    // Text identity and its exact NVS byte anchor are unchanged. Rebuild only
    // geometry; do not re-extract XML or relocate against the same text.
    pageOffsets.clear();uint32_t stage=millis();
    const bool progressive=format==document_format::Kind::Epub;
    if(!indexBook(false,preparedUpgrade,preparedAnchor.valid?&preparedAnchor:nullptr,preparedUpgrade,progressive)){sdError="Cannot restore saved position";return false;}
    timing.indexMs=millis()-stage;stage=millis();
    bool saved=saveReadingProgress();
    if(progressive)queueBackground(cache,!forceReflow);
    else if(saved&&!bookMarkupFallback&&!bookEpubTransientFallback&&!cache.save())Console.println("CACHE_SAVE_SKIPPED");
    if(cache.ioFailed)return sdReadFailure();
    timing.saveMs=millis()-stage;timing.result="reflow";
    rememberResident();
    selectedBook=index;Console.printf("SD_OPENED %s ms=%u text_cached=1\n",sdBooks[index].c_str(),unsigned(millis()-started));return true;
  }
  bookmarks::Anchor anchor;
  uint32_t legacyOffset=0;
  if(cache.loadLegacyForMigration(legacyOffset)){
    anchor=bookmarks::capture(book,bookLength,legacyOffset);
    Console.printf("PROGRESS_LEGACY offset=%u bytes=%u\n",legacyOffset,(unsigned)bookLength);
  }
  if(cache.ioFailed)return sdReadFailure();
  timing.cacheMs=millis()-cacheStarted;
  const uint32_t importStarted=millis();
  pageOffsets.clear();bookLength=0;
  cache.dependencies.clear();if(!directory)cache.observe("");
  if(cache.ioFailed)return sdReadFailure();
  f=SD.open(sdBooks[index]);if(!f)return sdReadFailure();
  Console.println("CACHE_MISS");
  bool ok=cache.loadPrepared();pageOffsets.clear();
  if(cache.ioFailed){f.close();return sdReadFailure();}
  if(ok){Console.println("BOOK_IMPORT prepared=1");}
  else if(format==document_format::Kind::Text||format==document_format::Kind::Markdown){
    bookEnglishHyphens=true; // Plain text uses the reader's English default.
    bookLanguage=text_language::Language::EnglishUS;
    bookLength=f.size();if(!bookLength||bookLength>(managedPush?managed_progress::MAX_BYTES:document_format::sourceLimit(format))){
      sdError=managedPush?"Push exceeds 16 KiB / empty":bookMarkdown?"Markdown exceeds 1 MiB / empty":"TXT exceeds 4 MiB / empty";return false;
    }
    ok=f.read((uint8_t*)book,bookLength)==bookLength;
    if(!ok){f.close();return sdReadFailure();}
    if(ok&&format==document_format::Kind::Text&&bookLength>=3&&!memcmp(book,"\xef\xbb\xbf",3)){memmove(book,book+3,bookLength-3);bookLength-=3;}
    if(ok&&!validUtf8(book,bookLength)){sdError="Text must be UTF-8; convert encoding";ok=false;}
  }else{
    if(!directory&&f.size()>64*1024*1024){sdError="EPUB exceeds 64 MiB";return false;}
    f.close();
    timing.result="pending";report.active=false;
    startExtraction(std::move(cache),index,paginate,forceReflow,anchor,started);
    return false;
  }
  f.close();if(managedPush&&ok&&!cache.unchanged()){sdError="Push changed while opening";ok=false;}
  if(cache.ioFailed)return sdReadFailure();
  if(!ok){bookLength=0;Console.printf("SD_OPEN_FAILED %s\n",sdError.c_str());return false;}
  timing.importMs=millis()-importStarted;
  book[bookLength]=0;
  if(!paginate){timing.result="import";return true;}
  uint32_t hash=crcUpdate(0xffffffff,(const uint8_t*)sdBooks[index].c_str(),sdBooks[index].length())^0xffffffff;
  char key[12];snprintf(key,sizeof(key),"s%08lx",(unsigned long)hash);bookmarkKey=key;
  bookIdentity=sdBooks[index];
  Console.printf("BOOK_PARSE_DONE bytes=%u ms=%u free_psram=%u\n",(unsigned)bookLength,(unsigned)(millis()-started),ESP.getFreePsram());
  bool recoveryRequired=cache.recoveryRequired();
  uint32_t stage=millis();
  const bool progressive=format==document_format::Kind::Epub;
  if(!indexBook(false,true,anchor.valid?&anchor:nullptr,recoveryRequired,progressive)){
    sdError=(anchor.valid||recoveryRequired)?"Cannot restore saved position":"Pagination failed";return false;
  }
  timing.indexMs=millis()-stage;stage=millis();
  // Commit the migrated position before replacing its old text cache. A
  // failed NVS write must leave the legacy cache available for another try.
  bool saved=saveReadingProgress();
  if(progressive)queueBackground(cache,!forceReflow);
  else if(saved&&cache.enabled&&!bookMarkupFallback&&!bookEpubTransientFallback&&!cache.save())Console.println("CACHE_SAVE_SKIPPED");
  if(cache.ioFailed)return sdReadFailure();
  timing.saveMs=millis()-stage;timing.result="import";
  rememberResident();
  selectedBook=index;Console.printf("SD_OPENED %s ms=%u\n",sdBooks[index].c_str(),(unsigned)(millis()-started));return true;
}
#include "sd_open_job.h"
namespace {
static bool (*openCancelProbe)()=nullptr;
static SDOpenResult openResult=SDOpenResult::Failed;
using SDOpenJob=sd_open::Job;
static std::unique_ptr<SDOpenJob> openJob;
}
static bool startExtraction(BookCache&& cache,size_t index,bool paginate,bool forceReflow,const bookmarks::Anchor& anchor,uint32_t started){
 openJob.reset(new(std::nothrow) SDOpenJob(std::move(cache),index,paginate,forceReflow,anchor,started,openCancelProbe));
 if(!openJob){sdError="Not enough memory for EPUB job";return false;}
 return true;
}
SDOpenResult beginOpenSD(size_t index,bool paginate,bool forceReflow){
 if(openJob)return SDOpenResult::Pending;
 const bool ready=openSDImmediate(index,paginate,forceReflow);
 openResult=openJob?SDOpenResult::Pending:ready?SDOpenResult::Ready:SDOpenResult::Failed;
 return openResult;
}
SDOpenResult pollOpenSD(){
 if(!openJob)return openResult;
 if(openCancelProbe&&openCancelProbe())openJob->cancelled=true;
 openResult=openJob->poll();if(openResult!=SDOpenResult::Pending)openJob.reset();return openResult;
}
void cancelOpenSD(){if(openJob)openJob->cancelled=true;}
bool openSDPending(){return bool(openJob);}
bool sdOpenClosePending(){return openJob&&openJob->cancelled;}
void setSDOpenCancelProbe(bool (*probe)()){openCancelProbe=probe;}
bool openSD(size_t index,bool paginate,bool forceReflow){
 SDOpenResult result=beginOpenSD(index,paginate,forceReflow);
 while(result==SDOpenResult::Pending){result=pollOpenSD();delay(1);}
 return result==SDOpenResult::Ready;
}

#ifdef PAPERDIY_DIAGNOSTICS
void logSDCacheInfo(){
  if(usbStorageHandoff){Console.println("CACHE_INFO_UNAVAILABLE usb_owner=1");return;}
  for(size_t i=0;i<sdBooks.size();++i){
    File source=SD.open(sdBooks[i]);if(!source)continue;
    BookCache cache(sdBooks[i],source.isDirectory());source.close();
    if(!cache.enabled)continue;
    File f=SD.open(cache.path);reader_cache::Header header{};
    size_t count=f?f.read((uint8_t*)&header,sizeof(header)):0;
    Console.printf("CACHE_INFO index=%u header_bytes=%u magic=%08x version=%u layout=%08x text=%u\n",
      unsigned(i),unsigned(count),header.magic,header.version,header.layout,header.textBytes);
  }
  Console.println("CACHE_INFO_DONE");
}
#endif
