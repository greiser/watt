// watt_testserver: local gRPC server with reflection, used by tests, benchmarks and manual QA.
//   watt_testserver [port] [--tls <dir>] [--mtls] [--reflection v1alpha|none] [--web <port>]
//     port 0 picks a free port (default 50051).
//     --web <port>  the gRPC-Web (HTTP/1.1) listener's port (see grpcweb.cpp); default: port + 1
//                   (50052), or a free port when port is 0. TLS with --tls (no client certs).
//     --tls <dir>   serve TLS with <dir>/server.pem + server.key (tools/testserver/certs: a test CA
//                   ca.pem, a server cert for localhost / 127.0.0.1, and a client cert)
//     --mtls        also require a client certificate signed by <dir>/ca.pem
//     --reflection  v1alpha: serve only the v1alpha reflection service (like older servers);
//                   none: no reflection at all. Default: v1 and v1alpha.
// Prints "watt_testserver listening on 127.0.0.1:<port>" on stdout once ready, then
// "watt_testserver grpc-web listening on 127.0.0.1:<port>" (or "watt_testserver grpc-web
// disabled: <why>" when that port cannot be opened; the gRPC port works either way).
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/duration.pb.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/wire_format_lite.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/generic/callback_generic_service.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/server_interceptor.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <memory>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4267)  // protoc-generated map size accessors narrow size_t -> int
#endif
#include "extra.pb.h"
#include "pack.grpc.pb.h"
#include "pack.pb.h"
#include "test.grpc.pb.h"
#include "test.pb.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace {

// The x-probe metadata value of the most recent reflection call, so tests can verify that clients
// send metadata on reflection (servers often put auth in front of it). Echo.Say returns it as the
// "x-last-reflection-probe" trailer.
std::mutex g_probe_mu;
std::string g_reflection_probe;

class ReflectionProbe final : public grpc::experimental::Interceptor {
 public:
  explicit ReflectionProbe(bool reflection) : reflection_(reflection) {}
  void Intercept(grpc::experimental::InterceptorBatchMethods* m) override {
    using HP = grpc::experimental::InterceptionHookPoints;
    if (reflection_ && m->QueryInterceptionHookPoint(HP::POST_RECV_INITIAL_METADATA)) {
      std::string v = "(none)";
      if (auto* md = m->GetRecvInitialMetadata()) {
        auto it = md->find("x-probe");
        if (it != md->end()) v.assign(it->second.data(), it->second.size());
      }
      std::lock_guard lk(g_probe_mu);
      g_reflection_probe = v;
    }
    m->Proceed();
  }

 private:
  bool reflection_;
};

class ReflectionProbeFactory final : public grpc::experimental::ServerInterceptorFactoryInterface {
 public:
  grpc::experimental::Interceptor* CreateServerInterceptor(grpc::experimental::ServerRpcInfo* info) override {
    return new ReflectionProbe(std::string(info->method()).find("ServerReflection") != std::string::npos);
  }
};

using grpc::ServerContext;
using grpc::Status;
namespace t = watt::test;

google::protobuf::Timestamp now_ts() {
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
  google::protobuf::Timestamp ts;
  ts.set_seconds(ns / 1000000000);
  ts.set_nanos(static_cast<int>(ns % 1000000000));
  return ts;
}

// x-server, and x-authority: the :authority the call arrived with.
void add_server_headers(ServerContext* ctx) {
  ctx->AddInitialMetadata("x-server", "watt-testserver");
  const grpc::string_ref authority = ctx->ExperimentalGetAuthority();
  ctx->AddInitialMetadata("x-authority", std::string(authority.data(), authority.size()));
}

// Copies client metadata into the response, echoes x-* keys as "echo-<key>" initial metadata.
void echo_metadata(ServerContext* ctx, t::EchoResponse* resp) {
  add_server_headers(ctx);
  for (const auto& [k, v] : ctx->client_metadata()) {
    std::string key(k.data(), k.size()), val(v.data(), v.size());
    (*resp->mutable_metadata())[key] = val;
    if (key.rfind("x-", 0) == 0) ctx->AddInitialMetadata("echo-" + key, val);
  }
  ctx->AddTrailingMetadata("x-trailer", "done");
}

// Sleeps in small slices so cancellation and deadlines are honoured promptly.
bool sleep_cancellable(ServerContext* ctx, int ms) {
  auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < until) {
    if (ctx->IsCancelled()) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return !ctx->IsCancelled();
}

class EchoService final : public t::Echo::Service {
  Status Say(ServerContext* ctx, const t::EchoRequest* req, t::EchoResponse* resp) override {
    std::string msg;
    int n = req->repeat() > 0 ? req->repeat() : 1;
    for (int i = 0; i < n; ++i) msg += req->message();
    resp->set_message(msg);
    *resp->mutable_received_at() = now_ts();
    echo_metadata(ctx, resp);
    {
      std::lock_guard lk(g_probe_mu);
      ctx->AddTrailingMetadata("x-last-reflection-probe", g_reflection_probe.empty() ? "(never)" : g_reflection_probe);
    }
    return Status::OK;
  }

  Status Count(ServerContext* ctx, const t::CountRequest* req,
               grpc::ServerWriter<t::CountResponse>* w) override {
    add_server_headers(ctx);
    for (int i = 0; i < req->count(); ++i) {
      if (i > 0 && req->interval_ms() > 0 && !sleep_cancellable(ctx, req->interval_ms()))
        return Status::CANCELLED;
      t::CountResponse r;
      r.set_index(i);
      if (!w->Write(r)) return Status::CANCELLED;
    }
    ctx->AddTrailingMetadata("x-count", std::to_string(req->count()));
    return Status::OK;
  }

  Status Sum(ServerContext* ctx, grpc::ServerReader<t::SumRequest>* r, t::SumResponse* resp) override {
    t::SumRequest m;
    int64_t total = 0;
    int count = 0;
    while (r->Read(&m)) {
      total += m.value();
      ++count;
    }
    resp->set_total(total);
    resp->set_count(count);
    ctx->AddTrailingMetadata("x-count", std::to_string(count));
    return Status::OK;
  }

  Status Chat(ServerContext* ctx, grpc::ServerReaderWriter<t::ChatMessage, t::ChatMessage>* s) override {
    add_server_headers(ctx);
    t::ChatMessage in;
    int seq = 0;
    while (s->Read(&in)) {
      t::ChatMessage out;
      out.set_text("echo: " + in.text());
      out.set_seq(seq++);
      if (!s->Write(out)) break;
    }
    ctx->AddTrailingMetadata("x-count", std::to_string(seq));
    return Status::OK;
  }

  Status Fail(ServerContext* ctx, const t::FailRequest* req, t::EchoResponse*) override {
    auto code = static_cast<grpc::StatusCode>(req->code());
    if (code == grpc::StatusCode::OK || req->code() < 0 || req->code() > 16) code = grpc::StatusCode::INTERNAL;
    std::string msg = req->message().empty() ? "requested failure" : req->message();
    ctx->AddTrailingMetadata("x-trailer", "failed");
    if (!req->with_details()) return Status(code, msg);

    t::RpcStatusMirror st;
    st.set_code(static_cast<int>(code));
    st.set_message(msg);
    t::ErrorDetail d;
    d.set_reason("BAD_THING");
    d.set_field("message");
    d.set_retry_after_ms(250);
    st.add_details()->PackFrom(d);
    t::ErrorInfoMirror info;
    info.set_reason("QUOTA");
    info.set_domain("watt.dev");
    (*info.mutable_metadata())["limit"] = "10";
    auto* any = st.add_details();
    any->set_type_url("type.googleapis.com/google.rpc.ErrorInfo");
    any->set_value(info.SerializeAsString());
    return Status(code, msg, st.SerializeAsString());
  }

  Status Slow(ServerContext* ctx, const t::SlowRequest* req, t::EchoResponse* resp) override {
    if (!sleep_cancellable(ctx, req->delay_ms())) return Status(grpc::StatusCode::CANCELLED, "cancelled");
    resp->set_message("slept " + std::to_string(req->delay_ms()) + "ms");
    *resp->mutable_received_at() = now_ts();
    return Status::OK;
  }
};

// A hand-written grpc.reflection.v1alpha server (and nothing else), for clients that must fall
// back from v1. Answers list_services, file_containing_symbol and file_by_filename from the
// generated pool; every other unregistered method is UNIMPLEMENTED.
class V1AlphaReflection final : public grpc::CallbackGenericService {
 public:
  grpc::ServerGenericBidiReactor* CreateReactor(grpc::GenericCallbackServerContext* ctx) override {
    if (ctx->method() != "/grpc.reflection.v1alpha.ServerReflection/ServerReflectionInfo") {
      class Unimplemented : public grpc::ServerGenericBidiReactor {
       public:
        Unimplemented() { Finish(Status(grpc::StatusCode::UNIMPLEMENTED, "")); }
        void OnDone() override { delete this; }
      };
      return new Unimplemented;
    }
    return new Reactor;
  }

 private:
  using WFL = google::protobuf::internal::WireFormatLite;
  static void put_varint(std::string& out, uint64_t v) {
    while (v >= 0x80) {
      out += char((v & 0x7F) | 0x80);
      v >>= 7;
    }
    out += char(v);
  }
  static void put_len(std::string& out, int field, const std::string& s) {
    put_varint(out, (uint64_t(field) << 3) | 2);
    put_varint(out, s.size());
    out += s;
  }
  static std::string respond(const std::string& req) {
    namespace pb = google::protobuf;
    const auto* pool = pb::DescriptorPool::generated_pool();
    pb::io::CodedInputStream in(reinterpret_cast<const uint8_t*>(req.data()), static_cast<int>(req.size()));
    int kind = 0;
    std::string arg;
    while (uint32_t tag = in.ReadTag()) {
      int f = static_cast<int>(WFL::GetTagFieldNumber(tag));
      if (WFL::GetTagWireType(tag) == WFL::WIRETYPE_LENGTH_DELIMITED && (f == 3 || f == 4 || f == 7)) {
        kind = f;
        WFL::ReadString(&in, &arg);
      } else if (!WFL::SkipField(&in, tag)) {
        break;
      }
    }
    std::string out;
    put_len(out, 2, req);  // original_request
    if (kind == 7) {
      std::string list;
      for (const char* name : {"watt.test.Echo", "watt.test.Everything", "watt.test.Packer",
                               "grpc.reflection.v1alpha.ServerReflection"}) {
        std::string svc;
        put_len(svc, 1, name);
        put_len(list, 1, svc);
      }
      put_len(out, 6, list);
      return out;
    }
    const pb::FileDescriptor* fd = nullptr;
    if (kind == 3) fd = pool->FindFileByName(arg);
    if (kind == 4) fd = pool->FindFileContainingSymbol(arg);
    if (!fd) {
      std::string e;
      put_varint(e, 1 << 3);
      put_varint(e, 5);  // NOT_FOUND
      put_len(e, 2, "not found: " + arg);
      put_len(out, 7, e);
      return out;
    }
    // The file and its transitive dependencies, dependencies first.
    std::string files;
    std::set<const pb::FileDescriptor*> seen;
    std::function<void(const pb::FileDescriptor*)> add = [&](const pb::FileDescriptor* f) {
      if (!seen.insert(f).second) return;
      for (int i = 0; i < f->dependency_count(); ++i) add(f->dependency(i));
      pb::FileDescriptorProto fdp;
      f->CopyTo(&fdp);
      put_len(files, 1, fdp.SerializeAsString());
    };
    add(fd);
    put_len(out, 4, files);
    return out;
  }

  class Reactor : public grpc::ServerGenericBidiReactor {
   public:
    Reactor() { StartRead(&in_); }
    void OnReadDone(bool ok) override {
      if (!ok) {
        Finish(Status::OK);
        return;
      }
      std::vector<grpc::Slice> slices;
      std::string req;
      if (in_.Dump(&slices).ok())
        for (auto& sl : slices) req.append(reinterpret_cast<const char*>(sl.begin()), sl.size());
      std::string resp = respond(req);
      grpc::Slice slice(resp);
      out_ = grpc::ByteBuffer(&slice, 1);
      StartWrite(&out_);
    }
    void OnWriteDone(bool ok) override {
      if (!ok) {
        Finish(Status::OK);
        return;
      }
      StartRead(&in_);
    }
    void OnDone() override { delete this; }

   private:
    grpc::ByteBuffer in_, out_;
  };
};

// Drops the reflection plugin, which grpc++_reflection registers for every server at static init.
class NoReflectionPlugin final : public grpc::ServerBuilderOption {
 public:
  void UpdateArguments(grpc::ChannelArguments*) override {}
  void UpdatePlugins(std::vector<std::unique_ptr<grpc::ServerBuilderPlugin>>* plugins) override {
    plugins->erase(std::remove_if(plugins->begin(), plugins->end(),
                                  [](const auto& p) { return p->name() == "proto_server_reflection"; }),
                   plugins->end());
  }
};

std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

class EverythingService final : public t::Everything::Service {
  Status Echo(ServerContext*, const t::Kitchen* req, t::Kitchen* resp) override {
    *resp = *req;
    return Status::OK;
  }
};

// Any round trips with types outside pack.proto's closure (see pack.proto for the kinds).
class PackerService final : public t::Packer::Service {
  static void pack_unknown(google::protobuf::Any* a) {
    a->set_type_url("type.googleapis.com/foo.Unknown");
    a->set_value(std::string("", 2));
  }
  Status Pack(ServerContext*, const t::PackRequest* req, t::PackResponse* resp) override {
    google::protobuf::Duration two;
    two.set_seconds(2);
    watt::extra::Note note;
    note.set_text("packed");
    note.set_priority(3);
    const std::string& k = req->kind();
    if (k == "note") resp->mutable_result()->PackFrom(note);
    else if (k == "duration") resp->mutable_result()->PackFrom(two);
    else if (k == "unknown") pack_unknown(resp->mutable_result());
    else if (k == "nested" || k == "nested_unknown") {
      if (k == "nested") note.mutable_attachment()->PackFrom(two);
      else pack_unknown(note.mutable_attachment());
      resp->mutable_result()->PackFrom(note);
    }
    if (resp->has_result()) (*resp->mutable_by_name())["result"] = resp->result();
    *resp->mutable_items() = req->items();
    return Status::OK;
  }
};

}  // namespace

int start_grpc_web(int port, std::shared_ptr<grpc::Channel> backend, const std::string& tls_dir, std::string& err);

int main(int argc, char** argv) {
  int port = 50051;
  std::string tls_dir, reflection = "both";
  bool mtls = false;
  int web_port = -1;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--tls" && i + 1 < argc) tls_dir = argv[++i];
    else if (a == "--web" && i + 1 < argc) web_port = std::atoi(argv[++i]);
    else if (a == "--mtls") mtls = true;
    else if (a == "--reflection" && i + 1 < argc) reflection = argv[++i];
    else port = std::atoi(a.c_str());
  }
  std::string addr = "127.0.0.1:" + std::to_string(port);

  std::shared_ptr<grpc::ServerCredentials> creds = grpc::InsecureServerCredentials();
  if (!tls_dir.empty()) {
    grpc::SslServerCredentialsOptions o(mtls ? GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY
                                             : GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE);
    o.pem_key_cert_pairs.push_back({read_file(tls_dir + "/server.key"), read_file(tls_dir + "/server.pem")});
    if (mtls) o.pem_root_certs = read_file(tls_dir + "/ca.pem");
    if (o.pem_key_cert_pairs[0].private_key.empty() || o.pem_key_cert_pairs[0].cert_chain.empty()) {
      std::fprintf(stderr, "watt_testserver: cannot read %s/server.pem and server.key\n", tls_dir.c_str());
      return 1;
    }
    creds = grpc::SslServerCredentials(o);
  }

  if (reflection == "both") grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  EchoService echo;
  EverythingService everything;
  PackerService packer;
  V1AlphaReflection v1alpha;
  grpc::ServerBuilder builder;
  int selected = 0;
  builder.AddListeningPort(addr, creds, &selected);
  builder.RegisterService(&echo);
  builder.RegisterService(&everything);
  builder.RegisterService(&packer);
  if (reflection == "v1alpha") builder.RegisterCallbackGenericService(&v1alpha);
  if (reflection != "both") builder.SetOption(std::make_unique<NoReflectionPlugin>());
  std::vector<std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>> interceptors;
  interceptors.push_back(std::make_unique<ReflectionProbeFactory>());
  builder.experimental().SetInterceptorCreators(std::move(interceptors));
  auto server = builder.BuildAndStart();
  if (!server || selected == 0) {
    std::fprintf(stderr, "watt_testserver: failed to listen on %s\n", addr.c_str());
    return 1;
  }
  std::printf("watt_testserver listening on 127.0.0.1:%d\n", selected);
  std::fflush(stdout);
  if (web_port < 0) web_port = port == 0 ? 0 : port + 1;
  grpc::ChannelArguments web_args;
  web_args.SetMaxReceiveMessageSize(-1);
  std::string web_err;
  const int web = start_grpc_web(web_port, server->InProcessChannel(web_args), tls_dir, web_err);
  if (web) std::printf("watt_testserver grpc-web listening on 127.0.0.1:%d\n", web);
  else std::printf("watt_testserver grpc-web disabled: %s\n", web_err.c_str());
  std::fflush(stdout);
  server->Wait();
  return 0;
}
