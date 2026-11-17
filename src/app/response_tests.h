#pragma once
// Helpers of the response view's Tests tab (the assertions themselves are evaluated by
// core/asserts.h). Pure functions, no wx: they run on worker threads.
//
// A JSONPath-style assert target ("res.body.items[1].id", gRPC "res.messages[2].name") is turned
// into a concrete path so a failing row can reveal its value in the Body view: json_locate()
// walks the JSON text (pretty or raw) to that value without building a tree.
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace watt::app::rv {

// One step from the root to a value: a member name or an array index (negative: from the end).
struct PathStep {
  std::string key;
  long index = 0;
  bool is_index = false;
  bool operator==(const PathStep&) const = default;
};

// "res.body.items[1].id" -> items, [1], id. gRPC "res.messages[2].x" -> *message = 2 and the
// path inside that message; "res.body..." on gRPC is message 0. False when the target is not a
// path into the body (status, headers, time ...). *message is -1 for an HTTP body.
bool target_json_path(std::string_view target, std::vector<PathStep>& path, long* message);

// Byte offset of the value at `path` in a JSON text, npos when the path is not there (then
// *deepest_at, if given, is the offset of the deepest step that exists). *len gets the value's
// length. Skips everything else at memory speed.
size_t json_locate(std::string_view text, const std::vector<PathStep>& path, size_t* len = nullptr,
                   size_t* deepest_at = nullptr);

// "$.items[3].name" for a path (for tooltips).
std::string path_string(const std::vector<PathStep>& path);

// Why `path` is not in the JSON `text`, in words: which part exists and what is there instead
// ("res.body is an array, not an object: index it first, e.g. res.body[0].items"). Empty when the
// path is there or the text is not JSON. root names the text ("res.body").
std::string json_path_miss(std::string_view text, const std::vector<PathStep>& path,
                           const std::string& root = "res.body");

// Why a capture may not store into variable `name` ("" when it may): the same rules as core
// apply_captures (length, no braces, no control characters, no outer spaces).
std::string capture_var_problem(std::string_view name, size_t max_len);

}  // namespace watt::app::rv
