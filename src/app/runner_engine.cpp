#include "app/runner_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <process.h>
#include <cctype>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "app/http_editor_pre.h"   // evaluate_pre_vars (the Scripts tab's pre-request variables)
#include "app/http_editor_util.h"  // with_path_values, split_row_docs
#include "core/model_json.h"  // read_file / write_file_atomic / json_pretty (run-order.json)
#include "core/oauth2.h"
#include "core/timing.h"

namespace watt::app::runner {

namespace fs = std::filesystem;

namespace {

// protobuf's JSON parse errors pad their parts with runs of spaces ("invalid JSON  in  x,   near
// 1:6"): one space each, as the rest of the UI writes them.
std::string one_space(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (const char c : s)
    if (!(c == ' ' && !o.empty() && o.back() == ' ')) o += c;
  return o;
}

std::string generic_utf8(const fs::path& p) {
  const auto u = p.generic_u8string();
  return std::string(u.begin(), u.end());
}

// Lexically normalized, case-insensitive (Windows paths) comparison key.
std::wstring path_key(const fs::path& p) {
  std::wstring s = p.lexically_normal().wstring();
  while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
  for (auto& c : s) {
    if (c == L'/') c = L'\\';
    c = static_cast<wchar_t>(towlower(c));
  }
  return s;
}

const CollectionNode* find_node(const CollectionNode& n, const std::wstring& key) {
  if (path_key(n.path) == key) return &n;
  for (const auto& c : n.children)
    if (const CollectionNode* f = find_node(c, key)) return f;
  return nullptr;
}

// A folder's own requests first, then its subfolders (a login request at the top of a folder
// runs before the flows below it), each in the sidebar's order.
// Depth first. `saved`: path_key(folder) -> the runner's saved order (run-order.json): its
// names first, folders and requests interleaved. The rest (and folders without a saved list) as
// the sidebar sorts: subfolders first, then the dragged order (`order`, see FolderOrder), then
// tree()'s order (folder.json "order", so "Run folder" runs what the sidebar lists, top to bottom).
using KeyedOrder = std::map<std::wstring, std::vector<std::wstring>>;
void flatten(const CollectionNode& n, std::vector<const CollectionNode*>& out, const KeyedOrder* order = nullptr,
             const KeyedOrder* saved = nullptr) {
  auto find = [&n](const KeyedOrder* m) -> const std::vector<std::wstring>* {
    if (!m) return nullptr;
    auto it = m->find(path_key(n.path));
    return it == m->end() ? nullptr : &it->second;
  };
  const std::vector<std::wstring>* ord = find(order);
  const std::vector<std::wstring>* sav = find(saved);
  auto rank_in = [](const std::vector<std::wstring>* list, const CollectionNode* c) -> size_t {
    if (!list) return SIZE_MAX;
    const std::wstring f = c->path.filename().wstring();
    for (size_t i = 0; i < list->size(); ++i)
      if (_wcsicmp((*list)[i].c_str(), f.c_str()) == 0) return i;
    return SIZE_MAX;
  };
  struct Kid {
    size_t saved, rank;
    const CollectionNode* c;
  };
  std::vector<Kid> kids;
  kids.reserve(n.children.size());
  for (const auto& c : n.children) kids.push_back({rank_in(sav, &c), rank_in(ord, &c), &c});
  std::stable_sort(kids.begin(), kids.end(), [](const Kid& a, const Kid& b) {
    if (a.saved != b.saved) return a.saved < b.saved;
    if (a.c->is_folder != b.c->is_folder) return a.c->is_folder;
    // tree() already lists children in the stored folder.json order: keep it (stable_sort).
    return a.rank < b.rank;
  });
  for (const Kid& k : kids) {
    if (k.c->is_folder) flatten(*k.c, out, order, saved);
    else out.push_back(k.c);
  }
}

std::string_view trim_ws(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
  return s;
}

// A client / bidi body given as a JSON array: one message per element (as the request tab does).
std::vector<std::string> split_messages(const std::string& body) {
  const auto t = trim_ws(body);
  if (t.empty() || t.front() != '[') return {std::string(t)};
  rapidjson::Document d;
  d.Parse(t.data(), t.size());
  if (d.HasParseError() || !d.IsArray()) return {std::string(t)};
  std::vector<std::string> out;
  out.reserve(d.Size());
  for (auto& v : d.GetArray()) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    v.Accept(w);
    out.emplace_back(sb.GetString(), sb.GetSize());
  }
  return out;
}

std::string join(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& x : v) s += (s.empty() ? "" : ", ") + x;
  return s;
}

// A response-time check failed in the standard form ("expected res.responseTime to be < 100, got
// 135.4425"): said in ms with the runner's precision ("expected response time to be < 100 ms,
// got 135 ms"), as Bruno says "expected 134 to be below 100". Anything else is kept.
void time_message(StepResult::Check& c) {
  std::string t = c.target;
  while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
  while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front()))) t.erase(0, 1);
  std::string low;
  for (char ch : t) low += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  if (low != "res.responsetime" && low != "responsetime" && low != "time" && low != "res.time") return;
  char* end = nullptr;
  const double v = std::strtod(c.actual.c_str(), &end);
  if (c.actual.empty() || !end || *end != 0 || !std::isfinite(v)) return;
  const std::string head = "expected " + t + " ";
  const std::string tail = ", got " + c.actual;
  if (c.message.size() < head.size() + tail.size() || c.message.compare(0, head.size(), head) != 0 ||
      c.message.compare(c.message.size() - tail.size(), tail.size(), tail) != 0)
    return;
  std::string mid = c.message.substr(head.size(), c.message.size() - head.size() - tail.size());
  if (!mid.empty() && std::isdigit(static_cast<unsigned char>(mid.back()))) mid += " ms";
  char b[32];
  const double ms = v < 0 ? 0 : v;
  if (ms < 1) std::snprintf(b, sizeof b, "%.2f ms", ms);
  else if (ms < 100) std::snprintf(b, sizeof b, "%.1f ms", ms);
  else std::snprintf(b, sizeof b, "%.0f ms", ms);
  c.actual = b;
  c.message = "expected response time " + mid + ", got " + b;
}

void fill_checks(StepResult& s, const std::vector<AssertResult>& ar, const std::vector<CaptureResult>& cr) {
  s.asserts.reserve(ar.size());
  for (const auto& a : ar) {
    StepResult::Check c;
    c.pass = a.pass;
    if (a.a) {
      c.target = a.a->target;
      c.op = a.a->op.empty() ? "eq" : a.a->op;
    }
    c.expected = tidy_numbers(a.expected);
    c.actual = tidy_numbers(a.actual);
    c.message = tidy_numbers(a.message);
    if (!a.pass) time_message(c);
    (a.pass ? s.asserts_passed : s.asserts_failed)++;
    s.asserts.push_back(std::move(c));
  }
  s.captures.reserve(cr.size());
  for (const auto& c : cr) {
    StepResult::Cap k;
    k.ok = c.ok;
    if (c.c) {
      k.var = c.c->var;
      k.scope = c.c->scope;
    }
    k.value = c.value;
    k.message = c.message;
    s.captures.push_back(std::move(k));
  }
}

void http_phases(StepResult& s, const HttpResponse& r) {
  const Timing& t = r.timing;
  s.phases = {{"Prepare", r.prepare_ms}, {"Queue", t.queue_ms},       {"DNS", t.dns_ms},
              {"Socket", r.socket_ms},   {"Connect", t.connect_ms},   {"TLS", t.tls_ms},
              {"Send", t.send_ms},       {"Wait (TTFB)", t.ttfb_ms},  {"Download", t.download_ms},
              {"Redirects", r.redirect_ms}, {"Process", r.process_ms}};
}

void grpc_phases(StepResult& s, const GrpcResponse& r) {
  const Timing& t = r.timing;
  s.phases = {{"Queue", t.queue_ms}, {"DNS", t.dns_ms},          {"Connect", t.connect_ms},
              {"TLS", t.tls_ms},     {"Send", t.send_ms},        {"Wait (TTFB)", t.ttfb_ms},
              {"Download", t.download_ms}};
  // Auto-reflection is inside total_ms but not one of Timing's phases: whatever the phases do
  // not cover (reflection, stream bookkeeping) is shown as its own line so the bars add up.
  double sum = 0;
  for (const auto& p : s.phases) sum += p.ms;
  const double rest = t.total_ms - sum;
  if (rest > 0.0005) s.phases.push_back({r.reflect_ms > 0 ? "Reflect / other" : "Other", rest});
}

// Up to kBodyKeep bytes; run_plan trims it to what the step may keep once pass / fail is known.
void keep_preview(StepResult& s, const std::string& body) {
  s.body_truncated = body.size() > kBodyKeep;
  s.body_preview.assign(body, 0, std::min(body.size(), kBodyKeep));
}

std::atomic<uint64_t> g_spill_bytes{0};
std::atomic<uint64_t> g_spill_seq{0};

fs::path spill_dir() {
  std::error_code ec;
  static const fs::path d = [&] {
    const fs::path top = fs::temp_directory_path(ec) / "watt-runner";
    // Left behind by a process that ended without cleaning up (killed): gone after two days.
    const auto old = fs::file_time_type::clock::now() - std::chrono::hours(48);
    for (fs::directory_iterator it(top, ec), end; !ec && it != end; it.increment(ec))
      if (std::error_code e2; it->is_directory(e2) && it->last_write_time(e2) < old) fs::remove_all(it->path(), e2);
    return top / std::to_string(_getpid());
  }();
  return d;
}

void keep_request_body(StepResult& s, const std::string& body) {
  s.req_body_size = body.size();
  s.req_body_truncated = body.size() > kBodyKeep;
  s.req_body.assign(body, 0, std::min(body.size(), kBodyKeep));
}

// Cuts a kept body to max bytes without splitting a UTF-8 sequence.
void cut_utf8(std::string& b, size_t max, bool& truncated) {
  if (b.size() <= max) return;
  size_t n = max;
  while (n > 0 && (static_cast<unsigned char>(b[n]) & 0xC0) == 0x80) --n;
  b.resize(n);
  b.shrink_to_fit();
  truncated = true;
}

char lower_ascii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
std::string lower(std::string_view s) {
  std::string o(s);
  for (auto& c : o) c = lower_ascii(c);
  return o;
}

// ---- scripts

std::vector<http_editor::PreVar> pre_rows(const SavedRequest& r) {
  std::vector<http_editor::PreVar> out;
  if (r.kind != RequestKind::Http || r.docs.find("watt:") == std::string::npos) return out;
  http_editor::RowDocs d;
  http_editor::split_row_docs(r.docs, d);
  for (const auto& x : d.pre) {
    if (!x.enabled || x.var.empty() || trim_ws(x.expr).empty()) continue;
    http_editor::PreVar v;
    v.var = std::string(trim_ws(x.var));
    v.expr = x.expr;
    v.scope = capture_scope_from(x.scope.empty() ? std::string("runtime") : x.scope);
    out.push_back(std::move(v));
  }
  return out;
}

ScriptOptions script_opts(const RunControl& ctl) {
  ScriptOptions o;
  o.cancel = ctl.flag();
  return o;
}

// A script run's results into the step: tests (and an uncaught error) as checks, console output,
// variable writes as captures (applied to ws first).
void add_script(StepResult& s, ScriptRun& run, bool post, Workspace& ws) {
  if (!run.ran && !run.untrusted) return;
  for (const auto& l : run.logs) s.logs.push_back({post, l.level, l.text});
  if (run.logs_truncated) s.logs_truncated = true;
  if (!run.ok && !run.error.empty()) s.logs.push_back({post, ScriptLog::Level::Error, "Uncaught: " + run.error});
  apply_script_vars(ws, run, false);
  for (const auto& v : run.vars_set) {
    StepResult::Cap k;
    k.ok = v.ok;
    k.script = true;
    k.removed = v.removed;
    k.var = v.name;
    k.value = v.value;
    k.message = v.message;
    k.scope = v.scope == ScriptVar::Scope::Environment ? Capture::Environment
              : v.scope == ScriptVar::Scope::Globals   ? Capture::Globals
                                                       : Capture::Runtime;
    s.captures.push_back(std::move(k));
  }
  for (const auto& t : run.tests) {
    StepResult::Check c;
    c.pass = t.pass;
    c.script = true;
    c.target = t.name;
    c.op = post ? "test" : "pre-request test";
    c.message = tidy_numbers(t.message);
    (c.pass ? s.asserts_passed : s.asserts_failed)++;
    ++s.script_tests;
    s.asserts.push_back(std::move(c));
  }
  if (!run.ok && post) {
    // A thrown post-response script fails the step (a pre-request one stops the send).
    StepResult::Check c;
    c.script = true;
    c.target = "post-response script";
    c.op = "error";
    c.message = run.error;
    ++s.asserts_failed;
    ++s.script_tests;
    s.asserts.push_back(std::move(c));
  }
}

void untrusted_check(StepResult& s) {
  StepResult::Check c;
  c.script = true;
  c.target = "scripts";
  c.op = "not run";
  c.message = "scripts not run (workspace not trusted)";
  ++s.asserts_failed;
  ++s.script_tests;
  s.asserts.push_back(std::move(c));
}

// Puts a phase first, taking its time out of the Queue phase (the runner's setup, which
// included it).
// Adds every {{name}} left in text (as sent) to s.unresolved, once each, at most 8; bodies are
// scanned up to their first 256 KB.
void find_unresolved(StepResult& s, std::string_view text) {
  if (text.size() > (256u << 10)) text = text.substr(0, 256u << 10);
  for (size_t at = text.find("{{"); at != std::string_view::npos && s.unresolved.size() < 8; at = text.find("{{", at + 2)) {
    const size_t end = text.find("}}", at + 2);
    if (end == std::string_view::npos) break;
    std::string_view name = trim_ws(text.substr(at + 2, end - at - 2));
    if (name.empty() || name.size() > 64) continue;
    const bool ident = std::all_of(name.begin(), name.end(), [](char c) {
      return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == '$';
    });
    if (!ident) continue;
    if (std::find(s.unresolved.begin(), s.unresolved.end(), name) == s.unresolved.end()) s.unresolved.emplace_back(name);
  }
}

// The time was measured inside the runner's setup (Queue, and so total_ms): it moves out of
// both into script_ms, so total_ms stays the response time.
void script_phase_front(StepResult& s, const char* name, double ms) {
  if (ms <= 0) return;
  for (auto& p : s.phases)
    if (std::strcmp(p.name, "Queue") == 0) p.ms = std::max(0.0, p.ms - ms);
  s.timing.queue_ms = std::max(0.0, s.timing.queue_ms - ms);
  s.timing.total_ms = std::max(0.0, s.timing.total_ms - ms);
  s.script_ms += ms;
  s.phases.insert(s.phases.begin(), Phase{name, ms});
}

// The OAuth token step (a cache lookup when the token is still valid) was measured inside Queue:
// its own phase, still setup (prep_ms, setup_phases_out), never script time: a request without
// scripts shows no Script time.
void setup_phase_front(StepResult& s, const char* name, double ms) {
  if (ms <= 0) return;
  for (auto& p : s.phases)
    if (std::strcmp(p.name, "Queue") == 0) p.ms = std::max(0.0, p.ms - ms);
  s.timing.queue_ms = std::max(0.0, s.timing.queue_ms - ms);
  s.phases.insert(s.phases.begin(), Phase{name, ms});
}

// The runner's own setup (expansion, files, consent checks), watt's request setup and the
// client's queue before the first I/O (Prepare, Queue): out of total_ms into prep_ms, so the
// Time column and every percentile measure the server and the network, not watt (Newman and
// Postman report their prepare time apart too). Called after the script phases left Queue.
void setup_phases_out(StepResult& s) {
  double prep = 0;
  for (const auto& p : s.phases)
    if (is_setup_phase(p)) prep += std::max(0.0, p.ms);
  s.prep_ms = prep;
  s.timing.total_ms = std::max(0.0, s.timing.total_ms - prep);
}


}  // namespace

bool is_script_phase(const Phase& p) {
  for (const char* n : {"Pre-request vars", "Pre-request script", "Post-response script"})
    if (std::strcmp(p.name, n) == 0) return true;
  return false;
}

bool is_setup_phase(const Phase& p) {
  return std::strcmp(p.name, "Prepare") == 0 || std::strcmp(p.name, "Queue") == 0 || std::strcmp(p.name, "Auth / token") == 0;
}

std::string strip_ext(std::string s) {
  static const std::string ext = ".req.json";
  if (s.size() > ext.size() && s.compare(s.size() - ext.size(), ext.size(), ext) == 0) s.resize(s.size() - ext.size());
  return s;
}

// ------------------------------------------------------------------ collect

std::vector<RunItem> collect(const Workspace& ws, const fs::path& folder, std::vector<std::string>* errors,
                             const FolderOrder* order) {
  KeyedOrder keyed, saved;
  if (order)
    for (const auto& [dir, names] : *order) keyed[path_key(dir)] = names;
  for (auto& [dir, names] : load_run_order(ws.root())) saved[path_key(dir)] = std::move(names);
  std::vector<RunItem> out;
  const CollectionNode tree = ws.tree(-1);
  const CollectionNode* base = &tree;
  if (!folder.empty() && path_key(folder) != path_key(ws.root())) {
    // Only what tree() lists can run: nothing outside the workspace, whatever path was asked for.
    base = find_node(tree, path_key(folder));
    if (!base) {
      if (errors) errors->push_back(generic_utf8(folder) + ": not a folder or request of this workspace");
      return out;
    }
  }
  std::vector<const CollectionNode*> files;
  if (base->is_folder || base == &tree)
    flatten(*base, files, order ? &keyed : nullptr, saved.empty() ? nullptr : &saved);
  else
    files.push_back(base);
  const fs::path rel_base = base->is_folder || base == &tree ? base->path : base->path.parent_path();
  out.reserve(files.size());
  for (const CollectionNode* f : files) {
    std::error_code ec;
    fs::path rel = fs::relative(f->path, rel_base, ec);
    if (ec || rel.empty()) rel = f->path.filename();
    std::string err;
    auto r = ws.load_request(f->path, &err);
    if (!r) {
      if (errors) errors->push_back(strip_ext(generic_utf8(rel)) + ": " + (err.empty() ? "cannot read" : err));
      continue;
    }
    // A WebSocket request is a connection, not a request / response the runner can time and
    // assert (Postman's and Bruno's runners leave them out too).
    if (r->kind == RequestKind::WebSocket) continue;
    RunItem it;
    it.req = std::move(*r);
    it.rel = strip_ext(generic_utf8(rel));
    if (it.req.name.empty()) it.req.name = f->name;
    out.push_back(std::move(it));
  }
  return out;
}

// ------------------------------------------------------------------ saved run order

namespace {
bool plain_name(const std::wstring& n) {
  return !n.empty() && n != L"." && n != L".." && n.find_first_of(L"/\\:") == std::wstring::npos;
}
// "." for the root, "a/b" below it; "" when folder is not inside root.
std::string order_key(const fs::path& root, const fs::path& folder) {
  if (path_key(folder) == path_key(root)) return ".";
  const fs::path rel = folder.lexically_normal().lexically_relative(root.lexically_normal());
  if (rel.empty() || rel.is_absolute()) return "";
  for (const auto& c : rel)
    if (c == L".." || c == L".") return "";
  return generic_utf8(rel);
}
std::mutex& order_file_mutex() {
  static std::mutex m;
  return m;
}
}  // namespace

FolderOrder load_run_order(const fs::path& root, std::string* err) {
  FolderOrder out;
  const fs::path file = root / kRunOrderFile;
  detail::PathStat st;
  if (!detail::stat_path(file, st) || !st.exists) return out;
  if (st.is_link || st.is_dir || st.size > (1u << 20)) {
    if (err) *err = std::string(kRunOrderFile) + ": ignored (a link, a folder or over 1 MB)";
    return out;
  }
  std::string text;
  if (!detail::read_file(file, text)) {
    if (err) *err = std::string(kRunOrderFile) + ": cannot read";
    return out;
  }
  rapidjson::Document d;
  d.Parse(text.data(), text.size());
  if (d.HasParseError() || !d.IsObject() || !d.HasMember("order") || !d["order"].IsObject()) {
    if (err) *err = std::string(kRunOrderFile) + ": not a run order (expected {\"order\": {...}})";
    return out;
  }
  const rapidjson::Value& obj = d["order"];
  for (auto m = obj.MemberBegin(); m != obj.MemberEnd(); ++m) {
    if (!m->value.IsArray()) continue;
    const std::string key(m->name.GetString(), m->name.GetStringLength());
    fs::path dir;
    if (key == "." || key.empty()) {
      dir = root;
    } else {
      const fs::path rel = detail::from_utf8(key);
      if (rel.is_absolute() || rel.has_root_name() || rel.has_root_directory()) continue;
      bool ok = true;
      for (const auto& c : rel) ok = ok && c != L"..";
      if (!ok) continue;
      dir = (root / rel).lexically_normal();
    }
    std::vector<std::wstring> names;
    for (auto v = m->value.Begin(); v != m->value.End(); ++v) {
      if (!v->IsString()) continue;
      std::wstring n = detail::from_utf8(std::string(v->GetString(), v->GetStringLength())).wstring();
      if (plain_name(n)) names.push_back(std::move(n));
    }
    out[dir] = std::move(names);
  }
  return out;
}

bool save_run_order(const fs::path& root, const FolderOrder& changed, uint64_t seq, std::string* err) {
  std::lock_guard lk(order_file_mutex());
  static std::map<std::wstring, uint64_t> last;
  uint64_t& done = last[path_key(root)];
  if (seq && seq < done) return true;  // a newer order is already on disk
  if (seq) done = seq;
  // What is there now (other folders' entries are kept).
  std::vector<std::pair<std::string, std::vector<std::string>>> entries;
  for (const auto& [dir, names] : load_run_order(root)) {
    const std::string k = order_key(root, dir);
    if (k.empty()) continue;
    std::vector<std::string> v;
    for (const auto& n : names) v.push_back(detail::to_utf8(fs::path(n)));
    entries.emplace_back(k, std::move(v));
  }
  for (const auto& [dir, names] : changed) {
    const std::string k = order_key(root, dir);
    if (k.empty()) continue;
    auto it = std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return e.first == k; });
    std::vector<std::string> v;
    for (const auto& n : names)
      if (plain_name(n)) v.push_back(detail::to_utf8(fs::path(n)));
    if (v.empty()) {
      if (it != entries.end()) entries.erase(it);
    } else if (it != entries.end()) {
      it->second = std::move(v);
    } else {
      entries.emplace_back(k, std::move(v));
    }
  }
  std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
    if ((a.first == ".") != (b.first == ".")) return a.first == ".";
    return a.first < b.first;
  });
  return detail::write_file_atomic(root / kRunOrderFile, run_order_json(entries), err);
}

// One name per line, commas at the line ends, two-space indents: a file meant to be committed
// diffs line by line when one item moves.
std::string run_order_json(const std::vector<std::pair<std::string, std::vector<std::string>>>& entries) {
  auto str = [](const std::string& v) {
    rapidjson::StringBuffer b;
    rapidjson::Writer<rapidjson::StringBuffer> w(b);
    w.String(v.c_str(), static_cast<rapidjson::SizeType>(v.size()));
    return std::string(b.GetString(), b.GetSize());
  };
  std::string o = "{\n  \"version\": 1,\n  \"order\": {";
  for (size_t e = 0; e < entries.size(); ++e) {
    o += e ? ",\n    " : "\n    ";
    o += str(entries[e].first) + ": [";
    const auto& v = entries[e].second;
    for (size_t i = 0; i < v.size(); ++i) o += (i ? ",\n      " : "\n      ") + str(v[i]);
    o += v.empty() ? "]" : "\n    ]";
  }
  o += entries.empty() ? "}\n}\n" : "\n  }\n}\n";
  return o;
}

namespace {
// An item's place below the run's folder: its folder names, then its file name.
std::vector<std::wstring> tree_path(const RunItem& it) {
  std::vector<std::wstring> v;
  const fs::path rel = detail::from_utf8(it.rel);
  for (const auto& c : rel.parent_path()) {
    std::wstring w = c.wstring();
    if (!w.empty() && w != L"." && w != L"/" && w != L"\\") v.push_back(std::move(w));
  }
  v.push_back(it.req.file.empty() ? rel.filename().wstring() + L".req.json" : it.req.file.filename().wstring());
  return v;
}
bool same_prefix(const std::vector<std::wstring>& a, const std::vector<std::wstring>& b, size_t n) {
  if (a.size() < n || b.size() < n) return false;
  for (size_t i = 0; i < n; ++i)
    if (a[i] != b[i]) return false;
  return true;
}
}  // namespace

bool move_block(const std::vector<RunItem>& items, std::vector<size_t>& order, const fs::path& base, size_t pos,
                int d, FolderOrder* changed) {
  if (pos >= order.size() || d == 0) return false;
  std::vector<std::vector<std::wstring>> p(order.size());
  for (size_t k = 0; k < order.size(); ++k)
    if (order[k] < items.size()) p[k] = tree_path(items[order[k]]);
  const std::vector<std::wstring> me = p[pos];
  for (size_t L = me.size(); L-- > 0;) {
    // The block: the items sharing me[0..L] (the request itself, or its folder at depth L).
    size_t bs = pos, be = pos + 1;
    while (bs > 0 && same_prefix(p[bs - 1], me, L + 1)) --bs;
    while (be < order.size() && same_prefix(p[be], me, L + 1)) ++be;
    if (d < 0) {
      if (bs == 0 || p[bs - 1].size() <= L || !same_prefix(p[bs - 1], me, L)) continue;  // first in its folder
      const std::vector<std::wstring> nb = p[bs - 1];
      size_t ns = bs - 1;
      while (ns > 0 && same_prefix(p[ns - 1], nb, L + 1)) --ns;
      std::rotate(order.begin() + static_cast<std::ptrdiff_t>(ns), order.begin() + static_cast<std::ptrdiff_t>(bs),
                  order.begin() + static_cast<std::ptrdiff_t>(be));
    } else {
      if (be >= order.size() || p[be].size() <= L || !same_prefix(p[be], me, L)) continue;  // last in its folder
      const std::vector<std::wstring> nb = p[be];
      size_t ne = be + 1;
      while (ne < order.size() && same_prefix(p[ne], nb, L + 1)) ++ne;
      std::rotate(order.begin() + static_cast<std::ptrdiff_t>(bs), order.begin() + static_cast<std::ptrdiff_t>(be),
                  order.begin() + static_cast<std::ptrdiff_t>(ne));
    }
    if (changed) {
      // The folder's new child list, in run order.
      fs::path dir = base;
      for (size_t i = 0; i < L; ++i) dir /= me[i];
      std::vector<std::wstring> names;
      for (size_t k = 0; k < order.size(); ++k) {
        if (order[k] >= items.size()) continue;
        const auto q = tree_path(items[order[k]]);
        if (q.size() <= L || !same_prefix(q, me, L)) continue;
        if (names.empty() || names.back() != q[L]) names.push_back(q[L]);
      }
      (*changed)[dir.lexically_normal()] = std::move(names);
    }
    return true;
  }
  return false;
}

bool drop_block(const std::vector<RunItem>& items, std::vector<size_t>& order, const fs::path& base, size_t pos,
                size_t before, FolderOrder* changed) {
  if (pos >= order.size() || before > order.size() || before == pos || before == pos + 1) return false;
  const size_t item = order[pos];
  auto where = [&] { return static_cast<size_t>(std::find(order.begin(), order.end(), item) - order.begin()); };
  bool moved = false;
  if (before < pos) {
    for (size_t cur = pos; cur > before;) {
      if (!move_block(items, order, base, cur, -1, changed)) break;
      const size_t now = where();
      moved = true;
      if (now >= cur) break;
      cur = now;
    }
  } else {
    for (size_t cur = pos; cur + 1 < before;) {
      if (!move_block(items, order, base, cur, 1, changed)) break;
      const size_t now = where();
      moved = true;
      if (now <= cur) break;
      cur = now;
    }
  }
  return moved;
}

namespace {
// Adds every {{name}} in text (first 256 KB) to out.
void vars_in(std::string_view text, std::vector<std::string>& out) {
  if (text.size() > (256u << 10)) text = text.substr(0, 256u << 10);
  for (size_t at = text.find("{{"); at != std::string_view::npos; at = text.find("{{", at + 2)) {
    const size_t end = text.find("}}", at + 2);
    if (end == std::string_view::npos) break;
    const std::string_view name = trim_ws(text.substr(at + 2, end - at - 2));
    if (name.empty() || name.size() > 64 || name.front() == '$') continue;  // {{$guid}}: dynamic
    if (std::find(out.begin(), out.end(), name) == out.end()) out.emplace_back(name);
  }
}
// Every {{var}} a request reads where captures matter (URL, headers, path params, body; gRPC:
// address, body, metadata).
void vars_used(const SavedRequest& r, std::vector<std::string>& used) {
  if (r.kind == RequestKind::Grpc) {
    vars_in(r.grpc_target.address, used);
    vars_in(r.grpc_body, used);
    for (const auto& m : r.grpc_opts.metadata) vars_in(m.second, used);
    for (const auto& m : r.grpc_opts.rpc_metadata) vars_in(m.second, used);
  } else {
    vars_in(r.http.url, used);
    for (const auto& h : r.http.headers) vars_in(h.second, used);
    for (const auto& p : r.path_params) vars_in(p.second, used);
    vars_in(r.http.body, used);
  }
}
}  // namespace

std::vector<OrderHint> capture_order_hints(const std::vector<RunItem>& items, const std::vector<size_t>& order,
                                           size_t max) {
  std::vector<OrderHint> out;
  // var -> first position that captures it.
  std::map<std::string, size_t, std::less<>> first_capture;
  for (size_t k = 0; k < order.size(); ++k) {
    if (order[k] >= items.size()) continue;
    for (const auto& c : items[order[k]].req.captures)
      if (c.enabled && !c.var.empty()) first_capture.emplace(c.var, k);
  }
  if (first_capture.empty()) return out;
  std::vector<std::string> used;
  for (size_t k = 0; k < order.size() && out.size() < max; ++k) {
    if (order[k] >= items.size()) continue;
    used.clear();
    vars_used(items[order[k]].req, used);
    for (const auto& v : used) {
      const auto it = first_capture.find(v);
      if (it == first_capture.end() || it->second <= k) continue;
      out.push_back({k, it->second, v});
      if (out.size() >= max) break;
    }
  }
  return out;
}

std::vector<size_t> with_capture_sources(const std::vector<RunItem>& items, const std::vector<size_t>& order,
                                         const std::vector<size_t>& positions) {
  std::set<size_t> want(positions.begin(), positions.end());
  std::vector<size_t> todo(positions.begin(), positions.end());
  std::vector<std::string> used;
  while (!todo.empty()) {
    const size_t k = todo.back();
    todo.pop_back();
    if (k >= order.size() || order[k] >= items.size()) continue;
    used.clear();
    vars_used(items[order[k]].req, used);
    for (const auto& v : used) {
      // The last request before k that captures v sets what k reads.
      for (size_t q = k; q-- > 0;) {
        if (order[q] >= items.size()) continue;
        const auto& caps = items[order[q]].req.captures;
        if (std::none_of(caps.begin(), caps.end(), [&](const Capture& c) { return c.enabled && c.var == v; })) continue;
        if (want.insert(q).second) todo.push_back(q);
        break;
      }
    }
  }
  return {want.begin(), want.end()};
}

std::string tidy_numbers(const std::string& in) {
  // Only a run of >= 13 digits with a '.' (what printing a double with 17 digits gives) changes.
  std::string out;
  size_t i = 0;
  const size_t n = in.size();
  bool changed = false;
  while (i < n) {
    const bool start = std::isdigit(static_cast<unsigned char>(in[i])) &&
                       (i == 0 || !(std::isalnum(static_cast<unsigned char>(in[i - 1])) || in[i - 1] == '.' || in[i - 1] == '_'));
    if (!start) {
      out += in[i++];
      continue;
    }
    size_t j = i, digits = 0, dots = 0;
    while (j < n && (std::isdigit(static_cast<unsigned char>(in[j])) || in[j] == '.')) {
      if (in[j] == '.') ++dots;
      else ++digits;
      ++j;
    }
    if (j > i && in[j - 1] == '.') {  // "5." at the end of a sentence
      --j;
      --dots;
    }
    const bool next_ok = j >= n || !(std::isalpha(static_cast<unsigned char>(in[j])) || in[j] == '_');
    if (dots == 1 && digits >= 13 && next_ok) {
      const std::string tok = in.substr(i, j - i);
      char* end = nullptr;
      const double v = std::strtod(tok.c_str(), &end);
      if (end && *end == 0 && std::isfinite(v)) {
        char b[40];
        std::snprintf(b, sizeof b, "%.10g", v);
        out += b;
        changed = true;
        i = j;
        continue;
      }
    }
    out.append(in, i, j - i);
    i = j;
  }
  return changed ? out : in;
}

std::string StepResult::Check::label() const {
  if (script) return target;
  return target + " " + op + (expected.empty() ? "" : " " + expected) + (from.empty() ? "" : "  (from " + from + ")");
}

std::string StepResult::Check::why() const {
  if (pass) return {};
  if (!message.empty()) return message;
  return "got " + (actual.empty() ? std::string("(undefined)") : actual);
}

std::string StepResult::Check::text() const {
  if (script && op == "not run") return message;
  if (script) return (op == "error" ? std::string("script error: ") : target + ": ") + why();
  if (!pass && !message.empty()) return message + (from.empty() ? "" : " (from " + from + ")");
  return label() + ": " + why();
}

std::string StepResult::reason() const {
  if (!failed()) return {};
  if (!error.empty()) return error;
  for (const auto& a : asserts)
    if (!a.pass) return a.text();
  if (kind == RequestKind::Grpc)
    return "gRPC status " + (status_text.empty() ? std::to_string(status) : status_text) +
           " (fails unless the request asserts on res.status)";
  return "HTTP " + std::to_string(status) + (status_text.empty() ? "" : " " + status_text) +
         " (a status of 400 or more fails unless the request asserts on res.status)";
}

bool has_scripts(const SavedRequest& r) { return !r.pre_script.empty() || !r.post_script.empty(); }

std::string url_with_path_params(const SavedRequest& r, const Workspace& ws) {
  if (r.path_params.empty()) return r.http.url;
  return http_editor::with_path_values(r.http.url, [&](const std::string& n) {
    for (const auto& [k, v] : r.path_params)
      if (k == n) return ws.expand(v);
    return std::string();
  });
}

void encode_as_sent(SavedRequest& r, const Workspace& ws) {
  const auto exp = [&ws](const std::string& span) { return ws.expand(span); };
  std::string u = http_editor::normalize_url(r.http.url);
  if (u.find("{{") != std::string::npos) {
    // Only where encoding changes what plain expansion would send (as the editor does), so a
    // value http_to_send() would expand the same is left to it.
    if (std::string sent = http_editor::expand_url(u, exp); sent != ws.expand(u)) u = std::move(sent);
  }
  r.http.url = std::move(u);
  if (r.form.empty() && r.http.body.find("{{") != std::string::npos) {
    bool urlencoded = false;
    for (size_t i = 0; i < r.http.headers.size(); ++i) {
      const auto& [k, v] = r.http.headers[i];
      if (r.disabled_headers.count(i) || lower(k) != "content-type") continue;
      urlencoded = lower(v).find("application/x-www-form-urlencoded") != std::string::npos;
    }
    if (urlencoded)
      if (std::string b = http_editor::expand_form(r.http.body, exp); b != ws.expand(r.http.body)) r.http.body = std::move(b);
  }
}

Consent consent_needed(const Workspace& ws, const std::vector<RunItem>& items, const std::vector<size_t>& order) {
  Consent c;
  std::set<std::wstring> seen_files;
  std::set<std::string> seen_env;
  std::set<std::string> seen_net;
  auto add_file = [&](const fs::path& p) {
    if (seen_files.insert(path_key(p)).second) c.files.push_back(p);
  };
  for (size_t i : order) {
    if (i >= items.size()) continue;
    SavedRequest r = items[i].req;
    if (r.kind == RequestKind::Http) r.http.url = url_with_path_params(r, ws);
    for (auto& v : ws.blocked_env_vars(r))
      if (seen_env.insert(v).second) c.env_vars.push_back(std::move(v));
    if (r.kind == RequestKind::Http) {
      for (const auto& p : ws.blocked_files(r)) add_file(p);
      for (auto& it : ws.blocked_network_settings(r))
        if (seen_net.insert(it.key).second) c.network.push_back(std::move(it));
      if (const auto rows = pre_rows(r); !rows.empty())
        for (auto& v : http_editor::pre_env_refs(rows, ws))
          if (seen_env.insert(v).second) c.env_vars.push_back(std::move(v));
    } else if (!r.proto_files.empty()) {
      for (const auto& p : ws.blocked_schema_paths(r)) add_file(p);
    }
    if (has_scripts(r)) ++c.script_requests;
  }
  if (c.script_requests) c.scripts_trusted = ws.scripts_trusted();
  return c;
}

std::string StepResult::content_type() const {
  for (const auto& [k, v] : resp_headers)
    if (lower(k) == "content-type") return v;
  return {};
}

bool StepResult::status_failed() const {
  if (!sent || client_error) return false;
  if (kind == RequestKind::Grpc ? transport_ok : status < 400) return false;
  for (const auto& a : asserts)
    if (!a.script && a.from.empty() && (a.target == "res.status" || a.target == "res.statusText")) return false;
  return true;
}

bool StepResult::failed() const {
  if (skipped || stopped()) return false;
  if (errored()) return true;
  // An error status fails the step even when every (inherited) assert passed, unless the
  // request itself asserts on its status (an expected 404, NOT_FOUND).
  if (status_failed()) return true;
  return asserts_failed > 0;
}

// ------------------------------------------------------------------ control

void RunControl::stop() {
  std::vector<HttpClient*> h;
  std::vector<std::shared_ptr<GrpcCall>> c;
  {
    std::lock_guard lk(m_);
    stop_.store(true, std::memory_order_release);
    for (size_t i = 0; i < kSlots; ++i) {
      if (http_[i]) h.push_back(http_[i]);
      if (call_[i]) c.push_back(call_[i]);
    }
  }
  cv_.notify_all();
  for (auto* x : h) x->cancel();
  for (auto& x : c) x->cancel();
}

bool RunControl::sleep_ms(int ms) {
  if (ms <= 0) return !stopped();
  std::unique_lock lk(m_);
  cv_.wait_for(lk, std::chrono::milliseconds(ms), [this] { return stopped(); });
  return !stopped();
}

bool RunControl::attach(HttpClient* c, size_t slot) {
  std::lock_guard lk(m_);
  http_[slot % kSlots] = c;
  return !stopped();
}

bool RunControl::attach(std::shared_ptr<GrpcCall> c, size_t slot) {
  std::lock_guard lk(m_);
  call_[slot % kSlots] = std::move(c);
  return !stopped();
}

// ------------------------------------------------------------------ summary

double percentile(std::vector<double>& v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  p = std::clamp(p, 0.0, 100.0);
  const double rank = p / 100.0 * static_cast<double>(v.size() - 1);
  const size_t lo = static_cast<size_t>(std::floor(rank));
  const size_t hi = std::min(lo + 1, v.size() - 1);
  return v[lo] + (v[hi] - v[lo]) * (rank - static_cast<double>(lo));
}

RunSummary summarize(const std::vector<std::shared_ptr<const StepResult>>& steps, size_t planned, double wall_ms,
                     bool stopped, bool with_requests) {
  RunSummary s;
  s.planned = planned;
  s.wall_ms = wall_ms;
  s.stopped = stopped;
  std::vector<double> t;
  t.reserve(steps.size());
  double sq = 0, dns = 0, con = 0, tls = 0, ttfb = 0;
  for (const auto& p : steps) {
    const StepResult& r = *p;
    if (r.skipped) {
      ++s.skipped;
      continue;
    }
    ++s.total;
    if (r.stopped())
      ++s.stopped_steps;
    else if (r.failed())
      ++s.failed;
    else
      ++s.passed;
    if (r.errored()) ++s.errors;
    s.asserts_passed += static_cast<size_t>(r.asserts_passed);
    s.asserts_failed += static_cast<size_t>(r.asserts_failed);
    if (r.sent && !r.stopped()) {
      s.prep_sum_ms += r.prep_ms;
      if (r.script_ms > 0) {
        ++s.scripted;
        s.script_sum_ms += r.script_ms;
        s.max_script_ms = std::max(s.max_script_ms, r.script_ms);
      }
      t.push_back(r.timing.total_ms);
      s.sum_ms += r.timing.total_ms;
      sq += r.timing.total_ms * r.timing.total_ms;
      s.bytes_received += r.bytes_received;
      ttfb += r.timing.ttfb_ms;
      if (!r.timing.connection_reused) {
        ++s.new_connections;
        dns += r.timing.dns_ms;
        con += r.timing.connect_ms;
        tls += r.timing.tls_ms;
      }
    }
  }
  if (stopped && planned > s.total + s.skipped) s.skipped = planned - s.total;
  if (s.scripted) s.avg_script_ms = s.script_sum_ms / static_cast<double>(s.scripted);
  if (!t.empty()) {
    const double n = static_cast<double>(t.size());
    s.avg_ms = s.sum_ms / n;
    s.stddev_ms = std::sqrt(std::max(0.0, sq / n - s.avg_ms * s.avg_ms));
    s.avg_ttfb_ms = ttfb / n;
    s.avg_prep_ms = s.prep_sum_ms / n;
    if (s.new_connections) {
      const double nc = static_cast<double>(s.new_connections);
      s.avg_dns_ms = dns / nc;
      s.avg_connect_ms = con / nc;
      s.avg_tls_ms = tls / nc;
    }
    s.samples = t.size();
    s.p50_ms = percentile(t, 50);  // sorts t
    s.p90_ms = percentile(t, 90);
    s.p95_ms = percentile(t, 95);
    s.p99_ms = percentile(t, 99);
    s.min_ms = t.front();
    s.max_ms = t.back();
  }
  if (with_requests) {
    std::map<size_t, size_t> at;  // item -> index in per_request
    std::vector<std::vector<double>> times;
    for (const auto& p : steps) {
      const StepResult& r = *p;
      if (r.skipped || r.stopped()) continue;
      auto [it, fresh] = at.try_emplace(r.item, s.per_request.size());
      if (fresh) {
        RequestStats q;
        q.item = r.item;
        q.name = r.name;
        q.rel = r.rel;
        q.method = r.method;
        q.kind = r.kind;
        s.per_request.push_back(std::move(q));
        times.emplace_back();
      }
      RequestStats& q = s.per_request[it->second];
      ++q.count;
      if (r.failed()) ++q.failures;
      if (r.sent) times[it->second].push_back(r.timing.total_ms);
    }
    for (size_t i = 0; i < s.per_request.size(); ++i) {
      auto& v = times[i];
      if (v.empty()) continue;
      double sum = 0;
      for (double x : v) sum += x;
      RequestStats& q = s.per_request[i];
      q.avg_ms = sum / static_cast<double>(v.size());
      q.p50_ms = percentile(v, 50);
      q.p95_ms = percentile(v, 95);
      q.min_ms = v.front();
      q.max_ms = v.back();
    }
  }
  return s;
}

// ------------------------------------------------------------------ run

namespace {

// A run's cookie jar (RFC 6265, simplified: no public-suffix list, Expires is not parsed, only
// Max-Age; a Domain attribute that does not match the host is ignored).
class CookieJar {
 public:
  static std::string host_of(const std::string& url, bool* https) {
    const size_t s = url.find("://");
    if (https) *https = s != std::string::npos && lower(url.substr(0, s)) == "https";
    std::string h = s == std::string::npos ? url : url.substr(s + 3);
    h = h.substr(0, h.find_first_of("/?#"));
    if (const size_t at = h.rfind('@'); at != std::string::npos) h.erase(0, at + 1);
    if (!h.empty() && h[0] == '[') return lower(h.substr(0, h.find(']') + 1));
    return lower(h.substr(0, h.find(':')));
  }
  static std::string path_of(const std::string& url) {
    const size_t s = url.find("://");
    const size_t p = url.find('/', s == std::string::npos ? 0 : s + 3);
    if (p == std::string::npos) return "/";
    return url.substr(p, url.find_first_of("?#", p) - p);
  }
  void take(const std::string& url, const std::vector<Header>& headers) {
    bool https = false;
    const std::string host = host_of(url, &https);
    for (const auto& [k, v] : headers) {
      if (lower(k) != "set-cookie") continue;
      C c;
      std::string_view rest = v;
      const size_t semi = rest.find(';');
      const std::string_view pair = trim_ws(rest.substr(0, semi));
      const size_t eq = pair.find('=');
      if (eq == std::string_view::npos || eq == 0) continue;
      c.name = std::string(trim_ws(pair.substr(0, eq)));
      c.value = std::string(trim_ws(pair.substr(eq + 1)));
      c.domain = host;
      c.path = "/";
      bool remove = false;
      rest = semi == std::string_view::npos ? std::string_view() : rest.substr(semi + 1);
      while (!rest.empty()) {
        const size_t n = rest.find(';');
        const std::string_view attr = trim_ws(rest.substr(0, n));
        rest = n == std::string_view::npos ? std::string_view() : rest.substr(n + 1);
        const size_t e = attr.find('=');
        const std::string an = lower(trim_ws(attr.substr(0, e)));
        const std::string av = e == std::string_view::npos ? std::string() : std::string(trim_ws(attr.substr(e + 1)));
        if (an == "domain" && !av.empty()) {
          std::string d = lower(av[0] == '.' ? av.substr(1) : av);
          if (d == host || (host.size() > d.size() && host.compare(host.size() - d.size(), d.size(), d) == 0 &&
                            host[host.size() - d.size() - 1] == '.')) {
            c.domain = d;
            c.host_only = false;
          }
        } else if (an == "path" && !av.empty() && av[0] == '/') {
          c.path = av;
        } else if (an == "secure") {
          c.secure = true;
        } else if (an == "max-age") {
          remove = !av.empty() && (av[0] == '-' || av == "0");
        }
      }
      auto it = std::find_if(jar_.begin(), jar_.end(), [&](const C& x) {
        return x.name == c.name && x.domain == c.domain && x.path == c.path;
      });
      if (it != jar_.end()) jar_.erase(it);
      if (!remove && jar_.size() < 300) jar_.push_back(std::move(c));
    }
  }
  // "a=1; b=2" for url ("" when none).
  std::string header_for(const std::string& url) const {
    bool https = false;
    const std::string host = host_of(url, &https);
    const std::string path = path_of(url);
    std::string out;
    for (const auto& c : jar_) {
      const bool dom = host == c.domain || (!c.host_only && host.size() > c.domain.size() &&
                                            host.compare(host.size() - c.domain.size(), c.domain.size(), c.domain) == 0 &&
                                            host[host.size() - c.domain.size() - 1] == '.');
      if (!dom || (c.secure && !https)) continue;
      if (path.compare(0, c.path.size(), c.path) != 0 ||
          (path.size() > c.path.size() && c.path.back() != '/' && path[c.path.size()] != '/'))
        continue;
      out += (out.empty() ? "" : "; ") + c.name + "=" + c.value;
    }
    return out;
  }

 private:
  struct C {
    std::string name, value, domain, path;
    bool host_only = true, secure = false;
  };
  std::vector<C> jar_;
};

struct Engines {
  CookieJar cookies;
  HttpClient* http = nullptr;
  std::unique_ptr<HttpClient> http_owned;
  std::shared_ptr<GrpcClient> grpc;
  std::set<std::string> schemas_loaded;  // proto file sets loaded into grpc (joined paths)
};

// After the response: captures, the post-response script, then the asserts (Bruno's order).
// The step's checks end up as: assert rows, then script tests; captures: rows, then script vars.
template <class Resp>
void after_response(const SavedRequest& saved, const SavedRequest& sent, const Resp& resp, Workspace& ws, RunControl& ctl,
                    bool scripts, StepResult& s) {
  ResponseData rd(resp);
  auto cr = apply_captures(saved, rd, ws, false);
  if (scripts && !saved.post_script.empty() && !ctl.stopped()) {
    const auto t = Clock::now();
    ScriptRun post = run_post_script(ws, sent, rd, script_opts(ctl));
    const double post_ms = ms_between(t, Clock::now());
    s.scripts_ran = s.scripts_ran || post.ran;
    if (post.untrusted) {
      if (s.script_tests == 0) untrusted_check(s);
    } else {
      add_script(s, post, true, ws);
    }
    if (post_ms > 0) {
      s.phases.push_back({"Post-response script", post_ms});
      s.script_ms += post_ms;  // not response time (nor is it Newman's)
    }
  }
  std::vector<StepResult::Check> script_checks;
  std::vector<StepResult::Cap> script_caps;
  script_checks.swap(s.asserts);
  script_caps.swap(s.captures);
  const int sp = s.asserts_passed, sf = s.asserts_failed;
  s.asserts_passed = s.asserts_failed = 0;
  // The request's own checks plus the folders' and the collection's (folder.json asserts), each
  // with where it came from: a failing folder check fails the step (and watt --run).
  SavedRequest checks = saved;
  std::vector<std::string> from;
  if (!saved.settings_merged && ws.has_inherited_settings(saved)) {
    const EffectiveSettings eff = ws.effective_settings(saved);
    checks.asserts.clear();
    for (const auto& c : eff.asserts) {
      checks.asserts.push_back(c.assertion);
      from.push_back(c.from.level == SettingsOrigin::Level::Request ? std::string() : c.from.label);
    }
  }
  auto ar = evaluate_asserts(checks, rd, &ws);
  const size_t first = s.asserts.size();
  fill_checks(s, ar, cr);
  for (size_t i = 0; i < ar.size() && first + i < s.asserts.size(); ++i)
    if (ar[i].a) {
      const size_t k = static_cast<size_t>(ar[i].a - checks.asserts.data());
      if (k < from.size()) s.asserts[first + i].from = from[k];
    }
  s.asserts_passed += sp;
  s.asserts_failed += sf;
  for (auto& c : script_checks) s.asserts.push_back(std::move(c));
  for (auto& c : script_caps) s.captures.push_back(std::move(c));
}

// The pre-request script on r (may edit it). False: the step ends here (s.error says why).
bool pre_script(SavedRequest& r, Workspace& ws, RunControl& ctl, bool scripts, StepResult& s, double& ms) {
  if (scripts && !r.pre_script.empty()) {
    const auto t = Clock::now();
    ScriptRun pre = run_pre_script(ws, r, script_opts(ctl));
    ms = ms_between(t, Clock::now());
    s.scripts_ran = pre.ran;
    if (pre.untrusted) {
      untrusted_check(s);
      return true;
    }
    add_script(s, pre, false, ws);
    if (!pre.ok) {
      s.error = ctl.stopped() ? std::string("Stopped") : "Not sent: pre-request script failed: " + pre.error;
      return false;
    }
  } else if (s.has_scripts && !scripts) {
    untrusted_check(s);
  }
  return true;
}

void run_http(const SavedRequest& saved, Workspace& ws, Engines& eng, RunControl& ctl, const RunOptions& opts,
              StepResult& s, Clock::time_point begin, size_t slot) {
  SavedRequest r = saved;
  s.method = r.http.method.empty() ? "GET" : r.http.method;
  s.has_scripts = has_scripts(saved);
  const bool scripts = s.has_scripts && opts.run_scripts;
  // Pre-request variables (Scripts tab rows), then the pre-request script, as the editor sends.
  double pre_vars_ms = 0, pre_ms = 0;
  if (const auto rows = pre_rows(saved); !rows.empty()) {
    const auto t = Clock::now();
    SavedRequest as_sent = r;
    as_sent.http.url = url_with_path_params(r, ws);
    const auto res = http_editor::evaluate_pre_vars(rows, as_sent, ws);
    pre_vars_ms = ms_between(t, Clock::now());
    for (const auto& x : res) {
      // Listed with the variables (runtime: evaluate_pre_vars sets ws.runtime_vars; a row's
      // environment / globals scope is applied by "Keep variable values" after the run).
      StepResult::Cap k;
      k.ok = x.ok;
      k.script = true;
      k.var = rows[x.index].var;
      k.scope = rows[x.index].scope;
      k.value = x.value.size() > kLogKeep && k.scope == Capture::Runtime ? x.value.substr(0, kLogKeep) + "..." : x.value;
      k.message = x.message;
      s.captures.push_back(std::move(k));
    }
    for (const auto& x : res)
      if (!x.ok) {
        s.error = "Not sent: pre-request variable '" + rows[x.index].var + "': " + x.message;
        s.url = ws.expand(as_sent.http.url);
        return;
      }
  }
  r.http.url = url_with_path_params(r, ws);
  if (!pre_script(r, ws, ctl, scripts, s, pre_ms)) {
    s.url = ws.expand(r.http.url);
    return;
  }
  // Never prompts: whatever was not approved up front stays blocked (http_to_send refuses it too).
  if (auto env = ws.blocked_env_vars(r); !env.empty()) {
    s.error = "Not sent: reads environment variable " + join(env) + ", which was not allowed";
    s.url = ws.expand(r.http.url);
    return;
  }
  // After the pre-request script (its variables count) and the consent check (on the URL as written).
  encode_as_sent(r, ws);
  // OAuth 2.0 (own or inherited): the token step, timed as its own phase; http_to_send() below
  // then finds the token in the cache.
  double token_ms = 0;
  if (r.auth.type == HttpAuth::Type::OAuth2 ||
      (r.auth.type == HttpAuth::Type::Inherit && ws.has_inherited_settings(r) &&
       ws.effective_settings(r).auth.type == HttpAuth::Type::OAuth2)) {
    const OAuth2Result t = oauth2_get_token(ws, r);
    token_ms = t.elapsed_ms;
    if (!t.ok) {
      s.error = "Not sent: OAuth 2.0: " + t.error + ": request not sent.";
      s.url = ws.expand(r.http.url);
      return;
    }
  }
  std::string err;
  auto req = ws.http_to_send(r, &err);
  if (!req) {
    s.error = "Not sent: " + (err.empty() ? std::string("request could not be prepared") : err);
    s.url = ws.expand(r.http.url);
    return;
  }
  req->busy_poll_ms = 0;  // a run may take minutes: sleep through server waits, never spin a core
  if (opts.cookies) {
    // Cookies set earlier in this run, after the request's own (a name it sets itself wins).
    if (std::string jar = eng.cookies.header_for(req->url); !jar.empty()) {
      auto own = std::find_if(req->headers.begin(), req->headers.end(), [](const Header& h) { return lower(h.first) == "cookie"; });
      if (own == req->headers.end()) {
        req->headers.emplace_back("Cookie", jar);
      } else {
        std::string merged = own->second;
        for (size_t at = 0; at < jar.size();) {
          size_t end = jar.find("; ", at);
          if (end == std::string::npos) end = jar.size();
          const std::string kv = jar.substr(at, end - at);
          const std::string name = kv.substr(0, kv.find('='));
          if (("; " + merged).find("; " + name + "=") == std::string::npos) merged += (merged.empty() ? "" : "; ") + kv;
          at = end + 2;
        }
        own->second = merged;
      }
    }
  }
  s.url = req->url;
  s.method = req->method;
  s.req_headers = req->headers;
  keep_request_body(s, req->body);
  find_unresolved(s, req->url);
  for (const auto& h : req->headers) find_unresolved(s, h.second);
  find_unresolved(s, req->body);
  if (!eng.http) {
    eng.http_owned = std::make_unique<HttpClient>();
    eng.http = eng.http_owned.get();
  }
  if (!ctl.attach(eng.http, slot)) {
    s.error = "Stopped";
    return;
  }
  const auto start = Clock::now();
  const double setup = ms_between(begin, start);
  HttpResponse resp = eng.http->send(*req);
  ctl.attach(static_cast<HttpClient*>(nullptr), slot);
  s.sent = true;
  resp.timing.queue_ms += setup;  // expansion, files, consent checks: the runner's own setup
  resp.timing.total_ms += setup;
  s.timing = resp.timing;
  http_phases(s, resp);
  script_phase_front(s, "Pre-request script", pre_ms);
  script_phase_front(s, "Pre-request vars", pre_vars_ms);
  setup_phase_front(s, "Auth / token", token_ms);
  setup_phases_out(s);
  s.transport_ok = resp.ok;
  s.status = resp.status;
  s.status_text = resp.status_text;
  s.size = resp.sizes.response_body ? resp.sizes.response_body : resp.body.size();
  if (!resp.ok) s.error = resp.error.empty() ? "transport error" : resp.error;
  if (!resp.effective_url.empty()) s.url = resp.effective_url;
  s.http_version = resp.http_version;
  s.resp_headers = resp.headers.to_vector();
  if (opts.cookies && resp.ok) eng.cookies.take(s.url, s.resp_headers);
  s.bytes_received = resp.timing.bytes_received ? resp.timing.bytes_received
                                                : resp.sizes.response_headers + resp.sizes.response_body_wire;
  keep_preview(s, resp.body);
  if (opts.spill_bodies && s.body_truncated) s.body_file = SpillFile::write(resp.body);
  if (resp.ok) after_response(saved, r, resp, ws, ctl, scripts, s);
}

void run_grpc(const SavedRequest& original, Workspace& ws, Engines& eng, const GrpcFactory& factory, RunControl& ctl,
              const RunOptions& opts, StepResult& s, Clock::time_point begin, size_t slot) {
  s.method = original.grpc_method;
  s.has_scripts = has_scripts(original);
  const bool scripts = s.has_scripts && opts.run_scripts;
  // The pre-request script may change the address, method, metadata and body.
  SavedRequest saved = original;
  double pre_ms = 0;
  if (!pre_script(saved, ws, ctl, scripts, s, pre_ms)) {
    s.url = ws.expand(saved.grpc_target.address);
    return;
  }
  s.url = ws.expand(saved.grpc_target.address);
  if (saved.grpc_method.empty() || saved.grpc_target.address.empty()) {
    s.error = "Not sent: no server address or method";
    return;
  }
  if (auto env = ws.blocked_env_vars(saved); !env.empty()) {
    s.error = "Not sent: reads environment variable " + join(env) + ", which was not allowed";
    return;
  }
  if (!eng.grpc) eng.grpc = factory ? factory() : nullptr;
  if (!eng.grpc) {
    s.error = "Not sent: gRPC engine unavailable";
    return;
  }
  const SavedRequest r = ws.expanded(saved);
  s.method = r.grpc_method;
  s.url = r.grpc_target.address;
  s.req_headers.assign(r.grpc_opts.metadata.begin(), r.grpc_opts.metadata.end());
  s.req_headers.insert(s.req_headers.end(), r.grpc_opts.rpc_metadata.begin(), r.grpc_opts.rpc_metadata.end());
  keep_request_body(s, r.grpc_body);
  find_unresolved(s, r.grpc_target.address);
  for (const auto& h : s.req_headers) find_unresolved(s, h.second);
  find_unresolved(s, r.grpc_body);
  if (!saved.proto_files.empty()) {
    // Same gate as the request tab's schema load: only root-anchored, approved paths are read.
    Workspace::SchemaPlan plan = ws.schema_plan(saved);
    if (!plan.invalid.empty()) {
      s.error = "Schema not loaded: " + plan.invalid.front();
      s.client_error = true;
      return;
    }
    if (!plan.blocked.empty()) {
      s.error = "Schema not loaded: " + generic_utf8(plan.blocked.front()) + " is outside the workspace and was not allowed";
      s.client_error = true;
      return;
    }
    std::string key;
    for (const auto& p : plan.proto_files) key += p + "\n";
    key += "|";
    for (const auto& p : plan.import_dirs) key += p + "\n";
    if (!eng.schemas_loaded.count(key)) {
      GrpcSchema sc = eng.grpc->load_proto_files(plan.proto_files, plan.import_dirs);
      if (!sc.ok) {
        s.error = "Schema not loaded: " + sc.error;
        s.client_error = true;
        return;
      }
      eng.schemas_loaded.insert(key);
    }
  }
  if (ctl.stopped()) {
    s.error = "Stopped";
    return;
  }
  const auto start = Clock::now();
  const double setup = ms_between(begin, start);
  std::shared_ptr<GrpcCall> call(eng.grpc->start_call(r.grpc_target, r.grpc_method, r.grpc_opts));
  if (!ctl.attach(call, slot)) call->cancel();
  bool encode_failed = false;
  if (!call->done()) {
    for (const auto& m : split_messages(r.grpc_body)) {
      std::string err;
      if (!call->send(m, &err)) {
        if (!call->done()) {
          call->cancel();
          s.error = "Request message: " + one_space(err);
          encode_failed = true;
        }
        break;
      }
    }
    call->close_send();
  }
  GrpcResponse resp = call->wait();
  ctl.attach(std::shared_ptr<GrpcCall>(), slot);
  if (encode_failed && !ctl.stopped()) {
    // Nothing usable went out: an error, never a CANCELLED response that inherited asserts pass.
    s.client_error = true;
    return;
  }
  s.sent = true;
  resp.timing.queue_ms += setup;
  resp.timing.total_ms += setup;
  s.timing = resp.timing;
  grpc_phases(s, resp);
  script_phase_front(s, "Pre-request script", pre_ms);
  setup_phases_out(s);
  s.transport_ok = resp.ok;
  s.status = resp.status_code;
  s.status_text = resp.status_name;
  if (!resp.ok && s.error.empty()) s.error = resp.status_message.empty() ? resp.status_name : resp.status_message;
  uint64_t bytes = 0;
  for (const auto& m : resp.messages) bytes += m.wire_bytes;
  s.size = bytes;
  s.bytes_received = resp.timing.bytes_received ? resp.timing.bytes_received : bytes;
  s.resp_headers.assign(resp.headers.begin(), resp.headers.end());
  s.resp_trailers.assign(resp.trailers.begin(), resp.trailers.end());
  if (!resp.ok) s.grpc_details = resp.error_details;
  if (resp.messages.size() == 1) {
    keep_preview(s, resp.messages.front().json);
  } else if (!resp.messages.empty()) {
    // A stream: the messages as one JSON array (as the request tab copies them).
    std::string all = "[";
    for (const auto& m : resp.messages) {
      if (all.size() > kBodyKeep) break;
      if (all.size() > 1) all += ",\n";
      all += m.json;
    }
    all += "]";
    keep_preview(s, all);
  }
  after_response(original, saved, resp, ws, ctl, scripts, s);
}

}  // namespace

namespace {

struct Shared {
  const std::vector<RunItem>& items;
  const std::vector<size_t>& order;
  const RunOptions& opts;
  const GrpcFactory& grpc;
  RunControl& ctl;
  const StepFn& on_step;
  bool persist_captures;
  Clock::time_point t0;
  int iterations;
  std::mutex m;                       // done, seq, budget, bail
  std::vector<std::shared_ptr<const StepResult>> done;
  size_t seq = 0;
  size_t passing_kept = 0;            // passing steps that kept a full body
  size_t budget = kBodyBudget;        // body bytes beyond the first kBodyPreview of each body
  size_t small_budget = kPreviewBudget;  // the first kBodyPreview bytes of each body
  size_t log_budget = kLogBudget;        // script console bytes
  std::atomic<bool> bail{false};
  std::atomic<int> next_iteration{0};
  // Data rows shared by the steps that used them (made on first use).
  std::mutex rows_m;
  std::vector<std::shared_ptr<const DataRow>> rows;
  std::shared_ptr<const DataRow> row(size_t i) {
    std::lock_guard lk(rows_m);
    if (rows.size() < opts.data.size()) rows.resize(opts.data.size());
    if (i >= rows.size()) return nullptr;
    if (!rows[i]) rows[i] = std::make_shared<const DataRow>(opts.data[i]);
    return rows[i];
  }
};

// Runs whole iterations taken from sh.next_iteration until none are left or the run ends.
void worker(Shared& sh, Workspace& ws, size_t slot, size_t workers) {
  Engines eng;
  bool first = true;
  const LoadWindow win = load_window(sh.opts, slot, workers);
  if (win.start_ms > 0) {
    // Ramp-up / spike: this worker joins later.
    const int wait = static_cast<int>(win.start_ms - ms_between(sh.t0, Clock::now()));
    if (wait > 0 && !sh.ctl.sleep_ms(wait)) return;
  }
  for (;;) {
    if (win.stop_ms > 0 && ms_between(sh.t0, Clock::now()) >= win.stop_ms) break;
    const int next = sh.next_iteration.fetch_add(1);
    if (next >= sh.iterations || sh.bail.load() || sh.ctl.stopped()) break;
    const RunOptions::Retry* retry = sh.opts.retry.empty() ? nullptr : &sh.opts.retry[static_cast<size_t>(next)];
    const int it = retry ? retry->iteration : next;  // a rerun keeps the iteration's own number (and data row)
    int data_index = -1;
    if (!sh.opts.data.empty()) {
      data_index = static_cast<int>(std::min(static_cast<size_t>(it), sh.opts.data.size() - 1));
      const DataRow& row = sh.opts.data[static_cast<size_t>(data_index)];
      for (const auto& [k, v] : row) ws.runtime_vars[k] = v;
    }
    for (size_t k = 0; k < sh.order.size(); ++k) {
      if (retry && !std::binary_search(retry->positions.begin(), retry->positions.end(), k)) continue;
      if (sh.bail.load()) break;
      if (!first && sh.opts.delay_ms > 0 && !sh.ctl.sleep_ms(sh.opts.delay_ms)) break;
      if (sh.ctl.stopped()) break;
      const size_t idx = sh.order[k];
      if (idx >= sh.items.size()) continue;
      first = false;
      const RunItem& item = sh.items[idx];
      auto s = std::make_shared<StepResult>();
      s->iteration = it;
      s->item = idx;
      s->pos = k;
      s->kind = item.req.kind;
      s->name = item.req.name;
      s->rel = item.rel;
      if (data_index >= 0) {
        s->data_index = data_index;
        s->data_row = sh.row(static_cast<size_t>(data_index));
      }
      const auto begin = Clock::now();
      s->started_ms = ms_between(sh.t0, begin);
      if (item.req.kind == RequestKind::Http)
        run_http(item.req, ws, eng, sh.ctl, sh.opts, *s, begin, slot);
      else
        run_grpc(item.req, ws, eng, sh.grpc, sh.ctl, sh.opts, *s, begin, slot);
      if (sh.ctl.stopped() && s->sent && !s->transport_ok) s->error = "Stopped";
      if (sh.persist_captures && std::any_of(s->captures.begin(), s->captures.end(), [](const StepResult::Cap& c) {
            return c.ok && c.scope != Capture::Runtime;
          }))
        ws.save_settings();
      const bool failed = s->failed();
      std::shared_ptr<const StepResult> cs = s;
      std::lock_guard lk(sh.m);
      s->seq = sh.seq++;
      // Bodies: failed steps keep up to kBodyKeep, passing ones too among the first
      // kKeepPreviews, later passing ones kBodyPreview; all of it within the run's budget, so
      // 100k results stay within ~100 MB.
      const size_t cap = failed || sh.passing_kept < kKeepPreviews ? kBodyKeep : kBodyPreview;
      if (!failed && s->body_preview.size() > kBodyPreview) ++sh.passing_kept;
      for (auto [b, cut] : {std::pair<std::string*, bool*>{&s->body_preview, &s->body_truncated},
                            {&s->req_body, &s->req_body_truncated}}) {
        size_t keep = std::min(cap, b->size());
        if (keep > kBodyPreview) {
          if (keep - kBodyPreview > sh.budget) keep = kBodyPreview;
          else sh.budget -= keep - kBodyPreview;
        }
        const size_t head = std::min(keep, kBodyPreview);
        if (head > sh.small_budget) keep = 0;
        else sh.small_budget -= head;
        cut_utf8(*b, keep, *cut);
      }
      if (!s->logs.empty()) {
        // Console output: at most kLogKeep bytes per step, kLogBudget per run.
        size_t used = 0, keep = 0;
        const size_t cap_logs = std::min(kLogKeep, sh.log_budget);
        for (; keep < s->logs.size(); ++keep) {
          const size_t n = s->logs[keep].text.size() + 1;
          if (used + n > cap_logs) break;
          used += n;
        }
        if (keep < s->logs.size()) {
          s->logs.resize(keep);
          s->logs_truncated = true;
        }
        sh.log_budget -= std::min(used, sh.log_budget);
      }
      if (sh.opts.stop_on_failure && failed) sh.bail.store(true);
      sh.done.push_back(cs);
      if (sh.on_step) sh.on_step(cs);
    }
  }
}

}  // namespace

LoadWindow load_window(const RunOptions& o, size_t slot, size_t workers) {
  LoadWindow w;
  const double d = o.duration_ms > 0 ? static_cast<double>(o.duration_ms) : 0;
  w.stop_ms = d;
  const double share = workers ? static_cast<double>(slot) / static_cast<double>(workers) : 0;
  if (o.spike_base > 0 && d > 0) {
    // Spike: the first spike_base workers carry the base load all through; the others all join
    // at once for the middle fifth of the run, then leave.
    if (slot >= static_cast<size_t>(o.spike_base)) {
      w.start_ms = d * 0.4;
      w.stop_ms = d * 0.6;
    }
    return w;
  }
  if (o.ramp_up_ms > 0 && slot > 0) w.start_ms = static_cast<double>(o.ramp_up_ms) * share;
  if (o.ramp_down_ms > 0 && d > 0 && slot > 0) {
    // Peak: the load falls back the way it came, the last worker to join leaving first.
    w.stop_ms = std::max(w.start_ms, d - static_cast<double>(o.ramp_down_ms) * share);
  }
  return w;
}

RunSummary run_plan(const std::vector<RunItem>& items, const std::vector<size_t>& order, const RunOptions& opts,
                    Workspace& ws, const GrpcFactory& grpc, RunControl& ctl, const StepFn& on_step,
                    bool persist_captures) {
  const int iterations = !opts.retry.empty() ? static_cast<int>(opts.retry.size())
                         : opts.duration_ms > 0 && opts.iterations <= 1 ? kMaxIterations
                                                                         : std::max(1, opts.iterations);
  if (!opts.retry.empty() && opts.duration_ms > 0) {
    RunOptions plain = opts;
    plain.duration_ms = 0;  // a rerun sends each listed step once
    return run_plan(items, order, plain, ws, grpc, ctl, on_step, persist_captures);
  }
  Shared sh{items, order, opts, grpc, ctl, on_step, persist_captures, Clock::now(), iterations, {}, {}};
  // A duration run plans no fixed number of steps.
  const size_t planned = planned_sends(opts, order.size());
  sh.done.reserve(std::min<size_t>(planned, 1 << 16));
  const int n = std::clamp(opts.concurrency, 1, std::min(sh.iterations, static_cast<int>(RunControl::kSlots)));
  const size_t workers = static_cast<size_t>(n);
  if (n == 1) {
    worker(sh, ws, 0, workers);
  } else {
    // Each worker gets its own copy of the workspace (captures stay within its iterations); the
    // caller's copy ends up with what worker 0 captured. Settings are never persisted from here.
    std::vector<Workspace> copies(static_cast<size_t>(n - 1), ws);
    sh.persist_captures = false;
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(n - 1));
    for (int w = 1; w < n; ++w)
      threads.emplace_back([&sh, &copies, w, workers] {
        worker(sh, copies[static_cast<size_t>(w - 1)], static_cast<size_t>(w), workers);
      });
    worker(sh, ws, 0, workers);
    for (auto& t : threads) t.join();
  }
  return summarize(sh.done, planned, ms_between(sh.t0, Clock::now()), ctl.stopped() || sh.bail.load());
}

SpillFile::~SpillFile() {
  std::error_code ec;
  fs::remove(path_, ec);
  g_spill_bytes.fetch_sub(size_);
}

uint64_t SpillFile::live_bytes() { return g_spill_bytes.load(); }

std::shared_ptr<const SpillFile> SpillFile::write(std::string_view body) {
  const uint64_t n = body.size();
  if (g_spill_bytes.fetch_add(n) + n > kSpillBudget) {
    g_spill_bytes.fetch_sub(n);
    return nullptr;
  }
  const fs::path dir = spill_dir();
  std::error_code ec;
  fs::create_directories(dir, ec);
  fs::path p = dir / ("body-" + std::to_string(g_spill_seq.fetch_add(1)) + ".bin");
  std::shared_ptr<const SpillFile> f(new SpillFile(p, n));  // from here on, the destructor cleans up
  std::ofstream o(p, std::ios::binary | std::ios::trunc);
  if (!o) return nullptr;
  o.write(body.data(), static_cast<std::streamsize>(body.size()));
  o.close();
  if (!o) return nullptr;
  return f;
}

bool SpillFile::read(std::string* out) const {
  std::ifstream in(path_, std::ios::binary);
  if (!in) return false;
  out->resize(static_cast<size_t>(size_));
  in.read(out->data(), static_cast<std::streamsize>(size_));
  return static_cast<uint64_t>(in.gcount()) == size_;
}

size_t planned_sends(const RunOptions& o, size_t order_size) {
  if (!o.retry.empty()) {
    size_t n = 0;
    for (const auto& r : o.retry) n += r.positions.size();
    return n;
  }
  if (o.duration_ms > 0) return 0;
  const int it = std::max(1, o.iterations);
  return order_size * static_cast<size_t>(it);
}

std::vector<std::shared_ptr<const StepResult>> skipped_steps(const std::vector<RunItem>& items,
                                                             const std::vector<size_t>& order, int iterations,
                                                             const std::vector<bool>& done, size_t first_seq,
                                                             size_t max) {
  std::vector<std::shared_ptr<const StepResult>> out;
  size_t seq = first_seq;
  for (int it = 0; it < iterations && out.size() < max; ++it)
    for (size_t k = 0; k < order.size() && out.size() < max; ++k) {
      const size_t p = static_cast<size_t>(it) * order.size() + k;
      if (p < done.size() && done[p]) continue;
      if (order[k] >= items.size()) continue;
      const RunItem& item = items[order[k]];
      auto s = std::make_shared<StepResult>();
      s->seq = seq++;
      s->iteration = it;
      s->item = order[k];
      s->pos = k;
      s->kind = item.req.kind;
      s->name = item.req.name;
      s->rel = item.rel;
      s->method = item.req.kind == RequestKind::Http ? (item.req.http.method.empty() ? "GET" : item.req.http.method)
                                                      : item.req.grpc_method;
      s->url = item.req.kind == RequestKind::Http ? item.req.http.url : item.req.grpc_target.address;
      s->skipped = true;
      s->error = "Skipped: the run ended before this request";
      out.push_back(std::move(s));
    }
  return out;
}

// ------------------------------------------------------------------ tags

std::vector<std::string> tags_of(const RunItem& it) {
  std::vector<std::string> t;
  auto add = [&](std::string_view w) {
    std::string l = lower(w);
    if (!l.empty() && std::find(t.begin(), t.end(), l) == t.end()) t.push_back(std::move(l));
  };
  const std::string& d = it.req.docs;
  for (size_t i = 0; i < d.size(); ++i) {
    if (d[i] != '#' || (i > 0 && !std::isspace(static_cast<unsigned char>(d[i - 1])) && d[i - 1] != ',')) continue;
    size_t j = i + 1;
    while (j < d.size() && (std::isalnum(static_cast<unsigned char>(d[j])) || d[j] == '-' || d[j] == '_' || d[j] == '.'))
      ++j;
    if (j > i + 1) add(std::string_view(d).substr(i + 1, j - i - 1));
    i = j;
  }
  std::string_view rel = it.rel;
  for (size_t at = rel.find('/'); at != std::string_view::npos; at = rel.find('/')) {
    add(rel.substr(0, at));
    rel.remove_prefix(at + 1);
  }
  return t;
}

bool tag_filter_match(const RunItem& it, const std::string& filter) {
  const auto tags = tags_of(it);
  const std::string hay = lower(it.req.name + "\n" + it.rel);
  size_t i = 0;
  while (i < filter.size()) {
    while (i < filter.size() && std::isspace(static_cast<unsigned char>(filter[i]))) ++i;
    size_t j = i;
    while (j < filter.size() && !std::isspace(static_cast<unsigned char>(filter[j]))) ++j;
    std::string w = lower(std::string_view(filter).substr(i, j - i));
    i = j;
    if (w.empty()) continue;
    bool neg = false;
    if (w[0] == '-' || w[0] == '!') {
      neg = true;
      w.erase(0, 1);
    } else if (w[0] == '+') {
      w.erase(0, 1);
    }
    if (!w.empty() && w[0] == '#') w.erase(0, 1);
    if (w.empty()) continue;
    const bool hit = std::find(tags.begin(), tags.end(), w) != tags.end() || hay.find(w) != std::string::npos;
    if (hit == neg) return false;
  }
  return true;
}

// ------------------------------------------------------------------ data files

bool parse_data(std::string_view text, bool json, std::vector<DataRow>* rows, std::string* err) {
  rows->clear();
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
      static_cast<unsigned char>(text[2]) == 0xBF)
    text.remove_prefix(3);
  if (json) {
    rapidjson::Document d;
    d.Parse(text.data(), text.size());
    if (d.HasParseError() || !d.IsArray()) {
      if (err) *err = "a JSON data file must be an array of objects";
      return false;
    }
    for (auto& v : d.GetArray()) {
      if (!v.IsObject()) {
        if (err) *err = "every element of the JSON array must be an object";
        return false;
      }
      DataRow r;
      for (auto m = v.MemberBegin(); m != v.MemberEnd(); ++m) {
        std::string val;
        if (m->value.IsString()) {
          val.assign(m->value.GetString(), m->value.GetStringLength());
        } else if (!m->value.IsNull()) {
          rapidjson::StringBuffer sb;
          rapidjson::Writer<rapidjson::StringBuffer> w(sb);
          m->value.Accept(w);
          val.assign(sb.GetString(), sb.GetSize());
        }
        r.emplace_back(std::string(m->name.GetString(), m->name.GetStringLength()), std::move(val));
      }
      rows->push_back(std::move(r));
    }
    return true;
  }
  // CSV (RFC 4180): comma separated (';' or tab when the header has none), "" quotes.
  std::vector<std::vector<std::string>> recs;
  {
    const size_t eol = text.find('\n');
    const std::string_view head = text.substr(0, eol);
    char sep = ',';
    if (head.find(',') == std::string_view::npos) {
      if (head.find(';') != std::string_view::npos) sep = ';';
      else if (head.find('\t') != std::string_view::npos) sep = '\t';
    }
    std::vector<std::string> rec;
    std::string cell;
    bool q = false, any = false;
    for (size_t i = 0; i < text.size(); ++i) {
      const char c = text[i];
      if (q) {
        if (c == '"') {
          if (i + 1 < text.size() && text[i + 1] == '"') {
            cell += '"';
            ++i;
          } else {
            q = false;
          }
        } else {
          cell += c;
        }
        continue;
      }
      if (c == '"' && cell.empty()) {
        q = any = true;
      } else if (c == sep) {
        rec.push_back(std::move(cell));
        cell.clear();
        any = true;
      } else if (c == '\n' || c == '\r') {
        if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
        if (any || !cell.empty()) {
          rec.push_back(std::move(cell));
          recs.push_back(std::move(rec));
        }
        rec.clear();
        cell.clear();
        any = false;
      } else {
        cell += c;
        any = true;
      }
    }
    if (q) {
      if (err) *err = "unterminated quoted field";
      return false;
    }
    if (any || !cell.empty()) {
      rec.push_back(std::move(cell));
      recs.push_back(std::move(rec));
    }
  }
  if (recs.empty()) {
    if (err) *err = "the data file is empty";
    return false;
  }
  const auto& head = recs.front();
  for (size_t r = 1; r < recs.size(); ++r) {
    DataRow row;
    for (size_t c = 0; c < head.size(); ++c) {
      std::string name(trim_ws(head[c]));
      if (name.empty()) continue;
      row.emplace_back(std::move(name), c < recs[r].size() ? recs[r][c] : std::string());
    }
    rows->push_back(std::move(row));
  }
  if (rows->empty()) {
    if (err) *err = "the data file has a header row but no data rows";
    return false;
  }
  return true;
}

bool load_data_file(const fs::path& path, std::vector<DataRow>* rows, std::string* err) {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  if (ec) {
    if (err) *err = "cannot read " + generic_utf8(path.filename());
    return false;
  }
  if (size > (64u << 20)) {
    if (err) *err = "the data file is larger than 64 MB";
    return false;
  }
  std::ifstream f(path, std::ios::binary);
  std::string text(static_cast<size_t>(size), '\0');
  f.read(text.data(), static_cast<std::streamsize>(text.size()));
  if (!f) {
    if (err) *err = "cannot read " + generic_utf8(path.filename());
    return false;
  }
  return parse_data(text, lower(generic_utf8(path.extension())) == ".json", rows, err);
}

// ------------------------------------------------------------------ export

std::string now_iso() {
  const auto t = std::chrono::system_clock::now();
  const std::time_t tt = std::chrono::system_clock::to_time_t(t);
  std::tm tm{};
  gmtime_s(&tm, &tt);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

namespace {
using JW = rapidjson::Writer<rapidjson::StringBuffer>;
void kv(JW& w, const char* k, const std::string& v) {
  w.Key(k);
  w.String(v.data(), static_cast<rapidjson::SizeType>(v.size()));
}
void kv(JW& w, const char* k, double v) {
  w.Key(k);
  w.Double(std::round(v * 1000.0) / 1000.0);
}
void kv_u(JW& w, const char* k, uint64_t v) {
  w.Key(k);
  w.Uint64(v);
}
void kv_b(JW& w, const char* k, bool v) {
  w.Key(k);
  w.Bool(v);
}
}  // namespace

namespace {
// history: a run history file (history_json): a big run keeps its failed steps only, and
// credential-like variable values are masked.
std::string report_json(const RunMeta& meta, const RunSummary& s, const std::vector<std::shared_ptr<const StepResult>>& steps,
                        bool history) {
  rapidjson::StringBuffer sb;
  JW w(sb);
  w.StartObject();
  kv(w, "tool", std::string("watt"));
  kv(w, "collection", meta.name);
  kv(w, "environment", meta.environment);
  kv(w, "started", meta.started_iso);
  w.Key("iterations");
  w.Int(meta.iterations);
  if (!meta.label.empty()) kv(w, "label", meta.label);
  if (!meta.plan.empty()) kv(w, "plan", meta.plan);
  const bool partial = history && steps.size() > kHistorySteps;
  if (partial) kv_b(w, "partial", true);
  w.Key("summary");
  w.StartObject();
  kv_u(w, "total", s.total);
  kv_u(w, "planned", s.planned);
  kv_u(w, "passed", s.passed);
  kv_u(w, "failed", s.failed);
  kv_u(w, "errors", s.errors);
  kv_u(w, "assertsPassed", s.asserts_passed);
  kv_u(w, "assertsFailed", s.asserts_failed);
  kv(w, "wallMs", s.wall_ms);
  kv(w, "sumMs", s.sum_ms);
  kv(w, "avgMs", s.avg_ms);
  kv(w, "p50Ms", s.p50_ms);
  kv(w, "p95Ms", s.p95_ms);
  kv(w, "p90Ms", s.p90_ms);
  kv(w, "p99Ms", s.p99_ms);
  kv_u(w, "samples", s.samples);
  kv(w, "minMs", s.min_ms);
  kv(w, "maxMs", s.max_ms);
  kv(w, "stdDevMs", s.stddev_ms);
  kv(w, "avgDnsMs", s.avg_dns_ms);
  kv(w, "avgConnectMs", s.avg_connect_ms);
  kv(w, "avgTlsMs", s.avg_tls_ms);
  kv(w, "avgTtfbMs", s.avg_ttfb_ms);
  kv(w, "scriptSumMs", s.script_sum_ms);  // outside every time above (Newman leaves scripts out too)
  kv(w, "avgScriptMs", s.avg_script_ms);
  kv(w, "maxScriptMs", s.max_script_ms);
  kv_u(w, "scripted", s.scripted);
  kv(w, "avgPrepMs", s.avg_prep_ms);  // setup before the first I/O, outside every time above
  kv_u(w, "newConnections", s.new_connections);
  kv_u(w, "bytesReceived", s.bytes_received);
  kv_u(w, "stoppedSteps", s.stopped_steps);
  kv_u(w, "skipped", s.skipped);
  kv_b(w, "stopped", s.stopped);
  w.EndObject();
  {
    const RunSummary per = summarize(steps, s.planned, s.wall_ms, s.stopped, true);
    w.Key("requests");
    w.StartArray();
    for (const auto& q : per.per_request) {
      w.StartObject();
      kv(w, "name", q.name);
      kv(w, "path", q.rel);
      kv(w, "method", q.method);
      kv(w, "kind", std::string(q.kind == RequestKind::Http ? "http" : "grpc"));
      kv_u(w, "count", q.count);
      kv_u(w, "failures", q.failures);
      kv(w, "avgMs", q.avg_ms);
      kv(w, "minMs", q.min_ms);
      kv(w, "p50Ms", q.p50_ms);
      kv(w, "p95Ms", q.p95_ms);
      kv(w, "maxMs", q.max_ms);
      w.EndObject();
    }
    w.EndArray();
  }
  w.Key("results");
  w.StartArray();
  size_t kept = 0;
  for (const auto& p : steps) {
    const StepResult& r = *p;
    if (partial && (!r.failed() || kept >= kHistorySteps)) continue;
    ++kept;
    w.StartObject();
    kv_u(w, "seq", r.seq + 1);
    w.Key("iteration");
    w.Int(r.iteration + 1);
    kv(w, "name", r.name);
    kv(w, "path", r.rel);
    kv(w, "kind", std::string(r.kind == RequestKind::Http ? "http" : "grpc"));
    kv(w, "method", r.method);
    kv(w, "url", r.url);
    kv_b(w, "sent", r.sent);
    kv_b(w, "passed", r.passed());
    if (r.stopped()) kv_b(w, "stopped", true);
    if (r.skipped) kv_b(w, "skipped", true);
    w.Key("status");
    w.Int64(r.status);
    kv(w, "statusText", r.status_text);
    if (!r.error.empty()) kv(w, "error", r.error);
    if (r.transport_ok) kv_b(w, "transportOk", true);
    if (r.client_error) kv_b(w, "clientError", true);
    if (!r.http_version.empty()) kv(w, "httpVersion", r.http_version);
    kv_u(w, "size", r.size);
    kv_u(w, "bytesReceived", r.bytes_received);
    kv(w, "startedMs", r.started_ms);
    if (r.data_index >= 0) {
      w.Key("dataIndex");
      w.Int(r.data_index);
      if (r.data_row) {
        w.Key("data");
        w.StartArray();
        for (const auto& [k, v] : *r.data_row) {
          w.StartArray();
          w.String(k.data(), static_cast<rapidjson::SizeType>(k.size()));
          w.String(v.data(), static_cast<rapidjson::SizeType>(v.size()));
          w.EndArray();
        }
        w.EndArray();
      }
    }
    if (!r.unresolved.empty()) {
      w.Key("unresolved");
      w.StartArray();
      for (const auto& n : r.unresolved) w.String(n.data(), static_cast<rapidjson::SizeType>(n.size()));
      w.EndArray();
    }
    w.Key("timing");
    w.StartObject();
    kv(w, "totalMs", r.timing.total_ms);  // response time, without scripts
    kv(w, "scriptMs", r.script_ms);
    kv(w, "prepMs", r.prep_ms);  // setup and client queue, apart from totalMs
    kv_b(w, "connectionReused", r.timing.connection_reused);
    kv(w, "dnsMs", r.timing.dns_ms);
    kv(w, "connectMs", r.timing.connect_ms);
    kv(w, "tlsMs", r.timing.tls_ms);
    kv(w, "ttfbMs", r.timing.ttfb_ms);
    w.Key("phases");
    w.StartArray();
    for (const auto& ph : r.phases) {
      w.StartObject();
      kv(w, "name", std::string(ph.name));
      kv(w, "ms", ph.ms);
      w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    w.Key("asserts");
    w.StartArray();
    for (const auto& a : r.asserts) {
      w.StartObject();
      kv(w, "target", a.target);
      kv(w, "op", a.op);
      kv(w, "expected", a.expected);
      kv(w, "actual", a.actual);
      kv_b(w, "passed", a.pass);
      if (a.script) kv_b(w, "script", true);
      if (!a.message.empty()) kv(w, "message", a.message);
      if (!a.from.empty()) kv(w, "from", a.from);
      w.EndObject();
    }
    w.EndArray();
    if (!r.logs.empty()) {
      static const char* const levels[] = {"log", "info", "warn", "error"};
      w.Key("console");
      w.StartArray();
      for (const auto& l : r.logs) {
        w.StartObject();
        kv(w, "script", std::string(l.post ? "post-response" : "pre-request"));
        kv(w, "level", std::string(levels[static_cast<int>(l.level) & 3]));
        kv(w, "text", l.text);
        w.EndObject();
      }
      w.EndArray();
    }
    w.Key("captures");
    w.StartArray();
    for (const auto& c : r.captures) {
      w.StartObject();
      kv(w, "var", c.var);
      kv(w, "scope", std::string(capture_scope_name(c.scope)));
      kv_b(w, "ok", c.ok);
      kv(w, "value", history && !c.value.empty() && sensitive_header(c.var) ? std::string("\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2 (not kept in history)") : c.value);
      if (!c.message.empty()) kv(w, "message", c.message);
      if (c.script) kv_b(w, "script", true);
      if (c.removed) kv_b(w, "removed", true);
      w.EndObject();
    }
    w.EndArray();
    w.EndObject();
  }
  w.EndArray();
  w.EndObject();
  return std::string(sb.GetString(), sb.GetSize());
}
}  // namespace

std::string to_json(const RunMeta& meta, const RunSummary& s, const std::vector<std::shared_ptr<const StepResult>>& steps) {
  return report_json(meta, s, steps, false);
}

std::string history_json(const RunMeta& meta, const RunSummary& s, const std::vector<std::shared_ptr<const StepResult>>& steps) {
  return report_json(meta, s, steps, true);
}

namespace {
// Phase::name is a static string: names read back from a file are kept once for the process.
const char* intern_phase(const std::string& n) {
  static std::mutex m;
  static std::set<std::string> names;
  std::lock_guard lk(m);
  return names.insert(n).first->c_str();
}
std::string jstr(const rapidjson::Value& o, const char* k) {
  if (!o.IsObject()) return {};
  const auto it = o.FindMember(k);
  return it != o.MemberEnd() && it->value.IsString() ? std::string(it->value.GetString(), it->value.GetStringLength())
                                                      : std::string();
}
double jnum(const rapidjson::Value& o, const char* k) {
  if (!o.IsObject()) return 0;
  const auto it = o.FindMember(k);
  return it != o.MemberEnd() && it->value.IsNumber() ? it->value.GetDouble() : 0.0;
}
uint64_t juint(const rapidjson::Value& o, const char* k) {
  if (!o.IsObject()) return 0;
  const auto it = o.FindMember(k);
  return it != o.MemberEnd() && it->value.IsUint64() ? it->value.GetUint64() : 0;
}
bool jbool(const rapidjson::Value& o, const char* k) {
  if (!o.IsObject()) return false;
  const auto it = o.FindMember(k);
  return it != o.MemberEnd() && it->value.IsBool() && it->value.GetBool();
}
const rapidjson::Value* jarr(const rapidjson::Value& o, const char* k) {
  if (!o.IsObject()) return nullptr;
  const auto it = o.FindMember(k);
  return it != o.MemberEnd() && it->value.IsArray() ? &it->value : nullptr;
}
}  // namespace

bool from_json(const std::string& text, LoadedRun& out, std::string* err) {
  rapidjson::Document d;
  d.Parse(text.data(), text.size());
  if (d.HasParseError() || !d.IsObject() || jstr(d, "tool") != "watt") {
    if (err) *err = "not a watt run report";
    return false;
  }
  out = LoadedRun();
  out.meta.name = jstr(d, "collection");
  out.meta.environment = jstr(d, "environment");
  out.meta.started_iso = jstr(d, "started");
  out.meta.iterations = std::max(1, static_cast<int>(jnum(d, "iterations")));
  out.meta.label = jstr(d, "label");
  out.meta.plan = jstr(d, "plan");
  out.partial = jbool(d, "partial");
  RunSummary& s = out.summary;
  if (const auto it = d.FindMember("summary"); it != d.MemberEnd() && it->value.IsObject()) {
    const auto& v = it->value;
    s.total = juint(v, "total");
    s.planned = juint(v, "planned");
    s.passed = juint(v, "passed");
    s.failed = juint(v, "failed");
    s.errors = juint(v, "errors");
    s.asserts_passed = juint(v, "assertsPassed");
    s.asserts_failed = juint(v, "assertsFailed");
    s.wall_ms = jnum(v, "wallMs");
    s.sum_ms = jnum(v, "sumMs");
    s.avg_ms = jnum(v, "avgMs");
    s.p50_ms = jnum(v, "p50Ms");
    s.p90_ms = jnum(v, "p90Ms");
    s.p95_ms = jnum(v, "p95Ms");
    s.p99_ms = jnum(v, "p99Ms");
    s.samples = juint(v, "samples");
    s.min_ms = jnum(v, "minMs");
    s.max_ms = jnum(v, "maxMs");
    s.stddev_ms = jnum(v, "stdDevMs");
    s.avg_dns_ms = jnum(v, "avgDnsMs");
    s.avg_connect_ms = jnum(v, "avgConnectMs");
    s.avg_tls_ms = jnum(v, "avgTlsMs");
    s.avg_ttfb_ms = jnum(v, "avgTtfbMs");
    s.script_sum_ms = jnum(v, "scriptSumMs");
    s.avg_script_ms = jnum(v, "avgScriptMs");
    s.max_script_ms = jnum(v, "maxScriptMs");
    s.scripted = juint(v, "scripted");
    s.avg_prep_ms = jnum(v, "avgPrepMs");
    s.new_connections = juint(v, "newConnections");
    s.bytes_received = juint(v, "bytesReceived");
    s.stopped_steps = juint(v, "stoppedSteps");
    s.skipped = juint(v, "skipped");
    s.stopped = jbool(v, "stopped");
  }
  std::map<std::string, size_t> items;  // path -> item (first-appearance order)
  auto item_of = [&](const std::string& rel) { return items.try_emplace(rel, items.size()).first->second; };
  if (const auto* a = jarr(d, "requests"))
    for (const auto& q : a->GetArray()) {
      RequestStats r;
      r.name = jstr(q, "name");
      r.rel = jstr(q, "path");
      r.item = item_of(r.rel);
      r.method = jstr(q, "method");
      r.kind = jstr(q, "kind") == "grpc" ? RequestKind::Grpc : RequestKind::Http;
      r.count = juint(q, "count");
      r.failures = juint(q, "failures");
      r.avg_ms = jnum(q, "avgMs");
      r.min_ms = jnum(q, "minMs");
      r.p50_ms = jnum(q, "p50Ms");
      r.p95_ms = jnum(q, "p95Ms");
      r.max_ms = jnum(q, "maxMs");
      s.per_request.push_back(std::move(r));
    }
  if (const auto* a = jarr(d, "results")) {
    out.steps.reserve(a->Size());
    std::map<size_t, size_t> pos;  // item -> its place in an iteration (first-seen order)
    for (const auto& v : a->GetArray()) {
      if (!v.IsObject()) continue;
      auto r = std::make_shared<StepResult>();
      r->seq = static_cast<size_t>(std::max<uint64_t>(juint(v, "seq"), 1) - 1);
      r->iteration = std::max(0, static_cast<int>(jnum(v, "iteration")) - 1);
      r->name = jstr(v, "name");
      r->rel = jstr(v, "path");
      r->item = item_of(r->rel);
      r->pos = pos.try_emplace(r->item, pos.size()).first->second;
      r->kind = jstr(v, "kind") == "grpc" ? RequestKind::Grpc : RequestKind::Http;
      r->method = jstr(v, "method");
      r->url = jstr(v, "url");
      r->sent = jbool(v, "sent");
      r->skipped = jbool(v, "skipped");
      r->transport_ok = jbool(v, "transportOk");
      r->client_error = jbool(v, "clientError");
      if (const auto it = v.FindMember("status"); it != v.MemberEnd() && it->value.IsInt64()) r->status = static_cast<long>(it->value.GetInt64());
      r->status_text = jstr(v, "statusText");
      r->error = jstr(v, "error");
      if (jbool(v, "stopped") && r->error.empty()) r->error = "Stopped";
      r->http_version = jstr(v, "httpVersion");
      r->size = juint(v, "size");
      r->bytes_received = juint(v, "bytesReceived");
      r->started_ms = jnum(v, "startedMs");
      if (const auto it = v.FindMember("dataIndex"); it != v.MemberEnd() && it->value.IsInt()) r->data_index = it->value.GetInt();
      if (const auto* da = jarr(v, "data")) {
        auto row = std::make_shared<DataRow>();
        for (const auto& kvp : da->GetArray())
          if (kvp.IsArray() && kvp.Size() == 2 && kvp[0].IsString() && kvp[1].IsString())
            row->emplace_back(kvp[0].GetString(), kvp[1].GetString());
        r->data_row = std::move(row);
      }
      if (const auto* ua = jarr(v, "unresolved"))
        for (const auto& n : ua->GetArray())
          if (n.IsString()) r->unresolved.emplace_back(n.GetString(), n.GetStringLength());
      if (const auto it = v.FindMember("timing"); it != v.MemberEnd() && it->value.IsObject()) {
        const auto& t = it->value;
        r->timing.total_ms = jnum(t, "totalMs");
        r->script_ms = jnum(t, "scriptMs");
        r->prep_ms = jnum(t, "prepMs");  // 0 in runs saved before it (their totalMs included it)
        r->timing.connection_reused = jbool(t, "connectionReused");
        r->timing.dns_ms = jnum(t, "dnsMs");
        r->timing.connect_ms = jnum(t, "connectMs");
        r->timing.tls_ms = jnum(t, "tlsMs");
        r->timing.ttfb_ms = jnum(t, "ttfbMs");
        if (const auto* pa = jarr(t, "phases"))
          for (const auto& ph : pa->GetArray())
            if (ph.IsObject()) {
              const std::string n = jstr(ph, "name");
              if (n == "Queue") r->timing.queue_ms = jnum(ph, "ms");
              r->phases.push_back({intern_phase(n), jnum(ph, "ms")});
            }
      }
      if (const auto* aa = jarr(v, "asserts"))
        for (const auto& x : aa->GetArray()) {
          StepResult::Check c;
          c.target = jstr(x, "target");
          c.op = jstr(x, "op");
          c.expected = jstr(x, "expected");
          c.actual = jstr(x, "actual");
          c.pass = jbool(x, "passed");
          c.script = jbool(x, "script");
          c.message = jstr(x, "message");
          c.from = jstr(x, "from");
          ++(c.pass ? r->asserts_passed : r->asserts_failed);
          if (c.script) ++r->script_tests;
          r->asserts.push_back(std::move(c));
        }
      if (const auto* la = jarr(v, "console"))
        for (const auto& x : la->GetArray()) {
          StepResult::Log l;
          l.post = jstr(x, "script") == "post-response";
          const std::string lv = jstr(x, "level");
          l.level = lv == "info" ? ScriptLog::Level::Info : lv == "warn" ? ScriptLog::Level::Warn
                  : lv == "error" ? ScriptLog::Level::Error : ScriptLog::Level::Log;
          l.text = jstr(x, "text");
          r->logs.push_back(std::move(l));
        }
      if (const auto* ca = jarr(v, "captures"))
        for (const auto& x : ca->GetArray()) {
          StepResult::Cap c;
          c.var = jstr(x, "var");
          c.scope = capture_scope_from(jstr(x, "scope"));
          c.ok = jbool(x, "ok");
          c.value = jstr(x, "value");
          c.message = jstr(x, "message");
          c.script = jbool(x, "script");
          c.removed = jbool(x, "removed");
          r->captures.push_back(std::move(c));
        }
      r->from_history = true;
      r->has_scripts = r->script_ms > 0 || r->script_tests > 0;
      r->scripts_ran = r->script_ms > 0;
      out.steps.push_back(std::move(r));
    }
  }
  return true;
}

namespace {
fs::path history_dir_for(const std::wstring& key) {
  const fs::path base = Workspace::user_config_dir();
  if (base.empty()) return {};
  uint64_t h = 1469598103934665603ull;  // FNV-1a over the key's UTF-16 units
  for (wchar_t c : key) {
    h ^= static_cast<uint16_t>(c);
    h *= 1099511628211ull;
  }
  char name[24];
  std::snprintf(name, sizeof name, "%016llx", static_cast<unsigned long long>(h));
  return base / "runs" / name;
}
}  // namespace

bool save_history(const std::wstring& key, const std::string& started_iso, const std::string& json, size_t keep,
                  std::string* err) {
  const fs::path dir = history_dir_for(key);
  if (dir.empty()) {
    if (err) *err = "no per-user config directory";
    return false;
  }
  std::error_code ec;
  fs::create_directories(dir, ec);
  // "2026-11-18T03:04:05Z" -> "20261118T030405Z-<n>.json": names sort by time.
  std::string stamp;
  for (char c : started_iso)
    if (std::isalnum(static_cast<unsigned char>(c))) stamp += c;
  if (stamp.empty()) stamp = "run";
  static std::atomic<unsigned> seq{0};
  char tail[48];
  std::snprintf(tail, sizeof tail, "-%06u-%06d.json", seq++ % 1000000u, _getpid());
  const std::string file = stamp + tail;
  if (!detail::write_file_atomic(dir / file, json, err)) return false;
  std::vector<fs::path> all;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.path().extension() == ".json") all.push_back(e.path());
  std::sort(all.begin(), all.end());
  while (all.size() > keep) {
    fs::remove(all.front(), ec);
    all.erase(all.begin());
  }
  return true;
}

std::vector<std::string> load_history(const std::wstring& key, size_t max) {
  std::vector<std::string> out;
  const fs::path dir = history_dir_for(key);
  if (dir.empty()) return out;
  std::error_code ec;
  std::vector<fs::path> all;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.path().extension() == ".json") all.push_back(e.path());
  std::sort(all.rbegin(), all.rend());
  for (const auto& p : all) {
    if (out.size() >= max) break;
    std::string t;
    if (detail::read_file(p, t)) out.push_back(std::move(t));
  }
  return out;
}

std::string xml_escape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    switch (ch) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      case '\'': o += "&apos;"; break;
      case '\n': o += "&#10;"; break;
      case '\r': o += "&#13;"; break;
      case '\t': o += "&#9;"; break;
      default:
        if (c < 0x20 || c == 0x7F) break;  // not allowed in XML 1.0
        o += ch;
    }
  }
  return o;
}

namespace {
// Element text: as xml_escape, but line breaks and tabs stay as they are (readable output).
std::string xml_text(const std::string& s) {
  std::string o = xml_escape(s);
  for (const auto& [from, to] : {std::pair<const char*, const char*>{"&#10;", "\n"}, {"&#9;", "\t"}}) {
    for (size_t at = o.find(from); at != std::string::npos; at = o.find(from, at + 1)) o.replace(at, std::strlen(from), to);
  }
  return o;
}
}  // namespace

std::string to_junit(const RunMeta& meta, const RunSummary& s, const std::vector<std::shared_ptr<const StepResult>>& steps) {
  auto secs = [](double ms) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3f", ms / 1000.0);
    return std::string(b);
  };
  // One <testsuite> per iteration; a request is a <testcase>, a failed assertion a <failure>.
  std::map<int, std::vector<const StepResult*>> by_iter;
  for (const auto& p : steps) by_iter[p->iteration].push_back(p.get());
  std::string o = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  size_t errors = 0, failures = 0, skipped = 0;
  for (const auto& p : steps) {
    if (p->skipped || p->stopped())
      ++skipped;
    else if (p->errored())
      ++errors;
    else if (p->failed())
      ++failures;
  }
  o += "<testsuites name=\"" + xml_escape(meta.name) + "\" tests=\"" + std::to_string(steps.size()) + "\" failures=\"" +
       std::to_string(failures) + "\" errors=\"" + std::to_string(errors) + "\"" +
       (skipped ? " skipped=\"" + std::to_string(skipped) + "\"" : std::string()) + " time=\"" + secs(s.wall_ms) +
       "\" timestamp=\"" + xml_escape(meta.started_iso) + "\">\n";
  for (const auto& [iter, list] : by_iter) {
    size_t f = 0, e = 0;
    double t = 0;
    for (const StepResult* r : list) {
      if (r->errored())
        ++e;
      else if (r->failed())
        ++f;
      t += r->timing.total_ms;
    }
    std::string suite = meta.name.empty() ? "watt" : meta.name;
    if (meta.iterations > 1) suite += " (iteration " + std::to_string(iter + 1) + ")";
    o += "  <testsuite name=\"" + xml_escape(suite) + "\" tests=\"" + std::to_string(list.size()) + "\" failures=\"" +
         std::to_string(f) + "\" errors=\"" + std::to_string(e) + "\" time=\"" + secs(t) + "\">\n";
    if (!meta.environment.empty())
      o += "    <properties><property name=\"environment\" value=\"" + xml_escape(meta.environment) +
           "\"/></properties>\n";
    for (const StepResult* r : list) {
      const std::string cls = r->rel.find('/') == std::string::npos ? meta.name : r->rel.substr(0, r->rel.rfind('/'));
      o += "    <testcase classname=\"" + xml_escape(cls.empty() ? std::string("watt") : cls) + "\" name=\"" +
           xml_escape(r->name.empty() ? r->rel : r->name) + "\" time=\"" + secs(r->timing.total_ms) + "\"";
      if (r->skipped || r->stopped()) {
        o += ">\n      <skipped message=\"" + xml_escape(r->skipped ? "not run: the run ended first" : "stopped") +
             "\"/>\n    </testcase>\n";
        continue;
      }
      if (!r->failed() && r->asserts.empty()) {
        o += "/>\n";
        continue;
      }
      o += ">\n";
      if (r->errored()) {
        o += "      <error message=\"" + xml_escape(r->error) + "\" type=\"" + (r->sent ? "transport" : "not-sent") +
             "\">" + xml_escape(r->method + " " + r->url) + "</error>\n";
      } else if (r->failed() && r->asserts.empty()) {
        const std::string m = r->reason();
        o += "      <failure message=\"" + xml_escape(m) + "\" type=\"status\">" + xml_escape(r->method + " " + r->url) +
             "</failure>\n";
      }
      for (const auto& a : r->asserts) {
        if (a.pass) continue;
        const std::string head = a.label();
        o += "      <failure message=\"" + xml_escape(head + (a.message.empty() ? "" : ": " + a.message)) +
             "\" type=\"" + (a.script ? "test" : "assert") + "\">" +
             xml_escape(a.script ? a.why() : "actual: " + (a.actual.empty() ? std::string("(undefined)") : a.actual)) +
             "</failure>\n";
      }
      std::string out;
      for (const auto& a : r->asserts) out += std::string(a.pass ? "pass " : "FAIL ") + a.label() + "\n";
      if (!out.empty()) o += "      <system-out>" + xml_text(out) + "</system-out>\n";
      o += "    </testcase>\n";
    }
    o += "  </testsuite>\n";
  }
  o += "</testsuites>\n";
  return o;
}

// ------------------------------------------------------------------ HTML report

bool sensitive_header(std::string_view name) {
  const std::string n = lower(name);
  for (const char* k : {"authorization", "proxy-authorization", "cookie", "set-cookie", "token", "secret", "password",
                        "api-key", "apikey", "x-api-key", "session", "signature", "credential"})
    if (n.find(k) != std::string::npos) return true;
  return false;
}

namespace {

std::string html_escape(std::string_view s) {
  std::string o;
  o.reserve(s.size() + 16);
  for (const char ch : s) {
    switch (ch) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      case '\'': o += "&#39;"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20 && ch != '\n' && ch != '\t') break;
        o += ch;
    }
  }
  return o;
}

std::string ms_text(double v) {
  char b[32];
  if (v < 1)
    std::snprintf(b, sizeof b, "%.2f ms", v);
  else if (v < 100)
    std::snprintf(b, sizeof b, "%.1f ms", v);
  else if (v < 10000)
    std::snprintf(b, sizeof b, "%.0f ms", v);
  else
    std::snprintf(b, sizeof b, "%.2f s", v / 1000);
  return b;
}

std::string bytes_text(uint64_t n) {
  char b[32];
  if (n < 1024)
    std::snprintf(b, sizeof b, "%llu B", static_cast<unsigned long long>(n));
  else if (n < 1024 * 1024)
    std::snprintf(b, sizeof b, "%.1f KB", static_cast<double>(n) / 1024);
  else
    std::snprintf(b, sizeof b, "%.1f MB", static_cast<double>(n) / (1024.0 * 1024.0));
  return b;
}

}  // namespace

std::string to_html(const RunMeta& meta, const RunSummary& s0, const std::vector<std::shared_ptr<const StepResult>>& steps) {
  const RunSummary s = summarize(steps, s0.planned, s0.wall_ms, s0.stopped, true);
  std::string o;
  o.reserve(64 * 1024 + steps.size() * 300);
  o += "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
       "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
       "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; style-src 'unsafe-inline'\">"
       "<title>" + html_escape(meta.name.empty() ? std::string("watt run") : meta.name + " run") + "</title><style>\n"
       ":root{--bg:#fff;--fg:#1d1d1f;--dim:#6b6b70;--grid:#e3e3e8;--ok:#1f8a4c;--err:#c8322b;--warn:#b26b00;--card:#f6f6f8}\n"
       "@media (prefers-color-scheme: dark){:root{--bg:#1e1e1e;--fg:#e6e6e6;--dim:#9a9aa0;--grid:#38383c;--ok:#4cc27a;--err:#f0605a;--warn:#e0a040;--card:#262628}}\n"
       "body{background:var(--bg);color:var(--fg);font:14px/1.45 'Segoe UI',system-ui,sans-serif;margin:0;padding:24px 16px}\n"
       "main{max-width:1200px;margin:0 auto}h1{font-size:22px;margin:0 0 4px}h2{font-size:15px;margin:28px 0 8px;color:var(--dim);text-transform:uppercase;letter-spacing:.04em}\n"
       ".sub{color:var(--dim)}.tiles{display:flex;flex-wrap:wrap;gap:10px 28px;margin:18px 0}.tile b{display:block;font-size:22px}.tile span{font-size:11px;color:var(--dim);text-transform:uppercase}\n"
       "table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums}th,td{text-align:left;padding:5px 8px;border-bottom:1px solid var(--grid);vertical-align:top}\n"
       "th{font-size:12px;color:var(--dim)}td.n,th.n{text-align:right}.ok{color:var(--ok)}.err{color:var(--err)}.warn{color:var(--warn)}.dim{color:var(--dim)}\n"
       ".wrap{overflow-x:auto}details{background:var(--card);border-radius:6px;margin:6px 0;padding:6px 10px}summary{cursor:pointer}\n"
       "pre{white-space:pre-wrap;word-break:break-all;font:12px/1.4 Consolas,monospace;margin:6px 0;max-height:480px;overflow:auto}code{font:12px Consolas,monospace}\n"
       "</style></head><body><main>\n";
  o += "<h1>" + html_escape(meta.name.empty() ? std::string("Run") : meta.name) + "</h1>\n<div class=\"sub\">" +
       html_escape(meta.started_iso) + (meta.environment.empty() ? "" : " &middot; environment " + html_escape(meta.environment)) +
       " &middot; " + std::to_string(meta.iterations) + " iteration" + (meta.iterations == 1 ? "" : "s") +
       (s.stopped ? " &middot; <span class=\"warn\">stopped</span>" : "") + " &middot; watt</div>\n";
  auto tile = [&](const std::string& label, const std::string& value, const char* cls) {
    o += "<div class=\"tile\"><span>" + label + "</span><b class=\"" + cls + "\">" + html_escape(value) + "</b></div>";
  };
  o += "<div class=\"tiles\">";
  tile("Passed", std::to_string(s.passed), s.passed ? "ok" : "dim");
  tile("Failed", std::to_string(s.failed), s.failed ? "err" : "dim");
  if (s.stopped_steps + s.skipped) tile("Skipped", std::to_string(s.stopped_steps + s.skipped), "warn");
  tile("Requests", std::to_string(s.total) + " / " + std::to_string(s.planned), "");
  tile("Asserts", std::to_string(s.asserts_passed) + " / " + std::to_string(s.asserts_passed + s.asserts_failed),
       s.asserts_failed ? "err" : "ok");
  tile("Duration", ms_text(s.wall_ms), "");
  tile("Avg", ms_text(s.avg_ms), "");
  tile("Min", ms_text(s.min_ms), "");
  tile("P50", ms_text(s.p50_ms), "");
  tile("P95", ms_text(s.p95_ms), "");
  tile("Max", ms_text(s.max_ms), "");
  tile("Std dev", ms_text(s.stddev_ms), "");
  tile("Received", bytes_text(s.bytes_received), "");
  o += "</div>\n<div class=\"sub\">Average DNS " + ms_text(s.avg_dns_ms) + ", connect " + ms_text(s.avg_connect_ms) +
       ", TLS " + ms_text(s.avg_tls_ms) + " (over " + std::to_string(s.new_connections) + " new connection" +
       (s.new_connections == 1 ? "" : "s") + "), TTFB " + ms_text(s.avg_ttfb_ms) + "</div>\n";
  o += "<h2>Requests</h2><div class=\"wrap\"><table><tr><th>Request</th><th class=\"n\">Count</th><th class=\"n\">Failed</th>"
       "<th class=\"n\">Avg</th><th class=\"n\">Min</th><th class=\"n\">P50</th><th class=\"n\">P95</th><th class=\"n\">Max</th></tr>\n";
  for (const auto& q : s.per_request) {
    o += "<tr><td><code>" + html_escape(q.kind == RequestKind::Http ? q.method : "gRPC") + "</code> " +
         html_escape(q.rel) + "</td><td class=\"n\">" + std::to_string(q.count) + "</td><td class=\"n " +
         (q.failures ? "err" : "dim") + "\">" + std::to_string(q.failures) + "</td><td class=\"n\">" + ms_text(q.avg_ms) +
         "</td><td class=\"n\">" + ms_text(q.min_ms) + "</td><td class=\"n\">" + ms_text(q.p50_ms) +
         "</td><td class=\"n\">" + ms_text(q.p95_ms) + "</td><td class=\"n\">" + ms_text(q.max_ms) + "</td></tr>\n";
  }
  o += "</table></div>\n";
  // Failed steps first, with what is needed to diagnose them; then every step in order.
  size_t shown = 0;
  for (const auto& p : steps) {
    const StepResult& r = *p;
    if (!r.failed()) continue;
    if (shown == 0) o += "<h2>Failures</h2>\n";
    if (++shown > 200) {
      o += "<p class=\"dim\">... more failures in the results below.</p>\n";
      break;
    }
    o += "<details open><summary><b class=\"err\">#" + std::to_string(r.seq + 1) + "</b> <code>" +
         html_escape(r.kind == RequestKind::Http ? r.method : "gRPC") + "</code> " + html_escape(r.rel) +
         " <span class=\"dim\">iteration " + std::to_string(r.iteration + 1) + "</span></summary>\n";
    o += "<div><code>" + html_escape(r.url) + "</code></div>";
    if (!r.error.empty()) o += "<div class=\"err\">" + html_escape(r.error) + "</div>";
    else if (r.asserts.empty()) o += "<div class=\"err\">" + html_escape(r.reason()) + "</div>";
    if (r.sent)
      o += "<div>Status <b>" + html_escape(std::to_string(r.status) + (r.status_text.empty() ? "" : " " + r.status_text)) +
           "</b> &middot; " + ms_text(r.timing.total_ms) + " &middot; " + bytes_text(r.size) + "</div>";
    for (const auto& a : r.asserts)
      o += std::string("<div class=\"") + (a.pass ? "ok" : "err") + "\">" + (a.pass ? "&#10003; " : "&#10007; ") + "<code>" +
           html_escape(a.label()) + "</code> " + html_escape(a.why()) + "</div>";
    if (!r.req_headers.empty()) {
      std::string h;
      for (const auto& [k, v] : r.req_headers) h += k + ": " + (sensitive_header(k) ? std::string("(hidden)") : v) + "\n";
      o += "<div class=\"dim\">Request headers</div><pre>" + html_escape(h) + "</pre>";
    }
    if (!r.resp_headers.empty()) {
      std::string h;
      for (const auto& [k, v] : r.resp_headers) h += k + ": " + (sensitive_header(k) ? std::string("(hidden)") : v) + "\n";
      o += "<div class=\"dim\">Response headers</div><pre>" + html_escape(h) + "</pre>";
    }
    if (!r.body_preview.empty()) {
      const std::string_view b = std::string_view(r.body_preview).substr(0, 16384);
      o += "<div class=\"dim\">Response body" +
           std::string(r.body_truncated || b.size() < r.body_preview.size() ? " (first " + bytes_text(b.size()) + ")" : "") +
           "</div><pre>" + html_escape(b) + "</pre>";
    }
    o += "</details>\n";
  }
  o += "<h2>Results</h2><div class=\"wrap\"><table><tr><th class=\"n\">#</th><th class=\"n\">Iter</th><th>Request</th>"
       "<th>Status</th><th class=\"n\">Time</th><th class=\"n\">Size</th><th>Asserts</th></tr>\n";
  size_t rows = 0;
  for (const auto& p : steps) {
    const StepResult& r = *p;
    if (++rows > 20000) {
      o += "<tr><td colspan=\"7\" class=\"dim\">" + std::to_string(steps.size() - 20000) +
           " more results are in the JSON export.</td></tr>\n";
      break;
    }
    std::string st;
    const char* cls = "ok";
    if (r.skipped) {
      st = "skipped";
      cls = "dim";
    } else if (r.stopped()) {
      st = "stopped";
      cls = "warn";
    } else if (!r.sent) {
      st = "not sent";
      cls = "err";
    } else if (r.kind == RequestKind::Http && !r.transport_ok) {
      st = "error";
      cls = "err";
    } else {
      st = r.kind == RequestKind::Http ? std::to_string(r.status) + (r.status_text.empty() ? "" : " " + r.status_text)
                                        : (r.status_text.empty() ? std::to_string(r.status) : r.status_text);
      cls = r.failed() ? "err" : "ok";
    }
    o += "<tr><td class=\"n dim\">" + std::to_string(r.seq + 1) + "</td><td class=\"n dim\">" +
         std::to_string(r.iteration + 1) + "</td><td><code>" + html_escape(r.kind == RequestKind::Http ? r.method : "gRPC") +
         "</code> " + html_escape(r.rel) + "</td><td class=\"" + cls + "\">" + html_escape(st) + "</td><td class=\"n\">" +
         (r.sent ? ms_text(r.timing.total_ms) : "-") + "</td><td class=\"n dim\">" + (r.sent ? bytes_text(r.size) : "-") +
         "</td><td class=\"" + (r.asserts_failed ? "err" : "ok") + "\">" +
         (r.asserts.empty() ? std::string(r.failed() ? "failed" : "") : std::to_string(r.asserts_passed) + " / " + std::to_string(r.asserts.size())) +
         "</td></tr>\n";
  }
  o += "</table></div>\n</main></body></html>\n";
  return o;
}

// ------------------------------------------------------------------ failure triage

std::string normalize_numbers(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  auto digit = [&](size_t i) { return i < s.size() && s[i] >= '0' && s[i] <= '9'; };
  for (size_t i = 0; i < s.size();) {
    const bool sign = (s[i] == '-' || s[i] == '+') && digit(i + 1) &&
                      (i == 0 || !std::isalnum(static_cast<unsigned char>(s[i - 1])));
    if (!digit(i) && !sign) {
      out += s[i++];
      continue;
    }
    if (sign) ++i;
    while (digit(i)) ++i;
    if (i + 1 < s.size() && s[i] == '.' && digit(i + 1)) {
      ++i;
      while (digit(i)) ++i;
    }
    if (i + 1 < s.size() && (s[i] == 'e' || s[i] == 'E') &&
        (digit(i + 1) || ((s[i + 1] == '-' || s[i + 1] == '+') && digit(i + 2)))) {
      i += 2;
      while (digit(i)) ++i;
    }
    if (out.empty() || out.back() != '#') out += '#';  // never two marks in a row
  }
  return out;
}

std::vector<Failure> failures_of(const StepResult& r) {
  std::vector<Failure> f;
  if (!r.failed()) return f;
  if (r.errored()) {
    f.push_back({r.sent ? "transport" : "sent", r.error.empty() ? std::string("not sent") : r.error});
    return f;
  }
  for (const auto& a : r.asserts)
    if (!a.pass)
      f.push_back({a.script ? (a.op == "error" ? std::string("script error") : "test: " + a.target) : a.label(), a.text()});
  if (f.empty()) {
    if (r.kind == RequestKind::Grpc)
      f.push_back({"expected gRPC OK", "gRPC " + (r.status_text.empty() ? std::to_string(r.status) : r.status_text) +
                                         (r.error.empty() || r.error == r.status_text ? "" : ": " + r.error)});
    else
      f.push_back({"expected status < 400", "HTTP " + std::to_string(r.status) + (r.status_text.empty() ? "" : " " + r.status_text)});
  }
  return f;
}

std::string failure_key(const StepResult& s, const Failure& f) {
  return s.rel + '\x1f' + f.check + '\x1f' + normalize_numbers(f.message);
}

bool has_failure(const StepResult& s, const std::string& key) {
  // Cheap reject first: the key starts with the request's path.
  if (key.size() <= s.rel.size() || key.compare(0, s.rel.size(), s.rel) != 0 || key[s.rel.size()] != '\x1f') return false;
  if (!s.failed()) return false;
  for (const auto& f : failures_of(s))
    if (failure_key(s, f) == key) return true;
  return false;
}

std::vector<FailureGroup> group_failures(const std::vector<std::shared_ptr<const StepResult>>& steps) {
  std::vector<FailureGroup> g;
  std::map<std::string, size_t> at;
  std::vector<std::set<std::string>> seen;  // distinct messages per group (capped)
  std::vector<std::vector<bool>> its;       // failing iterations per group
  for (const auto& p : steps) {
    const StepResult& r = *p;
    if (!r.failed()) continue;
    for (const auto& f : failures_of(r)) {
      std::string key = failure_key(r, f);
      auto [it, fresh] = at.try_emplace(key, g.size());
      if (fresh) {
        FailureGroup x;
        x.key = std::move(key);
        x.item = r.item;
        x.name = r.name;
        x.rel = r.rel;
        x.method = r.method;
        x.kind = r.kind;
        x.check = f.check;
        x.message = x.last_message = f.message;
        x.first_iteration = x.last_iteration = r.iteration;
        x.first_seq = x.last_seq = r.seq;
        x.iterations = 0;
        g.push_back(std::move(x));
        seen.emplace_back();
        its.emplace_back();
      }
      FailureGroup& x = g[it->second];
      ++x.count;
      if (!r.unresolved.empty()) {
        ++x.unresolved;
        for (const auto& n : r.unresolved)
          if (x.unresolved_names.size() < 4 &&
              std::find(x.unresolved_names.begin(), x.unresolved_names.end(), n) == x.unresolved_names.end())
            x.unresolved_names.push_back(n);
      }
      // Steps arrive in finishing order (parallel workers): the first / last message by sequence
      // number, but the iteration range by iteration number (completion order does not follow it).
      if (r.seq < x.first_seq) {
        x.first_seq = r.seq;
        x.message = f.message;
      }
      if (r.seq >= x.last_seq) {
        x.last_seq = r.seq;
        x.last_message = f.message;
      }
      x.first_iteration = std::min(x.first_iteration, r.iteration);
      x.last_iteration = std::max(x.last_iteration, r.iteration);
      if (r.iteration >= 0) {
        auto& bits = its[it->second];
        const size_t i = static_cast<size_t>(r.iteration);
        if (i >= bits.size()) bits.resize(i + 1, false);
        if (!bits[i]) {
          bits[i] = true;
          ++x.iterations;
        }
      }
      auto& sn = seen[it->second];
      if (sn.size() < 1000 && sn.insert(f.message).second) x.variants = sn.size();
    }
  }
  std::stable_sort(g.begin(), g.end(), [](const FailureGroup& a, const FailureGroup& b) {
    return a.count != b.count ? a.count > b.count : a.first_seq < b.first_seq;
  });
  return g;
}

// ------------------------------------------------------------------ thresholds

namespace {
const char* const kMetrics[] = {"avg", "min", "max", "p50", "p90", "p95", "p99", "err", "rps"};
}  // namespace

bool parse_thresholds(const std::string& spec, std::vector<Threshold>* out, std::string* err) {
  out->clear();
  const std::string_view s = spec;
  size_t i = 0;
  auto space = [&] {
    while (i < s.size() && (std::isspace(static_cast<unsigned char>(s[i])) || s[i] == ',' || s[i] == ';')) ++i;
  };
  for (space(); i < s.size(); space()) {
    const size_t m0 = i;
    while (i < s.size() && std::isalnum(static_cast<unsigned char>(s[i]))) ++i;
    std::string metric = lower(s.substr(m0, i - m0));
    if (metric == "error" || metric == "errors" || metric == "fail" || metric == "failed") metric = "err";
    if (metric == "throughput") metric = "rps";
    if (std::find_if(std::begin(kMetrics), std::end(kMetrics), [&](const char* k) { return metric == k; }) == std::end(kMetrics)) {
      if (err)
        *err = "unknown metric \"" + std::string(s.substr(m0, std::max<size_t>(i - m0, 1))) +
               "\" (avg, min, max, p50, p90, p95, p99, err, rps)";
      return false;
    }
    while (i < s.size() && s[i] == ' ') ++i;
    std::string op;
    if (i < s.size() && (s[i] == '<' || s[i] == '>')) op += s[i++];
    if (!op.empty() && i < s.size() && s[i] == '=') op += s[i++];
    if (op.empty()) {
      if (err) *err = metric + ": expected <, <=, > or >=";
      return false;
    }
    while (i < s.size() && s[i] == ' ') ++i;
    const size_t v0 = i;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) ++i;
    double v = 0;
    try {
      v = std::stod(std::string(s.substr(v0, i - v0)));
    } catch (...) {
      if (err) *err = metric + " " + op + ": expected a number";
      return false;
    }
    std::string unit;
    while (i < s.size() && (std::isalpha(static_cast<unsigned char>(s[i])) || s[i] == '%' || s[i] == '/'))
      unit += lower_ascii(s[i++]);
    const bool time = metric != "err" && metric != "rps";
    if (time && unit == "s") {
      v *= 1000;
    } else if (!(unit.empty() || (time && unit == "ms") || (metric == "err" && unit == "%") || (metric == "rps" && unit == "/s"))) {
      if (err) *err = metric + ": unit \"" + unit + "\" does not fit (ms or s for times, % for err, /s for rps)";
      return false;
    }
    Threshold t;
    t.metric = metric;
    t.op = op;
    t.value = v;
    char b[64];
    std::snprintf(b, sizeof b, "%g", v);
    t.text = metric + " " + op + " " + b + (time ? " ms" : metric == "err" ? "%" : "/s");
    out->push_back(std::move(t));
  }
  return true;
}

std::vector<ThresholdResult> check_thresholds(const std::vector<Threshold>& ts, const RunSummary& s) {
  std::vector<ThresholdResult> out;
  const size_t finished = s.passed + s.failed;
  for (const auto& t : ts) {
    ThresholdResult r;
    r.t = t;
    char b[64];
    if (t.metric == "err") {
      r.actual = finished ? 100.0 * static_cast<double>(s.failed) / static_cast<double>(finished) : 0;
      std::snprintf(b, sizeof b, "%.2f%%", r.actual);
    } else if (t.metric == "rps") {
      r.actual = s.wall_ms > 0 ? static_cast<double>(s.total - s.stopped_steps) * 1000.0 / s.wall_ms : 0;
      std::snprintf(b, sizeof b, "%.1f/s", r.actual);
    } else {
      r.actual = t.metric == "avg"   ? s.avg_ms
                 : t.metric == "min" ? s.min_ms
                 : t.metric == "max" ? s.max_ms
                 : t.metric == "p50" ? s.p50_ms
                 : t.metric == "p90" ? s.p90_ms
                 : t.metric == "p95" ? s.p95_ms
                                     : s.p99_ms;
      std::snprintf(b, sizeof b, r.actual < 100 ? "%.2f ms" : "%.0f ms", r.actual);
    }
    r.actual_text = b;
    const bool any = t.metric == "err" ? finished > 0 : s.samples > 0;
    r.pass = any && (t.op == "<"    ? r.actual < t.value
                     : t.op == "<=" ? r.actual <= t.value
                     : t.op == ">"  ? r.actual > t.value
                                    : r.actual >= t.value);
    if (!any) r.actual_text = "no data";
    out.push_back(std::move(r));
  }
  return out;
}

}  // namespace watt::app::runner
