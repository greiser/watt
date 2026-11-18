#include "app/http_editor_codegen.h"

#include <algorithm>
#include <cctype>
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

bool iequal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

std::string base_name(std::string_view path) {
  const size_t s = path.find_last_of("/\\");
  return std::string(s == std::string_view::npos ? path : path.substr(s + 1));
}

// What the body is, for the languages below: a text body, or a reason it is not shown.
struct BodyInfo {
  bool form = false;
  bool text = false;        // r.http.body as a UTF-8 string
  bool binary = false;      // r.http.body, not UTF-8 (not shown)
  bool file = false;        // r.body_file
  bool curl_data = false;   // r.body_data (built at send time)
};
BodyInfo body_info(const SavedRequest& r) {
  BodyInfo b;
  if (has_form(r)) b.form = true;
  else if (!r.body_file.empty()) b.file = true;
  else if (!r.body_data.empty()) b.curl_data = true;
  else if (!r.http.body.empty()) (code_valid_utf8(r.http.body) ? b.text : b.binary) = true;
  return b;
}

std::string node_axios(const SavedRequest& r) {
  const auto hs = sent_headers(r);
  const BodyInfo b = body_info(r);
  std::string o = "import axios from \"axios\";\n";
  if (!r.http.verify_tls) o += "import https from \"node:https\";\n";
  bool files = false;
  for (const auto& p : r.form) files |= p.enabled && p.kind != FormPart::Kind::Text;
  if (files || b.file) o += "import fs from \"node:fs\";\n";
  o += "\n";
  std::string data;
  if (b.form) {
    o += "const form = new FormData();\n";
    for (const auto& p : r.form) {
      if (!p.enabled) continue;
      const std::string name = c_like_str(p.name, true), value = c_like_str(p.value, true);
      if (p.kind == FormPart::Kind::Text) o += "form.append(" + name + ", " + value + ");\n";
      else if (p.kind == FormPart::Kind::File)
        o += "form.append(" + name + ", new Blob([fs.readFileSync(" + value + ")]), " +
             c_like_str(p.filename.empty() ? base_name(p.value) : p.filename, true) + ");\n";
      else o += "form.append(" + name + ", fs.readFileSync(" + value + ", \"utf8\"));\n";
    }
    data = "form";
  } else if (b.file) {
    data = "fs.readFileSync(" + c_like_str(r.body_file, true) + ")";
  } else if (b.text) {
    data = c_like_str(r.http.body, true);
  } else if (b.curl_data) {
    o += "// The body is built from curl -d arguments (files read at send time): see Copy as curl.\n";
  } else if (b.binary) {
    o += "// The body is binary and not shown.\n";
  }
  std::string m = r.http.method;
  o += "const response = await axios({\n";
  o += "  method: " + c_like_str(m, true) + ",\n";
  o += "  url: " + c_like_str(r.http.url, true) + ",\n";
  if (!hs.empty()) {
    o += "  headers: {\n";
    for (const auto& [k, v] : hs) o += "    " + c_like_str(k, true) + ": " + c_like_str(v, true) + ",\n";
    o += "  },\n";
  }
  if (!data.empty()) o += "  data: " + data + ",\n";
  if (r.basic_auth.enabled)
    o += "  auth: { username: " + c_like_str(r.basic_auth.user, true) + ", password: " +
         c_like_str(r.basic_auth.password, true) + " },\n";
  if (!r.http.follow_redirects) o += "  maxRedirects: 0,\n";
  if (r.http.timeout_ms > 0) o += "  timeout: " + std::to_string(r.http.timeout_ms) + ",\n";
  if (!r.http.verify_tls) o += "  httpsAgent: new https.Agent({ rejectUnauthorized: false }),\n";
  o += "  responseType: \"text\",\n";
  o += "  transformResponse: [(d) => d],\n";
  o += "  validateStatus: () => true,\n";
  o += "});\nconsole.log(response.status);\nconsole.log(response.data);\n";
  return o;
}

std::string go(const SavedRequest& r) {
  const auto hs = sent_headers(r);
  const BodyInfo b = body_info(r);
  bool form_files = false;
  for (const auto& p : r.form) form_files |= p.enabled && p.kind != FormPart::Kind::Text;
  std::vector<std::string> imports = {"fmt", "io", "net/http"};
  if (b.text) imports.push_back("strings");
  if (b.form) {
    imports.push_back("bytes");
    imports.push_back("mime/multipart");
  }
  if (b.file || form_files) imports.push_back("os");
  if (form_files) imports.push_back("path/filepath");
  if (r.http.timeout_ms > 0) imports.push_back("time");
  if (!r.http.verify_tls) imports.push_back("crypto/tls");
  std::sort(imports.begin(), imports.end());
  std::string o = "package main\n\nimport (\n";
  for (const auto& i : imports) o += "\t\"" + i + "\"\n";
  o += ")\n\nfunc main() {\n";
  std::string body = "nil";
  if (b.form) {
    o += "\tvar buf bytes.Buffer\n\tw := multipart.NewWriter(&buf)\n";
    for (const auto& p : r.form) {
      if (!p.enabled) continue;
      const std::string name = c_like_str(p.name, false), value = c_like_str(p.value, false);
      if (p.kind == FormPart::Kind::Text) {
        o += "\tw.WriteField(" + name + ", " + value + ")\n";
      } else if (p.kind == FormPart::Kind::File) {
        o += "\t{\n\t\tf, err := os.Open(" + value + ")\n\t\tif err != nil {\n\t\t\tpanic(err)\n\t\t}\n";
        o += "\t\tfw, _ := w.CreateFormFile(" + name + ", " +
             (p.filename.empty() ? "filepath.Base(" + value + ")" : c_like_str(p.filename, false)) + ")\n";
        o += "\t\tio.Copy(fw, f)\n\t\tf.Close()\n\t}\n";
      } else {
        o += "\t{\n\t\tdata, err := os.ReadFile(" + value + ")\n\t\tif err != nil {\n\t\t\tpanic(err)\n\t\t}\n";
        o += "\t\tw.WriteField(" + name + ", string(data))\n\t}\n";
      }
    }
    o += "\tw.Close()\n";
    body = "&buf";
  } else if (b.file) {
    o += "\tbody, err := os.Open(" + c_like_str(r.body_file, false) + ")\n\tif err != nil {\n\t\tpanic(err)\n\t}\n";
    o += "\tdefer body.Close()\n";
    body = "body";
  } else if (b.text) {
    body = "strings.NewReader(" + c_like_str(r.http.body, false) + ")";
  } else if (b.curl_data) {
    o += "\t// The body is built from curl -d arguments (files read at send time): see Copy as curl.\n";
  } else if (b.binary) {
    o += "\t// The body is binary and not shown.\n";
  }
  o += "\treq, err := http.NewRequest(" + c_like_str(r.http.method, false) + ", " + c_like_str(r.http.url, false) +
       ", " + body + ")\n\tif err != nil {\n\t\tpanic(err)\n\t}\n";
  for (const auto& [k, v] : hs) {
    if (iequal(k, "Host")) o += "\treq.Host = " + c_like_str(v, false) + "\n";
    else o += "\treq.Header.Add(" + c_like_str(k, false) + ", " + c_like_str(v, false) + ")\n";
  }
  if (b.form) o += "\treq.Header.Set(\"Content-Type\", w.FormDataContentType())\n";
  if (r.basic_auth.enabled)
    o += "\treq.SetBasicAuth(" + c_like_str(r.basic_auth.user, false) + ", " + c_like_str(r.basic_auth.password, false) + ")\n";
  o += "\tclient := &http.Client{";
  std::vector<std::string> fields;
  if (r.http.timeout_ms > 0) fields.push_back("Timeout: " + std::to_string(r.http.timeout_ms) + " * time.Millisecond");
  if (!r.http.verify_tls)
    fields.push_back("Transport: &http.Transport{TLSClientConfig: &tls.Config{InsecureSkipVerify: true}}");
  for (size_t i = 0; i < fields.size(); ++i) o += (i ? ", " : "") + fields[i];
  o += "}\n";
  if (!r.http.follow_redirects)
    o += "\tclient.CheckRedirect = func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }\n";
  o += "\tresp, err := client.Do(req)\n\tif err != nil {\n\t\tpanic(err)\n\t}\n\tdefer resp.Body.Close()\n";
  o += "\tdata, _ := io.ReadAll(resp.Body)\n\tfmt.Println(resp.StatusCode)\n\tfmt.Println(string(data))\n}\n";
  return o;
}

// Headers java.net.http refuses to set (IllegalArgumentException).
bool java_restricted(std::string_view k) {
  for (const char* h : {"Connection", "Content-Length", "Expect", "Host", "Upgrade"})
    if (iequal(k, h)) return true;
  return false;
}

std::string java(const SavedRequest& r) {
  const auto hs = sent_headers(r);
  const BodyInfo b = body_info(r);
  std::string o = "import java.net.URI;\nimport java.net.http.HttpClient;\nimport java.net.http.HttpRequest;\n"
                  "import java.net.http.HttpResponse;\n";
  if (r.http.timeout_ms > 0) o += "import java.time.Duration;\n";
  if (r.basic_auth.enabled) o += "import java.nio.charset.StandardCharsets;\nimport java.util.Base64;\n";
  if (b.file) o += "import java.nio.file.Path;\n";
  o += "\npublic class Main {\n  public static void main(String[] args) throws Exception {\n";
  if (!r.http.verify_tls) o += "    // TLS verification is off for this request: java.net.http needs a trust-all SSLContext for that.\n";
  o += "    HttpClient client = HttpClient.newBuilder()\n";
  o += std::string("        .followRedirects(HttpClient.Redirect.") + (r.http.follow_redirects ? "NORMAL" : "NEVER") + ")\n";
  o += "        .build();\n";
  std::string pub = "HttpRequest.BodyPublishers.noBody()";
  if (b.text) pub = "HttpRequest.BodyPublishers.ofString(" + c_like_str(r.http.body, true) + ")";
  else if (b.file) pub = "HttpRequest.BodyPublishers.ofFile(Path.of(" + c_like_str(r.body_file, true) + "))";
  else if (b.form) o += "    // multipart/form-data body: build it with a multipart library, or see Copy as curl.\n";
  else if (b.curl_data) o += "    // The body is built from curl -d arguments (files read at send time): see Copy as curl.\n";
  else if (b.binary) o += "    // The body is binary and not shown.\n";
  o += "    HttpRequest request = HttpRequest.newBuilder()\n";
  o += "        .uri(URI.create(" + c_like_str(r.http.url, true) + "))\n";
  if (r.http.timeout_ms > 0) o += "        .timeout(Duration.ofMillis(" + std::to_string(r.http.timeout_ms) + "))\n";
  for (const auto& [k, v] : hs) {
    if (java_restricted(k)) o += "        // .header(" + c_like_str(k, true) + ", ...) is set by the client\n";
    else o += "        .header(" + c_like_str(k, true) + ", " + c_like_str(v, true) + ")\n";
  }
  if (r.basic_auth.enabled)
    o += "        .header(\"Authorization\", \"Basic \" + Base64.getEncoder().encodeToString(" +
         c_like_str(r.basic_auth.user + ":" + r.basic_auth.password, true) + ".getBytes(StandardCharsets.UTF_8)))\n";
  o += "        .method(" + c_like_str(r.http.method, true) + ", " + pub + ")\n        .build();\n";
  o += "    HttpResponse<String> response = client.send(request, HttpResponse.BodyHandlers.ofString());\n";
  o += "    System.out.println(response.statusCode());\n    System.out.println(response.body());\n  }\n}\n";
  return o;
}

bool content_header(std::string_view k) {
  for (const char* h : {"Allow", "Content-Disposition", "Content-Encoding", "Content-Language", "Content-Length",
                        "Content-Location", "Content-MD5", "Content-Range", "Content-Type", "Expires", "Last-Modified"})
    if (iequal(k, h)) return true;
  return false;
}

std::string csharp(const SavedRequest& r) {
  const auto hs = sent_headers(r);
  const BodyInfo b = body_info(r);
  std::string o = "using System.Net.Http;\n";
  if (r.basic_auth.enabled) o += "using System.Net.Http.Headers;\n";
  o += "using System.Text;\n\n";
  std::vector<std::string> h;
  if (!r.http.follow_redirects) h.push_back("AllowAutoRedirect = false");
  if (!r.http.verify_tls)
    h.push_back("ServerCertificateCustomValidationCallback = HttpClientHandler.DangerousAcceptAnyServerCertificateValidator");
  o += "var handler = new HttpClientHandler";
  if (h.empty()) {
    o += "();\n";
  } else {
    o += " {";
    for (size_t i = 0; i < h.size(); ++i) o += (i ? ", " : " ") + h[i];
    o += " };\n";
  }
  o += "using var client = new HttpClient(handler)";
  if (r.http.timeout_ms > 0) o += " { Timeout = TimeSpan.FromMilliseconds(" + std::to_string(r.http.timeout_ms) + ") }";
  o += ";\n";
  o += "using var request = new HttpRequestMessage(new HttpMethod(" + c_like_str(r.http.method, true) + "), " +
       c_like_str(r.http.url, true) + ");\n";
  bool content = false;
  if (b.text) {
    o += "request.Content = new ByteArrayContent(Encoding.UTF8.GetBytes(" + c_like_str(r.http.body, true) + "));\n";
    content = true;
  } else if (b.file) {
    o += "request.Content = new StreamContent(File.OpenRead(" + c_like_str(r.body_file, true) + "));\n";
    content = true;
  } else if (b.form) {
    o += "var form = new MultipartFormDataContent();\n";
    for (const auto& p : r.form) {
      if (!p.enabled) continue;
      const std::string name = c_like_str(p.name, true), value = c_like_str(p.value, true);
      if (p.kind == FormPart::Kind::Text) o += "form.Add(new StringContent(" + value + "), " + name + ");\n";
      else if (p.kind == FormPart::Kind::File)
        o += "form.Add(new StreamContent(File.OpenRead(" + value + ")), " + name + ", " +
             c_like_str(p.filename.empty() ? base_name(p.value) : p.filename, true) + ");\n";
      else o += "form.Add(new StringContent(File.ReadAllText(" + value + ")), " + name + ");\n";
    }
    o += "request.Content = form;\n";
    content = true;
  } else if (b.curl_data) {
    o += "// The body is built from curl -d arguments (files read at send time): see Copy as curl.\n";
  } else if (b.binary) {
    o += "// The body is binary and not shown.\n";
  }
  for (const auto& [k, v] : hs) {
    if (content_header(k)) {
      if (!content || (b.form && iequal(k, "Content-Type"))) continue;
      o += "request.Content.Headers.Remove(" + c_like_str(k, true) + ");\n";
      o += "request.Content.Headers.TryAddWithoutValidation(" + c_like_str(k, true) + ", " + c_like_str(v, true) + ");\n";
    } else {
      o += "request.Headers.TryAddWithoutValidation(" + c_like_str(k, true) + ", " + c_like_str(v, true) + ");\n";
    }
  }
  if (r.basic_auth.enabled)
    o += "request.Headers.Authorization = new AuthenticationHeaderValue(\"Basic\", Convert.ToBase64String(Encoding.UTF8.GetBytes(" +
         c_like_str(r.basic_auth.user + ":" + r.basic_auth.password, true) + ")));\n";
  o += "using var response = await client.SendAsync(request);\n";
  o += "Console.WriteLine((int)response.StatusCode);\nConsole.WriteLine(await response.Content.ReadAsStringAsync());\n";
  return o;
}

// PowerShell single-quoted string: only ' is special (doubled; the curly quotes count too).
std::string ps(std::string_view s) {
  std::string o = "'";
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\'') {
      o += "''";
      continue;
    }
    // U+2018..U+201B are quote characters to PowerShell as well.
    if (static_cast<unsigned char>(s[i]) == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
        static_cast<unsigned char>(s[i + 2]) >= 0x98 && static_cast<unsigned char>(s[i + 2]) <= 0x9B) {
      o.append(s.substr(i, 3));
      o.append(s.substr(i, 3));
      i += 2;
      continue;
    }
    o += s[i];
  }
  return o + "'";
}

std::string powershell(const SavedRequest& r) {
  auto hs = sent_headers(r);
  const BodyInfo b = body_info(r);
  std::string o;
  std::string content_type;
  std::vector<std::pair<std::string, std::string>> shown;
  for (const auto& [k, v] : hs) {
    if (iequal(k, "Content-Type")) {
      content_type = v;
      continue;
    }
    if (std::any_of(shown.begin(), shown.end(), [&](const auto& x) { return iequal(x.first, k); })) {
      o += "# A second " + k + " header is not possible in a hashtable: only the first is sent.\n";
      continue;
    }
    shown.emplace_back(k, v);
  }
  if (r.basic_auth.enabled)
    shown.emplace_back("Authorization", "\x01");
  if (!shown.empty()) {
    o += "$headers = [ordered]@{\n";
    for (const auto& [k, v] : shown) {
      if (v == "\x01")
        o += "  " + ps(k) + " = 'Basic ' + [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(" +
             ps(r.basic_auth.user + ":" + r.basic_auth.password) + "))\n";
      else
        o += "  " + ps(k) + " = " + ps(v) + "\n";
    }
    o += "}\n";
  }
  std::string args;
  if (b.text) {
    o += "$body = " + ps(r.http.body) + "\n";
    args += " -Body ([Text.Encoding]::UTF8.GetBytes($body))";
  } else if (b.file) {
    args += " -InFile " + ps(r.body_file);
  } else if (b.form) {
    o += "$form = @{\n";
    for (const auto& p : r.form) {
      if (!p.enabled) continue;
      if (p.kind == FormPart::Kind::Text) o += "  " + ps(p.name) + " = " + ps(p.value) + "\n";
      else if (p.kind == FormPart::Kind::File) o += "  " + ps(p.name) + " = Get-Item -LiteralPath " + ps(p.value) + "\n";
      else o += "  " + ps(p.name) + " = Get-Content -Raw -LiteralPath " + ps(p.value) + "\n";
    }
    o += "}\n";
    args += " -Form $form";  // PowerShell 7+
  } else if (b.curl_data) {
    o += "# The body is built from curl -d arguments (files read at send time): see Copy as curl.\n";
  } else if (b.binary) {
    o += "# The body is binary and not shown.\n";
  }
  static const char* const std_methods[] = {"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS", "TRACE"};
  const bool std_method = std::any_of(std::begin(std_methods), std::end(std_methods),
                                      [&](const char* m) { return r.http.method == m; });
  o += "$response = Invoke-RestMethod -Uri " + ps(r.http.url) +
       (std_method ? " -Method " + ps(r.http.method) : " -CustomMethod " + ps(r.http.method));
  if (!shown.empty()) o += " -Headers $headers";
  if (!content_type.empty() && !b.form) o += " -ContentType " + ps(content_type);
  o += args;
  if (r.http.timeout_ms > 0) o += " -TimeoutSec " + std::to_string((r.http.timeout_ms + 999) / 1000);
  if (!r.http.follow_redirects) o += " -MaximumRedirection 0";
  if (!r.http.verify_tls) o += " -SkipCertificateCheck";
  o += "\n$response\n";
  return o;
}

}  // namespace

const char* code_lang_name(CodeLang lang) {
  switch (lang) {
    case CodeLang::PythonRequests: return "Python (requests)";
    case CodeLang::JavaScriptFetch: return "JavaScript (fetch)";
    case CodeLang::HTTPie: return "HTTPie";
    case CodeLang::NodeAxios: return "Node.js (axios)";
    case CodeLang::Go: return "Go (net/http)";
    case CodeLang::Java: return "Java (HttpClient)";
    case CodeLang::CSharp: return "C# (HttpClient)";
    case CodeLang::PowerShell: return "PowerShell (Invoke-RestMethod)";
  }
  return "";
}

void drop_shadowed_basic(SavedRequest& r) {
  if (!r.basic_auth.enabled) return;
  auto is_auth = [](const std::string& k) {
    static constexpr std::string_view a = "authorization";
    if (k.size() != a.size()) return false;
    for (size_t i = 0; i < k.size(); ++i)
      if (static_cast<char>(std::tolower(static_cast<unsigned char>(k[i]))) != a[i]) return false;
    return true;
  };
  for (size_t i = 0; i < r.http.headers.size(); ++i)
    if (!r.disabled_headers.count(i) && is_auth(r.http.headers[i].first)) {
      r.basic_auth = BasicAuth{};
      return;
    }
}

std::string to_code(const SavedRequest& r, CodeLang lang) {
  switch (lang) {
    case CodeLang::PythonRequests: return python(r);
    case CodeLang::JavaScriptFetch: return javascript(r);
    case CodeLang::HTTPie: return httpie(r);
    case CodeLang::NodeAxios: return node_axios(r);
    case CodeLang::Go: return go(r);
    case CodeLang::Java: return java(r);
    case CodeLang::CSharp: return csharp(r);
    case CodeLang::PowerShell: return powershell(r);
  }
  return {};
}

}  // namespace watt::app::http_editor
