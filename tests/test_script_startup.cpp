// Start-up cost of request scripts, in its own process so QuickJS is guaranteed cold: a request
// without scripts pays nothing and never initialises QuickJS; then the first script's cost (cold)
// and a typical HMAC pre-request script (warm). One test case, so the order is fixed.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "core/asserts.h"
#include "core/http_client.h"
#include "core/model.h"
#include "core/script.h"

using namespace watt;
namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

namespace {

fs::path temp_path(const char* prefix) {
  std::random_device rd;
  return fs::temp_directory_path() / (std::string(prefix) + std::to_string(rd()) + std::to_string(rd()));
}

struct TestConfigDir {
  fs::path dir = temp_path("watt_test_startup_cfg_");
  TestConfigDir() { Workspace::set_user_config_dir(dir); }
  ~TestConfigDir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
} test_config_dir;

struct TempWs {
  fs::path root = temp_path("watt_test_startup_ws_");
  Workspace ws;
  explicit TempWs(bool trusted) : ws(Workspace::create(root)) {
    Environment dev;
    dev.name = "dev";
    ws.environments().push_back(dev);
    ws.active_env = "dev";
    REQUIRE(ws.save_settings());
    if (trusted) REQUIRE(ws.trust_scripts(true));
  }
  ~TempWs() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

TEST_CASE("script start-up cost: zero without scripts, lazy QuickJS, warm HMAC pre-script", "[script][perf]") {
  TempWs t(true);
  REQUIRE_FALSE(script_engine_initialized());

  // 1. A request without scripts: the run functions return at once, QuickJS never starts.
  SavedRequest plain;
  plain.http.url = "http://example.invalid/";
  constexpr int kN = 1000000;
  int ran = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kN; ++i) ran += run_pre_script(t.ws, plain).ran;
  const double ns = ms_since(t0) * 1e6 / kN;
  HttpResponse resp;
  resp.ok = true;
  resp.status = 200;
  ResponseData rd(resp);
  CHECK_FALSE(run_post_script(t.ws, plain, rd).ran);
  CHECK(ran == 0);
  CHECK(ns < 1000);
  CHECK_FALSE(script_engine_initialized());

  // 2. A script in an untrusted workspace: refused before QuickJS starts.
  TempWs u(false);
  SavedRequest s;
  s.pre_script = "console.log('hi')";
  const ScriptRun refused = run_pre_script(u.ws, s);
  CHECK_FALSE(refused.ran);
  CHECK(refused.untrusted);
  CHECK_THAT(refused.error, ContainsSubstring("not trusted"));
  CHECK_FALSE(script_engine_initialized());

  // 3. The first script of the process (cold), then the same script warm.
  const std::string hmac_script = R"(
    const ts = String(Date.now());
    const body = pm.request.body.raw || "";
    const sig = CryptoJS.HmacSHA256(ts + "\n" + pm.request.method + "\n" + body, "s3cr3t-key").toString(CryptoJS.enc.Base64);
    pm.request.headers.upsert({ key: "X-Timestamp", value: ts });
    pm.request.headers.upsert({ key: "X-Signature", value: sig });
    pm.environment.set("last_sig", sig);
  )";
  SavedRequest h;
  h.http.method = "POST";
  h.http.url = "http://example.invalid/orders";
  h.http.body = R"({"sku":"A-1","qty":2})";
  h.pre_script = hmac_script;
  SavedRequest r1 = h;
  const auto tc = std::chrono::steady_clock::now();
  const ScriptRun first = run_pre_script(t.ws, r1);
  const double cold_ms = ms_since(tc);
  REQUIRE(first.ok);
  CHECK(script_engine_initialized());
  CHECK(first.request_changed);
  CHECK(r1.http.headers.size() == 2);

  constexpr int kRuns = 300;
  std::vector<double> warm, empty;
  double setup_us = 0;
  for (int i = 0; i < kRuns; ++i) {
    SavedRequest r = h;
    const auto t1 = std::chrono::steady_clock::now();
    const ScriptRun run = run_pre_script(t.ws, r);
    warm.push_back(ms_since(t1) * 1000);
    REQUIRE(run.ok);
  }
  for (int i = 0; i < kRuns; ++i) {
    SavedRequest r = h;
    r.pre_script = "1";
    const auto t1 = std::chrono::steady_clock::now();
    const ScriptRun run = run_pre_script(t.ws, r);
    empty.push_back(ms_since(t1) * 1000);
    setup_us += run.setup_us;
  }
  std::sort(warm.begin(), warm.end());
  std::sort(empty.begin(), empty.end());
  std::printf("[perf] script-less request: %.1f ns per run_pre_script call (QuickJS not initialised)\n", ns);
  std::printf("[perf] first script in the process (QuickJS runtime + prelude compile + run): %.2f ms (setup %.0f us)\n",
              cold_ms, first.setup_us);
  std::printf("[perf] HMAC pre-request script, warm: p50 %.0f us, p90 %.0f us, min %.0f us\n", warm[kRuns / 2],
              warm[kRuns * 9 / 10], warm.front());
  std::printf("[perf] trivial script, warm (fresh runtime + context + prelude + teardown): p50 %.0f us, setup avg %.0f us\n",
              empty[kRuns / 2], setup_us / kRuns);
  CHECK(warm[kRuns / 2] < 5000);
}
