#pragma once
// OAuth 2.0 for HTTP requests (SavedRequest::auth with HttpAuth::Type::OAuth2, see OAuth2Config
// in core/model.h): client credentials and password grants, authorization code with PKCE
// (system browser + a one-shot listener on 127.0.0.1), refresh tokens, and an in-memory token
// cache with expiry.
//
// Cache: one token per (workspace root, active environment, request id or file, the expanded
// settings that identify a token: grant, token URL, client id, scope, audience, user name). It
// lives in this process only (tokens are never written to disk or to history); changing any of
// those settings or the environment asks for a new token. Thread-safe.
//
// Safety: the token and authorization URLs come from workspace files (maybe a cloned repo), so
// both must be http(s) URLs without control characters or spaces before anything is opened or
// sent; the browser is only ever handed such an URL (ShellExecuteW of an arbitrary string would
// run programs). Token requests go through watt::HttpClient (http/https only, TLS verified).
// The redirect listener binds 127.0.0.1 only, accepts one callback with the matching `state`
// (anything else is answered 400 and ignored), and closes as soon as it has the code or times out.
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "core/http_client.h"

namespace watt {

class Workspace;
struct SavedRequest;
struct OAuth2Config;

struct OAuth2Token {
  std::string access_token;
  std::string token_type;     // as the server said ("Bearer")
  std::string refresh_token;  // empty if none was issued
  std::string scope;
  std::string id_token;
  int64_t expires_at = 0;     // unix seconds; 0 = the server gave no expires_in (treated as valid)
  int64_t obtained_at = 0;    // unix seconds
  // Valid at `now` with `slack_s` seconds to spare.
  bool valid(int64_t now, int64_t slack_s = 10) const {
    return !access_token.empty() && (expires_at == 0 || now + slack_s < expires_at);
  }
};

struct OAuth2Result {
  bool ok = false;
  std::string error;       // "invalid_client: bad secret" (the server's error / error_description), ...
  OAuth2Token token;
  bool from_cache = false;
  bool refreshed = false;  // obtained with the refresh token
  double elapsed_ms = 0;
};

struct OAuth2Hooks {
  // Authorization code grant only. Empty and interactive = true: the system browser
  // (ShellExecuteW). Must not block until the user finished: the listener waits after it returns.
  std::function<bool(const std::string& url, std::string* err)> open_browser;
  // false (the default, and what Workspace::http_to_send() uses): never open a browser; an
  // authorization code grant without a cached / refreshable token fails with "authorize first".
  bool interactive = false;
  long authorize_timeout_ms = 300000;  // how long to wait for the browser redirect
  long request_timeout_ms = 30000;     // token endpoint requests
  const std::atomic<bool>* cancel = nullptr;
  HttpClient* client = nullptr;        // reused for token requests if given (else a local one)
  bool verify_tls = true;
  std::string ca_pem;                  // as HttpRequest::ca_pem, for a private token endpoint
};

// The cached token of request `r` (expanded with ws) if there is one, valid or not.
std::optional<OAuth2Token> oauth2_cached_token(const Workspace& ws, const SavedRequest& r);
// A valid token for `r`: the cached one, else refreshed (auto_refresh and a refresh token), else
// a new grant. force_new skips the cache and the refresh. Blocking: call it on a worker thread.
OAuth2Result oauth2_get_token(const Workspace& ws, const SavedRequest& r, const OAuth2Hooks& hooks = {},
                              bool force_new = false);
// Drops `r`'s cached token (e.g. "Clear token" in the UI) / every cached token.
void oauth2_forget(const Workspace& ws, const SavedRequest& r);
void oauth2_clear_cache();

// Adds the token to an outgoing request as `cfg` says: "Authorization: <prefix> <token>" (unless
// the request has its own Authorization header) or ?<query_param>=<token> appended to the URL.
// False with *err when the token or the parameter name cannot be sent safely.
bool oauth2_apply_token(HttpRequest& req, const OAuth2Config& cfg, const OAuth2Token& t, std::string* err = nullptr);

// Building blocks (exposed for tests).
std::string pkce_verifier();                                  // 43 chars, base64url of 32 random bytes
std::string pkce_challenge_s256(const std::string& verifier); // base64url(SHA-256(verifier)), no padding
std::string form_urlencode(const std::string& s);             // application/x-www-form-urlencoded value
// An absolute http:// or https:// URL with no control characters, spaces or quotes.
bool is_safe_http_url(const std::string& url);

}  // namespace watt
