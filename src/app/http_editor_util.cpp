#include "app/http_editor_util.h"

#include <algorithm>
#include <cctype>
#include <cstring>

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

namespace {
// Next occurrence of needle at or after from (npos when none).
size_t find_from(std::string_view hay, std::string_view needle, bool match_case, size_t from) {
  if (needle.empty() || from > hay.size()) return std::string_view::npos;
  if (match_case) return hay.find(needle, from);
  auto low = [](unsigned char c) { return static_cast<unsigned char>(c >= 'A' && c <= 'Z' ? c + 32 : c); };
  const unsigned char f0 = low(static_cast<unsigned char>(needle[0]));
  const unsigned char f1 = f0 >= 'a' && f0 <= 'z' ? static_cast<unsigned char>(f0 - 32) : f0;
  const size_t last = hay.size() - std::min(hay.size(), needle.size() - 1);
  // memchr for either case of the first byte, then compare the rest. The next hit of each case
  // is kept until passed: searching again from every candidate for a case that occurs late (or
  // never: "item" in a body without a capital I) rescanned the rest of a 10 MB text each time.
  const char* const base = hay.data();
  const char* const end = base + last;
  auto next = [&](unsigned char c, size_t at) -> const char* {
    const void* q = at < last ? std::memchr(base + at, c, last - at) : nullptr;
    return q ? static_cast<const char*>(q) : end;
  };
  size_t i = from;
  if (i >= last) return std::string_view::npos;
  const char* na = next(f0, i);
  const char* nb = f1 != f0 ? next(f1, i) : end;
  while (i < last) {
    if (na < base + i) na = next(f0, i);
    if (nb < base + i && f1 != f0) nb = next(f1, i);
    const char* p = na < nb ? na : nb;
    if (p >= end) return std::string_view::npos;
    i = static_cast<size_t>(p - base);
    size_t k = 1;
    while (k < needle.size() && low(static_cast<unsigned char>(hay[i + k])) == low(static_cast<unsigned char>(needle[k]))) ++k;
    if (k == needle.size()) return i;
    ++i;
  }
  return std::string_view::npos;
}
}  // namespace

MatchCount count_matches(std::string_view hay, std::string_view needle, bool match_case, size_t before, size_t limit) {
  MatchCount m;
  if (needle.empty()) return m;
  for (size_t at = find_from(hay, needle, match_case, 0); at != std::string_view::npos;
       at = find_from(hay, needle, match_case, at + needle.size())) {
    if (m.total >= limit) {
      m.capped = true;
      break;
    }
    ++m.total;
    if (at < before) ++m.before;
  }
  return m;
}

size_t find_match(std::string_view hay, std::string_view needle, bool match_case, size_t from, bool forward) {
  if (needle.empty() || needle.size() > hay.size()) return std::string_view::npos;
  from = std::min(from, hay.size());
  if (forward) {
    const size_t at = find_from(hay, needle, match_case, from);
    return at != std::string_view::npos ? at : find_from(hay, needle, match_case, 0);
  }
  size_t last_before = std::string_view::npos, last = std::string_view::npos;
  for (size_t at = find_from(hay, needle, match_case, 0); at != std::string_view::npos;
       at = find_from(hay, needle, match_case, at + 1)) {
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
  for (size_t at = find_from(hay, needle, match_case, 0); at != std::string_view::npos;
       at = find_from(hay, needle, match_case, from)) {
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

}  // namespace watt::app::http_editor
