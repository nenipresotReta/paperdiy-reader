#include <Arduino.h>
#include <M5Unified.h>
#include "display_driver.h"
#include "refresh_policy.h"
#include "power.h"
static RefreshPolicy policy;
static bool full;
static uint32_t started;
static int normalRotation=0;
void displayInit(){
  M5.Display.setRotation(0);normalRotation=0;
  if(M5.Display.width()!=540){M5.Display.setRotation(1);normalRotation=1;}
  M5.Display.setAutoDisplay(false);
}
void displaySetRotated(bool rotated){
  // The panel's base portrait rotation is 0 or 1. A 180-degree flip is the
  // same relative rotation; using the base avoids depending on the absolute
  // M5GFX rotation index of this panel.
  int target=(normalRotation+(rotated?2:0))%4;
  M5.Display.setRotation(target);
  Serial.printf("DISPLAY_ROTATION base=%d target=%d size=%dx%d\n",normalRotation,target,M5.Display.width(),M5.Display.height());
}
bool beginRefresh(bool sceneChange,bool reading){
  powerActivity();
  M5.Display.waitDisplay();started=millis();full=policy.choose(sceneChange,reading);
  M5.Display.setEpdMode(full?epd_mode_t::epd_quality:epd_mode_t::epd_fast);
  M5.Display.startWrite();return full;
}
void endRefresh(const char* reason,int x,int y,int w,int h){
  M5.Display.endWrite();
  if(full)M5.Display.display(0,0,540,960);
  else if(w>0&&h>0)M5.Display.display(x,y,w,h);
  else M5.Display.display();
  M5.Display.waitDisplay();
  powerActivity();
  Serial.printf("REFRESH reason=%s mode=%s rect=%d,%d,%d,%d ms=%u partials=%u\n",reason,full?"quality":"fast",full?0:x,full?0:y,full?540:w,full?960:h,(unsigned)(millis()-started),policy.partials);
}
void logDisplayStatus(){Serial.printf("DISPLAY fulls=%u partials_since_full=%u\n",policy.fulls,policy.partials);}
