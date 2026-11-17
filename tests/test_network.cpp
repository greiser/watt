// CA / client certificate / proxy settings of HTTP requests (core NetworkSettings): folder.json
// and request file format, per-part inheritance with origins, what http_to_send() hands the
// engine, the upload policy for certificate files, the DPAPI-sealed passphrase, and the
// engine's client certificate / proxy error paths (no certificate store is touched here).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "core/http_client.h"
#include "core/model.h"

using namespace watt;
namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

namespace {

fs::path temp_path(const char* prefix) {
  std::random_device rd;
  return fs::temp_directory_path() / (std::string(prefix) + std::to_string(rd()) + std::to_string(rd()));
}

struct TestConfigDir {
  fs::path dir = temp_path("watt_test_network_cfg_");
  TestConfigDir() { Workspace::set_user_config_dir(dir); }
  ~TestConfigDir() {
    Workspace::set_user_config_dir({});
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

struct TempWs {
  fs::path root = temp_path("watt_test_network_ws_");
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

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("network settings: folder.json round trip and per-part inheritance", "[network]") {
  TestConfigDir cfg;
  TempWs t;
  FolderSettings coll;
  coll.network.ca_file = "certs/ca.pem";
  coll.network.proxy = NetworkSettings::Proxy::System;
  coll.network.no_proxy = ".corp";
  REQUIRE(t.ws.save_collection_settings(coll));
  FolderSettings api;
  api.network.cert_file = "certs/client.pem";
  api.network.key_file = "certs/client.key";
  REQUIRE(t.ws.save_folder_settings(t.root / "api", api));
  const auto back = t.ws.folder_settings(t.root / "api");
  REQUIRE(back);
  CHECK(back->network == api.network);
  CHECK(t.ws.collection_settings().network == coll.network);
  CHECK_THAT(slurp(t.root / "folder.json"), ContainsSubstring("\"proxy\": \"system\""));

  SavedRequest r = t.ws.new_request(RequestKind::Http, "r", t.root / "api");
  r.http.url = "https://example.test/";
  const EffectiveSettings e = t.ws.effective_settings(r);
  CHECK(e.network.ca_file == "certs/ca.pem");
  CHECK(e.ca_from.level == SettingsOrigin::Level::Collection);
  CHECK(e.network.cert_file == "certs/client.pem");
  CHECK(e.cert_from.label == "api");
  CHECK(e.network.proxy == NetworkSettings::Proxy::System);
  CHECK(e.proxy_from.level == SettingsOrigin::Level::Collection);

  // The request switches the certificate off and picks its own proxy.
  r.network.cert_off = true;
  r.network.proxy = NetworkSettings::Proxy::Off;
  const EffectiveSettings e2 = t.ws.effective_settings(r);
  CHECK(e2.network.cert_file.empty());
  CHECK(e2.cert_from.level == SettingsOrigin::Level::Request);
  CHECK(e2.network.proxy == NetworkSettings::Proxy::Off);
  CHECK(e2.network.ca_file == "certs/ca.pem");

  // Saved with the request and read back.
  REQUIRE(t.ws.save_request(r));
  const auto loaded = t.ws.load_request(r.file);
  REQUIRE(loaded);
  CHECK(loaded->network == r.network);
}

TEST_CASE("network settings: http_to_send reads the files and maps the proxy", "[network]") {
  TestConfigDir cfg;
  TempWs t;
  write(t.root / "certs" / "ca.pem", "-----BEGIN CERTIFICATE-----\nCA\n-----END CERTIFICATE-----\n");
  write(t.root / "certs" / "client.pem", "CERT");
  write(t.root / "certs" / "client.key", "KEY");
  FolderSettings coll;
  coll.network.ca_file = "certs/ca.pem";
  coll.network.cert_file = "certs/client.pem";
  coll.network.key_file = "certs/client.key";
  coll.network.proxy = NetworkSettings::Proxy::Url;
  coll.network.proxy_url = "http://{{proxyHost}}:3128";
  coll.network.no_proxy = "localhost";
  coll.vars.push_back({"proxyHost", "proxy.corp", "", true});
  REQUIRE(t.ws.save_collection_settings(coll));
  REQUIRE(t.ws.set_cert_passphrase("", "s3cret"));
  SavedRequest r = t.ws.new_request();
  r.http.url = "https://example.test/";
  std::string err;
  // Workspace-sourced proxy / CA / client certificate: refused until the user approved each.
  CHECK_FALSE(t.ws.http_to_send(r, &err));
  const auto items = t.ws.blocked_network_settings(r);
  REQUIRE(items.size() == 3);
  for (const auto& it : items) REQUIRE(t.ws.allow_network_item(it));
  CHECK(t.ws.blocked_network_settings(r).empty());
  const auto h = t.ws.http_to_send(r, &err);
  REQUIRE(h);
  CHECK(h->ca_pem.find("BEGIN CERTIFICATE") != std::string::npos);
  CHECK(h->client_cert == "CERT");
  CHECK(h->client_key == "KEY");
  CHECK(h->client_key_passphrase == "s3cret");
  CHECK(h->proxy == HttpRequest::Proxy::Url);
  CHECK(h->proxy_url == "http://proxy.corp:3128");
  CHECK(h->no_proxy == "localhost");

  // The passphrase is sealed (never in clear text in secrets.json) and survives save_settings().
  const std::string sec = slurp(t.root / ".watt" / "secrets.json");
  CHECK(sec.find("s3cret") == std::string::npos);
  REQUIRE(t.ws.save_settings());
  CHECK(t.ws.cert_passphrase("") == "s3cret");
  REQUIRE(t.ws.set_cert_passphrase("", ""));
  CHECK(t.ws.cert_passphrase("").empty());

  // A bad proxy scheme is refused.
  coll.network.proxy_url = "file://x";
  REQUIRE(t.ws.save_collection_settings(coll));
  CHECK_FALSE(t.ws.http_to_send(r, &err));
  CHECK_THAT(err, ContainsSubstring("proxy URL"));
}

TEST_CASE("network settings: certificate files outside the workspace need consent", "[network]") {
  TestConfigDir cfg;
  TempWs t;
  const fs::path outside = temp_path("watt_test_network_key_");
  write(outside / "client.pem", "CERT");
  FolderSettings coll;
  coll.network.cert_file = outside.generic_string() + "/client.pem";
  REQUIRE(t.ws.save_collection_settings(coll));
  SavedRequest r = t.ws.new_request();
  r.http.url = "https://example.test/";
  const auto blocked = t.ws.blocked_files(r);
  REQUIRE(blocked.size() == 1);
  std::string err;
  CHECK_FALSE(t.ws.http_to_send(r, &err));
  CHECK_THAT(err, ContainsSubstring("client certificate file"));
  REQUIRE(t.ws.allow_file(blocked[0]));
  CHECK_FALSE(t.ws.http_to_send(r, &err));  // readable now, but the certificate itself is not approved
  CHECK_THAT(err, ContainsSubstring("client certificate"));
  const auto items = t.ws.blocked_network_settings(r);
  REQUIRE(items.size() == 1);
  CHECK(items[0].kind == Workspace::NetworkItem::Kind::ClientCert);
  CHECK(items[0].file_blocked.empty());
  REQUIRE(t.ws.allow_network_item(items[0]));
  CHECK(t.ws.http_to_send(r, &err));
  std::error_code ec;
  fs::remove_all(outside, ec);
}

TEST_CASE("engine: unusable client certificates fail before connecting", "[network][http]") {
  HttpClient c;
  HttpRequest q;
  q.url = "https://127.0.0.1:1/";
  q.timeout_ms = 2000;
  q.client_cert = "-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n";
  HttpResponse r = c.send(q);
  CHECK_FALSE(r.ok);
  CHECK_THAT(r.error, ContainsSubstring("client certificate"));
  CHECK(r.conn.client_cert);
  q.client_cert = "not a certificate";
  r = c.send(q);
  CHECK_FALSE(r.ok);
  CHECK_THAT(r.error, ContainsSubstring("client certificate"));
  // Clearing it sends without one again (the connect then fails on its own).
  q.client_cert.clear();
  r = c.send(q);
  CHECK_FALSE(r.ok);
  CHECK(r.error.find("client certificate") == std::string::npos);
  CHECK_FALSE(r.conn.client_cert);
}

TEST_CASE("engine: proxy modes", "[network][http]") {
  HttpClient c;
  HttpRequest q;
  q.url = "http://127.0.0.1:1/";
  q.timeout_ms = 2000;
  q.proxy = HttpRequest::Proxy::Url;
  q.proxy_url = "http://user:pw@127.0.0.1:2";  // credentials never shown  // nothing listens: the proxy connect fails
  HttpResponse r = c.send(q);
  CHECK_FALSE(r.ok);
  CHECK(r.conn.proxy == "http://127.0.0.1:2");  // the attempt went to the proxy (the error is its)
  q.proxy = HttpRequest::Proxy::Off;
  r = c.send(q);
  CHECK_FALSE(r.ok);
  CHECK(r.conn.proxy.empty());
}
