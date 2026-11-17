#include "app/http_editor_codegen.h"

#include <cstdio>
#include <string_view>
#include <utility>
#include <vector>

namespace watt::app::http_editor {

namespace {

bool code_valid_utf8(std::string_view s) {
  size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i]);
    size_t n = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : 99;
    if (n == 99 || i + n >= s.size() + (n ? 0 : 1)) return false;
    for (size_t k = 1; k <= n; ++k)
      if ((static_cast<unsigned char>(s[i + k]) >> 6) != 0x2) return false;
    i += n + 1;
  }
  return true;
}

std::string hex2(unsigned char c) {
  char b[8];
  std::snprintf(b, sizeof b, "%02x", c);
  return b;
}

// "..." for Python and JavaScript: quotes, backslashes and control characters escaped, other
// UTF-8 text kept as is (both languages read UTF-8 source).
std::string c_like_str(std::string_view s, bool js) {
  std::string o = "\"";
  for (size_t i = 0; i < s.size(); ++i) {
    const char ch = s[i];
    const auto c = static_cast<unsigned char>(ch);
    // U+2028 / U+2029 (UTF-8 E2 80 A8/A9) are line terminators in JavaScript: inside a // comment
    // they would end it. Escape them so the literal is safe in a comment too.
    if (js && c == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
        (static_cast<unsigned char>(s[i + 2]) == 0xA8 || static_cast<unsigned char>(s[i + 2]) == 0xA9)) {
      o += static_cast<unsigned char>(s[i + 2]) == 0xA8 ? "\\u2028" : "\\u2029";
      i += 2;
      continue;
    }
    switch (ch) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20 || c == 0x7F) o += (js ? "\\u00" : "\\x") + hex2(c);
        else o += ch;
    }
  }
  return o + "\"";
}

std::string py_bytes(std::string_view s) {
  std::string o = "b\"";
  for (char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    if (ch == '"' || ch == '\\') o += std::string("\\") + ch;
    else if (c >= 0x20 && c < 0x7F) o += ch;
    else o += "\\x" + hex2(c);
  }
  return o + "\"";
}

// POSIX shell single quotes.
std::string sh(std::string_view s) {
  std::string o = "'";
  for (char ch : s) {
    if (ch == '\'') o += "'\\''";
    else o += ch;
  }
  return o + "'";
}

std::vector<std::pair<std::string, std::string>> sent_headers(const SavedRequest& r) {
  std::vector<std::pair<std::string, std::string>> out;
  for (size_t i = 0; i < r.http.headers.size(); ++i)
    if (!r.disabled_headers.count(i)) out.push_back(r.http.headers[i]);
  return out;
}

bool has_form(const SavedRequest& r) {
  for (const auto& p : r.form)
    if (p.enabled) return true;
  return false;
}

std::string python(const SavedRequest& r) {
  const auto hs = sent_headers(r);
  std::string o = "import requests\n\n";
  o += "url = " + c_like_str(r.http.url, false) + "\n";
  if (!hs.empty()) {
    o += "headers = {\n";
    for (const auto& [k, v] : hs) o += "    " + c_like_str(k, false) + ": " + c_like_str(v, false) + ",\n";
    o += "}\n";
  }
  std::string body_arg;
  if (has_form(r)) {
    // multipart/form-data: every part goes in files= (a text part as (None, value)).
    o += "files = [\n";
    for (const auto& p : r.form) {
      if (!p.enabled) continue;
      const std::string name = c_like_str(p.name, false), value = c_like_str(p.value, false);
      if (p.kind == FormPart::Kind::File) o += "    (" + name + ", open(" + value + ", \"rb\")),\n";
      else if (p.kind == FormPart::Kind::FileContent) o += "    (" + name + ", (None, open(" + value + ", \"rb\").read())),\n";
      else o += "    (" + name + ", (None, " + value + ")),\n";
    }
    o += "]\n";
    body_arg = ", files=files";
  } else if (!r.body_file.empty()) {
    o += "data = open(" + c_like_str(r.body_file, false) + ", \"rb\")\n";
    body_arg = ", data=data";
  } else if (!r.body_data.empty()) {
    o += "# The body is built from curl -d arguments (files read at send time): see Copy as curl.\n";
  } else if (!r.http.body.empty()) {
    o += "data = " + (code_valid_utf8(r.http.body) ? c_like_str(r.http.body, false) + ".encode()" : py_bytes(r.http.body)) + "\n";
    body_arg = ", data=data";
  }
  o += "\nresponse = requests.request(" + c_like_str(r.http.method, false) + ", url";
  if (!hs.empty()) o += ", headers=headers";
  o += body_arg;
  if (r.basic_auth.enabled)
    o += ", auth=(" + c_like_str(r.basic_auth.user, false) + ", " + c_like_str(r.basic_auth.password, false) + ")";
  if (!r.http.follow_redirects) o += ", allow_redirects=False";
  if (!r.http.verify_tls) o += ", verify=False";
  if (r.http.timeout_ms > 0) {
    char t[32];
    std::snprintf(t, sizeof t, "%g", static_cast<double>(r.http.timeout_ms) / 1000.0);
    o += std::string(", timeout=") + t;
  }
  o += ")\nprint(response.status_code)\nprint(response.text)\n";
  return o;
}

std::string javascript(const SavedRequest& r) {
  auto hs = sent_headers(r);
  std::string o;
  std::string body;
  if (has_form(r)) {
    o += "const form = new FormData();\n";
    for (const auto& p : r.form) {
      if (!p.enabled) continue;
      if (p.kind == FormPart::Kind::Text)
        o += "form.append(" + c_like_str(p.name, true) + ", " + c_like_str(p.value, true) + ");\n";
      else
        // The path as an escaped literal, not raw comment text: a newline in it would end the
        // comment and turn the rest into code.
        o += "// form.append(" + c_like_str(p.name, true) + ", fileBlob); // file: " + c_like_str(p.value, true) + "\n";
    }
    body = "form";
  } else if (!r.body_file.empty()) {
    o += "// The body is the file " + c_like_str(r.body_file, true) + " (read it into a Blob / Buffer).\n";
  } else if (!r.body_data.empty()) {
    o += "// The body is built from curl -d arguments (files read at send time): see Copy as curl.\n";
  } else if (!r.http.body.empty()) {
    body = code_valid_utf8(r.http.body) ? c_like_str(r.http.body, true) : "/* binary body, not shown */ new Uint8Array()";
  }
  if (r.basic_auth.enabled)
    hs.emplace_back("Authorization", "\x01");  // built below with btoa()
  o += "const response = await fetch(" + c_like_str(r.http.url, true) + ", {\n";
  o += "  method: " + c_like_str(r.http.method, true) + ",\n";
  if (!hs.empty()) {
    o += "  headers: {\n";
    for (const auto& [k, v] : hs) {
      if (v == "\x01")
        o += "    " + c_like_str(k, true) + ": \"Basic \" + btoa(" +
             c_like_str(r.basic_auth.user + ":" + r.basic_auth.password, true) + "),\n";
      else
        o += "    " + c_like_str(k, true) + ": " + c_like_str(v, true) + ",\n";
    }
    o += "  },\n";
  }
  if (!body.empty()) o += "  body: " + body + ",\n";
  if (!r.http.follow_redirects) o += "  redirect: \"manual\",\n";
  if (r.http.timeout_ms > 0) o += "  signal: AbortSignal.timeout(" + std::to_string(r.http.timeout_ms) + "),\n";
  o += "});\nconsole.log(response.status, await response.text());\n";
  return o;
}

std::string httpie(const SavedRequest& r) {
  std::string o = "http";
  if (r.http.follow_redirects) o += " --follow";
  if (!r.http.verify_tls) o += " --verify=no";
  if (r.http.timeout_ms > 0) {
    char t[32];
    std::snprintf(t, sizeof t, "%g", static_cast<double>(r.http.timeout_ms) / 1000.0);
    o += std::string(" --timeout=") + t;
  }
  if (r.basic_auth.enabled) o += " -a " + sh(r.basic_auth.user + ":" + r.basic_auth.password);
  const bool form = has_form(r);
  if (form) o += " --multipart";
  if (!form && r.body_file.empty() && r.body_data.empty() && !r.http.body.empty()) o += " --raw " + sh(r.http.body);
  // "--" ends HTTPie's (argparse) options: everything after it is METHOD, URL and request items,
  // so a saved header/form name, method or URL that starts with '-' (e.g. --output=...) can
  // never be read as an option. Quoting alone stops the shell, not the option parser.
  o += " -- " + sh(r.http.method) + " " + sh(r.http.url);
  for (const auto& [k, v] : sent_headers(r)) o += " " + sh(k + (v.empty() ? ";" : ":" + v));
  if (form) {
    for (const auto& p : r.form) {
      if (!p.enabled) continue;
      const char* op = p.kind == FormPart::Kind::File ? "@" : p.kind == FormPart::Kind::FileContent ? "=@" : "=";
      o += " " + sh(p.name + op + p.value);
    }
  } else if (!r.body_file.empty()) {
    o += " < " + sh(r.body_file);
  } else if (!r.body_data.empty()) {
    o += "  # body from curl -d arguments: see Copy as curl";
  }
  return o + "\n";
}

}  // namespace

const char* code_lang_name(CodeLang lang) {
  switch (lang) {
    case CodeLang::PythonRequests: return "Python (requests)";
    case CodeLang::JavaScriptFetch: return "JavaScript (fetch)";
    case CodeLang::HTTPie: return "HTTPie";
  }
  return "";
}

std::string to_code(const SavedRequest& r, CodeLang lang) {
  switch (lang) {
    case CodeLang::PythonRequests: return python(r);
    case CodeLang::JavaScriptFetch: return javascript(r);
    case CodeLang::HTTPie: return httpie(r);
  }
  return {};
}

}  // namespace watt::app::http_editor
