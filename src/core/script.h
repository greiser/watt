#pragma once
// Request scripts: JavaScript run before a request is sent (SavedRequest::pre_script) and after
// its response arrived (post_script), in an embedded QuickJS-ng sandbox. Two APIs, so scripts
// from either tool run unchanged:
//
//   Bruno:   bru.getEnvVar/setEnvVar/hasEnvVar/deleteEnvVar, bru.getVar/setVar/hasVar/deleteVar,
//            bru.getGlobalEnvVar/setGlobalEnvVar, bru.getEnvName, bru.interpolate,
//            bru.sendRequest, bru.sleep, req.getUrl/setUrl/getMethod/setMethod/getHeader/
//            getHeaders/setHeader/setHeaders/deleteHeader/getBody/setBody/getName,
//            req.getQueryParam/setQueryParam/deleteQueryParam, res.status/statusText/headers/
//            body/responseTime, res.getStatus/getStatusText/getHeader/getHeaders/getBody/
//            getResponseTime/getSize/getUrl/getTimings, res('path.to[0].x'), test(name, fn),
//            expect(x) (chai subset below)
//   Postman: pm.environment / pm.variables / pm.globals / pm.collectionVariables .get/set/has/
//            unset/toObject (pm.variables.replaceIn), pm.request.url (toString, query.add/upsert/
//            remove/get), pm.request.method, pm.request.headers.add/upsert/remove/get/has,
//            pm.request.body.raw, pm.response.code/status/responseTime/responseSize/json()/
//            text()/headers.get/has/toObject, pm.response.to.have.status/header/jsonBody,
//            pm.response.to.be.ok/success/json/clientError/serverError/..., pm.test,
//            pm.expect, pm.sendRequest, pm.info, pm.visualizer.set(template, data, options) /
//            clear() (kept on ScriptRun for the response view's Visualize tab, never rendered here)
//   Both:    console.log/info/warn/error/debug (captured into ScriptRun::logs), crypto.sha1/
//            sha256/sha384/sha512/md5(data, enc = "hex"), crypto.hmac(alg, key, data, enc),
//            crypto.base64/base64Decode/hex/hexDecode, crypto.randomUUID(), crypto.
//            getRandomValues(u8), atob/btoa, a CryptoJS shim (SHA1/SHA256/SHA512/MD5,
//            HmacSHA1/HmacSHA256/HmacSHA512/HmacMD5, enc.Hex/Base64/Utf8/Latin1, lib.WordArray.
//            random) also returned by require('crypto-js'), setTimeout/clearTimeout (callbacks
//            run after the script, in delay order, without waiting).
//   expect(): to/be/been/is/that/which/and/has/have/with/at/of/same/does/still chains, not,
//            deep, own; equal/equals/eq, eql, a/an, include/contain/includes/contains, property,
//            match, above/gt/greaterThan, below/lt/lessThan, least/gte, most/lte, within,
//            length/lengthOf, oneOf, keys, members, instanceOf, satisfy, closeTo, string, throw,
//            and the ok/true/false/null/undefined/NaN/exist/empty properties.
//   gRPC:    req.* maps to the target address (url), the method path, metadata (headers) and the
//            JSON body; res.messages / res.getMessages() and res.trailers / res.getTrailers()
//            hold the stream, res.getBody() the first message.
//
// Sandbox (scripts usually come with a cloned repository: untrusted code):
//   - QuickJS-ng is linked without quickjs-libc: there is no std / os module, no file system,
//     process, environment or socket access. Module loading is off: static `import` is a syntax
//     error in a script, dynamic import() rejects, and require() knows only 'crypto-js'.
//   - The only way out is sendRequest (pm.sendRequest / bru.sendRequest): watt::HttpClient,
//     http:// and https:// only (redirects too), RFC 9110 method tokens, no CR/LF/NUL in header
//     names or values, literal bodies only (no file uploads), no {{variable}} expansion (so no
//     OS environment variables, no .env, no secret substitution into URLs a script controls),
//     at most ScriptLimits::max_send_requests per run, each with a timeout.
//   - Each run gets a fresh QuickJS runtime and context, destroyed at the end: nothing a script
//     defines survives into the next request. No timers outlive the script (setTimeout callbacks
//     run before the run returns; setInterval is not available).
//   - Limits per run (ScriptLimits): CPU time (interrupt handler; time spent waiting in
//     sendRequest / sleep does not count), heap (JS_SetMemoryLimit), JS stack depth, log bytes.
//     Exceeding one stops the script with an error naming the limit.
//   - Scripts run only when the user trusted the workspace (Workspace::scripts_trusted(), stored
//     outside the workspace); otherwise the run returns untrusted = true and runs nothing.
//
// Variables: reads see the workspace (runtime vars, active environment, globals) plus what the
// script set earlier in the same run. Writes are recorded in ScriptRun::vars_set and applied to
// the workspace by apply_script_vars(), with the capture rules of core/asserts.h: a secret
// environment variable is never overwritten, values containing "{{" are refused (so a response
// cannot smuggle {{secret}} into the next request), names are at most kMaxVarName bytes.
// Values are strings: numbers / bools / objects are stored as their JSON text.
//
// Cost: a request without scripts pays nothing (the run functions return before touching
// QuickJS or the trust file). QuickJS is initialized on the first script run of the process
// (the prelude that defines the APIs above is compiled once to bytecode then), never at start-up.
//
// Typical send path:
//   ScriptRun pre = run_pre_script(ws, req);   // may edit req
//   apply_script_vars(ws, pre);               // so {{vars}} set by the script expand
//   if (!pre.ok) -> show pre.error, do not send
//   auto http = ws.http_to_send(req); HttpResponse resp = client.send(*http);
//   ResponseData rd(resp);                    // shared with evaluate_asserts / apply_captures
//   ScriptRun post = run_post_script(ws, req, rd); apply_script_vars(ws, post);
#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

namespace watt {

class Workspace;
struct SavedRequest;
class ResponseData;

struct ScriptLimits {
  long cpu_ms = 1000;                  // script CPU time (wall time minus host waits)
  size_t memory_bytes = 64u << 20;     // QuickJS heap
  size_t stack_bytes = 512u << 10;     // JS stack (the calling thread needs more than this)
  size_t max_log_bytes = 256u << 10;   // console output kept; the rest is dropped (logs_truncated)
  int max_send_requests = 20;          // sendRequest calls per run
  long send_timeout_ms = 30000;        // per sendRequest (a request's own timeout may be shorter)
  size_t max_send_response_bytes = 16u << 20;  // a larger sendRequest response is cut (and fails)
  long max_sleep_ms = 10000;           // bru.sleep total per run
};

struct ScriptOptions {
  ScriptLimits limits;
  const std::atomic<bool>* cancel = nullptr;  // checked by the interrupt handler and between sends
};

struct ScriptLog {
  enum class Level { Log, Info, Warn, Error };
  Level level = Level::Log;
  std::string text;
};

struct ScriptTest {
  std::string name;
  bool pass = false;
  std::string message;  // the assertion / exception message when it failed
};

struct ScriptVar {
  enum class Scope { Runtime, Environment, Globals };
  Scope scope = Scope::Runtime;
  std::string name;
  std::string value;
  bool removed = false;  // unset / delete
  bool ok = true;        // false: refused (secret, "{{", no active environment, bad name)
  std::string message;   // why it was refused / could not be applied
};

struct ScriptRun {
  bool ran = false;        // a script was executed
  bool untrusted = false;  // a script exists, but this workspace's scripts are not trusted yet
  bool ok = true;          // no script, or it finished without an uncaught error
  std::string error;       // "3:14: ReferenceError: x is not defined", "CPU time limit (1000 ms) exceeded", ...
  int line = 0, column = 0;  // of the error in the script (1-based; 0 = unknown)
  std::vector<ScriptLog> logs;
  bool logs_truncated = false;
  std::vector<ScriptTest> tests;
  std::vector<ScriptVar> vars_set;  // in the order the script made them
  bool request_changed = false;     // pre-request: the script modified the request
  int requests_sent = 0;            // sendRequest calls
  double elapsed_us = 0;            // the whole run, setup and teardown included
  double setup_us = 0;              // runtime + context + prelude (first run: + compiling the prelude)
  double wait_us = 0;               // time in sendRequest / sleep (not counted as CPU time)
  // pm.visualizer.set(template, data, options): the Handlebars template and its data as JSON text
  // (options are ignored), shown by the response view's Visualize tab. Rendering is the viewer's
  // job; nothing here runs the template. visualizer == false: never set, or cleared again.
  bool visualizer = false;
  bool visualizer_cleared = false;  // pm.visualizer.clear() was called (drops an earlier script's set)
  std::string visualizer_template;
  std::string visualizer_data;      // JSON ("{}" when set() got no data)
  std::string visualizer_error;     // why set() could not keep it (too large, data not JSON-serializable)
};

// Largest template / data JSON pm.visualizer.set() keeps (a larger one sets visualizer_error).
inline constexpr size_t kMaxVisualizerTemplate = 1u << 20;
inline constexpr size_t kMaxVisualizerData = 32u << 20;

// Runs req.pre_script (if any). It may change req's method, URL (query), headers and body (HTTP)
// or address, method, metadata and body (gRPC); on an error nothing is changed. Nothing is
// written to ws: apply the variables with apply_script_vars().
ScriptRun run_pre_script(const Workspace& ws, SavedRequest& req, const ScriptOptions& opts = {});
// Runs req.post_script (if any) against the response in `res` (HTTP or gRPC).
ScriptRun run_post_script(const Workspace& ws, const SavedRequest& req, ResponseData& res,
                          const ScriptOptions& opts = {});
// Applies run.vars_set to ws (runtime vars in memory; environment / globals saved once with
// save_settings() when persist and something changed). Entries that cannot be applied get
// ok = false and a message. False if saving failed (*err says why).
bool apply_script_vars(Workspace& ws, ScriptRun& run, bool persist = true, std::string* err = nullptr);

// Process-wide: whether QuickJS has been initialized (i.e. some script ran), for start-up tests.
bool script_engine_initialized();

}  // namespace watt
