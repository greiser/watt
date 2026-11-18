#include "app/sidebar_cookies_text.h"

#include <algorithm>
#include <cstdio>
#include <map>

namespace watt::app::sidebar {

namespace {

// Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's days_from_civil).
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

struct Civil {
  int64_t y;
  unsigned m, d, hh, mm, ss;
};

Civil civil_from_unix(int64_t t) {
  int64_t days = t / 86400;
  int64_t rem = t % 86400;
  if (rem < 0) {
    rem += 86400;
    --days;
  }
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(days - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  const int64_t y = static_cast<int64_t>(yoe) + era * 400 + (m <= 2);
  return {y, m, d, static_cast<unsigned>(rem / 3600), static_cast<unsigned>(rem / 60 % 60),
          static_cast<unsigned>(rem % 60)};
}

std::string ck_format_utc(int64_t t, bool seconds) {
  const Civil c = civil_from_unix(t);
  char buf[48];
  if (seconds)
    std::snprintf(buf, sizeof buf, "%04lld-%02u-%02u %02u:%02u:%02u", static_cast<long long>(c.y), c.m, c.d, c.hh,
                  c.mm, c.ss);
  else
    std::snprintf(buf, sizeof buf, "%04lld-%02u-%02u %02u:%02u", static_cast<long long>(c.y), c.m, c.d, c.hh, c.mm);
  return buf;
}

std::string ck_trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return std::string(s);
}

char ck_lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool ck_icontains(std::string_view hay, std::string_view needle) {
  if (needle.empty()) return true;
  if (needle.size() > hay.size()) return false;
  for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
    size_t k = 0;
    while (k < needle.size() && ck_lower(hay[i + k]) == ck_lower(needle[k])) ++k;
    if (k == needle.size()) return true;
  }
  return false;
}

// Reads n digits at s[i]; advances i.
bool ck_digits(std::string_view s, size_t& i, size_t n, unsigned& out) {
  if (i + n > s.size()) return false;
  out = 0;
  for (size_t k = 0; k < n; ++k) {
    const char c = s[i + k];
    if (c < '0' || c > '9') return false;
    out = out * 10 + static_cast<unsigned>(c - '0');
  }
  i += n;
  return true;
}

bool parse_iso(std::string_view s, int64_t& out) {
  size_t i = 0;
  unsigned y, m, d, hh = 0, mm = 0, ss = 0;
  if (!ck_digits(s, i, 4, y) || i >= s.size() || s[i++] != '-' || !ck_digits(s, i, 2, m) || i >= s.size() ||
      s[i++] != '-' || !ck_digits(s, i, 2, d))
    return false;
  if (i < s.size() && (s[i] == ' ' || s[i] == 'T' || s[i] == 't')) {
    ++i;
    if (!ck_digits(s, i, 2, hh) || i >= s.size() || s[i++] != ':' || !ck_digits(s, i, 2, mm)) return false;
    if (i < s.size() && s[i] == ':') {
      ++i;
      if (!ck_digits(s, i, 2, ss)) return false;
    }
  }
  std::string_view rest = s.substr(i);
  while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
  if (!(rest.empty() || rest == "Z" || rest == "z" || rest == "UTC" || rest == "GMT")) return false;
  if (m < 1 || m > 12 || d < 1 || d > 31 || hh > 23 || mm > 59 || ss > 60) return false;
  out = days_from_civil(y, m, d) * 86400 + hh * 3600 + mm * 60 + ss;
  return true;
}

std::string ck_relative(int64_t secs) {
  char buf[32];
  if (secs < 3600) std::snprintf(buf, sizeof buf, "in %lld min", static_cast<long long>(std::max<int64_t>(1, secs / 60)));
  else if (secs < 2 * 86400) std::snprintf(buf, sizeof buf, "in %lld h", static_cast<long long>(secs / 3600));
  else if (secs < 730 * 86400) std::snprintf(buf, sizeof buf, "in %lld d", static_cast<long long>(secs / 86400));
  else std::snprintf(buf, sizeof buf, "in %lld y", static_cast<long long>(secs / (365 * 86400)));
  return buf;
}

}  // namespace

std::string cookie_expiry_label(const Cookie& c, int64_t now) {
  if (c.session()) return "Session";
  std::string s = ck_format_utc(c.expires, false) + " UTC";
  s += c.expires <= now ? " (expired)" : " (" + ck_relative(c.expires - now) + ")";
  return s;
}

std::string cookie_expiry_edit_text(const Cookie& c) { return c.session() ? std::string() : ck_format_utc(c.expires, true); }

bool parse_cookie_expiry(std::string_view text_in, int64_t now, int64_t& out) {
  const std::string t = ck_trim(text_in);
  std::string l;
  for (char c : t) l.push_back(ck_lower(c));
  if (l.empty() || l == "session") {
    out = 0;
    return true;
  }
  if (l[0] == '+' && l.size() >= 3) {
    int64_t n = 0;
    size_t i = 1;
    while (i < l.size() && l[i] >= '0' && l[i] <= '9' && n < 100000000) n = n * 10 + (l[i++] - '0');
    if (i == 1 || i + 1 != l.size()) return false;
    const char u = l[i];
    const int64_t mul = u == 's' ? 1 : u == 'm' ? 60 : u == 'h' ? 3600 : u == 'd' ? 86400 : u == 'y' ? 365 * 86400 : 0;
    if (!mul || n <= 0) return false;
    out = now + n * mul;
    return true;
  }
  if (parse_iso(t, out)) return out > 0;
  int64_t v = 0;
  if (cookies::parse_cookie_date(t, v) && v > 0) {
    out = v;
    return true;
  }
  return false;
}

std::string cookie_flags_label(const Cookie& c) {
  std::string s;
  auto add = [&](std::string_view p) {
    if (!s.empty()) s += "  ";
    s += p;
  };
  if (c.secure) add("Secure");
  if (c.http_only) add("HttpOnly");
  if (c.same_site != SameSite::Unset) add(std::string("SameSite=") + same_site_name(c.same_site));
  return s;
}

std::string set_cookie_line(const Cookie& c) {
  std::string s = c.name + "=" + c.value;
  if (!c.host_only) s += "; Domain=" + c.domain;
  s += "; Path=" + (c.path.empty() ? std::string("/") : c.path);
  if (!c.session()) {
    static const char* const wd[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
    static const char* const mo[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const Civil x = civil_from_unix(c.expires);
    int64_t days = c.expires / 86400;
    if (c.expires % 86400 < 0) --days;
    const int64_t w = ((days % 7) + 7) % 7;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s, %02u %s %04lld %02u:%02u:%02u GMT", wd[w], x.d, mo[x.m - 1],
                  static_cast<long long>(x.y), x.hh, x.mm, x.ss);
    s += "; Expires=";
    s += buf;
  }
  if (c.secure) s += "; Secure";
  if (c.http_only) s += "; HttpOnly";
  if (c.same_site != SameSite::Unset) s += std::string("; SameSite=") + same_site_name(c.same_site);
  return s;
}

std::vector<CookieDomain> group_cookies(const std::vector<Cookie>& list, std::string_view filter_in) {
  const std::string filter = ck_trim(filter_in);
  std::map<std::string, std::vector<size_t>> by;
  for (size_t i = 0; i < list.size(); ++i) {
    const Cookie& c = list[i];
    if (!filter.empty() && !ck_icontains(c.domain, filter) && !ck_icontains(c.name, filter) && !ck_icontains(c.value, filter))
      continue;
    by[c.domain].push_back(i);
  }
  std::vector<CookieDomain> out;
  out.reserve(by.size());
  for (auto& [d, v] : by) out.push_back({d, std::move(v)});
  return out;
}

std::string cookie_origin_url(std::string_view domain_in) {
  std::string d = ck_trim(domain_in);
  while (!d.empty() && d.front() == '.') d.erase(0, 1);
  if (d.empty() || d.size() > 253) return {};
  for (char& c : d) {
    c = ck_lower(c);
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_';
    if (!ok) return {};
  }
  return "https://" + d + "/";
}

}  // namespace watt::app::sidebar
