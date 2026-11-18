#pragma once
// The response's Cookies tab: every Set-Cookie header of the final response, parsed into the
// columns Postman / Insomnia show (name, value, domain, path, expiry, flags), plus what the
// workspace cookie jar does with it under the same rules (core/cookies.h: stored, deletes a
// stored cookie, or ignored and why). No wx here (tests/test_response_asserts.cpp covers it).
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace watt::app::rv {

struct SetCookieRow {
  enum class Fate { Stored, Deletes, Ignored };
  std::string name;
  std::string value;
  std::string domain;  // as the jar keeps it: the Domain attribute, else the request host
  bool host_only = true;
  std::string path;     // the Path attribute, else the request path's default path
  std::string expires;  // "Session", "in 1 h", "2027-01-31", "now (deletes)"
  int64_t expires_at = 0;  // Unix seconds; 0 = session cookie
  bool http_only = false;
  bool secure = false;
  std::string same_site;  // "", "Lax", "Strict", "None"
  bool partitioned = false;
  size_t size = 0;  // name + value bytes (what browsers count against the 4096 limit)
  Fate fate = Fate::Stored;
  std::string why;  // Ignored: the jar's reason ("Domain does not match the request host")
  std::string raw;  // the header value as received
};

// headers: the response's (name, value) pairs in order; only Set-Cookie (any case) is read.
// url: the URL the response came from (the final one after redirects). now: Unix seconds.
std::vector<SetCookieRow> set_cookie_rows(const std::vector<std::pair<std::string, std::string>>& headers,
                                          std::string_view url, int64_t now);

// "Session", "in 5 min", "in 3 h", "in 12 days", "2027-01-31" (UTC, a year or more away).
std::string cookie_expiry_text(int64_t expires_at, int64_t now);
// "HttpOnly Secure Lax Partitioned" (the set attributes), "" when none.
std::string cookie_flags_text(const SetCookieRow& r);
// What the jar does with it: "stored", "deletes it", "ignored: <why>".
std::string cookie_fate_text(const SetCookieRow& r);

}  // namespace watt::app::rv
