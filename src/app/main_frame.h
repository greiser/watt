#pragma once
// Main window: sidebar (left AUI pane) + wxAuiNotebook of RequestTabs (center). Owns the
// workspace (opened on a background thread) and the shared gRPC engine. Keyboard:
//   Ctrl+Enter send   Ctrl+T new HTTP tab   Ctrl+Shift+T new gRPC tab   Ctrl+W close tab
//   Ctrl+L focus URL  Ctrl+S save           Ctrl+P quick open           Ctrl+(Shift+)Tab next/prev tab
//   Ctrl+Shift+F focus the sidebar filter
#include <wx/aui/aui.h>
#include <wx/frame.h>
#include <wx/timer.h>

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "app/panels.h"
#include "app/request_tab.h"  // TabContext
#include "core/timing.h"

namespace watt::app {

class RequestTab;

// Start-up measurement (implemented in app.cpp). Off unless watt runs with
// --trace-startup=<file> (or WATT_STARTUP_TRACE=<file>) or --first-request=<url>.
namespace startup {
// Milliseconds since the kernel created this process (includes DLL loading and static init).
double since_launch_ms();
// Records a named milestone (no-op when tracing is off). UI thread or any thread.
void mark(const char* what);
// A request tab calls this right after a final response has been shown and painted. The first
// call records "first response shown"; with --first-request it writes the report and, with
// --bench-exit, quits.
void response_shown();
// True while tracing and no response has been shown yet (callers skip the work otherwise).
bool awaiting_response();
// --first-request=<url>: the URL to send from the first tab as soon as the window is up (empty: none).
const std::string& first_request_url();
// --self-test=<file>: where MainFrame::self_test writes its result (empty: no self-test).
const std::wstring& self_test_file();
// Appends the milestones so far to the trace file (also done on exit and on the first response).
void flush();
}  // namespace startup

class MainFrame : public wxFrame, public SidebarHost {
 public:
  explicit MainFrame(std::filesystem::path workspace_dir);
  ~MainFrame() override;

  // The frame's "send the active tab" command (Ctrl+Enter). A control of a hidden tab that still
  // has the keyboard focus posts it instead of sending its own tab.
  static constexpr int kSendCommand = wxID_HIGHEST + 100;

  // Process-wide gRPC engine, created on first use (thread-safe). Never at start-up: an HTTP-only
  // session never starts the gRPC runtime or loads its (delay-loaded) DLLs.
  static std::shared_ptr<GrpcClient> grpc();

  // Starts opening the workspace on a background thread (the constructor calls it). Its result
  // reaches the sidebar no earlier than the first paint, so it never delays the window.
  void open_workspace_async();

  // Shortens "watt - <absolute workspace path>" to "<folder> - watt" (whoever sets it).
  void SetTitle(const wxString& title) override;

  // SidebarHost
  void open_request(const std::filesystem::path& file, bool preview) override;
  void open_history(const HistoryEntry& entry) override;
  void new_request(RequestKind kind, const std::filesystem::path& folder) override;
  void file_moved(const std::filesystem::path& from, const std::filesystem::path& to) override;
  void environment_changed() override;
  void folder_settings_changed() override { vars_changed(); }
  void run_folder(const std::filesystem::path& folder) override;

 private:
  // lazy: not selected, editor built when first shown; at: insert position (-1: append).
  // park: a restored tab (RequestTab::park_grpc).
  RequestTab* add_tab(SavedRequest r, bool preview, bool lazy = false, int at = -1, bool park = false);
  RequestTab* current_tab() const;
  TabContext tab_context();
  void prebuild_later(int ms);
  void prebuild_step();
  int tab_index(RequestTab* t) const;
  void update_title(RequestTab* t);
  void hook_tab(RequestTab* t);  // title and session callbacks
  bool close_tab(int idx);   // false if the user cancelled
  void save_current();
  void vars_changed();
  void quick_open();
  void workspace_opened(std::unique_ptr<Workspace> ws, const std::string& err);
  bool when_workspace(std::function<void()> fn);  // see TabContext::when_workspace
  bool save_session();     // false if a draft or the session file could not be written
  void restore_session();
 public:
  struct Session;          // a parsed .watt/session.json (main_frame.cpp)
  struct SessionSnapshot;  // what save_session writes (main_frame.cpp)
 private:
  std::shared_ptr<SessionSnapshot> snapshot_session();  // null without a workspace
  void session_changed();  // writes the session ~1.5 s later, off the UI thread
  void apply_session(Session& s);
  void on_close(wxCloseEvent& e);
  void first_idle();  // once, after the window has been shown and painted
  void self_test();
  int sidebar_width() const;
  void restore_geometry();
  void save_geometry();   // --self-test: keystrokes typed after start-up / Ctrl+T must all arrive

  std::filesystem::path ws_dir_;
  std::unique_ptr<Workspace> ws_;
  wxAuiManager aui_;
  wxAuiNotebook* nb_ = nullptr;
  SidebarPanel* sidebar_ = nullptr;
  bool painted_ = false;          // first_idle() has run
  bool ws_pending_ = false;       // the workspace arrived before the first paint
  std::unique_ptr<Workspace> pending_ws_;
  std::string pending_err_;
  bool ws_failed_ = false;        // the workspace could not be opened
  std::vector<std::function<void()>> ws_waiters_;
  bool quick_open_pending_ = false;
  bool quick_open_shown_ = false;
  Clock::time_point ws_opened_at_{};
  RequestTab* spare_[2] = {nullptr, nullptr};  // ready new tabs (HTTP, gRPC), hidden children of spare_park_
  wxWindow* spare_park_ = nullptr;              // hidden parent for spares, outside the notebook
  bool want_grpc_spare_ = false;  // gRPC is in use: keep a spare gRPC tab too
  wxTimer* prebuild_timer_ = nullptr;
  wxTimer* session_timer_ = nullptr;
  bool session_due_ = false;      // a session write is scheduled
  Clock::time_point session_first_change_{};
  bool closing_ = false;          // on_close has written the final session
  bool restoring_ = false;        // apply_session() is adding / selecting tabs
  std::set<std::filesystem::path> restored_roots_;  // sessions already restored in this process
  wxRect normal_rect_;            // last un-maximized window rectangle (saved on close)
};

}  // namespace watt::app
