#include "app/http_editor_pre.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <mutex>
#include <optional>

#include "core/http_client.h"
#include "core/model.h"

#pragma comment(lib, "bcrypt.lib")

namespace watt::app::http_editor {

namespace {

// ------------------------------------------------------------------ crypto (Windows CNG)

enum class Hash { Md5, Sha1, Sha256, Sha512 };

BCRYPT_ALG_HANDLE alg_handle(Hash h) {
  // Opened once per process; CNG algorithm handles may be shared between threads.
  static BCRYPT_ALG_HANDLE handles[4] = {};
  static std::once_flag once[4];
  const int i = static_cast<int>(h);
  std::call_once(once[i], [&] {
    LPCWSTR id = h == Hash::Md5      ? BCRYPT_MD5_ALGORITHM
                 : h == Hash::Sha1   ? BCRYPT_SHA1_ALGORITHM
                 : h == Hash::Sha256 ? BCRYPT_SHA256_ALGORITHM
                                     : BCRYPT_SHA512_ALGORITHM;
    if (BCryptOpenAlgorithmProvider(&handles[i], id, nullptr, 0) != 0) handles[i] = nullptr;
  });
  return handles[i];
}

size_t block_size(Hash h) { return h == Hash::Sha512 ? 128 : 64; }

// Digest of the parts concatenated; empty on failure.
std::string digest(Hash h, std::initializer_list<std::string_view> parts) {
  BCRYPT_ALG_HANDLE a = alg_handle(h);
  if (!a) return {};
  BCRYPT_HASH_HANDLE hh = nullptr;
  if (BCryptCreateHash(a, &hh, nullptr, 0, nullptr, 0, 0) != 0) return {};
  bool ok = true;
  for (std::string_view p : parts) {
    while (ok && !p.empty()) {
      const ULONG n = static_cast<ULONG>(std::min<size_t>(p.size(), size_t{1} << 30));
      ok = BCryptHashData(hh, reinterpret_cast<PUCHAR>(const_cast<char*>(p.data())), n, 0) == 0;
      p.remove_prefix(n);
    }
  }
  DWORD len = 0;
  ULONG got = 0;
  ok = ok && BCryptGetProperty(a, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&len), sizeof(len), &got, 0) == 0;
  std::string out(ok ? len : 0, '\0');
  ok = ok && BCryptFinishHash(hh, reinterpret_cast<PUCHAR>(out.data()), len, 0) == 0;
  BCryptDestroyHash(hh);
  return ok ? out : std::string();
}

// RFC 2104 on top of the plain hash (any key length, the empty key included).
std::string hmac(Hash h, std::string_view key, std::string_view data) {
  const size_t bs = block_size(h);
  std::string k = key.size() > bs ? digest(h, {key}) : std::string(key);
  k.resize(bs, '\0');
  std::string ipad(bs, '\0'), opad(bs, '\0');
  for (size_t i = 0; i < bs; ++i) {
    ipad[i] = static_cast<char>(k[i] ^ 0x36);
    opad[i] = static_cast<char>(k[i] ^ 0x5c);
  }
  const std::string inner = digest(h, {ipad, data});
  if (inner.empty()) return {};
  return digest(h, {opad, inner});
}

bool random_bytes(void* p, size_t n) {
  return BCryptGenRandom(nullptr, static_cast<PUCHAR>(p), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

// ------------------------------------------------------------------ encodings

std::string to_hex(std::string_view s) {
  static const char* d = "0123456789abcdef";
  std::string out;
  out.reserve(s.size() * 2);
  for (unsigned char c : s) {
    out += d[c >> 4];
    out += d[c & 15];
  }
  return out;
}

std::string b64(std::string_view in, bool url) {
  const char* t = url ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
                      : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const uint32_t v = (uint32_t(uint8_t(in[i])) << 16) | (uint32_t(uint8_t(in[i + 1])) << 8) | uint8_t(in[i + 2]);
    out += t[v >> 18];
    out += t[(v >> 12) & 63];
    out += t[(v >> 6) & 63];
    out += t[v & 63];
  }
  if (i < in.size()) {
    uint32_t v = uint32_t(uint8_t(in[i])) << 16;
    const bool two = i + 1 < in.size();
    if (two) v |= uint32_t(uint8_t(in[i + 1])) << 8;
    out += t[v >> 18];
    out += t[(v >> 12) & 63];
    if (two) out += t[(v >> 6) & 63];
    if (!url) out += two ? "=" : "==";
  }
  return out;
}

std::optional<std::string> b64_decode(std::string_view in) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
  };
  std::string out;
  uint32_t acc = 0;
  int bits = 0;
  for (char c : in) {
    if (c == '=') break;
    if (c == '\r' || c == '\n' || c == ' ') continue;
    const int v = val(c);
    if (v < 0) return std::nullopt;
    acc = ((acc << 6) | static_cast<uint32_t>(v)) & 0xffffff;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += static_cast<char>((acc >> bits) & 0xff);
    }
  }
  return out;
}

std::string url_encode(std::string_view s) {
  static const char* d = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
        c == '_' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += d[c >> 4];
      out += d[c & 15];
    }
  }
  return out;
}

int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string url_decode(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && hex_val(s[i + 1]) >= 0 && hex_val(s[i + 2]) >= 0) {
      out += static_cast<char>(hex_val(s[i + 1]) * 16 + hex_val(s[i + 2]));
      i += 2;
    } else {
      out += s[i] == '+' ? ' ' : s[i];
    }
  }
  return out;
}

std::string lower_ascii(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

std::string upper_ascii(std::string s) {
  for (char& c : s)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  return s;
}

bool blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string_view trim_view(std::string_view s) {
  while (!s.empty() && blank(s.front())) s.remove_prefix(1);
  while (!s.empty() && blank(s.back())) s.remove_suffix(1);
  return s;
}

std::optional<long long> to_int(std::string_view s) {
  s = trim_view(s);
  if (!s.empty() && s.front() == '+') s.remove_prefix(1);
  if (s.empty()) return std::nullopt;
  long long v = 0;
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || p != s.data() + s.size()) return std::nullopt;
  return v;
}

size_t utf8_length(std::string_view s) {
  size_t n = 0;
  for (unsigned char c : s) n += (c & 0xC0) != 0x80;
  return n;
}

long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string iso_utc(long long ms) {
  const std::time_t secs = static_cast<std::time_t>(ms / 1000);
  std::tm tm{};
  if (gmtime_s(&tm, &secs) != 0) return {};
  char buf[40];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms % 1000));
  return buf;
}

std::string uuid_v4() {
  unsigned char b[16] = {};
  if (!random_bytes(b, sizeof b)) return {};
  b[6] = static_cast<unsigned char>((b[6] & 0x0f) | 0x40);
  b[8] = static_cast<unsigned char>((b[8] & 0x3f) | 0x80);
  const std::string h = to_hex(std::string_view(reinterpret_cast<const char*>(b), 16));
  return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20);
}

// ------------------------------------------------------------------ evaluator

struct Ctx {
  const SavedRequest* r = nullptr;
  Workspace* ws = nullptr;  // null: syntax check only
  // req.url / req.body as sent, expanded once per row (an earlier row may change them)
  std::optional<std::string> url, body;
};

using Args = std::vector<std::string>;
using FnImpl = std::function<bool(Args& a, std::string& out, std::string& err)>;
struct Fn {
  const char* name;
  int min_args, max_args;
  FnImpl run;
};

bool encode_digest(const std::string& raw, const Args& a, size_t enc_at, std::string& out, std::string& err) {
  if (raw.empty()) {
    err = "hash failed";
    return false;
  }
  const std::string enc = a.size() > enc_at ? lower_ascii(std::string(trim_view(a[enc_at]))) : "hex";
  if (enc == "hex") out = to_hex(raw);
  else if (enc == "base64") out = b64(raw, false);
  else if (enc == "base64url") out = b64(raw, true);
  else {
    err = "unknown encoding '" + enc + "' (hex, base64, base64url)";
    return false;
  }
  return true;
}

bool fail_with(std::string& err, const char* m) {
  err = m;
  return false;
}

const std::vector<Fn>& functions() {
  auto hash_fn = [](const char* name, Hash h) {
    return Fn{name, 1, 2, [h](Args& a, std::string& out, std::string& err) {
                return encode_digest(digest(h, {a[0]}), a, 1, out, err);
              }};
  };
  auto hmac_fn = [](const char* name, Hash h) {
    return Fn{name, 2, 3, [h](Args& a, std::string& out, std::string& err) {
                return encode_digest(hmac(h, a[0], a[1]), a, 2, out, err);
              }};
  };
  auto unary = [](const char* name, std::string (*f)(const std::string&)) {
    return Fn{name, 1, 1, [f](Args& a, std::string& out, std::string&) {
                out = f(a[0]);
                return true;
              }};
  };
  static const std::vector<Fn> fns = {
      hmac_fn("hmacSha256", Hash::Sha256),
      hmac_fn("hmacSha1", Hash::Sha1),
      hmac_fn("hmacSha512", Hash::Sha512),
      hash_fn("sha256", Hash::Sha256),
      hash_fn("sha1", Hash::Sha1),
      hash_fn("sha512", Hash::Sha512),
      hash_fn("md5", Hash::Md5),
      unary("base64", [](const std::string& s) { return b64(s, false); }),
      unary("base64url", [](const std::string& s) { return b64(s, true); }),
      Fn{"base64Decode", 1, 1,
         [](Args& a, std::string& out, std::string& err) {
           auto d = b64_decode(a[0]);
           if (!d) return fail_with(err, "not base64");
           out = std::move(*d);
           return true;
         }},
      unary("hex", [](const std::string& s) { return to_hex(s); }),
      unary("urlencode", [](const std::string& s) { return url_encode(s); }),
      unary("urldecode", [](const std::string& s) { return url_decode(s); }),
      unary("lower", [](const std::string& s) { return lower_ascii(s); }),
      unary("upper", [](const std::string& s) { return upper_ascii(s); }),
      unary("trim", [](const std::string& s) { return std::string(trim_view(s)); }),
      unary("length", [](const std::string& s) { return std::to_string(utf8_length(s)); }),
      Fn{"replace", 3, 3,
         [](Args& a, std::string& out, std::string&) {
           out.clear();
           if (a[1].empty()) {
             out = a[0];
             return true;
           }
           size_t i = 0;
           for (size_t at; (at = a[0].find(a[1], i)) != std::string::npos; i = at + a[1].size())
             out.append(a[0], i, at - i).append(a[2]);
           out.append(a[0], i, std::string::npos);
           return true;
         }},
      Fn{"substr", 2, 3,
         [](Args& a, std::string& out, std::string& err) {
           const auto start = to_int(a[1]);
           const auto len = a.size() > 2 ? to_int(a[2]) : std::optional<long long>(-1);
           if (!start || !len || *start < 0) return fail_with(err, "substr(text, start[, length]) takes numbers");
           const size_t s = std::min(static_cast<size_t>(*start), a[0].size());
           out = *len < 0 ? a[0].substr(s) : a[0].substr(s, static_cast<size_t>(*len));
           return true;
         }},
      Fn{"concat", 0, 64,
         [](Args& a, std::string& out, std::string&) {
           out.clear();
           for (const auto& s : a) out += s;
           return true;
         }},
      Fn{"now", 0, 0,
         [](Args&, std::string& out, std::string&) {
           out = std::to_string(now_ms());
           return true;
         }},
      Fn{"unix", 0, 1,
         [](Args& a, std::string& out, std::string& err) {
           const auto off = a.empty() ? std::optional<long long>(0) : to_int(a[0]);
           if (!off) return fail_with(err, "unix(offset_seconds) takes a number");
           out = std::to_string(now_ms() / 1000 + *off);
           return true;
         }},
      Fn{"iso", 0, 1,
         [](Args& a, std::string& out, std::string& err) {
           const auto off = a.empty() ? std::optional<long long>(0) : to_int(a[0]);
           if (!off) return fail_with(err, "iso(offset_seconds) takes a number");
           out = iso_utc(now_ms() + *off * 1000);
           return true;
         }},
      Fn{"uuid", 0, 0,
         [](Args&, std::string& out, std::string& err) {
           out = uuid_v4();
           return out.empty() ? fail_with(err, "no random numbers") : true;
         }},
      Fn{"randomInt", 0, 2,
         [](Args& a, std::string& out, std::string& err) {
           long long lo = 0, hi = 1000;
           if (a.size() == 1) {
             const auto h = to_int(a[0]);
             if (!h) return fail_with(err, "randomInt(lo, hi) takes numbers");
             hi = *h;
           } else if (a.size() == 2) {
             const auto l = to_int(a[0]), h = to_int(a[1]);
             if (!l || !h) return fail_with(err, "randomInt(lo, hi) takes numbers");
             lo = *l;
             hi = *h;
           }
           if (hi < lo) std::swap(lo, hi);
           uint64_t r = 0;
           if (!random_bytes(&r, sizeof r)) return fail_with(err, "no random numbers");
           const uint64_t span = static_cast<uint64_t>(hi) - static_cast<uint64_t>(lo) + 1;
           out = std::to_string(span == 0 ? static_cast<long long>(r) : lo + static_cast<long long>(r % span));
           return true;
         }},
      Fn{"randomHex", 1, 1,
         [](Args& a, std::string& out, std::string& err) {
           const auto n = to_int(a[0]);
           if (!n || *n < 0 || *n > 4096) return fail_with(err, "randomHex(bytes) takes 0..4096");
           std::string b(static_cast<size_t>(*n), '\0');
           if (!b.empty() && !random_bytes(b.data(), b.size())) return fail_with(err, "no random numbers");
           out = to_hex(b);
           return true;
         }},
      Fn{"jsonPath", 2, 2,
         [](Args& a, std::string& out, std::string& err) {
           std::string_view p = trim_view(a[1]);
           if (!p.empty() && p.front() == '$') p.remove_prefix(1);
           std::string target = "res.body";
           if (!p.empty()) target += (p.front() == '.' || p.front() == '[') ? std::string(p) : "." + std::string(p);
           HttpResponse resp;
           resp.ok = true;
           resp.body = std::move(a[0]);
           ResponseData rd(resp);
           std::string e;
           if (!resolve_target(target, rd, out, &e)) {
             err = e.empty() ? std::string("no value at ") + std::string(a[1]) : e;
             return false;
           }
           return true;
         }},
  };
  return fns;
}

const Fn* find_fn(std::string_view name) {
  for (const auto& f : functions()) {
    const std::string_view n(f.name);
    if (n.size() == name.size() && std::equal(n.begin(), n.end(), name.begin(), [](char x, char y) {
          return (x >= 'A' && x <= 'Z' ? x + 32 : x) == (y >= 'A' && y <= 'Z' ? y + 32 : y);
        }))
      return &f;
  }
  return nullptr;
}

bool ident_start(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '$'; }
bool ident_char(char c) { return ident_start(c) || (c >= '0' && c <= '9') || c == '.' || c == '-'; }

// Recursive descent over the expression text; with a null workspace it only checks the syntax
// (function names, argument counts), computing nothing. Data only: there is nothing to execute.
class Parser {
 public:
  Parser(std::string_view s, Ctx& c) : s_(s), c_(c) {}

  bool run(std::string& out) {
    if (s_.size() > kMaxPreExprBytes) return fail("expression longer than " + std::to_string(kMaxPreExprBytes) + " bytes");
    skip();
    if (i_ >= s_.size()) return fail("no expression");
    if (!expr(out, 0)) return false;
    skip();
    if (i_ < s_.size()) return fail(std::string("unexpected '") + s_[i_] + "'" + (s_[i_] == ')' ? "" : " (join with +)"));
    return true;
  }
  // Text with {{vars}} and function calls in it, as Bruno / Postman users write a value:
  // "{{userId}}-lower(req.method)" -> "42-get". Everything else is kept as written; a name
  // followed by "(" must be one of the functions.
  bool run_template(std::string& out) {
    if (s_.size() > kMaxPreExprBytes) return fail("expression longer than " + std::to_string(kMaxPreExprBytes) + " bytes");
    err_.clear();
    out.clear();
    i_ = 0;
    while (i_ < s_.size() && blank(s_[i_])) ++i_;
    size_t end = s_.size();
    while (end > i_ && blank(s_[end - 1])) --end;
    std::string t;
    bool calls = false;
    while (i_ < end) {
      const char c = s_[i_];
      if (c == '{' && i_ + 1 < end && s_[i_ + 1] == '{') {
        const size_t close = s_.find("}}", i_ + 2);
        if (close == std::string_view::npos || close + 2 > end) return fail("{{ without }}");
        const std::string_view name = trim_view(s_.substr(i_ + 2, close - i_ - 2));
        if (name.empty()) return fail("empty {{}}");
        if (computing()) out += expand_("{{" + std::string(name) + "}}");
        i_ = close + 2;
      } else if (ident_start(c) && (i_ == 0 || !word(s_[i_ - 1]))) {
        const size_t b = i_;
        while (i_ < end && word(s_[i_])) ++i_;
        const std::string_view name = s_.substr(b, i_ - b);
        size_t p = i_;
        while (p < end && blank(s_[p])) ++p;
        if (p < end && s_[p] == '(') {
          i_ = p;
          if (!call(name, b, t, 1)) return false;
          out += t;
          calls = true;
        } else {
          out.append(name);
        }
      } else if (c == '\'' || c == '"' || c == '+') {
        return fail("unexpected");  // an expression after all ("{{a}} - 'x'"): its own error stands
      } else {
        out += c;
        ++i_;
      }
      if (out.size() > kMaxPreValueBytes) return fail("value too large");
    }
    return calls || fail("no call");  // text without calls is template_text()'s
  }
  const std::string& error() const { return err_; }

 private:
  static bool word(char ch) { return ident_start(ch) || (ch >= '0' && ch <= '9'); }
  bool fail(std::string m) {
    if (err_.empty()) err_ = i_ < s_.size() ? "col " + std::to_string(i_ + 1) + ": " + m : m;
    return false;
  }
  void skip() {
    while (i_ < s_.size() && blank(s_[i_])) ++i_;
  }
  bool computing() const { return c_.ws != nullptr; }

  bool expr(std::string& out, int depth) {
    if (depth > kMaxPreDepth) return fail("nested too deeply");
    out.clear();
    if (!term(out, depth)) return false;
    bool num = num_;  // as in JavaScript: number + number adds, anything else concatenates
    std::string t;
    for (;;) {
      skip();
      if (i_ >= s_.size() || s_[i_] != '+') {
        num_ = num;
        return true;
      }
      ++i_;
      if (!term(t, depth)) return false;
      if (num && num_ && computing()) {
        double a = 0, b = 0;
        if (to_num(out, a) && to_num(t, b)) {
          out = fmt_num(a + b);
          continue;
        }
      }
      num = false;
      if (out.size() + t.size() > kMaxPreValueBytes) return fail("value too large");
      out += t;
    }
  }
  static bool to_num(const std::string& s, double& v) {
    const char* e = s.data() + s.size();
    const auto r = std::from_chars(s.data(), e, v);
    return r.ec == std::errc() && r.ptr == e;
  }
  static std::string fmt_num(double v) {
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    return r.ec == std::errc() ? std::string(buf, r.ptr) : std::string("NaN");
  }

  bool term(std::string& out, int depth) {
    skip();
    out.clear();
    num_ = false;
    if (i_ >= s_.size()) return fail("a value is missing at the end");
    const char c = s_[i_];
    if (c == '\'' || c == '"') return string_lit(out, c);
    if ((c >= '0' && c <= '9') || c == '-' || c == '.') {
      const size_t b = i_;
      if (c == '-') ++i_;
      while (i_ < s_.size() && ((s_[i_] >= '0' && s_[i_] <= '9') || s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E'))
        ++i_;
      if (i_ == b + (c == '-' ? 1u : 0u)) return fail("expected a number");
      out.assign(s_.substr(b, i_ - b));
      num_ = true;  // kept as written ("007") unless it is added to another number
      return true;
    }
    if (c == '{' && i_ + 1 < s_.size() && s_[i_ + 1] == '{') {
      const size_t close = s_.find("}}", i_ + 2);
      if (close == std::string_view::npos) return fail("{{ without }}");
      const std::string_view name = trim_view(s_.substr(i_ + 2, close - i_ - 2));
      if (name.empty()) return fail("empty {{}}");
      i_ = close + 2;
      if (computing()) out = expand_("{{" + std::string(name) + "}}");
      return true;
    }
    if (c == '(') {
      ++i_;
      if (!expr(out, depth + 1)) return false;
      skip();
      if (i_ >= s_.size() || s_[i_] != ')') return fail("expected ')'");
      ++i_;
      return true;
    }
    if (!ident_start(c)) return fail(std::string("unexpected '") + c + "'");
    const size_t b = i_;
    while (i_ < s_.size() && ident_char(s_[i_])) ++i_;
    const std::string_view name = s_.substr(b, i_ - b);
    skip();
    if (i_ < s_.size() && s_[i_] == '(') return call(name, b, out, depth);
    return ident(name, b, out);
  }

  bool call(std::string_view name, size_t at, std::string& out, int depth) {
    const Fn* f = find_fn(name);
    if (!f) {
      i_ = at;
      return fail("unknown function '" + std::string(name) + "'");
    }
    ++i_;
    Args args;
    skip();
    if (i_ < s_.size() && s_[i_] == ')') {
      ++i_;
    } else {
      for (;;) {
        std::string a;
        if (!expr(a, depth + 1)) return false;
        args.push_back(std::move(a));
        skip();
        if (i_ < s_.size() && s_[i_] == ',') {
          ++i_;
          continue;
        }
        if (i_ < s_.size() && s_[i_] == ')') {
          ++i_;
          break;
        }
        return fail(i_ >= s_.size() ? "')' is missing" : "expected ',' or ')'");
      }
    }
    const int n = static_cast<int>(args.size());
    // A number in JavaScript too (Date.now(), str.length): "+" adds it to another number.
    const std::string_view fname(f->name);
    num_ = fname == "now" || fname == "unix" || fname == "randomInt" || fname == "length";
    if (n < f->min_args || n > f->max_args) {
      i_ = at;
      const std::string want = f->min_args == f->max_args ? std::to_string(f->min_args)
                               : f->max_args > 8          ? std::to_string(f->min_args) + "+"
                                                          : std::to_string(f->min_args) + "-" + std::to_string(f->max_args);
      return fail(std::string(f->name) + "() takes " + want + " argument" + (want == "1" ? "" : "s") + ", got " +
                  std::to_string(n));
    }
    if (!computing()) return true;
    std::string e;
    if (!f->run(args, out, e)) {
      i_ = at;
      return fail(std::string(f->name) + ": " + e);
    }
    if (out.size() > kMaxPreValueBytes) return fail("value too large");
    return true;
  }

  bool ident(std::string_view name, size_t at, std::string& out) {
    if (name.substr(0, 4) == "req.") {
      const std::string_view f = name.substr(4);
      const bool hdr = f.substr(0, 8) == "headers." && f.size() > 8;
      if (f != "method" && f != "url" && f != "body" && !hdr) {
        i_ = at;
        return fail("unknown '" + std::string(name) + "' (req.method, req.url, req.body, req.headers.<name>)");
      }
      if (!computing()) return true;
      const SavedRequest& r = *c_.r;
      if (f == "method") {
        out = r.http.method;
      } else if (f == "url") {
        if (!c_.url) c_.url = expand_(r.http.url);
        out = *c_.url;
      } else if (f == "body") {
        if (!c_.body) c_.body = expand_(r.http.body);
        out = *c_.body;
      } else {
        const std::string want = lower_ascii(std::string(f.substr(8)));
        out.clear();
        for (size_t k = 0; k < r.http.headers.size(); ++k) {
          if (r.disabled_headers.count(k)) continue;
          if (lower_ascii(expand_(r.http.headers[k].first)) == want) {
            out = expand_(r.http.headers[k].second);
            break;
          }
        }
      }
      return true;
    }
    if (!computing()) return true;
    const std::string ref = "{{" + std::string(name) + "}}";
    out = expand_(ref);
    if (out == ref) {
      i_ = at;
      return fail("'" + std::string(name) + "' is not defined (text needs quotes)");
    }
    return true;
  }

  bool string_lit(std::string& out, char q) {
    ++i_;
    out.clear();
    bool vars = false;
    while (i_ < s_.size() && s_[i_] != q) {
      char c = s_[i_++];
      if (c == '\\' && i_ < s_.size()) {
        const char e = s_[i_++];
        c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
      } else if (c == '{' && i_ < s_.size() && s_[i_] == '{') {
        vars = true;
      }
      out += c;
    }
    if (i_ >= s_.size()) return fail(std::string("closing ") + q + " is missing");
    ++i_;
    if (vars && computing()) out = expand_(out);
    return true;
  }

  // {{vars}} as the send expands them: with the request's folder / collection variables.
  std::string expand_(const std::string& t) const { return c_.r ? c_.ws->expand(t, *c_.r) : c_.ws->expand(t); }

  std::string_view s_;
  Ctx& c_;
  size_t i_ = 0;
  std::string err_;
  bool num_ = false;  // the last term / expression is a number (literal, sum, now(), length() ...)
};

bool valid_var_name(std::string_view v) {
  if (v.empty() || v.size() > kMaxVarName) return false;
  for (char c : v)
    if (c == '{' || c == '}' || blank(c) || static_cast<unsigned char>(c) < 0x20) return false;
  return true;
}

// Plain text with {{vars}} (Bruno's and Postman's way of writing a variable's value, e.g.
// {{userId}}-x): no quotes, parentheses or '+', so it cannot be a mistyped expression. When such
// a value does not parse as an expression it is taken as that text, its {{vars}} expanded.
bool template_text(std::string_view s) {
  return !trim_view(s).empty() && s.find_first_of("()'\"+") == std::string_view::npos;
}

// Only a value that stops parsing at a character no expression has there ({{a}}-x, v1/{{id}}) is
// text; an unknown req. field, a too long value or a bad call stays an error.
bool syntax_fails(std::string_view s) {
  Ctx c;
  Parser p(s, c);
  std::string out;
  return !p.run(out) && p.error().find("unexpected '") != std::string::npos;
}

// The parser's message plus what to write instead, so the fix is in the error itself.
std::string with_hint(std::string_view expr, std::string err) {
  const size_t dot = expr.find('.');
  if (err.find("unknown function") != std::string::npos && dot != std::string_view::npos)
    return err + ": JavaScript methods do not run here; write lower(req.method), upper(x), trim(x), length(x), "
                 "substr(x, 0, 4), replace(x, 'a', 'b'), or use the pre-request script below";
  if (err.find("unknown function") != std::string::npos) return err + " (the '?' above lists the functions)";
  if (err.find("not defined") != std::string::npos || err.find("unknown '") != std::string::npos) return err;
  return err + ". Write plain text like {{userId}}-x, or an expression like 'id-' + lower(req.method)";
}

}  // namespace

std::string check_pre_expr(std::string_view expr) {
  Ctx c;
  Parser p(expr, c);
  std::string out;
  if (p.run(out) || (template_text(expr) && syntax_fails(expr))) return {};
  if (syntax_fails(expr)) {
    Ctx c2;
    Parser t(expr, c2);
    if (t.run_template(out)) return {};
  }
  return with_hint(expr, p.error());
}

std::vector<PreVarResult> evaluate_pre_vars(const std::vector<PreVar>& rows, const SavedRequest& r, Workspace& ws) {
  std::vector<PreVarResult> out;
  const Environment* env = nullptr;
  for (const auto& e : ws.environments())
    if (e.name == ws.active_env) env = &e;
  for (size_t i = 0; i < rows.size(); ++i) {
    const PreVar& pv = rows[i];
    const std::string name(trim_view(pv.var));
    if (!pv.enabled || name.empty()) continue;
    const auto t0 = std::chrono::steady_clock::now();
    PreVarResult res;
    res.index = i;
    if (!valid_var_name(name)) {
      res.message = "invalid variable name";
    } else if (env && (env->secrets.count(name) || env->unreadable_secrets.count(name))) {
      res.message = "'" + name + "' is a secret variable: never overwritten";
    } else {
      Ctx c;
      c.r = &r;
      c.ws = &ws;
      Parser p(pv.expr, c);
      std::string v;
      bool parsed = p.run(v);
      if (!parsed && template_text(pv.expr) && syntax_fails(pv.expr)) {
        v = ws.expand(std::string(trim_view(pv.expr)), r);  // plain text, e.g. {{userId}}-x
        parsed = true;
      }
      if (!parsed && syntax_fails(pv.expr)) {
        Ctx c2;
        c2.r = &r;
        c2.ws = &ws;
        Parser t(pv.expr, c2);
        if (t.run_template(v)) parsed = true;  // text with calls: {{userId}}-lower(req.method)
      }
      if (!parsed) {
        res.message = with_hint(pv.expr, p.error());
      } else if (v.find("{{") != std::string::npos) {
        res.message = "the value holds \"{{\" (it would be expanded again when sent): not set";
      } else {
        res.ok = true;
        res.value = std::move(v);
        ws.runtime_vars[name] = res.value;
      }
    }
    res.eval_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    out.push_back(std::move(res));
  }
  return out;
}

std::vector<std::string> pre_env_refs(const std::vector<PreVar>& rows, const Workspace& ws) {
  SavedRequest probe;
  for (const auto& p : rows)
    if (p.enabled && p.expr.find("env.") != std::string::npos) probe.http.headers.emplace_back("x", p.expr);
  if (probe.http.headers.empty()) return {};
  return ws.blocked_env_vars(probe);
}

const std::vector<std::string>& pre_functions() {
  static const std::vector<std::string> sigs = {
      "hmacSha256(key, text)", "hmacSha256(key, text, \"base64\")", "hmacSha1(key, text)", "hmacSha512(key, text)",
      "sha256(text)", "sha1(text)", "sha512(text)", "md5(text)", "base64(text)", "base64url(text)",
      "base64Decode(text)", "hex(text)", "urlencode(text)", "urldecode(text)", "lower(text)", "upper(text)",
      "trim(text)", "replace(text, from, to)", "substr(text, start, length)", "length(text)", "concat(a, b)",
      "now()", "unix()", "unix(3600)", "iso()", "uuid()", "randomInt(1, 100)", "randomHex(16)",
      "jsonPath({{json}}, \"$.data.token\")", "req.method", "req.url", "req.body", "req.headers."};
  return sigs;
}

std::string hmac_sha256_hex(std::string_view key, std::string_view data) { return to_hex(hmac(Hash::Sha256, key, data)); }
std::string sha256_hex(std::string_view data) { return to_hex(digest(Hash::Sha256, {data})); }

}  // namespace watt::app::http_editor
