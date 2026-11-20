#include "core/script.h"

#include <quickjs.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <rapidjson/document.h>

#include "core/asserts.h"
#include "core/crypto.h"
#include "core/grpc_client.h"
#include "core/http_client.h"
#include "core/model.h"
#include "core/oauth2.h"

namespace watt {

namespace detail {
extern const char* const kScriptPrelude[];
extern const size_t kScriptPreludeChunks;
}  // namespace detail

namespace {

using Clock = std::chrono::steady_clock;

double us_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
}

std::atomic<bool> g_initialized{false};

// The prelude, compiled once per process to bytecode (our own code, never a script's).
struct PreludeCache {
  std::once_flag once;
  std::string source;
  std::vector<uint8_t> bytecode;
};
PreludeCache& prelude_cache() {
  static PreludeCache c;
  return c;
}

constexpr size_t kMaxVarValue = 1u << 20;
constexpr size_t kMaxTestText = 4096;
constexpr size_t kMaxRequestHeaders = 10000;

// ------------------------------------------------------------------ host state

struct Host {
  const Workspace* ws = nullptr;
  ScriptRun* run = nullptr;
  const ScriptOptions* opts = nullptr;
  bool pre = true;
  Clock::time_point deadline;
  bool cpu_exceeded = false;
  bool cancelled = false;
  size_t log_bytes = 0;
  const Environment* env = nullptr;
  // Read-only folder vars (nearest folder wins) and collection vars of the request: seen by
  // reads of every scope at once (pm.variables, interpolate), between runtime and environment /
  // between environment and globals, as in Workspace::expand().
  const std::map<std::string, std::string>* folder_vars = nullptr;
  const std::map<std::string, std::string>* collection_vars = nullptr;
  // What the script set in this run, per scope (0 runtime, 1 environment, 2 globals); nullopt = removed.
  std::map<std::string, std::optional<std::string>> ov[3];
  ResponseData* rd = nullptr;
  std::unique_ptr<HttpClient> client;
  long slept_ms = 0;
  // Promises rejected with no handler yet (a handler may still be attached later in the run).
  std::vector<std::pair<JSValue, std::string>> rejections;

  // Time spent waiting for the host (network, sleep) is not CPU time: move the deadline.
  void waited(Clock::time_point t0) {
    const auto d = Clock::now() - t0;
    deadline += d;
    run->wait_us += std::chrono::duration<double, std::micro>(d).count();
  }

  std::optional<std::string> base(int s, const std::string& n) const {
    const std::map<std::string, std::string>* m = s == 0   ? &ws->runtime_vars
                                                  : s == 1 ? (env ? &env->vars : nullptr)
                                                           : &ws->globals;
    if (!m) return std::nullopt;
    auto it = m->find(n);
    if (it == m->end()) return std::nullopt;
    return it->second;
  }
  std::optional<std::string> effective(int s, const std::string& n) const {
    if (auto it = ov[s].find(n); it != ov[s].end()) return it->second;
    return base(s, n);
  }
  static std::optional<std::string> in(const std::map<std::string, std::string>* m, const std::string& n) {
    if (!m) return std::nullopt;
    auto it = m->find(n);
    if (it == m->end()) return std::nullopt;
    return it->second;
  }
  std::optional<std::string> get(int s, const std::string& n) const {
    if (s < 3) return effective(s, n);
    if (auto v = effective(0, n)) return v;
    if (auto v = in(folder_vars, n)) return v;
    if (auto v = effective(1, n)) return v;
    if (auto v = in(collection_vars, n)) return v;
    return effective(2, n);
  }
};

Host& host(JSContext* ctx) { return *static_cast<Host*>(JS_GetContextOpaque(ctx)); }

int interrupt_handler(JSRuntime*, void* opaque) {
  Host& h = *static_cast<Host*>(opaque);
  if (h.opts->cancel && h.opts->cancel->load(std::memory_order_relaxed)) {
    h.cancelled = true;
    return 1;
  }
  if (Clock::now() > h.deadline) {
    h.cpu_exceeded = true;
    return 1;
  }
  return 0;
}

JSModuleDef* no_modules(JSContext* ctx, const char* name, void*) {
  JS_ThrowReferenceError(ctx, "cannot import '%s': module loading is disabled in watt scripts", name);
  return nullptr;
}

void on_unhandled_rejection(JSContext* ctx, JSValueConst promise, JSValueConst reason, bool is_handled, void*) {
  Host& h = host(ctx);
  if (is_handled) {
    for (auto it = h.rejections.begin(); it != h.rejections.end(); ++it)
      if (JS_IsSameValue(ctx, it->first, promise)) {
        JS_FreeValue(ctx, it->first);
        h.rejections.erase(it);
        break;
      }
    return;
  }
  if (h.rejections.size() >= 100) return;
  std::string t;
  const char* s = JS_ToCString(ctx, reason);
  t = s ? s : "?";
  if (s) JS_FreeCString(ctx, s);
  else JS_FreeValue(ctx, JS_GetException(ctx));
  h.rejections.emplace_back(JS_DupValue(ctx, promise), std::move(t));
}

// Logs the rejections nobody handled (except the script's own result: that is the run's error).
void flush_rejections(JSContext* ctx, Host& h, JSValueConst result) {
  for (auto& [p, text] : h.rejections) {
    if (!JS_IsSameValue(ctx, p, result)) {
      std::string line = "unhandled promise rejection: " + text;
      if (h.log_bytes + line.size() <= h.opts->limits.max_log_bytes) {
        h.log_bytes += line.size();
        h.run->logs.push_back({ScriptLog::Level::Warn, std::move(line)});
      }
    }
    JS_FreeValue(ctx, p);
  }
  h.rejections.clear();
}

// ------------------------------------------------------------------ value helpers

bool to_string(JSContext* ctx, JSValueConst v, std::string& out) {
  size_t n = 0;
  const char* s = JS_ToCStringLen(ctx, &n, v);
  if (!s) return false;
  out.assign(s, n);
  JS_FreeCString(ctx, s);
  return true;
}

std::string arg_string(JSContext* ctx, int argc, JSValueConst* argv, int i) {
  std::string s;
  if (i < argc) to_string(ctx, argv[i], s);
  return s;
}

// A Uint8Array (what the prelude passes) or a string (UTF-8).
bool arg_bytes(JSContext* ctx, int argc, JSValueConst* argv, int i, std::string& out) {
  if (i >= argc) {
    out.clear();
    return true;
  }
  if (JS_IsString(argv[i])) return to_string(ctx, argv[i], out);
  size_t n = 0;
  uint8_t* p = JS_GetUint8Array(ctx, &n, argv[i]);
  if (!p) return false;  // exception set
  out.assign(reinterpret_cast<const char*>(p), n);
  return true;
}

JSValue new_string(JSContext* ctx, std::string_view s) { return JS_NewStringLen(ctx, s.data(), s.size()); }
JSValue new_buffer(JSContext* ctx, std::string_view s) {
  return JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

void set_str(JSContext* ctx, JSValueConst o, const char* k, std::string_view v) { JS_SetPropertyStr(ctx, o, k, new_string(ctx, v)); }
void set_num(JSContext* ctx, JSValueConst o, const char* k, double v) { JS_SetPropertyStr(ctx, o, k, JS_NewFloat64(ctx, v)); }

template <class Pairs>
JSValue pairs_array(JSContext* ctx, const Pairs& pairs) {
  JSValue arr = JS_NewArray(ctx);
  uint32_t i = 0;
  for (const auto& [k, v] : pairs) {
    JSValue p = JS_NewArray(ctx);
    JS_DefinePropertyValueUint32(ctx, p, 0, new_string(ctx, k), JS_PROP_C_W_E);
    JS_DefinePropertyValueUint32(ctx, p, 1, new_string(ctx, v), JS_PROP_C_W_E);
    JS_DefinePropertyValueUint32(ctx, arr, i++, p, JS_PROP_C_W_E);
  }
  return arr;
}

// rapidjson DOM -> JS values (own properties defined, never set: "__proto__" stays a key).
JSValue from_json(JSContext* ctx, const rapidjson::Value& v, int depth) {
  if (depth > kMaxJsonDepth) return JS_ThrowRangeError(ctx, "JSON nested deeper than %d levels", kMaxJsonDepth);
  switch (v.GetType()) {
    case rapidjson::kNullType: return JS_NULL;
    case rapidjson::kFalseType: return JS_FALSE;
    case rapidjson::kTrueType: return JS_TRUE;
    case rapidjson::kNumberType:
      if (v.IsInt64()) return JS_NewInt64(ctx, v.GetInt64());
      return JS_NewFloat64(ctx, v.GetDouble());
    case rapidjson::kStringType: return JS_NewStringLen(ctx, v.GetString(), v.GetStringLength());
    case rapidjson::kArrayType: {
      JSValue a = JS_NewArray(ctx);
      if (JS_IsException(a)) return a;
      uint32_t i = 0;
      for (const auto& e : v.GetArray()) {
        JSValue x = from_json(ctx, e, depth + 1);
        if (JS_IsException(x) || JS_DefinePropertyValueUint32(ctx, a, i++, x, JS_PROP_C_W_E) < 0) {
          JS_FreeValue(ctx, a);
          return JS_EXCEPTION;
        }
      }
      return a;
    }
    case rapidjson::kObjectType: {
      JSValue o = JS_NewObject(ctx);
      if (JS_IsException(o)) return o;
      for (auto it = v.MemberBegin(); it != v.MemberEnd(); ++it) {
        JSValue x = from_json(ctx, it->value, depth + 1);
        if (JS_IsException(x)) {
          JS_FreeValue(ctx, o);
          return JS_EXCEPTION;
        }
        const JSAtom atom = JS_NewAtomLen(ctx, it->name.GetString(), it->name.GetStringLength());
        const int r = atom == JS_ATOM_NULL ? -1 : JS_DefinePropertyValue(ctx, o, atom, x, JS_PROP_C_W_E);
        if (atom != JS_ATOM_NULL) JS_FreeAtom(ctx, atom);
        else JS_FreeValue(ctx, x);
        if (r < 0) {
          JS_FreeValue(ctx, o);
          return JS_EXCEPTION;
        }
      }
      return o;
    }
  }
  return JS_UNDEFINED;
}

bool bad_header_text(std::string_view s) {
  return std::any_of(s.begin(), s.end(), [](char c) { return c == '\r' || c == '\n' || c == '\0'; });
}

// ------------------------------------------------------------------ natives

#define HOST_FN(name) JSValue name(JSContext* ctx, JSValueConst, [[maybe_unused]] int argc, [[maybe_unused]] JSValueConst* argv)

HOST_FN(h_log) {
  Host& h = host(ctx);
  int32_t lvl = 0;
  if (argc > 0) JS_ToInt32(ctx, &lvl, argv[0]);
  std::string t = arg_string(ctx, argc, argv, 1);
  const size_t cap = h.opts->limits.max_log_bytes;
  if (h.run->logs_truncated) return JS_UNDEFINED;
  if (h.log_bytes + t.size() > cap) {
    t.resize(cap > h.log_bytes ? cap - h.log_bytes : 0);
    h.run->logs_truncated = true;
  }
  h.log_bytes += t.size();
  ScriptLog::Level l = lvl == 1 ? ScriptLog::Level::Info : lvl == 2 ? ScriptLog::Level::Warn
                       : lvl == 3 ? ScriptLog::Level::Error : ScriptLog::Level::Log;
  h.run->logs.push_back({l, std::move(t)});
  return JS_UNDEFINED;
}

HOST_FN(h_test) {
  Host& h = host(ctx);
  ScriptTest t;
  t.name = arg_string(ctx, argc, argv, 0);
  t.pass = argc > 1 && JS_ToBool(ctx, argv[1]) > 0;
  t.message = arg_string(ctx, argc, argv, 2);
  if (t.name.size() > kMaxTestText) t.name.resize(kMaxTestText);
  if (t.message.size() > kMaxTestText) t.message.resize(kMaxTestText);
  if (h.run->tests.size() < 100000) h.run->tests.push_back(std::move(t));
  return JS_UNDEFINED;
}

// H.vis(on, template, dataJson, error): pm.visualizer.set (on = 1) / clear (on = 0).
HOST_FN(h_vis) {
  Host& h = host(ctx);
  ScriptRun& r = *h.run;
  int32_t on = 0;
  if (argc > 0) JS_ToInt32(ctx, &on, argv[0]);
  r.visualizer_template.clear();
  r.visualizer_data.clear();
  r.visualizer_error.clear();
  r.visualizer = false;
  if (!on) {
    r.visualizer_cleared = true;
    return JS_UNDEFINED;
  }
  r.visualizer_cleared = false;
  std::string err = arg_string(ctx, argc, argv, 3);
  std::string tpl = arg_string(ctx, argc, argv, 1);
  std::string data = arg_string(ctx, argc, argv, 2);
  if (err.empty() && tpl.size() > kMaxVisualizerTemplate)
    err = "pm.visualizer.set: the template is larger than " + std::to_string(kMaxVisualizerTemplate >> 20) + " MiB";
  if (err.empty() && data.size() > kMaxVisualizerData)
    err = "pm.visualizer.set: the data is larger than " + std::to_string(kMaxVisualizerData >> 20) + " MiB as JSON";
  r.visualizer = true;
  if (!err.empty()) {
    if (err.size() > kMaxTestText) err.resize(kMaxTestText);
    r.visualizer_error = std::move(err);
    return JS_UNDEFINED;
  }
  r.visualizer_template = std::move(tpl);
  r.visualizer_data = data.empty() ? std::string("{}") : std::move(data);
  return JS_UNDEFINED;
}

int scope_of(JSContext* ctx, int argc, JSValueConst* argv) {
  const std::string s = arg_string(ctx, argc, argv, 0);
  return s == "e" ? 1 : s == "g" ? 2 : s == "a" ? 3 : 0;
}

ScriptVar::Scope var_scope(int s) {
  return s == 1 ? ScriptVar::Scope::Environment : s == 2 ? ScriptVar::Scope::Globals : ScriptVar::Scope::Runtime;
}

const char* scope_label(int s) { return s == 1 ? "environment" : s == 2 ? "global" : "runtime"; }

HOST_FN(h_get) {
  Host& h = host(ctx);
  const auto v = h.get(scope_of(ctx, argc, argv), arg_string(ctx, argc, argv, 1));
  return v ? new_string(ctx, *v) : JS_UNDEFINED;
}

HOST_FN(h_has) {
  Host& h = host(ctx);
  return JS_NewBool(ctx, h.get(scope_of(ctx, argc, argv), arg_string(ctx, argc, argv, 1)).has_value());
}

// Records a write (or a refused one) under the capture rules. value == nullopt: remove.
bool record_var(Host& h, int s, const std::string& name, const std::optional<std::string>& value) {
  if (s == 3) s = 0;
  ScriptVar v;
  v.scope = var_scope(s);
  v.name = name;
  v.removed = !value;
  if (value) v.value = *value;
  auto refuse = [&](std::string why) {
    v.ok = false;
    v.message = why;
    std::string line = std::string("setting the ") + scope_label(s) + " variable '" + name.substr(0, 64) + "' was refused: " + why;
    if (h.log_bytes + line.size() <= h.opts->limits.max_log_bytes) {
      h.log_bytes += line.size();
      h.run->logs.push_back({ScriptLog::Level::Warn, std::move(line)});
    }
    h.run->vars_set.push_back(std::move(v));
    return false;
  };
  if (name.empty() || name.size() > kMaxVarName) return refuse("a variable name must be 1 to " + std::to_string(kMaxVarName) + " bytes");
  if (name.find("{{") != std::string::npos || name.find("}}") != std::string::npos) return refuse("the name contains {{ or }}");
  if (value && value->size() > kMaxVarValue) return refuse("the value is larger than 1 MiB");
  if (value && value->find("{{") != std::string::npos)
    return refuse("the value contains \"{{\" (it would be expanded as a template in the next request)");
  if (s == 1) {
    if (!h.env) return refuse("no environment is active");
    if (h.env->secrets.count(name) || h.env->unreadable_secrets.count(name))
      return refuse("it is a secret variable (secrets are never overwritten by scripts)");
  }
  h.ov[s][name] = value;
  h.run->vars_set.push_back(std::move(v));
  return true;
}

HOST_FN(h_set) {
  Host& h = host(ctx);
  return JS_NewBool(ctx, record_var(h, scope_of(ctx, argc, argv), arg_string(ctx, argc, argv, 1), arg_string(ctx, argc, argv, 2)));
}

HOST_FN(h_unset) {
  Host& h = host(ctx);
  return JS_NewBool(ctx, record_var(h, scope_of(ctx, argc, argv), arg_string(ctx, argc, argv, 1), std::nullopt));
}

HOST_FN(h_all) {
  Host& h = host(ctx);
  const int s = scope_of(ctx, argc, argv);
  std::map<std::string, std::string> m;
  auto merge = [&](int i) {
    const std::map<std::string, std::string>* b = i == 0   ? &h.ws->runtime_vars
                                                  : i == 1 ? (h.env ? &h.env->vars : nullptr)
                                                           : &h.ws->globals;
    if (b)
      for (const auto& [k, v] : *b) m[k] = v;
    for (const auto& [k, v] : h.ov[i]) {
      if (v) m[k] = *v;
      else m.erase(k);
    }
  };
  if (s == 3) {
    merge(2);
    if (h.collection_vars)
      for (const auto& [k, v] : *h.collection_vars) m[k] = v;
    merge(1);
    if (h.folder_vars)
      for (const auto& [k, v] : *h.folder_vars) m[k] = v;
    merge(0);
  } else {
    merge(s);
  }
  JSValue o = JS_NewObject(ctx);
  for (const auto& [k, v] : m) {
    const JSAtom a = JS_NewAtomLen(ctx, k.data(), k.size());
    JS_DefinePropertyValue(ctx, o, a, new_string(ctx, v), JS_PROP_C_W_E);
    JS_FreeAtom(ctx, a);
  }
  return o;
}

HOST_FN(h_env_name) {
  Host& h = host(ctx);
  return h.env ? new_string(ctx, h.env->name) : JS_UNDEFINED;
}

HOST_FN(h_res) {
  Host& h = host(ctx);
  if (!h.rd) return JS_UNDEFINED;
  JSValue o = JS_NewObject(ctx);
  if (const HttpResponse* r = h.rd->http()) {
    set_str(ctx, o, "kind", "http");
    set_num(ctx, o, "status", static_cast<double>(r->status));
    set_str(ctx, o, "statusText", r->status_text);
    set_str(ctx, o, "httpVersion", r->http_version);
    JS_SetPropertyStr(ctx, o, "headers", pairs_array(ctx, r->headers));
    set_str(ctx, o, "url", r->effective_url);
    set_num(ctx, o, "time", r->timing.total_ms);
    set_num(ctx, o, "size", static_cast<double>(r->body.size()));
    set_str(ctx, o, "error", r->error);
    set_str(ctx, o, "statusMessage", r->error);
    JSValue t = JS_NewObject(ctx);
    const Timing& tm = r->timing;
    set_num(ctx, t, "prepare", r->prepare_ms);
    set_num(ctx, t, "queue", tm.queue_ms);
    set_num(ctx, t, "dns", tm.dns_ms);
    set_num(ctx, t, "socket", r->socket_ms);
    set_num(ctx, t, "connect", tm.connect_ms);
    set_num(ctx, t, "tls", tm.tls_ms);
    set_num(ctx, t, "send", tm.send_ms);
    set_num(ctx, t, "ttfb", tm.ttfb_ms);
    set_num(ctx, t, "download", tm.download_ms);
    set_num(ctx, t, "redirect", r->redirect_ms);
    set_num(ctx, t, "process", r->process_ms);
    set_num(ctx, t, "total", tm.total_ms);
    JS_SetPropertyStr(ctx, t, "reused", JS_NewBool(ctx, tm.connection_reused));
    JS_SetPropertyStr(ctx, o, "timings", t);
  } else if (const GrpcResponse* g = h.rd->grpc()) {
    set_str(ctx, o, "kind", "grpc");
    set_num(ctx, o, "status", g->status_code);
    set_str(ctx, o, "statusText", g->status_name);
    set_str(ctx, o, "statusMessage", g->status_message);
    JS_SetPropertyStr(ctx, o, "headers", pairs_array(ctx, g->headers));
    JS_SetPropertyStr(ctx, o, "trailers", pairs_array(ctx, g->trailers));
    set_str(ctx, o, "url", "");
    set_num(ctx, o, "time", g->timing.total_ms);
    size_t size = 0;
    for (const auto& m : g->messages) size += m.wire_bytes;
    set_num(ctx, o, "size", static_cast<double>(size));
    set_num(ctx, o, "messageCount", static_cast<double>(g->messages.size()));
    JSValue t = JS_NewObject(ctx);
    const Timing& tm = g->timing;
    set_num(ctx, t, "queue", tm.queue_ms);
    set_num(ctx, t, "dns", tm.dns_ms);
    set_num(ctx, t, "connect", tm.connect_ms);
    set_num(ctx, t, "tls", tm.tls_ms);
    set_num(ctx, t, "send", tm.send_ms);
    set_num(ctx, t, "ttfb", tm.ttfb_ms);
    set_num(ctx, t, "download", tm.download_ms);
    set_num(ctx, t, "reflect", g->reflect_ms);
    set_num(ctx, t, "total", tm.total_ms);
    JS_SetPropertyStr(ctx, o, "timings", t);
  }
  return o;
}

std::string_view res_text(const ResponseData& rd, size_t i = 0) {
  if (const HttpResponse* r = rd.http()) return r->body;
  if (const GrpcResponse* g = rd.grpc(); g && i < g->messages.size()) return g->messages[i].json;
  return {};
}

// H.resBody(strict): the parsed JSON body (first gRPC message), else its text; strict: throws
// instead (pm.response.json()).
HOST_FN(h_res_body) {
  Host& h = host(ctx);
  if (!h.rd) return JS_UNDEFINED;
  const bool strict = argc > 0 && JS_ToBool(ctx, argv[0]) > 0;
  if (const GrpcResponse* g = h.rd->grpc(); g && g->messages.empty()) {
    if (strict) return JS_ThrowSyntaxError(ctx, "the response has no message");
    return JS_UNDEFINED;
  }
  if (const rapidjson::Value* v = h.rd->json(0)) return from_json(ctx, *v, 0);
  if (strict) return JS_ThrowSyntaxError(ctx, "the response body is not valid JSON");
  return new_string(ctx, res_text(*h.rd));
}

HOST_FN(h_res_text) {
  Host& h = host(ctx);
  if (!h.rd) return JS_UNDEFINED;
  return new_string(ctx, res_text(*h.rd));
}

HOST_FN(h_res_messages) {
  Host& h = host(ctx);
  const GrpcResponse* g = h.rd ? h.rd->grpc() : nullptr;
  if (!g) return JS_UNDEFINED;
  JSValue a = JS_NewArray(ctx);
  for (size_t i = 0; i < g->messages.size(); ++i) {
    JSValue x;
    if (const rapidjson::Value* v = h.rd->json(i)) x = from_json(ctx, *v, 0);
    else x = new_string(ctx, g->messages[i].json);
    if (JS_IsException(x)) {
      JS_FreeValue(ctx, a);
      return x;
    }
    JS_DefinePropertyValueUint32(ctx, a, static_cast<uint32_t>(i), x, JS_PROP_C_W_E);
  }
  return a;
}

HOST_FN(h_digest) {
  crypto::Hash alg;
  if (!crypto::hash_from_name(arg_string(ctx, argc, argv, 0), alg))
    return JS_ThrowTypeError(ctx, "unknown hash algorithm (md5, sha1, sha256, sha384, sha512)");
  std::string data;
  if (!arg_bytes(ctx, argc, argv, 1, data)) return JS_EXCEPTION;
  return new_buffer(ctx, crypto::digest(alg, data));
}

HOST_FN(h_hmac) {
  crypto::Hash alg;
  if (!crypto::hash_from_name(arg_string(ctx, argc, argv, 0), alg))
    return JS_ThrowTypeError(ctx, "unknown HMAC algorithm (md5, sha1, sha256, sha384, sha512)");
  std::string key, data;
  if (!arg_bytes(ctx, argc, argv, 1, key) || !arg_bytes(ctx, argc, argv, 2, data)) return JS_EXCEPTION;
  return new_buffer(ctx, crypto::hmac(alg, key, data));
}

HOST_FN(h_random) {
  uint32_t n = 0;
  if (argc > 0) JS_ToUint32(ctx, &n, argv[0]);
  if (n > (1u << 20)) return JS_ThrowRangeError(ctx, "at most 1 MiB of random bytes at once");
  return new_buffer(ctx, crypto::random_bytes(n));
}

HOST_FN(h_uuid) { return new_string(ctx, crypto::uuid_v4()); }

HOST_FN(h_utf8) {
  std::string s;
  if (argc > 0 && !to_string(ctx, argv[0], s)) return JS_EXCEPTION;
  return new_buffer(ctx, s);
}

HOST_FN(h_utf8dec) {
  std::string b;
  if (!arg_bytes(ctx, argc, argv, 0, b)) return JS_EXCEPTION;
  return new_string(ctx, b);
}

HOST_FN(h_b64) {
  std::string b;
  if (!arg_bytes(ctx, argc, argv, 0, b)) return JS_EXCEPTION;
  const bool url = argc > 1 && JS_ToBool(ctx, argv[1]) > 0;
  return new_string(ctx, crypto::base64(b, url, !url));
}

HOST_FN(h_unb64) {
  std::string out;
  if (!crypto::unbase64(arg_string(ctx, argc, argv, 0), out)) return JS_NULL;
  return new_buffer(ctx, out);
}

HOST_FN(h_hex) {
  std::string b;
  if (!arg_bytes(ctx, argc, argv, 0, b)) return JS_EXCEPTION;
  return new_string(ctx, crypto::hex(b));
}

HOST_FN(h_unhex) {
  std::string out;
  if (!crypto::unhex(arg_string(ctx, argc, argv, 0), out)) return JS_NULL;
  return new_buffer(ctx, out);
}

HOST_FN(h_sleep) {
  Host& h = host(ctx);
  double ms = 0;
  if (argc > 0) JS_ToFloat64(ctx, &ms, argv[0]);
  const long left = h.opts->limits.max_sleep_ms - h.slept_ms;
  long want = ms > 0 ? static_cast<long>(std::min<double>(ms, 1e9)) : 0;
  if (want > left) want = left > 0 ? left : 0;
  const auto t0 = Clock::now();
  const auto until = t0 + std::chrono::milliseconds(want);
  while (Clock::now() < until) {
    if (h.opts->cancel && h.opts->cancel->load()) break;
    const auto rest = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count();
    std::this_thread::sleep_for(std::chrono::milliseconds(std::clamp<long long>(rest, 1, 50)));
  }
  h.slept_ms += want;
  h.waited(t0);
  return JS_UNDEFINED;
}

// H.send({url, method, headers: [[k, v]], body: string | Uint8Array, timeout}) ->
//   {status, statusText, httpVersion, headers, body, timeMs, size, url} | {error}
HOST_FN(h_send) {
  Host& h = host(ctx);
  const ScriptLimits& lim = h.opts->limits;
  auto fail = [&](const std::string& e) {
    JSValue o = JS_NewObject(ctx);
    set_str(ctx, o, "error", e);
    return o;
  };
  if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "sendRequest: bad options");
  if (h.run->requests_sent >= lim.max_send_requests)
    return JS_ThrowRangeError(ctx, "sendRequest: at most %d requests per script run", lim.max_send_requests);
  ++h.run->requests_sent;
  HttpRequest req;
  JSValue o = argv[0];
  JSValue v = JS_GetPropertyStr(ctx, o, "url");
  bool okv = to_string(ctx, v, req.url);
  JS_FreeValue(ctx, v);
  if (!okv) return JS_EXCEPTION;
  v = JS_GetPropertyStr(ctx, o, "method");
  okv = to_string(ctx, v, req.method);
  JS_FreeValue(ctx, v);
  if (!okv) return JS_EXCEPTION;
  // The same gate as a normal send, minus everything that reads local data: http(s) only (curl
  // is restricted to http/https for redirects too), a method token, clean header lines, a literal
  // body, no {{variable}} expansion (no OS environment, .env or secret substitution).
  if (!is_safe_http_url(req.url))
    return fail("sendRequest: only http:// and https:// URLs can be requested from a script (got \"" +
                req.url.substr(0, 200) + "\")");
  if (!is_http_token(req.method)) return fail("sendRequest: invalid HTTP method \"" + req.method.substr(0, 50) + "\"");
  v = JS_GetPropertyStr(ctx, o, "headers");
  int64_t n = 0;
  if (JS_IsArray(v)) JS_GetLength(ctx, v, &n);
  for (int64_t i = 0; i < n && i < static_cast<int64_t>(kMaxRequestHeaders); ++i) {
    JSValue p = JS_GetPropertyUint32(ctx, v, static_cast<uint32_t>(i));
    JSValue k = JS_GetPropertyUint32(ctx, p, 0), val = JS_GetPropertyUint32(ctx, p, 1);
    Header hd;
    const bool ok = to_string(ctx, k, hd.first) && to_string(ctx, val, hd.second);
    JS_FreeValue(ctx, k);
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, p);
    if (!ok) {
      JS_FreeValue(ctx, v);
      return JS_EXCEPTION;
    }
    if (!is_http_token(hd.first) || bad_header_text(hd.second)) {
      JS_FreeValue(ctx, v);
      return fail("sendRequest: invalid header \"" + hd.first.substr(0, 100) + "\" (a name token, no line breaks)");
    }
    req.headers.push_back(std::move(hd));
  }
  JS_FreeValue(ctx, v);
  v = JS_GetPropertyStr(ctx, o, "body");
  if (!JS_IsUndefined(v)) {
    JSValueConst args[1] = {v};
    if (!arg_bytes(ctx, 1, args, 0, req.body)) {
      JS_FreeValue(ctx, v);
      return JS_EXCEPTION;
    }
  }
  JS_FreeValue(ctx, v);
  long timeout = lim.send_timeout_ms;
  v = JS_GetPropertyStr(ctx, o, "timeout");
  if (JS_IsNumber(v)) {
    double t = 0;
    JS_ToFloat64(ctx, &t, v);
    if (t > 0 && t < static_cast<double>(timeout)) timeout = static_cast<long>(t);
  }
  JS_FreeValue(ctx, v);
  req.timeout_ms = timeout;
  req.keep_body = false;
  if (h.opts->cancel && h.opts->cancel->load()) return fail("sendRequest: canceled");
  if (!h.client) h.client = std::make_unique<HttpClient>();
  std::string body;
  bool too_big = false;
  HttpClient* client = h.client.get();
  const auto t0 = Clock::now();
  HttpResponse r = client->send(req, [&](const char* data, size_t len) {
    if (body.size() + len > lim.max_send_response_bytes) {
      too_big = true;
      client->cancel();
      return;
    }
    body.append(data, len);
  });
  h.waited(t0);
  if (too_big)
    return fail("sendRequest: the response is larger than " + std::to_string(lim.max_send_response_bytes >> 20) + " MiB");
  if (!r.ok) return fail("sendRequest: " + r.error);
  JSValue out = JS_NewObject(ctx);
  set_num(ctx, out, "status", static_cast<double>(r.status));
  set_str(ctx, out, "statusText", r.status_text);
  set_str(ctx, out, "httpVersion", r.http_version);
  JS_SetPropertyStr(ctx, out, "headers", pairs_array(ctx, r.headers));
  set_str(ctx, out, "body", body);
  set_num(ctx, out, "size", static_cast<double>(body.size()));
  set_num(ctx, out, "timeMs", r.timing.total_ms);
  set_str(ctx, out, "url", r.effective_url);
  return out;
}

#undef HOST_FN

struct NativeFn {
  const char* name;
  JSCFunction* fn;
  int len;
};
const NativeFn kNatives[] = {
    {"log", h_log, 2},           {"test", h_test, 3},          {"get", h_get, 2},
    {"has", h_has, 2},           {"set", h_set, 3},            {"unset", h_unset, 2},
    {"all", h_all, 1},           {"envName", h_env_name, 0},   {"res", h_res, 0},
    {"resBody", h_res_body, 1},  {"resText", h_res_text, 0},   {"resMessages", h_res_messages, 0},
    {"digest", h_digest, 2},     {"hmac", h_hmac, 3},          {"random", h_random, 1},
    {"uuid", h_uuid, 0},         {"utf8", h_utf8, 1},          {"utf8dec", h_utf8dec, 1},
    {"b64", h_b64, 2},           {"unb64", h_unb64, 1},        {"hex", h_hex, 1},
    {"unhex", h_unhex, 1},       {"sleep", h_sleep, 1},        {"send", h_send, 1},
    {"vis", h_vis, 4},
};

// ------------------------------------------------------------------ errors

// "line:col" of the first stack frame in `file`.
void error_position(const std::string& stack, const std::string& file, int& line, int& col) {
  const std::string key = file + ":";
  size_t p = 0;
  while ((p = stack.find(key, p)) != std::string::npos) {
    p += key.size();
    int l = 0, c = 0;
    size_t q = p;
    while (q < stack.size() && stack[q] >= '0' && stack[q] <= '9') l = l * 10 + (stack[q++] - '0');
    if (q == p || q >= stack.size() || stack[q] != ':') continue;
    size_t r = ++q;
    while (r < stack.size() && stack[r] >= '0' && stack[r] <= '9') c = c * 10 + (stack[r++] - '0');
    line = l;
    col = r > q ? c : 0;
    return;
  }
}

void set_error(JSContext* ctx, Host& h, JSValue exc, const std::string& file) {
  ScriptRun& run = *h.run;
  run.ok = false;
  const ScriptLimits& lim = h.opts->limits;
  if (h.cancelled) {
    run.error = "canceled";
    return;
  }
  if (h.cpu_exceeded) {
    run.error = "CPU time limit (" + std::to_string(lim.cpu_ms) + " ms) exceeded: the script was stopped";
    return;
  }
  std::string text, stack;
  if (JS_IsError(exc)) {
    JSValue name = JS_GetPropertyStr(ctx, exc, "name"), msg = JS_GetPropertyStr(ctx, exc, "message"),
            st = JS_GetPropertyStr(ctx, exc, "stack");
    std::string n, m;
    to_string(ctx, name, n);
    to_string(ctx, msg, m);
    if (JS_IsString(st)) to_string(ctx, st, stack);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, msg);
    JS_FreeValue(ctx, st);
    text = (n.empty() ? "Error" : n) + ": " + m;
    if (m == "out of memory" || n == "InternalError" && m.find("out of memory") != std::string::npos)
      text = "memory limit (" + std::to_string(lim.memory_bytes >> 20) + " MiB) exceeded: the script was stopped";
    else if (n == "RangeError" && (m.find("stack") != std::string::npos))
      text += " (JS stack limit " + std::to_string(lim.stack_bytes >> 10) + " KiB)";
  } else {
    std::string s;
    if (!to_string(ctx, exc, s)) s = "?";
    text = "uncaught exception: " + s;
    // When even the error object cannot be allocated QuickJS throws null: tell it from a script's
    // own `throw null` by how full the heap is.
    JSMemoryUsage mu{};
    JS_ComputeMemoryUsage(JS_GetRuntime(ctx), &mu);
    if (s == "out of memory" ||
        (JS_IsNull(exc) && static_cast<double>(mu.malloc_size) > 0.9 * static_cast<double>(lim.memory_bytes)))
      text = "memory limit (" + std::to_string(lim.memory_bytes >> 20) + " MiB) exceeded: the script was stopped";
  }
  JS_FreeValue(ctx, JS_GetException(ctx));  // an OOM while reading the error leaves one pending
  error_position(stack, file, run.line, run.column);
  run.error = run.line > 0 ? std::to_string(run.line) + ":" + std::to_string(run.column) + ": " + text : text;
}

// ------------------------------------------------------------------ request data

// The URL / body without the editor's encode marks ({{name|q}} -> {{name}}, model.h): a script
// reads the template it would read in Bruno; read_req() keeps the marks when it is left as read.
bool has_var_marks(const std::string& s) {
  return s.find("|q}}") != std::string::npos || s.find("|f}}") != std::string::npos ||
         s.find("|p}}") != std::string::npos;
}
std::string as_script_sees(const std::string& s) { return has_var_marks(s) ? strip_var_marks(s) : s; }

JSValue make_req(JSContext* ctx, const SavedRequest& r) {
  JSValue o = JS_NewObject(ctx);
  const bool grpc = r.kind == RequestKind::Grpc;
  set_str(ctx, o, "kind", grpc ? "grpc" : "http");
  set_str(ctx, o, "name", r.name);
  set_str(ctx, o, "id", r.id);
  if (grpc) {
    set_str(ctx, o, "method", r.grpc_method);
    set_str(ctx, o, "url", r.grpc_target.address);
    JS_SetPropertyStr(ctx, o, "headers", pairs_array(ctx, r.grpc_opts.metadata));
    set_str(ctx, o, "body", r.grpc_body);
    set_num(ctx, o, "timeout", static_cast<double>(r.grpc_opts.deadline_ms));
  } else {
    set_str(ctx, o, "method", r.http.method);
    set_str(ctx, o, "url", as_script_sees(r.http.url));
    std::vector<Header> on;
    for (size_t k = 0; k < r.http.headers.size(); ++k)
      if (!r.disabled_headers.count(k)) on.push_back(r.http.headers[k]);
    JS_SetPropertyStr(ctx, o, "headers", pairs_array(ctx, on));
    set_str(ctx, o, "body", as_script_sees(r.http.body));
    set_num(ctx, o, "timeout", static_cast<double>(r.http.timeout_ms));
  }
  return o;
}

// Reads the (possibly edited) request back. False with *err if the script left it invalid.
bool read_req(JSContext* ctx, JSValueConst o, SavedRequest& r, bool& changed, std::string& err) {
  auto str = [&](const char* k, std::string& out) {
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    const bool ok = JS_IsString(v) && to_string(ctx, v, out);
    JS_FreeValue(ctx, v);
    if (!ok) err = std::string("req.") + k + " must be a string";
    return ok;
  };
  std::string method, url, body;
  if (!str("method", method) || !str("url", url) || !str("body", body)) return false;
  std::vector<Header> headers;
  JSValue hv = JS_GetPropertyStr(ctx, o, "headers");
  int64_t n = 0;
  if (!JS_IsArray(hv) || JS_GetLength(ctx, hv, &n) < 0 || n > static_cast<int64_t>(kMaxRequestHeaders)) {
    JS_FreeValue(ctx, hv);
    err = "the request headers must be a list of at most " + std::to_string(kMaxRequestHeaders) + " headers";
    return false;
  }
  for (int64_t i = 0; i < n; ++i) {
    JSValue p = JS_GetPropertyUint32(ctx, hv, static_cast<uint32_t>(i));
    JSValue k = JS_GetPropertyUint32(ctx, p, 0), v = JS_GetPropertyUint32(ctx, p, 1);
    Header h;
    const bool ok = JS_IsString(k) && JS_IsString(v) && to_string(ctx, k, h.first) && to_string(ctx, v, h.second);
    JS_FreeValue(ctx, k);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, p);
    if (!ok) {
      JS_FreeValue(ctx, hv);
      err = "a request header must be a [name, value] pair of strings";
      return false;
    }
    headers.push_back(std::move(h));
  }
  JS_FreeValue(ctx, hv);
  double timeout = 0;
  {
    JSValue t = JS_GetPropertyStr(ctx, o, "timeout");
    JS_ToFloat64(ctx, &timeout, t);
    JS_FreeValue(ctx, t);
  }
  const bool grpc = r.kind == RequestKind::Grpc;
  if (!grpc && !is_http_token(method)) {
    err = "invalid HTTP method \"" + method.substr(0, 50) + "\" set by the script";
    return false;
  }
  for (const auto& [k, v] : headers)
    if (!is_http_token(k) || bad_header_text(v)) {
      err = "invalid header \"" + k.substr(0, 100) + "\" set by the script (a name token, no line breaks in the value)";
      return false;
    }
  if (bad_header_text(url) || url.find(' ') != std::string::npos) {
    err = "the URL set by the script contains spaces or line breaks";
    return false;
  }
  const long tmo = timeout >= 0 && timeout < 1e9 ? static_cast<long>(timeout) : 0;
  if (grpc) {
    if (method != r.grpc_method || url != r.grpc_target.address || body != r.grpc_body || headers != r.grpc_opts.metadata ||
        tmo != r.grpc_opts.deadline_ms) {
      changed = true;
      r.grpc_method = std::move(method);
      r.grpc_target.address = std::move(url);
      r.grpc_body = std::move(body);
      r.grpc_opts.metadata = std::move(headers);
      r.grpc_opts.deadline_ms = tmo;
    }
    return true;
  }
  std::vector<Header> on, off;
  for (size_t k = 0; k < r.http.headers.size(); ++k)
    (r.disabled_headers.count(k) ? off : on).push_back(r.http.headers[k]);
  if (method != r.http.method) {
    r.http.method = std::move(method);
    changed = true;
  }
  if (url != r.http.url && !(has_var_marks(r.http.url) && url == strip_var_marks(r.http.url))) {
    // The editor marked its variables (encode on substitution): a rewritten URL gets the marks
    // back by where each {{name}} now sits, so a value is still encoded for the path / query.
    r.http.url = has_var_marks(r.http.url) ? mark_url_vars(url) : std::move(url);
    changed = true;
  }
  if (body != r.http.body && !(has_var_marks(r.http.body) && body == strip_var_marks(r.http.body))) {
    r.http.body = std::move(body);
    changed = true;
  }
  if (tmo != r.http.timeout_ms) {
    r.http.timeout_ms = tmo;
    changed = true;
  }
  if (headers != on) {
    // The script's headers, then the disabled rows as they were.
    changed = true;
    r.disabled_headers.clear();
    r.http.headers = std::move(headers);
    for (auto& hd : off) {
      r.disabled_headers.insert(r.http.headers.size());
      r.http.headers.push_back(std::move(hd));
    }
  }
  return true;
}

// ------------------------------------------------------------------ the run

void ensure_prelude(JSContext* ctx) {
  PreludeCache& pc = prelude_cache();
  std::call_once(pc.once, [&] {
    for (size_t i = 0; i < detail::kScriptPreludeChunks; ++i) pc.source += detail::kScriptPrelude[i];
    JSValue f = JS_Eval(ctx, pc.source.data(), pc.source.size(), "watt-prelude.js",
                        JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(f)) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      return;  // stays empty: every run then compiles the source (and reports its error)
    }
    size_t n = 0;
    // No source text (Function.prototype.toString) and no debug info: much less to read per run.
    uint8_t* buf = JS_WriteObject(ctx, &n, f, JS_WRITE_OBJ_BYTECODE | JS_WRITE_OBJ_STRIP_SOURCE | JS_WRITE_OBJ_STRIP_DEBUG);
    if (buf) {
      pc.bytecode.assign(buf, buf + n);
      js_free(ctx, buf);
    }
    JS_FreeValue(ctx, f);
  });
}

JSValue load_prelude(JSContext* ctx) {
  ensure_prelude(ctx);
  PreludeCache& pc = prelude_cache();
  if (!pc.bytecode.empty()) {
    JSValue f = JS_ReadObject(ctx, pc.bytecode.data(), pc.bytecode.size(), JS_READ_OBJ_BYTECODE);
    if (JS_IsException(f)) return f;
    return JS_EvalFunction(ctx, f);  // frees f
  }
  return JS_Eval(ctx, pc.source.data(), pc.source.size(), "watt-prelude.js", JS_EVAL_TYPE_GLOBAL);
}

struct Runtime {
  JSRuntime* rt = nullptr;
  JSContext* ctx = nullptr;
  ~Runtime() {
    if (ctx) JS_FreeContext(ctx);
    if (rt) JS_FreeRuntime(rt);
  }
};

// What a chain of scripts (collection, folders, request) shares: the folder / collection vars,
// and what the scripts before this one set (seen by this one's reads).
struct ChainCtx {
  std::map<std::string, std::string> folder_vars, collection_vars;
  const std::vector<ScriptVar>* earlier = nullptr;
  std::string file;  // the script's name in errors and stacks ("": pre-request.js / post-response.js)
};

ScriptRun run_script(const Workspace& ws, bool pre, const std::string& code, const SavedRequest& req,
                     SavedRequest* out_req, ResponseData* rd, const ScriptOptions& opts,
                     const ChainCtx* chain = nullptr) {
  const auto t0 = Clock::now();
  ScriptRun run;
  run.ran = true;
  Host h;
  if (chain) {
    h.folder_vars = &chain->folder_vars;
    h.collection_vars = &chain->collection_vars;
    if (chain->earlier)
      for (const ScriptVar& v : *chain->earlier) {
        if (!v.ok) continue;
        const int s = v.scope == ScriptVar::Scope::Environment ? 1 : v.scope == ScriptVar::Scope::Globals ? 2 : 0;
        h.ov[s][v.name] = v.removed ? std::nullopt : std::optional<std::string>(v.value);
      }
  }
  h.ws = &ws;
  h.run = &run;
  h.opts = &opts;
  h.pre = pre;
  h.rd = rd;
  if (!ws.active_env.empty())
    for (const auto& e : const_cast<Workspace&>(ws).environments())
      if (e.name == ws.active_env) {
        h.env = &e;
        break;
      }
  const ScriptLimits& lim = opts.limits;
  // Setup gets its own allowance (the first run compiles the prelude); the script's starts below.
  h.deadline = t0 + std::chrono::milliseconds(std::max<long>(lim.cpu_ms, 1000) * 2);
  const std::string file = chain && !chain->file.empty() ? chain->file : pre ? "pre-request.js" : "post-response.js";
  {
    Runtime R;
    R.rt = JS_NewRuntime();
    if (!R.rt) {
      run.ok = false;
      run.error = "cannot create the script runtime";
      return run;
    }
    g_initialized.store(true);
    JS_SetMemoryLimit(R.rt, lim.memory_bytes);
    JS_SetMaxStackSize(R.rt, lim.stack_bytes);
    JS_SetInterruptHandler(R.rt, interrupt_handler, &h);
    JS_SetModuleLoaderFunc(R.rt, nullptr, no_modules, nullptr);
    JS_SetHostPromiseRejectionTracker(R.rt, on_unhandled_rejection, nullptr);
    JS_SetCanBlock(R.rt, false);
    R.ctx = JS_NewContext(R.rt);
    if (!R.ctx) {
      run.ok = false;
      run.error = "cannot create the script context";
      return run;
    }
    JSContext* ctx = R.ctx;
    JS_SetContextOpaque(ctx, &h);

    JSValue fn = load_prelude(ctx);
    JSValue hostobj = JS_NewObject(ctx);
    for (const auto& n : kNatives) JS_SetPropertyStr(ctx, hostobj, n.name, JS_NewCFunction(ctx, n.fn, n.name, n.len));
    JSValue reqobj = make_req(ctx, req);
    JSValue api = JS_UNDEFINED;
    if (JS_IsException(fn)) {
      JSValue e = JS_GetException(ctx);
      set_error(ctx, h, e, "watt-prelude.js");
      JS_FreeValue(ctx, e);
      run.error = "script API failed to load: " + run.error;
    } else {
      JSValue args[3] = {hostobj, reqobj, new_string(ctx, pre ? "pre" : "post")};
      api = JS_Call(ctx, fn, JS_UNDEFINED, 3, args);
      JS_FreeValue(ctx, args[2]);
      if (JS_IsException(api)) {
        JSValue e = JS_GetException(ctx);
        set_error(ctx, h, e, "watt-prelude.js");
        JS_FreeValue(ctx, e);
        run.error = "script API failed to load: " + run.error;
        api = JS_UNDEFINED;
      }
    }
    JS_FreeValue(ctx, fn);
    run.setup_us = us_since(t0);

    if (run.ok) {
      h.deadline = Clock::now() + std::chrono::milliseconds(lim.cpu_ms);
      JS_UpdateStackTop(R.rt);
      // Global code with top-level await allowed (await pm.sendRequest(...)): the result is a promise.
      JSValue result = JS_Eval(ctx, code.data(), code.size(), file.c_str(), JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_ASYNC);
      if (JS_IsException(result)) {
        JSValue e = JS_GetException(ctx);
        set_error(ctx, h, e, file);
        JS_FreeValue(ctx, e);
      } else {
        // Promise jobs (async tests, awaits), then timers in delay order, until nothing is left.
        JSValue drain = JS_GetPropertyStr(ctx, api, "drain");
        for (bool more = true; more && run.ok;) {
          JSContext* jctx = nullptr;
          int jr;
          while ((jr = JS_ExecutePendingJob(R.rt, &jctx)) > 0) {
          }
          if (jr < 0) {
            JSValue e = JS_GetException(jctx ? jctx : ctx);
            set_error(ctx, h, e, file);
            JS_FreeValue(ctx, e);
            break;
          }
          if (JS_IsPromise(result) && JS_PromiseState(ctx, result) == JS_PROMISE_REJECTED) break;
          JSValue d = JS_Call(ctx, drain, JS_UNDEFINED, 0, nullptr);
          if (JS_IsException(d)) {
            JSValue e = JS_GetException(ctx);
            set_error(ctx, h, e, file);
            JS_FreeValue(ctx, e);
            break;
          }
          more = JS_ToBool(ctx, d) > 0;
          JS_FreeValue(ctx, d);
        }
        JS_FreeValue(ctx, drain);
        if (run.ok && JS_IsPromise(result) && JS_PromiseState(ctx, result) == JS_PROMISE_REJECTED) {
          JSValue e = JS_PromiseResult(ctx, result);
          set_error(ctx, h, e, file);
          JS_FreeValue(ctx, e);
        }
      }
      flush_rejections(ctx, h, result);
      JS_FreeValue(ctx, result);
      if (run.ok && pre && out_req) {
        std::string err;
        SavedRequest edited = *out_req;
        bool changed = false;
        if (!read_req(ctx, reqobj, edited, changed, err)) {
          run.ok = false;
          run.error = err;
        } else if (changed) {
          *out_req = std::move(edited);
          run.request_changed = true;
        }
      }
    }
    flush_rejections(ctx, h, JS_UNDEFINED);
    JS_FreeValue(ctx, api);
    JS_FreeValue(ctx, reqobj);
    JS_FreeValue(ctx, hostobj);
    // The runtime (every object the script made) goes away here.
  }
  run.elapsed_us = us_since(t0);
  return run;
}

ScriptRun untrusted_run() {
  ScriptRun run;
  run.untrusted = true;
  run.ok = false;
  run.error =
      "scripts in this workspace are not trusted yet: they run only after you trust this workspace's scripts "
      "(they are code from whoever wrote the workspace)";
  return run;
}

// The script file name of a level, as errors and stack traces show it.
std::string chain_file(const EffectiveSettings::Script& s, bool pre) {
  const char* base = pre ? "pre-request.js" : "post-response.js";
  switch (s.from.level) {
    case SettingsOrigin::Level::Request: return base;
    case SettingsOrigin::Level::Collection: return std::string("collection/") + base;
    case SettingsOrigin::Level::Folder: break;
  }
  return s.from.label + "/" + base;
}

// Runs the scripts of every level in order (core/model.h, FolderSettings): each one sees what
// the ones before it set and (pre-request) the request as they left it. One ScriptRun for them
// all: logs, tests and variables in order, times summed; the first error stops the chain.
ScriptRun run_chain(const Workspace& ws, bool pre, const SavedRequest& req, SavedRequest* out_req, ResponseData* rd,
                    const ScriptOptions& opts) {
  // No folder.json on the way: only the request's own script (and nothing at all without one:
  // no trust file read, no QuickJS).
  if (!ws.has_inherited_settings(req)) {
    const std::string& own = pre ? req.pre_script : req.post_script;
    if (own.empty()) return {};
    if (!ws.scripts_trusted()) return untrusted_run();
    if (pre) {
      const SavedRequest original = req;
      return run_script(ws, true, original.pre_script, original, out_req, nullptr, opts);
    }
    return run_script(ws, false, own, req, nullptr, rd, opts);
  }
  const EffectiveSettings eff = ws.effective_settings(req);
  const auto& scripts = pre ? eff.pre_scripts : eff.post_scripts;
  if (scripts.empty()) return {};
  // Inherited scripts are workspace code like the request's own: the same trust decision.
  if (!ws.scripts_trusted()) return untrusted_run();
  ChainCtx ctx;
  for (const auto& v : eff.vars) {
    if (!v.row.enabled) continue;
    if (v.from.level == SettingsOrigin::Level::Folder) ctx.folder_vars.emplace(v.row.name, v.row.value);
    else if (v.from.level == SettingsOrigin::Level::Collection) ctx.collection_vars.emplace(v.row.name, v.row.value);
  }
  ScriptRun total;
  ctx.earlier = &total.vars_set;
  for (const auto& sc : scripts) {
    ctx.file = chain_file(sc, pre);
    ScriptRun one;
    if (pre) {
      const SavedRequest current = *out_req;
      one = run_script(ws, true, sc.code, current, out_req, nullptr, opts, &ctx);
    } else {
      one = run_script(ws, false, sc.code, req, nullptr, rd, opts, &ctx);
    }
    total.ran = total.ran || one.ran;
    total.request_changed = total.request_changed || one.request_changed;
    total.logs_truncated = total.logs_truncated || one.logs_truncated;
    total.requests_sent += one.requests_sent;
    total.elapsed_us += one.elapsed_us;
    total.setup_us += one.setup_us;
    total.wait_us += one.wait_us;
    for (auto& l : one.logs) total.logs.push_back(std::move(l));
    for (auto& t : one.tests) total.tests.push_back(std::move(t));
    for (auto& v : one.vars_set) total.vars_set.push_back(std::move(v));
    // The last pm.visualizer.set / clear of the chain wins.
    if (one.visualizer || one.visualizer_cleared) {
      total.visualizer = one.visualizer;
      total.visualizer_cleared = one.visualizer_cleared;
      total.visualizer_template = std::move(one.visualizer_template);
      total.visualizer_data = std::move(one.visualizer_data);
      total.visualizer_error = std::move(one.visualizer_error);
    }
    if (!one.ok) {
      total.ok = false;
      total.line = one.line;
      total.column = one.column;
      total.error = sc.from.level == SettingsOrigin::Level::Request ? one.error : ctx.file + ": " + one.error;
      break;
    }
  }
  return total;
}

}  // namespace

ScriptRun run_pre_script(const Workspace& ws, SavedRequest& req, const ScriptOptions& opts) {
  return run_chain(ws, true, req, &req, nullptr, opts);
}

ScriptRun run_post_script(const Workspace& ws, const SavedRequest& req, ResponseData& res, const ScriptOptions& opts) {
  return run_chain(ws, false, req, nullptr, &res, opts);
}

bool apply_script_vars(Workspace& ws, ScriptRun& run, bool persist, std::string* err) {
  Environment* env = nullptr;
  if (!ws.active_env.empty())
    for (auto& e : ws.environments())
      if (e.name == ws.active_env) {
        env = &e;
        break;
      }
  bool dirty = false;
  for (ScriptVar& v : run.vars_set) {
    if (!v.ok) continue;
    if (v.value.find("{{") != std::string::npos || v.name.empty() || v.name.size() > kMaxVarName) {
      v.ok = false;
      v.message = "refused";
      continue;
    }
    std::map<std::string, std::string>* m = nullptr;
    switch (v.scope) {
      case ScriptVar::Scope::Runtime: m = &ws.runtime_vars; break;
      case ScriptVar::Scope::Globals: m = &ws.globals; break;
      case ScriptVar::Scope::Environment:
        if (!env) {
          v.ok = false;
          v.message = "no environment is active";
          continue;
        }
        if (env->secrets.count(v.name) || env->unreadable_secrets.count(v.name)) {
          v.ok = false;
          v.message = "it is a secret variable (secrets are never overwritten by scripts)";
          continue;
        }
        m = &env->vars;
        break;
    }
    if (v.removed) {
      if (m->erase(v.name) && v.scope != ScriptVar::Scope::Runtime) dirty = true;
    } else {
      auto it = m->find(v.name);
      if (it == m->end() || it->second != v.value) {
        (*m)[v.name] = v.value;
        if (v.scope != ScriptVar::Scope::Runtime) dirty = true;
      }
    }
  }
  if (dirty && persist) return ws.save_settings(err);
  return true;
}

bool script_engine_initialized() { return g_initialized.load(); }

}  // namespace watt
