// Workspace model tests: on-disk formats, variables, history, curl/grpcurl import/export.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "core/model.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <wincrypt.h>
#include <dpapi.h>
#include <sddl.h>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "crypt32.lib")
#else
#include <sys/stat.h>
#endif

using namespace watt;
namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using Catch::Matchers::EndsWith;

namespace {

struct TempDir {
  fs::path path;
  TempDir() {
    std::random_device rd;
    path = fs::temp_directory_path() / ("watt_test_model_" + std::to_string(rd()) + std::to_string(rd()));
    fs::create_directories(path);
  }
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

fs::path u8path(const char8_t* s) { return fs::path(std::u8string(s)); }

// Tests never touch the real per-user config (%APPDATA%\watt, ~/.config/watt): the upload
// allowlist goes to a throwaway directory for the whole run; tests that use it set their own.
struct TestConfigDir {
  fs::path dir;
  TestConfigDir() {
    std::random_device rd;
    dir = fs::temp_directory_path() / ("watt_test_cfg_" + std::to_string(rd()) + std::to_string(rd()));
    Workspace::set_user_config_dir(dir);
  }
  ~TestConfigDir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
} test_config_dir;

struct ConfigDirOverride {
  fs::path prev = Workspace::user_config_dir();
  explicit ConfigDirOverride(const fs::path& dir) { Workspace::set_user_config_dir(dir); }
  ~ConfigDirOverride() { Workspace::set_user_config_dir(prev); }
};

#ifdef _WIN32
// The DACL of p: whether it is protected, and the SID (string form) of each ACE ("!" + type for
// anything that is not an access-allowed ACE).
std::vector<std::string> dacl_sids(const fs::path& p, bool* is_protected = nullptr) {
  PSECURITY_DESCRIPTOR sd = nullptr;
  PACL dacl = nullptr;
  if (GetNamedSecurityInfoW(p.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr,
                            &sd) != ERROR_SUCCESS)
    return {"<no security info>"};
  SECURITY_DESCRIPTOR_CONTROL c = 0;
  DWORD rev = 0;
  GetSecurityDescriptorControl(sd, &c, &rev);
  if (is_protected) *is_protected = (c & SE_DACL_PROTECTED) != 0;
  std::vector<std::string> out;
  for (DWORD i = 0; dacl && i < dacl->AceCount; ++i) {
    void* ace = nullptr;
    GetAce(dacl, i, &ace);
    const auto* h = static_cast<ACE_HEADER*>(ace);
    if (h->AceType != ACCESS_ALLOWED_ACE_TYPE) {
      out.push_back("!" + std::to_string(h->AceType));
      continue;
    }
    char* s = nullptr;
    ConvertSidToStringSidA(&static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart, &s);
    out.push_back(s ? s : "?");
    LocalFree(s);
  }
  LocalFree(sd);
  std::sort(out.begin(), out.end());
  return out;
}

std::string current_user_sid() {
  HANDLE tok = nullptr;
  OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok);
  DWORD n = 0;
  GetTokenInformation(tok, TokenUser, nullptr, 0, &n);
  std::vector<BYTE> buf(n);
  GetTokenInformation(tok, TokenUser, buf.data(), n, &n);
  CloseHandle(tok);
  char* s = nullptr;
  ConvertSidToStringSidA(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &s);
  std::string out = s;
  LocalFree(s);
  return out;
}

// A value DPAPI-sealed with other entropy: what secrets.json holds when another program (or a
// tampered file) wrote it; CryptUnprotectData fails on it just as for another user's blob.
std::string foreign_sealed(const std::string& plain) {
  DATA_BLOB in{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
  char other[] = "not-watt";
  DATA_BLOB ent{static_cast<DWORD>(sizeof other - 1), reinterpret_cast<BYTE*>(other)};
  DATA_BLOB out{};
  REQUIRE(CryptProtectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out));
  DWORD len = 0;
  CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &len);
  std::string b64(len, '\0');
  CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64.data(), &len);
  b64.resize(len);
  LocalFree(out.pbData);
  return "dpapi1:" + b64;
}
#endif

// History segment files: .watt/history/00000001.jsonl, ...
fs::path seg_file(const fs::path& ws, int seq, bool recs = false) {
  char name[32];
  std::snprintf(name, sizeof name, recs ? "%08d.req.jsonl" : "%08d.jsonl", seq);
  return ws / ".watt" / "history" / name;
}

std::vector<int> segments(const fs::path& ws) {
  std::vector<int> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(ws / ".watt" / "history", ec)) {
    const std::string n = e.path().filename().string();
    if (n.size() == 14 && n.ends_with(".jsonl")) out.push_back(std::stoi(n.substr(0, 8)));
  }
  std::sort(out.begin(), out.end());
  return out;
}

uint64_t list_bytes(const fs::path& ws) {
  uint64_t n = 0;
  for (int s : segments(ws)) n += fs::file_size(seg_file(ws, s));
  return n;
}

size_t lines_of(const fs::path& p) {
  const std::string t = slurp(p);
  return static_cast<size_t>(std::count(t.begin(), t.end(), '\n'));
}

std::string u8str(const char8_t* s) {
  const std::u8string u(s);
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

std::string header(const HttpRequest& r, const std::string& name) {
  for (const auto& [k, v] : r.headers)
    if (k == name) return v;
  return "<missing>";
}

int count_header(const HttpRequest& r, const std::string& name) {
  int n = 0;
  for (const auto& [k, v] : r.headers) n += (k == name);
  return n;
}

SavedRequest sample_http() {
  SavedRequest r;
  r.name = "Create user";
  r.kind = RequestKind::Http;
  r.http.method = "POST";
  r.http.url = "{{base}}/users?x=1&y=\xC3\xA9";
  r.http.headers = {{"Content-Type", "application/json"}, {"Authorization", "Bearer {{token}}"}, {"X-Empty", ""}};
  r.http.body = "{\n  \"name\": \"Ada \\\"the\\\" first\",\n  \"emoji\": \"\xF0\x9F\x9A\x80\"\n}\n";
  r.http.timeout_ms = 1234;
  r.http.follow_redirects = false;
  r.http.verify_tls = false;
  r.http.version = HttpVersion::Http2PriorKnowledge;
  r.http.fresh_connection = true;
  return r;
}

SavedRequest sample_grpc() {
  SavedRequest r;
  r.name = "Say hello";
  r.kind = RequestKind::Grpc;
  r.grpc_target.address = "{{grpc_host}}:50051";
  r.grpc_target.tls = true;
  r.grpc_target.insecure_skip_verify = true;
  r.grpc_target.authority = "api.internal";
  r.grpc_target.root_cert_pem = "-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n";
  r.grpc_method = "/watt.test.Echo/Say";
  r.grpc_body = "{\"text\": \"hi {{who}}\"}";
  r.grpc_opts.deadline_ms = 2500;
  r.grpc_opts.metadata = {{"authorization", "Bearer {{token}}"}, {"x-trace", "1"}};
  r.proto_files = {"echo.proto"};
  r.import_dirs = {"{{protos}}", "third_party"};
  return r;
}

void require_same(const SavedRequest& a, const SavedRequest& b) {
  CHECK(a.id == b.id);
  CHECK(a.name == b.name);
  CHECK(a.kind == b.kind);
  CHECK(a.http.method == b.http.method);
  CHECK(a.http.url == b.http.url);
  CHECK(a.http.headers == b.http.headers);
  CHECK(a.http.body == b.http.body);
  CHECK(a.http.timeout_ms == b.http.timeout_ms);
  CHECK(a.http.follow_redirects == b.http.follow_redirects);
  CHECK(a.http.verify_tls == b.http.verify_tls);
  CHECK(a.http.version == b.http.version);
  CHECK(a.http.fresh_connection == b.http.fresh_connection);
  CHECK(a.grpc_target == b.grpc_target);
  CHECK(a.grpc_method == b.grpc_method);
  CHECK(a.grpc_body == b.grpc_body);
  CHECK(a.grpc_opts.deadline_ms == b.grpc_opts.deadline_ms);
  CHECK(a.grpc_opts.metadata == b.grpc_opts.metadata);
  CHECK(a.proto_files == b.proto_files);
  CHECK(a.import_dirs == b.import_dirs);
}

}  // namespace

// ==================================================================== workspace + settings

TEST_CASE("create and open a workspace; settings round-trip", "[workspace]") {
  TempDir t;
  const fs::path root = t.path / "ws";
  Workspace ws = Workspace::create(root);
  CHECK(fs::exists(root / "watt.json"));
  CHECK(slurp(root / ".gitignore") == ".watt/\n.env\n");
  CHECK(fs::equivalent(ws.root(), root));

  ws.environments().push_back({"dev", {{"base", "http://localhost:8080"}, {"token", "dev-tok"}}});
  ws.environments().push_back({"prod", {{"base", "https://api.example.com"}}});
  ws.active_env = "prod";
  ws.globals = {{"token", "global-tok"}, {"z", "last"}, {"a", "first"}};
  std::string err;
  REQUIRE(ws.save_settings(&err));

  auto ws2 = Workspace::open(root, &err);
  REQUIRE(ws2);
  CHECK(ws2->active_env == "prod");
  CHECK(ws2->globals == ws.globals);
  REQUIRE(ws2->environments().size() == 2);
  CHECK(ws2->environments()[0].name == "dev");  // user order preserved
  CHECK(ws2->environments()[0].vars.at("token") == "dev-tok");
  CHECK(ws2->environments()[1].name == "prod");

  // Stable, sorted, pretty output: saving again yields identical bytes.
  const std::string first = slurp(root / "watt.json");
  REQUIRE(ws2->save_settings());
  CHECK(slurp(root / "watt.json") == first);
  CHECK(first.find("\"a\": \"first\"") < first.find("\"z\": \"last\""));
  CHECK(first.back() == '\n');

  // create() on an existing workspace opens it instead of clobbering it.
  Workspace ws3 = Workspace::create(root);
  CHECK(ws3.active_env == "prod");
}

// Git-friendliness at workspace level (what Bruno does with environments/*.bru, vars:secret
// and app-held selection): switching environments or editing a secret never touches a tracked
// file, and each environment is its own file.
TEST_CASE("environments: one file each, secrets and active env stay out of git", "[workspace][env]") {
  TempDir t;
  const fs::path root = t.path / "ws";
  Workspace ws = Workspace::create(root);
  Environment dev{"dev", {{"base", "http://localhost:8080"}, {"token", "dev-secret"}}, {"token"}};
  Environment prod{"prod/eu", {{"base", "https://api.example.com"}, {"token", "prod-secret"}}, {"token"}};
  ws.environments() = {dev, prod};
  ws.active_env = "dev";
  REQUIRE(ws.save_settings());

  const std::string gj = slurp(root / "watt.json");
  CHECK_THAT(gj, !ContainsSubstring("active_env"));
  CHECK_THAT(gj, !ContainsSubstring("environments"));
  REQUIRE(fs::exists(root / "environments" / "dev.json"));
  const std::string dev_file = slurp(root / "environments" / "dev.json");
  CHECK_THAT(dev_file, ContainsSubstring("\"base\": \"http://localhost:8080\""));
  CHECK_THAT(dev_file, ContainsSubstring("\"token\""));       // the name is listed...
  CHECK_THAT(dev_file, !ContainsSubstring("dev-secret"));    // ...the value is not
  size_t env_files = 0;
  for (auto& e : fs::directory_iterator(root / "environments")) env_files += e.path().extension() == ".json";
  CHECK(env_files == 2);  // "prod/eu" gets a sanitized file name; its real name is inside
#ifdef _WIN32
  CHECK_THAT(slurp(root / ".watt" / "secrets.json"), !ContainsSubstring("prod-secret"));  // DPAPI-sealed
  CHECK_THAT(slurp(root / ".watt" / "secrets.json"), ContainsSubstring("\"dpapi1:"));
#else
  CHECK_THAT(slurp(root / ".watt" / "secrets.json"), ContainsSubstring("prod-secret"));
#endif
  CHECK_THAT(slurp(root / ".watt" / "state.json"), ContainsSubstring("\"dev\""));

  auto snapshot = [&] {
    std::map<std::string, std::string> m;
    for (auto& e : fs::recursive_directory_iterator(root)) {
      const std::string rel = fs::relative(e.path(), root).generic_string();
      if (e.is_regular_file() && rel.rfind(".watt/", 0) != 0) m[rel] = slurp(e.path());
    }
    return m;
  };
  const auto tracked = snapshot();

  auto ws2 = Workspace::open(root);
  REQUIRE(ws2);
  REQUIRE(ws2->environments().size() == 2);
  CHECK(ws2->environments()[0].name == "dev");
  CHECK(ws2->environments()[1].name == "prod/eu");
  CHECK(ws2->environments()[1].vars.at("token") == "prod-secret");  // merged back from .watt
  CHECK(ws2->environments()[1].secrets == std::set<std::string>{"token"});
  CHECK(ws2->active_env == "dev");
  CHECK(ws2->expand("{{token}}") == "dev-secret");

  // Switching the environment and changing a secret value: no tracked file changes.
  ws2->active_env = "prod/eu";
  ws2->environments()[0].vars["token"] = "rotated";
  REQUIRE(ws2->save_settings());
  CHECK(snapshot() == tracked);
  auto ws3 = Workspace::open(root);
  CHECK(ws3->active_env == "prod/eu");
  CHECK(ws3->environments()[0].vars.at("token") == "rotated");

  // Editing one environment's var touches only that environment's file.
  ws3->environments()[1].vars["base"] = "https://eu.example.com";
  REQUIRE(ws3->save_settings());
  auto now = snapshot();
  CHECK(now["environments/dev.json"] == tracked.at("environments/dev.json"));
  CHECK(now["watt.json"] == tracked.at("watt.json"));
  size_t changed = 0;
  for (const auto& [k, v] : now) changed += !tracked.count(k) || tracked.at(k) != v;
  CHECK(changed == 1);

  // Removing an environment removes its file.
  ws3->environments().pop_back();
  REQUIRE(ws3->save_settings());
  env_files = 0;
  for (auto& e : fs::directory_iterator(root / "environments")) env_files += e.path().extension() == ".json";
  CHECK(env_files == 1);
}

TEST_CASE("environments: pre-split watt.json is read and migrated", "[workspace][env]") {
  TempDir t;
  spit(t.path / "watt.json",
       R"({"version":1,"active_env":"prod","globals":{"g":"1"},"future":true,)"
       R"("environments":[{"name":"dev","vars":{"a":"1"}},{"name":"prod","vars":{"a":"2"}}]})");
  auto ws = Workspace::open(t.path);
  REQUIRE(ws);
  CHECK(ws->active_env == "prod");
  REQUIRE(ws->environments().size() == 2);
  CHECK(ws->expand("{{a}}") == "2");
  REQUIRE(ws->save_settings());
  const std::string gj = slurp(t.path / "watt.json");
  CHECK_THAT(gj, !ContainsSubstring("environments"));
  CHECK_THAT(gj, !ContainsSubstring("active_env"));
  CHECK_THAT(gj, ContainsSubstring("future"));
  auto ws2 = Workspace::open(t.path);
  REQUIRE(ws2);
  CHECK(ws2->active_env == "prod");
  REQUIRE(ws2->environments().size() == 2);
  CHECK(ws2->environments()[0].name == "dev");
  CHECK(ws2->expand("{{a}}") == "2");
}

TEST_CASE(".env feeds {{process.env.X}}", "[workspace][env]") {
  TempDir t;
  Workspace::create(t.path);
  spit(t.path / ".env",
       "# local secrets\r\nAPI_KEY=abc123\nexport QUOTED=\"two words\\nline\"\nSINGLE='x # y'\n"
       "TRAIL=val # comment\n  SPACED = v \nnot a line\n");
  auto ws = Workspace::open(t.path);
  REQUIRE(ws);
  CHECK(ws->expand("{{process.env.API_KEY}}") == "abc123");
  CHECK(ws->expand("{{process.env.QUOTED}}") == "two words\nline");
  CHECK(ws->expand("{{process.env.SINGLE}}") == "x # y");
  CHECK(ws->expand("{{process.env.TRAIL}}") == "val");
  CHECK(ws->expand("{{process.env.SPACED}}") == "v");
  CHECK(ws->expand("{{process.env.NOPE_NOT_SET_42}}") == "{{process.env.NOPE_NOT_SET_42}}");
}

namespace {

// Sets an OS environment variable of the test process for the scope (empty value: unset).
struct ScopedEnv {
  std::string name;
  ScopedEnv(std::string n, const std::string& value) : name(std::move(n)) { set(value); }
  ~ScopedEnv() { set({}); }
  void set(const std::string& value) const {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    if (value.empty()) unsetenv(name.c_str());
    else setenv(name.c_str(), value.c_str(), 1);
#endif
  }
};

}  // namespace

TEST_CASE("OS environment variables expand only once allowed for the workspace", "[workspace][env][security]") {
  TempDir t;
  TempDir cfg;
  ConfigDirOverride use_cfg(cfg.path / "watt");
  const fs::path root = t.path / "ws";
  Workspace::create(root);
  spit(root / ".env", "DOT_ONLY=from-dotenv\nWATT_TEST_BOTH=dot\n");
  const std::string value = "os-secret-4b9e";
  ScopedEnv secret("WATT_TEST_OS_SECRET", value);
  ScopedEnv both("WATT_TEST_BOTH", "os");
  // A planted allowlist inside the workspace (any name a newer version might use) is ignored.
  const std::string planted = "{\"version\": 1, \"workspaces\": {\"" + root.generic_string() + "\": [\"WATT_TEST_OS_SECRET\"], \"" +
                              fs::weakly_canonical(root).generic_string() + "\": [\"WATT_TEST_OS_SECRET\"]}}";
  for (const char* f : {".watt/allowed_env.json", "allowed_env.json", ".watt/allowed_files.json"}) spit(root / f, planted);
  auto ws = Workspace::open(root);
  REQUIRE(ws);

  // Not expanded without opt-in, under either name.
  CHECK(ws->expand("{{process.env.WATT_TEST_OS_SECRET}}") == "{{process.env.WATT_TEST_OS_SECRET}}");
  CHECK(ws->expand("{{$env.WATT_TEST_OS_SECRET}}") == "{{$env.WATT_TEST_OS_SECRET}}");
  CHECK_FALSE(ws->is_env_var_allowed("WATT_TEST_OS_SECRET"));
  // .env values are the workspace's own: they still expand (and win over the OS environment).
  CHECK(ws->expand("{{process.env.DOT_ONLY}}") == "from-dotenv");
  CHECK(ws->expand("{{process.env.WATT_TEST_BOTH}}") == "dot");

  SavedRequest r;
  r.http.method = "POST";
  r.http.url = "https://h.test/{{process.env.DOT_ONLY}}";
  r.http.headers = {{"X-Key", "{{process.env.WATT_TEST_OS_SECRET}}"}, {"X-Unset", "{{process.env.WATT_SURELY_UNSET_VAR}}"}};
  r.http.body = "{\"k\": \"{{indirect}}\"}";
  ws->globals["indirect"] = "{{$env.WATT_TEST_OS_SECRET}}";  // through another variable
  CHECK(ws->blocked_env_vars(r) == std::vector<std::string>{"WATT_TEST_OS_SECRET"});
  std::string err;
  CHECK_FALSE(ws->http_to_send(r, &err));
  CHECK_THAT(err, ContainsSubstring("WATT_TEST_OS_SECRET"));
  CHECK_THAT(err, !ContainsSubstring(value));
  // gRPC fields: expanded() (what the gRPC send, schema load and preconnect use) never resolves a
  // non-allowed OS variable, and blocked_env_vars() reports it for the consent prompt.
  {
    SavedRequest g;
    g.kind = RequestKind::Grpc;
    g.grpc_target.address = "{{process.env.WATT_TEST_OS_SECRET}}.h.test:443";
    g.grpc_target.authority = "{{$env.WATT_TEST_OS_SECRET}}";
    g.grpc_method = "/x.Y/{{process.env.WATT_TEST_OS_SECRET}}";
    g.grpc_body = "{\"k\": \"{{indirect}}\"}";
    g.grpc_opts.metadata = {{"x-key", "{{process.env.WATT_TEST_OS_SECRET}}"}};
    const SavedRequest e = ws->expanded(g);
    for (const std::string* s : {&e.grpc_target.address, &e.grpc_target.authority, &e.grpc_method, &e.grpc_body})
      CHECK_THAT(*s, !ContainsSubstring(value));
    for (const auto& [k, v] : e.grpc_opts.metadata) CHECK_THAT(v, !ContainsSubstring(value));
    CHECK(ws->blocked_env_vars(g) == std::vector<std::string>{"WATT_TEST_OS_SECRET"});
  }
  // Only through the body: blocked too.
  SavedRequest b = r;
  b.http.headers.clear();
  CHECK(ws->blocked_env_vars(b) == std::vector<std::string>{"WATT_TEST_OS_SECRET"});
  CHECK_FALSE(ws->http_to_send(b));
  // An unset variable has nothing to leak: sent with its placeholder.
  SavedRequest u = r;
  u.http.headers = {{"X-Unset", "{{process.env.WATT_SURELY_UNSET_VAR}}"}};
  u.http.body.clear();
  CHECK(ws->blocked_env_vars(u).empty());
  auto h = ws->http_to_send(u, &err);
  REQUIRE(h);
  CHECK(h->url == "https://h.test/from-dotenv");
  CHECK(header(*h, "X-Unset") == "{{process.env.WATT_SURELY_UNSET_VAR}}");

  // The user allows it: stored per user (not in the workspace), then expanded and sent.
  REQUIRE(ws->allow_env_var("WATT_TEST_OS_SECRET", &err));
  CHECK(ws->is_env_var_allowed("WATT_TEST_OS_SECRET"));
  CHECK(ws->allowed_env_vars() == std::vector<std::string>{"WATT_TEST_OS_SECRET"});
  CHECK(ws->expand("{{process.env.WATT_TEST_OS_SECRET}}") == value);
  CHECK(ws->expand("{{$env.WATT_TEST_OS_SECRET}}") == value);
  CHECK(ws->blocked_env_vars(r).empty());
  h = ws->http_to_send(r, &err);
  REQUIRE(h);
  CHECK(header(*h, "X-Key") == value);
  CHECK(h->body == "{\"k\": \"" + value + "\"}");
  CHECK_THAT(slurp(cfg.path / "watt" / "allowed_env.json"), ContainsSubstring("WATT_TEST_OS_SECRET"));
  CHECK(ws->expand("{{process.env.WATT_TEST_BOTH}}") == "dot");
  // Another workspace is not covered by it.
  Workspace other = Workspace::create(t.path / "other");
  CHECK(other.expand("{{process.env.WATT_TEST_OS_SECRET}}") == "{{process.env.WATT_TEST_OS_SECRET}}");
  CHECK(other.blocked_env_vars(r) == std::vector<std::string>{"WATT_TEST_OS_SECRET"});
  // The upload allowlist is untouched; disallowing puts the block back.
  CHECK(ws->allowed_files().empty());
  REQUIRE(ws->disallow_env_var("WATT_TEST_OS_SECRET", &err));
  CHECK(ws->allowed_env_vars().empty());
  CHECK(ws->blocked_env_vars(r) == std::vector<std::string>{"WATT_TEST_OS_SECRET"});
  CHECK_FALSE(ws->http_to_send(r));
}

TEST_CASE("settings keep unknown keys", "[workspace]") {
  TempDir t;
  spit(t.path / "watt.json", R"({"version":1,"active_env":"","future_feature":{"x":[1,2,3]},"globals":{"k":"v"}})");
  auto ws = Workspace::open(t.path);
  REQUIRE(ws);
  CHECK(ws->globals.at("k") == "v");
  ws->globals["k2"] = "v2";
  REQUIRE(ws->save_settings());
  const std::string s = slurp(t.path / "watt.json");
  CHECK_THAT(s, ContainsSubstring("future_feature"));
  CHECK_THAT(s, ContainsSubstring("\"k2\": \"v2\""));
}

TEST_CASE("open reports useful errors", "[workspace]") {
  TempDir t;
  std::string err;
  CHECK_FALSE(Workspace::open(t.path / "missing", &err));
  CHECK_THAT(err, ContainsSubstring("not a directory"));
  CHECK_FALSE(Workspace::open(t.path, &err));
  CHECK_THAT(err, ContainsSubstring("watt.json"));
  spit(t.path / "watt.json", "{ \"version\": 1, oops }");
  CHECK_FALSE(Workspace::open(t.path, &err));
  CHECK_THAT(err, ContainsSubstring("offset"));
}

// ==================================================================== requests on disk

TEST_CASE("HTTP request save/load round-trip", "[request]") {
  TempDir t;
  Workspace ws = Workspace::create(t.path);
  SavedRequest r = sample_http();
  std::string err;
  REQUIRE(ws.save_request(r, &err));
  CHECK(r.id.size() == 36);
  CHECK(r.id[14] == '4');
  CHECK(r.file == ws.root() / "Create user.req.json");

  auto back = ws.load_request(r.file, &err);
  REQUIRE(back);
  require_same(r, *back);
  CHECK(back->file == r.file);
}

TEST_CASE("gRPC request save/load round-trip", "[request]") {
  TempDir t;
  Workspace ws = Workspace::create(t.path);
  SavedRequest r = sample_grpc();
  r.file = "svc/echo/say.req.json";  // relative to the root, folders created on demand
  REQUIRE(ws.save_request(r));
  CHECK(fs::exists(t.path / "svc" / "echo" / "say.req.json"));
  auto back = ws.load_request(r.file);
  REQUIRE(back);
  require_same(r, *back);

  // A gRPC request does not carry an all-defaults "http" section.
  const std::string text = slurp(r.file);
  CHECK(text.find("\"http\"") == std::string::npos);
  CHECK_THAT(text, ContainsSubstring("\"grpc\": {"));
}

TEST_CASE(".req.json is stable, pretty, key-ordered and diff-friendly", "[request]") {
  TempDir t;
  Workspace ws = Workspace::create(t.path);
  SavedRequest r = sample_http();
  r.id = "00000000-0000-4000-8000-000000000001";
  REQUIRE(ws.save_request(r));
  const std::string text = slurp(r.file);
  const std::string expected =
      "{\n"
      "  \"version\": 1,\n"
      "  \"id\": \"00000000-0000-4000-8000-000000000001\",\n"
      "  \"name\": \"Create user\",\n"
      "  \"kind\": \"http\",\n"
      "  \"http\": {\n"
      "    \"method\": \"POST\",\n"
      "    \"url\": \"{{base}}/users?x=1&y=\xC3\xA9\",\n"
      "    \"headers\": [\n"
      "      [\"Content-Type\", \"application/json\"]\n"
      "    , [\"Authorization\", \"Bearer {{token}}\"]\n"
      "    , [\"X-Empty\", \"\"]\n"
      "    ],\n"
      "    \"body\": [\n"
      "      \"{\"\n"
      "    , \"  \\\"name\\\": \\\"Ada \\\\\\\"the\\\\\\\" first\\\",\"\n"
      "    , \"  \\\"emoji\\\": \\\"\xF0\x9F\x9A\x80\\\"\"\n"
      "    , \"}\"\n"
      "    , \"\"\n"
      "    ],\n"
      "    \"timeout_ms\": 1234,\n"
      "    \"follow_redirects\": false,\n"
      "    \"verify_tls\": false,\n"
      "    \"http_version\": \"http2-prior-knowledge\",\n"
      "    \"fresh_connection\": true\n"
      "  }\n"
      "}\n";
  CHECK(text == expected);

  // Re-saving an unchanged request does not rewrite the file.
  const auto mtime = fs::last_write_time(r.file);
  REQUIRE(ws.save_request(r));
  CHECK(fs::last_write_time(r.file) == mtime);
  CHECK(slurp(r.file) == expected);

  // A one-line body edit is a one-line diff.
  r.http.body = "{\n  \"name\": \"Ada \\\"the\\\" first\",\n  \"emoji\": \"x\"\n}\n";
  REQUIRE(ws.save_request(r));
  const std::string text2 = slurp(r.file);
  int diff_lines = 0;
  size_t a = 0, b = 0;
  while (a < text.size() && b < text2.size()) {
    const size_t ea = text.find('\n', a), eb = text2.find('\n', b);
    diff_lines += text.substr(a, ea - a) != text2.substr(b, eb - b);
    a = ea + 1;
    b = eb + 1;
  }
  CHECK(diff_lines == 1);

  // Appending a header, or removing the last one, adds or removes exactly one line.
  auto line_set_diff = [](const std::string& x, const std::string& y) {
    std::vector<std::string> lx, ly;
    for (const auto* p : {&x, &y}) {
      auto& l = p == &x ? lx : ly;
      for (size_t a2 = 0; a2 < p->size();) {
        const size_t e = p->find('\n', a2);
        l.push_back(p->substr(a2, e - a2));
        a2 = e + 1;
      }
    }
    // Longest common subsequence: lines only in one of them.
    std::vector<std::vector<int>> d(lx.size() + 1, std::vector<int>(ly.size() + 1, 0));
    for (size_t i = lx.size(); i-- > 0;)
      for (size_t j = ly.size(); j-- > 0;)
        d[i][j] = lx[i] == ly[j] ? d[i + 1][j + 1] + 1 : std::max(d[i + 1][j], d[i][j + 1]);
    return static_cast<int>(lx.size() + ly.size()) - 2 * d[0][0];
  };
  r.http.headers.emplace_back("X-New", "1");
  REQUIRE(ws.save_request(r));
  const std::string with_new = slurp(r.file);
  CHECK(line_set_diff(text2, with_new) == 1);
  r.http.headers.pop_back();
  r.http.headers.pop_back();
  REQUIRE(ws.save_request(r));
  CHECK(line_set_diff(text2, slurp(r.file)) == 1);
  r.http.headers.emplace_back("X-Empty", "");

  // A CRLF body (Chrome raw multipart) is stored line by line too, each line keeping its \r.
  r.http.body = "--b\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\n1\r\n--b--\r\n";
  REQUIRE(ws.save_request(r));
  const std::string text3 = slurp(r.file);
  CHECK_THAT(text3, ContainsSubstring("      \"--b\\r\"\n    , \"Content-Disposition: form-data; name=\\\"a\\\"\\r\"\n"));
  CHECK(ws.load_request(r.file)->http.body == r.http.body);
}

TEST_CASE("disabled headers stay in the file, in place, and are not sent", "[request]") {
  TempDir t;
  Workspace ws = Workspace::create(t.path);
  SavedRequest r;
  r.name = "h";
  r.http.method = "GET";
  r.http.url = "https://h.test/";
  r.http.headers = {{"X-A", "1"}, {"X-Off", "2"}, {"X-C", "3"}};
  REQUIRE(ws.save_request(r));
  const std::string on = slurp(r.file);
  r.disabled_headers = {1};
  REQUIRE(ws.save_request(r));
  const std::string off = slurp(r.file);
  CHECK_THAT(off, ContainsSubstring("    , [\"X-Off\", \"2\", false]\n"));
  // Toggling a header is a one-line change.
  int changed = 0;
  for (size_t a = 0, b = 0; a < on.size() && b < off.size();) {
    const size_t ea = on.find('\n', a), eb = off.find('\n', b);
    changed += on.substr(a, ea - a) != off.substr(b, eb - b);
    a = ea + 1;
    b = eb + 1;
  }
  CHECK(changed == 1);
  CHECK(std::count(on.begin(), on.end(), '\n') == std::count(off.begin(), off.end(), '\n'));
  auto loaded = ws.load_request(r.file);
  REQUIRE(loaded);
  CHECK(loaded->http.headers.size() == 3);
  CHECK(loaded->disabled_headers == std::set<size_t>{1});
  auto send = ws.http_to_send(*loaded);
  REQUIRE(send);
  REQUIRE(send->headers.size() == 2);
  CHECK(send->headers[0].first == "X-A");
  CHECK(send->headers[1].first == "X-C");
  CHECK_THAT(to_curl_command(*loaded), !ContainsSubstring("X-Off"));
  CHECK_THAT(to_curl_command(*loaded), ContainsSubstring("X-C: 3"));
  // Re-enabling restores the original bytes.
  loaded->disabled_headers.clear();
  REQUIRE(ws.save_request(*loaded));
  CHECK(slurp(r.file) == on);
}

TEST_CASE("load tolerates hand-written and partial files", "[request]") {
  TempDir t;
  Workspace ws = Workspace::create(t.path);
  spit(t.path / "hand.req.json", R"({"http":{"url":"https://x.test","headers":[{"name":"A","value":"1"}],"body":"b"}})");
  auto r = ws.load_request(t.path / "hand.req.json");
  REQUIRE(r);
  CHECK(r->name == "hand");  // falls back to the file name
  CHECK(r->http.method == "GET");
  CHECK(r->http.url == "https://x.test");
  CHECK(r->http.headers == std::vector<Header>{{"A", "1"}});
  CHECK(r->http.timeout_ms == 30000);

  std::string err;
  spit(t.path / "bad.req.json", "{nope");
  CHECK_FALSE(ws.load_request(t.path / "bad.req.json", &err));
  CHECK_THAT(err, ContainsSubstring("bad.req.json"));
  CHECK_FALSE(ws.load_request(t.path / "missing.req.json", &err));
}

TEST_CASE("file names: sanitized, unicode, collisions get a suffix", "[request]") {
  TempDir t;
  Workspace ws = Workspace::create(t.path);
  SavedRequest a;
  a.name = "GET /users/:id?x=1";
  REQUIRE(ws.save_request(a));
  CHECK(a.file.filename() == "GET _users__id_x=1.req.json");

  SavedRequest b;
  b.name = "GET /users/:id?x=1";  // same name, different request
  REQUIRE(ws.save_request(b));
  CHECK(b.file.filename() == "GET _users__id_x=1 2.req.json");
  CHECK(a.file != b.file);

  SavedRequest c;
  c.name = u8str(u8"Übersicht 日本");
  c.http.url = "https://x";
  REQUIRE(ws.save_request(c));
  CHECK(c.file.filename() == u8path(u8"\u00dcbersicht \u65e5\u672c.req.json"));
  auto back = ws.load_request(c.file);
  REQUIRE(back);
  CHECK(back->name == c.name);

  SavedRequest d;
  d.name = "con";
  REQUIRE(ws.save_request(d));
  CHECK(d.file.filename() == "_con.req.json");
}

TEST_CASE("tree lists folders first, sorted, skipping hidden dirs", "[tree]") {
  TempDir t;
  Workspace ws = Workspace::create(t.path);
  for (auto rel : {"zeta.req.json", "Alpha.req.json", "users/list.req.json", "users/admin/ban.req.json",
                   "billing/invoice.req.json", ".git/x.req.json", ".watt/y.req.json"})
    spit(t.path / rel, "{}");
  spit(t.path / "notes.txt", "x");
  fs::create_directories(t.path / "empty");
  SavedRequest h;
  h.name = "hist";
  ws.append_history({"", RequestKind::Http, "GET x", 200, 1.0, h});

  CollectionNode root = ws.tree();
  CHECK(root.is_folder);
  CHECK(root.path == ws.root());
  REQUIRE(root.children.size() == 5);
  CHECK(root.children[0].name == "billing");
  CHECK(root.children[1].name == "empty");
  CHECK(root.children[1].children.empty());
  CHECK(root.children[2].name == "users");
  CHECK(root.children[3].name == "Alpha");
  CHECK_FALSE(root.children[3].is_folder);
  CHECK(root.children[3].path == ws.root() / "Alpha.req.json");
  CHECK(root.children[4].name == "zeta");
  const auto& users = root.children[2];
  REQUIRE(users.children.size() == 2);
  CHECK(users.children[0].name == "admin");
  CHECK(users.children[0].children.at(0).name == "ban");
  CHECK(users.children[1].name == "list");
}

TEST_CASE("tree carries method badges from an mtime-checked index", "[tree]") {
  TempDir t;
  {
    Workspace ws = Workspace::create(t.path);
    SavedRequest a;
    a.name = "create";
    a.http.method = "POST";
    a.http.url = "https://x";
    a.file = t.path / "api" / "create.req.json";
    REQUIRE(ws.save_request(a));
    SavedRequest g = sample_grpc();
    g.name = "say";
    REQUIRE(ws.save_request(g));
    SavedRequest l;
    l.name = std::string(3000, 'n');  // badge beyond the first KiB: full parse fallback
    l.http.method = "PATCH";
    l.file = t.path / "long.req.json";
    REQUIRE(ws.save_request(l));
    CollectionNode root = ws.tree();
    REQUIRE(root.children.size() == 3);
    CHECK(root.children[0].children.at(0).method == "POST");
    CHECK(root.children[1].method == "PATCH");
    CHECK(root.children[2].kind == RequestKind::Grpc);
    CHECK(root.children[2].method.empty());
  }
  CHECK(fs::exists(t.path / ".watt" / "index.json"));
  CHECK_FALSE(fs::exists(t.path / ".watt" / "index.log"));  // the first write is the full index
  // A file changed behind watt's back (git pull, editor) is re-read; removed ones drop out.
  std::string text = slurp(t.path / "api" / "create.req.json");
  text.replace(text.find("\"POST\""), 6, "\"DELETE\"");
  spit(t.path / "api" / "create.req.json", text);
  fs::remove(t.path / "long.req.json");
  Workspace ws = *Workspace::open(t.path);
  CollectionNode root = ws.tree();
  REQUIRE(root.children.size() == 2);
  CHECK(root.children[0].children.at(0).method == "DELETE");
  // Recorded in the journal; index.json itself is not rewritten for small changes.
  const std::string log = slurp(t.path / ".watt" / "index.log");
  CHECK_THAT(log, ContainsSubstring("[\"long.req.json\"]\n"));
  CHECK_THAT(log, ContainsSubstring("[\"api/create.req.json\","));
  CHECK_THAT(log, ContainsSubstring("\"DELETE\"]\n"));
  root = Workspace::open(t.path)->tree(0);
  REQUIRE(root.children.size() == 2);
  CHECK(root.children[0].children.at(0).badge);
  CHECK(root.children[0].children.at(0).method == "DELETE");
}

TEST_CASE("tree index: a save appends one journal line, the log is compacted when it grows", "[tree]") {
  TempDir t;
  const fs::path idx = t.path / ".watt" / "index.json", log = t.path / ".watt" / "index.log";
  {
    Workspace ws = Workspace::create(t.path);
    for (int i = 0; i < 200; ++i) {
      SavedRequest r;
      r.name = "r" + std::to_string(i);
      r.file = t.path / ("f" + std::to_string(i / 20)) / (r.name + ".req.json");
      REQUIRE(ws.save_request(r));
    }
    (void)ws.tree();
  }
  REQUIRE(fs::exists(idx));
  const std::string base = slurp(idx);
  CHECK_FALSE(fs::exists(log));
  // One edit in a later run: one ~100-byte line, index.json untouched.
  {
    Workspace ws = *Workspace::open(t.path);
    auto r = ws.load_request(t.path / "f3" / "r61.req.json");
    REQUIRE(r);
    r->http.method = "PATCH";
    REQUIRE(ws.save_request(*r));
  }
  CHECK(slurp(idx) == base);
  REQUIRE(fs::exists(log));
  CHECK(lines_of(log) == 1);
  CHECK(fs::file_size(log) < 120);
  {
    CollectionNode root = Workspace::open(t.path)->tree(0);
    CHECK(root.children.at(3).children.at(1).name == "r61");
    CHECK(root.children.at(3).children.at(1).method == "PATCH");
  }
  // Many edits: the journal is folded back into index.json once it passes 16 KiB.
  for (int round = 0; round < 5; ++round) {
    Workspace ws = *Workspace::open(t.path);
    for (int i = 0; i < 60; ++i) {
      auto r = ws.load_request(t.path / ("f" + std::to_string(i / 20)) / ("r" + std::to_string(i) + ".req.json"));
      REQUIRE(r);
      r->http.method = round % 2 ? "PUT" : "DELETE";
      REQUIRE(ws.save_request(*r));
    }
    (void)ws.tree();
  }
  CHECK((!fs::exists(log) || fs::file_size(log) <= 16u << 10));
  CHECK(slurp(idx) != base);
  CollectionNode root = Workspace::open(t.path)->tree(0);
  CHECK(root.children.at(0).children.at(5).badge);
  CHECK(root.children.at(0).children.at(5).method == "DELETE");
  CHECK(root.children.at(3).children.at(1).method == "PATCH");
  // A torn journal line (crash mid-append) is skipped.
  {
    std::ofstream f(log, std::ios::binary | std::ios::app);
    f << "[\"f0/r1.req.json\",12";
  }
  root = Workspace::open(t.path)->tree(-1);
  CHECK(root.children.at(0).children.size() == 20);
}

TEST_CASE("tree badges: saves are indexed without tree(); a time budget bounds file reads", "[tree]") {
  TempDir t;
  {
    Workspace ws = Workspace::create(t.path);
    for (int i = 0; i < 20; ++i) {
      SavedRequest r;
      r.name = "r" + std::to_string(i);
      r.http.method = "POST";
      REQUIRE(ws.save_request(r));
    }
  }  // the index is written when the workspace goes away
  CollectionNode root = Workspace::open(t.path)->tree(0);  // zero budget: nothing may be opened
  REQUIRE(root.children.size() == 20);
  for (const auto& c : root.children) {
    CHECK(c.badge);
    CHECK(c.method == "POST");
  }
  // Written by someone else (git pull): with no budget left it has no badge yet.
  spit(t.path / "ext.req.json", "{\"version\":1,\"id\":\"x\",\"name\":\"ext\",\"kind\":\"http\",\"http\":{\"method\":\"PUT\"}}");
  Workspace ws = *Workspace::open(t.path);
  auto find = [](const CollectionNode& n, const std::string& name) {
    for (const auto& c : n.children)
      if (c.name == name) return c;
    return CollectionNode{};
  };
  root = ws.tree(0);
  CHECK_FALSE(find(root, "ext").badge);
  CHECK(find(root, "r3").badge);
  root = ws.tree(-1);
  CHECK(find(root, "ext").badge);
  CHECK(find(root, "ext").method == "PUT");  // compact JSON: read by the full-parse fallback
}

TEST_CASE("fresh clone: environments/ is not a folder, CRLF checkouts stay clean, no cold reads", "[tree][git]") {
  TempDir t;
  const fs::path origin = t.path / "origin", clone = t.path / "clone";
  {
    Workspace ws = Workspace::create(origin);
    ws.environments().push_back(Environment{"dev", {{"host", "localhost"}}, {}});
    REQUIRE(ws.save_settings());
    SavedRequest r = sample_http();
    r.file = origin / "api" / "get.req.json";
    REQUIRE(ws.save_request(r));
  }
  CHECK(slurp(origin / ".gitattributes") == "*.json text eol=lf\n");
  REQUIRE(fs::exists(origin / "environments"));
  // A Git for Windows clone (core.autocrlf=true): CRLF files and no .watt/ (gitignored).
  fs::copy(origin, clone, fs::copy_options::recursive);
  fs::remove_all(clone / ".watt");
  const fs::path req = clone / "api" / "get.req.json";
  std::string text = slurp(req), crlf;
  for (char c : text) {
    if (c == '\n') crlf += '\r';
    crlf += c;
  }
  spit(req, crlf);
  const auto mtime = fs::last_write_time(req);
  Workspace ws = *Workspace::open(clone);
  CollectionNode root = ws.tree();
  REQUIRE(root.children.size() == 1);  // "api" only, no "environments"
  CHECK(root.children[0].name == "api");
  REQUIRE(root.children[0].children.size() == 1);
  CHECK_FALSE(root.children[0].children[0].badge);  // no index yet: left to tree(-1)
  root = ws.tree(-1);
  CHECK(root.children[0].children[0].badge);
  CHECK(root.children[0].children[0].method == sample_http().http.method);
  // A no-op save (or rename to the same name) keeps the CRLF file as it is.
  auto loaded = ws.load_request(req);
  REQUIRE(loaded);
  REQUIRE(ws.save_request(*loaded));
  CHECK(slurp(req) == crlf);
  CHECK(fs::last_write_time(req) == mtime);
  // A real edit keeps CRLF too, so git sees a one-line change.
  loaded->http.url = "https://changed.test/";
  REQUIRE(ws.save_request(*loaded));
  const std::string after = slurp(req);
  CHECK(after.find("\r\n") != std::string::npos);
  CHECK(std::count(after.begin(), after.end(), '\n') == std::count(after.begin(), after.end(), '\r'));
  CHECK(ws.load_request(req)->http.url == "https://changed.test/");
}

TEST_CASE("rename and remove", "[tree]") {
  TempDir t;
  Workspace ws = Workspace::create(t.path);
  SavedRequest r = sample_http();
  r.file = "folder/old.req.json";
  REQUIRE(ws.save_request(r));
  SavedRequest other;
  other.name = "taken";
  other.file = "folder/taken.req.json";
  REQUIRE(ws.save_request(other));

  REQUIRE(ws.rename(r.file, "New name"));
  const fs::path renamed = ws.root() / "folder" / "New name.req.json";
  CHECK_FALSE(fs::exists(r.file));
  auto back = ws.load_request(renamed);
  REQUIRE(back);
  CHECK(back->name == "New name");
  CHECK(back->id == r.id);

  CHECK_FALSE(ws.rename(renamed, "taken"));  // never overwrites
  CHECK(ws.rename(renamed, "new name"));      // case-only rename works
  CHECK(ws.load_request(ws.root() / "folder" / "new name.req.json"));

  REQUIRE(ws.rename(ws.root() / "folder", "api"));
  CHECK(fs::is_directory(ws.root() / "api"));
  CHECK(fs::exists(ws.root() / "api" / "taken.req.json"));

  CHECK_FALSE(ws.remove(ws.root()));                 // refuses the root
  CHECK_FALSE(ws.remove(t.path / ".." / "outside"));  // refuses outside paths
  CHECK(ws.remove(ws.root() / "api" / "taken.req.json"));
  CHECK_FALSE(fs::exists(ws.root() / "api" / "taken.req.json"));
  CHECK(ws.remove("api"));
  CHECK_FALSE(fs::exists(ws.root() / "api"));
  CHECK_FALSE(ws.remove("api"));
}

// ==================================================================== security

TEST_CASE("secrets at rest: sealed with DPAPI, legacy plaintext resealed, no churn", "[workspace][env][security]") {
  TempDir t;
  const fs::path root = t.path / "ws";
  Workspace ws = Workspace::create(root);
  ws.environments() = {Environment{"dev", {{"base", "http://x"}, {"token", "s3cr\xC3\xA9t \"q\""}}, {"token"}}};
  REQUIRE(ws.save_settings());
  const fs::path sec = root / ".watt" / "secrets.json";
  const std::string sealed = slurp(sec);
#ifdef _WIN32
  CHECK_THAT(sealed, !ContainsSubstring("s3cr"));
  CHECK_THAT(sealed, ContainsSubstring("\"token\": \"dpapi1:"));
#else
  CHECK_THAT(sealed, ContainsSubstring("s3cr"));  // POSIX: plaintext, protected by 0600 / 0700
#endif
  auto ws2 = Workspace::open(root);
  REQUIRE(ws2);
  CHECK(ws2->environments()[0].vars.at("token") == "s3cr\xC3\xA9t \"q\"");
  CHECK(ws2->environments()[0].unreadable_secrets.empty());
  // A no-op save keeps the same ciphertext (DPAPI output is randomized): the file is untouched.
  REQUIRE(ws2->save_settings());
  CHECK(slurp(sec) == sealed);
  REQUIRE(ws.save_settings());  // also from the instance that wrote it
  CHECK(slurp(sec) == sealed);
  // A changed value is sealed anew.
  ws2->environments()[0].vars["token"] = "rotated";
  REQUIRE(ws2->save_settings());
  CHECK(slurp(sec) != sealed);
  CHECK(Workspace::open(root)->environments()[0].vars.at("token") == "rotated");

  // Legacy plaintext secrets.json (written before sealing): read as-is, sealed on the next save.
  spit(sec, "{\n  \"dev\": {\n    \"token\": \"old-plain\"\n  }\n}\n");
  auto ws3 = Workspace::open(root);
  REQUIRE(ws3);
  CHECK(ws3->environments()[0].vars.at("token") == "old-plain");
  CHECK(ws3->expand("{{token}}") == "{{token}}");  // no active env yet
  ws3->active_env = "dev";
  CHECK(ws3->expand("{{token}}") == "old-plain");
  REQUIRE(ws3->save_settings());
#ifdef _WIN32
  CHECK_THAT(slurp(sec), !ContainsSubstring("old-plain"));
  CHECK_THAT(slurp(sec), ContainsSubstring("dpapi1:"));
#endif
  CHECK(Workspace::open(root)->environments()[0].vars.at("token") == "old-plain");
}

TEST_CASE("secrets at rest: a value that cannot be decrypted is kept, flagged and not wiped", "[workspace][env][security]") {
  TempDir t;
  const fs::path root = t.path / "ws";
  Workspace ws = Workspace::create(root);
  ws.environments() = {Environment{"dev", {{"token", "mine"}, {"key", "k"}}, {"token", "key"}}};
  ws.active_env = "dev";
  REQUIRE(ws.save_settings());
  const fs::path sec = root / ".watt" / "secrets.json";
  // As if sealed by another Windows user or machine (or garbage after the prefix).
#ifdef _WIN32
  const std::string foreign = foreign_sealed("theirs");
#else
  const std::string foreign = "dpapi1:AQAAANCMnd8BFdERjHoAwE/Cl+sBAAAA";  // a Windows file opened on POSIX
#endif
  const std::string garbage = "dpapi1:!!not base64!!";
  spit(sec, "{\"dev\": {\"token\": \"" + foreign + "\", \"key\": \"" + garbage + "\"}}");
  std::string err;
  auto ws2 = Workspace::open(root, &err);
  REQUIRE(ws2);  // no crash, no failure
  Environment& dev = ws2->environments()[0];
  CHECK_FALSE(dev.vars.count("token"));
  CHECK_FALSE(dev.vars.count("key"));
  CHECK(dev.unreadable_secrets == std::map<std::string, std::string>{{"token", foreign}, {"key", garbage}});
  CHECK(ws2->expand("Bearer {{token}}") == "Bearer {{token}}");  // left unresolved
  // Saving (e.g. after switching environments) writes them back untouched.
  ws2->active_env = "";
  REQUIRE(ws2->save_settings());
  CHECK_THAT(slurp(sec), ContainsSubstring(foreign));
  CHECK_THAT(slurp(sec), ContainsSubstring("!!not base64!!"));
  auto ws3 = Workspace::open(root);
  CHECK(ws3->environments()[0].unreadable_secrets.size() == 2);
  // Entering a new value replaces the unreadable one.
  ws3->environments()[0].vars["token"] = "fresh";
  REQUIRE(ws3->save_settings());
  CHECK_THAT(slurp(sec), !ContainsSubstring(foreign));
  auto ws4 = Workspace::open(root);
  CHECK(ws4->environments()[0].vars.at("token") == "fresh");
  CHECK(ws4->environments()[0].unreadable_secrets == std::map<std::string, std::string>{{"key", garbage}});
}

TEST_CASE("uploads: only files inside the workspace, unless the user allowed them", "[workspace][form][security]") {
  TempDir t;
  TempDir cfg;
  ConfigDirOverride use_cfg(cfg.path / "watt");
  const fs::path root = t.path / "ws";
  Workspace ws = Workspace::create(root);
  spit(root / "files" / "in.txt", "inside");
  spit(t.path / "outside" / "id_rsa", "PRIVATE KEY");
  spit(root / ".env", "TOKEN=x");
  spit(root / ".watt" / "secrets.json", "{}");
  const fs::path secret = t.path / "outside" / "id_rsa";

  SavedRequest r;
  r.http.method = "POST";
  r.http.url = "https://h.test/";
  r.form = {FormPart{"ok", "files/in.txt", FormPart::Kind::File}, FormPart{"key", secret.string(), FormPart::Kind::File}};
  std::string err;
  CHECK_FALSE(ws.http_to_send(r, &err));  // the request is not built, so it cannot be sent
  CHECK_THAT(err, ContainsSubstring("form part 'key'"));
  CHECK_THAT(err, ContainsSubstring("id_rsa"));
  CHECK_THAT(err, ContainsSubstring("outside the workspace"));
  auto blocked = ws.blocked_files(r);
  REQUIRE(blocked.size() == 1);
  CHECK(blocked[0].filename() == "id_rsa");
  CHECK(blocked[0].is_absolute());
  CHECK_FALSE(ws.is_file_allowed(secret));
  CHECK(ws.is_file_allowed("files/in.txt"));

  // Relative escapes, {{vars}}, FileContent parts and body_file are checked the same way.
  for (const std::string v : {"../outside/id_rsa", "files/../../outside/id_rsa", "{{p}}"}) {
    SavedRequest q = r;
    q.form = {FormPart{"k", v, FormPart::Kind::FileContent}};
    ws.globals["p"] = secret.string();
    err.clear();
    CHECK_FALSE(ws.http_to_send(q, &err));
    CHECK_THAT(err, ContainsSubstring("id_rsa"));
  }
  SavedRequest b = r;
  b.form.clear();
  b.body_file = "../outside/id_rsa";
  err.clear();
  CHECK_FALSE(ws.http_to_send(b, &err));
  CHECK_THAT(err, ContainsSubstring("body file"));
  CHECK_THAT(err, ContainsSubstring("id_rsa"));
  CHECK(ws.blocked_files(b).size() == 1);
  // The workspace's own personal files are not uploaded either.
  for (const std::string v : {".env", ".watt/secrets.json", "./.ENV"}) {
    SavedRequest q = r;
    q.form = {FormPart{"k", v, FormPart::Kind::File}};
    CHECK_FALSE(ws.http_to_send(q));
    CHECK(ws.blocked_files(q).size() == 1);
  }

  // The user allows it: stored per user (not in the workspace), then sent.
  REQUIRE(ws.allow_file(secret, &err));
  CHECK(ws.is_file_allowed(secret));
  CHECK(ws.blocked_files(r).empty());
  auto h = ws.http_to_send(r, &err);
  REQUIRE(h);
  CHECK_THAT(h->body, ContainsSubstring("filename=\"id_rsa\""));
  CHECK_THAT(h->body, ContainsSubstring("PRIVATE KEY"));
  CHECK_THAT(h->body, ContainsSubstring("filename=\"in.txt\""));
  REQUIRE(ws.allowed_files().size() == 1);
  CHECK(ws.allowed_files()[0].filename() == "id_rsa");
  const std::string list = slurp(cfg.path / "watt" / "allowed_files.json");
  CHECK_THAT(list, ContainsSubstring("\"workspaces\""));
  CHECK_THAT(list, ContainsSubstring("id_rsa"));
  for (auto& e : fs::recursive_directory_iterator(root))
    CHECK_THAT(e.path().filename().string(), !ContainsSubstring("allowed_files"));
  // body_file is covered by the same entry; another workspace is not.
  CHECK(ws.http_to_send(b));
  Workspace other = Workspace::create(t.path / "other");
  CHECK_FALSE(other.is_file_allowed(secret));
  CHECK(ws.allow_file("files/in.txt"));  // inside: nothing to store
  CHECK(ws.allowed_files().size() == 1);
  REQUIRE(ws.disallow_file(secret));
  CHECK(ws.allowed_files().empty());
  CHECK_FALSE(ws.http_to_send(r));
}

TEST_CASE("uploads: a planted <ws>/.watt/allowed_files.json allows nothing", "[workspace][form][security]") {
  TempDir t;
  TempDir cfg;
  ConfigDirOverride use_cfg(cfg.path / "watt");
  const fs::path root = t.path / "ws";
  Workspace::create(root);
  const fs::path secret = t.path / "outside" / "id_rsa";
  spit(secret, "PRIVATE KEY");
  // What a malicious repo could commit, in the old in-workspace format and in the per-user one.
  std::string gen = secret.generic_string();
  std::string key = fs::weakly_canonical(root).generic_string();
  spit(root / ".watt" / "allowed_files.json",
       "{\"version\": 1, \"files\": [\"" + gen + "\"], \"workspaces\": {\"" + key + "\": [\"" + gen + "\"]}}");
  auto ws = Workspace::open(root);
  REQUIRE(ws);
  SavedRequest r;
  r.http.url = "https://h.test/";
  r.form = {FormPart{"key", secret.string(), FormPart::Kind::File}};
  CHECK_FALSE(ws->is_file_allowed(secret));
  CHECK(ws->allowed_files().empty());
  CHECK(ws->blocked_files(r).size() == 1);
  std::string err;
  CHECK_FALSE(ws->http_to_send(r, &err));
  CHECK_THAT(err, ContainsSubstring("id_rsa"));
}

TEST_CASE("uploads: relative paths never fall back to the cwd; symlinks out are resolved", "[workspace][form][security]") {
  TempDir t;
  const fs::path root = t.path / "ws";
  Workspace ws = Workspace::create(root);
  spit(t.path / "cwd" / "only-in-cwd.txt", "cwd file");
  const fs::path prev = fs::current_path();
  fs::current_path(t.path / "cwd");
  SavedRequest r;
  r.http.url = "https://h.test/";
  r.form = {FormPart{"f", "only-in-cwd.txt", FormPart::Kind::File}};
  std::string err;
  const bool sent = ws.http_to_send(r, &err).has_value();
  SavedRequest b;
  b.http.url = "https://h.test/";
  b.body_file = "only-in-cwd.txt";
  std::string berr;
  const bool bsent = ws.http_to_send(b, &berr).has_value();
  fs::current_path(prev);
  CHECK_FALSE(sent);
  CHECK_THAT(err, ContainsSubstring("only-in-cwd.txt"));
  CHECK_FALSE(bsent);
  CHECK_THAT(berr, ContainsSubstring("only-in-cwd.txt"));

  // A link inside the workspace that points outside is judged by its target.
  spit(t.path / "outside" / "id_rsa", "PRIVATE KEY");
  std::error_code ec;
  fs::create_symlink(t.path / "outside" / "id_rsa", root / "innocent.txt", ec);
  if (ec) fs::create_directory_symlink(t.path / "outside", root / "linkdir", ec);
  if (ec) {
    WARN("symlinks not available here (no privilege / developer mode): " << ec.message());
    return;
  }
  const std::string via = fs::exists(root / "innocent.txt") ? "innocent.txt" : "linkdir/id_rsa";
  r.form = {FormPart{"f", via, FormPart::Kind::File}};
  err.clear();
  CHECK_FALSE(ws.http_to_send(r, &err));
  CHECK_THAT(err, ContainsSubstring("id_rsa"));
  REQUIRE(ws.blocked_files(r).size() == 1);
  CHECK(ws.blocked_files(r)[0].filename() == "id_rsa");
}

TEST_CASE(".watt is private to the current user", "[workspace][security]") {
  TempDir t;
  TempDir cfg;
  ConfigDirOverride use_cfg(cfg.path / "watt");
  const fs::path root = t.path / "ws";
  Workspace ws = Workspace::create(root);
  ws.environments() = {Environment{"dev", {{"token", "x"}}, {"token"}}};
  ws.active_env = "dev";
  REQUIRE(ws.save_settings());
  HistoryEntry e;
  e.summary = "GET x";
  ws.append_history(e);
  spit(t.path / "out.txt", "o");
  REQUIRE(ws.allow_file(t.path / "out.txt"));
  const fs::path priv = root / ".watt";
  std::vector<fs::path> files = {priv / "secrets.json", priv / "state.json"};
  for (auto& f : fs::recursive_directory_iterator(priv / "history")) files.push_back(f.path());
  REQUIRE(files.size() >= 4);
  const fs::path user_cfg = cfg.path / "watt";
#ifdef _WIN32
  const std::vector<std::string> owner_only = [] {
    std::vector<std::string> v = {current_user_sid(), "S-1-5-18"};  // the user and SYSTEM
    std::sort(v.begin(), v.end());
    return v;
  }();
  for (const fs::path& dir : {priv, user_cfg}) {
    bool prot = false;
    CHECK(dacl_sids(dir, &prot) == owner_only);
    CHECK(prot);  // nothing inherited from the parent (Users, Authenticated Users, ...)
  }
  files.push_back(user_cfg / "allowed_files.json");
  for (const auto& f : files) {
    INFO(f.string());
    CHECK(dacl_sids(f) == owner_only);  // inherited from .watt
  }
  // Tracked files keep the usual inherited permissions.
  CHECK(dacl_sids(root / "watt.json") != owner_only);
#else
  for (const fs::path& dir : {priv, user_cfg}) CHECK((fs::status(dir).permissions() & fs::perms::all) == fs::perms::owner_all);
  files.push_back(user_cfg / "allowed_files.json");
  for (const auto& f : files) {
    INFO(f.string());
    CHECK((fs::status(f).permissions() & fs::perms::all) == (fs::perms::owner_read | fs::perms::owner_write));
  }
#endif
}

// ==================================================================== variables

TEST_CASE("{{var}} expansion", "[vars]") {
  TempDir t;
  Workspace ws = Workspace::create(t.path);
  ws.environments().push_back({"dev",
                               {{"host", "localhost"},
                                {"base", "http://{{host}}:{{port}}"},
                                {"self", "x{{self}}"},
                                {"a", "{{b}}"},
                                {"b", "{{a}}"},
                                {"token", "env-tok"},
                                {"api", "{{base}}/v1"},
                                {"deep", "{{api}}/items"}}});
  ws.globals = {{"port", "8080"}, {"token", "global-tok"}, {"only_global", "g"}};

  SECTION("no active env: globals only") {
    CHECK(ws.expand("{{token}}") == "global-tok");
    CHECK(ws.expand("{{host}}") == "{{host}}");
  }
  ws.active_env = "dev";
  CHECK(ws.expand("") == "");
  CHECK(ws.expand("plain text") == "plain text");
  CHECK(ws.expand("{{token}}") == "env-tok");       // env beats globals
  CHECK(ws.expand("{{only_global}}") == "g");       // then globals
  CHECK(ws.expand("{{ token }}") == "env-tok");     // whitespace inside braces
  CHECK(ws.expand("{{unknown}}/x") == "{{unknown}}/x");
  CHECK(ws.expand("{{base}}/api") == "http://localhost:8080/api");  // nested
  CHECK(ws.expand("{{self}}") == "x{{self}}");      // self reference stays visible: no loop
  CHECK(ws.expand("{{a}}") == "{{a}}");             // cycles stop where they close
  CHECK(ws.expand("{{deep}}") == "http://localhost:8080/v1/items");  // three levels deep
  CHECK(ws.expand("{{host") == "{{host");
  CHECK(ws.expand("}}{{host}}{{") == "}}localhost{{");
  CHECK(ws.expand("{{{host}}}") == "{localhost}");
  CHECK(ws.expand("{{}}") == "{{}}");
  CHECK(ws.expand("{\"a\":{\"b\":1}}") == "{\"a\":{\"b\":1}}");  // JSON is untouched
  CHECK(ws.expand(u8str(u8"é{{host}}é")) == u8str(u8"élocalhosté"));

  const std::string g = ws.expand("{{$guid}}");
  CHECK(g.size() == 36);
  CHECK(g != ws.expand("{{$guid}}"));
  CHECK(std::stoll(ws.expand("{{$timestamp}}")) > 1700000000);
  CHECK_THAT(ws.expand("{{$isoTimestamp}}"), StartsWith("20"));
#ifdef _WIN32
  // The OS environment only for variables the user allowed for this workspace (see [env]).
  CHECK(ws.expand("{{process.env.SystemRoot}}") == "{{process.env.SystemRoot}}");
#endif
  CHECK(ws.expand("{{process.env.WATT_SURELY_UNSET_VAR}}") == "{{process.env.WATT_SURELY_UNSET_VAR}}");
}

TEST_CASE("expanded() applies to every field", "[vars]") {
  TempDir t;
  Workspace ws = Workspace::create(t.path);
  ws.globals = {{"base", "https://api.test"}, {"token", "T"}, {"grpc_host", "10.0.0.1"},
                {"who", "Ada"}, {"protos", "C:/protos"}, {"h", "X-Custom"}};
  SavedRequest r = sample_http();
  r.http.headers.push_back({"{{h}}", "{{who}}"});
  r.http.body = "{\"user\":\"{{who}}\"}";
  SavedRequest e = ws.expanded(r);
  CHECK(e.http.url == "https://api.test/users?x=1&y=\xC3\xA9");
  CHECK(header(e.http, "Authorization") == "Bearer T");
  CHECK(header(e.http, "X-Custom") == "Ada");
  CHECK(e.http.body == "{\"user\":\"Ada\"}");
  CHECK(r.http.url == "{{base}}/users?x=1&y=\xC3\xA9");  // input untouched

  SavedRequest g = ws.expanded(sample_grpc());
  CHECK(g.grpc_target.address == "10.0.0.1:50051");
  CHECK(g.grpc_body == "{\"text\": \"hi Ada\"}");
  CHECK(g.grpc_opts.metadata[0].second == "Bearer T");
  CHECK(g.import_dirs[0] == "C:/protos");
}

// ==================================================================== history

TEST_CASE("history append and read newest first", "[history]") {
  TempDir t;
  Workspace ws = Workspace::create(t.path);
  CHECK(ws.history().empty());
  for (int i = 0; i < 20; ++i) {
    HistoryEntry e;
    e.kind = i % 2 ? RequestKind::Grpc : RequestKind::Http;
    e.summary = "GET https://x/" + std::to_string(i);
    e.status = 200 + i;
    e.total_ms = i + 0.5;
    e.request = i % 2 ? sample_grpc() : sample_http();
    ws.append_history(e);
  }
  auto h = ws.history(5);
  REQUIRE(h.size() == 5);
  CHECK(h[0].summary == "GET https://x/19");
  CHECK(h[4].summary == "GET https://x/15");
  CHECK(h[0].status == 219);
  CHECK(h[0].total_ms == 19.5);
  CHECK(h[0].kind == RequestKind::Grpc);
  // The list carries no request (fast); it is loaded when an entry is opened.
  CHECK(h[0].request.grpc_method.empty());
  auto r0 = ws.history_request(h[0]);
  auto r1 = ws.history_request(h[1]);
  REQUIRE(r0);
  REQUIRE(r1);
  CHECK(r0->grpc_opts.metadata == sample_grpc().grpc_opts.metadata);
  CHECK(r0->grpc_method == sample_grpc().grpc_method);
  CHECK(r1->http.body == sample_http().http.body);
  CHECK(r1->http.headers == sample_http().http.headers);
  CHECK_FALSE(ws.history_request(HistoryEntry{}));
  CHECK_THAT(h[0].when_iso, StartsWith("20"));
  CHECK(h[0].when_iso.back() == 'Z');
  CHECK(ws.history(1000).size() == 20);
  CHECK(ws.history(0).empty());

  // A torn/corrupt line (crash mid-write, hand edit) is skipped, not fatal.
  {
    std::ofstream f(seg_file(t.path, 1), std::ios::binary | std::ios::app);
    f << "{\"when\":\"2026\",\"summ\n";
  }
  ws.append_history({"2030-01-01T00:00:00.000Z", RequestKind::Http, "after", 204, 1, {}});
  h = ws.history(3);
  REQUIRE(h.size() == 3);
  CHECK(h[0].summary == "after");
  CHECK(h[0].when_iso == "2030-01-01T00:00:00.000Z");
  CHECK(h[1].summary == "GET https://x/19");
}

TEST_CASE("history with 10k entries loads fast", "[history][perf]") {
  TempDir t;
  Workspace ws = Workspace::create(t.path);
  // Build a realistic 10k-entry file through the real writer.
  const auto t0 = std::chrono::steady_clock::now();
  SavedRequest r = sample_http();
  r.http.body = std::string(300, 'x');
  for (int i = 0; i < 10000; ++i) {
    HistoryEntry e;
    e.summary = "POST https://api.test/items/" + std::to_string(i);
    e.status = 200;
    e.total_ms = 3.25;
    e.request = r;
    ws.append_history(e);
  }
  const double write_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const auto size = list_bytes(t.path);

  auto t1 = std::chrono::steady_clock::now();
  (void)ws.history(1);  // first open after a burst of writes (AV scan, cache)
  const double first_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
  t1 = std::chrono::steady_clock::now();
  auto recent = ws.history();  // default 500, tail read
  const double recent_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
  REQUIRE(recent.size() == 500);
  CHECK(recent[0].summary == "POST https://api.test/items/9999");
  CHECK(recent[499].summary == "POST https://api.test/items/9500");

  t1 = std::chrono::steady_clock::now();
  auto all = ws.history(100000);
  const double all_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
  REQUIRE(all.size() == 10000);
  CHECK(all.back().summary == "POST https://api.test/items/0");
  const auto t2 = std::chrono::steady_clock::now();
  auto oldest = ws.history_request(all.back());
  const double open_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t2).count();
  REQUIRE(oldest);
  CHECK(oldest->http.headers == r.http.headers);
  CHECK(oldest->http.body == r.http.body);

  std::printf("history: 10k appends %.0f ms, file %.1f MiB, first open %.2f ms, load 500 in %.2f ms, load 10k in %.1f ms, open one %.3f ms\n",
              write_ms, static_cast<double>(size) / (1 << 20), first_ms, recent_ms, all_ms, open_ms);
  CHECK(recent_ms < 50.0);
  CHECK(all_ms < 100.0);
}

TEST_CASE("history is capped; the single-file layout is migrated", "[history]") {
  TempDir t;
  Workspace ws = Workspace::create(t.path);
  SavedRequest r;
  r.http.url = "https://x";
  r.http.body = std::string(200 * 1024, 'b');  // bodies are clipped in history
  ws.append_history({"", RequestKind::Http, "big", 200, 1, r});
  CHECK(ws.history_request(ws.history(1)[0])->http.body.size() == 64 * 1024);
  // The list line stays small: the request lives in the segment's .req.jsonl.
  CHECK(fs::file_size(seg_file(t.path, 1)) < 200);
  CHECK(fs::file_size(seg_file(t.path, 1, true)) < 80 * 1024);

  // An older watt's single-file history past the cap: lines with the request inline, and
  // lines pointing into history.req.jsonl. The first append moves the newest 10000 into
  // segments of 1000 and deletes the old files.
  const fs::path t2 = t.path / "old";
  Workspace::create(t2);
  fs::create_directories(t2 / ".watt");
  {
    std::ofstream f(t2 / ".watt" / "history.jsonl", std::ios::binary | std::ios::trunc);
    std::ofstream fr(t2 / ".watt" / "history.req.jsonl", std::ios::binary | std::ios::trunc);
    uint64_t off = 0;
    for (int i = 0; i < 12000; ++i) {
      if (i % 2) {
        f << "{\"when\":\"x" << i << "\",\"kind\":\"http\",\"summary\":\"old " << i
          << "\",\"status\":1,\"total_ms\":1,\"request\":{\"http\":{\"url\":\"https://old/" << i << "\"}}}\n";
      } else {
        const std::string rec = "{\"when\":\"x" + std::to_string(i) + "\",\"summary\":\"old " + std::to_string(i) +
                                "\",\"request\":{\"http\":{\"url\":\"https://rec/" + std::to_string(i) + "\"}}}";
        fr << rec << "\n";
        f << "{\"when\":\"x" << i << "\",\"kind\":\"http\",\"summary\":\"old " << i
          << "\",\"status\":1,\"total_ms\":1,\"req\":[" << off << "," << rec.size() << "]}\n";
        off += rec.size() + 1;
      }
    }
  }
  Workspace ws2 = *Workspace::open(t2);
  {  // read as it is before any append
    auto before = ws2.history(3);
    REQUIRE(before.size() == 3);
    CHECK(before[0].summary == "old 11999");
    CHECK(ws2.history_request(before[0])->http.url == "https://old/11999");
    CHECK(ws2.history_request(before[1])->http.url == "https://rec/11998");
  }
  ws2.append_history({"", RequestKind::Http, "newest", 200, 1, r});
  CHECK_FALSE(fs::exists(t2 / ".watt" / "history.jsonl"));
  CHECK_FALSE(fs::exists(t2 / ".watt" / "history.req.jsonl"));
  CHECK(segments(t2) == std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
  CHECK(lines_of(seg_file(t2, 1)) == 1000);
  CHECK(lines_of(seg_file(t2, 11)) == 1);
  auto h = ws2.history(100000);
  REQUIRE(h.size() == 10001);
  CHECK(h[0].summary == "newest");
  CHECK(h[1].summary == "old 11999");
  CHECK(h.back().summary == "old 2000");
  CHECK(ws2.history_request(h[0])->http.body.size() == 64 * 1024);
  CHECK(ws2.history_request(h[1])->http.url == "https://old/11999");  // inline request moved to a record
  CHECK(ws2.history_request(h[2])->http.url == "https://rec/11998");  // record copied
  CHECK(ws2.history_request(h.back())->http.url == "https://rec/2000");
  // An entry listed before a trim or a move no longer matches: no wrong request is returned.
  HistoryEntry stale = h[0];
  stale.offset = 0;
  CHECK_FALSE(ws2.history_request(stale));

  // Old segments are never rewritten; once the newer ones hold 10000 entries the oldest is
  // deleted whole.
  const auto seg2_time = fs::last_write_time(seg_file(t2, 2));
  const std::string seg2 = slurp(seg_file(t2, 2));
  r.http.body = "small";
  for (int i = 0; i < 999; ++i) ws2.append_history({"", RequestKind::Http, "more " + std::to_string(i), 200, 1, r});
  CHECK(segments(t2).front() == 1);  // 11: 1000 entries, 2..10: 9000 entries, so 1 is needed
  ws2.append_history({"", RequestKind::Http, "rolls", 200, 1, r});
  CHECK(segments(t2) == std::vector<int>{2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});
  CHECK_FALSE(fs::exists(seg_file(t2, 1, true)));
  CHECK(fs::last_write_time(seg_file(t2, 2)) == seg2_time);
  CHECK(slurp(seg_file(t2, 2)) == seg2);
  h = Workspace::open(t2)->history(100000);
  CHECK(h.size() == 10001);
  CHECK(h[0].summary == "rolls");
  CHECK(h[1].summary == "more 998");
  CHECK(ws2.history_request(h[0])->http.body == "small");
}

TEST_CASE("history segments: listing reads only the newest, entries open across segments", "[history]") {
  TempDir t;
  Workspace ws = Workspace::create(t.path);
  SavedRequest r = sample_http();
  for (int i = 0; i < 2500; ++i) {
    r.http.url = "https://api.test/" + std::to_string(i);
    ws.append_history({"", RequestKind::Http, "GET " + std::to_string(i), 200, 1, r});
  }
  CHECK(segments(t.path) == std::vector<int>{1, 2, 3});
  CHECK(lines_of(seg_file(t.path, 1)) == 1000);
  CHECK(lines_of(seg_file(t.path, 3)) == 500);
  // Another process (fresh Workspace and handles) lists across segment boundaries.
  Workspace other = *Workspace::open(t.path);
  auto h = other.history(1200);
  REQUIRE(h.size() == 1200);
  CHECK(h[0].summary == "GET 2499");
  CHECK(h[499].summary == "GET 2000");
  CHECK(h[500].summary == "GET 1999");
  CHECK(h[1199].summary == "GET 1300");
  CHECK(other.history_request(h[499])->http.url == "https://api.test/2000");
  CHECK(other.history_request(h[500])->http.url == "https://api.test/1999");
  auto all = other.history(100000);
  REQUIRE(all.size() == 2500);
  CHECK(other.history_request(all.back())->http.url == "https://api.test/0");
  // Appends from the other object continue the newest segment.
  other.append_history({"", RequestKind::Http, "from other", 200, 1, r});
  CHECK(lines_of(seg_file(t.path, 3)) == 501);
  CHECK(ws.history(1)[0].summary == "from other");
}

TEST_CASE("history keeps 10k entries with realistic bodies", "[history][perf]") {
  TempDir t;
  Workspace ws = Workspace::create(t.path);
  SavedRequest r = sample_http();
  r.http.body = std::string(5000, 'j');  // 5 KB JSON bodies: the count cap, not bytes, limits
  for (int i = 0; i < 11500; ++i) {
    HistoryEntry e;
    e.summary = "POST https://api.test/items/" + std::to_string(i);
    e.status = 201;
    e.request = r;
    ws.append_history(e);
  }
  const auto t0 = std::chrono::steady_clock::now();
  auto all = Workspace::open(t.path)->history(100000);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  REQUIRE(all.size() >= 10000);
  CHECK(all.size() <= 11000);
  CHECK(all[0].summary == "POST https://api.test/items/11499");
  auto oldest = ws.history_request(all.back());
  REQUIRE(oldest);
  CHECK(oldest->http.body == r.http.body);
  std::printf("history: %zu entries with 5 KB bodies, list file %.1f MiB, listed in %.1f ms\n", all.size(),
              static_cast<double>(list_bytes(t.path)) / (1 << 20), ms);
  CHECK(ms < 100.0);
}

// ==================================================================== curl import

TEST_CASE("curl: Chrome 'Copy as cURL (bash)'", "[curl]") {
  const std::string cmd = R"(curl 'https://api.github.com/graphql' \
  -H 'accept: application/json' \
  -H 'accept-language: en-US,en;q=0.9,de;q=0.8' \
  -H 'content-type: application/json' \
  -H 'cookie: _octo=GH1.1.123; logged_in=yes; tz=Europe%2FBerlin' \
  -H 'origin: https://github.com' \
  -H 'priority: u=1, i' \
  -H 'sec-ch-ua: "Chromium";v="130", "Google Chrome";v="130", "Not?A_Brand";v="99"' \
  -H 'user-agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36' \
  --data-raw '{"query":"query { viewer { login } }","variables":{}}')";
  std::string err;
  auto r = parse_curl_command(cmd, &err);
  REQUIRE(r);
  CHECK(r->method == "POST");
  CHECK(r->url == "https://api.github.com/graphql");
  CHECK(r->body == R"({"query":"query { viewer { login } }","variables":{}})");
  CHECK(r->headers.size() == 8);
  CHECK(header(*r, "sec-ch-ua") == R"("Chromium";v="130", "Google Chrome";v="130", "Not?A_Brand";v="99")");
  CHECK(header(*r, "cookie") == "_octo=GH1.1.123; logged_in=yes; tz=Europe%2FBerlin");
  CHECK(count_header(*r, "Content-Type") == 0);  // user's content-type wins, no default added
  CHECK_FALSE(r->follow_redirects);
  CHECK(r->verify_tls);
}

TEST_CASE("curl: Chrome bash with $'...' quoting and --compressed", "[curl]") {
  const std::string cmd =
      "curl 'https://example.com/api/notes?id=7' \\\n"
      "  -X 'PUT' \\\n"
      "  -H 'accept-encoding: gzip, deflate, br, zstd' \\\n"
      "  -H 'content-type: application/json' \\\n"
      "  --data-raw $'{\"msg\":\"it\\'s a \\\\\"test\\\\\"\\\\nline2\",\"e\":\"\\u00e9\"}' \\\n"
      "  --compressed";
  auto r = parse_curl_command(cmd);
  REQUIRE(r);
  CHECK(r->method == "PUT");
  CHECK(r->body == "{\"msg\":\"it's a \\\"test\\\"\\nline2\",\"e\":\"\xC3\xA9\"}");
  // Kept exactly: it is part of the request the server sees.
  CHECK(header(*r, "accept-encoding") == "gzip, deflate, br, zstd");
}

TEST_CASE("curl: Chrome 'Copy as cURL (cmd)'", "[curl]") {
  const std::string cmd =
      "curl ^\"https://api.example.com/v1/items?q=a&b=2^\" ^\r\n"
      "  -H ^\"accept: application/json, text/plain, */*^\" ^\r\n"
      "  -H ^\"content-type: application/json^\" ^\r\n"
      "  -H ^\"cookie: sid=abc123; theme=dark^\" ^\r\n"
      "  -H ^\"sec-ch-ua: ^\\^\"Chromium^\\^\";v=^\\^\"130^\\^\"^\" ^\r\n"
      "  --data-raw ^\"^{^\\^\"name^\\^\":^\\^\"watt^\\^\",^\\^\"tags^\\^\":^[^\\^\"a^\\^\",^\\^\"b^\\^\"^],"
      "^\\^\"pct^\\^\":^\\^\"100^%^\\^\",^\\^\"path^\\^\":^\\^\"C:^\\^\\^\\^\\tmp^\\^\"^}^\" ^\r\n"
      "  --compressed";
  std::string err;
  auto r = parse_curl_command(cmd, &err);
  INFO(err);
  REQUIRE(r);
  CHECK(r->method == "POST");
  CHECK(r->url == "https://api.example.com/v1/items?q=a&b=2");
  CHECK(header(*r, "accept") == "application/json, text/plain, */*");
  CHECK(header(*r, "cookie") == "sid=abc123; theme=dark");
  CHECK(header(*r, "sec-ch-ua") == "\"Chromium\";v=\"130\"");
  // Chrome turned the two backslashes of the JSON "C:\\tmp" into four; curl.exe (MS CRT rules)
  // keeps all four because they are not before a quote, and so does the default import.
  CHECK(r->body == R"({"name":"watt","tags":["a","b"],"pct":"100%","path":"C:\\\\tmp"})");
  auto imp = parse_curl(cmd);
  REQUIRE(imp);
  CHECK(imp->browser_doubled_backslashes == 2);
  REQUIRE(imp->warnings.size() == 1);
  CHECK_THAT(imp->warnings[0], ContainsSubstring("curl.exe would send 2 more"));
  // The explicit choice: what the browser sent.
  imp = parse_curl(cmd, nullptr, CmdBackslashes::AsBrowserMeant);
  REQUIRE(imp);
  CHECK(imp->http.body == R"({"name":"watt","tags":["a","b"],"pct":"100%","path":"C:\\tmp"})");
  CHECK(header(imp->http, "sec-ch-ua") == "\"Chromium\";v=\"130\"");
  CHECK(imp->browser_doubled_backslashes == 2);
}

TEST_CASE("curl: Chrome cmd with a multi-line body", "[curl]") {
  // Chrome encodes a newline inside a value as ^ + two newlines.
  const std::string cmd = "curl ^\"https://x.test/^\" ^\n  --data-raw ^\"line1^\n\nline2^\"";
  auto r = parse_curl_command(cmd);
  REQUIRE(r);
  CHECK(r->body == "line1\nline2");
  CHECK(r->url == "https://x.test/");
}

TEST_CASE("curl: Firefox 'Copy as cURL' (POSIX)", "[curl]") {
  const std::string cmd =
      "curl 'https://developer.mozilla.org/api/v1/search?q=fetch&locale=en-US' --compressed -X POST "
      "-H 'User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:132.0) Gecko/20100101 Firefox/132.0' "
      "-H 'Accept: */*' -H 'Accept-Language: en-US,en;q=0.5' -H 'Accept-Encoding: gzip, deflate, br, zstd' "
      "-H 'Content-Type: application/x-www-form-urlencoded' -H 'Origin: https://developer.mozilla.org' "
      "-H 'Connection: keep-alive' -H 'Sec-Fetch-Dest: empty' -H 'Sec-Fetch-Mode: cors' "
      "-H 'Sec-Fetch-Site: same-origin' -H 'Priority: u=4' -H 'TE: trailers' --data-raw 'a=1&b=two%20words'";
  auto r = parse_curl_command(cmd);
  REQUIRE(r);
  CHECK(r->method == "POST");
  CHECK(r->url == "https://developer.mozilla.org/api/v1/search?q=fetch&locale=en-US");
  CHECK(r->body == "a=1&b=two%20words");
  CHECK(header(*r, "TE") == "trailers");
  CHECK(r->headers.size() == 12);
  CHECK(header(*r, "Accept-Encoding") == "gzip, deflate, br, zstd");
  CHECK(count_header(*r, "Content-Type") == 1);
}

TEST_CASE("curl: Firefox 'Copy as cURL (Windows)'", "[curl]") {
  // Firefox escapes " as \" and leaves {} and \ unescaped, so cmd.exe quote state toggles inside.
  const std::string cmd =
      "curl ^\"https://example.com/graphql^\" ^\n"
      "  -X POST ^\n"
      "  -H ^\"Content-Type: application/json^\" ^\n"
      "  --data-raw ^\"{\\\"a\\\":\\\"x y\\\",\\\"n\\\":1,\\\"cost\\\":\\\"\\$5\\\"}^\"";
  std::string err;
  auto r = parse_curl_command(cmd, &err);
  INFO(err);
  REQUIRE(r);
  CHECK(r->method == "POST");
  CHECK(r->url == "https://example.com/graphql");
  // \$ is not before a quote, so curl.exe keeps that backslash (MS CRT rules).
  CHECK(r->body == R"({"a":"x y","n":1,"cost":"\$5"})");
}

TEST_CASE("curl: options engineers use", "[curl]") {
  SECTION("-u, -k, -L, combined short flags, attached -X") {
    auto r = parse_curl_command("curl -sSLk -XDELETE -u admin:s3cret https://h.test/items/1 -o /dev/null -w '%{http_code}'");
    REQUIRE(r);
    CHECK(r->method == "DELETE");
    CHECK(r->url == "https://h.test/items/1");
    CHECK(r->follow_redirects);
    CHECK_FALSE(r->verify_tls);
    CHECK(header(*r, "Authorization") == "Basic YWRtaW46czNjcmV0");
  }
  SECTION("-u is structured basic auth: {{vars}} and secrets, built at send time") {
    auto imp = parse_curl("curl -u 'admin:p@ss:word' https://h.test/items");
    REQUIRE(imp);
    CHECK(imp->basic_auth.enabled);
    CHECK(imp->basic_auth.user == "admin");
    CHECK(imp->basic_auth.password == "p@ss:word");
    CHECK(count_header(imp->http, "Authorization") == 0);
    // Export: -u again, in every shell, and parse_curl reads it back.
    for (auto sh : {CurlShell::Bash, CurlShell::Cmd, CurlShell::PowerShell}) {
      const std::string out = to_curl_command(imp->http, imp->form, imp->basic_auth, sh);
      INFO(out);
      CHECK_THAT(out, ContainsSubstring("-u "));
      auto back = parse_curl(out);
      REQUIRE(back);
      CHECK(back->basic_auth == imp->basic_auth);
    }
    // Saved as fields, the password a {{secret}}; the header is built after expansion.
    TempDir t;
    Workspace ws = Workspace::create(t.path);
    ws.environments().push_back(Environment{"dev", {{"pw", "s3cret"}}, {"pw"}});
    ws.active_env = "dev";
    SavedRequest r;
    r.name = "auth";
    r.http = imp->http;
    r.basic_auth = {true, "admin", "{{pw}}"};
    REQUIRE(ws.save_request(r));
    const std::string text = slurp(r.file);
    CHECK_THAT(text, ContainsSubstring("\"auth\": {\n      \"type\": \"basic\",\n      \"user\": \"admin\",\n      \"password\": \"{{pw}}\"\n    }"));
    CHECK(text.find("Basic ") == std::string::npos);
    auto loaded = ws.load_request(r.file);
    REQUIRE(loaded);
    CHECK(loaded->basic_auth == r.basic_auth);
    auto sent = ws.http_to_send(*loaded);
    REQUIRE(sent);
    CHECK(header(*sent, "Authorization") == "Basic YWRtaW46czNjcmV0");
    // Disabled: kept in the file, not sent. The request's own header wins over it.
    loaded->basic_auth.enabled = false;
    REQUIRE(ws.save_request(*loaded));
    CHECK_FALSE(ws.load_request(r.file)->basic_auth.enabled);
    CHECK(count_header(*ws.http_to_send(*loaded), "Authorization") == 0);
    loaded->basic_auth.enabled = true;
    loaded->http.headers.emplace_back("Authorization", "Bearer x");
    CHECK(header(*ws.http_to_send(*loaded), "Authorization") == "Bearer x");
    CHECK(count_header(*ws.http_to_send(*loaded), "Authorization") == 1);
  }
  SECTION("--json sets method and both headers") {
    auto r = parse_curl_command(R"(curl --json '{"a":1}' https://h.test/x)");
    REQUIRE(r);
    CHECK(r->method == "POST");
    CHECK(r->body == R"({"a":1})");
    CHECK(header(*r, "Content-Type") == "application/json");
    CHECK(header(*r, "Accept") == "application/json");
  }
  SECTION("multiple -d are joined, default content type added") {
    auto r = parse_curl_command("curl https://h.test/form -d a=1 --data b=2 --data-binary 'c=3 4'");
    REQUIRE(r);
    CHECK(r->body == "a=1&b=2&c=3 4");
    CHECK(header(*r, "Content-Type") == "application/x-www-form-urlencoded");
  }
  SECTION("-G moves data into the query") {
    auto r = parse_curl_command("curl -G https://h.test/search?x=1 --data-urlencode 'q=hello world' -d n=5");
    REQUIRE(r);
    CHECK(r->method == "GET");
    CHECK(r->url == "https://h.test/search?x=1&q=hello+world&n=5");
    CHECK(r->body.empty());
  }
  SECTION("double quotes, escapes, --url, -H attached, -m, --http2") {
    auto r = parse_curl_command(
        "curl.exe --url \"https://h.test/a b\" -H\"X-A: \\\"q\\\" \\$HOME\" -m 2.5 --http2 -A watt/1 -e https://ref -b 'a=1; b=2'");
    REQUIRE(r);
    CHECK(r->url == "https://h.test/a b");
    CHECK(header(*r, "X-A") == "\"q\" $HOME");
    CHECK(r->timeout_ms == 2500);
    CHECK(r->version == HttpVersion::Http2);
    CHECK(header(*r, "User-Agent") == "watt/1");
    CHECK(header(*r, "Referer") == "https://ref");
    CHECK(header(*r, "Cookie") == "a=1; b=2");
  }
  SECTION("-I, empty header, no scheme, prompt prefix, CRLF continuations") {
    auto r = parse_curl_command("$ curl -I \\\r\n  -H 'X-Empty;' \\\r\n  localhost:8080/health");
    REQUIRE(r);
    CHECK(r->method == "HEAD");
    CHECK(r->url == "http://localhost:8080/health");
    CHECK(r->headers == std::vector<Header>{{"X-Empty", ""}});
  }
  SECTION("-F builds a multipart body") {
    auto r = parse_curl_command("curl -F name=watt -F 'note=a b' https://h.test/upload");
    REQUIRE(r);
    CHECK(r->method == "POST");
    const std::string ct = header(*r, "Content-Type");
    REQUIRE_THAT(ct, StartsWith("multipart/form-data; boundary="));
    const std::string boundary = ct.substr(ct.find('=') + 1);
    CHECK(r->body == "--" + boundary + "\r\nContent-Disposition: form-data; name=\"name\"\r\n\r\nwatt\r\n--" + boundary +
                         "\r\nContent-Disposition: form-data; name=\"note\"\r\n\r\na b\r\n--" + boundary + "--\r\n");
  }
  SECTION("-d @file is not read on import") {
    TempDir t;
    spit(t.path / "body.json", "{\n\"a\": 1\n}\n");
    const std::string p = (t.path / "body.json").generic_string();
    auto r = parse_curl_command("curl https://h.test -d @'" + p + "'");
    REQUIRE(r);
    CHECK(r->method == "POST");
    CHECK(r->body.empty());  // read at send time (Workspace::http_to_send), never on import
    r = parse_curl_command("curl https://h.test --data-binary @'" + p + "'");
    REQUIRE(r);
    CHECK(r->body.empty());
    r = parse_curl_command("curl https://h.test --data-raw @literal");
    REQUIRE(r);
    CHECK(r->body == "@literal");
  }
  SECTION("'Copy all as cURL' takes the first command") {
    auto r = parse_curl_command("curl 'https://a.test/1' -H 'x: 1' ;\ncurl 'https://b.test/2' -X DELETE");
    REQUIRE(r);
    CHECK(r->url == "https://a.test/1");
    CHECK(r->method == "GET");
    r = parse_curl_command("curl ^\"https://a.test/1^\" &\r\ncurl ^\"https://b.test/2^\" -X DELETE");
    REQUIRE(r);
    CHECK(r->url == "https://a.test/1");
    CHECK(r->method == "GET");
  }
  SECTION("bash data that happens to contain ^\" stays bash") {
    auto r = parse_curl_command(R"(curl https://h.test -d '{"re":"^\"x"}')");
    REQUIRE(r);
    CHECK(r->body == R"({"re":"^\"x"})");
  }
  SECTION("errors") {
    std::string err;
    CHECK_FALSE(parse_curl_command("wget https://x", &err));
    CHECK_THAT(err, ContainsSubstring("not a curl command"));
    CHECK_FALSE(parse_curl_command("curl -H 'a: b'", &err));
    CHECK_THAT(err, ContainsSubstring("no URL"));
    CHECK_FALSE(parse_curl_command("curl 'https://x", &err));
    CHECK_THAT(err, ContainsSubstring("unterminated"));
    CHECK_FALSE(parse_curl_command("curl https://x -H", &err));
    CHECK_FALSE(parse_curl_command("   ", &err));
  }
}

// ==================================================================== export

TEST_CASE("to_curl_command round-trips through the parser", "[curl][export]") {
  HttpRequest r;
  r.method = "PATCH";
  r.url = "https://h.test/it's?a=1&b=2";
  r.headers = {{"Content-Type", "application/json"}, {"X-Quote", "say \"hi\" it's $HOME `x`"}, {"X-Empty", ""}};
  r.body = "{\n  \"text\": \"it's \\\"q\\\" \\\\ $(rm -rf /) \xC3\xA9\"\n}";
  r.verify_tls = false;
  r.follow_redirects = true;
  r.timeout_ms = 1500;
  r.version = HttpVersion::Http1_1;
  const std::string cmd = to_curl_command(r);
  INFO(cmd);
  CHECK_THAT(cmd, StartsWith("curl -X PATCH 'https://h.test/it'\\''s?a=1&b=2'"));
  auto back = parse_curl_command(cmd);
  REQUIRE(back);
  CHECK(back->method == r.method);
  CHECK(back->url == r.url);
  CHECK(back->headers == r.headers);
  CHECK(back->body == r.body);
  CHECK(back->verify_tls == r.verify_tls);
  CHECK(back->follow_redirects == r.follow_redirects);
  CHECK(back->timeout_ms == r.timeout_ms);
  CHECK(back->version == r.version);

  HttpRequest g;
  g.url = "https://h.test/";
  g.follow_redirects = false;
  g.timeout_ms = 0;
  CHECK(to_curl_command(g) == "curl https://h.test/");
  HttpRequest p;
  p.method = "POST";
  p.url = "https://h.test/";
  p.body = "x=1";
  p.follow_redirects = false;
  p.timeout_ms = 0;
  CHECK(to_curl_command(p) == "curl https://h.test/ \\\n  --data-raw x=1");
  HttpRequest h;
  h.method = "HEAD";
  h.url = "https://h.test/";
  CHECK_THAT(to_curl_command(h), StartsWith("curl --head https://h.test/"));
}

// Each of these was a request curl sent differently after parse_curl_command + to_curl_command.
TEST_CASE("curl round-trip keeps what curl sends", "[curl][export]") {
  SECTION("-H 'Name:' removes a default header, -H 'Name;' sends an empty one") {
    auto r = parse_curl_command("curl https://h.test -H 'Accept:' -H 'X-Empty;' -H 'X-Ws:   '");
    REQUIRE(r);
    CHECK(header(*r, "Accept") == kRemoveHeader);
    CHECK(header(*r, "X-Empty") == "");
    CHECK(header(*r, "X-Ws") == kRemoveHeader);
    const std::string cmd = to_curl_command(*r);
    CHECK_THAT(cmd, ContainsSubstring("-H Accept:"));
    CHECK_THAT(cmd, ContainsSubstring("-H 'X-Empty;'"));
    auto back = parse_curl_command(cmd);
    REQUIRE(back);
    CHECK(back->headers == r->headers);
  }
  SECTION("URL with [] or {} gets -g") {
    auto r = parse_curl_command("curl -g 'https://h.test/g?f[a]=1&q={a,b}'");
    REQUIRE(r);
    CHECK_THAT(to_curl_command(*r), StartsWith("curl -g 'https://h.test/g?f[a]=1&q={a,b}'"));
    r = parse_curl_command("curl https://h.test/plain");
    CHECK(to_curl_command(*r) == "curl https://h.test/plain");  // and no default --max-time 30
  }
  SECTION("control characters use $'...'") {
    HttpRequest r;
    r.method = "POST";
    r.url = "https://h.test/";
    r.timeout_ms = 0;
    r.follow_redirects = false;
    r.body = "a\r\nb\\c 'd'\x1b\tend";
    const std::string cmd = to_curl_command(r);
    CHECK(cmd == "curl https://h.test/ \\\n  --data-raw $'a\\r\\nb\\\\c \\'d\\'\\x1B\\tend'");
    auto back = parse_curl_command(cmd);
    REQUIRE(back);
    CHECK(back->body == r.body);
    r.body = "multi\nline\tbody";  // LF and TAB alone keep the readable '...' form
    CHECK_THAT(to_curl_command(r), EndsWith("--data-raw 'multi\nline\tbody'"));
  }
  SECTION("--url-query is encoded like --data-urlencode; + means pre-encoded") {
    auto r = parse_curl_command("curl --url-query 'k=v w&z' --url-query '+a=%41 b' --url-query 'plain x' https://h.test/q");
    REQUIRE(r);
    CHECK(r->url == "https://h.test/q?k=v+w%26z&a=%41 b&plain+x");
  }
  SECTION("-d '' keeps an explicit empty body") {
    auto r = parse_curl_command("curl -X DELETE -d '' https://h.test/del");
    REQUIRE(r);
    CHECK(r->body.empty());
    CHECK(header(*r, "Content-Length") == "0");
    const std::string cmd = to_curl_command(*r);
    CHECK_THAT(cmd, ContainsSubstring("--data-raw ''"));
    CHECK_THAT(cmd, !ContainsSubstring("Content-Length"));
    auto back = parse_curl_command(cmd);
    REQUIRE(back);
    CHECK(back->method == "DELETE");
    CHECK(back->headers == r->headers);
    r = parse_curl_command("curl -d '' https://h.test/post");
    CHECK(to_curl_command(*r) ==
          "curl https://h.test/post \\\n  -H 'Content-Type: application/x-www-form-urlencoded' \\\n  --data-raw ''");
  }
  SECTION("--compressed survives as Accept-Encoding + --compressed") {
    auto r = parse_curl_command("curl https://h.test --compressed");
    REQUIRE(r);
    CHECK(header(*r, "Accept-Encoding") == "deflate, gzip");
    CHECK(to_curl_command(*r) == "curl https://h.test \\\n  -H 'Accept-Encoding: deflate, gzip' \\\n  --compressed");
    r = parse_curl_command("curl https://h.test -H 'accept-encoding: br, zstd'");  // kept as given
    CHECK(header(*r, "accept-encoding") == "br, zstd");
    CHECK_THAT(to_curl_command(*r), ContainsSubstring("-H 'accept-encoding: br, zstd'"));
  }
  SECTION("a user-set timeout is exported") {
    auto r = parse_curl_command("curl -m 2.5 https://h.test");
    CHECK(to_curl_command(*r) == "curl https://h.test \\\n  --max-time 2.5");
  }
}

TEST_CASE("curl -F keeps structured parts and file references", "[curl][form]") {
  TempDir t;
  const std::string png_bytes("\x89PNG\r\n\x1a\n\0\0\0\rIHDR", 16);
  spit(t.path / "a.png", png_bytes);
  spit(t.path / "notes.txt", "hello\n");
  const std::string png = (t.path / "a.png").generic_string();
  const std::string notes = (t.path / "notes.txt").generic_string();
  std::string err;
  auto imp = parse_curl("curl -X POST https://h.test/upload -F 'name=John Doe' -F 'avatar=@" + png +
                            "' -F 'meta={\"tags\":[\"x\"]};type=application/json' -F 'doc=@" + notes +
                            ";type=text/plain;filename=renamed.txt' --form-string 'handle=@not_a_file' "
                            "-F 'q=\"semi;colon\";type=text/x-a' -F 'n=<" + notes + "' -F 'a=x;y' -F 'g=@" + notes + "'",
                        &err);
  INFO(err);
  REQUIRE(imp);
  REQUIRE(imp->form.size() == 9);
  const auto& f = imp->form;
  CHECK(imp->http.method == "POST");
  CHECK(imp->http.body.empty());
  CHECK(count_header(imp->http, "Content-Type") == 0);  // added with the boundary at send time
  CHECK(f[0] == FormPart{"name", "John Doe"});
  CHECK(f[1].kind == FormPart::Kind::File);
  CHECK(f[1].value == png);  // a path, not the bytes
  CHECK(f[2].value == "{\"tags\":[\"x\"]}");  // ;type= is a parameter on text parts too
  CHECK(f[2].type == "application/json");
  CHECK(f[3].filename == "renamed.txt");
  CHECK(f[3].type == "text/plain");
  CHECK(f[4].literal);
  CHECK(f[4].value == "@not_a_file");
  CHECK(f[4].kind == FormPart::Kind::Text);
  CHECK(f[5].value == "semi;colon");
  CHECK(f[5].type == "text/x-a");
  CHECK(f[6].kind == FormPart::Kind::FileContent);
  CHECK(f[7].value == "x");  // curl drops the unknown ";y" ...
  REQUIRE(imp->warnings.size() == 1);
  CHECK_THAT(imp->warnings[0], ContainsSubstring("';y'"));  // ... and the import says so

  // curl's content types: from the extension for files, none for text and <file parts.
  CHECK(form_part_content_type(f[0]).empty());
  CHECK(form_part_content_type(f[1]) == "image/png");
  CHECK(form_part_content_type(f[6]).empty());
  CHECK(form_part_content_type(f[8]) == "text/plain");
  CHECK(form_part_content_type(FormPart{"x", "data.json", FormPart::Kind::File}) == "application/octet-stream");
  CHECK(form_part_content_type(FormPart{"x", "C:/p/IMG.JPG", FormPart::Kind::File}) == "image/jpeg");
  CHECK(form_part_content_type(FormPart{"x", "a.png", FormPart::Kind::File, false, "z.bin"}) == "image/png");
  CHECK(form_part_content_type(FormPart{"x", "v", FormPart::Kind::Text, false, "t.txt"}) == "text/plain");

  // The body, as curl_mime builds it.
  HttpRequest h = imp->http;
  REQUIRE(build_form_body(f, h, {}, &err));
  const std::string ct = header(h, "Content-Type");
  REQUIRE_THAT(ct, StartsWith("multipart/form-data; boundary=------------------------"));
  const std::string b = "--" + ct.substr(ct.find('=') + 1);
  CHECK_THAT(h.body, StartsWith(b + "\r\nContent-Disposition: form-data; name=\"name\"\r\n\r\nJohn Doe\r\n" + b +
                                "\r\nContent-Disposition: form-data; name=\"avatar\"; filename=\"a.png\"\r\n"
                                "Content-Type: image/png\r\n\r\n" + png_bytes + "\r\n" + b +
                                "\r\nContent-Disposition: form-data; name=\"meta\"\r\nContent-Type: application/json\r\n\r\n"
                                "{\"tags\":[\"x\"]}\r\n" + b +
                                "\r\nContent-Disposition: form-data; name=\"doc\"; filename=\"renamed.txt\"\r\n"
                                "Content-Type: text/plain\r\n\r\nhello\n\r\n"));
  CHECK_THAT(h.body, ContainsSubstring("name=\"n\"\r\n\r\nhello\n\r\n"));
  CHECK_THAT(h.body, EndsWith(b + "--\r\n"));
  HttpRequest bad = imp->http;
  CHECK_FALSE(build_form_body({FormPart{"f", "no/such/file.bin", FormPart::Kind::File}}, bad, {}, &err));
  CHECK_THAT(err, ContainsSubstring("no/such/file.bin"));

  // Export: one -F / --form-string per part, never the bytes; parses back to the same parts.
  for (CurlShell shell : {CurlShell::Bash, CurlShell::PowerShell, CurlShell::Cmd}) {
    const std::string cmd = to_curl_command(imp->http, imp->form, shell);
    INFO(cmd);
    CHECK_THAT(cmd, !ContainsSubstring("--data"));
    CHECK_THAT(cmd, !ContainsSubstring("-X"));
    CHECK_THAT(cmd, !ContainsSubstring("IHDR"));
    auto back = parse_curl(cmd);
    REQUIRE(back);
    CHECK(back->form == imp->form);
    CHECK(back->http.method == "POST");
  }
  const std::string bash = to_curl_command(imp->http, imp->form);
  CHECK_THAT(bash, ContainsSubstring("-F avatar=@" + png + " "));
  CHECK_THAT(bash, ContainsSubstring("-F 'meta={\"tags\":[\"x\"]};type=application/json'"));
  CHECK_THAT(bash, ContainsSubstring("-F 'doc=@" + notes + ";type=text/plain;filename=renamed.txt'"));
  CHECK_THAT(bash, ContainsSubstring("--form-string handle=@not_a_file"));
  CHECK_THAT(bash, ContainsSubstring("-F 'q=\"semi;colon\";type=text/x-a'"));
  CHECK_THAT(bash, ContainsSubstring("-F 'name=John Doe'"));

  // parse_curl_command builds the multipart body but reads no file: the part goes out empty.
  auto flat = parse_curl_command("curl https://h.test -F 'f=@" + png + "'");
  REQUIRE(flat);
  CHECK_THAT(flat->body, ContainsSubstring("name=\"f\"; filename=\"a.png\""));
  CHECK_THAT(flat->body, !ContainsSubstring(png_bytes));
}

TEST_CASE("form parts are saved as file references and built at send time", "[workspace][form]") {
  TempDir t;
  Workspace ws = Workspace::create(t.path);
  const std::string png_bytes("\x89PNG\0\0binary", 13);
  spit(t.path / "files" / "a.png", png_bytes);
  SavedRequest r;
  r.name = "upload";
  r.http.method = "POST";
  r.http.url = "https://{{host}}/upload";
  r.form = {FormPart{"title", "{{who}}'s pic"},
            FormPart{"pic", (t.path / "files" / "a.png").string(), FormPart::Kind::File},
            FormPart{"off", "x", FormPart::Kind::Text, false, "", "", false}};
  r.form[2].enabled = false;
  ws.globals = {{"host", "h.test"}, {"who", "Ada"}};
  REQUIRE(ws.save_request(r));
  const std::string text = slurp(r.file);
  CHECK_THAT(text, ContainsSubstring("\"file\": \"files/a.png\""));  // relative to the workspace root
  CHECK_THAT(text, !ContainsSubstring("PNG"));
  CHECK_THAT(text, ContainsSubstring("\"enabled\": false"));
  auto back = ws.load_request(r.file);
  REQUIRE(back);
  CHECK(back->form == r.form);
  CHECK(back->form[1].value == "files/a.png");

  auto h = ws.http_to_send(*back);
  REQUIRE(h);
  CHECK(h->url == "https://h.test/upload");
  CHECK_THAT(h->body, ContainsSubstring("name=\"title\"\r\n\r\nAda's pic\r\n"));
  CHECK_THAT(h->body, ContainsSubstring("filename=\"a.png\"\r\nContent-Type: image/png\r\n\r\n" + png_bytes + "\r\n"));
  CHECK_THAT(h->body, !ContainsSubstring("name=\"off\""));
  CHECK_THAT(header(*h, "Content-Type"), StartsWith("multipart/form-data; boundary="));
  CHECK_THAT(to_curl_command(*back), ContainsSubstring("-F pic=@files/a.png"));

  // A user-set multipart type keeps its value and gets the boundary.
  back->http.headers = {{"Content-Type", "multipart/mixed"}};
  h = ws.http_to_send(*back);
  REQUIRE(h);
  CHECK_THAT(header(*h, "Content-Type"), StartsWith("multipart/mixed; boundary="));
  back->form[1].value = "files/missing.png";
  std::string err;
  CHECK_FALSE(ws.http_to_send(*back, &err));
  CHECK_THAT(err, ContainsSubstring("missing.png"));
}

TEST_CASE("curl import matches what curl sends", "[curl]") {
  SECTION("cmd: backslashes follow the MS CRT rules curl.exe uses") {
    // Case 03, Chrome "Copy as cURL (cmd)" of {"path":"C:\\temp"}: curl.exe sends
    // {"path":"C:\\\\temp"} (60 B with httpbin's echo), not what the browser sent.
    const std::string c03 =
        R"(curl ^"https://httpbin.org/anything^" -H ^"content-type: application/json^" )"
        R"(--data-raw ^"^{^\^"path^\^":^\^"C:^\^\^\^\temp^\^"^}^")";
    auto imp = parse_curl(c03);
    REQUIRE(imp);
    CHECK(imp->http.body == R"({"path":"C:\\\\temp"})");
    CHECK(imp->http.body.size() == 21);
    CHECK(imp->browser_doubled_backslashes == 2);
    REQUIRE(imp->warnings.size() == 1);
    CHECK_THAT(imp->warnings[0], ContainsSubstring("doubles backslashes"));
    // Every export sends those same bytes again.
    for (auto sh : {CurlShell::Bash, CurlShell::Cmd, CurlShell::PowerShell}) {
      const std::string out = to_curl_command(imp->http, sh);
      INFO(out);
      auto back = parse_curl(out);
      REQUIRE(back);
      CHECK(back->http.body == imp->http.body);
      CHECK(back->browser_doubled_backslashes == 0);  // watt's own export is not a browser's
    }
    imp = parse_curl(c03, nullptr, CmdBackslashes::AsBrowserMeant);
    REQUIRE(imp);
    CHECK(imp->http.body == R"({"path":"C:\\temp"})");

    // A lone backslash is literal: curl.exe sends a\b C:\temp. Never dropped, in either mode.
    const std::string lone = R"(curl ^"https://httpbin.org/anything^" --data-raw ^"a\b C:\temp^")";
    for (auto bs : {CmdBackslashes::AsCurlExe, CmdBackslashes::AsBrowserMeant}) {
      imp = parse_curl(lone, nullptr, bs);
      REQUIRE(imp);
      CHECK(imp->http.body == R"(a\b C:\temp)");
      CHECK(imp->browser_doubled_backslashes == 0);
      CHECK(imp->warnings.empty());
    }
    // Three backslashes \\\ in a browser run: one pair collapses, the lone one stays.
    imp = parse_curl(R"(curl ^"https://h.test/^" --data-raw ^"x^\^\^\y^")", nullptr, CmdBackslashes::AsBrowserMeant);
    REQUIRE(imp);
    CHECK(imp->http.body == R"(x\\y)");
    CHECK(imp->browser_doubled_backslashes == 1);

    // Before a quote: 2n+1 backslashes give n and a literal quote, 2n give n and toggle quoting.
    auto r = parse_curl_command(R"(curl ^"https://h.test/^" -H "X-A: a\\\"b" -H "X-B: c\\")");
    REQUIRE(r);
    CHECK(header(*r, "X-A") == R"(a\"b)");
    CHECK(header(*r, "X-B") == R"(c\)");
  }
  SECTION("Chrome bash escapes [] {} in the URL; curl drops the backslashes") {
    auto r = parse_curl_command("curl 'https://h.test/items?filter\\[status\\]=open&f=\\{a\\}'");
    REQUIRE(r);
    CHECK(r->url == "https://h.test/items?filter[status]=open&f={a}");
    CHECK_THAT(to_curl_command(*r), StartsWith("curl -g 'https://h.test/items?filter[status]=open&f={a}'"));
    r = parse_curl_command("curl -g 'https://h.test/items?filter\\[status\\]=open'");  // -g: literal
    REQUIRE(r);
    CHECK(r->url == "https://h.test/items?filter\\[status\\]=open");
  }
  SECTION("-b next to -H Cookie: curl sends only the header") {
    auto imp = parse_curl("curl https://h.test -b 'a=1' -b 'b=two words' -H 'Cookie: c=3'");
    REQUIRE(imp);
    CHECK(header(imp->http, "Cookie") == "c=3");
    CHECK(count_header(imp->http, "Cookie") == 1);
    REQUIRE(imp->warnings.size() == 1);
    CHECK_THAT(imp->warnings[0], ContainsSubstring("-b"));
    imp = parse_curl("curl https://h.test -b 'a=1' -b 'b=2'");
    CHECK(header(imp->http, "Cookie") == "a=1; b=2");
    CHECK(imp->warnings.empty());
  }
  SECTION("-G with -d drops --url-query, like curl 8") {
    auto imp = parse_curl("curl -G https://h.test/l -d limit=10 --url-query 'sort=a b'");
    REQUIRE(imp);
    CHECK(imp->http.url == "https://h.test/l?limit=10");
    REQUIRE(imp->warnings.size() == 1);
    CHECK_THAT(imp->warnings[0], ContainsSubstring("--url-query"));
    imp = parse_curl("curl -G https://h.test/l --url-query 'sort=a b'");  // no data: kept
    CHECK(imp->http.url == "https://h.test/l?sort=a+b");
  }
  SECTION("a body with NUL bytes goes through printf in bash") {
    HttpRequest r;
    r.method = "PUT";
    r.url = "https://h.test/bin";
    r.timeout_ms = 0;
    r.follow_redirects = false;
    r.body = std::string("a\0b\\c'd\x01" "7\n", 10);
    const std::string cmd = to_curl_command(r);
    CHECK(cmd == "printf %b 'a\\0000b\\\\c'\\''d\\00017\n' | curl -X PUT https://h.test/bin \\\n  --data-binary @-");
    auto back = parse_curl_command(cmd);
    REQUIRE(back);
    CHECK(back->body == r.body);
    CHECK(back->method == "PUT");
    CHECK(curl_export_problem(r, {}, CurlShell::Bash).empty());
    CHECK(curl_export_problem(r, {}, CurlShell::PowerShell).empty());
    CHECK_THAT(curl_export_problem(r, {}, CurlShell::Cmd), ContainsSubstring("NUL"));
    // PowerShell writes the bytes to a temp file first.
    const std::string ps = to_curl_command(r, CurlShell::PowerShell);
    CHECK_THAT(ps, StartsWith("$wattBody = Join-Path ([IO.Path]::GetTempPath()) 'watt-body.bin'; "
                              "[IO.File]::WriteAllBytes($wattBody, [Convert]::FromBase64String('YQBiXGMnZAE3Cg=='))\n"
                              "curl.exe -X PUT "));
    CHECK_THAT(ps, EndsWith("--data-binary \"@$wattBody\""));
    back = parse_curl_command(ps);
    REQUIRE(back);
    CHECK(back->body == r.body);
  }
  SECTION("cmd.exe export problems are reported") {
    HttpRequest r;
    r.url = "https://h.test/";
    CHECK(curl_export_problem(r, {}, CurlShell::Cmd).empty());
    r.body = "caf\xC3\xA9";
    CHECK_THAT(curl_export_problem(r, {}, CurlShell::Cmd), ContainsSubstring("non-ASCII"));
    CHECK(curl_export_problem(r, {}, CurlShell::PowerShell).empty());
    r.body = "a\r\nb";
    CHECK_THAT(curl_export_problem(r, {}, CurlShell::Cmd), ContainsSubstring("CR"));
    CHECK(curl_export_problem(r, {}, CurlShell::Bash).empty());
    // Every issue is named, not only the first one.
    r.body = "caf\xC3\xA9\r\n";
    const std::string both = curl_export_problem(r, {}, CurlShell::Cmd);
    CHECK_THAT(both, ContainsSubstring("CR"));
    CHECK_THAT(both, ContainsSubstring("non-ASCII"));
    r.body = std::string("\xC3\xA9\0\r", 4);
    const std::string three = curl_export_problem(r, {}, CurlShell::Cmd);
    CHECK_THAT(three, ContainsSubstring("NUL"));
    CHECK_THAT(three, ContainsSubstring("CR"));
    CHECK_THAT(three, ContainsSubstring("non-ASCII"));
  }
}

TEST_CASE("curl: a one-line cmd.exe command is read with cmd's caret escapes", "[curl]") {
  SECTION("an unquoted ^& ^| ^< ^> ^( ^) ^^ ^\" can only be cmd") {
    auto imp = parse_curl("curl https://h.test/a?a=1^&b=2");
    REQUIRE(imp);
    CHECK(imp->http.url == "https://h.test/a?a=1&b=2");
    imp = parse_curl(R"(curl "https://h.test/x" -d a=1^&b=2)");
    REQUIRE(imp);
    CHECK(imp->http.body == "a=1&b=2");
    CHECK(imp->http.method == "POST");
    // The same text with a ^ continuation line was already cmd: both read the same.
    auto cont = parse_curl("curl \"https://h.test/x\" ^\n  -d a=1^&b=2");
    REQUIRE(cont);
    CHECK(cont->http.body == "a=1&b=2");
    imp = parse_curl("curl https://h.test/p?x=^(1^)^|y^<z^>^^");
    REQUIRE(imp);
    CHECK(imp->http.url == "https://h.test/p?x=(1)|y<z>^");
  }
  SECTION("case 13: caret outside quotes removed, inside cmd quotes kept") {
    auto imp = parse_curl(R"(curl -X POST https://httpbin.org/anything/plain?a=1^&b=2 -H "X-Q: \"q\" ^ caret" -d "{\"a\":\"%%\"}")");
    REQUIRE(imp);
    CHECK(imp->http.url == "https://httpbin.org/anything/plain?a=1&b=2");
    CHECK(header(imp->http, "X-Q") == R"("q" ^ caret)");
    CHECK(imp->http.body == R"({"a":"%%"})");
    // All three exports send that again.
    for (auto sh : {CurlShell::Bash, CurlShell::Cmd, CurlShell::PowerShell}) {
      const std::string out = to_curl_command(imp->http, sh);
      INFO(out);
      auto back = parse_curl(out);
      REQUIRE(back);
      CHECK(back->http.url == imp->http.url);
      CHECK(header(back->http, "X-Q") == R"("q" ^ caret)");
      CHECK(back->http.body == imp->http.body);
    }
  }
  SECTION("bash keeps carets that are quoted, escaped or before ordinary characters") {
    auto r = parse_curl_command("curl 'https://h.test/a?a=1^&b=2'");
    REQUIRE(r);
    CHECK(r->url == "https://h.test/a?a=1^&b=2");
    r = parse_curl_command("curl https://h.test/a -d \"x^&y\" -H 'X-A: ^|'");
    REQUIRE(r);
    CHECK(r->body == "x^&y");
    CHECK(header(*r, "X-A") == "^|");
    r = parse_curl_command("curl https://h.test/a?re=^abc -d $'q\\'^&'");
    REQUIRE(r);
    CHECK(r->url == "https://h.test/a?re=^abc");
    CHECK(r->body == "q'^&");
  }
}

TEST_CASE("curl: -e ;auto, -H without a colon, and @file bodies", "[curl]") {
  SECTION("-e 'url;auto' sends url (auto-referer is noted)") {
    auto imp = parse_curl("curl -e 'https://ref.example/;auto' https://h.test/");
    REQUIRE(imp);
    CHECK(header(imp->http, "Referer") == "https://ref.example/");
    REQUIRE(imp->warnings.size() == 1);
    CHECK_THAT(imp->warnings[0], ContainsSubstring(";auto"));
    imp = parse_curl("curl -e ';auto' https://h.test/");
    REQUIRE(imp);
    CHECK(count_header(imp->http, "Referer") == 0);
    imp = parse_curl("curl --referer https://ref.example/x https://h.test/");
    CHECK(header(imp->http, "Referer") == "https://ref.example/x");
    CHECK(imp->warnings.empty());
  }
  SECTION("-H 'NoColon' is not sent by curl: dropped with a warning") {
    auto imp = parse_curl("curl -H 'NoColon' -H 'Empty;' -H 'Spaced ;  ' -H 'X-A: 1' https://h.test/");
    REQUIRE(imp);
    CHECK(count_header(imp->http, "NoColon") == 0);
    CHECK(count_header(imp->http, "Empty") == 1);
    CHECK(header(imp->http, "Empty").empty());
    CHECK(count_header(imp->http, "Spaced") == 1);
    CHECK(header(imp->http, "X-A") == "1");
    REQUIRE(imp->warnings.size() == 1);
    CHECK_THAT(imp->warnings[0], ContainsSubstring("NoColon"));
    for (auto sh : {CurlShell::Bash, CurlShell::Cmd, CurlShell::PowerShell})
      CHECK_THAT(to_curl_command(imp->http, sh), !ContainsSubstring("NoColon"));
  }
  SECTION("--data-binary @file is kept as a file reference; a binary body stays valid JSON") {
    TempDir t;
    Workspace ws = Workspace::create(t.path);
    const fs::path bin = t.path / "body.bin";
    const std::string bytes("\xFF\xFE\x00\x01\x80zz", 7);
    spit(bin, bytes);
    auto imp = parse_curl("curl https://h.test/up --data-binary '@" + bin.string() + "'");
    REQUIRE(imp);
    CHECK(imp->http.method == "POST");
    CHECK(imp->http.body.empty());  // not read on import
    CHECK(imp->body_file == bin.string());
    SavedRequest r;
    r.name = "bin";
    r.http = imp->http;
    r.body_file = imp->body_file;
    REQUIRE(ws.save_request(r));
    CHECK(r.body_file == "body.bin");
    const std::string saved = slurp(r.file);
    CHECK_THAT(saved, ContainsSubstring("\"body_file\": \"body.bin\""));
    CHECK_THAT(saved, !ContainsSubstring("\"body\""));
    CHECK(saved.find('\xFF') == std::string::npos);
    auto loaded = ws.load_request(r.file);
    REQUIRE(loaded);
    auto send = ws.http_to_send(*loaded);
    REQUIRE(send);
    CHECK(send->body == bytes);
    // Exported as --data-binary @file (POST, no file name appended to the URL), in every shell.
    CHECK(to_curl_command(*loaded) ==
          "curl https://h.test/up \\\n  -H 'Content-Type: application/x-www-form-urlencoded' \\\n  --data-binary @body.bin");
    CHECK_THAT(to_curl_command(*loaded, CurlShell::PowerShell), ContainsSubstring("--data-binary '@body.bin'"));
    CHECK_THAT(to_curl_command(*loaded, CurlShell::Cmd), ContainsSubstring("--data-binary @body.bin"));
    // -d @file is kept as a -d reference (curl strips CR/LF when it reads it, at send time).
    spit(t.path / "two.txt", "a=1\r\n&b=2\n");
    const std::string two = (t.path / "two.txt").string();
    imp = parse_curl("curl https://h.test/ -d '@" + two + "'");
    REQUIRE(imp);
    CHECK(imp->body_file.empty());
    CHECK(imp->http.body.empty());
    CHECK(imp->body_data == std::vector<CurlData>{{CurlData::Kind::Data, "@" + two}});
    SavedRequest d;
    d.http = imp->http;
    d.body_data = imp->body_data;
    CHECK(ws.http_to_send(d)->body == "a=1&b=2");
    // Several -d values are joined with '&', when sent.
    imp = parse_curl("curl https://h.test/ -d '@" + two + "' -d c=3");
    CHECK(imp->body_file.empty());
    CHECK(imp->http.body.empty());
    d.body_data = imp->body_data;
    CHECK(ws.http_to_send(d)->body == "a=1&b=2&c=3");
    // -X PUT with a file body to a URL ending in '/': not -T (that would append the file name).
    SavedRequest p;
    p.http.method = "PUT";
    p.http.url = "https://h.test/dir/";
    p.http.follow_redirects = false;
    p.body_file = "x.bin";
    CHECK(to_curl_command(p) == "curl -X PUT https://h.test/dir/ \\\n  --data-binary @x.bin");
    p.http.url = "https://h.test/dir/x.bin";
    CHECK(to_curl_command(p) == "curl https://h.test/dir/x.bin \\\n  -T x.bin");
  }
  SECTION("a body that is not UTF-8 is saved as base64 and loads back byte for byte") {
    TempDir t;
    Workspace ws = Workspace::create(t.path);
    SavedRequest r;
    r.name = "raw";
    r.http.method = "POST";
    r.http.url = "https://h.test/";
    r.http.body = std::string("\xFF\xFE\x00x\xC3", 5);
    REQUIRE(ws.save_request(r));
    const std::string saved = slurp(r.file);
    CHECK_THAT(saved, ContainsSubstring("\"body_base64\": \"//4AeMM=\""));
    CHECK_THAT(saved, !ContainsSubstring("\"headers\""));
    auto loaded = ws.load_request(r.file);
    REQUIRE(loaded);
    CHECK(loaded->http.body == r.http.body);
    // A form request stores no empty headers or body.
    SavedRequest f;
    f.name = "form";
    f.http.method = "POST";
    f.http.url = "https://h.test/";
    FormPart part;
    part.name = "a";
    part.value = "1";
    f.form.push_back(part);
    REQUIRE(ws.save_request(f));
    const std::string ftext = slurp(f.file);
    CHECK_THAT(ftext, !ContainsSubstring("\"headers\""));
    CHECK_THAT(ftext, !ContainsSubstring("\"body\""));
    CHECK(ws.load_request(f.file)->form.size() == 1);
  }
}

TEST_CASE("to_curl_command for cmd.exe and PowerShell parses back", "[curl][export]") {
  HttpRequest r;
  r.method = "PUT";
  r.url = "https://h.test/a?x=100%25&y=%PATH%&z=a^b|c";
  r.headers = {{"Content-Type", "application/json"},
               {"X-Q", "say \"hi\" & <bye> it's $HOME `x` \xE2\x80\x99s"},
               {"Accept", kRemoveHeader},
               {"X-Empty", ""}};
  r.body = "{\n  \"path\": \"C:\\\\Users\\\\me\\\\\",\n  \"pct\": \"50% %OS%\"\n}";
  r.timeout_ms = 1500;
  for (CurlShell shell : {CurlShell::Cmd, CurlShell::PowerShell}) {
    const std::string cmd = to_curl_command(r, shell);
    INFO(cmd);
    auto back = parse_curl_command(cmd);
    REQUIRE(back);
    CHECK(back->method == r.method);
    CHECK(back->url == r.url);
    CHECK(back->headers == r.headers);
    CHECK(back->body == r.body);
    CHECK(back->timeout_ms == r.timeout_ms);
  }
  // Control characters: PowerShell writes $([char]N), which the import evaluates again.
  for (const std::string body : {std::string("a\x01" "b\r\nc\td\x1B"), std::string("--x\r\nk: \"v\"\r\n\x01$e `f\r\n")}) {
    HttpRequest c;
    c.method = "POST";
    c.url = "https://h.test/";
    c.headers = {{"X-C", "v\x01w"}};
    c.body = body;
    const std::string ps = to_curl_command(c, CurlShell::PowerShell);
    INFO(ps);
    CHECK_THAT(ps, ContainsSubstring("$([char]1)"));
    auto back = parse_curl(ps);
    REQUIRE(back);
    CHECK(back->http.body == body);
    CHECK(header(back->http, "X-C") == "v\x01w");
  }
  const std::string cmd = to_curl_command(r, CurlShell::Cmd);
  CHECK_THAT(cmd, ContainsSubstring(" ^\n  -H "));
  CHECK_THAT(cmd, ContainsSubstring("100\"%\"25"));  // % never expands
  const std::string ps = to_curl_command(r, CurlShell::PowerShell);
  CHECK_THAT(ps, StartsWith("$PSNativeCommandArgumentPassing = 'Legacy'\ncurl.exe -X PUT "));
  CHECK_THAT(ps, ContainsSubstring(" `\n  -H "));
  HttpRequest plain;
  plain.url = "https://h.test/";
  plain.headers = {{"Accept", "application/json"}};
  CHECK(to_curl_command(plain, CurlShell::PowerShell) == "curl.exe https://h.test/ `\n  -H 'Accept: application/json' `\n  -L");
  CHECK(to_curl_command(plain, CurlShell::Cmd) == "curl https://h.test/ ^\n  -H \"Accept: application/json\" ^\n  -L");
}

namespace {

bool any_warning(const CurlImport& imp, const std::string& needle) {
  for (const auto& w : imp.warnings)
    if (w.find(needle) != std::string::npos) return true;
  return false;
}

}  // namespace

TEST_CASE("curl: shells expand variables, curl reads files: imported as they would, with a warning", "[curl]") {
  SECTION("-A '' removes User-Agent, like -H 'User-Agent:'") {
    for (const char* cmd : {"curl -A '' https://h.test/", "curl --user-agent '' https://h.test/"}) {
      auto imp = parse_curl(cmd);
      REQUIRE(imp);
      CHECK(header(imp->http, "User-Agent") == kRemoveHeader);
      CHECK_THAT(to_curl_command(imp->http), ContainsSubstring("-H User-Agent:"));
    }
    auto imp = parse_curl("curl -A one -A two https://h.test/");
    CHECK(count_header(imp->http, "User-Agent") == 1);
    CHECK(header(imp->http, "User-Agent") == "two");
    imp = parse_curl("curl -H 'User-Agent: mine' -A other https://h.test/");
    CHECK(header(imp->http, "User-Agent") == "mine");  // a custom header wins, as in curl
  }
  SECTION("bash: $VAR and ${VAR} in double quotes or bare become {{process.env.VAR}}") {
    auto imp = parse_curl(
        "curl -H \"Authorization: Bearer $TOKEN\" -H 'X-Lit: $NOT' -H \"X-Esc: \\$NOT\" https://h.test/${API_PATH}/x");
    REQUIRE(imp);
    CHECK(header(imp->http, "Authorization") == "Bearer {{process.env.TOKEN}}");
    CHECK(header(imp->http, "X-Lit") == "$NOT");
    CHECK(header(imp->http, "X-Esc") == "$NOT");
    CHECK(imp->http.url == "https://h.test/{{process.env.API_PATH}}/x");
    CHECK(any_warning(*imp, "$TOKEN"));
    CHECK(any_warning(*imp, "$API_PATH"));
    CHECK(imp->warnings.size() == 2);
    imp = parse_curl("curl -H \"X-Now: $(date)\" https://h.test/");
    CHECK(any_warning(*imp, "command substitution"));
  }
  SECTION("cmd: a defined %VAR% becomes {{process.env.VAR}}, ^% and undefined ones stay") {
    auto imp = parse_curl("curl ^\n  -H \"X-Path: %PATH%\" ^\n  -H \"X-U: %WATT_SURELY_UNDEFINED_42%\" ^\n  -H X-E:^%PATH^% https://h.test/%20a%");
    REQUIRE(imp);
    CHECK(header(imp->http, "X-Path") == "{{process.env.PATH}}");
    CHECK(header(imp->http, "X-U") == "%WATT_SURELY_UNDEFINED_42%");
    CHECK(header(imp->http, "X-E") == "%PATH%");
    CHECK(imp->http.url == "https://h.test/%20a%");
    CHECK(any_warning(*imp, "%PATH%"));
  }
  SECTION("PowerShell: \"...\" expands $env:X, $var and $null; 5.1's native-argument split is flagged") {
    // Case 14 of the round-trip suite, run in Windows PowerShell 5.1.
    auto imp = parse_curl(
        "curl.exe -X DELETE 'https://httpbin.org/anything/ps?id=7&x=$env:USERNAME' `\n"
        "  -H 'X-Note: it''s $literal `tick' `\n"
        "  -H \"X-Dollar: cost `$5 for $null\" `\n"
        "  -H \"Authorization: Bearer $token\" `\n"
        "  -H \"X-Key: $env:API_KEY\" `\n"
        "  --data-raw '{\\\"a\\\":\\\"b c\\\",\\\"n\\\":1}'");
    REQUIRE(imp);
    CHECK(imp->http.url == "https://httpbin.org/anything/ps?id=7&x=$env:USERNAME");
    CHECK(header(imp->http, "X-Note") == "it's $literal `tick");
    CHECK(header(imp->http, "X-Dollar") == "cost $5 for ");
    CHECK(header(imp->http, "Authorization") == "Bearer {{process.env.token}}");
    CHECK(header(imp->http, "X-Key") == "{{process.env.API_KEY}}");
    CHECK(imp->http.body == "{\"a\":\"b c\",\"n\":1}");
    CHECK(any_warning(*imp, "$null"));
    CHECK(any_warning(*imp, "$token"));
    CHECK(any_warning(*imp, "$env:API_KEY"));
    CHECK(any_warning(*imp, "PowerShell 5.1 passes this argument split at the blank"));
    // A '"' without the \" escape: 5.1 strips the quotes, 7.3+ passes it. Imported as 7.3+.
    imp = parse_curl("curl.exe `\n  --data-raw '{\"a\":1}' https://h.test/");
    CHECK(imp->http.body == "{\"a\":1}");
    CHECK(any_warning(*imp, "quotes stripped"));
  }
  SECTION("a missing @file is kept as a reference, with a warning naming it") {
    auto imp = parse_curl("curl -d @watt-missing.json https://h.test/");
    REQUIRE(imp);
    CHECK(imp->http.body.empty());
    CHECK(header(imp->http, "Content-Length") == "<missing>");  // the body is built when sent
    CHECK(imp->body_data.size() == 1);
    CHECK(any_warning(*imp, "watt-missing.json"));
    imp = parse_curl("curl --data-binary @watt-missing.bin https://h.test/");
    CHECK(imp->http.body.empty());
    CHECK(imp->body_file == "watt-missing.bin");
    CHECK(any_warning(*imp, "watt-missing.bin"));
    imp = parse_curl("curl --data-urlencode 'x@watt-missing.txt' https://h.test/");
    CHECK(imp->http.body.empty());
    CHECK(imp->body_data == std::vector<CurlData>{{CurlData::Kind::UrlEncode, "x@watt-missing.txt"}});
    CHECK(any_warning(*imp, "watt-missing.txt"));
    imp = parse_curl("curl -H @watt-missing-h.txt https://h.test/");
    CHECK(imp->header_files == std::vector<std::string>{"watt-missing-h.txt"});
    CHECK(any_warning(*imp, "watt-missing-h.txt"));
    imp = parse_curl("curl -T watt-missing-up.bin https://h.test/");
    CHECK(any_warning(*imp, "watt-missing-up.bin"));
  }
  SECTION("-b cookie-file sends the jar's cookies for this URL, read when sent") {
    TempDir t;
    Workspace ws = Workspace::create(t.path);
    const fs::path jar = t.path / "cookies.txt";
    spit(jar,
         "# Netscape HTTP Cookie File\n"
         "h.test\tFALSE\t/\tFALSE\t0\tsid\tabc\n"
         ".h.test\tTRUE\t/api\tFALSE\t0\tapi\t1\n"
         "#HttpOnly_h.test\tFALSE\t/\tFALSE\t0\thttponly\tyes\n"
         "h.test\tFALSE\t/\tTRUE\t0\tsecure\tonly-https\n"
         "h.test\tFALSE\t/\tFALSE\t1000\told\texpired\n"
         "other.test\tFALSE\t/\tFALSE\t0\tother\tno\n"
         "h.test\tFALSE\t/admin\tFALSE\t0\tadmin\tno\n"
         "h.test\tFALSE\t/\tFALSE\t0\tempty\t\n");
    auto send = [&](const CurlImport& imp) {
      SavedRequest r;
      r.http = imp.http;
      r.cookie_files = imp.cookie_files;
      auto h = ws.http_to_send(r);
      REQUIRE(h);
      return header(*h, "Cookie");
    };
    auto imp = parse_curl("curl -b '" + jar.string() + "' http://www.h.test/api/v1");
    REQUIRE(imp);
    CHECK(header(imp->http, "Cookie") == "<missing>");  // not read on import
    CHECK(imp->cookie_files == std::vector<std::string>{jar.string()});
    CHECK(send(*imp) == "api=1");  // www.h.test: only the subdomain cookie
    imp = parse_curl("curl -b '" + jar.string() + "' -b 'x=1' https://h.test/api/v1");
    CHECK(header(imp->http, "Cookie") == "x=1");
    CHECK(send(*imp) == "sid=abc; api=1; httponly=yes; secure=only-https; empty=; x=1");
    // A Cookie header: curl drops all -b cookies.
    imp = parse_curl("curl -b '" + jar.string() + "' -H 'Cookie: c=1' https://h.test/api/v1");
    CHECK(imp->cookie_files.empty());
    CHECK(send(*imp) == "c=1");
    imp = parse_curl("curl -b watt-missing-jar.txt https://h.test/");
    CHECK(header(imp->http, "Cookie") == "<missing>");
    CHECK(any_warning(*imp, "watt-missing-jar.txt"));
  }
  SECTION("--digest/--ntlm keep -u disabled instead of sending Basic; dropped options and globs warn") {
    auto imp = parse_curl("curl --digest -u a:b https://h.test/");
    REQUIRE(imp);
    CHECK_FALSE(imp->basic_auth.enabled);
    CHECK(imp->basic_auth.user == "a");
    CHECK(imp->basic_auth.password == "b");
    CHECK(any_warning(*imp, "--digest"));
    imp = parse_curl("curl --ntlm -u a:b https://h.test/");
    CHECK_FALSE(imp->basic_auth.enabled);
    imp = parse_curl("curl -u a:b --basic https://h.test/");
    CHECK(imp->basic_auth.enabled);
    CHECK(imp->warnings.empty());
    imp = parse_curl("curl -x http://proxy:3128 --cacert ca.pem -E c.pem --connect-timeout 3 --retry 2 https://h.test/");
    CHECK(imp->warnings.size() == 5);
    CHECK(any_warning(*imp, "--proxy http://proxy:3128 is not applied"));
    CHECK(any_warning(*imp, "--cacert"));
    CHECK(any_warning(*imp, "--cert c.pem"));
    CHECK(any_warning(*imp, "--connect-timeout"));
    CHECK(any_warning(*imp, "--retry"));
    imp = parse_curl("curl -xhttp://p:1 https://h.test/");
    CHECK(any_warning(*imp, "--proxy http://p:1"));
    imp = parse_curl("curl 'https://h.test/{a,b}/[1-3]'");
    CHECK(any_warning(*imp, "glob {a,b}"));
    CHECK(imp->http.url == "https://h.test/{a,b}/[1-3]");
    CHECK(parse_curl("curl -g 'https://h.test/{a,b}'")->warnings.empty());
    CHECK(parse_curl("curl 'https://h.test/x\\[1-3\\]'")->warnings.empty());
    CHECK(parse_curl("curl 'http://[::1]:8080/x'")->warnings.empty());
  }
  SECTION("-T file: file name appended to a URL ending in '/', PUT, no Content-Type, kept as a reference") {
    TempDir t;
    Workspace ws = Workspace::create(t.path);
    const fs::path up = t.path / "data" / "up.bin";
    spit(up, std::string("x\0yz", 4));
    auto imp = parse_curl("curl -T '" + up.string() + "' https://h.test/dir/");
    REQUIRE(imp);
    CHECK(imp->http.url == "https://h.test/dir/up.bin");
    CHECK(imp->http.method == "PUT");
    CHECK(header(imp->http, "Content-Type") == kRemoveHeader);
    CHECK(imp->http.body.empty());  // not read on import
    CHECK(imp->body_file == up.string());
    CHECK(parse_curl("curl -T '" + up.string() + "' https://h.test")->http.url == "https://h.test/up.bin");
    CHECK(parse_curl("curl -T '" + up.string() + "' https://h.test/named")->http.url == "https://h.test/named");
    // The exported bytes carry no Content-Type either.
    CHECK_THAT(to_curl_command(imp->http), ContainsSubstring("-H Content-Type:"));
    // Saved as a reference (relative inside the workspace), read when sent, exported as -T.
    SavedRequest r;
    r.name = "upload";
    r.http = imp->http;
    r.body_file = imp->body_file;
    REQUIRE(ws.save_request(r));
    CHECK(r.body_file == "data/up.bin");
    const std::string saved = slurp(r.file);
    CHECK_THAT(saved, ContainsSubstring("\"body_file\": \"data/up.bin\""));
    CHECK_THAT(saved, !ContainsSubstring("\"body\""));
    auto loaded = ws.load_request(r.file);
    REQUIRE(loaded);
    CHECK(loaded->body_file == "data/up.bin");
    spit(up, "changed");
    auto send = ws.http_to_send(*loaded);
    REQUIRE(send);
    CHECK(send->body == "changed");
    const std::string cmd = to_curl_command(*loaded);
    CHECK_THAT(cmd, StartsWith("curl https://h.test/dir/up.bin"));
    CHECK_THAT(cmd, ContainsSubstring("-T data/up.bin"));
    CHECK_THAT(cmd, !ContainsSubstring("--data"));
    loaded->body_file = "data/nope.bin";
    std::string err;
    CHECK_FALSE(ws.http_to_send(*loaded, &err));
    CHECK_THAT(err, ContainsSubstring("nope.bin"));
  }
}

namespace {

// What an import keeps, as one saved request (every file reference included).
SavedRequest saved_from(const CurlImport& imp) {
  SavedRequest r;
  r.name = "imported";
  r.http = imp.http;
  r.form = imp.form;
  r.basic_auth = imp.basic_auth;
  r.body_file = imp.body_file;
  r.body_data = imp.body_data;
  r.header_files = imp.header_files;
  r.cookie_files = imp.cookie_files;
  return r;
}

// Every string an imported request carries.
std::string all_text(const SavedRequest& r) {
  std::string s = r.http.method + "\n" + r.http.url + "\n" + r.http.body + "\n" + r.body_file + "\n";
  for (const auto& [k, v] : r.http.headers) s += k + ": " + v + "\n";
  for (const auto& p : r.form) s += p.name + "=" + p.value + ";" + p.filename + ";" + p.type + "\n";
  for (const auto& d : r.body_data) s += d.value + "\n";
  for (const auto& f : r.header_files) s += f + "\n";
  for (const auto& f : r.cookie_files) s += f + "\n";
  return s + r.basic_auth.user + ":" + r.basic_auth.password;
}

}  // namespace

TEST_CASE("curl import never reads a local file", "[curl][security]") {
  TempDir t;
  Workspace ws = Workspace::create(t.path / "ws");
  const std::string secret = "SENTINEL-6d1f-PRIVATE-KEY";
  const fs::path sentinel = t.path / "outside" / "id_rsa";
  // Content that would be valid for every option (a header line, a cookie jar line, a body).
  spit(sentinel, "X-Secret: " + secret + "\nh.test\tFALSE\t/\tFALSE\t0\tk\t" + secret + "\n");
  const fs::path missing = t.path / "nope" / "missing.txt";
  for (const fs::path& file : {sentinel, missing}) {
    const std::string p = file.generic_string();
    const std::vector<std::string> cmds = {
        "curl https://h.test/ -d '@" + p + "'",
        "curl https://h.test/ --data-ascii '@" + p + "'",
        "curl https://h.test/ --data-binary '@" + p + "'",
        "curl https://h.test/ --json '@" + p + "'",
        "curl https://h.test/ --data-urlencode 'n@" + p + "'",
        "curl https://h.test/ --data-urlencode '@" + p + "'",
        "curl https://h.test/ -d a=1 --data-binary '@" + p + "' --data-raw z",
        "curl https://h.test/ -H '@" + p + "'",
        "curl https://h.test/ '-H@" + p + "'",
        "curl https://h.test/ -b '" + p + "'",
        "curl https://h.test/ -b '" + p + "' -b x=1",
        "curl https://h.test/up -T '" + p + "'",
        "curl https://h.test/ -F 'f=@" + p + "'",
        "curl https://h.test/ -F 'f=<" + p + "'",
        "curl https://h.test/ --url-query 'n@" + p + "'",
        "curl https://h.test/ -G -d '@" + p + "'",
        "curl https://h.test/ -G --data-urlencode 'n@" + p + "'",
    };
    for (const auto& cmd : cmds) {
      INFO(cmd);
      std::string err;
      auto imp = parse_curl(cmd, &err);
      INFO(err);
      REQUIRE(imp);
      SavedRequest r = saved_from(*imp);
      CHECK_THAT(all_text(r), !ContainsSubstring(secret));
      // Every file the command names is either kept as a reference or refused, by name.
      if (cmd.find(" -F ") == std::string::npos) CHECK(any_warning(*imp, p));
      auto flat = parse_curl_command(cmd, &err);
      REQUIRE(flat);
      SavedRequest f;
      f.http = *flat;
      CHECK_THAT(all_text(f), !ContainsSubstring(secret));
      // Nor does the saved request (or its export) hold it.
      r.file.clear();
      REQUIRE(ws.save_request(r, &err));
      CHECK_THAT(slurp(r.file), !ContainsSubstring(secret));
      for (auto sh : {CurlShell::Bash, CurlShell::Cmd, CurlShell::PowerShell})
        CHECK_THAT(to_curl_command(r, sh), !ContainsSubstring(secret));
      fs::remove(r.file);
    }
  }
  // The references are kept as written.
  const std::string p = sentinel.generic_string();
  auto imp = parse_curl("curl https://h.test/ -d '@" + p + "' --data-urlencode 'n@" + p + "' -H '@" + p + "' -b '" + p + "'");
  REQUIRE(imp);
  CHECK(imp->body_data == std::vector<CurlData>{{CurlData::Kind::Data, "@" + p}, {CurlData::Kind::UrlEncode, "n@" + p}});
  CHECK(imp->header_files == std::vector<std::string>{p});
  CHECK(imp->cookie_files == std::vector<std::string>{p});
  // Data piped into the command itself is still inlined (it is not a file).
  imp = parse_curl("printf %b 'a\\nb' | curl https://h.test/ --data-binary @- -d @-");
  REQUIRE(imp);
  CHECK(imp->body_data.empty());
  CHECK(imp->body_file.empty());
  CHECK(imp->http.body == "a\nb&ab");
}

TEST_CASE("curl file references are read when sent, as curl reads them", "[curl][workspace]") {
  TempDir t;
  Workspace ws = Workspace::create(t.path);
  spit(t.path / "data" / "crlf.txt", "a=1\r\n&b=2\n");
  const std::string bin("x\0\r\ny", 5);
  spit(t.path / "data" / "bin.bin", bin);
  spit(t.path / "data" / "text.txt", "hello world&=\xC3\xBC\n");
  const std::string enc = "hello+world%26%3D%C3%BC%0A";
  spit(t.path / "data" / "body.json", "{\"k\": 1}\n");
  spit(t.path / "data" / "h.txt", "X-From-File: 1\r\nContent-Type: text/plain\r\n\r\nX-Empty;\n");
  spit(t.path / "data" / "jar.txt", "h.test\tFALSE\t/\tFALSE\t0\ts\t1\nother.test\tFALSE\t/\tFALSE\t0\to\tno\n");

  // Imported, saved, loaded back and sent.
  auto send = [&](const std::string& cmd, SavedRequest* out = nullptr) {
    INFO(cmd);
    std::string err;
    auto imp = parse_curl(cmd, &err);
    REQUIRE(imp);
    SavedRequest r = saved_from(*imp);
    REQUIRE(ws.save_request(r, &err));
    auto loaded = ws.load_request(r.file, &err);
    REQUIRE(loaded);
    auto h = ws.http_to_send(*loaded, &err);
    INFO(err);
    REQUIRE(h);
    if (out) *out = *loaded;
    fs::remove(r.file);
    return *h;
  };
  HttpRequest h = send("curl https://h.test/ -d @data/crlf.txt");
  CHECK(h.method == "POST");
  CHECK(h.body == "a=1&b=2");  // -d strips CR and LF
  CHECK(header(h, "Content-Type") == "application/x-www-form-urlencoded");
  CHECK(send("curl https://h.test/ --data-ascii @data/crlf.txt").body == "a=1&b=2");
  CHECK(send("curl https://h.test/ --data-binary @data/bin.bin").body == bin);
  CHECK(send("curl https://h.test/ --data-binary @data/bin.bin --data-binary @data/bin.bin").body == bin + "&" + bin);
  CHECK(send("curl https://h.test/ --data-urlencode greeting@data/text.txt").body == "greeting=" + enc);
  CHECK(send("curl https://h.test/ --data-urlencode @data/text.txt").body == enc);
  CHECK(send("curl https://h.test/ -d a=1 --data-urlencode n@data/text.txt -d @data/crlf.txt --data-raw @z").body ==
        "a=1&n=" + enc + "&a=1&b=2&@z");
  h = send("curl https://h.test/ --json @data/body.json");
  CHECK(h.body == "{\"k\": 1}\n");
  CHECK(header(h, "Content-Type") == "application/json");
  CHECK(send("curl https://h.test/ --json '[' --json @data/body.json --json ']'").body == "[{\"k\": 1}\n]");
  // -H @file: its headers replace the ones of the same name (curl's custom headers win).
  h = send("curl https://h.test/ -H @data/h.txt -d x=1");
  CHECK(h.body == "x=1");
  CHECK(header(h, "X-From-File") == "1");
  CHECK(header(h, "Content-Type") == "text/plain");
  CHECK(count_header(h, "Content-Type") == 1);
  CHECK(header(h, "X-Empty").empty());
  // -b jar: the cookies for this URL, first, then the -b name=value ones.
  CHECK(header(send("curl https://h.test/ -b data/jar.txt -b y=2"), "Cookie") == "s=1; y=2");
  CHECK(header(send("curl https://h.test/ -b data/jar.txt"), "Cookie") == "s=1");
  CHECK(header(send("curl https://other.test/ -b data/jar.txt"), "Cookie") == "o=no");

  // Absolute paths inside the workspace are saved relative; the file is read on each send.
  SavedRequest saved;
  const std::string abs = (t.path / "data").generic_string();
  send("curl https://h.test/ -d '@" + abs + "/crlf.txt' --data-urlencode 'greeting@" + abs + "/text.txt' -H '@" + abs +
           "/h.txt' -b '" + abs + "/jar.txt'",
       &saved);
  CHECK(saved.body_data == std::vector<CurlData>{{CurlData::Kind::Data, "@data/crlf.txt"},
                                                 {CurlData::Kind::UrlEncode, "greeting@data/text.txt"}});
  CHECK(saved.header_files == std::vector<std::string>{"data/h.txt"});
  CHECK(saved.cookie_files == std::vector<std::string>{"data/jar.txt"});
  spit(t.path / "data" / "crlf.txt", "changed=1\n");
  h = *ws.http_to_send(saved);
  CHECK(h.body == "changed=1&greeting=" + enc);
  CHECK(header(h, "Cookie") == "s=1");
  // A file that is gone fails the send, naming it.
  fs::remove(t.path / "data" / "crlf.txt");
  std::string err;
  CHECK_FALSE(ws.http_to_send(saved, &err));
  CHECK_THAT(err, ContainsSubstring("crlf.txt"));

  // Exported as the arguments they came from, and imported back to the same references.
  const std::string bash = to_curl_command(saved);
  CHECK(bash ==
        "curl https://h.test/ \\\n  -H 'Content-Type: application/x-www-form-urlencoded' \\\n  -H @data/h.txt \\\n"
        "  -b data/jar.txt \\\n  -d @data/crlf.txt \\\n  --data-urlencode greeting@data/text.txt");
  for (auto sh : {CurlShell::Bash, CurlShell::Cmd, CurlShell::PowerShell}) {
    const std::string cmd = to_curl_command(saved, sh);
    INFO(cmd);
    auto back = parse_curl(cmd);
    REQUIRE(back);
    CHECK(back->body_data == saved.body_data);
    CHECK(back->header_files == saved.header_files);
    CHECK(back->cookie_files == saved.cookie_files);
    CHECK(back->http.method == "POST");
    CHECK(back->http.headers == saved.http.headers);
    SavedRequest again = saved_from(*back);
    CHECK(to_curl_command(again, sh) == cmd);
  }
  CHECK_THAT(to_curl_command(saved, CurlShell::PowerShell), ContainsSubstring("-d '@data/crlf.txt'"));
  CHECK_THAT(to_curl_command(saved, CurlShell::PowerShell), ContainsSubstring("-H '@data/h.txt'"));
  SavedRequest js;
  send("curl https://h.test/ --json @data/body.json", &js);
  CHECK_THAT(to_curl_command(js), EndsWith("--json @data/body.json"));
  CHECK(slurp(t.path / "data" / "body.json") == "{\"k\": 1}\n");
}

TEST_CASE("curl file references outside the workspace are blocked until allowed", "[curl][workspace][security]") {
  TempDir t;
  TempDir cfg;
  ConfigDirOverride use_cfg(cfg.path / "watt");
  const fs::path root = t.path / "ws";
  Workspace ws = Workspace::create(root);
  const fs::path secret = t.path / "outside" / "id_rsa";
  spit(secret, "PRIVATEKEY\n");
  spit(root / ".env", "TOKEN=x\n");
  const std::string s = secret.generic_string();
  const std::vector<std::string> cmds = {
      "curl https://h.test/ -d '@" + s + "'",
      "curl https://h.test/ --data-binary '@" + s + "'",
      "curl https://h.test/ -d a=1 --data-binary '@" + s + "'",
      "curl https://h.test/ --json '@" + s + "'",
      "curl https://h.test/ --data-urlencode 'k@" + s + "'",
      "curl https://h.test/ -H '@" + s + "'",
      "curl https://h.test/ -b '" + s + "'",
      "curl https://h.test/up -T '" + s + "'",
      "curl https://h.test/ -d @../outside/id_rsa",
  };
  for (const auto& cmd : cmds) {
    INFO(cmd);
    auto imp = parse_curl(cmd);
    REQUIRE(imp);
    const SavedRequest r = saved_from(*imp);
    auto blocked = ws.blocked_files(r);
    REQUIRE(blocked.size() == 1);
    CHECK(blocked[0].filename() == "id_rsa");
    CHECK(blocked[0].is_absolute());
    std::string err;
    CHECK_FALSE(ws.http_to_send(r, &err));
    CHECK_THAT(err, ContainsSubstring("id_rsa"));
    CHECK_THAT(err, ContainsSubstring("outside the workspace"));
  }
  // The workspace's private files are not sent either.
  for (const std::string cmd : {"curl https://h.test/ -d @.env", "curl https://h.test/ -H @.env",
                                "curl https://h.test/ -b .env", "curl https://h.test/ --data-urlencode k@.env"}) {
    INFO(cmd);
    const SavedRequest r = saved_from(*parse_curl(cmd));
    CHECK(ws.blocked_files(r).size() == 1);
    CHECK_FALSE(ws.http_to_send(r));
  }
  // {{vars}} in a reference are expanded before the check.
  SavedRequest v;
  v.http.method = "POST";
  v.http.url = "https://h.test/";
  v.body_data = {{CurlData::Kind::Data, "@{{key}}"}};
  ws.globals["key"] = s;
  CHECK(ws.blocked_files(v).size() == 1);
  CHECK_FALSE(ws.http_to_send(v));
  // Once the user allows the file, it is sent.
  std::string err;
  REQUIRE(ws.allow_file(secret, &err));
  CHECK(ws.blocked_files(v).empty());
  CHECK(ws.http_to_send(v)->body == "PRIVATEKEY");
  const SavedRequest k = saved_from(*parse_curl("curl https://h.test/ --data-urlencode 'k@" + s + "'"));
  CHECK(ws.blocked_files(k).empty());
  CHECK(ws.http_to_send(k)->body == "k=PRIVATEKEY%0A");
}

TEST_CASE("to_grpcurl_command", "[export]") {
  SavedRequest r;
  r.grpc_target.address = "localhost:50051";
  r.grpc_method = "/watt.test.Echo/Say";
  r.grpc_body = "{\"text\": \"it's\"}";
  r.grpc_opts.deadline_ms = 2500;
  r.grpc_opts.metadata = {{"authorization", "Bearer abc"}};
  CHECK(to_grpcurl_command(r) ==
        "grpcurl -plaintext -max-time 2.5 \\\n"
        "  -H 'authorization: Bearer abc' \\\n"
        "  -d '{\"text\": \"it'\\''s\"}' \\\n"
        "  localhost:50051 watt.test.Echo/Say");

  SavedRequest s = sample_grpc();
  s.grpc_body = "[{\"text\": \"a\"}, {\"text\": \"b\"}]";  // client stream -> one message per line
  const std::string cmd = to_grpcurl_command(s);
  CHECK_THAT(cmd, StartsWith("grpcurl -insecure -authority api.internal -max-time 2.5"));
  CHECK_THAT(cmd, ContainsSubstring("-import-path '{{protos}}' \\\n  -import-path third_party"));
  CHECK_THAT(cmd, ContainsSubstring("-proto echo.proto"));
  CHECK_THAT(cmd, ContainsSubstring("-d '{\"text\":\"a\"}\n{\"text\":\"b\"}'"));
  CHECK_THAT(cmd, ContainsSubstring("'{{grpc_host}}:50051' watt.test.Echo/Say"));

  SavedRequest e;
  e.grpc_target.address = "h:1";
  e.grpc_target.tls = true;
  e.grpc_method = "pkg.S/M";
  e.grpc_opts.deadline_ms = 0;
  CHECK(to_grpcurl_command(e) == "grpcurl \\\n  h:1 pkg.S/M");
}

// Prints generated commands so they can be executed by a real shell (see [.print]).
// Hidden: watt_test_model "[.bench]". 2000 requests in 200 folders, copied like a fresh clone
// (new files, so the antivirus scans each first open, and no .watt/ index).
TEST_CASE("bench: open + tree on a fresh clone", "[.bench]") {
  TempDir t;
  const fs::path origin = t.path / "origin";
  {
    Workspace ws = Workspace::create(origin);
    ws.environments().push_back(Environment{"dev", {{"host", "localhost"}}, {}});
    REQUIRE(ws.save_settings());
    for (int i = 0; i < 2000; ++i) {
      SavedRequest r = sample_http();
      r.name = "req " + std::to_string(i);
      r.id.clear();
      r.file = origin / ("folder" + std::to_string(i / 10)) / ("req" + std::to_string(i) + ".req.json");
      REQUIRE(ws.save_request(r));
    }
  }
  using clock = std::chrono::steady_clock;
  for (int round = 0; round < 3; ++round) {
    const fs::path clone = t.path / ("clone" + std::to_string(round));
    fs::copy(origin, clone, fs::copy_options::recursive);
    fs::remove_all(clone / ".watt");
    for (int pass = 0; pass < 2; ++pass) {
      const auto t0 = clock::now();
      auto ws = Workspace::open(clone);
      REQUIRE(ws);
      const auto t1 = clock::now();
      CollectionNode root = ws->tree();
      const auto t2 = clock::now();
      size_t folders = 0, badges = 0;
      for (const auto& f : root.children) {
        folders += f.is_folder;
        for (const auto& c : f.children) badges += c.badge;
      }
      CHECK(folders == 200);
      std::printf("clone %d pass %d: open %.1f ms, tree %.1f ms, %zu folders, %zu badges\n", round, pass,
                  std::chrono::duration<double, std::milli>(t1 - t0).count(),
                  std::chrono::duration<double, std::milli>(t2 - t1).count(), folders, badges);
      if (pass == 0) {
        const auto t3 = clock::now();
        root = ws->tree(-1);
        std::printf("  tree(-1) worker fill: %.0f ms\n", std::chrono::duration<double, std::milli>(clock::now() - t3).count());
        size_t filled = 0;
        for (const auto& f : root.children)
          for (const auto& c : f.children) filled += c.badge && c.method == "POST";
        CHECK(filled == 2000);
      }
    }
    // Warm files (already scanned by the antivirus), no index: what tree(-1) itself costs.
    fs::remove_all(clone / ".watt");
    auto ws = Workspace::open(clone);
    REQUIRE(ws);
    const auto t0 = clock::now();
    CollectionNode root = ws->tree(-1);
    std::printf("  warm, no index: tree(-1) %.1f ms\n", std::chrono::duration<double, std::milli>(clock::now() - t0).count());
  }
}

TEST_CASE("print export commands", "[.print]") {
  HttpRequest r;
  r.method = "PATCH";
  r.url = "https://h.test/it's?a=1&b=2";
  r.headers = {{"X-Quote", "say \"hi\" it's $HOME `x`"}, {"X-Empty", ""}};
  r.body = "{\n  \"text\": \"it's \\\"q\\\" \\\\ $(echo pwned) \xC3\xA9\"\n}";
  r.verify_tls = false;
  std::printf("%s\n", to_curl_command(r).c_str());
  std::printf("%s\n", to_grpcurl_command(sample_grpc()).c_str());
}

// Manual round-trip check: WATT_CURL_IN=<file with a curl command> writes what each export
// of its import would run next to it (<file>.sh, <file>.cmd.txt, <file>.ps1), to run against
// a real echo server.
TEST_CASE("export a curl command from a file", "[.roundtrip]") {
#ifdef _MSC_VER
  char* env = nullptr;
  size_t env_len = 0;
  if (_dupenv_s(&env, &env_len, "WATT_CURL_IN") != 0 || !env) return;
  const std::string in_path = env;
  std::free(env);
#else
  const char* env = std::getenv("WATT_CURL_IN");
  if (!env) return;
  const std::string in_path = env;
#endif
  const char* in = in_path.c_str();
  std::string err;
  auto imp = parse_curl(slurp(in), &err);
  INFO(err);
  REQUIRE(imp);
  for (const auto& w : imp->warnings) std::printf("warning: %s\n", w.c_str());
  SavedRequest r;
  r.http = imp->http;
  r.form = imp->form;
  r.basic_auth = imp->basic_auth;
  r.body_file = imp->body_file;
  r.body_data = imp->body_data;
  r.header_files = imp->header_files;
  r.cookie_files = imp->cookie_files;
  spit(std::string(in) + ".sh", to_curl_command(r, CurlShell::Bash) + "\n");
  spit(std::string(in) + ".cmd.txt", to_curl_command(r, CurlShell::Cmd) + "\n");
  spit(std::string(in) + ".ps1", to_curl_command(r, CurlShell::PowerShell) + "\n");
}

// Manual timing in a new process: WATT_WS=<workspace> (made with WATT_WS_MAKE=1 first:
// 2000 requests in 20 x 10 folders) prints open + tree(-1) with every badge filled.
TEST_CASE("open + full tree in a new process", "[.treebench]") {
  auto env = [](const char* name) {
    std::string v;
#ifdef _MSC_VER
    char* p = nullptr;
    size_t n = 0;
    if (_dupenv_s(&p, &n, name) == 0 && p) {
      v = p;
      std::free(p);
    }
#else
    if (const char* p = std::getenv(name)) v = p;
#endif
    return v;
  };
  const fs::path root = env("WATT_WS");
  if (root.empty()) return;
  if (!env("WATT_WS_MAKE").empty()) {
    fs::remove_all(root);
    Workspace ws = Workspace::create(root);
    for (int i = 0; i < 2000; ++i) {
      SavedRequest r = sample_http();
      r.name = "req " + std::to_string(i);
      r.id.clear();
      r.file = root / ("top" + std::to_string(i / 100)) / ("sub" + std::to_string(i / 10 % 10)) /
               ("req" + std::to_string(i) + ".req.json");
      REQUIRE(ws.save_request(r));
    }
    return;
  }
  using clock = std::chrono::steady_clock;
  const auto t0 = clock::now();
  auto ws = Workspace::open(root);
  REQUIRE(ws);
  CollectionNode n = ws->tree(-1);
  const double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
  size_t badges = 0;
  auto count = [&](auto& self, const CollectionNode& c) -> void {
    badges += c.badge;
    for (const auto& k : c.children) self(self, k);
  };
  count(count, n);
  std::printf("open + tree(-1): %.1f ms, %zu badges\n", ms, badges);
}
