#pragma once
#include "Arduino.h"
#include "WiFi.h"
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <vector>
enum HTTPMethod {HTTP_ANY,HTTP_GET,HTTP_POST};
inline constexpr size_t CONTENT_LENGTH_NOT_SET=static_cast<size_t>(-1);
struct RequestArgument {String key,value;};
struct RequestHandler {
 HTTPMethod method;
 String path;
 std::function<void()> action;
 RequestHandler* following=nullptr;
 RequestHandler(HTTPMethod m,const String& p,std::function<void()> fn):method(m),path(p),action(fn){}
 RequestHandler* next(){return following;}
 bool canHandle(HTTPMethod m,const String& p)const{return (method==HTTP_ANY||method==m)&&path==p;}
};
enum HTTPUploadStatus {UPLOAD_FILE_START,UPLOAD_FILE_WRITE,UPLOAD_FILE_END,UPLOAD_FILE_ABORTED};
struct HTTPUpload {
 HTTPUploadStatus status=UPLOAD_FILE_START;
 String filename,name="file",type="application/octet-stream";
 size_t currentSize=0,totalSize=0;
 uint8_t buf[8192]{};
};
struct MockResponse {
 int code=0;
 String type,body;
 std::map<std::string,String> headers;
};
class WebServer {
 public:
  struct Route {HTTPMethod method=HTTP_ANY;std::function<void()> handler,upload;};
  inline static WebServer* current=nullptr;
  inline static unsigned constructions=0,destructions=0;
  std::map<std::string,Route> routes;
  std::map<std::string,String> args,requestHeaders;
  std::set<std::string> collected{"Authorization"};
  MockResponse response;
  HTTPUpload uploadState;
  bool running=false;
  unsigned polls=0;
  std::function<void()> fallback;
  explicit WebServer(int){current=this;++constructions;}
  virtual ~WebServer(){delete[] _currentHeaders;delete[] _currentArgs;current=nullptr;++destructions;}
  void collectHeaders(const char** names,size_t count){
    for(size_t i=0;i<count;++i)collected.insert(names[i]);
    delete[] _currentHeaders;_headerKeysCount=int(collected.size());
    _currentHeaders=new RequestArgument[collected.size()];size_t i=0;
    for(const auto& key:collected)_currentHeaders[i++].key=key;
  }
  void on(const char* path,std::function<void()> fn){on(path,HTTP_ANY,fn);}
  void on(const char* path,HTTPMethod method,std::function<void()> fn){on(path,method,fn,{});}
  void on(const char* path,HTTPMethod method,std::function<void()> fn,std::function<void()> upload){
    routes[path]={method,fn,upload};
    auto handler=std::make_unique<RequestHandler>(method,path,fn);
    if(handlers_.empty())_firstHandler=handler.get();else handlers_.back()->following=handler.get();
    handlers_.push_back(std::move(handler));
  }
  void onNotFound(std::function<void()> fn){fallback=fn;}
  bool hasArg(const char* key)const{
    for(int i=0;i<_currentArgCount;++i)if(_currentArgs[i].key==key)return true;
    return args.count(key);
  }
  String arg(const char* key)const{
    for(int i=0;i<_currentArgCount;++i)if(_currentArgs[i].key==key)return _currentArgs[i].value;
    auto it=args.find(key);return it==args.end()?String():it->second;
  }
  String header(const char* key)const{
    for(int i=0;i<_headerKeysCount;++i)
      if(lower(_currentHeaders[i].key)==lower(key)&&_currentHeaders[i].value.length())return _currentHeaders[i].value;
    if(!collected.count(key))return "";
    auto it=requestHeaders.find(key);return it==requestHeaders.end()?String():it->second;
  }
  bool hasHeader(const char* key)const{return header(key).length()>0;}
  HTTPUpload& upload(){return uploadState;}
  void sendHeader(const char* key,const String& value,bool=false){response.headers[key]=value;}
  void send(int code,const char* type,const String& body=""){
    response.code=code;response.type=type;response.body=body;
    if(_currentClient){
      String output="HTTP/1.1 "+String(code)+" Response\r\nContent-Type: "+type+"\r\n";
      for(const auto& [key,value]:response.headers)output+=String(key)+": "+value+"\r\n";
      output+="Content-Length: "+String(body.length())+"\r\n\r\n"+body;
      _currentClientWrite(output.c_str(),output.length());
    }
  }
  void send_P(int code,const char* type,const char* body){send(code,type,body);}
  void begin(){running=true;}
  virtual void close(){running=false;}
  void stop(){close();}
  virtual void handleClient(){++polls;}
  std::shared_ptr<MockSocket> queueClient(const std::string& input,bool connected=true){
    auto socket=std::make_shared<MockSocket>();socket->input=input;socket->open=connected;
    _server.pending.emplace_back(socket);return socket;
  }
  void prepare(const String& cookie=""){
    args.clear();requestHeaders.clear();response={};uploadState={};
    if(cookie.length())requestHeaders["Cookie"]=cookie;
  }
  void invoke(const char* path,HTTPMethod method=HTTP_GET){
    auto found=routes.find(path);
    if(found==routes.end()||(found->second.method!=HTTP_ANY&&found->second.method!=method)){if(fallback)fallback();return;}
    found->second.handler();
  }
  void uploadEvent(HTTPUploadStatus status,const String& filename="",size_t bytes=0){
    uploadState.status=status;
    if(status==UPLOAD_FILE_START){uploadState.filename=filename;uploadState.totalSize=0;}
    uploadState.currentSize=bytes;
    if(status==UPLOAD_FILE_WRITE)uploadState.totalSize+=bytes;
    routes.at("/api/upload").upload();
  }
 protected:
  WiFiServer _server;
  WiFiClient _currentClient;
  HTTPMethod _currentMethod=HTTP_GET;
  String _currentUri,_hostHeader,_responseHeaders;
  uint8_t _currentVersion=1;
  bool _chunked=false;
  size_t _contentLength=CONTENT_LENGTH_NOT_SET;
  int _clientContentLength=0,_headerKeysCount=0,_currentArgCount=0;
  RequestHandler *_currentHandler=nullptr,*_firstHandler=nullptr;
  RequestArgument *_currentHeaders=nullptr,*_currentArgs=nullptr;
  virtual size_t _currentClientWrite(const char* data,size_t size){return _currentClient.write(data,size);}
  virtual size_t _currentClientWrite_P(const char* data,size_t size){return _currentClientWrite(data,size);}
  void _collectHeader(const char* key,const char* value){
    for(int i=0;i<_headerKeysCount;++i)if(lower(_currentHeaders[i].key)==lower(key))_currentHeaders[i].value=value;
  }
  void _handleRequest(){
    response={};
    if(_currentHandler)_currentHandler->action();else if(fallback)fallback();
  }
 private:
  static String lower(String text){text.toLowerCase();return text;}
  std::vector<std::unique_ptr<RequestHandler>> handlers_;
};
