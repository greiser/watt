#pragma once
// Declarative response assertions and captures (Bruno's Assert tab and post-response vars,
// Postman's Test Results), evaluated natively: no script engine.
//
// An Assertion is "<target> <op> <value>", e.g. res.status eq 200, res.body.id isNumber,
// res.responseTime lt 500. A Capture copies the value at a target into a variable, e.g.
// token <- res.body.access_token, so the next request can send {{token}}.
//
// Targets (one path language, Bruno-compatible names):
//   res.status          HTTP status (number). gRPC: the status code, which also compares equal to
//                       its name ("res.status eq OK", "eq 0", "eq not_found" all work)
//   res.statusText      HTTP reason phrase / gRPC status name
//   res.statusMessage   gRPC status message (HTTP: the transport error, if any)
//   res.responseTime    timing.total_ms
//   res.timing.<phase>  queue dns connect tls send ttfb download total (both); prepare socket
//                       redirect process (HTTP); reflect (gRPC)
//   res.size            response body bytes (HTTP: decoded; gRPC: sum of message wire bytes)
//   res.headersSize     response header bytes (HTTP only; gRPC: undefined)
//   res.url             effective URL (HTTP) / target address (gRPC: undefined)
//   res.headers         all headers as a JSON object (gRPC: the initial metadata)
//   res.headers.<name>  first header with that name, ASCII case-insensitive; names with dots or
//   res.headers["X-A"]  other punctuation use the bracket form (dashes work in both)
//   res.trailers.<name> gRPC trailing metadata; res.metadata.<name> gRPC initial metadata
//   res.body            the whole body: its JSON value when it parses, else the text. gRPC: the
//                       first message (the unary response)
//   res.body<seg>...    a path into the JSON body, segments:
//                         .key   ["key"]   ['key']    object member (bracket form for any key)
//                         [3]    [-1]                 array element, negative counts from the end
//                         .length                     size of an array / string (code points) /
//                                                     object, unless an object has a "length" key
//   res.messages        gRPC: all messages as a JSON array; res.messages.length their count;
//   res.messages[i]...  message i (negative from the end) and a path into it
//   Missing members, out-of-range indexes, paths into a non-JSON body: undefined (isUndefined
//   passes, every comparison fails with "got undefined").
//
// Operators (case-insensitive; an empty op means eq):
//   eq neq gt gte lt lte in notIn contains notContains length matches notMatches startsWith
//   endsWith between isEmpty isNotEmpty isNull isUndefined isDefined isTruthy isFalsy isJson
//   isNumber isString isBoolean isArray
// Values (after {{var}} expansion with Workspace::expand, as Bruno interpolates first):
//   200 -1.5 1e3 (number), true false (bool), null, undefined, "quoted" or 'quoted' (string,
//   compared only with strings), anything else is an unquoted string, which also compares with
//   the text of a number / bool. {...} / [...] that parse as JSON compare structurally.
//   in / notIn take a list "a, b, 3"; between takes "lo, hi" (inclusive); matches / notMatches
//   take "/regex/flags" (flags i m s) or a bare pattern, searched anywhere (RE2 syntax, linear
//   time). Number vs. numeric string (headers): compared as numbers.
//   eq on a number compares numerically; contains on a string is a substring test, on an
//   array an element eq test, on an object a key test; length takes a number.
//
// Safety: targets, values and patterns come from workspaces that may be cloned from anywhere.
// Targets are limited to kMaxTargetBytes and kMaxSegments, patterns to kMaxRegexBytes and an
// RE2 program of kMaxRegexProgram (memory kMaxRegexMem); JSON is parsed iteratively (no stack
// growth on deep input) and every walk over JSON (equality, serialization) stops at
// kMaxJsonDepth. Nothing throws out of this API. A capture never writes a secret variable,
// and never stores a value that contains "{{" (a hostile response could otherwise make the
// next request expand {{secret}} into a URL it controls).
//
// Performance: each target is compiled once (process-wide cache) and each response body is
// parsed at most once, lazily, in place in one buffer (no per-string copies), and only when a
// body / message target is evaluated. Share one ResponseData between evaluate_asserts() and
// apply_captures() to parse once for both.
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace watt {

struct HttpResponse;
struct GrpcResponse;
struct SavedRequest;
class Workspace;

struct Assertion {
  bool enabled = true;
  std::string target;  // "res.body.id"
  std::string op;      // "eq"
  std::string value;   // "200", "{{expected}}", "/^ab+c$/i", "10, 20"
  bool operator==(const Assertion&) const = default;
};

struct Capture {
  enum Scope { Environment, Globals, Runtime };
  bool enabled = true;
  std::string var;     // variable name, without braces
  std::string target;  // "res.body.token"
  Scope scope = Runtime;
  bool operator==(const Capture&) const = default;
};

inline constexpr size_t kMaxTargetBytes = 1024;
inline constexpr size_t kMaxSegments = 64;
inline constexpr size_t kMaxRegexBytes = 4096;
inline constexpr int kMaxRegexProgram = 20000;
inline constexpr long long kMaxRegexMem = 2 << 20;
inline constexpr int kMaxJsonDepth = 256;
inline constexpr size_t kMaxVarName = 256;

struct AssertResult {
  const Assertion* a = nullptr;  // into the SavedRequest passed in
  bool pass = false;
  std::string actual;    // the value found, as text (JSON compact, cut at ~200 bytes)
  std::string expected;  // the value after {{var}} expansion
  std::string message;   // why it failed ("" on pass)
  double eval_us = 0;    // this assertion's evaluation (body parse excluded, see parse_us())
};

struct CaptureResult {
  const Capture* c = nullptr;
  bool ok = false;
  std::string value;    // what was stored
  std::string message;  // why nothing was stored, or why it could not be saved
};

// A response prepared for evaluation: holds a pointer to the response (which must outlive it)
// and its lazily parsed JSON. Not thread-safe; cheap to construct (nothing is parsed up front).
class ResponseData {
 public:
  explicit ResponseData(const HttpResponse& r);
  explicit ResponseData(const GrpcResponse& r);
  ~ResponseData();
  ResponseData(const ResponseData&) = delete;
  ResponseData& operator=(const ResponseData&) = delete;
  double parse_us() const;  // time spent parsing JSON so far

  struct Impl;
  Impl& impl() const { return *impl_; }

 private:
  std::unique_ptr<Impl> impl_;
};

// Enabled assertions with a non-empty target, in order. ws (may be null) expands {{vars}} in
// values.
std::vector<AssertResult> evaluate_asserts(const SavedRequest& r, const ResponseData& res, const Workspace* ws);
std::vector<AssertResult> evaluate_asserts(const SavedRequest& r, const HttpResponse& res, const Workspace* ws);
std::vector<AssertResult> evaluate_asserts(const SavedRequest& r, const GrpcResponse& res, const Workspace* ws);

// Enabled captures with a non-empty var, in order. Values are strings: JSON strings as their
// text, numbers / bools / null as their JSON text, objects / arrays as compact JSON.
// Environment: the active environment (save_settings() is called once at the end if any value
// changed, unless persist is false; secrets are never written). Globals: ws->globals (saved the
// same way). Runtime: ws->runtime_vars (memory only).
std::vector<CaptureResult> apply_captures(const SavedRequest& r, const ResponseData& res, Workspace& ws,
                                          bool persist = true);
std::vector<CaptureResult> apply_captures(const SavedRequest& r, const HttpResponse& res, Workspace& ws,
                                          bool persist = true);
std::vector<CaptureResult> apply_captures(const SavedRequest& r, const GrpcResponse& res, Workspace& ws,
                                          bool persist = true);

// The value a target resolves to, as a capture would store it (tests, UI previews). False with
// *err when the target is invalid or undefined.
bool resolve_target(const std::string& target, const ResponseData& res, std::string& out, std::string* err = nullptr);
// Checks a target's syntax without a response ("" when valid), for editor feedback.
std::string check_target(const std::string& target);

// Bruno's .bru notation used in .req.json: "op value" (just "op" without a value).
std::string assert_op_value(const Assertion& a);
void set_assert_op_value(Assertion& a, const std::string& op_value);
const char* capture_scope_name(Capture::Scope s);  // "environment", "globals", "runtime"
Capture::Scope capture_scope_from(const std::string& s);

}  // namespace watt
