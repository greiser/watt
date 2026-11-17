#include "app/http_editor_util.h"

#include <algorithm>
#include <cstring>

namespace watt::app::http_editor {

namespace {

bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// End of a {{...}} starting at i ("{{" at i), or npos when it is not a variable.
size_t var_end(std::string_view s, size_t i) {
  if (i + 1 >= s.size() || s[i] != '{' || s[i + 1] != '{') return std::string_view::npos;
  const size_t close = s.find("}}", i + 2);
  if (close == std::string_view::npos) return std::string_view::npos;
  const std::string_view inner = s.substr(i + 2, close - i - 2);
  if (inner.find("{{") != std::string_view::npos || (!inner.empty() && inner.front() == '{'))
    return std::string_view::npos;
  if (inner.find('\n') != std::string_view::npos) return std::string_view::npos;
  return close + 2;
}

}  // namespace

std::string_view trim(std::string_view s) {
  while (!s.empty() && is_ws(s.front())) s.remove_prefix(1);
  while (!s.empty() && is_ws(s.back())) s.remove_suffix(1);
  return s;
}

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (lower(a[i]) != lower(b[i])) return false;
  return true;
}

bool istarts_with(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

bool looks_like_curl(std::string_view s) {
  s = trim(s);
  if (s.size() > 2 && s[0] == '$' && s[1] == ' ') s = trim(s.substr(2));
  for (std::string_view p : {std::string_view("curl"), std::string_view("curl.exe")}) {
    if (istarts_with(s, p) && s.size() > p.size() && (is_ws(s[p.size()]) || s[p.size()] == '^' || s[p.size()] == '\\'))
      return true;
  }
  return false;
}

size_t longest_line(std::string_view s) {
  size_t longest = 0;
  const char* p = s.data();
  const char* const end = p + s.size();
  while (p < end) {
    const auto* nl = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(end - p)));
    const char* stop = nl ? nl : end;
    longest = std::max(longest, static_cast<size_t>(stop - p));
    p = stop + 1;
  }
  return longest;
}

bool looks_like_json(std::string_view s) {
  for (char c : s) {
    if (is_ws(c)) continue;
    return c == '{' || c == '[';
  }
  return false;
}

bool looks_like_xml(std::string_view s) {
  for (char c : s) {
    if (is_ws(c)) continue;
    return c == '<';
  }
  return false;
}

UrlParts split_url(std::string_view url) {
  UrlParts p;
  const size_t hash = url.find('#');
  const size_t q = url.find('?');
  if (q != std::string_view::npos && (hash == std::string_view::npos || q < hash)) {
    p.has_query = true;
    p.base = url.substr(0, q);
    const size_t qend = hash == std::string_view::npos ? url.size() : hash;
    p.query = url.substr(q + 1, qend - q - 1);
    if (hash != std::string_view::npos) p.fragment = url.substr(hash);
  } else if (hash != std::string_view::npos) {
    p.base = url.substr(0, hash);
    p.fragment = url.substr(hash);
  } else {
    p.base = url;
  }
  return p;
}

std::vector<KvItem> parse_query(std::string_view query) {
  std::vector<KvItem> out;
  if (query.empty()) return out;
  size_t pos = 0;
  while (pos <= query.size()) {
    size_t amp = query.find('&', pos);
    if (amp == std::string_view::npos) amp = query.size();
    const std::string_view item = query.substr(pos, amp - pos);
    if (!item.empty()) {
      KvItem kv;
      const size_t eq = item.find('=');
      if (eq == std::string_view::npos) {
        kv.key = std::string(item);
        kv.bare = true;
      } else {
        kv.key = std::string(item.substr(0, eq));
        kv.value = std::string(item.substr(eq + 1));
      }
      out.push_back(std::move(kv));
    }
    pos = amp + 1;
  }
  return out;
}

std::string join_query(const std::vector<KvItem>& items) {
  std::string q;
  for (const auto& it : items) {
    if (!it.enabled || (it.key.empty() && it.value.empty())) continue;
    if (!q.empty()) q += '&';
    q += it.key;
    if (!(it.bare && it.value.empty())) {
      q += '=';
      q += it.value;
    }
  }
  return q;
}

std::string with_query(std::string_view url, const std::vector<KvItem>& items) {
  const UrlParts p = split_url(url);
  std::string out(p.base);
  const std::string q = join_query(items);
  if (!q.empty()) {
    out += '?';
    out += q;
  }
  out += p.fragment;
  return out;
}

std::string form_encode(std::string_view s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() + 8);
  for (size_t i = 0; i < s.size();) {
    if (const size_t e = var_end(s, i); e != std::string_view::npos) {
      out.append(s.substr(i, e - i));
      i = e;
      continue;
    }
    const auto c = static_cast<unsigned char>(s[i]);
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
        c == '_' || c == '~' || c == '*') {
      out += static_cast<char>(c);
    } else if (c == ' ') {
      out += '+';
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
    ++i;
  }
  return out;
}

std::string form_decode(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%' && i + 2 < s.size() && hex_value(s[i + 1]) >= 0 && hex_value(s[i + 2]) >= 0) {
      out += static_cast<char>(hex_value(s[i + 1]) * 16 + hex_value(s[i + 2]));
      i += 2;
    } else {
      out += c;
    }
  }
  return out;
}

std::vector<KvItem> parse_form_body(std::string_view body) {
  auto items = parse_query(trim(body));
  for (auto& it : items) {
    it.key = form_decode(it.key);
    it.value = form_decode(it.value);
    it.bare = false;
  }
  return items;
}

std::string build_urlencoded(const std::vector<KvItem>& items) {
  std::string out;
  for (const auto& it : items) {
    if (!it.enabled || (it.key.empty() && it.value.empty())) continue;
    if (!out.empty()) out += '&';
    out += form_encode(it.key);
    out += '=';
    out += form_encode(it.value);
  }
  return out;
}

std::vector<VarSpan> find_vars(std::string_view s) {
  std::vector<VarSpan> out;
  size_t i = 0;
  while (i < s.size()) {
    const size_t open = s.find("{{", i);
    if (open == std::string_view::npos) break;
    const size_t close = s.find("}}", open + 2);
    if (close == std::string_view::npos) break;
    const std::string_view inner = s.substr(open + 2, close - open - 2);
    if (inner.find("{{") != std::string_view::npos || (!inner.empty() && inner.front() == '{')) {
      i = open + 1;
      continue;
    }
    if (inner.find('\n') == std::string_view::npos) {
      VarSpan v;
      v.begin = open;
      v.end = close + 2;
      v.name = std::string(trim(inner));
      if (!v.name.empty()) out.push_back(std::move(v));
    }
    i = close + 2;
  }
  return out;
}

std::optional<JsonIssue> check_json(std::string_view s) {
  const size_t n = s.size();
  size_t i = 0;
  std::optional<JsonIssue> comment_error;
  auto skip = [&] {
    while (i < n) {
      const char c = s[i];
      if (is_ws(c)) {
        ++i;
      } else if (c == '/' && i + 1 < n && s[i + 1] == '/') {
        const size_t e = s.find('\n', i);
        i = e == std::string_view::npos ? n : e + 1;
      } else if (c == '/' && i + 1 < n && s[i + 1] == '*') {
        const size_t e = s.find("*/", i + 2);
        if (e == std::string_view::npos) {
          comment_error = JsonIssue{i, 2, "unterminated /* comment"};
          i = n;
        } else {
          i = e + 2;
        }
      } else {
        break;
      }
    }
  };
  auto issue = [](size_t at, size_t len, std::string msg) {
    return std::optional<JsonIssue>(JsonIssue{at, std::max<size_t>(len, 1), std::move(msg)});
  };
  auto token_len = [&](size_t at) {
    size_t j = at;
    while (j < n && !is_ws(s[j]) && s[j] != ',' && s[j] != ':' && s[j] != '}' && s[j] != ']' && s[j] != '"' &&
           s[j] != '{' && s[j] != '[')
      ++j;
    return std::max<size_t>(j - at, 1);
  };
  // Parses a string at i ('"'); false (with *err) when it does not end on its line.
  auto string_at = [&](std::optional<JsonIssue>* err) {
    size_t j = i + 1;
    while (j < n && s[j] != '"' && s[j] != '\n') j += s[j] == '\\' ? 2 : 1;
    if (j >= n || s[j] != '"') {
      *err = issue(i, std::min(j, n) - i, "unterminated string");
      return false;
    }
    i = j + 1;
    return true;
  };
  enum class Want { Value, ValueOrClose, Key, KeyOrClose, Colon, CommaOrClose, End };
  std::vector<char> stack;
  Want w = Want::Value;
  skip();
  if (comment_error) return comment_error;
  if (i >= n) return std::nullopt;  // an empty body is fine
  for (;;) {
    skip();
    if (comment_error) return comment_error;
    if (i >= n) {
      if (w == Want::End) return std::nullopt;
      if (!stack.empty())
        return issue(n ? n - 1 : 0, 1, std::string("missing '") + (stack.back() == '{' ? '}' : ']') + "' at the end");
      return issue(n ? n - 1 : 0, 1, "unexpected end of the body");
    }
    const char c = s[i];
    auto after_value = [&] { w = stack.empty() ? Want::End : Want::CommaOrClose; };
    switch (w) {
      case Want::End:
        return issue(i, token_len(i), "unexpected text after the JSON value");
      case Want::Colon:
        if (c != ':') return issue(i, token_len(i), "expected ':' after the property name");
        ++i;
        w = Want::Value;
        break;
      case Want::CommaOrClose: {
        const char close = stack.back() == '{' ? '}' : ']';
        if (c == ',') {
          ++i;
          w = stack.back() == '{' ? Want::Key : Want::Value;
          skip();
          if (i < n && (s[i] == '}' || s[i] == ']')) return issue(i, 1, "trailing comma before '" + std::string(1, s[i]) + "'");
        } else if (c == close) {
          ++i;
          stack.pop_back();
          after_value();
        } else {
          return issue(i, token_len(i), std::string("expected ',' or '") + close + "'");
        }
        break;
      }
      case Want::KeyOrClose:
        if (c == '}') {
          ++i;
          stack.pop_back();
          after_value();
          break;
        }
        [[fallthrough]];
      case Want::Key:
        if (c == '"') {
          std::optional<JsonIssue> err;
          if (!string_at(&err)) return err;
        } else if (const size_t e = var_end(s, i); e != std::string_view::npos) {
          i = e;
        } else {
          return issue(i, token_len(i), "expected a property name in double quotes");
        }
        w = Want::Colon;
        break;
      case Want::ValueOrClose:
        if (c == ']') {
          ++i;
          stack.pop_back();
          after_value();
          break;
        }
        [[fallthrough]];
      case Want::Value:
        if (const size_t e = c == '{' ? var_end(s, i) : std::string_view::npos; e != std::string_view::npos) {
          i = e;
          after_value();
        } else if (c == '{' || c == '[') {
          stack.push_back(c);
          ++i;
          w = c == '{' ? Want::KeyOrClose : Want::ValueOrClose;
        } else if (c == '"') {
          std::optional<JsonIssue> err;
          if (!string_at(&err)) return err;
          after_value();
        } else if (c == '-' || (c >= '0' && c <= '9')) {
          const size_t start = i;
          if (s[i] == '-') ++i;
          auto digits = [&] {
            const size_t d = i;
            while (i < n && s[i] >= '0' && s[i] <= '9') ++i;
            return i > d;
          };
          bool ok = digits();
          if (ok && i < n && s[i] == '.') {
            ++i;
            ok = digits();
          }
          if (ok && i < n && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < n && (s[i] == '+' || s[i] == '-')) ++i;
            ok = digits();
          }
          if (ok && i < n && !is_ws(s[i]) && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != '/') ok = false;
          if (!ok) return issue(start, token_len(start), "invalid number");
          after_value();
        } else {
          const size_t len = token_len(i);
          const std::string_view tok = s.substr(i, len);
          if (tok != "true" && tok != "false" && tok != "null") {
            if (c == '}' || c == ']' || c == ',' || c == ':') return issue(i, 1, std::string("expected a value before '") + c + "'");
            return issue(i, len, "unexpected '" + std::string(tok.substr(0, 40)) + "' (strings need double quotes)");
          }
          i += len;
          after_value();
        }
        break;
    }
  }
}

std::string reindent_json(std::string_view in) {
  std::string out;
  out.reserve(in.size() + in.size() / 3 + 16);
  size_t depth = 0;
  bool gap = false;  // whitespace seen since the last token
  const size_t n = in.size();
  auto newline = [&] {
    while (!out.empty() && out.back() == ' ') out.pop_back();
    out += '\n';
    out.append(depth * 2, ' ');
  };
  auto before_token = [&] {
    if (gap && !out.empty()) {
      const char b = out.back();
      if (b != ' ' && b != '\n' && b != '{' && b != '[' && b != ':' && b != ',') out += ' ';
    }
    gap = false;
  };
  auto next_sig = [&](size_t j) {
    while (j < n && is_ws(in[j])) ++j;
    return j;
  };
  size_t i = 0;
  while (i < n) {
    const char c = in[i];
    if (is_ws(c)) {
      gap = true;
      ++i;
      continue;
    }
    if (c == '{' || c == '[') {
      if (const size_t e = var_end(in, i); c == '{' && e != std::string_view::npos) {
        before_token();
        out.append(in.substr(i, e - i));
        i = e;
        continue;
      }
      before_token();
      out += c;
      const size_t j = next_sig(i + 1);
      if (j < n && in[j] == (c == '{' ? '}' : ']')) {
        out += in[j];
        i = j + 1;
      } else {
        ++depth;
        newline();
        i = i + 1;
      }
      continue;
    }
    if (c == '}' || c == ']') {
      gap = false;
      if (depth > 0) --depth;
      while (!out.empty() && out.back() == ' ') out.pop_back();
      if (!out.empty() && out.back() == '\n') out.pop_back();
      newline();
      out += c;
      ++i;
      continue;
    }
    if (c == ',') {
      gap = false;
      out += ',';
      newline();
      ++i;
      continue;
    }
    if (c == ':') {
      gap = false;
      out += ": ";
      ++i;
      continue;
    }
    if (c == '"') {
      before_token();
      size_t j = i + 1;
      while (j < n) {
        if (in[j] == '\\') {
          j += 2;
        } else if (in[j] == '"') {
          ++j;
          break;
        } else if (in[j] == '\n') {
          break;
        } else {
          ++j;
        }
      }
      j = std::min(j, n);
      out.append(in.substr(i, j - i));
      i = j;
      continue;
    }
    if (c == '/' && i + 1 < n && (in[i + 1] == '/' || in[i + 1] == '*')) {
      before_token();
      if (in[i + 1] == '/') {
        size_t j = in.find('\n', i);
        if (j == std::string_view::npos) j = n;
        std::string_view line = in.substr(i, j - i);
        while (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        out.append(line);
        newline();
        i = j;
      } else {
        size_t j = in.find("*/", i + 2);
        j = j == std::string_view::npos ? n : j + 2;
        out.append(in.substr(i, j - i));
        i = j;
      }
      continue;
    }
    // Bare token (number, true/false/null, or anything else): copied as written.
    before_token();
    size_t j = i;
    while (j < n) {
      const char d = in[j];
      if (is_ws(d) || d == ',' || d == ':' || d == '"' || d == '[' || d == ']' || d == '}') break;
      if (d == '{') {
        const size_t e = var_end(in, j);
        if (e == std::string_view::npos) break;
        j = e;
        continue;
      }
      ++j;
    }
    if (j == i) j = i + 1;
    out.append(in.substr(i, j - i));
    i = j;
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
  return out;
}

namespace {

// Strict UTF-8 (no overlong forms, no surrogates): what wxString::FromUTF8 accepts.
bool valid_utf8(std::string_view s) {
  size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      ++i;
      continue;
    }
    size_t n;
    unsigned char lo = 0x80, hi = 0xBF;  // allowed range of the second byte
    if (c >= 0xC2 && c <= 0xDF) n = 1;
    else if (c == 0xE0) { n = 2; lo = 0xA0; }
    else if (c == 0xED) { n = 2; hi = 0x9F; }
    else if (c >= 0xE1 && c <= 0xEF) n = 2;
    else if (c == 0xF0) { n = 3; lo = 0x90; }
    else if (c >= 0xF1 && c <= 0xF3) n = 3;
    else if (c == 0xF4) { n = 3; hi = 0x8F; }
    else return false;
    if (i + n >= s.size()) return false;
    for (size_t k = 1; k <= n; ++k) {
      const auto b = static_cast<unsigned char>(s[i + k]);
      if (k == 1 ? (b < lo || b > hi) : (b & 0xC0) != 0x80) return false;
    }
    i += n + 1;
  }
  return true;
}

bool has_control(std::string_view s) {
  for (char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    if (c < 0x20 || c == 0x7F) return true;
  }
  return false;
}

// Decoded for display, or as written when decoding would not give printable UTF-8.
std::string display_decode(std::string_view s) {
  if (s.find('%') == std::string_view::npos) return std::string(s);
  std::string d = query_decode(s);
  if (!valid_utf8(d) || has_control(d)) return std::string(s);
  return d;
}

constexpr char kHex[] = "0123456789ABCDEF";

void put_pct(std::string& out, unsigned char c) {
  out += '%';
  out += kHex[c >> 4];
  out += kHex[c & 15];
}

}  // namespace

std::string query_decode(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '%' && i + 2 < s.size() && hex_value(s[i + 1]) >= 0 && hex_value(s[i + 2]) >= 0) {
      out += static_cast<char>(hex_value(s[i + 1]) * 16 + hex_value(s[i + 2]));
      i += 2;
    } else {
      out += c;
    }
  }
  return out;
}

std::string query_encode(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (size_t i = 0; i < s.size();) {
    if (const size_t e = var_end(s, i); e != std::string_view::npos) {
      out.append(s.substr(i, e - i));
      i = e;
      continue;
    }
    const auto c = static_cast<unsigned char>(s[i]);
    const bool literal = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                         c == '.' || c == '_' || c == '~' || c == '!' || c == '$' || c == '\'' || c == '(' ||
                         c == ')' || c == '*' || c == ',' || c == ';' || c == ':' || c == '@' || c == '/' ||
                         c == '?' || c == '[' || c == ']';
    if (literal) out += static_cast<char>(c);
    else put_pct(out, c);
    ++i;
  }
  return out;
}

std::vector<KvItem> parse_query_decoded(std::string_view query) {
  auto items = parse_query(query);
  for (auto& it : items) {
    it.key = display_decode(it.key);
    it.value = display_decode(it.value);
  }
  return items;
}

std::string with_decoded_query(std::string_view url, const std::vector<KvItem>& items) {
  const UrlParts p = split_url(url);
  std::vector<KvItem> raw = parse_query(p.query);
  std::vector<bool> used(raw.size(), false);
  std::vector<KvItem> out;
  out.reserve(items.size());
  for (const auto& it : items) {
    KvItem w;
    w.enabled = it.enabled;
    bool found = false;
    for (size_t i = 0; i < raw.size(); ++i) {
      if (used[i]) continue;
      if (display_decode(raw[i].key) == it.key && display_decode(raw[i].value) == it.value) {
        used[i] = true;
        w.key = raw[i].key;
        w.value = raw[i].value;
        w.bare = raw[i].bare && raw[i].value.empty();
        found = true;
        break;
      }
    }
    if (!found) {
      w.key = query_encode(it.key);
      w.value = query_encode(it.value);
      w.bare = it.bare && it.value.empty();
    }
    out.push_back(std::move(w));
  }
  return with_query(url, out);
}

namespace {
// Where the path starts: after "scheme://authority", or at the first '/', '?' or '#' without a
// scheme (npos when there is none). {{base}}/x starts at the '/' after the var.
size_t path_start(std::string_view url) {
  const size_t sch = url.find("://");
  const size_t first_delim = url.find_first_of("/?#");
  if (sch != std::string_view::npos && (first_delim == std::string_view::npos || sch < first_delim))
    return url.find_first_of("/?#", sch + 3);
  return first_delim;
}
bool path_name_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}
}  // namespace

std::vector<PathVar> find_path_vars(std::string_view url) {
  std::vector<PathVar> out;
  size_t i = path_start(url);
  if (i == std::string_view::npos) return out;
  for (; i < url.size();) {
    if (const size_t e = var_end(url, i); e != std::string_view::npos) {
      i = e;
      continue;
    }
    const char c = url[i];
    if (c == '?' || c == '#') break;
    if (c == '/' && i + 2 < url.size() && url[i + 1] == ':' && path_name_char(url[i + 2])) {
      size_t j = i + 2;
      while (j < url.size() && path_name_char(url[j])) ++j;
      out.push_back({i + 1, j, std::string(url.substr(i + 2, j - i - 2))});
      i = j;
      continue;
    }
    ++i;
  }
  return out;
}

std::vector<std::string> path_var_names(std::string_view url) {
  std::vector<std::string> names;
  for (auto& v : find_path_vars(url))
    if (std::find(names.begin(), names.end(), v.name) == names.end()) names.push_back(std::move(v.name));
  return names;
}

std::string path_encode(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (size_t i = 0; i < s.size();) {
    if (const size_t e = var_end(s, i); e != std::string_view::npos) {
      out.append(s.substr(i, e - i));
      i = e;
      continue;
    }
    const auto c = static_cast<unsigned char>(s[i]);
    const bool literal = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                         c == '.' || c == '_' || c == '~' || c == '!' || c == '$' || c == '&' || c == 0x27 ||
                         c == '(' || c == ')' || c == '*' || c == '+' || c == ',' || c == ';' || c == '=' ||
                         c == ':' || c == '@';
    if (literal) out += static_cast<char>(c);
    else put_pct(out, c);
    ++i;
  }
  return out;
}

std::string with_path_values(std::string_view url, const std::function<std::string(const std::string&)>& value) {
  const auto vars = find_path_vars(url);
  if (vars.empty()) return std::string(url);
  std::string out;
  out.reserve(url.size() + 16);
  size_t pos = 0;
  for (const auto& v : vars) {
    const std::string val = value ? value(v.name) : std::string();
    if (val.empty()) continue;
    out.append(url.substr(pos, v.begin - pos));
    out += path_encode(val);
    pos = v.end;
  }
  out.append(url.substr(pos));
  return out;
}

std::string normalize_url(std::string_view url) {
  // Start of the path: after "scheme://authority", or the first '/', '?' or '#' without a scheme.
  size_t from = 0;
  const size_t sch = url.find("://");
  const size_t first_delim = url.find_first_of("/?#");
  if (sch != std::string_view::npos && (first_delim == std::string_view::npos || sch < first_delim)) {
    from = url.find_first_of("/?#", sch + 3);
  } else {
    from = first_delim;
  }
  if (from == std::string_view::npos) return std::string(url);
  std::string out(url.substr(0, from));
  out.reserve(url.size() + 16);
  for (size_t i = from; i < url.size();) {
    if (const size_t e = var_end(url, i); e != std::string_view::npos) {
      out.append(url.substr(i, e - i));
      i = e;
      continue;
    }
    const auto c = static_cast<unsigned char>(url[i]);
    bool enc = c <= 0x20 || c >= 0x7F || c == '"' || c == '<' || c == '>' || c == 0x5C || c == '^' || c == '`' ||
               c == '{' || c == '|' || c == '}';
    if (c == '%' && !(i + 2 < url.size() && hex_value(url[i + 1]) >= 0 && hex_value(url[i + 2]) >= 0)) enc = true;
    if (enc) put_pct(out, c);
    else out += static_cast<char>(c);
    ++i;
  }
  return out;
}

std::string minify_json(std::string_view in) {
  auto word = [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.' ||
           c == '-' || c == '+' || c == '$' || static_cast<unsigned char>(c) >= 0x80;
  };
  std::string out;
  out.reserve(in.size());
  const size_t n = in.size();
  size_t i = 0;
  while (i < n) {
    const char c = in[i];
    if (c == '"') {
      size_t j = i + 1;
      while (j < n && in[j] != '"') j += in[j] == 0x5C ? 2 : 1;
      j = std::min(n, j + 1);
      out.append(in.substr(i, j - i));
      i = j;
    } else if (c == '{' && var_end(in, i) != std::string_view::npos) {
      const size_t e = var_end(in, i);
      out.append(in.substr(i, e - i));
      i = e;
    } else if (c == '/' && i + 1 < n && in[i + 1] == '/') {
      size_t e = in.find('\n', i);
      e = e == std::string_view::npos ? n : e + 1;
      out.append(in.substr(i, e - i));
      i = e;
    } else if (c == '/' && i + 1 < n && in[i + 1] == '*') {
      size_t e = in.find("*/", i + 2);
      e = e == std::string_view::npos ? n : e + 2;
      out.append(in.substr(i, e - i));
      i = e;
    } else if (is_ws(c)) {
      size_t j = i;
      while (j < n && is_ws(in[j])) ++j;
      // Two bare words stay apart ("true false" must not become "truefalse").
      if (!out.empty() && j < n && word(out.back()) && word(in[j])) out += ' ';
      i = j;
    } else {
      out += c;
      ++i;
    }
  }
  return out;
}

namespace {

template <class Encode>
std::string substitute(std::string_view tmpl, const VarExpander& expand_var, bool track_query, Encode encode) {
  std::string out;
  out.reserve(tmpl.size() + 32);
  bool in_query = false, in_fragment = false;
  for (size_t i = 0; i < tmpl.size();) {
    if (const size_t e = var_end(tmpl, i); e != std::string_view::npos) {
      const std::string span(tmpl.substr(i, e - i));
      const std::string v = expand_var(span);
      if (v == span) out += span;  // not defined: kept as written
      else encode(out, v, in_query);
      i = e;
      continue;
    }
    const char c = tmpl[i];
    if (track_query) {
      if (c == '?' && !in_query && !in_fragment) {
        in_query = true;
      } else if (c == '#') {
        in_query = false;
        in_fragment = true;
      }
    }
    out += c;
    ++i;
  }
  return out;
}

}  // namespace

std::string expand_url(std::string_view tmpl, const VarExpander& expand_var) {
  if (tmpl.find("{{") == std::string_view::npos) return normalize_url(tmpl);
  return normalize_url(substitute(tmpl, expand_var, true, [](std::string& out, const std::string& v, bool in_query) {
    out += in_query ? query_encode(v) : v;
  }));
}

std::string expand_form(std::string_view tmpl, const VarExpander& expand_var) {
  if (tmpl.find("{{") == std::string_view::npos) return std::string(tmpl);
  return substitute(tmpl, expand_var, false,
                    [](std::string& out, const std::string& v, bool) { out += form_encode(v); });
}

std::string to_bulk(const std::vector<KvItem>& items) {
  std::string out;
  for (const auto& it : items) {
    if (it.key.empty() && it.value.empty()) continue;
    if (!it.enabled) out += "//";
    out += it.key;
    out += ':';
    if (!it.value.empty()) {
      out += ' ';
      out += it.value;
    }
    out += '\n';
  }
  return out;
}

std::vector<KvItem> parse_bulk(std::string_view text) {
  std::vector<KvItem> out;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string_view::npos) nl = text.size();
    std::string_view line = text.substr(pos, nl - pos);
    pos = nl + 1;
    while (!line.empty() && (line.back() == '\r')) line.remove_suffix(1);
    line = trim(line);
    if (line.empty()) continue;
    KvItem kv;
    if (line.size() >= 2 && line[0] == '/' && line[1] == '/') {
      kv.enabled = false;
      line = trim(line.substr(2));
      if (line.empty()) continue;
    }
    if (line.front() == ':') continue;  // HTTP/2 pseudo-header copied from devtools
    size_t sep = line.find(':');
    if (sep == std::string_view::npos) sep = line.find('=');
    if (sep == std::string_view::npos) {
      kv.key = std::string(line);
      kv.bare = true;
    } else {
      kv.key = std::string(trim(line.substr(0, sep)));
      kv.value = std::string(trim(line.substr(sep + 1)));
    }
    if (kv.key.empty() && kv.value.empty()) continue;
    out.push_back(std::move(kv));
  }
  return out;
}

std::string clean_header_name(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    if (c <= 0x20 || c == 0x7F) continue;
    out += ch;
  }
  return out;
}

std::string clean_header_value(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    if ((c < 0x20 && c != '\t') || c == 0x7F) continue;
    out += ch;
  }
  return out;
}

std::string var_closer(std::string_view before, std::string_view after) {
  const size_t n = before.size();
  if (n < 2 || before[n - 1] != '{' || before[n - 2] != '{') return {};
  if (n >= 3 && before[n - 3] == '{') return {};  // "{{{": a literal brace
  if (after.size() >= 2 && after[0] == '}' && after[1] == '}') return {};
  return !after.empty() && after[0] == '}' ? "}" : "}}";  // the body's auto-close may have put one
}

void TypingModel::type(char c) {
  auto_closed.resize(text.size(), false);
  if (caret < text.size() && steps_over(c, text[caret], auto_closed[caret])) {
    auto_closed[caret] = false;
    ++caret;
    return;
  }
  text.insert(caret, 1, c);
  auto_closed.insert(auto_closed.begin() + static_cast<std::ptrdiff_t>(caret), false);
  ++caret;
  if (c != '{') return;
  const std::string close = var_closer(std::string_view(text).substr(0, caret), std::string_view(text).substr(caret));
  text.insert(caret, close);
  auto_closed.insert(auto_closed.begin() + static_cast<std::ptrdiff_t>(caret), close.size(), true);
}

}  // namespace watt::app::http_editor
