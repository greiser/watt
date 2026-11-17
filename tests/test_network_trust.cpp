// Consent gate for workspace-sourced network trust (Workspace::blocked_network_settings): a cloned
// repository's folder.json / .req.json must not silently route requests through its proxy or make
// watt trust its CA (MITM of every HTTPS request). Approvals live in the per-user config dir,
// bound to the proxy origin / the CA file's SHA-256; files are read under the upload policy.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>

#include "core/http_client.h"
#include "core/model.h"

using namespace watt;
namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using Item = Workspace::NetworkItem;

namespace {

fs::path temp_path(const char* prefix) {
  std::random_device rd;
  return fs::temp_directory_path() / (std::string(prefix) + std::to_string(rd()) + std::to_string(rd()));
}

struct TestConfigDir {
  fs::path dir = temp_path("watt_test_nettrust_cfg_");
  TestConfigDir() { Workspace::set_user_config_dir(dir); }
  ~TestConfigDir() {
    Workspace::set_user_config_dir({});
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

struct TempWs {
  fs::path root = temp_path("watt_test_nettrust_ws_");
  Workspace ws = Workspace::create(root);
  ~TempWs() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

void write(const fs::path& p, const std::string& s) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  f << s;
}

const char* kCaPem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBjDCCATOgAwIBAgIUSKDmIVa3Zk6GiKZeAkkWg5YbT70wCgYIKoZIzj0EAwIw\n"
    "HDEaMBgGA1UEAwwRRXZpbCBDb3JwIFJvb3QgQ0EwHhcNMjYxMTE3MjI0MDA2WhcN\n"
    "MzYxMTE0MjI0MDA2WjAcMRowGAYDVQQDDBFFdmlsIENvcnAgUm9vdCBDQTBZMBMG\n"
    "ByqGSM49AgEGCCqGSM49AwEHA0IABLSfyLvkU4UfoXyFK8NfvOjAOdWT/SbHKVc8\n"
    "NggfnGYeTYKchIAlYaXGKrelZnAdJJPP6i8pZtkelJcSS7jEi1qjUzBRMB0GA1Ud\n"
    "DgQWBBRdV8dd8UCA4wihQ1925jPMJL8ulzAfBgNVHSMEGDAWgBRdV8dd8UCA4wih\n"
    "Q1925jPMJL8ulzAPBgNVHRMBAf8EBTADAQH/MAoGCCqGSM49BAMCA0cAMEQCIADf\n"
    "neXzx0Rn6zxpyDux1R5Em1XbmsyiO9niDbPug/9xAiA1FjJ/tQyib19/QK8vpCUJ\n"
    "gG6KlD1rzb0uRekIjKLafA==\n"
    "-----END CERTIFICATE-----\n";

SavedRequest request(TempWs& t, const std::string& url = "https://example.test/") {
  SavedRequest r = t.ws.new_request();
  r.http.url = url;
  return r;
}

}  // namespace

TEST_CASE("network trust: a planted workspace proxy is blocked until the user allows it", "[network][consent]") {
  TestConfigDir cfg;
  TempWs t;
  FolderSettings coll;
  coll.network.proxy = NetworkSettings::Proxy::Url;
  coll.network.proxy_url = "http://10.0.0.5:8080";
  REQUIRE(t.ws.save_collection_settings(coll));
  const SavedRequest r = request(t);

  auto items = t.ws.blocked_network_settings(r);
  REQUIRE(items.size() == 1);
  CHECK(items[0].kind == Item::Kind::Proxy);
  CHECK(items[0].proxy_origin == "http://10.0.0.5:8080");
  CHECK(items[0].level == "collection");
  CHECK_FALSE(items[0].proxy_credentials);
  std::string err;
  CHECK_FALSE(t.ws.http_to_send(r, &err));  // fails closed
  CHECK_THAT(err, ContainsSubstring("http://10.0.0.5:8080"));
  CHECK_THAT(err, ContainsSubstring("proxy"));

  // A planted approval inside the workspace is ignored.
  write(t.root / ".watt" / "allowed_network.json",
        R"({"version":1,"workspaces":{")" + t.root.generic_string() + R"(":["proxy http://10.0.0.5:8080"]}})");
  write(t.root / "allowed_network.json",
        R"({"version":1,"workspaces":{")" + t.root.generic_string() + R"(":["proxy http://10.0.0.5:8080"]}})");
  CHECK(t.ws.blocked_network_settings(r).size() == 1);
  CHECK_FALSE(t.ws.http_to_send(r, &err));

  REQUIRE(t.ws.allow_network_item(items[0]));
  CHECK(t.ws.blocked_network_settings(r).empty());
  CHECK(fs::exists(cfg.dir / "allowed_network.json"));
  auto h = t.ws.http_to_send(r, &err);
  REQUIRE(h);
  CHECK(h->proxy == HttpRequest::Proxy::Url);
  CHECK(h->proxy_url == "http://10.0.0.5:8080");

  // Another proxy is a new question; disallow revokes.
  coll.network.proxy_url = "http://10.0.0.6:8080";
  REQUIRE(t.ws.save_collection_settings(coll));
  CHECK(t.ws.blocked_network_settings(r).size() == 1);
  CHECK_FALSE(t.ws.http_to_send(r, &err));
  coll.network.proxy_url = "http://10.0.0.5:8080";
  REQUIRE(t.ws.save_collection_settings(coll));
  REQUIRE(t.ws.disallow_network_item(items[0]));
  CHECK_FALSE(t.ws.http_to_send(r, &err));

  // The user's own proxy settings (environment / Windows / none) need no approval.
  for (auto m : {NetworkSettings::Proxy::Env, NetworkSettings::Proxy::System, NetworkSettings::Proxy::Off}) {
    coll.network.proxy = m;
    REQUIRE(t.ws.save_collection_settings(coll));
    CHECK(t.ws.blocked_network_settings(r).empty());
    CHECK(t.ws.http_to_send(r, &err));
  }
}

TEST_CASE("network trust: CA approval is bound to the file's content", "[network][consent]") {
  TestConfigDir cfg;
  TempWs t;
  write(t.root / "certs" / "ca.pem", kCaPem);
  FolderSettings coll;
  coll.network.ca_file = "certs/ca.pem";
  REQUIRE(t.ws.save_collection_settings(coll));
  const SavedRequest r = request(t);

  auto items = t.ws.blocked_network_settings(r);
  REQUIRE(items.size() == 1);
  CHECK(items[0].kind == Item::Kind::Ca);
  CHECK(items[0].fingerprint.size() == 32 * 3 - 1);
  REQUIRE(items[0].subjects.size() == 1);
  CHECK(items[0].subjects[0] == "Evil Corp Root CA");
  CHECK_THAT(items[0].text, ContainsSubstring("Evil Corp Root CA"));
  CHECK_THAT(items[0].text, ContainsSubstring(items[0].fingerprint));
  std::string err;
  CHECK_FALSE(t.ws.http_to_send(r, &err));
  CHECK_THAT(err, ContainsSubstring("CA certificate"));

  // Planted approval in the workspace: ignored.
  write(t.root / ".watt" / "allowed_network.json",
        R"({"version":1,"workspaces":{")" + t.root.generic_string() + R"(":[")" + items[0].key + R"("]}})");
  CHECK(t.ws.blocked_network_settings(r).size() == 1);

  REQUIRE(t.ws.allow_network_item(items[0]));
  CHECK(t.ws.blocked_network_settings(r).empty());
  auto h = t.ws.http_to_send(r, &err);
  REQUIRE(h);
  CHECK(h->ca_pem == kCaPem);

  // Changing the file revokes the approval.
  write(t.root / "certs" / "ca.pem", std::string(kCaPem) + "\n");
  CHECK_FALSE(t.ws.http_to_send(r, &err));
  CHECK_THAT(err, ContainsSubstring("changed"));
  items = t.ws.blocked_network_settings(r);
  REQUIRE(items.size() == 1);
  REQUIRE(t.ws.allow_network_item(items[0]));
  CHECK(t.ws.http_to_send(r, &err));
  CHECK(t.ws.allowed_network_items().size() == 1);  // the old version's approval was replaced
}

TEST_CASE("network trust: a UNC CA path is judged from its text and never touched", "[network][consent]") {
  TestConfigDir cfg;
  TempWs t;
  FolderSettings coll;
  coll.network.ca_file = R"(\\192.0.2.1\share\ca.pem)";
  coll.network.cert_file = R"(\\192.0.2.1\share\client.pem)";
  coll.network.key_file = R"(\\192.0.2.1\share\client.key)";
  REQUIRE(t.ws.save_collection_settings(coll));
  const SavedRequest r = request(t);
  const auto t0 = std::chrono::steady_clock::now();
  const auto items = t.ws.blocked_network_settings(r);
  std::string err;
  CHECK_FALSE(t.ws.http_to_send(r, &err));
  const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  CHECK(ms < 200);
  REQUIRE(items.size() == 2);
  CHECK(items[0].kind == Item::Kind::Ca);
  CHECK(items[0].fingerprint.empty());
  REQUIRE(items[0].file_blocked.size() == 1);
  CHECK(items[1].kind == Item::Kind::ClientCert);
  CHECK(items[1].file_blocked.size() == 2);
  CHECK_THAT(err, ContainsSubstring("192.0.2.1"));
}

TEST_CASE("network trust: proxy URLs are parsed strictly", "[network][consent]") {
  bool creds = true;
  std::string norm;
  CHECK(Workspace::proxy_origin("http://10.0.0.5:8080", &creds, &norm) == "http://10.0.0.5:8080");
  CHECK_FALSE(creds);
  CHECK(Workspace::proxy_origin("proxy.corp:3128") == "http://proxy.corp:3128");
  CHECK(Workspace::proxy_origin("HTTPS://Proxy.Corp") == "https://proxy.corp:443");
  CHECK(Workspace::proxy_origin("socks5h://127.0.0.1") == "socks5h://127.0.0.1:1080");
  CHECK(Workspace::proxy_origin("socks5://h:9") == "socks5://h:9");
  // User info: the origin is the host libcurl connects to; credentials flagged, kept in the URL.
  CHECK(Workspace::proxy_origin("http://good.example@evil.example:8080", &creds, &norm) ==
        "http://evil.example:8080");
  CHECK(creds);
  CHECK(Workspace::proxy_origin("http://u:p%40ss@evil.example:8080", &creds, &norm) == "http://evil.example:8080");
  CHECK(creds);
  CHECK(norm == "http://u:p%40ss@evil.example:8080");
  // Refused: backslash, other schemes, spaces / control characters, path / query tricks.
  CHECK(Workspace::proxy_origin(R"(http://good.example\@evil.example:8080)").empty());
  CHECK(Workspace::proxy_origin("file:///etc/passwd").empty());
  CHECK(Workspace::proxy_origin("socks4://h:1080").empty());
  CHECK(Workspace::proxy_origin("ftp://h:21").empty());
  CHECK(Workspace::proxy_origin("http://h:1 ").empty());
  CHECK(Workspace::proxy_origin("http://h:1\r\nX: y").empty());
  CHECK(Workspace::proxy_origin("http://h:1/?q").empty());
  CHECK(Workspace::proxy_origin("http://evil#@good:1").empty());

  TestConfigDir cfg;
  TempWs t;
  FolderSettings coll;
  coll.network.proxy = NetworkSettings::Proxy::Url;
  const SavedRequest r = request(t);
  std::string err;
  for (const char* bad : {R"(http://good\@evil:8080)", "file://x", "gopher://h:70"}) {
    coll.network.proxy_url = bad;
    REQUIRE(t.ws.save_collection_settings(coll));
    CHECK(t.ws.blocked_network_settings(r).empty());  // nothing to approve ...
    CHECK_FALSE(t.ws.http_to_send(r, &err));           // ... and never sent
    CHECK_THAT(err, ContainsSubstring("proxy URL"));
  }
  // A literal proxy password in a workspace file is refused; a {{secret}} variable works.
  coll.network.proxy_url = "http://user:hunter2@10.0.0.5:8080";
  REQUIRE(t.ws.save_collection_settings(coll));
  auto items = t.ws.blocked_network_settings(r);
  REQUIRE(items.size() == 1);
  CHECK(items[0].proxy_credentials);
  CHECK_THAT(items[0].text, ContainsSubstring("credentials"));
  CHECK(items[0].text.find("hunter2") == std::string::npos);
  REQUIRE(t.ws.allow_network_item(items[0]));
  CHECK_FALSE(t.ws.http_to_send(r, &err));
  CHECK_THAT(err, ContainsSubstring("plain text"));
  coll.network.proxy_url = "http://user:{{proxyPass}}@10.0.0.5:8080";
  coll.vars.push_back({"proxyPass", "hunter2", "", true});
  REQUIRE(t.ws.save_collection_settings(coll));
  auto h = t.ws.http_to_send(r, &err);
  REQUIRE(h);
  CHECK(h->proxy_url == "http://user:hunter2@10.0.0.5:8080");
}

TEST_CASE("network trust: an approved proxy is actually used", "[network][consent][http]") {
  WSADATA wsa;
  REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
  SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  REQUIRE(ls != INVALID_SOCKET);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  REQUIRE(bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
  REQUIRE(listen(ls, 1) == 0);
  int alen = sizeof a;
  getsockname(ls, reinterpret_cast<sockaddr*>(&a), &alen);
  const int port = ntohs(a.sin_port);
  std::string seen;
  std::thread proxy([&] {  // a fake HTTP proxy: one request, answered directly
    SOCKET s = accept(ls, nullptr, nullptr);
    if (s == INVALID_SOCKET) return;
    char buf[4096];
    while (seen.find("\r\n\r\n") == std::string::npos) {
      const int n = recv(s, buf, sizeof buf, 0);
      if (n <= 0) break;
      seen.append(buf, n);
    }
    const char* resp = "HTTP/1.1 200 OK\r\nContent-Length: 7\r\nConnection: close\r\n\r\nproxied";
    send(s, resp, static_cast<int>(strlen(resp)), 0);
    closesocket(s);
  });

  TestConfigDir cfg;
  TempWs t;
  FolderSettings coll;
  coll.network.proxy = NetworkSettings::Proxy::Url;
  coll.network.proxy_url = "http://127.0.0.1:" + std::to_string(port);
  REQUIRE(t.ws.save_collection_settings(coll));
  const SavedRequest r = request(t, "http://target.invalid/path");
  for (const auto& it : t.ws.blocked_network_settings(r)) REQUIRE(t.ws.allow_network_item(it));
  std::string err;
  auto h = t.ws.http_to_send(r, &err);
  REQUIRE(h);
  h->timeout_ms = 5000;
  HttpClient c;
  const HttpResponse resp = c.send(*h);
  proxy.join();
  closesocket(ls);
  WSACleanup();
  CHECK(resp.ok);
  CHECK(resp.status == 200);
  CHECK(resp.body == "proxied");
  CHECK(seen.rfind("GET http://target.invalid/path HTTP/1.1\r\n", 0) == 0);
  CHECK(resp.conn.proxy == "http://127.0.0.1:" + std::to_string(port));
}
