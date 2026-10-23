#include <Arduino.h>
#include <M5Unified.h>
#include "app.h"
#include "version.h"
#include "reader.h"
#include "sd_reader.h"
#include "input.h"
#include "display_driver.h"
#include "power.h"
#include "ui.h"
#include "network.h"
#include "resume_store.h"
#include "reader_typography.h"
#include <SD.h>
#if defined(PAPERDIY_DIAGNOSTICS) && defined(PAPERDIY_NETWORK_TESTS)
#include "../tests/device_network.h"
#endif
#if defined(PAPERDIY_DIAGNOSTICS) && defined(PAPERDIY_DEVICE_TESTS)
#include "../tests/device_epub.h"
#include "../tests/device_power.h"
#include "../tests/device_typography.h"
#include "../tests/device_markdown.h"
#include "../tests/device_sd_io.h"
#endif
namespace {
enum class Screen {Library,Reading,Loading,Error,Settings};
Screen screen=Screen::Library;
controls::Buttons buttons;
uint32_t errorUntil=0,lastBatteryRefresh=0;
uint32_t lastCardCheck=0;
int cardLevel=HIGH,pendingCardLevel=HIGH;
bool shutdownRequested=false;
uint8_t keyMask(){
  uint8_t mask=(M5.BtnA.isPressed()?1:0)|(M5.BtnB.isPressed()?2:0);
  return controls::navigationMask(mask,settingsRotated,screen==Screen::Settings);
}
void discardLoadingInput(bool acceptPowerRequest=true){
  M5.update();buttons.discardUntilReleased(millis());
#ifdef PAPERDIY_DIAGNOSTICS
  while(Serial.available())Serial.read();
#endif
  // A/B navigation is discarded; a power-key click still requests a clean
  // shutdown once synchronous loading has finished.
  if(acceptPowerRequest&&M5.BtnPWR.wasClicked())shutdownRequested=true;
  Serial.println("INPUT_DISCARDED release_guard_ms=200");
}
bool leaveReading(){
  if(screen!=Screen::Reading||saveReadingProgress())return true;
  showError("Progress not saved; try again");return false;
}
void library(bool scene=true){
  networkStop();
  networkMode=NetworkMode::Off;
  if(!leaveReading())return;
  if(!sdOK&&digitalRead(48)==LOW){showLoading();scanSD();}
  screen=Screen::Library;showLibrary(scene);
}
void shutdown(){
  powerActivity();
  shutdownRequested=false;
  if(screen==Screen::Reading&&!pageOffsets.empty()){
    // Snapshot the current book and text offset before drawing the sleep image.
    uint32_t offset=currentReadingOffset();
    if(!saveReadingProgress()){
      showError("Progress not saved; try again");discardLoadingInput(false);return;
    }
    Serial.printf("RESUME_SAVED book=%s page=%u offset=%u\n",bookIdentity.c_str(),(unsigned)pageNumber+1,offset);
  }
  uint32_t bootCode=screen==Screen::Reading?1:screen==Screen::Settings?2:0;
  resume_state::Record resume;resume.screen=uint8_t(bootCode);resume.settingsId=settingsSelected?settingsId:SettingsId::None;
  const String selected=selectedBook<sdBooks.size()?sdBooks[selectedBook]:String();
  if(selected.length()>resume_state::MAX_PATH){
    showError("Position not saved; try again");discardLoadingInput(false);return;
  }
  memcpy(resume.path,selected.c_str(),selected.length()+1);
  if(!resume_state::save(prefs,resume)){
    showError("Position not saved; try again");discardLoadingInput(false);return;
  }
  networkStop();networkMode=NetworkMode::Off;
  showShutdownOverlay();
#if defined(PAPERDIY_DIAGNOSTICS) && defined(PAPERDIY_DEVICE_TESTS)
  devicePowerFinish();
#endif
  SD.end();saveBatteryHistory();prefs.end();
  Serial.println("SHUTDOWN display_complete=1");Serial.flush();
  M5.Power.powerOff();
}
void openSelected(){
  if(settingsSelected){screen=Screen::Settings;showSettings(true);return;}
  if(!leaveReading())return;
  if(!sdOK){showLoading();scanSD();library();discardLoadingInput();return;}
  if(selectedBook>=sdBooks.size())return;
  screen=Screen::Loading;showLoading();
  if(openSD(selectedBook)){screen=Screen::Reading;saveReadingProgress();showBook(true);}
  else {screen=Screen::Error;showError(sdError.length()?sdError:"Cannot open book");errorUntil=millis()+3500;}
  discardLoadingInput();
}
void dispatch(controls::Action action){
  using controls::Action;
  if(action==Action::None||screen==Screen::Loading)return;
  powerActivity();
  if(action==Action::Shutdown){Serial.println("SHUTDOWN_TRIGGER keys=A+B");shutdown();return;}
  if(screen==Screen::Settings){
    if(action==Action::Confirm){library();return;}
    if(action==Action::Previous){
      NetworkMode next=nextNetworkMode(networkMode);
      networkMode=next;
      networkApply(next);
      showSettings(true);
      return;
    }
    if(action==Action::Next){
      const bool rotated=!settingsRotated;
      if(!resume_state::saveRotation(prefs,rotated)){
        showError("Setting not saved; try again");return;
      }
      settingsRotated=rotated;
      displaySetRotated(settingsRotated);
      showSettings(true);return;
    }
    return;
  }
  if(screen==Screen::Error){library();return;}
  if(action==Action::Confirm){if(screen==Screen::Library)openSelected();else library();return;}
  int direction=(action==Action::Previous||action==Action::PreviousScreen)?-1:1;
  if(screen==Screen::Library){
    size_t old=selectedBook;bool oldSettings=settingsSelected;const auto oldId=settingsId;
    size_t cursor=settings_registry::libraryCursor(selectedBook,settingsSelected,settingsId);
    bool whole=action==Action::PreviousScreen||action==Action::NextScreen;
    cursor=controls::moveSelection(cursor,sdBooks.size()+settings_registry::COUNT,direction,whole);
    settingsSelected=cursor<settings_registry::COUNT;
    if(settingsSelected)settingsId=settings_registry::ENTRIES[cursor].id;
    else selectedBook=cursor-settings_registry::COUNT;
    updateLibrarySelection(old,oldSettings,oldId);
  }else if(screen==Screen::Reading&&!pageOffsets.empty()){
    uint32_t before=pageNumber;
    if(direction<0&&pageNumber>0)--pageNumber;
    if(direction>0&&pageNumber+1<pageOffsets.size())++pageNumber;
    if(before!=pageNumber){saveReadingProgress();showBook();}
  }
}
#ifdef PAPERDIY_DIAGNOSTICS
// Explicit opt-in diagnostics share the real navigation paths. No writes to
// books, arbitrary file access, network service, or production serial parser.
void diagnosticCommand(const String& line){
  using controls::Action;
#ifdef PAPERDIY_NETWORK_TESTS
  if(network_device_test::command(line))return;
#endif
#ifdef PAPERDIY_DEVICE_TESTS
  if(deviceMarkdownCommand(line,[](){
    if(screen==Screen::Reading)showBook(true);
    else if(screen==Screen::Settings)showSettings(true);
    else showLibrary(true);
  })){discardLoadingInput();return;}
  if(line.startsWith("SD_IO ")||line.startsWith("SD_CACHE_IO ")){
    const bool cachePrefix=line.startsWith("SD_CACHE_IO ");
    char* end=nullptr;const char* number=line.c_str()+(cachePrefix?12:6);
    unsigned long index=strtoul(number,&end,10);
    if(end!=number&&*end==0&&index<sdBooks.size())sd_io_device_test::run(index,0,cachePrefix);
    else Serial.println("SD_IO_REJECT index=1");
    discardLoadingInput();return;
  }
  if(devicePowerCommand(line))return;
  if(line=="TYPESET_CHECKS"){deviceTypographyChecks();return;}
  if(line=="XML_CHECKS"){deviceEpubChecks();discardLoadingInput();return;}
#endif
  if(line=="POWER_STATUS")logIdlePowerStatus();
  else if(line=="STATUS"){
    Serial.printf("STATUS screen=%u sd=%d files=%u selected=%u page=%u total=%u offset=%u key=%s save_failed=%d free_psram=%u stack_free=%u charging=%d english_hyphens=%d settings=%d mode=%u rotated=%d ap_client=%d sta_configured=%d sta_connected=%d network_active=%d anchor_offset=%u settings_id=%u cn_style=%u\n",
      unsigned(screen),sdOK,unsigned(sdBooks.size()),unsigned(selectedBook),
      unsigned(pageNumber+1),unsigned(pageOffsets.size()),
      pageOffsets.empty()?0:pageOffsets[pageNumber],bookmarkKey.c_str(),progressSaveFailed,
      ESP.getFreePsram(),unsigned(uxTaskGetStackHighWaterMark(nullptr)),chargingNow(),bookEnglishHyphens,
      settingsSelected?1:0,unsigned(networkMode),settingsRotated?1:0,
      settingsApClientConnected?1:0,settingsStaConfigured?1:0,settingsStaConnected?1:0,networkActive()?1:0,
      unsigned(pageOffsets.empty()?0:currentReadingOffset()),
      unsigned(settingsSelected?settingsId:SettingsId::None),unsigned(reader_typography::style.flags()));
    logDisplayStatus();
  }else if(line=="SCREEN"){
    M5.Display.waitDisplay();Serial.println("SCREEN 540 960 68");
    uint16_t pixels[540];uint8_t packed[68];
    for(int y=0;y<960;++y){
      M5.Display.readRect(0,y,540,1,pixels);memset(packed,255,sizeof(packed));
      for(int x=0;x<540;++x)if(pixels[x]==0)packed[x/8]&=~(0x80>>(x%8));
      Serial.write(packed,sizeof(packed));
    }
    Serial.println("SCREEN_DONE");
  }else if(line=="NEXT")dispatch(Action::Next);
  else if(line=="CACHEINFO")logSDCacheInfo();
  else if(line=="PREV")dispatch(Action::Previous);
  else if(line=="ENTER")dispatch(Action::Confirm);
  else if(line=="LIBRARY")library();
  else if(line=="RESCAN"){showLoading();scanSD();library();discardLoadingInput();}
  else if(line=="SHUTDOWN_PREVIEW")showShutdownOverlay();
  else if(line=="REFRESH"){if(screen==Screen::Reading)showBook(true);else library();}
  else if(line=="PAGE_TEXT"&&!pageOffsets.empty()){
    size_t start=pageOffsets[pageNumber],end=pageNumber+1<pageOffsets.size()?pageOffsets[pageNumber+1]:bookLength;
    Serial.printf("PAGE_TEXT bytes=%u\n",unsigned(end-start));
    Serial.write((const uint8_t*)book+start,end-start);Serial.println("\nPAGE_TEXT_DONE");
  }else if(line.startsWith("OPEN ")){
    char* end=nullptr;const char* number=line.c_str()+5;
    unsigned long n=strtoul(number,&end,10);
    if(end!=number&&*end==0&&n<sdBooks.size()){settingsSelected=false;selectedBook=n;openSelected();}
  }
}
void diagnostics(){
  static String line;
  static bool overflow=false;
  while(Serial.available()){
    char c=Serial.read();
    if(c=='\n'){
      if(!overflow){
        // This probe observes the idle frequency without waking the CPU into
        // the active profile. Commands that do work restore full speed first.
        if(line!="POWER_STATUS")powerActivity();
        diagnosticCommand(line);
        // End with a separate short USB packet. A reply whose size is a
        // multiple of 64 can otherwise stay buffered by the host until more
        // output arrives; draining the software ring alone is insufficient.
        Serial.flush();
        Serial.write('\n');
        Serial.flush();
      }
      line="";overflow=false;
    }else if(c!='\r'){
      if(line.length()<64)line+=c;else overflow=true;
    }
  }
}
#endif
}
void appSetup(){
  Serial.begin(115200);delay(300);
  auto cfg=M5.config();cfg.clear_display=false;cfg.internal_imu=false;cfg.internal_rtc=false;cfg.internal_mic=false;cfg.internal_spk=false;
  cfg.pmic_button=true;cfg.fallback_board=m5::board_t::board_M5PaperDIY;M5.begin(cfg);displayInit();
  Serial.printf("BOOT PaperDIY-Reader " PAPERDIY_VERSION " board=%d screen=%dx%d psram=%u network=off\n",M5.getBoard(),M5.Display.width(),M5.Display.height(),ESP.getPsramSize());
  if(M5.getBoard()!=m5::board_t::board_M5PaperDIY||M5.Display.width()!=540||M5.Display.height()!=960){Serial.println("FATAL board/display mismatch");while(true)delay(1000);}
  // Keep the original namespace and bookmark keys: app-only upgrades preserve progress.
  if(initializeProgress())reader_settings::load(prefs);
  book=(char*)ps_malloc(MAX_BOOK+1);heap_caps_malloc_extmem_enable(2048);
  networkMode=NetworkMode::Off;settingsSelected=false;settingsId=SettingsId::Network;
  settingsRotated=prefs.getUInt("rot180",0)!=0;
  displaySetRotated(settingsRotated);
  updatePowerIndicator(true);
  if(!book){showError("Not enough memory");while(true)delay(1000);}
  showLoading();
  scanSD();
  cardLevel=pendingCardLevel=digitalRead(48);
  String lastBook=prefs.getString("sdLast","");
  size_t lastIndex=sdBooks.size();
  for(size_t i=0;i<sdBooks.size();++i)if(sdBooks[i]==lastBook){lastIndex=i;break;}
  bool canResume=sdOK&&!lastBook.isEmpty()&&lastIndex<sdBooks.size();
  resume_state::Record resume;
  const auto resumeResult=resume_state::read(prefs,resume);
  uint32_t bootCode=resumeResult==resume_state::ReadResult::Valid?resume.screen:
    resumeResult==resume_state::ReadResult::Absent?prefs.getUInt("bootScr",0):0;
  auto restoreLibrarySelection=[&](){
    if(resumeResult!=resume_state::ReadResult::Absent){
      selectedBook=resumeResult==resume_state::ReadResult::Valid?resume_state::selectedIndex(resume,sdBooks):0;
      settingsSelected=resumeResult==resume_state::ReadResult::Valid&&resume.settingsId!=SettingsId::None;
      settingsId=settingsSelected?resume.settingsId:SettingsId::Network;
      return;
    }
    uint32_t savedBook=prefs.getUInt("selBook",lastIndex<sdBooks.size()?lastIndex:0);
    bool savedSet=prefs.getUInt("selSet",0)!=0;
    if(savedBook>=sdBooks.size()){savedBook=0;savedSet=false;}
    if(savedSet){selectedBook=0;settingsSelected=true;settingsId=SettingsId::Network;}
    else {selectedBook=savedBook;settingsSelected=false;}
  };
  if(bootCode==1&&canResume){
    selectedBook=lastIndex;
    // Keep the physical sleep image visible while loading; only the footer
    // busy icon is refreshed, then the original reading page replaces it.
    screen=Screen::Loading;showLoading();
    Serial.printf("BOOT_RESUME_BEGIN book=%s\n",lastBook.c_str());
    if(openSD(lastIndex)){
      screen=Screen::Reading;saveReadingProgress();showBook(true);
      Serial.printf("BOOT_RESUMED page=%u offset=%u anchor_offset=%u\n",(unsigned)pageNumber+1,pageOffsets[pageNumber],currentReadingOffset());
    }else {
      Serial.printf("BOOT_RESUME_FAILED error=%s\n",sdError.c_str());
      restoreLibrarySelection();screen=Screen::Library;showLibrary(true);
    }
  }else if(bootCode==2){
    restoreLibrarySelection();settingsSelected=true;
    screen=Screen::Settings;showSettings(true);
    Serial.println("BOOT_SCREEN settings");
  }else{
    restoreLibrarySelection();screen=Screen::Library;showLibrary(true);
    Serial.printf("BOOT_SCREEN library selected=%u settings=%d\n",(unsigned)selectedBook,settingsSelected?1:0);
  }
  discardLoadingInput(false);lastBatteryRefresh=millis();
  M5.Power.M5pm1.clearButtonIRQStatus();
  bool keyEnabled=M5.Power.M5pm1.setButtonIRQMaskBits(0);
  Serial.printf("POWER_KEY enabled=%d mask=%02x\n",keyEnabled,M5.Power.M5pm1.readRegister8(0x45));
  Serial.println("READER_READY rows=22; A/B navigate, hold to repeat, A+B confirm/back");
  powerActivity();
}
void appLoop(){
  if(rawPowerKeys())powerActivity();
  M5.update();updatePowerIndicator();
  if(shutdownRequested||M5.BtnPWR.wasClicked()){Serial.println("SHUTDOWN_TRIGGER pmic=1");shutdown();return;}
  auto action=buttons.update(keyMask(),millis(),screen==Screen::Library,screen!=Screen::Settings);
#if defined(PAPERDIY_DIAGNOSTICS) && defined(PAPERDIY_DEVICE_TESTS)
  devicePowerAction(action);
#endif
  dispatch(action);
  if(screen==Screen::Settings&&settingsId==SettingsId::Network){
    if(networkMode==NetworkMode::Ap){
      if(networkApExpired()){
        networkMode=NetworkMode::Off;
        networkStop();
        showSettings(true);
      }else if(networkPoll())updateSettingsNetwork();
    }else if(networkMode==NetworkMode::Sta){
      if(networkPoll())updateSettingsNetwork();
    }
  }
  if(millis()-lastCardCheck>=250){
    lastCardCheck=millis();int level=digitalRead(48);
    if(level!=cardLevel)powerActivity();
    if(level==pendingCardLevel&&level!=cardLevel){
      networkStorageChanged();
      cardLevel=level;sdOK=false;SD.end();sdBooks.clear();selectedBook=0;settingsSelected=screen==Screen::Settings;
      if(screen==Screen::Library||screen==Screen::Settings){
        if(level==LOW){
          if(screen==Screen::Library)showLoading();
          scanSD();networkStorageChanged();
        }
        if(screen==Screen::Library)showLibrary();
        discardLoadingInput();
      }
    }
    pendingCardLevel=level;
  }
  if(screen==Screen::Error&&(int32_t)(millis()-errorUntil)>=0)library();
  if(screen==Screen::Library&&millis()-lastBatteryRefresh>=60000){lastBatteryRefresh=millis();updateLibraryBattery();}
#ifdef PAPERDIY_DIAGNOSTICS
  diagnostics();
#ifdef PAPERDIY_DEVICE_TESTS
  devicePowerPoll();
#endif
#endif
  idlePower((screen==Screen::Library||screen==Screen::Reading||screen==Screen::Settings)&&
      pendingCardLevel==cardLevel,buttons.idle(),cardLevel,networkActive());
}
