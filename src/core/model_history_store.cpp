#include "core/model_history_store.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string_view>
#include <system_error>

#include "core/model_json.h"

namespace watt {

namespace fs = std::filesystem;

namespace {

fs::path responses_dir(const fs::path& root) { return root / ".watt" / "responses"; }
fs::path hidden_file(const fs::path& root) { return root / ".watt" / "history-hidden.json"; }

// One read-modify-write of history-hidden.json at a time in this process.
std::mutex& hidden_mutex() {
  static std::mutex m;
  return m;
}

void fnv1a(uint64_t& h, std::string_view s) {
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
}

void write_hidden(const fs::path& root, const HistoryHidden& h, bool* ok) {
  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> w(sb);
  w.StartObject();
  w.Key("version");
  w.Int(1);
  w.Key("keys");
  w.StartArray();
  for (const auto& k : h.keys) w.String(k.data(), static_cast<rapidjson::SizeType>(k.size()));
  w.EndArray();
  w.Key("cleared_before");
  w.String(h.cleared_before.data(), static_cast<rapidjson::SizeType>(h.cleared_before.size()));
  w.Key("cleared_at");
  w.StartArray();
  for (const auto& k : h.cleared_at) w.String(k.data(), static_cast<rapidjson::SizeType>(k.size()));
  w.EndArray();
  w.EndObject();
  detail::ensure_dir(root / ".watt");
  *ok = detail::write_file_atomic(hidden_file(root), std::string(sb.GetString(), sb.GetSize()), nullptr, true);
}

HistoryHidden read_hidden(const fs::path& root) {
  HistoryHidden h;
  std::string text;
  if (!detail::read_file(hidden_file(root), text)) return h;
  rapidjson::Document d;
  d.Parse(text.data(), text.size());
  if (d.HasParseError() || !d.IsObject()) return h;
  auto strings = [&](const char* k, std::set<std::string>& out) {
    if (!d.HasMember(k) || !d[k].IsArray()) return;
    for (const auto& v : d[k].GetArray())
      if (v.IsString()) out.emplace(v.GetString(), v.GetStringLength());
  };
  strings("keys", h.keys);
  strings("cleared_at", h.cleared_at);
  if (d.HasMember("cleared_before") && d["cleared_before"].IsString())
    h.cleared_before.assign(d["cleared_before"].GetString(), d["cleared_before"].GetStringLength());
  return h;
}

}  // namespace

std::string history_entry_key(const HistoryEntry& e) {
  // Whole microseconds: the history JSON reader may round a double by one ulp.
  const long long us = std::llround(e.total_ms * 1000.0);
  char nums[48];
  const int n = std::snprintf(nums, sizeof nums, "\n%ld\n%lld", e.status, us);
  uint64_t h = 1469598103934665603ull;
  fnv1a(h, e.when_iso);
  fnv1a(h, "\n");
  fnv1a(h, e.summary);
  fnv1a(h, std::string_view(nums, n > 0 ? static_cast<size_t>(n) : 0));
  static const char hex[] = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i, h >>= 4) out[static_cast<size_t>(i)] = hex[h & 15];
  return out;
}

fs::path history_response_file(const fs::path& root, const HistoryEntry& e) {
  return responses_dir(root) / (history_entry_key(e) + ".json");
}

bool save_history_response(const fs::path& root, const HistoryEntry& e, const std::string& json) {
  if (json.empty()) return false;
  detail::ensure_dir(responses_dir(root));
  return detail::write_file_atomic(history_response_file(root, e), json, nullptr, true);
}

std::optional<std::string> load_history_response(const fs::path& root, const HistoryEntry& e) {
  std::string out;
  if (!detail::read_file(history_response_file(root, e), out) || out.empty()) return std::nullopt;
  return out;
}

void prune_history_responses(const fs::path& root, size_t max_files, uint64_t max_bytes) {
  struct F {
    fs::path p;
    fs::file_time_type t;
    uint64_t size;
  };
  std::vector<F> files;
  std::error_code ec;
  for (fs::directory_iterator it(responses_dir(root), ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (!it->is_regular_file(e2)) continue;
    const auto t = it->last_write_time(e2);
    const uint64_t sz = it->file_size(e2);
    files.push_back({it->path(), t, sz});
  }
  if (files.size() <= max_files) {
    uint64_t total = 0;
    for (const auto& f : files) total += f.size;
    if (total <= max_bytes) return;
  }
  std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return a.t > b.t; });
  uint64_t bytes = 0;
  for (size_t i = 0; i < files.size(); ++i) {
    bytes += files[i].size;
    if (i >= max_files || (i > 0 && bytes > max_bytes)) {
      std::error_code e2;
      fs::remove(files[i].p, e2);
    }
  }
}

bool HistoryHidden::hides(const HistoryEntry& e) const {
  if (empty()) return false;
  if (!cleared_before.empty() && e.when_iso < cleared_before) return true;  // no key needed
  if (keys.empty() && e.when_iso != cleared_before) return false;
  return hides(e, history_entry_key(e));
}

bool HistoryHidden::hides(const HistoryEntry& e, const std::string& key) const {
  if (!cleared_before.empty()) {
    if (e.when_iso < cleared_before) return true;
    if (e.when_iso == cleared_before && cleared_at.count(key)) return true;
  }
  return keys.count(key) != 0;
}

HistoryHidden load_history_hidden(const fs::path& root) {
  std::lock_guard lk(hidden_mutex());
  return read_hidden(root);
}

bool hide_history_entries(const fs::path& root, const std::vector<HistoryEntry>& entries) {
  if (entries.empty()) return true;
  std::lock_guard lk(hidden_mutex());
  HistoryHidden h = read_hidden(root);
  for (const auto& e : entries) {
    const std::string k = history_entry_key(e);
    h.keys.insert(k);
    std::error_code ec;
    fs::remove(responses_dir(root) / (k + ".json"), ec);
  }
  bool ok = false;
  write_hidden(root, h, &ok);
  return ok;
}

bool clear_history(const fs::path& root, const std::vector<HistoryEntry>& newest_first) {
  std::lock_guard lk(hidden_mutex());
  HistoryHidden h = read_hidden(root);
  if (!newest_first.empty()) {
    const std::string newest = newest_first.front().when_iso;
    if (newest > h.cleared_before) {
      h.cleared_before = newest;
      h.cleared_at.clear();
    }
    for (const auto& e : newest_first)
      if (e.when_iso == h.cleared_before) h.cleared_at.insert(history_entry_key(e));
    // Single deletions of listed entries are covered by the cut now (keys of entries this
    // list does not have, e.g. appended by another window meanwhile, stay).
    for (const auto& e : newest_first) h.keys.erase(history_entry_key(e));
  }
  std::error_code ec;
  fs::remove_all(responses_dir(root), ec);
  bool ok = false;
  write_hidden(root, h, &ok);
  return ok;
}

void filter_hidden_history(const HistoryHidden& h, std::vector<HistoryEntry>& entries) {
  if (h.empty()) return;
  entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const HistoryEntry& e) { return h.hides(e); }),
                entries.end());
}

}  // namespace watt
