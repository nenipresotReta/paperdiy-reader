#pragma once
// Bounded declaration subset, deliberately not a general CSS engine. Accepts
// type, .class, #id and universal selectors (and comma lists), inheritance and
// inline overrides. Unsupported compound/descendant/@ rules are ignored.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <cstring>
#include <tinyxml2.h>
namespace epub { namespace vertical_style {
struct Values { int8_t mode=-1,combine=-1; };
inline bool space(char c){return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f';}
inline bool equal(const char* s,size_t n,const char* value){return std::strlen(value)==n&&!std::memcmp(s,value,n);}
inline void trim(const char*& s,size_t& n){while(n&&space(*s)){++s;--n;}while(n&&space(s[n-1]))--n;}
inline Values declarations(const char* s,size_t n){
  Values v;size_t at=0;
  while(at<n){size_t end=at;while(end<n&&s[end]!=';')++end;size_t colon=at;while(colon<end&&s[colon]!=':')++colon;
    if(colon<end){const char* name=s+at;size_t names=colon-at;trim(name,names);
      const char* value=s+colon+1;size_t values=end-colon-1;trim(value,values);
      if(equal(name,names,"writing-mode")||equal(name,names,"-epub-writing-mode")||equal(name,names,"-webkit-writing-mode")){
        if(equal(value,values,"vertical-rl"))v.mode=1;else if(!equal(value,values,"inherit")&&!equal(value,values,"unset"))v.mode=0;
      }else if(equal(name,names,"text-combine-upright")||equal(name,names,"-epub-text-combine")){
        if(equal(value,values,"all")||equal(value,values,"digits 2")||equal(value,values,"horizontal"))v.combine=1;
        else if(equal(value,values,"none"))v.combine=0;
      }
    }at=end+1;
  }return v;
}
class Sheet {
  struct Rule{char selector[64];Values value;uint8_t specificity;};
  std::unique_ptr<Rule[]> rules_;size_t count_=0;bool failed_=false;
 public:
  void clear(){count_=0;failed_=false;}
  bool failed()const{return failed_;}
  void add(const char* css,size_t bytes){
    if(!css||bytes>65536)return;if(!rules_){rules_.reset(new(std::nothrow) Rule[128]{});if(!rules_){failed_=true;return;}}size_t at=0;
    while(at<bytes){
      if(at+1<bytes&&css[at]=='/'&&css[at+1]=='*'){at+=2;while(at+1<bytes&&(css[at]!='*'||css[at+1]!='/'))++at;at=std::min(bytes,at+2);continue;}
      while(at<bytes&&space(css[at]))++at;size_t open=at;while(open<bytes&&css[open]!='{')++open;if(open==bytes)break;
      size_t end=open+1;while(end<bytes&&css[end]!='}')++end;if(end==bytes)break;
      bool nested=false;for(size_t k=open+1;k<end;++k)if(css[k]=='{')nested=true;
      const Values v=declarations(css+open+1,end-open-1);
      if(!nested&&(v.mode>=0||v.combine>=0))for(size_t from=at;from<open;){size_t next=from;while(next<open&&css[next]!=',')++next;
        const char* selector=css+from;size_t size=next-from;trim(selector,size);bool valid=size&&size<64;
        for(size_t k=0;k<size;++k){char c=selector[k];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||(k==0&&(c=='.'||c=='#'||c=='*'))))valid=false;}
        if(valid&&count_<128){auto& r=rules_[count_++];std::memcpy(r.selector,selector,size);r.selector[size]=0;r.value=v;r.specificity=selector[0]=='#'?100:selector[0]=='.'?10:selector[0]=='*'?0:1;}
        from=next+1;
      }at=end+1;
    }
  }
  static bool token(const char* list,const char* word){size_t length=std::strlen(word);while(*list){while(space(*list))++list;const char* first=list;while(*list&&!space(*list))++list;if(size_t(list-first)==length&&!std::memcmp(first,word,length))return true;}return false;}
  Values resolve(tinyxml2::XMLElement* element)const{
    Values v;if(!element)return v;int mode=-1,combine=-1;
    for(size_t i=0;i<count_;++i){const auto& r=rules_[i];const char* s=r.selector;bool match=false;
      if(*s=='*')match=true;else if(*s=='.'){const char* c=element->Attribute("class");match=c&&token(c,s+1);}else if(*s=='#'){const char* id=element->Attribute("id");match=id&&!std::strcmp(id,s+1);}else {const char* name=std::strchr(element->Name(),':');match=!std::strcmp(name?name+1:element->Name(),s);}
      if(match){if(r.value.mode>=0&&r.specificity>=mode){v.mode=r.value.mode;mode=r.specificity;}if(r.value.combine>=0&&r.specificity>=combine){v.combine=r.value.combine;combine=r.specificity;}}
    }
    if(const char* style=element->Attribute("style")){const auto inlineValue=declarations(style,std::strlen(style));if(inlineValue.mode>=0)v.mode=inlineValue.mode;if(inlineValue.combine>=0)v.combine=inlineValue.combine;}
    return v;
  }
};
}} // namespace epub::vertical_style
