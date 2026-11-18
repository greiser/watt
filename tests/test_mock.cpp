// Mock server (core/mock_server.h): route paths, matching with wildcards, example selection,
// the 404 listing, loopback-only binding, and a live keep-alive round trip through HttpClient.
#include <catch2/catch_test_macros.hpp>

#include <winsock2.h>

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/http_client.h"
#include "core/mock_server.h"
#include "core/model.h"

using namespace watt;
using namespace watt::mock;

namespace {

Route route(std::string method, std::string path, std::vector<Example> ex = {}) {
  Route r;
  r.method = std::move(method);
  r.path = std::move(path);
  r.source = r.path;
  r.examples = std::move(ex);
  return r;
}

Example example(std::string name, long status, std::string body, uint32_t delay = 0) {
  Example e;
  e.name = std::move(name);
  e.status = status;
  e.body = std::move(body);
  e.delay_ms = delay;
  e.headers = {{"Content-Type", "application/json"}, {"Content-Length", "999"}, {"X-From", "example"}};
  return e;
}

struct ConfigDir {
  std::filesystem::path dir;
  ConfigDir() {
    dir = std::filesystem::temp_directory_path() / ("watt_mock_cfg_" + std::to_string(::GetCurrentProcessId()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    Workspace::set_user_config_dir(dir);
  }
  ~ConfigDir() {
    Workspace::set_user_config_dir({});
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
};

}  // namespace

TEST_CASE("route_path strips scheme, host, base var, query and fragment") {
  CHECK(route_path("http://127.0.0.1:18080/users/:id?x=1#f") == "/users/:id");
  CHECK(route_path("{{base}}/users/{{id}}") == "/users/{{id}}");
  CHECK(route_path("https://api.example.com") == "/");
  CHECK(route_path("api.example.com/v1/items/") == "/v1/items");
  CHECK(route_path("/a//b/") == "/a/b");
  CHECK(route_path("  {{base}}  ") == "/");
}

TEST_CASE("route matching: literal, :param and {{var}} wildcards, method, specificity") {
  std::vector<Route> rs = {route("GET", "/users/:id"), route("GET", "/users/me"), route("POST", "/users"),
                           route("GET", "/items/{{item}}/tags"), route("*", "/health")};
  CHECK(find_route(rs, "GET", "/users/42") == 0);
  CHECK(find_route(rs, "GET", "/users/me") == 1);  // literal beats the wildcard
  CHECK(find_route(rs, "GET", "/users/42/") == 0);  // trailing slash
  CHECK(find_route(rs, "POST", "/users") == 2);
  CHECK(find_route(rs, "DELETE", "/users") == -1);
  CHECK(find_route(rs, "GET", "/users") == -1);
  CHECK(find_route(rs, "GET", "/users/1/2") == -1);
  CHECK(find_route(rs, "GET", "/items/7/tags") == 3);
  CHECK(find_route(rs, "HEAD", "/users/9") == 0);  // HEAD uses the GET route
  CHECK(find_route(rs, "PATCH", "/health") == 4);
  CHECK(find_route(rs, "GET", "/items/7/tags?q=1") == 3);
  CHECK(match_score(rs[0], "GET", "/users/x") < match_score(rs[1], "GET", "/users/me"));
  // An encoded segment matches its decoded literal.
  std::vector<Route> enc = {route("GET", "/a b")};
  CHECK(find_route(enc, "GET", "/a%20b") == 0);
}

TEST_CASE("example selection by query parameter and header") {
  std::vector<Route> rs = {route("GET", "/users/:id", {example("ok", 200, "{\"id\":1}"), example("not found", 404, "{}")})};
  auto r = reply_for(rs, "GET", "/users/1", {});
  CHECK(r.status == 200);
  CHECK(r.body == "{\"id\":1}");
  CHECK(r.example == "ok");
  CHECK(r.route == 0);

  // The default is the first 2xx example even when an error example comes first.
  std::vector<Route> err_first = {route("GET", "/x", {example("boom", 500, "e"), example("fine", 200, "f")})};
  CHECK(reply_for(err_first, "GET", "/x", {}).example == "fine");
  std::vector<Route> only_err = {route("GET", "/x", {example("boom", 500, "e")})};
  CHECK(reply_for(only_err, "GET", "/x", {}).status == 500);

  r = reply_for(rs, "GET", "/users/1?__example=Not%20Found", {});
  CHECK(r.status == 404);
  CHECK(r.example == "not found");

  r = reply_for(rs, "GET", "/users/1?__example=not_found", {});  // file-name spelling
  CHECK(r.status == 404);

  r = reply_for(rs, "GET", "/users/1", {{"X-Mock-Example", "not found"}});
  CHECK(r.status == 404);
  CHECK(r.example == "not found");

  // The query parameter wins over the header.
  r = reply_for(rs, "GET", "/users/1?__example=ok", {{"x-mock-example", "not found"}});
  CHECK(r.status == 200);

  // Unknown name: 404 listing the route's examples.
  r = reply_for(rs, "GET", "/users/1?__example=nope", {});
  CHECK(r.status == 404);
  CHECK(r.example.empty());
  CHECK(r.body.find("\"examples\":[\"ok\",\"not found\"]") != std::string::npos);

  // Framing headers of the example are dropped; the server adds its own.
  r = reply_for(rs, "GET", "/users/1", {});
  bool has_len = false, has_from = false, has_route = false;
  for (const auto& [k, v] : r.headers) {
    has_len |= k == "Content-Length";
    has_from |= k == "X-From" && v == "example";
    has_route |= k == "X-Mock-Route" && v == "GET /users/:id";
  }
  CHECK_FALSE(has_len);
  CHECK(has_from);
  CHECK(has_route);
}

TEST_CASE("delay comes from the example, overridable and capped") {
  std::vector<Route> rs = {route("GET", "/slow", {example("a", 200, "x", 250)})};
  CHECK(reply_for(rs, "GET", "/slow", {}).delay_ms == 250);
  CHECK(reply_for(rs, "GET", "/slow?__delay=5", {}).delay_ms == 5);
  CHECK(reply_for(rs, "GET", "/slow", {{"X-Mock-Delay", "0"}}).delay_ms == 0);
  CHECK(reply_for(rs, "GET", "/slow?__delay=99999999", {}).delay_ms == 60000);
}

TEST_CASE("unmatched requests get a 404 JSON listing the routes; no example gives 501") {
  std::vector<Route> rs = {route("GET", "/users/:id", {example("ok", 200, "{}")}), route("POST", "/orders")};
  auto r = reply_for(rs, "GET", "/nothing", {});
  CHECK(r.status == 404);
  CHECK(r.route == -1);
  CHECK(r.body.find("\"routes\"") != std::string::npos);
  CHECK(r.body.find("\"path\":\"/users/:id\"") != std::string::npos);
  CHECK(r.body.find("\"path\":\"/orders\"") != std::string::npos);
  r = reply_for(rs, "POST", "/orders", {});
  CHECK(r.status == 501);
  CHECK(r.route == 1);
  // A path never reaches the disk: a traversal is just an unmatched path.
  r = reply_for(rs, "GET", "/../../Windows/win.ini", {});
  CHECK(r.status == 404);
}

TEST_CASE("binding is loopback-only unless the user allowed the address") {
  ConfigDir cfg;
  CHECK(is_loopback_address("127.0.0.1"));
  CHECK(is_loopback_address("127.5.0.1"));
  CHECK(is_loopback_address("::1"));
  CHECK(is_loopback_address("[::1]"));
  CHECK(is_loopback_address("localhost"));
  CHECK_FALSE(is_loopback_address("0.0.0.0"));
  CHECK_FALSE(is_loopback_address("192.168.1.10"));
  CHECK(bind_allowed("127.0.0.1"));
  CHECK_FALSE(bind_allowed("0.0.0.0"));

  MockServer s;
  std::string err;
  MockServer::Options o;
  o.address = "0.0.0.0";
  CHECK_FALSE(s.start(o, {}, &err));
  CHECK(err.find("expose") != std::string::npos);
  CHECK_FALSE(s.running());
  o.address = "not-an-ip";
  CHECK_FALSE(s.start(o, {}, &err));

  // A workspace cannot plant the consent: only the per-user file counts.
  CHECK(allow_bind("0.0.0.0", &err));
  CHECK(std::filesystem::exists(cfg.dir / "allowed_mock_bind.json"));
  CHECK(bind_allowed("0.0.0.0"));
  CHECK(disallow_bind("0.0.0.0", &err));
  CHECK_FALSE(bind_allowed("0.0.0.0"));
  CHECK_FALSE(allow_bind("example.com", &err));
}

TEST_CASE("live server: keep-alive round trips, example selection, log, stop") {
  std::vector<Route> rs = {route("GET", "/users/:id", {example("ok", 200, "{\"id\":1}"), example("gone", 410, "{}")}),
                           route("POST", "/users", {example("created", 201, "{\"id\":2}")})};
  std::mutex m;
  std::vector<LogEntry> log;
  MockServer s;
  MockServer::Options o;
  o.on_request = [&](const LogEntry& e) {
    std::lock_guard lk(m);
    log.push_back(e);
  };
  std::string err;
  REQUIRE(s.start(o, rs, &err));
  REQUIRE(s.port() != 0);
  CHECK(s.base_url() == "http://127.0.0.1:" + std::to_string(s.port()));

  HttpClient c;
  HttpRequest q;
  q.url = s.base_url() + "/users/7";
  auto r = c.send(q);
  REQUIRE(r.ok);
  CHECK(r.status == 200);
  CHECK(r.body == "{\"id\":1}");
  CHECK(r.headers.get("X-Mock-Example") == "ok");

  q.url = s.base_url() + "/users/7?__example=gone";
  r = c.send(q);
  CHECK(r.status == 410);
  CHECK(r.timing.connection_reused);  // keep-alive

  q.method = "POST";
  q.url = s.base_url() + "/users";
  q.body = std::string(100000, 'x');
  r = c.send(q);
  CHECK(r.status == 201);

  q.method = "GET";
  q.body.clear();
  q.url = s.base_url() + "/missing";
  r = c.send(q);
  CHECK(r.status == 404);
  CHECK(r.body.find("/users/:id") != std::string::npos);

  // Routes can be replaced while running.
  s.set_routes({route("GET", "/missing", {example("now", 200, "here")})});
  r = c.send(q);
  CHECK(r.status == 200);
  CHECK(r.body == "here");

  {
    std::lock_guard lk(m);
    REQUIRE(log.size() == 5);
    CHECK(log[0].method == "GET");
    CHECK(log[0].target == "/users/7");
    CHECK(log[0].route == "GET /users/:id");
    CHECK(log[0].example == "ok");
    CHECK(log[0].status == 200);
    CHECK(log[0].served_ms >= 0);
    CHECK(log[3].route.empty());
    CHECK(log[3].status == 404);
  }

  // A delayed response is cut short by stop().
  s.set_routes({route("GET", "/slow", {example("slow", 200, "z", 30000)})});
  const auto t0 = std::chrono::steady_clock::now();
  std::thread t([&] {
    HttpClient c2;
    HttpRequest q2;
    q2.url = s.base_url() + "/slow";
    (void)c2.send(q2);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  s.stop();
  t.join();
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10));
  CHECK_FALSE(s.running());
  CHECK(s.base_url().empty());
}
