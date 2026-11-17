#include "app/response_tests.h"

#include <cctype>
#include <cstdlib>

namespace watt::app::rv {
namespace {

struct Scanner {
  std::string_view t;
  size_t p = 0;
  void ws() {
    while (p < t.size() && (t[p] == ' ' || t[p] == '\n' || t[p] == '\r' || t[p] == '\t')) ++p;
  }
  void utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  unsigned hex4(size_t at) const {
    if (at + 4 > t.size()) return 0;
    return static_cast<unsigned>(std::strtoul(std::string(t.substr(at, 4)).c_str(), nullptr, 16));
  }
  // A JSON string at p (decoded into *out when given).
  bool string(std::string* out) {
    if (p >= t.size() || t[p] != '"') return false;
    ++p;
    while (p < t.size()) {
      const char c = t[p];
      if (c == '"') {
        ++p;
        return true;
      }
      if (c != '\\') {
        if (out) *out += c;
        ++p;
        continue;
      }
      if (p + 1 >= t.size()) return false;
      const char e = t[p + 1];
      p += 2;
      if (!out) continue;
      switch (e) {
        case 'n': *out += '\n'; break;
        case 't': *out += '\t'; break;
        case 'r': *out += '\r'; break;
        case 'b': *out += '\b'; break;
        case 'f': *out += '\f'; break;
        case 'u': {
          unsigned cp = hex4(p);
          p += 4;
          if (cp >= 0xD800 && cp < 0xDC00 && p + 6 <= t.size() && t[p] == '\\' && t[p + 1] == 'u') {
            const unsigned lo = hex4(p + 2);
            p += 6;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          utf8(*out, cp);
          break;
        }
        default: *out += e; break;
      }
    }
    return false;
  }
  // Skips one value (p at its first byte, or at whitespace before it).
  bool skip() {
    ws();
    if (p >= t.size()) return false;
    const char c = t[p];
    if (c == '"') return string(nullptr);
    if (c == '{' || c == '[') {
      int depth = 0;
      while (p < t.size()) {
        const char d = t[p];
        if (d == '"') {
          if (!string(nullptr)) return false;
          continue;
        }
        if (d == '{' || d == '[') {
          ++depth;
        } else if (d == '}' || d == ']') {
          if (--depth == 0) {
            ++p;
            return true;
          }
        }
        ++p;
      }
      return false;
    }
    const size_t st = p;
    while (p < t.size() && t[p] != ',' && t[p] != '}' && t[p] != ']' && t[p] != ' ' && t[p] != '\n' && t[p] != '\r' &&
           t[p] != '\t')
      ++p;
    return p > st;
  }
  // p after '[': number of elements (p is restored).
  long count_elements() {
    const size_t save = p;
    long n = 0;
    ws();
    if (p < t.size() && t[p] == ']') {
      p = save;
      return 0;
    }
    while (p < t.size()) {
      if (!skip()) break;
      ++n;
      ws();
      if (p < t.size() && t[p] == ',') {
        ++p;
        continue;
      }
      break;
    }
    p = save;
    return n;
  }
};

}  // namespace

bool target_json_path(std::string_view t, std::vector<PathStep>& path, long* message) {
  path.clear();
  *message = -1;
  while (!t.empty() && t.front() == ' ') t.remove_prefix(1);
  while (!t.empty() && t.back() == ' ') t.remove_suffix(1);
  auto starts = [&](std::string_view p) { return t.substr(0, p.size()) == p; };
  bool grpc_messages = false;
  if (starts("res.body")) {
    t.remove_prefix(8);
  } else if (starts("res.messages")) {
    t.remove_prefix(12);
    grpc_messages = true;
  } else if (starts("$")) {
    t.remove_prefix(1);
  } else {
    return false;
  }
  size_t i = 0;
  while (i < t.size()) {
    if (t[i] == '.') {
      ++i;
      size_t j = i;
      while (j < t.size() && t[j] != '.' && t[j] != '[') ++j;
      if (j == i) return false;
      path.push_back(PathStep{std::string(t.substr(i, j - i))});
      i = j;
    } else if (t[i] == '[') {
      ++i;
      while (i < t.size() && t[i] == ' ') ++i;
      if (i >= t.size()) return false;
      if (t[i] == '"' || t[i] == '\'') {
        const char q = t[i++];
        std::string k;
        while (i < t.size() && t[i] != q) {
          if (t[i] == '\\' && i + 1 < t.size()) ++i;
          k += t[i++];
        }
        if (i >= t.size()) return false;
        ++i;
        path.push_back(PathStep{std::move(k)});
      } else {
        const size_t st = i;
        if (t[i] == '-') ++i;
        while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) ++i;
        if (i == st || (i == st + 1 && t[st] == '-')) return false;
        PathStep s;
        s.is_index = true;
        s.index = std::strtol(std::string(t.substr(st, i - st)).c_str(), nullptr, 10);
        path.push_back(std::move(s));
      }
      while (i < t.size() && t[i] == ' ') ++i;
      if (i >= t.size() || t[i] != ']') return false;
      ++i;
    } else {
      return false;
    }
  }
  // A trailing .length is the size of what is before it (unless it is a member: the walk finds
  // that anyway; revealing the container is the useful answer either way).
  if (!path.empty() && !path.back().is_index && path.back().key == "length") path.pop_back();
  if (grpc_messages) {
    if (path.empty() || !path.front().is_index) {
      *message = -2;  // all messages (no single one)
      return true;
    }
    *message = path.front().index;
    path.erase(path.begin());
  }
  return true;
}

size_t json_locate(std::string_view text, const std::vector<PathStep>& path, size_t* len, size_t* deepest_at) {
  Scanner s{text};
  s.ws();
  const size_t npos = std::string_view::npos;
  if (deepest_at) *deepest_at = s.p;
  for (const PathStep& st : path) {
    s.ws();
    if (s.p >= text.size()) return npos;
    if (st.is_index) {
      if (text[s.p] != '[') return npos;
      ++s.p;
      long want = st.index;
      if (want < 0) want += s.count_elements();
      if (want < 0) return npos;
      for (long i = 0;; ++i) {
        s.ws();
        if (s.p >= text.size() || text[s.p] == ']') return npos;
        if (i == want) break;
        if (!s.skip()) return npos;
        s.ws();
        if (s.p >= text.size() || text[s.p] != ',') return npos;
        ++s.p;
      }
    } else {
      if (text[s.p] != '{') return npos;
      ++s.p;
      for (;;) {
        s.ws();
        if (s.p >= text.size() || text[s.p] == '}') return npos;
        const size_t key_at = s.p;
        std::string key;
        if (!s.string(&key)) return npos;
        s.ws();
        if (s.p >= text.size() || text[s.p] != ':') return npos;
        ++s.p;
        if (key == st.key) {
          if (deepest_at) *deepest_at = key_at;
          break;
        }
        if (!s.skip()) return npos;
        s.ws();
        if (s.p >= text.size() || text[s.p] != ',') return npos;
        ++s.p;
      }
    }
    s.ws();
    if (deepest_at) *deepest_at = st.is_index ? s.p : *deepest_at;
  }
  s.ws();
  const size_t at = s.p;
  if (len) {
    Scanner e{text, at};
    *len = e.skip() ? e.p - at : 0;
  }
  return at;
}

std::string path_string(const std::vector<PathStep>& path) {
  std::string s = "$";
  for (const PathStep& p : path) {
    if (p.is_index) {
      s += "[" + std::to_string(p.index) + "]";
      continue;
    }
    bool plain = !p.key.empty() && !std::isdigit(static_cast<unsigned char>(p.key[0]));
    for (char c : p.key)
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$')) plain = false;
    if (plain) {
      s += "." + p.key;
    } else {
      s += "[\"";
      for (char c : p.key) {
        if (c == '"' || c == '\\') s += '\\';
        s += c;
      }
      s += "\"]";
    }
  }
  return s;
}

}  // namespace watt::app::rv
