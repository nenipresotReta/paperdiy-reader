#include "delivery_store.h"
#include <openssl/sha.h>
#include <cassert>
#include <cstdio>
#include <map>
#include <vector>

namespace {
using namespace delivery_store;
struct Fixture {
  std::map<std::string,std::vector<uint8_t>> files;
  SHA256_CTX sha;
  bool available=true,admissible=true,parse=true;
  unsigned mutations=0,failAt=0,published=0;
  size_t largestRead=0,readThisPoll=0;
  bool change(){++mutations;return !failAt||mutations!=failAt;}
  Hash hash(){Hash h;h.context=this;
    h.begin=[](void* p){return SHA256_Init(&static_cast<Fixture*>(p)->sha)==1;};
    h.update=[](void* p,const uint8_t* b,size_t n){return SHA256_Update(&static_cast<Fixture*>(p)->sha,b,n)==1;};
    h.finish=[](void* p,uint8_t out[32]){return SHA256_Final(out,&static_cast<Fixture*>(p)->sha)==1;};
    h.digest=[](void*,const uint8_t* b,size_t n,uint8_t out[32]){return SHA256(b,n,out)!=nullptr;};return h;}
  IO io(){IO io;io.context=this;
    io.stat=[](void* p,const char* path,uint32_t* size){auto& f=*static_cast<Fixture*>(p);assert(f.available);auto it=f.files.find(path);if(it==f.files.end())return Entry::Missing;*size=it->second.size();return Entry::File;};
    io.read=[](void* p,const char* path,uint32_t at,uint8_t* out,size_t n){auto& f=*static_cast<Fixture*>(p);assert(f.available);f.largestRead=std::max(f.largestRead,n);f.readThisPoll+=n;auto it=f.files.find(path);if(it==f.files.end()||uint64_t(at)+n>it->second.size())return false;if(n)std::memcpy(out,it->second.data()+at,n);return true;};
    io.write=[](void* p,const char* path,uint32_t at,const uint8_t* bytes,size_t n,bool truncate){auto& f=*static_cast<Fixture*>(p);assert(f.available);if(!f.change())return false;auto& file=f.files[path];if(truncate)file.clear();if(uint64_t(at)+n>64*1024*1024)return false;file.resize(std::max(file.size(),size_t(at)+n));if(n)std::memcpy(file.data()+at,bytes,n);return true;};
    io.rename=[](void* p,const char* from,const char* to,bool replace){auto& f=*static_cast<Fixture*>(p);assert(f.available);assert(std::string(from).find("/.paperdiy/lan/")==0);if(replace)assert(std::string(to).find("/.paperdiy/lan/")==0);if(!f.change())return false;auto it=f.files.find(from);if(it==f.files.end()||(!replace&&f.files.count(to)))return false;f.files[to]=std::move(it->second);f.files.erase(it);return true;};
    io.remove=[](void* p,const char* path){auto& f=*static_cast<Fixture*>(p);assert(f.available);assert(std::string(path).find("/.paperdiy/lan/")==0);if(!f.change())return false;f.files.erase(path);return true;};
    io.sync=[](void* p,const char*){auto& f=*static_cast<Fixture*>(p);assert(f.available);return f.change();};
    io.ready=[](void* p){return static_cast<Fixture*>(p)->available;};
    io.admit=[](void* p,const Metadata&){return static_cast<Fixture*>(p)->admissible;};
    io.validatePush=[](void* p,const uint8_t*,size_t){return static_cast<Fixture*>(p)->parse;};
    io.published=[](void* p,const Receipt&,const char*){++static_cast<Fixture*>(p)->published;};return io;}
  Metadata metadata(unsigned request,const std::string& body,Kind kind=Kind::Push,uint32_t base=0){
    Metadata m;m.requestId=std::string(24,'0')+hexadecimal(reinterpret_cast<const uint8_t*>(&request),4);
    m.clientId=std::string(16,'a');m.kind=kind;m.name=kind==Kind::Push?"Push.md":"synthetic.md";m.title="Synthetic";
    m.size=body.size();m.baseRevision=base;uint8_t digest[32];SHA256(reinterpret_cast<const uint8_t*>(body.data()),body.size(),digest);m.sha256=hexadecimal(digest,32);m.fingerprint=fingerprint(m,hash());return m;
  }
  void recover(Store& store){store.beginRecovery();for(unsigned i=0;i<20000&&!store.recovered();++i){readThisPoll=0;store.poll();assert(readThisPoll<=POLL_BYTES);}assert(store.recovered());}
  void send(Store& store,const Metadata& m,const std::string& body){
    auto result=store.begin(m);assert(result.code==Code::Ok&&result.state==State::Receiving);
    uint32_t at=0;while(at<body.size()){const size_t n=std::min(size_t(CHUNK_LIMIT),body.size()-at);assert(store.chunk(m.clientId,m.requestId,at,reinterpret_cast<const uint8_t*>(body.data()+at),n).code==Code::Ok);at+=n;}
    assert(store.commit(m.clientId,m.requestId).state==State::Checking);
    for(unsigned i=0;i<20000&&store.activeState()==State::Checking;++i){readThisPoll=0;store.poll();assert(readThisPoll<=POLL_BYTES);}
  }
};
void basic(){
  Fixture f;Store store(f.io(),f.hash());f.recover(store);
  assert(store.pushPath().empty());
  const std::string body="Synthetic **Markdown** $x^2$.";const auto m=f.metadata(1,body);
  f.send(store,m,body);const auto r=store.receipt(m.clientId,m.requestId);
  assert(r.state==State::Complete&&r.receipt.revision==1&&store.pushMetadata().revision==1&&f.published==1);
  assert(store.begin(m).state==State::Complete);
  auto conflicting=m;conflicting.title="Different";assert(store.begin(conflicting).code==Code::Conflict);
  auto next=f.metadata(2,body,Kind::Push,1);f.send(store,next,body);
  assert(store.pushMetadata().revision==1&&f.published==1); // same body no refresh
  store.stop();assert(store.pushMetadata().revision==1);
  Store reboot(f.io(),f.hash());f.recover(reboot);
  assert(reboot.pushMetadata().revision==1&&reboot.receipt(next.clientId,next.requestId).state==State::Complete);
}
void unalignedDuplicateAndGuards(){
  Fixture f;Store store(f.io(),f.hash());f.recover(store);
  std::string body(9000,'s');const auto m=f.metadata(2,body,Kind::File);
  assert(store.begin(m).code==Code::Ok);
  assert(store.chunk(m.clientId,m.requestId,0,reinterpret_cast<const uint8_t*>(body.data()),4096).offset==4096);
  assert(store.chunk(m.clientId,m.requestId,0,reinterpret_cast<const uint8_t*>(body.data()),4096).offset==4096);
  std::string changed(4096,'z');assert(store.chunk(m.clientId,m.requestId,0,reinterpret_cast<const uint8_t*>(changed.data()),4096).code==Code::Conflict);
  assert(store.chunk(m.clientId,m.requestId,4095,reinterpret_cast<const uint8_t*>(body.data()),4096).code==Code::Conflict);
  assert(store.commit(m.clientId,m.requestId).code==Code::Conflict);
  assert(store.begin(f.metadata(3,body,Kind::File)).code==Code::Busy);
  assert(store.status(std::string(16,'b'),m.requestId).code==Code::Unknown);
  assert(store.cancel(m.clientId,m.requestId).state==State::Cancelled);
  Store reboot(f.io(),f.hash());f.recover(reboot);assert(reboot.status(m.clientId,m.requestId).state==State::Cancelled);
  f.available=false;const auto before=f.mutations;reboot.poll();reboot.stop();assert(f.mutations==before);
}
void invalidAndPreservation(){
  Fixture f;Store store(f.io(),f.hash());f.recover(store);
  const std::string old="Previous synthetic Push";auto first=f.metadata(1,old);f.send(store,first,old);
  auto bad=f.metadata(2,"New",Kind::Push,0);assert(store.begin(bad).code==Code::RevisionConflict);
  bad=f.metadata(3,"New",Kind::Push,1);bad.name="../synthetic.md";assert(store.begin(bad).code==Code::Invalid);
  for(const auto& name:{std::string("bad?.md"),std::string("bad.md "),std::string("bad.md."),std::string("bad\xc2\x85.md"),std::string("bad\xc0\x80.md")})assert(!label(name,true));
  f.parse=false;bad=f.metadata(4,"Bad syntax",Kind::Push,1);f.send(store,bad,"Bad syntax");
  assert(store.activeState()==State::Uncertain&&store.lastCode()==Code::Parse&&store.pushMetadata().revision==1);
  assert(f.files[store.pushPath()]==std::vector<uint8_t>(old.begin(),old.end()));
  assert(store.cancel(bad.clientId,bad.requestId).state==State::Cancelled);
  f.admissible=false;assert(store.begin(f.metadata(8,"file",Kind::File)).code==Code::LibraryFull);
}
void faultRecovery(Kind kind){
  // Fail every publication mutation in turn, then reboot with persistent bytes
  // left exactly as the failed callback left them. Never overwrite/delete an
  // existing ordinary source; never create a second final file on replay.
  for(unsigned fault=1;fault<=32;++fault){
    Fixture f;f.files["/Books/synthetic.md"]={'o','l','d'};
    Store store(f.io(),f.hash());f.recover(store);
    std::string old="Old complete Push",body(17000,'n');
    if(kind==Kind::Push){f.send(store,f.metadata(1,old),old);body.resize(15000);}
    const auto m=f.metadata(20,body,kind,kind==Kind::Push?1:0);
    assert(store.begin(m).code==Code::Ok);
    for(size_t at=0;at<body.size();at+=CHUNK_LIMIT){const size_t n=std::min(size_t(CHUNK_LIMIT),body.size()-at);assert(store.chunk(m.clientId,m.requestId,at,reinterpret_cast<const uint8_t*>(body.data()+at),n).code==Code::Ok);}
    assert(store.commit(m.clientId,m.requestId).state==State::Checking);
    f.mutations=0;f.failAt=fault;
    for(unsigned n=0;n<20&&store.activeState()==State::Checking;++n)store.poll();
    f.failAt=0;Store reboot(f.io(),f.hash());f.recover(reboot);
    assert(f.files["/Books/synthetic.md"]==std::vector<uint8_t>({'o','l','d'}));
    const auto result=reboot.status(m.clientId,m.requestId);
    if(result.state==State::Complete){
      const auto replay=reboot.begin(m);assert(replay.state==State::Complete);
      if(kind==Kind::File)assert(result.receipt.finalName=="synthetic (2).md");
    }else if(result.state==State::Uncertain){assert(reboot.begin(m).state==State::Uncertain);}
    if(kind==Kind::Push){
      assert(reboot.pushMetadata().revision>=1&&reboot.pushMetadata().revision<=2);
      const auto& bytes=f.files[reboot.pushPath()];
      assert(bytes==std::vector<uint8_t>(old.begin(),old.end())||bytes==std::vector<uint8_t>(body.begin(),body.end()));
    }else {
      unsigned books=0;for(const auto& entry:f.files)if(entry.first.find("/Books/")==0)++books;
      assert(books<=2);
    }
  }
}
void boundedLedgerAndCorruption(){
  Fixture f;Store store(f.io(),f.hash());f.recover(store);
  auto first=f.metadata(1,"First",Kind::File);
  for(unsigned i=1;i<=40;++i)f.send(store,f.metadata(i,"Synthetic file "+std::to_string(i),Kind::File),"Synthetic file "+std::to_string(i));
  unsigned managed=0;for(const auto& entry:f.files)if(entry.first.find("/.paperdiy/lan/")==0)++managed;
  assert(managed==RECEIPTS&&f.largestRead<=POLL_BYTES);
  Store reboot(f.io(),f.hash());f.recover(reboot);assert(reboot.receipt(first.clientId,first.requestId).code==Code::Unknown);
  Fixture push;Store ps(push.io(),push.hash());push.recover(ps);
  push.send(ps,push.metadata(1,"Old"),"Old");push.send(ps,push.metadata(2,"New",Kind::Push,1),"New");
  push.files[ps.pushPath()][0]^=1;
  Store recovered(push.io(),push.hash());push.recover(recovered);assert(recovered.pushMetadata().revision==1);
}
uint32_t recordCrc(const std::vector<uint8_t>& b){uint32_t v=~uint32_t(0);for(size_t i=0;i+4<b.size();++i){v^=b[i];for(unsigned k=0;k<8;++k)v=(v>>1)^((v&1)?0xedb88320u:0);}return ~v;}
void resealRecord(std::vector<uint8_t>& b){const uint32_t c=recordCrc(b);for(unsigned i=0;i<4;++i)b[b.size()-1-i]=uint8_t(c>>(8*i));}
void pushPosition(){
  Fixture f;Store store(f.io(),f.hash());f.recover(store);
  const std::string body="Synthetic live Push";auto keep=f.metadata(1,body);keep.position=1;
  f.send(store,keep,body);
  assert(store.receipt(keep.clientId,keep.requestId).receipt.metadata.position==1);
  assert(store.pushMetadata().metadata.position==1&&f.published==1);
  // The hint is not fingerprinted, so a replay must repeat it exactly.
  auto top=keep;top.position=0;assert(store.begin(top).code==Code::Conflict);
  assert(store.begin(keep).state==State::Complete);
  auto file=f.metadata(2,"file",Kind::File);file.position=1;assert(store.begin(file).code==Code::Invalid);
  auto bad=f.metadata(3,"Next",Kind::Push,1);bad.position=2;assert(store.begin(bad).code==Code::Invalid);
  // Only byte 493 and the CRC distinguish the two hints on disk.
  const auto& pushRecord=f.files["/.paperdiy/lan/push1.rec"];assert(pushRecord.size()==1024&&pushRecord[493]==1);
  Fixture g;Store other(g.io(),g.hash());g.recover(other);auto plain=g.metadata(1,body);g.send(other,plain,body);
  const auto& plainRecord=g.files["/.paperdiy/lan/push1.rec"];assert(plainRecord[493]==0);
  unsigned differing=0;for(size_t i=0;i<1020;++i)differing+=pushRecord[i]!=plainRecord[i];
  assert(differing==1);
  // Hints survive recovery; records written before this byte existed read as top.
  Store reboot(f.io(),f.hash());f.recover(reboot);
  assert(reboot.pushMetadata().metadata.position==1&&reboot.receipt(keep.clientId,keep.requestId).receipt.metadata.position==1);
  Store old(g.io(),g.hash());g.recover(old);assert(old.pushMetadata().revision==1&&old.pushMetadata().metadata.position==0);
  // A value no firmware writes rejects the record even with a valid CRC.
  auto& sealed=f.files["/.paperdiy/lan/push1.rec"];sealed[493]=2;resealRecord(sealed);
  Store rejected(f.io(),f.hash());f.recover(rejected);assert(rejected.pushMetadata().revision==0);
}
void canonical(){
  Fixture f;Metadata m;m.kind=Kind::Push;m.name="Push.md";m.title="Synthetic";m.size=3;m.sha256=std::string(64,'0');
  assert(fingerprint(m,f.hash())=="7c5dc380b37e359158a305fc6aff88f3397a4c8a7d1b8a2d00c89c3fd0016d74");
}
void pushRecoveryDoesNotWaitForLargeFile(){
  Fixture f;Store store(f.io(),f.hash());f.recover(store);
  const std::string body(40000,'q');const auto m=f.metadata(99,body,Kind::File);
  assert(store.begin(m).code==Code::Ok);
  for(size_t at=0;at<body.size();at+=CHUNK_LIMIT){const auto n=std::min(size_t(CHUNK_LIMIT),body.size()-at);store.chunk(m.clientId,m.requestId,at,reinterpret_cast<const uint8_t*>(body.data()+at),n);}
  assert(store.commit(m.clientId,m.requestId).state==State::Checking);
  f.mutations=0;f.failAt=11; // final file exists; receipt write fails
  for(unsigned i=0;i<20&&store.activeState()==State::Checking;++i)store.poll();
  assert(store.activeState()==State::Uncertain);f.failAt=0;
  Store reboot(f.io(),f.hash());reboot.beginRecovery();
  for(unsigned i=0;i<100&&!reboot.pushRecovered();++i){f.readThisPoll=0;reboot.poll();assert(f.readThisPoll<=POLL_BYTES);}
  assert(reboot.pushRecovered()&&!reboot.recovered());
  for(unsigned i=0;i<100&&!reboot.recovered();++i){f.readThisPoll=0;reboot.poll();assert(f.readThisPoll<=POLL_BYTES);}
  assert(reboot.recovered()&&reboot.receipt(m.clientId,m.requestId).state==State::Complete);
}
}
int main(){canonical();basic();pushPosition();unalignedDuplicateAndGuards();invalidAndPreservation();faultRecovery(Kind::File);faultRecovery(Kind::Push);boundedLedgerAndCorruption();pushRecoveryDoesNotWaitForLargeFile();std::puts("delivery_store tests passed");}
