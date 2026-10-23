#pragma once

#include <WebServer.h>
#include <algorithm>
#include <cerrno>
#include <new>
#ifdef ARDUINO_ARCH_ESP32
#include <lwip/sockets.h>
#else
#include <sys/socket.h>
#endif
#include "bounded_http_request.h"

// WebServer 2.0.17's multipart parser can wait forever on a connected sender.
// Reuse its routing/response API, but never enter that parser. Every poll reads
// a bounded number of already available bytes; incomplete requests expire.
class BoundedWebServer : public WebServer {
 public:
  static constexpr uint32_t REQUEST_TIMEOUT_MS = 5000;
  static constexpr size_t POLL_BYTES = 2048;
  static constexpr size_t MAX_RESPONSE = 32768;
  explicit BoundedWebServer(int port = 80) : WebServer(port) {}
  ~BoundedWebServer() override { clearClient(); }

  void close() override {
    clearClient();
    WebServer::close();
  }

  void handleClient() override {
    if (sending_) { flushResponse(); return; }
    if (!receiving_) {
      _currentClient = _server.available();
      if (!_currentClient) return;
      receiving_ = true;
      started_ = millis();
      request_ = BoundedHttpRequest{};
    }
    if (uint32_t(millis() - started_) >= REQUEST_TIMEOUT_MS) { reject(408); return; }
    for (size_t work = 0; work < POLL_BYTES && _currentClient.available(); ++work) {
      const int byte = _currentClient.read();
      if (byte < 0) break;
      const auto result = request_.feed(char(byte));
      if (result == BoundedHttpRequest::State::Failed) { reject(request_.error); return; }
      if (result == BoundedHttpRequest::State::Ready) { dispatch(); return; }
    }
    if (!_currentClient.connected() && !_currentClient.available()) clearClient();
  }

 protected:
  // Unlike WiFiClient::write, this performs no retry loop. A full TCP send
  // buffer is retried by a later poll, within the response deadline.
  virtual int writeAvailable(const char* bytes, size_t size) {
    const int written = ::send(_currentClient.fd(), bytes, size, MSG_DONTWAIT);
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return written;
  }
  size_t _currentClientWrite(const char* bytes, size_t size) override {
    if (overflow_ || size > MAX_RESPONSE - response_.size()) {
      overflow_ = true;
      return 0;
    }
    response_.append(bytes, size);
    return size;
  }
  size_t _currentClientWrite_P(const char* bytes, size_t size) override {
    // ESP32 flash is memory mapped; PROGMEM strings are directly readable.
    return _currentClientWrite(bytes, size);
  }

 private:
  BoundedHttpRequest request_;
  bool receiving_ = false, sending_ = false, overflow_ = false;
  uint32_t started_ = 0;
  std::string response_;
  size_t sent_ = 0;

  void resetResponse() {
    _currentVersion = request_.http11 ? 1 : 0;
    _contentLength = CONTENT_LENGTH_NOT_SET;
    _responseHeaders = "";
    _chunked = false;
    response_.clear();
    sent_ = 0;
    overflow_ = false;
  }
  void clearRequest() {
    if (_currentArgs) { delete[] _currentArgs; _currentArgs = nullptr; }
    _currentArgCount = 0;
    _currentHandler = nullptr;
    _currentUri = "";
    _hostHeader = "";
    _responseHeaders = "";
    for (int i = 0; i < _headerKeysCount; ++i) _currentHeaders[i].value = "";
    request_ = BoundedHttpRequest{};
    receiving_ = false;
  }
  void clearClient() {
    _currentClient.stop();
    _currentClient = WiFiClient();
    clearRequest();
    std::string{}.swap(response_);
    sent_ = 0;
    sending_ = false;
  }
  void queueResponse() {
    if (overflow_) {
      resetResponse();
      send(503, "application/json", "{\"error\":\"Response too large\"}");
    }
    clearRequest();
    sending_ = true;
    started_ = millis();
    flushResponse();
  }
  void flushResponse() {
    if (uint32_t(millis() - started_) >= REQUEST_TIMEOUT_MS || !_currentClient.connected()) {
      clearClient();
      return;
    }
    const size_t remaining = response_.size() - sent_;
    if (!remaining) { clearClient(); return; }
    const size_t budget = remaining < POLL_BYTES ? remaining : POLL_BYTES;
    const int written = writeAvailable(response_.data() + sent_, budget);
    if (written < 0) { clearClient(); return; }
    sent_ += size_t(written);
    if (sent_ == response_.size()) clearClient();
  }
  void reject(int code) {
    resetResponse();
    sendHeader("Cache-Control", "no-store");
    send(code, "application/json", "{\"error\":\"Invalid or incomplete request\"}");
    queueResponse();
  }
  void dispatch() {
    resetResponse();
    _currentMethod = request_.post ? HTTP_POST : HTTP_GET;
    _currentUri = request_.path.c_str();
    _clientContentLength = int(request_.contentLength);
    for (int i = 0; i < _headerKeysCount; ++i) _currentHeaders[i].value = "";
    for (const auto& field : request_.headers) {
      _collectHeader(field.first.c_str(), field.second.c_str());
      if (field.first == "host") _hostHeader = field.second.c_str();
    }
    _currentArgs = new (std::nothrow) RequestArgument[request_.arguments.size() + 1];
    if (!_currentArgs) { reject(503); return; }
    _currentArgCount = int(request_.arguments.size());
    for (int i = 0; i < _currentArgCount; ++i) {
      _currentArgs[i].key = request_.arguments[size_t(i)].first.c_str();
      _currentArgs[i].value = request_.arguments[size_t(i)].second.c_str();
    }
    for (_currentHandler = _firstHandler; _currentHandler; _currentHandler = _currentHandler->next())
      if (_currentHandler->canHandle(_currentMethod, _currentUri)) break;
    _handleRequest();
    queueResponse();
  }
};
