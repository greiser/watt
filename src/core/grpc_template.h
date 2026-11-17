#pragma once
// Internal to the gRPC engine: builds the editable JSON skeleton for a request message.
#include <string>
#include <vector>

namespace google::protobuf {
class Descriptor;
}

namespace watt::grpc_detail {

// Pretty JSON (2-space indent) with every field present, in proto3 JSON mapping:
// nested messages filled recursively (a recursive type is expanded one more level, like grpcurl:
// its own self-reference is then null / []), repeated fields with one element, maps with one
// entry, enums as the first value name, the first member of each oneof, well-known types in
// their JSON form. The result always parses back with JsonStringToMessage.
// notes (optional) receives what JSON cannot say: each oneof with its alternatives, and
// Struct / Value / ListValue / Any fields that take arbitrary JSON.
// examples: plausible values instead of zero values (BloomRPC / Postman style), so the request
// means something unedited: "Hello" or a value guessed from the field name (a UUID for ids, an
// address for email / url fields, ...), 10, 1.5, true, the first enum value after an UNSPECIFIED
// zero, 2024-01-01T12:00:00Z, 1.5s.
std::string build_request_template(const google::protobuf::Descriptor* d,
                                   std::vector<std::string>* notes = nullptr, bool examples = false);

}  // namespace watt::grpc_detail
