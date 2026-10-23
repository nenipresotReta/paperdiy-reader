#include <cassert>
#include <cstring>
#include <iostream>
#include <set>
#include <vector>
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SD.h>
#include "network.h"
#include "document_format.h"
#include "reader.h"
#include "sd_reader.h"

Preferences prefs;
bool settingsSelected=false,settingsRotated=false;
NetworkMode networkMode=NetworkMode::Off;
String settingsApSsid,settingsApPassword,settingsApUrl;
bool settingsApClientConnected=false;
String settingsStaSsid,settingsStaIp,settingsStaError;
bool settingsStaConfigured=false,settingsStaConnected=false;
bool sdOK=true;
std::vector<String> sdBooks;
size_t selectedBook=0;
String sdError;
unsigned scans=0;
bool scanSD(){++scans;return sdOK;}

namespace {
constexpr uint32_t TIMEOUT=5u*60u*1000u;
std::string bytes(const String& value){return value.c_str();}
String token(){
  const std::string url=bytes(settingsApUrl);
  auto at=url.find("?t=");assert(at!=std::string::npos);
  return url.substr(at+3);
}
String cookie(){return "diy_token="+token();}
WebServer& server(){assert(WebServer::current);return *WebServer::current;}
void reset(uint32_t now=100){
  networkStop();prefs={};WiFi={};SD={};testClock=now;testCardDetect=LOW;
  settingsStaSsid="";settingsStaConfigured=false;sdOK=true;sdBooks.clear();scans=0;
  networkApply(NetworkMode::Ap);
  assert(WebServer::current&&server().running);
}
MockResponse request(const char* path,const String& auth,HTTPMethod method=HTTP_GET,
                     const std::map<std::string,String>& args={}){
  server().prepare(auth);server().args=args;server().invoke(path,method);return server().response;
}
uint32_t generation(){
  const auto result=request("/api/status",cookie());assert(result.code==200);
  std::string body=bytes(result.body),key="\"uploadGeneration\":";
  auto start=body.find(key);assert(start!=std::string::npos);
  return static_cast<uint32_t>(std::stoul(body.substr(start+key.size())));
}
struct UploadAttempt {
  String auth,id;
  uint32_t generation=0;
  size_t offset=0;
  bool failed=false,includeGeneration=true;
};
UploadAttempt attempt;
void prepareUpload(const String& auth="",bool validGeneration=true){
  const auto current=generation();
  attempt={auth.length()?auth:cookie(),"",current,0,false,validGeneration};
}
void startUpload(const String& name,size_t size){
  std::map<std::string,String> args={{"name",name},{"size",String(size)}};
  if(attempt.includeGeneration)args["generation"]=String(attempt.generation);
  const auto result=request("/api/upload/start",attempt.auth,HTTP_POST,args);
  if(result.code!=200){attempt.failed=true;return;}
  std::string body=bytes(result.body),key="\"id\":\"";
  auto begin=body.find(key);assert(begin!=std::string::npos);begin+=key.size();
  auto end=body.find('"',begin);assert(end!=std::string::npos);
  attempt.id=body.substr(begin,end-begin);assert(attempt.id.length());
}
String base64(size_t count){
  static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String encoded;
  for(size_t i=0;i<count;i+=3){
    const size_t n=std::min(size_t(3),count-i);
    const uint32_t v=(uint32_t('x')<<16)|(n>1?uint32_t('x')<<8:0)|(n>2?'x':0);
    encoded+=alphabet[(v>>18)&63];encoded+=alphabet[(v>>12)&63];
    encoded+=n>1?alphabet[(v>>6)&63]:'=';encoded+=n>2?alphabet[v&63]:'=';
  }
  return encoded;
}
void uploadBytes(size_t size){
  while(size&&!attempt.failed){
    const size_t chunk=std::min(size,size_t(4096));
    const auto result=request("/api/upload/chunk",attempt.auth,HTTP_POST,
      {{"id",attempt.id},{"offset",String(attempt.offset)},{"data",base64(chunk)}});
    if(result.code!=200){attempt.failed=true;return;}
    attempt.offset+=chunk;size-=chunk;
  }
}
void completeUpload(){
  if(!attempt.failed)request("/api/upload/commit",attempt.auth,HTTP_POST,{{"id",attempt.id}});
}
void cancelUpload(){request("/api/upload/cancel",attempt.auth,HTTP_POST,{{"id",attempt.id}});}
void upload(const String& name,size_t size=12){
  prepareUpload();startUpload(name,size);uploadBytes(size);completeUpload();
}
unsigned published(){
  unsigned count=0;for(const auto& [path,node]:SD.files)
    if(!node->directory&&path.find("/Books/")==0&&path.find("/.incoming/")==std::string::npos)++count;
  return count;
}
void assertNoTemporary(){
  for(const auto& [path,node]:SD.files){
    assert(node->openHandles==0);
    assert(node->directory||path.find("/.incoming/")==std::string::npos);
  }
}
void assertRejected(){
  assert(server().response.code>=400);assert(server().response.type=="application/json");
  assert(published()==0);assert(scans==0);assertNoTemporary();
}
void authenticationAndRandomness(){
  reset();assert(WiFi.currentMode==WIFI_AP&&!WiFi.persistentEnabled&&WiFi.maxStations==1);
  assert(server().collected.count("Cookie"));
  assert(settingsApSsid.startsWith("PaperDIY-"));assert(settingsApPassword.length()>=12);
  std::set<char> symbols;for(char c:bytes(settingsApPassword))symbols.insert(c);
  assert(symbols.size()>3);assert(token().length()>=32);
  auto savedToken=token();auto savedSsid=settingsApSsid;auto savedPass=settingsApPassword;
  assert(request("/","").code==403);
  auto root=request("/","",HTTP_GET,{{"t",token()}});
  assert(root.code==200||root.code==302||root.code==303);
  assert(root.headers.at("Set-Cookie").indexOf("HttpOnly")>=0);
  assert(root.headers.at("Set-Cookie").indexOf("SameSite=Strict")>=0);
  assert(root.headers.at("Referrer-Policy")=="no-referrer");
  assert(request("/api/status","",HTTP_GET,{{"t",token()}}).code==401);
  assert(request("/api/status","other_"+cookie()).code==401);
  assert(request("/api/status",cookie()+"extra").code==401);
  assert(request("/api/status","first=x; "+cookie()+"; last=y").code==200);
  assert(request("/api/files",cookie()).code==200);
  networkApply(NetworkMode::Ap);
  assert(token()!=savedToken&&settingsApSsid!=savedSsid&&settingsApPassword!=savedPass);
  assert(request("/api/status","diy_token="+savedToken).code==401);
  std::cout<<"network: authentication and fresh credentials\n";
}
void qrAndTimeout(){
  reset();assert(networkActive());assert(!settingsApClientConnected);assert(!networkPoll());
  WiFi.stations=1;assert(networkPoll());assert(settingsApClientConnected);assert(!networkPoll());
  WiFi.stations=0;assert(networkPoll());assert(!settingsApClientConnected);
  // Background status/file polling must not keep an unattended AP alive.
  testClock+=TIMEOUT-1;assert(request("/api/status",cookie()).code==200);
  assert(request("/api/files",cookie()).code==200);networkPoll();assert(!networkApExpired());
  ++testClock;assert(networkApExpired());
  reset(UINT32_MAX-100);testClock+=TIMEOUT-1;assert(!networkApExpired());
  ++testClock;assert(networkApExpired());
  reset();testClock+=TIMEOUT-100;
  auto root=request("/", "",HTTP_GET,{{"t",token()}});
  assert(root.code==200||root.code==302||root.code==303);
  testClock+=200;assert(!networkApExpired());
  // A genuinely active transfer can span the AP's inactivity interval.
  prepareUpload();startUpload("active.epub",100);
  for(unsigned i=0;i<10;++i){testClock+=40000;uploadBytes(10);assert(!networkApExpired());networkPoll();}
  completeUpload();assert(server().response.code==200);
  testClock+=TIMEOUT-1;assert(!networkApExpired());++testClock;assert(networkApExpired());
  // A phone disappearing halfway through an upload releases the global slot.
  reset(UINT32_MAX-100);prepareUpload();startUpload("lease.epub",100);uploadBytes(10);
  testClock+=45001;networkPoll();assertNoTemporary();assert(published()==0);
  upload("after-timeout.epub");assert(server().response.code==200);
  networkStop();assert(!networkActive());assert(!WebServer::current);assert(WiFi.currentMode==WIFI_OFF);
  assert(!settingsApClientConnected&&settingsApSsid.length()==0&&settingsApPassword.length()==0&&settingsApUrl.length()==0);
  std::cout<<"network: single QR transitions, activity and clock rollover\n";
}
void uploadsAndFilenames(){
  reset();assert(server().routes.count("/api/upload")==0);
  for(const auto& [path,route]:server().routes)assert(!route.upload);
  upload("中文 书名.epub");assert(server().response.code==200);
  assert(SD.exists("/Books/中文 书名.epub"));assert(scans==0&&sdBooks.size()==1);assertNoTemporary();
  const auto first=SD.files.at("/Books/中文 书名.epub");
  upload("中文 书名.epub",13);assert(server().response.code==200);
  assert(first->size==12&&SD.files.at("/Books/中文 书名-1.epub")->size==13);assert(scans==0&&sdBooks.size()==2);
  upload("BOOK.TXT");assert(server().response.code==200);assertNoTemporary();
  upload("中文文档.md");assert(server().response.code==200&&SD.exists("/Books/中文文档.md"));
  upload("文档.MARKDOWN");assert(server().response.code==200&&SD.exists("/Books/文档.MARKDOWN"));assertNoTemporary();
  const std::vector<String> invalid={"../escape.epub","a/b.epub","a\\b.epub",".hidden.txt","a.txt ",
    "a.epub.","a\nb.txt","a\"b.epub","a|b.epub","a.jpg",String("bad\xc0\xaf.txt"),
    String("bad\xed\xa0\x80.txt"),String("bad\xf4\x90\x80\x80.txt"),String("bad\xe4\xb8.txt"),
    String(std::string(97,'a')+".txt")};
  for(const auto& name:invalid){reset();upload(name);assertRejected();}
  for(const char* empty:{"empty.txt","empty.md","empty.MARKDOWN"}){reset();upload(empty,0);assertRejected();}
  reset();prepareUpload("invalid=x");startUpload("private.epub",10);uploadBytes(10);completeUpload();
  assert(server().response.code==401);assertRejected();
  reset();prepareUpload("",false);startUpload("missing-generation.epub",10);uploadBytes(10);completeUpload();assertRejected();
  reset();const auto old=generation();upload("first.epub");assert(server().response.code==200);
  prepareUpload();attempt.generation=old;startUpload("queued.epub",10);uploadBytes(10);completeUpload();
  assert(server().response.code==409&&published()==1);assertNoTemporary();
  // Another tab cannot replace/cancel the active upload or leave a queued file.
  reset();prepareUpload();startUpload("first.epub",10);const auto owner=attempt;
  auto status=request("/api/status",cookie());assert(status.body.indexOf("\"busy\":true")>=0);
  prepareUpload();startUpload("second.epub",10);assert(server().response.code==409);
  assert(published()==0);attempt=owner;uploadBytes(10);completeUpload();assert(server().response.code==200);
  assert(published()==1&&!SD.exists("/Books/second.epub"));assertNoTemporary();
  std::cout<<"network: UTF-8 filenames, nonoverwrite and stale upload rejection\n";
}
void uploadFailures(){
  for(unsigned fault=0;fault<8;++fault){
    reset();
    switch(fault){
      case 0:sdOK=false;break;case 1:SD.mounted=false;break;case 2:testCardDetect=HIGH;break;
      case 3:SD.failOpen=true;break;case 4:SD.failWrite=true;break;
      case 5:SD.failRename=true;break;case 6:SD.capacity=SD.used;break;
      case 7:SD.remove("/Books/.incoming");SD.remove("/Books");SD.failMkdir=true;break;
    }
    upload("failure.epub");assertRejected();
  }
  reset();prepareUpload();startUpload("removed.epub",10);uploadBytes(5);
  sdOK=false;testCardDetect=HIGH;uploadBytes(5);completeUpload();
  assert(server().response.code==503&&published()==0);
  sdOK=true;testCardDetect=LOW;networkStorageChanged();assertNoTemporary();
  reset();prepareUpload();startUpload("abort.epub",20);uploadBytes(10);cancelUpload();
  assert(server().response.code==200&&published()==0);assertNoTemporary();
  reset();prepareUpload();startUpload("stop.epub",20);uploadBytes(10);
  networkStop();assertNoTemporary();assert(published()==0);
  reset();prepareUpload();startUpload("short.epub",20);uploadBytes(10);completeUpload();assertRejected();
  // Chunks must have exact offsets and strict base64, with bounded decoded size.
  for(const auto& bad:std::vector<String>{"", "!!!!", "eA=", "eA===", "eA==junk", "eB==", "eHh=", base64(4097)}){
    reset();prepareUpload();startUpload("bad-chunk.epub",8192);
    auto result=request("/api/upload/chunk",cookie(),HTTP_POST,{{"id",attempt.id},{"offset","0"},{"data",bad}});
    assert(result.code>=400);cancelUpload();assertNoTemporary();assert(published()==0);
  }
  reset();prepareUpload();startUpload("ordered.epub",20);uploadBytes(10);
  auto result=request("/api/upload/chunk",cookie(),HTTP_POST,{{"id",attempt.id},{"offset","0"},{"data",base64(10)}});
  assert(result.code==409);cancelUpload();assertNoTemporary();assert(published()==0);
  reset();prepareUpload();startUpload("overflow.epub",10);uploadBytes(11);completeUpload();assertRejected();
  for(const auto& limit:std::vector<std::pair<String,size_t>>{{"limit.txt",document_format::sourceLimit(document_format::Kind::Text)},
       {"limit.epub",document_format::sourceLimit(document_format::Kind::Epub)},
       {"limit.md",document_format::sourceLimit(document_format::Kind::Markdown)},
       {"limit.MARKDOWN",document_format::sourceLimit(document_format::Kind::Markdown)}}){
    reset();upload(limit.first,limit.second);assert(server().response.code==200);assertNoTemporary();
    reset();upload(limit.first,limit.second+1);assert(server().response.code==413);assertRejected();
  }
  std::cout<<"network: chunk transaction failures, cancellation and size boundaries\n";
}
void ownershipAndCatalog(){
  reset();sdBooks={"/Books/Middle.epub","/Books/Zebra.epub"};selectedBook=1;
  SD.add("/Books/Middle.epub","keep middle");SD.add("/Books/Zebra.epub","keep zebra");
  upload("Apple.epub");assert(server().response.code==200&&scans==0);
  assert(sdBooks.size()==3&&sdBooks[0]=="/Books/Apple.epub"&&sdBooks[selectedBook]=="/Books/Zebra.epub");
  reset();prepareUpload();startUpload("race.epub",10);uploadBytes(10);
  SD.add("/Books/race.epub","other writer");completeUpload();assert(server().response.code==200);
  assert(SD.files.at("/Books/race.epub")->size==12&&SD.exists("/Books/race-1.epub"));assertNoTemporary();
  reset();SD.add("/Books/full.epub","keep");
  for(unsigned i=1;i<100;++i)SD.add("/Books/full-"+std::to_string(i)+".epub","keep");
  upload("full.epub");assert(server().response.code==409&&published()==100);assertNoTemporary();
  reset();String maxName;for(unsigned i=0;i<30;++i)maxName+="文";maxName+="a.epub";
  assert(maxName.length()==96);upload(maxName);assert(server().response.code==200);assertNoTemporary();
  reset();upload(String(std::string(92,'a')+".epub"));assertRejected();
  reset();prepareUpload();startUpload("owner.epub",20);uploadBytes(10);
  for(const char* route:{"/api/upload/chunk","/api/upload/commit","/api/upload/cancel"}){
    auto r=request(route,cookie(),HTTP_POST,{{"id","not-the-owner"},{"offset","10"},{"data",base64(10)}});
    assert(r.code==409&&published()==0);
  }
  uploadBytes(10);completeUpload();assert(server().response.code==200&&published()==1);
  auto oldId=attempt.id;
  prepareUpload();startUpload("next.epub",10);
  auto r=request("/api/upload/cancel",cookie(),HTTP_POST,{{"id",oldId}});assert(r.code==409);
  uploadBytes(10);completeUpload();assert(server().response.code==200&&published()==2);assertNoTemporary();
  reset();prepareUpload();startUpload("truncated.epub",10);uploadBytes(10);
  for(auto& [path,node]:SD.files)if(!node->directory&&path.find("/.incoming/")!=std::string::npos)--node->size;
  completeUpload();assertRejected();
  reset();prepareUpload();startUpload("unmounted.epub",10);uploadBytes(5);
  networkStorageChanged();assertNoTemporary();completeUpload();assertRejected();
  reset();for(unsigned i=0;i<256;++i)sdBooks.emplace_back("/Books/book-"+String(i)+".epub");
  upload("over-limit.epub");assertRejected();
  auto first=request("/api/files",cookie());
  assert(first.body.indexOf("\"next\":24")>=0&&first.body.indexOf("book-23.epub")>=0&&first.body.indexOf("book-24.epub")<0);
  auto last=request("/api/files",cookie(),HTTP_GET,{{"offset","240"}});
  assert(last.body.indexOf("book-255.epub")>=0&&last.body.indexOf("\"next\":null")>=0);
  auto end=request("/api/files",cookie(),HTTP_GET,{{"offset","4294967295"}});
  assert(end.code==200&&end.body.indexOf("\"files\":[]")>=0);
  assert(request("/api/files",cookie(),HTTP_GET,{{"offset","-1"}}).code==400);
  std::cout<<"network: transaction ownership, catalog selection and pagination\n";
}
void credentialsAndStation(){
  reset();prefs.strings["wifiSsid"]="legacy network";prefs.strings["wifiPass"]="legacy-pass";
  const auto originals=prefs.strings;
  auto result=request("/api/wifi",cookie(),HTTP_POST,{{"ssid","new network"},{"pass","new-password"}});
  assert(result.code==200);assert(prefs.writes==1);assert(prefs.strings==originals);assert(prefs.blobs.size()==1);
  const auto committed=prefs.blobs;
  prefs.failWrite=true;
  result=request("/api/wifi",cookie(),HTTP_POST,{{"ssid","failed network"},{"pass","failed-password"}});
  assert(result.code>=500);assert(prefs.blobs==committed);assert(settingsStaSsid=="new network");prefs.failWrite=false;
  auto before=prefs.writes;
  result=request("/api/wifi",cookie(),HTTP_POST,{{"ssid",""},{"pass","x"}});assert(result.code==400&&prefs.writes==before);
  result=request("/api/wifi",cookie(),HTTP_POST,{{"ssid",String(std::string(33,'a'))},{"pass","password"}});
  assert(result.code==400&&prefs.writes==before);
  result=request("/api/wifi",cookie(),HTTP_POST,{{"ssid","valid"},{"pass",String(std::string(64,'a'))}});
  assert(result.code==400&&prefs.writes==before);
  result=request("/api/wifi",cookie(),HTTP_POST,{{"ssid","bad\nnetwork"},{"pass","password"}});
  assert(result.code==400&&prefs.writes==before);
  result=request("/api/wifi","",HTTP_POST,{{"ssid","forged"},{"pass","password"}});
  assert(result.code==401&&prefs.writes==before);
  result=request("/api/wifi",cookie(),HTTP_GET,{{"ssid","wrong method"},{"pass","password"}});
  assert(result.code==404&&prefs.writes==before);
  networkApply(NetworkMode::Sta);assert(networkActive());assert(!WebServer::current);
  assert(WiFi.currentMode==WIFI_STA&&WiFi.joinedSsid=="new network"&&WiFi.joinedPass=="new-password");
  assert(!networkPoll());WiFi.connection=WL_CONNECTED;assert(networkPoll());assert(settingsStaConnected);
  assert(!networkPoll());WiFi.connection=WL_DISCONNECTED;assert(networkPoll());assert(!settingsStaConnected);
  testClock+=20001;assert(networkPoll());assert(settingsStaError.length());
  networkStop();prefs.blobs.clear();networkApply(NetworkMode::Sta);
  assert(!WebServer::current&&WiFi.joinedSsid=="legacy network"&&WiFi.joinedPass=="legacy-pass");
  networkStop();prefs.blobs["wifiCfgV1"]={1,2,3};networkApply(NetworkMode::Sta);
  assert(WiFi.joinedSsid=="legacy network");
  networkStop();prefs.blobs=committed;prefs.shortRead=true;networkApply(NetworkMode::Sta);
  assert(WiFi.joinedSsid=="legacy network");
  networkApply(NetworkMode::Usb);assert(!networkActive()&&!WebServer::current&&WiFi.currentMode==WIFI_OFF);
  networkStop();prefs={};networkApply(NetworkMode::Sta);
  assert(!settingsStaConfigured&&!networkActive()&&!WebServer::current&&WiFi.currentMode==WIFI_OFF);
  std::cout<<"network: atomic credentials, legacy fallback and STA without HTTP\n";
}
void uploadRecovery(){
  reset();
  const String id="0123456789abcdef0123456789abcdef";
  const auto initial=generation();
  const std::map<std::string,String> metadata={{"name","recovered.epub"},{"size","10"},{"generation",String(initial)},{"requestId",id}};
  auto start=request("/api/upload/start",cookie(),HTTP_POST,metadata);
  assert(start.code==200&&start.body.indexOf(id)>=0&&generation()==initial+1);
  // Losing the start response must not acquire a second transaction or epoch.
  start=request("/api/upload/start",cookie(),HTTP_POST,metadata);
  assert(start.code==200&&generation()==initial+1&&published()==0);
  auto state=request("/api/upload/status",cookie(),HTTP_GET,{{"id",id}});
  assert(state.code==200&&state.body.indexOf("\"receiving\"")>=0&&state.body.indexOf("\"offset\":0")>=0);
  auto changed=metadata;changed["size"]="11";
  assert(request("/api/upload/start",cookie(),HTTP_POST,changed).code==409);
  auto data=request("/api/upload/chunk",cookie(),HTTP_POST,{{"id",id},{"offset","0"},{"data",base64(10)}});
  assert(data.code==200);
  // A lost chunk response is recovered by querying the accepted offset.
  state=request("/api/upload/status",cookie(),HTTP_GET,{{"id",id}});
  assert(state.body.indexOf("\"offset\":10")>=0);
  assert(request("/api/upload/chunk",cookie(),HTTP_POST,{{"id",id},{"offset","0"},{"data",base64(10)}}).code==409);
  const auto completed=request("/api/upload/commit",cookie(),HTTP_POST,{{"id",id}});
  assert(completed.code==200&&published()==1);
  const auto renamed=SD.renames;
  auto replay=request("/api/upload/commit",cookie(),HTTP_POST,{{"id",id}});
  assert(replay.code==200&&replay.body==completed.body&&SD.renames==renamed&&published()==1);
  state=request("/api/upload/status",cookie(),HTTP_GET,{{"id",id}});
  assert(state.code==200&&state.body.indexOf("\"complete\"")>=0&&state.body.indexOf("recovered.epub")>=0);
  start=request("/api/upload/start",cookie(),HTTP_POST,metadata);
  assert(start.code==200&&start.body.indexOf("\"offset\":10")>=0&&generation()==initial+1&&published()==1);
  changed=metadata;changed["name"]="other.epub";
  assert(request("/api/upload/start",cookie(),HTTP_POST,changed).code==409);
  changed=metadata;changed["generation"]=String(generation());
  assert(request("/api/upload/start",cookie(),HTTP_POST,changed).code==409);
  assert(request("/api/upload/status","",HTTP_GET,{{"id",id}}).code==401);
  assert(request("/api/upload/commit","",HTTP_POST,{{"id",id}}).code==401);
  prepareUpload();startUpload("active.epub",3);
  replay=request("/api/upload/commit",cookie(),HTTP_POST,{{"id",id}});
  assert(replay.code==200&&published()==1);
  assert(request("/api/upload/status",cookie(),HTTP_GET,{{"id",attempt.id}}).body.indexOf("receiving")>=0);
  cancelUpload();assertNoTemporary();
  // Only four receipts are retained, independently of all existing books.
  for(unsigned i=0;i<4;++i)upload("receipt-"+String(i)+".epub",1);
  assert(published()==5&&request("/api/upload/commit",cookie(),HTTP_POST,{{"id",id}}).code==409);
  assert(request("/api/upload/start",cookie(),HTTP_POST,metadata).code==409);
  const String latest=attempt.id;
  networkStorageChanged();
  assert(request("/api/upload/status",cookie(),HTTP_GET,{{"id",latest}}).code==409&&published()==5);
  upload("fresh-session.epub",1);const String oldSession=attempt.id;
  networkApply(NetworkMode::Ap);
  assert(request("/api/upload/status",cookie(),HTTP_GET,{{"id",oldSession}}).code==409);
  std::cout<<"network: idempotent admission/commit, lost-response recovery and bounded receipts\n";
}
void stationReconnectWindow(){
  for(uint32_t start:{uint32_t(100),uint32_t(UINT32_MAX-60000)}){
    reset(start);prefs.strings["wifiSsid"]="test network";prefs.strings["wifiPass"]="test-password";
    networkApply(NetworkMode::Sta);WiFi.connection=WL_CONNECTED;assert(networkPoll());
    testClock+=120000;WiFi.connection=WL_DISCONNECTED;assert(networkPoll());
    assert(!settingsStaConnected&&settingsStaError.length()==0);
    testClock+=19999;assert(!networkPoll()&&settingsStaError.length()==0);
    ++testClock;assert(networkPoll()&&settingsStaError.length()>0);
    WiFi.connection=WL_CONNECTED;assert(networkPoll()&&settingsStaError.length()==0);
    testClock+=120000;WiFi.connection=WL_DISCONNECTED;assert(networkPoll());
    assert(settingsStaError.length()==0);
  }
  std::cout<<"network: fresh reconnect grace period, repeated outages and clock rollover\n";
}
void statusAndCleanup(){
  reset();auto result=request("/api/status",cookie());
  assert(result.body.indexOf("68719476736")>=0);
  sdBooks={"/Books/quote\"\\\n\t.txt"};result=request("/api/files",cookie());
  assert(result.body.indexOf("\\\"")>=0&&result.body.indexOf("\\\\")>=0);
  assert(result.body.indexOf('\n')<0&&result.body.indexOf('\t')<0);
  networkStop();SD.mkdir("/Books/.incoming");
  SD.add("/Books/.incoming/paperdiy-upload-0123456789abcdef.part","unfinished");
  SD.add("/Books/.incoming/paperdiy-upload-mybook.part","unrelated user file");
  SD.add("/Books/keep.epub","existing book");networkApply(NetworkMode::Ap);
  assert(SD.exists("/Books/keep.epub"));
  assert(!SD.exists("/Books/.incoming/paperdiy-upload-0123456789abcdef.part"));
  assert(SD.exists("/Books/.incoming/paperdiy-upload-mybook.part"));
  for(const auto& [path,node]:SD.files)assert(node->openHandles==0);
  networkStop();WiFi.failAp=true;networkApply(NetworkMode::Ap);
  assert(!WebServer::current&&!networkActive()&&WiFi.currentMode==WIFI_OFF);
  assert(settingsApSsid.length()==0&&settingsApPassword.length()==0&&settingsApUrl.length()==0);
  std::cout<<"network: 64-bit space reporting and lifecycle cleanup\n";
}
}

int main(){
 authenticationAndRandomness();qrAndTimeout();uploadsAndFilenames();uploadFailures();ownershipAndCatalog();
 credentialsAndStation();uploadRecovery();stationReconnectWindow();statusAndCleanup();networkStop();
 assert(WebServer::constructions==WebServer::destructions);
 std::cout<<"network integration passed\n";
}
