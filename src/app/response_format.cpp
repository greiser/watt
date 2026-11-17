#include "app/response_format.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace watt::app::fmt {
namespace {

bool ieq_prefix(std::string_view s, std::string_view prefix) {
  if (s.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c != prefix[i]) return false;
  }
  return true;
}

bool icontains(std::string_view s, std::string_view needle) {
  if (needle.size() > s.size()) return false;
  for (size_t i = 0; i + needle.size() <= s.size(); ++i)
    if (ieq_prefix(s.substr(i), needle)) return true;
  return false;
}

bool is_ws(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

}  // namespace

BodyKind detect_kind(std::string_view ct, std::string_view body) {
  if (body.empty()) return BodyKind::Empty;
  if (icontains(ct, "json")) return BodyKind::Json;
  if (icontains(ct, "html")) return BodyKind::Html;
  if (icontains(ct, "xml")) return BodyKind::Xml;
  const bool textual_ct = ieq_prefix(ct, "text/") || icontains(ct, "javascript");
  // Sniff the first 4 KB: NUL or many control bytes mean binary.
  const std::string_view head = body.substr(0, 4096);
  size_t ctrl = 0;
  for (unsigned char c : head) {
    if (c == 0) return textual_ct ? BodyKind::Text : BodyKind::Binary;
    if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') ++ctrl;
  }
  if (!textual_ct && ctrl * 20 > head.size()) return BodyKind::Binary;
  size_t i = 0;
  if (head.size() >= 3 && static_cast<unsigned char>(head[0]) == 0xEF) i = 3;  // UTF-8 BOM
  while (i < head.size() && is_ws(head[i])) ++i;
  if (i < head.size()) {
    const char c = head[i];
    if (c == '{' || c == '[') return BodyKind::Json;
    if (c == '<') {
      const std::string_view rest = head.substr(i);
      if (ieq_prefix(rest, "<!doctype html") || ieq_prefix(rest, "<html")) return BodyKind::Html;
      return BodyKind::Xml;
    }
  }
  if (!ct.empty() && !textual_ct && (ieq_prefix(ct, "image/") || ieq_prefix(ct, "audio/") ||
                                     ieq_prefix(ct, "video/") || icontains(ct, "octet-stream") ||
                                     icontains(ct, "protobuf") || icontains(ct, "zip")))
    return BodyKind::Binary;
  return BodyKind::Text;
}

const char* kind_name(BodyKind k) {
  switch (k) {
    case BodyKind::Empty: return "Empty";
    case BodyKind::Json: return "JSON";
    case BodyKind::Xml: return "XML";
    case BodyKind::Html: return "HTML";
    case BodyKind::Text: return "Text";
    case BodyKind::Binary: return "Binary";
  }
  return "";
}

bool pretty_json(std::string_view in, std::string& out, size_t* error_at) {
  out.clear();
  out.reserve(in.size() + in.size() / 2 + 64);
  // Indentation strings are appended from a fixed run of spaces (depth * 2, capped buffer).
  static const std::string spaces(512, ' ');
  std::vector<char> stack;
  stack.reserve(64);
  auto newline = [&](size_t depth) {
    out.push_back('\n');
    size_t n = depth * 2;
    while (n) {
      const size_t k = std::min(n, spaces.size());
      out.append(spaces.data(), k);
      n -= k;
    }
  };
  auto fail = [&](size_t at) {
    if (error_at) *error_at = at;
    return false;
  };
  const char* p = in.data();
  const size_t n = in.size();
  size_t i = 0;
  if (n >= 3 && static_cast<unsigned char>(p[0]) == 0xEF && static_cast<unsigned char>(p[1]) == 0xBB &&
      static_cast<unsigned char>(p[2]) == 0xBF)
    i = 3;
  bool seen_value = false;  // a complete top-level value has been written
  while (i < n) {
    const char c = p[i];
    switch (c) {
      case ' ': case '\n': case '\r': case '\t':
        ++i;
        break;
      case '"': {
        if (seen_value && stack.empty()) return fail(i);
        const size_t start = i++;
        for (;;) {
          // Fast scan to the next quote or backslash.
          const char* q = p + i;
          const char* end = p + n;
          while (q < end && *q != '"' && *q != '\\') ++q;
          if (q >= end) return fail(start);
          i = static_cast<size_t>(q - p);
          if (*q == '\\') {
            i += 2;
            if (i > n) return fail(start);
            continue;
          }
          ++i;
          break;
        }
        out.append(p + start, i - start);
        if (stack.empty()) seen_value = true;
        break;
      }
      case '{': case '[': {
        if (seen_value && stack.empty()) return fail(i);
        const char close = c == '{' ? '}' : ']';
        size_t j = i + 1;
        while (j < n && is_ws(p[j])) ++j;
        if (j < n && p[j] == close) {  // empty container stays on one line
          out.push_back(c);
          out.push_back(close);
          i = j + 1;
          if (stack.empty()) seen_value = true;
          break;
        }
        out.push_back(c);
        stack.push_back(close);
        newline(stack.size());
        i = j;
        break;
      }
      case '}': case ']':
        if (stack.empty() || stack.back() != c) return fail(i);
        stack.pop_back();
        newline(stack.size());
        out.push_back(c);
        ++i;
        if (stack.empty()) seen_value = true;
        break;
      case ',':
        if (stack.empty()) return fail(i);
        out.push_back(',');
        newline(stack.size());
        ++i;
        break;
      case ':':
        if (stack.empty() || stack.back() != '}') return fail(i);
        out.append(": ", 2);
        ++i;
        break;
      default: {
        // Number / true / false / null: copy the token.
        if (seen_value && stack.empty()) return fail(i);
        const size_t start = i;
        while (i < n) {
          const char d = p[i];
          if (is_ws(d) || d == ',' || d == ':' || d == '}' || d == ']' || d == '{' || d == '[' || d == '"') break;
          ++i;
        }
        out.append(p + start, i - start);
        if (stack.empty()) seen_value = true;
        break;
      }
    }
  }
  if (!stack.empty()) return fail(n);
  if (!seen_value) return fail(0);
  out.push_back('\n');
  return true;
}

bool pretty_xml(std::string_view in, std::string& out) {
  out.clear();
  out.reserve(in.size() + in.size() / 4 + 64);
  const char* p = in.data();
  const size_t n = in.size();
  size_t i = 0;
  int depth = 0;
  bool inline_text = false;  // the previous token was text right after an opening tag
  bool last_open = false;    // the previous token was an opening tag
  auto newline = [&] {
    if (!out.empty()) out.push_back('\n');
    for (int d = 0; d < depth; ++d) out.append("  ", 2);
  };
  while (i < n) {
    if (p[i] == '<') {
      size_t end;
      std::string_view rest(p + i, n - i);
      if (rest.substr(0, 4) == "<!--") {
        const size_t e = rest.find("-->");
        end = e == std::string_view::npos ? n : i + e + 3;
      } else if (rest.substr(0, 9) == "<![CDATA[") {
        const size_t e = rest.find("]]>");
        end = e == std::string_view::npos ? n : i + e + 3;
      } else {
        // Up to the closing '>' outside quoted attribute values.
        size_t j = i + 1;
        char quote = 0;
        while (j < n) {
          const char c = p[j];
          if (quote) {
            if (c == quote) quote = 0;
          } else if (c == '"' || c == '\'') {
            quote = c;
          } else if (c == '>') {
            break;
          }
          ++j;
        }
        if (j >= n) return false;
        end = j + 1;
      }
      const std::string_view tag(p + i, end - i);
      const bool closing = tag.size() > 1 && tag[1] == '/';
      const bool special = tag.size() > 1 && (tag[1] == '?' || tag[1] == '!');
      const bool self_close = tag.size() > 2 && tag[tag.size() - 2] == '/';
      if (closing) {
        --depth;
        if (depth < 0) return false;
        if (!inline_text && !last_open) newline();
        out.append(tag);
        last_open = false;
      } else {
        newline();
        out.append(tag);
        last_open = !special && !self_close;
        if (last_open) ++depth;
      }
      inline_text = false;
      i = end;
    } else {
      size_t j = i;
      while (j < n && p[j] != '<') ++j;
      size_t a = i, b = j;
      while (a < b && is_ws(p[a])) ++a;
      while (b > a && is_ws(p[b - 1])) --b;
      if (a < b) {
        if (!last_open) newline();
        out.append(p + a, b - a);
        inline_text = last_open;
        last_open = false;
      }
      i = j;
    }
  }
  out.push_back('\n');
  return depth == 0;
}

// ------------------------------------------------------------------ HTML

namespace {

char lower_c(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool name_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == ':' ||
         c == '_' || c == '.';
}

bool in_list(std::string_view name, std::initializer_list<std::string_view> l) {
  for (std::string_view s : l)
    if (s == name) return true;
  return false;
}

bool html_void(std::string_view n) {
  return in_list(n, {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source",
                     "track", "wbr", "keygen"});
}
bool html_inline(std::string_view n) {
  return in_list(n, {"a", "abbr", "b", "bdi", "bdo", "cite", "code", "data", "dfn", "em", "i", "kbd", "mark", "q", "s",
                     "samp", "small", "span", "strong", "sub", "sup", "time", "u", "var", "font", "label", "img",
                     "br", "wbr", "button", "input", "tt", "strike", "big", "nobr", "del", "ins"});
}

// One markup token starting at p[i] == '<'. kind: 0 = not a tag (a literal '<'), 1 = open,
// 2 = close, 3 = comment / doctype / processing instruction / CDATA. end = one past the token.
struct HtmlTag {
  int kind = 0;
  size_t end = 0;
  std::string name;  // lower case
  bool self_close = false;
};

HtmlTag scan_tag(const char* p, size_t n, size_t i) {
  HtmlTag t;
  if (i + 1 >= n) return t;
  const std::string_view rest(p + i, n - i);
  auto until = [&](std::string_view close) {
    const size_t e = rest.find(close);
    return e == std::string_view::npos ? n : i + e + close.size();
  };
  if (rest.substr(0, 4) == "<!--") {
    t.kind = 3;
    t.end = until("-->");
    return t;
  }
  if (rest.substr(0, 9) == "<![CDATA[") {
    t.kind = 3;
    t.end = until("]]>");
    return t;
  }
  const char c1 = p[i + 1];
  if (c1 == '!' || c1 == '?') {
    t.kind = 3;
    t.end = until(">");
    return t;
  }
  size_t j = i + 1;
  const bool closing = c1 == '/';
  if (closing) ++j;
  if (j >= n || !((p[j] >= 'a' && p[j] <= 'z') || (p[j] >= 'A' && p[j] <= 'Z'))) return t;
  const size_t ns = j;
  while (j < n && name_char(p[j])) ++j;
  t.name.reserve(j - ns);
  for (size_t k = ns; k < j; ++k) t.name.push_back(lower_c(p[k]));
  // Up to '>' outside quoted attribute values (a quote only opens a value right after '=', so an
  // apostrophe in an unquoted value such as href=it's does not swallow the page).
  char quote = 0;
  bool after_eq = false;
  for (; j < n; ++j) {
    const char c = p[j];
    if (quote) {
      if (c == quote) quote = 0;
      continue;
    }
    if (c == '>') break;
    if (c == '=') after_eq = true;
    else if ((c == '"' || c == '\'') && after_eq) quote = c;
    else if (!is_ws(c)) after_eq = false;
  }
  if (j >= n) return t;  // unterminated: the rest is text
  t.end = j + 1;
  t.kind = closing ? 2 : 1;
  t.self_close = !closing && j > i + 1 && p[j - 1] == '/';
  return t;
}

// Case-insensitive search for "</name" from `from`; npos if none.
size_t find_close_tag(const char* p, size_t n, size_t from, std::string_view name) {
  for (size_t k = from; k + 2 + name.size() <= n; ++k) {
    if (p[k] != '<' || p[k + 1] != '/') continue;
    bool eq = true;
    for (size_t m = 0; m < name.size() && eq; ++m) eq = lower_c(p[k + 2 + m]) == name[m];
    if (eq && (k + 2 + name.size() == n || !name_char(p[k + 2 + name.size()]))) return k;
  }
  return std::string_view::npos;
}

// Appends s with whitespace runs (outside quotes) collapsed to one space.
void append_collapsed_tag(std::string& out, std::string_view s) {
  bool ws = false;
  char quote = 0;
  for (char c : s) {
    if (quote) {
      out.push_back(c);
      if (c == quote) quote = 0;
      continue;
    }
    if (is_ws(c)) {
      ws = true;
      continue;
    }
    if (ws && c != '>') out.push_back(' ');
    ws = false;
    if (c == '"' || c == '\'') quote = c;
    out.push_back(c);
  }
}

}  // namespace

bool pretty_html(std::string_view in, std::string& out) {
  out.clear();
  out.reserve(in.size() + in.size() / 3 + 64);
  const char* p = in.data();
  const size_t n = in.size();
  std::vector<std::string> stack;  // open block elements
  std::string run;                 // inline content waiting for its line
  bool pending_space = false;
  bool last_open = false;    // the last thing written was a block open tag (its line can take text)
  bool inline_text = false;  // text was written on that open tag's line
  bool any_tag = false;
  auto indent = [&](size_t depth) {
    for (size_t d = 0; d < depth; ++d) out.append("  ", 2);
  };
  auto newline = [&] {
    if (!out.empty()) out.push_back('\n');
    indent(stack.size());
  };
  auto flush_run = [&] {
    pending_space = false;
    if (run.empty()) return;
    if (last_open) {
      inline_text = true;
    } else {
      newline();
      inline_text = false;
    }
    out.append(run);
    run.clear();
    last_open = false;
  };
  auto add_text = [&](std::string_view s) {
    for (char c : s) {
      if (is_ws(c)) {
        if (!run.empty()) pending_space = true;
        continue;
      }
      if (pending_space) run.push_back(' ');
      pending_space = false;
      run.push_back(c);
    }
  };
  auto pop_to = [&](size_t k) { stack.resize(k); };
  auto top_is = [&](std::initializer_list<std::string_view> l) { return !stack.empty() && in_list(stack.back(), l); };
  // Elements whose end tag may be left out: opening `name` closes them.
  auto implicit_close = [&](const std::string& name) {
    if (name == "li") {
      if (top_is({"li"})) stack.pop_back();
    } else if (name == "td" || name == "th") {
      if (top_is({"td", "th"})) stack.pop_back();
    } else if (name == "tr") {
      if (top_is({"td", "th"})) stack.pop_back();
      if (top_is({"tr"})) stack.pop_back();
    } else if (name == "dt" || name == "dd") {
      if (top_is({"dt", "dd"})) stack.pop_back();
    } else if (name == "option") {
      if (top_is({"option"})) stack.pop_back();
    } else if (name == "body") {
      if (top_is({"head"})) stack.pop_back();
    } else if (name == "tbody" || name == "thead" || name == "tfoot") {
      if (top_is({"td", "th"})) stack.pop_back();
      if (top_is({"tr"})) stack.pop_back();
      if (top_is({"tbody", "thead", "tfoot"})) stack.pop_back();
    }
    // A block start ends an open paragraph.
    if (top_is({"p"}) && !html_inline(name)) stack.pop_back();
  };

  size_t i = 0;
  while (i < n) {
    if (p[i] != '<') {
      const void* lt = std::memchr(p + i, '<', n - i);
      const size_t j = lt ? static_cast<size_t>(static_cast<const char*>(lt) - p) : n;
      add_text(std::string_view(p + i, j - i));
      i = j;
      continue;
    }
    const HtmlTag t = scan_tag(p, n, i);
    if (t.kind == 0) {
      add_text(std::string_view(p + i, 1));
      ++i;
      continue;
    }
    any_tag = true;
    const std::string_view tag(p + i, t.end - i);
    if (t.kind == 3) {
      flush_run();
      newline();
      out.append(tag);
      last_open = inline_text = false;
      i = t.end;
      continue;
    }
    if (html_inline(t.name)) {
      if (pending_space && !run.empty()) run.push_back(' ');
      pending_space = false;
      append_collapsed_tag(run, tag);
      i = t.end;
      if (t.name == "br") flush_run();  // a line break ends the line
      continue;
    }
    if (t.kind == 2) {  // block close tag
      size_t k = stack.size();
      const size_t floor = stack.size() > 256 ? stack.size() - 256 : 0;
      while (k > floor && stack[k - 1] != t.name) --k;
      const bool found = k > floor;
      const bool same_line = found && k == stack.size() && (last_open || inline_text);
      if (same_line && last_open) {
        // <p>text</p>: the text goes on the open tag's line, the close tag right after it.
        out.append(run);
        run.clear();
        pending_space = false;
      } else {
        flush_run();
      }
      const bool on_line = found && k == stack.size() && (inline_text || last_open || same_line);
      if (found) pop_to(k - 1);
      if (!on_line) newline();
      out.append(tag);
      last_open = inline_text = false;
      i = t.end;
      continue;
    }
    // Block (or unknown) open tag.
    implicit_close(t.name);
    flush_run();
    newline();
    append_collapsed_tag(out, tag);
    i = t.end;
    last_open = inline_text = false;
    if (t.self_close || html_void(t.name)) continue;
    const bool raw = t.name == "script" || t.name == "style";
    const bool verbatim = t.name == "pre" || t.name == "textarea" || t.name == "xmp" || t.name == "plaintext";
    if (raw || verbatim) {
      size_t close = find_close_tag(p, n, i, t.name);
      if (close == std::string_view::npos) close = n;
      std::string_view body(p + i, close - i);
      size_t close_end = close;
      if (close < n) {
        const HtmlTag ct = scan_tag(p, n, close);
        close_end = ct.kind == 2 ? ct.end : close + 2 + t.name.size();
      }
      const std::string_view close_tag(p + close, close_end - close);
      if (verbatim) {
        out.append(body);
        out.append(close_tag);
      } else {
        // Trim blank lines around the content, then re-indent it as a block one level deeper.
        size_t a = 0, b = body.size();
        while (a < b && is_ws(body[a])) ++a;
        while (b > a && is_ws(body[b - 1])) --b;
        std::string css;
        if (t.name == "style" && b - a > 60 && body.substr(a, b - a).find('\n') == std::string_view::npos) {
          // Minified CSS, expanded like a browser's dev tools: "sel {", one declaration per
          // line indented by 2 per nesting level (@media), "}" on its own line.
          char q = 0;
          int depth = 0, paren = 0;
          bool at_line_start = true;
          auto line_break = [&] {
            while (!css.empty() && css.back() == ' ') css.pop_back();
            if (!css.empty() && css.back() != '\n') css.push_back('\n');
            at_line_start = true;
          };
          for (size_t k = a; k < b; ++k) {
            const char c = body[k];
            if (at_line_start && !q) {
              if (is_ws(c)) continue;
              if (c != '}') css.append(static_cast<size_t>(std::max(depth, 0)) * 2, ' ');
              at_line_start = false;
            }
            if (q) {
              css.push_back(c);
              if (c == '\\' && k + 1 < b) css.push_back(body[++k]);
              else if (c == q) q = 0;
              continue;
            }
            if (c == '"' || c == '\'') {
              q = c;
              css.push_back(c);
            } else if (c == '(') {
              ++paren;
              css.push_back(c);
            } else if (c == ')') {
              paren = std::max(0, paren - 1);
              css.push_back(c);
            } else if (c == '{' && !paren) {
              while (!css.empty() && css.back() == ' ') css.pop_back();
              css.append(" {");
              ++depth;
              line_break();
            } else if (c == ';' && !paren) {
              css.push_back(';');
              line_break();
            } else if (c == '}' && !paren) {
              line_break();
              depth = std::max(0, depth - 1);
              css.append(static_cast<size_t>(depth) * 2, ' ');
              css.push_back('}');
              line_break();
            } else if (c == ':' && !paren && depth > 0 && k + 1 < b && body[k + 1] != ' ' && body[k + 1] != ':') {
              // "font:14px" -> "font: 14px" inside declaration blocks (not in selectors like a:hover)
              css.push_back(':');
              bool decl = true;  // a selector inside @media has a '{' before the next ';' / '}'
              for (size_t z = k + 1; z < b; ++z) {
                if (body[z] == '{') { decl = false; break; }
                if (body[z] == ';' || body[z] == '}') break;
              }
              if (decl) css.push_back(' ');
            } else {
              css.push_back(c);
            }
          }
          while (!css.empty() && (css.back() == '\n' || css.back() == ' ')) css.pop_back();
          body = css;
          a = 0;
          b = css.size();
        }
        const std::string_view inner = body.substr(a, b - a);
        if (inner.empty()) {
          out.append(close_tag);
        } else if (inner.find('\n') == std::string_view::npos && inner.size() <= 100) {
          out.append(inner);
          out.append(close_tag);
        } else {
          // Start of the first content line (keeps its own indentation for the common prefix).
          size_t ls = a;
          while (ls > 0 && body[ls - 1] != '\n') --ls;
          const std::string_view block = body.substr(ls, b - ls);
          size_t common = std::string_view::npos;
          for (size_t s = 0; s < block.size();) {
            size_t e = block.find('\n', s);
            if (e == std::string_view::npos) e = block.size();
            size_t w = s;
            while (w < e && (block[w] == ' ' || block[w] == '\t')) ++w;
            if (w < e && block[w] != '\r') common = std::min(common, w - s);
            s = e + 1;
          }
          if (common == std::string_view::npos) common = 0;
          for (size_t s = 0; s < block.size();) {
            size_t e = block.find('\n', s);
            if (e == std::string_view::npos) e = block.size();
            std::string_view line = block.substr(s, e - s);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            out.push_back('\n');
            size_t w = 0;
            while (w < line.size() && (line[w] == ' ' || line[w] == '\t')) ++w;
            if (w < line.size()) {
              indent(stack.size() + 1);
              out.append(line.substr(std::min(common, w)));
            }
            s = e + 1;
          }
          newline();
          out.append(close_tag);
        }
      }
      i = close_end;
      continue;
    }
    stack.push_back(t.name);
    last_open = true;
  }
  flush_run();
  out.push_back('\n');
  return any_tag;
}

namespace {

// Decodes the common character references; unknown ones stay as written.
void append_decoded(std::string& out, std::string_view s) {
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '&') {
      out.push_back(s[i]);
      continue;
    }
    const size_t semi = s.find(';', i);
    if (semi == std::string_view::npos || semi - i > 10) {
      out.push_back('&');
      continue;
    }
    const std::string_view ent = s.substr(i + 1, semi - i - 1);
    uint32_t cp = 0;
    if (!ent.empty() && ent[0] == '#') {
      const bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
      for (size_t k = hex ? 2 : 1; k < ent.size(); ++k) {
        const char c = ent[k];
        int d = -1;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
        if (d < 0) {
          cp = 0;
          break;
        }
        cp = cp * (hex ? 16 : 10) + static_cast<uint32_t>(d);
        if (cp > 0x10FFFF) break;
      }
    } else if (ent == "amp") cp = '&';
    else if (ent == "lt") cp = '<';
    else if (ent == "gt") cp = '>';
    else if (ent == "quot") cp = '"';
    else if (ent == "apos") cp = '\'';
    else if (ent == "nbsp") cp = ' ';
    else if (ent == "copy") cp = 0xA9;
    else if (ent == "reg") cp = 0xAE;
    else if (ent == "mdash") cp = 0x2014;
    else if (ent == "ndash") cp = 0x2013;
    else if (ent == "hellip") cp = 0x2026;
    else if (ent == "laquo") cp = 0xAB;
    else if (ent == "raquo") cp = 0xBB;
    if (cp == 0 || cp > 0x10FFFF) {
      out.push_back('&');
      continue;
    }
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    i = semi;
  }
}

std::string attr_value(std::string_view tag, std::string_view attr) {
  // Finds attr=value (quoted or not) in a tag, case-insensitively on the attribute name.
  for (size_t k = 1; k + attr.size() < tag.size(); ++k) {
    if (!is_ws(tag[k - 1])) continue;
    bool eq = true;
    for (size_t m = 0; m < attr.size() && eq; ++m) eq = lower_c(tag[k + m]) == attr[m];
    if (!eq) continue;
    size_t j = k + attr.size();
    while (j < tag.size() && is_ws(tag[j])) ++j;
    if (j >= tag.size() || tag[j] != '=') continue;
    ++j;
    while (j < tag.size() && is_ws(tag[j])) ++j;
    if (j >= tag.size()) return {};
    if (tag[j] == '"' || tag[j] == '\'') {
      const size_t e = tag.find(tag[j], j + 1);
      return std::string(tag.substr(j + 1, (e == std::string_view::npos ? tag.size() : e) - j - 1));
    }
    size_t e = j;
    while (e < tag.size() && !is_ws(tag[e]) && tag[e] != '>') ++e;
    return std::string(tag.substr(j, e - j));
  }
  return {};
}

}  // namespace

void html_to_text(std::string_view in, std::string& out) {
  out.clear();
  out.reserve(in.size() / 2 + 64);
  const char* p = in.data();
  const size_t n = in.size();
  std::string line;  // current line, whitespace collapsed
  bool space = false;
  int blank = 2;  // newlines at the end of out (2 = paragraph break already there)
  std::string href;
  size_t link_start = 0;
  std::string title;
  auto end_line = [&](bool paragraph) {
    if (!line.empty()) {
      out.append(line);
      out.push_back('\n');
      line.clear();
      blank = 1;
    }
    space = false;
    if (paragraph && blank < 2 && !out.empty()) {
      out.push_back('\n');
      blank = 2;
    }
  };
  auto text = [&](std::string_view s) {
    std::string dec;
    append_decoded(dec, s);
    for (char c : dec) {
      if (is_ws(c)) {
        space = !line.empty();
        continue;
      }
      if (space) line.push_back(' ');
      space = false;
      line.push_back(c);
    }
  };
  size_t i = 0;
  while (i < n) {
    if (p[i] != '<') {
      const void* lt = std::memchr(p + i, '<', n - i);
      const size_t j = lt ? static_cast<size_t>(static_cast<const char*>(lt) - p) : n;
      text(std::string_view(p + i, j - i));
      i = j;
      continue;
    }
    const HtmlTag t = scan_tag(p, n, i);
    if (t.kind == 0) {
      text("<");
      ++i;
      continue;
    }
    const std::string_view tag(p + i, t.end - i);
    i = t.end;
    if (t.kind == 3) continue;
    const std::string& nm = t.name;
    if (t.kind == 1 && (nm == "script" || nm == "style" || nm == "noscript" || nm == "template" || nm == "title" ||
                        nm == "svg")) {
      size_t close = find_close_tag(p, n, i, nm);
      if (close == std::string_view::npos) close = n;
      if (nm == "title") {
        std::string dec;
        append_decoded(dec, std::string_view(p + i, close - i));
        for (char c : dec)
          if (!is_ws(c) || (!title.empty() && title.back() != ' ')) title.push_back(is_ws(c) ? ' ' : c);
      }
      const HtmlTag ct = close < n ? scan_tag(p, n, close) : HtmlTag{};
      i = ct.kind == 2 ? ct.end : close;
      continue;
    }
    if (nm == "a") {
      if (t.kind == 1) {
        href = attr_value(tag, "href");
        link_start = line.size();
      } else if (!href.empty()) {
        const std::string label = line.substr(std::min(link_start, line.size()));
        if (label != href && href.rfind("#", 0) != 0 && href.rfind("javascript:", 0) != 0) {
          if (space || (!line.empty() && line.back() != ' ')) line.push_back(' ');
          space = false;
          line += "<" + href + ">";
        }
        href.clear();
      }
      continue;
    }
    if (nm == "img" && t.kind == 1) {
      const std::string alt = attr_value(tag, "alt");
      if (!alt.empty()) text("[" + alt + "]");
      continue;
    }
    if (nm == "br") {
      end_line(false);
      continue;
    }
    const bool heading = nm.size() == 2 && nm[0] == 'h' && nm[1] >= '1' && nm[1] <= '6';
    if (heading || nm == "p" || nm == "div" || nm == "section" || nm == "article" || nm == "header" ||
        nm == "footer" || nm == "main" || nm == "nav" || nm == "aside" || nm == "ul" || nm == "ol" || nm == "table" ||
        nm == "blockquote" || nm == "pre" || nm == "form" || nm == "hr" || nm == "dl" || nm == "figure" ||
        nm == "body") {
      end_line(true);
      if (heading && t.kind == 1) {
        line.assign(static_cast<size_t>(nm[1] - '0'), '#');
        line.push_back(' ');
      }
      if (nm == "hr") {
        out.append("----------\n\n");
        blank = 2;
      }
      continue;
    }
    if (nm == "li" || nm == "tr" || nm == "dt" || nm == "dd" || nm == "option") {
      end_line(false);
      if (t.kind == 1 && nm == "li") line = "\xE2\x80\xA2 ";  // bullet
      if (t.kind == 1 && nm == "dd") line = "    ";
      continue;
    }
    if ((nm == "td" || nm == "th") && t.kind == 1 && !line.empty()) {
      line += "  |  ";
      space = false;
    }
  }
  end_line(false);
  while (!out.empty() && (out.back() == '\n')) out.pop_back();
  if (!title.empty()) {
    while (!title.empty() && title.back() == ' ') title.pop_back();
    out.insert(0, "# " + title + "\n\n");
  }
  out.push_back('\n');
}

bool compact_json(std::string_view in, std::string& out, size_t max) {
  out.clear();
  for (size_t i = 0; i < in.size(); ++i) {
    const char c = in[i];
    if (c == '"') {
      const size_t s = i++;
      while (i < in.size() && in[i] != '"') i += in[i] == '\\' ? 2 : 1;
      if (i >= in.size()) return false;
      out.append(in.substr(s, i - s + 1));
    } else if (is_ws(c)) {
      continue;
    } else {
      out.push_back(c);
      if (c == ',' || c == ':') out.push_back(' ');
    }
    if (out.size() > max) return false;
  }
  return !out.empty();
}

size_t longest_line_chars(std::string_view text, size_t limit) {
  const size_t n = std::min(text.size(), limit);
  size_t best = 0, cur = 0;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c == '\n') {
      best = std::max(best, cur);
      cur = 0;
    } else if (c == '\t') {
      cur += 4;
    } else if ((c & 0xC0) != 0x80) {
      ++cur;
    }
  }
  return std::max(best, cur);
}

void hex_dump(std::string_view in, std::string& out, size_t max_bytes, bool* truncated) {
  static const char kHex[] = "0123456789abcdef";
  const size_t n = std::min(in.size(), max_bytes);
  if (truncated) *truncated = in.size() > max_bytes;
  constexpr size_t kLine = 8 + 2 + 16 * 3 + 1 + 2 + 16 + 1 + 1;  // 79
  out.assign(((n + 15) / 16) * kLine, ' ');
  char* o = out.data();
  const auto* b = reinterpret_cast<const unsigned char*>(in.data());
  for (size_t off = 0; off < n; off += 16) {
    char* line = o;
    uint32_t a = static_cast<uint32_t>(off);
    for (int k = 7; k >= 0; --k) {
      line[k] = kHex[a & 15];
      a >>= 4;
    }
    const size_t cnt = std::min<size_t>(16, n - off);
    char* hx = line + 10;
    char* asc = line + 10 + 16 * 3 + 1 + 2;
    asc[-1] = '|';
    for (size_t k = 0; k < cnt; ++k) {
      const unsigned char c = b[off + k];
      char* h = hx + k * 3 + (k >= 8 ? 1 : 0);
      h[0] = kHex[c >> 4];
      h[1] = kHex[c & 15];
      asc[k] = (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '.';
    }
    asc[cnt] = '|';
    line[kLine - 1] = '\n';
    // Trim the unused tail of a short last line.
    if (cnt < 16) {
      out.resize(static_cast<size_t>(asc + cnt + 1 - out.data()));
      out.push_back('\n');
      return;
    }
    o += kLine;
  }
}

void style_json(std::string_view text, std::string& styles, std::vector<int>& levels) {
  namespace js = json_style;
  constexpr int kBase = 0x400, kHeader = 0x2000;
  const size_t n = text.size();
  const char* p = text.data();
  styles.assign(n, js::Default);
  char* st = styles.data();
  levels.clear();
  levels.reserve(n / 12 + 16);
  int depth = 0;
  int line_depth = 0;       // depth at the start of the current line
  bool line_opens = false;  // the current line opened a container
  auto end_line = [&] {
    levels.push_back((kBase + line_depth) | ((line_opens && depth > line_depth) ? kHeader : 0));
    line_depth = depth;
    line_opens = false;
  };
  auto is_delim = [](char d) {
    return d == ' ' || d == '\n' || d == '\r' || d == '\t' || d == ',' || d == ':' || d == '{' || d == '}' ||
           d == '[' || d == ']' || d == '"';
  };
  size_t i = 0;
  while (i < n) {
    const char c = p[i];
    switch (c) {
      case '\n':
        end_line();
        ++i;
        break;
      case ' ': case '\t': case '\r':
        ++i;
        break;
      case '{': case '[':
        st[i++] = js::Operator;
        ++depth;
        line_opens = true;
        break;
      case '}': case ']':
        st[i++] = js::Operator;
        if (depth > 0) --depth;
        break;
      case ',': case ':':
        st[i++] = js::Operator;
        break;
      case '"': {
        const size_t start = i++;
        bool esc_seen = false;
        while (i < n && p[i] != '"' && p[i] != '\n') {
          if (p[i] == '\\') {
            esc_seen = true;
            i += 2;
          } else {
            ++i;
          }
        }
        if (i > n) i = n;
        const bool closed = i < n && p[i] == '"';
        if (closed) ++i;
        // A string followed by ':' is a property name.
        size_t j = i;
        while (j < n && (p[j] == ' ' || p[j] == '\t')) ++j;
        const char sty = (closed && j < n && p[j] == ':') ? js::PropertyName : js::String;
        std::memset(st + start, sty, i - start);
        if (esc_seen && sty == js::String)
          for (size_t k = start + 1; k + 1 < i; ++k)
            if (p[k] == '\\') {
              const size_t len = (p[k + 1] == 'u') ? 6 : 2;
              std::memset(st + k, js::Escape, std::min(len, i - 1 - k));
              k += len - 1;
            }
        break;
      }
      case '/':
        if (i + 1 < n && p[i + 1] == '/') {
          const size_t start = i;
          while (i < n && p[i] != '\n') ++i;
          std::memset(st + start, js::LineComment, i - start);
        } else if (i + 1 < n && p[i + 1] == '*') {
          const size_t start = i;
          i += 2;
          while (i + 1 < n && !(p[i] == '*' && p[i + 1] == '/')) ++i;
          i = std::min(n, i + 2);
          std::memset(st + start, js::BlockComment, i - start);
          for (size_t k = start; k < i; ++k)
            if (p[k] == '\n') end_line();
        } else {
          st[i++] = js::Error;
        }
        break;
      default: {
        const size_t start = i;
        while (i < n && !is_delim(p[i])) ++i;
        const std::string_view tok(p + start, i - start);
        char sty = js::Error;
        if (tok == "true" || tok == "false" || tok == "null") sty = js::Keyword;
        else if ((tok[0] >= '0' && tok[0] <= '9') || tok[0] == '-') sty = js::Number;
        std::memset(st + start, sty, i - start);
        break;
      }
    }
  }
  end_line();  // the last line (Scintilla always has one more line than newlines)
}

void html_for_preview(std::string_view in, std::string& out) {
  static const char* const kDrop[] = {"script", "style", "noscript", "iframe", "object", "embed", "template", "svg"};
  auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
  // Case-insensitive: does `in` at i start with tag name t followed by a delimiter?
  auto name_at = [&](size_t i, std::string_view t) {
    if (i + t.size() > in.size()) return false;
    for (size_t k = 0; k < t.size(); ++k)
      if (lower(in[i + k]) != t[k]) return false;
    const size_t e = i + t.size();
    return e == in.size() || in[e] == '>' || in[e] == '/' || in[e] == ' ' || in[e] == '\t' || in[e] == '\n' ||
           in[e] == '\r';
  };
  out.clear();
  out.reserve(in.size());
  size_t i = 0;
  while (i < in.size()) {
    const size_t lt = in.find('<', i);
    if (lt == std::string_view::npos) {
      out.append(in.substr(i));
      break;
    }
    out.append(in.substr(i, lt - i));
    const char* drop = nullptr;
    for (const char* t : kDrop)
      if (name_at(lt + 1, t)) drop = t;
    if (!drop) {
      out += '<';
      i = lt + 1;
      continue;
    }
    // Skip to the matching </name ...> (or the end of a self-closing tag / the input).
    const std::string_view t = drop;
    size_t gt = in.find('>', lt);
    if (gt == std::string_view::npos) break;
    if (in[gt - 1] == '/') {
      i = gt + 1;
      continue;
    }
    size_t j = gt + 1;
    for (;;) {
      j = in.find("</", j);
      if (j == std::string_view::npos || name_at(j + 2, t)) break;
      j += 2;
    }
    if (j == std::string_view::npos) break;
    gt = in.find('>', j);
    i = gt == std::string_view::npos ? in.size() : gt + 1;
  }
}

size_t fold_outline(std::string_view pretty, const std::vector<int>& levels, std::string& out, std::vector<int>& map,
                    size_t max_line) {
  constexpr int kBase = 0x400, kHeader = 0x2000, kMask = 0xFFF;
  out.clear();
  map.clear();
  out.reserve(pretty.size() / 3 + 64);
  size_t folded = 0;
  size_t pos = 0;
  const size_t nlines = levels.size();
  auto next_line = [&](size_t& p) {
    const size_t e = pretty.find('\n', p);
    const std::string_view l = pretty.substr(p, (e == std::string_view::npos ? pretty.size() : e) - p);
    p = e == std::string_view::npos ? pretty.size() : e + 1;
    return l;
  };
  auto trim = [](std::string_view l) {
    while (!l.empty() && (l.front() == ' ' || l.front() == '\t')) l.remove_prefix(1);
    while (!l.empty() && (l.back() == ' ' || l.back() == '\r')) l.remove_suffix(1);
    return l;
  };
  size_t i = 0;
  while (i < nlines && pos <= pretty.size()) {
    const int lv = levels[i];
    std::string_view line = next_line(pos);
    map.push_back(static_cast<int>(i));
    if (!(lv & kHeader) || (lv & kMask) != kBase + 1) {
      out += line;
      out += '\n';
      ++i;
      continue;
    }
    // A value one level below the root: its lines up to the next line back at that level.
    const size_t start = out.size();
    const size_t ind = line.find_first_not_of(" \t");
    out += line.substr(0, ind == std::string_view::npos ? line.size() : ind);
    out += trim(line);
    bool cut = false;
    ++i;
    while (i < nlines && (levels[i] & kMask) > kBase + 1) {
      const std::string_view t = trim(next_line(pos));
      ++i;
      if (cut || t.empty()) continue;
      const char prev = out.empty() ? 0 : out.back();
      if (prev != '{' && prev != '[' && t.front() != '}' && t.front() != ']') out += ' ';
      out += t;
      if (out.size() - start > max_line) {
        out.resize(start + max_line);
        while (out.size() > start && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) out.pop_back();
        if (out.size() > start) out.pop_back();  // a whole UTF-8 sequence's lead byte
        out += " \xE2\x80\xA6";
        cut = true;
      }
    }
    out += '\n';
    ++folded;
  }
  if (!out.empty() && out.back() == '\n') out.pop_back();  // the last line has no newline of its own
  return folded;
}

void style_hex(std::string_view text, std::string& styles) {
  styles.assign(text.size(), 0);
  size_t line = 0;
  while (line < text.size()) {
    size_t e = text.find('\n', line);
    if (e == std::string_view::npos) e = text.size();
    std::memset(styles.data() + line, 1, std::min<size_t>(8, e - line));
    const size_t bar = text.find('|', line);
    if (bar != std::string_view::npos && bar < e) std::memset(styles.data() + bar, 2, e - bar);
    line = e + 1;
  }
}

size_t longest_line(std::string_view text) {
  size_t best = 0;
  const char* p = text.data();
  const char* end = p + text.size();
  while (p < end) {
    const void* nl = std::memchr(p, '\n', static_cast<size_t>(end - p));
    const char* e = nl ? static_cast<const char*>(nl) : end;
    best = std::max(best, static_cast<size_t>(e - p));
    p = e + 1;
  }
  return best;
}

size_t break_long_lines(std::string_view text, std::string& out, size_t max) {
  out.clear();
  out.reserve(text.size() + text.size() / max + 16);
  size_t breaks = 0;
  const char* p = text.data();
  const char* end = p + text.size();
  while (p < end) {
    const void* nl = std::memchr(p, '\n', static_cast<size_t>(end - p));
    const char* e = nl ? static_cast<const char*>(nl) : end;
    while (static_cast<size_t>(e - p) > max) {
      // Break after the last separator in the final 1 KB before the limit.
      // Prefer "}," / "]," (between JSON values, never inside a string), then any separator.
      const char* cut = nullptr;
      // Search window: the final 1 KB, or the last third of a short limit (wrap at pane width).
      const char* lo = p + max - std::min<size_t>(1024, max / 3);
      if (lo < p + 2) lo = p + 2;
      for (const char* q = p + max; q > lo && !cut; --q)
        if (q[-1] == ',' && (q[-2] == '}' || q[-2] == ']')) cut = q;
      for (const char* q = p + max; q > lo && !cut; --q) {
        const char c = q[-1];
        if (c == ',' || c == ' ' || c == '>' || c == ';') cut = q;
      }
      if (!cut) cut = p + max;
      // Never split a UTF-8 sequence.
      while (cut > p + 1 && (static_cast<unsigned char>(*cut) & 0xC0) == 0x80) --cut;
      out.append(p, static_cast<size_t>(cut - p));
      out.push_back('\n');
      ++breaks;
      p = cut;
    }
    out.append(p, static_cast<size_t>(e - p));
    if (e < end) out.push_back('\n');
    p = e + 1;
  }
  return breaks;
}

std::string human_bytes(uint64_t b) {
  char buf[32];
  if (b < 1024) std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(b));
  else if (b < 1024 * 1024) std::snprintf(buf, sizeof buf, "%.1f KB", static_cast<double>(b) / 1024.0);
  else std::snprintf(buf, sizeof buf, "%.2f MB", static_cast<double>(b) / (1024.0 * 1024.0));
  return buf;
}

std::string ms(double v) {
  char buf[32];
  const double a = std::fabs(v);
  if (a < 1) std::snprintf(buf, sizeof buf, "%.3f ms", v);
  else if (a < 100) std::snprintf(buf, sizeof buf, "%.2f ms", v);
  else if (a < 1000) std::snprintf(buf, sizeof buf, "%.1f ms", v);
  else std::snprintf(buf, sizeof buf, "%.0f ms", v);
  return buf;
}

std::string delta_ms(double v) {
  if (std::fabs(v) < 0.0005) return "\xC2\xB1" "0";  // plus-minus zero
  std::string s = ms(v);
  if (v >= 0) s.insert(s.begin(), '+');
  return s;
}

}  // namespace watt::app::fmt
