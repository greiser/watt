#pragma once
// What the response view keeps of a live HTTP run beyond what the history keeps with an entry
// (request_tab's last_response_to_json: status, headers, body, timing as the shell measured it).
// Restoring a response from the history or an earlier session needs the rest to show it as it
// was shown live:
//   - the time a confirmation dialog was open (script trust, sign-in, env vars): it is in the
//     shell's Queue / total and must be taken out again, or a 9 ms run comes back as 25 s;
//   - the pre-request and post-response script results (tests, console, variables, time);
//   - the connection and TLS details (version, cipher, certificate), the redirect hops;
//   - everything but the body for a response too large to keep (10 MB): "body not kept".
//
// Stored per run, keyed by the request (kind, method, URL as saved) + status + the shell's exact
// total: the same values a history entry and a kept last response carry. One small JSON file per
// run under <ws>/.watt/response-runs/ (owner-only like everything under .watt), plus an
// in-memory copy for this process. No wx: unit tested in tests/test_response_asserts.cpp.
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/http_client.h"
#include "core/model.h"
#include "core/script.h"

namespace watt::app::rv {

struct RunExtras {
  double prompt_ms = 0;  // dialog time inside the shell's queue / total (not the request's)
  bool scripts_skipped_untrusted = false;
  std::shared_ptr<const ScriptRun> pre, post;
  // The response as received, body left out (body_size says how large it was).
  std::shared_ptr<const HttpResponse> head;
  uint64_t body_size = 0;
  bool visualize = false;  // the body was shown as Visualize (pm.visualizer): reopened that way
  // A text/event-stream body: when each event arrived (ms after the send, in stream order), so a
  // restored stream lists its events with their offsets as it did live. At most kKeptSseTimes.
  std::vector<double> sse_at;
  std::string key;  // the run key it was found under (set by find_extras*, not stored)
};

// The whole body of a run, kept next to its extras (<ws>/.watt/response-bodies/<key>.body) when
// it is at most kKeptBodyMax: the history keeps only its first kHistoryResponseCap, and a reopened
// request's Find / Filter / Table / Copy / Save must work on the body as received. The newest
// kKeptBodyFiles files, at most kKeptBodyBytes together, are kept.
inline constexpr size_t kKeptSseTimes = 20000;
inline constexpr uint64_t kKeptBodyMax = 16u << 20;
inline constexpr size_t kKeptBodyFiles = 64;
inline constexpr uint64_t kKeptBodyBytes = 256ull << 20;

// "GET https://x/{{id}}" (the request as saved, before {{var}} expansion), the status and the
// shell's total (whole microseconds).
std::string run_key(const std::string& summary, long status, double raw_total_ms);

// r without its body (cheap for a 10 MB response: the body is never copied).
std::shared_ptr<const HttpResponse> response_head(const HttpResponse& r);

std::string extras_to_json(const RunExtras& x);
std::optional<RunExtras> extras_from_json(const std::string& json);

// The response `kept` (from the history / session: body maybe capped or missing) with what was
// stored for it: connection, TLS, hops from x.head, the dialog time taken out of the timing.
// kept == nullptr: rebuilt from x.head alone (the body was not kept).
std::shared_ptr<HttpResponse> restore_response(const HttpResponse* kept, const RunExtras& x);

// UI thread or worker: remembered for this process and written to disk on a worker thread.
// full: the response (its body is written too when it is not over kKeptBodyMax; never copied).
void keep_extras(const std::filesystem::path& root, const std::string& key, RunExtras x,
                 std::shared_ptr<const HttpResponse> full = nullptr);
// Worker thread: the body kept by keep_extras for this run, when it is there and exactly
// expect_size bytes (null otherwise).
std::shared_ptr<std::string> load_kept_body(const std::filesystem::path& root, const std::string& key,
                                            uint64_t expect_size);
// Any thread (reads the disk when not in memory: call it off the UI thread).
std::optional<RunExtras> find_extras(const std::filesystem::path& root, const std::string& key);
// Memory only (UI thread safe).
std::optional<RunExtras> find_extras_cached(const std::string& key);

// Keeps in h (newest first, as Workspace::history() lists it) only the runs of the saved request
// q: the same kind and summary (method + URL as saved) and the same request id, read from each
// candidate's stored request. 19 Json never shows 02 Items' run of the same URL, nor a saved
// request a run sent from an Untitled tab. A request without an id (an older file) matches runs
// stored under its name and without an id. Looks at the newest 48 candidates. Worker thread
// (reads the history's request records).
void keep_runs_of(const Workspace& ws, const SavedRequest& q, const std::string& summary,
                  std::vector<HistoryEntry>& h);

}  // namespace watt::app::rv
