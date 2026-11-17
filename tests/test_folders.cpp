// Folder / collection settings (core/model.h FolderSettings): folder.json format, inheritance of
// headers, auth, variables, scripts and asserts, the consent checks on inherited settings, and
// the cost of open + tree with folder.json files present.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "core/asserts.h"
#include "core/model.h"
#include "core/model_json.h"
#include "core/script.h"

using namespace watt;
namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

namespace {

fs::path temp_path(const char* prefix) {
  std::random_device rd;
  return fs::temp_directory_path() / (std::string(prefix) + std::to_string(rd()) + std::to_string(rd()));
}

// Never the real per-user config (allowlists, script trust).
struct TestConfigDir {
  fs::path dir = temp_path("watt_test_folders_cfg_");
  TestConfigDir() { Workspace::set_user_config_dir(dir); }
  ~TestConfigDir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
} test_config_dir;

struct TempDir {
  fs::path path = temp_path("watt_test_folders_");
  TempDir() { fs::create_directories(path); }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}

void spit(const fs::path& p, const std::string& s) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  f << s;
}

SettingsRow row(const std::string& n, const std::string& v, bool on = true, const std::string& desc = {}) {
  return SettingsRow{n, v, desc, on};
}

// The value of header `name` (case-insensitive) as sent, and how many there are.
std::vector<std::string> header_values(const HttpRequest& h, const std::string& name) {
  auto lower = [](std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  std::vector<std::string> out;
  for (const auto& [k, v] : h.headers)
    if (lower(k) == lower(name)) out.push_back(v);
  return out;
}

// <ws>/folder.json (collection) -> api/folder.json -> api/users/folder.json -> request.
struct Chain {
  TempDir t;
  Workspace ws = Workspace::create(t.path);
  fs::path a = t.path / "api";
  fs::path b = t.path / "api" / "users";
  SavedRequest req;
  Chain() {
    fs::create_directories(b);
    req = ws.new_request(RequestKind::Http, "Get user", b);
    req.http.url = "http://example.invalid/users/1";
    REQUIRE(ws.save_request(req));
  }
};

std::map<std::string, std::string> all_files(const fs::path& root) {
  std::map<std::string, std::string> out;
  for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
    if (!it->is_regular_file()) continue;
    const std::string rel = it->path().lexically_relative(root).generic_string();
    if (rel.rfind(".watt/", 0) == 0) continue;  // personal cache (index), not part of the workspace
    out[rel] = slurp(it->path());
  }
  return out;
}

}  // namespace

TEST_CASE("folder.json: diff-friendly format, round-trip, written only when set", "[folders][format]") {
  TempDir t;
  Workspace ws = Workspace::create(t.path);
  const fs::path dir = t.path / "api";
  fs::create_directories(dir);
  FolderSettings s;
  s.headers = {row("X-Team", "core"), row("X-Debug", "1", false), row("X-Trace", "{{trace}}", true, "correlation id")};
  s.basic_auth = BasicAuth{true, "{{user}}", "{{pass}}"};
  s.auth.type = HttpAuth::Type::None;  // None + basic enabled: Basic
  s.vars = {row("base", "https://api.example.com"), row("old", "x", false)};
  Assertion as;
  as.target = "res.status";
  as.op = "eq";
  as.value = "200";
  s.asserts = {as};
  s.pre_script = "bru.setVar('a', 1);\nconsole.log('pre');";
  REQUIRE(ws.save_folder_settings(dir, s));
  const std::string text = slurp(dir / "folder.json");
  const std::string expected =
      "{\n"
      "  \"version\": 1,\n"
      "  \"headers\": [\n"
      "    [\"X-Team\", \"core\"]\n"
      "  , [\"X-Debug\", \"1\", false]\n"
      "  , {\n"
      "      \"name\": \"X-Trace\",\n"
      "      \"value\": \"{{trace}}\",\n"
      "      \"description\": \"correlation id\"\n"
      "    }\n"
      "  ],\n"
      "  \"auth\": {\n"
      "    \"type\": \"basic\",\n"
      "    \"user\": \"{{user}}\",\n"
      "    \"password\": \"{{pass}}\"\n"
      "  },\n"
      "  \"vars\": [\n"
      "    [\"base\", \"https://api.example.com\"]\n"
      "  , [\"old\", \"x\", false]\n"
      "  ],\n"
      "  \"asserts\": [\n"
      "    [\"res.status\", \"eq 200\"]\n"
      "  ],\n"
      "  \"pre_script\": [\"bru.setVar('a', 1);\", \"console.log('pre');\"]\n"  // two lines: inline, as in requests
      "}\n";
  CHECK(text == expected);
  auto back = ws.folder_settings(dir);
  REQUIRE(back);
  CHECK(*back == s);
  // Relative folder paths work too, and an unchanged save leaves the file alone.
  auto rel = ws.folder_settings("api");
  REQUIRE(rel);
  CHECK(*rel == s);
  const auto mtime = fs::last_write_time(dir / "folder.json");
  REQUIRE(ws.save_folder_settings(dir, s));
  CHECK(fs::last_write_time(dir / "folder.json") == mtime);

  // Other auth schemes use the request's auth_scheme fields; "none" is explicit; inherit is absent.
  FolderSettings d;
  d.auth.type = HttpAuth::Type::Digest;
  d.auth.user = "u";
  d.auth.password = "p";
  REQUIRE(ws.save_folder_settings(dir, d));
  CHECK_THAT(slurp(dir / "folder.json"), ContainsSubstring("\"type\": \"digest\""));
  CHECK(*ws.folder_settings(dir) == d);
  FolderSettings none;
  none.auth.type = HttpAuth::Type::None;
  REQUIRE(ws.save_folder_settings(dir, none));
  CHECK(slurp(dir / "folder.json") == "{\n  \"version\": 1,\n  \"auth\": {\n    \"type\": \"none\"\n  }\n}\n");
  CHECK(ws.folder_settings(dir)->auth.type == HttpAuth::Type::None);

  // Empty settings: no file at all (removed if there was one).
  REQUIRE(ws.save_folder_settings(dir, FolderSettings{}));
  CHECK_FALSE(fs::exists(dir / "folder.json"));
  CHECK(ws.folder_settings(dir)->empty());
  // Collection settings: <ws>/folder.json.
  FolderSettings c;
  c.vars = {row("v", "1")};
  REQUIRE(ws.save_collection_settings(c));
  CHECK(fs::exists(t.path / "folder.json"));
  CHECK(ws.collection_settings() == c);
  CHECK(*ws.folder_settings(t.path) == c);
}

TEST_CASE("folder.json: hand-written rows and unknown auth types are read safely", "[folders][format]") {
  TempDir t;
  Workspace ws = Workspace::create(t.path);
  spit(t.path / "f" / "folder.json",
       R"({"version": 1, "headers": [["A", "1"], ["B", "2", false, "why"], {"name": "C", "value": "3", "enabled": false},
           ["", "skipped"], 5, ["D"]], "auth": {"type": "kerberos5000"}})");
  auto s = ws.folder_settings(t.path / "f");
  REQUIRE(s);
  REQUIRE(s->headers.size() == 4);
  CHECK(s->headers[1] == row("B", "2", false, "why"));
  CHECK(s->headers[2] == row("C", "3", false));
  CHECK(s->headers[3] == row("D", ""));
  CHECK(s->auth.type == HttpAuth::Type::None);  // unknown scheme: no credentials, not Inherit

  spit(t.path / "bad" / "folder.json", "{ not json");
  std::string err;
  CHECK_FALSE(ws.folder_settings(t.path / "bad", &err));
  CHECK_THAT(err, ContainsSubstring("invalid JSON"));
}

TEST_CASE("folder.json is not a request; old workspaces stay byte-identical", "[folders][tree]") {
  TempDir t;
  {
    Workspace ws = Workspace::create(t.path);
    ws.globals["g"] = "1";
    ws.environments().push_back(Environment{"dev", {{"host", "localhost"}}, {}});
    ws.active_env = "dev";
    REQUIRE(ws.save_settings());
    for (const char* f : {"a/one.req.json", "a/b/two.req.json", "three.req.json"}) {
      SavedRequest r;
      r.name = fs::path(f).stem().stem().string();
      r.file = t.path / f;
      r.http.url = "http://{{host}}/x";
      r.http.headers = {{"Accept", "text/plain"}};
      r.basic_auth = BasicAuth{true, "u", "p"};
      REQUIRE(ws.save_request(r));
    }
  }
  const auto before = all_files(t.path);
  {
    auto ws = Workspace::open(t.path);
    REQUIRE(ws);
    CollectionNode tree = ws->tree(-1);
    // Every request keeps exactly its meaning: None auth (here Basic), no inheritance.
    auto r = ws->load_request(t.path / "a" / "b" / "two.req.json");
    REQUIRE(r);
    CHECK(r->auth.type == HttpAuth::Type::None);
    auto h = ws->http_to_send(*r);
    REQUIRE(h);
    CHECK(h->url == "http://localhost/x");
    CHECK(h->headers.size() == 2);
    CHECK_FALSE(ws->has_inherited_settings(*r));
    REQUIRE(ws->save_request(*r));
    REQUIRE(ws->save_settings());
    // resolved()/expanded() leave a request without folder settings as it was.
    const SavedRequest x = ws->resolved(*r);
    CHECK(x.http.headers == r->http.headers);
    CHECK(x.inherited_headers == 0);
  }
  CHECK(all_files(t.path) == before);
  CHECK_FALSE(fs::exists(t.path / "folder.json"));

  // A folder.json (and a request that happens to be called "folder") in the tree.
  {
    auto ws = Workspace::open(t.path);
    FolderSettings s;
    s.headers = {row("X", "1")};
    REQUIRE(ws->save_folder_settings(t.path / "a", s));
    REQUIRE(ws->save_collection_settings(s));
    SavedRequest named;
    named.name = "folder";
    named.file = t.path / "a" / "folder.req.json";
    REQUIRE(ws->save_request(named));
    CollectionNode tree = ws->tree(-1);
    std::vector<std::string> names;
    std::function<void(const CollectionNode&)> walk = [&](const CollectionNode& n) {
      for (const auto& c : n.children) {
        names.push_back(c.name + (c.is_folder ? "/" : ""));
        walk(c);
      }
    };
    walk(tree);
    std::sort(names.begin(), names.end());
    CHECK(names == std::vector<std::string>{"a/", "b/", "folder", "one", "three", "two"});
  }
}

TEST_CASE("headers: collection -> folders -> request, overrides, disabled rows", "[folders][headers]") {
  Chain c;
  FolderSettings coll, a, b;
  coll.headers = {row("X-Coll", "c"), row("X-Shared", "from-collection"), row("X-Off", "never", false)};
  a.headers = {row("x-shared", "from-a"), row("X-A", "a"), row("X-Override-Me", "a")};
  b.headers = {row("X-B", "b"), row("X-Req", "b-loses")};
  REQUIRE(c.ws.save_collection_settings(coll));
  REQUIRE(c.ws.save_folder_settings(c.a, a));
  REQUIRE(c.ws.save_folder_settings(c.b, b));
  c.req.http.headers = {{"x-req", "own"}, {"X-Override-Me", "disabled-own"}};
  c.req.disabled_headers = {1};  // a disabled own header does not replace the inherited one
  auto h = c.ws.http_to_send(c.req);
  REQUIRE(h);
  std::vector<std::pair<std::string, std::string>> want = {
      {"X-Coll", "c"}, {"x-shared", "from-a"}, {"X-A", "a"}, {"X-Override-Me", "a"}, {"X-B", "b"}, {"x-req", "own"}};
  CHECK(h->headers == want);
  CHECK(header_values(*h, "X-Off").empty());

  // expanded() / resolved(): inherited first (count recorded), disabled index shifted, idempotent.
  const SavedRequest x = c.ws.expanded(c.req);
  CHECK(x.settings_merged);
  CHECK(x.inherited_headers == 5);
  CHECK(x.disabled_headers == std::set<size_t>{6});
  const SavedRequest xx = c.ws.expanded(x);
  CHECK(xx.http.headers == x.http.headers);
  auto h2 = c.ws.http_to_send(x);
  REQUIRE(h2);
  CHECK(h2->headers == want);

  // effective_settings: every row with its origin, overridden ones flagged.
  const EffectiveSettings e = c.ws.effective_settings(c.req);
  REQUIRE(e.headers.size() == 10);
  CHECK(e.headers[0].from.level == SettingsOrigin::Level::Collection);
  CHECK(e.headers[0].from.label == "Collection");
  CHECK(e.headers[1].row.name == "X-Shared");
  CHECK(e.headers[1].overridden);  // by api's x-shared
  CHECK_FALSE(e.headers[2].overridden);  // disabled: listed, not sent, not "overridden"
  CHECK_FALSE(e.headers[2].row.enabled);
  CHECK(e.headers[3].from.label == "api");
  CHECK_FALSE(e.headers[3].overridden);
  CHECK(e.headers[7].from.label == "api/users");
  CHECK(e.headers[7].row.name == "X-Req");
  CHECK(e.headers[7].overridden);  // by the request's x-req
  CHECK(e.headers[8].from.level == SettingsOrigin::Level::Request);
  CHECK_FALSE(e.headers[9].row.enabled);
  CHECK_FALSE(e.headers[5].overridden);  // the own X-Override-Me is disabled
  CHECK(c.ws.has_inherited_settings(c.req));
}

TEST_CASE("gRPC: inherited headers become metadata", "[folders][grpc]") {
  Chain c;
  FolderSettings a;
  a.headers = {row("x-tenant", "{{tenant}}"), row("x-own", "inherited")};
  a.vars = {row("tenant", "acme")};
  REQUIRE(c.ws.save_folder_settings(c.a, a));
  SavedRequest g = c.ws.new_request(RequestKind::Grpc, "Call", c.b);
  g.grpc_target.address = "localhost:50051";
  g.grpc_method = "/pkg.Svc/M";
  g.grpc_opts.metadata = {{"X-Own", "mine"}};
  const SavedRequest x = c.ws.expanded(g);
  std::vector<std::pair<std::string, std::string>> want = {{"x-tenant", "acme"}, {"X-Own", "mine"}};
  CHECK(x.grpc_opts.metadata == want);
  CHECK(x.inherited_metadata == 1);
  CHECK(x.http.headers.empty());
}

TEST_CASE("auth: Inherit walks up; None stops it; saved requests keep their meaning", "[folders][auth]") {
  Chain c;
  CHECK(c.req.auth.type == HttpAuth::Type::Inherit);  // new_request(): inherit by default
  CHECK(c.req.file.parent_path() == c.b);
  {
    auto loaded = c.ws.load_request(c.req.file);
    REQUIRE(loaded);
    CHECK(loaded->auth.type == HttpAuth::Type::Inherit);
    CHECK_THAT(slurp(c.req.file), ContainsSubstring("\"auth_scheme\": {\n      \"type\": \"inherit\"\n    }"));
  }
  // Nothing anywhere: no auth.
  auto h = c.ws.http_to_send(c.req);
  REQUIRE(h);
  CHECK(header_values(*h, "Authorization").empty());
  CHECK(h->auth == HttpRequest::Auth{});

  // Collection Digest, api inherits, api/users inherits: Digest from the collection.
  FolderSettings coll;
  coll.auth.type = HttpAuth::Type::Digest;
  coll.auth.user = "{{u}}";
  coll.auth.password = "pw";
  coll.vars = {row("u", "digest-user")};
  REQUIRE(c.ws.save_collection_settings(coll));
  FolderSettings a;
  a.headers = {row("X-A", "1")};  // auth stays Inherit
  REQUIRE(c.ws.save_folder_settings(c.a, a));
  h = c.ws.http_to_send(c.req);
  REQUIRE(h);
  CHECK(h->auth == HttpRequest::Auth::Digest);
  CHECK(h->auth_user == "digest-user");
  EffectiveSettings e = c.ws.effective_settings(c.req);
  CHECK(e.auth.type == HttpAuth::Type::Digest);
  CHECK(e.auth_from.level == SettingsOrigin::Level::Collection);

  // api: Basic. Nearest decides.
  a.auth.type = HttpAuth::Type::None;
  a.basic_auth = BasicAuth{true, "ann", "secret"};
  REQUIRE(c.ws.save_folder_settings(c.a, a));
  h = c.ws.http_to_send(c.req);
  REQUIRE(h);
  CHECK(header_values(*h, "Authorization") == std::vector<std::string>{"Basic YW5uOnNlY3JldA=="});
  CHECK(h->auth == HttpRequest::Auth{});
  e = c.ws.effective_settings(c.req);
  CHECK(e.auth_from.label == "api");
  CHECK(e.basic_auth.enabled);

  // api/users: None stops the walk there.
  FolderSettings b;
  b.auth.type = HttpAuth::Type::None;
  REQUIRE(c.ws.save_folder_settings(c.b, b));
  h = c.ws.http_to_send(c.req);
  REQUIRE(h);
  CHECK(header_values(*h, "Authorization").empty());
  CHECK(c.ws.effective_settings(c.req).auth_from.label == "api/users");
  REQUIRE(c.ws.save_folder_settings(c.b, FolderSettings{}));

  // The request's own auth wins; a request saved with None (every file without auth_scheme)
  // does not inherit.
  SavedRequest own = c.req;
  own.auth.type = HttpAuth::Type::None;
  h = c.ws.http_to_send(own);
  REQUIRE(h);
  CHECK(header_values(*h, "Authorization").empty());
  own.basic_auth = BasicAuth{true, "me", "x"};
  h = c.ws.http_to_send(own);
  REQUIRE(h);
  CHECK(header_values(*h, "Authorization") == std::vector<std::string>{"Basic bWU6eA=="});
  // An own Authorization header beats inherited Basic, as with curl -u -H.
  SavedRequest hdr = c.req;
  hdr.http.headers = {{"Authorization", "Bearer t"}};
  h = c.ws.http_to_send(hdr);
  REQUIRE(h);
  CHECK(header_values(*h, "Authorization") == std::vector<std::string>{"Bearer t"});
}

TEST_CASE("variables: runtime > request folder (nearest first) > environment > collection > globals", "[folders][vars]") {
  Chain c;
  c.ws.globals = {{"v", "global"}, {"g_only", "g"}, {"coll_vs_global", "global"}};
  c.ws.environments().push_back(Environment{"dev", {{"v", "env"}, {"env_vs_coll", "env"}, {"folder_vs_env", "env"}}, {}});
  c.ws.active_env = "dev";
  REQUIRE(c.ws.save_settings());
  FolderSettings coll, a, b;
  coll.vars = {row("v", "collection"), row("env_vs_coll", "collection"), row("coll_vs_global", "collection"),
               row("coll_only", "c")};
  a.vars = {row("v", "api"), row("a_vs_b", "a"), row("folder_vs_env", "folder")};
  b.vars = {row("v", "users"), row("a_vs_b", "b"), row("off", "x", false)};
  REQUIRE(c.ws.save_collection_settings(coll));
  REQUIRE(c.ws.save_folder_settings(c.a, a));
  REQUIRE(c.ws.save_folder_settings(c.b, b));
  auto ex = [&](const std::string& s) { return c.ws.expand(s, c.req); };
  CHECK(ex("{{v}}") == "users");
  CHECK(ex("{{a_vs_b}}") == "b");
  CHECK(ex("{{folder_vs_env}}") == "folder");
  CHECK(ex("{{env_vs_coll}}") == "env");
  CHECK(ex("{{coll_vs_global}}") == "collection");
  CHECK(ex("{{coll_only}}/{{g_only}}") == "c/g");
  CHECK(ex("{{off}}") == "{{off}}");
  c.ws.runtime_vars["v"] = "runtime";
  CHECK(ex("{{v}}") == "runtime");
  // Without a request: runtime, environment, collection, globals (no folder vars).
  c.ws.runtime_vars.clear();
  CHECK(c.ws.expand("{{v}}") == "env");
  CHECK(c.ws.expand("{{coll_vs_global}}|{{a_vs_b}}") == "collection|{{a_vs_b}}");
  // The send path uses the same order everywhere (url, headers, auth).
  c.req.http.url = "http://{{v}}.invalid/{{coll_only}}";
  c.req.http.headers = {{"X-V", "{{a_vs_b}}"}};
  auto h = c.ws.http_to_send(c.req);
  REQUIRE(h);
  CHECK(h->url == "http://users.invalid/c");
  CHECK(header_values(*h, "X-V") == std::vector<std::string>{"b"});
  // effective_settings: nearest first, overridden flagged (by a nearer folder or the environment).
  const EffectiveSettings e = c.ws.effective_settings(c.req);
  auto find = [&](const std::string& n, const std::string& label) -> const EffectiveSettings::Row* {
    for (const auto& r : e.vars)
      if (r.row.name == n && r.from.label == label) return &r;
    return nullptr;
  };
  REQUIRE(find("v", "api/users"));
  CHECK_FALSE(find("v", "api/users")->overridden);
  CHECK(find("v", "api")->overridden);
  CHECK(find("env_vs_coll", "Collection")->overridden);  // the environment wins
  CHECK_FALSE(find("coll_vs_global", "Collection")->overridden);
}

TEST_CASE("scripts: collection -> folders -> request pre, request -> folders -> collection post", "[folders][scripts]") {
  Chain c;
  REQUIRE(c.ws.trust_scripts(true));
  FolderSettings coll, a, b;
  coll.pre_script = "console.log('pre collection'); bru.setVar('from_coll', 'c1');";
  coll.post_script = "console.log('post collection');";
  coll.vars = {row("cv", "collection-var")};
  a.pre_script = "console.log('pre api ' + bru.getVar('from_coll')); req.setHeader('X-Api', 'set');";
  a.post_script = "console.log('post api'); test('folder test', () => expect(res.status).to.equal(200));";
  a.vars = {row("fv", "folder-var")};
  b.post_script = "console.log('post users');";
  REQUIRE(c.ws.save_collection_settings(coll));
  REQUIRE(c.ws.save_folder_settings(c.a, a));
  REQUIRE(c.ws.save_folder_settings(c.b, b));
  c.req.pre_script = "console.log('pre request ' + req.getHeader('X-Api') + ' ' + pm.variables.get('fv') + ' ' + pm.variables.get('cv'));";
  c.req.post_script = "console.log('post request');";
  SavedRequest send = c.req;
  ScriptRun pre = run_pre_script(c.ws, send);
  INFO(pre.error);
  REQUIRE(pre.ok);
  CHECK(pre.ran);
  std::vector<std::string> logs;
  for (const auto& l : pre.logs) logs.push_back(l.text);
  CHECK(logs == std::vector<std::string>{"pre collection", "pre api c1", "pre request set folder-var collection-var"});
  CHECK(pre.request_changed);
  REQUIRE(pre.vars_set.size() == 1);
  CHECK(pre.vars_set[0].name == "from_coll");

  HttpResponse resp;
  resp.status = 200;
  ResponseData rd(resp);
  ScriptRun post = run_post_script(c.ws, send, rd);
  REQUIRE(post.ok);
  logs.clear();
  for (const auto& l : post.logs) logs.push_back(l.text);
  CHECK(logs == std::vector<std::string>{"post request", "post users", "post api", "post collection"});
  REQUIRE(post.tests.size() == 1);
  CHECK(post.tests[0].pass);

  const EffectiveSettings e = c.ws.effective_settings(c.req);
  REQUIRE(e.pre_scripts.size() == 3);
  CHECK(e.pre_scripts[1].from.label == "api");
  REQUIRE(e.post_scripts.size() == 4);
  CHECK(e.post_scripts[3].from.level == SettingsOrigin::Level::Collection);

  // An error in an inherited script stops the chain and names the script.
  a.pre_script = "nope();";
  REQUIRE(c.ws.save_folder_settings(c.a, a));
  send = c.req;
  pre = run_pre_script(c.ws, send);
  CHECK_FALSE(pre.ok);
  CHECK_THAT(pre.error, ContainsSubstring("api/pre-request.js"));
  CHECK(pre.logs.size() == 1);

  // Inherited scripts need the same trust as the request's own: a request without scripts in a
  // folder with one is not run until the workspace is trusted.
  REQUIRE(c.ws.trust_scripts(false));
  SavedRequest plain = c.req;
  plain.pre_script.clear();
  ScriptRun u = run_pre_script(c.ws, plain);
  CHECK(u.untrusted);
  CHECK_FALSE(u.ran);
}

TEST_CASE("asserts: the request's own plus the inherited ones", "[folders][asserts]") {
  Chain c;
  FolderSettings coll, a;
  Assertion s200;
  s200.target = "res.status";
  s200.op = "eq";
  s200.value = "200";
  Assertion hdr;
  hdr.target = "res.headers.x-api";
  hdr.op = "eq";
  hdr.value = "{{want}}";
  coll.asserts = {s200};
  a.asserts = {hdr};
  a.vars = {row("want", "yes")};
  REQUIRE(c.ws.save_collection_settings(coll));
  REQUIRE(c.ws.save_folder_settings(c.a, a));
  Assertion own;
  own.target = "res.status";
  own.op = "lt";
  own.value = "500";
  c.req.asserts = {own};
  const SavedRequest x = c.ws.resolved(c.req);
  REQUIRE(x.asserts.size() == 3);
  CHECK(x.asserts[0] == own);
  CHECK(x.asserts[1] == s200);
  CHECK(x.asserts[2] == hdr);
  const EffectiveSettings e = c.ws.effective_settings(c.req);
  REQUIRE(e.asserts.size() == 3);
  CHECK(e.asserts[2].from.label == "api");
  HttpResponse resp;
  resp.status = 201;
  ResponseData rd(resp);
  const auto res = evaluate_asserts(x, rd, &c.ws);
  REQUIRE(res.size() == 3);
  CHECK(res[0].pass);
  CHECK_FALSE(res[1].pass);
  CHECK(res[2].expected == "yes");  // the folder's {{want}}
  // resolving twice does not duplicate them
  CHECK(c.ws.resolved(x).asserts.size() == 3);
}

TEST_CASE("consent checks see inherited uploads, OS env vars and SSO auth", "[folders][security]") {
  Chain c;
#ifdef _WIN32
  _putenv_s("WATT_FOLDERS_SECRET", "s3cr3t");
#else
  setenv("WATT_FOLDERS_SECRET", "s3cr3t", 1);
#endif
  TempDir outside;
  spit(outside.path / "id_rsa", "KEY");
  FolderSettings a;
  a.headers = {row("X-Leak", "{{process.env.WATT_FOLDERS_SECRET}}")};
  a.vars = {row("upload", (outside.path / "id_rsa").string())};
  REQUIRE(c.ws.save_folder_settings(c.a, a));
  c.req.body_file = "{{upload}}";
  // OS env var through an inherited header.
  const auto env = c.ws.blocked_env_vars(c.req);
  REQUIRE(env.size() == 1);
  CHECK(env[0] == "WATT_FOLDERS_SECRET");
  // Upload through a folder variable.
  const auto files = c.ws.blocked_files(c.req);
  REQUIRE(files.size() == 1);
  CHECK(files[0].filename() == "id_rsa");
  std::string err;
  CHECK_FALSE(c.ws.http_to_send(c.req, &err));
  CHECK_THAT(err, ContainsSubstring("WATT_FOLDERS_SECRET"));
  REQUIRE(c.ws.allow_env_var("WATT_FOLDERS_SECRET"));
  err.clear();
  CHECK_FALSE(c.ws.http_to_send(c.req, &err));
  CHECK_THAT(err, ContainsSubstring("id_rsa"));
  REQUIRE(c.ws.allow_file(outside.path / "id_rsa"));
  auto h = c.ws.http_to_send(c.req, &err);
  REQUIRE(h);
  CHECK(h->body == "KEY");
  CHECK(header_values(*h, "X-Leak") == std::vector<std::string>{"s3cr3t"});

  // Single sign-on inherited from the collection: still needs the user's consent per host.
  FolderSettings coll;
  coll.auth.type = HttpAuth::Type::Ntlm;  // no user: the Windows logon
  REQUIRE(c.ws.save_collection_settings(coll));
  SavedRequest sso = c.ws.new_request(RequestKind::Http, "sso", c.b);
  sso.http.url = "http://intranet.example.invalid/";
  const auto hosts = c.ws.blocked_sso_hosts(sso);
  REQUIRE(hosts.size() == 1);
  CHECK(hosts[0] == "http://intranet.example.invalid:80");
  err.clear();
  CHECK_FALSE(c.ws.http_to_send(sso, &err));
  CHECK_THAT(err, ContainsSubstring("Windows account"));
  REQUIRE(c.ws.allow_sso_host(hosts[0]));
  CHECK(c.ws.blocked_sso_hosts(sso).empty());
  h = c.ws.http_to_send(sso, &err);
  REQUIRE(h);
  CHECK(h->auth == HttpRequest::Auth::Ntlm);
  // A request with its own auth (None) is not affected by the collection's NTLM.
  SavedRequest mine = sso;
  mine.auth.type = HttpAuth::Type::None;
  mine.http.url = "http://other.example.invalid/";
  CHECK(c.ws.blocked_sso_hosts(mine).empty());
}

TEST_CASE("inherited headers cannot smuggle CR/LF; folder.json must stay inside the workspace", "[folders][security]") {
  Chain c;
  FolderSettings a;
  a.headers = {row("X-Inject", "a\r\nX-Evil: 1"), row("X-Var", "{{nl}}"), row("X-Fine", "ok")};
  a.vars = {row("nl", "x\nHost: evil")};
  REQUIRE(c.ws.save_folder_settings(c.a, a));
  auto h = c.ws.http_to_send(c.req);
  REQUIRE(h);
  CHECK(header_values(*h, "X-Inject").empty());
  CHECK(header_values(*h, "X-Var").empty());
  CHECK(header_values(*h, "X-Fine") == std::vector<std::string>{"ok"});
  for (const auto& [k, v] : h->headers) CHECK(v.find('\n') == std::string::npos);
  const SavedRequest g = c.ws.expanded(c.req);
  CHECK(g.inherited_headers == 1);
  // The request's own header with a line break (e.g. from a variable) fails the send.
  SavedRequest own = c.req;
  own.http.headers = {{"X-Own", "{{nl2}}"}};
  c.ws.runtime_vars["nl2"] = "a\r\nb";
  std::string err;
  CHECK_FALSE(c.ws.http_to_send(own, &err));
  CHECK_THAT(err, ContainsSubstring("line break"));
  c.ws.runtime_vars.clear();

  // Folders outside the workspace are refused, for loading and saving.
  TempDir other;
  err.clear();
  CHECK_FALSE(c.ws.folder_settings(other.path, &err));
  CHECK_THAT(err, ContainsSubstring("not inside the workspace"));
  CHECK_FALSE(c.ws.save_folder_settings(c.t.path / ".." / other.path.filename(), a));
  CHECK_FALSE(fs::exists(other.path / "folder.json"));
  CHECK_FALSE(c.ws.folder_settings("\\\\evil-host\\share"));
  // A request file outside the workspace (e.g. from a planted history entry) gets only the
  // collection's settings, never the folder.json of where it claims to live.
  spit(other.path / "folder.json", R"({"version":1,"headers":[["X-Outside","1"]]})");
  SavedRequest stray = c.req;
  stray.file = other.path / "x.req.json";
  h = c.ws.http_to_send(stray);
  REQUIRE(h);
  CHECK(header_values(*h, "X-Outside").empty());
  // A link named folder.json is not followed.
  std::error_code ec;
  fs::create_symlink(other.path / "folder.json", c.b / "folder.json", ec);
  c.ws.tree();  // a refresh re-checks every folder.json at once
  if (!ec) {
    err.clear();
    CHECK_FALSE(c.ws.http_to_send(c.req, &err));
    CHECK_THAT(err, ContainsSubstring("is a link"));
    fs::remove(c.b / "folder.json");
  } else {
    WARN("symlinks not available here: " << ec.message());
  }
  // A broken folder.json fails the send (its auth / headers would silently be missing).
  spit(c.b / "folder.json", "{\"headers\": [");
  c.ws.tree();
  err.clear();
  CHECK_FALSE(c.ws.http_to_send(c.req, &err));
  CHECK_THAT(err, ContainsSubstring("folder.json"));
  CHECK(c.ws.effective_settings(c.req).errors.size() == 1);
  CHECK(c.ws.has_inherited_settings(c.req));
  // Fixed on disk, no refresh: seen at most 100 ms later (the stat cache's recheck interval).
  spit(c.b / "folder.json", "{\"version\": 1, \"headers\": [[\"X-Fixed\", \"1\"]]}");
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  h = c.ws.http_to_send(c.req, &err);
  REQUIRE(h);
  CHECK(header_values(*h, "X-Fixed") == std::vector<std::string>{"1"});
}

TEST_CASE("bench: stat cost", "[.statbench]") {
  TempDir t;
  fs::create_directories(t.path / "a" / "b");
  using clock = std::chrono::steady_clock;
  constexpr int kN = 5000;
  detail::PathStat st;
  auto t0 = clock::now();
  for (int i = 0; i < kN; ++i) detail::stat_path(t.path / "a" / "b" / "folder.json", st);
  std::printf("stat_path missing: %.2f us\n", std::chrono::duration<double, std::micro>(clock::now() - t0).count() / kN);
  spit(t.path / "a" / "folder.json", "{}");
  t0 = clock::now();
  for (int i = 0; i < kN; ++i) detail::stat_path(t.path / "a" / "folder.json", st);
  std::printf("stat_path present: %.2f us\n", std::chrono::duration<double, std::micro>(clock::now() - t0).count() / kN);
  const fs::path f = t.path / "a" / "b" / "x.req.json";
  t0 = clock::now();
  size_t n = 0;
  for (int i = 0; i < kN; ++i) n += f.lexically_normal().lexically_relative(t.path).native().size();
  std::printf("lexically_normal+relative: %.2f us (%zu)\n", std::chrono::duration<double, std::micro>(clock::now() - t0).count() / kN, n);
  Workspace ws = Workspace::create(t.path / "ws");
  fs::create_directories(t.path / "ws" / "a" / "b");
  SavedRequest r;
  r.file = t.path / "ws" / "a" / "b" / "x.req.json";
  t0 = clock::now();
  for (int i = 0; i < kN; ++i) (void)ws.has_inherited_settings(r);
  std::printf("has_inherited_settings 3 levels: %.2f us\n", std::chrono::duration<double, std::micro>(clock::now() - t0).count() / kN);
  t0 = clock::now();
  for (int i = 0; i < kN; ++i)
    for (const fs::path& d : {t.path / "ws", t.path / "ws" / "a", t.path / "ws" / "a" / "b"}) detail::stat_path(d / "folder.json", st);
  std::printf("3 stats: %.2f us\n", std::chrono::duration<double, std::micro>(clock::now() - t0).count() / kN);
  r.file.clear();
  t0 = clock::now();
  for (int i = 0; i < kN; ++i) (void)ws.has_inherited_settings(r);
  std::printf("has_inherited_settings root only: %.2f us\n", std::chrono::duration<double, std::micro>(clock::now() - t0).count() / kN);
}

TEST_CASE("perf: open + tree with folder.json in every folder; send-path overhead", "[folders][perf]") {
  TempDir t;
  const fs::path root = t.path / "ws";
  std::vector<fs::path> dirs;
  {
    Workspace ws = Workspace::create(root);
    for (int i = 0; i < 2000; ++i) {
      SavedRequest r = ws.new_request(RequestKind::Http, "req " + std::to_string(i));
      r.http.method = "POST";
      r.http.url = "{{base}}/x/" + std::to_string(i);
      r.http.headers = {{"Content-Type", "application/json"}};
      r.http.body = "{\n  \"a\": 1\n}\n";
      r.file = root / ("folder" + std::to_string(i / 10)) / ("req" + std::to_string(i) + ".req.json");
      REQUIRE(ws.save_request(r));
      if (i % 10 == 0) dirs.push_back(r.file.parent_path());
    }
  }
  using clock = std::chrono::steady_clock;
  auto ms = [](clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
  // Best of several passes (the machine may be busy): open + tree, warm index.
  auto measure = [&](const char* what) {
    double best = 1e9, best_open = 0, best_tree = 0;
    for (int pass = 0; pass < 7; ++pass) {
      const auto t0 = clock::now();
      auto ws = Workspace::open(root);
      REQUIRE(ws);
      const auto t1 = clock::now();
      CollectionNode tree = ws->tree();
      const auto t2 = clock::now();
      size_t folders = 0, requests = 0;
      for (const auto& f : tree.children) {
        folders += f.is_folder;
        requests += f.children.size();
      }
      CHECK(folders == 200);
      CHECK(requests == 2000);  // folder.json is not listed
      if (ms(t2 - t0) < best) {
        best = ms(t2 - t0);
        best_open = ms(t1 - t0);
        best_tree = ms(t2 - t1);
      }
    }
    std::printf("folders perf, %s: open %.2f ms + tree %.2f ms = %.2f ms (best of 7; 2000 requests in 200 folders)\n",
                what, best_open, best_tree, best);
    return best;
  };
  const double without = measure("no folder.json");
  {
    auto ws = Workspace::open(root);
    FolderSettings coll;
    coll.headers = {row("X-Coll", "1")};
    coll.vars = {row("base", "http://localhost")};
    REQUIRE(ws->save_collection_settings(coll));
    for (size_t i = 0; i < dirs.size(); ++i) {
      FolderSettings f;
      f.headers = {row("X-Folder", std::to_string(i))};
      f.vars = {row("folder", std::to_string(i))};
      REQUIRE(ws->save_folder_settings(dirs[i], f));
    }
  }
  const double with = measure("201 folder.json");
  CHECK(with < 50.0);
  CHECK(with < without * 1.5 + 5.0);

  // Send path: http_to_send of a request two levels of settings deep, warm cache.
  auto ws = Workspace::open(root);
  auto r = ws->load_request(root / "folder7" / "req70.req.json");
  REQUIRE(r);
  auto h = ws->http_to_send(*r);
  REQUIRE(h);
  CHECK(h->url == "http://localhost/x/70");
  CHECK(header_values(*h, "X-Folder") == std::vector<std::string>{"7"});
  CHECK(header_values(*h, "X-Coll") == std::vector<std::string>{"1"});
  constexpr int kN = 2000;
  auto t0 = clock::now();
  for (int i = 0; i < kN; ++i) {
    auto x = ws->http_to_send(*r);
    REQUIRE(x);
  }
  const double with_us = ms(clock::now() - t0) * 1000.0 / kN;
  t0 = clock::now();
  for (int i = 0; i < kN; ++i) (void)ws->has_inherited_settings(*r);
  const double chain_us = ms(clock::now() - t0) * 1000.0 / kN;
  t0 = clock::now();
  for (int i = 0; i < kN; ++i) (void)ws->effective_settings(*r);
  const double eff_us = ms(clock::now() - t0) * 1000.0 / kN;
  std::printf("folders perf: http_to_send %.1f us, folder chain lookup %.1f us, effective_settings %.1f us\n", with_us,
              chain_us, eff_us);
  CHECK(chain_us < 100.0);
  // A request in a folder without folder.json, in a workspace without any: the cost of the stats.
  TempDir t2;
  Workspace plain = Workspace::create(t2.path);
  SavedRequest p = plain.new_request(RequestKind::Http, "p", t2.path);
  p.file = t2.path / "a" / "b" / "p.req.json";
  p.http.url = "http://localhost/";
  t0 = clock::now();
  for (int i = 0; i < kN; ++i) (void)plain.has_inherited_settings(p);
  std::printf("folders perf: folder chain lookup, 3 levels without folder.json: %.1f us\n",
              ms(clock::now() - t0) * 1000.0 / kN);
}
