#pragma once
// What the sidebar keeps next to the workspace history (all under <ws>/.watt, owner-only):
//
//   .watt/responses/<key>.json   the response a history entry got, as an opaque JSON text the
//                                  app writes (request_tab's last_response_to_json), body capped
//                                  by the caller (kHistoryResponseCap). One file per entry, so
//                                  listing history never reads them; an entry reads its own
//                                  when it is opened. Pruned to the newest files.
//   .watt/history-hidden.json    entries the user deleted, and where "Clear history" cut the
//                                  list: the history segments are append-only (and shared with
//                                  other processes), so removal is a filter applied when listing.
//
// <key> is history_entry_key(): a hash of the list fields (when, summary, status, total_ms), the
// same for an entry built when the request finished and for the same entry read back by
// Workspace::history() (total_ms round-trips exactly through the history JSON).
// No wx: unit tested in tests/test_sidebar.cpp. Any thread.
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/model.h"

namespace watt {

// Body bytes kept with a history entry (Postman keeps a capped response with each entry too).
// Reopening an entry (or a request whose last run it is) puts the kept body into the response
// view in one UI step; measured with a 10 MB JSON run: 16 KB ~22 ms (the row alone), 64 KB
// ~25-35 ms, 128 KB ~45 ms, 256 KB 55-130 ms, 512 KB ~80 ms (worker-formatted, swapped in at
// once). 64 KB stays inside the 50 ms budget; the status line says how much was kept.
inline constexpr size_t kHistoryResponseCap = 64u << 10;

// 16 lower-case hex digits.
std::string history_entry_key(const HistoryEntry& e);

std::filesystem::path history_response_file(const std::filesystem::path& root, const HistoryEntry& e);
// Writes the response of an entry (atomically). False if it could not be written.
bool save_history_response(const std::filesystem::path& root, const HistoryEntry& e, const std::string& json);
// nullopt when the entry has no stored response.
std::optional<std::string> load_history_response(const std::filesystem::path& root, const HistoryEntry& e);
// Keeps the newest `max_files` response files whose sizes add up to at most `max_bytes`.
void prune_history_responses(const std::filesystem::path& root, size_t max_files = 2000,
                             uint64_t max_bytes = 96ull << 20);

// Entries removed from the list.
struct HistoryHidden {
  std::set<std::string> keys;           // deleted one by one
  std::string cleared_before;           // "Clear": every entry older than this ISO time ...
  std::set<std::string> cleared_at;     // ... and these keys of entries at exactly that time
  bool hides(const HistoryEntry& e) const;
  bool hides(const HistoryEntry& e, const std::string& key) const;
  bool empty() const { return keys.empty() && cleared_before.empty(); }
};
HistoryHidden load_history_hidden(const std::filesystem::path& root);
// Hides these entries (and removes their stored responses). False if the list could not be
// written.
bool hide_history_entries(const std::filesystem::path& root, const std::vector<HistoryEntry>& entries);
// Hides every entry up to and including `newest` (the first entry of the list) and removes
// every stored response.
bool clear_history(const std::filesystem::path& root, const std::vector<HistoryEntry>& newest_first);
// Drops hidden entries from a list (newest first), in place.
void filter_hidden_history(const HistoryHidden& h, std::vector<HistoryEntry>& entries);

// .watt/snapshots/origins.json: which saved request (or folder, for a request sent unsaved) a
// history snapshot tab was opened from, so a snapshot tab restored after a restart keeps
// inheriting that folder's settings. Keys are snapshot file names ("<hex>.req.json"), values
// workspace-relative generic paths ("api/users/Get user.req.json"). Loading drops anything that
// is not a plain relative path inside the workspace (absolute, "..", or under .watt), and keeps
// at most kSnapshotOriginsMax entries.
inline constexpr size_t kSnapshotOriginsMax = 4000;
std::map<std::string, std::string> load_snapshot_origins(const std::filesystem::path& root);
bool save_snapshot_origins(const std::filesystem::path& root, const std::map<std::string, std::string>& origins);
// True for a value load_snapshot_origins() keeps.
bool safe_relative_origin(std::string_view rel);

}  // namespace watt
