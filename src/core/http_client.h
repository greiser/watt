#pragma once
// REST engine. Thin, fast wrapper over libcurl. One HttpClient per worker thread; it keeps a
// persistent easy handle + share handle so DNS, TLS sessions and connections are reused across
// requests (that is what an engineer re-firing a request expects, and it is where Electron
// tools lose time). Thread-safety: an HttpClient instance is used by one thread at a time;
// cancel() may be called from any thread. A client that has sent a request holds a 1 ms
// Windows timer resolution (timeBeginPeriod) until it is destroyed: it keeps the CPU out of
// deep idle during server waits, which cuts warm per-request overhead by ~20%; for the first
// HttpRequest::busy_poll_ms of a transfer it does not sleep at all (see there).
//
// HTTP/2: HttpVersion::Auto offers h2 + http/1.1 via ALPN on TLS and uses whatever the server
// picks; plaintext stays HTTP/1.1 (use Http2PriorKnowledge for h2c).
//
// Timing: Timing's phases cover the final request. The extra HTTP phases live on HttpResponse
// (prepare_ms, socket_ms, redirect_ms, process_ms) so that
//   total_ms == prepare + queue + dns + socket + connect + tls + send + ttfb + download
//               + redirect_ms + process_ms                               (see phase_total())
// Every phase is a measured delta on one monotonic clock (curl's per-hop marks plus our own
// marks around the transfer), not a remainder. Split of "send() -> I/O started":
//   prepare_ms = watt's own request setup (options, headers, multi add): wrapper time
//   queue_ms   = libcurl's internal queue (URL parse, connection pool lookup / liveness check)
// and of "curl done -> send() returns": download_ms ends when the last curl_multi_perform
// returned the finished transfer, and process_ms (result collection) is measured from there.
// Each followed redirect hop has its own breakdown in HttpResponse::hops; their totals sum to
// redirect_ms.
//
// Sizes: Timing::bytes_sent is every request byte curl wrote (request lines + headers + body,
// all hops); Timing::bytes_received is every response header byte + body byte as it came off
// the wire (all hops), i.e. still compressed when Content-Encoding is used. HttpResponse::sizes splits the final request/response into headers and body, and gives
// the body both on the wire and decoded. For HTTP/2 the header sizes are the HTTP/1-style text
// length (before HPACK), like curl's size_header / size_request.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/timing.h"

namespace watt {

using Header = std::pair<std::string, std::string>;

// One response header as views into its HttpResponse (valid while that response lives and is
// not modified). Has .first / .second like Header, so `for (const auto& [k, v] : r.headers)`,
// `r.headers[i].first == "X"` work unchanged; use std::string(v) to keep a copy.
struct HeaderView {
  std::string_view first;
  std::string_view second;
};

// Response headers in arrival order (duplicates kept). Stored as one text buffer plus offsets,
// so receiving N headers costs two allocations, not 2N strings: the engine fills it straight
// from libcurl's header callback and send() does no per-header work after the last byte.
class ResponseHeaders {
 public:
  size_t size() const { return ranges_.size(); }
  bool empty() const { return ranges_.empty(); }
  HeaderView operator[](size_t i) const {
    const auto& r = ranges_[i];
    const std::string_view t(text_);
    return {t.substr(r[0], r[1]), t.substr(r[2], r[3])};
  }
  class iterator {
   public:
    using value_type = HeaderView;
    using difference_type = std::ptrdiff_t;
    iterator() = default;
    iterator(const ResponseHeaders* h, size_t i) : h_(h), i_(i) {}
    HeaderView operator*() const { return (*h_)[i_]; }
    iterator& operator++() { ++i_; return *this; }
    iterator operator++(int) { iterator t = *this; ++i_; return t; }
    bool operator==(const iterator& o) const { return i_ == o.i_; }
    bool operator!=(const iterator& o) const { return i_ != o.i_; }
   private:
    const ResponseHeaders* h_ = nullptr;
    size_t i_ = 0;
  };
  iterator begin() const { return {this, 0}; }
  iterator end() const { return {this, ranges_.size()}; }

  // First header with this name (ASCII case-insensitive). get() returns "" when absent.
  bool find(std::string_view name, std::string_view* value) const;
  bool contains(std::string_view name) const { return find(name, nullptr); }
  std::string_view get(std::string_view name) const {
    std::string_view v;
    find(name, &v);
    return v;
  }
  std::vector<Header> to_vector() const;  // owning copy

  // Engine side.
  void clear() {
    text_.clear();
    ranges_.clear();
  }
  void reserve(size_t bytes, size_t count) {
    text_.reserve(bytes);
    ranges_.reserve(count);
  }
  void add(std::string_view name, std::string_view value) {
    const auto off = static_cast<uint32_t>(text_.size());
    text_.append(name);
    text_.append(value);
    ranges_.push_back({off, static_cast<uint32_t>(name.size()),
                       off + static_cast<uint32_t>(name.size()),
                       static_cast<uint32_t>(value.size())});
  }
  size_t text_bytes() const { return text_.size(); }

 private:
  std::string text_;                                 // name0 value0 name1 value1 ...
  std::vector<std::array<uint32_t, 4>> ranges_;      // name off, name len, value off, value len
};

enum class HttpVersion { Auto, Http1_1, Http2, Http2PriorKnowledge };

struct HttpRequest {
  std::string method = "GET";
  std::string url;
  std::vector<Header> headers;
  std::string body;
  long timeout_ms = 30000;      // 0 = no timeout
  bool follow_redirects = true;
  bool verify_tls = true;
  HttpVersion version = HttpVersion::Auto;
  bool fresh_connection = false;  // force a new connection (measure cold cost)
  bool keep_body = true;  // false: body is only streamed to on_chunk, never held in memory
  // Busy-poll window: for the first busy_poll_ms of a transfer send() keeps polling libcurl on
  // the CPU instead of sleeping until the network wakes the thread (0 = always sleep). A thread
  // that sleeps through a server wait wakes on an idle, down-clocked core: the wake-up and all
  // the work after the response arrives run slow (warm HTTPS re-fire: ~0.15 ms client-side
  // when sleeping, ~0.05 ms when polling; a 3 ms local server wait: 3.17 -> 3.07 ms total).
  // Costs one core for at most this long per request; longer waits then sleep as usual.
  long busy_poll_ms = 20;
};

// Final request/response sizes in bytes (see "Sizes" above).
struct HttpSizes {
  uint64_t request_headers = 0;     // request line + headers
  uint64_t request_body = 0;
  uint64_t response_headers = 0;    // status line + headers (+ any 1xx interim responses)
  uint64_t response_body_wire = 0;  // body bytes received, before content decoding
  uint64_t response_body = 0;       // body bytes after decoding (what body / on_chunk got)
};

// One followed redirect hop (a 3xx response curl acted on). Phases are measured like the
// final request's and add up to total_ms; download_ms runs from the first response byte until
// curl moved on to the next hop.
struct RedirectHop {
  std::string url;
  long status = 0;
  std::string remote_ip;
  long remote_port = 0;
  bool connection_reused = false;
  double queue_ms = 0, dns_ms = 0, socket_ms = 0, connect_ms = 0, tls_ms = 0, send_ms = 0,
         ttfb_ms = 0, download_ms = 0, total_ms = 0;
  uint64_t bytes_sent = 0;      // request headers + body of this hop
  uint64_t bytes_received = 0;  // response headers + wire body of this hop
};

struct HttpResponse {
  bool ok = false;              // transport succeeded (any HTTP status)
  std::string error;            // transport error text when !ok
  long status = 0;
  std::string status_text;      // reason phrase if server sent one
  std::string http_version;     // "HTTP/1.1", "HTTP/2"
  ResponseHeaders headers;      // final response's headers (see ResponseHeaders)
  std::string body;
  std::string effective_url;
  std::string remote_ip;
  long remote_port = 0;
  Timing timing;
  long redirect_count = 0;
  HttpSizes sizes;
  std::vector<RedirectHop> hops;  // followed redirects, in order (size() == redirect_count)
  double prepare_ms = 0;   // send() -> request handed to libcurl (watt's own setup)
  double socket_ms = 0;    // name resolved -> socket created and configured (Socket Init.)
  double redirect_ms = 0;  // every redirect hop before the final request (sum of hops)
  // curl's done mark -> send() returns: libcurl's own untimed work around the transfer (its
  // per-transfer setup before its clock starts, putting the connection back in the pool,
  // detaching the handle) plus watt's result collection. curl.exe's -w time_total leaves
  // all of this out; here it is counted so total_ms is the real wall time of send().
  double process_ms = 0;
};

// Sum of all phases; equals timing.total_ms.
inline double phase_total(const HttpResponse& r) {
  const Timing& t = r.timing;
  return r.prepare_ms + t.queue_ms + t.dns_ms + r.socket_ms + t.connect_ms + t.tls_ms +
         t.send_ms + t.ttfb_ms + t.download_ms + r.redirect_ms + r.process_ms;
}

// Optional streaming hook: called on the worker thread for each received body chunk.
using BodyChunkFn = std::function<void(const char* data, size_t len)>;

class HttpClient {
 public:
  HttpClient();
  ~HttpClient();
  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  // Process-wide start-up warm-up, about 3 ms once: libcurl / TLS initialisation, one
  // pre-built libcurl multi handle (curl_multi_init costs about 0.5 ms), and one throwaway
  // HTTP exchange with an in-process responder over loopback, which loads the Winsock
  // providers, runs the TCP stack's first connect and pages in the request code (the first
  // local request after it: ~1.15 -> ~0.6 ms). Call it early on a worker thread (app start-up):
  // the first HttpClient then constructs in microseconds. A destroyed HttpClient returns its
  // multi handle to a small pool, so creating a client per tab or per request stays cheap.
  // Connections, DNS and TLS sessions are NOT carried over: every new HttpClient starts cold.
  // Thread-safe, idempotent.
  static void global_init();

  HttpResponse send(const HttpRequest& req, const BodyChunkFn& on_chunk = {});
  void cancel();  // aborts the in-flight send() ASAP; it returns with ok=false, error="cancelled"

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace watt
