// HttpClient tests against an in-process watt_httpserver on an ephemeral port.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../tools/httpserver/http_server.h"
#include "core/http_client.h"

#include <curl/curl.h>

using namespace watt;

namespace {

httpserver::Server& server() {
  static httpserver::Server s;
  static const bool started = s.start(0);
  REQUIRE(started);
  return s;
}

std::string url(const std::string& path) {
  return "http://127.0.0.1:" + std::to_string(server().port()) + path;
}

std::optional<std::string> find_header(const HttpResponse& r, const std::string& name) {
  std::string_view v;
  if (!r.headers.find(name, &v)) return std::nullopt;
  return std::string(v);
}

HttpRequest get(const std::string& path) {
  HttpRequest r;
  r.url = url(path);
  return r;
}

double phase_sum(const HttpResponse& r) { return phase_total(r); }

}  // namespace

TEST_CASE("status codes", "[http]") {
  HttpClient c;
  for (long code : {200L, 201L, 204L, 400L, 404L, 418L, 500L, 503L}) {
    auto r = c.send(get("/status/" + std::to_string(code)));
    INFO("code " << code << " error " << r.error);
    REQUIRE(r.ok);
    CHECK(r.status == code);
    CHECK(r.http_version == "HTTP/1.1");
    if (code == 204) CHECK(r.body.empty());
  }
  auto r = c.send(get("/status/418"));
  CHECK(r.status_text == "I'm a teapot");
  CHECK(c.send(get("/nope")).status == 404);
}

TEST_CASE("response headers are parsed in order", "[http]") {
  HttpClient c;
  auto r = c.send(get("/response-headers?X-First=1&X-Second=two%20words&X-Dup=a&X-Dup=b"));
  REQUIRE(r.ok);
  REQUIRE(r.status == 200);
  int first = -1, second = -1;
  int dups = 0;
  for (size_t i = 0; i < r.headers.size(); ++i) {
    if (r.headers[i].first == "X-First") first = static_cast<int>(i);
    if (r.headers[i].first == "X-Second") {
      second = static_cast<int>(i);
      CHECK(r.headers[i].second == "two words");
    }
    if (r.headers[i].first == "X-Dup") ++dups;
  }
  CHECK(first >= 0);
  CHECK(second > first);
  CHECK(dups == 2);
  REQUIRE(find_header(r, "content-length"));
  CHECK(*find_header(r, "content-length") == "2");
  CHECK(r.body == "{}");
  CHECK(r.remote_ip == "127.0.0.1");
  CHECK(r.remote_port == server().port());
}

TEST_CASE("request headers and body round-trip", "[http]") {
  HttpClient c;
  HttpRequest req = get("/echo?x=1");
  req.method = "POST";
  req.headers = {{"Content-Type", "application/json"}, {"X-Trace", "abc 123"}, {"X-Empty", ""}};
  req.body = "{\"hello\":\"world\",\"n\":[1,2,3]}";
  auto r = c.send(req);
  REQUIRE(r.ok);
  REQUIRE(r.status == 200);
  CHECK(r.body.find("\"method\":\"POST\"") != std::string::npos);
  CHECK(r.body.find("\"query\":\"x=1\"") != std::string::npos);
  CHECK(r.body.find("\"X-Trace\":\"abc 123\"") != std::string::npos);
  CHECK(r.body.find("\"X-Empty\":\"\"") != std::string::npos);
  CHECK(r.body.find("\"Content-Type\":\"application/json\"") != std::string::npos);
  CHECK(r.body.find("\"body\":\"{\\\"hello\\\":\\\"world\\\",\\\"n\\\":[1,2,3]}\"") !=
        std::string::npos);
  CHECK(r.body.find("Expect") == std::string::npos);

  for (const char* m : {"PUT", "PATCH", "DELETE", "OPTIONS"}) {
    HttpRequest q = get("/echo");
    q.method = m;
    q.body = std::string("payload-") + m;
    auto e = c.send(q);
    REQUIRE(e.ok);
    CHECK(e.body.find(std::string("\"method\":\"") + m + "\"") != std::string::npos);
    CHECK(e.body.find(std::string("\"body\":\"payload-") + m + "\"") != std::string::npos);
  }

  HttpRequest head = get("/bytes/1000");
  head.method = "HEAD";
  auto h = c.send(head);
  REQUIRE(h.ok);
  CHECK(h.status == 200);
  CHECK(h.body.empty());
}

TEST_CASE("large bodies arrive intact", "[http]") {
  HttpClient c;
  const size_t n = 5 * 1024 * 1024 + 7;
  auto r = c.send(get("/bytes/" + std::to_string(n)));
  REQUIRE(r.ok);
  REQUIRE(r.body.size() == n);
  bool pattern_ok = true;
  for (size_t i = 0; i < n && pattern_ok; i += 997)
    pattern_ok = r.body[i] == static_cast<char>('a' + i % 26);
  CHECK(pattern_ok);
  CHECK(r.timing.bytes_received >= n);

  size_t streamed = 0;
  auto s = c.send(get("/json/1000"), [&](const char*, size_t len) { streamed += len; });
  REQUIRE(s.ok);
  CHECK(streamed == s.body.size());
  CHECK(s.body.front() == '[');
  CHECK(s.body.back() == ']');
  CHECK(s.body.find("\"id\":999") != std::string::npos);

  std::string big(2 * 1024 * 1024, 'z');
  HttpRequest up = get("/echo");
  up.method = "POST";
  up.body = big;
  auto e = c.send(up);
  REQUIRE(e.ok);
  CHECK(e.body.find(big) != std::string::npos);
  // The body is counted once (curl's request size already includes a POSTFIELDS body).
  CHECK(e.sizes.request_body == big.size());
  CHECK(e.timing.bytes_sent == e.sizes.request_headers + big.size());
  CHECK(e.sizes.request_headers > 20);
  CHECK(e.sizes.request_headers < 1024);
}

TEST_CASE("redirects", "[http]") {
  HttpClient c;
  auto r = c.send(get("/redirect/3"));
  REQUIRE(r.ok);
  CHECK(r.status == 200);
  CHECK(r.effective_url == url("/echo"));
  CHECK(r.body.find("\"path\":\"/echo\"") != std::string::npos);
  // Only the final response's headers.
  CHECK_FALSE(find_header(r, "location"));
  CHECK(r.timing.total_ms == Catch::Approx(phase_sum(r)).margin(0.01));
  CHECK(r.redirect_count == 3);
  CHECK(r.redirect_ms > 0);

  HttpRequest no = get("/redirect/2");
  no.follow_redirects = false;
  auto n = c.send(no);
  REQUIRE(n.ok);
  CHECK(n.status == 302);
  REQUIRE(find_header(n, "location"));
  CHECK(*find_header(n, "location") == "/redirect/1");

  HttpRequest post = get("/status/307?location=/echo");
  post.method = "POST";
  post.body = "kept";
  auto p = c.send(post);
  REQUIRE(p.ok);
  CHECK(p.status == 200);
  CHECK(p.body.find("\"method\":\"POST\"") != std::string::npos);
  CHECK(p.body.find("\"body\":\"kept\"") != std::string::npos);

  HttpRequest see = get("/status/303?location=/echo");
  see.method = "PUT";
  see.body = "x";
  auto s = c.send(see);
  REQUIRE(s.ok);
  CHECK(s.body.find("\"method\":\"GET\"") != std::string::npos);
}

TEST_CASE("the method must be an HTTP token", "[http][security]") {
  HttpClient c;
  for (const char* bad : {"GET / HTTP/1.1\r\nHost: x\r\n\r\nGET", "GET\r\nX-Injected: 1", "GET; rm -rf ~", "PO ST", "", "GET\n"}) {
    HttpRequest r = get("/echo");
    r.method = bad;
    auto resp = c.send(r);
    INFO("method: " << bad);
    CHECK_FALSE(resp.ok);
    CHECK(resp.error.find("invalid HTTP method") != std::string::npos);
  }
  CHECK(is_http_token("PROPFIND"));
  CHECK(is_http_token("M-SEARCH"));
  HttpRequest custom = get("/echo");
  custom.method = "PURGE";  // custom tokens still work
  auto ok = c.send(custom);
  REQUIRE(ok.ok);
  CHECK(ok.body.find("\"method\":\"PURGE\"") != std::string::npos);
}

TEST_CASE("only http and https are reachable, directly or by redirect", "[http][security]") {
  HttpClient c;
  // win.ini exists on every Windows install; its content must never come back as a body.
  for (const char* u : {"file:///C:/Windows/win.ini", "FILE:///C:/Windows/win.ini",
                        "smb://127.0.0.1/c$/x", "ftp://127.0.0.1/x", "dict://127.0.0.1:1/x"}) {
    HttpRequest r;
    r.url = u;
    auto resp = c.send(r);
    INFO(u << " -> " << resp.error);
    CHECK_FALSE(resp.ok);
    CHECK(resp.body.find("[fonts]") == std::string::npos);
  }
  // A server-controlled Location must not pivot into file:// either.
  auto red = c.send(get("/status/302?location=file:///C:/Windows/win.ini"));
  INFO(red.error);
  CHECK_FALSE(red.ok);
  CHECK(red.body.find("[fonts]") == std::string::npos);
  // The client still works for http afterwards (options are persistent, not per-request).
  CHECK(c.send(get("/status/200")).status == 200);
}

TEST_CASE("timeouts", "[http]") {
  HttpClient c;
  HttpRequest req = get("/delay/2000");
  req.timeout_ms = 150;
  const auto t0 = Clock::now();
  auto r = c.send(req);
  const double took = ms_between(t0, Clock::now());
  CHECK_FALSE(r.ok);
  CHECK_FALSE(r.error.empty());
  CHECK(r.error != "cancelled");
  CHECK(took < 1000);
  CHECK(r.timing.total_ms >= 140);

  // The client recovers on the next request.
  auto ok = c.send(get("/status/200"));
  CHECK(ok.ok);
  CHECK(ok.status == 200);

  // Connection refused is a transport error, not a crash.
  HttpRequest dead;
  dead.url = "http://127.0.0.1:1/";
  dead.timeout_ms = 2000;
  auto d = c.send(dead);
  CHECK_FALSE(d.ok);
  CHECK(d.status == 0);
}

TEST_CASE("cancel", "[http]") {
  HttpClient c;
  SECTION("while waiting for the response") {
    std::thread t([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      c.cancel();
    });
    const auto t0 = Clock::now();
    auto r = c.send(get("/delay/5000"));
    const double took = ms_between(t0, Clock::now());
    t.join();
    CHECK_FALSE(r.ok);
    CHECK(r.error == "cancelled");
    CHECK(took < 1000);
  }
  SECTION("mid-body") {
    std::thread t([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      c.cancel();
    });
    const auto t0 = Clock::now();
    auto r = c.send(get("/drip/100?interval=50"));
    const double took = ms_between(t0, Clock::now());
    t.join();
    CHECK_FALSE(r.ok);
    CHECK(r.error == "cancelled");
    CHECK(took < 1500);
  }
  // A cancel does not poison the next send.
  auto ok = c.send(get("/status/200"));
  CHECK(ok.ok);
  CHECK(ok.status == 200);
}

TEST_CASE("connection reuse", "[http]") {
  HttpClient c;
  const uint64_t before = server().connections_accepted();
  auto first = c.send(get("/status/200"));
  REQUIRE(first.ok);
  CHECK_FALSE(first.timing.connection_reused);
  for (int i = 0; i < 20; ++i) {
    auto r = c.send(get("/json/10"));
    REQUIRE(r.ok);
    CHECK(r.timing.connection_reused);
    CHECK(r.timing.connect_ms == 0);
    CHECK(r.timing.dns_ms == 0);
  }
  CHECK(server().connections_accepted() - before == 1);

  HttpRequest fresh = get("/status/200");
  fresh.fresh_connection = true;
  auto f = c.send(fresh);
  REQUIRE(f.ok);
  CHECK_FALSE(f.timing.connection_reused);
  CHECK(server().connections_accepted() - before == 2);

  // Server closing the connection forces a reconnect, transparently (fresh client so the pool
  // holds exactly one connection).
  HttpClient c2;
  REQUIRE(c2.send(get("/status/200")).ok);
  auto cl = c2.send(get("/close"));
  REQUIRE(cl.ok);
  CHECK(cl.timing.connection_reused);
  const uint64_t conns = server().connections_accepted();
  auto after = c2.send(get("/status/200"));
  REQUIRE(after.ok);
  CHECK_FALSE(after.timing.connection_reused);
  CHECK(server().connections_accepted() == conns + 1);
}

TEST_CASE("timing phases add up to total", "[http][timing]") {
  HttpClient c;
  for (const char* path : {"/status/200", "/json/500", "/bytes/1048576", "/delay/50"}) {
    auto r = c.send(get(path));
    INFO(path);
    REQUIRE(r.ok);
    const Timing& t = r.timing;
    CHECK(t.total_ms > 0);
    CHECK(t.queue_ms >= 0);
    CHECK(t.dns_ms >= 0);
    CHECK(t.connect_ms >= 0);
    CHECK(t.tls_ms == 0);  // plaintext
    CHECK(t.send_ms >= 0);
    CHECK(t.ttfb_ms >= 0);
    CHECK(t.download_ms >= 0);
    CHECK(t.total_ms == Catch::Approx(phase_sum(r)).margin(0.01));
    CHECK(r.redirect_ms == 0);
    CHECK(r.process_ms >= 0);
    CHECK(r.process_ms < 50);
    CHECK(t.bytes_received > r.body.size());
    CHECK(t.bytes_sent > 0);
  }
  auto d = c.send(get("/delay/120"));
  REQUIRE(d.ok);
  CHECK(d.timing.ttfb_ms >= 100);  // server think time lands in TTFB
  CHECK(d.timing.total_ms >= 120);
}

TEST_CASE("http/1.1 forced and prior-knowledge failure is a clean error", "[http]") {
  HttpClient c;
  HttpRequest r = get("/status/200");
  r.version = HttpVersion::Http1_1;
  auto a = c.send(r);
  REQUIRE(a.ok);
  CHECK(a.http_version == "HTTP/1.1");

  r.version = HttpVersion::Http2PriorKnowledge;  // our server only speaks HTTP/1.1
  r.timeout_ms = 2000;
  auto b = c.send(r);
  CHECK_FALSE(b.ok);
  CHECK_FALSE(b.error.empty());
}

TEST_CASE("libcurl build has HTTP/2 and TLS", "[http]") {
  const curl_version_info_data* v = curl_version_info(CURLVERSION_NOW);
  REQUIRE(v);
  CHECK((v->features & CURL_VERSION_HTTP2) != 0);
  CHECK((v->features & CURL_VERSION_SSL) != 0);
  INFO("ssl backend " << (v->ssl_version ? v->ssl_version : "?"));
}

TEST_CASE("warm request latency", "[.bench]") {
  HttpClient c;
  REQUIRE(c.send(get("/status/200")).ok);
  const int n = 2000;
  Timing sum{};
  const auto t0 = Clock::now();
  for (int i = 0; i < n; ++i) {
    auto r = c.send(get("/json/10"));
    REQUIRE(r.ok);
    sum.total_ms += r.timing.total_ms;
    sum.queue_ms += r.timing.queue_ms;
    sum.ttfb_ms += r.timing.ttfb_ms;
    sum.download_ms += r.timing.download_ms;
  }
  const double wall = ms_between(t0, Clock::now());
  WARN("avg total " << sum.total_ms / n << " ms, queue " << sum.queue_ms / n << ", ttfb "
                    << sum.ttfb_ms / n << ", download " << sum.download_ms / n << ", wall/req "
                    << wall / n);
}

namespace {

// Minimal TCP listener on 127.0.0.1 that captures the first TLS record a client sends (the
// ClientHello) and then hangs up. Lets us check what libcurl offers without a TLS server.
struct HelloCatcher {
  SOCKET ls = INVALID_SOCKET;
  uint16_t port = 0;
  std::string hello;
  std::thread th;

  HelloCatcher() {
    ls = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    ::listen(ls, 4);
    int len = sizeof(a);
    ::getsockname(ls, reinterpret_cast<sockaddr*>(&a), &len);
    port = ntohs(a.sin_port);
    th = std::thread([this] {
      SOCKET c = ::accept(ls, nullptr, nullptr);
      if (c == INVALID_SOCKET) return;
      char buf[4096];
      for (;;) {
        int n = ::recv(c, buf, sizeof(buf), 0);
        if (n <= 0) break;
        hello.append(buf, static_cast<size_t>(n));
        if (hello.size() >= 5) {
          size_t rec = (static_cast<unsigned char>(hello[3]) << 8) |
                       static_cast<unsigned char>(hello[4]);
          if (hello.size() >= 5 + rec) break;
        }
      }
      ::closesocket(c);
    });
  }
  ~HelloCatcher() {
    ::closesocket(ls);
    if (th.joinable()) th.join();
  }
};

// ALPN protocol names from a ClientHello record ("" entries if none / unparsable).
std::vector<std::string> alpn_offers(const std::string& rec) {
  std::vector<std::string> out;
  auto u8 = [&](size_t i) { return static_cast<size_t>(static_cast<unsigned char>(rec[i])); };
  auto u16 = [&](size_t i) { return (u8(i) << 8) | u8(i + 1); };
  if (rec.size() < 9 || u8(0) != 0x16 || u8(5) != 0x01) return out;  // handshake / ClientHello
  size_t i = 9 + 2 + 32;                                               // version + random
  if (i >= rec.size()) return out;
  i += 1 + u8(i);                        // session id
  if (i + 2 > rec.size()) return out;
  i += 2 + u16(i);                       // cipher suites
  if (i >= rec.size()) return out;
  i += 1 + u8(i);                        // compression methods
  if (i + 2 > rec.size()) return out;
  const size_t ext_end = std::min(rec.size(), i + 2 + u16(i));
  i += 2;
  while (i + 4 <= ext_end) {
    const size_t type = u16(i), len = u16(i + 2);
    const size_t body = i + 4;
    if (type == 0x0010 && body + 2 <= ext_end) {
      size_t j = body + 2, end = std::min(ext_end, body + 2 + u16(body));
      while (j < end) {
        size_t l = u8(j);
        if (j + 1 + l > end) break;
        out.emplace_back(rec.substr(j + 1, l));
        j += 1 + l;
      }
    }
    i = body + len;
  }
  return out;
}

bool contains(const std::vector<std::string>& v, const char* s) {
  for (const auto& x : v)
    if (x == s) return true;
  return false;
}

}  // namespace

TEST_CASE("cold localhost against an IPv4-only server is fast", "[http][perf]") {
  // localhost resolves to ::1 first; the test server listens on 127.0.0.1 only. Without the
  // loopback SYN fix Windows retried the refused ::1 connect until curl's 200 ms
  // happy-eyeballs timer moved on to IPv4.
  const std::string u = "http://localhost:" + std::to_string(server().port()) + "/json/10";
  std::vector<double> totals;
  for (int i = 0; i < 5; ++i) {
    HttpClient c;  // nothing cached: DNS, connect, everything is cold
    HttpRequest r;
    r.url = u;
    r.fresh_connection = true;
    auto resp = c.send(r);
    INFO("run " << i << " error " << resp.error << " total " << resp.timing.total_ms
                << " connect " << resp.timing.connect_ms);
    REQUIRE(resp.ok);
    CHECK(resp.status == 200);
    CHECK(resp.remote_ip == "127.0.0.1");
    CHECK_FALSE(resp.timing.connection_reused);
    // Typically ~1.3 ms. The bound guards against the 200 ms happy-eyeballs stall; it is loose
    // because libcurl yields the CPU (SleepEx(0) in verifyconnect) on every connect check, which
    // costs a whole scheduler quantum (~15 ms) when all cores are busy.
    CHECK(resp.timing.total_ms < 100.0);
    CHECK(resp.timing.total_ms == Catch::Approx(phase_sum(resp)).margin(0.01));
    totals.push_back(resp.timing.total_ms);
  }
  std::sort(totals.begin(), totals.end());
  CHECK(totals[2] < 20.0);  // the median run is not hit by a scheduler quantum
  // A refused loopback port fails fast too (no SYN retries).
  HttpClient c;
  HttpRequest dead;
  dead.url = "http://localhost:1/";
  const auto t0 = Clock::now();
  auto d = c.send(dead);
  CHECK_FALSE(d.ok);
  CHECK(ms_between(t0, Clock::now()) < 100.0);
}

std::vector<std::string> offers_for(HttpVersion v) {
  HttpClient c;  // first: curl's global init also starts Winsock for the catcher
  HelloCatcher hc;
  HttpRequest r;
  r.url = "https://127.0.0.1:" + std::to_string(hc.port) + "/";
  r.verify_tls = false;
  r.timeout_ms = 3000;
  r.version = v;
  auto resp = c.send(r);
  CHECK_FALSE(resp.ok);  // the catcher hangs up mid-handshake
  hc.th.join();
  INFO("captured " << hc.hello.size() << " bytes");
  REQUIRE(hc.hello.size() > 50);
  return alpn_offers(hc.hello);
}

TEST_CASE("TLS ClientHello offers h2 via ALPN", "[http][h2]") {
  auto autov = offers_for(HttpVersion::Auto);
  CHECK(contains(autov, "h2"));
  CHECK(contains(autov, "http/1.1"));
  auto h1 = offers_for(HttpVersion::Http1_1);
  CHECK_FALSE(contains(h1, "h2"));
  CHECK(contains(h1, "http/1.1"));
}

TEST_CASE("real TLS server negotiates HTTP/2", "[http][h2][net]") {
  HttpClient c;
  HttpRequest r;
  r.url = "https://www.google.com/generate_204";
  r.timeout_ms = 10000;
  auto a = c.send(r);
  if (!a.ok) SKIP("network unavailable: " << a.error);
  CHECK(a.status == 204);
  CHECK(a.http_version == "HTTP/2");
  CHECK(a.timing.tls_ms > 0);
  CHECK(a.timing.total_ms == Catch::Approx(phase_sum(a)).margin(0.01));
  // Re-fire reuses the h2 connection.
  auto b = c.send(r);
  REQUIRE(b.ok);
  CHECK(b.http_version == "HTTP/2");
  CHECK(b.timing.connection_reused);
  CHECK(b.timing.tls_ms == 0);
}

TEST_CASE("redirect and process phases", "[http][timing]") {
  HttpClient c;
  REQUIRE(c.send(get("/status/200")).ok);  // warm the connection
  auto r = c.send(get("/status/302?location=/delay/100"));
  REQUIRE(r.ok);
  CHECK(r.status == 200);
  CHECK(r.redirect_count == 1);
  CHECK(r.redirect_ms > 0);
  CHECK(r.redirect_ms < 50);             // the 302 hop is fast; the delay is the final hop
  CHECK(r.timing.ttfb_ms >= 90);         // server think time of the final hop lands in TTFB
  CHECK(r.timing.total_ms == Catch::Approx(phase_sum(r)).margin(0.01));
  CHECK(r.process_ms >= 0);
  CHECK(r.process_ms < 20);

  HttpRequest no = get("/status/302?location=/delay/100");
  no.follow_redirects = false;
  auto n = c.send(no);
  REQUIRE(n.ok);
  CHECK(n.status == 302);
  CHECK(n.redirect_count == 0);
  CHECK(n.redirect_ms == 0);
  CHECK(n.timing.ttfb_ms < 90);
}

TEST_CASE("streaming without keeping the body", "[http]") {
  HttpClient c;
  HttpRequest r = get("/bytes/3000000");
  r.keep_body = false;
  size_t streamed = 0;
  auto resp = c.send(r, [&](const char*, size_t n) { streamed += n; });
  REQUIRE(resp.ok);
  CHECK(resp.body.empty());
  CHECK(resp.body.capacity() < 1024);
  CHECK(streamed == 3000000);
}

TEST_CASE("request and response sizes split into headers and body", "[http][sizes]") {
  HttpClient c;
  auto r = c.send(get("/json/100"));
  REQUIRE(r.ok);
  const HttpSizes& z = r.sizes;
  CHECK(z.request_body == 0);
  CHECK(z.request_headers > 20);
  CHECK(r.timing.bytes_sent == z.request_headers);
  CHECK(z.response_body == r.body.size());
  CHECK(z.response_body_wire == r.body.size());  // identity encoding
  CHECK(z.response_headers > 40);
  CHECK(r.timing.bytes_received == z.response_headers + z.response_body_wire);

  HttpRequest post = get("/echo");
  post.method = "POST";
  post.body = std::string(100, 'p');
  auto p = c.send(post);
  REQUIRE(p.ok);
  CHECK(p.sizes.request_body == 100);
  CHECK(p.timing.bytes_sent == p.sizes.request_headers + 100);

  // Compressed response: the wire size and the decoded size are both reported.
  auto g = c.send(get("/gzip/100000"));
  REQUIRE(g.ok);
  REQUIRE(g.status == 200);
  CHECK(g.body == std::string(100000, 'x'));
  CHECK(g.sizes.response_body == 100000);
  CHECK(g.sizes.response_body_wire > 0);
  CHECK(g.sizes.response_body_wire < 1000);
  CHECK(g.timing.bytes_received == g.sizes.response_headers + g.sizes.response_body_wire);

  // Streaming without keeping the body still counts it.
  HttpRequest st = get("/gzip/50000");
  st.keep_body = false;
  size_t streamed = 0;
  auto sr = c.send(st, [&](const char*, size_t n) { streamed += n; });
  REQUIRE(sr.ok);
  CHECK(sr.body.empty());
  CHECK(streamed == 50000);
  CHECK(sr.sizes.response_body == 50000);

  // HEAD: headers only.
  HttpRequest head = get("/bytes/5000");
  head.method = "HEAD";
  auto h = c.send(head);
  REQUIRE(h.ok);
  CHECK(h.sizes.response_body == 0);
  CHECK(h.sizes.response_body_wire == 0);
  CHECK(h.sizes.response_headers > 40);
}

TEST_CASE("redirect chain has a per-hop breakdown", "[http][timing]") {
  HttpClient c;  // cold: the first hop opens the connection, the others reuse it
  auto r = c.send(get("/redirect/3"));
  REQUIRE(r.ok);
  REQUIRE(r.redirect_count == 3);
  REQUIRE(r.hops.size() == 3);
  const char* paths[] = {"/redirect/3", "/redirect/2", "/redirect/1"};
  double sum = 0;
  for (size_t i = 0; i < r.hops.size(); ++i) {
    const RedirectHop& h = r.hops[i];
    INFO("hop " << i << " " << h.url);
    CHECK(h.url == url(paths[i]));
    CHECK(h.status == 302);
    CHECK(h.remote_ip == "127.0.0.1");
    CHECK(h.remote_port == server().port());
    CHECK(h.connection_reused == (i > 0));
    CHECK(h.total_ms == Catch::Approx(h.queue_ms + h.dns_ms + h.socket_ms + h.connect_ms +
                                      h.tls_ms + h.send_ms + h.ttfb_ms + h.download_ms)
                            .margin(0.002));
    CHECK(h.total_ms > 0);
    CHECK(h.bytes_sent > 20);
    CHECK(h.bytes_received > 40);
    sum += h.total_ms;
  }
  CHECK(r.hops[0].connect_ms + r.hops[0].socket_ms > 0);
  CHECK(r.hops[1].connect_ms == 0);
  CHECK(sum == Catch::Approx(r.redirect_ms).margin(0.002));
  // The final hop reused the connection the first hop opened.
  CHECK(r.timing.connection_reused);
  CHECK(r.timing.connect_ms == 0);
  CHECK(r.timing.total_ms == Catch::Approx(phase_sum(r)).margin(0.01));
  // Sizes cover the final request only; totals cover every hop.
  uint64_t sent = r.sizes.request_headers + r.sizes.request_body;
  uint64_t recv = r.sizes.response_headers + r.sizes.response_body_wire;
  for (const auto& h : r.hops) {
    sent += h.bytes_sent;
    recv += h.bytes_received;
  }
  CHECK(r.timing.bytes_sent == sent);
  CHECK(r.timing.bytes_received == recv);

  // Mixed redirect codes keep their own status per hop.
  auto d = c.send(get("/status/302?location=/status/307%3Flocation%3D/echo"));
  REQUIRE(d.ok);
  REQUIRE(d.hops.size() == 2);
  CHECK(d.hops[0].status == 302);
  CHECK(d.hops[1].status == 307);

  // A redirect that is not followed has no hops.
  HttpRequest no = get("/redirect/2");
  no.follow_redirects = false;
  auto n = c.send(no);
  REQUIRE(n.ok);
  CHECK(n.hops.empty());
  CHECK(n.redirect_ms == 0);

  // A redirect to a dead port: the hop is recorded, the final request fails in connect.
  auto dead = c.send(get("/status/302?location=http://127.0.0.1:1/"));
  CHECK_FALSE(dead.ok);
  CHECK(dead.hops.size() == 1);
  CHECK(dead.timing.total_ms == Catch::Approx(phase_sum(dead)).margin(0.01));
}

TEST_CASE("prepare, queue and socket phases are separate", "[http][timing]") {
  HttpClient::global_init();  // idempotent
  HttpClient c;
  HttpRequest r = get("/status/200");
  r.fresh_connection = true;
  auto a = c.send(r);
  REQUIRE(a.ok);
  CHECK_FALSE(a.timing.connection_reused);
  CHECK(a.prepare_ms > 0);
  CHECK(a.prepare_ms < 5);
  CHECK(a.socket_ms >= 0);
  CHECK(a.socket_ms < 5);
  CHECK(a.timing.connect_ms > 0);
  CHECK(a.timing.total_ms == Catch::Approx(phase_sum(a)).margin(0.01));
  auto b = c.send(get("/status/200"));
  REQUIRE(b.ok);
  CHECK(b.timing.connection_reused);
  CHECK(b.socket_ms == 0);
  CHECK(b.timing.connect_ms == 0);
  CHECK(b.prepare_ms > 0);
  CHECK(b.timing.total_ms == Catch::Approx(phase_sum(b)).margin(0.01));
}

TEST_CASE("options do not leak between requests", "[http]") {
  // The easy handle is never reset, so every per-request option must be re-applied when it
  // changes: method, body, HEAD, redirects, timeout, headers, fresh connection.
  HttpClient c;
  HttpRequest post = get("/echo");
  post.method = "POST";
  post.body = "one";
  REQUIRE(c.send(post).body.find("\"method\":\"POST\"") != std::string::npos);
  auto g = c.send(get("/echo"));
  CHECK(g.body.find("\"method\":\"GET\"") != std::string::npos);
  CHECK(g.body.find("\"body\":\"\"") != std::string::npos);
  HttpRequest head = get("/echo");
  head.method = "HEAD";
  auto h = c.send(head);
  CHECK(h.ok);
  CHECK(h.body.empty());
  auto g2 = c.send(get("/echo"));
  CHECK(g2.body.find("\"method\":\"GET\"") != std::string::npos);
  HttpRequest del = get("/echo");
  del.method = "DELETE";
  CHECK(c.send(del).body.find("\"method\":\"DELETE\"") != std::string::npos);
  CHECK(c.send(get("/echo")).body.find("\"method\":\"GET\"") != std::string::npos);
  HttpRequest put = get("/echo");
  put.method = "PUT";
  put.body = "two";
  auto pr = c.send(put);
  CHECK(pr.body.find("\"method\":\"PUT\"") != std::string::npos);
  CHECK(pr.body.find("\"body\":\"two\"") != std::string::npos);
  HttpRequest post2 = get("/echo");
  post2.method = "POST";
  post2.body = "three";
  CHECK(c.send(post2).body.find("\"method\":\"POST\"") != std::string::npos);

  HttpRequest nofollow = get("/redirect/1");
  nofollow.follow_redirects = false;
  CHECK(c.send(nofollow).status == 302);
  CHECK(c.send(get("/redirect/1")).status == 200);

  HttpRequest slow = get("/delay/300");
  slow.timeout_ms = 50;
  CHECK_FALSE(c.send(slow).ok);
  CHECK(c.send(get("/delay/100")).ok);  // the default timeout is back

  HttpRequest hdr = get("/echo");
  hdr.headers = {{"X-A", "1"}};
  CHECK(c.send(hdr).body.find("\"X-A\":\"1\"") != std::string::npos);
  CHECK(c.send(hdr).body.find("\"X-A\":\"1\"") != std::string::npos);  // reused list
  CHECK(c.send(get("/echo")).body.find("X-A") == std::string::npos);

  const uint64_t before = server().connections_accepted();
  HttpRequest fresh = get("/status/200");
  fresh.fresh_connection = true;
  REQUIRE(c.send(fresh).ok);
  REQUIRE(c.send(get("/status/200")).timing.connection_reused);
  CHECK(server().connections_accepted() - before == 1);
}

TEST_CASE("HTTP/2 sizes count the body once", "[http][h2][net]") {
  HttpClient c;
  HttpRequest r;
  r.url = "https://www.google.com/generate_204";
  r.method = "POST";
  r.body = std::string(100, 'b');
  r.timeout_ms = 10000;
  auto a = c.send(r);
  if (!a.ok) SKIP("network unavailable: " << a.error);
  CHECK(a.http_version == "HTTP/2");
  CHECK(a.sizes.request_body == 100);
  CHECK(a.timing.bytes_sent == a.sizes.request_headers + 100);
  CHECK(a.sizes.request_headers < 400);
  CHECK(a.timing.bytes_received == a.sizes.response_headers + a.sizes.response_body_wire);
}

TEST_CASE("response headers: lookup, copies and moves", "[http][headers]") {
  HttpClient c;
  auto r = c.send(get("/response-headers?X-Mixed-Case=v1&X-Dup=a&X-Dup=b"));
  REQUIRE(r.ok);
  // Case-insensitive lookup returns the first of duplicates.
  CHECK(r.headers.get("x-mixed-case") == "v1");
  CHECK(r.headers.get("X-MIXED-CASE") == "v1");
  CHECK(r.headers.get("x-dup") == "a");
  CHECK(r.headers.contains("Content-Length"));
  CHECK_FALSE(r.headers.contains("X-Nope"));
  CHECK(r.headers.get("X-Nope").empty());
  // Owning copy keeps order and duplicates.
  const std::vector<Header> v = r.headers.to_vector();
  REQUIRE(v.size() == r.headers.size());
  size_t i = 0;
  for (const auto& [k, val] : r.headers) {
    CHECK(v[i].first == k);
    CHECK(v[i].second == val);
    ++i;
  }
  CHECK(std::count_if(v.begin(), v.end(), [](const Header& h) { return h.first == "X-Dup"; }) == 2);
  // Views are rebuilt from offsets, so a moved / copied response still reads correctly (also
  // when the header text fits the small-string buffer).
  HttpResponse moved = std::move(r);
  CHECK(moved.headers.get("x-mixed-case") == "v1");
  HttpResponse copy = moved;
  CHECK(copy.headers.get("x-dup") == "a");
  CHECK(copy.headers.to_vector() == v);
  ResponseHeaders small;
  small.add("A", "1");
  ResponseHeaders small2 = std::move(small);
  CHECK(small2.get("a") == "1");
  CHECK(small2[0].first == "A");
  // The next response on the same client has only its own headers.
  auto next = c.send(get("/status/200"));
  REQUIRE(next.ok);
  CHECK_FALSE(next.headers.contains("X-Dup"));
  // A transfer that never got a response has none.
  HttpRequest refused;
  refused.url = "http://127.0.0.1:1/";
  auto f = c.send(refused);
  CHECK_FALSE(f.ok);
  CHECK(f.headers.empty());
}

TEST_CASE("client construction is cheap after global_init and every client starts cold",
          "[http][pool]") {
  HttpClient::global_init();
  {
    const auto t0 = Clock::now();
    HttpClient a;
    const double ctor_ms = ms_between(t0, Clock::now());
    INFO("ctor after global_init " << ctor_ms << " ms");
    CHECK(ctor_ms < 0.4);  // a fresh curl_multi_init alone is ~0.5 ms
    auto r = a.send(get("/status/200"));
    REQUIRE(r.ok);
    CHECK_FALSE(r.timing.connection_reused);
    CHECK(a.send(get("/status/200")).timing.connection_reused);
  }
  // a's multi handle went back to the pool: the next client is cheap too, but cold.
  double best = 1e9;
  for (int i = 0; i < 5; ++i) {
    const auto t0 = Clock::now();
    HttpClient b;
    best = std::min(best, ms_between(t0, Clock::now()));
    auto r = b.send(get("/status/200"));
    REQUIRE(r.ok);
    CHECK_FALSE(r.timing.connection_reused);
  }
  INFO("recycled ctor best " << best << " ms");
  CHECK(best < 0.4);
  // Recycled multi handles still cancel promptly (their wakeup pair is intact).
  HttpClient c;
  std::thread t([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    c.cancel();
  });
  const auto t0 = Clock::now();
  auto r = c.send(get("/delay/3000"));
  t.join();
  CHECK(r.error == "cancelled");
  CHECK(ms_between(t0, Clock::now()) < 1000);
  // Many clients alive at once (more than the pool holds) all work.
  std::vector<std::unique_ptr<HttpClient>> many;
  for (int i = 0; i < 12; ++i) many.push_back(std::make_unique<HttpClient>());
  for (auto& m : many) CHECK(m->send(get("/status/204")).status == 204);
}

TEST_CASE("the pinned libcurl is new enough", "[http]") {
  // 8.22 skips the per-reuse liveness poll when the connection was checked < 1 s ago (8.20
  // ran it on every warm request); build.ps1 pins it through ports/curl.
  const curl_version_info_data* v = curl_version_info(CURLVERSION_NOW);
  REQUIRE(v);
  INFO("libcurl " << v->version);
  CHECK(v->version_num >= 0x081600);
}

TEST_CASE("default User-Agent can be overridden or emptied", "[http]") {
  HttpClient c;
  auto d = c.send(get("/echo"));
  REQUIRE(d.ok);
  CHECK(d.body.find("\"User-Agent\":\"watt/0.1\"") != std::string::npos);

  HttpRequest own = get("/echo");
  own.headers = {{"user-agent", "mytool/2"}};
  auto o = c.send(own);
  REQUIRE(o.ok);
  CHECK(o.body.find("mytool/2") != std::string::npos);
  CHECK(o.body.find("watt/0.1") == std::string::npos);

  HttpRequest empty = get("/echo");
  empty.headers = {{"User-Agent", ""}};
  auto e = c.send(empty);
  REQUIRE(e.ok);
  CHECK(e.body.find("watt/0.1") == std::string::npos);

  // and the default comes back once the override is gone
  auto again = c.send(get("/echo"));
  REQUIRE(again.ok);
  CHECK(again.body.find("\"User-Agent\":\"watt/0.1\"") != std::string::npos);
}

TEST_CASE("a failure on a reused connection is booked as waiting, not DNS", "[http][timing]") {
  HttpClient c;
  REQUIRE(c.send(get("/status/200")).ok);
  HttpRequest slow = get("/delay/500");
  slow.timeout_ms = 100;
  auto r = c.send(slow);
  CHECK_FALSE(r.ok);
  CHECK(r.status == 0);
  CHECK(r.timing.connection_reused);
  CHECK(r.timing.dns_ms == 0);
  CHECK(r.timing.connect_ms == 0);
  CHECK(r.timing.ttfb_ms >= 90);
  CHECK(r.timing.download_ms == 0);
  CHECK(r.timing.total_ms == Catch::Approx(phase_sum(r)).margin(1e-6));

  // cancelled while waiting on a reused connection: same
  REQUIRE(c.send(get("/status/200")).ok);
  std::thread t([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    c.cancel();
  });
  auto k = c.send(get("/delay/2000"));
  t.join();
  CHECK(k.error == "cancelled");
  CHECK(k.timing.connection_reused);
  CHECK(k.timing.dns_ms == 0);
  CHECK(k.timing.ttfb_ms >= 70);
  CHECK(k.timing.total_ms == Catch::Approx(phase_sum(k)).margin(1e-6));
}

TEST_CASE("failures before a response land in the phase that was running", "[http][timing]") {
  HttpClient c;
  SECTION("refused connect") {
    HttpRequest dead;
    dead.url = "http://127.0.0.1:1/";
    auto d = c.send(dead);
    CHECK_FALSE(d.ok);
    CHECK_FALSE(d.timing.connection_reused);
    CHECK(d.timing.ttfb_ms == 0);
    CHECK(d.timing.send_ms == 0);
    CHECK(d.socket_ms > 0);  // the socket was set up even though the connect failed
    CHECK(d.timing.total_ms == Catch::Approx(phase_sum(d)).margin(1e-6));
  }
  SECTION("unresolvable host") {
    HttpRequest nx;
    nx.url = "http://no-such-host.invalid/";
    auto n = c.send(nx);
    CHECK_FALSE(n.ok);
    CHECK_FALSE(n.timing.connection_reused);
    CHECK(n.timing.dns_ms > 0);
    CHECK(n.timing.connect_ms == 0);
    CHECK(n.timing.ttfb_ms == 0);
    CHECK(n.timing.total_ms == Catch::Approx(phase_sum(n)).margin(1e-6));
  }
  SECTION("server never answers on a fresh connection") {
    HttpRequest slow = get("/delay/500");
    slow.timeout_ms = 100;
    slow.fresh_connection = true;
    auto r = c.send(slow);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.timing.connection_reused);
    CHECK(r.timing.connect_ms > 0);
    CHECK(r.timing.ttfb_ms >= 80);
    CHECK(r.timing.total_ms == Catch::Approx(phase_sum(r)).margin(1e-6));
  }
}

TEST_CASE("process is measured from the moment curl returns", "[http][timing]") {
  HttpClient c;
  REQUIRE(c.send(get("/json/10")).ok);
  for (int i = 0; i < 20; ++i) {
    const auto t0 = Clock::now();
    auto r = c.send(get("/json/10"));
    const double wall = ms_between(t0, Clock::now());
    REQUIRE(r.ok);
    CHECK(r.timing.connection_reused);
    CHECK(r.process_ms > 0);
    CHECK(r.process_ms < 1);
    CHECK(r.timing.download_ms >= 0);
    CHECK(r.timing.total_ms <= wall);
    CHECK(r.timing.total_ms == Catch::Approx(phase_sum(r)).margin(1e-6));
  }
}

TEST_CASE("busy-poll window on and off", "[http][timing]") {
  HttpClient c;
  for (long window : {0L, 20L}) {
    INFO("busy_poll_ms " << window);
    HttpRequest w = get("/wait/3000");  // precise 3 ms server wait
    w.busy_poll_ms = window;
    auto r = c.send(w);
    REQUIRE(r.ok);
    CHECK(r.status == 200);
    CHECK(r.body == "{\"wait_us\":3000}");
    CHECK(r.timing.ttfb_ms >= 2.9);
    CHECK(r.timing.total_ms == Catch::Approx(phase_sum(r)).margin(1e-6));
  }
  // a wait longer than the window still completes (the loop falls back to sleeping) and a
  // cancel inside the window is still prompt
  HttpRequest slow = get("/delay/60");
  slow.busy_poll_ms = 5;
  auto s = c.send(slow);
  REQUIRE(s.ok);
  CHECK(s.timing.ttfb_ms >= 55);
  std::thread t([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    c.cancel();
  });
  const auto t0 = Clock::now();
  auto k = c.send(get("/delay/2000"));
  t.join();
  CHECK(k.error == "cancelled");
  CHECK(ms_between(t0, Clock::now()) < 500);
}

// ---------------------------------------------------------------- local TLS server
namespace {

std::filesystem::path test_exe_dir() {
  wchar_t buf[MAX_PATH];
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return std::filesystem::path(buf).parent_path();
}

std::filesystem::path testserver_cert_dir() {
  auto here = std::filesystem::path(__FILE__).parent_path().parent_path() / "tools" / "testserver" / "certs";
  if (std::filesystem::exists(here / "ca.pem")) return here;
  return test_exe_dir().parent_path().parent_path() / "tools" / "testserver" / "certs";
}

std::string slurp_file(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// watt_testserver (built next to this exe) with --tls: its gRPC-Web listener is an HTTP/1.1
// server over OpenSSL with tools/testserver/certs/server.pem (CN=localhost, issued by the test
// CA). Any GET answers 405 on a kept-alive connection, which is all a TLS test needs. Started
// once, killed with the test process. port() == 0 when the exe is not there.
class TlsTestServer {
 public:
  static TlsTestServer& get() {
    static TlsTestServer s;
    return s;
  }
  int port() const { return port_; }
  std::string url(const std::string& path = "/") const {
    return "https://localhost:" + std::to_string(port_) + path;
  }
  HttpRequest request(const std::string& path = "/") const {
    HttpRequest r;
    r.url = url(path);
    r.ca_pem = ca_pem_;
    r.timeout_ms = 5000;
    return r;
  }
  ~TlsTestServer() {
    if (pi_.hProcess) {
      TerminateProcess(pi_.hProcess, 0);
      CloseHandle(pi_.hThread);
      CloseHandle(pi_.hProcess);
    }
    if (job_) CloseHandle(job_);
  }

 private:
  TlsTestServer() {
    const auto exe = test_exe_dir() / "watt_testserver.exe";
    const auto certs = testserver_cert_dir();
    if (!std::filesystem::exists(exe) || !std::filesystem::exists(certs / "ca.pem")) return;
    ca_pem_ = slurp_file(certs / "ca.pem");
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &li, sizeof(li));
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    std::wstring cmd = L"\"" + exe.wstring() + L"\" 0 --tls \"" + certs.wstring() + L"\"";
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi_)) {
      CloseHandle(rd);
      CloseHandle(wr);
      pi_ = {};
      return;
    }
    AssignProcessToJobObject(job_, pi_.hProcess);
    CloseHandle(wr);
    auto read_line = [&] {
      std::string line;
      char c;
      DWORD n;
      while (ReadFile(rd, &c, 1, &n, nullptr) && n == 1 && c != '\n') line += c;
      return line;
    };
    (void)read_line();                      // gRPC port banner
    const std::string web = read_line();    // "... grpc-web listening on 127.0.0.1:<port>"
    if (web.find("grpc-web listening") != std::string::npos)
      port_ = std::stoi(web.substr(web.rfind(':') + 1));
    CloseHandle(rd);
  }
  PROCESS_INFORMATION pi_{};
  HANDLE job_ = nullptr;
  int port_ = 0;
  std::string ca_pem_;
};

double p50(std::vector<double> v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

}  // namespace

// watt_test_http "[.bench]": warm p50 of total_ms, local plaintext and local HTTPS (and a
// public HTTPS server with WATT_NET_BENCH=1).
TEST_CASE("warm request p50", "[.bench]") {
  auto run = [](HttpClient& c, const HttpRequest& r, int n) {
    std::vector<double> t;
    t.reserve(n);
    for (int i = 0; i < 200; ++i) REQUIRE(c.send(r).status != 0);  // warm-up
    for (int i = 0; i < n; ++i) {
      auto x = c.send(r);
      REQUIRE(x.status != 0);
      REQUIRE(x.timing.connection_reused);
      t.push_back(x.timing.total_ms);
    }
    return p50(t);
  };
  for (int round = 0; round < 3; ++round) {
    HttpClient c;
    const double plain = run(c, get("/json/10"), 5000);
    double tls = 0;
    if (TlsTestServer::get().port()) {
      HttpClient ct;
      tls = run(ct, TlsTestServer::get().request(), 5000);
    }
    WARN("round " << round << ": warm p50 local http " << plain << " ms, local https " << tls
                  << " ms");
  }
  char* env = nullptr;
  size_t len = 0;
  if (_dupenv_s(&env, &len, "WATT_NET_BENCH") == 0 && env) {
    free(env);
    HttpClient c;
    HttpRequest r;
    r.url = "https://www.google.com/generate_204";
    std::vector<double> t;
    REQUIRE(c.send(r).ok);
    for (int i = 0; i < 50; ++i) t.push_back(c.send(r).timing.total_ms);
    WARN("warm p50 google https " << p50(t) << " ms");
  }
}

// ---------------------------------------------------------------- connection / TLS details
TEST_CASE("connection info: plaintext", "[http][conn]") {
  HttpClient c;
  auto a = c.send(get("/status/200"));
  REQUIRE(a.ok);
  const ConnectionInfo& ca = a.conn;
  CHECK(ca.id >= 0);
  CHECK_FALSE(ca.reused);
  CHECK_FALSE(ca.tls);
  CHECK(ca.tls_info == nullptr);
  CHECK(ca.local_ip == "127.0.0.1");
  CHECK(ca.local_port > 0);
  CHECK(ca.local_port != server().port());
  CHECK(ca.remote_ip == "127.0.0.1");
  CHECK(ca.remote_port == server().port());
  CHECK(ca.remote_ip == a.remote_ip);
  CHECK(ca.remote_port == a.remote_port);
  CHECK(a.local_ip == ca.local_ip);  // mirrors
  CHECK(a.local_port == ca.local_port);
  CHECK(a.tls_version.empty());
  CHECK(a.cert_subject.empty());
  // Reuse: the same connection, the same details.
  auto b = c.send(get("/json/10"));
  REQUIRE(b.ok);
  CHECK(b.conn.reused);
  CHECK(b.conn.id == ca.id);
  CHECK(b.conn.local_port == ca.local_port);
  CHECK(b.conn.local_ip == ca.local_ip);
  CHECK(b.conn.remote_port == ca.remote_port);
  // A new connection has a new id and local port.
  HttpRequest fr = get("/status/200");
  fr.fresh_connection = true;
  auto f = c.send(fr);
  REQUIRE(f.ok);
  CHECK_FALSE(f.conn.reused);
  CHECK(f.conn.id != ca.id);
  CHECK(f.conn.local_port != ca.local_port);
  // A refused connect has no connection details to show beyond what curl knows.
  HttpClient d;
  HttpRequest dead;
  dead.url = "http://127.0.0.1:1/";
  auto x = d.send(dead);
  CHECK_FALSE(x.ok);
  CHECK_FALSE(x.conn.tls);
  CHECK(x.conn.tls_info == nullptr);
}

TEST_CASE("connection info: TLS against the local test server", "[http][conn][tls]") {
  auto& srv = TlsTestServer::get();
  if (!srv.port()) SKIP("watt_testserver.exe is not next to the test exe");
  HttpClient c;
  auto a = c.send(srv.request());
  INFO("error " << a.error);
  REQUIRE(a.ok);
  CHECK(a.status == 405);  // the gRPC-Web listener only takes POST; any answer will do
  const ConnectionInfo& ci = a.conn;
  CHECK_FALSE(ci.reused);
  CHECK(ci.tls);
  CHECK(ci.local_ip == "127.0.0.1");
  CHECK(ci.local_port > 0);
  CHECK(ci.remote_ip == "127.0.0.1");
  CHECK(ci.remote_port == srv.port());
  REQUIRE(ci.tls_info);
  const TlsInfo& t = *ci.tls_info;
  WARN("local TLS: " << t.version << " " << t.cipher << " alpn '" << t.alpn << "', read in "
                     << t.read_ms * 1000 << " us");
  CHECK((t.version == "TLSv1.3" || t.version == "TLSv1.2"));
  CHECK(t.cipher.rfind("TLS_", 0) == 0);
  CHECK((t.alpn.empty() || t.alpn == "http/1.1"));  // the OpenSSL listener selects no ALPN
  CHECK(t.verify_result.rfind("verified", 0) == 0);
  CHECK(t.leaf.subject == "CN=localhost");
  CHECK(t.leaf.subject_cn == "localhost");
  CHECK(t.leaf.issuer == "CN=watt test CA");
  CHECK(t.leaf.issuer_cn == "watt test CA");
  CHECK(t.leaf.subject_alt_names ==
        std::vector<std::string>{"localhost", "watt.test", "127.0.0.1"});
  CHECK(t.leaf.not_before == "2026-11-16T23:06:29Z");
  CHECK(t.leaf.not_after == "2126-10-23T23:06:29Z");
  CHECK(t.leaf.serial == "1D29EDA45D4C26B44DD66472531F028C5556227A");
  CHECK(t.leaf.sha256_fingerprint ==
        "F3:01:B3:F3:55:F3:67:49:37:70:D4:E2:EC:C8:42:5D:4C:A5:F2:0B:00:67:85:D7:CD:22:99:FD:3C:"
        "BA:48:AE");
  CHECK(t.chain_length == 1);  // the server sends its leaf only
  CHECK(t.read_ms < 5.0);
  // Mirrors the response view reads.
  CHECK(a.tls_version == t.version);
  CHECK(a.tls_cipher == t.cipher);
  CHECK(a.cert_subject == "CN=localhost");
  CHECK(a.cert_issuer == "CN=watt test CA");
  CHECK(a.cert_san == "localhost, watt.test, 127.0.0.1");
  CHECK(a.cert_not_after == "2126-10-23 23:06:29 UTC");
  CHECK(a.cert_chain.find("localhost  (issued by watt test CA, until 2126-10-23)") == 0);
  CHECK(a.verify_result == t.verify_result);
  CHECK(a.local_ip == ci.local_ip);
  CHECK(a.local_port == ci.local_port);

  // Re-fires on the same connection share the very same details (no re-read).
  for (int i = 0; i < 5; ++i) {
    auto b = c.send(srv.request());
    REQUIRE(b.ok);
    CHECK(b.conn.reused);
    CHECK(b.conn.id == ci.id);
    CHECK(b.conn.tls);
    CHECK(b.conn.tls_info.get() == ci.tls_info.get());
    CHECK(b.conn.local_port == ci.local_port);
    CHECK(b.tls_version == a.tls_version);
    CHECK(b.cert_not_after == a.cert_not_after);
  }
  // A fresh connection is read again.
  HttpRequest fr = srv.request();
  fr.fresh_connection = true;
  auto f = c.send(fr);
  REQUIRE(f.ok);
  CHECK_FALSE(f.conn.reused);
  CHECK(f.conn.id != ci.id);
  REQUIRE(f.conn.tls_info);
  CHECK(f.conn.tls_info.get() != ci.tls_info.get());
  CHECK(f.conn.tls_info->leaf.sha256_fingerprint == t.leaf.sha256_fingerprint);

  // Verification off: still the details, and the verdict says so.
  HttpClient u;
  HttpRequest ur = srv.request();
  ur.ca_pem.clear();
  ur.verify_tls = false;
  auto v = u.send(ur);
  REQUIRE(v.ok);
  REQUIRE(v.conn.tls_info);
  CHECK(v.conn.tls_info->verify_result.rfind("not verified", 0) == 0);
  CHECK(v.conn.tls_info->leaf.subject == "CN=localhost");
}

TEST_CASE("connection info: real HTTPS server", "[http][conn][tls][net]") {
  HttpClient c;
  HttpRequest r;
  r.url = "https://www.google.com/generate_204";
  r.timeout_ms = 10000;
  auto a = c.send(r);
  if (!a.ok) SKIP("network unavailable: " << a.error);
  REQUIRE(a.conn.tls);
  REQUIRE(a.conn.tls_info);
  const TlsInfo& t = *a.conn.tls_info;
  WARN("google: " << t.version << " " << t.cipher << " alpn " << t.alpn << " | "
                  << t.leaf.subject << " | " << t.leaf.issuer << " | " << t.leaf.not_before
                  << " .. " << t.leaf.not_after << " | chain " << t.chain_length << " | read "
                  << t.read_ms * 1000 << " us");
  CHECK((t.version == "TLSv1.3" || t.version == "TLSv1.2"));
  CHECK(t.cipher.rfind("TLS_", 0) == 0);
  CHECK(t.alpn == "h2");
  CHECK(a.http_version == "HTTP/2");
  CHECK(t.verify_result.rfind("verified", 0) == 0);
  CHECK(t.leaf.subject.find("google") != std::string::npos);
  CHECK_FALSE(t.leaf.issuer.empty());
  CHECK(t.leaf.not_before.size() == 20);
  CHECK(t.leaf.not_after.size() == 20);
  CHECK(t.leaf.not_before < t.leaf.not_after);
  CHECK(t.leaf.sha256_fingerprint.size() == 32 * 3 - 1);
  CHECK_FALSE(t.leaf.serial.empty());
  CHECK(std::find(t.leaf.subject_alt_names.begin(), t.leaf.subject_alt_names.end(),
                  "www.google.com") != t.leaf.subject_alt_names.end());
  CHECK(t.chain_length >= 2);
  CHECK_FALSE(a.conn.local_ip.empty());
  CHECK(a.conn.local_port > 0);
  CHECK(a.conn.remote_port == 443);
  auto b = c.send(r);
  REQUIRE(b.ok);
  CHECK(b.conn.reused);
  CHECK(b.conn.id == a.conn.id);
  CHECK(b.conn.tls_info.get() == a.conn.tls_info.get());
  // A second new connection in the same process: the steady-state cost of reading the details.
  HttpRequest fr = r;
  fr.fresh_connection = true;
  auto f = c.send(fr);
  REQUIRE(f.ok);
  REQUIRE(f.conn.tls_info);
  CHECK(f.conn.tls_info->alpn == "h2");
  WARN("google: second new connection read in " << f.conn.tls_info->read_ms * 1000 << " us (tls "
                                                << f.timing.tls_ms << " ms)");
}

// watt_test_http "[.tlscold]": the first certificate read in a process after global_init
// (as the app runs it), against a public server.
TEST_CASE("connection info: first read in a process", "[.tlscold]") {
  const auto t0 = Clock::now();
  HttpClient::global_init();
  const double init_ms = ms_between(t0, Clock::now());
  HttpClient c;
  HttpRequest r;
  r.url = "https://www.google.com/generate_204";
  auto a = c.send(r);
  if (!a.ok) SKIP("network unavailable: " << a.error);
  REQUIRE(a.conn.tls_info);
  WARN("global_init " << init_ms << " ms; first certificate read " << a.conn.tls_info->read_ms * 1000
                      << " us; tls " << a.timing.tls_ms << " ms, send " << a.timing.send_ms << " ms");
}

// watt_test_http "[.bench]": what HttpResponse::conn adds to a warm request, measured in
// isolation (the warm end-to-end p50 moves by more than that from run to run): the copy send()
// makes from the per-connection cache (ConnectionInfo + the mirror strings) and the
// CURLINFO_CONN_ID lookup on_prereq does.
TEST_CASE("connection info: warm per-request cost", "[.bench]") {
  auto& srv = TlsTestServer::get();
  if (!srv.port()) SKIP("watt_testserver.exe is not next to the test exe");
  HttpClient c;
  auto a = c.send(srv.request());
  REQUIRE(a.conn.tls_info);
  const int n = 200000;
  volatile size_t sink = 0;
  auto t0 = Clock::now();
  for (int i = 0; i < n; ++i) {
    HttpResponse r;  // what send() does on a cache hit
    r.conn = a.conn;
    r.local_ip = a.conn.local_ip;
    r.local_port = a.conn.local_port;
    r.tls_version = a.tls_version;
    r.tls_cipher = a.tls_cipher;
    r.cert_subject = a.cert_subject;
    r.cert_san = a.cert_san;
    r.cert_issuer = a.cert_issuer;
    r.cert_not_after = a.cert_not_after;
    r.cert_chain = a.cert_chain;
    r.verify_result = a.verify_result;
    sink = sink + r.cert_san.size();
  }
  const double with_us = ms_between(t0, Clock::now()) * 1000 / n;
  t0 = Clock::now();
  for (int i = 0; i < n; ++i) {
    HttpResponse r;
    r.remote_ip = a.remote_ip;
    sink = sink + r.remote_ip.size();
  }
  const double without_us = ms_between(t0, Clock::now()) * 1000 / n;
  CURL* e = curl_easy_init();
  t0 = Clock::now();
  for (int i = 0; i < n; ++i) {
    curl_off_t id = -1;
    curl_easy_getinfo(e, CURLINFO_CONN_ID, &id);
    sink = sink + static_cast<size_t>(id);
  }
  const double getinfo_us = ms_between(t0, Clock::now()) * 1000 / n;
  curl_easy_cleanup(e);
  WARN("per warm request: copy conn + mirrors " << (with_us - without_us) * 1000
                                               << " ns (of which conn alone is a shared_ptr + 2 "
                                                  "short strings), CONN_ID lookup "
                                               << getinfo_us * 1000 << " ns");
}

TEST_CASE("redirect options: limit, keep method, keep credentials; URL as written", "[http][redirect]") {
  HttpClient c;
  HttpRequest lim = get("/redirect/3");
  lim.max_redirects = 2;
  auto l = c.send(lim);
  CHECK_FALSE(l.ok);  // curl stops at the limit
  lim.max_redirects = 3;
  l = c.send(lim);
  REQUIRE(l.ok);
  CHECK(l.status == 200);

  HttpRequest post = get("/status/302?location=/echo");
  post.method = "POST";
  post.body = "kept";
  auto p = c.send(post);
  REQUIRE(p.ok);
  CHECK(p.body.find("\"method\":\"GET\"") != std::string::npos);  // like a browser
  post.keep_method_on_redirect = true;
  p = c.send(post);
  REQUIRE(p.ok);
  CHECK(p.body.find("\"method\":\"POST\"") != std::string::npos);
  CHECK(p.body.find("\"body\":\"kept\"") != std::string::npos);
  HttpRequest put = get("/status/303?location=/echo");
  put.method = "PUT";
  put.body = "x";
  put.keep_method_on_redirect = true;
  p = c.send(put);
  REQUIRE(p.ok);
  CHECK(p.body.find("\"method\":\"PUT\"") != std::string::npos);

  // Credentials go to the original host only, unless keep_auth_on_redirect.
  const std::string other = "http://localhost:" + std::to_string(server().port()) + "/echo";
  HttpRequest au = get("/status/302?location=" + other);
  au.headers = {{"Authorization", "Bearer s3cret"}};
  auto a = c.send(au);
  REQUIRE(a.ok);
  CHECK(a.body.find("s3cret") == std::string::npos);
  au.keep_auth_on_redirect = true;
  a = c.send(au);
  REQUIRE(a.ok);
  CHECK(a.body.find("s3cret") != std::string::npos);
  // Windows SSO never follows to another host, even then.
  au.headers.clear();
  au.auth = HttpRequest::Auth::Ntlm;
  au.auth_user = "u";
  au.auth_password = "p";
  a = c.send(au);
  REQUIRE(a.ok);
  CHECK(a.body.find("authorization") == std::string::npos);

  // encode_url = false: dot segments are sent as written (curl would remove them).
  HttpRequest dots = get("/x/../echo");
  auto d = c.send(dots);
  REQUIRE(d.ok);
  CHECK(d.status == 200);
  dots.encode_url = false;
  d = c.send(dots);
  REQUIRE(d.ok);
  CHECK(d.status != 200);

  // TLS minimum version: a plain http request is unaffected.
  HttpRequest tls = get("/echo");
  tls.tls_min_version = TlsVersion::Tls1_3;
  CHECK(c.send(tls).status == 200);
}
