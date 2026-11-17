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
};

ResponsePanel* make_response_view(wxWindow* parent);

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
};

SidebarPanel* make_sidebar(wxWindow* parent, SidebarHost* host);

}  // namespace watt::app
