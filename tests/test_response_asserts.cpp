// Response view Tests tab helpers (wx-free, compiled in directly): assert target -> JSON path,
// and locating that value in a pretty or raw JSON text for "reveal in Body".
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "app/response_tests.cpp"

using namespace watt::app::rv;

TEST_CASE("assert targets become JSON paths", "[response_asserts]") {
  std::vector<PathStep> p;
  long msg = 0;
  REQUIRE(target_json_path("res.body.items[1].id", p, &msg));
  CHECK(msg == -1);
  CHECK(path_string(p) == "$.items[1].id");
  REQUIRE(target_json_path("res.body[\"a.b\"][-1]", p, &msg));
  CHECK(path_string(p) == "$[\"a.b\"][-1]");
  REQUIRE(target_json_path("res.body.items.length", p, &msg));
  CHECK(path_string(p) == "$.items");
  REQUIRE(target_json_path("res.messages[2].name", p, &msg));
  CHECK(msg == 2);
  CHECK(path_string(p) == "$.name");
  REQUIRE(target_json_path("res.body", p, &msg));
  CHECK(p.empty());
  CHECK_FALSE(target_json_path("res.status", p, &msg));
  CHECK_FALSE(target_json_path("res.headers.content-type", p, &msg));
  CHECK_FALSE(target_json_path("res.body.items[", p, &msg));
}

TEST_CASE("json_locate finds values in pretty and raw text", "[response_asserts]") {
  const std::string raw = R"({"a":{"b\"c":[10, {"d":"x,]}"}, 30]},"e":1})";
  auto key = [](const char* k) { return PathStep{k}; };
  auto idx = [](long i) {
    PathStep s;
    s.is_index = true;
    s.index = i;
    return s;
  };
  size_t len = 0;
  // The value at a path (located first: argument order is unspecified).
  auto at_path = [&](const std::string& text, const std::vector<PathStep>& p) {
    size_t l = 0;
    const size_t a = json_locate(text, p, &l);
    return a == std::string::npos ? std::string("<none>") : text.substr(a, l);
  };
  const size_t at = json_locate(raw, {key("a"), key("b\"c"), idx(1), key("d")}, &len);
  REQUIRE(at != std::string::npos);
  CHECK(raw.substr(at, len) == "\"x,]}\"");
  CHECK(at_path(raw, {key("a"), key("b\"c"), idx(2)}) == "30");
  CHECK(at_path(raw, {key("a"), key("b\"c"), idx(-1)}) == "30");
  CHECK(at_path(raw, {key("e")}) == "1");
  size_t deep = 0;
  CHECK(json_locate(raw, {key("a"), key("zz")}, nullptr, &deep) == std::string::npos);
  CHECK(raw.substr(deep, 3) == "\"a\"");
  const std::string pretty = "{\n  \"a\": [\n    1,\n    2\n  ]\n}\n";
  CHECK(at_path(pretty, {key("a"), idx(1)}) == "2");
  CHECK(json_locate(pretty, {key("a"), idx(2)}) == std::string::npos);
}

TEST_CASE("capture variable names follow the core rules", "[response_asserts]") {
  CHECK(capture_var_problem("token", 256).empty());
  CHECK(capture_var_problem("user.id-2", 256).empty());
  CHECK(capture_var_problem("a b", 256).empty());  // inner spaces are allowed, as in core
  CHECK_FALSE(capture_var_problem("", 256).empty());
  CHECK_FALSE(capture_var_problem(" token", 256).empty());
  CHECK_FALSE(capture_var_problem("token ", 256).empty());
  CHECK_FALSE(capture_var_problem("{{x}}", 256).empty());
  CHECK_FALSE(capture_var_problem("a}}b", 256).empty());
  CHECK_FALSE(capture_var_problem(std::string("a\x01" "b"), 256).empty());
  CHECK_FALSE(capture_var_problem(std::string(257, 'x'), 256).empty());
  CHECK(capture_var_problem(std::string(256, 'x'), 256).empty());
}

TEST_CASE("json_path_miss says why a path is not there", "[response_asserts]") {
  std::vector<PathStep> p;
  long msg = 0;
  const std::string arr = R"([{"id":1,"name":"a"},{"id":2}])";
  REQUIRE(target_json_path("res.body.items[0].name", p, &msg));
  CHECK(json_path_miss(arr, p) == "res.body is an array: try res.body[0].items");
  REQUIRE(target_json_path("res.body[5].id", p, &msg));
  CHECK(json_path_miss(arr, p) == "res.body has 2 elements, no [5]");
  REQUIRE(target_json_path("res.body[1].name", p, &msg));
  CHECK(json_path_miss(arr, p) == "res.body[1] has no member \"name\"");
  REQUIRE(target_json_path("res.body[0].name", p, &msg));
  CHECK(json_path_miss(arr, p).empty());
  const std::string obj = R"({"a":{"b":"x"}})";
  REQUIRE(target_json_path("res.body.a[0]", p, &msg));
  CHECK(json_path_miss(obj, p) == "res.body.a is an object, not an array");
  REQUIRE(target_json_path("res.body.a.b.c", p, &msg));
  CHECK(json_path_miss(obj, p) == "res.body.a.b is a string, not an object");
  CHECK(json_path_miss("not json", p).empty());
}

#include "app/response_filter.cpp"

TEST_CASE("json_filter evaluates JSONPath over a body", "[response_filter]") {
  const std::string b = R"({"items":[{"id":1,"name":"a","tags":["x"]},{"id":2,"name":"b"},{"id":3}],"n":{"name":"c"}})";
  auto f = [&](const char* e) { return json_filter(b, e); };
  CHECK(f("$.items[0].name").json == "\"a\"");
  CHECK(f("$.items[*].name").matches == 2);
  CHECK(f("$.items[*].name").json == "[\n  \"a\",\n  \"b\"\n]");
  CHECK(f("$..name").matches == 3);
  CHECK(f("$.items[-1].id").json == "3");
  CHECK(f("$.items[0,2].id").json == "[\n  1,\n  3\n]");
  CHECK(f("$.items[1:].id").matches == 2);
  CHECK(f("$.items[:1].id").json == "1");
  CHECK(f("$.items.length").json == "3");
  CHECK(f("res.body.n['name']").json == "\"c\"");
  CHECK(f(".n.name").json == "\"c\"");
  CHECK(f("$.n.*").json == "\"c\"");
  CHECK(f("$..*").matches == 13);
  CHECK(f("$.missing").ok);
  CHECK(f("$.missing").matches == 0);
  CHECK(f("$.items[?(@.id>1)]").matches == 2);
  CHECK_FALSE(f("items").ok);
  CHECK_FALSE(f("$.items[").ok);
  CHECK_FALSE(json_filter("not json", "$").ok);
  CHECK(json_filter_check("$.a[0]['b c']").empty());
  CHECK_FALSE(json_filter_check("$.a[").empty());
  // Deep input does not overflow the stack (parse iterative, walks explicit).
  std::string deep(200000, '[');
  deep += std::string(200000, ']');
  CHECK(json_filter(deep, "$..*").matches > 0);
  CHECK_FALSE(json_filter(deep, "$").ok);  // too deep to show
  // Output cap.
  const FilterResult capped = json_filter(b, "$", 10);
  CHECK(capped.truncated);
  CHECK(capped.json.size() == 10);
}

TEST_CASE("json_table builds rows and columns", "[response_filter]") {
  TableModel t = json_table(R"({"total":2,"items":[{"id":1,"name":"a"},{"id":2,"tags":["x",1],"o":{"k":null}}]})");
  CHECK(t.source == "$.items");
  REQUIRE(t.cols == std::vector<std::string>{"id", "name", "tags", "o"});
  REQUIRE(t.cells.size() == 2);
  CHECK(t.cells[0][1] == "a");
  CHECK(t.cells[1][1].empty());
  CHECK(t.cells[1][2] == "[\"x\",1]");
  CHECK(t.cells[1][3] == "{\"k\":null}");
  TableModel s = json_table("[1, 2.5, \"x\"]");
  CHECK(s.cols == std::vector<std::string>{"value"});
  CHECK(s.cells[1][0] == "2.5");
  TableModel kv = json_table(R"({"a":1,"b":"two"})");
  CHECK(kv.cols == std::vector<std::string>{"key", "value"});
  CHECK(kv.cells[1][1] == "two");
  CHECK_FALSE(json_table("<html>").error.empty());
  CHECK(table_cell_less("2", "10"));
  CHECK(table_cell_less("10", "abc"));
  CHECK(table_cell_less("Abc", "abd"));
  CHECK(table_cell_less("x", ""));
}

TEST_CASE("json_filter accepts .[ as [", "[response_filter]") {
  CHECK(json_filter(R"([{"name":"a"},{"name":"b"}])", "$.[*].name").matches == 2);
  CHECK(json_filter(R"([{"name":"a"},{"name":"b"}])", "$.[1].name").json == "\"b\"");
}

TEST_CASE("json_filter keeps the response's number text", "[response_filter]") {
  const std::string b = R"({"a":[{"p":0.00,"q":1.50},{"p":1e3,"q":-0.0}],"big":12345678901234567890123,"s":"1.50"})";
  CHECK(json_filter(b, "$.a[0].p").json == "0.00");
  CHECK(json_filter(b, "$.a[*].q").json == "[\n  1.50,\n  -0.0\n]");
  CHECK(json_filter(b, "$.a[1]").json == "{\n  \"p\": 1e3,\n  \"q\": -0.0\n}");
  CHECK(json_filter(b, "$.big").json == "12345678901234567890123");
  CHECK(json_filter(b, "$.s").json == "\"1.50\"");  // a string stays a string
  // A string that starts with NUL is not mistaken for a number.
  CHECK(json_filter(R"({"x":"\u0000N5"})", "$.x").json == "\"\\u0000N5\"");
  TableModel t = json_table(R"([{"price":1.50,"n":"0.00"}])");
  CHECK(t.cells[0][0] == "1.50");
  CHECK(t.cells[0][1] == "0.00");
}

TEST_CASE("json_filter supports [?()] filter expressions", "[response_filter]") {
  const std::string b = R"({"items":[{"id":1,"name":"apple","price":1.50,"tags":["x"],"ok":true},
    {"id":2,"name":"Banana","price":0.25,"ok":false},{"id":3,"name":"cherry","price":10,"meta":{"c":"red"}},
    {"id":4,"name":"","price":null}],"limit":2})";
  auto n = [&](const char* e) { return json_filter(b, e).matches; };
  auto j = [&](const char* e) { return json_filter(b, e).json; };
  CHECK(n("$.items[?(@.price < 1)]") == 1);
  CHECK(j("$.items[?(@.price < 1)].name") == "\"Banana\"");
  CHECK(n("$.items[?(@.price >= 1.5)]") == 2);
  CHECK(j("$.items[?(@.name == 'cherry')].id") == "3");
  CHECK(j("$.items[?(@.name === \"apple\")].id") == "1");
  CHECK(n("$.items[?(@.name != 'apple')]") == 3);
  CHECK(n("$.items[?(@.ok)]") == 1);           // truthy
  CHECK(n("$.items[?(!@.ok)]") == 3);
  CHECK(n("$.items[?(@.tags)]") == 1);         // exists (an array is truthy)
  CHECK(n("$.items[?(@.name)]") == 3);         // "" is falsy
  CHECK(n("$.items[?(@.price == null)]") == 1);
  CHECK(n("$.items[?(@.id > 1 && @.id < 4)]") == 2);
  CHECK(n("$.items[?(@.id == 1 || (@.id == 3 && @.meta.c == 'red'))]") == 2);
  CHECK(n("$.items[?(@.name =~ /^b/i)]") == 1);
  CHECK(n("$.items[?(@.id <= $.limit)]") == 2);  // $ is the body
  CHECK(n("$.items[?(@.tags.length == 1)]") == 1);
  CHECK(n("$.items[?(@['name'] == 'apple')]") == 1);
  CHECK(n("$.items[?@.id == 2]") == 1);         // RFC 9535: parentheses optional
  CHECK(n("$..[?(@.c == 'red')]") == 1);        // any depth
  CHECK(n("$.items[?(@.price > 'a')]") == 0);   // number vs string: false
  CHECK(j("$.items[?(@.id == 1)].price") == "1.50");
  CHECK_FALSE(json_filter_check("$.items[?(@.id >)]").empty());
  CHECK_FALSE(json_filter_check("$.items[?(@.id == 1]").empty());
  CHECK_FALSE(json_filter_check("$.items[?(@.x =~ /(/)]").empty());
  CHECK_FALSE(json_filter_check("$.items[?(foo)]").empty());
  std::string deep = "$.a[?(";
  for (int i = 0; i < 100; ++i) deep += "(";
  deep += "@.x";
  for (int i = 0; i < 100; ++i) deep += ")";
  deep += ")]";
  CHECK_FALSE(json_filter_check(deep).empty());  // too complex, not a stack overflow
}

// ------------------------------------------------------------------ wrap on the worker
#include "app/response_format.cpp"

TEST_CASE("line stats count lines and the widest line in characters", "[response_wrap]") {
  namespace fmt = watt::app::fmt;
  auto st = fmt::line_stats("ab\n\xC3\xA9\xC3\xA9\xC3\xA9\n\tx");
  CHECK(st.lines == 3);
  CHECK(st.longest == 5);  // tab = 4 + x
  st = fmt::line_stats("");
  CHECK(st.lines == 1);
  CHECK(st.longest == 0);
}

TEST_CASE("breaks are recorded and styles follow them", "[response_wrap]") {
  namespace fmt = watt::app::fmt;
  const std::string src = "{\n  \"s\": \"aaaa bbbb cccc dddd eeee\",\n  \"n\": 1\n}";
  std::string out;
  std::vector<size_t> breaks;
  const size_t n = fmt::break_long_lines(src, out, 12, &breaks);
  REQUIRE(n == breaks.size());
  REQUIRE(n > 0);
  CHECK(out.size() == src.size() + n);
  // Removing the inserted newlines gives the source back.
  std::string back;
  size_t from = 0;
  for (size_t i = 0; i < breaks.size(); ++i) {
    CHECK(out[breaks[i] + i] == '\n');
    back += out.substr(from + i, breaks[i] - from);
    from = breaks[i];
  }
  back += out.substr(from + breaks.size());
  CHECK(back == src);

  std::string styles, wstyles;
  std::vector<int> levels, wlevels;
  fmt::style_json(src, styles, levels);
  fmt::style_json(out, wstyles, wlevels);
  fmt::expand_styles_for_breaks(src, breaks, styles, levels);
  REQUIRE(styles.size() == out.size());
  // Every line of the broken text has a level; the string keeps its style past each break.
  size_t lines = 1;
  for (char c : out) lines += c == '\n';
  CHECK(levels.size() == lines);
  const size_t q = out.find('"', out.find(':')) + 1;  // inside the string value
  for (size_t i = 0; i < breaks.size(); ++i) {
    const size_t at = breaks[i] + i;  // the inserted newline
    if (at > q && at < out.rfind('"', out.find("\"n\"") - 1)) CHECK(styles[at + 1] == styles[q]);
  }
  CHECK(levels.front() == wlevels.front());  // the root header is unchanged
}

// ------------------------------------------------------------------ restored runs (response_keep)

#include <filesystem>

#include "app/response_keep.cpp"

TEST_CASE("run extras round-trip and restore the run as shown live", "[response_asserts]") {
  using namespace watt;
  HttpResponse live;
  live.ok = true;
  live.status = 200;
  live.status_text = "OK";
  live.http_version = "HTTP/2";
  live.headers.add("content-type", "application/json");
  live.body = std::string(1000, 'x');
  live.effective_url = "https://example.com/items";
  live.timing.queue_ms = 25837.0 + 1.5;
  live.timing.ttfb_ms = 7.0;
  live.timing.total_ms = 25837.0 + 9.39;
  live.timing.connection_reused = true;
  live.redirect_count = 1;
  RedirectHop hop;
  hop.url = "https://example.com/r";
  hop.status = 302;
  hop.total_ms = 3.25;
  live.hops.push_back(hop);
  auto tls = std::make_shared<TlsInfo>();
  tls->version = "TLSv1.3";
  tls->cipher = "TLS_AES_256_GCM_SHA384";
  tls->leaf.subject_alt_names = {"example.com", "www.example.com"};
  tls->leaf.subject = "CN=example.com";
  live.conn.tls = true;
  live.conn.tls_info = tls;

  RunExtras x;
  x.prompt_ms = 25837.0;
  auto post = std::make_shared<ScriptRun>();
  post->ran = true;
  post->elapsed_us = 8480;
  post->tests.push_back({"has items", true, ""});
  post->tests.push_back({"first is a", false, "expected 'b' to equal 'a'"});
  post->logs.push_back({ScriptLog::Level::Warn, "two\nlines"});
  post->vars_set.push_back({ScriptVar::Scope::Runtime, "token", "s3cret", false, true, ""});
  x.post = post;
  x.head = response_head(live);
  x.body_size = live.body.size();
  CHECK(x.head->body.empty());

  const auto back = extras_from_json(extras_to_json(x));
  REQUIRE(back);
  CHECK(back->prompt_ms == 25837.0);
  CHECK(back->body_size == 1000);
  REQUIRE(back->post);
  CHECK(back->post->ran);
  REQUIRE(back->post->tests.size() == 2);
  CHECK(back->post->tests[1].message == "expected 'b' to equal 'a'");
  CHECK(back->post->logs[0].text == "two\nlines");
  CHECK(back->post->logs[0].level == ScriptLog::Level::Warn);
  // Variable values are never written to disk.
  REQUIRE(back->post->vars_set.size() == 1);
  CHECK(back->post->vars_set[0].name == "token");
  CHECK(back->post->vars_set[0].value != "s3cret");
  CHECK(extras_to_json(x).find("s3cret") == std::string::npos);
  CHECK_FALSE(back->pre);

  // The history keeps the body and the shell's timing, not TLS / hops: merged back, the dialog
  // time taken out.
  HttpResponse kept = live;
  kept.conn = {};
  kept.hops.clear();
  kept.tls_version.clear();
  const auto r = restore_response(&kept, *back);
  REQUIRE(r);
  CHECK(r->body == live.body);
  CHECK(std::abs(r->timing.total_ms - 9.39) < 1e-9);
  CHECK(std::abs(r->timing.queue_ms - 1.5) < 1e-9);
  CHECK(r->tls_version == "TLSv1.3");
  CHECK(r->cert_san == "example.com, www.example.com");
  REQUIRE(r->hops.size() == 1);
  CHECK(r->hops[0].status == 302);
  CHECK(r->conn.reused);

  // Body not kept: rebuilt from the head alone.
  const auto h = restore_response(nullptr, *back);
  REQUIRE(h);
  CHECK(h->body.empty());
  CHECK(h->status == 200);
  CHECK(std::string(h->headers.get("content-type")) == "application/json");
  CHECK(std::abs(h->timing.total_ms - 9.39) < 1e-9);

  // A dialog time larger than the queue (wrong match) is not subtracted.
  RunExtras odd = *back;
  odd.prompt_ms = 1e9;
  CHECK(restore_response(&kept, odd)->timing.total_ms == kept.timing.total_ms);
}

TEST_CASE("run keys and the extras store", "[response_asserts]") {
  const std::string k = run_key("GET https://x/{{id}}", 200, 25846.39);
  CHECK(k.size() == 16);
  CHECK(k == run_key("GET https://x/{{id}}", 200, 25846.3900000001));
  CHECK(k != run_key("GET https://x/{{id}}", 201, 25846.39));
  CHECK(k != run_key("POST https://x/{{id}}", 200, 25846.39));
  CHECK_FALSE(extras_from_json("{}"));
  CHECK_FALSE(extras_from_json("not json"));

  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / ("watt_rv_keep_" + k);
  fs::remove_all(root);
  fs::create_directories(root / ".watt");
  RunExtras x;
  x.prompt_ms = 42;
  keep_extras(root, k, x);
  CHECK(find_extras_cached(k));
  // Written on a worker: wait for the file.
  const fs::path f = root / ".watt" / "response-runs" / (k + ".json");
  for (int i = 0; i < 200 && !fs::exists(f); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  REQUIRE(fs::exists(f));
  std::string json;
  REQUIRE(watt::detail::read_file(f, json));
  const auto y = extras_from_json(json);
  REQUIRE(y);
  CHECK(y->prompt_ms == 42);
  // Keys that are not run_key output never reach the file system.
  keep_extras(root, "../../evil", x);
  CHECK_FALSE(find_extras(root, "../../evil"));
  CHECK_FALSE(load_kept_body(root, "../../evil", 3));

  // The whole body is kept next to the extras and read back only at exactly its size.
  auto full = std::make_shared<watt::HttpResponse>();
  full->body = std::string(200000, 'b');
  const std::string k2 = run_key("GET https://x/big", 200, 12.5);
  keep_extras(root, k2, x, full);
  CHECK(find_extras_cached(k2)->key == k2);
  const fs::path bf = root / ".watt" / "response-bodies" / (k2 + ".body");
  for (int i = 0; i < 200 && !(fs::exists(bf) && fs::file_size(bf) == 200000); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  const auto back = load_kept_body(root, k2, 200000);
  REQUIRE(back);
  CHECK(*back == full->body);
  CHECK_FALSE(load_kept_body(root, k2, 199999));
  CHECK_FALSE(load_kept_body(root, run_key("GET https://x/other", 200, 1), 200000));
  fs::remove_all(root);
}

TEST_CASE("a saved request's last run is its own, not another request's run of the same URL",
          "[response_asserts]") {
  using namespace watt;
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / "watt_rv_runs_of_request";
  fs::remove_all(root);
  Workspace ws = Workspace::create(root);
  auto run = [&](const char* id, const char* name, const std::string& url, double ms) {
    HistoryEntry e;
    e.kind = RequestKind::Http;
    e.summary = "GET " + url;
    e.status = 200;
    e.total_ms = ms;
    e.request.kind = RequestKind::Http;
    e.request.id = id;
    e.request.name = name;
    e.request.http.method = "GET";
    e.request.http.url = url;
    ws.append_history(e);
  };
  const std::string url = "http://127.0.0.1:1/json/51";
  run("r02", "02 Items", url, 1.41);  // oldest
  run("r19", "19 Json", url, 2.49);
  run("", "", url, 3.0);               // sent from an Untitled tab
  run("r02", "02 Items", "http://127.0.0.1:1/other", 4.0);
  run("", "old", url, 5.0);            // an older file without an id

  SavedRequest items;
  items.kind = RequestKind::Http;
  items.id = "r02";
  items.name = "02 Items";
  items.http.url = url;
  std::vector<HistoryEntry> h = ws.history(200);
  REQUIRE(h.size() == 5);
  keep_runs_of(ws, items, "GET " + url, h);
  REQUIRE(h.size() == 1);
  CHECK(h[0].total_ms == 1.41);

  SavedRequest json = items;
  json.id = "r19";
  json.name = "19 Json";
  h = ws.history(200);
  keep_runs_of(ws, json, "GET " + url, h);
  REQUIRE(h.size() == 1);
  CHECK(h[0].total_ms == 2.49);

  // Never sent itself: nothing (not the Untitled run of the same URL).
  SavedRequest example = items;
  example.id = "r01";
  example.name = "01 Example";
  h = ws.history(200);
  keep_runs_of(ws, example, "GET " + url, h);
  CHECK(h.empty());

  // No id: matched by name among the runs stored without an id.
  SavedRequest old = items;
  old.id.clear();
  old.name = "old";
  h = ws.history(200);
  keep_runs_of(ws, old, "GET " + url, h);
  REQUIRE(h.size() == 1);
  CHECK(h[0].total_ms == 5.0);
  fs::remove_all(root);
}

// ------------------------------------------------------------------ last run of a saved request

TEST_CASE("a saved request restores its own last run, not another one with the same URL", "[response_asserts]") {
  using namespace watt;
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path root = fs::temp_directory_path() / ("watt_rv_keep_runs_" + std::to_string(reinterpret_cast<uintptr_t>(&ec)));
  fs::remove_all(root, ec);
  Workspace ws = Workspace::create(root);
  auto run = [&](const std::string& id, const std::string& name, double ms) {
    HistoryEntry e;
    e.when_iso = "2026-11-17T10:00:00Z";
    e.kind = RequestKind::Http;
    e.request.id = id;
    e.request.name = name;
    e.request.http.method = "GET";
    e.request.http.url = "http://127.0.0.1:1/json/51";
    e.summary = "GET " + e.request.http.url;
    e.status = 200;
    e.total_ms = ms;
    ws.append_history(e);
  };
  run("r02", "02 Items", 1.41);
  run("r19", "19 Json", 2.49);
  run("", "", 3.0);  // an Untitled tab's ad-hoc run of the same URL
  SavedRequest items;
  items.id = "r02";
  items.name = "02 Items";
  items.http.method = "GET";
  items.http.url = "http://127.0.0.1:1/json/51";
  const std::string summary = "GET " + items.http.url;

  std::vector<HistoryEntry> h = ws.history(50);
  REQUIRE(h.size() == 3);
  keep_runs_of(ws, items, summary, h);
  REQUIRE(h.size() == 1);
  CHECK(h[0].total_ms == 1.41);

  SavedRequest json = items;
  json.id = "r19";
  json.name = "19 Json";
  h = ws.history(50);
  keep_runs_of(ws, json, summary, h);
  REQUIRE(h.size() == 1);
  CHECK(h[0].total_ms == 2.49);

  SavedRequest other = items;
  other.id = "r99";  // never sent: no run, not the Untitled one
  h = ws.history(50);
  keep_runs_of(ws, other, summary, h);
  CHECK(h.empty());
  fs::remove_all(root, ec);
}

// ------------------------------------------------------------------ Cookies tab (response_cookies)

#include "app/response_cookies.cpp"

TEST_CASE("set-cookie rows read like the jar stores them", "[response_cookies]") {
  const int64_t now = 1'800'000'000;
  const std::vector<std::pair<std::string, std::string>> h = {
      {"Content-Type", "text/plain"},
      {"Set-Cookie", "sid=abc123; Path=/; HttpOnly; SameSite=Lax"},
      {"set-cookie", "theme=dark; Max-Age=3600; Domain=.example.com"},
      {"SET-COOKIE", "old=1; Max-Age=0"},
      {"Set-Cookie", "evil=1; Domain=other.org"},
      {"Set-Cookie", "sec=1; Secure"},
      {"Set-Cookie", "part=1; Secure; Partitioned; Path=/a"},
  };
  const auto rows = set_cookie_rows(h, "http://api.example.com/v1/users?x=1", now);
  REQUIRE(rows.size() == 6);
  CHECK(rows[0].name == "sid");
  CHECK(rows[0].value == "abc123");
  CHECK(rows[0].domain == "api.example.com");
  CHECK(rows[0].host_only);
  CHECK(rows[0].path == "/");
  CHECK(rows[0].expires == "Session");
  CHECK(cookie_flags_text(rows[0]) == "HttpOnly Lax");
  CHECK(cookie_fate_text(rows[0]) == "stored");
  CHECK(rows[0].size == 9);

  CHECK(rows[1].domain == "example.com");
  CHECK_FALSE(rows[1].host_only);
  CHECK(rows[1].path == "/v1");  // the default path of /v1/users
  CHECK(rows[1].expires == "in 1 h");
  CHECK(rows[1].expires_at == now + 3600);

  CHECK(rows[2].fate == SetCookieRow::Fate::Deletes);
  CHECK(cookie_fate_text(rows[2]) == "deletes it");

  CHECK(rows[3].fate == SetCookieRow::Fate::Ignored);
  CHECK(cookie_fate_text(rows[3]) == "ignored: Domain does not match the request host");
  CHECK(rows[3].domain == "other.org");

  // Secure from plain http to a non-loopback host: the jar ignores it.
  CHECK(rows[4].fate == SetCookieRow::Fate::Ignored);
  CHECK(rows[4].secure);

  CHECK(rows[5].partitioned);
  CHECK(cookie_flags_text(rows[5]) == "Secure Partitioned");
}

TEST_CASE("set-cookie rows from https and loopback, and expiry texts", "[response_cookies]") {
  const int64_t now = 1'800'000'000;
  auto rows = set_cookie_rows({{"Set-Cookie", "__Host-a=1; Secure; Path=/"}}, "https://x.test/", now);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].fate == SetCookieRow::Fate::Stored);
  rows = set_cookie_rows({{"Set-Cookie", "__Host-a=1; Secure; Path=/x"}}, "https://x.test/", now);
  CHECK(rows[0].fate == SetCookieRow::Fate::Ignored);
  // Loopback http counts as secure (core/cookies.h); the port is not part of the domain.
  rows = set_cookie_rows({{"Set-Cookie", "s=1; Secure"}}, "http://127.0.0.1:18080/a", now);
  CHECK(rows[0].fate == SetCookieRow::Fate::Stored);
  CHECK(rows[0].domain == "127.0.0.1");
  CHECK(set_cookie_rows({{"X-Other", "a=1"}}, "http://h/", now).empty());
  CHECK(cookie_expiry_text(0, now) == "Session");
  CHECK(cookie_expiry_text(now - 5, now) == "expired");
  CHECK(cookie_expiry_text(now + 30, now) == "in 30 s");
  CHECK(cookie_expiry_text(now + 600, now) == "in 10 min");
  CHECK(cookie_expiry_text(now + 3 * 86400, now) == "in 3 days");
  CHECK(cookie_expiry_text(1'900'000'000 + 0, 1'800'000'000).size() == 10);  // a date
}

// ------------------------------------------------------------------ Visualize (pm.visualizer)

#include "app/response_visualize.cpp"
#include "core/asserts.h"
#include "core/script.h"

TEST_CASE("handlebars templates render like Postman's visualizer", "[response_visualize]") {
  const std::string data =
      R"({"response":[{"name":"a<b","n":1,"tags":["x","y"]},{"name":"c","n":0,"tags":[]}],"title":"T","empty":[],)"
      R"("obj":{"k1":"v1","k2":"v2"},"zero":0,"f":1.5})";
  auto render = [&](const std::string& t) {
    VisualizeResult r = render_handlebars(t, data);
    INFO(r.error);
    REQUIRE(r.ok);
    return r.html;
  };
  CHECK(render("<table>{{#each response}}<tr><td>{{name}}</td></tr>{{/each}}</table>") ==
        "<table><tr><td>a&lt;b</td></tr><tr><td>c</td></tr></table>");
  CHECK(render("{{{response.0.name}}}|{{& response.[0].name}}|{{response.1.name}}") == "a<b|a<b|c");
  CHECK(render("{{#each response}}{{@index}}:{{n}}{{#if @first}}F{{/if}}{{#if @last}}L{{/if}},{{/each}}") ==
        "0:1F,1:0L,");
  CHECK(render("{{#each obj}}{{@key}}={{this}};{{/each}}") == "k1=v1;k2=v2;");
  CHECK(render("{{#each empty}}x{{else}}none{{/each}}") == "none");
  CHECK(render("{{#if zero}}y{{else}}n{{/if}}{{#if empty}}y{{^}}n{{/if}}{{#unless title}}u{{/unless}}") == "nn");
  CHECK(render("{{#if missing}}1{{else if title}}2{{else}}3{{/if}}") == "2");
  CHECK(render("{{#with obj}}{{k2}}{{/with}}") == "v2");
  CHECK(render("{{#each response}}{{#each tags}}{{../name}}-{{this}} {{/each}}{{/each}}") == "a&lt;b-x a&lt;b-y ");
  CHECK(render("{{#each response as |row i|}}{{i}}{{row.name}}{{/each}}") == "0a&lt;b1c");
  CHECK(render("{{response.length}} {{response.0.tags}} {{f}} {{@root.title}}") == "2 x,y 1.5 T");
  CHECK(render("{{lookup obj 'k1'}}{{! comment }}{{!-- {{x}} --}}") == "v1");
  CHECK(render("a  {{~title~}}  b \\{{raw}}") =="aTb {{raw}}");
  CHECK(render("{{#response}}[{{name}}]{{/response}}{{^empty}}E{{/empty}}") == "[a&lt;b][c]E");
  CHECK(render("{{title}} = '\"`") == "T = '\"`");
  CHECK(render("{{#with title}}{{.}}{{/with}}") == "T");

  // Errors say where; unknown helpers render nothing with a warning.
  VisualizeResult e = render_handlebars("<p>\n{{#each response}}x", data);
  CHECK_FALSE(e.ok);
  CHECK(e.error.find("line 2") != std::string::npos);
  e = render_handlebars("{{#if a}}x{{/each}}", data);
  CHECK_FALSE(e.ok);
  e = render_handlebars("{{title", data);
  CHECK_FALSE(e.ok);
  e = render_handlebars("{{x}}", "{not json");
  CHECK_FALSE(e.ok);
  e = render_handlebars("{{formatDate title}}|{{> part}}", data);
  REQUIRE(e.ok);
  CHECK(e.html == "|");
  CHECK(e.warnings.size() == 2);

  // The output stops at the cap.
  std::string big = "{\"a\":[";
  for (int i = 0; i < 5000; ++i) big += (i ? ",\"" : "\"") + std::string(100, 'x') + "\"";
  big += "]}";
  e = render_handlebars("{{#each a}}{{this}}{{/each}}", big, 64 << 10);
  REQUIRE(e.ok);
  CHECK(e.truncated);
  CHECK(e.html.size() < (64 << 10) + 200);

  // The page allows inline styles and data: images only.
  const std::string page = visualize_page("<b>x</b>");
  CHECK(page.find("default-src 'none'") != std::string::npos);
  CHECK(page.find("script-src") == std::string::npos);
  CHECK(page.find("<b>x</b>") != std::string::npos);
}

TEST_CASE("pm.visualizer.set keeps the template and data, and later tests still run", "[response_visualize]") {
  using namespace watt;
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path cfg = fs::temp_directory_path() / "watt_rv_vis_cfg";
  const fs::path root = fs::temp_directory_path() / "watt_rv_vis_ws";
  fs::remove_all(cfg, ec);
  fs::remove_all(root, ec);
  Workspace::set_user_config_dir(cfg);
  Workspace ws = Workspace::create(root);
  REQUIRE(ws.trust_scripts(true));
  SavedRequest q;
  q.id = "vis-1";
  q.name = "Items";
  q.http.method = "GET";
  q.http.url = "http://example.invalid/items";
  q.post_script =
      "var t='<table>{{#each response}}<tr><td>{{name}}</td></tr>{{/each}}</table>';\n"
      "pm.visualizer.set(t, {response: pm.response.json()});\n"
      "pm.test('ok', () => pm.expect(pm.response.code).to.equal(200));\n";
  HttpResponse resp;
  resp.ok = true;
  resp.status = 200;
  resp.headers.add("Content-Type", "application/json");
  resp.body = R"([{"name":"one"},{"name":"two"}])";
  ResponseData rd(resp);
  ScriptRun run = run_post_script(ws, q, rd);
  INFO(run.error);
  CHECK(run.ok);
  REQUIRE(run.tests.size() == 1);
  CHECK(run.tests[0].pass);
  REQUIRE(run.visualizer);
  CHECK(run.visualizer_data == R"({"response":[{"name":"one"},{"name":"two"}]})");
  const VisualizeResult r = render_handlebars(run.visualizer_template, run.visualizer_data);
  CHECK(r.html == "<table><tr><td>one</td></tr><tr><td>two</td></tr></table>");

  // clear() drops it; a circular data object is an error kept on the run, not an exception.
  q.post_script = "pm.visualizer.set('x'); pm.visualizer.clear(); pm.test('after', () => {});";
  ScriptRun c = run_post_script(ws, q, rd);
  CHECK(c.ok);
  CHECK_FALSE(c.visualizer);
  CHECK(c.tests.size() == 1);
  q.post_script = "var o = {}; o.o = o; pm.visualizer.set('{{a}}', o); pm.test('after', () => {});";
  ScriptRun cyc = run_post_script(ws, q, rd);
  CHECK(cyc.ok);
  CHECK(cyc.visualizer);
  CHECK(cyc.visualizer_error.find("JSON") != std::string::npos);
  CHECK(cyc.tests.size() == 1);

  // A restored run carries the template and the data.
  RunExtras x;
  x.post = std::make_shared<ScriptRun>(run);
  CHECK_FALSE(extras_from_json(extras_to_json(x))->visualize);
  x.visualize = true;  // the run was shown as Visualize: reopened that way
  const auto back = extras_from_json(extras_to_json(x));
  REQUIRE(back);
  CHECK(back->visualize);
  REQUIRE(back->post);
  CHECK(back->post->visualizer);
  CHECK(back->post->visualizer_template == run.visualizer_template);
  CHECK(back->post->visualizer_data == run.visualizer_data);
  fs::remove_all(root, ec);
  fs::remove_all(cfg, ec);
}

#include "app/response_sse.cpp"

TEST_CASE("event streams are parsed per the WHATWG rules", "[response_sse]") {
  using namespace watt::app::rv;
  const auto ev = parse_sse("\xEF\xBB\xBF: hello\r\nid: 7\r\nevent: tick\r\ndata: {\"n\":1}\r\ndata: two\r\n\r\n"
                            "data: plain\n\nretry: 1500\ndata:x\n\nevent: no-data\n\n");
  REQUIRE(ev.size() == 3);
  CHECK(ev[0].id == "7");
  CHECK(ev[0].type == "tick");
  CHECK(ev[0].data == "{\"n\":1}\ntwo");
  CHECK(ev[0].at_ms < 0);
  CHECK(ev[1].type.empty());
  CHECK(ev[1].data == "plain");
  CHECK(ev[2].retry == 1500);
  CHECK(ev[2].data == "x");
  // An event without a blank line after it is not dispatched (the stream may still be running).
  CHECK(parse_sse("data: a\n\ndata: b\n").size() == 1);

  // CRLF split across chunks, a BOM split across chunks, offsets from feed().
  SseParser p;
  std::vector<SseEvent> out;
  p.feed("\xEF\xBB", 2, 1.0, out);
  p.feed("\xBF" "data: a\r", 9, 2.0, out);
  p.feed("\n\r", 2, 3.0, out);
  p.feed("\n: c\n", 5, 4.0, out);
  REQUIRE(out.size() == 1);
  CHECK(out[0].data == "a");
  CHECK(out[0].at_ms == 3.0);
  CHECK(p.comments() == 1);

  CHECK(sniff_sse(": keepalive\n") == 1);
  CHECK(sniff_sse("\n\nevent: x\n") == 1);
  CHECK(sniff_sse("da") == -1);
  CHECK(sniff_sse("{\"a\":1}") == 0);
  CHECK(sniff_sse("<html>") == 0);
  CHECK(is_event_stream("text/event-stream"));
  CHECK(is_event_stream(" Text/Event-Stream; charset=utf-8"));
  CHECK_FALSE(is_event_stream("text/event-streams"));
  CHECK_FALSE(is_event_stream("application/json"));
  CHECK(one_line_preview("{\n  \"a\": 1\n}") == "{ \"a\": 1 }");
  CHECK(one_line_preview(std::string(300, 'x'), 10).size() == 13);  // 10 + the ellipsis
}

TEST_CASE("a stream without line ends or blank lines cannot take all memory", "[response_sse]") {
  using namespace watt::app::rv;
  SseParser p;
  std::vector<SseEvent> out;
  const std::string chunk(1u << 20, 'x');
  p.feed("data: ", 6, 1.0, out);
  for (int i = 0; i < 24; ++i) p.feed(chunk.data(), chunk.size(), 2.0, out);  // 24 MB, one line
  CHECK(p.pending_bytes() <= SseParser::kMaxField + 64);
  p.feed("\n\n", 2, 3.0, out);
  REQUIRE(out.size() == 1);
  CHECK(out[0].truncated);
  CHECK(out[0].data.size() <= SseParser::kMaxField);
  // Many data lines and no blank line: the joined data stops growing too.
  out.clear();
  const std::string line = "data: " + std::string((1u << 20) - 7, 'y') + "\n";
  for (int i = 0; i < 24; ++i) p.feed(line.data(), line.size(), 4.0, out);
  p.feed("\n", 1, 5.0, out);
  REQUIRE(out.size() == 1);
  CHECK(out[0].truncated);
  CHECK(out[0].data.size() <= SseParser::kMaxField);
  // A normal event after it is whole again.
  out.clear();
  p.feed("data: ok\n\n", 10, 6.0, out);
  REQUIRE(out.size() == 1);
  CHECK_FALSE(out[0].truncated);
  CHECK(out[0].data == "ok");
}

TEST_CASE("the body tap batches events and keeps the start of a cut download", "[response_sse]") {
  using namespace watt::app::rv;
  int posts = 0;
  SseTap t(SteadyClock::now(), [&] { ++posts; });
  CHECK(t.first_ms() < 0);
  t.feed("da", 2);  // undecided: held
  CHECK_FALSE(t.is_sse());
  CHECK_FALSE(t.is_off());
  t.feed("ta: 1\n\nid: 9\ndata: 2\n\n", 22);
  CHECK(t.is_sse());
  CHECK(t.first_ms() >= 0);
  t.feed(": ping\n\ndata: 3\n\n", 17);
  CHECK(posts == 1);  // one post until the UI drains
  SseTap::Batch b = t.drain();
  REQUIRE(b.events.size() == 3);
  CHECK(b.events[0].data == "1");
  CHECK(b.events[1].id == "9");
  CHECK(b.events[0].at_ms >= 0);
  CHECK(b.raw == "data: 1\n\nid: 9\ndata: 2\n\n: ping\n\ndata: 3\n\n");
  CHECK(b.comments == 1);
  t.feed("data: 4\n\n", 9);
  CHECK(posts == 2);
  CHECK(t.drain().events.size() == 1);
  CHECK(t.bytes() == 2 + 22 + 17 + 9);
  CHECK(t.partial().empty());

  // Not an event stream: off for good, its first bytes kept (a cancel shows them).
  int posts2 = 0;
  SseTap j(SteadyClock::now(), [&] { ++posts2; });
  j.feed("{\"a\"", 4);
  j.feed(":1}", 3);
  CHECK(j.is_off());
  CHECK(j.partial() == "{\"a\":1}");
  CHECK(j.bytes() == 7);
  const std::string big(SseTap::kPartialMax, 'x');
  j.feed(big.data(), big.size());
  CHECK(j.partial().size() == SseTap::kPartialMax);
  CHECK(j.bytes() == 7 + big.size());
  CHECK(posts2 == 0);
}

TEST_CASE("the response head reaches the view before the body", "[response_sse]") {
  using namespace watt::app::rv;
  int heads = 0;
  SseTap t(SteadyClock::now(), [] {}, [&] { ++heads; });
  CHECK(t.head_response() == nullptr);
  CHECK(t.head_ms() < 0);
  auto h = std::make_shared<watt::HttpResponse>();
  h->ok = true;
  h->status = 200;
  h->headers.add("Content-Type", "text/event-stream; charset=utf-8");
  h->headers.add("Cache-Control", "no-cache");
  t.head(h);
  CHECK(heads == 1);
  REQUIRE(t.head_response() != nullptr);
  CHECK(t.head_response()->status == 200);
  CHECK(t.head_response()->headers.size() == 2);
  CHECK(t.head_ms() >= 0);
  // Declared an event stream: decided before any body byte (no sniffing).
  CHECK(t.is_sse());
  t.feed("data: x\n\n", 9);
  CHECK(t.drain().events.size() == 1);

  // A JSON head leaves the tap to sniff (and turn itself off) as before.
  SseTap j(SteadyClock::now(), [] {});
  auto jh = std::make_shared<watt::HttpResponse>();
  jh->headers.add("Content-Type", "application/json");
  j.head(jh);
  CHECK(!j.is_sse());
  CHECK(!j.is_off());
  j.feed("{}", 2);
  CHECK(j.is_off());
  j.head(nullptr);  // ignored
  CHECK(j.head_response() == jh);
}

TEST_CASE("a stream's event offsets are kept with the run", "[response_sse]") {
  using namespace watt::app::rv;
  RunExtras x;
  x.sse_at = {2.15, 503.04, 1004.5};
  const auto back = extras_from_json(extras_to_json(x));
  REQUIRE(back);
  REQUIRE(back->sse_at.size() == 3);
  CHECK(back->sse_at[0] == 2.15);
  CHECK(back->sse_at[2] == 1004.5);
  RunExtras none;
  CHECK(extras_to_json(none).find("sse_at") == std::string::npos);
  CHECK(extras_from_json(extras_to_json(none))->sse_at.empty());
}

TEST_CASE("the Visualize page follows the app theme", "[response_visualize]") {
  using namespace watt::app::rv;
  const std::string dark = visualize_page("<table><tr><th>a</th></tr></table>", true);
  CHECK(dark.find("color-scheme:dark") != std::string::npos);
  CHECK(dark.find("background:#1e1e1e") != std::string::npos);
  CHECK(dark.find("script-src") == std::string::npos);
  // The base comes before the template, so the template's own styles win.
  CHECK(dark.find("<style>") < dark.find("<table>"));
  CHECK(visualize_page("x").find("color-scheme:light") != std::string::npos);
}
