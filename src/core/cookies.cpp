// Per-workspace cookie jar (see cookies.h).
#include "core/cookies.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <thread>
#include <tuple>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "core/http_client.h"
#include "core/model_json.h"
#include "core/model_secure.h"

namespace watt {

namespace fs = std::filesystem;

const char* same_site_name(SameSite s) {
  switch (s) {
    case SameSite::None: return "None";
    case SameSite::Lax: return "Lax";
    case SameSite::Strict: return "Strict";
    default: return "";
  }
}

namespace {

constexpr size_t kMaxCookieBytes = 4096;
constexpr size_t kMaxPerDomain = 50;
constexpr size_t kMaxCookies = 3000;
// Prefix of the sealed plaintext: a value sealed for another purpose (a secrets.json entry) is
// not accepted as a jar.
constexpr std::string_view kJarMagic = "watt-cookie-jar/v1\n";

char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }
std::string lower_str(std::string_view s) {
  std::string o(s);
  for (auto& c : o) c = lower(c);
  return o;
}
bool ieq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (lower(a[i]) != lower(b[i])) return false;
  return true;
}
std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}
bool has_ctl(std::string_view s) {
  for (unsigned char c : s)
    if ((c < 0x20 && c != '\t') || c == 0x7f) return true;
  return false;
}
bool starts_with_ci(std::string_view s, std::string_view p) { return s.size() >= p.size() && ieq(s.substr(0, p.size()), p); }

bool is_ipv4(std::string_view h) {
  int parts = 0;
  size_t i = 0;
  while (i <= h.size()) {
    size_t j = i;
    while (j < h.size() && h[j] >= '0' && h[j] <= '9') ++j;
    if (j == i || j - i > 3) return false;
    ++parts;
    if (j == h.size()) break;
    if (h[j] != '.') return false;
    i = j + 1;
  }
  return parts == 4;
}
bool is_ip(std::string_view h) { return is_ipv4(h) || h.find(':') != std::string_view::npos; }

struct Url {
  std::string scheme, host, path;
  bool ok = false;
};

Url parse_url(std::string_view u) {
  Url r;
  const size_t se = u.find("://");
  if (se == std::string_view::npos) return r;
  r.scheme = lower_str(u.substr(0, se));
  if (r.scheme != "http" && r.scheme != "https") return r;
  std::string_view rest = u.substr(se + 3);
  const size_t pe = rest.find_first_of("/?#");
  std::string_view auth = rest.substr(0, pe);
  std::string_view tail = pe == std::string_view::npos ? std::string_view{} : rest.substr(pe);
  if (const size_t at = auth.rfind('@'); at != std::string_view::npos) auth = auth.substr(at + 1);
  if (!auth.empty() && auth.front() == '[') {
    const size_t rb = auth.find(']');
    if (rb == std::string_view::npos) return r;
    auth = auth.substr(1, rb - 1);
  } else if (const size_t c = auth.rfind(':'); c != std::string_view::npos) {
    auth = auth.substr(0, c);
  }
  while (!auth.empty() && auth.back() == '.') auth.remove_suffix(1);  // "example.com." == "example.com"
  if (auth.empty() || has_ctl(auth)) return r;
  r.host = lower_str(auth);
  std::string_view path = tail.substr(0, tail.find_first_of("?#"));
  r.path = path.empty() ? "/" : std::string(path);
  r.ok = true;
  return r;
}

bool is_loopback(std::string_view h) {
  return h == "localhost" || (h.size() > 10 && h.substr(h.size() - 10) == ".localhost") || h == "::1" ||
         (is_ipv4(h) && h.substr(0, 4) == "127.");
}
bool secure_origin(const Url& u) { return u.scheme == "https" || is_loopback(u.host); }

std::string default_path(std::string_view p) {
  if (p.empty() || p.front() != '/') return "/";
  const size_t last = p.rfind('/');
  if (last == 0) return "/";
  return std::string(p.substr(0, last));
}

int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

}  // namespace

namespace cookies {

int64_t now() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool parse_cookie_date(std::string_view s, int64_t& out) {
  auto delim = [](unsigned char c) {
    return c == 0x09 || (c >= 0x20 && c <= 0x2f) || (c >= 0x3b && c <= 0x40) || (c >= 0x5b && c <= 0x60) ||
           (c >= 0x7b && c <= 0x7e);
  };
  bool have_time = false, have_day = false, have_month = false, have_year = false;
  int hh = 0, mi = 0, ss = 0, day = 0, month = 0, year = 0;
  static const char* kMonths[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && delim(static_cast<unsigned char>(s[i]))) ++i;
    size_t j = i;
    while (j < s.size() && !delim(static_cast<unsigned char>(s[j]))) ++j;
    const std::string_view tok = s.substr(i, j - i);
    i = j;
    if (tok.empty()) continue;
    auto digits = [&](std::string_view t, size_t mn, size_t mx, int& v, size_t& used) {
      size_t k = 0;
      v = 0;
      while (k < t.size() && k < mx && t[k] >= '0' && t[k] <= '9') v = v * 10 + (t[k++] - '0');
      used = k;
      return k >= mn && (k == t.size() || !(t[k] >= '0' && t[k] <= '9'));
    };
    if (!have_time) {  // hms-time = 1*2DIGIT ":" 1*2DIGIT ":" 1*2DIGIT ( non-digit *OCTET )
      int a, b, c;
      size_t u1, u2, u3;
      if (digits(tok, 1, 2, a, u1) && u1 < tok.size() && tok[u1] == ':') {
        const auto t2 = tok.substr(u1 + 1);
        if (digits(t2, 1, 2, b, u2) && u2 < t2.size() && t2[u2] == ':') {
          const auto t3 = t2.substr(u2 + 1);
          if (digits(t3, 1, 2, c, u3)) {
            have_time = true;
            hh = a, mi = b, ss = c;
            continue;
          }
        }
      }
    }
    int v;
    size_t used;
    if (!have_day && digits(tok, 1, 2, v, used)) {
      have_day = true;
      day = v;
      continue;
    }
    if (!have_month && tok.size() >= 3) {
      bool found = false;
      for (int m = 0; m < 12; ++m)
        if (ieq(tok.substr(0, 3), kMonths[m])) {
          month = m + 1;
          found = true;
          break;
        }
      if (found) {
        have_month = true;
        continue;
      }
    }
    if (!have_year && digits(tok, 2, 4, v, used)) {
      have_year = true;
      year = v;
      continue;
    }
  }
  if (have_year && year >= 70 && year <= 99) year += 1900;
  if (have_year && year >= 0 && year <= 69) year += 2000;
  if (!have_time || !have_day || !have_month || !have_year) return false;
  if (day < 1 || day > 31 || year < 1601 || hh > 23 || mi > 59 || ss > 59) return false;
  out = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 + hh * 3600 + mi * 60 + ss;
  return true;
}

bool domain_match(std::string_view host, std::string_view domain) {
  if (host == domain) return true;
  if (domain.empty() || host.size() <= domain.size() || is_ip(host)) return false;
  return host.substr(host.size() - domain.size()) == domain && host[host.size() - domain.size() - 1] == '.';
}

bool path_match(std::string_view rp, std::string_view cp) {
  if (rp == cp) return true;
  if (rp.size() > cp.size() && rp.substr(0, cp.size()) == cp)
    return cp.back() == '/' || rp[cp.size()] == '/';
  return false;
}

bool is_public_suffix(std::string_view d) {
  if (d.empty()) return true;
  if (d.find('.') == std::string_view::npos) return true;  // a bare TLD
  // Common multi-label public suffixes (a subset of the Public Suffix List: country second
  // levels and the shared hosting domains where each tenant is its own site).
  static const char* kList[] = {
      "co.uk", "org.uk", "me.uk", "ac.uk", "gov.uk", "net.uk", "ltd.uk", "plc.uk", "sch.uk", "nhs.uk",
      "com.au", "net.au", "org.au", "edu.au", "gov.au", "asn.au", "id.au", "co.nz", "net.nz", "org.nz",
      "govt.nz", "co.jp", "ne.jp", "or.jp", "ac.jp", "go.jp", "co.kr", "or.kr", "com.cn", "net.cn",
      "org.cn", "gov.cn", "com.hk", "com.tw", "com.sg", "com.my", "co.in", "net.in", "org.in", "gov.in",
      "co.id", "co.th", "com.ph", "com.vn", "com.pk", "com.tr", "com.br", "net.br", "org.br", "gov.br",
      "com.ar", "com.mx", "com.co", "com.pe", "com.ve", "co.za", "org.za", "com.ng", "co.ke", "com.eg",
      "com.sa", "co.il", "org.il", "ac.il", "com.ua", "com.pl", "com.ru", "com.es", "com.pt", "co.at",
      "or.at", "com.de", "co.it", "com.gr", "github.io", "githubusercontent.com", "gitlab.io",
      "herokuapp.com", "herokussl.com", "azurewebsites.net", "cloudapp.net", "azurestaticapps.net",
      "azure-api.net", "trafficmanager.net", "blob.core.windows.net", "cloudfront.net",
      "elasticbeanstalk.com", "amazonaws.com", "s3.amazonaws.com", "appspot.com", "web.app",
      "firebaseapp.com", "run.app", "cloudfunctions.net", "blogspot.com", "netlify.app", "netlify.com",
      "vercel.app", "now.sh", "pages.dev", "workers.dev", "fly.dev", "onrender.com", "glitch.me",
      "repl.co", "ngrok.io", "ngrok-free.app", "ngrok.app", "loca.lt", "duckdns.org", "dyndns.org",
      "no-ip.org", "surge.sh", "readthedocs.io", "bitbucket.io", "codeberg.page", "pythonanywhere.com",
      "nip.io", "sslip.io", "localhost.run"};
  for (const char* s : kList)
    if (d == s) return true;
  return false;
}

}  // namespace cookies

namespace {

bool ignore(std::string* why, const char* msg) {
  if (why) *why = msg;
  return false;
}

struct Parsed {
  Cookie c;
  bool has_domain = false;
  bool has_max_age = false;
  int64_t max_age = 0;
  bool has_expires = false;
  int64_t expires = 0;
};

bool parse_set_cookie(std::string_view h, Parsed& p, std::string* why) {
  const size_t semi = h.find(';');
  std::string_view nv = h.substr(0, semi);
  std::string_view attrs = semi == std::string_view::npos ? std::string_view{} : h.substr(semi + 1);
  if (const size_t eq = nv.find('='); eq == std::string_view::npos) {
    p.c.name.clear();
    p.c.value = std::string(trim(nv));  // RFC 6265bis: a nameless cookie
  } else {
    p.c.name = std::string(trim(nv.substr(0, eq)));
    p.c.value = std::string(trim(nv.substr(eq + 1)));
  }
  if (p.c.name.empty() && p.c.value.empty()) return ignore(why, "empty cookie");
  if (has_ctl(p.c.name) || has_ctl(p.c.value)) return ignore(why, "control character in name or value");
  if (p.c.name.size() + p.c.value.size() > kMaxCookieBytes) return ignore(why, "cookie larger than 4096 bytes");
  p.c.path.clear();
  while (!attrs.empty()) {
    const size_t n = attrs.find(';');
    std::string_view av = attrs.substr(0, n);
    attrs = n == std::string_view::npos ? std::string_view{} : attrs.substr(n + 1);
    std::string_view key = av, val;
    if (const size_t eq = av.find('='); eq != std::string_view::npos) {
      key = av.substr(0, eq);
      val = av.substr(eq + 1);
    }
    key = trim(key);
    val = trim(val);
    if (val.size() > 1024) continue;  // RFC 6265bis: oversized attribute values are ignored
    if (ieq(key, "expires")) {
      int64_t t;
      if (cookies::parse_cookie_date(val, t)) p.has_expires = true, p.expires = t;
    } else if (ieq(key, "max-age")) {
      if (val.empty()) continue;
      size_t k = val[0] == '-' ? 1 : 0;
      if (k == val.size()) continue;
      bool digits = true;
      for (size_t q = k; q < val.size(); ++q) digits = digits && val[q] >= '0' && val[q] <= '9';
      if (!digits) continue;
      int64_t v = 0;
      for (size_t q = k; q < val.size(); ++q) {
        v = v * 10 + (val[q] - '0');
        if (v > 400LL * 86400 * 365) break;  // clamp absurd values
      }
      p.has_max_age = true;
      p.max_age = k ? -v : v;
    } else if (ieq(key, "domain")) {
      if (val.empty()) continue;
      if (val.front() == '.') val.remove_prefix(1);
      while (!val.empty() && val.back() == '.') val.remove_suffix(1);
      if (val.empty()) continue;
      p.has_domain = true;
      p.c.domain = lower_str(val);
    } else if (ieq(key, "path")) {
      p.c.path = (val.empty() || val.front() != '/') ? std::string() : std::string(val);
    } else if (ieq(key, "secure")) {
      p.c.secure = true;
    } else if (ieq(key, "httponly")) {
      p.c.http_only = true;
    } else if (ieq(key, "samesite")) {
      if (ieq(val, "strict")) p.c.same_site = SameSite::Strict;
      else if (ieq(val, "lax")) p.c.same_site = SameSite::Lax;
      else if (ieq(val, "none")) p.c.same_site = SameSite::None;
    }
  }
  return true;
}

const Cookie* find_cookie(const std::vector<Cookie>& v, const Cookie& c) {
  for (const auto& e : v)
    if (e.name == c.name && e.domain == c.domain && e.path == c.path) return &e;
  return nullptr;
}

std::string jar_key(const fs::path& root) {
  std::error_code ec;
  fs::path p = fs::weakly_canonical(fs::absolute(root, ec), ec);
  if (ec) p = root;
  std::string k = detail::to_utf8(p.lexically_normal());
#ifdef _WIN32
  k = lower_str(k);
  std::replace(k.begin(), k.end(), '/', '\\');
#endif
  while (k.size() > 1 && (k.back() == '\\' || k.back() == '/')) k.pop_back();
  return k;
}

std::string serialize(const std::vector<Cookie>& v) {
  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> w(sb);
  w.StartArray();
  for (const auto& c : v) {
    w.StartObject();
    w.Key("name"), w.String(c.name.data(), static_cast<rapidjson::SizeType>(c.name.size()));
    w.Key("value"), w.String(c.value.data(), static_cast<rapidjson::SizeType>(c.value.size()));
    w.Key("domain"), w.String(c.domain.data(), static_cast<rapidjson::SizeType>(c.domain.size()));
    w.Key("path"), w.String(c.path.data(), static_cast<rapidjson::SizeType>(c.path.size()));
    w.Key("host_only"), w.Bool(c.host_only);
    w.Key("secure"), w.Bool(c.secure);
    w.Key("http_only"), w.Bool(c.http_only);
    w.Key("same_site"), w.Int(static_cast<int>(c.same_site));
    w.Key("expires"), w.Int64(c.expires);
    w.Key("created"), w.Int64(c.created);
    w.Key("last_access"), w.Int64(c.last_access);
    w.EndObject();
  }
  w.EndArray();
  return std::string(sb.GetString(), sb.GetSize());
}

bool deserialize(const std::string& text, std::vector<Cookie>& out) {
  rapidjson::Document d;
  d.Parse(text.data(), text.size());
  if (d.HasParseError() || !d.IsArray()) return false;
  auto str = [](const rapidjson::Value& o, const char* k) {
    auto it = o.FindMember(k);
    return it != o.MemberEnd() && it->value.IsString() ? std::string(it->value.GetString(), it->value.GetStringLength())
                                                       : std::string();
  };
  auto boolean = [](const rapidjson::Value& o, const char* k) {
    auto it = o.FindMember(k);
    return it != o.MemberEnd() && it->value.IsBool() && it->value.GetBool();
  };
  auto i64 = [](const rapidjson::Value& o, const char* k) {
    auto it = o.FindMember(k);
    return it != o.MemberEnd() && it->value.IsInt64() ? it->value.GetInt64() : int64_t{0};
  };
  for (const auto& o : d.GetArray()) {
    if (!o.IsObject()) continue;
    Cookie c;
    c.name = str(o, "name");
    c.value = str(o, "value");
    c.domain = str(o, "domain");
    c.path = str(o, "path");
    c.host_only = boolean(o, "host_only");
    c.secure = boolean(o, "secure");
    c.http_only = boolean(o, "http_only");
    const int64_t ss = i64(o, "same_site");
    c.same_site = ss >= 0 && ss <= 3 ? static_cast<SameSite>(ss) : SameSite::Unset;
    c.expires = i64(o, "expires");
    c.created = i64(o, "created");
    c.last_access = i64(o, "last_access");
    out.push_back(std::move(c));
  }
  return true;
}

}  // namespace

CookieJar::CookieJar(fs::path file) : file_(std::move(file)) {
  if (file_.empty()) loaded_ = true;
}

CookieJar::~CookieJar() {
  std::unique_lock lk(mu_);
  cv_.wait(lk, [&] { return !writer_running_; });
}

void CookieJar::ensure_loaded_locked() {
  if (!loaded_) load_locked();
}

void CookieJar::load_locked() {
  loaded_ = true;
  cookies_.clear();
  rejected_.clear();
  if (file_.empty()) return;
  std::string text;
  std::error_code ec;
  if (!fs::exists(file_, ec)) return;
  if (!detail::read_file(file_, text)) {
    rejected_ = "cookies.json could not be read: ignored";
    return;
  }
  rapidjson::Document d;
  d.Parse(text.data(), text.size());
  if (d.HasParseError() || !d.IsObject() || !d.HasMember("jar") || !d["jar"].IsString()) {
    rejected_ = "cookies.json is not a watt cookie jar: ignored";
    return;
  }
  const std::string stored(d["jar"].GetString(), d["jar"].GetStringLength());
  std::string plain;
  const detail::Unseal u = detail::unseal_secret(stored, plain);
#ifdef _WIN32
  // Only a jar this Windows user sealed counts: a plaintext or foreign file (planted in a cloned
  // repo, copied from another account) is never sent.
  if (u != detail::Unseal::Ok) {
    rejected_ = "cookies.json was not sealed by this Windows user (planted or copied file): ignored";
    return;
  }
#else
  if (u == detail::Unseal::Failed) {
    rejected_ = "cookies.json could not be unsealed: ignored";
    return;
  }
#endif
  if (plain.compare(0, kJarMagic.size(), kJarMagic) != 0) {
    rejected_ = "cookies.json does not hold a watt cookie jar: ignored";
    return;
  }
  std::vector<Cookie> v;
  if (!deserialize(plain.substr(kJarMagic.size()), v)) {
    rejected_ = "cookies.json is corrupt: ignored";
    return;
  }
  // Re-validate every entry, as if it had come from a server.
  for (auto& c : v) {
    if (c.domain.empty() || c.path.empty() || c.path.front() != '/' || has_ctl(c.name) || has_ctl(c.value) ||
        has_ctl(c.domain) || has_ctl(c.path) || c.name.size() + c.value.size() > kMaxCookieBytes)
      continue;
    if (!c.host_only && cookies::is_public_suffix(c.domain)) continue;
    if (find_cookie(cookies_, c)) continue;
    cookies_.push_back(std::move(c));
    if (cookies_.size() >= kMaxCookies) break;
  }
}

void CookieJar::preload() {
  std::lock_guard lk(mu_);
  ensure_loaded_locked();
}

void CookieJar::reload() {
  std::unique_lock lk(mu_);
  cv_.wait(lk, [&] { return !writer_running_; });
  load_locked();
}

std::string CookieJar::load_rejected() {
  std::lock_guard lk(mu_);
  ensure_loaded_locked();
  return rejected_;
}

void CookieJar::changed_locked() {
  if (file_.empty()) return;
  ++gen_;
  if (writer_running_) return;  // the running writer picks the new generation up
  writer_running_ = true;
  std::thread([this] { writer_loop(); }).detach();
}

void CookieJar::writer_loop() {
  std::unique_lock lk(mu_);
  while (written_gen_ != gen_) {
    const uint64_t g = gen_;
    const int64_t t = cookies::now();
    std::vector<Cookie> snap;
    snap.reserve(cookies_.size());
    for (const auto& c : cookies_)
      if (c.session() || c.expires > t) snap.push_back(c);
    lk.unlock();
    std::string err;
    std::string stored;
    bool ok = detail::seal_secret(std::string(kJarMagic) + serialize(snap), stored);
    if (!ok) err = "cannot seal the cookie jar";
    if (ok) {
      rapidjson::StringBuffer sb;
      rapidjson::Writer<rapidjson::StringBuffer> w(sb);
      w.StartObject();
      w.Key("version"), w.Int(1);
      w.Key("jar"), w.String(stored.data(), static_cast<rapidjson::SizeType>(stored.size()));
      w.EndObject();
      std::error_code ec;
      const fs::path dir = file_.parent_path();
      if (!fs::is_directory(dir, ec)) detail::create_private_dir(dir);
      ok = detail::write_file_atomic(file_, std::string(sb.GetString(), sb.GetSize()) + "\n", &err, true);
    }
    lk.lock();
    write_error_ = ok ? std::string() : (err.empty() ? "cannot write cookies.json" : err);
    written_gen_ = g;
  }
  writer_running_ = false;
  cv_.notify_all();
}

bool CookieJar::flush(std::string* err) {
  std::unique_lock lk(mu_);
  cv_.wait(lk, [&] { return !writer_running_ && written_gen_ == gen_; });
  if (err) *err = write_error_;
  return write_error_.empty();
}

void CookieJar::evict_locked(const std::string& domain) {
  auto lru = [&](auto pred) {
    auto victim = cookies_.end();
    for (auto it = cookies_.begin(); it != cookies_.end(); ++it)
      if (pred(*it) && (victim == cookies_.end() || it->last_access < victim->last_access)) victim = it;
    if (victim != cookies_.end()) cookies_.erase(victim);
  };
  size_t n = 0;
  for (const auto& c : cookies_) n += c.domain == domain;
  while (n-- > kMaxPerDomain) lru([&](const Cookie& c) { return c.domain == domain; });
  while (cookies_.size() > kMaxCookies) lru([](const Cookie&) { return true; });
}

bool CookieJar::upsert_locked(Cookie c, std::string* why, bool) {
  ensure_loaded_locked();
  const int64_t t = c.last_access;
  for (auto it = cookies_.begin(); it != cookies_.end(); ++it)
    if (it->name == c.name && it->domain == c.domain && it->path == c.path) {
      c.created = it->created ? it->created : c.created;
      if (!c.session() && c.expires <= t) {  // expired: a delete
        cookies_.erase(it);
        changed_locked();
        return true;
      }
      if (*it == c) return false;
      *it = std::move(c);
      changed_locked();
      return true;
    }
  if (!c.session() && c.expires <= t) return ignore(why, "already expired");
  const std::string domain = c.domain;
  cookies_.push_back(std::move(c));
  evict_locked(domain);
  changed_locked();
  return true;
}

bool CookieJar::set_from_header(std::string_view request_url, std::string_view set_cookie, std::string* why,
                                int64_t t) {
  const Url u = parse_url(request_url);
  if (!u.ok) return ignore(why, "not an http(s) URL");
  Parsed p;
  if (!parse_set_cookie(set_cookie, p, why)) return false;
  Cookie& c = p.c;
  if (p.has_domain) {
    if (cookies::is_public_suffix(c.domain)) {
      if (c.domain != u.host) return ignore(why, "Domain is a public suffix");
      p.has_domain = false;  // RFC 6265 5.3 step 5: host-only for the suffix itself
    } else if (is_ip(u.host)) {
      if (c.domain != u.host) return ignore(why, "Domain does not match the IP address host");
      p.has_domain = false;
    } else if (!cookies::domain_match(u.host, c.domain)) {
      return ignore(why, "Domain does not match the request host");
    }
  }
  c.host_only = !p.has_domain;
  if (c.host_only) c.domain = u.host;
  if (c.path.empty()) c.path = default_path(u.path);
  const bool secure_from = secure_origin(u);
  if (c.secure && !secure_from) return ignore(why, "Secure cookie from an insecure origin");
  if (starts_with_ci(c.name, "__Secure-") && !c.secure) return ignore(why, "__Secure- cookie without Secure");
  if (starts_with_ci(c.name, "__Host-") && (!c.secure || !c.host_only || c.path != "/"))
    return ignore(why, "__Host- cookie needs Secure, no Domain and Path=/");
  // An expiry at or before now (Max-Age <= 0, a past Expires) deletes the stored cookie.
  const int64_t gone = t > 1 ? t - 1 : 1;
  if (p.has_max_age) c.expires = p.max_age <= 0 ? gone : t + p.max_age;
  else if (p.has_expires) c.expires = p.expires <= t ? gone : p.expires;
  else c.expires = 0;
  c.created = t;
  c.last_access = t;
  std::lock_guard lk(mu_);
  ensure_loaded_locked();
  if (!secure_from && !c.secure) {
    // RFC 6265bis 5.6 step 16: an insecure origin cannot shadow a Secure cookie.
    for (const auto& e : cookies_)
      if (e.secure && e.name == c.name &&
          (cookies::domain_match(e.domain, c.domain) || cookies::domain_match(c.domain, e.domain)) &&
          cookies::path_match(c.path, e.path))
        return ignore(why, "would overwrite a Secure cookie from an insecure origin");
  }
  return upsert_locked(std::move(c), why, true);
}

size_t CookieJar::record(std::string_view url, const ResponseHeaders& headers, int64_t t) {
  size_t n = 0;
  for (const auto& [k, v] : headers)
    if (ieq(k, "set-cookie")) n += set_from_header(url, v, nullptr, t);
  return n;
}

size_t CookieJar::record(std::string_view url, const std::vector<std::pair<std::string, std::string>>& headers,
                         int64_t t) {
  size_t n = 0;
  for (const auto& [k, v] : headers)
    if (ieq(k, "set-cookie")) n += set_from_header(url, v, nullptr, t);
  return n;
}

std::string CookieJar::cookie_header(std::string_view url, int64_t t) {
  const Url u = parse_url(url);
  if (!u.ok) return {};
  const bool secure_to = secure_origin(u);
  std::lock_guard lk(mu_);
  ensure_loaded_locked();
  std::vector<Cookie*> hit;
  bool expired = false;
  for (auto& c : cookies_) {
    if (!c.session() && c.expires <= t) {
      expired = true;
      continue;
    }
    if (c.host_only ? c.domain != u.host : !cookies::domain_match(u.host, c.domain)) continue;
    if (!cookies::path_match(u.path, c.path)) continue;
    if (c.secure && !secure_to) continue;
    hit.push_back(&c);
  }
  if (expired) {
    std::erase_if(cookies_, [&](const Cookie& c) { return !c.session() && c.expires <= t; });
    changed_locked();
    hit.clear();
    for (auto& c : cookies_) {
      if (c.host_only ? c.domain != u.host : !cookies::domain_match(u.host, c.domain)) continue;
      if (!cookies::path_match(u.path, c.path) || (c.secure && !secure_to)) continue;
      hit.push_back(&c);
    }
  }
  std::stable_sort(hit.begin(), hit.end(), [](const Cookie* a, const Cookie* b) {
    if (a->path.size() != b->path.size()) return a->path.size() > b->path.size();
    return a->created < b->created;
  });
  std::string out;
  for (Cookie* c : hit) {
    if (!out.empty()) out += "; ";
    if (!c->name.empty()) out += c->name + "=";
    out += c->value;
    c->last_access = t;  // in memory only: not worth a disk write per request
  }
  return out;
}

std::vector<Cookie> CookieJar::list(int64_t t) {
  std::lock_guard lk(mu_);
  ensure_loaded_locked();
  std::vector<Cookie> out;
  for (const auto& c : cookies_)
    if (c.session() || c.expires > t) out.push_back(c);
  std::sort(out.begin(), out.end(), [](const Cookie& a, const Cookie& b) {
    return std::tie(a.domain, a.path, a.name) < std::tie(b.domain, b.path, b.name);
  });
  return out;
}

size_t CookieJar::size(int64_t t) { return list(t).size(); }

bool CookieJar::upsert(Cookie c, std::string* why) {
  std::string_view d = c.domain;
  while (!d.empty() && d.front() == '.') d.remove_prefix(1);
  c.domain = lower_str(d);
  if (c.domain.empty()) return ignore(why, "empty domain");
  if (has_ctl(c.name) || has_ctl(c.value) || has_ctl(c.domain) || has_ctl(c.path) ||
      c.name.find_first_of(";=") != std::string::npos || c.value.find(';') != std::string::npos ||
      c.domain.find_first_of(" ;,/") != std::string::npos)
    return ignore(why, "invalid character");
  if (c.name.empty() && c.value.empty()) return ignore(why, "empty cookie");
  if (c.name.size() + c.value.size() > kMaxCookieBytes) return ignore(why, "cookie larger than 4096 bytes");
  if (c.path.empty() || c.path.front() != '/') return ignore(why, "path must start with '/'");
  if (!c.host_only && cookies::is_public_suffix(c.domain)) return ignore(why, "domain is a public suffix");
  const int64_t t = cookies::now();
  if (!c.created) c.created = t;
  c.last_access = t;
  std::lock_guard lk(mu_);
  return upsert_locked(std::move(c), why, true);
}

bool CookieJar::remove(std::string_view domain, std::string_view path, std::string_view name) {
  std::lock_guard lk(mu_);
  ensure_loaded_locked();
  const std::string d = lower_str(domain);
  const size_t n = std::erase_if(cookies_, [&](const Cookie& c) { return c.domain == d && c.path == path && c.name == name; });
  if (n) changed_locked();
  return n > 0;
}

size_t CookieJar::remove_domain(std::string_view domain) {
  std::string_view dv = domain;
  while (!dv.empty() && dv.front() == '.') dv.remove_prefix(1);
  const std::string d = lower_str(dv);
  std::lock_guard lk(mu_);
  ensure_loaded_locked();
  const size_t n = std::erase_if(cookies_, [&](const Cookie& c) { return c.domain == d || cookies::domain_match(c.domain, d); });
  if (n) changed_locked();
  return n;
}

void CookieJar::clear() {
  std::lock_guard lk(mu_);
  ensure_loaded_locked();
  cookies_.clear();
  rejected_.clear();
  changed_locked();
}

std::shared_ptr<CookieJar> cookie_jar_for(const fs::path& root) {
  static std::mutex mu;
  static auto* jars = new std::map<std::string, std::shared_ptr<CookieJar>>();  // never destroyed: detached writers
  const std::string key = jar_key(root);
  std::lock_guard lk(mu);
  auto& j = (*jars)[key];
  if (!j) j = std::make_shared<CookieJar>(detail::from_utf8(key) / ".watt" / "cookies.json");
  return j;
}

}  // namespace watt
