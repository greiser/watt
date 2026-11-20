#include "app/http_editor_cookies.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>

#include "app/http_editor_util.h"

namespace watt::app::http_editor {

namespace {

std::string lower(std::string_view s) {
  std::string o(s);
  for (char& ch : o)
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  return o;
}

struct ParsedUrl {
  bool ok = false;
  bool https = false;
  std::string host;  // lower case, IPv6 without brackets
  std::string path = "/";
};

ParsedUrl parse(std::string_view url) {
  ParsedUrl u;
  url = trim(url);
  size_t colon = url.find("://");
  if (colon == std::string_view::npos) {
    // "127.0.0.1:8080/x" is sent as http:// (as the send path normalizes it); "{{base}}/x" and
    // other text that does not start with a host are not URLs yet.
    if (url.empty() || url.find("{{") != std::string_view::npos || url.front() == '/') return u;
    std::string with = "http://" + std::string(url);
    return parse(with);
  }
  const std::string scheme = lower(url.substr(0, colon));
  if (scheme != "http" && scheme != "https") return u;
  u.https = scheme == "https";
  std::string_view rest = url.substr(colon + 3);
  const size_t end = rest.find_first_of("/?#");
  std::string_view auth = rest.substr(0, end);
  if (const size_t at = auth.rfind('@'); at != std::string_view::npos) auth.remove_prefix(at + 1);
  std::string_view host = auth;
  if (!host.empty() && host.front() == '[') {
    const size_t rb = host.find(']');
    if (rb == std::string_view::npos) return u;
    host = host.substr(1, rb - 1);
  } else if (const size_t pc = host.rfind(':'); pc != std::string_view::npos) {
    host = host.substr(0, pc);
  }
  while (!host.empty() && host.back() == '.') host.remove_suffix(1);
  if (host.empty()) return u;
  u.host = lower(host);
  if (end != std::string_view::npos && rest[end] == '/') {
    std::string_view p = rest.substr(end);
    p = p.substr(0, p.find_first_of("?#"));
    u.path = std::string(p);
  }
  u.ok = true;
  return u;
}

bool loopback(const std::string& h) {
  return h == "localhost" || h == "::1" || h.rfind("127.", 0) == 0 ||
         (h.size() > 10 && h.compare(h.size() - 10, 10, ".localhost") == 0);
}

std::string two(int v) {
  char b[8];
  std::snprintf(b, sizeof b, "%02d", v);
  return b;
}

bool utc_tm(int64_t t, std::tm& out) {
  const std::time_t tt = static_cast<std::time_t>(t);
#ifdef _WIN32
  return gmtime_s(&out, &tt) == 0;
#else
  return gmtime_r(&tt, &out) != nullptr;
#endif
}

std::string utc_text(int64_t t) {
  std::tm tm{};
  if (!utc_tm(t, tm)) return {};
  return std::to_string(tm.tm_year + 1900) + "-" + two(tm.tm_mon + 1) + "-" + two(tm.tm_mday) + " " + two(tm.tm_hour) +
         ":" + two(tm.tm_min) + ":" + two(tm.tm_sec);
}
}  // namespace

// ------------------------------------------------------------------ pure helpers

std::vector<Cookie> jar_cookies_for(const std::vector<Cookie>& all, std::string_view url, int64_t now) {
  std::vector<Cookie> out;
  const ParsedUrl u = parse(url);
  if (!u.ok) return out;
  const bool secure_to = u.https || loopback(u.host);
  for (const auto& c : all) {
    if (!c.session() && c.expires <= now) continue;
    if (c.host_only ? c.domain != u.host : !cookies::domain_match(u.host, c.domain)) continue;
    if (!cookies::path_match(u.path, c.path)) continue;
    if (c.secure && !secure_to) continue;
    out.push_back(c);
  }
  std::stable_sort(out.begin(), out.end(), [](const Cookie& a, const Cookie& b) {
    if (a.path.size() != b.path.size()) return a.path.size() > b.path.size();
    return a.created < b.created;
  });
  return out;
}

std::string url_host(std::string_view url) { return parse(url).host; }

std::string expiry_text(int64_t expires, int64_t now) {
  if (expires == 0) return "Session";
  const int64_t d = expires - now;
  if (d <= 0) return "expired";
  if (d < 3600) return "in " + std::to_string(std::max<int64_t>(d / 60, 1)) + " min";
  if (d < 2 * 86400) return "in " + std::to_string(d / 3600) + " h";
  if (d < 60 * 86400) return "in " + std::to_string(d / 86400) + " days";
  return utc_text(expires).substr(0, 10);
}

std::string cookie_flags(const Cookie& c) {
  std::string f;
  auto add = [&](const char* s) {
    if (!f.empty()) f += ' ';
    f += s;
  };
  if (c.secure) add("Secure");
  if (c.http_only) add("HttpOnly");
  if (c.same_site != SameSite::Unset) add(same_site_name(c.same_site));
  if (!c.host_only) add("+subdomains");
  return f;
}
}  // namespace watt::app::http_editor
