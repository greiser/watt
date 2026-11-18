#pragma once
// WebSocket client (RFC 6455) for watt's WebSocket requests (RequestKind::WebSocket).
//
// libcurl does the connection: DNS, TCP, TLS (Schannel, the same trust settings as HttpClient:
// verify_tls, ca_pem, tls_min_version) and proxies (Env / System / Off / Url, tunnelled with
// CONNECT), with CURLOPT_CONNECT_ONLY. The upgrade request and the framing are done here over
// curl_easy_send / curl_easy_recv, so this works with watt's pinned libcurl, which is built
// without curl's own WebSocket API (CURL_DISABLE_WEBSOCKETS).
//
// Only ws://, wss://, http:// and https:// URLs (http(s) map to ws(s)); libcurl is limited to
// http,https. Headers come from Workspace::http_to_send() (inherited headers, Basic / Bearer /
// OAuth 2.0 / API key auth, cookies); Digest / NTLM / Negotiate / AWS SigV4 and client
// certificates are refused (they need a challenge round trip or a TLS identity the upgrade path
// does not do yet) instead of being dropped silently.
//
// Threading: connect() and run() on one worker thread (run() blocks until the connection ends);
// send(), close() and cancel() from any thread.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/http_client.h"

namespace watt {

struct WsMessage {
  bool out = false;     // sent by this client (false: received)
  bool binary = false;  // binary frame (data holds the raw bytes)
  std::string data;
  double at_ms = 0;     // since the connection opened (the 101 response arrived)
};

struct WsHandshake {
  bool ok = false;           // 101 with a valid Sec-WebSocket-Accept
  std::string error;         // why not (transport error, other status, bad accept key)
  long status = 0;           // the HTTP status of the upgrade response (101), 0 if none came
  std::string status_text;   // "Switching Protocols"
  std::vector<Header> headers;  // response headers
  std::string body;          // the body of a non-101 answer (up to 64 KB), for the error view
  std::string url;           // ws(s):// as connected
  std::string request_head;  // the upgrade request as sent (request line + headers)
  std::string subprotocol;   // Sec-WebSocket-Protocol the server chose ("" when none)
  std::string remote_ip, local_ip;
  long remote_port = 0, local_port = 0;
  std::string proxy;         // the proxy used ("" when direct), credentials left out
  bool tls = false;
  std::string tls_version;   // "TLSv1.3" (wss)
  // Phases, milliseconds: DNS, TCP connect, TLS handshake (wss), sending the upgrade request,
  // waiting for the 101 (upgrade), and the total from connect() to the open connection.
  double prepare_ms = 0, dns_ms = 0, connect_ms = 0, tls_ms = 0, send_ms = 0, upgrade_ms = 0, total_ms = 0;
  uint64_t bytes_sent = 0, bytes_received = 0;
};

struct WsClose {
  // 1000 normal ... as sent in the close frame; 1005: a close frame without a code; 1006: the
  // connection ended without a close frame (dropped, cancelled, transport error).
  int code = 1006;
  std::string reason;
  bool by_client = false;  // this side started the close
  std::string error;       // transport / protocol error, "cancelled"
  double at_ms = 0;        // since the connection opened
  uint64_t messages_in = 0, messages_out = 0, bytes_in = 0, bytes_out = 0;
};

class WsClient {
 public:
  WsClient();
  ~WsClient();
  WsClient(const WsClient&) = delete;
  WsClient& operator=(const WsClient&) = delete;

  // Connects and upgrades (blocking, on the worker). req.url: ws(s):// or http(s)://; headers,
  // timeout_ms (connect + handshake; 0 = 30 s), verify_tls, ca_pem, tls_min_version and the
  // proxy fields apply. On ok, call run() on the same thread.
  WsHandshake connect(const HttpRequest& req);
  // Receives until the connection ends. Messages in both directions are delivered to on_batch in
  // order, at most once every batch_ms (the first one after a quiet spell at once), and the rest
  // right before run() returns. Pings are answered; pings and pongs are not delivered.
  using BatchFn = std::function<void(std::vector<WsMessage>&&)>;
  WsClose run(const BatchFn& on_batch, int batch_ms = 50);

  // Any thread. Queues one message (text unless binary); false (with *err) when not open.
  bool send(std::string data, bool binary, std::string* err = nullptr);
  // Starts the close handshake (code 1000..4999, reason up to 123 bytes); run() returns when the
  // server answered, or after 3 s.
  void close(int code = 1000, std::string reason = {});
  // Ends at once (no close handshake), also during connect().
  void cancel();
  bool is_open() const;

  // Largest message accepted from the server; a bigger one closes with 1009.
  static constexpr uint64_t kMaxMessage = 256ull << 20;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Helpers (exposed for tests).
// base64(SHA-1(key + RFC 6455 GUID)).
std::string ws_accept_key(std::string_view key);
// ws:// -> http://, wss:// -> https://, http(s) unchanged (scheme case-insensitive); "" for any
// other scheme.
std::string ws_http_url(std::string_view url);
// http(s):// -> ws(s)://, unchanged otherwise.
std::string ws_display_url(std::string_view url);
// "Normal closure", "Going away", ... ("" for an unknown code).
const char* ws_close_code_name(int code);
// One frame. mask: 4 bytes, or nullptr for an unmasked (server) frame.
std::string ws_encode_frame(int opcode, std::string_view payload, bool fin, const unsigned char* mask);
// Incremental frame parser: feed bytes, take complete frames.
struct WsFrame {
  int opcode = 0;
  bool fin = true;
  std::string payload;  // unmasked
};
class WsFrameParser {
 public:
  void feed(const char* p, size_t n) { buf_.append(p, n); }
  // 1: a frame in *f; 0: needs more bytes; -1: protocol error (*err), e.g. too big.
  int next(WsFrame* f, std::string* err, uint64_t max_payload = WsClient::kMaxMessage);
  size_t buffered() const { return buf_.size() - pos_; }

 private:
  std::string buf_;
  size_t pos_ = 0;
};

}  // namespace watt
