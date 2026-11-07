// Sleep windows against the firmware's real SD adapter and storage lifecycle.
#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>
#include <nvs.h>
#include <openssl/sha.h>
#include "card_sleep.h"
#include "delivery_store_sd.h"
#include "lan_service.h"
#include "lan_tls.h"
#include "sd_reader.h"
#include "storage_validation.h"
#include "ui.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

bool sdOK=true;
std::vector<String> sdBooks;
size_t selectedBook=0;
String sdError,settingsLanType,settingsLanName,settingsLanState;

namespace lan_tls {
bool begin(lan_nvs::Store&,Handler){return true;}
void stop(){}
void stopAfterResponse(){}
void poll(){}
bool active(){return true;}
const char* fingerprint(){return "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";}
const char* deviceId(){return "aaaaaaaaaaaaaaaa";}
}
esp_err_t nvs_open(const char*,int,nvs_handle_t* handle){*handle=17;return ESP_OK;}
void nvs_close(nvs_handle_t){}
esp_err_t nvs_get_blob(nvs_handle_t,const char*,void*,size_t*){return ESP_ERR_NVS_NOT_FOUND;}
esp_err_t nvs_set_blob(nvs_handle_t,const char*,const void*,size_t){return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t){return ESP_OK;}

namespace {
using delivery_store::Code;using delivery_store::Kind;using delivery_store::State;
const char* const CLIENT="0123456789abcdef";
const char* const ADDRESS="127.0.0.1";
delivery_store::Store& store(){return delivery_store_sd::store();}
// Same calls as the production CardSleepBackend in src/power.cpp.
struct Pins {
  int after=0;bool failRestore=false;
  bool disableInterrupt(){return true;}
  bool armWake(int){return true;}
  bool disarmWake(){return true;}
  bool restoreEdge(){return !failRestore;}
  bool enableInterrupt(){return true;}
  int level(){return after;}
  void revokeProofs(){storage_validation::revokeProofs();}
  void revokeTrust(){storage_validation::invalidate();}
  void signalChange(){storage_validation::signalCardEvent();}
};
void sleepOnce(int after=0,bool failRestore=false){
  Pins pins;pins.after=after;pins.failRestore=failRestore;
  card_sleep::Window<Pins> window(pins,0);
  assert(window.begin());window.finish();
}
void settle(){
  for(unsigned i=0;i<200&&!store().recovered();++i)lan_service::storagePoll();
  assert(store().recovered());
}
void boot(){
  lan_service::stop();lan_service::storageChanged();
  std::filesystem::remove_all(testCardRoot);std::filesystem::create_directories(testCardRoot);
  SD.failReceiptRecord=false;sdOK=true;sdBooks.clear();selectedBook=0;
  storage_validation::acknowledgeCardEvent();storage_validation::beginExclusive();
  lan_service::storagePoll();settle();assert(delivery_store_sd::metadataCurrent());
  lan_service::takeLibraryChanged();lan_service::takePushUpdated();
}
delivery_store::Hash digestOnly(){
  delivery_store::Hash hash;
  hash.digest=[](void*,const uint8_t* bytes,size_t count,uint8_t out[32]){return SHA256(bytes,count,out)!=nullptr;};
  return hash;
}
delivery_store::Metadata metadata(unsigned id,const std::string& body,Kind kind,uint32_t base=0){
  delivery_store::Metadata m;
  m.clientId=CLIENT;m.requestId=std::string(24,'0')+delivery_store::hexadecimal(reinterpret_cast<const uint8_t*>(&id),4);
  m.kind=kind;m.name=kind==Kind::Push?"Push.md":"synthetic.md";m.title="Synthetic title";m.size=uint32_t(body.size());m.baseRevision=base;
  uint8_t digest[32];SHA256(reinterpret_cast<const uint8_t*>(body.data()),body.size(),digest);
  m.sha256=delivery_store::hexadecimal(digest,32);m.fingerprint=delivery_store::fingerprint(m,digestOnly());return m;
}
void send(const delivery_store::Metadata& m,const std::string& body){
  assert(store().begin(m).code==Code::Ok);
  const auto accepted=store().chunk(m.clientId,m.requestId,0,reinterpret_cast<const uint8_t*>(body.data()),body.size());
  assert(accepted.code==Code::Ok&&accepted.offset==body.size());
  assert(store().commit(m.clientId,m.requestId).state==State::Checking);
  for(unsigned i=0;i<100&&store().activeState()==State::Checking;++i)delivery_store_sd::poll();
}
void deliver(const delivery_store::Metadata& m,const std::string& body){
  send(m,body);assert(store().receipt(m.clientId,m.requestId).state==State::Complete);
  lan_service::storagePoll();
}
State receiptState(const delivery_store::Metadata& m){return store().receipt(m.clientId,m.requestId).state;}
std::string onCard(const std::string& path){return testCardRoot+path;}
// Records on the card now differ from the store's memory. Only a real recovery
// can notice, which is how the tests below tell recovery from a kept store.
void forgetReceiptOnCard(const delivery_store::Metadata& m){
  const auto slot=store().receipt(m.clientId,m.requestId).receipt.sequence%delivery_store::RECEIPTS;
  assert(std::filesystem::remove(onCard("/.paperdiy/lan/receipt"+std::to_string(slot)+".rec")));
}

void sleepLeavesSettledStoreAlone(){
  boot();
  const auto push=metadata(1,"Synthetic Push",Kind::Push);deliver(push,"Synthetic Push");
  const auto file=metadata(2,"Synthetic file",Kind::File);deliver(file,"Synthetic file");
  const std::string path=lan_service::currentPushPath();assert(!path.empty());
  const auto rows=sdBooks;const auto selected=selectedBook;const auto revision=store().pushMetadata().revision;
  const auto calls=SD.calls;const auto lease=storage_validation::lease();
  lan_service::takeLibraryChanged();
  for(unsigned i=0;i<8;++i){
    sleepOnce();
    assert(storage_validation::lease()&&storage_validation::lease()!=lease);
    lan_service::storagePoll();
    // Anything here would keep the app loop awake instead of sleeping again.
    assert(!lan_service::storageBusy());
    assert(delivery_store_sd::mounted()&&store().recovered()&&lan_service::pushReady());
  }
  assert(SD.calls==calls);
  assert(lan_service::currentPushPath()==path&&sdBooks==rows&&selectedBook==selected);
  assert(!lan_service::takeLibraryChanged());
  assert(store().pushMetadata().revision==revision);
  assert(receiptState(push)==State::Complete&&receiptState(file)==State::Complete);
}
void storeStillWritesAfterSleep(){
  boot();sleepOnce();lan_service::storagePoll();
  const auto first=metadata(3,"First after sleep",Kind::File);deliver(first,"First after sleep");
  assert(std::filesystem::exists(onCard("/Books/synthetic.md")));
  // Its own writes move the lease. That must not read as a foreign change.
  sleepOnce();lan_service::storagePoll();assert(store().recovered()&&delivery_store_sd::mounted());
  const auto second=metadata(4,"Second after sleep",Kind::File);deliver(second,"Second after sleep");
  assert(std::filesystem::exists(onCard("/Books/synthetic (2).md")));
  assert(store().recovered()&&SD.openFiles==0);
  // Those writes do not date the rest of the records after the window.
  assert(delivery_store_sd::mounted()&&!delivery_store_sd::metadataCurrent());
}
void bookProofsStillExpire(){
  boot();
  uint8_t key[32]={1},digest[32]={2},out[32];
  const auto lease=storage_validation::lease();
  storage_validation::remember(lease,key,12,34,digest);storage_validation::rememberGraph(lease,key,digest);
  assert(storage_validation::find(key,12,34,out)&&storage_validation::verifiedGraph(key,digest));
  sleepOnce();lan_service::storagePoll();
  assert(storage_validation::lease()!=lease);
  assert(!storage_validation::find(key,12,34,out)&&!storage_validation::verifiedGraph(key,digest));
  // A proof taken under the old lease cannot be recorded after the window.
  storage_validation::remember(lease,key,12,34,digest);storage_validation::rememberGraph(lease,key,digest);
  assert(!storage_validation::find(key,12,34,out)&&!storage_validation::verifiedGraph(key,digest));
}
// What src/app.cpp does once a card edge has been debounced.
void remountAfterCardEvent(){
  storage_validation::invalidate();storage_validation::acknowledgeCardEvent();
  storage_validation::beginExclusive();lan_service::storageChanged();
}
void everyOtherChangeStillRecovers(){
  struct Move {const char* name;void (*apply)();};
  const Move moves[]={
    {"foreign write",[]{storage_validation::sourceChanged();}},
    {"remount",[]{storage_validation::beginExclusive();}},
    {"explicit reset",[]{lan_service::storageChanged();}},
    {"invalidate",[]{
      storage_validation::invalidate();assert(!delivery_store_sd::mounted());
      const auto calls=SD.calls;lan_service::storagePoll();assert(SD.calls==calls&&!delivery_store_sd::mounted());
      storage_validation::beginExclusive();}},
    {"sleep, then foreign write",[]{sleepOnce();storage_validation::sourceChanged();}},
    {"foreign write, then sleep",[]{storage_validation::sourceChanged();sleepOnce();}},
    {"sleep, then remount",[]{sleepOnce();lan_service::storagePoll();storage_validation::beginExclusive();}},
    {"card edge during sleep",[]{
      sleepOnce(1);assert(storage_validation::cardEventPending()&&!delivery_store_sd::mounted());
      const auto calls=SD.calls;lan_service::storagePoll();assert(SD.calls==calls);
      remountAfterCardEvent();}},
    {"card edge while awake",[]{
      storage_validation::signalCardEvent();assert(!delivery_store_sd::mounted());
      remountAfterCardEvent();}},
    {"sleep guard failure",[]{
      sleepOnce(0,true);assert(!storage_validation::lease()&&!delivery_store_sd::mounted());
      const auto calls=SD.calls;lan_service::storagePoll();assert(SD.calls==calls);
      storage_validation::beginExclusive();}},
  };
  for(const auto& move:moves){
    std::printf("recovery after: %s\n",move.name);
    boot();const auto file=metadata(5,"Synthetic file",Kind::File);deliver(file,"Synthetic file");
    forgetReceiptOnCard(file);assert(receiptState(file)==State::Complete);
    move.apply();
    lan_service::storagePoll();assert(!store().recovered()&&lan_service::storageBusy());
    settle();assert(delivery_store_sd::mounted()&&receiptState(file)==State::Unknown);
  }
}
void uncertainWaitsForExplicitStart(){
  boot();const std::string body="Published before receipt failure";const auto m=metadata(6,body,Kind::File);
  SD.failReceiptRecord=true;send(m,body);SD.failReceiptRecord=false;
  assert(store().activeState()==State::Uncertain&&std::filesystem::exists(onCard("/Books/synthetic.md")));
  lan_service::storagePoll();const auto calls=SD.calls;
  for(unsigned i=0;i<4;++i){
    sleepOnce();lan_service::storagePoll();
    assert(!lan_service::storageBusy()&&store().activeState()==State::Uncertain);
  }
  assert(SD.calls==calls&&sdBooks.empty());
  assert(lan_service::start(ADDRESS));assert(!store().recovered());settle();
  lan_service::storagePoll();
  assert(store().activeState()==State::Unknown&&receiptState(m)==State::Complete);
  assert(sdBooks.size()==1&&!std::strcmp(sdBooks.front().c_str(),"/Books/synthetic.md"));
  lan_service::stop();
}
void startRereadsOnlyAfterSleep(bool live){
  boot();const auto file=metadata(7,"Synthetic file",Kind::File);deliver(file,"Synthetic file");
  auto calls=SD.calls;
  assert(lan_service::start(ADDRESS,live));assert(store().recovered()&&SD.calls==calls);lan_service::stop();
  forgetReceiptOnCard(file);
  // Sleep cannot prove which card it woke up with. Nothing reads until a sender
  // could depend on the receipts, then they come from the card.
  sleepOnce();lan_service::storagePoll();assert(receiptState(file)==State::Complete);
  assert(lan_service::start(ADDRESS,live));assert(!store().recovered());settle();
  assert(receiptState(file)==State::Unknown);lan_service::stop();
  calls=SD.calls;
  assert(lan_service::start(ADDRESS,live));assert(store().recovered()&&SD.calls==calls);lan_service::stop();
}
// P owns the radio, so it never sleeps while up. It only sleeps while paused
// with the radio off (battery below the threshold) and then starts again.
void pushSurvivesPauseAndRestart(){
  boot();const std::string first="First synthetic Push",second="Second synthetic Push";
  deliver(metadata(8,first,Kind::Push),first);lan_service::takePushUpdated();
  assert(lan_service::start(ADDRESS,true));assert(store().recovered());
  deliver(metadata(9,second,Kind::Push,1),second);
  assert(lan_service::takePushUpdated());
  const std::string path=lan_service::currentPushPath();assert(!path.empty()&&store().pushMetadata().revision==2);
  const auto rows=sdBooks;
  lan_service::stop();
  const auto calls=SD.calls;
  for(unsigned i=0;i<4;++i){sleepOnce();lan_service::storagePoll();assert(lan_service::pushReady()&&lan_service::currentPushPath()==path);}
  assert(SD.calls==calls);
  assert(lan_service::start(ADDRESS,true));settle();lan_service::storagePoll();
  assert(lan_service::currentPushPath()==path&&sdBooks==rows&&store().pushMetadata().revision==2);
  assert(!lan_service::takePushUpdated());
  const std::string third="Third synthetic Push";deliver(metadata(10,third,Kind::Push,2),third);
  assert(lan_service::takePushUpdated()&&store().pushMetadata().revision==3);
  lan_service::stop();
}
}

int main(){
  const char* base=std::getenv("TMPDIR");
  std::string pattern=std::string(base&&*base?base:"/tmp")+"/paperdiy-card-XXXXXX";
  assert(mkdtemp(pattern.data()));testCardRoot=pattern;
  sleepLeavesSettledStoreAlone();
  storeStillWritesAfterSleep();
  bookProofsStillExpire();
  everyOtherChangeStillRecovers();
  uncertainWaitsForExplicitStart();
  startRereadsOnlyAfterSleep(false);
  startRereadsOnlyAfterSleep(true);
  pushSurvivesPauseAndRestart();
  assert(SD.openFiles==0);
  std::filesystem::remove_all(testCardRoot);
}
