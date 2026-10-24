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
  static constexpr size_t MAX_FLASH_RESPONSE = 1024u * 1024u;
  static constexpr uint32_t FLASH_RESPONSE_TIMEOUT_MS = 45000;
  explicit BoundedWebServer(int port = 80) : WebServer(port) {}
  ~BoundedWebServer() override { clearClient(); }

  // The caller must provide immutable storage that outlives every request
  // (a compiled PROGMEM asset). Only the small HTTP header is buffered in RAM.
  void sendFlash(int code, const char* type, const uint8_t* bytes, size_t size) {
    if (!bytes || !size || size > MAX_FLASH_RESPONSE) {
      send(503, "application/json", "{\"error\":\"Asset unavailable\"}");
      return;
    }
    setContentLength(size);
    send(code, type, "");
    flash_ = reinterpret_cast<const char*>(bytes);
    flashBytes_ = size;
  }

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
  const char* flash_ = nullptr;
  size_t flashBytes_ = 0;
  uint32_t lastWrite_ = 0;

  void resetResponse() {
    _currentVersion = request_.http11 ? 1 : 0;
    _contentLength = CONTENT_LENGTH_NOT_SET;
    _responseHeaders = "";
    _chunked = false;
    response_.clear();
    sent_ = 0;
    overflow_ = false;
    flash_ = nullptr;
    flashBytes_ = 0;
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
    flash_ = nullptr;
    flashBytes_ = 0;
  }
  void queueResponse() {
    if (overflow_) {
      resetResponse();
      send(503, "application/json", "{\"error\":\"Response too large\"}");
    }
    clearRequest();
    sending_ = true;
    started_ = millis();
    lastWrite_ = started_;
    flushResponse();
  }
  void flushResponse() {
    const uint32_t now = millis();
    const uint32_t limit = flash_ ? FLASH_RESPONSE_TIMEOUT_MS : REQUEST_TIMEOUT_MS;
    if (uint32_t(now - started_) >= limit ||
        uint32_t(now - lastWrite_) >= REQUEST_TIMEOUT_MS || !_currentClient.connected()) {
      clearClient();
      return;
    }
    const size_t total = response_.size() + flashBytes_;
    const bool inHeader = sent_ < response_.size();
    const size_t remaining = inHeader ? response_.size() - sent_ : total - sent_;
    if (!remaining) { clearClient(); return; }
    const size_t budget = remaining < POLL_BYTES ? remaining : POLL_BYTES;
    const char* bytes = inHeader ? response_.data() + sent_ : flash_ + sent_ - response_.size();
    const int written = writeAvailable(bytes, budget);
    if (written < 0 || size_t(written) > budget) { clearClient(); return; }
    if (written) lastWrite_ = now;
    sent_ += size_t(written);
    if (sent_ == total) clearClient();
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
