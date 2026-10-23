#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "M5Unified.h"
#include "native_font_data.h"
#include "reader.h"
#include "reader_font.h"
#include "reading_font.h"
#include "sd_reader.h"
#include "ui.h"
#include "display_driver.h"
#include "reader_typography.h"
#include "markdown_samples.h"

std::vector<String> sdBooks;
bool sdOK = true;
size_t selectedBook = 0;
String sdError;

const lgfx::IFont* readerFont() { static lgfx::IFont font; return &font; }
const lgfx::IFont* bodyFont() { static lgfx::IFont font; return &font; }

uint32_t readerCodepoint(const char* text, size_t bytes) {
  uint32_t cp = uint8_t(text[0]);
  if (bytes > 1) {
    cp &= (1u << (7 - bytes)) - 1;
    for (size_t i = 1; i < bytes; ++i) cp = (cp << 6) | (uint8_t(text[i]) & 63);
  }
  return cp;
}

int readerGlyphWidth(uint32_t codepoint) {
  return reader_font::glyphAdvance(reader_font::FONT_24, codepoint);
}
int bodyGlyphAdvance(uint32_t codepoint) {
  return reader_font::glyphAdvance(reader_font::FONT_28, codepoint);
}
const reader_font::Glyph* bodyGlyph(uint32_t cp) { return reader_font::findGlyph(reader_font::FONT_28,cp); }
bool readerFontHasGlyph(uint32_t codepoint) {
  return reader_font::hasGlyph(reader_font::FONT_24, codepoint);
}
bool bodyFontHasGlyph(uint32_t codepoint) {
  return reader_font::hasGlyph(reader_font::FONT_28, codepoint);
}

struct CodeGlyph { uint32_t cp; int x,y; };
bool captureCodeGlyphs=false;
std::vector<CodeGlyph> capturedCodeGlyphs;
void drawReaderGlyph(const char* text, size_t bytes, int x, int y, uint32_t foreground) {
  if(captureCodeGlyphs)capturedCodeGlyphs.push_back({readerCodepoint(text,bytes),x,y});
  reader_font::drawGlyphRuns(reader_font::FONT_24, readerCodepoint(text, bytes), x, y,
    [&](int left, int top, int count) {
      M5.Display.drawFastHLine(left, top, count, foreground);
    });
}

void drawBodyGlyph(const char* text, size_t bytes, int x, int y, uint32_t foreground) {
  reader_font::drawGlyphRuns(reader_font::FONT_28, readerCodepoint(text, bytes), x, y,
    [&](int left, int top, int count) {
      M5.Display.drawFastHLine(left, top, count, foreground);
    });
}

bool chargingNow() { return false; }
void powerActivity() {}

namespace {
std::vector<char> textBuffer;

void loadBook(const std::string& text, bool markdown=false) {
  bookMarkdown=markdown;bookMarkupFallback=false;
  textBuffer.assign(text.begin(), text.end());
  textBuffer.push_back(0);
  book = textBuffer.data();
  bookLength = text.size();
  prefs.values.clear();
  prefs.failReadKey.clear();
  prefs.writes = 0;
  assert(initializeProgress());
  bookIdentity = "/books/sim.txt";
  bookmarkKey = "sim";
  bookEnglishHyphens = true;
  pageOffsets.clear();
  pageNumber = 0;
  assert(indexBook(false, false, nullptr, false));
  assert(!pageOffsets.empty());
}

bool black(int x, int y) { return M5.Display.fb[y * 540 + x] == 0; }

int countBlack(int x0, int y0, int x1, int y1) {
  int count = 0;
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x)
      if (black(x, y)) ++count;
  return count;
}

void writePGM(const char* path) {
  FILE* file = fopen(path, "wb");
  assert(file);
  fprintf(file, "P5\n540 960\n255\n");
  assert(fwrite(M5.Display.fb.data(), 1, M5.Display.fb.size(), file) ==
         M5.Display.fb.size());
  fclose(file);
}
}  // namespace

int main() {
  M5.Power.battery = 87;
  for (int i = 1; i <= 25; ++i) {
    if (i == 1) sdBooks.push_back("/books/模拟书籍 01.epub");
    else if (i == 2) sdBooks.push_back("/books/Simulated Book 02.epub");
    else sdBooks.push_back("/books/Book " + std::to_string(i) + ".epub");
  }
  settingsSelected = false;
  settingsNetwork = false;
  settingsRotated = false;

  // First library page: Settings at row 0, books 0..20 at rows 1..21.
  showLibrary(true);
  assert(M5.Display.lastFull);
  assert(M5.Display.lastRefreshX == 0 && M5.Display.lastRefreshW == 540);
  assert(!black(17, 57));                      // Settings row is not selected
  assert(black(17, 96));                       // first book row is selected
  assert(countBlack(24, 57, 500, 94) > 0);     // Settings text is drawn
  assert(countBlack(24, 875, 500, 913) > 0);   // 21st book row is drawn
  assert(!black(20, 914));                     // 22nd book row is not on page 1
  assert(countBlack(360, 12, 532, 48) > 0);    // battery area
  const int fullBattery = countBlack(504, 8, 532, 36);
  M5.Power.battery = 20;
  updateLibraryBattery();
  assert(countBlack(504, 8, 532, 36) < fullBattery);
  assert(M5.Display.lastRefreshX == 504 && M5.Display.lastRefreshY == 8);
  assert(M5.Display.lastRefreshW == 28 && M5.Display.lastRefreshH == 28);
  saveBatteryHistory();
  assert(prefs.values["battLast"] == 20);
  writePGM("ui-simulation-library.pgm");

  // The only library status text is the mount failure.
  sdOK = false;
  showLibrary(true);
  assert(countBlack(100, 440, 440, 490) > 0);
  sdOK = true;

  // In-page book selection refresh covers the two changed rows.
  selectedBook = 1;
  settingsSelected = false;
  updateLibrarySelection(0, false);
  assert(M5.Display.lastRefreshX == 16 && M5.Display.lastRefreshY == 95);
  assert(M5.Display.lastRefreshW == 508 && M5.Display.lastRefreshH == 78);
  selectedBook = 0;

  // Settings row selected, then book 0 selected again.
  settingsSelected = true;
  selectedBook = 0;
  showLibrary(true);
  assert(black(17, 57));
  assert(!black(17, 96));
  settingsSelected = false;
  selectedBook = 0;
  updateLibrarySelection(0, true);
  assert(M5.Display.lastRefreshX == 16 && M5.Display.lastRefreshY == 56);
  assert(M5.Display.lastRefreshW == 508 && M5.Display.lastRefreshH == 78);

  // Settings only exists on the first page. Book 21 starts page 2 at row 0.
  selectedBook = 21;
  showLibrary(true);
  assert(black(17, 57));
  assert(!black(17, 96));
  assert(countBlack(24, 57, 500, 94) > 0);     // book title, not Settings
  selectedBook = 0;
  settingsSelected = false;
  showLibrary(true);

  // Settings page: only the four glyphs, at the agreed coordinates.
  settingsNetwork = false;
  settingsRotated = false;
  showSettings(true);
  assert(M5.Display.lastFull);
  assert(countBlack(0, 0, 540, 600) == 0);
  assert(countBlack(0, 820, 540, 960) == 0);
  assert(countBlack(32, 641, 46, 680) > 0);    // ¬ of ¬N
  assert(countBlack(48, 641, 80, 680) > 0);    // N
  assert(countBlack(32, 753, 46, 792) > 0);    // ¬ of ¬R
  assert(countBlack(48, 753, 80, 792) > 0);    // R
  assert(countBlack(504, 928, 532, 956) == 0);// no status icon
  writePGM("ui-simulation-settings.pgm");

  // N on: only its ¬ disappears; the partial refresh covers the N line.
  settingsNetwork = true;
  updateSettingsNetwork();
  assert(M5.Display.lastRefreshX == 24 && M5.Display.lastRefreshY == 629);
  assert(M5.Display.lastRefreshW == 100 && M5.Display.lastRefreshH == 64);
  assert(countBlack(32, 641, 46, 680) == 0);
  assert(countBlack(48, 641, 80, 680) > 0);
  settingsNetwork = false;
  showSettings(true);

  // R on: the Settings group is compensated to stay at the same panel
  // position after the global 180-degree rotation. The logical draw is the
  // mirrored group, with N at 641/464 and R at 753/480.
  settingsRotated = true;
  settingsNetwork = false;
  showSettings(true);
  assert(M5.Display.lastFull);
  assert(countBlack(0, 0, 540, 100) == 0);
  assert(countBlack(0, 600, 540, 960) == 0);
  assert(countBlack(32, 641, 80, 680) == 0);   // no unrotated N group
  assert(countBlack(32, 753, 80, 792) == 0);   // no unrotated R group
  assert(countBlack(464, 168, 500, 200) > 0);  // ¬N in mirrored position
  assert(countBlack(480, 280, 520, 315) > 0);  // R in mirrored position

  // Rotated N toggle refreshes the mirrored rectangle and removes its ¬.
  settingsNetwork = true;
  updateSettingsNetwork();
  assert(M5.Display.lastRefreshX == 416 && M5.Display.lastRefreshY == 143);
  assert(M5.Display.lastRefreshW == 100 && M5.Display.lastRefreshH == 64);
  assert(countBlack(464, 168, 478, 200) == 0);
  assert(countBlack(480, 168, 510, 200) > 0);
  settingsNetwork = false;
  settingsRotated = false;
  showSettings(true);

  // Reading: the question mark starts at the old text edge (510) and its ink
  // reaches 522, so the clip and refresh must include the 30 px hang zone.
  std::string hang;
  for (int i = 0; i < 17; ++i) hang += "中";
  hang += "-？\n";
  for (int i = 0; i < 20; ++i) hang += "Second page line with ordinary words. ";
  loadBook(hang);
  showBook(true);
  assert(M5.Display.lastFull);
  assert(M5.Display.lastClipX == 20 && M5.Display.lastClipY == 20);
  assert(M5.Display.lastClipW == 520 && M5.Display.lastClipH == 896);
  assert(countBlack(24, 24, 510, 886) > 0);
  assert(countBlack(521, 20, 539, 900) > 0);   // hanging punctuation ink
  assert(countBlack(200, 926, 340, 960) > 0);  // page number
  assert(countBlack(504, 928, 532, 956) > 0);  // status icon
  writePGM("ui-simulation-reading.pgm");

  // Page-turn simulation: a partial refresh must clear and repaint the far
  // right of the hang zone. The pixels are deliberately placed outside the
  // old 520/532 rectangles.
  M5.Display.fb[910 * 540 + 530] = 0;
  M5.Display.fb[910 * 540 + 538] = 0;
  M5.Display.panel[910 * 540 + 530] = 0;
  M5.Display.panel[910 * 540 + 538] = 0;
  showBook(false);
  assert(!M5.Display.lastFull);
  assert(M5.Display.lastRefreshX == 20 && M5.Display.lastRefreshY == 20);
  assert(M5.Display.lastRefreshW == 520 && M5.Display.lastRefreshH == 940);
  assert(M5.Display.lastClipW == 520 && M5.Display.lastClipH == 896);
  assert(M5.Display.fb[910 * 540 + 530] == 255);
  assert(M5.Display.fb[910 * 540 + 538] == 255);
  assert(M5.Display.panel[910 * 540 + 530] == 255);
  assert(M5.Display.panel[910 * 540 + 538] == 255);

  // Protect prose, italic and formula pixels while code deliberately uses its
  // actual native advance rather than a padded 24 px cell. An independent
  // old-width render reproduced 0x5d7e6107fc773c9a; its only changed pixels were
  // this fixture's second-page code area (y >= 529), with offsets still 0/807.
  std::string asciiSource="## ASCII typography\n\n";
  for(unsigned pass=0;pass<6;++pass)asciiSource+="A quiet observer checks 0123456789, **bold words**, *italic words*, and a link [label](https://example.invalid). Mixed 中文 and Ελληνικά remain unchanged. Inline $x_1+2$ shares the baseline.\n\n";
  asciiSource+="```\n";for(unsigned cp=32;cp<=126;++cp)asciiSource+=char(cp);asciiSource+="\n```\n";
  loadBook(asciiSource,true);
  assert(bookMarkdown && !bookMarkupFallback);
  assert(pageOffsets==std::vector<uint32_t>({0,807}));
  uint64_t asciiRasterHash=1469598103934665603ULL,proseRasterHash=asciiRasterHash;
  capturedCodeGlyphs.clear();captureCodeGlyphs=true;
  for(size_t i=0;i<pageOffsets.size();++i){
    M5.Display.fillScreen(TFT_WHITE);
    assert(layoutPage(pageOffsets[i],true)==(i+1<pageOffsets.size()?pageOffsets[i+1]:bookLength));
    for(size_t pixel=0;pixel<M5.Display.fb.size();++pixel){
      asciiRasterHash=(asciiRasterHash^M5.Display.fb[pixel])*1099511628211ULL;
      const uint8_t prose=i==1&&pixel/540>=529?255:M5.Display.fb[pixel];
      proseRasterHash=(proseRasterHash^prose)*1099511628211ULL;
    }
  }
  captureCodeGlyphs=false;
  // This protected hash also matched the old padded-width renderer exactly.
  assert(proseRasterHash==0xbf395a2a06ddf2caULL);
  assert(capturedCodeGlyphs.size()==95);
  std::vector<uint8_t> codePixels(540*960,255);
  unsigned codeLines=0;
  for(size_t i=0;i<capturedCodeGlyphs.size();++i){
    const auto& glyph=capturedCodeGlyphs[i];assert(glyph.cp==32+i);
    if(!i||glyph.y!=capturedCodeGlyphs[i-1].y){
      assert(glyph.x==24&&glyph.y==529+int(codeLines)*40);++codeLines;
    }else{
      const auto& previous=capturedCodeGlyphs[i-1];
      assert(glyph.x==previous.x+reader_font::glyphAdvance(reader_font::FONT_24,previous.cp));
    }
    reader_font::drawGlyphRuns(reader_font::FONT_24,glyph.cp,glyph.x,glyph.y,
      [&](int x,int y,int count){
        for(int column=x;column<x+count;++column)codePixels[y*540+column]=0;
      });
  }
  assert(codeLines==4); // Removing artificial padding saves one code line.
  for(size_t pixel=529*540;pixel<codePixels.size();++pixel)
    assert(M5.Display.fb[pixel]==codePixels[pixel]);
  assert(asciiRasterHash==0xe54177027cb05b10ULL);

  // Render the production Markdown adapter with native glyphs and real math
  // commands, including a second page after source-offset pagination.
  loadBook(markdown_samples::mixed,true);
  assert(bookMarkdown&&!bookMarkupFallback&&pageOffsets.size()>=2);
  for(pageNumber=0;pageNumber<pageOffsets.size();++pageNumber){
    showBook(pageNumber==0);
    assert(countBlack(24,24,510,916)>500);
    char output[80];snprintf(output,sizeof(output),"ui-simulation-markdown-%u.pgm",unsigned(pageNumber));
    writePGM(output);
  }
  pageNumber=0;
  puts("UI simulation: first-page Settings, selection refresh, Settings four-glyph page, N partial, R compensation, reading hang-zone: PASS");
}
