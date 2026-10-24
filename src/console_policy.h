#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

// Application-task logging only. Each backend callback must make one finite,
// nonblocking attempt. No allocation, filesystem, ownership or reboot APIs.
namespace paperdiy_console {
constexpr uint32_t IDLE_MS=100,WRITE_MS=2000,FRAME_MS=10000;
struct IO {
  void* context=nullptr;
  bool (*connected)(void*)=nullptr;
  size_t (*available)(void*)=nullptr;
  size_t (*write)(void*,const uint8_t*,size_t)=nullptr;
  void (*flush)(void*)=nullptr;
  uint32_t (*now)(void*)=nullptr;
  void (*idle)(void*)=nullptr;
};
struct Stats {uint64_t accepted=0,dropped=0;uint32_t abortedFrames=0;};
class Writer {
 public:
  explicit Writer(IO io):io_(io){}
  bool beginFrame(uint32_t limit=FRAME_MS){
    if(framed_)return false;
    framed_=true;failed_=false;frameStarted_=now();frameLimit_=limit;return true;
  }
  void endFrame(){framed_=false;failed_=false;}
  bool good()const{return !failed_;}
  Stats stats()const{return stats_;}
  size_t write(const uint8_t* bytes,size_t count){
    if(!count)return 0;
    if(!bytes||!io_.connected||!io_.available||!io_.write||!io_.now||!io_.idle||(framed_&&failed_))return dropped(count);
    const uint32_t started=now();uint32_t progress=started;size_t accepted=0;
    while(accepted<count){
      const auto time=now();
      if(!io_.connected(io_.context)||uint32_t(time-started)>=WRITE_MS||uint32_t(time-progress)>=IDLE_MS||
         (framed_&&uint32_t(time-frameStarted_)>=frameLimit_))break;
      // Never ask a FIFO backend to queue more than its current free snapshot.
      // A short or zero write is retried under the same deadline.
      const size_t request=std::min(std::min(count-accepted,io_.available(io_.context)),size_t(64));
      if(request){
        const size_t n=io_.write(io_.context,bytes+accepted,request);
        if(n>request)break;
        if(n){accepted+=n;progress=now();flush();}
      }
      if(accepted<count)io_.idle(io_.context);
    }
    stats_.accepted+=accepted;
    if(accepted<count)dropped(count-accepted);
    return accepted;
  }
  void flush(){if(io_.flush)io_.flush(io_.context);}
 private:
  uint32_t now()const{return io_.now?io_.now(io_.context):0;}
  size_t dropped(size_t count){
    stats_.dropped+=count;
    if(framed_&&!failed_){failed_=true;++stats_.abortedFrames;}
    return 0;
  }
  IO io_;Stats stats_;
  uint32_t frameStarted_=0,frameLimit_=FRAME_MS;
  bool framed_=false,failed_=false;
};
}
