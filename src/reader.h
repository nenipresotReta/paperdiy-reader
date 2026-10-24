#pragma once
#include "text_language.h"
#include "reader_work.h"
#include "content_source.h"
#include "managed_progress.h"
#include <Arduino.h>
#include <Preferences.h>
#include <vector>
#include "bookmark_migration.h"
constexpr size_t MAX_BOOK=4*1024*1024;
constexpr int READER_FONT_PX=28,READER_LINE_HEIGHT=40;
extern Preferences prefs;
extern char* book;
extern size_t bookLength;
extern std::vector<uint32_t> pageOffsets;
extern uint32_t pageNumber;
extern String bookmarkKey,bookIdentity;
size_t utf8Char(const char* s,size_t remain,char out[5]);
void prepareReaderFont();
size_t layoutPage(size_t from,bool draw,int top=24,reader_work::Budget* budget=nullptr);
bool indexBook(bool reset=false,bool migrateLegacy=true,const bookmarks::Anchor* anchor=nullptr,bool requireRecovery=false,bool progressive=false);
// A progressive open publishes a measured local window immediately. Its page
// starts are for display/navigation only, never a complete cache index.
bool readerIndexPending();
bool readerIndexComplete();
bool readerIndexFailed();
bool readerHasProvisionalPages();
bool advanceReaderIndex(unsigned maxPages=2,uint32_t maxMs=12);
bool turnReaderPage(int direction);
bool pinReaderPage(size_t selected);
// Zero means unknown. Only a provisional window starting at zero has an ordinal.
uint32_t readerPageNumber();
uint32_t readerTotalPages();
uint32_t currentPageEnd();
const std::vector<uint32_t>* canonicalReaderPages();
// Incomplete canonical prefix only; never pass it to complete-cache writers.
const std::vector<uint32_t>* partialReaderPages();
uint32_t partialReaderCursor();
bool restoreReaderIndex(const uint32_t* pages,size_t count,uint32_t cursor);
// Call before clearing/replacing the book or loading a complete cached index.
// This discards background work; it does not mutate the visible page table.
void cancelReaderIndex();
bool initializeProgress();
bool saveReadingProgress();
// A successful import/cache load publishes the exact saved source offset.
void retainReadingAnchor(uint32_t offset);
// UINT32_MAX means no current readable page; normal redraw does not move it.
uint32_t currentReadingOffset();
extern bool progressSaveFailed;
extern bool bookEnglishHyphens;
extern text_language::Language bookLanguage;
extern bool bookMarkdown,bookMarkupFallback;
