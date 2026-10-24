#pragma once
#include "pagination.h"

namespace line_quality {
// A local two-line search, never a paragraph-sized allocation or unbounded DP.
// Only a sparse/automatically hyphenated Latin line pays for the search. At
// most two preceding word boundaries and 768 extra width queries are examined.
constexpr unsigned MAX_MEASUREMENTS = 768;
constexpr size_t MAX_SCAN_BYTES = 512;
struct Budget { unsigned measured=0;bool exhausted=false; };
inline int cost(const pagination::Line& row,int available,int spaceWidth) {
  if(row.emergency)return 100000;
  if(row.paragraphEnd){
    // Discourage a single very short word stranded at the end of a paragraph.
    return row.spaces==0&&row.advance<available/4?220:0;
  }
  const int spare=std::max(0,available-row.advance);
  const int normal=std::max(1,row.spaces*spaceWidth);
  const int ratio=std::min(1000,int(int64_t(spare)*100/normal));
  return ratio*ratio/20+(row.hyphen?160:0)+(row.spaces?0:2000);
}
template<class WidthFactory,class Tick>
pagination::Line choose(const char* text,size_t length,size_t from,
    pagination::Geometry geometry,const pagination::Line& greedy,
    WidthFactory makeWidth,Tick tick,unsigned hyphenLines,bool nextIsLast,
    Budget* observed=nullptr) {
  const int available=geometry.right-geometry.left;
  if(greedy.paragraphEnd||!greedy.latinText||!greedy.compatible||greedy.formatted||
     greedy.emergency||greedy.hang||!greedy.spaces||greedy.next>=length||
     greedy.end-from>MAX_SCAN_BYTES||available<=0)return greedy;
  const auto firstWidth=makeWidth(from);
  const int spaceWidth=std::max(1,firstWidth(" ",1));
  // Well-spaced lines do not need a speculative pass.
  if(!greedy.hyphen&&available-greedy.advance<=greedy.spaces*spaceWidth/2)return greedy;
  Budget budget;
  auto nextLine=[&](const pagination::Line& first){
    const auto measured=makeWidth(first.next);
    auto bounded=[&](const char* p,size_t n){
      if(budget.measured==MAX_MEASUREMENTS){budget.exhausted=true;return available+1;}
      ++budget.measured;
      return measured(p,n);
    };
    const bool allow=!nextIsLast&&(!first.hyphen||hyphenLines<1);
    const size_t limit=std::min(length,first.next+MAX_SCAN_BYTES);
    auto result=pagination::line(text,limit,first.next,geometry,bounded,tick,allow);
    if(limit<length&&result.next==limit&&text[limit-1]!='\n')budget.exhausted=true;
    return result;
  };
  pagination::Line best=greedy;
  auto next=nextLine(greedy);
  int bestScore=cost(greedy,available,spaceWidth)+cost(next,available,spaceWidth);
  pagination::Line choices[2];unsigned count=0;
  int advance=0,spaces=0;size_t inkEnd=from;
  for(size_t at=from;at<greedy.end;){
    const size_t bytes=pagination::utf8Bytes(text+at,greedy.end-at);
    const uint32_t cp=pagination::codepoint(text+at,bytes);
    if(cp==' '&&inkEnd>from){
      pagination::Line choice=greedy;
      choice.end=inkEnd;choice.next=at+1;choice.advance=advance;
      choice.spaces=spaces;choice.hyphen=false;choice.hang=0;
      if(count<2)choices[count++]=choice;else{choices[0]=choices[1];choices[1]=choice;}
    }
    if(budget.measured==MAX_MEASUREMENTS){budget.exhausted=true;break;}
    ++budget.measured;
    advance+=pagination::advance(text+at,bytes,cp,firstWidth);
    if(cp==' ')++spaces;else if(!pagination::invisible(cp))inkEnd=at+bytes;
    at+=bytes;
  }
  for(unsigned i=count;i>0&&!budget.exhausted;--i){
    const auto& candidate=choices[i-1];
    if(candidate.end>=greedy.end||!candidate.spaces)continue;
    const auto following=nextLine(candidate);
    const int score=cost(candidate,available,spaceWidth)+cost(following,available,spaceWidth);
    // Hysteresis avoids spending a different line break on a negligible gain.
    if(!budget.exhausted&&score+40<bestScore){best=candidate;bestScore=score;}
  }
  if(observed)*observed=budget;
  return budget.exhausted?greedy:best;
}
} // namespace line_quality
