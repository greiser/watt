// REST request editor: method + URL bar (paste a curl command anywhere in it to import it), query
// params synced both ways with the URL, headers with enable checkboxes and autocompletion, body
// (none / JSON / text / form-urlencoded / multipart / binary file) in a wxStyledTextCtrl with the
// JSON lexer and off-thread Beautify, auth (bearer / basic / API key), transport options,
// {{var}} highlighting with the resolved value on hover, Copy as curl, and an in-app prompt
// before a request uploads a file from outside the workspace.
//
// Performance: nothing here scales with the body size per keystroke. The body lives only in the
// Scintilla buffer (lexed lazily, only what is on screen); store_head() never touches it; {{var}}
// highlighting looks at the visible part of the body only; Beautify runs on a worker thread.
#include <wx/accel.h>
#include <wx/eventfilter.h>
#include <wx/toplevel.h>
#include <wx/utils.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/clipbrd.h>
#include <wx/dcbuffer.h>
#include <wx/filedlg.h>
#include <wx/image.h>
#include <wx/menu.h>
#include <wx/panel.h>
#include <wx/radiobut.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/stc/stc.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>
#include <wx/timer.h>
#include <wx/wupdlock.h>
#include <wx/msw/wrapwin.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#include "app/http_editor_codegen.h"
#include "app/http_editor_kv.h"
#include "app/http_editor_util.h"
#include "app/http_editor_widgets.h"
#include "app/panels.h"

namespace watt::app {
namespace {

using namespace http_editor;

const char* const kMethods[] = {"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"};

enum class BodyMode { None = 0, Json, Text, Xml, UrlEncoded, Multipart, Binary, CurlData };
const char* const kBodyModeLabels[] = {"None", "JSON", "Text", "XML", "URL-encoded", "Multipart", "File"};
constexpr int kBodyModeCount = 7;  // CurlData has no radio button (shown only for imported -d @file)

// Modes edited as text in the body editor.
constexpr bool text_mode(BodyMode m) { return m == BodyMode::Json || m == BodyMode::Text || m == BodyMode::Xml; }

enum class AuthMode { None = 0, Bearer, Basic, ApiKey };
const char* const kAuthLabels[] = {"No auth", "Bearer token", "Basic auth", "API key"};

// Indicators for {{var}} highlighting (Scintilla reserves 0-7 for lexers).
constexpr int kIndOkBox = 8, kIndOkText = 9, kIndBadBox = 10, kIndBadText = 11;
// Closers inserted by auto-close (hidden), and the JSON syntax error squiggle.
constexpr int kIndAutoClose = 12, kIndJsonError = 13;
// Style for the URL placeholder (an end-of-line annotation on the empty line).
constexpr int kStylePlaceholder = 40;

std::string path_utf8(const std::filesystem::path& p) {
  const std::u8string u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

wxString human_size(size_t b) {
  if (b < 1024) return wxString::Format("%zu B", b);
  if (b < 1024 * 1024) return wxString::Format("%.1f KB", b / 1024.0);
  return wxString::Format("%.1f MB", b / (1024.0 * 1024.0));
}

// Header names an API key is usually sent in (detected on load as "API key" auth).
bool is_api_key_header(std::string_view name) {
  for (std::string_view n : {"x-api-key", "api-key", "apikey", "x-apikey", "x-api-token", "x-auth-token"})
    if (iequals(name, n)) return true;
  return false;
}

// Content types this editor sets itself when the body mode changes.
bool is_auto_content_type(std::string_view v) {
  v = trim(v);
  return v.empty() || iequals(v, "application/json") || iequals(v, "text/plain") ||
         iequals(v, "application/x-www-form-urlencoded") || iequals(v, "multipart/form-data") ||
         iequals(v, "application/octet-stream") || iequals(v, "application/xml");
}

wxFont mono_font(int pt) {
  wxFont f(wxFontInfo(pt).Family(wxFONTFAMILY_TELETYPE).FaceName("Cascadia Mono"));
  if (!f.IsOk() || f.GetFaceName() != "Cascadia Mono") f = wxFont(wxFontInfo(pt).Family(wxFONTFAMILY_TELETYPE).FaceName("Consolas"));
  return f;
}

// Lifetime token for worker threads that post back to the editor.
struct Alive {
  std::mutex m;
  wxEvtHandler* target = nullptr;
  std::vector<void*> docs;  // Scintilla documents built on a worker, not yet swapped in
};

// Scintilla's ILoader (include/ILoader.h, stable ABI): a document filled on a worker thread and
// swapped into the control, so megabytes never pass through the UI thread.
class SciLoader {
 public:
  virtual int __stdcall Release() = 0;
  virtual int __stdcall AddData(const char* data, ptrdiff_t length) = 0;
  virtual void* __stdcall ConvertToDocument() = 0;

 protected:
  ~SciLoader() = default;
};

// Bodies at least this big are loaded / beautified into a document built off the UI thread.
constexpr size_t kAsyncBody = 256 * 1024;
// A body line at least this long is slow to edit (Scintilla lays out whole lines).
constexpr size_t kLongLine = 64 * 1024;

// Panel with a rounded 1px border, for the URL bar and the prompt banner.
class FramePanel : public wxPanel {
 public:
  FramePanel(wxWindow* parent, wxColour fill, wxColour border) : wxPanel(parent, wxID_ANY), fill_(fill), border_(border) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(fill);
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
      wxAutoBufferedPaintDC dc(this);
      dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
      dc.Clear();
      dc.SetPen(wxPen(focus_ ? Theme::get().accent : border_, FromDIP(1)));
      dc.SetBrush(wxBrush(fill_));
      const wxSize s = GetClientSize();
      dc.DrawRoundedRectangle(0, 0, s.x, s.y, FromDIP(5));
    });
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      Refresh();
    });
  }
  void set_colours(wxColour fill, wxColour border) {
    fill_ = fill;
    border_ = border;
    SetBackgroundColour(fill);
    Refresh();
  }
  void set_focus(bool f) {
    if (f != focus_) {
      focus_ = f;
      Refresh();
    }
  }

 private:
  wxColour fill_, border_;
  bool focus_ = false;
};

class HttpEditor : public wxPanel, public HttpEditorPanel, public wxEventFilter {
 public:
  HttpEditor(wxWindow* parent, EditorHost* host);
  ~HttpEditor() override;
  // Ctrl+Enter while the focus is outside the editor (sidebar, response) reaches the frame's
  // accelerator, which would send without do_send(): no {{var}} check, no encoding of variable
  // values. The visible editor takes that command and sends through do_send().
  int FilterEvent(wxEvent& e) override;

  wxWindow* window() override { return this; }
  void load(const SavedRequest& r) override;
  void store(SavedRequest& r) const override;
  void store_head(SavedRequest& r) const override;
  void set_in_flight(bool in_flight) override;
  void focus_url() override;

 private:
  // construction
  void build_url_bar(wxSizer* sz);
  void build_banner(wxSizer* sz);
  void ensure_banner();  // the banner is created the first time it is shown
  wxSizer* banner_sizer_ = nullptr;
  size_t banner_at_ = 0;
  wxWindow* build_params_page();
  wxWindow* build_body_page();
  wxWindow* build_headers_page();
  wxWindow* build_auth_page();
  wxWindow* build_options_page();
  void setup_url_stc();
  void setup_body_stc();
  void apply_lexer(BodyMode m);
  void update_line_margin();
  void setup_var_indicators(wxStyledTextCtrl* stc);

  // behaviour
  void touched();  // a user edit: tab dirty + badges
  void update_badges();
  void update_auto_headers(bool force = false);
  void set_method(const std::string& m);
  void show_method_menu();
  std::string url_text() const;
  void set_url_text(const std::string& u);
  void url_changed();
  void update_placeholder(bool empty);
  void params_changed();
  void update_preview();
  // The URL exactly as it is sent ({{vars}} expanded, then encoded): the preview and the send
  // both come from here.
  std::string sent_url() const;
  // Path variables: url with each "/:name" replaced by its URL-encoded value (when it has one).
  std::string with_path(const std::string& url) const;
  // Rows of the Path variables table from the URL's ":names" (the table is built on first use).
  void sync_path_rows();
  void layout_params_page();
  // ":names" in the URL that have no value (the send is refused until they have one).
  std::vector<std::string> empty_path_vars() const;
  // {{var}} names offered by completion: active environment, globals, dynamic.
  std::vector<std::string> var_names() const;
  std::string sent_form(const std::string& body) const;
  // {{names}} the request uses (URL, headers, auth, form fields) that resolve nowhere.
  std::vector<std::string> unresolved_vars(bool* in_url) const;
  void url_inserted(const wxString& text);
  void import_curl(const std::string& cmd, CmdBackslashes bs = CmdBackslashes::AsCurlExe);
  void set_body_mode(BodyMode m, bool user);
  BodyMode body_mode() const { return body_mode_; }
  void set_content_type(const char* ct);
  void set_auth_mode(AuthMode m, bool user);
  void sync_api_key_query();
  void beautify();
  bool paste_long_json();
  void auto_close(int ch);
  void lint();
  void show_lint(uint64_t gen, std::optional<JsonIssue> issue);
  void build_body_doc(std::shared_ptr<const std::string> text, bool reindent, uint64_t gen, uint64_t edits);
  void swap_body_doc(void* doc, bool reindent, uint64_t gen, uint64_t edits, double worker_ms, size_t bytes);
  void copy_as_curl(CurlShell shell);
  void show_curl_menu();
  void note(const wxString& text, bool warn = false);

  // {{vars}}
  std::optional<std::string> resolve(const std::string& name) const;
  void highlight_vars(wxStyledTextCtrl* stc, int start, int end);
  void highlight_body_visible();
  void var_tooltip(wxStyledTextCtrl* stc, int pos);
  // Typing "{{": closes it with "}}" and lists the variable names (environment, globals, dynamic).
  void var_completion(wxStyledTextCtrl* stc);
  bool step_over_closer(wxStyledTextCtrl* s, int key);
  void step_past_var_closer(wxStyledTextCtrl* s);

  // send + upload prompt
  void do_send();
  void show_upload_prompt(const std::vector<std::filesystem::path>& files, const std::vector<std::string>& env);
  void show_info(const wxString& title, const wxString& text = {}, const wxString& action = {},
                 std::function<void()> on_action = {});
  void hide_banner();
  void wrap_banner();
  void set_banner_texts(const wxString& title, const wxString& text);

  EditorHost* host_;
  std::shared_ptr<Alive> alive_ = std::make_shared<Alive>();

  // url bar
  FlatButton* method_ = nullptr;
  std::string method_str_ = "GET";
  FramePanel* url_box_ = nullptr;
  wxStyledTextCtrl* url_ = nullptr;
  FlatButton* send_ = nullptr;
  // banner
  FramePanel* banner_ = nullptr;
  wxStaticText* banner_title_ = nullptr;
  wxStaticText* banner_text_ = nullptr;
  wxButton* banner_action_ = nullptr;
  wxButton* banner_cancel_ = nullptr;
  std::function<void()> banner_on_action_;
  wxString banner_title_raw_, banner_text_raw_;
  int banner_wrap_ = -1;
  // The undefined {{vars}} the "Not sent" banner names: it goes away once none is left.
  std::vector<std::string> banner_missing_;
  // tabs
  TabStrip* tabs_ = nullptr;
  wxSimplebook* book_ = nullptr;
  FlatButton* curl_btn_ = nullptr;
  wxStaticText* note_ = nullptr;
  wxTimer note_timer_;
  // JSON syntax check of the body (debounced, off the UI thread for big bodies)
  wxTimer lint_timer_;
  wxStaticText* lint_label_ = nullptr;
  uint64_t lint_gen_ = 0;
  std::optional<JsonIssue> lint_issue_;
  // params
  KvGrid* params_ = nullptr;
  KvPanel* query_panel_ = nullptr;
  wxPanel* params_page_ = nullptr;
  KvPanel* path_panel_ = nullptr;  // built when the URL first has a ":name"
  KvGrid* path_ = nullptr;
  // Path variable values by name, including ones no longer in the URL (deleting ":id" and typing
  // it again keeps its value).
  std::map<std::string, std::string> path_values_;
  std::vector<std::string> path_names_;  // the ":names" of the URL, in order
  wxStaticText* preview_ = nullptr;  // the URL as sent: {{vars}} expanded, encoded
  // body
  RadioStrip* body_modes_ = nullptr;
  FlatButton* beautify_ = nullptr;
  wxSimplebook* body_book_ = nullptr;
  wxStyledTextCtrl* body_ = nullptr;
  // The form tables are built when their body mode is first shown; until then their rows live in
  // form_pending_ (0 urlencoded, 1 multipart).
  KvGrid* urlenc_ = nullptr;
  KvGrid* multipart_ = nullptr;
  // Body pages other than the editor are created when their mode is first shown (page index in
  // body_book_ per kind: 0 none, 1 text, 2 urlencoded, 3 multipart, 4 file, 5 curl data).
  int body_page_[6] = {-1, -1, -1, -1, -1, -1};
  int ensure_body_page(int kind);
  void sync_body_book();
  std::string binary_val_;     // the File mode path (binary_path_ shows it once built)
  wxString curl_data_label_;   // the curl -d arguments text
  std::vector<KvRow> form_pending_[2];
  KvGrid* form_grid(int which);  // builds it on first use
  std::vector<KvRow> form_rows(int which) const;
  void set_form_rows(int which, const std::vector<KvRow>& rows);
  wxTextCtrl* binary_path_ = nullptr;
  wxStaticText* curl_data_text_ = nullptr;
  BodyMode body_mode_ = BodyMode::None;
  // headers
  KvGrid* headers_ = nullptr;
  // Built on first show like Auth / Options; until then the rows live in header_pending_.
  wxPanel* headers_holder_ = nullptr;
  std::vector<KvRow> header_pending_;
  std::vector<KvRow> header_rows() const;
  void set_header_rows(const std::vector<KvRow>& rows);
  void update_files_row();
  wxPanel* files_row_ = nullptr;
  FlatButton* auto_btn_ = nullptr;
  FramePanel* auto_panel_ = nullptr;
  wxStaticText* auto_title_ = nullptr;
  wxFlexGridSizer* auto_grid_ = nullptr;
  std::vector<std::pair<std::string, std::string>> auto_shown_rows_;
  bool auto_shown_ = false;
  wxStaticText* files_text_ = nullptr;
  // auth
  FlatButton* auth_btn_ = nullptr;  // auth type: a themed dropdown (a native combo is light in the dark theme)
  wxSimplebook* auth_book_ = nullptr;
  // Token, user and API key fields: one-line editors with {{var}} colouring, tooltips and {{
  // completion like the URL (the password stays a masked native box).
  wxStyledTextCtrl* bearer_ = nullptr;
  wxStyledTextCtrl* basic_user_ = nullptr;
  wxTextCtrl* basic_pass_ = nullptr;      // the one shown (masked or, after Show, plain)
  wxTextCtrl* basic_pass_alt_ = nullptr;  // its hidden twin
  wxStyledTextCtrl* key_name_ = nullptr;
  wxStyledTextCtrl* key_value_ = nullptr;
  wxStyledTextCtrl* make_var_field(wxWindow* parent, const char* hint, std::string* val, wxWindow** frame);
  ChoiceButton* key_where_ = nullptr;
  AuthMode auth_mode_ = AuthMode::None;
  size_t auth_header_index_ = SIZE_MAX;  // where the auth header was in http.headers
  std::string api_key_param_;            // query param the API key auth manages
  // The Auth and Options pages are built the first time they are shown: until then (and after)
  // their values live here, so load / store / send never need the widgets.
  struct AuthVals {
    std::string bearer, user, pass, key_name, key_value;
    int key_where = 0;  // 0 header, 1 query
  } av_;
  struct OptVals {
    long timeout_ms = 30000;
    bool redirects = true, verify_tls = true, fresh = false;
    int version = 0;
  } ov_;
  wxPanel* auth_holder_ = nullptr;
  wxPanel* opts_holder_ = nullptr;
  // Docs: notes about the request (Markdown), in docs_text_ until the page is first shown.
  wxPanel* docs_holder_ = nullptr;
  wxStyledTextCtrl* docs_ = nullptr;
  std::string docs_text_;
  wxWindow* build_docs_page();
  void ensure_page(size_t i);
  void push_auth();  // av_ -> widgets (when built)
  void push_opts();  // ov_ -> widgets (when built)
  // options (themed widgets: native spin / choice / check boxes are light in the dark theme)
  wxTextCtrl* timeout_ = nullptr;
  FramePanel* timeout_frame_ = nullptr;
  CheckButton* redirects_ = nullptr;
  CheckButton* verify_tls_ = nullptr;
  ChoiceButton* version_ = nullptr;
  CheckButton* fresh_ = nullptr;

  // state
  SavedRequest base_;             // last loaded request, body cleared: fields this editor does not show
  std::string loaded_urlenc_;     // urlencoded body as loaded (kept byte for byte if not edited)
  std::vector<KvRow> loaded_urlenc_rows_;
  bool loading_ = false;
  bool syncing_ = false;
  bool in_flight_ = false;
  bool first_load_ = true;
  bool resolve_vars_in_curl_ = true;
  // True while do_send() hands the request to the shell: store() then writes the URL / form body
  // with {{vars}} expanded and encoded for where they stand (see sent_url()), when a plain
  // expansion would send something else.
  bool sending_ = false;
  bool force_send_ = false;  // "Send as written" past the undefined {{var}} check
  // Minified JSON shown formatted (a pasted or loaded one-line body): sent minified, and byte for
  // byte as pasted while it is unedited.
  CheckButton* minify_ = nullptr;
  std::shared_ptr<const std::string> raw_body_;      // the bytes as pasted / loaded
  std::shared_ptr<const std::string> raw_pending_;   // raw_body_ of the format still on a worker
  uint64_t raw_edits_ = 0;                           // body_edits_ when raw_body_ was shown
  // The document before the last swap (paste / format): Ctrl+Z with no undo history left goes
  // back to it in O(1) instead of Scintilla undoing a 25 MB insert.
  void* prev_doc_ = nullptr;
  bool prev_minify_ = false;
  void release_doc_async(void* doc);
  void install_doc(void* doc);
  void paste_arrived(uint64_t gen, uint64_t edits, int start, int end, bool whole,
                     std::shared_ptr<const std::string> text, bool long_json);
  uint64_t body_edits_ = 0;
  uint64_t beautify_gen_ = 0;
  uint64_t body_load_gen_ = 0;
  std::shared_ptr<const std::string> pending_body_;  // large body still being loaded into body_
  int body_hl_first_ = -1, body_hl_last_ = -1, body_hl_x_ = 0;
  int margin_digits_ = 0;
};

// ------------------------------------------------------------------ construction

HttpEditor::HttpEditor(wxWindow* parent, EditorHost* host) : wxPanel(parent, wxID_ANY), host_(host) {
  alive_->target = this;
  const Theme& t = Theme::get();
  SetBackgroundColour(t.bg);

  auto* sz = new wxBoxSizer(wxVERTICAL);
  build_url_bar(sz);
  build_banner(sz);

  // Tab strip + Copy as curl on the right.
  auto* tabrow = new wxBoxSizer(wxHORIZONTAL);
  tabs_ = new TabStrip(this);
  tabs_->set_tabs({"Params", "Body", "Headers", "Auth", "Options", "Docs"});
  tabrow->Add(tabs_, 1, wxEXPAND);
  curl_btn_ = new FlatButton(this, "Copy as", FlatButton::Style::Subtle, true);
  curl_btn_->SetToolTip("Copy this request as a curl command (bash, cmd, PowerShell) or as code (Python, "
                        "JavaScript, Node.js, Go, Java, C#, PowerShell, HTTPie), with {{variables}} resolved or kept");
  tabrow->Add(curl_btn_, 0, wxALIGN_CENTER_VERTICAL);
  sz->Add(tabrow, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
  auto* line = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
  line->SetBackgroundColour(t.border);
  sz->Add(line, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

  book_ = new wxSimplebook(this);
  book_->SetBackgroundColour(t.bg);
  book_->AddPage(build_params_page(), "Params");
  book_->AddPage(build_body_page(), "Body");
  for (wxPanel** h : {&headers_holder_, &auth_holder_, &opts_holder_, &docs_holder_}) {
    *h = new wxPanel(book_);
    (*h)->SetBackgroundColour(t.bg);
    (*h)->SetSizer(new wxBoxSizer(wxVERTICAL));
  }
  book_->AddPage(headers_holder_, "Headers");
  book_->AddPage(auth_holder_, "Auth");
  book_->AddPage(opts_holder_, "Options");
  book_->AddPage(docs_holder_, "Docs");
  sz->Add(book_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
  // Status line under the pages (paste / copy / format notes): never squeezes the tab strip.
  note_ = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
  note_->SetForegroundColour(t.dim);
  note_->SetMinSize(wxSize(-1, GetCharHeight()));
  note_->Hide();
  sz->Add(note_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
  SetSizer(sz);

  tabs_->on_select = [this](size_t i) {
    ensure_page(i);
    book_->ChangeSelection(i);
    if (i == 1) sync_body_book();
    if (i == 2) update_auto_headers();
  };
  curl_btn_->on_click = [this] { show_curl_menu(); };
  note_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { note(""); });
  lint_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { lint(); });

  // Ctrl+Enter sends from anywhere in the editor (ahead of the frame's own accelerator, so the
  // upload check below runs first).
  const int id_send = wxWindow::NewControlId();
  wxAcceleratorEntry acc[] = {{wxACCEL_CTRL, WXK_RETURN, id_send}, {wxACCEL_CTRL, WXK_NUMPAD_ENTER, id_send}};
  SetAcceleratorTable(wxAcceleratorTable(2, acc));
  Bind(wxEVT_MENU, [this](wxCommandEvent&) { do_send(); }, id_send);
  wxEvtHandler::AddFilter(this);
}

int HttpEditor::FilterEvent(wxEvent& e) {
  if (e.GetEventType() != wxEVT_MENU || !IsShownOnScreen()) return Event_Skip;
  if (!wxGetKeyState(WXK_CONTROL) || !(wxGetKeyState(WXK_RETURN) || wxGetKeyState(WXK_NUMPAD_ENTER))) return Event_Skip;
  auto* top = dynamic_cast<wxTopLevelWindow*>(wxGetTopLevelParent(this));
  if (!top || !top->IsActive()) return Event_Skip;
  do_send();
  return Event_Processed;
}

HttpEditor::~HttpEditor() {
  wxEvtHandler::RemoveFilter(this);
  std::lock_guard lk(alive_->m);
  alive_->target = nullptr;
  for (void* doc : alive_->docs) body_->ReleaseDocument(doc);  // built but never swapped in
  alive_->docs.clear();
  if (prev_doc_) body_->ReleaseDocument(prev_doc_);
  prev_doc_ = nullptr;
}

void HttpEditor::build_url_bar(wxSizer* sz) {
  const Theme& t = Theme::get();
  auto* row = new wxBoxSizer(wxHORIZONTAL);
  url_box_ = new FramePanel(this, t.field, t.border);
  auto* inner = new wxBoxSizer(wxHORIZONTAL);
  method_ = new FlatButton(url_box_, "GET", FlatButton::Style::Method, true);
  method_->set_text_colour(method_colour("GET"));
  method_->SetToolTip("HTTP method (Alt+Down in the URL opens this list, Ctrl+Up / Ctrl+Down cycles)");
  inner->Add(method_, 0, wxEXPAND | wxALL, FromDIP(3));
  auto* sep = new wxPanel(url_box_, wxID_ANY, wxDefaultPosition, wxSize(1, -1));
  sep->SetBackgroundColour(t.border);
  sep->Disable();  // never a Tab stop: Shift+Tab from the URL lands on the method button
  inner->Add(sep, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(7));
  url_ = new wxStyledTextCtrl(url_box_, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxWANTS_CHARS);
  setup_url_stc();
  inner->Add(url_, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(8));
  url_box_->SetSizer(inner);
  row->Add(url_box_, 1, wxEXPAND | wxRIGHT, FromDIP(8));
  send_ = new FlatButton(this, "Send", FlatButton::Style::Primary);
  send_->SetToolTip("Send (Ctrl+Enter)");
  row->Add(send_, 0, wxEXPAND);
  sz->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
  sz->AddSpacer(FromDIP(8));

  method_->on_click = [this] { show_method_menu(); };
  send_->on_click = [this] { in_flight_ ? host_->cancel() : do_send(); };
}

void HttpEditor::setup_url_stc() {
  const Theme& t = Theme::get();
  wxStyledTextCtrl* s = url_;
  wxFont f = GetFont();
  f.SetPointSize(f.GetPointSize() + 1);
  s->StyleSetFont(wxSTC_STYLE_DEFAULT, f);
  s->StyleSetForeground(wxSTC_STYLE_DEFAULT, t.fg);
  s->StyleSetBackground(wxSTC_STYLE_DEFAULT, t.field);
  s->StyleClearAll();
  s->StyleSetFont(kStylePlaceholder, f);
  s->StyleSetForeground(kStylePlaceholder, t.dim);
  s->StyleSetBackground(kStylePlaceholder, t.field);
  s->SetLexer(wxSTC_LEX_NULL);
  for (int m = 0; m < 5; ++m) s->SetMarginWidth(m, 0);
  s->SetMarginLeft(0);
  s->SetMarginRight(0);
  s->SetUseHorizontalScrollBar(false);
  s->SetUseVerticalScrollBar(false);
  s->SetWrapMode(wxSTC_WRAP_NONE);
  s->SetCaretForeground(t.fg);
  s->SetSelBackground(true, t.dark ? wxColour(0x26, 0x4F, 0x78) : wxColour(0xAD, 0xD6, 0xFF));
  s->SetScrollWidth(1);
  s->SetScrollWidthTracking(true);
  s->SetEOLMode(wxSTC_EOL_LF);
  // Only text changes notify: annotation / indicator / style changes would otherwise raise
  // wxEVT_STC_CHANGE too (and the placeholder annotation is set from the change handler).
  s->SetModEventMask(wxSTC_MOD_INSERTTEXT | wxSTC_MOD_DELETETEXT);
  s->SetMouseDwellTime(350);
  s->CallTipSetBackground(t.dark ? wxColour(0x2B, 0x2B, 0x2B) : wxColour(0xFF, 0xFF, 0xF0));
  s->CallTipSetForeground(t.fg);
  setup_var_indicators(s);
  s->EOLAnnotationSetVisible(wxSTC_EOLANNOTATION_STANDARD);
  s->EOLAnnotationSetText(0, "Enter a URL or paste a curl command");
  s->EOLAnnotationSetStyle(0, kStylePlaceholder);
  const int h = s->TextHeight(0);
  s->SetMinSize(wxSize(FromDIP(100), h));
  s->SetMaxSize(wxSize(-1, h));

  s->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
    const int k = e.GetKeyCode();
    if (url_->AutoCompActive() && !e.HasAnyModifiers()) {
      // The {{variable}} list is open: its keys pick / move / close it (Scintilla handles them).
      switch (k) {
        case WXK_RETURN: case WXK_NUMPAD_ENTER: case WXK_TAB: case WXK_UP: case WXK_DOWN:
        case WXK_NUMPAD_UP: case WXK_NUMPAD_DOWN: case WXK_PAGEUP: case WXK_PAGEDOWN:
        case WXK_HOME: case WXK_END: case WXK_ESCAPE: case WXK_NUMPAD_PAGEUP: case WXK_NUMPAD_PAGEDOWN:
        case WXK_NUMPAD_HOME: case WXK_NUMPAD_END:
          e.Skip();
          return;
        default: break;
      }
    }
    if ((k == WXK_RETURN || k == WXK_NUMPAD_ENTER) && !e.ControlDown()) {
      do_send();
    } else if ((k == WXK_DOWN || k == WXK_NUMPAD_DOWN) && e.GetModifiers() == wxMOD_ALT) {
      show_method_menu();
    } else if ((k == WXK_UP || k == WXK_DOWN) && e.GetModifiers() == wxMOD_CONTROL) {
      // Cycle through the standard methods without leaving the URL.
      const int n = static_cast<int>(std::size(kMethods));
      int i = 0;
      while (i < n && method_str_ != kMethods[i]) ++i;
      i = i == n ? 0 : (i + (k == WXK_DOWN ? 1 : n - 1)) % n;
      set_method(kMethods[i]);
      touched();
    } else if (k == WXK_TAB && !e.ControlDown()) {
      url_->Navigate(e.ShiftDown() ? wxNavigationKeyEvent::IsBackward : wxNavigationKeyEvent::IsForward);
    } else if ((k == WXK_UP || k == WXK_DOWN || k == WXK_PAGEUP || k == WXK_PAGEDOWN) && !e.HasAnyModifiers()) {
      // single line: nothing to move to
    } else {
      e.Skip();
    }
  });
  // Typing the "}}" that {{ already added steps over it (like the body), so a {{var}} typed by
  // hand does not end up as {{var}}}}.
  s->IndicatorSetStyle(kIndAutoClose, wxSTC_INDIC_HIDDEN);
  s->Bind(wxEVT_CHAR, [this](wxKeyEvent& e) {
    if (step_over_closer(url_, e.GetUnicodeKey())) return;
    e.Skip();
  });
  s->Bind(wxEVT_STC_CHARADDED, [this](wxStyledTextEvent& e) {
    e.Skip();
    if (e.GetKey() == '{') var_completion(url_);
  });
  s->Bind(wxEVT_STC_AUTOCOMP_COMPLETED, [this](wxStyledTextEvent& e) {
    e.Skip();
    CallAfter([this] { step_past_var_closer(url_); });
  });
  s->Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) {
    e.Skip();
    url_box_->set_focus(true);
  });
  s->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) {
    e.Skip();
    url_box_->set_focus(false);
    url_->CallTipCancel();
  });
  s->Bind(wxEVT_STC_MODIFIED, [this](wxStyledTextEvent& e) {
    e.Skip();
    if (loading_ || syncing_) return;
    if (e.GetModificationType() & wxSTC_MOD_INSERTTEXT) {
      const wxString ins = e.GetText();
      if (ins.find_first_of("\r\n") != wxString::npos || (ins.length() > 5 && looks_like_curl(ins.utf8_string())))
        CallAfter([this, ins] { url_inserted(ins); });
    }
  });
  s->Bind(wxEVT_STC_CHANGE, [this](wxStyledTextEvent& e) {
    e.Skip();
    if (loading_ || syncing_) return;
    url_changed();
  });
  s->Bind(wxEVT_STC_DWELLSTART, [this](wxStyledTextEvent& e) { var_tooltip(url_, e.GetPosition()); });
  s->Bind(wxEVT_STC_DWELLEND, [this](wxStyledTextEvent&) { url_->CallTipCancel(); });
}

void HttpEditor::setup_var_indicators(wxStyledTextCtrl* s) {
  const Theme& t = Theme::get();
  s->IndicatorSetStyle(kIndOkBox, wxSTC_INDIC_ROUNDBOX);
  s->IndicatorSetForeground(kIndOkBox, t.var_ok);
  s->IndicatorSetAlpha(kIndOkBox, 45);
  s->IndicatorSetOutlineAlpha(kIndOkBox, 0);
  s->IndicatorSetUnder(kIndOkBox, true);
  s->IndicatorSetStyle(kIndOkText, wxSTC_INDIC_TEXTFORE);
  s->IndicatorSetForeground(kIndOkText, t.var_ok);
  s->IndicatorSetStyle(kIndBadBox, wxSTC_INDIC_ROUNDBOX);
  s->IndicatorSetForeground(kIndBadBox, t.var_bad);
  s->IndicatorSetAlpha(kIndBadBox, 45);
  s->IndicatorSetOutlineAlpha(kIndBadBox, 0);
  s->IndicatorSetUnder(kIndBadBox, true);
  s->IndicatorSetStyle(kIndBadText, wxSTC_INDIC_TEXTFORE);
  s->IndicatorSetForeground(kIndBadText, t.var_bad);
}

void HttpEditor::build_banner(wxSizer* sz) {
  banner_sizer_ = sz;
  banner_at_ = sz->GetItemCount();
}

void HttpEditor::ensure_banner() {
  if (banner_) return;
  const Theme& t = Theme::get();
  banner_ = new FramePanel(this, t.warn_bg, t.warn_border);
  auto* bs = new wxBoxSizer(wxHORIZONTAL);
  auto* texts = new wxBoxSizer(wxVERTICAL);
  banner_title_ = new wxStaticText(banner_, wxID_ANY, "");
  banner_title_->SetFont(GetFont().Bold());
  banner_title_->SetForegroundColour(t.fg);
  banner_text_ = new wxStaticText(banner_, wxID_ANY, "");
  banner_text_->SetForegroundColour(t.fg);
  texts->Add(banner_title_, 0, wxBOTTOM, FromDIP(3));
  texts->Add(banner_text_, 0, wxEXPAND);
  bs->Add(texts, 1, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(10));
  banner_action_ = new wxButton(banner_, wxID_ANY, "Allow");
  banner_cancel_ = new wxButton(banner_, wxID_ANY, "Cancel");
  bs->Add(banner_action_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
  bs->Add(banner_cancel_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
  banner_->SetSizer(bs);
  banner_->Hide();
  banner_sizer_->Insert(banner_at_, banner_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
  banner_action_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    auto fn = banner_on_action_;
    if (fn) fn();
  });
  banner_cancel_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { hide_banner(); });
  banner_->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
    if (e.GetKeyCode() == WXK_ESCAPE) hide_banner();
    else e.Skip();
  });
  banner_->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
    e.Skip();
    wrap_banner();
  });
}

wxWindow* HttpEditor::build_params_page() {
  auto* p = new wxPanel(book_);
  p->SetBackgroundColour(Theme::get().bg);
  auto* s = new wxBoxSizer(wxVERTICAL);
  params_page_ = p;
  auto* kp = new KvPanel(p, KvGrid::Kind::Params, "Query parameters");
  query_panel_ = kp;
  params_ = kp->grid();
  params_->lookup = [this](const std::string& n) { return resolve(n); };
  params_->var_names = [this] { return var_names(); };
  params_->on_change = [this] { params_changed(); };
  p->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
    e.Skip();
    if (path_panel_ && path_panel_->IsShown()) CallAfter([this] { layout_params_page(); });
  });
  preview_ = new wxStaticText(kp, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_MIDDLE | wxST_NO_AUTORESIZE);
  preview_->SetForegroundColour(Theme::get().dim);
  preview_->SetFont(mono_font(GetFont().GetPointSize()));
  kp->add_above(preview_);
  preview_->Hide();  // shown by update_preview() once there is a URL
  s->Add(kp, 1, wxEXPAND | wxTOP, FromDIP(4));
  p->SetSizer(s);
  return p;
}

wxWindow* HttpEditor::build_body_page() {
  const Theme& t = Theme::get();
  auto* p = new wxPanel(book_);
  p->SetBackgroundColour(t.bg);
  auto* s = new wxBoxSizer(wxVERTICAL);
  auto* modes = new wxBoxSizer(wxHORIZONTAL);
  body_modes_ = new RadioStrip(p, std::vector<wxString>(std::begin(kBodyModeLabels), std::end(kBodyModeLabels)));
  modes->Add(body_modes_, 0, wxALIGN_CENTER_VERTICAL);
  body_modes_->on_select = [this](int i) { set_body_mode(static_cast<BodyMode>(i), true); };
  modes->AddStretchSpacer();
  beautify_ = new FlatButton(p, "Beautify", FlatButton::Style::Subtle);
  beautify_->SetToolTip("Re-indent the JSON body (keeps values and {{variables}} as written)  Ctrl+B");
  beautify_->on_click = [this] { beautify(); };
  minify_ = new CheckButton(p, "Minified");
  minify_->SetToolTip("Edit the JSON formatted, send it without the whitespace between tokens (an unedited "
                      "paste is sent byte for byte as pasted)");
  minify_->on_change = [this] {
    if (!loading_) touched();
  };
  modes->Add(minify_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
  modes->Add(beautify_, 0, wxALIGN_CENTER_VERTICAL);
  // As tall as the Beautify button in every mode, so the radio row never moves.
  modes->Add(0, beautify_->GetBestSize().y);
  s->Add(modes, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(8));

  body_book_ = new wxSimplebook(p);
  body_book_->SetBackgroundColour(t.bg);
  // 1: JSON / text editor
  {
    auto* frame = new FramePanel(body_book_, t.field, t.border);
    auto* fs = new wxBoxSizer(wxVERTICAL);
    body_ = new wxStyledTextCtrl(frame, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    setup_body_stc();
    fs->Add(body_, 1, wxEXPAND | wxALL, FromDIP(1));
    frame->SetSizer(fs);
    body_book_->AddPage(frame, "text");
    body_page_[1] = 0;
  }
  s->Add(body_book_, 1, wxEXPAND);
  lint_label_ = new wxStaticText(p, wxID_ANY, "");
  lint_label_->SetForegroundColour(t.js_error);
  lint_label_->SetCursor(wxCursor(wxCURSOR_HAND));
  lint_label_->SetToolTip("Go to the error");
  lint_label_->Hide();
  lint_label_->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
    if (!lint_issue_) return;
    const int at = static_cast<int>(std::min<size_t>(lint_issue_->offset, static_cast<size_t>(body_->GetLength())));
    body_->SetFocus();
    body_->GotoPos(at);
    body_->EnsureCaretVisible();
  });
  s->Add(lint_label_, 0, wxTOP, FromDIP(6));
  p->SetSizer(s);
  return p;
}

KvGrid* HttpEditor::form_grid(int which) {
  ensure_body_page(which == 0 ? 2 : 3);
  return which == 0 ? urlenc_ : multipart_;
}

// Shows the page of the current body mode, creating it on first use; only while the Body tab is
// shown (a new GET tab never builds the "no body" page or a form table it does not show).
void HttpEditor::sync_body_book() {
  if (book_->GetSelection() != 1) return;
  int kind = 0;
  switch (body_mode_) {
    case BodyMode::None: kind = 0; break;
    case BodyMode::Json:
    case BodyMode::Text:
    case BodyMode::Xml: kind = 1; break;
    case BodyMode::UrlEncoded: kind = 2; break;
    case BodyMode::Multipart: kind = 3; break;
    case BodyMode::Binary: kind = 4; break;
    case BodyMode::CurlData: kind = 5; break;
  }
  const int page = ensure_body_page(kind);
  if (body_book_->GetSelection() != page) body_book_->ChangeSelection(static_cast<size_t>(page));
}

int HttpEditor::ensure_body_page(int kind) {
  if (body_page_[kind] >= 0) return body_page_[kind];
  const Theme& t = Theme::get();
  wxWindowUpdateLocker lock(body_book_);
  wxWindow* page = nullptr;
  if (kind == 0) {
    auto* np = new wxPanel(body_book_);
    np->SetBackgroundColour(t.bg);
    auto* ns = new wxBoxSizer(wxVERTICAL);
    auto* l = new wxStaticText(np, wxID_ANY, "This request does not have a body");
    l->SetForegroundColour(t.dim);
    ns->AddStretchSpacer();
    ns->Add(l, 0, wxALIGN_CENTER_HORIZONTAL);
    ns->AddStretchSpacer(2);
    np->SetSizer(ns);
    page = np;
  } else if (kind == 2 || kind == 3) {
    const int which = kind - 2;
    KvGrid*& g = which == 0 ? urlenc_ : multipart_;
    g = new KvGrid(body_book_, which == 0 ? KvGrid::Kind::UrlEncoded : KvGrid::Kind::Multipart);
    g->lookup = [this](const std::string& n) { return resolve(n); };
    g->on_change = [this] {
      if (!loading_) touched();
    };
    g->set_rows(form_pending_[which]);
    form_pending_[which].clear();
    page = g;
  } else if (kind == 4) {
    auto* bp = new wxPanel(body_book_);
    bp->SetBackgroundColour(t.bg);
    auto* bs = new wxBoxSizer(wxVERTICAL);
    auto* l = new wxStaticText(bp, wxID_ANY, "Send the bytes of this file as the body (read when the request is sent):");
    l->SetForegroundColour(t.dim);
    bs->Add(l, 0, wxBOTTOM, FromDIP(6));
    auto* r = new wxBoxSizer(wxHORIZONTAL);
    binary_path_ = new wxTextCtrl(bp, wxID_ANY, wxString::FromUTF8(binary_val_));
    binary_path_->SetHint("Path (relative paths resolve against the workspace)");
    auto* browse = new wxButton(bp, wxID_ANY, wxString::FromUTF8("Choose file\xE2\x80\xA6"));
    r->Add(binary_path_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    r->Add(browse, 0, wxALIGN_CENTER_VERTICAL);
    bs->Add(r, 0, wxEXPAND);
    bp->SetSizer(bs);
    binary_path_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
      binary_val_ = binary_path_->GetValue().utf8_string();
      if (!loading_) touched();
    });
    browse->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
      wxFileDialog dlg(this, "Choose the body file", "", "", "All files (*.*)|*.*", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
      if (dlg.ShowModal() == wxID_OK) binary_path_->SetValue(dlg.GetPath());
    });
    page = bp;
  } else if (kind == 5) {
    auto* cp = new wxPanel(body_book_);
    cp->SetBackgroundColour(t.bg);
    auto* cs = new wxBoxSizer(wxVERTICAL);
    curl_data_text_ = new wxStaticText(cp, wxID_ANY, curl_data_label_);
    curl_data_text_->SetForegroundColour(t.fg);
    curl_data_text_->SetFont(mono_font(GetFont().GetPointSize()));
    cs->Add(curl_data_text_, 0, wxEXPAND);
    cp->SetSizer(cs);
    page = cp;
  }
  if (!page) return body_page_[1];
  body_book_->AddPage(page, "page");
  body_page_[kind] = static_cast<int>(body_book_->GetPageCount()) - 1;
  return body_page_[kind];
}

std::vector<KvRow> HttpEditor::form_rows(int which) const {
  const KvGrid* g = which == 0 ? urlenc_ : multipart_;
  return g ? g->rows() : form_pending_[which];
}

void HttpEditor::set_form_rows(int which, const std::vector<KvRow>& rows) {
  if (KvGrid* g = which == 0 ? urlenc_ : multipart_) g->set_rows(rows);
  else form_pending_[which] = rows;
}

// Line number margin as wide as the line count needs (GetLineCount() is O(1)).
void HttpEditor::update_line_margin() {
  int digits = 1;
  for (int n = body_->GetLineCount(); n >= 10; n /= 10) ++digits;
  digits = std::max(digits, 3);
  if (digits == margin_digits_) return;
  margin_digits_ = digits;
  body_->SetMarginWidth(0, body_->TextWidth(wxSTC_STYLE_LINENUMBER, wxString('9', static_cast<size_t>(digits) + 1)));
}

// Setting a lexer resets its keywords and properties, so they are set again each time.
void HttpEditor::apply_lexer(BodyMode m) {
  // JSON and XML style numbers overlap: the colours are set for the lexer in use.
  const Theme& t = Theme::get();
  for (int st = 0; st < 32; ++st) body_->StyleSetForeground(st, t.fg);
  if (m == BodyMode::Json) {
    body_->StyleSetForeground(wxSTC_JSON_PROPERTYNAME, t.js_key);
    body_->StyleSetForeground(wxSTC_JSON_STRING, t.js_string);
    body_->StyleSetForeground(wxSTC_JSON_STRINGEOL, t.js_error);
    body_->StyleSetForeground(wxSTC_JSON_NUMBER, t.js_number);
    body_->StyleSetForeground(wxSTC_JSON_KEYWORD, t.js_keyword);
    body_->StyleSetForeground(wxSTC_JSON_OPERATOR, t.js_punct);
    body_->StyleSetForeground(wxSTC_JSON_ESCAPESEQUENCE, t.js_keyword);
    body_->StyleSetForeground(wxSTC_JSON_LINECOMMENT, t.dim);
    body_->StyleSetForeground(wxSTC_JSON_BLOCKCOMMENT, t.dim);
    body_->StyleSetForeground(wxSTC_JSON_ERROR, t.js_error);
    body_->StyleSetForeground(wxSTC_JSON_URI, t.js_string);
    body_->SetLexer(wxSTC_LEX_JSON);
    body_->SetKeyWords(0, "true false null");
    body_->SetProperty("fold", "1");
    body_->SetProperty("fold.compact", "0");
    body_->SetProperty("lexer.json.allow.comments", "1");
    body_->SetProperty("lexer.json.escape.sequence", "1");
  } else if (m == BodyMode::Xml) {
    body_->StyleSetForeground(wxSTC_H_TAG, t.js_key);
    body_->StyleSetForeground(wxSTC_H_TAGUNKNOWN, t.js_key);
    body_->StyleSetForeground(wxSTC_H_TAGEND, t.js_key);
    body_->StyleSetForeground(wxSTC_H_ATTRIBUTE, t.js_number);
    body_->StyleSetForeground(wxSTC_H_ATTRIBUTEUNKNOWN, t.js_number);
    body_->StyleSetForeground(wxSTC_H_DOUBLESTRING, t.js_string);
    body_->StyleSetForeground(wxSTC_H_SINGLESTRING, t.js_string);
    body_->StyleSetForeground(wxSTC_H_NUMBER, t.js_number);
    body_->StyleSetForeground(wxSTC_H_OTHER, t.js_punct);
    body_->StyleSetForeground(wxSTC_H_COMMENT, t.dim);
    body_->StyleSetForeground(wxSTC_H_ENTITY, t.js_keyword);
    body_->StyleSetForeground(wxSTC_H_XMLSTART, t.js_keyword);
    body_->StyleSetForeground(wxSTC_H_XMLEND, t.js_keyword);
    body_->StyleSetForeground(wxSTC_H_CDATA, t.js_string);
    body_->SetLexer(wxSTC_LEX_XML);
    body_->SetProperty("fold", "1");
    body_->SetProperty("fold.compact", "0");
    body_->SetProperty("fold.html", "1");
  } else {
    body_->SetLexer(wxSTC_LEX_NULL);
  }
}

void HttpEditor::setup_body_stc() {
  const Theme& t = Theme::get();
  wxStyledTextCtrl* s = body_;
  const wxFont f = mono_font(10);
  s->StyleSetFont(wxSTC_STYLE_DEFAULT, f);
  s->StyleSetForeground(wxSTC_STYLE_DEFAULT, t.fg);
  s->StyleSetBackground(wxSTC_STYLE_DEFAULT, t.field);
  s->StyleClearAll();
  s->StyleSetForeground(wxSTC_STYLE_LINENUMBER, t.dim);
  s->StyleSetBackground(wxSTC_STYLE_LINENUMBER, t.field);
  s->StyleSetForeground(wxSTC_STYLE_BRACELIGHT, t.accent);
  apply_lexer(BodyMode::Json);
  s->SetMarginType(0, wxSTC_MARGIN_NUMBER);
  update_line_margin();
  s->SetMarginType(1, wxSTC_MARGIN_SYMBOL);
  s->SetMarginMask(1, wxSTC_MASK_FOLDERS);
  s->SetMarginWidth(1, FromDIP(12));
  s->SetMarginSensitive(1, true);
  s->SetFoldMarginColour(true, t.field);
  s->SetFoldMarginHiColour(true, t.field);
  const int markers[][2] = {{wxSTC_MARKNUM_FOLDEROPEN, wxSTC_MARK_BOXMINUS}, {wxSTC_MARKNUM_FOLDER, wxSTC_MARK_BOXPLUS},
                            {wxSTC_MARKNUM_FOLDERSUB, wxSTC_MARK_VLINE}, {wxSTC_MARKNUM_FOLDERTAIL, wxSTC_MARK_LCORNER},
                            {wxSTC_MARKNUM_FOLDEREND, wxSTC_MARK_BOXPLUSCONNECTED},
                            {wxSTC_MARKNUM_FOLDEROPENMID, wxSTC_MARK_BOXMINUSCONNECTED},
                            {wxSTC_MARKNUM_FOLDERMIDTAIL, wxSTC_MARK_TCORNER}};
  for (const auto& m : markers) {
    s->MarkerDefine(m[0], m[1], t.field, t.dim);
  }
  s->SetMarginLeft(FromDIP(4));
  s->SetTabWidth(2);
  s->SetUseTabs(false);
  s->SetIndent(2);
  s->SetTabIndents(true);
  s->SetBackSpaceUnIndents(true);
  s->SetWrapMode(wxSTC_WRAP_NONE);
  s->SetEOLMode(wxSTC_EOL_LF);
  s->SetCaretForeground(t.fg);
  s->SetSelBackground(true, t.dark ? wxColour(0x26, 0x4F, 0x78) : wxColour(0xAD, 0xD6, 0xFF));
  s->SetCaretLineVisible(true);
  s->SetCaretLineBackground(t.dark ? wxColour(0x2A, 0x2A, 0x2A) : wxColour(0xF5, 0xF8, 0xFC));
  s->SetLayoutCache(wxSTC_CACHE_PAGE);
  // Style the rest of a big document in idle time, so Ctrl+End in 17 MB does not lex it all at once.
  s->SetIdleStyling(wxSTC_IDLESTYLING_ALL);
  s->SetModEventMask(wxSTC_MOD_INSERTTEXT | wxSTC_MOD_DELETETEXT);
  s->SetScrollWidth(1);
  s->SetScrollWidthTracking(true);
  s->SetMouseDwellTime(350);
  s->CallTipSetBackground(t.dark ? wxColour(0x2B, 0x2B, 0x2B) : wxColour(0xFF, 0xFF, 0xF0));
  s->CallTipSetForeground(t.fg);
  setup_var_indicators(s);

  s->Bind(wxEVT_STC_CHANGE, [this](wxStyledTextEvent& e) {
    e.Skip();
    ++body_edits_;
    update_line_margin();
    if (loading_) return;
    body_hl_first_ = -1;
    if (body_mode_ == BodyMode::Json) lint_timer_.StartOnce(body_->GetLength() < 256 * 1024 ? 250 : 600);
    touched();
  });
  s->Bind(wxEVT_STC_UPDATEUI, [this](wxStyledTextEvent& e) {
    e.Skip();
    highlight_body_visible();
  });
  s->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
    const int k = e.GetKeyCode();
    const bool paste = (e.GetModifiers() == wxMOD_CONTROL && k == 'V') ||
                       (e.GetModifiers() == wxMOD_SHIFT && (k == WXK_INSERT || k == WXK_NUMPAD_INSERT));
    if (paste && paste_long_json()) return;
    // Undo past a swapped-in paste / format: the previous document comes back in O(1).
    if (e.GetModifiers() == wxMOD_CONTROL && k == 'Z' && prev_doc_ && !body_->CanUndo() && !pending_body_) {
      void* cur = body_->GetDocPointer();
      body_->AddRefDocument(cur);
      void* back = prev_doc_;
      prev_doc_ = nullptr;
      install_doc(back);
      body_->ReleaseDocument(back);  // the control holds it now
      release_doc_async(cur);
      minify_->set_value(prev_minify_);
      raw_body_.reset();
      ++body_edits_;
      note("Undid the paste");
      touched();
      return;
    }
    e.Skip();
  });
  // Ctrl+B (Postman) / Ctrl+Shift+F (Insomnia) beautify the JSON body. A char hook, so it runs
  // ahead of the frame's Ctrl+B (sidebar) while the body has the focus.
  s->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
    const int k = e.GetKeyCode();
    const int m = e.GetModifiers();
    if (body_mode_ == BodyMode::Json &&
        ((m == wxMOD_CONTROL && (k == 'B' || k == 'b')) || (m == (wxMOD_CONTROL | wxMOD_SHIFT) && (k == 'F' || k == 'f')))) {
      beautify();
      return;
    }
    e.Skip();
  });
  s->Bind(wxEVT_STC_MARGINCLICK, [this](wxStyledTextEvent& e) {
    if (e.GetMargin() == 1) body_->ToggleFold(body_->LineFromPosition(e.GetPosition()));
  });
  // Auto-indent: a new line keeps the previous line's indent, one level more after { or [.
  // Typing over a closer this editor inserted itself steps over it (like Postman / VS Code).
  s->IndicatorSetStyle(kIndAutoClose, wxSTC_INDIC_HIDDEN);
  s->IndicatorSetStyle(kIndJsonError, wxSTC_INDIC_SQUIGGLE);
  s->IndicatorSetForeground(kIndJsonError, t.js_error);
  s->Bind(wxEVT_CHAR, [this](wxKeyEvent& e) {
    if (text_mode(body_mode_) && step_over_closer(body_, e.GetUnicodeKey())) return;
    e.Skip();
  });
  s->Bind(wxEVT_STC_AUTOCOMP_COMPLETED, [this](wxStyledTextEvent& e) {
    e.Skip();
    CallAfter([this] { step_past_var_closer(body_); });
  });
  s->Bind(wxEVT_STC_CHARADDED, [this](wxStyledTextEvent& e) {
    e.Skip();
    const int key = e.GetKey();
    if (key == '{' || key == '[' || key == '"') {
      auto_close(key);
      if (key == '{') var_completion(body_);
      return;
    }
    if (key != '\n') return;
    const int line = body_->GetCurrentLine();
    if (line <= 0) return;
    const int base = body_->GetLineIndentation(line - 1);
    int indent = base;
    const int prev_end = body_->GetLineEndPosition(line - 1);
    const int prev_start = body_->PositionFromLine(line - 1);
    for (int p = prev_end - 1; p >= prev_start; --p) {
      const int c = body_->GetCharAt(p);
      if (c == ' ' || c == '\t' || c == '\r') continue;
      if (c == '{' || c == '[') indent += 2;
      break;
    }
    body_->SetLineIndentation(line, indent);
    const int at = body_->GetLineIndentPosition(line);
    body_->GotoPos(at);
    // Enter between "{" and "}": the closer goes on a line of its own.
    const int next = body_->GetCharAt(at);
    if (indent > base && (next == '}' || next == ']')) {
      const std::string nl = "\n" + std::string(static_cast<size_t>(base), ' ');
      body_->InsertTextRaw(at, nl.c_str());
      body_->GotoPos(at);
    }
  });
  s->Bind(wxEVT_STC_DWELLSTART, [this](wxStyledTextEvent& e) { var_tooltip(body_, e.GetPosition()); });
  s->Bind(wxEVT_STC_DWELLEND, [this](wxStyledTextEvent&) { body_->CallTipCancel(); });
}

wxWindow* HttpEditor::build_headers_page() {
  const Theme& t = Theme::get();
  auto* p = new wxPanel(headers_holder_);
  p->SetBackgroundColour(t.bg);
  auto* s = new wxBoxSizer(wxVERTICAL);
  auto* kp = new KvPanel(p, KvGrid::Kind::Headers, "Headers");
  headers_ = kp->grid();
  headers_->lookup = [this](const std::string& n) { return resolve(n); };
  headers_->on_change = [this] {
    if (!loading_) touched();
  };
  s->Add(kp, 1, wxEXPAND | wxTOP, FromDIP(4));
  // Headers the client adds by itself (Postman's hidden headers), listed so nothing on the wire
  // is a surprise; clicking one copies it into the table to override it.
  auto_btn_ = new FlatButton(kp, "Auto headers", FlatButton::Style::Subtle, true);
  auto_btn_->SetToolTip("Headers watt adds when it sends the request");
  kp->title_row()->Insert(2, auto_btn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
  auto_panel_ = new FramePanel(kp, t.bg, t.border);
  auto* as = new wxBoxSizer(wxVERTICAL);
  auto_title_ = new wxStaticText(auto_panel_, wxID_ANY,
                                 "Added when sent. A header of the same name in the table replaces it; click one to override it.");
  auto_title_->SetForegroundColour(t.dim);
  as->Add(auto_title_, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
  auto_grid_ = new wxFlexGridSizer(2, FromDIP(3), FromDIP(16));
  as->Add(auto_grid_, 0, wxALL, FromDIP(8));
  auto_panel_->SetSizer(as);
  auto_panel_->Hide();
  kp->add_above(auto_panel_);
  auto_btn_->on_click = [this, kp] {
    auto_shown_ = !auto_panel_->IsShown();
    auto_panel_->Show(auto_shown_);
    update_auto_headers(true);
    kp->Layout();
  };
  // curl -H @file / -b jar references (imported): read at send time, listed here.
  files_row_ = new wxPanel(p);
  files_row_->SetBackgroundColour(t.bg);
  auto* fs = new wxBoxSizer(wxHORIZONTAL);
  files_text_ = new wxStaticText(files_row_, wxID_ANY, "");
  files_text_->SetForegroundColour(t.dim);
  auto* rm = new wxButton(files_row_, wxID_ANY, "Remove");
  fs->Add(files_text_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
  fs->Add(rm, 0, wxALIGN_CENTER_VERTICAL);
  files_row_->SetSizer(fs);
  files_row_->Hide();
  s->Add(files_row_, 0, wxEXPAND | wxTOP, FromDIP(8));
  rm->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    base_.header_files.clear();
    base_.cookie_files.clear();
    files_row_->Hide();
    files_row_->GetParent()->Layout();
    touched();
  });
  p->SetSizer(s);
  return p;
}

std::vector<KvRow> HttpEditor::header_rows() const { return headers_ ? headers_->rows() : header_pending_; }

void HttpEditor::set_header_rows(const std::vector<KvRow>& rows) {
  if (headers_) headers_->set_rows(rows);
  else header_pending_ = rows;
}

void HttpEditor::ensure_page(size_t i) {
  if (i == 2 && !headers_) {
    wxWindowUpdateLocker lock(headers_holder_);
    headers_holder_->GetSizer()->Add(build_headers_page(), 1, wxEXPAND);
    const bool was = loading_;
    loading_ = true;
    headers_->set_rows(header_pending_);
    header_pending_.clear();
    loading_ = was;
    update_files_row();
    headers_holder_->Layout();
  } else if (i == 3 && !auth_book_) {
    wxWindowUpdateLocker lock(auth_holder_);
    auth_holder_->GetSizer()->Add(build_auth_page(), 1, wxEXPAND);
    push_auth();
    auth_holder_->Layout();
  } else if (i == 4 && !timeout_) {
    wxWindowUpdateLocker lock(opts_holder_);
    opts_holder_->GetSizer()->Add(build_options_page(), 1, wxEXPAND);
    push_opts();
    opts_holder_->Layout();
  } else if (i == 5 && !docs_) {
    wxWindowUpdateLocker lock(docs_holder_);
    docs_holder_->GetSizer()->Add(build_docs_page(), 1, wxEXPAND);
    docs_holder_->Layout();
  }
}

void HttpEditor::push_auth() {
  if (!auth_book_) return;
  const bool was = loading_;
  loading_ = true;
  bearer_->ChangeValue(wxString::FromUTF8(av_.bearer));
  basic_user_->ChangeValue(wxString::FromUTF8(av_.user));
  basic_pass_->ChangeValue(wxString::FromUTF8(av_.pass));
  basic_pass_alt_->ChangeValue(wxString::FromUTF8(av_.pass));
  key_name_->ChangeValue(wxString::FromUTF8(av_.key_name));
  key_value_->ChangeValue(wxString::FromUTF8(av_.key_value));
  key_where_->set_selection(av_.key_where);
  auth_btn_->SetLabel(kAuthLabels[static_cast<int>(auth_mode_)]);
  auth_book_->ChangeSelection(static_cast<size_t>(auth_mode_));
  loading_ = was;
}

void HttpEditor::push_opts() {
  if (!timeout_) return;
  const bool was = loading_;
  loading_ = true;
  timeout_->ChangeValue(wxString::Format("%ld", std::clamp<long>(ov_.timeout_ms, 0, 24L * 3600 * 1000)));
  redirects_->set_value(ov_.redirects);
  verify_tls_->set_value(ov_.verify_tls);
  fresh_->set_value(ov_.fresh);
  version_->set_selection(ov_.version);
  loading_ = was;
}

wxWindow* HttpEditor::build_auth_page() {
  const Theme& t = Theme::get();
  auto* p = new wxPanel(auth_holder_);
  p->SetBackgroundColour(t.bg);
  auto* s = new wxBoxSizer(wxVERTICAL);
  auto* top = new wxBoxSizer(wxHORIZONTAL);
  auto* l = new wxStaticText(p, wxID_ANY, "Type");
  l->SetForegroundColour(t.dim);
  top->Add(l, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
  auth_btn_ = new FlatButton(p, kAuthLabels[0], FlatButton::Style::Subtle, true);
  {
    // as wide as the widest type, so the button does not jump when the type changes
    int w = 0;
    for (const char* a : kAuthLabels) {
      auth_btn_->SetLabel(a);
      w = std::max(w, auth_btn_->GetBestSize().x);
    }
    auth_btn_->SetLabel(kAuthLabels[0]);
    auth_btn_->SetMinSize(wxSize(w, -1));
  }
  auth_btn_->SetToolTip("Authorization type (Up / Down change it)");
  auth_btn_->on_click = [this] {
    wxMenu menu;
    for (int i = 0; i < static_cast<int>(std::size(kAuthLabels)); ++i)
      menu.AppendRadioItem(i + 1, kAuthLabels[i])->Check(i == static_cast<int>(auth_mode_));
    const int sel = auth_btn_->GetPopupMenuSelectionFromUser(menu, wxPoint(0, auth_btn_->GetSize().y));
    if (sel >= 1 && sel <= static_cast<int>(std::size(kAuthLabels)) && sel - 1 != static_cast<int>(auth_mode_))
      set_auth_mode(static_cast<AuthMode>(sel - 1), true);
  };
  auth_btn_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
    const int k = e.GetKeyCode();
    const int n = static_cast<int>(std::size(kAuthLabels));
    const bool up = k == WXK_UP || k == WXK_NUMPAD_UP, down = k == WXK_DOWN || k == WXK_NUMPAD_DOWN;
    if ((up || down) && !e.HasAnyModifiers()) {
      const int i = (static_cast<int>(auth_mode_) + (down ? 1 : n - 1)) % n;
      set_auth_mode(static_cast<AuthMode>(i), true);
      return;
    }
    e.Skip();
  });
  top->Add(auth_btn_, 0, wxALIGN_CENTER_VERTICAL);
  s->Add(top, 0, wxTOP | wxBOTTOM, FromDIP(10));

  auth_book_ = new wxSimplebook(p);
  auth_book_->SetBackgroundColour(t.bg);
  auto page = [&](const char* hint) {
    auto* pg = new wxPanel(auth_book_);
    pg->SetBackgroundColour(t.bg);
    auto* grid = new wxFlexGridSizer(2, FromDIP(8), FromDIP(10));
    grid->AddGrowableCol(1, 1);
    pg->SetSizer(new wxBoxSizer(wxVERTICAL));
    auto* h = new wxStaticText(pg, wxID_ANY, hint);
    h->SetForegroundColour(t.dim);
    pg->GetSizer()->Add(h, 0, wxBOTTOM, FromDIP(10));
    pg->GetSizer()->Add(grid, 0, wxEXPAND);
    return std::make_pair(pg, grid);
  };
  auto var_field = [&](wxPanel* pg, wxFlexGridSizer* grid, const char* label, const char* hint, std::string* val) {
    auto* lb = new wxStaticText(pg, wxID_ANY, label);
    lb->SetForegroundColour(t.fg);
    wxWindow* frame = nullptr;
    wxStyledTextCtrl* f = make_var_field(pg, hint, val, &frame);
    grid->Add(lb, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(frame, 1, wxEXPAND);
    return f;
  };
  auto field = [&](wxPanel* pg, wxFlexGridSizer* grid, const char* label, const char* hint, std::string* val,
                   long style = 0) {
    auto* lb = new wxStaticText(pg, wxID_ANY, label);
    lb->SetForegroundColour(t.fg);
    auto* tc = new wxTextCtrl(pg, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(380), -1), style);
    tc->SetHint(hint);
    grid->Add(lb, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(tc, 1, wxEXPAND);
    tc->Bind(wxEVT_TEXT, [this, tc, val](wxCommandEvent&) {
      if (loading_) return;
      *val = tc->GetValue().utf8_string();
      if (auth_mode_ == AuthMode::ApiKey) sync_api_key_query();
      touched();
    });
    return tc;
  };
  {
    auto [pg, grid] = page("This request sends no authorization.");
    (void)grid;
    auth_book_->AddPage(pg, "none");
  }
  {
    auto [pg, grid] = page("Sent as the header  Authorization: Bearer <token>.  {{variables}} are allowed.");
    bearer_ = var_field(pg, grid, "Token", "{{token}}", &av_.bearer);
    auth_book_->AddPage(pg, "bearer");
  }
  {
    auto [pg, grid] = page("Sent as  Authorization: Basic base64(user:password)  unless the request sets its own Authorization header.");
    basic_user_ = var_field(pg, grid, "Username", "user", &av_.user);
    basic_pass_ = field(pg, grid, "Password", "password or {{secret}}", &av_.pass, wxTE_PASSWORD);
    // Show / Hide: wxTE_PASSWORD cannot change on a live control, so a plain twin takes its place.
    {
      grid->Detach(basic_pass_);  // the last item: the row below takes its place
      auto* row = new wxBoxSizer(wxHORIZONTAL);
      basic_pass_alt_ = new wxTextCtrl(pg, wxID_ANY, "", wxDefaultPosition, basic_pass_->GetSize());
      basic_pass_alt_->SetHint("password or {{secret}}");
      basic_pass_alt_->Hide();
      basic_pass_alt_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
        if (loading_) return;
        av_.pass = basic_pass_alt_->GetValue().utf8_string();
        touched();
      });
      auto* eye = new FlatButton(pg, "Hide", FlatButton::Style::Subtle);
      eye->SetMinSize(eye->GetBestSize());
      eye->SetLabel("Show");
      eye->SetToolTip("Show / hide the password");
      row->Add(basic_pass_, 1, wxALIGN_CENTER_VERTICAL);
      row->Add(basic_pass_alt_, 1, wxALIGN_CENTER_VERTICAL);
      row->Add(eye, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
      grid->Add(row, 1, wxEXPAND);
      eye->on_click = [this, eye, pg] {
        const bool was = loading_;
        loading_ = true;
        basic_pass_alt_->ChangeValue(basic_pass_->GetValue());
        loading_ = was;
        const bool focus = wxWindow::FindFocus() == basic_pass_;
        basic_pass_->Hide();
        basic_pass_alt_->Show();
        std::swap(basic_pass_, basic_pass_alt_);
        eye->SetLabel(basic_pass_->HasFlag(wxTE_PASSWORD) ? "Show" : "Hide");
        pg->Layout();
        if (focus) basic_pass_->SetFocus();
      };
    }
    auth_book_->AddPage(pg, "basic");
  }
  {
    auto [pg, grid] = page("An API key sent as a header or as a query parameter.");
    key_name_ = var_field(pg, grid, "Key", "X-API-Key", &av_.key_name);
    key_value_ = var_field(pg, grid, "Value", "{{api_key}}", &av_.key_value);
    auto* lb = new wxStaticText(pg, wxID_ANY, "Add to");
    lb->SetForegroundColour(t.fg);
    key_where_ = new ChoiceButton(pg, {"Header", "Query params"});
    grid->Add(lb, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(key_where_, 0);
    key_where_->on_select = [this](int sel) {
      av_.key_where = std::max(sel, 0);
      if (av_.key_where == 0 && !api_key_param_.empty()) {
        // moved from the query to a header: take it out of the URL
        auto rows = params_->rows();
        rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const KvRow& r) { return r.key == api_key_param_; }), rows.end());
        api_key_param_.clear();
        params_->set_rows(rows);
        params_changed();
      }
      sync_api_key_query();
      touched();
    };
    auth_book_->AddPage(pg, "apikey");
  }
  s->Add(auth_book_, 1, wxEXPAND);
  p->SetSizer(s);
  return p;
}

wxStyledTextCtrl* HttpEditor::make_var_field(wxWindow* parent, const char* hint, std::string* val, wxWindow** frame_out) {
  const Theme& t = Theme::get();
  auto* frame = new FramePanel(parent, t.field, t.border);
  auto* fs = new wxBoxSizer(wxHORIZONTAL);
  auto* s = new wxStyledTextCtrl(frame, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxWANTS_CHARS);
  const wxFont f = GetFont();
  s->StyleSetFont(wxSTC_STYLE_DEFAULT, f);
  s->StyleSetForeground(wxSTC_STYLE_DEFAULT, t.fg);
  s->StyleSetBackground(wxSTC_STYLE_DEFAULT, t.field);
  s->StyleClearAll();
  s->StyleSetFont(kStylePlaceholder, f);
  s->StyleSetForeground(kStylePlaceholder, t.dim);
  s->StyleSetBackground(kStylePlaceholder, t.field);
  s->SetLexer(wxSTC_LEX_NULL);
  for (int m = 0; m < 5; ++m) s->SetMarginWidth(m, 0);
  s->SetMarginLeft(0);
  s->SetMarginRight(0);
  s->SetUseHorizontalScrollBar(false);
  s->SetUseVerticalScrollBar(false);
  s->SetWrapMode(wxSTC_WRAP_NONE);
  s->SetCaretForeground(t.fg);
  s->SetSelBackground(true, t.dark ? wxColour(0x26, 0x4F, 0x78) : wxColour(0xAD, 0xD6, 0xFF));
  s->SetScrollWidth(1);
  s->SetScrollWidthTracking(true);
  s->SetModEventMask(wxSTC_MOD_INSERTTEXT | wxSTC_MOD_DELETETEXT);
  s->SetMouseDwellTime(350);
  s->CallTipSetBackground(t.dark ? wxColour(0x2B, 0x2B, 0x2B) : wxColour(0xFF, 0xFF, 0xF0));
  s->CallTipSetForeground(t.fg);
  setup_var_indicators(s);
  s->IndicatorSetStyle(kIndAutoClose, wxSTC_INDIC_HIDDEN);
  s->EOLAnnotationSetVisible(wxSTC_EOLANNOTATION_STANDARD);
  const wxString placeholder = wxString::FromUTF8(hint);
  s->EOLAnnotationSetText(0, placeholder);
  s->EOLAnnotationSetStyle(0, kStylePlaceholder);
  const int h = s->TextHeight(0);
  s->SetMinSize(wxSize(FromDIP(380), h));
  s->SetMaxSize(wxSize(-1, h));
  fs->Add(s, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, FromDIP(5));
  frame->SetSizer(fs);
  *frame_out = frame;

  s->Bind(wxEVT_KEY_DOWN, [s](wxKeyEvent& e) {
    const int k = e.GetKeyCode();
    if (s->AutoCompActive() && !e.HasAnyModifiers()) {
      e.Skip();  // the {{variable}} list takes its keys
      return;
    }
    if (k == WXK_TAB && !e.ControlDown()) {
      s->Navigate(e.ShiftDown() ? wxNavigationKeyEvent::IsBackward : wxNavigationKeyEvent::IsForward);
    } else if ((k == WXK_RETURN || k == WXK_NUMPAD_ENTER) && !e.ControlDown()) {
      // one line: nothing to insert (Ctrl+Enter sends)
    } else if ((k == WXK_UP || k == WXK_DOWN || k == WXK_PAGEUP || k == WXK_PAGEDOWN) && !e.HasAnyModifiers()) {
    } else {
      e.Skip();
    }
  });
  s->Bind(wxEVT_CHAR, [this, s](wxKeyEvent& e) {
    if (step_over_closer(s, e.GetUnicodeKey())) return;
    e.Skip();
  });
  s->Bind(wxEVT_STC_CHARADDED, [this, s](wxStyledTextEvent& e) {
    e.Skip();
    if (e.GetKey() == '{') var_completion(s);
  });
  s->Bind(wxEVT_STC_AUTOCOMP_COMPLETED, [this, s](wxStyledTextEvent& e) {
    e.Skip();
    CallAfter([this, s] { step_past_var_closer(s); });
  });
  s->Bind(wxEVT_SET_FOCUS, [frame](wxFocusEvent& e) {
    e.Skip();
    frame->set_focus(true);
  });
  s->Bind(wxEVT_KILL_FOCUS, [frame, s](wxFocusEvent& e) {
    e.Skip();
    frame->set_focus(false);
    s->CallTipCancel();
  });
  s->Bind(wxEVT_STC_MODIFIED, [this, s](wxStyledTextEvent& e) {
    e.Skip();
    // A pasted line break would end the header: keep one line.
    if ((e.GetModificationType() & wxSTC_MOD_INSERTTEXT) && e.GetText().find_first_of("\r\n") != wxString::npos)
      CallAfter([s] {
        wxString v = s->GetValue();
        v.Replace("\r", "");
        v.Replace("\n", "");
        s->SetValue(v);
        s->GotoPos(s->GetLength());
      });
  });
  s->Bind(wxEVT_STC_CHANGE, [this, s, val, placeholder](wxStyledTextEvent& e) {
    e.Skip();
    const int len = s->GetLength();
    s->EOLAnnotationClearAll();
    if (!len) {
      s->EOLAnnotationSetText(0, placeholder);
      s->EOLAnnotationSetStyle(0, kStylePlaceholder);
    }
    highlight_vars(s, 0, len);
    if (loading_) return;
    *val = std::string(s->GetCharacterPointer(), static_cast<size_t>(len));
    if (auth_mode_ == AuthMode::ApiKey) sync_api_key_query();
    touched();
  });
  s->Bind(wxEVT_STC_DWELLSTART, [this, s](wxStyledTextEvent& e) { var_tooltip(s, e.GetPosition()); });
  s->Bind(wxEVT_STC_DWELLEND, [s](wxStyledTextEvent&) { s->CallTipCancel(); });
  return s;
}

wxWindow* HttpEditor::build_options_page() {
  const Theme& t = Theme::get();
  auto* p = new wxPanel(opts_holder_);
  p->SetBackgroundColour(t.bg);
  auto* s = new wxBoxSizer(wxVERTICAL);
  auto* grid = new wxFlexGridSizer(2, FromDIP(10), FromDIP(14));
  auto label = [&](const char* text) {
    auto* l = new wxStaticText(p, wxID_ANY, text);
    l->SetForegroundColour(t.fg);
    grid->Add(l, 0, wxALIGN_CENTER_VERTICAL);
  };
  label("Timeout");
  {
    // A themed number field (ms): digits only, Up / Down step by 1000.
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    timeout_frame_ = new FramePanel(p, t.field, t.border);
    auto* fs = new wxBoxSizer(wxHORIZONTAL);
    timeout_ = new wxTextCtrl(timeout_frame_, wxID_ANY, "30000", wxDefaultPosition, wxSize(FromDIP(90), -1),
                              wxBORDER_NONE | wxTE_PROCESS_ENTER);
    timeout_->SetBackgroundColour(t.field);
    timeout_->SetForegroundColour(t.fg);
    timeout_->SetMaxLength(8);
    fs->Add(timeout_, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(8));
    timeout_frame_->SetSizer(fs);
    timeout_frame_->SetMinSize(wxSize(-1, timeout_->GetBestSize().y + FromDIP(10)));
    row->Add(timeout_frame_, 0, wxALIGN_CENTER_VERTICAL);
    auto* unit = new wxStaticText(p, wxID_ANY, "ms   (0 = no timeout)");
    unit->SetForegroundColour(t.dim);
    row->Add(unit, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    grid->Add(row, 0);
  }
  label("HTTP version");
  version_ = new ChoiceButton(p, {"Auto (HTTP/2 over TLS when offered, else HTTP/1.1)", "HTTP/1.1", "HTTP/2",
                                  "HTTP/2 prior knowledge (h2c)"});
  grid->Add(version_, 0);
  s->Add(grid, 0, wxTOP | wxBOTTOM, FromDIP(12));
  redirects_ = new CheckButton(p, "Follow redirects");
  verify_tls_ = new CheckButton(p, "Verify TLS certificates");
  fresh_ = new CheckButton(p, "Fresh connection (no reuse: measures DNS + connect + TLS every time)");
  for (auto* c : {redirects_, verify_tls_, fresh_}) {
    s->Add(c, 0, wxBOTTOM, FromDIP(8));
    c->on_change = [this] {
      if (loading_) return;
      ov_.redirects = redirects_->value();
      ov_.verify_tls = verify_tls_->value();
      ov_.fresh = fresh_->value();
      touched();
    };
  }
  auto timeout_changed = [this] {
    if (loading_) return;
    long v = 0;
    if (!timeout_->GetValue().ToLong(&v) || v < 0) v = 0;
    ov_.timeout_ms = std::min<long>(v, 24L * 3600 * 1000);
    touched();
  };
  timeout_->Bind(wxEVT_TEXT, [timeout_changed](wxCommandEvent&) { timeout_changed(); });
  timeout_->Bind(wxEVT_CHAR, [](wxKeyEvent& e) {
    const int k = e.GetKeyCode();
    // digits and editing keys only (no letters, no minus)
    if (k >= 32 && k < 127 && !(k >= '0' && k <= '9') && !e.ControlDown()) return;
    e.Skip();
  });
  timeout_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
    const int k = e.GetKeyCode();
    const bool up = k == WXK_UP || k == WXK_NUMPAD_UP, down = k == WXK_DOWN || k == WXK_NUMPAD_DOWN;
    if ((up || down) && !e.HasAnyModifiers()) {
      long v = 0;
      if (!timeout_->GetValue().ToLong(&v)) v = 0;
      v = std::clamp<long>(v + (up ? 1000 : -1000), 0, 24L * 3600 * 1000);
      timeout_->SetValue(wxString::Format("%ld", v));
      timeout_->SetInsertionPointEnd();
      return;
    }
    e.Skip();
  });
  timeout_->Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) {
    e.Skip();
    timeout_frame_->set_focus(true);
  });
  timeout_->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) {
    e.Skip();
    timeout_frame_->set_focus(false);
    if (timeout_->GetValue().empty()) timeout_->ChangeValue("0");
  });
  version_->on_select = [this](int sel) {
    if (loading_) return;
    ov_.version = std::max(sel, 0);
    touched();
  };
  p->SetSizer(s);
  return p;
}

wxWindow* HttpEditor::build_docs_page() {
  const Theme& t = Theme::get();
  auto* p = new wxPanel(docs_holder_);
  p->SetBackgroundColour(t.bg);
  auto* s = new wxBoxSizer(wxVERTICAL);
  auto* hint = new wxStaticText(p, wxID_ANY, "Notes about this request (Markdown), saved with it");
  hint->SetForegroundColour(t.dim);
  s->Add(hint, 0, wxTOP | wxBOTTOM, FromDIP(8));
  auto* frame = new FramePanel(p, t.field, t.border);
  auto* fs = new wxBoxSizer(wxVERTICAL);
  docs_ = new wxStyledTextCtrl(frame, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
  wxStyledTextCtrl* d = docs_;
  d->SetLexer(wxSTC_LEX_MARKDOWN);
  d->StyleSetFont(wxSTC_STYLE_DEFAULT, GetFont());
  d->StyleSetForeground(wxSTC_STYLE_DEFAULT, t.fg);
  d->StyleSetBackground(wxSTC_STYLE_DEFAULT, t.field);
  d->StyleClearAll();
  for (int h : {wxSTC_MARKDOWN_HEADER1, wxSTC_MARKDOWN_HEADER2, wxSTC_MARKDOWN_HEADER3, wxSTC_MARKDOWN_HEADER4,
                wxSTC_MARKDOWN_HEADER5, wxSTC_MARKDOWN_HEADER6}) {
    d->StyleSetForeground(h, t.js_keyword);
    d->StyleSetBold(h, true);
  }
  d->StyleSetBold(wxSTC_MARKDOWN_STRONG1, true);
  d->StyleSetBold(wxSTC_MARKDOWN_STRONG2, true);
  d->StyleSetItalic(wxSTC_MARKDOWN_EM1, true);
  d->StyleSetItalic(wxSTC_MARKDOWN_EM2, true);
  for (int c : {wxSTC_MARKDOWN_CODE, wxSTC_MARKDOWN_CODE2, wxSTC_MARKDOWN_CODEBK}) {
    d->StyleSetFont(c, mono_font(GetFont().GetPointSize()));
    d->StyleSetForeground(c, t.js_string);
  }
  d->StyleSetForeground(wxSTC_MARKDOWN_LINK, t.accent);
  d->StyleSetForeground(wxSTC_MARKDOWN_ULIST_ITEM, t.js_number);
  d->StyleSetForeground(wxSTC_MARKDOWN_OLIST_ITEM, t.js_number);
  d->StyleSetForeground(wxSTC_MARKDOWN_BLOCKQUOTE, t.dim);
  d->SetWrapMode(wxSTC_WRAP_WORD);
  d->SetMarginWidth(1, 0);
  d->SetMarginLeft(FromDIP(6));
  d->SetUseHorizontalScrollBar(false);
  d->SetCaretForeground(t.fg);
  d->SetSelBackground(true, t.dark ? wxColour(0x26, 0x4F, 0x78) : wxColour(0xAD, 0xD6, 0xFF));
  d->SetText(wxString::FromUTF8(docs_text_));
  d->EmptyUndoBuffer();
  d->SetSavePoint();
  d->Bind(wxEVT_STC_CHANGE, [this](wxStyledTextEvent& e) {
    e.Skip();
    if (loading_) return;
    docs_text_ = docs_->GetText().utf8_string();
    touched();
  });
  d->Bind(wxEVT_SET_FOCUS, [frame](wxFocusEvent& e) {
    e.Skip();
    frame->set_focus(true);
  });
  d->Bind(wxEVT_KILL_FOCUS, [frame](wxFocusEvent& e) {
    e.Skip();
    frame->set_focus(false);
  });
  fs->Add(d, 1, wxEXPAND | wxALL, FromDIP(6));
  frame->SetSizer(fs);
  s->Add(frame, 1, wxEXPAND | wxBOTTOM, FromDIP(4));
  p->SetSizer(s);
  return p;
}

// ------------------------------------------------------------------ load / store

void HttpEditor::load(const SavedRequest& r) {
  loading_ = true;
  hide_banner();
  base_ = r;
  base_.http.body.clear();
  base_.http.body.shrink_to_fit();

  set_method(r.http.method.empty() ? "GET" : r.http.method);
  set_url_text(r.http.url);
  path_values_.clear();
  for (const auto& [k, v] : r.path_params) path_values_[k] = v;
  sync_path_rows();
  update_preview();
  url_->GotoPos(0);

  std::vector<KvRow> prow;
  for (const auto& q : parse_query_decoded(split_url(r.http.url).query)) prow.push_back({q.key, q.value, true, false, q.bare});
  for (const auto& [k, v] : r.disabled_params) prow.push_back({k, v, false, false, false});
  params_->set_rows(prow);
  if (path_panel_ && path_panel_->IsShown()) layout_params_page();

  // Headers (an auth header is shown on the Auth tab instead, and put back at its place).
  std::vector<KvRow> hrows;
  auth_header_index_ = SIZE_MAX;
  AuthMode am = AuthMode::None;
  std::string bearer, key_name, key_value;
  for (size_t i = 0; i < r.http.headers.size(); ++i) {
    const auto& [k, v] = r.http.headers[i];
    const bool enabled = !r.disabled_headers.count(i);
    if (am == AuthMode::None && !r.basic_auth.enabled && enabled) {
      if (iequals(k, "Authorization") && istarts_with(v, "Bearer ")) {
        am = AuthMode::Bearer;
        bearer = v.substr(7);
        auth_header_index_ = i;
        continue;
      }
      if (is_api_key_header(k)) {
        am = AuthMode::ApiKey;
        key_name = k;
        key_value = v;
        auth_header_index_ = i;
        continue;
      }
    }
    hrows.push_back({k, v, enabled, false, false});
  }
  set_header_rows(hrows);
  if (r.basic_auth.enabled) am = AuthMode::Basic;
  av_.bearer = bearer;
  av_.user = r.basic_auth.user;
  av_.pass = r.basic_auth.password;
  av_.key_name = key_name;
  av_.key_value = key_value;
  av_.key_where = 0;
  api_key_param_.clear();
  auth_mode_ = am;
  push_auth();
  set_auth_mode(am, false);

  update_files_row();

  // Body.
  std::string ct;
  for (size_t i = 0; i < r.http.headers.size(); ++i)
    if (iequals(r.http.headers[i].first, "Content-Type") && !r.disabled_headers.count(i)) ct = r.http.headers[i].second;
  BodyMode bm;
  if (!r.form.empty()) bm = BodyMode::Multipart;
  else if (!r.body_file.empty()) bm = BodyMode::Binary;
  else if (!r.body_data.empty()) bm = BodyMode::CurlData;
  else if (ct.find("x-www-form-urlencoded") != std::string::npos) bm = BodyMode::UrlEncoded;
  else if (r.http.body.empty()) bm = BodyMode::None;
  else if (ct.find("json") != std::string::npos || looks_like_json(r.http.body)) bm = BodyMode::Json;
  else if (ct.find("xml") != std::string::npos || looks_like_xml(r.http.body)) bm = BodyMode::Xml;
  else bm = BodyMode::Text;

  std::vector<KvRow> mrows;
  for (const auto& part : r.form) mrows.push_back({part.name, part.value, part.enabled, part.kind != FormPart::Kind::Text, false});
  set_form_rows(1, mrows);
  loaded_urlenc_.clear();
  loaded_urlenc_rows_.clear();
  if (bm == BodyMode::UrlEncoded) {
    loaded_urlenc_ = r.http.body;
    for (const auto& kv : parse_form_body(r.http.body)) loaded_urlenc_rows_.push_back({kv.key, kv.value, true, false, false});
  }
  set_form_rows(0, loaded_urlenc_rows_);
  binary_val_ = r.body_file;
  if (binary_path_) binary_path_->ChangeValue(wxString::FromUTF8(r.body_file));
  {
    std::string t;
    for (const auto& d : r.body_data) {
      static const char* names[] = {"--data", "--data-binary", "--data-raw", "--data-urlencode", "--json"};
      t += std::string(names[static_cast<int>(d.kind)]) + " " + d.value + "\n";
    }
    curl_data_label_ = wxString::FromUTF8("Body built from curl arguments (files are read when the request is sent):\n\n" + t);
    if (curl_data_text_) curl_data_text_->SetLabel(curl_data_label_);
  }
  ++body_load_gen_;
  pending_body_.reset();
  minify_->set_value(false);
  raw_body_.reset();
  raw_pending_.reset();
  if (prev_doc_) {
    release_doc_async(prev_doc_);
    prev_doc_ = nullptr;
  }
  body_->SetReadOnly(false);
  body_->ClearAll();
  if (text_mode(bm) && !r.http.body.empty()) {
    if (r.http.body.size() < kAsyncBody) {
      body_->AddTextRaw(r.http.body.data(), static_cast<int>(r.http.body.size()));
    } else {
      // Read-only and empty for the few ms the worker needs; store() returns pending_body_.
      pending_body_ = std::make_shared<const std::string>(r.http.body);
      body_->SetReadOnly(true);
      build_body_doc(pending_body_, false, body_load_gen_, 0);
    }
  }
  body_->EmptyUndoBuffer();
  body_->SetSavePoint();
  body_->GotoPos(0);
  body_hl_first_ = -1;
  set_body_mode(bm, false);
  if (bm == BodyMode::Json && r.http.body.size() >= kLongLine) {
    const size_t longest = longest_line(r.http.body);
    if (longest >= kLongLine)
      show_info(wxString::Format("The body has a %s line", human_size(longest)),
                "Editing a line this long is slow: every keystroke lays the whole line out again. Beautify "
                "re-indents the JSON in a few ms on a worker thread (whitespace only, values kept as written).",
                "Beautify", [this] {
                  hide_banner();
                  beautify();
                });
  }

  // Docs.
  docs_text_ = r.docs;
  if (docs_) {
    docs_->SetText(wxString::FromUTF8(docs_text_));
    docs_->EmptyUndoBuffer();
  }

  // Options.
  ov_.timeout_ms = std::clamp<long>(r.http.timeout_ms, 0, 24L * 3600 * 1000);
  ov_.redirects = r.http.follow_redirects;
  ov_.verify_tls = r.http.verify_tls;
  ov_.fresh = r.http.fresh_connection;
  ov_.version = static_cast<int>(r.http.version);
  push_opts();

  // Show the body of a request that has one (unless the user is on another tab than Params).
  if (first_load_ || tabs_->selection() == 0 || tabs_->selection() == 1) {
    first_load_ = false;
    const size_t tab = (bm != BodyMode::None && prow.empty()) ? 1 : (tabs_->selection() == 1 && bm != BodyMode::None ? 1 : 0);
    tabs_->select(tab);
    book_->ChangeSelection(tab);
    sync_body_book();
  }
  update_badges();
  loading_ = false;
}

// curl -H @file / -b jar references (base_), listed on the Headers page.
void HttpEditor::update_files_row() {
  if (!files_row_) return;
  if (!base_.header_files.empty() || !base_.cookie_files.empty()) {
    std::string t = "Also sent:";
    for (const auto& f : base_.header_files) t += "  headers from " + f + ";";
    for (const auto& f : base_.cookie_files) t += "  cookies from " + f + ";";
    files_text_->SetLabel(wxString::FromUTF8(t));
    files_row_->Show();
  } else {
    files_row_->Hide();
  }
  files_row_->GetParent()->Layout();
}

void HttpEditor::store_head(SavedRequest& r) const {
  r.http.method = method_str_;
  // A space or other character libcurl rejects, typed straight into the URL, is sent encoded.
  r.http.url = normalize_url(url_text());
}

void HttpEditor::store(SavedRequest& r) const {
  store_head(r);
  r.docs = docs_text_;
  r.path_params.clear();
  for (const auto& n : path_names_) {
    const auto it = path_values_.find(n);
    r.path_params.emplace_back(n, it == path_values_.end() ? std::string() : it->second);
  }
  if (sending_) r.http.url = with_path(r.http.url);
  const Workspace* send_ws = sending_ ? host_->workspace() : nullptr;
  if (send_ws && r.http.url.find("{{") != std::string::npos) {
    // The shell expands {{vars}} without knowing where they stand; when that would send a
    // different URL (a value with a space or '&' in the query), hand it the URL as sent.
    if (std::string u = sent_url(); u != send_ws->expand(r.http.url)) r.http.url = std::move(u);
  }

  std::vector<Header> hs;
  std::set<size_t> disabled;
  for (const auto& row : header_rows()) {
    // Never a tab, CR or LF on the wire (they would split or corrupt the header block).
    std::string k = clean_header_name(row.key);
    if (k.empty()) continue;
    if (!row.enabled) disabled.insert(hs.size());
    hs.emplace_back(std::move(k), clean_header_value(row.value));
  }
  auto insert_auth = [&](std::string k, std::string v) {
    const size_t at = std::min(auth_header_index_, hs.size());
    hs.insert(hs.begin() + static_cast<std::ptrdiff_t>(at), {std::move(k), std::move(v)});
    std::set<size_t> shifted;
    for (size_t d : disabled) shifted.insert(d >= at ? d + 1 : d);
    disabled = std::move(shifted);
  };
  if (auth_mode_ == AuthMode::Bearer) {
    const std::string& tok = av_.bearer;
    if (!tok.empty()) insert_auth("Authorization", "Bearer " + tok);
  } else if (auth_mode_ == AuthMode::ApiKey && av_.key_where == 0) {
    if (!av_.key_name.empty()) insert_auth(av_.key_name, av_.key_value);
  }
  r.http.headers = std::move(hs);
  r.disabled_headers = std::move(disabled);
  // Unchecked params are not in the URL: kept beside it, so they survive a save.
  r.disabled_params.clear();
  for (const auto& row : params_->rows())
    if (!row.enabled && !trim(row.key).empty()) r.disabled_params.emplace_back(row.key, row.value);

  r.basic_auth = {};
  if (auth_mode_ == AuthMode::Basic) {
    r.basic_auth.enabled = true;
    r.basic_auth.user = av_.user;
    r.basic_auth.password = av_.pass;
  }

  r.form.clear();
  r.body_file.clear();
  r.body_data.clear();
  r.http.body.clear();
  switch (body_mode_) {
    case BodyMode::None:
      break;
    case BodyMode::Json:
    case BodyMode::Text:
    case BodyMode::Xml: {
      if (pending_body_) {
        r.http.body = *pending_body_;
        break;
      }
      const std::string_view shown(body_->GetCharacterPointer(), static_cast<size_t>(body_->GetLength()));
      if (body_mode_ == BodyMode::Json && minify_->value()) {
        if (raw_body_ && raw_edits_ == body_edits_) r.http.body = *raw_body_;  // as pasted
        else r.http.body = minify_json(shown);
        break;
      }
      // One copy straight out of Scintilla's buffer (GetTextRaw would copy twice).
      r.http.body.assign(shown);
      break;
    }
    case BodyMode::UrlEncoded: {
      const auto rows = form_rows(0);
      bool same = rows.size() == loaded_urlenc_rows_.size();
      for (size_t i = 0; same && i < rows.size(); ++i)
        same = rows[i].key == loaded_urlenc_rows_[i].key && rows[i].value == loaded_urlenc_rows_[i].value && rows[i].enabled;
      if (same) {
        r.http.body = loaded_urlenc_;
      } else {
        std::vector<KvItem> items;
        for (const auto& row : rows) items.push_back({row.key, row.value, row.enabled, false});
        r.http.body = build_urlencoded(items);
      }
      // Each {{var}} value form-encoded ("ann marie" -> "ann+marie", "a&b" stays one field).
      if (send_ws && r.http.body.find("{{") != std::string::npos) {
        if (std::string b = sent_form(r.http.body); b != send_ws->expand(r.http.body)) r.http.body = std::move(b);
      }
      break;
    }
    case BodyMode::Multipart: {
      const auto rows = form_rows(1);
      for (size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        FormPart p;
        // Keep what the table does not show (filename, type, <file, --form-string) of an
        // unchanged part.
        if (i < base_.form.size() && base_.form[i].name == row.key && base_.form[i].value == row.value &&
            (base_.form[i].kind != FormPart::Kind::Text) == row.file)
          p = base_.form[i];
        p.name = row.key;
        p.value = row.value;
        if (row.file && p.kind == FormPart::Kind::Text) p.kind = FormPart::Kind::File;
        if (!row.file) p.kind = FormPart::Kind::Text;
        p.enabled = row.enabled;
        r.form.push_back(std::move(p));
      }
      break;
    }
    case BodyMode::Binary:
      r.body_file = binary_val_;
      break;
    case BodyMode::CurlData:
      r.body_data = base_.body_data;
      break;
  }
  r.header_files = base_.header_files;
  r.cookie_files = base_.cookie_files;

  r.http.timeout_ms = ov_.timeout_ms;
  r.http.follow_redirects = ov_.redirects;
  r.http.verify_tls = ov_.verify_tls;
  r.http.fresh_connection = ov_.fresh;
  r.http.version = static_cast<HttpVersion>(ov_.version);
}

void HttpEditor::set_in_flight(bool f) {
  in_flight_ = f;
  send_->SetLabel(f ? "Cancel" : "Send");
  send_->SetToolTip(f ? "Cancel the request in flight" : "Send (Ctrl+Enter)");
}

void HttpEditor::focus_url() {
  url_->SetFocus();
  url_->SelectAll();
}

// ------------------------------------------------------------------ behaviour

void HttpEditor::touched() {
  if (!banner_missing_.empty() && banner_ && banner_->IsShown()) {
    auto now = unresolved_vars(nullptr);
    for (const auto& n : empty_path_vars()) now.push_back(":" + n);
    if (std::none_of(banner_missing_.begin(), banner_missing_.end(),
                     [&](const std::string& v) { return std::find(now.begin(), now.end(), v) != now.end(); }))
      hide_banner();
  }
  update_badges();
  host_->changed();
}

void HttpEditor::update_badges() {
  // Rows with a value but no key are not counted (they are not sent): the badge turns to the
  // warning colour instead.
  const size_t np = params_->enabled_count(), po = params_->orphan_count();
  tabs_->set_badge(0, np || po ? wxString::Format("%zu", np) : wxString(), po > 0);
  tabs_->set_badge(1, body_mode_ == BodyMode::None ? wxString() : wxString::FromUTF8("\xE2\x97\x8F"));
  size_t nh = 0, ho = 0;
  if (headers_) {
    nh = headers_->enabled_count();
    ho = headers_->orphan_count();
  } else {
    for (const auto& r : header_pending_) {
      if (trim(r.key).empty()) ++ho;
      else if (r.enabled) ++nh;
    }
  }
  if ((auth_mode_ == AuthMode::Bearer || (auth_mode_ == AuthMode::ApiKey && av_.key_where == 0)))
    ++nh;
  tabs_->set_badge(2, nh || ho ? wxString::Format("%zu", nh) : wxString(), ho > 0);
  tabs_->set_badge(3, auth_mode_ == AuthMode::None ? wxString() : wxString::FromUTF8("\xE2\x97\x8F"));
  const bool opts = ov_.timeout_ms != 30000 || !ov_.redirects || !ov_.verify_tls || ov_.fresh || ov_.version != 0;
  tabs_->set_badge(4, opts ? wxString::FromUTF8("\xE2\x97\x8F") : wxString());
  tabs_->set_badge(5, docs_text_.empty() ? wxString() : wxString::FromUTF8("\xE2\x97\x8F"));
  update_auto_headers();
}

// What HttpClient / libcurl add on their own (see src/core/http_client.cpp).
void HttpEditor::update_auto_headers(bool force) {
  if (!auto_btn_ || (!force && book_->GetSelection() != 2)) return;  // only while the Headers tab is shown
  std::vector<std::pair<std::string, std::string>> rows;
  std::string url = url_text();
  if (const Workspace* ws = host_->workspace(); ws && url.find("{{") != std::string::npos) url = ws->expand(url);
  {
    std::string_view u = url;
    if (const size_t sch = u.find("://"); sch != std::string_view::npos) u.remove_prefix(sch + 3);
    u = u.substr(0, u.find_first_of("/?#"));
    if (const size_t at = u.rfind('@'); at != std::string_view::npos) u.remove_prefix(at + 1);
    rows.emplace_back("Host", u.empty() ? std::string("(from the URL)") : std::string(u));
  }
  rows.emplace_back("User-Agent", "watt/0.1");
  rows.emplace_back("Accept", "*/*");
  rows.emplace_back("Accept-Encoding", "deflate, gzip");
  if (auth_mode_ == AuthMode::Basic) rows.emplace_back("Authorization", "Basic <user:password from the Auth tab>");
  switch (body_mode_) {
    case BodyMode::None:
      break;
    case BodyMode::Json:
    case BodyMode::Text:
    case BodyMode::Xml:
      rows.emplace_back("Content-Length",
                        pending_body_ ? std::to_string(pending_body_->size()) : std::to_string(body_->GetLength()));
      break;
    case BodyMode::Multipart:
      rows.emplace_back("Content-Type", "multipart/form-data; boundary=<generated>");
      rows.emplace_back("Content-Length", "<computed when sent>");
      break;
    default:
      rows.emplace_back("Content-Length", "<computed when sent>");
      break;
  }
  if (const wxString label = wxString::Format("Auto headers (%zu)", rows.size()); label != auto_btn_->GetLabel()) {
    auto_btn_->SetLabel(label);
    auto_btn_->InvalidateBestSize();
    auto_btn_->GetParent()->Layout();
  }
  if (!auto_panel_->IsShown()) return;
  std::set<std::string> overridden;
  for (const auto& r : header_rows())
    if (r.enabled) overridden.insert(canonical_header_name(r.key));
  if (auth_mode_ == AuthMode::Bearer) overridden.insert("Authorization");
  auto sig = rows;
  for (auto& [k, v] : sig)
    if (overridden.count(canonical_header_name(k))) v += '\x01';
  if (sig == auto_shown_rows_) return;
  auto_shown_rows_ = std::move(sig);
  const Theme& t = Theme::get();
  auto_panel_->Freeze();
  auto_grid_->Clear(true);
  for (const auto& [k, v] : rows) {
    const bool over = overridden.count(canonical_header_name(k)) > 0;
    auto* kl = new wxStaticText(auto_panel_, wxID_ANY, wxString::FromUTF8(k));
    auto* vl = new wxStaticText(auto_panel_, wxID_ANY, wxString::FromUTF8(over ? v + "   (replaced by your header)" : v));
    kl->SetForegroundColour(over ? t.dim : t.fg);
    vl->SetForegroundColour(t.dim);
    if (over) {
      wxFont f = kl->GetFont();
      f.SetStrikethrough(true);
      kl->SetFont(f);
    } else {
      const bool computed = v.front() == '<' || v.front() == '(';
      kl->SetCursor(wxCursor(wxCURSOR_HAND));
      kl->SetToolTip("Add " + wxString::FromUTF8(k) + " to the table to change or blank it");
      kl->Bind(wxEVT_LEFT_UP, [this, name = k, value = computed ? std::string() : v](wxMouseEvent&) {
        auto hrows = headers_->rows();
        hrows.push_back({name, value, true, false, false});
        headers_->set_rows(hrows);
        touched();
        update_auto_headers(true);
        const int r = static_cast<int>(hrows.size()) - 1;
        headers_->SetFocus();
        headers_->SetGridCursor(r, 2);
        headers_->MakeCellVisible(r, 2);
      });
    }
    auto_grid_->Add(kl, 0, wxALIGN_CENTER_VERTICAL);
    auto_grid_->Add(vl, 0, wxALIGN_CENTER_VERTICAL);
  }
  auto_panel_->Thaw();
  auto_panel_->GetParent()->Layout();
}

void HttpEditor::set_method(const std::string& m) {
  method_str_ = m;
  method_->SetLabel(wxString::FromUTF8(m));
  method_->set_text_colour(method_colour(m));
  url_box_->Layout();
}

void HttpEditor::show_method_menu() {
  wxMenu menu;
  int id = 1;
  for (const char* m : kMethods) {
    auto* it = new wxMenuItem(&menu, id++, m);
    // A dot in the method's colour (the button's text colour): the native dark menu ignores
    // per-item text colours.
    const int px = FromDIP(16);
    wxImage img(px, px);
    img.InitAlpha();
    const wxColour c = method_colour(m);
    const double r = px * 0.28, cx = (px - 1) / 2.0, cy = (px - 1) / 2.0;
    for (int y = 0; y < px; ++y)
      for (int x = 0; x < px; ++x) {
        const double d = std::hypot(x - cx, y - cy);
        const double a = std::clamp(r + 0.5 - d, 0.0, 1.0);  // anti-aliased edge
        img.SetRGB(x, y, c.Red(), c.Green(), c.Blue());
        img.SetAlpha(x, y, static_cast<unsigned char>(a * 255));
      }
    it->SetBitmap(wxBitmap(img));
    menu.Append(it);
  }
  menu.AppendSeparator();
  menu.Append(100, wxString::FromUTF8("Custom\xE2\x80\xA6"));
  const int sel = GetPopupMenuSelectionFromUser(menu, method_->GetPosition() + url_box_->GetPosition() +
                                                          wxPoint(0, method_->GetSize().y + FromDIP(4)));
  std::string m;
  if (sel >= 1 && sel <= static_cast<int>(std::size(kMethods))) {
    m = kMethods[sel - 1];
  } else if (sel == 100) {
    wxTextEntryDialog dlg(this, "Method (e.g. PROPFIND, PURGE):", "Custom method", wxString::FromUTF8(method_str_));
    if (dlg.ShowModal() != wxID_OK) return;
    m = std::string(trim(dlg.GetValue().Upper().utf8_string()));
    if (m.empty() || m.find_first_of(" \t\r\n") != std::string::npos) return;
  } else {
    return;
  }
  if (m == method_str_) return;
  set_method(m);
  touched();
}

std::string HttpEditor::url_text() const {
  const wxCharBuffer b = url_->GetTextRaw();
  return std::string(b.data(), b.length());
}

void HttpEditor::set_url_text(const std::string& u) {
  const bool was = syncing_;
  syncing_ = true;
  url_->ClearAll();
  url_->AddTextRaw(u.data(), static_cast<int>(u.size()));
  url_->EmptyUndoBuffer();
  syncing_ = was;
  update_placeholder(u.empty());
  highlight_vars(url_, 0, url_->GetLength());
}

void HttpEditor::update_placeholder(bool empty) {
  const bool was = syncing_;
  syncing_ = true;
  url_->EOLAnnotationClearAll();
  if (empty) {
    url_->EOLAnnotationSetText(0, "Enter a URL or paste a curl command");
    url_->EOLAnnotationSetStyle(0, kStylePlaceholder);
  }
  syncing_ = was;
}

void HttpEditor::url_changed() {
  const std::string u = url_text();
  update_placeholder(u.empty());
  highlight_vars(url_, 0, url_->GetLength());
  sync_path_rows();
  update_preview();
  // Params follow the URL; disabled params and rows without a key (not in the URL) are kept at
  // the end.
  std::vector<KvRow> rows;
  for (const auto& q : parse_query_decoded(split_url(u).query)) rows.push_back({q.key, q.value, true, false, q.bare});
  const size_t from_url = rows.size();
  for (auto& old : params_->rows()) {
    if (old.enabled && !trim(old.key).empty()) continue;
    if (old.enabled && std::any_of(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(from_url),
                                   [&](const KvRow& n) { return n.key == old.key && n.value == old.value; }))
      continue;  // typed into the URL as "=value": shown once
    rows.push_back(old);
  }
  params_->set_rows(rows);
  if (path_panel_ && path_panel_->IsShown()) layout_params_page();
  if (auth_mode_ == AuthMode::ApiKey && !api_key_param_.empty()) {
    // the user may have edited the managed param in the URL: reflect it on the Auth tab
    for (const auto& r : rows)
      if (r.key == api_key_param_ && r.enabled) {
        const bool was = loading_;
        loading_ = true;
        av_.key_value = r.value;
        if (key_value_) key_value_->ChangeValue(wxString::FromUTF8(r.value));
        loading_ = was;
      }
  }
  touched();
}

std::string HttpEditor::with_path(const std::string& url) const {
  if (path_names_.empty()) return url;
  return with_path_values(url, [this](const std::string& n) {
    const auto it = path_values_.find(n);
    return it == path_values_.end() ? std::string() : it->second;
  });
}

std::vector<std::string> HttpEditor::empty_path_vars() const {
  std::vector<std::string> out;
  for (const auto& n : path_names_)
    if (const auto it = path_values_.find(n); it == path_values_.end() || it->second.empty()) out.push_back(n);
  return out;
}

void HttpEditor::sync_path_rows() {
  std::vector<std::string> names = path_var_names(url_text());
  if (names == path_names_ && (path_panel_ || names.empty())) return;
  path_names_ = std::move(names);
  if (!path_panel_) {
    if (path_names_.empty()) return;
    wxWindowUpdateLocker lock(params_page_);
    path_panel_ = new KvPanel(params_page_, KvGrid::Kind::Path, "Path variables");
    path_panel_->SetToolTip("Each /:name segment of the URL is replaced by its value, URL-encoded");
    path_ = path_panel_->grid();
    path_->lookup = [this](const std::string& n) { return resolve(n); };
    path_->var_names = [this] { return var_names(); };
    path_->on_change = [this] {
      if (loading_) return;
      for (const auto& row : path_->rows()) path_values_[row.key] = row.value;
      update_preview();
      touched();
    };
    params_page_->GetSizer()->Add(path_panel_, 1, wxEXPAND | wxTOP, FromDIP(8));
  }
  std::vector<KvRow> rows;
  for (const auto& n : path_names_) {
    const auto it = path_values_.find(n);
    rows.push_back({n, it == path_values_.end() ? std::string() : it->second, true, false, false});
  }
  const bool was = loading_;
  loading_ = true;
  path_->set_rows(rows);
  loading_ = was;
  path_panel_->Show(!path_names_.empty());
  layout_params_page();
}

void HttpEditor::layout_params_page() {
  if (!params_page_ || !query_panel_) return;
  wxSizer* s = params_page_->GetSizer();
  const bool path = path_panel_ && path_panel_->IsShown();
  if (wxSizerItem* it = s->GetItem(query_panel_)) it->SetProportion(path ? 0 : 1);
  // With path variables the query table is as tall as its rows (at most half the page) and the
  // path table sits right under it, as in Postman / Insomnia.
  query_panel_->fit_rows(path ? std::max(FromDIP(90), params_page_->GetClientSize().y / 2) : -1);
  params_page_->Layout();
}

std::vector<std::string> HttpEditor::var_names() const {
  std::vector<std::string> out;
  const Workspace* ws = host_->workspace();
  if (!ws) return out;
  std::set<std::string> names;
  // environments() has no const overload; it is only read here.
  for (const auto& env : const_cast<Workspace*>(ws)->environments())
    if (env.name == ws->active_env)
      for (const auto& [k, v] : env.vars) names.insert(k);
  for (const auto& [k, v] : ws->globals) names.insert(k);
  for (const auto& n : names)
    if (!n.empty() && std::none_of(n.begin(), n.end(), [](char c) { return static_cast<unsigned char>(c) <= 32; }))
      out.push_back(n);
  for (const char* d : {"$guid", "$timestamp", "$isoTimestamp", "$randomInt"}) out.emplace_back(d);
  return out;
}

std::string HttpEditor::sent_url() const {
  const std::string t = normalize_url(with_path(url_text()));
  const Workspace* ws = host_->workspace();
  if (!ws || t.find("{{") == std::string::npos) return t;
  return expand_url(t, [ws](const std::string& span) { return ws->expand(span); });
}

std::string HttpEditor::sent_form(const std::string& body) const {
  const Workspace* ws = host_->workspace();
  if (!ws) return body;
  return expand_form(body, [ws](const std::string& span) { return ws->expand(span); });
}

std::vector<std::string> HttpEditor::unresolved_vars(bool* in_url) const {
  std::vector<std::string> out;
  auto scan = [&](const std::string& s) {
    bool any = false;
    for (const auto& v : find_vars(s))
      if (!resolve(v.name) && std::find(out.begin(), out.end(), v.name) == out.end()) {
        out.push_back(v.name);
        any = true;
      }
    return any;
  };
  {
    std::string u = url_text();
    for (const auto& n : path_names_)
      if (const auto it = path_values_.find(n); it != path_values_.end()) u += "\n" + it->second;
    if (in_url) *in_url = scan(u);
    else scan(u);
  }
  for (const auto& row : header_rows())
    if (row.enabled) scan(row.key + "\n" + row.value);
  switch (auth_mode_) {
    case AuthMode::Bearer: scan(av_.bearer); break;
    case AuthMode::Basic: scan(av_.user + "\n" + av_.pass); break;
    case AuthMode::ApiKey: scan(av_.key_name + "\n" + av_.key_value); break;
    default: break;
  }
  if (body_mode_ == BodyMode::UrlEncoded)
    for (const auto& row : form_rows(0))
      if (row.enabled) scan(row.key + "\n" + row.value);
  if (body_mode_ == BodyMode::Multipart)
    for (const auto& row : form_rows(1))
      if (row.enabled) scan(row.key + "\n" + row.value);
  return out;
}

void HttpEditor::update_preview() {
  const std::string u = sent_url();
  const wxString w = u.empty() ? wxString() : wxString::FromUTF8("Sent as  " + u);
  if (w == preview_->GetLabelText()) return;
  preview_->SetLabelText(w);  // no '&' mnemonics
  preview_->SetToolTip(wxString::FromUTF8(u));
  if (preview_->IsShown() != !w.empty()) {  // no blank reserved line under the title
    preview_->Show(!w.empty());
    preview_->GetParent()->Layout();
  }
}

void HttpEditor::params_changed() {
  if (loading_) return;
  const std::string u = url_text();
  // The table shows decoded text: what changed is percent-encoded into the URL, what did not
  // keeps its spelling (and a param written without '=' keeps that form).
  std::vector<KvItem> items;
  // A row with a value but no key stays out of the URL (it is flagged on the row): "&=f" would
  // be sent otherwise.
  for (const auto& r : params_->rows())
    items.push_back({r.key, r.value, r.enabled && !trim(r.key).empty(), r.value.empty()});
  const std::string nu = with_decoded_query(u, items);
  if (nu != u) {
    set_url_text(nu);
    url_->GotoPos(url_->GetLength());
    update_preview();
  }
  if (path_panel_ && path_panel_->IsShown()) layout_params_page();
  touched();
}

void HttpEditor::url_inserted(const wxString& ins) {
  const std::string text = ins.utf8_string();
  if (looks_like_curl(text)) {
    import_curl(std::string(trim(text)));
    return;
  }
  // Pasted text with line breaks (a URL wrapped by a chat client, ...): one line.
  std::string u = url_text();
  std::string out;
  out.reserve(u.size());
  for (char c : u)
    if (c != '\r' && c != '\n') out += c;
  if (out != u) {
    const int pos = url_->GetCurrentPos();
    set_url_text(out);
    url_->GotoPos(std::min(pos, url_->GetLength()));
    url_changed();
  }
}

void HttpEditor::import_curl(const std::string& cmd, CmdBackslashes bs) {
  std::string err;
  auto imp = parse_curl(cmd, &err, bs);
  if (!imp) {
    // Not importable: keep the text as a one-line URL and say why.
    std::string u = url_text();
    u.erase(std::remove_if(u.begin(), u.end(), [](char c) { return c == '\r' || c == '\n'; }), u.end());
    set_url_text(u);
    url_changed();
    show_info("Could not import the curl command", wxString::FromUTF8(err));
    return;
  }
  SavedRequest r = base_;
  r.http = imp->http;
  r.disabled_headers.clear();
  r.disabled_params.clear();
  r.form = imp->form;
  r.basic_auth = imp->basic_auth;
  r.body_file = imp->body_file;
  r.body_data = imp->body_data;
  r.header_files = imp->header_files;
  r.cookie_files = imp->cookie_files;
  first_load_ = true;  // pick the tab that shows what was imported
  load(r);
  host_->changed();
  std::string msg = r.http.method + " " + r.http.url;
  for (const auto& w : imp->warnings) msg += "\n\xE2\x80\xA2 " + w;
  if (imp->browser_doubled_backslashes > 0 && bs == CmdBackslashes::AsCurlExe) {
    show_info("Imported curl command", wxString::FromUTF8(msg), "Read backslashes as the browser meant",
              [this, cmd] { import_curl(cmd, CmdBackslashes::AsBrowserMeant); });
  } else if (!imp->warnings.empty()) {
    show_info("Imported curl command", wxString::FromUTF8(msg));
  } else {
    note(wxString::FromUTF8("Imported curl command"));
  }
}

void HttpEditor::set_body_mode(BodyMode m, bool user) {
  body_mode_ = m;
  body_modes_->select(static_cast<int>(m) < kBodyModeCount ? static_cast<int>(m) : -1);
  sync_body_book();
  apply_lexer(m);
  if (m == BodyMode::Text) {
    body_->StartStyling(0);
    body_->SetStyling(body_->GetLength(), 0);
  }
  body_->Colourise(0, std::min(body_->GetLength(), 64 * 1024));
  beautify_->Show(m == BodyMode::Json);
  // Only once a one-line body is shown formatted (or the request was saved that way).
  minify_->Show(m == BodyMode::Json && (minify_->value() || raw_body_));
  lint_timer_.StartOnce(50);
  beautify_->GetParent()->Layout();
  if (!user) return;
  switch (m) {
    case BodyMode::None: set_content_type(nullptr); break;
    case BodyMode::Json: set_content_type("application/json"); break;
    case BodyMode::Text: set_content_type("text/plain"); break;
    case BodyMode::Xml: set_content_type("application/xml"); break;
    case BodyMode::UrlEncoded: set_content_type("application/x-www-form-urlencoded"); break;
    case BodyMode::Multipart: set_content_type(nullptr); break;  // built with its boundary at send time
    case BodyMode::Binary: set_content_type("application/octet-stream"); break;
    case BodyMode::CurlData: break;
  }
  if (text_mode(m)) body_->SetFocus();
  touched();
}

// Sets (or with null removes) the Content-Type row, unless the user typed a content type of
// their own.
void HttpEditor::set_content_type(const char* ct) {
  auto rows = header_rows();
  auto it = std::find_if(rows.begin(), rows.end(), [](const KvRow& r) { return iequals(r.key, "Content-Type"); });
  if (it != rows.end()) {
    if (!is_auto_content_type(it->value)) return;
    if (ct) {
      it->value = ct;
      it->enabled = true;
    } else {
      rows.erase(it);
    }
  } else if (ct) {
    rows.push_back({"Content-Type", ct, true, false, false});
  } else {
    return;
  }
  set_header_rows(rows);
}

void HttpEditor::set_auth_mode(AuthMode m, bool user) {
  const AuthMode old = auth_mode_;
  auth_mode_ = m;
  if (auth_book_) {
    auth_btn_->SetLabel(kAuthLabels[static_cast<int>(m)]);
    auth_book_->ChangeSelection(static_cast<size_t>(m));
  }
  if (!user) return;
  if (old == AuthMode::ApiKey && m != AuthMode::ApiKey && !api_key_param_.empty()) {
    auto rows = params_->rows();
    rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const KvRow& r) { return r.key == api_key_param_; }), rows.end());
    api_key_param_.clear();
    params_->set_rows(rows);
    params_changed();
  }
  if (m == AuthMode::ApiKey && av_.key_name.empty()) {
    av_.key_name = "X-API-Key";
    push_auth();
  }
  if (m == AuthMode::ApiKey) sync_api_key_query();
  wxWindow* focus = m == AuthMode::Bearer ? bearer_ : m == AuthMode::Basic ? basic_user_ : m == AuthMode::ApiKey ? key_value_ : nullptr;
  if (focus) CallAfter([focus] { focus->SetFocus(); });  // after the dropdown has closed
  touched();
}

// API key "in query": the key is a real query param (visible in the URL and the Params tab).
void HttpEditor::sync_api_key_query() {
  if (auth_mode_ != AuthMode::ApiKey || av_.key_where != 1) return;
  const std::string name = av_.key_name;
  const std::string value = av_.key_value;
  auto rows = params_->rows();
  auto it = std::find_if(rows.begin(), rows.end(), [&](const KvRow& r) {
    return !api_key_param_.empty() ? r.key == api_key_param_ : r.key == name;
  });
  if (name.empty()) {
    if (it != rows.end()) rows.erase(it);
  } else if (it != rows.end()) {
    it->key = name;
    it->value = value;
    it->enabled = true;
  } else {
    rows.push_back({name, value, true, false, false});
  }
  api_key_param_ = name;
  params_->set_rows(rows);
  params_changed();
}

void HttpEditor::beautify() {
  if (body_mode_ != BodyMode::Json || pending_body_) return;
  auto text = std::make_shared<const std::string>(body_->GetCharacterPointer(), static_cast<size_t>(body_->GetLength()));
  const uint64_t gen = ++beautify_gen_;
  const uint64_t edits = body_edits_;
  if (text->size() >= kAsyncBody) {
    note(wxString::FromUTF8("Formatting\xE2\x80\xA6"));
    // A one-line (minified) body keeps being sent as it was: "Send minified" goes on.
    raw_pending_ = !minify_->value() && longest_line(*text) >= kLongLine ? text : nullptr;
    build_body_doc(text, true, gen, edits);
    return;
  }
  // Small body: re-indent here (well under a millisecond) and keep it undoable.
  const std::string out = reindent_json(*text);
  if (out == *text) {
    note("Already formatted");
    return;
  }
  body_->SetTargetStart(0);
  body_->SetTargetEnd(body_->GetLength());
  body_->ReplaceTargetRaw(out.data(), static_cast<int>(out.size()));
  body_->GotoPos(0);
  note("Formatted");
}

// Debounced JSON syntax check: small bodies here (~10 ms per MB), big ones on a worker.
void HttpEditor::lint() {
  const uint64_t gen = ++lint_gen_;
  if (body_mode_ != BodyMode::Json || pending_body_ || body_->GetLength() > 64 * 1024 * 1024) {
    show_lint(gen, std::nullopt);
    return;
  }
  const size_t len = static_cast<size_t>(body_->GetLength());
  if (len < 256 * 1024) {
    show_lint(gen, check_json(std::string_view(body_->GetCharacterPointer(), len)));
    return;
  }
  auto text = std::make_shared<const std::string>(body_->GetCharacterPointer(), len);
  std::thread([alive = alive_, text, gen] {
    auto issue = check_json(*text);
    std::lock_guard lk(alive->m);
    if (!alive->target) return;
    auto* ed = static_cast<HttpEditor*>(alive->target);
    ed->CallAfter([ed, gen, issue = std::move(issue)] { ed->show_lint(gen, issue); });
  }).detach();
}

void HttpEditor::show_lint(uint64_t gen, std::optional<JsonIssue> issue) {
  if (gen != lint_gen_) return;  // edited meanwhile: a newer check is on its way
  const int len = body_->GetLength();
  if (lint_issue_) {
    body_->SetIndicatorCurrent(kIndJsonError);
    body_->IndicatorClearRange(0, len);
  }
  lint_issue_ = std::move(issue);
  if (lint_issue_ && static_cast<int>(lint_issue_->offset) <= len) {
    const int at = static_cast<int>(lint_issue_->offset);
    const int n = std::max(1, std::min(static_cast<int>(lint_issue_->length), len - at));
    body_->SetIndicatorCurrent(kIndJsonError);
    body_->IndicatorFillRange(at, n);
    const int line = body_->LineFromPosition(at);
    const int col = at - body_->PositionFromLine(line);
    lint_label_->SetLabel(wxString::Format("Invalid JSON, line %d col %d: %s", line + 1, col + 1,
                                           wxString::FromUTF8(lint_issue_->message)));
  } else {
    lint_issue_.reset();
  }
  if (lint_label_->IsShown() != lint_issue_.has_value()) {
    lint_label_->Show(lint_issue_.has_value());
    lint_label_->GetParent()->Layout();
  }
}

// JSON auto-close: "{" "[" and an opening quote get their closer after the caret, when the
// caret is not inside a string and the next character does not continue a token.
void HttpEditor::auto_close(int ch) {
  if (body_mode_ != BodyMode::Json || !body_->GetSelectionEmpty()) return;
  const int pos = body_->GetCurrentPos();  // after the typed character
  const int line = body_->LineFromPosition(pos);
  const int ls = body_->PositionFromLine(line);
  if (pos - ls > 4096) return;
  // Inside a string? Count unescaped quotes before the typed character.
  const wxCharBuffer before = body_->GetTextRangeRaw(ls, pos - 1);
  bool in_str = false;
  for (size_t i = 0; i < before.length(); ++i) {
    if (before[i] == 0x5C && in_str) ++i;  // backslash escape
    else if (before[i] == '"') in_str = !in_str;
  }
  if (in_str) return;  // a quote here closes the string; a brace here is text
  const int next = body_->GetCharAt(pos);
  const bool ok_next = next == 0 || next == ' ' || next == 9 || next == 13 || next == 10 || next == ',' ||
                       next == '}' || next == ']' || next == ':';
  if (!ok_next) return;
  const char closer[2] = {ch == '{' ? '}' : ch == '[' ? ']' : '"', 0};
  body_->InsertTextRaw(pos, closer);
  body_->SetIndicatorCurrent(kIndAutoClose);
  body_->IndicatorFillRange(pos, 1);
}

// Pasting megabytes of one-line JSON: Scintilla lays out a whole line on every change, so a
// 10 MB line costs ~0.6 s per keystroke. Such a paste is shown re-indented (whitespace only,
// values kept as written) and never exists as one line, while "Send minified" sends the bytes as
// pasted. The UI thread only copies the clipboard's UTF-16 block (one memcpy); the UTF-8
// conversion, the check, the re-indent and the document build run on workers, and the old
// document is freed on a worker too.
bool HttpEditor::paste_long_json() {
  if (body_mode_ != BodyMode::Json || body_->GetReadOnly()) return false;
  std::shared_ptr<std::wstring> w;
  if (!::OpenClipboard(static_cast<HWND>(GetHWND()))) return false;
  if (HANDLE h = ::GetClipboardData(CF_UNICODETEXT)) {
    if (const auto* p = static_cast<const wchar_t*>(::GlobalLock(h))) {
      const size_t cap = ::GlobalSize(h) / sizeof(wchar_t);
      size_t n = 0;
      while (n < cap && p[n]) ++n;  // wcsnlen without the CRT's bounds noise
      size_t first = 0;
      while (first < n && (p[first] == ' ' || p[first] == '\t' || p[first] == '\r' || p[first] == '\n')) ++first;
      // Under ~64 KB, or not JSON: Scintilla's own paste is fast enough.
      if (n >= kLongLine / 3 && first < n && (p[first] == '{' || p[first] == '[')) w = std::make_shared<std::wstring>(p, n);
      ::GlobalUnlock(h);
    }
  }
  ::CloseClipboard();
  if (!w) return false;
  const int len = body_->GetLength();
  const int start = body_->GetSelectionStart(), end = body_->GetSelectionEnd();
  const bool whole = len == 0 || (start == 0 && end == len);
  const uint64_t gen = ++beautify_gen_, edits = body_edits_;
  note(wxString::FromUTF8("Pasting\xE2\x80\xA6"));
  std::thread([alive = alive_, w, gen, edits, start, end, whole] {
    auto text = std::make_shared<std::string>();
    if (!w->empty()) {
      const int n = ::WideCharToMultiByte(CP_UTF8, 0, w->data(), static_cast<int>(w->size()), nullptr, 0, nullptr, nullptr);
      text->resize(static_cast<size_t>(std::max(n, 0)));
      ::WideCharToMultiByte(CP_UTF8, 0, w->data(), static_cast<int>(w->size()), text->data(), n, nullptr, nullptr);
    }
    const bool long_json = looks_like_json(*text) && longest_line(*text) >= kLongLine;
    if (!long_json) {  // inserted as text: CRLF -> LF, as Scintilla's paste does
      size_t o = 0;
      for (size_t i = 0; i < text->size(); ++i)
        if (!((*text)[i] == 13 && i + 1 < text->size() && (*text)[i + 1] == 10)) (*text)[o++] = (*text)[i];
      text->resize(o);
    }
    // Into part of a body: re-indented here, so the UI thread only inserts it.
    if (long_json && !whole) *text = reindent_json(*text);
    std::shared_ptr<const std::string> ctext = std::move(text);
    std::lock_guard lk(alive->m);
    if (!alive->target) return;
    auto* ed = static_cast<HttpEditor*>(alive->target);
    ed->CallAfter([ed, gen, edits, start, end, whole, ctext, long_json] {
      ed->paste_arrived(gen, edits, start, end, whole, ctext, long_json);
    });
  }).detach();
  return true;
}

void HttpEditor::paste_arrived(uint64_t gen, uint64_t edits, int start, int end, bool whole,
                               std::shared_ptr<const std::string> text, bool long_json) {
  if (gen != beautify_gen_ || edits != body_edits_ || pending_body_ || body_->GetReadOnly()) {
    note("Paste dropped: the body changed while it was read", true);
    return;
  }
  if (long_json && whole) {
    // Formatted and loaded on a worker, like Beautify; sent as pasted.
    note(wxString::FromUTF8("Formatting the pasted JSON\xE2\x80\xA6"));
    raw_pending_ = text;
    build_body_doc(text, true, gen, edits);
    return;
  }
  const auto t0 = Clock::now();
  const std::string& ins = *text;  // a long JSON line arrives re-indented by the worker
  body_->SetTargetRange(start, end);
  body_->ReplaceTargetRaw(ins.data(), static_cast<int>(ins.size()));
  // Caret at the start of the paste: at its end, Scintilla would lex all of it at once.
  body_->GotoPos(long_json ? start : start + static_cast<int>(ins.size()));
  if (long_json)
    note(wxString::Format("Pasted one-line JSON as %s, formatted on a worker so it stays fast to edit (whitespace "
                          "only; UI %.0f ms)",
                          human_size(ins.size()), ms_between(t0, Clock::now())));
  else
    note("");
}

// Builds a Scintilla document from `text` (re-indented first when `reindent`) on a worker thread
// and swaps it in when done, if nothing changed meanwhile (see swap_body_doc).
void HttpEditor::build_body_doc(std::shared_ptr<const std::string> text, bool reindent, uint64_t gen, uint64_t edits) {
  const size_t hint = std::min<size_t>(text->size() + (reindent ? text->size() : 0), 0x7FFFFFF0);
  auto* loader = static_cast<SciLoader*>(body_->CreateLoader(static_cast<int>(hint)));
  if (!loader) {  // cannot happen with Scintilla 5; keep the body usable anyway
    if (!reindent) {
      pending_body_.reset();
      body_->SetReadOnly(false);
      body_->AddTextRaw(text->data(), static_cast<int>(text->size()));
    }
    return;
  }
  std::thread([alive = alive_, text, reindent, gen, edits, loader] {
    const auto t0 = Clock::now();
    std::string out;
    if (reindent) out = reindent_json(*text);
    const std::string& src = reindent ? out : *text;
    const bool same = reindent && out == *text;
    if (!same) {
      for (size_t off = 0; off < src.size(); off += size_t{1} << 20)
        loader->AddData(src.data() + off, static_cast<ptrdiff_t>(std::min(src.size() - off, size_t{1} << 20)));
    }
    const double worker_ms = ms_between(t0, Clock::now());
    const size_t bytes = src.size();
    std::lock_guard lk(alive->m);
    if (!alive->target || same) {
      loader->Release();
      if (same && alive->target) {
        auto* ed = static_cast<HttpEditor*>(alive->target);
        ed->CallAfter([ed] { ed->note("Already formatted"); });
      }
      return;
    }
    void* doc = loader->ConvertToDocument();
    alive->docs.push_back(doc);
    auto* ed = static_cast<HttpEditor*>(alive->target);
    ed->CallAfter([ed, doc, reindent, gen, edits, worker_ms, bytes] {
      ed->swap_body_doc(doc, reindent, gen, edits, worker_ms, bytes);
    });
  }).detach();
}

void HttpEditor::swap_body_doc(void* doc, bool reindent, uint64_t gen, uint64_t edits, double worker_ms, size_t bytes) {
  {
    std::lock_guard lk(alive_->m);
    auto& d = alive_->docs;
    d.erase(std::remove(d.begin(), d.end(), doc), d.end());
  }
  const bool current = reindent ? (gen == beautify_gen_ && edits == body_edits_ && !pending_body_)
                                : (gen == body_load_gen_ && pending_body_);
  if (!current) {  // edited or reloaded meanwhile
    body_->ReleaseDocument(doc);
    return;
  }
  const auto t0 = Clock::now();
  // The old document: kept for Ctrl+Z after a paste / format, else freed on a worker (freeing
  // 25 MB and a 1.8M-line index here would stall the UI).
  void* old = body_->GetDocPointer();
  body_->AddRefDocument(old);
  if (prev_doc_) release_doc_async(prev_doc_);
  prev_doc_ = nullptr;
  if (reindent) {
    prev_doc_ = old;
    prev_minify_ = minify_->value();
  } else {
    release_doc_async(old);
  }
  install_doc(doc);
  body_->ReleaseDocument(doc);  // the control holds its own reference now
  pending_body_.reset();
  if (reindent) {
    ++body_edits_;
    if (raw_pending_) {
      raw_body_ = std::move(raw_pending_);
      raw_edits_ = body_edits_;
      minify_->set_value(true);
      if (!minify_->IsShown()) {
        minify_->Show();
        minify_->GetParent()->Layout();
      }
      note(wxString::Format("Pasted %s of one-line JSON: shown formatted (%s, %.0f ms on a worker, UI %.1f ms), "
                            "sent as pasted. Ctrl+Z undoes.",
                            human_size(raw_body_->size()), human_size(bytes), worker_ms, ms_between(t0, Clock::now())));
    } else {
      note(wxString::Format("Formatted %s in %.0f ms (UI %.1f ms)", human_size(bytes), worker_ms,
                            ms_between(t0, Clock::now())));
    }
    touched();
  }
}

void HttpEditor::install_doc(void* doc) {
  const bool was = loading_;
  loading_ = true;
  body_->SetReadOnly(false);
  body_->SetDocPointer(doc);
  // Document-level settings travel with the document: set them on the new one.
  body_->SetCodePage(wxSTC_CP_UTF8);
  body_->SetEOLMode(wxSTC_EOL_LF);
  body_->SetTabWidth(2);
  body_->SetUseTabs(false);
  body_->SetIndent(2);
  apply_lexer(body_mode_);
  loading_ = was;
  body_->GotoPos(0);
  body_hl_first_ = -1;
  update_line_margin();
  highlight_body_visible();
  lint_timer_.StartOnce(50);
}

// Drops one reference to a Scintilla document on a worker thread. A detached document is touched
// by nothing else, and the editor's destructor waits on alive_->m, so the control outlives it.
void HttpEditor::release_doc_async(void* doc) {
  if (!doc) return;
  std::thread([alive = alive_, doc] {
    std::lock_guard lk(alive->m);
    if (auto* ed = static_cast<HttpEditor*>(alive->target)) ed->body_->ReleaseDocument(doc);
  }).detach();
}

void HttpEditor::show_curl_menu() {
  wxMenu menu;
  menu.Append(1, "Copy as curl (bash / zsh)");
  menu.Append(2, "Copy as curl (cmd.exe)");
  menu.Append(3, "Copy as curl (PowerShell)");
  menu.AppendSeparator();
  for (int i = 0; i < kCodeLangCount; ++i)
    menu.Append(21 + i, wxString("Copy as ") + code_lang_name(static_cast<CodeLang>(i)));
  menu.AppendSeparator();
  menu.AppendCheckItem(10, "Resolve {{variables}}")->Check(resolve_vars_in_curl_);
  const wxPoint at = curl_btn_->GetPosition() + wxPoint(0, curl_btn_->GetSize().y);
  const int sel = GetPopupMenuSelectionFromUser(menu, at);
  if (sel == 10) {
    resolve_vars_in_curl_ = !resolve_vars_in_curl_;
    note(resolve_vars_in_curl_ ? "Copy as resolves {{variables}}" : "Copy as keeps {{variables}}");
  } else if (sel >= 1 && sel <= 3) {
    copy_as_curl(sel == 1 ? CurlShell::Bash : sel == 2 ? CurlShell::Cmd : CurlShell::PowerShell);
  } else if (sel >= 21 && sel < 21 + kCodeLangCount) {
    const auto lang = static_cast<CodeLang>(sel - 21);
    SavedRequest r = base_;
    store(r);
    r.http.url = with_path(r.http.url);
    const Workspace* ws = host_->workspace();
    if (resolve_vars_in_curl_ && ws) r = ws->expanded(r);
    const std::string code = to_code(r, lang);
    if (wxTheClipboard->Open()) {
      wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(code)));
      wxTheClipboard->Close();
      note(wxString::Format("Copied %s, %zu chars", code_lang_name(lang), code.size()));
    } else {
      note("Could not open the clipboard", true);
    }
  }
}

void HttpEditor::copy_as_curl(CurlShell shell) {
  SavedRequest r = base_;
  store(r);
  r.http.url = with_path(r.http.url);
  const Workspace* ws = host_->workspace();
  if (resolve_vars_in_curl_ && ws) r = ws->expanded(r);
  const std::string cmd = to_curl_command(r, shell);
  const std::string problem = curl_export_problem(r.http, r.form, shell);
  if (wxTheClipboard->Open()) {
    wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(cmd)));
    wxTheClipboard->Close();
    const char* sh = shell == CurlShell::Bash ? "bash" : shell == CurlShell::Cmd ? "cmd" : "PowerShell";
    if (!problem.empty()) note(wxString::FromUTF8("Copied (" + std::string(sh) + "), but: " + problem), true);
    else note(wxString::Format("Copied curl (%s), %zu chars", sh, cmd.size()));
  } else {
    note("Could not open the clipboard", true);
  }
}

void HttpEditor::note(const wxString& text, bool warn) {
  const Theme& t = Theme::get();
  note_->SetForegroundColour(warn ? t.var_bad : t.dim);
  wxString shown = text;
  if (shown.length() > 400) shown = shown.Left(397) + "...";
  note_->SetLabel(shown);
  note_->SetToolTip(text);
  if (note_->IsShown() != !text.empty()) {
    note_->Show(!text.empty());
    Layout();
  }
  note_timer_.Stop();
  if (!text.empty()) note_timer_.StartOnce(warn ? 12000 : 3500);
}

// ------------------------------------------------------------------ {{vars}}

std::optional<std::string> HttpEditor::resolve(const std::string& name) const {
  const Workspace* ws = host_->workspace();
  if (!ws) return std::nullopt;
  const std::string ph = "{{" + name + "}}";
  std::string v = ws->expand(ph);
  if (v == ph) return std::nullopt;
  return v;
}

// '}' / ']' / '"' typed right before the same character that this editor inserted: step over it.
// (no modifier check: '"' is Shift+2 and '}' AltGr+0 on a German keyboard)
bool HttpEditor::step_over_closer(wxStyledTextCtrl* s, int key) {
  if (key == WXK_NONE || key > 127 || !s->GetSelectionEmpty()) return false;
  const int pos = s->GetCurrentPos();
  if (!steps_over(key, s->GetCharAt(pos), s->IndicatorValueAt(kIndAutoClose, pos) != 0)) return false;
  if (s->AutoCompActive()) s->AutoCompCancel();
  s->SetIndicatorCurrent(kIndAutoClose);
  s->IndicatorClearRange(pos, 1);
  s->GotoPos(pos + 1);
  return true;
}

// A variable picked from the list: the caret goes past the "}}" that {{ added, ready for the rest.
void HttpEditor::step_past_var_closer(wxStyledTextCtrl* s) {
  const int pos = s->GetCurrentPos();
  if (s->GetCharAt(pos) != '}' || s->GetCharAt(pos + 1) != '}') return;
  if (s->IndicatorValueAt(kIndAutoClose, pos) == 0 || s->IndicatorValueAt(kIndAutoClose, pos + 1) == 0) return;
  s->SetIndicatorCurrent(kIndAutoClose);
  s->IndicatorClearRange(pos, 2);
  s->GotoPos(pos + 2);
}

void HttpEditor::var_completion(wxStyledTextCtrl* s) {
  const int pos = s->GetCurrentPos();
  if (pos < 2 || s->GetCharAt(pos - 1) != '{' || s->GetCharAt(pos - 2) != '{') return;
  if (pos >= 3 && s->GetCharAt(pos - 3) == '{') return;  // "{{{": a literal brace
  {
    const char before[3] = {static_cast<char>(pos >= 3 ? s->GetCharAt(pos - 3) : ' '), '{', '{'};
    const char after[2] = {static_cast<char>(s->GetCharAt(pos)), static_cast<char>(s->GetCharAt(pos + 1))};
    const std::string close = var_closer(std::string_view(before, 3), std::string_view(after, 2));
    if (!close.empty()) {
      s->InsertText(pos, close);
      s->SetIndicatorCurrent(kIndAutoClose);
      s->IndicatorFillRange(pos, 2);  // with the body's own auto-closed '}' when it added one
    }
  }
  if (!host_->workspace()) return;
  std::string list;
  for (const auto& n : var_names()) list += (list.empty() ? "" : " ") + n;
  s->AutoCompSetIgnoreCase(true);
  // Scintilla finds the typed prefix by binary search: the "$..." entries come after the names,
  // so the list is unsorted and without this {{ba matched nothing and the list closed.
  s->AutoCompSetOrder(wxSTC_ORDER_PERFORMSORT);
  s->AutoCompSetAutoHide(true);
  s->AutoCompSetDropRestOfWord(false);
  s->AutoCompSetMaxHeight(10);
  s->AutoCompSetSeparator(' ');
  s->AutoCompShow(0, wxString::FromUTF8(list));
}

void HttpEditor::highlight_vars(wxStyledTextCtrl* s, int start, int end) {
  if (end <= start) return;
  for (int ind : {kIndOkBox, kIndOkText, kIndBadBox, kIndBadText}) {
    s->SetIndicatorCurrent(ind);
    s->IndicatorClearRange(start, end - start);
  }
  const std::string_view text(s->GetRangePointer(start, end - start), static_cast<size_t>(end - start));
  if (text.find("{{") == std::string_view::npos) return;
  std::map<std::string, bool> cache;
  for (const auto& v : find_vars(text)) {
    auto it = cache.find(v.name);
    if (it == cache.end()) it = cache.emplace(v.name, resolve(v.name).has_value()).first;
    const bool ok = it->second;
    const int from = start + static_cast<int>(v.begin), len = static_cast<int>(v.end - v.begin);
    s->SetIndicatorCurrent(ok ? kIndOkBox : kIndBadBox);
    s->IndicatorFillRange(from, len);
    s->SetIndicatorCurrent(ok ? kIndOkText : kIndBadText);
    s->IndicatorFillRange(from, len);
  }
}

// Only what is on screen (a 10 MB body costs the same as a 1 KB one). Long lines are cut to
// their visible columns.
void HttpEditor::highlight_body_visible() {
  if (!text_mode(body_mode_)) return;
  const int first = body_->DocLineFromVisible(body_->GetFirstVisibleLine());
  const int last = std::min(body_->GetLineCount() - 1, first + body_->LinesOnScreen() + 1);
  const int x_off = body_->GetXOffset();
  if (first == body_hl_first_ && last == body_hl_last_ && x_off == body_hl_x_) return;
  body_hl_first_ = first;
  body_hl_last_ = last;
  body_hl_x_ = x_off;
  // A long line's visible columns are estimated from the x offset (the font is monospaced):
  // PointFromPosition / PositionFromPoint would lay out the whole line, which costs ~0.5 s for
  // a 10 MB line.
  const int cw = std::max(1, body_->TextWidth(wxSTC_STYLE_DEFAULT, "0"));
  const int cols = body_->GetClientSize().x / cw + 1;
  for (int line = first; line <= last; ++line) {
    int s = body_->PositionFromLine(line);
    int e = body_->GetLineEndPosition(line);
    if (e - s > 4096) {
      const int vs = s + x_off / cw;
      s = std::max(s, vs - 1024);
      e = std::min(e, vs + cols + 1024);
    }
    highlight_vars(body_, s, e);
  }
}

void HttpEditor::var_tooltip(wxStyledTextCtrl* s, int pos) {
  if (pos < 0) return;
  const int from = std::max(0, pos - 256), to = std::min(s->GetLength(), pos + 256);
  const wxCharBuffer b = s->GetTextRangeRaw(from, to);
  const std::string_view text(b.data(), b.length());  // a copy: CallTipShow below may move the gap
  for (const auto& v : find_vars(text)) {
    const int vb = from + static_cast<int>(v.begin), ve = from + static_cast<int>(v.end);
    if (pos < vb || pos >= ve) continue;
    const auto val = resolve(v.name);
    std::string tip;
    if (val) {
      std::string shown = *val;
      if (shown.size() > 300) shown = shown.substr(0, 300) + "...";
      tip = "{{" + v.name + "}} = " + shown;
    } else if (!host_->workspace()) {
      tip = "{{" + v.name + "}}: the workspace is still opening";
    } else {
      tip = "{{" + v.name + "}} is not defined in the active environment or globals (sent as written)";
    }
    s->CallTipShow(vb, wxString::FromUTF8(tip));
    return;
  }
}

// ------------------------------------------------------------------ send + upload prompt

void HttpEditor::do_send() {
  if (in_flight_) {
    host_->cancel();
    return;
  }
  url_->CallTipCancel();
  body_->CallTipCancel();
  // The URL bar keeps what the user typed: store() and the "Sent as" line encode it.
  const Workspace* ws = host_->workspace();
  if (ws) {
    SavedRequest r = base_;
    const bool files = body_mode_ == BodyMode::Multipart || body_mode_ == BodyMode::Binary ||
                       body_mode_ == BodyMode::CurlData || !base_.header_files.empty() || !base_.cookie_files.empty();
    // Environment variables: only when the request mentions one (expanding a 10 MB body is not
    // free). The JSON body is checked by the shell's http_to_send() as a last resort.
    std::string head = url_text();
    for (const auto& row : header_rows()) head += row.key + row.value;
    head += av_.bearer + av_.user + av_.pass + av_.key_value;
    bool env = head.find("env.") != std::string::npos;
    // A body over 4 MB is not scanned here (it would cost milliseconds per send): if it reads an
    // environment variable that is not allowed, http_to_send() refuses it and names the variable.
    if (!env && text_mode(body_mode_) && !pending_body_ &&
        body_->GetLength() < 4 * 1024 * 1024) {
      const std::string_view all(body_->GetCharacterPointer(), static_cast<size_t>(body_->GetLength()));
      env = all.find("env.") != std::string_view::npos;
    }
    if (files || env) {
      store(r);
      std::vector<std::filesystem::path> blocked;
      std::vector<std::string> blocked_env;
      if (files) blocked = ws->blocked_files(r);
      if (env) blocked_env = ws->blocked_env_vars(r);
      if (!blocked.empty() || !blocked_env.empty()) {
        show_upload_prompt(blocked, blocked_env);
        return;
      }
    }
  }
  // A path variable without a value would go out as ":name": say so, and go to its row.
  if (const auto empty = empty_path_vars(); !empty.empty()) {
    std::string title = "Not sent: path variable :" + empty.front() + " has no value";
    std::string text;
    if (empty.size() > 1) {
      text = "Also empty:";
      for (size_t i = 1; i < empty.size(); ++i) text += " :" + empty[i];
      text += ". ";
    }
    text += "Fill it in under Path variables on the Params tab, or remove it from the URL.";
    const Theme& t = Theme::get();
    show_info(wxString::FromUTF8(title), wxString::FromUTF8(text), "Fill in", [this, name = empty.front()] {
      hide_banner();
      tabs_->select(0);
      book_->ChangeSelection(0);
      if (!path_) return;
      for (int r = 0; r < path_->GetNumberRows(); ++r)
        if (path_->GetCellValue(r, 1).utf8_string() == name) {
          path_->SetFocus();
          path_->SetGridCursor(r, 2);
          path_->MakeCellVisible(r, 2);
          path_->EnableCellEditControl();
        }
    });
    banner_->set_colours(t.warn_bg, t.warn_border);
    banner_->Refresh();
    banner_missing_.clear();
    for (const auto& n : empty) banner_missing_.push_back(":" + n);
    return;
  }
  // An undefined {{name}} would go out literally (curl: "Malformed input to a URL function"):
  // say which one, and where it was looked up, before anything is sent.
  if (ws && !force_send_) {
    bool in_url = false;
    if (const auto missing = unresolved_vars(&in_url); !missing.empty()) {
      const std::string where = ws->active_env.empty() ? std::string("the globals (no environment is active)")
                                                       : "environment '" + ws->active_env + "' or the globals";
      std::string title = "Not sent: {{" + missing.front() + "}} is not defined in " + where;
      std::string text;
      if (missing.size() > 1) {
        text = "Also undefined:";
        for (size_t i = 1; i < missing.size(); ++i) text += " {{" + missing[i] + "}}";
        text += ". ";
      }
      text += in_url ? "Define it in the environment, or remove it from the URL."
                     : "Define it in the environment, or send it as written.";
      const Theme& t = Theme::get();
      show_info(wxString::FromUTF8(title), wxString::FromUTF8(text), in_url ? wxString() : wxString("Send as written"),
                [this] {
                  force_send_ = true;
                  do_send();
                  force_send_ = false;
                });
      banner_->set_colours(t.warn_bg, t.warn_border);
      banner_->Refresh();
      banner_missing_ = missing;
      return;
    }
  }
  hide_banner();
  if (body_mode_ == BodyMode::Json && lint_issue_)
    note(wxString::FromUTF8("Sent, but the JSON body is invalid: " + lint_issue_->message), true);
  sending_ = true;
  host_->send();
  sending_ = false;
}

void HttpEditor::show_upload_prompt(const std::vector<std::filesystem::path>& files,
                                    const std::vector<std::string>& env) {
  const Theme& t = Theme::get();
  ensure_banner();
  banner_missing_.clear();
  banner_->set_colours(t.warn_bg, t.warn_border);
  std::string title, text;
  if (files.size() == 1 && env.empty()) {
    title = "This request uploads " + path_utf8(files[0]) + " from outside the workspace";
  } else if (!files.empty()) {
    title = "This request uploads " + std::to_string(files.size()) + " files from outside the workspace";
  } else {
    title = env.size() == 1 ? "This request reads the environment variable " + env[0] + " from your system"
                            : "This request reads environment variables from your system";
  }
  if (files.size() > 1 || (!files.empty() && !env.empty())) {
    for (const auto& f : files) text += "\xE2\x80\xA2 " + path_utf8(f) + "\n";
  }
  if (!env.empty() && (env.size() > 1 || !files.empty())) {
    for (const auto& e : env) text += "\xE2\x80\xA2 environment variable " + e + "\n";
  }
  text += "Nothing has been sent. Allow it only if you expect this request to send that data. "
          "The choice is remembered for this workspace (in your user settings, never in the workspace).";
  set_banner_texts(wxString::FromUTF8(title), wxString::FromUTF8(text));
  banner_action_->SetLabel(files.empty() ? "Allow and send" : "Allow upload and send");
  banner_action_->Show();
  banner_cancel_->SetLabel("Cancel");
  banner_on_action_ = [this, files, env] {
    const Workspace* ws = host_->workspace();
    if (!ws) return;
    for (const auto& f : files) {
      std::string err;
      if (!ws->allow_file(f, &err)) {
        show_info("Could not allow the file", wxString::FromUTF8(err.empty() ? path_utf8(f) : err));
        return;
      }
    }
    for (const auto& e : env) {
      std::string err;
      if (!ws->allow_env_var(e, &err)) {
        show_info("Could not allow the environment variable", wxString::FromUTF8(err.empty() ? e : err));
        return;
      }
    }
    hide_banner();
    do_send();  // checks again: anything still blocked prompts again
  };
  banner_->Show();
  Layout();
  wrap_banner();
  banner_cancel_->SetFocus();  // the safe choice is the default
  banner_title_->SetToolTip(wxString::FromUTF8(title));
}

void HttpEditor::show_info(const wxString& title, const wxString& text, const wxString& action,
                           std::function<void()> on_action) {
  const Theme& t = Theme::get();
  ensure_banner();
  banner_missing_.clear();
  banner_->set_colours(t.info_bg, t.border);
  set_banner_texts(title, text);
  banner_action_->Show(!action.empty());
  banner_action_->SetLabel(action);
  banner_on_action_ = std::move(on_action);
  banner_cancel_->SetLabel("Dismiss");
  banner_->Show();
  Layout();
  wrap_banner();
}

// Wraps the banner texts to the space left of the buttons (the full path must stay readable).
void HttpEditor::wrap_banner() {
  if (!banner_) return;
  int buttons = banner_cancel_->GetSize().x + FromDIP(36);
  if (banner_action_->IsShown()) buttons += banner_action_->GetSize().x + FromDIP(6);
  const int w = banner_->GetClientSize().x - buttons;
  if (w < FromDIP(80) || w == banner_wrap_) return;
  banner_wrap_ = w;
  banner_title_->SetLabel(banner_title_raw_);
  banner_title_->Wrap(w);
  banner_text_->SetLabel(banner_text_raw_);
  banner_text_->Wrap(w);
  banner_->Layout();
  Layout();
}

void HttpEditor::set_banner_texts(const wxString& title, const wxString& text) {
  banner_title_raw_ = title;
  banner_text_raw_ = text;
  banner_text_->Show(!text.empty());
  banner_wrap_ = -1;
  banner_title_->SetLabel(title);
  banner_text_->SetLabel(text);
}

void HttpEditor::hide_banner() {
  banner_missing_.clear();
  if (!banner_ || !banner_->IsShown()) return;
  banner_->Hide();
  banner_on_action_ = nullptr;
  Layout();
}

}  // namespace

HttpEditorPanel* make_http_editor(wxWindow* parent, EditorHost* host) { return new HttpEditor(parent, host); }

}  // namespace watt::app
