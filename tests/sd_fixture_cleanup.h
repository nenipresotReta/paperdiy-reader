#pragma once
#include "sd_fixture_core.h"
namespace sd_fixture {
constexpr size_t CACHE_COUNT=10;
inline const char* cacheSuffix(size_t index){
  static const char* const suffixes[]={"",".tmp",".bak",".chunks",".chunks.tmp",".idx0",".idx1",".exttext",".ext0",".ext1"};
  return index<CACHE_COUNT?suffixes[index]:nullptr;
}
enum class Inspect {Missing,Regular,Invalid,Error};
enum class RemoveStatus {Ok,Input,Guard,Source,Target,Mutation,Unlink};
inline const char* name(RemoveStatus status){
  switch(status){case RemoveStatus::Ok:return "ok";case RemoveStatus::Input:return "input";
    case RemoveStatus::Guard:return "guard";case RemoveStatus::Source:return "source";
    case RemoveStatus::Target:return "target";case RemoveStatus::Mutation:return "mutation";
    case RemoveStatus::Unlink:return "unlink";}return "invalid";
}
struct RemoveResult {RemoveStatus status=RemoveStatus::Input;size_t removed=0;bool sourceVerified=false,sourceRemoved=false;};
// Exactly ten cache targets followed by the source, never a glob. IO owns the
// fixed path mapping; inspect must reject symlinks/directories and oversized
// files. Verify every target before any deletion, then inspect again at removal.
// Source is last so a partial cache cleanup can be retried with source proof.
template<class IO,class Hash>
RemoveResult remove(IO& io,Hash& hash,size_t bytes,const uint8_t* expected,uint8_t* buffer,size_t capacity,
                    sd_audit::HandleStampPolicy policy=sd_audit::HandleStampPolicy::FullMetadata){
  RemoveResult result;
  if(!expected||!bytes||bytes>MAX_BYTES||!buffer||capacity<sd_audit::CHUNK)return result;
  if(!io.guard()){result.status=RemoveStatus::Guard;return result;}
  for(size_t index=0;index<=CACHE_COUNT;++index){
    if(!io.guard()){result.status=RemoveStatus::Guard;return result;}
    const auto state=io.inspect(index);
    if(state==Inspect::Invalid||state==Inspect::Error||(index==CACHE_COUNT&&state!=Inspect::Regular)){
      result.status=RemoveStatus::Target;return result;
    }
  }
  sd_audit::Stamp source;
  if(!io.pathStat(source)||!source.regular||source.bytes!=bytes){result.status=RemoveStatus::Source;return result;}
  const auto proof=sd_audit::run(io,hash,buffer,capacity,policy);
  if(proof.status!=sd_audit::Status::Ok||proof.bytes!=bytes||std::memcmp(proof.digest,expected,32)){
    result.status=RemoveStatus::Source;return result;
  }
  result.sourceVerified=true;
  if(!io.guard()){result.status=RemoveStatus::Guard;return result;}
  if(!io.prepareMutation()){result.status=RemoveStatus::Mutation;return result;}
  for(size_t index=0;index<=CACHE_COUNT;++index){
    if(!io.guard()){result.status=RemoveStatus::Guard;return result;}
    const auto state=io.inspect(index);
    if(state==Inspect::Missing&&index<CACHE_COUNT)continue;
    if(state!=Inspect::Regular){result.status=RemoveStatus::Target;return result;}
    if(!io.guard()){result.status=RemoveStatus::Guard;return result;}
    if(!io.removeOne(index)){result.status=RemoveStatus::Unlink;return result;}
    ++result.removed;if(index==CACHE_COUNT)result.sourceRemoved=true;
    if(!io.guard()){result.status=RemoveStatus::Guard;return result;}
    io.yield();
  }
  result.status=RemoveStatus::Ok;return result;
}
} // namespace sd_fixture
