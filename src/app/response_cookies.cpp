// See response_cookies.h.
#include "app/response_cookies.h"

#include <cstdio>
#include <ctime>

#include "core/cookies.h"

namespace watt::app::rv {

namespace {

bool ieq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

// The name / value and the attributes the jar does not keep (Partitioned), read from the
// header itself: shown also for a cookie the jar ignores.
void parse_own(std::string_view h, SetCookieRow& r) {
  const size_t semi = h.find(';');
  const std::string_view nv = h.substr(0, semi);
  if (const size_t eq = nv.find('='); eq == std::string_view::npos) {
    r.value = std::string(trim(nv));
  } else {
    r.name = std::string(trim(nv.substr(0, eq)));
    r.value = std::string(trim(nv.substr(eq + 1)));
  }
  r.size = r.name.size() + r.value.size();
  std::string_view attrs = semi == std::string_view::npos ? std::string_view{} : h.substr(semi + 1);
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
    if (ieq(key, "partitioned")) r.partitioned = true;
    else if (ieq(key, "secure")) r.secure = true;
    else if (ieq(key, "httponly")) r.http_only = true;
    else if (ieq(key, "path") && !val.empty() && val.front() == '/') r.path = std::string(val);
    else if (ieq(key, "domain") && !val.empty()) {
      while (!val.empty() && val.front() == '.') val.remove_prefix(1);
      r.domain = std::string(val);
      r.host_only = r.domain.empty();
    } else if (ieq(key, "samesite")) {
      if (ieq(val, "lax")) r.same_site = "Lax";
      else if (ieq(val, "strict")) r.same_site = "Strict";
      else if (ieq(val, "none")) r.same_site = "None";
    }
  }
}

}  // namespace

std::string cookie_expiry_text(int64_t at, int64_t now) {
  if (at == 0) return "Session";
  const int64_t d = at - now;
  if (d <= 0) return "expired";
  if (d < 90) return "in " + std::to_string(d) + " s";
  if (d < 3600) return "in " + std::to_string((d + 30) / 60) + " min";
  if (d < 36 * 3600) return "in " + std::to_string((d + 1800) / 3600) + " h";
  if (d < 365LL * 86400) return "in " + std::to_string((d + 43200) / 86400) + " days";
  const std::time_t t = static_cast<std::time_t>(at);
  std::tm tm{};
#ifdef _WIN32
  if (gmtime_s(&tm, &t) != 0) return "far future";
#else
  if (!gmtime_r(&t, &tm)) return "far future";
#endif
  char buf[16];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  return buf;
}

std::string cookie_flags_text(const SetCookieRow& r) {
  std::string o;
  auto add = [&](const char* s) {
    if (!o.empty()) o += ' ';
    o += s;
  };
  if (r.http_only) add("HttpOnly");
  if (r.secure) add("Secure");
  if (!r.same_site.empty()) add(r.same_site.c_str());
  if (r.partitioned) add("Partitioned");
  return o;
}

std::string cookie_fate_text(const SetCookieRow& r) {
  switch (r.fate) {
    case SetCookieRow::Fate::Stored: return "stored";
    case SetCookieRow::Fate::Deletes: return "deletes it";
    default: return "ignored: " + r.why;
  }
}

std::vector<SetCookieRow> set_cookie_rows(const std::vector<std::pair<std::string, std::string>>& headers,
                                          std::string_view url, int64_t now) {
  std::vector<SetCookieRow> out;
  for (const auto& [k, v] : headers) {
    if (!ieq(k, "set-cookie")) continue;
    SetCookieRow r;
    r.raw = v;
    parse_own(v, r);
    // What the workspace jar does with it, by the jar's own rules: a jar of its own (memory only)
    // per header, so the outcome is this header's alone.
    CookieJar jar;
    std::string why;
    const bool stored = jar.set_from_header(url, v, &why, now);
    const std::vector<Cookie> got = stored ? jar.list(now) : std::vector<Cookie>{};
    if (!got.empty()) {
      const Cookie& c = got.front();
      r.name = c.name;
      r.value = c.value;
      r.domain = c.domain;
      r.host_only = c.host_only;
      r.path = c.path;
      r.expires_at = c.expires;
      r.http_only = c.http_only;
      r.secure = c.secure;
      r.same_site = same_site_name(c.same_site);
      r.fate = SetCookieRow::Fate::Stored;
      r.expires = cookie_expiry_text(c.expires, now);
    } else if (why == "already expired") {
      r.fate = SetCookieRow::Fate::Deletes;
      r.expires = "now (deletes)";
      r.expires_at = now > 1 ? now - 1 : 1;
    } else {
      r.fate = SetCookieRow::Fate::Ignored;
      r.why = why.empty() ? std::string("not stored") : why;
      r.expires = "\xE2\x80\x94";
    }
    if (r.domain.empty()) {
      // Ignored before the jar settled it: the request host (what it would have been).
      std::string_view u = url;
      if (const size_t s = u.find("://"); s != std::string_view::npos) u.remove_prefix(s + 3);
      u = u.substr(0, u.find_first_of("/?#"));
      if (const size_t at = u.rfind('@'); at != std::string_view::npos) u.remove_prefix(at + 1);
      if (!u.empty() && u.front() != '[')
        if (const size_t c = u.rfind(':'); c != std::string_view::npos) u = u.substr(0, c);
      r.domain = std::string(u);
      r.host_only = true;
    }
    out.push_back(std::move(r));
  }
  return out;
}

}  // namespace watt::app::rv
