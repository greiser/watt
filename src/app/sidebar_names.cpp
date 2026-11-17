#include "app/sidebar_names.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace watt::app::sidebar {

namespace fs = std::filesystem;

namespace {

constexpr size_t kHead = 2048;

std::string to_u8(const fs::path& p) {
  const auto s = p.generic_u8string();
  return std::string(s.begin(), s.end());
}
fs::path from_u8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

void put_utf8(std::string& out, unsigned cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

bool hex4(const std::string& s, size_t p, unsigned* out) {
  if (p + 4 > s.size()) return false;
  unsigned v = 0;
  for (size_t i = p; i < p + 4; ++i) {
    const char c = s[i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
    else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
    else return false;
  }
  *out = v;
  return true;
}

// The JSON string value of the first "key" at or after `from`. False when it is not complete
// within the head.
bool string_value(const std::string& s, const char* key, size_t from, std::string* out, size_t* end = nullptr) {
  const std::string k = std::string("\"") + key + "\"";
  size_t p = s.find(k, from);
  if (p == std::string::npos) return false;
  p += k.size();
  while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p;
  if (p >= s.size() || s[p] != ':') return false;
  ++p;
  while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p;
  if (p >= s.size() || s[p] != '"') return false;
  ++p;
  std::string v;
  while (p < s.size()) {
    const char c = s[p++];
    if (c == '"') {
      *out = std::move(v);
      if (end) *end = p;
      return true;
    }
    if (c != '\\') {
      v += c;
      continue;
    }
    if (p >= s.size()) return false;
    const char e = s[p++];
    switch (e) {
      case 'n': v += '\n'; break;
      case 't': v += '\t'; break;
      case 'r': v += '\r'; break;
      case 'b': v += '\b'; break;
      case 'f': v += '\f'; break;
      case 'u': {
        if (p + 4 > s.size()) return false;
        unsigned cp = 0;
        if (!hex4(s, p, &cp)) return false;
        p += 4;
        if (cp >= 0xD800 && cp < 0xDC00 && p + 6 <= s.size() && s[p] == '\\' && s[p + 1] == 'u') {
          unsigned lo = 0;
          if (hex4(s, p + 2, &lo) && lo >= 0xDC00 && lo < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            p += 6;
          }
        }
        put_utf8(v, cp);
        break;
      }
      default: v += e;
    }
  }
  return false;
}

std::string escape(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) {
    if (c == '\\') o += "\\\\";
    else if (c == '\t') o += "\\t";
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else o += c;
  }
  return o;
}

std::string unescape(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\\' || i + 1 >= s.size()) {
      o += s[i];
      continue;
    }
    const char e = s[++i];
    o += e == 't' ? '\t' : e == 'n' ? '\n' : e == 'r' ? '\r' : e;
  }
  return o;
}

template <typename F>
void each_request(CollectionNode& n, F&& f) {
  for (auto& c : n.children) {
    if (c.is_folder) each_request(c, f);
    else f(c);
  }
}

}  // namespace

bool read_request_meta(const fs::path& file, ReqMeta* out) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return false;
  std::string head(kHead, '\0');
  in.read(head.data(), static_cast<std::streamsize>(head.size()));
  head.resize(static_cast<size_t>(in.gcount()));
  ReqMeta m;
  string_value(head, "id", 0, &m.id);
  size_t after_name = 0;
  if (!string_value(head, "name", 0, &m.name, &after_name)) after_name = 0;
  if (const size_t h = head.find("\"http\"", after_name); h != std::string::npos) string_value(head, "url", h, &m.url);
  else if (const size_t g = head.find("\"grpc\"", after_name); g != std::string::npos)
    string_value(head, "method", g, &m.url);
  *out = std::move(m);
  return true;
}

NameCache::NameCache(fs::path root) : root_(std::move(root)) {}

void NameCache::load_locked() {
  if (loaded_) return;
  loaded_ = true;
  std::ifstream in(root_ / ".watt" / "sidebar-names.tsv", std::ios::binary);
  if (!in) return;
  std::string line;
  while (std::getline(in, line)) {
    std::vector<std::string> f;
    size_t p = 0;
    for (;;) {
      const size_t t = line.find('\t', p);
      f.push_back(line.substr(p, t == std::string::npos ? std::string::npos : t - p));
      if (t == std::string::npos) break;
      p = t + 1;
    }
    if (f.size() != 5 && f.size() != 6) continue;
    Entry e;
    e.mtime = std::strtoll(f[1].c_str(), nullptr, 10);
    e.size = std::strtoull(f[2].c_str(), nullptr, 10);
    e.meta.name = unescape(f[3]);
    e.meta.url = unescape(f[4]);
    if (f.size() == 6) e.meta.id = unescape(f[5]);
    else e.mtime = -1;  // written before ids were cached: read the head again
    entries_[(root_ / from_u8(unescape(f[0]))).lexically_normal()] = std::move(e);
  }
}

void NameCache::save_locked() {
  std::error_code ec;
  const fs::path dir = root_ / ".watt";
  if (!fs::is_directory(dir, ec)) return;  // never create it just for this
  std::ostringstream o;
  for (const auto& [p, e] : entries_)
    o << escape(to_u8(p.lexically_relative(root_))) << '\t' << e.mtime << '\t' << e.size << '\t'
      << escape(e.meta.name) << '\t' << escape(e.meta.url) << '\n';
  const fs::path tmp = dir / "sidebar-names.tsv.tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return;
    const std::string s = o.str();
    out.write(s.data(), static_cast<std::streamsize>(s.size()));
    if (!out) return;
  }
  fs::rename(tmp, dir / "sidebar-names.tsv", ec);
}

MetaMap NameCache::apply_cached(CollectionNode& tree) {
  std::lock_guard<std::mutex> lock(mu_);
  load_locked();
  MetaMap out;
  each_request(tree, [&](CollectionNode& c) {
    auto it = entries_.find(c.path.lexically_normal());
    if (it == entries_.end()) return;
    if (!it->second.meta.name.empty()) c.name = it->second.meta.name;
    out.emplace(c.path, it->second.meta);
  });
  return out;
}

MetaMap NameCache::refresh(CollectionNode& tree, bool* changed) {
  *changed = false;
  // Stat and read outside the lock: a save on the UI thread (note()) must not wait.
  struct Item {
    CollectionNode* node;
    fs::path key;
    int64_t mtime;
    uint64_t size;
  };
  std::vector<Item> items;
  each_request(tree, [&](CollectionNode& c) {
    std::error_code ec;
    const auto t = fs::last_write_time(c.path, ec);
    const int64_t mtime = ec ? 0 : static_cast<int64_t>(t.time_since_epoch().count());
    const uint64_t size = fs::file_size(c.path, ec);
    items.push_back(Item{&c, c.path.lexically_normal(), mtime, ec ? 0 : size});
  });
  std::map<fs::path, Entry> snapshot;
  {
    std::lock_guard<std::mutex> lock(mu_);
    load_locked();
    snapshot = entries_;
  }
  MetaMap out;
  std::map<fs::path, Entry> next;
  bool dirty = snapshot.size() != items.size();
  for (const Item& it : items) {
    auto s = snapshot.find(it.key);
    Entry e;
    if (s != snapshot.end() && s->second.mtime == it.mtime && s->second.size == it.size) {
      e = s->second;
    } else {
      e.mtime = it.mtime;
      e.size = it.size;
      read_request_meta(it.node->path, &e.meta);
      dirty = true;
      if (s == snapshot.end() || s->second.meta.name != e.meta.name || s->second.meta.url != e.meta.url ||
          s->second.meta.id != e.meta.id)
        *changed = true;
    }
    if (!e.meta.name.empty()) it.node->name = e.meta.name;
    out.emplace(it.node->path, e.meta);
    next.emplace(it.key, std::move(e));
  }
  if (dirty) {
    std::lock_guard<std::mutex> lock(mu_);
    // Keep what note() recorded meanwhile when it is newer than the file state read here.
    for (auto& [k, e] : entries_)
      if (auto n = next.find(k); n != next.end() && e.mtime > n->second.mtime) n->second = e;
    entries_ = std::move(next);
    save_locked();
  }
  return out;
}

void NameCache::note(const fs::path& file, const std::string& name, const std::string& url, const std::string& id) {
  std::error_code ec;
  const auto t = fs::last_write_time(file, ec);
  Entry e;
  e.mtime = ec ? 0 : static_cast<int64_t>(t.time_since_epoch().count());
  e.size = fs::file_size(file, ec);
  if (ec) e.size = 0;
  e.meta.name = name;
  e.meta.url = url;
  e.meta.id = id;
  std::lock_guard<std::mutex> lock(mu_);
  load_locked();
  entries_[file.lexically_normal()] = std::move(e);
}

}  // namespace watt::app::sidebar
