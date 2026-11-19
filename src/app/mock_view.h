#pragma once
// Mock server tab (Bruno's Mock Servers, Insomnia's Mocks): serves the saved examples of a
// folder's or the collection's HTTP requests from a local listener (core/mock_server.h). The tab
// shows the base URL with Copy, the routes, and a live request log (method, path, matched route,
// example, status, time served). While it runs, {{mock}} (Workspace::runtime_vars) is its base
// URL, so requests can be pointed at it. Routes are read on a worker thread; the listener runs on
// its own threads and log rows reach the tab through CallAfter. UI thread only.
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "core/mock_server.h"
#include "core/model.h"

class wxWindow;

namespace watt::app {

// Opens the mock server tab for folder (empty or the workspace root: the whole collection) and
// starts it, or raises the one already open for it. workspace() returns the live workspace;
// vars_changed() is called after {{mock}} changed. The tab goes into parent's wxAuiNotebook.
// start = false opens it stopped (one click on Start); port (non-zero) is put in its Port field
// (a restart keeps the address {{mock}} had); select = false leaves the current tab selected;
// at (>= 0) is its place in the tab strip (a restart keeps the order), else it goes last.
void open_mock(wxWindow* parent, std::function<Workspace*()> workspace, const std::filesystem::path& folder,
               std::function<void()> vars_changed, bool start = true, unsigned port = 0, bool select = true,
               int at = -1);

// True when w is a mock server tab: its folder (empty: the collection), the port to reopen it on
// and whether it should come back running (it runs, on a loopback address). For the session.
bool mock_tab_info(wxWindow* w, std::filesystem::path* folder, unsigned* port, bool* running);

// An example was saved: every running mock tab reads its routes again (on a worker).
void mock_examples_changed();

// The routes of every saved HTTP request under folder (empty: the whole workspace): method, URL
// path after {{vars}} resolve, and the request's saved examples (newest first; at most 20 per
// request). Reads the disk: worker thread only.
std::vector<mock::Route> collect_mock_routes(const Workspace& ws, const std::filesystem::path& folder,
                                             std::vector<std::string>* errors = nullptr);

}  // namespace watt::app
