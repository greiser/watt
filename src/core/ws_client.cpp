#include "core/ws_client.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#define SECURITY_WIN32
#include <security.h>
#include <schannel.h>

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <deque>
#include <mutex>

#include "core/crypto.h"
#include "core/loopback_connect.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "secur32.lib")

namespace watt {

namespace {

constexpr const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr const char* kUserAgent = "watt/0.1";

std::string lower(std::string_view s) {
  std::string o(s);
  for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return o;
}

bool istarts(std::string_view s, std::string_view p) {
  if (s.size() < p.size()) return false;
  for (size_t i = 0; i < p.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(p[i]))) return false;
  return true;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

bool has_token(std::string_view list, std::string_view tok) {  // "keep-alive, Upgrade" has "upgrade"
  size_t i = 0;
  while (i <= list.size()) {
    const size_t c = list.find(',', i);
    const std::string_view part = trim(list.substr(i, c == std::string_view::npos ? std::string_view::npos : c - i));
    if (part.size() == tok.size() && istarts(part, tok)) return true;
    if (c == std::string_view::npos) break;
    i = c + 1;
  }
  return false;
}

std::string narrow(const wchar_t* w) {
  if (!w) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string s(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

// The current user's static Windows proxy for http / https and its bypass list (curl syntax).
void system_proxy(bool https, std::string& proxy, std::string& bypass) {
  bypass = "localhost,127.0.0.1,::1";
  WINHTTP_CURRENT_USER_IE_PROXY_CONFIG cfg{};
  if (!WinHttpGetIEProxyConfigForCurrentUser(&cfg)) return;
  const std::string list = narrow(cfg.lpszProxy), by = narrow(cfg.lpszProxyBypass);
  if (cfg.lpszProxy) GlobalFree(cfg.lpszProxy);
  if (cfg.lpszProxyBypass) GlobalFree(cfg.lpszProxyBypass);
  if (cfg.lpszAutoConfigUrl) GlobalFree(cfg.lpszAutoConfigUrl);
  auto url = [](const std::string& v) { return v.find("://") == std::string::npos ? "http://" + v : v; };
  size_t i = 0;
  while (i < list.size()) {
    size_t j = list.find_first_of("; \t", i);
    const std::string e = list.substr(i, j == std::string::npos ? std::string::npos : j - i);
    i = j == std::string::npos ? list.size() : j + 1;
    if (e.empty()) continue;
    const size_t eq = e.find('=');
    if (eq == std::string::npos) {
      proxy = url(e);
      continue;
    }
    const std::string k = lower(e.substr(0, eq)), v = e.substr(eq + 1);
    if (!v.empty() && k == (https ? "https" : "http")) proxy = url(v);
  }
  i = 0;
  while (i < by.size()) {
    size_t j = by.find_first_of("; ,\t", i);
    std::string e = by.substr(i, j == std::string::npos ? std::string::npos : j - i);
    i = j == std::string::npos ? by.size() : j + 1;
    if (e.empty() || e == "<local>" || e.find('*') != std::string::npos) {
      if (e.rfind("*.", 0) == 0 && e.find('*', 1) == std::string::npos) e.erase(0, 1);
      else continue;
    }
    bypass += "," + e;
  }
}

std::string redact_proxy(const std::string& p) {
  const size_t s = p.find("://");
  const size_t at = p.rfind('@');
  if (at == std::string::npos) return p;
  return (s == std::string::npos ? std::string() : p.substr(0, s + 3)) + p.substr(at + 1);
}

double us_ms(curl_off_t us) { return static_cast<double>(us) / 1000.0; }

}  // namespace

// ------------------------------------------------------------------ helpers

std::string ws_accept_key(std::string_view key) {
  return crypto::base64(crypto::digest(crypto::Hash::Sha1, std::string(key) + kGuid));
}

std::string ws_http_url(std::string_view url) {
  if (istarts(url, "ws://")) return "http://" + std::string(url.substr(5));
  if (istarts(url, "wss://")) return "https://" + std::string(url.substr(6));
  if (istarts(url, "http://") || istarts(url, "https://")) return std::string(url);
  return {};
}

std::string ws_display_url(std::string_view url) {
  if (istarts(url, "http://")) return "ws://" + std::string(url.substr(7));
  if (istarts(url, "https://")) return "wss://" + std::string(url.substr(8));
  return std::string(url);
}

const char* ws_close_code_name(int code) {
  switch (code) {
    case 1000: return "Normal closure";
    case 1001: return "Going away";
    case 1002: return "Protocol error";
    case 1003: return "Unsupported data";
    case 1005: return "No status received";
    case 1006: return "Abnormal closure";
    case 1007: return "Invalid frame payload data";
    case 1008: return "Policy violation";
    case 1009: return "Message too big";
    case 1010: return "Mandatory extension";
    case 1011: return "Internal error";
    case 1012: return "Service restart";
    case 1013: return "Try again later";
    case 1014: return "Bad gateway";
    case 1015: return "TLS handshake";
    default: return "";
  }
}

std::string ws_encode_frame(int opcode, std::string_view payload, bool fin, const unsigned char* mask) {
  std::string f;
  const uint64_t n = payload.size();
  f.reserve(payload.size() + 14);
  f.push_back(static_cast<char>((fin ? 0x80 : 0) | (opcode & 0x0f)));
  const char m = mask ? static_cast<char>(0x80) : 0;
  if (n < 126) {
    f.push_back(static_cast<char>(m | static_cast<char>(n)));
  } else if (n < 65536) {
    f.push_back(static_cast<char>(m | 126));
    f.push_back(static_cast<char>((n >> 8) & 0xff));
    f.push_back(static_cast<char>(n & 0xff));
  } else {
    f.push_back(static_cast<char>(m | 127));
    for (int i = 7; i >= 0; --i) f.push_back(static_cast<char>((n >> (8 * i)) & 0xff));
  }
  if (!mask) {
    f.append(payload);
    return f;
  }
  f.append(reinterpret_cast<const char*>(mask), 4);
  const size_t at = f.size();
  f.append(payload);
  char* p = f.data() + at;
  for (size_t i = 0; i < payload.size(); ++i) p[i] = static_cast<char>(p[i] ^ mask[i & 3]);
  return f;
}

int WsFrameParser::next(WsFrame* f, std::string* err, uint64_t max_payload) {
  const size_t avail = buf_.size() - pos_;
  if (avail < 2) return 0;
  const auto* p = reinterpret_cast<const unsigned char*>(buf_.data() + pos_);
  const bool fin = (p[0] & 0x80) != 0;
  const int op = p[0] & 0x0f;
  if (p[0] & 0x70) {
    if (err) *err = "frame with reserved bits set (an extension watt did not negotiate)";
    return -1;
  }
  const bool masked = (p[1] & 0x80) != 0;
  uint64_t len = p[1] & 0x7f;
  size_t off = 2;
  if (len == 126) {
    if (avail < 4) return 0;
    len = (uint64_t{p[2]} << 8) | p[3];
    off = 4;
  } else if (len == 127) {
    if (avail < 10) return 0;
    len = 0;
    for (int i = 0; i < 8; ++i) len = (len << 8) | p[2 + i];
    off = 10;
  }
  if (len > max_payload) {
    if (err) *err = "message too big";
    return -1;
  }
  if (op >= 8 && (len > 125 || !fin)) {
    if (err) *err = "invalid control frame";
    return -1;
  }
  unsigned char mk[4] = {0, 0, 0, 0};
  if (masked) {
    if (avail < off + 4) return 0;
    std::memcpy(mk, p + off, 4);
    off += 4;
  }
  if (avail - off < len) return 0;
  f->opcode = op;
  f->fin = fin;
  f->payload.assign(reinterpret_cast<const char*>(p + off), static_cast<size_t>(len));
  if (masked)
    for (size_t i = 0; i < f->payload.size(); ++i) f->payload[i] = static_cast<char>(f->payload[i] ^ mk[i & 3]);
  pos_ += off + static_cast<size_t>(len);
  if (pos_ == buf_.size()) {
    buf_.clear();
    pos_ = 0;
  } else if (pos_ > (1u << 20) && pos_ > buf_.size() / 2) {
    buf_.erase(0, pos_);
    pos_ = 0;
  }
  return 1;
}

// ------------------------------------------------------------------ client

struct WsClient::Impl {
  CURL* easy = nullptr;
  curl_socket_t sock = CURL_SOCKET_BAD;
  WSAEVENT sock_ev = WSA_INVALID_EVENT;
  HANDLE wake = nullptr;
  std::atomic<bool> cancelled{false};
  std::atomic<bool> open{false};
  WsFrameParser parser;
  Clock::time_point open_t{};
  char errbuf[CURL_ERROR_SIZE] = {0};

  std::mutex m;  // guards out_q, close_req
  struct Out {
    std::string data;
    bool binary = false;
  };
  std::deque<Out> out_q;
  bool close_req = false;
  int close_code = 1000;
  std::string close_reason;

  Impl() { wake = CreateEventW(nullptr, FALSE, FALSE, nullptr); }
  ~Impl() {
    if (sock_ev != WSA_INVALID_EVENT) {
      if (sock != CURL_SOCKET_BAD) WSAEventSelect(sock, sock_ev, 0);
      WSACloseEvent(sock_ev);
    }
    if (easy) curl_easy_cleanup(easy);
    if (wake) CloseHandle(wake);
  }

  static int on_progress(void* p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<Impl*>(p)->cancelled.load() ? 1 : 0;
  }

  // Loopback connects bypass libcurl's connect check (~10 ms on many fresh connections; see
  // core/loopback_connect.h), as HttpClient does.
  curl_socket_t preconnected = CURL_SOCKET_BAD;
  static curl_socket_t on_opensocket(void* p, curlsocktype purpose, curl_sockaddr* a) {
    auto* d = static_cast<Impl*>(p);
    d->preconnected = CURL_SOCKET_BAD;
    curl_socket_t s = socket(a->family, a->socktype, a->protocol);
    const auto* sa = reinterpret_cast<const sockaddr*>(&a->addr);
    if (s == CURL_SOCKET_BAD || purpose != CURLSOCKTYPE_IPCXN || a->socktype != SOCK_STREAM ||
        !detail::loopback_sockaddr(sa))
      return s;
    switch (detail::loopback_preconnect(s, sa, static_cast<int>(a->addrlen))) {
      case detail::PreConnect::Connected: d->preconnected = s; return s;
      case detail::PreConnect::Refused:
        closesocket(s);
        WSASetLastError(WSAECONNREFUSED);
        return CURL_SOCKET_BAD;  // curl tries the next address or reports the failure
      case detail::PreConnect::Unknown: break;
    }
    closesocket(s);
    return socket(a->family, a->socktype, a->protocol);  // curl connects this one itself
  }
  static int on_sockopt(void* p, curl_socket_t s, curlsocktype purpose) {
    const auto* d = static_cast<const Impl*>(p);
    return purpose == CURLSOCKTYPE_IPCXN && s != CURL_SOCKET_BAD && s == d->preconnected
               ? CURL_SOCKOPT_ALREADY_CONNECTED
               : CURL_SOCKOPT_OK;
  }

  // Waits until the socket is readable (or writable); false on cancel / timeout / error.
  bool wait(bool write, int ms) {
    WSAPOLLFD pf{};
    pf.fd = sock;
    pf.events = write ? POLLWRNORM : POLLRDNORM;
    const auto until = Clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
      if (cancelled.load()) return false;
      const int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count());
      if (left <= 0) return false;
      const int r = WSAPoll(&pf, 1, std::min(left, 50));
      if (r < 0) return false;
      if (r > 0) return true;
    }
  }

  // Writes all of s (curl_easy_send), reading meanwhile so a peer that writes too never
  // deadlocks against us. False with *err on failure.
  bool write_all(std::string_view s, int timeout_ms, std::string* err) {
    const auto until = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!s.empty()) {
      size_t n = 0;
      const CURLcode rc = curl_easy_send(easy, s.data(), s.size(), &n);
      if (rc == CURLE_OK) {
        s.remove_prefix(n);
        continue;
      }
      if (rc != CURLE_AGAIN) {
        if (err) *err = std::string("send failed: ") + curl_easy_strerror(rc);
        return false;
      }
      if (open.load()) drain(nullptr);
      if (cancelled.load()) {
        if (err) *err = "cancelled";
        return false;
      }
      if (Clock::now() > until) {
        if (err) *err = "send timed out";
        return false;
      }
      WSAPOLLFD pf{};
      pf.fd = sock;
      pf.events = POLLWRNORM;
      WSAPoll(&pf, 1, 20);
    }
    return true;
  }

  // Reads everything available into the parser. 1: read some / nothing, 0: EOF, -1: error.
  int drain(std::string* err, uint64_t* got = nullptr) {
    char buf[64 * 1024];
    for (;;) {
      size_t n = 0;
      const CURLcode rc = curl_easy_recv(easy, buf, sizeof buf, &n);
      if (rc == CURLE_AGAIN) return 1;
      if (rc != CURLE_OK) {
        if (err) *err = std::string("receive failed: ") + curl_easy_strerror(rc);
        return -1;
      }
      if (n == 0) return 0;
      parser.feed(buf, n);
      if (got) *got += n;
    }
  }

  std::string frame(int op, std::string_view payload) {
    unsigned char mask[4];
    crypto::random_bytes(mask, 4);
    return ws_encode_frame(op, payload, true, mask);
  }
};

WsClient::WsClient() : impl_(std::make_unique<Impl>()) {}
WsClient::~WsClient() = default;

bool WsClient::is_open() const { return impl_->open.load(); }

void WsClient::cancel() {
  impl_->cancelled.store(true);
  if (impl_->wake) SetEvent(impl_->wake);
}

bool WsClient::send(std::string data, bool binary, std::string* err) {
  Impl& d = *impl_;
  {
    std::lock_guard lk(d.m);
    if (!d.open.load() || d.close_req) {
      if (err) *err = "The connection is not open.";
      return false;
    }
    d.out_q.push_back({std::move(data), binary});
  }
  SetEvent(d.wake);
  return true;
}

void WsClient::close(int code, std::string reason) {
  Impl& d = *impl_;
  {
    std::lock_guard lk(d.m);
    if (d.close_req) return;
    d.close_req = true;
    d.close_code = code >= 1000 && code <= 4999 && code != 1005 && code != 1006 ? code : 1000;
    if (reason.size() > 123) reason.resize(123);
    d.close_reason = std::move(reason);
  }
  SetEvent(d.wake);
}

WsHandshake WsClient::connect(const HttpRequest& req) {
  Impl& d = *impl_;
  WsHandshake h;
  const auto t0 = Clock::now();
  HttpClient::global_init();
  const std::string url = ws_http_url(req.url);
  h.url = ws_display_url(url.empty() ? req.url : url);
  if (url.empty()) {
    h.error = "Not a WebSocket URL: use ws:// or wss://.";
    return h;
  }
  if (req.auth != HttpRequest::Auth::None || !req.aws_sigv4.empty()) {
    h.error = "Digest, NTLM, Negotiate and AWS SigV4 auth are not supported for WebSocket yet: use Basic, Bearer, "
              "OAuth 2.0 or an API key header.";
    return h;
  }
  if (!req.client_cert.empty() && istarts(url, "https://")) {
    h.error = "Client certificates (mTLS) are not supported for WebSocket yet.";
    return h;
  }
  for (const auto& [k, v] : req.headers)
    if (k.find_first_of("\r\n") != std::string::npos || v.find_first_of("\r\n") != std::string::npos) {
      h.error = "A header contains a line break: not sent.";
      return h;
    }
  // Request target and Host from the URL (curl's parser: the same one that connects).
  std::string target = "/", host;
  {
    CURLU* u = curl_url();
    if (curl_url_set(u, CURLUPART_URL, url.c_str(), 0) != CURLUE_OK) {
      curl_url_cleanup(u);
      h.error = "Invalid URL: " + req.url;
      return h;
    }
    char *path = nullptr, *query = nullptr, *hs = nullptr, *port = nullptr;
    curl_url_get(u, CURLUPART_PATH, &path, 0);
    curl_url_get(u, CURLUPART_QUERY, &query, 0);
    curl_url_get(u, CURLUPART_HOST, &hs, 0);
    curl_url_get(u, CURLUPART_PORT, &port, 0);  // only when given
    if (path && *path) target = path;
    if (query && *query) target += std::string("?") + query;
    if (hs) host = hs;
    if (port) {
      const bool https = istarts(url, "https://");
      const std::string p = port;
      if (!((https && p == "443") || (!https && p == "80"))) host += ":" + p;
    }
    curl_free(path);
    curl_free(query);
    curl_free(hs);
    curl_free(port);
    curl_url_cleanup(u);
  }
  if (host.empty()) {
    h.error = "Invalid URL (no host): " + req.url;
    return h;
  }
  CURL* e = d.easy = curl_easy_init();
  if (!e) {
    h.error = "libcurl could not start";
    return h;
  }
  const long timeout = req.timeout_ms > 0 ? req.timeout_ms : 30000;
  curl_easy_setopt(e, CURLOPT_URL, url.c_str());
  curl_easy_setopt(e, CURLOPT_CONNECT_ONLY, 1L);
  curl_easy_setopt(e, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(e, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));  // ALPN: http/1.1 only
  curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(e, CURLOPT_ERRORBUFFER, d.errbuf);
  curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT_MS, timeout);
  curl_easy_setopt(e, CURLOPT_TCP_NODELAY, 1L);
  curl_easy_setopt(e, CURLOPT_OPENSOCKETFUNCTION, &Impl::on_opensocket);
  curl_easy_setopt(e, CURLOPT_OPENSOCKETDATA, &d);
  curl_easy_setopt(e, CURLOPT_SOCKOPTFUNCTION, &Impl::on_sockopt);
  curl_easy_setopt(e, CURLOPT_SOCKOPTDATA, &d);
  curl_easy_setopt(e, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(e, CURLOPT_XFERINFOFUNCTION, &Impl::on_progress);
  curl_easy_setopt(e, CURLOPT_XFERINFODATA, &d);
  curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, req.verify_tls ? 1L : 0L);
  curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, req.verify_tls ? 2L : 0L);
  curl_blob ca{};
  if (!req.ca_pem.empty()) {
    ca.data = const_cast<char*>(req.ca_pem.data());
    ca.len = req.ca_pem.size();
    ca.flags = CURL_BLOB_COPY;
    curl_easy_setopt(e, CURLOPT_CAINFO_BLOB, &ca);
    curl_easy_setopt(e, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NO_REVOKE));
  } else {
    curl_easy_setopt(e, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
  }
  long tls = CURL_SSLVERSION_DEFAULT;
  switch (req.tls_min_version) {
    case TlsVersion::Tls1_0: tls = CURL_SSLVERSION_TLSv1_0; break;
    case TlsVersion::Tls1_1: tls = CURL_SSLVERSION_TLSv1_1; break;
    case TlsVersion::Tls1_2: tls = CURL_SSLVERSION_TLSv1_2; break;
    case TlsVersion::Tls1_3: tls = CURL_SSLVERSION_TLSv1_3; break;
    default: break;
  }
  curl_easy_setopt(e, CURLOPT_SSLVERSION, tls);
  std::string px, np;
  switch (req.proxy) {
    case HttpRequest::Proxy::Env: break;  // libcurl reads http_proxy / https_proxy / no_proxy
    case HttpRequest::Proxy::Off: curl_easy_setopt(e, CURLOPT_PROXY, ""); break;
    case HttpRequest::Proxy::Url:
      px = req.proxy_url;
      np = req.no_proxy;
      break;
    case HttpRequest::Proxy::System:
      system_proxy(istarts(url, "https://"), px, np);
      if (!req.no_proxy.empty()) np += "," + req.no_proxy;
      if (px.empty()) curl_easy_setopt(e, CURLOPT_PROXY, "");
      break;
  }
  if (!px.empty()) {
    curl_easy_setopt(e, CURLOPT_PROXY, px.c_str());
    if (!np.empty()) curl_easy_setopt(e, CURLOPT_NOPROXY, np.c_str());
  }
  // Through an HTTP proxy the upgrade must go through a CONNECT tunnel, also for ws://.
  curl_easy_setopt(e, CURLOPT_HTTPPROXYTUNNEL, 1L);

  const auto t_perform = Clock::now();
  h.prepare_ms = ms_between(t0, t_perform);
  const CURLcode rc = curl_easy_perform(e);
  curl_off_t dns = 0, con = 0, app = 0;
  curl_easy_getinfo(e, CURLINFO_NAMELOOKUP_TIME_T, &dns);
  curl_easy_getinfo(e, CURLINFO_CONNECT_TIME_T, &con);
  curl_easy_getinfo(e, CURLINFO_APPCONNECT_TIME_T, &app);
  h.dns_ms = us_ms(dns);
  h.connect_ms = con > dns ? us_ms(con - dns) : 0;
  h.tls_ms = app > con ? us_ms(app - con) : 0;
  char* ip = nullptr;
  long port = 0;
  if (curl_easy_getinfo(e, CURLINFO_PRIMARY_IP, &ip) == CURLE_OK && ip) h.remote_ip = ip;
  curl_easy_getinfo(e, CURLINFO_PRIMARY_PORT, &port);
  h.remote_port = port;
  ip = nullptr;
  if (curl_easy_getinfo(e, CURLINFO_LOCAL_IP, &ip) == CURLE_OK && ip) h.local_ip = ip;
  port = 0;
  curl_easy_getinfo(e, CURLINFO_LOCAL_PORT, &port);
  h.local_port = port;
  long used_proxy = 0;
  if (curl_easy_getinfo(e, CURLINFO_USED_PROXY, &used_proxy) == CURLE_OK && used_proxy) h.proxy = redact_proxy(px.empty() ? "(environment proxy)" : px);
  const auto t_conn = Clock::now();
  if (rc != CURLE_OK) {
    h.error = d.cancelled.load() ? "cancelled" : d.errbuf[0] ? std::string(d.errbuf) : std::string(curl_easy_strerror(rc));
    h.total_ms = ms_between(t0, t_conn);
    return h;
  }
  curl_easy_getinfo(e, CURLINFO_ACTIVESOCKET, &d.sock);
  if (d.sock == CURL_SOCKET_BAD) {
    h.error = "no connection";
    return h;
  }
  h.tls = istarts(url, "https://");
  if (h.tls) {
    curl_tlssessioninfo* ti = nullptr;
    if (curl_easy_getinfo(e, CURLINFO_TLS_SSL_PTR, &ti) == CURLE_OK && ti && ti->backend == CURLSSLBACKEND_SCHANNEL &&
        ti->internals) {
      SecPkgContext_ConnectionInfo ci{};
      if (QueryContextAttributesW(static_cast<PCtxtHandle>(ti->internals), SECPKG_ATTR_CONNECTION_INFO, &ci) == SEC_E_OK) {
        if (ci.dwProtocol & SP_PROT_TLS1_3_CLIENT) h.tls_version = "TLSv1.3";
        else if (ci.dwProtocol & SP_PROT_TLS1_2_CLIENT) h.tls_version = "TLSv1.2";
        else if (ci.dwProtocol & SP_PROT_TLS1_1_CLIENT) h.tls_version = "TLSv1.1";
        else if (ci.dwProtocol & SP_PROT_TLS1_0_CLIENT) h.tls_version = "TLSv1.0";
      }
    }
  }
  // The upgrade request.
  const std::string key = crypto::base64(crypto::random_bytes(16));
  std::string head = "GET " + target + " HTTP/1.1\r\n";
  bool own_host = false, own_ua = false;
  for (const auto& [k, v] : req.headers) {
    const std::string l = lower(k);
    own_host = own_host || l == "host";
    own_ua = own_ua || l == "user-agent";
  }
  if (!own_host) head += "Host: " + host + "\r\n";
  if (!own_ua) head += std::string("User-Agent: ") + kUserAgent + "\r\n";
  head += "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n";
  for (const auto& [k, v] : req.headers) {
    const std::string l = lower(k);
    if (k.empty() || l == "upgrade" || l == "connection" || l == "sec-websocket-key" || l == "sec-websocket-version" ||
        l == "content-length" || l == "transfer-encoding")
      continue;
    head += k + ": " + v + "\r\n";
  }
  head += "\r\n";
  h.request_head = head;
  const int left_ms = std::max<int>(1000, static_cast<int>(timeout - static_cast<long>(ms_between(t0, t_conn))));
  std::string err;
  if (!d.write_all(head, left_ms, &err)) {
    h.error = err;
    h.total_ms = ms_between(t0, Clock::now());
    return h;
  }
  h.bytes_sent = head.size();
  const auto t_sent = Clock::now();
  h.send_ms = ms_between(t_conn, t_sent);
  // The response head.
  std::string in;
  size_t end = std::string::npos;
  const auto until = t_sent + std::chrono::milliseconds(left_ms);
  char buf[16 * 1024];
  while (end == std::string::npos) {
    size_t n = 0;
    const CURLcode r = curl_easy_recv(e, buf, sizeof buf, &n);
    if (r == CURLE_OK && n > 0) {
      in.append(buf, n);
      end = in.find("\r\n\r\n");
      if (end == std::string::npos && in.size() > 256 * 1024) {
        h.error = "upgrade response head too large";
        return h;
      }
      continue;
    }
    if (r == CURLE_OK && n == 0) {
      h.error = in.empty() ? "the server closed the connection without answering the upgrade request"
                           : "the server closed the connection in the middle of the upgrade response";
      h.total_ms = ms_between(t0, Clock::now());
      return h;
    }
    if (r != CURLE_AGAIN) {
      h.error = std::string("receive failed: ") + curl_easy_strerror(r);
      return h;
    }
    const int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count());
    if (!d.wait(false, std::max(left, 0))) {
      h.error = d.cancelled.load() ? "cancelled" : "timed out waiting for the upgrade response";
      h.total_ms = ms_between(t0, Clock::now());
      return h;
    }
  }
  const auto t_head = Clock::now();
  h.upgrade_ms = ms_between(t_sent, t_head);
  h.bytes_received = end + 4;
  // Status line and headers.
  std::string_view hv(in.data(), end);
  const size_t eol = hv.find("\r\n");
  const std::string_view sl = hv.substr(0, eol);
  const size_t sp = sl.find(' ');
  if (sp == std::string_view::npos || !istarts(sl, "HTTP/")) {
    h.error = "not an HTTP response: " + std::string(sl.substr(0, 80));
    return h;
  }
  h.status = std::strtol(std::string(sl.substr(sp + 1, 3)).c_str(), nullptr, 10);
  const size_t sp2 = sl.find(' ', sp + 1);
  if (sp2 != std::string_view::npos) h.status_text = std::string(trim(sl.substr(sp2 + 1)));
  std::string_view rest = eol == std::string_view::npos ? std::string_view{} : hv.substr(eol + 2);
  std::string up, conn, accept;
  while (!rest.empty()) {
    const size_t e2 = rest.find("\r\n");
    const std::string_view line = rest.substr(0, e2);
    rest = e2 == std::string_view::npos ? std::string_view{} : rest.substr(e2 + 2);
    const size_t c = line.find(':');
    if (c == std::string_view::npos) continue;
    std::string k(trim(line.substr(0, c))), v(trim(line.substr(c + 1)));
    const std::string l = lower(k);
    if (l == "upgrade") up = v;
    else if (l == "connection") conn = v;
    else if (l == "sec-websocket-accept") accept = v;
    else if (l == "sec-websocket-protocol") h.subprotocol = v;
    h.headers.emplace_back(std::move(k), std::move(v));
  }
  std::string after = in.substr(end + 4);
  h.total_ms = ms_between(t0, t_head);
  if (h.status != 101) {
    // Show what the server said (a 401 / 404 page), as far as it already arrived.
    h.body = after.substr(0, 64 * 1024);
    h.error = "the server answered " + std::to_string(h.status) + (h.status_text.empty() ? "" : " " + h.status_text) +
              " instead of 101 Switching Protocols";
    return h;
  }
  if (lower(up) != "websocket" || !has_token(conn, "upgrade")) {
    h.error = "the 101 response does not upgrade to websocket (Upgrade: " + up + ", Connection: " + conn + ")";
    return h;
  }
  if (accept != ws_accept_key(key)) {
    h.error = "the server's Sec-WebSocket-Accept does not match the key sent: not a WebSocket server";
    return h;
  }
  if (!after.empty()) d.parser.feed(after.data(), after.size());
  d.sock_ev = WSACreateEvent();
  if (d.sock_ev == WSA_INVALID_EVENT || WSAEventSelect(d.sock, d.sock_ev, FD_READ | FD_CLOSE) != 0) {
    h.error = "cannot watch the socket";
    return h;
  }
  d.open_t = t_head;
  d.open.store(true);
  h.ok = true;
  return h;
}

WsClose WsClient::run(const BatchFn& on_batch, int batch_ms) {
  Impl& d = *impl_;
  WsClose c;
  if (!d.open.load()) {
    c.error = "not connected";
    return c;
  }
  std::vector<WsMessage> pending;
  auto last_flush = Clock::time_point{};
  auto since = [&](Clock::time_point t) { return ms_between(d.open_t, t); };
  auto flush = [&](bool force) {
    if (pending.empty()) return;
    const auto now = Clock::now();
    if (!force && ms_between(last_flush, now) < batch_ms) return;
    last_flush = now;
    if (on_batch) on_batch(std::move(pending));
    pending = {};
  };
  bool close_sent = false;
  Clock::time_point close_deadline{};
  std::string frag;  // a fragmented message being reassembled
  int frag_op = 0;
  bool done = false;
  auto finish = [&](int code, std::string reason, std::string error) {
    c.code = code;
    c.reason = std::move(reason);
    c.error = std::move(error);
    c.at_ms = since(Clock::now());
    done = true;
  };
  auto send_frame = [&](int op, std::string_view payload) {
    std::string err;
    const std::string f = d.frame(op, payload);
    if (!d.write_all(f, 30000, &err)) {
      finish(1006, {}, err);
      return false;
    }
    c.bytes_out += f.size();
    return true;
  };
  while (!done) {
    if (d.cancelled.load()) {
      finish(1006, {}, "cancelled");
      break;
    }
    // Incoming.
    std::string err;
    const int dr = d.drain(&err, &c.bytes_in);
    WsFrame f;
    int pr = 0;
    while (!done && (pr = d.parser.next(&f, &err)) == 1) {
      switch (f.opcode) {
        case 0x9:  // ping
          if (!close_sent) send_frame(0xA, f.payload);
          break;
        case 0xA: break;  // pong
        case 0x8: {       // close
          int code = 1005;
          std::string reason;
          if (f.payload.size() >= 2) {
            code = (static_cast<unsigned char>(f.payload[0]) << 8) | static_cast<unsigned char>(f.payload[1]);
            reason = f.payload.substr(2);
          }
          if (!close_sent) {
            const std::string echo = f.payload.size() >= 2 ? f.payload.substr(0, 2) : std::string();
            std::string e2;
            const std::string fr = d.frame(0x8, echo);
            if (d.write_all(fr, 2000, &e2)) c.bytes_out += fr.size();
          }
          c.by_client = close_sent;
          finish(code, std::move(reason), {});
          break;
        }
        case 0x1:
        case 0x2:
        case 0x0: {
          if (f.opcode != 0) {
            frag_op = f.opcode;
            frag.clear();
          } else if (frag_op == 0) {
            finish(1002, {}, "continuation frame without a message");
            break;
          }
          if (frag.size() + f.payload.size() > WsClient::kMaxMessage) {
            send_frame(0x8, std::string("\x03\xF1", 2));  // 1009
            finish(1009, "message too big", "a message over 256 MB");
            break;
          }
          if (frag.empty() && f.fin) frag = std::move(f.payload);
          else frag += f.payload;
          if (!f.fin) break;
          WsMessage m;
          m.binary = frag_op == 0x2;
          m.data = std::move(frag);
          m.at_ms = since(Clock::now());
          frag.clear();
          frag_op = 0;
          ++c.messages_in;
          pending.push_back(std::move(m));
          break;
        }
        default:
          finish(1002, {}, "unknown opcode " + std::to_string(f.opcode));
          break;
      }
    }
    if (done) break;
    if (pr < 0) {
      const bool big = err == "message too big";
      send_frame(0x8, big ? std::string("\x03\xF1", 2) : std::string("\x03\xEA", 2));
      finish(big ? 1009 : 1002, {}, err);
      break;
    }
    if (dr == 0) {  // EOF
      if (close_sent) finish(1006, {}, "the server closed the connection without answering the close");
      else finish(1006, {}, "the server closed the connection without a close frame");
      break;
    }
    if (dr < 0) {
      finish(1006, {}, err);
      break;
    }
    // Outgoing.
    for (;;) {
      Impl::Out o;
      bool want_close = false;
      int code = 1000;
      std::string reason;
      {
        std::lock_guard lk(d.m);
        if (!d.out_q.empty()) {
          o = std::move(d.out_q.front());
          d.out_q.pop_front();
        } else if (d.close_req && !close_sent) {
          want_close = true;
          code = d.close_code;
          reason = d.close_reason;
        } else {
          break;
        }
      }
      if (want_close) {
        std::string p;
        p.push_back(static_cast<char>((code >> 8) & 0xff));
        p.push_back(static_cast<char>(code & 0xff));
        p += reason;
        if (!send_frame(0x8, p)) break;
        close_sent = true;
        close_deadline = Clock::now() + std::chrono::seconds(3);
        c.code = code;
        c.reason = reason;
        break;
      }
      if (close_sent) continue;
      if (!send_frame(o.binary ? 0x2 : 0x1, o.data)) break;
      WsMessage m;
      m.out = true;
      m.binary = o.binary;
      m.data = std::move(o.data);
      m.at_ms = since(Clock::now());
      ++c.messages_out;
      pending.push_back(std::move(m));
    }
    if (done) break;
    if (close_sent && Clock::now() > close_deadline) {
      c.by_client = true;
      finish(c.code, c.reason, "the server did not answer the close within 3 s");
      break;
    }
    flush(false);
    // Wait for the socket, a queued message / close / cancel, or the next batch.
    int wait_ms = 1000;
    if (!pending.empty())
      wait_ms = std::max(1, batch_ms - static_cast<int>(ms_between(last_flush, Clock::now())));
    if (close_sent)
      wait_ms = std::min<int>(wait_ms, std::max<int>(1, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                            close_deadline - Clock::now()).count())));
    // drain() read until EAGAIN, so nothing waits inside libcurl: the socket event is enough.
    HANDLE hs[2] = {d.sock_ev, d.wake};
    WaitForMultipleObjects(2, hs, FALSE, static_cast<DWORD>(wait_ms));
    WSANETWORKEVENTS ne{};
    WSAEnumNetworkEvents(d.sock, d.sock_ev, &ne);  // resets the event
  }
  d.open.store(false);
  {
    std::lock_guard lk(d.m);
    d.out_q.clear();
  }
  flush(true);
  return c;
}

}  // namespace watt
