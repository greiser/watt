// gRPC-Web front end of watt_testserver: a small HTTP/1.1 listener (plaintext, or TLS with the
// same certificate as the gRPC port) that translates gRPC-Web requests into calls on the server's
// in-process channel, the way Envoy's grpc_web filter or grpcwebproxy do:
//   POST /<service>/<method>, Content-Type application/grpc-web[+proto] or application/grpc-web-text
//   request body: 1 flag byte + 4-byte big-endian length + message, per message (base64 for -text)
//   response: HTTP headers carry the initial metadata; each message is one data frame (flag 0x00),
//   sent as its own HTTP chunk the moment the backend produced it; a trailer frame (flag 0x80)
//   carries "grpc-status: N\r\ngrpc-message: ...\r\n<trailing metadata>".
// Every request body is forwarded whole and then half-closed, so any method kind can be reached
// (clients use that for the bidi reflection stream).
// Test controls (request headers, never forwarded):
//   x-grpcweb-trailers-only: 1   a call that ended without messages answers trailers-only: the
//                                status is in the HTTP headers and the body is empty.
//   x-grpcweb-http-status: N     answer with that HTTP status and no gRPC framing at all.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace {

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64_encode(const std::string& in) {
  std::string out;
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
    out += kB64[v >> 18];
    out += kB64[(v >> 12) & 63];
    out += kB64[(v >> 6) & 63];
    out += kB64[v & 63];
  }
  if (i < in.size()) {
    uint32_t v = uint8_t(in[i]) << 16;
    if (i + 1 < in.size()) v |= uint8_t(in[i + 1]) << 8;
    out += kB64[v >> 18];
    out += kB64[(v >> 12) & 63];
    out += i + 1 < in.size() ? kB64[(v >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

// Decodes base64 (standard or URL alphabet); '=' padding may appear between segments.
bool b64_decode(const std::string& in, std::string& out) {
  out.clear();
  uint32_t acc = 0;
  int bits = 0;
  for (char c : in) {
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+' || c == '-') v = 62;
    else if (c == '/' || c == '_') v = 63;
    else if (c == '=') {
      acc = 0;
      bits = 0;
      continue;
    } else if (c == '\r' || c == '\n' || c == ' ') continue;
    else return false;
    acc = (acc << 6) | uint32_t(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += char((acc >> bits) & 0xFF);
    }
  }
  return true;
}

std::string frame(uint8_t flag, const std::string& payload) {
  std::string f(5, '\0');
  f[0] = char(flag);
  const uint32_t n = static_cast<uint32_t>(payload.size());
  f[1] = char(n >> 24);
  f[2] = char(n >> 16);
  f[3] = char(n >> 8);
  f[4] = char(n);
  return f + payload;
}

// grpc-message is percent-encoded (gRPC over HTTP/2 spec).
std::string percent_encode(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    if (c < 0x20 || c > 0x7E || c == '%') {
      char b[4];
      std::snprintf(b, sizeof b, "%%%02X", c);
      out += b;
    } else {
      out += char(c);
    }
  }
  return out;
}

// One accepted connection, plaintext or TLS.
class Conn {
 public:
  Conn(SOCKET s, SSL* ssl) : s_(s), ssl_(ssl) {}
  ~Conn() {
    if (ssl_) {
      SSL_shutdown(ssl_);
      SSL_free(ssl_);
    }
    closesocket(s_);
  }
  int read(char* buf, int n) {
    if (ssl_) return SSL_read(ssl_, buf, n);
    return recv(s_, buf, n, 0);
  }
  bool write(const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
      const int chunk = static_cast<int>(std::min<size_t>(data.size() - off, 1 << 20));
      const int w = ssl_ ? SSL_write(ssl_, data.data() + off, chunk) : send(s_, data.data() + off, chunk, 0);
      if (w <= 0) return false;
      off += static_cast<size_t>(w);
    }
    return true;
  }

 private:
  SOCKET s_;
  SSL* ssl_;
};

struct Request {
  std::string method, path;
  std::vector<std::pair<std::string, std::string>> headers;  // keys lowercased
  std::string body;
  std::string get(const std::string& k) const {
    for (const auto& [n, v] : headers)
      if (n == k) return v;
    return {};
  }
};

// Reads one HTTP/1.1 request (Content-Length or chunked body). False on EOF / malformed.
bool read_request(Conn& c, std::string& buf, Request& r) {
  auto fill = [&]() {
    char tmp[16384];
    const int n = c.read(tmp, sizeof tmp);
    if (n <= 0) return false;
    buf.append(tmp, static_cast<size_t>(n));
    return true;
  };
  size_t end;
  while ((end = buf.find("\r\n\r\n")) == std::string::npos)
    if (!fill()) return false;
  const std::string head = buf.substr(0, end);
  buf.erase(0, end + 4);
  size_t line_end = head.find("\r\n");
  const std::string first = head.substr(0, line_end);
  const size_t sp1 = first.find(' '), sp2 = first.find(' ', sp1 + 1);
  if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
  r = Request{};
  r.method = first.substr(0, sp1);
  r.path = first.substr(sp1 + 1, sp2 - sp1 - 1);
  size_t pos = line_end == std::string::npos ? head.size() : line_end + 2;
  while (pos < head.size()) {
    size_t e = head.find("\r\n", pos);
    if (e == std::string::npos) e = head.size();
    const std::string line = head.substr(pos, e - pos);
    pos = e + 2;
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string v = line.substr(colon + 1);
    while (!v.empty() && v.front() == ' ') v.erase(0, 1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
    r.headers.emplace_back(lower(line.substr(0, colon)), v);
  }
  if (lower(r.get("transfer-encoding")).find("chunked") != std::string::npos) {
    for (;;) {
      size_t le;
      while ((le = buf.find("\r\n")) == std::string::npos)
        if (!fill()) return false;
      const size_t n = std::strtoul(buf.substr(0, le).c_str(), nullptr, 16);
      buf.erase(0, le + 2);
      while (buf.size() < n + 2)
        if (!fill()) return false;
      r.body.append(buf, 0, n);
      buf.erase(0, n + 2);
      if (n == 0) break;
    }
  } else {
    const size_t len = std::strtoul(r.get("content-length").c_str(), nullptr, 10);
    while (buf.size() < len)
      if (!fill()) return false;
    r.body = buf.substr(0, len);
    buf.erase(0, len);
  }
  return true;
}

const char* kCors =
    "Access-Control-Allow-Origin: *\r\n"
    "Access-Control-Expose-Headers: *\r\n";

void respond_plain(Conn& c, int status, const std::string& reason, const std::string& extra = {}) {
  c.write("HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n" + kCors + extra +
          "Content-Length: 0\r\n\r\n");
}

// grpc-timeout: up to 8 digits + unit (H M S m u n).
bool parse_timeout(const std::string& v, std::chrono::nanoseconds& out) {
  if (v.size() < 2) return false;
  const long long n = std::atoll(v.substr(0, v.size() - 1).c_str());
  switch (v.back()) {
    case 'H': out = std::chrono::hours(n); return true;
    case 'M': out = std::chrono::minutes(n); return true;
    case 'S': out = std::chrono::seconds(n); return true;
    case 'm': out = std::chrono::milliseconds(n); return true;
    case 'u': out = std::chrono::microseconds(n); return true;
    case 'n': out = std::chrono::nanoseconds(n); return true;
  }
  return false;
}

bool is_bin(const std::string& k) { return k.size() > 4 && k.compare(k.size() - 4, 4, "-bin") == 0; }

std::string md_value(const std::string& k, grpc::string_ref v) {
  std::string s(v.data(), v.size());
  return is_bin(k) ? b64_encode(s) : s;
}

// Headers that belong to HTTP or to this proxy, not to the call's metadata.
bool hop_header(const std::string& k) {
  static const char* skip[] = {"host", "content-type", "content-length", "accept", "accept-encoding",
                               "user-agent", "x-grpc-web", "x-user-agent", "connection", "te",
                               "grpc-timeout", "expect", "transfer-encoding", "origin", "referer",
                               "x-grpcweb-trailers-only", "x-grpcweb-http-status", "grpc-accept-encoding"};
  for (const char* s : skip)
    if (k == s) return true;
  return false;
}

class Proxy {
 public:
  Proxy(std::shared_ptr<grpc::Channel> ch, SSL_CTX* tls) : ch_(std::move(ch)), tls_(tls) {}

  void serve(SOCKET s) {
    SSL* ssl = nullptr;
    if (tls_) {
      ssl = SSL_new(tls_);
      SSL_set_fd(ssl, static_cast<int>(s));
      if (SSL_accept(ssl) <= 0) {
        SSL_free(ssl);
        closesocket(s);
        return;
      }
    }
    Conn c(s, ssl);
    std::string buf;
    Request r;
    while (read_request(c, buf, r)) {
      if (!handle(c, r)) break;
      if (lower(r.get("connection")) == "close") break;
    }
  }

 private:
  // False when the connection must be closed.
  bool handle(Conn& c, const Request& r) {
    if (r.method == "OPTIONS") {
      respond_plain(c, 204, "No Content",
                    "Access-Control-Allow-Methods: POST, OPTIONS\r\nAccess-Control-Allow-Headers: *\r\n");
      return true;
    }
    if (r.method != "POST") {
      respond_plain(c, 405, "Method Not Allowed");
      return true;
    }
    const std::string forced = r.get("x-grpcweb-http-status");
    if (!forced.empty()) {
      respond_plain(c, std::atoi(forced.c_str()), "Forced");
      return true;
    }
    const std::string ct = lower(r.get("content-type"));
    const bool text = ct.rfind("application/grpc-web-text", 0) == 0;
    if (!text && ct.rfind("application/grpc-web", 0) != 0) {
      respond_plain(c, 415, "Unsupported Media Type");
      return true;
    }
    std::string body = r.body;
    if (text && !b64_decode(r.body, body)) {
      respond_plain(c, 400, "Bad Request");
      return true;
    }
    std::vector<std::string> msgs;
    for (size_t off = 0; off < body.size();) {
      if (body.size() - off < 5) {
        respond_plain(c, 400, "Bad Request");
        return true;
      }
      const uint32_t n = (uint8_t(body[off + 1]) << 24) | (uint8_t(body[off + 2]) << 16) |
                         (uint8_t(body[off + 3]) << 8) | uint8_t(body[off + 4]);
      if (body.size() - off - 5 < n) {
        respond_plain(c, 400, "Bad Request");
        return true;
      }
      msgs.push_back(body.substr(off + 5, n));
      off += 5 + n;
    }

    grpc::ClientContext ctx;
    for (const auto& [k, v] : r.headers) {
      if (hop_header(k)) continue;
      if (is_bin(k)) {
        std::string raw;
        ctx.AddMetadata(k, b64_decode(v, raw) ? raw : v);
      } else {
        ctx.AddMetadata(k, v);
      }
    }
    std::chrono::nanoseconds timeout{};
    if (parse_timeout(r.get("grpc-timeout"), timeout))
      ctx.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::duration_cast<std::chrono::system_clock::duration>(timeout));
    const bool trailers_only = r.get("x-grpcweb-trailers-only") == "1";

    grpc::CompletionQueue cq;
    grpc::GenericStub stub(ch_);
    auto rw = stub.PrepareCall(&ctx, r.path, &cq);
    // Writes and reads run at once: a bidi backend (reflection) answers each request before it
    // reads the next, so writing everything before reading could stall on flow control.
    enum : intptr_t { kStart = 1, kWrite, kWritesDone, kRead, kFinish };
    auto tag = [](intptr_t v) { return reinterpret_cast<void*>(v); };
    bool headers_sent = false, conn_ok = true;
    auto send_headers = [&]() {
      std::string h = "HTTP/1.1 200 OK\r\n";
      h += kCors;
      h += std::string("Content-Type: ") + (text ? "application/grpc-web-text" : "application/grpc-web+proto") + "\r\n";
      h += "Transfer-Encoding: chunked\r\n";
      for (const auto& [k, v] : ctx.GetServerInitialMetadata()) {
        const std::string key(k.data(), k.size());
        h += key + ": " + md_value(key, v) + "\r\n";
      }
      h += "\r\n";
      headers_sent = true;
      conn_ok = c.write(h);
    };
    auto send_chunk = [&](const std::string& f) {
      const std::string payload = text ? b64_encode(f) : f;
      char len[32];
      std::snprintf(len, sizeof len, "%zx\r\n", payload.size());
      conn_ok = conn_ok && c.write(len + payload + "\r\n");
    };
    int messages = 0;
    size_t next = 0;
    int pending = 0;
    grpc::ByteBuffer out, in;
    auto write_next = [&]() {
      if (next < msgs.size()) {
        grpc::Slice sl(msgs[next++]);
        out = grpc::ByteBuffer(&sl, 1);
        rw->Write(out, tag(kWrite));
      } else {
        rw->WritesDone(tag(kWritesDone));
      }
      ++pending;
    };
    rw->StartCall(tag(kStart));
    ++pending;
    void* got;
    bool ok;
    while (pending > 0 && cq.Next(&got, &ok)) {
      --pending;
      switch (reinterpret_cast<intptr_t>(got)) {
        case kStart:
          if (!ok) break;
          write_next();
          rw->Read(&in, tag(kRead));
          ++pending;
          break;
        case kWrite:
          if (ok) write_next();
          break;
        case kWritesDone:
          break;
        case kRead: {
          if (!ok) break;
          if (!headers_sent) send_headers();
          std::vector<grpc::Slice> slices;
          std::string m;
          if (in.Dump(&slices).ok())
            for (auto& sl : slices) m.append(reinterpret_cast<const char*>(sl.begin()), sl.size());
          send_chunk(frame(0x00, m));
          ++messages;
          if (!conn_ok) {
            ctx.TryCancel();
            break;
          }
          rw->Read(&in, tag(kRead));
          ++pending;
          break;
        }
      }
    }
    grpc::Status st;
    rw->Finish(&st, tag(kFinish));
    while (cq.Next(&got, &ok) && got != tag(kFinish)) {
    }
    if (!conn_ok) return false;

    std::string trailer = "grpc-status: " + std::to_string(static_cast<int>(st.error_code())) + "\r\n";
    if (!st.error_message().empty()) trailer += "grpc-message: " + percent_encode(st.error_message()) + "\r\n";
    if (!st.error_details().empty()) trailer += "grpc-status-details-bin: " + b64_encode(st.error_details()) + "\r\n";
    for (const auto& [k, v] : ctx.GetServerTrailingMetadata()) {
      const std::string key(k.data(), k.size());
      trailer += key + ": " + md_value(key, v) + "\r\n";
    }
    if (!headers_sent && trailers_only && messages == 0) {
      std::string h = "HTTP/1.1 200 OK\r\n";
      h += kCors;
      h += std::string("Content-Type: ") + (text ? "application/grpc-web-text" : "application/grpc-web+proto") + "\r\n";
      for (const auto& [k, v] : ctx.GetServerInitialMetadata()) {
        const std::string key(k.data(), k.size());
        h += key + ": " + md_value(key, v) + "\r\n";
      }
      h += trailer;
      h += "Content-Length: 0\r\n\r\n";
      return c.write(h);
    }
    if (!headers_sent) send_headers();
    send_chunk(frame(0x80, trailer));
    return conn_ok && c.write("0\r\n\r\n");
  }

  std::shared_ptr<grpc::Channel> ch_;
  SSL_CTX* tls_;
};

}  // namespace

// Starts the gRPC-Web listener on 127.0.0.1:port (0 = any free port) in background threads.
// tls_dir non-empty: TLS with tls_dir/server.pem + server.key. Returns the bound port, 0 on failure.
int start_grpc_web(int port, std::shared_ptr<grpc::Channel> backend, const std::string& tls_dir, std::string& err) {
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  SSL_CTX* tls = nullptr;
  if (!tls_dir.empty()) {
    tls = SSL_CTX_new(TLS_server_method());
    if (!tls || SSL_CTX_use_certificate_chain_file(tls, (tls_dir + "/server.pem").c_str()) != 1 ||
        SSL_CTX_use_PrivateKey_file(tls, (tls_dir + "/server.key").c_str(), SSL_FILETYPE_PEM) != 1) {
      err = "cannot load " + tls_dir + "/server.pem / server.key for the gRPC-Web listener";
      return 0;
    }
  }
  SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (l == INVALID_SOCKET) {
    err = "socket() failed";
    return 0;
  }
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(static_cast<u_short>(port));
  BOOL excl = TRUE;
  setsockopt(l, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
  if (bind(l, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(l, SOMAXCONN) != 0) {
    err = "cannot listen on 127.0.0.1:" + std::to_string(port);
    closesocket(l);
    return 0;
  }
  int len = sizeof a;
  getsockname(l, reinterpret_cast<sockaddr*>(&a), &len);
  auto proxy = std::make_shared<Proxy>(std::move(backend), tls);
  std::thread([l, proxy] {
    for (;;) {
      SOCKET s = accept(l, nullptr, nullptr);
      if (s == INVALID_SOCKET) continue;
      BOOL nd = TRUE;
      setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof nd);
      std::thread([proxy, s] { proxy->serve(s); }).detach();
    }
  }).detach();
  return ntohs(a.sin_port);
}
