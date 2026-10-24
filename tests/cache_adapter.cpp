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
void legacyFixture(BookCache& c,uint32_t layout=0x20261025,uint32_t version=1){
 seed(c);bookmarkKey=version==1?"b0123456789abcd":"c0123456789abcd";
 reader_cache::Header h{};h.magic=reader_cache::MAGIC;h.version=version;h.layout=layout;
 h.directory=c.directory;h.textBytes=bookLength;h.pages=pageOffsets.size();h.deps=c.dependencies.size();
 std::vector<uint8_t> data;
 auto append=[&](const void* p,size_t n){auto b=(const uint8_t*)p;data.insert(data.end(),b,b+n);};
 if(version==3){reader_cache::Extension ext{reader_cache::TEXT_REVISION,0,1,0};append(&ext,sizeof(ext));}
 for(const auto& dep:c.dependencies){uint32_t fields[3]={(uint32_t)dep.name.length(),dep.size,dep.mtime};append(fields,sizeof(fields));if(version>=2)append(dep.digest,sizeof(dep.digest));append(dep.name.c_str(),dep.name.length());}
 h.metaBytes=data.size();append(pageOffsets.data(),pageOffsets.size()*4);append(book,bookLength);
 memcpy(h.source,c.sourceHash,32);memcpy(h.bookmark,bookmarkKey.c_str(),16);
 SHA256_CTX sha;SHA256_Init(&sha);SHA256_Update(&sha,&h,offsetof(reader_cache::Header,digest));SHA256_Update(&sha,data.data(),data.size());SHA256_Final(h.digest,&sha);
 std::ofstream f(root+c.path,std::ios::binary);f.write((char*)&h,sizeof(h));f.write((char*)data.data(),data.size());
}
// Keep the image authenticated so wrong-layout rejection cannot pass merely
// because the checksum became invalid when the layout identifier was changed.
std::string withLayout(std::string image,uint32_t layout){
 assert(image.size()>=sizeof(reader_cache::Header));
 reader_cache::Header h;memcpy(&h,image.data(),sizeof(h));h.layout=layout;
 SHA256_CTX sha;SHA256_Init(&sha);
 SHA256_Update(&sha,&h,offsetof(reader_cache::Header,digest));
 SHA256_Update(&sha,image.data()+sizeof(h),image.size()-sizeof(h));
 SHA256_Final(h.digest,&sha);memcpy(&image[0],&h,sizeof(h));return image;
}
void verticalLayoutMigrationRegression(){
 const std::string source="/prior-layout.epub";writeSource(source,"synthetic source");
 BookCache cache(source,false);seed(cache);assert(cache.save());
 const auto current=readFile(cache.path);
 const auto previous=withLayout(current,0x20266000|(reader_cache::LAYOUT&2047u));
 writeSource(cache.path,previous);if(SD.exists(cache.path+".bak"))SD.remove(cache.path+".bak");
 pageOffsets.clear();bookLength=0;
 assert(!cache.load()&&cache.loadTextForReflow()&&bookLength==12);
 assert(readFile(cache.path)==previous); // Text recovery is read-only.
 assert(reader_cache::compatibleText(0x20266000|35,false));
 assert(!reader_cache::currentLayout(0x20266000|35));
 const auto interim=withLayout(current,0x20268000|(reader_cache::LAYOUT&2047u));
 writeSource(cache.path,interim);prefs.data["c0123456789abcd"]=7;
 const auto progress=prefs.data;pageOffsets.clear();bookLength=0;
 assert(!cache.load()&&!cache.loadTextForReflow()&&bookLength==0&&pageOffsets.empty());
 uint32_t anchor=UINT32_MAX;
 assert(cache.loadLegacyForMigration(anchor)&&anchor==7&&bookLength==12&&retainedOffset==7);
 assert(!reader_content::active()&&prefs.data==progress&&readFile(cache.path)==interim);
 assert(reader_cache::knownV3Layout(0x20268000|35)&&!reader_cache::compatibleText(0x20268000|35,false));
}
void semanticRevisionMigrationRegression(){
 const auto savedProgress=prefs.data;
 for(unsigned kind=0;kind<4;++kind){
  const bool directory=kind==1,epub=kind<2;
  const std::string source=kind==0?"/semantic-revision.epub":directory?"/semantic-directory":kind==2?"/semantic-revision.txt":"/semantic-revision.md";
  if(directory){std::filesystem::create_directories(root+source);writeSource(source+"/section.xhtml","synthetic source");}
  else writeSource(source,"synthetic source");
  BookCache cache(source,directory);seed(cache);bookEnglishHyphens=true;
  if(directory){cache.dependencies.clear();cache.enabled=true;assert(cache.observe("section.xhtml"));}
  assert(cache.save());const auto image=readFile(cache.path);
  reader_cache::Header header;reader_cache::Extension ext;
  memcpy(&header,image.data(),sizeof(header));memcpy(&ext,image.data()+sizeof(header),sizeof(ext));
  assert(ext.semanticRevision==(epub?reader_cache::EPUB_SEMANTIC_REVISION:0));
  // Every saved record survives reimport. The current content key supplies
  // the exact offset even when geometry and canonical text are unchanged.
  for(unsigned i=0;i<49;++i)prefs.data["preserved-"+std::to_string(i)]=i*37;
  prefs.data[header.bookmark]=7;const auto progress=prefs.data;
  for(uint32_t revision:{0u,1u,reader_cache::EPUB_SEMANTIC_REVISION+1}){
   if(!epub&&revision==0)continue;
   for(uint32_t family:{reader_cache::LAYOUT_BASE,0x20268000u,0x20266000u}){
    auto old=image;memcpy(&old[sizeof(header)+offsetof(reader_cache::Extension,semanticRevision)],&revision,sizeof(revision));
    old=withLayout(old,family|(header.layout&2047u));writeSource(cache.path,old);
    if(SD.exists(cache.path+".bak"))assert(SD.remove(cache.path+".bak"));
    bookLength=777;pageOffsets={0,111};pageNumber=99;retainedOffset=123;
    assert(!cache.load()&&!cache.loadTextForReflow());
    assert(bookLength==777&&pageOffsets==std::vector<uint32_t>({0,111})&&pageNumber==99&&retainedOffset==123);
    uint32_t offset=UINT32_MAX;
    if(epub&&revision<reader_cache::EPUB_SEMANTIC_REVISION){
     assert(cache.loadLegacyForMigration(offset)&&offset==7&&retainedOffset==7);
     assert(bookLength==12&&!memcmp(book,"hello\nworld\n",12)&&bookmarkKey==header.bookmark);
     assert(!reader_content::active()); // Migration always authenticates every old byte.
    }else assert(!cache.loadLegacyForMigration(offset));
    assert(prefs.data==progress&&readFile(cache.path)==old&&!SD.exists(cache.path+".tmp"));
   }
  }
  writeSource(cache.path,image);assert(cache.load()&&retainedOffset==7&&prefs.data==progress);
 }
 prefs.data=savedProgress;
}
void managedCacheRegression(){
 std::filesystem::create_directories(root+"/.paperdiy/lan");
 for(const char* path:{"/.paperdiy/lan/push0.md","/.paperdiy/lan/push1.md"}){
  writeSource(path,"synthetic managed document");BookCache cache(path,false);seed(cache);
  assert(!cache.enabled);
  // Even a pre-existing legacy image must not supply a rolling document.
  legacyFixture(cache,reader_cache::layoutFor(true,true),3);const auto image=readFile(cache.path);const auto progress=prefs.data;
  uint32_t offset=123;const auto lease=storage_validation::lease();
  assert(!cache.load()&&!cache.loadTextForReflow()&&!cache.loadLegacyForMigration(offset)&&!cache.loadPrepared()&&!cache.preparedChanged());
  assert(!cache.recoveryRequired()&&offset==123&&!cache.save());
  DeferredCacheWrite writer(cache);assert(!writer.begin(pageOffsets));
  assert(prefs.data==progress&&readFile(cache.path)==image&&storage_validation::lease()==lease);
 }
}
void sourceStampRegression(){
 const std::string source="/stamp.txt",original(3*8192+1,'a');
 writeSource(source,original);BookCache c(source,false);
 seed(c);assert(c.save()&&c.save()); // Both current and backup are authenticated.
 const std::string image=readFile(c.path);
 const auto stamp=std::filesystem::last_write_time(root+source);
 watchedSource=root+source;
 auto restore=[&](){
   writeSource(source,original);std::filesystem::last_write_time(root+source,stamp);
   sourceReadBytes=0;
 };
 auto intact=[&](){
   assert(readFile(c.path)==image&&readFile(c.path+".bak")==image);
   assert(!SD.exists(c.path+".tmp"));
 };
 // Rejected candidates must not publish a book or move a saved anchor.
 bookLength=777;pageOffsets={0,111};pageNumber=99;
 bookmarkKey="unchanged-key";bookIdentity="unchanged-identity";
 retainedOffset=123;retainedPage=456;
 const auto progress=prefs.data;
 auto rejected=[&](){
   assert(!c.load()&&c.recoveryRequired());
   assert(!c.ioFailed); // Missing/replaced content must not lower the SD clock.
   assert(bookLength==777&&pageOffsets==std::vector<uint32_t>({0,111})&&pageNumber==99);
   assert(bookmarkKey=="unchanged-key"&&bookIdentity=="unchanged-identity");
   assert(retainedOffset==123&&retainedPage==456&&prefs.data==progress);
   intact();
 };
 // Stamp mismatches reject both candidates without reading any source bytes.
 writeSource(source,original+"b");sourceReadBytes=0;rejected();assert(sourceReadBytes==0);
 restore();std::filesystem::last_write_time(root+source,stamp+std::chrono::seconds(60));
 rejected();assert(sourceReadBytes==0);
 restore();assert(SD.remove(source));rejected();assert(sourceReadBytes==0);

 // Equal size/mtime are only a filter: every candidate still hashes all bytes.
 restore();std::string changed=original;changed[16384]='b';
 replaceKeepingTimestamp(source,changed);rejected();assert(sourceReadBytes==2*original.size());
 restore();onSourceStat=[&](){replaceKeepingTimestamp(source,changed);};
 rejected();assert(!onSourceStat&&sourceReadBytes==2*original.size());

 // A source modified during hashing fails the final stamp check. The backup
 // then sees the changed stamp and must not rehash that known-stale source.
 restore();onSourceRead=[&](){
   std::filesystem::last_write_time(root+source,stamp+std::chrono::seconds(60));
 };
 rejected();assert(!onSourceRead&&sourceReadBytes==original.size());

 restore();prefs.data["c0123456789abcd"]=7;
 assert(c.load()&&sourceReadBytes==original.size());
 assert(bookLength==12&&!memcmp(book,"hello\nworld\n",12));
 assert(pageNumber==1&&retainedOffset==7&&retainedPage==1);intact();

 // Current and backup may describe different sources: skip stale current,
 // then fully verify and publish the matching backup with its exact anchor.
 writeSource(source,original+"b");seed(c);assert(c.save());
 restore();assert(c.load()&&sourceReadBytes==original.size());
 assert(pageNumber==1&&retainedOffset==7&&retainedPage==1);
 writeSource(c.path,image);writeSource(c.path+".bak",image);

 // Import observes source before parsing. A subsequent change must prevent
 // save/rotation, whether its stamp changed or only its content changed.
 restore();seed(c);writeSource(source,original+"b");sourceReadBytes=0;
 assert(!c.save()&&sourceReadBytes==0);intact();
 restore();seed(c);replaceKeepingTimestamp(source,changed);sourceReadBytes=0;
 assert(!c.save()&&sourceReadBytes==original.size());intact();
 watchedSource.clear();
}
void styleCacheRegression(){
 assert(reader_typography::style.flags()==3);
 for(bool markdown:{false,true})for(unsigned saved=0;saved<4;++saved){
  const std::string source=markdown?"/style.md":"/style.txt";
  writeSource(source,"original style source");BookCache cache(source,false);
  SD.remove(cache.path);SD.remove(cache.path+".bak");
  // Authenticated caches from older selectable layouts remain migration inputs.
  legacyFixture(cache,reader_cache::layoutFor(true,markdown,saved),3);
  prefs.data["c0123456789abcd"]=7;prefs.data["cnStyle"]=0;
  const auto progress=prefs.data;const std::string image=readFile(cache.path);
  bookLength=777;pageOffsets={0,111};pageNumber=99;retainedOffset=123;retainedPage=456;
  if(saved==3){assert(cache.load()&&retainedOffset==7&&pageNumber==1);}
  else{
   assert(!cache.load()&&cache.recoveryRequired());
   assert(bookLength==777&&pageOffsets==std::vector<uint32_t>({0,111})&&pageNumber==99&&retainedOffset==123);
   uint32_t offset=0;assert(cache.loadLegacyForMigration(offset)&&offset==7&&retainedOffset==7);
  }
  assert(reader_typography::style.flags()==3&&prefs.data==progress&&readFile(cache.path)==image);
  assert(readFile(source)=="original style source"&&!SD.exists(cache.path+".bak"));
 }
}
void textReuseRegression(){
 const auto config=reader_settings::current();
 const std::string source="/reflow.epub";
 writeSource(source,"original source");BookCache c(source,false);
 seed(c);bookEnglishHyphens=false;assert(c.save());
 const std::string image=readFile(c.path);const auto progress=prefs.data;
 prefs.data["c0123456789abcd"]=7;
 reader_settings::current().preset=0;
 assert(!c.load());
 assert(c.loadTextForReflow()&&bookLength==12&&retainedOffset==7&&!bookEnglishHyphens);
 assert(bookmarkKey=="c0123456789abcd"&&bookIdentity==source);
 assert(readFile(c.path)==image&&!SD.exists(c.path+".bak"));
 // Reuse checks the entire text checksum, source bytes, mode and an explicit
 // extraction revision. A legacy cache may recover an anchor, but never supply
 // today's extraction merely because its envelope is well formed.
 for(uint32_t layout:{0x20261040u,0x20263023u,reader_cache::LAYOUT_MARKDOWN}){
   writeSource(c.path,withLayout(image,layout));assert(!c.loadTextForReflow());
 }
 writeSource(c.path,image);corrupt(c.path,image.size()-1);assert(!c.loadTextForReflow());
 writeSource(c.path,image);replaceKeepingTimestamp(source,"ORIGINAL SOURCE");assert(!c.loadTextForReflow());
 replaceKeepingTimestamp(source,"original source");assert(c.loadTextForReflow());
 prefs.failRead=true;assert(!c.loadTextForReflow());prefs.failRead=false;
 prefs.data["c0123456789abcd"]=12;assert(!c.loadTextForReflow());
 prefs.data=progress;reader_settings::current()=config;bookEnglishHyphens=true;
}
void exclusiveSourceRegression(){
 storage_validation::invalidate();
 const std::string source="/exclusive.epub",original(3*8192+1,'s');
 writeSource(source,original);BookCache c(source,false);seed(c);assert(c.save());
 prefs.data["c0123456789abcd"]=7;const std::string image=readFile(c.path);
 watchedSource=root+source;sourceReadBytes=0;
 storage_validation::beginExclusive();
 assert(c.load()&&sourceReadBytes==original.size());
 sourceReadBytes=0;BookCache reopened(source,false);assert(reopened.load());
 assert(sourceReadBytes==0&&reopened.work.reusedGraphs==1&&retainedOffset==7);
 // Cached source fingerprints never bypass cache payload checksums.
 corrupt(c.path,image.size()-1);assert(!reopened.load());writeSource(c.path,image);
 // The owner invalidates before every AP/USB/source write, even if size and
 // timestamp are retained. This must still reject substituted book content.
 storage_validation::sourceChanged();std::string changed=original;changed[8192]='t';
 replaceKeepingTimestamp(source,changed);sourceReadBytes=0;
 assert(!c.load()&&sourceReadBytes==original.size());
 storage_validation::sourceChanged();replaceKeepingTimestamp(source,original);
 sourceReadBytes=0;assert(c.load()&&sourceReadBytes==original.size());
 // Remount and host ownership changes force fresh validation.
 storage_validation::invalidate();sourceReadBytes=0;
 assert(c.load()&&sourceReadBytes==original.size()&&!storage_validation::lease());
 storage_validation::beginExclusive();sourceReadBytes=0;
 assert(c.load()&&sourceReadBytes==original.size());
 storage_validation::beginExclusive();sourceReadBytes=0;
 assert(c.load()&&sourceReadBytes==original.size());
 // A mutation during validation invalidates the result, not just its memo.
 // The following open must perform another complete read.
 storage_validation::sourceChanged();sourceReadBytes=0;
 onSourceRead=[](){storage_validation::sourceChanged();};
 assert(!c.load()&&sourceReadBytes==original.size()&&!storage_validation::lease());
 sourceReadBytes=0;assert(c.load()&&sourceReadBytes==original.size());
 // GPIO events invalidate proofs immediately, even while synchronous open
 // prevents the app loop from seeing an unplug/reinsert pair.
 storage_validation::beginExclusive();sourceReadBytes=0;assert(c.load());
 storage_validation::signalCardEvent();assert(storage_validation::cardEventPending()&&!storage_validation::lease());
 replaceKeepingTimestamp(source,changed);sourceReadBytes=0;
 assert(!c.load()&&sourceReadBytes==original.size());
 replaceKeepingTimestamp(source,original);storage_validation::beginExclusive();
 assert(!storage_validation::cardEventPending());sourceReadBytes=0;
 onSourceRead=[](){storage_validation::signalCardEvent();};
 assert(!c.load()&&sourceReadBytes==original.size()&&!storage_validation::lease());
 storage_validation::beginExclusive();sourceReadBytes=0;
 assert(c.load()&&sourceReadBytes==original.size());
 storage_validation::sourceChanged();sourceReadBudget=8192;
 assert(!c.load()&&!storage_validation::lease());sourceReadBudget=SIZE_MAX;
 sourceReadBytes=0;assert(c.load()&&sourceReadBytes==original.size());
 watchedSource.clear();storage_validation::invalidate();

 // Bounded eviction loses only speed; it cannot transfer trust to another key.
 storage_validation::beginExclusive();uint8_t key[32]={},digest[32]={},out[32];
 for(size_t i=0;i<storage_validation::CAPACITY+1;++i){
   key[0]=uint8_t(i);digest[0]=uint8_t(i+1);
   storage_validation::remember(storage_validation::lease(),key,123,456,digest);
 }
 key[0]=0;assert(!storage_validation::find(key,123,456,out));
 key[0]=1;assert(storage_validation::find(key,123,456,out)&&out[0]==2);
 assert(!storage_validation::find(key,124,456,out));
 const auto beforeEdge=storage_validation::lease();storage_validation::rememberGraph(beforeEdge,key,digest);
 storage_validation::signalCardEvent();
 assert(!storage_validation::find(key,123,456,out)&&!storage_validation::verifiedGraph(key,digest));
 storage_validation::remember(beforeEdge,key,123,456,digest);
 storage_validation::rememberGraph(beforeEdge,key,digest);
 assert(!storage_validation::lease());storage_validation::sourceChanged();assert(!storage_validation::lease());
 storage_validation::invalidate();storage_validation::acknowledgeCardEvent();
 assert(!storage_validation::cardEventPending()&&!storage_validation::lease());
 assert(!storage_validation::find(key,123,456,out)&&!storage_validation::verifiedGraph(key,digest));
 storage_validation::beginExclusive();
 assert(storage_validation::lease()!=beforeEdge&&!storage_validation::find(key,123,456,out)&&!storage_validation::verifiedGraph(key,digest));
 storage_validation::invalidate();assert(!storage_validation::find(key,123,456,out));
}
void semanticAndPreparedRegression(){
 const std::string source="/prepared.epub",original(256,'z');writeSource(source,original);
 BookCache c(source,false);seed(c);bookEnglishHyphens=true;bookLanguage=text_language::Language::EnglishGB;
 epub::structure::Collector collector;collector.begin();
 const auto language=collector.internLanguage("en-GB");
 auto paragraph=collector.open(epub::structure::Kind::Heading,0,epub::structure::NONE,epub::structure::NONE,0,0,language,2);
 collector.text(0,12,paragraph,epub::structure::NONE,epub::structure::Strong,1,language);
 collector.close(paragraph,12);collector.finish(book,bookLength);
 assert(bookDocument.assign(collector,book,bookLength));
 std::vector<uint8_t> metadata;assert(bookDocument.encode(metadata));
 memset(c.preparedDigest,0x5a,sizeof(c.preparedDigest));
 assert(c.save());prefs.data["c0123456789abcd"]=7;
 const std::string image=readFile(c.path);
 bookEpubTransientFallback=true;assert(!c.save());
 assert(readFile(c.path)==image&&!SD.exists(c.path+".tmp"));
 assert(c.load()&&!bookEpubTransientFallback&&bookDocument.ready());
 bookDocument.clear();bookLanguage=text_language::Language::None;
 assert(c.load()&&c.languageKnown&&bookLanguage==text_language::Language::EnglishGB&&bookDocument.ready());
 assert(bookDocument.run(0).flags&markdown::Heading);
 assert(bookDocument.run(0).flags&markdown::Strong);
 assert(bookDocument.run(0).language==text_language::Language::EnglishGB&&retainedOffset==7);
 reader_cache::Header h;memcpy(&h,image.data(),sizeof(h));
 assert(h.version==3&&(h.layout&1536u)==1536u);
 // The first semantic firmware has a different page geometry, but identical
 // extraction and v3 metadata. Reflow must retain all metadata and the exact
 // byte anchor while refusing its obsolete page table.
 const uint32_t earlier=0x20264000u|(h.layout&2047u);
 writeSource(c.path,withLayout(image,earlier));BookCache prior(source,false);
 assert(!prior.load()&&prior.loadTextForReflow());
 assert(prior.languageKnown&&bookLanguage==text_language::Language::EnglishGB&&bookDocument.ready());
 assert(bookDocument.run(0).language==text_language::Language::EnglishGB&&retainedOffset==7);
 assert(!memcmp(prior.preparedDigest,c.preparedDigest,32)&&prior.preparedDigest[0]==0x5a);
 for(uint32_t mismatch:{earlier&~512u,earlier&~1024u,0x2026c000u|(h.layout&2047u)}){
   writeSource(c.path,withLayout(image,mismatch));assert(!prior.loadTextForReflow());
 }
 writeSource(c.path,image);
 auto altered=image;altered[sizeof(h)+8]=char(255); // Invalid language, with valid envelope SHA.
 writeSource(c.path,withLayout(altered,h.layout));assert(!c.load());
 altered=image;altered[sizeof(h)+sizeof(reader_cache::Extension)+reader_cache::DEP_BYTES+metadata.size()-1]=char(1);
 writeSource(c.path,withLayout(altered,h.layout));assert(!c.load()); // Reserved Doc byte, authenticated envelope.
 writeSource(c.path,image);assert(c.load());
 const auto config=reader_settings::current();reader_settings::current().preset=0;
 assert(!c.load()&&c.loadTextForReflow()&&bookDocument.ready()&&retainedOffset==7);
 reader_settings::current()=config;

 auto prepared=[&](const std::string& text,const std::vector<uint8_t>& meta){
   prepared_epub::Header header;header.sourceBytes=original.size();header.textBytes=text.size();header.metadataBytes=meta.size();header.flags=2;
   SHA256((const uint8_t*)original.data(),original.size(),header.sourceDigest);
   SHA256_CTX sha;SHA256_Init(&sha);SHA256_Update(&sha,text.data(),text.size());SHA256_Update(&sha,meta.data(),meta.size());SHA256_Final(header.payloadDigest,&sha);
   std::string result(prepared_epub::HEADER_BYTES,'\0');assert(prepared_epub::encode(header,(uint8_t*)&result[0],result.size()));
   result+=text;if(!meta.empty())result.append((const char*)meta.data(),meta.size());return result;
 };
 const auto good=prepared("hello\nworld\n",metadata);writeSource(source+".pdt",good);
 assert(c.preparedChanged());
 assert(c.loadPrepared()&&bookLanguage==text_language::Language::EnglishGB&&bookDocument.ready()&&bookLength==12&&!c.preparedChanged());
 pageOffsets={0,6};bookmarkKey="c0123456789abcd";assert(c.save());
 BookCache resumed(source,false);assert(resumed.load()&&!resumed.preparedChanged()&&retainedOffset==7);
 const auto flat=prepared("hello\nworld\n",{});writeSource(source+".pdt",flat);
 assert(c.preparedChanged());
 assert(c.loadPrepared()&&!bookDocument.ready()&&bookLength==12);
 for(size_t at:{size_t(12),size_t(32),size_t(64),prepared_epub::HEADER_BYTES+1}){
   auto bad=good;bad[at]^=1;writeSource(source+".pdt",bad);bookLength=777;
   assert(!c.loadPrepared()&&bookLength==777);
 }
 auto invalidMetadata=metadata;invalidMetadata.back()=1;
 writeSource(source+".pdt",prepared("hello\nworld\n",invalidMetadata));assert(!c.loadPrepared());
 writeSource(source+".pdt",prepared(std::string("\xc0\xaf",2),{}));assert(!c.loadPrepared());
 writeSource(source+".pdt",good);assert(c.loadPrepared());
 c.dependencies.clear();replaceKeepingTimestamp(source,std::string(256,'Z'));assert(c.observe("")==original.size());
 assert(!c.loadPrepared());bookDocument.clear();bookLanguage=text_language::Language::EnglishUS;
}
void ioFailureRegression(){
 storage_validation::invalidate();const std::string source="/io-source.epub",original(8193,'x');
 writeSource(source,original);watchedSource=root+source;
 // A resource-size callback must distinguish I/O loss from an absent optional
 // EPUB file so the reader can remount at a lower bus clock.
 sourceReadBudget=17;BookCache shortSource(source,false);
 assert(shortSource.observe("")==SIZE_MAX&&shortSource.ioFailed&&!storage_validation::lease());
 sourceReadBudget=SIZE_MAX;sourceStatError=EIO;BookCache failedStat(source,false);
 assert(failedStat.observe("")==SIZE_MAX&&failedStat.ioFailed);
 sourceStatError=ENOENT;BookCache absent(source,false);
 assert(absent.observe("")==SIZE_MAX&&!absent.ioFailed);
 sourceStatError=0;BookCache fresh(source,false);seed(fresh);assert(fresh.save()&&!fresh.ioFailed);
 const auto image=readFile(fresh.path);watchedSource=root+fresh.path;
 // A valid-length file that returns too few bytes is I/O failure. A complete
 // read of an already truncated/corrupt file is simply an invalid cache.
 for(size_t budget:{size_t(17),sizeof(reader_cache::Header)+size_t(17)}){
   sourceReadBudget=budget;BookCache interrupted(source,false);
   assert(!interrupted.load()&&interrupted.ioFailed);
 }
 sourceReadBudget=SIZE_MAX;writeSource(fresh.path,"truncated");BookCache malformed(source,false);
 assert(!malformed.load()&&!malformed.ioFailed);
 writeSource(fresh.path,image);corrupt(fresh.path,image.size()-1);BookCache checksum(source,false);
 assert(!checksum.load()&&!checksum.ioFailed);
 writeSource(fresh.path,image);BookCache recovered(source,false);assert(recovered.load()&&!recovered.ioFailed);
 watchedSource.clear();storage_validation::invalidate();
}
void largeDependencyGraphRegression(){
 storage_validation::invalidate();const std::string source="/large-dir.epub";
 std::filesystem::create_directory(root+source);BookCache c(source,true);
 const size_t count=storage_validation::CAPACITY*2+1;const std::string resource(97,'x');
 for(size_t i=0;i<count;++i){
   const auto name="part-"+std::to_string(i)+".xhtml";writeSource(source+"/"+name,resource);
   assert(c.observe(name)==resource.size());
 }
 bookDocument.clear();bookLanguage=text_language::Language::EnglishUS;bookEnglishHyphens=true;
 bookLength=12;memcpy(book,"hello\nworld\n",12);pageOffsets={0,6};bookmarkKey="c0123456789abcd";assert(c.save());
 storage_validation::beginExclusive();BookCache first(source,true);assert(first.load());
 assert(first.work.sourceBytes==count*resource.size());
 BookCache warm(source,true);assert(warm.load());
 assert(warm.work.sourceBytes==0&&warm.work.reusedGraphs==1&&warm.dependencies.size()==count);
 // A single file modification in a graph larger than the per-file memo must
 // still be detected after the owning writer announces its mutation.
 storage_validation::sourceChanged();
 replaceKeepingTimestamp(source+"/part-128.xhtml",std::string(97,'y'));
 assert(!warm.load());
 storage_validation::sourceChanged();replaceKeepingTimestamp(source+"/part-128.xhtml",resource);
 assert(warm.load());storage_validation::invalidate();
 BookCache remounted(source,true);assert(remounted.load()&&remounted.work.sourceBytes==count*resource.size());
}
void residentRegression(){
 resident_book::Snapshot resident;const auto config=reader_settings::current();
 storage_validation::beginExclusive();const auto generation=storage_validation::lease();
 const auto shape=reader_cache::layoutFor(true,false,3,true,true);
 resident.remember("/one.epub","c0123456789abcd",generation,shape,12345);
 assert(resident.matches("/one.epub","c0123456789abcd",generation,shape,12345));
 assert(!resident.matches("/two.epub","c0123456789abcd",generation,shape,12345));
 assert(!resident.matches("/one.epub","c9876543210abcd",generation,shape,12345));
 assert(!resident.matches("/one.epub","c0123456789abcd",generation,shape,12346));
 reader_settings::current().preset=0;
 assert(!resident.matches("/one.epub","c0123456789abcd",generation,reader_cache::layoutFor(true,false,3,true,true),12345));
 reader_settings::current()=config;
 assert(!resident.matches("/one.epub","c0123456789abcd",generation,reader_cache::layoutFor(true,false,3,true,false),12345));
 storage_validation::signalCardEvent();
 assert(!resident.matches("/one.epub","c0123456789abcd",storage_validation::lease(),shape,12345));
 storage_validation::beginExclusive();
 assert(!resident.matches("/one.epub","c0123456789abcd",storage_validation::lease(),shape,12345));
 storage_validation::sourceChanged();
 assert(!resident.matches("/one.epub","c0123456789abcd",storage_validation::lease(),shape,12345));
 storage_validation::invalidate();
 assert(!resident.matches("/one.epub","c0123456789abcd",storage_validation::lease(),shape,12345));
 resident.clear();assert(!resident.matches("/one.epub","c0123456789abcd",generation,shape,12345));
 resident.remember("/one.epub","c0123456789abcd",0,shape,12345);assert(!resident.generation);
}
void deferredPublicationRegression(){
 SD.resetFaults();bookEpubTransientFallback=false;bookDocument.clear();
 const std::string source="/deferred.epub",payload(100003,'a');writeSource(source,payload);
 storage_validation::beginExclusive();BookCache cache(source,false);cache.observe("");
 bookIdentity=source;bookLength=payload.size();memcpy(book,payload.data(),payload.size());
 bookmarkKey="c0123456789abcd";bookEnglishHyphens=true;bookLanguage=text_language::Language::EnglishUS;
 const std::vector<uint32_t> canonical={0,12000,24000,50000,90000};pageOffsets=canonical;
 assert(cache.save());const std::string expected=readFile(cache.path);
 assert(cache.save());const auto originalProgress=prefs.data;
 // Visible local-window offsets must never leak into the published cache.
 pageOffsets={50100};pageNumber=0;retainedOffset=50105;
 auto run=[&](DeferredCacheWrite& writer){
   unsigned steps=0;
   while(!writer.done()&&!writer.failed()){
     const auto before=cache.work;writer.step();
     assert(cache.work.cacheWriteBytes-before.cacheWriteBytes<=8192);
     assert(cache.work.cacheReadBytes-before.cacheReadBytes<=8192);
     assert(++steps<200);assert(prefs.data==originalProgress);
     assert(pageOffsets==std::vector<uint32_t>({50100})&&retainedOffset==50105);
   }
   return steps;
 };
 {DeferredCacheWrite writer(cache);assert(writer.begin(canonical));assert(run(writer)>30);assert(writer.done());}
 assert(readFile(cache.path)==expected&&readFile(cache.path+".bak")==expected);
 // Any interrupted phase keeps a complete older generation. Neither short
 // writes, corrupt flushes, rename failures nor a revoked card lease publish.
 for(unsigned fault=0;fault<5;++fault){
   SD.resetFaults();storage_validation::beginExclusive();cache.ioFailed=false;
   writeSource(cache.path,expected);writeSource(cache.path+".bak",expected);
   {
     DeferredCacheWrite writer(cache);assert(writer.begin(canonical));
     if(fault==0)writeBudget=sizeof(reader_cache::Header)+20;
     if(fault==1)corruptOnFlush=true;
     if(fault==2)SD.failRenames={SD.renameCalls+1};
     if(fault==3)SD.failRenames={SD.renameCalls+2};
     if(fault==4){writer.step();storage_validation::sourceChanged();}
     run(writer);assert(writer.failed());
   }
   assert(readFile(cache.path)==expected||readFile(cache.path+".bak")==expected);
 }
 // A corrupt primary cannot cause a good rollback copy to be discarded.
 SD.resetFaults();storage_validation::beginExclusive();cache.ioFailed=false;
 writeSource(cache.path,expected);writeSource(cache.path+".bak",expected);corrupt(cache.path,0);
 {DeferredCacheWrite writer(cache);assert(writer.begin(canonical));run(writer);assert(writer.done());}
 assert(readFile(cache.path)==expected&&readFile(cache.path+".bak")==expected);
 SD.resetFaults();storage_validation::invalidate();
}
int main(){
 root=(std::filesystem::temp_directory_path()/("paperdiy-cache-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();assert(std::filesystem::create_directory(root));
 writeSource("/book.txt","original source");BookCache c("/book.txt",false);
 uint32_t noLegacy=0;assert(!c.load()&&!c.loadLegacyForMigration(noLegacy)&&!c.recoveryRequired());
 seed(c);assert(c.save());
 prefs.data[bookmarkKey]=6;pageOffsets.clear();memset(book,0,12);assert(c.load()&&!c.recoveryRequired());assert(pageNumber==1&&pageOffsets.size()==2&&!memcmp(book,"hello\nworld\n",12));
 prefs.failRead=true;pageNumber=99;assert(!c.load()&&c.recoveryRequired()&&pageNumber==99);
 prefs.failRead=false;assert(c.load()&&pageNumber==1);
 prefs.data[bookmarkKey]=7;assert(c.load()&&retainedOffset==7&&retainedPage==1);
 prefs.failRead=true;assert(!c.load()&&retainedOffset==7&&retainedPage==1);
 prefs.failRead=false;
 for(uint32_t invalid : {12u,UINT32_MAX-1}){
   prefs.data[bookmarkKey]=invalid;pageNumber=99;auto before=prefs.data;
   assert(!c.load()&&c.recoveryRequired()&&pageNumber==99&&prefs.data==before);
 }
 prefs.data[bookmarkKey]=6;assert(c.load()&&pageNumber==1);
 corrupt(c.path,std::filesystem::file_size(root+c.path)-1);assert(!c.load());seed(c);assert(c.save());
 corrupt(c.path,offsetof(reader_cache::Header,layout));assert(!c.load());seed(c);assert(c.save());
 std::filesystem::resize_file(root+c.path,40);assert(!c.load());seed(c);assert(c.save());
 writeSource("/book.txt","modified longer source");assert(!c.load());seed(c);assert(c.save());
 BookCache other("/other.txt",false);std::filesystem::copy_file(root+c.path,root+other.path);assert(!other.load());

 // A preserved size and mtime must never hide changed source bytes.
 replaceKeepingTimestamp("/book.txt","modified LONGER source");assert(!c.load());seed(c);assert(c.save());
 writeSource("/book.txt",std::string(32769,'a'));seed(c);assert(c.save());
 std::string changed(32769,'a');changed[16384]='b';replaceKeepingTimestamp("/book.txt",changed);assert(!c.load());seed(c);assert(c.save());

 // Failed writes, failed flushes, and both stages of rotation keep the old image.
 std::string old=readFile(c.path);
 SD.failWrite=true;assert(!c.save());SD.resetFaults();assert(readFile(c.path)==old&&c.load());
 writeBudget=sizeof(reader_cache::Header)+5;assert(!c.save());SD.resetFaults();assert(readFile(c.path)==old&&c.load());
 corruptOnFlush=true;assert(!c.save());SD.resetFaults();assert(readFile(c.path)==old&&c.load());
 SD.failRenames={1};assert(!c.save());SD.resetFaults();assert(readFile(c.path)==old&&c.load());
 SD.failRenames={2};assert(!c.save());SD.resetFaults();assert(readFile(c.path)==old&&c.load());
 SD.failRenames={2,3};assert(!c.save());SD.resetFaults();assert(!SD.exists(c.path)&&readFile(c.path+".bak")==old&&c.load());
 // Recovery after a power cut between backup rotation and final rename.
 assert(c.save());assert(SD.exists(c.path)&&SD.exists(c.path+".bak")&&c.load());
 std::filesystem::copy_file(root+c.path,root+c.path+".bak",std::filesystem::copy_options::overwrite_existing);
 corrupt(c.path,std::filesystem::file_size(root+c.path)-1);assert(c.load());assert(c.save());assert(c.load()&&SD.exists(c.path+".bak"));

 std::filesystem::create_directory(root+"/dir.epub");writeSource("/dir.epub/chapter.xhtml","chapter");
 BookCache d("/dir.epub",true);d.observe("chapter.xhtml");d.observe("META-INF/encryption.xml");assert(d.save());assert(d.load());
 replaceKeepingTimestamp("/dir.epub/chapter.xhtml","CHAPTER");assert(!d.load());
 d.dependencies.clear();d.observe("chapter.xhtml");d.observe("META-INF/encryption.xml");assert(d.save());
 std::filesystem::create_directory(root+"/dir.epub/META-INF");writeSource("/dir.epub/META-INF/encryption.xml","added");assert(!d.load());

 // Legacy cache is never a normal hit and migration preserves exact offsets.
 SD.remove(c.path+".bak");
 uint32_t offset=UINT32_MAX;
 for(uint32_t layout:{0x20261019u,0x20261020u,0x20261023u,0x20261024u,0x20261025u}){
   legacyFixture(c,layout);prefs.data["b0123456789abcd"]=7;old=readFile(c.path);
   assert(!c.load()&&c.recoveryRequired());assert(c.loadLegacyForMigration(offset)&&c.recoveryRequired());
   assert(offset==7&&pageNumber==1&&bookmarkKey=="b0123456789abcd"&&!memcmp(book,"hello\nworld\n",12));
   assert(readFile(c.path)==old&&!SD.exists(c.path+".bak"));
 }
 prefs.failRead=true;assert(!c.loadLegacyForMigration(offset)&&c.recoveryRequired());prefs.failRead=false;
 prefs.data["b0123456789abcd"]=12;assert(!c.loadLegacyForMigration(offset));prefs.data["b0123456789abcd"]=7;
 corrupt(c.path,std::filesystem::file_size(root+c.path)-1);assert(!c.loadLegacyForMigration(offset)&&c.recoveryRequired());
 assert(!c.load()&&c.recoveryRequired());
 legacyFixture(c);std::filesystem::resize_file(root+c.path,40);
 assert(!c.load()&&!c.loadLegacyForMigration(offset)&&c.recoveryRequired());
 legacyFixture(c,0x20261026);assert(c.loadLegacyForMigration(offset));
 legacyFixture(c);writeSource("/book.txt","different source length");assert(!c.loadLegacyForMigration(offset));
 legacyFixture(c);old=readFile(c.path);seed(c);SD.resetFaults();SD.failRenames={2};assert(!c.save());SD.resetFaults();assert(readFile(c.path)==old&&c.loadLegacyForMigration(offset));
 seed(c);assert(c.save());assert(c.load()&&c.loadLegacyForMigration(offset));
 assert(bookmarkKey=="b0123456789abcd");

 // Prior v2 layouts: read-only recovery verifies digests and preserves the c-key.
 for(uint32_t layout : {0x20261026u,0x20261030u,0x20261031u,0x20261032u,0x20261033u,0x20261034u,0x20261035u,0x20261036u,0x20261037u,0x20261040u}){
  SD.remove(c.path+".bak");legacyFixture(c,layout,2);
  prefs.data["c0123456789abcd"]=7;old=readFile(c.path);
  assert(!c.load()&&c.loadLegacyForMigration(offset)&&offset==7);
  assert(bookmarkKey=="c0123456789abcd"&&readFile(c.path)==old);
 }
 replaceKeepingTimestamp("/book.txt","DIFFERENT SOURCE LENGTH");
 assert(!c.loadLegacyForMigration(offset)); // v2 recovery checks content, not just mtime.
 seed(c);bookEnglishHyphens=false;assert(c.save());
 bookEnglishHyphens=true;assert(c.load()&&!bookEnglishHyphens);
 seed(c);bookEnglishHyphens=true;assert(c.save());
 bookEnglishHyphens=false;assert(c.load()&&bookEnglishHyphens);

 // Markdown caches retain the original bytes (including BOM and syntax),
 // both supported filename extensions, and offsets into that original source.
 const std::string rawMarkdown="\xef\xbb\xbf# 标题\n\nfirst **bold** $x_1$.\n\n第二页正文\n";
 const uint32_t rawSecond=uint32_t(rawMarkdown.find("第二页"));
 assert(rawSecond!=uint32_t(std::string::npos));
 for(const std::string& source:{std::string("/notes.md"),std::string("/notes.markdown")}){
   writeSource(source,rawMarkdown);BookCache md(source,false);
   assert(md.observe("")==rawMarkdown.size());
   bookLength=rawMarkdown.size();memcpy(book,rawMarkdown.data(),bookLength);
   pageOffsets={0,rawSecond};bookmarkKey="c123456789abcde";
   prefs.data[bookmarkKey]=rawSecond+3;bookEnglishHyphens=false;
   assert(md.save());const std::string image=readFile(md.path);
   reader_cache::Header h;memcpy(&h,image.data(),sizeof(h));
   assert(h.layout==reader_cache::LAYOUT_MARKDOWN);
   pageOffsets.clear();bookLength=0;memset(book,0,rawMarkdown.size());
   assert(md.load()&&pageNumber==1&&bookLength==rawMarkdown.size());
   assert(!memcmp(book,rawMarkdown.data(),bookLength)&&book[bookLength]==0);
   assert(pageOffsets==std::vector<uint32_t>({0,rawSecond}));
   assert(bookIdentity==source&&bookmarkKey=="c123456789abcde"&&bookEnglishHyphens);
   const auto progress=prefs.data;
   for(uint32_t wrong:{reader_cache::LAYOUT,reader_cache::LAYOUT_NO_HYPHENS}){
     writeSource(md.path,withLayout(image,wrong));pageNumber=99;
     assert(!md.load()&&md.recoveryRequired()&&pageNumber==99);
     assert(prefs.data==progress); // Never migrate/rewrite a mismatched current layout.
   }
   writeSource(md.path,image);assert(md.load()&&pageNumber==1);
   assert(readFile(source)==rawMarkdown&&prefs.data==progress);
 }
 // Isolation is symmetric: a digest-valid Markdown layout is not a TXT hit.
 writeSource("/layout.txt","original source");BookCache plain("/layout.txt",false);
 seed(plain);bookEnglishHyphens=true;assert(plain.save());
 const std::string plainImage=readFile(plain.path);
 writeSource(plain.path,withLayout(plainImage,reader_cache::LAYOUT_MARKDOWN));
 assert(!plain.load());writeSource(plain.path,plainImage);assert(plain.load());

 // Throttled yielding must not charge a full RTOS tick for every 8 KiB.
 std::string megabyte(1024*1024,'q');writeSource("/throughput.txt",megabyte);
 fakeMillis=0;ioMillis=0;delayCalls=0;BookCache fast("/throughput.txt",false);
 assert(fast.observe("")==megabyte.size());
 assert(delayCalls>0&&delayCalls<=16); // Old source hashing needed 128 delays.
 bookLength=megabyte.size();memcpy(book,megabyte.data(),bookLength);
 pageOffsets={0,512*1024};bookmarkKey="c0123456789abcd";
 delayCalls=0;assert(fast.save());
 assert(delayCalls>0&&delayCalls<=32); // Source hash + write + verified reread.
 delayCalls=0;assert(fast.load());
 assert(delayCalls>0&&delayCalls<=24&&!memcmp(book,megabyte.data(),bookLength));

 // Slow I/O yields on elapsed time even below the byte threshold. Many small
 // transfers share that deadline; wrapping millis() must not disable it.
 auto exerciseTransfers=[&](size_t count,size_t repeats,uint32_t latency,uint32_t start){
   fakeMillis=start;ioMillis=latency;delayCalls=0;
   BookCache timed("/throughput.txt",false);File input=SD.open("/throughput.txt");
   std::vector<uint8_t> chunk(count);SHA256_CTX sha;SHA256_Init(&sha);
   for(size_t i=0;i<repeats;++i)assert(timed.transfer(input,chunk.data(),chunk.size(),sha,false));
   uint8_t actual[32],expected[32];SHA256_Final(actual,&sha);
   SHA256((const uint8_t*)megabyte.data(),count*repeats,expected);
   assert(!memcmp(actual,expected,32)&&delayCalls>0);ioMillis=0;
 };
 exerciseTransfers(3*8192,1,25,0);
 exerciseTransfers(32,1000,1,0);
 exerciseTransfers(2*8192,1,30,UINT32_MAX-20);

 verticalLayoutMigrationRegression();
 semanticRevisionMigrationRegression();
 managedCacheRegression();
 sourceStampRegression();
 styleCacheRegression();
 textReuseRegression();
 exclusiveSourceRegression();
 semanticAndPreparedRegression();
 largeDependencyGraphRegression();
 residentRegression();
 ioFailureRegression();
 deferredPublicationRegression();

 uint32_t invalid[]={0,6,6};assert(!reader_cache::validOffsets(invalid,3,12));assert(!reader_cache::validResource("../secret",9));
 std::filesystem::remove_all(root);
 puts("cache round-trip, source stamp prefilter/full fingerprints, short writes, readback, rollback/recovery, directory dependencies, read-only legacy migration, bounded cooperative yielding, Markdown raw-source/layout isolation: PASS");
}
