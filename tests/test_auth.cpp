// HTTP authentication beyond Basic (core/model.h HttpAuth, core/http_client.h, core/oauth2.h)
// against the in-process watt_httpserver: Digest end to end, NTLM / Negotiate up to the first
// SSPI token (the test server does not complete the handshake), implicit Windows SSO consent,
// AWS SigV4 headers, OAuth 2.0 grants, refresh, caching and the authorization code flow with PKCE.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <winsock2.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <rapidjson/document.h>

#include "../tools/httpserver/http_server.h"
#include "core/http_client.h"
#include "core/model.h"
#include "core/oauth2.h"

using namespace watt;
namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;

namespace {

fs::path temp_path(const char* prefix) {
  std::random_device rd;
  return fs::temp_directory_path() / (std::string(prefix) + std::to_string(rd()) + std::to_string(rd()));
}

struct TestConfigDir {
  fs::path dir = temp_path("watt_test_auth_cfg_");
  TestConfigDir() { Workspace::set_user_config_dir(dir); }
  ~TestConfigDir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
} test_config_dir;

httpserver::Server& server() {
  static httpserver::Server s;
  static const bool started = s.start(0);
  REQUIRE(started);
  return s;
}
std::string port() { return std::to_string(server().port()); }
std::string url(const std::string& path) { return "http://127.0.0.1:" + port() + path; }

struct TempWs {
  fs::path root = temp_path("watt_test_auth_ws_");
  Workspace ws;
  TempWs() : ws(Workspace::create(root)) {
    Environment dev;
    dev.name = "dev";
    dev.vars["host"] = "http://127.0.0.1:" + port();
    dev.vars["client_secret"] = "s3cret";
    dev.vars["digest_pass"] = "p@ss w0rd";
    dev.vars["aws_secret"] = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
    dev.secrets = {"client_secret", "digest_pass", "aws_secret"};
    ws.environments().push_back(dev);
    ws.active_env = "dev";
    REQUIRE(ws.save_settings());
  }
  ~TempWs() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

int stat_of(const char* grant) {
  HttpClient c;
  HttpRequest r;
  r.url = url("/oauth/stats");
  const HttpResponse resp = c.send(r);
  rapidjson::Document d;
  d.Parse(resp.body.c_str());
  if (!d.IsObject() || !d.HasMember("grants") || !d["grants"].HasMember(grant)) return 0;
  return d["grants"][grant].GetInt();
}

HttpResponse send(const Workspace& ws, const SavedRequest& r, std::string* err = nullptr) {
  std::string e;
  auto http = ws.http_to_send(r, &e);
  if (err) *err = e;
  if (!http) return {};
  HttpClient c;
  return c.send(*http);
}

SavedRequest oauth_req(OAuth2Config::Grant g, const std::string& id) {
  SavedRequest r;
  r.id = id;
  r.http.url = "{{host}}/oauth/protected";
  r.auth.type = HttpAuth::Type::OAuth2;
  r.auth.oauth2.grant = g;
  r.auth.oauth2.token_url = "{{host}}/oauth/token";
  r.auth.oauth2.auth_url = "{{host}}/oauth/authorize";
  r.auth.oauth2.client_id = "watt-client";
  r.auth.oauth2.client_secret = "{{client_secret}}";
  r.auth.oauth2.scope = "read write";
  return r;
}

}  // namespace

// ------------------------------------------------------------------ Digest

TEST_CASE("Digest auth answers the challenge (password from a secret variable)", "[auth][digest]") {
  TempWs t;
  SavedRequest r;
  r.auth.type = HttpAuth::Type::Digest;
  r.auth.user = "bob";
  r.auth.password = "{{digest_pass}}";
  // The test server takes the expected password from the path.
  r.http.url = "{{host}}/digest-auth/bob/pw?x=1";
  t.ws.environments()[0].vars["digest_pass"] = "pw";
  std::string err;
  HttpResponse resp = send(t.ws, r, &err);
  INFO(err << resp.error << resp.body);
  CHECK(resp.status == 200);
  CHECK_THAT(resp.body, ContainsSubstring("\"authenticated\":true"));
  r.auth.password = "wrong";
  CHECK(send(t.ws, r).status == 401);
  r.auth.user.clear();
  CHECK_FALSE(t.ws.http_to_send(r, &err));
  CHECK_THAT(err, ContainsSubstring("Digest auth needs a user name"));
}

// ------------------------------------------------------------------ NTLM / Negotiate (unit level)

TEST_CASE("NTLM with explicit credentials starts the SSPI handshake, no consent needed", "[auth][ntlm]") {
  TempWs t;
  SavedRequest r;
  r.http.url = "{{host}}/ntlm-probe";
  r.auth.type = HttpAuth::Type::Ntlm;
  r.auth.user = "CORP\\alice";
  r.auth.password = "pw";
  CHECK(t.ws.blocked_sso_hosts(r).empty());
  const HttpResponse resp = send(t.ws, r);
  INFO(resp.error << resp.body);
  REQUIRE(resp.status == 200);
  // NTLMSSP negotiate message (type 1): base64 of "NTLMSSP\0" + 01 00 00 00.
  CHECK_THAT(resp.body, ContainsSubstring("\"token\":\"TlRMTVNTUAABAAAA"));
}

TEST_CASE("implicit Windows SSO needs per-user consent per origin", "[auth][ntlm][sso]") {
  TempWs t;
  SavedRequest r;
  r.http.url = "{{host}}/ntlm-probe";
  r.auth.type = HttpAuth::Type::Ntlm;  // no user: the current Windows logon
  const std::string origin = "http://127.0.0.1:" + port();

  // Blocked without opt-in: nothing is sent.
  REQUIRE(t.ws.blocked_sso_hosts(r) == std::vector<std::string>{origin});
  std::string err;
  CHECK_FALSE(t.ws.http_to_send(r, &err));
  CHECK_THAT(err, ContainsSubstring(origin));
  CHECK_THAT(err, ContainsSubstring("Windows account"));

  // A planted allowlist inside the workspace is ignored.
  const std::string planted = "{\"version\":1,\"workspaces\":{\"" + t.root.generic_string() + "\":[\"" + origin + "\"]}}";
  for (const fs::path& p : {t.root / ".watt" / "allowed_sso.json", t.root / "allowed_sso.json"}) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << planted;
  }
  auto reopened = Workspace::open(t.root);
  REQUIRE(reopened);
  CHECK(reopened->blocked_sso_hosts(r) == std::vector<std::string>{origin});
  CHECK_FALSE(reopened->http_to_send(r));

  // Allowed after the user's consent (stored in the per-user config, keyed by the root).
  REQUIRE(t.ws.allow_sso_host(url("/anything?x")));  // normalised to the origin
  CHECK(t.ws.allowed_sso_hosts() == std::vector<std::string>{origin});
  CHECK(fs::exists(Workspace::user_config_dir() / "allowed_sso.json"));
  CHECK(t.ws.blocked_sso_hosts(r).empty());
  const HttpResponse ok = send(t.ws, r, &err);
  INFO(err << ok.error << ok.body);
  REQUIRE(ok.status == 200);
  CHECK_THAT(ok.body, ContainsSubstring("\"token\":\"TlRMTVNTUAAB"));

  // Another host (same server, other name / port) is still blocked.
  SavedRequest other = r;
  other.http.url = "http://localhost:" + port() + "/ntlm-probe";
  CHECK(t.ws.blocked_sso_hosts(other) == std::vector<std::string>{"http://localhost:" + port()});
  CHECK_FALSE(t.ws.http_to_send(other));
  other.http.url = "https://127.0.0.1:" + port() + "/ntlm-probe";
  CHECK_FALSE(t.ws.blocked_sso_hosts(other).empty());
  other.http.url = "http://127.0.0.1:1/ntlm-probe";
  CHECK_FALSE(t.ws.blocked_sso_hosts(other).empty());
  // Negotiate shares the gate.
  SavedRequest neg = r;
  neg.auth.type = HttpAuth::Type::Negotiate;
  neg.http.url = "http://localhost:" + port() + "/negotiate-probe";
  CHECK_FALSE(t.ws.blocked_sso_hosts(neg).empty());

  // An allowed origin that redirects elsewhere: the other host gets no credentials.
  SavedRequest redir = r;
  redir.http.url = "{{host}}/status/302?location=http://localhost:" + port() + "/ntlm-probe";
  const HttpResponse rr = send(t.ws, redir, &err);
  INFO(err << rr.error << rr.body);
  CHECK(rr.redirect_count == 1);
  CHECK(rr.status == 401);
  // Also with explicit credentials (curl's CURLOPT_UNRESTRICTED_AUTH stays off).
  redir.auth.user = "alice";
  redir.auth.password = "pw";
  CHECK(send(t.ws, redir).status == 401);

  REQUIRE(t.ws.disallow_sso_host(origin));
  CHECK_FALSE(t.ws.blocked_sso_hosts(r).empty());
}

TEST_CASE("url_origin normalises origins", "[auth][sso]") {
  CHECK(Workspace::url_origin("HTTPS://Example.COM/a?b") == "https://example.com:443");
  CHECK(Workspace::url_origin("http://host.:8080#x") == "http://host:8080");
  // Parser differentials: the origin is what libcurl will connect to, or nothing at all.
  CHECK(Workspace::url_origin("http://user:pw@host:8080/").empty());       // user info: refused
  CHECK(Workspace::url_origin("http://good.test\\@evil.test/").empty());   // backslash: refused
  CHECK(Workspace::url_origin("http://good.test\\evil.test/").empty());
  CHECK(Workspace::url_origin("http://evil.test#@good.test/") == "http://evil.test:80");
  CHECK(Workspace::url_origin("http://evil.test?@good.test/") == "http://evil.test:80");
  CHECK(Workspace::url_origin("http://evil.test/@good.test/") == "http://evil.test:80");
  CHECK(Workspace::url_origin("http://good.test @evil.test/").empty());    // space
  CHECK(Workspace::url_origin(std::string("http://good.test\0@evil.test/", 29)).empty());
  CHECK(Workspace::url_origin("http://[::1]/") == "http://[::1]:80");
  CHECK(Workspace::url_origin("file:///c:/x").empty());
  CHECK(Workspace::url_origin("http://{{host}}/").empty());
  CHECK(Workspace::url_origin("http://h:99999/").empty());
}

TEST_CASE("Negotiate starts SPNEGO via SSPI (unit level: no KDC here)", "[auth][negotiate]") {
  TempWs t;
  SavedRequest r;
  r.http.url = "{{host}}/negotiate-probe";
  r.auth.type = HttpAuth::Type::Negotiate;
  r.auth.user = "alice";
  r.auth.password = "pw";
  const HttpResponse resp = send(t.ws, r);
  // Without a domain controller SSPI may or may not produce a token for HTTP/127.0.0.1 (it falls
  // back to NTLM inside SPNEGO on most machines); either way the request must not hang or crash.
  std::printf("[info] Negotiate probe: status %ld, body %s\n", resp.status, resp.body.substr(0, 80).c_str());
  CHECK((resp.status == 200 || resp.status == 401));
  if (resp.status == 200) CHECK_THAT(resp.body, ContainsSubstring("\"scheme\":\"Negotiate\""));
}

// ------------------------------------------------------------------ AWS SigV4

TEST_CASE("AWS Signature v4 signs the request (secret from a secret variable)", "[auth][aws]") {
  TempWs t;
  SavedRequest r;
  r.http.method = "POST";
  r.http.url = "{{host}}/echo?b=2&a=1";
  r.http.body = R"({"x":1})";
  r.auth.type = HttpAuth::Type::AwsSigV4;
  r.auth.aws_access_key = "AKIDEXAMPLE";
  r.auth.aws_secret_key = "{{aws_secret}}";
  r.auth.aws_session_token = "FQoGZXIvYXdzE//session";
  r.auth.aws_region = "eu-west-1";
  r.auth.aws_service = "execute-api";
  std::string err;
  const HttpResponse resp = send(t.ws, r, &err);
  INFO(err << resp.error << resp.body);
  REQUIRE(resp.status == 200);
  rapidjson::Document d;
  d.Parse(resp.body.c_str());
  REQUIRE(d.IsObject());
  const auto& h = d["headers"];
  REQUIRE(h.HasMember("Authorization"));
  const std::string auth = h["Authorization"].GetString();
  CHECK_THAT(auth, StartsWith("AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/"));
  CHECK_THAT(auth, ContainsSubstring("/eu-west-1/execute-api/aws4_request"));
  CHECK_THAT(auth, ContainsSubstring("x-amz-security-token"));
  CHECK_THAT(auth, ContainsSubstring("Signature="));
  CHECK(auth.substr(auth.find("Signature=") + 10).size() == 64);
  CHECK(h.HasMember("X-Amz-Date"));
  CHECK(std::string(h["X-Amz-Security-Token"].GetString()) == "FQoGZXIvYXdzE//session");
  CHECK(auth.find("wJalr") == std::string::npos);  // the secret never leaves

  r.auth.aws_region = "eu:west";
  CHECK_FALSE(t.ws.http_to_send(r, &err));
  CHECK_THAT(err, ContainsSubstring("region"));
}

// ------------------------------------------------------------------ OAuth 2.0

TEST_CASE("OAuth 2.0 client credentials: fetched by http_to_send, cached, sent as Bearer", "[auth][oauth2]") {
  TempWs t;
  oauth2_clear_cache();
  SavedRequest r = oauth_req(OAuth2Config::Grant::ClientCredentials, "cc-1");
  const int before = stat_of("client_credentials");
  std::string err;
  HttpResponse a = send(t.ws, r, &err);
  INFO(err << a.body);
  CHECK(a.status == 200);
  CHECK_THAT(a.body, ContainsSubstring("tok-client_credentials-"));
  CHECK(send(t.ws, r).status == 200);
  CHECK(stat_of("client_credentials") == before + 1);  // the second send used the cache
  auto cached = oauth2_cached_token(t.ws, r);
  REQUIRE(cached);
  CHECK(cached->scope == "read write");
  CHECK(cached->expires_at > cached->obtained_at);

  // Another environment is another cache entry.
  Environment prod = t.ws.environments()[0];
  prod.name = "prod";
  t.ws.environments().push_back(prod);
  t.ws.active_env = "prod";
  CHECK_FALSE(oauth2_cached_token(t.ws, r));
  t.ws.active_env = "dev";

  // Client secret in the body instead of a Basic header; token in the query.
  SavedRequest q = oauth_req(OAuth2Config::Grant::ClientCredentials, "cc-2");
  q.auth.oauth2.client_auth = OAuth2Config::ClientAuth::Body;
  q.auth.oauth2.add_to = OAuth2Config::AddTo::Query;
  auto http = t.ws.http_to_send(q, &err);
  REQUIRE(http);
  CHECK_THAT(http->url, ContainsSubstring("/oauth/protected?access_token=tok-client_credentials-"));
  for (const auto& [k, v] : http->headers) CHECK(k != "Authorization");
  HttpClient c;
  CHECK(c.send(*http).status == 200);

  // A wrong secret fails the send with the server's error.
  SavedRequest bad = oauth_req(OAuth2Config::Grant::ClientCredentials, "cc-3");
  bad.auth.oauth2.client_secret = "nope";
  CHECK_FALSE(t.ws.http_to_send(bad, &err));
  CHECK_THAT(err, ContainsSubstring("invalid_client"));
  // A non-http token URL is refused before anything is sent.
  bad.auth.oauth2.client_secret = "{{client_secret}}";
  bad.auth.oauth2.token_url = "file:///C:/Windows/win.ini";
  CHECK_FALSE(t.ws.http_to_send(bad, &err));
  CHECK_THAT(err, ContainsSubstring("not an http(s) URL"));
}

TEST_CASE("OAuth 2.0 password grant: expiry and refresh", "[auth][oauth2]") {
  TempWs t;
  oauth2_clear_cache();
  SavedRequest r = oauth_req(OAuth2Config::Grant::Password, "pw-1");
  r.auth.oauth2.token_url = "{{host}}/oauth/token?expires_in=5";  // inside the 10 s safety margin
  r.auth.oauth2.username = "alice";
  r.auth.oauth2.password = "wonderland";
  const OAuth2Result first = oauth2_get_token(t.ws, r);
  INFO(first.error);
  REQUIRE(first.ok);
  CHECK_FALSE(first.from_cache);
  CHECK_FALSE(first.token.refresh_token.empty());
  const int refreshes = stat_of("refresh_token");
  const OAuth2Result second = oauth2_get_token(t.ws, r);  // "expired": refreshed
  REQUIRE(second.ok);
  CHECK(second.refreshed);
  CHECK(second.token.access_token != first.token.access_token);
  CHECK(second.token.refresh_token == first.token.refresh_token);  // kept when not reissued
  CHECK(stat_of("refresh_token") == refreshes + 1);
  // force_new skips cache and refresh.
  const OAuth2Result forced = oauth2_get_token(t.ws, r, {}, true);
  REQUIRE(forced.ok);
  CHECK_FALSE(forced.refreshed);

  r.auth.oauth2.password = "wrong";
  oauth2_forget(t.ws, r);
  const OAuth2Result bad = oauth2_get_token(t.ws, r);
  CHECK_FALSE(bad.ok);
  CHECK_THAT(bad.error, ContainsSubstring("invalid_grant"));
}

TEST_CASE("OAuth 2.0 authorization code with PKCE through a loopback redirect", "[auth][oauth2][pkce]") {
  TempWs t;
  oauth2_clear_cache();
  SavedRequest r = oauth_req(OAuth2Config::Grant::AuthorizationCode, "ac-1");

  // Never started by a send: it needs a browser.
  std::string err;
  CHECK_FALSE(t.ws.http_to_send(r, &err));
  CHECK_THAT(err, ContainsSubstring("authorize first"));

  std::vector<std::thread> browsers;
  std::string seen_url;
  int wrong_state_status = 0;
  OAuth2Hooks hooks;
  hooks.interactive = true;
  hooks.authorize_timeout_ms = 10000;
  hooks.open_browser = [&](const std::string& u, std::string*) {
    seen_url = u;
    browsers.emplace_back([&, u] {
      // A stray callback with the wrong state first: refused, the wait goes on.
      const size_t p = u.find("redirect_uri=") + 13;
      std::string redirect = u.substr(p, u.find('&', p) - p);
      for (size_t i; (i = redirect.find("%3A")) != std::string::npos;) redirect.replace(i, 3, ":");
      for (size_t i; (i = redirect.find("%2F")) != std::string::npos;) redirect.replace(i, 3, "/");
      HttpClient c;
      HttpRequest evil;
      evil.url = redirect + "?code=evil&state=forged";
      wrong_state_status = c.send(evil).status;
      HttpRequest go;
      go.url = u;  // the server redirects to the loopback listener with code + state
      c.send(go);
    });
    return true;
  };
  const OAuth2Result res = oauth2_get_token(t.ws, r, hooks);
  for (auto& b : browsers) b.join();
  INFO(res.error);
  REQUIRE(res.ok);
  CHECK(wrong_state_status == 400);
  CHECK_THAT(seen_url, ContainsSubstring("code_challenge_method=S256"));
  CHECK_THAT(seen_url, ContainsSubstring("code_challenge="));
  CHECK_THAT(seen_url, ContainsSubstring("redirect_uri=http%3A%2F%2F127.0.0.1%3A"));
  CHECK_THAT(seen_url, ContainsSubstring("scope=read%20write"));
  CHECK_THAT(res.token.access_token, StartsWith("tok-authorization_code-"));

  // Now sends use the cached token.
  const HttpResponse ok = send(t.ws, r, &err);
  INFO(err);
  CHECK(ok.status == 200);

  // A dangerous authorization URL is never handed to the browser.
  SavedRequest evil = oauth_req(OAuth2Config::Grant::AuthorizationCode, "ac-2");
  bool opened = false;
  OAuth2Hooks h2;
  h2.interactive = true;
  h2.open_browser = [&](const std::string&, std::string*) { return opened = true; };
  for (const char* bad : {"calc.exe", "file:///C:/Windows/System32/calc.exe", "javascript:alert(1)",
                          "http://x\" & calc", "\\\\attacker\\share\\x"}) {
    evil.auth.oauth2.auth_url = bad;
    const OAuth2Result e = oauth2_get_token(t.ws, evil, h2);
    CHECK_FALSE(e.ok);
    CHECK_THAT(e.error, ContainsSubstring("not an http(s) URL"));
  }
  CHECK_FALSE(opened);
  evil.auth.oauth2.auth_url = "{{host}}/oauth/authorize";
  evil.auth.oauth2.redirect_uri = "https://attacker.example/cb";
  const OAuth2Result e2 = oauth2_get_token(t.ws, evil, h2);
  CHECK_FALSE(e2.ok);
  CHECK_THAT(e2.error, ContainsSubstring("redirect URI"));
  CHECK_FALSE(opened);
}

TEST_CASE("PKCE helpers follow RFC 7636", "[auth][pkce]") {
  // RFC 7636 appendix B.
  CHECK(pkce_challenge_s256("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
  const std::string v = pkce_verifier();
  CHECK(v.size() == 43);
  CHECK(v != pkce_verifier());
  CHECK(form_urlencode("a b&c=d/é") == "a%20b%26c%3Dd%2F%C3%A9");
  CHECK(is_safe_http_url("https://x.example/a?b=c"));
  CHECK_FALSE(is_safe_http_url("https:///nohost"));
  CHECK_FALSE(is_safe_http_url("http://a b"));
}

// ------------------------------------------------------------------ storage

TEST_CASE("auth_scheme is saved only when set and round-trips", "[auth][model]") {
  TempWs t;
  SavedRequest r;
  r.name = "authy";
  r.http.url = "{{host}}/x";
  REQUIRE(t.ws.save_request(r));
  std::ifstream f0(r.file, std::ios::binary);
  const std::string before((std::istreambuf_iterator<char>(f0)), {});
  CHECK(before.find("auth_scheme") == std::string::npos);

  for (auto type : {HttpAuth::Type::Digest, HttpAuth::Type::Ntlm, HttpAuth::Type::Negotiate, HttpAuth::Type::AwsSigV4,
                    HttpAuth::Type::OAuth2}) {
    SavedRequest x = r;
    x.auth.type = type;
    x.auth.user = "u";
    x.auth.password = "{{pw}}";
    x.auth.aws_access_key = "AK";
    x.auth.aws_secret_key = "{{aws_secret}}";
    x.auth.aws_region = "us-east-1";
    x.auth.aws_service = "s3";
    x.auth.oauth2 = oauth_req(OAuth2Config::Grant::AuthorizationCode, "x").auth.oauth2;
    x.auth.oauth2.pkce = false;
    x.auth.oauth2.add_to = OAuth2Config::AddTo::Query;
    REQUIRE(t.ws.save_request(x));
    auto back = t.ws.load_request(x.file);
    REQUIRE(back);
    CHECK(back->auth.type == type);
    if (type == HttpAuth::Type::OAuth2) {
      CHECK(back->auth.oauth2 == x.auth.oauth2);
    } else if (type == HttpAuth::Type::AwsSigV4) {
      CHECK(back->auth.aws_secret_key == "{{aws_secret}}");
      CHECK(back->auth.aws_region == "us-east-1");
    } else {
      CHECK(back->auth.user == "u");
      CHECK(back->auth.password == "{{pw}}");
    }
  }
  REQUIRE(t.ws.save_request(r));
  std::ifstream f1(r.file, std::ios::binary);
  CHECK(std::string((std::istreambuf_iterator<char>(f1)), {}) == before);
}
