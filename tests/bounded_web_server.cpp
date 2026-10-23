#include <cassert>
#include <iostream>
#include <string>
#include "bounded_web_server.h"

class SocketServer : public BoundedWebServer {
 public:
  bool writable = true;
  size_t writeLimit = POLL_BYTES, largestWrite = 0;
 protected:
  int writeAvailable(const char* bytes, size_t size) override {
    largestWrite = std::max(largestWrite, size);
    if (!writable) return 0;
    return int(_currentClient.write(bytes, std::min(size, writeLimit)));
  }
};

namespace {
const std::string GET = "GET / HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n";
std::string post(const std::string& body) {
  return "POST /chunk HTTP/1.1\r\nHost: 192.168.4.1\r\n"
    "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: " +
    std::to_string(body.size()) + "\r\n\r\n" + body;
}
void pollClosed(SocketServer& server, const std::shared_ptr<MockSocket>& client) {
  for (unsigned i = 0; i < 100 && client->open; ++i) {
    const size_t before = client->reads;
    server.handleClient();
    assert(client->reads - before <= BoundedWebServer::POLL_BYTES);
  }
  assert(!client->open);
  assert(server.largestWrite <= BoundedWebServer::POLL_BYTES);
}
void routingAndPartialDelivery() {
  SocketServer server;
  const char* names[] = {"Cookie"};
  server.collectHeaders(names, 1);
  unsigned calls = 0;
  String seenToken, seenCookie;
  server.on("/", HTTP_GET, [&] {
    ++calls;
    seenToken = server.arg("t");
    seenCookie = server.header("Cookie");
    server.send(200, "application/json", "{\"ok\":true}");
  });
  auto first = server.queueClient("GET /?t=secret HTTP/1.1\r\nHost: x\r\nCookie: diy_token=abc\r\n");
  server.handleClient();
  assert(calls == 0 && first->open && first->output.empty());
  server.handleClient();  // no bytes available: returns without waiting
  assert(calls == 0 && first->open);
  first->appendInput("\r\n");
  pollClosed(server, first);
  assert(calls == 1 && seenToken == "secret" && seenCookie == "diy_token=abc");
  assert(first->output.find("200 ") != std::string::npos);
  auto second = server.queueClient(GET);
  pollClosed(server, second);
  assert(calls == 2 && seenToken.length() == 0 && seenCookie.length() == 0);

  unsigned chunks = 0;
  size_t seenBytes = 0;
  server.on("/chunk", HTTP_POST, [&] {
    ++chunks;
    seenBytes = server.arg("data").length();
    server.send(200, "application/json", "{}");
  });
  auto chunk = server.queueClient(post("data=" + std::string(16384, 'A')));
  server.handleClient();
  assert(chunks == 0 && chunk->reads == BoundedWebServer::POLL_BYTES);
  pollClosed(server, chunk);
  assert(chunks == 1 && seenBytes == 16384);
  auto pipelined = server.queueClient(GET + GET);
  pollClosed(server, pipelined);
  assert(calls == 3 && pipelined->reads == GET.size());
}
void rejectAndRecover() {
  SocketServer server;
  unsigned calls = 0;
  server.on("/chunk", HTTP_POST, [&] { ++calls; server.send(200, "text/plain", "ok"); });
  server.on("/", HTTP_GET, [&] { ++calls; server.send(200, "text/plain", "ok"); });
  auto multipart = server.queueClient("POST /chunk HTTP/1.1\r\nHost: x\r\n"
    "Content-Type: multipart/form-data; boundary=x\r\nContent-Length: 100\r\n\r\n");
  pollClosed(server, multipart);
  assert(calls == 0 && server.response.code == 415);
  auto bad = server.queueClient(post("x=%00"));
  pollClosed(server, bad);
  assert(calls == 0 && server.response.code == 400);
  auto valid = server.queueClient(GET);
  pollClosed(server, valid);
  assert(calls == 1);
}
void incompleteRequestDeadlines() {
  for (uint32_t start : {uint32_t(100), uint32_t(UINT32_MAX - 100)}) {
    testClock = start;
    SocketServer server;
    unsigned calls = 0;
    server.on("/chunk", HTTP_POST, [&] { ++calls; server.send(200, "text/plain", "ok"); });
    auto stalled = server.queueClient(post("data=abcd").substr(0, post("data=abcd").size() - 1));
    server.handleClient();
    assert(calls == 0 && stalled->open);
    testClock += BoundedWebServer::REQUEST_TIMEOUT_MS - 1;
    server.handleClient();
    assert(stalled->open && calls == 0);
    ++testClock;
    server.handleClient();
    assert(!stalled->open && calls == 0 && server.response.code == 408);
    // The absolute deadline also covers a connection sending no headers.
    auto empty = server.queueClient("");
    server.handleClient();
    testClock += BoundedWebServer::REQUEST_TIMEOUT_MS;
    server.handleClient();
    assert(!empty->open && server.response.code == 408);
  }
  SocketServer server;
  auto stopped = server.queueClient("GET /");
  server.handleClient();
  server.stop();
  assert(!stopped->open);
  auto disconnected = server.queueClient("GET /", false);
  server.handleClient();
  assert(disconnected->output.empty());
}
void boundedResponses() {
  SocketServer server;
  server.on("/", HTTP_GET, [&] { server.send(200, "text/plain", String(std::string(8000, 'A'))); });
  auto large = server.queueClient(GET);
  server.handleClient();
  assert(large->open && large->output.size() == BoundedWebServer::POLL_BYTES);
  pollClosed(server, large);
  assert(large->output.find(std::string(8000, 'A')) != std::string::npos);
  // Small partial writes resume from the exact offset without duplication.
  server.writeLimit = 103;
  auto shortWrites = server.queueClient(GET);
  pollClosed(server, shortWrites);
  assert(shortWrites->output == large->output);

  server.writable = false;
  auto blocked = server.queueClient(GET);
  server.handleClient();
  assert(blocked->open && blocked->output.empty());
  testClock += BoundedWebServer::REQUEST_TIMEOUT_MS - 1;
  server.handleClient();
  assert(blocked->open);
  ++testClock;
  server.handleClient();
  assert(!blocked->open);

  server.writable = true;
  server.writeLimit = BoundedWebServer::POLL_BYTES;
  server.on("/oversize", HTTP_GET, [&] {
    server.send(200, "text/plain", String(std::string(BoundedWebServer::MAX_RESPONSE, 'A')));
  });
  auto oversize = server.queueClient("GET /oversize HTTP/1.1\r\nHost: x\r\n\r\n");
  pollClosed(server, oversize);
  assert(server.response.code == 503);
  assert(oversize->output.find("503 ") != std::string::npos);
  assert(oversize->output.find(std::string(100, 'A')) == std::string::npos);
}
}

int main() {
  routingAndPartialDelivery();
  rejectAndRecover();
  incompleteRequestDeadlines();
  boundedResponses();
  std::cout << "bounded HTTP transport: routing, framing, per-poll budget, timeout, overflow, cleanup passed\n";
}
