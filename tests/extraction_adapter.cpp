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
#include "bookmark_migration.h"
using String=std::string;
static std::string root;
static size_t writeBudget=SIZE_MAX,spoolWritten=0;
static bool corruptOnFlush=false;
static uint32_t fakeMillis=0,ioMillis=0,clockPerCall=0;
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
 File(std::string p,const char* mode):path(p){f=std::make_shared<std::fstream>(p,std::ios::binary|(*mode=='w'?(std::ios::in|std::ios::out|std::ios::trunc):std::strchr(mode,'+')?(std::ios::in|std::ios::out):std::ios::in));}
 explicit operator bool()const{return f&&bool(*f);}
 size_t read(uint8_t* p,size_t n){
   if(path==watchedSource&&sourceReadBudget!=SIZE_MAX){n=std::min(n,sourceReadBudget);sourceReadBudget-=n;}
   fakeMillis+=ioMillis;f->read((char*)p,n);size_t count=f->gcount();
   if(path==watchedSource){sourceReadBytes+=count;runOnce(onSourceRead);}
   return count;
 }
 size_t write(const uint8_t* p,size_t n){
   fakeMillis+=ioMillis;n=std::min(n,writeBudget);f->write((char*)p,n);
   if(path.size()>=8&&path.substr(path.size()-8)==".exttext")spoolWritten+=n;
   if(writeBudget!=SIZE_MAX)writeBudget-=n;
   return *f?n:0;
 }
 bool isDirectory()const{return std::filesystem::is_directory(path);}
 size_t size(){return std::filesystem::file_size(path);}
 bool seek(size_t p){f->clear();f->seekg(p);f->seekp(p);return bool(*f);}
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
 File open(const String& p,const char* mode="r"){if(failWrite&&*mode=='w')return {};return File(root+p,mode);}
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
void mbedtls_sha256_clone(mbedtls_sha256_context* a,const mbedtls_sha256_context* b){*a=*b;}
void mbedtls_sha256_starts_ret(mbedtls_sha256_context* p,int){SHA256_Init(p);}
void mbedtls_sha256_update_ret(mbedtls_sha256_context* p,const uint8_t* b,size_t n){SHA256_Update(p,b,n);}
void mbedtls_sha256_finish_ret(mbedtls_sha256_context* p,uint8_t* out){SHA256_Final(out,p);}
void mbedtls_sha256_ret(const uint8_t* p,size_t n,uint8_t* out,int){SHA256(p,n,out);}
uint32_t millis(){fakeMillis+=clockPerCall;return fakeMillis;}
// Model a 10 ms FreeRTOS tick even when firmware requests delay(1).
void delay(int ms){++delayCalls;fakeMillis+=ms?10:0;}
static size_t failPsSize=SIZE_MAX;
void* ps_malloc(size_t n){return n==failPsSize?nullptr:malloc(n);}
struct {template<class... T>void printf(const char*,T...) {} void println(const char*){}} Serial;
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
#include "sd_extraction.h"
namespace reader_typography {const Style style;}
#undef stat
void writeSource(const std::string& p,const std::string& data){std::ofstream(root+p,std::ios::binary)<<data;}
std::string readFile(const std::string& p){std::ifstream f(root+p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
void replaceKeepingTimestamp(const std::string& p,const std::string& data){
 auto before=std::filesystem::last_write_time(root+p);
 writeSource(p,data);std::filesystem::last_write_time(root+p,before);
}
// Execute the production cold-open lifecycle with only hardware/NVS/renderer
// bindings replaced. No partial state may reach the index or save callbacks.
std::vector<String> sdBooks={"/synthetic.epub"};size_t selectedBook=0;String sdError;bool sdOK=true;
struct {unsigned getFreePsram()const{return 8*1024*1024;}} ESP;
constexpr int MALLOC_CAP_SPIRAM=1;
size_t heap_caps_get_free_size(int){return 8*1024*1024;}
size_t heap_caps_get_largest_free_block(int){return 8*1024*1024;}
unsigned indexed=0,savedProgress=0,queued=0;
bool sdReadFailure(){sdOK=false;storage_validation::invalidate();return false;}
uint32_t crcUpdate(uint32_t c,const uint8_t* p,size_t n){while(n--){c^=*p++;for(unsigned i=0;i<8;++i)c=(c>>1)^((c&1)?0xedb88320:0);}return c;}
bool indexBook(bool,bool,const bookmarks::Anchor*,bool,bool){assert(bookLength>0);++indexed;pageOffsets={0};return true;}
bool saveReadingProgress(){assert(bookLength>0&&!pageOffsets.empty());++savedProgress;return true;}
void queueBackground(BookCache&,bool){++queued;}
void rememberResident(){}
#include "sd_open_job.h"
static SDOpenResult settleJob(sd_open::Job& job){
 SDOpenResult result=SDOpenResult::Pending;
 for(unsigned n=0;result==SDOpenResult::Pending;++n){assert(n<4000);result=job.poll();}
 return result;
}
struct Run {
 BookCache cache;
 epub::structure::Collector collector;
 epub::Importer importer;
 std::vector<char> text;
 std::unique_ptr<ExtractionCheckpoint> checkpoint;
 explicit Run(bool restore=true):cache("/synthetic.epub",true),text(MAX_BOOK+1){
  importer.structure=&collector;importer.output=text.data();importer.capacity=MAX_BOOK;
  importer.alloc=[](size_t n)->void*{return malloc(n);};
  importer.resourceSize=[this](const std::string& n){return cache.observe(n);};
  importer.resourceRead=[](const std::string& n,void* p,size_t count){std::ifstream f(root+"/synthetic.epub/"+n,std::ios::binary);return bool(f.read((char*)p,count));};
  assert(importer.begin());
  if(restore){checkpoint.reset(new ExtractionCheckpoint(cache,importer,collector));settle();}
 }
 void settle(){
  for(unsigned n=0;checkpoint->busy();++n){
   assert(n<2000);auto r=cache.work.cacheReadBytes,w=cache.work.cacheWriteBytes;
   checkpoint->step();
   // Each persistence poll performs at most one bounded data/header transfer.
   assert(cache.work.cacheReadBytes-r<=8192&&cache.work.cacheWriteBytes-w<=8192);
  }
 }
 void step(){
  const auto prefix=std::string(text.data(),importer.used);
  assert(importer.stepChapter()!=epub::Importer::Progress::Failed);
  assert(importer.used>=prefix.size()&&!memcmp(prefix.data(),text.data(),prefix.size()));
 }
 void commit(){assert(checkpoint->beginCommit());settle();assert(!checkpoint->failed());}
 std::vector<uint8_t> document(){epub_document::Document d;assert(d.assign(collector,text.data(),importer.used));std::vector<uint8_t> p(d.encodedSize());assert(d.encodeTo(p.data(),p.size()));return p;}
};
static void fixture(){
 std::filesystem::create_directories(root+"/synthetic.epub/META-INF");
 writeSource("/synthetic.epub/META-INF/container.xml","<container><rootfiles><rootfile full-path='book.opf'/></rootfiles></container>");
 writeSource("/synthetic.epub/book.opf","<package><metadata><title>Fixture</title><language>en-GB</language></metadata><manifest><item id='a' href='a.xhtml'/><item id='b' href='b.xhtml'/><item id='c' href='c.xhtml'/></manifest><spine><itemref idref='a'/><itemref idref='b'/><itemref idref='c'/></spine></package>");
 writeSource("/synthetic.epub/style.css",".v{writing-mode:vertical-rl}");
 const std::string start="<html><head><link rel='stylesheet' href='style.css'/></head><body>";
 writeSource("/synthetic.epub/a.xhtml",start+"<p class='v'>日本語 <ruby>漢<rt>かん</rt></ruby></p><p>"+std::string(19000,'a')+"</p></body></html>");
 writeSource("/synthetic.epub/b.xhtml",start+"<p lang='fr'>Second <math><mfrac><mi>x</mi><mi>y</mi></mfrac></math> ending.</p><table><tr><td>A</td><td>B</td></tr></table><p>"+std::string(20000,'b')+"</p></body></html>");
 writeSource("/synthetic.epub/c.xhtml",start+"<p>Final body.</p></body></html>");
}
int main(){
 root=(std::filesystem::temp_directory_path()/std::filesystem::path("paperdiy-extraction-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();
 std::filesystem::create_directory(root);SD.mkdir("/.paperdiy-cache");fixture();storage_validation::beginExclusive();
 Run uninterrupted(false);while(uninterrupted.importer.progress==epub::Importer::Progress::Pending)uninterrupted.step();
 const std::string expected(uninterrupted.text.data(),uninterrupted.importer.used);const auto semantics=uninterrupted.document();
 std::string path;
 {Run first;assert(!first.checkpoint->recovered());first.step();first.commit();path=first.cache.path;}
 const auto original=readFile(path+".ext0"),spool=readFile(path+".exttext");
 // Existing complete cache and NVS are never destinations of extraction work.
 writeSource(path,"previous-complete-cache");writeSource(path+".bak","older-complete-cache");prefs.data["c0123456789abcd"]=777;
 auto intact=[&](){assert(readFile(path)=="previous-complete-cache"&&readFile(path+".bak")=="older-complete-cache"&&prefs.data.size()==1&&prefs.data.begin()->second==777&&bookLength==0);};
 for(unsigned stop=0;stop<20;++stop){
  writeSource(path+".ext0",original);writeSource(path+".exttext",spool);if(SD.exists(path+".ext1"))SD.remove(path+".ext1");
  {Run second;assert(second.checkpoint->recovered()&&second.importer.chapterCursor==1);second.step();assert(second.checkpoint->beginCommit());
   for(unsigned n=0;n<stop&&second.checkpoint->busy();++n)second.checkpoint->step();}
  // Simulated restart revokes every process-local source fingerprint proof.
  storage_validation::invalidate();storage_validation::beginExclusive();
  Run recovered;assert(recovered.checkpoint->recovered());assert(recovered.importer.chapterCursor==1||recovered.importer.chapterCursor==2);
  while(recovered.importer.progress==epub::Importer::Progress::Pending)recovered.step();
  assert(std::string(recovered.text.data(),recovered.importer.used)==expected&&recovered.document()==semantics);recovered.commit();intact();
 }
 {Run complete;assert(complete.checkpoint->recovered()&&complete.checkpoint->complete());assert(complete.importer.progress==epub::Importer::Progress::Complete);assert(complete.document()==semantics);}
 // Torn newer metadata falls back to the older committed prefix. Orphan suffix
 // bytes never invalidate that prefix, and the next append overwrites the tail.
 writeSource(path+".ext0",original);writeSource(path+".ext1","bad slot");writeSource(path+".exttext",spool+std::string(30000,'!'));
 {Run r;assert(r.checkpoint->recovered()&&r.importer.chapterCursor==1);r.step();r.commit();assert(readFile(path+".exttext").substr(0,r.importer.used)==std::string(r.text.data(),r.importer.used));}
 // Same-length/mtime edits to a read XHTML, CSS and OPF all reject resume under
 // a fresh lease; neither path/stamp nor a matching plan is a content proof.
 for(const char* dep:{"/synthetic.epub/a.xhtml","/synthetic.epub/style.css","/synthetic.epub/book.opf"}){
  writeSource(path+".ext0",original);writeSource(path+".exttext",spool);if(SD.exists(path+".ext1"))SD.remove(path+".ext1");
  const auto before=readFile(dep);auto changed=before;const auto at=changed.find('>');changed[at==std::string::npos?0:at+1]^=1;
  // CSS has no XML boundary, and the OPF title is a safe textual mutation.
  if(std::string(dep).find("book.opf")!=std::string::npos){changed=before;changed[changed.find("Fixture")]='f';}
  replaceKeepingTimestamp(dep,changed);storage_validation::invalidate();storage_validation::beginExclusive();
  {Run r;assert(!r.checkpoint->recovered());}
  replaceKeepingTimestamp(dep,before);storage_validation::invalidate();storage_validation::beginExclusive();intact();
 }
 // Short/torn append and lost lease preserve the old slot/prefix.
 for(size_t budget:{size_t(0),size_t(17),size_t(8192),size_t(20000)}){
  writeSource(path+".ext0",original);writeSource(path+".exttext",spool);if(SD.exists(path+".ext1"))SD.remove(path+".ext1");
  {Run r;r.step();writeBudget=budget;assert(r.checkpoint->beginCommit());r.settle();SD.resetFaults();}
  storage_validation::invalidate();storage_validation::beginExclusive();Run r;assert(r.checkpoint->recovered()&&r.importer.chapterCursor==1);intact();
 }
 {Run r;r.step();assert(r.checkpoint->beginCommit());storage_validation::invalidate();r.checkpoint->step();assert(r.checkpoint->failed());}
 storage_validation::beginExclusive();{Run r;assert(r.checkpoint->recovered());}
 // Text checksum and explicit semantic revision prevent stale/malformed
 // products becoming a complete book, even with valid source bytes.
 writeSource(path+".ext0",original);if(SD.exists(path+".ext1"))SD.remove(path+".ext1");auto bad=spool;bad[0]^=1;writeSource(path+".exttext",bad);
 {Run r;assert(!r.checkpoint->recovered());}intact();
 // Production Job cancellation during a spool commit is terminal only after
 // verified commit/handle closure; it performs no pagination or NVS callback.
 auto removeJournal=[&](){for(const char* suffix:{".ext0",".ext1",".exttext"})if(SD.exists(path+suffix))SD.remove(path+suffix);};
 removeJournal();spoolWritten=0;
 {Run linear;while(linear.importer.progress==epub::Importer::Progress::Pending){linear.step();linear.commit();}
  assert(spoolWritten==expected.size());}
 removeJournal();bookLength=0;pageOffsets.clear();indexed=savedProgress=queued=0;
 {BookCache c("/synthetic.epub",true);sd_open::Job job(std::move(c),0,true,false,{},millis(),nullptr);
  for(unsigned n=0;!job.checkpoint||!job.checkpoint->committedBytes();++n){assert(n<1000);assert(job.poll()==SDOpenResult::Pending);assert(bookLength==0&&indexed==0&&savedProgress==0);}
  assert(job.importer.chapterCursor==1);assert(job.poll()==SDOpenResult::Pending); // chapter 2; not yet due
  job.cancelled=true;assert(settleJob(job)==SDOpenResult::Cancelled);
  assert(job.checkpoint->committedBytes()==job.importer.used&&bookLength==0&&indexed==0&&savedProgress==0);
 }
 {BookCache c("/synthetic.epub",true);sd_open::Job job(std::move(c),0,true,false,{},millis(),nullptr);
  assert(settleJob(job)==SDOpenResult::Ready);assert(job.importer.chapterCursor==3&&indexed==1&&savedProgress==1&&queued==1);
  assert(std::string(book,bookLength)==expected);std::vector<uint8_t> actual(bookDocument.encodedSize());assert(bookDocument.encodeTo(actual.data(),actual.size())&&actual==semantics);
 }
 // Both invalid slots may have overwritten the package prefix and Collector;
 // the actual job must rebuild setup before it appends new chapter bytes.
 writeSource(path+".ext0",original);if(SD.exists(path+".ext1"))SD.remove(path+".ext1");writeSource(path+".exttext",bad);
 bookLength=0;pageOffsets.clear();
 {BookCache c("/synthetic.epub",true);sd_open::Job job(std::move(c),0,true,false,{},millis(),nullptr);
  assert(settleJob(job)==SDOpenResult::Ready);assert(std::string(book,bookLength)==expected);
  std::vector<uint8_t> actual(bookDocument.encodedSize());assert(bookDocument.encodeTo(actual.data(),actual.size())&&actual==semantics);
 }
 // Optional journal permissions/allocation must not turn readable EPUB into
 // a new format requirement. Confirmed short writes are storage failures and
 // never publish book identity/progress from partial text.
 removeJournal();bookLength=0;pageOffsets.clear();indexed=savedProgress=0;SD.failWrite=true;
 {BookCache c("/synthetic.epub",true);sd_open::Job job(std::move(c),0,true,false,{},millis(),nullptr);
  assert(settleJob(job)==SDOpenResult::Ready&&job.checkpointDisabled&&indexed==1&&savedProgress==1);
  assert(std::string(book,bookLength)==expected);
 }SD.resetFaults();
 writeSource(path+".ext0",original);writeSource(path+".exttext",spool);
 failPsSize=original.size()-extraction_checkpoint::HEADER_BYTES;bookLength=0;pageOffsets.clear();
 {BookCache c("/synthetic.epub",true);sd_open::Job job(std::move(c),0,true,false,{},millis(),nullptr);
  assert(settleJob(job)==SDOpenResult::Ready&&job.checkpointDisabled);assert(readFile(path+".ext0")==original);
  assert(std::string(book,bookLength)==expected);
 }failPsSize=SIZE_MAX;
 removeJournal();bookLength=0;pageOffsets.clear();indexed=savedProgress=0;writeBudget=17;
 {BookCache c("/synthetic.epub",true);sd_open::Job job(std::move(c),0,true,false,{},millis(),nullptr);
  assert(settleJob(job)==SDOpenResult::Failed&&bookLength==0&&indexed==0&&savedProgress==0&&storage_validation::lease()==0);
 }SD.resetFaults();storage_validation::beginExclusive();sdOK=true;
 // The opening lease is independent of the optional journal. Even if all
 // source bytes still match, a revoked/replaced card cannot reach Ready.
 removeJournal();bookLength=0;pageOffsets.clear();indexed=savedProgress=0;SD.failWrite=true;
 {BookCache c("/synthetic.epub",true);sd_open::Job job(std::move(c),0,true,false,{},millis(),nullptr);
  for(unsigned n=0;!job.checkpointDisabled;++n){assert(n<1000);assert(job.poll()==SDOpenResult::Pending);}
  assert(job.importer.chapterCursor==1);storage_validation::sourceChanged();
  assert(job.poll()==SDOpenResult::Failed&&bookLength==0&&indexed==0&&savedProgress==0);
 }SD.resetFaults();storage_validation::beginExclusive();
 // A real per-document parser observer sees input while a large chapter is
 // still being parsed; cancellation still commits only its closed boundary.
 removeJournal();std::string paragraphs;for(unsigned n=0;n<1000;++n)paragraphs+="<p>Text</p>";
 writeSource("/synthetic.epub/a.xhtml","<html><body>"+paragraphs+"</body></html>");
 storage_validation::invalidate();storage_validation::beginExclusive();bookLength=0;pageOffsets.clear();indexed=savedProgress=0;
 static unsigned probes=0;static sd_open::Job* observedJob=nullptr;static bool cancelledInParser=false;
 probes=0;clockPerCall=1;
 {BookCache c("/synthetic.epub",true);sd_open::Job job(std::move(c),0,true,false,{},millis(),[]()->bool{if(++probes<10)return false;if(observedJob&&observedJob->importer.used==std::strlen("Fixture\n\n"))cancelledInParser=true;return true;});
  observedJob=&job;assert(settleJob(job)==SDOpenResult::Cancelled);observedJob=nullptr;assert(probes>=10&&cancelledInParser&&job.importer.chapterCursor==1);
  assert(job.checkpoint&&job.checkpoint->committedBytes()>0&&bookLength==0&&indexed==0&&savedProgress==0);
 }
 clockPerCall=0;
 std::filesystem::remove_all(root);std::puts("Extraction restart, append durability, source validation and PDS parity passed");
}
