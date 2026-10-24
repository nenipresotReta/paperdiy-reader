#include "english_hyphenation.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
int main(){
  using text_language::Language;
  for(const char* tag:{"en","en-US","EN-us"," en-Latn-US\t","en-CA","en-x-gb"})
    assert(text_language::fromTag(tag)==Language::EnglishUS);
  for(const char* tag:{"en-GB"," EN-gb ","en-Latn-GB","en-GB-x-reader"})
    assert(text_language::fromTag(tag)==Language::EnglishGB);
  for(const char* tag:{"","de","fr","zh-Hans","english","en-","en--GB","en_US","en<script>","en gb","en-x","en-x-"})
    assert(text_language::fromTag(tag)==Language::None);
  assert(text_language::fromTag(nullptr)==Language::None);
  assert(english_hyphenation::points("project",7,Language::EnglishGB)==(1u<<3));
  assert(english_hyphenation::points("project",7,Language::EnglishUS)==0);
  assert(english_hyphenation::points("hyphenation",11,Language::EnglishGB)==((1u<<2)|(1u<<6)|(1u<<7)));
  for(auto language:{Language::None,Language::EnglishUS,Language::EnglishGB}){
    assert(!english_hyphenation::points("NASA",4,language));
    assert(!english_hyphenation::points("hyper-link",10,language));
  }
  assert(!english_hyphenation::points("hyphenation",11,Language::None));
  puts("English language tags, British/US pattern separation, unsupported-language safety: PASS");
}
