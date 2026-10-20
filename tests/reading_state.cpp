#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include <openssl/sha.h>
#include "bookmark_store.h"
#include "bookmark_migration.h"
#include "pagination.h"
struct Preferences {
 std::map<std::string,uint32_t> offsets;
 std::string last;
 bool failPosition=false,failResume=false;
 unsigned writes=0;
 uint32_t getUInt(const char* key,uint32_t fallback){auto i=offsets.find(key);return i==offsets.end()?fallback:i->second;}
 size_t putUInt(const char* key,uint32_t value){++writes;if(failPosition)return 0;offsets[key]=value;return 4;}
 std::string getString(const char*,const char*){return last;}
 size_t putString(const char*,const char* path){++writes;if(failResume)return 0;last=path;return last.size();}
};
std::array<unsigned char,32> digest(const std::string& path,const std::string& text,bool legacy){
 SHA256_CTX sha;SHA256_Init(&sha);
 bookmarks::identity([&](const uint8_t* p,size_t n){SHA256_Update(&sha,p,n);},path.data(),path.size(),text.data(),text.size(),legacy);
 std::array<unsigned char,32> out;SHA256_Final(out.data(),&sha);return out;
}
int main(){
 using bookmarks::SaveResult;
 Preferences prefs;
 assert(bookmarks::save(prefs,false,"c1",10,"/a",2)==SaveResult::Unavailable);
 assert(prefs.writes==0);
 prefs.failPosition=true;
 assert(bookmarks::save(prefs,true,"c1",10,"/a",2)==SaveResult::PositionFailed);
 assert(prefs.last.empty()&&prefs.offsets.empty());
 prefs.failPosition=false;prefs.failResume=true;
 assert(bookmarks::save(prefs,true,"c1",10,"/a",2)==SaveResult::ResumeFailed);
 assert(prefs.offsets["c1"]==10&&prefs.last.empty());
 prefs.failResume=false;
 assert(bookmarks::save(prefs,true,"c1",10,"/a",2)==SaveResult::Saved);
 unsigned writes=prefs.writes;
 assert(bookmarks::save(prefs,true,"c1",10,"/a",2)==SaveResult::Saved&&prefs.writes==writes);
 assert(bookmarks::save(prefs,true,"c2",40,"/b",2)==SaveResult::Saved);
 assert(prefs.offsets["c1"]==10&&prefs.offsets["c2"]==40);
 assert(bookmarks::save(prefs,true,"c1",10,"/a",2)==SaveResult::Saved);
 assert(prefs.last=="/a"&&prefs.offsets["c2"]==40);
 assert(digest("/story.txt",".txtHello, world!",true)==digest("/story.txt.txt","Hello, world!",true));
 assert(digest("/story.txt",".txtHello, world!",false)!=digest("/story.txt.txt","Hello, world!",false));
 // The old display text gains spaces, paragraph breaks and an earlier note.
 std::string old="Intro text\n";
 for(int i=0;i<300;i++)old+="第"+std::to_string(i)+"段。Hello"+std::to_string(i)+"world.\n";
 size_t cursor=old.find("第150段");auto anchor=bookmarks::capture(old.data(),old.size(),cursor);
 std::string changed="New prefatory text\n"+old;
 for(size_t p=0;(p=changed.find("world",p))!=std::string::npos;p+=6)changed.insert(p," ");
 uint32_t relocated=0;unsigned ticks=0;
 assert(bookmarks::relocate(anchor,changed.data(),changed.size(),relocated,[&](){++ticks;}));
 assert(changed.compare(relocated,std::string("第150段").size(),"第150段")==0&&ticks>0);
 assert(!bookmarks::relocate(anchor,"unrelated",9,relocated,[](){}));
 auto start=bookmarks::capture(old.data(),old.size(),0);
 assert(bookmarks::relocate(start,changed.data(),changed.size(),relocated,[](){})&&relocated==0);
 assert(!bookmarks::capture(old.data(),old.size(),old.size()).valid);
 // Real layout core: cached offsets and drawn text agree, UTF-8 boundaries
 // survive wrapping, CRLF/tab handling advances, and malformed geometry
 // cannot create an infinite pagination loop.
 pagination::Geometry g;g.left=0;g.right=12;g.top=0;g.bottom=10;g.lineHeight=10;
 std::string text="a中文b\r\n\tcdef\n\nend";
 auto width=[](const char*,size_t n){return n==1?3:6;};
 auto next=[&](size_t p){return pagination::page(text.data(),text.size(),p,g,width,[](const char*,size_t,int,int){},[](){});};
 std::vector<uint32_t> offsets;assert(pagination::index(text.size(),offsets,next));
 size_t consumed=0;
 for(size_t i=0;i<offsets.size();++i){
   assert((uint8_t(text[offsets[i]])&0xc0)!=0x80);
   auto end=pagination::page(text.data(),text.size(),offsets[i],g,width,[&](const char*,size_t n,int x,int y){assert(x>=0&&y<=10);consumed+=n;},[](){});
   assert(end==(i+1<offsets.size()?offsets[i+1]:text.size()));
 }
 assert(consumed>0);
 assert(!pagination::index(text.size(),offsets,next,1)&&offsets.empty());
 assert(!pagination::index(text.size(),offsets,[](size_t p){return p;})&&offsets.empty());
 g.right=0;assert(pagination::page(text.data(),text.size(),0,g,width,[](const char*,size_t,int,int){},[](){})==0);
 g.right=1;g.bottom=0;assert(pagination::page("中",3,0,g,width,[](const char*,size_t,int,int){},[](){})==3);
 char partial[]={char(0xe4),char(0xb8)};assert(pagination::utf8Bytes(partial,2)==1);
 puts("NVS failures/retries, independent books, identity collision, text migration, UTF-8 pagination: PASS");
}
