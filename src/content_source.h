#pragma once
#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>
#include <cstdint>
#include <utility>

// The reader keeps absolute canonical offsets. A source pins authenticated
// bytes into that address space; renderers may inspect only its published span.
namespace reader_content {
struct Source {
  virtual ~Source(){}
  virtual bool pin(size_t begin,size_t end)=0;
  virtual size_t begin()const=0;
  virtual size_t end()const=0;
  virtual size_t length()const=0;
  // Restore a span previously published by this same source, without I/O.
  // Callers use this when semantic/page validation rejects a successful pin.
  virtual void restoreSpan(size_t begin,size_t end)=0;
};
inline std::unique_ptr<Source>& source(){static std::unique_ptr<Source> value;return value;}
inline bool active(){return bool(source());}
inline size_t begin(){return active()?source()->begin():0;}
inline size_t limit(){return active()?source()->end():size_t(-1);}
inline bool pin(size_t first,size_t last){return !active()||source()->pin(first,last);}
inline void clear(){source().reset();}
inline void install(std::unique_ptr<Source> value){source()=std::move(value);}
inline std::pair<size_t,size_t> pageRange(const std::vector<uint32_t>& pages,size_t page,size_t length){
  if(page>=pages.size()||pages[page]>=length)return {0,0};
  const size_t first=pages[page]>256?pages[page]-256:0;
  const size_t currentEnd=page+1<pages.size()?pages[page+1]:length;
  const size_t followingEnd=page+2<pages.size()?pages[page+2]:length;
  return {first,std::min(length,std::max(followingEnd,currentEnd+std::min(size_t(1024),length-currentEnd)))};
}
}
inline size_t readerContentLimit(){return reader_content::limit();}
