// Cookie jar (core/cookies.h) and its Workspace wiring (http_to_send, record_cookies).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "core/cookies.h"
#include "core/model.h"

using namespace watt;
namespace fs = std::filesystem;

namespace {

fs::path temp_path(const char* prefix) {
  std::random_device rd;
  return fs::temp_directory_path() / (std::string(prefix) + std::to_string(rd()) + std::to_string(rd()));
}

struct TestConfigDir {
  fs::path dir = temp_path("watt_test_cookies_cfg_");
  TestConfigDir() { Workspace::set_user_config_dir(dir); }
  ~TestConfigDir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
} test_config_dir;

struct TempDir {
  fs::path path = temp_path("watt_test_cookies_");
  TempDir() { fs::create_directories(path); }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}
void spit(const fs::path& p, const std::string& s) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  f << s;
}

std::string cookie_of(const HttpRequest& h) {
  for (const auto& [k, v] : h.headers)
    if (k == "Cookie" || k == "cookie") return v;
  return {};
}

constexpr int64_t T0 = 1767225600;  // 2026-01-01 00:00:00 UTC

}  // namespace

TEST_CASE("cookie dates (RFC 6265 5.1.1)") {
  int64_t t = 0;
  REQUIRE(cookies::parse_cookie_date("Wed, 21 Oct 2015 07:28:00 GMT", t));
  CHECK(t == 1445412480);
  REQUIRE(cookies::parse_cookie_date("Wednesday, 21-Oct-15 07:28:00 GMT", t));  // RFC 850
  CHECK(t == 1445412480);
  REQUIRE(cookies::parse_cookie_date("Wed Oct 21 07:28:00 2015", t));  // asctime
  CHECK(t == 1445412480);
  REQUIRE(cookies::parse_cookie_date("Thu, 01 Jan 1970 00:00:00 GMT", t));
  CHECK(t == 0);
  CHECK_FALSE(cookies::parse_cookie_date("tomorrow", t));
  CHECK_FALSE(cookies::parse_cookie_date("Wed, 32 Oct 2015 07:28:00 GMT", t));
  CHECK_FALSE(cookies::parse_cookie_date("Wed, 21 Foo 2015 07:28:00 GMT", t));
}

TEST_CASE("domain and path matching") {
  CHECK(cookies::domain_match("example.com", "example.com"));
  CHECK(cookies::domain_match("api.example.com", "example.com"));
  CHECK_FALSE(cookies::domain_match("badexample.com", "example.com"));
  CHECK_FALSE(cookies::domain_match("example.com", "api.example.com"));
  CHECK_FALSE(cookies::domain_match("10.0.0.1", "0.0.1"));
  CHECK(cookies::path_match("/", "/"));
  CHECK(cookies::path_match("/api/users", "/api"));
  CHECK(cookies::path_match("/api/users", "/api/"));
  CHECK_FALSE(cookies::path_match("/apiary", "/api"));
  CHECK_FALSE(cookies::path_match("/", "/api"));
  CHECK(cookies::is_public_suffix("com"));
  CHECK(cookies::is_public_suffix("co.uk"));
  CHECK(cookies::is_public_suffix("github.io"));
  CHECK_FALSE(cookies::is_public_suffix("example.com"));
}

TEST_CASE("Set-Cookie is stored by domain / path and sent back on matching requests") {
  CookieJar jar;
  CHECK(jar.set_from_header("https://api.example.com/v1/login", "sid=abc; Path=/; HttpOnly; SameSite=Lax", nullptr, T0));
  CHECK(jar.set_from_header("https://api.example.com/v1/login", "wide=1; Domain=.Example.COM; Path=/", nullptr, T0));
  CHECK(jar.set_from_header("https://api.example.com/v1/login", "deflt=1", nullptr, T0));  // default path /v1
  CHECK(jar.set_from_header("https://api.example.com/v1/login", "deep=1; Path=/v1/users", nullptr, T0 + 1));

  auto l = jar.list(T0);
  REQUIRE(l.size() == 4);
  auto sid = std::find_if(l.begin(), l.end(), [](const Cookie& c) { return c.name == "sid"; });
  REQUIRE(sid != l.end());
  CHECK(sid->host_only);
  CHECK(sid->domain == "api.example.com");
  CHECK(sid->http_only);
  CHECK(sid->same_site == SameSite::Lax);
  CHECK(sid->session());
  auto wide = std::find_if(l.begin(), l.end(), [](const Cookie& c) { return c.name == "wide"; });
  CHECK_FALSE(wide->host_only);
  CHECK(wide->domain == "example.com");
  auto deflt = std::find_if(l.begin(), l.end(), [](const Cookie& c) { return c.name == "deflt"; });
  CHECK(deflt->path == "/v1");

  // Longest path first, then oldest.
  CHECK(jar.cookie_header("https://api.example.com/v1/users/7", T0) == "deep=1; deflt=1; sid=abc; wide=1");
  CHECK(jar.cookie_header("https://api.example.com/other", T0) == "sid=abc; wide=1");
  CHECK(jar.cookie_header("https://www.example.com/", T0) == "wide=1");        // domain cookie only
  CHECK(jar.cookie_header("https://example.com/v1/x", T0) == "wide=1");
  CHECK(jar.cookie_header("https://notexample.com/", T0).empty());
  CHECK(jar.cookie_header("ftp://api.example.com/", T0).empty());
  CHECK(jar.cookie_header("https://API.Example.com:8443/v1?x=1", T0) == "deflt=1; sid=abc; wide=1");

  // Replacing keeps one entry per (name, domain, path).
  CHECK(jar.set_from_header("https://api.example.com/", "sid=xyz; Path=/", nullptr, T0 + 5));
  CHECK(jar.cookie_header("https://api.example.com/", T0 + 5) == "sid=xyz; wide=1");
  CHECK(jar.size(T0) == 4);
}

TEST_CASE("public suffixes, foreign domains and IPs never get domain cookies") {
  CookieJar jar;
  std::string why;
  CHECK_FALSE(jar.set_from_header("https://shop.example.com/", "a=1; Domain=com", &why, T0));
  CHECK(why.find("public suffix") != std::string::npos);
  CHECK_FALSE(jar.set_from_header("https://shop.example.co.uk/", "a=1; Domain=co.uk", nullptr, T0));
  CHECK_FALSE(jar.set_from_header("https://me.github.io/", "a=1; Domain=github.io", nullptr, T0));
  CHECK_FALSE(jar.set_from_header("https://shop.example.com/", "a=1; Domain=other.com", nullptr, T0));
  CHECK_FALSE(jar.set_from_header("https://shop.example.com/", "a=1; Domain=hop.example.com", nullptr, T0));
  CHECK_FALSE(jar.set_from_header("http://10.0.0.5/", "a=1; Domain=0.0.5", nullptr, T0));
  CHECK(jar.size(T0) == 0);
  // Domain equal to a suffix host: host-only (RFC 6265 5.3 step 5).
  CHECK(jar.set_from_header("http://localhost:18080/", "l=1; Domain=localhost", nullptr, T0));
  CHECK(jar.set_from_header("http://10.0.0.5/", "ip=1; Domain=10.0.0.5", nullptr, T0));
  for (const auto& c : jar.list(T0)) CHECK(c.host_only);
  CHECK(jar.cookie_header("http://localhost/", T0) == "l=1");
  CHECK(jar.cookie_header("http://sub.localhost/", T0).empty());
  // Injection attempts: control characters are refused, never sent.
  CHECK_FALSE(jar.set_from_header("https://a.example.com/", std::string("x=1\r\nX-Evil: 1"), nullptr, T0));
  CHECK_FALSE(jar.set_from_header("https://a.example.com/", "=", nullptr, T0));
  CHECK_FALSE(jar.set_from_header("https://a.example.com/", "big=" + std::string(5000, 'v'), nullptr, T0));
}

TEST_CASE("Secure, __Secure- and __Host- rules") {
  CookieJar jar;
  CHECK_FALSE(jar.set_from_header("http://api.example.com/", "s=1; Secure", nullptr, T0));
  CHECK(jar.set_from_header("https://api.example.com/", "s=1; Secure", nullptr, T0));
  CHECK(jar.cookie_header("https://api.example.com/", T0) == "s=1");
  CHECK(jar.cookie_header("http://api.example.com/", T0).empty());
  // An insecure origin cannot shadow it.
  CHECK_FALSE(jar.set_from_header("http://api.example.com/", "s=evil", nullptr, T0));
  // Loopback http counts as secure (as browsers do).
  CHECK(jar.set_from_header("http://127.0.0.1:18080/", "ls=1; Secure", nullptr, T0));
  CHECK(jar.cookie_header("http://127.0.0.1:18080/x", T0) == "ls=1");
  CHECK_FALSE(jar.set_from_header("https://api.example.com/", "__Secure-a=1", nullptr, T0));
  CHECK(jar.set_from_header("https://api.example.com/", "__Secure-a=1; Secure", nullptr, T0));
  CHECK_FALSE(jar.set_from_header("https://api.example.com/", "__Host-a=1; Secure; Domain=example.com; Path=/", nullptr, T0));
  CHECK_FALSE(jar.set_from_header("https://api.example.com/", "__Host-a=1; Secure; Path=/x", nullptr, T0));
  CHECK(jar.set_from_header("https://api.example.com/", "__Host-a=1; Secure; Path=/", nullptr, T0));
}

TEST_CASE("expiry: Max-Age beats Expires, past dates delete") {
  CookieJar jar;
  CHECK(jar.set_from_header("https://e.example.com/", "m=1; Max-Age=60; Expires=Thu, 01 Jan 1970 00:00:00 GMT", nullptr, T0));
  CHECK(jar.set_from_header("https://e.example.com/", "x=1; Expires=Fri, 02 Jan 2026 00:00:00 GMT", nullptr, T0));
  CHECK_FALSE(jar.set_from_header("https://e.example.com/", "old=1; Expires=Thu, 01 Jan 1970 00:00:00 GMT", nullptr, T0));
  auto l = jar.list(T0);
  REQUIRE(l.size() == 2);
  for (const auto& c : l) CHECK_FALSE(c.session());
  CHECK(jar.cookie_header("https://e.example.com/", T0 + 59) == "m=1; x=1");
  CHECK(jar.cookie_header("https://e.example.com/", T0 + 61) == "x=1");          // m expired
  CHECK(jar.cookie_header("https://e.example.com/", T0 + 86401).empty());       // x expired
  CHECK(jar.size(T0) == 0);  // expired cookies were dropped

  CHECK(jar.set_from_header("https://e.example.com/", "d=1", nullptr, T0));
  CHECK(jar.set_from_header("https://e.example.com/", "d=; Max-Age=0", nullptr, T0));  // delete
  CHECK(jar.size(T0) == 0);
}

TEST_CASE("editor API: upsert, remove, remove_domain, clear") {
  CookieJar jar;
  Cookie c;
  c.name = "tok";
  c.value = "1";
  c.domain = ".Example.com";
  c.host_only = false;
  std::string why;
  REQUIRE(jar.upsert(c, &why));
  CHECK(jar.cookie_header("https://a.example.com/") == "tok=1");
  c.value = "2";
  REQUIRE(jar.upsert(c));
  CHECK(jar.cookie_header("https://a.example.com/") == "tok=2");
  CHECK(jar.size() == 1);
  Cookie bad = c;
  bad.domain = "com";
  CHECK_FALSE(jar.upsert(bad, &why));
  bad = c;
  bad.value = "a\r\nb";
  CHECK_FALSE(jar.upsert(bad));
  bad = c;
  bad.path = "x";
  CHECK_FALSE(jar.upsert(bad));
  CHECK(jar.remove("example.com", "/", "tok"));
  CHECK(jar.size() == 0);
  REQUIRE(jar.set_from_header("https://a.example.com/", "a=1"));
  REQUIRE(jar.set_from_header("https://b.a.example.com/", "b=1"));
  REQUIRE(jar.set_from_header("https://other.com/", "o=1"));
  CHECK(jar.remove_domain("a.example.com") == 2);
  CHECK(jar.size() == 1);
  jar.clear();
  CHECK(jar.size() == 0);
}

TEST_CASE("the jar persists sealed under .watt/cookies.json") {
  TempDir t;
  const fs::path file = t.path / ".watt" / "cookies.json";
  {
    CookieJar jar(file);
    REQUIRE(jar.set_from_header("https://p.example.com/", "persist=supersecretvalue123; Max-Age=3600"));
    REQUIRE(jar.set_from_header("https://p.example.com/", "sess=s1"));
    std::string err;
    REQUIRE(jar.flush(&err));
    CHECK(err.empty());
  }
  REQUIRE(fs::exists(file));
  const std::string text = slurp(file);
#ifdef _WIN32
  CHECK(text.find("dpapi1:") != std::string::npos);
  CHECK(text.find("supersecretvalue123") == std::string::npos);
  CHECK(text.find("p.example.com") == std::string::npos);
#endif
  CookieJar again(file);
  CHECK(again.load_rejected().empty());
  CHECK(again.cookie_header("https://p.example.com/") == "persist=supersecretvalue123; sess=s1");
  again.clear();
  REQUIRE(again.flush());
  CookieJar third(file);
  CHECK(third.size() == 0);
}

TEST_CASE("a planted cookies.json (not sealed by this user) is ignored") {
  TempDir t;
  Workspace ws = Workspace::create(t.path);
  const fs::path file = t.path / ".watt" / "cookies.json";
  SECTION("plaintext jar") {
    spit(file, R"({"version":1,"jar":"watt-cookie-jar/v1\n[{\"name\":\"evil\",\"value\":\"1\",\"domain\":\"127.0.0.1\",\"path\":\"/\",\"host_only\":true}]"})");
  }
  SECTION("foreign DPAPI blob") {
    spit(file, R"({"version":1,"jar":"dpapi1:AQAAANCMnd8BFdERjHoAwE/Cl+sBAAAAexample=="})");
  }
  SECTION("not a jar") { spit(file, "[1,2,3]"); }
  CookieJar jar(file);
#ifdef _WIN32
  CHECK_FALSE(jar.load_rejected().empty());
  CHECK(jar.size() == 0);
  CHECK(jar.cookie_header("http://127.0.0.1/").empty());
#endif
  // And through the workspace's own jar: nothing is sent.
  SavedRequest r = ws.new_request();
  r.http.url = "http://127.0.0.1:18080/";
  ws.cookie_jar()->reload();
  auto h = ws.http_to_send(r);
  REQUIRE(h);
#ifdef _WIN32
  CHECK(cookie_of(*h).empty());
#endif
}

TEST_CASE("workspaces have isolated jars; http_to_send and record_cookies use them") {
  TempDir a, b;
  Workspace wa = Workspace::create(a.path);
  Workspace wb = Workspace::create(b.path);
  CHECK(wa.cookie_jar() != wb.cookie_jar());
  CHECK(wa.cookie_jar() == Workspace::create(a.path).cookie_jar());  // one jar per root

  SavedRequest r = wa.new_request();
  r.http.url = "http://127.0.0.1:18080/login";
  HttpResponse resp;
  resp.headers.add("Content-Type", "text/plain");
  resp.headers.add("Set-Cookie", "sid=A1; Path=/");
  resp.headers.add("set-cookie", "theme=dark; Path=/");
  CHECK(wa.record_cookies(r, r.http.url, resp) == 2);

  r.http.url = "http://127.0.0.1:18080/me";
  auto ha = wa.http_to_send(r);
  REQUIRE(ha);
  CHECK(cookie_of(*ha) == "sid=A1; theme=dark");
  SavedRequest rb = wb.new_request();
  rb.http.url = r.http.url;
  auto hb = wb.http_to_send(rb);
  REQUIRE(hb);
  CHECK(cookie_of(*hb).empty());

  // The request's own Cookie header wins for the same name; other jar cookies are appended.
  r.http.headers.emplace_back("Cookie", "sid=mine");
  ha = wa.http_to_send(r);
  REQUIRE(ha);
  CHECK(cookie_of(*ha) == "sid=mine; theme=dark");
  r.http.headers.clear();

  // Per-request opt-out: nothing sent, nothing recorded; the flag round-trips through the file.
  r.no_cookie_jar = true;
  ha = wa.http_to_send(r);
  REQUIRE(ha);
  CHECK(cookie_of(*ha).empty());
  HttpResponse more;
  more.headers.add("Set-Cookie", "x=1");
  CHECK(wa.record_cookies(r, r.http.url, more) == 0);
  REQUIRE(wa.save_request(r));
  auto loaded = Workspace::open(a.path);
  REQUIRE(loaded);
  const std::string saved = slurp(r.file);
  CHECK(saved.find("\"no_cookie_jar\": true") != std::string::npos);
  CHECK(slurp(r.file).find("A1") == std::string::npos);  // cookie values never in tracked files

  // Persisted under the owner-only folder.
  REQUIRE(wa.cookie_jar()->flush());
  CHECK(fs::exists(a.path / ".watt" / "cookies.json"));
  CHECK_FALSE(fs::exists(b.path / ".watt" / "cookies.json"));
  wb.cookie_jar()->flush();
}

TEST_CASE("gRPC-Web: Set-Cookie metadata is recorded and sent back as cookie metadata") {
  TempDir t;
  Workspace ws = Workspace::create(t.path);
  SavedRequest r = ws.new_request(RequestKind::Grpc);
  r.grpc_target.address = "127.0.0.1:50052";
  r.grpc_target.web = true;
  r.grpc_method = "/watt.test.Echo/Unary";
  CHECK(Workspace::grpc_web_url(r) == "http://127.0.0.1:50052/watt.test.Echo/Unary");
  GrpcResponse resp;
  resp.headers.emplace_back("set-cookie", "g=1; Path=/");
  CHECK(ws.record_cookies(r, resp) == 1);
  SavedRequest x = r;
  CHECK(ws.add_grpc_web_cookies(x));
  bool sent = false;
  for (const auto& [k, v] : x.grpc_opts.metadata) sent = sent || (k == "cookie" && v == "g=1");
  CHECK(sent);
  // Plain gRPC: neither recorded nor sent.
  SavedRequest plain = r;
  plain.grpc_target.web = false;
  CHECK(ws.record_cookies(plain, resp) == 0);
  CHECK_FALSE(ws.add_grpc_web_cookies(plain));
  ws.cookie_jar()->flush();
}
