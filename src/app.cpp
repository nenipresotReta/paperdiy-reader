#include <Arduino.h>
#include <M5Unified.h>
#include "app.h"
#include "reader.h"
#include "sd_reader.h"
#include "input.h"
#include "display_driver.h"
#include "power.h"
#include "ui.h"
#include <SD.h>
namespace {
enum class Screen {Library,Reading,Loading,Error};
Screen screen=Screen::Library;
controls::Buttons buttons;
uint32_t errorUntil=0,lastBatteryRefresh=0;
bool shutdownRequested=false;
uint8_t keyMask(){return (M5.BtnA.isPressed()?1:0)|(M5.BtnB.isPressed()?2:0);}
void discardLoadingInput(bool acceptPowerRequest=true){
  M5.update();buttons.discardUntilReleased(millis());
  // A/B navigation is discarded; a power-key click still requests a clean
  // shutdown once synchronous loading has finished.
  if(acceptPowerRequest&&M5.BtnPWR.wasClicked())shutdownRequested=true;
  Serial.println("INPUT_DISCARDED release_guard_ms=200");
}
void library(bool scene=true){screen=Screen::Library;showLibrary(scene);}
void shutdown(){
  if(screen==Screen::Reading&&!pageOffsets.empty()){
    // Snapshot the current book and text offset before drawing the sleep image.
    prefs.putString("sdLast",bookIdentity);
    uint32_t offset=pageOffsets[pageNumber];
    if(prefs.getUInt(bookmarkKey.c_str(),UINT32_MAX)!=offset)prefs.putUInt(bookmarkKey.c_str(),offset);
    Serial.printf("RESUME_SAVED book=%s page=%u offset=%u\n",bookIdentity.c_str(),(unsigned)pageNumber+1,offset);
  }
  showShutdownOverlay();
  SD.end();prefs.end();
  Serial.println("SHUTDOWN display_complete=1");Serial.flush();
  M5.Power.powerOff();
}
void openSelected(){
  if(!sdOK){showLoading();scanSD();library();discardLoadingInput();return;}
  if(selectedBook>=sdBooks.size())return;
  screen=Screen::Loading;showLoading();
  if(openSD(selectedBook)){screen=Screen::Reading;showBook(true);}
  else {screen=Screen::Error;showError(sdError.length()?sdError:"Cannot open book");errorUntil=millis()+3500;}
  discardLoadingInput();
}
void dispatch(controls::Action action){
  using controls::Action;
  if(action==Action::None||screen==Screen::Loading)return;
  if(action==Action::Shutdown){Serial.println("SHUTDOWN_TRIGGER keys=A+B");shutdown();return;}
  if(screen==Screen::Error){library();return;}
  if(action==Action::Confirm){if(screen==Screen::Library)openSelected();else library();return;}
  int direction=(action==Action::Previous||action==Action::PreviousScreen)?-1:1;
  if(screen==Screen::Library){
    size_t old=selectedBook;bool whole=action==Action::PreviousScreen||action==Action::NextScreen;
    selectedBook=controls::moveSelection(selectedBook,sdBooks.size(),direction,whole);updateLibrarySelection(old);
  }else if(screen==Screen::Reading&&!pageOffsets.empty()){
    uint32_t before=pageNumber;
    if(direction<0&&pageNumber>0)--pageNumber;
    if(direction>0&&pageNumber+1<pageOffsets.size())++pageNumber;
    if(before!=pageNumber)showBook();
  }
}

}
void appSetup(){
  Serial.begin(115200);delay(300);
  auto cfg=M5.config();cfg.clear_display=false;cfg.internal_imu=false;cfg.internal_rtc=false;cfg.internal_mic=false;cfg.internal_spk=false;
  cfg.pmic_button=true;cfg.fallback_board=m5::board_t::board_M5PaperDIY;M5.begin(cfg);displayInit();
  Serial.printf("BOOT PaperDIY-Reader 0.1.0 board=%d screen=%dx%d psram=%u network=off\n",M5.getBoard(),M5.Display.width(),M5.Display.height(),ESP.getPsramSize());
  if(M5.getBoard()!=m5::board_t::board_M5PaperDIY||M5.Display.width()!=540||M5.Display.height()!=960){Serial.println("FATAL board/display mismatch");while(true)delay(1000);}
  // Keep the original namespace and bookmark keys: app-only upgrades preserve progress.
  prefs.begin("paperwifi",false);book=(char*)ps_malloc(MAX_BOOK+1);heap_caps_malloc_extmem_enable(2048);
  updatePowerIndicator(true);
  if(!book){showError("Not enough memory");while(true)delay(1000);}
  scanSD();
  String lastBook=prefs.getString("sdLast","");
  bool canResume=sdOK&&!lastBook.isEmpty()&&selectedBook<sdBooks.size()&&sdBooks[selectedBook]==lastBook;
  if(canResume){
    // Keep the physical sleep image visible while loading; only the footer
    // busy icon is refreshed, then the original reading page replaces it.
    screen=Screen::Loading;showLoading();
    Serial.printf("BOOT_RESUME_BEGIN book=%s\n",lastBook.c_str());
    if(openSD(selectedBook)){
      screen=Screen::Reading;showBook(true);
      Serial.printf("BOOT_RESUMED page=%u offset=%u\n",(unsigned)pageNumber+1,pageOffsets[pageNumber]);
    }else {Serial.printf("BOOT_RESUME_FAILED error=%s\n",sdError.c_str());library();}
  }else library();
  discardLoadingInput(false);lastBatteryRefresh=millis();
  M5.Power.M5pm1.clearButtonIRQStatus();
  bool keyEnabled=M5.Power.M5pm1.setButtonIRQMaskBits(0);
  Serial.printf("POWER_KEY enabled=%d mask=%02x\n",keyEnabled,M5.Power.M5pm1.readRegister8(0x45));
  Serial.println("READER_READY rows=22; A/B navigate, hold to repeat, A+B confirm/back");
}
void appLoop(){
  M5.update();updatePowerIndicator();
  if(shutdownRequested||M5.BtnPWR.wasClicked()){Serial.println("SHUTDOWN_TRIGGER pmic=1");shutdown();return;}
  dispatch(buttons.update(keyMask(),millis(),screen==Screen::Library));
  if(screen==Screen::Error&&(int32_t)(millis()-errorUntil)>=0)library();
  if(screen==Screen::Library&&millis()-lastBatteryRefresh>=60000){lastBatteryRefresh=millis();updateLibraryBattery();}
  delay(10);
}
