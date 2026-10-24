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
#include "reader_typography.h"
#include "resume_store.h"
#include "display_driver.h"
#include "qr_wrapper.h"
#include "markdown_samples.h"

std::vector<String> sdBooks;
bool sdOK = true;
bool simulatedBackground=false;
bool sdBackgroundPending(){return simulatedBackground;}
bool drawStoredShutdownArt(){return false;}
size_t selectedBook = 0;
String sdError;

const lgfx::IFont* readerFont() { static lgfx::IFont font; return &font; }
const reader_font::Font& bodyNativeFont(){return reader_settings::geometry().pixels==24?reader_font::FONT_24:reader_font::FONT_28;}
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
bool readerFontHasGlyph(uint32_t codepoint) {
  return reader_font::hasGlyph(reader_font::FONT_24, codepoint);
}
const reader_font::Glyph* bodyGlyph(uint32_t codepoint) {
  return reader_font::findGlyph(reader_font::FONT_28, codepoint);
}
bool bodyFontHasGlyph(uint32_t codepoint) {
  return reader_font::hasGlyph(reader_font::FONT_28, codepoint);
}

struct CodeGlyph { uint32_t cp; int x,y; };
bool captureCodeGlyphs=false;
std::vector<CodeGlyph> capturedCodeGlyphs;
bool captureBodyGlyphs=false;
std::vector<CodeGlyph> capturedBodyGlyphs;
void drawReaderGlyph(const char* text, size_t bytes, int x, int y, uint16_t foreground) {
  if(captureCodeGlyphs)capturedCodeGlyphs.push_back({readerCodepoint(text,bytes),x,y});
  reader_font::drawGlyphRuns(reader_font::FONT_24, readerCodepoint(text, bytes), x, y,
    [&](int left, int top, int count) {
      M5.Display.drawFastHLine(left, top, count, foreground);
    });
}

void drawBodyGlyph(const char* text, size_t bytes, int x, int y, uint16_t foreground) {
  if(captureBodyGlyphs)capturedBodyGlyphs.push_back({readerCodepoint(text,bytes),x,y});
  reader_font::drawGlyphRuns(reader_font::FONT_28, readerCodepoint(text, bytes), x, y,
    [&](int left, int top, int count) {
      M5.Display.drawFastHLine(left, top, count, foreground);
    });
}

bool simulatedCharging=false;
bool chargingNow() { return simulatedCharging; }
void powerActivity() {}

namespace {
std::vector<char> textBuffer;

void loadBook(const std::string& text, bool markdown = false) {
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

void writePGM(const char* path, bool physical = false) {
  FILE* file = fopen(path, "wb");
  assert(file);
  fprintf(file, "P5\n540 960\n255\n");
  const auto& pixels = physical ? M5.Display.panel : M5.Display.fb;
  assert(fwrite(pixels.data(), 1, pixels.size(), file) == pixels.size());
  fclose(file);
}

void rotateSettings(bool rotated) {
  settingsRotated = rotated;
  displaySetRotated(rotated);
  assert(M5.Display.rotation == (rotated ? 2 : 0));
}

std::vector<uint8_t> crop(int x, int y, int width, int height) {
  std::vector<uint8_t> pixels;
  for (int row = y; row < y + height; ++row)
    pixels.insert(pixels.end(), M5.Display.fb.begin() + row * 540 + x,
                  M5.Display.fb.begin() + row * 540 + x + width);
  return pixels;
}

void expectedGlyph(std::vector<uint8_t>& pixels, uint32_t cp, int x, int y) {
  reader_font::drawGlyphRuns(reader_font::FONT_28, cp, x, y,
    [&](int left, int top, int count) {
      for (int column = left; column < left + count; ++column)
        pixels[top * 540 + column] = 0;
    });
}

void expectControls(NetworkMode mode, bool rotated) {
  // Check the full native glyph bitmaps, not merely whether each region has ink.
  // N/A/U pen positions are exactly 108 px apart in either orientation.
  const int letters[3] = {rotated ? 264 : 48, rotated ? 372 : 156,
                          rotated ? 480 : 264};
  assert(letters[1] - letters[0] == 108 && letters[2] - letters[1] == 108);
  const int modeY = rotated ? 280 : 641, rY = rotated ? 168 : 753;
  const int rX = rotated ? 480 : 48;
  const char labels[] = "NAU";
  const int active = mode == NetworkMode::Sta ? 0 : mode == NetworkMode::Ap ? 1 :
                     mode == NetworkMode::Usb ? 2 : -1;
  std::vector<uint8_t> expected(540 * 960, 255);
  for (int i = 0; i < 3; ++i) {
    if (i != active) expectedGlyph(expected, 0xac, letters[i] - 16, modeY);
    expectedGlyph(expected, labels[i], letters[i], modeY);
  }
  if (!rotated) expectedGlyph(expected, 0xac, rX - 16, rY);
  expectedGlyph(expected, 'R', rX, rY);
  for (int top : {modeY, rY}) {
    for (int y = top; y < top + reader_font::BODY_HEIGHT; ++y)
      for (int x = 0; x < 540; ++x) {
        const uint8_t value = expected[y * 540 + x];
        assert(M5.Display.fb[y * 540 + x] == value);
        const int px = rotated ? 539 - x : x, py = rotated ? 959 - y : y;
        assert(M5.Display.panel[py * 540 + px] == value);
      }
  }
  const int physicalModeY = rotated ? 960 - modeY - reader_font::BODY_HEIGHT : modeY;
  const int physicalRY = rotated ? 960 - rY - reader_font::BODY_HEIGHT : rY;
  assert(physicalModeY == 641 && physicalRY == 753);
  // A 28 px R cell stays at the physical left/button side after the flip.
  const int physicalRLeft = rotated ? 540 - rX - 28 : rX;
  assert(physicalRLeft >= 24 && physicalRLeft < 80);
  assert(countBlack(0, 0, 540, 112) == 0);  // no settings-page heading
}

void expectPartialModeChange(NetworkMode next, bool rotated) {
  const auto before = M5.Display.panel;
  networkMode = next;
  updateSettingsMode();
  const int lx = rotated ? 216 : 24, ly = rotated ? 268 : 629;
  assert(!M5.Display.lastFull);
  assert(M5.Display.lastRefreshX == lx && M5.Display.lastRefreshY == ly);
  assert(M5.Display.lastRefreshW == 300 && M5.Display.lastRefreshH == 64);
  const int px = 24, py = rotated ? 628 : 629;
  unsigned changed = 0, erased = 0;
  for (int y = 0; y < 960; ++y)
    for (int x = 0; x < 540; ++x) {
      const size_t index = size_t(y) * 540 + x;
      if (before[index] != M5.Display.panel[index]) {
        ++changed;
        assert(x >= px && x < px + 300 && y >= py && y < py + 64);
        if (before[index] == 0 && M5.Display.panel[index] == 255) ++erased;
      }
    }
  assert(changed > 0 && erased > 0);
  expectControls(next, rotated); // including actual clearing of the active prefix
}
void expectSingleQr(const char* payload, bool rotated) {
  // Match the complete symbol and its four-module quiet zone to the expected
  // payload; no second QR or text may occupy the surrounding QR-height band.
  uint8_t matrix[64 * 64];
  const int modules = qrMatrix(payload, matrix, 6);
  assert(modules == 41);
  constexpr int size = 294, scale = 6, left = 123;
  const int top = rotated ? 448 : 112;
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < 540; ++x) {
      const int col = (x - left) / scale - 4, row = y / scale - 4;
      const bool dark = x >= left && x < left + size &&
        col >= 0 && col < modules && row >= 0 && row < modules &&
        matrix[row * modules + col];
      assert(black(x, top + y) == dark);
    }
}

void expectPartialNetworkChange(bool connected, bool rotated) {
  const auto before = M5.Display.panel;
  settingsApClientConnected = connected;
  updateSettingsNetwork();
  assert(!M5.Display.lastFull);
  assert(M5.Display.lastRefreshX == 0);
  assert(M5.Display.lastRefreshY == (rotated ? 448 : 112));
  assert(M5.Display.lastRefreshW == 540 && M5.Display.lastRefreshH == 400);
  unsigned changed = 0, erased = 0;
  for (int y = 0; y < 960; ++y)
    for (int x = 0; x < 540; ++x) {
      const size_t index = size_t(y) * 540 + x;
      if (before[index] != M5.Display.panel[index]) {
        ++changed;
        // Both panel orientations reserve exactly this physical area.
        assert(y >= 112 && y < 512);
        if (before[index] == 0 && M5.Display.panel[index] == 255) ++erased;
      }
    }
  assert(changed > 0 && erased > 0);
  expectSingleQr(connected ? "http://192.168.4.1/?t=1234567890abcdef1234567890abcdef" :
                 "WIFI:T:WPA;S:PaperDIY-TEST;P:ABCDEFGHJKLM;H:false;;", rotated);
  expectControls(NetworkMode::Ap, rotated);
}
void lanPanelTransitions(){
  const String payload="{\"v\":1,\"deviceId\":\"0123456789abcdef\",\"host\":\"192.168.255.254\",\"port\":8443,\"certSha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"secret\":\"0123456789abcdef0123456789abcdef\"}";
  uint8_t matrix[64*64];const int modules=qrMatrix(payload.c_str(),matrix,10);
  assert(modules==57); // Longest IPv4 payload must fit, without truncation.
  for(bool rotated:{false,true}){
    rotateSettings(rotated);networkMode=NetworkMode::Sta;
    settingsLanPairing=payload;settingsLanType="";settingsLanName="";settingsLanState="Ready";
    showSettings(true);const int dy=rotated?336:0;
    constexpr int scale=3,size=195,left=34;
    for(int y=0;y<size;++y)for(int x=0;x<size;++x){
      const int row=y/scale-4,col=x/scale-4;
      const bool ink=row>=0&&row<modules&&col>=0&&col<modules&&matrix[row*modules+col];
      assert(black(left+x,290+dy+y)==ink);
    }
    const auto before=M5.Display.panel;
    settingsLanPairing="";settingsLanType="Push · Markdown";
    settingsLanName="Daily mathematics";settingsLanState="Receiving 42%";
    updateSettingsDelivery();assert(!M5.Display.lastFull);
    assert(M5.Display.lastRefreshY==268+dy&&M5.Display.lastRefreshH==244);
    for(int y=0;y<960;++y)for(int x=0;x<540;++x){
      if(before[y*540+x]==M5.Display.panel[y*540+x])continue;
      assert(y>=(rotated?112:268)&&y<(rotated?356:512));
    }
    assert(countBlack(32,280+dy,500,308+dy)>0);
    assert(countBlack(32,320+dy,500,348+dy)>0);
    assert(countBlack(32,360+dy,500,388+dy)>0);
    assert(countBlack(24,400+dy,240,496+dy)==0); // Old QR removed.
    expectControls(NetworkMode::Sta,rotated);
    writePGM(rotated?"ui-simulation-lan-rotated.pgm":"ui-simulation-lan.pgm",true);
  }
  settingsLanPairing="";settingsLanType="";settingsLanName="";settingsLanState="";
}
}  // namespace


void batteryPixelUpdates() {
  auto sample = [](int level, bool charging, int expectedFill, bool rotated,
                   bool shouldRefresh) {
    const unsigned before = M5.Display.refreshCount;
    const auto oldPixels = M5.Display.panel;
    M5.Power.battery = level;
    simulatedCharging = charging;
    updateLibraryBattery();
    assert(M5.Display.refreshCount == before + unsigned(shouldRefresh));
    // Assert the actual native logical AND physical battery pixels.
    for (int y = 18; y < 26; ++y) for (int x = 508; x < 526; ++x) {
      const uint8_t expected = x < 508 + expectedFill ? 0 : 255;
      assert(M5.Display.fb[y * 540 + x] == expected);
      const int px = rotated ? 539 - x : x, py = rotated ? 959 - y : y;
      assert(M5.Display.panel[py * 540 + px] == expected);
    }
    if (!shouldRefresh) assert(M5.Display.panel == oldPixels);
  };
  // Even an unknown battery must draw the frame on its first paint.
  sample(-1, false, 0, false, true);
  assert(black(506, 16));
  for (bool rotated : {false, true}) {
    rotateSettings(rotated);
    M5.Power.battery = 86;
    simulatedCharging = false;
    showLibrary(true);
    sample(87, false, 15, rotated, false);
    sample(88, false, 15, rotated, false);
    // Charging has no separate on-screen bolt and preserves discharge history.
    sample(100, true, 15, rotated, false);
    sample(50, true, 15, rotated, false);
    sample(89, false, 16, rotated, true);
    sample(88, false, 15, rotated, true);
    for (int i = 0; i < 100; ++i)
      sample(86 + i % 3, false, 15, rotated, false);
    sample(-1, false, 0, rotated, true);
    sample(0, false, 0, rotated, false);
    sample(5, false, 0, rotated, false);
    sample(6, false, 1, rotated, true);
    sample(100, false, 18, rotated, true);
    // The latest discharge estimate is retained even when its pixels matched.
    sample(87, false, 15, rotated, true);
    sample(88, false, 15, rotated, false);
    saveBatteryHistory();
    assert(prefs.getUInt("battLast", 999) == 88);
    sample(99, true, 15, rotated, false);
    saveBatteryHistory();
    assert(prefs.getUInt("battLast", 999) == 88);
  }
  simulatedCharging = false;
  rotateSettings(false);
}

int interfaceGlyphAdvance(uint32_t cp){return reader_font::glyphAdvance(reader_font::FONT_28,cp);}
void drawInterfaceGlyph(const char* text,size_t bytes,int x,int y,uint16_t color){drawBodyGlyph(text,bytes,x,y,color);}
int main() {
  displayInit();
  {
    std::string longText;for(unsigned i=0;i<600;++i)longText+="A saved reading position must remain visible while the background builds the complete index.\n";
    loadBook(longText);const auto expected=pageOffsets;
    const uint32_t anchor=uint32_t(longText.size()*3/4);
    prefs.values[bookmarkKey.c_str()]=anchor;
    assert(indexBook(false,false,nullptr,false,true));
    assert(readerHasProvisionalPages()&&readerTotalPages()==0&&currentReadingOffset()==anchor);
    simulatedBackground=true;showBook(true);const auto before=M5.Display.fb;
    while(readerIndexPending())advanceReaderIndex(2,12);
    assert(*canonicalReaderPages()==expected&&currentReadingOffset()==anchor);
    simulatedBackground=false;updateBookFooter();
    assert(std::equal(before.begin(),before.begin()+916*540,M5.Display.fb.begin()));
    assert(M5.Display.lastRefreshY==923&&M5.Display.lastRefreshH==37);
    assert(readerPageNumber()==0&&readerTotalPages()==expected.size());
    assert(turnReaderPage(1));assert(!readerHasProvisionalPages());
    cancelReaderIndex();pageOffsets.clear();bookLength=0;
  }
  batteryPixelUpdates();
  M5.Power.battery = 87;
  for (int i = 1; i <= 25; ++i) {
    if (i == 1) sdBooks.push_back("/books/模拟书籍 01.epub");
    else if (i == 2) sdBooks.push_back("/books/Simulated Book 02.epub");
    else sdBooks.push_back("/books/Book " + std::to_string(i) + ".epub");
  }
  settingsSelected = false;
  networkMode = NetworkMode::Off;
  settingsRotated = false;
  assert(nextNetworkMode(NetworkMode::Off) == NetworkMode::Sta);
  assert(nextNetworkMode(NetworkMode::Sta) == NetworkMode::Ap);
  assert(nextNetworkMode(NetworkMode::Ap) == NetworkMode::Usb);
  assert(nextNetworkMode(NetworkMode::Usb) == NetworkMode::Off);
  assert(bodyFontHasGlyph('&'));

  // One virtual settings book leads the library; real books 0..20 fill rows 1..21.
  showLibrary(true);
  assert(M5.Display.lastFull);
  assert(M5.Display.lastRefreshX == 0 && M5.Display.lastRefreshW == 540);
  assert(!black(17, 57));                      // Network & Rotation row is not selected
  assert(black(17, 96));                      // first book row is selected
  assert(countBlack(24, 57, 500, 94) > 0);     // Network & Rotation text is drawn
  assert(countBlack(24, 875, 500, 913) > 0);   // 21st book row is drawn
  assert(!black(20, 914));                     // next book row is not on page 1
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

  // First virtual setting selected, then real book 0 selected again.
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

  // Virtual settings stay on page 1. Book 21 starts page 2 at row 0.
  settingsSelected=false;selectedBook = 21;
  showLibrary(true);
  assert(black(17, 57));
  assert(!black(17, 96));
  assert(countBlack(24, 57, 500, 94) > 0);     // book title, not Network & Rotation
  selectedBook = 0;
  settingsSelected = false;
  showLibrary(true);

  // Verify all complete settings scenes, including their actual physical panel
  // orientation. The network area is mirrored to y=448 when the controls move.
  settingsApSsid = "PaperDIY-TEST";
  settingsApPassword = "ABCDEFGHJKLM";
  settingsApUrl = "http://192.168.4.1/?t=1234567890abcdef1234567890abcdef";
  settingsApClientConnected = false;
  settingsStaSsid = "Home";
  settingsStaConfigured = true;
  settingsStaConnected = true;
  settingsStaIp = "192.168.1.23";
  settingsStaError = "";
  for (NetworkMode mode : {NetworkMode::Off, NetworkMode::Sta,
                           NetworkMode::Ap, NetworkMode::Usb}) {
    rotateSettings(false);
    networkMode = mode;
    showSettings(true);
    assert(M5.Display.lastFull);
    expectControls(mode, false);
    const auto info = crop(0, 112, 540, 400);
    if (mode == NetworkMode::Off) {
      assert(countBlack(0, 112, 540, 600) == 0);
      writePGM("ui-simulation-settings.pgm", true);
    } else if (mode == NetworkMode::Ap) {
      expectSingleQr("WIFI:T:WPA;S:PaperDIY-TEST;P:ABCDEFGHJKLM;H:false;;", false);
      assert(countBlack(200, 414, 340, 438) > 0);
      assert(countBlack(24, 448, 500, 472) > 0);
      assert(countBlack(24, 480, 500, 504) > 0);
      writePGM("ui-simulation-settings-ap.pgm", true);
    } else if (mode == NetworkMode::Sta) {
      assert(countBlack(32, 112, 200, 150) > 0);
      assert(countBlack(32, 160, 500, 190) > 0);
      assert(countBlack(32, 196, 300, 226) > 0);
      assert(countBlack(32, 232, 400, 262) > 0);
    } else {
      assert(countBlack(32, 112, 400, 150) > 0);
      assert(countBlack(32, 160, 450, 190) > 0);
    }

    rotateSettings(true);
    showSettings(true);
    assert(M5.Display.lastFull);
    expectControls(mode, true);
    assert(crop(0, 448, 540, 400) == info);
    if (mode == NetworkMode::Ap)
      expectSingleQr("WIFI:T:WPA;S:PaperDIY-TEST;P:ABCDEFGHJKLM;H:false;;", true);
    for (int y = 448; y < 848; ++y)
      for (int x = 0; x < 540; ++x)
        assert(M5.Display.panel[(959 - y) * 540 + 539 - x] ==
               info[(y - 448) * 540 + x]);
    if (mode == NetworkMode::Off)
      writePGM("ui-simulation-settings-rotated.pgm", true);
    else if (mode == NetworkMode::Ap)
      writePGM("ui-simulation-settings-ap-rotated.pgm", true);
  }

  lanPanelTransitions();

  // A station association replaces the join symbol with the web symbol. A
  // disconnect restores the join symbol. Each update preserves controls and
  // erases the previous QR, including when the framebuffer is rotated.
  for (bool rotated : {false, true}) {
    rotateSettings(rotated);
    networkMode = NetworkMode::Ap;
    settingsApClientConnected = false;
    showSettings(true);
    expectPartialNetworkChange(true, rotated);
    writePGM(rotated ? "ui-simulation-settings-web-rotated.pgm" :
                       "ui-simulation-settings-web.pgm", true);
    expectPartialNetworkChange(false, rotated);
    expectPartialNetworkChange(true, rotated);
  }

  // Partial row updates must preserve every other physical pixel and remove
  // the previous prefix in both orientations. No network area redraw is needed.
  for (bool rotated : {false, true}) {
    rotateSettings(rotated);
    networkMode = NetworkMode::Off;
    showSettings(true);
    for (NetworkMode next : {NetworkMode::Sta, NetworkMode::Ap, NetworkMode::Usb})
      expectPartialModeChange(next, rotated);
  }

  settingsApSsid = "";
  settingsApPassword = "";
  settingsApUrl = "";
  settingsApClientConnected = false;
  settingsStaSsid = "";
  settingsStaIp = "";
  settingsStaError = "";
  settingsStaConfigured = false;
  settingsStaConnected = false;
  networkMode = NetworkMode::Off;
  rotateSettings(false);
  showSettings(true);

  assert(reader_typography::style.flags()==3);

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

  // Reviewed native Latin spacing, italic and formula pixels, plus native
  // 24 px code advances. Also protect the page boundary: widow avoidance must
  // not move a line after its first word has already begun on the prior page.
  std::string asciiSource="## ASCII typography\n\n";
  for(unsigned pass=0;pass<6;++pass)asciiSource+="A quiet observer checks 0123456789, **bold words**, *italic words*, and a link [label](https://example.invalid). Mixed 中文 and Ελληνικά remain unchanged. Inline $x_1+2$ shares the baseline.\n\n";
  asciiSource+="```\n";for(unsigned cp=32;cp<=126;++cp)asciiSource+=char(cp);asciiSource+="\n```\n";
  loadBook(asciiSource,true);
  assert(bookMarkdown && !bookMarkupFallback);
  assert((pageOffsets==std::vector<uint32_t>{0,807}));
  uint64_t asciiRasterHash=1469598103934665603ULL,proseRasterHash=asciiRasterHash;
  capturedCodeGlyphs.clear();captureCodeGlyphs=true;captureBodyGlyphs=true;
  for(size_t i=0;i<pageOffsets.size();++i){
    capturedBodyGlyphs.clear();
    M5.Display.fillScreen(TFT_WHITE);
    assert(layoutPage(pageOffsets[i],true)==(i+1<pageOffsets.size()?pageOffsets[i+1]:bookLength));
    if(i+1<pageOffsets.size()){
      // This fixture has no literal prose hyphens. A trailing drawn '-' would
      // be an automatic split; the old widow rule left "re-" / "main" here.
      assert(!capturedBodyGlyphs.empty()&&capturedBodyGlyphs.back().cp!='-');
      const size_t boundary=pageOffsets[i+1];
      auto letter=[](char ch){return (ch>='A'&&ch<='Z')||(ch>='a'&&ch<='z');};
      assert(boundary>0&&boundary<asciiSource.size());
      assert(!(letter(asciiSource[boundary-1])&&letter(asciiSource[boundary])));
    }
    char output[80];snprintf(output,sizeof(output),"ui-simulation-ascii-%u.pgm",unsigned(i));writePGM(output);
    for(size_t pixel=0;pixel<M5.Display.fb.size();++pixel){
      asciiRasterHash=(asciiRasterHash^M5.Display.fb[pixel])*1099511628211ULL;
      const uint8_t prose=i==1&&pixel/540>=529?255:M5.Display.fb[pixel];
      proseRasterHash=(proseRasterHash^prose)*1099511628211ULL;
    }
  }
  captureCodeGlyphs=false;captureBodyGlyphs=false;
  assert(proseRasterHash==0xeac0e649a851def4ULL);
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
  assert(asciiRasterHash==0x982b0cd3d4bfc53aULL);

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
  puts("UI simulation: equal mode spacing, physical rotation, button-side R, single-QR transitions, intact network panels, partial clearing, reading hang-zone: PASS");
}
