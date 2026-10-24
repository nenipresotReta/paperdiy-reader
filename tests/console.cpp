#include "console_policy.h"
#include "usb_storage_core.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
using paperdiy_console::Writer;
struct FIFO {
  uint32_t clock=0,step=1;
  size_t capacity=64,maximumWrite=64,drain=64;
  bool online=true,draining=false,zeroWrite=false;
  unsigned yields=0,attempts=0;
  std::vector<uint8_t> queued,received;
  void consume(){const size_t n=std::min(drain,queued.size());received.insert(received.end(),queued.begin(),queued.begin()+n);queued.erase(queued.begin(),queued.begin()+n);}
  paperdiy_console::IO io(){paperdiy_console::IO io;io.context=this;
    io.connected=[](void* p){return static_cast<FIFO*>(p)->online;};
    io.available=[](void* p){auto& f=*static_cast<FIFO*>(p);return f.capacity-f.queued.size();};
    io.write=[](void* p,const uint8_t* bytes,size_t count){auto& f=*static_cast<FIFO*>(p);++f.attempts;assert(count<=f.capacity-f.queued.size()&&count<=64);if(f.zeroWrite)return size_t(0);const size_t n=std::min(count,f.maximumWrite);f.queued.insert(f.queued.end(),bytes,bytes+n);return n;};
    io.flush=[](void*){};
    io.now=[](void* p){return static_cast<FIFO*>(p)->clock;};
    io.idle=[](void* p){auto& f=*static_cast<FIFO*>(p);++f.yields;f.clock+=f.step;if(f.draining)f.consume();};return io;
  }
};
void drainedScreenAndStatus(){
  FIFO fifo;fifo.draining=true;Writer writer(fifo.io());assert(writer.beginFrame());
  const char start[]="SCREEN 540 960 68\r\n",done[]="SCREEN_DONE\r\n";
  std::vector<uint8_t> expected(start,start+sizeof(start)-1);
  assert(writer.write(reinterpret_cast<const uint8_t*>(start),sizeof(start)-1)==sizeof(start)-1);
  for(unsigned row=0;row<960;++row){uint8_t bytes[68];for(unsigned i=0;i<68;++i)bytes[i]=uint8_t(row+i);expected.insert(expected.end(),bytes,bytes+68);assert(writer.write(bytes,68)==68);}
  expected.insert(expected.end(),done,done+sizeof(done)-1);
  assert(writer.write(reinterpret_cast<const uint8_t*>(done),sizeof(done)-1)==sizeof(done)-1);
  assert(writer.good());writer.endFrame();while(!fifo.queued.empty())fifo.consume();
  assert(fifo.received==expected&&writer.stats().dropped==0&&fifo.clock<paperdiy_console::FRAME_MS);
}
void stalledFrameAndResumedHost(){
  FIFO fifo;Writer writer(fifo.io());assert(writer.beginFrame());
  std::vector<uint8_t> bytes(4096,'x');assert(writer.write(bytes.data(),bytes.size())==64);
  assert(fifo.clock==paperdiy_console::IDLE_MS&&!writer.good());const auto time=fifo.clock;
  for(unsigned i=0;i<960;++i)assert(writer.write(bytes.data(),68)==0);
  assert(fifo.clock==time&&writer.stats().abortedFrames==1&&fifo.yields==paperdiy_console::IDLE_MS);
  writer.endFrame();fifo.draining=true;fifo.consume();assert(writer.beginFrame());
  const char status[]="USB_STATUS owned=1 state=1\r\n";
  assert(writer.write(reinterpret_cast<const uint8_t*>(status),sizeof(status)-1)==sizeof(status)-1);
  assert(writer.good());writer.endFrame();fifo.consume();
  assert(std::memcmp(fifo.received.data()+64,status,sizeof(status)-1)==0);
}
void totalAndIdleDeadlines(){
  const std::vector<uint8_t> bytes(4096,'x');
  FIFO trickle;trickle.draining=true;trickle.step=50;trickle.drain=1;Writer slow(trickle.io());
  assert(slow.write(bytes.data(),bytes.size())<bytes.size());assert(trickle.clock==paperdiy_console::WRITE_MS);
  FIFO frame;frame.draining=true;Writer bounded(frame.io());assert(bounded.beginFrame(20));
  for(unsigned i=0;i<100&&bounded.good();++i)bounded.write(bytes.data(),64);
  assert(!bounded.good()&&frame.clock==20);bounded.endFrame();
  FIFO wrapped;wrapped.capacity=0;wrapped.clock=UINT32_MAX-49;Writer wrap(wrapped.io());const auto started=wrapped.clock;
  assert(wrap.write(bytes.data(),1)==0&&uint32_t(wrapped.clock-started)==paperdiy_console::IDLE_MS);
  FIFO disconnected;disconnected.online=false;Writer absent(disconnected.io());assert(absent.write(bytes.data(),1)==0&&disconnected.yields==0);
  FIFO race;race.zeroWrite=true;Writer noProgress(race.io());assert(noProgress.write(bytes.data(),1)==0&&race.clock==paperdiy_console::IDLE_MS);
  FIFO partial;partial.draining=true;partial.maximumWrite=7;Writer shortWrites(partial.io());assert(shortWrites.write(bytes.data(),bytes.size())==bytes.size());
}
void computationGapDoesNotExpireNormalOutput(){
  FIFO fifo;fifo.draining=true;Writer writer(fifo.io());
  const char start[]="OPEN_BEGIN\r\n",done[]="OPEN_DONE\r\n",status[]="STATUS ready=1\r\n";
  assert(writer.write(reinterpret_cast<const uint8_t*>(start),sizeof(start)-1)==sizeof(start)-1);
  // Normal commands have no frame spanning their parsing/import computation.
  fifo.consume();fifo.clock+=paperdiy_console::FRAME_MS+5000;
  assert(writer.write(reinterpret_cast<const uint8_t*>(done),sizeof(done)-1)==sizeof(done)-1);
  fifo.consume();assert(writer.write(reinterpret_cast<const uint8_t*>(status),sizeof(status)-1)==sizeof(status)-1);
  fifo.consume();
  const std::string expected=std::string(start)+done+status;
  assert(std::string(fifo.received.begin(),fifo.received.end())==expected&&writer.stats().dropped==0);
  // A completed bulk frame must not leave a clock attached to later commands.
  assert(writer.beginFrame());assert(writer.write(reinterpret_cast<const uint8_t*>(done),sizeof(done)-1)==sizeof(done)-1);writer.endFrame();
  fifo.consume();fifo.clock+=paperdiy_console::FRAME_MS+5000;
  assert(writer.write(reinterpret_cast<const uint8_t*>(status),sizeof(status)-1)==sizeof(status)-1&&writer.stats().dropped==0);
}
void consoleFailureDoesNotReleaseStorage(){
  struct Card {uint8_t sector[512]{};} card;
  usb_storage::Backend backend;backend.context=&card;
  backend.read=[](void* p,uint32_t sector,uint8_t* out){assert(sector==0);std::memcpy(out,static_cast<Card*>(p)->sector,512);return true;};
  backend.write=[](void* p,uint32_t sector,const uint8_t* in){assert(sector==0);std::memcpy(static_cast<Card*>(p)->sector,in,512);return true;};backend.sync=[](void*){return true;};
  usb_storage::Core storage;assert(storage.begin(backend,1));FIFO fifo;fifo.capacity=0;Writer console(fifo.io());uint8_t bytes[512];std::memset(bytes,0xab,sizeof(bytes));
  assert(console.write(bytes,sizeof(bytes))==0&&storage.owns());
  assert(storage.write(0,0,bytes,sizeof(bytes))==512);std::memset(bytes,0,sizeof(bytes));assert(storage.read(0,0,bytes,sizeof(bytes))==512&&bytes[0]==0xab);
  assert(storage.owns()&&!storage.requestExit());assert(storage.startStop(0,false,true)&&!storage.owns());
}
int main(){drainedScreenAndStatus();stalledFrameAndResumedHost();totalAndIdleDeadlines();computationGapDoesNotExpireNormalOutput();consoleFailureDoesNotReleaseStorage();std::cout<<"Bounded FIFO deadlines, complete drained frames, stalled/reopened host, long computation gaps and storage independence passed\n";}
