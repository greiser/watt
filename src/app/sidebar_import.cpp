#include "app/sidebar_import.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <atomic>
#include <chrono>
#include <random>
#include <functional>
#include <thread>
#include <set>
#include <system_error>
#include <unordered_map>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/pointer.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "app/sidebar_files.h"
#include "app/sidebar_fuzzy.h"

namespace watt::app::sidebar {

namespace fs = std::filesystem;
using rapidjson::Document;
using rapidjson::Value;

namespace {

constexpr size_t kMaxImportBytes = 64u << 20;
constexpr size_t kMaxRequests = 50000;
constexpr int kMaxDepth = 64;

std::string u8s(const fs::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}
fs::path from_u8s(std::string_view s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string lower_ascii(std::string_view s) {
  std::string o(s);
  for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return o;
}
std::string upper(std::string_view s) {
  std::string o(s);
  for (char& c : o) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return o;
}
std::string trim(std::string_view s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return std::string(s.substr(a, b - a));
}
bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  return true;
}

// ---------------------------------------------------------------- JSON access

const Value* mem(const Value* o, const char* k) {
  if (!o || !o->IsObject()) return nullptr;
  auto it = o->FindMember(k);
  return it == o->MemberEnd() ? nullptr : &it->value;
}
const Value* mem(const Value& o, const char* k) { return mem(&o, k); }

std::string json_text(const Value& v) {
  rapidjson::StringBuffer b;
  rapidjson::Writer<rapidjson::StringBuffer> w(b);
  v.Accept(w);
  return std::string(b.GetString(), b.GetSize());
}
std::string json_pretty_text(const Value& v) {
  rapidjson::StringBuffer b;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> w(b);
  w.SetIndent(' ', 2);
  v.Accept(w);
  return std::string(b.GetString(), b.GetSize());
}

// A scalar as text (strings as they are, numbers and bools printed, objects / arrays as JSON).
std::string str(const Value* v) {
  if (!v || v->IsNull()) return {};
  if (v->IsString()) return std::string(v->GetString(), v->GetStringLength());
  if (v->IsBool()) return v->GetBool() ? "true" : "false";
  if (v->IsInt64()) return std::to_string(v->GetInt64());
  if (v->IsUint64()) return std::to_string(v->GetUint64());
  if (v->IsNumber()) {
    char b[40];
    std::snprintf(b, sizeof b, "%.17g", v->GetDouble());
    return b;
  }
  return json_text(*v);
}
std::string str(const Value& o, const char* k) { return str(mem(o, k)); }
bool flag(const Value& o, const char* k) {
  const Value* v = mem(o, k);
  return v && v->IsBool() && v->GetBool();
}
const Value* arr(const Value* o, const char* k) {
  const Value* v = mem(o, k);
  return v && v->IsArray() ? v : nullptr;
}
const Value* arr(const Value& o, const char* k) { return arr(&o, k); }

// ---------------------------------------------------------------- request helpers

bool method_ok(const std::string& m) {
  if (m.empty() || m.size() > 32) return false;
  for (unsigned char c : m)
    if (!(std::isalnum(c) || std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) != std::string_view::npos))
      return false;
  return true;
}

// Why a URL is refused ("" when it is fine): only http and https (or no scheme / a {{var}} base).
std::string url_problem(const std::string& url) {
  const size_t p = url.find("://");
  if (p == std::string::npos) {
    const std::string l = lower_ascii(url.substr(0, 7));
    if (starts_with(l, "file:") || starts_with(l, "data:") || starts_with(l, "javascript:")) return "not an http(s) URL";
    return {};
  }
  const std::string s = url.substr(0, p);
  for (unsigned char c : s)
    if (!(std::isalnum(c) || c == '+' || c == '-' || c == '.')) return {};  // "{{base}}://" and the like
  const std::string l = lower_ascii(s);
  if (l == "http" || l == "https") return {};
  return l + ":// URL (only http and https)";
}

bool has_header(const SavedRequest& r, std::string_view name) {
  for (const auto& h : r.http.headers)
    if (iequals(h.first, name)) return true;
  return false;
}
void add_header(SavedRequest& r, std::string name, std::string value, bool disabled = false) {
  if (disabled) r.disabled_headers.insert(r.http.headers.size());
  r.http.headers.emplace_back(std::move(name), std::move(value));
}
void default_header(SavedRequest& r, std::string_view name, std::string value) {
  if (!has_header(r, name)) add_header(r, std::string(name), std::move(value));
}

// Percent-encodes for a query / form value, keeping {{vars}} intact.
std::string form_encode(std::string_view s) {
  static const char* hex = "0123456789ABCDEF";
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == '{' && i + 1 < s.size() && s[i + 1] == '{') {
      const size_t e = s.find("}}", i + 2);
      if (e != std::string_view::npos) {
        o.append(s.substr(i, e + 2 - i));
        i = e + 1;
        continue;
      }
    }
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += static_cast<char>(c);
    else {
      o += '%';
      o += hex[c >> 4];
      o += hex[c & 15];
    }
  }
  return o;
}
void append_query(std::string& url, const std::string& k, const std::string& v) {
  const size_t hash = url.find('#');
  std::string frag;
  if (hash != std::string::npos) {
    frag = url.substr(hash);
    url.resize(hash);
  }
  url += url.find('?') == std::string::npos ? '?' : '&';
  url += form_encode(k);
  url += '=';
  url += form_encode(v);
  url += frag;
}

// "{{ _.base_url }}" / "{{_.a}}" / "{{ a }}" -> "{{base_url}}" / "{{a}}" (Insomnia's Nunjucks).
std::string insomnia_vars(std::string s, bool* tags = nullptr) {
  std::string o;
  for (size_t i = 0; i < s.size();) {
    if (s.compare(i, 2, "{{") == 0) {
      const size_t e = s.find("}}", i + 2);
      if (e != std::string::npos) {
        std::string inner = trim(std::string_view(s).substr(i + 2, e - i - 2));
        if (starts_with(inner, "_.")) inner.erase(0, 2);
        o += "{{" + inner + "}}";
        i = e + 2;
        continue;
      }
    }
    if (tags && s.compare(i, 2, "{%") == 0) *tags = true;
    o += s[i++];
  }
  return o;
}

// ---------------------------------------------------------------- auth

struct AuthSpec {
  std::string type;  // "basic", "bearer", ..., "" / "noauth" / "none": nothing, "inherit"
  std::map<std::string, std::string> kv;
};

std::string kv_get(const AuthSpec& a, std::initializer_list<const char*> keys) {
  for (const char* k : keys)
    if (auto it = a.kv.find(k); it != a.kv.end()) return it->second;
  return {};
}

void apply_auth(SavedRequest& r, const AuthSpec& a, ImportResult& res) {
  const std::string t = lower_ascii(a.type);
  if (t.empty() || t == "noauth" || t == "none" || t == "inherit") return;
  if (t == "basic") {
    r.basic_auth.enabled = true;
    r.basic_auth.user = kv_get(a, {"username", "user"});
    r.basic_auth.password = kv_get(a, {"password"});
  } else if (t == "bearer") {
    std::string prefix = kv_get(a, {"prefix"});
    if (prefix.empty()) prefix = "Bearer";
    if (!has_header(r, "Authorization")) add_header(r, "Authorization", prefix + " " + kv_get(a, {"token"}));
  } else if (t == "apikey") {
    const std::string k = kv_get(a, {"key"}), v = kv_get(a, {"value"});
    const std::string in = lower_ascii(kv_get(a, {"in", "addTo", "placement"}));
    if (k.empty()) return;
    if (in == "query" || in == "queryparams" || in == "queryparam") append_query(r.http.url, k, v);
    else if (!has_header(r, k)) add_header(r, k, v);
  } else if (t == "digest" || t == "ntlm") {
    r.auth.type = t == "digest" ? HttpAuth::Type::Digest : HttpAuth::Type::Ntlm;
    r.auth.user = kv_get(a, {"username", "user"});
    const std::string domain = kv_get(a, {"domain"});
    if (!domain.empty() && r.auth.user.find('\\') == std::string::npos) r.auth.user = domain + "\\" + r.auth.user;
    r.auth.password = kv_get(a, {"password"});
  } else if (t == "awsv4" || t == "iam") {
    r.auth.type = HttpAuth::Type::AwsSigV4;
    r.auth.aws_access_key = kv_get(a, {"accessKey", "accessKeyId"});
    r.auth.aws_secret_key = kv_get(a, {"secretKey", "secretAccessKey"});
    r.auth.aws_session_token = kv_get(a, {"sessionToken"});
    r.auth.aws_region = kv_get(a, {"region"});
    r.auth.aws_service = kv_get(a, {"service"});
  } else if (t == "oauth2") {
    r.auth.type = HttpAuth::Type::OAuth2;
    OAuth2Config& o = r.auth.oauth2;
    const std::string g = lower_ascii(kv_get(a, {"grant_type", "grantType"}));
    o.grant = g.find("password") != std::string::npos ? OAuth2Config::Grant::Password
              : g.find("code") != std::string::npos ? OAuth2Config::Grant::AuthorizationCode
                                                    : OAuth2Config::Grant::ClientCredentials;
    o.token_url = kv_get(a, {"accessTokenUrl", "access_token_url", "tokenUrl"});
    o.auth_url = kv_get(a, {"authUrl", "authorizationUrl", "authorization_url"});
    o.client_id = kv_get(a, {"clientId", "client_id"});
    o.client_secret = kv_get(a, {"clientSecret", "client_secret"});
    o.scope = kv_get(a, {"scope"});
    o.audience = kv_get(a, {"audience"});
    o.username = kv_get(a, {"username"});
    o.password = kv_get(a, {"password"});
    const std::string redirect = kv_get(a, {"redirect_uri", "redirectUrl", "callbackUrl", "callback_url"});
    if (starts_with(redirect, "http://127.0.0.1") || starts_with(redirect, "http://localhost")) o.redirect_uri = redirect;
  } else {
    res.notes.push_back(r.name + ": " + a.type + " auth is not imported");
  }
}

// A folder's (or the collection's) own auth as that folder's settings, so its requests inherit
// it instead of each getting a copy. False when it cannot be a folder setting (an API key in the
// query string): the requests get it themselves then.
bool folder_auth(const AuthSpec& a, ImportFolder& f, ImportResult& res) {
  const std::string t = lower_ascii(a.type);
  if (t.empty() || t == "inherit") return false;
  SavedRequest x;
  x.name = f.name;
  apply_auth(x, a, res);
  if (!x.http.url.empty()) return false;
  for (const auto& [k, v] : x.http.headers) f.headers.push_back(SettingsRow{k, v, {}, true});
  f.auth = x.auth;  // None unless digest / NTLM / AWS / OAuth 2.0: "no scheme here" stops the walk up
  f.basic_auth = x.basic_auth;
  return true;
}

// ---------------------------------------------------------------- result building

// Adds r to f unless it cannot be sent (bad method, non-http(s) URL), then records why.
void add_request(ImportResult& res, ImportFolder& f, SavedRequest r, size_t& count) {
  if (r.name.empty()) r.name = r.kind == RequestKind::Grpc ? r.grpc_method : r.http.method + " " + r.http.url;
  if (r.name.empty()) r.name = "Request";
  if (r.name.size() > 200) r.name.resize(200);
  if (count >= kMaxRequests) {
    res.skipped.push_back(r.name + ": more than 50000 requests");
    return;
  }
  if (r.kind == RequestKind::Http) {
    r.http.method = upper(trim(r.http.method.empty() ? "GET" : r.http.method));
    r.http.url = trim(r.http.url);
    if (!method_ok(r.http.method)) {
      res.skipped.push_back(r.name + ": method is not an HTTP token");
      return;
    }
    if (r.http.url.empty()) {
      res.skipped.push_back(r.name + ": no URL");
      return;
    }
    if (const std::string why = url_problem(r.http.url); !why.empty()) {
      res.skipped.push_back(r.name + ": " + why);
      return;
    }
  }
  ++count;
  f.requests.push_back(std::move(r));
}

void set_env_var(Environment& e, const std::string& k, const std::string& v) {
  if (!k.empty()) e.vars[k] = v;
}

// A collection / folder variable: a later definition of the same name replaces the earlier one.
void set_folder_var(ImportFolder& f, const std::string& k, const std::string& v, bool enabled = true) {
  if (k.empty()) return;
  for (auto& row : f.vars)
    if (row.name == k) {
      row.value = v;
      row.enabled = enabled;
      return;
    }
  SettingsRow row;
  row.name = k;
  row.value = v;
  row.enabled = enabled;
  f.vars.push_back(std::move(row));
}

// ======================================================================= Postman v2.0 / v2.1

struct PmCtx {
  ImportResult& res;
  size_t count = 0;
};

AuthSpec pm_auth(const Value* a) {
  AuthSpec s;
  if (!a || !a->IsObject()) return s;
  s.type = str(*a, "type");
  const Value* d = mem(a, s.type.c_str());
  if (d && d->IsArray()) {
    for (const auto& e : d->GetArray())
      if (e.IsObject()) s.kv[str(e, "key")] = str(mem(e, "value"));
  } else if (d && d->IsObject()) {
    for (auto it = d->MemberBegin(); it != d->MemberEnd(); ++it) s.kv[it->name.GetString()] = str(&it->value);
  }
  // An OAuth 2.0 token Postman already holds: sent as a bearer token, as Postman does.
  if (s.type == "oauth2" && s.kv.count("accessToken") && !s.kv["accessToken"].empty() && !s.kv.count("accessTokenUrl")) {
    s.type = "bearer";
    s.kv["token"] = s.kv["accessToken"];
  }
  return s;
}

std::string pm_script_lines(const Value* script) {
  if (!script) return {};
  const Value* exec = mem(script, "exec");
  if (!exec) return {};
  if (exec->IsString()) return exec->GetString();
  std::string out;
  if (exec->IsArray())
    for (const auto& l : exec->GetArray()) {
      out += str(&l);
      out += '\n';
    }
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
  return out;
}

void pm_events(const Value& o, std::string& pre, std::string& post) {
  if (const Value* ev = arr(o, "event"))
    for (const auto& e : ev->GetArray()) {
      if (!e.IsObject() || flag(e, "disabled")) continue;
      const std::string listen = str(e, "listen");
      const std::string code = pm_script_lines(mem(e, "script"));
      if (trim(code).empty()) continue;
      std::string& dst = listen == "prerequest" ? pre : listen == "test" ? post : pre;
      if (listen != "prerequest" && listen != "test") continue;
      if (!dst.empty()) dst += "\n\n";
      dst += code;
    }
}

// A collection's or folder's "variable" list: that folder's variables (folder.json), kept
// disabled when Postman has them disabled.
void pm_variables(const Value& o, ImportFolder& f) {
  if (const Value* vars = arr(o, "variable"))
    for (const auto& v : vars->GetArray())
      if (v.IsObject())
        set_folder_var(f, str(v, "key").empty() ? str(v, "id") : str(v, "key"), str(mem(v, "value")), !flag(v, "disabled"));
}

std::string pm_description(const Value* d) {
  if (!d) return {};
  if (d->IsString()) return d->GetString();
  return str(mem(d, "content"));
}

void pm_url(const Value* u, SavedRequest& r) {
  if (!u) return;
  if (u->IsString()) {
    r.http.url = u->GetString();
    return;
  }
  if (!u->IsObject()) return;
  std::string raw = str(*u, "raw");
  if (raw.empty()) {
    // Built from its parts (an export without "raw").
    std::string proto = str(*u, "protocol");
    std::string host;
    if (const Value* h = mem(u, "host"); h && h->IsArray()) {
      for (const auto& p : h->GetArray()) host += (host.empty() ? "" : ".") + str(&p);
    } else {
      host = str(mem(u, "host"));
    }
    raw = (proto.empty() ? "" : proto + "://") + host;
    if (const std::string port = str(*u, "port"); !port.empty()) raw += ":" + port;
    if (const Value* p = mem(u, "path"); p && p->IsArray()) {
      for (const auto& s : p->GetArray()) raw += "/" + (s.IsObject() ? str(s, "value") : str(&s));
    } else if (const std::string ps = str(mem(u, "path")); !ps.empty()) {
      raw += (ps[0] == '/' ? "" : "/") + ps;
    }
    if (const Value* q = arr(u, "query"))
      for (const auto& e : q->GetArray())
        if (e.IsObject() && !flag(e, "disabled")) append_query(raw, str(e, "key"), str(mem(e, "value")));
  } else if (const Value* q = arr(u, "query")) {
    // "raw" holds the enabled params; a disabled one that is still in it is taken out.
    std::vector<std::string> parts;
    std::string base = raw, frag;
    if (const size_t h = base.find('#'); h != std::string::npos) frag = base.substr(h), base.resize(h);
    if (const size_t qm = base.find('?'); qm != std::string::npos) {
      const std::string qs = base.substr(qm + 1);
      base.resize(qm);
      for (size_t p = 0; p <= qs.size();) {
        size_t e = qs.find('&', p);
        if (e == std::string::npos) e = qs.size();
        if (e > p) parts.push_back(qs.substr(p, e - p));
        p = e + 1;
      }
    }
    bool removed = false;
    for (const auto& e : q->GetArray()) {
      if (!e.IsObject() || !flag(e, "disabled")) continue;
      const std::string k = str(e, "key"), v = str(mem(e, "value"));
      for (const std::string& needle : {k + "=" + v, k + "=" + form_encode(v), v.empty() ? k : k + "=" + v}) {
        auto it = std::find(parts.begin(), parts.end(), needle);
        if (it == parts.end()) continue;
        parts.erase(it);
        removed = true;
        break;
      }
      r.disabled_params.emplace_back(k, v);
    }
    if (removed) {
      raw = base;
      for (size_t i = 0; i < parts.size(); ++i) raw += (i ? "&" : "?") + parts[i];
      raw += frag;
    }
  }
  r.http.url = raw;
  if (const Value* vars = arr(u, "variable"))
    for (const auto& v : vars->GetArray())
      if (v.IsObject()) r.path_params.emplace_back(str(v, "key"), str(mem(v, "value")));
}

void pm_body(const Value* b, SavedRequest& r) {
  if (!b || !b->IsObject() || flag(*b, "disabled")) return;
  const std::string mode = str(*b, "mode");
  if (mode == "raw") {
    r.http.body = str(*b, "raw");
    std::string lang;
    if (const Value* o = mem(mem(b, "options"), "raw")) lang = str(*o, "language");
    if (lang == "json") default_header(r, "Content-Type", "application/json");
    else if (lang == "xml") default_header(r, "Content-Type", "application/xml");
    else if (lang == "html") default_header(r, "Content-Type", "text/html");
    else if (lang == "javascript") default_header(r, "Content-Type", "application/javascript");
    else if (lang == "text" && !r.http.body.empty()) default_header(r, "Content-Type", "text/plain");
  } else if (mode == "urlencoded") {
    std::string body;
    if (const Value* a = arr(b, "urlencoded"))
      for (const auto& e : a->GetArray()) {
        if (!e.IsObject() || flag(e, "disabled")) continue;
        if (!body.empty()) body += '&';
        body += form_encode(str(e, "key")) + "=" + form_encode(str(mem(e, "value")));
      }
    r.http.body = body;
    default_header(r, "Content-Type", "application/x-www-form-urlencoded");
  } else if (mode == "formdata") {
    if (const Value* a = arr(b, "formdata"))
      for (const auto& e : a->GetArray()) {
        if (!e.IsObject()) continue;
        FormPart p;
        p.name = str(e, "key");
        p.enabled = !flag(e, "disabled");
        p.type = str(e, "contentType");
        if (str(e, "type") == "file") {
          p.kind = FormPart::Kind::File;
          const Value* src = mem(e, "src");
          p.value = src && src->IsArray() ? (src->Empty() ? std::string() : str(&(*src)[0])) : str(src);
        } else {
          p.value = str(mem(e, "value"));
        }
        r.form.push_back(std::move(p));
      }
  } else if (mode == "file") {
    r.body_file = str(mem(mem(b, "file"), "src"));
  } else if (mode == "graphql") {
    const Value* g = mem(b, "graphql");
    Document d;
    d.SetObject();
    auto& al = d.GetAllocator();
    const std::string q = str(mem(g, "query"));
    d.AddMember("query", Value(q.c_str(), static_cast<rapidjson::SizeType>(q.size()), al), al);
    Document vars;
    const std::string vs = str(mem(g, "variables"));
    if (!vs.empty() && !vars.Parse(vs.c_str(), vs.size()).HasParseError()) d.AddMember("variables", Value(vars, al), al);
    r.http.body = json_pretty_text(d);
    default_header(r, "Content-Type", "application/json");
  }
}

// Something under this item ("item" list) sets its own auth. Then a folder's auth stays a copy
// on each request: as a folder header it would also reach a request that says "no auth" or uses
// another scheme (watt's folder headers are inherited whatever the request's auth).
bool pm_descendant_auth(const Value& it, int depth = 0) {
  const Value* items = arr(it, "item");
  if (!items || depth > kMaxDepth) return false;
  auto own = [](const Value* a) { return a && a->IsObject() && str(*a, "type") != "inherit"; };
  for (const auto& c : items->GetArray()) {
    if (!c.IsObject()) continue;
    if (own(mem(c, "auth"))) return true;
    if (const Value* rq = mem(c, "request"); rq && rq->IsObject() && own(mem(rq, "auth"))) return true;
    if (pm_descendant_auth(c, depth + 1)) return true;
  }
  return false;
}

void pm_item(const Value& it, ImportFolder& out, const AuthSpec& auth, const std::string& pre, const std::string& post,
             PmCtx& c, int depth) {
  const std::string name = str(it, "name");
  if (const Value* items = arr(it, "item")) {
    if (depth > kMaxDepth) {
      c.res.skipped.push_back(name + ": folders nested too deep");
      return;
    }
    ImportFolder f;
    f.name = name.empty() ? "Folder" : name;
    AuthSpec a = auth;
    if (const Value* fa = mem(it, "auth")) {
      a = pm_auth(fa);
      if (!pm_descendant_auth(it) && folder_auth(a, f, c.res)) a = AuthSpec{"inherit", {}};  // its requests inherit it
    }
    std::string fpre = pre, fpost = post;
    pm_events(it, fpre, fpost);
    pm_variables(it, f);
    for (const auto& child : items->GetArray())
      if (child.IsObject()) pm_item(child, f, a, fpre, fpost, c, depth + 1);
    out.folders.push_back(std::move(f));
    return;
  }
  const Value* req = mem(it, "request");
  if (!req) {
    c.res.skipped.push_back((name.empty() ? std::string("item") : name) + ": no request");
    return;
  }
  SavedRequest r;
  r.kind = RequestKind::Http;
  r.name = name;
  if (req->IsString()) {
    r.http.url = req->GetString();
  } else {
    r.http.method = str(*req, "method");
    if (r.http.method.empty()) r.http.method = "GET";
    if (const Value* h = mem(req, "header")) {
      if (h->IsArray()) {
        for (const auto& e : h->GetArray())
          if (e.IsObject()) add_header(r, str(e, "key"), str(mem(e, "value")), flag(e, "disabled"));
      } else if (h->IsString()) {
        const std::string s = h->GetString();
        for (size_t p = 0; p < s.size();) {
          size_t e = s.find('\n', p);
          if (e == std::string::npos) e = s.size();
          const std::string line = trim(std::string_view(s).substr(p, e - p));
          if (const size_t col = line.find(':'); col != std::string::npos)
            add_header(r, trim(line.substr(0, col)), trim(line.substr(col + 1)));
          p = e + 1;
        }
      }
    }
    pm_url(mem(req, "url"), r);
    pm_body(mem(req, "body"), r);
    r.docs = pm_description(mem(req, "description"));
    AuthSpec a = auth;
    if (const Value* ra = mem(req, "auth")) {
      AuthSpec own = pm_auth(ra);
      if (own.type != "inherit") a = own;
    }
    if (a.type == "inherit") r.auth.type = HttpAuth::Type::Inherit;  // a folder's / the collection's
    else apply_auth(r, a, c.res);
  }
  if (r.docs.empty()) r.docs = pm_description(mem(it, "description"));
  std::string rpre = pre, rpost = post;
  pm_events(it, rpre, rpost);
  r.pre_script = rpre;
  r.post_script = rpost;
  add_request(c.res, out, std::move(r), c.count);
}

std::optional<ImportResult> parse_postman(const Document& d, std::string* err) {
  ImportResult res;
  const Value* info = mem(d, "info");
  const std::string schema = str(mem(info, "schema"));
  res.format = schema.find("v2.0") != std::string::npos ? "Postman Collection v2.0" : "Postman Collection v2.1";
  res.root.name = str(mem(info, "name"));
  if (res.root.name.empty()) res.root.name = "Postman collection";
  PmCtx c{res};
  pm_variables(d, res.root);
  AuthSpec auth = pm_auth(mem(d, "auth"));
  if (!pm_descendant_auth(d) && folder_auth(auth, res.root, res)) auth = AuthSpec{"inherit", {}};  // the collection folder's
  std::string pre, post;
  pm_events(d, pre, post);
  const Value* items = arr(d, "item");
  if (!items) {
    if (err) *err = "the collection has no \"item\" list";
    return std::nullopt;
  }
  for (const auto& it : items->GetArray())
    if (it.IsObject()) pm_item(it, res.root, auth, pre, post, c, 1);
  return res;
}

// Postman's environment export ({"name", "values": [{key, value, enabled}], "_postman_variable_scope"}).
std::optional<ImportResult> parse_postman_env(const Document& d) {
  ImportResult res;
  res.format = "Postman environment";
  Environment e;
  e.name = str(d, "name");
  if (e.name.empty()) e.name = "Imported";
  res.root.name = e.name;
  for (const auto& v : (*arr(d, "values")).GetArray()) {
    if (!v.IsObject()) continue;
    const Value* en = mem(v, "enabled");
    if (en && en->IsBool() && !en->GetBool()) continue;
    const std::string k = str(v, "key");
    set_env_var(e, k, str(mem(v, "value")));
    if (str(v, "type") == "secret" && !k.empty()) e.secrets.insert(k);
  }
  res.envs.push_back(std::move(e));
  return res;
}

// ======================================================================= Insomnia v4

AuthSpec insomnia_auth(const Value* a) {
  AuthSpec s;
  if (!a || !a->IsObject() || flag(*a, "disabled")) return s;
  s.type = str(*a, "type");
  for (auto it = a->MemberBegin(); it != a->MemberEnd(); ++it)
    if (it->name != "type") s.kv[it->name.GetString()] = insomnia_vars(str(&it->value));
  if (s.type == "oauth2") {
    // Insomnia's grant names: client_credentials, password, authorization_code, implicit.
    s.kv["grant_type"] = s.kv["grantType"];
  }
  return s;
}

std::optional<ImportResult> parse_insomnia(const Document& d, std::string* err) {
  ImportResult res;
  res.format = "Insomnia v4";
  const Value* resources = arr(d, "resources");
  if (!resources) {
    if (err) *err = "the Insomnia export has no resources";
    return std::nullopt;
  }
  std::unordered_map<std::string, std::vector<const Value*>> kids;
  std::vector<const Value*> workspaces;
  for (const auto& r : resources->GetArray()) {
    if (!r.IsObject()) continue;
    kids[str(r, "parentId")].push_back(&r);
    if (str(r, "_type") == "workspace") workspaces.push_back(&r);
  }
  auto sort_key = [](const Value* v) {
    const Value* k = mem(v, "metaSortKey");
    return k && k->IsNumber() ? k->GetDouble() : 0.0;
  };
  size_t count = 0;
  bool tags = false;
  std::function<void(const std::string&, ImportFolder&, const AuthSpec&, int)> walk =
      [&](const std::string& parent, ImportFolder& out, const AuthSpec& inherited, int depth) {
        auto it = kids.find(parent);
        if (it == kids.end()) return;
        std::vector<const Value*> list = it->second;
        std::stable_sort(list.begin(), list.end(), [&](const Value* a, const Value* b) { return sort_key(a) < sort_key(b); });
        for (const Value* v : list) {
          const std::string type = str(*v, "_type");
          const std::string name = str(*v, "name");
          if (type == "request_group") {
            if (depth > kMaxDepth) {
              res.skipped.push_back(name + ": folders nested too deep");
              continue;
            }
            ImportFolder f;
            f.name = name.empty() ? "Folder" : name;
            AuthSpec a = inherited;
            if (AuthSpec own = insomnia_auth(mem(v, "authentication")); !own.type.empty()) a = own;
            walk(str(*v, "_id"), f, a, depth + 1);
            out.folders.push_back(std::move(f));
          } else if (type == "request") {
            SavedRequest r;
            r.name = name;
            r.http.method = str(*v, "method");
            r.http.url = insomnia_vars(str(*v, "url"), &tags);
            r.docs = str(*v, "description");
            if (const Value* hs = arr(v, "headers"))
              for (const auto& h : hs->GetArray())
                if (h.IsObject() && !str(h, "name").empty())
                  add_header(r, insomnia_vars(str(h, "name")), insomnia_vars(str(mem(h, "value")), &tags), flag(h, "disabled"));
            if (const Value* ps = arr(v, "parameters"))
              for (const auto& p : ps->GetArray()) {
                if (!p.IsObject()) continue;
                const std::string k = insomnia_vars(str(p, "name")), val = insomnia_vars(str(mem(p, "value")), &tags);
                if (flag(p, "disabled")) r.disabled_params.emplace_back(k, val);
                else append_query(r.http.url, k, val);
              }
            if (const Value* ps = arr(v, "pathParameters"))
              for (const auto& p : ps->GetArray())
                if (p.IsObject()) r.path_params.emplace_back(str(p, "name"), insomnia_vars(str(mem(p, "value"))));
            if (const Value* b = mem(v, "body"); b && b->IsObject()) {
              const std::string mime = str(*b, "mimeType");
              if (const Value* params = arr(b, "params")) {
                if (mime == "multipart/form-data") {
                  for (const auto& p : params->GetArray()) {
                    if (!p.IsObject()) continue;
                    FormPart fp;
                    fp.name = insomnia_vars(str(p, "name"));
                    fp.enabled = !flag(p, "disabled");
                    if (str(p, "type") == "file") {
                      fp.kind = FormPart::Kind::File;
                      fp.value = str(p, "fileName");
                    } else {
                      fp.value = insomnia_vars(str(mem(p, "value")), &tags);
                    }
                    r.form.push_back(std::move(fp));
                  }
                } else {
                  std::string body;
                  for (const auto& p : params->GetArray()) {
                    if (!p.IsObject() || flag(p, "disabled")) continue;
                    if (!body.empty()) body += '&';
                    body += form_encode(insomnia_vars(str(p, "name"))) + "=" +
                            form_encode(insomnia_vars(str(mem(p, "value")), &tags));
                  }
                  r.http.body = body;
                }
              } else if (const std::string fn = str(*b, "fileName"); !fn.empty()) {
                r.body_file = fn;
              } else {
                r.http.body = insomnia_vars(str(*b, "text"), &tags);
              }
              if (!mime.empty() && mime != "multipart/form-data") {
                default_header(r, "Content-Type", mime == "application/graphql" ? "application/json" : mime);
              }
            }
            AuthSpec a = inherited;
            if (const Value* av = mem(v, "authentication"); av && av->IsObject() && av->MemberCount()) {
              AuthSpec own = insomnia_auth(av);
              if (own.type != "inherit") a = own;
            }
            apply_auth(r, a, res);
            auto script = [](std::string s) {
              if (s.find("insomnia.") != std::string::npos) s = "var insomnia = pm;\n" + s;
              return s;
            };
            if (const std::string s = str(*v, "preRequestScript"); !trim(s).empty()) r.pre_script = script(s);
            if (const std::string s = str(*v, "afterResponseScript"); !trim(s).empty()) r.post_script = script(s);
            add_request(res, out, std::move(r), count);
          } else if (type == "grpc_request") {
            SavedRequest r;
            r.kind = RequestKind::Grpc;
            r.name = name;
            std::string addr = insomnia_vars(str(*v, "url"));
            const std::string l = lower_ascii(addr);
            if (starts_with(l, "grpcs://")) {
              r.grpc_target.tls = true;
              addr.erase(0, 8);
            } else if (starts_with(l, "grpc://")) {
              addr.erase(0, 7);
            }
            r.grpc_target.address = addr;
            r.grpc_method = str(*v, "protoMethodName");
            r.grpc_body = insomnia_vars(str(mem(mem(v, "body"), "text")));
            if (const Value* md = arr(v, "metadata"))
              for (const auto& m : md->GetArray())
                if (m.IsObject() && !flag(m, "disabled"))
                  r.grpc_opts.metadata.emplace_back(lower_ascii(str(m, "name")), insomnia_vars(str(mem(m, "value"))));
            if (r.grpc_method.empty()) {
              res.skipped.push_back(name + ": gRPC request without a method");
              continue;
            }
            add_request(res, out, std::move(r), count);
          } else if (type == "websocket_request" || type == "websocket_payload") {
            res.skipped.push_back(name + ": WebSocket requests are not supported");
          }
        }
      };
  // Environments: the base one of a workspace is merged under each of its sub-environments.
  auto env_vars = [&](const Value* data, Environment& e) {
    std::function<void(const std::string&, const Value&)> flat = [&](const std::string& prefix, const Value& v) {
      if (v.IsObject() && v.MemberCount()) {
        for (auto m = v.MemberBegin(); m != v.MemberEnd(); ++m)
          flat(prefix.empty() ? m->name.GetString() : prefix + "." + m->name.GetString(), m->value);
      } else if (!prefix.empty()) {
        e.vars[prefix] = insomnia_vars(str(&v));
      }
    };
    if (data && data->IsObject()) flat("", *data);
  };
  // The base environment holds the workspace's own variables: those no sub-environment
  // redefines become the imported folder's variables (they resolve under any active environment,
  // as in Insomnia). Folder variables outrank environments here, so a base variable that a
  // sub-environment overrides stays in the environments (each gets the base value it lacks).
  auto envs_of = [&](const std::string& ws_id, ImportFolder& folder) {
    auto it = kids.find(ws_id);
    if (it == kids.end()) return;
    for (const Value* base : it->second) {
      if (str(*base, "_type") != "environment") continue;
      Environment b;
      env_vars(mem(base, "data"), b);
      std::vector<Environment> subs;
      std::set<std::string> overridden;
      if (auto s = kids.find(str(*base, "_id")); s != kids.end())
        for (const Value* e : s->second) {
          if (str(*e, "_type") != "environment") continue;
          Environment x;
          x.name = str(*e, "name");
          if (x.name.empty()) x.name = "Environment";
          env_vars(mem(e, "data"), x);
          for (const auto& kv : x.vars) overridden.insert(kv.first);
          subs.push_back(std::move(x));
        }
      for (const auto& [k, v] : b.vars) {
        if (!overridden.count(k)) {
          set_folder_var(folder, k, v);
          continue;
        }
        for (auto& x : subs) x.vars.emplace(k, v);
      }
      for (auto& x : subs) res.envs.push_back(std::move(x));
    }
  };

  if (workspaces.empty()) {
    res.root.name = "Insomnia";
    walk("", res.root, {}, 1);
  } else if (workspaces.size() == 1) {
    res.root.name = str(*workspaces[0], "name");
    if (res.root.name.empty()) res.root.name = "Insomnia";
    walk(str(*workspaces[0], "_id"), res.root, {}, 1);
    envs_of(str(*workspaces[0], "_id"), res.root);
  } else {
    res.root.name = "Insomnia";
    for (const Value* w : workspaces) {
      ImportFolder f;
      f.name = str(*w, "name");
      if (f.name.empty()) f.name = "Workspace";
      walk(str(*w, "_id"), f, {}, 2);
      envs_of(str(*w, "_id"), f);
      res.root.folders.push_back(std::move(f));
    }
  }
  if (tags) res.notes.push_back("Insomnia template tags ({% ... %}) are kept as text: watt does not run them");
  return res;
}

// ======================================================================= HAR

std::optional<ImportResult> parse_har(const Document& d, std::string* err) {
  ImportResult res;
  res.format = "HAR";
  res.root.name = "HAR import";
  const Value* entries = arr(mem(d, "log"), "entries");
  if (!entries) {
    if (err) *err = "the HAR file has no log.entries";
    return std::nullopt;
  }
  size_t count = 0;
  std::set<std::string> seen;
  for (const auto& e : entries->GetArray()) {
    const Value* q = mem(e, "request");
    if (!q) continue;
    SavedRequest r;
    r.http.method = str(*q, "method");
    r.http.url = str(*q, "url");
    if (!seen.insert(r.http.method + " " + r.http.url + "\n" + str(mem(mem(q, "postData"), "text"))).second) continue;
    std::string path = r.http.url;
    if (const size_t s = path.find("://"); s != std::string::npos) {
      const size_t slash = path.find('/', s + 3);
      path = slash == std::string::npos ? "/" : path.substr(slash);
    }
    if (const size_t qm = path.find_first_of("?#"); qm != std::string::npos) path.resize(qm);
    r.name = upper(r.http.method) + " " + path;
    if (const Value* hs = arr(q, "headers"))
      for (const auto& h : hs->GetArray()) {
        const std::string n = str(h, "name");
        if (n.empty() || n[0] == ':' || iequals(n, "content-length") || iequals(n, "host")) continue;
        add_header(r, n, str(mem(h, "value")));
      }
    if (const Value* pd = mem(q, "postData")) {
      const std::string mime = str(*pd, "mimeType");
      if (const Value* params = arr(pd, "params"); params && str(*pd, "text").empty()) {
        std::string body;
        for (const auto& p : params->GetArray()) {
          if (!body.empty()) body += '&';
          body += form_encode(str(p, "name")) + "=" + form_encode(str(mem(p, "value")));
        }
        r.http.body = body;
      } else {
        r.http.body = str(*pd, "text");
      }
      if (!mime.empty()) default_header(r, "Content-Type", mime);
    }
    add_request(res, res.root, std::move(r), count);
  }
  return res;
}

// ======================================================================= OpenAPI 3 / Swagger 2

struct OaCtx {
  const Document& doc;
  ImportResult& res;
  size_t count = 0;
};

const Value* oa_resolve(const OaCtx& c, const Value* v) {
  for (int i = 0; i < 16 && v && v->IsObject(); ++i) {
    const Value* ref = mem(v, "$ref");
    if (!ref || !ref->IsString()) return v;
    const std::string r = ref->GetString();
    if (!starts_with(r, "#/")) return nullptr;  // an external file: not read
    rapidjson::Pointer p(r.c_str() + 1, r.size() - 1);
    if (!p.IsValid()) return nullptr;
    v = p.Get(c.doc);
  }
  return v;
}

void oa_sample(const OaCtx& c, const Value* s, Value& out, Document::AllocatorType& al, int depth) {
  s = oa_resolve(c, s);
  if (!s || !s->IsObject() || depth > 8) {
    out.SetNull();
    return;
  }
  for (const char* k : {"example", "default"})
    if (const Value* ex = mem(s, k)) {
      out.CopyFrom(*ex, al);
      return;
    }
  if (const Value* e = arr(s, "enum"); e && !e->Empty()) {
    out.CopyFrom((*e)[0], al);
    return;
  }
  if (const Value* all = arr(s, "allOf")) {
    out.SetObject();
    for (const auto& part : all->GetArray()) {
      Value v;
      oa_sample(c, &part, v, al, depth + 1);
      if (v.IsObject())
        for (auto m = v.MemberBegin(); m != v.MemberEnd(); ++m)
          if (!out.HasMember(m->name)) out.AddMember(Value(m->name, al), Value(m->value, al), al);
    }
    return;
  }
  for (const char* k : {"oneOf", "anyOf"})
    if (const Value* one = arr(s, k); one && !one->Empty()) {
      oa_sample(c, &(*one)[0], out, al, depth + 1);
      return;
    }
  std::string type = str(mem(s, "type"));
  if (const Value* t = arr(s, "type"); t && !t->Empty()) type = str(&(*t)[0]);
  if (type.empty() && mem(s, "properties")) type = "object";
  if (type == "object") {
    out.SetObject();
    if (const Value* props = mem(s, "properties"); props && props->IsObject())
      for (auto m = props->MemberBegin(); m != props->MemberEnd(); ++m) {
        Value v;
        oa_sample(c, &m->value, v, al, depth + 1);
        out.AddMember(Value(m->name, al), v, al);
      }
  } else if (type == "array") {
    out.SetArray();
    Value v;
    oa_sample(c, mem(s, "items"), v, al, depth + 1);
    out.PushBack(v, al);
  } else if (type == "integer") {
    out.SetInt(0);
  } else if (type == "number") {
    out.SetDouble(0);
  } else if (type == "boolean") {
    out.SetBool(true);
  } else if (type == "string") {
    const std::string f = str(mem(s, "format"));
    const char* t = f == "date-time" ? "2024-01-01T00:00:00Z"
                    : f == "date"    ? "2024-01-01"
                    : f == "uuid"    ? "00000000-0000-0000-0000-000000000000"
                    : f == "email"   ? "user@example.com"
                    : f == "uri"     ? "https://example.com"
                                     : "string";
    out.SetString(t, al);
  } else {
    out.SetNull();
  }
}

std::string oa_param_value(const OaCtx& c, const Value& p) {
  if (const Value* ex = mem(p, "example")) return str(ex);
  if (const Value* exs = mem(p, "examples"); exs && exs->IsObject() && exs->MemberCount())
    if (const Value* e = oa_resolve(c, &exs->MemberBegin()->value)) return str(mem(e, "value"));
  const Value* schema = oa_resolve(c, mem(p, "schema"));
  if (!schema) schema = &p;  // Swagger 2: type / default on the parameter
  if (const Value* d = mem(schema, "default")) return str(d);
  if (const Value* ex = mem(schema, "example")) return str(ex);
  if (const Value* e = arr(schema, "enum"); e && !e->Empty()) return str(&(*e)[0]);
  return {};
}

std::optional<ImportResult> parse_openapi(const Document& d, std::string* err) {
  ImportResult res;
  OaCtx c{d, res};
  const bool swagger = mem(d, "swagger") != nullptr;
  res.format = swagger ? "Swagger " + str(d, "swagger") : "OpenAPI " + str(d, "openapi");
  const Value* info = mem(d, "info");
  res.root.name = str(mem(info, "title"));
  if (res.root.name.empty()) res.root.name = "OpenAPI";
  Environment env;
  env.name = res.root.name;
  // Base URL.
  std::string base;
  if (swagger) {
    std::string scheme = "https";
    if (const Value* s = arr(d, "schemes"); s && !s->Empty()) scheme = str(&(*s)[0]);
    const std::string host = str(d, "host");
    base = host.empty() ? "http://localhost" : scheme + "://" + host;
    base += str(d, "basePath");
  } else if (const Value* servers = arr(d, "servers"); servers && !servers->Empty()) {
    const Value& s0 = (*servers)[0];
    base = str(s0, "url");
    if (const Value* vars = mem(s0, "variables"); vars && vars->IsObject())
      for (auto m = vars->MemberBegin(); m != vars->MemberEnd(); ++m) {
        const std::string key = "{" + std::string(m->name.GetString()) + "}";
        const std::string val = str(m->value, "default");
        for (size_t p; (p = base.find(key)) != std::string::npos;) base.replace(p, key.size(), val);
      }
    if (starts_with(base, "/")) {
      res.notes.push_back("the server URL \"" + base + "\" is relative: set baseUrl in the collection's variables");
      base = "http://localhost" + base;
    }
  } else {
    base = "http://localhost";
  }
  while (!base.empty() && base.back() == '/') base.pop_back();
  if (!url_problem(base).empty()) {
    if (err) *err = "the server URL " + base + " is not http or https";
    return std::nullopt;
  }
  set_folder_var(res.root, "baseUrl", base);  // resolves under any active environment
  // Security schemes -> how each request authenticates.
  const Value* schemes = swagger ? mem(d, "securityDefinitions") : mem(mem(d, "components"), "securitySchemes");
  auto apply_security = [&](SavedRequest& r, const Value* sec) {
    if (!sec || !sec->IsArray()) return;
    for (const auto& req : sec->GetArray()) {
      if (!req.IsObject() || req.MemberCount() == 0) continue;
      for (auto m = req.MemberBegin(); m != req.MemberEnd(); ++m) {
        const Value* s = oa_resolve(c, mem(schemes, m->name.GetString()));
        if (!s) continue;
        const std::string type = lower_ascii(str(*s, "type")), scheme = lower_ascii(str(*s, "scheme"));
        if ((type == "http" && scheme == "basic") || type == "basic") {
          r.basic_auth = {true, "{{username}}", "{{password}}"};
          env.vars.emplace("username", "");
          env.vars.emplace("password", "");
          env.secrets.insert("password");
        } else if ((type == "http" && scheme == "bearer") || type == "oauth2" || type == "openidconnect") {
          default_header(r, "Authorization", "Bearer {{token}}");
          env.vars.emplace("token", "");
          env.secrets.insert("token");
        } else if (type == "apikey") {
          const std::string name = str(*s, "name"), in = str(*s, "in");
          if (name.empty()) continue;
          if (in == "query") append_query(r.http.url, name, "{{apiKey}}");
          else if (in == "header") default_header(r, name, "{{apiKey}}");
          else continue;
          env.vars.emplace("apiKey", "");
          env.secrets.insert("apiKey");
        }
      }
      return;  // the first requirement that is an alternative is enough
    }
  };
  const Value* paths = mem(d, "paths");
  if (!paths || !paths->IsObject()) {
    if (err) *err = "the document has no paths";
    return std::nullopt;
  }
  // Folders by tag, in the order the document declares its tags.
  std::map<std::string, size_t> folder_of;
  auto folder = [&](const std::string& tag) -> ImportFolder& {
    if (tag.empty()) return res.root;
    auto [it, fresh] = folder_of.emplace(tag, res.root.folders.size());
    if (fresh) res.root.folders.push_back(ImportFolder{tag, {}, {}});
    return res.root.folders[it->second];
  };
  if (const Value* tags = arr(d, "tags"))
    for (const auto& t : tags->GetArray()) folder(str(t, "name"));
  static const char* kMethods[] = {"get", "post", "put", "patch", "delete", "head", "options", "trace"};
  for (auto pm = paths->MemberBegin(); pm != paths->MemberEnd(); ++pm) {
    const std::string path = pm->name.GetString();
    const Value* item = oa_resolve(c, &pm->value);
    if (!item || !item->IsObject()) continue;
    for (const char* method : kMethods) {
      const Value* op = mem(item, method);
      if (!op || !op->IsObject()) continue;
      SavedRequest r;
      r.http.method = upper(method);
      r.name = trim(str(*op, "summary"));
      if (r.name.empty()) r.name = str(*op, "operationId");
      if (r.name.empty()) r.name = r.http.method + " " + path;
      if (r.name.size() > 80) r.name = r.name.substr(0, 77) + "...";
      r.docs = str(*op, "description");
      // "/users/{id}" -> "{{baseUrl}}/users/:id"
      std::string url = "{{baseUrl}}";
      for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '{') {
          const size_t e = path.find('}', i);
          if (e != std::string::npos) {
            url += ":" + path.substr(i + 1, e - i - 1);
            i = e;
            continue;
          }
        }
        url += path[i];
      }
      r.http.url = url;
      // Parameters: the path item's, then the operation's (which override by name + in).
      std::vector<const Value*> params;
      for (const Value* list : {arr(item, "parameters"), arr(op, "parameters")})
        if (list)
          for (const auto& p : list->GetArray())
            if (const Value* rp = oa_resolve(c, &p); rp && rp->IsObject()) {
              const std::string n = str(*rp, "name"), in = str(*rp, "in");
              params.erase(std::remove_if(params.begin(), params.end(),
                                          [&](const Value* x) { return str(*x, "name") == n && str(*x, "in") == in; }),
                           params.end());
              params.push_back(rp);
            }
      const Value* body_param = nullptr;
      for (const Value* p : params) {
        const std::string n = str(*p, "name"), in = str(*p, "in");
        const bool required = flag(*p, "required");
        const std::string v = oa_param_value(c, *p);
        if (in == "path") r.path_params.emplace_back(n, v);
        else if (in == "query") {
          if (required) append_query(r.http.url, n, v);
          else r.disabled_params.emplace_back(n, v);
        } else if (in == "header") {
          add_header(r, n, v, !required);
        } else if (in == "body") {
          body_param = p;
        }
      }
      // Request body: an example of the first media type (JSON preferred).
      const Value* rb = oa_resolve(c, mem(op, "requestBody"));
      if (const Value* content = mem(rb, "content"); content && content->IsObject() && content->MemberCount()) {
        const Value* media = nullptr;
        std::string mime;
        for (auto m = content->MemberBegin(); m != content->MemberEnd(); ++m)
          if (lower_ascii(m->name.GetString()).find("json") != std::string::npos) {
            media = &m->value;
            mime = m->name.GetString();
            break;
          }
        if (!media) {
          media = &content->MemberBegin()->value;
          mime = content->MemberBegin()->name.GetString();
        }
        Document sample;
        if (const Value* ex = mem(media, "example")) sample.CopyFrom(*ex, sample.GetAllocator());
        else if (const Value* exs = mem(media, "examples"); exs && exs->IsObject() && exs->MemberCount()) {
          if (const Value* e = oa_resolve(c, &exs->MemberBegin()->value); e && mem(e, "value"))
            sample.CopyFrom(*mem(e, "value"), sample.GetAllocator());
        } else {
          oa_sample(c, mem(media, "schema"), sample, sample.GetAllocator(), 0);
        }
        if (mime == "multipart/form-data") {
          if (sample.IsObject())
            for (auto m = sample.MemberBegin(); m != sample.MemberEnd(); ++m)
              r.form.push_back(FormPart{m->name.GetString(), m->value.IsString() ? m->value.GetString() : str(&m->value)});
        } else if (mime == "application/x-www-form-urlencoded") {
          std::string body;
          if (sample.IsObject())
            for (auto m = sample.MemberBegin(); m != sample.MemberEnd(); ++m)
              body += (body.empty() ? "" : "&") + form_encode(m->name.GetString()) + "=" + form_encode(str(&m->value));
          r.http.body = body;
          default_header(r, "Content-Type", mime);
        } else {
          if (!sample.IsNull()) r.http.body = sample.IsString() ? std::string(sample.GetString()) : json_pretty_text(sample);
          default_header(r, "Content-Type", mime);
        }
      } else if (body_param) {
        Document sample;
        oa_sample(c, mem(body_param, "schema"), sample, sample.GetAllocator(), 0);
        if (!sample.IsNull()) r.http.body = json_pretty_text(sample);
        default_header(r, "Content-Type", "application/json");
      }
      const Value* sec = mem(op, "security");
      apply_security(r, sec ? sec : mem(d, "security"));
      std::string tag;
      if (const Value* tags = arr(op, "tags"); tags && !tags->Empty()) tag = str(&(*tags)[0]);
      add_request(res, folder(tag), std::move(r), c.count);
    }
  }
  // Declared tags no operation uses would be empty folders.
  res.root.folders.erase(std::remove_if(res.root.folders.begin(), res.root.folders.end(),
                                        [](const ImportFolder& f) { return f.requests.empty() && f.folders.empty(); }),
                         res.root.folders.end());
  if (!env.vars.empty()) res.envs.push_back(std::move(env));  // credential placeholders
  return res;
}

// ======================================================================= Bruno

struct BruBlock {
  std::string tag;
  std::string text;  // body lines with the block's 2-space indent removed
  struct Kv {
    std::string key, value;
    bool disabled = false;
  };
  std::vector<Kv> kv;
  std::vector<std::string> list;  // "[ a, b ]" blocks
};

std::vector<BruBlock> parse_bru(const std::string& s) {
  std::vector<BruBlock> out;
  std::vector<std::string> lines;
  for (size_t p = 0; p <= s.size();) {
    size_t e = s.find('\n', p);
    if (e == std::string::npos) e = s.size();
    std::string l = s.substr(p, e - p);
    if (!l.empty() && l.back() == '\r') l.pop_back();
    lines.push_back(std::move(l));
    p = e + 1;
  }
  for (size_t i = 0; i < lines.size(); ++i) {
    std::string h = lines[i];
    while (!h.empty() && (h.back() == ' ' || h.back() == '\t')) h.pop_back();
    if (h.empty() || h[0] == ' ' || h[0] == '\t' || h.size() < 2) continue;
    const char open = h.back();
    if (open != '{' && open != '[') continue;
    BruBlock b;
    b.tag = trim(h.substr(0, h.size() - 1));
    const char close = open == '{' ? '}' : ']';
    std::vector<std::string> body;
    size_t j = i + 1;
    for (; j < lines.size(); ++j) {
      std::string t = lines[j];
      while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
      if (t.size() == 1 && t[0] == close) break;
      body.push_back(lines[j]);
    }
    i = j;
    for (size_t k = 0; k < body.size(); ++k) {
      std::string l = body[k];
      size_t cut = 0;
      while (cut < 2 && cut < l.size() && l[cut] == ' ') ++cut;
      l.erase(0, cut);
      b.text += l;
      if (k + 1 < body.size()) b.text += '\n';
      const std::string t = trim(l);
      if (t.empty()) continue;
      if (open == '[') {
        for (size_t p = 0; p <= t.size();) {
          size_t e = t.find(',', p);
          if (e == std::string::npos) e = t.size();
          if (std::string item = trim(t.substr(p, e - p)); !item.empty()) b.list.push_back(item);
          p = e + 1;
        }
      } else if (const size_t col = t.find(':'); col != std::string::npos) {
        BruBlock::Kv kv;
        kv.key = trim(t.substr(0, col));
        kv.value = trim(t.substr(col + 1));
        if (!kv.key.empty() && kv.key[0] == '~') {
          kv.disabled = true;
          kv.key.erase(0, 1);
        }
        b.kv.push_back(std::move(kv));
      }
    }
    out.push_back(std::move(b));
  }
  return out;
}

const BruBlock* bru_find(const std::vector<BruBlock>& bs, std::string_view tag) {
  for (const auto& b : bs)
    if (b.tag == tag) return &b;
  return nullptr;
}
std::string bru_kv(const BruBlock* b, std::string_view key) {
  if (!b) return {};
  for (const auto& kv : b->kv)
    if (kv.key == key && !kv.disabled) return kv.value;
  return {};
}

struct BruLevel {  // what a folder.bru / collection.bru passes down
  AuthSpec auth;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string pre, post;
};

AuthSpec bru_auth(const std::vector<BruBlock>& bs, std::string mode) {
  AuthSpec a;
  if (mode.empty()) {
    for (const auto& b : bs)
      if (starts_with(b.tag, "auth:")) {
        mode = b.tag.substr(5);
        break;
      }
    if (mode.empty()) mode = bru_kv(bru_find(bs, "auth"), "mode");
  }
  a.type = mode;
  if (mode.empty() || mode == "none" || mode == "inherit") return a;
  const BruBlock* b = bru_find(bs, "auth:" + mode);
  if (b)
    for (const auto& kv : b->kv)
      if (!kv.disabled) a.kv[kv.key] = kv.value;
  if (mode == "apikey") a.kv["in"] = a.kv["placement"];
  return a;
}

BruLevel bru_level(const std::vector<BruBlock>& bs, const BruLevel& parent) {
  BruLevel l = parent;
  if (AuthSpec a = bru_auth(bs, {}); !a.type.empty() && a.type != "inherit") l.auth = a;
  if (const BruBlock* h = bru_find(bs, "headers"))
    for (const auto& kv : h->kv)
      if (!kv.disabled) l.headers.emplace_back(kv.key, kv.value);
  if (const BruBlock* s = bru_find(bs, "script:pre-request"); s && !trim(s->text).empty())
    l.pre += (l.pre.empty() ? "" : "\n\n") + s->text;
  if (const BruBlock* s = bru_find(bs, "script:post-response"); s && !trim(s->text).empty())
    l.post = s->text + (l.post.empty() ? "" : "\n\n" + l.post);
  return l;
}

// collection.bru / folder.bru "vars:pre-request": that folder's variables.
void bru_folder_vars(const std::vector<BruBlock>& bs, ImportFolder& f) {
  if (const BruBlock* v = bru_find(bs, "vars:pre-request"))
    for (const auto& kv : v->kv) set_folder_var(f, kv.key, kv.value, !kv.disabled);
}

bool read_small(const fs::path& p, std::string& out, size_t cap = 8u << 20) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  const auto n = static_cast<size_t>(std::max<std::streamoff>(0, in.tellg()));
  if (n > cap) return false;
  in.seekg(0);
  out.resize(n);
  in.read(out.data(), static_cast<std::streamsize>(n));
  return static_cast<size_t>(in.gcount()) == n;
}

std::optional<SavedRequest> bru_request(const std::vector<BruBlock>& bs, const BruLevel& lvl, const std::string& fallback,
                                        ImportResult& res, int* seq) {
  const BruBlock* meta = bru_find(bs, "meta");
  SavedRequest r;
  r.name = bru_kv(meta, "name");
  if (r.name.empty()) r.name = fallback;
  if (seq) *seq = std::atoi(bru_kv(meta, "seq").c_str());
  const std::string type = bru_kv(meta, "type");
  if (type == "grpc" || bru_find(bs, "grpc")) {
    res.skipped.push_back(r.name + ": Bruno gRPC requests are not imported");
    return std::nullopt;
  }
  const BruBlock* m = nullptr;
  for (const char* verb : {"get", "post", "put", "patch", "delete", "head", "options", "connect", "trace"})
    if ((m = bru_find(bs, verb))) {
      r.http.method = upper(verb);
      break;
    }
  if (!m) {
    if (const BruBlock* http = bru_find(bs, "http")) {  // a custom method
      m = http;
      r.http.method = bru_kv(http, "method");
    }
  }
  if (!m) {
    res.skipped.push_back(r.name + ": no HTTP method block");
    return std::nullopt;
  }
  r.http.url = bru_kv(m, "url");
  if (const BruBlock* q = bru_find(bs, "params:query"))
    for (const auto& kv : q->kv)
      if (kv.disabled) r.disabled_params.emplace_back(kv.key, kv.value);
  if (const BruBlock* p = bru_find(bs, "params:path"))
    for (const auto& kv : p->kv) r.path_params.emplace_back(kv.key, kv.value);
  if (const BruBlock* h = bru_find(bs, "headers"))
    for (const auto& kv : h->kv) add_header(r, kv.key, kv.value, kv.disabled);
  for (const auto& [k, v] : lvl.headers) default_header(r, k, v);
  const std::string mode = bru_kv(m, "body");
  auto text_of = [&](const char* tag) -> std::string {
    const BruBlock* b = bru_find(bs, tag);
    return b ? b->text : std::string();
  };
  if (mode == "json") {
    r.http.body = text_of("body:json");
    default_header(r, "Content-Type", "application/json");
  } else if (mode == "xml") {
    r.http.body = text_of("body:xml");
    default_header(r, "Content-Type", "application/xml");
  } else if (mode == "text") {
    r.http.body = text_of("body:text");
    default_header(r, "Content-Type", "text/plain");
  } else if (mode == "sparql") {
    r.http.body = text_of("body:sparql");
    default_header(r, "Content-Type", "application/sparql-query");
  } else if (mode == "graphql") {
    Document d;
    d.SetObject();
    auto& al = d.GetAllocator();
    const std::string q = text_of("body:graphql");
    d.AddMember("query", Value(q.c_str(), static_cast<rapidjson::SizeType>(q.size()), al), al);
    Document vars;
    const std::string vs = text_of("body:graphql:vars");
    if (!trim(vs).empty() && !vars.Parse(vs.c_str(), vs.size()).HasParseError()) d.AddMember("variables", Value(vars, al), al);
    r.http.body = json_pretty_text(d);
    default_header(r, "Content-Type", "application/json");
  } else if (mode == "form-urlencoded" || mode == "formUrlEncoded") {
    std::string body;
    if (const BruBlock* f = bru_find(bs, "body:form-urlencoded"))
      for (const auto& kv : f->kv)
        if (!kv.disabled) body += (body.empty() ? "" : "&") + form_encode(kv.key) + "=" + form_encode(kv.value);
    r.http.body = body;
    default_header(r, "Content-Type", "application/x-www-form-urlencoded");
  } else if (mode == "multipart-form" || mode == "multipartForm") {
    if (const BruBlock* f = bru_find(bs, "body:multipart-form"))
      for (const auto& kv : f->kv) {
        FormPart p;
        p.name = kv.key;
        p.enabled = !kv.disabled;
        std::string v = kv.value;
        if (const size_t ct = v.find(" @contentType("); ct != std::string::npos) {
          const size_t e = v.find(')', ct);
          p.type = v.substr(ct + 14, e == std::string::npos ? std::string::npos : e - ct - 14);
          v.resize(ct);
        }
        if (starts_with(v, "@file(") && v.back() == ')') {
          p.kind = FormPart::Kind::File;
          v = v.substr(6, v.size() - 7);
          if (const size_t bar = v.find('|'); bar != std::string::npos) v.resize(bar);
        }
        p.value = v;
        r.form.push_back(std::move(p));
      }
  } else if (mode == "file") {
    if (const BruBlock* f = bru_find(bs, "body:file"))
      for (const auto& kv : f->kv) {
        if (kv.disabled) continue;
        std::string v = kv.value;
        if (const size_t at = v.find("@file("); at != std::string::npos) {
          const size_t e = v.find(')', at);
          r.body_file = v.substr(at + 6, e == std::string::npos ? std::string::npos : e - at - 6);
        }
        if (const size_t ct = v.find("@contentType("); ct != std::string::npos) {
          const size_t e = v.find(')', ct);
          default_header(r, "Content-Type", v.substr(ct + 13, e == std::string::npos ? std::string::npos : e - ct - 13));
        }
        break;
      }
  }
  AuthSpec a = bru_auth(bs, bru_kv(m, "auth"));
  if (a.type == "inherit" || a.type.empty()) a = lvl.auth;
  apply_auth(r, a, res);
  std::string pre = lvl.pre;
  if (const BruBlock* s = bru_find(bs, "script:pre-request"); s && !trim(s->text).empty())
    pre += (pre.empty() ? "" : "\n\n") + s->text;
  std::string post;
  if (const BruBlock* s = bru_find(bs, "script:post-response"); s && !trim(s->text).empty()) post = s->text;
  if (const BruBlock* t = bru_find(bs, "tests"); t && !trim(t->text).empty()) post += (post.empty() ? "" : "\n\n") + t->text;
  if (!lvl.post.empty()) post += (post.empty() ? "" : "\n\n") + lvl.post;
  r.pre_script = pre;
  r.post_script = post;
  if (const BruBlock* doc = bru_find(bs, "docs")) r.docs = doc->text;
  if (const BruBlock* as = bru_find(bs, "assert"))
    for (const auto& kv : as->kv) {
      Assertion x;
      x.enabled = !kv.disabled;
      x.target = kv.key;
      const size_t sp = kv.value.find(' ');
      x.op = sp == std::string::npos ? kv.value : kv.value.substr(0, sp);
      x.value = sp == std::string::npos ? std::string() : trim(kv.value.substr(sp + 1));
      r.asserts.push_back(std::move(x));
    }
  if (const BruBlock* vs = bru_find(bs, "vars:post-response"))
    for (const auto& kv : vs->kv) {
      Capture cap;
      cap.enabled = !kv.disabled;
      cap.var = kv.key;
      cap.target = kv.value;
      r.captures.push_back(std::move(cap));
    }
  if (const BruBlock* vs = bru_find(bs, "vars:pre-request"); vs && !vs->kv.empty())
    res.notes.push_back(r.name + ": vars:pre-request is not imported (use the environment)");
  return r;
}

bool bru_file(const fs::path& p) {
  const auto n = p.filename().native();
  return n.size() > 4 && (n.compare(n.size() - 4, 4, L".bru") == 0 || n.compare(n.size() - 4, 4, L".BRU") == 0);
}

void bru_walk(const fs::path& dir, ImportFolder& out, const BruLevel& parent, ImportResult& res, size_t& count,
              int depth, size_t& files) {
  std::error_code ec;
  std::vector<std::pair<int, fs::path>> reqs;
  std::vector<std::pair<int, fs::path>> subdirs;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const fs::path p = it->path();
    const std::wstring n = p.filename().native();
    if (n.empty() || n[0] == L'.' || n == L"node_modules") continue;
    if (it->is_directory(ec)) {
      if (depth == 1 && n == L"environments") continue;
      if (it->is_symlink(ec)) continue;
      subdirs.emplace_back(0, p);
    } else if (bru_file(p) && n != L"folder.bru" && n != L"collection.bru") {
      reqs.emplace_back(0, p);
    }
  }
  if (depth > kMaxDepth) {
    res.skipped.push_back(u8s(dir.filename()) + ": folders nested too deep");
    return;
  }
  struct Parsed {
    int seq;
    SavedRequest r;
  };
  std::vector<Parsed> parsed;
  for (auto& [seq, p] : reqs) {
    if (++files > kMaxRequests) break;
    std::string text;
    const std::string stem = u8s(p.stem());
    if (!read_small(p, text)) {
      res.skipped.push_back(stem + ": could not be read");
      continue;
    }
    int s = 0;
    if (auto r = bru_request(parse_bru(text), parent, stem, res, &s)) parsed.push_back({s, std::move(*r)});
  }
  std::stable_sort(parsed.begin(), parsed.end(), [](const Parsed& a, const Parsed& b) { return a.seq < b.seq; });
  for (auto& x : parsed) add_request(res, out, std::move(x.r), count);
  struct Sub {
    int seq;
    std::string name;
    fs::path path;
    std::vector<BruBlock> blocks;
  };
  std::vector<Sub> subs;
  for (auto& [seq, p] : subdirs) {
    Sub s{0, u8s(p.filename()), p, {}};
    std::string text;
    if (read_small(p / L"folder.bru", text)) {
      s.blocks = parse_bru(text);
      const BruBlock* meta = bru_find(s.blocks, "meta");
      if (const std::string n = bru_kv(meta, "name"); !n.empty()) s.name = n;
      s.seq = std::atoi(bru_kv(meta, "seq").c_str());
    }
    subs.push_back(std::move(s));
  }
  std::stable_sort(subs.begin(), subs.end(), [](const Sub& a, const Sub& b) {
    return a.seq != b.seq ? a.seq < b.seq : a.name < b.name;
  });
  for (auto& s : subs) {
    ImportFolder f;
    f.name = s.name;
    bru_folder_vars(s.blocks, f);
    bru_walk(s.path, f, bru_level(s.blocks, parent), res, count, depth + 1, files);
    if (!f.requests.empty() || !f.folders.empty()) out.folders.push_back(std::move(f));
  }
}

// ======================================================================= YAML (subset) -> JSON

class Yaml {
 public:
  Yaml(const std::string& text, Document::AllocatorType& al) : al_(al) {
    for (size_t p = 0; p <= text.size();) {
      size_t e = text.find('\n', p);
      if (e == std::string::npos) e = text.size();
      std::string l = text.substr(p, e - p);
      if (!l.empty() && l.back() == '\r') l.pop_back();
      Line x;
      size_t ind = 0;
      while (ind < l.size() && l[ind] == ' ') ++ind;
      x.indent = static_cast<int>(ind);
      x.raw = l;
      x.text = l.substr(ind);
      lines_.push_back(std::move(x));
      p = e + 1;
    }
  }
  bool parse(Value& out, std::string* err) {
    skip();
    if (i_ < lines_.size() && (lines_[i_].text == "---" || starts_with(lines_[i_].text, "--- "))) {
      ++i_;
      skip();
    }
    if (i_ >= lines_.size()) {
      out.SetNull();
    } else {
      node(lines_[i_].indent, out, 0);
    }
    skip();
    if (err_.empty() && i_ < lines_.size() && lines_[i_].text != "---" && lines_[i_].text != "...")
      fail("unexpected indentation");
    if (!err_.empty()) {
      if (err) *err = err_;
      return false;
    }
    return true;
  }

 private:
  struct Line {
    int indent = 0;
    std::string raw, text;
  };
  std::vector<Line> lines_;
  size_t i_ = 0;
  Document::AllocatorType& al_;
  std::string err_;

  void fail(const std::string& why) {
    if (err_.empty()) err_ = "YAML line " + std::to_string(std::min(i_, lines_.size() - 1) + 1) + ": " + why;
  }
  static std::string strip_comment(const std::string& t) {
    char q = 0;
    for (size_t k = 0; k < t.size(); ++k) {
      const char c = t[k];
      if (q) {
        if (c == '\\' && q == '"') ++k;
        else if (c == q) q = 0;
      } else if (c == '"' || c == '\'') {
        if (k == 0 || t[k - 1] == ' ' || t[k - 1] == '[' || t[k - 1] == '{' || t[k - 1] == ',' || t[k - 1] == ':') q = c;
      } else if (c == '#' && (k == 0 || t[k - 1] == ' ' || t[k - 1] == '\t')) {
        std::string o = t.substr(0, k);
        while (!o.empty() && (o.back() == ' ' || o.back() == '\t')) o.pop_back();
        return o;
      }
    }
    std::string o = t;
    while (!o.empty() && (o.back() == ' ' || o.back() == '\t')) o.pop_back();
    return o;
  }
  bool blank(size_t k) const {
    const std::string& t = lines_[k].text;
    return t.empty() || t[0] == '#' || (t[0] == '%' && lines_[k].indent == 0);
  }
  void skip() {
    while (i_ < lines_.size() && blank(i_)) ++i_;
  }
  std::string cur() const { return strip_comment(lines_[i_].text); }
  static bool seq_item(const std::string& c) { return c == "-" || starts_with(c, "- "); }
  // The ':' that ends a mapping key on this line, or npos.
  static size_t key_colon(const std::string& c) {
    if (c.empty() || c[0] == '[' || c[0] == '{' || c[0] == '|' || c[0] == '>') return std::string::npos;
    size_t k = 0;
    if (c[0] == '"' || c[0] == '\'') {
      const char q = c[0];
      for (k = 1; k < c.size(); ++k) {
        if (q == '"' && c[k] == '\\') ++k;
        else if (c[k] == q) {
          if (q == '\'' && k + 1 < c.size() && c[k + 1] == '\'') {
            ++k;
            continue;
          }
          break;
        }
      }
      ++k;
      while (k < c.size() && c[k] == ' ') ++k;
      return k < c.size() && c[k] == ':' && (k + 1 == c.size() || c[k + 1] == ' ') ? k : std::string::npos;
    }
    for (; k < c.size(); ++k)
      if (c[k] == ':' && (k + 1 == c.size() || c[k + 1] == ' ' || c[k + 1] == '\t')) return k;
    return std::string::npos;
  }

  void node(int ind, Value& out, int depth) {
    if (depth > 200) return fail("nested too deep");
    skip();
    if (i_ >= lines_.size()) return out.SetNull(), void();
    const std::string c = cur();
    if (seq_item(c)) return seq(lines_[i_].indent, out, depth);
    if (key_colon(c) != std::string::npos) return map(lines_[i_].indent, out, depth);
    ++i_;
    value_text(c, ind - 1, out, depth, false);
  }

  void map(int ind, Value& out, int depth) {
    out.SetObject();
    while (err_.empty()) {
      skip();
      if (i_ >= lines_.size() || lines_[i_].indent != ind) break;
      const std::string c = cur();
      if (seq_item(c) || c == "---" || c == "...") break;
      const size_t col = key_colon(c);
      if (col == std::string::npos) return fail("expected \"key: value\"");
      std::string key = trim(c.substr(0, col));
      if (!key.empty() && (key[0] == '"' || key[0] == '\'')) key = unquote(key);
      const std::string rest = trim(c.substr(col + 1));
      ++i_;
      Value v;
      value_text(rest, ind, v, depth, true);
      Value k(key.c_str(), static_cast<rapidjson::SizeType>(key.size()), al_);
      if (auto it = out.FindMember(k); it != out.MemberEnd()) it->value = v;
      else out.AddMember(k, v, al_);
    }
  }

  void seq(int ind, Value& out, int depth) {
    out.SetArray();
    while (err_.empty()) {
      skip();
      if (i_ >= lines_.size() || lines_[i_].indent != ind) break;
      const std::string c = cur();
      if (!seq_item(c)) break;
      size_t off = 1;
      while (off < c.size() && c[off] == ' ') ++off;
      const std::string item = c.substr(off);
      Value v;
      if (!item.empty() && (seq_item(item) || key_colon(item) != std::string::npos)) {
        // "- key: v" / "- - x": the rest of the line is a node at its own column.
        lines_[i_].indent = ind + static_cast<int>(off);
        lines_[i_].text = item;
        node(ind + static_cast<int>(off), v, depth + 1);
      } else {
        ++i_;
        value_text(item, ind, v, depth, false);
      }
      out.PushBack(v, al_);
    }
  }

  // The value after "key:" / "- " (rest, maybe empty) of a node at column ind.
  void value_text(std::string rest, int ind, Value& out, int depth, bool in_map) {
    // Anchors and tags are dropped; aliases are not followed.
    while (!rest.empty() && (rest[0] == '&' || rest[0] == '!')) {
      const size_t sp = rest.find(' ');
      rest = sp == std::string::npos ? std::string() : trim(rest.substr(sp + 1));
    }
    if (!rest.empty() && rest[0] == '*') return out.SetNull(), void();
    if (rest.empty()) {
      skip();
      if (i_ < lines_.size()) {
        const int n = lines_[i_].indent;
        if (n > ind) return node(n, out, depth + 1);
        if (in_map && n == ind && seq_item(cur())) return seq(n, out, depth + 1);
      }
      return out.SetNull(), void();
    }
    if (rest[0] == '|' || rest[0] == '>') return block_scalar(rest, ind, out);
    if (rest[0] == '[' || rest[0] == '{') {
      std::string flow = rest;
      while (!balanced(flow) && i_ < lines_.size()) flow += " " + strip_comment(lines_[i_++].text);
      size_t p = 0;
      flow_value(flow, p, out, depth);
      return;
    }
    if (rest[0] == '"' || rest[0] == '\'') {
      std::string q = rest;
      while (!closed_quote(q) && i_ < lines_.size()) {
        const std::string t = trim(lines_[i_++].text);
        q += t.empty() ? "\n" : " " + t;
      }
      const std::string s = unquote(trim(q));
      out.SetString(s.c_str(), static_cast<rapidjson::SizeType>(s.size()), al_);
      return;
    }
    // A plain scalar, maybe continued on more-indented lines.
    std::string s = rest;
    while (i_ < lines_.size() && !blank(i_) && lines_[i_].indent > ind) s += " " + trim(cur()), ++i_;
    plain(s, out);
  }

  void block_scalar(const std::string& head, int ind, Value& out) {
    const bool folded = head[0] == '>';
    const char chomp = head.find('-') != std::string::npos ? '-' : head.find('+') != std::string::npos ? '+' : ' ';
    std::vector<std::string> body;
    int bi = -1;
    while (i_ < lines_.size()) {
      const Line& l = lines_[i_];
      if (l.text.empty()) {
        body.emplace_back();
        ++i_;
        continue;
      }
      if (l.indent <= ind) break;
      if (bi < 0) bi = l.indent;
      if (l.indent < bi) break;
      body.push_back(l.raw.substr(static_cast<size_t>(bi)));
      ++i_;
    }
    size_t trailing = 0;
    while (!body.empty() && body.back().empty()) body.pop_back(), ++trailing;
    std::string s;
    for (size_t k = 0; k < body.size(); ++k) {
      if (k) {
        if (folded && !body[k].empty() && !body[k - 1].empty() && body[k][0] != ' ') s += ' ';
        else s += '\n';
      }
      s += body[k];
    }
    if (chomp != '-' && !body.empty()) s += '\n';
    if (chomp == '+') s.append(trailing, '\n');
    out.SetString(s.c_str(), static_cast<rapidjson::SizeType>(s.size()), al_);
  }

  static bool closed_quote(const std::string& s) {
    const char q = s[0];
    for (size_t k = 1; k < s.size(); ++k) {
      if (q == '"' && s[k] == '\\') ++k;
      else if (s[k] == q) {
        if (q == '\'' && k + 1 < s.size() && s[k + 1] == '\'') {
          ++k;
          continue;
        }
        return true;
      }
    }
    return false;
  }
  static bool balanced(const std::string& s) {
    int d = 0;
    char q = 0;
    for (size_t k = 0; k < s.size(); ++k) {
      const char c = s[k];
      if (q) {
        if (c == '\\' && q == '"') ++k;
        else if (c == q) q = 0;
      } else if (c == '"' || c == '\'') q = c;
      else if (c == '[' || c == '{') ++d;
      else if (c == ']' || c == '}') --d;
    }
    return d <= 0;
  }
  static void utf8(std::string& o, unsigned cp) {
    if (cp < 0x80) o += static_cast<char>(cp);
    else if (cp < 0x800) o += static_cast<char>(0xC0 | (cp >> 6)), o += static_cast<char>(0x80 | (cp & 63));
    else if (cp < 0x10000)
      o += static_cast<char>(0xE0 | (cp >> 12)), o += static_cast<char>(0x80 | ((cp >> 6) & 63)),
          o += static_cast<char>(0x80 | (cp & 63));
    else
      o += static_cast<char>(0xF0 | (cp >> 18)), o += static_cast<char>(0x80 | ((cp >> 12) & 63)),
          o += static_cast<char>(0x80 | ((cp >> 6) & 63)), o += static_cast<char>(0x80 | (cp & 63));
  }
  static std::string unquote(const std::string& s) {
    if (s.size() < 2) return s;
    const char q = s[0];
    std::string o;
    for (size_t k = 1; k < s.size(); ++k) {
      const char c = s[k];
      if (c == q) {
        if (q == '\'' && k + 1 < s.size() && s[k + 1] == '\'') {
          o += '\'';
          ++k;
          continue;
        }
        break;
      }
      if (q == '"' && c == '\\' && k + 1 < s.size()) {
        const char e = s[++k];
        switch (e) {
          case 'n': o += '\n'; break;
          case 't': o += '\t'; break;
          case 'r': o += '\r'; break;
          case '0': o += '\0'; break;
          case 'x':
          case 'u':
          case 'U': {
            const size_t n = e == 'x' ? 2 : e == 'u' ? 4 : 8;
            const std::string h = s.substr(k + 1, n);
            utf8(o, static_cast<unsigned>(std::strtoul(h.c_str(), nullptr, 16)));
            k += n;
            break;
          }
          default: o += e;
        }
        continue;
      }
      o += c;
    }
    return o;
  }
  void plain(const std::string& s, Value& out) {
    if (s == "~" || s == "null" || s == "Null" || s == "NULL") return out.SetNull(), void();
    if (s == "true" || s == "True" || s == "TRUE") return out.SetBool(true), void();
    if (s == "false" || s == "False" || s == "FALSE") return out.SetBool(false), void();
    if (!s.empty()) {
      size_t k = (s[0] == '-' || s[0] == '+') ? 1 : 0;
      bool digits = k < s.size(), dot = false, exp = false;
      for (size_t j = k; j < s.size() && digits; ++j) {
        const char c = s[j];
        if (std::isdigit(static_cast<unsigned char>(c))) continue;
        if (c == '.' && !dot && !exp) dot = true;
        else if ((c == 'e' || c == 'E') && !exp && j > k && j + 1 < s.size()) {
          exp = true;
          if (s[j + 1] == '-' || s[j + 1] == '+') ++j;
        } else digits = false;
      }
      // Leading zeros ("007", a zip code) stay strings.
      if (digits && !(s.size() - k > 1 && s[k] == '0' && !dot && !exp)) {
        if (!dot && !exp && s.size() < 18) return out.SetInt64(std::strtoll(s.c_str(), nullptr, 10)), void();
        return out.SetDouble(std::strtod(s.c_str(), nullptr)), void();
      }
    }
    out.SetString(s.c_str(), static_cast<rapidjson::SizeType>(s.size()), al_);
  }
  void flow_value(const std::string& f, size_t& p, Value& out, int depth) {
    auto ws = [&] {
      while (p < f.size() && (f[p] == ' ' || f[p] == '\t')) ++p;
    };
    ws();
    if (depth > 200 || p >= f.size()) return out.SetNull(), void();
    if (f[p] == '[' || f[p] == '{') {
      const bool obj = f[p] == '{';
      const char close = obj ? '}' : ']';
      ++p;
      obj ? out.SetObject() : out.SetArray();
      while (true) {
        ws();
        if (p >= f.size()) return fail("unclosed flow collection");
        if (f[p] == close) {
          ++p;
          return;
        }
        if (f[p] == ',') {
          ++p;
          continue;
        }
        if (obj) {
          Value k;
          scalar_token(f, p, k, true);
          ws();
          if (p < f.size() && f[p] == ':') ++p;
          Value v;
          ws();
          if (p < f.size() && (f[p] == ',' || f[p] == '}')) v.SetNull();
          else flow_value(f, p, v, depth + 1);
          if (!k.IsString()) {
            const std::string ks = str(&k);
            k.SetString(ks.c_str(), static_cast<rapidjson::SizeType>(ks.size()), al_);
          }
          out.AddMember(k, v, al_);
        } else {
          Value v;
          flow_value(f, p, v, depth + 1);
          out.PushBack(v, al_);
        }
      }
    }
    scalar_token(f, p, out, false);
  }
  void scalar_token(const std::string& f, size_t& p, Value& out, bool key) {
    if (p < f.size() && (f[p] == '"' || f[p] == '\'')) {
      const size_t start = p;
      const char q = f[p++];
      for (; p < f.size(); ++p) {
        if (q == '"' && f[p] == '\\') ++p;
        else if (f[p] == q) {
          if (q == '\'' && p + 1 < f.size() && f[p + 1] == '\'') {
            ++p;
            continue;
          }
          break;
        }
      }
      ++p;
      const std::string s = unquote(f.substr(start, p - start));
      out.SetString(s.c_str(), static_cast<rapidjson::SizeType>(s.size()), al_);
      return;
    }
    const size_t start = p;
    while (p < f.size() && f[p] != ',' && f[p] != ']' && f[p] != '}' &&
           !(key && f[p] == ':' && (p + 1 >= f.size() || f[p + 1] == ' ')))
      ++p;
    plain(trim(f.substr(start, p - start)), out);
  }
};

std::optional<ImportResult> parse_document(const Document& d, std::string* err) {
  if (!d.IsObject()) {
    if (err) *err = "not a collection (the file is not a JSON / YAML object)";
    return std::nullopt;
  }
  if (mem(d, "openapi") || mem(d, "swagger")) return parse_openapi(d, err);
  if (mem(d, "info") && mem(d, "item")) return parse_postman(d, err);
  if (str(d, "_type") == "export" || mem(d, "__export_format")) {
    const Value* f = mem(d, "__export_format");
    if (f && f->IsInt() && f->GetInt() != 4) {
      if (err) *err = "Insomnia export format " + std::to_string(f->GetInt()) + ": export it as \"Insomnia v4 (JSON)\"";
      return std::nullopt;
    }
    return parse_insomnia(d, err);
  }
  if (mem(mem(d, "log"), "entries")) return parse_har(d, err);
  if (arr(d, "values") && (mem(d, "_postman_variable_scope") || mem(d, "name"))) return parse_postman_env(d);
  if (mem(d, "requests") && mem(d, "order")) {
    if (err) *err = "a Postman v1 collection: export it again from Postman as Collection v2.1";
    return std::nullopt;
  }
  if (err) *err = "not a Postman, Insomnia, OpenAPI or HAR file";
  return std::nullopt;
}

std::string folder_name(const std::string& name) {
  std::string n = name.empty() ? std::string("Folder") : file_safe(name);
  if (n == "request" && name != "request") n = "Folder";
  if (n.size() > 100) {
    size_t cut = 100;
    while (cut > 0 && (static_cast<unsigned char>(n[cut]) & 0xC0) == 0x80) --cut;
    n.resize(cut);
    while (!n.empty() && (n.back() == ' ' || n.back() == '.')) n.pop_back();
  }
  if (n.empty()) n = "Folder";
  if (n[0] == '.') n = "_" + n;
  std::string stem = upper(n.substr(0, n.find('.')));
  static const char* kReserved[] = {"CON", "PRN", "AUX", "NUL"};
  bool reserved = std::find(std::begin(kReserved), std::end(kReserved), stem) != std::end(kReserved);
  if (stem.size() == 4 && (starts_with(stem, "COM") || starts_with(stem, "LPT")) && std::isdigit(static_cast<unsigned char>(stem[3])))
    reserved = true;
  if (reserved) n = "_" + n;
  return n;
}

bool inside(const fs::path& p, const fs::path& root) {
  const auto& a = p.native();
  const auto& b = root.native();
  if (a == b) return true;
  return a.size() > b.size() && a.compare(0, b.size(), b) == 0 && (a[b.size()] == L'\\' || a[b.size()] == L'/');
}

// One request file to write: the request (file and name set) and where it goes in the order.
struct WriteJob {
  SavedRequest* r;
  std::vector<std::wstring>* order;
  size_t slot;
  bool ok = false;
  std::string err;
};

// Creates the folders and names every file (in memory: the folders are new, so nothing on disk
// can clash), in collection order. The files are written afterwards, in parallel.
// A folder's variables to write as <dir>/folder.json.
using VarsJob = std::pair<fs::path, const ImportFolder*>;

void plan_folder(const fs::path& dir, ImportFolder& f, ImportWritten& w, std::vector<WriteJob>& jobs,
                 std::vector<VarsJob>& vars, int depth) {
  std::error_code ec;
  if (!fs::create_directories(dir, ec) && ec) {
    w.skipped.push_back(f.name + ": the folder could not be created (" + ec.message() + ")");
    return;
  }
  ++w.folders;
  if (f.has_settings()) vars.emplace_back(dir, &f);
  std::vector<std::wstring>& order = w.order[dir];
  std::set<std::string> used;  // lower-case names in dir (Windows names are case-insensitive)
  for (auto& sub : f.folders) {
    if (depth > kMaxDepth) break;
    const std::string base = folder_name(sub.name);
    std::string n = base;
    for (int k = 2; used.count(lower_ascii(n)) && k < 100000; ++k) n = base + " " + std::to_string(k);
    used.insert(lower_ascii(n));
    const fs::path p = dir / from_u8s(n);
    order.push_back(p.filename().native());
    plan_folder(p, sub, w, jobs, vars, depth + 1);
  }
  for (auto& r : f.requests) {
    std::string n = r.name;
    std::string file = slug(n) + ".req.json";
    for (int k = 2; used.count(lower_ascii(file)) && k < 100000; ++k) {
      n = r.name + " " + std::to_string(k);
      file = slug(n) + ".req.json";
    }
    used.insert(lower_ascii(file));
    r.name = n;
    r.file = dir / from_u8s(file);
    r.id.clear();
    jobs.push_back(WriteJob{&r, &order, order.size()});
    order.emplace_back();  // filled once written
  }
}

// Writes the planned files: Workspace::save_request() for each (it refuses any path outside the
// workspace), on a few threads (a file costs ~2.5 ms on Windows: create, rename, index line).
void write_jobs(Workspace& ws, std::vector<WriteJob>& jobs) {
  if (jobs.empty()) return;
  auto save = [&](WriteJob& j) { j.ok = ws.save_request(*j.r, &j.err); };
  save(jobs[0]);  // the first save sets up the shared badge index, before any thread shares it
  if (!jobs[0].ok) {  // maybe it did not get that far: no threads then
    for (size_t i = 1; i < jobs.size(); ++i) save(jobs[i]);
    return;
  }
  std::atomic<size_t> next{1};
  // 2000 files: 4.4 s on one thread, 2.5 s on 4-8 (the badge index's journal line per file is
  // written under its lock, which bounds it).
  const unsigned n = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
  std::vector<std::thread> pool;
  for (unsigned t = 0; t + 1 < n && next < jobs.size(); ++t)
    pool.emplace_back([&] {
      for (size_t i; (i = next++) < jobs.size();) save(jobs[i]);
    });
  for (size_t i; (i = next++) < jobs.size();) save(jobs[i]);
  for (auto& t : pool) t.join();
}

}  // namespace

std::optional<std::string> yaml_to_json(const std::string& yaml, std::string* err) {
  Document d;
  Yaml y(yaml, d.GetAllocator());
  Value v;
  if (!y.parse(v, err)) return std::nullopt;
  return json_text(v);
}

std::optional<ImportResult> parse_collection_text(const std::string& text_in, std::string* err) {
  std::string_view text = text_in;
  if (starts_with(text, "\xEF\xBB\xBF")) text.remove_prefix(3);
  size_t first = 0;
  while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first]))) ++first;
  if (first == text.size()) {
    if (err) *err = "the file is empty";
    return std::nullopt;
  }
  Document d;
  if (text[first] == '{' || text[first] == '[') {
    d.Parse<rapidjson::kParseNanAndInfFlag>(text.data(), text.size());
    if (d.HasParseError()) {
      if (err)
        *err = std::string("not valid JSON: ") + rapidjson::GetParseError_En(d.GetParseError()) + " (at byte " +
               std::to_string(d.GetErrorOffset()) + ")";
      return std::nullopt;
    }
  } else {
    Yaml y{std::string(text), d.GetAllocator()};
    if (!y.parse(d, err)) return std::nullopt;
    if (!d.IsObject() || !(mem(d, "openapi") || mem(d, "swagger"))) {
      if (err) *err = "a YAML file that is not an OpenAPI / Swagger document";
      return std::nullopt;
    }
  }
  return parse_document(d, err);
}

std::optional<ImportResult> parse_collection_file(const fs::path& file, std::string* err) {
  std::error_code ec;
  if (fs::is_directory(file, ec)) return parse_bruno_folder(file, err);
  const auto size = fs::file_size(file, ec);
  if (ec) {
    if (err) *err = "cannot read " + u8s(file) + ": " + ec.message();
    return std::nullopt;
  }
  if (size > kMaxImportBytes) {
    if (err) *err = "the file is larger than 64 MB";
    return std::nullopt;
  }
  std::string text;
  if (!read_small(file, text, kMaxImportBytes)) {
    if (err) *err = "cannot read " + u8s(file);
    return std::nullopt;
  }
  if (bru_file(file)) return parse_bruno_folder(file.parent_path(), err);
  return parse_collection_text(text, err);
}

std::optional<ImportResult> parse_bruno_folder(const fs::path& dir_in, std::string* err) {
  std::error_code ec;
  fs::path dir = dir_in;
  if (!fs::is_directory(dir, ec)) {
    if (err) *err = u8s(dir) + " is not a folder";
    return std::nullopt;
  }
  ImportResult res;
  res.format = "Bruno";
  res.root.name = u8s(dir.filename());
  std::string text;
  const bool has_json = read_small(dir / L"bruno.json", text, 1u << 20);
  if (has_json) {
    Document d;
    if (!d.Parse(text.c_str(), text.size()).HasParseError())
      if (const std::string n = str(mem(d, "name")); !n.empty()) res.root.name = n;
  }
  BruLevel top;
  if (read_small(dir / L"collection.bru", text)) {
    const auto bs = parse_bru(text);
    top = bru_level(bs, top);
    bru_folder_vars(bs, res.root);
  }
  else if (!has_json) {
    // Not marked as a collection: accept it only when it holds .bru requests.
    bool any = false;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end && !any; it.increment(ec))
      any = it->is_regular_file(ec) && bru_file(it->path());
    if (!any) {
      if (err) *err = "no Bruno collection here (no bruno.json and no .bru files)";
      return std::nullopt;
    }
  }
  size_t count = 0, files = 0;
  bru_walk(dir, res.root, top, res, count, 1, files);
  // environments/*.bru
  for (fs::directory_iterator it(dir / L"environments", ec), end; !ec && it != end; it.increment(ec)) {
    if (!bru_file(it->path()) || !read_small(it->path(), text)) continue;
    const auto bs = parse_bru(text);
    Environment e;
    e.name = u8s(it->path().stem());
    if (const BruBlock* v = bru_find(bs, "vars"))
      for (const auto& kv : v->kv)
        if (!kv.disabled) e.vars[kv.key] = kv.value;
    if (const BruBlock* s = bru_find(bs, "vars:secret"))
      for (std::string n : s->list) {
        if (!n.empty() && n[0] == '~') continue;
        e.vars.emplace(n, "");
        e.secrets.insert(n);
      }
    res.envs.push_back(std::move(e));
  }
  // Bruno ranks an environment above the collection's variables, watt ranks folder variables
  // above the environment: a collection variable an environment also defines moves into the
  // environments (each one that lacks it gets the collection value) so the environment still wins.
  std::erase_if(res.root.vars, [&](const SettingsRow& row) {
    const bool clash = std::any_of(res.envs.begin(), res.envs.end(), [&](const Environment& e) { return e.vars.count(row.name) > 0; });
    if (clash && row.enabled)
      for (auto& e : res.envs) e.vars.emplace(row.name, row.value);
    return clash;
  });
  if (count == 0 && res.skipped.empty()) {
    if (err) *err = "the Bruno collection has no requests";
    return std::nullopt;
  }
  return res;
}

ImportWritten write_import(Workspace& ws, const fs::path& dest_in, ImportResult& r) {
  ImportWritten w;
  w.skipped = r.skipped;
  const fs::path root = ws.root().lexically_normal();
  fs::path dest = dest_in.empty() ? root : dest_in.lexically_normal();
  if (!inside(dest, root)) {
    w.skipped.push_back("everything: the destination is outside the workspace");
    return w;
  }
  if (r.root.requests.empty() && r.root.folders.empty()) return w;  // environments only
  const std::string base = folder_name(r.root.name.empty() ? std::string("Imported") : r.root.name);
  std::string n = base;
  // Never the workspace's own environments/ folder.
  auto taken = [&](const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec) || (dest == root && lower_ascii(u8s(p.filename())) == "environments");
  };
  fs::path top = dest / from_u8s(n);
  for (int k = 2; taken(top) && k < 10000; ++k) top = dest / from_u8s(base + " " + std::to_string(k));
  w.top = top;
  std::vector<WriteJob> jobs;
  std::vector<VarsJob> vars;
  plan_folder(top, r.root, w, jobs, vars, 1);
  write_jobs(ws, jobs);
  // Collection / folder variables: the folder's settings (it is new, so nothing else is in it).
  for (const auto& [dir, f] : vars) {
    FolderSettings fs_;
    fs_.vars = f->vars;
    fs_.headers = f->headers;
    fs_.auth = f->auth;
    fs_.basic_auth = f->basic_auth;
    std::string err;
    if (ws.save_folder_settings(dir, fs_, &err)) w.vars += f->vars.size();
    else w.skipped.push_back(f->name + " settings: " + (err.empty() ? std::string("folder.json could not be written") : err));
  }
  for (auto& j : jobs) {
    if (!j.ok) {
      w.skipped.push_back(j.r->name + ": " + j.err);
      continue;
    }
    ++w.requests;
    if (w.first_file.empty()) w.first_file = j.r->file;
    (*j.order)[j.slot] = j.r->file.filename().native();
  }
  for (auto& [dir, names] : w.order) std::erase_if(names, [](const std::wstring& x) { return x.empty(); });
  return w;
}

size_t merge_import_envs(std::vector<Environment>& envs, const std::vector<Environment>& imported) {
  size_t changed = 0;
  for (const auto& e : imported) {
    if (e.name.empty()) continue;
    auto it = std::find_if(envs.begin(), envs.end(), [&](const Environment& x) { return x.name == e.name; });
    if (it == envs.end()) {
      envs.push_back(e);
      ++changed;
      continue;
    }
    bool any = false;
    for (const auto& [k, v] : e.vars)
      if (it->vars.emplace(k, v).second) {
        any = true;
        if (e.secrets.count(k)) it->secrets.insert(k);
      }
    changed += any;
  }
  return changed;
}

namespace {

// {{name}} uses in s (trimmed names; dynamic {{$guid}}-style names and anything with a space or
// a brace are not variables a user defines).
void var_uses(const std::string& s, std::vector<std::string>& out) {
  for (size_t i = s.find("{{"); i != std::string::npos; i = s.find("{{", i)) {
    const size_t e = s.find("}}", i + 2);
    if (e == std::string::npos) break;
    std::string n = s.substr(i + 2, e - i - 2);
    const size_t a = n.find_first_not_of(" \t"), b = n.find_last_not_of(" \t");
    n = a == std::string::npos ? std::string() : n.substr(a, b - a + 1);
    i = e + 2;
    if (n.empty() || n[0] == '$' || n.rfind("process.env.", 0) == 0 || n.find_first_of(" {}\t\r\n") != std::string::npos) continue;
    out.push_back(std::move(n));
  }
}

void undefined_in(const ImportFolder& f, std::set<std::string> defined, std::vector<std::string>& out) {
  for (const auto& v : f.vars)
    if (v.enabled) defined.insert(v.name);
  for (const auto& r : f.requests) {
    std::vector<std::string> uses;
    var_uses(r.http.url, uses);
    for (size_t i = 0; i < r.http.headers.size(); ++i)
      if (!r.disabled_headers.count(i)) {
        var_uses(r.http.headers[i].first, uses);
        var_uses(r.http.headers[i].second, uses);
      }
    var_uses(r.http.body, uses);
    var_uses(r.basic_auth.user, uses);
    var_uses(r.basic_auth.password, uses);
    for (const auto& [k, v] : r.path_params) var_uses(v, uses);
    for (auto& n : uses)
      if (!defined.count(n) && std::find(out.begin(), out.end(), n) == out.end()) out.push_back(std::move(n));
  }
  for (const auto& sub : f.folders) undefined_in(sub, defined, out);
}

}  // namespace

std::vector<std::string> undefined_import_vars(const ImportResult& r, const std::set<std::string>& defined) {
  std::set<std::string> d = defined;
  for (const auto& e : r.envs)
    for (const auto& [k, v] : e.vars) d.insert(k);
  std::vector<std::string> out;
  undefined_in(r.root, std::move(d), out);
  return out;
}

std::string import_summary(const ImportResult& r, const ImportWritten& w) {
  auto plural = [](size_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); };
  std::string s = "Imported " + plural(w.requests, "request", "requests") + ", " + plural(w.folders, "folder", "folders") +
                  ", " + std::to_string(w.skipped.size()) + " skipped";
  if (!w.skipped.empty()) {
    // Reasons grouped: "(3 ftp:// URL (only http and https), 1 no URL)".
    std::vector<std::pair<std::string, size_t>> why;
    for (const auto& x : w.skipped) {
      const size_t c = x.rfind(": ");
      const std::string reason = c == std::string::npos ? x : x.substr(c + 2);
      auto it = std::find_if(why.begin(), why.end(), [&](const auto& p) { return p.first == reason; });
      if (it == why.end()) why.emplace_back(reason, 1);
      else ++it->second;
    }
    s += " (";
    for (size_t i = 0; i < why.size() && i < 4; ++i) s += (i ? ", " : "") + std::to_string(why[i].second) + " " + why[i].first;
    if (why.size() > 4) s += ", ...";
    s += ")";
  }
  if (w.vars) s += ", " + plural(w.vars, "variable", "variables");
  if (!r.envs.empty()) s += ", " + plural(r.envs.size(), "environment", "environments");
  s += " from " + r.format + ".";
  return s;
}

// ======================================================================= export

namespace {

std::string new_id() {
  static thread_local std::mt19937_64 g{std::random_device{}() ^
                                        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
  const uint64_t a = g(), b = g();
  char buf[40];
  std::snprintf(buf, sizeof buf, "%08x-%04x-4%03x-%04x-%012llx", static_cast<unsigned>(a >> 32),
                static_cast<unsigned>((a >> 16) & 0xFFFF), static_cast<unsigned>(a & 0xFFF),
                static_cast<unsigned>(0x8000 | ((b >> 48) & 0x3FFF)), static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFull));
  return buf;
}

void request_files(const fs::path& dir, const std::map<fs::path, std::vector<std::wstring>>* order,
                   std::vector<fs::path>& dirs, std::vector<fs::path>& files) {
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const fs::path p = it->path();
    const std::wstring n = p.filename().native();
    if (n.empty() || n[0] == L'.') continue;
    if (it->is_directory(ec)) {
      if (!it->is_symlink(ec)) dirs.push_back(p);
    } else if (n.size() > 9 && n.compare(n.size() - 9, 9, L".req.json") == 0) {
      files.push_back(p);
    }
  }
  auto rank = [&](const fs::path& p) -> size_t {
    if (!order) return SIZE_MAX;
    auto it = order->find(dir);
    if (it == order->end()) return SIZE_MAX;
    for (size_t i = 0; i < it->second.size(); ++i)
      if (it->second[i] == p.filename().native()) return i;
    return SIZE_MAX;
  };
  auto less = [&](const fs::path& a, const fs::path& b) {
    const size_t ra = rank(a), rb = rank(b);
    if (ra != rb) return ra < rb;
    return natural_less(u8s(a.filename()), u8s(b.filename()));
  };
  std::sort(dirs.begin(), dirs.end(), less);
  std::sort(files.begin(), files.end(), less);
}

Value jv(const std::string& s, Document::AllocatorType& al) {
  return Value(s.c_str(), static_cast<rapidjson::SizeType>(s.size()), al);
}

Value kv_entry(const std::string& k, const std::string& v, Document::AllocatorType& al) {
  Value o(rapidjson::kObjectType);
  o.AddMember("key", jv(k, al), al);
  o.AddMember("value", jv(v, al), al);
  return o;
}

Value pm_request_item(const SavedRequest& r, Document::AllocatorType& al) {
  Value item(rapidjson::kObjectType);
  item.AddMember("name", jv(r.name, al), al);
  // Scripts as Postman events (both run in Postman's sandbox, with pm.*).
  Value events(rapidjson::kArrayType);
  auto event = [&](const char* listen, const std::string& code) {
    if (trim(code).empty()) return;
    Value exec(rapidjson::kArrayType);
    for (size_t p = 0; p <= code.size();) {
      size_t e = code.find('\n', p);
      if (e == std::string::npos) e = code.size();
      std::string line = code.substr(p, e - p);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      exec.PushBack(jv(line, al), al);
      p = e + 1;
    }
    Value script(rapidjson::kObjectType);
    script.AddMember("type", "text/javascript", al);
    script.AddMember("exec", exec, al);
    Value ev(rapidjson::kObjectType);
    ev.AddMember("listen", Value(listen, al), al);
    ev.AddMember("script", script, al);
    events.PushBack(ev, al);
  };
  event("prerequest", r.pre_script);
  event("test", r.post_script);
  if (!events.Empty()) item.AddMember("event", events, al);

  Value req(rapidjson::kObjectType);
  // Auth.
  auto auth_obj = [&](const char* type, std::initializer_list<std::pair<const char*, std::string>> kvs) {
    Value a(rapidjson::kObjectType);
    a.AddMember("type", Value(type, al), al);
    Value list(rapidjson::kArrayType);
    for (const auto& [k, v] : kvs) list.PushBack(kv_entry(k, v, al), al);
    a.AddMember(Value(type, al), list, al);
    req.AddMember("auth", a, al);
  };
  if (r.auth.type == HttpAuth::Type::Digest) auth_obj("digest", {{"username", r.auth.user}, {"password", r.auth.password}});
  else if (r.auth.type == HttpAuth::Type::Ntlm) auth_obj("ntlm", {{"username", r.auth.user}, {"password", r.auth.password}});
  else if (r.auth.type == HttpAuth::Type::AwsSigV4)
    auth_obj("awsv4", {{"accessKey", r.auth.aws_access_key}, {"secretKey", r.auth.aws_secret_key},
                       {"sessionToken", r.auth.aws_session_token}, {"region", r.auth.aws_region},
                       {"service", r.auth.aws_service}});
  else if (r.auth.type == HttpAuth::Type::OAuth2) {
    const OAuth2Config& o = r.auth.oauth2;
    const char* grant = o.grant == OAuth2Config::Grant::Password            ? "password_credentials"
                        : o.grant == OAuth2Config::Grant::AuthorizationCode ? "authorization_code"
                                                                            : "client_credentials";
    auth_obj("oauth2", {{"grant_type", grant}, {"accessTokenUrl", o.token_url}, {"authUrl", o.auth_url},
                        {"clientId", o.client_id}, {"clientSecret", o.client_secret}, {"scope", o.scope},
                        {"username", o.username}, {"password", o.password}});
  } else if (r.basic_auth.enabled)
    auth_obj("basic", {{"username", r.basic_auth.user}, {"password", r.basic_auth.password}});
  req.AddMember("method", jv(r.http.method.empty() ? "GET" : r.http.method, al), al);
  Value headers(rapidjson::kArrayType);
  std::string content_type;
  for (size_t i = 0; i < r.http.headers.size(); ++i) {
    const auto& [k, v] = r.http.headers[i];
    Value h = kv_entry(k, v, al);
    if (r.disabled_headers.count(i)) h.AddMember("disabled", true, al);
    else if (iequals(k, "Content-Type")) content_type = lower_ascii(v);
    headers.PushBack(h, al);
  }
  req.AddMember("header", headers, al);
  // Body.
  Value body(rapidjson::kObjectType);
  if (!r.form.empty()) {
    body.AddMember("mode", "formdata", al);
    Value parts(rapidjson::kArrayType);
    for (const auto& f : r.form) {
      Value p(rapidjson::kObjectType);
      p.AddMember("key", jv(f.name, al), al);
      if (f.kind == FormPart::Kind::File) {
        p.AddMember("type", "file", al);
        p.AddMember("src", jv(f.value, al), al);
      } else {
        p.AddMember("value", jv(f.value, al), al);
        p.AddMember("type", "text", al);
      }
      if (!f.type.empty()) p.AddMember("contentType", jv(f.type, al), al);
      if (!f.enabled) p.AddMember("disabled", true, al);
      parts.PushBack(p, al);
    }
    body.AddMember("formdata", parts, al);
  } else if (!r.body_file.empty()) {
    body.AddMember("mode", "file", al);
    Value file(rapidjson::kObjectType);
    file.AddMember("src", jv(r.body_file, al), al);
    body.AddMember("file", file, al);
  } else if (!r.http.body.empty()) {
    body.AddMember("mode", "raw", al);
    body.AddMember("raw", jv(r.http.body, al), al);
    const char* lang = content_type.find("json") != std::string::npos  ? "json"
                       : content_type.find("xml") != std::string::npos ? "xml"
                       : content_type.find("html") != std::string::npos ? "html"
                                                                        : "text";
    Value raw(rapidjson::kObjectType);
    raw.AddMember("language", Value(lang, al), al);
    Value opts(rapidjson::kObjectType);
    opts.AddMember("raw", raw, al);
    body.AddMember("options", opts, al);
  }
  if (body.MemberCount()) req.AddMember("body", body, al);
  // URL: raw plus the query list (Postman sends what the list holds: enabled and disabled).
  Value url(rapidjson::kObjectType);
  url.AddMember("raw", jv(r.http.url, al), al);
  Value query(rapidjson::kArrayType);
  if (const size_t q = r.http.url.find('?'); q != std::string::npos) {
    std::string qs = r.http.url.substr(q + 1);
    if (const size_t h = qs.find('#'); h != std::string::npos) qs.resize(h);
    for (size_t p = 0; p <= qs.size();) {
      size_t e = qs.find('&', p);
      if (e == std::string::npos) e = qs.size();
      const std::string part = qs.substr(p, e - p);
      if (!part.empty()) {
        const size_t eq = part.find('=');
        query.PushBack(kv_entry(part.substr(0, eq), eq == std::string::npos ? std::string() : part.substr(eq + 1), al), al);
      }
      p = e + 1;
    }
  }
  for (const auto& [k, v] : r.disabled_params) {
    Value e = kv_entry(k, v, al);
    e.AddMember("disabled", true, al);
    query.PushBack(e, al);
  }
  if (!query.Empty()) url.AddMember("query", query, al);
  if (!r.path_params.empty()) {
    Value vars(rapidjson::kArrayType);
    for (const auto& [k, v] : r.path_params) vars.PushBack(kv_entry(k, v, al), al);
    url.AddMember("variable", vars, al);
  }
  req.AddMember("url", url, al);
  if (!r.docs.empty()) req.AddMember("description", jv(r.docs, al), al);
  item.AddMember("request", req, al);
  item.AddMember("response", Value(rapidjson::kArrayType), al);
  return item;
}

void export_dir(const Workspace& ws, const fs::path& dir, const std::map<fs::path, std::vector<std::wstring>>* order,
                Value& items, Document::AllocatorType& al, ExportResult& res, int depth) {
  std::vector<fs::path> dirs, files;
  request_files(dir, order, dirs, files);
  for (const auto& d : dirs) {
    if (depth > kMaxDepth) break;
    Value sub(rapidjson::kArrayType);
    export_dir(ws, d, order, sub, al, res, depth + 1);
    if (sub.Empty()) continue;
    Value f(rapidjson::kObjectType);
    f.AddMember("name", jv(u8s(d.filename()), al), al);
    f.AddMember("item", sub, al);
    items.PushBack(f, al);
    ++res.folders;
  }
  for (const auto& p : files) {
    std::string err;
    auto r = ws.load_request(p, &err);
    if (!r) {
      res.skipped.push_back(u8s(p.filename()) + ": " + err);
      continue;
    }
    if (r->kind == RequestKind::Grpc) {
      res.skipped.push_back(r->name + ": gRPC (a Postman collection holds HTTP requests)");
      continue;
    }
    items.PushBack(pm_request_item(*r, al), al);
    ++res.requests;
  }
}

}  // namespace

ExportResult export_postman(const Workspace& ws, const fs::path& folder, const std::string& name,
                            const std::map<fs::path, std::vector<std::wstring>>* order) {
  ExportResult res;
  Document d;
  d.SetObject();
  auto& al = d.GetAllocator();
  Value info(rapidjson::kObjectType);
  info.AddMember("_postman_id", jv(new_id(), al), al);
  info.AddMember("name", jv(name, al), al);
  info.AddMember("schema", "https://schema.getpostman.com/json/collection/v2.1.0/collection.json", al);
  d.AddMember("info", info, al);
  Value items(rapidjson::kArrayType);
  export_dir(ws, folder, order, items, al, res, 1);
  d.AddMember("item", items, al);
  res.text = json_pretty_text(d);
  return res;
}

ExportResult export_curl(const Workspace& ws, const fs::path& folder, CurlShell shell,
                         const std::map<fs::path, std::vector<std::wstring>>* order) {
  ExportResult res;
  const char* comment = shell == CurlShell::Cmd ? "REM " : "# ";
  std::function<void(const fs::path&, const std::string&, int)> walk = [&](const fs::path& dir, const std::string& prefix,
                                                                            int depth) {
    std::vector<fs::path> dirs, files;
    request_files(dir, order, dirs, files);
    for (const auto& p : files) {
      std::string err;
      auto r = ws.load_request(p, &err);
      if (!r) {
        res.skipped.push_back(u8s(p.filename()) + ": " + err);
        continue;
      }
      const SavedRequest x = ws.expanded(*r);
      if (!res.text.empty()) res.text += "\n";
      res.text += comment + prefix + r->name + "\n";
      res.text += x.kind == RequestKind::Grpc ? to_grpcurl_command(x) : to_curl_command(x, shell);
      res.text += "\n";
      ++res.requests;
    }
    for (const auto& d : dirs) {
      if (depth > kMaxDepth) break;
      ++res.folders;
      walk(d, prefix + u8s(d.filename()) + "/", depth + 1);
    }
  };
  walk(folder, "", 1);
  return res;
}

}  // namespace watt::app::sidebar
