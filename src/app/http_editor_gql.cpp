#include "app/http_editor_gql.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>

#include <rapidjson/document.h>

namespace watt::app::http_editor {

const char* graphql_introspection_query() {
  return "query IntrospectionQuery {\n"
         "  __schema {\n"
         "    queryType { name }\n"
         "    mutationType { name }\n"
         "    subscriptionType { name }\n"
         "    types { ...FullType }\n"
         "    directives { name description locations args { ...InputValue } }\n"
         "  }\n"
         "}\n"
         "fragment FullType on __Type {\n"
         "  kind name description\n"
         "  fields(includeDeprecated: true) { name description args { ...InputValue } type { ...TypeRef } "
         "isDeprecated deprecationReason }\n"
         "  inputFields { ...InputValue }\n"
         "  interfaces { ...TypeRef }\n"
         "  enumValues(includeDeprecated: true) { name description isDeprecated deprecationReason }\n"
         "  possibleTypes { ...TypeRef }\n"
         "}\n"
         "fragment InputValue on __InputValue { name description type { ...TypeRef } defaultValue }\n"
         "fragment TypeRef on __Type {\n"
         "  kind name\n"
         "  ofType { kind name ofType { kind name ofType { kind name ofType { kind name ofType { kind name "
         "ofType { kind name ofType { kind name ofType { kind name } } } } } } } }\n"
         "}\n";
}

const GqlArg* GqlField::arg(std::string_view n) const {
  auto it = std::lower_bound(args.begin(), args.end(), n, [](const GqlArg& a, std::string_view k) { return a.name < k; });
  return it != args.end() && it->name == n ? &*it : nullptr;
}

const GqlField* GqlType::field(std::string_view n) const {
  auto it = std::lower_bound(fields.begin(), fields.end(), n,
                             [](const GqlField& f, std::string_view k) { return f.name < k; });
  return it != fields.end() && it->name == n ? &*it : nullptr;
}

const GqlType* GqlSchema::type(std::string_view n) const {
  auto it = types.find(n);
  return it == types.end() ? nullptr : &it->second;
}

size_t GqlSchema::user_type_count() const {
  size_t n = 0;
  for (const auto& [name, t] : types)
    if (name.compare(0, 2, "__") != 0) ++n;
  return n;
}

// ------------------------------------------------------------------ schema parse

namespace {

using JVal = rapidjson::Value;

std::string jstr(const JVal& o, const char* k) {
  if (!o.IsObject()) return {};
  auto it = o.FindMember(k);
  if (it == o.MemberEnd() || !it->value.IsString()) return {};
  return std::string(it->value.GetString(), it->value.GetStringLength());
}

const JVal* jget(const JVal& o, const char* k) {
  if (!o.IsObject()) return nullptr;
  auto it = o.FindMember(k);
  return it == o.MemberEnd() ? nullptr : &it->value;
}

std::string type_ref(const JVal& t, std::string* named, int depth = 0) {
  if (!t.IsObject() || depth > 16) return {};
  const std::string kind = jstr(t, "kind");
  const JVal* of = jget(t, "ofType");
  if (kind == "NON_NULL" && of) return type_ref(*of, named, depth + 1) + "!";
  if (kind == "LIST" && of) return "[" + type_ref(*of, named, depth + 1) + "]";
  std::string n = jstr(t, "name");
  *named = n;
  return n;
}

GqlArg input_value(const JVal& v) {
  GqlArg a;
  a.name = jstr(v, "name");
  a.desc = jstr(v, "description");
  a.default_value = jstr(v, "defaultValue");
  if (const JVal* t = jget(v, "type")) a.type = type_ref(*t, &a.named);
  return a;
}

std::vector<GqlArg> input_values(const JVal* arr) {
  std::vector<GqlArg> out;
  if (!arr || !arr->IsArray()) return out;
  out.reserve(arr->Size());
  for (const auto& v : arr->GetArray()) out.push_back(input_value(v));
  std::sort(out.begin(), out.end(), [](const GqlArg& a, const GqlArg& b) { return a.name < b.name; });
  return out;
}

}  // namespace

std::shared_ptr<const GqlSchema> parse_gql_schema(std::string_view json, std::string* err) {
  rapidjson::Document d;
  d.Parse(json.data(), json.size());
  if (d.HasParseError() || !d.IsObject()) {
    if (err) *err = "the response is not JSON";
    return nullptr;
  }
  const JVal* s = nullptr;
  if (const JVal* data = jget(d, "data"); data && data->IsObject()) s = jget(*data, "__schema");
  if (!s) s = jget(d, "__schema");
  if (!s && jget(d, "types")) s = &d;
  if (!s || !s->IsObject()) {
    std::string msg;
    if (const JVal* errs = jget(d, "errors"); errs && errs->IsArray() && !errs->Empty())
      msg = jstr((*errs)[0], "message");
    if (err) *err = msg.empty() ? "no __schema in the response (introspection disabled?)" : msg;
    return nullptr;
  }
  auto out = std::make_shared<GqlSchema>();
  if (const JVal* q = jget(*s, "queryType")) out->query = jstr(*q, "name");
  if (const JVal* q = jget(*s, "mutationType")) out->mutation = jstr(*q, "name");
  if (const JVal* q = jget(*s, "subscriptionType")) out->subscription = jstr(*q, "name");
  const JVal* types = jget(*s, "types");
  if (!types || !types->IsArray()) {
    if (err) *err = "the schema has no types";
    return nullptr;
  }
  for (const auto& tv : types->GetArray()) {
    GqlType t;
    t.name = jstr(tv, "name");
    if (t.name.empty()) continue;
    t.kind = jstr(tv, "kind");
    t.desc = jstr(tv, "description");
    if (const JVal* fs = jget(tv, "fields"); fs && fs->IsArray()) {
      t.fields.reserve(fs->Size());
      for (const auto& fv : fs->GetArray()) {
        GqlField f;
        f.name = jstr(fv, "name");
        f.desc = jstr(fv, "description");
        if (const JVal* ft = jget(fv, "type")) f.type = type_ref(*ft, &f.named);
        f.args = input_values(jget(fv, "args"));
        if (const JVal* dep = jget(fv, "isDeprecated"); dep && dep->IsBool()) f.deprecated = dep->GetBool();
        t.fields.push_back(std::move(f));
      }
      std::sort(t.fields.begin(), t.fields.end(), [](const GqlField& a, const GqlField& b) { return a.name < b.name; });
    }
    t.input_fields = input_values(jget(tv, "inputFields"));
    if (const JVal* ev = jget(tv, "enumValues"); ev && ev->IsArray())
      for (const auto& e : ev->GetArray()) t.enum_values.push_back(jstr(e, "name"));
    std::string key = t.name;
    out->types.emplace(std::move(key), std::move(t));
  }
  if (out->query.empty() && out->type("Query")) out->query = "Query";
  return out;
}

// ------------------------------------------------------------------ tokens

namespace {

struct Tok {
  char k;    // 'n' name, 's' string, 'd' number, 'p' punctuator ('.' stands for "..."), 'v' {{var}}
  char c;    // the punctuator
  uint32_t pos;
  uint32_t len;
};

bool name_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool name_char(char c) { return name_start(c) || (c >= '0' && c <= '9'); }

// Tokens that start before `stop`. *inside: `stop` is inside a string or a comment.
std::vector<Tok> tokenize(std::string_view s, size_t stop, bool* inside) {
  std::vector<Tok> out;
  out.reserve(s.size() / 4 + 4);
  const size_t n = std::min(s.size(), stop);
  if (inside) *inside = false;
  size_t i = 0;
  while (i < n) {
    const char c = s[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',') {
      ++i;
    } else if (c == '#') {
      const size_t e = s.find('\n', i);
      if (e == std::string_view::npos || e >= stop) {
        if (inside && stop <= s.size()) *inside = true;
        return out;
      }
      i = e + 1;
    } else if (c == '"') {
      const size_t at = i;
      size_t end;
      if (s.substr(i, 3) == "\"\"\"") {
        const size_t e = s.find("\"\"\"", i + 3);
        end = e == std::string_view::npos ? s.size() : e + 3;
      } else {
        size_t j = i + 1;
        while (j < s.size() && s[j] != '"' && s[j] != '\n') j += s[j] == '\\' ? 2 : 1;
        end = j < s.size() && s[j] == '"' ? j + 1 : std::min(j, s.size());
      }
      const bool terminated = end - at >= 2 && s[end - 1] == '"' && (end - at != 2 || s[at + 1] == '"');
      if (end > stop || (end == stop && !terminated)) {
        if (inside) *inside = true;
        return out;
      }
      out.push_back({'s', 0, static_cast<uint32_t>(at), static_cast<uint32_t>(end - at)});
      i = end;
    } else if (name_start(c)) {
      size_t j = i + 1;
      while (j < s.size() && name_char(s[j])) ++j;
      out.push_back({'n', 0, static_cast<uint32_t>(i), static_cast<uint32_t>(j - i)});
      i = j;
    } else if ((c >= '0' && c <= '9') || c == '-') {
      size_t j = i + 1;
      while (j < s.size() && (name_char(s[j]) || s[j] == '.' || s[j] == '+' || s[j] == '-')) ++j;
      out.push_back({'d', 0, static_cast<uint32_t>(i), static_cast<uint32_t>(j - i)});
      i = j;
    } else if (c == '.' && s.substr(i, 3) == "...") {
      out.push_back({'p', '.', static_cast<uint32_t>(i), 3});
      i += 3;
    } else if (c == '{' && i + 1 < s.size() && s[i + 1] == '{') {
      // {{var}}: never GraphQL ("{{" is not valid there), a watt variable standing for a value.
      const size_t e = s.find("}}", i + 2);
      const size_t nl = s.find('\n', i + 2);
      if (e != std::string_view::npos && (nl == std::string_view::npos || e < nl)) {
        out.push_back({'v', 0, static_cast<uint32_t>(i), static_cast<uint32_t>(e + 2 - i)});
        i = e + 2;
      } else {
        out.push_back({'p', '{', static_cast<uint32_t>(i), 1});
        ++i;
      }
    } else if (std::string_view("!$&():=@[]{|}").find(c) != std::string_view::npos) {
      out.push_back({'p', c, static_cast<uint32_t>(i), 1});
      ++i;
    } else {
      ++i;
    }
  }
  return out;
}

enum class Fk { Sel, Keyed, List, VarDefs };

struct Frame {
  Fk k = Fk::Sel;
  char close = '}';
  bool checked = false;  // Sel: type known; Keyed: keys known
  const GqlType* type = nullptr;
  // Sel
  const GqlType* pending = nullptr;  // type of the last field (its "{")
  bool pending_known = false;
  bool pending_leaf = false;  // the last field is a scalar / enum
  std::string_view pending_name;
  const GqlField* last = nullptr;
  bool last_args = false;           // a "(" followed the last field
  uint32_t last_pos = 0, last_len = 0;  // the last field's name in the document
  bool after_spread = false, after_on = false, after_at = false, dir = false;
  // Keyed
  const std::vector<GqlArg>* keys = nullptr;
  bool args = false;  // field arguments (else input object fields)
  std::string on;     // "User.posts" / "PostFilter"
  int ks = 0;         // 0 key, 1 ':', 2 value
  const GqlArg* cur = nullptr;
  std::vector<std::string_view> used;
  const GqlField* of = nullptr;  // args: the field they belong to (required ones are checked at ")")
  uint32_t of_pos = 0, of_len = 0;
  // List
  std::string elem;
  // VarDefs
  int vs = 0;  // 0 '$', 1 name, 2 ':', 3 type, 4 default value
};

struct Walker {
  std::string_view doc;
  const GqlSchema* schema = nullptr;
  bool collect = true;
  std::vector<GqlIssue> issues;
  std::vector<GqlOperation> ops;
  std::vector<Frame> st;
  // Top level: 0 definition, 1 operation header, 2 fragment name, 3 'on', 4 type condition,
  // 5 fragment header, 6 something else (SDL): its braces are skipped.
  int top = 0;
  bool top_at = false, top_dir = false;
  bool in_op = false;
  const GqlType* top_type = nullptr;
  bool top_checked = false;

  std::string_view text(const Tok& t) const { return doc.substr(t.pos, t.len); }

  void issue(const Tok& t, std::string msg) {
    if (!collect || issues.size() >= 64) return;
    issues.push_back({t.pos, t.len, std::move(msg)});
  }

  // Required arguments (non-null, no default) of `fd` not among `used`.
  void missing_args(const GqlField& fd, const std::vector<std::string_view>& used, uint32_t pos, uint32_t len) {
    for (const GqlArg& a : fd.args) {
      if (a.type.empty() || a.type.back() != '!' || !a.default_value.empty()) continue;
      if (std::find(used.begin(), used.end(), std::string_view(a.name)) != used.end()) continue;
      issue(Tok{'n', 0, pos, len}, "Field '" + fd.name + "' is missing the required argument '" + a.name + ": " + a.type + "'");
    }
  }
  // The last field of a selection ends (the next field, a spread, "}"): an object field needs
  // its own "{ ... }", a field with required arguments needs "(...)". Not at the end of the text:
  // the user may still be typing it.
  void field_done(Frame& f) {
    if (f.last && f.checked) {
      if (!f.last_args) missing_args(*f.last, {}, f.last_pos, f.last_len);
      if (f.pending_known && f.pending && f.pending->selectable())
        issue(Tok{'n', 0, f.last_pos, f.last_len}, "Field '" + f.last->name + "' of type " + f.pending->name +
                                                         " needs a selection of fields: { ... }");
    }
    f.last = nullptr;
    f.last_args = false;
  }

  const GqlType* type_named(std::string_view n) const { return schema ? schema->type(n) : nullptr; }

  void push_keyed_for(const GqlArg* a, char close) {
    Frame f;
    f.k = Fk::Keyed;
    f.close = close;
    if (a && schema) {
      if (const GqlType* t = schema->type(a->named); t && t->kind == "INPUT_OBJECT") {
        f.checked = true;
        f.type = t;
        f.keys = &t->input_fields;
        f.on = t->name;
      }
    }
    st.push_back(std::move(f));
  }
  void push_list(std::string elem) {
    Frame f;
    f.k = Fk::List;
    f.close = ']';
    f.elem = std::move(elem);
    st.push_back(std::move(f));
  }
  void push_sel(const GqlType* t, bool checked) {
    Frame f;
    f.k = Fk::Sel;
    f.close = '}';
    f.type = t;
    f.checked = checked && t;
    st.push_back(std::move(f));
  }

  // A closer: pops the frame it closes (and any unclosed one above it); a stray one is ignored.
  void close(char c) {
    auto it = std::find_if(st.rbegin(), st.rend(), [c](const Frame& f) { return f.close == c; });
    if (it == st.rend()) return;
    st.erase(std::next(it).base(), st.end());
    if (st.empty()) {
      if (c == '}') top = 0;
      return;
    }
    Frame& p = st.back();
    if (p.k == Fk::Keyed) {
      p.ks = 0;
      p.cur = nullptr;
    }
  }

  void var_ref(const std::vector<Tok>& t, size_t& i) {
    if (i + 1 >= t.size() || t[i + 1].k != 'n') return;
    ++i;
    if (!in_op || ops.empty()) return;
    const std::string_view n = text(t[i]);
    const auto& vars = ops.back().vars;
    if (std::none_of(vars.begin(), vars.end(), [&](const GqlVar& v) { return v.name == n; }))
      issue(t[i], "Variable $" + std::string(n) + " is not declared by the operation" +
                      (ops.back().name.empty() ? std::string() : " " + ops.back().name));
  }

  void run(const std::vector<Tok>& t) {
    for (size_t i = 0; i < t.size(); ++i) {
      const Tok& k = t[i];
      if (st.empty()) {
        top_level(t, i);
        continue;
      }
      Frame& f = st.back();
      switch (f.k) {
        case Fk::Sel: sel(t, i, f); break;
        case Fk::Keyed: keyed(t, i, f); break;
        case Fk::List:
          if (k.k == 'p' && k.c == '{') {
            GqlArg a;
            a.named = f.elem;
            push_keyed_for(&a, '}');
          } else if (k.k == 'p' && k.c == '[') {
            push_list(f.elem);
          } else if (k.k == 'p' && k.c == '$') {
            var_ref(t, i);
          } else if (k.k == 'p' && (k.c == ']' || k.c == '}' || k.c == ')')) {
            close(k.c);
          }
          break;
        case Fk::VarDefs: var_defs(t, i, f); break;
      }
    }
  }

  void top_level(const std::vector<Tok>& t, size_t& i) {
    const Tok& k = t[i];
    const bool name = k.k == 'n';
    const std::string_view s = text(k);
    if (top_at) {  // a directive's name
      top_at = false;
      top_dir = name;
      return;
    }
    if (k.k == 'p' && k.c == '@') {
      top_at = true;
      return;
    }
    if (k.k == 'p' && k.c == '(') {
      if (top == 1 && !top_dir) {
        Frame f;
        f.k = Fk::VarDefs;
        f.close = ')';
        st.push_back(std::move(f));
      } else {
        Frame f;
        f.k = Fk::Keyed;
        f.close = ')';
        st.push_back(std::move(f));
      }
      top_dir = false;
      return;
    }
    top_dir = false;
    if (k.k == 'p' && k.c == '{') {
      if (top == 0) {  // an anonymous query
        ops.push_back({"query", "", {}});
        in_op = true;
        top_type = schema && !schema->query.empty() ? schema->type(schema->query) : nullptr;
        top_checked = schema != nullptr;
      } else if (top == 6) {
        top_type = nullptr;
        top_checked = false;
      }
      push_sel(top_type, top_checked);
      top = 7;
      return;
    }
    switch (top) {
      case 0:
        if (name && (s == "query" || s == "mutation" || s == "subscription")) {
          ops.push_back({std::string(s), "", {}});
          in_op = true;
          top = 1;
          top_checked = schema != nullptr;
          top_type = nullptr;
          if (schema) {
            const std::string& root = s == "query" ? schema->query : s == "mutation" ? schema->mutation : schema->subscription;
            top_type = root.empty() ? nullptr : schema->type(root);
            if (!top_type) {
              issue(k, "The schema has no " + std::string(s) + " type");
              top_checked = false;
            }
          }
        } else if (name && s == "fragment") {
          in_op = false;
          top = 2;
        } else {
          in_op = false;
          top = 6;
        }
        break;
      case 1:
        if (name && ops.back().name.empty()) ops.back().name = std::string(s);
        break;
      case 2:
        top = 3;
        break;
      case 3:
        top = name && s == "on" ? 4 : 6;
        break;
      case 4:
        top = 5;
        top_type = type_named(s);
        top_checked = top_type != nullptr;
        if (schema && !top_type) issue(k, "Unknown type '" + std::string(s) + "'");
        break;
      default:
        break;
    }
  }

  void var_defs(const std::vector<Tok>& t, size_t& i, Frame& f) {
    const Tok& k = t[i];
    if (k.k == 'p' && k.c == ')') {
      close(')');
      return;
    }
    if (k.k == 'p' && k.c == '$') {
      f.vs = 1;
      return;
    }
    switch (f.vs) {
      case 1:
        if (k.k == 'n' && !ops.empty()) {
          ops.back().vars.push_back({std::string(text(k)), "", false});
          f.vs = 2;
        }
        break;
      case 2:
        if (k.k == 'p' && k.c == ':') f.vs = 3;
        break;
      case 3:
        if (k.k == 'p' && k.c == '=') {
          f.vs = 4;
          if (!ops.empty() && !ops.back().vars.empty()) ops.back().vars.back().has_default = true;
        } else if (k.k == 'p' && k.c == '@') {
          f.vs = 0;
        } else if (!ops.empty() && !ops.back().vars.empty() &&
                   (k.k == 'n' || (k.k == 'p' && (k.c == '[' || k.c == ']' || k.c == '!')))) {
          ops.back().vars.back().type += text(k);
          if (k.k == 'n' && schema && !schema->type(text(k))) issue(k, "Unknown type '" + std::string(text(k)) + "'");
        }
        break;
      case 4:
        if (k.k == 'p' && k.c == '{') {
          push_keyed_for(nullptr, '}');
        } else if (k.k == 'p' && k.c == '[') {
          push_list("");
        }
        break;
      default:
        break;
    }
  }

  void keyed(const std::vector<Tok>& t, size_t& i, Frame& f) {
    const Tok& k = t[i];
    if (k.k == 'p' && (k.c == ')' || k.c == '}' || k.c == ']')) {
      if (k.c == ')' && f.args && f.of) missing_args(*f.of, f.used, f.of_pos, f.of_len);
      close(k.c);
      return;
    }
    switch (f.ks) {
      case 0:
        if (k.k == 'n') {
          const std::string_view n = text(k);
          f.cur = nullptr;
          if (f.checked && f.keys) {
            auto it = std::lower_bound(f.keys->begin(), f.keys->end(), n,
                                       [](const GqlArg& a, std::string_view x) { return a.name < x; });
            if (it != f.keys->end() && it->name == n) f.cur = &*it;
            else
              issue(k, (f.args ? "Unknown argument '" : "Unknown field '") + std::string(n) +
                           (f.args ? "' on field " : "' on input type ") + f.on);
          }
          f.used.push_back(n);
          f.ks = 1;
        }
        break;
      case 1:
        if (k.k == 'p' && k.c == ':') f.ks = 2;
        else if (k.k == 'n') {  // "a b": the ':' is missing, b is the next key
          f.ks = 0;
          --i;  // seen again as a key (i >= 1: a key came before)
        }
        break;
      case 2: {
        if (k.k == 'p' && k.c == '{') {
          const GqlArg* a = f.cur;
          f.ks = 3;  // set back to 0 when the object closes
          push_keyed_for(a, '}');
        } else if (k.k == 'p' && k.c == '[') {
          const std::string elem = f.cur ? f.cur->named : std::string();
          f.ks = 3;
          push_list(elem);
        } else if (k.k == 'p' && k.c == '$') {
          var_ref(t, i);
          f.ks = 0;
          f.cur = nullptr;
        } else if (k.k == 'n' || k.k == 's' || k.k == 'd' || k.k == 'v') {
          if (k.k == 'n' && f.cur && schema) {
            const std::string_view v = text(k);
            if (const GqlType* et = schema->type(f.cur->named); et && et->kind == "ENUM" && v != "null" &&
                std::find(et->enum_values.begin(), et->enum_values.end(), v) == et->enum_values.end())
              issue(k, "Unknown value '" + std::string(v) + "' for enum " + et->name);
          }
          f.ks = 0;
          f.cur = nullptr;
        }
        break;
      }
      default:
        break;
    }
  }

  void sel(const std::vector<Tok>& t, size_t& i, Frame& f) {
    const Tok& k = t[i];
    if (k.k == 'p') {
      switch (k.c) {
        case '}':
          field_done(f);
          close('}');
          return;
        case ')':
        case ']':
          close(k.c);
          return;
        case '.':
          if (!f.after_spread) field_done(f);
          f.after_spread = true;
          f.after_on = false;
          return;
        case '@':
          if (f.after_spread) {  // "... @include(if: $x) {": an inline fragment on the same type
            f.after_spread = false;
            f.pending = f.type;
            f.pending_known = f.checked;
            f.pending_leaf = false;
          }
          f.after_at = true;
          return;
        case '(': {
          Frame a;
          a.k = Fk::Keyed;
          a.close = ')';
          if (!f.dir && f.last && f.checked) {
            a.checked = true;
            a.args = true;
            a.keys = &f.last->args;
            a.on = (f.type ? f.type->name : std::string()) + "." + f.last->name;
            a.of = f.last;
            a.of_pos = f.last_pos;
            a.of_len = f.last_len;
            f.last_args = true;
          }
          f.dir = false;
          st.push_back(std::move(a));  // invalidates f
          return;
        }
        case '{': {
          const GqlType* nt = f.pending;
          bool known = f.pending_known;
          if (f.after_spread) {  // "... {"
            nt = f.type;
            known = f.checked;
          } else if (known && f.pending_leaf) {
            issue(k, "Field '" + std::string(f.pending_name) + "' is a " +
                         (nt ? nt->kind == "ENUM" ? "enum" : "scalar" : "leaf") + " and has no fields to select");
            known = false;
          } else if (known && nt && !nt->selectable()) {
            known = false;
          }
          if (f.last && f.checked && !f.last_args && !f.after_spread) missing_args(*f.last, {}, f.last_pos, f.last_len);
          f.last_args = false;
          f.pending = nullptr;
          f.pending_known = false;
          f.pending_leaf = false;
          f.last = nullptr;
          f.after_spread = f.after_on = f.dir = false;
          push_sel(nt, known);  // invalidates f
          return;
        }
        default:
          return;
      }
    }
    if (k.k != 'n') return;
    const std::string_view n = text(k);
    if (f.after_at) {
      f.after_at = false;
      f.dir = true;
      return;
    }
    if (f.after_spread) {
      f.after_spread = false;
      if (n == "on") f.after_on = true;
      return;  // else a fragment spread
    }
    if (f.after_on) {
      f.after_on = false;
      f.pending = type_named(n);
      f.pending_known = f.pending != nullptr;
      f.pending_leaf = false;
      if (schema && !f.pending) issue(k, "Unknown type '" + std::string(n) + "'");
      return;
    }
    if (i + 1 < t.size() && t[i + 1].k == 'p' && t[i + 1].c == ':') {  // "alias: field"
      field_done(f);
      ++i;
      return;
    }
    field_done(f);
    f.dir = false;
    f.last = nullptr;
    f.pending = nullptr;
    f.pending_known = false;
    f.pending_leaf = false;
    f.pending_name = n;
    if (!f.checked || !f.type) return;
    if (n == "__typename") {
      return;
    }
    if (schema && f.type->name == schema->query && (n == "__schema" || n == "__type")) {
      f.pending = schema->type(n == "__schema" ? "__Schema" : "__Type");
      f.pending_known = f.pending != nullptr;
      return;
    }
    const GqlField* fd = f.type->field(n);
    if (!fd) {
      issue(k, "Unknown field '" + std::string(n) + "' on " +
                   (f.type->kind == "UNION" ? "union " + f.type->name + " (select through ... on Type)"
                                            : "type " + f.type->name));
      return;
    }
    f.last = fd;
    f.last_pos = k.pos;
    f.last_len = k.len;
    f.pending = type_named(fd->named);
    f.pending_known = f.pending != nullptr;
    f.pending_leaf = f.pending && !f.pending->selectable();
  }
};

bool icase_prefix(std::string_view s, std::string_view p) {
  if (p.size() > s.size()) return false;
  for (size_t i = 0; i < p.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(p[i]))) return false;
  return true;
}

std::string args_summary(const GqlField& f) {
  if (f.args.empty()) return {};
  std::string s = "(";
  for (size_t i = 0; i < f.args.size(); ++i) {
    if (i) s += ", ";
    if (i == 3) {
      s += "...";
      break;
    }
    s += f.args[i].name + ": " + f.args[i].type;
  }
  return s + ")";
}

}  // namespace

std::vector<GqlIssue> gql_validate(std::string_view doc, const GqlSchema* schema) {
  Walker w;
  w.doc = doc;
  w.schema = schema;
  w.run(tokenize(doc, doc.size(), nullptr));
  return std::move(w.issues);
}

std::vector<GqlOperation> gql_operations(std::string_view doc) {
  Walker w;
  w.doc = doc;
  w.collect = false;
  w.run(tokenize(doc, doc.size(), nullptr));
  return std::move(w.ops);
}

std::vector<GqlIssue> gql_check_variables(std::string_view doc, std::string_view vars, std::string_view operation_name) {
  std::vector<GqlIssue> out;
  const auto ops = gql_operations(doc);
  const GqlOperation* op = nullptr;
  for (const auto& o : ops)
    if (!operation_name.empty() && o.name == operation_name) op = &o;
  if (!op && !ops.empty()) op = &ops.front();
  if (!op) return out;
  // Top-level keys of the variables object.
  struct Key {
    std::string name;
    size_t pos, len;
  };
  std::vector<Key> keys;
  size_t i = 0;
  const size_t n = vars.size();
  auto ws = [&] {
    while (i < n) {
      const char c = vars[i];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++i;
      else if (c == '/' && i + 1 < n && vars[i + 1] == '/') {
        while (i < n && vars[i] != '\n') ++i;
      } else if (c == '/' && i + 1 < n && vars[i + 1] == '*') {
        const size_t e = vars.find("*/", i + 2);
        i = e == std::string_view::npos ? n : e + 2;
      } else break;
    }
  };
  ws();
  if (i < n && vars[i] != '{') return out;  // not an object: the JSON check says so
  bool any_template_key = false;
  if (i < n) {
    ++i;
    int depth = 1;
    bool expect_key = true;
    while (i < n && depth > 0) {
      ws();
      if (i >= n) break;
      const char c = vars[i];
      if (c == '"') {
        const size_t at = i++;
        std::string s;
        while (i < n && vars[i] != '"' && vars[i] != '\n') {
          if (vars[i] == '\\' && i + 1 < n) {
            s += vars[i + 1];
            i += 2;
          } else {
            s += vars[i++];
          }
        }
        if (i < n && vars[i] == '"') ++i;
        if (depth == 1 && expect_key) {
          keys.push_back({std::move(s), at, i - at});
          expect_key = false;
        }
      } else if (c == '{' && i + 1 < n && vars[i + 1] == '{') {
        const size_t e = vars.find("}}", i + 2);
        if (depth == 1 && expect_key) {
          any_template_key = true;
          expect_key = false;
        }
        i = e == std::string_view::npos ? n : e + 2;
      } else if (c == '{' || c == '[') {
        ++depth;
        ++i;
      } else if (c == '}' || c == ']') {
        --depth;
        ++i;
      } else if (c == ',') {
        if (depth == 1) expect_key = true;
        ++i;
      } else {
        ++i;
      }
    }
  }
  std::string declared;
  for (const auto& v : op->vars) declared += (declared.empty() ? "$" : ", $") + v.name;
  for (const auto& k : keys) {
    if (std::none_of(op->vars.begin(), op->vars.end(), [&](const GqlVar& v) { return v.name == k.name; }))
      out.push_back({k.pos, k.len,
                     "Variable \"" + k.name + "\" is not declared by " +
                         (op->name.empty() ? "the " + op->kind : op->kind + " " + op->name) +
                         (declared.empty() ? std::string(" (it declares none)") : " (declared: " + declared + ")")});
  }
  if (!any_template_key)
    for (const auto& v : op->vars) {
      if (v.has_default || v.type.empty() || v.type.back() != '!') continue;
      if (std::none_of(keys.begin(), keys.end(), [&](const Key& k) { return k.name == v.name; }))
        out.push_back({0, 0, "Missing required variable $" + v.name + " (" + v.type + ")"});
    }
  return out;
}

GqlCompletion gql_complete(std::string_view doc, size_t caret, const GqlSchema* schema) {
  GqlCompletion out;
  caret = std::min(caret, doc.size());
  size_t w = caret;
  while (w > 0 && name_char(doc[w - 1])) --w;
  out.from = w;
  if (w < caret && !name_start(doc[w])) return out;  // a number
  bool inside = false;
  const std::vector<Tok> toks = tokenize(doc, w, &inside);
  if (inside) return out;
  Walker wk;
  wk.doc = doc;
  wk.schema = schema;
  wk.collect = false;
  wk.run(toks);
  const std::string_view typed = doc.substr(w, caret - w);
  auto add = [&](std::string name, std::string detail, std::string desc) {
    if (!icase_prefix(name, typed)) return;
    out.items.push_back({std::move(name), std::move(detail), std::move(desc)});
  };
  auto add_types = [&](bool input) {
    if (!schema) return;
    for (const auto& [n, t] : schema->types) {
      if (n.compare(0, 2, "__") == 0) continue;
      const bool in = t.kind == "SCALAR" || t.kind == "ENUM" || t.kind == "INPUT_OBJECT";
      if (input ? in : t.selectable()) add(n, t.kind, t.desc);
    }
    out.kind = GqlCompletion::Kind::Type;
  };
  auto add_values = [&](const std::string& named) {
    if (named == "Boolean") {
      add("true", "Boolean", "");
      add("false", "Boolean", "");
      out.kind = GqlCompletion::Kind::EnumValue;
      return;
    }
    const GqlType* t = schema ? schema->type(named) : nullptr;
    if (!t || t->kind != "ENUM") return;
    for (const auto& v : t->enum_values) add(v, t->name, "");
    out.kind = GqlCompletion::Kind::EnumValue;
    out.on = t->name;
  };
  const Tok* prev = toks.empty() ? nullptr : &toks.back();
  if (prev && prev->k == 'p' && prev->c == '$' && prev->pos + 1 == w) {
    if (!wk.st.empty() && (wk.st.back().k == Fk::Keyed || wk.st.back().k == Fk::List) && wk.in_op && !wk.ops.empty()) {
      for (const auto& v : wk.ops.back().vars) add(v.name, v.type, "");
      out.kind = GqlCompletion::Kind::Variable;
    }
  } else if (wk.st.empty()) {
    if (wk.top == 0) {
      for (const char* kw : {"query", "mutation", "subscription", "fragment"}) add(kw, "", "");
      out.kind = GqlCompletion::Kind::Keyword;
    } else if (wk.top == 3) {
      add("on", "", "");
      out.kind = GqlCompletion::Kind::Keyword;
    } else if (wk.top == 4) {
      add_types(false);
    }
  } else {
    const Frame& f = wk.st.back();
    switch (f.k) {
      case Fk::Sel:
        if (f.after_at) {
          add("include", "(if: Boolean!)", "Include this field only when the argument is true");
          add("skip", "(if: Boolean!)", "Skip this field when the argument is true");
          out.kind = GqlCompletion::Kind::Keyword;
        } else if (f.after_spread) {
          add("on", "", "An inline fragment on a type");
          out.kind = GqlCompletion::Kind::Keyword;
        } else if (f.after_on) {
          add_types(false);
        } else if (f.checked && f.type) {
          out.kind = GqlCompletion::Kind::Field;
          out.on = f.type->name;
          for (const auto& fd : f.type->fields)
            add(fd.name, args_summary(fd) + (fd.args.empty() ? "" : ": ") + fd.type,
                fd.deprecated ? "Deprecated. " + fd.desc : fd.desc);
          add("__typename", "String!", "The name of the object's type");
          if (schema && f.type->name == schema->query) {
            add("__schema", "__Schema!", "Introspection: the whole schema");
            add("__type", "(name: String!): __Type", "Introspection: one type");
          }
        }
        break;
      case Fk::Keyed:
        if (f.ks == 0 && f.checked && f.keys) {
          out.kind = f.args ? GqlCompletion::Kind::Argument : GqlCompletion::Kind::InputField;
          out.on = f.on;
          for (const auto& a : *f.keys) {
            if (std::find(f.used.begin(), f.used.end(), a.name) != f.used.end()) continue;
            add(a.name, a.type + (a.default_value.empty() ? "" : " = " + a.default_value), a.desc);
          }
        } else if (f.ks == 2 && f.cur) {
          add_values(f.cur->named);
        }
        break;
      case Fk::List:
        add_values(f.elem);
        break;
      case Fk::VarDefs:
        if (f.vs == 3 && prev && prev->k == 'p' && (prev->c == ':' || prev->c == '[')) add_types(true);
        break;
    }
  }
  std::sort(out.items.begin(), out.items.end(), [](const GqlItem& a, const GqlItem& b) {
    const bool au = a.name.compare(0, 2, "__") == 0, bu = b.name.compare(0, 2, "__") == 0;
    return au != bu ? bu : a.name < b.name;  // __typename last
  });
  return out;
}

std::string gql_cache_key(std::string_view url, std::string_view env) {
  std::string k(url);
  k += '\x1f';
  k += env;
  return k;
}

std::string gql_cache_file_name(std::string_view key) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : key) {
    h ^= c;
    h *= 1099511628211ull;
  }
  char buf[32];
  std::snprintf(buf, sizeof buf, "%016llx.json", static_cast<unsigned long long>(h));
  return buf;
}

std::string gql_age(double seconds) {
  if (seconds < 45) return "just now";
  if (seconds < 90 * 60) return std::to_string(std::max(1, static_cast<int>(seconds / 60 + 0.5))) + " min ago";
  if (seconds < 48 * 3600) return std::to_string(static_cast<int>(seconds / 3600 + 0.5)) + " h ago";
  return std::to_string(static_cast<int>(seconds / 86400 + 0.5)) + " d ago";
}

}  // namespace watt::app::http_editor
