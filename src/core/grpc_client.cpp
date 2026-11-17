#include "core/grpc_client.h"
#include "core/grpc_wire_repair.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <mstcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// gRPC engine. Everything runs through grpc::GenericStub's async bidi call on a private
// CompletionQueue: unary and server-streaming calls are just streams with one request
// (sent with WriteLast so request + half-close go out together). Using the CQ directly lets
// one loop keep a read and a write in flight at once, timestamp every event precisely, and
// cancel at any point.
//
// Cold start is engineered: each client warms the runtime on worker threads (see Impl()), host
// names are resolved here (gRPC's resolver is slow on a cold process; IP literals skip the lookup),
// invoke() reflects only the one service it needs, and that reflection request is sent with
// wait_for_ready as soon as the channel exists, so it rides on the connection handshake.
//
// Reflection is spoken on the wire by hand (the reflection messages are tiny and vcpkg does
// not ship their generated headers), v1 first and v1alpha when v1 is UNIMPLEMENTED.

#include <grpc/event_engine/event_engine.h>
#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/proto_utils.h>
#include <grpcpp/support/proto_buffer_reader.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/security/tls_certificate_provider.h>
#include <grpcpp/security/tls_certificate_verifier.h>
#include <grpcpp/security/tls_credentials_options.h>

#include <openssl/ssl.h>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/descriptor_database.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/text_format.h>
#include <google/protobuf/util/json_util.h>
#include <google/protobuf/wire_format_lite.h>

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string_view>
#include <thread>

#include "core/grpc_template.h"

namespace watt {
namespace {

namespace pb = google::protobuf;
using WFL = pb::internal::WireFormatLite;

constexpr size_t kMaxSchemas = 32;
constexpr long kMaxConnectWaitMs = 20000;  // default connect limit when the target sets none
long connect_limit_ms(const GrpcTarget& t) { return t.connect_timeout_ms > 0 ? t.connect_timeout_ms : kMaxConnectWaitMs; }

const char* status_name(int c) {
  static const char* names[] = {"OK",
                                "CANCELLED",
                                "UNKNOWN",
                                "INVALID_ARGUMENT",
                                "DEADLINE_EXCEEDED",
                                "NOT_FOUND",
                                "ALREADY_EXISTS",
                                "PERMISSION_DENIED",
                                "RESOURCE_EXHAUSTED",
                                "FAILED_PRECONDITION",
                                "ABORTED",
                                "OUT_OF_RANGE",
                                "UNIMPLEMENTED",
                                "INTERNAL",
                                "UNAVAILABLE",
                                "DATA_LOSS",
                                "UNAUTHENTICATED"};
  return (c >= 0 && c <= 16) ? names[c] : "UNKNOWN";
}

// ------------------------------------------------------------------ small encoders

std::string b64_encode(std::string_view in) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
    out += t[v >> 18];
    out += t[(v >> 12) & 63];
    out += t[(v >> 6) & 63];
    out += t[v & 63];
  }
  if (i < in.size()) {
    uint32_t v = uint8_t(in[i]) << 16;
    if (i + 1 < in.size()) v |= uint8_t(in[i + 1]) << 8;
    out += t[v >> 18];
    out += t[(v >> 12) & 63];
    out += (i + 1 < in.size()) ? t[(v >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

bool b64_decode(std::string_view in, std::string& out) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
  };
  while (!in.empty() && in.back() == '=') in.remove_suffix(1);
  if (in.empty() || in.size() % 4 == 1) return false;
  out.clear();
  uint32_t acc = 0;
  int bits = 0;
  for (char c : in) {
    int v = val(c);
    if (v < 0) return false;
    acc = (acc << 6) | uint32_t(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += char((acc >> bits) & 0xFF);
    }
  }
  return true;
}

std::string hex(std::string_view in) {
  static const char* d = "0123456789abcdef";
  std::string out;
  out.reserve(in.size() * 2);
  for (unsigned char c : in) {
    out += d[c >> 4];
    out += d[c & 15];
  }
  return out;
}

void put_varint(std::string& out, uint64_t v) {
  while (v >= 0x80) {
    out += char((v & 0x7F) | 0x80);
    v >>= 7;
  }
  out += char(v);
}

void put_len_field(std::string& out, int field, std::string_view s) {
  put_varint(out, (uint64_t(field) << 3) | 2);
  put_varint(out, s.size());
  out.append(s.data(), s.size());
}

std::string to_string(const grpc::string_ref& s) { return std::string(s.data(), s.size()); }

std::string byte_buffer_to_string(const grpc::ByteBuffer& bb) {
  std::vector<grpc::Slice> slices;
  std::string out;
  if (!bb.Dump(&slices).ok()) return out;
  for (const auto& s : slices) out.append(reinterpret_cast<const char*>(s.begin()), s.size());
  return out;
}

grpc::ByteBuffer string_to_byte_buffer(const std::string& s) {
  grpc::Slice slice(s);
  return grpc::ByteBuffer(&slice, 1);
}

// ------------------------------------------------------------------ source tree / errors

// Disk source tree with two extras: in-memory files (used for embedded google.rpc protos) and
// a fallback that serves files from the descriptors linked into the process (google/protobuf/*.proto
// and any other generated file), so well-known types always resolve even when no include dir ships
// them, and an Any of a linked-in type can be imported on demand (resolve_any).
class WattSourceTree : public pb::compiler::SourceTree {
 public:
  pb::compiler::DiskSourceTree disk;
  std::map<std::string, std::string, std::less<>> mem;

  pb::io::ZeroCopyInputStream* Open(absl::string_view filename) override {
    std::string name(filename);
    if (auto it = mem.find(name); it != mem.end())
      return new pb::io::ArrayInputStream(it->second.data(), static_cast<int>(it->second.size()));
    if (auto* s = disk.Open(filename)) return s;
    if (const auto* fd = pb::DescriptorPool::generated_pool()->FindFileByName(name)) {
      keep_.push_back(fd->DebugString());
      const std::string& text = keep_.back();
      return new pb::io::ArrayInputStream(text.data(), static_cast<int>(text.size()));
    }
    return nullptr;
  }
  std::string GetLastErrorMessage() override { return disk.GetLastErrorMessage(); }

 private:
  std::deque<std::string> keep_;
};

class ImportErrors : public pb::compiler::MultiFileErrorCollector {
 public:
  std::string text;
  void RecordError(absl::string_view filename, int line, int column, absl::string_view message) override {
    if (!text.empty()) text += '\n';
    text += std::string(filename);
    if (line >= 0) text += ":" + std::to_string(line + 1) + ":" + std::to_string(column + 1);
    text += ": ";
    text += std::string(message);
  }
  void RecordWarning(absl::string_view, int, int, absl::string_view) override {}
};

class PoolErrors : public pb::DescriptorPool::ErrorCollector {
 public:
  std::string text;
  void RecordError(absl::string_view filename, absl::string_view element_name, const pb::Message*,
                   ErrorLocation, absl::string_view message) override {
    if (!text.empty()) text += '\n';
    text += std::string(filename) + ": " + std::string(element_name) + ": " + std::string(message);
  }
};

// The fallback database behind a reflection / protoset schema's pool: the files the source
// provided, files added later (types an Any names, fetched by reflection on demand), and every
// file linked into the process (the well-known types), served on demand. Lookups of a symbol the
// pool has not built yet, e.g. the printer resolving Any's @type google.protobuf.Duration, land
// here and pull in the generated file, so WKT Anys work in every schema without loading them all.
class SetDatabase : public pb::DescriptorDatabase {
 public:
  explicit SetDatabase(std::map<std::string, pb::FileDescriptorProto>&& files) {
    for (auto& [name, fdp] : files) files_.emplace(name, std::move(fdp));
  }
  bool add(const pb::FileDescriptorProto& fdp) {
    std::lock_guard lk(mu_);
    return files_.emplace(fdp.name(), fdp).second;
  }
  bool FindFileByName(StringViewArg filename, pb::FileDescriptorProto* out) override {
    {
      std::lock_guard lk(mu_);
      if (auto it = files_.find(std::string(filename)); it != files_.end()) {
        *out = it->second;
        return true;
      }
    }
    const auto* g = pb::DescriptorPool::generated_pool()->FindFileByName(filename);
    if (!g) return false;
    g->CopyTo(out);
    return true;
  }
  // Every provided file is built up front, so a symbol the pool misses can only be linked in. Only
  // message types are served (what an Any names): a service the client happens to link must not
  // pass for one the server has, which would skip auto-reflection.
  bool FindFileContainingSymbol(StringViewArg symbol, pb::FileDescriptorProto* out) override {
    const auto* g = pb::DescriptorPool::generated_pool()->FindMessageTypeByName(symbol);
    if (!g) return false;
    g->file()->CopyTo(out);
    return true;
  }
  bool FindFileContainingExtension(StringViewArg, int, pb::FileDescriptorProto*) override { return false; }

 private:
  std::mutex mu_;
  std::map<std::string, pb::FileDescriptorProto, std::less<>> files_;
};

// ------------------------------------------------------------------ schema sets

// One loaded schema (from reflection or from .proto files). Immutable once published, shared
// by in-flight calls via shared_ptr. Member order matters: factory dies before the pool.
struct SchemaSet {
  std::unique_ptr<WattSourceTree> tree;
  std::unique_ptr<ImportErrors> import_errors;
  std::unique_ptr<pb::compiler::Importer> importer;
  std::unique_ptr<SetDatabase> db;          // own_pool's fallback (reflection / protoset sets)
  std::unique_ptr<PoolErrors> pool_errors;  // own_pool's error collector
  std::unique_ptr<pb::DescriptorPool> own_pool;
  const pb::DescriptorPool* pool = nullptr;
  std::unique_ptr<pb::DynamicMessageFactory> factory;
  bool from_reflection = false;
  GrpcTarget target;
  std::string partial_of;  // reflection of just this service (from invoke), "" = full listing
  std::vector<std::string> roots;  // files this set was loaded from (deps reachable from them)

  // Any @type names resolve_any could not find (not asked again). Guarded by any_mu, which also
  // serializes adding files to the pool for Any types.
  std::mutex any_mu;
  std::set<std::string, std::less<>> any_missing;

  void init_factory() { factory = std::make_unique<pb::DynamicMessageFactory>(pool); }
  const pb::Message* prototype(const pb::Descriptor* d) const { return factory->GetPrototype(d); }
};

// google.rpc.Status + error_details, embedded so rich errors decode without googleapis.
constexpr const char* kRpcStatusProto = R"(syntax = "proto3";
package google.rpc;
import "google/protobuf/any.proto";
message Status { int32 code = 1; string message = 2; repeated google.protobuf.Any details = 3; }
)";
constexpr const char* kRpcErrorDetailsProto = R"(syntax = "proto3";
package google.rpc;
import "google/protobuf/duration.proto";
message ErrorInfo { string reason = 1; string domain = 2; map<string, string> metadata = 3; }
message RetryInfo { google.protobuf.Duration retry_delay = 1; }
message DebugInfo { repeated string stack_entries = 1; string detail = 2; }
message QuotaFailure {
  message Violation { string subject = 1; string description = 2; }
  repeated Violation violations = 1;
}
message PreconditionFailure {
  message Violation { string type = 1; string subject = 2; string description = 3; }
  repeated Violation violations = 1;
}
message BadRequest {
  message FieldViolation {
    string field = 1; string description = 2; string reason = 3; LocalizedMessage localized_message = 4;
  }
  repeated FieldViolation field_violations = 1;
}
message RequestInfo { string request_id = 1; string serving_data = 2; }
message ResourceInfo { string resource_type = 1; string resource_name = 2; string owner = 3; string description = 4; }
message Help {
  message Link { string description = 1; string url = 2; }
  repeated Link links = 1;
}
message LocalizedMessage { string locale = 1; string message = 2; }
)";

SchemaSet& rpc_types() {
  static SchemaSet* s = [] {
    auto* set = new SchemaSet;
    set->tree = std::make_unique<WattSourceTree>();
    set->tree->mem["google/rpc/status.proto"] = kRpcStatusProto;
    set->tree->mem["google/rpc/error_details.proto"] = kRpcErrorDetailsProto;
    set->import_errors = std::make_unique<ImportErrors>();
    set->importer = std::make_unique<pb::compiler::Importer>(set->tree.get(), set->import_errors.get());
    set->importer->Import("google/rpc/status.proto");
    set->importer->Import("google/rpc/error_details.proto");
    set->pool = set->importer->pool();
    set->init_factory();
    return set;
  }();
  return *s;
}

const pb::MethodDescriptor* find_method(const pb::DescriptorPool* pool, const std::string& service,
                                        const std::string& method) {
  const auto* svc = pool->FindServiceByName(service);
  return svc ? svc->FindMethodByName(method) : nullptr;
}

GrpcCallKind kind_of(const pb::MethodDescriptor* m) {
  if (m->client_streaming() && m->server_streaming()) return GrpcCallKind::Bidi;
  if (m->client_streaming()) return GrpcCallKind::ClientStreaming;
  if (m->server_streaming()) return GrpcCallKind::ServerStreaming;
  return GrpcCallKind::Unary;
}

void append_methods(const pb::ServiceDescriptor* s, std::vector<GrpcMethodInfo>& out) {
  for (int i = 0; i < s->method_count(); ++i) {
    const auto* m = s->method(i);
    GrpcMethodInfo info;
    info.service = s->full_name();
    info.method = m->name();
    info.full_path = "/" + info.service + "/" + info.method;
    info.input_type = m->input_type()->full_name();
    info.output_type = m->output_type()->full_name();
    info.kind = kind_of(m);
    info.request_template = grpc_detail::build_request_template(m->input_type(), &info.template_notes);
    out.push_back(std::move(info));
  }
}

// "/pkg.Svc/Method", "pkg.Svc/Method" or "pkg.Svc.Method" -> (service, method)
bool split_path(const std::string& path, std::string& service, std::string& method) {
  std::string p = path;
  while (!p.empty() && p.front() == '/') p.erase(0, 1);
  size_t cut = p.rfind('/');
  if (cut == std::string::npos) cut = p.rfind('.');
  if (cut == std::string::npos || cut == 0 || cut + 1 >= p.size()) return false;
  service = p.substr(0, cut);
  method = p.substr(cut + 1);
  return true;
}

std::string normalize_address(std::string a) {
  for (const char* scheme : {"http://", "https://", "grpc://", "grpcs://", "dns:///"}) {
    size_t n = std::char_traits<char>::length(scheme);
    if (a.size() > n && a.compare(0, n, scheme) == 0) {
      a.erase(0, n);
      break;
    }
  }
  while (!a.empty() && (a.back() == '/' || a.back() == ' ')) a.pop_back();
  while (!a.empty() && a.front() == ' ') a.erase(0, 1);
  return a;
}

// ------------------------------------------------------------------ JSON helpers

pb::util::JsonPrintOptions print_options(bool pretty) {
  pb::util::JsonPrintOptions o;
  o.add_whitespace = pretty;
  o.always_print_fields_with_no_presence = true;
  return o;
}

// protobuf pretty-prints with 1-space indent; widen to 2 like every other JSON view in watt.
// Raw newlines only occur as layout (string contents are escaped), so one linear pass is safe.
std::string widen_indent(const std::string& js) {
  std::string out;
  out.reserve(js.size() + js.size() / 4);
  size_t i = 0;
  const size_t n = js.size();
  while (i < n) {
    const size_t nl = js.find('\n', i);
    if (nl == std::string::npos) {
      out.append(js, i, std::string::npos);
      break;
    }
    out.append(js, i, nl + 1 - i);
    size_t j = nl + 1;
    while (j < n && js[j] == ' ') ++j;
    out.append((j - nl - 1) * 2, ' ');
    i = j;
  }
  return out;
}

// ------------------------------------------------------------------ huge string fields

// protobuf's JSON printer escapes strings one code point at a time (~90 MB/s), which dominated
// large responses (12 MB: 135 ms of a 160 ms decode). Strings and bytes of at least this size are
// therefore taken out of the message before printing, replaced by a token, and spliced back into
// the printed JSON with append_json_escaped / base64, which copy plain runs in bulk.
constexpr size_t kBigFieldBytes = 64 * 1024;

// JSON-escapes s exactly as protobuf's JsonWriter::WriteEscapedUtf8 does (json/internal/writer.cc,
// protobuf 33): the JSON escapes, \u00XX for controls, '<' '>' and a set of invisible / format
// code points as \uXXXX (surrogate pairs above U+FFFF), and a space for each undecodable sequence.
void append_json_escaped(std::string& out, std::string_view s) {
  // 1: printable ASCII that is copied as is.
  static const auto kPlain = [] {
    std::array<uint8_t, 256> t{};
    for (int c = 0x20; c < 0x7f; ++c) t[c] = 1;
    t['"'] = t['\\'] = t['<'] = t['>'] = 0;
    return t;
  }();
  auto u_escape = [&](uint32_t v) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(v));
    out.append(buf, 6);
  };
  auto must_escape = [](uint32_t c) {
    switch (c) {
      case '<': case '>': case 0xfeff: case 0xfff9: case 0xfffa: case 0xfffb: case 0x00ad: case 0x06dd:
      case 0x070f: case 0x17b4: case 0x17b5: case 0x000e0001:
        return true;
      default: break;
    }
    static constexpr std::pair<uint32_t, uint32_t> kRanges[] = {
        {0x0000, 0x001f}, {0x007f, 0x009f}, {0x0600, 0x0603}, {0x200b, 0x200f}, {0x2028, 0x202e},
        {0x2060, 0x2064}, {0x206a, 0x206f}, {0x0001d173, 0x0001d17a}, {0x000e0020, 0x000e007f}};
    for (const auto& [lo, hi] : kRanges)
      if (lo <= c && c <= hi) return true;
    return false;
  };
  constexpr uint32_t kError = 0xaaaaaaaa;
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    size_t run = i;
    while (run < n && kPlain[static_cast<uint8_t>(s[run])]) ++run;
    if (run > i) {
      out.append(s.data() + i, run - i);
      i = run;
      if (i >= n) break;
    }
    // One scalar, decoded like protobuf's ConsumeUtf8Scalar (no overlong / surrogate checks).
    const size_t start = i;
    uint32_t c = static_cast<uint8_t>(s[i++]);
    int more = 0;
    if (c >= 0x80) {
      if ((c & 0xe0) == 0xc0) more = 1, c &= 0x1f;
      else if ((c & 0xf0) == 0xe0) more = 2, c &= 0x0f;
      else if ((c & 0xf8) == 0xf0) more = 3, c &= 0x07;
      else c = kError;
    }
    for (int k = 0; k < more; ++k) {
      if (i >= n) {
        c = kError;
        break;
      }
      const uint8_t next = static_cast<uint8_t>(s[i++]);
      if ((next >> 6) != 2) {
        c = kError;
        break;
      }
      c = (c << 6) | (next & 0x3f);
    }
    if (c != kError && c > 0x10ffff) c = kError;
    switch (c) {
      case '\n': out += "\\n"; continue;
      case '\r': out += "\\r"; continue;
      case '\t': out += "\\t"; continue;
      case '"': out += "\\\""; continue;
      case '\f': out += "\\f"; continue;
      case '\b': out += "\\b"; continue;
      case '\\': out += "\\\\"; continue;
      case kError: out += ' '; continue;
      default: break;
    }
    if (!must_escape(c)) {
      out.append(s.data() + start, i - start);
    } else if (c < 0x10000) {
      u_escape(c);
    } else {
      u_escape((c >> 10) + (0xd800 - (0x10000 >> 10)));
      u_escape((c & 0x3ff) + 0xdc00);
    }
  }
}

// String / bytes fields of at least kBigFieldBytes, taken out of a message for printing.
class BigFields {
 public:
  bool empty() const { return slots_.empty(); }

  // Takes the big fields out of m (not descending into maps or well-known types, whose JSON forms
  // are special) and leaves a token in each.
  void extract(pb::Message& m, int depth = 0) {
    if (depth > 100) return;
    const auto* r = m.GetReflection();
    std::vector<const pb::FieldDescriptor*> fields;
    r->ListFields(m, &fields);
    for (const auto* f : fields) {
      if (f->cpp_type() == pb::FieldDescriptor::CPPTYPE_STRING) {
        if (f->is_repeated()) {
          for (int i = 0, n = r->FieldSize(m, f); i < n; ++i) take(m, f, i);
        } else {
          take(m, f, -1);
        }
      } else if (f->cpp_type() == pb::FieldDescriptor::CPPTYPE_MESSAGE && !f->is_map() &&
                 f->message_type()->file()->package() != "google.protobuf") {
        if (f->is_repeated()) {
          for (int i = 0, n = r->FieldSize(m, f); i < n; ++i) extract(*r->MutableRepeatedMessage(&m, f, i), depth + 1);
        } else {
          extract(*r->MutableMessage(&m, f), depth + 1);
        }
      }
    }
  }

  // Puts every value back (the message is printed some other way).
  void restore() {
    for (auto& s : slots_) {
      const auto* r = s.m->GetReflection();
      if (s.index < 0) r->SetString(s.m, s.f, std::move(s.value));
      else r->SetRepeatedString(s.m, s.f, s.index, std::move(s.value));
    }
    slots_.clear();
  }

  // The printed JSON with each token replaced by its value as a JSON string (bytes: base64).
  std::string splice(const std::string& js) const {
    const std::string open = "\"\\u0001" + prefix() + ":";
    size_t total = js.size();
    for (const auto& s : slots_) total += s.value.size() + s.value.size() / 3 + 8;
    std::string out;
    out.reserve(total);
    size_t i = 0;
    for (;;) {
      const size_t at = js.find(open, i);
      if (at == std::string::npos) break;
      const size_t num = at + open.size();
      size_t end = num;
      while (end < js.size() && js[end] >= '0' && js[end] <= '9') ++end;
      static constexpr std::string_view kClose = "\\u0002\"";
      const size_t idx = end > num ? std::strtoul(js.c_str() + num, nullptr, 10) : slots_.size();
      if (idx >= slots_.size() || js.compare(end, kClose.size(), kClose) != 0) {  // not ours after all
        out.append(js, i, num - i);
        i = num;
        continue;
      }
      out.append(js, i, at - i);
      const auto& s = slots_[idx];
      out += '"';
      if (s.f->type() == pb::FieldDescriptor::TYPE_BYTES) out += b64_encode(s.value);
      else append_json_escaped(out, s.value);
      out += '"';
      i = end + kClose.size();
    }
    out.append(js, i, std::string::npos);
    return out;
  }

 private:
  struct Slot {
    pb::Message* m;
    const pb::FieldDescriptor* f;
    int index;  // -1: singular
    std::string value;
  };
  // Unique per process, so no real string can pass for a token.
  static const std::string& prefix() {
    static const std::string p = [] {
      char buf[48];
      const auto t = static_cast<unsigned long long>(Clock::now().time_since_epoch().count());
      std::snprintf(buf, sizeof(buf), "watt-big-%llx-%llx", t,
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&buf)));
      return std::string(buf);
    }();
    return p;
  }
  void take(pb::Message& m, const pb::FieldDescriptor* f, int index) {
    const auto* r = m.GetReflection();
    std::string scratch;
    const std::string& ref =
        index < 0 ? r->GetStringReference(m, f, &scratch) : r->GetRepeatedStringReference(m, f, index, &scratch);
    if (ref.size() < kBigFieldBytes) return;
    std::string token = "\x01" + prefix() + ":" + std::to_string(slots_.size()) + "\x02";
    Slot s{&m, f, index, {}};
    if (&ref != &scratch) {
      // The message's own storage: swap the value out instead of copying megabytes.
      auto& own = const_cast<std::string&>(ref);
      s.value.swap(own);
      own = std::move(token);
    } else {
      s.value = std::move(scratch);
      if (index < 0) r->SetString(&m, f, std::move(token));
      else r->SetRepeatedString(&m, f, index, std::move(token));
    }
    slots_.push_back(std::move(s));
  }
  std::vector<Slot> slots_;
};

std::string message_to_json(const pb::Message& m, bool pretty) {
  std::string out;
  auto st = pb::util::MessageToJsonString(m, &out, print_options(pretty));
  if (!st.ok()) return "{\"error\": \"" + std::string(st.message()) + "\"}";
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
  return pretty ? widen_indent(out) : out;
}

constexpr const char* kSpace = " \t\r\n";

// Text format: messages separated by the record separator 0x1E (grpcurl's convention).
bool split_text_request(const std::string& body, bool streaming, std::vector<std::string>& out, std::string& err) {
  size_t from = 0;
  for (;;) {
    size_t cut = body.find(char(0x1E), from);
    std::string part = body.substr(from, cut == std::string::npos ? std::string::npos : cut - from);
    if (part.find_first_not_of(kSpace) != std::string::npos || (out.empty() && cut == std::string::npos))
      out.push_back(std::move(part));
    if (cut == std::string::npos) break;
    from = cut + 1;
  }
  if (out.empty()) out.emplace_back();
  if (!streaming && out.size() != 1) {
    err = "this method takes a single request message";
    return false;
  }
  return true;
}

// Splits the request body into one JSON document per message. Objects are one message; arrays
// are a message each (only allowed for client-streaming/bidi, or a single-element array).
bool split_request(const std::string& body, bool streaming, std::vector<std::string>& out, std::string& err) {
  size_t i = body.find_first_not_of(" \t\r\n");
  if (i == std::string::npos) {
    out.push_back("{}");
    return true;
  }
  if (body[i] != '[') {
    out.push_back(body);
    return true;
  }
  rapidjson::Document doc;
  doc.Parse<rapidjson::kParseFullPrecisionFlag>(body.c_str(), body.size());
  if (doc.HasParseError() || !doc.IsArray()) {
    err = "request body is not valid JSON (offset " + std::to_string(doc.GetErrorOffset()) + ")";
    return false;
  }
  if (!streaming && doc.Size() != 1) {
    err = "this method takes a single request message: send a JSON object, not an array";
    return false;
  }
  for (const auto& v : doc.GetArray()) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    v.Accept(w);
    out.emplace_back(sb.GetString(), sb.GetSize());
  }
  return true;
}

// Fetches the files defining a symbol from the server (reflection file_containing_symbol);
// false when the server does not know it or cannot be asked.
using AnyFetch = std::function<bool(const std::string& symbol, std::map<std::string, pb::FileDescriptorProto>& files)>;
const pb::Descriptor* resolve_any(SchemaSet& set, std::string_view type_url, const AnyFetch* fetch);
std::string_view any_type_name(std::string_view type_url) {
  const size_t slash = type_url.rfind('/');
  return slash == std::string_view::npos ? type_url : type_url.substr(slash + 1);
}
absl::Status print_json_resolving_any(SchemaSet& set, const pb::Message& m, const pb::util::JsonPrintOptions& opts,
                                      const AnyFetch* fetch, std::string& out, std::vector<std::string>* notes,
                                      bool* final_form);
std::string unknown_any_error(std::string_view type_url);

// Decodes grpc-status-details-bin (a google.rpc.Status) to pretty JSON, resolving each Any
// against the call's schema, then the embedded google.rpc types, then the linked-in types and the
// server's reflection (resolve_any). A detail nothing knows is printed as grpcurl does
// ({"@error", "@type", "@value"}).
std::string decode_status_details(const std::string& bin, SchemaSet* schema, const AnyFetch* fetch) {
  if (bin.empty()) return {};
  pb::io::CodedInputStream in(reinterpret_cast<const uint8_t*>(bin.data()), static_cast<int>(bin.size()));
  int64_t code = 0;
  std::string message;
  std::vector<std::pair<std::string, std::string>> details;  // type_url, value
  bool good = true;
  while (uint32_t tag = in.ReadTag()) {
    int field = static_cast<int>(WFL::GetTagFieldNumber(tag));
    auto wt = WFL::GetTagWireType(tag);
    if (field == 1 && wt == WFL::WIRETYPE_VARINT) {
      uint64_t v;
      good = in.ReadVarint64(&v);
      code = static_cast<int64_t>(v);
    } else if (field == 2 && wt == WFL::WIRETYPE_LENGTH_DELIMITED) {
      good = WFL::ReadString(&in, &message);
    } else if (field == 3 && wt == WFL::WIRETYPE_LENGTH_DELIMITED) {
      std::string any;
      good = WFL::ReadBytes(&in, &any);
      if (!good) break;
      pb::io::CodedInputStream ain(reinterpret_cast<const uint8_t*>(any.data()), static_cast<int>(any.size()));
      std::string url, value;
      while (uint32_t t2 = ain.ReadTag()) {
        int f2 = static_cast<int>(WFL::GetTagFieldNumber(t2));
        if (f2 == 1 && WFL::GetTagWireType(t2) == WFL::WIRETYPE_LENGTH_DELIMITED) good = WFL::ReadString(&ain, &url);
        else if (f2 == 2 && WFL::GetTagWireType(t2) == WFL::WIRETYPE_LENGTH_DELIMITED) good = WFL::ReadBytes(&ain, &value);
        else good = WFL::SkipField(&ain, t2);
        if (!good) break;
      }
      details.emplace_back(std::move(url), std::move(value));
    } else {
      good = WFL::SkipField(&in, tag);
    }
    if (!good) break;
  }
  if (!good || !in.ConsumedEntireMessage()) return "hex:" + hex(bin);

  rapidjson::Document doc(rapidjson::kObjectType);
  auto& a = doc.GetAllocator();
  doc.AddMember("code", rapidjson::Value(code), a);
  doc.AddMember("message", rapidjson::Value(message.c_str(), static_cast<rapidjson::SizeType>(message.size()), a), a);
  rapidjson::Value arr(rapidjson::kArrayType);
  auto str = [&](const std::string& x) {
    return rapidjson::Value(x.c_str(), static_cast<rapidjson::SizeType>(x.size()), a);
  };
  SchemaSet& rpc = rpc_types();
  for (const auto& [url, value] : details) {
    const std::string_view type = any_type_name(url);
    SchemaSet* owner = nullptr;
    const pb::Descriptor* d = nullptr;
    if (schema && (d = schema->pool->FindMessageTypeByName(type))) owner = schema;
    else if ((d = rpc.pool->FindMessageTypeByName(type))) owner = &rpc;
    else if (schema && (d = resolve_any(*schema, url, fetch))) owner = schema;
    else if ((d = resolve_any(rpc, url, nullptr))) owner = &rpc;
    rapidjson::Value obj(rapidjson::kObjectType);
    bool decoded = false;
    if (d) {
      std::unique_ptr<pb::Message> m(owner->prototype(d)->New());
      std::string js;
      bool final_form = false;
      rapidjson::Document sub;
      if (m->ParseFromString(value) &&
          print_json_resolving_any(*owner, *m, print_options(false), owner == schema ? fetch : nullptr, js, nullptr,
                                   &final_form)
              .ok() &&
          !sub.Parse(js.c_str(), js.size()).HasParseError()) {
        obj.AddMember("@type", str(url), a);
        if (sub.IsObject()) {
          for (auto mem = sub.MemberBegin(); mem != sub.MemberEnd(); ++mem)
            obj.AddMember(rapidjson::Value(mem->name, a), rapidjson::Value(mem->value, a), a);
        } else {
          obj.AddMember("value", rapidjson::Value(sub, a), a);  // a well-known type's JSON form, e.g. "2s"
        }
        decoded = true;
      }
    }
    if (!decoded) {
      obj.AddMember("@error", str(unknown_any_error(url)), a);
      obj.AddMember("@type", str(url), a);
      obj.AddMember("@value", str(b64_encode(value)), a);
    }
    arr.PushBack(obj, a);
  }
  doc.AddMember("details", arr, a);
  rapidjson::StringBuffer sb;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> w(sb);
  w.SetIndent(' ', 2);
  doc.Accept(w);
  return std::string(sb.GetString(), sb.GetSize());
}

Metadata convert_metadata(const std::multimap<grpc::string_ref, grpc::string_ref>& md) {
  Metadata out;
  out.reserve(md.size());
  for (const auto& [k, v] : md) {
    std::string key = to_string(k);
    bool bin = key.size() > 4 && key.compare(key.size() - 4, 4, "-bin") == 0;
    out.emplace_back(std::move(key), bin ? b64_encode(std::string_view(v.data(), v.size())) : to_string(v));
  }
  return out;
}

// Lowercases keys and validates them (gRPC aborts calls with malformed metadata). Values of
// "-bin" keys may be given as base64 and are decoded to raw bytes.
bool apply_metadata(grpc::ClientContext& ctx, const Metadata& md, std::string& err) {
  for (const auto& [k0, v] : md) {
    std::string k = k0;
    while (!k.empty() && k.back() == ' ') k.pop_back();
    while (!k.empty() && k.front() == ' ') k.erase(0, 1);
    if (k.empty()) continue;
    std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (char c : k) {
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) {
        err = "invalid metadata key '" + k0 + "' (allowed: a-z 0-9 - _ .)";
        return false;
      }
    }
    if (k.size() > 4 && k.compare(k.size() - 4, 4, "-bin") == 0) {
      std::string raw;
      ctx.AddMetadata(k, b64_decode(v, raw) ? raw : v);
      continue;
    }
    for (unsigned char c : v) {
      if (c < 0x20 || c > 0x7E) {
        err = "metadata value for '" + k + "' must be printable ASCII (use a -bin key for binary)";
        return false;
      }
    }
    ctx.AddMetadata(k, v);
  }
  return true;
}

// ------------------------------------------------------------------ reflection wire format

struct ReflResponse {
  std::vector<std::string> files;     // serialized FileDescriptorProto
  std::vector<std::string> services;  // list_services names
  bool is_error = false;
  int error_code = 0;
  std::string error_message;
};

bool read_len(pb::io::CodedInputStream& in, std::string& out) { return WFL::ReadBytes(&in, &out); }

bool parse_refl_response(const std::string& bytes, ReflResponse& r) {
  pb::io::CodedInputStream in(reinterpret_cast<const uint8_t*>(bytes.data()), static_cast<int>(bytes.size()));
  while (uint32_t tag = in.ReadTag()) {
    int field = static_cast<int>(WFL::GetTagFieldNumber(tag));
    bool ld = WFL::GetTagWireType(tag) == WFL::WIRETYPE_LENGTH_DELIMITED;
    if (ld && (field == 4 || field == 6 || field == 7)) {
      std::string sub;
      if (!read_len(in, sub)) return false;
      pb::io::CodedInputStream s(reinterpret_cast<const uint8_t*>(sub.data()), static_cast<int>(sub.size()));
      while (uint32_t t2 = s.ReadTag()) {
        int f2 = static_cast<int>(WFL::GetTagFieldNumber(t2));
        bool ld2 = WFL::GetTagWireType(t2) == WFL::WIRETYPE_LENGTH_DELIMITED;
        if (field == 4 && f2 == 1 && ld2) {  // FileDescriptorResponse.file_descriptor_proto
          std::string f;
          if (!read_len(s, f)) return false;
          r.files.push_back(std::move(f));
        } else if (field == 6 && f2 == 1 && ld2) {  // ListServiceResponse.service -> ServiceResponse.name
          std::string svc;
          if (!read_len(s, svc)) return false;
          pb::io::CodedInputStream s3(reinterpret_cast<const uint8_t*>(svc.data()), static_cast<int>(svc.size()));
          while (uint32_t t3 = s3.ReadTag()) {
            if (WFL::GetTagFieldNumber(t3) == 1 && WFL::GetTagWireType(t3) == WFL::WIRETYPE_LENGTH_DELIMITED) {
              std::string name;
              if (!read_len(s3, name)) return false;
              r.services.push_back(std::move(name));
            } else if (!WFL::SkipField(&s3, t3)) {
              return false;
            }
          }
        } else if (field == 7 && f2 == 1 && WFL::GetTagWireType(t2) == WFL::WIRETYPE_VARINT) {
          uint32_t v;
          if (!s.ReadVarint32(&v)) return false;
          r.is_error = true;
          r.error_code = static_cast<int>(v);
        } else if (field == 7 && f2 == 2 && ld2) {
          r.is_error = true;
          if (!read_len(s, r.error_message)) return false;
        } else if (!WFL::SkipField(&s, t2)) {
          return false;
        }
      }
      if (field == 7) r.is_error = true;
    } else if (!WFL::SkipField(&in, tag)) {
      return false;
    }
  }
  return true;
}

// ------------------------------------------------------------------ target resolution

// We resolve host names ourselves and hand gRPC a literal ipv4:/ipv6: URI. gRPC's own DNS
// resolver costs ~7ms on the first channel of a process (even for an IP literal), which was most
// of the cold-start gap to grpcurl; doing it here also lets the lookup be timed as dns_ms.
struct ResolvedTarget {
  std::string uri;        // what grpc::CreateCustomChannel gets
  std::string authority;  // host:port as written, when the uri was rewritten
  std::string host;       // host name for TLS name checks, when the uri was rewritten
  double dns_ms = 0;
};

void ensure_winsock() {
#ifdef _WIN32
  static const bool ok = [] {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
  }();
  (void)ok;
#endif
}

bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

// Dotted-quad IPv4 literal, each part 0-255 without leading zeros (which some parsers read as octal).
bool is_ipv4_literal(std::string_view h) {
  int parts = 0;
  size_t i = 0;
  while (i <= h.size()) {
    size_t j = i;
    int v = 0;
    while (j < h.size() && h[j] >= '0' && h[j] <= '9' && j - i < 3) v = v * 10 + (h[j++] - '0');
    if (j == i || v > 255 || (j - i > 1 && h[i] == '0')) return false;
    ++parts;
    if (j == h.size()) break;
    if (h[j] != '.') return false;
    i = j + 1;
  }
  return parts == 4;
}

ResolvedTarget resolve_target(const std::string& address) {
  ResolvedTarget r;
  std::string a = normalize_address(address);
  r.uri = a;
  for (const char* scheme : {"unix:", "unix-abstract:", "ipv4:", "ipv6:", "vsock:", "xds:", "dns:", "google-c2p:"})
    if (starts_with(a, scheme)) return r;
  std::string host, port;
  if (!a.empty() && a.front() == '[') {
    size_t close = a.find(']');
    if (close == std::string::npos || close + 1 >= a.size() || a[close + 1] != ':') return r;
    host = a.substr(1, close - 1);
    port = a.substr(close + 2);
  } else {
    size_t c = a.rfind(':');
    if (c == std::string::npos || a.find(':') != c) return r;  // no port, or a bare IPv6 literal
    host = a.substr(0, c);
    port = a.substr(c + 1);
  }
  if (host.empty() || port.empty() || port.size() > 5 ||
      !std::all_of(port.begin(), port.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
    return r;
  if (is_ipv4_literal(host)) {
    // No lookup at all: a cold process's first getaddrinfo costs ~0.5 ms even for a literal.
    r.uri = "ipv4:" + host + ":" + port;
    r.authority = a;
    r.host = host;
    return r;
  }

  ensure_winsock();
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICHOST;
  addrinfo* res = nullptr;
  auto t0 = Clock::now();
  int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
  if (rc != 0) {
    hints.ai_flags = 0;
    res = nullptr;
    rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    r.dns_ms = ms_between(t0, Clock::now());
  }
  if (rc != 0 || !res) return r;  // let gRPC's resolver produce the error message

  std::vector<std::string> v4, v6;
  for (addrinfo* p = res; p; p = p->ai_next) {
    char buf[INET6_ADDRSTRLEN] = {};
    if (p->ai_family == AF_INET) {
      inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(p->ai_addr)->sin_addr, buf, sizeof(buf));
      if (std::find(v4.begin(), v4.end(), buf) == v4.end()) v4.emplace_back(buf);
    } else if (p->ai_family == AF_INET6) {
      inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(p->ai_addr)->sin6_addr, buf, sizeof(buf));
      if (std::find(v6.begin(), v6.end(), buf) == v6.end()) v6.emplace_back(buf);
    }
  }
  freeaddrinfo(res);
  if (v4.empty() && v6.empty()) return r;

  std::string uri;
  auto add = [&](const std::string& part) { uri += (uri.empty() ? "" : ",") + part; };
  if (v6.empty()) {
    for (const auto& ip : v4) add(ip + ":" + port);
    uri = "ipv4:" + uri;
  } else if (v4.empty()) {
    for (const auto& ip : v6) add("[" + ip + "]:" + port);
    uri = "ipv6:" + uri;
  } else {
    // Mixed families share one ipv6: URI with v4-mapped addresses (gRPC dials dual-stack
    // sockets). Loopback names list IPv4 first: a server bound to 127.0.0.1 is the common case
    // and a refused ::1 costs a SYN retry on Windows.
    bool loopback = std::find(v6.begin(), v6.end(), "::1") != v6.end();
    std::vector<std::string> order;
    if (loopback) {
      for (const auto& ip : v4) order.push_back("[::ffff:" + ip + "]:" + port);
      for (const auto& ip : v6) order.push_back("[" + ip + "]:" + port);
    } else {
      for (const auto& ip : v6) order.push_back("[" + ip + "]:" + port);
      for (const auto& ip : v4) order.push_back("[::ffff:" + ip + "]:" + port);
    }
    for (const auto& o : order) add(o);
    uri = "ipv6:" + uri;
  }
  r.uri = std::move(uri);
  r.authority = a;
  r.host = host;
  return r;
}

// Watches a channel that is still connecting, on the completion queue of a call that was started
// with wait_for_ready on it, so the call's setup and first request overlap the TCP/TLS/HTTP/2
// handshake instead of following it. Records when the channel became READY and cancels the call
// as soon as the connection fails (a wait_for_ready call would otherwise wait out its deadline).
// Windows answers a connect to a closed port with RST, yet retries the SYN for ~2 s before it
// reports the refusal, so a call to a local port nothing listens on hung for 2 s. When a channel
// to loopback addresses is still connecting after kRefusedProbeMs (a local connect normally takes
// well under 5 ms), one probe connect per address with SYN retransmissions off tells refused from
// slow at once. Only loopback is probed: nothing is sent off the machine, and a remote host that
// drops packets keeps gRPC's own retries.
constexpr long kRefusedProbeMs = 40;

// The loopback addresses of a resolved "ipv4:a:p,b:p" / "ipv6:[a]:p" URI; empty unless all are.
std::vector<std::pair<std::string, std::string>> loopback_addresses(const std::string& uri) {
  std::vector<std::pair<std::string, std::string>> out;
  std::string list;
  if (uri.rfind("ipv4:", 0) == 0) list = uri.substr(5);
  else if (uri.rfind("ipv6:", 0) == 0) list = uri.substr(5);
  else return out;
  size_t i = 0;
  while (i < list.size()) {
    size_t j = list.find(',', i);
    if (j == std::string::npos) j = list.size();
    std::string a = list.substr(i, j - i);
    i = j + 1;
    std::string host, port;
    if (!a.empty() && a.front() == '[') {
      size_t close = a.find(']');
      if (close == std::string::npos || close + 2 > a.size() || a[close + 1] != ':') return {};
      host = a.substr(1, close - 1);
      port = a.substr(close + 2);
    } else {
      size_t c = a.rfind(':');
      if (c == std::string::npos) return {};
      host = a.substr(0, c);
      port = a.substr(c + 1);
    }
    const bool loop = host.rfind("127.", 0) == 0 || host == "::1" || host.rfind("::ffff:127.", 0) == 0;
    if (!loop || port.empty()) return {};
    out.emplace_back(host, port);
  }
  return out;
}

// True when every loopback address of uri refuses a connection right now.
bool loopback_refused(const std::string& uri) {
#ifdef _WIN32
  const auto addrs = loopback_addresses(uri);
  if (addrs.empty()) return false;
  ensure_winsock();
  for (const auto& [host, port] : addrs) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) return false;
    SOCKET sk = socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP);
    bool refused = false;
    if (sk != INVALID_SOCKET) {
      TCP_INITIAL_RTO_PARAMETERS rto{};
      rto.Rtt = TCP_INITIAL_RTO_UNSPECIFIED_RTT;
      rto.MaxSynRetransmissions = TCP_INITIAL_RTO_NO_SYN_RETRANSMISSIONS;
      DWORD n = 0;
      WSAIoctl(sk, SIO_TCP_INITIAL_RTO, &rto, sizeof(rto), nullptr, 0, &n, nullptr, nullptr);
      if (res->ai_family == AF_INET6) {  // v4-mapped addresses (gRPC dials dual-stack sockets)
        DWORD off = 0;
        setsockopt(sk, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof(off));
      }
      refused = connect(sk, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0 &&
                WSAGetLastError() == WSAECONNREFUSED;
      closesocket(sk);
    }
    freeaddrinfo(res);
    if (!refused) return false;
  }
  return true;
#else
  (void)uri;
  return false;  // elsewhere the refusal reaches gRPC at once
#endif
}

std::string refused_message(const std::string& address) {
  return "connection refused: nothing is listening on " + address;
}

struct ConnWatch {
  std::shared_ptr<grpc::Channel> ch;
  Clock::time_point cap = Clock::time_point::max();  // give up connecting at this point
  Clock::time_point t_ready{};
  bool failed = false, timed_out = false, pending = false;
  grpc::ClientContext* ctx = nullptr;
  std::string probe_uri;  // loopback targets: see loopback_refused
  std::string target_address;
  Clock::time_point probe_at = Clock::time_point::max();
  bool refused = false;

  bool ready() const { return t_ready != Clock::time_point{}; }
  // Re-evaluates the state; re-arms the notification while still connecting.
  void check(grpc::CompletionQueue* cq, void* tag) {
    pending = false;
    if (ready() || failed) return;
    auto st = ch->GetState(true);
    const auto now = Clock::now();
    if (st == GRPC_CHANNEL_READY) {
      t_ready = now;
      return;
    }
    if (st == GRPC_CHANNEL_TRANSIENT_FAILURE || st == GRPC_CHANNEL_SHUTDOWN || now >= cap) {
      failed = true;
      timed_out = now >= cap;
      if (ctx) ctx->TryCancel();
      return;
    }
    if (now >= probe_at) {
      probe_at = Clock::time_point::max();
      if (loopback_refused(probe_uri)) {
        failed = refused = true;
        if (ctx) ctx->TryCancel();
        return;
      }
    }
    // Short slices bound how long a queue shutdown can wait on a watch that is still armed.
    auto slice = std::min(cap, now + std::chrono::milliseconds(20));
    ch->NotifyOnStateChange(st, std::chrono::system_clock::now() +
                                    std::chrono::duration_cast<std::chrono::system_clock::duration>(slice - now),
                            cq, tag);
    pending = true;
  }
};

// Channels whose calls must carry an explicit :authority (a TLS server name that differs from it,
// see make_channel). gRPC C++ otherwise sends the TLS target-name override as every call's
// :authority. Keyed by channel; entries die with their channel.
struct CallAuthorities {
  std::mutex mu;
  std::map<const grpc::Channel*, std::pair<std::weak_ptr<grpc::Channel>, std::string>> map;
};
CallAuthorities& call_authorities() {
  static CallAuthorities* a = new CallAuthorities;
  return *a;
}
void set_call_authority(const std::shared_ptr<grpc::Channel>& ch, const std::string& authority) {
  auto& a = call_authorities();
  std::lock_guard lk(a.mu);
  for (auto it = a.map.begin(); it != a.map.end();) it = it->second.first.expired() ? a.map.erase(it) : std::next(it);
  a.map[ch.get()] = {ch, authority};
}
void apply_call_authority(const std::shared_ptr<grpc::Channel>& ch, grpc::ClientContext& ctx) {
  auto& a = call_authorities();
  std::lock_guard lk(a.mu);
  if (a.map.empty()) return;
  auto it = a.map.find(ch.get());
  if (it != a.map.end() && it->second.first.lock() == ch) ctx.set_authority(it->second.second);
}

// Synchronous wrapper over an async generic bidi stream. Initial metadata is corked so it leaves
// with the first request instead of in its own packet.
class SyncStream {
 public:
  SyncStream(std::shared_ptr<grpc::Channel> ch, std::string method, ConnWatch* watch = nullptr)
      : ch_(ch), stub_(std::move(ch)), method_(std::move(method)), watch_(watch) {
    if (watch_) {
      ctx.set_wait_for_ready(true);
      watch_->ctx = &ctx;
    }
  }
  ~SyncStream() {
    if (rw_ && !finished_) {
      ctx.TryCancel();
      finish();
    }
    rw_.reset();
    cq_.Shutdown();
    void* t;
    bool ok;
    while (cq_.Next(&t, &ok)) {
    }
  }
  grpc::ClientContext ctx;

  bool start() {
    ctx.set_initial_metadata_corked(true);
    apply_call_authority(ch_, ctx);
    rw_ = stub_.PrepareCall(&ctx, method_, &cq_);
    if (!rw_) return false;
    rw_->StartCall(nullptr);  // corked: no op and no tag; metadata leaves with the first write
    if (watch_) watch_->check(&cq_, &watch_tag_);
    return true;
  }
  // Queues several requests back to back; only the last one flushes. last: it also half-closes.
  bool write_all(const std::vector<std::string>& reqs, bool last = false) {
    for (size_t i = 0; i < reqs.size(); ++i) {
      grpc::ByteBuffer bb = string_to_byte_buffer(reqs[i]);
      grpc::WriteOptions o;
      if (i + 1 < reqs.size()) o.set_buffer_hint();
      if (last && i + 1 == reqs.size()) rw_->WriteLast(bb, o, this);
      else rw_->Write(bb, o, this);
      if (!wait()) return false;
      // A wait_for_ready write completes once the transport took it, i.e. right after READY:
      // the fallback for when the state notification is still queued behind it.
      if (watch_ && !watch_->ready() && !watch_->failed) watch_->t_ready = Clock::now();
    }
    return true;
  }
  bool read(std::string& out) {
    grpc::ByteBuffer bb;
    rw_->Read(&bb, this);
    if (!wait()) return false;
    out = byte_buffer_to_string(bb);
    return true;
  }
  // Half-close and collect the status with both ops in flight at once.
  grpc::Status close() {
    if (finished_ || !rw_) return status_;
    rw_->WritesDone(reinterpret_cast<void*>(1));
    rw_->Finish(&status_, reinterpret_cast<void*>(2));
    void* t;
    bool ok;
    for (int i = 0; i < 2 && cq_.Next(&t, &ok);) {
      if (t == &watch_tag_) watch_->check(&cq_, &watch_tag_);
      else ++i;
    }
    finished_ = true;
    return status_;
  }
  grpc::Status finish() {
    if (finished_ || !rw_) return status_;
    rw_->Finish(&status_, this);
    wait();
    finished_ = true;
    return status_;
  }

 private:
  bool wait() {
    void* t;
    bool ok = false;
    for (;;) {
      if (!cq_.Next(&t, &ok)) return false;
      if (t != &watch_tag_) return ok;
      watch_->check(&cq_, &watch_tag_);
    }
  }
  std::shared_ptr<grpc::Channel> ch_;
  grpc::GenericStub stub_;
  std::string method_;
  ConnWatch* watch_ = nullptr;
  char watch_tag_ = 0;
  grpc::CompletionQueue cq_;
  std::unique_ptr<grpc::GenericClientAsyncReaderWriter> rw_;
  bool finished_ = false;
  grpc::Status status_;
};

// Builds every file in `files` into set's own pool, dependencies first. Files the source did not
// provide fall back to the copies linked into libprotobuf (well-known types). The pool reads its
// files from a SetDatabase so Any types can be added later (resolve_any).
void build_pool(SchemaSet& set, std::map<std::string, pb::FileDescriptorProto> files, std::string& err) {
  std::vector<std::string> names;
  names.reserve(files.size());
  for (const auto& kv : files) names.push_back(kv.first);
  set.db = std::make_unique<SetDatabase>(std::move(files));
  set.pool_errors = std::make_unique<PoolErrors>();
  set.own_pool = std::make_unique<pb::DescriptorPool>(set.db.get(), set.pool_errors.get());
  set.pool = set.own_pool.get();
  for (const auto& name : names) (void)set.own_pool->FindFileByName(name);
  set.init_factory();
  if (!set.pool_errors->text.empty()) err += (err.empty() ? "" : "\n") + set.pool_errors->text;
  set.pool_errors->text.clear();
}

// ------------------------------------------------------------------ google.protobuf.Any

// The message type an Any's @type names, resolved the way grpcurl's AnyResolverFromDescriptorSource
// does: the schema itself, then every type linked into the process (the well-known types; reflection
// and protoset schemas get these from their SetDatabase, .proto schemas import them), then, for a
// reflection schema, the server's reflection (fetch), whose files are added to the schema. Null
// when nothing knows the type; that answer is remembered for the schema.
const pb::Descriptor* resolve_any(SchemaSet& set, std::string_view type_url, const AnyFetch* fetch) {
  const std::string_view name = any_type_name(type_url);
  if (name.empty()) return nullptr;
  if (const auto* d = set.pool->FindMessageTypeByName(name)) return d;
  std::lock_guard lk(set.any_mu);
  if (const auto* d = set.pool->FindMessageTypeByName(name)) return d;
  if (set.any_missing.count(name)) return nullptr;
  if (set.importer) {
    if (const auto* g = pb::DescriptorPool::generated_pool()->FindMessageTypeByName(name)) {
      (void)set.importer->Import(std::string(g->file()->name()));
      if (const auto* d = set.pool->FindMessageTypeByName(name)) return d;
    }
  }
  if (set.db && fetch && *fetch) {
    std::map<std::string, pb::FileDescriptorProto> files;
    if ((*fetch)(std::string(name), files)) {
      std::vector<std::string> added;
      for (const auto& [file, fdp] : files)
        if (set.db->add(fdp)) added.push_back(file);
      for (const auto& file : added) (void)set.pool->FindFileByName(file);
      if (const auto* d = set.pool->FindMessageTypeByName(name)) return d;
    }
  }
  set.any_missing.emplace(name);
  return nullptr;
}

// The @type URL in protobuf's JSON error for an Any it cannot resolve ("could not find @type 'X'"),
// or "" for any other error.
std::string missing_any_url(std::string_view error) {
  static constexpr std::string_view kPrefix = "could not find @type '";
  const size_t at = error.find(kPrefix);
  if (at == std::string_view::npos) return {};
  const size_t from = at + kPrefix.size(), to = error.find('\'', from);
  return to == std::string_view::npos ? std::string() : std::string(error.substr(from, to - from));
}

// An Any no resolver knows, printed the way grpcurl prints it.
struct UnknownAny {
  std::string type_url;
  std::string value_b64;
  std::string path;
};
// Stands in for an unknown Any while protobuf prints the message: a StringValue holding the index
// into the UnknownAny list, under a type URL no real Any carries.
constexpr std::string_view kUnknownAnyUrl = "watt.invalid/unknown-any/google.protobuf.StringValue";

std::string unknown_any_error(std::string_view type_url) {
  return std::string(any_type_name(type_url)) + " is not recognized; see @value for raw binary message data";
}

// Replaces every Any in m whose type cannot be resolved with a kUnknownAnyUrl stand-in, recording
// the original in out; descends into resolved Anys (nested Anys) and repacks them when changed.
bool mark_unknown_anys(SchemaSet& set, pb::Message& m, const AnyFetch* fetch, std::vector<UnknownAny>& out,
                       const std::string& path, int depth) {
  if (depth > 64) return false;
  const auto* d = m.GetDescriptor();
  const auto* r = m.GetReflection();
  if (d->full_name() == "google.protobuf.Any") {
    const auto* url_f = d->FindFieldByNumber(1);
    const auto* val_f = d->FindFieldByNumber(2);
    if (!url_f || !val_f) return false;
    const std::string url = r->GetString(m, url_f);
    if (url.empty()) return false;
    if (const auto* t = resolve_any(set, url, fetch)) {
      std::unique_ptr<pb::Message> inner(set.prototype(t)->New());
      if (!inner->ParseFromString(r->GetString(m, val_f))) return false;
      if (!mark_unknown_anys(set, *inner, fetch, out, path, depth + 1)) return false;
      r->SetString(&m, val_f, inner->SerializeAsString());
      return true;
    }
    std::string stand_in;
    put_len_field(stand_in, 1, std::to_string(out.size()));
    out.push_back({url, b64_encode(r->GetString(m, val_f)), path.empty() ? "(message)" : path});
    r->SetString(&m, url_f, std::string(kUnknownAnyUrl));
    r->SetString(&m, val_f, stand_in);
    return true;
  }
  bool changed = false;
  std::vector<const pb::FieldDescriptor*> fields;
  r->ListFields(m, &fields);
  for (const auto* f : fields) {
    if (f->cpp_type() != pb::FieldDescriptor::CPPTYPE_MESSAGE) continue;
    const std::string name = path.empty() ? std::string(f->name()) : path + "." + std::string(f->name());
    if (f->is_repeated()) {
      const int n = r->FieldSize(m, f);
      for (int i = 0; i < n; ++i)
        changed |= mark_unknown_anys(set, *r->MutableRepeatedMessage(&m, f, i), fetch, out,
                                     name + "[" + std::to_string(i) + "]", depth + 1);
    } else {
      changed |= mark_unknown_anys(set, *r->MutableMessage(&m, f), fetch, out, name, depth + 1);
    }
  }
  return changed;
}

// Swaps the stand-ins in printed JSON for grpcurl's {"@error", "@type", "@value"} objects.
void splice_unknown_anys(rapidjson::Value& v, const std::vector<UnknownAny>& unknown,
                         rapidjson::Document::AllocatorType& a) {
  if (v.IsArray()) {
    for (auto& e : v.GetArray()) splice_unknown_anys(e, unknown, a);
    return;
  }
  if (!v.IsObject()) return;
  auto t = v.FindMember("@type");
  if (t != v.MemberEnd() && t->value.IsString() &&
      std::string_view(t->value.GetString(), t->value.GetStringLength()) == kUnknownAnyUrl) {
    auto val = v.FindMember("value");
    size_t idx = unknown.size();
    if (val != v.MemberEnd() && val->value.IsString()) idx = std::strtoul(val->value.GetString(), nullptr, 10);
    if (idx >= unknown.size()) return;
    const auto& u = unknown[idx];
    auto str = [&](const std::string& x) {
      return rapidjson::Value(x.c_str(), static_cast<rapidjson::SizeType>(x.size()), a);
    };
    rapidjson::Value o(rapidjson::kObjectType);
    o.AddMember("@error", str(unknown_any_error(u.type_url)), a);
    o.AddMember("@type", str(u.type_url), a);
    o.AddMember("@value", str(u.value_b64), a);
    v = std::move(o);
    return;
  }
  for (auto m = v.MemberBegin(); m != v.MemberEnd(); ++m) splice_unknown_anys(m->value, unknown, a);
}

// Prints m as JSON (protobuf's printer, raw: 1-space indent), resolving Any types the schema lacks
// (resolve_any). An Any whose type nothing knows is printed as grpcurl does, {"@error", "@type",
// "@value"}, with one note each; *final_form is then set: the result is already 2-space indented.
// The fast path is one MessageToJsonString call; everything else only runs after it failed.
absl::Status print_json_resolving_any(SchemaSet& set, const pb::Message& m, const pb::util::JsonPrintOptions& opts,
                                      const AnyFetch* fetch, std::string& out, std::vector<std::string>* notes,
                                      bool* final_form) {
  *final_form = false;
  out.clear();
  absl::Status st = pb::util::MessageToJsonString(m, &out, opts);
  for (int round = 0; !st.ok() && round < 64; ++round) {
    const std::string url = missing_any_url(st.message());
    if (url.empty() || !resolve_any(set, url, fetch)) break;
    out.clear();
    st = pb::util::MessageToJsonString(m, &out, opts);
  }
  if (st.ok() || missing_any_url(st.message()).empty()) return st;
  // Some Any type is unknown everywhere: print a copy with stand-ins, then splice.
  if (!resolve_any(set, "type.googleapis.com/google.protobuf.StringValue", nullptr)) return st;
  std::unique_ptr<pb::Message> copy(m.New());
  copy->CopyFrom(m);
  std::vector<UnknownAny> unknown;
  if (!mark_unknown_anys(set, *copy, fetch, unknown, "", 0) || unknown.empty()) return st;
  std::string raw;
  if (!pb::util::MessageToJsonString(*copy, &raw, opts).ok()) return st;
  rapidjson::Document doc;
  doc.Parse<rapidjson::kParseFullPrecisionFlag>(raw.c_str(), raw.size());
  if (doc.HasParseError()) return st;
  splice_unknown_anys(doc, unknown, doc.GetAllocator());
  rapidjson::StringBuffer sb;
  if (opts.add_whitespace) {
    rapidjson::PrettyWriter<rapidjson::StringBuffer> w(sb);
    w.SetIndent(' ', 2);
    doc.Accept(w);
  } else {
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    doc.Accept(w);
  }
  out.assign(sb.GetString(), sb.GetSize());
  *final_form = true;
  if (notes) {
    for (const auto& u : unknown)
      notes->push_back(u.path + ": google.protobuf.Any of unknown type " + std::string(any_type_name(u.type_url)) +
                       " (not in the schema" +
                       (set.from_reflection ? ", the well-known types or the server's reflection" : " or the well-known types") +
                       "), shown as @error/@type/@value with the raw bytes in @value");
  }
  return absl::OkStatus();
}

class TextErrors : public pb::io::ErrorCollector {
 public:
  std::string text;
  void RecordError(int line, pb::io::ColumnNumber column, absl::string_view message) override {
    if (!text.empty()) text += "; ";
    text += std::to_string(line + 1) + ":" + std::to_string(column + 1) + ": " + std::string(message);
  }
};

// Encodes request documents (JSON or protobuf text format) into wire buffers with one reusable
// message.
struct Encoder {
  std::unique_ptr<pb::Message> m;
  pb::util::JsonParseOptions json_opts;
  bool text = false;
  explicit Encoder(const pb::Message* proto, bool allow_unknown = false, bool text_format = false)
      : m(proto->New()), text(text_format) {
    json_opts.ignore_unknown_fields = allow_unknown;
  }
  // Resolves an Any @type the request names that the schema lacks (resolve_any); unset = none.
  std::function<bool(const std::string& type_url)> resolve;
  bool encode(const std::string& doc, grpc::ByteBuffer& out, std::string& err) {
    m->Clear();
    if (text) {
      pb::TextFormat::Parser parser;
      TextErrors errors;
      parser.RecordErrorsTo(&errors);
      parser.AllowUnknownField(json_opts.ignore_unknown_fields);
      if (!parser.ParseFromString(doc, m.get())) {
        err = errors.text.empty() ? "not valid protobuf text format" : errors.text;
        return false;
      }
    } else {
      auto st = pb::util::JsonStringToMessage(doc, m.get(), json_opts);
      for (int round = 0; !st.ok() && resolve && round < 64; ++round) {
        const std::string url = missing_any_url(st.message());
        if (url.empty() || !resolve(url)) break;
        m->Clear();
        st = pb::util::JsonStringToMessage(doc, m.get(), json_opts);
      }
      if (!st.ok()) {
        err = std::string(st.message());
        if (!missing_any_url(err).empty())
          err += " (no such message in the schema, the well-known types or the server's reflection)";
        return false;
      }
    }
    bool own = false;
    grpc::SerializationTraits<pb::Message>::Serialize(*m, &out, &own);
    return true;
  }
};

// Process-wide warm-up of everything a first call touches outside the network: protobuf's
// generated descriptors for the well-known types, DescriptorPool::BuildFile, DynamicMessage, the
// JSON parser/printer (incl. WKT special cases), the request-template builder and the .proto
// parser. Runs once per process on the first client's worker thread.
void warm_protobuf() {
  for (const char* f : {"any", "duration", "empty", "struct", "timestamp", "wrappers", "field_mask"})
    pb::DescriptorPool::generated_pool()->FindFileByName(std::string("google/protobuf/") + f + ".proto");
  pb::FileDescriptorProto fdp;
  fdp.set_name("watt/warmup.proto");
  fdp.set_package("watt.warmup");
  fdp.set_syntax("proto3");
  fdp.add_dependency("google/protobuf/timestamp.proto");
  fdp.add_dependency("google/protobuf/struct.proto");
  auto* msg = fdp.add_message_type();
  msg->set_name("W");
  auto field = [&](const char* name, int num, pb::FieldDescriptorProto::Type type, const char* type_name = nullptr,
                   bool repeated = false) {
    auto* f = msg->add_field();
    f->set_name(name);
    f->set_number(num);
    f->set_type(type);
    f->set_label(repeated ? pb::FieldDescriptorProto::LABEL_REPEATED : pb::FieldDescriptorProto::LABEL_OPTIONAL);
    if (type_name) f->set_type_name(type_name);
  };
  field("text", 1, pb::FieldDescriptorProto::TYPE_STRING);
  field("num", 2, pb::FieldDescriptorProto::TYPE_INT64);
  field("ts", 3, pb::FieldDescriptorProto::TYPE_MESSAGE, ".google.protobuf.Timestamp");
  field("meta", 4, pb::FieldDescriptorProto::TYPE_MESSAGE, ".google.protobuf.Struct");
  field("kids", 5, pb::FieldDescriptorProto::TYPE_MESSAGE, ".watt.warmup.W", true);
  std::map<std::string, pb::FileDescriptorProto> files{{fdp.name(), fdp}};
  SchemaSet set;
  std::string err;
  build_pool(set, std::move(files), err);
  const auto* d = set.pool->FindMessageTypeByName("watt.warmup.W");
  if (!d) return;
  (void)grpc_detail::build_request_template(d);
  std::unique_ptr<pb::Message> m(set.prototype(d)->New());
  (void)pb::util::JsonStringToMessage(
      R"({"text":"x","num":"1","ts":"2024-01-01T00:00:00Z","meta":{"a":[1,"b",true,null]},"kids":[{}]})", m.get(),
      pb::util::JsonParseOptions());
  std::string out;
  (void)pb::util::MessageToJsonString(*m, &out, print_options(true));
  (void)m->SerializeAsString();
  (void)rpc_types();
}

// ------------------------------------------------------------------ connection timing

// gRPC does not report when a channel's TCP connect finished, only when the whole connection
// (TCP, TLS, HTTP/2 settings) is READY. To split connect_ms from tls_ms, the process-wide
// EventEngine is wrapped: Connect() calls from channels carrying kConnIdArg record when their TCP
// connect completed. Every other call is forwarded untouched. The real engine is created lazily,
// so installing the wrapper is cheap; warm-up builds it on a worker thread.
namespace ee = grpc_event_engine::experimental;
constexpr const char* kConnIdArg = "watt.conn_id";

struct TcpTimes {
  std::mutex mu;
  std::map<int, Clock::time_point> done;  // conn id -> last successful TCP connect
  void record(int id, Clock::time_point t) {
    std::lock_guard lk(mu);
    done[id] = t;
    if (done.size() > 256) done.erase(done.begin());
  }
  bool get(int id, Clock::time_point& t) {
    std::lock_guard lk(mu);
    auto it = done.find(id);
    if (it == done.end()) return false;
    t = it->second;
    return true;
  }
};
TcpTimes& tcp_times() {
  static TcpTimes* t = new TcpTimes;
  return *t;
}

class TimingEngine final : public ee::EventEngine {
 public:
  ee::EventEngine* inner() {
    std::call_once(once_, [this] { inner_ = ee::CreateEventEngine(); });
    return inner_.get();
  }
  absl::StatusOr<std::unique_ptr<Listener>> CreateListener(
      Listener::AcceptCallback on_accept, absl::AnyInvocable<void(absl::Status)> on_shutdown,
      const ee::EndpointConfig& config, std::unique_ptr<ee::MemoryAllocatorFactory> factory) override {
    return inner()->CreateListener(std::move(on_accept), std::move(on_shutdown), config, std::move(factory));
  }
  ConnectionHandle Connect(OnConnectCallback on_connect, const ResolvedAddress& addr,
                           const ee::EndpointConfig& args, ee::MemoryAllocator memory_allocator,
                           Duration timeout) override {
    auto id = args.GetInt(kConnIdArg);
    if (!id) return inner()->Connect(std::move(on_connect), addr, args, std::move(memory_allocator), timeout);
    return inner()->Connect(
        [id = *id, cb = std::move(on_connect)](absl::StatusOr<std::unique_ptr<Endpoint>> ep) mutable {
          if (ep.ok()) tcp_times().record(id, Clock::now());
          cb(std::move(ep));
        },
        addr, args, std::move(memory_allocator), timeout);
  }
  bool CancelConnect(ConnectionHandle handle) override { return inner()->CancelConnect(handle); }
  bool IsWorkerThread() override { return inner()->IsWorkerThread(); }
  absl::StatusOr<std::unique_ptr<DNSResolver>> GetDNSResolver(const DNSResolver::ResolverOptions& o) override {
    return inner()->GetDNSResolver(o);
  }
  void Run(Closure* closure) override { inner()->Run(closure); }
  void Run(absl::AnyInvocable<void()> closure) override { inner()->Run(std::move(closure)); }
  TaskHandle RunAfter(Duration when, Closure* closure) override { return inner()->RunAfter(when, closure); }
  TaskHandle RunAfter(Duration when, absl::AnyInvocable<void()> closure) override {
    return inner()->RunAfter(when, std::move(closure));
  }
  bool Cancel(TaskHandle handle) override { return inner()->Cancel(handle); }
  void* QueryExtension(absl::string_view id) override { return inner()->QueryExtension(id); }

 private:
  std::once_flag once_;
  std::shared_ptr<ee::EventEngine> inner_;
};

// Installs the wrapper as gRPC's default engine once per process, before the first grpc_init.
// Returns it so warm-up can build the real engine early. Held for the life of the process.
TimingEngine* timing_engine() {
  static TimingEngine* e = [] {
    auto sp = std::make_shared<TimingEngine>();
    auto* raw = sp.get();
    ee::SetDefaultEventEngine(sp);
    new std::shared_ptr<TimingEngine>(sp);  // never released: the engine outlives every channel
    return raw;
  }();
  return e;
}

#ifdef _WIN32
using sock_t = SOCKET;
constexpr sock_t kNoSock = INVALID_SOCKET;
void close_sock(sock_t s) { closesocket(s); }
#else
using sock_t = int;
constexpr sock_t kNoSock = -1;
void close_sock(sock_t s) { ::close(s); }
#endif

// Loads the Winsock provider and the socket/ConnectEx paths a first connection walks.
void warm_socket() {
  ensure_winsock();
#ifdef _WIN32
  SOCKET s = WSASocketW(AF_INET6, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
  if (s == INVALID_SOCKET) return;
  DWORD off = 0;
  setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof(off));
  BOOL on = TRUE;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
  GUID guid = WSAID_CONNECTEX;
  LPFN_CONNECTEX fn = nullptr;
  DWORD n = 0;
  WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &fn, sizeof(fn), &n, nullptr, nullptr);
  sockaddr_in6 any{};
  any.sin6_family = AF_INET6;
  bind(s, reinterpret_cast<sockaddr*>(&any), sizeof(any));
  closesocket(s);
#endif
}

grpc::ChannelArguments base_channel_args(int max_message_bytes = 0) {
  grpc::ChannelArguments args;
  args.SetMaxReceiveMessageSize(max_message_bytes > 0 ? max_message_bytes : -1);
  args.SetMaxSendMessageSize(max_message_bytes > 0 ? max_message_bytes : -1);
  args.SetInt(GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 1);  // a fresh channel really reconnects
  args.SetInt(GRPC_ARG_ENABLE_RETRIES, 0);             // a tester shows what the server did
  return args;
}

// Walks channel creation, the subchannel connector, a TCP connect and the HTTP/2 handshake once,
// against a throwaway listener on 127.0.0.1 that answers with an empty SETTINGS frame. The first
// real connection of the process then skips that code's cold cost. Nothing leaves the machine, and
// the channel is destroyed while its connection is still up, so it never retries in the background.
void warm_channel() {
  ensure_winsock();
  sock_t ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (ls == kNoSock) return;
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = sizeof(sa);
  if (bind(ls, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0 || listen(ls, 1) != 0 ||
      getsockname(ls, reinterpret_cast<sockaddr*>(&sa), &len) != 0) {
    close_sock(ls);
    return;
  }
  sock_t cs = kNoSock;
  {
    auto ch = grpc::CreateCustomChannel("ipv4:127.0.0.1:" + std::to_string(ntohs(sa.sin_port)),
                                        grpc::InsecureChannelCredentials(), base_channel_args());
    ch->GetState(true);
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(ls, &rd);
    timeval tv{0, 300000};
    if (select(static_cast<int>(ls) + 1, &rd, nullptr, nullptr, &tv) == 1) cs = accept(ls, nullptr, nullptr);
    if (cs != kNoSock) {
      static const char kSettings[9] = {0, 0, 0, 4, 0, 0, 0, 0, 0};  // empty server SETTINGS frame
      send(cs, kSettings, sizeof(kSettings), 0);
      ch->WaitForConnected(std::chrono::system_clock::now() + std::chrono::milliseconds(300));
      grpc::ByteBuffer bb = string_to_byte_buffer("warm");
      (void)byte_buffer_to_string(bb);
    }
  }
  if (cs != kNoSock) close_sock(cs);
  close_sock(ls);
}

// Process-wide stop for the deferred part of the warm-up: set when the client that started it is
// destroyed. warm_pause(ms) waits up to ms and returns false once stopped.
struct WarmStop {
  std::mutex mu;
  std::condition_variable cv;
  bool stop = false;
};
WarmStop& warm_stop() {
  static WarmStop* w = new WarmStop;
  return *w;
}
bool warm_pause(long ms) {
  auto& w = warm_stop();
  std::unique_lock lk(w.mu);
  if (ms > 0) w.cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return w.stop; });
  return !w.stop;
}
constexpr long kWarmLongLivedMs = 30;

// The first TLS channel of a process pays one-time setup that later ones skip: OpenSSL's init and
// first SSL_CTX (~3.5 ms, inside the call's SslCredentials creation, before its channel exists),
// gRPC's default root store (~7 ms, system roots, for targets without a custom CA), and gRPC's
// TSI/handshaker code. This does OpenSSL's part at once and the rest, for a process that lives
// on, by driving a default-roots channel against a throwaway loopback listener as far as its
// ClientHello, which the listener reads and drops. Nothing leaves the machine and no certificate
// is involved.
void warm_tls() {
  // OpenSSL first and at once: config, the default provider and the cipher/group/sigalg tables
  // the first SSL_CTX fetches. A TLS call that arrives meanwhile joins this instead of starting
  // from zero.
  OPENSSL_init_ssl(0, nullptr);
  SSL_CTX_free(SSL_CTX_new(TLS_client_method()));
  // The rest pays off only in a process that lives on (a GUI, a session of calls): wait a moment
  // and skip it when the client goes away meanwhile, so a one-shot process neither waits at exit
  // for it (the destructor joins this thread) nor has its first call share the CPU with it.
  if (!warm_pause(kWarmLongLivedMs)) return;
  ensure_winsock();
  sock_t ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (ls == kNoSock) return;
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = sizeof(sa);
  if (bind(ls, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0 || listen(ls, 1) != 0 ||
      getsockname(ls, reinterpret_cast<sockaddr*>(&sa), &len) != 0) {
    close_sock(ls);
    return;
  }
  sock_t cs = kNoSock;
  {
    grpc::ChannelArguments args = base_channel_args();
    args.SetSslTargetNameOverride("localhost");
    auto ch = grpc::CreateCustomChannel("ipv4:127.0.0.1:" + std::to_string(ntohs(sa.sin_port)),
                                        grpc::SslCredentials(grpc::SslCredentialsOptions()), args);
    ch->GetState(true);
    auto readable = [](sock_t s, long usec) {
      fd_set rd;
      FD_ZERO(&rd);
      FD_SET(s, &rd);
      timeval tv{0, usec};
      return select(static_cast<int>(s) + 1, &rd, nullptr, nullptr, &tv) == 1;
    };
    if (readable(ls, 500000)) cs = accept(ls, nullptr, nullptr);
    if (cs != kNoSock && readable(cs, 500000)) {
      char buf[2048];
      (void)recv(cs, buf, sizeof(buf), 0);  // the ClientHello: the handshaker is fully built
    }
  }  // channel destroyed while still handshaking, so it never retries
  if (cs != kNoSock) close_sock(cs);
  close_sock(ls);
}

// gRPC's TSI layer registers an atexit handler on first TLS use that waits up to
// grpc.openssl_cleanup_timeout seconds (default 2) for every grpc_init() to be released. This
// engine keeps one reference for the life of the process (see Impl()), so any process that made a
// TLS call sat in exit for the full 2 s. At exit nothing is waiting for gRPC any more: OpenSSL
// is a separate DLL whose own cleanup runs after the process's threads are gone, so the wait
// buys nothing. Respect a value the user set.
void no_tls_exit_wait() {
  constexpr const char* kVar = "grpc.openssl_cleanup_timeout";
#ifdef _WIN32
  if (GetEnvironmentVariableA(kVar, nullptr, 0) == 0) SetEnvironmentVariableA(kVar, "0");
#else
  setenv(kVar, "0", 0);
#endif
}

}  // namespace

// ================================================================== Impl

struct GrpcClient::Impl {
  // One cached channel per target. The slot mutex serialises (re)building so a preconnect and
  // a call never race to create two channels.
  struct ChanSlot {
    GrpcTarget target;
    std::mutex m;
    std::shared_ptr<grpc::Channel> ch;
    double dns_ms = 0;
    int conn_id = 0;
    std::string uri;  // what the channel dials (resolved), for the refused-connection probe
  };

  std::mutex mu;  // slots + schemas + reflection versions
  std::vector<std::shared_ptr<ChanSlot>> slots;
  std::vector<std::shared_ptr<SchemaSet>> schemas;  // newest last
  std::map<std::string, bool> refl_v1alpha;         // address -> server only speaks v1alpha

  std::mutex call_mu;  // active contexts for cancel()
  std::vector<grpc::ClientContext*> active;
  uint64_t cancel_gen = 0;

  // Background worker for preconnects and post-load warm-ups, started on first use.
  std::mutex job_mu;
  std::condition_variable job_cv;
  std::deque<std::function<void()>> jobs;
  bool stop = false;
  std::thread worker;
  std::once_flag grpc_once;
  bool grpc_ref = false;  // this client holds a grpc_init() reference (taken with its first channel)
  std::thread warm_thread;  // the process-wide warm-up, joined by the client that started it
  std::thread warm_tls_thread;  // the process-wide TLS warm-up (warm_tls), likewise

  static std::atomic<bool>& any_channel() {  // a real channel was built: the warm-up walk is moot
    static std::atomic<bool> built{false};
    return built;
  }

  // Cold start. The first client of the process starts one warm-up thread, so a call that arrives
  // straight away is never queued behind it: it builds gRPC's EventEngine (an IOCP poller plus
  // ~16 threads, which the first channel would otherwise build after grpc_init), loads the socket
  // provider, runs grpc_init (a call arriving first does that itself) and keeps that reference for
  // the life of the process, warms protobuf's descriptor/JSON machinery, and walks a loopback
  // channel if no real channel exists yet. A second thread warms the TLS stack (warm_tls). Work
  // that only helps a process that lives on is deferred and skipped if this client goes away
  // first, so a one-shot process does not wait for it at exit. Later clients find all of it done.
  Impl() {
    // The TLS stack warms on its own thread (see warm_tls), so a plaintext call never waits
    // behind it.
    static std::atomic<bool> tls_started{false};
    if (!tls_started.exchange(true)) {
      no_tls_exit_wait();
      warm_tls_thread = std::thread([] { warm_tls(); });
    }
    (void)timing_engine();  // before any grpc_init: gRPC's default engine is our wrapper
    static std::atomic<bool> started{false};
    if (!started.exchange(true)) warm_thread = std::thread([] {
      timing_engine()->inner();
      warm_socket();
      grpc_init();  // never released: gRPC stays initialized while the process runs
      // Before protobuf: a first TLS call spends its first ~4 ms in OpenSSL's init before its
      // channel exists, and this walk of the channel stack fits in that gap.
      if (!any_channel().load()) warm_channel();
      warm_protobuf();
      // Only for a process that lives on: a one-shot call resolves its own name meanwhile.
      if (warm_pause(kWarmLongLivedMs)) (void)resolve_target("localhost:1");  // the DNS client
    });
  }
  ~Impl() {
    {
      std::lock_guard lk(job_mu);
      stop = true;
      jobs.clear();
    }
    job_cv.notify_all();
    if (worker.joinable()) worker.join();
    if (warm_thread.joinable() || warm_tls_thread.joinable()) {  // the client that started them
      {
        std::lock_guard lk(warm_stop().mu);
        warm_stop().stop = true;
      }
      warm_stop().cv.notify_all();
    }
    if (warm_thread.joinable()) warm_thread.join();
    if (warm_tls_thread.joinable()) warm_tls_thread.join();
    slots.clear();
    if (grpc_ref) grpc_shutdown();
  }

  void work() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock lk(job_mu);
        job_cv.wait(lk, [&] { return stop || !jobs.empty(); });
        if (stop) return;
        job = std::move(jobs.front());
        jobs.pop_front();
      }
      job();
    }
  }
  // front: preconnects go ahead of the remaining warm-up, which only matters if no channel exists.
  void post(std::function<void()> job, bool front = false) {
    {
      std::lock_guard lk(job_mu);
      if (stop) return;
      if (front) jobs.push_front(std::move(job));
      else jobs.push_back(std::move(job));
      if (!worker.joinable()) worker = std::thread([this] { work(); });
    }
    job_cv.notify_one();
  }

  uint64_t generation() {
    std::lock_guard lk(call_mu);
    return cancel_gen;
  }
  bool cancelled_since(uint64_t gen) {
    std::lock_guard lk(call_mu);
    return cancel_gen != gen;
  }

  // RAII registration so cancel() reaches the context for exactly its lifetime.
  struct Active {
    Impl& im;
    grpc::ClientContext* ctx;
    Active(Impl& i, grpc::ClientContext* c, uint64_t gen) : im(i), ctx(c) {
      std::lock_guard lk(im.call_mu);
      im.active.push_back(c);
      if (im.cancel_gen != gen) c->TryCancel();
    }
    ~Active() {
      std::lock_guard lk(im.call_mu);
      im.active.erase(std::remove(im.active.begin(), im.active.end(), ctx), im.active.end());
    }
  };

  static std::shared_ptr<grpc::Channel> make_channel(const GrpcTarget& t, double& dns_ms, int conn_id,
                                                     std::string* uri = nullptr) {
    ResolvedTarget rt = resolve_target(t.address);
    dns_ms = rt.dns_ms;
    if (uri) *uri = rt.uri;
    grpc::ChannelArguments args = base_channel_args(t.max_message_bytes);
    args.SetInt(kConnIdArg, conn_id);  // lets the engine wrapper time this channel's TCP connects
    if (t.connect_timeout_ms > 0) {
      // gRPC gives each connection attempt max(20 s, backoff); make the attempt itself give up too.
      const int ms = static_cast<int>(std::min<long>(t.connect_timeout_ms, INT32_MAX));
      args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, ms);
      args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, ms);
    }
    if (t.keepalive_ms > 0) {
      args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, static_cast<int>(std::min<long>(t.keepalive_ms, INT32_MAX)));
      args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 20000);
      args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
      args.SetInt(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);
    }
    if (!t.user_agent.empty()) args.SetUserAgentPrefix(t.user_agent);
    // :authority is what the user set, else the host:port they typed when the URI was rewritten.
    // The TLS name (SNI + certificate check) is server_name, else that authority, else the host.
    const std::string& authority = !t.authority.empty() ? t.authority : rt.authority;
    if (!authority.empty()) args.SetString(GRPC_ARG_DEFAULT_AUTHORITY, authority);
    // A TLS name that differs from :authority: gRPC C++ sends the SSL target-name override as every
    // call's :authority, so each call on such a channel sets its authority explicitly (see
    // apply_call_authority). That call host then does not match the certificate on purpose, so
    // these channels use TlsCredentials, which can skip the per-call host check (the handshake
    // still verifies server_name).
    const bool split_name = t.tls && !t.server_name.empty() && !authority.empty() && t.server_name != authority;
    if (t.tls) {
      if (!t.server_name.empty()) {
        args.SetSslTargetNameOverride(t.server_name);
      } else if (!t.authority.empty()) {
        args.SetSslTargetNameOverride(t.authority);
      } else if (!rt.authority.empty()) {
        args.SetSslTargetNameOverride(rt.host);
      }
    }
    const bool mtls = !t.client_cert_pem.empty() && !t.client_key_pem.empty();
    std::shared_ptr<grpc::ChannelCredentials> creds;
    if (!t.tls) {
      creds = grpc::InsecureChannelCredentials();
    } else if (t.insecure_skip_verify || split_name) {
      grpc::experimental::TlsChannelCredentialsOptions o;
      o.set_check_call_host(false);
      if (t.insecure_skip_verify) {
        o.set_verify_server_certs(false);
        o.set_certificate_verifier(std::make_shared<grpc::experimental::NoOpCertificateVerifier>());
      } else {
        o.set_verify_server_certs(true);
        o.set_certificate_verifier(std::make_shared<grpc::experimental::HostNameCertificateVerifier>());
      }
      if (!t.root_cert_pem.empty() || mtls) {
        std::vector<grpc::experimental::IdentityKeyCertPair> ids;
        if (mtls) ids.push_back({t.client_key_pem, t.client_cert_pem});
        o.set_certificate_provider(
            std::make_shared<grpc::experimental::StaticDataCertificateProvider>(t.root_cert_pem, ids));
        if (!t.root_cert_pem.empty()) o.watch_root_certs();
        if (mtls) o.watch_identity_key_cert_pairs();
      }
      creds = grpc::experimental::TlsCredentials(o);
    } else {
      grpc::SslCredentialsOptions o;
      o.pem_root_certs = t.root_cert_pem;
      if (mtls) {
        o.pem_cert_chain = t.client_cert_pem;
        o.pem_private_key = t.client_key_pem;
      }
      creds = grpc::SslCredentials(o);
    }
    auto ch = grpc::CreateCustomChannel(rt.uri, creds, args);
    if (split_name) set_call_authority(ch, authority);
    return ch;
  }

  struct Lease {
    std::shared_ptr<grpc::Channel> ch;
    bool reused = false;
    double dns_ms = 0;      // name lookup done by this call
    double connect_ms = 0;  // channel setup + wait for READY done by this call (TLS: up to TCP)
    double tls_ms = 0;      // TLS targets: TCP connected -> READY (TLS + HTTP/2 settings)
    int conn_id = 0;
    bool timed_out = false; // still connecting when the connect limit or the deadline passed
    bool refused = false;   // a loopback target refused the connection (see loopback_refused)
    std::string uri;        // what the channel dials
  };

  // Splits the wait for a TLS channel into connect_ms (until TCP connected) and tls_ms (TLS
  // handshake + HTTP/2 settings until READY), using the engine wrapper's TCP timestamp.
  static void time_connect(const GrpcTarget& t, Lease& l, Clock::time_point t0, Clock::time_point ready) {
    const double dns = l.dns_ms;
    l.connect_ms = std::max(0.0, ms_between(t0, ready) - dns);
    Clock::time_point tcp;
    if (!t.tls || !tcp_times().get(l.conn_id, tcp) || tcp > ready) return;
    const auto from = std::max(tcp, t0);
    l.tls_ms = ms_between(from, ready);
    l.connect_ms = std::max(0.0, ms_between(t0, from) - dns);
  }

  std::shared_ptr<ChanSlot> slot_for(const GrpcTarget& t) {
    std::lock_guard lk(mu);
    for (auto& s : slots)
      if (s->target == t) return s;
    auto s = std::make_shared<ChanSlot>();
    s->target = t;
    slots.push_back(s);
    return s;
  }

  // Returns the slot's channel, (re)building it when missing or failed. built=true when this
  // thread built it (and so paid for the lookup).
  std::shared_ptr<grpc::Channel> channel(ChanSlot& s, bool& built, double& dns_ms, int* conn_id = nullptr,
                                         std::string* uri = nullptr) {
    static std::atomic<int> next_id{0};
    std::lock_guard lk(s.m);
    built = false;
    if (s.ch) {
      auto st = s.ch->GetState(false);
      if (st == GRPC_CHANNEL_TRANSIENT_FAILURE || st == GRPC_CHANNEL_SHUTDOWN) s.ch.reset();  // skip backoff
    }
    if (!s.ch) {
      // A plaintext channel makes the warm-up's loopback walk moot at once (running both costs
      // the call); a TLS call first spends ~4 ms in OpenSSL's init, and the walk fits in that gap.
      if (!s.target.tls) any_channel() = true;
      std::call_once(grpc_once, [this] {
        grpc_init();
        grpc_ref = true;
      });
      s.conn_id = ++next_id;
      s.ch = make_channel(s.target, s.dns_ms, s.conn_id, &s.uri);
      any_channel() = true;
      built = true;
      dns_ms = s.dns_ms;
    }
    if (conn_id) *conn_id = s.conn_id;
    if (uri) *uri = s.uri;
    return s.ch;
  }

  // Returns a cached channel, (re)connecting and timing the wait for READY when needed.
  // Gets the channel and starts it connecting without waiting. reused = already READY.
  Lease begin_connect(const GrpcTarget& t, Clock::time_point& t0) {
    Lease l;
    t0 = Clock::now();
    auto slot = slot_for(t);
    bool built = false;
    l.ch = channel(*slot, built, l.dns_ms, &l.conn_id, &l.uri);
    l.reused = l.ch->GetState(true) == GRPC_CHANNEL_READY && !built;
    if (l.reused) l.dns_ms = 0;
    return l;
  }

  Lease connect(const GrpcTarget& t, Clock::time_point deadline, uint64_t gen) {
    Clock::time_point t0;
    Lease l = begin_connect(t, t0);
    if (l.reused) return l;
    const double dns = l.dns_ms;
    auto state = l.ch->GetState(false);
    auto cap = t0 + std::chrono::milliseconds(connect_limit_ms(t));
    if (deadline > cap) deadline = cap;
    auto probe_at = t0 + std::chrono::milliseconds(kRefusedProbeMs);
    while (state != GRPC_CHANNEL_READY && state != GRPC_CHANNEL_TRANSIENT_FAILURE &&
           state != GRPC_CHANNEL_SHUTDOWN) {
      auto now = Clock::now();
      if (now >= deadline || cancelled_since(gen)) break;
      if (now >= probe_at) {
        probe_at = Clock::time_point::max();
        if (loopback_refused(l.uri)) {
          l.refused = true;
          break;
        }
      }
      auto slice = std::min(deadline, now + std::chrono::milliseconds(20));
      auto sys = std::chrono::system_clock::now() +
                 std::chrono::duration_cast<std::chrono::system_clock::duration>(slice - now);
      l.ch->WaitForStateChange(state, sys);
      state = l.ch->GetState(true);
    }
    l.timed_out = !l.refused && (state == GRPC_CHANNEL_IDLE || state == GRPC_CHANNEL_CONNECTING);
    l.dns_ms = dns;
    time_connect(t, l, t0, Clock::now());
    return l;
  }

  void preconnect(const GrpcTarget& t) {
    auto slot = slot_for(t);
    post([this, slot] {
      bool built = false;
      double dns = 0;
      channel(*slot, built, dns)->GetState(true);
    }, true);
  }

  // Full reflection results replace every reflection schema of their target; partial ones (one
  // service, from invoke) are added next to them.
  void publish(std::shared_ptr<SchemaSet> s, bool partial = false) {
    std::lock_guard lk(mu);
    if (s->from_reflection) {
      schemas.erase(std::remove_if(schemas.begin(), schemas.end(),
                                   [&](auto& o) {
                                     return o->from_reflection && o->target == s->target &&
                                            (!partial || o->partial_of == s->partial_of);
                                   }),
                    schemas.end());
    }
    schemas.push_back(std::move(s));
    if (schemas.size() > kMaxSchemas) schemas.erase(schemas.begin());
  }

  // Newest schema that knows the method, preferring reflection from the same target.
  std::pair<std::shared_ptr<SchemaSet>, const pb::MethodDescriptor*> lookup(const GrpcTarget& t,
                                                                             const std::string& svc,
                                                                             const std::string& method) {
    std::lock_guard lk(mu);
    std::pair<std::shared_ptr<SchemaSet>, const pb::MethodDescriptor*> fallback{nullptr, nullptr};
    for (auto it = schemas.rbegin(); it != schemas.rend(); ++it) {
      const auto* m = find_method((*it)->pool, svc, method);
      if (!m) continue;
      if (!(*it)->from_reflection || (*it)->target == t) return {*it, m};
      if (!fallback.second) fallback = {*it, m};
    }
    return fallback;
  }

  // Reflection over an existing channel. symbols empty = list_services and load every service;
  // otherwise only the files defining those symbols (plus missing dependencies) are fetched.
  // watch: the channel is still connecting; the first attempt rides on the handshake and the
  // connection outcome lands in *watch. status (optional) receives the failing status.
  GrpcSchema reflect(const GrpcTarget& t, const std::shared_ptr<grpc::Channel>& ch, Clock::time_point t0,
                     long deadline_ms, const Metadata& md, const std::vector<std::string>& symbols, uint64_t gen,
                     ConnWatch* watch = nullptr, grpc::Status* status = nullptr);
  GrpcSchema reflect(const GrpcTarget& t, long deadline_ms, const Metadata& md);
  grpc::Status reflect_once(const std::shared_ptr<grpc::Channel>& ch, const char* method, long deadline_ms,
                            uint64_t gen, const Metadata& md, bool list,
                            std::map<std::string, pb::FileDescriptorProto>& files, std::vector<std::string>& services,
                            std::string& err, ConnWatch* watch);
  // Fetches the files defining `symbol` (and their missing dependencies) from t's reflection
  // service, for an Any whose type the call's reflected schema lacks. Uses the version (v1 or
  // v1alpha) that worked for t before.
  bool fetch_symbol(const GrpcTarget& t, const Metadata& md, const std::string& symbol,
                    std::map<std::string, pb::FileDescriptorProto>& files);
  // The status a call fails with on a channel whose connection failed (UNAVAILABLE + gRPC's reason).
  grpc::Status connect_error(const std::shared_ptr<grpc::Channel>& ch, const ConnWatch& w);

  // After a full schema load, parses each method's request template once on the worker so the
  // first real call of each type skips the JSON parser's per-type setup (~1.6 ms for a rich type).
  void warm_json(const std::shared_ptr<SchemaSet>& set, const std::vector<GrpcMethodInfo>& methods) {
    std::vector<std::pair<std::string, std::string>> todo;  // input type, template
    std::set<std::string> seen;
    for (const auto& m : methods)
      if (seen.insert(m.input_type).second && todo.size() < 256) todo.emplace_back(m.input_type, m.request_template);
    post([set, todo = std::move(todo)] {
      for (const auto& [type, tmpl] : todo) {
        const auto* d = set->pool->FindMessageTypeByName(type);
        if (!d) continue;
        std::unique_ptr<pb::Message> m(set->prototype(d)->New());
        (void)pb::util::JsonStringToMessage(tmpl, m.get(), pb::util::JsonParseOptions());
      }
    });
  }

  // Resolves a method for a call, auto-reflecting (and connecting) on first use of a target.
  struct Prepared {
    std::shared_ptr<SchemaSet> schema;
    const pb::MethodDescriptor* method = nullptr;
    Lease lease;
    bool have_lease = false;
    double reflect_ms = 0;
    grpc::StatusCode code = grpc::StatusCode::OK;
    std::string error;
  };
  Prepared prepare(const GrpcTarget& t, const std::string& full_path, const GrpcCallOptions& opts,
                   Clock::time_point t0, Clock::time_point deadline, uint64_t gen);
};

grpc::Status GrpcClient::Impl::reflect_once(const std::shared_ptr<grpc::Channel>& ch, const char* method,
                                            long deadline_ms, uint64_t gen, const Metadata& md, bool list,
                                            std::map<std::string, pb::FileDescriptorProto>& files,
                                            std::vector<std::string>& services, std::string& err,
                                            ConnWatch* watch) {
  const auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(deadline_ms);
  auto add_files = [&](const ReflResponse& r) {
    for (const auto& bytes : r.files) {
      pb::FileDescriptorProto fdp;
      if (fdp.ParseFromString(bytes) && !files.count(fdp.name())) files.emplace(fdp.name(), std::move(fdp));
    }
  };
  // One stream per batch of requests, the last of which carries END_STREAM: the server closes the
  // stream right after its last response, so no extra round trip waits for the trailers. Only the
  // first stream can be waiting for the connection.
  auto exchange = [&](const std::vector<std::string>& reqs, std::vector<ReflResponse>& out) -> grpc::Status {
    SyncStream s(ch, method, watch);
    watch = nullptr;
    if (deadline_ms > 0) s.ctx.set_deadline(deadline);
    std::string md_err;
    if (!apply_metadata(s.ctx, md, md_err)) {
      err = md_err;
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, md_err);
    }
    Active reg(*this, &s.ctx, gen);
    if (!s.start() || !s.write_all(reqs, true)) return s.finish();
    for (size_t i = 0; i < reqs.size(); ++i) {
      std::string bytes;
      if (!s.read(bytes)) return s.finish();
      ReflResponse rr;
      if (!parse_refl_response(bytes, rr)) {
        s.ctx.TryCancel();
        s.finish();
        err = "malformed reflection response";
        return grpc::Status(grpc::StatusCode::INTERNAL, err);
      }
      out.push_back(std::move(rr));
    }
    return s.finish();
  };

  std::vector<ReflResponse> resp;
  if (list) {
    std::string req;
    put_len_field(req, 7, "*");  // list_services
    grpc::Status st = exchange({req}, resp);
    if (!st.ok()) return st;
    if (resp[0].is_error) {
      err = "list_services: " + resp[0].error_message;
      return grpc::Status(static_cast<grpc::StatusCode>(resp[0].error_code), resp[0].error_message);
    }
    for (auto& name : resp[0].services) {
      if (name == "grpc.reflection.v1.ServerReflection" || name == "grpc.reflection.v1alpha.ServerReflection")
        continue;
      services.push_back(name);
    }
  }

  std::vector<std::string> reqs;
  for (const auto& svc : services) {
    std::string r;
    put_len_field(r, 4, svc);  // file_containing_symbol
    reqs.push_back(std::move(r));
  }
  std::set<std::string> requested;
  while (!reqs.empty()) {
    resp.clear();
    grpc::Status st = exchange(reqs, resp);
    if (!st.ok()) return st;
    for (const auto& r : resp) {
      if (r.is_error && !err.empty()) err += "\n";
      if (r.is_error) err += r.error_message;
      add_files(r);
    }
    // Transitive dependencies the server has not sent yet (servers usually send them all, so
    // this normally costs nothing; linked-in well-known types are never fetched).
    reqs.clear();
    for (const auto& [name, fdp] : files) {
      for (const auto& dep : fdp.dependency()) {
        if (files.count(dep) || requested.count(dep)) continue;
        requested.insert(dep);
        if (pb::DescriptorPool::generated_pool()->FindFileByName(dep)) continue;
        std::string r;
        put_len_field(r, 3, dep);  // file_by_filename
        reqs.push_back(std::move(r));
      }
    }
  }
  return grpc::Status::OK;
}

bool GrpcClient::Impl::fetch_symbol(const GrpcTarget& t, const Metadata& md, const std::string& symbol,
                                    std::map<std::string, pb::FileDescriptorProto>& files) {
  static const char* kV1 = "/grpc.reflection.v1.ServerReflection/ServerReflectionInfo";
  static const char* kV1alpha = "/grpc.reflection.v1alpha.ServerReflection/ServerReflectionInfo";
  const uint64_t gen = generation();
  Clock::time_point tc;
  Lease l = begin_connect(t, tc);
  const std::string key = normalize_address(t.address);
  bool alpha;
  {
    std::lock_guard lk(mu);
    alpha = refl_v1alpha.count(key) && refl_v1alpha[key];
  }
  constexpr long kFetchDeadlineMs = 5000;
  for (int attempt = 0; attempt < 2; ++attempt, alpha = !alpha) {
    files.clear();
    std::vector<std::string> services{symbol};
    std::string err;
    grpc::Status st = reflect_once(l.ch, alpha ? kV1alpha : kV1, kFetchDeadlineMs, gen, md, false, files, services,
                                   err, nullptr);
    if (st.error_code() == grpc::StatusCode::UNIMPLEMENTED) continue;
    return st.ok() && !files.empty();
  }
  return false;
}

grpc::Status GrpcClient::Impl::connect_error(const std::shared_ptr<grpc::Channel>& ch, const ConnWatch& w) {
  if (w.refused) return grpc::Status(grpc::StatusCode::UNAVAILABLE, refused_message(w.target_address));
  if (w.timed_out) return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "timed out connecting to the server");
  // A call that does not wait for ready fails at once on a failed channel, carrying gRPC's own
  // reason ("failed to connect to all addresses; last error: ...").
  SyncStream probe(ch, "/grpc.reflection.v1.ServerReflection/ServerReflectionInfo");
  probe.ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(1000));
  grpc::Status st = probe.start() ? probe.close() : probe.finish();
  if (st.error_code() == grpc::StatusCode::UNAVAILABLE) return st;
  return grpc::Status(grpc::StatusCode::UNAVAILABLE, "failed to connect to the server");
}

GrpcSchema GrpcClient::Impl::reflect(const GrpcTarget& t, const std::shared_ptr<grpc::Channel>& ch,
                                     Clock::time_point t0, long deadline_ms, const Metadata& md,
                                     const std::vector<std::string>& symbols, uint64_t gen, ConnWatch* watch,
                                     grpc::Status* status) {
  GrpcSchema out;
  auto remaining = [&]() -> long {
    if (deadline_ms <= 0) return 0;
    return std::max<long>(1, deadline_ms - static_cast<long>(ms_between(t0, Clock::now())));
  };
  const bool list = symbols.empty();
  const std::string key = normalize_address(t.address);
  bool alpha_first;
  {
    std::lock_guard lk(mu);
    alpha_first = refl_v1alpha.count(key) && refl_v1alpha[key];
  }
  static const char* kV1 = "/grpc.reflection.v1.ServerReflection/ServerReflectionInfo";
  static const char* kV1alpha = "/grpc.reflection.v1alpha.ServerReflection/ServerReflectionInfo";

  std::map<std::string, pb::FileDescriptorProto> files;
  std::vector<std::string> services = symbols;
  std::string err;
  grpc::Status st =
      reflect_once(ch, alpha_first ? kV1alpha : kV1, remaining(), gen, md, list, files, services, err, watch);
  if (watch && watch->failed) {
    st = connect_error(ch, *watch);
    err.clear();
  } else if (st.error_code() == grpc::StatusCode::UNIMPLEMENTED) {
    files.clear();
    services = symbols;
    err.clear();
    alpha_first = !alpha_first;
    st = reflect_once(ch, alpha_first ? kV1alpha : kV1, remaining(), gen, md, list, files, services, err, nullptr);
    if (st.ok()) {
      std::lock_guard lk(mu);
      refl_v1alpha[key] = alpha_first;
    }
  }
  if (!st.ok()) {
    if (status) *status = st;
    out.error = std::string("reflection failed: ") + status_name(st.error_code()) + ": " + st.error_message();
    if (!err.empty() && err != st.error_message()) out.error += " (" + err + ")";
    out.load_ms = ms_between(t0, Clock::now());
    return out;
  }

  auto set = std::make_shared<SchemaSet>();
  set->from_reflection = true;
  set->target = t;
  if (!list) set->partial_of = symbols.front();
  for (const auto& kv : files) set->roots.push_back(kv.first);
  build_pool(*set, std::move(files), err);
  for (const auto& svc : services) {
    const auto* sd = set->pool->FindServiceByName(svc);
    if (sd) append_methods(sd, out.methods);
    else if (list) err += (err.empty() ? "" : "\n") + svc + ": service descriptor unavailable";
  }
  out.ok = !out.methods.empty() || (list && services.empty());
  out.error = err;
  if (list) warm_json(set, out.methods);
  publish(std::move(set), !list);
  out.load_ms = ms_between(t0, Clock::now());
  return out;
}

GrpcSchema GrpcClient::Impl::reflect(const GrpcTarget& t, long deadline_ms, const Metadata& md) {
  const uint64_t gen = generation();
  Clock::time_point t0;
  Lease l = begin_connect(t, t0);
  if (l.reused) return reflect(t, l.ch, t0, deadline_ms, md, {}, gen);
  ConnWatch w;
  w.ch = l.ch;
  w.cap = t0 + std::chrono::milliseconds(connect_limit_ms(t));
  w.probe_uri = l.uri;
  w.target_address = t.address;
  w.probe_at = t0 + std::chrono::milliseconds(kRefusedProbeMs);
  return reflect(t, l.ch, t0, deadline_ms, md, {}, gen, &w);
}

GrpcClient::Impl::Prepared GrpcClient::Impl::prepare(const GrpcTarget& t, const std::string& full_path,
                                                     const GrpcCallOptions& opts, Clock::time_point t0,
                                                     Clock::time_point deadline, uint64_t gen) {
  Prepared p;
  std::string svc, mname;
  if (!split_path(full_path, svc, mname)) {
    p.code = grpc::StatusCode::INVALID_ARGUMENT;
    p.error = "bad method path '" + full_path + "'";
    return p;
  }
  std::tie(p.schema, p.method) = lookup(t, svc, mname);
  if (p.method) return p;

  // First use of this service on this target: fetch only the file that defines the service. On a
  // channel that is not connected yet, the reflection request goes out with wait_for_ready right
  // away, so building and sending it overlaps the handshake. connect_ms runs until READY and
  // reflect_ms from there.
  Clock::time_point tc;
  p.lease = begin_connect(t, tc);
  p.have_lease = true;
  long budget = opts.deadline_ms > 0 ? std::min<long>(opts.deadline_ms, 10000) : 10000;
  budget = std::max<long>(1, budget - static_cast<long>(ms_between(t0, tc)));
  ConnWatch w;
  ConnWatch* watch = nullptr;
  if (!p.lease.reused) {
    w.ch = p.lease.ch;
    w.cap = std::min(deadline, tc + std::chrono::milliseconds(connect_limit_ms(t)));
    w.probe_uri = p.lease.uri;
    w.target_address = t.address;
    w.probe_at = tc + std::chrono::milliseconds(kRefusedProbeMs);
    watch = &w;
  }
  grpc::Status rst;
  Metadata reflect_md = opts.metadata;
  reflect_md.insert(reflect_md.end(), opts.reflect_metadata.begin(), opts.reflect_metadata.end());
  GrpcSchema s = reflect(t, p.lease.ch, tc, budget, reflect_md, {svc}, gen, watch, &rst);
  const grpc::StatusCode code = rst.error_code();
  const auto tr = Clock::now();
  if (watch) {
    const auto ready = w.ready() ? w.t_ready : tr;
    time_connect(t, p.lease, tc, ready);
    p.reflect_ms = ms_between(ready, tr);
  } else {
    p.reflect_ms = ms_between(tc, tr);
  }
  if (watch && w.failed) {
    // Never connected: the call fails the way it would have without reflection.
    p.code = code;
    p.error = rst.error_message();
    return p;
  }
  std::tie(p.schema, p.method) = lookup(t, svc, mname);
  if (!p.method) {
    // The method is unknown; a reflection failure that is about the server or the call itself
    // (unreachable, deadline, cancel, auth) keeps its own code.
    bool own = code == grpc::StatusCode::UNAVAILABLE || code == grpc::StatusCode::DEADLINE_EXCEEDED ||
               code == grpc::StatusCode::CANCELLED || code == grpc::StatusCode::UNAUTHENTICATED ||
               code == grpc::StatusCode::PERMISSION_DENIED;
    p.code = own ? code : grpc::StatusCode::NOT_FOUND;
    std::string why = s.ok ? "the server does not define it" : s.error;
    p.error = "method " + full_path + " not found: " + why;
  }
  return p;
}

// ================================================================== call engine

// One call, any kind, on a private CompletionQueue: at most one read and one write in flight.
// invoke() queues every message up front and drives the loop on its own thread; an interactive
// GrpcCall drives it on a dedicated thread while send()/close_send() feed the queue. All state is
// guarded by `m`; the loop releases it while waiting on the queue and around user callbacks.
struct GrpcCall::Core {
  std::shared_ptr<GrpcClient::Impl> impl;
  std::shared_ptr<SchemaSet> schema;
  const pb::MethodDescriptor* method = nullptr;
  std::string path;
  grpc::ClientContext ctx;
  GrpcMessageFn on_message;
  pb::util::JsonPrintOptions print = print_options(true);
  bool text_out = false;  // responses in protobuf text format
  bool text_single_line = false;
  Clock::time_point t0;
  GrpcResponse resp;
  std::unique_ptr<Encoder> encoder;
  // Reflection fetch for Any types the schema lacks (reflected schemas of this target only; unset
  // otherwise). any_fetch_ns: time spent in it (any thread), reported as reflect_ms: what ran
  // before the call started (any_fetch_pre_ms) comes out of queue_ms, the rest out of download_ms.
  AnyFetch any_fetch;
  std::atomic<int64_t> any_fetch_ns{0};
  double any_fetch_pre_ms = 0;

  mutable std::mutex m;
  std::condition_variable done_cv;
  std::deque<grpc::ByteBuffer> queue;  // encoded, not yet written
  grpc::ByteBuffer cur;                // the write in flight
  bool closed = false;                 // close_send(): no more messages
  bool started = false, write_pending = false, half_closed = false, read_eof = false, finish_issued = false;
  bool finished = false;
  int inflight = 0;

  grpc::CompletionQueue cq;
  std::unique_ptr<grpc::GenericClientAsyncReaderWriter> rw;
  grpc::Status status;
  grpc::ByteBuffer in;
  std::unique_ptr<pb::Message> rmsg;
  Clock::time_point t_start{}, t_first_write{}, t_sent{}, t_first_msg{}, t_end{};
  std::thread thread;

  // invoke() with a long message array: all but the first message are encoded on a feeder thread
  // while the first ones are already being written (encoding 1000 messages up front cost ~1.7 ms
  // before the first byte left). The first message is always encoded before the call starts, so
  // a request that does not match the schema at all still fails locally without any traffic.
  std::vector<std::string> pending_docs;
  size_t pending_next = 0;     // next doc to encode (feeder only)
  size_t pending_left = 0;     // docs not yet queued (guarded by m)
  std::string encode_error;    // a later message failed to encode: the call was cancelled
  std::string decode_failure;  // the first response message that could not be decoded (call: INTERNAL)

  void produce() {
    std::string err;
    std::vector<grpc::ByteBuffer> chunk;
    size_t i = pending_next, n = 8;  // small first chunk to get going, then larger ones
    while (i < pending_docs.size()) {
      chunk.clear();
      uint64_t bytes = 0;
      const size_t end = std::min(pending_docs.size(), i + n);
      n = std::min<size_t>(n * 2, 128);
      bool good = true;
      for (; i < end; ++i) {
        grpc::ByteBuffer bb;
        if (!encoder->encode(pending_docs[i], bb, err)) {
          good = false;
          break;
        }
        bytes += bb.Length() + 5;
        chunk.push_back(std::move(bb));
      }
      std::lock_guard lk(m);
      if (finished || read_eof) return;  // the call already ended
      for (auto& bb : chunk) queue.push_back(std::move(bb));
      resp.timing.bytes_sent += bytes;
      pending_left -= chunk.size();
      if (!good) {
        encode_error = "request message #" + std::to_string(i + 1) + ": " + err;
        pending_left = 0;
        closed = true;
        ctx.TryCancel();
        return;
      }
      if (pending_left == 0) closed = true;
      pump_writes();
    }
  }

  enum : intptr_t { kStart = 1, kWrite, kWritesDone, kRead, kFinish };
  static void* tag(intptr_t v) { return reinterpret_cast<void*>(v); }

  ~Core() {
    if (thread.joinable()) thread.join();
  }

  // m held. Issues the next write, the half-close, or nothing.
  // Writes with a buffer hint stay in the transport until a write without one. Hinting every write
  // of a long stream held the whole stream back until the end (the server sat idle meanwhile, then
  // had to catch up); flushing every message costs a syscall each. Flush every 64 messages / 16 KB.
  size_t unflushed_msgs = 0, unflushed_bytes = 0;
  bool hint_next() {
    ++unflushed_msgs;
    unflushed_bytes += cur.Length();
    if (unflushed_msgs < 64 && unflushed_bytes < 16384) return true;
    unflushed_msgs = unflushed_bytes = 0;
    return false;
  }

  void pump_writes() {
    if (!started || write_pending || half_closed || read_eof || finish_issued) return;
    if (!queue.empty()) {
      cur = std::move(queue.front());
      queue.pop_front();
      if (queue.empty() && closed) {
        rw->WriteLast(cur, grpc::WriteOptions(), tag(kWrite));  // message + END_STREAM together
        half_closed = true;
      } else if ((!queue.empty() || pending_left > 0) && hint_next()) {
        rw->Write(cur, grpc::WriteOptions().set_buffer_hint(), tag(kWrite));  // more behind it: don't flush
      } else {
        unflushed_msgs = unflushed_bytes = 0;
        rw->Write(cur, tag(kWrite));
      }
    } else if (closed) {
      rw->WritesDone(tag(kWritesDone));
      half_closed = true;
    } else {
      return;
    }
    write_pending = true;
    ++inflight;
  }

  void maybe_finish() {
    if (read_eof && !write_pending && !finish_issued) {
      rw->Finish(&status, tag(kFinish));
      finish_issued = true;
      ++inflight;
    }
  }

  bool enqueue(const std::string& json, std::string* error) {
    std::unique_lock lk(m);
    auto fail = [&](std::string e) {
      if (error) *error = std::move(e);
      return false;
    };
    if (finished || finish_issued || read_eof) return fail("the call has ended");
    if (closed) return fail("the send side is closed");
    if (!method->client_streaming() && (!queue.empty() || t_first_write != Clock::time_point{} || write_pending))
      return fail("this method takes a single request message");
    grpc::ByteBuffer bb;
    std::string err;
    if (!encoder->encode(json, bb, err)) return fail("request: " + err);
    resp.timing.bytes_sent += bb.Length() + 5;
    queue.push_back(std::move(bb));
    if (!method->client_streaming()) closed = true;  // unary/server-stream: one message, then half-close
    pump_writes();
    return true;
  }

  void close_send() {
    std::lock_guard lk(m);
    closed = true;
    pump_writes();
  }

  void run() {
    std::unique_lock lk(m);
    t_start = Clock::now();
    apply_call_authority(lease_channel, ctx);
    rw = grpc::GenericStub(lease_channel).PrepareCall(&ctx, path, &cq);
    rmsg.reset(schema->prototype(method->output_type())->New());
    rw->StartCall(tag(kStart));
    ++inflight;
    void* got;
    bool ok;
    while (inflight > 0) {
      lk.unlock();
      bool alive = cq.Next(&got, &ok);
      lk.lock();
      if (!alive) break;
      --inflight;
      const auto now = Clock::now();
      switch (reinterpret_cast<intptr_t>(got)) {
        case kStart:
          started = true;
          if (!ok) {
            read_eof = true;
            break;
          }
          pump_writes();
          rw->Read(&in, tag(kRead));
          ++inflight;
          break;
        case kWrite:
          write_pending = false;
          if (t_first_write == Clock::time_point{}) t_first_write = now;
          if (!ok) {
            half_closed = true;  // the stream is broken; the status will say why
            break;
          }
          if (half_closed) t_sent = now;
          pump_writes();
          break;
        case kWritesDone:
          write_pending = false;
          t_sent = now;
          break;
        case kRead: {
          if (!ok) {
            read_eof = true;
            break;
          }
          if (t_first_msg == Clock::time_point{}) t_first_msg = now;
          grpc::ByteBuffer msg_buf;
          msg_buf.Swap(&in);
          rw->Read(&in, tag(kRead));  // keep the next read in flight while this one is decoded
          ++inflight;
          GrpcMessageEvent ev;
          ev.at_ms = ms_between(t0, now);
          ev.wire_bytes = msg_buf.Length() + 5;
          resp.timing.bytes_received += ev.wire_bytes;
          decode(msg_buf, ev);
          if (on_message) {
            lk.unlock();  // the callback may send(), cancel() or take its time
            on_message(ev);
            lk.lock();
          }
          resp.messages.push_back(std::move(ev));
          break;
        }
        case kFinish:
          t_end = now;
          break;
      }
      maybe_finish();
    }
    if (t_end == Clock::time_point{}) t_end = Clock::now();
    complete();
    rw.reset();
    lk.unlock();
    cq.Shutdown();
    while (cq.Next(&got, &ok)) {
    }
    lk.lock();
    finished = true;
    done_cv.notify_all();
  }

  // m held. Decodes one received message into ev. The strict parse is the hot path; when it fails
  // (typically invalid UTF-8 in a proto3 string, which C++ servers send with only a log line), a
  // lenient pass repairs those strings with U+FFFD and parses again, noting each repaired field,
  // as grpcurl prints such messages. A message that still cannot be decoded gets the parser's
  // error and its raw bytes, never a made-up body, and the call reports INTERNAL as grpc-go does.
  void decode(grpc::ByteBuffer& buf, GrpcMessageEvent& ev) {
    rmsg->Clear();
    bool parsed = false;
    {
      grpc::ProtoBufferReader reader(&buf);
      parsed = reader.status().ok() && rmsg->ParseFromZeroCopyStream(&reader);
    }
    if (!parsed) {
      std::string wire;
      std::vector<grpc::Slice> slices;
      if (buf.Dump(&slices).ok()) {
        wire.reserve(buf.Length());
        for (const auto& sl : slices) wire.append(reinterpret_cast<const char*>(sl.begin()), sl.size());
      }
      auto fix = detail::repair_utf8(wire, rmsg->GetDescriptor());
      rmsg->Clear();
      if (fix.ok && rmsg->ParsePartialFromString(fix.bytes)) {
        ev.notes = std::move(fix.notes);
        if (!rmsg->IsInitialized()) ev.notes.push_back("missing required fields: " + rmsg->InitializationErrorString());
        if (ev.notes.empty()) ev.notes.push_back("the strict parser rejected this message; decoded leniently");
      } else {
        const std::string type(rmsg->GetDescriptor()->full_name());
        ev.decode_error = fix.ok ? "the protobuf parser rejected the bytes as " + type
                                 : "malformed " + type + ": " + fix.error;
        ev.raw_hex = hex(wire);
        if (decode_failure.empty())
          decode_failure = "grpc: failed to unmarshal response message #" + std::to_string(resp.messages.size() + 1) +
                           ": " + ev.decode_error;
        return;
      }
    }
    if (text_out) {
      pb::TextFormat::Printer tp;
      tp.SetSingleLineMode(text_single_line);
      tp.PrintToString(*rmsg, &ev.json);
      while (!ev.json.empty() && (ev.json.back() == '\n' || ev.json.back() == ' ')) ev.json.pop_back();
      return;
    }
    std::string js;
    bool final_form = false;
    BigFields big;
    if (ev.wire_bytes >= kBigFieldBytes) big.extract(*rmsg);
    auto jst = pb::util::MessageToJsonString(*rmsg, &js, print);
    if (!jst.ok() && !missing_any_url(jst.message()).empty()) {
      // An Any of a type the schema lacks: resolve it (linked-in types, reflection) or print it as
      // grpcurl does; the server's status stands either way.
      jst = print_json_resolving_any(*schema, *rmsg, print, any_fetch ? &any_fetch : nullptr, js, &ev.notes,
                                     &final_form);
    }
    if (!jst.ok()) {
      big.restore();
      ev.decode_error = "cannot print as JSON: " + std::string(jst.message());
      ev.raw_hex = hex(rmsg->SerializeAsString());
      if (decode_failure.empty())
        decode_failure = "response message #" + std::to_string(resp.messages.size() + 1) + ": " + ev.decode_error;
      return;
    }
    while (!js.empty() && (js.back() == '\n' || js.back() == ' ')) js.pop_back();
    if (!final_form) js = widen_indent(js);
    ev.json = big.empty() ? std::move(js) : big.splice(js);
  }

  // m held. Fills status, metadata and timing.
  void complete() {
    if (!encode_error.empty()) status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, encode_error);
    else if (!decode_failure.empty() && status.ok()) status = grpc::Status(grpc::StatusCode::INTERNAL, decode_failure);
    resp.ok = status.ok();
    resp.status_code = static_cast<int>(status.error_code());
    resp.status_name = status_name(resp.status_code);
    resp.status_message = status.error_message();
    resp.error_details = decode_status_details(status.error_details(), schema.get(), any_fetch ? &any_fetch : nullptr);
    resp.headers = convert_metadata(ctx.GetServerInitialMetadata());
    resp.trailers = convert_metadata(ctx.GetServerTrailingMetadata());

    Timing& tm = resp.timing;
    const double any_fetch_post_ms = std::max(0.0, double(any_fetch_ns.load()) / 1e6 - any_fetch_pre_ms);
    tm.queue_ms = std::max(0.0, ms_between(t0, t_start) - tm.dns_ms - tm.connect_ms - tm.tls_ms - resp.reflect_ms -
                                    any_fetch_pre_ms);
    if (t_sent == Clock::time_point{}) t_sent = t_first_write != Clock::time_point{} ? t_first_write : t_start;
    tm.send_ms = ms_between(t_start, t_sent);
    if (t_first_msg != Clock::time_point{}) {
      auto base = t_sent <= t_first_msg ? t_sent : (t_first_write != Clock::time_point{} ? t_first_write : t_start);
      tm.ttfb_ms = std::max(0.0, ms_between(base, t_first_msg));
      tm.download_ms = std::max(0.0, ms_between(t_first_msg, t_end) - any_fetch_post_ms);
    } else {
      tm.ttfb_ms = std::max(0.0, ms_between(t_sent, t_end));  // waited for status only
    }
    resp.reflect_ms += any_fetch_pre_ms + any_fetch_post_ms;
    tm.total_ms = ms_between(t0, t_end);
  }

  std::shared_ptr<grpc::Channel> lease_channel;
};

// ================================================================== public API

GrpcClient::GrpcClient() : impl_(std::make_shared<Impl>()) {}
GrpcClient::~GrpcClient() { cancel(); }

void GrpcClient::preconnect(const GrpcTarget& target) { impl_->preconnect(target); }

GrpcSchema GrpcClient::load_via_reflection(const GrpcTarget& target, long deadline_ms, const Metadata& metadata) {
  return impl_->reflect(target, deadline_ms, metadata);
}

namespace {
bool is_protoset_path(const std::string& p) {
  std::string l = p;
  std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  for (const char* ext : {".protoset", ".pb", ".desc", ".binpb", ".fds"}) {
    size_t n = std::char_traits<char>::length(ext);
    if (l.size() > n && l.compare(l.size() - n, n, ext) == 0) return true;
  }
  return false;
}
std::filesystem::path u8path(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }
}  // namespace

namespace {
// Finds a service, method, message or enum in the loaded schemas, newest first, and returns the
// file that defines it plus its definition as .proto source.
const pb::FileDescriptor* find_symbol(const std::vector<std::shared_ptr<SchemaSet>>& schemas, std::string sym,
                                      std::string* definition) {
  while (!sym.empty() && sym.front() == '/') sym.erase(0, 1);
  if (auto slash = sym.find('/'); slash != std::string::npos) sym[slash] = '.';
  while (!sym.empty() && sym.front() == '.') sym.erase(0, 1);
  pb::DebugStringOptions o;
  o.include_comments = true;
  for (auto it = schemas.rbegin(); it != schemas.rend(); ++it) {
    const auto* pool = (*it)->pool;
    if (const auto* d = pool->FindServiceByName(sym)) {
      if (definition) *definition = d->DebugStringWithOptions(o);
      return d->file();
    }
    if (const auto* d = pool->FindMethodByName(sym)) {
      if (definition) *definition = d->DebugStringWithOptions(o);
      return d->service()->file();
    }
    if (const auto* d = pool->FindMessageTypeByName(sym)) {
      if (definition) *definition = d->DebugStringWithOptions(o);
      return d->file();
    }
    if (const auto* d = pool->FindEnumTypeByName(sym)) {
      if (definition) *definition = d->DebugStringWithOptions(o);
      return d->file();
    }
  }
  return nullptr;
}
}  // namespace

std::string GrpcClient::example_request(const std::string& full_path, std::vector<std::string>* notes) const {
  std::string svc, mname;
  if (!split_path(full_path, svc, mname)) return {};
  std::lock_guard lk(impl_->mu);
  for (auto it = impl_->schemas.rbegin(); it != impl_->schemas.rend(); ++it)
    if (const auto* m = find_method((*it)->pool, svc, mname))
      return grpc_detail::build_request_template(m->input_type(), notes, /*examples=*/true);
  return {};
}

std::string GrpcClient::describe(const std::string& symbol) const {
  std::lock_guard lk(impl_->mu);
  std::string out;
  find_symbol(impl_->schemas, symbol, &out);
  while (!out.empty() && out.back() == '\n') out.pop_back();
  return out;
}

bool GrpcClient::export_protoset(const std::string& path, const std::vector<std::string>& symbols,
                                 std::string* error) const {
  auto fail = [&](std::string e) {
    if (error) *error = std::move(e);
    return false;
  };
  pb::FileDescriptorSet set;
  {
    std::lock_guard lk(impl_->mu);
    std::set<std::string> seen;
    std::function<void(const pb::FileDescriptor*)> add = [&](const pb::FileDescriptor* f) {
      if (!seen.insert(std::string(f->name())).second) return;
      for (int i = 0; i < f->dependency_count(); ++i) add(f->dependency(i));
      f->CopyTo(set.add_file());
      f->CopySourceCodeInfoTo(set.mutable_file(set.file_size() - 1));
    };
    if (symbols.empty()) {
      for (const auto& s : impl_->schemas)
        for (const auto& name : s->roots)
          if (const auto* f = s->pool->FindFileByName(name)) add(f);
    }
    for (const auto& sym : symbols) {
      const auto* f = find_symbol(impl_->schemas, sym, nullptr);
      if (!f) return fail("unknown symbol '" + sym + "'");
      add(f);
    }
  }
  if (set.file_size() == 0) return fail("no schema loaded");
  std::ofstream out(u8path(path), std::ios::binary | std::ios::trunc);
  if (!out || !set.SerializeToOstream(&out)) return fail(path + ": cannot write");
  return true;
}

bool GrpcClient::export_proto_files(const std::string& dir, const std::vector<std::string>& symbols,
                                    std::string* error) const {
  auto fail = [&](std::string e) {
    if (error) *error = std::move(e);
    return false;
  };
  std::vector<std::pair<std::string, std::string>> files;  // relative path -> .proto source
  {
    std::lock_guard lk(impl_->mu);
    std::set<std::string> seen;
    std::function<void(const pb::FileDescriptor*)> add = [&](const pb::FileDescriptor* f) {
      if (!seen.insert(std::string(f->name())).second) return;
      for (int i = 0; i < f->dependency_count(); ++i) add(f->dependency(i));
      files.emplace_back(std::string(f->name()), f->DebugString());
    };
    if (symbols.empty()) {
      for (const auto& s : impl_->schemas)
        for (const auto& name : s->roots)
          if (const auto* f = s->pool->FindFileByName(name)) add(f);
    }
    for (const auto& sym : symbols) {
      const auto* f = find_symbol(impl_->schemas, sym, nullptr);
      if (!f) return fail("unknown symbol '" + sym + "'");
      add(f);
    }
  }
  if (files.empty()) return fail("no schema loaded");
  // File names come from the schema source (a server, for reflection): only plain relative paths
  // below dir are written.
  auto safe = [](const std::string& name) {
    if (name.empty() || name.front() == '/') return false;
    size_t i = 0;
    while (i <= name.size()) {
      size_t j = name.find('/', i);
      if (j == std::string::npos) j = name.size();
      std::string_view part(name.data() + i, j - i);
      if (part.empty() || part == "." || part == ".." || part.find_first_of(":\\") != std::string_view::npos)
        return false;
      i = j + 1;
    }
    return true;
  };
  namespace fs = std::filesystem;
  const fs::path root = u8path(dir);
  for (const auto& [name, src] : files) {
    if (!safe(name)) return fail("refusing to write unsafe file name '" + name + "'");
    const fs::path out_path = root / u8path(name);
    std::error_code ec;
    fs::create_directories(out_path.parent_path(), ec);
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out || !(out << src)) return fail(name + ": cannot write");
  }
  return true;
}

GrpcSchema GrpcClient::load_protoset(const std::vector<std::string>& protoset_paths) {
  GrpcSchema out;
  auto t0 = Clock::now();
  std::map<std::string, pb::FileDescriptorProto> files;
  std::vector<std::string> order;  // file names in the order the sets list them
  std::string err;
  for (const auto& p : protoset_paths) {
    // A descriptor set is read straight from disk: only absolute paths, never relative to the
    // process cwd (callers pass Workspace::schema_plan() paths, already anchored and checked).
    if (!u8path(p).is_absolute()) {
      err += (err.empty() ? "" : "\n") + p + ": a descriptor set path must be absolute";
      continue;
    }
    std::ifstream f(u8path(p), std::ios::binary);
    if (!f) {
      err += (err.empty() ? "" : "\n") + p + ": cannot open";
      continue;
    }
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    pb::FileDescriptorSet fds;
    if (!fds.ParseFromString(bytes) || fds.file_size() == 0) {
      err += (err.empty() ? "" : "\n") + p + ": not a FileDescriptorSet (protoc --descriptor_set_out)";
      continue;
    }
    for (auto& fdp : *fds.mutable_file()) {
      if (files.count(fdp.name())) continue;
      order.push_back(fdp.name());
      files.emplace(fdp.name(), std::move(fdp));
    }
  }
  if (files.empty()) {
    out.error = err.empty() ? "no protoset files given" : err;
    out.load_ms = ms_between(t0, Clock::now());
    return out;
  }
  auto set = std::make_shared<SchemaSet>();
  build_pool(*set, std::move(files), err);
  set->roots = order;
  for (const auto& name : order) {
    const auto* fd = set->pool->FindFileByName(name);
    if (!fd) continue;
    for (int i = 0; i < fd->service_count(); ++i) append_methods(fd->service(i), out.methods);
  }
  out.ok = err.empty();
  out.error = err;
  impl_->warm_json(set, out.methods);
  impl_->publish(std::move(set));
  out.load_ms = ms_between(t0, Clock::now());
  return out;
}

GrpcSchema GrpcClient::load_proto_files(const std::vector<std::string>& proto_paths,
                                        const std::vector<std::string>& import_dirs) {
  namespace fs = std::filesystem;
  if (!proto_paths.empty() && std::all_of(proto_paths.begin(), proto_paths.end(), is_protoset_path))
    return load_protoset(proto_paths);
  GrpcSchema out;
  auto t0 = Clock::now();
  if (std::any_of(proto_paths.begin(), proto_paths.end(), is_protoset_path)) {
    out.error = "load .protoset files and .proto sources separately";
    return out;
  }
  auto set = std::make_shared<SchemaSet>();
  set->tree = std::make_unique<WattSourceTree>();
  // Paths are UTF-8 strings in the API and in protobuf's DiskSourceTree.
  auto utf8 = [](const fs::path& p) {
    std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
  };
  auto generic = [&](const fs::path& p) {
    std::error_code ec;
    fs::path a = fs::absolute(p, ec);
    return utf8((ec ? p : a).lexically_normal());
  };
  for (const auto& d : import_dirs)
    if (!d.empty()) set->tree->disk.MapPath("", generic(u8path(d)));

  std::vector<std::string> virtuals;
  std::string err;
  for (const auto& p : proto_paths) {
    std::error_code ec;
    // Only an absolute path is a disk file. A relative one is a virtual name looked up in the
    // import dirs, never against the process cwd (callers pass Workspace::schema_plan() paths,
    // which anchor workspace-relative files at the workspace root and check them first).
    if (u8path(p).is_absolute() && fs::is_regular_file(u8path(p), ec)) {
      std::string disk = generic(u8path(p)), virt, shadow;
      auto r = set->tree->disk.DiskFileToVirtualFile(disk, &virt, &shadow);
      if (r == pb::compiler::DiskSourceTree::NO_MAPPING) {
        set->tree->disk.MapPath("", utf8(u8path(disk).parent_path()));
        r = set->tree->disk.DiskFileToVirtualFile(disk, &virt, &shadow);
      }
      if (r == pb::compiler::DiskSourceTree::SUCCESS || r == pb::compiler::DiskSourceTree::SHADOWED) {
        virtuals.push_back(virt);
        continue;
      }
      err += (err.empty() ? "" : "\n") + p + ": cannot map to an import path";
    } else {
      virtuals.push_back(utf8(u8path(p)));  // relative to the import dirs
    }
  }

  set->import_errors = std::make_unique<ImportErrors>();
  set->importer = std::make_unique<pb::compiler::Importer>(set->tree.get(), set->import_errors.get());
  std::vector<const pb::FileDescriptor*> loaded;
  for (const auto& v : virtuals) {
    if (const auto* fd = set->importer->Import(v)) loaded.push_back(fd);
    else if (set->import_errors->text.empty()) set->import_errors->text = v + ": import failed";
  }
  if (!set->import_errors->text.empty()) err += (err.empty() ? "" : "\n") + set->import_errors->text;
  set->pool = set->importer->pool();
  set->init_factory();
  std::set<std::string> seen;
  for (const auto* fd : loaded) {
    if (!seen.insert(std::string(fd->name())).second) continue;
    set->roots.emplace_back(fd->name());
    for (int i = 0; i < fd->service_count(); ++i) append_methods(fd->service(i), out.methods);
  }
  out.ok = err.empty() && !loaded.empty();
  if (loaded.empty() && err.empty()) err = "no proto files given";
  out.error = err;
  if (!loaded.empty()) {
    impl_->warm_json(set, out.methods);
    impl_->publish(std::move(set));
  }
  out.load_ms = ms_between(t0, Clock::now());
  return out;
}

void GrpcClient::cancel() {
  std::lock_guard lk(impl_->call_mu);
  ++impl_->cancel_gen;
  for (auto* c : impl_->active) c->TryCancel();
}

namespace {
// Shared front half of invoke() and start_call(): resolve, connect, build the core.
// Returns null and fills resp on a local failure.
std::shared_ptr<GrpcCall::Core> make_core(const std::shared_ptr<GrpcClient::Impl>& impl, const GrpcTarget& target,
                                          const std::string& full_path, const GrpcCallOptions& opts,
                                          const GrpcMessageFn& on_message, Clock::time_point t0, uint64_t gen,
                                          const std::function<bool(GrpcCall::Core&, std::string&)>& fill,
                                          GrpcResponse& resp) {
  auto local_fail = [&](grpc::StatusCode code, std::string msg) {
    resp.status_code = static_cast<int>(code);
    resp.status_name = status_name(resp.status_code);
    resp.status_message = std::move(msg);
    resp.timing.total_ms = ms_between(t0, Clock::now());
    return nullptr;
  };
  const auto deadline =
      opts.deadline_ms > 0 ? t0 + std::chrono::milliseconds(opts.deadline_ms) : Clock::time_point::max();
  auto p = impl->prepare(target, full_path, opts, t0, deadline, gen);
  resp.reflect_ms = p.reflect_ms;
  if (p.have_lease) {
    resp.timing.dns_ms = p.lease.dns_ms;
    resp.timing.connect_ms = p.lease.connect_ms;
    resp.timing.tls_ms = p.lease.tls_ms;
    resp.timing.connection_reused = p.lease.reused;
  }
  if (!p.method) return local_fail(p.code, p.error);

  auto core = std::make_shared<GrpcCall::Core>();
  core->impl = impl;
  core->schema = p.schema;
  core->method = p.method;
  core->path = "/" + std::string(p.method->service()->full_name()) + "/" + std::string(p.method->name());
  core->on_message = on_message;
  core->t0 = t0;
  core->print.preserve_proto_field_names = opts.proto_field_names;
  core->print.always_print_fields_with_no_presence = opts.emit_defaults;
  core->print.add_whitespace = !opts.compact;
  core->text_out = opts.format == GrpcFormat::Text;
  core->text_single_line = opts.compact;
  core->encoder = std::make_unique<Encoder>(p.schema->prototype(p.method->input_type()), opts.allow_unknown_fields,
                                            core->text_out);
  if (p.schema->from_reflection && p.schema->target == target) {
    Metadata md = opts.metadata;
    md.insert(md.end(), opts.reflect_metadata.begin(), opts.reflect_metadata.end());
    std::weak_ptr<GrpcClient::Impl> weak = impl;
    std::atomic<int64_t>* spent = &core->any_fetch_ns;  // the core owns the fetch and outlives it
    core->any_fetch = [weak, target, md = std::move(md), spent](const std::string& symbol,
                                                                std::map<std::string, pb::FileDescriptorProto>& files) {
      auto i = weak.lock();
      if (!i) return false;
      const auto f0 = Clock::now();
      const bool ok = i->fetch_symbol(target, md, symbol, files);
      *spent += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - f0).count();
      return ok;
    };
  }
  {
    SchemaSet* set = p.schema.get();
    const AnyFetch* fetch = core->any_fetch ? &core->any_fetch : nullptr;
    core->encoder->resolve = [set, fetch](const std::string& url) { return resolve_any(*set, url, fetch) != nullptr; };
  }
  core->resp.reflect_ms = p.reflect_ms;
  core->resp.timing = resp.timing;

  std::string err;
  const bool filled = fill(*core, err);
  core->any_fetch_pre_ms = double(core->any_fetch_ns.load()) / 1e6;  // a reflection fetch, not queueing
  if (!filled) {
    resp.reflect_ms += core->any_fetch_pre_ms;
    return local_fail(grpc::StatusCode::INVALID_ARGUMENT, err);
  }
  if (!apply_metadata(core->ctx, opts.metadata, err) || !apply_metadata(core->ctx, opts.rpc_metadata, err))
    return local_fail(grpc::StatusCode::INVALID_ARGUMENT, err);
  if (opts.deadline_ms > 0)
    core->ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(opts.deadline_ms) -
                           std::chrono::duration_cast<std::chrono::system_clock::duration>(Clock::now() - t0));

  if (!p.have_lease) {
    auto lease = impl->connect(target, deadline, gen);
    core->resp.timing.dns_ms = lease.dns_ms;
    core->resp.timing.connect_ms = lease.connect_ms;
    core->resp.timing.tls_ms = lease.tls_ms;
    core->resp.timing.connection_reused = lease.reused;
    core->lease_channel = lease.ch;
    if (lease.refused) {
      resp.timing = core->resp.timing;
      return local_fail(grpc::StatusCode::UNAVAILABLE, refused_message(target.address));
    }
    if (lease.timed_out && !impl->cancelled_since(gen)) {
      resp.timing = core->resp.timing;
      return local_fail(grpc::StatusCode::DEADLINE_EXCEEDED,
                        "timed out after " + std::to_string(static_cast<long>(lease.connect_ms)) +
                            " ms connecting to " + target.address);
    }
  } else {
    core->lease_channel = p.lease.ch;
  }
  return core;
}
}  // namespace

namespace {
// Splits a long JSON message array on its own thread, started before the method is resolved so
// the split overlaps connect + reflection; once the call exists it encodes the rest of the array
// into the call's send queue (GrpcCall::Core::produce).
class Feeder {
 public:
  explicit Feeder(const std::string& body) : body_(body) {
    th_ = std::thread([this] { run(); });
  }
  ~Feeder() { join(); }
  // Blocks until the body is split; false with err when it is not valid JSON.
  bool docs(std::vector<std::string>& out, std::string& err) {
    std::unique_lock lk(mu_);
    cv_.wait(lk, [&] { return split_done_; });
    out = std::move(docs_);
    err = err_;
    return ok_;
  }
  void launch(GrpcCall::Core* core) {
    std::lock_guard lk(mu_);
    core_ = core;
    cv_.notify_all();
  }
  void join() {
    {
      std::lock_guard lk(mu_);
      stop_ = true;
      cv_.notify_all();
    }
    if (th_.joinable()) th_.join();
  }

 private:
  void run() {
    std::vector<std::string> d;
    std::string err;
    bool ok = split_request(body_, true, d, err);
    std::unique_lock lk(mu_);
    docs_ = std::move(d);
    err_ = std::move(err);
    ok_ = ok;
    split_done_ = true;
    cv_.notify_all();
    cv_.wait(lk, [&] { return core_ || stop_; });
    if (!core_) return;
    lk.unlock();
    core_->produce();
  }
  const std::string& body_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool split_done_ = false, ok_ = false, stop_ = false;
  std::vector<std::string> docs_;
  std::string err_;
  GrpcCall::Core* core_ = nullptr;
  std::thread th_;
};

bool is_long_array(const std::string& body) {
  constexpr size_t kFeedFrom = 2048;  // below this, splitting + encoding inline beats a thread
  if (body.size() < kFeedFrom) return false;
  size_t i = body.find_first_not_of(" \t\r\n");
  return i != std::string::npos && body[i] == '[';
}
}  // namespace

GrpcResponse GrpcClient::invoke(const GrpcTarget& target, const std::string& full_path,
                                const std::string& request_json, const GrpcCallOptions& opts,
                                const GrpcMessageFn& on_message) {
  GrpcResponse resp;
  const auto t0 = Clock::now();
  const uint64_t gen = impl_->generation();
  std::unique_ptr<Feeder> feeder;
  const bool text = opts.format == GrpcFormat::Text;
  if (!text && is_long_array(request_json)) feeder = std::make_unique<Feeder>(request_json);
  auto fill = [&](GrpcCall::Core& c, std::string& err) {
    std::vector<std::string> docs;
    if (feeder) {
      if (!feeder->docs(docs, err)) return false;
      if (!c.method->client_streaming() && docs.size() != 1) {
        err = "this method takes a single request message: send a JSON object, not an array";
        return false;
      }
    } else if (!(text ? split_text_request(request_json, c.method->client_streaming(), docs, err)
                      : split_request(request_json, c.method->client_streaming(), docs, err))) {
      return false;
    }
    const size_t eager = feeder ? 1 : docs.size();
    for (size_t i = 0; i < eager && i < docs.size(); ++i) {
      grpc::ByteBuffer bb;
      if (!c.encoder->encode(docs[i], bb, err)) {
        err = (docs.size() > 1 ? "request message #" + std::to_string(i + 1) : std::string("request")) + ": " + err;
        return false;
      }
      c.resp.timing.bytes_sent += bb.Length() + 5;  // 5-byte gRPC length prefix per message
      c.queue.push_back(std::move(bb));
    }
    if (eager < docs.size()) {
      c.pending_next = eager;
      c.pending_left = docs.size() - eager;
      c.pending_docs = std::move(docs);
    } else {
      c.closed = true;
    }
    return true;
  };
  auto core = make_core(impl_, target, full_path, opts, on_message, t0, gen, fill, resp);
  if (!core) return resp;
  {
    Impl::Active reg(*impl_, &core->ctx, gen);
    if (core->pending_left > 0) feeder->launch(core.get());
    core->run();
    if (feeder) feeder->join();  // before the core goes away
  }
  return std::move(core->resp);
}

std::unique_ptr<GrpcCall> GrpcClient::start_call(const GrpcTarget& target, const std::string& full_path,
                                                 const GrpcCallOptions& opts, const GrpcMessageFn& on_message) {
  std::unique_ptr<GrpcCall> call(new GrpcCall());
  const auto t0 = Clock::now();
  const uint64_t gen = impl_->generation();
  GrpcResponse resp;
  auto core = make_core(impl_, target, full_path, opts, on_message, t0, gen,
                        [](GrpcCall::Core&, std::string&) { return true; }, resp);
  if (!core) {
    core = std::make_shared<GrpcCall::Core>();
    core->resp = std::move(resp);
    core->finished = true;
    core->closed = true;
    call->core_ = core;
    return call;
  }
  call->core_ = core;
  GrpcCall::Core* c = core.get();
  core->thread = std::thread([c, gen] {
    GrpcClient::Impl::Active reg(*c->impl, &c->ctx, gen);
    c->run();
  });
  return call;
}

// ================================================================== GrpcCall

GrpcCall::~GrpcCall() {
  if (!core_) return;
  if (!done()) cancel();
  if (core_->thread.joinable()) core_->thread.join();
}

bool GrpcCall::send(const std::string& json, std::string* error) {
  if (!core_->method) {
    if (error) *error = "the call has ended";
    return false;
  }
  return core_->enqueue(json, error);
}

void GrpcCall::close_send() {
  if (core_->method) core_->close_send();
}

void GrpcCall::cancel() {
  if (core_->method) core_->ctx.TryCancel();
}

bool GrpcCall::done() const {
  std::lock_guard lk(core_->m);
  return core_->finished;
}

GrpcResponse GrpcCall::wait() {
  {
    std::unique_lock lk(core_->m);
    core_->done_cv.wait(lk, [&] { return core_->finished; });
  }
  if (core_->thread.joinable()) core_->thread.join();
  std::lock_guard lk(core_->m);
  return core_->resp;
}

}  // namespace watt
