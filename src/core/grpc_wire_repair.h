#pragma once
// Lenient decoding for response messages the strict protobuf parser rejects. The usual cause is a
// proto3 string field holding bytes that are not valid UTF-8 (C++ servers only log an error when
// they send that); grpcurl still prints such messages, with the bad bytes as U+FFFD. This walks the
// wire format with the message Descriptor, rewrites invalid UTF-8 in every string field (nested
// messages, repeated fields and map keys / values too) and records which fields it changed.
#include <string>
#include <string_view>
#include <vector>

namespace google::protobuf {
class Descriptor;
}

namespace watt::detail {

struct WireRepair {
  bool ok = false;                 // bytes is a well-formed message of the given type
  std::string bytes;               // the message with every string field valid UTF-8
  std::vector<std::string> notes;  // "metadata[x-data-bin]: invalid UTF-8 replaced with U+FFFD", ...
  std::string error;               // !ok: why the wire data is malformed, with the byte offset
};

WireRepair repair_utf8(std::string_view wire, const google::protobuf::Descriptor* type);

// Replaces each byte that does not start a valid UTF-8 sequence with U+FFFD (as Go does, so the
// output matches grpcurl). Returns false when `in` was already valid (out is then untouched).
bool fix_utf8(std::string_view in, std::string& out);

}  // namespace watt::detail
