#pragma once
#include "console.h"
// Opt-in hardware fixtures only; excluded from ordinary and diagnostic builds.
// Cleanup is restricted to exact fixture paths AND SHA-256 contents. Credentials
// are removed only when they equal the fixed test record and no legacy config exists.
#include <mbedtls/sha256.h>
#include <cstring>
#include <algorithm>
#include "reader.h"
#include "sd_reader.h"
#include <SD.h>
namespace network_device_test {
inline String hexDigest(const uint8_t* bytes,size_t count){
  static const char hex[]="0123456789abcdef";String out;
  for(size_t i=0;i<count;++i){out+=hex[bytes[i]>>4];out+=hex[bytes[i]&15];}return out;
}
inline String digestFile(const String& path){
  File f=SD.open(path);if(!f||f.isDirectory())return "";
  mbedtls_sha256_context ctx;mbedtls_sha256_init(&ctx);mbedtls_sha256_starts_ret(&ctx,0);
  uint8_t buf[1024],digest[32];size_t read=0;
  while(f.available()){size_t n=f.read(buf,sizeof(buf));if(!n)break;mbedtls_sha256_update_ret(&ctx,buf,n);read+=n;if((read&65535)==0)delay(1);}
  bool complete=read==f.size();f.close();mbedtls_sha256_finish_ret(&ctx,digest);mbedtls_sha256_free(&ctx);
  return complete?hexDigest(digest,sizeof(digest)):String();
}
inline String digestBook(const String& path,unsigned depth=0){
  if(depth>16)return "";
  File entry=SD.open(path);if(!entry)return "";
  bool directory=entry.isDirectory();
  if(!directory){entry.close();return digestFile(path);}
  std::vector<String> names;File child;
  while((child=entry.openNextFile())){String name=child.name();child.close();int slash=name.lastIndexOf('/');if(slash>=0)name=name.substring(slash+1);names.push_back(name);}
  entry.close();std::sort(names.begin(),names.end(),[](const String& a,const String& b){return a.compareTo(b)<0;});
  mbedtls_sha256_context ctx;mbedtls_sha256_init(&ctx);mbedtls_sha256_starts_ret(&ctx,0);
  const uint8_t tag='D';mbedtls_sha256_update_ret(&ctx,&tag,1);bool ok=true;
  for(const auto& name:names){
    String digest=digestBook(path+"/"+name,depth+1);if(digest.length()!=64){ok=false;break;}
    uint32_t size=name.length();uint8_t encoded[]={uint8_t(size>>24),uint8_t(size>>16),uint8_t(size>>8),uint8_t(size)};
    mbedtls_sha256_update_ret(&ctx,encoded,4);mbedtls_sha256_update_ret(&ctx,(const uint8_t*)name.c_str(),name.length());
    mbedtls_sha256_update_ret(&ctx,(const uint8_t*)digest.c_str(),digest.length());
  }
  uint8_t digest[32];mbedtls_sha256_finish_ret(&ctx,digest);mbedtls_sha256_free(&ctx);
  return ok?hexDigest(digest,sizeof(digest)):String();
}
struct Fixture {const char* path;const char* digest;};
const Fixture fixtures[]={
  {"/Books/__pdy_net_fixture_9f1e_中文.txt","0f6ddaa3cab9be0f18bdb4a61aecb5d274ce243d25f5af83062b60db028eaa6b"},
  {"/Books/__pdy_net_fixture_9f1e_中文-1.txt","0f6ddaa3cab9be0f18bdb4a61aecb5d274ce243d25f5af83062b60db028eaa6b"},
  {"/Books/__pdy_net_fixture_9f1e_binary.epub","8dfc315df088fcd670b9489a6e8012a795f233973eac150e0423815d6676640f"},
  {"/Books/__pdy_net_fixture_9f1e_synthetic.txt","9eb62ad478357ffe24a849f9539ab9ba9e9e63817cb472a21e011b9da8b7f373"},
  {"/Books/__pdy_net_fixture_9f1e_synthetic-1.txt","9eb62ad478357ffe24a849f9539ab9ba9e9e63817cb472a21e011b9da8b7f373"},
  {"/Books/__pdy_net_fixture_9f1e_math.md","0e7848b82348fe50cf2acdbbe221b6b30c9cc7e04cdda1f9476cf4c4bb625ddd"},
  {"/Books/__pdy_net_fixture_9f1e_valid.epub","36d5d0932b3b5972e23512e12af665e89a95e342ec03f3bc28dd5f87e8d7460a"},
};
// The fixture-only commands intentionally do not scan the library or access
// Preferences. An unexpected file at an exact fixture path is never deleted.
// Bound even hash verification when that path contains a different large file.
constexpr size_t MAX_FIXTURE_HASH_BYTES=1024*1024;
inline String fixtureDigest(const Fixture& fixture){
  File file=SD.open(fixture.path);
  if(!file)return "";
  bool eligible=!file.isDirectory()&&file.size()<=MAX_FIXTURE_HASH_BYTES;
  file.close();return eligible?digestFile(fixture.path):String();
}
inline bool command(const String& line){
  if(line=="NET_TEST_FIXTURES"){
    unsigned listed=0,presentCount=0,matched=0;bool ok=sdOK;
    for(const auto& fixture:fixtures){
      bool present=sdOK&&SD.exists(fixture.path);uint64_t bytes=0;String hash;
      if(present){
        ++presentCount;File file=SD.open(fixture.path);if(file){bytes=file.size();file.close();}
        hash=fixtureDigest(fixture);
      }
      bool match=present&&hash==fixture.digest;if(match)++matched;else if(present)ok=false;
      Console.printf("NET_FIXTURE path=%s present=%d bytes=%llu sha256=%s match=%d\n",
        fixture.path,present,(unsigned long long)bytes,hash.length()?hash.c_str():"-",match);++listed;
    }
    Console.printf("NET_FIXTURES_DONE listed=%u present=%u matched=%u ok=%d\n",listed,presentCount,matched,ok);return true;
  }
  if(line=="NET_TEST_FILES_CLEAN"){
    bool ok=sdOK;unsigned removed=0,rejected=0;
    if(sdOK)for(const auto& fixture:fixtures)if(SD.exists(fixture.path)){
      if(fixtureDigest(fixture)==fixture.digest&&SD.remove(fixture.path))++removed;
      else{++rejected;ok=false;}
    }
    // Host may issue RESCAN separately. Avoid an implicit all-library walk and
    // any Preferences calls in this file-only operation.
    Console.printf("NET_FILES_CLEAN_DONE removed=%u rejected=%u ok=%d rescan_required=%d\n",
      removed,rejected,ok,removed!=0);return true;
  }
  if(line=="NET_TEST_HASHES"){
    unsigned count=0;bool ok=sdOK;
    for(const auto& path:sdBooks){
      uint8_t identity[32];mbedtls_sha256_ret((const uint8_t*)path.c_str(),path.length(),identity,0);
      String hash=digestBook(path);if(hash.length()!=64)ok=false;
      Console.printf("NET_HASH %s %s\n",hexDigest(identity,32).c_str(),hash.c_str());++count;
    }
    Console.printf("NET_HASHES_DONE count=%u ok=%d\n",count,ok);return true;
  }
  if(line=="NET_TEST_CLEAN"){
    bool ok=sdOK;unsigned removed=0;
    for(const auto& fixture:fixtures)if(SD.exists(fixture.path)){
      if(digestFile(fixture.path)==fixture.digest&&SD.remove(fixture.path))++removed;else ok=false;
    }
    static const char ssid[]="PDY-TEST-4EDB9C73",pass[]="temporary-fixture-only";
    uint8_t expected[5+sizeof(ssid)-1+sizeof(pass)-1]={'P','W',1,sizeof(ssid)-1,sizeof(pass)-1};
    memcpy(expected+5,ssid,sizeof(ssid)-1);memcpy(expected+5+sizeof(ssid)-1,pass,sizeof(pass)-1);
    uint8_t saved[sizeof(expected)];bool wifiRemoved=false;
    if(prefs.isKey("wifiCfgV1")){
      if(!prefs.isKey("wifiSsid")&&!prefs.isKey("wifiPass")&&prefs.getBytesLength("wifiCfgV1")==sizeof(expected)&&
         prefs.getBytes("wifiCfgV1",saved,sizeof(saved))==sizeof(saved)&&memcmp(saved,expected,sizeof(saved))==0)
        {wifiRemoved=prefs.remove("wifiCfgV1");if(!wifiRemoved)ok=false;}
      else ok=false;
    }
    ok=scanSD()&&ok;Console.printf("NET_CLEAN_DONE removed=%u wifi_removed=%d ok=%d\n",removed,wifiRemoved,ok);return true;
  }
  return false;
}
}
