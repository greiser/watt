#include "app/http_editor_jshelp.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace watt::app::http_editor {

namespace {

bool js_ident_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$';
}

using Table = std::map<std::string, std::vector<std::string>, std::less<>>;

const Table& table() {
  static const Table t = [] {
    Table m;
    const std::vector<std::string> store = {"get", "set", "has", "unset", "toObject"};
    m["pm"] = {"collectionVariables", "environment", "expect", "globals", "info", "request", "response",
               "sendRequest", "test", "variables"};
    m["pm.environment"] = store;
    m["pm.globals"] = store;
    m["pm.collectionVariables"] = store;
    m["pm.variables"] = {"get", "has", "replaceIn", "set", "toObject", "unset"};
    m["pm.request"] = {"addHeader", "body", "headers", "method", "removeHeader", "upsertHeader", "url"};
    m["pm.request.url"] = {"query", "toString"};
    m["pm.request.url.query"] = {"add", "get", "remove", "toObject", "upsert"};
    m["pm.request.headers"] = {"add", "get", "has", "remove", "toObject", "upsert"};
    m["pm.request.body"] = {"raw"};
    m["pm.response"] = {"code", "headers", "json", "responseSize", "responseTime", "status", "text", "to"};
    m["pm.response.headers"] = {"all", "get", "has", "toObject"};
    m["pm.response.to"] = {"be", "have", "not"};
    m["pm.response.to.have"] = {"header", "jsonBody", "status"};
    m["pm.response.to.be"] = {"clientError", "error", "json", "notFound", "ok", "serverError", "success"};
    m["pm.info"] = {"eventName", "requestName"};
    m["bru"] = {"deleteEnvVar", "deleteVar", "getEnvName", "getEnvVar", "getGlobalEnvVar", "getVar", "hasEnvVar",
                "hasVar", "interpolate", "sendRequest", "setEnvVar", "setGlobalEnvVar", "setVar", "sleep"};
    m["req"] = {"deleteHeader", "deleteQueryParam", "getBody", "getHeader", "getHeaders", "getMethod", "getName",
                "getQueryParam", "getUrl", "setBody", "setHeader", "setHeaders", "setMethod", "setQueryParam",
                "setUrl"};
    m["res"] = {"body",      "getBody",         "getHeader", "getHeaders", "getResponseTime", "getSize",
                "getStatus", "getStatusText",   "getTimings", "getUrl",    "headers",         "responseTime",
                "status",    "statusText"};
    m["console"] = {"debug", "error", "info", "log", "warn"};
    m["crypto"] = {"base64", "base64Decode", "getRandomValues", "hex", "hexDecode", "hmac", "md5", "randomUUID",
                   "sha1", "sha256", "sha384", "sha512"};
    m["CryptoJS"] = {"HmacMD5", "HmacSHA1", "HmacSHA256", "HmacSHA512", "MD5", "SHA1", "SHA256", "SHA512", "enc", "lib"};
    m["CryptoJS.enc"] = {"Base64", "Hex", "Latin1", "Utf8"};
    m["JSON"] = {"parse", "stringify"};
    m["Date"] = {"now", "parse"};
    m["Math"] = {"abs", "ceil", "floor", "max", "min", "random", "round"};
    m["Object"] = {"assign", "entries", "keys", "values"};
    m[""] = {"CryptoJS", "Date", "JSON", "Math", "Object", "atob", "btoa", "bru", "console", "crypto", "expect",
             "pm", "req", "require", "res", "setTimeout", "test"};
    return m;
  }();
  return t;
}

bool post_only(std::string_view chain, std::string_view member) {
  if (chain.empty()) return member == "res";
  if (chain == "pm") return member == "response";
  return chain == "res" || chain.substr(0, 11) == "pm.response";
}

}  // namespace

std::string js_chain_before(std::string_view text, size_t pos) {
  pos = std::min(pos, text.size());
  size_t b = pos;
  while (b > 0 && (js_ident_char(text[b - 1]) || text[b - 1] == '.')) --b;
  std::string_view c = text.substr(b, pos - b);
  // "x..y", ".y", "1a" and the like are not chains
  if (c.empty() || c.front() == '.' || c.back() == '.' || c.find("..") != std::string_view::npos) return {};
  if (c.front() >= '0' && c.front() <= '9') return {};
  return std::string(c);
}

std::vector<std::string> js_members(std::string_view chain, bool post) {
  const auto it = table().find(chain);
  if (it == table().end()) return {};
  if (!post && post_only(chain, "")) return {};
  std::vector<std::string> out;
  for (const auto& m : it->second)
    if (post || !post_only(chain, m)) out.push_back(m);
  std::sort(out.begin(), out.end());
  return out;
}

const std::vector<JsSnippet>& js_snippets(bool post) {
  static const std::vector<JsSnippet> pre = {
      {"Set a variable (this session)", "pm.variables.set(\"name\", \"value\");\n"},
      {"Set an environment variable", "pm.environment.set(\"name\", \"value\");\n"},
      {"Get an environment variable", "const value = pm.environment.get(\"name\");\n"},
      {"Add a header", "pm.request.headers.upsert({ key: \"X-Request-Id\", value: crypto.randomUUID() });\n"},
      {"Add a timestamp header", "pm.request.headers.upsert({ key: \"X-Timestamp\", value: Date.now().toString() });\n"},
      {"Set a query parameter", "pm.request.url.query.upsert({ key: \"t\", value: Date.now().toString() });\n"},
      {"Sign the request (HMAC-SHA256)",
       "const sig = crypto.hmac(\"sha256\", pm.environment.get(\"secret\"), pm.request.method + pm.request.url.toString());\n"
       "pm.request.headers.upsert({ key: \"X-Signature\", value: sig });\n"},
      {"Set a JSON body", "pm.request.body.raw = JSON.stringify({ id: 1, at: new Date().toISOString() });\n"},
      {"Fetch a token first (sendRequest)",
       "const res = await pm.sendRequest({ url: pm.variables.replaceIn(\"{{base}}/token\"), method: \"POST\" });\n"
       "pm.variables.set(\"token\", res.json().access_token);\n"},
      {"Bruno: set a variable", "bru.setVar(\"name\", \"value\");\n"},
      {"Bruno: set a header", "req.setHeader(\"X-Request-Id\", crypto.randomUUID());\n"},
      {"Log a message", "console.log(\"sending\", pm.request.method, pm.request.url.toString());\n"},
  };
  static const std::vector<JsSnippet> after = {
      {"Status code is 200", "pm.test(\"Status code is 200\", () => {\n  pm.response.to.have.status(200);\n});\n"},
      {"Response time is below 500 ms",
       "pm.test(\"Response time is below 500 ms\", () => {\n  pm.expect(pm.response.responseTime).to.be.below(500);\n});\n"},
      {"Body contains a string",
       "pm.test(\"Body contains text\", () => {\n  pm.expect(pm.response.text()).to.include(\"text\");\n});\n"},
      {"JSON value check",
       "pm.test(\"id is 1\", () => {\n  const json = pm.response.json();\n  pm.expect(json.id).to.eql(1);\n});\n"},
      {"Content-Type header is present",
       "pm.test(\"Content-Type is present\", () => {\n  pm.response.to.have.header(\"Content-Type\");\n});\n"},
      {"Response is JSON", "pm.test(\"Response is JSON\", () => {\n  pm.response.to.be.json;\n});\n"},
      {"Save a body value as a variable", "pm.environment.set(\"token\", pm.response.json().token);\n"},
      {"Save a header as a variable", "pm.variables.set(\"etag\", pm.response.headers.get(\"ETag\"));\n"},
      {"Bruno: status test", "test(\"status is 200\", () => {\n  expect(res.getStatus()).to.equal(200);\n});\n"},
      {"Bruno: save a body value", "bru.setVar(\"id\", res.getBody().id);\n"},
      {"Log the response body", "console.log(pm.response.text());\n"},
  };
  return post ? after : pre;
}

std::optional<JsIssue> js_lint(std::string_view s) {
  struct Frame {
    char open;   // ( [ { or '`' (template literal) or '$' (a ${ } inside one)
    size_t pos;
  };
  std::vector<Frame> st;
  const auto line_of = [&](size_t p) { return std::to_string(1 + std::count(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(p), '\n')); };
  // Whether a '/' here starts a regex literal: after an operator, an opening bracket, a keyword
  // that takes an expression, or at the start; after a value (identifier, number, ')' ']') it
  // divides.
  char prev = 0;            // the last significant character outside comments
  char prev2 = 0;           // the one before it (a++ / 2 divides)
  std::string prev_word;    // the identifier it ended, if any
  const auto regex_ok = [&] {
    if (prev == 0) return true;
    if (js_ident_char(prev)) {
      static const char* const kw[] = {"return", "typeof", "case", "do", "else", "in", "of", "new", "delete",
                                       "void", "throw", "instanceof", "yield", "await"};
      return std::any_of(std::begin(kw), std::end(kw), [&](const char* k) { return prev_word == k; });
    }
    if ((prev == '+' || prev == '-') && prev2 == prev) return false;
    return prev != ')' && prev != ']' && prev != '}' && prev != '"' && prev != '\'' && prev != '`';
  };
  size_t i = 0;
  const size_t n = s.size();
  bool in_template = false;  // scanning template text (top frame is '`')
  while (i < n) {
    if (in_template) {
      const char c = s[i];
      if (c == '\\') {
        i += 2;
        continue;
      }
      if (c == '`') {
        st.pop_back();
        in_template = false;
        prev = '`';
        prev_word.clear();
        ++i;
        continue;
      }
      if (c == '$' && i + 1 < n && s[i + 1] == '{') {
        st.push_back({'$', i});
        in_template = false;
        prev = '{';
        prev_word.clear();
        i += 2;
        continue;
      }
      ++i;
      continue;
    }
    const char c = s[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      ++i;
      continue;
    }
    if (c == '/' && i + 1 < n && s[i + 1] == '/') {
      while (i < n && s[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < n && s[i + 1] == '*') {
      const size_t e = s.find("*/", i + 2);
      if (e == std::string_view::npos) return JsIssue{i, "comment /* is never closed"};
      i = e + 2;
      continue;
    }
    if (c == '"' || c == '\'') {
      size_t j = i + 1;
      for (; j < n; ++j) {
        if (s[j] == '\\') {
          ++j;
          continue;
        }
        if (s[j] == c || s[j] == '\n') break;
      }
      if (j >= n || s[j] != c) return JsIssue{i, std::string("string ") + c + "..." + " is not closed on its line"};
      i = j + 1;
      prev = c;
      prev_word.clear();
      continue;
    }
    if (c == '`') {
      st.push_back({'`', i});
      in_template = true;
      ++i;
      continue;
    }
    if (c == '/' && regex_ok()) {
      size_t j = i + 1;
      bool cls = false;
      for (; j < n && s[j] != '\n'; ++j) {
        if (s[j] == '\\') {
          ++j;
          continue;
        }
        if (s[j] == '[') cls = true;
        else if (s[j] == ']') cls = false;
        else if (s[j] == '/' && !cls) break;
      }
      if (j >= n || s[j] != '/') return JsIssue{i, "regular expression /.../ is not closed on its line"};
      i = j + 1;
      while (i < n && js_ident_char(s[i])) ++i;  // flags
      prev = ')';  // a value: a '/' after it divides
      prev_word.clear();
      continue;
    }
    if (c == '(' || c == '[' || c == '{') {
      st.push_back({c, i});
    } else if (c == ')' || c == ']' || c == '}') {
      const char want = c == ')' ? '(' : c == ']' ? '[' : '{';
      if (st.empty()) return JsIssue{i, std::string("unexpected '") + c + "': nothing to close"};
      const Frame top = st.back();
      if (c == '}' && top.open == '$') {
        st.pop_back();
        in_template = true;  // back in the template text
        ++i;
        continue;
      }
      if (top.open != want) {
        const char close = top.open == '(' ? ')' : top.open == '[' ? ']' : '}';
        return JsIssue{i, std::string("unexpected '") + c + "': '" + (top.open == '$' ? std::string("${") : std::string(1, top.open)) +
                              "' from line " + line_of(top.pos) + " needs '" + close + "' first"};
      }
      st.pop_back();
    }
    if (js_ident_char(c)) {
      const size_t b = i;
      while (i < n && js_ident_char(s[i])) ++i;
      prev_word = std::string(s.substr(b, i - b));
      prev = s[i - 1];
      continue;
    }
    prev2 = prev;
    prev = c;
    prev_word.clear();
    ++i;
  }
  if (!st.empty()) {
    const Frame f = st.back();
    if (f.open == '`') return JsIssue{f.pos, "template literal `...` is never closed"};
    const std::string o = f.open == '$' ? "${" : std::string(1, f.open);
    const char close = f.open == '(' ? ')' : f.open == '[' ? ']' : '}';
    return JsIssue{f.pos, "'" + o + "' opened on line " + line_of(f.pos) + " is never closed (missing '" + close + "')"};
  }
  return std::nullopt;
}

}  // namespace watt::app::http_editor
