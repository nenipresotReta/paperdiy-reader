#include "console.h"
#include "lan_service.h"
#include "lan_tls.h"
#include "lan_auth.h"
#include "lan_nvs.h"
#include "delivery_store_sd.h"
#include "managed_progress.h"
#include "storage_validation.h"
#include "reader.h"
#include "sd_reader.h"
#include "ui.h"
#include <SD.h>
#include <mbedtls/sha256.h>
#include <algorithm>
#include <memory>

namespace lan_service {
namespace {
using delivery_store::Code;using delivery_store::State;using delivery_store::Kind;
lan_nvs::Store credentials;
lan_auth::Registry registry;
bool running=false,storageInitialized=false,libraryChanged=false,credentialsDraining=false,pairingDirty=false;
std::string secret,epoch,address,pushPath,publishedPath,transferClient,transferRequest;
delivery_store::Receipt lastPublished;
delivery_store::Metadata shown;
std::string shownState;
uint32_t pairStarted=0,badWindow=0,badAttempts=0,lastUi=0,lastProgress=0,transferStarted=0;
uint32_t progressBytes=0;
constexpr uint32_t PAIR_MS=5u*60*1000,TRANSFER_IDLE_MS=45000,TRANSFER_TOTAL_MS=30u*60*1000;
bool randomBytes(uint8_t* out,size_t length){for(size_t i=0;i<length;i+=4){const auto value=esp_random();const size_t n=std::min(size_t(4),length-i);std::memcpy(out+i,&value,n);}return true;}
bool hash(const uint8_t* data,size_t length,uint8_t out[32]){return mbedtls_sha256_ret(data,length,out,0)==0;}
std::string randomHex(size_t count){uint8_t bytes[32];if(count>sizeof(bytes))return {};randomBytes(bytes,count);return lan_auth::encode(bytes,count);}
std::string escape(const std::string& text){
  std::string out;out.reserve(text.size()+8);
  for(unsigned char c:text){if(c=='"'||c=='\\'){out+='\\';out+=char(c);}else if(c<32){char b[7];snprintf(b,sizeof(b),"\\u%04x",c);out+=b;}else out+=char(c);}return out;
}
std::string quoted(const std::string& text){return "\""+escape(text)+"\"";}
std::string field(const BoundedHttpRequest& request,const char* name){for(const auto& value:request.arguments)if(value.first==name)return value.second;return {};}
std::string header(const BoundedHttpRequest& request,const char* name){for(const auto& value:request.headers)if(value.first==name)return value.second;return {};}
bool number(const std::string& text,uint32_t& out){
  if(text.empty()||text.size()>10||(text.size()>1&&text[0]=='0'))return false;
  uint32_t value=0;for(char c:text){if(c<'0'||c>'9'||value>(UINT32_MAX-uint32_t(c-'0'))/10)return false;value=value*10+uint32_t(c-'0');}out=value;return true;
}
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
bool pairing(const std::string& provided){
  return !secret.empty()&&uint32_t(millis()-pairStarted)<PAIR_MS&&provided.size()==secret.size()&&lan_auth::equal(provided.data(),secret.data(),secret.size());
}
void hidePairing(){if(settingsLanPairing.length()){settingsLanPairing="";pairingDirty=true;}}
void consumeSecret(){std::fill(secret.begin(),secret.end(),'\0');secret.clear();hidePairing();}
bool permittedAttempt(){
  if(uint32_t(millis()-badWindow)>=10000){badWindow=millis();badAttempts=0;}
  return badAttempts<8;
}
bool admission(const delivery_store::Metadata& m){return m.kind==Kind::Push||sdBooks.size()<256;}
void published(const delivery_store::Receipt& receipt,const char* path){lastPublished=receipt;publishedPath=path?path:"";shown=receipt.metadata;shownState="Done";}
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
    hidePairing();
  }else if(result.state==State::Complete){shown=result.receipt.metadata;shownState="Done";}
  else if(result.code!=Code::Ok&&result.code!=Code::Unknown){shownState=result.code==Code::Cancelled?"Cancelled":"Transfer failed";}
}
lan_tls::Response request(const BoundedHttpRequest& incoming){
  if(!running||!registry.valid())return error(503,"credentials_unavailable");
  if(!permittedAttempt())return error(429,"rate_limited");
  auto& store=delivery_store_sd::store();
  if(incoming.path=="/lan/v1/pair"&&incoming.post){
    if(!pairing(field(incoming,"secret"))){++badAttempts;return error(401,"pairing_rejected");}
    if(registry.count()>=lan_auth::CAPACITY)return error(409,"client_limit");
    if(!store.pushRecovered())return error(503,"recovering");
    const auto label=field(incoming,"label");std::string id,token;
    if(!registry.add(credentials,label,randomBytes,hash,id,token)){
      if(!registry.valid())consumeSecret();return error(500,"pairing_not_saved");
    }
    consumeSecret();
    return {200,"{\"deviceId\":"+quoted(lan_tls::deviceId())+",\"clientId\":"+quoted(id)+",\"token\":"+quoted(token)+
      ",\"pushRevision\":"+std::to_string(store.pushMetadata().revision)+"}"};
  }
  if(incoming.path=="/lan/v1/clients"&&incoming.post){
    if(!pairing(field(incoming,"secret"))){++badAttempts;return error(401,"pairing_rejected");}
    std::string body="{\"deviceId\":"+quoted(lan_tls::deviceId())+",\"clients\":[";bool first=true;
    for(size_t slot=0;slot<lan_auth::CAPACITY;++slot){
      std::string id,label;if(!registry.clientInfo(slot,id,label))continue;
      if(!first)body+=',';first=false;
      body+="{\"clientId\":"+quoted(id)+",\"label\":"+quoted(label)+"}";
    }
    // Listing is read-only and does not consume the physical pairing window.
    return {200,body+"]}"};
  }
  std::string client;const bool authenticated=registry.authenticate(header(incoming,"authorization"),hash,client);
  if(incoming.path=="/lan/v1/clients/revoke"&&incoming.post){
    const auto target=field(incoming,"clientId");const bool physical=pairing(field(incoming,"secret"));
    if((!authenticated||client!=target)&&!physical){++badAttempts;return error(401,"unauthorized");}
    if(!lan_auth::hex(target,8))return error(400,"invalid_client");
    if(!registry.revoke(credentials,target)){if(!registry.valid())consumeSecret();return error(500,"revocation_uncertain");}
    if(physical)consumeSecret();return {200,"{\"revoked\":true}"};
  }
  if(!authenticated){++badAttempts;return error(401,"unauthorized");}
  if(incoming.path=="/lan/v1/capabilities"&&!incoming.post){
    if(!store.pushRecovered())return error(503,"recovering");
    return {200,"{\"v\":1,\"deviceId\":"+quoted(lan_tls::deviceId())+",\"pushRevision\":"+std::to_string(store.pushMetadata().revision)+
      ",\"epoch\":"+quoted(epoch)+",\"receiptCapacity\":16,\"receiptScope\":\"client\",\"limits\":{\"chunkBytes\":4096,\"pushBytes\":16384,\"txtBytes\":4194304,\"epubBytes\":67108864,\"markdownBytes\":1048576}}"};
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
bool start(const char* ip){
  stop();if(!ip||!sdOK||sdOwnedByUsb())return false;
  // A publication/receipt I/O failure may have left a durable intent. Explicit
  // N restart replays bounded recovery against that same final target; it must
  // not strand the live Store in a non-polling Uncertain state indefinitely.
  if(delivery_store_sd::store().activeState()==State::Uncertain){delivery_store_sd::unmount();storageInitialized=false;}
  storagePoll();if(!delivery_store_sd::mounted()){storageInitialized=false;storagePoll();}
  if(!delivery_store_sd::mounted()||!credentials.open()||!registry.load(credentials)||!lan_tls::begin(credentials,request))return false;
  address=ip;secret=randomHex(16);epoch=randomHex(16);pairStarted=badWindow=lastUi=millis();badAttempts=0;running=true;credentialsDraining=false;
  shown={};shownState="Ready";progressBytes=0;
  settingsLanPairing=("{\"v\":1,\"deviceId\":"+quoted(lan_tls::deviceId())+",\"host\":"+quoted(address)+",\"port\":8443,\"certSha256\":"+quoted(lan_tls::fingerprint())+",\"secret\":"+quoted(secret)+"}").c_str();
  pairingDirty=true;
  settingsLanType="";settingsLanName="";settingsLanState="Ready";return true;
}
void stop(){
  lan_tls::stop();running=false;consumeSecret();credentials.close();
  delivery_store_sd::store().stop();shown={};shownState.clear();
  transferClient.clear();transferRequest.clear();progressBytes=0;credentialsDraining=false;
  settingsLanType="";settingsLanName="";settingsLanState="";
}
bool active(){return running&&lan_tls::active();}
bool poll(){
  if(!running)return false;
  lan_tls::poll();
  if(!secret.empty()&&uint32_t(millis()-pairStarted)>=PAIR_MS)consumeSecret();
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
      consumeSecret();store.stop();transferClient.clear();transferRequest.clear();
      // The handler's failure response has already been queued by TLS poll.
      // Stop accepting now, then drain/timeout that one response before close.
      lan_tls::stopAfterResponse();credentialsDraining=true;
    }
    if(!lan_tls::active())credentials.close();
    shownState="Credentials unavailable";
  }
  const auto kind=shown.requestId.empty()?std::string():shown.kind==Kind::Push?"Push · Markdown":
    std::string("File · ")+(document_format::detect(shown.name.c_str())==document_format::Kind::Epub?"EPUB":document_format::detect(shown.name.c_str())==document_format::Kind::Markdown?"Markdown":"TXT");
  const auto name=shown.kind==Kind::Push?shown.title:shown.name;
  const bool changed=pairingDirty||settingsLanType!=kind.c_str()||settingsLanName!=name.c_str()||settingsLanState!=shownState.c_str();
  if(!changed)return false;
  if(!pairingDirty&&shownState.find("Receiving ")==0&&uint32_t(millis()-lastUi)<750)return false;
  pairingDirty=false;
  settingsLanType=kind.c_str();settingsLanName=name.c_str();settingsLanState=shownState.c_str();lastUi=millis();return true;
}
bool takeLibraryChanged(){const bool changed=libraryChanged;libraryChanged=false;return changed;}
#ifdef PAPERDIY_DIAGNOSTICS
void printStatus(bool pairingPayload){
  const auto& store=delivery_store_sd::store();
  Console.printf("LAN_STATUS active=%u recovered=%u push_revision=%u clients=%u state=%u received=%u\n",unsigned(active()),unsigned(store.recovered()),store.pushMetadata().revision,unsigned(registry.count()),unsigned(store.activeState()),store.acceptedBytes());
  // Private diagnostic builds only. Production has no serial pairing export.
  if(pairingPayload&&active()&&settingsLanPairing.length())Console.printf("LAN_PAIRING %s\n",settingsLanPairing.c_str());
}
#endif
}
