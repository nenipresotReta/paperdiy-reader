#include <Arduino.h>
#include <Preferences.h>
#include <nvs.h>
#include <openssl/sha.h>
#include "lan_service.h"
#include "lan_tls.h"
#include "lan_auth.h"
#include "lan_batch.h"
#include <ESPmDNS.h>
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
String sdError,settingsLanType,settingsLanName,settingsLanState;
namespace {
using delivery_store::Code;using delivery_store::State;using delivery_store::Kind;
bool enabled=false,tlsLive=false,tlsWriting=false,tlsDraining=false,tlsAccepting=false;
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
bool accessible(){return enabled&&sdOK&&lease&&lease==storage_validation::lease();}
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
  enabled=false;reads=writes=polls=0;nvsGetError=nvsCommitError=nvsOpenError=ESP_OK;
  assert(nvsOpenHandles==0);failReceiptWrite=failReadbackAfterCommit=false;
  testClock=100;sdOK=true;sdBooks.clear();selectedBook=0;storage_validation::beginExclusive();
  lan_service::takeLibraryChanged();
  assert(lan_service::start("127.0.0.1"));settle();assert(lan_service::active());
}
struct Client {std::string id,token;std::string auth()const{return "Bearer "+id+":"+token;}};
bool testDigest(const uint8_t* bytes,size_t length,uint8_t out[32]){return SHA256(bytes,length,out)!=nullptr;}
// First contact as a sender performs it: commit to Ns, learn Nd, reveal Ns.
struct Contact {lan_tls::Response offer,reveal;std::string batchId,pairingId;uint8_t nonce[32];};
Contact contact(const std::string& label,const Fields& entries={},uint8_t seed=7,bool doReveal=true){
  Contact c;for(unsigned i=0;i<32;++i)c.nonce[i]=uint8_t(seed+i);
  std::string commit;assert(lan_batch::commitment(testDigest,c.nonce,commit));
  Fields fields={{"label",label},{"commit",commit}};fields.insert(fields.end(),entries.begin(),entries.end());
  c.offer=request("/lan/v2/batch/offer","",true,fields);
  if(c.offer.code!=200||c.offer.json.find("\"pairingId\"")==std::string::npos)return c;
  c.batchId=jsonString(c.offer.json,"batchId");c.pairingId=jsonString(c.offer.json,"pairingId");
  if(doReveal)c.reveal=request("/lan/v2/pair/reveal","",true,{{"pairingId",c.pairingId},{"senderNonce",delivery_store::hexadecimal(c.nonce,32)}});
  return c;
}
std::string proof(const Contact& c){std::string value;assert(lan_batch::statusProof(testDigest,c.nonce,c.batchId,value));return value;}
lan_tls::Response contactStatus(const Contact& c){return request("/lan/v2/batch/status","",false,{{"batchId",c.batchId},{"proof",proof(c)}});}
Client pair(const std::string& label="Synthetic client"){
  const auto c=contact(label);
  assert(c.offer.code==200&&jsonString(c.offer.json,"state")=="awaiting_reveal");
  assert(c.reveal.code==200&&lan_service::promptPending()&&lan_service::prompt().firstContact);
  assert(lan_service::acceptPrompt()&&!lan_service::promptPending());
  const auto status=contactStatus(c);assert(status.code==200&&jsonString(status.json,"state")=="accepted");
  return {jsonString(status.json,"clientId"),jsonString(status.json,"token")};
}
delivery_store::Metadata metadata(const Client& client,unsigned n,const std::string& body,Kind kind=Kind::Push,uint32_t base=0){
  delivery_store::Metadata m;m.clientId=client.id;m.requestId=std::string(24,'0')+delivery_store::hexadecimal(reinterpret_cast<const uint8_t*>(&n),4);
  m.kind=kind;m.name=kind==Kind::Push?"Push.md":"synthetic.md";m.title="Synthetic title";m.size=body.size();m.baseRevision=base;
  uint8_t digest[32];SHA256(reinterpret_cast<const uint8_t*>(body.data()),body.size(),digest);m.sha256=delivery_store::hexadecimal(digest,32);m.fingerprint=delivery_store::fingerprint(m,hashAdapter());return m;
}
lan_tls::Response startRaw(const Client& client,const delivery_store::Metadata& m){return request("/lan/v1/uploads/start",client.auth(),true,{{"requestId",m.requestId},{"fingerprint",m.fingerprint},{"kind",m.kind==Kind::Push?"push":"file"},{"name",m.name},{"title",m.title},{"size",std::to_string(m.size)},{"sha256",m.sha256},{"baseRevision",m.kind==Kind::Push?std::to_string(m.baseRevision):""}});}
Fields entryFields(const std::vector<delivery_store::Metadata>& entries){
  Fields fields;for(size_t i=0;i<entries.size();++i)fields.push_back({"e"+std::to_string(i),lan_batch::packEntry(entries[i])});return fields;
}
lan_tls::Response offer(const Client& client,const std::vector<delivery_store::Metadata>& entries){return request("/lan/v2/batch/offer",client.auth(),true,entryFields(entries));}
void accept(const Client& client,const std::vector<delivery_store::Metadata>& entries){
  const auto response=offer(client,entries);
  assert(response.code==200&&jsonString(response.json,"state")=="awaiting_acceptance");
  assert(lan_service::promptPending()&&lan_service::acceptPrompt());
}
lan_tls::Response begin(const Client& client,const delivery_store::Metadata& m){accept(client,{m});return startRaw(client,m);}
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

namespace delivery_store_sd {
delivery_store::Store& store(){if(!instance)instance.reset(new delivery_store::Store(ioAdapter(),hashAdapter()));return *instance;}
void setHooks(delivery_store::SDHooks value){hooks=value;}
bool mount(){lease=storage_validation::lease();enabled=sdOK&&lease;store().beginRecovery();return enabled;}
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
const char* deviceId(){return "aaaaaaaaaaaaaaaa";} // first 16 hex digits of the fingerprint
}
esp_err_t nvs_open(const char* name,int mode,nvs_handle_t* handle){assert(!std::strcmp(name,"paperlan")&&mode==NVS_READWRITE);if(nvsOpenError)return nvsOpenError;*handle=17;++nvsOpenHandles;return ESP_OK;}
void nvs_close(nvs_handle_t handle){assert(handle==17&&nvsOpenHandles);--nvsOpenHandles;nvsPending.clear();}
esp_err_t nvs_get_blob(nvs_handle_t handle,const char* key,void* out,size_t* length){assert(handle==17);if(nvsGetError)return nvsGetError;auto found=nvsCommitted.find(key);if(found==nvsCommitted.end())return ESP_ERR_NVS_NOT_FOUND;if(out){if(*length<found->second.size())return ESP_FAIL;std::memcpy(out,found->second.data(),found->second.size());}*length=found->second.size();return ESP_OK;}
esp_err_t nvs_set_blob(nvs_handle_t handle,const char* key,const void* bytes,size_t length){assert(handle==17&&!std::strcmp(key,"lanAuthV1"));const auto* p=static_cast<const uint8_t*>(bytes);nvsPending[key]={p,p+length};return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t handle){assert(handle==17);if(nvsCommitError)return nvsCommitError;for(const auto& entry:nvsPending)nvsCommitted[entry.first]=entry.second;nvsPending.clear();if(failReadbackAfterCommit)nvsGetError=ESP_FAIL;return ESP_OK;}

namespace {
void pushFileReceiptScopeAndUi(){
  reset();const Client first=pair();
  const auto push=metadata(first,1,"Synthetic Push");deliver(first,push,"Synthetic Push");tick(1000);
  assert(text(settingsLanState)=="Done · 1 file"&&text(settingsLanType).empty());
  assert(sdBooks.size()==1&&lan_service::isPushPath(sdBooks.front().c_str()));
  const auto file=metadata(first,2,"Synthetic ordinary file",Kind::File);deliver(first,file,"Synthetic ordinary file");tick(1000);
  assert(text(settingsLanState)=="Done · 1 file");
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
  assert(!lan_service::active()&&!lan_service::promptPending());
  assert(lan_service::start("127.0.0.1"));settle();
  const auto second=metadata(client,4,"Card loss");assert(begin(client,second).code==200);
  // A removed card refuses the transfer without touching storage.
  const auto beforeReads=reads,beforeWrites=writes;sdOK=false;
  assert(chunk(client,second,0,"Card loss").code!=200);tick();
  assert(reads==beforeReads&&writes==beforeWrites);sdOK=true;
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
  assert(text(settingsLanState)=="Done · 1 file");
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
using lan_service::RegistryStatus;
delivery_store::Metadata renamed(delivery_store::Metadata m,const std::string& name){m.name=name;m.fingerprint=delivery_store::fingerprint(m,hashAdapter());return m;}
std::string admissionOf(const std::string& json,const std::string& request){
  const auto at=json.find(request);assert(at!=std::string::npos);
  const auto key=json.find("\"admission\":\"",at);assert(key!=std::string::npos);
  const auto start=key+13;return json.substr(start,json.find('"',start)-start);
}
void upload(const Client& client,const delivery_store::Metadata& m,const std::string& body){
  assert(startRaw(client,m).code==200);
  for(size_t at=0;at<body.size();at+=4096)assert(chunk(client,m,at,body.substr(at,4096)).code==200);
  assert(request("/lan/v1/uploads/commit",client.auth(),true,{{"requestId",m.requestId}}).code==200);
  for(unsigned i=0;i<100&&delivery_store_sd::store().activeState()==State::Checking;++i)tick();
  assert(delivery_store_sd::store().receipt(client.id,m.requestId).state==State::Complete);
  tick(1000);
}
void firstContactPairingAndAuthentication(){
  reset();assert(request("/lan/v1/capabilities").code==401);
  assert(request("/lan/v2/pair/reveal","",true,{{"pairingId",std::string(16,'0')},{"senderNonce",std::string(64,'0')}}).code==404);
  const auto c=contact("Synthetic client",{},7,false);
  assert(c.offer.code==200&&jsonString(c.offer.json,"state")=="awaiting_reveal"&&jsonString(c.offer.json,"deviceId")=="aaaaaaaaaaaaaaaa");
  assert(!lan_service::promptPending()); // nothing is shown before the sender reveals Ns
  assert(contactStatus(c).code==404);
  assert(contact("Other",{},9,false).offer.code==409); // one pending batch at a time
  const auto revealed=request("/lan/v2/pair/reveal","",true,{{"pairingId",c.pairingId},{"senderNonce",delivery_store::hexadecimal(c.nonce,32)}});
  assert(revealed.code==200&&revealed.json.find("sas")==std::string::npos);
  const auto& shownPrompt=lan_service::prompt();
  assert(lan_service::promptPending()&&shownPrompt.firstContact&&shownPrompt.label=="Synthetic client"&&shownPrompt.total==0);
  // The displayed SAS equals the sender's own computation over the certificate
  // it observed; a different (MITM) certificate yields a different number.
  uint8_t certificate[32],deviceNonce[32],other[32];std::memset(other,0xbb,32);
  assert(lan_batch::decodeHex(lan_tls::fingerprint(),certificate,32)&&lan_batch::decodeHex(jsonString(c.offer.json,"deviceNonce"),deviceNonce,32));
  uint32_t expected,mitm;
  assert(lan_batch::sas(testDigest,certificate,c.nonce,deviceNonce,c.pairingId,"Synthetic client",expected));
  assert(lan_batch::sas(testDigest,other,c.nonce,deviceNonce,c.pairingId,"Synthetic client",mitm));
  assert(shownPrompt.sas==lan_batch::sasText(expected)&&mitm!=expected);
  const auto waiting=contactStatus(c);
  assert(waiting.code==200&&jsonString(waiting.json,"state")=="awaiting_acceptance"&&waiting.json.find("token")==std::string::npos);
  assert(request("/lan/v2/batch/status","",false,{{"batchId",c.batchId},{"proof",std::string(64,'0')}}).code==404);
  assert(request("/lan/v2/batch/status","",false,{{"batchId",c.batchId}}).code==404);
  assert(lan_service::acceptPrompt()&&!lan_service::promptPending());
  const auto accepted=contactStatus(c);assert(accepted.code==200&&jsonString(accepted.json,"state")=="accepted");
  const Client client{jsonString(accepted.json,"clientId"),jsonString(accepted.json,"token")};
  // A lost status response can be retried with the same proof until the
  // sender's first bearer request shows it stored the token.
  assert(contactStatus(c).json.find(client.token)!=std::string::npos);
  tick(10000);
  assert(request("/lan/v1/capabilities",client.auth()).code==200);
  assert(contactStatus(c).json.find("token")==std::string::npos);
  const auto capabilities=request("/lan/v1/capabilities",client.auth());assert(capabilities.json.find("\"v\":2")!=std::string::npos);
  const auto& bytes=nvsCommitted.at("lanAuthV1");uint8_t token[32];assert(lan_auth::decode(client.token,token,32));
  assert(std::search(bytes.begin(),bytes.end(),token,token+32)==bytes.end());
  assert(request("/lan/v1/capabilities","Bearer "+client.token).code==401);
  assert(request("/lan/v1/clients/revoke",client.auth(),true,{{"clientId",client.id}}).code==200);
  assert(request("/lan/v1/capabilities",client.auth()).code==401);
  for(const char* removed:{"/lan/v1/pair","/lan/v1/clients"})assert(request(removed,"",true,{{"secret",std::string(32,'e')}}).code==401);
  lan_service::stop();assert(!lan_service::active());assert(request("/lan/v1/capabilities",client.auth()).code==503);
}
void wrongRevealExpiryAndCancel(){
  reset();
  auto c=contact("Synthetic client",{},7,false);uint8_t wrong[32];std::memset(wrong,1,32);
  assert(request("/lan/v2/pair/reveal","",true,{{"pairingId",c.pairingId},{"senderNonce",delivery_store::hexadecimal(wrong,32)}}).code==409);
  assert(!lan_service::promptPending());
  assert(request("/lan/v2/pair/reveal","",true,{{"pairingId",c.pairingId},{"senderNonce",delivery_store::hexadecimal(c.nonce,32)}}).code==404);
  tick(10000);
  c=contact("Synthetic client",{},11,false);tick(60000);
  assert(request("/lan/v2/pair/reveal","",true,{{"pairingId",c.pairingId},{"senderNonce",delivery_store::hexadecimal(c.nonce,32)}}).code==404);
  tick(10000);
  c=contact("Synthetic client",{},13);assert(c.reveal.code==200&&lan_service::promptPending());
  assert(lan_service::poll());assert(!lan_service::poll()); // the prompt dirties the panel once
  tick(59999);assert(lan_service::promptPending());tick(1);
  assert(!lan_service::promptPending()&&!lan_service::acceptPrompt()&&text(settingsLanState)=="Expired");
  const auto expired=contactStatus(c);
  assert(expired.code==200&&jsonString(expired.json,"state")=="expired"&&expired.json.find("token")==std::string::npos);
  assert(!nvsCommitted.count("lanAuthV1"));
  tick(10000);
  c=contact("Synthetic client",{},15);assert(lan_service::promptPending());
  lan_service::stop();assert(!lan_service::promptPending()&&!lan_service::acceptPrompt()&&!nvsCommitted.count("lanAuthV1"));
}
void rateAndCredentialFailure(){
  reset();const Client client=pair();tick(10000);
  for(unsigned i=0;i<8;++i)assert(request("/lan/v1/capabilities","invalid").code==401);
  assert(request("/lan/v1/capabilities",client.auth()).code==429);tick(10000);
  assert(request("/lan/v1/capabilities",client.auth()).code==200);
  // Unauthenticated offers spend the same budget, even when malformed.
  for(unsigned i=0;i<8;++i)assert(request("/lan/v2/batch/offer","",true,{{"label","Flood"}}).code==401);
  assert(request("/lan/v1/capabilities",client.auth()).code==429);tick(10000);
  lan_service::stop();nvsGetError=ESP_ERR_NVS_TYPE_MISMATCH;assert(!lan_service::start("127.0.0.1"));
  assert(!lan_service::active());assert(request("/lan/v1/capabilities",client.auth()).code==503);
  nvsGetError=ESP_OK;assert(lan_service::start("127.0.0.1"));settle();
  assert(request("/lan/v1/capabilities",client.auth()).code==200);
  const auto c=contact("Commit failure");assert(c.reveal.code==200);nvsCommitError=ESP_FAIL;
  assert(!lan_service::acceptPrompt()&&!lan_service::promptPending());
  tick();assert(tlsDraining&&!tlsAccepting);assert(text(settingsLanState)=="Credentials unavailable");
  tick();assert(!lan_service::active()&&!tlsLive&&nvsOpenHandles==0);
  assert(request("/lan/v1/capabilities",client.auth()).code==503);nvsCommitError=ESP_OK;
}
void savedPairWithFailedReadbackRemainsRecoverable(){
  reset();const auto c=contact("Readback orphan");assert(c.reveal.code==200);failReadbackAfterCommit=true;
  assert(!lan_service::acceptPrompt());assert(nvsCommitted.count("lanAuthV1"));
  tick();tick();assert(!lan_service::active());
  failReadbackAfterCommit=false;nvsGetError=ESP_OK;
  std::vector<lan_service::ClientInfo> list;
  assert(lan_service::listClients(list)==RegistryStatus::Busy); // switching to A stops N first
  lan_service::stop();
  assert(lan_service::listClients(list)==RegistryStatus::Ok&&list.size()==1&&list.front().label=="Readback orphan");
  assert(lan_service::revokeClient(list.front().clientId)==RegistryStatus::Ok);
  assert(lan_service::listClients(list)==RegistryStatus::Ok&&list.empty());
}
void batchConsentGatesUploads(){
  reset();const Client client=pair();
  const std::string firstBody="First synthetic file",pushBody="Synthetic Push",secondBody="Second synthetic file";
  const auto first=metadata(client,20,firstBody,Kind::File);
  const auto push=metadata(client,21,pushBody);
  const auto extraPush=metadata(client,22,"Another Push");
  const auto second=renamed(metadata(client,23,secondBody,Kind::File),"second.md");
  const auto unsupported=renamed(metadata(client,24,"image bytes",Kind::File),"image.png");
  const auto refused=startRaw(client,first);assert(refused.code==409&&refused.json.find("not_accepted")!=std::string::npos);
  const auto offered=offer(client,{first,push,extraPush,second,unsupported});
  assert(offered.code==200&&jsonString(offered.json,"state")=="awaiting_acceptance");
  assert(admissionOf(offered.json,first.requestId)=="ok"&&admissionOf(offered.json,push.requestId)=="ok");
  assert(admissionOf(offered.json,extraPush.requestId)=="duplicate_push"&&admissionOf(offered.json,second.requestId)=="ok");
  assert(admissionOf(offered.json,unsupported.requestId)=="unsupported_format");
  const auto& shownPrompt=lan_service::prompt();
  assert(lan_service::promptPending()&&!shownPrompt.firstContact&&shownPrompt.label=="Synthetic client"&&shownPrompt.sas.empty());
  assert(shownPrompt.total==3&&shownPrompt.names.size()==3&&shownPrompt.names[0]=="synthetic.md"&&shownPrompt.names[1]=="Push: Synthetic title"&&shownPrompt.names[2]=="second.md");
  assert(shownPrompt.replacesPush.empty());
  assert(startRaw(client,first).code==409); // still waiting for A+B
  assert(offer(client,{first}).code==409);   // one pending batch
  assert(lan_service::acceptPrompt());
  assert(startRaw(client,unsupported).code==409&&startRaw(client,extraPush).code==409);
  upload(client,first,firstBody);assert(text(settingsLanType).find("1/3")==0);
  upload(client,push,pushBody);assert(text(settingsLanType).find("2/3")==0);
  assert(contact("Other sender").offer.code==409); // unfinished batch keeps the slot from others
  upload(client,second,secondBody);assert(text(settingsLanState)=="Done · 3 files");
  // A finished batch frees the slot. Stale revisions and receipts already held
  // are reported per entry; when nothing is admissible there is no prompt.
  const auto stale=metadata(client,25,"Stale Push",Kind::Push,0);
  const auto blocked=offer(client,{stale,first});
  assert(blocked.code==200&&jsonString(blocked.json,"state")=="blocked"&&!lan_service::promptPending());
  assert(admissionOf(blocked.json,stale.requestId)=="revision_conflict"&&admissionOf(blocked.json,first.requestId)=="already_received");
  const auto next=metadata(client,26,"Next Push",Kind::Push,1);
  assert(offer(client,{next}).code==200&&lan_service::prompt().replacesPush=="Synthetic title");
}
void malformedOffers(){
  reset();const Client client=pair();const auto m=metadata(client,30,"Synthetic body",Kind::File);
  const auto packed=lan_batch::packEntry(m);
  assert(request("/lan/v2/batch/offer",client.auth(),true,{{"e1",packed}}).code==400);
  assert(request("/lan/v2/batch/offer",client.auth(),true,{{"e0",packed},{"e2",packed}}).code==400);
  assert(request("/lan/v2/batch/offer",client.auth(),true,{{"e0",packed},{"e8",packed}}).code==400);
  assert(request("/lan/v2/batch/offer",client.auth(),true,{{"e0",packed},{"x","1"}}).code==400);
  assert(request("/lan/v2/batch/offer",client.auth(),true,{{"e0",packed},{"e1",packed}}).code==400);
  assert(request("/lan/v2/batch/offer",client.auth(),true,{{"e0",packed.substr(0,packed.size()-1)}}).code==400);
  assert(request("/lan/v2/batch/offer",client.auth(),true,{}).code==400);
  assert(!lan_service::promptPending());
  // Eight worst-case entries (96-byte UTF-8 names and titles) fit the shared
  // 16-field and 17,000-byte request bounds; the prompt lists five names.
  std::string name,title;for(unsigned i=0;i<31;++i)name+="\xe6\x9b\xb8";name+=".md";for(unsigned i=0;i<32;++i)title+="\xe9\xa1\x8c";
  std::vector<delivery_store::Metadata> many;
  for(unsigned i=0;i<8;++i){auto e=metadata(client,40+i,"Synthetic "+std::to_string(i),Kind::File);e.title=title;e=renamed(e,name);many.push_back(e);}
  const auto fields=entryFields(many);size_t encoded=0;for(const auto& f:fields)encoded+=formEncode(f.first).size()+formEncode(f.second).size()+2;
  assert(encoded<=BoundedHttpRequest::MAX_BODY&&fields.size()<=BoundedHttpRequest::MAX_FIELDS);
  const auto response=offer(client,many);assert(response.code==200);
  assert(lan_service::prompt().total==8&&lan_service::prompt().names.size()==5);
}
void firstContactWithEntriesPairsAndAccepts(){
  reset();const Client none{std::string(16,'0'),""};const std::string body="First contact file";
  auto m=metadata(none,50,body,Kind::File);
  const auto c=contact("Synthetic sender",entryFields({m}));
  assert(c.reveal.code==200&&lan_service::prompt().firstContact&&lan_service::prompt().total==1&&lan_service::prompt().names.front()=="synthetic.md");
  assert(lan_service::acceptPrompt());
  const auto status=contactStatus(c);const Client client{jsonString(status.json,"clientId"),jsonString(status.json,"token")};
  m.clientId=client.id;upload(client,m,body);assert(text(settingsLanState)=="Done · 1 file");
  // Nothing admissible on first contact: no prompt and no client is created.
  lan_service::stop();assert(lan_service::start("127.0.0.1"));settle();tick(10000);
  const auto before=nvsCommitted;
  const auto blocked=contact("Blocked sender",entryFields({renamed(metadata(none,51,"x",Kind::File),"bad.png")}),17,false);
  assert(blocked.offer.code==200&&jsonString(blocked.offer.json,"state")=="blocked"&&!lan_service::promptPending()&&nvsCommitted==before);
  // Four senders fill the registry; a fifth first contact is refused.
  for(unsigned i=0;i<3;++i){pair("Synthetic filler "+std::to_string(i));tick(10000);}
  assert(contact("Fifth sender",{},21,false).offer.code==409);
}
void senderManagementForWebUi(){
  reset();std::vector<Client> clients;for(unsigned i=0;i<3;++i){clients.push_back(pair("Synthetic sender "+std::to_string(i)));tick(10000);}
  std::vector<lan_service::ClientInfo> list;
  assert(lan_service::listClients(list)==RegistryStatus::Busy&&lan_service::forgetClients()==RegistryStatus::Busy);
  lan_service::stop();
  assert(lan_service::listClients(list)==RegistryStatus::Ok&&list.size()==3);
  for(unsigned i=0;i<3;++i)assert(list[i].clientId==clients[i].id&&list[i].label=="Synthetic sender "+std::to_string(i));
  assert(lan_service::revokeClient(clients[1].id)==RegistryStatus::Ok);
  assert(lan_service::listClients(list)==RegistryStatus::Ok&&list.size()==2&&list[0].clientId==clients[0].id&&list[1].clientId==clients[2].id);
  assert(lan_service::revokeClient("zz")==RegistryStatus::Invalid);
  nvsCommitted["lanAuthV1"][12]^=0xff; // a corrupt record is reported, never silently reset
  assert(lan_service::listClients(list)==RegistryStatus::Unreadable&&lan_service::revokeClient(clients[0].id)==RegistryStatus::Unreadable);
  assert(lan_service::forgetClients()==RegistryStatus::Ok);
  assert(lan_service::listClients(list)==RegistryStatus::Ok&&list.empty()&&nvsOpenHandles==0);
  assert(lan_service::start("127.0.0.1"));settle();
  assert(request("/lan/v1/capabilities",clients[0].auth()).code==401);
}
void mdnsFollowsTheReceiver(){
  reset();
  assert(MDNS.running&&MDNS.host=="paperdiy-aaaaaaaaaaaaaaaa"&&MDNS.service=="paperdiy"&&MDNS.proto=="tcp"&&MDNS.port==8443);
  assert(MDNS.txtKey=="id"&&MDNS.txtValue=="aaaaaaaaaaaaaaaa");
  lan_service::stop();assert(!MDNS.running);
  MDNS.failBegin=true;assert(lan_service::start("127.0.0.1"));settle();assert(lan_service::active()&&!MDNS.running);
  MDNS.failBegin=false;lan_service::stop();
}
}
// A sender that stopped uploading its own accepted batch can offer again; the
// replacement needs a new A+B and the old acceptance no longer admits uploads.
void ownAcceptedBatchCanBeReplaced(){
  reset();const Client client=pair();
  const auto hello=request("/lan/v2/hello","",false);
  assert(hello.code==200&&jsonString(hello.json,"deviceId")==lan_tls::deviceId()&&hello.json.find("\"pushRevision\":0")!=std::string::npos);
  assert(hello.json.find("token")==std::string::npos&&hello.json.find("client")==std::string::npos);
  const auto a=metadata(client,40,"A body",Kind::File);
  const auto b=renamed(metadata(client,41,"B body",Kind::File),"b.md");
  accept(client,{a,b});upload(client,a,"A body");
  const auto replaced=offer(client,{b});
  assert(replaced.code==200&&jsonString(replaced.json,"state")=="awaiting_acceptance"&&lan_service::promptPending());
  const auto refused=startRaw(client,b);assert(refused.code==409&&refused.json.find("not_accepted")!=std::string::npos);
  assert(lan_service::acceptPrompt());upload(client,b,"B body");assert(text(settingsLanState)=="Done · 1 file");
}
// P: a paired sender replaces Push with no batch; everything else stays in N.
lan_tls::Response startLive(const Client& client,const delivery_store::Metadata& m,const char* position){
  Fields fields={{"requestId",m.requestId},{"fingerprint",m.fingerprint},{"kind",m.kind==Kind::Push?"push":"file"},{"name",m.name},{"title",m.title},{"size",std::to_string(m.size)},{"sha256",m.sha256},{"baseRevision",m.kind==Kind::Push?std::to_string(m.baseRevision):""}};
  if(position)fields.push_back({"position",position});
  return request("/lan/v1/uploads/start",client.auth(),true,fields);
}
void finishLive(const Client& client,const delivery_store::Metadata& m,const std::string& body){
  for(size_t at=0;at<body.size();at+=4096)assert(chunk(client,m,at,body.substr(at,4096)).code==200);
  assert(request("/lan/v1/uploads/commit",client.auth(),true,{{"requestId",m.requestId}}).code==200);
  for(unsigned i=0;i<100&&delivery_store_sd::store().activeState()==State::Checking;++i)tick();
  assert(delivery_store_sd::store().receipt(client.id,m.requestId).state==State::Complete);
  tick(1000);
}
void liveModeAdmitsPushOnly(){
  reset();const Client client=pair();
  assert(request("/lan/v2/hello").json.find("\"live\":false")!=std::string::npos);
  assert(request("/lan/v1/capabilities",client.auth()).json.find("\"live\":false")!=std::string::npos);
  lan_service::stop();
  size_t count=0;assert(lan_service::pairedCount(count)==RegistryStatus::Ok&&count==1);
  assert(lan_service::start("127.0.0.1",true));settle();
  assert(lan_service::pairedCount(count)==RegistryStatus::Ok&&count==1);
  assert(request("/lan/v2/hello").json.find("\"live\":true")!=std::string::npos);
  assert(request("/lan/v1/capabilities",client.auth()).json.find("\"live\":true")!=std::string::npos);
  lan_service::takePushUpdated();
  const std::string body="Synthetic live Push";const auto keep=metadata(client,50,body);
  const auto started=startLive(client,keep,"keep");assert(started.code==200&&!lan_service::promptPending());
  finishLive(client,keep,body);
  assert(lan_service::takePushUpdated()&&!lan_service::takePushUpdated()&&lan_service::pushKeepsPosition());
  assert(sdBooks.size()==1&&lan_service::isPushPath(sdBooks.front().c_str()));
  // A replay must repeat its hint; the receipt answers either way it matches.
  assert(startLive(client,keep,"keep").json.find("\"complete\"")!=std::string::npos);
  const auto changed=startLive(client,keep,"top");assert(changed.code==409&&changed.json.find("request_conflict")!=std::string::npos);
  const auto omitted=startLive(client,keep,nullptr);assert(omitted.code==409&&omitted.json.find("request_conflict")!=std::string::npos);
  // The default is the first page; the same body publishes nothing new.
  const auto top=metadata(client,51,"Second synthetic Push",Kind::Push,1);
  assert(startLive(client,top,nullptr).code==200);finishLive(client,top,"Second synthetic Push");
  assert(lan_service::takePushUpdated()&&!lan_service::pushKeepsPosition());
  const auto same=metadata(client,52,"Second synthetic Push",Kind::Push,2);
  assert(startLive(client,same,"keep").code==200);finishLive(client,same,"Second synthetic Push");
  assert(!lan_service::takePushUpdated());
  const auto stale=metadata(client,53,"Stale",Kind::Push,1);
  const auto conflict=startLive(client,stale,nullptr);assert(conflict.code==409&&conflict.json.find("revision_conflict")!=std::string::npos);
  const auto bogus=metadata(client,54,"Bogus",Kind::Push,2);
  assert(startLive(client,bogus,"middle").code==400&&startLive(client,bogus,"").code==400);
  // Files, batches and pairing belong to N.
  const auto file=metadata(client,55,"Synthetic file",Kind::File);
  assert(startLive(client,file,"top").code==400);
  const auto refused=startLive(client,file,nullptr);assert(refused.code==409&&refused.json.find("not_accepted")!=std::string::npos);
  const auto batch=offer(client,{file});assert(batch.code==409&&batch.json.find("live_mode")!=std::string::npos);
  const auto contacted=contact("Live stranger");assert(contacted.offer.code==409&&contacted.offer.json.find("live_mode")!=std::string::npos);
  assert(!lan_service::promptPending());
  // A self-revocation leaves no paired sender, which ends P in the app.
  assert(request("/lan/v1/clients/revoke",client.auth(),true,{{"clientId",client.id}}).code==200);
  assert(lan_service::pairedCount(count)==RegistryStatus::Ok&&count==0);
  // Back in N, a Push again needs an accepted batch.
  lan_service::stop();assert(lan_service::start("127.0.0.1"));settle();const Client again=pair("Second synthetic client");
  const auto queued=metadata(again,56,"Queued",Kind::Push,2);
  const auto gated=startRaw(again,queued);assert(gated.code==409&&gated.json.find("not_accepted")!=std::string::npos);
}
#ifndef PAPERDIY_LAN_WIRE
int main(){
  liveModeAdmitsPushOnly();
  ownAcceptedBatchCanBeReplaced();
  firstContactPairingAndAuthentication();wrongRevealExpiryAndCancel();rateAndCredentialFailure();savedPairWithFailedReadbackRemainsRecoverable();
  batchConsentGatesUploads();malformedOffers();firstContactWithEntriesPairsAndAccepts();senderManagementForWebUi();mdnsFollowsTheReceiver();
  pushFileReceiptScopeAndUi();stopAndStorageOwnership();malformedBase64AndProgressLease();statusTrafficCannotResetLeaseUsingOldReceipt();
  sameBodyNoopGetsTerminalUiWithoutClientPolling();sameSlotRowReturnsAfterOrdinaryScanAndRemount();nRestartReconcilesExistingPublicationIntent();totalLeaseBoundsProgressAndChecking();
  lan_service::stop();assert(!nvsOpenHandles);std::puts("LAN service integration tests passed");
}
#else
// Line protocol for tests/lan_client.py: the real Python sender drives these
// real handlers with its exact request bytes. Responses are framed as lan_tls
// frames them. Commands: REQ <hex>, ACCEPT, PROMPT, TICK <ms>, OFF, ON, LIVE.
#include <iostream>
#include <string>
namespace {
std::string hexOf(const std::string& bytes){static const char d[]="0123456789abcdef";std::string out;for(unsigned char c:bytes){out+=d[c>>4];out+=d[c&15];}return out;}
std::string bytesOf(const std::string& hex){std::string out;for(size_t i=0;i+1<hex.size();i+=2)out+=char(std::stoi(hex.substr(i,2),nullptr,16));return out;}
}
int main(){
  reset();std::string line;
  while(std::getline(std::cin,line)){
    const auto space=line.find(' ');const std::string command=line.substr(0,space),argument=space==std::string::npos?"":line.substr(space+1);
    if(command=="REQ"){
      if(!lan_service::active()){std::cout<<"DOWN"<<std::endl;continue;}
      const auto wire=bytesOf(argument);BoundedHttpRequest incoming;for(char byte:wire)incoming.feed(byte);
      lan_tls::Response response{400,"{\"code\":\"bad_request\"}"};
      if(incoming.state==BoundedHttpRequest::State::Ready)response=captured(incoming);
      const std::string framed="HTTP/1.1 "+std::to_string(response.code)+" Response\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\nContent-Length: "+std::to_string(response.json.size())+"\r\n\r\n"+response.json;
      tick();std::cout<<"RES "<<hexOf(framed)<<std::endl;
    }else if(command=="ACCEPT"){std::cout<<(lan_service::acceptPrompt()?"OK":"NO")<<std::endl;}
    else if(command=="PROMPT"){
      const auto& p=lan_service::prompt();
      std::cout<<"PROMPT "<<(lan_service::promptPending()?1:0)<<" "<<p.total<<" "<<hexOf(p.label)<<" "<<hexOf(p.sas.empty()?"-":p.sas)<<std::endl;
    }else if(command=="TICK"){tick(uint32_t(std::stoul(argument)));std::cout<<"OK"<<std::endl;}
    else if(command=="OFF"){lan_service::stop();std::cout<<"OK"<<std::endl;}
    else if(command=="ON"){std::cout<<(lan_service::start("127.0.0.1")?"OK":"NO")<<std::endl;settle();}
    else if(command=="LIVE"){std::cout<<(lan_service::start("127.0.0.1",true)?"OK":"NO")<<std::endl;settle();}
    else std::cout<<"ERR"<<std::endl;
  }
  lan_service::stop();
}
#endif
