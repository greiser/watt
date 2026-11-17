#pragma once
// gRPC engine. Dynamic invocation without generated code: method schemas come from server
// reflection (v1 with v1alpha fallback), from .proto files parsed at runtime
// (google::protobuf::compiler::Importer) or from compiled FileDescriptorSets (.protoset).
// Messages are edited as JSON and converted with protobuf's JsonStringToMessage /
// MessageToJsonString. Calls go through grpc::GenericStub so all four call types work.
// Channels are cached per target so re-sends skip the handshake.
//
// Cold-start: constructing a GrpcClient starts a background warm-up (gRPC runtime, EventEngine,
// channel stack, protobuf descriptor + JSON machinery), so construct it early. preconnect() starts
// the DNS lookup and connection for a target as soon as the target is known.
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/timing.h"

namespace watt {

struct GrpcTarget {
  std::string address;          // host:port (dns names, IPv4, [IPv6]; unix:/ipv4:/ipv6: URIs pass through)
  bool tls = false;
  bool insecure_skip_verify = false;
  std::string root_cert_pem;    // optional custom CA
  std::string authority;        // optional :authority override (TLS: also the server name, unless server_name is set)
  std::string server_name;      // optional TLS server name (SNI + certificate check), independent of :authority
  std::string client_cert_pem;  // optional mTLS client certificate chain (needs client_key_pem)
  std::string client_key_pem;   // optional mTLS client private key
  // Channel options (each distinct combination gets its own cached channel).
  long connect_timeout_ms = 0;  // give up connecting after this long; 0 = 20 s (or the call deadline)
  long keepalive_ms = 0;        // HTTP/2 keepalive ping interval; 0 = off
  int max_message_bytes = 0;    // largest message sent or received; 0 = unlimited
  std::string user_agent;       // prepended to gRPC's own user-agent
  // gRPC-Web (as BloomRPC / Postman offer it): calls go over HTTP/1.1 or HTTP/2 to a gRPC-Web
  // endpoint (Envoy's grpc_web filter, grpcwebproxy, a Connect/ASP.NET/tonic-web server) instead
  // of a gRPC channel: POST <http|https>://<address>/<service>/<method>. address may carry a
  // scheme and a path prefix ("https://api.example.com/rpc"); tls or an https:// scheme selects
  // https. Unary and server streaming work (messages are timed as their frames arrive, timing is
  // filled as for HTTP); client and bidi streaming cannot be expressed in gRPC-Web and fail with
  // UNIMPLEMENTED. Reflection is sent as one request body on the reflection stream, which
  // proxies that forward the body whole (Envoy, grpcwebproxy) serve. insecure_skip_verify,
  // root_cert_pem, authority (as Host), user_agent and max_message_bytes apply; client
  // certificates, server_name, connect_timeout_ms and keepalive_ms do not (client certificates
  // and server_name are refused with UNIMPLEMENTED rather than ignored).
  bool web = false;
  bool web_text = false;        // with web: application/grpc-web-text (base64 bodies) instead of +proto
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
  // Human-readable hints about the template that JSON cannot carry, one per line-item, e.g.
  // "choice: oneof, set one of text | number | nested (template uses text)" or
  // "meta: google.protobuf.Struct, any JSON object".
  std::vector<std::string> template_notes;
};

struct GrpcSchema {
  bool ok = false;
  std::string error;
  std::vector<GrpcMethodInfo> methods;
  double load_ms = 0;
};

using Metadata = std::vector<std::pair<std::string, std::string>>;

struct GrpcMessageEvent {
  std::string json;             // pretty JSON of the received message (empty when decode_error is set)
  double at_ms = 0;             // offset from call start
  size_t wire_bytes = 0;
  // What a lenient second decode pass had to change for a message the strict parser rejected, one
  // line per field, e.g. "metadata[x-data-bin]: invalid UTF-8 replaced with U+FFFD" (grpcurl shows
  // such strings the same way). json is then the real message with those repairs.
  // Also one line per google.protobuf.Any whose type neither the schema, the linked-in types nor
  // (reflected schemas) the server's reflection knows; json shows it as grpcurl does,
  // {"@error": "... is not recognized ...", "@type": url, "@value": base64 bytes}, and the
  // call keeps the server's status.
  std::vector<std::string> notes;
  // Set when the message could not be decoded at all: the parser's error, with raw_hex holding the
  // message bytes. json stays empty (never a made-up body) and the call reports INTERNAL.
  std::string decode_error;
  std::string raw_hex;
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
  // dns_ms: our own name lookup for a new channel (0 for IP literals). connect_ms: channel setup
  // + TCP connect that this call waited for (plaintext: also the HTTP/2 settings exchange).
  // tls_ms (TLS targets): TCP connected -> READY, i.e. the TLS handshake + HTTP/2 settings.
  // connection_reused: the channel was already READY (an earlier call or preconnect()).
  // For an auto-reflected first call, connect ends when the channel is READY; the reflection
  // request was already in flight by then, and reflect_ms runs from READY to the schema.
  Timing timing;
  double reflect_ms = 0;        // auto-reflection before the call (also inside timing.total_ms)
  // What the channel dials after watt's own name resolution, as a gRPC target URI
  // ("ipv4:127.0.0.1:50051", "ipv4:a:443,b:443" for several addresses, "dns:///host:443" when
  // gRPC resolves itself). Empty when no channel was leased (gRPC-Web, local failures).
  std::string peer;
};

// Message format for requests and responses. Text is protobuf text format (grpcurl -format text):
// GrpcMessageEvent::json then carries text, and several request messages for a client stream are
// separated by the ASCII record separator 0x1E, as grpcurl does.
enum class GrpcFormat { Json, Text };

struct GrpcCallOptions {
  long deadline_ms = 30000;     // 0 = none
  Metadata metadata;            // sent on the RPC and on its auto-reflection request
  Metadata reflect_metadata;    // sent only on the auto-reflection request (grpcurl -reflect-header)
  Metadata rpc_metadata;        // sent only on the RPC itself (grpcurl -rpc-header)
  bool proto_field_names = false;  // response JSON keys as in the .proto (received_at) not lowerCamel
  bool allow_unknown_fields = false;  // request fields the schema does not know are dropped, not an error
  bool emit_defaults = true;    // response JSON prints fields without presence at their defaults (grpcurl
                                // -emit-defaults); false prints only what is set, as grpcurl does by default
  bool compact = false;         // responses on one line (JSON without whitespace, text format single-line)
  GrpcFormat format = GrpcFormat::Json;
};

// Streaming callbacks fire on the engine thread.
using GrpcMessageFn = std::function<void(const GrpcMessageEvent&)>;

class GrpcClient;

// An interactive call: messages are sent one at a time while responses arrive, and the caller
// decides when to half-close. All methods are thread-safe. Must not outlive its GrpcClient's
// last reference to the call (the call keeps the engine alive on its own).
class GrpcCall {
 public:
  ~GrpcCall();  // cancels the call if it is still running and waits for it
  GrpcCall(const GrpcCall&) = delete;
  GrpcCall& operator=(const GrpcCall&) = delete;

  // Encodes one JSON object and queues it for sending. False (with *error) when the JSON does not
  // match the request type, the send side is closed, or the call has ended.
  bool send(const std::string& json, std::string* error = nullptr);
  void close_send();            // half-close: no more messages
  void cancel();
  bool done() const;            // the call has ended (status available)
  GrpcResponse wait();          // blocks until the call ends; the full response with timing

  struct Core;
 private:
  friend class GrpcClient;
  GrpcCall() = default;
  std::shared_ptr<Core> core_;
};

class GrpcClient {
 public:
  GrpcClient();
  ~GrpcClient();
  GrpcClient(const GrpcClient&) = delete;
  GrpcClient& operator=(const GrpcClient&) = delete;

  // Starts name resolution + connection for target in the background and returns immediately.
  void preconnect(const GrpcTarget& target);

  GrpcSchema load_via_reflection(const GrpcTarget& target, long deadline_ms = 5000,
                                 const Metadata& metadata = {});
  // .proto sources; paths ending in .protoset/.pb/.desc/.binpb are loaded as FileDescriptorSets.
  GrpcSchema load_proto_files(const std::vector<std::string>& proto_paths,
                              const std::vector<std::string>& import_dirs);
  GrpcSchema load_protoset(const std::vector<std::string>& protoset_paths);

  // request_json for client/bidi streaming: a JSON array of messages, sent in order.
  GrpcResponse invoke(const GrpcTarget& target, const std::string& full_path,
                      const std::string& request_json, const GrpcCallOptions& opts,
                      const GrpcMessageFn& on_message = {});

  // Interactive call (any kind). Resolves the method (auto-reflecting if needed) and connects
  // before returning; on failure the returned call is already done() with the error status.
  std::unique_ptr<GrpcCall> start_call(const GrpcTarget& target, const std::string& full_path,
                                       const GrpcCallOptions& opts, const GrpcMessageFn& on_message = {});
  void cancel();                // cancels every in-flight call of this client

  // The method's request filled with example values instead of zero values ("Hello", 10, a UUID
  // for id fields, the first real enum value, ...), like BloomRPC / Postman, so it means something
  // unedited; notes as GrpcMethodInfo::template_notes. Empty when no loaded schema has the method.
  std::string example_request(const std::string& full_path, std::vector<std::string>* notes = nullptr) const;
  // The definition of a loaded service, method ("pkg.Svc.Method" or "/pkg.Svc/Method"), message
  // or enum as .proto source, with comments when the schema has them (.proto files do; reflection
  // usually does not). Empty when no loaded schema knows the symbol.
  std::string describe(const std::string& symbol) const;
  // Writes the files that define `symbols` (all loaded files when empty), dependencies first, as a
  // FileDescriptorSet (grpcurl -protoset-out). False with *error when a symbol is unknown or the
  // file cannot be written.
  bool export_protoset(const std::string& path, const std::vector<std::string>& symbols,
                       std::string* error = nullptr) const;
  // Writes the same files as .proto sources under dir (grpcurl -proto-out-dir), one per schema file
  // at its import path (e.g. dir/google/protobuf/timestamp.proto), creating directories as needed.
  bool export_proto_files(const std::string& dir, const std::vector<std::string>& symbols,
                          std::string* error = nullptr) const;

  struct Impl;
 private:
  std::shared_ptr<Impl> impl_;
};

}  // namespace watt
