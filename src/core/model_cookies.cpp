// Workspace side of the cookie jar (see core/cookies.h and Workspace::cookie_jar()).
#include <string_view>

#include "core/cookies.h"
#include "core/model.h"

namespace watt {

namespace {

bool ieq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

}  // namespace

namespace detail {

void add_jar_cookies(CookieJar& jar, const std::string& url, std::vector<std::pair<std::string, std::string>>& headers) {
  const std::string jar_value = jar.cookie_header(url);
  if (jar_value.empty()) return;
  std::pair<std::string, std::string>* existing = nullptr;
  for (auto& h : headers)
    if (ieq(h.first, "cookie")) {
      existing = &h;
      break;
    }
  if (!existing) {
    headers.emplace_back("Cookie", jar_value);
    return;
  }
  // Names the request already sends keep its value; the jar's other cookies follow.
  std::vector<std::string_view> own;
  for (std::string_view v = existing->second; !v.empty();) {
    const size_t semi = v.find(';');
    const std::string_view part = trim(v.substr(0, semi));
    own.push_back(trim(part.substr(0, part.find('='))));
    v = semi == std::string_view::npos ? std::string_view{} : v.substr(semi + 1);
  }
  std::string add;
  for (std::string_view v = jar_value; !v.empty();) {
    const size_t semi = v.find("; ");
    const std::string_view part = v.substr(0, semi);
    const size_t eq = part.find('=');
    const std::string_view name = eq == std::string_view::npos ? std::string_view{} : part.substr(0, eq);
    bool dup = false;
    for (auto o : own) dup = dup || o == name;
    if (!dup) add += (add.empty() ? "" : "; ") + std::string(part);
    v = semi == std::string_view::npos ? std::string_view{} : v.substr(semi + 2);
  }
  if (add.empty()) return;
  std::string own_value(trim(existing->second));
  while (!own_value.empty() && own_value.back() == ';') own_value.pop_back();
  existing->second = own_value.empty() ? add : own_value + "; " + add;
}

}  // namespace detail

std::shared_ptr<CookieJar> Workspace::cookie_jar() const { return cookie_jar_for(root_); }

size_t Workspace::record_cookies(const SavedRequest& r, const std::string& sent_url, const HttpResponse& resp) const {
  if (r.no_cookie_jar || resp.headers.empty()) return 0;
  return cookie_jar()->record(resp.effective_url.empty() ? sent_url : resp.effective_url, resp.headers);
}

std::string Workspace::grpc_web_url(const SavedRequest& r) {
  const GrpcTarget& t = r.grpc_target;
  if (!t.web || t.address.empty()) return {};
  std::string base = t.address;
  if (base.find("://") == std::string::npos) base = (t.tls ? "https://" : "http://") + base;
  while (!base.empty() && base.back() == '/') base.pop_back();
  std::string m = r.grpc_method;
  if (!m.empty() && m.front() != '/') m.insert(m.begin(), '/');
  return base + (m.empty() ? "/" : m);
}

size_t Workspace::record_cookies(const SavedRequest& r, const GrpcResponse& resp) const {
  if (r.no_cookie_jar || !r.grpc_target.web) return 0;
  const std::string url = grpc_web_url(r);
  if (url.empty()) return 0;
  return cookie_jar()->record(url, resp.headers);
}

bool Workspace::add_grpc_web_cookies(SavedRequest& x) const {
  if (x.no_cookie_jar || !x.grpc_target.web) return false;
  const std::string url = grpc_web_url(x);
  if (url.empty()) return false;
  const auto before = x.grpc_opts.metadata;
  detail::add_jar_cookies(*cookie_jar(), url, x.grpc_opts.metadata);
  if (x.grpc_opts.metadata == before) return false;
  for (auto& [k, v] : x.grpc_opts.metadata)
    if (ieq(k, "cookie")) k = "cookie";  // gRPC metadata keys are lower case
  return true;
}

}  // namespace watt
