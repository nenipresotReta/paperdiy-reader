#pragma once
#include <cstddef>
#include <cstring>
namespace document_format {
enum class Kind { Text, Epub, Markdown, Unsupported };
inline bool suffix(const char* path,const char* extension){
  if(!path)return false;
  size_t n=std::strlen(path),e=std::strlen(extension);if(n<e)return false;
  for(size_t i=0;i<e;++i){
    char c=path[n-e+i];if(c>='A'&&c<='Z')c+=char('a'-'A');
    if(c!=extension[i])return false;
  }
  return true;
}
inline Kind detect(const char* path){
  if(suffix(path,".md")||suffix(path,".markdown"))return Kind::Markdown;
  if(suffix(path,".epub"))return Kind::Epub;
  if(suffix(path,".txt"))return Kind::Text;
  return Kind::Unsupported;
}
inline size_t sourceLimit(Kind kind){
  return kind==Kind::Epub?64u*1024*1024:kind==Kind::Markdown?1024u*1024:
         kind==Kind::Text?4u*1024*1024:0;
}
inline size_t extensionLength(const char* path){
  if(suffix(path,".markdown"))return 9;
  if(suffix(path,".epub"))return 5;
  if(suffix(path,".txt"))return 4;
  if(suffix(path,".md"))return 3;
  return 0;
}
}
