#pragma once
#include <cstddef>
#include <cstdint>
#include "settings_registry.h"

// Library rows: the settings books, then Recent while it names a listed book,
// then every book by path. Recent repeats a listed book; it never replaces it.
namespace library_rows {
enum class Kind : uint8_t { Settings, Recent, Book };
struct Row { Kind kind; size_t index; }; // settings entry or book index
struct Layout {
  size_t books;
  size_t recent; // book index shown by Recent; >= books hides the row
  bool hasRecent() const { return recent<books; }
  size_t count() const { return settings_registry::COUNT+(hasRecent()?1:0)+books; }
};
inline size_t cursor(const Layout& layout,Row row) {
  if(row.kind==Kind::Settings)return row.index;
  if(row.kind==Kind::Recent&&layout.hasRecent())return settings_registry::COUNT;
  return settings_registry::COUNT+(layout.hasRecent()?1:0)+row.index;
}
inline Row at(const Layout& layout,size_t cursor) {
  if(cursor<settings_registry::COUNT)return {Kind::Settings,cursor};
  cursor-=settings_registry::COUNT;
  if(layout.hasRecent()){
    if(cursor==0)return {Kind::Recent,layout.recent};
    --cursor;
  }
  return {Kind::Book,cursor};
}
}

// The most recently opened ordinary book. Push has its own row and is never
// recorded. Only a change of book writes NVS; page turns never do.
namespace recent_book {
constexpr const char* KEY = "recentBk";
template<class Preferences>
bool save(Preferences& prefs,const char* path) {
  size_t length=0;while(path[length])++length;
  if(!length||path[0]!='/')return false;
  if(prefs.isKey(KEY)&&prefs.getString(KEY,"")==path)return true;
  return prefs.putString(KEY,path)==length&&prefs.getString(KEY,"")==path;
}
template<class Books>
size_t index(const char* path,const Books& books) {
  if(!path[0])return books.size();
  for(size_t i=0;i<books.size();++i)if(books[i]==path)return i;
  return books.size();
}
}
