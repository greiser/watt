#pragma once
// watt_httpserver: tiny dependency-free winsock HTTP/1.1 keep-alive server for local tests and
// benchmarks. Header-only so tests can embed it on an ephemeral port without spawning a process.
//
// Endpoints (any method unless noted; HEAD omits the body):
//   /echo                 -> {"method","path","query","version","headers":{..},"body":"..."}
//   /bytes/<n>            -> n bytes of application/octet-stream ('a'..'z' repeating)
//   /delay/<ms>           -> waits ms, then {"delay_ms":ms}
//   /status/<code>        -> that status; 3xx carry Location (?location=... or /echo)
//   /redirect/<n>         -> 302 chain of n hops ending at /echo
//   /json/<n>             -> JSON array of n objects
//   /response-headers?k=v -> echoes each query pair as a response header
//   /drip/<n>?interval=ms -> n body bytes, one every `interval` ms (Content-Length known)
//   /gzip/<n>             -> n bytes of 'x', sent with Content-Encoding: gzip (wire << n)
//   /close                -> responds then closes the connection
//   /ws                   -> WebSocket echo (?burst=n, ?every=ms&count=n, ?protocol=p; a text
//                            "close [code reason]" closes), see websocket()
// Authentication (watt's auth tests; credentials are test fixtures):
//   /digest-auth/<user>/<pass> -> RFC 7616 Digest (MD5, qop=auth): 401 + challenge, then
//                                {"authenticated":true,"user":...} when the response checks out
//   /ntlm-probe, /negotiate-probe -> 401 WWW-Authenticate: NTLM / Negotiate until a request carries
//                                "Authorization: NTLM|Negotiate <token>"; then 200 {"scheme","token"}
//                                (the handshake is not completed: proves the client started it)
//   /oauth/token          -> OAuth 2.0 token endpoint (form POST). Client "watt-client" /
//                            "s3cret" (Basic header or client_id / client_secret fields). Grants:
//                            client_credentials, password (alice / wonderland), authorization_code
//                            (with PKCE S256 when the code was issued with a challenge),
//                            refresh_token. ?expires_in=<s> sets the lifetime (default 3600).
//   /oauth/authorize      -> 302 to redirect_uri?code=...&state=... (no login page)
//   /oauth/protected      -> 200 {"ok":true,...} with a live token (Bearer header or
//                            ?access_token=), else 401
//   /oauth/stats          -> {"token_requests":n,"grants":{...}}
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace watt::httpserver {

namespace detail {

inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

inline bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (lower(a[i]) != lower(b[i])) return false;
  return true;
}

inline std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

inline bool parse_u64(std::string_view s, uint64_t& out) {
  if (s.empty() || s.size() > 19) return false;
  uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<uint64_t>(c - '0');
  }
  out = v;
  return true;
}

inline int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

inline std::string url_decode(std::string_view s) {
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

inline std::vector<std::pair<std::string, std::string>> parse_query(std::string_view q) {
  std::vector<std::pair<std::string, std::string>> out;
  while (!q.empty()) {
    size_t amp = q.find('&');
    std::string_view part = q.substr(0, amp);
    q = amp == std::string_view::npos ? std::string_view{} : q.substr(amp + 1);
    if (part.empty()) continue;
    size_t eq = part.find('=');
    if (eq == std::string_view::npos)
      out.emplace_back(url_decode(part), std::string{});
    else
      out.emplace_back(url_decode(part.substr(0, eq)), url_decode(part.substr(eq + 1)));
  }
  return out;
}

inline void json_escape(std::string& out, std::string_view s) {
  static const char* hex = "0123456789abcdef";
  out.push_back('"');
  for (char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          out += "\\u00";
          out.push_back(hex[c >> 4]);
          out.push_back(hex[c & 15]);
        } else {
          out.push_back(ch);
        }
    }
  }
  out.push_back('"');
}

inline const char* reason(int code) {
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
    case 418: return "I'm a teapot";
    case 422: return "Unprocessable Entity";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "Status";
  }
}

// 26 * 2520 bytes: a whole number of alphabets, so consecutive chunks keep the pattern.
constexpr size_t kPatternLen = 26 * 2520;
inline const char* pattern() {
  static const std::string p = [] {
    std::string s(kPatternLen, '\0');
    for (size_t i = 0; i < kPatternLen; ++i) s[i] = static_cast<char>('a' + i % 26);
    return s;
  }();
  return p.data();
}

// gzip member holding n bytes of 'x': one fixed-Huffman deflate block of a literal followed by
// <length 258, distance 1> back-references (13 bits per 258 bytes), so the wire size is a tiny
// fraction of the decoded size, as with real compressed responses. No zlib needed.
inline std::string gzip_x(uint64_t n) {
  std::string out("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff", 10);
  uint32_t acc = 0;
  int nbits = 0;
  auto put = [&](uint32_t v, int len) {  // LSB-first fields
    acc |= v << nbits;
    nbits += len;
    while (nbits >= 8) {
      out.push_back(static_cast<char>(acc & 0xff));
      acc >>= 8;
      nbits -= 8;
    }
  };
  auto huff = [&](uint32_t code, int len) {  // Huffman codes go MSB-first
    uint32_t r = 0;
    for (int i = 0; i < len; ++i) r |= ((code >> i) & 1u) << (len - 1 - i);
    put(r, len);
  };
  put(1, 1);  // BFINAL
  put(1, 2);  // BTYPE = fixed Huffman
  uint64_t left = n;
  const uint32_t lit_x = 0x30 + 'x';  // literals 0..143: 8-bit codes from 0x30
  if (left > 0) {
    huff(lit_x, 8);
    --left;
  }
  for (; left >= 258; left -= 258) {
    huff(0xC5, 8);  // length code 285 (= 258), no extra bits
    huff(0, 5);     // distance code 0 (= 1), no extra bits
  }
  for (; left > 0; --left) huff(lit_x, 8);
  huff(0, 7);  // end of block (256)
  if (nbits > 0) put(0, 8 - nbits);
  // CRC-32 of the decoded data, then its size mod 2^32 (both little-endian).
  uint32_t table[256];
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    table[i] = c;
  }
  uint32_t crc = 0xFFFFFFFFu;
  for (uint64_t i = 0; i < n; ++i) crc = table[(crc ^ 'x') & 0xff] ^ (crc >> 8);
  crc ^= 0xFFFFFFFFu;
  const auto isize = static_cast<uint32_t>(n);
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((crc >> (8 * i)) & 0xff));
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((isize >> (8 * i)) & 0xff));
  return out;
}

// Digest helpers (BCrypt) for the Digest and PKCE checks.
inline std::string bcrypt_digest(const wchar_t* alg, std::string_view data) {
  BCRYPT_ALG_HANDLE a = nullptr;
  std::string out;
  if (BCryptOpenAlgorithmProvider(&a, alg, nullptr, 0) != 0) return out;
  DWORD len = 0, got = 0;
  BCryptGetProperty(a, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&len), sizeof(len), &got, 0);
  out.resize(len);
  static UCHAR empty = 0;
  BCryptHash(a, nullptr, 0, data.empty() ? &empty : reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
             static_cast<ULONG>(data.size()), reinterpret_cast<PUCHAR>(out.data()), len);
  BCryptCloseAlgorithmProvider(a, 0);
  return out;
}
inline std::string md5_hex(std::string_view s) {
  static const char* hx = "0123456789abcdef";
  std::string d = bcrypt_digest(BCRYPT_MD5_ALGORITHM, s), out;
  for (unsigned char c : d) {
    out.push_back(hx[c >> 4]);
    out.push_back(hx[c & 15]);
  }
  return out;
}
inline std::string b64(std::string_view in, bool url) {
  const char* t = url ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
                      : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  const auto* p = reinterpret_cast<const unsigned char*>(in.data());
  for (; i + 2 < in.size(); i += 3) {
    const uint32_t v = (uint32_t{p[i]} << 16) | (uint32_t{p[i + 1]} << 8) | p[i + 2];
    for (int k = 18; k >= 0; k -= 6) out.push_back(t[(v >> k) & 63]);
  }
  if (i + 1 == in.size()) {
    const uint32_t v = uint32_t{p[i]} << 16;
    out.push_back(t[(v >> 18) & 63]);
    out.push_back(t[(v >> 12) & 63]);
    if (!url) out += "==";
  } else if (i + 2 == in.size()) {
    const uint32_t v = (uint32_t{p[i]} << 16) | (uint32_t{p[i + 1]} << 8);
    out.push_back(t[(v >> 18) & 63]);
    out.push_back(t[(v >> 12) & 63]);
    out.push_back(t[(v >> 6) & 63]);
    if (!url) out.push_back('=');
  }
  return out;
}
inline std::string unb64(std::string_view in) {
  std::string out;
  uint32_t acc = 0;
  int bits = 0;
  for (char c : in) {
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+' || c == '-') v = 62;
    else if (c == '/' || c == '_') v = 63;
    else continue;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((acc >> bits) & 0xFF));
    }
  }
  return out;
}
// key="value" / key=token pairs of an auth header value ("a=b, c=\"d\"").
inline std::map<std::string, std::string> auth_params(std::string_view s) {
  std::map<std::string, std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == ',')) ++i;
    const size_t eq = s.find('=', i);
    if (eq == std::string_view::npos) break;
    std::string k(trim(s.substr(i, eq - i)));
    for (auto& c : k) c = lower(c);
    i = eq + 1;
    std::string v;
    if (i < s.size() && s[i] == '"') {
      ++i;
      while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) ++i;
        v.push_back(s[i++]);
      }
      ++i;
    } else {
      while (i < s.size() && s[i] != ',') v.push_back(s[i++]);
      v = std::string(trim(v));
    }
    out[k] = v;
  }
  return out;
}

struct Request {
  std::string method, target, path, query, version;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  const std::string* header(std::string_view name) const {
    for (const auto& h : headers)
      if (iequals(h.first, name)) return &h.second;
    return nullptr;
  }
};

}  // namespace detail

class Server {
 public:
  Server() = default;
  ~Server() { stop(); }
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // port 0 picks an ephemeral port; port() returns the bound one. Binds 127.0.0.1 unless
  // `any_interface`.
  bool start(uint16_t port, bool any_interface = false) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    wsa_ = true;
    listen_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_ == INVALID_SOCKET) return false;
    BOOL excl = TRUE;
    setsockopt(listen_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl),
               sizeof(excl));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(any_interface ? INADDR_ANY : INADDR_LOOPBACK);
    if (::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
    if (::listen(listen_, SOMAXCONN) != 0) return false;
    int len = sizeof(addr);
    getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    stopping_ = false;
    acceptor_ = std::thread([this] { accept_loop(); });
    return true;
  }

  uint16_t port() const { return port_; }
  uint64_t connections_accepted() const { return accepted_.load(); }
  uint64_t requests_served() const { return served_.load(); }

  void stop() {
    if (listen_ == INVALID_SOCKET && !acceptor_.joinable()) {
      if (wsa_) { WSACleanup(); wsa_ = false; }
      return;
    }
    {
      std::lock_guard<std::mutex> lk(mu_);
      stopping_ = true;
      for (SOCKET s : clients_) ::shutdown(s, SD_BOTH);
    }
    cv_.notify_all();
    if (listen_ != INVALID_SOCKET) {
      ::closesocket(listen_);
      listen_ = INVALID_SOCKET;
    }
    if (acceptor_.joinable()) acceptor_.join();
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [this] { return active_ == 0; });
    }
    if (wsa_) { WSACleanup(); wsa_ = false; }
  }

 private:
  void accept_loop() {
    for (;;) {
      SOCKET c = ::accept(listen_, nullptr, nullptr);
      if (c == INVALID_SOCKET) {
        std::lock_guard<std::mutex> lk(mu_);
        if (stopping_) return;
        continue;
      }
      BOOL nodelay = TRUE;
      setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay),
                 sizeof(nodelay));
      {
        std::lock_guard<std::mutex> lk(mu_);
        if (stopping_) {
          ::closesocket(c);
          return;
        }
        clients_.insert(c);
        ++active_;
      }
      ++accepted_;
      std::thread([this, c] {
        serve(c);
        std::lock_guard<std::mutex> lk(mu_);
        clients_.erase(c);
        ::closesocket(c);
        --active_;
        cv_.notify_all();
      }).detach();
    }
  }

  // Interruptible sleep; returns false if the server is stopping.
  bool sleep_ms(uint64_t ms) {
    std::unique_lock<std::mutex> lk(mu_);
    return !cv_.wait_for(lk, std::chrono::milliseconds(ms), [this] { return stopping_; });
  }

  static bool send_all(SOCKET s, const char* p, size_t n) {
    while (n > 0) {
      const int chunk = static_cast<int>(std::min<size_t>(n, 1 << 20));
      const int w = ::send(s, p, chunk, 0);
      if (w <= 0) return false;
      p += w;
      n -= static_cast<size_t>(w);
    }
    return true;
  }

  // Reads more bytes into buf; false on EOF/error.
  static bool fill(SOCKET s, std::string& buf) {
    char tmp[64 * 1024];
    const int r = ::recv(s, tmp, sizeof(tmp), 0);
    if (r <= 0) return false;
    buf.append(tmp, static_cast<size_t>(r));
    return true;
  }

  // Parses one request from the connection; false when the connection should close.
  static bool read_request(SOCKET s, std::string& buf, detail::Request& req, bool& bad) {
    bad = false;
    size_t end;
    while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
      if (buf.size() > 1024 * 1024) { bad = true; return false; }
      if (!fill(s, buf)) return false;
    }
    std::string_view head(buf.data(), end);
    size_t eol = head.find("\r\n");
    std::string_view rl = head.substr(0, eol);
    size_t sp1 = rl.find(' ');
    size_t sp2 = sp1 == std::string_view::npos ? sp1 : rl.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) { bad = true; return false; }
    req = detail::Request{};
    req.method.assign(rl.substr(0, sp1));
    req.target.assign(rl.substr(sp1 + 1, sp2 - sp1 - 1));
    req.version.assign(rl.substr(sp2 + 1));
    size_t qm = req.target.find('?');
    req.path = req.target.substr(0, qm);
    if (qm != std::string::npos) req.query = req.target.substr(qm + 1);
    std::string_view rest = eol == std::string_view::npos ? std::string_view{} : head.substr(eol + 2);
    while (!rest.empty()) {
      size_t e = rest.find("\r\n");
      std::string_view line = rest.substr(0, e);
      rest = e == std::string_view::npos ? std::string_view{} : rest.substr(e + 2);
      size_t colon = line.find(':');
      if (colon == std::string_view::npos) continue;
      req.headers.emplace_back(std::string(detail::trim(line.substr(0, colon))),
                               std::string(detail::trim(line.substr(colon + 1))));
    }
    buf.erase(0, end + 4);

    if (const std::string* ex = req.header("expect"); ex && detail::iequals(*ex, "100-continue")) {
      static const char kContinue[] = "HTTP/1.1 100 Continue\r\n\r\n";
      if (!send_all(s, kContinue, sizeof(kContinue) - 1)) return false;
    }

    const std::string* te = req.header("transfer-encoding");
    if (te && detail::iequals(*te, "chunked")) {
      for (;;) {
        size_t le;
        while ((le = buf.find("\r\n")) == std::string::npos)
          if (!fill(s, buf)) return false;
        uint64_t sz = 0;
        for (size_t i = 0; i < le; ++i) {
          int hv = detail::hexval(buf[i]);
          if (hv < 0) break;
          sz = sz * 16 + static_cast<uint64_t>(hv);
        }
        buf.erase(0, le + 2);
        if (sz == 0) {  // skip trailers up to the blank line
          size_t te2;
          while ((te2 = buf.find("\r\n")) == std::string::npos)
            if (!fill(s, buf)) return false;
          while (te2 != 0) {
            buf.erase(0, te2 + 2);
            while ((te2 = buf.find("\r\n")) == std::string::npos)
              if (!fill(s, buf)) return false;
          }
          buf.erase(0, 2);
          break;
        }
        while (buf.size() < sz + 2)
          if (!fill(s, buf)) return false;
        req.body.append(buf, 0, static_cast<size_t>(sz));
        buf.erase(0, static_cast<size_t>(sz) + 2);
      }
    } else if (const std::string* cl = req.header("content-length")) {
      uint64_t len = 0;
      if (!detail::parse_u64(*cl, len)) { bad = true; return false; }
      while (buf.size() < len)
        if (!fill(s, buf)) return false;
      req.body.assign(buf, 0, static_cast<size_t>(len));
      buf.erase(0, static_cast<size_t>(len));
    }
    return true;
  }

  static bool wants_close(const detail::Request& req) {
    const std::string* c = req.header("connection");
    if (req.version == "HTTP/1.0") return !(c && detail::iequals(*c, "keep-alive"));
    return c && detail::iequals(*c, "close");
  }

  static std::string head_block(int code, std::string_view ctype, uint64_t len, bool close,
                                const std::vector<std::pair<std::string, std::string>>& extra) {
    std::string h;
    h.reserve(256);
    h += "HTTP/1.1 ";
    h += std::to_string(code);
    h += ' ';
    h += detail::reason(code);
    h += "\r\nServer: watt_httpserver\r\n";
    for (const auto& [k, v] : extra) {
      h += k;
      h += ": ";
      h += v;
      h += "\r\n";
    }
    const bool no_body = (code >= 100 && code < 200) || code == 204 || code == 304;
    if (!no_body) {
      if (!ctype.empty()) {
        h += "Content-Type: ";
        h += ctype;
        h += "\r\n";
      }
      h += "Content-Length: ";
      h += std::to_string(len);
      h += "\r\n";
    }
    h += close ? "Connection: close\r\n\r\n" : "Connection: keep-alive\r\n\r\n";
    return h;
  }

  bool respond(SOCKET s, const detail::Request& req, int code, std::string_view ctype,
               std::string_view body, bool close,
               const std::vector<std::pair<std::string, std::string>>& extra = {}) {
    const bool no_body = req.method == "HEAD" || (code >= 100 && code < 200) || code == 204 ||
                         code == 304;
    std::string out = head_block(code, ctype, body.size(), close, extra);
    if (!no_body) out.append(body);
    ++served_;
    return send_all(s, out.data(), out.size());
  }

  static bool tail_num(std::string_view path, std::string_view prefix, uint64_t& n) {
    if (path.size() <= prefix.size() || path.compare(0, prefix.size(), prefix) != 0) return false;
    return detail::parse_u64(path.substr(prefix.size()), n);
  }

  // Handles one request; false closes the connection.
  bool handle(SOCKET s, const detail::Request& req) {
    const bool close = wants_close(req);
    const std::string_view path = req.path;
    uint64_t n = 0;

    if (path == "/echo" || path == "/close") {
      std::string j;
      j.reserve(256 + req.body.size() * 2);
      j += "{\"method\":";
      detail::json_escape(j, req.method);
      j += ",\"path\":";
      detail::json_escape(j, req.path);
      j += ",\"query\":";
      detail::json_escape(j, req.query);
      j += ",\"version\":";
      detail::json_escape(j, req.version);
      j += ",\"headers\":{";
      // Duplicate names are joined with ", " (RFC 9110 field combination).
      std::vector<std::pair<std::string, std::string>> merged;
      for (const auto& h : req.headers) {
        auto it = std::find_if(merged.begin(), merged.end(),
                               [&](const auto& m) { return detail::iequals(m.first, h.first); });
        if (it == merged.end())
          merged.push_back(h);
        else
          it->second += ", " + h.second;
      }
      for (size_t i = 0; i < merged.size(); ++i) {
        if (i) j += ',';
        detail::json_escape(j, merged[i].first);
        j += ':';
        detail::json_escape(j, merged[i].second);
      }
      j += "},\"body\":";
      detail::json_escape(j, req.body);
      j += '}';
      const bool c = close || path == "/close";
      return respond(s, req, 200, "application/json", j, c) && !c;
    }

    if (tail_num(path, "/bytes/", n)) {
      std::string h = head_block(200, "application/octet-stream", n, close, {});
      ++served_;
      if (!send_all(s, h.data(), h.size())) return false;
      if (req.method != "HEAD") {
        uint64_t left = n;
        while (left > 0) {
          const size_t chunk = static_cast<size_t>(std::min<uint64_t>(left, detail::kPatternLen));
          if (!send_all(s, detail::pattern(), chunk)) return false;
          left -= chunk;
        }
      }
      return !close;
    }

    if (tail_num(path, "/delay/", n)) {
      if (!sleep_ms(n)) return false;
      std::string body = "{\"delay_ms\":" + std::to_string(n) + "}";
      return respond(s, req, 200, "application/json", body, close) && !close;
    }

    if (tail_num(path, "/wait/", n)) {
      // A precise server wait of n microseconds (busy wait, capped at 1 s): unlike /delay it
      // does not depend on the server's timer resolution, so client wake-up latency shows.
      const auto until = std::chrono::steady_clock::now() +
                         std::chrono::microseconds(std::min<uint64_t>(n, 1000000));
      while (std::chrono::steady_clock::now() < until) {
      }
      std::string body = "{\"wait_us\":" + std::to_string(n) + "}";
      return respond(s, req, 200, "application/json", body, close) && !close;
    }

    if (tail_num(path, "/status/", n)) {
      if (n < 100 || n > 999) return respond(s, req, 400, "text/plain", "bad status", close) && !close;
      const int code = static_cast<int>(n);
      std::vector<std::pair<std::string, std::string>> extra;
      if (code >= 300 && code < 400 && code != 304) {
        std::string loc = "/echo";
        for (auto& [k, v] : detail::parse_query(req.query))
          if (k == "location") loc = v;
        extra.emplace_back("Location", loc);
      }
      std::string body = "{\"status\":" + std::to_string(code) + "}";
      return respond(s, req, code, "application/json", body, close, extra) && !close;
    }

    if (tail_num(path, "/redirect/", n)) {
      std::string loc = n <= 1 ? std::string("/echo") : "/redirect/" + std::to_string(n - 1);
      return respond(s, req, 302, "text/plain", "redirect", close, {{"Location", loc}}) && !close;
    }

    if (tail_num(path, "/json/", n)) {
      std::string j;
      j.reserve(static_cast<size_t>(std::min<uint64_t>(n, 1u << 20)) * 80 + 2);
      j += '[';
      char item[160];
      for (uint64_t i = 0; i < n; ++i) {
        const int len = std::snprintf(
            item, sizeof(item),
            "%s{\"id\":%llu,\"name\":\"item %llu\",\"value\":%.2f,\"active\":%s,\"tags\":[\"a\",\"b\"]}",
            i ? "," : "", static_cast<unsigned long long>(i), static_cast<unsigned long long>(i),
            static_cast<double>(i) * 1.5, (i % 2) ? "false" : "true");
        if (len > 0) j.append(item, static_cast<size_t>(len));
      }
      j += ']';
      return respond(s, req, 200, "application/json", j, close) && !close;
    }

    if (path == "/response-headers") {
      auto extra = detail::parse_query(req.query);
      return respond(s, req, 200, "application/json", "{}", close, extra) && !close;
    }

    if (tail_num(path, "/drip/", n)) {
      uint64_t interval = 100;
      for (auto& [k, v] : detail::parse_query(req.query))
        if (k == "interval") detail::parse_u64(v, interval);
      std::string h = head_block(200, "application/octet-stream", n, close, {});
      ++served_;
      if (!send_all(s, h.data(), h.size())) return false;
      if (req.method != "HEAD") {
        for (uint64_t i = 0; i < n; ++i) {
          if (i && !sleep_ms(interval)) return false;
          if (!send_all(s, detail::pattern() + (i % 26), 1)) return false;
        }
      }
      return !close;
    }

    if (tail_num(path, "/gzip/", n)) {
      if (n > (uint64_t{1} << 30))
        return respond(s, req, 400, "text/plain", "too big", close) && !close;
      return respond(s, req, 200, "text/plain", detail::gzip_x(n), close,
                     {{"Content-Encoding", "gzip"}}) &&
             !close;
    }

    if (path.rfind("/digest-auth/", 0) == 0) return digest_auth(s, req, close) && !close;
    if (path == "/ntlm-probe" || path == "/negotiate-probe") {
      const char* scheme = path == "/ntlm-probe" ? "NTLM" : "Negotiate";
      const std::string* a = req.header("authorization");
      const std::string pre = std::string(scheme) + " ";
      if (a && a->size() > pre.size() && detail::iequals(std::string_view(*a).substr(0, pre.size()), pre)) {
        std::string j = "{\"scheme\":";
        detail::json_escape(j, scheme);
        j += ",\"token\":";
        detail::json_escape(j, a->substr(pre.size()));
        j += "}";
        return respond(s, req, 200, "application/json", j, close) && !close;
      }
      return respond(s, req, 401, "application/json", "{\"error\":\"unauthorized\"}", close,
                     {{"WWW-Authenticate", scheme}}) && !close;
    }
    if (path.rfind("/oauth/", 0) == 0) return oauth(s, req, close) && !close;

    if (path == "/") {
      return respond(s, req, 200, "text/plain", "watt_httpserver\n", close) && !close;
    }
    return respond(s, req, 404, "application/json", "{\"error\":\"not found\"}", close) && !close;
  }

  // ---------------------------------------------------------------- auth endpoints
  bool digest_auth(SOCKET s, const detail::Request& req, bool close) {
    const std::string rest = req.path.substr(13);
    const size_t sl = rest.find('/');
    if (sl == std::string::npos) return respond(s, req, 404, "text/plain", "use /digest-auth/<user>/<pass>", close);
    const std::string user = rest.substr(0, sl), pass = rest.substr(sl + 1);
    const std::string realm = "watt@test";
    const std::string* a = req.header("authorization");
    if (a && a->size() > 7 && detail::iequals(std::string_view(*a).substr(0, 7), "Digest ")) {
      auto p = detail::auth_params(std::string_view(*a).substr(7));
      bool known_nonce;
      {
        std::lock_guard<std::mutex> lk(mu_);
        known_nonce = nonces_.count(p["nonce"]) > 0;
      }
      const std::string ha1 = detail::md5_hex(user + ":" + realm + ":" + pass);
      const std::string ha2 = detail::md5_hex(req.method + ":" + p["uri"]);
      const std::string expect =
          detail::md5_hex(ha1 + ":" + p["nonce"] + ":" + p["nc"] + ":" + p["cnonce"] + ":" + p["qop"] + ":" + ha2);
      if (known_nonce && p["username"] == user && p["realm"] == realm && p["qop"] == "auth" && p["response"] == expect &&
          p["uri"] == req.target) {
        std::string j = "{\"authenticated\":true,\"user\":";
        detail::json_escape(j, user);
        j += "}";
        return respond(s, req, 200, "application/json", j, close);
      }
    }
    char nonce[48];
    std::snprintf(nonce, sizeof(nonce), "%016llx%08x",
                  static_cast<unsigned long long>(GetTickCount64()) * 2654435761ull, static_cast<unsigned>(++nonce_seq_));
    {
      std::lock_guard<std::mutex> lk(mu_);
      nonces_.insert(nonce);
    }
    const std::string chal = "Digest realm=\"" + realm + "\", qop=\"auth\", nonce=\"" + nonce +
                             "\", opaque=\"5ccc069c403ebaf9f0171e9517f40e41\", algorithm=MD5";
    return respond(s, req, 401, "application/json", "{\"authenticated\":false}", close, {{"WWW-Authenticate", chal}});
  }

  bool oauth(SOCKET s, const detail::Request& req, bool close) {
    auto form = detail::parse_query(req.body);
    auto query = detail::parse_query(req.query);
    auto get = [](const std::vector<std::pair<std::string, std::string>>& v, const char* k) {
      for (const auto& [a, b] : v)
        if (a == k) return b;
      return std::string{};
    };
    auto json_error = [&](int code, const char* err, const char* desc) {
      std::string j = std::string("{\"error\":\"") + err + "\",\"error_description\":\"" + desc + "\"}";
      return respond(s, req, code, "application/json", j, close);
    };
    if (req.path == "/oauth/authorize") {
      const std::string redirect = get(query, "redirect_uri"), state = get(query, "state");
      if (get(query, "response_type") != "code" || get(query, "client_id") != "watt-client" || redirect.empty())
        return json_error(400, "invalid_request", "bad authorize request");
      std::string code = "code-" + std::to_string(++token_seq_);
      {
        std::lock_guard<std::mutex> lk(mu_);
        codes_[code] = {get(query, "code_challenge"), get(query, "code_challenge_method"), redirect};
      }
      std::string loc = redirect + (redirect.find('?') == std::string::npos ? "?" : "&") + "code=" + code + "&state=" + state;
      return respond(s, req, 302, "text/plain", "redirect", close, {{"Location", loc}});
    }
    if (req.path == "/oauth/protected") {
      std::string tok;
      if (const std::string* a = req.header("authorization"); a && a->size() > 7 && detail::iequals(a->substr(0, 7), "Bearer "))
        tok = a->substr(7);
      if (tok.empty()) tok = get(query, "access_token");
      bool live = false;
      {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = tokens_.find(tok);
        live = it != tokens_.end() && GetTickCount64() < it->second;
      }
      if (!live) return respond(s, req, 401, "application/json", "{\"ok\":false}", close, {{"WWW-Authenticate", "Bearer"}});
      std::string j = "{\"ok\":true,\"token\":";
      detail::json_escape(j, tok);
      j += "}";
      return respond(s, req, 200, "application/json", j, close);
    }
    if (req.path == "/oauth/stats") {
      std::lock_guard<std::mutex> lk(mu_);
      std::string j = "{\"token_requests\":" + std::to_string(token_requests_) + ",\"grants\":{";
      bool first = true;
      for (const auto& [g, n] : grants_) {
        if (!first) j += ',';
        first = false;
        detail::json_escape(j, g);
        j += ':' + std::to_string(n);
      }
      j += "}}";
      return respond(s, req, 200, "application/json", j, close);
    }
    if (req.path != "/oauth/token") return json_error(404, "not_found", "unknown oauth endpoint");
    if (req.method != "POST") return json_error(405, "invalid_request", "POST only");
    std::string id = get(form, "client_id"), secret = get(form, "client_secret");
    if (const std::string* a = req.header("authorization"); a && a->size() > 6 && detail::iequals(a->substr(0, 6), "Basic ")) {
      const std::string dec = detail::unb64(a->substr(6));
      const size_t c = dec.find(':');
      id = detail::url_decode(dec.substr(0, c));
      secret = c == std::string::npos ? std::string{} : detail::url_decode(dec.substr(c + 1));
    }
    const std::string grant = get(form, "grant_type");
    {
      std::lock_guard<std::mutex> lk(mu_);
      ++token_requests_;
      ++grants_[grant];
    }
    if (id != "watt-client" || secret != "s3cret") return json_error(401, "invalid_client", "bad client credentials");
    bool with_refresh = false;
    if (grant == "client_credentials") {
    } else if (grant == "password") {
      if (get(form, "username") != "alice" || get(form, "password") != "wonderland")
        return json_error(400, "invalid_grant", "bad user credentials");
      with_refresh = true;
    } else if (grant == "authorization_code") {
      Code c;
      {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = codes_.find(get(form, "code"));
        if (it == codes_.end()) return json_error(400, "invalid_grant", "unknown code");
        c = it->second;
        codes_.erase(it);
      }
      if (c.redirect != get(form, "redirect_uri")) return json_error(400, "invalid_grant", "redirect_uri mismatch");
      if (!c.challenge.empty()) {
        const std::string v = get(form, "code_verifier");
        const std::string want =
            c.method == "S256" ? detail::b64(detail::bcrypt_digest(BCRYPT_SHA256_ALGORITHM, v), true) : v;
        if (v.empty() || want != c.challenge) return json_error(400, "invalid_grant", "PKCE verification failed");
      }
      with_refresh = true;
    } else if (grant == "refresh_token") {
      std::lock_guard<std::mutex> lk(mu_);
      if (!refresh_.count(get(form, "refresh_token"))) return json_error(400, "invalid_grant", "unknown refresh token");
    } else {
      return json_error(400, "unsupported_grant_type", "unsupported grant");
    }
    uint64_t expires = 3600;
    detail::parse_u64(get(query, "expires_in"), expires);
    const std::string tok = "tok-" + grant + "-" + std::to_string(++token_seq_);
    std::string rt;
    {
      std::lock_guard<std::mutex> lk(mu_);
      tokens_[tok] = GetTickCount64() + expires * 1000;
      if (with_refresh) {
        rt = "rt-" + std::to_string(++token_seq_);
        refresh_.insert(rt);
      }
    }
    std::string j = "{\"access_token\":\"" + tok + "\",\"token_type\":\"Bearer\",\"expires_in\":" + std::to_string(expires);
    if (!rt.empty()) j += ",\"refresh_token\":\"" + rt + "\"";
    const std::string scope = get(form, "scope");
    if (!scope.empty()) {
      j += ",\"scope\":";
      detail::json_escape(j, scope);
    }
    j += "}";
    return respond(s, req, 200, "application/json", j, close, {{"Cache-Control", "no-store"}});
  }

  struct Code {
    std::string challenge, method, redirect;
  };
  std::set<std::string> nonces_;
  std::atomic<unsigned> nonce_seq_{0};
  std::atomic<uint64_t> token_seq_{0};
  std::map<std::string, Code> codes_;
  std::map<std::string, uint64_t> tokens_;  // token -> expiry (GetTickCount64 ms)
  std::set<std::string> refresh_;
  std::map<std::string, int> grants_;
  int token_requests_ = 0;

  void serve(SOCKET s) {
    std::string buf;
    buf.reserve(16 * 1024);
    detail::Request req;
    for (;;) {
      bool bad = false;
      if (!read_request(s, buf, req, bad)) {
        if (bad) respond(s, req, 400, "text/plain", "bad request", true);
        return;
      }
      if (req.path == "/ws" || req.path.rfind("/ws/", 0) == 0) {
        websocket(s, req, buf);
        return;
      }
      if (!handle(s, req)) return;
    }
  }

  // ---------------------------------------------------------------- WebSocket (RFC 6455)
  // /ws                     echo: every text / binary message comes back as it was sent
  //   ?burst=<n>            right after the upgrade, n text messages {"i":k,"msg":"burst"}
  //   ?every=<ms>&count=<n> then n messages {"tick":k}, one every ms
  //   ?protocol=<p>         answers Sec-WebSocket-Protocol: p
  // A text message "close" makes the server close with 1000 "bye"; "close <code> <reason>"
  // with that code and reason. Pings are answered with pongs.
  static bool ws_frame(SOCKET s, int opcode, std::string_view payload) {
    std::string f;
    f.reserve(payload.size() + 10);
    f.push_back(static_cast<char>(0x80 | opcode));
    if (payload.size() < 126) {
      f.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() < 65536) {
      f.push_back(126);
      f.push_back(static_cast<char>((payload.size() >> 8) & 0xff));
      f.push_back(static_cast<char>(payload.size() & 0xff));
    } else {
      f.push_back(127);
      for (int i = 7; i >= 0; --i) f.push_back(static_cast<char>((static_cast<uint64_t>(payload.size()) >> (8 * i)) & 0xff));
    }
    f.append(payload);
    return send_all(s, f.data(), f.size());
  }
  static bool ws_close(SOCKET s, int code, std::string_view reason) {
    std::string p;
    p.push_back(static_cast<char>((code >> 8) & 0xff));
    p.push_back(static_cast<char>(code & 0xff));
    p.append(reason.substr(0, 120));
    return ws_frame(s, 8, p);
  }
  static uint64_t query_num(std::string_view q, std::string_view key, uint64_t def) {
    size_t i = 0;
    while (i <= q.size()) {
      const size_t amp = q.find('&', i);
      const std::string_view item = q.substr(i, amp == std::string_view::npos ? std::string_view::npos : amp - i);
      if (item.size() > key.size() && item.compare(0, key.size(), key) == 0 && item[key.size()] == '=') {
        uint64_t v = 0;
        if (detail::parse_u64(item.substr(key.size() + 1), v)) return v;
      }
      if (amp == std::string_view::npos) break;
      i = amp + 1;
    }
    return def;
  }
  void websocket(SOCKET s, const detail::Request& req, std::string& buf) {
    const std::string* up = req.header("upgrade");
    const std::string* key = req.header("sec-websocket-key");
    if (!up || !detail::iequals(*up, "websocket") || !key || req.method != "GET") {
      respond(s, req, 426, "text/plain", "WebSocket upgrade required", true, {{"Upgrade", "websocket"}});
      return;
    }
    const std::string accept =
        detail::b64(detail::bcrypt_digest(BCRYPT_SHA1_ALGORITHM, *key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"), false);
    std::string h = "HTTP/1.1 101 Switching Protocols\r\nServer: watt_httpserver\r\nUpgrade: websocket\r\n"
                    "Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n";
    const size_t pq = req.query.find("protocol=");
    if (pq != std::string::npos && (pq == 0 || req.query[pq - 1] == '&')) {
      std::string p = req.query.substr(pq + 9, req.query.find('&', pq) - pq - 9);
      p.erase(std::remove_if(p.begin(), p.end(), [](char c) { return c == '\r' || c == '\n'; }), p.end());
      h += "Sec-WebSocket-Protocol: " + p + "\r\n";
    }
    h += "\r\n";
    ++served_;
    if (!send_all(s, h.data(), h.size())) return;
    const uint64_t burst = std::min<uint64_t>(query_num(req.query, "burst", 0), 1000000);
    for (uint64_t i = 0; i < burst; ++i)
      if (!ws_frame(s, 1, "{\"i\":" + std::to_string(i) + ",\"msg\":\"burst\"}")) return;
    const uint64_t every = query_num(req.query, "every", 0);
    uint64_t ticks = every ? std::min<uint64_t>(query_num(req.query, "count", 10), 100000) : 0, tick = 0;
    auto next_tick = std::chrono::steady_clock::now() + std::chrono::milliseconds(every);
    std::string msg;  // a fragmented message being reassembled
    int msg_op = 0;
    for (;;) {
      // A complete frame in buf?
      while (buf.size() >= 2) {
        const auto b0 = static_cast<unsigned char>(buf[0]), b1 = static_cast<unsigned char>(buf[1]);
        uint64_t len = b1 & 0x7f;
        size_t off = 2;
        if (len == 126) {
          if (buf.size() < 4) break;
          len = (uint64_t{static_cast<unsigned char>(buf[2])} << 8) | static_cast<unsigned char>(buf[3]);
          off = 4;
        } else if (len == 127) {
          if (buf.size() < 10) break;
          len = 0;
          for (int i = 0; i < 8; ++i) len = (len << 8) | static_cast<unsigned char>(buf[2 + i]);
          off = 10;
        }
        const bool masked = (b1 & 0x80) != 0;
        if (len > (64u << 20)) {
          ws_close(s, 1009, "message too big");
          return;
        }
        if (buf.size() < off + (masked ? 4 : 0) + len) break;
        char mk[4] = {0, 0, 0, 0};
        if (masked) {
          std::memcpy(mk, buf.data() + off, 4);
          off += 4;
        }
        std::string payload = buf.substr(off, static_cast<size_t>(len));
        for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= mk[i & 3];
        buf.erase(0, off + static_cast<size_t>(len));
        const int op = b0 & 0x0f;
        const bool fin = (b0 & 0x80) != 0;
        if (op == 8) {  // close: answer with the same code, then hang up
          ws_frame(s, 8, payload.substr(0, 2));
          return;
        }
        if (op == 9) {
          if (!ws_frame(s, 10, payload)) return;
          continue;
        }
        if (op == 10) continue;
        if (op != 0) {
          msg_op = op;
          msg.clear();
        }
        msg += payload;
        if (!fin) continue;
        if (msg_op == 1 && msg.rfind("close", 0) == 0) {
          int code = 1000;
          std::string reason = "bye";
          if (msg.size() > 6) {
            code = std::atoi(msg.c_str() + 6);
            const size_t sp = msg.find(' ', 6);
            reason = sp == std::string::npos ? "" : msg.substr(sp + 1);
            if (code < 1000 || code > 4999) code = 1000;
          }
          ws_close(s, code, reason);
          // Wait (briefly) for the client's close echo.
          fd_set rs;
          FD_ZERO(&rs);
          FD_SET(s, &rs);
          timeval tv{1, 0};
          select(0, &rs, nullptr, nullptr, &tv);
          return;
        }
        ++served_;
        if (!ws_frame(s, msg_op, msg)) return;
        msg.clear();
      }
      // Wait for more input or the next tick.
      int wait_ms = 1000;
      if (tick < ticks) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_tick) {
          if (!ws_frame(s, 1, "{\"tick\":" + std::to_string(tick) + "}")) return;
          ++tick;
          next_tick += std::chrono::milliseconds(every);
          continue;
        }
        wait_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(next_tick - now).count()) + 1;
      }
      fd_set rs;
      FD_ZERO(&rs);
      FD_SET(s, &rs);
      timeval tv{wait_ms / 1000, (wait_ms % 1000) * 1000};
      const int sel = select(0, &rs, nullptr, nullptr, &tv);
      if (sel < 0) return;
      if (sel == 0) {
        std::lock_guard<std::mutex> lk(mu_);
        if (stopping_) return;
        continue;
      }
      if (!fill(s, buf)) return;
    }
  }

  SOCKET listen_ = INVALID_SOCKET;
  uint16_t port_ = 0;
  bool wsa_ = false;
  std::thread acceptor_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::set<SOCKET> clients_;
  bool stopping_ = false;
  int active_ = 0;
  std::atomic<uint64_t> accepted_{0};
  std::atomic<uint64_t> served_{0};
};

}  // namespace watt::httpserver
