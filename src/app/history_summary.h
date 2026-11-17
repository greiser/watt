#pragma once
// What a history entry records about where a request went. HistoryEntry::summary keeps its old
// form ("GET {{base}}/x", "/pkg.Svc/M @ {{grpc}}"), the request as written, and since round 4
// also carries what was actually sent, after a unit separator (U+001F):
//
//   "GET {{base}}/x" US "http://127.0.0.1:18971/x" US "local"
//   "/pkg.Svc/M @ {{grpc}}" US "127.0.0.1:51971" US "local"
//
// i.e. template, the expanded URL / gRPC address at send time, and the environment that was
// active. Rows, tooltips, the history filter and grouping read the sent part, never a
// re-expansion with whatever environment is active now. Entries written before this have no
// separator: history_sent() is empty and they show their raw {{template}}.
#include <string>
#include <string_view>

#include "core/model.h"

namespace watt::app {

inline constexpr char kHistorySep = '\x1f';

// The request as written: "GET {{base}}/x" / "/pkg.Svc/M @ {{grpc}}".
inline std::string_view history_template(const std::string& summary) {
  return std::string_view(summary).substr(0, summary.find(kHistorySep));
}
inline std::string_view history_template(const HistoryEntry& e) { return history_template(e.summary); }

// The expanded URL (HTTP, with scheme) or gRPC address it was sent to; empty for old entries.
inline std::string_view history_sent(const HistoryEntry& e) {
  const size_t a = e.summary.find(kHistorySep);
  if (a == std::string::npos) return {};
  const std::string_view rest = std::string_view(e.summary).substr(a + 1);
  return rest.substr(0, rest.find(kHistorySep));
}

// The environment active when it was sent ("" = none, or an old entry).
inline std::string_view history_env(const HistoryEntry& e) {
  const size_t a = e.summary.find(kHistorySep);
  if (a == std::string::npos) return {};
  const size_t b = e.summary.find(kHistorySep, a + 1);
  if (b == std::string::npos) return {};
  return std::string_view(e.summary).substr(b + 1);
}

// Separators inside the parts (never in a URL that was sent, but a template is user text).
inline std::string history_part(std::string_view s) {
  std::string out(s);
  for (char& c : out)
    if (c == kHistorySep) c = ' ';
  return out;
}

inline std::string make_history_summary(std::string_view tmpl, std::string_view sent, std::string_view env) {
  return history_part(tmpl) + kHistorySep + history_part(sent) + kHistorySep + history_part(env);
}

}  // namespace watt::app
