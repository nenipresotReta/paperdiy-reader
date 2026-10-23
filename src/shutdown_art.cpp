#include <SD.h>
#include <M5Unified.h>
#include "shutdown_art.h"
bool drawStoredShutdownArt(){
  const auto mode=reader_settings::current().art;
  if(mode!=1&&mode!=2)return false;
  File file=SD.open(shutdown_art::path(mode),FILE_READ);
  auto data=shutdown_art::read(file);if(file)file.close();
  if(!data)return false;
  const unsigned w=shutdown_art::word(data.get()+4),h=shutdown_art::word(data.get()+6);
  M5.Display.drawBitmap(540-w,960-h,data.get()+shutdown_art::HEADER,w,h,TFT_BLACK);
  return true;
}
