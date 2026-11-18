#include "core/asserts.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include <rapidjson/document.h>
#include <re2/re2.h>

#include "core/model.h"

namespace watt {

namespace {

using Value = rapidjson::Value;
using Clock = std::chrono::steady_clock;
constexpr unsigned kIterative = rapidjson::kParseIterativeFlag;  // no recursion on deep input
constexpr size_t kShowBytes = 200;                               // AssertResult::actual

// ------------------------------------------------------------------ small helpers

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (lower(a[i]) != lower(b[i])) return false;
  return true;
}

std::string_view trim(std::string_view s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
  return s.substr(a, b - a);
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// JSON-like numbers only ("inf", "nan", "0x1" are not numbers); a leading '+' is allowed.
bool parse_number(std::string_view s, double& out) {
  if (s.empty() || s.size() > 64) return false;
  if (s[0] == '+') s.remove_prefix(1);
  const size_t i = (!s.empty() && s[0] == '-') ? 1 : 0;
  if (i >= s.size()) return false;
  if (!(is_digit(s[i]) || (s[i] == '.' && i + 1 < s.size() && is_digit(s[i + 1])))) return false;
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
  return ec == std::errc() && p == s.data() + s.size();
}

std::string fmt_num(double x) {
  if (x == static_cast<double>(static_cast<long long>(x)) && x > -1e15 && x < 1e15)
    return std::to_string(static_cast<long long>(x));
  char buf[64];
  const auto [p, ec] = std::to_chars(buf, buf + sizeof buf, x);
  return ec == std::errc() ? std::string(buf, p) : std::string("NaN");
}

size_t utf8_length(std::string_view s) {
  size_t n = 0;
  for (const char c : s) n += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
  return n;
}

void cut_utf8(std::string& s, size_t limit) {
  if (s.size() <= limit) return;
  size_t n = limit;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
  s.resize(n);
  s += "...";
}

void json_escape(std::string& out, std::string_view s) {
  out += '"';
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          static const char hex[] = "0123456789abcdef";
          out += "\\u00";
          out += hex[c >> 4];
          out += hex[c & 15];
        } else {
          out += ch;
        }
    }
  }
  out += '"';
}

// Compact JSON of v. False when v is nested deeper than kMaxJsonDepth. Stops early (with
// *cut set) once out exceeds limit.
bool write_json(const Value& v, std::string& out, size_t limit, bool* cut, int depth = 0) {
  if (depth > kMaxJsonDepth) return false;
  if (out.size() > limit) {
    if (cut) *cut = true;
    return true;
  }
  switch (v.GetType()) {
    case rapidjson::kNullType: out += "null"; break;
    case rapidjson::kFalseType: out += "false"; break;
    case rapidjson::kTrueType: out += "true"; break;
    case rapidjson::kStringType: json_escape(out, std::string_view(v.GetString(), v.GetStringLength())); break;
    case rapidjson::kNumberType:
      if (v.IsInt64()) out += std::to_string(v.GetInt64());
      else if (v.IsUint64()) out += std::to_string(v.GetUint64());
      else out += fmt_num(v.GetDouble());
      break;
    case rapidjson::kArrayType: {
      out += '[';
      bool first = true;
      for (const auto& e : v.GetArray()) {
        if (!first) out += ',';
        first = false;
        if (!write_json(e, out, limit, cut, depth + 1)) return false;
        if (out.size() > limit) break;
      }
      out += ']';
      break;
    }
    case rapidjson::kObjectType: {
      out += '{';
      bool first = true;
      for (const auto& m : v.GetObject()) {
        if (!first) out += ',';
        first = false;
        json_escape(out, std::string_view(m.name.GetString(), m.name.GetStringLength()));
        out += ':';
        if (!write_json(m.value, out, limit, cut, depth + 1)) return false;
        if (out.size() > limit) break;
      }
      out += '}';
      break;
    }
  }
  if (out.size() > limit && cut) *cut = true;
  return true;
}

// Iterative depth check (rapidjson's deep copy is recursive).
bool deeper_than(const Value& root, int max) {
  std::vector<std::pair<const Value*, int>> stack{{&root, 0}};
  while (!stack.empty()) {
    auto [v, d] = stack.back();
    stack.pop_back();
    if (d > max) return true;
    if (v->IsArray())
      for (const auto& e : v->GetArray()) stack.emplace_back(&e, d + 1);
    else if (v->IsObject())
      for (const auto& m : v->GetObject()) stack.emplace_back(&m.value, d + 1);
  }
  return false;
}

// -1: too deep.
int json_equal(const Value& a, const Value& b, int depth = 0) {
  if (depth > kMaxJsonDepth) return -1;
  if (a.IsNumber() && b.IsNumber()) {
    if (a.IsInt64() && b.IsInt64()) return a.GetInt64() == b.GetInt64();
    return a.GetDouble() == b.GetDouble();
  }
  if (a.GetType() != b.GetType()) return 0;
  switch (a.GetType()) {
    case rapidjson::kStringType:
      return a.GetStringLength() == b.GetStringLength() &&
             std::memcmp(a.GetString(), b.GetString(), a.GetStringLength()) == 0;
    case rapidjson::kArrayType: {
      if (a.Size() != b.Size()) return 0;
      for (rapidjson::SizeType i = 0; i < a.Size(); ++i) {
        const int r = json_equal(a[i], b[i], depth + 1);
        if (r != 1) return r;
      }
      return 1;
    }
    case rapidjson::kObjectType: {
      if (a.MemberCount() != b.MemberCount()) return 0;
      for (const auto& m : a.GetObject()) {
        auto it = b.FindMember(m.name);
        if (it == b.MemberEnd()) return 0;
        const int r = json_equal(m.value, it->value, depth + 1);
        if (r != 1) return r;
      }
      return 1;
    }
    default:
      return 1;  // null / true / false: same type
  }
}

// Skips a UTF-8 BOM and whitespace; whether the text can start a JSON value at all (a cheap
// reject for HTML / text bodies before the parser runs).
std::string_view json_start(std::string_view s) {
  if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB &&
      static_cast<unsigned char>(s[2]) == 0xBF)
    s.remove_prefix(3);
  s = trim(s);
  if (s.empty() || std::string_view("{[\"-0123456789tfn").find(s[0]) == std::string_view::npos) return {};
  return s;
}

// Parses a copy of text (only for small texts: values, isJson on strings).
std::unique_ptr<rapidjson::Document> parse_copy(std::string_view text) {
  text = json_start(text);
  if (text.empty()) return nullptr;
  auto d = std::make_unique<rapidjson::Document>();
  d->Parse<kIterative>(text.data(), text.size());
  if (d->HasParseError()) return nullptr;
  return d;
}

// ------------------------------------------------------------------ compiled targets

enum class Root {
  Status, StatusText, StatusMessage, ResponseTime, Timing, Size, HeadersSize, Url,
  Headers, Trailers, Metadata, Body, Messages,
};

struct Seg {
  bool index = false;  // [n]
  std::string key;     // .key / ["key"]
  long long n = 0;
};

struct Path {
  Root root = Root::Body;
  std::vector<Seg> segs;  // after the root (for timing: segs[0] is the phase)
  std::string error;      // non-empty: invalid
};

const struct {
  const char* name;
  Root root;
} kRoots[] = {
    {"status", Root::Status},           {"statusText", Root::StatusText},
    {"statusMessage", Root::StatusMessage}, {"responseTime", Root::ResponseTime},
    {"timing", Root::Timing},           {"size", Root::Size},
    {"headersSize", Root::HeadersSize}, {"url", Root::Url},
    {"headers", Root::Headers},         {"trailers", Root::Trailers},
    {"metadata", Root::Metadata},       {"body", Root::Body},
    {"messages", Root::Messages},
};

const char* const kPhases[] = {"queue", "dns", "connect", "tls", "send", "ttfb", "download", "total",
                               "prepare", "socket", "redirect", "process", "reflect"};

// Short forms (also used by watt's response view): status, body, time, time:<phase>,
// header:<name>, trailer:<name>, json:<path> / json:$.<path> -> the res.* target.
std::string expand_short(std::string_view s) {
  auto starts = [&](std::string_view p) { return s.size() >= p.size() && iequals(s.substr(0, p.size()), p); };
  auto quoted = [](std::string_view name) {
    std::string q = "[\"";
    for (const char c : name) {
      if (c == '"' || c == '\\') q += '\\';
      q += c;
    }
    return q + "\"]";
  };
  if (iequals(s, "status")) return "res.status";
  if (iequals(s, "body")) return "res.body";
  if (iequals(s, "time")) return "res.responseTime";
  if (starts("time:")) return "res.timing." + std::string(s.substr(5));
  if (starts("header:")) return "res.headers" + quoted(trim(s.substr(7)));
  if (starts("trailer:")) return "res.trailers" + quoted(trim(s.substr(8)));
  if (starts("json:")) {
    std::string_view j = trim(s.substr(5));
    if (!j.empty() && j[0] == '$') j.remove_prefix(1);
    if (!j.empty() && j[0] != '.' && j[0] != '[') return "res.body." + std::string(j);
    return "res.body" + std::string(j);
  }
  return std::string(s);
}

std::shared_ptr<const Path> compile_path(std::string_view target) {
  auto p = std::make_shared<Path>();
  auto fail = [&](std::string msg) {
    p->error = std::move(msg);
    p->segs.clear();
    return p;
  };
  if (trim(target).size() > kMaxTargetBytes) return fail("target longer than " + std::to_string(kMaxTargetBytes) + " bytes");
  const std::string full = expand_short(trim(target));
  const std::string_view s = full;
  if (s.size() < 4 || s.substr(0, 4) != "res.") return fail("a target starts with res. (res.status, res.body.id, ...)");
  std::vector<Seg> segs;
  size_t i = 3;
  while (i < s.size()) {
    if (segs.size() > kMaxSegments) return fail("target has more than " + std::to_string(kMaxSegments) + " segments");
    const char c = s[i];
    if (c == '.') {
      const size_t start = ++i;
      while (i < s.size() && s[i] != '.' && s[i] != '[') {
        const auto u = static_cast<unsigned char>(s[i]);
        if (u < 0x20 || u == ' ' || u == ']') return fail("unexpected character in name at offset " + std::to_string(i));
        ++i;
      }
      if (i == start) return fail("empty name at offset " + std::to_string(start));
      segs.push_back(Seg{false, std::string(s.substr(start, i - start)), 0});
    } else if (c == '[') {
      ++i;
      while (i < s.size() && s[i] == ' ') ++i;
      if (i >= s.size()) return fail("unterminated [");
      Seg seg;
      if (s[i] == '"' || s[i] == '\'') {
        const char q = s[i++];
        bool closed = false;
        while (i < s.size()) {
          char ch = s[i++];
          if (ch == q) {
            closed = true;
            break;
          }
          if (ch == '\\' && i < s.size()) ch = s[i++];
          seg.key += ch;
        }
        if (!closed) return fail("unterminated string in []");
      } else {
        const size_t start = i;
        if (s[i] == '-') ++i;
        while (i < s.size() && is_digit(s[i])) ++i;
        const std::string_view num = s.substr(start, i - start);
        if (num.empty() || num == "-" || num.size() > 18)
          return fail("expected an index or a quoted key in [] at offset " + std::to_string(start));
        std::from_chars(num.data(), num.data() + num.size(), seg.n);
        seg.index = true;
      }
      while (i < s.size() && s[i] == ' ') ++i;
      if (i >= s.size() || s[i] != ']') return fail("expected ] at offset " + std::to_string(i));
      ++i;
      segs.push_back(std::move(seg));
    } else {
      return fail(std::string("unexpected '") + c + "' at offset " + std::to_string(i));
    }
  }
  if (segs.empty() || segs[0].index) return fail("expected a name after res.");
  const std::string& root = segs[0].key;
  bool found = false;
  for (const auto& r : kRoots)
    if (root == r.name) {
      p->root = r.root;
      found = true;
      break;
    }
  if (!found)
    return fail("unknown target res." + root +
                " (status, statusText, statusMessage, responseTime, timing, size, headersSize, url, headers, "
                "trailers, metadata, body, messages)");
  segs.erase(segs.begin());
  if (p->root == Root::Timing && !segs.empty() && !segs[0].index) {
    for (auto& c : segs[0].key) c = lower(c);
    if (segs[0].key == "wait") segs[0].key = "ttfb";
  }
  if (p->root == Root::Timing) {
    if (segs.empty() || segs[0].index ||
        std::none_of(std::begin(kPhases), std::end(kPhases), [&](const char* ph) { return segs[0].key == ph; }))
      return fail("res.timing.<phase>: queue, dns, connect, tls, send, ttfb, download, total, prepare, socket, "
                  "redirect, process, reflect");
  }
  if ((p->root == Root::Headers || p->root == Root::Trailers || p->root == Root::Metadata) && !segs.empty() &&
      segs[0].index)
    return fail("res." + root + "[n]: use a header name, res." + root + "[\"Name\"]");
  p->segs = std::move(segs);
  return p;
}

template <class T>
struct Cache {
  std::mutex m;
  std::unordered_map<std::string, std::shared_ptr<const T>> map;
};

std::shared_ptr<const Path> path_for(const std::string& target) {
  static Cache<Path> cache;
  {
    std::lock_guard lock(cache.m);
    if (auto it = cache.map.find(target); it != cache.map.end()) return it->second;
  }
  auto p = compile_path(target);
  std::lock_guard lock(cache.m);
  if (cache.map.size() >= 4096) cache.map.clear();  // bounded: a workspace has far fewer
  if (target.size() <= kMaxTargetBytes) cache.map.emplace(target, p);
  return p;
}

struct Regex {
  std::unique_ptr<RE2> re;
  std::string error;
};

std::shared_ptr<const Regex> regex_for(std::string_view value) {
  static Cache<Regex> cache;
  const std::string key(value);
  {
    std::lock_guard lock(cache.m);
    if (auto it = cache.map.find(key); it != cache.map.end()) return it->second;
  }
  auto rx = std::make_shared<Regex>();
  std::string_view pat = value;
  std::string flags;
  if (pat.size() >= 2 && pat[0] == '/') {
    const size_t close = pat.rfind('/');
    const std::string_view f = pat.substr(close + 1);
    if (close > 0 && f.find_first_not_of("imsgu") == std::string_view::npos) {
      for (const char c : f)
        if ((c == 'i' || c == 'm' || c == 's') && flags.find(c) == std::string::npos) flags += c;
      pat = pat.substr(1, close - 1);
    }
  }
  if (pat.size() > kMaxRegexBytes) {
    rx->error = "pattern longer than " + std::to_string(kMaxRegexBytes) + " bytes";
  } else {
    RE2::Options opt;
    opt.set_log_errors(false);
    opt.set_max_mem(kMaxRegexMem);
    const std::string full = flags.empty() ? std::string(pat) : "(?" + flags + ")" + std::string(pat);
    rx->re = std::make_unique<RE2>(full, opt);
    if (!rx->re->ok()) {
      rx->error = "invalid pattern: " + rx->re->error();
      rx->re.reset();
    } else if (rx->re->ProgramSize() > kMaxRegexProgram) {
      rx->error = "pattern too complex (program size " + std::to_string(rx->re->ProgramSize()) + " > " +
                  std::to_string(kMaxRegexProgram) + ")";
      rx->re.reset();
    }
  }
  std::lock_guard lock(cache.m);
  if (cache.map.size() >= 1024) cache.map.clear();
  if (key.size() <= kMaxRegexBytes + 16) cache.map.emplace(key, rx);
  return rx;
}

// ------------------------------------------------------------------ values

struct Val {
  enum Kind { Undefined, Null, Bool, Number, String, Json } kind = Undefined;  // Json: object / array
  bool b = false;
  double num = 0;
  std::string_view str;
  const Value* json = nullptr;
  std::string_view status_name;  // gRPC res.status
};

Val from_json(const Value& v) {
  Val r;
  switch (v.GetType()) {
    case rapidjson::kNullType: r.kind = Val::Null; break;
    case rapidjson::kFalseType: r.kind = Val::Bool; break;
    case rapidjson::kTrueType: r.kind = Val::Bool; r.b = true; break;
    case rapidjson::kNumberType: r.kind = Val::Number; r.num = v.GetDouble(); break;
    case rapidjson::kStringType: r.kind = Val::String; r.str = {v.GetString(), v.GetStringLength()}; break;
    default: r.kind = Val::Json; r.json = &v;
  }
  return r;
}

Val num_val(double x) {
  Val v;
  v.kind = Val::Number;
  v.num = x;
  return v;
}

Val str_val(std::string_view s) {
  Val v;
  v.kind = Val::String;
  v.str = s;
  return v;
}

// Text of a value: strings as-is, JSON compact (false: too deep). limit cuts JSON early.
bool val_text(const Val& v, std::string& out, size_t limit = SIZE_MAX, bool* cut = nullptr) {
  switch (v.kind) {
    case Val::Undefined: out = "undefined"; return true;
    case Val::Null: out = "null"; return true;
    case Val::Bool: out = v.b ? "true" : "false"; return true;
    case Val::Number: out = fmt_num(v.num); return true;
    case Val::String: out.assign(v.str); return true;
    case Val::Json: out.clear(); return write_json(*v.json, out, limit, cut);
  }
  return true;
}

std::string show(const Val& v) {
  std::string s;
  if (!val_text(v, s, kShowBytes + 8)) return "(JSON nested too deeply)";
  if (v.kind == Val::String && !v.status_name.empty()) s = std::string(v.status_name);
  if (v.kind == Val::Number && !v.status_name.empty()) s = std::string(v.status_name) + " (" + s + ")";
  cut_utf8(s, kShowBytes);
  return s;
}

}  // namespace

// ------------------------------------------------------------------ ResponseData

struct ResponseData::Impl {
  const HttpResponse* http = nullptr;
  const GrpcResponse* grpc = nullptr;
  struct Parsed {
    bool tried = false, ok = false;
    std::unique_ptr<char[]> buf;  // in-situ: the document's strings point into it
    std::unique_ptr<rapidjson::Document> doc;
  };
  std::vector<std::unique_ptr<Parsed>> parsed;  // HTTP: [0] = body; gRPC: one per message
  std::unique_ptr<rapidjson::Document> headers_doc, trailers_doc, messages_doc;
  bool messages_too_deep = false;
  double parse_us = 0;

  size_t text_count() const { return http ? 1 : grpc->messages.size(); }
  std::string_view text(size_t i) const {
    return http ? std::string_view(http->body) : std::string_view(grpc->messages[i].json);
  }

  // The parsed JSON of body / message i, or null when it is not JSON.
  const Value* json(size_t i) {
    if (i >= text_count()) return nullptr;
    if (parsed.size() < text_count()) parsed.resize(text_count());
    auto& p = parsed[i];
    if (!p) p = std::make_unique<Parsed>();
    if (!p->tried) {
      p->tried = true;
      const auto t0 = Clock::now();
      const std::string_view t = json_start(text(i));
      if (!t.empty()) {
        p->buf.reset(new char[t.size() + 1]);
        std::memcpy(p->buf.get(), t.data(), t.size());
        p->buf[t.size()] = '\0';
        p->doc = std::make_unique<rapidjson::Document>();
        p->doc->ParseInsitu<kIterative>(p->buf.get());
        p->ok = !p->doc->HasParseError();
        if (!p->ok) p->buf.reset();
      }
      parse_us += std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
    }
    return p->ok ? p->doc.get() : nullptr;
  }

  Val message(size_t i) {
    if (const Value* v = json(i)) return from_json(*v);
    return str_val(text(i));
  }

  static std::unique_ptr<rapidjson::Document> meta_doc(const std::vector<std::pair<std::string, std::string>>& m) {
    auto d = std::make_unique<rapidjson::Document>();
    d->SetObject();
    auto& a = d->GetAllocator();
    for (const auto& [k, v] : m)
      d->AddMember(Value(k.c_str(), static_cast<rapidjson::SizeType>(k.size()), a),
                   Value(v.c_str(), static_cast<rapidjson::SizeType>(v.size()), a), a);
    return d;
  }

  const Value& headers_json() {
    if (!headers_doc) {
      if (http) {
        headers_doc = std::make_unique<rapidjson::Document>();
        headers_doc->SetObject();
        auto& a = headers_doc->GetAllocator();
        for (const auto& [k, v] : http->headers)
          headers_doc->AddMember(Value(k.data(), static_cast<rapidjson::SizeType>(k.size()), a),
                                 Value(v.data(), static_cast<rapidjson::SizeType>(v.size()), a), a);
      } else {
        headers_doc = meta_doc(grpc->headers);
      }
    }
    return *headers_doc;
  }

  const Value& trailers_json() {
    if (!trailers_doc) trailers_doc = meta_doc(grpc ? grpc->trailers : Metadata{});
    return *trailers_doc;
  }

  const Value* messages_json() {
    if (!messages_doc && !messages_too_deep) {
      auto d = std::make_unique<rapidjson::Document>();
      d->SetArray();
      auto& a = d->GetAllocator();
      for (size_t i = 0; i < text_count(); ++i) {
        if (const Value* v = json(i)) {
          if (deeper_than(*v, kMaxJsonDepth)) {
            messages_too_deep = true;
            return nullptr;
          }
          d->PushBack(Value(*v, a), a);
        } else {
          const std::string_view t = text(i);
          d->PushBack(Value(t.data(), static_cast<rapidjson::SizeType>(t.size()), a), a);
        }
      }
      messages_doc = std::move(d);
    }
    return messages_doc.get();
  }
};

ResponseData::ResponseData(const HttpResponse& r) : impl_(std::make_unique<Impl>()) { impl_->http = &r; }
ResponseData::ResponseData(const GrpcResponse& r) : impl_(std::make_unique<Impl>()) { impl_->grpc = &r; }
ResponseData::~ResponseData() = default;
double ResponseData::parse_us() const { return impl_->parse_us; }
const HttpResponse* ResponseData::http() const { return impl_->http; }
const GrpcResponse* ResponseData::grpc() const { return impl_->grpc; }
const rapidjson::Value* ResponseData::json(size_t i) const { return impl_->json(i); }

namespace {

bool meta_find(const Metadata& m, std::string_view name, std::string_view& out) {
  for (const auto& [k, v] : m)
    if (iequals(k, name)) {
      out = v;
      return true;
    }
  return false;
}

// Applies segs[from..] to v.
Val walk(Val v, const std::vector<Seg>& segs, size_t from) {
  for (size_t k = from; k < segs.size(); ++k) {
    const Seg& s = segs[k];
    if (v.kind == Val::Json) {
      const Value& j = *v.json;
      if (j.IsObject()) {
        const std::string key = s.index ? std::to_string(s.n) : s.key;
        auto it = j.FindMember(Value(rapidjson::StringRef(key.data(), static_cast<rapidjson::SizeType>(key.size()))));
        if (it != j.MemberEnd()) v = from_json(it->value);
        else if (!s.index && s.key == "length") v = num_val(j.MemberCount());
        else return {};
      } else {  // array
        long long n = s.n;
        if (!s.index) {
          if (s.key == "length") {
            v = num_val(j.Size());
            continue;
          }
          double d;
          if (!parse_number(s.key, d) || s.key.find_first_not_of("0123456789") != std::string::npos) return {};
          n = static_cast<long long>(d);
        }
        const auto size = static_cast<long long>(j.Size());
        if (n < 0) n += size;
        if (n < 0 || n >= size) return {};
        v = from_json(j[static_cast<rapidjson::SizeType>(n)]);
      }
    } else if (v.kind == Val::String && !s.index && s.key == "length") {
      v = num_val(static_cast<double>(utf8_length(v.str)));
      v.status_name = {};
    } else {
      return {};
    }
  }
  return v;
}

// Resolves a compiled target; *why says why it is undefined when that is not obvious.
Val resolve(const Path& p, ResponseData::Impl& d, std::string* why) {
  const HttpResponse* h = d.http;
  const GrpcResponse* g = d.grpc;
  const Timing& t = h ? h->timing : g->timing;
  Val v;
  size_t from = 0;
  switch (p.root) {
    case Root::Status:
      v = num_val(h ? h->status : g->status_code);
      if (g) v.status_name = g->status_name;
      break;
    case Root::StatusText: v = str_val(h ? h->status_text : g->status_name); break;
    case Root::StatusMessage: v = str_val(h ? h->error : g->status_message); break;
    case Root::ResponseTime:
      // A user-paced stream: open -> trailers is the user's pace, not a latency.
      v = num_val(g && g->pacing.on && g->pacing.tail_ms >= 0 ? g->pacing.tail_ms : t.total_ms);
      break;
    case Root::Timing: {
      const std::string& ph = p.segs[0].key;
      from = 1;
      double x = 0;
      bool have = true;
      if (ph == "queue") x = t.queue_ms;
      else if (ph == "dns") x = t.dns_ms;
      else if (ph == "connect") x = t.connect_ms;
      else if (ph == "tls") x = t.tls_ms;
      else if (ph == "send") x = t.send_ms;
      else if (ph == "ttfb") x = t.ttfb_ms;
      else if (ph == "download") x = t.download_ms;
      else if (ph == "total") x = t.total_ms;
      else if (ph == "reflect") have = g != nullptr, x = g ? g->reflect_ms : 0;
      else if (!h) have = false;
      else if (ph == "prepare") x = h->prepare_ms;
      else if (ph == "socket") x = h->socket_ms;
      else if (ph == "redirect") x = h->redirect_ms;
      else if (ph == "process") x = h->process_ms;
      if (!have) {
        if (why) *why = "res.timing." + ph + " is not measured for " + (h ? "HTTP" : "gRPC");
        return {};
      }
      v = num_val(x);
      break;
    }
    case Root::Size:
      if (h) {
        v = num_val(static_cast<double>(h->sizes.response_body));
      } else {
        double n = 0;
        for (const auto& m : g->messages) n += static_cast<double>(m.wire_bytes);
        v = num_val(n);
      }
      break;
    case Root::HeadersSize:
      if (!h) return {};
      v = num_val(static_cast<double>(h->sizes.response_headers));
      break;
    case Root::Url:
      if (!h) return {};
      v = str_val(h->effective_url);
      break;
    case Root::Headers:
    case Root::Metadata:
    case Root::Trailers: {
      if (p.root != Root::Headers && h) {
        if (why) *why = "res.trailers / res.metadata are gRPC targets; use res.headers";
        return {};
      }
      if (p.segs.empty()) {
        v = from_json(p.root == Root::Trailers ? d.trailers_json() : d.headers_json());
        break;
      }
      std::string_view hv;
      const std::string& name = p.segs[0].key;
      bool found;
      if (h && p.root == Root::Headers) {  // ResponseHeaders::find lives in watt_http
        found = false;
        for (const auto& [k, val] : h->headers)
          if (iequals(k, name)) {
            hv = val;
            found = true;
            break;
          }
      }
      else
        found = meta_find(p.root == Root::Trailers ? g->trailers : g->headers, name, hv);
      if (!found) return {};
      v = str_val(hv);
      from = 1;
      break;
    }
    case Root::Body:
      if (g && g->messages.empty()) {
        if (why) *why = "the call returned no message";
        return {};
      }
      if (const Value* j = d.json(0)) {
        v = from_json(*j);
      } else {
        if (!p.segs.empty()) {
          if (why) *why = "the body is not JSON";
          return {};
        }
        v = str_val(d.text(0));
      }
      break;
    case Root::Messages: {
      if (!g) {
        if (why) *why = "res.messages is a gRPC target; use res.body";
        return {};
      }
      if (p.segs.empty()) {
        const Value* j = d.messages_json();
        if (!j) {
          if (why) *why = "a message is nested more than " + std::to_string(kMaxJsonDepth) + " levels deep";
          return {};
        }
        v = from_json(*j);
        break;
      }
      const Seg& s = p.segs[0];
      const auto n_msgs = static_cast<long long>(g->messages.size());
      if (!s.index) {
        if (s.key != "length") return {};
        v = num_val(static_cast<double>(n_msgs));
      } else {
        long long n = s.n < 0 ? s.n + n_msgs : s.n;
        if (n < 0 || n >= n_msgs) {
          if (why) *why = "the call returned " + std::to_string(n_msgs) + " message(s)";
          return {};
        }
        v = d.message(static_cast<size_t>(n));
      }
      from = 1;
      break;
    }
  }
  Val r = walk(v, p.segs, from);
  return r;
}

// Parses what a target will read, so that the parse is not billed to one assertion.
void warm(const Path& p, ResponseData::Impl& d) {
  if (p.root == Root::Body) d.json(0);
  else if (p.root == Root::Messages && d.grpc) {
    if (p.segs.empty()) d.messages_json();
    else if (p.segs[0].index) {
      const auto n = static_cast<long long>(d.text_count());
      const long long i = p.segs[0].n < 0 ? p.segs[0].n + n : p.segs[0].n;
      if (i >= 0 && i < n) d.json(static_cast<size_t>(i));
    }
  }
}

// ------------------------------------------------------------------ expected values

struct Expect {
  enum Kind { Undefined, Null, Bool, Number, String, Json } kind = String;
  bool quoted = false;
  bool b = false;
  double num = 0;
  std::string text;  // the string (unquoted), or the value as written
  std::unique_ptr<rapidjson::Document> doc;
};

Expect parse_expect(std::string_view raw) {
  Expect e;
  const std::string_view s = trim(raw);
  e.text.assign(s);
  if (s.size() >= 2 && (s[0] == '"' || s[0] == '\'') && s.back() == s[0]) {
    e.quoted = true;
    e.text.clear();
    for (size_t i = 1; i + 1 < s.size(); ++i) {
      char c = s[i];
      if (c == '\\' && i + 2 < s.size()) {
        c = s[++i];
        if (c == 'n') c = '\n';
        else if (c == 't') c = '\t';
        else if (c == 'r') c = '\r';
      }
      e.text += c;
    }
    return e;
  }
  if (s == "true" || s == "false") {
    e.kind = Expect::Bool;
    e.b = s == "true";
  } else if (s == "null") {
    e.kind = Expect::Null;
  } else if (s == "undefined") {
    e.kind = Expect::Undefined;
  } else if (parse_number(s, e.num)) {
    e.kind = Expect::Number;
  } else if (!s.empty() && (s[0] == '{' || s[0] == '[')) {
    if (auto d = parse_copy(s)) {
      e.kind = Expect::Json;
      e.doc = std::move(d);
    }
  }
  return e;
}

// "a, 'b,c', 3" -> items (commas inside quotes or brackets do not split).
std::vector<std::string_view> split_list(std::string_view s) {
  std::vector<std::string_view> out;
  char q = 0;
  int depth = 0;
  size_t start = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (q) {
      if (c == '\\') ++i;
      else if (c == q) q = 0;
    } else if (c == '"' || c == '\'') {
      q = c;
    } else if (c == '[' || c == '{') {
      ++depth;
    } else if ((c == ']' || c == '}') && depth > 0) {
      --depth;
    } else if (c == ',' && depth == 0) {
      out.push_back(trim(s.substr(start, i - start)));
      start = i + 1;
    }
  }
  if (!trim(s.substr(start)).empty() || !out.empty()) out.push_back(trim(s.substr(start)));
  return out;
}

bool as_number(const Val& v, double& out) {
  if (v.kind == Val::Number) {
    out = v.num;
    return true;
  }
  return v.kind == Val::String && parse_number(trim(v.str), out);
}

// 1 equal, 0 not, -1 too deep to compare.
int equals(const Val& a, const Expect& e) {
  switch (e.kind) {
    case Expect::Undefined: return a.kind == Val::Undefined;
    case Expect::Null: return a.kind == Val::Null;
    case Expect::Bool:
      return (a.kind == Val::Bool && a.b == e.b) || (a.kind == Val::String && a.str == (e.b ? "true" : "false"));
    case Expect::Number: {
      double x;
      return as_number(a, x) && x == e.num;
    }
    case Expect::String:
      if (!a.status_name.empty() && iequals(a.status_name, e.text)) return 1;
      if (a.kind == Val::String) return a.str == e.text;
      if (e.quoted || a.kind == Val::Undefined) return 0;
      {
        std::string t;
        if (!val_text(a, t, e.text.size() + 1)) return -1;
        return t == e.text;
      }
    case Expect::Json:
      if (a.kind != Val::Json) return 0;
      return json_equal(*a.json, *e.doc);
  }
  return 0;
}

bool truthy(const Val& v) {
  switch (v.kind) {
    case Val::Undefined:
    case Val::Null: return false;
    case Val::Bool: return v.b;
    case Val::Number: return v.num != 0 && v.num == v.num;
    case Val::String: return !v.str.empty();
    case Val::Json: return true;
  }
  return false;
}

enum class Op {
  Eq, Neq, Gt, Gte, Lt, Lte, In, NotIn, Contains, NotContains, Length, Matches, NotMatches, StartsWith,
  EndsWith, Between, IsEmpty, IsNotEmpty, IsNull, IsUndefined, IsDefined, IsTruthy, IsFalsy, IsJson,
  IsNumber, IsString, IsBoolean, IsArray, Unknown,
};

const struct {
  const char* name;
  Op op;
  bool takes_value;
} kOps[] = {
    {"eq", Op::Eq, true},
    {"neq", Op::Neq, true},
    {"gt", Op::Gt, true},
    {"gte", Op::Gte, true},
    {"lt", Op::Lt, true},
    {"lte", Op::Lte, true},
    {"in", Op::In, true},
    {"notIn", Op::NotIn, true},
    {"contains", Op::Contains, true},
    {"notContains", Op::NotContains, true},
    {"length", Op::Length, true},
    {"matches", Op::Matches, true},
    {"notMatches", Op::NotMatches, true},
    {"startsWith", Op::StartsWith, true},
    {"endsWith", Op::EndsWith, true},
    {"between", Op::Between, true},
    {"isEmpty", Op::IsEmpty, false},
    {"isNotEmpty", Op::IsNotEmpty, false},
    {"isNull", Op::IsNull, false},
    {"isUndefined", Op::IsUndefined, false},
    {"isDefined", Op::IsDefined, false},
    {"isTruthy", Op::IsTruthy, false},
    {"isFalsy", Op::IsFalsy, false},
    {"isJson", Op::IsJson, false},
    {"isNumber", Op::IsNumber, false},
    {"isString", Op::IsString, false},
    {"isBoolean", Op::IsBoolean, false},
    {"isArray", Op::IsArray, false},
};

Op op_from(std::string_view s, bool* takes_value) {
  s = trim(s);
  if (s.empty()) s = "eq";
  // Aliases: symbols and the snake_case names of watt's response view.
  static const std::pair<const char*, const char*> kAliases[] = {
      {"==", "eq"}, {"!=", "neq"}, {"ne", "neq"}, {">", "gt"}, {">=", "gte"}, {"ge", "gte"},
      {"<", "lt"}, {"<=", "lte"}, {"le", "lte"}, {"not_in", "notIn"}, {"not_contains", "notContains"},
      {"exists", "isDefined"}, {"not_exists", "isUndefined"}, {"not_matches", "notMatches"},
      {"starts_with", "startsWith"}, {"ends_with", "endsWith"}};
  for (const auto& [from, to] : kAliases)
    if (iequals(s, from)) {
      s = to;
      break;
    }
  for (const auto& o : kOps)
    if (iequals(s, o.name)) {
      *takes_value = o.takes_value;
      return o.op;
    }
  *takes_value = false;
  return Op::Unknown;
}

const char* op_words(Op op) {
  switch (op) {
    case Op::Eq: return "to equal";
    case Op::Neq: return "not to equal";
    case Op::Gt: return "to be >";
    case Op::Gte: return "to be >=";
    case Op::Lt: return "to be <";
    case Op::Lte: return "to be <=";
    case Op::In: return "to be one of";
    case Op::NotIn: return "not to be one of";
    case Op::Contains: return "to contain";
    case Op::NotContains: return "not to contain";
    case Op::Length: return "to have length";
    case Op::Matches: return "to match";
    case Op::NotMatches: return "not to match";
    case Op::StartsWith: return "to start with";
    case Op::EndsWith: return "to end with";
    case Op::Between: return "to be between";
    case Op::IsEmpty: return "to be empty";
    case Op::IsNotEmpty: return "not to be empty";
    case Op::IsNull: return "to be null";
    case Op::IsUndefined: return "to be undefined";
    case Op::IsDefined: return "to be defined";
    case Op::IsTruthy: return "to be truthy";
    case Op::IsFalsy: return "to be falsy";
    case Op::IsJson: return "to be JSON (an object or array)";
    case Op::IsNumber: return "to be a number";
    case Op::IsString: return "to be a string";
    case Op::IsBoolean: return "to be a boolean";
    case Op::IsArray: return "to be an array";
    default: return "";
  }
}

// Evaluates one assertion. Returns pass; *err set for a malformed assertion (wrong value type,
// bad pattern) or an undefined reason.
bool check(Op op, const Val& a, const std::string& value, std::string& err) {
  auto number_arg = [&](std::string_view s, double& x) {
    if (parse_number(trim(s), x)) return true;
    err = "expected a number, got '" + std::string(trim(s)) + "'";
    return false;
  };
  auto actual_text = [&](std::string& t) {
    if (a.kind == Val::Undefined) return false;
    if (!val_text(a, t)) {
      err = "value nested more than " + std::to_string(kMaxJsonDepth) + " levels deep";
      return false;
    }
    return true;
  };
  switch (op) {
    case Op::Eq:
    case Op::Neq: {
      const int r = equals(a, parse_expect(value));
      if (r < 0) {
        err = "value nested more than " + std::to_string(kMaxJsonDepth) + " levels deep";
        return false;
      }
      return (r == 1) == (op == Op::Eq);
    }
    case Op::Gt:
    case Op::Gte:
    case Op::Lt:
    case Op::Lte: {
      double e, x;
      if (!number_arg(value, e)) return false;
      if (!as_number(a, x)) return false;
      return op == Op::Gt ? x > e : op == Op::Gte ? x >= e : op == Op::Lt ? x < e : x <= e;
    }
    case Op::Between: {
      auto items = split_list(value);
      if (items.size() == 1) {  // "10 20"
        const std::string_view s = items[0];
        const size_t sp = s.find(' ');
        if (sp != std::string_view::npos) items = {trim(s.substr(0, sp)), trim(s.substr(sp + 1))};
      }
      double lo, hi, x;
      if (items.size() != 2) {
        err = "between takes two numbers: between 10, 20";
        return false;
      }
      if (!number_arg(items[0], lo) || !number_arg(items[1], hi)) return false;
      return as_number(a, x) && x >= lo && x <= hi;
    }
    case Op::In:
    case Op::NotIn: {
      const auto items = split_list(value);
      if (items.empty()) {
        err = "in takes a list: in 200, 201, 204";
        return false;
      }
      bool any = false;
      for (const auto& it : items) {
        // Status ranges: "200-299" and "2xx" (numbers only).
        double x, lo, hi;
        if (const size_t dash = it.find('-', 1); dash != std::string_view::npos &&
            parse_number(it.substr(0, dash), lo) && parse_number(it.substr(dash + 1), hi)) {
          if (as_number(a, x) && x >= lo && x <= hi) {
            any = true;
            break;
          }
          continue;
        }
        if (it.size() == 3 && is_digit(it[0]) && lower(it[1]) == 'x' && lower(it[2]) == 'x') {
          if (as_number(a, x) && x >= (it[0] - '0') * 100 && x < (it[0] - '0' + 1) * 100) {
            any = true;
            break;
          }
          continue;
        }
        const int r = equals(a, parse_expect(it));
        if (r < 0) {
          err = "value nested too deeply";
          return false;
        }
        if (r == 1) {
          any = true;
          break;
        }
      }
      return any == (op == Op::In);
    }
    case Op::Contains:
    case Op::NotContains: {
      bool has = false;
      const Expect e = parse_expect(value);
      if (a.kind == Val::Json && a.json->IsArray()) {
        for (const auto& el : a.json->GetArray())
          if (equals(from_json(el), e) == 1) {
            has = true;
            break;
          }
      } else if (a.kind == Val::Json) {
        has = a.json->HasMember(Value(rapidjson::StringRef(e.text.data(), static_cast<rapidjson::SizeType>(e.text.size()))));
      } else if (a.kind == Val::Undefined || a.kind == Val::Null) {
        return false;  // not even notContains holds for a missing value
      } else {
        std::string t;
        actual_text(t);
        has = t.find(e.text) != std::string::npos;
      }
      return has == (op == Op::Contains);
    }
    case Op::Length: {
      double e;
      if (!number_arg(value, e)) return false;
      double n;
      if (a.kind == Val::String) n = static_cast<double>(utf8_length(a.str));
      else if (a.kind == Val::Json) n = a.json->IsArray() ? a.json->Size() : a.json->MemberCount();
      else return false;
      return n == e;
    }
    case Op::Matches:
    case Op::NotMatches: {
      const auto rx = regex_for(trim(value));
      if (!rx->re) {
        err = rx->error;
        return false;
      }
      std::string t;
      if (!actual_text(t)) return false;
      return RE2::PartialMatch(t, *rx->re) == (op == Op::Matches);
    }
    case Op::StartsWith:
    case Op::EndsWith: {
      const Expect e = parse_expect(value);
      std::string t;
      if (!actual_text(t)) return false;
      if (t.size() < e.text.size()) return false;
      return op == Op::StartsWith ? t.compare(0, e.text.size(), e.text) == 0
                                  : t.compare(t.size() - e.text.size(), e.text.size(), e.text) == 0;
    }
    case Op::IsEmpty:
    case Op::IsNotEmpty: {
      bool empty;
      if (a.kind == Val::String) empty = a.str.empty();
      else if (a.kind == Val::Json) empty = a.json->IsArray() ? a.json->Empty() : a.json->ObjectEmpty();
      else return false;  // numbers, booleans, null, undefined are neither
      return empty == (op == Op::IsEmpty);
    }
    case Op::IsNull: return a.kind == Val::Null;
    case Op::IsUndefined: return a.kind == Val::Undefined;
    case Op::IsDefined: return a.kind != Val::Undefined;
    case Op::IsTruthy: return truthy(a);
    case Op::IsFalsy: return !truthy(a);
    case Op::IsJson:
      if (a.kind == Val::Json) return true;
      if (a.kind == Val::String) {
        auto d = parse_copy(a.str);
        return d && (d->IsObject() || d->IsArray());
      }
      return false;
    case Op::IsNumber: return a.kind == Val::Number;
    case Op::IsString: return a.kind == Val::String;
    case Op::IsBoolean: return a.kind == Val::Bool;
    case Op::IsArray: return a.kind == Val::Json && a.json->IsArray();
    case Op::Unknown: return false;
  }
  return false;
}

double us_since(Clock::time_point t0) { return std::chrono::duration<double, std::micro>(Clock::now() - t0).count(); }

std::string var_name_problem(const std::string& v) {
  if (v.size() > kMaxVarName) return "variable name longer than " + std::to_string(kMaxVarName) + " bytes";
  if (v.find("{{") != std::string::npos || v.find("}}") != std::string::npos) return "variable name contains braces";
  for (const char c : v)
    if (static_cast<unsigned char>(c) < 0x20) return "variable name contains a control character";
  if (trim(v) != v) return "variable name has leading or trailing spaces";
  return {};
}

}  // namespace

// ------------------------------------------------------------------ API

std::vector<AssertResult> evaluate_asserts(const SavedRequest& r, const ResponseData& res, const Workspace* ws) {
  std::vector<AssertResult> out;
  out.reserve(r.asserts.size());
  ResponseData::Impl& d = res.impl();
  for (const Assertion& a : r.asserts) {
    if (!a.enabled || trim(a.target).empty()) continue;
    AssertResult ar;
    ar.a = &a;
    try {
      const auto path = path_for(a.target);
      if (path->error.empty()) warm(*path, d);
      const auto t0 = Clock::now();
      bool takes_value = false;
      const Op op = op_from(a.op, &takes_value);
      // r's folder / collection vars count too (Workspace::expand(s, r)).
      ar.expected = (ws && a.value.find("{{") != std::string::npos) ? ws->expand(a.value, r) : a.value;
      if (!path->error.empty()) {
        ar.message = "invalid target: " + path->error;
      } else if (op == Op::Unknown) {
        ar.message = "unknown operator '" + a.op + "'";
      } else {
        std::string why;
        const Val v = resolve(*path, d, &why);
        ar.actual = show(v);
        std::string err;
        ar.pass = check(op, v, ar.expected, err);
        if (!ar.pass) {
          if (!err.empty()) ar.message = err;
          else {
            ar.message = "expected " + std::string(trim(a.target)) + " " + op_words(op);
            if (takes_value) ar.message += " " + std::string(trim(ar.expected));
            ar.message += ", got " + ar.actual;
            if (!why.empty()) ar.message += " (" + why + ")";
          }
        }
      }
      ar.eval_us = us_since(t0);
    } catch (const std::exception& e) {
      ar.pass = false;
      ar.message = std::string("internal error: ") + e.what();
    } catch (...) {
      ar.pass = false;
      ar.message = "internal error";
    }
    out.push_back(std::move(ar));
  }
  return out;
}

std::vector<AssertResult> evaluate_asserts(const SavedRequest& r, const HttpResponse& res, const Workspace* ws) {
  ResponseData d(res);
  return evaluate_asserts(r, d, ws);
}

std::vector<AssertResult> evaluate_asserts(const SavedRequest& r, const GrpcResponse& res, const Workspace* ws) {
  ResponseData d(res);
  return evaluate_asserts(r, d, ws);
}

bool resolve_target(const std::string& target, const ResponseData& res, std::string& out, std::string* err) {
  try {
    const auto path = path_for(target);
    if (!path->error.empty()) {
      if (err) *err = "invalid target: " + path->error;
      return false;
    }
    std::string why;
    const Val v = resolve(*path, res.impl(), &why);
    if (v.kind == Val::Undefined) {
      if (err) *err = std::string(trim(target)) + " is undefined" + (why.empty() ? "" : " (" + why + ")");
      return false;
    }
    if (!val_text(v, out)) {
      if (err) *err = std::string(trim(target)) + " is nested more than " + std::to_string(kMaxJsonDepth) + " levels deep";
      return false;
    }
    return true;
  } catch (...) {
    if (err) *err = "internal error";
    return false;
  }
}

std::string check_target(const std::string& target) {
  try {
    return path_for(target)->error;
  } catch (...) {
    return "internal error";
  }
}

std::vector<CaptureResult> apply_captures(const SavedRequest& r, const ResponseData& res, Workspace& ws, bool persist) {
  std::vector<CaptureResult> out;
  out.reserve(r.captures.size());
  std::vector<size_t> persisted;  // results whose value went to a saved scope and changed
  for (const Capture& c : r.captures) {
    if (!c.enabled || c.var.empty()) continue;
    CaptureResult cr;
    cr.c = &c;
    try {
      Environment* env = nullptr;
      for (auto& e : ws.environments())
        if (!ws.active_env.empty() && e.name == ws.active_env) env = &e;
      const bool env_secret = env && (env->secrets.count(c.var) || env->unreadable_secrets.count(c.var));
      std::string value, err;
      if (const std::string p = var_name_problem(c.var); !p.empty()) {
        cr.message = p;
      } else if (!resolve_target(c.target, res, value, &err)) {
        cr.message = err;
      } else if (value.find("{{") != std::string::npos) {
        cr.message = "the value contains '{{', which would be expanded as a variable when sent; not captured";
      } else if (env_secret && c.scope != Capture::Globals) {
        cr.message = "'" + c.var + "' is a secret variable of environment '" + env->name +
                     "': captures never write or shadow secrets";
      } else if (c.scope == Capture::Environment) {
        if (!env) {
          cr.message = "no active environment to capture '" + c.var + "' into";
        } else {
          auto& slot = env->vars[c.var];
          if (slot != value) persisted.push_back(out.size());
          slot = value;
          cr.ok = true;
        }
      } else if (c.scope == Capture::Globals) {
        auto& slot = ws.globals[c.var];
        if (slot != value) persisted.push_back(out.size());
        slot = value;
        cr.ok = true;
      } else {
        ws.runtime_vars[c.var] = value;
        cr.ok = true;
      }
      if (cr.ok) cr.value = std::move(value);
    } catch (const std::exception& e) {
      cr.ok = false;
      cr.message = std::string("internal error: ") + e.what();
    } catch (...) {
      cr.ok = false;
      cr.message = "internal error";
    }
    out.push_back(std::move(cr));
  }
  if (persist && !persisted.empty()) {
    std::string err;
    bool saved = false;
    try {
      saved = ws.save_settings(&err);
    } catch (...) {
      err = "internal error";
    }
    if (!saved)
      for (size_t i : persisted) {
        out[i].ok = false;
        out[i].message = "captured but not saved: " + err;
      }
  }
  return out;
}

std::vector<CaptureResult> apply_captures(const SavedRequest& r, const HttpResponse& res, Workspace& ws, bool persist) {
  ResponseData d(res);
  return apply_captures(r, d, ws, persist);
}

std::vector<CaptureResult> apply_captures(const SavedRequest& r, const GrpcResponse& res, Workspace& ws, bool persist) {
  ResponseData d(res);
  return apply_captures(r, d, ws, persist);
}

std::string assert_op_value(const Assertion& a) { return a.value.empty() ? a.op : a.op + " " + a.value; }

void set_assert_op_value(Assertion& a, const std::string& op_value) {
  const size_t sp = op_value.find(' ');
  if (sp == std::string::npos) {
    a.op = op_value;
    a.value.clear();
  } else {
    a.op = op_value.substr(0, sp);
    a.value = op_value.substr(sp + 1);
  }
}

const char* capture_scope_name(Capture::Scope s) {
  switch (s) {
    case Capture::Environment: return "environment";
    case Capture::Globals: return "globals";
    default: return "runtime";
  }
}

Capture::Scope capture_scope_from(const std::string& s) {
  if (s == "environment" || s == "env") return Capture::Environment;
  if (s == "globals" || s == "global") return Capture::Globals;
  return Capture::Runtime;
}

}  // namespace watt
