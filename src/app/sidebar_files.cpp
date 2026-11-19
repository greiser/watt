#include "app/sidebar_files.h"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <vector>

#include <rapidjson/document.h>

#include "core/model_json.h"

namespace watt::app::sidebar {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kReqExt = ".req.json";

std::string u8(const fs::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}
fs::path from_u8(std::string_view s) { return fs::path(std::u8string(s.begin(), s.end())); }

bool is_request_file(const fs::path& p) {
  const std::string n = u8(p.filename());
  return n.size() > kReqExt.size() && n.compare(n.size() - kReqExt.size(), kReqExt.size(), kReqExt) == 0;
}

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// Same file (a case-only rename on a case-insensitive file system).
bool same_file(const fs::path& a, const fs::path& b) {
  std::error_code ec;
  return a == b || (fs::exists(a, ec) && fs::exists(b, ec) && fs::equivalent(a, b, ec));
}

}  // namespace

std::string slug(std::string_view name) {
  std::string out;
  for (unsigned char c : name) {
    if (c >= 0x80 || std::isalnum(c)) out += static_cast<char>(c >= 0x80 ? c : std::tolower(c));
    else if (!out.empty() && out.back() != '-') out += '-';
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out.empty() ? std::string("request") : out;
}

std::string file_safe(std::string_view name) {
  std::string out;
  for (unsigned char c : name)
    out += (c < 0x20 || std::string_view("<>:\"/\\|?*").find(static_cast<char>(c)) != std::string_view::npos)
               ? '_'
               : static_cast<char>(c);
  while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
  while (!out.empty() && out.front() == ' ') out.erase(out.begin());
  return out.empty() ? std::string("request") : out;
}

fs::path free_request_file(const fs::path& folder, const std::string& name, std::string* final_name) {
  std::error_code ec;
  std::string n = name;
  fs::path file = folder / from_u8(slug(n) + std::string(kReqExt));
  for (int i = 2; fs::exists(file, ec) && i < 10000; ++i) {
    n = name + " " + std::to_string(i);
    file = folder / from_u8(slug(n) + std::string(kReqExt));
  }
  if (final_name) *final_name = n;
  return file;
}

std::optional<fs::path> rename_item(Workspace& ws, const fs::path& from, const std::string& name, std::string* err) {
  std::error_code ec;
  if (name.empty()) {
    if (err) *err = "the name is empty";
    return std::nullopt;
  }
  if (!fs::exists(from, ec)) {
    if (err) *err = "it no longer exists";
    return std::nullopt;
  }
  if (fs::is_directory(from, ec)) {
    const fs::path to = from.parent_path() / from_u8(file_safe(name));
    if (to == from) return to;
    if (!same_file(to, from) && fs::exists(to, ec)) {
      if (err) *err = "\"" + u8(to.filename()) + "\" already exists";
      return std::nullopt;
    }
    fs::rename(from, to, ec);
    if (ec) {
      if (err) *err = ec.message();
      return std::nullopt;
    }
    return to;
  }
  if (!is_request_file(from)) {
    if (err) *err = "not a request file";
    return std::nullopt;
  }
  // Another request of the folder already has that file name: the name stays as typed (two
  // requests may share a name, as in Postman) and the file gets a suffix (cancel-order-2.req.json).
  // Never overwrites the other one.
  fs::path to = from.parent_path() / from_u8(slug(name) + std::string(kReqExt));
  for (int i = 2; !same_file(to, from) && fs::exists(to, ec) && i < 10000; ++i)
    to = from.parent_path() / from_u8(slug(name + " " + std::to_string(i)) + std::string(kReqExt));
  if (!same_file(to, from) && fs::exists(to, ec)) {
    if (err) *err = "\"" + u8(to.filename()) + "\" already exists";
    return std::nullopt;
  }
  auto r = ws.load_request(from, err);
  if (!r) return std::nullopt;
  r->name = name;
  r->file = from;
  if (!ws.save_request(*r, err)) return std::nullopt;
  if (to != from) {
    fs::rename(from, to, ec);
    if (ec) {
      if (err) *err = ec.message();
      return std::nullopt;
    }
  }
  return to;
}

std::optional<SavedRequest> duplicate_request(Workspace& ws, const fs::path& from, std::string* err) {
  auto r = ws.load_request(from, err);
  if (!r) return std::nullopt;
  std::string base = r->name;
  if (base.empty()) {
    base = u8(from.filename());
    if (base.size() > kReqExt.size()) base.resize(base.size() - kReqExt.size());
  }
  std::string name;
  const fs::path file = free_request_file(from.parent_path(), base + " copy", &name);
  r->id.clear();
  r->name = name;
  r->file = file;
  if (!ws.save_request(*r, err)) return std::nullopt;
  return r;
}

std::optional<SavedRequest> create_request(Workspace& ws, const fs::path& folder, RequestKind kind,
                                           const std::string& name, std::string* err) {
  SavedRequest r;
  r.kind = kind;
  if (kind != RequestKind::Grpc) r.http.method = "GET";
  // Every kind starts on Inherit, as Workspace::new_request() and Postman / Insomnia / Bruno do:
  // the folder's / collection's auth applies until the request picks its own.
  r.auth.type = HttpAuth::Type::Inherit;
  r.file = free_request_file(folder, name, &r.name);
  if (!ws.save_request(r, err)) return std::nullopt;
  return r;
}

int filter_rank(std::string_view q, std::string_view name, std::string_view folder_path, std::string_view rest) {
  return filter_rank_lower(lower(std::string(q)), lower(name), lower(folder_path), lower(rest));
}

int filter_rank_lower(std::string_view whole, std::string_view n, std::string_view f, std::string_view o) {
  int in_name = 0, prefix = 0, score = 0, words = 0;
  for (size_t p = 0; p < whole.size();) {
    size_t e = whole.find(' ', p);
    if (e == std::string_view::npos) e = whole.size();
    const std::string_view w = whole.substr(p, e - p);
    p = e + 1;
    if (w.empty()) continue;
    ++words;
    const size_t at = n.find(w);
    if (at != std::string_view::npos) {
      ++in_name;
      // At a word start ("order" in "Create order", not in "Reorder").
      if (at == 0 || !std::isalnum(static_cast<unsigned char>(n[at - 1]))) ++prefix;
      continue;
    }
    if (f.find(w) != std::string_view::npos) score += 30;
    else if (o.find(w) != std::string_view::npos) score += 20;
    else return -1;
  }
  if (words == 0) return -1;
  if (n == whole) score += 10000;
  else if (n.rfind(whole, 0) == 0) score += 5000;
  else if (n.find(whole) != std::string_view::npos) score += 3000;
  if (in_name == words) score += 1000;
  score += in_name * 100 + prefix * 40;
  // Shorter names are the tighter match.
  score -= static_cast<int>(std::min<size_t>(n.size(), 80));
  return std::max(score, 0);
}

std::vector<UnreadableEnv> unreadable_env_files(const fs::path& root) {
  std::vector<UnreadableEnv> out;
  std::error_code ec;
  const fs::path dir = root / "environments";
  if (!fs::is_directory(dir, ec)) return out;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec) || it->path().extension() != ".json") continue;
    std::string text;
    if (!detail::read_file(it->path(), text)) continue;
    rapidjson::Document d;
    d.Parse(text.data(), text.size());
    if (!d.HasParseError() && d.IsObject()) continue;
    UnreadableEnv u{it->path(), false};
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) {
      rapidjson::Document d2;
      d2.Parse(text.data() + 3, text.size() - 3);
      u.bom = !d2.HasParseError() && d2.IsObject();
    }
    out.push_back(std::move(u));
  }
  std::sort(out.begin(), out.end(), [](const UnreadableEnv& a, const UnreadableEnv& b) { return a.file < b.file; });
  return out;
}

bool remove_utf8_bom(const fs::path& file, std::string* err) {
  std::string text;
  if (!detail::read_file(file, text)) {
    if (err) *err = "cannot read " + u8(file.filename());
    return false;
  }
  if (text.size() < 3 || text.compare(0, 3, "\xEF\xBB\xBF") != 0) return true;
  return detail::write_file_atomic(file, text.substr(3), err);
}

}  // namespace watt::app::sidebar
