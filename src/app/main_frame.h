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
#include <utility>
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
  void open_loaded(SavedRequest r, bool preview) override;
  void open_history(const HistoryEntry& entry) override;
  void new_request(RequestKind kind, const std::filesystem::path& folder) override;
  void file_moved(const std::filesystem::path& from, const std::filesystem::path& to) override;
  void environment_changed() override;
  void folder_settings_changed() override { vars_changed(); }
  void run_folder(const std::filesystem::path& folder) override;
  void start_mock(const std::filesystem::path& folder) override;

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
  // After the window was closed (hidden): one tab destroyed per turn of the message loop, then the
  // frame, so no single UI step tears down every editor and response view at once.
  void close_slice();
  wxTimer* close_timer_ = nullptr;  // drives close_slice()
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
  RequestTab* spare_[3] = {nullptr, nullptr, nullptr};  // ready new tabs (HTTP, gRPC, WebSocket), hidden children of spare_park_
  static int spare_slot(RequestKind k) { return k == RequestKind::Http ? 0 : k == RequestKind::Grpc ? 1 : 2; }
  wxWindow* spare_park_ = nullptr;              // hidden parent for spares, outside the notebook
  std::vector<wxWindow*> retired_;  // tabs replaced by a spare, parked hidden, destroyed in a quiet moment
  std::vector<RequestTab*> closed_;  // closed tabs, parked hidden, torn down in slices (prebuild_step)
  // A closed HTTP tab: recycled as the spare when it is a pristine built tab (Ctrl+T, Ctrl+W),
  // else parked in closed_ (at most kMaxParked: the oldest are destroyed at once beyond that).
  void park_closed(RequestTab* t);
  // One teardown slice of closed_ / surplus retired_ tabs, run by reap_timer_ whether or not
  // the user is typing (parked tabs hold ~76 USER objects each). False: nothing left.
  bool reap_slice();
  void reap_later();
  wxTimer* reap_timer_ = nullptr;
  // Closes the given pages (indices) in one batch: one save question for the dirty ones.
  void close_tabs(std::vector<int> idx);
  void tab_menu(int idx);  // the tab strip's right-click menu
  RequestTab* replace_with_spare(size_t idx, SavedRequest& r, bool preview, bool defer = false);
  // A swap replace_with_spare(defer) loaded but has not shown yet (finish_swap does, from a posted
  // kSwapMsg, or first thing in any call that looks at or changes the pages).
  struct PendingSwap {
    RequestTab* t = nullptr;   // the loaded spare, hidden in spare_park_
    wxWindow* old = nullptr;   // the page it replaces
    bool preview = false;
  };
  PendingSwap swap_;
  void finish_swap();
  static constexpr unsigned kSwapMsg = 0x8000 + 0x47;  // WM_APP + 0x47
  WXLRESULT MSWWindowProc(WXUINT msg, WXWPARAM wp, WXLPARAM lp) override;
  void open_impl(const std::filesystem::path& file, SavedRequest* loaded, bool preview);
  bool want_grpc_spare_ = false;  // gRPC is in use: keep a spare gRPC tab too
  wxTimer* prebuild_timer_ = nullptr;
  // [0] HTTP, [1] gRPC: a tab of that kind was opened (or, HTTP, a spare built): refill the spare soon
  bool spare_built_[2] = {false, false};
  // HTTP / gRPC spares beyond spare_[k]: a second finished one (so tabs opened in a burst never
  // build a tab inline), and the one being built in idle slices (shell + editor, response view,
  // layout), never handed out before it is finished.
  RequestTab* reserve_[2] = {nullptr, nullptr};
  RequestTab* building_[2] = {nullptr, nullptr};
  bool spare_slice(int k, bool run);  // one slice of spare k's refill; false: nothing to do
  void promote_reserve(RequestKind kind) {  // an empty spare slot takes its finished reserve
    const int k = spare_slot(kind);
    if (k < 2 && !spare_[k] && reserve_[k]) std::swap(spare_[k], reserve_[k]);
  }
  bool spare_ready(RequestTab* t) const;  // built and laid out at the notebook's page size
  wxSize page_size() const;
  wxTimer* session_timer_ = nullptr;
  bool session_due_ = false;      // a session write is scheduled
  Clock::time_point session_first_change_{};
  bool closing_ = false;          // on_close has written the final session
  bool restoring_ = false;        // apply_session() is adding / selecting tabs
  std::set<std::filesystem::path> restored_roots_;  // sessions already restored in this process
  wxRect normal_rect_;            // last un-maximized window rectangle (saved on close)
};

}  // namespace watt::app
