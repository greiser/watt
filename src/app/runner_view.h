#pragma once
// Collection runner window (Postman's Collection Runner, Bruno's Runner): the requests of a
// folder with checkboxes and order, iterations, delay, stop-on-failure; a live results table
// (request, status, total ms, assert pass / fail), a timing breakdown + asserts + captures for
// the selected row, a summary (passed / failed / total time / p50 / p95 / throughput), Stop, a
// per-request table and an over-time chart (load mode: parallel workers, duration, ramp-up), the
// run's environment, scripts (asked once to trust the workspace), and export to JSON, JUnit XML
// and HTML. The engine (runner_engine.h) runs on a worker thread against a copy of
// the workspace; the UI thread only copies the workspace, asks for consent once up front and
// draws batched results. UI thread only.
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
void open_runner(wxWindow* parent, std::function<Workspace*()> workspace, const std::filesystem::path& folder,
                 std::function<std::shared_ptr<GrpcClient>()> grpc);
// When `page` (a notebook page) is a runner tab: starts a run (Ctrl+Enter; failed_only: Ctrl+
// Shift+Enter, the failed requests again) and returns true.
bool runner_shortcut(wxWindow* page, bool failed_only);

}  // namespace watt::app
