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
namespace {
constexpr int TOP=56,ROW=39;
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
  }else {M5.Display.drawCircle(518,942,7,TFT_BLACK);M5.Display.fillCircle(518,942,3,TFT_BLACK);}
}
void battery(){
  font();M5.Display.fillRect(360,12,172,36,TFT_WHITE);
  int value=M5.Power.getBatteryLevel();String text=value<0?"--%":String(std::min(100,std::max(0,value)))+"%";
  int tx=516-M5.Display.textWidth(text);M5.Display.setCursor(tx,16);M5.Display.print(text);
  int x=tx-48;M5.Display.drawRect(x,19,34,18,TFT_BLACK);M5.Display.fillRect(x+34,24,3,8,TFT_BLACK);
  if(value>=0)M5.Display.fillRect(x+3,22,28*std::min(100,std::max(0,value))/100,12,TFT_BLACK);
  if(chargingNow()){M5.Display.drawLine(x-12,19,x-19,29,TFT_BLACK);M5.Display.drawLine(x-19,29,x-11,29,TFT_BLACK);M5.Display.drawLine(x-11,29,x-18,39,TFT_BLACK);}
}
void row(size_t index,size_t first){
  int y=TOP+(index-first)*ROW;bool chosen=index==selectedBook;
  M5.Display.fillRect(16,y,508,ROW,chosen?TFT_BLACK:TFT_WHITE);
  M5.Display.setTextColor(chosen?TFT_WHITE:TFT_BLACK,chosen?TFT_BLACK:TFT_WHITE);
  String name=sdBooks[index].substring(sdBooks[index].lastIndexOf('/')+1);int dot=name.lastIndexOf('.');if(dot>0)name=name.substring(0,dot);
  bookName(name,24,y+6,488,chosen?TFT_WHITE:TFT_BLACK);
}
}
void showLibrary(bool sceneChange){
  beginRefresh(sceneChange);M5.Display.fillScreen(TFT_WHITE);font();battery();
  size_t first=selectedBook/controls::LIBRARY_ROWS*controls::LIBRARY_ROWS;
  for(size_t i=first;i<sdBooks.size()&&i<first+controls::LIBRARY_ROWS;++i)row(i,first);
  if(!sdOK){font();String text="sd card mount failed";M5.Display.setCursor((540-M5.Display.textWidth(text))/2,450);M5.Display.print(text);}
  statusIcon();endRefresh("library",0,0,540,960);
  Serial.printf("LIBRARY books=%u selected=%u first=%u rows=%u mounted=%d\n",(unsigned)sdBooks.size(),(unsigned)selectedBook,(unsigned)first,controls::LIBRARY_ROWS,sdOK);
}
void updateLibrarySelection(size_t oldSelection){
  if(oldSelection==selectedBook)return;
  size_t first=selectedBook/controls::LIBRARY_ROWS*controls::LIBRARY_ROWS;
  if(oldSelection/controls::LIBRARY_ROWS!=selectedBook/controls::LIBRARY_ROWS){showLibrary(false);return;}
  beginRefresh();font();row(oldSelection,first);row(selectedBook,first);
  int top=TOP+(std::min(oldSelection,selectedBook)-first)*ROW;
  int height=(std::max(oldSelection,selectedBook)-std::min(oldSelection,selectedBook)+1)*ROW;
  endRefresh("selection",16,top,508,height);
  Serial.printf("SELECTION index=%u\n",(unsigned)selectedBook);
}
void showBook(bool sceneChange){
  if(pageOffsets.empty())return;
  beginRefresh(sceneChange,true);
  if(sceneChange){M5.Display.fillScreen(TFT_WHITE);statusIcon();}
  else {M5.Display.fillRect(20,20,500,896,TFT_WHITE);M5.Display.fillRect(120,923,300,37,TFT_WHITE);}
  font();prepareReaderFont();layoutPage(pageOffsets[pageNumber],true);
  font();
  String number=String(pageNumber+1)+" / "+String(pageOffsets.size());M5.Display.setCursor((540-M5.Display.textWidth(number))/2,930);M5.Display.print(number);
  endRefresh("page",20,20,500,940);
  uint32_t offset=pageOffsets[pageNumber];if(prefs.getUInt(bookmarkKey.c_str(),UINT32_MAX)!=offset)prefs.putUInt(bookmarkKey.c_str(),offset);
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
  M5.Display.setCursor(32,448);M5.Display.print(fitName(message,476));endRefresh("error",20,416,500,100);
}
void updateLibraryBattery(){beginRefresh();battery();endRefresh("battery",360,12,172,36);}
void showShutdownOverlay(){
  beginRefresh(false);
  // Leave a dimmed page and a hollow status icon; no shutdown artwork.
  constexpr uint8_t keepRows[4]={0b1010,0b0101,0b1010,0b0101};
  for(int y=0;y<960;++y){
    for(int x=0;x<540;++x)if(!(keepRows[y&3]&(1u<<(x&3))))M5.Display.drawPixel(x,y,TFT_WHITE);
  }
  M5.Display.fillRect(504,928,28,28,TFT_WHITE);
  M5.Display.drawCircle(518,942,7,TFT_BLACK);
  endRefresh("shutdown-overlay",0,0,540,960);
}
