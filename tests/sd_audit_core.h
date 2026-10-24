#pragma once
// Opt-in device-test implementation, also used by native fault tests.
// No writes, enumeration, progress, cache or credential interfaces exist here.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sd_audit {
constexpr size_t MAX_PATH=1024,CHUNK=8192,COMMAND_BYTES=8+2*MAX_PATH;
constexpr uint64_t MAX_BYTES=64ull*1024*1024;
enum class Status {Ok,Hex,Path,Utf8,Denied,Guard,Open,Stat,NotRegular,TooLarge,Changed,Read,Close,Hash};
inline const char* name(Status s){
  switch(s){
    case Status::Ok:return "ok";case Status::Hex:return "hex";case Status::Path:return "path";
    case Status::Utf8:return "utf8";case Status::Denied:return "denied";case Status::Guard:return "guard";
    case Status::Open:return "open";case Status::Stat:return "stat";case Status::NotRegular:return "not_regular";
    case Status::TooLarge:return "too_large";case Status::Changed:return "changed";case Status::Read:return "read";
    case Status::Close:return "close";case Status::Hash:return "hash";
  }return "invalid";
}
inline size_t commandLimit(const char* line,size_t bytes){return bytes>=8&&!std::memcmp(line,"SD_HASH ",8)?COMMAND_BYTES:64;}
inline int nibble(char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;}
inline Status decode(const char* hex,size_t bytes,char (&out)[MAX_PATH+1]){
  if(!hex||!bytes||(bytes&1)||bytes>MAX_PATH*2)return Status::Hex;
  const size_t count=bytes/2;
  for(size_t i=0;i<count;++i){const int a=nibble(hex[i*2]),b=nibble(hex[i*2+1]);if(a<0||b<0)return Status::Hex;out[i]=char((a<<4)|b);}
  out[count]=0;
  if(count<2||out[0]!='/'||out[count-1]=='/')return Status::Path;
  for(size_t at=0;at<count;){
    uint8_t first=uint8_t(out[at++]);uint32_t cp=first,minimum=0;unsigned tail=0;
    if(first>=0xc2&&first<=0xdf){cp=first&31;tail=1;minimum=0x80;}
    else if(first>=0xe0&&first<=0xef){cp=first&15;tail=2;minimum=0x800;}
    else if(first>=0xf0&&first<=0xf4){cp=first&7;tail=3;minimum=0x10000;}
    else if(first>=0x80)return Status::Utf8;
    if(tail>count-at)return Status::Utf8;
    for(unsigned n=0;n<tail;++n){const uint8_t c=uint8_t(out[at++]);if((c&0xc0)!=0x80)return Status::Utf8;cp=(cp<<6)|(c&63);}
    if(cp<minimum||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return Status::Utf8;
    if(cp<32||(cp>=0x7f&&cp<=0x9f)||cp=='\\')return Status::Path;
  }
  for(size_t start=1,at=1;at<=count;++at)if(at==count||out[at]=='/'){
    const size_t n=at-start;if(!n||(n==1&&out[start]=='.')||(n==2&&out[start]=='.'&&out[start+1]=='.'))return Status::Path;
    if(start==1&&n==9){ // Private shutdown artwork and LAN-owned files are outside this diagnostic.
      static const char denied[]=".paperdiy";bool match=true;
      for(size_t i=0;i<n;++i){char c=out[start+i];if(c>='A'&&c<='Z')c=char(c-'A'+'a');if(c!=denied[i])match=false;}
      if(match)return Status::Denied;
    }
    start=at+1;
  }
  return Status::Ok;
}
struct Stamp {uint64_t bytes=0;int64_t modified=0,changed=0;uint64_t inode=0;bool regular=false;};
inline bool same(const Stamp& a,const Stamp& b){return a.bytes==b.bytes&&a.modified==b.modified&&a.changed==b.changed&&a.inode==b.inode&&a.regular==b.regular;}
enum class HandleStampPolicy {FullMetadata,TypeAndLength};
inline bool sameHandle(const Stamp& a,const Stamp& b,HandleStampPolicy policy){
  return policy==HandleStampPolicy::FullMetadata?same(a,b):a.bytes==b.bytes&&a.regular==b.regular;
}
struct Result {Status status=Status::Guard;uint64_t bytes=0,readBytes=0;uint32_t reads=0;uint8_t digest[32]={};};

// IO must provide only guard/stat/open/read/error/close/yield. Hash offers
// begin/update/finish; a failed or interrupted operation never publishes a digest.
template<class IO,class Hash>
Result run(IO& io,Hash& hash,uint8_t* buffer,size_t capacity,HandleStampPolicy handlePolicy=HandleStampPolicy::FullMetadata){
  Result result;if(!buffer||capacity<CHUNK||!io.guard())return result;
  Stamp before,opened,after,pathAfter;
  if(!io.pathStat(before)){result.status=Status::Stat;return result;}
  if(!before.regular){result.status=Status::NotRegular;return result;}
  if(before.bytes>MAX_BYTES){result.status=Status::TooLarge;return result;}
  result.bytes=before.bytes;
  if(!io.guard())return result;
  if(!io.open()){result.status=Status::Open;return result;}
  struct Close {IO& io;bool active=true;explicit Close(IO& value):io(value){}~Close(){if(active)io.close();}} closer(io);
  // ESP-IDF FAT fstat supplies only mode/size; path stat supplies FAT mtime.
  // The caller selects that limited policy explicitly. Never synthesize missing
  // handle metadata or weaken the independent before/after path comparison.
  if(!io.fileStat(opened)||!sameHandle(before,opened,handlePolicy)){result.status=Status::Changed;return result;}
  if(!hash.begin()){result.status=Status::Hash;return result;}
  while(result.readBytes<result.bytes){
    if(!io.guard()){result.status=Status::Guard;return result;}
    const size_t n=size_t(std::min(uint64_t(CHUNK),result.bytes-result.readBytes));
    const size_t got=io.read(buffer,n);++result.reads;
    if(got!=n||io.error()){result.status=Status::Read;return result;}
    result.readBytes+=got;
    if(!io.guard()){result.status=Status::Guard;return result;}
    if(!hash.update(buffer,got)){result.status=Status::Hash;return result;}
    io.yield();
  }
  if(!io.guard()){result.status=Status::Guard;return result;}
  // A changed length or a readable byte after the declared end is not a valid hash.
  if(!io.fileStat(after)||!io.pathStat(pathAfter)||!sameHandle(opened,after,handlePolicy)||!same(before,pathAfter)){
    result.status=Status::Changed;return result;
  }
  if(io.read(buffer,1)!=0||io.error()){result.status=Status::Read;return result;}
  if(!io.guard()){result.status=Status::Guard;return result;}
  uint8_t digest[32];if(!hash.finish(digest)){result.status=Status::Hash;return result;}
  const bool closed=io.close();closer.active=false;
  if(!closed){result.status=Status::Close;return result;}
  if(!io.guard()){result.status=Status::Guard;return result;}
  std::memcpy(result.digest,digest,32);result.status=Status::Ok;return result;
}
} // namespace sd_audit
