#include "console.h"
#include "lan_service.h"
#include "lan_tls.h"
#include "lan_auth.h"
#include "lan_batch.h"
#include "lan_nvs.h"
#include "delivery_store_sd.h"
#include "managed_progress.h"
#include "storage_validation.h"
#include "reader.h"
#include "sd_reader.h"
#include "ui.h"
#include <SD.h>
#include <ESPmDNS.h>
#include <mbedtls/sha256.h>
#include <algorithm>
#include <memory>
#include <vector>

namespace lan_service {
namespace {
using delivery_store::Code;using delivery_store::State;using delivery_store::Kind;
lan_nvs::Store credentials;
lan_auth::Registry registry;
bool running=false,storageInitialized=false,libraryChanged=false,credentialsDraining=false,promptDirty=false,mdnsRunning=false;
// P: paired senders may replace Push without a batch; batches and pairing
// belong to N and are refused.
bool liveMode=false,pushUpdated=false;
std::string epoch,address,pushPath,publishedPath,transferClient,transferRequest;
delivery_store::Receipt lastPublished;
delivery_store::Metadata shown;
std::string shownState;
uint32_t badWindow=0,badAttempts=0,lastUi=0,lastProgress=0,transferStarted=0;
uint32_t progressBytes=0;
constexpr uint32_t TRANSFER_IDLE_MS=45000,TRANSFER_TOTAL_MS=30u*60*1000;
constexpr uint64_t SPACE_MARGIN=64u*1024;
constexpr size_t LIBRARY_LIMIT=256;

// One batch per N session slot. Pairing state and the issued token live only
// in RAM and are wiped when the session stops.
struct BatchEntry {delivery_store::Metadata metadata;std::string admission;};
enum class Phase:uint8_t {None,AwaitingReveal,AwaitingAcceptance,Accepted,Expired};
struct Batch {
  Phase phase=Phase::None;
  bool firstContact=false;
  std::string id,clientId,label,pairingId,commit,issuedClient,issuedToken;
  uint8_t deviceNonce[32]{},senderNonce[32]{};
  uint32_t sas=0,started=0,prompted=0;
  std::vector<BatchEntry> entries;
  size_t admitted=0;
};
Batch batch;
Prompt shownPrompt;

bool randomBytes(uint8_t* out,size_t length){for(size_t i=0;i<length;i+=4){const auto value=esp_random();const size_t n=std::min(size_t(4),length-i);std::memcpy(out+i,&value,n);}return true;}
bool hash(const uint8_t* data,size_t length,uint8_t out[32]){return mbedtls_sha256_ret(data,length,out,0)==0;}
std::string randomHex(size_t count){uint8_t bytes[32];if(count>sizeof(bytes))return {};randomBytes(bytes,count);return lan_auth::encode(bytes,count);}
std::string escape(const std::string& text){
  std::string out;out.reserve(text.size()+8);
  for(unsigned char c:text){if(c=='"'||c=='\\'){out+='\\';out+=char(c);}else if(c<32){char b[7];snprintf(b,sizeof(b),"\\u%04x",c);out+=b;}else out+=char(c);}return out;
}
std::string quoted(const std::string& text){return "\""+escape(text)+"\"";}
std::string field(const BoundedHttpRequest& request,const char* name){for(const auto& value:request.arguments)if(value.first==name)return value.second;return {};}
bool hasField(const BoundedHttpRequest& request,const char* name){for(const auto& value:request.arguments)if(value.first==name)return true;return false;}
std::string header(const BoundedHttpRequest& request,const char* name){for(const auto& value:request.headers)if(value.first==name)return value.second;return {};}
bool number(const std::string& text,uint32_t& out){return lan_batch::canonicalDecimal(text,out);}
lan_tls::Response error(int code,const char* reason){return {code,"{\"code\":"+quoted(reason)+"}"};}
std::string receiptJson(const delivery_store::Receipt& receipt){
  const auto& m=receipt.metadata;
  std::string out="{\"requestId\":"+quoted(m.requestId)+",\"fingerprint\":"+quoted(m.fingerprint)+
    ",\"kind\":"+quoted(m.kind==Kind::Push?"push":"file")+",\"size\":"+std::to_string(m.size)+",\"sha256\":"+quoted(m.sha256);
  out+=m.kind==Kind::Push?",\"revision\":"+std::to_string(receipt.revision):",\"name\":"+quoted(receipt.finalName);
  return out+"}";
}
lan_tls::Response resultJson(const delivery_store::Result& result){
  if(result.state==State::Complete&&result.code==Code::Ok)return {200,"{\"state\":\"complete\",\"receipt\":"+receiptJson(result.receipt)+"}"};
  if(result.code!=Code::Ok){
    int status=409;
    if(result.code==Code::Invalid)status=400;
    else if(result.code==Code::SizeLimit)status=413;
    else if(result.code==Code::Unknown)status=404;
    else if(result.code==Code::Storage||result.code==Code::Busy)status=503;
    else if(result.code==Code::Parse||result.code==Code::HashMismatch)status=422;
    return error(status,delivery_store::codeName(result.code));
  }
  const auto& m=result.receipt.metadata;
  return {200,"{\"state\":"+quoted(delivery_store::stateName(result.state))+",\"requestId\":"+quoted(m.requestId)+
    ",\"fingerprint\":"+quoted(m.fingerprint)+",\"offset\":"+std::to_string(result.offset)+",\"size\":"+std::to_string(result.size)+"}"};
}
bool permittedAttempt(){
  if(uint32_t(millis()-badWindow)>=10000){badWindow=millis();badAttempts=0;}
  return badAttempts<8;
}
bool admission(const delivery_store::Metadata& m){return m.kind==Kind::Push||sdBooks.size()<LIBRARY_LIMIT;}
void published(const delivery_store::Receipt& receipt,const char* path){
  lastPublished=receipt;publishedPath=path?path:"";shown=receipt.metadata;shownState="Done";
  if(receipt.metadata.kind==Kind::Push)pushUpdated=true;
}
void publishLibraryRow(){
  if(publishedPath.empty())return;
  const String selected=selectedBook<sdBooks.size()?sdBooks[selectedBook]:String();
  if(lastPublished.metadata.kind==Kind::File){
    const String path=publishedPath.c_str();
    if(std::find(sdBooks.begin(),sdBooks.end(),path)==sdBooks.end())sdBooks.push_back(path);
    std::sort(sdBooks.begin(),sdBooks.end(),[](const String& a,const String& b){return std::strcmp(a.c_str(),b.c_str())<0;});
    selectedBook=0;for(size_t i=0;i<sdBooks.size();++i)if(sdBooks[i]==selected){selectedBook=i;break;}
    libraryChanged=true;
  }
  publishedPath.clear();
}
int base64(char c){if(c>='A'&&c<='Z')return c-'A';if(c>='a'&&c<='z')return c-'a'+26;if(c>='0'&&c<='9')return c-'0'+52;if(c=='+')return 62;if(c=='/')return 63;return -1;}
bool decodeChunk(const std::string& text,uint8_t* bytes,size_t& size){
  size=0;if(text.empty()||text.size()%4||text.size()>((delivery_store::CHUNK_LIMIT+2)/3)*4)return false;
  for(size_t i=0;i<text.size();i+=4){
    const int a=base64(text[i]),b=base64(text[i+1]);const bool p2=text[i+2]=='=',p1=text[i+3]=='=';
    const int c=p2?0:base64(text[i+2]),d=p1?0:base64(text[i+3]);
    if(a<0||b<0||c<0||d<0||(p2&&!p1)||((p1||p2)&&i+4!=text.size())||(p2&&(b&15))||(p1&&!p2&&(c&3)))return false;
    const size_t n=p2?1:p1?2:3;if(size+n>delivery_store::CHUNK_LIMIT)return false;
    bytes[size++]=uint8_t(a<<2|b>>4);if(n>1)bytes[size++]=uint8_t(b<<4|c>>2);if(n>2)bytes[size++]=uint8_t(c<<6|d);
  }return true;
}
void note(const delivery_store::Result& result){
  if(result.state==State::Receiving||result.state==State::Checking){
    const auto& metadata=result.receipt.metadata;
    // A historical receipt may change the display, but cannot restart the
    // current transaction's idle or total lease. Ownership is client scoped.
    const bool newTransfer=transferClient!=metadata.clientId||transferRequest!=metadata.requestId;
    if(newTransfer){transferClient=metadata.clientId;transferRequest=metadata.requestId;transferStarted=lastProgress=millis();progressBytes=0;}
    if(result.offset>progressBytes){progressBytes=result.offset;lastProgress=millis();}
    shown=result.receipt.metadata;
    shownState=result.state==State::Checking?"Checking":"Receiving "+std::to_string(result.size?uint64_t(result.offset)*100/result.size:0)+"%";
  }else if(result.state==State::Complete){shown=result.receipt.metadata;shownState="Done";}
  else if(result.code!=Code::Ok&&result.code!=Code::Unknown){shownState=result.code==Code::Cancelled?"Cancelled":"Transfer failed";}
}

// ---- batch session ----------------------------------------------------------
void wipe(std::string& value){std::fill(value.begin(),value.end(),'\0');value.clear();}
void clearBatch(){
  lan_auth::erase(batch.deviceNonce,sizeof(batch.deviceNonce));lan_auth::erase(batch.senderNonce,sizeof(batch.senderNonce));
  wipe(batch.issuedToken);wipe(batch.commit);
  batch=Batch{};
  if(shownPrompt.active){shownPrompt=Prompt{};promptDirty=true;}
}
bool promptVisible(){return batch.phase==Phase::AwaitingAcceptance;}
void buildPrompt(){
  Prompt next;
  next.active=promptVisible();
  if(next.active){
    next.firstContact=batch.firstContact;next.label=batch.label;
    if(batch.firstContact)next.sas=lan_batch::sasText(batch.sas);
    const auto& push=delivery_store_sd::store().pushMetadata();
    for(const auto& entry:batch.entries){
      if(entry.admission!="ok")continue;
      ++next.total;
      if(entry.metadata.kind==Kind::Push){
        next.pushTitle=entry.metadata.title;next.hasPush=true;
        if(push.state==State::Complete)next.replacesPush=push.metadata.title.empty()?"Push":push.metadata.title;
      }
      if(next.names.size()<lan_batch::PROMPT_NAMES)
        next.names.push_back(entry.metadata.kind==Kind::Push?"Push: "+(entry.metadata.title.empty()?std::string("untitled"):entry.metadata.title):entry.metadata.name);
    }
  }
  shownPrompt=next;promptDirty=true;
}
uint64_t freeBytes(){
  const uint64_t total=SD.totalBytes(),used=SD.usedBytes();
  return total>used?total-used:0;
}
// Advisory precheck. Upload-time and commit-time checks remain authoritative.
std::string precheck(delivery_store::Metadata m,const std::string& client,bool& pushSeen,size_t& files,uint64_t& bytes,uint64_t available){
  auto& store=delivery_store_sd::store();
  if(m.kind==Kind::Push){if(m.name!="Push.md")return "invalid_metadata";if(m.size>delivery_store::PUSH_LIMIT)return "size_limit";}
  else{
    const auto format=document_format::detect(m.name.c_str());
    if(format==document_format::Kind::Unsupported)return "unsupported_format";
    if(m.size>document_format::sourceLimit(format))return "size_limit";
  }
  m.clientId=client.empty()?std::string(16,'0'):client;
  delivery_store::Hash digest;digest.digest=[](void*,const uint8_t* bytes,size_t length,uint8_t out[32]){return hash(bytes,length,out);};
  if(!delivery_store::valid(m)||delivery_store::fingerprint(m,digest)!=m.fingerprint)return "invalid_metadata";
  if(!client.empty()){
    const auto previous=store.receipt(client,m.requestId);
    if(previous.state==State::Complete||previous.state==State::Cancelled)return "already_received";
  }
  if(m.kind==Kind::Push){
    if(pushSeen)return "duplicate_push";
    if(m.baseRevision!=store.pushMetadata().revision)return "revision_conflict";
  }else if(sdBooks.size()+files+1>LIBRARY_LIMIT)return "library_full";
  if(bytes+m.size+SPACE_MARGIN>available)return "no_space";
  if(m.kind==Kind::Push)pushSeen=true;else ++files;
  bytes+=m.size;return "ok";
}
bool batchFinished(){
  if(batch.phase==Phase::None||batch.phase==Phase::Expired)return true;
  if(batch.phase!=Phase::Accepted)return false;
  auto& store=delivery_store_sd::store();
  for(const auto& entry:batch.entries){
    if(entry.admission!="ok")continue;
    const auto r=store.receipt(batch.clientId,entry.metadata.requestId);
    if(r.state!=State::Complete&&r.state!=State::Cancelled)return false;
  }
  return true;
}
const BatchEntry* acceptedEntry(const std::string& client,const std::string& request){
  if(batch.phase!=Phase::Accepted||batch.clientId!=client)return nullptr;
  for(const auto& entry:batch.entries)if(entry.admission=="ok"&&entry.metadata.requestId==request)return &entry;
  return nullptr;
}
const char* phaseName(){
  switch(batch.phase){
    case Phase::AwaitingReveal:return "awaiting_reveal";case Phase::AwaitingAcceptance:return "awaiting_acceptance";
    case Phase::Accepted:return "accepted";case Phase::Expired:return "expired";default:return "none";
  }
}
std::string entriesJson(){
  std::string out="[";bool first=true;
  for(const auto& entry:batch.entries){
    if(!first)out+=',';first=false;
    out+="{\"requestId\":"+quoted(entry.metadata.requestId)+",\"admission\":"+quoted(entry.admission)+"}";
  }
  return out+"]";
}
lan_tls::Response batchJson(bool includeToken){
  std::string out="{\"batchId\":"+quoted(batch.id)+",\"state\":"+quoted(phaseName())+",\"entries\":"+entriesJson();
  if(includeToken&&batch.phase==Phase::Accepted&&!batch.issuedToken.empty())
    out+=",\"deviceId\":"+quoted(lan_tls::deviceId())+",\"clientId\":"+quoted(batch.issuedClient)+",\"token\":"+quoted(batch.issuedToken)+
      ",\"pushRevision\":"+std::to_string(delivery_store_sd::store().pushMetadata().revision);
  return {200,out+"}"};
}
std::string registeredLabel(const std::string& client){
  for(size_t slot=0;slot<lan_auth::CAPACITY;++slot){std::string id,label;if(registry.clientInfo(slot,id,label)&&id==client)return label;}
  return {};
}
lan_tls::Response offer(const BoundedHttpRequest& incoming,bool authenticated,const std::string& client){
  auto& store=delivery_store_sd::store();
  const bool firstContact=!authenticated;
  // Every unauthenticated offer consumes rate-limit budget, valid or not.
  if(firstContact)++badAttempts;
  if(!store.recovered())return error(503,"recovering");
  // A sender may replace its own accepted batch that it stopped uploading;
  // the replacement needs a fresh A+B. Other senders wait for it to finish.
  const bool ownAccepted=authenticated&&batch.phase==Phase::Accepted&&batch.clientId==client;
  if((!batchFinished()&&!ownAccepted)||store.busy())return error(409,"batch_busy");
  std::string label,commit;
  if(firstContact){
    if(!hasField(incoming,"label")||!hasField(incoming,"commit"))return error(401,"unauthorized");
    label=field(incoming,"label");commit=field(incoming,"commit");
    if(!lan_auth::labelValid(label)||!lan_auth::hex(commit,32))return error(400,"invalid_pairing");
    if(registry.count()>=lan_auth::CAPACITY)return error(409,"client_limit");
  }
  std::vector<delivery_store::Metadata> parsed;
  for(size_t i=0;i<lan_batch::MAX_ENTRIES;++i){
    const std::string name="e"+std::to_string(i);
    if(!hasField(incoming,name.c_str())){
      for(size_t j=i+1;j<lan_batch::MAX_ENTRIES;++j)if(hasField(incoming,("e"+std::to_string(j)).c_str()))return error(400,"invalid_entry");
      break;
    }
    delivery_store::Metadata m;
    if(!lan_batch::parseEntry(field(incoming,name.c_str()),m))return error(400,"invalid_entry");
    for(const auto& previous:parsed)if(previous.requestId==m.requestId)return error(400,"invalid_entry");
    parsed.push_back(m);
  }
  for(const auto& argument:incoming.arguments){
    const auto& key=argument.first;
    const bool known=key=="label"||key=="commit"||(key.size()==2&&key[0]=='e'&&key[1]>='0'&&key[1]<'0'+int(lan_batch::MAX_ENTRIES));
    if(!known)return error(400,"invalid_entry");
  }
  if(parsed.empty()&&!firstContact)return error(400,"invalid_entry");
  // A paired sender is shown by its registry label, never a request field.
  if(!firstContact)label=registeredLabel(client);
  Batch next;next.firstContact=firstContact;next.clientId=firstContact?std::string():client;next.label=label;next.commit=commit;
  bool pushSeen=false;size_t files=0;uint64_t bytes=0;const uint64_t available=parsed.empty()?0:freeBytes();
  for(auto& m:parsed){
    BatchEntry entry;entry.metadata=m;entry.metadata.clientId=next.clientId;
    entry.admission=precheck(m,next.clientId,pushSeen,files,bytes,available);
    if(entry.admission=="ok")++next.admitted;
    next.entries.push_back(entry);
  }
  if(!parsed.empty()&&!next.admitted){
    // Nothing to accept: no prompt, no pairing, no retained session state.
    std::string out="{\"state\":\"blocked\",\"entries\":[";bool first=true;
    for(const auto& entry:next.entries){if(!first)out+=',';first=false;out+="{\"requestId\":"+quoted(entry.metadata.requestId)+",\"admission\":"+quoted(entry.admission)+"}";}
    return {200,out+"]}"};
  }
  clearBatch();
  batch=next;batch.id=randomHex(8);batch.started=millis();
  if(firstContact){
    batch.pairingId=randomHex(8);randomBytes(batch.deviceNonce,sizeof(batch.deviceNonce));
    batch.phase=Phase::AwaitingReveal;
    return {200,"{\"batchId\":"+quoted(batch.id)+",\"state\":\"awaiting_reveal\",\"pairingId\":"+quoted(batch.pairingId)+
      ",\"deviceNonce\":"+quoted(lan_auth::encode(batch.deviceNonce,32))+",\"deviceId\":"+quoted(lan_tls::deviceId())+",\"entries\":"+entriesJson()+"}"};
  }
  batch.phase=Phase::AwaitingAcceptance;batch.prompted=millis();buildPrompt();
  shown={};shownState="";
  return batchJson(false);
}
lan_tls::Response reveal(const BoundedHttpRequest& incoming){
  const auto pairingId=field(incoming,"pairingId"),nonce=field(incoming,"senderNonce");
  if(!batch.firstContact||batch.phase!=Phase::AwaitingReveal||!lan_auth::equal(pairingId.data(),batch.pairingId.data(),std::min(pairingId.size(),batch.pairingId.size()))||pairingId.size()!=batch.pairingId.size()){
    ++badAttempts;return error(404,"unknown_pairing");
  }
  uint8_t senderNonce[32];std::string commit;
  if(!lan_batch::decodeHex(nonce,senderNonce,32)||!lan_batch::commitment(hash,senderNonce,commit)||!lan_batch::equalTime(commit,batch.commit)){
    // A failed reveal ends this attempt; the sender must start again.
    ++badAttempts;clearBatch();lan_auth::erase(senderNonce,sizeof(senderNonce));return error(409,"pairing_rejected");
  }
  uint8_t certificate[32];
  if(!lan_batch::decodeHex(lan_tls::fingerprint(),certificate,32)||
     !lan_batch::sas(hash,certificate,senderNonce,batch.deviceNonce,batch.pairingId,batch.label,batch.sas)){
    clearBatch();lan_auth::erase(senderNonce,sizeof(senderNonce));return error(503,"pairing_unavailable");
  }
  std::memcpy(batch.senderNonce,senderNonce,32);lan_auth::erase(senderNonce,sizeof(senderNonce));
  batch.phase=Phase::AwaitingAcceptance;batch.prompted=millis();buildPrompt();
  shown={};shownState="";
  return {200,"{\"batchId\":"+quoted(batch.id)+",\"state\":\"awaiting_acceptance\"}"};
}
lan_tls::Response status(const BoundedHttpRequest& incoming,bool authenticated,const std::string& client){
  const auto id=field(incoming,"batchId");
  if(batch.phase==Phase::None||!lan_auth::hex(id,8)||id!=batch.id){if(!authenticated)++badAttempts;return error(404,"unknown_batch");}
  if(authenticated&&!batch.firstContact&&batch.clientId==client)return batchJson(false);
  if(authenticated&&batch.firstContact&&batch.issuedClient==client)return batchJson(false);
  if(!batch.firstContact||batch.phase==Phase::AwaitingReveal){if(!authenticated)++badAttempts;return error(404,"unknown_batch");}
  std::string expected;
  if(!lan_batch::statusProof(hash,batch.senderNonce,batch.id,expected)||!lan_batch::equalTime(field(incoming,"proof"),expected)){
    ++badAttempts;return error(404,"unknown_batch");
  }
  return batchJson(true);
}
lan_tls::Response request(const BoundedHttpRequest& incoming){
  if(!running||!registry.valid())return error(503,"credentials_unavailable");
  if(!permittedAttempt())return error(429,"rate_limited");
  auto& store=delivery_store_sd::store();
  std::string client;const bool authenticated=registry.authenticate(header(incoming,"authorization"),hash,client);
  // The first bearer use proves the sender stored its token; drop the RAM copy.
  if(authenticated&&!batch.issuedToken.empty()&&client==batch.issuedClient)wipe(batch.issuedToken);
  const bool batchPath=incoming.path=="/lan/v2/batch/offer"||incoming.path=="/lan/v2/pair/reveal"||incoming.path=="/lan/v2/batch/status";
  if(liveMode&&batchPath){if(!authenticated)++badAttempts;return error(409,"live_mode");}
  if(incoming.path=="/lan/v2/batch/offer"&&incoming.post){
    if(!authenticated&&!header(incoming,"authorization").empty()){++badAttempts;return error(401,"unauthorized");}
    return offer(incoming,authenticated,client);
  }
  if(incoming.path=="/lan/v2/pair/reveal"&&incoming.post)return reveal(incoming);
  // Unauthenticated identity probe for first contact: the device ID is already
  // advertised over mDNS and the Push revision is observable via the precheck.
  if(incoming.path=="/lan/v2/hello"&&!incoming.post){
    if(!authenticated)++badAttempts;
    if(!store.pushRecovered())return error(503,"recovering");
    return {200,"{\"v\":2,\"deviceId\":"+quoted(lan_tls::deviceId())+",\"pushRevision\":"+std::to_string(store.pushMetadata().revision)+",\"batchEntries\":8,\"live\":"+(liveMode?"true":"false")+"}"};
  }
  if(incoming.path=="/lan/v2/batch/status"&&!incoming.post)return status(incoming,authenticated,client);
  if(incoming.path=="/lan/v1/clients/revoke"&&incoming.post){
    const auto target=field(incoming,"clientId");
    if(!authenticated||client!=target){++badAttempts;return error(401,"unauthorized");}
    if(!registry.revoke(credentials,target))return error(500,"revocation_uncertain");
    return {200,"{\"revoked\":true}"};
  }
  if(!authenticated){++badAttempts;return error(401,"unauthorized");}
  if(incoming.path=="/lan/v1/capabilities"&&!incoming.post){
    if(!store.pushRecovered())return error(503,"recovering");
    return {200,"{\"v\":2,\"deviceId\":"+quoted(lan_tls::deviceId())+",\"pushRevision\":"+std::to_string(store.pushMetadata().revision)+
      ",\"epoch\":"+quoted(epoch)+",\"receiptCapacity\":16,\"receiptScope\":\"client\",\"batchEntries\":8,\"limits\":{\"chunkBytes\":4096,\"pushBytes\":16384,\"txtBytes\":4194304,\"epubBytes\":67108864,\"markdownBytes\":1048576},\"live\":"+(liveMode?"true":"false")+"}"};
  }
  if(incoming.path=="/lan/v1/push"&&!incoming.post){
    if(!store.pushRecovered())return error(503,"recovering");const auto& push=store.pushMetadata();
    return {200,"{\"revision\":"+std::to_string(push.revision)+",\"title\":"+quoted(push.metadata.title)+",\"sha256\":"+quoted(push.metadata.sha256)+",\"size\":"+std::to_string(push.metadata.size)+"}"};
  }
  if(incoming.path=="/lan/v1/uploads/start"&&incoming.post){
    delivery_store::Metadata m;m.clientId=client;m.requestId=field(incoming,"requestId");m.fingerprint=field(incoming,"fingerprint");
    m.name=field(incoming,"name");m.title=field(incoming,"title");m.sha256=field(incoming,"sha256");
    const auto kind=field(incoming,"kind");if(kind!="push"&&kind!="file")return error(400,"invalid_metadata");m.kind=kind=="push"?Kind::Push:Kind::File;
    const auto base=field(incoming,"baseRevision");
    if(!number(field(incoming,"size"),m.size)||(m.kind==Kind::Push?!number(base,m.baseRevision):!base.empty()))return error(400,"invalid_metadata");
    if(hasField(incoming,"position")){
      const auto position=field(incoming,"position");
      if(m.kind!=Kind::Push||(position!="top"&&position!="keep"))return error(400,"invalid_metadata");
      m.position=position=="keep";
    }
    // A replay of a finished request returns its receipt; anything new must be
    // part of the batch the user accepted in this N session, or a Push in P.
    const auto previous=store.receipt(client,m.requestId);
    if(previous.state==State::Unknown&&!(liveMode&&m.kind==Kind::Push)){
      const auto* entry=acceptedEntry(client,m.requestId);
      if(!entry||entry->metadata.fingerprint!=m.fingerprint)return error(409,"not_accepted");
    }
    const auto result=store.begin(m);note(result);return resultJson(result);
  }
  const auto id=field(incoming,"requestId");if(!lan_auth::hex(id,16))return error(400,"invalid_request_id");
  delivery_store::Result result;
  if(incoming.path=="/lan/v1/uploads/chunk"&&incoming.post){
    uint32_t offset;std::unique_ptr<uint8_t[]> bytes(new(std::nothrow) uint8_t[delivery_store::CHUNK_LIMIT]);size_t size;
    if(!bytes)return error(503,"memory_unavailable");
    if(!number(field(incoming,"offset"),offset)||!decodeChunk(field(incoming,"data"),bytes.get(),size))return error(400,"invalid_chunk");
    result=store.chunk(client,id,offset,bytes.get(),size);
  }else if(incoming.path=="/lan/v1/uploads/commit"&&incoming.post)result=store.commit(client,id);
  else if(incoming.path=="/lan/v1/uploads/cancel"&&incoming.post)result=store.cancel(client,id);
  else if(incoming.path=="/lan/v1/uploads/status"&&!incoming.post)result=store.status(client,id);
  else if(incoming.path=="/lan/v1/receipts"&&!incoming.post)result=store.receipt(client,id);
  else return error(404,"not_found");
  note(result);return resultJson(result);
}
void startMdns(){
  if(mdnsRunning)return;
  const std::string host=std::string("paperdiy-")+lan_tls::deviceId();
  if(!MDNS.begin(host.c_str()))return;
  mdnsRunning=true;
  if(MDNS.addService("paperdiy","tcp",lan_tls::PORT))MDNS.addServiceTxt("paperdiy","tcp","id",lan_tls::deviceId());
}
void stopMdns(){if(mdnsRunning){MDNS.end();mdnsRunning=false;}}
std::string batchProgress(){
  if(batch.phase!=Phase::Accepted||!batch.admitted)return {};
  auto& store=delivery_store_sd::store();size_t done=0;
  for(const auto& entry:batch.entries)if(entry.admission=="ok"&&store.receipt(batch.clientId,entry.metadata.requestId).state==State::Complete)++done;
  if(done==batch.admitted)return "Done · "+std::to_string(done)+(done==1?" file":" files");
  return std::to_string(done)+"/"+std::to_string(batch.admitted);
}
}
bool isPushPath(const char* path){return managed_progress::isPushPath(path);}
const char* currentPushPath(){return pushPath.c_str();}
bool refreshLibrary(){
  if(sdOwnedByUsb()||!sdOK||!storageInitialized||!delivery_store_sd::store().pushRecovered())return false;
  const auto next=delivery_store_sd::store().pushPath();
  const String selected=selectedBook<sdBooks.size()?sdBooks[selectedBook]:String();
  const auto old=sdBooks;
  sdBooks.erase(std::remove_if(sdBooks.begin(),sdBooks.end(),[](const String& p){return isPushPath(p.c_str());}),sdBooks.end());
  if(!next.empty())sdBooks.insert(sdBooks.begin(),String(next.c_str()));
  selectedBook=0;for(size_t i=0;i<sdBooks.size();++i)if(sdBooks[i]==selected||(isPushPath(selected.c_str())&&isPushPath(sdBooks[i].c_str()))){selectedBook=i;break;}
  pushPath=next;return old!=sdBooks;
}
void storagePoll(){
  if(!sdOK||sdOwnedByUsb()||storage_validation::cardEventPending())return;
  if(!storageInitialized||!delivery_store_sd::mounted()){
    delivery_store::SDHooks hooks;hooks.admit=admission;hooks.published=published;delivery_store_sd::setHooks(hooks);
    if(!delivery_store_sd::mount())return;storageInitialized=true;
  }
  // Settled metadata needs no disk I/O while reading/sleeping. A real card
  // change explicitly resets it; source bytes are revalidated when Push opens.
  if(!delivery_store_sd::store().recovered()||running)delivery_store_sd::poll();
  publishLibraryRow();
  if(delivery_store_sd::store().pushRecovered()){
    const auto next=delivery_store_sd::store().pushPath();unsigned rows=0;
    for(const auto& path:sdBooks)rows+=isPushPath(path.c_str());
    const bool membership=next.empty()?rows!=0:rows!=1||sdBooks.empty()||std::strcmp(sdBooks.front().c_str(),next.c_str())!=0;
    // A fresh ordinary SD scan can drop the virtual row while its recovered
    // source still occupies the same fixed slot. Path equality is insufficient.
    if(pushPath!=next||membership)libraryChanged|=refreshLibrary();
  }
}
bool storageBusy(){return storageInitialized&&(!delivery_store_sd::store().recovered()||delivery_store_sd::store().activeState()==State::Checking);}
bool pushReady(){return storageInitialized&&delivery_store_sd::store().pushRecovered();}
void storageStop(){delivery_store_sd::unmount();storageInitialized=false;}
void storageChanged(){delivery_store_sd::invalidate();storageInitialized=false;pushPath.clear();publishedPath.clear();}
bool start(const char* ip,bool live){
  stop();if(!ip||!sdOK||sdOwnedByUsb())return false;
  // A publication/receipt I/O failure may have left a durable intent. Explicit
  // N restart replays bounded recovery against that same final target; it must
  // not strand the live Store in a non-polling Uncertain state indefinitely.
  if(delivery_store_sd::store().activeState()==State::Uncertain){delivery_store_sd::unmount();storageInitialized=false;}
  storagePoll();if(!delivery_store_sd::mounted()){storageInitialized=false;storagePoll();}
  if(!delivery_store_sd::mounted()||!credentials.open()||!registry.load(credentials)||!lan_tls::begin(credentials,request))return false;
  address=ip;epoch=randomHex(16);badWindow=lastUi=millis();badAttempts=0;running=true;credentialsDraining=false;liveMode=live;
  shown={};shownState="Ready";progressBytes=0;clearBatch();
  startMdns();
  promptDirty=true;
  settingsLanType="";settingsLanName="";settingsLanState="Ready";return true;
}
void stop(){
  stopMdns();
  lan_tls::stop();running=false;liveMode=false;clearBatch();credentials.close();
  delivery_store_sd::store().stop();shown={};shownState.clear();
  transferClient.clear();transferRequest.clear();progressBytes=0;credentialsDraining=false;
  settingsLanType="";settingsLanName="";settingsLanState="";
}
bool active(){return running&&lan_tls::active();}
bool promptPending(){return running&&promptVisible();}
const Prompt& prompt(){return shownPrompt;}
bool acceptPrompt(){
  if(!promptPending())return false;
  if(batch.firstContact){
    std::string id,token;
    if(!registry.add(credentials,batch.label,randomBytes,hash,id,token)){
      clearBatch();shownState="Pairing not saved";promptDirty=true;return false;
    }
    batch.clientId=id;batch.issuedClient=id;batch.issuedToken=token;wipe(token);
    for(auto& entry:batch.entries)entry.metadata.clientId=id;
  }
  batch.phase=Phase::Accepted;
  shownPrompt=Prompt{};promptDirty=true;
  shown={};shownState=batch.admitted?"Accepted":"Paired · "+batch.label;
  return true;
}
bool poll(){
  if(!running)return false;
  lan_tls::poll();
  const uint32_t now=millis();
  if(batch.phase==Phase::AwaitingReveal&&uint32_t(now-batch.started)>=lan_batch::REVEAL_MS)clearBatch();
  if(batch.phase==Phase::AwaitingAcceptance&&uint32_t(now-batch.prompted)>=lan_batch::PROMPT_MS){
    // Keep the record so the sender can observe "expired"; never accept.
    batch.phase=Phase::Expired;
    shownPrompt=Prompt{};promptDirty=true;shownState="Expired";
  }
  auto& store=delivery_store_sd::store();
  if(store.activeState()==State::Receiving||store.activeState()==State::Checking){
    // Historical/status requests may change shown metadata inside the handler;
    // the physical panel always describes the actual active transaction.
    const auto current=store.activeMetadata();note(store.status(current.clientId,current.requestId));
    if((store.activeState()==State::Receiving&&uint32_t(millis()-lastProgress)>=TRANSFER_IDLE_MS)||uint32_t(millis()-transferStarted)>=TRANSFER_TOTAL_MS){
      const auto m=store.activeMetadata();note(store.cancel(m.clientId,m.requestId));
    }
    if(store.activeState()==State::Checking)shownState="Checking";
  }else if(store.activeState()==State::Uncertain)shownState="Transfer uncertain";
  // An identical Push body completes without a publication callback. Observe
  // its terminal receipt even when the sender never makes another request.
  if(!shown.requestId.empty()&&(shownState=="Checking"||shownState.find("Receiving ")==0)){
    const auto terminal=store.receipt(shown.clientId,shown.requestId);
    if(terminal.state==State::Complete||terminal.state==State::Cancelled)note(terminal);
  }
  if(!registry.valid()){
    if(!credentialsDraining){
      clearBatch();store.stop();transferClient.clear();transferRequest.clear();
      // The handler's failure response has already been queued by TLS poll.
      // Stop accepting now, then drain/timeout that one response before close.
      lan_tls::stopAfterResponse();credentialsDraining=true;
    }
    if(!lan_tls::active())credentials.close();
    shownState="Credentials unavailable";
  }
  std::string type,name,state=shownState;
  const std::string progress=batchProgress();
  if(!progress.empty()&&progress.find("Done")==0){state=progress;}
  else{
    if(!shown.requestId.empty()){
      type=shown.kind==Kind::Push?"Push · Markdown":
        std::string("File · ")+(document_format::detect(shown.name.c_str())==document_format::Kind::Epub?"EPUB":document_format::detect(shown.name.c_str())==document_format::Kind::Markdown?"Markdown":"TXT");
      name=shown.kind==Kind::Push?shown.title:shown.name;
    }
    if(!progress.empty())type=progress+(type.empty()?"":" · "+type);
  }
  const bool changed=promptDirty||settingsLanType!=type.c_str()||settingsLanName!=name.c_str()||settingsLanState!=state.c_str();
  if(!changed)return false;
  if(!promptDirty&&state.find("Receiving ")==0&&uint32_t(millis()-lastUi)<750)return false;
  promptDirty=false;
  settingsLanType=type.c_str();settingsLanName=name.c_str();settingsLanState=state.c_str();lastUi=millis();return true;
}
bool takeLibraryChanged(){const bool changed=libraryChanged;libraryChanged=false;return changed;}
bool takePushUpdated(){const bool updated=pushUpdated;pushUpdated=false;return updated;}
bool pushKeepsPosition(){return storageInitialized&&delivery_store_sd::store().pushMetadata().metadata.position==1;}

// ---- sender management (AP WebUI; LAN service is stopped in AP mode) -------
namespace {
template<class Operation> RegistryStatus withRegistry(Operation operation){
  if(running)return RegistryStatus::Busy;
  lan_nvs::Store store;lan_auth::Registry local;
  if(!store.open())return RegistryStatus::Unavailable;
  const bool loaded=local.load(store);
  const auto status=operation(store,local,loaded);
  store.close();return status;
}
}
RegistryStatus listClients(std::vector<ClientInfo>& out){
  out.clear();
  return withRegistry([&](lan_nvs::Store&,lan_auth::Registry& local,bool loaded){
    if(!loaded)return RegistryStatus::Unreadable;
    for(size_t slot=0;slot<lan_auth::CAPACITY;++slot){
      std::string id,label;if(local.clientInfo(slot,id,label))out.push_back({id,label});
    }
    return RegistryStatus::Ok;
  });
}
RegistryStatus revokeClient(const std::string& clientId){
  if(!lan_auth::hex(clientId,8))return RegistryStatus::Invalid;
  return withRegistry([&](lan_nvs::Store& store,lan_auth::Registry& local,bool loaded){
    if(!loaded)return RegistryStatus::Unreadable;
    return local.revoke(store,clientId)?RegistryStatus::Ok:RegistryStatus::Unavailable;
  });
}
RegistryStatus pairedCount(size_t& count){
  count=0;
  if(running){if(!registry.valid())return RegistryStatus::Unreadable;count=registry.count();return RegistryStatus::Ok;}
  return withRegistry([&](lan_nvs::Store&,lan_auth::Registry& local,bool loaded){
    if(!loaded)return RegistryStatus::Unreadable;
    count=local.count();return RegistryStatus::Ok;
  });
}
RegistryStatus forgetClients(){
  return withRegistry([&](lan_nvs::Store& store,lan_auth::Registry& local,bool){
    // Explicit repair: replaces even an unreadable record with an empty one.
    return local.reset(store)?RegistryStatus::Ok:RegistryStatus::Unavailable;
  });
}
#ifdef PAPERDIY_DIAGNOSTICS
void printStatus(){
  const auto& store=delivery_store_sd::store();
  Console.printf("LAN_STATUS active=%u live=%u recovered=%u push_revision=%u clients=%u state=%u received=%u batch=%s\n",unsigned(active()),unsigned(liveMode),unsigned(store.recovered()),store.pushMetadata().revision,unsigned(registry.count()),unsigned(store.activeState()),unsigned(store.acceptedBytes()),phaseName());
}
#endif
}
