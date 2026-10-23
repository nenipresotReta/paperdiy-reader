// Compile the actual reader, replacing only hardware and NVS boundaries.
#include <cassert>
#include <vector>
#include <array>
#include <tuple>
#include <type_traits>
#include <set>
#include "native_font_data.h"
#include "../src/reader.cpp"

const lgfx::IFont* bodyFont(){static lgfx::IFont font;return &font;}
const lgfx::IFont* readerFont(){return bodyFont();}
int bodyGlyphAdvance(uint32_t cp){return reader_font::glyphAdvance((reader_settings::geometry().pixels==24?reader_font::FONT_24:reader_font::FONT_28),cp);}
const reader_font::Glyph* bodyGlyph(uint32_t cp){return reader_font::findGlyph((reader_settings::geometry().pixels==24?reader_font::FONT_24:reader_font::FONT_28),cp);}
uint32_t readerCodepoint(const char* text,size_t bytes){return reader_font::decodeCodepoint(text,bytes);}
using Mark=std::tuple<std::string,int,int>;
static std::vector<Mark> bodyMarks;
void drawBodyGlyph(const char* text,size_t size,int x,int y,uint32_t){bodyMarks.emplace_back(std::string(text,size),x,y);}
void drawReaderGlyph(const char*,size_t,int,int,uint32_t){}
int readerGlyphWidth(uint32_t cp){return bodyGlyphAdvance(cp);}
bool bodyFontHasGlyph(uint32_t){return true;}

static std::vector<char> bytes;
void setBook(const std::string& text){
 bytes.assign(text.begin(),text.end());bytes.push_back(0);
 book=bytes.data();bookLength=text.size();bookIdentity="/test.epub";
 bookmarkKey="slegacy";pageOffsets.clear();pageNumber=0;
}
void reset(const std::string& text){
 prefs=Preferences{};assert(initializeProgress());setBook(text);
}
void assertContains(uint32_t offset){
 assert(!pageOffsets.empty());
 assert(pageOffsets[pageNumber]<=offset);
 assert(pageNumber+1==pageOffsets.size()||offset<pageOffsets[pageNumber+1]);
}
static_assert(std::is_const<decltype(reader_typography::style)>::value,"Standalone default style remains immutable");
void typographyRegression(){
 std::string source;
 for(unsigned i=0;i<160;++i)source+="她说：“《春日》很好。”中文EPUB3第12章。**原文** 中 A 。 ";
 assert(reader_typography::style.flags()==3);
 for(bool markdown:{false,true}){
  clearMarkdown();bookMarkdown=markdown;bookMarkupFallback=false;bookEnglishHyphens=true;
  reset(source);const std::string key=contentKey(false);
  const uint32_t exact=uint32_t(source.find("中文",source.size()/2));assert(exact!=UINT32_MAX);
  prefs.values[key]=exact;prefs.last=bookIdentity;
  // A legacy disabled preference cannot alter the fixed production style.
  prefs.values["cnStyle"]=0;
  const auto original=prefs.values;const unsigned writes=prefs.writes;
  std::vector<uint32_t> baseline;
  for(unsigned repeat=0;repeat<3;++repeat){
   pageOffsets.clear();assert(indexBook());assert(bookmarkKey==key&&currentReadingOffset()==exact);
   assertContains(exact);if(!repeat)baseline=pageOffsets;else assert(pageOffsets==baseline);
   assert(prefs.values==original&&prefs.writes==writes&&reader_typography::style.flags()==3);
   for(size_t i=0;i<pageOffsets.size();++i){
    const size_t from=pageOffsets[i],end=i+1<pageOffsets.size()?pageOffsets[i+1]:bookLength;
    bodyMarks.clear();assert(layoutPage(from,true)==end);
   }
   assert(currentReadingOffset()==exact&&saveReadingProgress());
   assert(prefs.values==original&&prefs.writes==writes);
   assert(bookLength==source.size()&&!memcmp(book,source.data(),source.size()));
  }
 }
 clearMarkdown();bookMarkdown=false;bodyMarks.clear();
}
void settingsReflowRegression(){
 std::string source;for(unsigned i=0;i<400;++i)source+="中文Markdown3与标点：“hello.” Internationalization allows different line lengths.\n";
 for(bool markdown:{false,true}){
  reader_settings::current()={};bookMarkdown=markdown;bookMarkupFallback=false;bookEnglishHyphens=true;reset(source);
  const auto key=contentKey(false);const auto exact=uint32_t(source.find("Internationalization",source.size()/2));prefs.values[key]=exact;
  std::set<size_t> totals;
  for(uint8_t preset:{uint8_t(0),uint8_t(1),uint8_t(2)}){
   auto& c=reader_settings::current();c.preset=preset;c.spacing=preset==0?2:4;c.hyphenation=preset!=0;c.justify=preset!=0;
   assert(indexBook());assertContains(exact);assert(currentReadingOffset()==exact);totals.insert(pageOffsets.size());
   assert(saveReadingProgress()&&prefs.values[key]==exact);
   for(auto offset:pageOffsets){const auto end=layoutPage(offset,false);bodyMarks.clear();assert(layoutPage(offset,true)==end);}
  }
  assert(totals.size()==3);assert(!memcmp(book,source.data(),source.size()));
 }
 reader_settings::current()={};clearMarkdown();bookMarkdown=false;bookMarkupFallback=false;
}
int main(){
 settingsReflowRegression();
 typographyRegression();
 std::string text;
 for(int i=0;i<300;++i)text+="Paragraph "+std::to_string(i)+". Enough text to wrap into stable pages. 中文段落。\n";
 reset(text);const std::string current=contentKey(false),legacy=contentKey(true);
 assert(current[0]=='c'&&legacy[0]=='b');

 // Prefer the existing current-format bookmark over every migration source.
 prefs.values[current]=4500;prefs.values[legacy]=1200;prefs.values["slegacy"]=1;
 auto anchor=bookmarks::capture(text.data(),text.size(),2100);
 assert(indexBook(false,true,&anchor,true));assertContains(4500);
 assert(saveReadingProgress());assert(prefs.values[legacy]==1200&&prefs.values["slegacy"]==1);
 assert(prefs.values[current]==4500); // Opening/reflow must retain the exact source anchor.
 assert(prefs.last=="/test.epub");

 // A same-text legacy key migrates without deleting or overwriting the old key.
 reset(text);prefs.values[legacy]=3200;
 assert(indexBook(false,true,nullptr,true));assertContains(3200);
 assert(prefs.values.find(current)==prefs.values.end()); // Indexing is read-only.
 assert(saveReadingProgress());assert(prefs.values[legacy]==3200);
 assert(prefs.values[current]==3200);

 // Translate changed extraction text using old context; persist only the new key.
 reset(text);uint32_t oldOffset=text.find("Paragraph 150.");
 anchor=bookmarks::capture(text.data(),text.size(),oldOffset);
 prefs.values[legacy]=oldOffset;
 std::string changed="New introduction.\n"+text;
 for(size_t p=0;(p=changed.find("to wrap",p))!=std::string::npos;p+=8)changed.insert(p," ");
 setBook(changed);std::string changedKey=contentKey(false);
 uint32_t mapped=0;assert(bookmarks::relocate(anchor,changed.data(),changed.size(),mapped,[](){}));
 assert(indexBook(false,true,&anchor,true));assertContains(mapped);
 assert(saveReadingProgress());assert(prefs.values[legacy]==oldOffset);
 assert(prefs.values[changedKey]==mapped);

 // An unreadable cache requires recovery; never manufacture a new bookmark at 0.
 reset(changed);prefs.values[legacy]=oldOffset;
 auto before=prefs.values;
 assert(!indexBook(false,true,nullptr,true));
 assert(pageOffsets.empty()&&prefs.values==before&&prefs.writes==0);
 assert(!saveReadingProgress()&&prefs.values==before);
 setBook(changed);auto missing=bookmarks::capture("unknown passage",15,4);
 assert(!indexBook(false,true,&missing,true)&&prefs.values==before&&prefs.writes==0);

 // Fail closed when a known current, legacy, or page key cannot be read.
 for(const auto& key:std::vector<std::string>{current,legacy,"slegacy"}){
   reset(text);prefs.values[key]=2;prefs.failReadKey=key;before=prefs.values;
   assert(!indexBook(false,true,nullptr,true));
   assert(pageOffsets.empty()&&prefs.values==before&&prefs.writes==0);
 }

 // A failed migration write remains retryable and preserves the source bookmark.
 reset(text);prefs.values[legacy]=3200;
 assert(indexBook(false,true,nullptr,true));prefs.failWrite=true;
 assert(!saveReadingProgress()&&progressSaveFailed);
 assert(prefs.values[legacy]==3200&&prefs.values.find(current)==prefs.values.end());
 prefs.failWrite=false;prefs.failResume=true;
 assert(!saveReadingProgress()&&progressSaveFailed&&prefs.last.empty());
 prefs.failResume=false;
 assert(saveReadingProgress()&&!progressSaveFailed&&prefs.last=="/test.epub");
 assert(prefs.values[legacy]==3200);

 // Truly new books may start at zero; historical page keys remain read-only.
 reset(text);assert(indexBook(false,true,nullptr,false)&&pageNumber==0);
 reset(text);prefs.values["slegacy"]=2;
 assert(indexBook(false,true,nullptr,true));assert(pageNumber>0);
 assert(saveReadingProgress()&&prefs.values["slegacy"]==2);
 // A corrupt/outdated offset must not be silently rewritten to an endpoint.
 for(uint32_t invalid : {uint32_t(text.size()),UINT32_MAX-1}){
   reset(text);prefs.values[current]=invalid;auto original=prefs.values;
   assert(!indexBook(false,true,nullptr,false));
   assert(pageOffsets.empty()&&prefs.values==original&&prefs.writes==0);
   assert(!saveReadingProgress()&&prefs.values==original);
 }
 // A previously saved raw-source offset must survive rebuilding from a
 // plain-text fallback layout into Markdown, including a UTF-8 BOM and syntax
 // that disappears from the rendered buffer. Both source extensions retain
 // independent book identities and the exact original bytes.
 std::string markdown="\xef\xbb\xbf# 阅读笔记\n\n";
 for(int i=0;i<90;++i){
   markdown+="## Section "+std::to_string(i)+"\n\n";
   markdown+="- **Chinese 中文** and *emphasis* with `code` and $x_1$. ";
   markdown+="A paragraph with enough words to wrap across multiple lines.\n\n";
 }
 std::string firstMarkdownKey;
 for(const std::string& path:{std::string("/notes.md"),std::string("/notes.markdown")}){
   clearMarkdown();bookMarkdown=false;bookMarkupFallback=false;reset(markdown);
   bookIdentity=path;const std::string rawKey=contentKey(false);
   if(firstMarkdownKey.empty())firstMarkdownKey=rawKey;
   else assert(rawKey!=firstMarkdownKey);
   assert(indexBook(false,true,nullptr,false)&&pageOffsets.size()>3);
   const auto plainPages=pageOffsets;
   pageNumber=uint32_t(pageOffsets.size()/2);
   const uint32_t rawSaved=pageOffsets[pageNumber];
   assert(saveReadingProgress()&&prefs.values[rawKey]==rawSaved&&prefs.last==path);
   const auto saved=prefs.values;const unsigned writes=prefs.writes;

   pageOffsets.clear();pageNumber=0;bookMarkdown=true;
   assert(indexBook(false,true,nullptr,true));
   assert(bookMarkdown&&!bookMarkupFallback&&bookmarkKey==rawKey);
   assert(pageOffsets!=plainPages);assertContains(rawSaved);
   assert(bookLength==markdown.size()&&!memcmp(book,markdown.data(),bookLength));
   assert(prefs.values==saved&&prefs.writes==writes&&prefs.last==path);
   // Every rebuilt page boundary is a raw-source UTF-8 boundary, and using
   // that boundary in the production renderer reaches exactly the next page.
   for(size_t i=0;i<pageOffsets.size();++i){
     const size_t start=pageOffsets[i];assert(start<bookLength);
     assert((uint8_t(book[start])&0xc0)!=0x80);
     const size_t end=i+1<pageOffsets.size()?pageOffsets[i+1]:bookLength;
     assert(layoutPage(start,false)==end&&end>start);
   }
   assert(saveReadingProgress());const uint32_t exactRaw=rawSaved;
   const uint32_t expectedPage=pageNumber;const auto rebuiltPages=pageOffsets;
   const auto rebuiltSaved=prefs.values;const unsigned rebuiltWrites=prefs.writes;
   assert(rebuiltSaved.at(rawKey)==exactRaw);
   clearMarkdown();pageOffsets.clear();pageNumber=0;
   assert(indexBook(false,true,nullptr,true)&&pageNumber==expectedPage);
   assert(pageOffsets==rebuiltPages);assertContains(exactRaw);
   assert(bookmarkKey==rawKey&&prefs.values==rebuiltSaved&&prefs.writes==rebuiltWrites);
   assert(bookLength==markdown.size()&&!memcmp(book,markdown.data(),bookLength));
   // Reopening/reflowing repeatedly must not ratchet the source anchor backward.
   for(unsigned pass=0;pass<8;++pass){
     clearMarkdown();bookMarkdown=(pass%2)==0;pageOffsets.clear();
     assert(indexBook(false,true,nullptr,true));assertContains(exactRaw);
     const unsigned beforeSave=prefs.writes;
     assert(currentReadingOffset()==exactRaw&&saveReadingProgress());
     assert(prefs.values.at(rawKey)==exactRaw&&prefs.writes==beforeSave);
   }
   // A real page turn establishes its page start, including failure/retry and
   // turning back. It must not resurrect the pre-turn reflow anchor.
   const uint32_t previousPage=pageNumber;
   assert(pageNumber+1<pageOffsets.size());++pageNumber;
   const uint32_t nextStart=pageOffsets[pageNumber];
   prefs.failWrite=true;assert(!saveReadingProgress()&&progressSaveFailed);
   assert(prefs.values.at(rawKey)==exactRaw&&currentReadingOffset()==nextStart);
   prefs.failWrite=false;assert(saveReadingProgress()&&prefs.values.at(rawKey)==nextStart);
   pageNumber=previousPage;assert(saveReadingProgress());
   assert(prefs.values.at(rawKey)==pageOffsets[previousPage]);
 }
 clearMarkdown();bookMarkdown=false;bookMarkupFallback=false;
 puts("reader integration: recovery precedence, legacy context, corrupt-cache guard, NVS faults, preserved old keys, Markdown raw-source rebuild/resume: PASS");
}
