#include "app/http_editor_util.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <initializer_list>

#include <emmintrin.h>
#include <intrin.h>

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

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

std::string brace_problem(std::string_view s) {
  if (s.find("{{") == std::string_view::npos && s.find("}}") == std::string_view::npos) return {};
  std::string m(s);
  for (const auto& v : find_vars(s))
    for (size_t i = v.begin; i < v.end; ++i) m[i] = ' ';
  auto word = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$' ||
           c == '.' || c == '-';
  };
  for (size_t at = m.find("{{"); at != std::string::npos; at = m.find("{{", at + 2)) {
    size_t j = at + 2;
    while (j < m.size() && m[j] == '{') ++j;  // "{{{" is a literal brace
    if (j > at + 2) {
      at = j - 2;
      continue;
    }
    while (j < m.size() && m[j] == ' ') ++j;
    if (j < m.size() && word(m[j])) {
      size_t e = j;
      while (e < m.size() && word(m[e])) ++e;
      return "\"{{" + m.substr(j, e - j) + "\" is not closed with }}";
    }
  }
  for (size_t at = m.find("}}"); at != std::string::npos; at = m.find("}}", at + 2)) {
    size_t b = at;
    while (b > 0 && word(m[b - 1])) --b;
    if (b == at) continue;
    if (b == 0 || m[b - 1] == '{' || m[b - 1] == ' ' || m[b - 1] == '/' || m[b - 1] == '=' || m[b - 1] == '&')
      return "\"" + m.substr(b - (b > 0 && m[b - 1] == '{' ? 1 : 0), at + 2 - b + (b > 0 && m[b - 1] == '{' ? 1 : 0)) +
             "\" has no opening {{";
  }
  return {};
}

std::optional<JsonIssue> check_graphql(std::string_view s) {
  std::vector<std::pair<char, size_t>> open;
  auto closer_of = [](char c) { return c == '{' ? '}' : c == '(' ? ')' : ']'; };
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '#') {
      while (i < s.size() && s[i] != '\n') ++i;
    } else if (c == '"') {
      const size_t at = i;
      if (s.substr(i, 3) == "\"\"\"") {
        const size_t end = s.find("\"\"\"", i + 3);
        if (end == std::string_view::npos) return JsonIssue{at, 3, "unterminated block string"};
        i = end + 2;
      } else {
        for (++i; i < s.size() && s[i] != '"' && s[i] != '\n'; ++i)
          if (s[i] == '\\') ++i;
        if (i >= s.size() || s[i] != '"') return JsonIssue{at, 1, "unterminated string"};
      }
    } else if (c == '{' || c == '(' || c == '[') {
      open.emplace_back(c, i);
    } else if (c == '}' || c == ')' || c == ']') {
      if (open.empty()) return JsonIssue{i, 1, std::string("'") + c + "' without an opening one"};
      if (closer_of(open.back().first) != c)
        return JsonIssue{i, 1, std::string("expected '") + closer_of(open.back().first) + "' before '" + c + "'"};
      open.pop_back();
    }
  }
  if (!open.empty())
    return JsonIssue{open.back().second, 1, std::string("'") + open.back().first + "' is never closed"};
  return std::nullopt;
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

std::string with_path_values(std::string_view url, const std::function<std::string(const std::string&)>& value,
                             bool encode) {
  const auto vars = find_path_vars(url);
  if (vars.empty()) return std::string(url);
  std::string out;
  out.reserve(url.size() + 16);
  size_t pos = 0;
  for (const auto& v : vars) {
    const std::string val = value ? value(v.name) : std::string();
    if (val.empty()) continue;
    out.append(url.substr(pos, v.begin - pos));
    out += encode ? path_encode(val) : val;
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

std::string minimal_url(std::string_view url) {
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
  out.reserve(url.size() + 8);
  for (size_t i = from; i < url.size();) {
    if (const size_t e = var_end(url, i); e != std::string_view::npos) {
      out.append(url.substr(i, e - i));
      i = e;
      continue;
    }
    const auto c = static_cast<unsigned char>(url[i]);
    if (c <= 0x20 || c == 0x7F) put_pct(out, c);
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
  // Where a span sits (track_query): 'q' in the query, 'p' in the path or fragment, 0 before
  // the path (scheme / host, e.g. {{base}}).
  const size_t from = track_query ? path_start(tmpl) : std::string_view::npos;
  for (size_t i = 0; i < tmpl.size();) {
    if (const size_t e = var_end(tmpl, i); e != std::string_view::npos) {
      const std::string span(tmpl.substr(i, e - i));
      const std::string v = expand_var(span);
      const char zone = in_query ? 'q' : (from != std::string_view::npos && i >= from) ? 'p' : 0;
      if (v == span) out += span;  // not defined: kept as written
      else encode(out, v, zone);
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

namespace {

// keep_script_vars() returns a kept span as kKept + "{{name}}"; the expanders below write it
// back as "{{name}}", or with the core's encode-on-substitution mark where the value needs one.
constexpr char kKept = '\x01';
bool is_kept(const std::string& v) { return v.size() > 4 && v[0] == kKept; }
void put_kept(std::string& out, const std::string& v, char mark) {
  if (!mark) {
    out.append(v, 1, std::string::npos);
    return;
  }
  out += "{{";
  out += trim(std::string_view(v).substr(3, v.size() - 5));
  out += '|';
  out += mark;
  out += "}}";
}

}  // namespace

std::string expand_url(std::string_view tmpl, const VarExpander& expand_var) {
  if (tmpl.find("{{") == std::string_view::npos) return normalize_url(tmpl);
  return normalize_url(substitute(tmpl, expand_var, true, [](std::string& out, const std::string& v, char zone) {
    if (is_kept(v)) put_kept(out, v, zone);  // 'p' / 'q': the core encodes the value as below
    else out += zone == 'q' ? query_encode(v) : v;
  }));
}

std::string expand_url(std::string_view tmpl, const VarExpander& expand_var, bool encode) {
  if (encode) return expand_url(tmpl, expand_var);
  if (tmpl.find("{{") == std::string_view::npos) return minimal_url(tmpl);
  return minimal_url(substitute(tmpl, expand_var, false, [](std::string& out, const std::string& v, char) {
    if (is_kept(v)) put_kept(out, v, 0);
    else out += v;
  }));
}

std::string expand_text(std::string_view tmpl, const VarExpander& expand_var) {
  if (tmpl.find("{{") == std::string_view::npos) return std::string(tmpl);
  return substitute(tmpl, expand_var, false, [](std::string& out, const std::string& v, char) {
    if (is_kept(v)) put_kept(out, v, 0);
    else out += v;
  });
}

VarExpander keep_script_vars(VarExpander inner, std::vector<std::string> set_names, bool any_name) {
  return [inner = std::move(inner), names = std::move(set_names), any_name](const std::string& span) -> std::string {
    if (span.size() > 4 && span.compare(0, 2, "{{") == 0) {
      const std::string_view name = trim(std::string_view(span).substr(2, span.size() - 4));
      // An OS environment variable is never kept: the consent prompt and check read the name
      // as written, and no script sets one.
      const bool os_env = name.substr(0, 12) == "process.env." || name.substr(0, 5) == "$env.";
      if (!os_env && (any_name || std::find(names.begin(), names.end(), name) != names.end())) return kKept + span;
    }
    return inner(span);
  };
}

std::string expand_form(std::string_view tmpl, const VarExpander& expand_var) {
  if (tmpl.find("{{") == std::string_view::npos) return std::string(tmpl);
  return substitute(tmpl, expand_var, false, [](std::string& out, const std::string& v, char) {
    if (is_kept(v)) put_kept(out, v, 'f');
    else out += form_encode(v);
  });
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

std::string header_name_problem(std::string_view name) {
  // {{variables}} expand at send time: only the literal text around them is checked.
  std::string lit;
  size_t pos = 0;
  for (const auto& v : find_vars(name)) {
    lit.append(name.substr(pos, v.begin - pos));
    lit += 'x';
    pos = v.end;
  }
  lit.append(name.substr(pos));
  const std::string_view t = trim(lit);
  if (t.empty()) return {};
  auto tchar = [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) != std::string_view::npos;
  };
  for (char ch : t) {
    const auto c = static_cast<unsigned char>(ch);
    if (tchar(c)) continue;
    if (c == ' ' || c == '\t') {
      // "Authorization Bearer x": a known name followed by its value. Anything else ("X Bad/Name")
      // is a name of several words: suggest them joined with '-'.
      const std::string_view head = t.substr(0, t.find_first_of(" \t"));
      static constexpr const char* kKnown[] = {"Authorization", "Content-Type", "Accept", "Cookie", "User-Agent",
                                               "Cache-Control", "Accept-Encoding", "Accept-Language", "Origin",
                                               "Referer", "If-None-Match", "If-Match", "X-Api-Key", "Api-Key",
                                               "X-Request-Id", "Content-Length", "Connection", "Host", "Range"};
      bool known = false;
      for (const char* k : kKnown) known = known || iequals(head, k);
      const std::string msg = "a header name cannot contain spaces (it would be sent as \"" + clean_header_name(t) + "\")";
      if (known) return msg + ": put the value in the Value column";
      if (name.find("{{") != std::string_view::npos) return msg + ": join the words with '-'";
      std::string joined;
      for (const char x : t) {
        if (tchar(static_cast<unsigned char>(x))) joined += x;
        else if (!joined.empty() && joined.back() != '-') joined += '-';
      }
      while (!joined.empty() && joined.back() == '-') joined.pop_back();
      return msg + ": join the words with '-', like \"" + joined + "\"";
    }
    if (c == ':') return "a header name cannot contain ':': put the value in the Value column";
    if (c < 0x20 || c == 0x7F) return "a header name cannot contain control characters";
    if (c >= 0x80) return "a header name must be ASCII (RFC 9110 token)";
    return std::string("'") + ch + "' is not allowed in a header name (RFC 9110 token: letters, digits and !#$%&'*+-.^_`|~)";
  }
  return {};
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

std::vector<std::string> auto_header_losers(const std::vector<std::pair<std::string, std::string>>& rows,
                                            const std::vector<std::string>& own, std::string_view auth_name,
                                            std::string_view from_auth) {
  std::vector<std::string> out(rows.size());
  const std::string auth = clean_header_name(auth_name);
  for (size_t i = 0; i < rows.size(); ++i) {
    const std::string name = clean_header_name(rows[i].first);
    const std::string& v = rows[i].second;
    if (name.empty()) continue;
    if (std::any_of(own.begin(), own.end(), [&](const std::string& o) { return iequals(clean_header_name(o), name); })) {
      out[i] = "(replaced by your header)";
      continue;
    }
    const bool mine = !from_auth.empty() && v.size() >= from_auth.size() &&
                      std::string_view(v).substr(v.size() - from_auth.size()) == from_auth;
    const size_t at = v.rfind("   (from ");
    const bool inherited = !mine && at != std::string::npos && !v.empty() && v.back() == ')';
    if (inherited && !auth.empty() && iequals(auth, name)) out[i] = "(replaced by the Auth tab)";
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

const std::vector<std::string>& header_name_choices() {
  static const std::vector<std::string> names = {
      "Accept", "Accept-Encoding", "Accept-Language", "Accept-Charset", "Authorization", "Cache-Control",
      "Connection", "Content-Type", "Content-Length", "Content-Encoding", "Content-Disposition",
      "Content-Language", "Cookie", "DNT", "Date", "Expect", "Forwarded", "From", "Host", "If-Match",
      "If-Modified-Since", "If-None-Match", "If-Range", "If-Unmodified-Since", "Idempotency-Key",
      "Keep-Alive", "Max-Forwards", "Origin", "Pragma", "Prefer", "Proxy-Authorization", "Range", "Referer",
      "TE", "Traceparent", "Tracestate", "Upgrade", "User-Agent", "Via", "X-API-Key", "X-Correlation-ID",
      "X-CSRF-Token", "X-Forwarded-For", "X-Forwarded-Host", "X-Forwarded-Proto", "X-HTTP-Method-Override",
      "X-Request-ID", "X-Requested-With"};
  return names;
}

const std::vector<std::string>& header_value_choices(std::string_view header_name) {
  static const std::vector<std::string> none;
  static const std::vector<std::string> media = {
      "application/json", "application/json; charset=utf-8", "application/xml", "application/x-www-form-urlencoded",
      "application/octet-stream", "application/grpc-web+proto", "application/problem+json", "multipart/form-data",
      "text/plain", "text/html", "text/csv", "text/xml"};
  static const std::vector<std::string> accept = [] {
    std::vector<std::string> v = {"*/*"};
    for (const auto& m : media)
      if (m != "multipart/form-data" && m != "application/x-www-form-urlencoded") v.push_back(m);
    return v;
  }();
  static const std::vector<std::string> codings = {"gzip, deflate, br", "gzip, deflate, br, zstd", "gzip", "deflate",
                                                   "br", "zstd", "identity"};
  static const std::vector<std::string> content_codings = {"gzip", "deflate", "br", "zstd", "identity"};
  static const std::vector<std::string> languages = {"en-US,en;q=0.9", "en", "en-GB", "de-DE,de;q=0.9", "fr-FR,fr;q=0.9", "*"};
  static const std::vector<std::string> charsets = {"utf-8", "iso-8859-1", "*"};
  static const std::vector<std::string> schemes = {"Bearer ", "Basic ", "Digest ", "Negotiate ", "AWS4-HMAC-SHA256 "};
  static const std::vector<std::string> cache = {"no-cache", "no-store", "max-age=0", "no-transform", "only-if-cached"};
  static const std::vector<std::string> pragma = {"no-cache"};
  static const std::vector<std::string> connection = {"keep-alive", "close", "Upgrade"};
  static const std::vector<std::string> expect = {"100-continue"};
  static const std::vector<std::string> prefer = {"return=representation", "return=minimal", "respond-async", "wait="};
  static const std::vector<std::string> requested_with = {"XMLHttpRequest"};
  static const std::vector<std::string> te = {"trailers"};
  static const std::vector<std::string> upgrade = {"websocket", "h2c"};
  static const std::vector<std::string> dnt = {"1", "0"};
  static const std::vector<std::string> override_methods = {"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"};
  static const std::vector<std::string> disposition = {"inline", "attachment", "attachment; filename=\"\"",
                                                       "form-data; name=\"\""};
  static const std::pair<const char*, const std::vector<std::string>*> table[] = {
      {"Content-Type", &media},          {"Accept", &accept},
      {"Accept-Encoding", &codings},     {"Content-Encoding", &content_codings},
      {"Accept-Language", &languages},   {"Content-Language", &languages},
      {"Accept-Charset", &charsets},     {"Authorization", &schemes},
      {"Proxy-Authorization", &schemes}, {"Cache-Control", &cache},
      {"Pragma", &pragma},               {"Connection", &connection},
      {"Expect", &expect},               {"Prefer", &prefer},
      {"X-Requested-With", &requested_with}, {"TE", &te},
      {"Upgrade", &upgrade},             {"DNT", &dnt},
      {"X-HTTP-Method-Override", &override_methods}, {"Content-Disposition", &disposition}};
  const std::string_view n = trim(header_name);
  for (const auto& [name, list] : table)
    if (iequals(n, name)) return *list;
  return none;
}

std::vector<std::string> completions_for(const std::vector<std::string>& choices, std::string_view typed) {
  std::vector<std::string> out;
  if (typed.empty()) return out;
  for (const auto& c : choices)
    if (c.size() > typed.size() && istarts_with(c, typed)) out.push_back(c);
  return out;
}

void HeaderGridModel::type(std::string_view keys) {
  for (char k : keys) key(k);
}

void HeaderGridModel::open(int r, int c) {
  row = r;
  col = c;
  const Row& rw = rows[static_cast<size_t>(r)];
  text = c == 0 ? rw.name : c == 1 ? rw.value : rw.desc;
  list.clear();
  sel = -1;
}

void HeaderGridModel::refresh_list() {
  const std::vector<std::string>* choices = nullptr;
  if (col == 0) choices = &header_name_choices();
  else if (col == 1) choices = &header_value_choices(rows[static_cast<size_t>(row)].name);
  list = choices ? completions_for(*choices, text) : std::vector<std::string>{};
  sel = -1;  // never chosen by itself
}

void HeaderGridModel::commit() {
  Row& rw = rows[static_cast<size_t>(row)];
  (col == 0 ? rw.name : col == 1 ? rw.value : rw.desc) = text;
  const Row& last = rows.back();
  if (!last.name.empty() || !last.value.empty() || !last.desc.empty()) rows.push_back(Row{});
}

void HeaderGridModel::key(char k) {
  if (k == '\v' || k == '\f') {
    if (list.empty()) return;
    const int n = static_cast<int>(list.size());
    sel = k == '\v' ? std::min(sel + 1, n - 1) : std::max(sel - 1, 0);
    return;
  }
  if (k == '\t' || k == '\n') {
    if (!list.empty() && sel >= 0) {
      text = list[static_cast<size_t>(sel)];
      list.clear();
      sel = -1;
      if (choice_continues(text)) return;  // "Bearer ": the token follows
    }
    list.clear();
    commit();
    const bool enter = k == '\n';
    if (enter && col == 0 && text.empty()) return open(row, 0);  // nothing to commit: keep typing here
    int r = row, c = col + 1;
    if (c > (enter ? 1 : 2)) {
      c = 0;
      ++r;
    }
    if (r >= static_cast<int>(rows.size())) r = static_cast<int>(rows.size()) - 1;
    return open(r, c);
  }
  text.push_back(k);
  refresh_list();
}

std::vector<std::pair<std::string, std::string>> HeaderGridModel::headers() const {
  std::vector<std::pair<std::string, std::string>> out;
  for (const auto& r : rows)
    if (!r.name.empty() || !r.value.empty()) out.emplace_back(r.name, r.value);
  return out;
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

namespace {

constexpr std::string_view kRowDocsOpen = "<!-- watt:descriptions";

std::string escape_doc_field(std::string_view s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '\\': o += "\\\\"; break;
      case '\t': o += "\\t"; break;
      case '\r': o += "\\r"; break;
      case '\n': o += "\\n"; break;
      case '>': o += "\\g"; break;
      default: o += c; break;
    }
  }
  return o;
}

std::string unescape_doc_field(std::string_view s) {
  std::string o;
  o.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\\' || i + 1 == s.size()) {
      o += s[i];
      continue;
    }
    switch (s[++i]) {
      case 't': o += '\t'; break;
      case 'r': o += '\r'; break;
      case 'n': o += '\n'; break;
      case 'g': o += '>'; break;
      default: o += s[i]; break;
    }
  }
  return o;
}

}  // namespace

std::string split_row_docs(std::string_view docs, RowDocs& out) {
  out = {};
  std::string_view body = docs;
  while (!body.empty() && (body.back() == '\n' || body.back() == '\r' || body.back() == ' ')) body.remove_suffix(1);
  if (body.size() < 3 || body.substr(body.size() - 3) != "-->") return std::string(docs);
  const size_t open = body.rfind(kRowDocsOpen);
  if (open == std::string_view::npos || (open > 0 && body[open - 1] != '\n')) return std::string(docs);
  std::string_view block = body.substr(open + kRowDocsOpen.size(), body.size() - 3 - open - kRowDocsOpen.size());
  if (block.find("-->") != std::string_view::npos) return std::string(docs);
  // first line is the title (rest of the opening line)
  size_t nl = block.find('\n');
  block = nl == std::string_view::npos ? std::string_view() : block.substr(nl + 1);
  while (!block.empty()) {
    nl = block.find('\n');
    std::string_view line = block.substr(0, nl);
    block = nl == std::string_view::npos ? std::string_view() : block.substr(nl + 1);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    const size_t t1 = line.find('\t');
    if (t1 == std::string_view::npos) continue;
    const size_t t2 = line.find('\t', t1 + 1);
    if (t2 == std::string_view::npos) continue;
    const std::string_view kind = line.substr(0, t1);
    std::string key = unescape_doc_field(line.substr(t1 + 1, t2 - t1 - 1));
    if (kind == "pre") {
      // var, expression, scope, on|off
      std::string_view rest = line.substr(t2 + 1);
      const size_t t3 = rest.find('\t');
      const std::string_view expr = rest.substr(0, t3);
      rest = t3 == std::string_view::npos ? std::string_view() : rest.substr(t3 + 1);
      const size_t t4 = rest.find('\t');
      PreRow pr;
      pr.var = std::move(key);
      pr.expr = unescape_doc_field(expr);
      pr.scope = unescape_doc_field(rest.substr(0, t4));
      pr.enabled = t4 == std::string_view::npos || rest.substr(t4 + 1) != "off";
      if (!pr.var.empty()) out.pre.push_back(std::move(pr));
      continue;
    }
    std::string text = unescape_doc_field(line.substr(t2 + 1));
    if (kind == "auth") {
      if (!key.empty()) out.auth.emplace_back(std::move(key), std::move(text));
      continue;
    }
    if (key.empty() || text.empty()) continue;
    if (kind == "param") out.params.emplace_back(std::move(key), std::move(text));
    else if (kind == "header") out.headers.emplace_back(std::move(key), std::move(text));
    else if (kind == "assert") out.asserts.emplace_back(std::move(key), std::move(text));
  }
  std::string_view rest = docs.substr(0, open);
  while (!rest.empty() && (rest.back() == '\n' || rest.back() == '\r')) rest.remove_suffix(1);
  return std::string(rest);
}

std::string join_row_docs(std::string_view docs, const RowDocs& d) {
  if (d.empty()) return std::string(docs);
  std::string o(docs);
  while (!o.empty() && (o.back() == '\n' || o.back() == '\r')) o.pop_back();
  if (!o.empty()) o += "\n\n";
  o += kRowDocsOpen;
  o += " (watt editor data: row notes, pre-request variables, auth settings)\n";
  auto add = [&](const char* kind, const std::vector<std::pair<std::string, std::string>>& rows) {
    for (const auto& [k, t] : rows) {
      if (k.empty() || t.empty()) continue;
      o += kind;
      o += '\t';
      o += escape_doc_field(k);
      o += '\t';
      o += escape_doc_field(t);
      o += '\n';
    }
  };
  add("param", d.params);
  add("header", d.headers);
  add("assert", d.asserts);
  for (const auto& p : d.pre) {
    if (p.var.empty()) continue;
    o += "pre\t" + escape_doc_field(p.var) + '\t' + escape_doc_field(p.expr) + '\t' + escape_doc_field(p.scope) +
         (p.enabled ? "\ton\n" : "\toff\n");
  }
  for (const auto& [k, v] : d.auth) {
    if (k.empty()) continue;
    o += "auth\t" + escape_doc_field(k) + '\t' + escape_doc_field(v) + '\n';
  }
  o += "-->\n";
  return o;
}

std::vector<std::pair<std::string, std::string>> parse_cookie_header(std::string_view v) {
  std::vector<std::pair<std::string, std::string>> out;
  while (!v.empty()) {
    const size_t semi = v.find(';');
    std::string_view part = trim(v.substr(0, semi));
    v = semi == std::string_view::npos ? std::string_view() : v.substr(semi + 1);
    if (part.empty()) continue;
    const size_t eq = part.find('=');
    if (eq == std::string_view::npos) out.emplace_back(std::string(part), std::string());
    else out.emplace_back(std::string(trim(part.substr(0, eq))), std::string(trim(part.substr(eq + 1))));
  }
  return out;
}

std::string strip_curl_default_scheme(std::string_view url, std::string_view cmd) {
  constexpr std::string_view kAdded = "http://{{";
  if (url.size() < kAdded.size() || !istarts_with(url, kAdded)) return std::string(url);
  // written in the command (any case): kept
  for (size_t at = 0; (at = cmd.find("://{{", at)) != std::string_view::npos; ++at)
    if (at >= 4 && istarts_with(cmd.substr(at - 4), "http")) return std::string(url);
  return std::string(url.substr(7));
}

bool doubled_scheme(std::string_view url) {
  const size_t a = url.find("://");
  if (a == std::string_view::npos || a == 0 || a > 5) return false;
  const std::string_view rest = url.substr(a + 3);
  return istarts_with(rest, "http://") || istarts_with(rest, "https://");
}

namespace {

void append_json_string(std::string& out, std::string_view v) {
  out += '"';
  for (const char ch : v) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          static const char* const hex = "0123456789abcdef";
          out += "\\u00";
          out += hex[c >> 4];
          out += hex[c & 15];
        } else {
          out += ch;
        }
    }
  }
  out += '"';
}

}  // namespace

std::string graphql_body(std::string_view query, std::string_view variables, std::string_view operation_name) {
  std::string out = "{\"query\":";
  append_json_string(out, query);
  const std::string_view vars = trim(variables);
  if (!vars.empty()) {
    out += ",\"variables\":";
    out += vars;
  }
  if (!trim(operation_name).empty()) {
    out += ",\"operationName\":";
    append_json_string(out, trim(operation_name));
  }
  out += '}';
  return out;
}

bool split_graphql_body(std::string_view body, std::string& query, std::string& variables, std::string* operation_name) {
  const std::string_view b = trim(body);
  if (b.size() < 10 || b.front() != '{' || b.find("\"query\"") == std::string_view::npos) return false;
  rapidjson::Document d;
  d.Parse(b.data(), b.size());
  if (d.HasParseError() || !d.IsObject()) return false;
  const auto q = d.FindMember("query");
  if (q == d.MemberEnd() || !q->value.IsString()) return false;
  std::string vars, op;
  for (auto it = d.MemberBegin(); it != d.MemberEnd(); ++it) {
    const std::string_view k(it->name.GetString(), it->name.GetStringLength());
    if (k == "query") continue;
    if (k == "variables") {
      if (it->value.IsNull()) continue;
      if (!it->value.IsObject()) return false;
      rapidjson::StringBuffer sb;
      rapidjson::PrettyWriter<rapidjson::StringBuffer> w(sb);
      w.SetIndent(' ', 2);
      it->value.Accept(w);
      vars.assign(sb.GetString(), sb.GetSize());
    } else if (k == "operationName") {
      if (it->value.IsNull()) continue;
      if (!it->value.IsString()) return false;
      op.assign(it->value.GetString(), it->value.GetStringLength());
    } else {
      return false;  // something else rides along: not a plain GraphQL body
    }
  }
  query.assign(q->value.GetString(), q->value.GetStringLength());
  variables = std::move(vars);
  if (operation_name) *operation_name = std::move(op);
  else if (!op.empty()) return false;  // the caller could not keep it
  return true;
}

void lex_json(std::string_view text, JsonLexState& st, std::string* styles, std::vector<JsonLexLine>* lines) {
  const size_t n = text.size();
  styles->assign(n, static_cast<char>(kJsDefault));
  char* out = styles->data();
  const char* p = text.data();
  auto word_char = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '.' || c == '-' || c == '+'; };
  size_t i = 0;
  while (i < n) {
    // One line: [i, eol), then the line break.
    const void* nlp = std::memchr(p + i, '\n', n - i);
    const size_t eol = nlp ? static_cast<size_t>(static_cast<const char*>(nlp) - p) : n;
    JsonLexLine line{st.depth, false, st.in_comment};
    size_t k = i;
    while (k < eol) {
      const char c = p[k];
      if (st.in_comment) {
        const size_t s0 = k;
        while (k < eol && !(p[k] == '*' && k + 1 < eol && p[k + 1] == '/')) ++k;
        if (k < eol) {
          k += 2;
          st.in_comment = false;
        }
        std::memset(out + s0, kJsBlockComment, k - s0);
        continue;
      }
      if (c == ' ' || c == '\t' || c == '\r') {
        ++k;
        continue;
      }
      if (c == '"') {
        const size_t s0 = k++;
        bool closed = false;
        while (k < eol) {
          if (p[k] == '\\') {
            const size_t e = std::min(k + 2, eol);
            std::memset(out + k, kJsEscape, e - k);
            k = e;
            continue;
          }
          if (p[k] == '"') {
            ++k;
            closed = true;
            break;
          }
          out[k++] = kJsString;
        }
        // A key when ':' follows.
        size_t a = k;
        while (a < eol && (p[a] == ' ' || p[a] == '\t')) ++a;
        const char st_kind = !closed ? kJsStringEol : (a < eol && p[a] == ':') ? kJsProperty : kJsString;
        for (size_t x = s0; x < k; ++x)
          if (out[x] != kJsEscape || !closed) out[x] = st_kind;
        continue;
      }
      if (c == '{' && k + 1 < eol && p[k + 1] == '{') {
        // {{var}}: plain text up to the closing braces on this line.
        const size_t e = text.substr(0, eol).find("}}", k + 2);
        if (e != std::string_view::npos) {
          k = e + 2;  // style stays kJsDefault
          continue;
        }
      }
      if (c == '{' || c == '[') {
        out[k++] = kJsOperator;
        ++st.depth;
        continue;
      }
      if (c == '}' || c == ']') {
        out[k++] = kJsOperator;
        if (st.depth > 0) --st.depth;
        continue;
      }
      if (c == ',' || c == ':') {
        out[k++] = kJsOperator;
        continue;
      }
      if (c == '/' && k + 1 < eol && p[k + 1] == '/') {
        std::memset(out + k, kJsLineComment, eol - k);
        k = eol;
        continue;
      }
      if (c == '/' && k + 1 < eol && p[k + 1] == '*') {
        out[k] = out[k + 1] = kJsBlockComment;
        k += 2;
        st.in_comment = true;
        continue;
      }
      if (word_char(c)) {
        const size_t s0 = k;
        while (k < eol && word_char(p[k])) ++k;
        const std::string_view w = text.substr(s0, k - s0);
        char sty = kJsError;
        if (w == "true" || w == "false" || w == "null") {
          sty = kJsKeyword;
        } else {
          // A number: -?digits[.digits][e[+-]digits]
          size_t j = 0;
          if (j < w.size() && w[j] == '-') ++j;
          const size_t d0 = j;
          while (j < w.size() && std::isdigit(static_cast<unsigned char>(w[j]))) ++j;
          if (j > d0) {
            if (j < w.size() && w[j] == '.') {
              ++j;
              while (j < w.size() && std::isdigit(static_cast<unsigned char>(w[j]))) ++j;
            }
            if (j < w.size() && (w[j] == 'e' || w[j] == 'E')) {
              ++j;
              if (j < w.size() && (w[j] == '+' || w[j] == '-')) ++j;
              while (j < w.size() && std::isdigit(static_cast<unsigned char>(w[j]))) ++j;
            }
            if (j == w.size()) sty = kJsNumber;
          }
        }
        std::memset(out + s0, sty, k - s0);
        continue;
      }
      out[k++] = kJsError;
    }
    line.header = st.depth > line.depth;
    if (lines) lines->push_back(line);
    i = eol + 1;
    if (!nlp) break;
  }
}

size_t json_block_end(std::string_view text, size_t opener) {
  const char* const p = text.data();
  const size_t len = text.size();
  if (opener >= len || (p[opener] != '{' && p[opener] != '[')) return std::string_view::npos;
  size_t depth = 0;
  bool str = false;
  size_t skip_to = 0;  // the byte after a backslash inside a string is not looked at
  // One byte that may matter: go on, the block end (Found), or resume at `jump` (a comment).
  enum class Step { Go, Found, Jump };
  auto handle = [&](size_t pos, size_t& jump) -> Step {
    if (pos < skip_to) return Step::Go;
    const char c = p[pos];
    if (str) {
      if (c == '\\') skip_to = pos + 2;
      else if (c == '"') str = false;
      return Step::Go;
    }
    switch (c) {
      case '"': str = true; break;
      case '{': case '[': ++depth; break;
      case '}': case ']':
        if (depth > 0 && --depth == 0) return Step::Found;
        break;
      case '/':
        if (pos + 1 < len && p[pos + 1] == '/') {
          const void* nl = std::memchr(p + pos, '\n', len - pos);
          jump = nl ? static_cast<size_t>(static_cast<const char*>(nl) - p) : len;
          return Step::Jump;
        }
        if (pos + 1 < len && p[pos + 1] == '*') {
          const size_t e = text.find("*/", pos + 2);
          jump = e == std::string_view::npos ? len : e + 2;
          return Step::Jump;
        }
        break;
      default: break;
    }
    return Step::Go;
  };
  // 16 bytes per step; every candidate byte of a block is handled from its bit mask (JSON is dense
  // with quotes and brackets, so leaving the vector loop per candidate would cost more than it saves).
  const __m128i q = _mm_set1_epi8('"'), bs = _mm_set1_epi8('\\'), ob = _mm_set1_epi8('{'), cb = _mm_set1_epi8('}'),
                os = _mm_set1_epi8('['), cs = _mm_set1_epi8(']'), sl = _mm_set1_epi8('/');
  size_t i = opener;
  while (i + 16 <= len) {
    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
    const __m128i m = _mm_or_si128(
        _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, q), _mm_cmpeq_epi8(v, bs)),
                     _mm_or_si128(_mm_cmpeq_epi8(v, ob), _mm_cmpeq_epi8(v, cb))),
        _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, os), _mm_cmpeq_epi8(v, cs)), _mm_cmpeq_epi8(v, sl)));
    unsigned bits = static_cast<unsigned>(_mm_movemask_epi8(m));
    size_t next = i + 16;
    while (bits) {
      unsigned long k = 0;
      _BitScanForward(&k, bits);
      bits &= bits - 1;
      size_t jump = 0;
      const Step s = handle(i + k, jump);
      if (s == Step::Found) return i + k;
      if (s == Step::Jump) {
        next = jump;
        break;
      }
    }
    i = next;
  }
  for (; i < len; ++i) {
    size_t jump = 0;
    const Step s = handle(i, jump);
    if (s == Step::Found) return i;
    if (s == Step::Jump) i = jump - 1;  // the loop's ++i lands on jump
  }
  return std::string_view::npos;
}

std::vector<std::pair<int, int>> json_big_blocks(std::string_view text, int min_lines) {
  std::vector<std::pair<int, int>> out;
  std::vector<int> open;  // opener lines, innermost last
  const char* p = text.data();
  const size_t len = text.size();
  int line = 0;
  for (size_t i = 0; i < len; ++i) {
    switch (p[i]) {
      case '\n': ++line; break;
      case '"':
        for (++i; i < len && p[i] != '"'; ++i) {
          if (p[i] == '\\') ++i;
          if (i < len && p[i] == '\n') ++line;  // not JSON, but keep the count right
        }
        break;
      case '{': case '[': open.push_back(line); break;
      case '}': case ']':
        if (!open.empty()) {
          const int from = open.back();
          open.pop_back();
          if (line - from >= min_lines) out.emplace_back(from, line);
        }
        break;
      case '/':
        if (i + 1 < len && p[i + 1] == '/') {
          const void* nl = std::memchr(p + i, '\n', len - i);
          i = nl ? static_cast<size_t>(static_cast<const char*>(nl) - p) - 1 : len;  // ++i lands on the '\n'
        } else if (i + 1 < len && p[i + 1] == '*') {
          const size_t e = text.find("*/", i + 2);
          const size_t stop = e == std::string_view::npos ? len : e + 2;
          for (size_t k = i; k < stop; ++k) line += p[k] == '\n';
          i = stop - 1;
        }
        break;
      default: break;
    }
  }
  // Inner blocks close first: sort by opener line and keep the outermost (longest) per line.
  std::sort(out.begin(), out.end());
  std::vector<std::pair<int, int>> merged;
  for (const auto& b : out) {
    if (!merged.empty() && merged.back().first == b.first) merged.back().second = std::max(merged.back().second, b.second);
    else merged.push_back(b);
  }
  return merged;
}

namespace {
// Finds successive occurrences of needle (ASCII case folding unless match_case). The next hit of
// each case of the first byte is kept across calls, so walking every match of a 10 MB text is
// one pass: a fresh memchr per call for a case that never occurs ("name" in a body without a
// capital N) rescanned the rest of the text for every match (O(matches x size)).
class Scanner {
 public:
  Scanner(std::string_view hay, std::string_view needle, bool match_case)
      : hay_(hay), needle_(needle), mc_(match_case) {
    if (needle.empty() || needle.size() > hay.size()) {
      last_ = 0;
      return;
    }
    last_ = hay.size() - needle.size() + 1;
    f0_ = low(static_cast<unsigned char>(needle[0]));
    f1_ = f0_ >= 'a' && f0_ <= 'z' ? static_cast<unsigned char>(f0_ - 32) : f0_;
    if (mc_) f0_ = f1_ = static_cast<unsigned char>(needle[0]);
  }

  // Next occurrence at or after from; from must not go backwards between calls.
  size_t next(size_t from) {
    if (from >= last_) return std::string_view::npos;
    const char* const base = hay_.data();
    const char* const end = base + last_;
    size_t i = from;
    while (i < last_) {
      if (na_ < base + i) na_ = scan(f0_, i);
      if (f1_ != f0_) {
        if (nb_ < base + i) nb_ = scan(f1_, i);
      } else {
        nb_ = end;
      }
      const char* p = na_ < nb_ ? na_ : nb_;
      if (p >= end) return std::string_view::npos;
      i = static_cast<size_t>(p - base);
      size_t k = 1;
      if (mc_) {
        while (k < needle_.size() && hay_[i + k] == needle_[k]) ++k;
      } else {
        while (k < needle_.size() &&
               low(static_cast<unsigned char>(hay_[i + k])) == low(static_cast<unsigned char>(needle_[k])))
          ++k;
      }
      if (k == needle_.size()) return i;
      ++i;
    }
    return std::string_view::npos;
  }

 private:
  static unsigned char low(unsigned char c) { return static_cast<unsigned char>(c >= 'A' && c <= 'Z' ? c + 32 : c); }
  const char* scan(unsigned char c, size_t at) const {
    const void* q = at < last_ ? std::memchr(hay_.data() + at, c, last_ - at) : nullptr;
    return q ? static_cast<const char*>(q) : hay_.data() + last_;
  }

  std::string_view hay_, needle_;
  bool mc_;
  size_t last_ = 0;  // one past the last position a match can start at
  unsigned char f0_ = 0, f1_ = 0;
  const char* na_ = nullptr;  // next hit of f0_ / f1_ (stale when before the cursor)
  const char* nb_ = nullptr;
};

bool stopped(const std::atomic<bool>* stop) { return stop && stop->load(std::memory_order_relaxed); }
}  // namespace

MatchCount count_matches(std::string_view hay, std::string_view needle, bool match_case, size_t before, size_t limit,
                         const std::atomic<bool>* stop) {
  MatchCount m;
  if (needle.empty()) return m;
  Scanner sc(hay, needle, match_case);
  for (size_t at = sc.next(0); at != std::string_view::npos; at = sc.next(at + needle.size())) {
    if (m.total >= limit) {
      m.capped = true;
      break;
    }
    if ((m.total & 0xFFF) == 0 && stopped(stop)) {
      m.cancelled = true;
      break;
    }
    ++m.total;
    if (at < before) ++m.before;
  }
  return m;
}

size_t find_match(std::string_view hay, std::string_view needle, bool match_case, size_t from, bool forward,
                  const std::atomic<bool>* stop) {
  if (needle.empty() || needle.size() > hay.size()) return std::string_view::npos;
  from = std::min(from, hay.size());
  if (forward) {
    const size_t at = Scanner(hay, needle, match_case).next(from);
    return at != std::string_view::npos ? at : Scanner(hay, needle, match_case).next(0);
  }
  Scanner sc(hay, needle, match_case);
  size_t last_before = std::string_view::npos, last = std::string_view::npos;
  size_t n = 0;
  for (size_t at = sc.next(0); at != std::string_view::npos; at = sc.next(at + 1)) {
    if ((++n & 0xFFF) == 0 && stopped(stop)) return std::string_view::npos;
    if (at >= from && last_before != std::string_view::npos) break;
    if (at < from) last_before = at;
    last = at;
  }
  return last_before != std::string_view::npos ? last_before : last;
}

std::string replace_all(std::string_view hay, std::string_view needle, std::string_view with, bool match_case, size_t* n) {
  std::string out;
  size_t count = 0;
  if (needle.empty()) {
    if (n) *n = 0;
    return std::string(hay);
  }
  out.reserve(hay.size());
  size_t from = 0;
  Scanner sc(hay, needle, match_case);
  for (size_t at = sc.next(0); at != std::string_view::npos; at = sc.next(from)) {
    out.append(hay.substr(from, at - from));
    out.append(with);
    from = at + needle.size();
    ++count;
  }
  out.append(hay.substr(from));
  if (n) *n = count;
  return out;
}

namespace {
void html_escape_to(std::string& out, std::string_view s) {
  for (const char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
}

// `code`, **bold**, *italic* / _italic_, [text](http(s)://url); everything escaped.
std::string md_inline(std::string_view s) {
  std::string out;
  size_t i = 0;
  bool bold = false, em = false;
  while (i < s.size()) {
    const char c = s[i];
    if (c == '`') {
      const size_t e = s.find('`', i + 1);
      if (e != std::string_view::npos) {
        out += "<code>";
        html_escape_to(out, s.substr(i + 1, e - i - 1));
        out += "</code>";
        i = e + 1;
        continue;
      }
    }
    if (c == '*' && i + 1 < s.size() && s[i + 1] == '*') {
      out += bold ? "</b>" : "<b>";
      bold = !bold;
      i += 2;
      continue;
    }
    if ((c == '*' || c == '_') && (em || (i + 1 < s.size() && s[i + 1] != ' '))) {
      // '_' inside a word (snake_case) stays a character
      if (c == '_' && !em && i > 0 && std::isalnum(static_cast<unsigned char>(s[i - 1]))) {
        out += c;
        ++i;
        continue;
      }
      out += em ? "</i>" : "<i>";
      em = !em;
      ++i;
      continue;
    }
    if (c == '[') {
      const size_t close = s.find(']', i + 1);
      if (close != std::string_view::npos && close + 1 < s.size() && s[close + 1] == '(') {
        const size_t pe = s.find(')', close + 2);
        if (pe != std::string_view::npos) {
          const std::string_view text = s.substr(i + 1, close - i - 1);
          const std::string_view url = trim(s.substr(close + 2, pe - close - 2));
          if (istarts_with(url, "https://") || istarts_with(url, "http://")) {
            out += "<a href=\"";
            html_escape_to(out, url);
            out += "\">";
            html_escape_to(out, text);
            out += "</a>";
          } else {
            html_escape_to(out, text);
          }
          i = pe + 1;
          continue;
        }
      }
    }
    html_escape_to(out, std::string_view(&c, 1));
    ++i;
  }
  if (em) out += "</i>";
  if (bold) out += "</b>";
  return out;
}
}  // namespace

std::string markdown_to_html(std::string_view md_in) {
  // CRLF and a lone CR (old Mac files) end a line like LF.
  std::string norm;
  norm.reserve(md_in.size());
  for (size_t i = 0; i < md_in.size(); ++i) {
    if (md_in[i] == '\r') {
      norm += '\n';
      if (i + 1 < md_in.size() && md_in[i + 1] == '\n') ++i;
    } else {
      norm += md_in[i];
    }
  }
  const std::string_view md = norm;
  std::string out;
  std::string para;
  enum class List { None, Ul, Ol } list = List::None;
  bool fence = false;
  std::string code;
  auto flush_para = [&] {
    if (!para.empty()) {
      out += "<p>" + md_inline(para) + "</p>\n";
      para.clear();
    }
  };
  auto close_list = [&] {
    if (list == List::Ul) out += "</ul>\n";
    if (list == List::Ol) out += "</ol>\n";
    list = List::None;
  };
  size_t pos = 0;
  while (pos <= md.size()) {
    size_t nl = md.find('\n', pos);
    if (nl == std::string_view::npos) nl = md.size();
    std::string_view line = md.substr(pos, nl - pos);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    pos = nl + 1;
    const std::string_view t = trim(line);
    if (t.substr(0, 3) == "```") {
      if (fence) {
        out += "<pre><code>";
        html_escape_to(out, code);
        out += "</code></pre>\n";
        code.clear();
      } else {
        flush_para();
        close_list();
      }
      fence = !fence;
      continue;
    }
    if (fence) {
      code.append(line);
      code += '\n';
      continue;
    }
    if (t.empty()) {
      flush_para();
      close_list();
      continue;
    }
    size_t h = 0;
    while (h < t.size() && h < 6 && t[h] == '#') ++h;
    if (h > 0 && h < t.size() && t[h] == ' ') {
      flush_para();
      close_list();
      out += "<h" + std::to_string(h) + ">" + md_inline(trim(t.substr(h + 1))) + "</h" + std::to_string(h) + ">\n";
      continue;
    }
    if (t == "---" || t == "***" || t == "___") {
      flush_para();
      close_list();
      out += "<hr>\n";
      continue;
    }
    if (t[0] == '>') {
      flush_para();
      close_list();
      out += "<blockquote>" + md_inline(trim(t.substr(1))) + "</blockquote>\n";
      continue;
    }
    const bool ul = t.size() > 2 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ';
    size_t d = 0;
    while (d < t.size() && std::isdigit(static_cast<unsigned char>(t[d]))) ++d;
    const bool ol = d > 0 && d + 1 < t.size() && (t[d] == '.' || t[d] == ')') && t[d + 1] == ' ';
    if (ul || ol) {
      flush_para();
      const List want = ul ? List::Ul : List::Ol;
      if (list != want) {
        close_list();
        out += ul ? "<ul>\n" : "<ol>\n";
        list = want;
      }
      out += "<li>" + md_inline(trim(t.substr(ul ? 2 : d + 2))) + "</li>\n";
      continue;
    }
    close_list();
    if (!para.empty()) para += ' ';
    para.append(t);
  }
  if (fence) {
    out += "<pre><code>";
    html_escape_to(out, code);
    out += "</code></pre>\n";
  }
  flush_para();
  close_list();
  return out;
}

std::vector<std::string> script_set_names(std::string_view js, bool* dynamic) {
  if (dynamic) *dynamic = false;
  std::vector<std::string> out;
  // The code with comments blanked out (string contents kept), so a commented-out call or a
  // "//" inside a URL string is read right.
  std::string code(js);
  for (size_t i = 0; i < code.size();) {
    const char c = code[i];
    if (c == '"' || c == '\'' || c == '`') {
      size_t j = i + 1;
      while (j < code.size() && code[j] != c) j += code[j] == '\\' ? 2 : 1;
      i = j + 1;
    } else if (c == '/' && i + 1 < code.size() && code[i + 1] == '/') {
      while (i < code.size() && code[i] != '\n') code[i++] = ' ';
    } else if (c == '/' && i + 1 < code.size() && code[i + 1] == '*') {
      const size_t e = code.find("*/", i + 2);
      const size_t end = e == std::string::npos ? code.size() : e + 2;
      for (; i < end; ++i)
        if (code[i] != '\n') code[i] = ' ';
    } else {
      ++i;
    }
  }
  auto ident = [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '$'; };
  auto skip_ws = [&](size_t p) {
    while (p < code.size() && std::isspace(static_cast<unsigned char>(code[p]))) ++p;
    return p;
  };
  // "a.b.c" with optional spaces around the dots, starting at p; returns the end or npos.
  auto match_path = [&](size_t p, std::initializer_list<std::string_view> parts) -> size_t {
    bool first = true;
    for (std::string_view part : parts) {
      if (!first) {
        p = skip_ws(p);
        if (p >= code.size() || code[p] != '.') return std::string::npos;
        p = skip_ws(p + 1);
      }
      first = false;
      if (code.compare(p, part.size(), part) != 0) return std::string::npos;
      p += part.size();
      if (p < code.size() && ident(code[p])) return std::string::npos;
    }
    return p;
  };
  static const std::vector<std::vector<std::string_view>> setters = {
      {"pm", "variables", "set"},  {"pm", "environment", "set"}, {"pm", "globals", "set"},
      {"pm", "collectionVariables", "set"}, {"bru", "setVar"}, {"bru", "setEnvVar"},
      {"bru", "setGlobalEnvVar"}};
  for (size_t i = 0; i < code.size(); ++i) {
    if ((code[i] != 'p' && code[i] != 'b' && code[i] != 'r') || (i > 0 && (ident(code[i - 1]) || code[i - 1] == '.')))
      continue;
    // A rewrite of the URL: "pm.request.url = ..." / ".update(" / req.setUrl(...).
    if (size_t e = match_path(i, {"pm", "request", "url"}); e != std::string::npos) {
      const size_t q = skip_ws(e);
      if (q < code.size() && (code[q] == '.' || (code[q] == '=' && (q + 1 >= code.size() || code[q + 1] != '='))))
        if (dynamic) *dynamic = true;
      continue;
    }
    if (match_path(i, {"req", "setUrl"}) != std::string::npos) {
      if (dynamic) *dynamic = true;
      continue;
    }
    for (const auto& w : setters) {
      size_t e = w.size() == 3 ? match_path(i, {w[0], w[1], w[2]}) : match_path(i, {w[0], w[1]});
      if (e == std::string::npos) continue;
      e = skip_ws(e);
      if (e >= code.size() || code[e] != '(') break;
      e = skip_ws(e + 1);
      const char q = e < code.size() ? code[e] : '\0';
      std::string name;
      bool literal = false;
      if (q == '"' || q == '\'' || q == '`') {
        size_t j = e + 1;
        literal = true;
        while (j < code.size() && code[j] != q) {
          if (code[j] == '\\' || (q == '`' && code[j] == '$' && j + 1 < code.size() && code[j + 1] == '{')) {
            literal = false;
            break;
          }
          name += code[j++];
        }
        if (j >= code.size()) literal = false;
        if (literal) {
          const size_t after = skip_ws(j + 1);
          literal = after < code.size() && (code[after] == ',' || code[after] == ')');
        }
      }
      if (literal && !name.empty()) {
        if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(std::move(name));
      } else if (dynamic) {
        *dynamic = true;
      }
      break;
    }
  }
  return out;
}

namespace {
std::string pick_lower(std::string_view s) {
  std::string o(s);
  for (char& c : o)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return o;
}
}  // namespace

std::vector<size_t> rank_pick_matches(const std::vector<PickMatch>& items, std::string_view filter) {
  std::vector<size_t> out;
  if (filter.empty()) {
    for (size_t i = 0; i < items.size(); ++i) out.push_back(i);
    return out;
  }
  const std::string f = pick_lower(filter);
  std::vector<int> tier(items.size(), -1);
  for (size_t i = 0; i < items.size(); ++i) {
    const std::string v = pick_lower(items[i].value);
    bool exact = v == f, pre = v.starts_with(f);
    const bool in = pre || v.find(f) != std::string::npos;
    std::string_view al = items[i].alias;
    while (!al.empty()) {
      const size_t sp = al.find(' ');
      const std::string a = pick_lower(al.substr(0, sp));
      al = sp == std::string_view::npos ? std::string_view{} : al.substr(sp + 1);
      if (a.empty()) continue;
      if (a == f) exact = true;
      if (a.starts_with(f)) pre = true;
    }
    if (exact) tier[i] = 0;
    else if (pre) tier[i] = 1;
    else if (in) tier[i] = 2;
    else if (pick_lower(items[i].desc).find(f) != std::string::npos) tier[i] = 3;
  }
  for (int t = 0; t < 4; ++t)
    for (size_t i = 0; i < items.size(); ++i)
      if (tier[i] == t) out.push_back(i);
  return out;
}

const std::vector<AssertOperator>& assert_operators() {
  static const std::vector<AssertOperator> ops = {
      {"eq", "equals (numbers numerically)", "Compare", "== === to.equal to.eql equals"},
      {"neq", "not equal", "Compare", "!= !== <>"},
      {"gt", "greater than", "Compare", ">"},
      {"gte", "greater or equal", "Compare", ">="},
      {"lt", "less than", "Compare", "<"},
      {"lte", "less or equal", "Compare", "<="},
      {"between", "lo, hi (inclusive)", "Compare", ""},
      {"in", "one of a, b, c", "Compare", ""},
      {"notIn", "none of a, b, c", "Compare", ""},
      {"contains", "substring / element / key", "String", "includes to.include has"},
      {"notContains", "does not contain", "String", ""},
      {"startsWith", "begins with", "String", ""},
      {"endsWith", "ends with", "String", ""},
      {"matches", "regular expression", "String", "regex to.match"},
      {"notMatches", "regex does not match", "String", ""},
      {"length", "string / array / object size", "String", "size count len to.have.length"},
      {"isString", "", "Type", ""},
      {"isNumber", "", "Type", ""},
      {"isBoolean", "", "Type", ""},
      {"isArray", "", "Type", ""},
      {"isJson", "parses as JSON", "Type", ""},
      {"isDefined", "present", "Existence", "exists exist to.exist defined present"},
      {"isUndefined", "missing", "Existence", "undefined to.not.exist notexists missing absent"},
      {"isNull", "null", "Existence", "null to.be.null"},
      {"isEmpty", "\"\", [] or {}", "Existence", "empty to.be.empty"},
      {"isNotEmpty", "", "Existence", ""},
      {"isTruthy", "", "Existence", ""},
      {"isFalsy", "false, 0, \"\", null", "Existence", ""}};
  return ops;
}

}  // namespace watt::app::http_editor
