#include "console.h"
#include "../tests/device_loop_trace.h"
#include <Arduino.h>
#include <M5Unified.h>
#include "app.h"
#include "version.h"
#include "reader.h"
#include "sd_reader.h"
#include "input.h"
#include "display_driver.h"
#include "power.h"
#include "idle_policy.h"
#include "ui.h"
#include "network.h"
#include "resume_store.h"
#include "reader_typography.h"
#include "storage_validation.h"
#include "usb_storage.h"
#include "usb_transport.h"
#include "lan_service.h"
#include <SD.h>
#include <esp_heap_caps.h>
#if defined(PAPERDIY_DIAGNOSTICS) && defined(PAPERDIY_NETWORK_TESTS)
#include "../tests/device_network.h"
#endif
#if defined(PAPERDIY_DIAGNOSTICS) && defined(PAPERDIY_DEVICE_TESTS)
#include "../tests/device_epub.h"
#include "../tests/device_power.h"
#include "../tests/device_typography.h"
#include "../tests/device_markdown.h"
#include "../tests/device_sd_io.h"
#include "../tests/device_sd_audit.h"
#ifdef PAPERDIY_ACCEPTANCE_FIXTURE_HEADER
#include "../tests/device_sd_fixture.h"
#endif
#endif
#if defined(PAPERDIY_DIAGNOSTICS) && defined(PAPERDIY_EPUB_TESTS)
#include "../tests/device_epub_render.h"
#endif
namespace {
enum class Screen {Library,Reading,Loading,Error,Settings};
Screen screen=Screen::Library;
controls::Buttons buttons;
uint32_t errorUntil=0,lastBatteryRefresh=0;
uint32_t lastCardCheck=0;
int cardLevel=HIGH,pendingCardLevel=HIGH;
bool shutdownRequested=false;
bool usbWaiting=false;
uint64_t usbLastWritten=0;
uint64_t usbLastRead=0;
uint32_t usbLastWriteAt=0;
bool releaseUsb(){
  if(!sdOwnedByUsb()){usbWaiting=false;return true;}
  usb_storage::poll();
  if(usb_storage::hostOwnsCard()){
    usb_storage::requestExit();settingsUsbStatus="Eject on computer";
    if(screen==Screen::Settings)updateSettingsNetwork();
    return false;
  }
  if(!usb_storage::finish())return false;
  usb_transport::setStorageOwned(false);
  usbWaiting=false;
  settingsUsbStatus=endUsbStorageHandoff()?"Ejected":"SD card mount failed";
  lan_service::storageChanged();
  return true;
}
void beginUsb(){
  if(sdOwnedByUsb()||!usbWaiting||!usb_transport::hostConnected())return;
  // U may have been armed on battery before a host arrives. Raise the clock
  // before SD teardown/raw initialization and the first advertised callback.
  powerActivity();
  const uint32_t frequency=sdStorageFrequency();
  lan_service::storageStop();
  usb_transport::setStorageOwned(true);
  usb_storage::diagnosticMark(usb_storage::DiagnosticStage::Prepare);
  if(!beginUsbStorageHandoff()){
    usb_transport::setStorageOwned(false);
    usbWaiting=false;settingsUsbStatus="Cannot release SD";return;
  }
  if(!usb_storage::begin(frequency)){
    if(!usb_storage::hostOwnsCard()){
      usb_storage::finish();usb_transport::setStorageOwned(false);endUsbStorageHandoff();
      lan_service::storageChanged();
    }
    usbWaiting=false;settingsUsbStatus="USB storage unavailable";return;
  }
  usbWaiting=false;usbLastWritten=0;usbLastRead=0;usbLastWriteAt=0;
  settingsUsbStatus=usb_storage::ramTestMode()?"Test disk connected":"Connected";
}
void pollUsb(){
  if(networkMode!=NetworkMode::Usb&&!sdOwnedByUsb())return;
  const String before=settingsUsbStatus;
  beginUsb();usb_storage::poll();
  const auto state=usb_storage::status();
  if(state.bytesWritten!=usbLastWritten||state.bytesRead!=usbLastRead)powerActivity();
  usbLastRead=state.bytesRead;
  if(state.bytesWritten!=usbLastWritten){usbLastWritten=state.bytesWritten;usbLastWriteAt=millis();}
  if(state.state==usb_storage::State::Ejected&&!usb_storage::hostOwnsCard()){
    releaseUsb();
  }else if(state.state==usb_storage::State::Fault)settingsUsbStatus="Storage error; eject";
  else if(state.state==usb_storage::State::ExitRequested)settingsUsbStatus="Eject on computer";
  else if(state.state==usb_storage::State::HostOwned){
    settingsUsbStatus=usbLastWriteAt&&uint32_t(millis()-usbLastWriteAt)<1000?"Writing":
      !usb_transport::hostConnected()?"Eject on computer":usb_storage::ramTestMode()?"Test disk connected":"Connected";
  }
  if(before!=settingsUsbStatus&&screen==Screen::Settings)updateSettingsNetwork();
}
void IRAM_ATTR cardChanged(){storage_validation::signalCardEvent();}
uint8_t keyMask(){
  uint8_t mask=(M5.BtnA.isPressed()?1:0)|(M5.BtnB.isPressed()?2:0);
  return controls::navigationMask(mask,settingsRotated,screen==Screen::Settings);
}
bool loadingCancelProbe(){
  if(screen!=Screen::Loading)return false;
  // The importer calls this on the application task while processing a
  // chapter. Observe input only; the job closes its own handles and commits
  // a verified chapter boundary before cancellation becomes terminal.
  M5.update();
  const auto action=buttons.update(keyMask(),millis(),false,false);
  if(action==controls::Action::Shutdown||M5.BtnPWR.wasClicked())shutdownRequested=true;
  return shutdownRequested;
}
void logUsbDiagnostics(){
  const auto d=usb_storage::diagnostics();
  Console.printf("USB_DIAG reset=%u previous_valid=%u previous_stage=%s previous_reads=%u previous_writes=%u previous_stack=%u stage=%s reads=%u writes=%u stack=%u\n",
    unsigned(d.resetReason),unsigned(d.previousValid),
    usb_storage::diagnosticStageName(usb_storage::DiagnosticStage(d.previous.stage)),
    unsigned(d.previous.readCalls),unsigned(d.previous.writeCalls),unsigned(d.previous.usbStackLow),
    usb_storage::diagnosticStageName(usb_storage::DiagnosticStage(d.current.stage)),
    unsigned(d.current.readCalls),unsigned(d.current.writeCalls),unsigned(d.current.usbStackLow));
}
void discardLoadingInput(bool acceptPowerRequest=true){
  M5.update();buttons.discardUntilReleased(millis());
#ifdef PAPERDIY_DIAGNOSTICS
  while(Serial.available())Serial.read();
#endif
  // A/B navigation is discarded; a power-key click still requests a clean
  // shutdown once synchronous loading has finished.
  if(acceptPowerRequest&&M5.BtnPWR.wasClicked())shutdownRequested=true;
  Console.println("INPUT_DISCARDED release_guard_ms=200");
}
bool leaveReading(){
  if(screen!=Screen::Reading||saveReadingProgress())return true;
  showError("Progress not saved; try again");return false;
}
void library(bool scene=true){
  if(!releaseUsb())return;
  networkStop();
  networkMode=NetworkMode::Off;
  if(!leaveReading())return;
  if(!sdOK&&digitalRead(48)==LOW){showLoading();scanSD();lan_service::storageChanged();}
  lan_service::storagePoll();lan_service::refreshLibrary();
  screen=Screen::Library;showLibrary(scene);
}
void shutdown(){
  powerActivity();
  shutdownRequested=false;
  if(!releaseUsb()){discardLoadingInput(false);return;}
  if(screen==Screen::Reading&&!pageOffsets.empty()){
    // Snapshot the current book and text offset before drawing the sleep image.
    uint32_t offset=currentReadingOffset();
    if(!saveReadingProgress()){
      showError("Progress not saved; try again");discardLoadingInput(false);return;
    }
    Console.printf("RESUME_SAVED book=%s page=%u offset=%u\n",bookIdentity.c_str(),(unsigned)pageNumber+1,offset);
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
  cancelSDBackground();lan_service::storageStop();
  showShutdownOverlay();
#if defined(PAPERDIY_DIAGNOSTICS) && defined(PAPERDIY_DEVICE_TESTS)
  devicePowerFinish();
#endif
  storage_validation::invalidate();SD.end();saveBatteryHistory();prefs.end();
  Console.println("SHUTDOWN display_complete=1");Console.flush();
  M5.Power.powerOff();
}
void finishOpen(SDOpenResult result){
  if(result==SDOpenResult::Pending)return;
  if(result==SDOpenResult::Ready){screen=Screen::Reading;saveReadingProgress();showBook(true);}
  else if(result==SDOpenResult::Cancelled){screen=Screen::Library;showLibrary(true);}
  else {screen=Screen::Error;showError(sdError.length()?sdError:"Cannot open book");errorUntil=millis()+3500;}
  discardLoadingInput();
}
void openSelected(bool forceReflow=false){
  if(sdOwnedByUsb()||screen==Screen::Loading||openSDPending())return;
  if(settingsSelected){screen=Screen::Settings;showSettings(true);return;}
  if(!leaveReading())return;
  if(!sdOK){showLoading();scanSD();library();discardLoadingInput();return;}
  if(selectedBook>=sdBooks.size())return;
  networkStop();networkMode=NetworkMode::Off;
  screen=Screen::Loading;showLoading();
  finishOpen(beginOpenSD(selectedBook,true,forceReflow));
}
void dispatch(controls::Action action){
  using controls::Action;
  if(action==Action::None||screen==Screen::Loading)return;
  powerActivity();
  if(action==Action::Shutdown){Console.println("SHUTDOWN_TRIGGER keys=A+B");shutdown();return;}
  if(screen==Screen::Settings){
    // While a delivery prompt is shown, short A+B accepts it instead of leaving
    // Settings. A (next mode) and leaving Settings stop N, which cancels it.
    if(action==Action::Confirm&&lan_service::promptPending()){
      lan_service::acceptPrompt();updateSettingsNetwork();return;
    }
    if(action==Action::Confirm){library();return;}
    if(action==Action::Previous){
      NetworkMode next=nextNetworkMode(networkMode);
      if(networkMode==NetworkMode::Usb&&!releaseUsb())return;
      networkStop();
      cancelContentPrefetch();
      networkMode=next;
      if(next==NetworkMode::Usb){
        usbWaiting=usb_storage::available();
        settingsUsbStatus=usbWaiting?"Connect USB":"USB storage unavailable";
        beginUsb();
      }else networkApply(next);
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
    if(turnReaderPage(direction)){saveReadingProgress();showBook();}
    else if(reader_content::leaseInvalidated()){
      // A page-window source whose lease expired after sleep or a storage
      // change cannot read the next block. Reopen at the saved anchor rather
      // than leaving the page turn silently stuck.
      Console.println("READER_REOPEN reason=stale_lease");
      openSelected();
    }
  }
}
#ifdef PAPERDIY_DIAGNOSTICS
// Explicit opt-in diagnostics share the real navigation paths. Additional
// device tests and the fixed private fixture installer require separate flags.
void diagnosticCommand(const String& line){
  device_loop_trace::CommandScope traceCommand(line.c_str(),line.length());
  using controls::Action;
  if(screen==Screen::Loading&&line!="STATUS"&&line!="SCREEN"&&line!="POWER_STATUS"&&
      line!="USB_STATUS"&&line!="CANCEL_OPEN"){
    Console.println("INPUT_IGNORED loading=1");return;
  }
  if(line=="CANCEL_OPEN"){
    if(openSDPending())cancelOpenSD();
    Console.println("OPEN_CANCEL_REQUESTED");return;
  }
  if(line=="USB_STATUS"){
    const auto state=usb_storage::status();
    Console.printf("USB_STATUS supported=%u ram=%u state=%u owned=%u connected=%u read=%llu written=%llu error=%u first_error=%u backend_reason=%s card_event=%u card_present=%u\n",
      unsigned(usb_storage::available()),unsigned(usb_storage::ramTestMode()),unsigned(state.state),
      unsigned(sdOwnedByUsb()),unsigned(usb_transport::hostConnected()),
      (unsigned long long)state.bytesRead,(unsigned long long)state.bytesWritten,unsigned(state.error),
      unsigned(state.firstError),usb_storage::backendReasonName(state.backendReason),
      unsigned(storage_validation::cardEventPending()),unsigned(digitalRead(48)==LOW));
    logUsbDiagnostics();return;
  }
  if(line=="LAN_STATUS"){lan_service::printStatus();return;}
  if(line=="USB_ENTER"){
    if(!usb_storage::available()){Console.println("USB_DISABLED experimental=0");return;}
    if(!leaveReading())return;
    networkStop();settingsSelected=true;settingsId=SettingsId::Network;screen=Screen::Settings;
    networkMode=NetworkMode::Usb;usbWaiting=usb_storage::available();
    settingsUsbStatus=usbWaiting?"Connect USB":"USB storage unavailable";beginUsb();showSettings(true);return;
  }
  if(line=="USB_EXIT"){if(releaseUsb()){networkStop();networkMode=NetworkMode::Off;showSettings(true);}return;}
  // A block-device host owns the entire card until safe eject. Even explicit
  // diagnostics may not access FAT, redraw SD artwork or remount it meanwhile.
  if(sdOwnedByUsb()&&line!="STATUS"&&line!="SCREEN"&&line!="POWER_STATUS"){
    Console.println("USB_BUSY eject_on_computer=1");return;
  }
#ifdef PAPERDIY_NETWORK_TESTS
  if(network_device_test::command(line))return;
#endif
#ifdef PAPERDIY_DEVICE_TESTS
  if(sd_audit_device::command(line))return;
#ifdef PAPERDIY_ACCEPTANCE_FIXTURE_HEADER
  if(sd_fixture_device::command(line,screen==Screen::Library))return;
#endif
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
    else Console.println("SD_IO_REJECT index=1");
    discardLoadingInput();return;
  }
  if(devicePowerCommand(line))return;
  if(line=="TYPESET_CHECKS"){deviceTypographyChecks();return;}
  if(line=="XML_CHECKS"){deviceEpubChecks();discardLoadingInput();return;}
#endif
#ifdef PAPERDIY_EPUB_TESTS
  if(line=="EPUB_CHECKS"){
    epub_device_test::run(0,false);epub_device_test::run(1,false);
    if(screen==Screen::Settings)showSettings(true);else if(screen==Screen::Reading)showBook(true);else showLibrary(true);
    discardLoadingInput();return;
  }
  if(line=="EPUB_PREVIEW 0"||line=="EPUB_PREVIEW 1"){
    epub_device_test::run(line.endsWith("1")?1:0,true);discardLoadingInput();return;
  }
#endif
  if(line=="POWER_STATUS")logIdlePowerStatus();
  else if(line=="STATUS"){
    Console.printf("STATUS screen=%u sd=%d files=%u selected=%u page=%u total=%u offset=%u key=%s save_failed=%d free_psram=%u stack_free=%u charging=%d english_hyphens=%d settings=%d mode=%u rotated=%d ap_client=%d sta_configured=%d sta_connected=%d network_active=%d anchor_offset=%u settings_id=%u cn_style=%u\n",
      unsigned(screen),sdOK,unsigned(sdBooks.size()),unsigned(selectedBook),
      unsigned(readerPageNumber()),unsigned(readerTotalPages()),
      pageOffsets.empty()?0:pageOffsets[pageNumber],bookmarkKey.c_str(),progressSaveFailed,
      ESP.getFreePsram(),unsigned(uxTaskGetStackHighWaterMark(nullptr)),chargingNow(),bookEnglishHyphens,
      settingsSelected?1:0,unsigned(networkMode),settingsRotated?1:0,
      settingsApClientConnected?1:0,settingsStaConfigured?1:0,settingsStaConnected?1:0,networkActive()?1:0,
      unsigned(pageOffsets.empty()?0:currentReadingOffset()),
      unsigned(settingsSelected?settingsId:SettingsId::None),unsigned(reader_typography::currentStyle().flags()));
    logDisplayStatus();
    Console.printf("MEMORY internal_free=%u internal_min=%u internal_largest=%u\n",
      unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
      unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
      unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    Console.printf("INDEX_STATUS pending=%u complete=%u provisional=%u failed=%u background=%u\n",
      unsigned(readerIndexPending()),unsigned(readerIndexComplete()),unsigned(readerHasProvisionalPages()),unsigned(readerIndexFailed()),unsigned(sdBackgroundPending()));
    Console.printf("PREFETCH_STATUS state=%s pending=%u failed=%u loaded=%u/%u read=%llu\n",
      contentPrefetchStateName(),unsigned(contentPrefetchPending()),unsigned(contentPrefetchFailed()),
      unsigned(reader_content::prefetchLoadedBlocks()),unsigned(reader_content::prefetchTotalBlocks()),
      (unsigned long long)reader_content::prefetchReadBytes());
    Console.printf("OPEN_STATUS pending=%u closing=%u\n",unsigned(openSDPending()),unsigned(sdOpenClosePending()));
  }else if(line=="SCREEN"){
    M5.Display.waitDisplay();
    paperdiy_console::Frame consoleFrame;
    Console.println("SCREEN 540 960 68");
    uint16_t pixels[540];uint8_t packed[68];
    for(int y=0;y<960&&consoleFrame.good();++y){
      M5.Display.readRect(0,y,540,1,pixels);memset(packed,255,sizeof(packed));
      for(int x=0;x<540;++x)if(pixels[x]==0)packed[x/8]&=~(0x80>>(x%8));
      Console.write(packed,sizeof(packed));
    }
    if(consoleFrame.good())Console.println("SCREEN_DONE");
    else {consoleFrame.finish();Console.println("SCREEN_ABORTED");}
  }else if(line=="NEXT")dispatch(Action::Next);
  else if(line=="CACHEINFO")logSDCacheInfo();
  else if(line=="PREV")dispatch(Action::Previous);
  else if(line=="ENTER")dispatch(Action::Confirm);
  else if(line=="LIBRARY")library();
  else if(line=="RESCAN"){showLoading();scanSD();library();discardLoadingInput();}
  else if(line=="SHUTDOWN_PREVIEW")showShutdownOverlay();
  else if(line=="REFRESH"){if(screen==Screen::Reading)showBook(true);else library();}
  else if(line=="PAGE_TEXT"&&!pageOffsets.empty()){
    size_t start=pageOffsets[pageNumber],end=currentPageEnd();
    if(end==UINT32_MAX||end<=start||end>bookLength){Console.println("PAGE_TEXT_INVALID");return;}
    paperdiy_console::Frame consoleFrame;
    Console.printf("PAGE_TEXT bytes=%u\n",unsigned(end-start));
    Console.write((const uint8_t*)book+start,end-start);
    if(consoleFrame.good())Console.println("\nPAGE_TEXT_DONE");
    else {consoleFrame.finish();Console.println("\nPAGE_TEXT_ABORTED");}
  }else if(line.startsWith("OPEN ")||line.startsWith("OPEN_REFLOW ")){
    const bool reflow=line.startsWith("OPEN_REFLOW ");
    char* end=nullptr;const char* number=line.c_str()+(reflow?12:5);
    unsigned long n=strtoul(number,&end,10);
    if(end!=number&&*end==0&&n<sdBooks.size()){settingsSelected=false;selectedBook=n;openSelected(reflow);}
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
        Console.flush();
        Console.write('\n');
        Console.flush();
      }
#ifdef PAPERDIY_DEVICE_TESTS
      else if(line.startsWith("SD_HASH "))sd_audit_device::reject("command_too_long");
#endif
      line="";overflow=false;
    }else if(c!='\r'){
#ifdef PAPERDIY_DEVICE_TESTS
      if(line.length()<sd_audit::commandLimit(line.c_str(),line.length()))line+=c;else overflow=true;
#else
      if(line.length()<64)line+=c;else overflow=true;
#endif
    }
  }
}
#endif
}
void appSetup(){
  device_loop_trace::mark(device_loop_trace::Stage::Boot);
  usb_storage::initializeDiagnostics();
  Serial.begin(115200);delay(300);
  auto cfg=M5.config();cfg.clear_display=false;cfg.internal_imu=false;cfg.internal_rtc=false;cfg.internal_mic=false;cfg.internal_spk=false;
  cfg.pmic_button=true;cfg.fallback_board=m5::board_t::board_M5PaperDIY;M5.begin(cfg);displayInit();
  Console.printf("BOOT PaperDIY-Reader " PAPERDIY_VERSION " board=%d screen=%dx%d psram=%u network=off\n",M5.getBoard(),M5.Display.width(),M5.Display.height(),ESP.getPsramSize());
  logUsbDiagnostics();
  if(M5.getBoard()!=m5::board_t::board_M5PaperDIY||M5.Display.width()!=540||M5.Display.height()!=960){Console.println("FATAL board/display mismatch");while(true)delay(1000);}
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
  lan_service::storageChanged();lan_service::storagePoll();
  attachInterrupt(digitalPinToInterrupt(48),cardChanged,CHANGE);
  cardLevel=pendingCardLevel=digitalRead(48);
  String lastBook=prefs.getString("sdLast","");
  if(lan_service::isPushPath(lastBook.c_str())){
    // Only the two short managed slots precede Push resume. Recovery of an
    // interrupted ordinary upload continues later in bounded background work.
    for(unsigned i=0;i<64&&!lan_service::pushReady();++i){lan_service::storagePoll();delay(1);}
    if(lan_service::currentPushPath()[0])lastBook=lan_service::currentPushPath();
  }
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
    Console.printf("BOOT_RESUME_BEGIN book=%s\n",lastBook.c_str());
    if(openSD(lastIndex)){
      screen=Screen::Reading;saveReadingProgress();showBook(true);
      Console.printf("BOOT_RESUMED page=%u offset=%u anchor_offset=%u\n",(unsigned)pageNumber+1,pageOffsets[pageNumber],currentReadingOffset());
    }else {
      Console.printf("BOOT_RESUME_FAILED error=%s\n",sdError.c_str());
      restoreLibrarySelection();screen=Screen::Library;showLibrary(true);
    }
  }else if(bootCode==2){
    restoreLibrarySelection();settingsSelected=true;
    screen=Screen::Settings;showSettings(true);
    Console.println("BOOT_SCREEN settings");
  }else{
    restoreLibrarySelection();screen=Screen::Library;showLibrary(true);
    Console.printf("BOOT_SCREEN library selected=%u settings=%d\n",(unsigned)selectedBook,settingsSelected?1:0);
  }
  discardLoadingInput(false);lastBatteryRefresh=millis();
  M5.Power.M5pm1.clearButtonIRQStatus();
  bool keyEnabled=M5.Power.M5pm1.setButtonIRQMaskBits(0);
  Console.printf("POWER_KEY enabled=%d mask=%02x\n",keyEnabled,M5.Power.M5pm1.readRegister8(0x45));
  Console.println("READER_READY rows=22; A/B navigate, hold to repeat, A+B confirm/back");
  setSDOpenCancelProbe(loadingCancelProbe);
  powerActivity();
}
void appLoop(){
  using device_loop_trace::Stage;
  device_loop_trace::mark(Stage::LoopBegin);
  if(rawPowerKeys())powerActivity();
  device_loop_trace::mark(Stage::BeforeM5);M5.update();device_loop_trace::mark(Stage::AfterM5);
  device_loop_trace::mark(Stage::BeforeIndicator);updatePowerIndicator();device_loop_trace::mark(Stage::AfterIndicator);
  if(M5.BtnPWR.wasClicked())shutdownRequested=true;
  if(shutdownRequested&&screen!=Screen::Loading){Console.println("SHUTDOWN_TRIGGER pmic=1");shutdown();return;}
  auto action=buttons.update(keyMask(),millis(),screen==Screen::Library,screen!=Screen::Settings);
#if defined(PAPERDIY_DIAGNOSTICS) && defined(PAPERDIY_DEVICE_TESTS)
  devicePowerAction(action);
#endif
  if(screen==Screen::Loading){
    if(action==controls::Action::Shutdown)shutdownRequested=true;
    if(shutdownRequested)cancelOpenSD();
    finishOpen(pollOpenSD());
    // Loading input is consumed here even when this step just completed. It
    // cannot turn into a page action or a second open after the job finishes.
    if(screen!=Screen::Loading&&shutdownRequested){shutdown();return;}
#ifdef PAPERDIY_DIAGNOSTICS
    if(screen==Screen::Loading){
      device_loop_trace::mark(Stage::BeforeDiagnostics);diagnostics();device_loop_trace::mark(Stage::AfterDiagnostics);
    }
#endif
    delay(1);return;
  }
  dispatch(action);
  // beginOpenSD may have started a cold import. Do not interleave library,
  // delivery recovery, remounts or pagination jobs with its owned handles.
  if(screen==Screen::Loading){
#ifdef PAPERDIY_DIAGNOSTICS
    device_loop_trace::mark(Stage::BeforeDiagnostics);diagnostics();device_loop_trace::mark(Stage::AfterDiagnostics);
#endif
    delay(1);return;
  }
  device_loop_trace::mark(Stage::BeforeUsb);pollUsb();device_loop_trace::mark(Stage::AfterUsb);
  device_loop_trace::mark(Stage::BeforeStorage);lan_service::storagePoll();device_loop_trace::mark(Stage::AfterStorage);
  if(contentPrefetchPending()&&(networkActive()||lan_service::storageBusy()))cancelContentPrefetch();
  if(lan_service::takeLibraryChanged()&&screen==Screen::Library)showLibrary(false);
  if(screen==Screen::Settings&&settingsId==SettingsId::Network){
    if(networkMode==NetworkMode::Ap){
      if(networkApExpired()){
        networkMode=NetworkMode::Off;
        networkStop();
        showSettings(true);
      }else {
        const bool changed=networkPoll(),delivery=networkDeliveryChanged();
        if(changed)updateSettingsNetwork();else if(delivery)updateSettingsDelivery();
      }
    }else if(networkMode==NetworkMode::Sta){
      const bool changed=networkPoll(),delivery=networkDeliveryChanged();
      if(changed)updateSettingsNetwork();else if(delivery)updateSettingsDelivery();
    }
  }
  if(!sdOwnedByUsb()&&millis()-lastCardCheck>=250){
    lastCardCheck=millis();int level=digitalRead(48);
    if(level!=cardLevel)powerActivity();
    if(level==pendingCardLevel&&(level!=cardLevel||storage_validation::cardEventPending())){
      networkStorageChanged();
      cancelSDBackground();
      storage_validation::invalidate();storage_validation::acknowledgeCardEvent();
      cardLevel=level;sdOK=false;SD.end();sdBooks.clear();selectedBook=0;settingsSelected=screen==Screen::Settings;
      if(level==LOW)resetSDSpeed();
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
  if(screen==Screen::Library&&millis()-lastBatteryRefresh>=60000){
    lastBatteryRefresh=millis();device_loop_trace::mark(Stage::BeforeBattery);
    updateLibraryBattery();device_loop_trace::mark(Stage::AfterBattery);
  }
#ifdef PAPERDIY_DIAGNOSTICS
  device_loop_trace::mark(Stage::BeforeDiagnostics);diagnostics();device_loop_trace::mark(Stage::AfterDiagnostics);
#ifdef PAPERDIY_DEVICE_TESTS
  devicePowerPoll();
#endif
#endif
  // Work runs only between foreground actions. Do not refresh the body when
  // canonical pagination catches up with an already-visible anchor window.
  if((screen==Screen::Reading||screen==Screen::Library)&&sdBackgroundPending()&&
      buttons.idle()&&!rawPowerKeys()){
    const uint32_t total=readerTotalPages();const bool busy=sdBackgroundPending();
    powerActivity();pollSDBackground();
    if(screen==Screen::Reading&&(total!=readerTotalPages()||busy!=sdBackgroundPending()))updateBookFooter();
  }
  if((screen==Screen::Reading||screen==Screen::Library||screen==Screen::Settings)&&
      contentPrefetchPending()&&!sdBackgroundPending()&&!lan_service::storageBusy()&&
      buttons.idle()&&!rawPowerKeys()){
    powerActivity();
    const bool failedBefore=contentPrefetchFailed();
    contentPrefetchStep();
    if(failedBefore!=contentPrefetchFailed()||!contentPrefetchPending()){
      if(screen==Screen::Reading)updateBookFooter();
      else if(contentPrefetchFailed())showPrefetchFailureBanner();
    }
  }
  if(screen==Screen::Error)cancelContentPrefetch();
  device_loop_trace::mark(Stage::BeforeIdle);
  idlePower((screen==Screen::Library||screen==Screen::Reading||screen==Screen::Settings)&&
      pendingCardLevel==cardLevel&&
      idle_power::backgroundAllowsSleep(sdBackgroundPending(),contentPrefetchPending(),screen==Screen::Settings)&&
      !lan_service::storageBusy(),buttons.idle(),cardLevel,networkActive()||usb_transport::blocksSleep());
  device_loop_trace::mark(Stage::AfterIdle);
}
