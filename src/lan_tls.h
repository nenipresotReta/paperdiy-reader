#pragma once
#include <cstdint>
#include <string>
#include "bounded_http_request.h"
#include "lan_nvs.h"
namespace lan_tls {
constexpr uint16_t PORT=8443;
struct Response {
  int code;std::string json;
  Response(int status=200,std::string body="{}"):code(status),json(std::move(body)){}
};
using Handler=Response(*)(const BoundedHttpRequest&);
// Device-only TLS listener. Each poll does finite I/O/handshake steps; all
// handlers and storage run on the application owner, never an Async callback.
bool begin(lan_nvs::Store& store,Handler handler);
void stop();
// Application task AFTER poll returns: stop accepting immediately and drain the
// current queued response within its existing deadlines, then destroy server.
// Never call from inside a Handler.
void stopAfterResponse();
void poll();
bool active();
const char* fingerprint();
const char* deviceId();
}
