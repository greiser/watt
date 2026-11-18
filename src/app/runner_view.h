#pragma once
// Collection runner window (Postman's Collection Runner, Bruno's Runner): the requests of a
// folder with checkboxes and order, iterations, delay, stop-on-failure; a live results table
// (request, status, total ms, assert pass / fail), a timing breakdown + asserts + captures for
// the selected row, a summary (passed / failed / total time / p50 / p95 / throughput), Stop, a
// per-request table and an over-time chart (load mode: parallel workers, duration, ramp-up), the
// run's environment, scripts (asked once to trust the workspace), and export to JSON, JUnit XML
// and HTML. The engine (runner_engine.h) runs on a worker thread against a copy of
// the workspace; the UI thread only copies the workspace, asks for consent once up front and
// draws batched results. Run, Run again and Rerun failed read the folder again first (on a
// worker), and an open runner follows changes on disk (its own WorkspaceWatcher): it always
// sends the requests as saved, in the order of run-order.json (as `watt --run` does), and
// says when the plan changed since the run shown. UI thread only.
#include <filesystem>
#include <functional>
#include <memory>

#include "core/grpc_client.h"
#include "core/model.h"

class wxWindow;

namespace watt::app {

// Opens the runner for folder (empty or the workspace root: every request; a .req.json file:
// that request), or raises the one already open for it. workspace() returns the live workspace
// (null while it is still opening) and is read when the window opens and when Run is pressed;
// captured values are merged back into it after a run when "Keep variable values" is on.
// The runner is a tab of parent's wxAuiNotebook (the main window's request tabs).
// select = false (a restored session): the tab is added behind the current one.
void open_runner(wxWindow* parent, std::function<Workspace*()> workspace, const std::filesystem::path& folder,
                 std::function<std::shared_ptr<GrpcClient>()> grpc, bool select = true);
// When `page` is a runner tab: true, with *folder the folder it was opened for (as given to
// open_runner). The session keeps it to open the runner again after a restart.
bool runner_folder(wxWindow* page, std::filesystem::path* folder);
// When `page` (a notebook page) is a runner tab: starts a run (Ctrl+Enter; failed_only: Ctrl+
// Shift+Enter, the failed requests again) and returns true.
bool runner_shortcut(wxWindow* page, bool failed_only);

}  // namespace watt::app
