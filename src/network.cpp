#include "console.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <SD.h>
#include <mbedtls/sha256.h>
#include "prepared_epub.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include "bounded_web_server.h"
#include "network.h"
#include "document_format.h"
#include "ui.h"
#include "reader.h"
#include "sd_reader.h"
#include "reader_settings.h"
#include "shutdown_art.h"
#include "storage_validation.h"
#include "web_page.h"
#include "prepared_web_assets.h"
#include "prepare_worker.h"
#include "lan_service.h"

namespace {
static_assert(sizeof(prepared_web_assets::CORE_JS_GZ)+sizeof(prepared_web_assets::CORE_WASM_GZ)+
              sizeof(PREPARE_WORKER_JS)<=BoundedWebServer::MAX_FLASH_RESPONSE,
              "Browser preparation assets exceed the bounded flash budget");
BoundedWebServer* web=nullptr;
DNSServer dns;
bool apRunning=false,staRunning=false;
bool radioStopFailed=false;
bool deliveryDirty=false,receiverAttempted=false;
uint32_t apActivity=0,staStarted=0,uploadGeneration=0;
String apToken;

constexpr uint32_t AP_TIMEOUT_MS=5u*60u*1000u;
constexpr uint32_t UPLOAD_IDLE_MS=45u*1000u;
constexpr size_t UPLOAD_CHUNK_SIZE=4096;
constexpr char ALPHABET[]="ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
constexpr char WIFI_CONFIG_KEY[]="wifiCfgV1";
constexpr size_t WIFI_CONFIG_MAX=5+32+63;
constexpr char TEMP_PREFIX[]="paperdiy-upload-";

struct UploadState {
  File file;
  bool active=false,art=false,prepared=false;
  String id,temp,finalPath,sourceName;
  uint32_t expected=0,total=0,lastActivity=0,requestGeneration=0;
} upload;
// Session-scoped receipts bound memory and let a lost commit response be retried
// without importing the book twice. They never survive an AP or SD change.
struct UploadReceipt {
  String id,sourceName,finalName;
  uint32_t size=0,requestGeneration=0;
  bool prepared=false,art=false;
};
constexpr size_t RECEIPT_COUNT=4;
UploadReceipt receipts[RECEIPT_COUNT];
size_t nextReceipt=0;
uint8_t chunkBuffer[UPLOAD_CHUNK_SIZE];
mbedtls_sha256_context preparedSha;
bool preparedShaActive=false;



void touchAp(){apActivity=millis();}
String randomString(size_t length){
  String out;
  for(size_t i=0;i<length;++i)out+=(char)ALPHABET[esp_random()%(sizeof(ALPHABET)-1)];
  return out;
}
String randomHex(size_t length){
  static constexpr char digits[]="0123456789abcdef";
  String out;
  for(size_t i=0;i<length;++i)out+=(char)digits[esp_random()&15u];
  return out;
}
bool authorized(bool allowQuery=false){
  if(!web||apToken.length()==0)return false;
  if(allowQuery&&web->hasArg("t")&&web->arg("t")==apToken)return true;
  const String cookie=web->header("Cookie");
  const String expected="diy_token="+apToken;
  size_t start=0;
  while(start<cookie.length()){
    int end=cookie.indexOf(';',start);
    String item=end<0?cookie.substring(start):cookie.substring(start,end);
    item.trim();
    if(item==expected)return true;
    if(end<0)break;
    start=(size_t)end+1;
  }
  return false;
}
String jsonEscape(const String& in){
  String out;
  for(size_t i=0;i<in.length();++i){
    unsigned char c=(unsigned char)in.charAt(i);
    if(c=='"'||c=='\\'){out+='\\';out+=(char)c;}
    else if(c<32){char escaped[7];snprintf(escaped,sizeof(escaped),"\\u%04x",c);out+=escaped;}
    else out+=(char)c;
  }
  return out;
}
void sendJson(const String& body,int code=200){
  web->sendHeader("Cache-Control","no-store");
  web->sendHeader("X-Content-Type-Options","nosniff");
  web->send(code,"application/json",body);
}
void sendError(int code,const String& message){sendJson("{\"error\":\""+jsonEscape(message)+"\"}",code);}
bool requireAuth(){if(authorized())return true;sendError(401,"Scan the QR code on the reader to open this page.");return false;}

// Reject malformed UTF-8 and control characters without discarding Chinese names.
bool validText(const String& text){
  size_t i=0;
  while(i<text.length()){
    uint8_t c=(uint8_t)text.charAt(i++);
    if(c<128){if(c<32||c==127)return false;continue;}
    unsigned count;
    uint32_t cp,min;
    if(c>=0xc2&&c<=0xdf){count=1;cp=c&31;min=0x80;}
    else if(c>=0xe0&&c<=0xef){count=2;cp=c&15;min=0x800;}
    else if(c>=0xf0&&c<=0xf4){count=3;cp=c&7;min=0x10000;}
    else return false;
    if(text.length()-i<count)return false;
    while(count--){uint8_t d=(uint8_t)text.charAt(i++);if((d&0xc0)!=0x80)return false;cp=(cp<<6)|(d&63);}
    if(cp<min||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff)||(cp>=0x80&&cp<=0x9f))return false;
  }
  return true;
}
String validName(const String& name,bool prepared=false){
  // A valid 96-byte original may gain "-99" on collision and ".pdt" when
  // paired. Derived names must still refer to an existing unique source leaf.
  if(name.length()==0||name.length()>(prepared?103u:96u)||name.charAt(0)=='.'||!validText(name))return "";
  if(name.charAt(name.length()-1)==' '||name.charAt(name.length()-1)=='.')return "";
  for(size_t i=0;i<name.length();++i)if(strchr("/\\:*?\"<>|",name.charAt(i)))return "";
  const String original=prepared&&name.length()>4?name.substring(0,name.length()-4):name;
  String lower=name;lower.toLowerCase();
  if(prepared){if(!lower.endsWith(".epub.pdt")||document_format::detect(original.c_str())!=document_format::Kind::Epub)return "";}
  else if(document_format::detect(name.c_str())==document_format::Kind::Unsupported)return "";
  return name;
}
bool parseUnsigned(const String& value,uint32_t& number){
  if(value.length()==0||value.length()>10)return false;
  uint32_t result=0;
  for(size_t i=0;i<value.length();++i){
    const char c=value.charAt(i);
    if(c<'0'||c>'9'||result>(UINT32_MAX-(uint32_t)(c-'0'))/10u)return false;
    result=result*10u+(uint32_t)(c-'0');
  }
  number=result;return true;
}
bool cardReady(){return sdOK&&!sdOwnedByUsb()&&digitalRead(48)==LOW&&SD.cardType()!=CARD_NONE;}
uint64_t freeBytes(){
  if(!cardReady())return 0;
  const uint64_t total=SD.totalBytes(),used=SD.usedBytes();
  return total>used?total-used:0;
}
String uniquePath(const String& path){
  if(!SD.exists(path))return path;
  int dot=path.lastIndexOf('.');
  const String base=dot>0?path.substring(0,dot):path;
  const String ext=dot>0?path.substring(dot):String();
  for(unsigned i=1;i<100;++i){String candidate=base+"-"+String(i)+ext;if(!SD.exists(candidate))return candidate;}
  return "";
}
void resetUpload(){
  if(preparedShaActive){mbedtls_sha256_free(&preparedSha);preparedShaActive=false;}
  if(upload.file)upload.file.close();
  if(upload.temp.length()&&cardReady())SD.remove(upload.temp);
  upload=UploadState{};
}
void uploadFailure(int code,const String& message){resetUpload();sendError(code,message);}
void cleanIncoming(){
  if(!cardReady())return;
  File dir=SD.open("/Books/.incoming");
  if(!dir||!dir.isDirectory())return;
  File file;
  while((file=dir.openNextFile())){
    String name=file.name();
    const int slash=name.lastIndexOf('/');if(slash>=0)name=name.substring(slash+1);
    bool owned=!file.isDirectory()&&name.startsWith(TEMP_PREFIX)&&name.endsWith(".part")&&
      name.length()==sizeof(TEMP_PREFIX)-1+16+5;
    for(size_t i=sizeof(TEMP_PREFIX)-1;owned&&i<sizeof(TEMP_PREFIX)-1+16;++i){
      const char c=name.charAt(i);owned=(c>='0'&&c<='9')||(c>='a'&&c<='f');
    }
    file.close();
    if(owned)SD.remove("/Books/.incoming/"+name);
  }
  dir.close();
}

bool validCredentials(const String& ssid,const String& pass){
  return ssid.length()>0&&ssid.length()<=32&&pass.length()>=8&&pass.length()<=63&&validText(ssid)&&validText(pass);
}
bool loadCredentials(String& ssid,String& pass){
  uint8_t data[WIFI_CONFIG_MAX];
  const size_t length=prefs.getBytesLength(WIFI_CONFIG_KEY);
  if(length>=5&&length<=sizeof(data)&&prefs.getBytes(WIFI_CONFIG_KEY,data,length)==length&&
     data[0]=='P'&&data[1]=='W'&&data[2]==1&&data[3]>=1&&data[3]<=32&&data[4]>=8&&data[4]<=63&&length==(size_t)(5+data[3]+data[4])){
    ssid="";pass="";
    for(size_t i=0;i<data[3];++i)ssid+=(char)data[5+i];
    for(size_t i=0;i<data[4];++i)pass+=(char)data[5+data[3]+i];
    if(validCredentials(ssid,pass))return true;
  }
  // Read old settings without migrating/writing until the user explicitly saves.
  ssid=prefs.getString("wifiSsid","");pass=prefs.getString("wifiPass","");
  if(validCredentials(ssid,pass))return true;
  ssid="";pass="";return false;
}
bool saveCredentials(const String& ssid,const String& pass){
  uint8_t data[WIFI_CONFIG_MAX]={'P','W',1,(uint8_t)ssid.length(),(uint8_t)pass.length()};
  memcpy(data+5,ssid.c_str(),ssid.length());
  memcpy(data+5+ssid.length(),pass.c_str(),pass.length());
  const size_t length=5+ssid.length()+pass.length();
  // One NVS value is the transaction boundary; never expose a mixed SSID/password.
  return prefs.putBytes(WIFI_CONFIG_KEY,data,length)==length;
}

void handleRoot(){
  if(!authorized(true)){sendError(403,"Scan the QR code on the reader to open this page.");return;}
  touchAp();
  web->sendHeader("Set-Cookie","diy_token="+apToken+"; Path=/; HttpOnly; SameSite=Strict");
  web->sendHeader("Cache-Control","no-store");
  web->sendHeader("Content-Security-Policy","default-src 'self'; style-src 'unsafe-inline'; script-src 'self' 'unsafe-inline' 'wasm-unsafe-eval'; worker-src 'self'; connect-src 'self'; img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'");
  web->sendHeader("X-Content-Type-Options","nosniff");
  web->sendHeader("X-Frame-Options","DENY");
  web->sendHeader("Referrer-Policy","no-referrer");
  web->send_P(200,"text/html",INDEX_HTML);
}
void handlePrepareAsset(const uint8_t* bytes,size_t size,const char* type,bool gzip=true){
  if(!requireAuth())return;
  touchAp();
  web->sendHeader("Cache-Control","no-store");
  if(gzip)web->sendHeader("Content-Encoding","gzip");
  web->sendHeader("X-Content-Type-Options","nosniff");
  web->sendHeader("Content-Security-Policy","default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; connect-src 'self'");
  web->sendFlash(200,type,bytes,size);
}
void handleStatus(){
  if(!requireAuth())return;
  char freeString[24];snprintf(freeString,sizeof(freeString),"%llu",(unsigned long long)freeBytes());
  sendJson("{\"clients\":"+String(WiFi.softAPgetStationNum())+",\"busy\":"+(upload.active?"true":"false")+
    ",\"sd\":"+(cardReady()?"true":"false")+",\"free\":"+freeString+",\"preprocessing\":true,\"uploadGeneration\":"+String(uploadGeneration)+"}");
}
void handleFiles(){
  if(!requireAuth())return;
  uint32_t offset=0;
  if(web->hasArg("offset")&&!parseUnsigned(web->arg("offset"),offset)){sendError(400,"Invalid book list offset.");return;}
  const size_t count=cardReady()?sdBooks.size():0;
  size_t end=offset;
  String body="{\"files\":[";
  while(end<count&&end-(size_t)offset<24){
    if(end!=offset)body+=",";
    body+="\""+jsonEscape(sdBooks[end].substring(sdBooks[end].lastIndexOf('/')+1))+"\"";
    ++end;
  }
  sendJson(body+"],\"next\":"+(end<count?String(end):String("null"))+"}");
}
void handleWifi(){
  if(!requireAuth())return;
  if(!web->hasArg("ssid")||!web->hasArg("pass")){sendError(400,"Missing SSID or password");return;}
  const String ssid=web->arg("ssid"),pass=web->arg("pass");
  if(!validCredentials(ssid,pass)){sendError(400,"SSID must be 1–32 UTF-8 bytes and password 8–63 UTF-8 bytes, without control characters.");return;}
  if(!saveCredentials(ssid,pass)){sendError(500,"Could not save Wi-Fi settings. Please retry.");return;}
  settingsStaSsid=ssid;settingsStaConfigured=true;settingsStaConnected=false;settingsStaError="";settingsStaIp="";
  touchAp();sendJson("{\"ok\":true}");
}
void sendSettings(){
  const auto& c=reader_settings::current();
  sendJson("{\"punctuation\":"+String(unsigned(c.punctuation))+",\"spacing\":"+String(c.spacing)+
    ",\"doubleMs\":"+String(c.doubleMs)+",\"preset\":"+String(c.preset)+",\"hyphenation\":"+String(unsigned(c.hyphenation))+
    ",\"justify\":"+String(unsigned(c.justify))+",\"refresh\":"+String(c.refresh)+",\"ink\":"+String(c.ink)+",\"art\":"+String(c.art)+"}");
}
void handleSettingsGet(){if(requireAuth())sendSettings();}
void handleSettingsSave(){
  if(!requireAuth())return;
  if(upload.active){sendError(409,"Wait for the upload to finish.");return;}
  const char* fields[]={"punctuation","spacing","doubleMs","preset","hyphenation","justify","refresh","ink"};
  uint32_t v[8];
  for(unsigned i=0;i<8;++i)if(!web->hasArg(fields[i])||!parseUnsigned(web->arg(fields[i]),v[i])||v[i]>400){sendError(400,"Invalid settings.");return;}
  if(v[0]>1||v[4]>1||v[5]>1){sendError(400,"Invalid settings.");return;}
  auto next=reader_settings::current();next.punctuation=v[0];next.spacing=v[1];next.doubleMs=v[2];next.preset=v[3];next.hyphenation=v[4];next.justify=v[5];next.refresh=v[6];next.ink=v[7];
  // Check before narrowing: values like 256 must never become a valid zero.
  if(v[1]>4||v[3]>2||v[6]>2||v[7]>100||!reader_settings::valid(next)){sendError(400,"Invalid settings.");return;}
  if(!reader_settings::save(prefs,next)){sendError(500,"Could not save settings. Please retry.");return;}
  touchAp();sendSettings();
}
void handleArtwork(){
  if(!requireAuth())return;
  if(upload.active){sendError(409,"Wait for the upload to finish.");return;}
  const auto action=web->arg("action");
  if(action!="remove"&&action!="default"){sendError(400,"Invalid image action.");return;}
  auto next=reader_settings::current();next.art=action=="remove"?3:0;
  if(!reader_settings::save(prefs,next)){sendError(500,"Could not save image choice. Please retry.");return;}
  // Disable the image durably before deleting only our two upload slots.
  // The compiled private fallback, books and progress are untouched.
  if(action=="remove"&&cardReady()){
    storage_validation::sourceChanged();
    for(uint8_t slot=1;slot<=2;++slot)if(SD.exists(shutdown_art::path(slot))&&!SD.remove(shutdown_art::path(slot))){
      sendError(500,"Image disabled, but SD cleanup failed. Please retry.");return;
    }
  }
  touchAp();sendSettings();
}
bool validRequestId(const String& id){
  if(id.length()!=32)return false;
  for(size_t i=0;i<id.length();++i){const char c=id.charAt(i);if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;}
  return true;
}
void clearReceipts(){for(auto& receipt:receipts)receipt=UploadReceipt{};nextReceipt=0;}
const UploadReceipt* findReceipt(const String& id){
  if(id.length()==0)return nullptr;
  for(const auto& receipt:receipts)if(receipt.id==id)return &receipt;
  return nullptr;
}
void sendStart(const String& id,uint32_t offset){
  sendJson("{\"id\":\""+id+"\",\"chunkSize\":"+String(UPLOAD_CHUNK_SIZE)+",\"offset\":"+String(offset)+"}");
}
void sendReceipt(const UploadReceipt& receipt){
  sendJson("{\"ok\":true,\"name\":\""+jsonEscape(receipt.finalName)+"\"}");
}
String preparedSource(const String& name){
  const String leaf=name.substring(0,name.length()-4);String source;
  for(const auto& path:sdBooks)if(path.substring(path.lastIndexOf('/')+1)==leaf){
    if(source.length())return "";source=path;
  }
  if(source.length()){File file=SD.open(source);const bool regular=file&&!file.isDirectory();file.close();if(!regular)return "";}
  return source;
}
void handleUploadStart(){
  if(!requireAuth())return;
  const String requestId=web->hasArg("requestId")?web->arg("requestId"):String();
  if(web->hasArg("requestId")&&!validRequestId(requestId)){sendError(400,"Invalid upload request identity.");return;}
  const bool art=web->arg("kind")=="art",prepared=web->arg("kind")=="prepared";
  if(web->hasArg("kind")&&!art&&!prepared){sendError(400,"Invalid upload kind.");return;}
  const String name=art?String("shutdown.pdi"):validName(web->arg("name"),prepared);
  if(name.length()==0){sendError(400,"Use a valid book or .epub.pdt filename, up to 96 UTF-8 bytes.");return;}
  uint32_t size=0,requestedGeneration=0;
  const auto format=document_format::detect(name.c_str());
  const size_t limit=art?shutdown_art::MAX_BYTES:prepared?prepared_epub::HEADER_BYTES+prepared_epub::MAX_TEXT_BYTES+prepared_epub::MAX_METADATA_BYTES:document_format::sourceLimit(format);
  if(!parseUnsigned(web->arg("size"),size)||size==0||size>limit){
    sendError(413,prepared?"Prepared content exceeds the supported size.":format==document_format::Kind::Text?"TXT must be 1 byte to 4 MiB.":
      format==document_format::Kind::Markdown?"Markdown must be 1 byte to 1 MiB.":"EPUB must be 1 byte to 64 MiB.");return;
  }
  if(!parseUnsigned(web->arg("generation"),requestedGeneration)||web->arg("generation")!=String(requestedGeneration)){
    sendError(409,"Upload state changed. Refresh and retry.");return;
  }
  if(requestId.length()){
    if(upload.active&&upload.id==requestId){
      if(upload.sourceName!=name||upload.expected!=size||upload.requestGeneration!=requestedGeneration||upload.art!=art||upload.prepared!=prepared){sendError(409,"Upload identity already used for different metadata.");return;}
      upload.lastActivity=millis();touchAp();sendStart(upload.id,upload.total);return;
    }
    if(const UploadReceipt* receipt=findReceipt(requestId)){
      if(receipt->sourceName!=name||receipt->size!=size||receipt->requestGeneration!=requestedGeneration||receipt->art!=art||receipt->prepared!=prepared){sendError(409,"Upload identity already used for different metadata.");return;}
      touchAp();sendStart(receipt->id,receipt->size);return;
    }
  }
  if(upload.active){sendError(409,"Another upload is in progress. Retry when it finishes.");return;}
  if(!art&&!prepared&&sdBooks.size()>=256){sendError(409,"The library is full (256 books).");return;}
  if(requestedGeneration!=uploadGeneration){sendError(409,"Upload state changed. Refresh and retry.");return;}
  if(!cardReady()){sendError(503,"SD card unavailable");return;}
  const String source=prepared?preparedSource(name):String();
  if(prepared&&source.length()==0){sendError(409,"Upload the matching EPUB first. Its filename must be unique in the library.");return;}
  if(size>freeBytes()){sendError(507,"Not enough free space on the SD card.");return;}
  storage_validation::sourceChanged();
  if((!SD.exists("/Books")&&!SD.mkdir("/Books"))||(!SD.exists("/Books/.incoming")&&!SD.mkdir("/Books/.incoming"))){
    sendError(500,"Cannot create the book directory.");return;
  }
  if(art&&!SD.exists("/.paperdiy")&&!SD.mkdir("/.paperdiy")){sendError(500,"Cannot create image directory.");return;}
  upload.art=art;upload.prepared=prepared;
  upload.finalPath=art?String(shutdown_art::path(reader_settings::current().art==1?2:1)):prepared?source+".pdt":uniquePath("/Books/"+name);
  if(upload.finalPath.length()==0){resetUpload();sendError(409,"Cannot allocate a filename.");return;}
  for(unsigned attempt=0;attempt<4;++attempt){
    const String candidate=String("/Books/.incoming/")+TEMP_PREFIX+randomHex(16)+".part";
    if(!SD.exists(candidate)){upload.temp=candidate;break;}
  }
  if(upload.temp.length()==0){resetUpload();sendError(500,"Cannot create a temporary file.");return;}
  upload.file=SD.open(upload.temp,FILE_WRITE);
  if(!upload.file){uploadFailure(500,"Cannot write to the SD card.");return;}
  upload.active=true;upload.id=requestId.length()?requestId:randomHex(32);upload.expected=size;upload.total=0;upload.lastActivity=millis();
  upload.sourceName=name;upload.requestGeneration=requestedGeneration;
  if(prepared){mbedtls_sha256_init(&preparedSha);mbedtls_sha256_starts_ret(&preparedSha,0);preparedShaActive=true;}
  ++uploadGeneration;touchAp();sendStart(upload.id,0);
}
void handleUploadStatus(){
  if(!requireAuth())return;
  const String id=web->arg("id");
  if(upload.active&&id==upload.id){
    sendJson("{\"state\":\"receiving\",\"offset\":"+String(upload.total)+",\"size\":"+String(upload.expected)+"}");return;
  }
  if(const UploadReceipt* receipt=findReceipt(id)){
    sendJson("{\"state\":\"complete\",\"offset\":"+String(receipt->size)+",\"size\":"+String(receipt->size)+
      ",\"name\":\""+jsonEscape(receipt->finalName)+"\"}");return;
  }
  sendError(409,"Upload expired or is unknown. Check the book list before retrying.");
}
bool requireUpload(){
  if(!requireAuth())return false;
  if(!upload.active||!web->hasArg("id")||web->arg("id")!=upload.id){sendError(409,"Upload expired or is not owned by this page. Start again.");return false;}
  return true;
}
int base64Value(char c){
  if(c>='A'&&c<='Z')return c-'A';
  if(c>='a'&&c<='z')return c-'a'+26;
  if(c>='0'&&c<='9')return c-'0'+52;
  if(c=='+')return 62;
  if(c=='/')return 63;
  return -1;
}
bool decodeChunk(const String& encoded,size_t& bytes){
  bytes=0;
  if(encoded.length()==0||encoded.length()%4||encoded.length()>((UPLOAD_CHUNK_SIZE+2)/3)*4)return false;
  for(size_t i=0;i<encoded.length();i+=4){
    const int a=base64Value(encoded.charAt(i)),b=base64Value(encoded.charAt(i+1));
    const char third=encoded.charAt(i+2),fourth=encoded.charAt(i+3);
    const bool pad2=third=='=',pad1=fourth=='=';
    const int c=pad2?0:base64Value(third),d=pad1?0:base64Value(fourth);
    if(a<0||b<0||c<0||d<0||(pad2&&!pad1)||((pad1||pad2)&&i+4!=encoded.length())||
       (pad2&&(b&15))||(pad1&&!pad2&&(c&3)))return false;
    const size_t count=pad2?1:(pad1?2:3);
    if(bytes+count>UPLOAD_CHUNK_SIZE)return false;
    chunkBuffer[bytes++]=(uint8_t)((a<<2)|(b>>4));
    if(count>1)chunkBuffer[bytes++]=(uint8_t)((b<<4)|(c>>2));
    if(count>2)chunkBuffer[bytes++]=(uint8_t)((c<<6)|d);
  }
  return true;
}
void handleUploadChunk(){
  if(!requireUpload())return;
  uint32_t offset=0;
  if(!parseUnsigned(web->arg("offset"),offset)||offset!=upload.total){sendError(409,"Unexpected upload offset. Cancel and retry.");return;}
  size_t bytes=0;
  if(!decodeChunk(web->arg("data"),bytes)||bytes>upload.expected-upload.total){uploadFailure(400,"Invalid upload chunk.");return;}
  if(!cardReady()){uploadFailure(503,"SD card removed during upload.");return;}
  if(!upload.file||upload.file.write(chunkBuffer,bytes)!=bytes){uploadFailure(500,"SD write failed.");return;}
  if(upload.prepared){
    const size_t skip=upload.total<prepared_epub::HEADER_BYTES?std::min(bytes,prepared_epub::HEADER_BYTES-size_t(upload.total)):0;
    if(bytes>skip)mbedtls_sha256_update_ret(&preparedSha,chunkBuffer+skip,bytes-skip);
  }
  upload.total+=(uint32_t)bytes;upload.lastActivity=millis();touchAp();
  sendJson("{\"offset\":"+String(upload.total)+"}");
}
void handleUploadCommit(){
  if(!requireAuth())return;
  if(const UploadReceipt* receipt=findReceipt(web->arg("id"))){touchAp();sendReceipt(*receipt);return;}
  if(!requireUpload())return;
  if(upload.total!=upload.expected){uploadFailure(400,"Upload is incomplete.");return;}
  if(!cardReady()){uploadFailure(503,"SD card removed during upload.");return;}
  upload.file.flush();upload.file.close();
  File completed=SD.open(upload.temp);
  bool complete=completed&&completed.size()==upload.expected;
  if(complete&&upload.art)complete=bool(shutdown_art::read(completed));
  if(complete&&upload.prepared){
    uint8_t header[prepared_epub::HEADER_BYTES],digest[32];prepared_epub::Header metadata;
    complete=completed.read(header,sizeof(header))==sizeof(header)&&prepared_epub::decode(header,sizeof(header),completed.size(),metadata);
    mbedtls_sha256_finish_ret(&preparedSha,digest);mbedtls_sha256_free(&preparedSha);preparedShaActive=false;
    complete=complete&&!memcmp(digest,metadata.payloadDigest,sizeof(digest));
    File source=SD.open(upload.finalPath.substring(0,upload.finalPath.length()-4));
    complete=complete&&source&&!source.isDirectory()&&source.size()==metadata.sourceBytes;source.close();
    // The reader independently verifies source SHA, payload SHA, UTF-8 and IR
    // after rereading SD, before any prepared content becomes visible.
  }
  if(completed)completed.close();
  if(!complete){uploadFailure(500,"Could not verify the saved file.");return;}
  // Recheck immediately before rename; the destination must never overwrite a book.
  if(upload.art){
    if(SD.exists(upload.finalPath)&&!SD.remove(upload.finalPath)){uploadFailure(500,"Cannot replace inactive image.");return;}
  }else if(upload.prepared){
    const String backup=upload.finalPath+".bak";
    if(SD.exists(upload.finalPath)){
      if(SD.exists(backup)&&!SD.remove(backup)){uploadFailure(500,"Cannot rotate prepared content.");return;}
      if(!SD.rename(upload.finalPath,backup)){uploadFailure(500,"Cannot retain previous prepared content.");return;}
    }
  }else upload.finalPath=uniquePath(upload.finalPath);
  if(upload.finalPath.length()==0||!SD.rename(upload.temp,upload.finalPath)){
    if(upload.prepared&&!SD.exists(upload.finalPath)&&SD.exists(upload.finalPath+".bak"))SD.rename(upload.finalPath+".bak",upload.finalPath);
    uploadFailure(500,"Cannot finalize the book.");return;
  }
  const bool artwork=upload.art,prepared=upload.prepared;
  if(artwork){
    auto next=reader_settings::current();next.art=next.art==1?2:1;
    if(!reader_settings::save(prefs,next)){SD.remove(upload.finalPath);uploadFailure(500,"Could not save image choice. Please retry.");return;}
  }
  const String savedPath=upload.finalPath;
  const String name=savedPath.substring(savedPath.lastIndexOf('/')+1);
  UploadReceipt& receipt=receipts[nextReceipt];
  receipt.id=upload.id;receipt.sourceName=upload.sourceName;receipt.finalName=name;
  receipt.art=upload.art;receipt.prepared=upload.prepared;
  receipt.size=upload.expected;receipt.requestGeneration=upload.requestGeneration;
  nextReceipt=(nextReceipt+1)%RECEIPT_COUNT;
  upload.temp="";resetUpload();
  if(artwork||prepared){touchAp();sendReceipt(receipt);return;}
  // This AP is the only SD writer. Publish the known leaf without unmounting
  // the card or disturbing the selected book and its independent bookmark.
  const String previous=selectedBook<sdBooks.size()?sdBooks[selectedBook]:String();
  sdBooks.push_back(savedPath);
  std::sort(sdBooks.begin(),sdBooks.end(),[](const String& a,const String& b){return strcmp(a.c_str(),b.c_str())<0;});
  selectedBook=0;
  for(size_t i=0;i<sdBooks.size();++i)if(sdBooks[i]==previous){selectedBook=i;break;}
  touchAp();
  sendJson("{\"ok\":true,\"name\":\""+jsonEscape(name)+"\"}");
}
void handleUploadCancel(){
  if(!requireUpload())return;
  resetUpload();touchAp();sendJson("{\"ok\":true}");
}
void handleNotFound(){sendError(404,"Not found. Open the QR link on the reader.");}
void startAp(){
  WiFi.persistent(false);
  // The radio must be running before using the hardware RNG for credentials.
  if(!WiFi.mode(WIFI_AP))return;
  settingsApSsid="PaperDIY-"+randomString(4);
  settingsApPassword=randomString(12);
  apToken=randomHex(32);
  if(!WiFi.softAP(settingsApSsid.c_str(),settingsApPassword.c_str(),1,0,1)){
    networkStop();return;
  }
  web=new(std::nothrow) BoundedWebServer(80);
  if(!web){networkStop();return;}
  const IPAddress ip=WiFi.softAPIP();
  settingsApUrl="http://"+ip.toString()+"/?t="+apToken;
  settingsApClientConnected=false;
  apRunning=true;uploadGeneration=0;touchAp();
  cleanIncoming();
  const char* headerKeys[]={"Cookie"};
  web->collectHeaders(headerKeys,1);
  web->on("/",HTTP_GET,handleRoot);
  web->on("/prepare-worker.js",HTTP_GET,[]{handlePrepareAsset(reinterpret_cast<const uint8_t*>(PREPARE_WORKER_JS),sizeof(PREPARE_WORKER_JS)-1,"application/javascript",false);});
  web->on("/prepare-core.js",HTTP_GET,[]{handlePrepareAsset(prepared_web_assets::CORE_JS_GZ,sizeof(prepared_web_assets::CORE_JS_GZ),"application/javascript");});
  web->on("/prepare-core.wasm",HTTP_GET,[]{handlePrepareAsset(prepared_web_assets::CORE_WASM_GZ,sizeof(prepared_web_assets::CORE_WASM_GZ),"application/wasm");});
  web->on("/api/status",HTTP_GET,handleStatus);
  web->on("/api/files",HTTP_GET,handleFiles);
  web->on("/api/wifi",HTTP_POST,handleWifi);
  web->on("/api/settings",HTTP_GET,handleSettingsGet);
  web->on("/api/settings",HTTP_POST,handleSettingsSave);
  web->on("/api/artwork",HTTP_POST,handleArtwork);
  web->on("/api/upload/start",HTTP_POST,handleUploadStart);
  web->on("/api/upload/status",HTTP_GET,handleUploadStatus);
  web->on("/api/upload/chunk",HTTP_POST,handleUploadChunk);
  web->on("/api/upload/commit",HTTP_POST,handleUploadCommit);
  web->on("/api/upload/cancel",HTTP_POST,handleUploadCancel);
  web->onNotFound(handleNotFound);
  web->begin();dns.start(53,"*",ip);
}
void startSta(){
  String ssid,pass;
  settingsStaConfigured=loadCredentials(ssid,pass);
  settingsStaSsid=ssid;settingsStaConnected=false;settingsStaError="";settingsStaIp="";
  if(!settingsStaConfigured)return;
  WiFi.persistent(false);
  if(!WiFi.mode(WIFI_STA)){settingsStaError="Connection failed";return;}
  WiFi.begin(ssid.c_str(),pass.c_str());staRunning=true;receiverAttempted=false;staStarted=millis();
}
}

void networkStop(){
  lan_service::stop();receiverAttempted=false;deliveryDirty=false;
  if(web){web->stop();delete web;web=nullptr;}
  dns.stop();resetUpload();clearReceipts();
  apRunning=false;staRunning=false;
  WiFi.softAPdisconnect(true);WiFi.disconnect(true);
  radioStopFailed=!WiFi.mode(WIFI_OFF);
#ifdef ARDUINO
  if(radioStopFailed)Console.println("NETWORK_STOP_FAILED radio_state_unconfirmed=1");
#endif
  settingsApSsid="";settingsApPassword="";settingsApUrl="";apToken="";settingsApClientConnected=false;
  settingsStaConnected=false;settingsStaIp="";settingsStaError="";
}
void networkApply(NetworkMode mode){
  networkStop();
  if(mode==NetworkMode::Ap)startAp();
  else if(mode==NetworkMode::Sta)startSta();
}
bool networkPoll(){
  bool changed=false;
  if(apRunning&&web){
    const bool connected=WiFi.softAPgetStationNum()>0;
    if(connected!=settingsApClientConnected){
      settingsApClientConnected=connected;changed=true;
      if(connected)touchAp();else resetUpload();
    }
    if(upload.active&&(!cardReady()||millis()-upload.lastActivity>=UPLOAD_IDLE_MS))resetUpload();
    dns.processNextRequest();web->handleClient();
  }
  if(staRunning){
    const bool connected=WiFi.status()==WL_CONNECTED;
    if(connected){
      const String ip=WiFi.localIP().toString();
      if(!settingsStaConnected||ip!=settingsStaIp){
        // A DHCP address change invalidates the endpoint in the physical QR.
        if(receiverAttempted){lan_service::stop();receiverAttempted=false;}
        settingsStaConnected=true;settingsStaError="";settingsStaIp=ip;changed=true;
      }
      if(!receiverAttempted){
        receiverAttempted=true;
        if(!lan_service::start(ip.c_str())){settingsLanState="Receiver unavailable";deliveryDirty=true;}
      }
      if(lan_service::poll())deliveryDirty=true;
    }else if(settingsStaConnected){
      lan_service::stop();receiverAttempted=false;
      settingsStaConnected=false;settingsStaIp="";settingsStaError="";
      staStarted=millis();changed=true;
    }
    if(!connected&&settingsStaError.length()==0&&millis()-staStarted>=20000){
      lan_service::stop();WiFi.disconnect(true);radioStopFailed=!WiFi.mode(WIFI_OFF);staRunning=false;
      settingsStaError="Connection failed";changed=true;
    }
  }
  return changed;
}
bool networkDeliveryChanged(){const bool changed=deliveryDirty;deliveryDirty=false;return changed;}
bool networkApExpired(){return apRunning&&!upload.active&&millis()-apActivity>=AP_TIMEOUT_MS;}
bool networkActive(){
  // Never allow the battery sleep policy to infer a stopped radio merely from
  // application flags when the SDK rejected shutdown. A later explicit mode
  // transition retries shutdown; querying here does not mutate the radio.
  return apRunning||staRunning||radioStopFailed||WiFi.getMode()!=WIFI_OFF;
}
void networkStorageChanged(){
  lan_service::stop();receiverAttempted=false;
  lan_service::storageChanged();resetUpload();clearReceipts();cleanIncoming();
}
