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
#include <string>
#include <vector>
#include <sys/stat.h>
#include <openssl/sha.h>
#include <chrono>
using String=std::string;
static std::string root;
struct File {
 std::shared_ptr<std::fstream> f;std::string path;
 File()=default;
 File(std::string p,bool write):path(p){f=std::make_shared<std::fstream>(p,std::ios::binary|(write?(std::ios::in|std::ios::out|std::ios::trunc):std::ios::in));}
 explicit operator bool()const{return f&&bool(*f);}
 size_t read(uint8_t* p,size_t n){f->read((char*)p,n);return f->gcount();}
 size_t write(const uint8_t* p,size_t n){f->write((char*)p,n);return *f?n:0;}
 size_t size(){return std::filesystem::file_size(path);}
 bool seek(size_t p){f->seekp(p);return bool(*f);}
 void flush(){f->flush();}
 void close(){if(f)f->close();}
};
struct Disk {
 bool failRename=false,failWrite=false;
 bool exists(const String& p){return std::filesystem::exists(root+p);}
 bool mkdir(const String& p){return std::filesystem::create_directory(root+p);}
 bool remove(const String& p){return std::filesystem::remove(root+p);}
 bool rename(const String& a,const String& b){if(failRename)return false;std::filesystem::rename(root+a,root+b);return true;}
 File open(const String& p,const char* mode="r"){if(failWrite&&*mode=='w')return {};return File(root+p,*mode=='w');}
} SD;
int cacheStat(const char* p,struct stat* s){std::string path=p;assert(path.rfind("/sd",0)==0);return ::stat((root+path.substr(3)).c_str(),s);}
using mbedtls_sha256_context=SHA256_CTX;
void mbedtls_sha256_init(mbedtls_sha256_context*){}
void mbedtls_sha256_free(mbedtls_sha256_context*){}
void mbedtls_sha256_starts_ret(mbedtls_sha256_context* p,int){SHA256_Init(p);}
void mbedtls_sha256_update_ret(mbedtls_sha256_context* p,const uint8_t* b,size_t n){SHA256_Update(p,b,n);}
void mbedtls_sha256_finish_ret(mbedtls_sha256_context* p,uint8_t* out){SHA256_Final(out,p);}
void mbedtls_sha256_ret(const uint8_t* p,size_t n,uint8_t* out,int){SHA256(p,n,out);}
void delay(int){}void* ps_malloc(size_t n){return malloc(n);}
struct {template<class... T>void printf(const char*,T...) {}} Serial;
constexpr size_t MAX_BOOK=4*1024*1024;char storage[MAX_BOOK+1];char* book=storage;size_t bookLength=0;
std::vector<uint32_t> pageOffsets;uint32_t pageNumber;String bookmarkKey,bookIdentity;
struct {std::map<std::string,uint32_t> data;uint32_t getUInt(const char* k,uint32_t d){auto i=data.find(k);return i==data.end()?d:i->second;}} prefs;
void prepareReaderFont(){}
#define stat(p,s) cacheStat(p,s)
#include "sd_cache.h"
#undef stat
void writeSource(const std::string& p,const std::string& data){std::ofstream(root+p,std::ios::binary)<<data;}
void seed(BookCache& c){c.dependencies.clear();c.observe("");bookLength=12;memcpy(book,"hello\nworld\n",12);pageOffsets={0,6};bookmarkKey="b0123456789abcd";}
int main(){
 root=(std::filesystem::temp_directory_path()/("paperdiy-cache-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();assert(std::filesystem::create_directory(root));
 writeSource("/book.txt","original source");BookCache c("/book.txt",false);seed(c);assert(c.save());
 prefs.data[bookmarkKey]=6;pageOffsets.clear();memset(book,0,12);assert(c.load());assert(pageNumber==1&&pageOffsets.size()==2&&!memcmp(book,"hello\nworld\n",12));
 auto corrupt=[&](size_t pos){std::fstream f(root+c.path,std::ios::binary|std::ios::in|std::ios::out);f.seekg(pos);char v;f.get(v);v^=1;f.seekp(pos);f.put(v);};
 corrupt(std::filesystem::file_size(root+c.path)-1);assert(!c.load());seed(c);assert(c.save());
 corrupt(offsetof(reader_cache::Header,layout));assert(!c.load());seed(c);assert(c.save());
 std::filesystem::resize_file(root+c.path,40);assert(!c.load());seed(c);assert(c.save());
 writeSource("/book.txt","modified longer source");assert(!c.load());seed(c);assert(c.save());
 BookCache other("/other.txt",false);std::filesystem::copy_file(root+c.path,root+other.path);assert(!other.load());
 SD.failWrite=true;assert(!c.save());SD.failWrite=false;assert(c.load());
 SD.failRename=true;assert(!c.save());SD.failRename=false;assert(!c.load());seed(c);assert(c.save());assert(c.load());
 std::filesystem::create_directory(root+"/dir.epub");writeSource("/dir.epub/chapter.xhtml","chapter");
 BookCache d("/dir.epub",true);d.observe("chapter.xhtml");d.observe("META-INF/encryption.xml");assert(d.save());assert(d.load());
 writeSource("/dir.epub/chapter.xhtml","changed chapter");assert(!d.load());
 uint32_t invalid[]={0,6,6};assert(!reader_cache::validOffsets(invalid,3,12));assert(!reader_cache::validResource("../secret",9));
 std::filesystem::remove_all(root);puts("cache round-trip, bookmarks, corruption, truncation, layout/source invalidation, I/O failures, directory dependencies: PASS");
}
