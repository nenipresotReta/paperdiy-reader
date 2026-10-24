#include "lan_tls.h"
#ifdef ARDUINO
#include <Arduino.h>
#include <WiFi.h>
#include <lwip/sockets.h>
#include <cerrno>
#include <memory>
#include <new>
#include <mbedtls/ssl.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/sha256.h>
#include "lan_auth.h"

namespace lan_tls {
namespace {
constexpr size_t IDENTITY_LIMIT=2048,RESPONSE_LIMIT=4096;
constexpr uint32_t REQUEST_MS=20000,IDLE_MS=5000,HANDSHAKE_MS=7000;
int randomBytes(void*,unsigned char* out,size_t length){esp_fill_random(out,length);return 0;}
bool hash(const uint8_t* data,size_t length,uint8_t out[32]){return mbedtls_sha256_ret(data,length,out,0)==0;}
// Key and certificate are persisted together. An unreadable existing identity
// is an error, never permission to silently rotate a sender's trust anchor.
struct WipeBuffer{void operator()(uint8_t* bytes)const{if(bytes){lan_auth::erase(bytes,IDENTITY_LIMIT);delete[] bytes;}}};
using SensitiveBuffer=std::unique_ptr<uint8_t[],WipeBuffer>;
struct Identity {
  mbedtls_pk_context key;
  mbedtls_x509_crt cert;
  std::string fingerprint,id;
  Identity(){mbedtls_pk_init(&key);mbedtls_x509_crt_init(&cert);}
  ~Identity(){mbedtls_pk_free(&key);mbedtls_x509_crt_free(&cert);}
  bool load(lan_nvs::Store& preferences){
    constexpr const char* name="lanTlsV1";
    SensitiveBuffer data(new(std::nothrow) uint8_t[IDENTITY_LIMIT]);if(!data)return false;
    const auto state=preferences.keyState(name);if(state==lan_nvs::KeyState::Error)return false;
    size_t size=preferences.getBytesLength(name);
    if(state==lan_nvs::KeyState::Present){
      if(size<8+32||size>IDENTITY_LIMIT||preferences.getBytes(name,data.get(),size)!=size||std::memcmp(data.get(),"LTI1",4))return false;
      const size_t keyLength=size_t(data[4])|(size_t(data[5])<<8),certLength=size_t(data[6])|(size_t(data[7])<<8);
      uint8_t digest[32];
      if(!keyLength||!certLength||size!=8+keyLength+certLength+32||!hash(data.get(),size-32,digest)||
         !lan_auth::equal(digest,data.get()+size-32,32)||
         mbedtls_pk_parse_key(&key,data.get()+8,keyLength,nullptr,0)||
         mbedtls_x509_crt_parse_der(&cert,data.get()+8+keyLength,certLength))return false;
    }else{
      if(mbedtls_pk_setup(&key,mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))||
         mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,mbedtls_pk_ec(key),randomBytes,nullptr))return false;
      mbedtls_x509write_cert writer;mbedtls_x509write_crt_init(&writer);
      mbedtls_mpi serial;mbedtls_mpi_init(&serial);
      uint8_t serialBytes[16];randomBytes(nullptr,serialBytes,sizeof(serialBytes));serialBytes[0]&=0x7f;serialBytes[0]|=1;
      bool ok=mbedtls_mpi_read_binary(&serial,serialBytes,sizeof(serialBytes))==0;
      mbedtls_x509write_crt_set_md_alg(&writer,MBEDTLS_MD_SHA256);
      mbedtls_x509write_crt_set_subject_key(&writer,&key);mbedtls_x509write_crt_set_issuer_key(&writer,&key);
      mbedtls_x509write_crt_set_version(&writer,MBEDTLS_X509_CRT_VERSION_3);
      ok=ok&&mbedtls_x509write_crt_set_serial(&writer,&serial)==0&&
        mbedtls_x509write_crt_set_subject_name(&writer,"CN=PaperDIY") ==0&&
        mbedtls_x509write_crt_set_issuer_name(&writer,"CN=PaperDIY")==0&&
        mbedtls_x509write_crt_set_validity(&writer,"20200101000000","21200101000000")==0&&
        mbedtls_x509write_crt_set_basic_constraints(&writer,0,-1)==0&&
        mbedtls_x509write_crt_set_key_usage(&writer,MBEDTLS_X509_KU_DIGITAL_SIGNATURE)==0;
      SensitiveBuffer der(new(std::nothrow) uint8_t[IDENTITY_LIMIT]);
      int certLength=ok&&der?mbedtls_x509write_crt_der(&writer,der.get(),IDENTITY_LIMIT,randomBytes,nullptr):-1;
      mbedtls_x509write_crt_free(&writer);mbedtls_mpi_free(&serial);
      if(certLength<=0||mbedtls_x509_crt_parse_der(&cert,der.get()+IDENTITY_LIMIT-certLength,size_t(certLength)))return false;
      const int keyLength=mbedtls_pk_write_key_der(&key,der.get(),IDENTITY_LIMIT);
      if(keyLength<=0||8+size_t(keyLength)+size_t(certLength)+32>IDENTITY_LIMIT)return false;
      size=8+size_t(keyLength)+size_t(certLength)+32;
      std::memcpy(data.get(),"LTI1",4);data[4]=uint8_t(keyLength);data[5]=uint8_t(keyLength>>8);
      data[6]=uint8_t(certLength);data[7]=uint8_t(certLength>>8);
      std::memcpy(data.get()+8,der.get()+IDENTITY_LIMIT-keyLength,size_t(keyLength));
      std::memcpy(data.get()+8+keyLength,cert.raw.p,size_t(certLength));
      if(!hash(data.get(),size-32,data.get()+size-32)||preferences.putBytes(name,data.get(),size)!=size||
         preferences.getBytesLength(name)!=size||preferences.getBytes(name,der.get(),size)!=size||
         !lan_auth::equal(data.get(),der.get(),size))return false;
      lan_auth::erase(der.get(),IDENTITY_LIMIT);
    }
    lan_auth::erase(data.get(),IDENTITY_LIMIT);
    if(mbedtls_pk_check_pair(&cert.pk,&key))return false;
    uint8_t digest[32];if(!hash(cert.raw.p,cert.raw.len,digest))return false;
    fingerprint=lan_auth::encode(digest,32);id=fingerprint.substr(0,16);return true;
  }
};
class Server {
 public:
  Identity identity;
  WiFiServer listener{PORT,1};WiFiClient client;
  mbedtls_ssl_config configuration;mbedtls_ssl_context tls;
  Handler handler=nullptr;
  std::unique_ptr<BoundedHttpRequest> request;
  std::string response;size_t sent=0;
  uint32_t started=0,activity=0;bool serving=false,handshake=false,writing=false,draining=false;
  Server(){mbedtls_ssl_config_init(&configuration);mbedtls_ssl_init(&tls);}
  ~Server(){close();listener.end();mbedtls_ssl_config_free(&configuration);}
  bool begin(lan_nvs::Store& preferences,Handler callback){
    if(!callback||!identity.load(preferences)||
       mbedtls_ssl_config_defaults(&configuration,MBEDTLS_SSL_IS_SERVER,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT))return false;
    mbedtls_ssl_conf_rng(&configuration,randomBytes,nullptr);
    mbedtls_ssl_conf_min_version(&configuration,MBEDTLS_SSL_MAJOR_VERSION_3,MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_max_version(&configuration,MBEDTLS_SSL_MAJOR_VERSION_3,MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_authmode(&configuration,MBEDTLS_SSL_VERIFY_NONE);
    static const int suites[]={MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,0};
    mbedtls_ssl_conf_ciphersuites(&configuration,suites);
    static const mbedtls_ecp_group_id curves[]={MBEDTLS_ECP_DP_SECP256R1,MBEDTLS_ECP_DP_NONE};
    mbedtls_ssl_conf_curves(&configuration,curves);
    mbedtls_ssl_conf_renegotiation(&configuration,MBEDTLS_SSL_RENEGOTIATION_DISABLED);
    if(mbedtls_ssl_conf_own_cert(&configuration,&identity.cert,&identity.key))return false;
    handler=callback;listener.begin();listener.setNoDelay(true);return bool(listener);
  }
  void close(){
    client.stop();mbedtls_ssl_free(&tls);mbedtls_ssl_init(&tls);
    request.reset();response.clear();sent=0;serving=handshake=writing=false;
  }
  static int sendBytes(void* context,const unsigned char* data,size_t length){
    auto& self=*static_cast<Server*>(context);const int socket=self.client.fd();if(socket<0)return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    const int n=::send(socket,data,length,MSG_DONTWAIT);
    if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR))return MBEDTLS_ERR_SSL_WANT_WRITE;
    if(n<=0)return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    self.activity=millis();return n;
  }
  static int receiveBytes(void* context,unsigned char* data,size_t length){
    auto& self=*static_cast<Server*>(context);const int socket=self.client.fd();if(socket<0)return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    const int n=::recv(socket,data,length,MSG_DONTWAIT);
    if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR))return MBEDTLS_ERR_SSL_WANT_READ;
    if(n<0)return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    if(n)self.activity=millis();return n;
  }
  void reply(Response value){
    if(value.json.size()>RESPONSE_LIMIT)value={500,"{\"code\":\"response_limit\"}"};
    response="HTTP/1.1 "+std::to_string(value.code)+" Response\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\nContent-Length: "+std::to_string(value.json.size())+"\r\n\r\n"+value.json;
    writing=true;sent=0;
  }
  void poll(){
    if(draining&&!serving)return;
    if(!serving){
      client=listener.available();if(!client)return;
      client.setNoDelay(true);started=activity=millis();
      request.reset(new(std::nothrow) BoundedHttpRequest);
      if(!request||mbedtls_ssl_setup(&tls,&configuration)){close();return;}
      mbedtls_ssl_set_bio(&tls,this,sendBytes,receiveBytes,nullptr);serving=true;
    }
    const auto now=millis();
    if(now-started>=REQUEST_MS||now-activity>=IDLE_MS||(!handshake&&now-started>=HANDSHAKE_MS)){close();return;}
    if(!handshake){
      // ECDHE itself is finite CPU work. Never call a blocking socket or the
      // looping handshake API; input/timeout/shutdown get another main turn.
      const int result=mbedtls_ssl_handshake_step(&tls);
      if(result&&result!=MBEDTLS_ERR_SSL_WANT_READ&&result!=MBEDTLS_ERR_SSL_WANT_WRITE){close();return;}
      handshake=tls.state==MBEDTLS_SSL_HANDSHAKE_OVER;return;
    }
    if(writing){
      const int n=mbedtls_ssl_write(&tls,reinterpret_cast<const uint8_t*>(response.data()+sent),response.size()-sent);
      if(n>0){sent+=size_t(n);if(sent==response.size())close();}
      else if(n!=MBEDTLS_ERR_SSL_WANT_READ&&n!=MBEDTLS_ERR_SSL_WANT_WRITE)close();
      return;
    }
    uint8_t bytes[2048];const int n=mbedtls_ssl_read(&tls,bytes,sizeof(bytes));
    if(n<=0){if(n!=MBEDTLS_ERR_SSL_WANT_READ&&n!=MBEDTLS_ERR_SSL_WANT_WRITE)close();return;}
    for(int i=0;i<n;++i){
      const auto state=request->feed(char(bytes[i]));
      if(state==BoundedHttpRequest::State::Failed){reply({request->error,"{\"code\":\"invalid_request\"}"});return;}
      if(state==BoundedHttpRequest::State::Ready){
        // No pipelining: trailing bytes cannot be smuggled into a second call.
        if(i+1!=n){reply({400,"{\"code\":\"unexpected_bytes\"}"});return;}
        reply(handler(*request));return;
      }
    }
  }
};
std::unique_ptr<Server> server;
}
bool begin(lan_nvs::Store& preferences,Handler handler){
  stop();std::unique_ptr<Server> next(new(std::nothrow) Server);
  if(!next||!next->begin(preferences,handler))return false;server=std::move(next);return true;
}
void stop(){server.reset();}
void stopAfterResponse(){if(server){server->draining=true;server->listener.end();if(!server->writing)server->close();}}
void poll(){if(server){server->poll();if(server&&server->draining&&!server->serving)server.reset();}}
bool active(){return bool(server);}
const char* fingerprint(){return server?server->identity.fingerprint.c_str():"";}
const char* deviceId(){return server?server->identity.id.c_str():"";}
}
#else
namespace lan_tls {
bool begin(lan_nvs::Store&,Handler){return false;}
void stop(){}
void stopAfterResponse(){}
void poll(){}
bool active(){return false;}
const char* fingerprint(){return "";}
const char* deviceId(){return "";}
}
#endif
