#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include "bounded_sd_probe.h"

namespace {
struct TemporaryDirectory {
  char path[64]="/tmp/paperdiy-sd-probe-XXXXXX";
  TemporaryDirectory(){assert(mkdtemp(path));}
  ~TemporaryDirectory(){assert(rmdir(path)==0);}
  std::string file() const{return std::string(path)+"/synthetic.bin";}
};
struct Clock {
  bool cancelled=false;
  unsigned cancelAfter=0,cancelChecks=0;
  uint64_t offset=0;
  bool expire=false;
};
uint64_t now(void* context){
  Clock& clock=*static_cast<Clock*>(context);
  if(clock.expire)clock.offset+=1000000;
  return clock.offset+std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool cancel(void* context){
  Clock& clock=*static_cast<Clock*>(context);
  return clock.cancelled||(clock.cancelAfter&&++clock.cancelChecks>=clock.cancelAfter);
}
void yield(void*){}
void writeFixture(const std::string& path,size_t size){
  FILE* file=std::fopen(path.c_str(),"wb");assert(file);
  uint8_t bytes[4096];for(size_t i=0;i<sizeof(bytes);++i)bytes[i]=uint8_t(i%251);
  while(size){size_t count=std::min(size,sizeof(bytes));assert(std::fwrite(bytes,1,count,file)==count);size-=count;}
  assert(std::fclose(file)==0);
}
void bounds(const bounded_sd_probe::Result& result){
  for(const auto& pass:result.pass){
    assert(pass.bytes<=bounded_sd_probe::MAX_BYTES);
    assert(pass.reads<=(bounded_sd_probe::MAX_BYTES+bounded_sd_probe::CHUNK-1)/bounded_sd_probe::CHUNK);
  }
}
}
int main(){
  TemporaryDirectory directory;const std::string path=directory.file();
  uint8_t buffer[bounded_sd_probe::CHUNK];
  Clock clock;bounded_sd_probe::Hooks hooks;
  hooks.now=now;hooks.cancel=cancel;hooks.yield=yield;hooks.context=&clock;
  using bounded_sd_probe::Status;
  using bounded_sd_probe::SourceKind;
  // Small, exact chunk, exact cap, and beyond-cap sources all read the same
  // prefix twice. Larger source files must not expand the diagnostic budget.
  for(size_t sourceBytes:{size_t(1),size_t(777),size_t(bounded_sd_probe::CHUNK),
      size_t(bounded_sd_probe::MAX_BYTES),size_t(bounded_sd_probe::MAX_BYTES+4096)}){
    writeFixture(path,sourceBytes);
    errno=EACCES; // Successful syscalls must not expose a stale errno.
    auto result=bounded_sd_probe::run(path.c_str(),buffer,sizeof(buffer),hooks);
    bounds(result);assert(result.status==Status::Ok&&result.consistent);
    assert(result.statOk&&result.sourceKind==SourceKind::Regular&&result.sourceSize==int64_t(sourceBytes));
    assert(result.errorNumber==0&&result.statError==0&&result.openError==0);
    size_t expected=std::min(sourceBytes,bounded_sd_probe::MAX_BYTES);
    for(const auto& pass:result.pass){
      assert(pass.bytes==expected);
      assert(pass.reads==(expected+bounded_sd_probe::CHUNK-1)/bounded_sd_probe::CHUNK);
    }
    struct stat info{};assert(stat(path.c_str(),&info)==0&&uint64_t(info.st_size)==sourceBytes);
  }
  clock.cancelled=true;
  auto result=bounded_sd_probe::run(path.c_str(),buffer,sizeof(buffer),hooks);
  bounds(result);assert(result.status==Status::Cancelled&&!result.consistent&&!result.pass[0].bytes);
  clock.cancelled=false;clock.cancelAfter=3;
  result=bounded_sd_probe::run(path.c_str(),buffer,sizeof(buffer),hooks);
  bounds(result);assert(result.status==Status::Cancelled&&!result.consistent&&result.pass[0].bytes>0);
  clock.cancelAfter=0;clock.expire=true;
  result=bounded_sd_probe::run(path.c_str(),buffer,sizeof(buffer),hooks);
  bounds(result);assert(result.status==Status::Deadline&&!result.consistent);
  clock.expire=false;
  result=bounded_sd_probe::run(directory.path,buffer,sizeof(buffer),hooks);
  assert(result.status==Status::NotRegular&&!result.consistent);
  assert(result.statOk&&result.sourceKind==SourceKind::Directory&&result.sourceSize>=0);
  assert(result.errorNumber==0&&result.statError==0&&result.openError==0);
  assert(result.pass[0].reads==0&&result.pass[1].reads==0);
  const std::string absent=std::string(directory.path)+"/absent";
  result=bounded_sd_probe::run(absent.c_str(),buffer,sizeof(buffer),hooks);
  assert(result.status==Status::Open&&!result.consistent);
  assert(!result.statOk&&result.sourceKind==SourceKind::Unknown&&result.sourceSize==-1);
  assert(result.errorNumber==ENOENT&&result.statError==ENOENT&&result.openError==0);
  writeFixture(path,0);result=bounded_sd_probe::run(path.c_str(),buffer,sizeof(buffer),hooks);
  assert(result.status==Status::Empty&&!result.consistent);
  assert(result.statOk&&result.sourceKind==SourceKind::Regular&&result.sourceSize==0&&result.errorNumber==0);
  result=bounded_sd_probe::run(path.c_str(),buffer,sizeof(buffer)-1,hooks);
  assert(result.status==Status::Invalid&&!result.consistent);
  assert(std::remove(path.c_str())==0);
  puts("bounded SD read: zero/small/capped sources, repeat hash, cancellation, deadline, invalid file: PASS");
}
