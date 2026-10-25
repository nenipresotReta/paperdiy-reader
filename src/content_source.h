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
enum class PrefetchState : uint8_t {
  NotNeeded, Pending, Reading, VerifyingUtf8, VerifyingSemantics,
  Complete, Failed, Cancelled
};
enum class PrefetchFailure : uint8_t {None, Lease, Read, Hash, Utf8, Semantic};

struct Source {
  virtual ~Source(){}
  virtual bool pin(size_t begin,size_t end)=0;
  virtual size_t begin()const=0;
  virtual size_t end()const=0;
  virtual size_t length()const=0;
  // Restore a span previously published by this same source, without I/O.
  // Callers use this when semantic/page validation rejects a successful pin.
  virtual void restoreSpan(size_t begin,size_t end)=0;

  // Optional authenticated background fill. A source that does not implement
  // it reports NotNeeded/unsupported and the existing on-demand path is kept.
  virtual bool prefetchSupported()const{return false;}
  virtual void beginPrefetch(){}
  virtual void abortPrefetch(PrefetchFailure){}
  virtual void cancelPrefetch(){}
  virtual bool prefetchStep(){return false;}
  virtual bool prefetchPending()const{return false;}
  virtual bool prefetchFailed()const{return false;}
  virtual bool prefetchComplete()const{return false;}
  virtual PrefetchState prefetchState()const{return PrefetchState::NotNeeded;}
  virtual PrefetchFailure prefetchFailure()const{return PrefetchFailure::None;}
  virtual uint64_t prefetchToken()const{return 0;}
  virtual size_t prefetchLoadedBlocks()const{return 0;}
  virtual size_t prefetchTotalBlocks()const{return 0;}
  virtual uint64_t prefetchReadBytes()const{return 0;}
  // True when a pin failed because the storage lease changed underneath it.
  // The reader uses this to reopen at the saved anchor instead of silently
  // leaving a page turn stuck.
  virtual bool leaseInvalidated()const{return false;}
};
inline std::unique_ptr<Source>& source(){static std::unique_ptr<Source> value;return value;}
inline bool active(){return bool(source());}
inline size_t begin(){return active()?source()->begin():0;}
inline size_t limit(){return active()?source()->end():size_t(-1);}
inline bool pin(size_t first,size_t last){return !active()||source()->pin(first,last);}
inline void clear(){
  if(active())source()->cancelPrefetch();
  source().reset();
}
inline void install(std::unique_ptr<Source> value){
  if(active())source()->cancelPrefetch();
  source()=std::move(value);
}
inline bool prefetchSupported(){return active()&&source()->prefetchSupported();}
inline void beginPrefetch(){if(active())source()->beginPrefetch();}
inline void abortPrefetch(PrefetchFailure failure){if(active())source()->abortPrefetch(failure);}
inline void cancelPrefetch(){if(active())source()->cancelPrefetch();}
inline bool prefetchPending(){return active()&&source()->prefetchPending();}
inline bool prefetchFailed(){return active()&&source()->prefetchFailed();}
inline bool prefetchComplete(){return active()&&source()->prefetchComplete();}
inline PrefetchState prefetchState(){return active()?source()->prefetchState():PrefetchState::NotNeeded;}
inline PrefetchFailure prefetchFailure(){return active()?source()->prefetchFailure():PrefetchFailure::None;}
inline size_t prefetchLoadedBlocks(){return active()?source()->prefetchLoadedBlocks():0;}
inline size_t prefetchTotalBlocks(){return active()?source()->prefetchTotalBlocks():0;}
inline uint64_t prefetchReadBytes(){return active()?source()->prefetchReadBytes():0;}
inline bool leaseInvalidated(){return active()&&source()->leaseInvalidated();}
inline std::pair<size_t,size_t> pageRange(const std::vector<uint32_t>& pages,size_t page,size_t length){
  if(page>=pages.size()||pages[page]>=length)return {0,0};
  const size_t first=pages[page]>256?pages[page]-256:0;
  const size_t currentEnd=page+1<pages.size()?pages[page+1]:length;
  const size_t followingEnd=page+2<pages.size()?pages[page+2]:length;
  return {first,std::min(length,std::max(followingEnd,currentEnd+std::min(size_t(1024),length-currentEnd)))};
}
}
inline size_t readerContentLimit(){return reader_content::limit();}
