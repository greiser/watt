// Pure helpers of the HTTP request editor (src/app/http_editor_util.*): query encoding between
// the Params table and the URL, URL normalisation at send time, bulk edit text, header cleaning.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

// The helpers have no wx dependency; they are compiled into this test directly.
#include "app/http_editor_util.cpp"
#include "app/http_editor_codegen.cpp"
#include "app/http_editor_jshelp.cpp"
#include "core/model.h"

#include <filesystem>
#include <fstream>

using namespace watt::app::http_editor;

namespace {

std::vector<KvItem> table_from(const std::string& url) { return parse_query_decoded(split_url(url).query); }

}  // namespace

TEST_CASE("table value round-trips through the URL", "[http_editor]") {
  const std::string v = "a b&c=d#x+y%";
  const std::string url = with_decoded_query("http://127.0.0.1:18080/echo", {{"tag", v, true, false}});
  CHECK(url == "http://127.0.0.1:18080/echo?tag=a%20b%26c%3Dd%23x%2By%25");
  const auto rows = table_from(url);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].key == "tag");
  CHECK(rows[0].value == v);
  // and again (table -> URL with the URL already holding it): unchanged
  CHECK(with_decoded_query(url, rows) == url);
}

TEST_CASE("keys are encoded, vars stay literal", "[http_editor]") {
  const std::string url = with_decoded_query("{{base}}/x", {{"a key", "{{v}} 1", true, false}});
  CHECK(url == "{{base}}/x?a%20key={{v}}%201");
  const auto rows = table_from(url);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].key == "a key");
  CHECK(rows[0].value == "{{v}} 1");
}

TEST_CASE("untouched items keep their spelling", "[http_editor]") {
  const std::string url = "http://h/p?q=a+b&t=%7e&flag#frag";
  auto rows = table_from(url);
  REQUIRE(rows.size() == 3);
  CHECK(rows[0].value == "a+b");
  CHECK(rows[1].value == "~");
  CHECK(rows[2].bare);
  CHECK(with_decoded_query(url, rows) == url);
  rows.push_back({"n", "x y", true, false});
  CHECK(with_decoded_query(url, rows) == "http://h/p?q=a+b&t=%7e&flag&n=x%20y#frag");
}

TEST_CASE("undecodable items are shown as written", "[http_editor]") {
  const auto rows = table_from("http://h/?a=%FF&b=%0A&c=%zz");
  REQUIRE(rows.size() == 3);
  CHECK(rows[0].value == "%FF");
  CHECK(rows[1].value == "%0A");
  CHECK(rows[2].value == "%zz");
}

TEST_CASE("normalize_url encodes what libcurl rejects", "[http_editor]") {
  CHECK(normalize_url("http://127.0.0.1:18080/echo?q=hello world") == "http://127.0.0.1:18080/echo?q=hello%20world");
  CHECK(normalize_url("http://h/a b/c?x=\"1\"&y={{v w}}#f g") == "http://h/a%20b/c?x=%221%22&y={{v w}}#f%20g");
  CHECK(normalize_url("http://h/?p=100%&q=%20") == "http://h/?p=100%25&q=%20");
  CHECK(normalize_url("http://h/caf\xC3\xA9") == "http://h/caf%C3%A9");
  CHECK(normalize_url("http://[::1]:8080/x") == "http://[::1]:8080/x");
  CHECK(normalize_url("{{base}}/a b") == "{{base}}/a%20b");
  CHECK(normalize_url("localhost:8080/a b") == "localhost:8080/a%20b");
  CHECK(normalize_url("http://h") == "http://h");
}

TEST_CASE("bulk edit text", "[http_editor]") {
  const std::vector<KvItem> items = {{"Accept", "*/*", true, false}, {"X-Off", "1", false, false}, {"Empty", "", true, false}};
  const std::string text = to_bulk(items);
  CHECK(text == "Accept: */*\n//X-Off: 1\nEmpty:\n");
  const auto back = parse_bulk(text);
  REQUIRE(back.size() == 3);
  CHECK(back[0].key == "Accept");
  CHECK(back[0].value == "*/*");
  CHECK(!back[1].enabled);
  CHECK(back[1].key == "X-Off");
  CHECK(back[2].value.empty());

  const auto pasted = parse_bulk(":authority: example.com\r\nContent-Type: application/json\r\n\r\n  time: 10:30 \r\nq=1\n");
  REQUIRE(pasted.size() == 3);
  CHECK(pasted[0].key == "Content-Type");
  CHECK(pasted[1].key == "time");
  CHECK(pasted[1].value == "10:30");
  CHECK(pasted[2].key == "q");
  CHECK(pasted[2].value == "1");
}

TEST_CASE("check_json finds the first syntax error", "[http_editor]") {
  CHECK(!check_json(""));
  CHECK(!check_json("  "));
  CHECK(!check_json(R"({"a": [1, -2.5e3, true, null, "x\"y"], "b": {}})"));
  CHECK(!check_json("{\"user\": {{user}}, {{key}}: 1} // note"));
  CHECK(!check_json("/* c */ [1, 2]"));
  auto e = check_json("{a: 1}");
  REQUIRE(e);
  CHECK(e->offset == 1);
  CHECK(e->message.find("double quotes") != std::string::npos);
  e = check_json("{\"a\": 1 \"b\": 2}");
  REQUIRE(e);
  CHECK(e->offset == 8);
  e = check_json("[1, 2,]");
  REQUIRE(e);
  CHECK(e->message.find("trailing comma") != std::string::npos);
  e = check_json("{\"a\": [1, 2}");
  REQUIRE(e);
  CHECK(e->offset == 11);
  e = check_json("{\"a\": 1");
  REQUIRE(e);
  CHECK(e->message.find("missing '}'") != std::string::npos);
  e = check_json("{\"a\": 01x}");
  REQUIRE(e);
  CHECK(e->message == "invalid number");
  e = check_json("{\"a\": \"open\n}");
  REQUIRE(e);
  CHECK(e->message == "unterminated string");
  e = check_json("{} {}");
  REQUIRE(e);
  CHECK(e->offset == 3);
}

TEST_CASE("header names and values are cleaned", "[http_editor]") {
  CHECK(clean_header_name("Zfoo\tbar\r\n ") == "Zfoobar");
  CHECK(clean_header_value("a\r\nb\tc") == "ab\tc");
}

TEST_CASE("typing {{base}}/echo?id=1 one key at a time leaves no extra }}", "[http_editor]") {
  TypingModel m;
  m.type("{{base}}/echo?id=1");
  CHECK(m.text == "{{base}}/echo?id=1");
  CHECK(m.caret == m.text.size());
  // only the auto-inserted closer is stepped over: a second "}}" typed after it is kept
  TypingModel m2;
  m2.type("{{");
  CHECK(m2.text == "{{}}");
  CHECK(m2.caret == 2);
  m2.type("a}}}}");
  CHECK(m2.text == "{{a}}}}");
  // "{{{" stays a literal brace, nothing is added
  TypingModel m3;
  m3.type("{{{");
  CHECK(m3.text == "{{{}}");
}

TEST_CASE("var_closer completes {{ once", "[http_editor]") {
  CHECK(var_closer("x{{", "") == "}}");
  CHECK(var_closer("x{{", "}") == "}");
  CHECK(var_closer("x{{", "}}") == "");
  CHECK(var_closer("{{{", "") == "");
  CHECK(var_closer("x{", "") == "");
  CHECK(steps_over('}', '}', true));
  CHECK_FALSE(steps_over('}', '}', false));
  CHECK_FALSE(steps_over('a', 'a', true));
}

TEST_CASE("looks_like_xml", "[http_editor]") {
  CHECK(looks_like_xml("  <?xml version=\"1.0\"?><a/>"));
  CHECK(looks_like_xml("<soap:Envelope/>"));
  CHECK_FALSE(looks_like_xml("{\"a\":1}"));
  CHECK_FALSE(looks_like_xml(""));
}

TEST_CASE("copy as code leaves disabled headers out and quotes values", "[http_editor][codegen]") {
  watt::SavedRequest r;
  r.http.method = "POST";
  r.http.url = "http://127.0.0.1:18080/echo?a=1";
  r.http.headers = {{"Content-Type", "application/json"}, {"X-Off", "1"}, {"X-Quote", "it's \"q\""}};
  r.disabled_headers = {1};
  r.http.body = "{\"a\": \"line\nnext\"}";
  const std::string py = to_code(r, CodeLang::PythonRequests);
  CHECK(py.find("X-Off") == std::string::npos);
  CHECK(py.find("\"X-Quote\": \"it's \\\"q\\\"\"") != std::string::npos);
  CHECK(py.find("requests.request(\"POST\", url, headers=headers, data=data") != std::string::npos);
  const std::string js = to_code(r, CodeLang::JavaScriptFetch);
  CHECK(js.find("method: \"POST\"") != std::string::npos);
  CHECK(js.find("X-Off") == std::string::npos);
  const std::string hp = to_code(r, CodeLang::HTTPie);
  CHECK(hp.find("'X-Quote:it'\\''s \"q\"'") != std::string::npos);
  CHECK(hp.find("--raw '") != std::string::npos);
  r.basic_auth = {true, "alice", "s3cret"};
  CHECK(to_code(r, CodeLang::PythonRequests).find("auth=(\"alice\", \"s3cret\")") != std::string::npos);
  CHECK(to_code(r, CodeLang::HTTPie).find("-a 'alice:s3cret'") != std::string::npos);
}

TEST_CASE("disabled query params survive a save", "[http_editor][model]") {
  const auto dir = std::filesystem::temp_directory_path() / "watt_test_disabled_params";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  watt::Workspace ws = watt::Workspace::create(dir);
  watt::SavedRequest r;
  r.name = "p";
  r.http.url = "http://x/echo?a=1&d={{userId}}";
  r.disabled_params = {{"b", "2"}, {"c", "three & more"}};
  r.file = dir / "p.req.json";
  std::string err;
  REQUIRE(ws.save_request(r, &err));
  const auto back = ws.load_request(r.file);
  REQUIRE(back);
  CHECK(back->http.url == r.http.url);
  CHECK(back->disabled_params == r.disabled_params);
  std::filesystem::remove_all(dir);
}

TEST_CASE("path variables", "[http_editor][path]") {
  CHECK(path_var_names("{{base}}/echo/users/:id/orders?limit=5") == std::vector<std::string>{"id"});
  CHECK(path_var_names("http://localhost:8080/a/:x/b/:y/:x#f/:z") == std::vector<std::string>{"x", "y"});
  CHECK(path_var_names("http://h/a?q=/:no").empty());
  CHECK(path_var_names("http://h/{{a/:b}}/c").empty());
  CHECK(path_var_names("http://h/a:b/:/c").empty());
  auto val = [](const std::string& n) -> std::string {
    if (n == "id") return "42 a/b";
    if (n == "v") return "{{userId}}";
    return "";
  };
  CHECK(with_path_values("{{base}}/users/:id/orders?x=:id", val) == "{{base}}/users/42%20a%2Fb/orders?x=:id");
  CHECK(with_path_values("http://h/:v/:empty", val) == "http://h/{{userId}}/:empty");
  CHECK(path_encode("a+b@c:d;e=f") == "a+b@c:d;e=f");
}

TEST_CASE("path variable values survive a save", "[http_editor][model]") {
  const auto dir = std::filesystem::temp_directory_path() / "watt_test_path_params";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  watt::Workspace ws = watt::Workspace::create(dir);
  watt::SavedRequest r;
  r.name = "p";
  r.http.url = "http://x/users/:id/orders/:order";
  r.path_params = {{"id", "{{userId}}"}, {"order", ""}};
  r.file = dir / "p.req.json";
  std::string err;
  REQUIRE(ws.save_request(r, &err));
  const auto back = ws.load_request(r.file);
  REQUIRE(back);
  CHECK(back->http.url == r.http.url);
  CHECK(back->path_params == r.path_params);
  std::filesystem::remove_all(dir);
}

TEST_CASE("copy as code: more languages", "[http_editor][codegen]") {
  watt::SavedRequest r;
  r.http.method = "POST";
  r.http.url = "http://127.0.0.1:18080/echo?a=1";
  r.http.headers = {{"Content-Type", "application/json"}, {"X-Q", "it's \"q\""}};
  r.http.body = "{\"name\": \"x\"}";
  r.basic_auth = {true, "alice", "s3cret"};
  const std::string ax = to_code(r, CodeLang::NodeAxios);
  CHECK(ax.find("data: \"{\\\"name\\\": \\\"x\\\"}\"") != std::string::npos);
  CHECK(ax.find("username: \"alice\"") != std::string::npos);
  const std::string g = to_code(r, CodeLang::Go);
  CHECK(g.find("\"strings\"") != std::string::npos);
  CHECK(g.find("\"time\"") != std::string::npos);
  CHECK(g.find("req.SetBasicAuth(\"alice\", \"s3cret\")") != std::string::npos);
  const std::string j = to_code(r, CodeLang::Java);
  CHECK(j.find(".method(\"POST\", HttpRequest.BodyPublishers.ofString(") != std::string::npos);
  const std::string cs = to_code(r, CodeLang::CSharp);
  CHECK(cs.find("request.Content.Headers.TryAddWithoutValidation(\"Content-Type\", \"application/json\")") != std::string::npos);
  const std::string p = to_code(r, CodeLang::PowerShell);
  CHECK(p.find("-ContentType 'application/json'") != std::string::npos);
  CHECK(p.find("'it''s \"q\"'") != std::string::npos);
  CHECK(p.find("-Method 'POST'") != std::string::npos);
  for (int i = 0; i < kCodeLangCount; ++i) CHECK(std::string(code_lang_name(static_cast<CodeLang>(i))).size() > 2);
}

TEST_CASE("row descriptions round-trip through the docs", "[http_editor][docs]") {
  RowDocs d;
  d.params = {{"limit", "page size, max 100"}, {":id", "user id"}};
  d.headers = {{"X-Trace", "tab\there, line\nbreak, --> and \\ back"}};
  const std::string docs = "# Orders\n\nLists orders.\n";
  const std::string full = join_row_docs(docs, d);
  CHECK(full.find("# Orders\n\nLists orders.\n\n<!-- watt:descriptions") == 0);
  // nothing inside can close the comment early
  CHECK(full.find("-->") == full.size() - 4);
  RowDocs back;
  CHECK(split_row_docs(full, back) == "# Orders\n\nLists orders.");
  CHECK(back == d);
  // no descriptions: docs untouched, and plain docs split to themselves
  CHECK(join_row_docs(docs, {}) == docs);
  CHECK(split_row_docs(docs, back) == docs);
  CHECK(back.empty());
  // only descriptions
  RowDocs only;
  only.headers = {{"Accept", "json please"}};
  const std::string o = join_row_docs("", only);
  CHECK(o.rfind("<!-- watt:descriptions", 0) == 0);
  CHECK(split_row_docs(o, back).empty());
  CHECK(back == only);
  // a comment the user wrote is left alone
  const std::string user = "notes\n<!-- todo -->\n";
  CHECK(split_row_docs(user, back) == user);
  CHECK(back.empty());
}

TEST_CASE("cookie header parsing", "[http_editor]") {
  const auto c = parse_cookie_header(" session=abc ; theme=dark;flag; ;x= {{v}} ");
  REQUIRE(c.size() == 4);
  CHECK(c[0] == std::pair<std::string, std::string>{"session", "abc"});
  CHECK(c[1] == std::pair<std::string, std::string>{"theme", "dark"});
  CHECK(c[2] == std::pair<std::string, std::string>{"flag", ""});
  CHECK(c[3] == std::pair<std::string, std::string>{"x", "{{v}}"});
  CHECK(parse_cookie_header("").empty());
}

// ------------------------------------------------------------------ pre-request variables

#include "app/http_editor_pre.cpp"
#include "app/http_editor_inherit.cpp"

namespace {

std::string pre_eval(const std::string& expr, watt::Workspace& ws, const watt::SavedRequest& r = {},
                     std::string* err = nullptr) {
  std::vector<PreVar> rows{{true, "out", expr, watt::Capture::Runtime}};
  const auto res = evaluate_pre_vars(rows, r, ws);
  if (res.empty()) return "<none>";
  if (err) *err = res[0].message;
  return res[0].ok ? res[0].value : "<error>";
}

}  // namespace

TEST_CASE("pre-request: hashes and HMACs match the RFC vectors", "[http_editor][pre]") {
  // RFC 4231 test case 2
  CHECK(hmac_sha256_hex("Jefe", "what do ya want for nothing?") ==
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(hmac_sha256_hex("", "") == "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad");
  const auto dir = std::filesystem::temp_directory_path() / "watt_test_pre";
  std::filesystem::remove_all(dir);
  watt::Workspace ws = watt::Workspace::create(dir);
  CHECK(pre_eval("md5('')", ws) == "d41d8cd98f00b204e9800998ecf8427e");
  CHECK(pre_eval("sha1('abc')", ws) == "a9993e364706816aba3e25717850c26c9cd0d89d");
  CHECK(pre_eval("sha512('abc')", ws).rfind("ddaf35a193617aba", 0) == 0);
  CHECK(pre_eval("hmacSha256('Jefe', 'what do ya want for nothing?', 'base64')", ws) ==
        "W9zBRr9gdU5qBCQmCJV1x1oAPwidJzmDnexYuWTsOEM=");
  CHECK(pre_eval("base64('any carnal pleas')", ws) == "YW55IGNhcm5hbCBwbGVhcw==");
  CHECK(pre_eval("base64('any carnal pleasu')", ws) == "YW55IGNhcm5hbCBwbGVhc3U=");
  CHECK(pre_eval("base64url('any carnal pleas')", ws) == "YW55IGNhcm5hbCBwbGVhcw");
  CHECK(pre_eval("base64Decode('YW55IGNhcm5hbCBwbGVhcw==')", ws) == "any carnal pleas");
  CHECK(pre_eval("urlencode('a b&c=d/\xC3\xBC')", ws) == "a%20b%26c%3Dd%2F%C3%BC");
  CHECK(pre_eval("urldecode('a%20b+c%zz')", ws) == "a b c%zz");
  CHECK(pre_eval("hex('AB')", ws) == "4142");
  CHECK(pre_eval("'a\\tb\\'c'", ws) == "a\tb'c");
}

TEST_CASE("pre-request: values, vars, req fields and functions", "[http_editor][pre]") {
  const auto dir = std::filesystem::temp_directory_path() / "watt_test_pre2";
  std::filesystem::remove_all(dir);
  watt::Workspace ws = watt::Workspace::create(dir);
  ws.globals["secret"] = "k3y";
  ws.globals["login"] = R"({"data":{"token":"abc","list":[1,2,3]}})";
  ws.globals["base"] = "http://h";
  watt::SavedRequest r;
  r.http.method = "POST";
  r.http.url = "{{base}}/orders";
  r.http.body = R"({"id":{{secret}}})";
  r.http.headers = {{"X-Api-Key", "{{secret}}"}, {"X-Off", "1"}};
  r.disabled_headers = {1};
  CHECK(pre_eval("req.method + ' ' + req.url", ws, r) == "POST http://h/orders");
  CHECK(pre_eval("req.body", ws, r) == R"({"id":k3y})");
  CHECK(pre_eval("req.headers.x-api-key", ws, r) == "k3y");
  CHECK(pre_eval("req.headers.X-Off", ws, r).empty());  // a disabled header is not sent
  CHECK(pre_eval("hmacSha256({{secret}}, req.body)", ws, r) == hmac_sha256_hex("k3y", R"({"id":k3y})"));
  CHECK(pre_eval("hmacSha256(secret, 'x')", ws, r) == hmac_sha256_hex("k3y", "x"));
  CHECK(pre_eval("'Bearer {{secret}}'", ws) == "Bearer k3y");
  CHECK(pre_eval("jsonPath({{login}}, '$.data.token')", ws) == "abc");
  CHECK(pre_eval("jsonPath(login, 'data.list[-1]')", ws) == "3");
  CHECK(pre_eval("jsonPath(login, 'data.list.length')", ws) == "3");
  CHECK(pre_eval("upper('ab') + lower('CD') + trim('  x ') + length('h\xC3\xA9')", ws) == "ABcdx2");
  CHECK(pre_eval("replace('a-b-c', '-', '+')", ws) == "a+b+c");
  CHECK(pre_eval("substr('abcdef', 2, 3)", ws) == "cde");
  CHECK(pre_eval("concat('a', 1, (('b')))", ws) == "a1b");
  CHECK(pre_eval("uuid()", ws).size() == 36);
  CHECK(pre_eval("uuid()", ws)[14] == '4');
  const long long now = std::stoll(pre_eval("unix()", ws));
  CHECK(std::stoll(pre_eval("unix(60)", ws)) - now >= 59);
  CHECK(pre_eval("now()", ws).size() == 13);
  CHECK(pre_eval("iso()", ws).size() == 24);
  CHECK(pre_eval("iso()", ws).back() == 'Z');
  for (int i = 0; i < 50; ++i) {
    const int v = std::stoi(pre_eval("randomInt(5, 7)", ws));
    CHECK((v >= 5 && v <= 7));
  }
  CHECK(pre_eval("randomHex(8)", ws).size() == 16);
  CHECK(pre_eval("{{$timestamp}}", ws).size() >= 10);
  // rows see the ones before them
  std::vector<PreVar> rows{{true, "ts", "unix()", watt::Capture::Runtime},
                           {true, "sig", "hmacSha256(secret, req.method + {{ts}})", watt::Capture::Runtime},
                           {false, "off", "'x'", watt::Capture::Runtime},
                           {true, "", "'no name'", watt::Capture::Runtime}};
  const auto res = evaluate_pre_vars(rows, r, ws);
  REQUIRE(res.size() == 2);
  CHECK(res[1].ok);
  CHECK(res[1].value == hmac_sha256_hex("k3y", "POST" + res[0].value));
  CHECK(ws.runtime_vars.at("sig") == res[1].value);
  CHECK(!ws.runtime_vars.count("off"));
}

TEST_CASE("pre-request: errors and safety", "[http_editor][pre]") {
  CHECK(check_pre_expr("hmacSha256({{k}}, req.body)").empty());
  CHECK(check_pre_expr("HMACSHA256('a','b')").empty());  // names are case-insensitive
  CHECK(!check_pre_expr("").empty());
  CHECK(check_pre_expr("nope(1)").find("unknown function") != std::string::npos);
  CHECK(check_pre_expr("sha256()").find("takes 1-2 arguments") != std::string::npos);
  CHECK(check_pre_expr("'open").find("missing") != std::string::npos);
  CHECK(check_pre_expr("sha256('a'").find("')'") != std::string::npos);
  CHECK(check_pre_expr("'a' 'b'").find("join with +") != std::string::npos);
  CHECK(check_pre_expr("req.cookies").find("unknown") != std::string::npos);
  CHECK(!check_pre_expr(std::string(kMaxPreExprBytes + 1, 'a')).empty());
  std::string deep;
  for (int i = 0; i < 200; ++i) deep += "lower(";
  deep += "'x'";
  for (int i = 0; i < 200; ++i) deep += ")";
  CHECK(check_pre_expr(deep).find("nested too deeply") != std::string::npos);

  const auto dir = std::filesystem::temp_directory_path() / "watt_test_pre3";
  std::filesystem::remove_all(dir);
  watt::Workspace ws = watt::Workspace::create(dir);
  std::string err;
  CHECK(pre_eval("undefinedName", ws, {}, &err) == "<error>");
  CHECK(err.find("not defined") != std::string::npos);
  CHECK(pre_eval("jsonPath('not json', 'a')", ws, {}, &err) == "<error>");
  CHECK(pre_eval("'{{unresolved}}'", ws, {}, &err) == "<error>");  // would be expanded again
  CHECK(err.find("{{") != std::string::npos);
  CHECK(pre_eval("base64Decode('***')", ws, {}, &err) == "<error>");
  CHECK(pre_eval("hmacSha256('k', 'x', 'rot13')", ws, {}, &err) == "<error>");
  // a secret of the active environment is never written
  watt::Environment env;
  env.name = "dev";
  env.vars["tok"] = "s3cret";
  env.secrets.insert("tok");
  ws.environments().push_back(env);
  ws.active_env = "dev";
  std::vector<PreVar> rows{{true, "tok", "'overwritten'", watt::Capture::Runtime}};
  const auto res = evaluate_pre_vars(rows, {}, ws);
  REQUIRE(res.size() == 1);
  CHECK(!res[0].ok);
  CHECK(!ws.runtime_vars.count("tok"));
  // an OS environment variable the workspace may not read is listed for the consent prompt, and
  // is never expanded
  _putenv_s("WATT_PRE_TEST_SECRET", "x");
  std::vector<PreVar> env_rows{{true, "v", "sha256({{process.env.WATT_PRE_TEST_SECRET}})", watt::Capture::Runtime}};
  const auto refs = pre_env_refs(env_rows, ws);
  REQUIRE(refs.size() == 1);
  CHECK(refs[0] == "WATT_PRE_TEST_SECRET");
  CHECK(pre_eval("{{process.env.WATT_PRE_TEST_SECRET}}", ws, {}, &err) == "<error>");
}

TEST_CASE("pre-request rows round-trip through the docs", "[http_editor][pre][docs]") {
  RowDocs d;
  d.pre = {{"sig", "hmacSha256({{k}}, req.body + '\t-->')", "environment", true}, {"ts", "unix()", "runtime", false}};
  d.headers = {{"X-A", "note"}};
  const std::string full = join_row_docs("Docs", d);
  CHECK(full.find("-->") == full.size() - 4);
  RowDocs back;
  CHECK(split_row_docs(full, back) == "Docs");
  CHECK(back == d);
}

TEST_CASE("curl import keeps a {{variable}} base URL without curl's default scheme", "[http_editor]") {
  // curl -X POST '{{base}}/echo?src=x': the parser adds http://, the variable already has one
  CHECK(strip_curl_default_scheme("http://{{base}}/echo?src=x", "curl -X POST '{{base}}/echo?src=x'") ==
        "{{base}}/echo?src=x");
  // written by the user: kept
  CHECK(strip_curl_default_scheme("http://{{host}}/x", "curl http://{{host}}/x") == "http://{{host}}/x");
  CHECK(strip_curl_default_scheme("http://{{host}}/x", "curl HTTP://{{host}}/x") == "http://{{host}}/x");
  // ordinary URLs: untouched
  CHECK(strip_curl_default_scheme("http://example.com/a", "curl example.com/a") == "http://example.com/a");
  CHECK(strip_curl_default_scheme("https://{{h}}/a", "curl https://{{h}}/a") == "https://{{h}}/a");
}

TEST_CASE("a doubled scheme in the URL as sent is detected", "[http_editor]") {
  CHECK(doubled_scheme("http://http://127.0.0.1:18080/echo"));
  CHECK(doubled_scheme("http://HTTPS://example.com"));
  CHECK(doubled_scheme("https://http://x"));
  CHECK_FALSE(doubled_scheme("http://127.0.0.1/http://x"));
  CHECK_FALSE(doubled_scheme("http://example.com/?u=http://x"));
  CHECK_FALSE(doubled_scheme("{{base}}/echo"));
  CHECK_FALSE(doubled_scheme(""));
}

TEST_CASE("GraphQL body: built from query and variables, and split back", "[http_editor]") {
  const std::string q = "query User($id: ID!) {\n  user(id: $id) { name \"x\" }\n}";
  const std::string body = graphql_body(q, "  {\"id\": 42}\n");
  CHECK(body == "{\"query\":\"query User($id: ID!) {\\n  user(id: $id) { name \\\"x\\\" }\\n}\",\"variables\":{\"id\": 42}}");
  std::string q2, v2, op;
  REQUIRE(split_graphql_body(body, q2, v2, &op));
  CHECK(q2 == q);
  CHECK(v2 == "{\n  \"id\": 42\n}");
  CHECK(op.empty());
  // no variables: none sent
  CHECK(graphql_body("{ ping }", "  ") == "{\"query\":\"{ ping }\"}");
  CHECK(graphql_body("{ a }", "", "Op") == "{\"query\":\"{ a }\",\"operationName\":\"Op\"}");
  // {{vars}} in the variables go in as written (JSON once expanded)
  CHECK(graphql_body("{ a }", "{\"id\": {{id}}}") == "{\"query\":\"{ a }\",\"variables\":{\"id\": {{id}}}}");
}

TEST_CASE("GraphQL body: only plain GraphQL JSON is recognised", "[http_editor]") {
  std::string q, v, op;
  CHECK_FALSE(split_graphql_body("{\"name\":\"Ada\"}", q, v, &op));
  CHECK_FALSE(split_graphql_body("{\"query\":1}", q, v, &op));
  CHECK_FALSE(split_graphql_body("{\"query\":\"{ a }\",\"extra\":true}", q, v, &op));
  CHECK_FALSE(split_graphql_body("{\"query\":\"{ a }\",\"variables\":[1]}", q, v, &op));
  CHECK_FALSE(split_graphql_body("not json {\"query\"", q, v, &op));
  REQUIRE(split_graphql_body(" {\"query\":\"{ a }\",\"variables\":null,\"operationName\":\"A\"} ", q, v, &op));
  CHECK(q == "{ a }");
  CHECK(v.empty());
  CHECK(op == "A");
  // an operation name the caller cannot keep: not split
  CHECK_FALSE(split_graphql_body("{\"query\":\"{ a }\",\"operationName\":\"A\"}", q, v));
}


TEST_CASE("typing a URL with :id one key at a time: the path rows follow each prefix", "[http_editor][path]") {
  // The editor rebuilds the Path table whenever these names change while the URL has the focus
  // (the first one creates it). Each prefix must give the names the table shows, and the
  // focus must stay in the URL then (checked in the app: sync_path_rows keeps the focus).
  const std::string url = "https://api.example.com/users/:id/posts?limit=5";
  std::vector<std::string> last;
  int changes = 0;
  for (size_t i = 1; i <= url.size(); ++i) {
    const auto names = path_var_names(url.substr(0, i));
    if (names != last) ++changes;
    last = names;
  }
  CHECK(last == std::vector<std::string>{"id"});
  CHECK(changes == 2);  // ":i" creates the row, ":id" renames it
  CHECK(path_var_names("{{base}}/echo/:u") == std::vector<std::string>{"u"});
  CHECK(path_var_names("{{base}}/echo/:userId/orders?limit=5") == std::vector<std::string>{"userId"});
}

TEST_CASE("script help: member completion", "[http_editor][script]") {
  CHECK(js_chain_before("const x = pm.response", 21) == "pm.response");
  CHECK(js_chain_before("foo(pm", 6) == "pm");
  CHECK(js_chain_before("a..b", 4).empty());
  CHECK(js_chain_before("1.5", 3).empty());
  const auto pm_post = js_members("pm", true);
  CHECK(std::find(pm_post.begin(), pm_post.end(), "response") != pm_post.end());
  const auto pm_pre = js_members("pm", false);
  CHECK(std::find(pm_pre.begin(), pm_pre.end(), "response") == pm_pre.end());
  CHECK(std::find(pm_pre.begin(), pm_pre.end(), "environment") != pm_pre.end());
  CHECK(js_members("res", false).empty());
  CHECK_FALSE(js_members("res", true).empty());
  CHECK(js_members("pm.response.to.have", true) == std::vector<std::string>{"header", "jsonBody", "status"});
  CHECK(js_members("nothing.here", true).empty());
  const auto top = js_members("", true);
  CHECK(std::is_sorted(top.begin(), top.end()));
}

TEST_CASE("script help: lint finds structural errors only", "[http_editor][script]") {
  CHECK_FALSE(js_lint("pm.test('ok', () => { pm.response.to.have.status(200); });"));
  CHECK_FALSE(js_lint("const re = /a[/]b\\/c/g; const d = a / b / c; x = (y) / 2; i++ / 2;"));
  CHECK_FALSE(js_lint("const s = `a ${b + `c ${d}`} e`; // } not counted\n/* ( */ const t = '{';"));
  CHECK_FALSE(js_lint("if (x) { return /re/.test(y); }"));
  CHECK_FALSE(js_lint(""));
  auto e = js_lint("pm.test('ok', () => {\n  pm.expect(1).to.eql(1);\n);");
  REQUIRE(e);
  CHECK(e->pos == 48);  // the ')' that came before the '}'
  CHECK(e->message.find("line 1") != std::string::npos);
  e = js_lint("function f() {\n  return 1;\n");
  REQUIRE(e);
  CHECK(e->pos == 13);
  CHECK(e->message.find("never closed") != std::string::npos);
  CHECK(js_lint("const s = 'abc;\nx();"));
  CHECK(js_lint("/* open"));
  CHECK(js_lint("x = `abc"));
  CHECK(js_lint("x());"));
  // every snippet is clean
  for (bool post : {false, true})
    for (const auto& sn : js_snippets(post)) CHECK_FALSE(js_lint(sn.code));
}

// ------------------------------------------------------------------ folder / collection inheritance

TEST_CASE("folder auth form: Bearer and API key travel as the folder's header", "[http_editor][inherit]") {
  watt::FolderSettings s;
  s.auth.type = watt::HttpAuth::Type::None;
  s.headers = {{"X-Trace", "t-1", "", true}, {"Authorization", "Bearer {{token}}", "", true}};
  FolderForm f = folder_form(s);
  CHECK(f.auth == FolderAuth::Bearer);
  CHECK(f.token == "{{token}}");
  REQUIRE(f.headers.size() == 1);
  CHECK(f.headers[0].name == "X-Trace");
  watt::FolderSettings back = s;
  apply_folder_form(f, back);
  CHECK(back.auth.type == watt::HttpAuth::Type::None);
  REQUIRE(back.headers.size() == 2);
  CHECK(back.headers[0].name == "Authorization");
  CHECK(back.headers[0].value == "Bearer {{token}}");
  CHECK(back.headers[1].name == "X-Trace");

  f.auth = FolderAuth::ApiKey;
  f.key_name = "X-Api-Key\r\n";
  f.key_value = "k\n1";
  apply_folder_form(f, back);
  REQUIRE(back.headers.size() == 2);
  CHECK(back.headers[0].name == "X-Api-Key");  // control characters never reach the wire
  CHECK(back.headers[0].value == "k1");
  CHECK(folder_form(back).auth == FolderAuth::ApiKey);

  f.auth = FolderAuth::Inherit;
  apply_folder_form(f, back);
  CHECK(back.auth.type == watt::HttpAuth::Type::Inherit);
  CHECK(back.headers.size() == 1);
  CHECK(folder_form(back).auth == FolderAuth::Inherit);
}

TEST_CASE("folder auth form: Basic and schemes", "[http_editor][inherit]") {
  watt::FolderSettings s;
  FolderForm f;
  f.auth = FolderAuth::Basic;
  f.user = "alice";
  f.pass = "{{pw}}";
  apply_folder_form(f, s);
  CHECK(s.auth.type == watt::HttpAuth::Type::None);
  CHECK(s.basic_auth.enabled);
  CHECK(folder_form(s).auth == FolderAuth::Basic);
  CHECK(folder_form(s).user == "alice");
  f = {};
  f.auth = FolderAuth::OAuth2;
  f.adv.oauth2.token_url = "https://a/token";
  apply_folder_form(f, s);
  CHECK(s.auth.type == watt::HttpAuth::Type::OAuth2);
  CHECK(!s.basic_auth.enabled);
  CHECK(folder_form(s).auth == FolderAuth::OAuth2);
  CHECK(folder_form(s).adv.oauth2.token_url == "https://a/token");
}

TEST_CASE("inherit view: source, type and a masked token", "[http_editor][inherit]") {
  watt::EffectiveSettings e;
  watt::SettingsOrigin api{watt::SettingsOrigin::Level::Folder, "C:/ws/api", "api"};
  e.auth_from = api;
  e.headers.push_back({{"Authorization", "Bearer abcdef0123456789", "", true}, api, false});
  e.headers.push_back({{"X-Trace", "t", "", true}, {watt::SettingsOrigin::Level::Collection, "C:/ws", "Collection"}, false});
  e.headers.push_back({{"X-Old", "o", "", true}, api, true});    // replaced by a nearer level
  e.headers.push_back({{"X-Off", "o", "", false}, api, false});  // disabled
  const InheritView v = make_inherit_view(e);
  CHECK(v.known);
  CHECK(v.type == "Bearer token");
  CHECK(v.source == "api");
  CHECK(v.detail == "token abcdef\xE2\x80\xA6" "6789");
  REQUIRE(v.headers.size() == 2);
  CHECK(v.headers[0].auth);
  CHECK(v.headers[1].from == "Collection");
  CHECK(v.summary() == "Bearer token from api: token abcdef\xE2\x80\xA6" "6789");

  watt::EffectiveSettings none;
  CHECK(make_inherit_view(none).source.empty());
  CHECK(make_inherit_view(none).type == "No auth");

  watt::EffectiveSettings basic;
  basic.auth_from = {watt::SettingsOrigin::Level::Collection, "C:/ws", "Collection"};
  basic.basic_auth = {true, "bob", "x"};
  CHECK(make_inherit_view(basic).summary() == "Basic auth from Collection: user bob");
}

TEST_CASE("mask_secret and header bulk text", "[http_editor][inherit]") {
  CHECK(mask_secret("{{token}}") == "{{token}}");
  CHECK(mask_secret("short") == "*****");
  CHECK(mask_secret("") == "(empty)");
  const auto rows = rows_from_bulk("X-A: 1\n// X-B: 2\n\nX-C:3\x01\n");
  REQUIRE(rows.size() == 3);
  CHECK(rows[0].name == "X-A");
  CHECK(!rows[1].enabled);
  CHECK(rows[2].value == "3");
  CHECK(rows_from_bulk(rows_to_bulk(rows)) == rows);
}

TEST_CASE("inherit end to end: a folder's Bearer token and headers reach an inheriting request", "[http_editor][inherit]") {
  const auto dir = std::filesystem::temp_directory_path() / "watt_test_inherit";
  std::filesystem::remove_all(dir);
  watt::Workspace ws = watt::Workspace::create(dir);
  std::filesystem::create_directories(dir / "api" / "users");
  watt::FolderSettings coll;
  coll.headers = {{"X-Coll", "c", "", true}, {"X-Trace", "coll", "", true}};
  REQUIRE(ws.save_folder_settings(dir, coll));
  watt::FolderSettings api;
  FolderForm f;
  f.auth = FolderAuth::Bearer;
  f.token = "tok-{{userId}}";
  f.headers = {{"X-Trace", "api", "", true}};
  apply_folder_form(f, api);
  REQUIRE(ws.save_folder_settings(dir / "api", api));
  ws.globals["userId"] = "42";

  watt::SavedRequest r;
  r.kind = watt::RequestKind::Http;
  r.http.method = "GET";
  r.http.url = "http://127.0.0.1:1/x";
  r.file = dir / "api" / "users" / "get.req.json";
  r.auth.type = watt::HttpAuth::Type::Inherit;
  const InheritView v = make_inherit_view(ws.effective_settings(r));
  CHECK(v.type == "Bearer token");
  CHECK(v.source == "api");
  auto sent = ws.http_to_send(r);
  REQUIRE(sent);
  auto header = [&](const std::string& n) {
    for (const auto& [k, val] : sent->headers)
      if (iequals(k, n)) return val;
    return std::string("<none>");
  };
  CHECK(header("Authorization") == "Bearer tok-42");
  CHECK(header("X-Trace") == "api");  // the nearer folder wins
  CHECK(header("X-Coll") == "c");
  // The request's own header replaces the folder's.
  r.http.headers = {{"X-Trace", "mine"}};
  sent = ws.http_to_send(r);
  REQUIRE(sent);
  CHECK(header("X-Trace") == "mine");
  // A nearer folder's Basic auth wins over the parent's for the inheriting request.
  watt::FolderSettings basic;
  basic.auth.type = watt::HttpAuth::Type::None;
  basic.basic_auth = {true, "u", "p"};
  REQUIRE(ws.save_folder_settings(dir / "api" / "users", basic));
  CHECK(make_inherit_view(ws.effective_settings(r)).type == "Basic auth");
  std::filesystem::remove_all(dir);
}

TEST_CASE("find bar: counting and replacing matches", "[http_editor][find]") {
  auto m = count_matches("Foo foo FOO fo", "foo", false, 5);
  CHECK(m.total == 3);
  CHECK(m.before == 2);
  CHECK(count_matches("Foo foo FOO", "foo", true, 0).total == 1);
  CHECK(count_matches("aaaa", "aa", true, 0).total == 2);  // non-overlapping
  CHECK(count_matches("ab", "abc", true, 0).total == 0);
  CHECK(count_matches("", "a", true, 0).total == 0);
  CHECK(count_matches("aaaa", "a", true, 0, 2).capped);
  size_t n = 0;
  CHECK(replace_all("a-A-b", "a", "x", false, &n) == "x-x-b");
  CHECK(n == 2);
  CHECK(replace_all("a-A-b", "a", "", true, &n) == "-A-b");
  CHECK(replace_all("abc", "", "x", true, &n) == "abc");
}

TEST_CASE("docs preview: Markdown subset, raw HTML escaped, only http(s) links", "[http_editor][docs]") {
  const std::string h = markdown_to_html("# Title\n\nSome **bold** and *it* and `x<y`.\n\n- one\n- two\n\n1. first\n\n"
                                         "```\n<script>alert(1)</script>\n```\n> quote\n\n"
                                         "[ok](https://example.com) [bad](javascript:alert(1)) <img src=x onerror=y>\n"
                                         "snake_case_name");
  CHECK(h.find("<h1>Title</h1>") != std::string::npos);
  CHECK(h.find("<b>bold</b>") != std::string::npos);
  CHECK(h.find("<i>it</i>") != std::string::npos);
  CHECK(h.find("<code>x&lt;y</code>") != std::string::npos);
  CHECK(h.find("<ul>\n<li>one</li>\n<li>two</li>\n</ul>") != std::string::npos);
  CHECK(h.find("<ol>\n<li>first</li>") != std::string::npos);
  CHECK(h.find("&lt;script&gt;alert(1)&lt;/script&gt;") != std::string::npos);
  CHECK(h.find("<script") == std::string::npos);
  CHECK(h.find("<img") == std::string::npos);
  CHECK(h.find("<blockquote>quote</blockquote>") != std::string::npos);
  CHECK(h.find("<a href=\"https://example.com\">ok</a>") != std::string::npos);
  CHECK(h.find("javascript:") == std::string::npos);
  CHECK(h.find("snake_case_name") != std::string::npos);
}

TEST_CASE("docs preview: CRLF and CR line ends", "[http_editor][docs]") {
  const std::string h = markdown_to_html("# T\r\rtext\r\n- a\r\n- b\r\n");
  CHECK(h.find("<h1>T</h1>") != std::string::npos);
  CHECK(h.find("<p>text</p>") != std::string::npos);
  CHECK(h.find("<li>a</li>\n<li>b</li>") != std::string::npos);
}

TEST_CASE("brace_problem flags a {{var}} typed wrong, not JSON", "[http_editor]") {
  CHECK(brace_problem("").empty());
  CHECK(brace_problem("{{userId}}").empty());
  CHECK(brace_problem("Bearer {{token}} x {{ a }}").empty());
  CHECK(brace_problem("{\"a\":{\"b\":1}}").empty());
  CHECK(brace_problem("{\"a\":{\"b\":true}}").empty());
  CHECK(brace_problem("{{{literal}}").empty());
  // what the native auto-append produced
  CHECK_FALSE(brace_problem("Id}}").empty());
  CHECK_FALSE(brace_problem("{{d}").empty());
  CHECK_FALSE(brace_problem("{userd}}").empty());
  CHECK_FALSE(brace_problem("{userId}}").empty());
  CHECK_FALSE(brace_problem("{{user").empty());
  CHECK_FALSE(brace_problem("x={{userId}}&y={{other").empty());
  CHECK(brace_problem("{{user").find("{{user") != std::string::npos);
}

TEST_CASE("find_match goes forward / backward with wrap, ignoring case", "[http_editor]") {
  const std::string hay = "Item 1, item 2, ITEM 3";
  CHECK(find_match(hay, "item", false, 0, true) == 0);
  CHECK(find_match(hay, "item", false, 1, true) == 8);
  CHECK(find_match(hay, "item", false, 17, true) == 0);  // wraps
  CHECK(find_match(hay, "item", true, 1, true) == 8);
  CHECK(find_match(hay, "item", false, 8, false) == 0);
  CHECK(find_match(hay, "item", false, 0, false) == 16);  // wraps to the last
  CHECK(find_match(hay, "nope", false, 0, true) == std::string::npos);
  CHECK(find_match(hay, "", false, 0, true) == std::string::npos);
}

TEST_CASE("case-insensitive find stays linear when one case never occurs", "[http_editor]") {
  // 8 MB of lower-case "item" with no capital I: each candidate used to rescan the rest of the
  // text for an 'I' (minutes); now one pass.
  std::string hay;
  hay.reserve(8u << 20);
  while (hay.size() < (8u << 20)) hay += "{\"id\": 1, \"name\": \"item 1\", \"active\": true}\n";
  hay += "item 99999";
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(find_match(hay, "item 99999", false, 0, true) == hay.size() - 10);
  const MatchCount m = count_matches(hay, "item 99999", false, 0);
  CHECK(m.total == 1);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  CHECK(ms < 2000);
}
