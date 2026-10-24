#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <atomic>

// Volatile source fingerprints are reusable only while the firmware owns the
// mounted card exclusively. A mount, removal, I/O failure or host handoff must
// invalidate that lease. Every firmware source mutation clears the records
// before writing, including writes which later fail. No trust survives boot.
namespace storage_validation {
// Card-detect CHANGE may happen while synchronous loading blocks the app loop.
// The ISR only raises this lock-free latch. It never touches allocation, tables,
// filesystem state or logs. A fresh mount starts with empty proofs; an edge
// during or after beginExclusive remains latched until the next mount.
static_assert(ATOMIC_BOOL_LOCK_FREE==2,"Card event latch must be ISR-safe");
inline __attribute__((always_inline)) std::atomic<bool>& cardEventLatch(){static std::atomic<bool> pending{false};return pending;}
inline __attribute__((always_inline)) void signalCardEvent(){cardEventLatch().store(true,std::memory_order_relaxed);}
inline bool cardEventPending(){return cardEventLatch().load(std::memory_order_relaxed);}
// Main task only: first revoke the lease, then acknowledge the handled edge.
inline void acknowledgeCardEvent(){cardEventLatch().store(false,std::memory_order_relaxed);}
constexpr size_t CAPACITY=64;
struct Fingerprint {
  uint8_t path[32]={},digest[32]={};
  uint32_t size=0,mtime=0;
  bool used=false;
};
struct Graph {uint8_t path[32]={},digest[32]={};bool used=false;};
struct State {
  uint64_t generation=1;
  bool exclusive=false;
  size_t next=0;
  Fingerprint records[CAPACITY];
  Graph graphs[CAPACITY];size_t nextGraph=0;
};
inline State& state(){static State value;return value;}
inline void clear(){
  auto& s=state();++s.generation;s.next=0;s.nextGraph=0;
  for(auto& record:s.records)record.used=false;
  for(auto& graph:s.graphs)graph.used=false;
}
inline void invalidate(){clear();state().exclusive=false;}
inline void beginExclusive(){cardEventLatch().store(false,std::memory_order_relaxed);clear();state().exclusive=true;}
inline void sourceChanged(){clear();}
inline uint64_t lease(){return state().exclusive&&!cardEventPending()?state().generation:0;}
inline bool verifiedGraph(const uint8_t path[32],const uint8_t digest[32]){
  const auto& s=state();if(!s.exclusive||cardEventPending())return false;
  for(const auto& graph:s.graphs)if(graph.used&&!memcmp(graph.path,path,32)&&!memcmp(graph.digest,digest,32))return !cardEventPending();
  return false;
}
inline void rememberGraph(uint64_t leaseId,const uint8_t path[32],const uint8_t digest[32]){
  auto& s=state();if(!leaseId||leaseId!=lease())return;
  Graph* target=nullptr;
  for(auto& graph:s.graphs)if(graph.used&&!memcmp(graph.path,path,32)){target=&graph;break;}
  if(!target){target=&s.graphs[s.nextGraph];s.nextGraph=(s.nextGraph+1)%CAPACITY;}
  memcpy(target->path,path,32);memcpy(target->digest,digest,32);target->used=true;
}
inline bool find(const uint8_t path[32],uint32_t size,uint32_t mtime,uint8_t digest[32]){
  auto& s=state();if(!s.exclusive||cardEventPending())return false;
  for(const auto& record:s.records){
    if(record.used&&record.size==size&&record.mtime==mtime&&!memcmp(record.path,path,32)){
      memcpy(digest,record.digest,32);return !cardEventPending();
    }
  }
  return false;
}
inline void remember(uint64_t leaseId,const uint8_t path[32],uint32_t size,uint32_t mtime,const uint8_t digest[32]){
  auto& s=state();if(!leaseId||leaseId!=lease())return;
  Fingerprint* target=nullptr;
  for(auto& record:s.records)if(record.used&&!memcmp(record.path,path,32)){target=&record;break;}
  if(!target){target=&s.records[s.next];s.next=(s.next+1)%CAPACITY;}
  memcpy(target->path,path,32);memcpy(target->digest,digest,32);
  target->size=size;target->mtime=mtime;target->used=true;
}
}
