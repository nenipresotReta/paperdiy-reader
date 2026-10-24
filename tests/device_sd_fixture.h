#pragma once
#if !defined(PAPERDIY_DEVICE_TESTS) || !defined(PAPERDIY_ACCEPTANCE_FIXTURE_HEADER)
#error Fixture installation requires explicit device tests and a private embedded fixture header
#endif
#include "device_sd_audit.h"
#include "sd_fixture_file.h"
#include "sd_fixture_cleanup_file.h"
#include "reader.h"
#include PAPERDIY_ACCEPTANCE_FIXTURE_HEADER
namespace sd_fixture_device {
static_assert(sizeof(paperdiy_acceptance_fixture::DATA)>0&&sizeof(paperdiy_acceptance_fixture::DATA)<=sd_fixture::MAX_BYTES,"Fixture must fit 32 KiB");
static_assert(sizeof(paperdiy_acceptance_fixture::SHA256)==32,"Fixture SHA-256 length");
static_assert(sizeof(paperdiy_acceptance_fixture::PATH)<=128,"Fixture path bound");
inline bool validPath(){
  const char* p=paperdiy_acceptance_fixture::PATH;
  const size_t n=sizeof(paperdiy_acceptance_fixture::PATH)-1;
  static const char prefix[]="/PaperDIY-resume-acceptance-";
  if(n<sizeof(prefix)+5||std::memcmp(p,prefix,sizeof(prefix)-1)||std::strcmp(p+n-5,".epub"))return false;
  for(size_t i=1;i<n;++i){const char c=p[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='.'))return false;}
  return true;
}
inline bool prepareMutation(void* p){
  if(!sd_audit_device::allowed(p))return false;
  storage_validation::sourceChanged();
  static_cast<sd_audit_device::Context*>(p)->lease=storage_validation::lease();
  return sd_audit_device::allowed(p);
}
inline bool removeCommand(const String& line,bool inLibrary){
  if(line!="FIXTURE_REMOVE"&&!line.startsWith("FIXTURE_REMOVE "))return false;
  if(line!="FIXTURE_REMOVE"||!inLibrary||!validPath()||bookIdentity==paperdiy_acceptance_fixture::PATH){
    Console.println("FIXTURE_REMOVE_DONE status=error code=precondition removed=0 source_verified=0 source_removed=0");return true;
  }
  sd_audit_device::Context context{storage_validation::lease(),millis()};
  if(!sd_audit_device::allowed(&context)){
    Console.println("FIXTURE_REMOVE_DONE status=error code=guard removed=0 source_verified=0 source_removed=0");return true;
  }
  std::unique_ptr<uint8_t,decltype(&free)> buffer(static_cast<uint8_t*>(ps_malloc(sd_audit::CHUNK)),free);
  if(!buffer){Console.println("FIXTURE_REMOVE_DONE status=error code=allocation removed=0 source_verified=0 source_removed=0");return true;}
  const char* path=paperdiy_acceptance_fixture::PATH;uint8_t pathHash[32];char identity[65];
  if(mbedtls_sha256_ret(reinterpret_cast<const uint8_t*>(path),strlen(path),pathHash,0)){
    Console.println("FIXTURE_REMOVE_DONE status=error code=hash removed=0 source_verified=0 source_removed=0");return true;
  }
  sd_audit_device::hexadecimal(pathHash,identity);
  char source[sizeof(paperdiy_acceptance_fixture::PATH)+3],base[96];
  snprintf(source,sizeof(source),"/sd%s",path);snprintf(base,sizeof(base),"/sd/.paperdiy-cache/%s.bin",identity);
  sd_fixture::CleanupFile io(source,base,"/sd/.paperdiy-cache",sd_audit_device::allowed,sd_audit_device::cooperate,&context,prepareMutation);
  sd_audit_device::Hash hash;
  Console.printf("FIXTURE_REMOVE_BEGIN path_sha256=%s targets=11\n",identity);
  const auto result=sd_fixture::remove(io,hash,sizeof(paperdiy_acceptance_fixture::DATA),paperdiy_acceptance_fixture::SHA256,
    buffer.get(),sd_audit::CHUNK,sd_audit::HandleStampPolicy::TypeAndLength);
  Console.printf("FIXTURE_REMOVE_DONE status=%s code=%s path_sha256=%s removed=%u source_verified=%u source_removed=%u elapsed_ms=%u\n",
    result.status==sd_fixture::RemoveStatus::Ok?"ok":"error",sd_fixture::name(result.status),identity,unsigned(result.removed),
    unsigned(result.sourceVerified),unsigned(result.sourceRemoved),unsigned(millis()-context.started));return true;
}
inline bool command(const String& line,bool inLibrary){
  if(removeCommand(line,inLibrary))return true;
  if(line!="FIXTURE_INSTALL"&&!line.startsWith("FIXTURE_INSTALL "))return false;
  if(line!="FIXTURE_INSTALL"||!inLibrary||!validPath()){
    Console.println("FIXTURE_INSTALL_DONE status=error code=precondition created=0 written=0 verified=0");return true;
  }
  sd_audit_device::Context context{storage_validation::lease(),millis()};
  if(!sd_audit_device::allowed(&context)){
    Console.println("FIXTURE_INSTALL_DONE status=error code=guard created=0 written=0 verified=0");return true;
  }
  std::unique_ptr<uint8_t,decltype(&free)> buffer(static_cast<uint8_t*>(ps_malloc(sd_audit::CHUNK)),free);
  if(!buffer){Console.println("FIXTURE_INSTALL_DONE status=error code=allocation created=0 written=0 verified=0");return true;}
  const char* path=paperdiy_acceptance_fixture::PATH;uint8_t pathHash[32];char identity[65],expected[65];
  if(mbedtls_sha256_ret(reinterpret_cast<const uint8_t*>(path),strlen(path),pathHash,0)){
    Console.println("FIXTURE_INSTALL_DONE status=error code=hash created=0 written=0 verified=0");return true;
  }
  sd_audit_device::hexadecimal(pathHash,identity);sd_audit_device::hexadecimal(paperdiy_acceptance_fixture::SHA256,expected);
  char nativePath[sizeof(paperdiy_acceptance_fixture::PATH)+3];snprintf(nativePath,sizeof(nativePath),"/sd%s",path);
  sd_fixture::File io(nativePath,sd_audit_device::allowed,sd_audit_device::cooperate,&context,prepareMutation);
  sd_audit_device::Hash inputHash,readHash;
  Console.printf("FIXTURE_INSTALL_BEGIN path_sha256=%s bytes=%u sha256=%s\n",identity,unsigned(sizeof(paperdiy_acceptance_fixture::DATA)),expected);
  const auto result=sd_fixture::run(io,inputHash,readHash,paperdiy_acceptance_fixture::DATA,sizeof(paperdiy_acceptance_fixture::DATA),
    paperdiy_acceptance_fixture::SHA256,buffer.get(),sd_audit::CHUNK,sd_audit::HandleStampPolicy::TypeAndLength);
  Console.printf("FIXTURE_INSTALL_DONE status=%s code=%s path_sha256=%s bytes=%u sha256=%s created=%u written=%u verified=%u read_status=%s elapsed_ms=%u\n",
    result.status==sd_fixture::Status::Ok?"ok":"error",sd_fixture::name(result.status),identity,
    unsigned(sizeof(paperdiy_acceptance_fixture::DATA)),expected,unsigned(result.created),unsigned(result.written),
    unsigned(result.verified),sd_audit::name(result.readStatus),unsigned(millis()-context.started));
  return true;
}
} // namespace sd_fixture_device
