// Compile the actual reader, replacing only hardware and NVS boundaries.
#include <cassert>
#include <vector>
#include "../src/reader.cpp"

const lgfx::IFont* bodyFont(){static lgfx::IFont font;return &font;}
const lgfx::IFont* readerFont(){return bodyFont();}
int bodyGlyphAdvance(uint32_t cp){return cp<32?0:cp<128?10:28;}
uint32_t readerCodepoint(const char* text,size_t bytes){return reader_font::decodeCodepoint(text,bytes);}
void drawBodyGlyph(const char*,size_t,int,int,uint32_t){}

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
int main(){
 std::string text;
 for(int i=0;i<300;++i)text+="Paragraph "+std::to_string(i)+". Enough text to wrap into stable pages. 中文段落。\n";
 reset(text);const std::string current=contentKey(false),legacy=contentKey(true);
 assert(current[0]=='c'&&legacy[0]=='b');

 // Prefer the existing current-format bookmark over every migration source.
 prefs.values[current]=4500;prefs.values[legacy]=1200;prefs.values["slegacy"]=1;
 auto anchor=bookmarks::capture(text.data(),text.size(),2100);
 assert(indexBook(false,true,&anchor,true));assertContains(4500);
 assert(saveReadingProgress());assert(prefs.values[legacy]==1200&&prefs.values["slegacy"]==1);
 assert(prefs.last=="/test.epub");

 // A same-text legacy key migrates without deleting or overwriting the old key.
 reset(text);prefs.values[legacy]=3200;
 assert(indexBook(false,true,nullptr,true));assertContains(3200);
 assert(prefs.values.find(current)==prefs.values.end()); // Indexing is read-only.
 assert(saveReadingProgress());assert(prefs.values[legacy]==3200);
 assert(prefs.values[current]==pageOffsets[pageNumber]);

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
 assert(prefs.values[changedKey]==pageOffsets[pageNumber]);

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
 puts("reader integration: recovery precedence, legacy context, corrupt-cache guard, NVS faults, preserved old keys: PASS");
}
