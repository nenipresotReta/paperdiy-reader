#include "sd_audit_core.h"
#include "sd_audit_file.h"
#include <openssl/sha.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <unistd.h>
namespace {
std::string hex(const std::string& s){static const char d[]="0123456789abcdef";std::string out;for(unsigned char c:s){out+=d[c>>4];out+=d[c&15];}return out;}
sd_audit::Status decode(const std::string& path){char out[sd_audit::MAX_PATH+1];const auto encoded=hex(path);return sd_audit::decode(encoded.data(),encoded.size(),out);}
struct Hash {
  SHA256_CTX context;bool failBegin=false,failUpdate=false,failFinish=false;
  bool begin(){return !failBegin&&SHA256_Init(&context)==1;}
  bool update(const uint8_t* p,size_t n){return !failUpdate&&SHA256_Update(&context,p,n)==1;}
  bool finish(uint8_t* p){return !failFinish&&SHA256_Final(p,&context)==1;}
};
struct Fake {
  std::vector<uint8_t> data;sd_audit::Stamp stamp;
  bool permitted=true,opened=false,failOpen=false,failRead=false,failClose=false,failStat=false,failFileStat=false,errorFlag=false,changePath=false,changeOpened=false;
  bool fatHandle=false,changedHandleSize=false,changedHandleKind=false,changeHandleAfterRead=false;
  size_t at=0,maxRead=0,readCalls=0,guards=0,revokeAt=SIZE_MAX,closeCalls=0,yields=0,shortAt=SIZE_MAX;
  explicit Fake(size_t bytes):data(bytes){stamp.bytes=bytes;stamp.regular=true;stamp.modified=7;stamp.inode=1;for(size_t i=0;i<bytes;++i)data[i]=uint8_t(i*31);}
  bool guard(){return permitted&&++guards!=revokeAt;}
  bool pathStat(sd_audit::Stamp& out){out=stamp;if(changePath&&opened)++out.modified;return !failStat;}
  bool fileStat(sd_audit::Stamp& out){out=stamp;if(fatHandle)out.modified=out.changed=out.inode=0;if(changeOpened)++out.inode;
    if(!changeHandleAfterRead||at){if(changedHandleSize)++out.bytes;if(changedHandleKind)out.regular=false;}return !failFileStat;}
  bool open(){if(failOpen)return false;opened=true;return true;}
  size_t read(uint8_t* p,size_t n){assert(opened);++readCalls;maxRead=std::max(n,maxRead);if(failRead)return 0;size_t got=std::min(n,data.size()-std::min(at,data.size()));if(readCalls==shortAt&&got)--got;if(got)std::memcpy(p,data.data()+at,got);at+=got;return got;}
  bool error(){return errorFlag;}
  bool close(){assert(opened);opened=false;++closeCalls;return !failClose;}
  void yield(){++yields;}
};
sd_audit::Result run(Fake& io,sd_audit::HandleStampPolicy policy=sd_audit::HandleStampPolicy::FullMetadata){Hash hash;std::vector<uint8_t> buffer(sd_audit::CHUNK);return sd_audit::run(io,hash,buffer.data(),buffer.size(),policy);}
void pathTests(){
  using sd_audit::Status;
  for(const auto& path:{"/book.epub", "/directory.epub/章.xhtml", "/.paperdiy-cache/123.bin", "/a b.jpg"})assert(decode(path)==Status::Ok);
  for(const auto& path:{"", "/", "relative", "/a/", "/a//b", "/../x", "/a/./b", "/a/../b", "/a\\b", "/a\nb"})assert(decode(path)!=Status::Ok);
  for(const auto& path:{"/.paperdiy/shutdown-1.bin", "/.PAPERDIY/lan/anything", "/.paperdiy"})assert(decode(path)==Status::Denied);
  assert(decode(std::string("/a\0b",4))==Status::Path);
  assert(decode(std::string("/\xc0\xaf",3))==Status::Utf8);
  assert(decode(std::string("/\xed\xa0\x80",4))==Status::Utf8);
  assert(decode(std::string("/\xf4\x90\x80\x80",5))==Status::Utf8);
  assert(decode(std::string("/\xc2\x85",3))==Status::Path);
  assert(decode("/"+std::string(1023,'a'))==Status::Ok);
  assert(decode("/"+std::string(1024,'a'))==Status::Hex);
  char out[1025];assert(sd_audit::decode("2f0",3,out)==Status::Hex);assert(sd_audit::decode("2fzz",4,out)==Status::Hex);
  assert(sd_audit::commandLimit("SD_HASH ",8)==2056);assert(sd_audit::commandLimit("SD_HASH",7)==64);assert(sd_audit::commandLimit("SD_HASHX ",9)==64);
}
void hashTests(){
  using sd_audit::Status;
  for(size_t size:{size_t(0),size_t(1),size_t(8192),size_t(8193),size_t(20000)}){
    Fake io(size);auto result=run(io);uint8_t expected[32];SHA256(io.data.data(),io.data.size(),expected);
    assert(result.status==Status::Ok&&result.bytes==size&&result.readBytes==size&&!std::memcmp(result.digest,expected,32));
    assert(io.maxRead<=8192&&io.closeCalls==1&&!io.opened&&io.yields==(size+8191)/8192);
  }
  for(unsigned failure=0;failure<12;++failure){
    Fake io(20000);Status expected=Status::Guard;
    switch(failure){
      case 0:io.permitted=false;break;
      case 1:io.stamp.regular=false;expected=Status::NotRegular;break;
      case 2:io.stamp.bytes=sd_audit::MAX_BYTES+1;expected=Status::TooLarge;break;
      case 3:io.failOpen=true;expected=Status::Open;break;
      case 4:io.failStat=true;expected=Status::Stat;break;
      case 5:io.failFileStat=true;expected=Status::Changed;break;
      case 6:io.shortAt=2;expected=Status::Read;break;
      case 7:io.errorFlag=true;expected=Status::Read;break;
      case 8:io.changePath=true;expected=Status::Changed;break;
      case 9:io.changeOpened=true;expected=Status::Changed;break;
      case 10:io.failClose=true;expected=Status::Close;break;
      case 11:io.revokeAt=5;break;
    }
    const auto result=run(io);uint8_t zero[32]={};
    assert(result.status==expected&&!io.opened&&!std::memcmp(result.digest,zero,32));
    assert(io.closeCalls<=1&&io.maxRead<=8192);
  }
  Fake extra(10);extra.stamp.bytes=9;assert(run(extra).status==Status::Read);
  for(unsigned n=0;n<3;++n){Fake io(1);Hash hash;hash.failBegin=n==0;hash.failUpdate=n==1;hash.failFinish=n==2;uint8_t buffer[8192];assert(sd_audit::run(io,hash,buffer,sizeof(buffer)).status==Status::Hash&&!io.opened);}
}
void fatHandleTests(){
  using sd_audit::HandleStampPolicy;using sd_audit::Status;
  // Model actual ESP-IDF FAT: stat returns FAT mtime; fstat clears timestamps
  // and inode. This must hash real bytes under the explicit device policy.
  Fake strict(4096);strict.fatHandle=true;assert(run(strict).status==Status::Changed&&strict.readCalls==0);
  Fake fat(4096);fat.fatHandle=true;auto result=run(fat,HandleStampPolicy::TypeAndLength);uint8_t expected[32];
  SHA256(fat.data.data(),fat.data.size(),expected);
  assert(result.status==Status::Ok&&!std::memcmp(result.digest,expected,32)&&fat.closeCalls==1&&fat.yields==1);
  for(unsigned failure=0;failure<6;++failure){
    Fake io(4096);io.fatHandle=true;
    switch(failure){case 0:io.changePath=true;break;case 1:io.changedHandleSize=true;break;
      case 2:io.changedHandleKind=true;break;case 3:io.changedHandleSize=io.changeHandleAfterRead=true;break;
      case 4:io.changedHandleKind=io.changeHandleAfterRead=true;break;case 5:io.failFileStat=true;break;}
    const auto failed=run(io,HandleStampPolicy::TypeAndLength);uint8_t zero[32]={};
    assert(failed.status==Status::Changed&&!std::memcmp(failed.digest,zero,32)&&!io.opened&&io.closeCalls==1);
  }
}
bool allowed(void*){return true;}void cooperate(void* p){++*static_cast<unsigned*>(p);}
void stdioTest(){
  char path[]="/tmp/paperdiy-sd-audit-XXXXXX";int fd=mkstemp(path);assert(fd>=0);
  std::vector<uint8_t> input(20000);for(size_t i=0;i<input.size();++i)input[i]=uint8_t(i);
  assert(write(fd,input.data(),input.size())==ssize_t(input.size()));assert(close(fd)==0);
  unsigned yielded=0;sd_audit::File io(path,allowed,cooperate,&yielded);Hash hash;uint8_t buffer[8192];
  auto result=sd_audit::run(io,hash,buffer,sizeof(buffer));uint8_t expected[32];SHA256(input.data(),input.size(),expected);
  assert(result.status==sd_audit::Status::Ok&&!std::memcmp(result.digest,expected,32)&&yielded==3&&!io.file);
  FILE* check=fopen(path,"rb");assert(check);std::vector<uint8_t> after(input.size());assert(fread(after.data(),1,after.size(),check)==after.size());assert(after==input);assert(fclose(check)==0);assert(unlink(path)==0);
  sd_audit::File directory("/tmp",allowed,cooperate,&yielded);Hash another;assert(sd_audit::run(directory,another,buffer,sizeof(buffer)).status==sd_audit::Status::NotRegular&&!directory.file);
}
} // namespace
int main(){pathTests();hashTests();fatHandleTests();stdioTest();std::puts("SD audit: strict paths, bounded reads, FAT handle policy, fail-closed faults and read-only stdio passed");}
