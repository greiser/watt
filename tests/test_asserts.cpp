// Native assertions and captures (core/asserts.h): operators, targets, values, captures and
// chaining, the .req.json format, hostile input and evaluation speed.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "core/asserts.h"
#include "core/model.h"

using namespace watt;
namespace fs = std::filesystem;

namespace {

struct TempDir {
  fs::path path;
  TempDir() {
    std::random_device rd;
    path = fs::temp_directory_path() / ("watt_test_asserts_" + std::to_string(rd()) + std::to_string(rd()));
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

struct TestConfigDir {
  fs::path dir;
  TestConfigDir() {
    std::random_device rd;
    dir = fs::temp_directory_path() / ("watt_test_asserts_cfg_" + std::to_string(rd()));
    Workspace::set_user_config_dir(dir);
  }
  ~TestConfigDir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
} test_config_dir;

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}

void spit(const fs::path& p, const std::string& s) {
  std::ofstream f(p, std::ios::binary);
  f << s;
}

const char* kBody = R"({
  "id": 42, "name": "Ada", "price": 9.5, "active": true, "deleted": false, "nothing": null,
  "tags": ["a", "b", "c"], "empty": [], "obj": {}, "zero": 0, "blank": "",
  "items": [{"id": 1, "sku": "X-1"}, {"id": 2, "sku": "Y-2"}],
  "nested": {"deep": {"key with space": "v", "dash-key": 7, "length": 99}},
  "email": "ada@example.com", "unicode": "h\u00e9llo"
})";

HttpResponse http_response(const std::string& body = kBody) {
  HttpResponse r;
  r.ok = true;
  r.status = 200;
  r.status_text = "OK";
  r.http_version = "HTTP/1.1";
  r.headers.add("Content-Type", "application/json; charset=utf-8");
  r.headers.add("X-Request-Id", "abc-123");
  r.headers.add("Content-Length", "321");
  r.headers.add("X.Dotted", "yes");
  r.body = body;
  r.effective_url = "https://api.example.com/users/42";
  r.timing.dns_ms = 1.5;
  r.timing.connect_ms = 2.5;
  r.timing.tls_ms = 3.5;
  r.timing.ttfb_ms = 20;
  r.timing.download_ms = 4;
  r.timing.total_ms = 33;
  r.prepare_ms = 0.25;
  r.sizes.response_headers = 180;
  r.sizes.response_body = body.size();
  return r;
}

GrpcResponse grpc_response() {
  GrpcResponse g;
  g.ok = false;
  g.status_code = 5;
  g.status_name = "NOT_FOUND";
  g.status_message = "no such user";
  g.headers = {{"content-type", "application/grpc"}, {"x-server", "s1"}};
  g.trailers = {{"grpc-status", "5"}, {"X-Trace", "t-9"}};
  GrpcMessageEvent m1;
  m1.json = "{\n  \"n\": 1,\n  \"user\": {\"name\": \"a\"}\n}";
  m1.wire_bytes = 10;
  GrpcMessageEvent m2;
  m2.json = R"({"n": 2, "list": [1, 2, 3]})";
  m2.wire_bytes = 12;
  g.messages = {m1, m2};
  g.timing.total_ms = 12;
  g.timing.dns_ms = 0.5;
  g.reflect_ms = 3;
  return g;
}

SavedRequest req_with(std::vector<Assertion> as) {
  SavedRequest r;
  r.asserts = std::move(as);
  return r;
}

// Evaluates one assertion; *out gets the result.
template <class Res>
bool one(const std::string& target, const std::string& op, const std::string& value, const Res& res,
         AssertResult* out = nullptr, const Workspace* ws = nullptr) {
  const SavedRequest r = req_with({{true, target, op, value}});
  auto results = evaluate_asserts(r, res, ws);
  REQUIRE(results.size() == 1);
  if (out) *out = results[0];
  if (!results[0].pass) {
    UNSCOPED_INFO(target << " " << op << " " << value << ": " << results[0].message);
  }
  return results[0].pass;
}

bool H(const std::string& t, const std::string& op, const std::string& v = "") {
  static const HttpResponse res = http_response();
  return one(t, op, v, res);
}

}  // namespace

TEST_CASE("every operator", "[asserts][ops]") {
  // eq / neq with each value kind
  CHECK(H("res.status", "eq", "200"));
  CHECK(H("res.status", "", "200"));  // empty op = eq
  CHECK_FALSE(H("res.status", "eq", "201"));
  CHECK(H("res.status", "neq", "500"));
  CHECK_FALSE(H("res.status", "neq", "200"));
  CHECK(H("res.body.name", "eq", "Ada"));
  CHECK(H("res.body.name", "eq", "\"Ada\""));
  CHECK(H("res.body.name", "eq", "'Ada'"));
  CHECK(H("res.body.id", "eq", "42"));
  CHECK(H("res.body.id", "eq", "42.0"));
  CHECK_FALSE(H("res.body.id", "eq", "\"42\""));  // quoted: strings only
  CHECK(H("res.body.price", "eq", "9.5"));
  CHECK(H("res.body.active", "eq", "true"));
  CHECK(H("res.body.deleted", "eq", "false"));
  CHECK(H("res.body.nothing", "eq", "null"));
  CHECK(H("res.body.missing", "eq", "undefined"));
  CHECK(H("res.body.tags", "eq", R"(["a","b","c"])"));
  CHECK(H("res.body.items[0]", "eq", R"({"sku": "X-1", "id": 1})"));
  CHECK_FALSE(H("res.body.items[0]", "eq", R"({"sku": "X-1"})"));
  CHECK(H("res.headers.content-length", "eq", "321"));  // numeric header vs number
  CHECK(H("res.EQ", "isUndefined") == false);           // invalid target never passes
  CHECK(H("res.status", "EQ", "200"));                  // ops are case-insensitive
  // gt gte lt lte
  CHECK(H("res.body.id", "gt", "41"));
  CHECK_FALSE(H("res.body.id", "gt", "42"));
  CHECK(H("res.body.id", "gte", "42"));
  CHECK(H("res.body.id", "lt", "43"));
  CHECK_FALSE(H("res.body.id", "lt", "42"));
  CHECK(H("res.body.id", "lte", "42"));
  CHECK(H("res.headers.content-length", "gt", "300"));
  CHECK_FALSE(H("res.body.name", "gt", "1"));  // not a number
  CHECK_FALSE(H("res.body.id", "gt", "abc"));  // value not a number: message
  // in / notIn
  CHECK(H("res.status", "in", "200, 201, 204"));
  CHECK_FALSE(H("res.status", "in", "201,204"));
  CHECK(H("res.status", "in", "200-299"));
  CHECK(H("res.status", "in", "2xx"));
  CHECK(H("res.body.name", "in", "Bob, 'Ada', Eve"));
  CHECK(H("res.status", "notIn", "404,500"));
  CHECK_FALSE(H("res.status", "notIn", "200,500"));
  // contains / notContains
  CHECK(H("res.body.email", "contains", "@example"));
  CHECK(H("res.body.tags", "contains", "b"));
  CHECK_FALSE(H("res.body.tags", "contains", "z"));
  CHECK(H("res.body.nested", "contains", "deep"));  // object: key
  CHECK(H("res.body.email", "notContains", "gmail"));
  CHECK_FALSE(H("res.body.email", "notContains", "ada"));
  CHECK(H("res.headers.content-type", "contains", "json"));
  CHECK(H("res.body", "contains", "items"));  // an object: has the key
  // length
  CHECK(H("res.body.tags", "length", "3"));
  CHECK(H("res.body.name", "length", "3"));
  CHECK(H("res.body.unicode", "length", "5"));  // code points
  CHECK(H("res.body.obj", "length", "0"));
  CHECK_FALSE(H("res.body.tags", "length", "2"));
  // matches / notMatches
  CHECK(H("res.body.email", "matches", "/^[a-z]+@[a-z.]+$/"));
  CHECK(H("res.body.name", "matches", "/^ADA$/i"));
  CHECK_FALSE(H("res.body.name", "matches", "/^ADA$/"));
  CHECK(H("res.body.name", "matches", "^A"));  // bare pattern
  CHECK(H("res.body.name", "notMatches", "/\\d/"));
  CHECK(H("res.headers.x-request-id", "matches", "[a-z]+-\\d+"));
  // startsWith / endsWith
  CHECK(H("res.body.email", "startsWith", "ada@"));
  CHECK(H("res.body.email", "endsWith", ".com"));
  CHECK_FALSE(H("res.body.email", "endsWith", ".org"));
  CHECK(H("res.url", "startsWith", "https://"));
  // between
  CHECK(H("res.responseTime", "between", "10, 50"));
  CHECK(H("res.body.id", "between", "42,42"));
  CHECK(H("res.body.id", "between", "40 50"));
  CHECK_FALSE(H("res.body.id", "between", "1,10"));
  // emptiness, null / undefined / defined
  CHECK(H("res.body.empty", "isEmpty"));
  CHECK(H("res.body.obj", "isEmpty"));
  CHECK(H("res.body.blank", "isEmpty"));
  CHECK_FALSE(H("res.body.tags", "isEmpty"));
  CHECK(H("res.body.tags", "isNotEmpty"));
  CHECK_FALSE(H("res.body.blank", "isNotEmpty"));
  CHECK(H("res.body.nothing", "isNull"));
  CHECK_FALSE(H("res.body.missing", "isNull"));
  CHECK(H("res.body.missing", "isUndefined"));
  CHECK_FALSE(H("res.body.nothing", "isUndefined"));
  CHECK(H("res.body.nothing", "isDefined"));
  CHECK_FALSE(H("res.body.missing", "isDefined"));
  // truthy / falsy (JavaScript rules)
  CHECK(H("res.body.id", "isTruthy"));
  CHECK(H("res.body.empty", "isTruthy"));  // [] is truthy
  CHECK(H("res.body.zero", "isFalsy"));
  CHECK(H("res.body.blank", "isFalsy"));
  CHECK(H("res.body.nothing", "isFalsy"));
  CHECK(H("res.body.missing", "isFalsy"));
  CHECK(H("res.body.deleted", "isFalsy"));
  CHECK_FALSE(H("res.body.name", "isFalsy"));
  // types
  CHECK(H("res.body", "isJson"));
  CHECK(H("res.body.items[0]", "isJson"));
  CHECK_FALSE(H("res.body.id", "isJson"));
  CHECK(H("res.body.id", "isNumber"));
  CHECK_FALSE(H("res.body.name", "isNumber"));
  CHECK(H("res.body.name", "isString"));
  CHECK(H("res.headers.content-length", "isString"));
  CHECK(H("res.body.active", "isBoolean"));
  CHECK_FALSE(H("res.body.nothing", "isBoolean"));
  CHECK(H("res.body.tags", "isArray"));
  CHECK_FALSE(H("res.body.obj", "isArray"));
  // aliases
  CHECK(H("res.status", "ne", "404"));
  CHECK(H("res.status", ">=", "200"));
  CHECK(H("res.headers.x-request-id", "exists"));
  CHECK(H("res.headers.x-nope", "not_exists"));

  AssertResult r;
  const HttpResponse res = http_response();
  CHECK_FALSE(one("res.status", "frobnicate", "1", res, &r));
  CHECK(r.message.find("unknown operator") != std::string::npos);
  CHECK_FALSE(one("res.body.id", "gt", "abc", res, &r));
  CHECK(r.message.find("expected a number") != std::string::npos);
  CHECK_FALSE(one("res.status", "eq", "404", res, &r));
  CHECK(r.actual == "200");
  CHECK(r.expected == "404");
  CHECK(r.message == "expected res.status to equal 404, got 200");
  CHECK(r.eval_us >= 0);
}

TEST_CASE("every HTTP target kind", "[asserts][targets]") {
  const HttpResponse res = http_response();
  ResponseData d(res);
  auto val = [&](const std::string& t) {
    std::string out, err;
    if (!resolve_target(t, d, out, &err)) return "<" + err + ">";
    return out;
  };
  CHECK(val("res.status") == "200");
  CHECK(val("res.statusText") == "OK");
  CHECK(val("res.responseTime") == "33");
  CHECK(val("res.timing.dns") == "1.5");
  CHECK(val("res.timing.connect") == "2.5");
  CHECK(val("res.timing.tls") == "3.5");
  CHECK(val("res.timing.ttfb") == "20");
  CHECK(val("res.timing.download") == "4");
  CHECK(val("res.timing.total") == "33");
  CHECK(val("res.timing.prepare") == "0.25");
  CHECK(val("res.timing.reflect").find("not measured") != std::string::npos);
  CHECK(val("res.size") == std::to_string(res.body.size()));
  CHECK(val("res.headersSize") == "180");
  CHECK(val("res.url") == "https://api.example.com/users/42");
  CHECK(val("res.headers.content-type") == "application/json; charset=utf-8");
  CHECK(val("res.headers.CONTENT-TYPE") == "application/json; charset=utf-8");
  CHECK(val("res.headers[\"X-Request-Id\"]") == "abc-123");
  CHECK(val("res.headers['x.dotted']") == "yes");
  CHECK(val("res.headers.x-request-id.length") == "7");
  CHECK(val("res.headers").find("\"X-Request-Id\":\"abc-123\"") != std::string::npos);
  CHECK(val("res.body.id") == "42");
  CHECK(val("res.body.name") == "Ada");
  CHECK(val("res.body.tags[0]") == "a");
  CHECK(val("res.body.tags[-1]") == "c");
  CHECK(val("res.body.tags[3]").find("undefined") != std::string::npos);
  CHECK(val("res.body.tags[-4]").find("undefined") != std::string::npos);
  CHECK(val("res.body.tags.length") == "3");
  CHECK(val("res.body.name.length") == "3");
  CHECK(val("res.body.items[1].sku") == "Y-2");
  CHECK(val("res.body.items[-1].id") == "2");
  CHECK(val("res.body[\"items\"][0][\"sku\"]") == "X-1");
  CHECK(val("res.body.nested.deep[\"key with space\"]") == "v");
  CHECK(val("res.body.nested.deep.dash-key") == "7");
  CHECK(val("res.body.nested.deep.length") == "99");  // an object's own "length" key wins
  CHECK(val("res.body.nested.length") == "1");
  CHECK(val("res.body.items[0]") == R"({"id":1,"sku":"X-1"})");
  CHECK(val("res.body.tags") == R"(["a","b","c"])");
  CHECK(val("res.body.nothing") == "null");
  CHECK(val("res.body.unicode") == "h\xC3\xA9llo");
  // gRPC-only targets on HTTP are undefined with a reason
  CHECK(val("res.trailers.x").find("gRPC") != std::string::npos);
  CHECK(val("res.messages").find("gRPC") != std::string::npos);
  // short forms
  CHECK(val("status") == "200");
  CHECK(val("header:X-Request-Id") == "abc-123");
  CHECK(val("json:$.items[1].sku") == "Y-2");
  CHECK(val("json:tags[0]") == "a");
  CHECK(val("time") == "33");
  CHECK(val("time:wait") == "20");

  // Non-JSON body: res.body is the text, paths into it are undefined.
  const HttpResponse html = http_response("<html>hi</html>");
  ResponseData hd(html);
  std::string out, err;
  REQUIRE(resolve_target("res.body", hd, out));
  CHECK(out == "<html>hi</html>");
  CHECK_FALSE(resolve_target("res.body.x", hd, out, &err));
  CHECK(err.find("not JSON") != std::string::npos);
  CHECK(one("res.body", "contains", "hi", html));
  CHECK_FALSE(one("res.body", "isJson", "", html));

  // Syntax errors are reported, never thrown.
  for (const char* bad : {"status.x", "res", "res.", "res.nope", "res.body.", "res.body[", "res.body[1",
                          "res.body[\"x]", "res.body[abc]", "res.body..x", "res.timing", "res.timing.foo",
                          "res.headers[0]", "res.body x", "res.body[99999999999999999999]"}) {
    INFO(bad);
    CHECK_FALSE(check_target(bad).empty());
  }
  CHECK(check_target("res.body.a[0][\"b\"].c[-1].length").empty());
}

TEST_CASE("gRPC targets", "[asserts][targets][grpc]") {
  const GrpcResponse g = grpc_response();
  ResponseData d(g);
  auto val = [&](const std::string& t) {
    std::string out, err;
    if (!resolve_target(t, d, out, &err)) return "<" + err + ">";
    return out;
  };
  CHECK(val("res.status") == "5");
  CHECK(val("res.statusText") == "NOT_FOUND");
  CHECK(val("res.statusMessage") == "no such user");
  CHECK(val("res.messages.length") == "2");
  CHECK(val("res.messages[0].n") == "1");
  CHECK(val("res.messages[0].user.name") == "a");
  CHECK(val("res.messages[-1].list[2]") == "3");
  CHECK(val("res.messages[1].list.length") == "3");
  CHECK(val("res.messages[2].n").find("2 message") != std::string::npos);
  CHECK(val("res.messages") == R"([{"n":1,"user":{"name":"a"}},{"n":2,"list":[1,2,3]}])");
  CHECK(val("res.body.n") == "1");  // the first message
  CHECK(val("res.trailers.x-trace") == "t-9");
  CHECK(val("res.trailers[\"grpc-status\"]") == "5");
  CHECK(val("res.metadata.x-server") == "s1");
  CHECK(val("res.headers.X-Server") == "s1");
  CHECK(val("res.size") == "22");
  CHECK(val("res.timing.reflect") == "3");
  CHECK(val("res.timing.dns") == "0.5");
  CHECK(val("res.responseTime") == "12");
  CHECK(val("res.headersSize").find("undefined") != std::string::npos);
  CHECK(val("trailer:X-Trace") == "t-9");

  CHECK(one("res.status", "eq", "NOT_FOUND", g));
  CHECK(one("res.status", "eq", "not_found", g));
  CHECK(one("res.status", "eq", "5", g));
  CHECK(one("res.status", "eq", "\"NOT_FOUND\"", g));
  CHECK_FALSE(one("res.status", "eq", "OK", g));
  CHECK(one("res.status", "neq", "OK", g));
  CHECK(one("res.status", "in", "OK, NOT_FOUND", g));
  CHECK(one("res.messages", "isArray", "", g));
  CHECK(one("res.messages", "length", "2", g));
  CHECK(one("res.messages[1].list", "contains", "2", g));
  CHECK(one("res.trailers.grpc-status", "eq", "5", g));
  AssertResult r;
  CHECK_FALSE(one("res.status", "eq", "OK", g, &r));
  CHECK(r.actual == "NOT_FOUND (5)");

  // Text-format messages are strings; a call without messages has no body.
  GrpcResponse t = g;
  t.messages.resize(1);
  t.messages[0].json = "n: 1";
  CHECK(one("res.body", "eq", "n: 1", t));
  CHECK(one("res.messages[0]", "isString", "", t));
  GrpcResponse none;
  none.status_name = "OK";
  CHECK(one("res.body", "isUndefined", "", none));
  CHECK(one("res.messages.length", "eq", "0", none));
  CHECK(one("res.status", "eq", "OK", none));

  // A user-paced (Manual) stream: open -> trailers is the user's pace; responseTime is the
  // server's part after the client's last action, res.timing.total stays the wall time.
  GrpcResponse paced = g;
  paced.timing.total_ms = 18479;
  paced.pacing.on = true;
  paced.pacing.sent = 2;
  paced.pacing.first_send_ms = 13168;
  paced.pacing.end_ms = 18478;
  paced.pacing.tail_ms = 0.75;
  CHECK(one("res.responseTime", "lt", "3000", paced));
  CHECK_FALSE(one("res.timing.total", "lt", "3000", paced));
  paced.pacing.on = false;
  CHECK_FALSE(one("res.responseTime", "lt", "3000", paced));
}

TEST_CASE("{{var}} values are expanded before comparison", "[asserts][vars]") {
  TempDir t;
  Workspace ws = Workspace::create(t.path);
  ws.globals["want_status"] = "200";
  ws.globals["name"] = "Ada";
  ws.globals["lo"] = "10";
  ws.globals["pattern"] = "/^ada@/i";
  ws.runtime_vars["tag"] = "b";
  const HttpResponse res = http_response();
  AssertResult r;
  CHECK(one("res.status", "eq", "{{want_status}}", res, &r, &ws));
  CHECK(r.expected == "200");
  CHECK(one("res.body.name", "eq", "\"{{name}}\"", res, nullptr, &ws));
  CHECK(one("res.responseTime", "between", "{{lo}}, 100", res, nullptr, &ws));
  CHECK(one("res.body.email", "matches", "{{pattern}}", res, nullptr, &ws));
  CHECK(one("res.body.tags", "contains", "{{tag}}", res, nullptr, &ws));
  CHECK_FALSE(one("res.body.name", "eq", "{{unknown}}", res, &r, &ws));
  CHECK(r.expected == "{{unknown}}");
  // Without a workspace values are used as written.
  CHECK_FALSE(one("res.status", "eq", "{{want_status}}", res));
  // Disabled and blank rows are skipped.
  SavedRequest sr = req_with({{false, "res.status", "eq", "500"}, {true, "", "", ""}, {true, "res.status", "eq", "200"}});
  const auto results = evaluate_asserts(sr, res, &ws);
  REQUIRE(results.size() == 1);
  CHECK(results[0].a == &sr.asserts[2]);
  CHECK(results[0].pass);
}

TEST_CASE("captures: each scope, chaining, secrets and persistence", "[asserts][captures]") {
  TempDir t;
  {
    Workspace ws = Workspace::create(t.path);
    Environment dev;
    dev.name = "dev";
    dev.vars["base"] = "https://api.example.com";
    dev.vars["password"] = "hunter2";
    dev.secrets.insert("password");
    ws.environments().push_back(dev);
    ws.active_env = "dev";
    REQUIRE(ws.save_settings());

    // Request 1 returns a token; captures put values in all three scopes.
    const HttpResponse login = http_response(R"({"access_token": "tok-123", "user": {"id": 7, "roles": ["a", "b"]},
                                               "expires": 3600, "ok": true, "none": null})");
    SavedRequest r1;
    r1.captures = {
        {true, "token", "res.body.access_token", Capture::Runtime},
        {true, "user_id", "res.body.user.id", Capture::Environment},
        {true, "user", "res.body.user", Capture::Globals},
        {true, "ok", "res.body.ok", Capture::Runtime},
        {true, "none", "res.body.none", Capture::Runtime},
        {true, "req_id", "res.headers.x-request-id", Capture::Environment},
        {true, "missing", "res.body.nope", Capture::Runtime},
        {false, "skipped", "res.status", Capture::Runtime},
        {true, "password", "res.body.access_token", Capture::Environment},
        {true, "password", "res.body.access_token", Capture::Runtime},
        {true, "bad name}}", "res.status", Capture::Runtime},
        {true, "status", "res.bogus", Capture::Runtime},
    };
    const auto cr = apply_captures(r1, login, ws);
    REQUIRE(cr.size() == 11);  // the disabled one is skipped
    CHECK(cr[0].ok);
    CHECK(cr[0].value == "tok-123");
    CHECK(ws.runtime_vars["token"] == "tok-123");
    CHECK(cr[1].ok);
    CHECK(ws.environments()[0].vars["user_id"] == "7");
    CHECK(cr[2].ok);
    CHECK(ws.globals["user"] == R"({"id":7,"roles":["a","b"]})");
    CHECK(ws.runtime_vars["ok"] == "true");
    CHECK(ws.runtime_vars["none"] == "null");
    CHECK(ws.environments()[0].vars["req_id"] == "abc-123");
    CHECK_FALSE(cr[6].ok);
    CHECK(cr[6].message.find("undefined") != std::string::npos);
    CHECK_FALSE(cr[7].ok);  // secret: refused, value untouched
    CHECK(cr[7].message.find("secret") != std::string::npos);
    CHECK_FALSE(cr[8].ok);  // a runtime var must not shadow the secret either
    CHECK(ws.environments()[0].vars["password"] == "hunter2");
    CHECK_FALSE(ws.runtime_vars.count("password"));
    CHECK_FALSE(cr[9].ok);
    CHECK_FALSE(cr[10].ok);
    CHECK(cr[10].message.find("invalid target") != std::string::npos);
    CHECK(cr[0].c == &r1.captures[0]);

    // Request 2 sends {{token}} and {{user_id}}.
    SavedRequest r2;
    r2.http.url = "{{base}}/users/{{user_id}}";
    r2.http.headers = {{"Authorization", "Bearer {{token}}"}};
    std::string err;
    const auto sent = ws.http_to_send(r2, &err);
    REQUIRE(sent);
    CHECK(sent->url == "https://api.example.com/users/7");
    CHECK(sent->headers[0].second == "Bearer tok-123");
    // Runtime vars win over the environment.
    ws.runtime_vars["base"] = "http://localhost";
    CHECK(ws.expand("{{base}}") == "http://localhost");
    ws.runtime_vars.erase("base");

    // A hostile response cannot plant a {{reference}} that the next request would expand.
    const HttpResponse evil = http_response(R"({"t": "{{password}}"})");
    SavedRequest r3;
    r3.captures = {{true, "token", "res.body.t", Capture::Runtime}};
    const auto ev = apply_captures(r3, evil, ws);
    CHECK_FALSE(ev[0].ok);
    CHECK(ws.runtime_vars["token"] == "tok-123");

    // Without an active environment an environment capture fails.
    ws.active_env.clear();
    SavedRequest r4;
    r4.captures = {{true, "x", "res.status", Capture::Environment}};
    const auto ne = apply_captures(r4, login, ws, false);
    CHECK_FALSE(ne[0].ok);
    CHECK(ne[0].message.find("no active environment") != std::string::npos);
    ws.active_env = "dev";
    REQUIRE(ws.save_settings());
  }
  // Environment and global captures were saved; runtime ones and the secret stayed as they were.
  auto ws = Workspace::open(t.path);
  REQUIRE(ws);
  REQUIRE(ws->environments().size() == 1);
  CHECK(ws->environments()[0].vars["user_id"] == "7");
  CHECK(ws->environments()[0].vars["password"] == "hunter2");
  CHECK(ws->globals["user"] == R"({"id":7,"roles":["a","b"]})");
  CHECK(ws->runtime_vars.empty());
  CHECK(slurp(t.path / "environments" / "dev.json").find("hunter2") == std::string::npos);

  // gRPC captures.
  const GrpcResponse g = grpc_response();
  SavedRequest rg;
  rg.captures = {{true, "trace", "res.trailers.x-trace", Capture::Runtime},
                 {true, "last_n", "res.messages[-1].n", Capture::Runtime}};
  const auto gc = apply_captures(rg, g, *ws);
  CHECK(gc[0].ok);
  CHECK(ws->runtime_vars["trace"] == "t-9");
  CHECK(ws->runtime_vars["last_n"] == "2");
}

TEST_CASE("asserts and captures in the .req.json; old files keep their bytes", "[asserts][model]") {
  TempDir t;
  Workspace ws = Workspace::create(t.path);
  const std::string old_text =
      "{\n"
      "  \"version\": 1,\n"
      "  \"id\": \"00000000-0000-4000-8000-000000000009\",\n"
      "  \"name\": \"Old\",\n"
      "  \"kind\": \"http\",\n"
      "  \"http\": {\n"
      "    \"method\": \"GET\",\n"
      "    \"url\": \"https://example.com/x\"\n"
      "  }\n"
      "}\n";
  spit(t.path / "Old.req.json", old_text);
  auto old = ws.load_request(t.path / "Old.req.json");
  REQUIRE(old);
  CHECK(old->asserts.empty());
  CHECK(old->captures.empty());
  REQUIRE(ws.save_request(*old));
  CHECK(slurp(t.path / "Old.req.json") == old_text);

  SavedRequest r;
  r.id = "00000000-0000-4000-8000-000000000010";
  r.name = "New";
  r.http.url = "https://example.com/y";
  r.asserts = {{true, "res.status", "eq", "200"},
               {false, "res.body.id", "isNumber", ""},
               {true, "res.body.name", "eq", " two  spaces "},
               {true, "res.body.x", "", "y"}};
  r.captures = {{true, "token", "res.body.token", Capture::Runtime},
                {false, "uid", "res.body.user.id", Capture::Environment},
                {true, "g", "res.status", Capture::Globals}};
  REQUIRE(ws.save_request(r));
  const std::string text = slurp(r.file);
  const std::string expected =
      "{\n"
      "  \"version\": 1,\n"
      "  \"id\": \"00000000-0000-4000-8000-000000000010\",\n"
      "  \"name\": \"New\",\n"
      "  \"kind\": \"http\",\n"
      "  \"http\": {\n"
      "    \"method\": \"GET\",\n"
      "    \"url\": \"https://example.com/y\"\n"
      "  },\n"
      "  \"asserts\": [\n"
      "    [\"res.status\", \"eq 200\"]\n"
      "  , [\"res.body.id\", \"isNumber\", false]\n"
      "  , [\"res.body.name\", \"eq  two  spaces \"]\n"
      "  , [\"res.body.x\", \" y\"]\n"
      "  ],\n"
      "  \"captures\": [\n"
      "    {\n"
      "      \"var\": \"token\",\n"
      "      \"from\": \"res.body.token\",\n"
      "      \"scope\": \"runtime\"\n"
      "    }\n"
      "  , {\n"
      "      \"var\": \"uid\",\n"
      "      \"from\": \"res.body.user.id\",\n"
      "      \"scope\": \"environment\",\n"
      "      \"enabled\": false\n"
      "    }\n"
      "  , {\n"
      "      \"var\": \"g\",\n"
      "      \"from\": \"res.status\",\n"
      "      \"scope\": \"globals\"\n"
      "    }\n"
      "  ]\n"
      "}\n";
  CHECK(text == expected);
  auto back = ws.load_request(r.file);
  REQUIRE(back);
  CHECK(back->asserts == r.asserts);
  CHECK(back->captures == r.captures);
  // Saving the loaded request again yields the same bytes.
  REQUIRE(ws.save_request(*back));
  CHECK(slurp(r.file) == expected);

  // The [target, op, value] form is read too.
  spit(t.path / "Tri.req.json",
       "{\"version\":1,\"id\":\"t\",\"name\":\"Tri\",\"kind\":\"http\",\"http\":{\"method\":\"GET\",\"url\":\"u\"},"
       "\"asserts\":[[\"status\",\"in\",\"200-299\"],[\"json:$.id\",\"eq\",\"7\",false],5,[\"x\"]],"
       "\"captures\":[{\"var\":\"a\",\"from\":\"res.body.a\"},\"junk\"]}");
  auto tri = ws.load_request(t.path / "Tri.req.json");
  REQUIRE(tri);
  REQUIRE(tri->asserts.size() == 2);
  CHECK(tri->asserts[0] == Assertion{true, "status", "in", "200-299"});
  CHECK(tri->asserts[1] == Assertion{false, "json:$.id", "eq", "7"});
  REQUIRE(tri->captures.size() == 1);
  CHECK(tri->captures[0].scope == Capture::Runtime);
}

TEST_CASE("hostile input: long paths, pathological regexes, deep JSON", "[asserts][security]") {
  const HttpResponse res = http_response();
  AssertResult r;
  // Huge target.
  std::string huge = "res.body";
  while (huge.size() < 100000) huge += ".a";
  CHECK_FALSE(one(huge, "isDefined", "", res, &r));
  CHECK(r.message.find("longer than") != std::string::npos);
  // Too many segments (within the byte limit).
  std::string many = "res.body";
  for (int i = 0; i < 100; ++i) many += "[0]";
  CHECK_FALSE(one(many, "isUndefined", "", res, &r));
  CHECK(r.message.find("segments") != std::string::npos);
  // Control characters and garbage never throw.
  std::mt19937 rng(7);
  for (int i = 0; i < 2000; ++i) {
    std::string t = "res.";
    const int n = static_cast<int>(rng() % 40);
    for (int k = 0; k < n; ++k) t += static_cast<char>("ab.[]\"'-01\\ \x01\xff"[rng() % 15]);
    (void)check_target(t);
    (void)one(t, "isDefined", "", res);
  }

  // Pathological regexes: RE2 matches in linear time, oversized programs are refused.
  const HttpResponse a_lot = http_response("{\"s\": \"" + std::string(200000, 'a') + "\"}");
  const auto t0 = std::chrono::steady_clock::now();
  CHECK_FALSE(one("res.body.s", "matches", "/(a+)+$b/", a_lot));
  CHECK_FALSE(one("res.body.s", "matches", "/^(a|aa)*c$/", a_lot));
  CHECK(one("res.body.s", "matches", "/^(a*)*$/", a_lot));
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  CHECK(ms < 500);
  std::printf("pathological regexes on a 200 KB string: %.2f ms\n", ms);
  CHECK_FALSE(one("res.body.name", "matches", "/((a{100}){100}){100}/", res, &r));
  CHECK_FALSE(r.message.empty());
  CHECK_FALSE(one("res.body.name", "matches", "/" + std::string(5000, 'a') + "/", res, &r));
  CHECK(r.message.find("longer than") != std::string::npos);
  CHECK_FALSE(one("res.body.name", "matches", "/(unclosed/", res, &r));
  CHECK(r.message.find("invalid pattern") != std::string::npos);
  CHECK_FALSE(one("res.body.name", "matches", "/(?<!x)a/", res, &r));  // lookbehind: not RE2

  // Deep JSON: parsed without recursion; walks stop at kMaxJsonDepth.
  const int depth = 100000;
  const HttpResponse deep = http_response(std::string(depth, '[') + "1" + std::string(depth, ']'));
  CHECK(one("res.body", "isArray", "", deep));
  CHECK(one("res.body[0][0][0][0]", "isArray", "", deep));
  CHECK(one("res.body", "length", "1", deep));
  CHECK_FALSE(one("res.body", "eq", "[[1]]", deep));
  CHECK_FALSE(one("res.body", "contains", "1", deep));
  CHECK_FALSE(one("res.body", "matches", "1", deep, &r));
  CHECK(r.message.find("levels deep") != std::string::npos);
  ResponseData dd(deep);
  std::string out, err;
  CHECK_FALSE(resolve_target("res.body", dd, out, &err));
  CHECK(err.find("levels deep") != std::string::npos);
  TempDir t;
  Workspace ws = Workspace::create(t.path);
  SavedRequest rc;
  rc.captures = {{true, "deep", "res.body", Capture::Runtime}};
  CHECK_FALSE(apply_captures(rc, deep, ws)[0].ok);
  // A deep expected value is just compared (and stops at the limit).
  const std::string deep_expect = std::string(5000, '[') + std::string(5000, ']');
  CHECK_FALSE(one("res.body", "eq", deep_expect, deep));
  // Deep gRPC messages.
  GrpcResponse g;
  GrpcMessageEvent m;
  m.json = std::string(depth, '[') + std::string(depth, ']');
  g.messages = {m};
  CHECK(one("res.messages[0]", "isArray", "", g));
  CHECK_FALSE(one("res.messages", "isArray", "", g, &r));
  CHECK(r.message.find("levels deep") != std::string::npos);
  // Malformed JSON body: a text body.
  const HttpResponse broken = http_response("{\"a\": [1, 2");
  CHECK(one("res.body", "isString", "", broken));
  CHECK(one("res.body.a", "isUndefined", "", broken));
}

TEST_CASE("speed: 20 asserts on a 1 MB JSON body", "[asserts][perf]") {
  std::string body = "{\"meta\": {\"total\": 5000, \"page\": 1, \"next\": \"https://api.example.com/items?page=2\"}, \"items\": [";
  for (int i = 0; body.size() < (1 << 20); ++i) {
    if (i) body += ',';
    body += "{\"id\": " + std::to_string(i) + ", \"name\": \"item number " + std::to_string(i) +
            "\", \"price\": " + std::to_string(i % 100) + ".25, \"tags\": [\"x\", \"y\", \"z\"], \"active\": " +
            (i % 2 ? "true" : "false") + ", \"desc\": \"" + std::string(60, 'd') + "\"}";
  }
  body += "], \"status\": \"ok\"}";
  const HttpResponse res = http_response(body);
  SavedRequest r = req_with({
      {true, "res.status", "eq", "200"},
      {true, "res.responseTime", "lt", "500"},
      {true, "res.headers.content-type", "contains", "json"},
      {true, "res.body.meta.total", "eq", "5000"},
      {true, "res.body.meta.next", "startsWith", "https://"},
      {true, "res.body.items", "isArray", ""},
      {true, "res.body.items.length", "gt", "1000"},
      {true, "res.body.items[0].id", "eq", "0"},
      {true, "res.body.items[-1].name", "matches", "/^item number \\d+$/"},
      {true, "res.body.items[100].price", "between", "0, 100"},
      {true, "res.body.items[5].tags", "contains", "y"},
      {true, "res.body.items[5].tags", "length", "3"},
      {true, "res.body.items[7].active", "eq", "true"},
      {true, "res.body.items[8].active", "isFalsy", ""},
      {true, "res.body.status", "in", "ok, done"},
      {true, "res.body.items[3].desc", "isNotEmpty", ""},
      {true, "res.body.missing", "isUndefined", ""},
      {true, "res.body.meta", "isJson", ""},
      {true, "res.body.items[2].name", "endsWith", "2"},
      {true, "res.body.items[9].id", "neq", "10"},
  });
  REQUIRE(r.asserts.size() == 20);
  // Cold: compile the targets / patterns and parse the body.
  {
    ResponseData d(res);
    const auto t0 = std::chrono::steady_clock::now();
    const auto results = evaluate_asserts(r, d, nullptr);
    const double cold_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    for (const auto& x : results) {
      INFO(x.a->target << " " << x.a->op << " " << x.a->value << ": " << x.message);
      CHECK(x.pass);
    }
    std::printf("1 MB body (%zu bytes): parse %.0f us, first evaluation incl. parse %.3f ms\n", body.size(),
                d.parse_us(), cold_ms);
  }
  // Warm: a new response each time (parse once), targets already compiled.
  double best_parse = 1e9, best_eval = 1e9, sum_eval_us = 0;
  for (int k = 0; k < 20; ++k) {
    ResponseData d(res);
    const auto t0 = std::chrono::steady_clock::now();
    const auto results = evaluate_asserts(r, d, nullptr);
    const double total_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    best_parse = std::min(best_parse, d.parse_us());
    best_eval = std::min(best_eval, total_us - d.parse_us());
    double s = 0;
    for (const auto& x : results) s += x.eval_us;
    sum_eval_us = s;
  }
  // Already parsed: evaluate again on the same ResponseData.
  ResponseData d(res);
  evaluate_asserts(r, d, nullptr);
  double best_reuse = 1e9;
  for (int k = 0; k < 50; ++k) {
    const auto t0 = std::chrono::steady_clock::now();
    const auto results = evaluate_asserts(r, d, nullptr);
    best_reuse = std::min(best_reuse,
                          std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
    REQUIRE(results.size() == 20);
  }
  std::printf("20 asserts on 1 MB: parse %.0f us (once per response), evaluation %.1f us "
              "(sum of eval_us %.1f us), re-evaluation on the parsed body %.1f us\n",
              best_parse, best_eval, sum_eval_us, best_reuse);
  CHECK(best_reuse < 1000);
  CHECK(best_eval < 1000);
}
