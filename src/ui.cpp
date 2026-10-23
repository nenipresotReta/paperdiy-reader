#include <M5Unified.h>
#include <algorithm>
#include <cstring>
#include "reader.h"
#include "qr_wrapper.h"
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
bool settingsSelected=false,settingsRotated=false;
SettingsId settingsId=SettingsId::Network;
NetworkMode networkMode=NetworkMode::Off;
String settingsApSsid,settingsApPassword,settingsApUrl;
bool settingsApClientConnected=false;
String settingsStaSsid,settingsStaIp,settingsStaError;
bool settingsStaConfigured=false,settingsStaConnected=false;
namespace {
constexpr int TOP=56,ROW=39;
constexpr int BATTERY_CELL_X=504,BATTERY_CELL_Y=8,BATTERY_CELL_W=28,BATTERY_CELL_H=28;
constexpr int BATTERY_X=506,BATTERY_Y=16,BATTERY_W=22,BATTERY_H=12;
constexpr int SETTINGS_LEFT=32,SETTINGS_TOP=641,SETTINGS_LINE=112,SETTINGS_W=260,SETTINGS_H=151;
constexpr int MODE_STEP=108;
constexpr int MODE_PREFIX_X[3]={32,32+MODE_STEP,32+2*MODE_STEP};
constexpr int MODE_LETTER_X[3]={48,48+MODE_STEP,48+2*MODE_STEP};
// Network content and button controls occupy separate physical panel areas.
// Mirror the content area as well as the controls when rotating the display.
constexpr int SETTINGS_CONTENT_TOP=112,SETTINGS_CONTENT_H=400;
constexpr int MODE_REFRESH_X=24;
constexpr int MODE_REFRESH_W=300,MODE_REFRESH_H=64;
int settingsDx(){return settingsRotated?540-SETTINGS_LEFT-SETTINGS_W-SETTINGS_LEFT:0;}
// In physical panel coordinates the mode row stays above R in both orientations.
int settingsModeY(){return settingsRotated?960-SETTINGS_TOP-SETTINGS_H+SETTINGS_LINE:SETTINGS_TOP;}
int settingsRotationY(){return settingsRotated?960-SETTINGS_TOP-SETTINGS_H:SETTINGS_TOP+SETTINGS_LINE;}
int settingsContentDy(){return settingsRotated?960-2*SETTINGS_CONTENT_TOP-SETTINGS_CONTENT_H:0;}
int lastBatteryFill=-1; // No battery pixels have been painted yet.
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
void bodyText(const char* text,int x,int y){
  const size_t length=strlen(text);
  size_t pos=0;int pen=x;
  while(pos<length){
    char ch[5];size_t n=utf8Char(text+pos,length-pos,ch);
    drawBodyGlyph(ch,n,pen,y,TFT_BLACK);
    pen+=bodyGlyphAdvance(readerCodepoint(ch,n));pos+=n;
  }
}
int activeModeIndex(){
  if(networkMode==NetworkMode::Sta)return 0;
  if(networkMode==NetworkMode::Ap)return 1;
  if(networkMode==NetworkMode::Usb)return 2;
  return -1;
}
void modeRow(int dx,int y){
  static const char* labels[3]={"N","A","U"};
  const int active=activeModeIndex();
  for(int i=0;i<3;++i){
    if(i!=active)drawBodyGlyph("\xc2\xac",2,MODE_PREFIX_X[i]+dx,y,TFT_BLACK);
    drawBodyGlyph(labels[i],1,MODE_LETTER_X[i]+dx,y,TFT_BLACK);
  }
}
void smallLine(const String& text,int x,int y){
  font();
  M5.Display.setCursor(x,y);
  M5.Display.print(fitName(text,540-x-24));
}
void qrCode(const String& data,int x,int y,int size){
  uint8_t matrix[64*64];
  const int count=qrMatrix(data.c_str(),matrix,6);
  if(count<=0)return;
  const int scale=size/(count+8);
  if(scale<1)return;
  const int total=(count+8)*scale;
  const int left=x+(size-total)/2;
  const int top=y+(size-total)/2;
  M5.Display.fillRect(x,y,size,size,TFT_WHITE);
  for(int row=0;row<count;++row)
    for(int col=0;col<count;++col)
      if(matrix[row*count+col])
        M5.Display.fillRect(left+(col+4)*scale,top+(row+4)*scale,scale,scale,TFT_BLACK);
}
void apPanel(){
  const int dy=settingsContentDy();
  if(settingsApSsid.length()==0){
    bodyText("AP not running",32,120+dy);
    return;
  }
  constexpr int qrY=112,qrSize=294,qrX=(540-qrSize)/2;
  const String payload=settingsApClientConnected?settingsApUrl:
    String("WIFI:T:WPA;S:")+settingsApSsid+";P:"+settingsApPassword+";H:false;;";
  qrCode(payload,qrX,qrY+dy,qrSize);
  const String caption=settingsApClientConnected?"Web UI":"Wi-Fi";
  font();
  smallLine(caption,(540-M5.Display.textWidth(caption))/2,414+dy);
  smallLine(String("SSID: ")+settingsApSsid,24,448+dy);
  smallLine(String("PASS: ")+settingsApPassword,24,480+dy);
}
void staPanel(){
  const int dy=settingsContentDy();
  bodyText("STA",32,112+dy);
  font();
  int y=160+dy;
  String ssidLine=settingsStaConfigured?String("SSID: ")+settingsStaSsid:String("No WiFi config");
  M5.Display.setCursor(32,y);
  M5.Display.print(fitName(ssidLine,484));
  y+=36;
  if(settingsStaConfigured){
    String status=settingsStaError.length()?settingsStaError:String(settingsStaConnected?"Connected":"Connecting...");
    M5.Display.setCursor(32,y);
    M5.Display.print(fitName(status,484));
    y+=36;
    if(settingsStaConnected){
      M5.Display.setCursor(32,y);
      M5.Display.print(String("IP: ")+settingsStaIp);
    }
  }else{
    M5.Display.setCursor(32,y);
    M5.Display.print("Configure in A mode");
  }
}
void usbPanel(){
  const int dy=settingsContentDy();
  bodyText("USB Storage",32,112+dy);
  smallLine("Not enabled in this build",32,160+dy);
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
int batteryFillWidth(int value){
  return value<0?0:(BATTERY_W-4)*std::min(100,value)/100;
}
void battery(int value){
  lastBatteryFill=batteryFillWidth(value);
  M5.Display.fillRect(BATTERY_CELL_X,BATTERY_CELL_Y,BATTERY_CELL_W,BATTERY_CELL_H,TFT_WHITE);
  M5.Display.drawRect(BATTERY_X,BATTERY_Y,BATTERY_W,BATTERY_H,TFT_BLACK);
  M5.Display.fillRect(BATTERY_X+BATTERY_W,BATTERY_Y+4,2,4,TFT_BLACK);
  if(lastBatteryFill>0)M5.Display.fillRect(BATTERY_X+2,BATTERY_Y+2,lastBatteryFill,BATTERY_H-4,TFT_BLACK);
}
void battery(){
  int raw=M5.Power.getBatteryLevel();bool charging=chargingNow();
  battery(batteryLevelForDisplay(raw,charging));
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
void settingsRow(size_t index,int rowIndex,bool chosen){
  int y=TOP+rowIndex*ROW;
  M5.Display.fillRect(16,y,508,ROW,chosen?TFT_BLACK:TFT_WHITE);
  M5.Display.setTextColor(chosen?TFT_WHITE:TFT_BLACK,chosen?TFT_BLACK:TFT_WHITE);
  M5.Display.setClipRect(16,y,508,ROW);
  bookName(settings_registry::ENTRIES[index].title,24,y+2,488,chosen?TFT_WHITE:TFT_BLACK);
  M5.Display.clearClipRect();
}
}
void showLibrary(bool sceneChange){
  beginRefresh(sceneChange);M5.Display.fillScreen(TFT_WHITE);font();battery();
  size_t cursor=settings_registry::libraryCursor(selectedBook,settingsSelected,settingsId);
  size_t page=cursor/controls::LIBRARY_ROWS;
  size_t first=page*controls::LIBRARY_ROWS;
  size_t firstBook=first>=settings_registry::COUNT?first-settings_registry::COUNT:0;
  for(size_t r=0;r<controls::LIBRARY_ROWS;++r){
    const size_t entry=first+r;
    if(entry<settings_registry::COUNT)settingsRow(entry,int(r),entry==cursor);
    else if(entry-settings_registry::COUNT<sdBooks.size())
      row(entry-settings_registry::COUNT,int(r),entry==cursor);
  }
  if(!sdOK){font();String text="sd card mount failed";M5.Display.setCursor((540-M5.Display.textWidth(text))/2,450);M5.Display.print(text);}
  statusIcon();endRefresh("library",0,0,540,960);
  Serial.printf("LIBRARY books=%u selected=%u settings=%d first=%u rows=%u mounted=%d\n",(unsigned)sdBooks.size(),(unsigned)selectedBook,settingsSelected?1:0,(unsigned)firstBook,controls::LIBRARY_ROWS,sdOK);
}
void updateLibrarySelection(size_t oldSelection,bool oldSettings,SettingsId oldId){
  size_t oldCursor=settings_registry::libraryCursor(oldSelection,oldSettings,oldId);
  size_t newCursor=settings_registry::libraryCursor(selectedBook,settingsSelected,settingsId);
  if(oldCursor==newCursor)return;
  size_t page=newCursor/controls::LIBRARY_ROWS;
  if(oldCursor/controls::LIBRARY_ROWS!=page){showLibrary(false);return;}
  int oldRow=int(oldCursor%controls::LIBRARY_ROWS);
  int newRow=int(newCursor%controls::LIBRARY_ROWS);
  beginRefresh();font();
  auto drawRow=[&](size_t cursor,bool chosen){
    int r=int(cursor%controls::LIBRARY_ROWS);
    if(cursor<settings_registry::COUNT){settingsRow(cursor,r,chosen);return;}
    size_t book=cursor-settings_registry::COUNT;
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
  if(networkMode==NetworkMode::Ap)apPanel();
  else if(networkMode==NetworkMode::Sta)staPanel();
  else if(networkMode==NetworkMode::Usb)usbPanel();
  int dx=settingsDx();
  modeRow(dx,settingsModeY());
  if(!settingsRotated)drawBodyGlyph("\xc2\xac",2,SETTINGS_LEFT+dx,settingsRotationY(),TFT_BLACK);
  // The panel-side button edge is the far end of the mirrored mode row.
  const int rotationX=MODE_LETTER_X[settingsRotated?2:0]+dx;
  drawBodyGlyph("R",1,rotationX,settingsRotationY(),TFT_BLACK);
  endRefresh("settings",0,0,540,960);
}
void updateSettingsMode(){
  if(settingsId!=SettingsId::Network)return;
  beginRefresh();
  int rx=settingsRotated?540-(MODE_REFRESH_X+MODE_REFRESH_W):MODE_REFRESH_X;
  int ry=settingsModeY()-12;
  M5.Display.fillRect(rx,ry,MODE_REFRESH_W,MODE_REFRESH_H,TFT_WHITE);
  prepareReaderFont();
  modeRow(settingsDx(),settingsModeY());
  endRefresh("settings-mode",rx,ry,MODE_REFRESH_W,MODE_REFRESH_H);
}
void updateSettingsNetwork(){
  if(settingsId!=SettingsId::Network)return;
  beginRefresh();
  const int y=SETTINGS_CONTENT_TOP+settingsContentDy();
  M5.Display.fillRect(0,y,540,SETTINGS_CONTENT_H,TFT_WHITE);
  prepareReaderFont();
  if(networkMode==NetworkMode::Ap)apPanel();
  else if(networkMode==NetworkMode::Sta)staPanel();
  else if(networkMode==NetworkMode::Usb)usbPanel();
  endRefresh("settings-network",0,y,540,SETTINGS_CONTENT_H);
}

void updateLibraryBattery(){
  int raw=M5.Power.getBatteryLevel();bool charging=chargingNow();
  int level=batteryLevelForDisplay(raw,charging);
  // Only the quantized fill is visible; charging freezes the displayed level
  // but has no separate icon. Keep sampling/history even when no pixel changes.
  if(batteryFillWidth(level)==lastBatteryFill)return;
  beginRefresh();battery(level);
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
