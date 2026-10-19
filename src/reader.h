#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <vector>
constexpr size_t MAX_BOOK=4*1024*1024;
constexpr int READER_FONT_PX=28,READER_LINE_HEIGHT=38;
extern Preferences prefs;
extern char* book;
extern size_t bookLength;
extern std::vector<uint32_t> pageOffsets;
extern uint32_t pageNumber;
extern String bookmarkKey,bookIdentity;
size_t utf8Char(const char* s,size_t remain,char out[5]);
void prepareReaderFont();
size_t layoutPage(size_t from,bool draw,int top=24);
bool indexBook(bool reset=false,bool migrateLegacy=true);
