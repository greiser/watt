// WsClient tests against the in-process watt_httpserver's /ws echo endpoint (ephemeral port).
#include <catch2/catch_test_macros.hpp>

#include <winsock2.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "../tools/httpserver/http_server.h"
#include "core/model.h"
#include "core/ws_client.h"

using namespace watt;

namespace {

httpserver::Server& server() {
  static httpserver::Server s;
  static const bool started = s.start(0);
  REQUIRE(started);
  return s;
}

std::string ws_url(const std::string& path) { return "ws://127.0.0.1:" + std::to_string(server().port()) + path; }

struct Run {
  WsHandshake hs;
  WsClose close;
  std::vector<WsMessage> msgs;
  int batches = 0;
};

// Connects, runs `script` on another thread once open, collects everything.
template <class F>
Run run_ws(const std::string& url, F&& script, std::vector<Header> headers = {}) {
  Run r;
  WsClient c;
  HttpRequest req;
  req.url = url;
  req.headers = std::move(headers);
  r.hs = c.connect(req);
  if (!r.hs.ok) return r;
  std::thread t([&] { script(c); });
  r.close = c.run([&](std::vector<WsMessage>&& b) {
    ++r.batches;
    for (auto& m : b) r.msgs.push_back(std::move(m));
  });
  t.join();
  return r;
}

}  // namespace

TEST_CASE("ws helpers") {
  // RFC 6455 section 1.3 example.
  CHECK(ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
  CHECK(ws_http_url("ws://h/x") == "http://h/x");
  CHECK(ws_http_url("WSS://h/x") == "https://h/x");
  CHECK(ws_http_url("https://h") == "https://h");
  CHECK(ws_http_url("file:///c:/x").empty());
  CHECK(ws_http_url("ftp://h").empty());
  CHECK(ws_display_url("https://h/a") == "wss://h/a");
  CHECK(std::string(ws_close_code_name(1000)) == "Normal closure");
  CHECK(request_kind_from(request_kind_name(RequestKind::WebSocket)) == RequestKind::WebSocket);
}

TEST_CASE("ws frame encode / parse round trip") {
  const unsigned char mask[4] = {1, 2, 3, 4};
  for (size_t n : {size_t{0}, size_t{5}, size_t{125}, size_t{126}, size_t{65535}, size_t{65536}, size_t{200000}}) {
    std::string payload(n, 'x');
    for (size_t i = 0; i < n; ++i) payload[i] = static_cast<char>('a' + i % 26);
    for (const unsigned char* m : {static_cast<const unsigned char*>(nullptr), mask}) {
      const std::string f = ws_encode_frame(1, payload, true, m);
      WsFrameParser p;
      WsFrame fr;
      std::string err;
      // Byte by byte for small ones: incomplete input never yields a frame.
      if (n < 200) {
        for (size_t i = 0; i + 1 < f.size(); ++i) {
          p.feed(f.data() + i, 1);
          REQUIRE(p.next(&fr, &err) == 0);
        }
        p.feed(f.data() + f.size() - 1, 1);
      } else {
        p.feed(f.data(), f.size());
      }
      REQUIRE(p.next(&fr, &err) == 1);
      CHECK(fr.opcode == 1);
      CHECK(fr.fin);
      CHECK(fr.payload == payload);
      CHECK(p.buffered() == 0);
    }
  }
  WsFrameParser p;
  const std::string big = ws_encode_frame(2, std::string(2000, 'b'), true, nullptr);
  p.feed(big.data(), big.size());
  WsFrame fr;
  std::string err;
  CHECK(p.next(&fr, &err, 1000) == -1);
  CHECK(err == "message too big");
}

TEST_CASE("ws echo: text, binary, close by client") {
  Run r = run_ws(ws_url("/ws"), [](WsClient& c) {
    c.send("hello", false);
    c.send(std::string("\x00\x01\x02", 3), true);
    c.send(std::string(70000, 'z'), false);  // 64-bit length frame
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    c.close(1000, "done");
  });
  REQUIRE(r.hs.ok);
  CHECK(r.hs.status == 101);
  CHECK(r.hs.url.rfind("ws://127.0.0.1:", 0) == 0);
  CHECK(r.hs.request_head.find("Sec-WebSocket-Version: 13") != std::string::npos);
  REQUIRE(r.msgs.size() == 6);
  CHECK(r.msgs[0].out);
  CHECK(r.msgs[0].data == "hello");
  // Echoes come back in order.
  std::vector<std::string> in;
  for (const auto& m : r.msgs)
    if (!m.out) in.push_back(m.data);
  REQUIRE(in.size() == 3);
  CHECK(in[0] == "hello");
  CHECK(in[1] == std::string("\x00\x01\x02", 3));
  CHECK(in[2].size() == 70000);
  CHECK(r.close.code == 1000);
  CHECK(r.close.by_client);
  CHECK(r.close.error.empty());
  CHECK(r.close.messages_in == 3);
  CHECK(r.close.messages_out == 3);
}

TEST_CASE("ws server close code and reason") {
  Run r = run_ws(ws_url("/ws"), [](WsClient& c) { c.send("close 4001 custom bye", false); });
  REQUIRE(r.hs.ok);
  CHECK(r.close.code == 4001);
  CHECK(r.close.reason == "custom bye");
  CHECK_FALSE(r.close.by_client);
}

TEST_CASE("ws burst is delivered in few batches, in order") {
  Run r = run_ws(ws_url("/ws?burst=10000"), [](WsClient& c) {
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    c.close();
  });
  REQUIRE(r.hs.ok);
  REQUIRE(r.msgs.size() == 10000);
  CHECK(r.msgs[0].data == "{\"i\":0,\"msg\":\"burst\"}");
  CHECK(r.msgs[9999].data == "{\"i\":9999,\"msg\":\"burst\"}");
  for (size_t i = 1; i < r.msgs.size(); ++i) REQUIRE(r.msgs[i].at_ms >= r.msgs[i - 1].at_ms);
  // 50 ms batches: a burst that arrives within ~0.4 s never needs hundreds of UI posts.
  CHECK(r.batches <= 20);
}

TEST_CASE("ws headers and subprotocol go out with the upgrade") {
  Run r = run_ws(ws_url("/ws?protocol=chat"), [](WsClient& c) { c.close(); },
                 {{"Authorization", "Bearer abc"}, {"Sec-WebSocket-Protocol", "chat"}});
  REQUIRE(r.hs.ok);
  CHECK(r.hs.subprotocol == "chat");
  CHECK(r.hs.request_head.find("Authorization: Bearer abc\r\n") != std::string::npos);
}

TEST_CASE("ws failures are reported, not hung") {
  WsClient c;
  HttpRequest req;
  req.url = "http://127.0.0.1:" + std::to_string(server().port()) + "/echo";  // no upgrade there
  WsHandshake h = c.connect(req);
  CHECK_FALSE(h.ok);
  CHECK(h.status == 200);
  CHECK(h.error.find("instead of 101") != std::string::npos);

  WsClient c2;
  req.url = "file:///c:/windows/win.ini";
  h = c2.connect(req);
  CHECK_FALSE(h.ok);
  CHECK(h.error.find("ws://") != std::string::npos);

  WsClient c3;
  req.url = "ws://127.0.0.1:1/x";  // nothing listens
  req.timeout_ms = 3000;
  h = c3.connect(req);
  CHECK_FALSE(h.ok);

  WsClient c4;
  req.url = ws_url("/ws");
  req.headers = {{"X-Bad", "a\r\nInjected: 1"}};
  h = c4.connect(req);
  CHECK_FALSE(h.ok);
  CHECK(h.error.find("line break") != std::string::npos);
}

TEST_CASE("ws cancel ends run at once") {
  WsClient c;
  HttpRequest req;
  req.url = ws_url("/ws");
  REQUIRE(c.connect(req).ok);
  std::thread t([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    c.cancel();
  });
  const auto t0 = std::chrono::steady_clock::now();
  const WsClose cl = c.run({});
  t.join();
  CHECK(cl.error == "cancelled");
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1000));
  std::string err;
  CHECK_FALSE(c.send("x", false, &err));
}

TEST_CASE("ws request kind round-trips through a .req.json") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / ("watt_ws_test_" + std::to_string(GetCurrentProcessId()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  {
    Workspace ws = Workspace::create(dir);
    SavedRequest r = ws.new_request(RequestKind::WebSocket, "Chat", dir);
    r.http.url = "ws://{{host}}/ws";
    r.http.body = "{\"hi\":1}";
    r.http.headers = {{"X-A", "1"}};
    std::string err;
    REQUIRE(ws.save_request(r, &err));
    auto back = ws.load_request(r.file);
    REQUIRE(back);
    CHECK(back->kind == RequestKind::WebSocket);
    CHECK(back->http.url == "ws://{{host}}/ws");
    CHECK(back->http.body == "{\"hi\":1}");
    const auto tree = ws.tree(-1);
    bool found = false;
    std::function<void(const CollectionNode&)> walk = [&](const CollectionNode& n) {
      if (!n.is_folder && n.name == "Chat") {
        found = true;
        CHECK(n.kind == RequestKind::WebSocket);
        CHECK(n.method == "WS");
      }
      for (const auto& ch : n.children) walk(ch);
    };
    walk(tree);
    CHECK(found);
  }
  fs::remove_all(dir);
}
