#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include "document_format.h"

// A single application-task owner drives this store. Network/USB must never
// concurrently mount or mutate its filesystem. All paths are assigned here.
namespace delivery_store {
constexpr uint32_t PUSH_LIMIT=16384,CHUNK_LIMIT=4096,POLL_BYTES=8192;
constexpr unsigned RECEIPTS=16;
enum class Kind : uint8_t { Push=1,File=2 };
enum class State : uint8_t { Unknown,Receiving,Checking,Complete,Cancelled,Uncertain };
enum class Code : uint8_t { Ok,Busy,Invalid,Conflict,RevisionConflict,SizeLimit,Storage,HashMismatch,Parse,Unknown,Uncertain,Cancelled,LibraryFull };
enum class Entry : uint8_t { Missing,File,Directory,Error };
struct Metadata {
  std::string requestId,clientId,fingerprint,name,title,sha256;
  Kind kind=Kind::File;
  uint32_t size=0,baseRevision=0;
  // Push display hint for an open reader: 0 = first page, 1 = keep position.
  // Not part of the fingerprint; replays must repeat it. Files always use 0.
  uint8_t position=0;
};
struct Receipt {Metadata metadata;std::string finalName;uint32_t revision=0;uint64_t sequence=0;State state=State::Unknown;};
struct Result {Code code=Code::Unknown;State state=State::Unknown;uint32_t offset=0,size=0;Receipt receipt;};
struct IO {
  void* context=nullptr;
  Entry (*stat)(void*,const char*,uint32_t*)=nullptr;
  bool (*read)(void*,const char*,uint32_t,uint8_t*,size_t)=nullptr;
  bool (*write)(void*,const char*,uint32_t,const uint8_t*,size_t,bool truncate)=nullptr;
  bool (*rename)(void*,const char*,const char*,bool replaceOwned)=nullptr;
  bool (*remove)(void*,const char*)=nullptr;
  bool (*sync)(void*,const char*)=nullptr;
  bool (*ready)(void*)=nullptr;
  bool (*admit)(void*,const Metadata&)=nullptr;
  bool (*validatePush)(void*,const uint8_t*,size_t)=nullptr;
  // Notification only: enqueue UI/library updates, never reenter this store or
  // open another SD source while publication is finishing.
  void (*published)(void*,const Receipt&,const char*)=nullptr;
};
struct Hash {
  void* context=nullptr;
  bool (*begin)(void*)=nullptr;
  bool (*update)(void*,const uint8_t*,size_t)=nullptr;
  bool (*finish)(void*,uint8_t[32])=nullptr;
  bool (*digest)(void*,const uint8_t*,size_t,uint8_t[32])=nullptr;
};
inline const char* codeName(Code code){
  switch(code){
    case Code::Ok:return "ok";case Code::Busy:return "busy";case Code::Invalid:return "invalid_metadata";
    case Code::Conflict:return "request_conflict";case Code::RevisionConflict:return "revision_conflict";
    case Code::SizeLimit:return "size_limit";case Code::Storage:return "storage_unavailable";
    case Code::HashMismatch:return "hash_mismatch";case Code::Parse:return "invalid_markdown";
    case Code::Uncertain:return "receipt_unknown";case Code::Cancelled:return "cancelled";
    case Code::LibraryFull:return "library_full";default:return "unknown_receipt";
  }
}
inline const char* stateName(State state){
  switch(state){case State::Receiving:return "receiving";case State::Checking:return "checking";
    case State::Complete:return "complete";case State::Cancelled:return "cancelled";
    case State::Uncertain:return "uncertain";default:return "unknown";}
}
inline bool hex(const std::string& value,size_t count){
  if(value.size()!=count)return false;
  for(char c:value)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
  return true;
}
inline bool label(const std::string& value,bool filename=false){
  if(value.size()>96||(filename&&(value.empty()||value[0]=='.'||value.back()=='.'||value.back()==' ')))return false;
  for(unsigned char c:value)if(c<32||c==127||(filename&&std::strchr("/\\:<>\"|?*",c)))return false;
  // Strict UTF-8, including overlongs, surrogates and out-of-range scalars.
  for(size_t p=0;p<value.size();){
    const auto c=uint8_t(value[p++]);if(c<128)continue;
    unsigned n;uint32_t v,min;
    if(c>=0xc2&&c<=0xdf){n=1;v=c&31;min=0x80;}
    else if(c>=0xe0&&c<=0xef){n=2;v=c&15;min=0x800;}
    else if(c>=0xf0&&c<=0xf4){n=3;v=c&7;min=0x10000;}else return false;
    if(p+n>value.size())return false;
    while(n--){const auto d=uint8_t(value[p++]);if((d&0xc0)!=0x80)return false;v=v<<6|(d&63);}
    if(v<min||v>0x10ffff||(v>=0xd800&&v<=0xdfff)||(v>=0x80&&v<=0x9f))return false;
  }
  return true;
}
inline std::string hexadecimal(const uint8_t* bytes,size_t count){
  constexpr char digits[]="0123456789abcdef";std::string out(count*2,'0');
  for(size_t i=0;i<count;++i){out[2*i]=digits[bytes[i]>>4];out[2*i+1]=digits[bytes[i]&15];}return out;
}
inline bool valid(const Metadata& m){
  if(!hex(m.requestId,32)||!hex(m.clientId,16)||!hex(m.fingerprint,64)||!hex(m.sha256,64)||!label(m.name,true)||!label(m.title)||!m.size)return false;
  if(m.position>1||(m.kind!=Kind::Push&&m.position))return false;
  if(m.kind==Kind::Push)return m.name=="Push.md"&&m.size<=PUSH_LIMIT;
  return m.kind==Kind::File&&m.baseRevision==0&&m.size<=document_format::sourceLimit(document_format::detect(m.name.c_str()));
}
inline std::string fingerprint(const Metadata& m,Hash hash){
  std::array<uint8_t,512> bytes{};size_t at=0;
  const std::string values[]={m.kind==Kind::Push?"push":"file",m.name,m.title,std::to_string(m.size),m.sha256,m.kind==Kind::Push?std::to_string(m.baseRevision):""};
  for(const auto& value:values){const size_t n=value.size();if(at>bytes.size()-4||n>bytes.size()-at-4)return {};for(int i=3;i>=0;--i)bytes[at++]=uint8_t(n>>(8*i));std::memcpy(bytes.data()+at,value.data(),n);at+=n;}
  uint8_t digest[32];if(!hash.digest||!hash.digest(hash.context,bytes.data(),at,digest))return {};
  return hexadecimal(digest,32);
}

class Store {
 public:
  Store(IO io,Hash hash):io_(io),hash_(hash){}
  // Startup does no large synchronous read. poll() loads at most one 1 KiB
  // record or hashes at most 8 KiB. No new admission until recovered()==true.
  void beginRecovery(){
    active_=Receipt();pending_=Receipt();push_=Receipt();receipts_={};pushRecords_={};
    nextSequence_=1;recoverAt_=0;recovering_=true;checking_=false;uncertain_=false;
    checkPath_.clear();pushBuffer_.reset();recoveryPush_=0;offset_=0;lastCode_=Code::Ok;hashComplete_=false;
  }
  bool recovered() const {return !recovering_;}
  bool pushRecovered() const {return !recovering_||recoveryPush_>=2;}
  bool busy() const {return recovering_||active_.state==State::Receiving||checking_||uncertain_;}
  const Receipt& pushMetadata() const {return push_;}
  const Metadata& activeMetadata() const {return active_.metadata;}
  State activeState() const {return uncertain_?State::Uncertain:checking_?State::Checking:active_.state;}
  uint32_t acceptedBytes() const {return offset_;}
  Code lastCode() const {return lastCode_;}
  std::string pushPath() const {return push_.state==State::Complete?managed("push",unsigned(push_.revision&1),".md"):std::string();}
  Result begin(const Metadata& metadata){
    if(!ready())return error(Code::Storage);
    if(recovering_)return error(Code::Busy);
    // Receipt lookup must precede both CAS and admission, including old
    // requests replayed after another sender has changed the Push revision.
    if(const auto* receipt=find(metadata.clientId,metadata.requestId))return same(*receipt,metadata)?result(*receipt):error(Code::Conflict);
    if(active_.state!=State::Unknown&&key(active_,metadata.clientId,metadata.requestId))return same(active_,metadata)?current():error(Code::Conflict);
    if(busy())return error(Code::Busy);
    if(!valid(metadata)||fingerprint(metadata,hash_)!=metadata.fingerprint)return error(Code::Invalid);
    if(metadata.kind==Kind::Push&&metadata.baseRevision!=push_.revision)return error(Code::RevisionConflict);
    if(io_.admit&&!io_.admit(io_.context,metadata))return error(Code::LibraryFull);
    active_=Receipt();active_.metadata=metadata;active_.state=State::Receiving;offset_=0;lastCode_=Code::Ok;
    if(!io_.write(io_.context,stage(),0,nullptr,0,true)){active_=Receipt();return error(Code::Storage);}
    return current();
  }
  Result status(const std::string& client,const std::string& request) const {
    if(!hex(client,16)||!hex(request,32))return error(Code::Invalid);
    if(const auto* receipt=find(client,request))return result(*receipt);
    if(key(active_,client,request))return current();
    return error(Code::Unknown);
  }
  Result receipt(const std::string& client,const std::string& request) const {
    if(const auto* value=find(client,request))return result(*value);
    return error(Code::Unknown);
  }
  Result chunk(const std::string& client,const std::string& request,uint32_t at,const uint8_t* bytes,size_t count){
    if(!ready())return fail(Code::Storage);
    if(!key(active_,client,request))return error(Code::Unknown);
    if(uncertain_||checking_||active_.state!=State::Receiving)return current();
    if(!count||count>CHUNK_LIMIT||!bytes||uint64_t(at)+count>active_.metadata.size)return error(Code::SizeLimit);
    if(at<offset_){
      // Identical already-accepted blocks are safe to acknowledge. Different
      // bytes or overlap beyond the accepted prefix are conflicts.
      if(uint64_t(at)+count>offset_||!io_.read(io_.context,stage(),at,buffer_.data(),count)||std::memcmp(buffer_.data(),bytes,count))return error(Code::Conflict);
      return current();
    }
    if(at!=offset_)return error(Code::Conflict);
    if(!io_.write(io_.context,stage(),at,bytes,count,false))return fail(Code::Storage);
    offset_+=uint32_t(count);return current();
  }
  Result commit(const std::string& client,const std::string& request){
    if(const auto* value=find(client,request))return result(*value);
    if(!key(active_,client,request))return error(Code::Unknown);
    if(uncertain_||checking_)return current();
    if(!ready())return fail(Code::Storage);
    if(offset_!=active_.metadata.size)return error(Code::Conflict);
    if(!io_.sync(io_.context,stage())||!startHash(stage()))return fail(Code::Storage);
    checking_=true;active_.state=State::Checking;checkAt_=0;hashComplete_=false;
    if(active_.metadata.kind==Kind::Push){pushBuffer_.reset(new(std::nothrow) uint8_t[active_.metadata.size]);if(!pushBuffer_)return fail(Code::Storage);}
    return current();
  }
  Result cancel(const std::string& client,const std::string& request){
    if(const auto* value=find(client,request))return result(*value);
    if(!key(active_,client,request))return error(Code::Unknown);
    if(!ready())return fail(Code::Storage);
    // Never remove a final book, even for an uncertain publication. Intent is
    // retained if that target exists; cancellation must not erase its proof.
    if(!active_.finalName.empty()){
      uint32_t size;const auto entry=stat(finalPath(active_),size);
      if(entry!=Entry::Missing)return current();
    }
    Receipt cancelled=active_;cancelled.state=State::Cancelled;cancelled.sequence=nextSequence_++;
    if(cancelled.finalName.empty())cancelled.finalName=cancelled.metadata.name;
    if(!writeRecord(receiptPath(cancelled.sequence),cancelled,1))return fail(Code::Storage);
    remember(cancelled);clearPending();active_=Receipt();checking_=uncertain_=false;offset_=0;pushBuffer_.reset();
    return result(cancelled);
  }
  void poll(){
    if(!ready()){if(checking_||active_.state==State::Receiving)fail(Code::Storage);return;}
    if(recovering_){recover();return;}
    if(!checking_)return;
    if(!hashComplete_){hashComplete_=hashStep(active_);return;}
    uint8_t digest[32];
    if(!hash_.finish(hash_.context,digest)){fail(Code::Storage);return;}
    if(hexadecimal(digest,32)!=active_.metadata.sha256){fail(Code::HashMismatch);return;}
    if(active_.metadata.kind==Kind::Push&&(!io_.validatePush||!io_.validatePush(io_.context,pushBuffer_.get(),active_.metadata.size))){fail(Code::Parse);return;}
    publish();
  }
  // Explicit lifecycle stop discards an uncommitted transfer only. Once an
  // intent may have published, leave its fixed recovery evidence untouched.
  void stop(){
    if(active_.state!=State::Unknown&&active_.finalName.empty()&&!uncertain_){
      if(ready())io_.remove(io_.context,stage());active_=Receipt();checking_=false;offset_=0;pushBuffer_.reset();
    }else if(active_.state!=State::Unknown){uncertain_=true;checking_=false;pushBuffer_.reset();}
  }
 private:
  static constexpr size_t RECORD=1024;
  static const char* stage(){return "/.paperdiy/lan/upload.bin";}
  static std::string managed(const char* stem,unsigned slot,const char* extension=".rec"){return std::string("/.paperdiy/lan/")+stem+std::to_string(slot)+extension;}
  static std::string receiptPath(uint64_t sequence){return managed("receipt",unsigned(sequence%RECEIPTS));}
  bool ready() const {return io_.stat&&io_.read&&io_.write&&io_.rename&&io_.remove&&io_.sync&&io_.ready&&io_.ready(io_.context)&&hash_.begin&&hash_.update&&hash_.finish;}
  Entry stat(const std::string& path,uint32_t& size) const {return io_.stat(io_.context,path.c_str(),&size);}
  static bool key(const Receipt& r,const std::string& client,const std::string& request){return r.state!=State::Unknown&&r.metadata.clientId==client&&r.metadata.requestId==request;}
  static bool same(const Receipt& r,const Metadata& m){return r.metadata.fingerprint==m.fingerprint&&r.metadata.sha256==m.sha256&&r.metadata.size==m.size&&r.metadata.kind==m.kind&&r.metadata.name==m.name&&r.metadata.title==m.title&&r.metadata.baseRevision==m.baseRevision&&r.metadata.position==m.position;}
  const Receipt* find(const std::string& client,const std::string& request) const {
    for(const auto& r:receipts_)if(key(r,client,request))return &r;
    for(const auto& r:pushRecords_)if(r.state==State::Complete&&key(r,client,request))return &r;
    return nullptr;
  }
  static Result error(Code code){Result r;r.code=code;return r;}
  static Result result(const Receipt& receipt){Result r;r.receipt=receipt;r.state=receipt.state;r.size=receipt.metadata.size;r.offset=r.size;r.code=receipt.state==State::Cancelled?Code::Cancelled:Code::Ok;return r;}
  Result current() const {Result r=result(active_);r.offset=offset_;r.state=activeState();r.code=uncertain_?Code::Uncertain:lastCode_;return r;}
  Result fail(Code code){lastCode_=code;checking_=false;uncertain_=true;pushBuffer_.reset();return current();}
  void remember(const Receipt& receipt){receipts_[receipt.sequence%RECEIPTS]=receipt;nextSequence_=std::max(nextSequence_,receipt.sequence+1);}
  static std::string finalPath(const Receipt& receipt){return receipt.metadata.kind==Kind::Push?managed("push",receipt.revision&1,".md"):std::string("/Books/")+receipt.finalName;}
  static uint32_t crc(const uint8_t* data,size_t count){uint32_t v=~uint32_t(0);while(count--){v^=*data++;for(unsigned i=0;i<8;++i)v=(v>>1)^((v&1)?0xedb88320u:0);}return ~v;}
  static void number(uint8_t* out,uint64_t value,unsigned count){for(unsigned i=0;i<count;++i)out[count-1-i]=uint8_t(value>>(i*8));}
  static uint64_t number(const uint8_t* in,unsigned count){uint64_t value=0;while(count--)value=value<<8|*in++;return value;}
  static void text(uint8_t* out,const std::string& value,size_t maximum){if(value.size()<=maximum)std::memcpy(out,value.data(),value.size());}
  static std::string text(const uint8_t* in,size_t maximum){size_t n=0;while(n<maximum&&in[n])++n;return std::string(reinterpret_cast<const char*>(in),n);}
  bool readRecord(const std::string& path,Receipt& r,uint8_t wanted){
    uint32_t size;const auto entry=stat(path,size);if(entry==Entry::Missing)return false;
    if(entry!=Entry::File||size!=RECORD||!io_.read(io_.context,path.c_str(),0,buffer_.data(),RECORD))return false;
    const auto* b=buffer_.data();if(std::memcmp(b,"PDL1",4)||b[4]!=wanted||b[6]||b[7]||number(b+RECORD-4,4)!=crc(b,RECORD-4))return false;
    r=Receipt();r.metadata.kind=Kind(b[5]);r.sequence=number(b+8,8);r.metadata.size=number(b+16,4);r.metadata.baseRevision=number(b+20,4);r.revision=number(b+24,4);
    r.metadata.requestId=text(b+28,32);r.metadata.clientId=text(b+60,16);r.metadata.fingerprint=text(b+76,64);r.metadata.sha256=text(b+140,64);
    r.metadata.name=text(b+204,96);r.metadata.title=text(b+300,96);r.finalName=text(b+396,96);
    if(b[492]!=uint8_t(State::Cancelled)&&b[492]!=uint8_t(State::Complete))return false;
    r.state=State(b[492]);
    // Byte 493 was zero before Push positions existed, so older records read
    // as "first page". Older firmware ignores it, keeping records valid on a
    // downgrade. Any other value means the record is not one we wrote.
    if(b[493]>1)return false;
    r.metadata.position=b[493];
    if(!r.sequence||r.sequence==UINT64_MAX||!valid(r.metadata)||!label(r.finalName,true)||fingerprint(r.metadata,hash_)!=r.metadata.fingerprint)return false;
    return true;
  }
  bool writeRecord(const std::string& path,const Receipt& r,uint8_t type){
    auto* b=buffer_.data();std::memset(b,0,RECORD);std::memcpy(b,"PDL1",4);b[4]=type;b[5]=uint8_t(r.metadata.kind);
    number(b+8,r.sequence,8);number(b+16,r.metadata.size,4);number(b+20,r.metadata.baseRevision,4);number(b+24,r.revision,4);
    text(b+28,r.metadata.requestId,32);text(b+60,r.metadata.clientId,16);text(b+76,r.metadata.fingerprint,64);text(b+140,r.metadata.sha256,64);
    text(b+204,r.metadata.name,96);text(b+300,r.metadata.title,96);text(b+396,r.finalName,96);b[492]=uint8_t(r.state);b[493]=r.metadata.position;
    number(b+RECORD-4,crc(b,RECORD-4),4);
    constexpr const char* temporary="/.paperdiy/lan/record.tmp";
    if(!io_.write(io_.context,temporary,0,b,RECORD,true)||!io_.sync(io_.context,temporary)||!io_.rename(io_.context,temporary,path.c_str(),true)||!io_.sync(io_.context,path.c_str()))return false;
    Receipt verified;return readRecord(path,verified,type)&&same(verified,r.metadata)&&verified.sequence==r.sequence&&verified.revision==r.revision&&verified.finalName==r.finalName&&verified.state==r.state;
  }
  bool startHash(const std::string& path){
    uint32_t size;if(stat(path,size)!=Entry::File||size!=active_.metadata.size)return false;
    checkPath_=path;checkAt_=0;hashComplete_=false;return hash_.begin(hash_.context);
  }
  bool hashStep(const Receipt& receipt){
    const uint32_t n=std::min(POLL_BYTES,receipt.metadata.size-checkAt_);
    if(n&&(!io_.read(io_.context,checkPath_.c_str(),checkAt_,buffer_.data(),n)||!hash_.update(hash_.context,buffer_.data(),n))){fail(Code::Storage);return false;}
    if(pushBuffer_)std::memcpy(pushBuffer_.get()+checkAt_,buffer_.data(),n);
    checkAt_+=n;return checkAt_==receipt.metadata.size;
  }
  bool chooseName(){
    if(active_.metadata.kind==Kind::Push){
      if(push_.revision==UINT32_MAX)return false;
      active_.revision=push_.revision+1;active_.finalName="Push.md";return true;
    }
    const auto& name=active_.metadata.name;const size_t extension=document_format::extensionLength(name.c_str());
    for(unsigned i=0;i<256;++i){
      const std::string tail=i?" ("+std::to_string(i+1)+")":"";
      if(name.size()+tail.size()>96)continue;
      const std::string candidate=name.substr(0,name.size()-extension)+tail+name.substr(name.size()-extension);
      uint32_t size;const auto entry=stat(std::string("/Books/")+candidate,size);
      if(entry==Entry::Missing){active_.finalName=candidate;return true;}if(entry==Entry::Error)return false;
    }
    return false;
  }
  void clearPending(){
    // Only managed files are removable; ordinary final names never appear.
    io_.remove(io_.context,"/.paperdiy/lan/pending0.rec");io_.remove(io_.context,"/.paperdiy/lan/pending1.rec");
    io_.remove(io_.context,stage());pending_=Receipt();
  }
  void publish(){
    if(active_.metadata.kind==Kind::Push&&push_.state==State::Complete&&active_.metadata.sha256==push_.metadata.sha256&&active_.metadata.size==push_.metadata.size){
      // Identical body is a receipt-backed no-op; retain current revision,
      // source, title and reader anchor without a publication notification.
      active_.revision=push_.revision;active_.finalName="Push.md";active_.sequence=nextSequence_++;active_.state=State::Complete;
      if(!writeRecord(receiptPath(active_.sequence),active_,1)){fail(Code::Storage);return;}
      remember(active_);clearPending();active_=Receipt();checking_=uncertain_=false;offset_=0;pushBuffer_.reset();lastCode_=Code::Ok;return;
    }
    if(!chooseName()){fail(Code::Storage);return;}
    active_.sequence=nextSequence_++;active_.state=State::Complete;
    // Two fixed intent copies survive one corrupt/torn record. Publication is
    // forbidden until both have been written and read back successfully.
    if(!writeRecord(managed("pending",0),active_,2)||!writeRecord(managed("pending",1),active_,2)){fail(Code::Storage);return;}
    pending_=active_;
    const std::string target=finalPath(active_);uint32_t size;
    if(active_.metadata.kind==Kind::File&&stat(target,size)!=Entry::Missing){fail(Code::Storage);return;}
    if(!io_.rename(io_.context,stage(),target.c_str(),active_.metadata.kind==Kind::Push)||!io_.sync(io_.context,target.c_str())||stat(target,size)!=Entry::File||size!=active_.metadata.size){fail(Code::Storage);return;}
    if(active_.metadata.kind==Kind::Push){
      if(!writeRecord(managed("push",active_.revision&1),active_,3)){fail(Code::Storage);return;}
      push_=active_;pushRecords_[active_.revision&1]=active_;
    }
    if(!writeRecord(receiptPath(active_.sequence),active_,1)){fail(Code::Storage);return;}
    remember(active_);checking_=uncertain_=false;lastCode_=Code::Ok;pushBuffer_.reset();
    clearPending();
    if(io_.published)io_.published(io_.context,active_,target.c_str());
    active_=Receipt();offset_=0;
  }
  void recover(){
    if(recoverAt_<RECEIPTS){Receipt r;if(readRecord(managed("receipt",recoverAt_),r,1))remember(r);++recoverAt_;return;}
    if(recoverAt_<RECEIPTS+2){
      Receipt r;if(readRecord(managed("pending",recoverAt_-RECEIPTS),r,2)&&r.sequence>=pending_.sequence)pending_=r;
      ++recoverAt_;return;
    }
    if(recoverAt_<RECEIPTS+4){
      Receipt r;const unsigned slot=recoverAt_-RECEIPTS-2;
      if(readRecord(managed("push",slot),r,3)&&r.metadata.kind==Kind::Push&&(r.revision&1)==slot)pushRecords_[slot]=r;
      ++recoverAt_;return;
    }
    // Verify at most one Push source per phase, in 8 KiB slices. A corrupt
    // newest generation falls back to the other complete, verified slot.
    if(recoveryPush_<2){
      auto& r=pushRecords_[recoveryPush_];
      if(r.state==State::Unknown){++recoveryPush_;return;}
      if(checkPath_.empty()){active_=r;if(!startHash(finalPath(r))){r=Receipt();active_=Receipt();++recoveryPush_;return;}}
      if(!hashStep(r)){if(uncertain_){r=Receipt();active_=Receipt();uncertain_=false;checkPath_.clear();++recoveryPush_;}return;}
      uint8_t digest[32];const bool good=hash_.finish(hash_.context,digest)&&hexadecimal(digest,32)==r.metadata.sha256;
      if(good){if(r.revision>push_.revision)push_=r;nextSequence_=std::max(nextSequence_,r.sequence+1);}else r=Receipt();
      active_=Receipt();checkPath_.clear();++recoveryPush_;return;
    }
    if(pending_.state!=State::Unknown){
      if(find(pending_.metadata.clientId,pending_.metadata.requestId)){clearPending();recovering_=false;active_=Receipt();return;}
      active_=pending_;offset_=active_.metadata.size;
      if(checkPath_.empty()){
        if(!startHash(finalPath(active_))){uncertain_=true;recovering_=false;return;}
      }
      if(!hashComplete_){hashComplete_=hashStep(active_);if(uncertain_)recovering_=false;return;}
      uint8_t digest[32];
      if(!hash_.finish(hash_.context,digest)||hexadecimal(digest,32)!=active_.metadata.sha256){uncertain_=true;recovering_=false;return;}
      if(active_.metadata.kind==Kind::Push){
        if(!writeRecord(managed("push",active_.revision&1),active_,3)){uncertain_=true;recovering_=false;return;}
        pushRecords_[active_.revision&1]=active_;if(active_.revision>push_.revision)push_=active_;
      }
      if(!writeRecord(receiptPath(active_.sequence),active_,1)){uncertain_=true;recovering_=false;return;}
      remember(active_);clearPending();
      if(io_.published){const auto path=finalPath(active_);io_.published(io_.context,active_,path.c_str());}
      active_=Receipt();offset_=0;
    }
    recovering_=false;
  }
  IO io_;Hash hash_;
  std::array<Receipt,RECEIPTS> receipts_{};
  std::array<Receipt,2> pushRecords_{};
  Receipt active_,pending_,push_;
  std::array<uint8_t,POLL_BYTES> buffer_{};
  std::unique_ptr<uint8_t[]> pushBuffer_;
  std::string checkPath_;
  uint64_t nextSequence_=1;
  uint32_t offset_=0,checkAt_=0;
  unsigned recoverAt_=0,recoveryPush_=0;
  bool recovering_=true,checking_=false,uncertain_=false,hashComplete_=false;
  Code lastCode_=Code::Ok;
};
}
