#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <openssl/sha.h>
#include <chrono>
#include "epub_reader.h"
#include "resident_book.h"
using String=std::string;
static std::string root;
static size_t writeBudget=SIZE_MAX;
static bool corruptOnFlush=false;
static uint32_t fakeMillis=0,ioMillis=0;
static unsigned delayCalls=0;
static std::string watchedSource;
static size_t sourceReadBytes=0;
static size_t sourceReadBudget=SIZE_MAX;
static int sourceStatError=0;
static std::function<void()> onSourceRead,onSourceStat;
void runOnce(std::function<void()>& callback){
 if(callback){auto action=std::move(callback);callback=nullptr;action();}
}
struct File {
 std::shared_ptr<std::fstream> f;std::string path;
 File()=default;
 File(std::string p,bool write):path(p){f=std::make_shared<std::fstream>(p,std::ios::binary|(write?(std::ios::in|std::ios::out|std::ios::trunc):std::ios::in));}
 explicit operator bool()const{return f&&bool(*f);}
 size_t read(uint8_t* p,size_t n){
   if(path==watchedSource&&sourceReadBudget!=SIZE_MAX){n=std::min(n,sourceReadBudget);sourceReadBudget-=n;}
   fakeMillis+=ioMillis;f->read((char*)p,n);size_t count=f->gcount();
   if(path==watchedSource){sourceReadBytes+=count;runOnce(onSourceRead);}
   return count;
 }
 size_t write(const uint8_t* p,size_t n){
   fakeMillis+=ioMillis;n=std::min(n,writeBudget);f->write((char*)p,n);
   if(writeBudget!=SIZE_MAX)writeBudget-=n;
   return *f?n:0;
 }
 size_t size(){return std::filesystem::file_size(path);}
 bool seek(size_t p){f->seekp(p);return bool(*f);}
 void flush(){
   f->flush();
   if(corruptOnFlush){f->seekp(0);f->put('!');f->flush();}
 }
 void close(){if(f)f->close();}
};
struct Disk {
 bool failWrite=false;
 std::set<unsigned> failRenames;
 unsigned renameCalls=0;
 bool exists(const String& p){return std::filesystem::exists(root+p);}
 bool mkdir(const String& p){return std::filesystem::create_directory(root+p);}
 bool remove(const String& p){return std::filesystem::remove(root+p);}
 bool rename(const String& a,const String& b){
   if(failRenames.count(++renameCalls))return false;
   std::filesystem::rename(root+a,root+b);return true;
 }
 File open(const String& p,const char* mode="r"){if(failWrite&&*mode=='w')return {};return File(root+p,*mode=='w');}
 void resetFaults(){failWrite=false;failRenames.clear();renameCalls=0;writeBudget=SIZE_MAX;corruptOnFlush=false;}
} SD;
int cacheStat(const char* p,struct stat* s){
 std::string path=p;assert(path.rfind("/sd",0)==0);path=root+path.substr(3);
 if(path==watchedSource&&sourceStatError){errno=sourceStatError;return -1;}
 int result=::stat(path.c_str(),s);
 if(path==watchedSource)runOnce(onSourceStat);
 return result;
}
using mbedtls_sha256_context=SHA256_CTX;
void mbedtls_sha256_init(mbedtls_sha256_context*){}
void mbedtls_sha256_free(mbedtls_sha256_context*){}
void mbedtls_sha256_starts_ret(mbedtls_sha256_context* p,int){SHA256_Init(p);}
void mbedtls_sha256_update_ret(mbedtls_sha256_context* p,const uint8_t* b,size_t n){SHA256_Update(p,b,n);}
void mbedtls_sha256_finish_ret(mbedtls_sha256_context* p,uint8_t* out){SHA256_Final(out,p);}
void mbedtls_sha256_ret(const uint8_t* p,size_t n,uint8_t* out,int){SHA256(p,n,out);}
uint32_t millis(){return fakeMillis;}
// Model a 10 ms FreeRTOS tick even when firmware requests delay(1).
void delay(int ms){++delayCalls;fakeMillis+=ms?10:0;}
void* ps_malloc(size_t n){return malloc(n);}
struct {template<class... T>void printf(const char*,T...) {}} Serial;
constexpr size_t MAX_BOOK=4*1024*1024;char storage[MAX_BOOK+1];char* book=storage;size_t bookLength=0;
std::vector<uint32_t> pageOffsets;uint32_t pageNumber;String bookmarkKey,bookIdentity;
struct {
 std::map<std::string,uint32_t> data;bool failRead=false;
 bool isKey(const char* k){return data.find(k)!=data.end();}
 uint32_t getUInt(const char* k,uint32_t d){if(failRead)return d;auto i=data.find(k);return i==data.end()?d:i->second;}
} prefs;
bool bookEnglishHyphens=true;
text_language::Language bookLanguage=text_language::Language::EnglishUS;
void prepareReaderFont(){}
static uint32_t retainedOffset=UINT32_MAX,retainedPage=UINT32_MAX;
void retainReadingAnchor(uint32_t offset){retainedOffset=offset;retainedPage=pageNumber;}
#define stat(p,s) cacheStat(p,s)
#include "sd_cache.h"
#include "deferred_cache.h"
namespace reader_typography {const Style style;}
#undef stat
void writeSource(const std::string& p,const std::string& data){std::ofstream(root+p,std::ios::binary)<<data;}
std::string readFile(const std::string& p){std::ifstream f(root+p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
void replaceKeepingTimestamp(const std::string& p,const std::string& data){
 auto before=std::filesystem::last_write_time(root+p);
 writeSource(p,data);std::filesystem::last_write_time(root+p,before);
}
void seed(BookCache& c){bookDocument.clear();bookLanguage=text_language::Language::EnglishUS;c.dependencies.clear();c.observe("");bookLength=12;memcpy(book,"hello\nworld\n",12);pageOffsets={0,6};bookmarkKey="c0123456789abcd";}
void corrupt(const std::string& path,size_t pos){
 std::fstream f(root+path,std::ios::binary|std::ios::in|std::ios::out);f.seekg(pos);char v;f.get(v);v^=1;f.seekp(pos);f.put(v);
}
void refreshCacheDigests(std::string& cache,std::string& manifest){
 reader_cache::Header h;assert(cache.size()>=sizeof(h));memcpy(&h,cache.data(),sizeof(h));
 SHA256_CTX sha;SHA256_Init(&sha);SHA256_Update(&sha,&h,offsetof(reader_cache::Header,digest));
 SHA256_Update(&sha,cache.data()+sizeof(h),cache.size()-sizeof(h));SHA256_Final(h.digest,&sha);
 memcpy(cache.data(),&h,sizeof(h));
 content_manifest::Header m;assert(manifest.size()>=sizeof(m));memcpy(&m,manifest.data(),sizeof(m));
 memcpy(m.cacheDigest,h.digest,32);
 assert(cache.size()==m.prefixBytes+m.textBytes);
 for(size_t block=0;block<m.blocks;++block){
  const size_t at=m.prefixBytes+block*content_manifest::BLOCK_BYTES;
  const size_t count=std::min(size_t(content_manifest::BLOCK_BYTES),size_t(m.textBytes)-block*content_manifest::BLOCK_BYTES);
  SHA256((const uint8_t*)cache.data()+at,count,(uint8_t*)manifest.data()+sizeof(m)+block*32);
 }
 SHA256((const uint8_t*)cache.data(),m.prefixBytes,m.prefixDigest);
 SHA256_Init(&sha);SHA256_Update(&sha,&m,offsetof(content_manifest::Header,digest));
 SHA256_Update(&sha,manifest.data()+sizeof(m),manifest.size()-sizeof(m));SHA256_Final(m.digest,&sha);
 memcpy(manifest.data(),&m,sizeof(m));
}

void finish(DeferredCacheWrite& writer,BookCache& cache){
 for(unsigned i=0;!writer.done()&&!writer.failed();++i){
  assert(i<2000);const auto before=cache.work;writer.step();
  assert(cache.work.cacheReadBytes-before.cacheReadBytes<=8192&&cache.work.cacheWriteBytes-before.cacheWriteBytes<=8192);
 }
 assert(writer.done());
}
struct CancelProbe final:public reader_content::Source {
 static bool canceled;
 bool pin(size_t,size_t)override{return true;}
 size_t begin()const override{return 0;}
 size_t end()const override{return 0;}
 size_t length()const override{return 0;}
 void restoreSpan(size_t,size_t)override{}
 void cancelPrefetch()override{canceled=true;}
};
bool CancelProbe::canceled=false;
void cancelHookRegression(){
 CancelProbe::canceled=false;
 reader_content::install(std::unique_ptr<reader_content::Source>(new CancelProbe()));
 CancelProbe::canceled=false;
 reader_content::install(std::unique_ptr<reader_content::Source>(new CancelProbe()));
 assert(CancelProbe::canceled);
 CancelProbe::canceled=false;
 reader_content::clear();
 assert(CancelProbe::canceled);
 assert(!reader_content::active());
}

int main(){
 root=(std::filesystem::temp_directory_path()/("paperdiy-content-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();
 std::filesystem::create_directory(root);SD.mkdir("/.paperdiy-cache");
 std::string text;while(text.size()<1024*1024)text+="Synthetic chapter. Stable UTF-8 中文 and English text for bounded page access.\n";
 writeSource("/content.epub",text);storage_validation::beginExclusive();BookCache cache("/content.epub",false);assert(cache.observe("")==text.size());
 bookLength=text.size();memcpy(book,text.data(),text.size());bookIdentity="/content.epub";bookmarkKey="c0123456789abcd";
 bookDocument.clear();epub::structure::Collector collector;collector.begin();
 const auto block=collector.open(epub::structure::Kind::Paragraph,0,epub::structure::NONE,epub::structure::NONE,0,0,0);
 collector.text(0,bookLength,block,epub::structure::NONE,0,0,0);collector.close(block,bookLength);collector.finish(book,bookLength);
 assert(bookDocument.assign(collector,book,bookLength));
 pageOffsets.clear();for(size_t at=0;at<text.size();at+=400){while(at<text.size()&&(uint8_t(text[at])&0xc0)==0x80)++at;if(at<text.size())pageOffsets.push_back(uint32_t(at));}
 const auto pages=pageOffsets;const uint32_t anchor=pages[pages.size()*4/5]+7;prefs.data[bookmarkKey]=anchor;
 {DeferredCacheWrite writer(cache);assert(writer.begin(pages));finish(writer,cache);}
 assert(SD.exists(cache.path+".chunks")&&!SD.exists(cache.path+".chunks.tmp"));
 const auto complete=readFile(cache.path),manifest=readFile(cache.path+".chunks");
 assert(manifest.size()<=content_manifest::MAX_BYTES);
 auto openFresh=[&](){reader_content::clear();bookDocument.clear();bookLength=0;pageOffsets.clear();memset(book,0xa5,text.size());storage_validation::invalidate();storage_validation::beginExclusive();};
 openFresh();BookCache fast("/content.epub",false);assert(fast.load());
 assert(reader_content::active()&&bookDocument.ready()&&pageOffsets==pages&&retainedOffset==anchor);
 assert(fast.work.sourceBytes==text.size()); // Cold source verification remains mandatory.
 assert(fast.work.cacheReadBytes<128*1024&&reader_content::limit()-reader_content::begin()<64*1024);
 assert(!memcmp(book+reader_content::begin(),text.data()+reader_content::begin(),reader_content::limit()-reader_content::begin()));
 assert(uint8_t(book[0])==0xa5); // No hidden full-text load.
 const auto saved=prefs.data;const auto oldFirst=reader_content::begin(),oldLast=reader_content::limit();
 const std::string oldPage(book+oldFirst,book+oldLast);
 // A corrupted not-yet-read block cannot overwrite the visible page or move
 // its exact progress. Independent cache data is checked when requested.
 reader_cache::Header header;memcpy(&header,complete.data(),sizeof(header));
 corrupt(cache.path,sizeof(header)+header.metaBytes+header.pages*4+23);
 assert(!reader_content::pin(0,1000));assert(reader_content::begin()==oldFirst&&reader_content::limit()==oldLast);
 assert(std::string(book+oldFirst,book+oldLast)==oldPage&&prefs.data==saved&&retainedOffset==anchor);
 writeSource(cache.path,complete);assert(reader_content::pin(0,1000));assert(!memcmp(book,text.data(),1000));
 // Lease loss permits the verified resident bytes, but cannot authorize a new
 // SD read. The existing pinned span remains intact on that failed transition.
 const auto residentFirst=reader_content::begin(),residentLast=reader_content::limit();storage_validation::invalidate();
 assert(reader_content::pin(0,1000));assert(!reader_content::pin(text.size()/2,text.size()/2+1000));
 assert(reader_content::begin()==residentFirst&&reader_content::limit()==residentLast&&prefs.data==saved);
 const auto readBefore=static_cast<SdContentSource*>(reader_content::source().get())->readBytes();
 reader_content::source()->restoreSpan(oldFirst,oldLast);
 assert(reader_content::begin()==oldFirst&&reader_content::limit()==oldLast&&std::string(book+oldFirst,book+oldLast)==oldPage);
 assert(static_cast<SdContentSource*>(reader_content::source().get())->readBytes()==readBefore); // Rollback needs no lease or I/O.
 // Missing, torn and self-consistent-but-wrong optional manifests recover via
 // the original complete-cache envelope, never by ignoring its checksum.
 for(unsigned damage=0;damage<3;++damage){
  openFresh();std::string bad=manifest;
  if(damage==0)bad.resize(20);
  else if(damage==1)bad.back()^=1;
  else{content_manifest::Header h;memcpy(&h,bad.data(),sizeof(h));h.prefixDigest[0]^=1;
   SHA256_CTX sha;SHA256_Init(&sha);SHA256_Update(&sha,&h,offsetof(content_manifest::Header,digest));SHA256_Update(&sha,bad.data()+sizeof(h),bad.size()-sizeof(h));SHA256_Final(h.digest,&sha);memcpy(&bad[0],&h,sizeof(h));}
  writeSource(cache.path+".chunks",bad);BookCache fallback("/content.epub",false);assert(fallback.load());
  assert(!reader_content::active()&&fallback.work.cacheReadBytes>=text.size()&&!memcmp(book,text.data(),text.size())&&retainedOffset==anchor&&prefs.data==saved);
 }
 // A numerically ordered page table can still point inside a codepoint. Keep
 // both SHA envelopes consistent so only boundary validation rejects it.
 auto badPages=complete;auto badManifest=manifest;auto badHeader=header;
 const size_t following=pages.size()*4/5+1;
 const uint32_t split=uint32_t(text.find("中",pages[following])+1);
 assert(split>pages[following]&&split<pages[following+1]);
 memcpy(&badPages[sizeof(header)+header.metaBytes+following*4],&split,4);
 SHA256_CTX digest;SHA256_Init(&digest);SHA256_Update(&digest,&badHeader,offsetof(reader_cache::Header,digest));
 SHA256_Update(&digest,badPages.data()+sizeof(header),badPages.size()-sizeof(header));SHA256_Final(badHeader.digest,&digest);
 memcpy(&badPages[0],&badHeader,sizeof(badHeader));
 content_manifest::Header badMap;memcpy(&badMap,badManifest.data(),sizeof(badMap));memcpy(badMap.cacheDigest,badHeader.digest,32);
 SHA256((const uint8_t*)badPages.data(),badMap.prefixBytes,badMap.prefixDigest);
 SHA256_Init(&digest);SHA256_Update(&digest,&badMap,offsetof(content_manifest::Header,digest));
 SHA256_Update(&digest,badManifest.data()+sizeof(badMap),badManifest.size()-sizeof(badMap));SHA256_Final(badMap.digest,&digest);
 memcpy(&badManifest[0],&badMap,sizeof(badMap));writeSource(cache.path,badPages);writeSource(cache.path+".chunks",badManifest);
 openFresh();BookCache invalidBoundary("/content.epub",false);assert(!invalidBoundary.load());
 assert(!reader_content::active()&&bookLength==0&&pageOffsets.empty()&&prefs.data==saved);
 writeSource(cache.path,complete);
 writeSource(cache.path+".chunks",manifest);openFresh();BookCache restored("/content.epub",false);assert(restored.load()&&reader_content::active());
 // Same-size, same-time source replacement still rejects the fast path.
 reader_content::clear();replaceKeepingTimestamp("/content.epub",std::string(text.size(),'x'));storage_validation::invalidate();storage_validation::beginExclusive();
 BookCache replaced("/content.epub",false);assert(!replaced.load()&&!reader_content::active()&&prefs.data==saved);
 // ---- authenticated background fill ---------------------------------
 replaceKeepingTimestamp("/content.epub",text);
 writeSource(cache.path,complete);
 writeSource(cache.path+".chunks",manifest);
 openFresh();BookCache prefetch("/content.epub",false);assert(prefetch.load()&&reader_content::active());
 auto* prefetchSource=static_cast<SdContentSource*>(reader_content::source().get());
 assert(prefetchSource->prefetchSupported());
 const size_t prefetchFirst=reader_content::begin(),prefetchLast=reader_content::limit();
 const uint32_t prefetchPage=pageNumber,prefetchAnchor=retainedOffset;
 prefetchSource->beginPrefetch();assert(prefetchSource->prefetchPending());
 size_t prefetchSteps=0;
 while(prefetchSource->prefetchPending()){
  const auto before=prefetchSource->prefetchReadBytes();
  assert(prefetchSource->prefetchStep());
  assert(prefetchSource->prefetchReadBytes()-before<=8192);
  assert(reader_content::begin()==prefetchFirst&&reader_content::limit()==prefetchLast);
  assert(pageNumber==prefetchPage&&retainedOffset==prefetchAnchor);
  assert(++prefetchSteps<600);
 }
 assert(prefetchSource->prefetchComplete()&&!prefetchSource->prefetchFailed());
 assert(bookDocument.ready()&&bookDocument.validateSpan(0,text.size()));
 assert(reader_content::begin()==prefetchFirst&&reader_content::limit()==prefetchLast);
 reader_content::clear();
 assert(!reader_content::active()&&!memcmp(book,text.data(),text.size()));
 assert(pageNumber==prefetchPage&&retainedOffset==prefetchAnchor);

 // A cancelled fill can resume from its loaded-block bitmap when the resident
 // path re-arms it; cancellation must not permanently poison the source.
 openFresh();BookCache resume("/content.epub",false);assert(resume.load()&&reader_content::active());
 auto* resumeSource=static_cast<SdContentSource*>(reader_content::source().get());
 resumeSource->beginPrefetch();assert(resumeSource->prefetchPending());
 assert(resumeSource->prefetchStep());
 resumeSource->cancelPrefetch();
 assert(resumeSource->prefetchState()==reader_content::PrefetchState::Cancelled);
 assert(!resumeSource->prefetchPending());
 resumeSource->beginPrefetch();assert(resumeSource->prefetchPending());
 while(resumeSource->prefetchPending())resumeSource->prefetchStep();
 assert(resumeSource->prefetchComplete()&&!resumeSource->prefetchFailed());
 assert(bookDocument.ready()&&bookDocument.validateSpan(0,text.size()));
 assert(!memcmp(book,text.data(),text.size()));
 reader_content::clear();

 // A pin that sees a changed lease reports it so the app can reopen instead of
 // leaving the page turn silently stuck.
 openFresh();BookCache stalePin("/content.epub",false);assert(stalePin.load()&&reader_content::active());
 auto* staleSource=static_cast<SdContentSource*>(reader_content::source().get());
 storage_validation::invalidate();
 assert(!reader_content::pin(0,text.size()));
 assert(staleSource->leaseInvalidated());
 assert(reader_content::leaseInvalidated());
 reader_content::clear();

 // A mismatched block hash stops the fill without touching the visible span.
 writeSource(cache.path,complete);writeSource(cache.path+".chunks",manifest);
 openFresh();BookCache hashFail("/content.epub",false);assert(hashFail.load()&&reader_content::active());
 prefetchSource=static_cast<SdContentSource*>(reader_content::source().get());
 const size_t hashFirst=reader_content::begin(),hashLast=reader_content::limit();
 content_manifest::Header mh;memcpy(&mh,manifest.data(),sizeof(mh));
 corrupt(cache.path,mh.prefixBytes+size_t(mh.blocks-1)*content_manifest::BLOCK_BYTES+1);
 prefetchSource->beginPrefetch();
 while(prefetchSource->prefetchPending())prefetchSource->prefetchStep();
 assert(prefetchSource->prefetchFailed()&&prefetchSource->prefetchFailure()==reader_content::PrefetchFailure::Hash);
 assert(!prefetchSource->prefetchPending());
 prefetchSource->cancelPrefetch();
 assert(prefetchSource->prefetchFailed()); // cancellation does not turn a failure into a retry.
 assert(reader_content::begin()==hashFirst&&reader_content::limit()==hashLast);
 assert(reader_content::pin(hashFirst,hashLast));
 assert(!reader_content::pin(0,text.size()));
 reader_content::clear();writeSource(cache.path,complete);

 // Lease loss never falls back to a new lease for the remaining blocks.
 writeSource(cache.path,complete);writeSource(cache.path+".chunks",manifest);
 openFresh();BookCache leaseFail("/content.epub",false);assert(leaseFail.load()&&reader_content::active());
 prefetchSource=static_cast<SdContentSource*>(reader_content::source().get());
 const size_t leaseFirst=reader_content::begin(),leaseLast=reader_content::limit();
 prefetchSource->beginPrefetch();storage_validation::invalidate();
 assert(!prefetchSource->prefetchStep());
 assert(prefetchSource->prefetchFailed()&&prefetchSource->prefetchFailure()==reader_content::PrefetchFailure::Lease);
 assert(!prefetchSource->prefetchPending());
 assert(prefetchSource->leaseInvalidated());
 assert(reader_content::leaseInvalidated());
 assert(reader_content::begin()==leaseFirst&&reader_content::limit()==leaseLast);
 reader_content::clear();

 // Full UTF-8 is checked after every block is authenticated. The invalid byte
 // is deliberately in the final block, outside the visible page, with all
 // cache/manifest digests recomputed so only the UTF-8 stage can reject it.
 writeSource(cache.path,complete);writeSource(cache.path+".chunks",manifest);
 std::string badFill=complete,badFillManifest=manifest;
 memcpy(&mh,manifest.data(),sizeof(mh));
 badFill[mh.prefixBytes+mh.textBytes-2]=0xff;
 refreshCacheDigests(badFill,badFillManifest);
 writeSource(cache.path,badFill);writeSource(cache.path+".chunks",badFillManifest);
 openFresh();BookCache utf8Fail("/content.epub",false);assert(utf8Fail.load()&&reader_content::active());
 prefetchSource=static_cast<SdContentSource*>(reader_content::source().get());
 const size_t utf8First=reader_content::begin(),utf8Last=reader_content::limit();
 const uint64_t utf8Lease=storage_validation::lease();
 prefetchSource->beginPrefetch();
 while(prefetchSource->prefetchPending())prefetchSource->prefetchStep();
 assert(prefetchSource->prefetchFailed()&&prefetchSource->prefetchFailure()==reader_content::PrefetchFailure::Utf8);
 assert(!prefetchSource->prefetchPending());
 assert(!prefetchSource->leaseInvalidated());
 assert(storage_validation::lease()==utf8Lease);
 assert(reader_content::begin()==utf8First&&reader_content::limit()==utf8Last);
 reader_content::clear();
 writeSource(cache.path,complete);writeSource(cache.path+".chunks",manifest);

 // Injected semantic failure: all blocks are authenticated, but the document
 // cannot validate the complete semantic span. The visible page must not move.
 openFresh();BookCache semanticFail("/content.epub",false);assert(semanticFail.load()&&reader_content::active());
 prefetchSource=static_cast<SdContentSource*>(reader_content::source().get());
 const size_t semanticFirst=reader_content::begin(),semanticLast=reader_content::limit();
 const uint64_t semanticLease=storage_validation::lease();
 prefetchSource->beginPrefetch();bookDocument.clear();
 while(prefetchSource->prefetchPending())prefetchSource->prefetchStep();
 assert(prefetchSource->prefetchFailed()&&prefetchSource->prefetchFailure()==reader_content::PrefetchFailure::Semantic);
 assert(!prefetchSource->prefetchPending());
 assert(!prefetchSource->leaseInvalidated());
 assert(storage_validation::lease()==semanticLease);
 assert(reader_content::begin()==semanticFirst&&reader_content::limit()==semanticLast);
 reader_content::clear();

 // .bak has no separate block manifest, so it deliberately uses the complete
 // independently authenticated cache path rather than a partial prefetch.
 writeSource(cache.path,complete);writeSource(cache.path+".chunks",manifest);
 writeSource(cache.path+".bak",complete);assert(SD.remove(cache.path));
 openFresh();BookCache backup("/content.epub",false);assert(backup.load());
 assert(!reader_content::active()&&!memcmp(book,text.data(),text.size()));
 assert(SD.remove(cache.path+".bak"));writeSource(cache.path,complete);
 puts("authenticated background fill: one-block cursor, visible-span stability, hash/lease/UTF-8/semantic failure, .bak full-cache fallback: PASS");
 reader_content::clear();bookDocument.clear();std::filesystem::remove_all(root);
 puts("authenticated content blocks: cold source validation, deep-page read reduction, metadata/manifest integrity, failed pin and lease retention, complete-cache fallback: PASS");
 cancelHookRegression();
}
