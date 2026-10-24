#pragma once
// Optional device-test fixture creation. No replace, deletion or arbitrary
// command-path interface exists; the firmware adapter supplies one fixed path.
#include "sd_audit_core.h"
namespace sd_fixture {
constexpr size_t MAX_BYTES=32*1024;
enum class Create {Created,Exists,Failed};
enum class Status {Ok,Input,Guard,Hash,Exists,Create,Write,Sync,Close,Readback,Digest};
inline const char* name(Status status){
  switch(status){case Status::Ok:return "ok";case Status::Input:return "input";
    case Status::Guard:return "guard";case Status::Hash:return "hash";
    case Status::Exists:return "exists";case Status::Create:return "create";
    case Status::Write:return "write";case Status::Sync:return "sync";
    case Status::Close:return "close";case Status::Readback:return "readback";
    case Status::Digest:return "digest";}return "invalid";
}
struct Result {
  Status status=Status::Input;sd_audit::Status readStatus=sd_audit::Status::Guard;
  size_t written=0;bool created=false,verified=false;
};
// Every caller provides the embedded expected SHA, verified before create.
// IO extends the read-only audit adapter with prepareMutation, which revokes
// source proofs, and createExclusive/write/sync/closeWrite.
// A short write is a failure, not permission to retry or replace. Failed partial
// files are intentionally retained. Successful return requires durable close
// followed by exact-size/EOF/digest readback under the same live storage guard.
template<class IO,class Hash>
Result run(IO& io,Hash& inputHash,Hash& readHash,const uint8_t* source,size_t bytes,
           const uint8_t* expected,uint8_t* buffer,size_t capacity,
           sd_audit::HandleStampPolicy policy=sd_audit::HandleStampPolicy::FullMetadata){
  Result result;
  if(!source||!expected||!bytes||bytes>MAX_BYTES||!buffer||capacity<sd_audit::CHUNK)return result;
  if(!io.guard()){result.status=Status::Guard;return result;}
  if(!inputHash.begin()){result.status=Status::Hash;return result;}
  for(size_t at=0;at<bytes;){
    if(!io.guard()){result.status=Status::Guard;return result;}
    const size_t n=std::min(sd_audit::CHUNK,bytes-at);
    if(!inputHash.update(source+at,n)){result.status=Status::Hash;return result;}
    at+=n;io.yield();
  }
  uint8_t calculated[32];
  if(!inputHash.finish(calculated)){result.status=Status::Hash;return result;}
  if(std::memcmp(calculated,expected,32)){result.status=Status::Digest;return result;}
  if(!io.guard()){result.status=Status::Guard;return result;}
  if(!io.prepareMutation()||!io.guard()){result.status=Status::Guard;return result;}
  const auto created=io.createExclusive();
  if(created!=Create::Created){result.status=created==Create::Exists?Status::Exists:Status::Create;return result;}
  result.created=true;
  struct Close {IO& io;bool active=true;explicit Close(IO& value):io(value){}
    ~Close(){if(active)io.closeWrite();}} closer(io);
  while(result.written<bytes){
    if(!io.guard()){result.status=Status::Guard;return result;}
    const size_t n=std::min(sd_audit::CHUNK,bytes-result.written);
    const size_t got=io.write(source+result.written,n);
    result.written+=std::min(got,n);
    if(got!=n){result.status=Status::Write;return result;}
    if(!io.guard()){result.status=Status::Guard;return result;}
    io.yield();
  }
  if(!io.guard()){result.status=Status::Guard;return result;}
  if(!io.sync()){result.status=Status::Sync;return result;}
  if(!io.guard()){result.status=Status::Guard;return result;}
  const bool closed=io.closeWrite();closer.active=false;
  if(!closed){result.status=Status::Close;return result;}
  if(!io.guard()){result.status=Status::Guard;return result;}
  const auto read=sd_audit::run(io,readHash,buffer,capacity,policy);result.readStatus=read.status;
  if(read.status!=sd_audit::Status::Ok||read.bytes!=bytes){result.status=Status::Readback;return result;}
  if(std::memcmp(read.digest,expected,32)){result.status=Status::Digest;return result;}
  result.verified=true;result.status=Status::Ok;return result;
}
} // namespace sd_fixture
