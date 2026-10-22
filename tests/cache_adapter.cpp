#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <openssl/sha.h>
#include <chrono>
using String=std::string;
static std::string root;
static size_t writeBudget=SIZE_MAX;
static bool corruptOnFlush=false;
static uint32_t fakeMillis=0,ioMillis=0;
static unsigned delayCalls=0;
struct File {
 std::shared_ptr<std::fstream> f;std::string path;
 File()=default;
 File(std::string p,bool write):path(p){f=std::make_shared<std::fstream>(p,std::ios::binary|(write?(std::ios::in|std::ios::out|std::ios::trunc):std::ios::in));}
 explicit operator bool()const{return f&&bool(*f);}
 size_t read(uint8_t* p,size_t n){fakeMillis+=ioMillis;f->read((char*)p,n);return f->gcount();}
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
int cacheStat(const char* p,struct stat* s){std::string path=p;assert(path.rfind("/sd",0)==0);return ::stat((root+path.substr(3)).c_str(),s);}
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
void prepareReaderFont(){}
#define stat(p,s) cacheStat(p,s)
#include "sd_cache.h"
#undef stat
void writeSource(const std::string& p,const std::string& data){std::ofstream(root+p,std::ios::binary)<<data;}
std::string readFile(const std::string& p){std::ifstream f(root+p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
void replaceKeepingTimestamp(const std::string& p,const std::string& data){
 auto before=std::filesystem::last_write_time(root+p);
 writeSource(p,data);std::filesystem::last_write_time(root+p,before);
}
void seed(BookCache& c){c.dependencies.clear();c.observe("");bookLength=12;memcpy(book,"hello\nworld\n",12);pageOffsets={0,6};bookmarkKey="c0123456789abcd";}
void corrupt(const std::string& path,size_t pos){
 std::fstream f(root+path,std::ios::binary|std::ios::in|std::ios::out);f.seekg(pos);char v;f.get(v);v^=1;f.seekp(pos);f.put(v);
}
void legacyFixture(BookCache& c,uint32_t layout=0x20261025,uint32_t version=1){
 seed(c);bookmarkKey=version==1?"b0123456789abcd":"c0123456789abcd";
 reader_cache::Header h{};h.magic=reader_cache::MAGIC;h.version=version;h.layout=layout;
 h.directory=c.directory;h.textBytes=bookLength;h.pages=pageOffsets.size();h.deps=c.dependencies.size();
 std::vector<uint8_t> data;
 auto append=[&](const void* p,size_t n){auto b=(const uint8_t*)p;data.insert(data.end(),b,b+n);};
 for(const auto& dep:c.dependencies){uint32_t fields[3]={(uint32_t)dep.name.length(),dep.size,dep.mtime};append(fields,sizeof(fields));if(version==2)append(dep.digest,sizeof(dep.digest));append(dep.name.c_str(),dep.name.length());}
 h.metaBytes=data.size();append(pageOffsets.data(),pageOffsets.size()*4);append(book,bookLength);
 memcpy(h.source,c.sourceHash,32);memcpy(h.bookmark,bookmarkKey.c_str(),16);
 SHA256_CTX sha;SHA256_Init(&sha);SHA256_Update(&sha,&h,offsetof(reader_cache::Header,digest));SHA256_Update(&sha,data.data(),data.size());SHA256_Final(h.digest,&sha);
 std::ofstream f(root+c.path,std::ios::binary);f.write((char*)&h,sizeof(h));f.write((char*)data.data(),data.size());
}
int main(){
 root=(std::filesystem::temp_directory_path()/("paperdiy-cache-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();assert(std::filesystem::create_directory(root));
 writeSource("/book.txt","original source");BookCache c("/book.txt",false);
 uint32_t noLegacy=0;assert(!c.load()&&!c.loadLegacyForMigration(noLegacy)&&!c.recoveryRequired());
 seed(c);assert(c.save());
 prefs.data[bookmarkKey]=6;pageOffsets.clear();memset(book,0,12);assert(c.load()&&!c.recoveryRequired());assert(pageNumber==1&&pageOffsets.size()==2&&!memcmp(book,"hello\nworld\n",12));
 prefs.failRead=true;pageNumber=99;assert(!c.load()&&c.recoveryRequired()&&pageNumber==99);
 prefs.failRead=false;assert(c.load()&&pageNumber==1);
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
 for(uint32_t layout : {0x20261026u,0x20261030u,0x20261031u,0x20261032u,0x20261033u,0x20261034u,0x20261035u}){
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

 uint32_t invalid[]={0,6,6};assert(!reader_cache::validOffsets(invalid,3,12));assert(!reader_cache::validResource("../secret",9));
 std::filesystem::remove_all(root);
 puts("cache round-trip, source fingerprints, short writes, readback, rollback/recovery, directory dependencies, read-only legacy migration, bounded cooperative yielding: PASS");
}
