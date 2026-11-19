#include "app/sidebar_specs.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <set>
#include <string_view>
#include <system_error>

namespace watt::app::sidebar {

namespace fs = std::filesystem;

namespace {

std::string spec_lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool is_spec_ext(const fs::path& p) {
  const std::string e = spec_lower(p.extension().string());
  return e == ".yaml" || e == ".yml" || e == ".json";
}

std::string spec_trim(std::string_view s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return std::string(s.substr(a, b - a));
}

// A YAML scalar on one line: quotes removed, a trailing " # comment" cut.
std::string spec_yaml_scalar(std::string_view v) {
  std::string s = spec_trim(v);
  if (!s.empty() && (s[0] == '"' || s[0] == '\'')) {
    const char q = s[0];
    const size_t e = s.find(q, 1);
    return e == std::string::npos ? s.substr(1) : s.substr(1, e - 1);
  }
  if (const size_t h = s.find(" #"); h != std::string::npos) s = spec_trim(s.substr(0, h));
  return s;
}

// "key": "value" in JSON text from `from`: the string value (empty when not a string).
std::string spec_json_value(std::string_view t, std::string_view key, size_t from, size_t* at = nullptr) {
  const std::string k = "\"" + std::string(key) + "\"";
  for (size_t i = t.find(k, from); i != std::string_view::npos; i = t.find(k, i + 1)) {
    size_t j = i + k.size();
    while (j < t.size() && std::isspace(static_cast<unsigned char>(t[j]))) ++j;
    if (j >= t.size() || t[j] != ':') continue;
    ++j;
    while (j < t.size() && std::isspace(static_cast<unsigned char>(t[j]))) ++j;
    if (at) *at = i;
    if (j >= t.size() || t[j] != '"') return {};
    std::string out;
    for (++j; j < t.size() && t[j] != '"' && out.size() < 200; ++j) {
      if (t[j] == '\\' && j + 1 < t.size()) ++j;
      out += t[j];
    }
    return out;
  }
  return {};
}

}  // namespace

fs::path specs_dir(const fs::path& root) { return root / L"specs"; }

SpecInfo peek_spec(const fs::path& file) {
  SpecInfo s;
  s.file = file;
  std::error_code ec;
  if (!fs::is_regular_file(file, ec)) {
    s.missing = true;
    return s;
  }
  s.size = fs::file_size(file, ec);
  std::ifstream in(file, std::ios::binary);
  std::string text(16 * 1024, '\0');
  in.read(text.data(), static_cast<std::streamsize>(text.size()));
  text.resize(static_cast<size_t>(std::max<std::streamsize>(0, in.gcount())));
  std::string_view t = text;
  if (t.substr(0, 3) == "\xEF\xBB\xBF") t.remove_prefix(3);
  size_t first = 0;
  while (first < t.size() && std::isspace(static_cast<unsigned char>(t[first]))) ++first;
  if (first == t.size()) return s;
  std::string key;
  if (t[first] == '{') {
    if (std::string v = spec_json_value(t, "openapi", first); !v.empty()) {
      key = "OpenAPI";
      s.version = v;
    } else if (std::string w = spec_json_value(t, "swagger", first); !w.empty()) {
      key = "Swagger";
      s.version = w;
    }
    if (!key.empty()) {
      size_t info = std::string_view::npos;
      const size_t i = t.find("\"info\"");
      if (i != std::string_view::npos) s.title = spec_json_value(t, "title", i, &info);
    }
  } else {
    // Top-level keys start at column 0; info's title is the first indented "title:" after "info:".
    bool in_info = false;
    for (size_t i = 0; i < t.size();) {
      const size_t e = std::min(t.find('\n', i), t.size());
      std::string_view line = t.substr(i, e - i);
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      i = e + 1;
      if (line.empty() || line[0] == '#') continue;
      const bool top = !std::isspace(static_cast<unsigned char>(line[0]));
      if (top) {
        in_info = line.substr(0, 5) == "info:";
        if (line.substr(0, 8) == "openapi:") {
          key = "OpenAPI";
          s.version = spec_yaml_scalar(line.substr(8));
        } else if (line.substr(0, 8) == "swagger:") {
          key = "Swagger";
          s.version = spec_yaml_scalar(line.substr(8));
        }
        continue;
      }
      if (in_info && s.title.empty()) {
        const std::string l = spec_trim(line);
        if (l.rfind("title:", 0) == 0) s.title = spec_yaml_scalar(std::string_view(l).substr(6));
      }
    }
  }
  if (!key.empty()) s.kind = key + (s.version.empty() ? "" : " " + s.version);
  return s;
}

std::vector<SpecInfo> scan_specs(const fs::path& root, const std::vector<fs::path>& recorded) {
  std::vector<SpecInfo> out;
  std::vector<fs::path> files;
  std::error_code ec;
  const fs::path dir = specs_dir(root);
  if (fs::is_directory(dir, ec)) {
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end && files.size() < 200; it.increment(ec)) {
      if (it.depth() >= 1) it.disable_recursion_pending();
      std::error_code ec2;
      if (it->is_symlink(ec2)) {
        if (it->is_directory(ec2)) it.disable_recursion_pending();
        continue;  // never follow a link out of the workspace
      }
      if (it->is_regular_file(ec2) && is_spec_ext(it->path())) files.push_back(it->path());
    }
  }
  std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
    return spec_lower(a.generic_string()) < spec_lower(b.generic_string());
  });
  std::set<std::string> seen;
  for (const auto& f : files) {
    SpecInfo s = peek_spec(f);
    if (s.kind.empty()) continue;  // some other JSON / YAML file
    seen.insert(spec_lower(f.lexically_normal().generic_string()));
    out.push_back(std::move(s));
  }
  for (const auto& f : recorded) {
    if (f.empty() || !seen.insert(spec_lower(f.lexically_normal().generic_string())).second) continue;
    SpecInfo s = peek_spec(f);
    s.imported = true;
    out.push_back(std::move(s));
  }
  return out;
}

bool in_workspace(const fs::path& p, const fs::path& root) {
  if (p.empty() || root.empty()) return false;
  auto norm = [](const fs::path& x) {
    std::wstring s = x.lexically_normal().native();
    constexpr wchar_t sep = L'\\';
    for (auto& c : s) c = c == L'/' ? sep : static_cast<wchar_t>(std::towlower(c));
    while (s.size() > 1 && s.back() == sep) s.pop_back();
    return s;
  };
  const std::wstring a = norm(p), b = norm(root);
  if (a == b) return true;
  return a.size() > b.size() && a.compare(0, b.size(), b) == 0 && a[b.size()] == L'\\';
}

bool spec_sync_allowed(const fs::path& spec, const fs::path& opened_for, const fs::path& current_root, bool recorded) {
  if (spec.empty() || current_root.empty()) return false;
  if (!opened_for.empty() && !in_workspace(opened_for, current_root)) return false;
  if (!opened_for.empty() && !in_workspace(current_root, opened_for)) return false;
  return recorded || in_workspace(spec, current_root);
}

std::string spec_op_path(const std::string& url) {
  std::string u = url;
  if (const size_t q = u.find_first_of("?#"); q != std::string::npos) u.resize(q);
  if (u.rfind("{{", 0) == 0) {
    if (const size_t e = u.find("}}"); e != std::string::npos) u.erase(0, e + 2);
  } else if (const size_t sch = u.find("://"); sch != std::string::npos) {
    const size_t slash = u.find('/', sch + 3);
    u.erase(0, slash == std::string::npos ? u.size() : slash);
  }
  // ":petId" and "{{petId}}" back to the document's "{petId}".
  std::string p;
  for (size_t i = 0; i < u.size(); ++i) {
    if (u[i] == ':' && (i == 0 || u[i - 1] == '/')) {
      size_t e = u.find('/', i);
      if (e == std::string::npos) e = u.size();
      p += "{" + u.substr(i + 1, e - i - 1) + "}";
      i = e - 1;
    } else if (u.compare(i, 2, "{{") == 0 && u.find("}}", i) != std::string::npos) {
      const size_t e = u.find("}}", i);
      p += "{" + u.substr(i + 2, e - i - 2) + "}";
      i = e + 1;
    } else {
      p += u[i];
    }
  }
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  return p.empty() ? "/" : p;
}


}  // namespace watt::app::sidebar
