#include "core/http_client.h"

// libcurl-backed REST engine.
//
// Design notes (performance first):
//  * One persistent easy handle per client, driven through a private multi handle so that
//    cancel() can wake the transfer loop instantly (curl_multi_wakeup) instead of waiting for
//    curl's ~1s idle progress tick. The atomic flag is also checked in the xferinfo callback.
//  * The easy handle is never curl_easy_reset(): options that never change (share, callbacks,
//    buffers, socket options) are set once when the client is built, and send() sets every
//    per-request option to an explicit value. A reset plus re-applying everything cost about
//    10 us per warm request; an unchanged header list is reused as is.
//  * A share handle holds DNS, TLS session and connection caches, so re-firing a request skips
//    DNS/TCP/TLS entirely.
//  * The response body is appended straight from curl's receive buffer into
//    HttpResponse::body (reserved from Content-Length): no intermediate copies.
//  * Loopback connects never retransmit SYNs (SIO_TCP_INITIAL_RTO). Windows retries a SYN to a
//    refused loopback port instead of failing at once, so "localhost" against an IPv4-only dev
//    server used to sit on ::1 until curl's 200 ms happy-eyeballs timer fired. Real networks
//    keep the normal SYN retry behaviour.
//  * Busy-poll (HttpRequest::busy_poll_ms): for the first 20 ms of a transfer the loop calls
//    curl_multi_perform again after a few pause instructions instead of sleeping in
//    curl_multi_poll. A thread that sleeps through the server wait wakes on an idle,
//    down-clocked core, and that wake-up plus everything after the last byte ran ~3x slower:
//    in-process A/B against a bare libcurl loop showed no option, callback or buffer size
//    made a measurable difference, only that.
//  * libcurl is pinned in the repo (ports/curl, 8.22, linked statically; see build.ps1): 8.22
//    skips the per-reuse connection liveness poll when the connection was checked < 1 s ago.
//  * Timing comes from curl's per-hop microsecond marks plus our own steady_clock marks
//    around the transfer; see the end of send() for how they map to phases.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <timeapi.h>

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <string_view>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

namespace watt {

namespace {

struct CurlGlobal {
  CurlGlobal() {
    // libcurl 8.20 + Schannel: schannel_init() asks "Windows >= 8.1?" (the ALPN gate) before
    // curl's win32 init has loaded RtlVerifyVersionInfo, so it falls back to VerifyVersionInfo,
    // which reports Windows 8 to any exe without a supportedOS manifest. ALPN then stays off for
    // the whole process and HTTP/2 over TLS is never offered. A second init, after a cleanup,
    // re-runs schannel_init with the pointer already loaded (it is never cleared). The second
    // init costs about 0.35 ms, so it is skipped when VerifyVersionInfo already tells the truth
    // (the exe declares Windows 8.1+ in its manifest, as the GUI does): curl's fallback then
    // reaches the same answer on the first init.
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (!version_info_reports_81()) {
      curl_global_cleanup();
      curl_global_init(CURL_GLOBAL_DEFAULT);
    }
  }
  static bool version_info_reports_81() {
    OSVERSIONINFOEXW v{};
    v.dwOSVersionInfoSize = sizeof(v);
    v.dwMajorVersion = 6;
    v.dwMinorVersion = 3;
    DWORDLONG mask = 0;
    mask = VerSetConditionMask(mask, VER_MAJORVERSION, VER_GREATER_EQUAL);
    mask = VerSetConditionMask(mask, VER_MINORVERSION, VER_GREATER_EQUAL);
    return VerifyVersionInfoW(&v, VER_MAJORVERSION | VER_MINORVERSION, mask) != FALSE;
  }
  ~CurlGlobal() { curl_global_cleanup(); }
};

void ensure_global_init() { static CurlGlobal g; }

// Spare multi handles. curl_multi_init costs about 0.5 ms (its wakeup socket pair is a
// loopback listen/connect/accept), which dwarfs a warm request, so HttpClient takes one from
// here and gives it back when destroyed. A multi with no easy handle attached holds no
// connections (those live in each client's share handle), so a recycled one carries no state
// into the next client. Destroyed at exit before CurlGlobal (constructed after it).
class MultiPool {
 public:
  static MultiPool& get() {
    ensure_global_init();
    static MultiPool p;
    return p;
  }
  CURLM* take() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (!spare_.empty()) {
        CURLM* m = spare_.back();
        spare_.pop_back();
        return m;
      }
    }
    return curl_multi_init();
  }
  void give(CURLM* m) {
    if (!m) return;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (spare_.size() < kMax) {
        spare_.push_back(m);
        return;
      }
    }
    curl_multi_cleanup(m);
  }
  void prefill(size_t n) {
    for (;;) {
      {
        std::lock_guard<std::mutex> lk(mu_);
        if (spare_.size() >= n) return;
      }
      CURLM* m = curl_multi_init();  // outside the lock: it is the slow part
      if (!m) return;
      give(m);
    }
  }
  ~MultiPool() {
    for (CURLM* m : spare_) curl_multi_cleanup(m);
  }

 private:
  static constexpr size_t kMax = 8;
  std::mutex mu_;
  std::vector<CURLM*> spare_;
};

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() &&
         (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
    s.remove_suffix(1);
  return s;
}

// Upper bound for reserving from Content-Length, so a lying server cannot make us allocate
// absurd amounts up front. Larger bodies still work; they just grow geometrically.
constexpr size_t kMaxReserve = size_t{256} * 1024 * 1024;

curl_off_t info_off(CURL* h, CURLINFO what) {
  curl_off_t v = 0;
  if (curl_easy_getinfo(h, what, &v) != CURLE_OK) return 0;
  return v < 0 ? 0 : v;
}

curl_off_t info_long(CURL* h, CURLINFO what) {
  long v = 0;
  if (curl_easy_getinfo(h, what, &v) != CURLE_OK) return 0;
  return v < 0 ? 0 : static_cast<curl_off_t>(v);
}

// curl's per-hop time marks (microseconds since the hop started) and its byte / connection
// counters. curl accumulates all of them across redirect hops, so one hop's values are the
// difference of two snapshots.
struct Marks {
  curl_off_t q = 0, n = 0, c = 0, a = 0, p = 0, pt = 0, st = 0;
  curl_off_t req = 0, up = 0, conns = 0;
  // `connect_marks` false skips the DNS / connect / TLS marks: the caller knows that no
  // connection was opened during this send() (they stay 0, as curl reports for a reuse).
  static Marks read(CURL* h, bool connect_marks = true) {
    Marks m;
    m.q = info_off(h, CURLINFO_QUEUE_TIME_T);
    if (connect_marks) {
      m.n = info_off(h, CURLINFO_NAMELOOKUP_TIME_T);
      m.c = info_off(h, CURLINFO_CONNECT_TIME_T);
      m.a = info_off(h, CURLINFO_APPCONNECT_TIME_T);
    }
    m.p = info_off(h, CURLINFO_PRETRANSFER_TIME_T);
    m.pt = info_off(h, CURLINFO_POSTTRANSFER_TIME_T);
    m.st = info_off(h, CURLINFO_STARTTRANSFER_TIME_T);
    m.req = info_long(h, CURLINFO_REQUEST_SIZE);
    m.up = info_off(h, CURLINFO_SIZE_UPLOAD_T);
    m.conns = info_long(h, CURLINFO_NUM_CONNECTS);
    return m;
  }
  Marks minus(const Marks& o) const {
    auto d = [](curl_off_t x, curl_off_t y) { return x > y ? x - y : curl_off_t{0}; };
    Marks r;
    r.q = d(q, o.q);
    r.n = d(n, o.n);
    r.c = d(c, o.c);
    r.a = d(a, o.a);
    r.p = d(p, o.p);
    r.pt = d(pt, o.pt);
    r.st = d(st, o.st);
    r.req = d(req, o.req);
    r.up = d(up, o.up);
    r.conns = d(conns, o.conns);
    return r;
  }
};

struct SocketOpen {
  Clock::time_point at;     // curl asked for the socket
  Clock::time_point ready;  // socket created and configured
  sockaddr_storage addr;
};

// A redirect hop as captured while it happened; turned into a RedirectHop after the transfer.
struct RawHop {
  Marks at_end;              // curl's accumulated marks when the 3xx header block ended
  curl_off_t redirect_end = -1;  // curl's accumulated redirect time once it moved on
  curl_off_t dl_after = 0;   // accumulated wire body bytes once it moved on
  uint64_t header_bytes = 0;
  long status = 0;
  long port = 0;
  std::string url, ip;
};

struct TransferCtx {
  HttpResponse* resp = nullptr;
  const BodyChunkFn* on_chunk = nullptr;
  bool has_chunk = false;
  std::atomic<bool>* cancel = nullptr;
  CURL* easy = nullptr;
  bool keep_body = true;
  bool follow = false;
  bool hop_open = false;      // a followed 3xx ended; the next status line starts a new hop
  long hop_status = 0;
  bool have_location = false;
  uint64_t hop_header_bytes = 0;  // response header bytes of the current hop (incl. 1xx)
  uint64_t hop_body_bytes = 0;    // decoded body bytes of the current hop
  std::vector<RawHop> hops;
  // Response headers go straight into HttpResponse::headers (one text buffer + offsets),
  // reserved from what the previous response needed, so a re-fired request allocates exactly
  // twice for all its headers and nothing is converted after the transfer.
  size_t last_hdr_bytes = 512, last_hdr_count = 16;
  const char* hop_version = "";  // protocol of the latest status line ("HTTP/1.1", "HTTP/2")
  std::array<SocketOpen, 8> opens;  // sockets curl opened during this send(), in order
  size_t n_opens = 0;

  void begin(HttpResponse* r, const BodyChunkFn* chunk, const HttpRequest& req) {
    resp = r;
    on_chunk = chunk;
    has_chunk = chunk && *chunk;
    keep_body = req.keep_body;
    follow = req.follow_redirects;
    hop_open = false;
    hop_status = 0;
    have_location = false;
    hop_header_bytes = 0;
    hop_body_bytes = 0;
    hops.clear();
    hop_version = "";
    n_opens = 0;
    r->headers.reserve(last_hdr_bytes, last_hdr_count);
  }
  // curl has moved on from the last followed 3xx: close that hop.
  void close_hop() {
    RawHop& h = hops.back();
    h.redirect_end = info_off(easy, CURLINFO_REDIRECT_TIME_T);
    h.dl_after = info_off(easy, CURLINFO_SIZE_DOWNLOAD_T);
    hop_open = false;
    hop_header_bytes = 0;
    hop_body_bytes = 0;
  }
};

// Protocol token of a status line as a static string (no allocation when assigned).
const char* version_name(std::string_view v) {
  if (v == "HTTP/1.1") return "HTTP/1.1";
  if (v == "HTTP/2" || v == "HTTP/2.0") return "HTTP/2";
  if (v == "HTTP/1.0") return "HTTP/1.0";
  if (v == "HTTP/3") return "HTTP/3";
  return "";
}

bool is_redirect_code(long s) { return s == 301 || s == 302 || s == 303 || s == 307 || s == 308; }

size_t on_header(char* buf, size_t size, size_t nitems, void* userdata) {
  auto* ctx = static_cast<TransferCtx*>(userdata);
  const size_t n = size * nitems;
  std::string_view line(buf, n);
  HttpResponse& r = *ctx->resp;

  if (line.size() >= 5 && line.compare(0, 5, "HTTP/") == 0) {
    // New response (1xx, redirect hop or final): keep only the latest one.
    if (ctx->hop_open) ctx->close_hop();
    ctx->hop_header_bytes += n;
    r.headers.clear();
    r.body.clear();
    r.status_text.clear();
    ctx->hop_status = 0;
    ctx->have_location = false;
    std::string_view rest = trim(line);
    size_t sp = rest.find(' ');
    if (sp != std::string_view::npos) {
      ctx->hop_version = version_name(rest.substr(0, sp));
      rest.remove_prefix(sp + 1);
      long code = 0;
      for (size_t i = 0; i < rest.size() && i < 3 && rest[i] >= '0' && rest[i] <= '9'; ++i)
        code = code * 10 + (rest[i] - '0');
      ctx->hop_status = code;
      size_t sp2 = rest.find(' ');
      if (sp2 != std::string_view::npos) r.status_text = std::string(trim(rest.substr(sp2 + 1)));
    }
    return n;
  }
  ctx->hop_header_bytes += n;
  std::string_view t = trim(line);
  if (t.empty()) {
    // End of a header block. If curl is about to follow this one, remember where the hop
    // stands: everything after this belongs to the next hop.
    if (ctx->follow && ctx->have_location && is_redirect_code(ctx->hop_status)) {
      RawHop& h = ctx->hops.emplace_back();
      h.at_end = Marks::read(ctx->easy);
      h.header_bytes = ctx->hop_header_bytes;
      h.status = ctx->hop_status;
      char* s = nullptr;
      if (curl_easy_getinfo(ctx->easy, CURLINFO_EFFECTIVE_URL, &s) == CURLE_OK && s) h.url = s;
      s = nullptr;
      if (curl_easy_getinfo(ctx->easy, CURLINFO_PRIMARY_IP, &s) == CURLE_OK && s) h.ip = s;
      curl_easy_getinfo(ctx->easy, CURLINFO_PRIMARY_PORT, &h.port);
      ctx->hop_open = true;
    }
    return n;
  }
  size_t colon = t.find(':');
  if (colon == std::string_view::npos) return n;
  std::string_view name = trim(t.substr(0, colon));
  std::string_view value = trim(t.substr(colon + 1));
  if (iequals(name, "content-length")) {
    size_t len = 0;
    bool valid = !value.empty();
    for (char c : value) {
      if (c < '0' || c > '9') { valid = false; break; }
      len = len * 10 + static_cast<size_t>(c - '0');
      if (len > kMaxReserve) { len = kMaxReserve; break; }
    }
    if (ctx->keep_body && valid && len > r.body.capacity()) r.body.reserve(len);
  } else if (iequals(name, "location") && !value.empty()) {
    ctx->have_location = true;
  }
  r.headers.add(name, value);
  return n;
}

size_t on_write(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* ctx = static_cast<TransferCtx*>(userdata);
  const size_t n = size * nmemb;
  if (ctx->cancel->load(std::memory_order_relaxed)) return CURL_WRITEFUNC_ERROR;
  ctx->hop_body_bytes += n;
  if (ctx->keep_body) ctx->resp->body.append(ptr, n);
  if (ctx->has_chunk) {
    try {
      (*ctx->on_chunk)(ptr, n);
    } catch (...) {  // never let an exception unwind through libcurl's C frames
      return CURL_WRITEFUNC_ERROR;
    }
  }
  return n;
}

bool is_loopback(const sockaddr* sa) {
  if (sa->sa_family == AF_INET) {
    const auto* in = reinterpret_cast<const sockaddr_in*>(sa);
    return (ntohl(in->sin_addr.s_addr) >> 24) == 127;
  }
  if (sa->sa_family == AF_INET6) {
    const auto* in6 = reinterpret_cast<const sockaddr_in6*>(sa);
    const unsigned char* b = in6->sin6_addr.s6_addr;
    static constexpr unsigned char kLoop[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    if (std::memcmp(b, kLoop, 16) == 0) return true;
    static constexpr unsigned char kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    return std::memcmp(b, kMapped, 12) == 0 && b[12] == 127;  // ::ffff:127.x.y.z
  }
  return false;
}

curl_socket_t on_opensocket(void* userdata, curlsocktype purpose, curl_sockaddr* address) {
  auto* ctx = static_cast<TransferCtx*>(userdata);
  const auto at = Clock::now();
  curl_socket_t s = socket(address->family, address->socktype, address->protocol);
  if (s == CURL_SOCKET_BAD) return s;
  const auto* sa = reinterpret_cast<const sockaddr*>(&address->addr);
  if (purpose == CURLSOCKTYPE_IPCXN && address->socktype == SOCK_STREAM && is_loopback(sa)) {
    // A refused loopback port answers with RST immediately; without SYN retransmissions the
    // connect fails at once and curl moves on to the next address (::1 -> 127.0.0.1).
    TCP_INITIAL_RTO_PARAMETERS rto{};
    rto.Rtt = TCP_INITIAL_RTO_UNSPECIFIED_RTT;
    rto.MaxSynRetransmissions = TCP_INITIAL_RTO_NO_SYN_RETRANSMISSIONS;
    DWORD bytes = 0;
    WSAIoctl(s, SIO_TCP_INITIAL_RTO, &rto, sizeof(rto), nullptr, 0, &bytes, nullptr, nullptr);
  }
  if (ctx->n_opens < ctx->opens.size()) {
    SocketOpen& o = ctx->opens[ctx->n_opens++];
    o.at = at;
    std::memset(&o.addr, 0, sizeof(o.addr));
    std::memcpy(&o.addr, sa,
                std::min<size_t>(sizeof(o.addr), static_cast<size_t>(address->addrlen)));
    o.ready = Clock::now();
  }
  return s;
}

bool same_ip(const sockaddr_storage& ss, const std::string& ip) {
  if (ip.empty()) return false;
  char buf[INET6_ADDRSTRLEN] = {};
  const void* src = nullptr;
  if (ss.ss_family == AF_INET)
    src = &reinterpret_cast<const sockaddr_in*>(&ss)->sin_addr;
  else if (ss.ss_family == AF_INET6)
    src = &reinterpret_cast<const sockaddr_in6*>(&ss)->sin6_addr;
  else
    return false;
  if (!inet_ntop(ss.ss_family, src, buf, sizeof(buf))) return false;
  return ip == buf;
}

const char* version_string(long v) {
  switch (v) {
    case CURL_HTTP_VERSION_1_0: return "HTTP/1.0";
    case CURL_HTTP_VERSION_1_1: return "HTTP/1.1";
    case CURL_HTTP_VERSION_2_0: return "HTTP/2";
    case CURL_HTTP_VERSION_3: return "HTTP/3";
    default: return "";
  }
}

// One hop's phases in microseconds; consecutive, so they add up to the hop's duration.
struct Phases {
  curl_off_t queue = 0, dns = 0, socket = 0, connect = 0, tls = 0, send = 0, ttfb = 0,
             download = 0;
};

// f: the hop's own marks; dur: hop duration; hop_start: the hop's time zero on our clock.
Phases split_hop(const Marks& f, curl_off_t dur, bool reused, Clock::time_point hop_start,
                 const TransferCtx& ctx, const std::string& ip) {
  // Clamp the marks into a monotonic sequence (a skipped phase collapses to 0).
  const curl_off_t q = f.q;
  curl_off_t n = std::max(q, f.n);
  curl_off_t c = std::max(n, f.c);
  curl_off_t sock = n;
  if (!reused) {
    // Winning socket of this hop: the one opened to the address curl reports (else the last).
    const SocketOpen* win = nullptr;
    const auto hop_end = hop_start + std::chrono::microseconds(dur);
    for (size_t i = 0; i < ctx.n_opens; ++i) {
      const SocketOpen& o = ctx.opens[i];
      if (o.at < hop_start || o.at > hop_end) continue;
      if (!win || same_ip(o.addr, ip) || !same_ip(win->addr, ip)) win = &o;
    }
    if (win) {
      auto rel = [&](Clock::time_point tp) {
        return static_cast<curl_off_t>(
            std::chrono::duration<double, std::micro>(tp - hop_start).count());
      };
      // When curl starts connecting before name resolution is fully done (happy eyeballs on
      // the first answer), DNS ends when the winning socket was opened instead of swallowing
      // the TCP handshake.
      const curl_off_t at = rel(win->at);
      if (at >= q && at < n) n = at;
      // Socket Initialization is creating + configuring the winning socket; time spent on
      // addresses that failed first (::1 before 127.0.0.1) stays in connect. A connect that
      // never completed (refused, timed out) still had its socket set up: it ends at the hop.
      const curl_off_t sock_end = f.c > 0 ? c : std::max(n, dur);
      sock = n + std::clamp<curl_off_t>(rel(win->ready) - at, 0, sock_end - n);
      c = std::max(c, sock);
    }
  }
  const curl_off_t a = std::max(c, f.a);
  const curl_off_t p = std::max(a, f.p);
  const curl_off_t pt = std::max(p, f.pt);
  const curl_off_t st = std::max(pt, f.st);
  const curl_off_t end = std::max(dur, st);
  Phases ph;
  ph.queue = q;
  ph.dns = n - q;
  ph.socket = sock - n;
  ph.connect = c - sock;
  ph.tls = a - c;
  ph.send = pt - a;  // protocol setup (pretransfer) + request upload
  ph.ttfb = st - pt;
  ph.download = end - st;
  return ph;
}

double us_ms(curl_off_t v) { return static_cast<double>(v) / 1000.0; }

curl_off_t us_between(Clock::time_point a, Clock::time_point b) {
  return static_cast<curl_off_t>(std::chrono::duration<double, std::micro>(b - a).count());
}

constexpr const char* kUserAgent = "watt/0.1";

}  // namespace

struct HttpClient::Impl {
  CURL* easy = nullptr;
  CURLM* multi = nullptr;
  CURLSH* share = nullptr;
  std::atomic<bool> cancel_flag{false};
  char errbuf[CURL_ERROR_SIZE] = {};
  TransferCtx ctx;
  curl_slist* hdrs = nullptr;        // request header list, reused while unchanged
  std::vector<Header> hdrs_for;      // the request headers `hdrs` was built from
  bool hdrs_built = false;
  bool fine_timer = false;           // this client holds a 1 ms timeBeginPeriod
  curl_off_t last_conn_id = -1;      // the connection curl used last, and its peer
  std::string last_ip;
  long last_port = 0;

  Impl() {
    ensure_global_init();
    share = curl_share_init();
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
    multi = MultiPool::get().take();
    easy = curl_easy_init();
    ctx.cancel = &cancel_flag;
    ctx.easy = easy;
    ctx.hops.reserve(4);
    if (!easy) return;
    // Options that never change between requests.
    CURL* h = easy;
    curl_easy_setopt(h, CURLOPT_SHARE, share);
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(h, CURLOPT_BUFFERSIZE, 256L * 1024L);
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");  // all decoders curl was built with
    curl_easy_setopt(h, CURLOPT_OPENSOCKETFUNCTION, &on_opensocket);
    curl_easy_setopt(h, CURLOPT_OPENSOCKETDATA, &ctx);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, &on_header);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, &on_write);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &ctx);
    // No progress callback: cancel() wakes the transfer loop, which checks the flag after
    // every curl_multi_perform, and the write callback checks it per body chunk. A progress
    // callback would only add calls on every perform.
    curl_easy_setopt(h, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 30L);
    // Only HTTP(S), for the request and for every redirect hop. libcurl is built with other
    // protocols (file://, smb://, ...); a URL or a Location header must never reach them
    // (local file reads, UNC/SMB NTLM credential leaks).
    curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    // Like curl (curl/x) and Postman (PostmanRuntime/x), every request names its client: some
    // APIs refuse requests without one (api.github.com answers 403). A User-Agent header in the
    // request replaces it; an empty one ("User-Agent" with no value) sends it empty.
    curl_easy_setopt(h, CURLOPT_USERAGENT, kUserAgent);
  }
  ~Impl() {
    if (fine_timer) timeEndPeriod(1);
    if (easy) curl_easy_cleanup(easy);
    MultiPool::get().give(multi);  // the easy handle is never left attached after send()
    if (share) curl_share_cleanup(share);
    curl_slist_free_all(hdrs);
  }

  // Per-request options as last applied to the easy handle.
  struct Applied {
    bool valid = false;
    std::string url;
    long timeout = 0, version = 0, follow = 0;
    bool verify = false, fresh = false;
    std::string ca_pem;
    int kind = 0;  // 0 GET-like, 1 HEAD, 2 with body
    std::string custom;
  } last;

  void apply(const HttpRequest& req) {
    CURL* h = easy;
    const bool all = !last.valid;
    if (all || req.url != last.url) {
      curl_easy_setopt(h, CURLOPT_URL, req.url.c_str());
      last.url = req.url;
    }
    const long timeout = req.timeout_ms > 0 ? req.timeout_ms : 0L;
    if (all || timeout != last.timeout) curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, timeout);
    last.timeout = timeout;

    long ver = CURL_HTTP_VERSION_2TLS;  // h2 offered via ALPN over TLS, HTTP/1.1 in plaintext
    switch (req.version) {
      case HttpVersion::Auto: ver = CURL_HTTP_VERSION_2TLS; break;
      case HttpVersion::Http1_1: ver = CURL_HTTP_VERSION_1_1; break;
      case HttpVersion::Http2: ver = CURL_HTTP_VERSION_2_0; break;
      case HttpVersion::Http2PriorKnowledge: ver = CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE; break;
    }
    if (all || ver != last.version) curl_easy_setopt(h, CURLOPT_HTTP_VERSION, ver);
    last.version = ver;
    if (all || req.verify_tls != last.verify) {
      curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, req.verify_tls ? 1L : 0L);
      curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, req.verify_tls ? 2L : 0L);
    }
    last.verify = req.verify_tls;
    if ((all && !req.ca_pem.empty()) || req.ca_pem != last.ca_pem) {
      if (req.ca_pem.empty()) {
        curl_easy_setopt(h, CURLOPT_CAINFO_BLOB, nullptr);
        curl_easy_setopt(h, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
      } else {
        curl_blob blob{const_cast<char*>(req.ca_pem.data()), req.ca_pem.size(), CURL_BLOB_COPY};
        curl_easy_setopt(h, CURLOPT_CAINFO_BLOB, &blob);
        curl_easy_setopt(h, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NO_REVOKE));
      }
      last.ca_pem = req.ca_pem;
    }
    if (all || req.fresh_connection != last.fresh) {
      curl_easy_setopt(h, CURLOPT_FRESH_CONNECT, req.fresh_connection ? 1L : 0L);
      curl_easy_setopt(h, CURLOPT_SSL_SESSIONID_CACHE, req.fresh_connection ? 0L : 1L);
      curl_easy_setopt(h, CURLOPT_DNS_CACHE_TIMEOUT, req.fresh_connection ? 0L : 60L);
    }
    last.fresh = req.fresh_connection;

    // Method + body. The body is handed to curl by pointer: no copy (so it is set on every
    // request that has one). POSTFIELDS switches curl to POST and HTTPGET back to GET (clearing
    // NOBODY), so the order below gives a clean state.
    const std::string& m = req.method;
    const bool custom = !(m == "GET" || m == "POST" || m == "HEAD");
    const bool send_body = !req.body.empty() || m == "POST" || m == "PUT" || m == "PATCH";
    const int kind = m == "HEAD" ? 1 : send_body ? 2 : 0;
    const bool need_custom = kind == 2 ? m != "POST" : custom;
    if (kind == 2) {
      curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(req.body.size()));
      curl_easy_setopt(h, CURLOPT_POSTFIELDS, req.body.data());
    } else if (all || kind != last.kind) {
      curl_easy_setopt(h, CURLOPT_POSTFIELDS, nullptr);
      curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, curl_off_t{-1});
      curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);
      if (kind == 1) curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
    }
    last.kind = kind;
    const char* want_custom = need_custom ? m.c_str() : "";
    if (all || last.custom != want_custom) {
      curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, need_custom ? m.c_str() : nullptr);
      last.custom = want_custom;
    }
    // OBEYCODE: a custom method is still switched to GET on 301/302/303 like a browser would.
    const long follow = !req.follow_redirects    ? 0L
                        : custom || send_body ? CURLFOLLOW_OBEYCODE
                                              : CURLFOLLOW_ALL;
    if (all || follow != last.follow) curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, follow);
    last.follow = follow;
    last.valid = true;
  }

  void set_headers(const std::vector<Header>& headers) {
    if (hdrs_built && headers == hdrs_for) return;
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, nullptr);
    curl_slist_free_all(hdrs);
    hdrs = nullptr;
    bool has_expect = false;
    std::string line;
    for (const auto& [k, v] : headers) {
      if (k.empty()) continue;
      if (iequals(k, "expect")) has_expect = true;
      line.clear();
      line.reserve(k.size() + v.size() + 2);
      line.append(k);
      if (v.empty()) {
        line.push_back(';');  // curl syntax for "send this header with an empty value"
      } else {
        line.append(": ");
        line.append(v);
      }
      hdrs = curl_slist_append(hdrs, line.c_str());
    }
    if (!has_expect) hdrs = curl_slist_append(hdrs, "Expect:");  // never stall on 100-continue
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, hdrs);
    hdrs_for = headers;
    hdrs_built = true;
  }
};

HttpClient::HttpClient() : impl_(std::make_unique<Impl>()) {}
HttpClient::~HttpClient() = default;

namespace {

// One throwaway HTTP/1.1 exchange over loopback with a responder thread. The first request in a
// process otherwise pays for loading the Winsock providers (first socket ~0.2 ms), the TCP
// stack's first connect (~0.3 ms) and paging in libcurl's and our own request / response code:
// a cold local request measured 1.15 -> 0.55 ms after it. Its client is destroyed afterwards,
// so no connection, DNS entry or TLS session carries over: real clients still start cold.
void warm_up_transfer() {
  SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (l == INVALID_SOCKET) return;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int alen = sizeof(a);
  if (bind(l, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(l, 1) != 0 ||
      getsockname(l, reinterpret_cast<sockaddr*>(&a), &alen) != 0) {
    closesocket(l);
    return;
  }
  SOCKET v6 = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);  // "localhost" tries ::1 first
  if (v6 != INVALID_SOCKET) closesocket(v6);
  std::thread responder([l] {
    SOCKET c = accept(l, nullptr, nullptr);
    if (c == INVALID_SOCKET) return;
    DWORD tmo = 2000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
    std::string got;
    char buf[2048];
    while (got.find("\r\n\r\n") == std::string::npos) {
      const int n = recv(c, buf, sizeof(buf), 0);
      if (n <= 0) break;
      got.append(buf, static_cast<size_t>(n));
    }
    static constexpr char kReply[] =
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\nok";
    send(c, kReply, static_cast<int>(sizeof(kReply) - 1), 0);
    shutdown(c, SD_SEND);
    closesocket(c);
  });
  {
    HttpClient warm;
    HttpRequest req;
    req.url = "http://127.0.0.1:" + std::to_string(ntohs(a.sin_port)) + "/";
    req.timeout_ms = 2000;
    req.busy_poll_ms = 0;
    (void)warm.send(req);
  }
  closesocket(l);  // unblocks accept() if curl never connected
  responder.join();
}

}  // namespace

void HttpClient::global_init() {
  static std::once_flag once;
  std::call_once(once, [] {
    MultiPool::get().prefill(1);
    // The first timeBeginPeriod in a process costs ~0.05 ms (winmm start-up); later ones ~5 us.
    timeBeginPeriod(1);
    timeEndPeriod(1);
    warm_up_transfer();
  });
}

bool ResponseHeaders::find(std::string_view name, std::string_view* value) const {
  for (size_t i = 0; i < ranges_.size(); ++i) {
    const HeaderView h = (*this)[i];
    if (iequals(h.first, name)) {
      if (value) *value = h.second;
      return true;
    }
  }
  return false;
}

std::vector<Header> ResponseHeaders::to_vector() const {
  std::vector<Header> out;
  out.reserve(ranges_.size());
  for (const HeaderView h : *this) out.emplace_back(std::string(h.first), std::string(h.second));
  return out;
}

void HttpClient::cancel() {
  impl_->cancel_flag.store(true, std::memory_order_relaxed);
  if (impl_->multi) curl_multi_wakeup(impl_->multi);
}

HttpResponse HttpClient::send(const HttpRequest& req, const BodyChunkFn& on_chunk) {
  const auto t0 = Clock::now();
  HttpResponse resp;
  Impl& im = *impl_;
  im.cancel_flag.store(false, std::memory_order_relaxed);

  if (!im.easy || !im.multi || !im.share) {
    resp.error = "libcurl initialisation failed";
    resp.timing.total_ms = resp.prepare_ms = ms_between(t0, Clock::now());
    return resp;
  }
  // The method goes into the request line verbatim (CURLOPT_CUSTOMREQUEST): it must be an
  // RFC 9110 token, or a saved request could smuggle CR/LF, spaces or a second request line.
  if (!is_http_token(req.method)) {
    resp.error = "invalid HTTP method \"" + req.method +
                 "\": use letters, digits or !#$%&'*+-.^_`|~ only (no spaces or line breaks)";
    resp.timing.total_ms = resp.prepare_ms = ms_between(t0, Clock::now());
    return resp;
  }

  CURL* h = im.easy;
  im.errbuf[0] = '\0';
  TransferCtx& ctx = im.ctx;
  ctx.begin(&resp, &on_chunk, req);

  // While a client is in use the process asks for 1 ms timer resolution (as Chromium-based
  // tools do). With Windows' default 15.6 ms tick the CPU drops into deep idle during every
  // server wait, and the work right after the response (and before the next request) runs on
  // a cold, down-clocked core: warm HTTPS re-fires measured 0.198 -> 0.163 ms client-side
  // (p50, 10/10 interleaved process runs). Taken once per client on its first send, so that
  // request pays the call (~5 us after global_init) in prepare_ms; released when the client
  // is destroyed. Toggling it per request costs ~0.13 ms, more than it saves.
  if (!im.fine_timer) im.fine_timer = timeBeginPeriod(1) == TIMERR_NOERROR;
  // Every per-request option gets an explicit value (no curl_easy_reset, see the notes above);
  // curl never changes them itself, so only what differs from the last request is applied.
  im.apply(req);
  im.set_headers(req.headers);

  // Drive the transfer. curl starts its clock inside the first curl_multi_perform, so t_io
  // (taken right before it) is curl's time zero on our clock. t_curl is taken as each
  // curl_multi_perform returns: after the last one it is the moment curl handed the finished
  // transfer back, so everything after it is our own result collection (process_ms).
  CURLcode result = CURLE_OK;
  bool cancelled = false;
  std::string multi_error;
  CURLMcode mc = curl_multi_add_handle(im.multi, h);
  const bool added = mc == CURLM_OK;
  const auto t_io = Clock::now();
  auto t_curl = t_io;
  // See HttpRequest::busy_poll_ms: for the first busy_poll_ms of the transfer this thread
  // stays on the CPU and re-polls curl instead of sleeping until the network wakes it.
  const bool spin = req.busy_poll_ms > 0;
  const auto spin_until = t_io + std::chrono::milliseconds(spin ? req.busy_poll_ms : 0);
  bool connected = false;
  if (!added) {
    multi_error = curl_multi_strerror(mc);
  } else {
    for (;;) {
      int running = 0;
      mc = curl_multi_perform(im.multi, &running);
      t_curl = Clock::now();
      if (mc != CURLM_OK) {
        multi_error = curl_multi_strerror(mc);
        break;
      }
      if (running == 0) break;
      if (im.cancel_flag.load(std::memory_order_relaxed)) {
        cancelled = true;
        break;
      }
      // Busy-poll: let curl look at its sockets again shortly instead of sleeping in the
      // kernel; a few pause instructions keep the loop from hammering the socket calls.
      // While a connection is still being set up the loop sleeps in 1 ms polls instead:
      // spinning there now and then left curl blind to a completed loopback connect until
      // its next ~10 ms timer.
      const bool in_window = spin && t_curl < spin_until;
      if (in_window &&
          (connected || (connected = info_off(h, CURLINFO_CONNECT_TIME_T) > 0 ||
                                     info_off(h, CURLINFO_PRETRANSFER_TIME_T) > 0))) {
        for (int k = 0; k < 64; ++k) YieldProcessor();
        continue;
      }
      mc = curl_multi_poll(im.multi, nullptr, 0, in_window ? 1 : 1000, nullptr);
      if (mc != CURLM_OK) {
        t_curl = Clock::now();
        multi_error = curl_multi_strerror(mc);
        break;
      }
    }
    int left = 0;
    while (CURLMsg* msg = curl_multi_info_read(im.multi, &left)) {
      if (msg->msg == CURLMSG_DONE && msg->easy_handle == h) result = msg->data.result;
    }
    curl_multi_remove_handle(im.multi, h);  // curl's marks stay readable after removal
  }
  if (im.cancel_flag.load(std::memory_order_relaxed) &&
      (result == CURLE_ABORTED_BY_CALLBACK || result == CURLE_WRITE_ERROR))
    cancelled = true;

  // Collect results.
  if (cancelled) {
    resp.ok = false;
    resp.error = "cancelled";
  } else if (!multi_error.empty()) {
    resp.ok = false;
    resp.error = multi_error;
  } else if (result != CURLE_OK) {
    resp.ok = false;
    resp.error = im.errbuf[0] ? std::string(im.errbuf) : std::string(curl_easy_strerror(result));
  } else {
    resp.ok = true;
  }

  // Status and protocol come from the status line the header callback already parsed; libcurl
  // is asked only when there was none it could parse.
  resp.status = ctx.hop_status;
  if (resp.status == 0) curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &resp.status);
  if (*ctx.hop_version) {
    resp.http_version = ctx.hop_version;
  } else {
    long hv = 0;
    curl_easy_getinfo(h, CURLINFO_HTTP_VERSION, &hv);
    resp.http_version = version_string(hv);
  }
  curl_easy_getinfo(h, CURLINFO_REDIRECT_COUNT, &resp.redirect_count);
  char* s = nullptr;
  if (curl_easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &s) == CURLE_OK && s) resp.effective_url = s;
  // The peer belongs to the connection: re-firing on the same one keeps the last address.
  const bool any_open = ctx.n_opens > 0;
  curl_off_t conn_id = -1;
  curl_easy_getinfo(h, CURLINFO_CONN_ID, &conn_id);
  if (any_open || conn_id < 0 || conn_id != im.last_conn_id) {
    s = nullptr;
    if (curl_easy_getinfo(h, CURLINFO_PRIMARY_IP, &s) == CURLE_OK && s) resp.remote_ip = s;
    curl_easy_getinfo(h, CURLINFO_PRIMARY_PORT, &resp.remote_port);
    im.last_conn_id = conn_id;
    im.last_ip = resp.remote_ip;
    im.last_port = resp.remote_port;
  } else {
    resp.remote_ip = im.last_ip;
    resp.remote_port = im.last_port;
  }
  if (resp.status != 0) {
    ctx.last_hdr_bytes = std::max<size_t>(resp.headers.text_bytes(), 64);
    ctx.last_hdr_count = std::max<size_t>(resp.headers.size(), 4);
  }
  if (!resp.ok && resp.status == 0) resp.headers.clear();
  if (!resp.ok) resp.body.clear();

  // Redirect hops. A 3xx that curl did not follow (redirect limit) is the final response; a
  // followed one whose successor never produced a status line (refused, timed out) closes now.
  // Without a socket opened in this send() curl has no DNS / connect / TLS marks to give.
  const Marks all = Marks::read(h, any_open || !ctx.hops.empty());
  const auto followed = static_cast<size_t>(std::max<long>(resp.redirect_count, 0));
  if (ctx.hops.size() > followed) {
    ctx.hops.resize(followed);
    ctx.hop_open = false;
  }
  if (ctx.hop_open) ctx.close_hop();
  while (!ctx.hops.empty() && ctx.hops.back().redirect_end < 0) ctx.hops.pop_back();

  // Timeline (one monotonic clock; curl's zero is t_io):
  //   t0 .. t_io            our request setup                        -> prepare_ms
  //   0 .. R                every redirect hop (curl time_redirect)  -> hops / redirect_ms
  //   R + [0 .. q]          curl's own queue for the final hop       -> queue
  //   R + [q .. st]         dns / socket / connect / tls / send / ttfb of the final hop
  //   R + st .. t_curl      first byte -> curl returned the transfer -> download
  //   t_curl .. t_end       result collection                        -> process_ms
  // Each phase is its own measured delta (see split_hop for DNS vs socket vs connect).
  Timing& tm = resp.timing;
  Marks base;
  curl_off_t R = 0, dl_base = 0;
  resp.hops.reserve(ctx.hops.size());
  for (const RawHop& rh : ctx.hops) {
    const Marks d = rh.at_end.minus(base);
    const curl_off_t dur = std::max<curl_off_t>(rh.redirect_end - R, 0);
    const bool reused = d.conns == 0;
    const Phases ph = split_hop(d, dur, reused, t_io + std::chrono::microseconds(R), ctx, rh.ip);
    RedirectHop& hop = resp.hops.emplace_back();
    hop.url = rh.url;
    hop.status = rh.status;
    hop.remote_ip = rh.ip;
    hop.remote_port = rh.port;
    hop.connection_reused = reused;
    hop.queue_ms = us_ms(ph.queue);
    hop.dns_ms = us_ms(ph.dns);
    hop.socket_ms = us_ms(ph.socket);
    hop.connect_ms = us_ms(ph.connect);
    hop.tls_ms = us_ms(ph.tls);
    hop.send_ms = us_ms(ph.send);
    hop.ttfb_ms = us_ms(ph.ttfb);
    hop.download_ms = us_ms(ph.download);
    const curl_off_t hop_us = ph.queue + ph.dns + ph.socket + ph.connect + ph.tls + ph.send +
                              ph.ttfb + ph.download;  // == max(dur, the hop's last mark)
    hop.total_ms = us_ms(hop_us);
    hop.bytes_sent = static_cast<uint64_t>(d.req);
    hop.bytes_received =
        rh.header_bytes + static_cast<uint64_t>(std::max<curl_off_t>(rh.dl_after - dl_base, 0));
    base = rh.at_end;
    R += hop_us;
    dl_base = rh.dl_after;
  }

  Marks f = all.minus(base);
  // The final hop reused a connection when curl made none and no socket was opened for it.
  // That holds for a request that then failed (timeout, cancel, no answer) too, as long as the
  // request went out: a DNS failure opens nothing and sends nothing.
  const auto final_start = t_io + std::chrono::microseconds(R);
  bool opened_final = false;
  for (size_t i = 0; i < ctx.n_opens && !opened_final; ++i)
    opened_final = ctx.opens[i].at >= final_start;
  const bool no_new_conn = f.conns == 0 && !opened_final;
  const bool failed_early = !resp.ok && resp.status == 0;  // no response line at all
  tm.connection_reused = no_new_conn && (resp.status != 0 || f.req > 0);
  if (failed_early) {
    // When a transfer fails before it has a response, curl stamps the transfer marks it never
    // reached (pretransfer, posttransfer, first byte) with the failure time; only those it
    // really passed count. Nothing written means it never got to pretransfer.
    f.st = 0;
    if (f.req == 0) f.p = f.pt = 0;
  }
  const curl_off_t st_f = std::max({f.q, f.n, f.c, f.a, f.p, f.pt, f.st});
  const curl_off_t T = std::max(R + st_f, us_between(t_io, t_curl));
  Phases ph = split_hop(f, T - R, no_new_conn, final_start, ctx, resp.remote_ip);
  // The phase that ran until curl handed the transfer back: download, or for a transfer that
  // failed before any response byte the phase that was in progress (refused or timed-out
  // connect, TLS failure, a server that never answers, a reused connection gone silent).
  curl_off_t* last = &ph.download;
  if (failed_early) {
    const bool tls = resp.effective_url.compare(0, 6, "https:") == 0 ||
                     req.url.compare(0, 6, "https:") == 0;
    if (f.req > 0)  // the request went out (all of it, unless the body upload was cut short)
      last = f.up < static_cast<curl_off_t>(req.body.size()) ? &ph.send : &ph.ttfb;
    else if (no_new_conn) last = &ph.dns;  // never got as far as a connection
    else if (f.c == 0) last = &ph.connect;
    else if (tls && f.a == 0) last = &ph.tls;
    else last = &ph.send;
    *last += ph.download;
    ph.download = 0;
  }

  // Sizes of the final request / response, and totals over all hops.
  const curl_off_t up = std::min(f.up, f.req);
  resp.sizes.request_body = static_cast<uint64_t>(up);
  resp.sizes.request_headers = static_cast<uint64_t>(f.req - up);
  resp.sizes.response_headers = ctx.hop_header_bytes;
  const curl_off_t dl_all = info_off(h, CURLINFO_SIZE_DOWNLOAD_T);
  resp.sizes.response_body_wire = static_cast<uint64_t>(std::max<curl_off_t>(dl_all - dl_base, 0));
  resp.sizes.response_body = ctx.hop_body_bytes;
  tm.bytes_sent = static_cast<uint64_t>(all.req);
  tm.bytes_received = static_cast<uint64_t>(info_long(h, CURLINFO_HEADER_SIZE) + dl_all);

  resp.redirect_ms = us_ms(R);
  tm.queue_ms = us_ms(ph.queue);
  tm.dns_ms = us_ms(ph.dns);
  resp.socket_ms = us_ms(ph.socket);
  tm.connect_ms = us_ms(ph.connect);
  tm.tls_ms = us_ms(ph.tls);
  tm.send_ms = us_ms(ph.send);
  tm.ttfb_ms = us_ms(ph.ttfb);
  tm.download_ms = us_ms(ph.download);
  // curl's marks are whole microseconds; the phase that ran until t_curl also gets the
  // sub-microsecond rest, so curl's phases cover exactly t_io .. t_curl.
  double* last_ms = last == &ph.download  ? &tm.download_ms
                    : last == &ph.ttfb    ? &tm.ttfb_ms
                    : last == &ph.send    ? &tm.send_ms
                    : last == &ph.tls     ? &tm.tls_ms
                    : last == &ph.connect ? &tm.connect_ms
                                          : &tm.dns_ms;
  *last_ms += ms_between(t_io, t_curl) - us_ms(T);
  double overshoot = 0;  // curl's last mark landed a hair after t_curl (its clock rounds up)
  if (*last_ms < 0) {
    overshoot = -*last_ms;
    *last_ms = 0;
  }

  const auto t_end = Clock::now();
  tm.total_ms = ms_between(t0, t_end);
  resp.prepare_ms = ms_between(t0, t_io);
  resp.process_ms = std::max(0.0, ms_between(t_curl, t_end) - overshoot);
  return resp;
}

}  // namespace watt
