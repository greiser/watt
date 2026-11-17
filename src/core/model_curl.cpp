// curl import/export and grpcurl export.
//
// parse_curl_command understands what browsers put on the clipboard:
//   - "Copy as cURL (bash)" (Chrome, Edge, Firefox): '...' and $'...' quoting, "..." with
//     backslash escapes, backslash-newline continuations (LF or CRLF).
//   - "Copy as cURL (cmd)" / "(Windows)" (Chrome, Edge, Firefox): cmd.exe caret escaping (^", ^\,
//     ^<newline>) followed by MS CRT argument quoting. Browsers escape every \ as \\ and every " as
//     \", so in that mode a backslash escapes the next character.
// and the curl options engineers actually use. Options that make no sense for an API tester
// (-o, -w, -s, -v, ...) are accepted and ignored so a pasted command never fails on them.
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "core/model.h"
#include "core/model_json.h"

namespace watt {

namespace {

// ------------------------------------------------------------------ tokenizers

void append_utf8(std::string& out, uint32_t cp) {
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

int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

std::string lower_ascii_copy(std::string v) {
  for (char& c : v)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return v;
}
bool lower_ascii_eq(const std::string& a, const char* b) { return lower_ascii_copy(a) == b; }

// What the shell would have substituted, noted by the splitters for the import warnings. A
// variable becomes {{process.env.NAME}} (the model reads .env or the OS environment).
struct ShellNotes {
  // As written ("$TOKEN", "%USERNAME%", "$env:X", "$null") and what it was imported as.
  std::vector<std::pair<std::string, std::string>> vars;
  bool subst = false;             // $(...) or `...`: a command the shell would run
};
thread_local ShellNotes* t_notes = nullptr;

bool var_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool var_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

void note_var(const std::string& as_written, const std::string& imported) {
  if (t_notes) t_notes->vars.emplace_back(as_written, imported);
}

std::string env_ref(const std::string& name) { return "{{process.env." + name + "}}"; }

// bash: $NAME or ${NAME} at s[i] (== '$'). Appends {{process.env.NAME}} to cur and returns the
// index of the reference's last character, or npos when it is not a plain variable (then the
// '$' is literal; $(...) is noted as a command substitution).
size_t bash_var(const std::string& s, size_t i, std::string& cur) {
  size_t j = i + 1;
  const bool brace = j < s.size() && s[j] == '{';
  if (brace) ++j;
  if (j >= s.size() || !var_start(s[j])) {
    if (!brace && j < s.size() && s[j] == '(' && t_notes) t_notes->subst = true;
    return std::string::npos;
  }
  size_t k = j;
  while (k < s.size() && var_char(s[k])) ++k;
  if (brace && (k >= s.size() || s[k] != '}')) return std::string::npos;  // ${x:-y}: kept as is
  const std::string name = s.substr(j, k - j);
  cur += env_ref(name);
  note_var("$" + name, env_ref(name));
  return brace ? k : k - 1;
}

// PowerShell: $env:NAME, ${env:NAME}, $null, $true, $false or $name at s[i] (== '$') inside "..."
// or in a bare word. Same contract as bash_var. A plain $name is a PowerShell variable, not an
// environment variable: it becomes {{process.env.name}} so it has a place to be filled in.
size_t ps_var(const std::string& s, size_t i, std::string& cur) {
  size_t j = i + 1;
  const bool brace = j < s.size() && s[j] == '{';
  if (brace) ++j;
  bool env = false;
  if (s.size() >= j + 4 && lower_ascii_eq(s.substr(j, 4), "env:")) {
    env = true;
    j += 4;
  }
  if (j >= s.size() || !var_start(s[j])) {
    if (!brace && !env && j < s.size() && s[j] == '(' && t_notes) t_notes->subst = true;
    return std::string::npos;
  }
  size_t k = j;
  while (k < s.size() && var_char(s[k])) ++k;
  if (brace && (k >= s.size() || s[k] != '}')) return std::string::npos;
  const std::string name = s.substr(j, k - j);
  if (!env && !brace && name == "wattBody") return std::string::npos;  // our own export's temp file (kPsBodyVar)
  const std::string lname = lower_ascii_copy(name);
  if (!env && lname == "null") {
    note_var("$null", "nothing");
  } else if (!env && (lname == "true" || lname == "false")) {
    cur += lname == "true" ? "True" : "False";
    note_var("$" + name, lname == "true" ? "True" : "False");
  } else {
    cur += env_ref(name);
    note_var(env ? "$env:" + name : "$" + name, env_ref(name));
  }
  return brace ? k : k - 1;
}

// POSIX shell words. Returns false on an unterminated quote.
bool split_bash(const std::string& s, std::vector<std::string>& out, std::string* err) {
  std::string cur;
  bool in_word = false;
  const size_t n = s.size();
  auto flush = [&] {
    if (in_word) out.push_back(cur);
    cur.clear();
    in_word = false;
  };
  for (size_t i = 0; i < n; ++i) {
    const char c = s[i];
    if (is_space(c)) {
      flush();
    } else if (c == '\\') {
      if (i + 1 < n && s[i + 1] == '\n') {
        ++i;  // line continuation
      } else if (i + 2 < n && s[i + 1] == '\r' && s[i + 2] == '\n') {
        i += 2;
      } else if (i + 1 < n) {
        cur += s[++i];
        in_word = true;
      }
    } else if (c == '\'') {
      const size_t close = s.find('\'', i + 1);
      if (close == std::string::npos) {
        if (err) *err = "unterminated ' quote";
        return false;
      }
      cur.append(s, i + 1, close - i - 1);
      in_word = true;
      i = close;
    } else if (c == '$' && i + 1 < n && s[i + 1] == '\'') {  // ANSI-C quoting $'...'
      in_word = true;
      size_t j = i + 2;
      bool closed = false;
      for (; j < n; ++j) {
        const char d = s[j];
        if (d == '\'') {
          closed = true;
          break;
        }
        if (d != '\\' || j + 1 >= n) {
          cur += d;
          continue;
        }
        const char e = s[++j];
        switch (e) {
          case 'n': cur += '\n'; break;
          case 't': cur += '\t'; break;
          case 'r': cur += '\r'; break;
          case 'a': cur += '\a'; break;
          case 'b': cur += '\b'; break;
          case 'f': cur += '\f'; break;
          case 'v': cur += '\v'; break;
          case 'e': case 'E': cur += '\x1b'; break;
          case '\\': cur += '\\'; break;
          case '\'': cur += '\''; break;
          case '"': cur += '"'; break;
          case '?': cur += '?'; break;
          case 'x': {
            uint32_t v = 0;
            int k = 0;
            while (k < 2 && j + 1 < n && hex_val(s[j + 1]) >= 0) v = v * 16 + static_cast<uint32_t>(hex_val(s[++j])), ++k;
            if (k == 0) cur += "\\x";
            else cur += static_cast<char>(v);
            break;
          }
          case 'u': case 'U': {
            const int maxd = e == 'u' ? 4 : 8;
            uint32_t v = 0;
            int k = 0;
            while (k < maxd && j + 1 < n && hex_val(s[j + 1]) >= 0) v = v * 16 + static_cast<uint32_t>(hex_val(s[++j])), ++k;
            if (k == 0) {
              cur += '\\';
              cur += e;
            } else {
              append_utf8(cur, v);
            }
            break;
          }
          default:
            if (e >= '0' && e <= '7') {
              uint32_t v = static_cast<uint32_t>(e - '0');
              int k = 1;
              while (k < 3 && j + 1 < n && s[j + 1] >= '0' && s[j + 1] <= '7') v = v * 8 + static_cast<uint32_t>(s[++j] - '0'), ++k;
              cur += static_cast<char>(v);
            } else {
              cur += '\\';
              cur += e;
            }
        }
      }
      if (!closed) {
        if (err) *err = "unterminated $' quote";
        return false;
      }
      i = j;
    } else if (c == '"') {
      in_word = true;
      size_t j = i + 1;
      bool closed = false;
      for (; j < n; ++j) {
        const char d = s[j];
        if (d == '"') {
          closed = true;
          break;
        }
        if (d == '\\' && j + 1 < n) {
          const char e = s[j + 1];
          if (e == '"' || e == '\\' || e == '$' || e == '`') {
            cur += e;
            ++j;
            continue;
          }
          if (e == '\n') {
            ++j;
            continue;
          }
          if (e == '\r' && j + 2 < n && s[j + 2] == '\n') {
            j += 2;
            continue;
          }
        }
        if (d == '$') {
          const size_t end = bash_var(s, j, cur);
          if (end != std::string::npos) {
            j = end;
            continue;
          }
        }
        if (d == '`' && t_notes) t_notes->subst = true;
        cur += d;
      }
      if (!closed) {
        if (err) *err = "unterminated \" quote";
        return false;
      }
      i = j;
    } else if (c == '#' && !in_word) {  // comment to end of line
      while (i + 1 < n && s[i + 1] != '\n') ++i;
    } else if (size_t end; c == '$' && (end = bash_var(s, i, cur)) != std::string::npos) {
      i = end;
      in_word = true;
    } else {
      if (c == '`' && t_notes) t_notes->subst = true;
      cur += c;
      in_word = true;
    }
  }
  flush();
  return true;
}

bool env_defined(const std::string& name) {
#ifdef _MSC_VER
  size_t len = 0;
  return getenv_s(&len, nullptr, 0, name.c_str()) == 0 && len > 0;
#else
  return std::getenv(name.c_str()) != nullptr;
#endif
}

// cmd.exe pass: ^ escapes the next char (outside cmd quotes); ^<newline> joins lines and
// takes the first character of the next line literally. Quotes are kept for the CRT pass.
std::string cmd_unescape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  bool quoted = false;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '"') {
      quoted = !quoted;
      out += c;
    } else if (c == '^' && !quoted && i + 1 < s.size()) {
      size_t j = i + 1;
      if (s[j] == '\r' && j + 1 < s.size() && s[j + 1] == '\n') ++j;
      if (s[j] == '\n') {
        if (j + 1 < s.size()) out += s[j + 1];  // the escaped char after the newline
        i = j + 1;
      } else {
        out += s[j];
        i = j;
      }
    } else if (c == '%') {
      // %NAME%: cmd.exe substitutes a defined variable (inside quotes too); an undefined one
      // stays as typed at the prompt.
      size_t close = s.find('%', i + 1);
      std::string name = close == std::string::npos ? std::string() : s.substr(i + 1, close - i - 1);
      bool var = !name.empty() && var_start(name[0]);
      for (char ch : name) var = var && var_char(ch);
      if (var && env_defined(name)) {
        out += env_ref(name);
        note_var("%" + name + "%", env_ref(name));
        i = close;
      } else {
        out += c;
      }
    } else if (c == '\n') {
      // An unescaped line break ends the command (leading blank lines are skipped).
      if (out.find_first_not_of(" \t") != std::string::npos) break;
      out.clear();
    } else {
      out += c;
    }
  }
  return out;
}

// MS CRT argv rules, exactly as curl.exe gets its argv: backslashes are literal unless they come
// before a '"'; 2n backslashes + '"' give n backslashes and toggle quoting, 2n+1 give n and a
// literal '"'. collapse_doubled: also turn each pair of backslashes in a run that is not before a
// quote into one (what a browser's "Copy as cURL (cmd)" meant: it doubles every backslash, but the CRT
// only halves them before a quote). A lone backslash is never dropped. *doubled counts the
// backslashes curl.exe gets beyond what was meant, in either mode.
bool split_crt(const std::string& s, bool collapse_doubled, std::vector<std::string>& out, std::string* err,
               size_t* doubled = nullptr) {
  std::string cur;
  bool in_word = false, quoted = false;
  const size_t n = s.size();
  for (size_t i = 0; i < n; ++i) {
    const char c = s[i];
    if (!quoted && (c == ' ' || c == '\t')) {  // the CRT splits on these only; LF is literal
      if (in_word) out.push_back(cur);
      cur.clear();
      in_word = false;
    } else if (c == '\\') {
      in_word = true;
      size_t k = i;
      while (k < n && s[k] == '\\') ++k;
      const size_t count = k - i;
      if (k < n && s[k] == '"') {
        cur.append(count / 2, '\\');
        if (count % 2) {
          cur += '"';
          i = k;
        } else {
          i = k - 1;  // the quote toggles on the next iteration
        }
      } else {
        const size_t meant = (count + 1) / 2;
        if (doubled) *doubled += count - meant;
        cur.append(collapse_doubled ? meant : count, '\\');
        i = k - 1;
      }
    } else if (c == '"') {
      in_word = true;
      if (quoted && i + 1 < n && s[i + 1] == '"') {
        cur += '"';
        ++i;
      } else {
        quoted = !quoted;
      }
    } else {
      cur += c;
      in_word = true;
    }
  }
  if (quoted) {
    if (err) *err = "unterminated \" quote";
    return false;
  }
  if (in_word) out.push_back(cur);
  return true;
}

// PowerShell words: '...' ('' is a quote), "..." (`x escapes, "" is a quote), ` escapes the next
// character outside quotes and `<newline> continues the line. Variables in "..." and bare words
// become {{process.env.NAME}} (see ps_var).
bool split_ps(const std::string& s, std::vector<std::string>& out, std::string* err) {
  std::string cur;
  bool in_word = false;
  const size_t n = s.size();
  auto flush = [&] {
    if (in_word) out.push_back(cur);
    cur.clear();
    in_word = false;
  };
  for (size_t i = 0; i < n; ++i) {
    const char c = s[i];
    if (is_space(c)) {
      flush();
    } else if (c == '`') {
      if (i + 1 < n && s[i + 1] == '\r') ++i;
      if (i + 1 < n && s[i + 1] == '\n') {
        ++i;
      } else if (i + 1 < n) {
        cur += s[++i];
        in_word = true;
      }
    } else if (c == '\'') {
      in_word = true;
      size_t j = i + 1;
      // U+2018..U+201B count as ' too (doubled to escape).
      auto typo_sq = [&](size_t k) {
        return k + 2 < n && static_cast<unsigned char>(s[k]) == 0xE2 && static_cast<unsigned char>(s[k + 1]) == 0x80 &&
               static_cast<unsigned char>(s[k + 2]) >= 0x98 && static_cast<unsigned char>(s[k + 2]) <= 0x9B;
      };
      for (;; ++j) {
        if (j >= n) {
          if (err) *err = "unterminated ' quote";
          return false;
        }
        if (typo_sq(j)) {
          if (!typo_sq(j + 3)) {
            j += 2;
            break;
          }
          cur.append(s, j, 3);
          j += 5;
        } else if (s[j] != '\'') {
          cur += s[j];
        } else if (j + 1 < n && s[j + 1] == '\'') {
          cur += '\'';
          ++j;
        } else {
          break;
        }
      }
      i = j;
    } else if (c == '"') {
      in_word = true;
      size_t j = i + 1;
      for (;; ++j) {
        if (j >= n) {
          if (err) *err = "unterminated \" quote";
          return false;
        }
        const char d = s[j];
        if (d == '`' && j + 1 < n) {
          const char e = s[++j];
          cur += e == 'n' ? '\n' : e == 'r' ? '\r' : e == 't' ? '\t' : e == '0' ? '\0' : e;
        } else if (d == '$' && s.compare(j, 8, "$([char]") == 0) {  // $([char]27)
          size_t k = j + 8;
          uint32_t v = 0;
          while (k < n && k < j + 15 && s[k] >= '0' && s[k] <= '9') v = v * 10 + static_cast<uint32_t>(s[k++] - '0');
          if (k < n && s[k] == ')' && k > j + 8 && v <= 0x10FFFF) {
            append_utf8(cur, v);
            j = k;
          } else {
            cur += d;
          }
        } else if (d == '"' && j + 1 < n && s[j + 1] == '"') {
          cur += '"';
          ++j;
        } else if (d == '"') {
          break;
        } else if (size_t end; d == '$' && (end = ps_var(s, j, cur)) != std::string::npos) {
          j = end;
        } else {
          cur += d;
        }
      }
      i = j;
    } else if (c == '#' && !in_word) {
      while (i + 1 < n && s[i + 1] != '\n') ++i;
    } else if (size_t end; c == '$' && (end = ps_var(s, i, cur)) != std::string::npos) {
      i = end;
      in_word = true;
    } else {
      cur += c;
      in_word = true;
    }
  }
  flush();
  return true;
}

// The PowerShell export of a body with NUL bytes writes it to this temp file first.
constexpr const char* kPsBodyVar = "$wattBody";

bool looks_like_powershell(const std::string& s) {
  if (s.find("$PSNativeCommandArgumentPassing") != std::string::npos) return true;
  if (s.rfind(kPsBodyVar, 0) == 0) return true;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\n') continue;
    size_t j = i;
    if (j > 0 && s[j - 1] == '\r') --j;
    while (j > 0 && (s[j - 1] == ' ' || s[j - 1] == '\t')) --j;
    if (j > 0 && s[j - 1] == '`') return true;
  }
  return false;
}

bool looks_like_cmd(const std::string& s) {
  bool bash_cont = false, caret_cont = false;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\n') continue;
    size_t j = i;
    if (j > 0 && s[j - 1] == '\r') --j;
    while (j > 0 && (s[j - 1] == ' ' || s[j - 1] == '\t')) --j;
    if (j > 0 && s[j - 1] == '\\') bash_cont = true;
    if (j > 0 && s[j - 1] == '^') caret_cont = true;
  }
  if (bash_cont) return false;
  // Browsers quote every cmd argument as ^"...^", always preceded by a space.
  if (caret_cont || s.find(" ^\"") != std::string::npos) return true;
  // A caret outside bash quotes in front of & | < > ( ) ^ or ": cmd's escape. In bash an
  // unquoted & | < > ( ) would end the command or be a syntax error, so this is not bash
  // (curl https://h.test/a?a=1^&b=2).
  char q = 0;  // the open bash quote: ' or " (0: none)
  bool ansi = false;  // $'...': backslash escapes inside
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (q) {
      if (c == '\\' && (q == '"' || ansi) && i + 1 < s.size()) ++i;
      else if (c == q) q = 0;
    } else if (c == '\\') {
      ++i;
    } else if (c == '\'' || c == '"') {
      q = c;
      ansi = c == '\'' && i > 0 && s[i - 1] == '$';
    } else if (c == '^' && i + 1 < s.size() && std::string_view("&|<>()^\"").find(s[i + 1]) != std::string_view::npos) {
      return true;
    }
  }
  return false;
}

// ------------------------------------------------------------------ helpers

std::string lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

using detail::base64;

// What curl does for --data-urlencode and --url-query: unreserved bytes kept, space as '+',
// everything else %XX.
std::string url_encode(const std::string& s) {
  std::string out;
  out.reserve(s.size() + s.size() / 2);
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
        c == '_' || c == '~') {
      out += static_cast<char>(c);
    } else if (c == ' ') {
      out += '+';
    } else {
      char b[4];
      std::snprintf(b, sizeof b, "%%%02X", c);
      out += b;
    }
  }
  return out;
}

// Data piped into curl (`printf %b '...' | curl ... --data-binary @-`), while parsing.
thread_local const std::string* t_stdin = nullptr;

// --data-urlencode content | =content | name=content | @file | name@file: where the '@' of a
// file reference is (npos for content).
size_t urlencode_at(const std::string& v) {
  const size_t eq = v.find('=');
  const size_t at = v.find('@');
  if (at == std::string::npos || (eq != std::string::npos && eq < at)) return std::string::npos;
  return at;
}

// What an import reads for "@-": only data the command itself pipes in (`printf ... | curl`).
bool read_stdin(std::string& out, std::vector<std::string>& warnings) {
  if (t_stdin) {
    out = *t_stdin;
    return true;
  }
  warnings.push_back("data from stdin (not available here): curl sends nothing for it");
  out.clear();
  return true;
}

// Cookies of a Netscape cookie file (curl -b file, written by curl -c or a browser export)
// that curl sends to `url`: domain (tail match with a leading '.' or the subdomain flag), path
// prefix, secure only over https, not expired. Lines: domain, subdomains, path, secure,
// expires, name, value (tab-separated); "#HttpOnly_" before the domain is part of the format.
std::string jar_cookies(const std::string& text, const std::string& url) {
  const size_t se = url.find("://");
  const std::string scheme = se == std::string::npos ? "http" : lower_ascii_copy(url.substr(0, se));
  const size_t hs = se == std::string::npos ? 0 : se + 3;
  size_t he = url.find_first_of("/?#", hs);
  if (he == std::string::npos) he = url.size();
  std::string host = url.substr(hs, he - hs);
  if (const size_t at = host.rfind('@'); at != std::string::npos) host = host.substr(at + 1);
  if (!host.empty() && host[0] == '[') host = host.substr(0, host.find(']') + 1);
  else if (const size_t colon = host.find(':'); colon != std::string::npos) host.resize(colon);
  host = lower_ascii_copy(host);
  std::string path = he < url.size() && url[he] == '/' ? url.substr(he, url.find_first_of("?#", he) - he) : "/";
  const long long now = static_cast<long long>(std::time(nullptr));
  std::string out;
  for (size_t p = 0; p < text.size();) {
    size_t e = text.find('\n', p);
    if (e == std::string::npos) e = text.size();
    std::string line = text.substr(p, e - p);
    p = e + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("#HttpOnly_", 0) == 0) line.erase(0, 10);
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> f;
    for (size_t a = 0;;) {
      const size_t t = line.find('\t', a);
      f.push_back(line.substr(a, t == std::string::npos ? std::string::npos : t - a));
      if (t == std::string::npos) break;
      a = t + 1;
    }
    if (f.size() == 6) f.emplace_back();  // empty value
    if (f.size() != 7) continue;
    std::string dom = lower_ascii_copy(f[0]);
    const bool tail = (!dom.empty() && dom[0] == '.') || f[1] == "TRUE";
    if (!dom.empty() && dom[0] == '.') dom.erase(0, 1);
    const bool dom_ok = host == dom || (tail && host.size() > dom.size() &&
                                        host.compare(host.size() - dom.size(), dom.size(), dom) == 0 &&
                                        host[host.size() - dom.size() - 1] == '.');
    const std::string& cp = f[2].empty() ? std::string("/") : f[2];
    const bool path_ok = path.compare(0, cp.size(), cp) == 0 &&
                         (path.size() == cp.size() || cp.back() == '/' || path[cp.size()] == '/');
    const bool secure_ok = f[3] != "TRUE" || scheme == "https";
    long long exp = 0;
    try {
      exp = std::stoll(f[4]);
    } catch (...) {
    }
    if (!dom_ok || !path_ok || !secure_ok || (exp != 0 && exp < now)) continue;
    out += (out.empty() ? "" : "; ") + f[5] + "=" + f[6];
  }
  return out;
}

// A URL glob curl would expand into several requests without -g: {a,b} or [1-3] / [a-z]
// (escaped \{ \[ do not count). Returns the glob, or "".
std::string url_glob(const std::string& url) {
  for (size_t i = 0; i < url.size(); ++i) {
    if (url[i] == '\\') {
      ++i;
      continue;
    }
    if (url[i] == '{') {
      const size_t close = url.find('}', i);
      if (close != std::string::npos && url.find(',', i) < close) return url.substr(i, close - i + 1);
    } else if (url[i] == '[') {
      const size_t close = url.find(']', i);
      if (close == std::string::npos) continue;
      const std::string in = url.substr(i + 1, close - i - 1);
      const size_t dash = in.find('-');
      if (dash != std::string::npos && dash > 0 && dash + 1 < in.size() &&
          in.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-:") == std::string::npos)
        return url.substr(i, close - i + 1);
    }
  }
  return {};
}

bool is_remove_marker(const std::string& v) {
  return !v.empty() && v.find_first_not_of(" \t") == std::string::npos;
}

std::string trim(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t");
  if (b == std::string::npos) return {};
  return s.substr(b, s.find_last_not_of(" \t") - b + 1);
}

bool has_header(const std::vector<Header>& h, const std::string& name) {
  const std::string l = lower(name);
  for (const auto& [k, v] : h)
    if (lower(k) == l) return true;
  return false;
}

void add_header_line(std::vector<Header>& headers, const std::string& line, std::vector<std::string>& warnings) {
  const size_t colon = line.find(':');
  if (colon == std::string::npos) {
    // "Name;" is curl's syntax for a header with an empty value. Without a ';' or a ':' curl
    // sends nothing for it.
    std::string name = line;
    while (!name.empty() && is_space(name.back())) name.pop_back();
    if (name.empty() || name.back() != ';') {
      if (!name.empty())
        warnings.push_back("header '" + name + "' has no ':' (or ';' for an empty value): curl does not send it");
      return;
    }
    name.pop_back();
    while (!name.empty() && is_space(name.back())) name.pop_back();
    if (!name.empty()) headers.emplace_back(name, "");
    return;
  }
  std::string name = line.substr(0, colon);
  while (!name.empty() && is_space(name.back())) name.pop_back();
  size_t vs = colon + 1;
  while (vs < line.size() && (line[vs] == ' ' || line[vs] == '\t')) ++vs;
  std::string value = line.substr(vs);
  while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();
  // "Name:" with nothing after the colon removes a header curl would add by default.
  if (value.find_first_not_of(" \t") == std::string::npos) value = kRemoveHeader;
  if (!name.empty()) headers.emplace_back(std::move(name), std::move(value));
}

// The lines of a curl -H @file (LF or CRLF, blank lines skipped).
void add_header_lines(std::vector<Header>& headers, const std::string& content, std::vector<std::string>& warnings) {
  size_t p = 0;
  while (p < content.size()) {
    size_t e = content.find('\n', p);
    if (e == std::string::npos) e = content.size();
    const std::string line = content.substr(p, e - p);
    if (!line.empty() && line != "\r") add_header_line(headers, line, warnings);
    p = e + 1;
  }
}

// One word of a -F value, as curl's tool reads it: "..." (with \" and \ escapes) when the
// closing quote is followed by ';' or the end, otherwise everything up to the next ';' with
// trailing blanks dropped. `pos` ends on the ';' (or the end).
std::string form_word_at(const std::string& s, size_t& pos) {
  if (pos < s.size() && s[pos] == '"') {
    std::string w;
    size_t j = pos + 1;
    for (; j < s.size(); ++j) {
      if (s[j] == '\\' && j + 1 < s.size() && (s[j + 1] == '"' || s[j + 1] == '\\')) {
        w += s[++j];
        continue;
      }
      if (s[j] == '"') break;
      w += s[j];
    }
    if (j < s.size()) {
      size_t k = j + 1;
      while (k < s.size() && is_space(s[k])) ++k;
      if (k == s.size() || s[k] == ';') {
        pos = k;
        return w;
      }
    }
  }
  const size_t end = s.find(';', pos);
  std::string w = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
  while (!w.empty() && is_space(w.back())) w.pop_back();
  pos = end == std::string::npos ? s.size() : end;
  return w;
}

// -F name=value | name=@file | name=<file, each with optional ;type= ;filename= ;headers=.
// --form-string name=value takes the value verbatim.
FormPart parse_form(const std::string& arg, bool literal, std::vector<std::string>& warnings) {
  FormPart p;
  const size_t eq = arg.find('=');
  p.name = arg.substr(0, eq);
  if (eq == std::string::npos) {
    warnings.push_back("form part '" + arg + "' has no '=': curl refuses it");
    return p;
  }
  const std::string v = arg.substr(eq + 1);
  if (literal) {
    p.value = v;
    p.literal = true;
    return p;
  }
  size_t pos = 0;
  if (!v.empty() && (v[0] == '@' || v[0] == '<')) {
    p.kind = v[0] == '@' ? FormPart::Kind::File : FormPart::Kind::FileContent;
    pos = 1;
  }
  p.value = form_word_at(v, pos);
  while (pos < v.size() && v[pos] == ';') {
    ++pos;
    while (pos < v.size() && is_space(v[pos])) ++pos;
    if (v.compare(pos, 5, "type=") == 0) {
      pos += 5;
      p.type = form_word_at(v, pos);
    } else if (v.compare(pos, 9, "filename=") == 0) {
      pos += 9;
      p.filename = form_word_at(v, pos);
    } else if (v.compare(pos, 8, "headers=") == 0) {
      pos += 8;
      const std::string h = form_word_at(v, pos);
      if (lower(h).rfind("content-type:", 0) == 0) {
        const size_t b = h.find_first_not_of(" \t", 13);
        p.type = b == std::string::npos ? "" : h.substr(b);
      } else if (!h.empty()) {
        warnings.push_back("form part '" + p.name + "': part header '" + h + "' is not kept");
      }
    } else {
      const std::string unknown = form_word_at(v, pos);
      if (!unknown.empty())
        warnings.push_back("form part '" + p.name + "': curl drops ';" + unknown + "' (use --form-string to send a ';')");
    }
  }
  if (p.kind != FormPart::Kind::Text) {
    std::error_code ec;
    if (p.value == "-") warnings.push_back("form part '" + p.name + "' reads stdin: not available, pick a file");
    else if (!std::filesystem::is_regular_file(detail::from_utf8(p.value), ec))
      warnings.push_back("form file not found: " + p.value);
  }
  return p;
}

// Options that take an argument and are ignored (no meaning for an API tester or not supported).
bool ignored_with_arg(std::string_view o) {
  static const std::string_view list[] = {
      "--output", "--write-out", "--connect-timeout", "--retry", "--retry-delay", "--retry-max-time",
      "--proxy", "--proxy-user", "--cacert", "--capath", "--cert", "--key", "--cert-type", "--key-type",
      "--pass", "--resolve", "--connect-to", "--interface", "--limit-rate", "--max-redirs", "--dump-header",
      "--config", "--cookie-jar", "--unix-socket", "--abstract-unix-socket", "--ciphers", "--tls-max",
      "--expect100-timeout", "--keepalive-time", "--local-port", "--max-filesize", "--noproxy",
      "--proxy-header", "--socks4", "--socks4a", "--socks5", "--socks5-hostname", "--stderr", "--trace",
      "--trace-ascii", "--aws-sigv4", "--hostpubmd5", "--krb", "--output-dir", "--create-file-mode",
      "--dns-servers", "--doh-url", "--happy-eyeballs-timeout-ms", "--pinnedpubkey", "--proto",
      "--proto-redir", "--speed-limit", "--speed-time", "--time-cond", "--trace-config", "--variable",
      "--engine", "--egd-file", "--random-file", "--crlfile", "--netrc-file", "--parallel-max",
      "--sasl-authzid", "--service-name", "--login-options", "--mail-from", "--mail-rcpt", "--mail-auth",
      "--ftp-port", "--quote", "--telnet-option", "--etag-save", "--etag-compare", "--proxy-cacert",
      "--proxy-cert", "--proxy-key", "--preproxy", "--ipfs-gateway", "--ech", "--curves"};
  for (auto s : list)
    if (s == o) return true;
  return false;
}

// Short options that take an argument.
bool short_takes_arg(char c) { return std::string_view("XHdAebcoworxTFDEmuKUtPQyYzCr").find(c) != std::string_view::npos; }

std::string long_of(char c) {
  switch (c) {
    case 'X': return "--request";
    case 'H': return "--header";
    case 'd': return "--data";
    case 'A': return "--user-agent";
    case 'e': return "--referer";
    case 'b': return "--cookie";
    case 'u': return "--user";
    case 'T': return "--upload-file";
    case 'x': return "--proxy";
    case 'U': return "--proxy-user";
    case 'E': return "--cert";
    case 'F': return "--form";
    case 'm': return "--max-time";
    case 'r': return "--range";
    case 'k': return "--insecure";
    case 'L': return "--location";
    case 'I': return "--head";
    case 'G': return "--get";
    case 'g': return "--globoff";
    case '0': return "--http1.0";
    default: return std::string("-") + c;  // other short flags: ignored
  }
}

bool needs_ansi_c(const std::string& s) {
  for (unsigned char c : s)
    if ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7F) return true;
  return false;
}

// bash words. '...' keeps everything but ' verbatim, LF and TAB included, so multi-line bodies
// stay readable. Any other control character (CR, ESC, ...) switches to $'...' like Chrome's
// "Copy as cURL (bash)": Git Bash drops a raw CR inside '...'.
std::string sh_quote(const std::string& s) {
  if (!s.empty() && s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_./:@%+=,") ==
                        std::string::npos)
    return s;
  if (needs_ansi_c(s)) {
    std::string out = "$'";
    for (unsigned char c : s) {
      switch (c) {
        case '\\': out += "\\\\"; break;
        case '\'': out += "\\'"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (c < 0x20 || c == 0x7F) {
            char b[8];
            std::snprintf(b, sizeof b, "\\x%02X", c);
            out += b;
          } else {
            out += static_cast<char>(c);
          }
      }
    }
    out += '\'';
    return out;
  }
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  out += '\'';
  return out;
}

bool plain_word(const std::string& s) {
  return !s.empty() &&
         s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_./:@+=,") ==
             std::string::npos;
}

// MS CRT argv quoting (what curl.exe's startup code undoes), with "" for a literal quote inside
// the quoted string instead of \". Every " then comes in a pair, so cmd.exe's and PowerShell
// 5.1's naive quote tracking never disagrees with the CRT's. Backslashes are doubled only where
// the CRT halves them: before a quote and before the closing quote.
// `pct` / `lf` are what a % and a line feed become (cmd needs both special-cased).
std::string crt_quote(const std::string& s, const char* pct, const char* lf) {
  std::string out = "\"";
  for (size_t i = 0; i < s.size();) {
    const char c = s[i];
    if (c == '\\') {
      size_t k = i;
      while (k < s.size() && s[k] == '\\') ++k;
      const size_t n = k - i;
      out.append(k == s.size() || s[k] == '"' ? 2 * n : n, '\\');
      i = k;
      continue;
    }
    if (c == '"') out += "\"\"";
    else if (c == '%' && pct) out += pct;
    else if (c == '\n' && lf) out += lf;
    else if (c == '\r' && lf) {  // cmd.exe cannot carry a CR in an argument
    } else out += c;
    ++i;
  }
  out += '"';
  return out;
}

// cmd.exe: inside "..." only % (variable expansion) is still special. "%" closes the quote
// around it, so the would-be variable name contains a quote and never matches. A line feed is
// ^<LF><LF> outside the quotes (the caret takes the second LF literally), as browsers emit.
std::string cmd_quote(const std::string& s) {
  if (plain_word(s)) return s;
  return crt_quote(s, "\"%\"", "\"^\n\n\"");
}

// PowerShell: a '...' literal ($ and ` are not special, ' doubles). Windows PowerShell 5.1 does
// not quote arguments for native programs correctly, so a value with a " (or an empty value, or
// a trailing backslash after whitespace) is passed pre-quoted for the CRT; `legacy` tells the
// caller to make PowerShell 7.3+ pass it the same way.
// A value with control characters (LF included: a multi-line '...' literal would pick up the
// script file's CRLF) becomes a "..." string with `n, `r, `t and $([char]N) escapes instead.
// PowerShell also treats the typographic quotes U+2018-U+201B / U+201C-U+201E as quotes.
std::string ps_quote(const std::string& s, bool& legacy) {
  if (plain_word(s)) return s;
  std::string v = s;
  const bool ws = s.find_first_of(" \t\n") != std::string::npos;
  if (s.empty() || s.find('"') != std::string::npos || (ws && s.back() == '\\')) {
    v = crt_quote(s, nullptr, nullptr);
    legacy = true;
  }
  bool ctl = false;
  for (unsigned char c : v)
    if (c < 0x20 || c == 0x7F) ctl = true;
  std::string out = ctl ? "\"" : "'";
  for (size_t i = 0; i < v.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(v[i]);
    const bool typo = c == 0xE2 && i + 2 < v.size() && static_cast<unsigned char>(v[i + 1]) == 0x80 &&
                      static_cast<unsigned char>(v[i + 2]) >= 0x98 && static_cast<unsigned char>(v[i + 2]) <= 0x9E;
    if (!ctl) {
      if (c == '\'') {
        out += "''";
      } else if (typo && static_cast<unsigned char>(v[i + 2]) <= 0x9B) {
        out += v.substr(i, 3) + v.substr(i, 3);  // doubled, like '
        i += 2;
      } else {
        out += static_cast<char>(c);
      }
      continue;
    }
    switch (c) {
      case '\n': out += "`n"; break;
      case '\r': out += "`r"; break;
      case '\t': out += "`t"; break;
      case '`': out += "``"; break;
      case '$': out += "`$"; break;
      case '"': out += "`\""; break;
      default:
        if (c < 0x20 || c == 0x7F) {
          out += "$([char]" + std::to_string(c) + ")";
        } else {
          if (typo && static_cast<unsigned char>(v[i + 2]) >= 0x9C) out += '`';
          out += static_cast<char>(c);
        }
    }
  }
  out += ctl ? '"' : '\'';
  return out;
}

std::string seconds(long ms) {
  char b[32];
  std::snprintf(b, sizeof b, "%g", static_cast<double>(ms) / 1000.0);
  return b;
}

// An argument for bash's `printf %b`: single-quoted, backslash doubled, control bytes as
// \0ooo (always three digits, so a following digit is never taken into the escape).
std::string printf_b_quote(const std::string& s) {
  std::string out = "'";
  for (unsigned char c : s) {
    if (c == '\\') {
      out += "\\\\";
    } else if (c == '\'') {
      out += "'\\''";
    } else if ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7F) {
      char b[8];
      std::snprintf(b, sizeof b, "\\0%03o", c);
      out += b;
    } else {
      out += static_cast<char>(c);
    }
  }
  out += '\'';
  return out;
}

// Inverse of printf %b for the escapes it knows.
std::string printf_b_decode(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\\' || i + 1 >= s.size()) {
      out += s[i];
      continue;
    }
    const char e = s[++i];
    switch (e) {
      case '\\': out += '\\'; break;
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case 'r': out += '\r'; break;
      case 'a': out += '\a'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'v': out += '\v'; break;
      case '0': {
        unsigned v = 0;
        for (int k = 0; k < 3 && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7'; ++k)
          v = v * 8 + static_cast<unsigned>(s[++i] - '0');
        out += static_cast<char>(v);
        break;
      }
      default:
        out += '\\';
        out += e;
    }
  }
  return out;
}

}  // namespace

std::string curl_data_file(const CurlData& d) {
  switch (d.kind) {
    case CurlData::Kind::Raw: return {};
    case CurlData::Kind::UrlEncode: {
      const size_t at = urlencode_at(d.value);
      return at == std::string::npos ? std::string() : d.value.substr(at + 1);
    }
    default: return d.value.size() > 1 && d.value[0] == '@' ? d.value.substr(1) : std::string();
  }
}

bool build_curl_data_body(const std::vector<CurlData>& data,
                          const std::function<bool(const std::string& path, std::string& out)>& read,
                          std::string& body) {
  std::string json;
  bool has_json = false;
  std::vector<std::string> parts;
  for (const auto& d : data) {
    const std::string file = curl_data_file(d);
    std::string content;
    if (!file.empty() && !read(file, content)) return false;
    std::string piece;
    switch (d.kind) {
      case CurlData::Kind::Raw: piece = d.value; break;
      case CurlData::Kind::Data:
        // -d @file: curl strips CR and LF from the file. --data-binary / --json keep it as-is.
        piece = file.empty() ? d.value : content;
        if (!file.empty()) std::erase_if(piece, [](char c) { return c == '\r' || c == '\n'; });
        break;
      case CurlData::Kind::Binary:
      case CurlData::Kind::Json: piece = file.empty() ? d.value : content; break;
      case CurlData::Kind::UrlEncode: {
        // content | =content | name=content | @file | name@file
        if (const size_t at = urlencode_at(d.value); at != std::string::npos) {
          const std::string name = d.value.substr(0, at);
          piece = name.empty() ? url_encode(content) : name + "=" + url_encode(content);
        } else if (const size_t eq = d.value.find('='); eq != std::string::npos) {
          const std::string name = d.value.substr(0, eq);
          const std::string enc = url_encode(d.value.substr(eq + 1));
          piece = name.empty() ? enc : name + "=" + enc;
        } else {
          piece = url_encode(d.value);
        }
        break;
      }
    }
    if (d.kind == CurlData::Kind::Json) {
      json += piece;
      has_json = true;
    } else {
      parts.push_back(std::move(piece));
    }
  }
  body.clear();
  if (has_json) {
    body = json;
    if (!parts.empty()) body += parts.front();
  } else {
    for (size_t k = 0; k < parts.size(); ++k) body += (k ? "&" : "") + parts[k];
  }
  return true;
}

void add_curl_header_file(std::vector<Header>& headers, const std::string& content) {
  std::vector<Header> add;
  std::vector<std::string> ignored;
  add_header_lines(add, content, ignored);
  // As curl's custom headers replace the ones it would add (Content-Type of -d, ...).
  for (const auto& [k, v] : add) {
    const std::string l = lower(k);
    std::erase_if(headers, [&](const Header& h) { return lower(h.first) == l; });
  }
  for (auto& h : add) headers.push_back(std::move(h));
}

std::string curl_jar_cookies(const std::string& jar, const std::string& url) { return jar_cookies(jar, url); }

// ==================================================================== parse

std::optional<CurlImport> parse_curl(const std::string& cmd_in, std::string* err, CmdBackslashes bs) {
  std::string cmd = cmd_in;
  bool browser_cmd = false;
  size_t browser_doubled = 0;
  // Strip a UTF-8 BOM and a leading shell prompt ("$ curl ...", "> curl ...").
  if (cmd.rfind("\xEF\xBB\xBF", 0) == 0) cmd.erase(0, 3);
  std::vector<std::string> args;
  bool ok;
  ShellNotes notes;
  t_notes = &notes;
  struct NotesScope {
    ~NotesScope() { t_notes = nullptr; }
  } notes_scope;
  std::vector<std::string> ps51;  // PowerShell words Windows PowerShell 5.1 passes differently
  std::string ps_body;  // watt's PowerShell export of a binary body (see to_curl_command)
  bool has_ps_body = false;
  if (looks_like_powershell(cmd)) {
    const size_t bpos = cmd.find(kPsBodyVar);
    const size_t b64 = cmd.find("FromBase64String('");
    if (bpos != std::string::npos && b64 != std::string::npos && bpos < b64) {
      const size_t b64_end = cmd.find('\'', b64 + 18);
      const size_t eol = cmd.find('\n', b64);
      if (b64_end != std::string::npos && eol != std::string::npos && detail::base64_decode(cmd.substr(b64 + 18, b64_end - b64 - 18), ps_body)) {
        has_ps_body = true;
        cmd.erase(bpos, eol + 1 - bpos);
      }
    }
    // watt's own PowerShell export: a "$PSNativeCommandArgumentPassing = 'Legacy'" line means
    // the arguments are pre-quoted for the CRT.
    bool legacy = false;
    const size_t pos = cmd.find("$PSNativeCommandArgumentPassing");
    if (pos != std::string::npos) {
      legacy = true;
      const size_t eol = cmd.find_first_of(";\n", pos);
      cmd.erase(pos, eol == std::string::npos ? std::string::npos : eol + 1 - pos);
    }
    std::vector<std::string> words;
    ok = split_ps(cmd, words, err);
    for (const auto& w : words) {
      if (has_ps_body && w == std::string("@") + kPsBodyVar) {
        args.push_back("@-");
        continue;
      }
      if (!legacy && w.find('"') != std::string::npos) {
        // Windows PowerShell 5.1 does not quote a native argument that has a '"' in it (unless
        // it also has a blank outside "..."), so curl.exe gets it split by MS CRT rules: the
        // quotes go away and a blank inside splits it. \" is the usual 5.1 escape for a quote:
        // with it the meaning is the CRT reading, kept as one argument. Without it, it is
        // imported as PowerShell 7.3+ passes it.
        std::vector<std::string> parts;
        std::string e2;
        if (split_crt(w, false, parts, &e2)) {
          std::string joined;
          for (const auto& p : parts) joined += (joined.empty() ? "" : " ") + p;
          const bool escaped = w.find("\\\"") != std::string::npos;
          std::string got;
          for (const auto& p : parts) got += (got.empty() ? "'" : " '") + p + "'";
          if (escaped) {
            ps51.push_back("Windows PowerShell 5.1 passes " + std::string(parts.size() > 1 ? "this argument split at the blank" : "this argument") +
                           " to curl.exe as " + got + ": imported as one argument, " + joined +
                           " (PowerShell 7.3+ would pass the backslashes literally)");
            args.push_back(joined);
            continue;
          }
          if (parts.size() != 1 || parts[0] != w)
            ps51.push_back("Windows PowerShell 5.1 passes " + w + " to curl.exe as " + got +
                           " (quotes stripped): imported as PowerShell 7.3+ passes it");
        }
        args.push_back(w);
        continue;
      }
      if (!legacy) {
        args.push_back(w);
        continue;
      }
      std::vector<std::string> parts;
      if (!(ok = split_crt(w, false, parts, err))) break;
      std::string joined;
      for (const auto& p : parts) joined += (joined.empty() ? "" : " ") + p;
      args.push_back(joined);
    }
  } else if (looks_like_cmd(cmd)) {
    // cmd.exe reads lines: a CR before LF never reaches the program.
    std::erase(cmd, '\r');
    // Chrome/Firefox "Copy as cURL (cmd)" starts every value with ^" and doubles every
    // backslash, which curl.exe (CRT rules) only halves before a quote.
    browser_cmd = cmd.find(" ^\"") != std::string::npos;
    ok = split_crt(cmd_unescape(cmd), browser_cmd && bs == CmdBackslashes::AsBrowserMeant, args, err,
                   browser_cmd ? &browser_doubled : nullptr);
  } else {
    ok = split_bash(cmd, args, err);
  }
  if (!ok) return std::nullopt;
  t_notes = nullptr;
  size_t i = 0;
  while (i < args.size() && (args[i] == "$" || args[i] == ">" || args[i] == "%")) ++i;
  // watt's bash export of a body with NUL bytes: printf %b '...' | curl ...
  std::string piped;
  struct StdinScope {
    ~StdinScope() { t_stdin = nullptr; }
  } stdin_scope;
  if (has_ps_body) {
    piped = std::move(ps_body);
    t_stdin = &piped;
  } else if (i + 3 < args.size() && args[i] == "printf" && args[i + 1] == "%b" && args[i + 3] == "|") {
    piped = printf_b_decode(args[i + 2]);
    t_stdin = &piped;
    i += 4;
  }
  if (i >= args.size()) {
    if (err) *err = "empty command";
    return std::nullopt;
  }
  {
    std::string prog = lower(args[i]);
    const size_t slash = prog.find_last_of("/\\");
    if (slash != std::string::npos) prog = prog.substr(slash + 1);
    if (prog != "curl" && prog != "curl.exe") {
      if (err) *err = "not a curl command (starts with '" + args[i] + "')";
      return std::nullopt;
    }
  }
  ++i;

  CurlImport out;
  HttpRequest& r = out.http;
  std::vector<std::string>& warnings = out.warnings;
  out.browser_doubled_backslashes = browser_doubled;
  if (browser_doubled) {
    const std::string n = std::to_string(browser_doubled);
    warnings.push_back(bs == CmdBackslashes::AsBrowserMeant
                           ? "Chrome's cmd export doubles backslashes: imported what the browser sent, " + n +
                                 " backslash" + (browser_doubled == 1 ? "" : "es") + " fewer than curl.exe would send"
                           : "Chrome's cmd export doubles backslashes; curl.exe would send " + n + " more than the browser did");
  }
  r.follow_redirects = false;  // curl only follows with -L
  std::string method, url;
  // The -d family in order. No file is read here (see CurlImport): file references stay as
  // written, data piped into the command ("@-") is inlined.
  std::vector<CurlData> data;
  bool has_json = false, has_data = false, get = false, head = false;
  std::vector<FormPart>& form = out.form;
  std::string user, upload;  // upload: -T - (stdin) only
  bool has_upload = false, compressed = false, globoff = false;
  std::vector<std::string> url_query;
  std::vector<std::string> cookies;
  std::vector<std::string> cookie_files;
  std::string auth_scheme;  // --digest, --ntlm, ... (watt sends Basic only)
  bool agent_from_A = false;  // a User-Agent header added by -A (the last -A wins)
  auto stdin_reader = [&](const std::string&, std::string& content) { return read_stdin(content, warnings); };
  auto add_data = [&](CurlData::Kind kind, const std::string& v) {
    CurlData d{kind, v};
    if (curl_data_file(d) == "-") {  // piped in: inline it now (as a value that reads no file)
      std::string piece;
      build_curl_data_body({d}, stdin_reader, piece);
      d = CurlData{kind == CurlData::Kind::Json ? kind : CurlData::Kind::Raw, piece};
    }
    data.push_back(std::move(d));
  };
  auto deferred = [&](const std::string& what, const std::string& path) {
    warnings.push_back(what + " '" + path + "' is read when the request is sent, not now");
  };
  for (const auto& [as_written, imported] : notes.vars)
    warnings.push_back("shell variable " + as_written + ": the shell would expand it; imported as " + imported +
                       (imported.rfind("{{", 0) == 0 ? " (set it in .env)" : ""));
  if (notes.subst) warnings.push_back("the command has a $(...) or `...` command substitution: imported literally, not run");
  for (auto& w : ps51) warnings.push_back(std::move(w));

  auto need = [&](const std::string& opt, std::string& val) -> bool {
    if (i + 1 >= args.size()) {
      if (err) *err = "option " + opt + " needs a value";
      return false;
    }
    val = args[++i];
    return true;
  };

  // Applies one long-form option; `val` is filled lazily via need() or given (attached short).
  auto apply = [&](const std::string& opt, const std::string* attached) -> bool {
    std::string val;
    auto arg = [&]() -> bool {
      if (attached) {
        val = *attached;
        return true;
      }
      return need(opt, val);
    };
    if (opt == "--request") {
      if (!arg()) return false;
      method = val;
    } else if (opt == "--header") {
      if (!arg()) return false;
      if (val == "@-") {
        std::string content;
        read_stdin(content, warnings);
        add_header_lines(r.headers, content, warnings);
      } else if (!val.empty() && val[0] == '@') {
        out.header_files.push_back(val.substr(1));
        deferred("header file", val.substr(1));
      } else {
        add_header_line(r.headers, val, warnings);
      }
    } else if (opt == "--data" || opt == "--data-ascii") {
      if (!arg()) return false;
      add_data(CurlData::Kind::Data, val);
      has_data = true;
    } else if (opt == "--data-raw") {
      if (!arg()) return false;
      add_data(CurlData::Kind::Raw, val);
      has_data = true;
    } else if (opt == "--data-binary") {
      if (!arg()) return false;
      add_data(CurlData::Kind::Binary, val);
      has_data = true;
    } else if (opt == "--data-urlencode") {
      if (!arg()) return false;
      add_data(CurlData::Kind::UrlEncode, val);
      has_data = true;
    } else if (opt == "--json") {
      if (!arg()) return false;
      add_data(CurlData::Kind::Json, val);
      has_json = true;
    } else if (opt == "--form" || opt == "--form-string") {
      if (!arg()) return false;
      form.push_back(parse_form(val, opt == "--form-string", warnings));
    } else if (opt == "--user") {
      if (!arg()) return false;
      user = val;
    } else if (opt == "--user-agent") {
      if (!arg()) return false;
      // -A '' removes the User-Agent header, like -H 'User-Agent:'. A -H User-Agent wins.
      const std::string ua = val.empty() ? std::string(kRemoveHeader) : val;
      if (agent_from_A) {
        for (auto& [k, v] : r.headers)
          if (lower(k) == "user-agent") v = ua;
      } else if (!has_header(r.headers, "User-Agent")) {
        r.headers.emplace_back("User-Agent", ua);
        agent_from_A = true;
      }
    } else if (opt == "--referer") {
      if (!arg()) return false;
      // "url;auto" (or just ";auto"): curl sends url and also sets Referer on redirects.
      if (val.size() >= 5 && val.compare(val.size() - 5, 5, ";auto") == 0) {
        val.resize(val.size() - 5);
        warnings.push_back("-e ...;auto: curl also sets Referer on each redirect it follows; watt sends the "
                           "given Referer only" + std::string(val.empty() ? " (none here)" : ""));
      }
      if (!val.empty() && !has_header(r.headers, "Referer")) r.headers.emplace_back("Referer", val);
    } else if (opt == "--cookie") {
      if (!arg()) return false;
      if (val.find('=') != std::string::npos) cookies.push_back(val);
      else cookie_files.push_back(val);  // a Netscape cookie file, read once the URL is known
    } else if (opt == "--oauth2-bearer") {
      if (!arg()) return false;
      r.headers.emplace_back("Authorization", "Bearer " + val);
    } else if (opt == "--range") {
      if (!arg()) return false;
      r.headers.emplace_back("Range", "bytes=" + val);
    } else if (opt == "--url") {
      if (!arg()) return false;
      if (url.empty()) url = val;
    } else if (opt == "--url-query") {
      if (!arg()) return false;
      // Like --data-urlencode, except that a leading '+' means "already encoded". The query is
      // built now, so a file in it cannot be kept: refused (the file is not read).
      if (!val.empty() && val[0] == '+') {
        url_query.push_back(val.substr(1));
      } else {
        const CurlData d{CurlData::Kind::UrlEncode, val};
        const std::string f = curl_data_file(d);
        if (!f.empty() && f != "-") {
          warnings.push_back("--url-query file '" + f + "' is not imported: watt does not read files on import, "
                             "and the query cannot refer to one (add the value to the URL)");
        } else {
          std::string q;
          build_curl_data_body({d}, stdin_reader, q);
          url_query.push_back(std::move(q));
        }
      }
    } else if (opt == "--upload-file") {
      if (!arg()) return false;
      has_upload = true;
      out.body_file = val;
      upload.clear();
      if (val == "-") read_stdin(upload, warnings);
      else deferred("upload file", val);
    } else if (opt == "--max-time") {
      if (!arg()) return false;
      try {
        r.timeout_ms = static_cast<long>(std::stod(val) * 1000.0);
      } catch (...) {
      }
    } else if (opt == "--insecure") {
      r.verify_tls = false;
    } else if (opt == "--location" || opt == "--location-trusted") {
      r.follow_redirects = true;
    } else if (opt == "--head") {
      head = true;
    } else if (opt == "--get") {
      get = true;
    } else if (opt == "--http1.1" || opt == "--http1.0") {
      r.version = HttpVersion::Http1_1;
    } else if (opt == "--http2") {
      r.version = HttpVersion::Http2;
    } else if (opt == "--http2-prior-knowledge") {
      r.version = HttpVersion::Http2PriorKnowledge;
    } else if (opt == "--globoff") {
      globoff = true;
    } else if (opt == "--compressed") {
      compressed = true;
    } else if (opt == "--no-keepalive") {
      r.fresh_connection = true;
    } else if (opt == "--digest" || opt == "--ntlm" || opt == "--ntlm-wb" || opt == "--negotiate" || opt == "--anyauth") {
      auth_scheme = opt;
    } else if (opt == "--basic") {
      auth_scheme.clear();
    } else if (ignored_with_arg(opt)) {
      if (!arg()) return false;
      static const std::pair<std::string_view, const char*> noted[] = {
          {"--proxy", "the request goes direct, not through the proxy"},
          {"--proxy-user", "no proxy is used"},
          {"--socks5", "the request goes direct, not through the proxy"},
          {"--socks5-hostname", "the request goes direct, not through the proxy"},
          {"--socks4", "the request goes direct, not through the proxy"},
          {"--socks4a", "the request goes direct, not through the proxy"},
          {"--preproxy", "the request goes direct, not through the proxy"},
          {"--cacert", "TLS is verified against the system trust store"},
          {"--capath", "TLS is verified against the system trust store"},
          {"--cert", "no client certificate is sent"},
          {"--key", "no client certificate is sent"},
          {"--pinnedpubkey", "the server key is not pinned"},
          {"--connect-timeout", "only the whole-request timeout (--max-time) is kept"},
          {"--retry", "watt sends the request once"},
          {"--resolve", "the host name is resolved normally"},
          {"--connect-to", "the host name is resolved normally"},
          {"--unix-socket", "the request goes over TCP"},
          {"--interface", "the default interface is used"},
          {"--limit-rate", "the transfer is not rate-limited"},
          {"--aws-sigv4", "the request is not signed"},
          {"--max-redirs", "curl's default redirect limit applies"},
          {"--noproxy", "no proxy is used"},
      };
      for (const auto& [name, why] : noted)
        if (opt == name) warnings.push_back(opt + " " + val + " is not applied: " + why);
    } else if (opt.size() == 2 && short_takes_arg(opt[1])) {  // -o, -w, -D, ... ignored
      if (!attached && !arg()) return false;
    }
    // Anything else (-s, -S, -v, -i, --fail, --globoff, ...) is a no-op flag.
    return true;
  };

  for (; i < args.size(); ++i) {
    const std::string& a = args[i];
    // "Copy all as cURL" joins commands with " ;" (bash) or " &" (cmd): take the first one.
    if (a == ";" || a == "&" || a == "&&" || a == "|" || a == "||") break;
    if (a.size() > 2 && a[0] == '-' && a[1] == '-') {
      if (!apply(a, nullptr)) return std::nullopt;
    } else if (a.size() >= 2 && a[0] == '-') {
      // Short options, possibly combined (-sSL) or with an attached value (-XPOST, -H'a: b').
      for (size_t k = 1; k < a.size(); ++k) {
        const char c = a[k];
        if (short_takes_arg(c)) {
          if (k + 1 < a.size()) {
            const std::string rest = a.substr(k + 1);
            if (!apply(long_of(c), &rest)) return std::nullopt;
          } else if (!apply(long_of(c), nullptr)) {
            return std::nullopt;
          }
          break;
        }
        if (!apply(long_of(c), nullptr)) return std::nullopt;
      }
    } else if (url.empty()) {
      url = a;
    }
  }
  if (url.empty()) {
    if (err) *err = "no URL in curl command";
    return std::nullopt;
  }
  if (!globoff) {
    const std::string glob = url_glob(url);
    if (!glob.empty())
      warnings.push_back("curl expands the URL glob " + glob +
                         " into several requests (-g sends it as written): imported as one request, literally");
  }
  // Without -g, curl reads [] and {} in the URL as globs and \[ \] \{ \} as the literal
  // characters: Chrome's "Copy as cURL (bash)" escapes them that way (filter\[status\]=open).
  if (!globoff && url.find('\\') != std::string::npos) {
    std::string u;
    u.reserve(url.size());
    for (size_t k = 0; k < url.size(); ++k) {
      if (url[k] == '\\' && k + 1 < url.size() && std::string_view("[]{}").find(url[k + 1]) != std::string_view::npos)
        continue;
      u += url[k];
    }
    url = std::move(u);
  }
  if (url.find("://") == std::string::npos) url = "http://" + url;  // curl's default scheme
  if (has_upload && !out.body_file.empty() && out.body_file != "-" && url.find_first_of("?#") == std::string::npos) {
    // -T file to a URL with no file name part: curl appends the local file's name.
    const size_t host = url.find("://") + 3;
    std::string base = out.body_file.substr(out.body_file.find_last_of("/\\") + 1);
    if (url.find('/', host) == std::string::npos) url += "/" + base;
    else if (url.back() == '/') url += base;
  }

  bool data_files = false;
  for (const auto& d : data) data_files = data_files || !curl_data_file(d).empty();
  if (data_files && get) {
    // -G puts the data into the URL query, which is built now: a file there cannot be kept.
    std::erase_if(data, [&](const CurlData& d) {
      const std::string f = curl_data_file(d);
      if (f.empty()) return false;
      warnings.push_back("-G data file '" + f + "' is not imported: watt does not read files on import, "
                         "and the query cannot refer to one (add the value to the URL)");
      return true;
    });
    data_files = false;
  }
  std::string body;
  // With a file among them, the body is built when sending (SavedRequest::body_data).
  if (!data_files) build_curl_data_body(data, stdin_reader, body);

  auto add_query = [&](const std::string& q) {
    if (q.empty()) return;
    const size_t hash = url.find('#');
    std::string frag;
    if (hash != std::string::npos) {
      frag = url.substr(hash);
      url.resize(hash);
    }
    url += (url.find('?') == std::string::npos ? "?" : (url.back() == '?' || url.back() == '&' ? "" : "&")) + q;
    url += frag;
  };
  if (get && (has_data || has_json) && !url_query.empty()) {
    // curl 8 builds the -G query from the data and loses the --url-query parts: do the same,
    // so the request is the one curl sends, and say so.
    warnings.push_back("curl ignores --url-query together with -G and -d: those query parts are not sent");
    url_query.clear();
  }
  for (const auto& q : url_query) add_query(q);
  if (get && (has_data || has_json)) {
    add_query(body);
    body.clear();
    has_data = has_json = false;
  }

  if (!form.empty()) {
    body.clear();  // built from the parts at send time (build_form_body)
    if (has_data || has_json) warnings.push_back("-d/--json and -F cannot be combined (curl refuses it): the data is dropped");
    has_data = has_json = false;
  } else if (has_upload) {
    body = upload;
    // curl -T sends no Content-Type; without this the engine (and a --data-raw export) would
    // add application/x-www-form-urlencoded.
    if (!has_header(r.headers, "Content-Type")) r.headers.emplace_back("Content-Type", kRemoveHeader);
  }
  if (!has_upload || !form.empty()) out.body_file.clear();
  // A body that reads files (-d @f, --data-binary @f, ...): kept as references, read when sent.
  // A single --data-binary @f is the body file as it is (like -T).
  if (!has_upload && (has_data || has_json) && form.empty() && data_files) {
    for (const auto& d : data)
      if (const std::string f = curl_data_file(d); !f.empty()) deferred("data file", f);
    if (data.size() == 1 && data[0].kind == CurlData::Kind::Binary) out.body_file = curl_data_file(data[0]);
    else out.body_data = data;
  } else {
    data_files = false;
  }

  if (!method.empty()) r.method = method;
  else if (head) r.method = "HEAD";
  else if (get) r.method = "GET";
  else if (has_upload) r.method = "PUT";
  else if (has_data || has_json || !form.empty()) r.method = "POST";
  else r.method = "GET";

  if (has_json) {
    if (!has_header(r.headers, "Content-Type")) r.headers.emplace_back("Content-Type", "application/json");
    if (!has_header(r.headers, "Accept")) r.headers.emplace_back("Accept", "application/json");
  } else if (has_data && !has_header(r.headers, "Content-Type")) {
    r.headers.emplace_back("Content-Type", "application/x-www-form-urlencoded");  // what curl sends
  }
  if (!user.empty() && !has_header(r.headers, "Authorization")) {
    // Structured (user and password can become {{vars}}); the header is built at send time.
    // curl splits at the first ':' (no ':': it would prompt; an empty password is sent).
    const size_t colon = user.find(':');
    out.basic_auth.enabled = true;
    out.basic_auth.user = user.substr(0, colon);
    out.basic_auth.password = colon == std::string::npos ? std::string() : user.substr(colon + 1);
    if (!auth_scheme.empty() && auth_scheme != "--anyauth") {
      // Sending Basic instead would put the credentials in another scheme than asked for.
      out.basic_auth.enabled = false;
      warnings.push_back(auth_scheme + ": watt has Basic auth only; the -u credentials are kept but disabled");
    } else if (auth_scheme == "--anyauth") {
      warnings.push_back("--anyauth: imported as Basic auth (curl would use the best scheme the server offers)");
    }
  }
  size_t jar_count = 0;
  for (const auto& f : cookie_files) {
    if (f != "-") {  // a jar file: read when sent, its cookies then go first (as curl's)
      out.cookie_files.push_back(f);
      continue;
    }
    std::string jar;
    read_stdin(jar, warnings);
    const std::string c = jar_cookies(jar, url);
    if (!c.empty()) cookies.insert(cookies.begin() + static_cast<std::ptrdiff_t>(jar_count++), c);  // curl: jar first
  }
  if ((!cookies.empty() || !out.cookie_files.empty()) && has_header(r.headers, "Cookie")) {
    // curl sends only the custom header and drops the -b cookies.
    warnings.push_back("-b cookies are not sent because the command also sets a Cookie header (curl does the same)");
    cookies.clear();
    out.cookie_files.clear();
  }
  for (const auto& f : out.cookie_files) deferred("cookie file", f);
  if (!cookies.empty()) {
    std::string joined;
    for (const auto& c : cookies) joined += (joined.empty() ? "" : "; ") + c;
    bool merged = false;
    for (auto& [k, v] : r.headers)
      if (lower(k) == "cookie") {
        v += (v.empty() ? "" : "; ") + joined;
        merged = true;
        break;
      }
    if (!merged) r.headers.emplace_back("Cookie", joined);
  }
  // -d '' (or an empty @file): curl still sends a body, Content-Length: 0. HttpRequest has no
  // has_body flag, so the explicit header carries it; to_curl_command turns it back into -d ''.
  if ((has_data || has_json) && body.empty() && form.empty() && !has_upload && !data_files &&
      !has_header(r.headers, "Content-Length"))
    r.headers.emplace_back("Content-Length", "0");
  // Accept-Encoding is kept exactly as given (browsers list br and zstd): it is part of the
  // request the server sees. --compressed alone adds what curl offers: deflate, gzip.
  if (compressed && !has_header(r.headers, "Accept-Encoding")) r.headers.emplace_back("Accept-Encoding", "deflate, gzip");

  r.url = url;
  r.body = std::move(body);
  return out;
}

std::optional<HttpRequest> parse_curl_command(const std::string& cmd, std::string* err) {
  auto imp = parse_curl(cmd, err);
  if (!imp) return std::nullopt;
  if (imp->basic_auth.enabled)
    imp->http.headers.emplace_back("Authorization",
                                   "Basic " + base64(imp->basic_auth.user + ":" + imp->basic_auth.password));
  if (!imp->form.empty()) {
    // No file is read (see CurlImport): a file part is sent empty, with its file name and type.
    std::vector<FormPart> parts = imp->form;
    for (auto& p : parts)
      if (p.kind != FormPart::Kind::Text) {
        if (p.kind == FormPart::Kind::File && p.filename.empty()) {
          const size_t slash = p.value.find_last_of("/\\");
          p.filename = slash == std::string::npos ? p.value : p.value.substr(slash + 1);
        }
        if (p.type.empty()) p.type = form_part_content_type(p);
        p.kind = FormPart::Kind::Text;
        p.value.clear();
      }
    if (!build_form_body(parts, imp->http, {}, err)) return std::nullopt;
  }
  return std::move(imp->http);
}

// ==================================================================== export

std::string to_curl_command(const HttpRequest& r, CurlShell shell) { return to_curl_command(r, {}, shell); }

std::string to_curl_command(const HttpRequest& r, const std::vector<FormPart>& form, CurlShell shell) {
  return to_curl_command(r, form, BasicAuth{}, shell);
}

namespace {

// upload: export the body as -T <file> (SavedRequest::body_file) instead of the bytes. refs: the
// saved request's other file references (body_data, header_files, cookie_files), exported as the
// arguments they came from.
std::string to_curl_impl(const HttpRequest& r, const std::vector<FormPart>& form, const BasicAuth& auth, CurlShell shell,
                         const std::string* upload, const SavedRequest* refs = nullptr) {
  bool legacy = false;  // PowerShell: arguments are pre-quoted for the CRT
  auto q = [&](const std::string& s) -> std::string {
    switch (shell) {
      case CurlShell::Cmd: return cmd_quote(s);
      case CurlShell::PowerShell: return ps_quote(s, legacy);
      default: return sh_quote(s);
    }
  };
  // An empty body imported from -d '' is an explicit "Content-Length: 0" (see model.h).
  bool empty_body = false;
  if (r.body.empty() && has_header(r.headers, "Content-Type"))
    for (const auto& [k, v] : r.headers)
      if (lower(k) == "content-length" && trim(v) == "0") empty_body = true;
  bool has_form = false;
  for (const auto& p : form) has_form = has_form || p.enabled;
  if (has_form) empty_body = false;
  if (upload) empty_body = false;
  const bool data_refs = refs && !refs->body_data.empty() && !has_form && !upload;
  if (data_refs) empty_body = false;
  const bool has_body = !upload && (has_form || data_refs || !r.body.empty() || empty_body);
  // No shell can put a NUL byte into an argument: bash pipes such a body in through printf,
  // PowerShell writes it to a temp file first (cmd.exe cannot, see curl_export_problem).
  const bool binary = !upload && !has_form && !data_refs && r.body.find('\0') != std::string::npos;
  const bool piped = binary && shell == CurlShell::Bash;
  const bool ps_file = binary && shell == CurlShell::PowerShell;

  // A file body goes out as -T only where -T sends exactly it: PUT (its default method) to a URL
  // with a file name part (-T appends the local file name to a URL that ends in '/'). Otherwise
  // --data-binary @file sends the same bytes.
  bool as_T = false;
  if (upload && r.method == "PUT") {
    const size_t host = r.url.find("://");
    const size_t path = host == std::string::npos ? std::string::npos : r.url.find('/', host + 3);
    as_T = *upload == "-" || r.url.find_first_of("?#") != std::string::npos || (path != std::string::npos && r.url.back() != '/');
  }
  std::string out = shell == CurlShell::PowerShell ? "curl.exe" : "curl";
  if (r.method == "HEAD" && !has_body && !upload) out += " --head";
  else if (!((r.method == "GET" && !has_body && !upload) || (r.method == "POST" && (has_body || (upload && !as_T))) ||
             (r.method == "PUT" && as_T)))
    out += " -X " + q(r.method);
  // curl treats [] and {} in a URL as globbing ranges unless -g (--globoff) is given.
  if (r.url.find_first_of("[]{}") != std::string::npos) out += " -g";
  out += " " + q(r.url);
  const char* sep = shell == CurlShell::Cmd ? " ^\n  " : shell == CurlShell::PowerShell ? " `\n  " : " \\\n  ";
  bool compressed = false;
  for (const auto& [k, v] : r.headers) {
    const std::string lk = lower(k);
    if (empty_body && lk == "content-length") continue;
    if (lk == "accept-encoding" && !is_remove_marker(v)) compressed = true;
    out += sep;
    out += "-H " + q(v.empty() ? k + ";" : is_remove_marker(v) ? k + ":" : k + ": " + v);
  }
  // PowerShell reads a bare @name as splatting: quote it.
  auto qa = [&](const std::string& s) {
    return shell == CurlShell::PowerShell && !s.empty() && s[0] == '@' && plain_word(s) ? "'" + s + "'" : q(s);
  };
  if (refs) {
    for (const auto& f : refs->header_files) {
      out += sep;
      out += "-H " + qa("@" + f);
    }
    for (const auto& f : refs->cookie_files) {
      out += sep;
      out += "-b " + q(f);
    }
  }
  if (auth.enabled) {
    out += sep;
    out += "-u " + q(auth.user + ":" + auth.password);
  }
  if (upload && as_T) {
    out += sep;
    out += "-T " + q(*upload);
  } else if (upload) {
    out += sep;
    out += "--data-binary " + qa("@" + *upload);
  } else if (data_refs) {
    for (const auto& d : refs->body_data) {
      static constexpr const char* opts[] = {"-d", "--data-binary", "--data-raw", "--data-urlencode", "--json"};
      out += sep;
      out += std::string(opts[static_cast<int>(d.kind)]) + " " + qa(d.value);
    }
  } else if (has_form) {
    for (const auto& p : form) {
      if (!p.enabled) continue;
      const auto [opt, arg] = form_part_curl_arg(p);
      out += sep;
      out += opt + " " + q(arg);
    }
  } else if (piped) {
    out += sep;
    out += "--data-binary @-";
    out = "printf %b " + printf_b_quote(r.body) + " | " + out;
  } else if (ps_file) {
    out += sep;
    out += std::string("--data-binary \"@") + kPsBodyVar + "\"";
    out = std::string(kPsBodyVar) + " = Join-Path ([IO.Path]::GetTempPath()) 'watt-body.bin'; [IO.File]::WriteAllBytes(" +
          kPsBodyVar + ", [Convert]::FromBase64String('" + base64(r.body) + "'))\n" + out;
  } else if (has_body) {
    out += sep;
    out += "--data-raw " + q(r.body);
  }
  std::string flags;
  if (!r.verify_tls) flags += " -k";
  if (r.follow_redirects) flags += " -L";
  // Only a timeout the user chose: every request starts with the default, and curl has none.
  if (r.timeout_ms > 0 && r.timeout_ms != HttpRequest{}.timeout_ms) flags += " --max-time " + seconds(r.timeout_ms);
  switch (r.version) {
    case HttpVersion::Http1_1: flags += " --http1.1"; break;
    case HttpVersion::Http2: flags += " --http2"; break;
    case HttpVersion::Http2PriorKnowledge: flags += " --http2-prior-knowledge"; break;
    default: break;
  }
  if (r.fresh_connection) flags += " --no-keepalive";
  // The engine decodes what it asks for; curl only does with --compressed.
  if (compressed) flags += " --compressed";
  if (!flags.empty()) {
    out += sep;
    out += flags.substr(1);
  }
  // PowerShell 7.3+ quotes native arguments itself; Legacy makes it pass them as 5.1 does.
  if (legacy) out = "$PSNativeCommandArgumentPassing = 'Legacy'\n" + out;
  return out;
}

}  // namespace

std::string to_curl_command(const HttpRequest& r, const std::vector<FormPart>& form, const BasicAuth& auth,
                            CurlShell shell) {
  return to_curl_impl(r, form, auth, shell, nullptr);
}

std::string to_curl_command(const SavedRequest& r, CurlShell shell) {
  bool has_form = false;
  for (const auto& p : r.form) has_form = has_form || p.enabled;
  const std::string* file = r.body_file.empty() || has_form ? nullptr : &r.body_file;
  if (r.disabled_headers.empty()) return to_curl_impl(r.http, r.form, r.basic_auth, shell, file, &r);
  HttpRequest h = r.http;  // disabled headers are not sent, so not exported
  h.headers.clear();
  for (size_t k = 0; k < r.http.headers.size(); ++k)
    if (!r.disabled_headers.count(k)) h.headers.push_back(r.http.headers[k]);
  return to_curl_impl(h, r.form, r.basic_auth, shell, file, &r);
}

std::string curl_export_problem(const HttpRequest& r, const std::vector<FormPart>& form, CurlShell shell) {
  bool nul = false, cr = false, non_ascii = false;
  auto scan = [&](const std::string& s) {
    for (unsigned char c : s) {
      nul = nul || c == 0;
      cr = cr || c == '\r';
      non_ascii = non_ascii || c >= 0x80;
    }
  };
  scan(r.url);
  for (const auto& [k, v] : r.headers) {
    scan(k);
    scan(v);
  }
  bool has_form = false;
  for (const auto& p : form)
    if (p.enabled) {
      has_form = true;
      scan(p.name);
      scan(p.value);
      scan(p.filename);
      scan(p.type);
    }
  bool body_nul = false;
  if (!has_form) {
    body_nul = r.body.find('\0') != std::string::npos;
    scan(r.body);
  }
  if (nul) {
    // A NUL only in the body: bash pipes it through printf, PowerShell writes a temp file.
    std::string rest = r.url;
    for (const auto& [k, v] : r.headers) rest += k + v;
    if (rest.find('\0') != std::string::npos || !body_nul) return "a NUL byte cannot be passed to curl in any shell";
  }
  if (shell == CurlShell::Cmd && (nul || cr || non_ascii)) {
    // Name every issue, not just the first: fixing one must not reveal another.
    std::vector<std::string> why;
    if (nul) why.push_back("cannot pass the NUL bytes in the body");
    if (cr) why.push_back("cannot pass a carriage return (CR) in an argument");
    if (non_ascii) why.push_back("passes non-ASCII text in the console code page, not UTF-8");
    std::string msg = "cmd.exe";
    for (size_t k = 0; k < why.size(); ++k)
      msg += (k == 0 ? " " : k + 1 == why.size() ? (why.size() > 2 ? ", and " : " and ") : ", ") + why[k];
    return msg + ": use the bash or PowerShell export";
  }
  return {};
}

std::string to_grpcurl_command(const SavedRequest& r) {
  const std::string sep = " \\\n  ";
  std::string out = "grpcurl";
  if (!r.grpc_target.tls) out += " -plaintext";
  else if (r.grpc_target.insecure_skip_verify) out += " -insecure";
  if (!r.grpc_target.authority.empty()) out += " -authority " + sh_quote(r.grpc_target.authority);
  if (r.grpc_opts.deadline_ms > 0) out += " -max-time " + seconds(r.grpc_opts.deadline_ms);
  for (const auto& d : r.import_dirs) out += sep + "-import-path " + sh_quote(d);
  for (const auto& p : r.proto_files) out += sep + "-proto " + sh_quote(p);
  for (const auto& [k, v] : r.grpc_opts.metadata) out += sep + "-H " + sh_quote(k + ": " + v);
  // grpcurl reads a stream of concatenated JSON messages, so a JSON array body (client/bidi
  // streaming in watt) becomes one message per line.
  std::string body = r.grpc_body;
  size_t p = body.find_first_not_of(" \t\r\n");
  if (p != std::string::npos && body[p] == '[') {
    rapidjson::Document d;
    d.Parse(body.data(), body.size());
    if (!d.HasParseError() && d.IsArray()) {
      std::string msgs;
      for (const auto& m : d.GetArray()) msgs += (msgs.empty() ? "" : "\n") + detail::json_compact(m);
      body = msgs;
    }
  }
  if (body.find_first_not_of(" \t\r\n") != std::string::npos) out += sep + "-d " + sh_quote(body);
  std::string method = r.grpc_method;
  if (!method.empty() && method[0] == '/') method.erase(0, 1);
  out += sep + sh_quote(r.grpc_target.address) + " " + sh_quote(method);
  return out;
}

}  // namespace watt
