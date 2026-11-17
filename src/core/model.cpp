#include "core/model.h"

#include <curl/curl.h>  // curl_url: the transport's own URL parser for single sign-on origins

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <random>
#include <system_error>
#include <thread>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/encodedstream.h>
#include <rapidjson/memorystream.h>
#include <rapidjson/reader.h>

#include "core/crypto.h"
#include "core/model_history_file.h"
#include "core/model_json.h"
#include "core/model_prescan.h"
#include "core/model_secure.h"
#include "core/oauth2.h"

// On-disk formats
// ---------------
// <name>.req.json (one request per file, pretty, fixed key order, diff-friendly):
//   { "version": 1, "id": "...", "name": "...", "kind": "http"|"grpc",
//     "http": { method, url, headers?: [[k, v] | [k, v, false] (disabled), ...],
//               body? | body_base64?, timeout_ms,
//               follow_redirects, verify_tls, http_version, fresh_connection, body_file? },
//     "grpc": { address, tls, insecure_skip_verify, authority, root_cert_pem, method, body,
//               deadline_ms, metadata: [[k, v], ...], proto_files, import_dirs } }
//   Only the section matching "kind" is always written; the other one only if it is not all
//   defaults. The HTTP settings (timeout_ms ... fresh_connection) are written only when not
//   HttpRequest's defaults. Multi-line bodies are stored as an array of lines so a one-line
//   edit in a JSON body is a one-line git diff; single-line bodies stay a plain string. Empty
//   headers and body are left out; a body that is not UTF-8 is stored as body_base64; with
//   body_file (curl -T / -d @file) or form parts there is no body at all.
//   Multi-line arrays put the comma first (", [k, v]"), so appending or removing the last
//   header, form part or body line is a one-line diff too.
//   A request whose auth is Inherit has "auth_scheme": {"type": "inherit"}; without auth_scheme
//   it is None (files written before Inherit existed keep their meaning).
// watt.json: { version, globals: {sorted} } plus any unknown top-level keys written by newer
//   versions, which are preserved on save.
// <folder>/folder.json and <ws>/folder.json (the collection): FolderSettings, see folder_value()
//   below: { version, headers?, auth?, vars?, asserts?, pre_script?, post_script? }, written
//   only while something is set. The tree lists only *.req.json, so it is never a request.
// environments/<name>.json: { name, seq, vars: {sorted}, secrets: [names] }; secret values are
//   in .watt/secrets.json { env: { var: value } } (on Windows value = "dpapi1:" + base64 of a
//   DPAPI blob, current user; older plaintext values are read and sealed on the next save; see
//   model_secure.h), the active environment in
//   .watt/state.json { active_env }. Older workspaces kept environments and active_env in
//   watt.json: they are still read and moved on the next save_settings().
//   "http" may carry "form": [{name, value|file|file_content, filename?, type?, literal?,
//   enabled?}]: file parts are path references (relative to the workspace root when inside it).
//   and "auth": {type: "basic", enabled?, user, password} (user and password may be {{vars}}).
//   curl file references, read at send time: "body_data": [[option, value], ...] (option: data,
//   data-binary, data-raw, data-urlencode, json; no body stored then), "header_files": [paths]
//   (curl -H @file), "cookie_files": [paths] (curl -b file).
// After "http" / "grpc", only when not empty (see core/asserts.h):
//   "asserts": [[target, "op value"] | [target, "op value", false] (disabled), ...]
//   "captures": [{var, from: target, scope: environment|globals|runtime, enabled?}, ...]
// .watt/history/<seq>.jsonl: segments of at most 1000 entries, one small compact JSON object
//   per line (when, kind, summary, status, total_ms, req: [offset, length]), oldest first,
//   appended to the newest segment only. The request itself is a line of <seq>.req.jsonl
//   ({when, summary, request}) at that offset. Old segments never change; the oldest are
//   deleted whole once the newer ones hold 10000 entries. history() reads segments from the
//   newest and returns entries newest first. The single-file layout of older versions
//   (.watt/history.jsonl + history.req.jsonl, lines may hold the request inline) is read
//   as it is and moved into segments by the first append.
// .watt/index.json: sidebar badges {files: {relpath: [mtime, size, kind, method]}}, a cache,
//   plus .watt/index.log, its append-only journal (see TreeIndex).

namespace watt {

namespace fs = std::filesystem;
using detail::from_utf8;
using detail::to_utf8;
using rapidjson::SizeType;
using rapidjson::Value;
using Alloc = rapidjson::Document::AllocatorType;

namespace {

constexpr const char* kSettingsFile = "watt.json";
constexpr const char* kReqExt = ".req.json";
constexpr const char* kFolderFile = "folder.json";  // folder / collection settings (not a request)
constexpr uint64_t kFolderFileMax = 4u << 20;        // a larger folder.json is refused
constexpr const char* kEnvDir = "environments";  // at the root: environment files, not requests
// History is kept in segments of 1000 entries (or 8 MiB of records, whichever comes first)
// and capped by entry count: the newest segments that hold 10000 entries are kept (10000 to
// 10999 entries). The byte limit only guards against pathological sizes (64 KiB bodies x 10k).
constexpr size_t kHistoryKeepEntries = 10000;
constexpr size_t kHistorySegEntries = 1000;
constexpr uint64_t kHistorySegMaxBytes = 8ull << 20;
constexpr uint64_t kHistoryKeepBytes = 128ull << 20;
constexpr size_t kHistoryBodyCap = 64u << 10;

// ------------------------------------------------------------------ small utils

bool ends_with(const std::string& s, const char* suffix) {
  const size_t n = std::char_traits<char>::length(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::string lower_ascii(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

std::mt19937_64& rng() {
  thread_local std::mt19937_64 g([] {
    std::random_device rd;
    return (static_cast<uint64_t>(rd()) << 32) ^ rd() ^
           static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
  }());
  return g;
}

std::string new_uuid() {
  uint64_t a = rng()(), b = rng()();
  a = (a & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;  // version 4
  b = (b & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;  // RFC 4122 variant
  char buf[40];
  std::snprintf(buf, sizeof buf, "%08x-%04x-%04x-%04x-%012llx", static_cast<unsigned>(a >> 32),
                static_cast<unsigned>((a >> 16) & 0xFFFF), static_cast<unsigned>(a & 0xFFFF),
                static_cast<unsigned>(b >> 48), static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFull));
  return buf;
}

std::tm utc_tm(std::time_t t) {
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  return tm;
}

std::string now_iso() {
  const auto now = std::chrono::system_clock::now();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
  const std::tm tm = utc_tm(static_cast<std::time_t>(ms / 1000));
  char buf[40];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1,
                tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms % 1000));
  return buf;
}

std::string get_env(const std::string& name) {
#ifdef _WIN32
  char* v = nullptr;
  size_t n = 0;
  if (_dupenv_s(&v, &n, name.c_str()) != 0 || !v) return {};
  std::string s(v);
  std::free(v);
  return s;
#else
  const char* v = std::getenv(name.c_str());
  return v ? v : "";
#endif
}

// File-system-safe name: keeps UTF-8, replaces characters Windows forbids, avoids reserved
// device names and trailing dots/spaces. Never empty.
std::string sanitize_name(const std::string& name) {
  std::string out;
  out.reserve(name.size());
  for (unsigned char c : name) {
    if (c < 0x20 || std::string_view("<>:\"/\\|?*").find(static_cast<char>(c)) != std::string_view::npos)
      out += '_';
    else
      out += static_cast<char>(c);
  }
  while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
  while (!out.empty() && out.front() == ' ') out.erase(out.begin());
  if (out.size() > 120) {  // cut on a UTF-8 boundary
    size_t n = 120;
    while (n > 0 && (static_cast<unsigned char>(out[n]) & 0xC0) == 0x80) --n;
    out.resize(n);
  }
  if (out.empty()) out = "request";
  static const char* reserved[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5",
                                   "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4",
                                   "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
  const std::string low = lower_ascii(out.substr(0, out.find('.')));
  for (const char* r : reserved)
    if (low == r) return "_" + out;
  return out;
}

// ------------------------------------------------------------------ JSON <-> model

Value jstr(const std::string& s, Alloc& a) { return Value(s.c_str(), static_cast<SizeType>(s.size()), a); }

std::string get_str(const Value& o, const char* key, const std::string& dflt = {}) {
  auto it = o.FindMember(key);
  if (it == o.MemberEnd() || !it->value.IsString()) return dflt;
  return std::string(it->value.GetString(), it->value.GetStringLength());
}

bool get_bool(const Value& o, const char* key, bool dflt) {
  auto it = o.FindMember(key);
  return (it != o.MemberEnd() && it->value.IsBool()) ? it->value.GetBool() : dflt;
}

double get_num(const Value& o, const char* key, double dflt) {
  auto it = o.FindMember(key);
  return (it != o.MemberEnd() && it->value.IsNumber()) ? it->value.GetDouble() : dflt;
}

long get_long(const Value& o, const char* key, long dflt) {
  auto it = o.FindMember(key);
  if (it == o.MemberEnd() || !it->value.IsNumber()) return dflt;
  if (it->value.IsInt64()) return static_cast<long>(it->value.GetInt64());
  return static_cast<long>(it->value.GetDouble());
}

const Value* get_obj(const Value& o, const char* key) {
  auto it = o.FindMember(key);
  return (it != o.MemberEnd() && it->value.IsObject()) ? &it->value : nullptr;
}

const Value* get_arr(const Value& o, const char* key) {
  auto it = o.FindMember(key);
  return (it != o.MemberEnd() && it->value.IsArray()) ? &it->value : nullptr;
}

Value pairs_value(const std::vector<std::pair<std::string, std::string>>& v, Alloc& a) {
  Value arr(rapidjson::kArrayType);
  arr.Reserve(static_cast<SizeType>(v.size()), a);
  for (const auto& [k, val] : v) {
    Value p(rapidjson::kArrayType);
    p.PushBack(jstr(k, a), a).PushBack(jstr(val, a), a);
    arr.PushBack(p, a);
  }
  return arr;
}

std::vector<std::pair<std::string, std::string>> pairs_from(const Value& o, const char* key) {
  std::vector<std::pair<std::string, std::string>> out;
  const Value* arr = get_arr(o, key);
  if (!arr) return out;
  for (const auto& e : arr->GetArray()) {
    if (e.IsArray() && e.Size() >= 1 && e[0].IsString()) {
      std::string v = (e.Size() >= 2 && e[1].IsString()) ? std::string(e[1].GetString(), e[1].GetStringLength()) : "";
      out.emplace_back(std::string(e[0].GetString(), e[0].GetStringLength()), std::move(v));
    } else if (e.IsObject()) {  // tolerate {"name": .., "value": ..}
      out.emplace_back(get_str(e, "name"), get_str(e, "value"));
    }
  }
  return out;
}

Value strings_value(const std::vector<std::string>& v, Alloc& a) {
  Value arr(rapidjson::kArrayType);
  for (const auto& s : v) arr.PushBack(jstr(s, a), a);
  return arr;
}

std::vector<std::string> strings_from(const Value& o, const char* key) {
  std::vector<std::string> out;
  if (const Value* arr = get_arr(o, key))
    for (const auto& e : arr->GetArray())
      if (e.IsString()) out.emplace_back(e.GetString(), e.GetStringLength());
  return out;
}

// Multi-line bodies become an array of lines (exactly reversible: joined with '\n'). With CRLF
// (raw multipart, Windows text) each line keeps its "\r", so one edited line is one diff line.
Value body_value(const std::string& body, Alloc& a, bool as_lines) {
  if (!as_lines || body.find('\n') == std::string::npos) return jstr(body, a);
  Value arr(rapidjson::kArrayType);
  size_t start = 0;
  for (;;) {
    const size_t nl = body.find('\n', start);
    if (nl == std::string::npos) {
      arr.PushBack(jstr(body.substr(start), a), a);
      break;
    }
    arr.PushBack(jstr(body.substr(start, nl - start), a), a);
    start = nl + 1;
  }
  return arr;
}

std::string body_from(const Value& o, const char* key) {
  auto it = o.FindMember(key);
  if (it == o.MemberEnd()) return {};
  if (it->value.IsString()) return std::string(it->value.GetString(), it->value.GetStringLength());
  std::string out;
  if (it->value.IsArray()) {
    bool first = true;
    for (const auto& l : it->value.GetArray()) {
      if (!first) out += '\n';
      first = false;
      if (l.IsString()) out.append(l.GetString(), l.GetStringLength());
    }
  }
  return out;
}

const char* version_name(HttpVersion v) {
  switch (v) {
    case HttpVersion::Http1_1: return "http1.1";
    case HttpVersion::Http2: return "http2";
    case HttpVersion::Http2PriorKnowledge: return "http2-prior-knowledge";
    default: return "auto";
  }
}

HttpVersion version_from(const std::string& s) {
  if (s == "http1.1") return HttpVersion::Http1_1;
  if (s == "http2") return HttpVersion::Http2;
  if (s == "http2-prior-knowledge") return HttpVersion::Http2PriorKnowledge;
  return HttpVersion::Auto;
}

// Cut at a character boundary, so a clipped UTF-8 body stays UTF-8.
std::string clip(const std::string& s, size_t cap) {
  if (s.size() <= cap) return s;
  while (cap > 0 && (static_cast<unsigned char>(s[cap]) & 0xC0) == 0x80) --cap;
  return s.substr(0, cap);
}

Value http_section(const HttpRequest& h, Alloc& a, bool lines, size_t body_cap) {
  Value o(rapidjson::kObjectType);
  o.AddMember("method", jstr(h.method, a), a);
  o.AddMember("url", jstr(h.url, a), a);
  // Empty headers and body are left out (like .bru's empty blocks): adding the first header
  // turns "headers": [] into a block either way.
  if (!h.headers.empty()) o.AddMember("headers", pairs_value(h.headers, a), a);
  const std::string body = clip(h.body, body_cap);
  if (!detail::valid_utf8(body)) {
    // A JSON string must hold UTF-8: binary bytes (an image, a protobuf) are kept as base64.
    o.AddMember("body_base64", jstr(detail::base64(body), a), a);
  } else if (!body.empty()) {
    o.AddMember("body", body_value(body, a, lines), a);
  }
  // Settings are written only when they differ from HttpRequest's defaults (what a load fills
  // in), so a typical file is method, url, headers and body, and a change is one added line.
  const HttpRequest def;
  if (h.timeout_ms != def.timeout_ms) o.AddMember("timeout_ms", Value(static_cast<int64_t>(h.timeout_ms)), a);
  if (h.follow_redirects != def.follow_redirects) o.AddMember("follow_redirects", h.follow_redirects, a);
  if (h.verify_tls != def.verify_tls) o.AddMember("verify_tls", h.verify_tls, a);
  if (h.version != def.version) o.AddMember("http_version", Value(rapidjson::StringRef(version_name(h.version))), a);
  if (h.fresh_connection != def.fresh_connection) o.AddMember("fresh_connection", h.fresh_connection, a);
  return o;
}

Value grpc_section(const SavedRequest& r, Alloc& a, bool lines, size_t body_cap) {
  Value o(rapidjson::kObjectType);
  o.AddMember("address", jstr(r.grpc_target.address, a), a);
  o.AddMember("tls", r.grpc_target.tls, a);
  o.AddMember("insecure_skip_verify", r.grpc_target.insecure_skip_verify, a);
  // gRPC-Web: written only when on, so existing request files stay byte-identical.
  if (r.grpc_target.web) o.AddMember("web", true, a);
  if (r.grpc_target.web_text) o.AddMember("web_text", true, a);
  o.AddMember("authority", jstr(r.grpc_target.authority, a), a);
  o.AddMember("root_cert_pem", body_value(r.grpc_target.root_cert_pem, a, lines), a);
  o.AddMember("method", jstr(r.grpc_method, a), a);
  o.AddMember("body", body_value(clip(r.grpc_body, body_cap), a, lines), a);
  o.AddMember("deadline_ms", Value(static_cast<int64_t>(r.grpc_opts.deadline_ms)), a);
  o.AddMember("metadata", pairs_value(r.grpc_opts.metadata, a), a);
  o.AddMember("proto_files", strings_value(r.proto_files, a), a);
  o.AddMember("import_dirs", strings_value(r.import_dirs, a), a);
  return o;
}

const std::string& default_http_json() {
  static const std::string s = [] {
    rapidjson::Document d;
    return detail::json_compact(http_section(HttpRequest{}, d.GetAllocator(), false, SIZE_MAX));
  }();
  return s;
}

const std::string& default_grpc_json() {
  static const std::string s = [] {
    rapidjson::Document d;
    return detail::json_compact(grpc_section(SavedRequest{}, d.GetAllocator(), false, SIZE_MAX));
  }();
  return s;
}

// Form parts: one object each with only the keys that are set. A file part is a path reference
// ("file": path, like Bruno's @file(path)); its bytes are read when the request is sent.
Value form_value(const std::vector<FormPart>& form, Alloc& a) {
  Value arr(rapidjson::kArrayType);
  for (const auto& p : form) {
    Value o(rapidjson::kObjectType);
    o.AddMember("name", jstr(p.name, a), a);
    const char* key = p.kind == FormPart::Kind::File ? "file" : p.kind == FormPart::Kind::FileContent ? "file_content" : "value";
    o.AddMember(rapidjson::StringRef(key), jstr(p.value, a), a);
    if (!p.filename.empty()) o.AddMember("filename", jstr(p.filename, a), a);
    if (!p.type.empty()) o.AddMember("type", jstr(p.type, a), a);
    if (p.literal) o.AddMember("literal", true, a);
    if (!p.enabled) o.AddMember("enabled", false, a);
    arr.PushBack(o, a);
  }
  return arr;
}

std::vector<FormPart> form_from(const Value& o) {
  std::vector<FormPart> out;
  const Value* arr = get_arr(o, "form");
  if (!arr) return out;
  for (const auto& e : arr->GetArray()) {
    if (!e.IsObject()) continue;
    FormPart p;
    p.name = get_str(e, "name");
    if (e.HasMember("file")) {
      p.kind = FormPart::Kind::File;
      p.value = get_str(e, "file");
    } else if (e.HasMember("file_content")) {
      p.kind = FormPart::Kind::FileContent;
      p.value = get_str(e, "file_content");
    } else {
      p.value = get_str(e, "value");
    }
    p.filename = get_str(e, "filename");
    p.type = get_str(e, "type");
    p.literal = get_bool(e, "literal", false);
    p.enabled = get_bool(e, "enabled", true);
    out.push_back(std::move(p));
  }
  return out;
}

constexpr const char* kCurlDataOpts[] = {"data", "data-binary", "data-raw", "data-urlencode", "json"};

Value curl_data_value(const std::vector<CurlData>& data, Alloc& a) {
  Value arr(rapidjson::kArrayType);
  for (const auto& d : data) {
    Value e(rapidjson::kArrayType);
    e.PushBack(Value(rapidjson::StringRef(kCurlDataOpts[static_cast<int>(d.kind)])), a);
    e.PushBack(jstr(d.value, a), a);
    arr.PushBack(e, a);
  }
  return arr;
}

std::vector<CurlData> curl_data_from(const Value& o) {
  std::vector<CurlData> out;
  const Value* arr = get_arr(o, "body_data");
  if (!arr) return out;
  for (const auto& e : arr->GetArray()) {
    if (!e.IsArray() || e.Size() < 2 || !e[0].IsString() || !e[1].IsString()) continue;
    const std::string opt(e[0].GetString(), e[0].GetStringLength());
    for (int k = 0; k < static_cast<int>(std::size(kCurlDataOpts)); ++k)
      if (opt == kCurlDataOpts[k]) {
        out.push_back(CurlData{static_cast<CurlData::Kind>(k), std::string(e[1].GetString(), e[1].GetStringLength())});
        break;
      }
  }
  return out;
}

// "auth_scheme": {type, ...}: only the fields of that type, and of OAuth 2.0 only what is not
// OAuth2Config's default.
Value auth_scheme_value(const HttpAuth& x, Alloc& a) {
  Value o(rapidjson::kObjectType);
  o.AddMember("type", Value(rapidjson::StringRef(auth_type_name(x.type))), a);
  auto put = [&](const char* k, const std::string& v, bool always = false) {
    if (always || !v.empty()) o.AddMember(rapidjson::StringRef(k), jstr(v, a), a);
  };
  switch (x.type) {
    case HttpAuth::Type::None: break;
    case HttpAuth::Type::Inherit: break;
    case HttpAuth::Type::Digest:
    case HttpAuth::Type::Ntlm:
    case HttpAuth::Type::Negotiate:
      put("user", x.user, true);
      put("password", x.password, true);
      break;
    case HttpAuth::Type::AwsSigV4:
      put("access_key", x.aws_access_key, true);
      put("secret_key", x.aws_secret_key, true);
      put("session_token", x.aws_session_token);
      put("region", x.aws_region, true);
      put("service", x.aws_service, true);
      break;
    case HttpAuth::Type::OAuth2: {
      const OAuth2Config& c = x.oauth2;
      const OAuth2Config d;
      const char* g = c.grant == OAuth2Config::Grant::Password            ? "password"
                      : c.grant == OAuth2Config::Grant::AuthorizationCode ? "authorization_code"
                                                                          : "client_credentials";
      o.AddMember("grant", Value(rapidjson::StringRef(g)), a);
      put("token_url", c.token_url, true);
      put("auth_url", c.auth_url);
      put("client_id", c.client_id, true);
      put("client_secret", c.client_secret);
      put("scope", c.scope);
      put("audience", c.audience);
      put("username", c.username);
      put("password", c.password);
      put("redirect_uri", c.redirect_uri);
      if (c.pkce != d.pkce) o.AddMember("pkce", c.pkce, a);
      if (c.client_auth != d.client_auth) o.AddMember("client_auth", "body", a);
      if (c.add_to != d.add_to) o.AddMember("add_to", "query", a);
      if (c.header_prefix != d.header_prefix) put("header_prefix", c.header_prefix, true);
      if (c.query_param != d.query_param) put("query_param", c.query_param, true);
      if (c.auto_refresh != d.auto_refresh) o.AddMember("auto_refresh", c.auto_refresh, a);
      break;
    }
  }
  return o;
}

void auth_scheme_from(const Value& o, HttpAuth& x) {
  x = HttpAuth{};
  x.type = auth_type_from(get_str(o, "type"));
  x.user = get_str(o, "user");
  x.password = get_str(o, "password");
  x.aws_access_key = get_str(o, "access_key");
  x.aws_secret_key = get_str(o, "secret_key");
  x.aws_session_token = get_str(o, "session_token");
  x.aws_region = get_str(o, "region");
  x.aws_service = get_str(o, "service");
  if (x.type == HttpAuth::Type::OAuth2) {
    OAuth2Config& c = x.oauth2;
    const std::string g = get_str(o, "grant");
    c.grant = g == "password"             ? OAuth2Config::Grant::Password
              : g == "authorization_code" ? OAuth2Config::Grant::AuthorizationCode
                                          : OAuth2Config::Grant::ClientCredentials;
    c.token_url = get_str(o, "token_url");
    c.auth_url = get_str(o, "auth_url");
    c.client_id = get_str(o, "client_id");
    c.client_secret = get_str(o, "client_secret");
    c.scope = get_str(o, "scope");
    c.audience = get_str(o, "audience");
    c.username = get_str(o, "username");
    c.password = get_str(o, "password");
    c.redirect_uri = get_str(o, "redirect_uri");
    c.pkce = get_bool(o, "pkce", c.pkce);
    c.client_auth = get_str(o, "client_auth") == "body" ? OAuth2Config::ClientAuth::Body
                                                         : OAuth2Config::ClientAuth::BasicHeader;
    c.add_to = get_str(o, "add_to") == "query" ? OAuth2Config::AddTo::Query : OAuth2Config::AddTo::Header;
    c.header_prefix = get_str(o, "header_prefix", c.header_prefix);
    c.query_param = get_str(o, "query_param", c.query_param);
    c.auto_refresh = get_bool(o, "auto_refresh", c.auto_refresh);
  }
}

// [target, "op value"] (+ false when disabled), one line per row.
Value asserts_value(const std::vector<Assertion>& v, Alloc& a) {
  Value arr(rapidjson::kArrayType);
  for (const auto& as : v) {
    Value e(rapidjson::kArrayType);
    e.PushBack(jstr(as.target, a), a).PushBack(jstr(assert_op_value(as), a), a);
    if (!as.enabled) e.PushBack(false, a);
    arr.PushBack(e, a);
  }
  return arr;
}

// [target, "op value"(, false)]; [target, op, value(, false)] is read too.
std::vector<Assertion> asserts_from(const Value& o) {
  std::vector<Assertion> out;
  if (const Value* as = get_arr(o, "asserts"))
    for (const auto& e : as->GetArray()) {
      if (!e.IsArray() || e.Size() < 2 || !e[0].IsString() || !e[1].IsString()) continue;
      Assertion x;
      x.target.assign(e[0].GetString(), e[0].GetStringLength());
      const std::string second(e[1].GetString(), e[1].GetStringLength());
      if (e.Size() >= 3 && e[2].IsString()) {
        x.op = second;
        x.value.assign(e[2].GetString(), e[2].GetStringLength());
      } else {
        set_assert_op_value(x, second);
      }
      const Value& last = e[e.Size() - 1];
      x.enabled = !(e.Size() >= 3 && last.IsBool() && !last.GetBool());
      out.push_back(std::move(x));
    }
  return out;
}

// folder.json rows (headers, vars): [name, value] or [name, value, false] (disabled) on one
// line; a row with a description is an object {name, value, enabled?, description}.
Value rows_value(const std::vector<SettingsRow>& rows, Alloc& a) {
  Value arr(rapidjson::kArrayType);
  for (const auto& x : rows) {
    if (x.description.empty()) {
      Value e(rapidjson::kArrayType);
      e.PushBack(jstr(x.name, a), a).PushBack(jstr(x.value, a), a);
      if (!x.enabled) e.PushBack(false, a);
      arr.PushBack(e, a);
    } else {
      Value e(rapidjson::kObjectType);
      e.AddMember("name", jstr(x.name, a), a);
      e.AddMember("value", jstr(x.value, a), a);
      if (!x.enabled) e.AddMember("enabled", false, a);
      e.AddMember("description", jstr(x.description, a), a);
      arr.PushBack(e, a);
    }
  }
  return arr;
}

std::vector<SettingsRow> rows_from(const Value& o, const char* key) {
  std::vector<SettingsRow> out;
  const Value* arr = get_arr(o, key);
  if (!arr) return out;
  for (const auto& e : arr->GetArray()) {
    SettingsRow x;
    if (e.IsArray()) {
      if (e.Size() < 1 || !e[0].IsString()) continue;
      x.name.assign(e[0].GetString(), e[0].GetStringLength());
      if (e.Size() >= 2 && e[1].IsString()) x.value.assign(e[1].GetString(), e[1].GetStringLength());
      for (SizeType i = 2; i < e.Size(); ++i) {
        if (e[i].IsBool()) x.enabled = e[i].GetBool();
        else if (e[i].IsString()) x.description.assign(e[i].GetString(), e[i].GetStringLength());
      }
    } else if (e.IsObject()) {
      x.name = get_str(e, "name");
      x.value = get_str(e, "value");
      x.description = get_str(e, "description");
      x.enabled = get_bool(e, "enabled", true);
    } else {
      continue;
    }
    if (x.name.empty()) continue;
    out.push_back(std::move(x));
  }
  return out;
}

// folder.json:
//   { "version": 1,
//     "headers": [[name, value] | [name, value, false] | {name, value, enabled?, description}, ...],
//     "auth": {type: "none"} | {type: "basic", enabled?, user, password} | {type: digest | ntlm |
//             negotiate | awsv4 | oauth2, ...as a request's "auth_scheme"}   (absent: inherit)
//     "vars": [rows as headers],
//     "asserts": [as in a request],
//     "pre_script": ..., "post_script": ... (as in a request) }
// Every key but "version" only when set.
// "network": { ca_file?, ca_off?, cert_file?, key_file?, cert_off?, proxy?, proxy_url?, no_proxy? }
// (NetworkSettings; proxy is "env" | "system" | "none" | "url", absent = inherit). Only what is set.
const char* proxy_mode_name(NetworkSettings::Proxy p) {
  switch (p) {
    case NetworkSettings::Proxy::Env: return "env";
    case NetworkSettings::Proxy::System: return "system";
    case NetworkSettings::Proxy::Off: return "none";
    case NetworkSettings::Proxy::Url: return "url";
    default: return "inherit";
  }
}

Value network_value(const NetworkSettings& n, Alloc& a) {
  Value o(rapidjson::kObjectType);
  if (!n.ca_file.empty()) o.AddMember("ca_file", jstr(n.ca_file, a), a);
  if (n.ca_off) o.AddMember("ca_off", true, a);
  if (!n.cert_file.empty()) o.AddMember("cert_file", jstr(n.cert_file, a), a);
  if (!n.key_file.empty()) o.AddMember("key_file", jstr(n.key_file, a), a);
  if (n.cert_off) o.AddMember("cert_off", true, a);
  if (n.proxy != NetworkSettings::Proxy::Inherit)
    o.AddMember("proxy", Value(rapidjson::StringRef(proxy_mode_name(n.proxy))), a);
  if (!n.proxy_url.empty()) o.AddMember("proxy_url", jstr(n.proxy_url, a), a);
  if (!n.no_proxy.empty()) o.AddMember("no_proxy", jstr(n.no_proxy, a), a);
  return o;
}

NetworkSettings network_from(const Value& parent) {
  NetworkSettings n;
  const Value* o = get_obj(parent, "network");
  if (!o) return n;
  n.ca_file = get_str(*o, "ca_file");
  n.ca_off = get_bool(*o, "ca_off", false);
  n.cert_file = get_str(*o, "cert_file");
  n.key_file = get_str(*o, "key_file");
  n.cert_off = get_bool(*o, "cert_off", false);
  const std::string p = lower_ascii(get_str(*o, "proxy", "inherit"));
  n.proxy = p == "env"                                   ? NetworkSettings::Proxy::Env
            : p == "system"                              ? NetworkSettings::Proxy::System
            : p == "none" || p == "off" || p == "direct" ? NetworkSettings::Proxy::Off
            : p == "url"                                 ? NetworkSettings::Proxy::Url
                                                         : NetworkSettings::Proxy::Inherit;
  n.proxy_url = get_str(*o, "proxy_url");
  n.no_proxy = get_str(*o, "no_proxy");
  return n;
}

Value folder_value(const FolderSettings& s, Alloc& a) {
  Value o(rapidjson::kObjectType);
  o.AddMember("version", 1, a);
  if (!s.headers.empty()) o.AddMember("headers", rows_value(s.headers, a), a);
  if (s.auth.type == HttpAuth::Type::None) {
    const BasicAuth& ba = s.basic_auth;
    Value auth(rapidjson::kObjectType);
    if (ba.enabled || !ba.user.empty() || !ba.password.empty()) {
      auth.AddMember("type", "basic", a);
      if (!ba.enabled) auth.AddMember("enabled", false, a);
      auth.AddMember("user", jstr(ba.user, a), a);
      auth.AddMember("password", jstr(ba.password, a), a);
    } else {
      auth.AddMember("type", "none", a);
    }
    o.AddMember("auth", auth, a);
  } else if (s.auth.type != HttpAuth::Type::Inherit) {
    o.AddMember("auth", auth_scheme_value(s.auth, a), a);
  }
  if (!s.vars.empty()) o.AddMember("vars", rows_value(s.vars, a), a);
  if (!s.asserts.empty()) o.AddMember("asserts", asserts_value(s.asserts, a), a);
  if (!s.pre_script.empty()) o.AddMember("pre_script", body_value(s.pre_script, a, true), a);
  if (!s.post_script.empty()) o.AddMember("post_script", body_value(s.post_script, a, true), a);
  if (!s.network.empty()) o.AddMember("network", network_value(s.network, a), a);
  return o;
}

void folder_from_value(const Value& o, FolderSettings& s) {
  s = FolderSettings{};
  s.headers = rows_from(o, "headers");
  if (const Value* auth = get_obj(o, "auth")) {
    const std::string t = lower_ascii(get_str(*auth, "type", "inherit"));
    if (t == "basic") {
      s.auth = HttpAuth{};
      s.basic_auth.enabled = get_bool(*auth, "enabled", true);
      s.basic_auth.user = get_str(*auth, "user");
      s.basic_auth.password = get_str(*auth, "password");
    } else if (t == "none" || t == "noauth") {
      s.auth = HttpAuth{};
    } else if (t != "inherit") {
      auth_scheme_from(*auth, s.auth);
      // An unknown scheme name reads as None: fail closed (no credentials), never as Inherit.
    }
  }
  s.vars = rows_from(o, "vars");
  s.asserts = asserts_from(o);
  s.pre_script = body_from(o, "pre_script");
  s.post_script = body_from(o, "post_script");
  s.network = network_from(o);
}

Value request_value(const SavedRequest& r, Alloc& a, bool lines, size_t body_cap) {
  Value o(rapidjson::kObjectType);
  o.AddMember("version", 1, a);
  o.AddMember("id", jstr(r.id, a), a);
  o.AddMember("name", jstr(r.name, a), a);
  if (!r.docs.empty()) o.AddMember("docs", jstr(r.docs, a), a);
  o.AddMember("kind", Value(rapidjson::StringRef(r.kind == RequestKind::Grpc ? "grpc" : "http")), a);
  Value http = http_section(r.http, a, lines, body_cap);
  // A disabled header is [name, value, false], in its place.
  if (!r.disabled_headers.empty())
    if (auto hs = http.FindMember("headers"); hs != http.MemberEnd())
      for (size_t k : r.disabled_headers)
        if (k < hs->value.Size()) hs->value[static_cast<SizeType>(k)].PushBack(false, a);
  if (!r.body_file.empty() || !r.form.empty() || !r.body_data.empty()) {
    // The body is the file (read at send time) or built from the form parts / curl data: not stored.
    http.RemoveMember("body");
    http.RemoveMember("body_base64");
  }
  if (!r.body_file.empty()) http.AddMember("body_file", jstr(r.body_file, a), a);
  if (!r.body_data.empty()) http.AddMember("body_data", curl_data_value(r.body_data, a), a);
  if (!r.disabled_params.empty()) http.AddMember("disabled_params", pairs_value(r.disabled_params, a), a);
  if (!r.path_params.empty()) http.AddMember("path_params", pairs_value(r.path_params, a), a);
  if (!r.header_files.empty()) http.AddMember("header_files", strings_value(r.header_files, a), a);
  if (!r.cookie_files.empty()) http.AddMember("cookie_files", strings_value(r.cookie_files, a), a);
  if (!r.form.empty()) http.AddMember("form", form_value(r.form, a), a);
  const BasicAuth& ba = r.basic_auth;
  if (ba.enabled || !ba.user.empty() || !ba.password.empty()) {
    Value auth(rapidjson::kObjectType);
    auth.AddMember("type", "basic", a);
    if (!ba.enabled) auth.AddMember("enabled", false, a);
    auth.AddMember("user", jstr(ba.user, a), a);
    auth.AddMember("password", jstr(ba.password, a), a);
    http.AddMember("auth", auth, a);
  }
  if (r.auth.type != HttpAuth::Type::None) http.AddMember("auth_scheme", auth_scheme_value(r.auth, a), a);
  if (!r.network.empty()) http.AddMember("network", network_value(r.network, a), a);
  if (r.kind == RequestKind::Http || detail::json_compact(http) != default_http_json())
    o.AddMember("http", http, a);
  Value grpc = grpc_section(r, a, lines, body_cap);
  if (r.kind == RequestKind::Grpc || detail::json_compact(grpc) != default_grpc_json())
    o.AddMember("grpc", grpc, a);
  // Last, and only when present, so files without them keep their bytes. One line per row, as
  // in .bru files: an assertion is [target, "op value"] (+ false when disabled).
  if (!r.asserts.empty()) o.AddMember("asserts", asserts_value(r.asserts, a), a);
  if (!r.captures.empty()) {
    Value arr(rapidjson::kArrayType);
    for (const auto& c : r.captures) {
      Value e(rapidjson::kObjectType);
      e.AddMember("var", jstr(c.var, a), a);
      e.AddMember("from", jstr(c.target, a), a);
      e.AddMember("scope", Value(rapidjson::StringRef(capture_scope_name(c.scope))), a);
      if (!c.enabled) e.AddMember("enabled", false, a);
      arr.PushBack(e, a);
    }
    o.AddMember("captures", arr, a);
  }
  // Scripts: multi-line ones as an array of lines, like bodies (a one-line edit is a one-line diff).
  if (!r.pre_script.empty()) o.AddMember("pre_script", body_value(r.pre_script, a, true), a);
  if (!r.post_script.empty()) o.AddMember("post_script", body_value(r.post_script, a, true), a);
  return o;
}

void request_from_value(const Value& o, SavedRequest& r) {
  r.id = get_str(o, "id");
  r.name = get_str(o, "name");
  r.docs = get_str(o, "docs");
  r.kind = get_str(o, "kind") == "grpc" ? RequestKind::Grpc : RequestKind::Http;
  if (const Value* h = get_obj(o, "http")) {
    r.http.method = get_str(*h, "method", "GET");
    r.http.url = get_str(*h, "url");
    r.http.headers = pairs_from(*h, "headers");
    if (const Value* hs = get_arr(*h, "headers")) {
      size_t k = 0;
      for (const auto& e : hs->GetArray()) {
        if (!(e.IsArray() && e.Size() >= 1 && e[0].IsString()) && !e.IsObject()) continue;  // as pairs_from
        const bool off = e.IsArray() ? e.Size() >= 3 && e[2].IsBool() && !e[2].GetBool()
                                     : !get_bool(e, "enabled", true);
        if (off) r.disabled_headers.insert(k);
        ++k;
      }
    }
    r.http.body = body_from(*h, "body");
    if (auto b64 = h->FindMember("body_base64"); b64 != h->MemberEnd() && b64->value.IsString())
      detail::base64_decode(std::string(b64->value.GetString(), b64->value.GetStringLength()), r.http.body);
    r.http.timeout_ms = get_long(*h, "timeout_ms", r.http.timeout_ms);
    r.http.follow_redirects = get_bool(*h, "follow_redirects", r.http.follow_redirects);
    r.http.verify_tls = get_bool(*h, "verify_tls", r.http.verify_tls);
    r.http.version = version_from(get_str(*h, "http_version"));
    r.http.fresh_connection = get_bool(*h, "fresh_connection", r.http.fresh_connection);
    r.form = form_from(*h);
    r.body_file = get_str(*h, "body_file");
    r.body_data = curl_data_from(*h);
    r.disabled_params = pairs_from(*h, "disabled_params");
    r.path_params = pairs_from(*h, "path_params");
    r.header_files = strings_from(*h, "header_files");
    r.cookie_files = strings_from(*h, "cookie_files");
    if (const Value* auth = get_obj(*h, "auth"); auth && get_str(*auth, "type", "basic") == "basic") {
      r.basic_auth.enabled = get_bool(*auth, "enabled", true);
      r.basic_auth.user = get_str(*auth, "user");
      r.basic_auth.password = get_str(*auth, "password");
    }
    if (const Value* sch = get_obj(*h, "auth_scheme")) auth_scheme_from(*sch, r.auth);
    r.network = network_from(*h);
  }
  if (const Value* g = get_obj(o, "grpc")) {
    r.grpc_target.address = get_str(*g, "address");
    r.grpc_target.tls = get_bool(*g, "tls", false);
    r.grpc_target.insecure_skip_verify = get_bool(*g, "insecure_skip_verify", false);
    r.grpc_target.web = get_bool(*g, "web", false);
    r.grpc_target.web_text = get_bool(*g, "web_text", false);
    r.grpc_target.authority = get_str(*g, "authority");
    r.grpc_target.root_cert_pem = body_from(*g, "root_cert_pem");
    r.grpc_method = get_str(*g, "method");
    r.grpc_body = body_from(*g, "body");
    r.grpc_opts.deadline_ms = get_long(*g, "deadline_ms", r.grpc_opts.deadline_ms);
    r.grpc_opts.metadata = pairs_from(*g, "metadata");
    r.proto_files = strings_from(*g, "proto_files");
    r.import_dirs = strings_from(*g, "import_dirs");
  }
  r.asserts = asserts_from(o);
  if (const Value* cs = get_arr(o, "captures"))
    for (const auto& e : cs->GetArray()) {
      if (!e.IsObject()) continue;
      Capture c;
      c.var = get_str(e, "var");
      c.target = get_str(e, "from");
      c.scope = capture_scope_from(get_str(e, "scope"));
      c.enabled = get_bool(e, "enabled", true);
      r.captures.push_back(std::move(c));
    }
  r.pre_script = body_from(o, "pre_script");
  r.post_script = body_from(o, "post_script");
}

std::string parse_error_text(const rapidjson::Document& d, const fs::path& file) {
  return to_utf8(file) + ": invalid JSON at offset " + std::to_string(d.GetErrorOffset()) + ": " +
         rapidjson::GetParseError_En(d.GetParseError());
}

// ------------------------------------------------------------------ tree

bool node_less(const CollectionNode& a, const CollectionNode& b) {
  if (a.is_folder != b.is_folder) return a.is_folder;
  const std::string la = lower_ascii(a.name), lb = lower_ascii(b.name);
  return la != lb ? la < lb : a.name < b.name;
}

// `on_file(child, entry, key)` fills a request node's badge; key = path relative to the root.
template <class OnFile>
void scan_dir(const fs::path& dir, CollectionNode& node, int depth, const std::string& prefix, const OnFile& on_file) {
  if (depth > 32) return;
  std::error_code ec;
  fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
  for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
    const fs::directory_entry& e = *it;
    const std::string name = to_utf8(e.path().filename());
    if (name.empty() || name[0] == '.') continue;  // .watt, .git, ...
    std::error_code ec2;
    if (e.is_directory(ec2)) {
      if (name == "node_modules" || (depth == 0 && name == kEnvDir) || e.is_symlink(ec2)) continue;
      CollectionNode child;
      child.name = name;
      child.path = e.path();
      child.is_folder = true;
      scan_dir(e.path(), child, depth + 1, prefix + name + "/", on_file);
      node.children.push_back(std::move(child));
    } else if (ends_with(name, kReqExt) && name.size() > std::char_traits<char>::length(kReqExt)) {
      CollectionNode child;
      child.name = name.substr(0, name.size() - std::char_traits<char>::length(kReqExt));
      child.path = e.path();
      on_file(child, e, prefix + name);
      node.children.push_back(std::move(child));
    }
  }
  std::sort(node.children.begin(), node.children.end(), node_less);
}

// Kind and HTTP method from the first bytes of a .req.json (our writer puts "kind" and then
// "http": {"method" ...} right after the id and name). False if they are not in `head`.
bool badge_from_head(const std::string& head, RequestKind& kind, std::string& method) {
  const size_t k = head.find("\"kind\": \"");
  if (k == std::string::npos) return false;
  const size_t ke = head.find('"', k + 9);
  if (ke == std::string::npos) return false;
  if (head.compare(k + 9, ke - k - 9, "grpc") == 0) {
    kind = RequestKind::Grpc;
    method.clear();
    return true;
  }
  kind = RequestKind::Http;
  const size_t h = head.find("\"http\": {", ke);
  if (h == std::string::npos) return false;
  const size_t m = head.find("\"method\": \"", h);
  if (m == std::string::npos) return false;
  const size_t me = head.find('"', m + 11);
  if (me == std::string::npos || me - m - 11 > 32) return false;
  method = head.substr(m + 11, me - m - 11);
  return true;
}

bool read_head(const fs::path& p, std::string& out, size_t n) { return detail::read_file_head(p, out, n); }

int64_t mtime_of(const fs::file_time_type& t) { return static_cast<int64_t>(t.time_since_epoch().count()); }

std::string generic_utf8(const fs::path& p) {
  const std::u8string u = p.generic_u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

bool is_inside(const fs::path& root, const fs::path& p) {
  const fs::path rel = p.lexically_normal().lexically_relative(root);
  if (rel.empty()) return false;
  const std::string first = to_utf8(*rel.begin());
  return first != ".." && first != ".";
}

}  // namespace

namespace {

constexpr const char* kStateFile = "state.json";      // in .watt/
constexpr const char* kSecretsFile = "secrets.json";  // in .watt/
// In secrets.json: { level key: sealed passphrase } (Workspace::set_cert_passphrase).
constexpr const char* kCertPassKey = "watt:cert_passphrases";

std::string sealed_key(const std::string& env, const std::string& var) { return env + '\0' + var; }

void read_string_map(const Value& o, const char* key, std::map<std::string, std::string>& out) {
  if (const Value* m = get_obj(o, key))
    for (auto it = m->MemberBegin(); it != m->MemberEnd(); ++it)
      if (it->value.IsString())
        out[std::string(it->name.GetString(), it->name.GetStringLength())] =
            std::string(it->value.GetString(), it->value.GetStringLength());
}

bool parse_json_file(const fs::path& file, rapidjson::Document& d) {
  std::string text;
  if (!detail::read_file(file, text)) return false;
  d.Parse(text.data(), text.size());
  return !d.HasParseError() && d.IsObject();
}

// Writes `text` (LF line ends) unless the file already holds exactly that (a no-op save leaves
// git clean). A file checked out with CRLF (Git for Windows, core.autocrlf=true) is compared and
// written with CRLF, so git sees no change either way. Our JSON has no raw CR in it.
// The written file is pre-scanned (model_prescan.h): `when` says whether the next start needs it.
bool write_if_changed(const fs::path& file, const std::string& text, std::string* err, bool* wrote = nullptr,
                      detail::Prescan when = detail::Prescan::BeforeExit) {
  if (wrote) *wrote = false;
  std::string old;
  const bool had = detail::read_file(file, old);
  const size_t lf = had ? old.find('\n') : std::string::npos;
  std::string crlf;
  const std::string* out = &text;
  if (lf != std::string::npos && lf > 0 && old[lf - 1] == '\r') {
    crlf.reserve(text.size() + text.size() / 16);
    for (char c : text) {
      if (c == '\n') crlf += '\r';
      crlf += c;
    }
    out = &crlf;
  }
  if (had && old == *out) return true;
  detail::ensure_dir(file.parent_path());
  if (!detail::write_file_atomic(file, *out, err)) return false;
  if (wrote) *wrote = true;
  detail::prescan_later(file, when);
  return true;
}

// .env: KEY=value lines; '#' comments, optional "export ", optional single or double quotes.
std::map<std::string, std::string> parse_dotenv(const std::string& text) {
  std::map<std::string, std::string> out;
  size_t p = 0;
  while (p < text.size()) {
    size_t e = text.find('\n', p);
    if (e == std::string::npos) e = text.size();
    std::string line = text.substr(p, e - p);
    p = e + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t b = line.find_first_not_of(" \t");
    if (b == std::string::npos || line[b] == '#') continue;
    if (line.compare(b, 7, "export ") == 0) b += 7;
    const size_t eq = line.find('=', b);
    if (eq == std::string::npos) continue;
    std::string key = line.substr(b, eq - b);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
    std::string v = line.substr(eq + 1);
    const size_t vb = v.find_first_not_of(" \t");
    v = vb == std::string::npos ? "" : v.substr(vb);
    if (v.size() >= 2 && (v[0] == '"' || v[0] == '\'') && v.find(v[0], 1) != std::string::npos) {
      const char q = v[0];
      const size_t close = v.find(q, 1);
      std::string inner = v.substr(1, close - 1);
      if (q == '"') {  // \n escapes inside double quotes, as dotenv does
        std::string un;
        for (size_t i = 0; i < inner.size(); ++i) {
          if (inner[i] == '\\' && i + 1 < inner.size() && inner[i + 1] == 'n') {
            un += '\n';
            ++i;
          } else {
            un += inner[i];
          }
        }
        inner = un;
      }
      v = inner;
    } else {
      const size_t hash = v.find(" #");
      if (hash != std::string::npos) v.resize(hash);
      while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
    }
    if (!key.empty()) out[key] = v;
  }
  return out;
}

}  // namespace

// ==================================================================== Workspace

namespace detail {
void expire_folder_cache(FolderCache& fc);  // with the folder settings, below
}
using detail::expire_folder_cache;

std::optional<Workspace> Workspace::open(const fs::path& root, std::string* err) {
  std::error_code ec;
  const fs::path abs = fs::absolute(root, ec).lexically_normal();
  if (ec || !fs::is_directory(abs, ec)) {
    if (err) *err = "not a directory: " + to_utf8(root);
    return std::nullopt;
  }
  const fs::path file = abs / kSettingsFile;
  std::string text;
  if (!detail::read_file(file, text)) {
    if (err) *err = "not a watt workspace (no watt.json): " + to_utf8(abs);
    return std::nullopt;
  }
  rapidjson::Document d;
  d.Parse(text.data(), text.size());
  if (d.HasParseError() || !d.IsObject()) {
    if (err) *err = d.HasParseError() ? parse_error_text(d, file) : to_utf8(file) + ": not a JSON object";
    return std::nullopt;
  }
  Workspace ws;
  ws.root_ = abs;
  read_string_map(d, "globals", ws.globals);

  // Environments: one file each in environments/, ordered by "seq" then name. Workspaces
  // written before that layout kept them (and active_env) in watt.json; the next
  // save_settings() moves them.
  const fs::path env_dir = abs / kEnvDir;
  if (fs::is_directory(env_dir, ec)) {
    std::vector<std::pair<std::pair<int64_t, std::string>, Environment>> found;
    for (fs::directory_iterator it(env_dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
      if (!it->is_regular_file(ec) || it->path().extension() != ".json") continue;
      rapidjson::Document e;
      if (!parse_json_file(it->path(), e)) continue;
      Environment env;
      env.name = get_str(e, "name", to_utf8(it->path().stem()));
      read_string_map(e, "vars", env.vars);
      for (const auto& s : strings_from(e, "secrets")) env.secrets.insert(s);
      const int64_t seq = static_cast<int64_t>(get_num(e, "seq", 1e15));
      found.push_back({{seq, env.name}, std::move(env)});
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [key, env] : found) ws.envs_.push_back(std::move(env));
  } else if (const Value* envs = get_arr(d, "environments")) {
    for (const auto& e : envs->GetArray()) {
      if (!e.IsObject()) continue;
      Environment env;
      env.name = get_str(e, "name");
      read_string_map(e, "vars", env.vars);
      ws.envs_.push_back(std::move(env));
    }
  }
  ws.active_env = get_str(d, "active_env");
  rapidjson::Document st;
  if (parse_json_file(abs / ".watt" / kStateFile, st)) ws.active_env = get_str(st, "active_env");
  rapidjson::Document sec;
  if (parse_json_file(abs / ".watt" / kSecretsFile, sec))
    for (auto& env : ws.envs_)
      if (const Value* m = get_obj(sec, env.name.c_str()))
        for (auto it = m->MemberBegin(); it != m->MemberEnd(); ++it) {
          const std::string k(it->name.GetString(), it->name.GetStringLength());
          if (!it->value.IsString() || !env.secrets.count(k)) continue;
          const std::string stored(it->value.GetString(), it->value.GetStringLength());
          std::string plain;
          switch (detail::unseal_secret(stored, plain)) {
            case detail::Unseal::Ok:
              ws.sealed_[sealed_key(env.name, k)] = {plain, stored};
              env.vars[k] = std::move(plain);
              break;
            case detail::Unseal::Plain:  // legacy plaintext: sealed by the next save_settings()
              env.vars[k] = std::move(plain);
              break;
            case detail::Unseal::Failed:  // another user / machine: kept, flagged, never wiped
              env.unreadable_secrets[k] = stored;
              break;
          }
        }
  std::string dot;
  if (detail::read_file(abs / ".env", dot)) ws.dotenv = parse_dotenv(dot);
  expire_folder_cache(ws.folder_cache());  // a (re)opened workspace sees folder.json as it is on disk now
  return ws;
}

Workspace Workspace::create(const fs::path& root) {
  std::error_code ec;
  detail::ensure_dir(root);
  if (fs::exists(root / kSettingsFile, ec)) {
    if (auto ws = open(root)) return std::move(*ws);
  }
  Workspace ws;
  ws.root_ = fs::absolute(root, ec).lexically_normal();
  if (!fs::exists(ws.root_ / kSettingsFile, ec)) ws.save_settings();
  // History, the active environment and secret values are personal; so is .env.
  const fs::path gi = ws.root_ / ".gitignore";
  if (!fs::exists(gi, ec)) detail::write_file_atomic(gi, ".watt/\n.env\n");
  // Saved files stay LF in git and on disk, also with Git for Windows' core.autocrlf=true.
  const fs::path ga = ws.root_ / ".gitattributes";
  if (!fs::exists(ga, ec)) detail::write_file_atomic(ga, "*.json text eol=lf\n");
  return ws;
}

}  // namespace watt

namespace watt::detail {

// Sidebar badge cache: .watt/index.json {version, files: {relpath: [mtime, size, kind, method]}}
// plus an append-only journal .watt/index.log, one compact JSON array per line:
// ["relpath", mtime, size, kind, method] sets an entry, ["relpath"] removes it. A save appends
// one ~80-byte line instead of rewriting the whole index (139 KB for 2000 requests), so the
// next start opens an unchanged index.json and a small log. The log is folded into index.json
// (compacted) once it passes kLogMaxBytes or a flush has many changes.
struct TreeIndex {
  struct Entry {
    int64_t mtime = 0;
    uint64_t size = 0;
    RequestKind kind = RequestKind::Http;
    std::string method;
  };
  static constexpr uint64_t kLogMaxBytes = 16u << 10;
  static constexpr size_t kLogMaxChanges = 64;  // more in one flush: compact instead
  fs::path file;
  std::map<std::string, Entry> entries;  // keyed by the path relative to the root (generic, UTF-8)
  std::map<std::string, std::optional<Entry>> pending;  // changes not written yet (nullopt: removed)
  uint64_t log_bytes = 0;  // size of index.log when loaded, plus what this process appended
  bool loaded = false;
  bool has_base = false;  // index.json exists
  std::mutex mu;

  fs::path log_file() const { return file.parent_path() / "index.log"; }

  void set(const std::string& key, Entry e) {
    pending.insert_or_assign(key, e);
    entries.insert_or_assign(key, std::move(e));
  }
  void erase(const std::string& key) {
    pending.insert_or_assign(key, std::nullopt);
    entries.erase(key);
  }

  static bool entry_from(const Value* v, Entry& e) {  // v: [mtime, size, kind, method]
    if (!v[0].IsInt64() || !v[1].IsUint64() || !v[2].IsString() || !v[3].IsString()) return false;
    e.mtime = v[0].GetInt64();
    e.size = v[1].GetUint64();
    e.kind = std::string_view(v[2].GetString(), v[2].GetStringLength()) == "grpc" ? RequestKind::Grpc : RequestKind::Http;
    e.method.assign(v[3].GetString(), v[3].GetStringLength());
    return true;
  }
  static void entry_value(Value& v, const Entry& e, Alloc& a) {
    v.PushBack(Value(e.mtime), a).PushBack(Value(e.size), a);
    v.PushBack(Value(rapidjson::StringRef(e.kind == RequestKind::Grpc ? "grpc" : "http")), a);
    v.PushBack(jstr(e.method, a), a);
  }

  void load() {
    if (loaded) return;
    loaded = true;
    std::map<std::string, Entry> disk;
    rapidjson::Document d;
    has_base = parse_json_file(file, d);
    if (const Value* files = has_base ? get_obj(d, "files") : nullptr) {
      for (auto it = files->MemberBegin(); it != files->MemberEnd(); ++it) {
        const Value& v = it->value;
        Entry e;
        if (v.IsArray() && v.Size() == 4 && entry_from(&v[0], e))
          disk.emplace(std::string(it->name.GetString(), it->name.GetStringLength()), std::move(e));
      }
    }
    std::string log;
    if (detail::read_file(log_file(), log)) {
      log_bytes = log.size();
      for (size_t p = 0; p < log.size();) {
        size_t nl = log.find('\n', p);
        if (nl == std::string::npos) nl = log.size();
        rapidjson::Document l;
        l.Parse(log.data() + p, nl - p);
        p = nl + 1;
        if (l.HasParseError() || !l.IsArray() || l.Empty() || !l[0].IsString()) continue;  // torn line
        std::string key(l[0].GetString(), l[0].GetStringLength());
        Entry e;
        if (l.Size() == 1) disk.erase(key);
        else if (l.Size() == 5 && entry_from(&l[1], e)) disk.insert_or_assign(std::move(key), std::move(e));
      }
    }
    // Changes noted by save_request before the load are newer.
    for (const auto& [k, e] : pending) {
      if (e) disk.insert_or_assign(k, *e);
      else disk.erase(k);
    }
    entries = std::move(disk);
  }

  // Rewrites index.json from `entries` and drops the log.
  bool compact() {
    rapidjson::Document d;
    d.SetObject();
    auto& a = d.GetAllocator();
    d.AddMember("version", 1, a);
    Value files(rapidjson::kObjectType);
    for (const auto& [k, e] : entries) {
      Value v(rapidjson::kArrayType);
      entry_value(v, e, a);
      files.AddMember(jstr(k, a), v, a);
    }
    d.AddMember("files", files, a);
    std::error_code ec;
    detail::ensure_dir(file.parent_path());
    if (!detail::write_file_atomic(file, detail::json_compact(d))) return false;
    fs::remove(log_file(), ec);
    log_bytes = 0;
    has_base = true;
    detail::prescan_later(file);
    return true;
  }

  // Writes the pending changes: appended to the log, or (when loaded and the log would grow past
  // kLogMaxBytes, or there is no index.json yet) by compacting.
  void flush() {
    // Many changes (a first fill of 2000 badges): straight to compaction, no log text.
    if (loaded && ((!has_base && !entries.empty()) || pending.size() > kLogMaxChanges)) {
      if (compact()) pending.clear();
      return;
    }
    std::string text;
    for (const auto& [k, e] : pending) {
      rapidjson::Document l;
      l.SetArray();
      auto& a = l.GetAllocator();
      l.PushBack(jstr(k, a), a);
      if (e) entry_value(l, *e, a);
      text += detail::json_compact(l);
      text += '\n';
    }
    if (loaded && ((!has_base && !entries.empty()) || log_bytes + text.size() > kLogMaxBytes ||
                   pending.size() > kLogMaxChanges)) {
      if (compact()) pending.clear();
      return;
    }
    if (text.empty()) return;
    detail::ensure_dir(file.parent_path());
    if (!detail::append_file(log_file(), text)) return;
    log_bytes += text.size();
    pending.clear();
    detail::prescan_later(log_file());
  }

  ~TreeIndex() {
    // At exit: changes not written yet are appended to the log, so the next process does not
    // have to open those files.
    std::error_code ec;
    if (pending.empty() || !fs::exists(file.parent_path().parent_path() / kSettingsFile, ec)) return;
    flush();
  }
};

}  // namespace watt::detail

namespace watt {

namespace {

// One badge index and one set of history handles per workspace root for the whole process:
// a Workspace opened again (another window, a reload) finds them warm instead of re-reading
// index.json or re-opening history files (each first open after a write costs an antivirus
// scan on Windows).
template <class T>
std::shared_ptr<T> shared_for_root(const fs::path& root) {
  detail::prescan_init();  // destroyed after the cache below: ~TreeIndex may still prescan
  static std::mutex m;
  static std::map<fs::path, std::shared_ptr<T>> cache;
  std::lock_guard<std::mutex> lock(m);
  auto& p = cache[root];
  if (!p) p = std::make_shared<T>();
  return p;
}

}  // namespace

detail::TreeIndex& Workspace::tree_index() const {
  if (!tree_index_) {
    tree_index_ = shared_for_root<detail::TreeIndex>(root_);
    std::lock_guard<std::mutex> lock(tree_index_->mu);
    tree_index_->file = root_ / ".watt" / "index.json";
  }
  return *tree_index_;
}

CollectionNode Workspace::tree(double badge_budget_ms) const {
  using clock = std::chrono::steady_clock;
  const auto t0 = clock::now();
  expire_folder_cache(folder_cache());  // a refresh also re-checks folder.json files (git pull, an editor)
  CollectionNode node;
  node.name = to_utf8(root_.filename());
  if (node.name.empty()) node.name = to_utf8(root_);
  node.path = root_;
  node.is_folder = true;

  // Badges are validated by the mtime and size the directory listing already has. Only new or
  // changed files are opened, and only their first KiB.
  detail::TreeIndex& idx = tree_index();
  std::unique_lock<std::mutex> lock(idx.mu);
  idx.load();
  std::set<std::string> seen;
  std::string head;
  // No index at all (fresh clone, first open): every file is cold (an antivirus scan of ~10 ms
  // each on Windows), so a budgeted call reads none and leaves the badges to tree(-1) on a
  // worker. Otherwise reads stop at the budget, or at the first read slower than kSlowReadMs.
  constexpr double kSlowReadMs = 2;
  const bool unbudgeted = badge_budget_ms < 0;
  bool out_of_time = !unbudgeted && idx.entries.empty();
  // tree(-1): files to read are queued and read in parallel by a pool that starts with the
  // first one, so the reads overlap the directory listing (the antivirus scans files
  // concurrently). The index lock is released once the listing is done: a save on the UI
  // thread must not wait for the whole fill.
  struct Miss {
    std::string key;
    fs::path path;
    detail::TreeIndex::Entry entry;  // mtime and size from the listing; kind and method read
  };
  auto read_badge = [this](const fs::path& p, std::string& buf, detail::TreeIndex::Entry& ne) {
    if (!(read_head(p, buf, 1024) && badge_from_head(buf, ne.kind, ne.method))) {
      if (auto r = load_request(p)) {
        ne.kind = r->kind;
        ne.method = r->kind == RequestKind::Http ? r->http.method : "";
      }
    }
  };
  std::deque<Miss> misses;  // push_back keeps references to queued elements valid
  std::mutex qmu;
  std::condition_variable qcv;
  size_t qnext = 0;
  bool listed = false;
  // Warm files: a few readers beat many (they contend with the listing in the file system
  // filters). Cold ones (antivirus scans of ~10 ms each): the scans run in parallel, so up
  // to 16.
  // Cold: a few reads over kColdReadMs (one slow read alone is just contention).
  constexpr double kColdReadMs = 5;
  std::atomic<int> slow_reads{0};
  auto note_read = [&](clock::time_point r0) {
    if (std::chrono::duration<double, std::milli>(clock::now() - r0).count() > kColdReadMs) ++slow_reads;
  };
  std::vector<std::thread> pool;
  const size_t cores = std::max(1u, std::thread::hardware_concurrency());
  const size_t warm_threads = std::min<size_t>(cores, 4), cold_threads = std::min<size_t>(cores, 16);
  auto grow = [&](size_t queued) {  // the calling (listing) thread only
    const size_t want = std::min((slow_reads >= 3 ? cold_threads : warm_threads), (queued + 7) / 8);
    while (pool.size() < want) pool.emplace_back([&] {
      std::string buf;
      for (;;) {
        Miss* m;
        {
          std::unique_lock<std::mutex> ql(qmu);
          qcv.wait(ql, [&] { return qnext < misses.size() || listed; });
          if (qnext >= misses.size()) return;
          m = &misses[qnext++];
        }
        const auto r0 = clock::now();
        read_badge(m->path, buf, m->entry);
        note_read(r0);
      }
    });
  };
  auto queue_miss = [&](Miss&& m) {
    size_t queued;
    {
      std::lock_guard<std::mutex> ql(qmu);
      misses.push_back(std::move(m));
      queued = misses.size() - qnext;
    }
    grow(queued);
    qcv.notify_one();
  };
  scan_dir(root_, node, 0, std::string(), [&](CollectionNode& child, const fs::directory_entry& e, std::string key) {
    std::error_code ec;
    const int64_t mtime = mtime_of(e.last_write_time(ec));
    const uint64_t size = e.file_size(ec);
    auto it = idx.entries.find(key);
    if (it == idx.entries.end() || it->second.mtime != mtime || it->second.size != size) {
      detail::TreeIndex::Entry ne;
      ne.mtime = mtime;
      ne.size = size;
      if (unbudgeted) {
        seen.insert(key);
        queue_miss(Miss{std::move(key), e.path(), std::move(ne)});
        return;
      }
      if (!out_of_time && std::chrono::duration<double, std::milli>(clock::now() - t0).count() > badge_budget_ms)
        out_of_time = true;
      if (out_of_time) {
        if (it != idx.entries.end()) seen.insert(std::move(key));  // stale: keep until read
        return;
      }
      const auto r0 = clock::now();
      read_badge(e.path(), head, ne);
      if (std::chrono::duration<double, std::milli>(clock::now() - r0).count() > kSlowReadMs)
        out_of_time = true;  // cold files (antivirus): the rest go to tree(-1)
      idx.set(key, std::move(ne));
      it = idx.entries.find(key);
    }
    child.badge = true;
    child.kind = it->second.kind;
    child.method = it->second.method;
    seen.insert(std::move(key));
  });
  if (seen.size() != idx.entries.size()) {  // deleted or renamed files
    std::vector<std::string> gone;
    for (const auto& kv : idx.entries)
      if (!seen.count(kv.first)) gone.push_back(kv.first);
    for (const auto& k : gone) idx.erase(k);
  }
  if (!misses.empty()) {
    lock.unlock();
    {
      std::lock_guard<std::mutex> ql(qmu);
      listed = true;
    }
    qcv.notify_all();
    // This thread reads too; more readers join while the files turn out to be cold.
    for (std::string buf;;) {
      Miss* m;
      size_t left;
      {
        std::lock_guard<std::mutex> ql(qmu);
        if (qnext >= misses.size()) break;
        m = &misses[qnext++];
        left = misses.size() - qnext;
      }
      grow(left);
      const auto r0 = clock::now();
      read_badge(m->path, buf, m->entry);
      note_read(r0);
    }
    for (auto& th : pool) th.join();
    lock.lock();
    std::map<std::string, const detail::TreeIndex::Entry*> got;
    for (auto& m : misses) {
      auto it = idx.entries.find(m.key);
      // A save while the lock was released (note_saved) is newer than what was read here.
      if (it == idx.entries.end() || it->second.mtime <= m.entry.mtime) {
        idx.set(m.key, std::move(m.entry));
        it = idx.entries.find(m.key);
      }
      got.emplace(m.key, &it->second);
    }
    // Fill the badges of the nodes that were waiting for these reads.
    auto fill = [&](auto& self, CollectionNode& n, const std::string& prefix) -> void {
      for (auto& c : n.children) {
        const std::string key = prefix + c.name + (c.is_folder ? "" : kReqExt);
        if (c.is_folder) {
          self(self, c, key + "/");
        } else if (!c.badge) {
          auto g = got.find(key);
          if (g == got.end()) continue;
          c.badge = true;
          c.kind = g->second->kind;
          c.method = g->second->method;
        }
      }
    };
    fill(fill, node, std::string());
  }
  idx.flush();
  return node;
}

void Workspace::note_saved(const fs::path& file, const SavedRequest& r) const {
  if (!is_inside(root_, file)) return;
  std::error_code ec;
  detail::TreeIndex::Entry e;
  e.mtime = mtime_of(fs::last_write_time(file, ec));
  if (ec) return;
  e.size = fs::file_size(file, ec);
  if (ec) return;
  e.kind = r.kind;
  e.method = r.kind == RequestKind::Http ? r.http.method : "";
  detail::TreeIndex& idx = tree_index();
  std::lock_guard<std::mutex> lock(idx.mu);
  idx.set(generic_utf8(file.lexically_relative(root_)), std::move(e));
  idx.flush();  // one journal line now: survives a crash, and no rewrite of index.json
}

namespace {
bool is_remote_or_device_path(const fs::path& p);
bool is_ambiguous_rooted_path(const fs::path& p);
fs::path resolved_path(const fs::path& p);

// A request file must live inside the workspace. Paths can come from untrusted places (a
// session or history file committed to a cloned repo), so judge the text first (no UNC or
// device path is ever touched: an SMB open leaks the NTLM hash), then the resolved path
// (a junction inside the workspace must not lead out of it).
bool request_path_inside(const fs::path& root, const fs::path& p, std::string* err) {
  auto refuse = [&] {
    if (err) *err = to_utf8(p) + " is not inside the workspace " + to_utf8(root);
    return false;
  };
  if (is_remote_or_device_path(p) || is_ambiguous_rooted_path(p) || !is_inside(root, p)) return refuse();
  const fs::path rr = resolved_path(root);
  if (!is_inside(rr, resolved_path(p))) return refuse();
  return true;
}
}  // namespace

bool Workspace::save_request(SavedRequest& r, std::string* err) {
  if (r.id.empty()) r.id = new_uuid();
  if (r.name.empty()) r.name = "Untitled";
  std::error_code ec;
  fs::path target = r.file;
  if (target.empty()) {
    const std::string base = sanitize_name(r.name);
    for (int i = 1; i < 10000; ++i) {
      fs::path cand = root_ / from_utf8((i == 1 ? base : base + " " + std::to_string(i)) + kReqExt);
      if (!fs::exists(cand, ec)) {
        target = cand;
        break;
      }
      auto existing = load_request(cand);
      if (existing && existing->id == r.id) {
        target = cand;
        break;
      }
    }
    if (target.empty()) {
      if (err) *err = "no free file name for " + r.name;
      return false;
    }
  } else if (target.is_relative() && !is_ambiguous_rooted_path(target)) {
    target = root_ / target;
  }
  target = target.lexically_normal();
  if (!request_path_inside(root_, target, err)) return false;
  detail::ensure_dir(target.parent_path());

  // Form files inside the workspace are stored relative to its root (portable through git).
  for (auto& p : r.form) {
    if (p.kind == FormPart::Kind::Text || p.value.find("{{") != std::string::npos) continue;
    const fs::path fp = from_utf8(p.value);
    if (fp.is_absolute() && is_inside(root_, fp)) p.value = generic_utf8(fp.lexically_normal().lexically_relative(root_));
  }
  auto relative = [&](std::string& ref) {
    if (ref.empty() || ref.find("{{") != std::string::npos) return;
    const fs::path fp = from_utf8(ref);
    if (fp.is_absolute() && is_inside(root_, fp)) ref = generic_utf8(fp.lexically_normal().lexically_relative(root_));
  };
  relative(r.body_file);
  for (auto& f : r.header_files) relative(f);
  for (auto& f : r.cookie_files) relative(f);
  for (auto& d : r.body_data) {  // the file part of "@file" / "name@file"
    std::string f = curl_data_file(d);
    if (f.empty()) continue;
    const std::string head = d.value.substr(0, d.value.size() - f.size());
    relative(f);
    d.value = head + f;
  }

  rapidjson::Document d;
  const std::string text = detail::json_pretty(request_value(r, d.GetAllocator(), true, SIZE_MAX));
  // Unchanged content: do not touch the file (keeps mtime, no spurious git/editor reloads).
  bool wrote = false;
  if (!write_if_changed(target, text, err, &wrote, detail::Prescan::Background)) return false;
  if (wrote) note_saved(target, r);
  r.file = target;
  return true;
}

std::optional<SavedRequest> Workspace::load_request(const fs::path& file, std::string* err) const {
  const fs::path p = (file.is_relative() && !is_ambiguous_rooted_path(file) ? root_ / file : file).lexically_normal();
  if (!request_path_inside(root_, p, err)) return std::nullopt;
  std::string text;
  if (!detail::read_file(p, text)) {
    if (err) *err = "cannot read " + to_utf8(p);
    return std::nullopt;
  }
  rapidjson::Document d;
  d.Parse(text.data(), text.size());
  if (d.HasParseError() || !d.IsObject()) {
    if (err) *err = d.HasParseError() ? parse_error_text(d, p) : to_utf8(p) + ": not a JSON object";
    return std::nullopt;
  }
  SavedRequest r;
  request_from_value(d, r);
  r.file = p;
  if (r.name.empty()) {
    std::string n = to_utf8(p.filename());
    if (ends_with(n, kReqExt)) n.resize(n.size() - std::char_traits<char>::length(kReqExt));
    r.name = n;
  }
  return r;
}

// ------------------------------------------------------------------ folder / collection settings

namespace detail {

struct LoadedFolder {
  FolderSettings s;
  std::map<std::string, std::string> vars;  // the enabled vars (the first row of a name wins)
  std::string error;                        // why the file cannot be used ("" = it can)
};

// folder.json files by path: parsed once, re-read when the file's mtime or size changed. A
// send asks for the same few files many times (blocked_*(), http_to_send(), scripts, asserts),
// and each stat costs 5-30 us on Windows (antivirus filters), so a file's stat result (also
// "there is none") is trusted for kRecheck before the next stat. save_folder_settings() drops
// its entry, open() and tree() expire them all: edits made outside watt (git pull, an editor)
// are seen on the next refresh, or kRecheck later at most.
struct FolderCache {
  static constexpr std::chrono::milliseconds kRecheck{100};
  struct Entry {
    std::chrono::steady_clock::time_point checked_at{};
    bool fresh = false;  // checked_at is valid (cleared by expire())
    bool exists = false;
    int64_t mtime = 0;
    uint64_t size = 0;
    std::shared_ptr<const LoadedFolder> data;  // nullptr: no file
  };
  std::mutex mu;
  std::map<fs::path, Entry> entries;
  void expire() {
    std::lock_guard<std::mutex> lock(mu);
    for (auto& [k, e] : entries) e.fresh = false;
  }
};

void expire_folder_cache(FolderCache& fc) { fc.expire(); }

}  // namespace detail

struct Workspace::FolderChain {
  struct Level {
    SettingsOrigin origin;
    std::shared_ptr<const detail::LoadedFolder> data;
  };
  std::vector<Level> levels;  // outermost (the collection) first; only valid, non-empty files
  std::vector<std::string> errors;
  const Level* collection() const {
    return !levels.empty() && levels[0].origin.level == SettingsOrigin::Level::Collection ? &levels[0] : nullptr;
  }
};

namespace {

SettingsOrigin collection_origin(const fs::path& root) {
  return SettingsOrigin{SettingsOrigin::Level::Collection, root, "Collection"};
}

// The folder.json of `dir` (nullptr when there is none). folder.json comes from the workspace,
// maybe a cloned repository: it must be a plain file (no link) inside the workspace once
// resolved, at most kFolderFileMax bytes and valid JSON; otherwise the result carries `error`.
std::shared_ptr<const detail::LoadedFolder> load_folder_file(detail::FolderCache& fc, const fs::path& root,
                                                             const fs::path& dir) {
  if (root.empty()) return nullptr;  // no workspace (a default-constructed one): never the cwd
  const fs::path file = dir / kFolderFile;
  using clock = std::chrono::steady_clock;
  const auto now = clock::now();
  {
    std::lock_guard<std::mutex> lock(fc.mu);
    auto it = fc.entries.find(file);
    if (it != fc.entries.end() && it->second.fresh && now - it->second.checked_at < detail::FolderCache::kRecheck)
      return it->second.data;
  }
  auto remember = [&](bool exists, int64_t mtime, uint64_t size, std::shared_ptr<const detail::LoadedFolder> d) {
    std::lock_guard<std::mutex> lock(fc.mu);
    fc.entries.insert_or_assign(file, detail::FolderCache::Entry{now, true, exists, mtime, size, d});
    return d;
  };
  detail::PathStat st;
  if (!detail::stat_path(file, st)) return remember(false, 0, 0, nullptr);
  auto fail = [&](const std::string& why) {
    auto d = std::make_shared<detail::LoadedFolder>();
    d->error = to_utf8(file) + ": " + why;
    return std::shared_ptr<const detail::LoadedFolder>(std::move(d));
  };
  const uint64_t size = st.size;
  const int64_t mtime = st.mtime;
  if (st.is_link) return remember(true, mtime, size, fail("is a link: folder settings must be a plain file inside the workspace"));
  if (st.is_dir) return remember(true, mtime, size, fail("is not a file"));
  {
    std::lock_guard<std::mutex> lock(fc.mu);
    auto it = fc.entries.find(file);
    if (it != fc.entries.end() && it->second.exists && it->second.data && it->second.mtime == mtime &&
        it->second.size == size) {
      it->second.checked_at = now;
      it->second.fresh = true;
      return it->second.data;
    }
  }
  std::shared_ptr<const detail::LoadedFolder> out;
  std::string text, why;
  if (!request_path_inside(root, file, &why)) {
    out = fail("is not inside the workspace");
  } else if (size > kFolderFileMax) {
    out = fail("is larger than " + std::to_string(kFolderFileMax >> 20) + " MiB");
  } else if (!detail::read_file(file, text)) {
    out = fail("cannot be read");
  } else {
    rapidjson::Document d;
    d.Parse(text.data(), text.size());
    if (d.HasParseError() || !d.IsObject()) {
      auto e = std::make_shared<detail::LoadedFolder>();
      e->error = d.HasParseError() ? parse_error_text(d, file) : to_utf8(file) + ": not a JSON object";
      out = std::move(e);
    } else {
      auto l = std::make_shared<detail::LoadedFolder>();
      folder_from_value(d, l->s);
      for (const auto& v : l->s.vars)
        if (v.enabled) l->vars.emplace(v.name, v.value);
      out = std::move(l);
    }
  }
  return remember(true, mtime, size, std::move(out));
}

bool has_ctl_break(const std::string& s) {
  return std::any_of(s.begin(), s.end(), [](char c) { return c == '\r' || c == '\n' || c == '\0'; });
}

}  // namespace

detail::FolderCache& Workspace::folder_cache() const {
  if (!folder_cache_) folder_cache_ = shared_for_root<detail::FolderCache>(root_);
  return *folder_cache_;
}

Workspace::FolderChain Workspace::folder_chain(const SavedRequest& r) const {
  FolderChain c;
  std::vector<fs::path> dirs{root_};
  if (!r.file.empty()) {
    const fs::path f =
        (r.file.is_relative() && !is_ambiguous_rooted_path(r.file) ? root_ / r.file : r.file).lexically_normal();
    // Judged on the text (no UNC path is touched); each folder.json found is checked resolved.
    if (!is_remote_or_device_path(f) && is_inside(root_, f)) {
      fs::path d = root_;
      for (const auto& part : f.parent_path().lexically_relative(root_)) {
        if (part.empty() || part == "." || dirs.size() > 32) continue;
        d /= part;
        dirs.push_back(d);
      }
    }
  }
  detail::FolderCache& fc = folder_cache();
  for (size_t i = 0; i < dirs.size(); ++i) {
    auto d = load_folder_file(fc, root_, dirs[i]);
    if (!d) continue;
    if (!d->error.empty()) {
      c.errors.push_back(d->error);
      continue;
    }
    if (d->s.empty()) continue;
    FolderChain::Level l;
    l.data = std::move(d);
    l.origin = i == 0 ? collection_origin(root_)
                      : SettingsOrigin{SettingsOrigin::Level::Folder, dirs[i], generic_utf8(dirs[i].lexically_relative(root_))};
    c.levels.push_back(std::move(l));
  }
  return c;
}

namespace {

// CA, client certificate and proxy as they apply: the request first, then its folders nearest
// first, then the collection; each part from the first level that sets it.
struct NetworkResolved {
  NetworkSettings n;
  SettingsOrigin ca_from, cert_from, proxy_from;
};

NetworkResolved resolve_network(const NetworkSettings& own,
                                const std::vector<std::pair<const NetworkSettings*, SettingsOrigin>>& outer,
                                const fs::path& root) {
  NetworkResolved out;
  const SettingsOrigin def{SettingsOrigin::Level::Collection, root, ""};
  out.ca_from = out.cert_from = out.proxy_from = def;
  out.n.proxy = NetworkSettings::Proxy::Env;
  bool ca = false, cert = false, proxy = false;
  auto take = [&](const NetworkSettings& s, const SettingsOrigin& o) {
    if (!ca && (s.ca_off || !s.ca_file.empty())) {
      ca = true;
      out.n.ca_off = s.ca_off;
      out.n.ca_file = s.ca_off ? std::string() : s.ca_file;
      out.ca_from = o;
    }
    if (!cert && (s.cert_off || !s.cert_file.empty())) {
      cert = true;
      out.n.cert_off = s.cert_off;
      out.n.cert_file = s.cert_off ? std::string() : s.cert_file;
      out.n.key_file = s.cert_off ? std::string() : s.key_file;
      out.cert_from = o;
    }
    if (!proxy && s.proxy != NetworkSettings::Proxy::Inherit) {
      proxy = true;
      out.n.proxy = s.proxy;
      out.n.proxy_url = s.proxy == NetworkSettings::Proxy::Url ? s.proxy_url : std::string();
      out.n.no_proxy = s.no_proxy;
      out.proxy_from = o;
    }
  };
  take(own, SettingsOrigin{SettingsOrigin::Level::Request, {}, "Request"});
  for (const auto& [s, o] : outer) take(*s, o);
  return out;
}

}  // namespace

std::string Workspace::passphrase_key(const SettingsOrigin& level, const std::string& request_id) {
  switch (level.level) {
    case SettingsOrigin::Level::Request: return "request:" + request_id;
    case SettingsOrigin::Level::Collection: return "";
    default: return level.label;
  }
}

SavedRequest Workspace::resolve_with(const SavedRequest& r, const FolderChain& c) const {
  if (r.settings_merged) return r;
  SavedRequest x = r;
  x.settings_merged = true;
  if (r.kind == RequestKind::Http) {
    std::vector<std::pair<const NetworkSettings*, SettingsOrigin>> outer;
    for (size_t i = c.levels.size(); i-- > 0;) outer.emplace_back(&c.levels[i].data->s.network, c.levels[i].origin);
    NetworkResolved nr = resolve_network(r.network, outer, root_);
    x.network = std::move(nr.n);
    x.cert_from_key = passphrase_key(nr.cert_from, r.id);
  }
  // Auth: Inherit walks up to the nearest level that decides (anything but Inherit).
  if (x.auth.type == HttpAuth::Type::Inherit) {
    x.auth = HttpAuth{};
    x.basic_auth = BasicAuth{};
    for (size_t i = c.levels.size(); i-- > 0;) {
      const FolderSettings& s = c.levels[i].data->s;
      if (s.auth.type == HttpAuth::Type::Inherit) continue;
      x.auth = s.auth;
      x.basic_auth = s.auth.type == HttpAuth::Type::None ? s.basic_auth : BasicAuth{};
      break;
    }
  }
  if (c.levels.empty()) return x;
  // Headers (gRPC: metadata): collection, folders outermost first, then the request's own. A
  // name set (enabled) at a nearer level replaces the inherited one; disabled rows are not sent.
  const bool grpc = r.kind == RequestKind::Grpc;
  std::vector<Header>& own = grpc ? x.grpc_opts.metadata : x.http.headers;
  std::set<std::string> taken;
  for (size_t k = 0; k < own.size(); ++k)
    if (grpc || !x.disabled_headers.count(k)) taken.insert(lower_ascii(own[k].first));
  std::vector<std::vector<Header>> per_level(c.levels.size());
  for (size_t i = c.levels.size(); i-- > 0;) {
    std::vector<std::string> here;
    for (const auto& row : c.levels[i].data->s.headers) {
      if (!row.enabled || row.name.empty()) continue;
      std::string l = lower_ascii(row.name);
      if (taken.count(l)) continue;
      per_level[i].emplace_back(row.name, row.value);
      here.push_back(std::move(l));
    }
    taken.insert(here.begin(), here.end());
  }
  std::vector<Header> merged;
  for (auto& l : per_level)
    for (auto& h : l) merged.push_back(std::move(h));
  const size_t n = merged.size();
  if (n) {
    merged.insert(merged.end(), std::make_move_iterator(own.begin()), std::make_move_iterator(own.end()));
    own = std::move(merged);
    if (grpc) {
      x.inherited_metadata = n;
    } else {
      x.inherited_headers = n;
      std::set<size_t> shifted;
      for (size_t k : x.disabled_headers) shifted.insert(k + n);
      x.disabled_headers = std::move(shifted);
    }
  }
  // Asserts: the request's own first (their indexes stay), then the inherited ones.
  for (const auto& l : c.levels)
    x.asserts.insert(x.asserts.end(), l.data->s.asserts.begin(), l.data->s.asserts.end());
  return x;
}

std::optional<FolderSettings> Workspace::folder_settings(const fs::path& folder, std::string* err) const {
  const fs::path file =
      ((folder.is_relative() && !is_ambiguous_rooted_path(folder) ? root_ / folder : folder) / kFolderFile).lexically_normal();
  if (!request_path_inside(root_, file, err)) return std::nullopt;
  auto d = load_folder_file(folder_cache(), root_, file.parent_path());
  if (!d) return FolderSettings{};
  if (!d->error.empty()) {
    if (err) *err = d->error;
    return std::nullopt;
  }
  return d->s;
}

FolderSettings Workspace::collection_settings() const {
  auto s = folder_settings(root_);
  return s ? std::move(*s) : FolderSettings{};
}

bool Workspace::save_folder_settings(const fs::path& folder, const FolderSettings& s, std::string* err) const {
  const fs::path file =
      ((folder.is_relative() && !is_ambiguous_rooted_path(folder) ? root_ / folder : folder) / kFolderFile).lexically_normal();
  if (!request_path_inside(root_, file, err)) return false;
  std::error_code ec;
  if (fs::is_symlink(file, ec)) {
    if (err) *err = to_utf8(file) + " is a link: not written";
    return false;
  }
  rapidjson::Document d;
  const Value v = folder_value(s, d.GetAllocator());
  bool ok = true;
  if (v.MemberCount() <= 1) {  // nothing set: no file (a workspace that does not use it never has one)
    if (fs::exists(file, ec)) {
      fs::remove(file, ec);
      if (ec) {
        if (err) *err = "cannot remove " + to_utf8(file) + ": " + ec.message();
        ok = false;
      }
    }
  } else {
    ok = write_if_changed(file, detail::json_pretty(v), err);
  }
  detail::FolderCache& fc = folder_cache();
  std::lock_guard<std::mutex> lock(fc.mu);
  fc.entries.erase(file);  // read again next time (a rewrite within the same mtime tick and size)
  return ok;
}

SavedRequest Workspace::new_request(RequestKind kind, const std::string& name, const fs::path& folder) const {
  SavedRequest r;
  r.id = new_uuid();
  r.name = name.empty() ? "New Request" : name;
  r.kind = kind;
  r.auth.type = HttpAuth::Type::Inherit;
  if (!folder.empty()) {
    const fs::path dir = (folder.is_relative() && !is_ambiguous_rooted_path(folder) ? root_ / folder : folder).lexically_normal();
    const std::string base = sanitize_name(r.name);
    std::error_code ec;
    for (int i = 1; i < 10000; ++i) {
      const fs::path cand = (dir / from_utf8((i == 1 ? base : base + " " + std::to_string(i)) + kReqExt)).lexically_normal();
      if (!request_path_inside(root_, cand, nullptr)) break;
      if (!fs::exists(cand, ec)) {
        r.file = cand;
        break;
      }
    }
  }
  return r;
}

bool Workspace::has_inherited_settings(const SavedRequest& r) const {
  const FolderChain c = folder_chain(r);
  return !c.levels.empty() || !c.errors.empty();
}

SavedRequest Workspace::resolved(const SavedRequest& r) const { return resolve_with(r, folder_chain(r)); }

EffectiveSettings Workspace::effective_settings(const SavedRequest& r) const {
  const FolderChain c = folder_chain(r);
  EffectiveSettings e;
  e.errors = c.errors;
  const SettingsOrigin req{SettingsOrigin::Level::Request, {}, "Request"};
  const bool grpc = r.kind == RequestKind::Grpc;
  // Headers in send order; overridden = an enabled row of the same name at a nearer level.
  for (const auto& l : c.levels)
    for (const auto& row : l.data->s.headers) e.headers.push_back({row, l.origin, false});
  const auto& own = grpc ? r.grpc_opts.metadata : r.http.headers;
  for (size_t k = 0; k < own.size(); ++k)
    e.headers.push_back({SettingsRow{own[k].first, own[k].second, {}, grpc || !r.disabled_headers.count(k)}, req, false});
  {
    // By level, nearest first: rows of one level never override each other.
    std::set<std::string> taken;
    size_t end = e.headers.size();
    while (end > 0) {
      size_t begin = end - 1;
      while (begin > 0 && e.headers[begin - 1].from == e.headers[end - 1].from) --begin;
      std::vector<std::string> here;
      for (size_t k = begin; k < end; ++k) {
        auto& h = e.headers[k];
        if (!h.row.enabled) continue;
        const std::string l = lower_ascii(h.row.name);
        h.overridden = h.from.level != SettingsOrigin::Level::Request && taken.count(l) > 0;
        if (!h.overridden) here.push_back(l);
      }
      taken.insert(here.begin(), here.end());
      end = begin;
    }
  }
  // Variables, highest precedence first: runtime, folders nearest first, (environment), collection.
  const Environment* env = nullptr;
  for (const auto& en : envs_)
    if (!active_env.empty() && en.name == active_env) env = &en;
  std::set<std::string> defined;
  for (const auto& [k, v] : runtime_vars) {
    e.vars.push_back({SettingsRow{k, v, {}, true}, SettingsOrigin{SettingsOrigin::Level::Request, {}, "Runtime"}, false});
    defined.insert(k);
  }
  for (size_t i = c.levels.size(); i-- > 0;) {
    const auto& l = c.levels[i];
    if (l.origin.level == SettingsOrigin::Level::Collection && env)
      for (const auto& [k, v] : env->vars) defined.insert(k);
    std::vector<std::string> here;
    for (const auto& row : l.data->s.vars) {
      const bool over = row.enabled && defined.count(row.name) > 0;
      e.vars.push_back({row, l.origin, over});
      if (row.enabled) here.push_back(row.name);
    }
    defined.insert(here.begin(), here.end());
  }
  // Auth.
  e.auth = r.auth;
  e.basic_auth = r.basic_auth;
  e.auth_from = req;
  if (r.auth.type == HttpAuth::Type::Inherit) {
    e.auth = HttpAuth{};
    e.basic_auth = BasicAuth{};
    e.auth_from = collection_origin(root_);
    for (size_t i = c.levels.size(); i-- > 0;) {
      const FolderSettings& s = c.levels[i].data->s;
      if (s.auth.type == HttpAuth::Type::Inherit) continue;
      e.auth = s.auth;
      e.basic_auth = s.auth.type == HttpAuth::Type::None ? s.basic_auth : BasicAuth{};
      e.auth_from = c.levels[i].origin;
      break;
    }
  }
  // Scripts: pre outermost first, post nearest first.
  for (const auto& l : c.levels)
    if (!l.data->s.pre_script.empty()) e.pre_scripts.push_back({l.data->s.pre_script, l.origin});
  if (!r.pre_script.empty()) e.pre_scripts.push_back({r.pre_script, req});
  if (!r.post_script.empty()) e.post_scripts.push_back({r.post_script, req});
  for (size_t i = c.levels.size(); i-- > 0;)
    if (!c.levels[i].data->s.post_script.empty()) e.post_scripts.push_back({c.levels[i].data->s.post_script, c.levels[i].origin});
  // CA / client certificate / proxy.
  if (!grpc) {
    std::vector<std::pair<const NetworkSettings*, SettingsOrigin>> outer;
    for (size_t i = c.levels.size(); i-- > 0;) outer.emplace_back(&c.levels[i].data->s.network, c.levels[i].origin);
    NetworkResolved nr = resolve_network(r.settings_merged ? NetworkSettings{} : r.network, outer, root_);
    e.network = std::move(nr.n);
    e.ca_from = nr.ca_from;
    e.cert_from = nr.cert_from;
    e.proxy_from = nr.proxy_from;
  }
  // Asserts: the request's own, then the inherited ones (as resolved()).
  for (const auto& a : r.asserts) e.asserts.push_back({a, req});
  for (const auto& l : c.levels)
    for (const auto& a : l.data->s.asserts) e.asserts.push_back({a, l.origin});
  return e;
}

bool Workspace::remove(const fs::path& p) {
  const fs::path abs = (p.is_relative() ? root_ / p : p).lexically_normal();
  if (!is_inside(root_, abs)) return false;  // never delete the root or anything outside it
  std::error_code ec;
  if (!fs::exists(abs, ec)) return false;
  const auto n = fs::remove_all(abs, ec);
  return !ec && n != static_cast<std::uintmax_t>(-1) && n > 0;
}

bool Workspace::rename(const fs::path& from, const std::string& new_name) {
  if (new_name.empty()) return false;
  const fs::path src = (from.is_relative() ? root_ / from : from).lexically_normal();
  if (!is_inside(root_, src)) return false;
  std::error_code ec;
  if (!fs::exists(src, ec)) return false;
  const bool is_req = !fs::is_directory(src, ec) && ends_with(to_utf8(src.filename()), kReqExt);
  const fs::path dst = src.parent_path() / from_utf8(sanitize_name(new_name) + (is_req ? kReqExt : ""));
  const bool same_file = dst == src || (fs::exists(dst, ec) && fs::equivalent(dst, src, ec));
  if (!same_file && fs::exists(dst, ec)) return false;  // never overwrite another request/folder
  if (is_req) {
    auto r = load_request(src);
    if (!r) return false;
    r->name = new_name;
    r->file = src;
    if (!save_request(*r)) return false;
  } else if (!fs::is_directory(src, ec)) {
    return false;
  }
  if (dst != src) {  // also handles case-only renames on case-insensitive file systems
    fs::rename(src, dst, ec);
    if (ec) return false;
  }
  return true;
}

bool Workspace::save_settings(std::string* err) const {
  // watt.json: shared settings only. Keys we do not know about (written by a newer watt or
  // by hand) are kept; the pre-split "environments" and "active_env" keys are dropped.
  const fs::path file = root_ / kSettingsFile;
  rapidjson::Document d;
  d.SetObject();
  auto& a = d.GetAllocator();
  d.AddMember("version", 1, a);
  Value g(rapidjson::kObjectType);
  for (const auto& [k, v] : globals) g.AddMember(jstr(k, a), jstr(v, a), a);
  d.AddMember("globals", g, a);
  rapidjson::Document od;
  if (parse_json_file(file, od))
    for (auto it = od.MemberBegin(); it != od.MemberEnd(); ++it) {
      const std::string_view k(it->name.GetString(), it->name.GetStringLength());
      if (k == "environments" || k == "active_env" || d.HasMember(it->name)) continue;
      d.AddMember(Value(it->name, a), Value(it->value, a), a);
    }
  if (!write_if_changed(file, detail::json_pretty(d), err)) return false;

  // environments/<name>.json, one per environment. Secret values are left out.
  std::error_code ec;
  const fs::path env_dir = root_ / kEnvDir;
  std::set<fs::path> written;
  rapidjson::Document sec;
  sec.SetObject();
  auto& sa = sec.GetAllocator();
  for (size_t i = 0; i < envs_.size(); ++i) {
    const Environment& e = envs_[i];
    rapidjson::Document ed;
    ed.SetObject();
    auto& ea = ed.GetAllocator();
    ed.AddMember("name", jstr(e.name, ea), ea);
    ed.AddMember("seq", static_cast<int64_t>(i + 1), ea);
    Value vars(rapidjson::kObjectType);
    Value secret_vals(rapidjson::kObjectType);
    for (const auto& [k, v] : e.vars) {
      if (!e.secrets.count(k)) {
        vars.AddMember(jstr(k, ea), jstr(v, ea), ea);
        continue;
      }
      // Sealed (DPAPI on Windows). An unchanged value keeps its ciphertext: no rewrite.
      auto& cached = sealed_[sealed_key(e.name, k)];
      if (cached.second.empty() || cached.first != v) {
        std::string stored;
        if (!detail::seal_secret(v, stored)) {
          if (err) *err = "cannot encrypt secret '" + k + "' of environment '" + e.name + "'";
          return false;
        }
        cached = {v, std::move(stored)};
      }
      secret_vals.AddMember(jstr(k, sa), jstr(cached.second, sa), sa);
    }
    // Values this account cannot decrypt are written back as they were (not wiped).
    for (const auto& [k, stored] : e.unreadable_secrets)
      if (e.secrets.count(k) && !e.vars.count(k)) secret_vals.AddMember(jstr(k, sa), jstr(stored, sa), sa);
    ed.AddMember("vars", vars, ea);
    if (!e.secrets.empty()) {
      Value names(rapidjson::kArrayType);
      for (const auto& s : e.secrets) names.PushBack(jstr(s, ea), ea);
      ed.AddMember("secrets", names, ea);
    }
    if (secret_vals.MemberCount() > 0 && !sec.HasMember(e.name.c_str()))
      sec.AddMember(jstr(e.name, sa), secret_vals, sa);
    const std::string stem = sanitize_name(e.name);
    fs::path target = env_dir / from_utf8(stem + ".json");
    for (int n = 2; written.count(target); ++n) target = env_dir / from_utf8(stem + " " + std::to_string(n) + ".json");
    written.insert(target);
    if (!write_if_changed(target, detail::json_pretty(ed), err)) return false;
  }
  // Removed (or renamed) environments.
  if (fs::is_directory(env_dir, ec)) {
    std::vector<fs::path> stale;
    for (fs::directory_iterator it(env_dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
      if (it->path().extension() == ".json" && !written.count(it->path())) stale.push_back(it->path());
    for (const auto& p : stale) fs::remove(p, ec);
  }

  // .watt/: the active environment and secret values (gitignored, personal).
  const fs::path priv = root_ / ".watt";
  rapidjson::Document st;
  st.SetObject();
  st.AddMember("active_env", jstr(active_env, st.GetAllocator()), st.GetAllocator());
  if (!active_env.empty() || fs::exists(priv / kStateFile, ec))
    if (!write_if_changed(priv / kStateFile, detail::json_pretty(st), err)) return false;
  // Client certificate passphrases (set_cert_passphrase) are kept as they are in the file.
  if (rapidjson::Document old; parse_json_file(priv / kSecretsFile, old))
    if (const Value* cp = get_obj(old, kCertPassKey); cp && !sec.HasMember(kCertPassKey))
      sec.AddMember(Value(kCertPassKey, sa), Value(*cp, sa), sa);
  if (sec.MemberCount() > 0 || fs::exists(priv / kSecretsFile, ec))
    if (!write_if_changed(priv / kSecretsFile, detail::json_pretty(sec), err)) return false;
  return true;
}

std::string Workspace::cert_passphrase(const std::string& key) const {
  rapidjson::Document d;
  if (!parse_json_file(root_ / ".watt" / kSecretsFile, d)) return {};
  const Value* cp = get_obj(d, kCertPassKey);
  if (!cp) return {};
  const auto it = cp->FindMember(Value(key.c_str(), static_cast<SizeType>(key.size())));
  if (it == cp->MemberEnd() || !it->value.IsString()) return {};
  std::string plain;
  const std::string stored(it->value.GetString(), it->value.GetStringLength());
  return detail::unseal_secret(stored, plain) == detail::Unseal::Failed ? std::string() : plain;
}

bool Workspace::set_cert_passphrase(const std::string& key, const std::string& passphrase, std::string* err) const {
  const fs::path priv = root_ / ".watt";
  rapidjson::Document d;
  if (!parse_json_file(priv / kSecretsFile, d)) d.SetObject();
  auto& a = d.GetAllocator();
  if (!d.HasMember(kCertPassKey)) {
    if (passphrase.empty()) return true;
    d.AddMember(Value(kCertPassKey, a), Value(rapidjson::kObjectType), a);
  }
  Value& cp = d[kCertPassKey];
  if (!cp.IsObject()) cp.SetObject();
  cp.RemoveMember(Value(key.c_str(), static_cast<SizeType>(key.size())));
  if (!passphrase.empty()) {
    std::string stored;
    if (!detail::seal_secret(passphrase, stored)) {
      if (err) *err = "cannot encrypt the certificate passphrase";
      return false;
    }
    cp.AddMember(jstr(key, a), jstr(stored, a), a);
  }
  if (cp.MemberCount() == 0) d.RemoveMember(kCertPassKey);
  std::error_code ec;
  if (!fs::is_directory(priv, ec)) detail::create_private_dir(priv);
  return write_if_changed(priv / kSecretsFile, detail::json_pretty(d), err);
}

// ------------------------------------------------------------------ history

namespace {

// SAX handler for a history line: fills the list fields and stops at "request".
struct HistoryListHandler : rapidjson::BaseReaderHandler<rapidjson::UTF8<>, HistoryListHandler> {
  enum Field { None, When, Kind, Summary, Status, Total };
  HistoryEntry* h = nullptr;
  int depth = 0;
  Field field = None;
  int seen = 0;          // list fields read
  bool at_request = false;

  bool Default() {
    field = None;
    return true;
  }
  bool StartObject() {
    ++depth;
    field = None;
    return true;
  }
  bool EndObject(SizeType) { return --depth >= 0; }
  bool StartArray() {
    ++depth;
    field = None;
    return true;
  }
  bool EndArray(SizeType) { return --depth >= 0; }
  bool Key(const char* s, SizeType n, bool) {
    field = None;
    if (depth != 1) return true;
    const std::string_view k(s, n);
    if (k == "request") {
      at_request = true;
      return false;  // stop: the list does not need it
    }
    if (k == "when") field = When;
    else if (k == "kind") field = Kind;
    else if (k == "summary") field = Summary;
    else if (k == "status") field = Status;
    else if (k == "total_ms") field = Total;
    return true;
  }
  bool String(const char* s, SizeType n, bool) {
    if (field == When) h->when_iso.assign(s, n), ++seen;
    else if (field == Summary) h->summary.assign(s, n), ++seen;
    else if (field == Kind) h->kind = std::string_view(s, n) == "grpc" ? RequestKind::Grpc : RequestKind::Http, ++seen;
    field = None;
    return true;
  }
  bool number(double v) {
    if (field == Status) h->status = static_cast<long>(v), ++seen;
    else if (field == Total) h->total_ms = v, ++seen;
    field = None;
    return true;
  }
  bool Int(int v) { return number(v); }
  bool Uint(unsigned v) { return number(v); }
  bool Int64(int64_t v) { return number(static_cast<double>(v)); }
  bool Uint64(uint64_t v) { return number(static_cast<double>(v)); }
  bool Double(double v) { return number(v); }
};

bool parse_history_line(rapidjson::Reader& reader, const char* p, size_t n, HistoryEntry& h) {
  HistoryListHandler hd;
  hd.h = &h;
  rapidjson::MemoryStream ms(p, n);
  rapidjson::EncodedInputStream<rapidjson::UTF8<>, rapidjson::MemoryStream> is(ms);
  const rapidjson::ParseResult res = reader.Parse<rapidjson::kParseStopWhenDoneFlag>(is, hd);
  if (res) return true;
  if (!hd.at_request) return false;  // corrupt line (a crash mid-append): skip it
  if (hd.seen >= 5) return true;
  // Fields after "request" (not our writer's order): parse the whole line.
  rapidjson::Document d;
  d.Parse(p, n);
  if (d.HasParseError() || !d.IsObject()) return false;
  h.when_iso = get_str(d, "when");
  h.kind = get_str(d, "kind") == "grpc" ? RequestKind::Grpc : RequestKind::Http;
  h.summary = get_str(d, "summary");
  h.status = get_long(d, "status", 0);
  h.total_ms = get_num(d, "total_ms", 0);
  return true;
}

// "req": [offset, length] of the line's request record in its segment's .req.jsonl, if any.
bool history_req_ref(const Value& line, uint64_t& off, uint64_t& len) {
  const Value* ref = get_arr(line, "req");
  if (!ref || ref->Size() != 2 || !(*ref)[0].IsUint64() || !(*ref)[1].IsUint64()) return false;
  off = (*ref)[0].GetUint64();
  len = (*ref)[1].GetUint64();
  return true;
}

// HistoryEntry::offset: segment number in the high bits, byte offset in its list file below.
constexpr int kSegShift = 40;
constexpr uint64_t kSegOffMask = (uint64_t{1} << kSegShift) - 1;

fs::path history_dir(const fs::path& root) { return root / ".watt" / "history"; }
// Segment 0 is the pre-segment layout.
fs::path seg_list_path(const fs::path& root, uint32_t seq) {
  if (seq == 0) return root / ".watt" / "history.jsonl";
  char name[32];
  std::snprintf(name, sizeof name, "%08u.jsonl", seq);
  return history_dir(root) / name;
}
fs::path seg_recs_path(const fs::path& root, uint32_t seq) {
  if (seq == 0) return root / ".watt" / "history.req.jsonl";
  char name[32];
  std::snprintf(name, sizeof name, "%08u.req.jsonl", seq);
  return history_dir(root) / name;
}

// Segments on disk, oldest first (0 first if the old single-file history is still there).
std::vector<uint32_t> list_segments(const fs::path& root) {
  std::vector<uint32_t> out;
  std::error_code ec;
  if (fs::exists(seg_list_path(root, 0), ec)) out.push_back(0);
  for (fs::directory_iterator it(history_dir(root), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
    const auto n = it->path().filename().native();
    if (n.size() != 14) continue;
    uint32_t seq = 0;
    bool ok = n[8] == '.' && n[9] == 'j' && n[10] == 's' && n[11] == 'o' && n[12] == 'n' && n[13] == 'l';
    for (size_t i = 0; i < 8 && ok; ++i) {
      ok = n[i] >= '0' && n[i] <= '9';
      seq = seq * 10 + static_cast<uint32_t>(n[i] - '0');
    }
    if (ok && seq > 0) out.push_back(seq);
  }
  std::sort(out.begin(), out.end());
  return out;
}

detail::HistorySegment& segment(detail::HistoryHandles& hf, uint32_t seq) {
  auto& s = hf.segs[seq];
  if (!s) s = std::make_unique<detail::HistorySegment>();
  return *s;
}

// The segment's list file, opened (created if `create`). nullptr if it does not exist.
detail::HistoryFile* seg_list(detail::HistoryHandles& hf, const fs::path& root, uint32_t seq, bool create) {
  detail::HistorySegment& s = segment(hf, seq);
  if (!s.list.is_open() || create) s.list.open(seg_list_path(root, seq), create, create);
  return s.list.is_open() ? &s.list : nullptr;
}

detail::HistoryFile* seg_recs(detail::HistoryHandles& hf, const fs::path& root, uint32_t seq, bool create) {
  detail::HistorySegment& s = segment(hf, seq);
  if (!s.recs.is_open() || create) s.recs.open(seg_recs_path(root, seq), create, create);
  return s.recs.is_open() ? &s.recs : nullptr;
}

void delete_segment(detail::HistoryHandles& hf, const fs::path& root, uint32_t seq) {
  hf.segs.erase(seq);  // closes our handles first
  std::error_code ec;
  fs::remove(seg_list_path(root, seq), ec);
  fs::remove(seg_recs_path(root, seq), ec);
}

std::string read_all(const detail::HistoryFile& f) {
  std::string all;
  f.read_at(0, static_cast<size_t>(f.size()), all);
  return all;
}

size_t count_lines(const std::string& s) { return static_cast<size_t>(std::count(s.begin(), s.end(), '\n')); }

// Up to `need` entries of one segment's list, newest first, appended to `out`. Reads just
// enough of the tail: 256 KiB first, then (from the average line length so far) enough for the
// lines still needed, growing at least 4x per step.
void read_segment(const detail::HistoryFile& f, uint32_t seq, size_t need, std::vector<HistoryEntry>& out) {
  const uint64_t size = f.size();
  if (size == 0 || need == 0) return;
  std::string buf;
  size_t first = 0;  // start of the first complete line in buf
  size_t lines = 0;
  uint64_t start = 0;  // file offset of buf[0]
  for (uint64_t chunk = 256u << 10;;) {
    const uint64_t n = std::min(size, chunk);
    start = size - n;
    f.read_at(start, static_cast<size_t>(n), buf);
    first = 0;
    if (n < size) {
      const size_t nl = buf.find('\n');
      if (nl == std::string::npos) {
        chunk *= 4;
        continue;
      }
      first = nl + 1;
    }
    lines = static_cast<size_t>(std::count(buf.begin() + static_cast<std::ptrdiff_t>(first), buf.end(), '\n'));
    if (n >= size || lines >= need + 1) break;
    const double avg = static_cast<double>(buf.size() - first) / static_cast<double>(std::max<size_t>(lines, 1));
    chunk = std::max<uint64_t>(chunk * 4, static_cast<uint64_t>(avg * 1.25 * static_cast<double>(need + 1)));
  }
  // The list needs only the small fields our writer puts first: a SAX pass stops at an inline
  // "request" (older lines), so a request object is never parsed. A line written in a different
  // key order falls back to a DOM parse of that line.
  rapidjson::Reader reader;
  const uint64_t base = static_cast<uint64_t>(seq) << kSegShift;
  size_t end = buf.size(), got = 0;
  while (end > first && got < need) {
    size_t line_end = end;
    if (buf[line_end - 1] == '\n') --line_end;
    size_t line_start = line_end;
    while (line_start > first && buf[line_start - 1] != '\n') --line_start;
    end = line_start;
    if (line_end > line_start && buf[line_end - 1] == '\r') --line_end;
    if (line_end <= line_start) continue;
    HistoryEntry h;
    if (parse_history_line(reader, buf.data() + line_start, line_end - line_start, h)) {
      h.offset = base | (start + line_start);
      out.push_back(std::move(h));
      ++got;
    }
  }
}

// One-time move of the single-file history (segment 0) into segments 1, 2, ...: the newest
// kHistoryKeepEntries entries, records copied as they are and lines with an inline request
// split into line + record. Only when no segment exists yet (another process may have done it).
void migrate_single_file(detail::HistoryHandles& hf, const fs::path& root) {
  const std::vector<uint32_t> segs = list_segments(root);
  if (segs.size() != 1 || segs.front() != 0) return;
  detail::HistoryFile* list = seg_list(hf, root, 0, false);
  if (!list) return;
  const std::string all = read_all(*list);
  std::string recs;
  if (detail::HistoryFile* rf = seg_recs(hf, root, 0, false)) recs = read_all(*rf);
  std::vector<std::pair<size_t, size_t>> lines;  // [start, end) of each non-empty line
  for (size_t p = 0; p < all.size();) {
    size_t e = all.find('\n', p);
    if (e == std::string::npos) e = all.size();
    size_t le = e;
    if (le > p && all[le - 1] == '\r') --le;
    if (le > p) lines.emplace_back(p, le);
    p = e + 1;
  }
  if (lines.size() > kHistoryKeepEntries)
    lines.erase(lines.begin(), lines.end() - static_cast<std::ptrdiff_t>(kHistoryKeepEntries));
  std::error_code ec;
  detail::ensure_dir(history_dir(root));
  uint32_t seq = 1;
  std::string out_list, out_recs;
  size_t in_seg = 0;
  auto write_seg = [&]() -> bool {
    if (in_seg == 0) return true;
    if (!detail::write_file_atomic(seg_recs_path(root, seq), out_recs) ||
        !detail::write_file_atomic(seg_list_path(root, seq), out_list))
      return false;
    ++seq;
    out_list.clear();
    out_recs.clear();
    in_seg = 0;
    return true;
  };
  for (const auto& [b, e] : lines) {
    rapidjson::Document d;
    d.Parse(all.data() + b, e - b);
    if (d.HasParseError() || !d.IsObject()) continue;  // torn line: drop it
    uint64_t off = 0, len = 0;
    std::string rec;
    if (history_req_ref(d, off, len)) {
      if (off + len > recs.size()) continue;
      rec.assign(recs, static_cast<size_t>(off), static_cast<size_t>(len));
    } else if (d.HasMember("request")) {
      rapidjson::Document rd;
      rd.SetObject();
      auto& ra = rd.GetAllocator();
      rd.AddMember("when", jstr(get_str(d, "when"), ra), ra);
      rd.AddMember("summary", jstr(get_str(d, "summary"), ra), ra);
      rd.AddMember("request", Value(d["request"], ra), ra);
      rec = detail::json_compact(rd);
      d.RemoveMember("request");
    }
    if (!rec.empty()) {
      auto& a = d.GetAllocator();
      d.RemoveMember("req");
      Value ref(rapidjson::kArrayType);
      ref.PushBack(Value(static_cast<uint64_t>(out_recs.size())), a).PushBack(Value(static_cast<uint64_t>(rec.size())), a);
      d.AddMember("req", ref, a);
      out_recs += rec;
      out_recs += '\n';
    }
    out_list += detail::json_compact(d);
    out_list += '\n';
    if (++in_seg >= kHistorySegEntries && !write_seg()) return;
  }
  if (!write_seg()) return;
  delete_segment(hf, root, 0);
  for (uint32_t s = 1; s < seq; ++s) {
    detail::prescan_later(seg_list_path(root, s));
    detail::prescan_later(seg_recs_path(root, s), detail::Prescan::Background);
  }
}

// Keeps the newest segments that together hold kHistoryKeepEntries entries (and at most about
// kHistoryKeepBytes of records); older segments are deleted whole, nothing is rewritten.
void trim_segments(detail::HistoryHandles& hf, const fs::path& root) {
  const std::vector<uint32_t> segs = list_segments(root);
  if (segs.size() <= 1) return;
  size_t entries = 0;
  uint64_t bytes = 0;
  size_t keep = 0;  // newest segments kept
  for (size_t i = segs.size(); i-- > 0;) {
    if (entries >= kHistoryKeepEntries || (keep > 0 && bytes > kHistoryKeepBytes)) break;
    ++keep;
    const uint32_t seq = segs[i];
    if (seq == hf.head && hf.head_count >= 0) {
      entries += static_cast<size_t>(hf.head_count);
    } else if (detail::HistoryFile* l = seg_list(hf, root, seq, false)) {
      entries += count_lines(read_all(*l));
    }
    std::error_code ec;
    const auto rs = fs::file_size(seg_recs_path(root, seq), ec);
    if (!ec) bytes += rs;
  }
  for (size_t i = 0; i + keep < segs.size(); ++i) delete_segment(hf, root, segs[i]);
}

}  // namespace

namespace {

std::mutex& history_mutex(std::shared_ptr<detail::HistoryHandles>& h, const fs::path& root) {
  if (!h) h = shared_for_root<detail::HistoryHandles>(root);
  return h->mu;
}

}  // namespace

detail::HistoryHandles& Workspace::history_handles() const {
  if (!hist_) hist_ = shared_for_root<detail::HistoryHandles>(root_);
  return *hist_;
}

void Workspace::append_history(const HistoryEntry& e) {
  rapidjson::Document d;
  d.SetObject();
  auto& a = d.GetAllocator();
  d.AddMember("when", jstr(e.when_iso.empty() ? now_iso() : e.when_iso, a), a);
  d.AddMember("kind", Value(rapidjson::StringRef(e.kind == RequestKind::Grpc ? "grpc" : "http")), a);
  d.AddMember("summary", jstr(e.summary, a), a);
  d.AddMember("status", Value(static_cast<int64_t>(e.status)), a);
  d.AddMember("total_ms", e.total_ms, a);

  // The request goes to the segment's .req.jsonl; the list line only points at it, so listing
  // 10k entries reads ~1.5 MB whatever the bodies are.
  const std::string when = get_str(d, "when");
  rapidjson::Document rd;
  rd.SetObject();
  auto& ra = rd.GetAllocator();
  rd.AddMember("when", jstr(when, ra), ra);
  rd.AddMember("summary", jstr(e.summary, ra), ra);
  rd.AddMember("request", request_value(e.request, ra, false, kHistoryBodyCap), ra);
  std::string rec = detail::json_compact(rd);
  rec += '\n';
  std::lock_guard<std::mutex> lock(history_mutex(hist_, root_));
  detail::HistoryHandles& hf = history_handles();
  bool trim = false;
  if (!hf.checked) {  // first append from this process
    hf.checked = true;
    migrate_single_file(hf, root_);
    const std::vector<uint32_t> segs = list_segments(root_);
    hf.head = segs.empty() ? 1 : std::max<uint32_t>(segs.back(), 1);
    hf.head_count = -1;
    trim = true;
  }
  if (hf.head_count < 0) {
    detail::HistoryFile* l = seg_list(hf, root_, hf.head, false);
    hf.head_count = l ? static_cast<int64_t>(count_lines(read_all(*l))) : 0;
  }
  detail::HistoryFile* recs = seg_recs(hf, root_, hf.head, false);
  if (hf.head_count >= static_cast<int64_t>(kHistorySegEntries) || (recs && recs->size() >= kHistorySegMaxBytes)) {
    // Full: start the next segment (after any that another process started meanwhile).
    const std::vector<uint32_t> segs = list_segments(root_);
    hf.head = std::max(hf.head, segs.empty() ? 0u : segs.back()) + 1;
    hf.head_count = 0;
    trim = true;
  }
  recs = seg_recs(hf, root_, hf.head, true);
  detail::HistoryFile* list = seg_list(hf, root_, hf.head, true);
  if (!recs || !list) return;
  uint64_t rec_end = 0;
  if (!recs->append(rec, &rec_end)) return;
  Value ref(rapidjson::kArrayType);
  ref.PushBack(Value(static_cast<uint64_t>(rec_end - rec.size())), a).PushBack(Value(static_cast<uint64_t>(rec.size() - 1)), a);
  d.AddMember("req", ref, a);
  std::string line = detail::json_compact(d);
  line += '\n';
  if (!list->append(line, nullptr)) return;
  ++hf.head_count;
  // The next start lists this segment and likely opens its newest entry.
  detail::prescan_later(seg_list_path(root_, hf.head));
  detail::prescan_later(seg_recs_path(root_, hf.head));
  if (trim) trim_segments(hf, root_);
}

std::vector<HistoryEntry> Workspace::history(size_t max) const {
  std::vector<HistoryEntry> out;
  if (max == 0) return out;
  std::lock_guard<std::mutex> lock(history_mutex(hist_, root_));
  detail::HistoryHandles& hf = history_handles();
  const std::vector<uint32_t> segs = list_segments(root_);
  std::vector<std::pair<uint32_t, detail::HistoryFile*>> files;  // newest first
  uint64_t bytes = 0;
  size_t i = segs.size();
  while (i > 0 && bytes / 100 < max) {  // our lines are 120-200 bytes: likely enough segments
    --i;
    if (detail::HistoryFile* l = seg_list(hf, root_, segs[i], false)) {
      files.emplace_back(segs[i], l);
      bytes += l->size();
    }
  }
  // HistoryEntry is large: one allocation up front, not a move of every entry per segment.
  out.reserve(static_cast<size_t>(std::min<uint64_t>(max, bytes / 100 + 16)));
  for (const auto& [seq, l] : files) {
    if (out.size() >= max) break;
    read_segment(*l, seq, max - out.size(), out);
  }
  // Shorter lines than estimated (hand-written, older): go on with the older segments.
  while (i > 0 && out.size() < max) {
    --i;
    if (detail::HistoryFile* l = seg_list(hf, root_, segs[i], false)) read_segment(*l, segs[i], max - out.size(), out);
  }
  return out;
}

std::optional<SavedRequest> Workspace::history_request(const HistoryEntry& e) const {
  if (e.offset == UINT64_MAX) return std::nullopt;
  const uint32_t seq = static_cast<uint32_t>(e.offset >> kSegShift);
  const uint64_t at = e.offset & kSegOffMask;
  std::lock_guard<std::mutex> lock(history_mutex(hist_, root_));
  detail::HistoryHandles& hf = history_handles();
  detail::HistoryFile* list = seg_list(hf, root_, seq, false);
  if (!list) return std::nullopt;  // trimmed
  std::string line;
  for (size_t n = 4096;; n *= 4) {
    if (!list->read_at(at, n, line)) return std::nullopt;
    const size_t nl = line.find('\n');
    if (nl != std::string::npos) {
      line.resize(nl);
      break;
    }
    if (line.size() < n) break;  // last line without a newline
  }
  if (!line.empty() && line.back() == '\r') line.pop_back();
  rapidjson::Document d;
  d.Parse(line.data(), line.size());
  if (d.HasParseError() || !d.IsObject()) return std::nullopt;
  // Make sure this is the same entry (a hand edit, or a migrated or trimmed file).
  if (get_str(d, "when") != e.when_iso || get_str(d, "summary") != e.summary) return std::nullopt;
  SavedRequest r;
  uint64_t off = 0, len = 0;
  if (const Value* req = get_obj(d, "request")) {  // older lines hold the request inline
    request_from_value(*req, r);
  } else if (history_req_ref(d, off, len)) {
    std::string rec;
    detail::HistoryFile* recs = seg_recs(hf, root_, seq, false);
    if (!recs || !recs->read_at(off, static_cast<size_t>(len), rec) || rec.size() != len) return std::nullopt;
    rapidjson::Document rd;
    rd.Parse(rec.data(), rec.size());
    if (rd.HasParseError() || !rd.IsObject() || get_str(rd, "when") != e.when_iso || get_str(rd, "summary") != e.summary)
      return std::nullopt;
    if (const Value* rq = get_obj(rd, "request")) request_from_value(*rq, r);
  }
  return r;
}

// ------------------------------------------------------------------ variables

namespace {

std::string trim(std::string_view s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
  return std::string(s.substr(a, b - a));
}

// Built-in dynamic variables (Postman/Bruno compatible names).
bool dynamic_var(const std::string& name, std::string& out) {
  if (name.empty()) return false;
  if (name == "$guid" || name == "$uuid" || name == "$randomUUID") {
    out = new_uuid();
    return true;
  }
  if (name == "$timestamp") {
    out = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count());
    return true;
  }
  if (name == "$isoTimestamp") {
    out = now_iso();
    return true;
  }
  if (name == "$randomInt") {
    out = std::to_string(rng()() % 1001);
    return true;
  }
  return false;
}

// "process.env.X" / "$env.X" -> "X" (empty for any other name).
std::string os_env_name(std::string_view name) {
  for (std::string_view prefix : {std::string_view("process.env."), std::string_view("$env.")})
    if (name.substr(0, prefix.size()) == prefix) return std::string(name.substr(prefix.size()));
  return {};
}

bool same_env_name(const std::string& a, const std::string& b) {
#ifdef _WIN32
  return lower_ascii(a) == lower_ascii(b);  // Windows environment names are case-insensitive
#else
  return a == b;
#endif
}

// The OS environment variables still referenced ({{process.env.X}} / {{$env.X}} left in an
// expanded string), in order of appearance.
void collect_env_refs(const std::string& s, std::vector<std::string>& out) {
  for (size_t open = s.find("{{"); open != std::string::npos; open = s.find("{{", open + 2)) {
    const size_t close = s.find("}}", open + 2);
    if (close == std::string::npos) return;
    const std::string var = os_env_name(trim(std::string_view(s).substr(open + 2, close - open - 2)));
    if (var.empty()) continue;
    if (std::none_of(out.begin(), out.end(), [&](const std::string& o) { return same_env_name(o, var); }))
      out.push_back(var);
  }
}

std::vector<std::string> read_env_allowlist(const fs::path& root);  // below, with the upload allowlist
fs::path resolved_path(const fs::path& p);

}  // namespace

std::string Workspace::expand(const std::string& s) const { return expand_in(s, nullptr); }

std::string Workspace::expand(const std::string& s, const SavedRequest& r) const {
  if (s.find("{{") == std::string::npos) return s;
  const FolderChain c = folder_chain(r);
  return expand_in(s, &c);
}

// c: the folder chain of the request being expanded (folder and collection vars); nullptr: no
// request (collection vars read from <ws>/folder.json when a name gets that far).
std::string Workspace::expand_in(const std::string& s, const FolderChain* c) const {
  if (s.find("{{") == std::string::npos) return s;
  std::optional<std::vector<std::string>> env_allowed;  // read when an OS variable is met
  bool coll_looked = false;
  const std::map<std::string, std::string>* coll = nullptr;
  std::shared_ptr<const detail::LoadedFolder> coll_data;
  const Environment* env = nullptr;
  if (!active_env.empty())
    for (const auto& e : envs_)
      if (e.name == active_env) {
        env = &e;
        break;
      }
  auto lookup = [&](const std::string& name, std::string& value) -> bool {
    if (!runtime_vars.empty()) {
      auto it = runtime_vars.find(name);
      if (it != runtime_vars.end()) {
        value = it->second;
        return true;
      }
    }
    if (c)  // folder vars, nearest folder first
      for (size_t i = c->levels.size(); i-- > 0;) {
        const auto& l = c->levels[i];
        if (l.origin.level != SettingsOrigin::Level::Folder) continue;
        auto it = l.data->vars.find(name);
        if (it != l.data->vars.end()) {
          value = it->second;
          return true;
        }
      }
    if (env) {
      auto it = env->vars.find(name);
      if (it != env->vars.end()) {
        value = it->second;
        return true;
      }
    }
    if (!coll_looked) {
      coll_looked = true;
      if (c) {
        if (const auto* l = c->collection()) coll = &l->data->vars;
      } else {
        coll_data = load_folder_file(folder_cache(), root_, root_);
        if (coll_data && coll_data->error.empty()) coll = &coll_data->vars;
      }
    }
    if (coll) {
      auto it = coll->find(name);
      if (it != coll->end()) {
        value = it->second;
        return true;
      }
    }
    auto it = globals.find(name);
    if (it != globals.end()) {
      value = it->second;
      return true;
    }
    if (!dotenv.empty() && name.rfind("process.env.", 0) == 0) {
      auto d = dotenv.find(name.substr(12));
      if (d != dotenv.end()) {
        value = d->second;
        return true;
      }
    }
    // The OS environment only for names the user allowed for this workspace (see
    // blocked_env_vars()): a cloned workspace must not send e.g. AWS_SECRET_ACCESS_KEY.
    if (const std::string var = os_env_name(name); !var.empty()) {
      if (!env_allowed) env_allowed = read_env_allowlist(root_);
      if (std::none_of(env_allowed->begin(), env_allowed->end(), [&](const std::string& a) { return same_env_name(a, var); }))
        return false;
      std::string v = get_env(var);
      if (v.empty()) return false;  // unset: leave the placeholder visible
      value = std::move(v);
      return true;
    }
    return dynamic_var(name, value);
  };
  // Values are expanded again (nested variables, up to 8 levels like Bruno's recursive
  // interpolation). A variable already being expanded is left as-is, so self-referencing or
  // cyclic variables cannot loop.
  std::vector<std::string> stack;
  auto run = [&](const std::string& in, auto& self) -> std::string {
    std::string out;
    out.reserve(in.size() + 32);
    size_t i = 0;
    while (i < in.size()) {
      const size_t open = in.find("{{", i);
      if (open == std::string::npos) {
        out.append(in, i, std::string::npos);
        break;
      }
      out.append(in, i, open - i);
      const size_t close = in.find("}}", open + 2);
      if (close == std::string::npos) {
        out.append(in, open, std::string::npos);
        break;
      }
      const std::string_view inner(in.data() + open + 2, close - open - 2);
      if (inner.find("{{") != std::string_view::npos || (!inner.empty() && inner.front() == '{')) {
        out += '{';  // e.g. "{{{x}}}": emit one brace literally and retry from the next one
        i = open + 1;
        continue;
      }
      std::string value;
      const std::string name = trim(inner);
      if (std::find(stack.begin(), stack.end(), name) == stack.end() && lookup(name, value)) {
        if (stack.size() < 8 && value.find("{{") != std::string::npos) {
          stack.push_back(name);
          out += self(value, self);
          stack.pop_back();
        } else {
          out += value;
        }
      } else {
        out.append(in, open, close + 2 - open);  // unknown: leave as-is
      }
      i = close + 2;
    }
    return out;
  };
  return run(s, run);
}

SavedRequest Workspace::expanded(const SavedRequest& r) const { return expanded_with(r, folder_chain(r)); }

SavedRequest Workspace::expanded_with(const SavedRequest& r, const FolderChain& c) const {
  SavedRequest x = resolve_with(r, c);
  auto expand = [&](const std::string& s) { return expand_in(s, &c); };
  x.http.url = expand(x.http.url);
  for (auto& [k, v] : x.http.headers) {
    k = expand(k);
    v = expand(v);
  }
  x.http.body = expand(x.http.body);
  for (std::string* f : {&x.network.ca_file, &x.network.cert_file, &x.network.key_file, &x.network.proxy_url,
                         &x.network.no_proxy})
    *f = expand(*f);
  x.grpc_target.address = expand(x.grpc_target.address);
  x.grpc_target.authority = expand(x.grpc_target.authority);
  x.grpc_method = expand(x.grpc_method);
  x.grpc_body = expand(x.grpc_body);
  for (auto& [k, v] : x.grpc_opts.metadata) {
    k = expand(k);
    v = expand(v);
  }
  for (auto& p : x.proto_files) p = expand(p);
  for (auto& p : x.import_dirs) p = expand(p);
  x.basic_auth.user = expand(x.basic_auth.user);
  x.basic_auth.password = expand(x.basic_auth.password);
  if (x.auth.type != HttpAuth::Type::None) {
    HttpAuth& au = x.auth;
    for (std::string* f : {&au.user, &au.password, &au.aws_access_key, &au.aws_secret_key, &au.aws_session_token,
                           &au.aws_region, &au.aws_service, &au.oauth2.token_url, &au.oauth2.auth_url,
                           &au.oauth2.client_id, &au.oauth2.client_secret, &au.oauth2.scope, &au.oauth2.audience,
                           &au.oauth2.username, &au.oauth2.password, &au.oauth2.redirect_uri,
                           &au.oauth2.header_prefix, &au.oauth2.query_param})
      *f = expand(*f);
  }
  for (auto& p : x.form) {
    p.name = expand(p.name);
    p.value = expand(p.value);
    p.filename = expand(p.filename);
    p.type = expand(p.type);
  }
  // An inherited header / metadata entry cannot smuggle a line break (a second header, a
  // request split) into the request, whatever its {{vars}} expand to: dropped.
  if (x.inherited_headers) {
    std::vector<Header> keep;
    std::set<size_t> off;
    size_t inh = 0;
    for (size_t k = 0; k < x.http.headers.size(); ++k) {
      auto& h = x.http.headers[k];
      const bool inherited = k < x.inherited_headers;
      if (inherited && (has_ctl_break(h.first) || has_ctl_break(h.second))) continue;
      if (x.disabled_headers.count(k)) off.insert(keep.size());
      inh += inherited;
      keep.push_back(std::move(h));
    }
    x.http.headers = std::move(keep);
    x.disabled_headers = std::move(off);
    x.inherited_headers = inh;
  }
  if (x.inherited_metadata) {
    std::vector<Header> keep;
    size_t inh = 0;
    for (size_t k = 0; k < x.grpc_opts.metadata.size(); ++k) {
      auto& h = x.grpc_opts.metadata[k];
      const bool inherited = k < x.inherited_metadata;
      if (inherited && (has_ctl_break(h.first) || has_ctl_break(h.second))) continue;
      inh += inherited;
      keep.push_back(std::move(h));
    }
    x.grpc_opts.metadata = std::move(keep);
    x.inherited_metadata = inh;
  }
  return x;
}

// ------------------------------------------------------------------ upload policy

namespace {

// The per-user upload allowlist: <user_config_dir()>/allowed_files.json
//   { "version": 1, "workspaces": { "<resolved root>": ["<resolved file>", ...] } }
// It lives outside every workspace on purpose: anything under <ws> (a .watt/allowed_files.json
// included) can come from a cloned repo, so it is never read for this.
std::mutex& config_mutex() {
  static std::mutex mu;
  return mu;
}

fs::path& config_dir_override() {
  static fs::path p;
  return p;
}

fs::path default_config_dir() {
#ifdef _WIN32
  wchar_t* v = nullptr;
  size_t n = 0;
  fs::path base;
  if (_wdupenv_s(&v, &n, L"APPDATA") == 0 && v && *v) base = fs::path(v);
  std::free(v);
  return base.empty() ? fs::path() : base / "watt";
#else
  if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x == '/') return fs::path(x) / "watt";
  if (const char* h = std::getenv("HOME"); h && *h) return fs::path(h) / ".config" / "watt";
  return {};
#endif
}

constexpr const char* kAllowedFilesFile = "allowed_files.json";  // in Workspace::user_config_dir()
// OS environment variables a workspace may expand: { "version": 1, "workspaces": { "<resolved
// root>": ["NAME", ...] } }, next to allowed_files.json and for the same reason.
constexpr const char* kAllowedEnvFile = "allowed_env.json";

bool same_path(const fs::path& a, const fs::path& b) {
#ifdef _WIN32
  return lower_ascii(generic_utf8(a)) == lower_ascii(generic_utf8(b));  // NTFS is case-insensitive
#else
  return a == b;
#endif
}

// \\host\share\..., //host/..., \\?\UNC\..., \\.\device, \??\...: decided from the text alone.
// Touching such a path (even weakly_canonical's stat) opens an SMB session, which sends the
// user's NTLM hash to whoever named the host in a cloned workspace, before any consent prompt
// could be answered. On Windows the only local root accepted is a plain drive letter ("C:").
bool is_remote_or_device_path(const fs::path& p) {
  const auto& s = p.native();
  auto sep = [](auto c) { return c == '\\' || c == '/'; };
  if (s.size() >= 2 && sep(s[0]) && sep(s[1])) return true;  // UNC, \\?\, \\.\, //host
#ifdef _WIN32
  if (s.size() >= 4 && sep(s[0]) && s[1] == '?' && s[2] == '?' && sep(s[3])) return true;  // \??\ NT prefix
  const auto rn = p.root_name().native();
  if (!rn.empty()) {
    const bool drive = rn.size() == 2 && rn[1] == ':' && ((rn[0] >= 'A' && rn[0] <= 'Z') || (rn[0] >= 'a' && rn[0] <= 'z'));
    if (!drive) return true;
  }
#endif
  return false;
}

// "\foo" (root-relative: current drive) and "C:foo" (drive-relative: that drive's cwd) mean
// something different to every process; a workspace path must be relative or fully absolute.
bool is_ambiguous_rooted_path(const fs::path& p) {
#ifdef _WIN32
  return p.has_root_name() != p.has_root_directory();
#else
  (void)p;
  return false;
#endif
}

fs::path resolved_path(const fs::path& p) {
  if (is_remote_or_device_path(p)) return p.lexically_normal();  // never touch the network
  std::error_code ec;
  fs::path c = fs::weakly_canonical(p, ec);  // symlinks / junctions in the existing part resolved
  return ec || c.empty() ? p.lexically_normal() : c;
}

// Allowlist key of a resolved workspace root: generic UTF-8, drive letter lowercased on Windows.
std::string root_key(const fs::path& resolved_root) {
  std::string k = generic_utf8(resolved_root);
#ifdef _WIN32
  if (k.size() >= 2 && k[1] == ':' && k[0] >= 'A' && k[0] <= 'Z') k[0] = static_cast<char>(k[0] - 'A' + 'a');
#endif
  return k;
}

bool is_root_key(const Value& name, const fs::path& resolved_root) {
  return same_path(from_utf8(std::string(name.GetString(), name.GetStringLength())), resolved_root);
}

std::vector<fs::path> read_allowlist(const fs::path& resolved_root) {
  std::vector<fs::path> out;
  const fs::path dir = Workspace::user_config_dir();
  if (dir.empty()) return out;
  rapidjson::Document d;
  std::lock_guard<std::mutex> lock(config_mutex());
  if (!parse_json_file(dir / kAllowedFilesFile, d)) return out;
  const Value* wss = get_obj(d, "workspaces");
  if (!wss) return out;
  for (auto it = wss->MemberBegin(); it != wss->MemberEnd(); ++it)
    if (it->value.IsArray() && is_root_key(it->name, resolved_root))
      for (const auto& f : it->value.GetArray())
        if (f.IsString()) out.push_back(from_utf8(std::string(f.GetString(), f.GetStringLength())));
  return out;
}

std::vector<std::string> read_env_allowlist(const fs::path& root) {
  std::vector<std::string> out;
  const fs::path dir = Workspace::user_config_dir();
  if (dir.empty()) return out;
  const fs::path rroot = resolved_path(root);
  rapidjson::Document d;
  std::lock_guard<std::mutex> lock(config_mutex());
  if (!parse_json_file(dir / kAllowedEnvFile, d)) return out;
  const Value* wss = get_obj(d, "workspaces");
  if (!wss) return out;
  for (auto it = wss->MemberBegin(); it != wss->MemberEnd(); ++it)
    if (it->value.IsArray() && is_root_key(it->name, rroot))
      for (const auto& f : it->value.GetArray())
        if (f.IsString()) out.emplace_back(f.GetString(), f.GetStringLength());
  return out;
}

// <user_config_dir()>/trusted_scripts.json: {"version": 1, "workspaces": {"<resolved root>": ["scripts"]}}.
constexpr const char* kTrustedScriptsFile = "trusted_scripts.json";
// <user_config_dir()>/allowed_sso.json: {"version": 1, "workspaces": {"<resolved root>": ["https://host:443", ...]}}.
constexpr const char* kAllowedSsoFile = "allowed_sso.json";

// `root`'s string list in <user_config_dir()>/<file>.
std::vector<std::string> read_root_list(const char* file, const fs::path& root) {
  std::vector<std::string> out;
  const fs::path dir = Workspace::user_config_dir();
  if (dir.empty()) return out;
  const fs::path rroot = resolved_path(root);
  rapidjson::Document d;
  std::lock_guard<std::mutex> lock(config_mutex());
  if (!parse_json_file(dir / file, d)) return out;
  const Value* wss = get_obj(d, "workspaces");
  if (!wss) return out;
  for (auto it = wss->MemberBegin(); it != wss->MemberEnd(); ++it)
    if (it->value.IsArray() && is_root_key(it->name, rroot))
      for (const auto& f : it->value.GetArray())
        if (f.IsString()) out.emplace_back(f.GetString(), f.GetStringLength());
  return out;
}

// Replaces `root`'s entry in <user_config_dir()>/<file> (other workspaces kept), owner-only.
bool write_root_list(const char* file, const fs::path& root, const std::vector<std::string>& items, const char* what,
                     std::string* err) {
  const fs::path dir = Workspace::user_config_dir();
  if (dir.empty()) {
    if (err) *err = std::string("no per-user config directory (APPDATA / HOME not set): cannot store the ") + what;
    return false;
  }
  const fs::path rroot = resolved_path(root);
  std::lock_guard<std::mutex> lock(config_mutex());
  rapidjson::Document d;
  if (!parse_json_file(dir / file, d)) d.SetObject();
  auto& a = d.GetAllocator();
  if (!d.HasMember("version")) d.AddMember("version", 1, a);
  if (!get_obj(d, "workspaces")) {
    d.RemoveMember("workspaces");
    d.AddMember("workspaces", Value(rapidjson::kObjectType), a);
  }
  Value& wss = d["workspaces"];
  for (auto it = wss.MemberBegin(); it != wss.MemberEnd();) {  // this root's old entry
    if (is_root_key(it->name, rroot)) it = wss.EraseMember(it);
    else ++it;
  }
  if (!items.empty()) {
    Value arr(rapidjson::kArrayType);
    for (const auto& f : items) arr.PushBack(jstr(f, a), a);
    wss.AddMember(jstr(root_key(rroot), a), arr, a);
  }
  // Owner-only, like .watt: the directory (protected DACL / 0700) and the file (0600 on POSIX).
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    detail::ensure_dir(dir.parent_path());
    detail::create_private_dir(dir);
  }
  return detail::write_file_atomic(dir / file, detail::json_pretty(d), err, true);
}

// Which files a send may read (see Workspace::blocked_files()). The allowlist is read only when a
// file outside the workspace is met.
class UploadPolicy {
 public:
  explicit UploadPolicy(const fs::path& root) : root_(resolved_path(root)) {}

  const fs::path& root() const { return root_; }
  // File reference from a request: relative to the workspace root (never the process cwd).
  fs::path resolve(const fs::path& ref) const { return resolved_path(ref.is_relative() ? root_ / ref : ref); }
  fs::path resolve(const std::string& ref) const { return resolve(from_utf8(ref)); }

  bool inside(const fs::path& fp) const {
    if (is_remote_or_device_path(fp) || !is_inside(root_, fp)) return false;
    const std::string first = lower_ascii(to_utf8(*fp.lexically_relative(root_).begin()));
    return first != ".watt" && first != ".git" && first != ".env";  // personal, never uploaded silently
  }
  bool listed(const fs::path& fp) const {
    if (!list_) list_ = read_allowlist(root_);
    for (const auto& a : *list_)
      if (same_path(a, fp)) return true;
    return false;
  }
  bool allowed(const fs::path& fp) const { return inside(fp) || listed(fp); }

  std::string blocked_message(const std::string& what, const fs::path& fp) const {
    return what + " wants to upload " + to_utf8(fp) + ", which is outside the workspace " + to_utf8(root_) +
           " (or in its private .watt/, .git/ or .env): request not sent. The user must allow this file "
           "for this workspace first.";
  }

 private:
  fs::path root_;  // resolved
  mutable std::optional<std::vector<fs::path>> list_;
};

}  // namespace

fs::path Workspace::user_config_dir() {
  {
    std::lock_guard<std::mutex> lock(config_mutex());
    if (!config_dir_override().empty()) return config_dir_override();
  }
  return default_config_dir();
}

void Workspace::set_user_config_dir(const fs::path& dir) {
  std::lock_guard<std::mutex> lock(config_mutex());
  config_dir_override() = dir;
}

std::vector<fs::path> Workspace::blocked_files(const SavedRequest& r) const {
  const FolderChain c = folder_chain(r);
  const SavedRequest x = expanded_with(r, c);
  auto expand = [&](const std::string& s) { return expand_in(s, &c); };
  const UploadPolicy policy(root_);
  std::vector<fs::path> out;
  auto check = [&](const fs::path& fp) {
    if (policy.allowed(fp)) return;
    for (const auto& o : out)
      if (same_path(o, fp)) return;
    out.push_back(fp);
  };
  bool any = false;
  for (const auto& p : x.form) any = any || p.enabled;
  if (any) {
    for (const auto& p : x.form)
      if (p.enabled && p.kind != FormPart::Kind::Text) check(policy.resolve(p.value));
  } else if (!x.body_file.empty()) {
    check(policy.resolve(expand(x.body_file)));
  } else {
    for (const auto& d : x.body_data)
      if (const std::string f = curl_data_file(CurlData{d.kind, expand(d.value)}); !f.empty()) check(policy.resolve(f));
  }
  for (const auto& f : x.header_files) check(policy.resolve(expand(f)));
  for (const auto& f : x.cookie_files) check(policy.resolve(expand(f)));
  // CA bundle and client certificate / key (already expanded): a cloned workspace must not make
  // Send present the user's client certificate from outside it without asking.
  if (r.kind == RequestKind::Http)
    for (const std::string* f : {&x.network.ca_file, &x.network.cert_file, &x.network.key_file})
      if (!f->empty()) check(policy.resolve(*f));
  return out;
}

// Proto files and import directories a schema load would read. The loader runs when a gRPC
// request is merely opened, so anything outside the workspace (UNC shares included) must be
// approved first, through the same per-user allowlist as uploads (allow_file()).
// One plan, used both to ask for approval and to load: the loader gets exactly the paths that
// were checked (absolute, anchored at the workspace root), never the raw strings it would resolve
// against the process cwd.
//   import dir  : relative -> <root>/dir; absolute -> as is. Checked against the policy.
//   proto file  : absolute -> checked like a dir. Relative and an existing file under <root>
//                 -> that file, checked. Otherwise a virtual name looked up only in the (checked)
//                 import dirs: it must have no root and no "..".
//   "\foo" and "C:foo" are refused: they name a different place for every process.
Workspace::SchemaPlan Workspace::schema_plan(const SavedRequest& r) const {
  const SavedRequest x = expanded(r);  // folder vars included
  const UploadPolicy policy(root_);
  SchemaPlan plan;
  auto block = [&](const fs::path& fp) {
    if (policy.allowed(fp)) return;
    for (const auto& o : plan.blocked)
      if (same_path(o, fp)) return;
    plan.blocked.push_back(fp);
  };
  auto bad = [&](const std::string& ref, const char* why) { plan.invalid.push_back(ref + ": " + why); };
  for (const auto& d : x.import_dirs) {
    if (d.empty()) continue;
    const fs::path p = from_utf8(d);
    if (is_ambiguous_rooted_path(p)) { bad(d, "use a path relative to the workspace or a full path with a drive letter"); continue; }
    const fs::path fp = policy.resolve(p);
    block(fp);
    plan.import_dirs.push_back(to_utf8(fp));
  }
  for (const auto& f : x.proto_files) {
    if (f.empty()) continue;
    const fs::path p = from_utf8(f);
    if (is_ambiguous_rooted_path(p)) { bad(f, "use a path relative to the workspace or a full path with a drive letter"); continue; }
    if (p.is_absolute() || is_remote_or_device_path(p)) {
      const fs::path fp = policy.resolve(p);
      block(fp);
      plan.proto_files.push_back(to_utf8(fp));
      continue;
    }
    const fs::path under_root = policy.resolve(p);  // local only: p is relative, root is local
    std::error_code ec;
    if (!is_remote_or_device_path(under_root) && fs::is_regular_file(under_root, ec)) {
      block(under_root);
      plan.proto_files.push_back(to_utf8(under_root));
      continue;
    }
    bool escapes = false;
    for (const auto& part : p.lexically_normal()) escapes = escapes || part == "..";
    if (escapes) { bad(f, "an import name cannot contain \"..\""); continue; }
    // Only .proto sources are looked up through the import dirs. Anything else (a .protoset,
    // .pb, .desc ...) is read straight from disk by the loader, so a relative one that is not
    // a file under the workspace would be opened against the process cwd: refuse it.
    std::string ext = lower_ascii(to_utf8(p.extension()));
    if (ext != ".proto") { bad(f, "not found in the workspace (only .proto files can be looked up in import paths)"); continue; }
    plan.proto_files.push_back(generic_utf8(p.lexically_normal()));  // virtual: import dirs only
  }
  return plan;
}

std::vector<fs::path> Workspace::blocked_schema_paths(const SavedRequest& r) const { return schema_plan(r).blocked; }

bool Workspace::is_file_allowed(const fs::path& file) const {
  const UploadPolicy policy(root_);
  return policy.allowed(policy.resolve(file));
}

std::vector<fs::path> Workspace::allowed_files() const { return read_allowlist(resolved_path(root_)); }

// Replaces this workspace's entry (read-modify-write of the per-user file; other workspaces kept).
bool Workspace::write_allowlist(const std::vector<fs::path>& files, std::string* err) const {
  std::vector<std::string> items;
  for (const auto& f : files) items.push_back(generic_utf8(f));
  return write_root_list(kAllowedFilesFile, root_, items, "upload allowlist", err);
}

std::vector<std::string> Workspace::blocked_env_vars(const SavedRequest& r) const {
  const FolderChain c = folder_chain(r);
  return env_refs_left(expanded_with(r, c), &c);
}

std::vector<std::string> Workspace::env_refs_left(const SavedRequest& x, const FolderChain* c) const {
  auto expand = [&](const std::string& s) { return expand_in(s, c); };
  std::vector<std::string> refs;
  auto scan = [&](const std::string& s) { collect_env_refs(s, refs); };
  scan(x.http.url);
  for (const auto& [k, v] : x.http.headers) {
    scan(k);
    scan(v);
  }
  scan(x.http.body);
  scan(x.basic_auth.user);
  scan(x.basic_auth.password);
  if (x.auth.type != HttpAuth::Type::None) {
    const HttpAuth& au = x.auth;
    for (const std::string* f : {&au.user, &au.password, &au.aws_access_key, &au.aws_secret_key, &au.aws_session_token,
                                 &au.aws_region, &au.aws_service, &au.oauth2.token_url, &au.oauth2.auth_url,
                                 &au.oauth2.client_id, &au.oauth2.client_secret, &au.oauth2.scope,
                                 &au.oauth2.audience, &au.oauth2.username, &au.oauth2.password,
                                 &au.oauth2.redirect_uri, &au.oauth2.header_prefix, &au.oauth2.query_param})
      scan(*f);
  }
  for (const auto& p : x.form) {
    scan(p.name);
    scan(p.value);
    scan(p.filename);
    scan(p.type);
  }
  scan(expand(x.body_file));
  for (const auto& d : x.body_data) scan(expand(d.value));
  for (const auto& f : x.header_files) scan(expand(f));
  for (const auto& f : x.cookie_files) scan(expand(f));
  for (const std::string* f : {&x.network.ca_file, &x.network.cert_file, &x.network.key_file, &x.network.proxy_url,
                               &x.network.no_proxy})
    scan(*f);
  scan(x.grpc_target.address);
  scan(x.grpc_target.authority);
  scan(x.grpc_method);
  scan(x.grpc_body);
  for (const auto& [k, v] : x.grpc_opts.metadata) {
    scan(k);
    scan(v);
  }
  for (const auto& p : x.proto_files) scan(p);
  for (const auto& p : x.import_dirs) scan(p);
  // Left unresolved and set in the OS environment: not allowed (an allowed one was expanded; an
  // unset one has nothing to leak and stays visible as {{...}}).
  std::erase_if(refs, [](const std::string& v) { return get_env(v).empty(); });
  return refs;
}

bool Workspace::is_env_var_allowed(const std::string& name) const {
  const auto list = read_env_allowlist(root_);
  return std::any_of(list.begin(), list.end(), [&](const std::string& a) { return same_env_name(a, name); });
}

std::vector<std::string> Workspace::allowed_env_vars() const { return read_env_allowlist(root_); }

bool Workspace::allow_env_var(const std::string& name, std::string* err) const {
  if (name.empty()) return true;
  std::vector<std::string> list = read_env_allowlist(root_);
  if (std::any_of(list.begin(), list.end(), [&](const std::string& a) { return same_env_name(a, name); })) return true;
  list.push_back(name);
  return write_root_list(kAllowedEnvFile, root_, list, "environment variable allowlist", err);
}

bool Workspace::disallow_env_var(const std::string& name, std::string* err) const {
  std::vector<std::string> list = read_env_allowlist(root_);
  const auto end = std::remove_if(list.begin(), list.end(), [&](const std::string& a) { return same_env_name(a, name); });
  if (end == list.end()) return true;
  list.erase(end, list.end());
  return write_root_list(kAllowedEnvFile, root_, list, "environment variable allowlist", err);
}

bool Workspace::allow_file(const fs::path& file, std::string* err) const {
  const UploadPolicy policy(root_);
  const fs::path fp = policy.resolve(file);
  if (policy.inside(fp)) return true;
  std::vector<fs::path> list = read_allowlist(policy.root());
  for (const auto& a : list)
    if (same_path(a, fp)) return true;
  list.push_back(fp);
  return write_allowlist(list, err);
}

bool Workspace::disallow_file(const fs::path& file, std::string* err) const {
  const UploadPolicy policy(root_);
  const fs::path fp = policy.resolve(file);
  std::vector<fs::path> list = read_allowlist(policy.root());
  const auto end = std::remove_if(list.begin(), list.end(), [&](const fs::path& a) { return same_path(a, fp); });
  if (end == list.end()) return true;
  list.erase(end, list.end());
  return write_allowlist(list, err);
}

// ------------------------------------------------------------------ network trust (proxy / CA / client cert)

namespace {

// <user_config_dir()>/allowed_network.json: {"version": 1, "workspaces": {"<resolved root>": [keys]}}.
constexpr const char* kAllowedNetworkFile = "allowed_network.json";

std::string level_name(const SettingsOrigin& o) {
  switch (o.level) {
    case SettingsOrigin::Level::Request: return "request";
    case SettingsOrigin::Level::Collection: return "collection";
    default: return "folder " + o.label;
  }
}

std::string colon_hex(const std::string& raw) {
  static const char* d = "0123456789ABCDEF";
  std::string out;
  for (const unsigned char c : raw) {
    if (!out.empty()) out += ':';
    out += d[c >> 4];
    out += d[c & 15];
  }
  return out;
}

std::string ca_key(const std::string& pem, const fs::path& file) {
  return "ca sha256:" + crypto::hex(crypto::digest(crypto::Hash::Sha256, pem)) + " " + generic_utf8(file);
}

std::string cert_key(const fs::path& cert, const fs::path& key) {
  std::string k = "cert " + generic_utf8(cert);
  if (!key.empty()) k += " key " + generic_utf8(key);
  return k;
}

// The password of a proxy URL's user info as written (before {{var}} expansion), "" if none.
std::string literal_proxy_password(const std::string& url) {
  const size_t s = url.find("://");
  const size_t a0 = s == std::string::npos ? 0 : s + 3;
  const size_t a1 = url.find_first_of("/?#", a0);
  const std::string auth = url.substr(a0, a1 == std::string::npos ? std::string::npos : a1 - a0);
  const size_t at = auth.rfind('@');
  if (at == std::string::npos) return {};
  const std::string ui = auth.substr(0, at);
  const size_t colon = ui.find(':');
  return colon == std::string::npos ? std::string() : ui.substr(colon + 1);
}

}  // namespace

std::string Workspace::proxy_origin(const std::string& proxy_url, bool* has_credentials, std::string* normalized) {
  if (has_credentials) *has_credentials = false;
  if (normalized) normalized->clear();
  if (proxy_url.empty() || proxy_url.find('\\') != std::string::npos) return {};
  for (const char c : proxy_url)
    if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F) return {};
  std::string url = proxy_url;
  if (url.find("://") == std::string::npos) url = "http://" + url;  // libcurl's default proxy scheme
  const std::string scheme = lower_ascii(url.substr(0, url.find("://")));
  if (scheme != "http" && scheme != "https" && scheme != "socks5" && scheme != "socks5h") return {};
  CURLU* u = curl_url();
  if (!u) return {};
  std::string out;
  if (curl_url_set(u, CURLUPART_URL, url.c_str(), CURLU_NON_SUPPORT_SCHEME) == CURLUE_OK) {
    char *host = nullptr, *port = nullptr, *user = nullptr, *pass = nullptr, *query = nullptr, *frag = nullptr,
         *path = nullptr;
    const bool got = curl_url_get(u, CURLUPART_HOST, &host, 0) == CURLUE_OK && host && *host;
    if (curl_url_get(u, CURLUPART_PORT, &port, 0) != CURLUE_OK) port = nullptr;
    const bool has_user = curl_url_get(u, CURLUPART_USER, &user, 0) == CURLUE_OK && user && *user;
    const bool has_pass = curl_url_get(u, CURLUPART_PASSWORD, &pass, 0) == CURLUE_OK && pass && *pass;
    const bool has_query = curl_url_get(u, CURLUPART_QUERY, &query, 0) == CURLUE_OK && query;
    const bool has_frag = curl_url_get(u, CURLUPART_FRAGMENT, &frag, 0) == CURLUE_OK && frag;
    const bool has_path = curl_url_get(u, CURLUPART_PATH, &path, 0) == CURLUE_OK && path && std::string(path) != "/";
    if (got && !has_query && !has_frag && !has_path) {
      std::string h = lower_ascii(host);
      while (!h.empty() && h.back() == '.') h.pop_back();
      const std::string p = port ? std::string(port)
                            : scheme == "https" ? std::string("443")
                            : scheme == "http"  ? std::string("80")
                                                : std::string("1080");
      if (!h.empty()) {
        out = scheme + "://" + h + ":" + p;
        if (has_credentials) *has_credentials = has_user || has_pass;
        if (normalized) {
          std::string ui;
          if (has_user || has_pass) ui = std::string(user ? user : "") + (has_pass ? ":" + std::string(pass) : "") + "@";
          *normalized = scheme + "://" + ui + h + ":" + p;
        }
      }
    }
    curl_free(host);
    curl_free(port);
    curl_free(user);
    curl_free(pass);
    curl_free(query);
    curl_free(frag);
    curl_free(path);
  }
  curl_url_cleanup(u);
  return out;
}

std::vector<Workspace::NetworkItem> Workspace::blocked_network_settings(const SavedRequest& r) const {
  std::vector<NetworkItem> out;
  if (r.kind != RequestKind::Http) return out;
  const FolderChain c = folder_chain(r);
  const SavedRequest x = expanded_with(r, c);
  const NetworkSettings& n = x.network;
  if (n.ca_file.empty() && n.cert_file.empty() && n.proxy != NetworkSettings::Proxy::Url) return out;
  const EffectiveSettings e = effective_settings(r);
  const auto allowed = read_root_list(kAllowedNetworkFile, root_);
  auto listed = [&](const std::string& k) { return std::find(allowed.begin(), allowed.end(), k) != allowed.end(); };
  const UploadPolicy policy(root_);

  if (n.proxy == NetworkSettings::Proxy::Url && !n.proxy_url.empty()) {
    bool creds = false;
    const std::string origin = proxy_origin(n.proxy_url, &creds);
    // Not a valid proxy URL: nothing to approve, http_to_send() refuses it.
    if (!origin.empty() && !listed("proxy " + origin)) {
      NetworkItem it;
      it.kind = NetworkItem::Kind::Proxy;
      it.key = "proxy " + origin;
      it.level = level_name(e.proxy_from);
      it.proxy_origin = origin;
      it.proxy_credentials = creds;
      it.text = "the proxy " + origin + (creds ? " (the URL carries proxy credentials)" : "");
      out.push_back(std::move(it));
    }
  }
  if (!n.ca_file.empty()) {
    NetworkItem it;
    it.kind = NetworkItem::Kind::Ca;
    it.level = level_name(e.ca_from);
    it.file = policy.resolve(n.ca_file);
    if (!policy.allowed(it.file)) {  // judged from the text: never touched before approval
      it.file_blocked.push_back(it.file);
      it.key = "file " + generic_utf8(it.file);
      it.text = "the CA certificate file " + to_utf8(it.file) + " (outside the workspace)";
      out.push_back(std::move(it));
    } else {
      std::string pem;
      if (detail::read_file(it.file, pem)) {  // unreadable: http_to_send() fails naming it
        it.key = ca_key(pem, it.file);
        if (!listed(it.key)) {
          it.fingerprint = colon_hex(crypto::digest(crypto::Hash::Sha256, pem));
          it.subjects = pem_certificate_subjects(pem);
          std::string who;
          for (size_t k = 0; k < it.subjects.size() && k < 3; ++k) who += (k ? ", " : "") + it.subjects[k];
          if (it.subjects.size() > 3) who += ", ...";
          it.text = "the CA certificate file " + to_utf8(it.file) + (who.empty() ? "" : " (" + who + ")") +
                    ", SHA-256 " + it.fingerprint;
          out.push_back(std::move(it));
        }
      }
    }
  }
  if (!n.cert_file.empty()) {
    NetworkItem it;
    it.kind = NetworkItem::Kind::ClientCert;
    it.level = level_name(e.cert_from);
    it.file = policy.resolve(n.cert_file);
    if (!n.key_file.empty()) it.key_file = policy.resolve(n.key_file);
    for (const fs::path* f : {&it.file, &it.key_file})
      if (!f->empty() && !policy.allowed(*f)) it.file_blocked.push_back(*f);
    it.key = cert_key(it.file, it.key_file);
    if (!it.file_blocked.empty() || !listed(it.key)) {
      it.text = "the client certificate " + to_utf8(it.file) +
                (it.key_file.empty() ? std::string() : " with the key " + to_utf8(it.key_file)) +
                (it.file_blocked.empty() ? "" : " (outside the workspace)");
      out.push_back(std::move(it));
    }
  }
  return out;
}

bool Workspace::allow_network_item(const NetworkItem& item, std::string* err) const {
  for (const auto& f : item.file_blocked)
    if (!allow_file(f, err)) return false;
  // A CA file not read yet: only the file is allowed now; its content is asked about next.
  if (item.kind == NetworkItem::Kind::Ca && (!item.file_blocked.empty() || item.fingerprint.empty())) return true;
  if (item.key.empty()) return false;
  std::vector<std::string> list = read_root_list(kAllowedNetworkFile, root_);
  if (std::find(list.begin(), list.end(), item.key) != list.end()) return true;
  if (item.kind == NetworkItem::Kind::Ca) {  // one approved version per CA file
    const std::string suffix = " " + generic_utf8(item.file);
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const std::string& k) {
                                return k.rfind("ca ", 0) == 0 && k.size() > suffix.size() &&
                                       k.compare(k.size() - suffix.size(), suffix.size(), suffix) == 0;
                              }),
               list.end());
  }
  list.push_back(item.key);
  return write_root_list(kAllowedNetworkFile, root_, list, "network settings allowlist", err);
}

bool Workspace::disallow_network_item(const NetworkItem& item, std::string* err) const {
  std::vector<std::string> list = read_root_list(kAllowedNetworkFile, root_);
  const auto end = std::remove(list.begin(), list.end(), item.key);
  if (end == list.end()) return true;
  list.erase(end, list.end());
  return write_root_list(kAllowedNetworkFile, root_, list, "network settings allowlist", err);
}

std::vector<std::string> Workspace::allowed_network_items() const {
  return read_root_list(kAllowedNetworkFile, root_);
}

std::optional<HttpRequest> Workspace::http_to_send(const SavedRequest& r, std::string* err) const {
  // A folder.json on the way that cannot be read: its headers / auth would be missing.
  const FolderChain chain = folder_chain(r);
  if (!chain.errors.empty()) {
    if (err) *err = "folder settings: " + chain.errors.front() + ": request not sent.";
    return std::nullopt;
  }
  auto expand = [&](const std::string& s) { return expand_in(s, &chain); };
  // An OS environment variable this workspace may not read: not sent (the UI asks first).
  SavedRequest x = expanded_with(r, chain);
  if (const auto env = env_refs_left(x, &chain); !env.empty()) {
    if (err)
      *err = "the request uses the environment variable " + env.front() +
             " ({{process.env." + env.front() + "}}), which this workspace may not read: request not sent. The user "
             "must allow it for this workspace first.";
    return std::nullopt;
  }
  if (!x.disabled_headers.empty()) {
    std::vector<Header> on;
    for (size_t k = 0; k < x.http.headers.size(); ++k)
      if (!x.disabled_headers.count(k)) on.push_back(std::move(x.http.headers[k]));
    x.http.headers = std::move(on);
    x.disabled_headers.clear();
  }
  // Every file this send reads is resolved (root-relative, symlinks resolved) and checked first:
  // a blocked one fails the whole send, naming the path.
  const UploadPolicy policy(root_);
  bool any = false;
  for (const auto& p : x.form) any = any || p.enabled;
  if (any) {
    for (auto& p : x.form) {
      if (!p.enabled || p.kind == FormPart::Kind::Text) continue;
      const fs::path fp = policy.resolve(p.value);
      if (!policy.allowed(fp)) {
        if (err) *err = policy.blocked_message("form part '" + p.name + "'", fp);
        return std::nullopt;
      }
      // Read exactly the checked file; the part keeps the file name it was given (curl's).
      if (p.kind == FormPart::Kind::File && p.filename.empty())
        p.filename = p.value.substr(p.value.find_last_of("/\\") + 1);
      p.value = to_utf8(fp);
    }
    if (!build_form_body(x.form, x.http, root_, err)) return std::nullopt;
  }
  if (!any && !x.body_file.empty()) {
    const fs::path fp = policy.resolve(expand(x.body_file));
    if (!policy.allowed(fp)) {
      if (err) *err = policy.blocked_message("body file", fp);
      return std::nullopt;
    }
    if (!detail::read_file(fp, x.http.body)) {
      if (err) *err = "cannot read body file " + to_utf8(fp);
      return std::nullopt;
    }
  }
  // curl -d @file, -H @file, -b file: each file checked, then read (as curl reads it).
  auto read_ref = [&](const std::string& what, const std::string& ref, std::string& out) {
    const fs::path fp = policy.resolve(ref);
    if (!policy.allowed(fp)) {
      if (err) *err = policy.blocked_message(what, fp);
      return false;
    }
    if (!detail::read_file(fp, out)) {
      if (err) *err = "cannot read " + what + " " + to_utf8(fp);
      return false;
    }
    return true;
  };
  if (!any && x.body_file.empty() && !x.body_data.empty()) {
    std::vector<CurlData> data = x.body_data;
    for (auto& d : data) d.value = expand(d.value);
    if (!build_curl_data_body(data, [&](const std::string& f, std::string& out) { return read_ref("data file", f, out); },
                              x.http.body))
      return std::nullopt;
  }
  bool file_cookie = false;  // a Cookie header from a header file: curl then drops the -b cookies
  for (const auto& f : x.header_files) {
    std::string content;
    if (!read_ref("header file", expand(f), content)) return std::nullopt;
    std::vector<Header> from_file;
    add_curl_header_file(from_file, content);
    for (const auto& [k, v] : from_file) file_cookie = file_cookie || lower_ascii(k) == "cookie";
    add_curl_header_file(x.http.headers, content);
  }
  if (!file_cookie && !x.cookie_files.empty()) {
    std::string jar_cookies;
    for (const auto& f : x.cookie_files) {
      std::string jar;
      if (!read_ref("cookie file", expand(f), jar)) return std::nullopt;
      const std::string c = curl_jar_cookies(jar, x.http.url);
      if (!c.empty()) jar_cookies += (jar_cookies.empty() ? "" : "; ") + c;
    }
    if (!jar_cookies.empty()) {
      bool merged = false;  // the jar's cookies go first, as curl's
      for (auto& [k, v] : x.http.headers)
        if (lower_ascii(k) == "cookie") {
          v = v.empty() ? jar_cookies : jar_cookies + "; " + v;
          merged = true;
          break;
        }
      if (!merged) x.http.headers.emplace_back("Cookie", jar_cookies);
    }
  }
  // CA bundle, client certificate (mTLS) and proxy (NetworkSettings, already resolved and
  // expanded): all come from workspace files, so each is used only once the user approved it
  // (blocked_network_settings()); files read under the same policy as uploads, the passphrase
  // from secrets.json. Fail closed: anything not approved stops the send, naming it.
  {
    const NetworkSettings& n = x.network;
    const auto approved = read_root_list(kAllowedNetworkFile, root_);
    auto listed = [&](const std::string& k) { return std::find(approved.begin(), approved.end(), k) != approved.end(); };
    const char* how = ": request not sent. The user must allow it for this workspace first.";
    if (!n.ca_file.empty()) {
      if (!read_ref("CA certificate file", n.ca_file, x.http.ca_pem)) return std::nullopt;
      const fs::path fp = policy.resolve(n.ca_file);
      if (!listed(ca_key(x.http.ca_pem, fp))) {
        if (err)
          *err = "the workspace wants watt to trust the CA certificate file " + to_utf8(fp) + " (SHA-256 " +
                 colon_hex(crypto::digest(crypto::Hash::Sha256, x.http.ca_pem)) +
                 "), which is not approved (or changed since it was approved)" + how;
        return std::nullopt;
      }
    }
    if (n.ca_off) x.http.ca_pem.clear();
    if (!n.cert_file.empty()) {
      if (!read_ref("client certificate file", n.cert_file, x.http.client_cert)) return std::nullopt;
      if (!n.key_file.empty() && !read_ref("client key file", n.key_file, x.http.client_key)) return std::nullopt;
      const fs::path cf = policy.resolve(n.cert_file);
      const fs::path kf = n.key_file.empty() ? fs::path() : policy.resolve(n.key_file);
      if (!listed(cert_key(cf, kf))) {
        if (err)
          *err = "the workspace wants to present the client certificate " + to_utf8(cf) +
                 (kf.empty() ? std::string() : " (key " + to_utf8(kf) + ")") + ", which is not approved" + how;
        return std::nullopt;
      }
      x.http.client_key_passphrase = cert_passphrase(x.cert_from_key);
    }
    switch (n.proxy) {
      case NetworkSettings::Proxy::System: x.http.proxy = HttpRequest::Proxy::System; break;
      case NetworkSettings::Proxy::Off: x.http.proxy = HttpRequest::Proxy::Off; break;
      case NetworkSettings::Proxy::Url:
        x.http.proxy = n.proxy_url.empty() ? HttpRequest::Proxy::Off : HttpRequest::Proxy::Url;
        break;
      default: x.http.proxy = HttpRequest::Proxy::Env; break;
    }
    x.http.no_proxy = n.no_proxy;
    x.http.proxy_url.clear();
    if (has_ctl_break(n.proxy_url) || has_ctl_break(x.http.no_proxy)) {
      if (err) *err = "the proxy setting contains a line break: request not sent.";
      return std::nullopt;
    }
    if (x.http.proxy == HttpRequest::Proxy::Url) {
      // A password typed into the proxy URL would sit in plain text in a git-tracked file.
      if (const std::string pw = literal_proxy_password(effective_settings(r).network.proxy_url);
          !pw.empty() && pw.find("{{") == std::string::npos) {
        if (err)
          *err = "the proxy URL holds a password in plain text in a workspace file (shared through git): put it "
                 "in a secret variable, e.g. http://user:{{proxyPassword}}@host:port: request not sent.";
        return std::nullopt;
      }
      bool creds = false;
      std::string url;
      const std::string origin = proxy_origin(n.proxy_url, &creds, &url);
      if (origin.empty()) {
        if (err)
          *err = "the proxy URL must be a valid http://, https://, socks5:// or socks5h:// URL "
                 "(scheme://host:port): request not sent.";
        return std::nullopt;
      }
      if (!listed("proxy " + origin)) {
        if (err)
          *err = "the workspace wants to send all requests through the proxy " + origin +
                 ", which is not approved" + how;
        return std::nullopt;
      }
      x.http.proxy_url = std::move(url);  // rebuilt from the parsed parts: exactly the approved host
    }
  }
  // No header may carry a line break (header injection / request splitting), own or inherited.
  for (const auto& [k, v] : x.http.headers)
    if (has_ctl_break(k) || has_ctl_break(v)) {
      if (err) *err = "the header '" + k.substr(0, k.find_first_of(std::string_view("\r\n\0", 3))) +
                      "' contains a line break: request not sent.";
      return std::nullopt;
    }
  if (x.auth.type != HttpAuth::Type::None) {
    if (!apply_http_auth(r, x, err)) return std::nullopt;
  } else if (x.basic_auth.enabled) {
    bool own = false;  // the request's own Authorization header wins, as with curl -u -H
    for (const auto& [k, v] : x.http.headers) own = own || lower_ascii(k) == "authorization";
    if (!own)
      x.http.headers.emplace_back("Authorization",
                                  "Basic " + detail::base64(x.basic_auth.user + ":" + x.basic_auth.password));
  }
  return std::move(x.http);
}

// The non-Basic schemes of `x` (already expanded) into x.http: libcurl options for Digest / NTLM /
// Negotiate / SigV4, or a token from the OAuth 2.0 cache (fetched or refreshed now if needed).
bool Workspace::apply_http_auth(const SavedRequest& r, SavedRequest& x, std::string* err) const {
  const HttpAuth& au = x.auth;
  auto has_header = [&](std::string_view name) {
    for (const auto& [k, v] : x.http.headers)
      if (lower_ascii(k) == name) return true;
    return false;
  };
  auto bad_text = [](const std::string& v) {
    return std::any_of(v.begin(), v.end(), [](char c) { return c == '\r' || c == '\n' || c == '\0'; });
  };
  switch (au.type) {
    case HttpAuth::Type::None: return true;
    case HttpAuth::Type::Inherit: return true;  // resolved before: never reaches here
    case HttpAuth::Type::Digest:
    case HttpAuth::Type::Ntlm:
    case HttpAuth::Type::Negotiate:
      if (au.type == HttpAuth::Type::Digest && au.user.empty()) {
        if (err) *err = "Digest auth needs a user name: request not sent.";
        return false;
      }
      if (au.type != HttpAuth::Type::Digest && au.user.empty()) {
        // Implicit Windows credentials: only to an origin the user allowed (blocked_sso_hosts()).
        const std::string origin = url_origin(x.http.url);
        if (origin.empty() || !is_sso_host_allowed(origin)) {
          if (err)
            *err = "the request wants to sign in to " + (origin.empty() ? x.http.url : origin) +
                   " with your Windows account (" + auth_type_name(au.type) +
                   " without a user name), which is not allowed for this workspace: request not sent. The user "
                   "must allow single sign-on to this host first, or enter a user name and password.";
          return false;
        }
      }
      x.http.auth = au.type == HttpAuth::Type::Digest ? HttpRequest::Auth::Digest
                    : au.type == HttpAuth::Type::Ntlm ? HttpRequest::Auth::Ntlm
                                                      : HttpRequest::Auth::Negotiate;
      x.http.auth_user = au.user;
      x.http.auth_password = au.password;
      return true;
    case HttpAuth::Type::AwsSigV4: {
      auto field_ok = [](const std::string& v) {
        if (v.empty()) return false;
        for (const char c : v)
          if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                c == '.'))
            return false;
        return true;
      };
      if (au.aws_access_key.empty() || au.aws_secret_key.empty()) {
        if (err) *err = "AWS Signature v4 needs an access key id and a secret access key: request not sent.";
        return false;
      }
      if (!field_ok(au.aws_region) || !field_ok(au.aws_service)) {
        if (err) *err = "AWS Signature v4 needs a region and a service (letters, digits, '-', '_', '.'): request not sent.";
        return false;
      }
      if (bad_text(au.aws_session_token)) {
        if (err) *err = "the AWS session token contains a line break: request not sent.";
        return false;
      }
      x.http.aws_sigv4 = "aws:amz:" + au.aws_region + ":" + au.aws_service;
      x.http.auth_user = au.aws_access_key;
      x.http.auth_password = au.aws_secret_key;
      if (!au.aws_session_token.empty() && !has_header("x-amz-security-token"))
        x.http.headers.emplace_back("X-Amz-Security-Token", au.aws_session_token);
      return true;
    }
    case HttpAuth::Type::OAuth2: {
      OAuth2Hooks hooks;  // non-interactive: an authorization code grant must have been run first
      const OAuth2Result t = oauth2_get_token(*this, r, hooks);
      if (!t.ok) {
        if (err) *err = "OAuth 2.0: " + t.error + ": request not sent.";
        return false;
      }
      std::string e;
      if (!oauth2_apply_token(x.http, x.auth.oauth2, t.token, &e)) {
        if (err) *err = "OAuth 2.0: " + e + ": request not sent.";
        return false;
      }
      return true;
    }
  }
  return true;
}

const char* auth_type_name(HttpAuth::Type t) {
  switch (t) {
    case HttpAuth::Type::None: return "none";
    case HttpAuth::Type::Digest: return "digest";
    case HttpAuth::Type::Ntlm: return "ntlm";
    case HttpAuth::Type::Negotiate: return "negotiate";
    case HttpAuth::Type::AwsSigV4: return "awsv4";
    case HttpAuth::Type::OAuth2: return "oauth2";
    case HttpAuth::Type::Inherit: return "inherit";
  }
  return "none";
}

HttpAuth::Type auth_type_from(const std::string& s) {
  const std::string l = lower_ascii(s);
  if (l == "digest") return HttpAuth::Type::Digest;
  if (l == "ntlm") return HttpAuth::Type::Ntlm;
  if (l == "negotiate" || l == "kerberos" || l == "spnego") return HttpAuth::Type::Negotiate;
  if (l == "awsv4" || l == "aws_sigv4" || l == "awssigv4" || l == "aws") return HttpAuth::Type::AwsSigV4;
  if (l == "oauth2" || l == "oauth2.0" || l == "oauth") return HttpAuth::Type::OAuth2;
  if (l == "inherit") return HttpAuth::Type::Inherit;
  return HttpAuth::Type::None;
}

// ------------------------------------------------------------------ single sign-on allowlist

// The origin the transport will actually connect to, decided by libcurl's own URL parser (the
// same one the HTTP engine hands the URL to), so the allowlist check and the connection can
// never disagree about the host ("http://good\@evil/" and friends). Anything that is not a
// plain http(s) URL, or that carries a backslash or user info, has no origin: implicit Windows
// credentials are then refused (fail closed).
std::string Workspace::url_origin(const std::string& url) {
  if (url.find('\\') != std::string::npos) return {};
  for (const char c : url)
    if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F) return {};
  CURLU* u = curl_url();
  if (!u) return {};
  std::string out;
  if (curl_url_set(u, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK) {
    char *scheme = nullptr, *host = nullptr, *port = nullptr, *user = nullptr, *pass = nullptr;
    const bool got = curl_url_get(u, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
                     curl_url_get(u, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
                     curl_url_get(u, CURLUPART_PORT, &port, CURLU_DEFAULT_PORT) == CURLUE_OK;
    const bool has_user = curl_url_get(u, CURLUPART_USER, &user, 0) == CURLUE_OK && user && *user;
    const bool has_pass = curl_url_get(u, CURLUPART_PASSWORD, &pass, 0) == CURLUE_OK && pass && *pass;
    if (got && !has_user && !has_pass) {
      const std::string s = lower_ascii(scheme);
      std::string h = lower_ascii(host);
      while (!h.empty() && h.back() == '.') h.pop_back();  // "example.com." is example.com
      if ((s == "http" || s == "https") && !h.empty()) out = s + "://" + h + ":" + port;
    }
    curl_free(scheme);
    curl_free(host);
    curl_free(port);
    curl_free(user);
    curl_free(pass);
  }
  curl_url_cleanup(u);
  return out;
}

std::vector<std::string> Workspace::blocked_sso_hosts(const SavedRequest& r) const {
  if (r.kind != RequestKind::Http) return {};
  // The auth may be inherited from a folder or the collection.
  if (r.auth.type != HttpAuth::Type::Ntlm && r.auth.type != HttpAuth::Type::Negotiate &&
      r.auth.type != HttpAuth::Type::Inherit)
    return {};
  const SavedRequest x = expanded(r);
  if (x.auth.type != HttpAuth::Type::Ntlm && x.auth.type != HttpAuth::Type::Negotiate) return {};
  if (!x.auth.user.empty()) return {};
  const std::string origin = url_origin(x.http.url);
  // Not an http(s) URL (yet): nothing to ask; http_to_send() refuses it anyway.
  if (origin.empty() || is_sso_host_allowed(origin)) return {};
  return {origin};
}

bool Workspace::is_sso_host_allowed(const std::string& origin_or_url) const {
  const std::string o = url_origin(origin_or_url);
  if (o.empty()) return false;
  const auto list = read_root_list(kAllowedSsoFile, root_);
  return std::find(list.begin(), list.end(), o) != list.end();
}

std::vector<std::string> Workspace::allowed_sso_hosts() const { return read_root_list(kAllowedSsoFile, root_); }

bool Workspace::allow_sso_host(const std::string& origin_or_url, std::string* err) const {
  const std::string o = url_origin(origin_or_url);
  if (o.empty()) {
    if (err) *err = "not an http(s) origin: " + origin_or_url;
    return false;
  }
  std::vector<std::string> list = read_root_list(kAllowedSsoFile, root_);
  if (std::find(list.begin(), list.end(), o) != list.end()) return true;
  list.push_back(o);
  return write_root_list(kAllowedSsoFile, root_, list, "single sign-on allowlist", err);
}

bool Workspace::disallow_sso_host(const std::string& origin_or_url, std::string* err) const {
  const std::string o = url_origin(origin_or_url);
  std::vector<std::string> list = read_root_list(kAllowedSsoFile, root_);
  const auto end = std::remove(list.begin(), list.end(), o);
  if (end == list.end()) return true;
  list.erase(end, list.end());
  return write_root_list(kAllowedSsoFile, root_, list, "single sign-on allowlist", err);
}

// ------------------------------------------------------------------ script trust

bool Workspace::scripts_trusted() const {
  const auto list = read_root_list(kTrustedScriptsFile, root_);
  return std::find(list.begin(), list.end(), "scripts") != list.end();
}

bool Workspace::trust_scripts(bool trusted, std::string* err) const {
  if (trusted == scripts_trusted()) return true;
  return write_root_list(kTrustedScriptsFile, root_,
                         trusted ? std::vector<std::string>{"scripts"} : std::vector<std::string>{},
                         "script trust decision", err);
}

}  // namespace watt
