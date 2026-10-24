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
#include "sd_checkpoint.h"
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

using CheckpointIdentity=pagination_checkpoint::Identity;
void settle(PaginationCheckpoint& checkpoint,BookCache& cache){
 for(unsigned step=0;!checkpoint.ready()&&!checkpoint.failed();++step){
  assert(step<200);const auto reads=cache.work.cacheReadBytes,writes=cache.work.cacheWriteBytes;
  checkpoint.step();
  assert(cache.work.cacheReadBytes-reads<=8192&&cache.work.cacheWriteBytes-writes<=8192);
 }
}
std::vector<uint32_t> recover(BookCache& cache,const CheckpointIdentity& identity,uint32_t& cursor){
 PaginationCheckpoint checkpoint(cache,identity);settle(checkpoint,cache);
 std::vector<uint32_t> pages;
 checkpoint.restore([&](const uint32_t* p,size_t count,uint32_t end){pages.assign(p,p+count);cursor=end;return true;});
 return pages;
}
void publish(BookCache& cache,const CheckpointIdentity& identity,const std::vector<uint32_t>& pages,uint32_t cursor){
 PaginationCheckpoint checkpoint(cache,identity);settle(checkpoint,cache);
 assert(checkpoint.begin(pages,cursor));settle(checkpoint,cache);
 assert(checkpoint.ready()&&checkpoint.savedPages()==pages.size());
}
std::string authenticated(std::string image,std::function<void(pagination_checkpoint::Header&)> change){
 pagination_checkpoint::Header h;memcpy(&h,image.data(),sizeof(h));change(h);
 SHA256_CTX sha;SHA256_Init(&sha);SHA256_Update(&sha,&h,offsetof(pagination_checkpoint::Header,digest));
 SHA256_Update(&sha,image.data()+sizeof(h),image.size()-sizeof(h));SHA256_Final(h.digest,&sha);
 memcpy(&image[0],&h,sizeof(h));return image;
}
int main(){
 root=(std::filesystem::temp_directory_path()/std::filesystem::path("paperdiy-checkpoint-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();
 std::filesystem::create_directory(root);SD.mkdir("/.paperdiy-cache");
 const std::string source(900000,' ');writeSource("/synthetic.epub",source);
 storage_validation::beginExclusive();BookCache cache("/synthetic.epub",false);
 assert(cache.observe("")==source.size());bookLength=source.size();memcpy(book,source.data(),source.size());
 bookmarkKey="c0123456789abcd";bookIdentity="/synthetic.epub";bookDocument.clear();
 CheckpointIdentity identity;assert(checkpointIdentity(cache,reader_cache::LAYOUT,identity));
 const auto sourceImage=readFile("/synthetic.epub");
 // Keep unrelated complete generations and exact progress as sentinels.
 writeSource(cache.path,"complete-generation");writeSource(cache.path+".bak","previous-generation");
 const auto progress=prefs.data;retainedOffset=777;retainedPage=3;pageOffsets={0,222};pageNumber=1;
 auto intact=[&](){assert(readFile(cache.path)=="complete-generation"&&readFile(cache.path+".bak")=="previous-generation"&&readFile("/synthetic.epub")==sourceImage&&prefs.data==progress&&retainedOffset==777&&retainedPage==3&&pageOffsets==std::vector<uint32_t>({0,222})&&pageNumber==1);};
 std::vector<uint32_t> first,second;
 for(unsigned i=0;i<64;++i)first.push_back(i*10);
 for(unsigned i=0;i<5000;++i)second.push_back(i*10);
 uint32_t cursor=0;assert(recover(cache,identity,cursor).empty());
 publish(cache,identity,first,640);assert(recover(cache,identity,cursor)==first&&cursor==640);intact();
 const auto older=readFile(cache.path+".idx0");
 // Stop at each write/readback state. Restart admits either the prior durable
 // prefix or a fully sealed new prefix; an unsealed/torn slot never wins.
 for(unsigned stop=0;stop<14;++stop){
  writeSource(cache.path+".idx0",older);if(SD.exists(cache.path+".idx1"))SD.remove(cache.path+".idx1");
  {PaginationCheckpoint checkpoint(cache,identity);settle(checkpoint,cache);assert(checkpoint.begin(second,50000));
   for(unsigned i=0;i<stop&&!checkpoint.ready()&&!checkpoint.failed();++i)checkpoint.step();}
  const auto restored=recover(cache,identity,cursor);
  assert((restored==first&&cursor==640)||(restored==second&&cursor==50000));intact();
 }
 publish(cache,identity,second,50000);assert(recover(cache,identity,cursor)==second&&cursor==50000);
 // Both slots exist. The newer valid generation wins even when its filename
 // order is reversed; allocation is capped before payload admission.
 const auto slot0=readFile(cache.path+".idx0"),slot1=readFile(cache.path+".idx1");
 writeSource(cache.path+".idx0",slot1);writeSource(cache.path+".idx1",slot0);
 assert(recover(cache,identity,cursor)==second);
 writeSource(cache.path+".idx0",older);writeSource(cache.path+".idx1",slot0.size()>older.size()?slot0:slot1);
 const auto newer=readFile(cache.path+".idx1");assert(newer.size()==sizeof(pagination_checkpoint::Header)+second.size()*4);
 auto rejectsNew=[&](const std::string& damaged){writeSource(cache.path+".idx1",damaged);assert(recover(cache,identity,cursor)==first&&cursor==640);intact();};
 for(size_t length:{size_t(0),sizeof(pagination_checkpoint::Header)-1,newer.size()-1})rejectsNew(newer.substr(0,length));
 for(size_t at:{size_t(0),offsetof(pagination_checkpoint::Header,digest),sizeof(pagination_checkpoint::Header),newer.size()-1}){auto damaged=newer;damaged[at]^=1;rejectsNew(damaged);}
 for(auto change:{std::function<void(pagination_checkpoint::Header&)>([](auto& h){h.cursor=h.identity.textBytes;}),
   std::function<void(pagination_checkpoint::Header&)>([](auto& h){h.pages=UINT32_MAX;}),
   std::function<void(pagination_checkpoint::Header&)>([](auto& h){h.reserved=1;}),
   std::function<void(pagination_checkpoint::Header&)>([](auto& h){++h.identity.layout;}),
   std::function<void(pagination_checkpoint::Header&)>([](auto& h){++h.identity.textRevision;}),
   std::function<void(pagination_checkpoint::Header&)>([](auto& h){h.identity.content[0]^=1;}),
   std::function<void(pagination_checkpoint::Header&)>([](auto& h){h.identity.dependencies[0]^=1;}),
   std::function<void(pagination_checkpoint::Header&)>([](auto& h){h.identity.source[0]^=1;})})rejectsNew(authenticated(newer,change));
 auto badOffsets=newer;uint32_t duplicate=0;memcpy(&badOffsets[sizeof(pagination_checkpoint::Header)+4],&duplicate,4);
 rejectsNew(authenticated(badOffsets,[](auto&){}));
 // An interrupted write cannot replace the other slot or promote an incomplete
 // prefix to a complete cache; failed optional work leaves progress untouched.
 for(size_t budget:{size_t(0),sizeof(pagination_checkpoint::Header)-1,sizeof(pagination_checkpoint::Header)+99}){
  writeSource(cache.path+".idx0",older);writeSource(cache.path+".idx1","");storage_validation::beginExclusive();
  {PaginationCheckpoint checkpoint(cache,identity);settle(checkpoint,cache);assert(checkpoint.begin(second,50000));writeBudget=budget;settle(checkpoint,cache);assert(checkpoint.failed());}
  SD.resetFaults();storage_validation::beginExclusive();assert(recover(cache,identity,cursor)==first);intact();
 }
 writeSource(cache.path+".idx0",older);writeSource(cache.path+".idx1","");
 {PaginationCheckpoint checkpoint(cache,identity);settle(checkpoint,cache);assert(checkpoint.begin(second,50000));corruptOnFlush=true;settle(checkpoint,cache);assert(checkpoint.failed());}
 SD.resetFaults();assert(recover(cache,identity,cursor)==first);intact();
 // A stale lease closes, but never deletes or touches a replacement card.
 {PaginationCheckpoint checkpoint(cache,identity);settle(checkpoint,cache);assert(checkpoint.begin(second,50000));checkpoint.step();
  const auto wrote=cache.work.cacheWriteBytes;storage_validation::invalidate();checkpoint.step();assert(checkpoint.failed());assert(cache.work.cacheWriteBytes==wrote&&readFile(cache.path+".idx0")==older);}
 storage_validation::beginExclusive();assert(recover(cache,identity,cursor)==first);intact();
 // Actual source, extracted bytes and semantic bytes independently invalidate
 // recovery, even if file size and timestamp were kept unchanged.
 auto changed=identity;book[13]='x';assert(checkpointIdentity(cache,reader_cache::LAYOUT,changed));assert(!pagination_checkpoint::same(changed,identity));assert(recover(cache,changed,cursor).empty());book[13]=' ';
 epub::structure::Collector structure;structure.begin();auto block=structure.open(epub::structure::Kind::Paragraph,0,epub::structure::NONE,epub::structure::NONE,0,0,0);structure.text(0,bookLength,block,epub::structure::NONE,0,0,0);structure.close(block,bookLength);structure.finish(book,bookLength);
 assert(bookDocument.assign(structure,book,bookLength));assert(checkpointIdentity(cache,reader_cache::LAYOUT,changed));assert(!pagination_checkpoint::same(changed,identity));assert(recover(cache,changed,cursor).empty());bookDocument.clear();
 replaceKeepingTimestamp("/synthetic.epub",std::string(source.size(),'z'));storage_validation::invalidate();storage_validation::beginExclusive();
 assert(!checkpointIdentity(cache,reader_cache::LAYOUT,changed));
 // Maximum prefix uses bounded space, and rejects a complete cursor. Even
 // at this bound every individual read/write stays within one 8 KiB slice.
 writeSource("/synthetic.epub",source);cache.dependencies.clear();cache.observe("");assert(checkpointIdentity(cache,reader_cache::LAYOUT,identity));
 std::vector<uint32_t> maximum(pagination_checkpoint::MAX_PAGES);for(size_t i=0;i<maximum.size();++i)maximum[i]=i*2;
 {PaginationCheckpoint checkpoint(cache,identity);settle(checkpoint,cache);assert(!checkpoint.begin(maximum,uint32_t(bookLength)));assert(checkpoint.begin(maximum,uint32_t(maximum.size()*2)));settle(checkpoint,cache);assert(checkpoint.ready());}
 assert(recover(cache,identity,cursor)==maximum);
 assert(std::filesystem::file_size(root+cache.path+".idx0")<=pagination_checkpoint::MAX_FILE_BYTES&&std::filesystem::file_size(root+cache.path+".idx1")<=pagination_checkpoint::MAX_FILE_BYTES);
 std::filesystem::remove_all(root);puts("partial checkpoint: bounded slots/I-O, restart/cancellation, source/content/semantics/layout binding, torn writes, complete-cache and anchor preservation: PASS");
}
