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
// Lowest TLS version offered (CURLOPT_SSLVERSION). Default: what Schannel allows (TLS 1.2 and
// 1.3 on current Windows).
enum class TlsVersion { Default, Tls1_0, Tls1_1, Tls1_2, Tls1_3 };

struct HttpRequest {
  std::string method = "GET";
  std::string url;
  std::vector<Header> headers;
  std::string body;
  long timeout_ms = 30000;      // 0 = no timeout
  bool follow_redirects = true;
  // Redirects (only while follow_redirects):
  //   max_redirects            hops followed before the send fails (CURLOPT_MAXREDIRS, 0..100)
  //   keep_method_on_redirect  keep the method and body on 301 / 302 / 303 (CURLOPT_POSTREDIR
  //                            CURL_REDIR_POST_ALL) instead of switching to GET like a browser
  //   keep_auth_on_redirect    also send credentials (Authorization / Cookie headers, Basic,
  //                            Digest, SigV4) to a redirect target on another host or port
  //                            (CURLOPT_UNRESTRICTED_AUTH). Off by default: credentials go to the
  //                            original host only. Windows SSO (NTLM / Negotiate) never follows.
  long max_redirects = 30;
  bool keep_method_on_redirect = false;
  bool keep_auth_on_redirect = false;
  // false: the path and query go out as written, dot segments ("/a/../b") included
  // (CURLOPT_PATH_AS_IS). The app percent-encodes the URL before it gets here when true.
  bool encode_url = true;
  TlsVersion tls_min_version = TlsVersion::Default;
  bool verify_tls = true;
  // PEM CA certificate(s) trusted for this request's TLS instead of the system store (a private
  // CA, e.g. a dev proxy's). Revocation is not checked for such a chain: private CAs rarely
  // publish CRLs. Empty = the system roots.
  std::string ca_pem;
  // Client certificate for mutual TLS, presented when the server asks for one (Schannel). Either
  //   PEM: client_cert holds the certificate (optionally followed by its chain) and client_key
  //        its private key: PKCS#8 "PRIVATE KEY" / "ENCRYPTED PRIVATE KEY" (RSA or EC) or PKCS#1
  //        "RSA PRIVATE KEY"; client_key may be empty when client_cert holds the key too; or
  //   PKCS#12: client_cert holds the .pfx / .p12 bytes (DER) and client_key is empty.
  // client_key_passphrase decrypts an encrypted key or the PKCS#12. The key never leaves the
  // process (it is converted to an in-memory PKCS#12 once per distinct value and never persisted
  // to a key store). A bad key or passphrase fails the send with a message saying which.
  std::string client_cert;
  std::string client_key;
  std::string client_key_passphrase;
  // Proxy (CURLOPT_PROXY / CURLOPT_NOPROXY):
  //   Env     libcurl's default: the https_proxy / http_proxy / all_proxy / no_proxy variables
  //   System  the Windows (Internet Options) proxy of the current user: its static proxy server
  //           and bypass list ("<local>" = loopback and dotless names); auto-config scripts (PAC)
  //           and auto-detection are not evaluated (the request goes direct then)
  //   Off     always direct
  //   Url     proxy_url: "http://host:port", "https://...", "socks5h://...", may carry
  //           "user:password@"; no_proxy lists hosts that go direct (comma separated, curl syntax)
  // Loopback hosts never go through a System proxy.
  enum class Proxy { Env, System, Off, Url };
  Proxy proxy = Proxy::Env;
  std::string proxy_url;
  std::string no_proxy;
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
  // HTTP authentication done by libcurl (CURLOPT_HTTPAUTH with CURLOPT_USERNAME / _PASSWORD):
  // Digest, NTLM and Negotiate (SPNEGO) answer the server's 401 challenge, so they cost one
  // extra round trip on the first request of a connection. NTLM and Negotiate use Windows SSPI:
  // with an empty auth_user they sign in as the current Windows user. Basic here sends the
  // header pre-emptively (Workspace::http_to_send() adds Basic as a header instead). Credentials
  // are sent to the original host only, never to a redirect target on another host.
  enum class Auth { None, Basic, Digest, Ntlm, Negotiate };
  Auth auth = Auth::None;
  std::string auth_user;
  std::string auth_password;
  // AWS Signature Version 4 (CURLOPT_AWS_SIGV4), e.g. "aws:amz:eu-west-1:execute-api": curl
  // signs the request with auth_user = access key id, auth_password = secret access key (auth
  // is ignored then). A session token goes in an X-Amz-Security-Token header, which is signed.
  std::string aws_sigv4;
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

// The server's leaf certificate, as Schannel received it.
struct TlsCertificate {
  std::string subject;      // X.500 DN, most specific first: "CN=www.google.com"
  std::string subject_cn;   // simple display name: "www.google.com"
  std::string issuer;       // X.500 DN: "CN=WR2, O=Google Trust Services, C=US"
  std::string issuer_cn;    // "WR2"
  std::vector<std::string> subject_alt_names;  // DNS names and IP addresses, in certificate order
  std::string not_before;   // ISO 8601 UTC, "2026-01-15T08:00:00Z"
  std::string not_after;    // ISO 8601 UTC
  std::string serial;       // big-endian hex, upper case, no separators (as openssl x509 -serial)
  std::string sha256_fingerprint;  // "AB:CD:...", 32 colon-separated bytes (openssl -fingerprint)
};

// TLS session of one connection. Immutable once read; shared by every response that went over
// that connection.
struct TlsInfo {
  std::string version;  // "TLSv1.3", "TLSv1.2", ... (SECPKG_ATTR_CONNECTION_INFO)
  std::string cipher;   // "TLS_AES_256_GCM_SHA384" (SECPKG_ATTR_CIPHER_INFO)
  std::string alpn;     // "h2", "http/1.1"; empty when the server did not negotiate ALPN
  std::string verify_result;  // "verified: ..." / "not verified (...)" / "failed (...)"
  TlsCertificate leaf;
  int chain_length = 0;  // certificates in the chain the server sent, leaf included
  std::string chain;     // that chain, one "CN  (issued by CN, until YYYY-MM-DD)" per line
  // The client certificate this side presented (mutual TLS), "" when none was sent.
  std::string client_cert_subject;  // simple display name of it: "my-client"
  std::string client_cert_issuer;   // its issuer's simple display name
  double read_ms = 0;    // what reading all this cost (once per connection)
};

// The connection that carried the final request. Read once per connection, right after it is
// ready (TLS handshake done) and before the first request on it goes out (libcurl's pre-request
// callback, for a new connection or a pooled one this client has not seen yet), and cached per
// client: a request on a reused connection gets a copy of the same values (two short strings
// and a shared_ptr), which describe that same connection, whose handshake happened earlier.
// TLS details come from the Schannel context (no CURLOPT_CERTINFO, nothing extra per request).
// A transfer that never got a connection ready (refused, TLS failure) has addresses at most.
struct ConnectionInfo {
  long long id = -1;     // libcurl's connection id (CURLINFO_CONN_ID); equal id = same connection
  bool reused = false;   // == timing.connection_reused
  std::string local_ip;  // CURLINFO_LOCAL_IP / _PORT
  long local_port = 0;
  std::string remote_ip;  // == HttpResponse::remote_ip / remote_port
  long remote_port = 0;
  bool tls = false;       // a TLS session was established on this connection
  std::shared_ptr<const TlsInfo> tls_info;  // non-null when tls (null if it could not be read)
  // The request went through this proxy (CURLINFO_USED_PROXY): "http://proxy.corp:8080", user
  // name and password left out; remote_ip / remote_port are then the proxy's. Empty when direct.
  std::string proxy;
  bool client_cert = false;  // a client certificate was configured for the request (HttpRequest)
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
  // Mirrors of conn / conn.tls_info, in the form the response view first used (kept so it
  // compiles unchanged; new code should read `conn`). Empty when conn.tls_info is null.
  std::string local_ip;        // == conn.local_ip
  long local_port = 0;         // == conn.local_port
  std::string tls_version;     // == conn.tls_info->version ("TLSv1.3")
  std::string tls_cipher;      // == conn.tls_info->cipher
  std::string cert_subject;    // == conn.tls_info->leaf.subject
  std::string cert_san;        // leaf.subject_alt_names joined with ", "
  std::string cert_issuer;     // == conn.tls_info->leaf.issuer
  std::string cert_not_after;  // leaf expiry, "2026-01-15 23:59:59 UTC"
  std::string cert_chain;      // == conn.tls_info->chain
  std::string verify_result;   // == conn.tls_info->verify_result
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
  ConnectionInfo conn;  // see ConnectionInfo
};

// Sum of all phases; equals timing.total_ms.
inline double phase_total(const HttpResponse& r) {
  const Timing& t = r.timing;
  return r.prepare_ms + t.queue_ms + t.dns_ms + r.socket_ms + t.connect_ms + t.tls_ms +
         t.send_ms + t.ttfb_ms + t.download_ms + r.redirect_ms + r.process_ms;
}

// RFC 9110 token (what an HTTP method must be): 1+ of ALPHA / DIGIT / !#$%&'*+-.^_`|~
inline bool is_http_token(std::string_view s) {
  if (s.empty()) return false;
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    const bool alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    if (!alnum && std::string_view("!#$%&'*+-.^_`|~").find(ch) == std::string_view::npos) return false;
  }
  return true;
}

// Optional streaming hook: called on the worker thread for each received body chunk.
using BodyChunkFn = std::function<void(const char* data, size_t len)>;

// Optional head hook: called on the worker thread as soon as the final response's header block
// has ended (the blank line after a non-1xx status line that curl does not follow), before any
// body byte. `head` is a partial HttpResponse: ok, status, status_text, http_version, headers,
// effective_url, remote_ip / port, conn (with the TLS details when the connection was read),
// redirect_count, sizes.response_headers and the timing so far: prepare_ms, redirect_ms and
// queue .. ttfb are final (DNS, socket, connect, TLS, send, Wait); download_ms and process_ms
// are 0 and timing.total_ms is the time until the head. It may fire more than once in one send
// (an authentication retry: Digest / NTLM answer a 401 with a second request); the last one
// belongs to the response send() returns. A send that never gets a final header block (refused,
// cut before the blank line) never calls it. It also fires for a send that is then cancelled,
// which is the point: a stream that never ends still shows its status, headers and phases.
using HeadFn = std::function<void(const HttpResponse& head)>;

class HttpClient {
 public:
  HttpClient();
  ~HttpClient();
  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  // Process-wide start-up warm-up, about 5 ms once: libcurl / TLS initialisation, crypt32's
  // name tables (the first certificate read otherwise pays ~5 ms for them), one
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
  HttpResponse send(const HttpRequest& req, const BodyChunkFn& on_chunk, const HeadFn& on_head);
  void cancel();  // aborts the in-flight send() ASAP; it returns with ok=false, error="cancelled"

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Simple display names of the subjects of the certificates in a PEM bundle, in file order (a
// block that does not parse is skipped). For consent prompts: "watt will trust certificates
// signed by <subject>".
std::vector<std::string> pem_certificate_subjects(std::string_view pem);

}  // namespace watt
