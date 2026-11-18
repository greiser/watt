#include "app/response_visualize.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace watt::app::rv {

// Own namespace: the unit tests compile this file together with other response_*.cpp files.
namespace hbs {
namespace {

using JV = rapidjson::Value;

// ------------------------------------------------------------------ template tokens

enum class TK { Text, Var, Open, Inverted, Close, Else, Comment, Partial };

struct Tok {
  TK k = TK::Text;
  std::string text;  // Text: the text; tags: the expression (without the sigil)
  bool raw = false;  // Var: {{{x}}} / {{& x}}
  bool strip_l = false, strip_r = false;
  size_t pos = 0;
};

constexpr int kMaxNesting = 100;
constexpr size_t kMaxWarnings = 5;

bool ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

std::string_view trim(std::string_view s) {
  while (!s.empty() && ws(s.front())) s.remove_prefix(1);
  while (!s.empty() && ws(s.back())) s.remove_suffix(1);
  return s;
}

int line_at(std::string_view tpl, size_t pos) {
  return 1 + static_cast<int>(std::count(tpl.begin(), tpl.begin() + static_cast<ptrdiff_t>(std::min(pos, tpl.size())), '\n'));
}

bool tokenize(std::string_view tpl, std::vector<Tok>& out, std::string& err) {
  std::string text;
  size_t text_pos = 0;
  auto flush = [&] {
    if (text.empty()) return;
    Tok t;
    t.k = TK::Text;
    t.text = std::move(text);
    t.pos = text_pos;
    out.push_back(std::move(t));
    text.clear();
  };
  size_t i = 0;
  const size_t n = tpl.size();
  while (i < n) {
    if (tpl[i] == '\\' && tpl.substr(i + 1, 2) == "{{") {  // \{{ is a literal "{{"
      if (text.empty()) text_pos = i;
      text += "{{";
      i += 3;
      continue;
    }
    if (tpl.substr(i, 2) != "{{") {
      if (text.empty()) text_pos = i;
      const size_t next = tpl.find_first_of("\\{", i + 1);
      const size_t end = next == std::string_view::npos ? n : next;
      text.append(tpl.substr(i, end - i));
      i = end;
      continue;
    }
    flush();
    Tok t;
    t.pos = i;
    size_t j = i + 2;
    const bool triple = j < n && tpl[j] == '{';
    if (triple) ++j;
    if (j < n && tpl[j] == '~') {
      t.strip_l = true;
      ++j;
    }
    size_t end, close_len;
    if (!triple && tpl.substr(j, 3) == "!--") {
      end = tpl.find("--", j + 3);
      for (;;) {
        if (end == std::string_view::npos) break;
        size_t q = end + 2;
        if (q < n && tpl[q] == '~') ++q;
        if (tpl.substr(q, 2) == "}}") break;
        end = tpl.find("--", end + 1);
      }
      if (end == std::string_view::npos) {
        err = "line " + std::to_string(line_at(tpl, i)) + ": {{!-- comment is never closed (--}})";
        return false;
      }
      size_t q = end + 2;
      if (q < n && tpl[q] == '~') {
        t.strip_r = true;
        ++q;
      }
      t.k = TK::Comment;
      out.push_back(std::move(t));
      i = q + 2;
      continue;
    }
    end = tpl.find(triple ? "}}}" : "}}", j);
    close_len = triple ? 3 : 2;
    if (end == std::string_view::npos) {
      err = "line " + std::to_string(line_at(tpl, i)) + ": \"{{" + std::string(triple ? "{" : "") +
            "\" is never closed";
      return false;
    }
    std::string_view inner = tpl.substr(j, end - j);
    if (!inner.empty() && inner.back() == '~') {
      t.strip_r = true;
      inner.remove_suffix(1);
    }
    i = end + close_len;
    inner = trim(inner);
    if (triple) {
      t.k = TK::Var;
      t.raw = true;
      t.text = std::string(inner);
    } else if (!inner.empty() && inner[0] == '!') {
      t.k = TK::Comment;
    } else if (!inner.empty() && inner[0] == '#') {
      t.k = TK::Open;
      t.text = std::string(trim(inner.substr(1)));
      if (!t.text.empty() && (t.text[0] == '>' || t.text[0] == '*')) t.k = TK::Partial;
    } else if (!inner.empty() && inner[0] == '/') {
      t.k = TK::Close;
      t.text = std::string(trim(inner.substr(1)));
    } else if (!inner.empty() && inner[0] == '^') {
      const std::string_view rest = trim(inner.substr(1));
      t.k = rest.empty() ? TK::Else : TK::Inverted;
      t.text = std::string(rest);
    } else if (!inner.empty() && inner[0] == '>') {
      t.k = TK::Partial;
      t.text = std::string(trim(inner.substr(1)));
    } else if (!inner.empty() && inner[0] == '&') {
      t.k = TK::Var;
      t.raw = true;
      t.text = std::string(trim(inner.substr(1)));
    } else if (inner == "else" || (inner.size() > 5 && inner.substr(0, 4) == "else" && ws(inner[4]))) {
      t.k = TK::Else;
      t.text = std::string(trim(inner.substr(4)));
    } else {
      t.k = TK::Var;
      t.text = std::string(inner);
    }
    out.push_back(std::move(t));
  }
  flush();
  // Whitespace control: {{~ trims the text before the tag, ~}} the text after it.
  for (size_t k = 0; k < out.size(); ++k) {
    if (out[k].k == TK::Text) continue;
    if (out[k].strip_l && k > 0 && out[k - 1].k == TK::Text) {
      std::string& s = out[k - 1].text;
      while (!s.empty() && ws(s.back())) s.pop_back();
    }
    if (out[k].strip_r && k + 1 < out.size() && out[k + 1].k == TK::Text) {
      std::string& s = out[k + 1].text;
      size_t a = 0;
      while (a < s.size() && ws(s[a])) ++a;
      s.erase(0, a);
    }
  }
  return true;
}

// ------------------------------------------------------------------ expressions

struct Val {
  enum class T { Undef, Json, Num, Str, Bool } t = T::Undef;
  const JV* j = nullptr;
  double n = 0;
  std::string s;
  bool b = false;
  static Val json(const JV* v) {
    Val x;
    if (v) {
      x.t = T::Json;
      x.j = v;
    }
    return x;
  }
  static Val num(double d) {
    Val x;
    x.t = T::Num;
    x.n = d;
    return x;
  }
  static Val str(std::string v) {
    Val x;
    x.t = T::Str;
    x.s = std::move(v);
    return x;
  }
  static Val boolean(bool v) {
    Val x;
    x.t = T::Bool;
    x.b = v;
    return x;
  }
};

struct Arg {
  bool literal = false;
  Val lit;
  std::string path;
};

struct Expr {
  std::vector<Arg> args;  // args[0]: the helper name / path
  std::vector<std::string> bparams;
  bool subexpr = false;
};

bool parse_expr(std::string_view s, Expr& e) {
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    while (i < n && ws(s[i])) ++i;
    if (i >= n) break;
    const char c = s[i];
    if (c == '"' || c == '\'') {
      size_t k = i + 1;
      std::string v;
      while (k < n && s[k] != c) {
        if (s[k] == '\\' && k + 1 < n) ++k;
        v += s[k++];
      }
      Arg a;
      a.literal = true;
      a.lit = Val::str(std::move(v));
      e.args.push_back(std::move(a));
      i = k < n ? k + 1 : n;
      continue;
    }
    if (c == '(') {
      e.subexpr = true;
      int depth = 0;
      while (i < n) {
        if (s[i] == '(') ++depth;
        else if (s[i] == ')' && --depth == 0) {
          ++i;
          break;
        }
        ++i;
      }
      continue;
    }
    if (c == '|') {
      const size_t close = s.find('|', i + 1);
      std::string_view names = s.substr(i + 1, close == std::string_view::npos ? std::string_view::npos : close - i - 1);
      size_t p = 0;
      while (p < names.size()) {
        while (p < names.size() && ws(names[p])) ++p;
        size_t q = p;
        while (q < names.size() && !ws(names[q])) ++q;
        if (q > p) e.bparams.emplace_back(names.substr(p, q - p));
        p = q;
      }
      i = close == std::string_view::npos ? n : close + 1;
      continue;
    }
    // A path / literal / hash pair: up to whitespace, but [..] segments may hold spaces.
    size_t k = i;
    while (k < n && !ws(s[k])) {
      if (s[k] == '[') {
        const size_t close = s.find(']', k);
        k = close == std::string_view::npos ? n : close + 1;
      } else {
        ++k;
      }
    }
    std::string_view tok = s.substr(i, k - i);
    i = k;
    if (tok == "as") continue;  // "as |x i|"
    if (const size_t eq = tok.find('='); eq != std::string_view::npos && eq > 0 && tok[0] != '[') {
      // hash argument (includeZero=true, ...): accepted, not used. A quoted value may follow.
      if (eq + 1 == tok.size() && i < n) {
        while (i < n && ws(s[i])) ++i;
        if (i < n && (s[i] == '"' || s[i] == '\'')) {
          const size_t close = s.find(s[i], i + 1);
          i = close == std::string_view::npos ? n : close + 1;
        }
      }
      continue;
    }
    Arg a;
    if (tok == "true" || tok == "false") {
      a.literal = true;
      a.lit = Val::boolean(tok == "true");
    } else if (tok == "null" || tok == "undefined") {
      a.literal = true;
    } else if ((tok[0] >= '0' && tok[0] <= '9') || (tok[0] == '-' && tok.size() > 1)) {
      char* end = nullptr;
      const std::string t(tok);
      const double d = std::strtod(t.c_str(), &end);
      if (end && *end == 0) {
        a.literal = true;
        a.lit = Val::num(d);
      } else {
        a.path = t;
      }
    } else {
      a.path = std::string(tok);
    }
    e.args.push_back(std::move(a));
  }
  return !e.args.empty();
}

// ------------------------------------------------------------------ tree

struct Node {
  enum class K { Text, Var, Block } k = K::Text;
  std::string text;  // Text
  bool raw = false;
  bool inverted = false;  // {{^name}}
  Expr e;
  std::string name;  // Block: helper name / section path
  std::vector<Node> body, inverse;
};

class Parser {
 public:
  Parser(std::string_view tpl, std::vector<Tok>& t, std::vector<std::string>& warn) : tpl_(tpl), t_(t), warn_(warn) {}
  bool run(std::vector<Node>& out, std::string& err) {
    size_t p = 0;
    if (!list(p, out, 0)) {
      err = err_;
      return false;
    }
    if (p < t_.size()) {
      const Tok& s = t_[p];
      err = at(s) + (s.k == TK::Close ? "{{/" + s.text + "}} closes no open block" : "{{else}} outside a block");
      return false;
    }
    return true;
  }

 private:
  std::string at(const Tok& t) const { return "line " + std::to_string(line_at(tpl_, t.pos)) + ": "; }
  void warn(const Tok& t, const std::string& w) {
    if (warn_.size() < kMaxWarnings) warn_.push_back(at(t) + w);
  }
  bool fail(const Tok& t, const std::string& m) {
    if (err_.empty()) err_ = at(t) + m;
    return false;
  }
  // Nodes up to the end, or an {{else}} / {{/x}} at this level (not consumed).
  bool list(size_t& p, std::vector<Node>& out, int depth) {
    while (p < t_.size()) {
      Tok& t = t_[p];
      switch (t.k) {
        case TK::Text: {
          Node x;
          x.text = std::move(t.text);
          out.push_back(std::move(x));
          ++p;
          break;
        }
        case TK::Comment: ++p; break;
        case TK::Partial:
          warn(t, "partials and decorators are not available ({{>" + t.text.substr(0, 40) + "}})");
          ++p;
          break;
        case TK::Var: {
          Node x;
          x.k = Node::K::Var;
          x.raw = t.raw;
          if (!parse_expr(t.text, x.e)) return fail(t, "empty {{ }}");
          if (x.e.subexpr) warn(t, "sub-expressions ( ... ) are not available");
          out.push_back(std::move(x));
          ++p;
          break;
        }
        case TK::Close:
        case TK::Else: return true;
        case TK::Open:
        case TK::Inverted: {
          if (depth >= kMaxNesting) return fail(t, "blocks nested more than " + std::to_string(kMaxNesting) + " deep");
          Node b;
          b.k = Node::K::Block;
          b.inverted = t.k == TK::Inverted;
          if (!parse_expr(t.text, b.e) || b.e.args[0].literal) return fail(t, "a block needs a helper or a name");
          if (b.e.subexpr) warn(t, "sub-expressions ( ... ) are not available");
          b.name = b.e.args[0].path;
          const size_t open = p++;
          if (!block(p, b, b.name, open, depth + 1)) return false;
          out.push_back(std::move(b));
          break;
        }
      }
    }
    return true;
  }
  bool block(size_t& p, Node& b, const std::string& name, size_t open, int depth) {
    if (!list(p, b.body, depth)) return false;
    if (p >= t_.size()) return fail(t_[open], "{{#" + name + "}} is never closed");
    if (t_[p].k == TK::Else) {
      Tok& el = t_[p++];
      if (!trim(el.text).empty()) {  // {{else if x}} / {{else each y}}: a chained block
        Node c;
        c.k = Node::K::Block;
        if (!parse_expr(el.text, c.e) || c.e.args[0].literal) return fail(el, "bad {{else ...}}");
        c.name = c.e.args[0].path;
        if (depth >= kMaxNesting) return fail(el, "too many {{else if}} in a row");
        if (!block(p, c, name, open, depth + 1)) return false;
        b.inverse.push_back(std::move(c));
        return true;  // the chain's last block consumed the {{/name}}
      }
      if (!list(p, b.inverse, depth)) return false;
      if (p >= t_.size()) return fail(t_[open], "{{#" + name + "}} is never closed");
      if (t_[p].k == TK::Else) return fail(t_[p], "a second {{else}} in {{#" + name + "}}");
    }
    const Tok& c = t_[p];
    if (c.text != name) return fail(c, "{{/" + c.text + "}} does not close {{#" + name + "}}");
    ++p;
    return true;
  }

  std::string_view tpl_;
  std::vector<Tok>& t_;
  std::vector<std::string>& warn_;
  std::string err_;
};

// ------------------------------------------------------------------ rendering

void put_number(std::string& out, double d) {
  if (std::isnan(d)) {
    out += "NaN";
    return;
  }
  if (std::isinf(d)) {
    out += d > 0 ? "Infinity" : "-Infinity";
    return;
  }
  if (d == std::floor(d) && std::fabs(d) < 1e21) {
    char b[32];
    std::snprintf(b, sizeof b, "%.0f", d);
    out += std::string_view(b) == "-0" ? "0" : b;
    return;
  }
  char b[64];
  const auto r = std::to_chars(b, b + sizeof b, d);
  out.append(b, r.ptr);
}

void json_text(std::string& out, const JV& v, int depth) {
  if (v.IsString()) out.append(v.GetString(), v.GetStringLength());
  else if (v.IsBool()) out += v.GetBool() ? "true" : "false";
  else if (v.IsInt64()) out += std::to_string(v.GetInt64());
  else if (v.IsUint64()) out += std::to_string(v.GetUint64());
  else if (v.IsNumber()) put_number(out, v.GetDouble());
  else if (v.IsObject()) out += "[object Object]";
  else if (v.IsArray()) {  // like JavaScript's Array.prototype.toString
    if (depth > 32) return;
    bool first = true;
    for (const JV& x : v.GetArray()) {
      if (!first) out += ',';
      first = false;
      if (!x.IsNull()) json_text(out, x, depth + 1);
    }
  }
}

void val_text(std::string& out, const Val& v) {
  switch (v.t) {
    case Val::T::Undef: break;
    case Val::T::Json: json_text(out, *v.j, 0); break;
    case Val::T::Num: put_number(out, v.n); break;
    case Val::T::Str: out += v.s; break;
    case Val::T::Bool: out += v.b ? "true" : "false"; break;
  }
}

bool truthy(const Val& v) {
  switch (v.t) {
    case Val::T::Undef: return false;
    case Val::T::Num: return v.n != 0 && !std::isnan(v.n);
    case Val::T::Str: return !v.s.empty();
    case Val::T::Bool: return v.b;
    case Val::T::Json: {
      const JV& j = *v.j;
      if (j.IsNull()) return false;
      if (j.IsBool()) return j.GetBool();
      if (j.IsNumber()) return j.GetDouble() != 0;
      if (j.IsString()) return j.GetStringLength() > 0;
      if (j.IsArray()) return !j.Empty();
      return true;
    }
  }
  return false;
}

void escape_into(std::string& out, std::string_view s) {
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#x27;"; break;
      case '`': out += "&#x60;"; break;
      case '=': out += "&#x3D;"; break;
      default: out += c;
    }
  }
}

struct Frame {
  Val ctx;
  bool has_data = false;
  size_t index = 0;
  bool has_key = false;
  std::string key;
  bool first = false, last = false;
  std::vector<std::pair<std::string, Val>> bparams;
};

// Path segments: "a.b", "a/b", "a.[x y]", "[0]".
std::vector<std::string> split_path(std::string_view p) {
  std::vector<std::string> segs;
  size_t i = 0;
  while (i < p.size()) {
    if (p[i] == '.' || p[i] == '/') {
      ++i;
      continue;
    }
    if (p[i] == '[') {
      const size_t close = p.find(']', i);
      segs.emplace_back(p.substr(i + 1, close == std::string_view::npos ? std::string_view::npos : close - i - 1));
      i = close == std::string_view::npos ? p.size() : close + 1;
      continue;
    }
    size_t k = i;
    while (k < p.size() && p[k] != '.' && p[k] != '/' && p[k] != '[') ++k;
    segs.emplace_back(p.substr(i, k - i));
    i = k;
  }
  return segs;
}

Val member(const Val& base, const std::string& seg) {
  if (base.t == Val::T::Str) return seg == "length" ? Val::num(static_cast<double>(base.s.size())) : Val{};
  if (base.t != Val::T::Json) return {};
  const JV& j = *base.j;
  if (j.IsObject()) {
    const auto it = j.FindMember(rapidjson::StringRef(seg.data(), seg.size()));
    return it == j.MemberEnd() ? Val{} : Val::json(&it->value);
  }
  if (j.IsArray()) {
    if (seg == "length") return Val::num(static_cast<double>(j.Size()));
    if (seg.empty() || seg.size() > 9 || seg.find_first_not_of("0123456789") != std::string::npos) return {};
    const unsigned long idx = std::strtoul(seg.c_str(), nullptr, 10);
    return idx < j.Size() ? Val::json(&j[static_cast<rapidjson::SizeType>(idx)]) : Val{};
  }
  if (j.IsString() && seg == "length") return Val::num(static_cast<double>(j.GetStringLength()));
  return {};
}

class Renderer {
 public:
  Renderer(const JV& root, size_t max_out, std::vector<std::string>& warn) : root_(root), max_(max_out), warn_(warn) {
    Frame f;
    f.ctx = Val::json(&root);
    frames_.push_back(std::move(f));
  }
  void render(const std::vector<Node>& nodes) {
    for (const Node& n : nodes) {
      if (full()) return;
      switch (n.k) {
        case Node::K::Text: out_ += n.text; break;
        case Node::K::Var: var(n); break;
        case Node::K::Block: block(n); break;
      }
    }
  }
  std::string out_;
  bool truncated_ = false;

 private:
  bool full() {
    if (out_.size() < max_) return false;
    truncated_ = true;
    return true;
  }
  void warn(const std::string& w) {
    if (warn_.size() >= kMaxWarnings) return;
    if (std::find(warn_.begin(), warn_.end(), w) == warn_.end()) warn_.push_back(w);
  }

  Val value_of(const Arg& a) const { return a.literal ? a.lit : resolve(a.path); }

  Val resolve(std::string_view p) const {
    if (p.empty()) return {};
    size_t up = 0;
    if (p[0] == '@') {
      p.remove_prefix(1);
      while (p.substr(0, 3) == "../") {
        ++up;
        p.remove_prefix(3);
      }
      std::vector<std::string> segs = split_path(p);
      if (segs.empty()) return {};
      Val base;
      if (segs[0] == "root") {
        base = Val::json(&root_);
      } else {
        const ptrdiff_t top = static_cast<ptrdiff_t>(frames_.size()) - 1 - static_cast<ptrdiff_t>(up);
        const Frame* f = nullptr;
        for (ptrdiff_t i = top; i >= 0 && !f; --i)
          if (frames_[static_cast<size_t>(i)].has_data) f = &frames_[static_cast<size_t>(i)];
        if (!f) return {};
        if (segs[0] == "index") base = Val::num(static_cast<double>(f->index));
        else if (segs[0] == "key") base = f->has_key ? Val::str(f->key) : Val::num(static_cast<double>(f->index));
        else if (segs[0] == "first") base = Val::boolean(f->first);
        else if (segs[0] == "last") base = Val::boolean(f->last);
        else return {};
      }
      for (size_t i = 1; i < segs.size() && base.t != Val::T::Undef; ++i) base = member(base, segs[i]);
      return base;
    }
    while (p.substr(0, 3) == "../") {
      ++up;
      p.remove_prefix(3);
    }
    if (p == "..") {
      ++up;
      p = {};
    }
    const size_t fi = up < frames_.size() ? frames_.size() - 1 - up : 0;
    std::vector<std::string> segs = split_path(p);
    Val base = frames_[fi].ctx;
    size_t s0 = 0;
    if (!segs.empty() && (segs[0] == "this" || segs[0] == ".")) {
      s0 = 1;
    } else if (p.substr(0, 2) == "./") {
      // "./name": the context's own member (never a block param)
    } else if (!segs.empty() && up == 0) {
      for (size_t i = frames_.size(); i-- > 0;) {
        bool hit = false;
        for (const auto& [name, v] : frames_[i].bparams)
          if (name == segs[0]) {
            base = v;
            hit = true;
            break;
          }
        if (hit) {
          s0 = 1;
          break;
        }
      }
    }
    for (size_t i = s0; i < segs.size() && base.t != Val::T::Undef; ++i) base = member(base, segs[i]);
    return base;
  }

  void var(const Node& n) {
    const Expr& e = n.e;
    Val v;
    if (e.args.size() == 1) {
      v = value_of(e.args[0]);
    } else {
      const std::string& h = e.args[0].literal ? std::string() : e.args[0].path;
      if (h == "lookup" && e.args.size() >= 3) {
        const Val obj = value_of(e.args[1]);
        std::string key;
        val_text(key, value_of(e.args[2]));
        v = member(obj, key);
      } else if (h == "log") {
        return;
      } else {
        warn("helper \"" + h.substr(0, 40) + "\" is not available (rendered as nothing)");
        return;
      }
    }
    if (n.raw) {
      val_text(out_, v);
    } else {
      std::string t;
      val_text(t, v);
      escape_into(out_, t);
    }
  }

  void each(const Node& n, const Val& v, bool use_bparams) {
    if (v.t == Val::T::Json && v.j->IsArray() && !v.j->Empty()) {
      const auto a = v.j->GetArray();
      const size_t sz = a.Size();
      for (size_t i = 0; i < sz && !full(); ++i) {
        Frame f;
        f.ctx = Val::json(&a[static_cast<rapidjson::SizeType>(i)]);
        f.has_data = true;
        f.index = i;
        f.first = i == 0;
        f.last = i + 1 == sz;
        if (use_bparams) {
          if (n.e.bparams.size() > 0) f.bparams.emplace_back(n.e.bparams[0], f.ctx);
          if (n.e.bparams.size() > 1) f.bparams.emplace_back(n.e.bparams[1], Val::num(static_cast<double>(i)));
        }
        frames_.push_back(std::move(f));
        render(n.body);
        frames_.pop_back();
      }
      return;
    }
    if (v.t == Val::T::Json && v.j->IsObject() && v.j->MemberCount() > 0) {
      const size_t sz = v.j->MemberCount();
      size_t i = 0;
      for (const auto& m : v.j->GetObject()) {
        if (full()) break;
        Frame f;
        f.ctx = Val::json(&m.value);
        f.has_data = true;
        f.index = i;
        f.has_key = true;
        f.key.assign(m.name.GetString(), m.name.GetStringLength());
        f.first = i == 0;
        f.last = i + 1 == sz;
        if (use_bparams) {
          if (n.e.bparams.size() > 0) f.bparams.emplace_back(n.e.bparams[0], f.ctx);
          if (n.e.bparams.size() > 1) f.bparams.emplace_back(n.e.bparams[1], Val::str(f.key));
        }
        frames_.push_back(std::move(f));
        render(n.body);
        frames_.pop_back();
        ++i;
      }
      return;
    }
    render(n.inverse);
  }

  void with(const Node& n, const Val& v) {
    if (!truthy(v)) return render(n.inverse);
    Frame f;
    f.ctx = v;
    if (!n.e.bparams.empty()) f.bparams.emplace_back(n.e.bparams[0], v);
    frames_.push_back(std::move(f));
    render(n.body);
    frames_.pop_back();
  }

  void block(const Node& n) {
    const Expr& e = n.e;
    const std::string& h = n.name;
    const bool has_arg = e.args.size() > 1;
    const Val arg = has_arg ? value_of(e.args[1]) : Val{};
    if (!n.inverted && h == "each" && has_arg) return each(n, arg, true);
    if (!n.inverted && h == "if" && has_arg) return render(truthy(arg) ? n.body : n.inverse);
    if (!n.inverted && h == "unless" && has_arg) return render(truthy(arg) ? n.inverse : n.body);
    if (!n.inverted && h == "with" && has_arg) return with(n, arg);
    if (has_arg) {
      warn("block helper \"" + h.substr(0, 40) + "\" is not available (rendered as nothing)");
      return;
    }
    // A mustache section: {{#items}}...{{/items}} / {{^items}}...{{/items}}.
    const Val v = resolve(h);
    if (n.inverted) return render(truthy(v) ? n.inverse : n.body);
    if (v.t == Val::T::Json && v.j->IsArray()) return each(n, v, false);
    if (v.t == Val::T::Json && v.j->IsObject()) return with(n, v);
    render(truthy(v) ? n.body : n.inverse);
  }

  const JV& root_;
  size_t max_;
  std::vector<std::string>& warn_;
  std::vector<Frame> frames_;
};

}  // namespace
}  // namespace hbs

VisualizeResult render_handlebars(std::string_view tpl, std::string_view data_json, size_t max_out) {
  VisualizeResult r;
  rapidjson::Document d;
  if (data_json.empty()) data_json = "{}";
  d.Parse<rapidjson::kParseIterativeFlag | rapidjson::kParseNanAndInfFlag>(data_json.data(), data_json.size());
  if (d.HasParseError()) {
    r.error = std::string("the data is not JSON: ") + rapidjson::GetParseError_En(d.GetParseError()) + " (offset " +
              std::to_string(d.GetErrorOffset()) + ")";
    return r;
  }
  std::vector<hbs::Tok> toks;
  if (!hbs::tokenize(tpl, toks, r.error)) return r;
  std::vector<hbs::Node> tree;
  hbs::Parser p(tpl, toks, r.warnings);
  if (!p.run(tree, r.error)) return r;
  hbs::Renderer rd(d, max_out, r.warnings);
  rd.render(tree);
  r.html = std::move(rd.out_);
  r.truncated = rd.truncated_;
  r.ok = true;
  return r;
}

std::string visualize_page(std::string_view body_html, bool dark) {
  std::string out;
  out.reserve(body_html.size() + 1536);
  out +=
      "<!doctype html><html><head><meta charset=\"utf-8\">"
      "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; style-src 'unsafe-inline'; "
      "img-src data:; font-src data:; media-src data:; form-action 'none'; base-uri 'none'\">";
  // A base the template builds on (it comes first, so the template's own rules win): the app's
  // colours, tables with rules and a header band, readable links, code and form controls.
  out += dark ? "<style>:root{color-scheme:dark}html{background:#1e1e1e;color:#d4d4d4}"
                "th,td{border-bottom:1px solid #3a3a3a}th{background:#262a2e;color:#e8e8e8;font-weight:600}"
                "tr:hover td{background:#25282b}a{color:#4daafc}hr{border:0;border-top:1px solid #3a3a3a}"
                "code,pre{background:#262626;border-radius:3px}input,select,button,textarea{background:#2b2b2b;"
                "color:#d4d4d4;border:1px solid #444}"
              : "<style>:root{color-scheme:light}html{background:#fff;color:#1f2328}"
                "th,td{border-bottom:1px solid #e3e5e8}th{background:#f4f5f7;font-weight:600}"
                "a{color:#0969da}hr{border:0;border-top:1px solid #e3e5e8}code,pre{background:#f4f5f7;"
                "border-radius:3px}";
  out +=
      "body{margin:12px 14px;font:13px/1.45 'Segoe UI',system-ui,sans-serif}"
      "table{border-collapse:collapse}th,td{padding:4px 10px;text-align:left;vertical-align:top}"
      "pre{padding:8px 10px}code{padding:0 3px}pre code{padding:0}"
      "pre,code{font-family:Consolas,'Cascadia Mono',monospace}</style></head><body>";
  out.append(body_html);
  out += "</body></html>";
  return out;
}

}  // namespace watt::app::rv
