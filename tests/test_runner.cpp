// Collection runner engine (src/app/runner_engine.cpp, wx-free, compiled in directly) against an
// in-process watt_httpserver: order, iterations, captures feeding later requests, asserts,
// stop, consent, summary percentiles and the JSON / JUnit exports.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <winsock2.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "../tools/httpserver/http_server.h"
#include "app/http_editor_util.cpp"
#include "app/http_editor_pre.cpp"
#include "app/runner_engine.cpp"
#include "app/runner_cli.cpp"

#include <rapidjson/document.h>

using namespace watt;
using namespace watt::app::runner;
namespace fs = std::filesystem;

namespace {

httpserver::Server& server() {
  static httpserver::Server s;
  static const bool started = s.start(0);
  REQUIRE(started);
  return s;
}

std::string base() { return "http://127.0.0.1:" + std::to_string(server().port()); }

struct TempWs {
  fs::path dir;
  fs::path cfg;
  Workspace ws;
  TempWs()
      : dir(fs::temp_directory_path() / ("watt_runner_" + std::to_string(GetCurrentProcessId()) + "_" +
                                         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))),
        cfg(dir.string() + "_cfg"),
        ws(make(dir, cfg)) {}
  ~TempWs() {
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::remove_all(cfg, ec);
    Workspace::set_user_config_dir({});
  }
  static Workspace make(const fs::path& d, const fs::path& c) {
    fs::create_directories(d);
    fs::create_directories(c);
    Workspace::set_user_config_dir(c);
    return Workspace::create(d);
  }
  SavedRequest& add(SavedRequest& r, const std::string& folder) {
    r.file = dir / folder / (r.name + ".req.json");
    fs::create_directories(r.file.parent_path());
    std::string err;
    REQUIRE(ws.save_request(r, &err));
    return r;
  }
};

SavedRequest http(const std::string& name, const std::string& method, const std::string& url) {
  SavedRequest r;
  r.name = name;
  r.kind = RequestKind::Http;
  r.http.method = method;
  r.http.url = url;
  return r;
}

Assertion as(const std::string& t, const std::string& op, const std::string& v) {
  Assertion a;
  a.target = t;
  a.op = op;
  a.value = v;
  return a;
}

std::vector<size_t> all(size_t n) {
  std::vector<size_t> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = i;
  return v;
}

}  // namespace

TEST_CASE("percentiles interpolate between ranks", "[runner]") {
  std::vector<double> v{4, 1, 3, 2};
  CHECK(percentile(v, 50) == Catch::Approx(2.5));
  CHECK(percentile(v, 0) == 1);
  CHECK(percentile(v, 100) == 4);
  std::vector<double> e;
  CHECK(percentile(e, 95) == 0);
  std::vector<double> h;
  for (int i = 1; i <= 100; ++i) h.push_back(i);
  CHECK(percentile(h, 95) == Catch::Approx(95.05));
}

TEST_CASE("xml_escape handles markup and control bytes", "[runner]") {
  CHECK(xml_escape("a<b>&\"c'") == "a&lt;b&gt;&amp;&quot;c&apos;");
  CHECK(xml_escape(std::string("x\x01y\x1f", 4)) == "xy");
  CHECK(xml_escape("l1\nl2") == "l1&#10;l2");
}

TEST_CASE("runs a folder in order with captures feeding later requests", "[runner]") {
  TempWs t;
  // 1. POST /echo: capture the method into a runtime var and the body field into another.
  SavedRequest a = http("1 login", "POST", base() + "/echo");
  a.http.body = "hello";
  a.asserts.push_back(as("res.status", "eq", "200"));
  a.asserts.push_back(as("res.body.method", "eq", "POST"));
  Capture c;
  c.var = "who";
  c.target = "res.body.method";
  a.captures.push_back(c);
  t.add(a, "api");
  // 2. GET /echo?m={{who}}: the captured value reaches the URL.
  SavedRequest b = http("2 use", "GET", base() + "/echo?m={{who}}");
  b.asserts.push_back(as("res.body.query", "contains", "m=POST"));
  t.add(b, "api");
  // 3. /status/500 with an assert that fails.
  SavedRequest f = http("3 broken", "GET", base() + "/status/500");
  f.asserts.push_back(as("res.status", "eq", "200"));
  t.add(f, "api");
  // 4. /status/404 without asserts: failed by status (subfolder order: see the sidebar-order test).
  SavedRequest n = http("4 missing", "GET", base() + "/status/404");
  t.add(n, "api");
  // Outside the folder: never run.
  SavedRequest o = http("other", "GET", base() + "/echo");
  t.add(o, "elsewhere");

  std::vector<std::string> errs;
  auto items = collect(t.ws, t.dir / "api", &errs);
  CHECK(errs.empty());
  REQUIRE(items.size() == 4);
  CHECK(items[0].req.name == "1 login");
  CHECK(items[3].rel == "4 missing");

  Workspace run_ws = t.ws;
  RunControl ctl;
  RunOptions opts;
  opts.iterations = 2;
  std::vector<std::shared_ptr<const StepResult>> seen;
  const auto s = run_plan(items, all(items.size()), opts, run_ws, {}, ctl, [&](auto r) { seen.push_back(r); });
  REQUIRE(seen.size() == 8);
  CHECK(s.total == 8);
  CHECK(s.planned == 8);
  CHECK(s.passed == 4);
  CHECK(s.failed == 4);
  CHECK(s.errors == 0);
  CHECK(s.asserts_passed == 6);
  CHECK(s.asserts_failed == 2);
  CHECK_FALSE(s.stopped);
  CHECK(run_ws.runtime_vars["who"] == "POST");
  CHECK(t.ws.runtime_vars.count("who") == 0);  // the live workspace is untouched
  const StepResult& use = *seen[1];
  CHECK(use.url.find("m=POST") != std::string::npos);
  CHECK(use.asserts_passed == 1);
  CHECK_FALSE(use.failed());
  CHECK(seen[2]->failed());
  CHECK(seen[3]->failed());
  CHECK(seen[3]->asserts.empty());
  CHECK(seen[4]->iteration == 1);
  CHECK(seen[7]->seq == 7);
  // The network phases add up to the response time; Prepare and Queue (watt's setup before
  // the first I/O) to prep_ms, apart from it.
  for (const auto& r : seen) {
    double sum = 0, setup = 0;
    for (const auto& p : r->phases) (is_setup_phase(p) ? setup : sum) += p.ms;
    CHECK(sum == Catch::Approx(r->timing.total_ms).margin(0.01));
    CHECK(setup == Catch::Approx(r->prep_ms).margin(0.01));
    CHECK(r->prep_ms > 0);
    CHECK(r->timing.total_ms > 0);
  }
  CHECK(s.p50_ms > 0);
  CHECK(s.p95_ms >= s.p50_ms);
  CHECK(s.max_ms >= s.p95_ms);

  RunMeta meta;
  meta.name = "api";
  meta.iterations = 2;
  meta.started_iso = now_iso();
  const std::string js = to_json(meta, s, seen);
  rapidjson::Document d;
  d.Parse(js.c_str());
  REQUIRE_FALSE(d.HasParseError());
  CHECK(d["results"].Size() == 8);
  CHECK(d["summary"]["failed"].GetUint64() == 4);
  CHECK(d["results"][2]["asserts"][0]["passed"].GetBool() == false);
  const std::string xml = to_junit(meta, s, seen);
  CHECK(xml.find("<testsuites name=\"api\" tests=\"8\" failures=\"4\" errors=\"0\"") != std::string::npos);
  CHECK(xml.find("api (iteration 2)") != std::string::npos);
  CHECK(xml.find("type=\"assert\"") != std::string::npos);
  CHECK(xml.find("type=\"status\"") != std::string::npos);

  // Run history across restarts: saved per user, read back with the same pass / fail, times and checks.
  meta.label = "rerun failed";
  meta.plan = "4 requests x 2 iterations";
  const std::wstring key = L"test-history-key";
  for (int i = 0; i < 14; ++i) REQUIRE(save_history(key, "2026-11-18T03:04:05Z", history_json(meta, s, seen), 12));
  const auto files = load_history(key, 50);
  CHECK(files.size() == 12);  // the newest 12 kept
  REQUIRE_FALSE(files.empty());
  LoadedRun lr;
  REQUIRE(from_json(files.front(), lr));
  CHECK(lr.meta.label == "rerun failed");
  CHECK(lr.meta.plan == "4 requests x 2 iterations");
  CHECK(lr.meta.iterations == meta.iterations);
  CHECK_FALSE(lr.partial);
  CHECK(lr.summary.passed == 4);
  CHECK(lr.summary.failed == 4);
  CHECK(lr.summary.p95_ms == Catch::Approx(s.p95_ms).margin(0.001));
  CHECK(lr.summary.per_request.size() == 4);
  REQUIRE(lr.steps.size() == seen.size());
  for (size_t i = 0; i < seen.size(); ++i) {
    INFO(i);
    CHECK(lr.steps[i]->passed() == seen[i]->passed());
    CHECK(lr.steps[i]->failed() == seen[i]->failed());
    CHECK(lr.steps[i]->status == seen[i]->status);
    CHECK(lr.steps[i]->rel == seen[i]->rel);
    CHECK(lr.steps[i]->iteration == seen[i]->iteration);
    CHECK(lr.steps[i]->asserts.size() == seen[i]->asserts.size());
    CHECK(lr.steps[i]->timing.total_ms == Catch::Approx(seen[i]->timing.total_ms).margin(0.001));
    CHECK(lr.steps[i]->prep_ms == Catch::Approx(seen[i]->prep_ms).margin(0.001));
    CHECK(lr.steps[i]->phases.size() == seen[i]->phases.size());
    CHECK(lr.steps[i]->reason() == seen[i]->reason());
  }
  CHECK(lr.steps[0]->from_history);
  CHECK(lr.steps[0]->item == lr.steps[4]->item);  // the same request in both iterations
  CHECK(lr.steps[0]->item != lr.steps[1]->item);
  CHECK(load_history(L"another-key", 5).empty());
  LoadedRun bad;
  CHECK_FALSE(from_json("{\"tool\":\"other\"}", bad));
  CHECK_FALSE(from_json("not json", bad));
}

TEST_CASE("stop cancels the request in flight and the delay", "[runner]") {
  TempWs t;
  SavedRequest a = http("slow", "GET", base() + "/delay/3000");
  t.add(a, "");
  auto items = collect(t.ws, {});
  REQUIRE(items.size() == 1);
  RunControl ctl;
  RunOptions opts;
  opts.iterations = 50;
  opts.delay_ms = 10000;
  std::vector<std::shared_ptr<const StepResult>> seen;
  const auto t0 = std::chrono::steady_clock::now();
  std::thread stopper([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    ctl.stop();
  });
  Workspace w = t.ws;
  const auto s = run_plan(items, {0}, opts, w, {}, ctl, [&](auto r) { seen.push_back(r); });
  stopper.join();
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  CHECK(ms < 1500);
  CHECK(s.stopped);
  REQUIRE(seen.size() == 1);
  CHECK(seen[0]->error == "Stopped");
}

TEST_CASE("delay and stop-on-failure", "[runner]") {
  TempWs t;
  SavedRequest a = http("a", "GET", base() + "/status/200");
  SavedRequest b = http("b", "GET", base() + "/status/503");
  SavedRequest c = http("c", "GET", base() + "/status/200");
  t.add(a, "f");
  t.add(b, "f");
  t.add(c, "f");
  auto items = collect(t.ws, t.dir / "f");
  REQUIRE(items.size() == 3);
  RunControl ctl;
  RunOptions opts;
  opts.delay_ms = 60;
  opts.stop_on_failure = true;
  Workspace w = t.ws;
  size_t n = 0;
  const auto s = run_plan(items, {0, 1, 2}, opts, w, {}, ctl, [&](auto) { ++n; });
  CHECK(n == 2);
  CHECK(s.stopped);
  CHECK(s.wall_ms >= 55);
  // A chosen order (the UI's reordered / unchecked list).
  RunControl ctl2;
  std::vector<std::string> names;
  run_plan(items, {2, 0}, {}, w, {}, ctl2, [&](auto r) { names.push_back(r->name); });
  CHECK(names == std::vector<std::string>{"c", "a"});
}

TEST_CASE("a failed response time check reads in ms, not as a raw float", "[runner]") {
  TempWs t;
  SavedRequest a = http("slow", "GET", base() + "/delay/120");
  a.asserts.push_back(as("res.responseTime", "lt", "1"));
  t.add(a, "f");
  auto items = collect(t.ws, t.dir / "f");
  REQUIRE(items.size() == 1);
  RunControl ctl;
  Workspace w = t.ws;
  std::shared_ptr<const StepResult> got;
  run_plan(items, {0}, {}, w, {}, ctl, [&](auto r) { got = r; });
  REQUIRE(got);
  REQUIRE(got->asserts.size() == 1);
  const auto& c = got->asserts[0];
  INFO(c.message);
  CHECK_FALSE(c.pass);
  CHECK(c.message.rfind("expected response time to be < 1 ms, got ", 0) == 0);
  CHECK(c.message.size() > 3);
  CHECK(c.message.substr(c.message.size() - 3) == " ms");
  CHECK(c.actual.find('.') == std::string::npos);  // >= 100 ms: whole ms (watt's precision)
  CHECK(c.expected == "1");                        // the check's label stays as written
}

TEST_CASE("path params are substituted and encoded", "[runner]") {
  TempWs t;
  SavedRequest a = http("p", "GET", base() + "/echo/:id/x");
  a.path_params = {{"id", "{{v}}"}};
  t.ws.runtime_vars["v"] = "a b";
  CHECK(url_with_path_params(a, t.ws) == base() + "/echo/a%20b/x");
}

TEST_CASE("a workspace proxy needs consent once for the whole run", "[runner][network]") {
  TempWs t;
  FolderSettings coll;
  coll.network.proxy = NetworkSettings::Proxy::Url;
  coll.network.proxy_url = "http://10.0.0.5:8080";
  REQUIRE(t.ws.save_collection_settings(coll));
  SavedRequest ra = http("a", "GET", base() + "/echo"), rb = http("b", "GET", base() + "/echo");
  t.add(ra, "");
  t.add(rb, "");
  auto items = collect(t.ws, {});
  REQUIRE(items.size() == 2);
  const Consent c = consent_needed(t.ws, items, all(items.size()));
  REQUIRE(c.network.size() == 1);  // asked once, not per request
  CHECK(c.network[0].proxy_origin == "http://10.0.0.5:8080");
  CHECK_FALSE(c.empty());
  // Not approved: neither request is sent (fail closed), the reason names the proxy.
  Workspace w = t.ws;
  RunControl ctl;
  std::vector<std::shared_ptr<const StepResult>> seen;
  run_plan(items, all(items.size()), {}, w, {}, ctl, [&](auto r) { seen.push_back(r); });
  REQUIRE(seen.size() == 2);
  for (const auto& r : seen) {
    CHECK_FALSE(r->sent);
    CHECK(r->error.find("10.0.0.5:8080") != std::string::npos);
  }
  REQUIRE(t.ws.allow_network_item(c.network[0]));
  CHECK(consent_needed(t.ws, items, all(items.size())).network.empty());
}

TEST_CASE("uploads and env vars outside the workspace need consent up front", "[runner]") {
  TempWs t;
  const fs::path outside = t.cfg / "secret.txt";
  { std::ofstream(outside) << "top secret"; }
  SavedRequest a = http("upload", "POST", base() + "/echo");
  a.body_file = outside.string();
  t.add(a, "");
  _putenv_s("WATT_RUNNER_TEST_VAR", "leak");
  SavedRequest e = http("env", "GET", base() + "/echo?x={{process.env.WATT_RUNNER_TEST_VAR}}");
  t.add(e, "");
  auto items = collect(t.ws, {});
  REQUIRE(items.size() == 2);
  const Consent c = consent_needed(t.ws, items, all(items.size()));
  REQUIRE(c.files.size() == 1);
  CHECK(c.files[0].filename() == "secret.txt");
  REQUIRE(c.env_vars.size() == 1);
  CHECK(c.env_vars[0] == "WATT_RUNNER_TEST_VAR");

  // Not approved: neither is sent, and nothing leaks.
  {
    Workspace w = t.ws;
    RunControl ctl;
    std::vector<std::shared_ptr<const StepResult>> seen;
    const auto s = run_plan(items, all(items.size()), {}, w, {}, ctl, [&](auto r) { seen.push_back(r); });
    REQUIRE(seen.size() == 2);
    for (const auto& r : seen) {
      INFO(r->name << ": " << r->error);
      CHECK_FALSE(r->sent);
      CHECK(r->error.rfind("Not sent", 0) == 0);
      CHECK(r->failed());
    }
    CHECK(s.errors == 2);
  }
  // Approved once for the whole run: both go out.
  REQUIRE(t.ws.allow_file(c.files[0]));
  REQUIRE(t.ws.allow_env_var(c.env_vars[0]));
  CHECK(consent_needed(t.ws, items, all(items.size())).empty());
  Workspace w = t.ws;
  RunControl ctl;
  std::vector<std::shared_ptr<const StepResult>> seen;
  run_plan(items, all(items.size()), {}, w, {}, ctl, [&](auto r) { seen.push_back(r); });
  REQUIRE(seen.size() == 2);
  const StepResult& up = seen[0]->name == "upload" ? *seen[0] : *seen[1];
  const StepResult& env = seen[0]->name == "upload" ? *seen[1] : *seen[0];
  INFO(up.error << " | " << env.error);
  CHECK(up.sent);
  CHECK(up.body_preview.find("top secret") != std::string::npos);
  CHECK(env.url.find("x=leak") != std::string::npos);
}

TEST_CASE("collect refuses paths outside the workspace", "[runner]") {
  TempWs t;
  SavedRequest a = http("a", "GET", base() + "/echo");
  t.add(a, "");
  std::vector<std::string> errs;
  auto items = collect(t.ws, t.cfg, &errs);
  CHECK(items.empty());
  CHECK(errs.size() == 1);
  // A single request file runs alone.
  auto one = collect(t.ws, a.file);
  REQUIRE(one.size() == 1);
  CHECK(one[0].rel == "a");
}

TEST_CASE("steps keep the request as sent, response headers and a large body when failed", "[runner]") {
  TempWs t;
  SavedRequest a = http("post", "POST", base() + "/echo?x={{v}}");
  a.http.body = "{\"sku\":{{v}}}";
  a.http.headers.push_back({"X-Trace", "t-{{v}}"});
  a.asserts.push_back(as("res.status", "eq", "201"));  // fails: keeps the full body
  t.add(a, "");
  SavedRequest big = http("big", "GET", base() + "/bytes/300000");
  big.asserts.push_back(as("res.status", "eq", "500"));  // fails
  t.add(big, "");
  SavedRequest ok = http("okbig", "GET", base() + "/bytes/300000");
  t.add(ok, "");
  auto items = collect(t.ws, {});
  REQUIRE(items.size() == 3);
  Workspace w = t.ws;
  w.runtime_vars["v"] = "42";
  RunControl ctl;
  std::vector<std::shared_ptr<const StepResult>> seen;
  run_plan(items, all(3), {}, w, {}, ctl, [&](auto r) { seen.push_back(r); });
  REQUIRE(seen.size() == 3);
  auto by = [&](const std::string& n) -> const StepResult& {
    for (const auto& r : seen)
      if (r->name == n) return *r;
    FAIL("no step " << n);
    return *seen[0];
  };
  const StepResult& p = by("post");
  CHECK(p.method == "POST");
  CHECK(p.req_body == "{\"sku\":42}");
  CHECK(p.req_body_size == 10);
  bool trace = false;
  for (const auto& [k, v] : p.req_headers) trace = trace || (k == "X-Trace" && v == "t-42");
  CHECK(trace);
  CHECK(p.content_type().find("application/json") != std::string::npos);
  CHECK(p.http_version.find("HTTP/") == 0);
  CHECK(p.bytes_received > p.size);
  CHECK(p.body_preview.find("\"method\":\"POST\"") != std::string::npos);
  // A failed step keeps up to 256 KB; the first passing ones too.
  CHECK(by("big").failed());
  CHECK(by("big").body_preview.size() == kBodyKeep);
  CHECK(by("big").body_truncated);
  CHECK(by("okbig").passed());
  CHECK(by("okbig").body_preview.size() == kBodyKeep);
}

TEST_CASE("stopped and skipped steps are neither passed nor failed", "[runner]") {
  StepResult a;
  a.sent = true;
  a.transport_ok = false;
  a.error = "Stopped";
  CHECK_FALSE(a.failed());
  CHECK_FALSE(a.passed());
  CHECK_FALSE(a.errored());
  std::vector<RunItem> items(2);
  items[0].req.name = "a";
  items[1].req.name = "b";
  std::vector<bool> done = {true, false, false, false};
  auto sk = skipped_steps(items, {0, 1}, 2, done, 1, 100);
  REQUIRE(sk.size() == 3);
  CHECK(sk[0]->name == "b");
  CHECK(sk[0]->iteration == 0);
  CHECK(sk[2]->iteration == 1);
  CHECK(sk[2]->seq == 3);
  auto one = std::make_shared<StepResult>(a);
  std::vector<std::shared_ptr<const StepResult>> all_steps{one};
  all_steps.insert(all_steps.end(), sk.begin(), sk.end());
  const RunSummary s = summarize(all_steps, 4, 10, true);
  CHECK(s.failed == 0);
  CHECK(s.passed == 0);
  CHECK(s.stopped_steps == 1);
  CHECK(s.skipped == 3);
}

TEST_CASE("summary statistics and the per-request table", "[runner]") {
  std::vector<std::shared_ptr<const StepResult>> v;
  const double times[] = {1, 2, 3, 4, 10};
  for (int i = 0; i < 5; ++i) {
    auto s = std::make_shared<StepResult>();
    s->seq = static_cast<size_t>(i);
    s->item = static_cast<size_t>(i % 2);
    s->name = i % 2 ? "b" : "a";
    s->sent = s->transport_ok = true;
    s->status = i == 4 ? 500 : 200;
    s->timing.total_ms = times[i];
    s->timing.ttfb_ms = 1;
    s->timing.dns_ms = 2;
    s->timing.connection_reused = i > 0;
    s->bytes_received = 100;
    v.push_back(s);
  }
  const RunSummary s = summarize(v, 5, 30, false, true);
  CHECK(s.avg_ms == Catch::Approx(4));
  CHECK(s.stddev_ms == Catch::Approx(std::sqrt(10.0)));
  CHECK(s.min_ms == 1);
  CHECK(s.max_ms == 10);
  CHECK(s.avg_ttfb_ms == Catch::Approx(1));
  CHECK(s.new_connections == 1);
  CHECK(s.avg_dns_ms == Catch::Approx(2));
  CHECK(s.bytes_received == 500);
  REQUIRE(s.per_request.size() == 2);
  CHECK(s.per_request[0].name == "a");
  CHECK(s.per_request[0].count == 3);
  CHECK(s.per_request[0].failures == 1);
  CHECK(s.per_request[0].max_ms == 10);
  CHECK(s.per_request[0].p50_ms == Catch::Approx(3));
  CHECK(s.per_request[1].count == 2);
  CHECK(s.per_request[1].avg_ms == Catch::Approx(3));
}

TEST_CASE("data files: CSV and JSON rows become variables per iteration", "[runner]") {
  std::vector<DataRow> rows;
  std::string err;
  REQUIRE(parse_data("id,name\r\n1,\"Smith, J\"\r\n2,\"say \"\"hi\"\"\"\n", false, &rows, &err));
  REQUIRE(rows.size() == 2);
  CHECK(rows[0][0] == std::pair<std::string, std::string>{"id", "1"});
  CHECK(rows[0][1].second == "Smith, J");
  CHECK(rows[1][1].second == "say \"hi\"");
  REQUIRE(parse_data("[{\"id\": 7, \"tag\": \"x\", \"o\": {\"a\": 1}}]", true, &rows, &err));
  REQUIRE(rows.size() == 1);
  CHECK(rows[0][0].second == "7");
  CHECK(rows[0][2].second == "{\"a\":1}");
  CHECK_FALSE(parse_data("{\"id\": 1}", true, &rows, &err));
  CHECK_FALSE(parse_data("a,b\n\"open", false, &rows, &err));
  CHECK_FALSE(parse_data("a,b\n", false, &rows, &err));

  TempWs t;
  SavedRequest a = http("d", "GET", base() + "/echo?id={{id}}");
  a.asserts.push_back(as("res.body.query", "contains", "id={{id}}"));
  t.add(a, "");
  auto items = collect(t.ws, {});
  RunOptions o;
  REQUIRE(parse_data("id\n10\n20\n30\n", false, &o.data, &err));
  o.iterations = 4;  // more iterations than rows: the last row repeats
  Workspace w = t.ws;
  RunControl ctl;
  std::vector<std::string> urls;
  const auto s = run_plan(items, {0}, o, w, {}, ctl, [&](auto r) { urls.push_back(r->url); });
  REQUIRE(urls.size() == 4);
  CHECK(urls[0].find("id=10") != std::string::npos);
  CHECK(urls[2].find("id=30") != std::string::npos);
  CHECK(urls[3].find("id=30") != std::string::npos);
  CHECK(s.passed == 4);
}

TEST_CASE("capture order hints: a {{var}} used before its capture", "[runner]") {
  std::vector<RunItem> items(3);
  items[0].req.name = "Get order";
  items[0].req.http.url = "http://h/orders/{{ firstId }}?u={{user}}&g={{$guid}}";
  items[1].req.name = "List items";
  items[1].req.http.url = "http://h/items";
  Capture c;
  c.var = "firstId";
  c.target = "res.body[0].id";
  items[1].req.captures.push_back(c);
  items[2].req.name = "Item";
  items[2].req.http.headers.push_back({"X-Id", "{{firstId}}"});
  // Get order, List items, Item: Get order runs before the capture.
  auto h = capture_order_hints(items, {0, 1, 2});
  REQUIRE(h.size() == 1);
  CHECK(h[0].user == 0);
  CHECK(h[0].capture == 1);
  CHECK(h[0].var == "firstId");
  // Capture first: nothing to say.
  CHECK(capture_order_hints(items, {1, 0, 2}).empty());
  // A disabled capture captures nothing.
  items[1].req.captures[0].enabled = false;
  CHECK(capture_order_hints(items, {0, 1, 2}).empty());
  // gRPC metadata counts too; max caps the list.
  items[1].req.captures[0].enabled = true;
  items[2].req.kind = RequestKind::Grpc;
  items[2].req.grpc_opts.metadata.push_back({"x-id", "{{firstId}}"});
  CHECK(capture_order_hints(items, {2, 0, 1}).size() == 2);
  CHECK(capture_order_hints(items, {2, 0, 1}, 1).size() == 1);
}

TEST_CASE("tag filter includes and excludes", "[runner]") {
  RunItem it;
  it.req.name = "Login";
  it.req.docs = "Signs in. #smoke #Auth\nnot#atag";
  it.rel = "users/login";
  const auto tags = tags_of(it);
  CHECK(tags == std::vector<std::string>{"smoke", "auth", "users"});
  CHECK(tag_filter_match(it, ""));
  CHECK(tag_filter_match(it, "smoke"));
  CHECK(tag_filter_match(it, "#SMOKE users"));
  CHECK_FALSE(tag_filter_match(it, "smoke -auth"));
  CHECK_FALSE(tag_filter_match(it, "slow"));
  CHECK(tag_filter_match(it, "-slow"));
  CHECK(tag_filter_match(it, "logi"));  // name substring
}

TEST_CASE("parallel workers run every iteration once and stop together", "[runner]") {
  TempWs t;
  SavedRequest a = http("a", "GET", base() + "/delay/20");
  SavedRequest b = http("b", "GET", base() + "/echo");
  t.add(a, "");
  t.add(b, "");
  auto items = collect(t.ws, {});
  REQUIRE(items.size() == 2);
  RunOptions o;
  o.iterations = 16;
  o.concurrency = 4;
  Workspace w = t.ws;
  RunControl ctl;
  std::mutex m;
  std::vector<std::shared_ptr<const StepResult>> seen;
  const auto t0 = std::chrono::steady_clock::now();
  const auto s = run_plan(items, {0, 1}, o, w, {}, ctl, [&](auto r) {
    std::lock_guard lk(m);
    seen.push_back(r);
  });
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  CHECK(s.total == 32);
  CHECK(s.passed == 32);
  CHECK(ms < 16 * 20);  // 4 at a time: well under the serial 320+ ms
  std::vector<int> per_iter(16);
  std::vector<bool> seqs(32);
  for (const auto& r : seen) {
    ++per_iter[static_cast<size_t>(r->iteration)];
    REQUIRE(r->seq < 32);
    CHECK_FALSE(seqs[r->seq]);
    seqs[r->seq] = true;
  }
  for (int n : per_iter) CHECK(n == 2);

  // Stop reaches every worker's request in flight.
  SavedRequest slow = http("slow", "GET", base() + "/delay/3000");
  t.add(slow, "s");
  auto si = collect(t.ws, t.dir / "s");
  RunOptions o2;
  o2.iterations = 8;
  o2.concurrency = 4;
  RunControl ctl2;
  std::thread stopper([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    ctl2.stop();
  });
  const auto t1 = std::chrono::steady_clock::now();
  const auto s2 = run_plan(si, {0}, o2, w, {}, ctl2, {});
  stopper.join();
  CHECK(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count() < 1500);
  CHECK(s2.stopped);
  CHECK(s2.stopped_steps == 4);
  CHECK(s2.failed == 0);
}

TEST_CASE("HTML report escapes everything and masks credentials", "[runner]") {
  auto s = std::make_shared<StepResult>();
  s->name = "<script>x</script>";
  s->rel = "a/<b>";
  s->method = "GET";
  s->sent = s->transport_ok = true;
  s->status = 500;
  s->req_headers = {{"Authorization", "Bearer SECRET1"}, {"X-Api-Key", "SECRET2"}, {"Accept", "*/*"}};
  s->body_preview = "<img src=x onerror=alert(1)>";
  std::vector<std::shared_ptr<const StepResult>> v{s};
  RunMeta meta;
  meta.name = "n<i>";
  const RunSummary sum = summarize(v, 1, 5, false);
  const std::string h = to_html(meta, sum, v);
  CHECK(h.find("<script>") == std::string::npos);
  CHECK(h.find("<img") == std::string::npos);
  CHECK(h.find("&lt;img src=x") != std::string::npos);
  CHECK(h.find("SECRET1") == std::string::npos);
  CHECK(h.find("SECRET2") == std::string::npos);
  CHECK(h.find("Accept: */*") != std::string::npos);
  CHECK(h.find("n&lt;i&gt;") != std::string::npos);
  CHECK(sensitive_header("Cookie"));
  CHECK_FALSE(sensitive_header("Content-Type"));
}

TEST_CASE("headless run writes reports and returns the exit code", "[runner]") {
  TempWs t;
  SavedRequest a = http("ok", "GET", base() + "/echo");
  a.asserts.push_back(as("res.status", "eq", "200"));
  SavedRequest b = http("bad", "GET", base() + "/status/500");
  t.add(a, "api");
  t.add(b, "api");
  const fs::path junit = t.cfg / "out.xml", html = t.cfg / "out.html";
  std::string out;
  const auto u = [](const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
  };
  int code = run_cli({"--run", u(t.dir / "api"), "--workspace", u(t.dir), "--reporter-junit", u(junit), "--reporter-html",
                      u(html), "--iterations", "2"},
                     {}, &out);
  INFO(out);
  CHECK(code == 1);
  CHECK(out.find("2 passed, 2 failed") != std::string::npos);
  CHECK(out.find("average first byte (TTFB) ") != std::string::npos);  // Newman's averages
  CHECK(fs::file_size(junit) > 100);
  CHECK(fs::file_size(html) > 1000);
  out.clear();
  code = run_cli({"--run", u(t.dir / "api"), "--workspace", u(t.dir), "--filter", "-bad"}, {}, &out);
  CHECK(code == 0);
  out.clear();
  CHECK(run_cli({"--run", u(t.dir / "api"), "--bogus"}, {}, &out) == 2);
  CHECK(run_cli({"--run", u(t.dir / "api"), "--workspace", u(t.dir), "--env", "nope"}, {}, &out) == 2);
}

TEST_CASE("tidy_numbers shortens long doubles only", "[runner]") {
  CHECK(tidy_numbers("got 122.28609999999999") == "got 122.2861");
  CHECK(tidy_numbers("expected 0.30000000000000004 to equal 0.3") == "expected 0.3 to equal 0.3");
  CHECK(tidy_numbers("id 12345678901234567") == "id 12345678901234567");
  CHECK(tidy_numbers("v1.2.3 and 1.5") == "v1.2.3 and 1.5");
  CHECK(tidy_numbers("") == "");
}

namespace {
SavedRequest scripted_pair_a() {
  SavedRequest a = http("1 login", "POST", base() + "/echo");
  a.http.body = "x";
  a.post_script =
      "console.log('logged in');\n"
      "bru.setVar('token', 'abc');\n"
      "pm.test('status is 200', () => pm.response.to.have.status(200));\n"
      "test('is wrong on purpose', () => expect(1).to.equal(2));\n";
  return a;
}
}  // namespace

TEST_CASE("scripts run in the runner: tests, variables, console, phases", "[runner][scripts]") {
  TempWs t;
  REQUIRE(t.ws.trust_scripts(true));
  SavedRequest a = scripted_pair_a();
  t.add(a, "api");
  // The pre-request script signs with the token the first request's script set.
  SavedRequest b = http("2 signed", "GET", base() + "/echo?n={{nonce}}");
  b.pre_script = "req.setHeader('X-Sig', bru.getVar('token') + '-signed');";
  b.post_script =
      "const h = res.getBody().headers;\n"
      "test('signature sent', () => expect(h['X-Sig'] || h['x-sig']).to.equal('abc-signed'));\n";
  // Scripts tab pre-request variable.
  b.docs = "<!-- watt:descriptions (watt editor data)\npre\tnonce\t'n-' + 42\truntime\ton\n-->";
  b.asserts.push_back(as("res.body.query", "contains", "n=n-42"));
  t.add(b, "api");
  SavedRequest c = http("3 throws", "GET", base() + "/echo");
  c.post_script = "throw new Error('boom');";
  t.add(c, "api");
  SavedRequest d = http("4 pre throws", "GET", base() + "/echo");
  d.pre_script = "nope();";
  t.add(d, "api");

  auto items = collect(t.ws, t.dir / "api");
  REQUIRE(items.size() == 4);
  const Consent cons = consent_needed(t.ws, items, all(4));
  CHECK(cons.script_requests == 4);
  CHECK(cons.scripts_trusted);
  CHECK_FALSE(cons.scripts_need_trust());

  Workspace run_ws = t.ws;
  RunControl ctl;
  RunOptions opts;
  opts.run_scripts = true;
  std::vector<std::shared_ptr<const StepResult>> seen;
  const auto s = run_plan(items, all(4), opts, run_ws, {}, ctl, [&](auto r) { seen.push_back(r); });
  REQUIRE(seen.size() == 4);
  const StepResult& r1 = *seen[0];
  REQUIRE(r1.asserts.size() == 2);
  CHECK(r1.asserts[0].script);
  CHECK(r1.asserts[0].pass);
  CHECK(r1.asserts[0].label() == "status is 200");
  CHECK_FALSE(r1.asserts[1].pass);
  CHECK(r1.failed());
  CHECK(r1.reason().find("is wrong on purpose") != std::string::npos);
  REQUIRE(r1.logs.size() == 1);
  CHECK(r1.logs[0].text == "logged in");
  CHECK(r1.logs[0].post);
  bool var = false;
  for (const auto& k : r1.captures) var = var || (k.script && k.var == "token" && k.value == "abc" && k.ok);
  CHECK(var);
  bool phase = false;
  for (const auto& p : r1.phases) phase = phase || std::string(p.name) == "Post-response script";
  CHECK(phase);
  const StepResult& r2 = *seen[1];
  INFO(r2.reason());
  CHECK(r2.passed());
  CHECK(r2.asserts.size() == 2);  // the assert row, then the script test
  CHECK_FALSE(r2.asserts[0].script);
  CHECK(r2.asserts[1].script);
  std::string sig;
  for (const auto& [k, v] : r2.req_headers)
    if (k == "X-Sig") sig = v;
  CHECK(sig == "abc-signed");
  bool pre_phase = false, vars_phase = false;
  for (const auto& p : r2.phases) {
    pre_phase = pre_phase || std::string(p.name) == "Pre-request script";
    vars_phase = vars_phase || std::string(p.name) == "Pre-request vars";
  }
  CHECK(pre_phase);
  CHECK(vars_phase);
  // The response time (Time column, percentiles, Newman's figure) leaves the scripts out: the
  // network phases add up to timing.total_ms, the script phases to script_ms.
  for (const auto& r : seen) {
    double net = 0, scr = 0;
    for (const auto& p : r->phases)
      if (!is_setup_phase(p)) (is_script_phase(p) ? scr : net) += p.ms;
    CHECK(net == Catch::Approx(r->timing.total_ms).margin(0.01));
    CHECK(scr == Catch::Approx(r->script_ms).margin(0.01));
    CHECK(r->total_incl_scripts_ms() == Catch::Approx(net + scr).margin(0.01));
  }
  CHECK(r1.script_ms > 0);  // its post-response script
  CHECK(r2.script_ms > 0);  // pre-request vars, pre-request and post-response scripts
  {
    // A slow post-response script does not move the response time or its percentiles.
    SavedRequest slow = http("slow script", "GET", base() + "/echo");
    slow.post_script = "let x = 0; for (let i = 0; i < 3000000; ++i) x += i; test('ran', () => expect(x > 0).to.equal(true));";
    TempWs t2;
    REQUIRE(t2.ws.trust_scripts(true));
    t2.add(slow, "api");
    auto it2 = collect(t2.ws, t2.dir / "api");
    Workspace w2 = t2.ws;
    RunControl c2;
    std::vector<std::shared_ptr<const StepResult>> got;
    const auto s2 = run_plan(it2, all(1), opts, w2, {}, c2, [&](auto r) { got.push_back(r); });
    REQUIRE(got.size() == 1);
    INFO(got[0]->reason());
    CHECK(got[0]->passed());
    CHECK(got[0]->script_ms > got[0]->timing.total_ms);
    CHECK(s2.max_ms == Catch::Approx(got[0]->timing.total_ms));
    CHECK(s2.scripted == 1);
    CHECK(s2.avg_script_ms == Catch::Approx(got[0]->script_ms));
  }
  CHECK(seen[2]->failed());
  CHECK(seen[2]->reason().find("boom") != std::string::npos);
  CHECK_FALSE(seen[3]->sent);
  CHECK(seen[3]->failed());
  CHECK(seen[3]->error.find("pre-request script failed") != std::string::npos);
  CHECK(s.passed == 1);
  CHECK(s.failed == 3);
  CHECK(run_ws.runtime_vars["token"] == "abc");
  CHECK(t.ws.runtime_vars.count("token") == 0);

  RunMeta meta;
  meta.name = "api";
  const std::string xml = to_junit(meta, s, seen);
  CHECK(xml.find("is wrong on purpose") != std::string::npos);
  CHECK(xml.find("type=\"test\"") != std::string::npos);
  const std::string js = to_json(meta, s, seen);
  CHECK(js.find("\"console\"") != std::string::npos);
  CHECK(to_html(meta, s, seen).find("is wrong on purpose") != std::string::npos);
}

TEST_CASE("untrusted scripts are not run and the request does not pass", "[runner][scripts]") {
  TempWs t;
  SavedRequest a = scripted_pair_a();
  t.add(a, "api");
  SavedRequest plain = http("2 plain", "GET", base() + "/echo");
  t.add(plain, "api");
  auto items = collect(t.ws, t.dir / "api");
  const Consent cons = consent_needed(t.ws, items, all(2));
  CHECK(cons.script_requests == 1);
  CHECK(cons.scripts_need_trust());
  Workspace run_ws = t.ws;
  RunControl ctl;
  RunOptions opts;  // run_scripts false
  std::vector<std::shared_ptr<const StepResult>> seen;
  const auto s = run_plan(items, all(2), opts, run_ws, {}, ctl, [&](auto r) { seen.push_back(r); });
  REQUIRE(seen.size() == 2);
  CHECK(seen[0]->sent);
  CHECK(seen[0]->failed());
  CHECK(seen[0]->reason() == "scripts not run (workspace not trusted)");
  CHECK(seen[0]->logs.empty());
  CHECK(run_ws.runtime_vars.count("token") == 0);
  CHECK(seen[1]->passed());
  CHECK(s.passed == 1);
  // Asked to run them, but the workspace is still not trusted: script.h refuses too.
  opts.run_scripts = true;
  seen.clear();
  run_plan(items, all(2), opts, run_ws, {}, ctl, [&](auto r) { seen.push_back(r); });
  CHECK(seen[0]->failed());
  CHECK(seen[0]->reason() == "scripts not run (workspace not trusted)");
}

TEST_CASE("headless run: scripts, failure table, reason lines", "[runner][scripts]") {
  TempWs t;
  SavedRequest a = scripted_pair_a();
  t.add(a, "api");
  SavedRequest m = http("2 missing", "GET", base() + "/status/404");
  t.add(m, "api");
  const auto u = [](const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
  };
  std::string out;
  int code = run_cli({"--run", u(t.dir / "api"), "--workspace", u(t.dir), "--quiet"}, {}, &out);
  INFO(out);
  CHECK(code == 1);
  CHECK(out.find("not trusted") != std::string::npos);
  CHECK(out.find("scripts not run (workspace not trusted)") != std::string::npos);  // the failure table
  CHECK(out.find("HTTP 404") != std::string::npos);
  out.clear();
  code = run_cli({"--run", u(t.dir / "api"), "--workspace", u(t.dir), "--trust-scripts"}, {}, &out);
  CHECK(code == 1);
  CHECK(out.find("| logged in") != std::string::npos);
  CHECK(out.find("x is wrong on purpose") != std::string::npos);
  CHECK(out.find("for this run only") != std::string::npos);
  // Trusted for that run only: the app (and the next CI run) still asks.
  CHECK_FALSE(t.ws.scripts_trusted());
}

TEST_CASE("load profile: duration and ramp-up", "[runner]") {
  TempWs t;
  SavedRequest a = http("ping", "GET", base() + "/echo");
  t.add(a, "api");
  auto items = collect(t.ws, t.dir / "api");
  RunControl ctl;
  RunOptions opts;
  opts.duration_ms = 400;
  opts.concurrency = 4;
  opts.ramp_up_ms = 200;
  std::mutex m;
  std::vector<std::shared_ptr<const StepResult>> seen;
  const auto t0 = std::chrono::steady_clock::now();
  const auto s = run_plan(items, all(1), opts, t.ws, {}, ctl, [&](auto r) {
    std::lock_guard lk(m);
    seen.push_back(r);
  });
  const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  CHECK(s.planned == 0);
  CHECK(wall >= 400);
  CHECK(wall < 3000);
  CHECK(seen.size() > 8);
  CHECK(s.passed == seen.size());
  CHECK_FALSE(s.stopped);
  // Ramp-up: the last worker's first send starts about 3/4 of the ramp in.
  double first_late = 1e9;
  for (const auto& r : seen) first_late = std::min(first_late, r->started_ms);
  CHECK(first_late < 300);
  size_t early = 0;
  for (const auto& r : seen)
    if (r->started_ms < 100) ++early;
  CHECK(early < seen.size());
}

TEST_CASE("cookies set by a response go with the later requests of the run", "[runner]") {
  TempWs t;
  SavedRequest a = http("1 login", "GET", base() + "/response-headers?Set-Cookie=sid%3Dabc%3B%20Path%3D%2F");
  SavedRequest b = http("2 use", "GET", base() + "/echo");
  b.asserts.push_back(as("res.body.headers.Cookie", "eq", "sid=abc"));
  SavedRequest c = http("3 own cookie", "GET", base() + "/echo");
  c.http.headers.push_back({"Cookie", "mine=1"});
  c.asserts.push_back(as("res.body.headers.Cookie", "eq", "mine=1; sid=abc"));
  t.add(a, "api");
  t.add(b, "api");
  t.add(c, "api");
  auto items = collect(t.ws, t.dir / "api");
  REQUIRE(items.size() == 3);
  RunControl ctl;
  RunOptions opts;
  std::vector<std::shared_ptr<const StepResult>> seen;
  Workspace w1 = t.ws;
  run_plan(items, all(3), opts, w1, {}, ctl, [&](auto r) { seen.push_back(r); });
  REQUIRE(seen.size() == 3);
  INFO(seen[1]->reason() << " / " << seen[2]->reason());
  CHECK(seen[1]->passed());
  CHECK(seen[2]->passed());
  opts.cookies = false;
  seen.clear();
  Workspace w2 = t.ws;
  run_plan(items, all(3), opts, w2, {}, ctl, [&](auto r) { seen.push_back(r); });
  CHECK(seen[1]->failed());
}

TEST_CASE("assert failures read as one line", "[runner]") {
  StepResult s;
  s.sent = s.transport_ok = true;
  s.status = 404;
  s.status_text = "Not Found";
  CHECK(s.reason().find("HTTP 404 Not Found") == 0);
  StepResult::Check c;
  c.target = "res.status";
  c.op = "eq";
  c.expected = "200";
  c.actual = "404";
  c.message = "expected res.status to equal 200, got 404";
  s.asserts.push_back(c);
  s.asserts_failed = 1;
  CHECK(s.reason() == "expected res.status to equal 200, got 404");
}

TEST_CASE("data rows, captures and form bodies are encoded as the editor sends them", "[runner][encode]") {
  TempWs t;
  // 1. Data row value in the query: "c d" and "a&b=c" stay one value each.
  SavedRequest a = http("q", "GET", base() + "/echo?u={{user}}&z=1");
  a.asserts.push_back(as("res.status", "eq", "200"));
  t.add(a, "");
  auto items = collect(t.ws, {});
  REQUIRE(items.size() == 1);
  RunOptions o;
  std::string err;
  REQUIRE(parse_data("user\nc d\na&b=c\nplain\n", false, &o.data, &err));
  o.iterations = 3;
  Workspace w = t.ws;
  RunControl ctl;
  std::vector<std::shared_ptr<const StepResult>> seen;
  const auto s = run_plan(items, {0}, o, w, {}, ctl, [&](auto r) { seen.push_back(r); });
  REQUIRE(seen.size() == 3);
  CHECK(s.passed == 3);
  CHECK(seen[0]->error.empty());
  CHECK(seen[0]->url == base() + "/echo?u=c%20d&z=1");
  CHECK(seen[0]->body_preview.find("\"query\":\"u=c%20d&z=1\"") != std::string::npos);
  CHECK(seen[1]->url == base() + "/echo?u=a%26b%3Dc&z=1");
  CHECK(seen[2]->url == base() + "/echo?u=plain&z=1");
}

TEST_CASE("a captured value with a space reaches the query and a form body encoded", "[runner][encode]") {
  TempWs t;
  SavedRequest a = http("1 cap", "POST", base() + "/echo");
  a.http.body = "ann marie";
  Capture c;
  c.var = "who";
  c.target = "res.body.body";
  a.captures.push_back(c);
  t.add(a, "f");
  SavedRequest b = http("2 use", "GET", base() + "/echo?n={{who}}");
  b.asserts.push_back(as("res.status", "eq", "200"));
  t.add(b, "f");
  SavedRequest f = http("3 form", "POST", base() + "/echo");
  f.http.headers.emplace_back("Content-Type", "application/x-www-form-urlencoded");
  f.http.body = "name={{who}}&k=v";
  t.add(f, "f");
  auto items = collect(t.ws, t.dir / "f");
  REQUIRE(items.size() == 3);
  Workspace w = t.ws;
  RunControl ctl;
  RunOptions o;
  std::vector<std::shared_ptr<const StepResult>> seen;
  const auto s = run_plan(items, all(3), o, w, {}, ctl, [&](auto r) { seen.push_back(r); });
  REQUIRE(seen.size() == 3);
  CHECK(s.failed == 0);
  CHECK(w.runtime_vars["who"] == "ann marie");
  CHECK(seen[1]->error.empty());
  CHECK(seen[1]->url == base() + "/echo?n=ann%20marie");
  CHECK(seen[2]->req_body == "name=ann+marie&k=v");
}

TEST_CASE("encode_as_sent leaves plain requests alone", "[runner][encode]") {
  TempWs t;
  t.ws.runtime_vars["base"] = "http://h:1";
  t.ws.runtime_vars["p"] = "a b";
  SavedRequest r = http("x", "GET", "{{base}}/x?q=1");
  encode_as_sent(r, t.ws);
  CHECK(r.http.url == "{{base}}/x?q=1");  // same either way: left to http_to_send
  r.http.url = "{{base}}/{{p}}?q={{p}}";
  encode_as_sent(r, t.ws);
  CHECK(r.http.url == "http://h:1/a%20b?q=a%20b");
  SavedRequest j = http("j", "POST", "http://h/x");
  j.http.headers.emplace_back("Content-Type", "application/json");
  j.http.body = "{\"n\":\"{{p}}\"}";
  encode_as_sent(j, t.ws);
  CHECK(j.http.body == "{\"n\":\"{{p}}\"}");  // only urlencoded bodies are form-encoded
}

TEST_CASE("rerun failed: each failed iteration again with its own data row", "[runner][encode]") {
  TempWs t;
  SavedRequest a = http("a", "GET", base() + "/echo?u={{user}}");
  a.asserts.push_back(as("res.body.query", "contains", "u=ok"));
  SavedRequest b = http("b", "GET", base() + "/echo?n=1");
  t.add(a, "");
  t.add(b, "");
  auto items = collect(t.ws, {});
  REQUIRE(items.size() == 2);
  RunOptions o;
  std::string err;
  REQUIRE(parse_data("user\nok\nc d\nok\nzed\n", false, &o.data, &err));
  // The run before failed "a" in iterations 2 and 4 (index 1 and 3).
  o.retry = {{1, {0}}, {3, {0}}};
  o.iterations = 4;  // ignored: retry decides
  CHECK(planned_sends(o, 2) == 2);
  Workspace w = t.ws;
  w.runtime_vars["user"] = "leftover";
  RunControl ctl;
  std::vector<std::shared_ptr<const StepResult>> seen;
  const auto s = run_plan(items, all(2), o, w, {}, ctl, [&](auto r) { seen.push_back(r); });
  REQUIRE(seen.size() == 2);
  CHECK(s.planned == 2);
  CHECK(seen[0]->iteration == 1);
  CHECK(seen[0]->url == base() + "/echo?u=c%20d");
  CHECK(seen[1]->iteration == 3);
  CHECK(seen[1]->url == base() + "/echo?u=zed");
  CHECK(seen[0]->item == 0);
  CHECK(seen[1]->item == 0);
}

TEST_CASE("big bodies go whole to a temp file, removed with the last step", "[runner][spill]") {
  TempWs t;
  SavedRequest a = http("big", "GET", base() + "/bytes/1000000");
  t.add(a, "");
  auto items = collect(t.ws, {});
  RunOptions o;
  o.spill_bodies = true;
  o.iterations = 2;
  Workspace w = t.ws;
  RunControl ctl;
  std::vector<std::shared_ptr<const StepResult>> seen;
  const uint64_t before = SpillFile::live_bytes();
  run_plan(items, {0}, o, w, {}, ctl, [&](auto r) { seen.push_back(r); });
  REQUIRE(seen.size() == 2);
  REQUIRE(seen[0]->body_file);
  CHECK(seen[0]->body_truncated);
  CHECK(seen[0]->body_file->size() == 1000000);
  CHECK(SpillFile::live_bytes() == before + 2000000);
  std::string full;
  REQUIRE(seen[0]->body_file->read(&full));
  CHECK(full.size() == 1000000);
  CHECK(full.substr(0, 3) == "abc");
  const fs::path p = seen[0]->body_file->path();
  CHECK(fs::exists(p));
  seen.clear();
  CHECK_FALSE(fs::exists(p));
  CHECK(SpillFile::live_bytes() == before);
  // Off (the CLI): nothing written.
  o.spill_bodies = false;
  run_plan(items, {0}, o, w, {}, ctl, [&](auto r) { seen.push_back(r); });
  CHECK_FALSE(seen[0]->body_file);
}

// ------------------------------------------------------------------ round 5: triage

TEST_CASE("normalize_numbers folds every number into one mark", "[runner][triage]") {
  CHECK(normalize_numbers("expected res.responseTime to be < 100, got 123.5") == "expected res.responseTime to be < #, got #");
  CHECK(normalize_numbers("got 129.9") == normalize_numbers("got 123.5"));
  CHECK(normalize_numbers("got -4 and 1e3") == "got # and #");
  CHECK(normalize_numbers("v1.2.3") == "v#.#");
  CHECK(normalize_numbers("HTTP 404 Not Found") == "HTTP # Not Found");
  CHECK(normalize_numbers("no numbers") == "no numbers");
}

namespace {
std::shared_ptr<StepResult> failed_step(size_t seq, int it, size_t item, const std::string& rel, const std::string& target,
                                        const std::string& op, const std::string& expected, const std::string& message) {
  auto s = std::make_shared<StepResult>();
  s->seq = seq;
  s->iteration = it;
  s->item = item;
  s->rel = rel;
  s->name = rel;
  s->method = "GET";
  s->sent = s->transport_ok = true;
  s->status = 200;
  StepResult::Check c;
  c.pass = false;
  c.target = target;
  c.op = op;
  c.expected = expected;
  c.message = message;
  s->asserts.push_back(c);
  s->asserts_failed = 1;
  return s;
}
}  // namespace

TEST_CASE("failures group by request, check and message with numbers normalised", "[runner][triage]") {
  std::vector<std::shared_ptr<const StepResult>> v;
  size_t seq = 0;
  for (int it = 0; it < 4; ++it) {
    v.push_back(failed_step(seq++, it, 0, "slow", "res.responseTime", "lt", "100",
                            "expected res.responseTime to be < 100, got " + std::to_string(120 + it) + ".5"));
    auto ok = std::make_shared<StepResult>();
    ok->seq = seq++;
    ok->iteration = it;
    ok->item = 1;
    ok->rel = "fine";
    ok->sent = ok->transport_ok = true;
    ok->status = 200;
    v.push_back(ok);
    auto nf = std::make_shared<StepResult>();  // no asserts: a 404 fails on its status
    nf->seq = seq++;
    nf->iteration = it;
    nf->item = 2;
    nf->rel = "missing";
    nf->sent = nf->transport_ok = true;
    nf->status = 404;
    nf->status_text = "Not Found";
    v.push_back(nf);
  }
  // A different check of the same request is its own group.
  v.push_back(failed_step(seq++, 3, 0, "slow", "res.status", "eq", "201", "expected res.status to equal 201, got 200"));
  const auto g = group_failures(v);
  REQUIRE(g.size() == 3);
  CHECK(g[0].rel == "slow");
  CHECK(g[0].count == 4);
  CHECK(g[0].check == "res.responseTime lt 100");
  CHECK(g[0].first_iteration == 0);
  CHECK(g[0].last_iteration == 3);
  CHECK(g[0].variants == 4);
  CHECK(g[0].message == "expected res.responseTime to be < 100, got 120.5");
  CHECK(g[0].last_message == "expected res.responseTime to be < 100, got 123.5");
  CHECK(g[1].rel == "missing");
  CHECK(g[1].count == 4);
  CHECK(g[1].message == "HTTP 404 Not Found");
  CHECK(g[2].count == 1);
  // A group's key selects exactly its steps.
  size_t hits = 0;
  for (const auto& s : v) hits += has_failure(*s, g[0].key) ? 1 : 0;
  CHECK(hits == 4);
  hits = 0;
  for (const auto& s : v) hits += has_failure(*s, g[2].key) ? 1 : 0;
  CHECK(hits == 1);
  CHECK_FALSE(has_failure(*v[1], g[0].key));  // a passing step
  // Every failed check of a step is listed, in order.
  auto two = failed_step(99, 0, 0, "slow", "res.status", "eq", "201", "expected res.status to equal 201, got 200");
  StepResult::Check c2;
  c2.target = "res.body.id";
  c2.op = "exists";
  two->asserts.push_back(c2);
  two->asserts_failed = 2;
  const auto f = failures_of(*two);
  REQUIRE(f.size() == 2);
  CHECK(f[1].check == "res.body.id exists");
  CHECK(f[1].message == "res.body.id exists: got (undefined)");
}

TEST_CASE("thresholds parse and check against the summary", "[runner][triage]") {
  std::vector<Threshold> t;
  std::string err;
  REQUIRE(parse_thresholds("p95<200 p99 <= 1s, err<1%; rps>=50 avg<100ms", &t, &err));
  REQUIRE(t.size() == 5);
  CHECK(t[0].metric == "p95");
  CHECK(t[1].value == 1000);
  CHECK(t[1].op == "<=");
  CHECK(t[2].metric == "err");
  CHECK(t[3].text == "rps >= 50/s");
  CHECK_FALSE(parse_thresholds("p97<3", &t, &err));
  CHECK(err.find("unknown metric") != std::string::npos);
  CHECK_FALSE(parse_thresholds("p95 200", &t, &err));
  CHECK_FALSE(parse_thresholds("err<5ms", &t, &err));
  REQUIRE(parse_thresholds("", &t, &err));
  CHECK(t.empty());

  RunSummary s;
  s.passed = 99;
  s.failed = 1;
  s.total = 100;
  s.samples = 100;
  s.wall_ms = 1000;
  s.p95_ms = 150;
  s.p99_ms = 1200;
  REQUIRE(parse_thresholds("p95<200 p99<1s err<1% rps>=100", &t, &err));
  const auto r = check_thresholds(t, s);
  REQUIRE(r.size() == 4);
  CHECK(r[0].pass);
  CHECK_FALSE(r[1].pass);
  CHECK_FALSE(r[2].pass);  // 1.00% is not < 1%
  CHECK(r[2].actual_text == "1.00%");
  CHECK(r[3].pass);
  CHECK(check_thresholds(t, RunSummary{}).front().actual_text == "no data");
}

TEST_CASE("steps carry their data row, p90 / p99 and unresolved variables", "[runner][triage]") {
  TempWs t;
  SavedRequest a = http("d", "GET", base() + "/echo?u={{user}}&t={{nope}}");
  t.add(a, "");
  auto items = collect(t.ws, {});
  RunOptions o;
  std::string err;
  REQUIRE(parse_data("user,expect\nann,1\nzed,WRONG\n", false, &o.data, &err));
  o.iterations = 2;
  Workspace w = t.ws;
  RunControl ctl;
  std::vector<std::shared_ptr<const StepResult>> got;
  const auto s = run_plan(items, {0}, o, w, {}, ctl, [&](auto r) { got.push_back(r); });
  REQUIRE(got.size() == 2);
  REQUIRE(got[1]->data_row);
  CHECK(got[1]->data_index == 1);
  CHECK((*got[1]->data_row)[0].second == "zed");
  CHECK((*got[1]->data_row)[1].second == "WRONG");
  REQUIRE(got[0]->unresolved.size() == 1);
  CHECK(got[0]->unresolved[0] == "nope");
  CHECK(s.samples == 2);
  CHECK(s.p99_ms >= s.p90_ms);
  CHECK(s.p90_ms >= s.p50_ms);
}

TEST_CASE("headless run: relative folder, iteration separators, causes, thresholds", "[runner][triage]") {
  TempWs t;
  SavedRequest a = http("ok", "GET", base() + "/echo?u={{user}}");
  a.asserts.push_back(as("res.body.query", "contains", "u=ann"));
  t.add(a, "Users API");
  const fs::path data = t.cfg / "rows.csv";
  {
    std::ofstream f(data, std::ios::binary);
    f << "user\nann\nzed\nzed\n";
  }
  const auto u = [](const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
  };
  std::string out;
  int code = run_cli({"--run", "Users API", "--workspace", u(t.dir), "--data", u(data), "--threshold", "p95<60000 err<50%"},
                     {}, &out);
  INFO(out);
  CHECK(code == 1);
  CHECK(out.find("Iteration 2/3  (data row 2: user=zed)") != std::string::npos);
  CHECK(out.find("failures by cause") != std::string::npos);
  CHECK(out.find("2x") != std::string::npos);
  CHECK(out.find(" received") != std::string::npos);
  CHECK(out.find("p99 ") != std::string::npos);
  CHECK(out.find("FAIL  err < 50%") != std::string::npos);
  CHECK(out.find("ok    p95 < 60000 ms") != std::string::npos);
  out.clear();
  CHECK(run_cli({"--run", "Users API", "--workspace", u(t.dir), "--threshold", "p97<1"}, {}, &out) == 2);
  out.clear();
  // One send: no spread and no rate.
  code = run_cli({"--run", "Users API", "--workspace", u(t.dir)}, {}, &out);
  CHECK(out.find("std dev -; throughput -") != std::string::npos);
}

TEST_CASE("failure groups: iteration range by iteration number, not finishing order", "[runner][triage]") {
  // Finishing order (seq) of a parallel run: iteration 7 first, iteration 0 last.
  std::vector<std::shared_ptr<const StepResult>> v;
  const int order[] = {7, 2, 5, 9, 1, 8, 3, 6, 4, 0};
  size_t seq = 0;
  for (int it : order)
    v.push_back(failed_step(seq++, it, 0, "lookup", "res.status", "eq", "200", "expected res.status to equal 200, got 404"));
  v.push_back(failed_step(seq++, 4, 0, "lookup", "res.status", "eq", "200", "expected res.status to equal 200, got 404"));
  const auto g = group_failures(v);
  REQUIRE(g.size() == 1);
  CHECK(g[0].count == 11);
  CHECK(g[0].first_iteration == 0);
  CHECK(g[0].last_iteration == 9);
  CHECK(g[0].iterations == 10);  // iteration 4 failed twice: counted once
  CHECK(g[0].first_seq == 0);
  CHECK(g[0].last_seq == 10);
}

TEST_CASE("parallel run: a cause that always fails spans every iteration", "[runner][triage]") {
  TempWs t;
  SavedRequest a = http("fast", "GET", base() + "/echo");
  SavedRequest b = http("slow fail", "GET", base() + "/delay/3");
  b.asserts.push_back(as("res.status", "eq", "201"));
  SavedRequest c = http("missing", "GET", base() + "/status/404");
  t.add(a, "");
  t.add(b, "");
  t.add(c, "");
  auto items = collect(t.ws, {});
  REQUIRE(items.size() == 3);
  constexpr int N = 60;
  RunOptions o;
  o.iterations = N;
  o.concurrency = 8;
  Workspace w = t.ws;
  RunControl ctl;
  std::mutex m;
  std::vector<std::shared_ptr<const StepResult>> seen;
  run_plan(items, all(3), o, w, {}, ctl, [&](auto r) {
    std::lock_guard lk(m);
    seen.push_back(r);
  });
  REQUIRE(seen.size() == 3u * N);
  const auto g = group_failures(seen);
  REQUIRE(g.size() == 2);
  for (const auto& x : g) {
    INFO(x.rel);
    CHECK(x.count == static_cast<size_t>(N));
    CHECK(x.first_iteration == 0);
    CHECK(x.last_iteration == N - 1);
    CHECK(x.iterations == static_cast<size_t>(N));
  }
  // The CLI reports the same range and count, and --quiet prints the causes, not every failure.
  const auto u = [](const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
  };
  std::string out;
  const int code = run_cli({"--run", u(t.dir), "--workspace", u(t.dir), "--iterations", std::to_string(N), "--parallel", "8", "--quiet"},
                           {}, &out);
  INFO(out);
  CHECK(code == 1);
  CHECK(out.find("(iterations 1-" + std::to_string(N) + "; " + std::to_string(N) + " of " + std::to_string(N) + " iterations)") !=
        std::string::npos);
  CHECK(out.find("failures by cause") != std::string::npos);
  CHECK(out.find("  #  ") == std::string::npos);  // no per-failure table with --quiet
  CHECK(out.find("  1.") == std::string::npos);
}

TEST_CASE("collect runs in the sidebar's order: subfolders first, natural names", "[runner]") {
  TempWs t;
  SavedRequest r10 = http("req 10", "GET", base() + "/echo");
  SavedRequest r2 = http("req 2", "GET", base() + "/echo");
  SavedRequest sub = http("in sub", "GET", base() + "/echo");
  SavedRequest deep = http("deep", "GET", base() + "/echo");
  t.add(r10, "f");
  t.add(r2, "f");
  t.add(sub, "f/b sub");
  t.add(deep, "f/a sub");
  const auto items = collect(t.ws, t.dir / "f");
  REQUIRE(items.size() == 4);
  CHECK(items[0].req.name == "deep");
  CHECK(items[1].req.name == "in sub");
  CHECK(items[2].req.name == "req 2");
  CHECK(items[3].req.name == "req 10");
}

TEST_CASE("headless run: gRPC-wide status column and aligned failure numbers", "[runner]") {
  TempWs t;
  for (int i = 0; i < 12; ++i) {
    SavedRequest r = http("m" + std::to_string(i), "GET", base() + "/status/404");
    t.add(r, "api");
  }
  const auto u = [](const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
  };
  std::string out;
  run_cli({"--run", u(t.dir / "api"), "--workspace", u(t.dir)}, {}, &out);
  INFO(out);
  // "  9." and "  10." both leave the name in the same column.
  const size_t a = out.find("  9. "), b = out.find("  10. ");
  REQUIRE(a != std::string::npos);
  REQUIRE(b != std::string::npos);
  const size_t la = a - out.rfind('\n', a), lb = b - out.rfind('\n', b);
  const size_t na = out.find_first_not_of(" .0123456789", a) - a + la, nb = out.find_first_not_of(" .0123456789", b) - b + lb;
  CHECK(na == nb);
}

TEST_CASE("load profiles: ramp, peak and spike worker windows", "[runner]") {
  RunOptions o;
  o.duration_ms = 10000;
  // Fixed: everyone from 0 to the end.
  CHECK(load_window(o, 3, 4).start_ms == 0);
  CHECK(load_window(o, 3, 4).stop_ms == 10000);
  // Ramp up over 2 s: worker 2 of 4 joins at 1 s.
  o.ramp_up_ms = 2000;
  CHECK(load_window(o, 2, 4).start_ms == Catch::Approx(1000));
  CHECK(load_window(o, 2, 4).stop_ms == 10000);
  // Peak: and leaves 1 s before the end; worker 0 runs all through.
  o.ramp_down_ms = 2000;
  CHECK(load_window(o, 2, 4).stop_ms == Catch::Approx(9000));
  CHECK(load_window(o, 0, 4).stop_ms == 10000);
  // Spike: 2 base workers all through, the others in the middle fifth.
  RunOptions sp;
  sp.duration_ms = 10000;
  sp.spike_base = 2;
  CHECK(load_window(sp, 1, 8).start_ms == 0);
  CHECK(load_window(sp, 1, 8).stop_ms == 10000);
  CHECK(load_window(sp, 5, 8).start_ms == Catch::Approx(4000));
  CHECK(load_window(sp, 5, 8).stop_ms == Catch::Approx(6000));
  // No duration: no end but the iterations.
  RunOptions n;
  n.ramp_up_ms = 1000;
  CHECK(load_window(n, 1, 2).stop_ms == 0);
}

TEST_CASE("spike run: the extra workers only send in the middle of the run", "[runner]") {
  TempWs t;
  SavedRequest a = http("a", "GET", base() + "/delay/10");
  t.add(a, "");
  auto items = collect(t.ws, {});
  RunOptions o;
  o.concurrency = 8;
  o.duration_ms = 1000;
  o.spike_base = 2;
  Workspace w = t.ws;
  RunControl ctl;
  std::mutex m;
  std::vector<std::shared_ptr<const StepResult>> seen;
  run_plan(items, {0}, o, w, {}, ctl, [&](auto r) {
    std::lock_guard lk(m);
    seen.push_back(r);
  });
  size_t early = 0, mid = 0;
  for (const auto& r : seen) {
    if (r->started_ms < 350) ++early;
    else if (r->started_ms > 420 && r->started_ms < 580) ++mid;
  }
  INFO(early << " early, " << mid << " mid");
  // ~2 workers x 35 sends before 350 ms; ~8 workers in the spike: per ms, far more in the middle.
  CHECK(static_cast<double>(mid) / 160.0 > 2.0 * static_cast<double>(early) / 350.0);
}

TEST_CASE("folder and collection asserts are checked on every request below them", "[runner][folders]") {
  TempWs t;
  SavedRequest a = http("ok", "GET", base() + "/echo");
  a.asserts.push_back(as("res.status", "eq", "200"));
  SavedRequest b = http("no own checks", "GET", base() + "/echo");
  t.add(a, "api");
  t.add(b, "api");
  const auto u = [](const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
  };
  std::string out;
  CHECK(run_cli({"--run", u(t.dir / "api"), "--workspace", u(t.dir)}, {}, &out) == 0);
  // A folder check every response fails: the run (and a CI gate on its exit code) fails.
  FolderSettings fs_api;
  fs_api.asserts.push_back(as("res.status", "lt", "1"));
  std::string err;
  REQUIRE(t.ws.save_folder_settings(t.dir / "api", fs_api, &err));
  out.clear();
  const int code = run_cli({"--run", u(t.dir / "api"), "--workspace", u(t.dir)}, {}, &out);
  INFO(out);
  CHECK(code == 1);
  CHECK(out.find("0 passed, 2 failed") != std::string::npos);
  CHECK(out.find("from api") != std::string::npos);
  // The engine lists the folder's check after the request's own, with its origin.
  const auto items = collect(t.ws, t.dir / "api");
  REQUIRE(items.size() == 2);
  REQUIRE(items[1].req.name == "ok");
  Workspace run_ws = t.ws;
  RunControl ctl;
  RunOptions opts;
  std::vector<std::shared_ptr<const StepResult>> seen;
  run_plan(items, all(items.size()), opts, run_ws, {}, ctl, [&](auto r) { seen.push_back(r); });
  REQUIRE(seen.size() == 2);
  const StepResult& s1 = *seen[1];
  REQUIRE(s1.asserts.size() == 2);
  CHECK(s1.asserts[0].from.empty());
  CHECK(s1.asserts[0].pass);
  CHECK(s1.asserts[1].from == "api");
  CHECK_FALSE(s1.asserts[1].pass);
  CHECK(s1.failed());
  REQUIRE(seen[0]->asserts.size() == 1);
  CHECK(seen[0]->asserts[0].label().find("(from api)") != std::string::npos);
  // Collection-level checks count too.
  FolderSettings coll;
  coll.asserts.push_back(as("res.status", "eq", "200"));
  REQUIRE(t.ws.save_collection_settings(coll, &err));
  REQUIRE(t.ws.save_folder_settings(t.dir / "api", FolderSettings{}, &err));
  out.clear();
  CHECK(run_cli({"--run", u(t.dir / "api"), "--workspace", u(t.dir)}, {}, &out) == 0);
  CHECK(out.find("asserts 3 / 3") != std::string::npos);
}

TEST_CASE("collect orders requests by file name, as the sidebar lists them", "[runner]") {
  // 01-/02-/03- prefixes order a folder (the tree shows the display names in this order too),
  // so a capture set by the first request reaches the later ones in Run folder and --run.
  TempWs t;
  SavedRequest p = http("Protected", "GET", base() + "/echo");
  SavedRequest l = http("List items", "GET", base() + "/echo");
  SavedRequest e = http("Echo uses capture", "GET", base() + "/echo");
  p.file = t.dir / "api" / "01-protected.req.json";
  l.file = t.dir / "api" / "02-items.req.json";
  e.file = t.dir / "api" / "03-echo.req.json";
  fs::create_directories(t.dir / "api");
  for (SavedRequest* r : {&p, &l, &e}) REQUIRE(t.ws.save_request(*r));
  const auto items = collect(t.ws, t.dir / "api");
  REQUIRE(items.size() == 3);
  CHECK(items[0].req.name == "Protected");
  CHECK(items[1].req.name == "List items");
  CHECK(items[2].req.name == "Echo uses capture");
}

TEST_CASE("a saved run order interleaves folders and requests, for the runner and --run", "[runner][order]") {
  // The common layout: a root request captures a value that requests in a subfolder use. The
  // sidebar lists subfolders first, so by default the subfolder runs before the capture.
  TempWs t;
  SavedRequest l = http("List items", "POST", base() + "/echo");
  Capture c;
  c.var = "firstId";
  c.target = "res.body.method";
  l.captures.push_back(c);
  t.add(l, "");
  SavedRequest g = http("Get order", "GET", base() + "/echo?m={{firstId}}");
  g.asserts.push_back(as("res.body.query", "contains", "m=POST"));
  t.add(g, "orders");
  SavedRequest h = http("Create order", "GET", base() + "/echo");
  t.add(h, "orders");
  const auto u = [](const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
  };
  auto items = collect(t.ws, {});
  REQUIRE(items.size() == 3);
  CHECK(items[0].rel == "orders/Create order");
  CHECK(items[1].rel == "orders/Get order");
  CHECK(items[2].rel == "List items");
  std::string out;
  CHECK(run_cli({"--run", u(t.dir), "--workspace", u(t.dir), "--quiet"}, {}, &out) == 1);

  // The runner's arrow: List items trades places with the whole orders folder (one step).
  std::vector<size_t> order = all(items.size());
  FolderOrder changed;
  REQUIRE(move_block(items, order, t.dir, 2, -1, &changed));
  CHECK(order == std::vector<size_t>{2, 0, 1});
  REQUIRE(changed.size() == 1);
  REQUIRE(changed.begin()->second == std::vector<std::wstring>{L"List items.req.json", L"orders"});
  // Already first: nothing moves.
  CHECK_FALSE(move_block(items, order, t.dir, 0, -1, nullptr));
  // Get order up: stays in orders/, swaps with Create order.
  std::vector<size_t> o2 = order;
  FolderOrder ch2;
  REQUIRE(move_block(items, o2, t.dir, 2, -1, &ch2));
  CHECK(o2 == std::vector<size_t>{2, 1, 0});
  CHECK(ch2.begin()->first == (t.dir / "orders").lexically_normal());
  // Create order (first in orders/) up: the orders folder moves above List items again.
  std::vector<size_t> o3 = order;
  REQUIRE(move_block(items, o3, t.dir, 1, -1, nullptr));
  CHECK(o3 == std::vector<size_t>{0, 1, 2});
  // Drag List items to the end: past the orders block.
  std::vector<size_t> o4 = order;
  REQUIRE(drop_block(items, o4, t.dir, 0, 3, nullptr));
  CHECK(o4 == std::vector<size_t>{0, 1, 2});

  std::string err;
  REQUIRE(save_run_order(t.dir, changed, 1, &err));
  REQUIRE(fs::exists(t.dir / kRunOrderFile));
  // An older write arriving late never undoes the newer order.
  FolderOrder stale{{t.dir, {L"orders", L"List items.req.json"}}};
  REQUIRE(save_run_order(t.dir, stale, 0, &err));  // seq 0: always written
  REQUIRE(save_run_order(t.dir, changed, 3, &err));
  REQUIRE(save_run_order(t.dir, stale, 2, &err));  // dropped: 2 < 3
  const FolderOrder back = load_run_order(t.dir);
  REQUIRE(back.count(t.dir) == 1);
  CHECK(back.at(t.dir) == std::vector<std::wstring>{L"List items.req.json", L"orders"});

  // Reopened (a fresh collect) and in CI (--run): the root capture runs first and feeds orders/.
  items = collect(t.ws, {});
  REQUIRE(items.size() == 3);
  CHECK(items[0].rel == "List items");
  CHECK(items[1].rel == "orders/Create order");
  out.clear();
  CHECK(run_cli({"--run", u(t.dir), "--workspace", u(t.dir), "--quiet"}, {}, &out) == 0);
  // Running only the subfolder keeps its own order.
  const auto sub = collect(t.ws, t.dir / "orders");
  REQUIRE(sub.size() == 2);
  CHECK(sub[0].rel == "Create order");

  // Keys outside the workspace, absolute keys and names with separators are ignored.
  {
    std::ofstream f(t.dir / kRunOrderFile, std::ios::binary | std::ios::trunc);
    f << R"({"version":1,"order":{"..":["x"],"C:/abs":["y"],"orders/../..":["z"],".":["a/b","..","orders"]}})";
  }
  const FolderOrder bad = load_run_order(t.dir);
  REQUIRE(bad.size() == 1);
  CHECK(bad.at(t.dir) == std::vector<std::wstring>{L"orders"});
  // A file that is not a run order is reported, not fatal.
  { std::ofstream(t.dir / kRunOrderFile, std::ios::binary | std::ios::trunc) << "[1,2]"; }
  std::string why;
  CHECK(load_run_order(t.dir, &why).empty());
  CHECK_FALSE(why.empty());
  CHECK(collect(t.ws, {}).size() == 3);
}

TEST_CASE("the runner's reset drops a folder's run order: the sidebar's order again", "[runner][order]") {
  // What the runner's circular arrow writes: an empty list for the folder and each folder below
  // it; other folders' entries stay.
  TempWs t;
  SavedRequest r1 = http("Login", "GET", base() + "/echo"), r2 = http("List items", "GET", base() + "/echo"),
               r3 = http("Get order", "GET", base() + "/echo"), r4 = http("Big", "GET", base() + "/echo");
  t.add(r1, "Shop");
  t.add(r2, "Shop");
  t.add(r3, "Shop/orders");
  t.add(r4, "Other");
  const fs::path shop = t.dir / "Shop";
  std::string err;
  FolderOrder custom{{shop, {L"List items.req.json", L"orders", L"Login.req.json"}},
                     {shop / "orders", {L"Get order.req.json"}},
                     {t.dir, {L"Shop", L"Other"}}};
  REQUIRE(save_run_order(t.dir, custom, 0, &err));
  auto items = collect(t.ws, shop);
  REQUIRE(items.size() == 3);
  CHECK(items[0].rel == "List items");

  FolderOrder reset;
  for (const auto& [dir, names] : load_run_order(t.dir)) {
    const fs::path rel = dir.lexically_normal().lexically_relative(shop.lexically_normal());
    if (!rel.empty() && *rel.begin() != "..") reset[dir] = {};
  }
  CHECK(reset.size() == 2);  // Shop and Shop/orders, not the root
  REQUIRE(save_run_order(t.dir, reset, 0, &err));
  const FolderOrder back = load_run_order(t.dir);
  CHECK(back.size() == 1);
  CHECK(back.count(t.dir) == 1);
  items = collect(t.ws, shop);
  REQUIRE(items.size() == 3);
  CHECK(items[0].rel == "orders/Get order");  // folders first, then natural names, as the sidebar
  CHECK(items[1].rel == "List items");
  CHECK(items[2].rel == "Login");
}

TEST_CASE("run-order.json puts commas at line ends, one name per line", "[runner][order]") {
  CHECK(run_order_json({}) == "{\n  \"version\": 1,\n  \"order\": {}\n}\n");
  const std::string j = run_order_json({{".", {"List items.req.json", "orders"}}, {"orders", {"Get \"x\".req.json"}}});
  CHECK(j ==
        "{\n  \"version\": 1,\n  \"order\": {\n"
        "    \".\": [\n      \"List items.req.json\",\n      \"orders\"\n    ],\n"
        "    \"orders\": [\n      \"Get \\\"x\\\".req.json\"\n    ]\n  }\n}\n");
  for (size_t at = j.find('\n'); at != std::string::npos; at = j.find('\n', at + 1)) {
    size_t k = at + 1;
    while (k < j.size() && j[k] == ' ') ++k;
    CHECK((k >= j.size() || j[k] != ','));
  }
  // What it writes reads back.
  TempWs t;
  fs::create_directories(t.dir / "orders");
  std::string err;
  REQUIRE(save_run_order(t.dir, {{t.dir, {L"orders", L"a.req.json"}}, {t.dir / "orders", {L"b.req.json"}}}, 0, &err));
  const FolderOrder back = load_run_order(t.dir);
  REQUIRE(back.size() == 2);
  CHECK(back.at(t.dir) == std::vector<std::wstring>{L"orders", L"a.req.json"});
}

TEST_CASE("rerun failed adds the requests whose captures the failed ones read", "[runner]") {
  std::vector<RunItem> items(4);
  Capture tok;
  tok.var = "token";
  tok.target = "res.body.token";
  items[0].req.name = "Login";
  items[0].req.captures.push_back(tok);
  Capture id;
  id.var = "firstId";
  id.target = "res.body[0].id";
  items[1].req.name = "List items";
  items[1].req.http.headers.push_back({"Authorization", "Bearer {{token}}"});
  items[1].req.captures.push_back(id);
  items[2].req.name = "Get order";
  items[2].req.http.url = "http://h/orders/{{firstId}}";
  items[3].req.name = "Health";
  items[3].req.http.url = "http://h/health";
  const std::vector<size_t> order{0, 1, 2, 3};
  // Get order failed: List items (firstId), and Login (List items reads token) run first.
  CHECK(with_capture_sources(items, order, {2}) == std::vector<size_t>{0, 1, 2});
  // Health reads nothing captured.
  CHECK(with_capture_sources(items, order, {3}) == std::vector<size_t>{3});
  // A capture after the reader does not feed it; a disabled one captures nothing.
  CHECK(with_capture_sources(items, {2, 0, 1, 3}, {0}) == std::vector<size_t>{0});
  items[1].req.captures[0].enabled = false;
  CHECK(with_capture_sources(items, order, {2}) == std::vector<size_t>{2});
}

TEST_CASE("collect again reads the requests as saved now", "[runner]") {
  // The runner re-collects on every Run: an edit, a new file and a deleted one show at once.
  TempWs t;
  SavedRequest a = http("A", "GET", "http://h/json/50");
  t.add(a, "");
  SavedRequest b = http("B", "GET", "http://h/b");
  t.add(b, "");
  REQUIRE(collect(t.ws, {}).size() == 2);
  a.http.url = "http://h/json/9";
  t.add(a, "");
  SavedRequest c = http("C", "GET", "http://h/c");
  t.add(c, "");
  fs::remove(b.file);
  const auto items = collect(t.ws, {});
  REQUIRE(items.size() == 2);
  CHECK(items[0].req.http.url == "http://h/json/9");
  CHECK(items[1].req.name == "C");
}

namespace {

// HKCU\Software\watt\Workspace values, put back after the app run below (it records the
// workspace it opened as the last one).
constexpr const wchar_t* kWsKey = L"Software\\watt\\Workspace";

struct KeepRegValue {
  std::wstring name;
  std::wstring value;
  bool had = false;
  explicit KeepRegValue(const wchar_t* n) : name(n) {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kWsKey, n, RRF_RT_REG_SZ, nullptr, nullptr, &size) == ERROR_SUCCESS) {
      value.resize(size / sizeof(wchar_t));
      had = RegGetValueW(HKEY_CURRENT_USER, kWsKey, n, RRF_RT_REG_SZ, nullptr, value.data(), &size) == ERROR_SUCCESS;
      value.resize(wcsnlen(value.c_str(), value.size()));
    }
  }
  ~KeepRegValue() {
    if (had)
      RegSetKeyValueW(HKEY_CURRENT_USER, kWsKey, name.c_str(), REG_SZ, value.c_str(),
                      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    else
      RegDeleteKeyValueW(HKEY_CURRENT_USER, kWsKey, name.c_str());
  }
};

std::string read_all(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

int json_int(const std::string& line, const std::string& key) {
  const size_t at = line.find("\"" + key + "\":");
  return at == std::string::npos ? -1 : std::stoi(line.substr(at + key.size() + 3));
}

}  // namespace

// The runner window itself (watt.exe next to this test, WATT_RUNNER_LAYOUT_CHECK): nothing
// that shows or hides the "Plan changed" note (a run, typing a filter, unchecking a request, a
// request edited on disk, the next run) may move Iterations, Parallel or Run, or a click aimed at
// Iterations lands in Delay. Then the run-order chips: moving a request (the capture-order hint
// clears, the reset chip turns active) and resetting the order may not move the up / down chips
// (a second click on up would land on down) or the request list's top (a click after Alt+Up would
// pick another row).
TEST_CASE("runner settings never move when the Plan changed note shows or hides", "[runner][app]") {
  wchar_t self[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  const fs::path exe = fs::path(self).parent_path() / "watt.exe";
  if (!fs::exists(exe)) SKIP("watt.exe is not built next to this test");
  KeepRegValue last(L"Last"), recent(L"Recent");
  TempWs t;
  SavedRequest a = http("A", "GET", base() + "/json/3");
  a.asserts.push_back(as("res.status", "eq", "200"));
  a.http.headers.push_back({"X-Id", "{{firstId}}"});  // read before B captures it: the hint shows
  t.add(a, "Shop");
  SavedRequest b = http("B", "GET", base() + "/json/2");
  Capture cap;
  cap.var = "firstId";
  cap.target = "res.status";
  b.captures.push_back(cap);
  t.add(b, "Shop");
  const fs::path out = t.cfg / "layout.jsonl";
  SetEnvironmentVariableW(L"WATT_RUN_FOLDER", (t.dir / "Shop").wstring().c_str());
  SetEnvironmentVariableW(L"WATT_RUNNER_LAYOUT_CHECK", out.wstring().c_str());
  std::wstring cmd = L"\"" + exe.wstring() + L"\" --new-instance \"" + t.dir.wstring() + L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  const BOOL started = CreateProcessW(exe.wstring().c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                      nullptr, &si, &pi);
  SetEnvironmentVariableW(L"WATT_RUN_FOLDER", nullptr);
  SetEnvironmentVariableW(L"WATT_RUNNER_LAYOUT_CHECK", nullptr);
  REQUIRE(started);
  bool edited = false;
  std::string got;
  const auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(70)) {
    got = read_all(out);
    if (!edited && got.find("\"ran\"") != std::string::npos) {
      // Edited on disk while the runner is open: the note appears; the next run clears it.
      a.http.url = base() + "/json/4";
      t.add(a, "Shop");
      edited = true;
    }
    if (got.find("\"done\"") != std::string::npos || got.find("\"timeout\"") != std::string::npos) break;
    if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0 && read_all(out) == got) break;
  }
  if (WaitForSingleObject(pi.hProcess, 5000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 9);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  INFO(got);
  REQUIRE(got.find("\"done\"") != std::string::npos);
  const size_t at = got.find("{\"check\":\"done\"");
  const std::string line = got.substr(at);
  for (const char* step : {"run", "filter", "filter_cleared", "uncheck", "disk_edit", "run_again", "keystroke"})
    CHECK(got.find(std::string("\"step\":\"") + step + "\"") != std::string::npos);
  CHECK(json_int(line, "moved") == 0);                            // no step moved a field
  CHECK(line.find("\"note_filter\":true") != std::string::npos);   // the note did show
  CHECK(line.find("\"note_uncheck\":true") != std::string::npos);
  CHECK(line.find("\"note_after\":false") != std::string::npos);   // and the run cleared it
  for (const char* step : {"reordered", "reset"})
    CHECK(got.find(std::string("\"check\":\"chips\",\"step\":\"") + step + "\"") != std::string::npos);
  CHECK(json_int(line, "chips_moved") == 0);                       // up / down and the list stayed
  CHECK(line.find("\"hint_before\":true") != std::string::npos);   // the capture hint did show
  CHECK(line.find("\"hint_after\":false") != std::string::npos);   // and the move cleared it
}

// Open request on a result of a big parallel run, close that tab with its X, open it again and
// close it again (the runner drives it: WATT_RUNNER_OPEN_CHECK). wx 3.3.1's tab strip kept a
// pointer to the closed tab's close button; the next mouse move wrote into the freed block and a
// later close crashed in ~wxSizer. After each close no hover pointer may be left.
TEST_CASE("open request from a big parallel run: open, close, reopen, close", "[runner][app]") {
  wchar_t self[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  const fs::path exe = fs::path(self).parent_path() / "watt.exe";
  if (!fs::exists(exe)) SKIP("watt.exe is not built next to this test");
  KeepRegValue last(L"Last"), recent(L"Recent");
  TempWs t;
  SavedRequest a = http("List", "GET", base() + "/json/3");
  a.asserts.push_back(as("res.status", "eq", "200"));
  t.add(a, "Shop");
  SavedRequest b = http("Missing", "GET", base() + "/status/404");
  b.asserts.push_back(as("res.status", "eq", "200"));  // fails: the result opened is a failure
  t.add(b, "Shop");
  const fs::path out = t.cfg / "open.jsonl";
  SetEnvironmentVariableW(L"WATT_RUN_FOLDER", (t.dir / "Shop").wstring().c_str());
  SetEnvironmentVariableW(L"WATT_RUNNER_OPEN_CHECK", out.wstring().c_str());
  std::wstring cmd = L"\"" + exe.wstring() + L"\" --new-instance \"" + t.dir.wstring() + L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  const BOOL started = CreateProcessW(exe.wstring().c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                      nullptr, &si, &pi);
  SetEnvironmentVariableW(L"WATT_RUN_FOLDER", nullptr);
  SetEnvironmentVariableW(L"WATT_RUNNER_OPEN_CHECK", nullptr);
  REQUIRE(started);
  std::string got;
  DWORD code = 0;
  const auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(70)) {
    got = read_all(out);
    if (got.find("\"done\"") != std::string::npos || got.find("\"timeout\"") != std::string::npos) break;
    if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) {
      got = read_all(out);
      break;
    }
  }
  if (WaitForSingleObject(pi.hProcess, 5000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 9);
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  INFO(got);
  CHECK(code == 0);  // not an access violation (0xC0000005) or the check's timeout
  REQUIRE(got.find("\"done\"") != std::string::npos);
  CHECK(got.find("{\"check\":\"ran\",\"steps\":600}") != std::string::npos);
  for (const char* n : {"1", "2"}) {
    const std::string line = std::string("{\"check\":\"close\",\"n\":") + n +
                             ",\"hovered\":true,\"hover_left\":false,\"closed\":true}";
    CHECK(got.find(line) != std::string::npos);
  }
}

namespace {

// watt_testserver.exe from the same build dir, on a free port; killed with this process.
struct GrpcServer {
  PROCESS_INFORMATION pi{};
  HANDLE job = nullptr;
  int port = 0;
  GrpcServer() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    const fs::path exe = fs::path(buf).parent_path() / "watt_testserver.exe";
    if (!fs::exists(exe)) return;
    job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
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
    std::wstring cmd = L"\"" + exe.wstring() + L"\" 0";
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
      CloseHandle(rd);
      CloseHandle(wr);
      return;
    }
    AssignProcessToJobObject(job, pi.hProcess);
    CloseHandle(wr);
    const auto line = [&] {
      std::string l;
      char c;
      DWORD n;
      while (ReadFile(rd, &c, 1, &n, nullptr) && n == 1 && c != '\n') l += c;
      return l;
    };
    const std::string grpc = line();
    line();  // the gRPC-Web banner
    CloseHandle(rd);
    if (const auto colon = grpc.rfind(':'); colon != std::string::npos) port = std::atoi(grpc.c_str() + colon + 1);
  }
  ~GrpcServer() {
    if (pi.hProcess) {
      TerminateProcess(pi.hProcess, 0);
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
    }
    if (job) CloseHandle(job);
  }
};

StepResult::Check check(const std::string& target, bool pass, const std::string& from) {
  StepResult::Check c;
  c.target = target;
  c.op = "eq";
  c.pass = pass;
  c.from = from;
  return c;
}

}  // namespace

TEST_CASE("a client-side error or an error status is a failure even when inherited asserts pass", "[runner][grpc]") {
  // The rule itself, without a server.
  StepResult s;
  s.kind = RequestKind::Grpc;
  s.client_error = true;
  s.error = "Request message: invalid JSON in watt.test.CountRequest";
  s.asserts.push_back(check("res.responseTime", true, "Collection"));
  s.asserts_passed = 1;
  CHECK(s.errored());
  CHECK(s.failed());
  // Sent, NOT_FOUND, only the collection's passing time assert: failed.
  StepResult g = s;
  g.client_error = false;
  g.error.clear();
  g.sent = true;
  g.transport_ok = false;
  g.status = 5;
  g.status_text = "NOT_FOUND";
  CHECK_FALSE(g.errored());
  CHECK(g.failed());
  CHECK(g.reason().find("NOT_FOUND") != std::string::npos);
  // An inherited status assert does not excuse it; the request's own does.
  g.asserts.push_back(check("res.status", true, "api"));
  g.asserts_passed = 2;
  CHECK(g.failed());
  g.asserts.back().from.clear();
  CHECK_FALSE(g.failed());
  // HTTP 404 with an inherited passing assert fails; with its own status assert it passes.
  StepResult h;
  h.kind = RequestKind::Http;
  h.sent = h.transport_ok = true;
  h.status = 404;
  h.asserts.push_back(check("res.responseTime", true, "Collection"));
  h.asserts_passed = 1;
  CHECK(h.failed());
  h.asserts.push_back(check("res.status", true, ""));
  h.asserts_passed = 2;
  CHECK_FALSE(h.failed());
  h.status = 200;
  h.asserts.pop_back();
  h.asserts_passed = 1;
  CHECK(h.passed());

  // The real thing: Count with a field CountRequest does not have, under a collection assert.
  GrpcServer srv;
  if (!srv.port) {
    WARN("watt_testserver.exe not next to the test exe: the live part is skipped");
    return;
  }
  TempWs t;
  FolderSettings coll;
  coll.asserts.push_back(as("res.responseTime", "lt", "5000"));
  std::string err;
  REQUIRE(t.ws.save_collection_settings(coll, &err));
  SavedRequest bad;
  bad.name = "Count";
  bad.kind = RequestKind::Grpc;
  bad.grpc_target.address = "127.0.0.1:" + std::to_string(srv.port);
  bad.grpc_method = "/watt.test.Echo/Count";
  bad.grpc_body = R"({"n": 3})";
  t.add(bad, "grpc");
  SavedRequest good = bad;
  good.name = "Say";
  good.grpc_method = "/watt.test.Echo/Say";
  good.grpc_body = R"({"message": "hi"})";
  t.add(good, "grpc");
  const auto items = collect(t.ws, t.dir / "grpc");
  REQUIRE(items.size() == 2);
  Workspace w = t.ws;
  RunControl ctl;
  RunOptions o;
  std::vector<std::shared_ptr<const StepResult>> seen;
  const GrpcFactory f = [] { return std::make_shared<GrpcClient>(); };
  const auto sum = run_plan(items, all(items.size()), o, w, f, ctl, [&](auto r) { seen.push_back(r); });
  REQUIRE(seen.size() == 2);
  for (const auto& r : seen) {
    INFO(r->name << ": " << r->error);
    if (r->name == "Count") {
      CHECK(r->client_error);
      CHECK_FALSE(r->sent);
      CHECK(r->failed());
      CHECK(r->error.find("Request message") != std::string::npos);
    } else {
      CHECK(r->passed());
    }
  }
  CHECK(sum.passed == 1);
  CHECK(sum.failed == 1);
  CHECK(sum.errors == 1);
}

TEST_CASE("run history masks credential values and keeps only failures of a big run", "[runner][history]") {
  std::vector<std::shared_ptr<const StepResult>> steps;
  for (size_t i = 0; i < kHistorySteps + 10; ++i) {
    auto s = std::make_shared<StepResult>();
    s->seq = i;
    s->rel = "r";
    s->sent = true;
    s->transport_ok = true;
    s->status = i % 1000 == 0 ? 500 : 200;  // a few failures
    s->timing.total_ms = 1;
    StepResult::Cap c;
    c.ok = true;
    c.var = "authToken";
    c.value = "s3cret";
    s->captures.push_back(c);
    steps.push_back(std::move(s));
  }
  RunMeta meta;
  const RunSummary sum = summarize(steps, steps.size(), 10, false, true);
  CHECK(to_json(meta, sum, {steps.front()}).find("s3cret") != std::string::npos);  // an export keeps it
  const std::string h = history_json(meta, sum, steps);
  CHECK(h.find("s3cret") == std::string::npos);
  LoadedRun lr;
  REQUIRE(from_json(h, lr));
  CHECK(lr.partial);
  CHECK(lr.steps.size() == 6);
  for (const auto& s : lr.steps) CHECK(s->failed());
  CHECK(lr.summary.total == steps.size());
}

TEST_CASE("failure causes count the steps sent with unresolved variables", "[runner]") {
  std::vector<std::shared_ptr<const StepResult>> steps;
  for (int i = 0; i < 4; ++i) {
    auto s = std::make_shared<StepResult>();
    s->seq = static_cast<size_t>(i);
    s->iteration = i;
    s->rel = "get";
    s->sent = true;
    s->transport_ok = true;
    s->status = 404;
    if (i < 3) s->unresolved = {"firstId"};
    steps.push_back(std::move(s));
  }
  auto g = group_failures(steps);
  REQUIRE(g.size() == 1);
  CHECK(g[0].check == "expected status < 400");
  CHECK(g[0].count == 4);
  CHECK(g[0].unresolved == 3);
  REQUIRE(g[0].unresolved_names.size() == 1);
  CHECK(g[0].unresolved_names[0] == "firstId");
}

TEST_CASE("collect runs a folder in the tree's stored order, not by name", "[runner]") {
  TempWs t;
  const std::vector<std::string> names = {"List orders", "Track shipment", "Get order", "Create order",
                                          "Cancel order", "Refund order", "Ship order", "Track by id"};
  for (const auto& n : names) {
    SavedRequest r = http(n, "GET", base() + "/echo");
    t.add(r, "Shop API/orders");
  }
  SavedRequest sub = http("Nested", "GET", base() + "/echo");
  t.add(sub, "Shop API/orders/sub");
  std::vector<std::string> order;
  for (const auto& n : names) order.push_back(n + ".req.json");
  REQUIRE(t.ws.save_folder_order(t.dir / "Shop API/orders", order));
  // The tree's depth-first order is what Run folder must run.
  std::vector<std::string> expect;
  std::function<void(const CollectionNode&)> walk = [&](const CollectionNode& n) {
    for (const auto& c : n.children) {
      if (c.is_folder) walk(c);
      else expect.push_back(fs::relative(c.path, t.dir / "Shop API/orders").generic_string());
    }
  };
  const CollectionNode tree = t.ws.tree(-1);
  const CollectionNode* orders = find_node(tree, path_key(t.dir / "Shop API/orders"));
  REQUIRE(orders);
  walk(*orders);
  auto items = collect(t.ws, t.dir / "Shop API/orders");
  REQUIRE(items.size() == expect.size());
  for (size_t i = 0; i < items.size(); ++i) CHECK(items[i].rel == strip_ext(expect[i]));
  // And that order is the stored one (folders first), not alphabetical.
  REQUIRE(items.size() == 9);
  CHECK(items[0].rel == "sub/Nested");
  for (size_t i = 0; i < names.size(); ++i) CHECK(items[i + 1].rel == names[i]);
}
