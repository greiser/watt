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
};

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
void keep_extras(const std::filesystem::path& root, const std::string& key, RunExtras x);
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
