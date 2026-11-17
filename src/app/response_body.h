#pragma once
// Body tab of the response view: Pretty / Raw / Hex over a read-only wxStyledTextCtrl with
// JSON / XML / HTML lexers, folding, Ctrl+F search and copy.
//
// Large bodies: every view of the body is built on the view's own worker thread straight into a
// Scintilla document (ILoader: pretty-printing, hex dumping and Scintilla's line indexing all
// happen off the UI thread); the UI thread only swaps the finished document in. Styling is done
// by Scintilla lazily for the visible lines and in idle time. Bodies up to kSyncBytes are built
// inline so small responses appear in the same UI turn as the status.
#include <wx/panel.h>
#include <wx/timer.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "app/response_format.h"
#include "app/response_tests.h"
#include "core/grpc_client.h"
#include "core/http_client.h"

class wxStyledTextCtrl;
class wxStyledTextEvent;
class wxTextCtrl;
class wxHtmlWindow;
class wxWebView;

namespace watt::app::rv {

class ChipBar;
class BodyWorker;

class BodyView : public wxPanel {
 public:
  explicit BodyView(wxWindow* parent);
  ~BodyView() override;

  // Plain message instead of a body (empty state, errors).
  void show_message(const std::string& text);
  // HTTP response body (kept alive by r, never copied for Raw).
  void set_http(std::shared_ptr<const HttpResponse> r);
  // gRPC: messages are appended live; each gets a "// #i  +offset  size" header line.
  void begin_stream();
  void append_messages(const GrpcMessageEvent* m, size_t n, size_t first_index);
  void append_note(const std::string& text);  // "// ..." line after the messages (e.g. status)
  // The call is over. A unary reply (one message, status OK) is then shown as its plain JSON, and
  // Copy / Save use the messages themselves (one JSON value, or a JSON array for a stream), never
  // the "#n +offset size" annotations of the display.
  void finish_grpc(std::shared_ptr<const GrpcResponse> r);
  // gRPC failure without messages: status line + the decoded google.rpc.Status details (JSON),
  // highlighted and foldable like a message.
  void show_grpc_error(const std::string& status_line, const std::string& details_json);
  void goto_message(size_t i);
  // Selects the JSON value at `path` and scrolls it into view (the Tests tab's failing row).
  // HTTP: in the Pretty view (built first when needed; the lookup runs on the worker for big
  // bodies). gRPC: inside message `message` (0-based; -1 = the first one). When the path is not
  // there the deepest part that is gets selected, and the note says so.
  void reveal_json(std::vector<PathStep> path, long message, const std::string& label);
  size_t messages_shown() const { return msg_pos_.size(); }

  void show_find();
  // A new request is in flight: the body shown is the previous response, drawn dimmed and labelled
  // so it is not read as the new answer. Cleared by the next result (and by set_stale(false)).
  void set_stale(bool stale);

 private:
  // RawWrap: Raw with its lines broken at the pane width on the worker (Scintilla's own wrapping
  // lays out every line of a 10 MB document on the UI thread), used for Wrap on big Raw bodies.
  // Outline: "Fold all" of a very large JSON body, built on the worker (one line per top-level
  // value) instead of hundreds of thousands of Scintilla folds.
  enum Mode { Pretty = 0, Raw = 1, Hex = 2, Preview = 3, RawWrap = 4, Outline = 5, kModes = 6 };
  struct Doc {
    void* doc = nullptr;                // Scintilla document (our reference)
    std::shared_ptr<const void> owner;  // keeps `text` alive
    std::string_view text;              // exactly what the document holds
    int lexer = 0;
    std::string note;
    bool building = false;
    bool lexer_set = false;
    bool prestyled = false;
    bool display_breaks = false;                     // long lines were broken for display
    std::shared_ptr<const std::vector<int>> levels;  // fold levels when prestyled
    std::shared_ptr<const std::vector<int>> line_map;  // Outline: line -> line of the Pretty document
  };
  struct Built;

  void setup_stc();
  void apply_styles(int lexer);
  void release_docs();
  void request_mode(int mode);
  void build(int k);  // starts building docs_[k] (inline for small bodies, else on the worker)
  void apply_built(uint64_t gen, int mode, Built& b);
  void show_doc(int mode);
  void set_mode_chips();
  void update_note();
  void on_chip(int id);
  void copy_all();
  // The whole body as the current view would copy / save it (kept alive by .first).
  std::pair<std::shared_ptr<const void>, std::string_view> whole_text();
  void append_raw(const std::string& s);  // gRPC document: append, following the tail
  void save_body();
  void on_margin_click(wxStyledTextEvent& e);
  void auto_wrap(const Doc& d);
  int shown_mode() const;
  void fold_level1();
  void unfold_all();
  void open_outline_line(int line);  // Outline -> the full document at that value
  // View as: the format the body is shown as (auto-detected unless the user picked one).
  void view_as_menu();
  void apply_kind(fmt::BodyKind k);  // rebuilds every view of the current body as kind k
  // Preview of an HTML body rendered by wxHtmlWindow (no scripts, no CSS, no network).
  bool show_html_preview();
  void hide_html_preview();
  void update_margin();
  void display_break_note(const Doc& d);
  void do_reveal(int mode);
  void select_found(int at, int len, bool exact, const std::string& label, const std::string& what);
  struct Reveal {
    std::vector<PathStep> path;
    std::string label;
    int mode = 0;
    uint64_t gen = 0;
  };
  std::unique_ptr<Reveal> pending_reveal_;
  void start_styling();
  void style_slice();

  // search
  void find_from(bool forward, bool from_selection_start);
  void show_match(int at, int len);  // at < 0: no match
  std::pair<std::shared_ptr<const void>, std::string_view> search_text();
  void schedule_count();
  void highlight_visible();
  void close_find();

  wxStyledTextCtrl* stc_ = nullptr;
  // Hidden control that only creates documents: Scintilla's CreateLoader / CreateDocument reset the
  // calling view's fold (contraction) state, so they must not be called on the visible one.
  wxStyledTextCtrl* factory_ = nullptr;
  ChipBar* bar_ = nullptr;
  wxPanel* findbar_ = nullptr;
  wxTextCtrl* find_text_ = nullptr;
  ChipBar* find_chips_ = nullptr;
  wxTimer find_timer_;
  wxTimer style_timer_;
  int style_chunk_ = 256 << 10;
  bool match_case_ = false;
  std::string find_count_;
  uint64_t find_seq_ = 0;  // newest async search; older answers are dropped
  std::pair<int, int> hl_range_{0, 0};

  std::unique_ptr<BodyWorker> worker_;
  std::shared_ptr<std::atomic<uint64_t>> gen_;  // bumps on every new body: stale jobs drop out
  std::shared_ptr<bool> alive_;
  std::shared_ptr<const HttpResponse> resp_;
  std::shared_ptr<const GrpcResponse> grpc_resp_;  // set by finish_grpc()
  fmt::BodyKind kind_ = fmt::BodyKind::Empty;
  Doc docs_[kModes];
  int mode_ = Pretty;
  int cur_lexer_ = -1;
  bool grpc_ = false;
  bool wrap_ = false;
  bool outline_ = false;  // Pretty is shown folded (Outline document)
  bool kind_forced_ = false;  // the user chose the format (View as)
  wxHtmlWindow* html_ = nullptr;  // created on the first rendered Preview (no Edge runtime)
  // The Preview in Edge WebView2 (CSS layout like a browser), created on the first Preview click,
  // with scripts, context menu and dev tools off and every request but the page itself refused.
  wxWebView* web_ = nullptr;
  bool web_ready_ = false;        // WebView2 finished creating (settings applied)
  bool web_allow_nav_ = false;    // the next navigation is our own SetPage
  std::string web_pending_;       // page to load once ready
  std::string web_base_;          // response URL (base for relative links)
  bool stale_ = false;
  // A new body is being built on the worker: the previous document is still shown, faded.
  bool holding_ = false;
  void fade_styles();
  bool show_web_preview(const std::string& page);
  void web_load(const std::string& page);
  bool wrap_chosen_ = false;  // the user toggled Wrap for this body (no more automatic choice)
  int wrap_cols_ = 0;         // RawWrap: columns the document was built for
  bool follow_ = true;   // gRPC: keep the newest message in view (until the user scrolls away)
  int auto_first_ = 0;   // first visible line after our last automatic scroll
  std::vector<int> msg_pos_;  // gRPC: start position of each message header in the document
};

}  // namespace watt::app::rv
