#pragma once
#include "experimental_typography.h"
#include "reader_settings.h"

namespace reader_typography {
struct Style {
  bool punctuation=true,spacing=true;
  uint8_t gap=4;
  uint8_t flags() const {return (punctuation?1u:0u)|(spacing?2u:0u);}
  static Style fromFlags(uint8_t value){Style result;result.punctuation=value&1u;result.spacing=value&2u;return result;}
};
// Production typography is fixed; persisted experimental flags are ignored.
extern const Style style; // Legacy default for standalone layout callers.
inline Style currentStyle(){Style s;const auto& c=reader_settings::current();s.punctuation=c.punctuation;s.spacing=c.spacing!=0;s.gap=c.spacing;return s;}
using Metrics=typography_experiment::Metrics;
inline typography_experiment::Options options(Style value){
  typography_experiment::Options result;
  result.compressAdjacentPunctuation=value.punctuation;
  result.hanLatinGap=value.spacing?value.gap:0;
  result.maxPunctuationTrim=14;result.minInkGap=2;
  return result;
}
inline int adjustment(uint32_t previous,uint32_t current,Style value,const Metrics& left,const Metrics& right){
  return typography_experiment::adjustment(previous,current,options(value),left,right);
}
template<class Measure>
int leading(const char* text,size_t length,size_t at,size_t lineBegin,Style value,Measure measure){
  return typography_experiment::leading(text,length,at,lineBegin,options(value),measure);
}
template<class Measure,class Draw,class Tick>
size_t page(const char* text,size_t length,size_t from,pagination::Geometry geometry,
            Style value,Measure measure,Draw draw,Tick tick){
  return typography_experiment::page(text,length,from,geometry,options(value),measure,draw,tick);
}
} // namespace reader_typography
