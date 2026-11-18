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
#include "core/script.h"

class wxStyledTextCtrl;
class wxStyledTextEvent;
class wxTextCtrl;
class wxHtmlWindow;
class wxWebView;

namespace watt::app::rv {

class ChipBar;
class BodyWorker;
enum class Tone;
class JsonTable;

class BodyView : public wxPanel {
 public:
  explicit BodyView(wxWindow* parent);
  ~BodyView() override;

  // Plain message instead of a body (empty state, errors).
  void show_message(const std::string& text);
  // HTTP response body (kept alive by r, never copied for Raw).
  void set_http(std::shared_ptr<const HttpResponse> r);
  // Where the next set_http body came from when it is not the bytes as received ("history copy:
  // first 64 KB of 10 MB, indented when kept"): said in the note of Raw and Hex. "" = as received.
  void set_origin_note(std::string note) { origin_note_ = std::move(note); }
  // The run's post-response script result: when it called pm.visualizer.set(), the Visualize chip
  // shows its template rendered with its data (response_visualize.h), on the worker, in the
  // WebView2 page with scripts off and nothing fetched. Call before set_http (null: none).
  void set_visualizer(std::shared_ptr<const ScriptRun> post);
  // Visualize is the view to open the next body in (the response view keeps it per request).
  bool visualize_chosen() const { return vis_chosen_; }
  void set_visualize_chosen(bool on) { vis_chosen_ = on; }
  // The user picked Visualize (true) or another view (false) for a body that has a template.
  std::function<void(bool)> on_visualize_chosen;
  // An event stream's body: an "Events" chip before Pretty / Raw goes back to the event list
  // (the response view's SSE page). Empty: no such chip.
  void set_events_chip(std::function<void()> on_events);
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
  // A short note in the body's bar (e.g. "saved as example").
  void note(const std::string& text, Tone tone);
  // The JSONPath filter row (Ctrl+Shift+F): created on first use.
  void show_filter();
  // The filter applied ("" = none), and setting one without moving the focus ("" removes it and
  // hides the row). The response view keeps one per request.
  const std::string& filter_text() const { return filter_; }
  void set_filter(const std::string& expr);
  // A new request is in flight: the body shown is the previous response, drawn dimmed and labelled
  // so it is not read as the new answer. Cleared by the next result (and by set_stale(false)).
  void set_stale(bool stale);

 private:
  // RawWrap: Raw with its lines broken at the pane width on the worker (Scintilla's own wrapping
  // lays out every line of a 10 MB document on the UI thread), used for Wrap on big Raw bodies.
  // Outline: "Fold all" of a very large JSON body, built on the worker (one line per top-level
  // value) instead of hundreds of thousands of Scintilla folds.
  // Filter: the result of the JSONPath filter (response_filter.h) over a JSON body, shown in
  // place of Pretty while a filter is set.
  // Table: a JSON array as rows and columns (JsonTable), Postman's Visualize without a script.
  // Wrapped: Pretty / Filter / Preview / Raw of a heavy document (over 1 MB or 100k lines) with
  // Wrap on and a line wider than the pane: its lines broken at the pane width on the worker,
  // rebuilt after a width change. Heavy documents never get Scintilla's own wrap (it lays out
  // every line up to the target on the UI thread: 170-330 ms for Ctrl+End on 1.3M lines).
  // Visualize: the post-response script's pm.visualizer template, rendered (no document of its own).
  enum Mode { Pretty = 0, Raw = 1, Hex = 2, Preview = 3, RawWrap = 4, Outline = 5, Filter = 6, Table = 7, Wrapped = 8,
              Visualize = 9, kModes = 10 };
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
    bool heavy = false;   // over 1 MB or 100k lines: no Scintilla wrap
    size_t longest = 0;   // longest line in characters (heavy documents)
    // Wrapped / RawWrap: what the document was built from and for.
    int src = -1;
    void* src_doc = nullptr;
    int cols = 0;
    // Wrapped, RawWrap and display_breaks: offsets in `full` (the text before the breaks) of the
    // inserted breaks.
    std::shared_ptr<const std::vector<size_t>> breaks;
    std::shared_ptr<const void> full_owner;
    std::string_view full;
  };
  struct Built;

  void setup_stc();
  void apply_styles(int lexer);
  void release_docs();
  void request_mode(int mode);
  void choose_mode(int mode);  // a mode chip: remembered for this kind of body (chosen_mode_)
  void build(int k);  // starts building docs_[k] (inline for small bodies, else on the worker)
  void apply_built(uint64_t gen, int mode, Built& b);
  void show_doc(int mode);
  // Every document change on the visible control goes through here (wrap / annotations off first).
  void swap_doc(void* doc);
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
  int base_mode() const;   // shown_mode() before Wrap of a heavy document is applied
  int on_screen() const;   // the docs_ index the control shows (-1: none of them)
  int pane_cols() const;   // text columns that fit the pane (0: not laid out yet)
  // Brings the document on screen in line with Wrap and the pane width (worker-built wrap for
  // heavy documents); called after a document is shown, on the Wrap toggle and after a resize.
  void sync_wrap();
  bool wrap_stale(int k) const;
  // Positions between a Wrapped document and its source.
  int wrapped_pos(int src_pos) const;
  int source_pos(int wrapped_pos) const;
  struct Keep {
    int src = -1;  // the source mode the positions are in (-1: nothing kept)
    int top = 0, caret = 0, anchor = 0;
    double ratio = -1;  // RawWrap <-> Raw: the first visible line as a share of the document
  };
  Keep keep_;
  uint64_t wrap_seq_ = 0;  // bumps when a Wrapped / RawWrap document is dropped: its build drops out
  wxTimer wrap_timer_;
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
  // why: the reason a path was not found (json_path_miss), shown under the line.
  void select_found(int at, int len, bool exact, const std::string& label, const std::string& what,
                    const std::string& why = {});
  int miss_line_ = -1;           // line carrying the "not found" explanation box
  void* miss_doc_ = nullptr;     // ... in this document
  struct Reveal {
    std::vector<PathStep> path;
    std::string label;
    int mode = 0;
    uint64_t gen = 0;
  };
  std::unique_ptr<Reveal> pending_reveal_;
  void start_styling();
  void style_slice();

  // visualize
  void show_visualize();
  void show_vis_page();
  std::function<void()> on_events_;
  std::shared_ptr<const ScriptRun> vis_;  // post-response run with a template (null: none)
  bool vis_chosen_ = false;
  uint64_t vis_seq_ = 0;        // bumps with every new template / data: older renders drop out
  uint64_t vis_built_ = ~0ull;  // the vis_seq_ vis_page_ was rendered for
  bool vis_rendering_ = false;
  std::string vis_page_;        // the sandboxed page (visualize_page) or "" with vis_note_ saying why
  std::string vis_note_;
  Tone vis_tone_{};

  // table (created on first use; the model is built on the worker)
  void show_table();
  JsonTable* table_ = nullptr;
  wxWindow* table_win_ = nullptr;  // the same control (usable where JsonTable is incomplete)
  uint64_t table_gen_ = ~0ull, table_fseq_ = ~0ull;  // what the table shows (body, filter)

  // filter
  void close_filter();
  void show_filter_row();  // creates / shows the row (no focus change)
  void apply_filter();  // the text in the filter box changed (debounced)
  wxPanel* filterbar_ = nullptr;
  wxTextCtrl* filter_text_ = nullptr;
  ChipBar* filter_chips_ = nullptr;
  wxTimer filter_timer_;
  std::string filter_;       // the expression applied ("" = none)
  uint64_t filter_seq_ = 0;  // bumps with every new expression: older results drop out

  // search
  void find_from(bool forward, bool from_selection_start);
  void show_match(int at, int len);  // at < 0: no match
  std::pair<std::shared_ptr<const void>, std::string_view> search_text();
  void schedule_count();
  void highlight_visible();
  void make_findbar();  // the find row, created on the first Ctrl+F
  bool find_shown() const { return findbar_ && findbar_->IsShown(); }
  void close_find();
  // Hides the find bar and drops its count / note (the query stays for the next Ctrl+F): a new
  // response, a message page or the Table view has nothing the old count was about.
  void reset_find();

  wxStyledTextCtrl* stc_ = nullptr;
  // Hidden control that only creates documents: Scintilla's CreateLoader / CreateDocument reset the
  // calling view's fold (contraction) state, so they must not be called on the visible one.
  wxStyledTextCtrl* factory_ = nullptr;
  wxStyledTextCtrl* factory();  // creates it on first use
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
  // By size class (0: up to kBigChoice, 1: larger) and fmt::BodyKind; -1: the default. Raw picked
  // for a 10 MB one-line body is not the view for the next 3 KB one (and the other way round).
  signed char chosen_mode_[2][6] = {{-1, -1, -1, -1, -1, -1}, {-1, -1, -1, -1, -1, -1}};
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
  bool create_web();    // the WebView2 control, hidden (false: no Edge runtime)
  void prewarm_web();   // creates it ahead of the first Preview click (after an HTML response)
  bool web_prewarm_ = false;
  std::string origin_note_;
  void web_load(const std::string& page);
  bool wrap_chosen_ = false;  // the user toggled Wrap for this body (no more automatic choice)
  int wrap_cols_ = 0;         // RawWrap: columns the document was built for
  bool follow_ = true;   // gRPC: keep the newest message in view (until the user scrolls away)
  int auto_first_ = 0;   // first visible line after our last automatic scroll
  std::vector<int> msg_pos_;  // gRPC: start position of each message header in the document
};

}  // namespace watt::app::rv
