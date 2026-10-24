#pragma once
#include <cstdint>
#include <cstddef>

namespace text_language {
enum class Language : uint8_t { None=0, EnglishUS=1, EnglishGB=2 };
inline bool valid(Language language){return unsigned(language)<=unsigned(Language::EnglishGB);}
inline bool space(char c){return c==' '||c=='\t'||c=='\r'||c=='\n';}
// Explicit English package metadata enables automatic patterns. Missing,
// malformed and non-English tags do not guess from ASCII text or filenames.
// Unspecified English uses US rules; an explicit GB region selects UK rules.
inline Language fromTag(const char* text){
  if(!text)return Language::None;
  while(space(*text))++text;
  char tag[36]={};size_t size=0;
  while(*text&&!space(*text)){
    if(size==sizeof(tag)-1)return Language::None;
    unsigned char c=*text++;
    if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'))return Language::None;
    tag[size++]=c>='A'&&c<='Z'?char(c+'a'-'A'):char(c);
  }
  while(space(*text))++text;
  if(*text||size<2||tag[0]!='e'||tag[1]!='n'||(size>2&&tag[2]!='-'))return Language::None;
  bool british=false;
  for(size_t at=3;at<size;){
    const size_t start=at;
    while(at<size&&tag[at]!='-')++at;
    const size_t length=at-start;
    if(!length||length>8)return Language::None;
    // Extensions/private use cannot change the language's earlier region.
    if(length==1){
      if(at==size)return Language::None;
      while(at<size){
        const size_t extension=++at;
        while(at<size&&tag[at]!='-')++at;
        if(at-extension<1||at-extension>8)return Language::None;
      }
      break;
    }
    if(length==2&&tag[start]=='g'&&tag[start+1]=='b')british=true;
    if(at<size&&++at==size)return Language::None;
  }
  if(size==3)return Language::None;
  return british?Language::EnglishGB:Language::EnglishUS;
}
}
