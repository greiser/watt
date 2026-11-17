// Pure helpers of the HTTP request editor (src/app/http_editor_util.*): query encoding between
// the Params table and the URL, URL normalisation at send time, bulk edit text, header cleaning.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

// The helpers have no wx dependency; they are compiled into this test directly.
#include "app/http_editor_util.cpp"
#include "app/http_editor_codegen.cpp"
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
