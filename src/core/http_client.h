#pragma once
// REST engine. Thin, fast wrapper over libcurl. One HttpClient per worker thread; it keeps a
// persistent easy handle + share handle so DNS, TLS sessions and connections are reused across
// requests (that is what an engineer re-firing a request expects, and it is where Electron
// tools lose time). Thread-safety: an HttpClient instance is used by one thread at a time;
// cancel() may be called from any thread.
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/timing.h"

namespace watt {

using Header = std::pair<std::string, std::string>;

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
};

struct HttpResponse {
  bool ok = false;              // transport succeeded (any HTTP status)
  std::string error;            // transport error text when !ok
  long status = 0;
  std::string status_text;      // reason phrase if server sent one
  std::string http_version;     // "HTTP/1.1", "HTTP/2"
  std::vector<Header> headers;
  std::string body;
  std::string effective_url;
  std::string remote_ip;
  long remote_port = 0;
  Timing timing;
};

// Optional streaming hook: called on the worker thread for each received body chunk.
using BodyChunkFn = std::function<void(const char* data, size_t len)>;

class HttpClient {
 public:
  HttpClient();
  ~HttpClient();
  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  HttpResponse send(const HttpRequest& req, const BodyChunkFn& on_chunk = {});
  void cancel();  // aborts the in-flight send() ASAP; it returns with ok=false, error="cancelled"

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace watt
