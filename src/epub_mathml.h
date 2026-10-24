#pragma once
// Presentation MathML -> bounded TeX for the existing native math compositor.
// This never writes canonical EPUB text. Unknown constructs reject the entire
// atom; no partly translated formula is published.
#include <tinyxml2.h>
#include <cstddef>
#include <cstring>
namespace epub_mathml {
class Converter {
  char* out_;size_t capacity_,used_=0;unsigned nodes_=0;bool ok_=true;
  static const char* name(const char* value){const auto* colon=std::strchr(value,':');return colon?colon+1:value;}
  void emit(const char* value){const size_t n=std::strlen(value);if(n>capacity_-used_){ok_=false;return;}std::memcpy(out_+used_,value,n);used_+=n;}
  void token(const char* value){if(!value)return;for(;*value&&ok_;++value){char c=*value;if(c=='#'||c=='$'||c=='%'||c=='&'||c=='_'||c=='{'||c=='}'||c=='\\'||c=='^'||c=='~'){ok_=false;return;}if(used_==capacity_){ok_=false;return;}out_[used_++]=c;}}
  bool children(tinyxml2::XMLElement* element,unsigned depth){for(auto n=element->FirstChildElement();n&&ok_;n=n->NextSiblingElement())node(n,depth+1);return ok_;}
  void group(tinyxml2::XMLElement* element,unsigned depth){emit("{");node(element,depth+1);emit("}");}
  void node(tinyxml2::XMLElement* e,unsigned depth){
    if(!e||!ok_)return;if(depth>16||++nodes_>256){ok_=false;return;}const char* tag=name(e->Name());
    tinyxml2::XMLElement* parts[3]={};unsigned count=0;for(auto child=e->FirstChildElement();child;child=child->NextSiblingElement()){if(count<3)parts[count]=child;++count;}
    if(!std::strcmp(tag,"math")||!std::strcmp(tag,"mrow")){children(e,depth);return;}
    if(!std::strcmp(tag,"semantics")){if(!parts[0])ok_=false;else node(parts[0],depth+1);return;}
    if(!std::strcmp(tag,"mi")||!std::strcmp(tag,"mn")||!std::strcmp(tag,"mo")||!std::strcmp(tag,"mtext")){
      if(count){ok_=false;return;}bool text=!std::strcmp(tag,"mtext"),normal=e->Attribute("mathvariant")&&!std::strcmp(e->Attribute("mathvariant"),"normal");
      if(text)emit("\\text{");else if(normal)emit("\\mathrm{");token(e->GetText());if(text||normal)emit("}");return;
    }
    if(!std::strcmp(tag,"mfrac")&&count==2){emit("\\frac");group(parts[0],depth);group(parts[1],depth);return;}
    if(!std::strcmp(tag,"msqrt")&&count){emit("\\sqrt{");children(e,depth);emit("}");return;}
    if(!std::strcmp(tag,"mroot")&&count==2){emit("\\sqrt[");node(parts[1],depth+1);emit("]");group(parts[0],depth);return;}
    bool sub=!std::strcmp(tag,"msub"),sup=!std::strcmp(tag,"msup"),both=!std::strcmp(tag,"msubsup");
    if((sub||sup||both)&&count==(both?3u:2u)){group(parts[0],depth);emit(sub||both?"_":"^");group(parts[1],depth);if(both){emit("^");group(parts[2],depth);}return;}
    if(!std::strcmp(tag,"mfenced")){
      const char* open=e->Attribute("open"),*close=e->Attribute("close"),*separators=e->Attribute("separators");if(!open)open="(";if(!close)close=")";
      auto delimiter=[&](const char* value){if(std::strlen(value)!=1||!std::strchr("()[]{}|",*value)){ok_=false;return;}if(*value=='{'||*value=='}')emit("\\");emit(value);};
      if(separators&&std::strcmp(separators,",")&&*separators){ok_=false;return;}emit("\\left");delimiter(open);bool first=true;
      for(auto child=e->FirstChildElement();child&&ok_;child=child->NextSiblingElement()){if(!first&&(!separators||*separators))emit(",");node(child,depth+1);first=false;}emit("\\right");delimiter(close);return;
    }
    ok_=false;
  }
 public:
  Converter(char* output,size_t capacity):out_(output),capacity_(capacity){}
  bool convert(tinyxml2::XMLElement* root){node(root,0);return ok_&&used_>0;}
  size_t size()const{return used_;}
};
}
