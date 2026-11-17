#include "app/response_filter.h"

#include <re2/re2.h>
#include <rapidjson/document.h>
#include <rapidjson/memorystream.h>
#include <rapidjson/encodedstream.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/reader.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <deque>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace watt::app::rv {
namespace {

using rapidjson::Value;

// ------------------------------------------------------------------ number-preserving parse
//
// The body is parsed with numbers kept as their original text (kParseNumbersAsStringsFlag), so a
// match shows "1.50" and "0.00" as the response wrote them, not reprinted as 1.5 / 0. To tell a
// number from a string afterwards, the text of a number is stored behind a two-byte tag "\0N";
// a real string that itself starts with NUL gets "\0S" (so the tag is never ambiguous).

constexpr char kTagNum = 'N', kTagStr = 'S';

struct Tagger {
  rapidjson::Document& d;
  std::string buf;
  bool Null() { return d.Null(); }
  bool Bool(bool b) { return d.Bool(b); }
  bool Int(int i) { return d.Int(i); }
  bool Uint(unsigned i) { return d.Uint(i); }
  bool Int64(int64_t i) { return d.Int64(i); }
  bool Uint64(uint64_t i) { return d.Uint64(i); }
  bool Double(double x) { return d.Double(x); }
  bool tagged(char tag, const char* s, rapidjson::SizeType n) {
    buf.assign(1, '\0');
    buf += tag;
    buf.append(s, n);
    return d.String(buf.data(), static_cast<rapidjson::SizeType>(buf.size()), true);
  }
  bool RawNumber(const char* s, rapidjson::SizeType n, bool) { return tagged(kTagNum, s, n); }
  bool String(const char* s, rapidjson::SizeType n, bool copy) {
    if (n > 0 && s[0] == '\0') return tagged(kTagStr, s, n);
    return d.String(s, n, copy);
  }
  bool StartObject() { return d.StartObject(); }
  bool Key(const char* s, rapidjson::SizeType n, bool copy) { return d.Key(s, n, copy); }
  bool EndObject(rapidjson::SizeType n) { return d.EndObject(n); }
  bool StartArray() { return d.StartArray(); }
  bool EndArray(rapidjson::SizeType n) { return d.EndArray(n); }
};

// Parses text into d; false with *err_at = the error offset when it is not JSON.
bool parse_tagged(std::string_view text, rapidjson::Document& d, size_t* err_at) {
  bool ok = false;
  auto gen = [&](rapidjson::Document& doc) {
    Tagger t{doc, {}};
    rapidjson::MemoryStream ms(text.data(), text.size());
    rapidjson::EncodedInputStream<rapidjson::UTF8<>, rapidjson::MemoryStream> is(ms);
    rapidjson::Reader reader;
    const rapidjson::ParseResult r =
        reader.Parse<rapidjson::kParseIterativeFlag | rapidjson::kParseNumbersAsStringsFlag>(is, t);
    ok = !r.IsError();
    if (!ok && err_at) *err_at = r.Offset();
    return ok;
  };
  d.Populate(gen);
  if (ok && err_at) *err_at = 0;
  return ok;
}

bool is_tagged(const Value& v, char tag) {
  return v.IsString() && v.GetStringLength() >= 2 && v.GetString()[0] == '\0' && v.GetString()[1] == tag;
}
bool is_num(const Value& v) { return v.IsNumber() || is_tagged(v, kTagNum); }
bool is_str(const Value& v) { return v.IsString() && !is_tagged(v, kTagNum); }
// The text of a string or of a tagged number (without the tag).
std::string_view text_of(const Value& v) {
  if (!v.IsString()) return {};
  std::string_view s(v.GetString(), v.GetStringLength());
  if (is_tagged(v, kTagNum) || is_tagged(v, kTagStr)) s.remove_prefix(2);
  return s;
}
bool num_of(const Value& v, double* out) {
  if (v.IsNumber()) {
    *out = v.GetDouble();
    return true;
  }
  if (!is_tagged(v, kTagNum)) return false;
  const std::string s(text_of(v));
  *out = std::strtod(s.c_str(), nullptr);
  return true;
}

// ------------------------------------------------------------------ expression

struct FilterExpr;

struct Step {
  enum Kind { Key, Index, Slice, Wild, DescKey, DescWild, Filter } kind = Key;
  std::vector<std::string> keys;  // Key / DescKey (several: ['a','b'])
  std::vector<long> idx;          // Index
  long a = 0, b = 0;              // Slice
  bool has_a = false, has_b = false;
  std::shared_ptr<const FilterExpr> filter;  // Filter: [?(...)]
};

// [?(...)] : JavaScript-like semantics (jsonpath-plus, which Insomnia uses): comparisons of
// numbers by value and strings by text, a lone path is true when its value is truthy.
struct Operand {
  enum Kind { Rel, Root, Num, Str, Bool, Null } kind = Num;
  std::vector<Step> path;  // Rel (@...) / Root ($...): member names and indices only
  double num = 0;
  std::string str;
  bool b = false;
};

struct FNode {
  enum Kind { Or, And, Not, Cmp, Test } kind = Test;
  int l = -1, r = -1;  // children (node indices)
  std::string op;      // Cmp: == != < <= > >= =~
  Operand lhs, rhs;
  std::shared_ptr<RE2> re;  // =~
};

struct FilterExpr {
  std::vector<FNode> nodes;
  int root = -1;
};

bool name_char(char c) {
  const auto u = static_cast<unsigned char>(c);
  return std::isalnum(u) || c == '_' || c == '-' || c == '$' || c == '@' || u >= 0x80;
}
bool fname_char(char c) {  // inside a filter '-' is an operator, '@'/'$' start paths
  const auto u = static_cast<unsigned char>(c);
  return std::isalnum(u) || c == '_' || u >= 0x80;
}

class Parser {
 public:
  explicit Parser(std::string s) : s_(std::move(s)) {}

  std::string parse(std::vector<Step>& out) {
    if (s_.empty() || s_[0] != '$') return "start with $ (the body), e.g. $.items[*].name";
    if (s_.size() > 4096) return "expression too long";
    p_ = 1;
    while (p_ < s_.size()) {
      if (out.size() > 64) return "too many steps";
      if (s_[p_] == '[') {
        if (std::string err = bracket(false, out); !err.empty()) return err;
        continue;
      }
      if (s_[p_] != '.') return std::string("unexpected '") + s_[p_] + "' at " + std::to_string(p_ + 1);
      ++p_;
      bool desc = false;
      if (p_ < s_.size() && s_[p_] == '.') {
        desc = true;
        ++p_;
      }
      if (p_ < s_.size() && s_[p_] == '*') {
        ++p_;
        Step st;
        st.kind = desc ? Step::DescWild : Step::Wild;
        out.push_back(std::move(st));
        continue;
      }
      if (p_ < s_.size() && s_[p_] == '[') {  // "$.[0]" is read as "$[0]" (and "$..[0]" as "$..[0]")
        if (std::string err = bracket(desc, out); !err.empty()) return err;
        continue;
      }
      const size_t from = p_;
      while (p_ < s_.size() && name_char(s_[p_])) ++p_;
      if (p_ == from) return "expected a name after " + std::string(desc ? ".." : ".");
      Step st;
      st.kind = desc ? Step::DescKey : Step::Key;
      st.keys.push_back(s_.substr(from, p_ - from));
      out.push_back(std::move(st));
    }
    return {};
  }

 private:
  void ws() {
    while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t')) ++p_;
  }
  bool number(long* v) {
    size_t q = p_;
    if (q < s_.size() && s_[q] == '-') ++q;
    if (q >= s_.size() || !std::isdigit(static_cast<unsigned char>(s_[q]))) return false;
    while (q < s_.size() && std::isdigit(static_cast<unsigned char>(s_[q]))) ++q;
    if (q - p_ > 12) return false;
    *v = std::strtol(s_.c_str() + p_, nullptr, 10);
    p_ = q;
    return true;
  }
  bool quoted(std::string* k) {
    const char q = s_[p_++];
    while (p_ < s_.size() && s_[p_] != q) {
      if (s_[p_] == '\\' && p_ + 1 < s_.size()) ++p_;
      *k += s_[p_++];
    }
    if (p_ >= s_.size()) return false;
    ++p_;
    return true;
  }

  // [...] after a step; desc: after "..".
  std::string bracket(bool desc, std::vector<Step>& out) {
    ++p_;  // '['
    ws();
    Step st;
    if (p_ < s_.size() && s_[p_] == '*') {
      ++p_;
      ws();
      if (p_ >= s_.size() || s_[p_] != ']') return "expected ] after [*";
      ++p_;
      st.kind = desc ? Step::DescWild : Step::Wild;
      out.push_back(std::move(st));
      return {};
    }
    if (p_ < s_.size() && s_[p_] == '?') {
      ++p_;
      auto fe = std::make_shared<FilterExpr>();
      expr_ = fe.get();
      depth_ = 0;
      int root = -1;
      if (std::string err = parse_or(&root); !err.empty()) return "filter: " + err;
      ws();
      if (p_ >= s_.size() || s_[p_] != ']') return "filter: expected ] after the expression";
      ++p_;
      fe->root = root;
      if (desc) {  // $..[?(...)]: every value below, then the filter on each container
        Step d;
        d.kind = Step::DescWild;
        out.push_back(std::move(d));
      }
      st.kind = Step::Filter;
      st.filter = std::move(fe);
      out.push_back(std::move(st));
      return {};
    }
    if (p_ < s_.size() && (s_[p_] == '\'' || s_[p_] == '"')) {
      for (;;) {
        std::string k;
        if (!quoted(&k)) return "unterminated quote";
        st.keys.push_back(std::move(k));
        ws();
        if (p_ < s_.size() && s_[p_] == ',') {
          ++p_;
          ws();
          if (p_ >= s_.size() || (s_[p_] != '\'' && s_[p_] != '"')) return "expected a quoted name after ,";
          continue;
        }
        break;
      }
      if (p_ >= s_.size() || s_[p_] != ']') return "expected ]";
      ++p_;
      st.kind = desc ? Step::DescKey : Step::Key;
      out.push_back(std::move(st));
      return {};
    }
    if (desc) return "after .. use a name, *, ['name'] or [?(...)]";
    long v = 0;
    const bool first = number(&v);
    ws();
    if (p_ < s_.size() && s_[p_] == ':') {
      ++p_;
      ws();
      st.kind = Step::Slice;
      st.has_a = first;
      st.a = v;
      long w = 0;
      if (number(&w)) {
        st.has_b = true;
        st.b = w;
      }
      ws();
      if (p_ < s_.size() && s_[p_] == ':') return "slice steps ([a:b:step]) are not supported";
      if (p_ >= s_.size() || s_[p_] != ']') return "expected ] after the slice";
      ++p_;
      out.push_back(std::move(st));
      return {};
    }
    if (!first) return "expected an index, a slice, *, [?(...)] or a quoted name inside [ ]";
    st.kind = Step::Index;
    st.idx.push_back(v);
    while (p_ < s_.size() && s_[p_] == ',') {
      ++p_;
      ws();
      if (!number(&v)) return "expected an index after ,";
      st.idx.push_back(v);
      ws();
    }
    if (p_ >= s_.size() || s_[p_] != ']') return "expected ]";
    ++p_;
    out.push_back(std::move(st));
    return {};
  }

  // ---- filter expressions (recursive descent, depth-limited)
  int add(FNode n) {
    expr_->nodes.push_back(std::move(n));
    return static_cast<int>(expr_->nodes.size()) - 1;
  }
  bool eat(const char* tok) {
    ws();
    const size_t n = std::char_traits<char>::length(tok);
    if (s_.compare(p_, n, tok) != 0) return false;
    p_ += n;
    return true;
  }
  std::string parse_or(int* out) {
    if (++depth_ > 32 || expr_->nodes.size() > 256) return "expression too complex";
    int l = -1;
    if (std::string e = parse_and(&l); !e.empty()) return e;
    while (eat("||")) {
      int r = -1;
      if (std::string e = parse_and(&r); !e.empty()) return e;
      FNode n;
      n.kind = FNode::Or;
      n.l = l;
      n.r = r;
      l = add(std::move(n));
    }
    --depth_;
    *out = l;
    return {};
  }
  std::string parse_and(int* out) {
    int l = -1;
    if (std::string e = parse_unary(&l); !e.empty()) return e;
    while (eat("&&")) {
      int r = -1;
      if (std::string e = parse_unary(&r); !e.empty()) return e;
      FNode n;
      n.kind = FNode::And;
      n.l = l;
      n.r = r;
      l = add(std::move(n));
    }
    *out = l;
    return {};
  }
  std::string parse_unary(int* out) {
    ws();
    if (p_ < s_.size() && s_[p_] == '!' && (p_ + 1 >= s_.size() || s_[p_ + 1] != '=')) {
      ++p_;
      if (++depth_ > 32) return "expression too complex";
      int c = -1;
      if (std::string e = parse_unary(&c); !e.empty()) return e;
      --depth_;
      FNode n;
      n.kind = FNode::Not;
      n.l = c;
      *out = add(std::move(n));
      return {};
    }
    if (p_ < s_.size() && s_[p_] == '(') {
      ++p_;
      if (std::string e = parse_or(out); !e.empty()) return e;
      if (!eat(")")) return "expected )";
      return {};
    }
    FNode n;
    if (std::string e = operand(&n.lhs); !e.empty()) return e;
    ws();
    static const char* kOps[] = {"===", "!==", "==", "!=", "<=", ">=", "=~", "<", ">"};
    for (const char* op : kOps)
      if (eat(op)) {
        n.op = op;
        if (n.op == "===") n.op = "==";
        if (n.op == "!==") n.op = "!=";
        break;
      }
    if (n.op.empty()) {
      n.kind = FNode::Test;
      *out = add(std::move(n));
      return {};
    }
    n.kind = FNode::Cmp;
    ws();
    if (n.op == "=~") {
      if (p_ >= s_.size() || s_[p_] != '/') return "expected /regex/ after =~";
      ++p_;
      std::string pat;
      while (p_ < s_.size() && s_[p_] != '/') {
        if (s_[p_] == '\\' && p_ + 1 < s_.size()) {
          if (s_[p_ + 1] != '/') pat += '\\';
          ++p_;
        }
        pat += s_[p_++];
      }
      if (p_ >= s_.size()) return "unterminated /regex/";
      ++p_;
      RE2::Options o;
      o.set_log_errors(false);
      while (p_ < s_.size() && std::isalpha(static_cast<unsigned char>(s_[p_]))) {
        if (s_[p_] == 'i') o.set_case_sensitive(false);
        else if (s_[p_] != 'g' && s_[p_] != 'm' && s_[p_] != 's' && s_[p_] != 'u') return "unknown regex flag";
        ++p_;
      }
      n.re = std::make_shared<RE2>(pat, o);
      if (!n.re->ok()) return "bad regex: " + n.re->error();
      *out = add(std::move(n));
      return {};
    }
    if (std::string e = operand(&n.rhs); !e.empty()) return e;
    *out = add(std::move(n));
    return {};
  }
  std::string operand(Operand* o) {
    ws();
    if (p_ >= s_.size()) return "expected a value";
    const char c = s_[p_];
    if (c == '@' || c == '$') {
      o->kind = c == '@' ? Operand::Rel : Operand::Root;
      ++p_;
      for (;;) {
        if (o->path.size() > 32) return "path too long";
        if (p_ < s_.size() && s_[p_] == '.') {
          ++p_;
          const size_t from = p_;
          while (p_ < s_.size() && fname_char(s_[p_])) ++p_;
          if (p_ == from) return "expected a name after .";
          Step st;
          st.keys.push_back(s_.substr(from, p_ - from));
          o->path.push_back(std::move(st));
        } else if (p_ < s_.size() && s_[p_] == '[') {
          ++p_;
          ws();
          Step st;
          long v = 0;
          if (p_ < s_.size() && (s_[p_] == '\'' || s_[p_] == '"')) {
            std::string k;
            if (!quoted(&k)) return "unterminated quote";
            st.keys.push_back(std::move(k));
          } else if (number(&v)) {
            st.kind = Step::Index;
            st.idx.push_back(v);
          } else {
            return "expected an index or a quoted name inside [ ]";
          }
          ws();
          if (p_ >= s_.size() || s_[p_] != ']') return "expected ]";
          ++p_;
          o->path.push_back(std::move(st));
        } else {
          break;
        }
      }
      return {};
    }
    if (c == '\'' || c == '"') {
      o->kind = Operand::Str;
      if (!quoted(&o->str)) return "unterminated quote";
      return {};
    }
    if (c == '-' || c == '+' || c == '.' || std::isdigit(static_cast<unsigned char>(c))) {
      const char* b = s_.c_str() + p_;
      char* e = nullptr;
      o->num = std::strtod(b, &e);
      if (e == b) return "expected a number";
      o->kind = Operand::Num;
      p_ += static_cast<size_t>(e - b);
      return {};
    }
    for (const char* w : {"true", "false", "null"}) {
      const size_t n = std::char_traits<char>::length(w);
      if (s_.compare(p_, n, w) == 0 && (p_ + n >= s_.size() || !fname_char(s_[p_ + n]))) {
        p_ += n;
        if (w[0] == 'n') o->kind = Operand::Null;
        else {
          o->kind = Operand::Bool;
          o->b = w[0] == 't';
        }
        return {};
      }
    }
    return std::string("unexpected '") + c + "' at " + std::to_string(p_ + 1) + " (paths start with @ or $)";
  }

  std::string s_;
  size_t p_ = 0;
  FilterExpr* expr_ = nullptr;
  int depth_ = 0;
};

std::string parse(std::string_view e, std::vector<Step>& out) {
  while (!e.empty() && std::isspace(static_cast<unsigned char>(e.front()))) e.remove_prefix(1);
  while (!e.empty() && std::isspace(static_cast<unsigned char>(e.back()))) e.remove_suffix(1);
  std::string s;
  if (e.rfind("res.body", 0) == 0) s = "$" + std::string(e.substr(8));
  else if (!e.empty() && (e[0] == '.' || e[0] == '[')) s = "$" + std::string(e);
  else s = std::string(e);
  return Parser(std::move(s)).parse(out);
}

size_t utf8_length(const char* s, size_t n) {
  size_t k = 0;
  for (size_t i = 0; i < n; ++i) k += (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80;
  return k;
}

// ---- filter evaluation

// A resolved operand: a JSON value, or a literal.
struct Val {
  enum Kind { Missing, Num, Str, Bool, Null, Other } kind = Missing;
  double num = 0;
  std::string_view str;
  bool b = false;
  const Value* v = nullptr;
};

Val from_json(const Value* v) {
  Val r;
  if (!v) return r;
  r.v = v;
  if (is_num(*v)) {
    r.kind = Val::Num;
    num_of(*v, &r.num);
  } else if (v->IsString()) {
    r.kind = Val::Str;
    r.str = text_of(*v);
  } else if (v->IsBool()) {
    r.kind = Val::Bool;
    r.b = v->GetBool();
  } else if (v->IsNull()) {
    r.kind = Val::Null;
  } else {
    r.kind = Val::Other;
  }
  return r;
}

const Value* walk(const Value* v, const std::vector<Step>& path, std::deque<Value>& made) {
  for (const Step& st : path) {
    if (!v) return nullptr;
    if (st.kind == Step::Index) {
      if (!v->IsArray()) return nullptr;
      const long n = static_cast<long>(v->Size());
      const long j = st.idx[0] < 0 ? st.idx[0] + n : st.idx[0];
      v = j >= 0 && j < n ? &(*v)[static_cast<rapidjson::SizeType>(j)] : nullptr;
      continue;
    }
    const std::string& k = st.keys[0];
    if (v->IsObject()) {
      const auto it = v->FindMember(Value(rapidjson::StringRef(k.data(), k.size())));
      if (it != v->MemberEnd()) {
        v = &it->value;
        continue;
      }
    }
    if (k == "length" && (v->IsArray() || v->IsObject() || is_str(*v))) {
      const std::string_view t = text_of(*v);
      const uint64_t n = v->IsArray() ? v->Size() : v->IsObject() ? v->MemberCount() : utf8_length(t.data(), t.size());
      made.emplace_back(n);
      v = &made.back();
      continue;
    }
    return nullptr;
  }
  return v;
}

Val resolve(const Operand& o, const Value* at, const Value* root, std::deque<Value>& made) {
  Val r;
  switch (o.kind) {
    case Operand::Rel: return from_json(walk(at, o.path, made));
    case Operand::Root: return from_json(walk(root, o.path, made));
    case Operand::Num: r.kind = Val::Num; r.num = o.num; return r;
    case Operand::Str: r.kind = Val::Str; r.str = o.str; return r;
    case Operand::Bool: r.kind = Val::Bool; r.b = o.b; return r;
    case Operand::Null: r.kind = Val::Null; return r;
  }
  return r;
}

bool truthy(const Val& v) {
  switch (v.kind) {
    case Val::Missing: case Val::Null: return false;
    case Val::Num: return v.num != 0 && !std::isnan(v.num);
    case Val::Str: return !v.str.empty();
    case Val::Bool: return v.b;
    case Val::Other: return true;
  }
  return false;
}

bool compare(const std::string& op, const Val& a, const Val& b) {
  if (op == "==" || op == "!=") {
    bool eq = false;
    if (a.kind == b.kind) {
      switch (a.kind) {
        case Val::Num: eq = a.num == b.num; break;
        case Val::Str: eq = a.str == b.str; break;
        case Val::Bool: eq = a.b == b.b; break;
        case Val::Null: case Val::Missing: eq = true; break;
        case Val::Other: eq = a.v == b.v; break;
      }
    }
    return op == "==" ? eq : !eq;
  }
  int c = 0;
  if (a.kind == Val::Num && b.kind == Val::Num) {
    if (std::isnan(a.num) || std::isnan(b.num)) return false;
    c = a.num < b.num ? -1 : a.num > b.num ? 1 : 0;
  } else if (a.kind == Val::Str && b.kind == Val::Str) {
    c = a.str.compare(b.str);
  } else {
    return false;
  }
  if (op == "<") return c < 0;
  if (op == "<=") return c <= 0;
  if (op == ">") return c > 0;
  return c >= 0;
}

bool eval_node(const FilterExpr& e, int i, const Value* at, const Value* root, std::deque<Value>& made) {
  const FNode& n = e.nodes[static_cast<size_t>(i)];
  switch (n.kind) {
    case FNode::Or: return eval_node(e, n.l, at, root, made) || eval_node(e, n.r, at, root, made);
    case FNode::And: return eval_node(e, n.l, at, root, made) && eval_node(e, n.r, at, root, made);
    case FNode::Not: return !eval_node(e, n.l, at, root, made);
    case FNode::Test: return truthy(resolve(n.lhs, at, root, made));
    case FNode::Cmp: {
      const Val a = resolve(n.lhs, at, root, made);
      if (n.re) {
        // Strings, and numbers by their text (a literal number has none: no match).
        std::string_view t;
        if (a.kind == Val::Str) t = a.str;
        else if (a.kind == Val::Num && a.v && a.v->IsString()) t = text_of(*a.v);
        else return false;
        return RE2::PartialMatch(re2::StringPiece(t.data(), t.size()), *n.re);
      }
      return compare(n.op, a, resolve(n.rhs, at, root, made));
    }
  }
  return false;
}

// Output stream for the writer that stops storing at a limit.
struct CappedOut {
  using Ch = char;
  std::string* s;
  size_t max;
  bool cut = false;
  void Put(char c) {
    if (s->size() < max) s->push_back(c);
    else cut = true;
  }
  void Flush() {}
};

// Depth of v (explicit stack): the writer recurses, so very deep values are refused.
bool depth_ok(const Value& v, int limit) {
  std::vector<std::pair<const Value*, int>> st{{&v, 1}};
  while (!st.empty()) {
    auto [x, d] = st.back();
    st.pop_back();
    if (d > limit) return false;
    if (x->IsArray())
      for (const Value& c : x->GetArray()) st.push_back({&c, d + 1});
    else if (x->IsObject())
      for (const auto& m : x->GetObject()) st.push_back({&m.value, d + 1});
  }
  return true;
}

// Writes v with numbers as the response wrote them (depth checked by depth_ok first).
template <class W>
void emit(const Value& v, W& w) {
  if (v.IsObject()) {
    w.StartObject();
    for (const auto& m : v.GetObject()) {
      w.Key(m.name.GetString(), m.name.GetStringLength());
      emit(m.value, w);
    }
    w.EndObject();
  } else if (v.IsArray()) {
    w.StartArray();
    for (const Value& c : v.GetArray()) emit(c, w);
    w.EndArray();
  } else if (is_tagged(v, kTagNum)) {
    const std::string_view t = text_of(v);
    w.RawValue(t.data(), t.size(), rapidjson::kNumberType);
  } else if (v.IsString()) {
    const std::string_view t = text_of(v);
    w.String(t.data(), static_cast<rapidjson::SizeType>(t.size()));
  } else {
    v.Accept(w);
  }
}

}  // namespace

std::string json_filter_check(std::string_view expr) {
  std::vector<Step> steps;
  return parse(expr, steps);
}

FilterResult json_filter(std::string_view body, std::string_view expr, size_t max_out) {
  FilterResult r;
  try {
    std::vector<Step> steps;
    if (std::string err = parse(expr, steps); !err.empty()) {
      r.error = err;
      return r;
    }
    rapidjson::Document d;
    size_t err_at = 0;
    if (!parse_tagged(body, d, &err_at)) {
      r.error = "the body is not JSON (error at byte " + std::to_string(err_at) + ")";
      return r;
    }
    std::deque<Value> made;  // values the query creates (.length)
    std::vector<const Value*> cur{&d};
    for (const Step& st : steps) {
      std::vector<const Value*> next;
      auto add = [&](const Value* v) {
        if (next.size() >= kMaxFilterMatches) {
          r.truncated = true;
          return false;
        }
        next.push_back(v);
        return true;
      };
      for (const Value* v : cur) {
        bool more = true;
        switch (st.kind) {
          case Step::Key:
            for (const std::string& k : st.keys) {
              if (v->IsObject()) {
                const auto it = v->FindMember(Value(rapidjson::StringRef(k.data(), k.size())));
                if (it != v->MemberEnd()) {
                  more = add(&it->value);
                  continue;
                }
              }
              if (k == "length" && (v->IsArray() || v->IsObject() || is_str(*v))) {
                const std::string_view t = text_of(*v);
                const uint64_t n = v->IsArray()    ? v->Size()
                                   : v->IsObject() ? v->MemberCount()
                                                   : utf8_length(t.data(), t.size());
                made.emplace_back(n);
                more = add(&made.back());
              }
            }
            break;
          case Step::Index:
            if (!v->IsArray()) break;
            for (long i : st.idx) {
              const long n = static_cast<long>(v->Size());
              const long j = i < 0 ? i + n : i;
              if (j >= 0 && j < n) more = add(&(*v)[static_cast<rapidjson::SizeType>(j)]);
            }
            break;
          case Step::Slice: {
            if (!v->IsArray()) break;
            const long n = static_cast<long>(v->Size());
            long a = st.has_a ? st.a : 0, b = st.has_b ? st.b : n;
            if (a < 0) a += n;
            if (b < 0) b += n;
            a = std::clamp(a, 0L, n);
            b = std::clamp(b, 0L, n);
            for (long j = a; j < b && more; ++j) more = add(&(*v)[static_cast<rapidjson::SizeType>(j)]);
            break;
          }
          case Step::Wild:
            if (v->IsArray())
              for (const Value& c : v->GetArray())
                if (!(more = add(&c))) break;
            if (v->IsObject())
              for (const auto& m : v->GetObject())
                if (!(more = add(&m.value))) break;
            break;
          case Step::Filter: {
            const FilterExpr& fe = *st.filter;
            auto test = [&](const Value* c) {
              const size_t keep = made.size();
              const bool ok = eval_node(fe, fe.root, c, &d, made);
              made.resize(keep);  // .length temporaries of this test
              return ok;
            };
            if (v->IsArray()) {
              for (const Value& c : v->GetArray())
                if (test(&c) && !(more = add(&c))) break;
            } else if (v->IsObject()) {
              for (const auto& m : v->GetObject())
                if (test(&m.value) && !(more = add(&m.value))) break;
            }
            break;
          }
          case Step::DescKey:
          case Step::DescWild: {
            // Every value below v in document order (explicit stack, children pushed reversed).
            std::vector<const Value*> stack{v};
            while (!stack.empty() && more) {
              const Value* x = stack.back();
              stack.pop_back();
              if (x->IsObject()) {
                if (st.kind == Step::DescKey)
                  for (const std::string& k : st.keys) {
                    const auto it = x->FindMember(Value(rapidjson::StringRef(k.data(), k.size())));
                    if (it != x->MemberEnd() && !(more = add(&it->value))) break;
                  }
                const auto& o = x->GetObject();
                for (auto it = o.end(); it != o.begin();) {
                  --it;
                  stack.push_back(&it->value);
                }
                if (st.kind == Step::DescWild)
                  for (const auto& m : o)
                    if (!(more = add(&m.value))) break;
              } else if (x->IsArray()) {
                const auto& a = x->GetArray();
                for (auto it = a.end(); it != a.begin();) {
                  --it;
                  stack.push_back(&*it);
                }
                if (st.kind == Step::DescWild)
                  for (const Value& c : a)
                    if (!(more = add(&c))) break;
              }
            }
            break;
          }
        }
        if (!more) break;
      }
      cur = std::move(next);
    }
    r.matches = cur.size();
    r.ok = true;
    if (cur.empty()) return r;
    for (const Value* v : cur)
      if (!depth_ok(*v, 400)) {
        r.ok = false;
        r.error = "a match is nested too deeply to show";
        return r;
      }
    CappedOut out{&r.json, max_out};
    rapidjson::PrettyWriter<CappedOut> w(out);
    w.SetIndent(' ', 2);
    if (cur.size() == 1) {
      emit(*cur[0], w);
    } else {
      w.StartArray();
      for (const Value* v : cur) emit(*v, w);
      w.EndArray();
    }
    if (out.cut) r.truncated = true;
  } catch (...) {
    r.ok = false;
    r.error = "out of memory";
  }
  return r;
}

namespace {

// Compact JSON of v, at most `cap` bytes ("..." when cut) and kMaxCellDepth levels deep (deeper
// containers are written as [...] / {...}): bounded recursion.
constexpr int kMaxCellDepth = 32;
void compact(const Value& v, std::string& o, size_t cap, int depth) {
  if (o.size() >= cap) return;
  if (v.IsObject()) {
    if (depth >= kMaxCellDepth) {
      o += "{\xE2\x80\xA6}";
      return;
    }
    o += '{';
    bool first = true;
    for (const auto& m : v.GetObject()) {
      if (o.size() >= cap) return;
      if (!first) o += ',';
      first = false;
      o += '"';
      o.append(m.name.GetString(), m.name.GetStringLength());
      o += "\":";
      compact(m.value, o, cap, depth + 1);
    }
    o += '}';
  } else if (v.IsArray()) {
    if (depth >= kMaxCellDepth) {
      o += "[\xE2\x80\xA6]";
      return;
    }
    o += '[';
    bool first = true;
    for (const Value& c : v.GetArray()) {
      if (o.size() >= cap) return;
      if (!first) o += ',';
      first = false;
      compact(c, o, cap, depth + 1);
    }
    o += ']';
  } else if (is_tagged(v, kTagNum)) {
    o += text_of(v);  // as the response wrote it
  } else if (v.IsString()) {
    const std::string_view t = text_of(v);
    if (depth > 0) o += '"';
    o.append(t.data(), std::min<size_t>(t.size(), cap));
    if (depth > 0) o += '"';
  } else if (v.IsBool()) {
    o += v.GetBool() ? "true" : "false";
  } else if (v.IsNull()) {
    o += "null";
  } else if (v.IsInt64()) {
    o += std::to_string(v.GetInt64());
  } else if (v.IsUint64()) {
    o += std::to_string(v.GetUint64());
  } else {
    char b[40];
    std::snprintf(b, sizeof b, "%.17g", v.GetDouble());
    // Shortest of %.15g / %.17g that round-trips (0.1 stays 0.1).
    char s[40];
    std::snprintf(s, sizeof s, "%.15g", v.GetDouble());
    o += std::strtod(s, nullptr) == v.GetDouble() ? s : b;
  }
}

std::string cell(const Value& v) {
  std::string o;
  compact(v, o, 200, 0);
  if (o.size() > 200) {
    o.resize(200);
    o += "\xE2\x80\xA6";
  }
  // One line in a list cell.
  for (char& c : o)
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
  return o;
}

}  // namespace

TableModel json_table(std::string_view json, const std::string& prefer_path) {
  TableModel t;
  try {
    rapidjson::Document d;
    if (!parse_tagged(json, d, nullptr)) {
      t.error = "not JSON";
      return t;
    }
    const Value* arr = nullptr;
    t.source = "$";
    if (d.IsArray()) {
      arr = &d;
    } else if (d.IsObject() && prefer_path.empty()) {
      // The largest member array (of objects first): {"items": [...], "total": 3} -> items.
      size_t best = 0;
      bool best_obj = false;
      for (const auto& m : d.GetObject()) {
        if (!m.value.IsArray() || m.value.Empty()) continue;
        const bool objs = m.value[0].IsObject();
        if ((objs && !best_obj) || (objs == best_obj && m.value.Size() > best)) {
          arr = &m.value;
          best = m.value.Size();
          best_obj = objs;
          t.source = "$." + std::string(m.name.GetString(), m.name.GetStringLength());
        }
      }
    }
    if (!arr) {
      if (!d.IsObject()) {
        t.cols = {"value"};
        t.cells.push_back({cell(d)});
        return t;
      }
      t.cols = {"key", "value"};
      for (const auto& m : d.GetObject()) {
        if (t.cells.size() >= kMaxTableRows) {
          t.truncated = true;
          break;
        }
        t.cells.push_back({std::string(m.name.GetString(), m.name.GetStringLength()), cell(m.value)});
      }
      return t;
    }
    // Columns: every member name of the objects, in first-seen order.
    std::vector<std::string> cols;
    std::unordered_map<std::string, size_t> at;
    bool scalars = false;
    for (const Value& row : arr->GetArray()) {
      if (!row.IsObject()) {
        scalars = true;
        continue;
      }
      for (const auto& m : row.GetObject()) {
        std::string k(m.name.GetString(), m.name.GetStringLength());
        if (at.count(k)) continue;
        if (cols.size() >= kMaxTableCols) {
          t.truncated = true;
          continue;
        }
        at.emplace(k, cols.size());
        cols.push_back(std::move(k));
      }
    }
    if (scalars) {
      at.emplace("", cols.size());  // non-object rows: their value in a "value" column
      cols.push_back("value");
    }
    t.cols = cols;
    const size_t n = std::min<size_t>(arr->Size(), kMaxTableRows);
    if (arr->Size() > n) t.truncated = true;
    t.cells.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      const Value& row = (*arr)[static_cast<rapidjson::SizeType>(i)];
      std::vector<std::string> r(cols.size());
      if (row.IsObject()) {
        for (const auto& m : row.GetObject()) {
          const auto it = at.find(std::string(m.name.GetString(), m.name.GetStringLength()));
          if (it != at.end()) r[it->second] = cell(m.value);
        }
      } else {
        r[at[""]] = cell(row);
      }
      t.cells.push_back(std::move(r));
    }
  } catch (...) {
    t = TableModel{};
    t.error = "out of memory";
  }
  return t;
}

bool table_cell_less(const std::string& a, const std::string& b) {
  auto num = [](const std::string& s, double* v) {
    if (s.empty()) return false;
    char* end = nullptr;
    *v = std::strtod(s.c_str(), &end);
    return end && *end == 0;
  };
  double x = 0, y = 0;
  const bool nx = num(a, &x), ny = num(b, &y);
  if (nx && ny) return x < y;
  if (nx != ny) return nx;  // numbers before text
  if (a.empty() != b.empty()) return !a.empty();  // empty cells last
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    const int ca = std::tolower(static_cast<unsigned char>(a[i])), cb = std::tolower(static_cast<unsigned char>(b[i]));
    if (ca != cb) return ca < cb;
  }
  return a.size() < b.size();
}

}  // namespace watt::app::rv
