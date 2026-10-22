#include <M5Unified.h>
#include <algorithm>
#include "reader.h"
#include "reader_font.h"
#include "reading_font.h"
#include "sd_reader.h"
#include "input.h"
#include "display_driver.h"
#include "power.h"
#include "ui.h"
#ifdef PAPERDIY_SHUTDOWN_HEADER
#include PAPERDIY_SHUTDOWN_HEADER
#endif
bool settingsSelected=false,settingsNetwork=false,settingsRotated=false;
namespace {
constexpr int TOP=56,ROW=39;
constexpr int BATTERY_CELL_X=504,BATTERY_CELL_Y=8,BATTERY_CELL_W=28,BATTERY_CELL_H=28;
constexpr int BATTERY_X=506,BATTERY_Y=16,BATTERY_W=22,BATTERY_H=12;
constexpr int SETTINGS_LEFT=32,SETTINGS_TOP=641,SETTINGS_LINE=112,SETTINGS_W=44,SETTINGS_H=151;
int settingsDx(){return settingsRotated?540-SETTINGS_LEFT-SETTINGS_W-SETTINGS_LEFT:0;}
int settingsDy(){return settingsRotated?960-SETTINGS_TOP-SETTINGS_H-SETTINGS_TOP:0;}
int lastBattery=-2;
bool lastCharging=false;
int batteryHistory=-1;
bool batteryHistoryLoaded=false;
void loadBatteryHistory(){
  if(batteryHistoryLoaded)return;
  batteryHistoryLoaded=true;
  uint32_t value=prefs.getUInt("battLast",0xffffffffu);
  if(value<=100)batteryHistory=int(value);
}
int batteryLevelForDisplay(int raw,bool charging){
  loadBatteryHistory();
  if(!charging&&raw>=0&&raw<=100)batteryHistory=raw;
  return charging?batteryHistory:raw;
}
String fitName(const String& input,int width){
  String out;size_t pos=0;
  while(pos<input.length()){char c[5];size_t n=utf8Char(input.c_str()+pos,input.length()-pos,c);if(M5.Display.textWidth(out+c+"…")>width){out+="…";break;}out+=c;pos+=n;}
  return out;
}
void font(){M5.Display.setFont(readerFont());M5.Display.setTextSize(1);M5.Display.setTextColor(TFT_BLACK,TFT_WHITE);}
void bookName(const String& name,int x,int y,int width,uint32_t foreground){
  size_t pos=0;int used=0;
  while(pos<name.length()){
    char ch[5];size_t n=utf8Char(name.c_str()+pos,name.length()-pos,ch);
    uint32_t cp=readerCodepoint(ch,n);
    int advance=readerGlyphWidth(cp);
    if(used+advance+(pos+n<name.length()?24:0)>width){
      M5.Display.setFont(readerFont());M5.Display.setCursor(x+used,y);M5.Display.print("…");break;
    }
    drawReaderGlyph(ch,n,x+used,y,foreground);used+=advance;pos+=n;
  }
  M5.Display.setFont(readerFont());
}
void statusIcon(bool busy=false){
  M5.Display.fillRect(504,928,28,28,TFT_WHITE);
  if(busy){
    M5.Display.fillCircle(518,942,11,TFT_BLACK);
    M5.Display.drawLine(518,942,518,935,TFT_WHITE);M5.Display.drawLine(518,942,524,942,TFT_WHITE);
  }else if(progressSaveFailed){
    M5.Display.drawCircle(518,942,11,TFT_BLACK);
    M5.Display.fillRect(517,935,3,8,TFT_BLACK);M5.Display.fillRect(517,946,3,3,TFT_BLACK);
  }else {M5.Display.drawCircle(518,942,7,TFT_BLACK);M5.Display.fillCircle(518,942,3,TFT_BLACK);}
}
void battery(){
  int raw=M5.Power.getBatteryLevel();bool charging=chargingNow();
  int value=batteryLevelForDisplay(raw,charging);lastBattery=value;lastCharging=charging;
  M5.Display.fillRect(BATTERY_CELL_X,BATTERY_CELL_Y,BATTERY_CELL_W,BATTERY_CELL_H,TFT_WHITE);
  M5.Display.drawRect(BATTERY_X,BATTERY_Y,BATTERY_W,BATTERY_H,TFT_BLACK);
  M5.Display.fillRect(BATTERY_X+BATTERY_W,BATTERY_Y+4,2,4,TFT_BLACK);
  if(value>=0)M5.Display.fillRect(BATTERY_X+2,BATTERY_Y+2,(BATTERY_W-4)*std::min(100,value)/100,BATTERY_H-4,TFT_BLACK);
}
void row(size_t index,int rowIndex,bool chosen){
  int y=TOP+rowIndex*ROW;
  M5.Display.fillRect(16,y,508,ROW,chosen?TFT_BLACK:TFT_WHITE);
  M5.Display.setTextColor(chosen?TFT_WHITE:TFT_BLACK,chosen?TFT_BLACK:TFT_WHITE);
  String name=sdBooks[index].substring(sdBooks[index].lastIndexOf('/')+1);int dot=name.lastIndexOf('.');if(dot>0)name=name.substring(0,dot);
  M5.Display.setClipRect(16,y,508,ROW);
  bookName(name,24,y+2,488,chosen?TFT_WHITE:TFT_BLACK);
  M5.Display.clearClipRect();
}
void settingsRow(bool chosen){
  int y=TOP;
  M5.Display.fillRect(16,y,508,ROW,chosen?TFT_BLACK:TFT_WHITE);
  M5.Display.setTextColor(chosen?TFT_WHITE:TFT_BLACK,chosen?TFT_BLACK:TFT_WHITE);
  M5.Display.setClipRect(16,y,508,ROW);
  bookName("Settings",24,y+2,488,chosen?TFT_WHITE:TFT_BLACK);
  M5.Display.clearClipRect();
}
}
void showLibrary(bool sceneChange){
  beginRefresh(sceneChange);M5.Display.fillScreen(TFT_WHITE);font();battery();
  size_t cursor=settingsSelected?0:selectedBook+1;
  size_t page=cursor/controls::LIBRARY_ROWS;
  size_t firstBook=page?page*controls::LIBRARY_ROWS-1:0;
  if(page==0){
    settingsRow(settingsSelected);
    for(size_t r=1;r<controls::LIBRARY_ROWS&&r-1<sdBooks.size();++r)
      row(r-1,int(r),!settingsSelected&&selectedBook==r-1);
  }else{
    for(size_t r=0;r<controls::LIBRARY_ROWS&&firstBook+r<sdBooks.size();++r)
      row(firstBook+r,int(r),!settingsSelected&&selectedBook==firstBook+r);
  }
  if(!sdOK){font();String text="sd card mount failed";M5.Display.setCursor((540-M5.Display.textWidth(text))/2,450);M5.Display.print(text);}
  statusIcon();endRefresh("library",0,0,540,960);
  Serial.printf("LIBRARY books=%u selected=%u settings=%d first=%u rows=%u mounted=%d\n",(unsigned)sdBooks.size(),(unsigned)selectedBook,settingsSelected?1:0,(unsigned)firstBook,controls::LIBRARY_ROWS,sdOK);
}
void updateLibrarySelection(size_t oldSelection,bool oldSettings){
  if(oldSelection==selectedBook&&oldSettings==settingsSelected)return;
  size_t oldCursor=oldSettings?0:oldSelection+1;
  size_t newCursor=settingsSelected?0:selectedBook+1;
  size_t page=newCursor/controls::LIBRARY_ROWS;
  if(oldCursor/controls::LIBRARY_ROWS!=page){showLibrary(false);return;}
  int oldRow=int(oldCursor%controls::LIBRARY_ROWS);
  int newRow=int(newCursor%controls::LIBRARY_ROWS);
  beginRefresh();font();
  auto drawRow=[&](size_t cursor,bool chosen){
    int r=int(cursor%controls::LIBRARY_ROWS);
    if(page==0&&r==0){settingsRow(chosen);return;}
    size_t book=page==0?size_t(r-1):page*controls::LIBRARY_ROWS-1+size_t(r);
    if(book<sdBooks.size())row(book,r,chosen);
  };
  drawRow(oldCursor,false);drawRow(newCursor,true);
  int top=TOP+std::min(oldRow,newRow)*ROW;
  int height=(std::max(oldRow,newRow)-std::min(oldRow,newRow)+1)*ROW;
  endRefresh("selection",16,top,508,height);
  Serial.printf("SELECTION selected=%u settings=%d\n",(unsigned)selectedBook,settingsSelected?1:0);
}
void showBook(bool sceneChange){
  if(pageOffsets.empty())return;
  beginRefresh(sceneChange,true);
  if(sceneChange){M5.Display.fillScreen(TFT_WHITE);}
  else {M5.Display.fillRect(20,20,520,896,TFT_WHITE);M5.Display.fillRect(120,923,300,37,TFT_WHITE);}
  statusIcon();
  font();prepareReaderFont();
  // The 30 px right margin is also the hang zone; clip and refresh it too.
  M5.Display.setClipRect(20,20,520,896);layoutPage(pageOffsets[pageNumber],true);M5.Display.clearClipRect();
  font();
  String number=String(pageNumber+1)+" / "+String(pageOffsets.size());M5.Display.setCursor((540-M5.Display.textWidth(number))/2,960-READER_FONT_HEIGHT);M5.Display.print(number);
  endRefresh("page",20,20,520,940);
  uint32_t offset=pageOffsets[pageNumber];
  uint32_t end=pageNumber+1<pageOffsets.size()?pageOffsets[pageNumber+1]:bookLength;
  Serial.printf("BOOK_PAGE page=%u total=%u key=%s offset=%u end=%u font_px=%d line_height=%d\n",(unsigned)pageNumber+1,(unsigned)pageOffsets.size(),bookmarkKey.c_str(),offset,end,READER_FONT_PX,READER_LINE_HEIGHT);
}
void showLoading(){
  beginRefresh();font();
  statusIcon(true);endRefresh("loading",504,928,28,28);
  Serial.println("LOADING_VISIBLE input=discard");
}
void showError(const String& message){
  beginRefresh();font();M5.Display.fillRect(20,416,500,100,TFT_WHITE);M5.Display.drawRect(20,416,500,100,TFT_BLACK);
  M5.Display.setCursor(32,448);M5.Display.print(fitName(message,476));
  statusIcon();endRefresh("error",20,416,512,540);
}
void showSettings(bool sceneChange){
  beginRefresh(sceneChange);
  M5.Display.fillScreen(TFT_WHITE);
  prepareReaderFont();
  int dx=settingsDx(),dy=settingsDy();
  if(!settingsNetwork)drawBodyGlyph("\xc2\xac",2,SETTINGS_LEFT+dx,SETTINGS_TOP+dy,TFT_BLACK);
  drawBodyGlyph("N",1,48+dx,SETTINGS_TOP+dy,TFT_BLACK);
  if(!settingsRotated)drawBodyGlyph("\xc2\xac",2,SETTINGS_LEFT+dx,SETTINGS_TOP+SETTINGS_LINE+dy,TFT_BLACK);
  drawBodyGlyph("R",1,48+dx,SETTINGS_TOP+SETTINGS_LINE+dy,TFT_BLACK);
  endRefresh("settings",0,0,540,960);
}
void updateSettingsNetwork(){
  beginRefresh();
  int dx=settingsDx(),dy=settingsDy();
  int rx=settingsRotated?540-(24+100):24;
  int ry=settingsRotated?960-(SETTINGS_TOP+SETTINGS_LINE+64):SETTINGS_TOP-12;
  M5.Display.fillRect(rx,ry,100,64,TFT_WHITE);
  prepareReaderFont();
  if(!settingsNetwork)drawBodyGlyph("\xc2\xac",2,SETTINGS_LEFT+dx,SETTINGS_TOP+dy,TFT_BLACK);
  drawBodyGlyph("N",1,48+dx,SETTINGS_TOP+dy,TFT_BLACK);
  endRefresh("settings-n",rx,ry,100,64);
}
void updateLibraryBattery(){
  int raw=M5.Power.getBatteryLevel();bool charging=chargingNow();
  int level=batteryLevelForDisplay(raw,charging);
  if(level==lastBattery&&charging==lastCharging)return;
  beginRefresh();battery();
  endRefresh("battery",BATTERY_CELL_X,BATTERY_CELL_Y,BATTERY_CELL_W,BATTERY_CELL_H);
}
void saveBatteryHistory(){
  loadBatteryHistory();
  int raw=M5.Power.getBatteryLevel();
  if(!chargingNow()&&raw>=0&&raw<=100)batteryHistory=raw;
  if(batteryHistory>=0&&batteryHistory<=100)
    prefs.putUInt("battLast",batteryHistory);
}
void showShutdownOverlay(){
  beginRefresh(false);
  // Leave a dimmed page and a hollow status icon; no shutdown artwork.
  constexpr uint8_t keepRows[4]={0b1010,0b0101,0b1010,0b0101};
  for(int y=0;y<960;++y){
    for(int x=0;x<540;++x)if(!(keepRows[y&3]&(1u<<(x&3))))M5.Display.drawPixel(x,y,TFT_WHITE);
  }
  M5.Display.fillRect(504,928,28,28,TFT_WHITE);
  M5.Display.drawCircle(518,942,7,TFT_BLACK);
#ifdef PAPERDIY_SHUTDOWN_HEADER
  // Optional local artwork is supplied outside the source repository.
  M5.Display.drawBitmap(540-SHUTDOWN_W,960-SHUTDOWN_H,SHUTDOWN_MASK,SHUTDOWN_W,SHUTDOWN_H,TFT_BLACK);
#endif
  endRefresh("shutdown-overlay",0,0,540,960);
}
