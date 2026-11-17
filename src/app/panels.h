#pragma once
// Contract between the shell (main_frame, request_tab) and the replaceable panels. A panel piece
// implements one interface in its own file (http_editor.cpp, grpc_editor.cpp, response_view.cpp,
// sidebar.cpp) and never needs to touch main_frame or request_tab.
//
// Ownership: every factory returns a panel whose window() is a child of `parent`; the wx parent
// owns and destroys it (do not delete the returned pointer). The usual pattern is
//   class MyPanel : public wxPanel, public watt::app::ResponsePanel { wxWindow* window() override { return this; } };
//
// Threading: every method here is called on the UI thread, and every host method must be called
// on the UI thread. The shell does all network and file work (send, schema load, history) on
// per-tab worker threads; panels never block. Large payloads arrive as shared_ptr<const ...> so
// they are never copied on the way to the UI: a panel may keep the pointer and hand it to its own
// worker thread (e.g. pretty-printing a 10 MB body) without copying.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "core/grpc_client.h"
#include "core/http_client.h"
#include "core/model.h"
#include "core/script.h"

class wxWindow;

namespace watt::app {

// ------------------------------------------------------------------ editors

// Implemented by the request tab; given to both editors. UI thread only.
class EditorHost {
 public:
  virtual ~EditorHost() = default;
  // Send the request as currently shown (the shell calls store() first). Ctrl+Enter does the same.
  virtual void send() = 0;
  // Abort the request in flight (the editor's Send button turns into Cancel, see set_in_flight).
  virtual void cancel() = 0;
  // The user edited something: the tab marks itself dirty and refreshes its title from store().
  virtual void changed() = 0;
  // Switch this tab to the other editor kind (e.g. a "HTTP | gRPC" toggle). The shell stores the
  // current editor, flips SavedRequest::kind and load()s the other editor.
  virtual void switch_kind(RequestKind kind) = 0;
  // Workspace for {{var}} highlighting / completion and the environment list. May be null until
  // the workspace has been opened (in the background at start-up). Do not keep the pointer.
  virtual const Workspace* workspace() const = 0;
  // The file whose folders this request inherits from (folder.json auth, headers, vars, asserts,
  // scripts): its own file; for a history snapshot the saved request it came from; for an unsaved
  // request the folder its first save goes to. Empty: the collection only.
  virtual std::filesystem::path inherit_file() const { return {}; }

  // gRPC only ---------------------------------------------------------------
  // Load the schema for r (reflection when r.proto_files is empty, else .proto / .protoset files)
  // on the worker; the result arrives in GrpcEditorPanel::set_schema().
  virtual void load_schema(const SavedRequest& r) = 0;
  // Start DNS + connect for a target as soon as the user has typed it (cheap, non-blocking).
  virtual void preconnect(const GrpcTarget& target) = 0;
  // Interactive streams (GrpcEditorPanel::interactive() == true): send one more JSON message on
  // the call in flight / half-close it. False with *error when no call is open or the JSON is bad.
  virtual bool stream_send(const std::string& json, std::string* error) = 0;
  virtual void stream_close() = 0;
  // The shared gRPC engine (for example_request() / describe() once a schema is loaded). Never null.
  virtual const GrpcClient& grpc_client() const = 0;
};

// Common part of both editors.
class RequestEditorPanel {
 public:
  virtual ~RequestEditorPanel() = default;
  virtual wxWindow* window() = 0;
  // Show r. Called when a tab opens, after switch_kind, and after a file is reloaded.
  virtual void load(const SavedRequest& r) = 0;
  // Write what the user edited back into r (fields this editor does not own stay untouched).
  virtual void store(SavedRequest& r) const = 0;
  // Only the small fields the tab title needs (name, http.method, http.url, grpc_target,
  // grpc_method), never the body: called on every changed(), so it must be cheap.
  virtual void store_head(SavedRequest& r) const { store(r); }
  // While a request is in flight the Send button shows Cancel (host->cancel()).
  virtual void set_in_flight(bool in_flight) = 0;
  // Ctrl+L: focus and select the URL / target field.
  virtual void focus_url() = 0;
  // The workspace opened, or its environment / variables changed: refresh what shows resolved
  // {{variables}} (the HTTP editor's "Sent as" line).
  virtual void vars_changed() {}
};

class HttpEditorPanel : public RequestEditorPanel {};

class GrpcEditorPanel : public RequestEditorPanel {
 public:
  // Result of EditorHost::load_schema() (ok == false carries the error).
  virtual void set_schema(std::shared_ptr<const GrpcSchema> schema) = 0;
  // true: a client / bidi stream is driven by hand. The shell opens the call and sends the request
  // body as the first message (unless it is empty), then leaves the send side open for
  // EditorHost::stream_send() / stream_close(). false: the body is sent whole (a JSON array of
  // messages for client / bidi streams) and the send side closed.
  virtual bool interactive() const { return false; }
};

HttpEditorPanel* make_http_editor(wxWindow* parent, EditorHost* host);
GrpcEditorPanel* make_grpc_editor(wxWindow* parent, EditorHost* host);

// ------------------------------------------------------------------ response

// What was actually sent, for the response's Timeline / request view.
struct SentHttp {
  HttpRequest request;  // after {{var}} expansion and form building
  // Results of the request's scripts (null when it has none or scripts are not trusted):
  // logs, tests (name/pass/message), variables set, error with line:col, elapsed_us.
  std::shared_ptr<const ScriptRun> pre_script;
  std::shared_ptr<const ScriptRun> post_script;
  bool scripts_skipped_untrusted = false;  // the request has scripts but the user did not trust them
  // OAuth 2.0 (own or inherited auth): the token step before the request, on the worker. Its time
  // is inside timing.queue_ms; the response view shows it as its own "Auth / token" phase.
  struct TokenStep {
    bool ok = false;
    bool from_cache = false;
    bool refreshed = false;
    double ms = 0;
    std::string grant;      // "client credentials", ...
    std::string token_url;  // expanded
    std::string from;       // where the auth is set: "Request", "api", "Collection"
    std::string error;
  };
  std::shared_ptr<const TokenStep> token;
};

class ResponsePanel {
 public:
  virtual ~ResponsePanel() = default;
  virtual wxWindow* window() = 0;
  // Empty state (new tab, kind switched).
  virtual void clear() = 0;
  // A request has started (show a spinner / "Sending..." and a running clock if desired).
  virtual void show_sending(RequestKind kind) = 0;
  // HTTP download progress, at most ~20 times a second while a body arrives. Optional.
  virtual void show_progress(uint64_t bytes_received) { (void)bytes_received; }
  // Final HTTP response. resp->timing.queue_ms includes the shell's click-to-engine time, and
  // timing.total_ms is the user-visible total (phase_total(*resp) == resp->timing.total_ms).
  // Transport failures arrive here too with resp->ok == false and resp->error set.
  virtual void show_http(std::shared_ptr<const HttpResponse> resp, std::shared_ptr<const SentHttp> sent) = 0;
  // gRPC messages received so far on the call in flight, in order, batched (one call per UI
  // turn, not one per message, so a 10k message stream stays responsive).
  virtual void append_grpc_messages(const std::vector<GrpcMessageEvent>& batch) = 0;
  // Final gRPC result. resp->messages holds every message, including those already appended;
  // a panel that appended live should only add messages past the count it already shows.
  virtual void show_grpc(std::shared_ptr<const GrpcResponse> resp) = 0;
  // The user cancelled (the late engine result, if any, is dropped by the shell).
  virtual void show_cancelled(double after_ms) = 0;
  // Could not send at all (bad URL, blocked upload file, no method selected, ...).
  virtual void show_error(const std::string& message) = 0;
  // The next restored show_http / show_grpc (one without show_sending) is a history entry's
  // response received at `when_iso` (UTC ISO); empty: a tab's response from an earlier session.
  virtual void set_history_when(const std::string& when_iso) { (void)when_iso; }
};

ResponsePanel* make_response_view(wxWindow* parent);

// A gRPC request's name as tabs and history show it. A name of the form "<Service>.<Method>" for
// the same service as `method` (what watt names a new gRPC request) whose method is not the one
// called any more (Say switched to Count) reads "Echo.Say -> Count": the row and the tab
// never claim a call that is not the one sent. Any other name is shown as it is.
inline std::string grpc_display_name(const std::string& name, const std::string& method) {
  const size_t slash = method.rfind('/');
  if (name.empty() || slash == std::string::npos) return name;
  const std::string m = method.substr(slash + 1);
  std::string svc = method.substr(0, slash);
  if (const size_t dot = svc.rfind('.'); dot != std::string::npos) svc.erase(0, dot + 1);
  if (m.empty() || svc.empty()) return name;
  // A bare method name (what saving an unnamed gRPC tab names it: "Say") that is not the method
  // called any more reads "Say -> Count" too.
  auto ident = [](const std::string& s) {
    if (s.empty() || s[0] < 'A' || s[0] > 'Z') return false;
    for (char c : s)
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
  };
  if (ident(name)) return name == m || name == svc ? name : name + " \xE2\x86\x92 " + m;
  if (name.size() <= svc.size() + 1 || name.compare(0, svc.size(), svc) != 0 || name[svc.size()] != '.')
    return name;
  const std::string named = name.substr(svc.size() + 1);
  if (named == m || named.find_first_of(" ./") != std::string::npos) return name;
  return name + " \xE2\x86\x92 " + m;
}

// ------------------------------------------------------------------ sidebar

// Implemented by the main frame. UI thread only.
class SidebarHost {
 public:
  virtual ~SidebarHost() = default;
  // Single click (preview = true: reuses an unmodified preview tab, like VS Code) or double
  // click / Enter (preview = false: a permanent tab). Activates the tab if the file is open.
  virtual void open_request(const std::filesystem::path& file, bool preview) = 0;
  // A history entry was chosen: the shell loads its request (Workspace::history_request) in a new tab.
  virtual void open_history(const HistoryEntry& entry) = 0;
  // "New request" in a folder (empty = workspace root): opens an unsaved tab whose first save
  // goes into that folder.
  virtual void new_request(RequestKind kind, const std::filesystem::path& folder) = 0;
  // The sidebar renamed / moved / deleted a file: open tabs follow (from -> to) or close (to empty).
  virtual void file_moved(const std::filesystem::path& from, const std::filesystem::path& to) = 0;
  // The active environment changed (Workspace::active_env already set and saved by the sidebar).
  virtual void environment_changed() = 0;
  // "Run folder": opens the collection runner (runner_view.h) for every request under folder
  // (empty = the whole workspace; a .req.json file runs alone), or raises the one already open.
  virtual void run_folder(const std::filesystem::path& folder) { (void)folder; }
  // A folder's / the collection's settings (folder.json) were saved from the sidebar: open tabs
  // refresh what they inherit.
  virtual void folder_settings_changed() {}
};

class SidebarPanel {
 public:
  virtual ~SidebarPanel() = default;
  virtual wxWindow* window() = 0;
  // The workspace is opened in the background at start-up; null until then. The pointer stays
  // valid until the next set_workspace() call. The sidebar may call ws->tree() (on its own
  // worker with a copy for a full badge scan) and edit ws->active_env / environments().
  virtual void set_workspace(Workspace* ws) = 0;
  // Something on disk changed (a request was saved): rescan.
  virtual void refresh() = 0;
  // A request was saved (r.file is set): update its row / badge without a full rescan.
  virtual void request_saved(const SavedRequest& r) = 0;
  // A request finished and was appended to the workspace history: prepend it to the history list.
  // entry.request holds the request as sent (unexpanded); entry.offset is not set, so
  // open_history() with this entry uses entry.request directly.
  virtual void history_added(const HistoryEntry& entry) = 0;
  // The active tab changed: highlight its file (empty = unsaved request).
  virtual void select_file(const std::filesystem::path& file) = 0;
  // Ctrl+Shift+F / a search box shortcut: focus the sidebar filter. Optional.
  virtual void focus_filter() {}
  // A history snapshot tab (<ws>/.watt/snapshots/...): the saved request it was sent as (by the
  // snapshot map, else by the request id), so it keeps inheriting that request's folder settings.
  // Empty for any other file, or when the origin is unknown.
  virtual std::filesystem::path snapshot_origin(const std::filesystem::path& file, const std::string& id) const {
    (void)file;
    (void)id;
    return {};
  }
};

SidebarPanel* make_sidebar(wxWindow* parent, SidebarHost* host);

}  // namespace watt::app
