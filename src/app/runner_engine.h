#pragma once
// Collection runner engine (Postman's Collection Runner, Bruno's Runner), wx-free so it is
// tested directly (tests/test_runner.cpp). Runs a list of saved requests in order, N
// iterations, with an optional delay between requests, against a private copy of the
// workspace: each request's asserts are evaluated and its captures applied to that copy, so a
// later request sends the values an earlier one captured ({{token}} from Workspace::runtime_vars).
//
// Threading: run_plan() blocks; call it on a worker thread, never on the UI thread. It owns one
// HttpClient for the whole run (connections, DNS and TLS sessions are reused across requests,
// busy_poll_ms is forced to 0 so a long run sleeps through server waits instead of spinning a
// core). RunControl::stop() may be called from any thread: it cancels the request in flight
// and wakes a delay at once.
//
// Consent: nothing here ever asks. consent_needed() lists what the runner would read from
// outside the workspace (upload files, gRPC schema files, OS environment variables) so the UI
// can ask once up front; the caller approves with Workspace::allow_file / allow_env_var. During
// the run, a request that still needs anything not allowed (e.g. a captured value pointed a
// path outside the workspace) is not sent and its result carries the reason.
//
// Scripts (core/script.h) run like the request tab runs them, on the worker: the Scripts tab's
// pre-request variables (http_editor_pre.h), then the pre-request script, before http_to_send;
// the post-response script after the captures and before the asserts. Their variable writes go
// to the run's private copy, their test() / pm.test results are checks of the step (Check::
// script), a thrown error fails the step, console output is kept in StepResult::logs and each
// script's time is its own phase. Scripts run only when RunOptions::run_scripts (the caller
// checked Workspace::scripts_trusted() once, up front); otherwise a request with scripts is sent
// without them and gets a failed check "scripts not run (workspace not trusted)".
//
// Pass / fail of one step: failed when it could not be sent, the transport failed, an assertion
// or script test failed, or a script threw. A request without checks fails on an HTTP status >=
// 400 or a gRPC status other than OK (a run of requests without tests still shows what broke).
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/asserts.h"
#include "core/grpc_client.h"
#include "core/http_client.h"
#include "core/model.h"
#include "core/script.h"

namespace watt::app::runner {

struct RunItem {
  SavedRequest req;
  std::string rel;  // path below the run's folder, '/'-separated, without .req.json ("auth/login")
};

// Tags of a request, lower case, unique: every "#word" in its docs (Bruno's tags) and every
// folder name of its path below the run's folder ("auth/login" -> "auth").
std::vector<std::string> tags_of(const RunItem& it);
// Tag filter: "smoke -slow" = has tag smoke and not slow. A word (optionally prefixed '+' or
// '#') must match; a '-' word excludes. A word matches a whole tag (case-insensitive) or is a
// substring of the request's name / path (so "login" finds "auth/login").
bool tag_filter_match(const RunItem& it, const std::string& filter);

// The order the user dragged a folder's items into in the sidebar: folder -> file names (the
// rest follow in natural name order).
using FolderOrder = std::map<std::filesystem::path, std::vector<std::wstring>>;

// The run order saved by the runner (its arrows / drag), shared in git so `watt --run` in CI
// runs what the runner shows: <ws>/run-order.json
//   { "version": 1, "order": { ".": ["List items.req.json", "orders"], "orders": [...] } }
// Per folder (relative to the workspace root, "/"-separated, "." = the root) the file names of
// its requests and subfolders, interleaved: a root request listed before a subfolder runs before
// it. Items a list does not name follow it in the default order. Keys are only matched against
// the workspace tree (never opened); a link or a file over 1 MB is ignored.
inline constexpr const char* kRunOrderFile = "run-order.json";
// Absolute folder -> names, as read from <root>/run-order.json (empty when there is none).
FolderOrder load_run_order(const std::filesystem::path& root, std::string* err = nullptr);
// Sets the lists of the folders in `changed` (absolute folder -> names; an empty list removes
// the folder's entry) and writes the file atomically; other folders' entries are kept. Disk:
// worker thread only. Calls are serialized; a call whose seq is lower than one already written
// is dropped (a slow older write never undoes a newer order).
bool save_run_order(const std::filesystem::path& root, const FolderOrder& changed, uint64_t seq = 0,
                    std::string* err = nullptr);
// The text of run-order.json for (folder key, names) entries: one name per line, commas at line
// ends (clean git diffs).
std::string run_order_json(const std::vector<std::pair<std::string, std::vector<std::string>>>& entries);

// Every request under folder (the workspace root when folder is empty or the root), depth-first
// in the saved run order (run-order.json, folders and requests interleaved), else the sidebar's
// order (`order`: folders first, then the dragged order), else natural name order; a .req.json
// file runs alone. Reads the disk: worker thread only. Files that cannot be read are listed in
// *errors ("rel: why").
std::vector<RunItem> collect(const Workspace& ws, const std::filesystem::path& folder,
                             std::vector<std::string>* errors = nullptr, const FolderOrder* order = nullptr);

// Reordering the runner's list as a tree: requests stay in their folders, so a step moves a
// whole block (a request, or a subfolder with all its requests). `order` is item indexes in run
// order, each folder's items contiguous (as collect() returns them). base: the run's folder.
// Moves the request at order position pos one sibling up (d < 0) or down (d > 0) in its folder;
// at the edge of its folder the folder itself moves (and so on up). True when something moved;
// *changed gets the moved folder's new child list (absolute folder -> file names) to save.
bool move_block(const std::vector<RunItem>& items, std::vector<size_t>& order, const std::filesystem::path& base,
                size_t pos, int d, FolderOrder* changed);
// Drag and drop: moves the request at pos (block by block, as move_block) until it stands at or
// just past position `before` (order.size(): the end). True when something moved.
bool drop_block(const std::vector<RunItem>& items, std::vector<size_t>& order, const std::filesystem::path& base,
                size_t pos, size_t before, FolderOrder* changed);

// "a/b.req.json" -> "a/b".
std::string strip_ext(std::string s);

// A request of the run that uses {{var}} before the request that captures var has run: the
// first iteration sends it with the variable's old value (or unresolved). order positions.
struct OrderHint {
  size_t user = 0;     // position (in order) of the request that uses {{var}}
  size_t capture = 0;  // position of the first later request that captures it
  std::string var;
};
// Every such pair in the run's order (a {{var}} captured by an earlier request, or by the same
// request, is fine), at most max. Scans URL, headers, path params, body (first 256 KB), gRPC
// address, body and metadata. Cheap enough for the UI thread for typical folders.
std::vector<OrderHint> capture_order_hints(const std::vector<RunItem>& items, const std::vector<size_t>& order,
                                           size_t max = 8);
// positions (indexes into order) plus, transitively, every earlier position whose capture sets a
// {{var}} one of them reads (the last capture of it before the reader), sorted: what "Rerun
// failed" sends so a failed request gets fresh values instead of ones left from the last run.
std::vector<size_t> with_capture_sources(const std::vector<RunItem>& items, const std::vector<size_t>& order,
                                         const std::vector<size_t>& positions);

// The URL with its "/:name" path variables replaced by the request's path_params values
// ({{vars}} in a value expanded first, then path-encoded), as the HTTP editor sends it.
std::string url_with_path_params(const SavedRequest& r, const Workspace& ws);
// r made ready for http_to_send() the way the HTTP editor sends it: the URL normalize_url()ed and
// each {{var}} value encoded for where it stands (expand_url: a query value is query-encoded, so
// "c d" is "c%20d" and "a&b=c" stays one value), and an application/x-www-form-urlencoded body's
// values form-encoded (expand_form). Values from data rows, captures and scripts alike.
void encode_as_sent(SavedRequest& r, const Workspace& ws);

struct Consent {
  std::vector<std::filesystem::path> files;  // uploads and schema files outside the workspace
  std::vector<std::string> env_vars;         // OS environment variables not yet allowed
  // Proxy / CA / client certificate from workspace files not yet approved (Workspace::
  // blocked_network_settings, unique by key); approve with Workspace::allow_network_item.
  std::vector<Workspace::NetworkItem> network;
  size_t script_requests = 0;                // requests of the run that have a pre / post script
  bool scripts_trusted = false;              // Workspace::scripts_trusted() when it was checked
  // Files, environment variables and network items only (scripts are asked separately).
  bool empty() const { return files.empty() && env_vars.empty() && network.empty(); }
  bool scripts_need_trust() const { return script_requests > 0 && !scripts_trusted; }
};
// Whether the request has a pre-request or post-response script.
bool has_scripts(const SavedRequest& r);
// What the given items would read that is not allowed yet (unique, in first-use order), and
// whether they have scripts this workspace does not trust yet. Reads the per-user allowlists and
// the trust file: worker thread only.
Consent consent_needed(const Workspace& ws, const std::vector<RunItem>& items, const std::vector<size_t>& order);

struct Phase {
  const char* name;  // static string: "DNS", "Connect", ...
  double ms;
};
// Pre-request vars / script, Auth / token, post-response script: time outside the response time.
bool is_script_phase(const Phase& p);
bool is_setup_phase(const Phase& p);  // Prepare, Queue: StepResult::prep_ms, not response time

// One data file row: (column, value) in the file's column order.
using DataRow = std::vector<std::pair<std::string, std::string>>;

struct StepResult {
  size_t seq = 0;       // position in the run, from 0
  int iteration = 0;    // from 0
  size_t item = 0;      // index into the run's items
  size_t pos = 0;       // index into the run's order (the request's place in one iteration)
  RequestKind kind = RequestKind::Http;
  std::string name;
  std::string rel;
  std::string method;   // HTTP method, or the gRPC method path
  std::string url;      // as sent: expanded URL / gRPC address
  bool sent = false;    // reached the engine
  // Failed on the client before anything usable went out: the request message did not encode,
  // the schema / reflection had no such method. Counted as an error (sent stays false).
  bool client_error = false;
  bool transport_ok = false;  // HTTP: a response arrived; gRPC: status OK
  long status = 0;            // HTTP status / gRPC status code
  std::string status_text;    // reason phrase / gRPC status name
  std::string error;          // why it was not sent, or the transport / gRPC error message
  // timing.total_ms is the response time as Newman / Postman report it: the network phases, DNS
  // through Process. Neither the scripts (script_ms) nor the setup before the first I/O (prep_ms)
  // are in it.
  Timing timing;
  // The runner's own setup (expansion, files, consent checks), watt's request setup and the
  // client queue before the first I/O: the Prepare and Queue phases (is_setup_phase).
  double prep_ms = 0;
  // Pre-request vars, pre-request script, Auth / token and post-response script, apart from
  // timing.total_ms (the Script column, the "incl. scripts" figures).
  double script_ms = 0;
  // In order; the network phases add up to timing.total_ms, the script ones (is_script_phase)
  // to script_ms, the setup ones (is_setup_phase) to prep_ms.
  std::vector<Phase> phases;
  double total_incl_scripts_ms() const { return timing.total_ms + script_ms; }
  uint64_t size = 0;          // response body bytes
  double started_ms = 0;      // offset of the send from the run's start
  struct Check {
    bool pass = false;
    bool script = false;  // a script's test() / pm.test (target = its name), or a script error
    std::string target, op, expected, actual, message;
    std::string from;     // a folder's / the collection's check: its label ("api", "Collection"); "": the request's
    // "status eq 200", or a script test's name.
    std::string label() const;
    // Why it failed: the message, else "got <actual>" ("" when it passed).
    std::string why() const;
    // One line for a failure list: "expected res.status to equal 200, got 404" (an assert's
    // message names its target), "status is 200: expected 404 to equal 200" (a script test).
    std::string text() const;
  };
  std::vector<Check> asserts;
  struct Cap {
    bool ok = false;
    Capture::Scope scope = Capture::Runtime;
    std::string var, value, message;
    bool script = false;   // set by a script (bru.setVar, pm.environment.set...), not a capture row
    bool removed = false;  // a script's unset / delete
  };
  std::vector<Cap> captures;
  int asserts_passed = 0;
  int asserts_failed = 0;
  // Response body kept for the detail pane: up to kBodyKeep bytes (failed steps, and passing
  // ones among the first kKeepPreviews), kBodyPreview bytes for later passing steps; within
  // the run's budgets (kBodyBudget, kPreviewBudget). gRPC: the message JSON (a stream: an array).
  std::string body_preview;
  bool body_truncated = false;
  // The whole body when body_preview was cut and RunOptions::spill_bodies was on (HTTP).
  std::shared_ptr<const class SpillFile> body_file;
  std::string http_version;               // "HTTP/1.1", "HTTP/2" (HTTP only)
  std::vector<Header> resp_headers;       // HTTP: final response headers; gRPC: initial metadata
  std::vector<Header> resp_trailers;      // gRPC trailers
  std::vector<Header> req_headers;        // as sent (variables expanded, auth applied)
  std::string req_body;                   // as sent, capped like body_preview
  uint64_t req_body_size = 0;             // full size of the request body
  bool req_body_truncated = false;
  uint64_t bytes_received = 0;            // on the wire: headers + body, all hops
  bool skipped = false;                   // planned but never run (the run was stopped first)
  bool from_history = false;              // read back from a saved run (from_json): no bodies, no headers
  // Script console output (pre-request first), capped (see kLogKeep); logs_truncated when cut.
  struct Log {
    bool post = false;
    ScriptLog::Level level = ScriptLog::Level::Log;
    std::string text;
  };
  std::vector<Log> logs;
  bool logs_truncated = false;
  bool has_scripts = false;     // the request has a pre / post script
  bool scripts_ran = false;     // ... and they ran (false: not trusted, or not reached)
  int script_tests = 0;         // checks that come from scripts (part of asserts)
  // The data file row this step's iteration used (data_index from 0; -1 and null: no data file).
  int data_index = -1;
  std::shared_ptr<const DataRow> data_row;
  // gRPC: the google.rpc.Status details of a failed call, decoded (JSON), from
  // grpc-status-details-bin ("" when there were none).
  std::string grpc_details;
  // {{names}} left in the URL, headers or body as sent: not defined anywhere, sent literally.
  std::vector<std::string> unresolved;

  // Stopped by Stop / Esc while in flight or before it was sent: neither passed nor failed.
  bool stopped() const { return error == "Stopped"; }
  bool errored() const {
    return !skipped && !stopped() && (client_error || !sent || (kind == RequestKind::Http && !transport_ok));
  }
  // The status is an error (HTTP 400+, a gRPC status other than OK) and the request has no
  // res.status / res.statusText assert of its own (an inherited folder assert does not count).
  bool status_failed() const;
  bool failed() const;
  bool passed() const { return !skipped && !stopped() && !failed(); }
  // One line saying why the step failed: the error, the first failed check ("status eq 200:
  // got 404"), or the status for a request without checks. "" when it did not fail.
  std::string reason() const;
  std::string content_type() const;  // from resp_headers ("" when none)
};

inline constexpr size_t kBodyPreview = 4096;
inline constexpr size_t kBodyKeep = 256 * 1024;
inline constexpr size_t kKeepPreviews = 1000;     // passing steps after this many keep kBodyPreview
inline constexpr size_t kBodyBudget = 96u << 20;     // per run: body bytes beyond the first kBodyPreview
inline constexpr size_t kPreviewBudget = 64u << 20;  // per run: the first kBodyPreview bytes of bodies
inline constexpr int kMaxIterations = 10000000;  // a duration run's iteration cap
inline constexpr uint64_t kSpillBudget = 2ull << 30;  // live spilled body bytes, all runs together
inline constexpr size_t kLogKeep = 64 * 1024;        // script console bytes kept per step
inline constexpr size_t kLogBudget = 32u << 20;      // per run: script console bytes kept

// Numbers in assert / script text written with too many digits ("122.28609999999999") shortened
// to 10 significant digits ("122.2861"); everything else unchanged.
std::string tidy_numbers(const std::string& s);

// One row per planned step that never ran (iteration it, position k), for the Skipped filter.
// done[it * order.size() + k] tells which ran. At most max rows.
std::vector<std::shared_ptr<const StepResult>> skipped_steps(const std::vector<RunItem>& items,
                                                             const std::vector<size_t>& order, int iterations,
                                                             const std::vector<bool>& done, size_t first_seq,
                                                             size_t max);

// Data-driven iterations (Postman's data file): CSV with a header row, or a JSON array of
// objects. Each row's columns become {{vars}} for one iteration. Values that are not strings
// (JSON numbers, objects) are kept as their JSON text. False with *err on malformed input.
bool parse_data(std::string_view text, bool json, std::vector<DataRow>* rows, std::string* err);
// Reads and parses path (".json" => JSON, else CSV), at most 64 MB. Disk: worker thread only.
bool load_data_file(const std::filesystem::path& path, std::vector<DataRow>* rows, std::string* err);

// A full response body in a temp file of this process (%TEMP%\watt-runner\<pid>), deleted
// when the last StepResult holding it goes. Never on the UI thread: write and read touch disk.
class SpillFile {
 public:
  ~SpillFile();
  SpillFile(const SpillFile&) = delete;
  SpillFile& operator=(const SpillFile&) = delete;
  // Null when the bytes would go over kSpillBudget, or on an I/O error.
  static std::shared_ptr<const SpillFile> write(std::string_view body);
  bool read(std::string* out) const;
  uint64_t size() const { return size_; }
  const std::filesystem::path& path() const { return path_; }
  static uint64_t live_bytes();

 private:
  SpillFile(std::filesystem::path p, uint64_t n) : path_(std::move(p)), size_(n) {}
  std::filesystem::path path_;
  uint64_t size_ = 0;
};

struct RunOptions {
  int iterations = 1;
  int delay_ms = 0;              // before every request but the first
  bool stop_on_failure = false;  // end the run after the first failed step
  // Data file rows: iteration i uses rows[min(i, size - 1)] as runtime vars (set before its first
  // request, so a capture later in the iteration can still override them).
  std::vector<DataRow> data;
  // Parallel workers (load mode). 1: one request at a time, captures chain in order. N > 1:
  // iterations are spread over N workers, each with its own connection pool and its own copy of
  // the workspace (captures chain within an iteration, not across workers). At most 64.
  int concurrency = 1;
  // Load profile (Postman's performance runner), parallel or not:
  //   ramp_up_ms: worker w (of N) starts its first iteration w / N * ramp_up_ms after the run's
  //               start, so the load grows linearly to N workers.
  //   duration_ms: > 0 runs iterations until this much time has passed (iterations is then a
  //               cap, kMaxIterations when 0 or 1 is given); an iteration started finishes.
  //   ramp_down_ms (peak, with duration_ms): the load falls back linearly over the run's last
  //               ramp_down_ms, worker w leaving w / N * ramp_down_ms before the end.
  //   spike_base (spike, with duration_ms): > 0 keeps workers 0..spike_base-1 running all
  //               through; the others join at once for the middle fifth of the duration.
  int ramp_up_ms = 0;
  int duration_ms = 0;
  int ramp_down_ms = 0;
  int spike_base = 0;
  // Cookies (Postman's runner keeps them by default): a Set-Cookie from one response is sent by
  // the later requests of the run to that host (domain / path / Secure / Max-Age respected),
  // added to the request's own Cookie header. Per worker, in memory, gone after the run; no
  // stored cookies are ever read. false: every request sends only the cookies it sets itself.
  bool cookies = true;
  // Run pre-request / post-response scripts (the workspace's scripts are trusted). The engine
  // never asks; script.h checks the trust again before running anything.
  bool run_scripts = false;
  // Bodies over kBodyKeep also go whole to a temp file (StepResult::body_file), written on the
  // worker after the response, so the app can show the full response of every step.
  bool spill_bodies = false;
  // Rerun failed (Postman's "Run failed"): when not empty, it replaces iterations. Each entry is
  // one iteration of the run before: its data row (data[min(iteration, size - 1)]) is set again
  // and only the order positions listed are sent, reported under that iteration's number.
  struct Retry {
    int iteration = 0;
    std::vector<size_t> positions;  // into the order, ascending
  };
  std::vector<Retry> retry;
};
// Sends a RunOptions plans (order.size() x iterations, or the positions of retry).
size_t planned_sends(const RunOptions& o, size_t order_size);

class RunControl {
 public:
  void stop();
  bool stopped() const { return stop_.load(std::memory_order_acquire); }
  // Waits ms unless stopped first; false when stopped.
  bool sleep_ms(int ms);
  // The request in flight, so stop() can cancel it (null to detach). True if not stopped yet.
  // slot: the worker (0 .. concurrency-1); each worker has one request in flight at most.
  bool attach(HttpClient* c, size_t slot = 0);
  bool attach(std::shared_ptr<GrpcCall> c, size_t slot = 0);
  static constexpr size_t kSlots = 64;
  // For ScriptOptions::cancel: a script stops when the run is stopped.
  const std::atomic<bool>* flag() const { return &stop_; }

 private:
  std::atomic<bool> stop_{false};
  std::mutex m_;
  std::condition_variable cv_;
  HttpClient* http_[kSlots] = {};
  std::shared_ptr<GrpcCall> call_[kSlots];
};

// Per-request aggregate over all iterations (Newman's per-request table).
struct RequestStats {
  size_t item = 0;
  std::string name, rel, method;
  RequestKind kind = RequestKind::Http;
  size_t count = 0, failures = 0;  // finished steps (stopped / skipped not counted) / failed steps
  double avg_ms = 0, min_ms = 0, p50_ms = 0, p95_ms = 0, max_ms = 0;
};

struct RunSummary {
  size_t total = 0;    // steps finished
  size_t planned = 0;  // items x iterations
  size_t passed = 0, failed = 0, errors = 0;  // errors are also counted in failed
  size_t asserts_passed = 0, asserts_failed = 0;
  double wall_ms = 0;  // run start -> end, delays included
  double sum_ms = 0;   // sum of the sent steps' totals
  double avg_ms = 0, p50_ms = 0, p90_ms = 0, p95_ms = 0, p99_ms = 0, min_ms = 0, max_ms = 0;
  size_t samples = 0;    // sent steps with a time (a spread or a rate needs two)
  double stddev_ms = 0;  // population standard deviation of the sent steps' totals
  // DNS / connect / TLS: averaged over the steps that opened a new connection; TTFB over every
  // sent step.
  double avg_dns_ms = 0, avg_connect_ms = 0, avg_tls_ms = 0, avg_ttfb_ms = 0;
  size_t new_connections = 0;
  // Script and token time (StepResult::script_ms), outside every time above: summed over the
  // sent steps, their count, average and maximum.
  double script_sum_ms = 0, avg_script_ms = 0, max_script_ms = 0;
  // Setup before the first I/O (StepResult::prep_ms), outside every time above too.
  double prep_sum_ms = 0, avg_prep_ms = 0;
  size_t scripted = 0;
  uint64_t bytes_received = 0;  // on the wire, all sent steps
  size_t stopped_steps = 0;     // in flight / not yet sent when stopped (neither passed nor failed)
  size_t skipped = 0;           // planned but never run (a stopped run)
  std::vector<RequestStats> per_request;  // in first-run order (with_requests = true)
  bool stopped = false;
};

// Linear interpolation between closest ranks (p in [0, 100]); 0 for an empty set. Reorders v.
double percentile(std::vector<double>& v, double p);
// with_requests: also fill per_request (one more pass and a sort per request).
RunSummary summarize(const std::vector<std::shared_ptr<const StepResult>>& steps, size_t planned, double wall_ms,
                     bool stopped, bool with_requests = false);

using GrpcFactory = std::function<std::shared_ptr<GrpcClient>()>;
using StepFn = std::function<void(std::shared_ptr<const StepResult>)>;

// When worker slot (of workers) of a load profile runs: it starts its first iteration at
// start_ms and starts none at or after stop_ms (0: no end but the iterations), from the run's start.
struct LoadWindow {
  double start_ms = 0, stop_ms = 0;
};
LoadWindow load_window(const RunOptions& o, size_t slot, size_t workers);

// Runs items[order[0]], items[order[1]], ... opts.iterations times, applying captures to ws.
// on_step is called on this thread after each step. grpc is called once, on first gRPC use.
// persist_captures false: environment / globals captures change ws in memory only.
RunSummary run_plan(const std::vector<RunItem>& items, const std::vector<size_t>& order, const RunOptions& opts,
                    Workspace& ws, const GrpcFactory& grpc, RunControl& ctl, const StepFn& on_step,
                    bool persist_captures = false);

struct RunMeta {
  std::string name;         // folder / collection name
  std::string environment;  // active environment ("" = none)
  std::string started_iso;  // UTC, "2026-11-17T10:00:00Z"
  int iterations = 1;
  // Run history only (written when not empty): "rerun failed", and the plan By request compares.
  std::string label, plan;
};
std::string now_iso();
std::string to_json(const RunMeta& meta, const RunSummary& s, const std::vector<std::shared_ptr<const StepResult>>& steps);
std::string to_junit(const RunMeta& meta, const RunSummary& s, const std::vector<std::shared_ptr<const StepResult>>& steps);
std::string xml_escape(const std::string& s);  // text and attribute values; drops XML-invalid control bytes

// ---- run history across restarts (Postman keeps past runs per collection)

// A to_json report read back. Steps get item = the order their path first appears in (the
// caller maps it onto its own items); no bodies or request headers (a report has none).
struct LoadedRun {
  RunMeta meta;
  RunSummary summary;  // as saved, per_request included
  std::vector<std::shared_ptr<const StepResult>> steps;
  bool partial = false;  // only the failed steps were kept (a big run)
};
bool from_json(const std::string& text, LoadedRun& out, std::string* err = nullptr);
// The report a history file keeps: every step up to kHistorySteps, else the failed ones (up to
// kHistorySteps, "partial": true). Script-set and captured values whose names look like
// credentials (token, secret, password, key...) are masked.
constexpr size_t kHistorySteps = 5000;
std::string history_json(const RunMeta& meta, const RunSummary& s, const std::vector<std::shared_ptr<const StepResult>>& steps);
// Per user (<Workspace::user_config_dir()>/runs/<hash of key>/), never in the workspace (the
// results hold URLs and values a repository should not get). save_history writes one run and
// keeps the newest `keep`; load_history returns up to max files' contents, newest first. Disk
// work: worker threads only.
bool save_history(const std::wstring& key, const std::string& started_iso, const std::string& json, size_t keep,
                  std::string* err = nullptr);
std::vector<std::string> load_history(const std::wstring& key, size_t max);
// Self-contained HTML report (Bruno's Download Report): summary, per-request table, every step
// with its asserts; failed steps also with the response (escaped) and the request headers with
// credentials masked. No script, no external resources.
std::string to_html(const RunMeta& meta, const RunSummary& s, const std::vector<std::shared_ptr<const StepResult>>& steps);
// Header names whose values carry credentials (Authorization, Cookie, *token*, *key*, *secret*...).
bool sensitive_header(std::string_view name);

// ---- failure triage (Postman's performance view groups errors by type)

// Every number in s ("123.5", "-4", "1e3") replaced by '#', so "got 123.5" and "got 129.9" read
// the same.
std::string normalize_numbers(std::string_view s);
// One failed check of a step: what was checked ("res.status eq 201", "expected status < 400", "sent") and
// the message ("expected res.status to equal 201, got 200", "HTTP 404 Not Found").
struct Failure {
  std::string check, message;
};
// Every failure of a failed step, in order (empty when it did not fail).
std::vector<Failure> failures_of(const StepResult& s);
// Request + check + message with numbers normalised: equal for failures of one cause.
std::string failure_key(const StepResult& s, const Failure& f);
bool has_failure(const StepResult& s, const std::string& key);
struct FailureGroup {
  std::string key;
  size_t item = 0;
  std::string name, rel, method;
  RequestKind kind = RequestKind::Http;
  std::string check;
  std::string message;       // of the first occurrence
  std::string last_message;  // of the latest one
  size_t count = 0;          // failed steps with this failure
  size_t variants = 1;       // distinct messages (numbers differ), at most 1000 counted
  int first_iteration = 0, last_iteration = 0;  // lowest / highest failing iteration (0-based)
  size_t iterations = 1;                         // distinct failing iterations
  size_t first_seq = 0, last_seq = 0;
  // Failed steps of this group sent with a {{name}} nothing defined, and the names (up to 4):
  // often the cause ("Get order got 404" because {{firstId}} was not captured yet).
  size_t unresolved = 0;
  std::vector<std::string> unresolved_names;
};
// Most frequent first (ties: the one that happened first).
std::vector<FailureGroup> group_failures(const std::vector<std::shared_ptr<const StepResult>>& steps);

// ---- thresholds (Postman's performance runner pass / fail criteria)
// "p95<200 err<1% rps>=50": metrics avg min max p50 p90 p95 p99 (ms; a value may end in "ms" or
// "s"), err (failed steps, % of the finished ones) and rps (sends per second over the wall time);
// operators < <= > >=; separated by spaces, commas or semicolons.
struct Threshold {
  std::string metric;
  std::string op;
  double value = 0;
  std::string text;  // as written, normalised: "p95 < 200 ms"
};
bool parse_thresholds(const std::string& spec, std::vector<Threshold>* out, std::string* err);
struct ThresholdResult {
  Threshold t;
  bool pass = false;
  double actual = 0;
  std::string actual_text;  // "212 ms", "0.40%", "497/s"
};
std::vector<ThresholdResult> check_thresholds(const std::vector<Threshold>& t, const RunSummary& s);

// Headless run for CI (Newman, bru run): watt.exe --run <folder> [options], see runner_cli.cpp.
// Writes the console text to *out; returns the exit code (0 all passed, 1 failures, 2 usage or
// setup error).
int run_cli(const std::vector<std::string>& args, const GrpcFactory& grpc, std::string* out);

}  // namespace watt::app::runner
