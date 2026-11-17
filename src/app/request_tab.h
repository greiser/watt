#pragma once
// One request tab: owns a SavedRequest, the editor for its kind, a response view, and a worker
// thread that runs the engines. The UI thread never waits on the network: the worker posts every
// result back with CallAfter, tagged with a generation number so a cancelled or superseded
// request's late result is dropped. Closing a tab never blocks: the worker is cancelled and left
// to finish on its own (it holds its state by shared_ptr).
#include <wx/panel.h>
#include <wx/timer.h>
#include <wx/weakref.h>

#include <functional>
#include <memory>
#include <string>

#include "app/panels.h"
#include "core/timing.h"

class wxIdleEvent;
class wxChildFocusEvent;
class wxKeyEvent;
class wxSplitterWindow;

namespace watt::app {

struct TabWorker;

// A tab's last response, kept in the session so a restored tab shows it again (as Postman and
// Insomnia do). Bodies over kMaxBytes are not kept.
struct LastResponse {
  static constexpr size_t kMaxBytes = 2u << 20;
  std::shared_ptr<const HttpResponse> http;
  std::shared_ptr<const SentHttp> sent;
  std::shared_ptr<const GrpcResponse> grpc;
  // Set when it comes from a history entry (UTC ISO time it was received): shown as "last run
  // at ...", not as a tab restored from an earlier session. Not stored.
  std::string when_iso;
  explicit operator bool() const { return http || grpc; }
};
// "" when r holds nothing or is too large to keep.
std::string last_response_to_json(const LastResponse& r);
// Any thread. `req` is the restored request (what the HTTP response was sent for).
LastResponse last_response_from_json(const std::string& json, const SavedRequest& req);

// Speculative UI work (building the views of tabs not used yet) runs only after this long without
// any keyboard or mouse input: a typist's pauses are shorter, so it never lands between keys.
constexpr int kQuietMs = 1500;
constexpr int kQuietShortMs = 200;  // building to show a stored last response
// Milliseconds still to wait until quiet_ms have passed since the last input (0: quiet now).
int input_quiet_wait(int quiet_ms);

// A loaded gRPC schema, kept in the session (method list, kinds, types, templates). "" when
// s is not ok; null when json holds no method.
std::string schema_to_json(const GrpcSchema& s);
std::shared_ptr<const GrpcSchema> schema_from_json(const std::string& json);

// What a tab needs from the frame.
struct TabContext {
  std::function<Workspace*()> workspace;          // may return null (workspace still opening)
  std::function<std::shared_ptr<GrpcClient>()> grpc;  // shared engine (channel + schema cache), created once
  std::function<void(const HistoryEntry&)> history_added;
  // Keeps fn until the workspace has opened (then runs it) and returns true; false (fn dropped)
  // if the workspace is already open or failed to open. A send during start-up waits on it.
  std::function<bool(std::function<void()>)> when_workspace;
  // The saved request a history snapshot came from (SidebarPanel::snapshot_origin); may be unset.
  std::function<std::filesystem::path(const SavedRequest&)> snapshot_origin;
};

class RequestTab : public wxPanel, public EditorHost {
 public:
  // lazy: build the editor only when the tab is first shown (realize()): for restored tabs.
  RequestTab(wxWindow* parent, TabContext ctx, SavedRequest req, bool preview = false, bool lazy = false);
  ~RequestTab() override;

  // Called whenever title() or dirty() may have changed.
  std::function<void(RequestTab*)> on_title_changed;
  // Called after a final response (or error) has been shown.
  std::function<void(RequestTab*)> on_response;

  std::wstring title() const;        // "GET echo", "Say" ...
  bool dirty() const { return dirty_; }
  bool preview() const { return preview_; }
  void make_permanent();
  bool in_flight() const { return in_flight_; }
  const SavedRequest& request() const;  // after storing the editor's state (copies the body: not per frame)
  const std::filesystem::path& file() const { return req_.file; }
  // Never edited, never saved, nothing typed: an empty start tab that can be reused.
  bool pristine() const { return !dirty_ && req_.file.empty() && req_.http.url.empty() && req_.grpc_method.empty(); }
  // Replace the shown request (reload, preview reuse). Cancels anything in flight.
  void set_request(SavedRequest req, bool preview);
  // Ctrl+S: store + Workspace::save_request. False (with *err) on failure.
  bool save(Workspace& ws, std::string* err);
  void focus_url();
  void vars_changed() {
    if (RequestEditorPanel* ed = editor()) ed->vars_changed();
  }
  // Builds the editor of a lazy tab (the frame calls it when the page is shown). Idempotent.
  void realize();
  // Builds the response view now instead of shortly after the tab is first shown (the start tab
  // does this before the window shows, so its first paint is complete and a first send never
  // waits for the ~70 ms build).
  void build_response_view() { response(); }
  bool realized() const { return split_ != nullptr; }
  bool has_response_view() const { return resp_ != nullptr; }
  RequestKind request_kind() const { return req_.kind; }
  // A restored draft: show the unsaved-changes mark.
  void mark_dirty();
  // Creates this tab's curl handle on its worker now (after start-up), not on the first send.
  void warm_up();
  void file_moved(const std::filesystem::path& to);
  // The last response shown (for the session), and one restored from it (shown as soon as the
  // response view exists; replaced by the next send).
  const LastResponse& last_response() const { return last_; }
  void restore_response(LastResponse r);
  // A tab restored from the session: its gRPC editor does not start the gRPC engine, reflect or
  // preconnect when it is built (that is ~250 ms of CPU and 20 threads right as the window first
  // shows). All of that is held until the first sign that the user works in this tab: a key or a
  // click in it, Ctrl+Enter, a schema reload, or switching to it (wake_grpc). Call before realize().
  void park_grpc() { grpc_parked_ = true; }
  // A parked gRPC tab: the schema it had in the last session (its origin must match), shown by
  // the editor instead of "not loaded" until the tab wakes and asks the server again. Call right
  // after creating the tab, before the first event-loop turn.
  void restore_schema(std::shared_ptr<const GrpcSchema> s);
  // The schema to keep in the session for this tab: the last one loaded for its server or files,
  // if it has the tab's method (null otherwise).
  std::shared_ptr<const GrpcSchema> session_schema() const;
  void wake_grpc();
  bool grpc_parked() const { return grpc_parked_; }
  // Where the first save of an unsaved request goes (empty: Workspace::save_request's default).
  void set_save_folder(const std::filesystem::path& folder) { save_folder_ = folder; }
  const std::filesystem::path& save_folder() const { return save_folder_; }
  // The name the Save dialog offers for an unsaved request: the URL's last path segment ("echo"
  // for {{base}}/echo?x=1) or the gRPC method, never a {{variable}}.
  std::string suggested_name() const;
  // The Save dialog's choice: the next save() writes <folder>/<name>.req.json (a free name).
  void set_save_target(const std::string& name, const std::filesystem::path& folder) {
    save_name_ = name;
    save_folder_ = folder;
  }

  // EditorHost
  void send() override;
  void cancel() override;
  void changed() override;
  void switch_kind(RequestKind kind) override;
  const Workspace* workspace() const override;
  std::filesystem::path inherit_file() const override;
  // req with file = inherit_file(): what the workspace's inheritance (folder.json) is computed on.
  SavedRequest for_inheritance(const SavedRequest& req) const;
  void load_schema(const SavedRequest& r) override;
  void preconnect(const GrpcTarget& target) override;
  bool stream_send(const std::string& json, std::string* error) override;
  void stream_close() override;
  const GrpcClient& grpc_client() const override { return *ctx_.grpc(); }

 private:
  friend struct TabWorker;
  RequestEditorPanel* editor() const;
  RequestEditorPanel* ensure_editor(RequestKind kind);
  void show_editor(RequestKind kind);
  void store() const;
  void show_last();  // last_ into the response view (built)
  ResponsePanel* response();  // creates the response view on first use
  void on_idle(wxIdleEvent& e);
  void on_resp_timer();
  void center_sash();
  void send_now(Clock::time_point click);
  void restore_focus();
  void on_resp_focus(wxChildFocusEvent& e);
  void finish(uint64_t gen);
  void send_http(Clock::time_point click);
  void send_grpc(Clock::time_point click);
  void load_schema_now(const SavedRequest& r);
  void hold_schema_ui();
  void on_tab_key(wxKeyEvent& e);
  void on_tab_child_focus(wxChildFocusEvent& e);

  // worker -> UI (only for the current generation)
  void on_http_done(uint64_t gen, std::shared_ptr<const HttpResponse> r, std::shared_ptr<const SentHttp> sent);
  void on_grpc_messages(uint64_t gen);
  void on_grpc_done(uint64_t gen, std::shared_ptr<const GrpcResponse> r);
  void on_error(uint64_t gen, const std::string& msg);

  TabContext ctx_;
  mutable SavedRequest req_;
  bool dirty_ = false;
  bool preview_ = false;
  bool loading_ = false;   // suppress changed() while load()ing an editor
  bool user_sash_ = false;  // the user dragged the sash: stop keeping it at 50%
  bool layout_pending_ = false;  // resized while hidden: Layout() when shown again
  std::filesystem::path save_folder_;
  std::string save_name_;

  wxSplitterWindow* split_ = nullptr;
  HttpEditorPanel* http_ = nullptr;
  GrpcEditorPanel* grpc_ = nullptr;
  ResponsePanel* resp_ = nullptr;      // null until response() (see the constructor)
  wxPanel* resp_ph_ = nullptr;         // stands in for resp_ until then
  wxWeakRef<wxWindow> focus_keep_;     // focus to keep while the response view is being built
  Clock::time_point focus_guard_until_{};

  static constexpr int kResponseViewDelayMs = 300;
  wxTimer resp_timer_;                 // builds the response view shortly after the first show
  bool pending_send_ = false;          // send() waiting for the workspace (when_workspace)

  LastResponse last_;
  bool last_pending_ = false;                    // last_ restored, not shown yet
  bool grpc_parked_ = false;                     // see park_grpc()
  std::unique_ptr<SavedRequest> parked_schema_;  // the schema load held while parked
  std::shared_ptr<const GrpcSchema> restored_schema_;  // see restore_schema()
  std::string restored_origin_;
  wxWeakRef<wxWindow> held_status_;              // the editor's "Reflecting ..." line, hidden meanwhile
  bool timer_hook_ = false;

  std::shared_ptr<TabWorker> worker_;
  uint64_t gen_ = 0;
  bool in_flight_ = false;
  Clock::time_point sent_at_;
};

}  // namespace watt::app
