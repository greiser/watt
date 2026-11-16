#pragma once
// gRPC engine. Dynamic invocation without generated code: method schemas come from server
// reflection (v1 with v1alpha fallback) or from .proto files parsed at runtime
// (google::protobuf::compiler::Importer). Messages are edited as JSON and converted with
// protobuf's JsonStringToMessage / MessageToJsonString. Calls go through grpc::GenericStub
// so all four call types work. Channels are cached per target so re-sends skip the handshake.
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/timing.h"

namespace watt {

struct GrpcTarget {
  std::string address;          // host:port
  bool tls = false;
  bool insecure_skip_verify = false;
  std::string root_cert_pem;    // optional custom CA
  std::string authority;        // optional :authority override
  bool operator==(const GrpcTarget&) const = default;
};

enum class GrpcCallKind { Unary, ServerStreaming, ClientStreaming, Bidi };

struct GrpcMethodInfo {
  std::string service;          // fully-qualified, e.g. "watt.test.Echo"
  std::string method;           // short name, e.g. "Say"
  std::string full_path;        // "/watt.test.Echo/Say"
  std::string input_type;       // fully-qualified message names
  std::string output_type;
  GrpcCallKind kind = GrpcCallKind::Unary;
  std::string request_template; // JSON skeleton with every field filled with a default value
};

struct GrpcSchema {
  bool ok = false;
  std::string error;
  std::vector<GrpcMethodInfo> methods;
  double load_ms = 0;
};

using Metadata = std::vector<std::pair<std::string, std::string>>;

struct GrpcMessageEvent {
  std::string json;             // pretty JSON of the received message
  double at_ms = 0;             // offset from call start
  size_t wire_bytes = 0;
};

struct GrpcResponse {
  bool ok = false;              // status OK
  int status_code = 0;          // grpc::StatusCode
  std::string status_name;      // "OK", "UNAVAILABLE", ...
  std::string status_message;
  std::string error_details;    // binary details as hex/text if present
  Metadata headers;             // initial metadata
  Metadata trailers;
  std::vector<GrpcMessageEvent> messages;
  Timing timing;                // connect/tls/ttfb/total; dns folded into connect
};

struct GrpcCallOptions {
  long deadline_ms = 30000;     // 0 = none
  Metadata metadata;
};

// Streaming callbacks fire on the engine thread.
using GrpcMessageFn = std::function<void(const GrpcMessageEvent&)>;

class GrpcClient {
 public:
  GrpcClient();
  ~GrpcClient();
  GrpcClient(const GrpcClient&) = delete;
  GrpcClient& operator=(const GrpcClient&) = delete;

  GrpcSchema load_via_reflection(const GrpcTarget& target, long deadline_ms = 5000);
  GrpcSchema load_proto_files(const std::vector<std::string>& proto_paths,
                              const std::vector<std::string>& import_dirs);

  // request_json for client/bidi streaming: a JSON array of messages, sent in order.
  GrpcResponse invoke(const GrpcTarget& target, const std::string& full_path,
                      const std::string& request_json, const GrpcCallOptions& opts,
                      const GrpcMessageFn& on_message = {});
  void cancel();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace watt
