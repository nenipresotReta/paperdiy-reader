#pragma once
#include "Arduino.h"
#include <deque>
#include <memory>
enum {WIFI_OFF,WIFI_STA,WIFI_AP};
enum {WL_IDLE_STATUS=0,WL_CONNECTED=3,WL_CONNECT_FAILED=4,WL_DISCONNECTED=6};
struct IPAddress {
 String text="192.168.4.1";
 IPAddress()=default;
 IPAddress(unsigned a,unsigned b,unsigned c,unsigned d):text(String(a)+"."+String(b)+"."+String(c)+"."+String(d)){}
 String toString()const{return text;}
};
struct MockSocket {
 std::string input,output;
 size_t offset=0,reads=0;
 bool open=true;
 uint32_t timeout=0;
 void appendInput(const std::string& bytes){input+=bytes;}
};
class WiFiClient {
 public:
  WiFiClient()=default;
  explicit WiFiClient(const std::shared_ptr<MockSocket>& socket):socket_(socket){}
  explicit operator bool()const{return bool(socket_);}
  bool connected()const{return socket_&&socket_->open;}
  int available()const{return socket_?int(socket_->input.size()-socket_->offset):0;}
  int read(){
    if(!available())return -1;
    ++socket_->reads;return static_cast<unsigned char>(socket_->input[socket_->offset++]);
  }
  void setTimeout(uint32_t timeout){if(socket_)socket_->timeout=timeout;}
  void stop(){if(socket_)socket_->open=false;}
  int fd()const{return -1;}
  size_t write(const uint8_t* data,size_t size){
    if(!connected())return 0;socket_->output.append(reinterpret_cast<const char*>(data),size);return size;
  }
  size_t write(const char* data,size_t size){return write(reinterpret_cast<const uint8_t*>(data),size);}
 private:
  std::shared_ptr<MockSocket> socket_;
};
class WiFiServer {
 public:
  std::deque<WiFiClient> pending;
  WiFiClient available(){
    if(pending.empty())return {};
    auto client=pending.front();pending.pop_front();return client;
  }
};
struct MockWiFi {
 int currentMode=WIFI_OFF,connection=WL_DISCONNECTED;
 uint8_t stations=0,maxStations=0;
 bool failAp=false,failOff=false,persistentEnabled=true,sleepEnabled=false;
 unsigned beginCalls=0;
 String joinedSsid,joinedPass;
 IPAddress staIp{192,168,1,25};
 void persistent(bool value){persistentEnabled=value;}
 bool mode(int value){if(value==WIFI_OFF&&failOff)return false;currentMode=value;return true;}
 int getMode()const{return currentMode;}
 bool softAP(const char*,const char*,int=1,int=0,int max=4){maxStations=max;return !failAp;}
 IPAddress softAPIP()const{return {};}
 uint8_t softAPgetStationNum()const{return stations;}
 bool softAPdisconnect(bool){stations=0;return true;}
 bool disconnect(bool=false,bool=false){connection=WL_DISCONNECTED;return true;}
 void begin(const char* ssid,const char* pass){++beginCalls;joinedSsid=ssid;joinedPass=pass;}
 int status()const{return connection;}
 IPAddress localIP()const{return staIp;}
 bool setSleep(bool value){sleepEnabled=value;return true;}
};
inline MockWiFi WiFi;
