// Headless collection run for CI (Newman's `newman run`, Bruno's `bru run`):
//
//   watt.exe --run <folder or .req.json> [options]
//     --workspace <dir>      workspace root (default: the nearest parent of <folder> holding .watt)
//     --env <name>           environment to use (default: the workspace's active one)
//     --iterations <n>       default 1, or one per data row with --data
//     --delay <ms>           pause before every request but the first
//     --data <file.csv|json> data-driven iterations: each row's columns become {{vars}}
//     --filter "<tags>"      tag / name filter: "smoke -slow"
//     --parallel <n>         n workers (load mode)
//     --duration <s>         load profile: repeat the iterations for s seconds
//     --ramp-up <s>          load profile: start the parallel workers one by one over s seconds
//     --profile <p>          fixed | ramp (default with --ramp-up) | peak (ramp up, hold, ramp down
//                            over the last --ramp-up seconds of --duration) | spike (a quarter of
//                            the workers all through, all at once in the middle fifth of --duration)
//     --bail                 stop at the first failure
//     --no-cookies           do not carry Set-Cookie values to the later requests of the run
//     --trust-scripts        run this workspace's pre-request / post-response scripts for this
//                            run only (the trust is withdrawn when the run ends, so a CI run does
//                            not trust a cloned folder in the app); without it they run only when
//                            the workspace is already trusted, else such requests fail unrun
//     --reporter-junit <file>  --reporter-json <file>  --reporter-html <file>
//     --quiet                no line per request: the failures by cause and the summary only
//     --threshold "<spec>"   pass / fail criteria over the run: "p95<200 p99<500 err<1% rps>=50"
//                            (a threshold missed exits 1, like a failed request)
//
// A relative <folder> is looked up below --workspace when one is given (and the current
// directory otherwise).
//
// Exit code: 0 all passed, 1 at least one failure, 2 usage or setup error. Nothing is ever asked:
// upload / schema files outside the workspace and OS environment variables are read only when the
// workspace already allows them (approved once in the app); requests needing anything else are
// not sent and fail with the reason.
//
// wx-free: the wxModule in runner_view.cpp calls run_cli() before any window exists.
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <mutex>

#include "app/runner_engine.h"

namespace watt::app::runner {

namespace fs = std::filesystem;

namespace {

std::string u8(const fs::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}

fs::path from_u8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string pad_right(std::string s, size_t n) {
  if (s.size() < n) s.append(n - s.size(), ' ');
  return s;
}

// Display width in characters (UTF-8 code points), for padding names like "über".
size_t width(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s) n += (c & 0xC0) != 0x80;
  return n;
}

std::string pad_cols(std::string s, size_t n) {
  if (const size_t w = width(s); w < n) s.append(n - w, ' ');
  return s;
}

std::string ms(double v) {
  char b[32];
  if (v < 1)
    std::snprintf(b, sizeof b, "%.2f ms", v);
  else if (v < 100)
    std::snprintf(b, sizeof b, "%.1f ms", v);
  else
    std::snprintf(b, sizeof b, "%.0f ms", v);
  return b;
}

std::string bytes(uint64_t n) {
  char b[32];
  if (n < 1024)
    std::snprintf(b, sizeof b, "%llu B", static_cast<unsigned long long>(n));
  else if (n < 1024 * 1024)
    std::snprintf(b, sizeof b, "%.1f KB", static_cast<double>(n) / 1024);
  else
    std::snprintf(b, sizeof b, "%.1f MB", static_cast<double>(n) / (1024.0 * 1024.0));
  return b;
}

bool write_file(const std::string& path, const std::string& text, std::string* out) {
  std::ofstream f(from_u8(path), std::ios::binary | std::ios::trunc);
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!f) {
    *out += "error: cannot write " + path + "\n";
    return false;
  }
  return true;
}

}  // namespace

int run_cli(const std::vector<std::string>& args, const GrpcFactory& grpc, std::string* out) {
  std::string folder_s, ws_s, env, data_s, filter, junit, json, html, threshold_s, profile = "ramp";
  int iterations = 0, delay = 0, parallel = 1, duration = 0, ramp = 0;
  bool bail = false, quiet = false, trust = false, cookies = true;
  auto usage = [&](const std::string& why) {
    *out += "error: " + why +
            "\nusage: watt --run <folder> [--workspace <dir>] [--env <name>] [--iterations <n>] [--delay <ms>]\n"
            "       [--data <file.csv|file.json>] [--filter \"<tags>\"] [--parallel <n>] [--duration <s>] [--ramp-up <s>]\n"
            "       [--profile fixed|ramp|peak|spike]\n"
            "       [--bail] [--no-cookies] [--trust-scripts]\n"
            "       [--reporter-junit <file>] [--reporter-json <file>] [--reporter-html <file>] [--quiet]\n"
            "       [--threshold \"p95<200 err<1%\"]\n";
    return 2;
  };
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto val = [&](std::string& v) {
      if (i + 1 >= args.size()) return false;
      v = args[++i];
      return true;
    };
    auto num = [&](int& v, int lo, int hi) {
      std::string t;
      if (!val(t)) return false;
      try {
        v = std::stoi(t);
      } catch (...) {
        return false;
      }
      return v >= lo && v <= hi;
    };
    bool ok = true;
    if (a == "--run") ok = val(folder_s);
    else if (a == "--workspace") ok = val(ws_s);
    else if (a == "--env") ok = val(env);
    else if (a == "--iterations" || a == "-n") ok = num(iterations, 1, 1000000);
    else if (a == "--delay") ok = num(delay, 0, 600000);
    else if (a == "--data" || a == "-d") ok = val(data_s);
    else if (a == "--filter" || a == "--tags") ok = val(filter);
    else if (a == "--parallel") ok = num(parallel, 1, static_cast<int>(RunControl::kSlots));
    else if (a == "--duration") ok = num(duration, 1, 86400);
    else if (a == "--ramp-up") ok = num(ramp, 0, 3600);
    else if (a == "--profile") ok = val(profile) && (profile == "fixed" || profile == "ramp" || profile == "peak" || profile == "spike");
    else if (a == "--bail") bail = true;
    else if (a == "--no-cookies") cookies = false;
    else if (a == "--quiet") quiet = true;
    else if (a == "--trust-scripts") trust = true;
    else if (a == "--reporter-junit") ok = val(junit);
    else if (a == "--reporter-json") ok = val(json);
    else if (a == "--reporter-html") ok = val(html);
    else if (a == "--threshold" || a == "--thresholds") ok = val(threshold_s);
    else return usage("unknown option " + a);
    if (!ok) return usage("bad or missing value for " + a);
  }
  if (folder_s.empty()) return usage("--run needs a folder");
  std::vector<Threshold> thresholds;
  {
    std::string terr;
    if (!threshold_s.empty() && !parse_thresholds(threshold_s, &thresholds, &terr)) return usage("--threshold: " + terr);
  }
  std::error_code ec;
  fs::path root = ws_s.empty() ? fs::path() : fs::absolute(from_u8(ws_s), ec);
  // A relative folder names a folder of the workspace given ("Users API"), as the app shows it.
  const fs::path folder_arg = from_u8(folder_s);
  const fs::path folder = folder_arg.is_relative() && !root.empty() ? (root / folder_arg).lexically_normal()
                                                                    : fs::absolute(folder_arg, ec);
  if (root.empty()) {
    for (fs::path p = fs::is_directory(folder, ec) ? folder : folder.parent_path(); !p.empty(); p = p.parent_path()) {
      if (fs::is_directory(p / ".watt", ec)) {
        root = p;
        break;
      }
      if (p == p.parent_path()) break;
    }
    if (root.empty()) root = fs::is_directory(folder, ec) ? folder : folder.parent_path();
  }
  std::string err;
  auto ws = Workspace::open(root, &err);
  if (!ws) {
    *out += "error: cannot open workspace " + u8(root) + (err.empty() ? "" : ": " + err) + "\n";
    return 2;
  }
  if (!env.empty()) {
    bool found = false;
    for (const auto& e : ws->environments()) found = found || e.name == env;
    if (!found) {
      *out += "error: no environment named \"" + env + "\"\n";
      return 2;
    }
    ws->active_env = env;
  }
  std::vector<std::string> errs;
  auto items = collect(*ws, folder, &errs);
  for (const auto& e : errs) *out += "warning: " + e + "\n";
  std::vector<size_t> order;
  for (size_t i = 0; i < items.size(); ++i)
    if (filter.empty() || tag_filter_match(items[i], filter)) order.push_back(i);
  if (order.empty()) {
    *out += "error: no requests to run\n";
    return 2;
  }
  RunOptions o;
  if (!data_s.empty()) {
    if (!load_data_file(from_u8(data_s), &o.data, &err)) {
      *out += "error: data file: " + err + "\n";
      return 2;
    }
  }
  o.iterations = iterations ? iterations : (o.data.empty() ? 1 : static_cast<int>(o.data.size()));
  o.delay_ms = delay;
  o.stop_on_failure = bail;
  o.concurrency = parallel;
  o.duration_ms = duration * 1000;
  if (profile == "fixed") ramp = 0;
  if ((profile == "peak" || profile == "spike") && duration <= 0) return usage("--profile " + profile + " needs --duration");
  if (profile == "spike") ramp = 0;
  o.ramp_up_ms = ramp * 1000;
  if (profile == "peak") o.ramp_down_ms = std::min(o.ramp_up_ms, o.duration_ms / 2);
  if (profile == "spike" && parallel > 1) o.spike_base = std::max(1, parallel / 4);
  o.cookies = cookies;
  Consent c = consent_needed(*ws, items, order);
  // --trust-scripts trusts for this run only. The script engine reads the trust record itself, so
  // it is written for the run and withdrawn when run_cli returns (any path), leaving the app's
  // decision for this folder as it was (untrusted: only then is it written).
  struct TrustForRun {
    const Workspace* ws = nullptr;
    ~TrustForRun() {
      if (ws) ws->trust_scripts(false);
    }
  } trust_for_run;
  if (c.scripts_need_trust() && trust) {
    if (!ws->trust_scripts(true, &err)) {
      *out += "error: cannot trust the scripts of " + u8(ws->root()) + (err.empty() ? "" : ": " + err) + "\n";
      return 2;
    }
    trust_for_run.ws = &*ws;
    *out += "trusting the scripts of " + u8(ws->root()) + " for this run only\n";
    c.scripts_trusted = true;
  }
  if (c.scripts_need_trust())
    *out += "warning: " + std::to_string(c.script_requests) +
            " request(s) have scripts, but this workspace's scripts are not trusted: they are sent without them and "
            "fail (trust them in the app, or pass --trust-scripts)\n";
  o.run_scripts = c.script_requests > 0 && c.scripts_trusted;
  for (const auto& f : c.files)
    *out += "warning: " + u8(f) + " is outside the workspace and not allowed: requests reading it are not sent\n";
  for (const auto& v : c.env_vars)
    *out += "warning: environment variable " + v + " is not allowed: requests reading it are not sent\n";

  RunMeta meta;
  meta.name = u8((fs::equivalent(folder, ws->root(), ec) ? ws->root() : folder).filename());
  meta.name = strip_ext(meta.name);
  meta.environment = ws->active_env;
  meta.started_iso = now_iso();
  meta.iterations = o.iterations;
  *out += "watt run " + meta.name + (meta.environment.empty() ? "" : " (environment " + meta.environment + ")") + ": " +
          std::to_string(order.size()) + " request(s) x " +
          (duration ? "as many iterations as fit in " + std::to_string(duration) + " s" : std::to_string(o.iterations) + " iteration(s)") +
          (parallel > 1 ? ", " + std::to_string(parallel) + " in parallel" : "") +
          (ramp > 0 ? (profile == "peak" ? ", peak: ramp " : ", ramp-up ") + std::to_string(ramp) + " s" : "") +
          (o.spike_base ? ", spike: " + std::to_string(o.spike_base) + " base, all " + std::to_string(parallel) + " in the middle fifth" : "") +
          "\n\n";
  // Requests by name, as the app lists them (the file name only when a request has none).
  const auto label = [](const StepResult& r) { return r.name.empty() ? r.rel : r.name; };
  // The status column fits the longest gRPC status name (RESOURCE_EXHAUSTED) when there is gRPC.
  size_t status_w = 4;
  for (size_t i : order)
    if (items[i].req.kind == RequestKind::Grpc) status_w = 19;
  std::vector<std::shared_ptr<const StepResult>> steps;
  std::mutex m;
  RunControl ctl;
  int last_iteration = -1;
  const bool many = o.iterations > 1 || duration > 0;
  const RunSummary s = run_plan(items, order, o, *ws, grpc, ctl, [&](std::shared_ptr<const StepResult> r) {
    std::lock_guard lk(m);
    steps.push_back(r);
    if (quiet) return;
    if (many && parallel <= 1 && r->iteration != last_iteration) {
      // Newman's "Iteration n/N" separator (a parallel run interleaves iterations: none there).
      last_iteration = r->iteration;
      std::string head = "Iteration " + std::to_string(r->iteration + 1) + (duration ? "" : "/" + std::to_string(o.iterations));
      if (r->data_row && !r->data_row->empty()) {
        std::string row;
        for (const auto& [k, v] : *r->data_row) {
          if (row.size() > 80) {
            row += ", ...";
            break;
          }
          row += (row.empty() ? "" : ", ") + k + "=" + (v.size() > 24 ? v.substr(0, 21) + "..." : v);
        }
        head += "  (data row " + std::to_string(r->data_index + 1) + ": " + row + ")";
      }
      *out += (steps.size() > 1 ? "\n" : "") + head + "\n";
    }
    std::string line = r->passed() ? "  ok    " : "  FAIL  ";
    line += pad_right(r->kind == RequestKind::Http ? r->method : "gRPC", 7) + pad_cols(label(*r), 34) + " ";
    if (r->sent) {
      const std::string st = r->kind == RequestKind::Http ? std::to_string(r->status) : r->status_text;
      line += pad_right(st, std::max(status_w, st.size() + 1)) + ms(r->timing.total_ms);
      if (r->script_ms > 0) line += "  (+" + ms(r->script_ms) + " scripts)";
    }
    else
      line += r->error;
    *out += line + "\n";
    for (const auto& l : r->logs) *out += "          | " + l.text + "\n";
    if (!r->unresolved.empty()) {
      std::string names;
      for (const auto& n : r->unresolved) names += (names.empty() ? "{{" : ", {{") + n + "}}";
      *out += "          ! unresolved variable" + std::string(r->unresolved.size() == 1 ? " " : "s ") + names + " sent as written\n";
    }
    for (const auto& a : r->asserts)
      if (!a.pass) *out += "          x " + a.text() + "\n";
    if (r->failed() && (r->sent || r->asserts.empty())) {
      // The reason line for what the asserts do not say: a transport error, or the status of a
      // request without asserts (a not-sent request's reason is on its own line already).
      if (r->sent && !r->transport_ok && !r->error.empty()) *out += "          " + r->error + "\n";
      else if (r->sent && r->asserts.empty()) *out += "          " + r->reason() + "\n";
    }
  });
  if (duration) {
    // A duration run: the iterations that actually ran.
    meta.iterations = 0;
    for (const auto& r : steps) meta.iterations = std::max(meta.iterations, r->iteration + 1);
  }
  const RunSummary full = summarize(steps, s.planned, s.wall_ms, s.stopped, true);
  // A spread or a rate needs two samples: one send reads "-".
  const std::string sd = full.samples > 1 ? ms(full.stddev_ms) : std::string("-");
  std::string tput = "-";
  if (full.samples > 1 && full.wall_ms > 0) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f / s", static_cast<double>(full.total - full.stopped_steps) * 1000.0 / full.wall_ms);
    tput = b;
  }
  char line[768];
  std::snprintf(line, sizeof line,
                "\n%zu passed, %zu failed, %zu requests in %s; asserts %zu / %zu; %s received\n"
                "response time: avg %s, min %s, p50 %s, p90 %s, p95 %s, p99 %s, max %s, std dev %s; throughput %s\n",
                full.passed, full.failed, full.total, ms(full.wall_ms).c_str(), full.asserts_passed,
                full.asserts_passed + full.asserts_failed, bytes(full.bytes_received).c_str(), ms(full.avg_ms).c_str(),
                ms(full.min_ms).c_str(), ms(full.p50_ms).c_str(), ms(full.p90_ms).c_str(), ms(full.p95_ms).c_str(),
                ms(full.p99_ms).c_str(), ms(full.max_ms).c_str(), sd.c_str(), tput.c_str());
  // Newman's averages: where the time goes (DNS, connect and TLS over the new connections).
  std::string avg;
  if (full.total > full.stopped_steps + full.skipped) {
    avg = "average first byte (TTFB) " + ms(full.avg_ttfb_ms);
    if (full.new_connections) {
      avg += ", DNS " + ms(full.avg_dns_ms) + ", connect " + ms(full.avg_connect_ms);
      if (full.avg_tls_ms > 0) avg += ", TLS " + ms(full.avg_tls_ms);
      avg += " (" + std::to_string(full.new_connections) + " new connection" + (full.new_connections == 1 ? ")" : "s)");
    } else {
      avg += "; every connection reused";
    }
    // watt's setup before the first I/O (Newman's "prepare"): not in the response times.
    if (full.avg_prep_ms > 0) avg += "; setup " + ms(full.avg_prep_ms) + " (not in the times)";
    avg += '\n';
    // Scripts and token requests run outside the response time (as in Newman): their own line.
    if (full.scripted)
      avg += "scripts (not in response time): " + std::to_string(full.scripted) + " request" +
             (full.scripted == 1 ? "" : "s") + ", avg " + ms(full.avg_script_ms) + ", max " + ms(full.max_script_ms) +
             ", total " + ms(full.script_sum_ms) + "\n";
  }
  // Newman's failure table: every failed step once more (not with --quiet: the causes say it),
  // in iteration order (parallel workers finish out of order).
  std::vector<const StepResult*> failed;
  for (const auto& r : steps)
    if (r->failed()) failed.push_back(r.get());
  const size_t nfail = failed.size();
  std::stable_sort(failed.begin(), failed.end(), [](const StepResult* a, const StepResult* b) {
    return a->iteration != b->iteration ? a->iteration < b->iteration : a->seq < b->seq;
  });
  if (nfail && !quiet) {
    const size_t shown = std::min<size_t>(nfail, 100);
    const size_t num_w = std::to_string(shown).size() + 3;  // "  100."
    *out += "\n" + pad_right("  #", num_w) + " " + pad_right("failure", 42) + " detail\n";
    size_t listed = 0;
    for (const StepResult* r : failed) {
      if (++listed > 100) {
        *out += "  ... and " + std::to_string(nfail - 100) + " more (see the reports)\n";
        break;
      }
      std::string where = label(*r) + (many ? " (iteration " + std::to_string(r->iteration + 1) + ")" : "");
      *out += pad_right("  " + std::to_string(listed) + ".", num_w) + " " + pad_cols(where, 42) + " " + r->reason() + "\n";
    }
  }
  // Failures by cause (Postman's performance view): one line per kind of failure, with the
  // lowest - highest failing iteration and how many iterations failed.
  if (nfail) {
    const auto groups = group_failures(steps);
    if (groups.size() < nfail || quiet) {
      *out += "\nfailures by cause\n";
      size_t n = 0;
      for (const auto& g : groups) {
        if (++n > 20) {
          *out += "  ... and " + std::to_string(groups.size() - 20) + " more causes\n";
          break;
        }
        const std::string where = g.name.empty() ? g.rel : g.name;
        std::string its;
        if (many) {
          its = g.last_iteration != g.first_iteration ? "iterations " + std::to_string(g.first_iteration + 1) + "-" +
                                                            std::to_string(g.last_iteration + 1)
                                                      : "iteration " + std::to_string(g.first_iteration + 1);
          its = "  (" + its + "; " + std::to_string(g.iterations) + " of " + std::to_string(meta.iterations) + " iterations)";
        }
        *out += pad_right("  " + std::to_string(g.count) + "x", 9) + pad_cols(where, 28) + " " + g.message + its + "\n";
        if (g.unresolved) {
          std::string names;
          for (const auto& u : g.unresolved_names) names += (names.empty() ? "{{" : ", {{") + u + "}}";
          *out += std::string(9 + 28 + 1, ' ') + "! " + names + " unresolved in " + std::to_string(g.unresolved) + " of " +
                  std::to_string(g.count) + "\n";
        }
      }
    }
  }
  *out += line;
  *out += avg;
  bool thresholds_ok = true;
  if (!thresholds.empty()) {
    *out += "thresholds\n";
    for (const auto& t : check_thresholds(thresholds, full)) {
      thresholds_ok = thresholds_ok && t.pass;
      *out += std::string(t.pass ? "  ok    " : "  FAIL  ") + pad_right(t.t.text, 18) + " actual " + t.actual_text + "\n";
    }
  }
  bool ok = true;
  if (!junit.empty()) ok = write_file(junit, to_junit(meta, full, steps), out) && ok;
  if (!json.empty()) ok = write_file(json, to_json(meta, full, steps), out) && ok;
  if (!html.empty()) ok = write_file(html, to_html(meta, full, steps), out) && ok;
  if (!ok) return 2;
  return full.failed || full.errors || !thresholds_ok ? 1 : 0;
}

}  // namespace watt::app::runner
