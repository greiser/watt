#include "core/oauth2.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

#include <rapidjson/document.h>

#include "core/crypto.h"
#include "core/model.h"
#include "core/model_json.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")

namespace watt {

namespace {

int64_t unix_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool istarts_with(std::string_view s, std::string_view p) {
  if (s.size() < p.size()) return false;
  for (size_t i = 0; i < p.size(); ++i)
    if (lower(s[i]) != lower(p[i])) return false;
  return true;
}

int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string url_decode(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') {
      out.push_back(' ');
    } else if (s[i] == '%' && i + 2 < s.size() && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
      out.push_back(static_cast<char>(hexval(s[i + 1]) * 16 + hexval(s[i + 2])));
      i += 2;
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

std::map<std::string, std::string> parse_form(std::string_view q) {
  std::map<std::string, std::string> out;
  while (!q.empty()) {
    const size_t amp = q.find('&');
    const std::string_view part = q.substr(0, amp);
    q = amp == std::string_view::npos ? std::string_view{} : q.substr(amp + 1);
    if (part.empty()) continue;
    const size_t eq = part.find('=');
    const std::string k = url_decode(part.substr(0, eq));
    if (out.count(k)) continue;  // first one wins
    out[k] = eq == std::string_view::npos ? std::string{} : url_decode(part.substr(eq + 1));
  }
  return out;
}

// ------------------------------------------------------------------ cache

struct Cache {
  std::mutex mu;
  std::map<std::string, OAuth2Token> tokens;
  std::map<std::string, std::shared_ptr<std::mutex>> fetching;  // one token request per key at a time
};
Cache& cache() {
  static Cache c;
  return c;
}

// The expanded settings of `r` and the cache key they make.
struct Resolved {
  OAuth2Config cfg;
  std::string key;
};

Resolved resolve(const Workspace& ws, const SavedRequest& r) {
  Resolved out;
  out.cfg = ws.expanded(r).auth.oauth2;
  const OAuth2Config& c = out.cfg;
  std::string k;
  auto add = [&](const std::string& v) {
    k += std::to_string(v.size());
    k += ':';
    k += v;
  };
  add(detail::to_utf8(ws.root()));
  add(ws.active_env);
  add(!r.id.empty() ? r.id : detail::to_utf8(r.file));
  add(std::to_string(static_cast<int>(c.grant)));
  add(c.token_url);
  add(c.client_id);
  add(c.scope);
  add(c.audience);
  add(c.username);
  add(c.auth_url);
  out.key = std::move(k);
  return out;
}

// ------------------------------------------------------------------ token endpoint

struct TokenCall {
  const OAuth2Config& cfg;
  const OAuth2Hooks& hooks;
};

bool parse_token_response(const HttpResponse& resp, OAuth2Token& t, std::string& err) {
  if (!resp.ok) {
    err = "token request failed: " + resp.error;
    return false;
  }
  std::map<std::string, std::string> f;  // flattened fields
  rapidjson::Document d;
  d.Parse(resp.body.data(), resp.body.size());
  int64_t expires_in = -1;
  if (!d.HasParseError() && d.IsObject()) {
    for (auto it = d.MemberBegin(); it != d.MemberEnd(); ++it) {
      const std::string name(it->name.GetString(), it->name.GetStringLength());
      if (it->value.IsString()) f[name].assign(it->value.GetString(), it->value.GetStringLength());
      else if (it->value.IsInt64()) f[name] = std::to_string(it->value.GetInt64());
      else if (it->value.IsNumber()) f[name] = std::to_string(static_cast<int64_t>(it->value.GetDouble()));
    }
  } else if (istarts_with(resp.headers.get("content-type"), "application/x-www-form-urlencoded") ||
             resp.body.find('=') != std::string::npos) {
    f = parse_form(resp.body);  // GitHub without Accept: application/json, and others
  }
  if (resp.status < 200 || resp.status >= 300 || f.count("error") || !f.count("access_token")) {
    std::string e = f.count("error") ? f["error"] : "HTTP " + std::to_string(resp.status);
    if (f.count("error_description")) e += ": " + f["error_description"];
    else if (!f.count("error") && resp.status >= 200 && resp.status < 300) e = "the token response has no access_token";
    err = e;
    return false;
  }
  t.access_token = f["access_token"];
  t.token_type = f.count("token_type") ? f["token_type"] : "Bearer";
  if (f.count("refresh_token")) t.refresh_token = f["refresh_token"];
  if (f.count("scope")) t.scope = f["scope"];
  if (f.count("id_token")) t.id_token = f["id_token"];
  if (f.count("expires_in")) {
    try {
      expires_in = std::stoll(f["expires_in"]);
    } catch (...) {
      expires_in = -1;
    }
  }
  t.obtained_at = unix_now();
  t.expires_at = expires_in > 0 ? t.obtained_at + expires_in : 0;
  return true;
}

// POST the form `fields` to the token endpoint with the client's credentials.
bool token_request(const TokenCall& call, std::vector<std::pair<std::string, std::string>> fields, OAuth2Token& t,
                   std::string& err) {
  const OAuth2Config& c = call.cfg;
  if (!is_safe_http_url(c.token_url)) {
    err = "the token URL \"" + c.token_url + "\" is not an http(s) URL";
    return false;
  }
  HttpRequest req;
  req.method = "POST";
  req.url = c.token_url;
  req.timeout_ms = call.hooks.request_timeout_ms;
  req.verify_tls = call.hooks.verify_tls;
  req.ca_pem = call.hooks.ca_pem;
  req.follow_redirects = false;  // a token endpoint does not redirect; never re-post secrets elsewhere
  req.headers.emplace_back("Content-Type", "application/x-www-form-urlencoded");
  req.headers.emplace_back("Accept", "application/json");
  if (c.client_auth == OAuth2Config::ClientAuth::BasicHeader && !c.client_id.empty()) {
    // RFC 6749 2.3.1: id and secret form-encoded, then Basic.
    req.headers.emplace_back("Authorization",
                             "Basic " + crypto::base64(form_urlencode(c.client_id) + ":" + form_urlencode(c.client_secret)));
  } else {
    if (!c.client_id.empty()) fields.emplace_back("client_id", c.client_id);
    if (!c.client_secret.empty()) fields.emplace_back("client_secret", c.client_secret);
  }
  if (!c.audience.empty()) fields.emplace_back("audience", c.audience);
  for (const auto& [k, v] : fields) {
    if (!req.body.empty()) req.body += '&';
    req.body += form_urlencode(k) + '=' + form_urlencode(v);
  }
  std::unique_ptr<HttpClient> own;
  HttpClient* client = call.hooks.client;
  if (!client) {
    own = std::make_unique<HttpClient>();
    client = own.get();
  }
  if (call.hooks.cancel && call.hooks.cancel->load()) {
    err = "cancelled";
    return false;
  }
  const HttpResponse resp = client->send(req);
  return parse_token_response(resp, t, err);
}

// ------------------------------------------------------------------ authorization code

struct Redirect {
  bool v6 = false;
  uint16_t port = 0;
  std::string host;  // as written ("127.0.0.1", "localhost", "[::1]")
  std::string path;  // "/callback"
};

bool parse_redirect(const std::string& uri, Redirect& r, std::string& err) {
  if (uri.empty()) {
    r.host = "127.0.0.1";
    r.path = "/callback";
    return true;
  }
  if (!istarts_with(uri, "http://")) {
    err = "the redirect URI must be http://127.0.0.1[:port]/path (or localhost / [::1]) to be received here";
    return false;
  }
  std::string_view rest(uri);
  rest.remove_prefix(7);
  const size_t slash = rest.find_first_of("/?#");
  std::string_view auth = rest.substr(0, slash);
  r.path = slash == std::string_view::npos ? "/" : std::string(rest.substr(slash));
  if (const size_t q = r.path.find_first_of("?#"); q != std::string::npos) r.path.resize(q);
  if (r.path.empty()) r.path = "/";
  std::string_view host = auth, port;
  if (!auth.empty() && auth.front() == '[') {
    const size_t close = auth.find(']');
    if (close == std::string_view::npos) {
      err = "invalid redirect URI";
      return false;
    }
    host = auth.substr(0, close + 1);
    if (close + 1 < auth.size() && auth[close + 1] == ':') port = auth.substr(close + 2);
  } else if (const size_t colon = auth.rfind(':'); colon != std::string_view::npos) {
    host = auth.substr(0, colon);
    port = auth.substr(colon + 1);
  }
  std::string h;
  for (char c : host) h.push_back(lower(c));
  if (h == "[::1]") r.v6 = true;
  else if (h != "127.0.0.1" && h != "localhost") {
    err = "the redirect URI host must be 127.0.0.1, localhost or [::1] (got \"" + std::string(host) + "\")";
    return false;
  }
  r.host = std::string(host);
  if (!port.empty()) {
    unsigned long p = 0;
    for (char c : port) {
      if (c < '0' || c > '9') {
        err = "invalid redirect URI port";
        return false;
      }
      p = p * 10 + static_cast<unsigned long>(c - '0');
      if (p > 65535) {
        err = "invalid redirect URI port";
        return false;
      }
    }
    r.port = static_cast<uint16_t>(p);
  }
  return true;
}

struct Wsa {
  bool ok = false;
  Wsa() {
    WSADATA d;
    ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
  }
  ~Wsa() {
    if (ok) WSACleanup();
  }
};

struct Sock {
  SOCKET s = INVALID_SOCKET;
  ~Sock() {
    if (s != INVALID_SOCKET) closesocket(s);
  }
};

void reply(SOCKET c, int code, const char* reason, const std::string& html) {
  std::string out = "HTTP/1.1 " + std::to_string(code) + " " + reason +
                    "\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\n"
                    "Content-Length: " +
                    std::to_string(html.size()) + "\r\n\r\n" + html;
  send(c, out.data(), static_cast<int>(out.size()), 0);
}

std::string page(const char* title, const char* text) {
  return std::string("<!doctype html><html><head><meta charset=\"utf-8\"><title>watt</title></head>"
                     "<body style=\"font-family:sans-serif;margin:3em\"><h2>") +
         title + "</h2><p>" + text + "</p></body></html>";
}

bool wide(const std::string& s, std::wstring& out) {
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return false;
  out.resize(static_cast<size_t>(n));
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return true;
}

bool system_browser(const std::string& url, std::string* err) {
  std::wstring w;
  if (!is_safe_http_url(url) || !wide(url, w)) {
    if (err) *err = "refusing to open a non-http(s) URL";
    return false;
  }
  const auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (r <= 32) {
    if (err) *err = "could not open the browser (ShellExecute error " + std::to_string(r) + ")";
    return false;
  }
  return true;
}

// Runs the authorization code grant: browser, redirect listener, code exchange.
bool authorization_code(const TokenCall& call, OAuth2Token& t, std::string& err) {
  const OAuth2Config& c = call.cfg;
  if (!is_safe_http_url(c.auth_url)) {
    err = "the authorization URL \"" + c.auth_url + "\" is not an http(s) URL";
    return false;
  }
  Redirect rd;
  if (!parse_redirect(c.redirect_uri, rd, err)) return false;
  Wsa wsa;
  if (!wsa.ok) {
    err = "Winsock initialisation failed";
    return false;
  }
  Sock ls;
  ls.s = socket(rd.v6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (ls.s == INVALID_SOCKET) {
    err = "cannot create the redirect listener";
    return false;
  }
  BOOL excl = TRUE;
  setsockopt(ls.s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof(excl));
  sockaddr_storage ss{};
  int sslen = 0;
  if (rd.v6) {
    auto* a = reinterpret_cast<sockaddr_in6*>(&ss);
    a->sin6_family = AF_INET6;
    a->sin6_port = htons(rd.port);
    a->sin6_addr = in6addr_loopback;
    sslen = sizeof(sockaddr_in6);
  } else {
    auto* a = reinterpret_cast<sockaddr_in*>(&ss);
    a->sin_family = AF_INET;
    a->sin_port = htons(rd.port);
    a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sslen = sizeof(sockaddr_in);
  }
  if (bind(ls.s, reinterpret_cast<sockaddr*>(&ss), sslen) != 0 || listen(ls.s, 4) != 0) {
    err = "cannot listen for the redirect on " + rd.host + ":" + std::to_string(rd.port) + " (port in use?)";
    return false;
  }
  getsockname(ls.s, reinterpret_cast<sockaddr*>(&ss), &sslen);
  const uint16_t port = ntohs(rd.v6 ? reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port
                                    : reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
  const std::string redirect_uri =
      c.redirect_uri.empty() ? "http://127.0.0.1:" + std::to_string(port) + rd.path : c.redirect_uri;

  const std::string state = crypto::base64(crypto::random_bytes(16), true, false);
  const std::string verifier = c.pkce ? pkce_verifier() : std::string{};
  if (state.empty() || (c.pkce && verifier.empty())) {
    err = "no random numbers";
    return false;
  }
  std::string url = c.auth_url;
  const size_t hash = url.find('#');
  const std::string frag = hash == std::string::npos ? std::string{} : url.substr(hash);
  if (hash != std::string::npos) url.resize(hash);
  auto param = [&](const char* k, const std::string& v) {
    if (url.find('?') == std::string::npos) url += '?';
    else if (url.back() != '?' && url.back() != '&') url += '&';
    url += k;
    url += '=';
    url += form_urlencode(v);
  };
  param("response_type", "code");
  param("client_id", c.client_id);
  param("redirect_uri", redirect_uri);
  if (!c.scope.empty()) param("scope", c.scope);
  if (!c.audience.empty()) param("audience", c.audience);
  param("state", state);
  if (c.pkce) {
    param("code_challenge", pkce_challenge_s256(verifier));
    param("code_challenge_method", "S256");
  }
  url += frag;

  std::string berr;
  const bool opened = call.hooks.open_browser ? call.hooks.open_browser(url, &berr) : system_browser(url, &berr);
  if (!opened) {
    err = berr.empty() ? "could not open the browser" : berr;
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(call.hooks.authorize_timeout_ms);
  std::string code;
  while (code.empty()) {
    if (call.hooks.cancel && call.hooks.cancel->load()) {
      err = "cancelled";
      return false;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      err = "timed out waiting for the browser to come back to " + redirect_uri;
      return false;
    }
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(ls.s, &rs);
    timeval tv{0, 200 * 1000};
    if (select(0, &rs, nullptr, nullptr, &tv) <= 0) continue;
    Sock cs;
    cs.s = accept(ls.s, nullptr, nullptr);
    if (cs.s == INVALID_SOCKET) continue;
    DWORD rto = 5000;
    setsockopt(cs.s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rto), sizeof(rto));
    std::string buf;
    char tmp[4096];
    while (buf.find("\r\n\r\n") == std::string::npos && buf.size() < 16384) {
      const int n = recv(cs.s, tmp, sizeof(tmp), 0);
      if (n <= 0) break;
      buf.append(tmp, static_cast<size_t>(n));
    }
    const size_t eol = buf.find("\r\n");
    const std::string line = buf.substr(0, eol);
    const size_t sp1 = line.find(' ');
    const size_t sp2 = sp1 == std::string::npos ? sp1 : line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos || line.compare(0, sp1, "GET") != 0) {
      reply(cs.s, 400, "Bad Request", page("Bad request", ""));
      continue;
    }
    const std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const size_t qm = target.find('?');
    if (target.substr(0, qm) != rd.path) {
      reply(cs.s, 404, "Not Found", page("Not found", ""));
      continue;
    }
    auto q = parse_form(qm == std::string::npos ? std::string_view{} : std::string_view(target).substr(qm + 1));
    // Only a callback carrying our state counts: anything else (a stale tab, a page on another
    // site posting to this port) is refused and the wait goes on.
    if (q["state"] != state) {
      reply(cs.s, 400, "Bad Request", page("watt: state mismatch", "This sign-in response was not for watt's request and was ignored."));
      continue;
    }
    if (q.count("error")) {
      err = "authorization failed: " + q["error"] + (q.count("error_description") ? ": " + q["error_description"] : "");
      reply(cs.s, 200, "OK", page("watt: sign-in failed", "You can close this tab and return to watt."));
      return false;
    }
    if (q["code"].empty()) {
      reply(cs.s, 400, "Bad Request", page("watt: no code", ""));
      continue;
    }
    code = q["code"];
    reply(cs.s, 200, "OK", page("watt: signed in", "You can close this tab and return to watt."));
  }
  std::vector<std::pair<std::string, std::string>> f = {
      {"grant_type", "authorization_code"}, {"code", code}, {"redirect_uri", redirect_uri}};
  if (c.pkce) f.emplace_back("code_verifier", verifier);
  return token_request(call, std::move(f), t, err);
}

}  // namespace

// ------------------------------------------------------------------ public

std::string form_urlencode(const std::string& s) {
  static const char* hx = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3 / 2);
  for (char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
        c == '_' || c == '~') {
      out.push_back(ch);
    } else {
      out.push_back('%');
      out.push_back(hx[c >> 4]);
      out.push_back(hx[c & 15]);
    }
  }
  return out;
}

bool is_safe_http_url(const std::string& url) {
  size_t start;
  if (istarts_with(url, "https://")) start = 8;
  else if (istarts_with(url, "http://")) start = 7;
  else return false;
  if (url.size() <= start || url[start] == '/' || url.size() > 8192) return false;
  for (char ch : url) {
    const auto c = static_cast<unsigned char>(ch);
    if (c <= 0x20 || c == 0x7f || c == '"' || c == '\\' || c == '<' || c == '>' || c == '`') return false;
  }
  return true;
}

std::string pkce_verifier() {
  const std::string r = crypto::random_bytes(32);
  return r.empty() ? std::string{} : crypto::base64(r, true, false);
}

std::string pkce_challenge_s256(const std::string& verifier) {
  return crypto::base64(crypto::digest(crypto::Hash::Sha256, verifier), true, false);
}

bool oauth2_apply_token(HttpRequest& req, const OAuth2Config& cfg, const OAuth2Token& t, std::string* err) {
  for (char c : t.access_token)
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
      if (err) *err = "the access token contains control characters";
      return false;
    }
  if (cfg.add_to == OAuth2Config::AddTo::Query) {
    if (cfg.query_param.empty()) {
      if (err) *err = "no query parameter name for the token";
      return false;
    }
    std::string& u = req.url;
    const size_t hash = u.find('#');
    const std::string frag = hash == std::string::npos ? std::string{} : u.substr(hash);
    if (hash != std::string::npos) u.resize(hash);
    u += u.find('?') == std::string::npos ? "?" : (u.back() == '?' || u.back() == '&') ? "" : "&";
    u += form_urlencode(cfg.query_param) + "=" + form_urlencode(t.access_token) + frag;
    return true;
  }
  for (char c : cfg.header_prefix)
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
      if (err) *err = "the header prefix contains control characters";
      return false;
    }
  for (const auto& [k, v] : req.headers) {
    std::string l;
    for (char c : k) l.push_back(lower(c));
    if (l == "authorization") return true;  // the request's own header wins
  }
  req.headers.emplace_back("Authorization", cfg.header_prefix.empty() ? t.access_token
                                                                      : cfg.header_prefix + " " + t.access_token);
  return true;
}

std::optional<OAuth2Token> oauth2_cached_token(const Workspace& ws, const SavedRequest& r) {
  const Resolved rs = resolve(ws, r);
  Cache& c = cache();
  std::lock_guard<std::mutex> lk(c.mu);
  auto it = c.tokens.find(rs.key);
  if (it == c.tokens.end()) return std::nullopt;
  return it->second;
}

void oauth2_forget(const Workspace& ws, const SavedRequest& r) {
  const Resolved rs = resolve(ws, r);
  Cache& c = cache();
  std::lock_guard<std::mutex> lk(c.mu);
  c.tokens.erase(rs.key);
}

void oauth2_clear_cache() {
  Cache& c = cache();
  std::lock_guard<std::mutex> lk(c.mu);
  c.tokens.clear();
}

OAuth2Result oauth2_get_token(const Workspace& ws, const SavedRequest& r, const OAuth2Hooks& hooks, bool force_new) {
  const auto t0 = std::chrono::steady_clock::now();
  OAuth2Result res;
  auto done = [&]() -> OAuth2Result {
    res.elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return res;
  };
  const Resolved rs = resolve(ws, r);
  const OAuth2Config& cfg = rs.cfg;
  Cache& c = cache();
  std::shared_ptr<std::mutex> key_mu;
  std::optional<OAuth2Token> cached;
  {
    std::lock_guard<std::mutex> lk(c.mu);
    auto& m = c.fetching[rs.key];
    if (!m) m = std::make_shared<std::mutex>();
    key_mu = m;
  }
  std::lock_guard<std::mutex> fetch_lock(*key_mu);  // a second send waits for the first one's token
  {
    std::lock_guard<std::mutex> lk(c.mu);
    if (auto it = c.tokens.find(rs.key); it != c.tokens.end()) cached = it->second;
  }
  const int64_t now = unix_now();
  if (!force_new && cached && cached->valid(now)) {
    res.ok = true;
    res.from_cache = true;
    res.token = *cached;
    return done();
  }
  TokenCall call{cfg, hooks};
  OAuth2Token t;
  std::string err;
  bool got = false;
  if (!force_new && cached && cfg.auto_refresh && !cached->refresh_token.empty()) {
    std::vector<std::pair<std::string, std::string>> f = {{"grant_type", "refresh_token"},
                                                           {"refresh_token", cached->refresh_token}};
    if (!cfg.scope.empty()) f.emplace_back("scope", cfg.scope);
    got = token_request(call, std::move(f), t, err);
    if (got) {
      if (t.refresh_token.empty()) t.refresh_token = cached->refresh_token;  // RFC 6749 6: may be kept
      res.refreshed = true;
    }
  }
  if (!got) {
    switch (cfg.grant) {
      case OAuth2Config::Grant::ClientCredentials: {
        std::vector<std::pair<std::string, std::string>> f = {{"grant_type", "client_credentials"}};
        if (!cfg.scope.empty()) f.emplace_back("scope", cfg.scope);
        got = token_request(call, std::move(f), t, err);
        break;
      }
      case OAuth2Config::Grant::Password: {
        std::vector<std::pair<std::string, std::string>> f = {
            {"grant_type", "password"}, {"username", cfg.username}, {"password", cfg.password}};
        if (!cfg.scope.empty()) f.emplace_back("scope", cfg.scope);
        got = token_request(call, std::move(f), t, err);
        break;
      }
      case OAuth2Config::Grant::AuthorizationCode:
        if (!hooks.interactive) {
          if (err.empty())
            err = cached ? "the access token expired and could not be refreshed: authorize again (Get new access token)"
                         : "no access token yet: authorize first (Get new access token opens the browser)";
          else
            err = "the access token expired and refreshing it failed (" + err + "): authorize again";
          break;
        }
        err.clear();
        got = authorization_code(call, t, err);
        break;
    }
  }
  if (!got) {
    res.error = err;
    return done();
  }
  {
    std::lock_guard<std::mutex> lk(c.mu);
    c.tokens[rs.key] = t;
  }
  res.ok = true;
  res.token = std::move(t);
  return done();
}

}  // namespace watt
