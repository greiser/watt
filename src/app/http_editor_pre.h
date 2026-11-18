#pragma once
// Pre-request variables of the HTTP editor (Bruno's vars:pre-request, Postman's
// pm.variables.set in a pre-request script, without a script engine). Each row
// "name = expression" is evaluated in order, off the UI thread, right before the request is
// sent; its value is then a {{name}} like any other (and visible to the rows after it). No wx.
//
// Expressions: terms joined with '+', as in JavaScript: number + number adds ("p-" + (1+1) is
// "p-2", now() + 60000 a time), anything with a string concatenates ({{vars}} are strings).
//   'text' "text"       string literal (\n \t \r \\ \' \" escapes); {{vars}} inside are expanded
//   123  -1.5           number (kept as written)
//   {{name}}            a variable (runtime, environment, globals, dynamic {{$timestamp}} ...)
//   name                the same, bare: undefined is an error
//   req.method req.url  the request being sent ({{vars}} expanded with the rows so far)
//   req.body            its text body (JSON as sent, text, XML, url-encoded; empty for multipart
//                       and file bodies)
//   req.headers.<name>  a header value (case-insensitive name)
//   ( expr )            grouping
//   fn(arg, ...)        a function (names are case-insensitive):
//     hmacSha256(key, text[, enc])  hmacSha1  hmacSha512   enc: "hex" (default), "base64",
//     sha256(text[, enc])  sha1  sha512  md5                       "base64url"
//     base64(text)  base64url(text)  base64Decode(text)  hex(text)
//     urlencode(text) (RFC 3986)  urldecode(text) ('+' is a space)  lower  upper  trim
//     replace(text, from, to)  substr(text, start[, length])  length(text)  concat(a, b, ...)
//     now()        milliseconds since 1970        unix([offset_s])  seconds since 1970 (+offset)
//     iso([offset_s])  "2026-11-17T06:53:00.123Z"  uuid()  randomInt(lo, hi)  randomHex(bytes)
//     jsonPath(json, path)   "$.a.b[0]", "a.b[0]" or "[0].id" (core/asserts.h path rules)
//
// Safety: expressions come from workspaces that may be cloned from anywhere, so the language has
// no loops, no I/O and at most kMaxPreDepth nested calls; an expression is at most
// kMaxPreExprBytes, a value at most kMaxPreValueBytes. A row never writes a secret variable of the
// active environment, and a value that contains "{{" is refused (it would be expanded again when
// sent). An OS environment variable the workspace may not read stays unexpanded, as everywhere
// (pre_env_refs() lists them for the consent prompt). Nothing throws out of this API.
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/asserts.h"

namespace watt {
struct SavedRequest;
class Workspace;
}  // namespace watt

namespace watt::app::http_editor {

struct PreVar {
  bool enabled = true;
  std::string var;   // name, without braces
  std::string expr;  // "hmacSha256({{secret}}, req.method + req.url + req.body)"
  Capture::Scope scope = Capture::Runtime;
  bool operator==(const PreVar&) const = default;
};

inline constexpr size_t kMaxPreExprBytes = 8192;
inline constexpr size_t kMaxPreValueBytes = size_t{256} << 20;
inline constexpr int kMaxPreDepth = 32;

struct PreVarResult {
  size_t index = 0;     // into the rows passed in
  bool ok = false;
  std::string value;    // the value set
  std::string message;  // why nothing was set ("" when ok)
  double eval_us = 0;
};

// Evaluates the enabled rows that have a name, in order, setting each value in ws.runtime_vars
// (so the next rows and the request see it; the caller decides where it is kept). r is the
// request as it will be sent (req.*). A failed row sets nothing; the rows after it still run.
std::vector<PreVarResult> evaluate_pre_vars(const std::vector<PreVar>& rows, const SavedRequest& r, Workspace& ws);
// Syntax check without evaluating ("" when valid), for editor feedback.
std::string check_pre_expr(std::string_view expr);
// Names of OS environment variables the enabled rows read ({{process.env.X}}) that this workspace
// may not read yet (Workspace::blocked_env_vars for the rows).
std::vector<std::string> pre_env_refs(const std::vector<PreVar>& rows, const Workspace& ws);
// The function signatures the editor offers ("hmacSha256(key, text)").
const std::vector<std::string>& pre_functions();

// Lowercase hex HMAC-SHA256 / SHA-256 (exposed for tests and the OAuth helpers).
std::string hmac_sha256_hex(std::string_view key, std::string_view data);
std::string sha256_hex(std::string_view data);

}  // namespace watt::app::http_editor
