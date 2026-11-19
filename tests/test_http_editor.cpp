// Pure helpers of the HTTP request editor (src/app/http_editor_util.*): query encoding between
// the Params table and the URL, URL normalisation at send time, bulk edit text, header cleaning.
#include <catch2/catch_test_macros.hpp>

#include <winsock2.h>  // before anything that may pull in windows.h (the embedded test server)

#include <algorithm>
#include <chrono>
#include <thread>
#include <string>
#include <vector>

// The helpers have no wx dependency; they are compiled into this test directly.
#include "app/http_editor_util.cpp"
#include "app/http_editor_codegen.cpp"
#include "app/http_editor_jshelp.cpp"
#include "app/http_editor_cookies.cpp"
#include "app/http_editor_gql.cpp"
#include "core/model.h"
#include "core/script.h"

#include <filesystem>
#include <fstream>

using namespace watt::app::http_editor;
using watt::Cookie;
using watt::SameSite;

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
  // '+' as in JavaScript: numbers add, a string anywhere on the left concatenates.
  CHECK(pre_eval("\"p-\" + (1+1)", ws) == "p-2");
  CHECK(pre_eval("1 + 2 + 'x'", ws) == "3x");
  CHECK(pre_eval("'x' + 1 + 2", ws) == "x12");
  CHECK(pre_eval("0.5 + 1", ws) == "1.5");
  CHECK(pre_eval("007", ws) == "007");  // a lone number stays as written
  CHECK(pre_eval("length('abc') + 1", ws) == "4");
  CHECK(pre_eval("{{secret}} + 1", ws) == "k3y1");  // variables are strings
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
  // Bruno-style plain text: taken as text (vars expanded) when it is no expression
  ws.globals["userId"] = "42";
  CHECK(check_pre_expr("{{userId}}-x").empty());
  CHECK(pre_eval("{{userId}}-x", ws) == "42-x");
  CHECK(pre_eval("  v1/{{userId}}/items ", ws) == "v1/42/items");
  CHECK(pre_eval("{{nope}}-x", ws, {}, &err) == "<error>");  // an unresolved var is still refused
  // JavaScript methods: the error says what to write instead
  err = check_pre_expr("req.method.toLowerCase()");
  CHECK(err.find("lower(req.method)") != std::string::npos);
  CHECK(pre_eval("req.method.toLowerCase()", ws, {}, &err) == "<error>");
  CHECK(err.find("lower(req.method)") != std::string::npos);
  // a broken expression (quotes / parentheses / +) is never taken as text
  CHECK(!check_pre_expr("sha256('a'").empty());
  CHECK(check_pre_expr("{{a}} - 'x'").find("plain text") != std::string::npos);
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

TEST_CASE("counting many case-insensitive matches is one pass", "[http_editor]") {
  // ~1 MB with ~40k "name" hits and no capital N: the count used to restart a memchr for 'N'
  // from every hit to the end of the text (O(matches x size)).
  std::string hay;
  hay.reserve(1u << 20);
  size_t want = 0;
  while (hay.size() < (1u << 20)) {
    hay += "{\"id\": 1, \"name\": \"x\"},\n";
    ++want;
  }
  size_t want_before = 0;
  for (size_t p = hay.find("name"); p != std::string::npos && p < hay.size() / 2; p = hay.find("name", p + 4))
    ++want_before;
  const auto t0 = std::chrono::steady_clock::now();
  for (int r = 0; r < 10; ++r) {
    const MatchCount m = count_matches(hay, "name", false, hay.size() / 2);
    CHECK(m.total == want);
    CHECK(m.before == want_before);
  }
  CHECK(find_match(hay, "name", false, hay.size(), false) == hay.rfind("name"));
  CHECK(find_match(hay, "NAME", false, 30, false) == hay.find("name"));
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  CHECK(ms < 300);
  // Mixed case: both cases' next hits kept in step.
  CHECK(count_matches("Name name NAME nAmE na", "name", false, 0).total == 4);
  CHECK(count_matches("Name name NAME nAmE na", "name", true, 0).total == 1);
  // A set stop flag ends the walk.
  std::atomic<bool> stop{true};
  CHECK(count_matches(hay, "name", false, 0, 1000000, &stop).cancelled);
  CHECK(find_match(hay, "name", false, hay.size(), false, &stop) == std::string::npos);
}

TEST_CASE("header names must be RFC 9110 tokens", "[http_editor]") {
  CHECK(header_name_problem("Authorization").empty());
  CHECK(header_name_problem("X-Custom_1.v2!").empty());
  CHECK(header_name_problem("").empty());
  CHECK(header_name_problem("  Accept  ").empty());           // surrounding blanks are trimmed
  CHECK(header_name_problem("X-{{tenant}} -Id").find("spaces") != std::string::npos);
  CHECK(header_name_problem("X-{{tenant}}-Id").empty());       // a {{variable}} is not checked
  // A name of several words is not "a name and its value".
  const std::string words = header_name_problem("X Bad/Name");
  CHECK(words.find("Value column") == std::string::npos);
  CHECK(words.find("\"X-Bad-Name\"") != std::string::npos);
  CHECK(header_name_problem("Authorization Bearer tok").find("Value column") != std::string::npos);
  const std::string sp = header_name_problem("Authorization Bearer tok");
  CHECK(sp.find("spaces") != std::string::npos);
  CHECK(sp.find("AuthorizationBearertok") != std::string::npos);
  CHECK(header_name_problem("Content-Type: text/plain").find("':'") != std::string::npos);
  CHECK(header_name_problem("Content-Dispositiontext/plain").find("'/'") != std::string::npos);
  CHECK(!header_name_problem("X-\xC3\xA4").empty());
}

TEST_CASE("jar cookies for a URL follow the RFC 6265 match rules", "[http_editor][cookies]") {
  const int64_t now = 1'800'000'000;
  auto mk = [](std::string n, std::string d, std::string p, bool host_only, bool secure, int64_t exp, int64_t created) {
    Cookie c;
    c.name = std::move(n);
    c.value = "v";
    c.domain = std::move(d);
    c.path = std::move(p);
    c.host_only = host_only;
    c.secure = secure;
    c.expires = exp;
    c.created = created;
    return c;
  };
  const std::vector<Cookie> all = {
      mk("root", "example.com", "/", false, false, 0, 1),
      mk("api", "example.com", "/api", true, false, now + 60, 2),
      mk("sec", "example.com", "/", true, true, 0, 3),
      mk("old", "example.com", "/", true, false, now - 1, 4),
      mk("other", "other.com", "/", true, false, 0, 5),
      mk("host", "api.example.com", "/", true, false, 0, 6),
  };
  auto names = [](const std::vector<Cookie>& v) {
    std::string s;
    for (const auto& c : v) s += c.name + ",";
    return s;
  };
  CHECK(names(jar_cookies_for(all, "http://example.com/api/users?x=1", now)) == "api,root,");
  CHECK(names(jar_cookies_for(all, "https://EXAMPLE.com:8443/", now)) == "root,sec,");
  CHECK(names(jar_cookies_for(all, "https://api.example.com/api", now)) == "root,host,");  // root: Domain=
  CHECK(names(jar_cookies_for(all, "http://example.com/apiary", now)) == "root,");          // path boundary
  CHECK(jar_cookies_for(all, "ftp://example.com/", now).empty());
  CHECK(jar_cookies_for(all, "{{base}}/x", now).empty());
  CHECK(names(jar_cookies_for(all, "example.com/api", now)) == "api,root,");  // sent as http://
  CHECK(url_host("https://user:pw@Api.Example.com:443/x") == "api.example.com");
  CHECK(url_host("http://[::1]:8080/") == "::1");
}

TEST_CASE("cookie expiry and attribute text", "[http_editor][cookies]") {
  const int64_t now = 1'800'000'000;
  CHECK(expiry_text(0, now) == "Session");
  CHECK(expiry_text(now - 5, now) == "expired");
  CHECK(expiry_text(now + 300, now) == "in 5 min");
  CHECK(expiry_text(now + 3 * 3600, now) == "in 3 h");
  CHECK(expiry_text(now + 10 * 86400, now) == "in 10 days");
  CHECK(expiry_text(1800000000 + 400 * 86400, now) == "2028-02-19");
  Cookie c;
  c.secure = true;
  c.same_site = SameSite::Lax;
  c.host_only = false;
  CHECK(cookie_flags(c) == "Secure Lax +subdomains");
}
TEST_CASE("json_block_end finds the matching bracket past strings and comments", "[http_editor]") {
  CHECK(json_block_end("{}", 0) == 1);
  CHECK(json_block_end("[1, [2], {\"a\": \"]}\"}]", 0) == 20);
  CHECK(json_block_end("{\"k\\\"}\": 1}", 0) == 10);  // escaped quote inside a key
  CHECK(json_block_end("{ // }\n /* } */ }", 0) == 16);
  CHECK(json_block_end("{ [ }", 0) == std::string::npos);  // not closed
  CHECK(json_block_end("x{}", 0) == std::string::npos);    // not an opener
  // Long input: candidates on both sides of the 16-byte steps.
  std::string big = "{\"items\": [";
  for (int i = 0; i < 20000; ++i) big += "{\"id\": " + std::to_string(i) + ", \"name\": \"it\\em {[\"},\n";
  big.pop_back();
  big.pop_back();
  big += "]}";
  CHECK(json_block_end(big, 0) == big.size() - 1);
  CHECK(json_block_end(big, big.find('[')) == big.size() - 2);
  CHECK(json_block_end(big, 11) == big.find('}', 11));
}

TEST_CASE("lex_json styles keys after an unquoted {{var}} as keys", "[http_editor]") {
  const std::string t = "{\"id\": {{userId}}, \"name\": \"ada\", \"tags\": [1, true]}";
  JsonLexState st;
  std::string sty;
  std::vector<JsonLexLine> lines;
  lex_json(t, st, &sty, &lines);
  REQUIRE(sty.size() == t.size());
  auto at = [&](std::string_view what, size_t from = 0) { return static_cast<int>(sty[t.find(what, from)]); };
  CHECK(at("\"id\"") == kJsProperty);
  CHECK(at("{{userId}}") == kJsDefault);
  CHECK(at("\"name\"") == kJsProperty);
  CHECK(at("\"ada\"") == kJsString);
  CHECK(at("\"tags\"") == kJsProperty);
  CHECK(at(", \"name\"") == kJsOperator);
  CHECK(at("1,") == kJsNumber);
  CHECK(at("true") == kJsKeyword);
  CHECK(lines.size() == 1);
  CHECK(st.depth == 0);
  // Folding: depth at each line start, headers where a block opens; a comment spans lines.
  const std::string m = "{\n  \"a\": [\n    1 /* x\n y */, bogus\n  ]\n}\n";
  JsonLexState s2;
  lines.clear();
  lex_json(m, s2, &sty, &lines);
  REQUIRE(lines.size() == 6);
  CHECK(lines[0].depth == 0);
  CHECK(lines[0].header);
  CHECK(lines[1].depth == 1);
  CHECK(lines[1].header);
  CHECK(lines[2].depth == 2);
  CHECK_FALSE(lines[2].header);
  CHECK(lines[3].in_comment);
  CHECK(static_cast<int>(sty[m.find("y */")]) == kJsBlockComment);
  CHECK(static_cast<int>(sty[m.find("bogus")]) == kJsError);
  CHECK(static_cast<int>(sty[m.find("bogus") + 5]) != kJsError);  // only the word
  CHECK(lines[4].depth == 2);
  CHECK(lines[5].depth == 1);
  CHECK(s2.depth == 0);
  CHECK_FALSE(s2.in_comment);
  // A string with an escape stays one string; an unclosed one is string-EOL.
  const std::string e = "[\"a\\\"b\", \"open\n";
  JsonLexState s3;
  lex_json(e, s3, &sty, nullptr);
  CHECK(static_cast<int>(sty[e.find("b\"")]) == kJsString);
  CHECK(static_cast<int>(sty[e.find("\\")]) == kJsEscape);
  CHECK(static_cast<int>(sty[e.find("\"open")]) == kJsStringEol);
}

TEST_CASE("json_block_end walks a dense 8 MB body quickly", "[http_editor]") {
  std::string big = "{\"items\": [\n";
  while (big.size() < (8u << 20)) big += "  {\"id\": 1, \"name\": \"item 1\", \"tags\": [\"a\", \"b\"], \"ok\": true},\n";
  big += "  {}\n]}";
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(json_block_end(big, big.find('[')) == big.size() - 2);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  CHECK(ms < 60);
}

TEST_CASE("json_big_blocks lists the long blocks by opener line, outermost per line", "[http_editor]") {
  // line 0: {   line 1: "items": [   lines 2..6: values   line 7: ]   line 8: }
  std::string t = "{\n  \"items\": [\n";
  for (int i = 0; i < 5; ++i) t += "    \"a]\\\"b\",\n";
  t += "  ]\n}\n";
  auto b = json_big_blocks(t, 3);
  REQUIRE(b.size() == 2);
  CHECK(b[0] == std::pair<int, int>{0, 8});
  CHECK(b[1] == std::pair<int, int>{1, 7});
  CHECK(json_big_blocks(t, 9).empty());
  // Two openers on one line: the outer one's end; comments are skipped.
  const std::string two = "[{ // ] }\n1,\n2,\n/* ]\n} */ 3\n}]\n";
  b = json_big_blocks(two, 2);
  REQUIRE(b.size() == 1);
  CHECK(b[0] == std::pair<int, int>{0, 5});
  // 1M lines in well under a second.
  std::string big = "{\"items\": [\n";
  for (int i = 0; i < 1000000; ++i) big += "{\"id\": 1, \"s\": \"x\"},\n";
  big += "{}]}\n";
  const auto t0 = std::chrono::steady_clock::now();
  b = json_big_blocks(big, 20000);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  REQUIRE(b.size() == 1);
  CHECK(b[0] == std::pair<int, int>{0, 1000001});
  CHECK(ms < 400);
}

TEST_CASE("Encode URL off: the path and query go out as typed", "[http_editor][options]") {
  // Only what no request line can carry is encoded.
  CHECK(minimal_url("http://h/a b?q=\"x\"|{y}&e=\xC3\xA9&p=%zz#f g") == "http://h/a%20b?q=\"x\"|{y}&e=\xC3\xA9&p=%zz#f%20g");
  CHECK(minimal_url("http://h/a/../b?x=1") == "http://h/a/../b?x=1");
  CHECK(minimal_url("{{base}}/x y") == "{{base}}/x%20y");
  // Variable values are not query_encode()d either.
  auto exp = [](const std::string& s) { return s == "{{v}}" ? std::string("a&b=c|d") : s; };
  CHECK(expand_url("http://h/?q={{v}}", exp, false) == "http://h/?q=a&b=c|d");
  CHECK(expand_url("http://h/?q={{v}}", exp, true) == "http://h/?q=a%26b%3Dc%7Cd");
  CHECK(with_path_values("http://h/:id", [](const std::string&) { return std::string("a/b"); }, false) == "http://h/a/b");
  CHECK(with_path_values("http://h/:id", [](const std::string&) { return std::string("a/b"); }) == "http://h/a%2Fb");
}

TEST_CASE("Options: what applies is the request's own, else the levels above, else the default", "[http_editor][options]") {
  watt::EffectiveSettings e;
  e.network.max_redirects = 5;
  e.max_redirects_from = {watt::SettingsOrigin::Level::Folder, {}, "api"};
  e.network.encode_url = false;
  e.encode_url_from = {watt::SettingsOrigin::Level::Collection, {}, "Collection"};
  const OptionsAbove above = options_above(e);
  CHECK(above.max_redirects_from == "api");
  CHECK(above.encode_url_from == "Collection");
  CHECK(above.keep_auth_from.empty());
  watt::NetworkSettings own;
  watt::HttpRequest h = effective_options(own, above);
  CHECK(h.max_redirects == 5);
  CHECK_FALSE(h.encode_url);
  CHECK_FALSE(h.keep_auth_on_redirect);
  CHECK(h.tls_min_version == watt::TlsVersion::Default);
  own.max_redirects = 0;
  own.encode_url = true;
  own.tls_min = watt::TlsVersion::Tls1_3;
  h = effective_options(own, above);
  CHECK(h.max_redirects == 0);
  CHECK(h.encode_url);
  CHECK(h.tls_min_version == watt::TlsVersion::Tls1_3);
}

TEST_CASE("Basic auth inside a folder that sets an Authorization header: sent and exported as Basic", "[http_editor][auth]") {
  const auto dir = std::filesystem::temp_directory_path() / "watt_test_basic_vs_folder_auth";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir / "api");
  watt::Workspace ws = watt::Workspace::create(dir);
  ws.globals = {{"token", "tok-777"}};
  watt::FolderSettings coll;
  coll.headers = {{"Authorization", "Bearer {{token}}", {}, true}, {"X-Team", "core", {}, true}};
  REQUIRE(ws.save_collection_settings(coll));
  watt::SavedRequest r;  // as pasted from curl -u 'ada:s3cret'
  r.name = "b";
  r.file = dir / "api" / "b.req.json";
  r.http.url = "http://127.0.0.1:18080/echo";
  r.auth.type = watt::HttpAuth::Type::None;
  r.basic_auth = watt::BasicAuth{true, "ada", "s3cret"};
  const auto h = ws.http_to_send(r);
  REQUIRE(h);
  std::vector<std::string> auth;
  for (const auto& [k, v] : h->headers)
    if (iequals(k, "Authorization")) auth.push_back(v);
  CHECK(auth == std::vector<std::string>{"Basic YWRhOnMzY3JldA=="});
  // Copy as curl: -u only, the collection's Bearer is not exported; X-Team still is.
  watt::SavedRequest x = ws.expanded(r);
  drop_shadowed_basic(x);
  const std::string cmd = watt::to_curl_command(x);
  CHECK(cmd.find("Bearer") == std::string::npos);
  CHECK(cmd.find("ada:s3cret") != std::string::npos);
  CHECK(cmd.find("X-Team") != std::string::npos);
  // The request's own Authorization header wins on the wire: the export drops -u.
  r.http.headers = {{"Authorization", "Bearer mine"}};
  x = ws.expanded(r);
  drop_shadowed_basic(x);
  CHECK_FALSE(x.basic_auth.enabled);
  const std::string own = watt::to_curl_command(x);
  CHECK(own.find("Bearer mine") != std::string::npos);
  CHECK(own.find("s3cret") == std::string::npos);
  CHECK(to_code(x, CodeLang::PythonRequests).find("auth=") == std::string::npos);
  // A disabled own header does not count.
  for (size_t i = 0; i < x.http.headers.size(); ++i)
    if (iequals(x.http.headers[i].first, "Authorization")) x.disabled_headers.insert(i);
  x.basic_auth = watt::BasicAuth{true, "ada", "s3cret"};
  drop_shadowed_basic(x);
  CHECK(x.basic_auth.enabled);
  std::filesystem::remove_all(dir);
}

TEST_CASE("Auto headers: the losing row of each name is marked", "[http_editor][auth]") {
  const std::string from_auth = "   (Auth tab)";
  const std::vector<std::pair<std::string, std::string>> rows = {
      {"Host", "127.0.0.1:18080"},
      {"Authorization", "Basic <user:password>" + from_auth},
      {"Authorization", "Bearer {{token}}   (from Collection)"},
      {"X-Team", "core   (from Collection)"},
      {"Accept", "*/*"}};
  // Basic on the Auth tab: the collection's Authorization loses, the Auth tab's is sent.
  auto lost = auto_header_losers(rows, {}, "Authorization", from_auth);
  CHECK(lost == std::vector<std::string>{"", "", "(replaced by the Auth tab)", "", ""});
  // An Authorization row in the Headers table beats both; Accept in the table replaces the default.
  lost = auto_header_losers(rows, {"authorization", "ACCEPT"}, "Authorization", from_auth);
  CHECK(lost == std::vector<std::string>{"", "(replaced by your header)", "(replaced by your header)", "",
                                         "(replaced by your header)"});
  // No auth header from the Auth tab (inherit / none): the inherited one is sent.
  lost = auto_header_losers(rows, {}, "", from_auth);
  CHECK(lost == std::vector<std::string>(5));
}

#include "../tools/httpserver/http_server.h"
#include "core/http_client.h"
#include "core/oauth2.h"

// The Auth tab's scheme beats an Authorization header inherited from the collection: OAuth 2.0
// (in the header), Digest, NTLM, Negotiate and SigV4 as Basic does (core resolve_with()). Before
// the fix the collection's "Bearer {{token}}" went out and /oauth/protected answered 401.
TEST_CASE("Auth tab schemes beat an inherited Authorization header", "[http_editor][auth][oauth2]") {
  static watt::httpserver::Server srv;
  static const bool started = srv.start(0);
  REQUIRE(started);
  const std::string host = "http://127.0.0.1:" + std::to_string(srv.port());
  const auto dir = std::filesystem::temp_directory_path() / "watt_test_auth_inherit";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  {
    watt::Workspace ws = watt::Workspace::create(dir);
    watt::FolderSettings coll;
    coll.headers = {watt::SettingsRow{"Authorization", "Bearer tok-777", {}, true},
                    watt::SettingsRow{"X-Team", "core", {}, true}};
    REQUIRE(ws.save_collection_settings(coll));
    watt::oauth2_clear_cache();
    watt::SavedRequest r;
    r.id = "auth-inherit-oauth";
    r.file = dir / "o.req.json";
    r.http.url = host + "/oauth/protected";
    r.auth.type = watt::HttpAuth::Type::OAuth2;
    r.auth.oauth2.grant = watt::OAuth2Config::Grant::ClientCredentials;
    r.auth.oauth2.token_url = host + "/oauth/token";
    r.auth.oauth2.client_id = "watt-client";
    r.auth.oauth2.client_secret = "s3cret";
    auto auth_of = [](const watt::HttpRequest& h) {
      std::vector<std::string> out;
      for (const auto& [k, v] : h.headers)
        if (iequals(k, "Authorization")) out.push_back(v);
      return out;
    };
    std::string err;
    auto h = ws.http_to_send(r, &err);
    INFO(err);
    REQUIRE(h);
    const auto a = auth_of(*h);
    REQUIRE(a.size() == 1);
    CHECK(a[0].rfind("Bearer tok-client_credentials-", 0) == 0);
    bool team = false;
    for (const auto& [k, v] : h->headers) team = team || (k == "X-Team" && v == "core");
    CHECK(team);  // other inherited headers still go out
    watt::HttpClient c;
    CHECK(c.send(*h).status == 200);
    // Effective settings mark the collection's Authorization as overridden.
    bool over = false;
    for (const auto& eh : ws.effective_settings(r).headers)
      if (eh.row.name == "Authorization" && eh.from.level == watt::SettingsOrigin::Level::Collection) over = eh.overridden;
    CHECK(over);
    // Copy as curl (export: resolved()) follows the same rule.
    CHECK(watt::to_curl_command(ws.expanded(r)).find("tok-777") == std::string::npos);

    // OAuth 2.0 with the token in the query: the inherited header is kept (nothing replaces it).
    watt::SavedRequest q = r;
    q.id = "auth-inherit-oauth-q";
    q.auth.oauth2.add_to = watt::OAuth2Config::AddTo::Query;
    h = ws.http_to_send(q, &err);
    REQUIRE(h);
    CHECK(auth_of(*h) == std::vector<std::string>{"Bearer tok-777"});

    // Digest / SigV4 (libcurl writes Authorization): no inherited header either.
    watt::SavedRequest d = r;
    d.auth = watt::HttpAuth{};
    d.auth.type = watt::HttpAuth::Type::Digest;
    d.auth.user = "bob";
    d.auth.password = "pw";
    d.http.url = host + "/digest-auth/bob/pw";
    h = ws.http_to_send(d, &err);
    REQUIRE(h);
    CHECK(auth_of(*h).empty());
    CHECK(c.send(*h).status == 200);
    d.auth.type = watt::HttpAuth::Type::AwsSigV4;
    d.auth.aws_access_key = "AKIDEXAMPLE";
    d.auth.aws_secret_key = "secret";
    d.auth.aws_region = "eu-west-1";
    d.auth.aws_service = "execute-api";
    h = ws.http_to_send(d, &err);
    REQUIRE(h);
    CHECK(auth_of(*h).empty());

    // Inherited from a folder: the folder's OAuth 2.0 beats the collection's header too.
    std::filesystem::create_directories(dir / "api");
    watt::FolderSettings api;
    api.auth = r.auth;
    REQUIRE(ws.save_folder_settings(dir / "api", api));
    watt::SavedRequest in = r;
    in.id = "auth-inherit-folder";
    in.file = dir / "api" / "i.req.json";
    in.auth = watt::HttpAuth{};
    in.auth.type = watt::HttpAuth::Type::Inherit;
    h = ws.http_to_send(in, &err);
    INFO(err);
    REQUIRE(h);
    const auto ai = auth_of(*h);
    REQUIRE(ai.size() == 1);
    CHECK(ai[0].rfind("Bearer tok-client_credentials-", 0) == 0);
  }
  std::filesystem::remove_all(dir);
}


TEST_CASE("GraphQL query lint: brackets and strings", "[http_editor][graphql]") {
  CHECK_FALSE(check_graphql("query { user(id: $id) { id name } }"));
  CHECK_FALSE(check_graphql("# a } comment\n{ a(s: \"}\" b: \"\"\"\n ) \"\"\") }"));
  auto i = check_graphql("query { user(id: 1) { id name }");
  REQUIRE(i);
  CHECK(i->offset == 6);
  CHECK(i->message == "'{' is never closed");
  i = check_graphql("{ a(x: 1 }");
  REQUIRE(i);
  CHECK(i->message == "expected ')' before '}'");
  i = check_graphql("{ a }}");
  REQUIRE(i);
  CHECK(i->offset == 5);
  i = check_graphql("{ a(s: \"open) }");
  REQUIRE(i);
  CHECK(i->message == "unterminated string");
  CHECK(check_graphql("{ a(s: \"\"\"x) }"));
}


// ------------------------------------------------------------------ GraphQL schema (http_editor_gql)

namespace {

// A small introspection result: Query { user(id: ID!): User, users(filter: UserFilter, first: Int): [User!]!,
// search(q: String!): [SearchResult] }, User { id, name, role: Role, posts(first: Int): [Post] },
// Post { id, title }, Role enum, UserFilter input, SearchResult union, Mutation { rename }.
const char* kGqlSchemaJson = R"({"data":{"__schema":{
 "queryType":{"name":"Query"},"mutationType":{"name":"Mutation"},"subscriptionType":null,
 "types":[
  {"kind":"OBJECT","name":"Query","description":null,"fields":[
    {"name":"user","description":"One user by id","args":[{"name":"id","description":null,"type":{"kind":"NON_NULL","name":null,"ofType":{"kind":"SCALAR","name":"ID","ofType":null}},"defaultValue":null}],
     "type":{"kind":"OBJECT","name":"User","ofType":null},"isDeprecated":false},
    {"name":"users","description":null,"args":[
       {"name":"filter","description":null,"type":{"kind":"INPUT_OBJECT","name":"UserFilter","ofType":null},"defaultValue":null},
       {"name":"first","description":null,"type":{"kind":"SCALAR","name":"Int","ofType":null},"defaultValue":"10"}],
     "type":{"kind":"NON_NULL","name":null,"ofType":{"kind":"LIST","name":null,"ofType":{"kind":"NON_NULL","name":null,"ofType":{"kind":"OBJECT","name":"User","ofType":null}}}},"isDeprecated":false},
    {"name":"search","description":null,"args":[{"name":"q","description":null,"type":{"kind":"NON_NULL","name":null,"ofType":{"kind":"SCALAR","name":"String","ofType":null}},"defaultValue":null}],
     "type":{"kind":"LIST","name":null,"ofType":{"kind":"UNION","name":"SearchResult","ofType":null}},"isDeprecated":false}
  ],"inputFields":null,"enumValues":null},
  {"kind":"OBJECT","name":"Mutation","fields":[{"name":"rename","args":[{"name":"id","type":{"kind":"SCALAR","name":"ID"}},{"name":"name","type":{"kind":"SCALAR","name":"String"}}],"type":{"kind":"OBJECT","name":"User"}}]},
  {"kind":"OBJECT","name":"User","description":"A person","fields":[
    {"name":"id","args":[],"type":{"kind":"NON_NULL","name":null,"ofType":{"kind":"SCALAR","name":"ID"}}},
    {"name":"name","description":"Display name","args":[],"type":{"kind":"SCALAR","name":"String"}},
    {"name":"role","args":[],"type":{"kind":"ENUM","name":"Role"}},
    {"name":"posts","args":[{"name":"first","type":{"kind":"SCALAR","name":"Int"}}],"type":{"kind":"LIST","ofType":{"kind":"OBJECT","name":"Post"}}}]},
  {"kind":"OBJECT","name":"Post","fields":[{"name":"id","args":[],"type":{"kind":"SCALAR","name":"ID"}},{"name":"title","args":[],"type":{"kind":"SCALAR","name":"String"}}]},
  {"kind":"ENUM","name":"Role","enumValues":[{"name":"ADMIN"},{"name":"USER"}]},
  {"kind":"INPUT_OBJECT","name":"UserFilter","inputFields":[{"name":"role","type":{"kind":"ENUM","name":"Role"}},{"name":"nameLike","type":{"kind":"SCALAR","name":"String"}}]},
  {"kind":"UNION","name":"SearchResult","possibleTypes":[{"kind":"OBJECT","name":"User"},{"kind":"OBJECT","name":"Post"}]},
  {"kind":"SCALAR","name":"ID"},{"kind":"SCALAR","name":"String"},{"kind":"SCALAR","name":"Int"},{"kind":"SCALAR","name":"Boolean"},
  {"kind":"OBJECT","name":"__Schema","fields":[{"name":"types","args":[],"type":{"kind":"LIST","ofType":{"kind":"OBJECT","name":"__Type"}}}]},
  {"kind":"OBJECT","name":"__Type","fields":[{"name":"name","args":[],"type":{"kind":"SCALAR","name":"String"}}]}
 ]}}})";

std::shared_ptr<const GqlSchema> gql_fixture() {
  std::string err;
  auto s = parse_gql_schema(kGqlSchemaJson, &err);
  INFO(err);
  REQUIRE(s);
  return s;
}

std::vector<std::string> names_of(const GqlCompletion& c) {
  std::vector<std::string> v;
  for (const auto& i : c.items) v.push_back(i.name);
  return v;
}

// "|" marks the caret.
GqlCompletion complete_at(const std::string& with_caret, const GqlSchema* s) {
  const size_t at = with_caret.find('|');
  std::string doc = with_caret;
  doc.erase(at, 1);
  return gql_complete(doc, at, s);
}

}  // namespace

TEST_CASE("GraphQL schema: introspection parse", "[http_editor][graphql]") {
  const auto s = gql_fixture();
  CHECK(s->query == "Query");
  CHECK(s->mutation == "Mutation");
  CHECK(s->subscription.empty());
  CHECK(s->user_type_count() == 11);
  const GqlType* q = s->type("Query");
  REQUIRE(q);
  const GqlField* users = q->field("users");
  REQUIRE(users);
  CHECK(users->type == "[User!]!");
  CHECK(users->named == "User");
  REQUIRE(users->arg("first"));
  CHECK(users->arg("first")->default_value == "10");
  CHECK(q->field("user")->args[0].type == "ID!");
  std::string err;
  CHECK_FALSE(parse_gql_schema(R"({"errors":[{"message":"introspection is disabled"}]})", &err));
  CHECK(err == "introspection is disabled");
  CHECK_FALSE(parse_gql_schema("<html>", &err));
  CHECK(err == "the response is not JSON");
  // A bigger schema (~1.5 MB) parses on the worker in well under a second.
  std::string big = R"({"data":{"__schema":{"queryType":{"name":"Query"},"types":[)";
  for (int t = 0; t < 1500; ++t) {
    if (t) big += ",";
    big += R"({"kind":"OBJECT","name":"T)" + std::to_string(t) + R"(","description":"type number )" + std::to_string(t) +
           R"(","fields":[)";
    for (int f = 0; f < 8; ++f)
      big += std::string(f ? "," : "") + R"({"name":"f)" + std::to_string(f) +
             R"(","description":"a field","args":[],"type":{"kind":"NON_NULL","ofType":{"kind":"SCALAR","name":"String"}}})";
    big += "]}";
  }
  big += "]}}}";
  const auto t0 = std::chrono::steady_clock::now();
  const auto bs = parse_gql_schema(big, &err);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  REQUIRE(bs);
  CHECK(bs->types.size() == 1500);
  CHECK(ms < 1000);
}

TEST_CASE("GraphQL schema: field, argument and value completion", "[http_editor][graphql]") {
  const auto s = gql_fixture();
  // The critic's case: "query { user(id: 1) { id na" offers name.
  auto c = complete_at("query { user(id: 1) { id na|", s.get());
  CHECK(c.kind == GqlCompletion::Kind::Field);
  CHECK(c.on == "User");
  CHECK(names_of(c) == std::vector<std::string>{"name"});
  CHECK(c.items[0].detail == "String");
  CHECK(c.items[0].desc == "Display name");
  CHECK(c.from == std::string("query { user(id: 1) { id ").size());
  // Right after "{": every field of the type, __typename last.
  c = complete_at("{ user(id: 1) { | } }", s.get());
  CHECK(names_of(c) == std::vector<std::string>{"id", "name", "posts", "role", "__typename"});
  CHECK(c.items[2].detail == "(first: Int): [Post]");
  // Root fields, nesting, aliases.
  c = complete_at("query Q { |", s.get());
  CHECK(names_of(c) == std::vector<std::string>{"search", "user", "users", "__schema", "__type", "__typename"});
  c = complete_at("{ me: user(id: 1) { posts(first: 2) { t| } } }", s.get());
  CHECK(names_of(c) == std::vector<std::string>{"title"});
  c = complete_at("{ me: u|", s.get());
  CHECK(names_of(c) == std::vector<std::string>{"user", "users"});
  // Inline fragments on a union.
  c = complete_at("{ search(q: \"x\") { ... on Post { | } } }", s.get());
  CHECK(c.on == "Post");
  c = complete_at("{ search(q: \"x\") { ... on |", s.get());
  CHECK(c.kind == GqlCompletion::Kind::Type);
  CHECK(names_of(c) == std::vector<std::string>{"Mutation", "Post", "Query", "SearchResult", "User"});
  // Fragments.
  c = complete_at("fragment F on User { po| }", s.get());
  CHECK(names_of(c) == std::vector<std::string>{"posts"});
  // Arguments: those not given yet.
  c = complete_at("{ users(|) }", s.get());
  CHECK(c.kind == GqlCompletion::Kind::Argument);
  CHECK(c.on == "Query.users");
  CHECK(names_of(c) == std::vector<std::string>{"filter", "first"});
  CHECK(c.items[1].detail == "Int = 10");
  c = complete_at("{ users(first: 2, |) }", s.get());
  CHECK(names_of(c) == std::vector<std::string>{"filter"});
  // Input object fields and enum values.
  c = complete_at("{ users(filter: { |", s.get());
  CHECK(c.kind == GqlCompletion::Kind::InputField);
  CHECK(names_of(c) == std::vector<std::string>{"nameLike", "role"});
  c = complete_at("{ users(filter: { role: A| }) { id } }", s.get());
  CHECK(c.kind == GqlCompletion::Kind::EnumValue);
  CHECK(names_of(c) == std::vector<std::string>{"ADMIN"});
  // Variables declared by the operation.
  c = complete_at("query Q($id: ID!, $n: Int) { user(id: $|", s.get());
  CHECK(c.kind == GqlCompletion::Kind::Variable);
  CHECK(names_of(c) == std::vector<std::string>{"id", "n"});
  CHECK(c.items[0].detail == "ID!");
  c = complete_at("query Q($id: ID!, $n: Int) { users(first: $n|", s.get());
  CHECK(names_of(c) == std::vector<std::string>{"n"});
  // Types in variable definitions.
  c = complete_at("query Q($f: User|", s.get());
  CHECK(names_of(c) == std::vector<std::string>{"UserFilter"});
  // Never inside a string or a comment; keywords at the top.
  CHECK(complete_at("{ search(q: \"na|\") }", s.get()).items.empty());
  CHECK(complete_at("{ user(id: 1) { # na|\n } }", s.get()).items.empty());
  CHECK(names_of(complete_at("mu|", s.get())) == std::vector<std::string>{"mutation"});
  // No schema: $variables still complete.
  c = complete_at("query Q($id: ID!) { user(id: $|", nullptr);
  CHECK(names_of(c) == std::vector<std::string>{"id"});
  CHECK(complete_at("{ user { |", nullptr).items.empty());
}

TEST_CASE("GraphQL schema: unknown fields, arguments and variables", "[http_editor][graphql]") {
  const auto s = gql_fixture();
  CHECK(gql_validate("query Q($id: ID!) { user(id: $id) { id name role posts(first: 2) { title } } }", s.get()).empty());
  CHECK(gql_validate("{ me: user(id: 1) { ...F __typename } } fragment F on User { name }", s.get()).empty());
  CHECK(gql_validate("{ search(q: \"x\") { __typename ... on User { name } ... on Post { title } } }", s.get()).empty());
  CHECK(gql_validate("{ users(filter: {role: ADMIN, nameLike: \"a\"}, first: {{n}}) { id } }", s.get()).empty());
  CHECK(gql_validate("{ __schema { types { name } } }", s.get()).empty());
  CHECK(gql_validate("{ user(id: 1) @include(if: true) { id } }", s.get()).empty());

  const std::string doc = "query { user(id: 1) { id nam } }";
  auto v = gql_validate(doc, s.get());
  REQUIRE(v.size() == 1);
  CHECK(v[0].offset == doc.find("nam"));
  CHECK(v[0].length == 3);
  CHECK(v[0].message == "Unknown field 'nam' on type User");
  v = gql_validate("{ user(id: 1, x: 2) { id } }", s.get());
  REQUIRE(v.size() == 1);
  CHECK(v[0].message == "Unknown argument 'x' on field Query.user");
  v = gql_validate("{ users(filter: {rol: ADMIN}) { id } }", s.get());
  REQUIRE(v.size() == 1);
  CHECK(v[0].message == "Unknown field 'rol' on input type UserFilter");
  v = gql_validate("{ users(filter: {role: ROOT}) { id } }", s.get());
  REQUIRE(v.size() == 1);
  CHECK(v[0].message == "Unknown value 'ROOT' for enum Role");
  v = gql_validate("{ search(q: \"x\") { name } }", s.get());
  REQUIRE(v.size() == 1);
  CHECK(v[0].message == "Unknown field 'name' on union SearchResult (select through ... on Type)");
  v = gql_validate("{ user(id: 1) { name { x } } }", s.get());
  REQUIRE(v.size() == 1);
  CHECK(v[0].message == "Field 'name' is a scalar and has no fields to select");
  v = gql_validate("fragment F on Nope { id }", s.get());
  REQUIRE(v.size() == 1);
  CHECK(v[0].message == "Unknown type 'Nope'");
  v = gql_validate("subscription { x }", s.get());
  REQUIRE(v.size() == 1);
  CHECK(v[0].message == "The schema has no subscription type");
  // A required argument left out, with or without "(...)".
  v = gql_validate("{ user { id } }", s.get());
  REQUIRE(v.size() == 1);
  CHECK(v[0].message == "Field 'user' is missing the required argument 'id: ID!'");
  CHECK(v[0].offset == 2);
  CHECK(v[0].length == 4);
  v = gql_validate("{ search { __typename } }", s.get());
  CHECK(v.size() == 1);
  v = gql_validate("{ user(x: 1) { id } }", s.get());
  CHECK(v.size() == 2);  // x unknown, id missing
  // An object field without its selection set.
  v = gql_validate("{ user(id: 1) }", s.get());
  REQUIRE(v.size() == 1);
  CHECK(v[0].message == "Field 'user' of type User needs a selection of fields: { ... }");
  v = gql_validate("{ user(id: 1) { id posts(first: 2) name } }", s.get());
  REQUIRE(v.size() == 1);
  CHECK(v[0].message.find("Field 'posts'") == 0);
  v = gql_validate("{ user(id: 1) { posts(first: 2) ...F } } fragment F on User { id }", s.get());
  CHECK(v.size() == 1);
  // Still being typed at the end of the text: not flagged yet.
  CHECK(gql_validate("{ user(id: 1) { posts", s.get()).empty());
  // An unknown field's own selection is not checked again (one issue, not a cascade).
  v = gql_validate("{ nope { a b c } }", s.get());
  CHECK(v.size() == 1);
  // Undeclared variables, with or without a schema.
  v = gql_validate("query Q($id: ID!) { user(id: $uid) { id } }", nullptr);
  REQUIRE(v.size() == 1);
  CHECK(v[0].message == "Variable $uid is not declared by the operation Q");
  CHECK(gql_validate("fragment F on User { posts(first: $n) { id } }", nullptr).empty());
  // A document still being typed: no crash, and only what is wrong so far.
  for (const char* partial : {"{", "query (", "{ user(", "{ user(id:", "{ users(filter: {", "{ ... on", "query Q($", "{ a: ",
                              "{ user(id: 1) { posts(first: [{", ")))}}}", "\"\"\"", "{ user(id: \"x"})
    CHECK(gql_validate(partial, s.get()).size() <= 1);
}

TEST_CASE("GraphQL schema: variables JSON against the declarations", "[http_editor][graphql]") {
  const std::string q = "query Q($id: ID!, $n: Int, $r: Role! = USER) { user(id: $id) { id } }";
  CHECK(gql_check_variables(q, "{ \"id\": \"{{userId}}\", \"n\": 2 }").empty());
  CHECK(gql_check_variables(q, "{ \"id\": 1, \"r\": \"ADMIN\" }").empty());
  const std::string vars = "{\n  \"id\": 1,\n  \"idx\": {\"nested\": 1}\n}";
  auto v = gql_check_variables(q, vars);
  REQUIRE(v.size() == 1);
  CHECK(v[0].offset == vars.find("\"idx\""));
  CHECK(v[0].length == 5);
  CHECK(v[0].message == "Variable \"idx\" is not declared by query Q (declared: $id, $n, $r)");
  v = gql_check_variables(q, "{ \"n\": 2 }");
  REQUIRE(v.size() == 1);
  CHECK(v[0].message == "Missing required variable $id (ID!)");
  CHECK(gql_check_variables(q, "").size() == 1);
  CHECK(gql_check_variables(q, "[1]").empty());  // not an object: the JSON check reports it
  // The named operation of several.
  const std::string two = "query A($a: Int!) { x } query B($b: Int) { y }";
  CHECK(gql_check_variables(two, "{\"b\": 1}", "B").empty());
  CHECK(gql_check_variables(two, "{\"b\": 1}").size() == 2);  // the first one: b undeclared, a missing
  v = gql_check_variables("{ user(id: 1) { id } }", "{\"x\": 1}");
  REQUIRE(v.size() == 1);
  CHECK(v[0].message == "Variable \"x\" is not declared by the query (it declares none)");
}

TEST_CASE("GraphQL schema: cache key, file name and age", "[http_editor][graphql]") {
  CHECK(gql_cache_key("https://a/graphql", "dev") != gql_cache_key("https://a/graphql", "prod"));
  const std::string f = gql_cache_file_name(gql_cache_key("https://a/graphql", "dev"));
  CHECK(f.size() == 21);
  CHECK(f.substr(16) == ".json");
  CHECK(f == gql_cache_file_name(gql_cache_key("https://a/graphql", "dev")));
  CHECK(gql_age(10) == "just now");
  CHECK(gql_age(4 * 60) == "4 min ago");
  CHECK(gql_age(3 * 3600) == "3 h ago");
  CHECK(gql_age(3 * 86400) == "3 d ago");
  CHECK(std::string(graphql_introspection_query()).find("__schema") != std::string::npos);
}

TEST_CASE("Headers grid by keyboard: a typed value is never replaced by a suggestion", "[http_editor][kv]") {
  // The critic's case: X-Api-Version, Tab, 1, Enter, then X-Mode gives two rows.
  HeaderGridModel g;
  g.type("X-Api-Version\t1\nX-Mode\tfast\n");
  const auto h = g.headers();
  REQUIRE(h.size() == 2);
  CHECK(h[0] == std::pair<std::string, std::string>{"X-Api-Version", "1"});
  CHECK(h[1] == std::pair<std::string, std::string>{"X-Mode", "fast"});
  CHECK(g.row == 2);
  CHECK(g.col == 0);
  CHECK(g.text.empty());

  // Unknown and X-* headers get no value list; known ones get their own.
  CHECK(header_value_choices("X-Debug").empty());
  CHECK(header_value_choices("X-Api-Version").empty());
  CHECK(completions_for(header_value_choices("Authorization"), "x").empty());
  CHECK(completions_for(header_value_choices("x-debug"), "t").empty());
  CHECK(completions_for(header_value_choices("Expect"), "1") == std::vector<std::string>{"100-continue"});
  CHECK(completions_for(header_value_choices("content-type"), "text/p") == std::vector<std::string>{"text/plain"});
  CHECK(completions_for(header_value_choices("Accept-Encoding"), "GZ").front() == "gzip, deflate, br");
  CHECK(completions_for(header_value_choices("Connection"), "close").empty());  // typed in full: no list

  // Authorization, Tab, x, Enter: "x", not XMLHttpRequest. X-Debug, Tab, t, Enter: "t".
  HeaderGridModel a;
  a.type("Authorization\tx\nX-Debug\tt\n");
  CHECK(a.headers() == std::vector<std::pair<std::string, std::string>>{{"Authorization", "x"}, {"X-Debug", "t"}});

  // The name cell follows the same rule: X-Req, Tab stays X-Req.
  HeaderGridModel n;
  n.type("X-Req\tv\n");
  CHECK(n.headers() == std::vector<std::pair<std::string, std::string>>{{"X-Req", "v"}});

  // An entry chosen with Down goes in, and Enter still commits and moves on.
  HeaderGridModel d;
  d.type("Content-T\v\napplication/j\v\v\nAccept\t*\n");
  CHECK(d.headers() == std::vector<std::pair<std::string, std::string>>{
                           {"Content-Type", "application/json; charset=utf-8"}, {"Accept", "*"}});
  CHECK(d.row == 2);
  CHECK(d.col == 0);

  // A chosen scheme continues: "Bearer " stays open for the token.
  HeaderGridModel b;
  b.type("Authorization\tB\v\ntok\n");
  CHECK(b.headers() == std::vector<std::pair<std::string, std::string>>{{"Authorization", "Bearer tok"}});

  // Tab walks Name -> Value -> Description -> next Name.
  HeaderGridModel t;
  t.type("X-A\t1\tnote\tX-B\t2\n");
  REQUIRE(t.rows.size() == 3);
  CHECK(t.rows[0].desc == "note");
  CHECK(t.headers() == std::vector<std::pair<std::string, std::string>>{{"X-A", "1"}, {"X-B", "2"}});
}

TEST_CASE("names a pre-request script sets", "[http_editor][script]") {
  bool dyn = true;
  CHECK(script_set_names("pm.variables.set('ts', Date.now());\n"
                         "pm.environment.set(\"tok\", 'x');\n"
                         "pm . collectionVariables . set( `c` , 1)\n"
                         "bru.setVar('b1', 2); bru.setEnvVar('b2', 3); bru.setGlobalEnvVar('b3', 4);\n"
                         "pm.globals.set('ts', 5); // pm.variables.set('commented', 1)\n"
                         "/* bru.setVar('blocked', 1) */ const u = 'http://h//x'; xpm.variables.set('no', 1);\n"
                         "pm.variables.get('read')",
                         &dyn) == std::vector<std::string>{"ts", "tok", "c", "b1", "b2", "b3"});
  CHECK_FALSE(dyn);
  CHECK(script_set_names("const n = 'a'; pm.variables.set(n, 1)", &dyn).empty());
  CHECK(dyn);
  CHECK(script_set_names("bru.setVar(`id_${i}`, 1)", &dyn).empty());
  CHECK(dyn);
  CHECK(script_set_names("pm.variables.set('a' + 'b', 1)", &dyn).empty());
  CHECK(dyn);
  CHECK(script_set_names("pm.request.url = 'http://other/'", &dyn).empty());
  CHECK(dyn);
  script_set_names("if (pm.request.url == 'x') pm.variables.set('k', 1)", &dyn);
  CHECK_FALSE(dyn);
  CHECK(script_set_names("", &dyn).empty());
  CHECK_FALSE(dyn);
}

TEST_CASE("a {{var}} set only by the pre-request script is sent with its value", "[http_editor][script]") {
  const auto dir = std::filesystem::temp_directory_path() / "watt_test_http_editor_prevar";
  const auto cfg = std::filesystem::temp_directory_path() / "watt_test_http_editor_prevar_cfg";
  std::filesystem::remove_all(dir);
  std::filesystem::remove_all(cfg);
  std::filesystem::create_directories(dir);
  watt::Workspace::set_user_config_dir(cfg);  // never the real script-trust list
  watt::Workspace ws = watt::Workspace::create(dir);
  REQUIRE(ws.trust_scripts(true));
  watt::SavedRequest r;
  r.kind = watt::RequestKind::Http;
  r.name = "ts";
  r.http.method = "GET";
  r.http.url = "http://127.0.0.1:18080/echo?t={{ts9}}";
  r.http.headers = {{"X-Ts", "{{ts9}}"}};
  r.pre_script = "pm.variables.set('ts9', 'abc');";
  r.file = dir / "ts.req.json";
  // The editor counts the name as defined (no "Not sent" banner) ...
  CHECK(script_set_names(r.pre_script) == std::vector<std::string>{"ts9"});
  // ... and the send path's first send carries the scripted value.
  watt::ScriptRun pre = watt::run_pre_script(ws, r);
  REQUIRE(pre.ok);
  REQUIRE(watt::apply_script_vars(ws, pre, false));
  std::string err;
  const auto sent = ws.http_to_send(r, &err);
  REQUIRE(sent);
  CHECK(sent->url == "http://127.0.0.1:18080/echo?t=abc");
  bool header = false;
  for (const auto& [k, v] : sent->headers) header = header || (k == "X-Ts" && v == "abc");
  CHECK(header);
  std::filesystem::remove_all(dir);
  std::filesystem::remove_all(cfg);
}

TEST_CASE("pre-request variables read folder variables and mix text with calls", "[http_editor][pre]") {
  const auto dir = std::filesystem::temp_directory_path() / "watt_test_pre_folder";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir / "api");
  watt::Workspace ws = watt::Workspace::create(dir);
  watt::FolderSettings fs;
  fs.vars.push_back({"userId", "42"});
  std::string err;
  REQUIRE(ws.save_folder_settings(dir / "api", fs, &err));
  watt::SavedRequest r;
  r.kind = watt::RequestKind::Http;
  r.http.method = "POST";
  r.http.url = "http://127.0.0.1:18080/echo";
  r.file = dir / "api" / "stamp.req.json";
  CHECK(pre_eval("{{userId}} + '-' + lower(req.method)", ws, r, &err) == "42-post");
  CHECK(pre_eval("{{userId}}-x", ws, r) == "42-x");
  CHECK(pre_eval("{{userId}}-lower(req.method)", ws, r, &err) == "42-post");
  CHECK(pre_eval("id lower('AB') / upper( 'c' )", ws, r) == "id ab / C");
  CHECK(check_pre_expr("{{userId}}-lower(req.method)").empty());
  CHECK(pre_eval("{{userId}}-lowr(req.method)", ws, r, &err) == "<error>");
  // Outside the folder the variable is not defined.
  watt::SavedRequest top = r;
  top.file = dir / "stamp.req.json";
  CHECK(pre_eval("{{userId}} + '-x'", ws, top) == "<error>");
  std::filesystem::remove_all(dir);
}

TEST_CASE("Assert Operator cell: a typed symbol picks its own operator", "[http_editor][kv]") {
  // The critic's case: "<" then Tab saved neq (its alias "<>" starts with "<"), a test that
  // passes for nearly any response. The best match is what Tab / Enter picks.
  std::vector<PickMatch> ops;
  for (const AssertOperator& o : assert_operators()) ops.push_back({o.value, o.alias, o.desc});
  auto pick = [&](std::string_view typed) {
    const auto r = rank_pick_matches(ops, typed);
    return r.empty() ? std::string() : ops[r.front()].value;
  };
  CHECK(pick("==") == "eq");
  CHECK(pick("===") == "eq");
  CHECK(pick("!=") == "neq");
  CHECK(pick("!==") == "neq");
  CHECK(pick("<>") == "neq");
  CHECK(pick("<") == "lt");
  CHECK(pick("<=") == "lte");
  CHECK(pick(">") == "gt");
  CHECK(pick(">=") == "gte");
  CHECK(pick("=") == "eq");
  CHECK(pick("!") == "neq");
  // words: a prefix wins over a word that only contains it; case does not matter
  CHECK(pick("l") == "lt");
  CHECK(pick("LTE") == "lte");
  CHECK(pick("cont") == "contains");
  CHECK(pick("not") == "notIn");
  CHECK(pick("in") == "in");
  CHECK(pick("regular") == "matches");  // from the description
  CHECK(pick("zzz").empty());
  // Postman / Chai / Bruno words for existence (the critic typed "exists" and got eq)
  CHECK(pick("exists") == "isDefined");
  CHECK(pick("exist") == "isDefined");
  CHECK(pick("to.exist") == "isDefined");
  CHECK(pick("defined") == "isDefined");
  CHECK(pick("undefined") == "isUndefined");
  CHECK(pick("to.not.exist") == "isUndefined");
  CHECK(pick("in") == "in");
  CHECK(pick("not") == "notIn");
  CHECK(pick("includes") == "contains");
  CHECK(pick("empty") == "isEmpty");
  CHECK(pick("null") == "isNull");
  CHECK(pick("le") == "length");
  CHECK(rank_pick_matches(ops, "").size() == ops.size());
}

TEST_CASE("an inherited folder pre-script's variable is sent fresh on every send", "[http_editor][script][folders]") {
  // The critic's case: api/users/folder.json sets a timestamp with bru.setVar, the request uses
  // it in its URL; from the second send on the request went out with the previous run's value.
  const auto dir = std::filesystem::temp_directory_path() / "watt_test_http_editor_folderstamp";
  const auto cfg = std::filesystem::temp_directory_path() / "watt_test_http_editor_folderstamp_cfg";
  std::filesystem::remove_all(dir);
  std::filesystem::remove_all(cfg);
  std::filesystem::create_directories(dir / "api" / "users");
  watt::Workspace::set_user_config_dir(cfg);
  watt::Workspace real = watt::Workspace::create(dir);
  REQUIRE(real.trust_scripts(true));
  watt::FolderSettings fs;
  fs.pre_script = "bru.setVar('stamp', 'S' + Date.now());";
  REQUIRE(real.save_folder_settings(dir / "api" / "users", fs));
  watt::SavedRequest r = real.new_request(watt::RequestKind::Http, "stamp", dir / "api" / "users");
  r.http.url = "http://127.0.0.1:18080/echo?s={{stamp}}";
  REQUIRE(real.save_request(r));
  std::string previous;
  for (int send = 0; send < 3; ++send) {
    // As request_tab.cpp send_http_job: a workspace copy for the worker, the pre-request chain,
    // its variables into the copy, then the request built from the copy.
    // The editor builds the URL as sent on the UI thread (sent_url(): {{vars}} encoded where
    // they stand), from the live workspace whose runtime {{stamp}} is the previous run's.
    const VarExpander live = [&](const std::string& span) { return real.expand(span, r); };
    const std::vector<std::string> names = script_set_names(fs.pre_script);
    REQUIRE(names == std::vector<std::string>{"stamp"});
    if (!previous.empty())  // the bug: expanded here, the previous value is baked into the URL
      CHECK(expand_url(r.http.url, live, true) == "http://127.0.0.1:18080/echo?s=" + previous);
    const std::string built = expand_url(r.http.url, keep_script_vars(live, names, false), true);
    CHECK(built == "http://127.0.0.1:18080/echo?s={{stamp|q}}");  // encoded by the core
    CHECK(expand_url(r.http.url, keep_script_vars(live, {}, true), true) == built);  // a computed name
    watt::Workspace wsc = real;
    watt::SavedRequest toSend = r;
    toSend.http.url = built;
    watt::ScriptRun pre = watt::run_pre_script(wsc, toSend);
    INFO("send " << send << ": " << pre.error);
    REQUIRE(pre.ok);
    REQUIRE(pre.vars_set.size() == 1);
    const std::string stamp = pre.vars_set[0].value;
    CHECK(stamp != previous);
    REQUIRE(watt::apply_script_vars(wsc, pre, false));
    std::string err;
    const auto sent = wsc.http_to_send(toSend, &err);
    INFO(err);
    REQUIRE(sent);
    CHECK(sent->url == "http://127.0.0.1:18080/echo?s=" + stamp);
    REQUIRE(watt::apply_script_vars(real, pre, false));  // the UI thread's copy, as after the send
    CHECK(real.runtime_vars["stamp"] == stamp);
    previous = stamp;
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
  }
  std::filesystem::remove_all(dir);
  std::filesystem::remove_all(cfg);
}

TEST_CASE("a script-set variable is encoded in the query and a form body as an environment value is",
          "[http_editor][script][encoding]") {
  // The critic's case: bru.setVar('user', 'x&admin=1#frag') injected a parameter and cut the
  // rest of the query off as a fragment; 'dave 1700...' made libcurl reject the URL.
  const auto dir = std::filesystem::temp_directory_path() / "watt_test_http_editor_scriptenc";
  const auto cfg = std::filesystem::temp_directory_path() / "watt_test_http_editor_scriptenc_cfg";
  std::filesystem::remove_all(dir);
  std::filesystem::remove_all(cfg);
  std::filesystem::create_directories(dir);
  watt::Workspace::set_user_config_dir(cfg);
  watt::Workspace real = watt::Workspace::create(dir);
  REQUIRE(real.trust_scripts(true));
  const std::string tmpl = "http://127.0.0.1:18080/echo?u={{user}}&s=1";
  const std::string form_tmpl = "u={{user}}&s=1";
  struct Case {
    std::string value, query, form;
  };
  for (const Case& c : {Case{"a b", "u=a%20b", "u=a+b"}, Case{"x&y=1", "u=x%26y%3D1", "u=x%26y%3D1"},
                        Case{"p#q", "u=p%23q", "u=p%23q"}, Case{"x&admin=1#frag", "u=x%26admin%3D1%23frag", "u=x%26admin%3D1%23frag"}}) {
    INFO("value '" << c.value << "'");
    // From an environment (runtime variable): the editor encodes it itself.
    watt::Workspace env_ws = real;
    env_ws.runtime_vars["user"] = c.value;
    const VarExpander env_exp = [&](const std::string& span) { return env_ws.expand(span); };
    CHECK(expand_url(tmpl, env_exp, true) == "http://127.0.0.1:18080/echo?" + c.query + "&s=1");
    CHECK(expand_form(form_tmpl, env_exp) == c.form + "&s=1");

    // Set by the pre-request script of this send: kept for the core, which encodes it.
    watt::SavedRequest r = real.new_request(watt::RequestKind::Http, "enc", dir);
    r.pre_script = "const k = 'us' + 'er'; bru.setVar(k, " + std::string("decodeURIComponent('") +
                   query_encode(c.value) + "'));";
    const VarExpander live = [&](const std::string& span) { return real.expand(span, r); };
    r.http.url = expand_url(tmpl, keep_script_vars(live, {}, true), true);  // a computed name: any kept
    CHECK(r.http.url == "http://127.0.0.1:18080/echo?u={{user|q}}&s=1");
    r.http.method = "POST";
    r.http.headers = {{"Content-Type", "application/x-www-form-urlencoded"}};
    r.http.body = expand_form(form_tmpl, keep_script_vars(live, {"user"}, false));
    CHECK(r.http.body == "u={{user|f}}&s=1");
    watt::Workspace wsc = real;
    watt::ScriptRun pre = watt::run_pre_script(wsc, r);
    INFO(pre.error);
    REQUIRE(pre.ok);
    REQUIRE(watt::apply_script_vars(wsc, pre, false));
    REQUIRE(wsc.runtime_vars["user"] == c.value);
    std::string err;
    const auto sent = wsc.http_to_send(r, &err);
    INFO(err);
    REQUIRE(sent);
    CHECK(sent->url == "http://127.0.0.1:18080/echo?" + c.query + "&s=1");
    CHECK(sent->body == c.form + "&s=1");
  }
  // A script reads the template without the marks, and leaving it as read keeps them.
  {
    watt::SavedRequest r = real.new_request(watt::RequestKind::Http, "read", dir);
    r.http.url = "http://127.0.0.1:18080/echo?u={{user|q}}";
    r.pre_script = "bru.setVar('seen', req.getUrl() === 'http://127.0.0.1:18080/echo?u={{user}}' ? 'plain' : 'marked');";
    watt::Workspace wsc = real;
    watt::ScriptRun pre = watt::run_pre_script(wsc, r);
    INFO(pre.error);
    REQUIRE(pre.ok);
    REQUIRE(watt::apply_script_vars(wsc, pre, false));
    CHECK(wsc.runtime_vars["seen"] == "plain");
    CHECK(!pre.request_changed);
    CHECK(r.http.url == "http://127.0.0.1:18080/echo?u={{user|q}}");
  }
  // Undefined: left as the plain {{name}} (the send then names it), never with the mark.
  CHECK(real.expand("u={{nobody|q}}") == "u={{nobody}}");
  CHECK(watt::strip_var_marks("{{a|q}}/{{b|f}}/{{c}}/{{d|x}}") == "{{a}}/{{b}}/{{c}}/{{d|x}}");
  std::filesystem::remove_all(dir);
  std::filesystem::remove_all(cfg);
}

TEST_CASE("a computed setVar name makes every name script-settable", "[http_editor][script][encoding]") {
  bool dyn = false;
  script_set_names("const k = 'us' + 'er';\nbru.setVar(k, 'x&admin=1#frag dave ' + Date.now());\nconsole.log(req.getUrl());", &dyn);
  CHECK(dyn);
  // Every name kept for the core (query spans marked), except an OS environment variable.
  const VarExpander none = [](const std::string& span) { return span; };
  CHECK(expand_url("{{base}}/a b?u={{user}}&e={{process.env.HOME}}&d={{$env.X}}", keep_script_vars(none, {}, true), true) ==
        "{{base}}/a%20b?u={{user|q}}&e={{process.env.HOME}}&d={{$env.X}}");
  CHECK(expand_form("u={{ user }}&e={{process.env.HOME}}", keep_script_vars(none, {}, true)) ==
        "u={{user|f}}&e={{process.env.HOME}}");
}

TEST_CASE("variables in the URL path are encoded with and without a pre-request script",
          "[http_editor][script][encoding]") {
  // Round 5 critic: with any pre-request script the path spans reached the core unmarked and went
  // out raw ("Zoë Müller" -> URL rejected); a script that rewrote the URL dropped every mark.
  const auto dir = std::filesystem::temp_directory_path() / "watt_test_http_editor_pathenc";
  const auto cfg = std::filesystem::temp_directory_path() / "watt_test_http_editor_pathenc_cfg";
  std::filesystem::remove_all(dir);
  std::filesystem::remove_all(cfg);
  std::filesystem::create_directories(dir);
  watt::Workspace::set_user_config_dir(cfg);
  watt::Workspace real = watt::Workspace::create(dir);
  REQUIRE(real.trust_scripts(true));
  real.runtime_vars["base"] = "http://127.0.0.1:18080";
  real.runtime_vars["who"] = "Zo\xC3\xAB M\xC3\xBCller";
  real.runtime_vars["city"] = "New York";
  const std::string who_enc = "Zo%C3%AB%20M%C3%BCller";
  watt::SavedRequest r = real.new_request(watt::RequestKind::Http, "path", dir);
  const VarExpander live = [&](const std::string& span) { return real.expand(span, r); };
  auto send = [&](const std::string& url, const std::string& pre_script) {
    watt::SavedRequest x = r;
    x.http.url = url;
    x.pre_script = pre_script;
    watt::Workspace wsc = real;
    if (!pre_script.empty()) {
      watt::ScriptRun pre = watt::run_pre_script(wsc, x);
      INFO(pre.error);
      REQUIRE(pre.ok);
      REQUIRE(watt::apply_script_vars(wsc, pre, false));
    }
    std::string err;
    const auto sent = wsc.http_to_send(x, &err);
    INFO(err);
    REQUIRE(sent);
    return sent->url;
  };
  const std::string tmpl = "{{base}}/users/{{who}}?c={{city}}";
  const std::string want = "http://127.0.0.1:18080/users/" + who_enc + "?c=New%20York";
  // No script: the editor substitutes and encodes itself.
  CHECK(expand_url(tmpl, live, true) == want);
  CHECK(send(expand_url(tmpl, live, true), "") == want);
  // A pre-request script: every name kept, the path span with the path mark.
  const std::string kept = expand_url(tmpl, keep_script_vars(live, {}, true), true);
  CHECK(kept == "{{base}}/users/{{who|p}}?c={{city|q}}");
  CHECK(send(kept, "console.log(1)") == want);
  // A path variable (:id) holding {{who}}: the same.
  const std::string with_id = with_path_values("{{base}}/users/:id?c={{city}}", [](const std::string&) {
    return std::string("{{who}}");
  }, true);
  CHECK(send(expand_url(with_id, keep_script_vars(live, {}, true), true), "console.log(1)") == want);
  // The path mark keeps reserved characters, encodes a stray %.
  CHECK(watt::var_mark_encode("a/b?c#d %zz %41\"<>", 'p') == "a/b?c#d%20%25zz%20%41%22%3C%3E");
  // An unknown {{name}} stays readable; braces holding anything else are encoded like the rest
  // (a captured value cannot add query parameters by wrapping them in braces).
  CHECK(watt::var_mark_encode("x {{later}}", 'q') == "x%20{{later}}");
  CHECK(watt::var_mark_encode("{{a&admin=1#f}}", 'q').find_first_of("&#") == std::string::npos);
  CHECK(watt::var_mark_encode("{{a/b?c}}", 'p').find('{') == std::string::npos);
  CHECK(watt::strip_var_marks("{{a|p}}/x") == "{{a}}/x");

  // A script that rewrites the URL: the marks come back by position.
  const std::string q = expand_url("{{base}}/echo/{{who}}?u={{user}}&n={{city}}", keep_script_vars(live, {}, true), true);
  CHECK(q == "{{base}}/echo/{{who|p}}?u={{user|q}}&n={{city|q}}");
  const std::string want_q = "http://127.0.0.1:18080/echo/" + who_enc + "?u=x%26admin%3D1&n=New%20York";
  CHECK(send(q, "pm.request.addQueryParams('ts=1'); bru.setVar('user', 'x&admin=1');") == want_q + "&ts=1");
  CHECK(send(q, "req.setUrl(req.getUrl() + '&z=1'); bru.setVar('user', 'x&admin=1');") == want_q + "&z=1");
  CHECK(send(q, "pm.request.url = req.getUrl() + '&p={{city}}'; bru.setVar('user', 'x&admin=1');") ==
        want_q + "&p=New%20York");
  CHECK(send(q, "req.setUrl(req.getUrl().replace('/echo/', '/echo/{{city}}/')); bru.setVar('user', 'a b#c');") ==
        "http://127.0.0.1:18080/echo/New%20York/" + who_enc + "?u=a%20b%23c&n=New%20York");
  CHECK(watt::mark_url_vars("{{base}}/a/{{x}}?q={{y}}&e={{process.env.HOME}}#{{f}}") ==
        "{{base}}/a/{{x|p}}?q={{y|q}}&e={{process.env.HOME}}#{{f|p}}");
  std::filesystem::remove_all(dir);
  std::filesystem::remove_all(cfg);
}
