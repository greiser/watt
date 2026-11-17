// Collection runner engine (src/app/runner_engine.cpp, wx-free, compiled in directly) against an
// in-process watt_httpserver: order, iterations, captures feeding later requests, asserts,
// stop, consent, summary percentiles and the JSON / JUnit exports.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <winsock2.h>

#include <chrono>
#include <filesystem>
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
  // Phases add up to the total.
  for (const auto& r : seen) {
    double sum = 0;
    for (const auto& p : r->phases) sum += p.ms;
    CHECK(sum == Catch::Approx(r->timing.total_ms).margin(0.01));
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
  for (const auto& r : seen) {
    double sum = 0;
    for (const auto& p : r->phases) sum += p.ms;
    CHECK(sum == Catch::Approx(r->timing.total_ms).margin(0.01));
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
