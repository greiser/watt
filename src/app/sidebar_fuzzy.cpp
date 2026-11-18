#include "app/sidebar_fuzzy.h"

#include <algorithm>
#include <climits>

namespace watt::app::sidebar {

namespace {

constexpr int kMatch = 16;
constexpr int kBoundarySlash = 12;
constexpr int kBoundary = 9;
constexpr int kCamel = 7;
constexpr int kConsecutive = 14;
constexpr int kNamePart = 6;
constexpr int kFirstChar = 8;
constexpr int kNeg = INT_MIN / 4;
constexpr size_t kMaxTarget = 512;

inline char lc(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

int bonus_at(std::string_view t, size_t j, size_t name_start) {
  int b = kMatch;
  if (j >= name_start) b += kNamePart;
  if (j == 0) return b + kFirstChar + kBoundarySlash;
  const char p = t[j - 1], c = t[j];
  if (p == '/' || p == '\\') b += kBoundarySlash;
  else if (p == ' ' || p == '-' || p == '_' || p == '.' || p == ':') b += kBoundary;
  else if (p >= 'a' && p <= 'z' && c >= 'A' && c <= 'Z') b += kCamel;
  else if (!(p >= '0' && p <= '9') && c >= '0' && c <= '9') b += kBoundary / 2;
  if (j == name_start) b += kFirstChar;
  return b;
}

// Best placement of word w (lower-case) in t. DP over (word char, target position) with linear gap
// penalty; backtracking gives the positions.
bool match_word(const std::string& w, std::string_view t, size_t name_start, int* score, std::vector<uint16_t>* pos) {
  const size_t m = w.size(), n = std::min(t.size(), kMaxTarget);
  if (m == 0) return true;
  if (m > n) return false;
  // Quick subsequence test first: most candidates fail here.
  {
    size_t i = 0;
    for (size_t j = 0; j < n && i < m; ++j)
      if (lc(t[j]) == w[i]) ++i;
    if (i < m) return false;
  }
  thread_local std::vector<int> M;
  thread_local std::vector<int16_t> from;
  M.assign(m * n, kNeg);
  from.assign(m * n, -1);
  for (size_t i = 0; i < m; ++i) {
    int run = kNeg;     // max over k <= j-2 of M[i-1][k] + k
    int16_t run_k = -1;
    for (size_t j = i; j < n; ++j) {
      if (i > 0 && j >= 2) {
        const int cand = M[(i - 1) * n + (j - 2)];
        if (cand > kNeg && cand + static_cast<int>(j - 2) > run) {
          run = cand + static_cast<int>(j - 2);
          run_k = static_cast<int16_t>(j - 2);
        }
      }
      if (lc(t[j]) != w[i]) continue;
      const int b = bonus_at(t, j, name_start);
      if (i == 0) {
        M[j] = b - static_cast<int>(std::min<size_t>(j, 20)) / 4;  // a little earlier is better
        continue;
      }
      int best = kNeg;
      int16_t bk = -1;
      const int adj = M[(i - 1) * n + (j - 1)];
      if (adj > kNeg) {
        best = adj + b + kConsecutive;
        bk = static_cast<int16_t>(j - 1);
      }
      if (run > kNeg) {
        const int gap = run - static_cast<int>(j) + 1 + b;
        if (gap > best) {
          best = gap;
          bk = run_k;
        }
      }
      M[i * n + j] = best;
      from[i * n + j] = bk;
    }
  }
  int best = kNeg;
  size_t bj = 0;
  for (size_t j = m - 1; j < n; ++j)
    if (M[(m - 1) * n + j] > best) {
      best = M[(m - 1) * n + j];
      bj = j;
    }
  if (best <= kNeg) return false;
  *score += best;
  if (pos) {
    size_t j = bj;
    for (size_t i = m; i-- > 0;) {
      pos->push_back(static_cast<uint16_t>(j));
      if (i > 0) j = static_cast<size_t>(from[i * n + j]);
    }
  }
  return true;
}

}  // namespace

std::string ascii_lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = lc(c);
  return out;
}

FuzzyQuery::FuzzyQuery(std::string_view query) {
  const std::string q = ascii_lower(query);
  for (size_t p = 0; p < q.size();) {
    size_t e = q.find(' ', p);
    if (e == std::string::npos) e = q.size();
    if (e > p) words_.push_back(q.substr(p, e - p));
    p = e + 1;
  }
}

// A number in a query word is an id ("svc150", "v2"): its digits must stand together in the
// target. Scattered digits matched "svc150" in "service-152/delete resource 0 of svc152".
bool numbers_present(const std::string& w, std::string_view t) {
  for (size_t i = 0; i < w.size();) {
    if (w[i] < '0' || w[i] > '9') {
      ++i;
      continue;
    }
    size_t e = i;
    while (e < w.size() && w[e] >= '0' && w[e] <= '9') ++e;
    if (e - i > 1 && t.find(std::string_view(w).substr(i, e - i)) == std::string_view::npos) return false;
    i = e;
  }
  return true;
}

bool FuzzyQuery::match(std::string_view target, size_t name_start, int* score, std::vector<uint16_t>* pos) const {
  int s = 0;
  if (pos) pos->clear();
  for (const auto& w : words_)
    if (!numbers_present(w, target) || !match_word(w, target, name_start, &s, pos)) return false;
  if (pos) {
    std::sort(pos->begin(), pos->end());
    pos->erase(std::unique(pos->begin(), pos->end()), pos->end());
  }
  // Shorter targets win ties (a tighter match).
  *score = s * 4 - static_cast<int>(std::min<size_t>(target.size(), 200)) / 8;
  return true;
}

std::string url_search_text(std::string_view url) {
  const size_t sch = url.find("://");
  const size_t a = sch == std::string_view::npos ? 0 : sch + 3;
  // Without a scheme only a leading host:port / IP is an authority ("127.0.0.1:8080/x").
  size_t e = url.find_first_of("/?#", a);
  if (e == std::string_view::npos) e = url.size();
  std::string_view auth = url.substr(a, e - a);
  if (auth.empty() || auth.find("{{") != std::string_view::npos) return std::string(url);
  if (sch == std::string_view::npos &&
      (auth.find_first_not_of("0123456789.:[]") != std::string_view::npos || auth.find_first_of(".:") == std::string_view::npos))
    return std::string(url);  // "users/42" or "42/items": a path, not a host
  if (const size_t at = auth.rfind('@'); at != std::string_view::npos) auth = auth.substr(at + 1);
  std::string_view host = auth;
  if (!host.empty() && host[0] == '[') {
    const size_t rb = host.find(']');
    host = host.substr(0, rb == std::string_view::npos ? host.size() : rb + 1);
  } else if (const size_t c = host.rfind(':'); c != std::string_view::npos) {
    host = host.substr(0, c);
  }
  const bool numeric = host.find_first_not_of("0123456789.:[]") == std::string_view::npos;
  std::string out(url.substr(0, a));
  if (!numeric) out += host;
  out += url.substr(e);
  return out;
}

bool words_match(std::string_view q, std::string_view target) {
  const std::string t = ascii_lower(target);
  for (size_t p = 0; p < q.size();) {
    size_t e = q.find(' ', p);
    if (e == std::string_view::npos) e = q.size();
    if (e > p && t.find(q.substr(p, e - p)) == std::string::npos) return false;
    p = e + 1;
  }
  return true;
}

bool natural_less(std::string_view a, std::string_view b) {
  auto digit = [](char c) { return c >= '0' && c <= '9'; };
  auto low = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (digit(a[i]) && digit(b[j])) {
      size_t ie = i, je = j;
      while (ie < a.size() && digit(a[ie])) ++ie;
      while (je < b.size() && digit(b[je])) ++je;
      size_t is = i, js = j;  // skip leading zeros
      while (is + 1 < ie && a[is] == '0') ++is;
      while (js + 1 < je && b[js] == '0') ++js;
      if (ie - is != je - js) return ie - is < je - js;
      if (int c = a.substr(is, ie - is).compare(b.substr(js, je - js)); c != 0) return c < 0;
      if (ie - i != je - j) return ie - i < je - j;  // "07" after "7"
      i = ie;
      j = je;
      continue;
    }
    const char x = low(a[i]), y = low(b[j]);
    if (x != y) return static_cast<unsigned char>(x) < static_cast<unsigned char>(y);
    ++i;
    ++j;
  }
  if (a.size() - i != b.size() - j) return a.size() - i < b.size() - j;
  return a < b;  // equal ignoring case: a stable tie-break
}

}  // namespace watt::app::sidebar
