#include "sd_fixture_core.h"
#include "sd_fixture_file.h"
#include "sd_fixture_cleanup_file.h"
#include "storage_validation.h"
#include <openssl/sha.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
namespace {
struct Hash {
  SHA256_CTX context;bool failBegin=false,failUpdate=false,failFinish=false;
  bool begin(){return !failBegin&&SHA256_Init(&context)==1;}
  bool update(const uint8_t* p,size_t n){return !failUpdate&&SHA256_Update(&context,p,n)==1;}
  bool finish(uint8_t* p){return !failFinish&&SHA256_Final(p,&context)==1;}
};
struct Fake {
  std::vector<uint8_t> data;bool exists=false,writer=false,reader=false,allowed=true;
  bool failCreate=false,failSync=false,failCloseWrite=false,failOpen=false,failRead=false,failCloseRead=false;
  bool shortWrite=false,corrupt=false,extra=false,revokeAfterWrite=false,revokeAfterSync=false,revokeAfterClose=false,revokeOnRead=false;
  size_t writes=0,syncs=0,writeCloses=0,opens=0,readCloses=0,yields=0,creates=0,at=0,maxWrite=0,maxRead=0;
  size_t mutations=0;
  bool guard(){return allowed;}
  bool prepareMutation(){++mutations;return guard();}
  sd_fixture::Create createExclusive(){assert(mutations>0);++creates;if(exists)return sd_fixture::Create::Exists;if(failCreate)return sd_fixture::Create::Failed;exists=writer=true;return sd_fixture::Create::Created;}
  size_t write(const uint8_t* p,size_t n){assert(writer);++writes;maxWrite=std::max(maxWrite,n);size_t got=shortWrite?n-1:n;data.insert(data.end(),p,p+got);if(revokeAfterWrite)allowed=false;return got;}
  bool sync(){assert(writer);++syncs;if(revokeAfterSync)allowed=false;return !failSync;}
  bool closeWrite(){assert(writer);writer=false;++writeCloses;if(revokeAfterClose)allowed=false;
    if(corrupt&&!data.empty())data[0]^=1;if(extra)data.push_back(1);return !failCloseWrite;}
  bool pathStat(sd_audit::Stamp& out){out.bytes=data.size();out.regular=true;out.modified=10;return exists;}
  bool fileStat(sd_audit::Stamp& out){return reader&&pathStat(out);}
  bool open(){++opens;if(failOpen)return false;reader=true;at=0;return true;}
  size_t read(uint8_t* p,size_t n){assert(reader);maxRead=std::max(maxRead,n);if(failRead)return 0;
    const size_t got=std::min(n,data.size()-std::min(at,data.size()));if(got)std::memcpy(p,data.data()+at,got);at+=got;if(revokeOnRead)allowed=false;return got;}
  bool error(){return false;}
  bool close(){assert(reader);reader=false;++readCloses;return !failCloseRead;}
  void yield(){++yields;}
};
std::vector<uint8_t> input(size_t size){std::vector<uint8_t> data(size);for(size_t i=0;i<size;++i)data[i]=uint8_t(i*31);return data;}
sd_fixture::Result run(Fake& io,const std::vector<uint8_t>& data){
  uint8_t expected[32],buffer[8192];SHA256(data.data(),data.size(),expected);Hash sourceHash,readHash;
  return sd_fixture::run(io,sourceHash,readHash,data.data(),data.size(),expected,buffer,sizeof(buffer));
}
void successes(){
  for(const size_t size:{size_t(1),size_t(8192),size_t(8193),size_t(21853),size_t(32768)}){
    Fake io;const auto source=input(size);const auto result=run(io,source);
    assert(result.status==sd_fixture::Status::Ok&&result.created&&result.verified&&result.written==size);
    assert(io.data==source&&io.exists&&!io.writer&&!io.reader&&io.creates==1&&io.syncs==1&&io.writeCloses==1&&io.readCloses==1);
    assert(io.maxRead<=8192&&io.maxWrite<=8192&&io.yields==3*((size+8191)/8192));
    // Exact same existing content is still never replaced or rewritten.
    const auto second=run(io,source);assert(second.status==sd_fixture::Status::Exists&&!second.created&&!second.verified);
    assert(io.data==source&&io.creates==2&&io.writeCloses==1&&io.syncs==1);
  }
  Fake existing;existing.exists=true;existing.data={8,2,9};const auto old=existing.data;
  assert(run(existing,input(20)).status==sd_fixture::Status::Exists&&existing.data==old&&existing.writes==0);
}
void faults(){
  using sd_fixture::Status;const auto source=input(20000);
  for(unsigned mode=0;mode<13;++mode){
    Fake io;Status expected=Status::Guard;
    switch(mode){case 0:io.allowed=false;break;case 1:io.failCreate=true;expected=Status::Create;break;
      case 2:io.shortWrite=true;expected=Status::Write;break;case 3:io.failSync=true;expected=Status::Sync;break;
      case 4:io.failCloseWrite=true;expected=Status::Close;break;case 5:io.revokeAfterWrite=true;break;
      case 6:io.revokeAfterSync=true;break;case 7:io.revokeAfterClose=true;break;
      case 8:io.failOpen=true;expected=Status::Readback;break;case 9:io.failRead=true;expected=Status::Readback;break;
      case 10:io.failCloseRead=true;expected=Status::Readback;break;case 11:io.corrupt=true;expected=Status::Digest;break;
      case 12:io.extra=true;expected=Status::Readback;break;}
    const auto result=run(io,source);assert(result.status==expected&&!result.verified&&!io.writer&&!io.reader);
    assert(io.writeCloses<=1&&io.readCloses<=1);
    if(mode>=2){assert(result.created&&io.exists&&io.writeCloses==1&&!io.data.empty());}
    if(mode==2){assert(result.written==8191&&io.writes==1&&io.syncs==0);}
  }
  Fake lostRead;lostRead.revokeOnRead=true;const auto stopped=run(lostRead,source);
  assert(stopped.status==Status::Readback&&stopped.readStatus==sd_audit::Status::Guard&&lostRead.exists&&lostRead.readCloses==1);
  for(unsigned where=0;where<6;++where){
    Fake io;Hash sourceHash,readHash;Hash& bad=where<3?sourceHash:readHash;
    bad.failBegin=where%3==0;bad.failUpdate=where%3==1;bad.failFinish=where%3==2;
    uint8_t expected[32],buffer[8192];SHA256(source.data(),source.size(),expected);
    const auto result=sd_fixture::run(io,sourceHash,readHash,source.data(),source.size(),expected,buffer,sizeof(buffer));
    assert(!result.verified&&result.status==(where<3?Status::Hash:Status::Readback));
    assert(where<3?io.creates==0:io.exists&&!io.writer&&!io.reader);
  }
  Fake badDigest;Hash sourceHash,readHash;uint8_t expected[32]={},buffer[8192];
  assert(sd_fixture::run(badDigest,sourceHash,readHash,source.data(),source.size(),expected,buffer,sizeof(buffer)).status==Status::Digest&&badDigest.creates==0);
  Fake tooBig;assert(run(tooBig,input(32769)).status==Status::Input&&tooBig.creates==0);
}
bool allowed(void*){return true;}void cooperate(void*){}
struct Lease {uint64_t value;unsigned mutations=0;explicit Lease(uint64_t generation):value(generation){}};
bool liveLease(void* context){const auto& lease=*static_cast<Lease*>(context);return lease.value&&lease.value==storage_validation::lease();}
bool mutateLease(void* context){
  if(!liveLease(context))return false;auto& lease=*static_cast<Lease*>(context);
  storage_validation::sourceChanged();lease.value=storage_validation::lease();++lease.mutations;return liveLease(context);
}
void actualFile(){
  char folder[]="/tmp/paperdiy-sd-fixture-XXXXXX";assert(mkdtemp(folder));const std::string path=std::string(folder)+"/fixture.epub";
  const auto source=input(21853);uint8_t expected[32],buffer[8192];SHA256(source.data(),source.size(),expected);
  storage_validation::beginExclusive();Lease lease(storage_validation::lease());const uint64_t before=lease.value;
  uint8_t pathHash[32]={1},sourceHashValue[32]={2},memo[32];
  storage_validation::remember(lease.value,pathHash,10,7,sourceHashValue);assert(storage_validation::find(pathHash,10,7,memo));
  {
    sd_fixture::File io(path.c_str(),liveLease,cooperate,&lease,mutateLease);Hash sourceHash,readHash;
    const auto result=sd_fixture::run(io,sourceHash,readHash,source.data(),source.size(),expected,buffer,sizeof(buffer));
    assert(result.status==sd_fixture::Status::Ok&&result.verified&&io.writer==-1&&!io.file);
    assert(lease.mutations==1&&lease.value!=before&&!storage_validation::find(pathHash,10,7,memo));
  }
  {
    sd_fixture::File io(path.c_str(),allowed,cooperate,nullptr);Hash sourceHash,readHash;
    const auto changed=input(128);SHA256(changed.data(),changed.size(),expected);
    assert(sd_fixture::run(io,sourceHash,readHash,changed.data(),changed.size(),expected,buffer,sizeof(buffer)).status==sd_fixture::Status::Exists);
  }
  FILE* file=fopen(path.c_str(),"rb");assert(file);std::vector<uint8_t> read(source.size());assert(fread(read.data(),1,read.size(),file)==read.size()&&fgetc(file)==EOF&&read==source);assert(!fclose(file));
  assert(!unlink(path.c_str())&&!rmdir(folder)); // Host temporary-test cleanup only.
}
struct Cleanup : Fake {
  sd_fixture::Inspect states[11];std::vector<size_t> removed;
  size_t failRemove=SIZE_MAX,revokeAfterRemove=SIZE_MAX;bool failMutation=false;
  explicit Cleanup(const std::vector<uint8_t>& source){exists=true;data=source;for(auto& state:states)state=sd_fixture::Inspect::Regular;}
  bool prepareMutation(){++mutations;return !failMutation&&guard();}
  sd_fixture::Inspect inspect(size_t index){assert(index<11);return states[index];}
  bool removeOne(size_t index){assert(index<11&&states[index]==sd_fixture::Inspect::Regular);
    if(index==failRemove)return false;removed.push_back(index);states[index]=sd_fixture::Inspect::Missing;
    if(index==sd_fixture::CACHE_COUNT)exists=false;if(index==revokeAfterRemove)allowed=false;return true;}
};
sd_fixture::RemoveResult cleanup(Cleanup& io,const std::vector<uint8_t>& source){
  uint8_t expected[32],buffer[8192];SHA256(source.data(),source.size(),expected);Hash hash;
  return sd_fixture::remove(io,hash,source.size(),expected,buffer,sizeof(buffer));
}
void removalFaults(){
  using sd_fixture::RemoveStatus;using sd_fixture::Inspect;const auto source=input(21853);
  Cleanup all(source);const auto done=cleanup(all,source);
  assert(done.status==RemoveStatus::Ok&&done.sourceVerified&&done.sourceRemoved&&done.removed==11&&all.mutations==1);
  for(size_t i=0;i<11;++i)assert(all.removed[i]==i);
  Cleanup absentCaches(source);for(size_t i=0;i<10;++i)absentCaches.states[i]=Inspect::Missing;
  assert(cleanup(absentCaches,source).removed==1&&absentCaches.removed[0]==10);
  for(size_t index=0;index<11;++index){
    for(auto bad:{Inspect::Invalid,Inspect::Error}){Cleanup io(source);io.states[index]=bad;
      assert(cleanup(io,source).status==RemoveStatus::Target&&io.removed.empty()&&io.mutations==0);}
  }
  Cleanup missing(source);missing.states[10]=Inspect::Missing;assert(cleanup(missing,source).status==RemoveStatus::Target&&missing.removed.empty());
  Cleanup wrong(source);wrong.data[0]^=1;assert(cleanup(wrong,source).status==RemoveStatus::Source&&wrong.removed.empty()&&wrong.mutations==0);
  Cleanup truncated(source);truncated.data.pop_back();assert(cleanup(truncated,source).status==RemoveStatus::Source&&truncated.removed.empty()&&truncated.mutations==0);
  Cleanup openFailure(source);openFailure.failOpen=true;assert(cleanup(openFailure,source).status==RemoveStatus::Source&&openFailure.removed.empty());
  Cleanup guard(source);guard.allowed=false;assert(cleanup(guard,source).status==RemoveStatus::Guard&&guard.removed.empty());
  Cleanup mutation(source);mutation.failMutation=true;assert(cleanup(mutation,source).status==RemoveStatus::Mutation&&mutation.removed.empty());
  Cleanup edge(source);edge.revokeAfterRemove=2;const auto stopped=cleanup(edge,source);
  assert(stopped.status==RemoveStatus::Guard&&stopped.removed==3&&!stopped.sourceRemoved&&edge.exists);
  Cleanup unlinkFailure(source);unlinkFailure.failRemove=3;const auto partial=cleanup(unlinkFailure,source);
  assert(partial.status==RemoveStatus::Unlink&&partial.removed==3&&unlinkFailure.exists&&!partial.sourceRemoved);
  unlinkFailure.failRemove=SIZE_MAX;const auto retry=cleanup(unlinkFailure,source);
  assert(retry.status==RemoveStatus::Ok&&retry.removed==8&&retry.sourceRemoved);
}
void writeFile(const std::string& path,const std::vector<uint8_t>& bytes){FILE* file=fopen(path.c_str(),"wb");assert(file);assert(fwrite(bytes.data(),1,bytes.size(),file)==bytes.size());assert(!fclose(file));}
void actualRemoval(){
  char folder[]="/tmp/paperdiy-sd-cleanup-XXXXXX";assert(mkdtemp(folder));const std::string root=folder;
  const std::string sourcePath=root+"/fixture.epub",directory=root+"/cache",base=directory+"/fixture.bin",foreign=directory+"/unrelated.bin";
  assert(!mkdir(directory.c_str(),0700));const auto source=input(21853);writeFile(sourcePath,source);writeFile(foreign,{7,9,3});
  for(size_t i=0;i<10;++i)if(i!=3)writeFile(base+sd_fixture::cacheSuffix(i),{uint8_t(i)});
  const std::string link=base+sd_fixture::cacheSuffix(3);assert(!symlink(foreign.c_str(),link.c_str()));
  uint8_t expected[32],buffer[8192];SHA256(source.data(),source.size(),expected);
  sd_fixture::CleanupFile io(sourcePath.c_str(),base.c_str(),directory.c_str(),allowed,cooperate,nullptr);Hash rejectHash;
  assert(sd_fixture::remove(io,rejectHash,source.size(),expected,buffer,sizeof(buffer)).status==sd_fixture::RemoveStatus::Target);
  assert(!access(sourcePath.c_str(),F_OK)&&!access(base.c_str(),F_OK)&&!access(foreign.c_str(),F_OK));
  assert(!unlink(link.c_str()));writeFile(link,{3});Hash hash;
  const auto done=sd_fixture::remove(io,hash,source.size(),expected,buffer,sizeof(buffer));
  assert(done.status==sd_fixture::RemoveStatus::Ok&&done.removed==11&&done.sourceRemoved);
  assert(access(sourcePath.c_str(),F_OK)<0&&!access(foreign.c_str(),F_OK));
  FILE* file=fopen(foreign.c_str(),"rb");assert(file&&fgetc(file)==7&&fgetc(file)==9&&fgetc(file)==3&&fgetc(file)==EOF);assert(!fclose(file));
  assert(!unlink(foreign.c_str())&&!rmdir(directory.c_str())&&!rmdir(folder));
}
} // namespace
int main(){successes();faults();actualFile();removalFaults();actualRemoval();std::puts("Fixture: exclusive creation, durable verified bytes, exact cleanup, retained failures and unrelated-file protection passed");}
