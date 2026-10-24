#include <Arduino.h>
#include <Preferences.h>
#include <nvs.h>
#include <openssl/sha.h>
#include "lan_service.h"
#include "lan_tls.h"
#include "lan_auth.h"
#include "delivery_store_sd.h"
#include "storage_validation.h"
#include "sd_reader.h"
#include "ui.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <memory>
#include <vector>

bool sdOK=true;
std::vector<String> sdBooks;
size_t selectedBook=0;
String sdError,settingsLanType,settingsLanName,settingsLanState,settingsLanPairing;
namespace {
using delivery_store::Code;using delivery_store::State;using delivery_store::Kind;
bool usbOwned=false,enabled=false,tlsLive=false,tlsWriting=false,tlsDraining=false,tlsAccepting=false;
uint64_t lease=0;
lan_tls::Handler captured=nullptr;
delivery_store::SDHooks hooks;
std::unique_ptr<delivery_store::Store> instance;
std::map<std::string,std::vector<uint8_t>> files,nvsCommitted,nvsPending;
unsigned reads=0,writes=0,polls=0;
int nvsGetError=ESP_OK,nvsCommitError=ESP_OK,nvsOpenError=ESP_OK;
unsigned nvsOpenHandles=0;
bool failReceiptWrite=false;
bool failReadbackAfterCommit=false;
SHA256_CTX streaming;
bool accessible(){return enabled&&sdOK&&!usbOwned&&lease&&lease==storage_validation::lease();}
delivery_store::Hash hashAdapter(){delivery_store::Hash h;
  h.begin=[](void*){return SHA256_Init(&streaming)==1;};
  h.update=[](void*,const uint8_t* b,size_t n){return SHA256_Update(&streaming,b,n)==1;};
  h.finish=[](void*,uint8_t out[32]){return SHA256_Final(out,&streaming)==1;};
  h.digest=[](void*,const uint8_t* b,size_t n,uint8_t out[32]){return SHA256(b,n,out)!=nullptr;};return h;}
delivery_store::IO ioAdapter(){delivery_store::IO io;
  io.stat=[](void*,const char* path,uint32_t* size){assert(accessible());++reads;auto it=files.find(path);if(it==files.end())return delivery_store::Entry::Missing;*size=it->second.size();return delivery_store::Entry::File;};
  io.read=[](void*,const char* path,uint32_t at,uint8_t* out,size_t n){assert(accessible());++reads;auto it=files.find(path);if(it==files.end()||uint64_t(at)+n>it->second.size())return false;if(n)std::memcpy(out,it->second.data()+at,n);return true;};
  io.write=[](void*,const char* path,uint32_t at,const uint8_t* data,size_t n,bool truncate){assert(accessible());++writes;if(failReceiptWrite&&n==1024&&data&&data[4]==1)return false;auto& file=files[path];if(truncate)file.clear();file.resize(std::max(file.size(),size_t(at)+n));if(n)std::memcpy(file.data()+at,data,n);return true;};
  io.rename=[](void*,const char* from,const char* to,bool replace){assert(accessible());++writes;assert(std::string(from).find("/.paperdiy/lan/")==0);if(replace)assert(std::string(to).find("/.paperdiy/lan/")==0);if(!files.count(from)||(!replace&&files.count(to)))return false;files[to]=std::move(files[from]);files.erase(from);return true;};
  io.remove=[](void*,const char* path){assert(accessible());++writes;assert(std::string(path).find("/.paperdiy/lan/")==0);files.erase(path);return true;};
  io.sync=[](void*,const char*){assert(accessible());++writes;return true;};
  io.ready=[](void*){return accessible();};
  io.admit=[](void*,const delivery_store::Metadata& m){return !hooks.admit||hooks.admit(m);};
  io.validatePush=[](void*,const uint8_t* b,size_t n){return n&&b[0]!='!';};
  io.published=[](void*,const delivery_store::Receipt& r,const char* path){if(hooks.published)hooks.published(r,path);};return io;
}
std::string text(const String& value){return value.c_str();}
std::string jsonString(const std::string& json,const char* name){
  const std::string key=std::string("\"")+name+"\":\"";const auto begin=json.find(key);assert(begin!=std::string::npos);const auto start=begin+key.size();const auto end=json.find('"',start);assert(end!=std::string::npos);return json.substr(start,end-start);
}
std::string formEncode(const std::string& value){
  constexpr char digits[]="0123456789ABCDEF";std::string out;
  for(unsigned char c:value){if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.')out+=char(c);else{out+='%';out+=digits[c>>4];out+=digits[c&15];}}return out;
}
using Fields=std::vector<BoundedHttpRequest::Field>;
lan_tls::Response request(const char* path,const std::string& auth="",bool post=false,const Fields& fields={}){
  assert(captured);std::string encoded;
  for(const auto& field:fields){if(!encoded.empty())encoded+='&';encoded+=formEncode(field.first)+"="+formEncode(field.second);}
  std::string target=path;if(!post&&!encoded.empty())target+="?"+encoded;
  std::string wire=std::string(post?"POST ":"GET ")+target+" HTTP/1.1\r\nHost: 127.0.0.1\r\n";
  if(!auth.empty())wire+="Authorization: "+auth+"\r\n";
  if(post)wire+="Content-Type: application/x-www-form-urlencoded\r\nContent-Length: "+std::to_string(encoded.size())+"\r\n";
  wire+="\r\n";if(post)wire+=encoded;
  BoundedHttpRequest incoming;for(char byte:wire)incoming.feed(byte);
  assert(incoming.state==BoundedHttpRequest::State::Ready);
  auto response=captured(incoming);if(tlsLive)tlsWriting=true;return response;
}
void tick(uint32_t elapsed=0){testClock+=elapsed;lan_service::storagePoll();lan_service::poll();}
void settle(){for(unsigned i=0;i<100&&!delivery_store_sd::store().recovered();++i)tick();assert(delivery_store_sd::store().recovered());}
void reset(){
  lan_service::stop();lan_service::storageChanged();instance.reset();files.clear();nvsCommitted.clear();nvsPending.clear();
  usbOwned=false;enabled=false;reads=writes=polls=0;nvsGetError=nvsCommitError=nvsOpenError=ESP_OK;
  assert(nvsOpenHandles==0);failReceiptWrite=failReadbackAfterCommit=false;
  testClock=100;sdOK=true;sdBooks.clear();selectedBook=0;storage_validation::beginExclusive();
  lan_service::takeLibraryChanged();
  assert(lan_service::start("127.0.0.1"));settle();assert(lan_service::active());
}
struct Client {std::string id,token;std::string auth()const{return "Bearer "+id+":"+token;}};
Client pair(const std::string& label="Synthetic client"){
  const auto secret=jsonString(text(settingsLanPairing),"secret");
  const auto response=request("/lan/v1/pair","",true,{{"secret",secret},{"label",label}});
  assert(response.code==200&&settingsLanPairing.length()==0);
  return {jsonString(response.json,"clientId"),jsonString(response.json,"token")};
}
delivery_store::Metadata metadata(const Client& client,unsigned n,const std::string& body,Kind kind=Kind::Push,uint32_t base=0){
  delivery_store::Metadata m;m.clientId=client.id;m.requestId=std::string(24,'0')+delivery_store::hexadecimal(reinterpret_cast<const uint8_t*>(&n),4);
  m.kind=kind;m.name=kind==Kind::Push?"Push.md":"synthetic.md";m.title="Synthetic title";m.size=body.size();m.baseRevision=base;
  uint8_t digest[32];SHA256(reinterpret_cast<const uint8_t*>(body.data()),body.size(),digest);m.sha256=delivery_store::hexadecimal(digest,32);m.fingerprint=delivery_store::fingerprint(m,hashAdapter());return m;
}
lan_tls::Response begin(const Client& client,const delivery_store::Metadata& m){return request("/lan/v1/uploads/start",client.auth(),true,{{"requestId",m.requestId},{"fingerprint",m.fingerprint},{"kind",m.kind==Kind::Push?"push":"file"},{"name",m.name},{"title",m.title},{"size",std::to_string(m.size)},{"sha256",m.sha256},{"baseRevision",m.kind==Kind::Push?std::to_string(m.baseRevision):""}});}
std::string base64(const std::string& body){
  constexpr char digits[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";std::string out;
  for(size_t i=0;i<body.size();i+=3){const size_t n=std::min(size_t(3),body.size()-i);const uint32_t v=uint32_t(uint8_t(body[i]))<<16|(n>1?uint32_t(uint8_t(body[i+1]))<<8:0)|(n>2?uint8_t(body[i+2]):0);out+=digits[v>>18];out+=digits[(v>>12)&63];out+=n>1?digits[(v>>6)&63]:'=';out+=n>2?digits[v&63]:'=';}return out;
}
lan_tls::Response chunk(const Client& client,const delivery_store::Metadata& m,size_t at,const std::string& body){return request("/lan/v1/uploads/chunk",client.auth(),true,{{"requestId",m.requestId},{"offset",std::to_string(at)},{"data",base64(body)}});}
lan_tls::Response status(const Client& client,const delivery_store::Metadata& m){return request("/lan/v1/uploads/status",client.auth(),false,{{"requestId",m.requestId}});}
void deliver(const Client& client,const delivery_store::Metadata& m,const std::string& body){
  assert(begin(client,m).code==200);
  for(size_t at=0;at<body.size();at+=4096)assert(chunk(client,m,at,body.substr(at,4096)).code==200);
  const auto result=request("/lan/v1/uploads/commit",client.auth(),true,{{"requestId",m.requestId}});
  assert(result.code==200&&jsonString(result.json,"state")=="checking");
  for(unsigned i=0;i<100&&delivery_store_sd::store().activeState()==State::Checking;++i)tick();
  assert(delivery_store_sd::store().receipt(client.id,m.requestId).state==State::Complete);
}
}

bool sdOwnedByUsb(){return usbOwned;}
namespace delivery_store_sd {
delivery_store::Store& store(){if(!instance)instance.reset(new delivery_store::Store(ioAdapter(),hashAdapter()));return *instance;}
void setHooks(delivery_store::SDHooks value){hooks=value;}
bool mount(){lease=storage_validation::lease();enabled=sdOK&&!usbOwned&&lease;store().beginRecovery();return enabled;}
void unmount(){store().stop();enabled=false;lease=0;}
bool mounted(){return accessible();}
void poll(){if(enabled)store().poll();}
void invalidate(){enabled=false;lease=0;store().stop();}
}
namespace lan_tls {
bool begin(lan_nvs::Store& credentials,Handler handler){assert(credentials.ready());captured=handler;tlsLive=tlsAccepting=true;tlsWriting=tlsDraining=false;return true;}
void stop(){tlsLive=tlsAccepting=tlsWriting=tlsDraining=false;}
void stopAfterResponse(){tlsDraining=true;tlsAccepting=false;}
void poll(){++polls;if(tlsDraining){tlsWriting=false;tlsLive=false;}}
bool active(){return tlsLive;}
const char* fingerprint(){return "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";}
const char* deviceId(){return "synthetic-service";}
}
esp_err_t nvs_open(const char* name,int mode,nvs_handle_t* handle){assert(!std::strcmp(name,"paperlan")&&mode==NVS_READWRITE);if(nvsOpenError)return nvsOpenError;*handle=17;++nvsOpenHandles;return ESP_OK;}
void nvs_close(nvs_handle_t handle){assert(handle==17&&nvsOpenHandles);--nvsOpenHandles;nvsPending.clear();}
esp_err_t nvs_get_blob(nvs_handle_t handle,const char* key,void* out,size_t* length){assert(handle==17);if(nvsGetError)return nvsGetError;auto found=nvsCommitted.find(key);if(found==nvsCommitted.end())return ESP_ERR_NVS_NOT_FOUND;if(out){if(*length<found->second.size())return ESP_FAIL;std::memcpy(out,found->second.data(),found->second.size());}*length=found->second.size();return ESP_OK;}
esp_err_t nvs_set_blob(nvs_handle_t handle,const char* key,const void* bytes,size_t length){assert(handle==17&&!std::strcmp(key,"lanAuthV1"));const auto* p=static_cast<const uint8_t*>(bytes);nvsPending[key]={p,p+length};return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t handle){assert(handle==17);if(nvsCommitError)return nvsCommitError;for(const auto& entry:nvsPending)nvsCommitted[entry.first]=entry.second;nvsPending.clear();if(failReadbackAfterCommit)nvsGetError=ESP_FAIL;return ESP_OK;}

namespace {
void pairingAuthenticationAndRevoke(){
  reset();assert(request("/lan/v1/capabilities").code==401);
  const auto secret=jsonString(text(settingsLanPairing),"secret");const Client client=pair();
  assert(request("/lan/v1/pair","",true,{{"secret",secret},{"label","Replay"}}).code==401);
  assert(request("/lan/v1/capabilities",client.auth()).code==200);
  assert(request("/lan/v1/capabilities","Bearer "+client.token).code==401);
  const auto& bytes=nvsCommitted.at("lanAuthV1");uint8_t token[32];assert(lan_auth::decode(client.token,token,32));
  assert(std::search(bytes.begin(),bytes.end(),token,token+32)==bytes.end());
  assert(request("/lan/v1/clients/revoke",client.auth(),true,{{"clientId",client.id}}).code==200);
  assert(request("/lan/v1/capabilities",client.auth()).code==401);
  lan_service::stop();assert(!lan_service::active());assert(request("/lan/v1/capabilities",client.auth()).code==503);
}
void rateAndCredentialFailure(){
  reset();const Client client=pair();
  for(unsigned i=0;i<8;++i)assert(request("/lan/v1/capabilities","invalid").code==401);
  assert(request("/lan/v1/capabilities",client.auth()).code==429);tick(10000);
  assert(request("/lan/v1/capabilities",client.auth()).code==200);
  lan_service::stop();nvsGetError=ESP_ERR_NVS_TYPE_MISMATCH;assert(!lan_service::start("127.0.0.1"));
  assert(!lan_service::active());assert(request("/lan/v1/capabilities",client.auth()).code==503);
  nvsGetError=ESP_OK;assert(lan_service::start("127.0.0.1"));settle();
  assert(request("/lan/v1/capabilities",client.auth()).code==200);
  const auto secret=jsonString(text(settingsLanPairing),"secret");nvsCommitError=ESP_FAIL;
  const auto response=request("/lan/v1/pair","",true,{{"secret",secret},{"label","Commit failure"}});
  assert(response.code==500&&text(settingsLanPairing).empty());
  assert(tlsWriting);
  tick();assert(tlsDraining&&!tlsAccepting&&tlsLive&&tlsWriting);
  assert(text(settingsLanState)=="Credentials unavailable");
  tick();assert(!lan_service::active()&&!tlsLive&&nvsOpenHandles==0);
  assert(request("/lan/v1/capabilities",client.auth()).code==503);nvsCommitError=ESP_OK;
}
void physicalInventoryAndOrphanRecovery(){
  reset();std::vector<Client> clients;
  for(unsigned i=0;i<4;++i){
    clients.push_back(pair("Synthetic orphan "+std::to_string(i)));
    lan_service::stop();assert(lan_service::start("127.0.0.1"));settle();
  }
  const auto secret=jsonString(text(settingsLanPairing),"secret");
  // An ordinary client cannot enumerate other identities without the current
  // physically displayed secret, including when all four slots are occupied.
  assert(request("/lan/v1/clients",clients.front().auth(),true).code==401);
  const auto before=nvsCommitted;
  const auto inventory=request("/lan/v1/clients","",true,{{"secret",secret}});
  assert(inventory.code==200&&jsonString(inventory.json,"deviceId")=="synthetic-service");
  assert(inventory.json.find("token")==std::string::npos&&inventory.json.find("verifier")==std::string::npos);
  for(unsigned i=0;i<clients.size();++i){
    assert(inventory.json.find(clients[i].id)!=std::string::npos);
    assert(inventory.json.find("Synthetic orphan "+std::to_string(i))!=std::string::npos);
    assert(inventory.json.find(clients[i].token)==std::string::npos);
  }
  assert(nvsCommitted==before&&jsonString(text(settingsLanPairing),"secret")==secret);
  assert(request("/lan/v1/clients","",true,{{"secret",secret}}).code==200);
  assert(request("/lan/v1/pair","",true,{{"secret",secret},{"label","Overflow"}}).code==409);
  assert(request("/lan/v1/clients/revoke","",true,{{"secret",secret},{"clientId",clients[1].id}}).code==200);
  assert(text(settingsLanPairing).empty());
  assert(request("/lan/v1/capabilities",clients[1].auth()).code==401);
  assert(request("/lan/v1/capabilities",clients[0].auth()).code==200);
  lan_service::stop();assert(lan_service::start("127.0.0.1"));settle();
  const auto nextSecret=jsonString(text(settingsLanPairing),"secret");
  const auto next=request("/lan/v1/clients","",true,{{"secret",nextSecret}});
  assert(next.code==200&&next.json.find(clients[1].id)==std::string::npos);
  assert(!pair("Recovered slot").id.empty());
  // Invalid inventory secrets share the same bounded rate window as pairing.
  reset();const auto valid=jsonString(text(settingsLanPairing),"secret");
  for(unsigned i=0;i<8;++i)assert(request("/lan/v1/clients","",true,{{"secret","invalid"}}).code==401);
  assert(request("/lan/v1/clients","",true,{{"secret",valid}}).code==429);tick(10000);
  assert(request("/lan/v1/clients","",true,{{"secret",valid}}).code==200);
  tick(5u*60u*1000u);assert(request("/lan/v1/clients","",true,{{"secret",valid}}).code==401);
}
void savedPairWithFailedReadbackRemainsRecoverable(){
  reset();const auto secret=jsonString(text(settingsLanPairing),"secret");failReadbackAfterCommit=true;
  const auto response=request("/lan/v1/pair","",true,{{"secret",secret},{"label","Readback orphan"}});
  assert(response.code==500&&response.json.find("pairing_not_saved")!=std::string::npos);
  assert(nvsCommitted.count("lanAuthV1"));tick();tick();assert(!lan_service::active());
  failReadbackAfterCommit=false;nvsGetError=ESP_OK;
  assert(lan_service::start("127.0.0.1"));settle();
  const auto fresh=jsonString(text(settingsLanPairing),"secret");
  const auto inventory=request("/lan/v1/clients","",true,{{"secret",fresh}});
  assert(inventory.code==200&&inventory.json.find("Readback orphan")!=std::string::npos);
  const auto orphan=jsonString(inventory.json,"clientId");assert(lan_auth::hex(orphan,8));
  assert(request("/lan/v1/clients/revoke","",true,{{"secret",fresh},{"clientId",orphan}}).code==200);
}
void pushFileReceiptScopeAndUi(){
  reset();const Client first=pair();
  const auto push=metadata(first,1,"Synthetic Push");deliver(first,push,"Synthetic Push");tick(1000);
  assert(text(settingsLanType)=="Push · Markdown"&&text(settingsLanName)=="Synthetic title"&&text(settingsLanState)=="Done");
  assert(sdBooks.size()==1&&lan_service::isPushPath(sdBooks.front().c_str()));
  const auto file=metadata(first,2,"Synthetic ordinary file",Kind::File);deliver(first,file,"Synthetic ordinary file");tick(1000);
  assert(text(settingsLanType)=="File · Markdown"&&text(settingsLanName)=="synthetic.md"&&text(settingsLanState)=="Done");
  assert(sdBooks.size()==2&&selectedBook==0&&lan_service::isPushPath(sdBooks.front().c_str()));
  assert(lan_service::takeLibraryChanged());
  const auto replay=request("/lan/v1/uploads/commit",first.auth(),true,{{"requestId",file.requestId}});assert(replay.code==200&&jsonString(replay.json,"state")=="complete");
  lan_service::stop();assert(lan_service::start("127.0.0.1"));settle();const Client second=pair("Second synthetic client");
  assert(request("/lan/v1/receipts",second.auth(),false,{{"requestId",file.requestId}}).code==404);
  assert(request("/lan/v1/uploads/cancel",second.auth(),true,{{"requestId",file.requestId}}).code==404);
  assert(files.count("/Books/synthetic.md"));
  assert(request("/lan/v1/receipts",first.auth(),false,{{"requestId",file.requestId}}).code==200);
}
void stopAndStorageOwnership(){
  reset();const Client client=pair();const auto m=metadata(client,3,"An unfinished synthetic file",Kind::File);
  assert(begin(client,m).code==200);assert(chunk(client,m,0,"An unfinished").code==200);
  lan_service::stop();assert(!files.count("/Books/synthetic.md"));assert(!files.count("/.paperdiy/lan/upload.bin"));
  assert(!lan_service::active()&&text(settingsLanPairing).empty());
  assert(lan_service::start("127.0.0.1"));settle();
  const auto second=metadata(client,4,"Card loss");assert(begin(client,second).code==200);
  const auto beforeReads=reads,beforeWrites=writes;usbOwned=true;
  assert(chunk(client,second,0,"Card loss").code!=200);tick();
  assert(reads==beforeReads&&writes==beforeWrites);usbOwned=false;
  lan_service::storageChanged();storage_validation::invalidate();
  const auto r=reads,w=writes;tick();assert(reads==r&&writes==w);
  lan_service::stop();assert(reads==r&&writes==w);
}
void malformedBase64AndProgressLease(){
  reset();const Client client=pair();const auto m=metadata(client,5,"x");assert(begin(client,m).code==200);
  for(const auto& data:{"eA=", "eB==", "eA==AAAA", "====", "eA==\n"})assert(request("/lan/v1/uploads/chunk",client.auth(),true,{{"requestId",m.requestId},{"offset","0"},{"data",data}}).code==400);
  assert(delivery_store_sd::store().acceptedBytes()==0);
  tick(44000);assert(status(client,m).code==200);tick(1001);
  assert(delivery_store_sd::store().status(client.id,m.requestId).state==State::Cancelled);
}
void statusTrafficCannotResetLeaseUsingOldReceipt(){
  reset();const Client client=pair();const auto old=metadata(client,6,"A complete old file",Kind::File);deliver(client,old,"A complete old file");
  const auto pending=metadata(client,7,"A pending new file",Kind::File);assert(begin(client,pending).code==200);
  tick(30000);
  assert(request("/lan/v1/receipts",client.auth(),false,{{"requestId",old.requestId}}).code==200);
  assert(status(client,pending).code==200);
  tick(16000);
  assert(delivery_store_sd::store().status(client.id,pending.requestId).state==State::Cancelled);
}
void sameBodyNoopGetsTerminalUiWithoutClientPolling(){
  reset();const Client client=pair();const std::string body="Same synthetic Push";auto original=metadata(client,8,body);deliver(client,original,body);
  auto next=metadata(client,9,body,Kind::Push,1);deliver(client,next,body);tick(1000);
  assert(delivery_store_sd::store().pushMetadata().revision==1);
  assert(text(settingsLanState)=="Done");
}
void sameSlotRowReturnsAfterOrdinaryScanAndRemount(){
  reset();const Client client=pair();const auto m=metadata(client,10,"Stable synthetic Push");deliver(client,m,"Stable synthetic Push");
  const std::string path=lan_service::currentPushPath();assert(!path.empty());
  sdBooks={String("/Books/existing.md")};selectedBook=0;
  lan_service::storagePoll();
  assert(sdBooks.size()==2&&text(sdBooks.front())==path&&selectedBook==1);
  lan_service::stop();lan_service::storageStop();
  sdBooks={String("/Books/existing.md")};selectedBook=0;storage_validation::beginExclusive();
  lan_service::storagePoll();settle();
  assert(sdBooks.size()==2&&text(sdBooks.front())==path&&selectedBook==1);
  sdBooks={String("/Books/existing.md")};selectedBook=0;storage_validation::beginExclusive();
  lan_service::storagePoll();settle();
  assert(sdBooks.size()==2&&text(sdBooks.front())==path&&selectedBook==1);
}
void pairingQrDirtyOnSuccessAndExpiry(){
  reset();assert(!lan_service::poll());pair();
  assert(lan_service::poll());assert(text(settingsLanPairing).empty());assert(!lan_service::poll());
  reset();assert(!lan_service::poll());testClock+=5u*60u*1000u;
  assert(lan_service::poll());assert(text(settingsLanPairing).empty());assert(!lan_service::poll());
}
void nRestartReconcilesExistingPublicationIntent(){
  reset();const Client client=pair();const std::string body="Published before receipt failure";const auto m=metadata(client,11,body,Kind::File);
  assert(begin(client,m).code==200&&chunk(client,m,0,body).code==200);
  failReceiptWrite=true;assert(request("/lan/v1/uploads/commit",client.auth(),true,{{"requestId",m.requestId}}).code==200);
  for(unsigned i=0;i<100&&delivery_store_sd::store().activeState()==State::Checking;++i)tick();
  assert(delivery_store_sd::store().activeState()==State::Uncertain&&files.count("/Books/synthetic.md"));
  failReceiptWrite=false;lan_service::stop();assert(lan_service::start("127.0.0.1"));settle();
  assert(delivery_store_sd::store().receipt(client.id,m.requestId).state==State::Complete);
  assert(request("/lan/v1/receipts",client.auth(),false,{{"requestId",m.requestId}}).code==200);
  unsigned books=0;for(const auto& file:files)books+=file.first.find("/Books/")==0;assert(books==1);
}
void totalLeaseBoundsProgressAndChecking(){
  reset();const Client client=pair();const auto m=metadata(client,12,std::string(100,'x'),Kind::File);
  assert(begin(client,m).code==200);
  for(unsigned i=0;i<45;++i){assert(chunk(client,m,i,"x").code==200);tick(40000);}
  assert(delivery_store_sd::store().status(client.id,m.requestId).state==State::Cancelled);
  reset();const Client second=pair();const std::string body="Checking synthetic file";const auto check=metadata(second,13,body,Kind::File);
  assert(begin(second,check).code==200&&chunk(second,check,0,body).code==200);
  assert(request("/lan/v1/uploads/commit",second.auth(),true,{{"requestId",check.requestId}}).code==200);
  testClock+=30u*60u*1000u;lan_service::poll(); // deliberately do not advance store checker
  assert(delivery_store_sd::store().status(second.id,check.requestId).state==State::Cancelled);
  assert(!files.count("/Books/synthetic.md"));
}
}
int main(){pairingAuthenticationAndRevoke();rateAndCredentialFailure();physicalInventoryAndOrphanRecovery();savedPairWithFailedReadbackRemainsRecoverable();pushFileReceiptScopeAndUi();stopAndStorageOwnership();malformedBase64AndProgressLease();statusTrafficCannotResetLeaseUsingOldReceipt();sameBodyNoopGetsTerminalUiWithoutClientPolling();sameSlotRowReturnsAfterOrdinaryScanAndRemount();pairingQrDirtyOnSuccessAndExpiry();nRestartReconcilesExistingPublicationIntent();totalLeaseBoundsProgressAndChecking();lan_service::stop();assert(!nvsOpenHandles);std::puts("LAN service integration tests passed");}
