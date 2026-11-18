#pragma once
// Text helpers of the cookie manager (sidebar_cookies.h): how a cookie's expiry, flags and
// Set-Cookie form read, parsing an expiry typed by the user, and the per-domain grouping with a
// filter. No wx: unit tested in tests/test_sidebar.cpp.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/cookies.h"

namespace watt::app::sidebar {

// "Session", "2026-11-18 14:03 UTC (in 3 h)", "2026-11-18 14:03 UTC (expired)".
std::string cookie_expiry_label(const Cookie& c, int64_t now);
// The expiry as the editor shows it: "" for a session cookie, else "2026-11-18 14:03:05" (UTC).
std::string cookie_expiry_edit_text(const Cookie& c);
// What the user typed into the expiry field: "" / "session" -> 0 (session cookie); "2026-11-18",
// "2026-11-18 14:03", "2026-11-18T14:03:05Z" (UTC); a cookie date ("Wed, 18 Nov 2026 14:03:05
// GMT"); or a relative "+30m" / "+2h" / "+7d". False when it is none of those.
bool parse_cookie_expiry(std::string_view text, int64_t now, int64_t& out);
// "Secure  HttpOnly  SameSite=Lax" (only what is set; the table shows subdomain cookies as ".example.com").
std::string cookie_flags_label(const Cookie& c);
// The cookie as a Set-Cookie header value: "a=1; Domain=example.com; Path=/; Expires=...;
// Secure; HttpOnly; SameSite=Lax" (Domain only when it is not host-only).
std::string set_cookie_line(const Cookie& c);

// Cookies grouped by domain, domains sorted; only cookies whose domain, name or value contains
// `filter` (ASCII case-insensitive; empty: all). Indices point into the list given.
struct CookieDomain {
  std::string domain;
  std::vector<size_t> cookies;
};
std::vector<CookieDomain> group_cookies(const std::vector<Cookie>& list, std::string_view filter);

// The URL a Set-Cookie line pasted for `domain` is recorded against ("https://<domain>/"), so the
// jar's own rules (public suffixes, __Host- / __Secure-, size limits) apply to it. "" when the
// domain is empty or holds characters a host cannot have.
std::string cookie_origin_url(std::string_view domain);

}  // namespace watt::app::sidebar
