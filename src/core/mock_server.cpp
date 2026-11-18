#include "core/mock_server.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

#include "core/model.h"  // Workspace::user_config_dir
#include "core/model_json.h"
#include "core/model_secure.h"

namespace watt::mock {
namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxHead = 64 * 1024;
constexpr size_t kMaxBody = 32u << 20;
constexpr int kMaxConnections = 64;
constexpr uint32_t kMaxDelayMs = 60000;
constexpr const char* kConsentFile = "allowed_mock_bind.json";

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (lower(a[i]) != lower(b[i])) return false;
  return true;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
  return s;
}

int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string url_decode(std::string_view s, bool plus_space) {
  std::string o;
  o.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
      o += static_cast<char>(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
      i += 2;
    } else if (plus_space && s[i] == '+') {
      o += ' ';
    } else {
      o += s[i];
    }
  }
  return o;
}

std::vector<std::string_view> segments(std::string_view p) {
  std::vector<std::string_view> out;
  size_t i = 0;
  while (i < p.size()) {
    if (p[i] == '/') {
      ++i;
      continue;
    }
    const size_t e = p.find('/', i);
    out.push_back(p.substr(i, e == std::string_view::npos ? std::string_view::npos : e - i));
    if (e == std::string_view::npos) break;
    i = e + 1;
  }
  return out;
}

bool wildcard(std::string_view seg) {
  return (seg.size() > 1 && seg[0] == ':') || seg.find("{{") != std::string_view::npos;
}

// Example names come from file names ('_' for ' '): compare them the same way.
bool same_name(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = lower(a[i]), y = lower(b[i]);
    if (x == '_') x = ' ';
    if (y == '_') y = ' ';
    if (x != y) return false;
  }
  return true;
}

uint32_t parse_delay(std::string_view s) {
  s = trim(s);
  if (s.empty() || s.size() > 9) return UINT32_MAX;
  uint32_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return UINT32_MAX;
    v = v * 10 + static_cast<uint32_t>(c - '0');
  }
  return std::min(v, kMaxDelayMs);
}

const char* reason(int code) {
  switch (code) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 422: return "Unprocessable Entity";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return code < 300 ? "OK" : code < 400 ? "Redirect" : code < 500 ? "Client Error" : "Server Error";
  }
}

using JWriter = rapidjson::Writer<rapidjson::StringBuffer>;
void jstr(JWriter& w, std::string_view s) { w.String(s.data(), static_cast<rapidjson::SizeType>(s.size())); }

std::string error_json(const std::string& error, const std::vector<Route>& routes, const Route* only) {
  rapidjson::StringBuffer sb;
  JWriter w(sb);
  w.StartObject();
  w.Key("error");
  jstr(w, error);
  auto examples = [&w](const Route& r) {
    w.Key("examples");
    w.StartArray();
    for (const auto& e : r.examples) jstr(w, e.name);
    w.EndArray();
  };
  if (only) {
    examples(*only);
  } else {
    w.Key("routes");
    w.StartArray();
    for (const auto& r : routes) {
      w.StartObject();
      w.Key("method");
      jstr(w, r.method.empty() ? std::string_view("*") : std::string_view(r.method));
      w.Key("path");
      jstr(w, r.path);
      w.Key("source");
      jstr(w, r.source);
      examples(r);
      w.EndObject();
    }
    w.EndArray();
  }
  w.EndObject();
  return std::string(sb.GetString(), sb.GetSize());
}

std::string norm_address(std::string_view a) {
  std::string s(trim(a));
  for (char& c : s) c = lower(c);
  if (s.size() > 2 && s.front() == '[' && s.back() == ']') s = s.substr(1, s.size() - 2);
  return s;
}

// Numeric IPv4 / IPv6 (or "localhost"): the sockaddr to bind. false when not one.
bool parse_address(const std::string& a, sockaddr_storage& ss, int& len) {
  std::memset(&ss, 0, sizeof ss);
  const std::string s = a == "localhost" ? std::string("127.0.0.1") : a;
  auto* v4 = reinterpret_cast<sockaddr_in*>(&ss);
  if (inet_pton(AF_INET, s.c_str(), &v4->sin_addr) == 1) {
    v4->sin_family = AF_INET;
    len = sizeof(sockaddr_in);
    return true;
  }
  auto* v6 = reinterpret_cast<sockaddr_in6*>(&ss);
  if (inet_pton(AF_INET6, s.c_str(), &v6->sin6_addr) == 1) {
    v6->sin6_family = AF_INET6;
    len = sizeof(sockaddr_in6);
    return true;
  }
  return false;
}

std::mutex& consent_mutex() {
  static std::mutex m;
  return m;
}

std::vector<std::string> read_consent() {
  std::vector<std::string> out;
  const fs::path dir = Workspace::user_config_dir();
  if (dir.empty()) return out;
  std::string text;
  if (!detail::read_file(dir / kConsentFile, text)) return out;
  rapidjson::Document d;
  d.Parse(text.data(), text.size());
  if (d.HasParseError() || !d.IsObject() || !d.HasMember("addresses") || !d["addresses"].IsArray()) return out;
  for (const auto& v : d["addresses"].GetArray())
    if (v.IsString()) out.push_back(norm_address(std::string_view(v.GetString(), v.GetStringLength())));
  return out;
}

bool write_consent(const std::vector<std::string>& list, std::string* err) {
  const fs::path dir = Workspace::user_config_dir();
  if (dir.empty()) {
    if (err) *err = "no per-user config directory (APPDATA / HOME not set): cannot store the bind address";
    return false;
  }
  rapidjson::Document d;
  d.SetObject();
  auto& a = d.GetAllocator();
  d.AddMember("version", 1, a);
  rapidjson::Value arr(rapidjson::kArrayType);
  for (const auto& s : list) arr.PushBack(rapidjson::Value(s.c_str(), static_cast<rapidjson::SizeType>(s.size()), a), a);
  d.AddMember("addresses", arr, a);
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    detail::ensure_dir(dir.parent_path());
    detail::create_private_dir(dir);
  }
  return detail::write_file_atomic(dir / kConsentFile, detail::json_pretty(d), err, true);
}

}  // namespace

// ------------------------------------------------------------------ routes

std::string route_path(std::string_view u) {
  u = trim(u);
  if (const size_t cut = u.find_first_of("?#"); cut != std::string_view::npos) u = u.substr(0, cut);
  const size_t scheme = u.find("://");
  const size_t slash = u.find('/');
  if (scheme != std::string_view::npos && slash > scheme) {
    const size_t s = u.find('/', scheme + 3);
    u = s == std::string_view::npos ? std::string_view{} : u.substr(s);
  } else if (u.substr(0, 2) == "{{") {
    const size_t e = u.find("}}");
    u = e == std::string_view::npos ? std::string_view{} : u.substr(e + 2);
  } else if (!u.empty() && u[0] != '/') {
    u = slash == std::string_view::npos ? std::string_view{} : u.substr(slash);
  }
  std::string out;
  for (const auto seg : segments(u)) {
    out += '/';
    out.append(seg);
  }
  return out.empty() ? std::string("/") : out;
}

int match_score(const Route& r, std::string_view method, std::string_view path) {
  const bool any = r.method.empty() || r.method == "*";
  const bool exact = !any && iequals(r.method, method);
  if (!any && !exact && !(iequals(method, "HEAD") && iequals(r.method, "GET"))) return -1;
  const std::string rp = route_path(r.path);
  const auto rs = segments(rp);
  const std::string p(path.substr(0, path.find_first_of("?#")));
  const auto ps = segments(p);
  if (rs.size() != ps.size()) return -1;
  int literal = 0;
  for (size_t i = 0; i < rs.size(); ++i) {
    if (wildcard(rs[i])) continue;
    if (rs[i] != ps[i] && url_decode(rs[i], false) != url_decode(ps[i], false)) return -1;
    ++literal;
  }
  return literal * 2 + (exact ? 1 : 0);
}

int find_route(const std::vector<Route>& routes, std::string_view method, std::string_view path) {
  int best = -1, best_score = -1;
  for (size_t i = 0; i < routes.size(); ++i) {
    const int s = match_score(routes[i], method, path);
    if (s > best_score) {
      best_score = s;
      best = static_cast<int>(i);
    }
  }
  return best;
}

const Example* pick_example(const Route& r, std::string_view name) {
  if (r.examples.empty()) return nullptr;
  name = trim(name);
  if (name.empty()) {  // the first success example, else the first
    for (const auto& e : r.examples)
      if (e.status >= 200 && e.status < 300) return &e;
    return &r.examples.front();
  }
  for (const auto& e : r.examples)
    if (same_name(e.name, name)) return &e;
  return nullptr;
}

Reply reply_for(const std::vector<Route>& routes, std::string_view method, std::string_view target,
                const std::vector<std::pair<std::string, std::string>>& headers) {
  Reply rep;
  const size_t qm = target.find('?');
  const std::string_view path = target.substr(0, qm);
  std::string want;
  uint32_t delay = UINT32_MAX;
  if (qm != std::string_view::npos) {
    std::string_view q = target.substr(qm + 1);
    while (!q.empty()) {
      const size_t amp = q.find('&');
      const std::string_view kv = q.substr(0, amp);
      q = amp == std::string_view::npos ? std::string_view{} : q.substr(amp + 1);
      const size_t eq = kv.find('=');
      const std::string k = url_decode(kv.substr(0, eq), true);
      const std::string v = eq == std::string_view::npos ? std::string() : url_decode(kv.substr(eq + 1), true);
      if (k == "__example") want = v;
      else if (k == "__delay") delay = parse_delay(v);
    }
  }
  for (const auto& [k, v] : headers) {
    if (want.empty() && iequals(k, "x-mock-example")) want = std::string(trim(v));
    else if (delay == UINT32_MAX && iequals(k, "x-mock-delay")) delay = parse_delay(v);
  }
  rep.headers.emplace_back("Content-Type", "application/json");
  const int i = find_route(routes, method, path);
  if (i < 0) {
    rep.status = 404;
    rep.body = error_json("No mock route matches " + std::string(method) + " " + std::string(path), routes, nullptr);
    return rep;
  }
  const Route& r = routes[static_cast<size_t>(i)];
  rep.route = i;
  rep.headers.emplace_back("X-Mock-Route", (r.method.empty() ? std::string("*") : r.method) + " " + r.path);
  const Example* e = pick_example(r, want);
  if (!e) {
    rep.status = r.examples.empty() ? 501 : 404;
    rep.body = error_json(r.examples.empty() ? "The request behind " + r.method + " " + r.path +
                                                   " has no saved example: send it and choose Save as example"
                                             : "Route " + r.method + " " + r.path + " has no example named \"" + want + "\"",
                          routes, &r);
    return rep;
  }
  rep.headers.clear();
  rep.status = static_cast<int>(e->status > 99 && e->status < 1000 ? e->status : 200);
  for (const auto& [k, v] : e->headers) {
    // Framing is the server's (the example body is stored decoded); CR / LF never reach the wire.
    if (iequals(k, "content-length") || iequals(k, "transfer-encoding") || iequals(k, "connection") ||
        iequals(k, "keep-alive") || iequals(k, "content-encoding") || iequals(k, "x-mock-route") ||
        iequals(k, "x-mock-example") || k.empty() || k.find_first_of("\r\n:") != std::string::npos ||
        v.find_first_of("\r\n") != std::string::npos)
      continue;
    rep.headers.emplace_back(k, v);
  }
  rep.headers.emplace_back("X-Mock-Route", (r.method.empty() ? std::string("*") : r.method) + " " + r.path);
  if (!e->name.empty() && e->name.find_first_of("\r\n") == std::string::npos) rep.headers.emplace_back("X-Mock-Example", e->name);
  rep.body = e->body;
  rep.example = e->name;
  rep.delay_ms = delay != UINT32_MAX ? delay : std::min(e->delay_ms, kMaxDelayMs);
  return rep;
}

// ------------------------------------------------------------------ consent

bool is_loopback_address(std::string_view address) {
  const std::string a = norm_address(address);
  if (a == "localhost" || a == "::1") return true;
  in_addr v4{};
  if (inet_pton(AF_INET, a.c_str(), &v4) == 1) return (ntohl(v4.s_addr) >> 24) == 127;
  return false;
}

bool bind_allowed(const std::string& address) {
  if (is_loopback_address(address)) return true;
  const std::string a = norm_address(address);
  std::lock_guard lk(consent_mutex());
  const auto list = read_consent();
  return std::find(list.begin(), list.end(), a) != list.end();
}

bool allow_bind(const std::string& address, std::string* err) {
  const std::string a = norm_address(address);
  sockaddr_storage ss;
  int len = 0;
  if (!parse_address(a, ss, len)) {
    if (err) *err = "not an IP address: " + a;
    return false;
  }
  if (is_loopback_address(a)) return true;
  std::lock_guard lk(consent_mutex());
  auto list = read_consent();
  if (std::find(list.begin(), list.end(), a) != list.end()) return true;
  list.push_back(a);
  return write_consent(list, err);
}

bool disallow_bind(const std::string& address, std::string* err) {
  const std::string a = norm_address(address);
  std::lock_guard lk(consent_mutex());
  auto list = read_consent();
  const auto n = list.size();
  list.erase(std::remove(list.begin(), list.end(), a), list.end());
  return list.size() == n || write_consent(list, err);
}

// ------------------------------------------------------------------ server

struct MockServer::Impl : std::enable_shared_from_this<MockServer::Impl> {
  mutable std::mutex m;
  std::condition_variable cv;
  std::shared_ptr<const std::vector<Route>> routes = std::make_shared<std::vector<Route>>();
  SOCKET listen_sock = INVALID_SOCKET;
  bool stopping = false;
  bool running = false;
  bool wsa = false;
  int active = 0;
  std::set<SOCKET> clients;
  std::thread acceptor;
  uint16_t port = 0;
  std::string base;
  std::atomic<uint64_t> served{0};
  std::function<void(const LogEntry&)> on_request;

  bool sleep_ms(uint32_t ms) {
    std::unique_lock lk(m);
    return !cv.wait_for(lk, std::chrono::milliseconds(ms), [this] { return stopping; });
  }

  static bool send_all(SOCKET s, const char* p, size_t n) {
    while (n > 0) {
      const int w = ::send(s, p, static_cast<int>(std::min<size_t>(n, 1 << 20)), 0);
      if (w <= 0) return false;
      p += w;
      n -= static_cast<size_t>(w);
    }
    return true;
  }

  static bool fill(SOCKET s, std::string& buf) {
    char tmp[16 * 1024];
    const int r = ::recv(s, tmp, sizeof tmp, 0);
    if (r <= 0) return false;
    buf.append(tmp, static_cast<size_t>(r));
    return true;
  }

  struct Req {
    std::string method, target, version;
    std::vector<std::pair<std::string, std::string>> headers;
    const std::string* header(std::string_view n) const {
      for (const auto& h : headers)
        if (iequals(h.first, n)) return &h.second;
      return nullptr;
    }
  };

  // 0: got a request; 1: connection closed; 2: bad request (answer 400 and close); 3: too big.
  static int read_request(SOCKET s, std::string& buf, Req& req) {
    size_t end;
    while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
      if (buf.size() > kMaxHead) return 3;
      if (!fill(s, buf)) return 1;
    }
    std::string_view head(buf.data(), end);
    const size_t eol = head.find("\r\n");
    const std::string_view rl = head.substr(0, eol);
    const size_t sp1 = rl.find(' ');
    const size_t sp2 = sp1 == std::string_view::npos ? sp1 : rl.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) return 2;
    req = Req{};
    req.method.assign(rl.substr(0, sp1));
    req.target.assign(rl.substr(sp1 + 1, sp2 - sp1 - 1));
    req.version.assign(rl.substr(sp2 + 1));
    for (char& c : req.method) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    if (req.target.empty() || req.target[0] != '/') {
      // absolute-form ("http://host/path"): only the path is matched
      const size_t sc = req.target.find("://");
      const size_t sl = sc == std::string::npos ? std::string::npos : req.target.find('/', sc + 3);
      req.target = sl == std::string::npos ? std::string("/") : req.target.substr(sl);
    }
    std::string_view rest = eol == std::string_view::npos ? std::string_view{} : head.substr(eol + 2);
    while (!rest.empty()) {
      const size_t e = rest.find("\r\n");
      const std::string_view line = rest.substr(0, e);
      rest = e == std::string_view::npos ? std::string_view{} : rest.substr(e + 2);
      const size_t colon = line.find(':');
      if (colon == std::string_view::npos) continue;
      req.headers.emplace_back(std::string(trim(line.substr(0, colon))), std::string(trim(line.substr(colon + 1))));
    }
    buf.erase(0, end + 4);
    if (const std::string* ex = req.header("expect"); ex && iequals(*ex, "100-continue")) {
      static const char kContinue[] = "HTTP/1.1 100 Continue\r\n\r\n";
      if (!send_all(s, kContinue, sizeof kContinue - 1)) return 1;
    }
    // The body is read and dropped: examples do not depend on it.
    if (const std::string* te = req.header("transfer-encoding"); te && iequals(*te, "chunked")) {
      size_t total = 0;
      for (;;) {
        size_t le;
        while ((le = buf.find("\r\n")) == std::string::npos) {
          if (buf.size() > kMaxHead) return 2;
          if (!fill(s, buf)) return 1;
        }
        uint64_t sz = 0;
        size_t digits = 0;
        for (size_t i = 0; i < le && hexval(buf[i]) >= 0 && digits < 12; ++i, ++digits) sz = sz * 16 + static_cast<uint64_t>(hexval(buf[i]));
        if (digits == 0) return 2;
        buf.erase(0, le + 2);
        if (sz == 0) {
          for (;;) {  // trailers up to the blank line
            size_t t;
            while ((t = buf.find("\r\n")) == std::string::npos) {
              if (buf.size() > kMaxHead) return 2;
              if (!fill(s, buf)) return 1;
            }
            buf.erase(0, t + 2);
            if (t == 0) break;
          }
          break;
        }
        total += static_cast<size_t>(sz);
        if (total > kMaxBody) return 3;
        while (buf.size() < sz + 2)
          if (!fill(s, buf)) return 1;
        buf.erase(0, static_cast<size_t>(sz) + 2);
      }
    } else if (const std::string* cl = req.header("content-length")) {
      uint64_t len = 0;
      const std::string_view v = trim(*cl);
      if (v.empty() || v.size() > 15) return 2;
      for (char c : v) {
        if (c < '0' || c > '9') return 2;
        len = len * 10 + static_cast<uint64_t>(c - '0');
      }
      if (len > kMaxBody) return 3;
      while (buf.size() < len)
        if (!fill(s, buf)) return 1;
      buf.erase(0, static_cast<size_t>(len));
    }
    return 0;
  }

  static bool wants_close(const Req& req) {
    const std::string* c = req.header("connection");
    if (req.version == "HTTP/1.0") return !(c && iequals(*c, "keep-alive"));
    return c && iequals(*c, "close");
  }

  static std::string head_block(int code, const std::vector<std::pair<std::string, std::string>>& headers, size_t len,
                                bool close) {
    std::string h = "HTTP/1.1 " + std::to_string(code) + " " + reason(code) + "\r\nServer: watt-mock\r\n";
    for (const auto& [k, v] : headers) h += k + ": " + v + "\r\n";
    if (!((code >= 100 && code < 200) || code == 204 || code == 304)) h += "Content-Length: " + std::to_string(len) + "\r\n";
    h += close ? "Connection: close\r\n\r\n" : "Connection: keep-alive\r\n\r\n";
    return h;
  }

  static void plain_error(SOCKET s, int code) {
    const std::string body = std::string("{\"error\":\"") + reason(code) + "\"}";
    const std::string out = head_block(code, {{"Content-Type", "application/json"}}, body.size(), true) + body;
    send_all(s, out.data(), out.size());
  }

  void serve(SOCKET s) {
    std::string buf;
    for (;;) {
      Req req;
      const int rc = read_request(s, buf, req);
      if (rc == 1) return;
      if (rc == 2 || rc == 3) {
        plain_error(s, rc == 2 ? 400 : 413);
        return;
      }
      const auto t0 = std::chrono::steady_clock::now();
      std::shared_ptr<const std::vector<Route>> rt;
      {
        std::lock_guard lk(m);
        if (stopping) return;
        rt = routes;
      }
      Reply rep = reply_for(*rt, req.method, req.target, req.headers);
      if (rep.delay_ms && !sleep_ms(rep.delay_ms)) return;
      const bool close = wants_close(req);
      const bool no_body = req.method == "HEAD" || (rep.status >= 100 && rep.status < 200) || rep.status == 204 ||
                           rep.status == 304;
      std::string out = head_block(rep.status, rep.headers, rep.body.size(), close);
      bool ok;
      if (!no_body && rep.body.size() > (1u << 16)) {
        ok = send_all(s, out.data(), out.size()) && send_all(s, rep.body.data(), rep.body.size());
      } else {
        if (!no_body) out += rep.body;
        ok = send_all(s, out.data(), out.size());
      }
      ++served;
      if (on_request) {
        LogEntry e;
        e.when = std::chrono::system_clock::now();
        e.method = req.method;
        e.target = req.target;
        if (rep.route >= 0) {
          const Route& r = (*rt)[static_cast<size_t>(rep.route)];
          e.route = (r.method.empty() ? std::string("*") : r.method) + " " + r.path;
        }
        e.example = rep.example;
        e.status = rep.status;
        e.served_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        try {
          on_request(e);
        } catch (...) {
        }
      }
      if (!ok || close) return;
    }
  }

  void accept_loop() {
    for (;;) {
      SOCKET c = ::accept(listen_sock, nullptr, nullptr);
      if (c == INVALID_SOCKET) {
        std::lock_guard lk(m);
        if (stopping) return;
        continue;
      }
      BOOL nodelay = TRUE;
      setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof nodelay);
      DWORD idle = 120000;  // an idle keep-alive connection gives its thread back after 2 min
      setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&idle), sizeof idle);
      {
        std::lock_guard lk(m);
        if (stopping) {
          ::closesocket(c);
          return;
        }
        if (active >= kMaxConnections) {
          plain_error(c, 503);
          ::closesocket(c);
          continue;
        }
        clients.insert(c);
        ++active;
      }
      std::thread([self = shared_from_this(), c] {
        self->serve(c);
        std::lock_guard lk(self->m);
        self->clients.erase(c);
        ::shutdown(c, SD_SEND);
        ::closesocket(c);
        --self->active;
        self->cv.notify_all();
      }).detach();
    }
  }
};

MockServer::MockServer() : impl_(std::make_shared<Impl>()) {}
MockServer::~MockServer() { stop(); }

bool MockServer::start(Options opts, std::vector<Route> routes, std::string* err) {
  stop();
  auto fail = [&](const std::string& why) {
    if (err) *err = why;
    if (impl_->listen_sock != INVALID_SOCKET) {
      ::closesocket(impl_->listen_sock);
      impl_->listen_sock = INVALID_SOCKET;
    }
    return false;
  };
  const std::string addr = norm_address(opts.address.empty() ? std::string("127.0.0.1") : opts.address);
  sockaddr_storage ss;
  int len = 0;
  if (!parse_address(addr, ss, len)) return fail("not an IP address: " + addr);
  if (!bind_allowed(addr))
    return fail("binding the mock server to " + addr +
                " would expose it to the network; allow that address first (it is stored per user)");
  if (!impl_->wsa) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return fail("WSAStartup failed");
    impl_->wsa = true;
  }
  const int family = ss.ss_family;
  if (family == AF_INET) reinterpret_cast<sockaddr_in*>(&ss)->sin_port = htons(opts.port);
  else reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port = htons(opts.port);
  impl_->listen_sock = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
  if (impl_->listen_sock == INVALID_SOCKET) return fail("cannot create a socket");
  BOOL excl = TRUE;
  setsockopt(impl_->listen_sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
  if (::bind(impl_->listen_sock, reinterpret_cast<sockaddr*>(&ss), len) != 0)
    return fail("cannot listen on " + addr + ":" + std::to_string(opts.port) + " (port in use?)");
  if (::listen(impl_->listen_sock, SOMAXCONN) != 0) return fail("listen failed");
  len = sizeof ss;
  getsockname(impl_->listen_sock, reinterpret_cast<sockaddr*>(&ss), &len);
  impl_->port = ntohs(family == AF_INET ? reinterpret_cast<sockaddr_in*>(&ss)->sin_port
                                        : reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port);
  std::string host = addr == "localhost" || addr == "0.0.0.0" ? std::string("127.0.0.1") : addr == "::" ? std::string("::1") : addr;
  if (host.find(':') != std::string::npos) host = "[" + host + "]";
  {
    std::lock_guard lk(impl_->m);
    impl_->routes = std::make_shared<const std::vector<Route>>(std::move(routes));
    impl_->stopping = false;
    impl_->running = true;
    impl_->base = "http://" + host + ":" + std::to_string(impl_->port);
  }
  impl_->on_request = std::move(opts.on_request);
  impl_->acceptor = std::thread([i = impl_.get()] { i->accept_loop(); });
  return true;
}

void MockServer::set_routes(std::vector<Route> routes) {
  auto p = std::make_shared<const std::vector<Route>>(std::move(routes));
  std::lock_guard lk(impl_->m);
  impl_->routes = std::move(p);
}

std::shared_ptr<const std::vector<Route>> MockServer::routes() const {
  std::lock_guard lk(impl_->m);
  return impl_->routes;
}

void MockServer::stop() {
  Impl& i = *impl_;
  {
    std::lock_guard lk(i.m);
    if (!i.running && i.listen_sock == INVALID_SOCKET) {
      if (i.wsa) {
        WSACleanup();
        i.wsa = false;
      }
      return;
    }
    i.stopping = true;
    i.running = false;
    // shutdown() alone does not wake a recv() blocked on another thread on Windows: cancel it.
    for (SOCKET s : i.clients) {
      ::shutdown(s, SD_BOTH);
      CancelIoEx(reinterpret_cast<HANDLE>(s), nullptr);
    }
  }
  i.cv.notify_all();
  if (i.listen_sock != INVALID_SOCKET) {
    ::closesocket(i.listen_sock);
    i.listen_sock = INVALID_SOCKET;
  }
  if (i.acceptor.joinable()) i.acceptor.join();
  {
    std::unique_lock lk(i.m);
    i.cv.wait(lk, [&i] { return i.active == 0; });
    i.base.clear();
  }
  if (i.wsa) {
    WSACleanup();
    i.wsa = false;
  }
}

bool MockServer::running() const {
  std::lock_guard lk(impl_->m);
  return impl_->running;
}

uint16_t MockServer::port() const { return running() ? impl_->port : 0; }

std::string MockServer::base_url() const {
  std::lock_guard lk(impl_->m);
  return impl_->base;
}

uint64_t MockServer::requests_served() const { return impl_->served.load(); }

}  // namespace watt::mock
