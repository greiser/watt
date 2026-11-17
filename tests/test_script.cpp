// Request scripts (core/script.h): the sandbox and its limits, the trust gate, the Bruno and
// Postman APIs, variables, crypto, sendRequest; plus start-up / per-run cost.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <winsock2.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../tools/httpserver/http_server.h"
#include "core/asserts.h"
#include "core/crypto.h"
#include "core/grpc_client.h"
#include "core/http_client.h"
#include "core/model.h"
#include "core/script.h"

using namespace watt;
namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;

namespace {

fs::path temp_path(const char* prefix) {
  std::random_device rd;
  return fs::temp_directory_path() / (std::string(prefix) + std::to_string(rd()) + std::to_string(rd()));
}

// Never the real per-user config: trust decisions go to a throwaway directory.
struct TestConfigDir {
  fs::path dir = temp_path("watt_test_script_cfg_");
  TestConfigDir() { Workspace::set_user_config_dir(dir); }
  ~TestConfigDir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
} test_config_dir;

struct TempWs {
  fs::path root = temp_path("watt_test_script_ws_");
  Workspace ws;
  explicit TempWs(bool trusted = true) : ws(Workspace::create(root)) {
    Environment dev;
    dev.name = "dev";
    dev.vars["base"] = "http://example.invalid";
    dev.vars["api_secret"] = "hunter2";
    dev.secrets.insert("api_secret");
    ws.environments().push_back(dev);
    ws.active_env = "dev";
    ws.globals["g1"] = "global-one";
    REQUIRE(ws.save_settings());
    if (trusted) REQUIRE(ws.trust_scripts(true));
  }
  ~TempWs() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

httpserver::Server& server() {
  static httpserver::Server s;
  static const bool started = s.start(0);
  REQUIRE(started);
  return s;
}
std::string url(const std::string& path) { return "http://127.0.0.1:" + std::to_string(server().port()) + path; }

SavedRequest http_req(const std::string& pre, const std::string& post = {}) {
  SavedRequest r;
  r.id = "req-1";
  r.name = "Get thing";
  r.http.method = "GET";
  r.http.url = "{{base}}/things?a=1";
  r.http.headers = {{"Accept", "application/json"}, {"X-Off", "1"}};
  r.disabled_headers = {1};
  r.pre_script = pre;
  r.post_script = post;
  return r;
}

ScriptRun pre(TempWs& t, const std::string& code, SavedRequest* out = nullptr, ScriptOptions opts = {}) {
  SavedRequest r = http_req(code);
  ScriptRun run = run_pre_script(t.ws, r, opts);
  if (out) *out = r;
  return run;
}

HttpResponse fake_response() {
  HttpResponse r;
  r.ok = true;
  r.status = 201;
  r.status_text = "Created";
  r.http_version = "HTTP/1.1";
  r.headers.add("Content-Type", "application/json");
  r.headers.add("X-Request-Id", "abc-123");
  r.body = R"({"id":42,"name":"widget","tags":["a","b"],"nested":{"ok":true},"token":"t0k"})";
  r.effective_url = "http://example.invalid/things";
  r.timing.total_ms = 12.5;
  r.timing.dns_ms = 1.0;
  r.timing.ttfb_ms = 8.0;
  return r;
}

ScriptRun post(TempWs& t, const std::string& code, const HttpResponse& resp, ScriptOptions opts = {}) {
  SavedRequest r = http_req({}, code);
  ResponseData rd(resp);
  return run_post_script(t.ws, r, rd, opts);
}

std::string all_logs(const ScriptRun& r) {
  std::string s;
  for (const auto& l : r.logs) s += l.text + "\n";
  return s;
}

int failed_tests(const ScriptRun& r) {
  int n = 0;
  for (const auto& t : r.tests) n += !t.pass;
  return n;
}

std::string failures(const ScriptRun& r) {
  std::string s;
  for (const auto& t : r.tests)
    if (!t.pass) s += t.name + ": " + t.message + "\n";
  return s;
}

double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

// ------------------------------------------------------------------ trust

TEST_CASE("script trust is stored per user outside the workspace; planted files are ignored", "[script][trust]") {
  TempWs t(false);
  CHECK_FALSE(t.ws.scripts_trusted());
  // A cloned repo could carry any of these: none of them may trust the workspace.
  const std::string key = t.root.generic_string();
  const std::string planted = "{\"version\":1,\"workspaces\":{\"" + key + "\":[\"scripts\"]}}";
  for (const fs::path& p : {t.root / ".watt" / "trusted_scripts.json", t.root / "trusted_scripts.json",
                            t.root / ".watt" / "allowed_files.json"}) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << planted;
  }
  {
    std::ofstream(t.root / "watt.json", std::ios::binary)
        << "{\"version\":1,\"trust_scripts\":true,\"scripts_trusted\":true,\"globals\":{}}";
  }
  auto reopened = Workspace::open(t.root);
  REQUIRE(reopened);
  CHECK_FALSE(reopened->scripts_trusted());
  SavedRequest r = http_req("pm.environment.set('pwned', '1')");
  const ScriptRun run = run_pre_script(*reopened, r);
  CHECK(run.untrusted);
  CHECK_FALSE(run.ran);

  // The user's decision, in <user_config_dir()>, keyed by the canonical root.
  REQUIRE(reopened->trust_scripts(true));
  CHECK(reopened->scripts_trusted());
  CHECK(fs::exists(Workspace::user_config_dir() / "trusted_scripts.json"));
  auto again = Workspace::open(t.root);
  REQUIRE(again);
  CHECK(again->scripts_trusted());
  // Another folder (a second clone) is not trusted by this decision.
  TempWs other(false);
  CHECK_FALSE(other.ws.scripts_trusted());
  REQUIRE(reopened->trust_scripts(false));
  CHECK_FALSE(reopened->scripts_trusted());
}

// ------------------------------------------------------------------ sandbox

TEST_CASE("scripts have no file system, process or module access", "[script][sandbox]") {
  TempWs t;
  const ScriptRun run = pre(t, R"(
    test("no std / os modules or libc globals", () => {
      expect(typeof std).to.equal("undefined");
      expect(typeof os).to.equal("undefined");
      expect(typeof scriptArgs).to.equal("undefined");
      expect(typeof print).to.equal("undefined");
      expect(typeof process).to.equal("undefined");
      expect(typeof H).to.equal("undefined");
      expect(typeof XMLHttpRequest).to.equal("undefined");
      expect(typeof fetch).to.equal("undefined");
    });
    test("require only knows the built-ins", () => {
      expect(() => require("fs")).to.throw("not available");
      expect(() => require("child_process")).to.throw("not available");
      expect(require("crypto-js")).to.equal(CryptoJS);
    });
    test("a static import is a syntax error", () => {
      expect(() => eval("import * as os from 'os'")).to.throw();
    });
    let dyn = "pending";
    import("os").then(() => { dyn = "loaded"; }, (e) => { dyn = String(e); });
    setTimeout(() => {
      test("dynamic import() is refused", () => { expect(dyn).to.include("module loading is disabled"); });
    }, 0);
  )");
  INFO(run.error << "\n" << failures(run) << all_logs(run));
  REQUIRE(run.ok);
  CHECK(run.tests.size() == 4);
  CHECK(failed_tests(run) == 0);
}

TEST_CASE("an infinite loop is stopped at the CPU time limit, even inside try/catch", "[script][sandbox]") {
  TempWs t;
  ScriptOptions o;
  o.limits.cpu_ms = 200;
  for (const char* code : {"while (true) {}", "try { for (;;) {} } catch (e) { console.log('caught') }",
                           "async function f() { while (true) await null; } f();",
                           "function again() { setTimeout(again, 0); } again();"}) {
    const auto t0 = std::chrono::steady_clock::now();
    const ScriptRun run = pre(t, code, nullptr, o);
    const double ms = ms_since(t0);
    INFO(code << " -> " << run.error << " in " << ms << " ms");
    CHECK_FALSE(run.ok);
    CHECK_THAT(run.error, ContainsSubstring("CPU time limit (200 ms)"));
    CHECK(all_logs(run).find("caught") == std::string::npos);
    CHECK(ms < 200 + 250);
  }
}

TEST_CASE("a memory bomb is stopped at the memory limit", "[script][sandbox]") {
  TempWs t;
  ScriptOptions o;
  o.limits.memory_bytes = 32u << 20;
  for (const char* code : {"let s = 'x'; for (;;) s += s;", "const a = []; for (;;) a.push(new Array(100000).fill(7));",
                           "const a = []; for (let i = 0;; i++) a.push('k' + i + 'x'.repeat(100));",
                           "const m = new Map(); for (let i = 0;; i++) m.set(i, { i, s: String(i) });",
                           "'x'.repeat(256 * 1024 * 1024)", "new ArrayBuffer(512 * 1024 * 1024)"}) {
    const ScriptRun run = pre(t, code, nullptr, o);
    INFO(code << " -> " << run.error);
    CHECK_FALSE(run.ok);
    // Stopped at the heap limit, or refused up front (a length over QuickJS's maximum).
    const bool limit = run.error.find("memory limit (32 MiB)") != std::string::npos ||
                       run.error.find("RangeError") != std::string::npos ||
                       run.error.find("string too long") != std::string::npos;
    CHECK(limit);
  }
  // The next run starts clean.
  const ScriptRun ok = pre(t, "const a = new Array(1000).fill(1); console.log(a.length)");
  CHECK(ok.ok);
}

TEST_CASE("deep recursion hits the stack limit, not the process stack", "[script][sandbox]") {
  TempWs t;
  const ScriptRun run = pre(t, "function f(n) { return f(n + 1) + 1; } f(0);");
  CHECK_FALSE(run.ok);
  CHECK_THAT(run.error, ContainsSubstring("RangeError"));
  const ScriptRun json = pre(t, "let o = {}; for (let i = 0; i < 100000; i++) o = {o}; JSON.stringify(o);");
  CHECK_FALSE(json.ok);
}

TEST_CASE("console output is capped", "[script][sandbox]") {
  TempWs t;
  ScriptOptions o;
  o.limits.max_log_bytes = 1000;
  const ScriptRun run = pre(t, "for (let i = 0; i < 10000; i++) console.log('line ' + i);", nullptr, o);
  CHECK(run.ok);
  CHECK(run.logs_truncated);
  size_t n = 0;
  for (const auto& l : run.logs) n += l.text.size();
  CHECK(n <= 1000);
  CHECK(run.logs.front().text == "line 0");
}

TEST_CASE("nothing leaks from one run to the next", "[script][sandbox]") {
  TempWs t;
  REQUIRE(pre(t, "globalThis.leak = 1; Object.prototype.polluted = 1; JSON.parse = () => 666; var v = 3;").ok);
  const ScriptRun run = pre(t, R"(
    test("fresh globals", () => {
      expect(typeof leak).to.equal("undefined");
      expect(typeof v).to.equal("undefined");
      expect(({}).polluted).to.equal(undefined);
      expect(JSON.parse("1")).to.equal(1);
    });
  )");
  CHECK(run.ok);
  CHECK(failed_tests(run) == 0);
  CHECK(run.tests.size() == 1);
}

TEST_CASE("timers run before the script returns and never outlive it", "[script][sandbox]") {
  TempWs t;
  const auto t0 = std::chrono::steady_clock::now();
  const ScriptRun run = pre(t, R"(
    setTimeout(() => console.log("b"), 5000);
    setTimeout(() => { console.log("a"); setTimeout(() => console.log("c"), 1); }, 10);
    const id = setTimeout(() => console.log("never"), 1);
    clearTimeout(id);
    console.log("main");
    try { setInterval(() => {}, 10); } catch (e) { console.log(e.message); }
  )");
  CHECK(run.ok);
  CHECK(all_logs(run) == "main\nsetInterval is not available in watt scripts\na\nc\nb\n");
  CHECK(ms_since(t0) < 1000);  // no waiting
}

TEST_CASE("sendRequest goes through HttpClient with the normal send checks", "[script][sandbox][send]") {
  TempWs t;
  const std::string base = url("");
  const ScriptRun run = pre(t, "const BASE = '" + base + R"(';
    let r1, r2, err1, err2, err3, err4, err5;
    pm.sendRequest({ url: BASE + "/echo?x=1", method: "POST", header: { "X-From": "script" }, body: { mode: "raw", raw: "hello" } },
                   (e, res) => { err1 = e; r1 = res; });
    pm.sendRequest("file:///C:/Windows/win.ini", (e) => { err2 = e; });
    pm.sendRequest("ftp://127.0.0.1/x", (e) => { err3 = e; });
    pm.sendRequest({ url: BASE + "/echo", method: "GET\r\nX: y" }, (e) => { err4 = e; });
    pm.sendRequest(BASE + "/status/302?location=file:///C:/Windows/win.ini", (e, res) => { err5 = e; r2 = res; });
    test("http works", () => {
      expect(err1).to.equal(null);
      expect(r1.code).to.equal(200);
      const j = r1.json();
      expect(j.method).to.equal("POST");
      expect(j.body).to.equal("hello");
      expect(j.headers["X-From"]).to.equal("script");
      pm.expect(r1).to.have.status(200);
    });
    test("file:// is refused", () => { expect(String(err2)).to.include("only http:// and https://"); });
    test("ftp:// is refused", () => { expect(String(err3)).to.include("only http:// and https://"); });
    test("bad method is refused", () => { expect(String(err4)).to.include("invalid HTTP method"); });
    test("a redirect to file:// is not followed", () => {
      if (err5) expect(String(err5)).to.match(/protocol|not supported|disabled/i);
      else expect(r2.code).to.equal(302);
    });
  )");
  INFO(run.error << "\n" << failures(run) << all_logs(run));
  REQUIRE(run.ok);
  CHECK(run.tests.size() == 5);
  CHECK(failed_tests(run) == 0);
  CHECK(run.requests_sent == 5);
  CHECK(run.wait_us > 0);
}

TEST_CASE("sendRequest never expands {{variables}} (no secrets or OS environment into script URLs)", "[script][sandbox][send]") {
  TempWs t;
  const ScriptRun run = pre(t, "const BASE = '" + url("") + R"(';
    let body;
    bru.sendRequest({ url: BASE + "/echo", method: "POST", headers: { "X-S": "{{api_secret}}" }, data: "{{api_secret}} {{process.env.PATH}}" },
                    (e, r) => { body = r.data; });
    test("sent literally", () => {
      expect(body.headers["X-S"]).to.equal("{{api_secret}}");
      expect(body.body).to.equal("{{api_secret}} {{process.env.PATH}}");
    });
    test("await works too", async () => {
      const r = await bru.sendRequest({ url: BASE + "/status/418" });
      expect(r.status).to.equal(418);
    });
  )");
  INFO(run.error << "\n" << failures(run));
  REQUIRE(run.ok);
  CHECK(run.tests.size() == 2);
  CHECK(failed_tests(run) == 0);
}

TEST_CASE("sendRequest is limited per run", "[script][sandbox][send]") {
  TempWs t;
  ScriptOptions o;
  o.limits.max_send_requests = 2;
  const ScriptRun run = pre(t, "const u = '" + url("/echo") + "'; for (let i = 0; i < 3; i++) pm.sendRequest(u);", nullptr, o);
  CHECK_FALSE(run.ok);
  CHECK_THAT(run.error, ContainsSubstring("at most 2 requests"));
  // Waiting on the network is not CPU time.
  ScriptOptions o2;
  o2.limits.cpu_ms = 100;
  const ScriptRun slow = pre(t, "pm.sendRequest('" + url("/delay/300") + "', (e, r) => console.log(r.code));", nullptr, o2);
  CHECK(slow.ok);
  CHECK(all_logs(slow) == "200\n");
}

// ------------------------------------------------------------------ pre-request API

TEST_CASE("pre-request scripts edit the request (Bruno API)", "[script][pre]") {
  TempWs t;
  SavedRequest r;
  const ScriptRun run = pre(t, R"(
    req.setHeader("X-Trace", "t-1");
    req.setHeader("accept", "text/plain");
    req.deleteHeader("X-Nope");
    req.setMethod("POST");
    req.setBody({ hello: "world" });
    req.setQueryParam("b", "two words");
    req.setQueryParam("tok", "{{token}}");
    bru.setVar("token", "abc");
    bru.setEnvVar("from_script", 42);
    bru.setGlobalEnvVar("g2", { a: 1 });
    test("reads see this run's writes", () => {
      expect(bru.getVar("token")).to.equal("abc");
      expect(bru.getEnvVar("from_script")).to.equal("42");
      expect(bru.getEnvVar("base")).to.equal("http://example.invalid");
      expect(bru.getGlobalEnvVar("g1")).to.equal("global-one");
      expect(bru.getEnvName()).to.equal("dev");
      expect(req.getHeader("ACCEPT")).to.equal("text/plain");
      expect(req.getQueryParam("a")).to.equal("1");
      expect(bru.interpolate("{{base}}/x/{{token}}")).to.equal("http://example.invalid/x/abc");
    });
  )", &r);
  INFO(run.error << "\n" << failures(run));
  REQUIRE(run.ok);
  CHECK(failed_tests(run) == 0);
  CHECK(run.request_changed);
  CHECK(r.http.method == "POST");
  CHECK(r.http.body == R"({"hello":"world"})");
  CHECK(r.http.url == "{{base}}/things?a=1&b=two%20words&tok={{token}}");
  REQUIRE(r.http.headers.size() == 3);
  CHECK(r.http.headers[0] == Header{"Accept", "text/plain"});
  CHECK(r.http.headers[1] == Header{"X-Trace", "t-1"});
  CHECK(r.http.headers[2] == Header{"X-Off", "1"});  // the disabled row is kept, still disabled
  CHECK(r.disabled_headers == std::set<size_t>{2});

  // Nothing is written until apply_script_vars(); then {{token}} expands in the send.
  CHECK(t.ws.runtime_vars.count("token") == 0);
  ScriptRun applied = run;
  REQUIRE(apply_script_vars(t.ws, applied));
  CHECK(t.ws.runtime_vars["token"] == "abc");
  CHECK(t.ws.environments()[0].vars["from_script"] == "42");
  CHECK(t.ws.globals["g2"] == R"({"a":1})");
  auto http = t.ws.http_to_send(r);
  REQUIRE(http);
  CHECK(http->url == "http://example.invalid/things?a=1&b=two%20words&tok=abc");
  // Persisted (environment / globals), runtime vars are not.
  auto re = Workspace::open(t.root);
  REQUIRE(re);
  CHECK(re->globals["g2"] == R"({"a":1})");
  CHECK(re->runtime_vars.empty());
}

TEST_CASE("pre-request scripts edit the request (Postman API)", "[script][pre]") {
  TempWs t;
  SavedRequest r;
  const ScriptRun run = pre(t, R"(
    pm.request.headers.add({ key: "X-A", value: "1" });
    pm.request.headers.upsert({ key: "Accept", value: "*/*" });
    pm.request.headers.remove("X-A");
    pm.request.headers.add({ key: "X-B", value: "2" });
    pm.request.url.query.add({ key: "q", value: "x&y" });
    pm.request.url.query.remove("a");
    pm.variables.set("v1", "one");
    pm.environment.set("e1", "env-one");
    pm.globals.set("g1", "changed");
    pm.environment.unset("base");
    test("Postman reads", () => {
      pm.expect(pm.variables.get("v1")).to.equal("one");
      pm.expect(pm.variables.get("e1")).to.equal("env-one");
      pm.expect(pm.environment.has("base")).to.be.false;
      pm.expect(pm.globals.get("g1")).to.eql("changed");
      pm.expect(pm.request.headers.get("accept")).to.equal("*/*");
      pm.expect(pm.request.url.getHost()).to.equal("");
      pm.expect(pm.info.requestName).to.equal("Get thing");
      pm.expect(pm.info.eventName).to.equal("prerequest");
      pm.expect(pm.request.method).to.equal("GET");
      pm.expect(pm.response).to.be.undefined;
    });
  )", &r);
  INFO(run.error << "\n" << failures(run));
  REQUIRE(run.ok);
  CHECK(failed_tests(run) == 0);
  CHECK(r.http.url == "{{base}}/things?q=x%26y");
  REQUIRE(r.http.headers.size() == 3);
  CHECK(r.http.headers[0] == Header{"Accept", "*/*"});
  CHECK(r.http.headers[1] == Header{"X-B", "2"});
  REQUIRE(run.vars_set.size() == 4);
  CHECK(run.vars_set[3].removed);
  CHECK(run.vars_set[3].scope == ScriptVar::Scope::Environment);
}

TEST_CASE("script variable writes follow the capture rules", "[script][vars]") {
  TempWs t;
  const ScriptRun run = pre(t, R"(
    pm.environment.set("api_secret", "overwritten");
    bru.deleteEnvVar("api_secret");
    bru.setVar("evil", "{{api_secret}}");
    bru.setVar("", "x");
    bru.setVar("x".repeat(300), "x");
    console.log(bru.getEnvVar("api_secret"));
  )");
  REQUIRE(run.ok);
  REQUIRE(run.vars_set.size() == 5);
  for (const auto& v : run.vars_set) CHECK_FALSE(v.ok);
  CHECK_THAT(run.vars_set[0].message, ContainsSubstring("secret"));
  CHECK_THAT(run.vars_set[2].message, ContainsSubstring("{{"));
  CHECK_THAT(all_logs(run), ContainsSubstring("refused"));
  CHECK_THAT(all_logs(run), ContainsSubstring("hunter2"));  // reading a secret is allowed (HMAC keys live there)
  ScriptRun applied = run;
  REQUIRE(apply_script_vars(t.ws, applied));
  CHECK(t.ws.environments()[0].vars["api_secret"] == "hunter2");
  CHECK(t.ws.runtime_vars.empty());

  // No active environment: environment writes are refused.
  t.ws.active_env.clear();
  const ScriptRun none = pre(t, "pm.environment.set('a', 'b'); bru.setVar('ok', 'yes');");
  REQUIRE(none.vars_set.size() == 2);
  CHECK_FALSE(none.vars_set[0].ok);
  CHECK(none.vars_set[1].ok);
}

TEST_CASE("errors report line and column; on error the request is not changed", "[script][errors]") {
  TempWs t;
  SavedRequest r;
  const ScriptRun run = pre(t, "req.setMethod('PUT');\nlet x = 1;\n  undefinedFunction(x);\n", &r);
  CHECK_FALSE(run.ok);
  CHECK(run.line == 3);
  CHECK(run.column == 3);
  CHECK_THAT(run.error, StartsWith("3:3: ReferenceError"));
  CHECK(r.http.method == "GET");
  CHECK_FALSE(run.request_changed);

  const ScriptRun syn = pre(t, "let a = 1;\nlet b = ;\n");
  CHECK_FALSE(syn.ok);
  CHECK(syn.line == 2);
  CHECK_THAT(syn.error, ContainsSubstring("SyntaxError"));

  const ScriptRun thrown = pre(t, "throw 'plain'");
  CHECK_THAT(thrown.error, ContainsSubstring("plain"));

  const ScriptRun rejected = pre(t, "await Promise.reject(new TypeError('nope'))");
  CHECK_FALSE(rejected.ok);
  CHECK_THAT(rejected.error, ContainsSubstring("TypeError: nope"));

  const ScriptRun bad_header = pre(t, "req.setHeader('X-Bad', 'a\\r\\nInjected: 1')", &r);
  CHECK_FALSE(bad_header.ok);
  CHECK_THAT(bad_header.error, ContainsSubstring("invalid header"));
  const ScriptRun bad_method = pre(t, "req.setMethod('GET /x HTTP/1.1')", &r);
  CHECK_FALSE(bad_method.ok);
  CHECK_THAT(bad_method.error, ContainsSubstring("invalid HTTP method"));
}

// ------------------------------------------------------------------ post-response API

TEST_CASE("post-response scripts read the response (Bruno API)", "[script][post]") {
  TempWs t;
  const ScriptRun run = post(t, R"(
    test("status", () => expect(res.getStatus()).to.equal(201));
    test("body", () => {
      const b = res.getBody();
      expect(b).to.be.an("object");
      expect(b.id).to.equal(42);
      expect(b).to.have.property("name", "widget");
      expect(b.tags).to.eql(["a", "b"]);
      expect(b.tags).to.include("b");
      expect(b.tags).to.have.lengthOf(2);
      expect(b.tags).to.have.length(2);
      expect(res("nested.ok")).to.be.true;
      expect(res("tags[-1]")).to.equal("b");
      expect(res.body.name).to.match(/^wid/);
    });
    test("headers", () => {
      expect(res.getHeader("x-request-id")).to.equal("abc-123");
      expect(res.headers["content-type"]).to.include("json");
    });
    test("timing", () => {
      expect(res.getResponseTime()).to.be.below(100);
      expect(res.getResponseTime()).to.be.above(10);
      expect(res.getTimings().dns).to.equal(1);
      expect(res.getTimings()).to.have.property("ttfb");
      expect(res.getUrl()).to.equal("http://example.invalid/things");
    });
    test("a failing one", () => expect(res.getStatus()).to.equal(200));
    test("not", () => { expect(1).to.not.equal(2); expect([1]).to.not.be.empty; expect(null).to.not.exist; });
    bru.setVar("token", res.getBody().token);
  )", fake_response());
  INFO(run.error << "\n" << failures(run));
  REQUIRE(run.ok);
  REQUIRE(run.tests.size() == 6);
  CHECK(failed_tests(run) == 1);
  CHECK_FALSE(run.tests[4].pass);
  CHECK(run.tests[4].message == "expected 201 to equal 200");
  REQUIRE(run.vars_set.size() == 1);
  CHECK(run.vars_set[0].value == "t0k");
}

TEST_CASE("post-response scripts read the response (Postman API)", "[script][post]") {
  TempWs t;
  const ScriptRun run = post(t, R"(
    pm.test("status code is 201", () => { pm.response.to.have.status(201); });
    pm.test("is success", () => { pm.response.to.be.success; pm.response.to.not.be.error; });
    pm.test("json", () => {
      const j = pm.response.json();
      pm.expect(j.id).to.be.a("number").and.to.equal(42);
      pm.expect(j.nested).to.deep.equal({ ok: true });
      pm.expect(j).to.have.all.keys("id", "name", "tags", "nested", "token");
      pm.expect(j.name).to.be.oneOf(["widget", "gadget"]);
      pm.expect(pm.response.text()).to.include("widget");
    });
    pm.test("headers", () => {
      pm.expect(pm.response.headers.get("Content-Type")).to.eql("application/json");
      pm.response.to.have.header("X-Request-Id", "abc-123");
      pm.expect(pm.response.responseTime).to.be.within(1, 100);
      pm.expect(pm.response.code).to.be.at.least(200);
    });
    pm.test("jsonBody", () => { pm.response.to.have.jsonBody("nested.ok", true); });
    pm.test("async", async () => { await null; pm.expect(1).to.equal(2); });
    pm.environment.set("token", pm.response.json().token);
    console.info("done", { a: 1 }, [1, 2], 3, null, undefined);
  )", fake_response());
  INFO(run.error << "\n" << failures(run));
  REQUIRE(run.ok);
  REQUIRE(run.tests.size() == 6);
  CHECK(failed_tests(run) == 1);
  CHECK(run.tests.back().name == "async");
  CHECK_FALSE(run.tests.back().pass);
  REQUIRE(run.logs.size() == 1);
  CHECK(run.logs[0].level == ScriptLog::Level::Info);
  CHECK(run.logs[0].text == R"(done {"a":1} [1,2] 3 null undefined)");
}

TEST_CASE("post-response scripts on a non-JSON body and on gRPC responses", "[script][post][grpc]") {
  TempWs t;
  HttpResponse html = fake_response();
  html.body = "<html>hi</html>";
  const ScriptRun run = post(t, R"(
    test("text", () => { expect(res.getBody()).to.equal("<html>hi</html>"); });
    test("json() throws", () => { expect(() => pm.response.json()).to.throw("not valid JSON"); });
  )", html);
  CHECK(run.ok);
  CHECK(failed_tests(run) == 0);

  GrpcResponse g;
  g.ok = true;
  g.status_code = 0;
  g.status_name = "OK";
  g.headers = {{"content-type", "application/grpc"}};
  g.trailers = {{"x-trailer", "tv"}};
  GrpcMessageEvent m1, m2;
  m1.json = R"({"text":"one","n":1})";
  m1.wire_bytes = 7;
  m2.json = R"({"text":"two","n":2})";
  m2.wire_bytes = 7;
  g.messages = {m1, m2};
  g.timing.total_ms = 3;
  SavedRequest r;
  r.kind = RequestKind::Grpc;
  r.grpc_method = "/pkg.Svc/Stream";
  r.post_script = R"(
    test("grpc", () => {
      expect(res.getStatus()).to.equal(0);
      expect(res.getStatusText()).to.equal("OK");
      expect(res.getBody().text).to.equal("one");
      expect(res.getMessages().map((m) => m.n)).to.eql([1, 2]);
      expect(res.getTrailer("X-Trailer")).to.equal("tv");
      expect(res.getSize()).to.equal(14);
      expect(req.getMethod()).to.equal("/pkg.Svc/Stream");
    });
  )";
  ResponseData rd(g);
  const ScriptRun gr = run_post_script(t.ws, r, rd);
  INFO(gr.error << "\n" << failures(gr));
  CHECK(gr.ok);
  CHECK(gr.tests.size() == 1);
  CHECK(failed_tests(gr) == 0);
}

TEST_CASE("the response body is parsed once, shared with asserts", "[script][post]") {
  TempWs t;
  HttpResponse resp = fake_response();
  ResponseData rd(resp);
  SavedRequest r = http_req({}, "res.getBody(); pm.response.json(); res.getBody();");
  r.asserts.push_back({true, "res.body.id", "eq", "42"});
  const auto ar = evaluate_asserts(r, rd, &t.ws);
  const double after_asserts = rd.parse_us();
  REQUIRE(ar.size() == 1);
  CHECK(ar[0].pass);
  const ScriptRun run = run_post_script(t.ws, r, rd);
  CHECK(run.ok);
  CHECK(rd.parse_us() == after_asserts);  // no second parse
}

// ------------------------------------------------------------------ crypto

TEST_CASE("crypto helpers and the CryptoJS shim", "[script][crypto]") {
  TempWs t;
  const ScriptRun run = pre(t, R"(
    const fox = "The quick brown fox jumps over the lazy dog";
    test("digests", () => {
      expect(crypto.sha256("abc")).to.equal("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
      expect(crypto.sha1("abc")).to.equal("a9993e364706816aba3e25717850c26c9cd0d89d");
      expect(crypto.md5("")).to.equal("d41d8cd98f00b204e9800998ecf8427e");
      expect(crypto.sha512("abc")).to.match(/^ddaf35a193617aba/);
      expect(crypto.hmac("sha256", "key", fox)).to.equal("f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
      expect(crypto.hmac("md5", "key", fox)).to.equal("80070713463e7749b90c2dc24911e275");
    });
    test("CryptoJS", () => {
      expect(CryptoJS.HmacSHA256(fox, "key").toString()).to.equal("f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
      const b64 = CryptoJS.enc.Base64.stringify(CryptoJS.HmacSHA256(fox, "key"));
      expect(b64).to.equal("97yD9DBThCSxMpjmqm+xQ+9NWaFJRhdZl0edvC0aPNg=");
      expect(CryptoJS.HmacSHA256(fox, "key").toString(CryptoJS.enc.Base64)).to.equal(b64);
      expect(crypto.hmac("sha256", "key", fox, "base64")).to.equal(b64);
      expect(CryptoJS.SHA256("abc").toString(CryptoJS.enc.Hex)).to.equal(crypto.sha256("abc"));
      expect(CryptoJS.MD5("").toString()).to.equal("d41d8cd98f00b204e9800998ecf8427e");
      expect(CryptoJS.enc.Utf8.parse("hé").toString(CryptoJS.enc.Base64)).to.equal("aMOp");
      expect(CryptoJS.enc.Base64.parse("aMOp").toString(CryptoJS.enc.Utf8)).to.equal("hé");
      expect(CryptoJS.enc.Hex.parse("00ff").sigBytes).to.equal(2);
      expect(CryptoJS.lib.WordArray.random(16).sigBytes).to.equal(16);
      expect(CryptoJS.HmacSHA256(fox, CryptoJS.enc.Utf8.parse("key")).toString()).to.equal(crypto.hmac("sha256", "key", fox));
      expect(() => CryptoJS.AES.encrypt("x", "k")).to.throw("not available");
    });
    test("encodings", () => {
      expect(crypto.base64("hello")).to.equal("aGVsbG8=");
      expect(crypto.base64Decode("aGVsbG8=")).to.equal("hello");
      expect(btoa("hello")).to.equal("aGVsbG8=");
      expect(atob("aGVsbG8=")).to.equal("hello");
      expect(crypto.hex("hi")).to.equal("6869");
      expect(Buffer.from("hello").toString("base64")).to.equal("aGVsbG8=");
      expect(Buffer.from("aGVsbG8=", "base64").toString()).to.equal("hello");
      expect(crypto.randomUUID()).to.match(/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
      expect(crypto.randomUUID()).to.not.equal(crypto.randomUUID());
      const u = crypto.getRandomValues(new Uint8Array(8));
      expect(u.length).to.equal(8);
      expect(require("uuid").v4()).to.have.lengthOf(36);
    });
  )");
  INFO(run.error << "\n" << failures(run));
  REQUIRE(run.ok);
  CHECK(run.tests.size() == 3);
  CHECK(failed_tests(run) == 0);
}

TEST_CASE("native crypto helpers", "[crypto]") {
  CHECK(crypto::hex(crypto::digest(crypto::Hash::Sha256, "abc")) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(crypto::hex(crypto::hmac(crypto::Hash::Sha1, "key", "The quick brown fox jumps over the lazy dog")) ==
        "de7c9b85b8b78aa6bc8a7a36f70a90701c9db4d9");
  CHECK(crypto::hex(crypto::hmac(crypto::Hash::Sha256, "", "")) ==
        "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad");
  std::string out;
  CHECK(crypto::unbase64("aGVsbG8", out));
  CHECK(out == "hello");
  CHECK(crypto::base64("\xfb\xff", true, false) == "-_8");
  CHECK_FALSE(crypto::unhex("abc", out));
  CHECK(crypto::uuid_v4().size() == 36);
}

// ------------------------------------------------------------------ storage

TEST_CASE("scripts are saved last, only when present, multi-line as lines", "[script][model]") {
  TempWs t;
  SavedRequest r = http_req({});
  r.name = "plain";
  r.file.clear();
  REQUIRE(t.ws.save_request(r));
  std::ifstream f(r.file, std::ios::binary);
  const std::string plain((std::istreambuf_iterator<char>(f)), {});
  CHECK(plain.find("pre_script") == std::string::npos);
  CHECK(plain.find("post_script") == std::string::npos);
  CHECK(plain.find("auth_scheme") == std::string::npos);

  r.pre_script = "pm.environment.set('a', 1);\nconsole.log('x');";
  r.post_script = "test('one line', () => {});";
  REQUIRE(t.ws.save_request(r));
  std::ifstream g(r.file, std::ios::binary);
  const std::string with((std::istreambuf_iterator<char>(g)), {});
  CHECK(with.find("\"pre_script\"") > with.find("\"http\""));
  CHECK_THAT(with, ContainsSubstring("\"post_script\": \"test('one line', () => {});\""));
  auto back = t.ws.load_request(r.file);
  REQUIRE(back);
  CHECK(back->pre_script == r.pre_script);
  CHECK(back->post_script == r.post_script);
  // Removing the scripts gives the old bytes back.
  r.pre_script.clear();
  r.post_script.clear();
  REQUIRE(t.ws.save_request(r));
  std::ifstream h(r.file, std::ios::binary);
  CHECK(std::string((std::istreambuf_iterator<char>(h)), {}) == plain);
}
