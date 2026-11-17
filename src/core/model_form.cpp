// Multipart form parts: curl-compatible body building and -F argument formatting.
#include "core/model_form.h"

#include <random>

#include "core/model_json.h"

namespace watt {

namespace fs = std::filesystem;

namespace {

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

std::string_view base_name(std::string_view path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// curl (MIMESTRATEGY_FORM) percent-escapes these in Content-Disposition names and filenames.
std::string escape_disposition(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '"') out += "%22";
    else if (c == '\r') out += "%0D";
    else if (c == '\n') out += "%0A";
    else out += c;
  }
  return out;
}

std::string random_boundary() {
  static thread_local std::mt19937_64 g{std::random_device{}()};
  static const char* chars = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  std::string b(24, '-');
  for (int i = 0; i < 22; ++i) b += chars[g() % 62];
  return b;
}

// Relative paths: against base_dir only when one is given (no fallback to the process cwd).
bool read_part_file(const std::string& path, const fs::path& base_dir, std::string& out) {
  const fs::path p = detail::from_utf8(path);
  if (p.is_relative() && !base_dir.empty()) return detail::read_file(base_dir / p, out);
  return detail::read_file(p, out);
}

// A word curl's -F parser reads back unchanged: quoted ("..." with \" and \\) when it holds the
// ';' separator (',' too for file names, which curl splits into a file list), starts with a
// quote, has blanks at either end (dropped unquoted), or is a text value that starts with a
// character the parser treats specially.
std::string form_word(const std::string& s, bool is_value, bool is_path = false) {
  bool quote = s.find(';') != std::string::npos || (is_path && s.find(',') != std::string::npos) ||
               (!s.empty() && (s.front() == '"' || s.front() == ' ' || s.front() == '\t' || s.back() == ' ' ||
                               s.back() == '\t'));
  if (is_value && !s.empty() && (s[0] == '@' || s[0] == '<' || s[0] == '(')) quote = true;
  if (!quote) return s;
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  out += '"';
  return out;
}

}  // namespace

std::string_view mime_type_for_filename(std::string_view filename) {
  static const std::pair<std::string_view, std::string_view> table[] = {
      {".gif", "image/gif"},   {".jpg", "image/jpeg"},     {".jpeg", "image/jpeg"},
      {".png", "image/png"},   {".svg", "image/svg+xml"},  {".txt", "text/plain"},
      {".htm", "text/html"},   {".html", "text/html"},     {".pdf", "application/pdf"},
      {".xml", "application/xml"}};
  for (const auto& [ext, type] : table)
    if (filename.size() >= ext.size() && iequals(filename.substr(filename.size() - ext.size()), ext)) return type;
  return {};
}

std::string form_part_content_type(const FormPart& p) {
  if (!p.type.empty()) return p.type;
  switch (p.kind) {
    case FormPart::Kind::File: {
      std::string_view t = mime_type_for_filename(p.filename.empty() ? base_name(p.value) : p.filename);
      if (t.empty()) t = mime_type_for_filename(p.value);
      return std::string(t.empty() ? "application/octet-stream" : t);
    }
    default:
      return std::string(mime_type_for_filename(p.filename));
  }
}

bool build_form_body(const std::vector<FormPart>& parts, HttpRequest& r, const fs::path& base_dir, std::string* err) {
  std::string boundary;
  // A user-set Content-Type (multipart/mixed, or a fixed boundary) is kept, as curl does.
  Header* ct = nullptr;
  for (auto& h : r.headers)
    if (iequals(h.first, "content-type")) ct = &h;
  if (ct && ct->second.find_first_not_of(" \t") == std::string::npos) ct->second = "multipart/form-data";
  if (ct) {
    const size_t b = ct->second.find("boundary=");
    if (b != std::string::npos) {
      boundary = ct->second.substr(b + 9);
      boundary = boundary.substr(0, boundary.find(';'));
      if (boundary.size() >= 2 && boundary.front() == '"' && boundary.back() == '"')
        boundary = boundary.substr(1, boundary.size() - 2);
    }
  }
  const bool own_boundary = boundary.empty();
  if (own_boundary) boundary = random_boundary();

  std::string body;
  std::string data;
  for (const auto& p : parts) {
    if (!p.enabled) continue;
    const std::string* content = &p.value;
    std::string filename = p.filename;
    if (p.kind != FormPart::Kind::Text) {
      if (!read_part_file(p.value, base_dir, data)) {
        if (err) *err = "cannot read form file '" + p.value + "' for part '" + p.name + "'";
        return false;
      }
      content = &data;
      if (p.kind == FormPart::Kind::File && filename.empty()) filename = std::string(base_name(p.value));
    }
    body.reserve(body.size() + content->size() + 160);
    body += "--";
    body += boundary;
    body += "\r\nContent-Disposition: form-data; name=\"";
    body += escape_disposition(p.name);
    body += '"';
    if (!filename.empty()) {
      body += "; filename=\"";
      body += escape_disposition(filename);
      body += '"';
    }
    body += "\r\n";
    const std::string type = form_part_content_type(p);
    if (!type.empty()) body += "Content-Type: " + type + "\r\n";
    body += "\r\n";
    body += *content;
    body += "\r\n";
  }
  body += "--" + boundary + "--\r\n";

  if (ct) {
    if (own_boundary) ct->second += "; boundary=" + boundary;
  } else {
    r.headers.emplace_back("Content-Type", "multipart/form-data; boundary=" + boundary);
  }
  r.body = std::move(body);
  return true;
}

std::pair<std::string, std::string> form_part_curl_arg(const FormPart& p) {
  std::string params;
  if (!p.type.empty()) {
    // curl's type= ends at ';': a type with parameters goes through headers= instead.
    if (p.type.find(';') == std::string::npos) params += ";type=" + p.type;
    else params += ";headers=" + form_word("Content-Type: " + p.type, false);
  }
  if (!p.filename.empty() && !(p.kind == FormPart::Kind::File && p.filename == base_name(p.value)))
    params += ";filename=" + form_word(p.filename, false);
  switch (p.kind) {
    case FormPart::Kind::File: return {"-F", p.name + "=@" + form_word(p.value, false, true) + params};
    case FormPart::Kind::FileContent: return {"-F", p.name + "=<" + form_word(p.value, false, true) + params};
    default: break;
  }
  if (params.empty()) {
    if (form_word(p.value, true) == p.value) return {"-F", p.name + "=" + p.value};
    return {"--form-string", p.name + "=" + p.value};  // sent verbatim, no quoting rules
  }
  return {"-F", p.name + "=" + form_word(p.value, true) + params};
}

}  // namespace watt
