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
