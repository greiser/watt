// gRPC engine tests. Spawns tools/testserver (watt_testserver.exe next to this exe) on a free
// port once per run and drives every engine feature against it.
#include <catch2/catch_test_macros.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/util/json_util.h>
#include <google/protobuf/wrappers.pb.h>

#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/grpcpp.h>

#include "core/grpc_client.h"
#include "core/grpc_wire_repair.h"

using namespace watt;
namespace fs = std::filesystem;

namespace {

fs::path exe_dir() {
  wchar_t buf[MAX_PATH];
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return fs::path(buf).parent_path();
}

fs::path proto_dir();

class TestServer {
 public:
  // One server per distinct argument list ("" = plaintext, v1 + v1alpha reflection), started on
  // first use and killed with the test process.
  static TestServer& get(const std::wstring& args = L"") {
    static std::mutex mu;
    static std::map<std::wstring, std::unique_ptr<TestServer>> servers;
    std::lock_guard lk(mu);
    auto& s = servers[args];
    if (!s) s.reset(new TestServer(args));
    return *s;
  }
  ~TestServer() {
    TerminateProcess(pi_.hProcess, 0);
    CloseHandle(pi_.hThread);
    CloseHandle(pi_.hProcess);
    CloseHandle(job_);
  }
  std::string address() const { return "127.0.0.1:" + std::to_string(port_); }
  GrpcTarget target() const {
    GrpcTarget t;
    t.address = address();
    return t;
  }
  // The server's gRPC-Web listener (tools/testserver/grpcweb.cpp).
  GrpcTarget web_target(bool text = false) const {
    if (web_port_ == 0) throw std::runtime_error("watt_testserver has no gRPC-Web listener");
    GrpcTarget t;
    t.address = "127.0.0.1:" + std::to_string(web_port_);
    t.web = true;
    t.web_text = text;
    return t;
  }

 private:
  explicit TestServer(const std::wstring& args) {
    fs::path exe = exe_dir() / "watt_testserver.exe";
    if (!fs::exists(exe)) throw std::runtime_error("watt_testserver.exe not found next to the test exe");
    job_ = CreateJobObjectW(nullptr, nullptr);  // the server dies with us, even on a crash
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &li, sizeof(li));

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    std::wstring cmd = L"\"" + exe.wstring() + L"\" 0 " + args;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi_))
      throw std::runtime_error("cannot start watt_testserver");
    AssignProcessToJobObject(job_, pi_.hProcess);
    CloseHandle(wr);

    auto read_line = [&] {
      std::string line;
      char c;
      DWORD n;
      while (ReadFile(rd, &c, 1, &n, nullptr) && n == 1 && c != '\n') line += c;
      return line;
    };
    const std::string line = read_line();
    auto colon = line.rfind(':');
    if (colon == std::string::npos) throw std::runtime_error("unexpected server banner: " + line);
    port_ = std::stoi(line.substr(colon + 1));
    // "... grpc-web listening on 127.0.0.1:<port>", or "... grpc-web disabled: <why>"
    const std::string web = read_line();
    if (web.find("grpc-web listening") != std::string::npos) web_port_ = std::stoi(web.substr(web.rfind(':') + 1));
    CloseHandle(rd);
  }
  PROCESS_INFORMATION pi_{};
  HANDLE job_ = nullptr;
  int port_ = 0;
  int web_port_ = 0;
};

fs::path proto_dir() {
  fs::path here = fs::path(__FILE__).parent_path().parent_path() / "tools" / "testserver";
  if (fs::exists(here / "test.proto")) return here;
  return exe_dir().parent_path().parent_path() / "tools" / "testserver";
}

std::string header(const Metadata& md, const std::string& key) {
  for (const auto& [k, v] : md)
    if (k == key) return v;
  return {};
}

const GrpcMethodInfo* find(const GrpcSchema& s, const std::string& path) {
  for (const auto& m : s.methods)
    if (m.full_path == path) return &m;
  return nullptr;
}

rapidjson::Document parse(const std::string& s) {
  rapidjson::Document d;
  d.Parse(s.c_str(), s.size());
  return d;
}

void check_schema(const GrpcSchema& s) {
  REQUIRE(s.ok);
  INFO(s.error);
  // test.proto's Echo + Everything; reflection also lists pack.proto's Packer.Pack.
  const auto own = std::count_if(s.methods.begin(), s.methods.end(),
                                 [](const GrpcMethodInfo& m) { return m.service != "watt.test.Packer"; });
  REQUIRE(own == 7);
  const auto* say = find(s, "/watt.test.Echo/Say");
  REQUIRE(say);
  CHECK(say->service == "watt.test.Echo");
  CHECK(say->method == "Say");
  CHECK(say->input_type == "watt.test.EchoRequest");
  CHECK(say->output_type == "watt.test.EchoResponse");
  CHECK(say->kind == GrpcCallKind::Unary);
  CHECK(find(s, "/watt.test.Echo/Count")->kind == GrpcCallKind::ServerStreaming);
  CHECK(find(s, "/watt.test.Echo/Sum")->kind == GrpcCallKind::ClientStreaming);
  CHECK(find(s, "/watt.test.Echo/Chat")->kind == GrpcCallKind::Bidi);
  CHECK(find(s, "/watt.test.Everything/Echo"));
  for (const auto& m : s.methods) CHECK(m.service.find("grpc.reflection") == std::string::npos);
}

}  // namespace

TEST_CASE("reflection lists services and resolves descriptors transitively", "[grpc][reflection]") {
  GrpcClient c;
  auto s = c.load_via_reflection(TestServer::get().target());
  check_schema(s);
  CHECK(s.load_ms > 0);
  // Kitchen pulls in Timestamp/Duration/Struct/wrappers/Empty: they must resolve for the template.
  auto d = parse(find(s, "/watt.test.Everything/Echo")->request_template);
  REQUIRE(d.IsObject());
  CHECK(d["ts"].IsString());
}

TEST_CASE("reflection against an unreachable server fails fast with an error", "[grpc][reflection]") {
  GrpcClient c;
  GrpcTarget t;
  t.address = "127.0.0.1:1";
  auto t0 = std::chrono::steady_clock::now();
  auto s = c.load_via_reflection(t, 3000);
  CHECK_FALSE(s.ok);
  CHECK(s.error.find("UNAVAILABLE") != std::string::npos);
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(2900));
}

TEST_CASE("proto files load with import dirs and well-known types", "[grpc][proto]") {
  GrpcClient c;
  auto dir = proto_dir().string();
  SECTION("explicit import dir") { check_schema(c.load_proto_files({(proto_dir() / "test.proto").string()}, {dir})); }
  SECTION("no import dirs: the file's folder is used") {
    check_schema(c.load_proto_files({(proto_dir() / "test.proto").string()}, {}));
  }
  SECTION("virtual path relative to an import dir") { check_schema(c.load_proto_files({"test.proto"}, {dir})); }
}

TEST_CASE("proto file errors carry file and line", "[grpc][proto]") {
  auto tmp = fs::temp_directory_path() / "watt_test_grpc";
  fs::create_directories(tmp);
  auto bad = tmp / "bad.proto";
  {
    FILE* f = nullptr;
    _wfopen_s(&f, bad.wstring().c_str(), L"wb");
    REQUIRE(f);
    std::fputs("syntax = \"proto3\";\nmessage A {\n  strin x = 1;\n}\n", f);
    std::fclose(f);
  }
  GrpcClient c;
  auto s = c.load_proto_files({bad.string()}, {});
  CHECK_FALSE(s.ok);
  CHECK(s.error.find("bad.proto:3") != std::string::npos);

  auto missing = c.load_proto_files({"does/not/exist.proto"}, {tmp.string()});
  CHECK_FALSE(missing.ok);
  CHECK_FALSE(missing.error.empty());
  fs::remove_all(tmp);
}

TEST_CASE("request template has every field", "[grpc][template]") {
  GrpcClient c;
  auto s = c.load_proto_files({(proto_dir() / "test.proto").string()}, {});
  REQUIRE(s.ok);
  const auto* m = find(s, "/watt.test.Everything/Echo");
  REQUIRE(m);
  INFO(m->request_template);
  auto d = parse(m->request_template);
  REQUIRE(d.IsObject());
  for (const char* k : {"fDouble", "fFloat", "fInt32", "fInt64", "fUint32", "fUint64", "fSint32", "fSint64",
                        "fFixed32", "fFixed64", "fSfixed32", "fSfixed64", "fBool", "fString", "fBytes", "level",
                        "inner", "tags", "items", "levels", "counts", "byId", "text", "maybe", "ts", "dur", "meta",
                        "value", "wrapped", "nothing"})
    CHECK(d.HasMember(k));
  CHECK(d["fInt64"].IsString());
  CHECK(d["level"] == "LEVEL_UNSPECIFIED");
  CHECK(d["levels"].Size() == 1);
  CHECK(d["tags"].Size() == 1);
  CHECK(d["items"].Size() == 1);
  CHECK(d["items"][0]["name"] == "");
  CHECK(d["counts"].HasMember("key"));
  CHECK(d["byId"].HasMember("0"));
  CHECK(d["byId"]["0"].IsObject());
  // oneof: only the first member, and a note lists the alternatives
  CHECK_FALSE(d.HasMember("number"));
  CHECK_FALSE(d.HasMember("nested"));
  auto has_note = [&](const std::string& part) {
    for (const auto& n : m->template_notes)
      if (n.find(part) != std::string::npos) return true;
    return false;
  };
  CHECK(has_note("choice: oneof, set exactly one of text | number | nested (template uses text)"));
  CHECK(has_note("meta: google.protobuf.Struct, any JSON object"));
  CHECK(has_note("value: google.protobuf.Value, any JSON value"));
  CHECK(has_note("level: enum watt.test.Level, one of LEVEL_UNSPECIFIED | "));
  CHECK(has_note("levels[]: enum watt.test.Level"));
  CHECK(has_note("maybe: optional, leave it out to send it unset"));
  CHECK_FALSE(has_note("fString: optional"));  // plain proto3 scalars have no presence
  // recursion: Inner is expanded one more level (like grpcurl); its own self-references are unset
  REQUIRE(d["inner"]["child"].IsObject());
  CHECK(d["inner"]["child"].HasMember("name"));
  CHECK(d["inner"]["child"]["child"].IsNull());
  CHECK(d["inner"]["child"]["children"].Size() == 0);
  REQUIRE(d["inner"]["children"].Size() == 1);
  CHECK(d["inner"]["children"][0]["child"].IsNull());
  CHECK(d["ts"] == "1970-01-01T00:00:00Z");
  CHECK(d["dur"] == "0s");
  CHECK(d["meta"].IsObject());
  CHECK(d["wrapped"] == 0);

  // The template is a valid request as-is, and the server echoes it back.
  auto r = c.invoke(TestServer::get().target(), m->full_path, m->request_template, {});
  INFO(r.status_message);
  REQUIRE(r.ok);
  REQUIRE(r.messages.size() == 1);
  auto back = parse(r.messages[0].json);
  CHECK(back["level"] == "LEVEL_UNSPECIFIED");
  CHECK(back["tags"].Size() == 1);
}

TEST_CASE("unary call with metadata, headers, trailers and timing", "[grpc][unary]") {
  GrpcClient c;
  auto t = TestServer::get().target();
  GrpcCallOptions o;
  o.metadata = {{"X-Foo", "bar"}, {"x-blob-bin", "AAEC"}};
  auto r = c.invoke(t, "/watt.test.Echo/Say", R"({"message":"hi","repeat":3})", o);
  INFO(r.status_message);
  REQUIRE(r.ok);
  CHECK(r.status_code == 0);
  CHECK(r.status_name == "OK");
  REQUIRE(r.messages.size() == 1);
  auto d = parse(r.messages[0].json);
  CHECK(d["message"] == "hihihi");
  CHECK(r.messages[0].json.find("{\n  \"message\": \"hihihi\"") == 0);  // pretty, 2-space indent
  CHECK(d["metadata"]["x-foo"] == "bar");
  CHECK(header(r.headers, "x-server") == "watt-testserver");
  CHECK(header(r.headers, "echo-x-foo") == "bar");
  CHECK(header(r.headers, "echo-x-blob-bin") == "AAEC");  // binary round-trips as base64
  CHECK(header(r.trailers, "x-trailer") == "done");
  CHECK(r.messages[0].wire_bytes > 5);
  CHECK(r.timing.bytes_sent > 5);
  CHECK(r.timing.bytes_received == r.messages[0].wire_bytes);
  CHECK(r.timing.total_ms > 0);
  CHECK(r.timing.total_ms >= r.timing.connect_ms);
  CHECK(r.messages[0].at_ms <= r.timing.total_ms);
  CHECK_FALSE(r.timing.connection_reused);  // a new client: auto-reflection connected for this call
  CHECK(r.timing.connect_ms > 0);
  CHECK(r.reflect_ms > 0);
  CHECK(r.timing.queue_ms < r.reflect_ms + r.timing.connect_ms);  // reflection is not hidden in queue
  CHECK(r.timing.total_ms >= r.timing.queue_ms + r.timing.connect_ms + r.reflect_ms + r.timing.send_ms);

  GrpcClient fresh;
  auto first = fresh.invoke(t, "/watt.test.Echo/Say", R"({"message":"x"})", {});
  REQUIRE(first.ok);  // auto-reflection on first use
  CHECK(first.reflect_ms > 0);
  auto second = fresh.invoke(t, "/watt.test.Echo/Say", R"({"message":"x"})", {});
  REQUIRE(second.ok);
  CHECK(second.timing.connection_reused);
  CHECK(second.timing.connect_ms == 0);
  CHECK(second.reflect_ms == 0);
  CHECK(second.timing.ttfb_ms > 0);

  GrpcClient cold;
  cold.load_proto_files({(proto_dir() / "test.proto").string()}, {});
  auto r3 = cold.invoke(t, "/watt.test.Echo/Say", R"({"message":"x"})", {});
  REQUIRE(r3.ok);
  CHECK_FALSE(r3.timing.connection_reused);  // new channel: connect timed
  CHECK(r3.timing.connect_ms > 0);
}

TEST_CASE("server streaming delivers each message with offsets", "[grpc][stream]") {
  GrpcClient c;
  int seen = 0;
  auto r = c.invoke(TestServer::get().target(), "/watt.test.Echo/Count", R"({"count":5,"intervalMs":20})", {},
                    [&](const GrpcMessageEvent& e) {
                      ++seen;
                      CHECK_FALSE(e.json.empty());
                    });
  INFO(r.status_message);
  REQUIRE(r.ok);
  CHECK(seen == 5);
  REQUIRE(r.messages.size() == 5);
  for (int i = 0; i < 5; ++i) CHECK(parse(r.messages[i].json)["index"] == i);
  for (int i = 1; i < 5; ++i) CHECK(r.messages[i].at_ms >= r.messages[i - 1].at_ms + 10);
  CHECK(header(r.trailers, "x-count") == "5");
  CHECK(r.timing.download_ms >= 60);
}

TEST_CASE("client streaming sends a JSON array in order", "[grpc][stream]") {
  GrpcClient c;
  auto r = c.invoke(TestServer::get().target(), "/watt.test.Echo/Sum",
                    R"([{"value":"1"},{"value":2},{"value":"40"}])", {});
  INFO(r.status_message);
  REQUIRE(r.ok);
  REQUIRE(r.messages.size() == 1);
  auto d = parse(r.messages[0].json);
  CHECK(d["total"] == "43");
  CHECK(d["count"] == 3);

  auto empty = c.invoke(TestServer::get().target(), "/watt.test.Echo/Sum", "[]", {});
  REQUIRE(empty.ok);
  CHECK(parse(empty.messages[0].json)["count"] == 0);
}

TEST_CASE("bidi streaming echoes each message", "[grpc][stream]") {
  GrpcClient c;
  auto r = c.invoke(TestServer::get().target(), "/watt.test.Echo/Chat",
                    R"([{"text":"a"},{"text":"b"},{"text":"c"}])", {});
  INFO(r.status_message);
  REQUIRE(r.ok);
  REQUIRE(r.messages.size() == 3);
  CHECK(parse(r.messages[0].json)["text"] == "echo: a");
  CHECK(parse(r.messages[2].json)["text"] == "echo: c");
  CHECK(parse(r.messages[2].json)["seq"] == 2);
  CHECK(header(r.headers, "x-server") == "watt-testserver");
  CHECK(header(r.trailers, "x-count") == "3");
}

TEST_CASE("deadline exceeded", "[grpc][deadline]") {
  GrpcClient c;
  auto t = TestServer::get().target();
  c.load_via_reflection(t);
  GrpcCallOptions o;
  o.deadline_ms = 150;
  auto r = c.invoke(t, "/watt.test.Echo/Slow", R"({"delayMs":3000})", o);
  CHECK_FALSE(r.ok);
  CHECK(r.status_name == "DEADLINE_EXCEEDED");
  CHECK(r.timing.total_ms >= 140);
  CHECK(r.timing.total_ms < 1500);

  o.deadline_ms = 3000;
  auto ok = c.invoke(t, "/watt.test.Echo/Slow", R"({"delayMs":50})", o);
  REQUIRE(ok.ok);
  CHECK(ok.timing.ttfb_ms >= 45);
}

TEST_CASE("errors carry status, message, trailers and decoded details", "[grpc][errors]") {
  GrpcClient c;
  auto t = TestServer::get().target();
  auto r = c.invoke(t, "/watt.test.Echo/Fail", R"({"code":5,"message":"no such thing","withDetails":true})", {});
  CHECK_FALSE(r.ok);
  CHECK(r.status_code == 5);
  CHECK(r.status_name == "NOT_FOUND");
  CHECK(r.status_message == "no such thing");
  CHECK(header(r.trailers, "x-trailer") == "failed");
  INFO(r.error_details);
  auto d = parse(r.error_details);
  REQUIRE(d.IsObject());
  CHECK(d["code"] == 5);
  REQUIRE(d["details"].Size() == 2);
  CHECK(d["details"][0]["@type"] == "type.googleapis.com/watt.test.ErrorDetail");
  CHECK(d["details"][0]["reason"] == "BAD_THING");
  CHECK(d["details"][0]["retryAfterMs"] == 250);
  CHECK(d["details"][1]["@type"] == "type.googleapis.com/google.rpc.ErrorInfo");
  CHECK(d["details"][1]["reason"] == "QUOTA");
  CHECK(d["details"][1]["metadata"]["limit"] == "10");

  auto plain = c.invoke(t, "/watt.test.Echo/Fail", R"({"code":7})", {});
  CHECK(plain.status_name == "PERMISSION_DENIED");
  CHECK(plain.error_details.empty());
}

TEST_CASE("client-side errors are reported without a call", "[grpc][errors]") {
  GrpcClient c;
  auto t = TestServer::get().target();
  auto bad = c.invoke(t, "/watt.test.Echo/Say", R"({"mesage":"typo"})", {});
  CHECK(bad.status_name == "INVALID_ARGUMENT");
  CHECK(bad.status_message.find("mesage") != std::string::npos);

  auto arr = c.invoke(t, "/watt.test.Echo/Say", R"([{},{}])", {});
  CHECK(arr.status_name == "INVALID_ARGUMENT");

  auto unknown = c.invoke(t, "/watt.test.Echo/Nope", "{}", {});
  CHECK(unknown.status_name == "NOT_FOUND");

  GrpcCallOptions o;
  o.metadata = {{"bad key", "v"}};
  auto md = c.invoke(t, "/watt.test.Echo/Say", "{}", o);
  CHECK(md.status_name == "INVALID_ARGUMENT");
}

TEST_CASE("unreachable target returns UNAVAILABLE", "[grpc][errors]") {
  GrpcClient c;
  c.load_proto_files({(proto_dir() / "test.proto").string()}, {});
  GrpcTarget t;
  t.address = "127.0.0.1:1";
  auto r = c.invoke(t, "/watt.test.Echo/Say", "{}", {});
  CHECK(r.status_name == "UNAVAILABLE");
  CHECK(r.timing.total_ms < 5000);
}

TEST_CASE("cancel aborts an in-flight call", "[grpc][cancel]") {
  GrpcClient c;
  auto t = TestServer::get().target();
  c.load_via_reflection(t);

  SECTION("unary") {
    std::thread th([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      c.cancel();
    });
    auto r = c.invoke(t, "/watt.test.Echo/Slow", R"({"delayMs":5000})", {});
    th.join();
    CHECK(r.status_name == "CANCELLED");
    CHECK(r.timing.total_ms < 2000);
  }
  SECTION("server stream, from the message callback") {
    auto r = c.invoke(t, "/watt.test.Echo/Count", R"({"count":1000,"intervalMs":10})", {},
                      [&](const GrpcMessageEvent&) { c.cancel(); });
    CHECK(r.status_name == "CANCELLED");
    CHECK(r.messages.size() < 10);
  }
  SECTION("a cancel before a call does not poison the next one") {
    c.cancel();
    auto r = c.invoke(t, "/watt.test.Echo/Say", R"({"message":"ok"})", {});
    CHECK(r.ok);
  }
}

// ---------------------------------------------------------------- benchmarks (hidden; run explicitly)
// watt_test_grpc "[.cold]"   one fresh-process cold call; env WATT_COLD_MODE=reflect|auto,
//                              WATT_COLD_GAP_MS=<ms between constructing the client and calling>.
// watt_test_grpc "[.perf]"   warm unary and 1000-message streams, p50 over several runs.
namespace {
double ms_since(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}
double p50(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v.empty() ? 0 : v[v.size() / 2];
}
std::string env(const char* name) {
  char* v = nullptr;
  size_t n = 0;
  std::string out;
  if (_dupenv_s(&v, &n, name) == 0 && v) out = v;
  std::free(v);
  return out;
}
std::string many(const char* field, int n) {
  std::string s = "[";
  for (int i = 0; i < n; ++i) s += std::string(i ? "," : "") + "{\"" + field + "\":" + (field[0] == 't' ? "\"m\"" : "1") + "}";
  return s + "]";
}
}  // namespace

TEST_CASE("bench: cold first call", "[.cold]") {
  // WATT_SERVER=host:port uses an already-running (warm) server instead of spawning one.
  GrpcTarget t;
  t.address = env("WATT_SERVER");
  if (t.address.empty()) t = TestServer::get().target();
  const std::string gap = env("WATT_COLD_GAP_MS");
  const bool autoreflect = env("WATT_COLD_MODE") == "auto";
  auto t0 = std::chrono::steady_clock::now();
  GrpcClient c;
  double ctor = ms_since(t0);
  if (!gap.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(gap.c_str())));
  auto t1 = std::chrono::steady_clock::now();
  double refl = 0;
  if (!autoreflect) {
    auto s = c.load_via_reflection(t);
    REQUIRE(s.ok);
    refl = ms_since(t1);
  }
  // WATT_COLD_CALL=sum|count|chat runs a 1000-message stream instead of Say.
  const std::string call = env("WATT_COLD_CALL");
  std::string path = "/watt.test.Echo/Say", body = R"({"message":"hi"})";
  if (call == "sum") path = "/watt.test.Echo/Sum", body = many("value", 1000);
  if (call == "count") path = "/watt.test.Echo/Count", body = R"({"count":1000})";
  if (call == "chat") path = "/watt.test.Echo/Chat", body = many("text", 1000);
  auto r = c.invoke(t, path, body, {});
  double total = ms_since(t1);
  REQUIRE(r.ok);
  std::printf("COLD mode=%s ctor=%.3f reflect=%.3f total=%.3f | q=%.3f conn=%.3f refl=%.3f send=%.3f ttfb=%.3f reused=%d\n",
              autoreflect ? "auto" : "reflect", ctor, refl, total, r.timing.queue_ms, r.timing.connect_ms,
              r.reflect_ms, r.timing.send_ms, r.timing.ttfb_ms, int(r.timing.connection_reused));
}

TEST_CASE("bench: warm unary and 1000-message streams", "[.perf]") {
  auto t = TestServer::get().target();
  GrpcClient c;
  REQUIRE(c.load_via_reflection(t).ok);
  std::vector<double> unary;
  for (int i = 0; i < 200; ++i) {
    auto t0 = std::chrono::steady_clock::now();
    REQUIRE(c.invoke(t, "/watt.test.Echo/Say", R"({"message":"hi"})", {}).ok);
    unary.push_back(ms_since(t0));
  }
  std::sort(unary.begin(), unary.end());
  std::printf("PERF unary p50=%.3f p99=%.3f\n", unary[100], unary[198]);
  auto run = [&](const char* name, const char* path, const std::string& body, size_t expect) {
    std::vector<double> v;
    for (int i = 0; i < 15; ++i) {
      auto t0 = std::chrono::steady_clock::now();
      auto r = c.invoke(t, path, body, {});
      v.push_back(ms_since(t0));
      REQUIRE(r.ok);
      REQUIRE(r.messages.size() == expect);
    }
    std::printf("PERF %s p50=%.3f\n", name, p50(v));
  };
  run("server-stream-1000", "/watt.test.Echo/Count", R"({"count":1000})", 1000);
  run("client-stream-1000", "/watt.test.Echo/Sum", many("value", 1000), 1);
  run("bidi-1000", "/watt.test.Echo/Chat", many("text", 1000), 1000);
}

TEST_CASE("auto-reflection fetches only the called service; full listing still works", "[grpc][reflection]") {
  GrpcClient c;
  auto t = TestServer::get().target();
  auto a = c.invoke(t, "/watt.test.Everything/Echo", "{}", {});
  REQUIRE(a.ok);
  REQUIRE(a.reflect_ms > 0);
  // Echo lives in the same file, which the targeted fetch already brought: no second round trip.
  auto b = c.invoke(t, "/watt.test.Echo/Say", R"({"message":"y"})", {});
  REQUIRE(b.ok);
  CHECK(b.reflect_ms == 0);
  auto again = c.invoke(t, "/watt.test.Everything/Echo", "{}", {});  // both stay cached
  REQUIRE(again.ok);
  CHECK(again.reflect_ms == 0);
  check_schema(c.load_via_reflection(t));
}

TEST_CASE("reflection sends metadata", "[grpc][reflection][metadata]") {
  auto t = TestServer::get().target();
  GrpcClient c;
  auto s = c.load_via_reflection(t, 5000, {{"X-Probe", "explicit"}});
  check_schema(s);
  auto r = c.invoke(t, "/watt.test.Echo/Say", "{}", {});
  REQUIRE(r.ok);
  CHECK(header(r.trailers, "x-last-reflection-probe") == "explicit");

  GrpcClient fresh;  // invoke's auto-reflection uses the call's metadata
  GrpcCallOptions o;
  o.metadata = {{"x-probe", "auto"}};
  auto r2 = fresh.invoke(t, "/watt.test.Echo/Say", "{}", o);
  REQUIRE(r2.ok);
  CHECK(r2.reflect_ms > 0);
  CHECK(header(r2.trailers, "x-last-reflection-probe") == "auto");
}

TEST_CASE("preconnect and host names", "[grpc][connect]") {
  auto t = TestServer::get().target();
  GrpcClient c;
  REQUIRE(c.load_proto_files({(proto_dir() / "test.proto").string()}, {}).ok);
  c.preconnect(t);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  auto r = c.invoke(t, "/watt.test.Echo/Say", "{}", {});
  REQUIRE(r.ok);
  CHECK(r.timing.connection_reused);  // the connection was already up
  CHECK(r.timing.connect_ms == 0);

  // "localhost" resolves to ::1 and 127.0.0.1; the server only listens on IPv4.
  GrpcTarget named = t;
  named.address = "localhost:" + t.address.substr(t.address.rfind(':') + 1);
  auto n = c.invoke(named, "/watt.test.Echo/Say", "{}", {});
  INFO(n.status_message);
  REQUIRE(n.ok);
  CHECK_FALSE(n.timing.connection_reused);
  CHECK(n.timing.dns_ms > 0);
  CHECK(n.timing.total_ms < 1000);
}

TEST_CASE("response JSON can keep proto field names", "[grpc][json]") {
  GrpcClient c;
  GrpcCallOptions o;
  o.proto_field_names = true;
  auto r = c.invoke(TestServer::get().target(), "/watt.test.Echo/Say", R"({"message":"p"})", o);
  REQUIRE(r.ok);
  auto d = parse(r.messages[0].json);
  CHECK(d.HasMember("received_at"));
  CHECK_FALSE(d.HasMember("receivedAt"));
}

TEST_CASE("protoset files load like proto sources", "[grpc][proto]") {
  namespace pb = google::protobuf;
  // Compile test.proto in-process and write it as a FileDescriptorSet, like protoc --descriptor_set_out.
  // Disk tree plus the well-known types from the descriptors linked into libprotobuf.
  struct Tree : pb::compiler::SourceTree {
    pb::compiler::DiskSourceTree disk;
    std::vector<std::unique_ptr<std::string>> keep;
    pb::io::ZeroCopyInputStream* Open(absl::string_view name) override {
      if (auto* s = disk.Open(name)) return s;
      const auto* g = pb::DescriptorPool::generated_pool()->FindFileByName(std::string(name));
      if (!g) return nullptr;
      keep.push_back(std::make_unique<std::string>(g->DebugString()));
      return new pb::io::ArrayInputStream(keep.back()->data(), static_cast<int>(keep.back()->size()));
    }
  } tree;
  tree.disk.MapPath("", proto_dir().generic_string());
  struct Quiet : pb::compiler::MultiFileErrorCollector {
    void RecordError(absl::string_view, int, int, absl::string_view) override {}
  } quiet;
  pb::compiler::Importer imp(&tree, &quiet);
  const pb::FileDescriptor* fd = imp.Import("test.proto");
  REQUIRE(fd);
  pb::FileDescriptorSet fds;
  fd->CopyTo(fds.add_file());  // without its WKT deps: those resolve from the linked-in copies
  auto tmp = fs::temp_directory_path() / "watt_test_grpc_protoset";
  fs::create_directories(tmp);
  auto path = tmp / "test.protoset";
  {
    std::ofstream f(path, std::ios::binary);
    f << fds.SerializeAsString();
  }
  GrpcClient p;
  auto s = p.load_proto_files({path.string()}, {});
  INFO(s.error);
  check_schema(s);
  auto r = p.invoke(TestServer::get().target(), "/watt.test.Echo/Say", R"({"message":"set"})", {});
  REQUIRE(r.ok);
  CHECK(r.reflect_ms == 0);  // the protoset supplied the schema
  auto bad = p.load_protoset({(proto_dir() / "test.proto").string()});
  CHECK_FALSE(bad.ok);
  // A relative descriptor-set path is refused, even when it exists relative to the cwd.
  {
    const auto old_cwd = fs::current_path();
    fs::current_path(tmp);
    GrpcClient q;
    auto rel = q.load_protoset({"test.protoset"});
    auto rel2 = q.load_proto_files({"test.protoset"}, {});
    fs::current_path(old_cwd);
    CHECK_FALSE(rel.ok);
    CHECK(rel.error.find("must be absolute") != std::string::npos);
    CHECK_FALSE(rel2.ok);
  }
  fs::remove_all(tmp);
}

TEST_CASE("interactive bidi: send after each reply, then half-close", "[grpc][stream][interactive]") {
  GrpcClient c;
  auto t = TestServer::get().target();
  std::mutex mu;
  std::condition_variable cv;
  int replies = 0;
  auto call = c.start_call(t, "/watt.test.Echo/Chat", {}, [&](const GrpcMessageEvent&) {
    std::lock_guard lk(mu);
    ++replies;
    cv.notify_all();
  });
  REQUIRE(call);
  REQUIRE_FALSE(call->done());
  for (int i = 0; i < 3; ++i) {
    std::string err;
    REQUIRE(call->send(R"({"text":"m)" + std::to_string(i) + R"("})", &err));
    std::unique_lock lk(mu);
    REQUIRE(cv.wait_for(lk, std::chrono::seconds(5), [&] { return replies == i + 1; }));  // reply before next send
  }
  std::string err;
  CHECK_FALSE(call->send(R"({"nope":1})", &err));  // bad JSON is rejected locally, the call continues
  CHECK(err.find("nope") != std::string::npos);
  call->close_send();
  CHECK_FALSE(call->send(R"({"text":"late"})", &err));
  auto r = call->wait();
  INFO(r.status_message);
  REQUIRE(r.ok);
  CHECK(call->done());
  REQUIRE(r.messages.size() == 3);
  CHECK(parse(r.messages[2].json)["text"] == "echo: m2");
  CHECK(header(r.trailers, "x-count") == "3");
  CHECK(r.timing.total_ms > 0);
}

TEST_CASE("interactive client stream, unary, cancel and errors", "[grpc][stream][interactive]") {
  GrpcClient c;
  auto t = TestServer::get().target();
  SECTION("client stream") {
    auto call = c.start_call(t, "/watt.test.Echo/Sum", {});
    for (int i = 1; i <= 4; ++i) REQUIRE(call->send(R"({"value":)" + std::to_string(i) + "}"));
    call->close_send();
    auto r = call->wait();
    REQUIRE(r.ok);
    CHECK(parse(r.messages[0].json)["total"] == "10");
  }
  SECTION("unary takes one message and half-closes by itself") {
    auto call = c.start_call(t, "/watt.test.Echo/Say", {});
    REQUIRE(call->send(R"({"message":"one"})"));
    std::string err;
    CHECK_FALSE(call->send(R"({"message":"two"})", &err));
    auto r = call->wait();
    REQUIRE(r.ok);
    CHECK(parse(r.messages[0].json)["message"] == "one");
  }
  SECTION("cancel") {
    auto call = c.start_call(t, "/watt.test.Echo/Chat", {});
    REQUIRE(call->send(R"({"text":"a"})"));
    call->cancel();
    auto r = call->wait();
    CHECK(r.status_name == "CANCELLED");
  }
  SECTION("destroying an open call cancels it") {
    auto call = c.start_call(t, "/watt.test.Echo/Chat", {});
    REQUIRE(call->send(R"({"text":"a"})"));
    auto t0 = std::chrono::steady_clock::now();
    call.reset();
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
  }
  SECTION("client cancel() reaches interactive calls") {
    auto call = c.start_call(t, "/watt.test.Echo/Chat", {});
    c.cancel();
    CHECK(call->wait().status_name == "CANCELLED");
  }
  SECTION("unknown method: already done with the error") {
    auto call = c.start_call(t, "/watt.test.Echo/Nope", {});
    CHECK(call->done());
    CHECK_FALSE(call->send("{}"));
    CHECK(call->wait().status_name == "NOT_FOUND");
  }
}

TEST_CASE("large client stream is sent in order", "[grpc][stream]") {
  GrpcClient c;
  std::string body = "[";
  for (int i = 1; i <= 2000; ++i) body += std::string(i > 1 ? "," : "") + R"({"value":)" + std::to_string(i) + "}";
  body += "]";
  auto r = c.invoke(TestServer::get().target(), "/watt.test.Echo/Sum", body, {});
  REQUIRE(r.ok);
  auto d = parse(r.messages[0].json);
  CHECK(d["total"] == "2001000");
  CHECK(d["count"] == 2000);
}

// watt_test_grpc "[.tlsbench]"  cold TLS call against a public server (env WATT_TLS_TARGET, default
// demo.connectrpc.com:443, Eliza Say), printing the timing breakdown.
TEST_CASE("bench: cold TLS call", "[.tlsbench]") {
  GrpcTarget t;
  t.address = env("WATT_TLS_TARGET");
  if (t.address.empty()) t.address = "demo.connectrpc.com:443";
  t.tls = true;
  GrpcClient c;
  auto t0 = std::chrono::steady_clock::now();
  auto r = c.invoke(t, "/connectrpc.eliza.v1.ElizaService/Say", R"({"sentence":"hi"})", {});
  double total = ms_since(t0);
  std::printf("TLS %s %s total=%.1f | q=%.2f dns=%.1f conn=%.1f tls=%.1f refl=%.1f send=%.2f ttfb=%.1f\n",
              r.status_name.c_str(), r.status_message.c_str(), total, r.timing.queue_ms, r.timing.dns_ms,
              r.timing.connect_ms, r.timing.tls_ms, r.reflect_ms, r.timing.send_ms, r.timing.ttfb_ms);
  auto s = c.load_via_reflection(t);
  std::printf("TLS full reflection ok=%d load_ms=%.1f methods=%zu\n", int(s.ok), s.load_ms, s.methods.size());
}

// ---------------------------------------------------------------- round 3: connect, TLS, fallback
namespace {
std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
fs::path cert_dir() { return proto_dir() / "certs"; }
std::wstring tls_args(bool mtls) {
  return L"--tls \"" + cert_dir().wstring() + L"\"" + (mtls ? L" --mtls" : L"");
}
GrpcTarget tls_target(const TestServer& s) {
  GrpcTarget t;
  t.address = s.address();
  t.authority = "localhost";  // the name in the test certificate (127.0.0.1 only: no ::1 retry)
  t.tls = true;
  t.root_cert_pem = slurp(cert_dir() / "ca.pem");
  return t;
}
double phase_sum(const GrpcResponse& r) {
  const Timing& t = r.timing;
  return t.queue_ms + t.dns_ms + t.connect_ms + t.tls_ms + r.reflect_ms + t.send_ms + t.ttfb_ms + t.download_ms;
}
}  // namespace

TEST_CASE("auto-reflection against an unreachable server reports UNAVAILABLE as a connect failure",
          "[grpc][errors][reflection]") {
  GrpcClient c;
  GrpcTarget t;
  t.address = "127.0.0.1:1";
  auto r = c.invoke(t, "/watt.test.Echo/Say", "{}", {});
  CHECK(r.status_name == "UNAVAILABLE");
  CHECK(r.status_message.find("not found") == std::string::npos);
  CHECK_FALSE(r.status_message.empty());
  CHECK(r.reflect_ms == 0);
  CHECK(r.timing.connect_ms > 0);
  CHECK(r.timing.total_ms < 5000);
}

TEST_CASE("a cold auto-reflected call splits connect from reflection and the phases add up", "[grpc][timing]") {
  GrpcClient c;
  auto r = c.invoke(TestServer::get().target(), "/watt.test.Echo/Say", R"({"message":"t"})", {});
  REQUIRE(r.ok);
  CHECK_FALSE(r.timing.connection_reused);
  CHECK(r.timing.connect_ms > 0);
  CHECK(r.reflect_ms > 0);
  CHECK(r.timing.tls_ms == 0);  // plaintext
  CHECK(phase_sum(r) <= r.timing.total_ms + 0.01);
  CHECK(phase_sum(r) > r.timing.total_ms * 0.9);
}

TEST_CASE("a long client stream with a bad message fails with its position", "[grpc][stream][errors]") {
  GrpcClient c;
  auto t = TestServer::get().target();
  std::string body = "[";
  for (int i = 1; i <= 300; ++i) {
    body += i > 1 ? "," : "";
    body += i == 200 ? std::string(R"({"value":"not a number"})") : R"({"value":)" + std::to_string(i) + "}";
  }
  body += "]";
  auto r = c.invoke(t, "/watt.test.Echo/Sum", body, {});
  CHECK(r.status_name == "INVALID_ARGUMENT");
  CHECK(r.status_message.find("request message #200") != std::string::npos);
  // A bad first message is caught before any traffic, like a short request.
  auto first = c.invoke(t, "/watt.test.Echo/Sum", "[{\"nope\":1}," + body.substr(1), {});
  CHECK(first.status_name == "INVALID_ARGUMENT");
  CHECK(first.status_message.find("request message #1") != std::string::npos);
  CHECK(first.timing.bytes_sent == 0);
  // A long array for a unary method is still rejected locally.
  auto unary = c.invoke(t, "/watt.test.Echo/Say", body, {});
  CHECK(unary.status_name == "INVALID_ARGUMENT");
  // And a long valid stream still sums correctly after the bad one.
  std::string good = "[";
  for (int i = 1; i <= 300; ++i) good += std::string(i > 1 ? "," : "") + R"({"value":)" + std::to_string(i) + "}";
  auto ok = c.invoke(t, "/watt.test.Echo/Sum", good + "]", {});
  REQUIRE(ok.ok);
  CHECK(parse(ok.messages[0].json)["total"] == "45150");
}

TEST_CASE("TLS: verified, untrusted, skip-verify, with timing split", "[grpc][tls]") {
  auto& s = TestServer::get(tls_args(false));
  GrpcClient c;
  auto t = tls_target(s);
  auto r = c.invoke(t, "/watt.test.Echo/Say", R"({"message":"tls"})", {});
  REQUIRE(r.ok);
  CHECK(parse(r.messages[0].json)["message"] == "tls");
  CHECK(r.timing.connect_ms > 0);
  CHECK(r.timing.tls_ms > 0);
  CHECK(r.reflect_ms > 0);
  CHECK(phase_sum(r) <= r.timing.total_ms + 0.01);
  auto warm = c.invoke(t, "/watt.test.Echo/Say", "{}", {});
  REQUIRE(warm.ok);
  CHECK(warm.timing.connection_reused);
  CHECK(warm.timing.tls_ms == 0);

  GrpcClient c2;
  auto untrusted = t;
  untrusted.root_cert_pem.clear();  // system roots do not know the test CA
  auto bad = c2.invoke(untrusted, "/watt.test.Echo/Say", "{}", {});
  CHECK(bad.status_name == "UNAVAILABLE");

  auto skip = untrusted;
  skip.insecure_skip_verify = true;
  auto r3 = c2.invoke(skip, "/watt.test.Echo/Say", "{}", {});
  CHECK(r3.ok);
}

TEST_CASE("mTLS: the client certificate is required", "[grpc][tls]") {
  auto& s = TestServer::get(tls_args(true));
  auto t = tls_target(s);
  {
    GrpcClient c;
    auto r = c.invoke(t, "/watt.test.Echo/Say", "{}", {});
    CHECK_FALSE(r.ok);
  }
  t.client_cert_pem = slurp(cert_dir() / "client.pem");
  t.client_key_pem = slurp(cert_dir() / "client.key");
  GrpcClient c;
  auto r = c.invoke(t, "/watt.test.Echo/Say", R"({"message":"m"})", {});
  REQUIRE(r.ok);
  CHECK(r.timing.tls_ms > 0);
  check_schema(c.load_via_reflection(t));
}

TEST_CASE("reflection falls back to v1alpha on servers without v1", "[grpc][reflection]") {
  auto& s = TestServer::get(L"--reflection v1alpha");
  auto t = s.target();
  {
    // The server really lacks v1: a raw v1 reflection call is UNIMPLEMENTED.
    auto ch = grpc::CreateChannel(s.address(), grpc::InsecureChannelCredentials());
    grpc::GenericStub stub(ch);
    grpc::ClientContext ctx;
    grpc::CompletionQueue cq;
    auto rw = stub.PrepareCall(&ctx, "/grpc.reflection.v1.ServerReflection/ServerReflectionInfo", &cq);
    rw->StartCall(reinterpret_cast<void*>(1));
    grpc::Status st;
    rw->Finish(&st, reinterpret_cast<void*>(2));
    void* tag;
    bool ok;
    while (cq.Next(&tag, &ok) && tag != reinterpret_cast<void*>(2)) {
    }
    CHECK(st.error_code() == grpc::StatusCode::UNIMPLEMENTED);
    cq.Shutdown();
    while (cq.Next(&tag, &ok)) {
    }
  }
  GrpcClient c;
  auto r = c.invoke(t, "/watt.test.Echo/Say", R"({"message":"alpha"})", {});
  REQUIRE(r.ok);
  CHECK(parse(r.messages[0].json)["message"] == "alpha");
  check_schema(c.load_via_reflection(t));
  // The server's version is remembered: the next listing goes straight to v1alpha.
  check_schema(c.load_via_reflection(t));
}

TEST_CASE("protobuf text format in and out", "[grpc][text]") {
  GrpcClient c;
  auto t = TestServer::get().target();
  GrpcCallOptions o;
  o.format = GrpcFormat::Text;
  auto r = c.invoke(t, "/watt.test.Echo/Say", "message: \"txt\" repeat: 2", o);
  REQUIRE(r.ok);
  REQUIRE(r.messages.size() == 1);
  CHECK(r.messages[0].json.find("message: \"txttxt\"") != std::string::npos);
  CHECK(r.messages[0].json.find("\"message\":") == std::string::npos);  // not JSON
  // A client stream: messages separated by the record separator, as grpcurl -format text does.
  const std::string rs(1, char(0x1E));
  auto s = c.invoke(t, "/watt.test.Echo/Sum", "value: 2" + rs + "value: 3\n" + rs + "value: 5", o);
  REQUIRE(s.ok);
  CHECK(s.messages[0].json.find("total: 10") != std::string::npos);
  auto bad = c.invoke(t, "/watt.test.Echo/Say", "message: 12 nonsense {", o);
  CHECK(bad.status_name == "INVALID_ARGUMENT");
  // Interactive calls use the same format.
  auto call = c.start_call(t, "/watt.test.Echo/Chat", o);
  REQUIRE(call->send("text: \"hi\""));
  call->close_send();
  auto cr = call->wait();
  REQUIRE(cr.ok);
  REQUIRE(cr.messages.size() == 1);
  CHECK(cr.messages[0].json.find("text: \"echo: hi\"") != std::string::npos);
}

TEST_CASE("unknown request fields are an error unless allowed", "[grpc][json]") {
  GrpcClient c;
  auto t = TestServer::get().target();
  auto strict = c.invoke(t, "/watt.test.Echo/Say", R"({"message":"u","extra":1})", {});
  CHECK(strict.status_name == "INVALID_ARGUMENT");
  GrpcCallOptions o;
  o.allow_unknown_fields = true;
  auto lax = c.invoke(t, "/watt.test.Echo/Say", R"({"message":"u","extra":1})", o);
  REQUIRE(lax.ok);
  CHECK(parse(lax.messages[0].json)["message"] == "u");
  o.format = GrpcFormat::Text;
  CHECK(c.invoke(t, "/watt.test.Echo/Say", "message: \"u\" extra: 1", o).ok);
}

TEST_CASE("describe shows definitions as proto source; protosets export and reload", "[grpc][proto]") {
  GrpcClient c;
  REQUIRE(c.load_proto_files({(proto_dir() / "test.proto").string()}, {}).ok);
  auto svc = c.describe("watt.test.Echo");
  CHECK(svc.find("service Echo {") != std::string::npos);
  CHECK(svc.find("rpc Sum(stream .watt.test.SumRequest) returns (.watt.test.SumResponse)") != std::string::npos);
  CHECK(c.describe("/watt.test.Echo/Say").find("rpc Say(") != std::string::npos);
  auto msg = c.describe("watt.test.EchoRequest");
  CHECK(msg.find("message EchoRequest {") != std::string::npos);
  CHECK(msg.find("response message = message repeated N times") != std::string::npos);  // comments kept
  CHECK(c.describe("watt.test.Nope").empty());

  auto path = (fs::temp_directory_path() / ("watt_export_" + std::to_string(GetCurrentProcessId()) + ".protoset")).string();
  std::string err;
  REQUIRE(c.export_protoset(path, {"watt.test.Echo"}, &err));
  CHECK_FALSE(c.export_protoset(path + ".x", {"watt.test.Nope"}, &err));
  CHECK(err.find("watt.test.Nope") != std::string::npos);
  GrpcClient c2;
  auto s = c2.load_protoset({path});
  check_schema(s);
  auto r = c2.invoke(TestServer::get().target(), "/watt.test.Echo/Say", R"({"message":"set"})", {});
  REQUIRE(r.ok);
  CHECK(r.reflect_ms == 0);  // the exported set had everything
  fs::remove(path);

  // A reflected schema exports too.
  GrpcClient c3;
  REQUIRE(c3.load_via_reflection(TestServer::get().target()).ok);
  REQUIRE(c3.export_protoset(path, {}, &err));
  GrpcClient c4;
  check_schema(c4.load_protoset({path}));
  fs::remove(path);
}

TEST_CASE("channel options: user agent, message size limit, keepalive, connect timeout", "[grpc][connect]") {
  auto t = TestServer::get().target();
  GrpcClient c;
  REQUIRE(c.load_proto_files({(proto_dir() / "test.proto").string()}, {}).ok);
  t.user_agent = "watt-test/1.0";
  t.keepalive_ms = 10000;
  auto r = c.invoke(t, "/watt.test.Echo/Say", R"({"message":"ua"})", {});
  REQUIRE(r.ok);
  std::string ua = parse(r.messages[0].json)["metadata"]["user-agent"].GetString();
  CHECK(ua.rfind("watt-test/1.0", 0) == 0);

  t = TestServer::get().target();
  t.max_message_bytes = 1000;
  auto big = c.invoke(t, "/watt.test.Echo/Say", R"({"message":"0123456789","repeat":500})", {});
  CHECK_FALSE(big.ok);  // RESOURCE_EXHAUSTED, or CANCELLED when gRPC's cancel races the server's trailers
  auto small = c.invoke(t, "/watt.test.Echo/Say", R"({"message":"0123456789","repeat":5})", {});
  CHECK(small.ok);

  GrpcTarget dead;
  dead.address = "10.255.255.1:9";  // not routable: the connect never completes on its own
  dead.connect_timeout_ms = 300;
  auto t0 = std::chrono::steady_clock::now();
  auto d = c.invoke(dead, "/watt.test.Echo/Say", "{}", {});
  CHECK_FALSE(d.ok);
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(3000));
  GrpcClient cold;  // the same through auto-reflection
  t0 = std::chrono::steady_clock::now();
  auto d2 = cold.invoke(dead, "/watt.test.Echo/Say", "{}", {});
  CHECK_FALSE(d2.ok);
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(3000));
}

// ---------------------------------------------------------------- round 4: exit, TLS name, output
TEST_CASE("a TLS call does not leave gRPC's 2 s OpenSSL cleanup wait at process exit", "[grpc][tls]") {
  GrpcClient c;  // constructing a client is enough; a value the user set is respected
  char buf[16] = {};
  const DWORD n = GetEnvironmentVariableA("grpc.openssl_cleanup_timeout", buf, sizeof(buf));
  REQUIRE(n > 0);
  CHECK(std::string(buf) == "0");
}

TEST_CASE("TLS server name is independent of :authority", "[grpc][tls]") {
  auto& s = TestServer::get(tls_args(false));
  GrpcClient c;
  auto t = tls_target(s);
  t.authority = "api.example.test";  // not the certificate's name
  auto bad = c.invoke(t, "/watt.test.Echo/Say", "{}", {});
  CHECK(bad.status_name == "UNAVAILABLE");  // the authority doubles as the TLS name by default

  t.server_name = "localhost";  // the certificate's name, :authority stays as set
  auto r = c.invoke(t, "/watt.test.Echo/Say", "{}", {});
  INFO(r.status_message);
  REQUIRE(r.ok);
  CHECK(header(r.headers, "x-authority") == "api.example.test");
}

TEST_CASE("response JSON: defaults omitted on request, compact on one line", "[grpc][json]") {
  auto t = TestServer::get().target();
  GrpcClient c;
  const std::string req = R"({"fInt32":0,"fString":"","fBool":true})";
  auto full = c.invoke(t, "/watt.test.Everything/Echo", req, {});
  REQUIRE(full.ok);
  auto d = parse(full.messages[0].json);
  CHECK(d.HasMember("fInt32"));  // default: every field, like grpcurl -emit-defaults
  CHECK(full.messages[0].json.find('\n') != std::string::npos);

  GrpcCallOptions o;
  o.emit_defaults = false;
  o.compact = true;
  auto lean = c.invoke(t, "/watt.test.Everything/Echo", req, o);
  REQUIRE(lean.ok);
  CHECK(lean.messages[0].json == R"({"fBool":true})");

  o.format = GrpcFormat::Text;
  auto text = c.invoke(t, "/watt.test.Everything/Echo", "f_bool: true f_int32: 7", o);
  REQUIRE(text.ok);
  CHECK(text.messages[0].json.find('\n') == std::string::npos);
  CHECK(text.messages[0].json.find("f_int32: 7") != std::string::npos);
}

TEST_CASE("metadata can go to reflection only or to the RPC only", "[grpc][reflection][metadata]") {
  auto t = TestServer::get().target();
  GrpcClient c;
  GrpcCallOptions o;
  o.reflect_metadata = {{"x-probe", "reflect-only"}};
  o.rpc_metadata = {{"x-rpc", "rpc-only"}};
  auto r = c.invoke(t, "/watt.test.Echo/Say", "{}", o);
  REQUIRE(r.ok);
  CHECK(r.reflect_ms > 0);
  CHECK(header(r.trailers, "x-last-reflection-probe") == "reflect-only");
  CHECK(header(r.headers, "echo-x-rpc") == "rpc-only");
  CHECK(header(r.headers, "echo-x-probe").empty());

  GrpcClient c2;  // RPC-only metadata never reaches reflection
  GrpcCallOptions o2;
  o2.rpc_metadata = {{"x-probe", "rpc"}};
  auto r2 = c2.invoke(t, "/watt.test.Echo/Say", "{}", o2);
  REQUIRE(r2.ok);
  CHECK(r2.reflect_ms > 0);
  CHECK(header(r2.trailers, "x-last-reflection-probe") == "(none)");
}

TEST_CASE("the loaded schema exports as .proto files", "[grpc][proto]") {
  auto t = TestServer::get().target();
  GrpcClient c;
  REQUIRE(c.load_via_reflection(t).ok);
  auto dir = fs::temp_directory_path() / ("watt_proto_out_" + std::to_string(GetCurrentProcessId()));
  fs::remove_all(dir);
  std::string err;
  REQUIRE(c.export_proto_files(dir.string(), {"watt.test.Everything"}, &err));
  CHECK(err.empty());
  CHECK(fs::exists(dir / "google" / "protobuf" / "timestamp.proto"));
  fs::path main_file;
  for (auto& e : fs::recursive_directory_iterator(dir))
    if (e.is_regular_file() && slurp(e.path()).find("service Everything") != std::string::npos) main_file = e.path();
  REQUIRE_FALSE(main_file.empty());
  GrpcClient reload;  // what was written is valid .proto that loads again
  auto s = reload.load_proto_files({main_file.string()}, {dir.string()});
  INFO(s.error);
  CHECK(s.ok);
  CHECK_FALSE(c.export_proto_files(dir.string(), {"no.such.Symbol"}, &err));
  fs::remove_all(dir);
}

TEST_CASE("example requests carry meaningful values and round-trip", "[grpc][template]") {
  auto t = TestServer::get().target();
  GrpcClient c;
  REQUIRE(c.load_proto_files({(proto_dir() / "test.proto").string()}, {}).ok);
  CHECK(c.example_request("/no.Such/Method").empty());
  std::vector<std::string> notes;
  const std::string ex = c.example_request("/watt.test.Everything/Echo", &notes);
  INFO(ex);
  REQUIRE_FALSE(ex.empty());
  CHECK_FALSE(notes.empty());
  auto d = parse(ex);
  CHECK(d["fInt32"].GetInt() == 10);
  CHECK(d["fBool"].GetBool());
  CHECK(std::string(d["fString"].GetString()) == "Hello");
  auto r = c.invoke(t, "/watt.test.Everything/Echo", ex, {});  // valid as is
  INFO(r.status_message);
  REQUIRE(r.ok);
  auto back = parse(r.messages[0].json);
  CHECK(back["fInt32"].GetInt() == 10);

  const std::string say = c.example_request("watt.test.Echo.Say");
  auto s = parse(say);
  REQUIRE(s.HasMember("message"));
  CHECK(std::string(s["message"].GetString()) == "Hello");
  CHECK(c.invoke(t, "/watt.test.Echo/Say", say, {}).ok);
}

TEST_CASE("a closed local port is refused at once, not after Windows' 2 s of SYN retries", "[grpc][errors][connect]") {
  for (const char* address : {"127.0.0.1:1", "localhost:1"}) {
    INFO(address);
    GrpcTarget t;
    t.address = address;
    GrpcClient cold;  // auto-reflection path
    auto r = cold.invoke(t, "/watt.test.Echo/Say", "{}", {});
    CHECK(r.status_name == "UNAVAILABLE");
    CHECK(r.status_message.find("refused") != std::string::npos);
    CHECK(r.timing.total_ms < 600);

    GrpcClient c;  // schema already loaded: the plain connect path
    REQUIRE(c.load_proto_files({(proto_dir() / "test.proto").string()}, {}).ok);
    auto r2 = c.invoke(t, "/watt.test.Echo/Say", "{}", {});
    CHECK(r2.status_name == "UNAVAILABLE");
    CHECK(r2.status_message.find("refused") != std::string::npos);
    CHECK(r2.timing.total_ms < 600);

    auto t0 = std::chrono::steady_clock::now();
    auto s = c.load_via_reflection(t, 3000);
    CHECK_FALSE(s.ok);
    CHECK(s.error.find("refused") != std::string::npos);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(600));
  }
}

TEST_CASE("invalid UTF-8 in a response string is shown with U+FFFD and a note, as grpcurl does", "[grpc][utf8]") {
  GrpcClient c;
  auto t = TestServer::get().target();
  // The server copies request metadata into EchoResponse.metadata (map<string,string>): raw bytes
  // in a -bin value become an invalid proto3 string that the strict parser rejects.
  for (const auto& md : std::vector<Metadata>{{{"x-data-bin", "\x01\x02\xff"}}, {{"x-data-bin", "AQL/"}}}) {
    GrpcCallOptions o;
    o.metadata = md;  // the second form is base64 (grpcurl -H 'x-data-bin: AQL/'): the same three bytes
    auto r = c.invoke(t, "/watt.test.Echo/Say", R"({"message":"hi"})", o);
    INFO(r.status_message);
    REQUIRE(r.ok);
    REQUIRE(r.messages.size() == 1);
    const auto& m = r.messages[0];
    INFO(m.json);
    CHECK(m.decode_error.empty());
    auto d = parse(m.json);
    REQUIRE(d.IsObject());
    CHECK(d["message"] == "hi");
    REQUIRE(d["metadata"].HasMember("x-data-bin"));
    CHECK(std::string(d["metadata"]["x-data-bin"].GetString()) == "\x01\x02\xEF\xBF\xBD");
    CHECK(d.HasMember("receivedAt"));
    REQUIRE(m.notes.size() == 1);
    CHECK(m.notes[0] == "metadata[x-data-bin]: invalid UTF-8 replaced with U+FFFD");
    CHECK(header(r.headers, "echo-x-data-bin") == "AQL/");  // binary metadata shown as base64
  }
  // Text format takes the same lenient path.
  GrpcCallOptions o;
  o.metadata = {{"x-data-bin", "AQL/"}};
  o.format = GrpcFormat::Text;
  auto r = c.invoke(t, "/watt.test.Echo/Say", R"(message: "hi")", o);
  REQUIRE(r.ok);
  REQUIRE(r.messages.size() == 1);
  CHECK(r.messages[0].json.find("message: \"hi\"") != std::string::npos);
  CHECK(r.messages[0].notes.size() == 1);

  // A well-formed response has no notes.
  auto ok = c.invoke(t, "/watt.test.Echo/Say", R"({"message":"hi"})", {});
  REQUIRE(ok.ok);
  CHECK(ok.messages[0].notes.empty());
}

TEST_CASE("an undecodable response message is an error with raw bytes, never a made-up body", "[grpc][utf8]") {
  // A schema that reads EchoResponse.message as a nested message: the server's "\x07" is then a
  // field with number 0, which no parser accepts.
  fs::path dir = fs::temp_directory_path() / ("watt_garbled_" + std::to_string(GetCurrentProcessId()));
  fs::create_directories(dir);
  {
    std::ofstream f(dir / "garbled.proto");
    f << "syntax = \"proto3\";\npackage watt.test;\nmessage EchoRequest { string message = 1; }\n"
         "message Garbled { int32 x = 1; }\nmessage EchoResponse { Garbled message = 1; }\n"
         "service Echo { rpc Say(EchoRequest) returns (EchoResponse); }\n";
  }
  GrpcClient c;
  auto s = c.load_proto_files({(dir / "garbled.proto").string()}, {dir.string()});
  INFO(s.error);
  REQUIRE(s.ok);
  auto r = c.invoke(TestServer::get().target(), "/watt.test.Echo/Say", R"({"message":"\u0007"})", {});
  CHECK_FALSE(r.ok);
  CHECK(r.status_name == "INTERNAL");
  CHECK(r.status_message.find("message #1") != std::string::npos);
  CHECK(r.status_message.find("field number 0") != std::string::npos);
  REQUIRE(r.messages.size() == 1);
  const auto& m = r.messages[0];
  CHECK(m.json.empty());
  INFO(m.decode_error);
  CHECK(m.decode_error.find("invalid tag (field number 0) at byte 2") != std::string::npos);
  CHECK(m.raw_hex.rfind("0a0107", 0) == 0);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("example notes describe the example, and Struct / Value examples carry a sample", "[grpc][template]") {
  GrpcClient c;
  REQUIRE(c.load_proto_files({(proto_dir() / "test.proto").string()}, {}).ok);
  std::vector<std::string> notes;
  const std::string ex = c.example_request("/watt.test.Everything/Echo", &notes);
  INFO(ex);
  auto d = parse(ex);
  CHECK(d["level"] == "LEVEL_LOW");
  CHECK(d["meta"]["key"] == "Hello");
  CHECK(d["value"] == "Hello");
  auto has_note = [&](const std::string& prefix) {
    for (const auto& n : notes)
      if (n.rfind(prefix, 0) == 0) return true;
    return false;
  };
  for (const auto& n : notes) {
    INFO(n);
    CHECK(n.find("template") == std::string::npos);
    CHECK(n.find("(example uses LEVEL_UNSPECIFIED)") == std::string::npos);
  }
  CHECK(has_note("level: enum watt.test.Level, one of LEVEL_UNSPECIFIED | "));
  bool level_note = false;
  for (const auto& n : notes) level_note |= n.rfind("level: enum", 0) == 0 && n.find("(example uses LEVEL_LOW)") != std::string::npos;
  CHECK(level_note);
  CHECK(has_note("maybe: optional, leave it out to send it unset (the example sets it)"));
  CHECK(has_note("choice: oneof, set exactly one of text | number | nested (example uses text)"));
  auto r = c.invoke(TestServer::get().target(), "/watt.test.Everything/Echo", ex, {});
  INFO(r.status_message);
  REQUIRE(r.ok);
  auto back = parse(r.messages[0].json);
  CHECK(back["meta"]["key"] == "Hello");
  CHECK(back["value"] == "Hello");
}

TEST_CASE("the UTF-8 repair walks nested, repeated and map fields and reports malformed wire data", "[grpc][utf8]") {
  // Kitchen's string-bearing fields, with the same numbers, without the well-known-type imports.
  fs::path dir = fs::temp_directory_path() / ("watt_repair_" + std::to_string(GetCurrentProcessId()));
  fs::create_directories(dir);
  {
    std::ofstream f(dir / "k.proto");
    f << "syntax = \"proto3\";\npackage watt.test;\nmessage Inner { string name = 1; }\n"
         "message Kitchen { string f_string = 14; bytes f_bytes = 15; Inner inner = 17; repeated string tags = 18;\n"
         "  repeated Inner items = 19; map<string, int32> counts = 21; map<int32, Inner> by_id = 22; }\n";
  }
  google::protobuf::compiler::DiskSourceTree tree;
  tree.MapPath("", dir.generic_string());
  struct Quiet : google::protobuf::compiler::MultiFileErrorCollector {
    void RecordError(absl::string_view, int, int, absl::string_view) override {}
  } quiet;
  google::protobuf::compiler::Importer imp(&tree, &quiet);
  REQUIRE(imp.Import("k.proto"));
  const auto* kitchen = imp.pool()->FindMessageTypeByName("watt.test.Kitchen");
  REQUIRE(kitchen);

  auto ld = [](int field, const std::string& payload) {
    std::string s;
    for (unsigned tag = unsigned(field) << 3 | 2; ; tag >>= 7) {  // varint tag (2 bytes from field 16)
      if (tag < 0x80) {
        s += char(tag);
        break;
      }
      s += char((tag & 0x7F) | 0x80);
    }
    s += char(payload.size());  // all payloads here are < 128 bytes
    return s + payload;
  };
  std::string wire;
  wire += ld(14, "ok");                                             // f_string: valid
  wire += ld(17, ld(1, std::string("a\xff", 2)));                   // inner.name
  wire += ld(18, "fine") + ld(18, "\x80");                          // tags[1]
  wire += ld(19, "") + ld(19, ld(1, "\xc3"));                       // items[1].name
  wire += ld(21, ld(1, "k\xff") + std::string("\x10\x03", 2));      // counts["k�"] key
  wire += ld(22, std::string("\x08\x07", 2) + ld(2, ld(1, "\xfe"))); // by_id[7].name
  wire += ld(15, "\xff");                                           // f_bytes: bytes are not checked

  google::protobuf::DynamicMessageFactory factory(imp.pool());
  std::unique_ptr<google::protobuf::Message> m(factory.GetPrototype(kitchen)->New());
  CHECK_FALSE(m->ParseFromString(wire));

  auto fix = detail::repair_utf8(wire, kitchen);
  INFO(fix.error);
  REQUIRE(fix.ok);
  CHECK(fix.notes == std::vector<std::string>{
                         "inner.name: invalid UTF-8 replaced with U+FFFD",
                         "tags[1]: invalid UTF-8 replaced with U+FFFD",
                         "items[1].name: invalid UTF-8 replaced with U+FFFD",
                         "counts[k\xEF\xBF\xBD] (key): invalid UTF-8 replaced with U+FFFD",
                         "by_id[7].name: invalid UTF-8 replaced with U+FFFD",
                     });
  REQUIRE(m->ParseFromString(fix.bytes));
  const auto* r = m->GetReflection();
  CHECK(r->GetString(*m, kitchen->FindFieldByName("f_bytes")) == "\xff");
  CHECK(r->GetString(*m, kitchen->FindFieldByName("f_string")) == "ok");

  std::string out;
  CHECK_FALSE(detail::fix_utf8("plain \xE2\x82\xAC \xF0\x9F\x98\x80", out));
  REQUIRE(detail::fix_utf8("\xED\xA0\x80" "a\xC0\xAF", out));  // a surrogate and an overlong '/'
  CHECK(out == "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD" "a\xEF\xBF\xBD\xEF\xBF\xBD");

  auto bad = detail::repair_utf8(std::string("\x72\x05" "ab", 4), kitchen);
  CHECK_FALSE(bad.ok);
  CHECK(bad.error == "length 5 of field f_string runs past the end at byte 1");
  bad = detail::repair_utf8(std::string("\x08\x80", 2), kitchen);
  CHECK(bad.error == "truncated varint at byte 1");
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ------------------------------------------------------------------ google.protobuf.Any
// pack.proto imports only any.proto: every type packed into Packer's Anys is outside the service's
// file closure (google.protobuf.Duration, watt.extra.Note from extra.proto, foo.Unknown).

namespace {
GrpcResponse pack(GrpcClient& c, const std::string& body, GrpcCallOptions o = {}) {
  return c.invoke(TestServer::get().target(), "/watt.test.Packer/Pack", body, o);
}
std::string str(const rapidjson::Value& v) { return v.IsString() ? std::string(v.GetString(), v.GetStringLength()) : "(not a string)"; }
}  // namespace

TEST_CASE("Any of a well-known type outside the schema's files is sent and printed", "[grpc][any]") {
  GrpcClient c;
  SECTION("reflection") {}
  SECTION(".proto files") {
    auto s = c.load_proto_files({(proto_dir() / "pack.proto").string()}, {proto_dir().string()});
    INFO(s.error);
    REQUIRE(s.ok);
  }
  auto r = pack(c, R"({"kind":"duration","items":[{"@type":"type.googleapis.com/google.protobuf.Duration","value":"2s"}]})");
  INFO(r.status_message);
  REQUIRE(r.ok);
  REQUIRE(r.messages.size() == 1);
  const auto& ev = r.messages[0];
  CHECK(ev.decode_error.empty());
  CHECK(ev.notes.empty());
  auto d = parse(ev.json);
  REQUIRE(d.IsObject());
  CHECK(str(d["result"]["@type"]) == "type.googleapis.com/google.protobuf.Duration");
  CHECK(str(d["result"]["value"]) == "2s");
  CHECK(str(d["items"][0]["value"]) == "2s");
  CHECK(str(d["byName"]["result"]["value"]) == "2s");
  CHECK(ev.json.find("\n  \"result\": {") != std::string::npos);  // 2-space indent like every response

  // The same Any again: resolved once per schema.
  auto again = pack(c, R"({"kind":"duration"})");
  CHECK(again.ok);
}

TEST_CASE("Any types from files outside the closure are fetched by reflection on demand", "[grpc][any][reflection]") {
  GrpcClient c;
  // Auto-reflection loads pack.proto (+ any.proto) only; watt.extra.Note lives in extra.proto.
  auto r = pack(c, R"({"kind":"nested","items":[{"@type":"type.googleapis.com/watt.extra.Note","text":"hi","priority":7}]})");
  INFO(r.status_message);
  REQUIRE(r.ok);
  REQUIRE(r.messages.size() == 1);
  CHECK(r.messages[0].notes.empty());
  auto d = parse(r.messages[0].json);
  REQUIRE(d.IsObject());
  CHECK(str(d["items"][0]["@type"]) == "type.googleapis.com/watt.extra.Note");
  CHECK(str(d["items"][0]["text"]) == "hi");
  CHECK(d["items"][0]["priority"] == 7);
  CHECK(str(d["result"]["@type"]) == "type.googleapis.com/watt.extra.Note");
  CHECK(str(d["result"]["text"]) == "packed");
  CHECK(str(d["result"]["attachment"]["value"]) == "2s");  // an Any inside the fetched type
  // The fetch is part of reflect_ms and the phases still add up.
  CHECK(r.reflect_ms > 0);
  const auto& t = r.timing;
  const double sum = t.queue_ms + t.dns_ms + t.connect_ms + t.tls_ms + r.reflect_ms + t.send_ms + t.ttfb_ms + t.download_ms;
  CHECK(sum <= t.total_ms + 0.05);

  // The response-side fetch on its own, after a full listing (which lacks extra.proto as well).
  GrpcClient c2;
  REQUIRE(c2.load_via_reflection(TestServer::get().target()).ok);
  auto r2 = pack(c2, R"({"kind":"note"})");
  INFO(r2.status_message);
  REQUIRE(r2.ok);
  auto d2 = parse(r2.messages.at(0).json);
  CHECK(str(d2["result"]["text"]) == "packed");
  CHECK(d2["result"]["priority"] == 3);
  CHECK(r2.reflect_ms > 0);
  CHECK(c2.describe("watt.extra.Note").find("message Note") != std::string::npos);
}

TEST_CASE("Any of an unknown type is printed as @error/@type/@value and keeps the server's status", "[grpc][any]") {
  GrpcClient c;
  const std::string kErr = "foo.Unknown is not recognized; see @value for raw binary message data";
  auto check_unknown = [&](const rapidjson::Value& v, const std::string& type = "type.googleapis.com/foo.Unknown",
                           const std::string& value = "CAI=") {
    REQUIRE(v.IsObject());
    CHECK(v.MemberCount() == 3);
    CHECK(str(v["@type"]) == type);
    CHECK(str(v["@value"]) == value);
    CHECK(std::string(v.MemberBegin()->name.GetString()) == "@error");  // grpcurl's key order
  };
  auto r = pack(c, R"({"kind":"unknown"})");
  INFO(r.status_message);
  CHECK(r.ok);
  CHECK(r.status_name == "OK");
  REQUIRE(r.messages.size() == 1);
  const auto& ev = r.messages[0];
  CHECK(ev.decode_error.empty());
  CHECK(ev.raw_hex.empty());
  auto d = parse(ev.json);
  REQUIRE(d.IsObject());
  check_unknown(d["result"]);
  CHECK(str(d["result"]["@error"]) == kErr);
  check_unknown(d["byName"]["result"]);
  REQUIRE(ev.notes.size() == 2);
  CHECK(ev.notes[0].find("result: google.protobuf.Any of unknown type foo.Unknown") == 0);
  CHECK(ev.json.find("\n  \"result\": {\n    \"@error\"") != std::string::npos);

  SECTION("nested inside a known Any") {
    auto n = pack(c, R"({"kind":"nested_unknown"})");
    REQUIRE(n.ok);
    auto dn = parse(n.messages.at(0).json);
    CHECK(str(dn["result"]["text"]) == "packed");
    check_unknown(dn["result"]["attachment"]);
  }
  SECTION("compact and proto field names") {
    GrpcCallOptions o;
    o.compact = true;
    o.proto_field_names = true;
    auto n = pack(c, R"({"kind":"unknown"})", o);
    REQUIRE(n.ok);
    const auto& js = n.messages.at(0).json;
    CHECK(js.find('\n') == std::string::npos);
    auto dn = parse(js);
    check_unknown(dn["by_name"]["result"]);
  }
  SECTION(".proto schema without reflection: extra.proto's Note is unknown too") {
    GrpcClient p;
    REQUIRE(p.load_proto_files({(proto_dir() / "pack.proto").string()}, {proto_dir().string()}).ok);
    auto n = pack(p, R"({"kind":"note"})");
    INFO(n.status_message);
    REQUIRE(n.ok);
    auto dn = parse(n.messages.at(0).json);
    REQUIRE(dn["result"].IsObject());
    CHECK(str(dn["result"]["@type"]) == "type.googleapis.com/watt.extra.Note");
    CHECK(str(dn["result"]["@error"]) == "watt.extra.Note is not recognized; see @value for raw binary message data");
  }
  SECTION("a request naming an unknown type fails locally") {
    auto n = pack(c, R"({"items":[{"@type":"type.googleapis.com/foo.Unknown","x":1}]})");
    CHECK(n.status_name == "INVALID_ARGUMENT");
    CHECK(n.status_message.find("could not find @type 'type.googleapis.com/foo.Unknown'") != std::string::npos);
  }
}

TEST_CASE("Any request templates name a sendable type", "[grpc][any][template]") {
  GrpcClient c;
  auto s = c.load_via_reflection(TestServer::get().target());
  REQUIRE(s.ok);
  const auto* m = find(s, "/watt.test.Packer/Pack");
  REQUIRE(m);
  auto d = parse(m->request_template);
  REQUIRE(d.IsObject());
  REQUIRE(d["items"].IsArray());
  CHECK(str(d["items"][0]["@type"]) == "type.googleapis.com/google.protobuf.Empty");
  bool noted = false;
  for (const auto& n : m->template_notes) noted |= n.find("items[]: google.protobuf.Any") == 0;
  CHECK(noted);
  auto r = pack(c, m->request_template);
  INFO(r.status_message);
  CHECK(r.ok);

  std::vector<std::string> notes;
  const std::string ex = c.example_request("/watt.test.Packer/Pack", &notes);
  auto e = parse(ex);
  REQUIRE(e.IsObject());
  CHECK(str(e["items"][0]["@type"]) == "type.googleapis.com/watt.test.PackResponse");  // a real type of the file
  auto r2 = pack(c, ex);
  INFO(ex);
  INFO(r2.status_message);
  REQUIRE(r2.ok);
  CHECK(str(parse(r2.messages.at(0).json)["items"][0]["@type"]) == "type.googleapis.com/watt.test.PackResponse");
}

TEST_CASE("huge string fields print exactly as protobuf's JSON printer does, fast", "[grpc][large]") {
  // Strings of 64 KiB and more skip protobuf's per-code-point escaping; the output must not change.
  GrpcClient c;
  const std::string piece =
      "plain ASCII text, \"quoted\" \\ back/slash <tag> & \x01\x1f\x7f\t\n\r\b\f "
      "\xC3\xA9 \xC2\xAD \xE2\x80\xA8 \xEF\xBB\xBF \xF0\x9F\x98\x80 \xF3\xA0\x80\x81 \xF0\x9D\x85\xB3 \xD8\x80 end.";
  const int repeat = 2000;  // ~170 KB
  rapidjson::Document req(rapidjson::kObjectType);
  req.AddMember("message", rapidjson::Value(piece.c_str(), static_cast<rapidjson::SizeType>(piece.size()), req.GetAllocator()),
                req.GetAllocator());
  req.AddMember("repeat", repeat, req.GetAllocator());
  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> w(sb);
  req.Accept(w);
  for (bool compact : {false, true}) {
    GrpcCallOptions o;
    o.compact = compact;
    auto r = c.invoke(TestServer::get().target(), "/watt.test.Echo/Say", sb.GetString(), o);
    INFO(r.status_message);
    REQUIRE(r.ok);
    REQUIRE(r.messages.size() == 1);
    std::string full;
    for (int i = 0; i < repeat; ++i) full += piece;
    google::protobuf::StringValue sv;
    sv.set_value(full);
    std::string expected;
    REQUIRE(google::protobuf::util::MessageToJsonString(sv, &expected).ok());
    const std::string key = compact ? "\"message\":" : "\"message\": ";
    CHECK(r.messages[0].json.find(key + expected + ",") != std::string::npos);
    auto d = parse(r.messages[0].json);
    REQUIRE(d.IsObject());
    CHECK(str(d["message"]) == full);
    CHECK(d.HasMember("receivedAt"));
  }
}

TEST_CASE("a 12 MB response decodes in well under the transfer time", "[grpc][large][perf]") {
  GrpcClient c;
  GrpcCallOptions o;
  REQUIRE(c.invoke(TestServer::get().target(), "/watt.test.Echo/Say", R"({"message":"x"})", o).ok);
  auto r = c.invoke(TestServer::get().target(), "/watt.test.Echo/Say", R"({"message":"abcdefgh","repeat":1500000})", o);
  REQUIRE(r.ok);
  REQUIRE(r.messages.size() == 1);
  CHECK(r.messages[0].json.size() > 12000000);
  std::printf("12MB response: total=%.1f ttfb=%.1f download(decode)=%.1f ms\n", r.timing.total_ms, r.timing.ttfb_ms,
              r.timing.download_ms);
  CHECK(r.timing.download_ms < 60);  // was ~155 ms with protobuf's printer alone
}

// ---------------------------------------------------------------- gRPC-Web (tools/testserver/grpcweb.cpp)
namespace {
// One-shot fake gRPC-Web server: answers the first request with `frames` as the body (chunked),
// then keeps the connection open for `hold_ms`, as a hostile server would.
struct HostileWebServer {
  SOCKET ls = INVALID_SOCKET;
  int port = 0;
  std::thread th;
  HostileWebServer(std::string frames, int hold_ms) {
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof a);
    int n = sizeof a;
    getsockname(ls, reinterpret_cast<sockaddr*>(&a), &n);
    port = ntohs(a.sin_port);
    listen(ls, 1);
    th = std::thread([this, frames = std::move(frames), hold_ms] {
      SOCKET c = accept(ls, nullptr, nullptr);
      if (c == INVALID_SOCKET) return;
      char buf[8192];
      recv(c, buf, sizeof buf, 0);  // the request: contents do not matter
      char hex[16];
      std::snprintf(hex, sizeof hex, "%zx\r\n", frames.size());
      std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/grpc-web+proto\r\nTransfer-Encoding: chunked\r\n\r\n";
      resp += hex + frames + "\r\n";
      send(c, resp.data(), static_cast<int>(resp.size()), 0);
      std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
      closesocket(c);
    });
  }
  ~HostileWebServer() {
    closesocket(ls);
    if (th.joinable()) th.join();
  }
};
std::string web_frame(uint8_t flag, uint32_t len, const std::string& body) {
  std::string f(5, '\0');
  f[0] = static_cast<char>(flag);
  f[1] = static_cast<char>(len >> 24);
  f[2] = static_cast<char>(len >> 16);
  f[3] = static_cast<char>(len >> 8);
  f[4] = static_cast<char>(len);
  return f + body;
}
}  // namespace

TEST_CASE("gRPC-Web trailer frames are capped before they are buffered", "[grpc][web][security]") {
  GrpcClient c;
  REQUIRE(c.load_proto_files({(proto_dir() / "test.proto").string()}, {}).ok);
  GrpcCallOptions o;
  o.deadline_ms = 10000;
  SECTION("a 1 GiB trailer header fails at once, without waiting for (or holding) the body") {
    HostileWebServer s(web_frame(0x80, 1u << 30, "grpc-status: 0\r\n"), 4000);
    GrpcTarget t;
    t.address = "127.0.0.1:" + std::to_string(s.port);
    t.web = true;
    const auto t0 = std::chrono::steady_clock::now();
    auto r = c.invoke(t, "/watt.test.Echo/Say", R"({"message":"x"})", o);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    INFO(r.status_message);
    CHECK_FALSE(r.ok);
    CHECK(r.status_code == static_cast<int>(grpc::StatusCode::RESOURCE_EXHAUSTED));
    CHECK(ms < 2000);  // the server holds the connection for 4 s
  }
  SECTION("a second trailer frame is refused") {
    const std::string tr = "grpc-status: 0\r\n";
    HostileWebServer s(web_frame(0x80, static_cast<uint32_t>(tr.size()), tr) + web_frame(0x80, static_cast<uint32_t>(tr.size()), tr), 300);
    GrpcTarget t;
    t.address = "127.0.0.1:" + std::to_string(s.port);
    t.web = true;
    auto r = c.invoke(t, "/watt.test.Echo/Say", R"({"message":"x"})", o);
    INFO(r.status_message);
    CHECK_FALSE(r.ok);
    CHECK(r.status_message.find("second gRPC-Web trailer") != std::string::npos);
  }
}

TEST_CASE("gRPC-Web unary: message, metadata both ways, trailers (binary and base64 text)", "[grpc][web]") {
  for (bool text : {false, true}) {
    INFO((text ? "application/grpc-web-text" : "application/grpc-web+proto"));
    GrpcClient c;
    auto t = TestServer::get().web_target(text);
    GrpcCallOptions o;
    o.metadata = {{"X-Foo", "bar"}, {"x-blob-bin", "AAEC"}};
    auto r = c.invoke(t, "/watt.test.Echo/Say", R"({"message":"hi","repeat":3})", o);
    INFO(r.status_message);
    REQUIRE(r.ok);
    CHECK(r.status_name == "OK");
    REQUIRE(r.messages.size() == 1);
    CHECK(r.messages[0].json.find("{\n  \"message\": \"hihihi\"") == 0);  // same pretty JSON as native
    auto d = parse(r.messages[0].json);
    CHECK(d["message"] == "hihihi");
    CHECK(d["metadata"]["x-foo"] == "bar");
    CHECK_FALSE(d["metadata"].HasMember("x-grpc-web"));  // the proxy strips the transport headers
    CHECK(header(r.headers, "x-server") == "watt-testserver");
    CHECK(header(r.headers, "echo-x-foo") == "bar");
    CHECK(header(r.headers, "echo-x-blob-bin") == "AAEC");  // binary metadata round-trips as base64
    CHECK(header(r.headers, "content-type").empty());       // HTTP headers are not metadata
    CHECK(header(r.trailers, "x-trailer") == "done");
    CHECK(header(r.trailers, "grpc-status").empty());
    CHECK(r.messages[0].wire_bytes > 5);
    CHECK(r.timing.bytes_sent > r.messages[0].wire_bytes);  // HTTP request line + headers + body
    CHECK(r.timing.bytes_received > r.messages[0].wire_bytes);
    CHECK(r.messages[0].at_ms <= r.timing.total_ms);
  }
}

TEST_CASE("gRPC-Web reflection lists services and auto-reflects a single service", "[grpc][web][reflection]") {
  GrpcClient c;
  auto t = TestServer::get().web_target();
  check_schema(c.load_via_reflection(t));
  // Auto-reflection on first use; JSON conversion as native.
  GrpcClient fresh;
  auto r = fresh.invoke(t, "/watt.test.Everything/Echo", R"({"fString":"s","fInt64":"7","level":"LEVEL_HIGH"})", {});
  INFO(r.status_message);
  REQUIRE(r.ok);
  auto d = parse(r.messages[0].json);
  CHECK(d["fString"] == "s");
  CHECK(d["fInt64"] == "7");
  CHECK(d["level"] == "LEVEL_HIGH");
  // .proto files instead of reflection: no reflection traffic at all.
  GrpcClient files;
  REQUIRE(files.load_proto_files({(proto_dir() / "test.proto").string()}, {}).ok);
  auto f = files.invoke(t, "/watt.test.Echo/Say", R"({"message":"p"})", {});
  REQUIRE(f.ok);
  CHECK(f.reflect_ms == 0);
  CHECK_FALSE(f.timing.connection_reused);
  CHECK(f.timing.connect_ms > 0);
}

TEST_CASE("gRPC-Web errors: status, message, details from the trailer frame and trailers-only", "[grpc][web][errors]") {
  GrpcClient c;
  auto t = TestServer::get().web_target();
  for (bool trailers_only : {false, true}) {
    INFO((trailers_only ? "trailers-only" : "trailer frame"));
    GrpcCallOptions o;
    if (trailers_only) o.rpc_metadata = {{"x-grpcweb-trailers-only", "1"}};
    auto r = c.invoke(t, "/watt.test.Echo/Fail", R"({"code":5,"message":"no such thing: 100%","withDetails":true})", o);
    CHECK_FALSE(r.ok);
    CHECK(r.status_code == 5);
    CHECK(r.status_name == "NOT_FOUND");
    CHECK(r.status_message == "no such thing: 100%");  // percent-encoded on the wire
    CHECK(r.messages.empty());
    CHECK(header(r.trailers, "x-trailer") == "failed");
    if (trailers_only) CHECK(r.headers.empty());
    INFO(r.error_details);
    auto d = parse(r.error_details);
    REQUIRE(d.IsObject());
    CHECK(d["code"] == 5);
    REQUIRE(d["details"].Size() == 2);
    CHECK(d["details"][0]["@type"] == "type.googleapis.com/watt.test.ErrorDetail");
    CHECK(d["details"][0]["reason"] == "BAD_THING");
    CHECK(d["details"][1]["@type"] == "type.googleapis.com/google.rpc.ErrorInfo");
    CHECK(d["details"][1]["metadata"]["limit"] == "10");

    auto plain = c.invoke(t, "/watt.test.Echo/Fail", R"({"code":7})", o);
    CHECK(plain.status_name == "PERMISSION_DENIED");
    CHECK(plain.status_message == "requested failure");
    CHECK(plain.error_details.empty());
  }
  // A response without any grpc-status maps its HTTP status as gRPC does.
  GrpcCallOptions o;
  o.rpc_metadata = {{"x-grpcweb-http-status", "404"}};
  auto nf = c.invoke(t, "/watt.test.Echo/Say", "{}", o);
  CHECK(nf.status_name == "UNIMPLEMENTED");
  CHECK(nf.status_message.find("HTTP 404") != std::string::npos);
  o.rpc_metadata = {{"x-grpcweb-http-status", "503"}};
  CHECK(c.invoke(t, "/watt.test.Echo/Say", "{}", o).status_name == "UNAVAILABLE");
  // Client-side errors are the same as native.
  CHECK(c.invoke(t, "/watt.test.Echo/Say", R"({"mesage":"typo"})", {}).status_name == "INVALID_ARGUMENT");
  CHECK(c.invoke(t, "/watt.test.Echo/Nope", "{}", {}).status_name == "NOT_FOUND");
  GrpcCallOptions bad;
  bad.metadata = {{"bad key", "v"}};
  CHECK(c.invoke(t, "/watt.test.Echo/Say", "{}", bad).status_name == "INVALID_ARGUMENT");
}

TEST_CASE("gRPC-Web server streaming: each message is timed as its frame arrives", "[grpc][web][stream]") {
  for (bool text : {false, true}) {
    INFO((text ? "application/grpc-web-text" : "application/grpc-web+proto"));
    GrpcClient c;
    auto t = TestServer::get().web_target(text);
    std::vector<double> seen_at;
    auto r = c.invoke(t, "/watt.test.Echo/Count", R"({"count":5,"intervalMs":30})", {},
                      [&](const GrpcMessageEvent& e) { seen_at.push_back(e.at_ms); });
    INFO(r.status_message);
    REQUIRE(r.ok);
    REQUIRE(r.messages.size() == 5);
    REQUIRE(seen_at.size() == 5);
    for (int i = 0; i < 5; ++i) CHECK(parse(r.messages[i].json)["index"] == i);
    for (int i = 1; i < 5; ++i) CHECK(r.messages[i].at_ms >= r.messages[i - 1].at_ms + 10);  // not all at the end
    CHECK(r.messages[4].at_ms - r.messages[0].at_ms >= 100);
    CHECK(header(r.headers, "x-server") == "watt-testserver");
    CHECK(header(r.trailers, "x-count") == "5");
    CHECK(r.timing.download_ms >= 100);
  }
}

TEST_CASE("gRPC-Web deadline exceeded", "[grpc][web][deadline]") {
  GrpcClient c;
  auto t = TestServer::get().web_target();
  REQUIRE(c.load_via_reflection(t).ok);
  GrpcCallOptions o;
  o.deadline_ms = 150;
  auto r = c.invoke(t, "/watt.test.Echo/Slow", R"({"delayMs":3000})", o);
  CHECK_FALSE(r.ok);
  CHECK(r.status_name == "DEADLINE_EXCEEDED");
  CHECK(r.timing.total_ms >= 140);
  CHECK(r.timing.total_ms < 1500);
  o.deadline_ms = 3000;
  auto ok = c.invoke(t, "/watt.test.Echo/Slow", R"({"delayMs":50})", o);
  REQUIRE(ok.ok);
  CHECK(ok.timing.ttfb_ms >= 45);
}

TEST_CASE("gRPC-Web refuses client and bidi streaming without sending anything", "[grpc][web][stream]") {
  GrpcClient c;
  auto t = TestServer::get().web_target();
  REQUIRE(c.load_via_reflection(t).ok);
  for (const char* path : {"/watt.test.Echo/Sum", "/watt.test.Echo/Chat"}) {
    INFO(path);
    auto r = c.invoke(t, path, R"([{"value":1}])", {});
    CHECK(r.status_name == "UNIMPLEMENTED");
    CHECK(r.status_message.find("not possible over gRPC-Web") != std::string::npos);
    CHECK(r.status_message.find(path) != std::string::npos);
    CHECK(r.timing.bytes_sent == 0);
    auto call = c.start_call(t, path, {});
    CHECK(call->done());
    CHECK(call->wait().status_name == "UNIMPLEMENTED");
    CHECK_FALSE(call->send("{}"));
  }
  // Targets the transport cannot serve are refused, not silently downgraded.
  auto mtls = t;
  mtls.client_cert_pem = "x";
  mtls.client_key_pem = "y";
  auto m = c.invoke(mtls, "/watt.test.Echo/Say", "{}", {});
  CHECK(m.status_name == "UNIMPLEMENTED");
  CHECK(m.status_message.find("mTLS") != std::string::npos);
}

TEST_CASE("gRPC-Web timing: phases filled like HTTP and adding up", "[grpc][web][timing]") {
  GrpcClient c;
  auto t = TestServer::get().web_target();
  auto r = c.invoke(t, "/watt.test.Echo/Say", R"({"message":"t"})", {});
  INFO(r.status_message);
  REQUIRE(r.ok);
  CHECK_FALSE(r.timing.connection_reused);  // auto-reflection opened the connection for this call
  CHECK(r.timing.connect_ms > 0);
  CHECK(r.reflect_ms > 0);
  CHECK(r.timing.tls_ms == 0);  // plaintext
  CHECK(r.timing.ttfb_ms > 0);
  CHECK(phase_sum(r) <= r.timing.total_ms + 0.01);
  CHECK(phase_sum(r) > r.timing.total_ms * 0.9);

  auto warm = c.invoke(t, "/watt.test.Echo/Say", R"({"message":"t"})", {});
  REQUIRE(warm.ok);
  CHECK(warm.timing.connection_reused);  // the reflection's keep-alive connection
  CHECK(warm.timing.connect_ms == 0);
  CHECK(warm.timing.dns_ms == 0);
  CHECK(warm.reflect_ms == 0);
  CHECK(warm.timing.ttfb_ms > 0);
  CHECK(phase_sum(warm) <= warm.timing.total_ms + 0.01);
  CHECK(phase_sum(warm) > warm.timing.total_ms * 0.9);

  // A host name is resolved and timed as dns_ms.
  GrpcClient named;
  auto host = t;
  host.address = "localhost:" + host.address.substr(host.address.rfind(':') + 1);
  auto n = named.invoke(host, "/watt.test.Echo/Say", "{}", {});
  INFO(n.status_message);
  REQUIRE(n.ok);
  CHECK(n.timing.dns_ms > 0);
  CHECK(phase_sum(n) <= n.timing.total_ms + 0.01);
}

TEST_CASE("gRPC-Web cancel, interactive calls and unreachable targets", "[grpc][web][cancel]") {
  GrpcClient c;
  auto t = TestServer::get().web_target();
  REQUIRE(c.load_via_reflection(t).ok);
  SECTION("cancel from another thread") {
    std::thread th([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      c.cancel();
    });
    auto r = c.invoke(t, "/watt.test.Echo/Slow", R"({"delayMs":5000})", {});
    th.join();
    CHECK(r.status_name == "CANCELLED");
    CHECK(r.timing.total_ms < 2000);
  }
  SECTION("cancel from the message callback") {
    auto r = c.invoke(t, "/watt.test.Echo/Count", R"({"count":1000,"intervalMs":10})", {},
                      [&](const GrpcMessageEvent&) { c.cancel(); });
    CHECK(r.status_name == "CANCELLED");
    CHECK(r.messages.size() < 10);
  }
  SECTION("interactive server stream and unary") {
    auto call = c.start_call(t, "/watt.test.Echo/Count", {});
    CHECK_FALSE(call->done());
    REQUIRE(call->send(R"({"count":3})"));
    std::string err;
    CHECK_FALSE(call->send(R"({"count":3})", &err));  // one request message
    auto r = call->wait();
    INFO(r.status_message);
    REQUIRE(r.ok);
    CHECK(r.messages.size() == 3);

    auto slow = c.start_call(t, "/watt.test.Echo/Slow", {});
    REQUIRE(slow->send(R"({"delayMs":5000})"));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    slow->cancel();
    auto s = slow->wait();
    CHECK(s.status_name == "CANCELLED");
    CHECK(s.timing.total_ms < 2000);

    auto unsent = c.start_call(t, "/watt.test.Echo/Say", {});
    unsent->cancel();
    CHECK(unsent->wait().status_name == "CANCELLED");
  }
  SECTION("unreachable") {
    GrpcClient u;
    GrpcTarget dead;
    dead.address = "127.0.0.1:1";
    dead.web = true;
    auto r = u.invoke(dead, "/watt.test.Echo/Say", "{}", {});
    CHECK(r.status_name == "UNAVAILABLE");
    CHECK(r.reflect_ms == 0);
    CHECK(r.timing.total_ms < 5000);
    auto s = u.load_via_reflection(dead);
    CHECK_FALSE(s.ok);
    CHECK(s.error.find("UNAVAILABLE") != std::string::npos);
  }
}

TEST_CASE("gRPC-Web over TLS: verified with a custom CA, untrusted, skip-verify", "[grpc][web][tls]") {
  auto& s = TestServer::get(tls_args(false));
  auto t = s.web_target();
  t.tls = true;
  t.root_cert_pem = slurp(cert_dir() / "ca.pem");
  GrpcClient c;
  auto r = c.invoke(t, "/watt.test.Echo/Say", R"({"message":"tls"})", {});
  INFO(r.status_message);
  REQUIRE(r.ok);
  CHECK(parse(r.messages[0].json)["message"] == "tls");
  CHECK(r.timing.tls_ms > 0);
  CHECK(r.timing.connect_ms > 0);
  CHECK(phase_sum(r) <= r.timing.total_ms + 0.01);
  auto warm = c.invoke(t, "/watt.test.Echo/Say", "{}", {});
  REQUIRE(warm.ok);
  CHECK(warm.timing.connection_reused);
  CHECK(warm.timing.tls_ms == 0);

  GrpcClient c2;
  auto untrusted = t;
  untrusted.root_cert_pem.clear();  // the system store does not know the test CA
  auto bad = c2.invoke(untrusted, "/watt.test.Echo/Say", "{}", {});
  CHECK(bad.status_name == "UNAVAILABLE");
  auto skip = untrusted;
  skip.insecure_skip_verify = true;
  auto ok = c2.invoke(skip, "/watt.test.Echo/Say", "{}", {});
  INFO(ok.status_message);
  CHECK(ok.ok);
}
