#pragma once
// Per-workspace cookie jar (Postman's / Insomnia's / Bruno's): cookies a server sets with
// Set-Cookie (HTTP responses and gRPC-Web initial metadata) are kept per domain / path / name
// with their expiry, Secure, HttpOnly and SameSite attributes, and sent back on matching
// requests (Workspace::http_to_send() adds them; gRPC-Web through Workspace::add_grpc_web_cookies()).
//
// Rules (RFC 6265, with the RFC 6265bis hardening browsers use):
//   - Domain=: leading dot ignored; the request host must domain-match it; an IP host only gets
//     host-only cookies. A public suffix is never a cookie domain: a bare TLD ("com", "localhost"
//     unless it is the host itself) and a built-in list of common multi-label suffixes ("co.uk",
//     "github.io", ...) are rejected, or become host-only when equal to the request host.
//   - Path=: must start with '/', otherwise the default path (the request path up to its last '/').
//   - Max-Age wins over Expires; Max-Age <= 0 or an Expires in the past deletes the cookie.
//   - Secure cookies are only set from and sent to https (or loopback http: localhost, 127.x, ::1),
//     and an insecure origin cannot overwrite a Secure cookie of the same name.
//   - "__Secure-" needs Secure, "__Host-" needs Secure, no Domain= and Path=/.
//   - Names / values with control characters, and cookies over 4096 bytes, are ignored; at most
//     50 cookies per domain and 3000 in all (the least recently used go first).
//   - SameSite is stored and shown but does not filter: requests from an API tester are direct
//     (top-level, same-site) requests, as in Postman / Insomnia.
//   - Cookies set by a 3xx that libcurl followed are not seen (only the final response's headers).
//
// Persistence: <ws>/.watt/cookies.json (the owner-only folder; .watt/ is gitignored). The
// whole jar is one DPAPI-sealed blob (current user, as .watt/secrets.json): {"version": 1,
// "jar": "dpapi1:..."}. A file that does not unseal for this user (planted in a cloned repo,
// copied from another account or machine, or plaintext) is ignored: load_rejected() says so, and
// the next save replaces it. On POSIX the jar is plaintext JSON in the 0600 file.
// Changes are written by a background thread (coalesced), never by the caller; flush() waits.
//
// Thread-safe: every method may be called from any thread. The jar is loaded on first use
// (one small file read + one DPAPI call): call preload() from a worker before showing it.
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace watt {

class ResponseHeaders;

enum class SameSite { Unset, None, Lax, Strict };

struct Cookie {
  std::string name;
  std::string value;
  std::string domain;      // lower case, no leading dot
  std::string path = "/";
  bool host_only = true;   // no Domain= attribute: sent to exactly `domain`, not its subdomains
  bool secure = false;
  bool http_only = false;
  SameSite same_site = SameSite::Unset;
  int64_t expires = 0;     // Unix seconds (UTC); 0 = session cookie (kept until cleared)
  int64_t created = 0;     // Unix seconds, kept when a cookie is replaced
  int64_t last_access = 0;
  bool session() const { return expires == 0; }
  bool operator==(const Cookie&) const = default;
};

const char* same_site_name(SameSite s);  // "", "None", "Lax", "Strict"

// Helpers, exposed for tests and the cookie editor.
namespace cookies {
// RFC 6265 5.1.1 cookie-date ("Wed, 21 Oct 2015 07:28:00 GMT", also the RFC 850 / asctime
// forms). False when it is not a date.
bool parse_cookie_date(std::string_view s, int64_t& unix_seconds);
// RFC 6265 5.1.3: host == domain, or host ends with "." + domain and is not an IP address.
bool domain_match(std::string_view host, std::string_view domain);
// RFC 6265 5.1.4.
bool path_match(std::string_view request_path, std::string_view cookie_path);
// A bare TLD or one of the built-in multi-label public suffixes.
bool is_public_suffix(std::string_view domain);
int64_t now();
}  // namespace cookies

class CookieJar {
 public:
  // file empty: memory only (nothing is read or written).
  explicit CookieJar(std::filesystem::path file = {});
  ~CookieJar();
  CookieJar(const CookieJar&) = delete;
  CookieJar& operator=(const CookieJar&) = delete;

  const std::filesystem::path& file() const { return file_; }

  // One Set-Cookie header value received for request_url. True when the jar changed (a cookie
  // stored, replaced or deleted); false when it was ignored (*why says why, if given).
  bool set_from_header(std::string_view request_url, std::string_view set_cookie, std::string* why = nullptr,
                       int64_t now = cookies::now());
  // Every Set-Cookie header among the response headers (names compared case-insensitively).
  // Returns the number of cookies stored, replaced or deleted.
  size_t record(std::string_view request_url, const ResponseHeaders& headers, int64_t now = cookies::now());
  size_t record(std::string_view request_url, const std::vector<std::pair<std::string, std::string>>& headers,
                int64_t now = cookies::now());

  // The Cookie header value for a request to url ("a=1; b=2"; "" when none match): longer
  // paths first, then older cookies first (RFC 6265 5.4). Only http / https URLs match.
  std::string cookie_header(std::string_view url, int64_t now = cookies::now());

  // Editor API. list(): unexpired cookies sorted by domain, path, name. upsert(): adds or
  // replaces the cookie with the same name / domain / path (domain lowered, leading dot dropped);
  // false (and *why) for an empty domain, a public-suffix domain, a path not starting with '/',
  // or control characters. remove(): by identity. remove_domain(): that domain and its subdomains.
  std::vector<Cookie> list(int64_t now = cookies::now());
  bool upsert(Cookie c, std::string* why = nullptr);
  bool remove(std::string_view domain, std::string_view path, std::string_view name);
  size_t remove_domain(std::string_view domain);
  void clear();
  size_t size(int64_t now = cookies::now());

  // Loads the file now if it was not loaded yet (call from a worker thread).
  void preload();
  // Drops the in-memory state and reads the file again.
  void reload();
  // Why the file on disk was not used ("" when it was, or there is none): e.g. "cookies.json was
  // not sealed by this Windows user (planted or copied file): ignored".
  std::string load_rejected();
  // Waits until every change so far is on disk. False (and *err) when the write failed.
  bool flush(std::string* err = nullptr);

 private:
  void ensure_loaded_locked();
  void load_locked();
  void changed_locked();
  void writer_loop();
  void evict_locked(const std::string& domain);
  bool upsert_locked(Cookie c, std::string* why, bool from_http);

  std::filesystem::path file_;
  std::mutex mu_;
  bool loaded_ = false;
  std::string rejected_;
  std::vector<Cookie> cookies_;
  // Background writer state (guarded by mu_).
  uint64_t gen_ = 0, written_gen_ = 0;
  bool writer_running_ = false;
  std::string write_error_;
  std::condition_variable cv_;
};

// The jar of the workspace at root (<root>/.watt/cookies.json), one per process and root
// (paths compared after weakly_canonical, case-insensitively on Windows). Never null.
std::shared_ptr<CookieJar> cookie_jar_for(const std::filesystem::path& root);

namespace detail {
// Adds the jar's cookies for url to the Cookie header in headers (created when absent). Names
// already in that header are not added again (the request's own value wins). Used by
// Workspace::http_to_send() and add_grpc_web_cookies() (case-insensitive header name match).
void add_jar_cookies(CookieJar& jar, const std::string& url, std::vector<std::pair<std::string, std::string>>& headers);
}  // namespace detail

}  // namespace watt
