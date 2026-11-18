// REST request editor: method + URL bar (paste a curl command anywhere in it to import it), query
// params synced both ways with the URL, headers with enable checkboxes and autocompletion, body
// (none / JSON / text / form-urlencoded / multipart / binary file) in a wxStyledTextCtrl with the
// JSON lexer and off-thread Beautify, auth (bearer / basic / API key / OAuth 2.0 with token status
// and Get new token / Digest / NTLM / Negotiate / AWS SigV4), a Scripts page (pre-request
// variables computed on a worker right before each send, see http_editor_pre.h; assertions,
// captures; pre-request and post-response JavaScript saved as SavedRequest::pre_script /
// post_script), transport options,
// {{var}} highlighting with the resolved value on hover, Copy as curl, and an in-app prompt
// before a request uploads a file from outside the workspace.
//
// Performance: nothing here scales with the body size per keystroke. The body lives only in the
// Scintilla buffer (lexed lazily, only what is on screen); store_head() never touches it; {{var}}
// highlighting looks at the visible part of the body only; Beautify runs on a worker thread.
#include <wx/scrolwin.h>
#include <wx/accel.h>
#include <wx/aui/auibook.h>
#include <wx/display.h>
#include <wx/popupwin.h>
#include <wx/eventfilter.h>
#include <wx/toplevel.h>
#include <wx/app.h>
#include <wx/utils.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/clipbrd.h>
#include <wx/dcbuffer.h>
#include <wx/filedlg.h>
#include <wx/html/htmlwin.h>
#include <wx/image.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/radiobut.h>
#include <wx/richtooltip.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/stc/stc.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>
#include <wx/timer.h>
#include <wx/weakref.h>
#include <wx/wupdlock.h>
#include <wx/msw/wrapwin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#include "app/autocomp_guard.h"
#include "app/http_editor_codegen.h"
#include "app/http_editor_cookies.h"
#include "app/sidebar_cookies.h"
#include "app/http_editor_find.h"
#include "app/http_editor_folder.h"
#include "app/http_editor_gql.h"
#include "app/http_editor_inherit.h"
#include "app/http_editor_network.h"
#include "app/http_editor_jshelp.h"
#include "app/http_editor_kv.h"
#include "app/http_editor_lexer.h"
#include "app/http_editor_pre.h"
#include "app/http_editor_util.h"
#include "app/http_editor_widgets.h"
#include "app/main_frame.h"
#include "app/panels.h"
#include "app/request_tab.h"
#include "core/http_client.h"
#include "core/oauth2.h"

namespace watt::app {
namespace {

using namespace http_editor;

const char* const kMethods[] = {"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"};

// GraphQL: a JSON body {"query", "variables"} edited as two editors (http_editor_util.h).
enum class BodyMode { None = 0, Json, Text, Xml, UrlEncoded, Multipart, Binary, GraphQL, CurlData };
const char* const kBodyModeLabels[] = {"None", "JSON", "Text", "XML", "URL-encoded", "Multipart", "File", "GraphQL"};
constexpr int kBodyModeCount = 8;  // CurlData has no radio button (shown only for imported -d @file)

// Modes edited as text in the body editor.
constexpr bool text_mode(BodyMode m) { return m == BodyMode::Json || m == BodyMode::Text || m == BodyMode::Xml; }

// Bearer / API key are headers of the request, Basic is SavedRequest::basic_auth, the rest are
// SavedRequest::auth (core: libcurl does Digest / NTLM / Negotiate / SigV4, oauth2.h the tokens).
// Inherit: the folder's / collection's auth (HttpAuth::Type::Inherit, resolved by core when sent).
enum class AuthMode { None = 0, Bearer, Basic, ApiKey, OAuth2, Digest, Ntlm, Negotiate, AwsSigV4, Inherit };
const char* const kAuthLabels[] = {"No auth", "Bearer token", "Basic auth", "API key", "OAuth 2.0",
                                   "Digest", "NTLM", "Negotiate (Kerberos)", "AWS Signature v4",
                                   "Inherit from parent"};
// The order the type list shows them in (Inherit first, as Postman / Insomnia / Bruno).
constexpr AuthMode kAuthOrder[] = {AuthMode::Inherit, AuthMode::None, AuthMode::Bearer, AuthMode::Basic,
                                   AuthMode::ApiKey, AuthMode::OAuth2, AuthMode::Digest, AuthMode::Ntlm,
                                   AuthMode::Negotiate, AuthMode::AwsSigV4};
constexpr bool scheme_mode(AuthMode m) { return m >= AuthMode::OAuth2 && m != AuthMode::Inherit; }
HttpAuth::Type scheme_type(AuthMode m) {
  switch (m) {
    case AuthMode::OAuth2: return HttpAuth::Type::OAuth2;
    case AuthMode::Digest: return HttpAuth::Type::Digest;
    case AuthMode::Ntlm: return HttpAuth::Type::Ntlm;
    case AuthMode::Negotiate: return HttpAuth::Type::Negotiate;
    case AuthMode::AwsSigV4: return HttpAuth::Type::AwsSigV4;
    default: return HttpAuth::Type::None;
  }
}
AuthMode mode_of(HttpAuth::Type t) {
  switch (t) {
    case HttpAuth::Type::OAuth2: return AuthMode::OAuth2;
    case HttpAuth::Type::Digest: return AuthMode::Digest;
    case HttpAuth::Type::Ntlm: return AuthMode::Ntlm;
    case HttpAuth::Type::Negotiate: return AuthMode::Negotiate;
    case HttpAuth::Type::AwsSigV4: return AuthMode::AwsSigV4;
    case HttpAuth::Type::Inherit: return AuthMode::Inherit;
    default: return AuthMode::None;
  }
}
// Page of the auth book per mode: Digest / NTLM / Negotiate share the user + password page.
size_t auth_page(AuthMode m) {
  switch (m) {
    case AuthMode::Digest: case AuthMode::Ntlm: case AuthMode::Negotiate: return 5;
    case AuthMode::AwsSigV4: return 6;
    case AuthMode::Inherit: return 7;
    default: return static_cast<size_t>(m);
  }
}

// Indicators for {{var}} highlighting (Scintilla reserves 0-7 for lexers).
constexpr int kIndOkBox = 8, kIndOkText = 9, kIndBadBox = 10, kIndBadText = 11;
// Closers inserted by auto-close (hidden), and the JSON syntax error squiggle.
constexpr int kIndAutoClose = 12, kIndJsonError = 13;
// The script syntax problem (pre / post script editors) and its annotation under the line.
constexpr int kIndJsLint = 14;
// GraphQL: unknown fields / arguments / variables (query) and keys (variables JSON).
constexpr int kIndGql = 15;
constexpr int kStyleJsLint = 41;
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
// The Text body's kinds (Postman's raw Text / HTML / JavaScript, Insomnia's and Bruno's YAML,
// EDN, SPARQL): the Content-Type each sets. Nothing else is stored: the header is the kind.
struct TextKind {
  const char* label;
  const char* type;
};
constexpr TextKind kTextKinds[] = {{"Plain text", "text/plain"},          {"HTML", "text/html"},
                                   {"JavaScript", "application/javascript"}, {"YAML", "application/yaml"},
                                   {"CSV", "text/csv"},                    {"EDN", "application/edn"},
                                   {"SPARQL", "application/sparql-query"}};

bool is_auto_content_type(std::string_view v) {
  v = trim(v);
  if (v.empty() || iequals(v, "application/json") || iequals(v, "application/x-www-form-urlencoded") ||
      iequals(v, "multipart/form-data") || iequals(v, "application/octet-stream") || iequals(v, "application/xml"))
    return true;
  for (const auto& k : kTextKinds)
    if (iequals(v, k.type)) return true;
  return false;
}

// The Text kind a Content-Type names (0, plain text, for any other).
int text_kind_of(std::string_view ct) {
  ct = trim(ct.substr(0, ct.find(';')));
  for (size_t i = 0; i < std::size(kTextKinds); ++i)
    if (iequals(ct, kTextKinds[i].type)) return static_cast<int>(i);
  if (iequals(ct, "text/javascript")) return 2;
  if (iequals(ct, "text/yaml") || iequals(ct, "application/x-yaml")) return 3;
  return 0;
}

wxFont mono_font(int pt) {
  wxFont f(wxFontInfo(pt).Family(wxFONTFAMILY_TELETYPE).FaceName("Cascadia Mono"));
  if (!f.IsOk() || f.GetFaceName() != "Cascadia Mono") f = wxFont(wxFontInfo(pt).Family(wxFONTFAMILY_TELETYPE).FaceName("Consolas"));
  return f;
}

// A '?' reference: a dark panel with the title and the wrapped text, at most `width` px wide.
// Closes on Esc, on a click elsewhere and when the app is deactivated (wxPopupTransientWindow).
class HelpPopup : public wxPopupTransientWindow {
 public:
  HelpPopup(wxWindow* parent, const wxString& title, const wxString& text, int width)
      : wxPopupTransientWindow(parent, wxBORDER_SIMPLE) {
    const Theme& t = Theme::get();
    SetBackgroundColour(t.border);
    panel_ = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS);
    panel_->SetBackgroundColour(t.field);
    auto* s = new wxBoxSizer(wxVERTICAL);
    const int pad = FromDIP(10);
    auto* head = new wxStaticText(panel_, wxID_ANY, title);
    head->SetForegroundColour(t.fg);
    head->SetFont(head->GetFont().Bold());
    s->Add(head, 0, wxLEFT | wxRIGHT | wxTOP, pad);
    auto* body = new wxStaticText(panel_, wxID_ANY, text);
    body->SetForegroundColour(t.fg);
    body->Wrap(width - 2 * pad);
    s->Add(body, 0, wxLEFT | wxRIGHT | wxTOP, pad);
    auto* foot = new wxStaticText(panel_, wxID_ANY, "Esc or a click elsewhere closes this");
    foot->SetForegroundColour(t.dim);
    s->Add(foot, 0, wxALL, pad);
    panel_->SetSizer(s);
    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(panel_, 1, wxEXPAND | wxALL, 1);
    SetSizerAndFit(outer);
    auto esc = [this](wxKeyEvent& e) {
      if (e.GetKeyCode() == WXK_ESCAPE) DismissAndNotify();
      else e.Skip();
    };
    panel_->Bind(wxEVT_CHAR_HOOK, esc);
    panel_->Bind(wxEVT_KEY_DOWN, esc);
    // The popup does not take the keyboard: Esc reaches the window under it, whose top-level
    // window sees it first (wxEVT_CHAR_HOOK). Unbound when this popup goes (sink tracking).
    if (wxWindow* top = wxGetTopLevelParent(parent)) top->Bind(wxEVT_CHAR_HOOK, &HelpPopup::on_hook, this);
  }
  void on_hook(wxKeyEvent& e) {
    if (e.GetKeyCode() == WXK_ESCAPE && IsShown()) {
      DismissAndNotify();
      return;
    }
    e.Skip();
  }
  wxWindow* focus() const { return panel_; }
  // wxPopupWindow is created always-on-top (WS_EX_TOPMOST): it would float over other apps.
  // Owned by the main window, it stays above that one without it.
  void Popup(wxWindow* focus = nullptr) override {
    wxPopupTransientWindow::Popup(focus);
#ifdef __WXMSW__
    ::SetWindowPos(static_cast<HWND>(GetHWND()), HWND_NOTOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#endif
  }

 protected:
  void OnDismiss() override {
    CallAfter([this] { Destroy(); });
  }

 private:
  wxPanel* panel_ = nullptr;
};

// Lifetime token for worker threads that post back to the editor.
struct Alive {
  std::mutex m;
  wxEvtHandler* target = nullptr;
  std::vector<void*> docs;  // Scintilla documents built on a worker, not yet swapped in
};

// Self test (WATT_HTTPED_SELFTEST): a thread that pings the top-level window with WM_NULL every
// 2 ms; the longest answer time is the longest UI-thread stall since the last take().
struct StallProbe {
  std::atomic<bool> run{true};
  std::atomic<double> worst{0};
  HWND hwnd = nullptr;
  double take() { return worst.exchange(0); }
  void start() {
    std::thread([self = this] {
      while (self->run.load()) {
        const auto a = std::chrono::steady_clock::now();
        DWORD_PTR r = 0;
        ::SendMessageTimeoutW(self->hwnd, WM_NULL, 0, 0, SMTO_NORMAL, 20000, &r);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
        double w = self->worst.load();
        while (ms > w && !self->worst.compare_exchange_weak(w, ms)) {
        }
        ::Sleep(2);
      }
    }).detach();
  }
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
// A worker-built body document holds this many lines; more are appended in slices (start_fill).
constexpr size_t kFillHeadLines = 40000;
// Blocks this long have their end found on a worker (json_big_blocks), not when folded.
constexpr int kBigBlockLines = 20000;

// The bytes of `s` up to and with its kFillHeadLines-th line end (all of it when it has fewer).
size_t fill_head(const std::string& s) {
  if (s.size() < 2 * kFillHeadLines) return s.size();
  static const bool off = ::GetEnvironmentVariableW(L"WATT_HTTPED_NOGROW", nullptr, 0) > 0;  // self test baseline
  if (off) return s.size();
  const char* p = s.data();
  const char* e = p + s.size();
  for (size_t n = 0; n < kFillHeadLines; ++n) {
    p = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(e - p)));
    if (!p) return s.size();
    ++p;
  }
  return static_cast<size_t>(p - s.data());
}
// A body line at least this long is slow to edit (Scintilla lays out whole lines).
constexpr size_t kLongLine = 64 * 1024;

// Tabs of the editor, in order.
enum : size_t { kTabParams, kTabBody, kTabHeaders, kTabAuth, kTabTests, kTabCookies, kTabOptions, kTabDocs, kTabCount };
// The tab strip has one more tab than the book has pages: the Scripts page shows under two
// tabs, "Scripts" (its pre-request side) and "Tests" (assertions, captures and the
// post-response script), like Postman's Pre-request Script / Tests and Bruno's Script / Assert.
constexpr size_t kStripScripts = kTabTests, kStripTests = kTabTests + 1;
constexpr size_t strip_of(size_t page) { return page <= kTabTests ? page : page + 1; }
constexpr size_t page_of(size_t strip) { return strip <= kTabTests ? strip : strip - 1; }

// A 1px separator line: never a Tab stop (an empty wxPanel would take the focus).
class Rule : public wxWindow {
 public:
  Rule(wxWindow* parent, wxSize size, wxColour c) : wxWindow(parent, wxID_ANY, wxDefaultPosition, size) {
    SetBackgroundColour(c);
  }
  bool AcceptsFocus() const override { return false; }
  bool AcceptsFocusFromKeyboard() const override { return false; }
};

// A page with only text on it: not a Tab stop either.
class QuietPanel : public wxPanel {
 public:
  explicit QuietPanel(wxWindow* parent) : wxPanel(parent) {}
  bool AcceptsFocus() const override { return false; }
  bool AcceptsFocusFromKeyboard() const override { return false; }
};

// A {{var}} name as a capture writes it: no braces, blanks or control characters.
std::string check_var_name(const std::string& v) {
  if (v.empty()) return {};
  if (v.size() > kMaxVarName) return "Variable name is too long";
  if (v.find("{{") != std::string::npos || v.find("}}") != std::string::npos || v.front() == '{')
    return "Type the variable name without braces: token, not {{token}}";
  for (unsigned char c : v)
    if (c <= 32 || c == 127) return "A variable name cannot hold blanks or control characters";
  return {};
}

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
  bool teardown_step() override;

  wxWindow* window() override { return this; }
  void load(const SavedRequest& r) override;
  void store(SavedRequest& r) const override;
  void store_head(SavedRequest& r) const override;
  void set_in_flight(bool in_flight) override;
  bool show_unresolved(const std::vector<std::string>& missing, bool in_url,
                       std::function<void()> send_anyway) override;
  void focus_url() override;
  void vars_changed() override {
    // Folder / collection variables, headers and auth too: the workspace may have just opened
    // (tabs restored at start-up load before it), or a folder.json was saved.
    refresh_inherited();
    crumb_sig_.clear();  // the breadcrumb's folders need the workspace
    update_crumb();
    update_preview();
    // Another environment: a {{token}} defined before may be undefined now (and back).
    if (url_) highlight_vars(url_, 0, url_->GetLength());
    for (auto& f : var_fields_)
      if (wxStyledTextCtrl* s = f.get()) highlight_vars(s, 0, s->GetLength());
    if (!jar_) refresh_jar();  // the workspace has just opened
    if (gql_query_) gql_timer_.StartOnce(50);  // another environment: another endpoint / schema
    recheck_banner();  // "{{base}} is not defined" may be defined in this environment
  }

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
  void note_body_method();
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
  // Options' "Encode URL" as it applies (the request's own, else a folder's, else on).
  bool encode_url_now() const { return ov_.net.encode_url.value_or(inh_.opts.n.encode_url.value_or(true)); }
  // Rows of the Path variables table from the URL's ":names" (the table is built on first use).
  void sync_path_rows();
  void layout_params_page();
  // ":names" in the URL that have no value (the send is refused until they have one).
  std::vector<std::string> empty_path_vars() const;
  // {{var}} names offered by completion: active environment, globals, dynamic.
  std::vector<std::string> var_names() const;
  std::string sent_form(const std::string& body) const;
  // One "{{name}}" span as sent: runtime vars, this request's folder / collection vars, then the
  // environment and globals (Workspace::expand).
  std::string expand_span(const Workspace& ws, const std::string& span) const;
  // {{names}} the request uses (URL, headers, auth, form fields) that resolve nowhere.
  std::vector<std::string> unresolved_vars(bool* in_url) const;
  // Braces of a {{var}} typed wrong ("{{user", "Id}}") in the URL, a path variable, a header or a
  // form field: "where" and what is wrong, first one first (brace_problem).
  std::vector<std::pair<std::string, std::string>> brace_problems() const;
  void url_inserted(const wxString& text);
  void import_curl(const std::string& cmd, CmdBackslashes bs = CmdBackslashes::AsCurlExe);
  void set_body_mode(BodyMode m, bool user);
  BodyMode body_mode() const { return body_mode_; }
  void set_content_type(const char* ct);
  void set_auth_mode(AuthMode m, bool user);
  void sync_api_key_query();
  void beautify();
  bool paste_long_json();
  // Ctrl+C / Ctrl+Insert of a selection over 1 MB: the UTF-16 clipboard text is built on a worker
  // (Scintilla's own copy converts 20 MB on the UI thread, ~300 ms), then handed to the
  // clipboard in one call.
  bool copy_big_selection();
  void auto_close(int ch);
  void lint();
  void show_lint(uint64_t gen, std::optional<JsonIssue> issue);
  void build_body_doc(std::shared_ptr<const std::string> text, bool reindent, uint64_t gen, uint64_t edits);
  void fill_body_doc(std::shared_ptr<const std::string> src, bool reindent, uint64_t gen, uint64_t edits, double worker_ms);
  void swap_body_doc(void* doc, bool reindent, uint64_t gen, uint64_t edits, double worker_ms,
                     std::shared_ptr<const std::string> src, size_t head);
  // A body of many lines is built on the worker with only its first kFillHeadLines lines; the rest
  // is appended here in slices of ~8 ms (run_fill), read-only meanwhile. Scintilla's fold table
  // (ContractionState::EnsureData, one entry per line) is made while the document is still short
  // (HideLines(1, 0) in start_fill) and grows with each slice, so the first fold of a 960k-line
  // body no longer builds it in one 70 ms step. store() gives the whole text (pending_body_).
  void start_fill(std::shared_ptr<const std::string> src, size_t head);
  void run_fill();
  // WATT_HTTPED_SELFTEST=<log file> (with WATT_HTTPED_BIG=<one-line JSON file>): the first
  // editor pastes the file into a JSON body three times and, each time, folds the biggest block,
  // unfolds it, folds it again and finds a term inside it, logging each step's UI time and the
  // longest UI-thread stall (a watchdog thread pings the window), then quits the app.
  void self_test_tick();
  // Pages built ahead of their first open, one per step while the app is idle (the Tests page's
  // shell, its two sides, then Options), like the response view's pages: a first click on
  // Tests / Scripts / Options then only shows a page. A step waits while input is coming in or a
  // big body is being filled / folded.
  void prebuild_step();
  void schedule_prebuild(int ms);
  wxTimer prebuild_timer_;  // wakes the idle handler, which runs the step when it is due
  std::chrono::steady_clock::time_point prebuild_at_{};
  bool prebuild_done_ = false;
  std::string prebuild_times_;
  bool tests_laid_out_ = false;
  // A tab that is not in the notebook yet (main_frame's hidden spare): its steps run while it
  // waits there, so Ctrl+T hands out a tab whose Tests / Scripts pages are already built.
  bool parked() const;
  // A click on Scripts / Tests before their side is built shows the page with one piece (a table)
  // and builds the rest one piece per timer tick (each 5-10 ms), never all of it in one step.
  wxTimer side_timer_;
  void finish_side_step();
  wxTimer st_timer_;
  double st_swap_ms_ = 0, st_install_ms_ = 0;
  std::string ctor_times_;  // the constructor's steps in ms (self test)
  int st_state_ = 0, st_round_ = 0, st_line_ = -1, st_swaps_ = 0, st_swaps_at_ = 0;
  std::chrono::steady_clock::time_point st_t0_;
  std::string st_log_;
  std::wstring st_out_;
  std::shared_ptr<const std::string> st_big_;
  std::shared_ptr<struct StallProbe> st_probe_;
  void end_fill(bool done);
  std::shared_ptr<const std::string> fill_src_;      // the whole text being appended
  std::shared_ptr<const std::string> fill_pending_;  // pending_body_ while it fills
  size_t fill_off_ = 0;                              // bytes of fill_src_ in the document
  size_t fill_step_ = size_t{1} << 20;               // bytes per slice (adapted to ~8 ms)
  void* fill_doc_ = nullptr;
  wxTimer fill_timer_;
  double fill_ui_ms_ = 0, fill_max_ms_ = 0;
  int fill_slices_ = 0;
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
  // A character typed after "{{" while the list is open (or with "{{ab" before the caret): the
  // list narrows to the names that start with what was typed, and closes when none does.
  void var_narrow(wxStyledTextCtrl* stc);
  void show_var_list(wxStyledTextCtrl* stc, const std::string& typed);
  std::set<wxStyledTextCtrl*> var_list_bound_;  // editors whose list pick inserts the name only
  bool step_over_closer(wxStyledTextCtrl* s, int key);
  void step_past_var_closer(wxStyledTextCtrl* s);
  std::string auth_header_name() const;
  bool close_on_blank_line(int key);  // JSON body: "}" on the blank line above an auto-closed "}"
  void dedent_closer();               // JSON body: a closer that starts its line takes the block's indent

  // send + upload prompt
  void do_send();
  void show_upload_prompt(const std::vector<std::filesystem::path>& files, const std::vector<std::string>& env);
  void show_info(const wxString& title, const wxString& text = {}, const wxString& action = {},
                 std::function<void()> on_action = {});
  void hide_banner();
  void recheck_banner();
  void wrap_banner();
  void set_banner_texts(const wxString& title, const wxString& text);

  EditorHost* host_;
  std::shared_ptr<Alive> alive_ = std::make_shared<Alive>();

  // url bar
  FlatButton* method_ = nullptr;
  std::string method_str_ = "GET";
  PickPopup* method_pick_ = nullptr;  // the method list while open (keys come from url_ / method_)
  bool method_pick_key(wxKeyEvent& e);
  bool method_pick_char(wxKeyEvent& e);
  void method_pick_focus_lost();
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
  bool long_line_banner_ = false;  // the banner shown is "The body has a N MB line": re-checked by lint()
  // tabs
  TabStrip* tabs_ = nullptr;
  wxSimplebook* book_ = nullptr;
  FlatButton* curl_btn_ = nullptr;
  wxStaticText* note_ = nullptr;
  wxTimer note_timer_;
  wxTimer jar_timer_;  // reads the jar again once the response view has recorded its Set-Cookie
  // JSON syntax check of the body (debounced, off the UI thread for big bodies)
  wxTimer lint_timer_;
  wxStaticText* lint_label_ = nullptr;
  uint64_t lint_gen_ = 0;
  // Folding in a big body (http_editor.cpp, big_fold): Scintilla's own fold walks and styles
  // every line of the block at once (250-370 ms for the root of a 1.4M-line body). Here the
  // block end comes from a bracket scan and the lines are hidden / shown in slices, one per
  // event-loop turn, in the order the clicks came.
  struct FoldOp {
    int from = 0, to = 0;  // lines, inclusive
    bool show = false;
  };
  bool big_fold(int line);
  int fold_block_end(int line);  // last line of the block opened on `line`; -1 when none
  // Find: unfolds the folded blocks that hide lines first..last. A big body is unfolded by the
  // sliced fold ops (no long stall), then the selection is scrolled into view.
  void reveal_body(int first, int last);
  bool reveal_pending_ = false;
  void run_fold_ops();
  std::deque<FoldOp> fold_ops_;
  uint64_t fold_edits_ = 0;  // body_edits_ the queued ops are for
  int fold_lines_ = 60000;   // lines hidden / shown per turn (~7 ms)
  double fold_max_ms_ = 0;   // the longest turn (self test)
  void* fold_doc_ = nullptr;
  wxTimer fold_timer_;  // a fallback: the slices run in idle time (on_idle)
  void on_idle(wxIdleEvent& e);
  // The big blocks (json_big_blocks, >= kBigBlockLines lines) of the body, found by the lint's
  // worker pass for body_edits_ == blocks_edits_: fold_block_end needs no 18 MB scan for them.
  std::vector<std::pair<int, int>> big_blocks_;
  uint64_t blocks_edits_ = ~uint64_t{0};
  void* blocks_doc_ = nullptr;
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
  // Minified + Beautify: right of the mode radios, or on a row of their own under them when the
  // editor is too narrow for both (never clipped).
  wxBoxSizer* body_tools_ = nullptr;
  wxBoxSizer* body_modes_row_ = nullptr;
  wxBoxSizer* body_tools_row_ = nullptr;
  wxPanel* body_page_panel_ = nullptr;
  bool body_tools_wrapped_ = false;
  void fit_body_tools();
  FlatButton* beautify_ = nullptr;
  ChoiceButton* text_kind_ = nullptr;  // the Text body's kind (sets the Content-Type)
  void sync_text_kind();
  wxSimplebook* body_book_ = nullptr;
  wxStyledTextCtrl* body_ = nullptr;
  // The form tables are built when their body mode is first shown; until then their rows live in
  // form_pending_ (0 urlencoded, 1 multipart).
  KvGrid* urlenc_ = nullptr;
  KvGrid* multipart_ = nullptr;
  // Body pages other than the editor are created when their mode is first shown (page index in
  // body_book_ per kind: 0 none, 1 text, 2 urlencoded, 3 multipart, 4 file, 5 curl data).
  int body_page_[7] = {-1, -1, -1, -1, -1, -1, -1};
  // GraphQL body: the two editors (built with their page) and their text until then.
  wxStyledTextCtrl* gql_query_ = nullptr;
  wxStaticText* gql_lint_ = nullptr;  // the query's first unbalanced bracket / string
  void lint_gql();
  wxStyledTextCtrl* gql_vars_ = nullptr;
  std::string gql_q_val_, gql_v_val_, gql_op_;
  std::string gql_query_text() const;
  std::string gql_vars_text() const;
  // GraphQL schema (http_editor_gql.h): introspection POSTed on a worker through the same
  // resolve path as a send (inherited headers and auth, OAuth token, {{vars}}, consents: the
  // worker's http_to_send() is the gate and a blocked fetch comes back here to ask), parsed on
  // the worker, cached per URL + environment in memory and in <ws>/.watt/graphql/. It drives
  // the query's completion (fields, arguments, input fields, enum values, $variables) and the
  // lint (unknown fields / arguments / types / variables underlined; variables JSON keys).
  wxStaticText* gql_chip_ = nullptr;   // "Schema: 42 types, 3 min ago" / "Fetching..." / error
  FlatButton* gql_fetch_ = nullptr;    // Fetch schema / Refresh
  // Explore: every type's fields (arguments, type, description) in a searchable list, grouped by
  // type with the root types first; picking one inserts it at the query's caret with its
  // required arguments and a { } for an object.
  FlatButton* gql_explore_ = nullptr;
  PickPopup* gql_explore_pop_ = nullptr;
  void gql_explore();
  void gql_insert_field(const std::string& type, const std::string& field);
  wxStaticText* gql_vars_lint_ = nullptr;
  std::shared_ptr<const GqlSchema> gql_schema_;
  std::string gql_key_;          // cache key of the URL + environment shown (gql_schema_ is for it)
  std::string gql_disk_tried_;   // the key whose .watt copy was looked for
  double gql_time_ = 0;          // when gql_schema_ was fetched (seconds since the epoch)
  bool gql_busy_ = false;
  bool gql_consented_ = false;   // the fetch in flight follows an Allow in the banner
  std::string gql_error_;
  uint64_t gql_gen_ = 0;
  std::vector<GqlItem> gql_items_;  // the completion list shown (descriptions for the call tip)
  GqlCompletion::Kind gql_items_kind_ = GqlCompletion::Kind::None;
  wxTimer gql_timer_;            // the URL / environment settled: look the schema up again
  wxTimer gql_age_timer_;        // the chip's "3 min ago"
  void build_gql_schema_row(wxWindow* parent, wxSizer* sizer);
  std::string gql_current_key() const;
  void gql_lookup();             // the schema for the current URL + environment (memory, then disk)
  void gql_fetch();
  void gql_fetch_blocked(std::shared_ptr<SavedRequest> req, const std::string& err);
  void gql_set_schema(std::shared_ptr<const GqlSchema> schema, const std::string& key, double when);
  void update_gql_chip();
  void gql_completion(bool explicit_request);
  PickPopup* gql_pop_ = nullptr;   // the completion list while it is open
  void gql_close_pop();
  void gql_pick(const wxString& name);
  bool gql_key(wxKeyEvent& e);   // auto-close helpers on wxEVT_CHAR
  void gql_char_added(int key);
  void lint_gql_vars();
  // WATT_HTTPED_GQLTEST=<log file> (WATT_HTTPED_GQLURL=<endpoint>): the first editor switches
  // to GraphQL, fetches the schema, types into the query and logs each step's UI time, the
  // completion lists and the lint, plus the longest UI-thread stall (WATT_HTTPED_SELFTEST_STAY:
  // stays open with a completion list showing).
  wxTimer gqlst_timer_;
  int gqlst_state_ = 0;
  std::chrono::steady_clock::time_point gqlst_t0_{};
  std::string gqlst_log_;
  std::wstring gqlst_out_;
  std::shared_ptr<StallProbe> gqlst_probe_;
  void gql_self_test_tick();
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
  std::vector<wxWeakRef<wxStyledTextCtrl>> var_fields_;  // every make_var_field, recoloured by vars_changed
  ChoiceButton* key_where_ = nullptr;
  AuthMode auth_mode_ = AuthMode::None;
  size_t auth_header_index_ = SIZE_MAX;  // where the auth header was in http.headers
  std::string api_key_param_;            // query param the API key auth manages
  // The Auth and Options pages are built the first time they are shown: until then (and after)
  // their values live here, so load / store / send never need the widgets.
  struct AuthVals {
    std::string bearer, user, pass, key_name, key_value;
    int key_where = 0;  // 0 header, 1 query
    HttpAuth adv;       // OAuth 2.0 / Digest / NTLM / Negotiate / SigV4 settings (type set on store)
  } av_;
  // OAuth 2.0 / Digest / NTLM / Negotiate / SigV4 widgets (built with the Auth page).
  wxStyledTextCtrl *o_token_url_ = nullptr, *o_auth_url_ = nullptr, *o_client_id_ = nullptr, *o_scope_ = nullptr,
                   *o_audience_ = nullptr, *o_user_ = nullptr, *o_prefix_ = nullptr, *o_param_ = nullptr;
  wxTextCtrl *o_secret_ = nullptr, *o_pass_ = nullptr;
  ChoiceButton *o_grant_ = nullptr, *o_client_auth_ = nullptr, *o_add_to_ = nullptr;
  CheckButton* o_refresh_ = nullptr;
  wxStaticText* o_status_ = nullptr;
  FlatButton* o_get_ = nullptr;
  FlatButton* o_clear_ = nullptr;
  wxTimer o_timer_;
  bool o_busy_ = false;
  std::string o_last_error_;
  void update_oauth_status();
  void get_oauth_token();
  wxStyledTextCtrl *s_user_ = nullptr, *s_aws_key_ = nullptr, *s_aws_region_ = nullptr, *s_aws_service_ = nullptr,
                   *s_aws_token_ = nullptr;
  wxTextCtrl *s_pass_ = nullptr, *s_aws_secret_ = nullptr;
  wxStaticText* s_hint_ = nullptr;
  void set_scheme_hint();
  std::vector<wxStaticText*> auth_hints_;
  int auth_wrap_ = -1;
  // Each auth type's page is built the first time it is shown (the OAuth 2.0 page alone has 13
  // fields): opening the Auth tab builds only the selected one.
  void build_auth_subpage(size_t index);
  std::array<bool, 8> auth_built_{};
  // Inherit from parent: what the folders / collection give (Workspace::effective_settings, read
  // on a worker after each load and folder save), shown on the Auth tab and under Auto headers.
  InheritView inh_;
  uint64_t inh_gen_ = 0;
  uint64_t inh_done_gen_ = 0;  // inh_ is current when it equals inh_gen_
  wxStaticText* inh_title_ = nullptr;
  wxStaticText* inh_detail_ = nullptr;
  wxStaticText* inh_errors_ = nullptr;
  FlatButton* inh_edit_ = nullptr;
  FlatButton* inh_own_ = nullptr;
  FlatButton* inh_coll_ = nullptr;
  // "No auth": the folder / collection Authorization headers that still go (they are headers,
  // not auth), with links to the Headers tab and the folder that sets them.
  wxStaticText* none_inh_ = nullptr;
  FlatButton* none_headers_ = nullptr;
  FlatButton* none_folder_ = nullptr;
  std::filesystem::path none_folder_path_;
  void show_none_note();
  void refresh_inherited();
  void show_inherited();
  void edit_folder(const std::filesystem::path& folder);
  // The folder of this request (the root for an unsaved one).
  std::filesystem::path own_folder() const;
  // The file whose folders this request inherits from (a history copy: the request it came from).
  std::filesystem::path inherit_from() const {
    std::filesystem::path f = host_->inherit_file();
    return f.empty() ? base_.file : f;
  }
  PickPopup* auth_pick_ = nullptr;
  void show_auth_menu();
  // A masked one-line box themed like the {{var}} fields (a native edit with no border inside a
  // FramePanel).
  wxTextCtrl* make_secret_box(wxWindow* parent, const char* hint, wxWindow** frame, long style = wxTE_PASSWORD);
  // OAuth 2.0 rows shown only for some grants / token placements: {label, field}.
  std::vector<wxWindow*> o_code_rows_, o_password_rows_, o_header_rows_, o_query_rows_;
  wxPanel* o_page_ = nullptr;
  void update_oauth_fields();
  // Widget creation split into short steps (the OAuth 2.0 page), run a few per UI turn.
  std::vector<std::function<void()>> auth_steps_;
  bool o_ready_ = false;  // every OAuth 2.0 widget exists
  void run_auth_steps();
  struct OptVals {
    long timeout_ms = 30000;
    bool redirects = true, verify_tls = true, fresh = false;
    bool jar = true;  // !SavedRequest::no_cookie_jar
    int version = 0;
    NetworkSettings net;  // the request's own CA / client certificate / proxy
    std::string pass;     // its passphrase (read on a worker after load)
  } ov_;
  NetworkPanel* net_panel_ = nullptr;
  uint64_t pass_gen_ = 0;  // passphrase reads / writes: only the latest one counts
  void load_passphrase();
  void save_passphrase(const std::string& pass);
  wxPanel* auth_holder_ = nullptr;
  wxPanel* opts_holder_ = nullptr;
  // Tests: Assert and Capture tables (core/asserts.h), built on first show; until then the rows
  // live in assert_vals_ / capture_vals_.
  wxPanel* tests_holder_ = nullptr;
  KvGrid* asserts_ = nullptr;
  KvGrid* captures_ = nullptr;
  wxStaticText* tests_hint_ = nullptr;
  // The folders' / collection's assertions (inh_.asserts), listed under the table with their
  // origin as the Headers tab lists inherited headers; a click edits that folder's settings.
  wxPanel* inh_asserts_box_ = nullptr;
  std::vector<std::pair<std::string, std::string>> inh_asserts_shown_;
  void show_inherited_asserts();
  wxPanel* tests_page_ = nullptr;
  KvPanel* assert_panel_ = nullptr;
  KvPanel* capture_panel_ = nullptr;
  void layout_tests_page();
  std::vector<Assertion> assert_vals_;
  std::vector<Capture> capture_vals_;
  // The checks as last loaded / stored, and into which request: the response view's Tests tab
  // edits the tab's request directly, and store() into that same request then keeps (and shows)
  // those edits instead of writing the table's older rows over them.
  mutable std::vector<Assertion> asserts_stored_;
  mutable const SavedRequest* asserts_target_ = nullptr;
  void set_assert_rows(const std::vector<Assertion>& a);
  wxWindow* build_tests_page();
  std::vector<Assertion> asserts_now() const;
  std::vector<Capture> captures_now() const;
  void update_tests_hint();
  // Notes on the Scripts page that wrap to its width (never cut with "..."): text as written.
  std::vector<std::pair<wxStaticText*, wxString>> tests_notes_;
  // Script trust (Workspace::scripts_trusted, a file outside the workspace): read on a worker,
  // shown under both script editors with a Trust / Stop trusting button. -1: not known yet.
  int trust_state_ = -1;
  uint64_t trust_gen_ = 0;
  wxStaticText* trust_text_[2] = {nullptr, nullptr};
  FlatButton* trust_btn_[2] = {nullptr, nullptr};
  void refresh_trust(bool after_send = false);
  // The Scripts tab's two sides, each built when first shown (or a UI turn after the other).
  wxPanel* pre_pg_ = nullptr;
  wxPanel* post_pg_ = nullptr;
  void build_pre_side();
  void build_post_side();
  // Each side in two idle steps (prebuild_step): its tables, then its script editor (an
  // STC with its help, ~10-20 ms each); a user who opens the side gets both at once.
  void build_pre_script();
  void build_post_script();
  void build_post_captures();  // the Captures table: its own idle step after the assertions
  bool split_sides_ = false;
  wxSizer* pre_sizer_ = nullptr;
  wxSizer* post_sizer_ = nullptr;
  // Options: the page, then its Certificates & proxy panel in a later idle step.
  bool defer_net_ = false;
  wxSizer* opts_sizer_ = nullptr;
  wxWindow* opts_page_ = nullptr;
  size_t net_at_ = 0;
  NetworkPanel* net_staged_ = nullptr;  // being built in idle steps (net_panel_ once complete)
  void build_net_panel(bool step);
  void add_trust_row(wxWindow* parent, wxSizer* s, int side);
  void setup_tests_grid(KvGrid* g);
  void show_trust();
  void set_trust(bool trusted);
  bool has_scripts() const;
  int tests_wrap_ = -1;
  // Pre-request | Post-response sides of the Scripts page, and their JavaScript editors
  // (SavedRequest::pre_script / post_script, run by the core sandbox, see core/script.h).
  TabStrip* script_tabs_ = nullptr;
  wxSimplebook* script_book_ = nullptr;
  wxStyledTextCtrl* pre_js_ = nullptr;
  wxStyledTextCtrl* post_js_ = nullptr;
  wxStyledTextCtrl* make_script_editor(wxWindow* parent, const char* placeholder);
  // pm. / bru. / req. / res. completion, Ctrl+Space, the lint squiggle; and the Snippets button.
  void setup_script_help(wxStyledTextCtrl* s, bool post);
  wxWindow* make_script_label(wxWindow* parent, wxStyledTextCtrl** editor, bool post);
  // A small "?" that opens the reference for a table or script (a popover, not page text).
  FlatButton* help_button(wxWindow* parent, const wxString& title, const wxString& text);
  void add_help(KvPanel* panel, const wxString& title, const wxString& text);
  void lint_script(wxStyledTextCtrl* s);
  void script_completion(wxStyledTextCtrl* s, bool post, bool explicit_request);
  // The first syntax problem of either script, as "Pre-request script, line 3: ..." (empty: none).
  wxString script_lint_summary() const;
  void update_script_badges();
  // The Scripts page on one side (0 pre-request, 1 post-response: the Tests tab).
  void show_script_side(size_t side);
  size_t want_side_ = 2;  // the side the Scripts page opens on when it is built (2: by content)
  // Pre-request variables (http_editor_pre.h): the first table of the Scripts page, saved in the
  // docs block (RowDocs::pre) while the model has no field for them. Evaluated on a worker right
  // before a send (do_send), and for the "Value now" column a moment after an edit.
  KvGrid* pre_ = nullptr;
  KvPanel* pre_panel_ = nullptr;
  std::vector<PreRow> pre_vals_;  // the rows until the page is built
  std::vector<PreRow> pre_now() const;
  static std::vector<PreVar> to_pre_vars(const std::vector<PreRow>& rows);
  std::set<std::string> pre_names_;  // enabled rows' names: they count as defined {{vars}}
  void update_pre_names();
  std::map<std::string, KvGrid::Preview> pre_preview_;
  wxTimer pre_timer_;
  uint64_t pre_gen_ = 0, pre_send_gen_ = 0;
  void preview_pre();
  bool pre_busy_ = false;   // a send waits for its pre-request variables
  bool pre_ready_ = false;  // do_send(): they were just computed and stored
  void run_pre_then_send(std::vector<PreVar> rows);
  void pre_done(uint64_t gen, const std::vector<PreVar>& rows, const std::vector<PreVarResult>& res, double ms);
  // The request as it would be sent, for evaluation (pre-request values, Copy as): store() into a
  // copy of base_ that leaves the assert bookkeeping alone; the body only when asked for.
  SavedRequest eval_request(bool with_body, bool sending) const;
  mutable bool store_no_body_ = false;
  // Copy as: the request with {{vars}} (and the previewed pre-request values) resolved when that
  // is on, and what the copy cannot do as sent ("" when nothing).
  SavedRequest export_request(std::string* warning) const;
  // The minified JSON body (Minified on, an edited big body), built on a worker after each edit
  // burst so a send never minifies 10 MB on the UI thread.
  std::shared_ptr<const std::string> minified_;
  uint64_t minified_edits_ = ~uint64_t{0};
  bool minify_busy_ = false;
  bool minified_current() const { return minified_ && minified_edits_ == body_edits_; }
  void minify_then_send();
  // Cookies: what the Cookie header(s) and curl -b jars will send (read-only list).
  wxPanel* cookies_holder_ = nullptr;
  wxPanel* cookie_list_ = nullptr;
  wxFlexGridSizer* cookie_grid_ = nullptr;
  wxStaticText* cookie_head_ = nullptr;
  std::vector<std::string> cookie_sig_;
  wxWindow* build_cookies_page();
  void update_cookies();
  // The workspace cookie jar (core/cookies.h): found and loaded on a worker (refresh_jar), then
  // every jar cookie in memory here; the ones the URL matches are filtered on the UI thread.
  std::shared_ptr<CookieJar> jar_;
  std::filesystem::path jar_root_;
  std::vector<Cookie> jar_all_;
  uint64_t jar_gen_ = 0;
  CheckButton* jar_check_ = nullptr;  // the Cookies tab's copy of Options' "Use the cookie jar"
  CheckButton* jar_opt_ = nullptr;
  void refresh_jar();
  std::vector<Cookie> jar_matches() const;
  void set_use_jar(bool on);
  // Descriptions of params (by key) and path variables (by name), kept when a row goes away and
  // comes back (retyping the URL).
  std::map<std::string, std::string> param_descs_, path_descs_;
  // Assertion descriptions by target (kept in the docs like the params' ones).
  mutable std::map<std::string, std::string> assert_descs_;
  // Bytes of the body as sent ({{vars}} expanded, minified), or nullopt when that is not known
  // before sending (files, multipart, or too big to compute here).
  std::optional<size_t> sent_body_size() const;
  // Docs: notes about the request (Markdown), in docs_text_ until the page is first shown.
  wxPanel* docs_holder_ = nullptr;
  wxStyledTextCtrl* docs_ = nullptr;
  wxHtmlWindow* docs_html_ = nullptr;  // the Preview (built on first use)
  std::function<void(bool, bool)> docs_preview_;  // true: show the rendered notes
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

  // Title row above the URL (Postman's request title + Save, Insomnia's breadcrumb): the folders
  // and name of the request with an unsaved mark, and a Save button (the frame's Ctrl+S).
  wxStaticText* crumb_ = nullptr;
  FlatButton* save_btn_ = nullptr;
  std::string crumb_sig_;
  void update_crumb();
  void save_now();

  // state
  SavedRequest base_;             // last loaded request, body cleared: fields this editor does not show
  std::string loaded_urlenc_;     // urlencoded body as loaded (kept byte for byte if not edited)
  std::vector<KvRow> loaded_urlenc_rows_;
  bool loading_ = false;
  bool torn_ = false;  // teardown_step() began: the tab is closing
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
  std::shared_ptr<const std::string> raw_on_load_;  // a loaded one-line JSON body being formatted
  // A loaded one-line JSON body: re-indented on a worker, then loaded like any big body; it is
  // shown formatted and sent / saved as loaded (raw_body_), like a pasted one.
  void load_formatted(std::shared_ptr<const std::string> text, uint64_t gen);
  std::shared_ptr<const std::string> raw_pending_;   // raw_body_ of the format still on a worker
  uint64_t raw_edits_ = 0;                           // body_edits_ when raw_body_ was shown
  // The document before the last swap (paste / format): Ctrl+Z with no undo history left goes
  // back to it in O(1) instead of Scintilla undoing a 25 MB insert.
  void* prev_doc_ = nullptr;
  bool prev_minify_ = false;
  // The document Ctrl+Z swapped out (with its own undo history): Ctrl+Y swaps it back. Dropped
  // on the next edit of the restored document.
  void* redo_doc_ = nullptr;
  bool redo_minify_ = false;
  std::shared_ptr<const std::string> redo_raw_;
  void drop_redo_doc();
  // Ctrl+Z / Ctrl+Y past the start / end of the document's own history.
  bool undo_swap();
  bool redo_swap();
  void release_doc_async(void* doc);
  // fresh: a document built by a loader (Scintilla leaves its undo collection off), whose
  // history starts empty here; else a document that keeps the history it had.
  void install_doc(void* doc, bool fresh);
  void paste_arrived(uint64_t gen, uint64_t edits, int start, int end, bool whole,
                     std::shared_ptr<const std::string> text, bool long_json);
  // A paste over a big selection: `body` is the whole new text, loaded on a worker and swapped in.
  void paste_swap(uint64_t gen, uint64_t edits, int caret, std::shared_ptr<const std::string> body);
  wxString swap_note_;  // said by swap_body_doc instead of "Formatted ..." (a paste_swap)
  int swap_caret_ = -1;
  uint64_t swap_note_gen_ = 0;  // the beautify_gen_ swap_note_ belongs to
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
  const auto c0 = Clock::now();
  auto lap = [this, c = c0](const char* what) mutable {
    const auto now = Clock::now();
    ctor_times_ += std::string(what) + " " + std::to_string(static_cast<int>(ms_between(c, now) * 10) / 10.0).substr(0, 4) + "  ";
    c = now;
  };

  auto* sz = new wxBoxSizer(wxVERTICAL);
  build_url_bar(sz);
  lap("url");
  build_banner(sz);

  // Tab strip + Copy as curl on the right.
  auto* tabrow = new wxBoxSizer(wxHORIZONTAL);
  tabs_ = new TabStrip(this);
  tabs_->set_tabs({"Params", "Body", "Headers", "Auth", "Scripts", "Tests", "Cookies", "Options", "Docs"});
  tabrow->Add(tabs_, 1, wxEXPAND);
  curl_btn_ = new FlatButton(this, "Copy as", FlatButton::Style::Subtle, true);
  curl_btn_->SetToolTip("Copy this request as a curl command (bash, cmd, PowerShell) or as code (Python, "
                        "JavaScript, Node.js, Go, Java, C#, PowerShell, HTTPie), with {{variables}} resolved or kept");
  tabrow->Add(curl_btn_, 0, wxALIGN_CENTER_VERTICAL);
  sz->Add(tabrow, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
  auto* line = new Rule(this, wxSize(-1, 1), t.border);
  sz->Add(line, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

  book_ = new wxSimplebook(this);
  book_->SetBackgroundColour(t.bg);
  lap("tabs");
  book_->AddPage(build_params_page(), "Params");
  lap("params");
  book_->AddPage(build_body_page(), "Body");
  lap("body");
  for (wxPanel** h : {&headers_holder_, &auth_holder_, &tests_holder_, &cookies_holder_, &opts_holder_, &docs_holder_}) {
    *h = new wxPanel(book_);
    (*h)->SetBackgroundColour(t.bg);
    (*h)->SetSizer(new wxBoxSizer(wxVERTICAL));
  }
  book_->AddPage(headers_holder_, "Headers");
  book_->AddPage(auth_holder_, "Auth");
  book_->AddPage(tests_holder_, "Tests");
  book_->AddPage(cookies_holder_, "Cookies");
  book_->AddPage(opts_holder_, "Options");
  book_->AddPage(docs_holder_, "Docs");
  sz->Add(book_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
  // Status line under the pages (paste / copy / format notes): never squeezes the tab strip.
  // Wrapped to the editor's width (at most a few lines), never cut.
  note_ = new wxStaticText(this, wxID_ANY, "");
  note_->SetForegroundColour(t.dim);
  note_->Hide();
  sz->Add(note_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
  SetSizer(sz);

  tabs_->on_select = [this](size_t strip) {
    if (strip == kStripScripts || strip == kStripTests) {
      show_script_side(strip == kStripTests ? 1 : 0);
      return;
    }
    const size_t i = page_of(strip);
    ensure_page(i);
    book_->ChangeSelection(i);
    if (i == kTabBody) sync_body_book();
    if (i == kTabHeaders) update_auto_headers();
    // The folders' settings may have changed on disk (another tab, git pull): read them again.
    if (i == kTabAuth || i == kTabHeaders) refresh_inherited();
    if (i == kTabCookies) {
      update_cookies();
      refresh_jar();  // another tab's response or the manager may have changed the jar
    }
    if (i == kTabTests) preview_pre();
  };
  curl_btn_->on_click = [this] { show_curl_menu(); };
  pre_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { preview_pre(); });
  note_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { note(""); });
  jar_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { refresh_jar(); });
  lint_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { lint(); });
  fold_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { run_fold_ops(); });
  fill_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { run_fill(); });
  prebuild_timer_.Bind(wxEVT_TIMER, [](wxTimerEvent&) { wxWakeUpIdle(); });
  side_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { finish_side_step(); });
  schedule_prebuild(800);
  Bind(wxEVT_IDLE, &HttpEditor::on_idle, this);

  // Ctrl+Enter sends from anywhere in the editor (ahead of the frame's own accelerator, so the
  // upload check below runs first).
  const int id_send = wxWindow::NewControlId();
  wxAcceleratorEntry acc[] = {{wxACCEL_CTRL, WXK_RETURN, id_send}, {wxACCEL_CTRL, WXK_NUMPAD_ENTER, id_send}};
  SetAcceleratorTable(wxAcceleratorTable(2, acc));
  Bind(wxEVT_MENU, [this](wxCommandEvent&) {
    // The focus can stay in a control of a tab that is no longer shown (a preview tab replaced by
    // quick-open): Ctrl+Enter always sends the active tab, never this hidden one.
    if (!IsShownOnScreen()) {
      if (wxWindow* top = wxGetTopLevelParent(this)) {
        wxCommandEvent ev(wxEVT_MENU, MainFrame::kSendCommand);
        ev.SetEventObject(top);
        wxPostEvent(top, ev);
      }
      return;
    }
    do_send();
  }, id_send);
  wxEvtHandler::AddFilter(this);
  lap("rest");
  ctor_times_ += "total " + std::to_string(ms_between(c0, Clock::now())).substr(0, 5);
  static bool st_ran = false;
  wxString st_out;
  if (!st_ran && wxGetEnv("WATT_HTTPED_SELFTEST", &st_out) && !st_out.empty()) {
    st_ran = true;
    st_out_ = st_out.ToStdWstring();
    st_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { self_test_tick(); });
    st_timer_.Start(20);
  }
  static bool gqlst_ran = false;
  if (!gqlst_ran && wxGetEnv("WATT_HTTPED_GQLTEST", &st_out) && !st_out.empty()) {
    gqlst_ran = true;
    gqlst_out_ = st_out.ToStdWstring();
    gqlst_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { gql_self_test_tick(); });
    gqlst_timer_.Start(20);
  }
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
  if (redo_doc_) body_->ReleaseDocument(redo_doc_);
  redo_doc_ = nullptr;
}

// The tab is closing (hidden, parked outside the notebook, deleted once this returns false): the
// big documents are let go one per step, each freed on a worker, so neither the close nor the
// final delete frees a 12 MB, 960k-line document (and its undo copy) on the UI thread.
bool HttpEditor::teardown_step() {
  if (!torn_) {
    torn_ = true;
    loading_ = true;  // nothing below is an edit
    for (wxTimer* t : {&prebuild_timer_, &side_timer_, &st_timer_, &fill_timer_, &note_timer_, &jar_timer_, &lint_timer_,
                       &fold_timer_, &gql_timer_, &gql_age_timer_, &gqlst_timer_, &o_timer_, &pre_timer_})
      t->Stop();
    Unbind(wxEVT_IDLE, &HttpEditor::on_idle, this);
    ++beautify_gen_;  // a paste / format still on its worker is dropped when it comes back
    ++body_load_gen_;
    end_fill(false);
    return true;
  }
  for (wxStyledTextCtrl* s : {body_, gql_query_, gql_vars_, pre_js_, post_js_}) {
    if (!s || s->GetLength() < 64 * 1024) continue;
    void* doc = s->GetDocPointer();
    s->AddRefDocument(doc);
    s->SetDocPointer(nullptr);  // an empty document: the old one keeps the reference taken above
    release_doc_async(doc);
    return true;
  }
  if (prev_doc_ || redo_doc_) {
    release_doc_async(std::exchange(prev_doc_, nullptr));
    release_doc_async(std::exchange(redo_doc_, nullptr));
    redo_raw_.reset();
    return true;
  }
  return false;
}

void HttpEditor::build_url_bar(wxSizer* sz) {
  const Theme& t = Theme::get();
  {
    auto* head = new wxBoxSizer(wxHORIZONTAL);
    crumb_ = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                              wxST_ELLIPSIZE_START | wxST_NO_AUTORESIZE);
    crumb_->SetForegroundColour(t.dim);
    crumb_->SetMinSize(wxSize(FromDIP(80), GetCharHeight()));
    head->Add(crumb_, 1, wxALIGN_CENTER_VERTICAL);
    save_btn_ = new FlatButton(this, "Save", FlatButton::Style::Subtle);
    save_btn_->SetToolTip("Save this request to its file (Ctrl+S)");
    save_btn_->on_click = [this] { save_now(); };
    head->Add(save_btn_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    sz->AddSpacer(FromDIP(8));
    sz->Add(head, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
  }
  auto* row = new wxBoxSizer(wxHORIZONTAL);
  url_box_ = new FramePanel(this, t.field, t.border);
  auto* inner = new wxBoxSizer(wxHORIZONTAL);
  method_ = new FlatButton(url_box_, "GET", FlatButton::Style::Method, true);
  method_->set_text_colour(method_colour("GET"));
  method_->SetToolTip("HTTP method (Alt+Down in the URL opens this list, Ctrl+Up / Ctrl+Down cycles)");
  inner->Add(method_, 0, wxEXPAND | wxALL, FromDIP(3));
  auto* sep = new Rule(url_box_, wxSize(1, -1), t.border);
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
  sz->AddSpacer(FromDIP(4));
  sz->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
  sz->AddSpacer(FromDIP(8));

  method_->on_click = [this] { show_method_menu(); };
  // The method button with the focus: Down / Alt+Down open the list, typing a letter opens it
  // filtered (as in Postman, where the method box takes typed text). Bound after FlatButton's own
  // handlers, so these run first.
  method_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
    if (method_pick_ && method_pick_key(e)) return;
    const int k = e.GetKeyCode();
    if (!method_pick_ && (k == WXK_DOWN || k == WXK_NUMPAD_DOWN) && (!e.HasAnyModifiers() || e.GetModifiers() == wxMOD_ALT)) {
      show_method_menu();
      return;
    }
    e.Skip();
  });
  method_->Bind(wxEVT_CHAR, [this](wxKeyEvent& e) {
    if (method_pick_ && method_pick_char(e)) return;
    const wxChar c = e.GetUnicodeKey();
    if (!method_pick_ && !e.ControlDown() && !e.AltDown() && c > 32 && c < 127 && c != ' ') {
      show_method_menu();
      if (method_pick_) method_pick_->chr(static_cast<wxChar>(wxToupper(c)));
      return;
    }
    e.Skip();
  });
  method_->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) {
    e.Skip();
    method_pick_focus_lost();
  });
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
    if (method_pick_ && method_pick_key(e)) return;
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
      note_body_method();
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
    if (method_pick_ && method_pick_char(e)) return;
    if (step_over_closer(url_, e.GetUnicodeKey())) return;
    e.Skip();
  });
  s->Bind(wxEVT_STC_CHARADDED, [this](wxStyledTextEvent& e) {
    e.Skip();
    if (e.GetKey() == '{') var_completion(url_);
    else var_narrow(url_);
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
    method_pick_focus_lost();
    // Focus moved elsewhere in watt (Send, another tab, the body): a left-over selection would
    // look like the URL is selected and hide the {{var}} colours. A switch to another app keeps
    // it (GetWindow() is null then), like a native text field.
    if (e.GetWindow() && url_->GetSelectionStart() != url_->GetSelectionEnd()) url_->SetEmptySelection(url_->GetCurrentPos());
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
  minify_ = new CheckButton(p, "Send compact");
  minify_->SetToolTip("The body is shown and edited formatted. On: sent on one line, an unedited paste byte for "
                      "byte as pasted, an edited body without the whitespace between tokens. Off: sent as shown");
  minify_->on_change = [this] {
    if (!loading_) touched();
    // Only what is sent changes: the view stays, repainted whole (no stale line paint over a
    // big document).
    if (body_) body_->Refresh(false);
  };
  {
    std::vector<wxString> kinds;
    for (const auto& k : kTextKinds) kinds.emplace_back(k.label);
    text_kind_ = new ChoiceButton(p, std::move(kinds));
    text_kind_->SetToolTip("What the text is: sets the Content-Type header (text/html, application/yaml, ...)");
    text_kind_->Hide();
    text_kind_->on_select = [this](int i) {
      if (i < 0 || i >= static_cast<int>(std::size(kTextKinds))) return;
      set_content_type(kTextKinds[i].type);
      touched();
    };
  }
  body_tools_ = new wxBoxSizer(wxHORIZONTAL);
  body_tools_->Add(text_kind_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
  body_tools_->Add(minify_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
  body_tools_->Add(beautify_, 0, wxALIGN_CENTER_VERTICAL);
  // As tall as the Beautify button in every mode, so the radio row never moves.
  body_tools_->Add(0, beautify_->GetBestSize().y);
  modes->Add(body_tools_, 0, wxALIGN_CENTER_VERTICAL);
  body_modes_row_ = modes;
  s->Add(modes, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(8));
  body_tools_row_ = new wxBoxSizer(wxHORIZONTAL);
  body_tools_row_->AddStretchSpacer();
  s->Add(body_tools_row_, 0, wxEXPAND);
  body_page_panel_ = p;
  p->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
    e.Skip();
    fit_body_tools();
  });

  body_book_ = new wxSimplebook(p);
  body_book_->SetBackgroundColour(t.bg);
  // 1: JSON / text editor
  {
    auto* frame = new FramePanel(body_book_, t.field, t.border);
    auto* fs = new wxBoxSizer(wxVERTICAL);
    body_ = new wxStyledTextCtrl(frame, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    setup_body_stc();
    attach_find(body_, [this](int first, int last) { reveal_body(first, last); });
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

void HttpEditor::fit_body_tools() {
  if (!body_tools_ || !body_page_panel_) return;
  const int avail = body_page_panel_->GetClientSize().x;
  int tools = 0;
  if (minify_->IsShown()) tools += minify_->GetBestSize().x + FromDIP(10);
  if (text_kind_ && text_kind_->IsShown()) tools += text_kind_->GetBestSize().x + FromDIP(10);
  if (beautify_->IsShown()) tools += beautify_->GetBestSize().x;
  const bool wrap = avail > 0 && tools > 0 && body_modes_->GetBestSize().x + FromDIP(16) + tools > avail;
  if (wrap != body_tools_wrapped_) {
    body_tools_wrapped_ = wrap;
    (wrap ? body_modes_row_ : body_tools_row_)->Detach(body_tools_);
    (wrap ? body_tools_row_ : body_modes_row_)->Add(body_tools_, 0, wxALIGN_CENTER_VERTICAL | wxBOTTOM, wrap ? FromDIP(6) : 0);
  }
  body_page_panel_->Layout();
}

KvGrid* HttpEditor::form_grid(int which) {
  ensure_body_page(which == 0 ? 2 : 3);
  return which == 0 ? urlenc_ : multipart_;
}

// Shows the page of the current body mode, creating it on first use; only while the Body tab is
// shown (a new GET tab never builds the "no body" page or a form table it does not show).
void HttpEditor::sync_body_book() {
  if (book_->GetSelection() != kTabBody) return;
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
    case BodyMode::GraphQL: kind = 6; break;
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
    auto* np = new QuietPanel(body_book_);
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
    g->var_names = [this] { return var_names(); };
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
  } else if (kind == 6) {
    auto* gp = new wxPanel(body_book_);
    gp->SetBackgroundColour(t.bg);
    auto* gs = new wxBoxSizer(wxVERTICAL);
    auto label = [&](const char* text) {
      auto* l = new wxStaticText(gp, wxID_ANY, text);
      l->SetForegroundColour(t.dim);
      gs->Add(l, 0, wxTOP | wxBOTTOM, FromDIP(4));
    };
    build_gql_schema_row(gp, gs);
    label("Query");
    gql_query_ = make_script_editor(gp, "query { user(id: $id) { id name } }");
    gql_query_->SetKeyWords(0, "query mutation subscription fragment on true false null");
    gql_query_->SetKeyWords(1, "");
    {
      wxStyledTextCtrl* q = gql_query_;
      q->IndicatorSetStyle(kIndAutoClose, wxSTC_INDIC_HIDDEN);
      q->IndicatorSetStyle(kIndGql, wxSTC_INDIC_SQUIGGLE);
      q->IndicatorSetForeground(kIndGql, t.js_error);
      q->Bind(wxEVT_CHAR, [this](wxKeyEvent& e) {
        if (gql_key(e)) return;
        e.Skip();
      });
      // The completion list (a themed PickPopup that never takes the focus) is driven from here.
      q->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
        const int k = e.GetKeyCode();
        if (k == WXK_SPACE && e.GetModifiers() == wxMOD_CONTROL) {
          gql_completion(true);
          return;
        }
        if (gql_pop_ && e.GetModifiers() == wxMOD_NONE) {
          switch (k) {
            case WXK_UP: case WXK_DOWN: case WXK_PAGEUP: case WXK_PAGEDOWN: case WXK_NUMPAD_UP:
            case WXK_NUMPAD_DOWN: case WXK_RETURN: case WXK_NUMPAD_ENTER: case WXK_TAB: case WXK_ESCAPE:
              gql_pop_->key(k);  // moves in the list, picks, or closes it
              return;
            case WXK_BACK:  // the word shrinks: the list follows once Scintilla deleted the character
              CallAfter([this] {
                if (gql_pop_) gql_completion(false);
              });
              break;
            case WXK_LEFT: case WXK_RIGHT: case WXK_HOME: case WXK_END:
              gql_close_pop();
              break;
            default:
              break;
          }
        }
        e.Skip();
      });
      q->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) {
        e.Skip();
        CallAfter([this] { gql_close_pop(); });
      });
      q->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
        e.Skip();
        gql_close_pop();
      });
      q->Bind(wxEVT_STC_CHARADDED, [this](wxStyledTextEvent& e) {
        e.Skip();
        gql_char_added(e.GetKey());
      });
    }
    gs->Add(gql_query_, 3, wxEXPAND);
    // The lint lines keep their row when empty: a message coming or going never moves the
    // editors (a click aimed at Variables landed in the query when it did).
    gql_lint_ = new wxStaticText(gp, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                 wxST_NO_AUTORESIZE | wxST_ELLIPSIZE_END);
    gql_lint_->SetForegroundColour(t.var_bad);
    gql_lint_->SetMinSize(wxSize(-1, GetCharHeight()));
    gs->Add(gql_lint_, 0, wxEXPAND | wxTOP, FromDIP(2));
    gql_query_->Bind(wxEVT_STC_CHANGE, [this](wxStyledTextEvent& e) {
      e.Skip();
      lint_gql();
      lint_gql_vars();  // the declarations may have changed
    });
    label("Variables (JSON, {{variables}} allowed)");
    gql_vars_ = make_script_editor(gp, "{ \"id\": \"{{userId}}\" }");
    gql_vars_->IndicatorSetStyle(kIndGql, wxSTC_INDIC_SQUIGGLE);
    gql_vars_->IndicatorSetForeground(kIndGql, t.js_error);
    gs->Add(gql_vars_, 2, wxEXPAND);
    gql_vars_lint_ = new wxStaticText(gp, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                      wxST_NO_AUTORESIZE | wxST_ELLIPSIZE_END);
    gql_vars_lint_->SetForegroundColour(t.var_bad);
    gql_vars_lint_->SetMinSize(wxSize(-1, GetCharHeight()));
    gs->Add(gql_vars_lint_, 0, wxEXPAND | wxTOP, FromDIP(2));
    gql_vars_->Bind(wxEVT_STC_CHANGE, [this](wxStyledTextEvent& e) {
      e.Skip();
      lint_gql_vars();
    });
    auto* note = new wxStaticText(gp, wxID_ANY, "Sent as a JSON POST body {\"query\": ..., \"variables\": ...} "
                                                "(Content-Type: application/json).");
    note->SetForegroundColour(t.dim);
    gs->Add(note, 0, wxTOP, FromDIP(4));
    gp->SetSizer(gs);
    const bool was = loading_;
    loading_ = true;
    gql_query_->SetText(wxString::FromUTF8(gql_q_val_));
    gql_vars_->SetText(wxString::FromUTF8(gql_v_val_));
    gql_query_->EmptyUndoBuffer();
    gql_vars_->EmptyUndoBuffer();
    loading_ = was;
    lint_gql();
    lint_gql_vars();
    gql_lookup();
    page = gp;
  } else if (kind == 5) {
    auto* cp = new QuietPanel(body_book_);
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
    // Our own lexer (http_editor_lexer.cpp): LexJSON styles the rest of a line as an error
    // after an unquoted {{var}}.
    body_->SetILexer(make_json_lexer());
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
  // A folded line ends in a boxed "..." (with the line count when Scintilla folds it itself).
  s->SetDefaultFoldDisplayText(wxString::FromUTF8("\xE2\x80\xA6"));
  s->FoldDisplayTextSetStyle(wxSTC_FOLDDISPLAYTEXT_BOXED);
  s->EOLAnnotationSetVisible(wxSTC_EOLANNOTATION_STANDARD);  // the line count of a big fold (big_fold)
  s->StyleSetForeground(wxSTC_STYLE_FOLDDISPLAYTEXT, t.dim);
  s->StyleSetBackground(wxSTC_STYLE_FOLDDISPLAYTEXT, t.field);
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
    // The first edit of a pasted / loaded one-line JSON: it no longer goes out byte for byte.
    if (raw_body_ && raw_edits_ + 1 == body_edits_ && minify_ && minify_->value())
      note("Body edited: now sent without the whitespace between tokens (Send compact), not as pasted. "
           "Undo the edit to send the pasted bytes again.");
    drop_redo_doc();  // an edit after Ctrl+Z: the swapped-out document is no longer a redo
    body_hl_first_ = -1;
    if (body_mode_ == BodyMode::Json) lint_timer_.StartOnce(body_->GetLength() < 256 * 1024 ? 250 : 300);
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
    const bool copy = (e.GetModifiers() == wxMOD_CONTROL && (k == 'C' || k == WXK_INSERT || k == WXK_NUMPAD_INSERT));
    if (copy && copy_big_selection()) return;
    // Undo past a swapped-in paste / format: the previous document comes back in O(1) (the edits
    // made after the paste are undone one by one first, by Scintilla). Redo swaps forward again.
    const int mods = e.GetModifiers();
    if (mods == wxMOD_CONTROL && k == 'Z' && undo_swap()) return;
    if (((mods == wxMOD_CONTROL && k == 'Y') || (mods == (wxMOD_CONTROL | wxMOD_SHIFT) && k == 'Z')) && redo_swap())
      return;
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
    if (e.GetMargin() != 1) return;
    const int line = body_->LineFromPosition(e.GetPosition());
    if (big_fold(line)) return;
    // A folded block shows how much it hides after its first line ("{ ... 12 lines").
    if (body_->GetFoldExpanded(line) && (body_->GetFoldLevel(line) & wxSTC_FOLDLEVELHEADERFLAG)) {
      const int last = body_->GetLastChild(line, -1);
      if (last > line) {
        body_->ToggleFoldShowText(line, wxString::Format(wxString::FromUTF8("\xE2\x80\xA6 %d lines"), last - line));
        return;
      }
    }
    body_->ToggleFold(line);
  });
  // Auto-indent: a new line keeps the previous line's indent, one level more after { or [.
  // Typing over a closer this editor inserted itself steps over it (like Postman / VS Code).
  s->IndicatorSetStyle(kIndAutoClose, wxSTC_INDIC_HIDDEN);
  s->IndicatorSetStyle(kIndJsonError, wxSTC_INDIC_SQUIGGLE);
  s->IndicatorSetForeground(kIndJsonError, t.js_error);
  s->Bind(wxEVT_CHAR, [this](wxKeyEvent& e) {
    if (text_mode(body_mode_) && step_over_closer(body_, e.GetUnicodeKey())) return;
    if (body_mode_ == BodyMode::Json && close_on_blank_line(e.GetUnicodeKey())) return;
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
    if (body_->AutoCompActive()) var_narrow(body_);
    if ((key == '}' || key == ']') && body_mode_ == BodyMode::Json) {
      dedent_closer();
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
  headers_->var_names = [this] { return var_names(); };
  headers_->check = [g = headers_](int col, const std::string& text) -> std::string {
    return col == g->col_key() ? header_name_problem(text) : std::string();
  };
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
  if (i == kTabHeaders && !headers_) {
    wxWindowUpdateLocker lock(headers_holder_);
    headers_holder_->GetSizer()->Add(build_headers_page(), 1, wxEXPAND);
    const bool was = loading_;
    loading_ = true;
    headers_->set_rows(header_pending_);
    header_pending_.clear();
    loading_ = was;
    update_files_row();
    headers_holder_->Layout();
  } else if (i == kTabAuth && !auth_book_) {
    wxWindowUpdateLocker lock(auth_holder_);
    auth_holder_->GetSizer()->Add(build_auth_page(), 1, wxEXPAND);
    push_auth();
    auth_holder_->Layout();
  } else if (i == kTabTests && !tests_page_) {
    wxWindowUpdateLocker lock(tests_holder_);
    tests_holder_->GetSizer()->Add(build_tests_page(), 1, wxEXPAND);
    tests_holder_->Layout();
  } else if (i == kTabCookies && !cookie_list_) {
    wxWindowUpdateLocker lock(cookies_holder_);
    cookies_holder_->GetSizer()->Add(build_cookies_page(), 1, wxEXPAND);
    cookies_holder_->Layout();
  } else if (i == kTabOptions && !timeout_) {
    wxWindowUpdateLocker lock(opts_holder_);
    opts_holder_->GetSizer()->Add(build_options_page(), 1, wxEXPAND);
    push_opts();
    opts_holder_->Layout();
  } else if (i == kTabOptions && !net_panel_) {
    build_net_panel(false);
    opts_holder_->Layout();
  } else if (i == kTabDocs && !docs_) {
    wxWindowUpdateLocker lock(docs_holder_);
    docs_holder_->GetSizer()->Add(build_docs_page(), 1, wxEXPAND);
    docs_holder_->Layout();
  }
}

void HttpEditor::push_auth() {
  if (!auth_book_) return;
  const bool was = loading_;
  loading_ = true;
  build_auth_subpage(auth_page(auth_mode_));
  if (bearer_) bearer_->ChangeValue(wxString::FromUTF8(av_.bearer));
  if (basic_user_) {
    basic_user_->ChangeValue(wxString::FromUTF8(av_.user));
    basic_pass_->ChangeValue(wxString::FromUTF8(av_.pass));
    basic_pass_alt_->ChangeValue(wxString::FromUTF8(av_.pass));
  }
  if (key_name_) {
    key_name_->ChangeValue(wxString::FromUTF8(av_.key_name));
    key_value_->ChangeValue(wxString::FromUTF8(av_.key_value));
    key_where_->set_selection(av_.key_where);
  }
  const HttpAuth& a = av_.adv;
  const OAuth2Config& o = a.oauth2;
  if (o_ready_) {
  o_token_url_->ChangeValue(wxString::FromUTF8(o.token_url));
  o_auth_url_->ChangeValue(wxString::FromUTF8(o.auth_url));
  o_client_id_->ChangeValue(wxString::FromUTF8(o.client_id));
  o_secret_->ChangeValue(wxString::FromUTF8(o.client_secret));
  o_scope_->ChangeValue(wxString::FromUTF8(o.scope));
  o_audience_->ChangeValue(wxString::FromUTF8(o.audience));
  o_user_->ChangeValue(wxString::FromUTF8(o.username));
  o_pass_->ChangeValue(wxString::FromUTF8(o.password));
  o_prefix_->ChangeValue(wxString::FromUTF8(o.header_prefix));
  o_param_->ChangeValue(wxString::FromUTF8(o.query_param));
  o_grant_->set_selection(static_cast<int>(o.grant));
  o_client_auth_->set_selection(static_cast<int>(o.client_auth));
  o_add_to_->set_selection(static_cast<int>(o.add_to));
  o_refresh_->set_value(o.auto_refresh);
  update_oauth_fields();
  }
  if (s_user_) {
    s_user_->ChangeValue(wxString::FromUTF8(a.user));
    s_pass_->ChangeValue(wxString::FromUTF8(a.password));
  }
  if (s_aws_key_) {
  s_aws_key_->ChangeValue(wxString::FromUTF8(a.aws_access_key));
  s_aws_secret_->ChangeValue(wxString::FromUTF8(a.aws_secret_key));
  s_aws_token_->ChangeValue(wxString::FromUTF8(a.aws_session_token));
  s_aws_region_->ChangeValue(wxString::FromUTF8(a.aws_region));
  s_aws_service_->ChangeValue(wxString::FromUTF8(a.aws_service));
  }
  auth_btn_->SetLabel(kAuthLabels[static_cast<int>(auth_mode_)]);
  auth_book_->ChangeSelection(auth_page(auth_mode_));
  set_scheme_hint();
  update_oauth_status();
  loading_ = was;
}

void HttpEditor::set_scheme_hint() {
  if (!s_hint_) return;
  const char* h = auth_mode_ == AuthMode::Ntlm
                      ? "NTLM (Windows SSPI): libcurl answers the server's 401 challenge. Leave both empty to sign in as "
                        "the current Windows user."
                  : auth_mode_ == AuthMode::Negotiate
                      ? "Negotiate / Kerberos (SPNEGO, Windows SSPI). Leave both empty to use the current Windows logon."
                      : "Digest: libcurl answers the server's 401 challenge (one extra round trip per connection). "
                        "{{variables}} are allowed.";
  if (s_hint_->GetLabel() != h) {
    s_hint_->SetLabel(h);
    s_hint_->Wrap(auth_wrap_ > 0 ? auth_wrap_ : FromDIP(560));
    s_hint_->GetParent()->Layout();
  }
}

// "Token valid for 54 min" from the core cache (expanding a few settings: cheap).
void HttpEditor::update_oauth_status() {
  if (!o_status_) return;
  const Workspace* ws = host_->workspace();
  wxString st;
  bool warn = false;
  if (o_busy_) {
    st = wxString::FromUTF8("Getting a token\xE2\x80\xA6");
  } else if (!ws) {
    st = "The workspace is still opening.";
  } else {
    SavedRequest r = base_;
    r.auth = av_.adv;
    r.auth.type = HttpAuth::Type::OAuth2;
    const auto tok = oauth2_cached_token(*ws, r);
    const long long now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const bool code = av_.adv.oauth2.grant == OAuth2Config::Grant::AuthorizationCode;
    if (tok && tok->valid(now, 0)) {
      if (tok->expires_at == 0) {
        st = "Token cached (no expiry given by the server).";
      } else {
        const long long left = tok->expires_at - now;
        st = left >= 120 ? wxString::Format("Token valid for %lld min", left / 60) : wxString::Format("Token valid for %lld s", left);
      }
      st += wxString::FromUTF8(" \xC2\xB7 " + (tok->token_type.empty() ? std::string("Bearer") : tok->token_type));
      if (!tok->scope.empty()) st += wxString::FromUTF8(" \xC2\xB7 scope " + tok->scope);
      if (!tok->refresh_token.empty()) st += wxString::FromUTF8(" \xC2\xB7 refresh token");
    } else if (tok) {
      st = !tok->refresh_token.empty() && av_.adv.oauth2.auto_refresh
               ? "Token expired: the next send refreshes it first (off the UI thread)."
           : code ? "Token expired: Get new token to sign in again."
                  : "Token expired: the next send gets a new one first (off the UI thread).";
      warn = code && tok->refresh_token.empty();
    } else {
      st = code ? "No token: Get new token opens the sign-in page in your browser."
                : "No token yet: the next send gets one first (off the UI thread), or Get new token now.";
      warn = code;
    }
    if (!o_last_error_.empty()) {
      st = wxString::FromUTF8("\xE2\x9A\xA0 " + o_last_error_);
      warn = true;
    }
  }
  const Theme& t = Theme::get();
  o_status_->SetForegroundColour(warn ? t.var_bad : t.dim);
  if (o_status_->GetLabel() != st) {
    o_status_->SetLabel(st);
    o_status_->Wrap(auth_wrap_ > 0 ? auth_wrap_ : FromDIP(560));
    o_status_->GetParent()->Layout();
  }
  if (o_get_) o_get_->Enable(!o_busy_);
}

void HttpEditor::get_oauth_token() {
  const Workspace* ws = host_->workspace();
  if (!ws || o_busy_) return;
  auto req = std::make_shared<SavedRequest>(base_);
  store(*req);
  req->auth = av_.adv;
  req->auth.type = HttpAuth::Type::OAuth2;
  auto wsc = std::make_shared<Workspace>(*ws);
  o_busy_ = true;
  o_last_error_.clear();
  update_oauth_status();
  const bool verify = ov_.verify_tls;
  std::thread([alive = alive_, req, wsc, verify] {
    OAuth2Hooks hooks;
    hooks.interactive = true;  // the authorization code grant opens the system browser
    hooks.verify_tls = verify;
    const OAuth2Result res = oauth2_get_token(*wsc, *req, hooks, true);
    std::lock_guard lk(alive->m);
    if (!alive->target) return;
    auto* ed = static_cast<HttpEditor*>(alive->target);
    ed->CallAfter([ed, ok = res.ok, err = res.error, ms = res.elapsed_ms] {
      ed->o_busy_ = false;
      ed->o_last_error_ = ok ? std::string() : (err.empty() ? std::string("no token") : err);
      ed->update_oauth_status();
      if (ok) ed->note(wxString::Format("Got a new OAuth 2.0 token in %.1f ms (off the UI thread)", ms));
    });
  }).detach();
}

void HttpEditor::push_opts() {
  if (!timeout_) return;
  const bool was = loading_;
  loading_ = true;
  timeout_->ChangeValue(wxString::Format("%ld", std::clamp<long>(ov_.timeout_ms, 0, 24L * 3600 * 1000)));
  redirects_->set_value(ov_.redirects);
  verify_tls_->set_value(ov_.verify_tls);
  fresh_->set_value(ov_.fresh);
  if (jar_opt_) jar_opt_->set_value(ov_.jar);
  version_->set_selection(ov_.version);
  if (net_panel_) {
    net_panel_->set(ov_.net, ov_.pass);
    net_panel_->set_inherited(wxString::FromUTF8(inh_.network));
    net_panel_->set_above(inh_.opts);
    net_panel_->set_follow(ov_.redirects);
  }
  loading_ = was;
}

// The request's own client certificate passphrase lives in .watt/secrets.json (DPAPI): read
// and written on a worker, keyed by the request id.
void HttpEditor::load_passphrase() {
  const Workspace* ws = host_->workspace();
  const uint64_t gen = ++pass_gen_;
  if (!ws || ov_.net.cert_file.empty()) {
    ov_.pass.clear();
    return;
  }
  auto wsc = std::make_shared<Workspace>(*ws);
  const std::string key = Workspace::passphrase_key(SettingsOrigin{}, base_.id);
  std::thread([alive = alive_, wsc, key, gen] {
    std::string pass = wsc->cert_passphrase(key);
    std::lock_guard lk(alive->m);
    auto* ed = static_cast<HttpEditor*>(alive->target);
    if (!ed) return;
    ed->CallAfter([ed, pass = std::move(pass), gen]() mutable {
      if (gen != ed->pass_gen_) return;
      ed->ov_.pass = std::move(pass);
      ed->push_opts();
    });
  }).detach();
}

void HttpEditor::save_passphrase(const std::string& pass) {
  const Workspace* ws = host_->workspace();
  if (!ws || base_.id.empty()) return;
  const uint64_t gen = ++pass_gen_;
  ov_.pass = pass;
  auto wsc = std::make_shared<Workspace>(*ws);
  const std::string key = Workspace::passphrase_key(SettingsOrigin{}, base_.id);
  static std::mutex mu;  // one writer at a time; a newer value is never overwritten by an older one
  static uint64_t written = 0;
  std::thread([wsc, key, pass, gen] {
    std::lock_guard lk(mu);
    if (gen < written) return;
    written = gen;
    wsc->set_cert_passphrase(key, pass);
  }).detach();
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
  auth_btn_ = new FlatButton(p, kAuthLabels[0], FlatButton::Style::Outline, true);
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
  auth_btn_->SetToolTip("Authorization type: click or type to pick (\"bear\" Enter: Bearer token); Up / Down change it");
  auth_btn_->on_click = [this] { show_auth_menu(); };
  auth_btn_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
    const int k = e.GetKeyCode();
    if (auth_pick_) {
      if (e.ControlDown() || e.AltDown()) {
        auth_pick_->close();
        e.Skip();
        return;
      }
      if (!auth_pick_->key(k)) e.Skip();  // characters arrive as wxEVT_CHAR
      return;
    }
    const int n = static_cast<int>(std::size(kAuthOrder));
    const bool up = k == WXK_UP || k == WXK_NUMPAD_UP, down = k == WXK_DOWN || k == WXK_NUMPAD_DOWN;
    if ((up || down) && !e.HasAnyModifiers()) {
      int at = 0;
      for (int i = 0; i < n; ++i)
        if (kAuthOrder[i] == auth_mode_) at = i;
      set_auth_mode(kAuthOrder[(at + (down ? 1 : n - 1)) % n], true);
      return;
    }
    if ((k == WXK_RETURN || k == WXK_NUMPAD_ENTER || k == WXK_SPACE) && !e.HasAnyModifiers()) {
      show_auth_menu();
      return;
    }
    e.Skip();
  });
  auth_btn_->Bind(wxEVT_CHAR, [this](wxKeyEvent& e) {
    const wxChar c = e.GetUnicodeKey();
    if (e.ControlDown() || e.AltDown() || c == WXK_NONE || c < 32) {
      e.Skip();
      return;
    }
    if (!auth_pick_) show_auth_menu();  // typing on the closed button opens the list filtered
    if (auth_pick_) auth_pick_->chr(c);
  });
  auth_btn_->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) {
    e.Skip();
    CallAfter([this] {
      if (!auth_pick_) return;
      if (auth_pick_->GetScreenRect().Contains(wxGetMousePosition()) && wxGetMouseState().LeftIsDown()) return;
      if (wxWindow::FindFocus() != auth_btn_) auth_pick_->close();
    });
  });
  top->Add(auth_btn_, 0, wxALIGN_CENTER_VERTICAL);
  s->Add(top, 0, wxTOP | wxBOTTOM, FromDIP(10));

  auth_book_ = new wxSimplebook(p);
  auth_book_->SetBackgroundColour(t.bg);
  // Empty pages: build_auth_subpage() fills the one shown.
  for (const char* n : {"none", "bearer", "basic", "apikey", "oauth2", "challenge", "awsv4", "inherit"}) {
    auto* pg = new wxPanel(auth_book_);
    pg->SetBackgroundColour(t.bg);
    pg->SetSizer(new wxBoxSizer(wxVERTICAL));
    auth_book_->AddPage(pg, n);
  }
  s->Add(auth_book_, 1, wxEXPAND);
  p->SetSizer(s);
  // The hints wrap to the page width (never cut).
  p->Bind(wxEVT_SIZE, [this, p](wxSizeEvent& e) {
    e.Skip();
    const int w = p->GetClientSize().x - FromDIP(8);
    if (w < FromDIP(200) || w == auth_wrap_) return;
    auth_wrap_ = w;
    CallAfter([this, w] {
      for (wxStaticText* h : auth_hints_) {
        if (h == s_hint_) continue;  // set_scheme_hint() wraps its own text
        wxString raw = h->GetLabel();
        raw.Replace("\n", " ");
        h->SetLabel(raw);
        h->Wrap(w);
      }
      if (s_hint_) {
        s_hint_->SetLabel("");
        set_scheme_hint();
      }
      if (o_status_) {
        o_status_->SetLabel("");
        update_oauth_status();
      }
      for (size_t i = 0; i < auth_book_->GetPageCount(); ++i) auth_book_->GetPage(i)->Layout();
    });
  });
  return p;
}

void HttpEditor::build_auth_subpage(size_t index) {
  if (!auth_book_ || index >= auth_built_.size() || auth_built_[index]) return;
  auth_built_[index] = true;
  const Theme& t = Theme::get();
  auto* holder = static_cast<wxPanel*>(auth_book_->GetPage(index));
  wxWindowUpdateLocker lock(holder);
  const size_t hints_before = auth_hints_.size();
  auto page = [&](const char* hint) {
    wxPanel* pg = holder;
    auto* grid = new wxFlexGridSizer(2, FromDIP(8), FromDIP(10));
    grid->AddGrowableCol(1, 1);
    auto* h = new wxStaticText(pg, wxID_ANY, hint);
    h->SetForegroundColour(t.dim);
    auth_hints_.push_back(h);
    pg->GetSizer()->Add(h, 0, wxBOTTOM, FromDIP(10));
    pg->GetSizer()->Add(grid, 0, wxEXPAND);
    return std::make_pair(pg, grid);
  };
  auto var_field = [this](wxPanel* pg, wxFlexGridSizer* grid, const char* label, const char* hint, std::string* val) {
    const Theme& t = Theme::get();
    auto* lb = new wxStaticText(pg, wxID_ANY, label);
    lb->SetForegroundColour(t.fg);
    wxWindow* frame = nullptr;
    wxStyledTextCtrl* f = make_var_field(pg, hint, val, &frame);
    grid->Add(lb, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(frame, 1, wxEXPAND);
    return f;
  };
  auto secret_field = [this](wxPanel* pg, wxFlexGridSizer* grid, const char* label, const char* hint, std::string* val) {
    const Theme& t = Theme::get();
    auto* lb = new wxStaticText(pg, wxID_ANY, label);
    lb->SetForegroundColour(t.fg);
    wxWindow* frame = nullptr;
    wxTextCtrl* tc = make_secret_box(pg, hint, &frame);
    grid->Add(lb, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(frame, 1, wxEXPAND);
    tc->Bind(wxEVT_TEXT, [this, tc, val](wxCommandEvent&) {
      if (loading_) return;
      *val = tc->GetValue().utf8_string();
      o_last_error_.clear();
      touched();
    });
    return tc;
  };
  auto choice = [this](wxPanel* pg, wxFlexGridSizer* grid, const char* label, std::vector<wxString> items) {
    const Theme& t = Theme::get();
    auto* lb = new wxStaticText(pg, wxID_ANY, label);
    lb->SetForegroundColour(t.fg);
    auto* c = new ChoiceButton(pg, std::move(items));
    grid->Add(lb, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(c, 0);
    return c;
  };
  switch (index) {
    case 0: {
      auto [pg, grid] = page("This request uses no auth scheme: a folder's or the collection's auth (Basic, "
                             "OAuth 2.0, Digest, ...) is not applied.");
      (void)grid;
      none_inh_ = new wxStaticText(pg, wxID_ANY, "");
      none_inh_->SetForegroundColour(t.fg);
      pg->GetSizer()->Add(none_inh_, 0, wxBOTTOM, FromDIP(8));
      auto* links = new wxBoxSizer(wxHORIZONTAL);
      none_headers_ = new FlatButton(pg, "Show in Headers", FlatButton::Style::Outline);
      none_headers_->SetToolTip("The Headers tab's Auto headers list every inherited header and where it comes from");
      none_headers_->on_click = [this] {
        tabs_->select(strip_of(kTabHeaders));
        if (tabs_->on_select) tabs_->on_select(strip_of(kTabHeaders));
      };
      links->Add(none_headers_, 0, wxRIGHT, FromDIP(8));
      none_folder_ = new FlatButton(pg, "Edit on folder", FlatButton::Style::Outline);
      none_folder_->on_click = [this] {
        if (!none_folder_path_.empty()) edit_folder(none_folder_path_);
      };
      links->Add(none_folder_, 0);
      pg->GetSizer()->Add(links, 0, wxBOTTOM, FromDIP(8));
      show_none_note();
      break;
    }
    case 7: {
      auto [pg, grid] = page("Uses the auth of the nearest folder above this request that sets one, else the "
                             "collection's. Pick another type to override it here.");
      (void)grid;
      inh_title_ = new wxStaticText(pg, wxID_ANY, "");
      inh_title_->SetForegroundColour(t.fg);
      inh_title_->SetFont(inh_title_->GetFont().Bold());
      pg->GetSizer()->Add(inh_title_, 0, wxBOTTOM, FromDIP(4));
      inh_detail_ = new wxStaticText(pg, wxID_ANY, "");
      inh_detail_->SetForegroundColour(t.dim);
      inh_detail_->SetFont(mono_font(9));
      pg->GetSizer()->Add(inh_detail_, 0, wxBOTTOM, FromDIP(10));
      auto* links = new wxBoxSizer(wxHORIZONTAL);
      inh_edit_ = new FlatButton(pg, "Edit on folder", FlatButton::Style::Outline);
      inh_edit_->on_click = [this] {
        edit_folder(!inh_.source_folder.empty() ? inh_.source_folder : own_folder());
      };
      links->Add(inh_edit_, 0, wxRIGHT, FromDIP(8));
      inh_own_ = new FlatButton(pg, "This folder's settings", FlatButton::Style::Outline);
      inh_own_->SetToolTip("Auth, headers, variables and scripts of the folder this request is in");
      inh_own_->on_click = [this] { edit_folder(own_folder()); };
      links->Add(inh_own_, 0, wxRIGHT, FromDIP(8));
      inh_coll_ = new FlatButton(pg, "Collection settings", FlatButton::Style::Outline);
      inh_coll_->SetToolTip("Auth, headers, variables and scripts of the whole workspace (folder.json at its root)");
      inh_coll_->on_click = [this] {
        if (const Workspace* ws = host_->workspace()) edit_folder(ws->root());
      };
      links->Add(inh_coll_, 0);
      pg->GetSizer()->Add(links, 0, wxBOTTOM, FromDIP(8));
      inh_errors_ = new wxStaticText(pg, wxID_ANY, "");
      inh_errors_->SetForegroundColour(t.js_error);
      inh_errors_->Hide();
      pg->GetSizer()->Add(inh_errors_, 0, wxBOTTOM, FromDIP(8));
      show_inherited();
      break;
    }
    case 1: {
      auto [pg, grid] = page("Sent as the header  Authorization: Bearer <token>.  {{variables}} are allowed.");
      bearer_ = var_field(pg, grid, "Token", "{{token}}", &av_.bearer);
      break;
    }
    case 2: {
      auto [pg, grid] = page("Sent as  Authorization: Basic base64(user:password).  It replaces an Authorization header inherited from a "
                            "folder or the collection; one in this request's Headers table replaces it.");
      basic_user_ = var_field(pg, grid, "Username", "user", &av_.user);
      {
        auto* lb = new wxStaticText(pg, wxID_ANY, "Password");
        lb->SetForegroundColour(t.fg);
        grid->Add(lb, 0, wxALIGN_CENTER_VERTICAL);
        // Show / Hide: wxTE_PASSWORD cannot change on a live control, so a plain twin takes its
        // place in the same frame.
        wxWindow* frame = nullptr;
        basic_pass_ = make_secret_box(pg, "password or {{secret}}", &frame);
        basic_pass_alt_ = new wxTextCtrl(frame, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        basic_pass_alt_->SetBackgroundColour(t.field);
        basic_pass_alt_->SetForegroundColour(t.fg);
        basic_pass_alt_->SetHint("password or {{secret}}");
        basic_pass_alt_->Hide();
        frame->GetSizer()->Add(basic_pass_alt_, 1, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));
        for (wxTextCtrl* tc : {basic_pass_, basic_pass_alt_}) {
          tc->Bind(wxEVT_TEXT, [this, tc](wxCommandEvent&) {
            if (loading_ || tc != basic_pass_) return;
            av_.pass = tc->GetValue().utf8_string();
            touched();
          });
        }
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        auto* eye = new FlatButton(pg, "Hide", FlatButton::Style::Subtle);
        eye->SetMinSize(eye->GetBestSize());
        eye->SetLabel("Show");
        eye->SetToolTip("Show / hide the password");
        row->Add(frame, 1, wxALIGN_CENTER_VERTICAL);
        row->Add(eye, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
        grid->Add(row, 1, wxEXPAND);
        eye->on_click = [this, eye, frame] {
          const bool was = loading_;
          loading_ = true;
          basic_pass_alt_->ChangeValue(basic_pass_->GetValue());
          loading_ = was;
          const bool focus = wxWindow::FindFocus() == basic_pass_;
          basic_pass_->Hide();
          basic_pass_alt_->Show();
          std::swap(basic_pass_, basic_pass_alt_);
          eye->SetLabel(basic_pass_->HasFlag(wxTE_PASSWORD) ? "Show" : "Hide");
          frame->Layout();
          if (focus) basic_pass_->SetFocus();
        };
      }
      break;
    }
    case 3: {
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
      break;
    }
    case 4: {
      // 13 fields cost ~60 ms to create: built in steps of at most ~20 ms (run_auth_steps), the
      // page frozen until the last one, so no UI-thread turn is long.
      OAuth2Config* o = &av_.adv.oauth2;
      auto [pg0, grid0] = page("OAuth 2.0: the access token is fetched off the UI thread, cached in memory per workspace "
                               "and environment (never written to disk), refreshed when it expires, and sent as "
                               "Authorization: Bearer <token>. Put secrets in {{secret}} environment variables.");
      wxPanel* pg = pg0;
      wxFlexGridSizer* grid = grid0;
      holder->Freeze();  // thawed by the last step
      // the label and the field just added (rows shown only for some grants / placements)
      auto last_row = [grid](std::vector<wxWindow*>& v) {
        const size_t n = grid->GetItemCount();
        v.push_back(grid->GetItem(n - 2)->GetWindow());
        v.push_back(grid->GetItem(n - 1)->GetWindow());
      };
      auto& st = auth_steps_;
      st.push_back([=, this] { o_grant_ = choice(pg, grid, "Grant", {"Client credentials", "Password", "Authorization code (browser, PKCE)"}); });
      st.push_back([=, this] { o_token_url_ = var_field(pg, grid, "Token URL", "https://auth.example.com/oauth/token", &o->token_url); });
      st.push_back([=, this] {
        o_auth_url_ = var_field(pg, grid, "Auth URL", "https://auth.example.com/authorize", &o->auth_url);
        last_row(o_code_rows_);
      });
      st.push_back([=, this] { o_client_id_ = var_field(pg, grid, "Client ID", "{{client_id}}", &o->client_id); });
      st.push_back([=, this] { o_secret_ = secret_field(pg, grid, "Client secret", "{{client_secret}}", &o->client_secret); });
      st.push_back([=, this] {
        o_user_ = var_field(pg, grid, "Username", "{{username}}", &o->username);
        last_row(o_password_rows_);
      });
      st.push_back([=, this] {
        o_pass_ = secret_field(pg, grid, "Password", "{{password}}", &o->password);
        last_row(o_password_rows_);
      });
      st.push_back([=, this] { o_scope_ = var_field(pg, grid, "Scope", "read write (optional)", &o->scope); });
      st.push_back([=, this] { o_audience_ = var_field(pg, grid, "Audience", "optional (Auth0 and others)", &o->audience); });
      st.push_back([=, this] {
        o_client_auth_ = choice(pg, grid, "Client auth", {"Basic auth header", "In the request body"});
        o_add_to_ = choice(pg, grid, "Add token to", {"Header", "Query param"});
      });
      st.push_back([=, this] {
        o_prefix_ = var_field(pg, grid, "Header prefix", "Bearer", &o->header_prefix);
        last_row(o_header_rows_);
      });
      st.push_back([=, this] {
        o_param_ = var_field(pg, grid, "Query param", "access_token", &o->query_param);
        last_row(o_query_rows_);
      });
      st.push_back([=, this] {
        const Theme& th = Theme::get();
        grid->Add(new wxPanel(pg), 0);
        o_refresh_ = new CheckButton(pg, "Refresh with the refresh token when the access token expired");
        grid->Add(o_refresh_, 0);
        o_grant_->on_select = [this](int i) {
          av_.adv.oauth2.grant = static_cast<OAuth2Config::Grant>(std::clamp(i, 0, 2));
          o_last_error_.clear();
          update_oauth_fields();
          update_oauth_status();
          touched();
        };
        o_client_auth_->on_select = [this](int i) {
          av_.adv.oauth2.client_auth = i == 1 ? OAuth2Config::ClientAuth::Body : OAuth2Config::ClientAuth::BasicHeader;
          touched();
        };
        o_add_to_->on_select = [this](int i) {
          av_.adv.oauth2.add_to = i == 1 ? OAuth2Config::AddTo::Query : OAuth2Config::AddTo::Header;
          update_oauth_fields();
          touched();
        };
        o_refresh_->on_change = [this] {
          av_.adv.oauth2.auto_refresh = o_refresh_->value();
          touched();
        };
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        o_get_ = new FlatButton(pg, "Get new token", FlatButton::Style::Primary);
        o_get_->SetToolTip("Fetch a new access token now (a worker thread; the authorization code grant opens your browser)");
        o_get_->on_click = [this] { get_oauth_token(); };
        o_clear_ = new FlatButton(pg, "Clear token", FlatButton::Style::Subtle);
        o_clear_->SetToolTip("Forget the cached token of this request");
        o_clear_->on_click = [this] {
          if (const Workspace* ws = host_->workspace()) {
            SavedRequest r = base_;
            r.auth = av_.adv;
            r.auth.type = HttpAuth::Type::OAuth2;
            oauth2_forget(*ws, r);
          }
          o_last_error_.clear();
          update_oauth_status();
        };
        row->Add(o_get_, 0, wxALIGN_CENTER_VERTICAL);
        row->Add(o_clear_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
        o_status_ = new wxStaticText(pg, wxID_ANY, "");
        o_status_->SetForegroundColour(th.dim);
        pg->GetSizer()->Add(row, 0, wxTOP, FromDIP(12));
        pg->GetSizer()->Add(o_status_, 0, wxEXPAND | wxTOP, FromDIP(6));
        o_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
          if (auth_mode_ == AuthMode::OAuth2 && book_->GetSelection() == kTabAuth && IsShownOnScreen()) update_oauth_status();
        });
        o_timer_.Start(15000);
        o_page_ = pg;
        o_ready_ = true;
        push_auth();  // the values, the grant's rows and the token status
        pg->Layout();
        if (pg->IsFrozen()) pg->Thaw();
      });
      run_auth_steps();
      break;
    }
    case 5: {
      auto [pg, grid] = page("Digest");
      s_hint_ = static_cast<wxStaticText*>(pg->GetSizer()->GetItem(size_t{0})->GetWindow());
      s_user_ = var_field(pg, grid, "Username", "user or DOMAIN\\user", &av_.adv.user);
      s_pass_ = secret_field(pg, grid, "Password", "password or {{secret}}", &av_.adv.password);
      break;
    }
    case 6: {
      auto [pg, grid] = page("AWS Signature Version 4: libcurl signs each request (curl --aws-sigv4). A session token is "
                             "sent (and signed) as X-Amz-Security-Token.");
      s_aws_key_ = var_field(pg, grid, "Access key ID", "{{aws_access_key_id}}", &av_.adv.aws_access_key);
      s_aws_secret_ = secret_field(pg, grid, "Secret key", "{{aws_secret_access_key}}", &av_.adv.aws_secret_key);
      s_aws_token_ = var_field(pg, grid, "Session token", "optional: {{aws_session_token}}", &av_.adv.aws_session_token);
      s_aws_region_ = var_field(pg, grid, "Region", "eu-west-1", &av_.adv.aws_region);
      s_aws_service_ = var_field(pg, grid, "Service", "execute-api", &av_.adv.aws_service);
      break;
    }
    default: break;
  }
  if (auth_wrap_ > 0)
    for (size_t i = hints_before; i < auth_hints_.size(); ++i)
      if (auth_hints_[i] != s_hint_) auth_hints_[i]->Wrap(auth_wrap_);
  holder->Layout();
}

void HttpEditor::run_auth_steps() {
  const auto t0 = Clock::now();
  size_t i = 0;
  while (i < auth_steps_.size() && (i == 0 || ms_between(t0, Clock::now()) < 18.0)) {
    auto step = std::move(auth_steps_[i]);
    step();
    ++i;
  }
  auth_steps_.erase(auth_steps_.begin(), auth_steps_.begin() + static_cast<std::ptrdiff_t>(i));
  if (!auth_steps_.empty()) CallAfter([this] { run_auth_steps(); });
}

// Only the fields the chosen grant uses (Postman / Insomnia): the client credentials grant has
// no auth URL, user or password; the token goes either in a header (prefix) or in a query param.
void HttpEditor::update_oauth_fields() {
  if (!o_page_) return;
  const OAuth2Config& o = av_.adv.oauth2;
  bool changed = false;
  auto show = [&](const std::vector<wxWindow*>& rows, bool on) {
    for (wxWindow* w : rows)
      if (w && w->IsShown() != on) {
        w->Show(on);
        changed = true;
      }
  };
  show(o_code_rows_, o.grant == OAuth2Config::Grant::AuthorizationCode);
  show(o_password_rows_, o.grant == OAuth2Config::Grant::Password);
  show(o_header_rows_, o.add_to != OAuth2Config::AddTo::Query);
  show(o_query_rows_, o.add_to == OAuth2Config::AddTo::Query);
  if (changed) o_page_->Layout();
}

wxTextCtrl* HttpEditor::make_secret_box(wxWindow* parent, const char* hint, wxWindow** frame_out, long style) {
  const Theme& t = Theme::get();
  auto* frame = new FramePanel(parent, t.field, t.border);
  auto* fs = new wxBoxSizer(wxHORIZONTAL);
  auto* tc = new wxTextCtrl(frame, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(380), -1), style | wxBORDER_NONE);
  tc->SetBackgroundColour(t.field);
  tc->SetForegroundColour(t.fg);
  tc->SetHint(hint);
  fs->Add(tc, 1, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));
  frame->SetSizer(fs);
  tc->Bind(wxEVT_SET_FOCUS, [frame](wxFocusEvent& e) {
    e.Skip();
    frame->set_focus(true);
  });
  tc->Bind(wxEVT_KILL_FOCUS, [frame](wxFocusEvent& e) {
    e.Skip();
    frame->set_focus(false);
  });
  *frame_out = frame;
  return tc;
}

wxStyledTextCtrl* HttpEditor::make_var_field(wxWindow* parent, const char* hint, std::string* val, wxWindow** frame_out) {
  const Theme& t = Theme::get();
  auto* frame = new FramePanel(parent, t.field, t.border);
  auto* fs = new wxBoxSizer(wxHORIZONTAL);
  auto* s = new wxStyledTextCtrl(frame, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxWANTS_CHARS);
  var_fields_.emplace_back(s);
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
    else var_narrow(s);
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

// CA / client certificate / proxy of this request, over its folders' (Certificates & proxy):
// built with the Options page when the user opens it, or in its own idle step after the page.
void HttpEditor::build_net_panel(bool step) {
  if (net_panel_ || !opts_sizer_) return;
  wxWindowUpdateLocker lock(opts_page_);
  if (!net_staged_) {
    const Workspace* ws = host_->workspace();
    net_staged_ = new NetworkPanel(opts_page_, ws ? ws->root() : std::filesystem::path(), false, step);
    opts_sizer_->Insert(net_at_, net_staged_, 0, wxEXPAND | wxTOP | wxBOTTOM | wxRIGHT, FromDIP(6));
    if (step && !net_staged_->complete()) {
      opts_page_->Layout();
      return;
    }
  }
  if (step) {
    if (!net_staged_->build_step()) {
      opts_page_->Layout();
      return;
    }
  } else {
    while (!net_staged_->build_step()) {
    }
  }
  net_panel_ = net_staged_;
  net_panel_->on_change = [this] {
    if (loading_) return;
    ov_.net = net_panel_->get();
    const std::string pass = ov_.net.cert_file.empty() ? std::string() : net_panel_->passphrase();
    if (pass != ov_.pass) save_passphrase(pass);
    update_preview();  // "Encode URL" changes the Sent-as line
    touched();
  };
  const bool was = loading_;  // its values (push_opts does the same once the page is built)
  loading_ = true;
  net_panel_->set(ov_.net, ov_.pass);
  net_panel_->set_inherited(wxString::FromUTF8(inh_.network));
  net_panel_->set_above(inh_.opts);
  net_panel_->set_follow(ov_.redirects);
  loading_ = was;
  opts_page_->Layout();
  opts_page_->FitInside();
}

wxWindow* HttpEditor::build_options_page() {
  const Theme& t = Theme::get();
  // Scrolls: with Certificates & proxy the page is taller than a small request pane.
  auto* p = new wxScrolled<wxPanel>(opts_holder_);
  p->SetScrollRate(0, FromDIP(12));
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
  version_->set_field_look(true);  // a field like Timeout above it, text on the same left edge
  version_->SetMinSize(wxSize(version_->GetBestSize().x, timeout_frame_->GetMinSize().y));
  grid->Add(version_, 0);
  s->Add(grid, 0, wxTOP | wxBOTTOM, FromDIP(12));
  redirects_ = new CheckButton(p, "Follow redirects");
  verify_tls_ = new CheckButton(p, "Verify TLS certificates");
  fresh_ = new CheckButton(p, "Fresh connection (no reuse: measures DNS + connect + TLS every time)");
  jar_opt_ = new CheckButton(p, "Use the cookie jar (send its cookies for this URL; off: only this request's own)");
  for (auto* c : {redirects_, verify_tls_, fresh_}) {
    s->Add(c, 0, wxBOTTOM, FromDIP(8));
    c->on_change = [this] {
      if (loading_) return;
      ov_.redirects = redirects_->value();
      ov_.verify_tls = verify_tls_->value();
      ov_.fresh = fresh_->value();
      if (net_panel_) net_panel_->set_follow(ov_.redirects);
      touched();
    };
  }
  s->Add(jar_opt_, 0, wxBOTTOM, FromDIP(8));
  jar_opt_->on_change = [this] {
    if (!loading_) set_use_jar(jar_opt_->value());
  };
  opts_sizer_ = s;
  opts_page_ = p;
  net_at_ = s->GetItemCount();
  if (!defer_net_) build_net_panel(false);
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
  auto* head = new wxBoxSizer(wxHORIZONTAL);
  auto* hint = new wxStaticText(p, wxID_ANY, "Notes about this request (Markdown), saved with it");
  hint->SetForegroundColour(t.dim);
  head->Add(hint, 1, wxALIGN_CENTER_VERTICAL);
  auto* mode = new RadioStrip(p, {"Edit", "Preview"});
  mode->SetToolTip("Edit the Markdown or preview it rendered (Ctrl+Shift+V)");
  head->Add(mode, 0, wxALIGN_CENTER_VERTICAL);
  s->Add(head, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(8));
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
  attach_find(d);
  fs->Add(d, 1, wxEXPAND | wxALL, FromDIP(6));
  frame->SetSizer(fs);
  s->Add(frame, 1, wxEXPAND | wxBOTTOM, FromDIP(4));
  p->SetSizer(s);
  // Preview: a wxHtmlWindow (no script engine; raw HTML in the docs is escaped, only http(s)
  // links open, in the system browser), built on first use.
  auto show_preview = [this, p, s, frame, mode](bool on, bool user) {
    const Theme& th = Theme::get();
    mode->select(on ? 1 : 0);
    if (on && !docs_html_) {
      docs_html_ = new wxHtmlWindow(p, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxHW_SCROLLBAR_AUTO | wxBORDER_NONE);
      docs_html_->SetBackgroundColour(th.field);
      docs_html_->Bind(wxEVT_HTML_LINK_CLICKED, [](wxHtmlLinkEvent& e) {
        const wxString href = e.GetLinkInfo().GetHref();
        if (href.Lower().StartsWith("https://") || href.Lower().StartsWith("http://")) wxLaunchDefaultBrowser(href);
      });
      s->Insert(2, docs_html_, 1, wxEXPAND | wxBOTTOM, FromDIP(4));
    }
    if (on) {
      auto hex = [](const wxColour& c) { return c.GetAsString(wxC2S_HTML_SYNTAX).utf8_string(); };
      std::string body = markdown_to_html(docs_text_);
      if (trim(docs_text_).empty()) body = "<p><font color=\"" + hex(th.dim) + "\">Nothing written yet.</font></p>";
      const std::string page = "<html><body bgcolor=\"" + hex(th.field) + "\" text=\"" + hex(th.fg) + "\" link=\"" +
                               hex(th.accent) + "\">" + body + "</body></html>";
      docs_html_->SetPage(wxString::FromUTF8(page));
    }
    frame->Show(!on);
    if (docs_html_) docs_html_->Show(on);
    p->Layout();
    if (!on && user) docs_->SetFocus();
  };
  docs_preview_ = show_preview;
  mode->on_select = [show_preview](int i) { show_preview(i == 1, true); };
  p->Bind(wxEVT_CHAR_HOOK, [mode, show_preview](wxKeyEvent& e) {
    if (e.GetModifiers() == (wxMOD_CONTROL | wxMOD_SHIFT) && (e.GetKeyCode() == 'V' || e.GetKeyCode() == 'v')) {
      show_preview(mode->selection() != 1, true);
      return;
    }
    e.Skip();
  });
  // A request whose notes are written opens on the rendered view, like Bruno's Docs tab.
  if (!trim(docs_text_).empty()) show_preview(true, false);
  return p;
}

// ------------------------------------------------------------------ load / store

void HttpEditor::load(const SavedRequest& r) {
  loading_ = true;
  hide_banner();
  base_ = r;
  base_.http.body.clear();
  base_.http.body.shrink_to_fit();

  // Row descriptions travel at the end of the docs (see split_row_docs).
  RowDocs row_docs;
  docs_text_ = split_row_docs(r.docs, row_docs);
  pre_vals_ = row_docs.pre;
  pre_preview_.clear();
  param_descs_.clear();
  path_descs_.clear();
  assert_descs_.clear();
  for (const auto& [k, d] : row_docs.asserts) assert_descs_.emplace(k, d);
  for (const auto& [k, d] : row_docs.params) {
    if (!k.empty() && k[0] == ':') path_descs_.emplace(k.substr(1), d);
    else param_descs_.emplace(k, d);
  }
  auto header_desc = [&](const std::string& name) {
    for (const auto& [k, d] : row_docs.headers)
      if (iequals(k, name)) return d;
    return std::string();
  };

  set_method(r.http.method.empty() ? "GET" : r.http.method);
  set_url_text(r.http.url);
  path_values_.clear();
  for (const auto& [k, v] : r.path_params) path_values_[k] = v;
  path_names_.clear();  // rebuilt with the descriptions just read
  sync_path_rows();
  update_preview();
  url_->GotoPos(0);

  std::vector<KvRow> prow;
  for (const auto& q : parse_query_decoded(split_url(r.http.url).query)) prow.push_back({q.key, q.value, true, false, q.bare});
  for (const auto& [k, v] : r.disabled_params) prow.push_back({k, v, false, false, false});
  for (auto& row : prow)
    if (const auto it = param_descs_.find(row.key); it != param_descs_.end()) row.desc = it->second;
  params_->set_rows(prow);
  if (path_panel_ && path_panel_->IsShown()) layout_params_page();

  // Headers (an auth header is shown on the Auth tab instead, and put back at its place).
  std::vector<KvRow> hrows;
  auth_header_index_ = SIZE_MAX;
  AuthMode am = AuthMode::None;
  std::string bearer, key_name, key_value;
  // An inheriting request's own Authorization / API key header stays a plain header row.
  const bool inherits = r.auth.type == HttpAuth::Type::Inherit;
  for (size_t i = 0; i < r.http.headers.size(); ++i) {
    const auto& [k, v] = r.http.headers[i];
    const bool enabled = !r.disabled_headers.count(i);
    if (!inherits && am == AuthMode::None && !r.basic_auth.enabled && enabled) {
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
    hrows.push_back({k, v, enabled, false, false, header_desc(k)});
  }
  set_header_rows(hrows);
  if (r.basic_auth.enabled) am = AuthMode::Basic;
  av_.adv = r.auth;
  if (r.auth.type != HttpAuth::Type::None) am = mode_of(r.auth.type);  // replaces basic_auth
  // A new request (nothing in it yet) starts on Inherit from parent, as in Postman / Insomnia /
  // Bruno: one folder or collection auth covers it.
  if (r.kind == RequestKind::Http && r.file.empty() && r.id.empty() && r.http.url.empty() && r.http.headers.empty() &&
      r.http.body.empty() && !r.basic_auth.enabled && r.auth.type == HttpAuth::Type::None && r.form.empty())
    am = AuthMode::Inherit;
  if (am == AuthMode::Inherit) av_.adv.type = HttpAuth::Type::None;  // the scheme pages start empty
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
  refresh_inherited();

  update_files_row();

  // Body.
  std::string ct;
  for (size_t i = 0; i < r.http.headers.size(); ++i)
    if (iequals(r.http.headers[i].first, "Content-Type") && !r.disabled_headers.count(i)) ct = r.http.headers[i].second;
  BodyMode bm;
  if (!r.form.empty()) bm = BodyMode::Multipart;
  else if (!r.body_file.empty()) bm = BodyMode::Binary;
  else if (!r.body_data.empty()) bm = BodyMode::CurlData;
  // A form Content-Type over a JSON / XML body (curl -d '{"k":1}' without -H sends exactly that)
  // is shown as the raw body it is, with the header kept so the bytes sent stay curl's, as
  // Postman and Insomnia import it, not as one form row named {"k":1}.
  else if (ct.find("x-www-form-urlencoded") != std::string::npos && !looks_like_json(r.http.body) &&
           !looks_like_xml(r.http.body))
    bm = BodyMode::UrlEncoded;
  else if (r.http.body.empty()) bm = BodyMode::None;
  else if (text_kind_of(ct) > 0) bm = BodyMode::Text;  // HTML, JavaScript, YAML, ... as the header says
  else if (ct.find("json") != std::string::npos || looks_like_json(r.http.body)) bm = BodyMode::Json;
  else if (ct.find("xml") != std::string::npos || looks_like_xml(r.http.body)) bm = BodyMode::Xml;
  else bm = BodyMode::Text;

  // {"query": "...", "variables": {...}}: the GraphQL editors (a body over 1 MB stays JSON).
  gql_q_val_.clear();
  gql_v_val_.clear();
  gql_op_.clear();
  if (bm == BodyMode::Json && r.http.body.size() < 1024 * 1024 &&
      split_graphql_body(r.http.body, gql_q_val_, gql_v_val_, &gql_op_))
    bm = BodyMode::GraphQL;
  if (gql_query_) {
    const bool was = loading_;
    loading_ = true;
    gql_query_->SetText(wxString::FromUTF8(gql_q_val_));
    gql_vars_->SetText(wxString::FromUTF8(gql_v_val_));
    gql_query_->EmptyUndoBuffer();
    gql_vars_->EmptyUndoBuffer();
    loading_ = was;
    lint_gql();
    lint_gql_vars();
    gql_timer_.StartOnce(50);  // another URL: its schema (memory, then the .watt copy)
  }

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
  end_fill(false);
  pending_body_.reset();
  minify_->set_value(false);
  raw_body_.reset();
  raw_pending_.reset();
  raw_on_load_.reset();
  if (prev_doc_) {
    release_doc_async(prev_doc_);
    prev_doc_ = nullptr;
  }
  drop_redo_doc();
  body_->SetReadOnly(false);
  body_->ClearAll();
  body_->SetScrollWidth(1);  // tracking only grows it: a wide body before must not leave a scroll bar
  if (text_mode(bm) && !r.http.body.empty()) {
    if (r.http.body.size() < kAsyncBody) {
      body_->AddTextRaw(r.http.body.data(), static_cast<int>(r.http.body.size()));
    } else {
      // Read-only and empty for the few ms the worker needs; store() returns pending_body_.
      pending_body_ = std::make_shared<const std::string>(r.http.body);
      body_->SetReadOnly(true);
      // A one-line JSON body would be laid out as one 10 MB line (about a second per paint and
      // per keystroke): it is shown formatted instead, like a paste.
      if (bm == BodyMode::Json && longest_line(*pending_body_) >= kLongLine && looks_like_json(*pending_body_))
        load_formatted(pending_body_, body_load_gen_);
      else
        build_body_doc(pending_body_, false, body_load_gen_, 0);
    }
  }
  body_->EmptyUndoBuffer();
  body_->SetSavePoint();
  body_->GotoPos(0);
  body_hl_first_ = -1;
  set_body_mode(bm, false);
  if (bm == BodyMode::Json && r.http.body.size() >= kLongLine && !raw_on_load_) {
    const size_t longest = longest_line(r.http.body);
    if (longest >= kLongLine)
      show_info(wxString::Format("The body has a %s line", human_size(longest)),
                "Editing a line this long is slow: every keystroke lays the whole line out again. Beautify "
                "re-indents the JSON in a few ms on a worker thread (whitespace only, values kept as written).",
                "Beautify", [this] {
                  hide_banner();
                  beautify();
                });
    long_line_banner_ = true;
  }

  // Tests.
  assert_vals_ = r.asserts;
  capture_vals_ = r.captures;
  asserts_stored_ = r.asserts;
  asserts_target_ = nullptr;
  if (asserts_) {
    std::vector<KvRow> cr;
    for (const auto& c : r.captures) cr.push_back({c.var, c.target, c.enabled, false, false, {}, capture_scope_name(c.scope)});
    set_assert_rows(r.asserts);
    if (captures_) captures_->set_rows(cr);
    if (post_js_) {
      post_js_->SetText(wxString::FromUTF8(r.post_script));
      post_js_->EmptyUndoBuffer();
    }
  }
  if (pre_) {
    std::vector<KvRow> pr;
    for (const auto& x : pre_vals_) pr.push_back({x.var, x.expr, x.enabled, false, false, {}, x.scope.empty() ? "runtime" : x.scope});
    pre_->set_rows(pr);
    if (pre_js_) {
      pre_js_->SetText(wxString::FromUTF8(r.pre_script));
      pre_js_->EmptyUndoBuffer();
    }
  }
  if (tests_page_) {
    update_tests_hint();
    layout_tests_page();
  }
  update_pre_names();
  if (book_->GetSelection() == kTabTests) pre_timer_.StartOnce(150);
  cookie_sig_.clear();

  // Docs (docs_text_ was set above, without the row descriptions).
  if (docs_) {
    docs_->SetText(wxString::FromUTF8(docs_text_));
    docs_->EmptyUndoBuffer();
    if (docs_preview_) docs_preview_(!trim(docs_text_).empty(), false);
  }

  // Options.
  ov_.timeout_ms = std::clamp<long>(r.http.timeout_ms, 0, 24L * 3600 * 1000);
  ov_.redirects = r.http.follow_redirects;
  ov_.verify_tls = r.http.verify_tls;
  ov_.fresh = r.http.fresh_connection;
  ov_.jar = !r.no_cookie_jar;
  ov_.version = static_cast<int>(r.http.version);
  ov_.net = r.network;
  ov_.pass.clear();
  load_passphrase();
  push_opts();
  refresh_jar();

  // Show the body of a request that has one (unless the user is on another tab than Params).
  if (first_load_ || tabs_->selection() == kTabParams || tabs_->selection() == kTabBody) {
    first_load_ = false;
    const size_t tab = (bm != BodyMode::None && prow.empty()) ? kTabBody
                       : (tabs_->selection() == kTabBody && bm != BodyMode::None ? kTabBody : kTabParams);
    tabs_->select(strip_of(tab));
    book_->ChangeSelection(tab);
    sync_body_book();
  }
  update_badges();
  if (book_->GetSelection() == kTabCookies) update_cookies();
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
  // A space or other character libcurl rejects, typed straight into the URL, is sent encoded
  // (with "Encode URL" off only what no request line can carry: spaces and control characters).
  r.http.url = encode_url_now() ? normalize_url(url_text()) : minimal_url(url_text());
}

void HttpEditor::store(SavedRequest& r) const {
  store_head(r);
  {
    RowDocs d;
    for (const auto& row : params_->rows())
      if (!row.desc.empty() && !trim(row.key).empty()) d.params.emplace_back(row.key, row.desc);
    for (const auto& n : path_names_)
      if (const auto it = path_descs_.find(n); it != path_descs_.end() && !it->second.empty())
        d.params.emplace_back(":" + n, it->second);
    for (const auto& row : header_rows())
      if (!row.desc.empty() && !clean_header_name(row.key).empty()) d.headers.emplace_back(clean_header_name(row.key), row.desc);
    d.pre = pre_now();
    if (asserts_) {
      assert_descs_.clear();
      for (const auto& row : asserts_->rows())
        if (!row.desc.empty() && !trim(row.key).empty()) assert_descs_[std::string(trim(row.key))] = row.desc;
    }
    const std::vector<Assertion> live = asserts_now();
    for (const auto& [k, v] : assert_descs_)
      if (std::any_of(live.begin(), live.end(), [&](const Assertion& a) { return a.target == k; })) d.asserts.emplace_back(k, v);
    r.docs = join_row_docs(docs_text_, d);
  }
  {
    std::vector<Assertion> now = asserts_now();
    if (&r == asserts_target_ && r.asserts != asserts_stored_ && now == asserts_stored_) {
      // Edited elsewhere (the response's Tests tab) since the last store, not here: adopt it.
      auto* self = const_cast<HttpEditor*>(this);
      self->assert_vals_ = r.asserts;
      self->set_assert_rows(r.asserts);
      self->update_badges();
    } else {
      r.asserts = std::move(now);
    }
    asserts_stored_ = r.asserts;
    asserts_target_ = &r;
  }
  r.captures = captures_now();
  // Saved with LF line ends (the editors use LF; a script loaded with CRLF keeps none either).
  auto lf = [](wxStyledTextCtrl* s) {
    std::string out(s->GetCharacterPointer(), static_cast<size_t>(s->GetLength()));
    out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
    return out;
  };
  if (pre_js_) r.pre_script = lf(pre_js_);
  if (post_js_) r.post_script = lf(post_js_);
  r.path_params.clear();
  for (const auto& n : path_names_) {
    const auto it = path_values_.find(n);
    r.path_params.emplace_back(n, it == path_values_.end() ? std::string() : it->second);
  }
  if (sending_) r.http.url = with_path(r.http.url);
  const Workspace* send_ws = sending_ ? host_->workspace() : nullptr;
  // While inh_ is being re-read (a folder.json just changed), its folder variables may be stale:
  // the core expands the URL from the files itself then.
  if (send_ws && inh_done_gen_ == inh_gen_ && r.http.url.find("{{") != std::string::npos) {
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
  r.auth = {};
  if (scheme_mode(auth_mode_)) {
    r.auth = av_.adv;
    r.auth.type = scheme_type(auth_mode_);
  } else if (auth_mode_ == AuthMode::Inherit) {
    r.auth.type = HttpAuth::Type::Inherit;
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
      if (store_no_body_) break;
      if (pending_body_) {
        r.http.body = *pending_body_;
        break;
      }
      if (body_mode_ == BodyMode::Json && minify_->value()) {
        if (raw_body_ && raw_edits_ == body_edits_) r.http.body = *raw_body_;  // as pasted
        else if (minified_current()) r.http.body = *minified_;               // built on a worker
        else r.http.body = minify_json(std::string_view(body_->GetCharacterPointer(), static_cast<size_t>(body_->GetLength())));
        break;
      }
      const std::string_view shown(body_->GetCharacterPointer(), static_cast<size_t>(body_->GetLength()));
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
    case BodyMode::GraphQL:
      r.http.body = graphql_body(gql_query_text(), gql_vars_text(), gql_op_);
      break;
  }
  r.header_files = base_.header_files;
  r.cookie_files = base_.cookie_files;

  r.http.timeout_ms = ov_.timeout_ms;
  r.http.follow_redirects = ov_.redirects;
  r.http.verify_tls = ov_.verify_tls;
  r.http.fresh_connection = ov_.fresh;
  r.no_cookie_jar = !ov_.jar;
  r.http.version = static_cast<HttpVersion>(ov_.version);
  r.network = ov_.net;
}

void HttpEditor::set_in_flight(bool f) {
  in_flight_ = f;
  // A send the shell dispatched (Ctrl+Enter from the sidebar, the Send menu): a "Not sent"
  // banner whose problems are all gone no longer describes this request.
  if (f) recheck_banner();
  if (!f && auth_mode_ == AuthMode::OAuth2) update_oauth_status();
  if (!f) jar_timer_.StartOnce(300);  // the response may have set cookies (recorded on a worker)
  // The trust question may have been answered meanwhile (the send starts / the response came).
  if (trust_state_ != 1 && has_scripts()) refresh_trust();
  send_->SetLabel(f ? "Cancel" : "Send");
  send_->SetToolTip(f ? "Cancel the request in flight" : "Send (Ctrl+Enter)");
}

void HttpEditor::focus_url() {
  url_->SetFocus();
  // Selected only when it really has the focus (a hidden tab does not take it): an unfocused
  // URL never shows a selection.
  if (wxWindow::FindFocus() == url_) url_->SelectAll();
}

// ------------------------------------------------------------------ behaviour

// The "Not sent" banner goes away once none of the problems it names is left (an edit, another
// environment, a send that now resolves).
void HttpEditor::recheck_banner() {
  if (banner_missing_.empty() || !banner_ || !banner_->IsShown()) return;
  auto now = unresolved_vars(nullptr);
  for (const auto& n : empty_path_vars()) now.push_back(":" + n);
  for (const auto& b : brace_problems()) now.push_back("{}" + b.first);
  for (const auto& r : header_rows())
    if (r.enabled && !header_name_problem(r.key).empty()) now.push_back("{}header name " + r.key);
  if (std::none_of(banner_missing_.begin(), banner_missing_.end(),
                   [&](const std::string& v) { return std::find(now.begin(), now.end(), v) != now.end(); }))
    hide_banner();
}

void HttpEditor::touched() {
  recheck_banner();
  update_badges();
  if (body_mode_ == BodyMode::Text) sync_text_kind();  // a Content-Type typed on the Headers tab
  if (!KvGrid::echo()) host_->changed();  // a cell's commit of text already reported: not a new edit
}

void HttpEditor::update_badges() {
  // Rows with a value but no key are not counted (they are not sent): the badge turns to the
  // warning colour instead.
  const size_t np = params_->enabled_count(), po = params_->orphan_count();
  tabs_->set_badge(strip_of(kTabParams), np || po ? wxString::Format("%zu", np) : wxString(), po > 0);
  tabs_->set_badge(strip_of(kTabBody), body_mode_ == BodyMode::None ? wxString() : wxString::FromUTF8("\xE2\x97\x8F"));
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
  // The rows of the table plus the folder / collection headers this request inherits (and does
  // not replace), like Postman's count with hidden headers. The Auth tab's header is listed
  // under Auto headers.
  if (!inh_.headers.empty()) {
    std::set<std::string> own;
    for (const auto& r : header_rows())
      if (r.enabled && !trim(r.key).empty()) own.insert(canonical_header_name(r.key));
    const std::string by_auth = auth_header_name();  // the Auth tab's header replaces an inherited one
    for (const auto& h : inh_.headers)
      nh += own.count(canonical_header_name(h.name)) == 0 && !(!by_auth.empty() && iequals(h.name, by_auth));
  }
  tabs_->set_badge(strip_of(kTabHeaders), nh || ho ? wxString::Format("%zu", nh) : wxString(), ho > 0);
  tabs_->set_badge(strip_of(kTabAuth), auth_mode_ == AuthMode::None || (auth_mode_ == AuthMode::Inherit && (!inh_.known || inh_.type == "No auth"))
                                 ? wxString()
                                 : wxString::FromUTF8("\xE2\x97\x8F"));
  {
    size_t npre = 0, npost = 0;
    if (asserts_) {
      npost = asserts_->enabled_count();
      if (captures_) npost += captures_->enabled_count();
      else
        for (const auto& c : capture_vals_) npost += c.enabled && !c.var.empty();
    } else {
      for (const auto& a : assert_vals_) npost += a.enabled && !a.target.empty();
      for (const auto& c : capture_vals_) npost += c.enabled && !c.var.empty();
    }
    if (pre_) {
      npre = pre_->enabled_count();
    } else {
      for (const auto& x : pre_vals_) npre += x.enabled && !x.var.empty();
    }
    const bool bad = tests_hint_ && !tests_hint_->GetLabel().empty();
    npost += inh_.asserts.size();  // the folders' checks run too (listed under the table)
    npre += pre_js_ ? pre_js_->GetLength() > 0 : !base_.pre_script.empty();
    npost += post_js_ ? post_js_->GetLength() > 0 : !base_.post_script.empty();
    tabs_->set_badge(kStripScripts, npre ? wxString::Format("%zu", npre) : wxString());
    tabs_->set_badge(kStripTests, npost || bad ? wxString::Format("%zu", npost) : wxString(), bad);
  }
  {
    size_t nc = base_.cookie_files.size();
    std::set<std::string> own;
    for (const auto& r : header_rows())
      if (r.enabled && iequals(trim(r.key), "Cookie"))
        for (auto& [n, v] : parse_cookie_header(r.value)) {
          own.insert(n);
          ++nc;
        }
    // jar cookies the request's own Cookie header does not already send (add_jar_cookies)
    if (ov_.jar && !jar_all_.empty())
      for (const auto& c : jar_matches()) nc += own.count(c.name) == 0;
    tabs_->set_badge(strip_of(kTabCookies), nc ? wxString::Format("%zu", nc) : wxString());
  }
  const bool opts = ov_.timeout_ms != 30000 || !ov_.redirects || !ov_.verify_tls || ov_.fresh || ov_.version != 0 ||
                    !ov_.net.empty() || !ov_.jar;
  tabs_->set_badge(strip_of(kTabOptions), opts ? wxString::FromUTF8("\xE2\x97\x8F") : wxString());
  tabs_->set_badge(strip_of(kTabDocs), docs_text_.empty() ? wxString() : wxString::FromUTF8("\xE2\x97\x8F"));
  if (book_->GetSelection() == kTabCookies) update_cookies();
  update_auto_headers();
}

// The header the Auth tab sends that beats one of the same name inherited from a folder or the
// collection (core resolve_with: the request's own Basic, Bearer and API key header); "" = none.
std::string HttpEditor::auth_header_name() const {
  if (auth_mode_ == AuthMode::Basic || (auth_mode_ == AuthMode::Bearer && !av_.bearer.empty())) return "Authorization";
  if (auth_mode_ == AuthMode::ApiKey && av_.key_where == 0) return clean_header_name(av_.key_name);
  // OAuth 2.0 in the header, Digest / NTLM / Negotiate / SigV4: their Authorization replaces an
  // inherited one (core resolve_with, auth_sets_authorization).
  if (auth_mode_ == AuthMode::OAuth2) return av_.adv.oauth2.add_to == OAuth2Config::AddTo::Header ? "Authorization" : "";
  if (scheme_mode(auth_mode_)) return "Authorization";
  return {};
}

// What HttpClient / libcurl add on their own (see src/core/http_client.cpp).
void HttpEditor::update_auto_headers(bool force) {
  if (!auto_btn_ || (!force && book_->GetSelection() != kTabHeaders)) return;  // only while the Headers tab is shown
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
  // The Auth tab's header (as written there; {{vars}} are expanded when sent).
  const char* kFromAuth = "   (Auth tab)";
  if (auth_mode_ == AuthMode::Basic) {
    rows.emplace_back("Authorization", std::string("Basic <user:password>") + kFromAuth);
  } else if (auth_mode_ == AuthMode::Bearer && !av_.bearer.empty()) {
    std::string tok = av_.bearer;
    // a literal token is shown cut: the list is on screen, the token is a secret
    if (tok.find("{{") == std::string::npos && tok.size() > 12) tok = tok.substr(0, 6) + "\xE2\x80\xA6" + tok.substr(tok.size() - 4);
    rows.emplace_back("Authorization", "Bearer " + tok + kFromAuth);
  } else if (auth_mode_ == AuthMode::ApiKey && av_.key_where == 0 && !av_.key_name.empty()) {
    rows.emplace_back(av_.key_name, av_.key_value + kFromAuth);
  } else if (auth_mode_ == AuthMode::OAuth2 && av_.adv.oauth2.add_to == OAuth2Config::AddTo::Header) {
    const std::string pre = av_.adv.oauth2.header_prefix.empty() ? std::string("Bearer") : av_.adv.oauth2.header_prefix;
    rows.emplace_back("Authorization", pre + " <OAuth 2.0 access token>" + kFromAuth);
  } else if (auth_mode_ == AuthMode::AwsSigV4) {
    rows.emplace_back("Authorization", std::string("AWS4-HMAC-SHA256 <signed by libcurl>") + kFromAuth);
    rows.emplace_back("X-Amz-Date", std::string("<when sent>") + kFromAuth);
  } else if (scheme_mode(auth_mode_)) {
    rows.emplace_back("Authorization", std::string(kAuthLabels[static_cast<int>(auth_mode_)]) +
                                           " <answers the 401 challenge>" + kFromAuth);
  } else if (auth_mode_ == AuthMode::Inherit && inh_.known && !inh_.source.empty()) {
    // The inherited auth's header (a Bearer / API key one is listed with the folder headers).
    const std::string from = "   (from " + inh_.source + ")";
    if (inh_.type == "Basic auth") rows.emplace_back("Authorization", "Basic <user:password>" + from);
    else if (inh_.type == "OAuth 2.0") rows.emplace_back("Authorization", "<OAuth 2.0 access token>" + from);
    else if (inh_.type == "AWS Signature v4") rows.emplace_back("Authorization", "AWS4-HMAC-SHA256 <signed by libcurl>" + from);
    else if (inh_.type != "No auth" && inh_.type != "Bearer token" && inh_.type != "API key")
      rows.emplace_back("Authorization", inh_.type + " <answers the 401 challenge>" + from);
  }
  // Folder / collection headers, with the folder they come from (a request header of the same
  // name replaces one). A folder's Bearer / API key header is its auth: shown masked.
  for (const auto& h : inh_.headers) {
    std::string v = !h.auth ? h.value
                    : iequals(h.name, "Authorization") ? "Bearer " + mask_secret(std::string_view(h.value).substr(7))
                                                       : mask_secret(h.value);
    rows.emplace_back(h.name, v + "   (from " + h.from + ")");
  }
  switch (body_mode_) {
    case BodyMode::None:
      break;
    case BodyMode::Multipart:
      rows.emplace_back("Content-Type", "multipart/form-data; boundary=<generated>");
      rows.emplace_back("Content-Length", "<computed when sent>");
      break;
    default:
      if (const auto n = sent_body_size()) rows.emplace_back("Content-Length", std::to_string(*n));
      else rows.emplace_back("Content-Length", "<computed when sent>");
      break;
  }
  std::vector<std::string> own;  // enabled names of the Headers table
  for (const auto& r : header_rows())
    if (r.enabled) own.push_back(r.key);
  // The header the Auth tab sends beats one of the same name inherited from a folder or the
  // collection (core resolve_with: the request's own Basic, Bearer and API key header).
  const std::vector<std::string> lost = auto_header_losers(rows, own, auth_header_name(), kFromAuth);
  // Counted: what is sent (a replaced row is listed, struck through, but not counted).
  const size_t sent = static_cast<size_t>(std::count(lost.begin(), lost.end(), std::string()));
  size_t inh_sent = 0;
  for (size_t i = 0; i < rows.size(); ++i)
    inh_sent += lost[i].empty() && rows[i].second.rfind("   (from ") != std::string::npos && rows[i].second.back() == ')';
  const wxString label = inh_.headers.empty() ? wxString::Format("Auto headers (%zu)", sent)
                                              : wxString::Format("Auto headers (%zu, %zu inherited)", sent, inh_sent);
  if (label != auto_btn_->GetLabel()) {
    auto_btn_->SetLabel(label);
    auto_btn_->InvalidateBestSize();
    auto_btn_->GetParent()->Layout();
  }
  if (!auto_panel_->IsShown()) return;
  auto sig = rows;
  for (size_t i = 0; i < sig.size(); ++i) sig[i].second += '\x01' + lost[i];
  if (sig == auto_shown_rows_) return;
  auto_shown_rows_ = std::move(sig);
  const Theme& t = Theme::get();
  auto_panel_->Freeze();
  auto_grid_->Clear(true);
  for (size_t i = 0; i < rows.size(); ++i) {
    const auto& [k, v] = rows[i];
    const bool over = !lost[i].empty();
    auto* kl = new wxStaticText(auto_panel_, wxID_ANY, wxString::FromUTF8(k));
    auto* vl = new wxStaticText(auto_panel_, wxID_ANY, wxString::FromUTF8(over ? v + "   " + lost[i] : v));
    kl->SetForegroundColour(over ? t.dim : t.fg);
    vl->SetForegroundColour(t.dim);
    if (over) {
      wxFont f = kl->GetFont();
      f.SetStrikethrough(true);
      kl->SetFont(f);
      const wxString tip = "Not sent: " + wxString::FromUTF8(lost[i].substr(1, lost[i].size() - 2));
      kl->SetToolTip(tip);
      vl->SetToolTip(tip);
    } else if (v.size() >= strlen(kFromAuth) && v.compare(v.size() - strlen(kFromAuth), std::string::npos, kFromAuth) == 0) {
      kl->SetToolTip("Set on the Auth tab");
      vl->SetToolTip("Set on the Auth tab");
    } else if (const size_t at = v.rfind("   (from "); at != std::string::npos && v.back() == ')') {
      // Inherited: the folder's settings open on a click.
      const std::string from = v.substr(at + 9, v.size() - at - 10);
      const wxString tip = "Inherited from " + wxString::FromUTF8(from) +
                           ". A header of the same name in the table replaces it for this request. Click to edit the "
                           "folder's settings.";
      kl->SetToolTip(tip);
      vl->SetToolTip(tip);
      vl->SetForegroundColour(t.var_ok);
      kl->SetCursor(wxCursor(wxCURSOR_HAND));
      vl->SetCursor(wxCursor(wxCURSOR_HAND));
      auto open = [this, from](wxMouseEvent&) {
        const Workspace* ws = host_->workspace();
        if (!ws) return;
        const std::u8string u8(reinterpret_cast<const char8_t*>(from.data()), from.size());
        edit_folder(from == "Collection" ? ws->root() : ws->root() / std::filesystem::path(u8));
      };
      kl->Bind(wxEVT_LEFT_UP, open);
      vl->Bind(wxEVT_LEFT_UP, open);
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

// A method picked by hand that usually carries no body while the request has one: say so (the
// body is still sent, as curl -X GET -d would).
void HttpEditor::note_body_method() {
  if ((method_str_ == "GET" || method_str_ == "HEAD") && body_mode_ != BodyMode::None && body_mode_ != BodyMode::CurlData)
    note(wxString::FromUTF8(method_str_ + " with a body: it is sent, but many servers and proxies ignore or refuse it"), true);
}

void HttpEditor::set_method(const std::string& m) {
  method_str_ = m;
  method_->SetLabel(wxString::FromUTF8(m));
  method_->set_text_colour(method_colour(m));
  url_box_->Layout();
}

void HttpEditor::show_method_menu() {
  if (method_pick_) {  // a second click / Alt+Down closes it, like a combo box
    method_pick_->close();
    return;
  }
  // A themed, keyboard-first list (not the native menu, where Down+Enter picked the first item):
  // the current method is selected, Up / Down move, Enter picks, typing filters, and a typed
  // method that is not in the list (PROPFIND, PURGE) is used as typed.
  static const char* const desc[] = {"read",          "create / submit", "replace", "modify part",
                                     "remove",        "headers only",    "allowed methods / CORS"};
  std::vector<PickItem> items;
  for (size_t i = 0; i < std::size(kMethods); ++i) items.push_back({kMethods[i], desc[i], ""});
  const wxString cur = wxString::FromUTF8(method_str_);
  if (std::none_of(std::begin(kMethods), std::end(kMethods), [&](const char* m) { return method_str_ == m; }))
    items.push_back({cur, "custom", ""});
  wxWindow* f = wxWindow::FindFocus();
  if (f != url_ && f != method_) method_->SetFocus();  // the window that forwards the keys
  auto* p = new PickPopup(this, std::move(items), cur);
  p->free_hint = "Enter: custom method";
  method_pick_ = p;
  p->on_close = [this, p] {
    if (method_pick_ == p) method_pick_ = nullptr;
  };
  p->on_pick = [this](const wxString& v) {
    std::string m(trim(v.Upper().utf8_string()));
    if (m.empty() || !watt::is_http_token(m)) {
      wxBell();  // not an RFC 9110 token: the method stays
      return;
    }
    if (m == method_str_) return;
    set_method(m);
    note_body_method();
    touched();
  };
  const wxRect r = method_->GetScreenRect();
  p->show_below(wxRect(r.x, r.y, r.width, r.height + FromDIP(4)));
}

bool HttpEditor::method_pick_key(wxKeyEvent& e) {
  const int k = e.GetKeyCode();
  if (k == WXK_ALT || k == WXK_CONTROL || k == WXK_SHIFT || k == WXK_WINDOWS_LEFT || k == WXK_WINDOWS_RIGHT) return false;
  if ((k == WXK_DOWN || k == WXK_NUMPAD_DOWN || k == WXK_UP || k == WXK_NUMPAD_UP) && e.GetModifiers() == wxMOD_ALT) {
    method_pick_->close();  // Alt+Down / Alt+Up toggles
    return true;
  }
  if (e.ControlDown() || e.AltDown() || k == WXK_LEFT || k == WXK_RIGHT) {
    method_pick_->close();  // and the key does what it does
    return false;
  }
  if (k == WXK_SPACE && method_pick_->filter().empty()) {
    method_pick_->pick_selected();
    return true;
  }
  return method_pick_->key(k);  // a character: filtered on wxEVT_CHAR
}

bool HttpEditor::method_pick_char(wxKeyEvent& e) {
  if (e.ControlDown() || e.AltDown()) return false;
  const wxChar c = e.GetUnicodeKey();
  if (c == WXK_NONE || c < 32) return c != WXK_NONE;  // Enter / Tab / Esc were handled on key down
  return method_pick_->chr(static_cast<wxChar>(wxToupper(c)));
}

void HttpEditor::method_pick_focus_lost() {
  if (!method_pick_) return;
  CallAfter([this] {
    if (!method_pick_) return;
    // A press on the list picks there (on the press); keep it open for that.
    if (method_pick_->GetScreenRect().Contains(wxGetMousePosition()) && wxGetMouseState().LeftIsDown()) return;
    wxWindow* f = wxWindow::FindFocus();
    if (f == url_ || f == method_) return;
    method_pick_->close();
  });
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
  if (gql_query_) gql_timer_.StartOnce(300);  // the endpoint's schema, once the URL settles
  update_placeholder(u.empty());
  highlight_vars(url_, 0, url_->GetLength());
  sync_path_rows();
  update_preview();
  // Params follow the URL; disabled params and rows without a key (not in the URL) are kept at
  // the end.
  std::vector<KvRow> rows;
  for (const auto& q : parse_query_decoded(split_url(u).query)) {
    rows.push_back({q.key, q.value, true, false, q.bare});
    if (const auto it = param_descs_.find(q.key); it != param_descs_.end()) rows.back().desc = it->second;
  }
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
  return with_path_values(
      url,
      [this](const std::string& n) {
        const auto it = path_values_.find(n);
        return it == path_values_.end() ? std::string() : it->second;
      },
      encode_url_now());
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
  if (!path_panel_ && path_names_.empty()) return;
  // Building or showing the Path table happens while the user types in the URL; on Windows the
  // new grid (or the page around it) takes the keyboard focus, and every later keystroke landed
  // in a panel that ignores it. Keep the focus where it was, now and after queued focus events.
  wxWindow* focus = wxWindow::FindFocus();
  const auto keep_focus = [this, focus] {
    wxWindow* now = wxWindow::FindFocus();
    if (focus && now != focus && (!now || now == params_page_ || params_page_->IsDescendant(now)))
      focus->SetFocus();
  };
  if (!path_panel_) {
    wxWindowUpdateLocker lock(params_page_);
    path_panel_ = new KvPanel(params_page_, KvGrid::Kind::Path, "Path variables");
    path_panel_->SetToolTip("Each /:name segment of the URL is replaced by its value, URL-encoded");
    path_ = path_panel_->grid();
    path_->lookup = [this](const std::string& n) { return resolve(n); };
    path_->var_names = [this] { return var_names(); };
    path_->on_change = [this] {
      if (loading_) return;
      for (const auto& row : path_->rows()) {
        path_values_[row.key] = row.value;
        path_descs_[row.key] = row.desc;
      }
      update_preview();
      touched();
    };
    params_page_->GetSizer()->Add(path_panel_, 1, wxEXPAND | wxTOP, FromDIP(8));
  }
  std::vector<KvRow> rows;
  for (const auto& n : path_names_) {
    const auto it = path_values_.find(n);
    const auto d = path_descs_.find(n);
    rows.push_back({n, it == path_values_.end() ? std::string() : it->second, true, false, false,
                    d == path_descs_.end() ? std::string() : d->second});
  }
  const bool was = loading_;
  loading_ = true;
  path_->set_rows(rows);
  loading_ = was;
  path_panel_->Show(!path_names_.empty());
  layout_params_page();
  if (focus && !params_page_->IsDescendant(focus)) {
    keep_focus();
    wxWeakRef<wxWindow> weak(focus);
    CallAfter([this, weak, keep_focus] {
      if (weak) keep_focus();
    });
  }
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
  for (const auto& [k, v] : inh_.vars) names.insert(k);  // folder / collection variables
  for (const auto& [k, v] : ws->runtime_vars) names.insert(k);  // captured / set by scripts
  for (const auto& n : names)
    if (!n.empty() && std::none_of(n.begin(), n.end(), [](char c) { return static_cast<unsigned char>(c) <= 32; }))
      out.push_back(n);
  for (const char* d : {"$guid", "$timestamp", "$isoTimestamp", "$randomInt"}) out.emplace_back(d);
  for (const auto& n : pre_names_)
    if (!names.count(n)) out.push_back(n);
  return out;
}

std::string HttpEditor::expand_span(const Workspace& ws, const std::string& span) const {
  // A folder / collection variable (inh_.vars, nearest folder first) comes after the runtime
  // variables and before the environment, as core expands it when the request is sent.
  if (!inh_.vars.empty() && span.size() > 4 && span.compare(0, 2, "{{") == 0) {
    const std::string name(trim(std::string_view(span).substr(2, span.size() - 4)));
    if (!ws.runtime_vars.count(name))
      for (const auto& [k, v] : inh_.vars)
        if (k == name) return v.find("{{") == std::string::npos ? v : ws.expand(v);
  }
  return ws.expand(span);
}

std::string HttpEditor::sent_url() const {
  const bool enc = encode_url_now();
  const std::string t = enc ? normalize_url(with_path(url_text())) : minimal_url(with_path(url_text()));
  const Workspace* ws = host_->workspace();
  if (!ws || t.find("{{") == std::string::npos) return t;
  return expand_url(t, [this, ws](const std::string& span) { return expand_span(*ws, span); }, enc);
}

std::string HttpEditor::sent_form(const std::string& body) const {
  const Workspace* ws = host_->workspace();
  if (!ws) return body;
  return expand_form(body, [this, ws](const std::string& span) { return expand_span(*ws, span); });
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
    case AuthMode::OAuth2: {
      const OAuth2Config& o = av_.adv.oauth2;
      scan(o.token_url + "\n" + o.client_id + "\n" + o.client_secret + "\n" + o.scope + "\n" + o.audience);
      if (o.grant == OAuth2Config::Grant::Password) scan(o.username + "\n" + o.password);
      if (o.grant == OAuth2Config::Grant::AuthorizationCode) scan(o.auth_url);
      break;
    }
    case AuthMode::Digest: case AuthMode::Ntlm: case AuthMode::Negotiate:
      scan(av_.adv.user + "\n" + av_.adv.password);
      break;
    case AuthMode::AwsSigV4:
      scan(av_.adv.aws_access_key + "\n" + av_.adv.aws_secret_key + "\n" + av_.adv.aws_session_token + "\n" +
           av_.adv.aws_region + "\n" + av_.adv.aws_service);
      break;
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

std::vector<std::pair<std::string, std::string>> HttpEditor::brace_problems() const {
  std::vector<std::pair<std::string, std::string>> out;
  auto scan = [&](const std::string& where, const std::string& text) {
    if (std::string p = brace_problem(text); !p.empty()) out.emplace_back(where, std::move(p));
  };
  scan("the URL", url_text());
  for (const auto& n : path_names_)
    if (const auto it = path_values_.find(n); it != path_values_.end()) scan("path variable :" + n, it->second);
  for (const auto& row : header_rows())
    if (row.enabled) {
      scan("header name " + row.key, row.key);
      scan("header " + row.key, row.value);
    }
  if (body_mode_ == BodyMode::UrlEncoded || body_mode_ == BodyMode::Multipart)
    for (const auto& row : form_rows(body_mode_ == BodyMode::UrlEncoded ? 0 : 1))
      if (row.enabled) {
        scan("form field name " + row.key, row.key);
        if (!row.file) scan("form field " + row.key, row.value);
      }
  return out;
}

void HttpEditor::update_preview() {
  const std::string u = sent_url();
  // "{{base}}" that already holds a scheme behind a typed / imported "http://": the request
  // would go to the host "http" (curl -> http://http://127.0.0.1/...). Said first, so the middle
  // ellipsis never hides it.
  const bool doubled = doubled_scheme(u);
  const wxString w = u.empty() ? wxString()
                     : doubled ? wxString::FromUTF8("\xE2\x9A\xA0 Doubled scheme  " + u)
                               : wxString::FromUTF8("Sent as  " + u);
  preview_->SetForegroundColour(doubled ? Theme::get().var_bad : Theme::get().dim);
  if (w == preview_->GetLabelText()) return;
  preview_->SetLabelText(w);  // no '&' mnemonics
  preview_->SetToolTip(doubled ? wxString::FromUTF8("Sent as " + u +
                                                    "\nA {{variable}} in the URL already holds a scheme: remove the "
                                                    "http:// typed in front of it.")
                               : wxString::FromUTF8(u));
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
  for (const auto& r : params_->rows()) {
    items.push_back({r.key, r.value, r.enabled && !trim(r.key).empty(), r.value.empty()});
    if (!r.key.empty()) param_descs_[r.key] = r.desc;
  }
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
  // curl's default "http://" goes in front of a URL without a scheme, but not in front of a
  // {{variable}} (Postman's {{baseUrl}}/path): that variable holds the scheme.
  if (imp) imp->http.url = strip_curl_default_scheme(imp->http.url, cmd);
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
  // Redirect / URL / TLS flags (--max-redirs, --post30x, --location-trusted, --path-as-is,
  // --tlsv1.x): the request's own Options (SavedRequest::network), which the Options tab shows
  // and the send applies; the imported command decides all five (a flag it lacks: inherit).
  // HttpRequest's copies go back to their defaults so Copy as reads the same single source.
  {
    NetworkSettings n = r.network;
    n.max_redirects.reset();
    n.keep_method.reset();
    n.keep_auth.reset();
    n.encode_url.reset();
    n.tls_min.reset();
    r.network = with_http_options(std::move(n), imp->http);
    const HttpRequest def;
    r.http.max_redirects = def.max_redirects;
    r.http.keep_method_on_redirect = def.keep_method_on_redirect;
    r.http.keep_auth_on_redirect = def.keep_auth_on_redirect;
    r.http.encode_url = def.encode_url;
    r.http.tls_min_version = def.tls_min_version;
  }
  r.disabled_headers.clear();
  r.disabled_params.clear();
  r.form = imp->form;
  r.basic_auth = imp->basic_auth;
  if (imp->auth.type != HttpAuth::Type::None) r.auth = imp->auth;  // --digest -u, --aws-sigv4 -u, ...
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
  // JSON -> GraphQL by hand: a body that already is {"query": ...} fills the two editors.
  if (user && m == BodyMode::GraphQL && body_mode_ == BodyMode::Json && gql_query_text().empty() && !pending_body_ &&
      body_->GetLength() < 1024 * 1024) {
    std::string q, v, op;
    if (split_graphql_body(std::string_view(body_->GetCharacterPointer(), static_cast<size_t>(body_->GetLength())), q, v, &op)) {
      gql_q_val_ = q;
      gql_v_val_ = v;
      gql_op_ = op;
      if (gql_query_) {
        gql_query_->SetText(wxString::FromUTF8(q));
        gql_vars_->SetText(wxString::FromUTF8(v));
      }
    }
  }
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
  if (text_kind_) {
    text_kind_->Show(m == BodyMode::Text);
    if (m == BodyMode::Text) sync_text_kind();
  }
  // Only once a one-line body is shown formatted (or the request was saved that way).
  minify_->Show(m == BodyMode::Json && (minify_->value() || raw_body_));
  lint_timer_.StartOnce(50);
  fit_body_tools();
  if (!user) return;
  // A body picked on a GET / HEAD: POST, as GraphQL does (curl -d does the same). Most servers
  // and proxies ignore or refuse a GET body; picking GET again keeps it (with a note).
  if (m != BodyMode::None && m != BodyMode::GraphQL && (method_str_ == "GET" || method_str_ == "HEAD")) {
    note(wxString::FromUTF8(method_str_ + " -> POST: a " + method_str_ +
                            " body is ignored or refused by most servers (pick GET to keep it)"));
    set_method("POST");
  }
  switch (m) {
    case BodyMode::None: set_content_type(nullptr); break;
    case BodyMode::Json: set_content_type("application/json"); break;
    case BodyMode::Text: set_content_type("text/plain"); break;
    case BodyMode::Xml: set_content_type("application/xml"); break;
    case BodyMode::UrlEncoded: set_content_type("application/x-www-form-urlencoded"); break;
    case BodyMode::Multipart: set_content_type(nullptr); break;  // built with its boundary at send time
    case BodyMode::Binary: set_content_type("application/octet-stream"); break;
    case BodyMode::GraphQL:
      set_content_type("application/json");
      // The query goes as a JSON POST body (as the note under it says): GET / HEAD carry none.
      if (method_str_ == "GET" || method_str_ == "HEAD") set_method("POST");
      break;
    case BodyMode::CurlData: break;
  }
  if (text_mode(m)) body_->SetFocus();
  if (m == BodyMode::GraphQL && gql_query_) gql_query_->SetFocus();
  touched();
}

void HttpEditor::sync_text_kind() {
  if (!text_kind_ || body_mode_ != BodyMode::Text) return;
  int k = 0;
  for (const auto& r : header_rows())
    if (r.enabled && iequals(trim(r.key), "Content-Type")) k = text_kind_of(r.value);
  text_kind_->set_selection(k);
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
    if (!auth_built_[auth_page(m)]) {
      build_auth_subpage(auth_page(m));
      push_auth();  // the new page's fields
    }
    auth_book_->ChangeSelection(auth_page(m));
    set_scheme_hint();
    update_oauth_status();
    if (m == AuthMode::Inherit) show_inherited();
  }
  if (m != old) update_auto_headers();
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
  wxWindow* focus = m == AuthMode::Bearer   ? static_cast<wxWindow*>(bearer_)
                    : m == AuthMode::Basic  ? basic_user_
                    : m == AuthMode::ApiKey ? key_value_
                    : m == AuthMode::OAuth2 ? o_token_url_
                    : m == AuthMode::AwsSigV4 ? s_aws_key_
                    : scheme_mode(m)        ? s_user_
                                            : nullptr;
  if (focus) CallAfter([focus] { focus->SetFocus(); });  // after the dropdown has closed
  touched();
}

// The auth type list: a themed popup that filters on typed text like the method list ("bear"
// Enter picks Bearer token), Inherit from parent first.
void HttpEditor::show_auth_menu() {
  if (auth_pick_) {
    auth_pick_->close();
    return;
  }
  static const char* const desc[] = {"no Authorization header",   "Authorization: Bearer <token>",
                                     "user + password",           "a header or query parameter",
                                     "token from a token URL",    "challenge / response",
                                     "Windows challenge / response", "Kerberos / SPNEGO",
                                     "signed request",            "the folder's / collection's auth"};
  std::vector<PickItem> items;
  for (AuthMode m : kAuthOrder) items.push_back({kAuthLabels[static_cast<int>(m)], desc[static_cast<int>(m)], ""});
  if (wxWindow::FindFocus() != auth_btn_) auth_btn_->SetFocus();  // the window that forwards the keys
  auto* p = new PickPopup(this, std::move(items), kAuthLabels[static_cast<int>(auth_mode_)]);
  auth_pick_ = p;
  p->on_close = [this, p] {
    if (auth_pick_ == p) auth_pick_ = nullptr;
  };
  p->on_pick = [this](const wxString& v) {
    for (AuthMode m : kAuthOrder)
      if (v == kAuthLabels[static_cast<int>(m)]) {
        if (m != auth_mode_) set_auth_mode(m, true);
        return;
      }
  };
  const wxRect r = auth_btn_->GetScreenRect();
  p->show_below(wxRect(r.x, r.y, r.width, r.height + FromDIP(4)));
}

std::filesystem::path HttpEditor::own_folder() const {
  const Workspace* ws = host_->workspace();
  if (!ws) return {};
  const std::filesystem::path f = inherit_from();
  if (f.empty()) return ws->root();
  const std::filesystem::path dir = f.parent_path().lexically_normal();
  const std::filesystem::path rel = dir.lexically_relative(ws->root().lexically_normal());
  if (rel.empty() || *rel.begin() == "..") return ws->root();
  return dir;
}

// What the folders and the collection give this request, read on a worker (folder.json files
// are on disk; the UI thread never waits on them).
void HttpEditor::refresh_inherited() {
  const Workspace* ws = host_->workspace();
  const uint64_t gen = ++inh_gen_;
  if (!ws) return;  // opened before the workspace (start-up): read when the Auth / Headers tab shows
  SavedRequest probe;  // only what decides the inheritance: the file (its folders), an inheriting auth
  probe.kind = RequestKind::Http;
  probe.file = inherit_from();
  probe.auth.type = HttpAuth::Type::Inherit;
  auto wsc = std::make_shared<Workspace>(*ws);
  std::thread([alive = alive_, wsc, probe, gen] {
    InheritView v = make_inherit_view(wsc->effective_settings(probe));
    std::lock_guard lk(alive->m);
    auto* ed = static_cast<HttpEditor*>(alive->target);
    if (!ed) return;
    ed->CallAfter([ed, v = std::move(v), gen]() mutable {
      if (gen != ed->inh_gen_) return;
      ed->inh_ = std::move(v);
      ed->inh_done_gen_ = gen;
      // Folder variables just became known (a restored tab, a saved folder.json): repaint them.
      if (ed->url_) ed->highlight_vars(ed->url_, 0, ed->url_->GetLength());
      for (auto& f : ed->var_fields_)
        if (wxStyledTextCtrl* s = f.get()) ed->highlight_vars(s, 0, s->GetLength());
      ed->show_inherited();
      if (ed->net_panel_) {
        ed->net_panel_->set_inherited(wxString::FromUTF8(ed->inh_.network));
        ed->net_panel_->set_above(ed->inh_.opts);
      }
      ed->update_preview();  // a folder's "Encode URL" changes what is sent
      ed->show_inherited_asserts();
      ed->update_badges();
      ed->update_auto_headers();
    });
  }).detach();
}

// "No auth" does not drop the Authorization headers a folder or the collection sends (a folder's
// Bearer token / API key is one of its headers): say which still go, and from where.
void HttpEditor::show_none_note() {
  if (!none_inh_) return;
  const InheritView::Header* h = nullptr;
  for (const auto& x : inh_.headers) {
    if (x.auth || iequals(x.name, "Authorization")) {
      h = &x;
      break;
    }
  }
  // A row of this request's own with the same name replaces the inherited one.
  bool own = false;
  for (const auto& r : header_rows())
    if (h && r.enabled && iequals(trim(r.key), h->name)) own = true;
  const bool show = h && !own;
  none_inh_->Show(show);
  none_headers_->Show(show);
  none_folder_->Show(show);
  if (show) {
    const std::string from = h->from.empty() ? std::string("Collection") : h->from;
    const std::string shown = iequals(h->name, "Authorization") && h->value.rfind("Bearer ", 0) == 0
                                  ? "Bearer " + mask_secret(std::string_view(h->value).substr(7))
                                  : mask_secret(h->value);
    none_inh_->SetLabel(wxString::FromUTF8("Still sent: the " + h->name + " header from " + from + " (" +
                                           shown + "). It is a header, not auth: add a " + h->name +
                                           " row on the Headers tab to replace it, or remove it on " + from + "."));
    none_inh_->Wrap(auth_wrap_ > 0 ? auth_wrap_ : FromDIP(560));
    const Workspace* ws = host_->workspace();
    none_folder_path_ = !ws ? std::filesystem::path()
                            : from == "Collection" ? ws->root()
                                                   : ws->root() / std::filesystem::path(wxString::FromUTF8(from).ToStdWstring());
    none_folder_->SetLabel(wxString::FromUTF8("Edit on " + from));
    none_folder_->InvalidateBestSize();
  }
  if (auto* pg = none_inh_->GetParent()) pg->Layout();
}

void HttpEditor::show_inherited() {
  show_none_note();
  if (!inh_title_) return;
  const Theme& t = Theme::get();
  wxString title, detail;
  if (!inh_.known) {
    title = wxString::FromUTF8("Reading the folder settings\xE2\x80\xA6");
  } else if (inh_.source.empty() || (inh_.type == "No auth" && inh_.source == "Collection")) {
    title = "Inherit: no auth set above this request";
    detail = "No folder above it and not the collection sets auth, so it is sent with none. Set it once on a folder or "
             "the collection and every request below that inherits uses it.";
  } else {
    title = wxString::FromUTF8("Inherit from " + inh_.source + ": " + inh_.type);
    detail = wxString::FromUTF8(inh_.detail);
  }
  inh_title_->SetLabel(title);
  inh_detail_->SetLabel(detail);
  inh_detail_->Wrap(auth_wrap_ > 0 ? auth_wrap_ : FromDIP(560));
  inh_edit_->SetLabel(inh_.source.empty() ? wxString("Edit on folder")
                                          : wxString::FromUTF8("Edit on " + inh_.source));
  inh_edit_->SetToolTip(inh_.source.empty() ? wxString("Open the settings of this request's folder")
                                            : wxString::FromUTF8("Open the settings of " + inh_.source +
                                                                 ", where this auth is set"));
  inh_edit_->InvalidateBestSize();
  inh_own_->Show(!inh_.source.empty() && own_folder() != inh_.source_folder);
  if (!inh_.errors.empty()) {
    wxString e = "Not used (the request is not sent while it is broken):";
    for (const auto& x : inh_.errors) e += "\n" + wxString::FromUTF8(x);
    inh_errors_->SetLabel(e);
    inh_errors_->SetForegroundColour(t.js_error);
    inh_errors_->Show();
  } else {
    inh_errors_->Hide();
  }
  if (auto* pg = inh_title_->GetParent()) pg->Layout();
}

void HttpEditor::edit_folder(const std::filesystem::path& folder) {
  const Workspace* ws = host_->workspace();
  if (!ws || folder.empty()) return;
  wxWeakRef<HttpEditor> self(this);
  edit_folder_settings(this, *ws, folder, [self] {
    if (!self) return;
    self->refresh_inherited();
    self->note("Folder settings saved");
  });
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
  // Width tracking only grows: the one long line just re-indented must not leave a scroll bar.
  body_->SetXOffset(0);
  body_->SetScrollWidth(1);
  note("Formatted");
}


bool HttpEditor::big_fold(int line) {
  if (fill_src_) return true;  // the rest is still being appended (a few frames)
  if (body_->GetLineCount() < 100000 || line < 0) return false;
  if (!(body_->GetFoldLevel(line) & wxSTC_FOLDLEVELHEADERFLAG)) return false;
  const int last = fold_block_end(line);
  if (last <= line) return false;
  if (fold_edits_ != body_edits_ || fold_doc_ != body_->GetDocPointer()) {
    fold_ops_.clear();  // queued for another text
    fold_edits_ = body_edits_;
    fold_doc_ = body_->GetDocPointer();
  }
  const bool contract = body_->GetFoldExpanded(line);
  body_->SetFoldExpanded(line, !contract);
  if (contract) {
    // The caret inside the block would be hidden: put it on the header (as Scintilla does).
    const int cur = body_->LineFromPosition(body_->GetCurrentPos());
    if (cur > line && cur <= last) body_->GotoPos(body_->GetLineEndPosition(line));
    // After the boxed "..." Scintilla draws: how many lines the fold hides.
    body_->EOLAnnotationSetText(line, wxString::Format("%d lines", last - line));
    body_->EOLAnnotationSetStyle(line, wxSTC_STYLE_FOLDDISPLAYTEXT);
  } else {
    body_->EOLAnnotationSetText(line, "");
  }
  fold_ops_.push_back({line + 1, last, !contract});
  // The slices run from a timer: a message, so input and paints get in between them (CallAfter
  // events would all run in one go).
  wxWakeUpIdle();
  if (!fold_timer_.IsRunning()) fold_timer_.StartOnce(40);
  return true;
}

int HttpEditor::fold_block_end(int line) {
  if (blocks_edits_ == body_edits_ && blocks_doc_ == body_->GetDocPointer()) {
    const auto it = std::lower_bound(big_blocks_.begin(), big_blocks_.end(), std::make_pair(line, -1));
    if (it != big_blocks_.end() && it->first == line) return it->second;
  }
  const int len = body_->GetLength();
  const char* p = body_->GetCharacterPointer();
  const int ls = body_->PositionFromLine(line), le = body_->GetLineEndPosition(line);
  // The outermost opener on the line that the line itself does not close.
  int opener = -1, depth = 0;
  bool str = false;
  for (int i = ls; i < le; ++i) {
    const char c = p[i];
    if (str) {
      if (c == '\\') ++i;
      else if (c == '"') str = false;
      continue;
    }
    if (c == '"') str = true;
    else if (c == '/' && i + 1 < le && p[i + 1] == '/') break;
    else if (c == '{' || c == '[') {
      if (depth++ == 0) opener = i;
    } else if ((c == '}' || c == ']') && depth > 0) {
      if (--depth == 0) opener = -1;
    }
  }
  if (opener < 0 || depth == 0) return -1;
  const size_t close = json_block_end(std::string_view(p, static_cast<size_t>(len)), static_cast<size_t>(opener));
  if (close != std::string_view::npos) return body_->LineFromPosition(static_cast<int>(close));
  return body_->GetLineCount() - 1;  // not closed: to the end, as the lexer folds it
}

void HttpEditor::run_fold_ops() {
  if (fold_edits_ != body_edits_ || fold_doc_ != body_->GetDocPointer()) fold_ops_.clear();
  // ~7 ms per turn (fold_lines_ adapts): a 1.4M-line root folds over a few frames, no long stall.
  const auto t0 = Clock::now();
  int budget = fold_lines_;
  const int lines0 = budget;
  while (!fold_ops_.empty() && budget > 0) {
    FoldOp& op = fold_ops_.front();
    const int e = std::min(op.to, op.from + budget - 1);
    if (op.show) body_->ShowLines(op.from, e);
    else body_->HideLines(op.from, e);
    budget -= e - op.from + 1;
    if (e < op.to) {
      op.from = e + 1;
      break;
    }
    const FoldOp done = op;
    fold_ops_.pop_front();
    if (done.show) {
      // A block folded inside the one just shown stays folded.
      std::vector<FoldOp> again;
      for (int h = body_->ContractedFoldNext(done.from); h >= 0 && h <= done.to;) {
        const int last = fold_block_end(h);
        if (last > h) again.push_back({h + 1, std::min(last, done.to), false});
        h = body_->ContractedFoldNext(std::max(last, h) + 1);
      }
      fold_ops_.insert(fold_ops_.begin(), again.begin(), again.end());
    }
  }
  const double ms = ms_between(t0, Clock::now());
  fold_max_ms_ = std::max(fold_max_ms_, ms);
  if (const int done = lines0 - budget; done >= 10000) {
    const double want = done * 7.0 / std::max(ms, 0.25);
    fold_lines_ = static_cast<int>(std::clamp(want, fold_lines_ * 0.5, fold_lines_ * 1.5));
    fold_lines_ = std::clamp(fold_lines_, 20000, 400000);
  }
  if (!fold_ops_.empty()) {
    fold_timer_.StartOnce(40);
  } else if (reveal_pending_) {
    reveal_pending_ = false;
    scroll_to_match(body_, body_->GetSelectionStart(), body_->GetSelectionEnd());
  }
}

void HttpEditor::reveal_body(int first, int last) {
  if (body_->GetLineCount() < 100000) {  // Scintilla unfolds a small body at once
    for (int l = first; l <= last; ++l)
      if (!body_->GetLineVisible(l)) body_->EnsureVisible(l);
    return;
  }
  if (fold_edits_ != body_edits_ || fold_doc_ != body_->GetDocPointer()) {
    fold_ops_.clear();
    fold_edits_ = body_edits_;
    fold_doc_ = body_->GetDocPointer();
  }
  // The folded blocks around the match, outermost first: expanding them all and showing the
  // outermost one's lines shows the match (folded blocks beside it stay folded, see run_fold_ops).
  int outer = -1, outer_end = -1;
  for (int h = body_->ContractedFoldNext(0); h >= 0 && h < first;) {
    const int end = fold_block_end(h);
    if (end >= first) {
      body_->SetFoldExpanded(h, true);
      body_->EOLAnnotationSetText(h, "");
      if (outer < 0) {
        outer = h;
        outer_end = end;
      }
      h = body_->ContractedFoldNext(h + 1);
    } else {
      h = body_->ContractedFoldNext(std::max(end, h) + 1);
    }
  }
  if (outer < 0) {
    for (int l = first; l <= last; ++l)
      if (!body_->GetLineVisible(l)) body_->EnsureVisible(l);
    return;
  }
  fold_ops_.push_back({outer + 1, outer_end, true});
  reveal_pending_ = true;
  wxWakeUpIdle();
  if (!fold_timer_.IsRunning()) fold_timer_.StartOnce(40);
}

// Debounced JSON syntax check: small bodies here (~10 ms per MB), big ones on a worker.
void HttpEditor::lint() {
  // The "long line" banner goes once the body no longer has one (a paste shown formatted, a
  // Beautify, another body mode): ~2 ms per 10 MB, debounced with the lint.
  if (long_line_banner_ && !pending_body_) {
    if (!banner_ || !banner_->IsShown()) {
      long_line_banner_ = false;
    } else if (body_mode_ != BodyMode::Json ||
               longest_line(std::string_view(body_->GetCharacterPointer(), static_cast<size_t>(body_->GetLength()))) <
                   kLongLine) {
      hide_banner();
    }
  }
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
  // Sent minified: build that copy here too, so the next send does not minify on the UI thread.
  const bool minify = minify_ && minify_->value() && !(raw_body_ && raw_edits_ == body_edits_) && !minified_current();
  const uint64_t edits = body_edits_;
  const bool blocks = body_->GetLineCount() >= 100000;  // where big_fold folds
  void* doc = body_->GetDocPointer();
  // Edited back to the pasted tokens (x then Backspace): the pasted bytes go out again.
  std::shared_ptr<const std::string> raw = minify ? raw_body_ : nullptr;
  std::thread([alive = alive_, text, gen, minify, edits, blocks, doc, raw] {
    auto issue = check_json(*text);
    std::shared_ptr<const std::string> mini;
    if (minify) mini = std::make_shared<const std::string>(minify_json(*text));
    const bool same_as_raw = mini && raw && mini->size() <= raw->size() && *mini == minify_json(*raw);
    std::vector<std::pair<int, int>> big;
    if (blocks) big = json_big_blocks(*text, kBigBlockLines);
    std::lock_guard lk(alive->m);
    if (!alive->target) return;
    auto* ed = static_cast<HttpEditor*>(alive->target);
    ed->CallAfter([ed, gen, issue = std::move(issue), mini, edits, blocks, doc, big = std::move(big), raw,
                   same_as_raw]() mutable {
      if (mini && edits == ed->body_edits_) {
        ed->minified_ = mini;
        ed->minified_edits_ = edits;
        if (same_as_raw && raw == ed->raw_body_ && ed->raw_edits_ != edits) {
          ed->raw_edits_ = edits;
          ed->note("Body back to the pasted JSON: sent as pasted.");
        }
      }
      if (blocks && edits == ed->body_edits_ && doc == ed->body_->GetDocPointer()) {
        ed->big_blocks_ = std::move(big);
        ed->blocks_edits_ = edits;
        ed->blocks_doc_ = doc;
      }
      ed->show_lint(gen, issue);
    });
  }).detach();
}

void HttpEditor::lint_gql() {
  if (!gql_query_ || !gql_lint_) return;
  const int len = gql_query_->GetLength();
  gql_query_->SetIndicatorCurrent(kIndGql);
  gql_query_->IndicatorClearRange(0, len);
  std::optional<JsonIssue> issue;
  std::vector<GqlIssue> schema_issues;
  if (len < 256 * 1024) {  // a query is small: checked as typed (~1 ms per 100 KB)
    const std::string_view doc(gql_query_->GetCharacterPointer(), static_cast<size_t>(len));
    issue = check_graphql(doc);
    // Brackets first: an unclosed one makes everything after it ambiguous.
    if (!issue) schema_issues = gql_validate(doc, gql_schema_.get());
  }
  wxString msg;
  auto where = [&](int at) {
    const int line = gql_query_->LineFromPosition(at);
    return wxString::Format("Query line %d col %d: ", line + 1, at - gql_query_->PositionFromLine(line) + 1);
  };
  if (issue && static_cast<int>(issue->offset) <= len) {
    msg = where(static_cast<int>(issue->offset)) + wxString::FromUTF8(issue->message);
  } else if (!schema_issues.empty()) {
    for (const auto& i : schema_issues) {
      const int at = static_cast<int>(i.offset);
      if (at < len) gql_query_->IndicatorFillRange(at, std::max(1, std::min(static_cast<int>(i.length), len - at)));
    }
    msg = where(static_cast<int>(std::min<size_t>(schema_issues[0].offset, static_cast<size_t>(len)))) +
          wxString::FromUTF8(schema_issues[0].message);
    if (schema_issues.size() > 1)
      msg += wxString::Format("  (+%d more, underlined)", static_cast<int>(schema_issues.size() - 1));
  }
  if (gql_lint_->GetLabel() != msg) {
    gql_lint_->SetLabel(msg);
    gql_lint_->SetToolTip(msg);  // the whole message when it is cut short
  }
}

// The variables JSON: its syntax first, then its keys against the operation's declarations.
void HttpEditor::lint_gql_vars() {
  if (!gql_vars_ || !gql_vars_lint_ || !gql_query_) return;
  const int len = gql_vars_->GetLength();
  gql_vars_->SetIndicatorCurrent(kIndGql);
  gql_vars_->IndicatorClearRange(0, len);
  wxString msg;
  if (len < 256 * 1024 && gql_query_->GetLength() < 256 * 1024) {
    const std::string vars(gql_vars_->GetCharacterPointer(), static_cast<size_t>(len));
    const std::string_view doc(gql_query_->GetCharacterPointer(), static_cast<size_t>(gql_query_->GetLength()));
    auto where = [&](int at) {
      const int line = gql_vars_->LineFromPosition(at);
      return wxString::Format("Variables line %d col %d: ", line + 1, at - gql_vars_->PositionFromLine(line) + 1);
    };
    if (const auto ji = check_json(vars)) {
      const int at = static_cast<int>(std::min<size_t>(ji->offset, static_cast<size_t>(len)));
      if (at < len) gql_vars_->IndicatorFillRange(at, std::max(1, std::min(static_cast<int>(ji->length), len - at)));
      msg = where(at) + wxString::FromUTF8(ji->message);
    } else if (const auto v = gql_check_variables(doc, vars, gql_op_); !v.empty()) {
      for (const auto& i : v)
        if (i.length > 0 && static_cast<int>(i.offset) < len)
          gql_vars_->IndicatorFillRange(static_cast<int>(i.offset),
                                        std::min(static_cast<int>(i.length), len - static_cast<int>(i.offset)));
      msg = (v[0].length > 0 ? where(static_cast<int>(v[0].offset)) : wxString("Variables: ")) +
            wxString::FromUTF8(v[0].message);
      if (v.size() > 1) msg += wxString::Format("  (+%d more)", static_cast<int>(v.size() - 1));
    }
  }
  if (gql_vars_lint_->GetLabel() != msg) {
    gql_vars_lint_->SetLabel(msg);
    gql_vars_lint_->SetToolTip(msg);
  }
}

// ------------------------------------------------------------------ GraphQL schema

namespace {
struct GqlCached {
  std::shared_ptr<const GqlSchema> schema;
  double when = 0;
};
// Fetched schemas by URL + environment, for every tab (UI thread only).
std::map<std::string, GqlCached>& gql_cache() {
  static std::map<std::string, GqlCached> c;
  return c;
}
double epoch_now() {
  return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}
// A schema of thousands of types is ~100k small allocations: the last reference to one is
// dropped on a worker, never in a UI step.
void release_schema(std::shared_ptr<const GqlSchema>&& old) {
  if (old && old.use_count() == 1 && old->types.size() > 64) std::thread([o = std::move(old)] {}).detach();
  old.reset();
}
}  // namespace

void HttpEditor::build_gql_schema_row(wxWindow* parent, wxSizer* sizer) {
  const Theme& t = Theme::get();
  auto* row = new wxBoxSizer(wxHORIZONTAL);
  auto* l = new wxStaticText(parent, wxID_ANY, "Schema");
  l->SetForegroundColour(t.dim);
  row->Add(l, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
  // Fixed size, ellipsized: a long error must not widen the editor (RequestTab::center_sash).
  gql_chip_ = new wxStaticText(parent, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                               wxST_NO_AUTORESIZE | wxST_ELLIPSIZE_END);
  gql_chip_->SetMinSize(wxSize(FromDIP(60), GetCharHeight()));
  row->Add(gql_chip_, 1, wxALIGN_CENTER_VERTICAL);
  gql_fetch_ = new FlatButton(parent, "Fetch schema", FlatButton::Style::Outline);
  gql_fetch_->SetToolTip("POST the introspection query to the URL above (its headers, auth and variables apply), "
                         "on a worker thread. The schema completes fields, arguments and $variables as you type "
                         "(Ctrl+Space) and underlines unknown ones.");
  gql_fetch_->on_click = [this] { gql_fetch(); };
  gql_explore_ = new FlatButton(parent, "Explore", FlatButton::Style::Outline);
  gql_explore_->SetToolTip("Browse the schema: each type's fields with their arguments, types and descriptions. "
                           "Type to search, Enter inserts the field at the query's caret.");
  gql_explore_->on_click = [this] { gql_explore(); };
  gql_explore_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
    if (gql_explore_pop_ && !e.ControlDown() && !e.AltDown()) {
      const int k = e.GetKeyCode();
      if (k == WXK_LEFT || k == WXK_RIGHT) {
        gql_explore_pop_->close();
      } else if (gql_explore_pop_->key(k)) {
        return;
      }
    }
    e.Skip();
  });
  gql_explore_->Bind(wxEVT_CHAR, [this](wxKeyEvent& e) {
    const wxChar c = e.GetUnicodeKey();
    if (gql_explore_pop_ && !e.ControlDown() && !e.AltDown() && c >= 32 && gql_explore_pop_->chr(c)) return;
    e.Skip();
  });
  gql_explore_->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) {
    e.Skip();
    CallAfter([this] {
      if (!gql_explore_pop_) return;
      if (gql_explore_pop_->GetScreenRect().Contains(wxGetMousePosition()) && wxGetMouseState().LeftIsDown()) return;
      gql_explore_pop_->close();
    });
  });
  gql_explore_->Hide();
  row->Add(gql_explore_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
  row->Add(gql_fetch_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
  sizer->Add(row, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(4));
  gql_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { gql_lookup(); });
  gql_age_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { update_gql_chip(); });
  update_gql_chip();
}

void HttpEditor::gql_explore() {
  if (gql_explore_pop_) {
    gql_explore_pop_->close();
    return;
  }
  if (!gql_schema_) return;
  const GqlSchema& sc = *gql_schema_;
  std::vector<PickItem> items;
  auto add = [&](const GqlType* t) {
    if (!t) return;
    for (const GqlField& f : t->fields) {
      if (items.size() >= 4000) return;
      std::string d;
      if (!f.args.empty()) {
        d = "(";
        for (size_t i = 0; i < f.args.size(); ++i) d += (i ? ", " : "") + f.args[i].name + ": " + f.args[i].type;
        d += ")";
      }
      d += ": " + f.type;
      if (f.deprecated) d += "  deprecated";
      if (!f.desc.empty()) {
        std::string first = f.desc.substr(0, f.desc.find('\n'));
        if (first.size() > 70) first = first.substr(0, 67) + "...";
        d += "  \xE2\x80\x94 " + first;
      }
      // "name \xC2\xB7 Type": typing a field name finds it first, the type tells same-named ones apart.
      items.push_back({wxString::FromUTF8(f.name + " \xC2\xB7 " + t->name), wxString::FromUTF8(d), wxString::FromUTF8(t->name)});
    }
  };
  for (const std::string* root : {&sc.query, &sc.mutation, &sc.subscription})
    if (!root->empty()) add(sc.type(*root));
  for (const auto& [name, t] : sc.types)
    if ((t.kind == "OBJECT" || t.kind == "INTERFACE") && name.rfind("__", 0) != 0 && name != sc.query &&
        name != sc.mutation && name != sc.subscription)
      add(&t);
  if (items.empty()) {
    note("The schema has no fields to show");
    return;
  }
  auto* p = new PickPopup(gql_explore_, std::move(items), "");
  gql_explore_pop_ = p;
  p->on_close = [this, p] {
    if (gql_explore_pop_ == p) gql_explore_pop_ = nullptr;
  };
  p->on_pick = [this](const wxString& v) {
    const std::string tf = v.utf8_string();
    const size_t dot = tf.find(" \xC2\xB7 ");
    if (dot != std::string::npos) gql_insert_field(tf.substr(dot + 4), tf.substr(0, dot));
  };
  gql_explore_->SetFocus();  // it forwards the keys to the list
  const wxRect b = gql_explore_->GetScreenRect();
  const int w = FromDIP(520);
  p->show_below(wxRect(b.GetRight() - w, b.y, w, b.height + FromDIP(2)));
}

void HttpEditor::gql_insert_field(const std::string& type, const std::string& field) {
  if (!gql_query_ || !gql_schema_) return;
  const GqlType* t = gql_schema_->type(type);
  const GqlField* f = t ? t->field(field) : nullptr;
  if (!f) return;
  std::string text = f->name;
  int caret = -1;
  std::string args;
  for (const GqlArg& a : f->args)
    if (!a.type.empty() && a.type.back() == '!' && a.default_value.empty()) {
      if (!args.empty()) args += ", ";
      args += a.name + ": ";
      if (caret < 0) caret = static_cast<int>(text.size() + 1 + args.size());
    }
  if (!args.empty()) text += "(" + args + ")";
  const GqlType* nt = gql_schema_->type(f->named);
  if (nt && nt->selectable()) {
    text += " {  }";
    if (caret < 0) caret = static_cast<int>(text.size()) - 2;
  }
  if (caret < 0) caret = static_cast<int>(text.size());
  wxStyledTextCtrl* s = gql_query_;
  const int at = s->GetCurrentPos();
  s->InsertText(at, wxString::FromUTF8(text));
  s->GotoPos(at + caret);
  s->SetFocus();
  note(wxString::FromUTF8("Inserted " + type + "." + field + ": " + f->type));
}

std::string HttpEditor::gql_current_key() const {
  const Workspace* ws = host_->workspace();
  return gql_cache_key(sent_url(), ws ? ws->active_env : std::string());
}

void HttpEditor::update_gql_chip() {
  if (!gql_chip_) return;
  const Theme& t = Theme::get();
  wxString text, tip;
  bool warn = false;
  if (gql_busy_) {
    text = wxString::FromUTF8("Fetching the schema\xE2\x80\xA6");
  } else if (!gql_error_.empty()) {
    // A failed refresh keeps the copy it had: say both.
    const std::string kept = gql_schema_ ? "; still using the copy fetched " + gql_age(epoch_now() - gql_time_) : "";
    text = wxString::FromUTF8("\xE2\x9A\xA0 " + gql_error_ + kept);
    tip = wxString::FromUTF8(gql_error_ + kept);
    warn = true;
  } else if (gql_schema_) {
    const size_t n = gql_schema_->user_type_count();
    text = wxString::FromUTF8(std::to_string(n) + (n == 1 ? " type" : " types") + " \xC2\xB7 fetched " +
                              gql_age(epoch_now() - gql_time_));
    tip = "Completion (Ctrl+Space) and the checks below use this copy of the schema (kept per URL and "
          "environment). Refresh fetches it again.";
  } else {
    text = "None: fetch it to complete fields and check the query";
  }
  gql_chip_->SetForegroundColour(warn ? t.var_bad : gql_schema_ ? t.fg : t.dim);
  if (gql_chip_->GetLabel() != text) gql_chip_->SetLabel(text);
  gql_chip_->SetToolTip(tip.empty() ? text : tip);
  if (gql_explore_ && gql_explore_->IsShown() != (gql_schema_ != nullptr)) {
    gql_explore_->Show(gql_schema_ != nullptr);
    gql_explore_->GetParent()->Layout();
  }
  if (gql_fetch_) {
    const wxString b = gql_schema_ ? "Refresh" : "Fetch schema";
    if (gql_fetch_->GetLabel() != b) {
      gql_fetch_->SetLabel(b);
      gql_fetch_->GetParent()->Layout();
    }
    gql_fetch_->Enable(!gql_busy_);
  }
  if (gql_schema_ && !gql_age_timer_.IsRunning()) gql_age_timer_.Start(30000);
}

void HttpEditor::gql_set_schema(std::shared_ptr<const GqlSchema> schema, const std::string& key, double when) {
  GqlCached& slot = gql_cache()[key];
  std::shared_ptr<const GqlSchema> old = std::move(slot.schema);
  slot = {schema, when};
  if (key != gql_key_) {  // the URL / environment changed meanwhile: kept for when it comes back
    release_schema(std::move(old));
    return;
  }
  std::shared_ptr<const GqlSchema> prev = std::move(gql_schema_);
  gql_schema_ = std::move(schema);
  release_schema(std::move(old));
  release_schema(std::move(prev));
  gql_time_ = when;
  gql_error_.clear();
  update_gql_chip();
  lint_gql();
}

void HttpEditor::gql_lookup() {
  if (!gql_query_) return;
  const std::string key = gql_current_key();
  if (key == gql_key_ && gql_schema_) return;
  if (key != gql_key_) {
    gql_key_ = key;
    gql_error_.clear();
  }
  if (const auto it = gql_cache().find(key); it != gql_cache().end()) {
    gql_set_schema(it->second.schema, key, it->second.when);
    return;
  }
  const bool had = gql_schema_ != nullptr;
  release_schema(std::move(gql_schema_));  // another endpoint's would complete and flag the wrong fields
  update_gql_chip();
  if (had) lint_gql();
  // The copy kept in <ws>/.watt/graphql/ (read and parsed on a worker, once per key).
  const Workspace* ws = host_->workspace();
  if (!ws || gql_disk_tried_ == key) return;
  gql_disk_tried_ = key;
  const std::filesystem::path file = ws->root() / L".watt" / L"graphql" / gql_cache_file_name(key);
  std::thread([alive = alive_, file, key] {
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec || size == 0 || size > (64u << 20)) return;
    const auto mtime = std::filesystem::last_write_time(file, ec);
    std::string text;
    {
      std::ifstream in(file, std::ios::binary);
      if (!in) return;
      text.resize(static_cast<size_t>(size));
      in.read(text.data(), static_cast<std::streamsize>(size));
      text.resize(static_cast<size_t>(in.gcount()));
    }
    std::string err;
    std::shared_ptr<const GqlSchema> schema = parse_gql_schema(text, &err);
    if (!schema) return;
    double when = epoch_now();
    if (!ec) {
      const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(mtime);
      when = std::chrono::duration<double>(sys.time_since_epoch()).count();
    }
    std::lock_guard lk(alive->m);
    if (!alive->target) return;
    auto* ed = static_cast<HttpEditor*>(alive->target);
    ed->CallAfter([ed, schema, key, when] {
      if (gql_cache().count(key)) return;  // fetched meanwhile: that one is newer
      ed->gql_set_schema(schema, key, when);
    });
  }).detach();
}

void HttpEditor::gql_fetch() {
  if (gql_busy_) return;
  if (trim(url_text()).empty()) {
    gql_error_ = "enter the GraphQL endpoint URL first";
    update_gql_chip();
    return;
  }
  const Workspace* ws = host_->workspace();
  // The request as it is, sent as the introspection query: the same headers, auth (OAuth tokens
  // included), options and {{vars}} as a send, inherited from its folders like a send.
  auto req = std::make_shared<SavedRequest>(base_);
  store(*req);
  req->file = host_->inherit_file();
  req->kind = RequestKind::Http;
  req->http.method = "POST";
  req->form.clear();
  req->body_file.clear();
  req->body_data.clear();
  req->http.body = graphql_body(graphql_introspection_query(), "", "IntrospectionQuery");
  auto& hs = req->http.headers;
  hs.erase(std::remove_if(hs.begin(), hs.end(), [](const Header& h) { return iequals(trim(h.first), "Content-Type"); }),
           hs.end());
  hs.emplace_back("Content-Type", "application/json");
  if (std::none_of(hs.begin(), hs.end(), [](const Header& h) { return iequals(trim(h.first), "Accept"); }))
    hs.emplace_back("Accept", "application/json");
  gql_key_ = gql_current_key();
  const std::string key = gql_key_;
  auto wsc = ws ? std::make_shared<Workspace>(*ws) : nullptr;
  const uint64_t gen = ++gql_gen_;
  gql_busy_ = true;
  gql_error_.clear();
  update_gql_chip();
  std::thread([alive = alive_, req, wsc, key, gen] {
    const auto t0 = std::chrono::steady_clock::now();
    HttpRequest hr;
    std::string err;
    if (wsc) {
      // The gate for every consent (uploads, OS environment variables, Windows sign-in, network
      // settings): what it refuses is asked about on the UI thread, then fetched again.
      auto r = wsc->http_to_send(*req, &err);
      if (!r) {
        std::lock_guard lk(alive->m);
        if (!alive->target) return;
        auto* ed = static_cast<HttpEditor*>(alive->target);
        ed->CallAfter([ed, req, err, gen] {
          if (gen != ed->gql_gen_) return;
          ed->gql_busy_ = false;
          ed->gql_fetch_blocked(req, err);
        });
        return;
      }
      hr = std::move(*r);
    } else {
      hr = req->http;
    }
    HttpResponse resp;
    {
      HttpClient client;
      resp = client.send(hr);
    }
    std::shared_ptr<const GqlSchema> schema;
    if (!resp.ok) {
      err = resp.error.empty() ? "the request failed" : resp.error;
    } else {
      std::string perr;
      schema = parse_gql_schema(resp.body, &perr);
      if (!schema)
        err = (resp.status >= 300 ? "HTTP " + std::to_string(resp.status) + ": " : std::string()) + perr;
    }
    // A copy in <ws>/.watt/graphql/ for the next session (only into an existing .watt: it is
    // created owner-only by the workspace, never here).
    if (schema && wsc) {
      std::error_code ec;
      const std::filesystem::path dir = wsc->root() / L".watt";
      if (std::filesystem::is_directory(dir, ec)) {
        const std::filesystem::path sub = dir / L"graphql";
        std::filesystem::create_directory(sub, ec);
        const std::filesystem::path file = sub / gql_cache_file_name(key);
        const std::filesystem::path tmp = sub / (gql_cache_file_name(key) + ".tmp");
        {
          std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
          if (out) out.write(resp.body.data(), static_cast<std::streamsize>(resp.body.size()));
        }
        std::filesystem::rename(tmp, file, ec);
        if (ec) std::filesystem::remove(tmp, ec);
      }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const double net_ms = resp.timing.total_ms;
    const size_t bytes = resp.body.size();
    std::lock_guard lk(alive->m);
    if (!alive->target) return;
    auto* ed = static_cast<HttpEditor*>(alive->target);
    ed->CallAfter([ed, schema, err, key, gen, ms, net_ms, bytes] {
      if (gen != ed->gql_gen_) return;
      ed->gql_busy_ = false;
      ed->gql_consented_ = false;
      if (!schema) {
        ed->gql_error_ = err;
        ed->update_gql_chip();
        return;
      }
      ed->gql_set_schema(schema, key, epoch_now());
      ed->note(wxString::Format("Schema: %d types (%.1f KB) in %.1f ms, %.1f ms of it on the network (off the UI thread)",
                                static_cast<int>(schema->user_type_count()), static_cast<double>(bytes) / 1024.0, ms,
                                net_ms));
    });
  }).detach();
}

// http_to_send() refused the introspection request: the consents a send would ask for, asked in
// the editor's banner (the safe choice is the default), then the fetch runs again and the worker
// checks again. Anything else (or a second refusal) is shown as the error.
void HttpEditor::gql_fetch_blocked(std::shared_ptr<SavedRequest> req, const std::string& err) {
  const Workspace* ws = host_->workspace();
  const bool asked = gql_consented_;
  gql_consented_ = false;
  std::vector<std::filesystem::path> files;
  std::vector<std::string> env, sso;
  std::vector<Workspace::NetworkItem> net;
  if (ws && !asked) {
    files = ws->blocked_files(*req);
    env = ws->blocked_env_vars(*req);
    sso = ws->blocked_sso_hosts(*req);
    net = ws->blocked_network_settings(*req);
  }
  if (!net.empty()) {
    gql_error_ = "not fetched: " + net[0].text + " (set in the " + net[0].level +
                 ") needs your approval; send the request once to review it";
    update_gql_chip();
    return;
  }
  if (files.empty() && env.empty() && sso.empty()) {
    gql_error_ = err.empty() ? std::string("not fetched") : err;
    update_gql_chip();
    return;
  }
  gql_error_ = "not fetched: it needs your approval (the banner above)";
  update_gql_chip();
  const Theme& t = Theme::get();
  ensure_banner();
  banner_missing_.clear();
  long_line_banner_ = false;
  banner_->set_colours(t.warn_bg, t.warn_border);
  std::string text;
  for (const auto& f : files) text += "\xE2\x80\xA2 uploads " + path_utf8(f.filename()) + "   " + path_utf8(f) + "\n";
  for (const auto& e : env) text += "\xE2\x80\xA2 reads the environment variable " + e + " from your system\n";
  for (const auto& h : sso)
    text += "\xE2\x80\xA2 signs in to " + h + " with your Windows account (no user name given; only for servers you trust)\n";
  text += "Nothing has been sent. The choice is remembered for this workspace (in your user settings, never in "
          "the workspace).";
  set_banner_texts("Fetching the schema sends this request's data", wxString::FromUTF8(text));
  banner_action_->SetLabel("Allow and fetch schema");
  banner_action_->Show();
  banner_cancel_->SetLabel("Cancel");
  banner_on_action_ = [this, files, env, sso] {
    const Workspace* w = host_->workspace();
    if (!w) return;
    std::string e;
    for (const auto& f : files)
      if (!w->allow_file(f, &e)) return show_info("Could not allow the file", wxString::FromUTF8(e.empty() ? path_utf8(f) : e));
    for (const auto& v : env)
      if (!w->allow_env_var(v, &e))
        return show_info("Could not allow the environment variable", wxString::FromUTF8(e.empty() ? v : e));
    for (const auto& h : sso)
      if (!w->allow_sso_host(h, &e)) return show_info("Could not allow Windows sign-in", wxString::FromUTF8(e.empty() ? h : e));
    hide_banner();
    gql_consented_ = true;
    gql_fetch();  // the worker checks again
  };
  banner_->Show();
  Layout();
  wrap_banner();
  banner_cancel_->SetFocus();  // the safe choice is the default
}

// Completion in the query: what gql_complete() offers at the caret, in a themed list under the
// word (each item with its type and the start of its description, like Postman / Insomnia).
// Built once per list and refilled as the word grows: a keystroke costs the walk (microseconds)
// and one repaint.
void HttpEditor::gql_completion(bool explicit_request) {
  wxStyledTextCtrl* s = gql_query_;
  const int len = s->GetLength();
  if (len > 256 * 1024) return;
  const int pos = s->GetCurrentPos();
  const std::string_view doc(s->GetCharacterPointer(), static_cast<size_t>(len));
  GqlCompletion c = gql_complete(doc, static_cast<size_t>(pos), gql_schema_.get());
  const std::string typed(doc.substr(c.from, static_cast<size_t>(pos) - c.from));
  if (c.items.empty() || (c.items.size() == 1 && c.items[0].name == typed && !explicit_request)) {
    gql_close_pop();
    if (explicit_request && c.items.empty() && !gql_schema_ && c.kind == GqlCompletion::Kind::None)
      note("Fetch the schema (above the query) to complete fields and arguments");
    return;
  }
  if (c.items.size() > 300) c.items.resize(300);
  std::vector<PickItem> items;
  items.reserve(c.items.size());
  for (const auto& i : c.items) {
    std::string d = i.detail;
    if (!i.desc.empty()) {
      std::string first = i.desc.substr(0, i.desc.find('\n'));
      if (first.size() > 56) first = first.substr(0, 53) + "...";
      d += (d.empty() ? "" : "   ") + first;
    }
    items.push_back({wxString::FromUTF8(i.name), wxString::FromUTF8(d), ""});
  }
  gql_items_ = std::move(c.items);
  gql_items_kind_ = c.kind;
  PickPopup* p = gql_pop_;
  if (!p) {
    p = new PickPopup(s, {}, "");
    p->compact = true;
    p->SetFont(s->StyleGetFont(wxSTC_STYLE_DEFAULT));
    gql_pop_ = p;
    p->on_close = [this, p] {
      if (gql_pop_ == p) gql_pop_ = nullptr;
    };
  }
  p->on_pick = [this](const wxString& v) { gql_pick(v); };
  p->set_items(std::move(items), "");
  if (gql_pop_ != p) return;
  // Under the word being typed (screen coordinates), at least as wide as 18 characters.
  const wxPoint at = s->PointFromPosition(static_cast<int>(c.from));
  const int lh = s->TextHeight(s->LineFromPosition(pos));
  const wxRect r(s->ClientToScreen(at), wxSize(s->TextWidth(wxSTC_STYLE_DEFAULT, "MMMMMMMMMMMMMMMMMM"), lh));
  p->show_below(r);
}

void HttpEditor::gql_close_pop() {
  if (gql_pop_) gql_pop_->close();  // its on_close clears gql_pop_
}

// A pick: the word at the caret becomes the name; an argument / input field gets its ": ".
void HttpEditor::gql_pick(const wxString& name) {
  wxStyledTextCtrl* s = gql_query_;
  const int pos = s->GetCurrentPos();
  int from = pos;
  while (from > 0) {
    const int ch = s->GetCharAt(from - 1);
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_')) break;
    --from;
  }
  int to = pos;  // the rest of the word after the caret is replaced too
  for (;;) {
    const int ch = s->GetCharAt(to);
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_')) break;
    ++to;
  }
  wxString text = name;
  const bool keyed = gql_items_kind_ == GqlCompletion::Kind::Argument || gql_items_kind_ == GqlCompletion::Kind::InputField;
  if (keyed) {
    int p = to;
    while (s->GetCharAt(p) == ' ') ++p;
    if (s->GetCharAt(p) != ':') text += ": ";
  }
  s->SetTargetRange(from, to);
  s->ReplaceTarget(text);
  s->GotoPos(from + static_cast<int>(text.utf8_string().size()));
}

// '}' ')' ']' '"' typed right before the closer auto-close put there: step over it.
bool HttpEditor::gql_key(wxKeyEvent& e) {
  wxStyledTextCtrl* s = gql_query_;
  const int k = e.GetUnicodeKey();
  if ((k != '}' && k != ')' && k != ']' && k != '"') || !s->GetSelectionEmpty()) return false;
  const int pos = s->GetCurrentPos();
  if (s->GetCharAt(pos) != k || s->IndicatorValueAt(kIndAutoClose, pos) == 0) return false;
  gql_close_pop();
  s->SetIndicatorCurrent(kIndAutoClose);
  s->IndicatorClearRange(pos, 1);
  s->GotoPos(pos + 1);
  return true;
}

void HttpEditor::gql_char_added(int key) {
  wxStyledTextCtrl* s = gql_query_;
  const bool word = (key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') || key == '_';
  if (key == '{' || key == '(' || key == '[' || key == '"') {
    // Auto-close (as the JSON body does): not inside a string or a comment, and only where the
    // next character does not continue a token.
    if (!s->GetSelectionEmpty() || s->GetSelections() != 1) return;
    const int pos = s->GetCurrentPos();
    const int ls = s->PositionFromLine(s->LineFromPosition(pos));
    if (pos - ls > 4096) return;
    const wxCharBuffer before = s->GetTextRangeRaw(ls, pos - 1);
    bool in_str = false;
    for (size_t i = 0; i < before.length(); ++i) {
      if (before[i] == 0x5C && in_str) ++i;
      else if (before[i] == '"') in_str = !in_str;
      else if (before[i] == '#' && !in_str) return;  // a comment
    }
    if (in_str) return;
    if (key == '"' && pos >= 2 && s->GetCharAt(pos - 2) == '"') return;  // """block strings"""
    const int next = s->GetCharAt(pos);
    const bool ok_next = next == 0 || next == ' ' || next == 9 || next == 13 || next == 10 || next == ',' ||
                         next == '}' || next == ')' || next == ']';
    if (!ok_next) return;
    const char closer[2] = {key == '{' ? '}' : key == '(' ? ')' : key == '[' ? ']' : '"', 0};
    s->InsertTextRaw(pos, closer);
    s->SetIndicatorCurrent(kIndAutoClose);
    s->IndicatorFillRange(pos, 1);
    return;
  }
  if (key == '\n') {
    // After the script editor's own indentation: "{|}" Enter puts the closer on its own line and
    // the caret indented between them (Monaco / Postman).
    CallAfter([this] {
      wxStyledTextCtrl* q = gql_query_;
      if (!q) return;
      const int line = q->GetCurrentLine();
      if (line <= 0) return;
      const int prev_end = q->GetLineEndPosition(line - 1);
      const int prev_start = q->PositionFromLine(line - 1);
      int p = prev_end - 1;
      while (p >= prev_start && (q->GetCharAt(p) == ' ' || q->GetCharAt(p) == '\t' || q->GetCharAt(p) == '\r')) --p;
      if (p < prev_start) return;
      const int open = q->GetCharAt(p);
      if (open != '{' && open != '(' && open != '[') return;
      const int base = q->GetLineIndentation(line - 1);
      q->SetLineIndentation(line, base + 2);
      const int caret = q->GetLineIndentPosition(line);
      q->GotoPos(caret);
      const int next = q->GetCharAt(caret);
      if ((open == '{' && next == '}') || (open == '(' && next == ')') || (open == '[' && next == ']')) {
        q->InsertText(caret, "\n" + wxString(' ', static_cast<size_t>(base)));
        q->GotoPos(caret);
      }
    });
    return;
  }
  if (!word && key != '$') {
    gql_close_pop();
    return;
  }
  if (key == '$' || (word && gql_pop_)) {
    gql_completion(false);
  } else if (word) {
    const int pos = s->GetCurrentPos();
    if (pos - s->WordStartPosition(pos, true) == 1) gql_completion(false);  // the first letter typed
  }
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
  if (!text_mode(body_mode_) || body_->GetReadOnly()) return false;
  // A paste over a big selection (Ctrl+A, Ctrl+V on a 10 MB body): Scintilla would delete the
  // old text here line by line (~0.3 s for 1M lines, more with folds); the result is built as
  // a new document on a worker and swapped in instead, like Beautify (Ctrl+Z swaps back).
  const int sel0 = body_->GetSelectionStart(), sel1 = body_->GetSelectionEnd();
  const bool big_sel = body_->GetSelections() == 1 && !body_->SelectionIsRectangle() &&
                       (sel1 - sel0 >= (4 << 20) || (sel1 - sel0 >= (1 << 20) && body_->GetLineCount() >= 100000));
  if (body_mode_ != BodyMode::Json && !big_sel) return false;
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
      if (big_sel || (n >= kLongLine / 3 && first < n && (p[first] == '{' || p[first] == '[')))
        w = std::make_shared<std::wstring>(p, n);
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
  // The text around a big selection (one copy each; nothing for a whole-body selection).
  std::shared_ptr<const std::string> before, after;
  if (big_sel && !whole) {
    const char* doc = body_->GetCharacterPointer();
    before = std::make_shared<const std::string>(doc, static_cast<size_t>(start));
    after = std::make_shared<const std::string>(doc + end, static_cast<size_t>(len - end));
  }
  const bool json = body_mode_ == BodyMode::Json;
  std::thread([alive = alive_, w, gen, edits, start, end, whole, big_sel, before, after, json] {
    auto text = std::make_shared<std::string>();
    if (!w->empty()) {
      const int n = ::WideCharToMultiByte(CP_UTF8, 0, w->data(), static_cast<int>(w->size()), nullptr, 0, nullptr, nullptr);
      text->resize(static_cast<size_t>(std::max(n, 0)));
      ::WideCharToMultiByte(CP_UTF8, 0, w->data(), static_cast<int>(w->size()), text->data(), n, nullptr, nullptr);
    }
    const bool long_json = json && looks_like_json(*text) && longest_line(*text) >= kLongLine;
    if (!long_json) {  // inserted as text: CRLF -> LF, as Scintilla's paste does
      size_t o = 0;
      for (size_t i = 0; i < text->size(); ++i)
        if (!((*text)[i] == 13 && i + 1 < text->size() && (*text)[i + 1] == 10)) (*text)[o++] = (*text)[i];
      text->resize(o);
    }
    // Into part of a body: re-indented here, so the UI thread only inserts it.
    if (long_json && !whole) *text = reindent_json(*text);
    if (big_sel && !(long_json && whole) && before) {
      std::string full;
      full.reserve(before->size() + text->size() + after->size());
      full.append(*before).append(*text).append(*after);
      *text = std::move(full);
    }
    std::shared_ptr<const std::string> ctext = std::move(text);
    std::lock_guard lk(alive->m);
    if (!alive->target) return;
    auto* ed = static_cast<HttpEditor*>(alive->target);
    ed->CallAfter([ed, gen, edits, start, end, whole, ctext, long_json, big_sel] {
      if (big_sel && !(long_json && whole)) ed->paste_swap(gen, edits, start, ctext);
      else ed->paste_arrived(gen, edits, start, end, whole, ctext, long_json);
    });
  }).detach();
  return true;
}

bool HttpEditor::copy_big_selection() {
  const int s = body_->GetSelectionStart(), e = body_->GetSelectionEnd();
  if (e - s < (1 << 20) || body_->GetSelections() != 1 || body_->SelectionIsRectangle()) return false;
  const auto t0 = Clock::now();
  // One contiguous copy (a memcpy, a few ms for 20 MB); the worker converts it.
  auto text = std::make_shared<const std::string>(body_->GetCharacterPointer() + s, static_cast<size_t>(e - s));
  const double ui_ms = ms_between(t0, Clock::now());
  note(wxString::FromUTF8("Copying " + std::string(human_size(text->size()).utf8_string()) + "\xE2\x80\xA6"));
  std::thread([alive = alive_, text, ui_ms] {
    const auto w0 = Clock::now();
    const int n = MultiByteToWideChar(CP_UTF8, 0, text->data(), static_cast<int>(text->size()), nullptr, 0);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (static_cast<size_t>(n) + 1) * sizeof(wchar_t));
    if (h) {
      auto* p = static_cast<wchar_t*>(GlobalLock(h));
      MultiByteToWideChar(CP_UTF8, 0, text->data(), static_cast<int>(text->size()), p, n);
      p[n] = 0;
      GlobalUnlock(h);
    }
    const double worker_ms = ms_between(w0, Clock::now());
    const size_t bytes = text->size();
    std::lock_guard lk(alive->m);
    auto* ed = static_cast<HttpEditor*>(alive->target);
    if (!ed || !h) {
      if (h) GlobalFree(h);
      return;
    }
    ed->CallAfter([ed, h, bytes, worker_ms, ui_ms] {
      const auto t1 = Clock::now();
      bool ok = false;
      if (OpenClipboard(static_cast<HWND>(ed->GetHWND()))) {
        EmptyClipboard();
        ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
        CloseClipboard();
      }
      if (!ok) GlobalFree(h);  // the clipboard owns it only once set
      if (ok)
        ed->note(wxString::Format("Copied %s (%.0f ms on a worker, UI %.1f ms)", human_size(bytes), worker_ms,
                                  ui_ms + ms_between(t1, Clock::now())));
      else
        ed->note("Copy failed: the clipboard is in use by another program", true);
    });
  }).detach();
  return true;
}

void HttpEditor::paste_swap(uint64_t gen, uint64_t edits, int caret, std::shared_ptr<const std::string> body) {
  if (gen != beautify_gen_ || edits != body_edits_ || pending_body_ || body_->GetReadOnly()) {
    note("Paste dropped: the body changed while it was read", true);
    return;
  }
  swap_note_ = wxString::Format("Pasted over the selection (%s body, built on a worker). Ctrl+Z undoes.",
                                human_size(body->size()));
  swap_caret_ = caret;
  swap_note_gen_ = gen;
  fill_body_doc(std::move(body), true, gen, edits, 0);
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

void HttpEditor::load_formatted(std::shared_ptr<const std::string> text, uint64_t gen) {
  raw_on_load_ = text;
  note(wxString::FromUTF8("Formatting the one-line JSON body\xE2\x80\xA6"));
  std::thread([alive = alive_, text, gen] {
    const auto t0 = Clock::now();
    auto out = std::make_shared<const std::string>(reindent_json(*text));
    const double ms = ms_between(t0, Clock::now());
    std::lock_guard lk(alive->m);
    auto* ed = static_cast<HttpEditor*>(alive->target);
    if (!ed) return;
    ed->CallAfter([ed, out, gen, ms] {
      if (gen != ed->body_load_gen_ || !ed->pending_body_) return;  // another request loaded meanwhile
      ed->fill_body_doc(out, false, gen, 0, ms);
    });
  }).detach();
}

// Builds a Scintilla document from `text` (re-indented first when `reindent`) on a worker thread
// and swaps it in when done, if nothing changed meanwhile (see swap_body_doc).
void HttpEditor::build_body_doc(std::shared_ptr<const std::string> text, bool reindent, uint64_t gen, uint64_t edits) {
  if (reindent) {
    // Re-indent first, then make the loader: an already formatted body then never creates (and
    // releases) a Scintilla document at all, and the view is left exactly as it is.
    std::thread([alive = alive_, text, gen, edits] {
      const auto t0 = Clock::now();
      auto out = std::make_shared<const std::string>(reindent_json(*text));
      const bool same = *out == *text;
      const double ms = ms_between(t0, Clock::now());
      std::lock_guard lk(alive->m);
      auto* ed = static_cast<HttpEditor*>(alive->target);
      if (!ed) return;
      ed->CallAfter([ed, out, same, gen, edits, ms] {
        if (gen != ed->beautify_gen_ || edits != ed->body_edits_ || ed->pending_body_) return;  // edited meanwhile
        // A paste (raw_pending_) is still inserted when it needed no formatting.
        if (same && !ed->raw_pending_) {
          ed->note("Already formatted");
          ed->body_->Refresh(false);
          return;
        }
        ed->fill_body_doc(out, true, gen, edits, ms);
      });
    }).detach();
    return;
  }
  fill_body_doc(std::move(text), false, gen, edits, 0);
}

// A Scintilla document filled with `src` on a worker (the loader is made here, on the UI thread),
// then swapped in by swap_body_doc().
void HttpEditor::fill_body_doc(std::shared_ptr<const std::string> src, bool reindent, uint64_t gen, uint64_t edits,
                               double worker_ms) {
  const size_t hint = std::min<size_t>(src->size(), 0x7FFFFFF0);
  auto* loader = static_cast<SciLoader*>(body_->CreateLoader(static_cast<int>(hint)));
  if (!loader) {  // cannot happen with Scintilla 5; keep the body usable anyway
    if (!reindent) {
      pending_body_.reset();
      body_->SetReadOnly(false);
      body_->AddTextRaw(src->data(), static_cast<int>(src->size()));
    }
    return;
  }
  std::thread([alive = alive_, src, reindent, gen, edits, loader, worker_ms] {
    const auto t0 = Clock::now();
    const size_t head = fill_head(*src);
    for (size_t off = 0; off < head; off += size_t{1} << 20)
      loader->AddData(src->data() + off, static_cast<ptrdiff_t>(std::min(head - off, size_t{1} << 20)));
    const double ms = worker_ms + ms_between(t0, Clock::now());
    std::lock_guard lk(alive->m);
    if (!alive->target) {
      loader->Release();
      return;
    }
    void* doc = loader->ConvertToDocument();
    alive->docs.push_back(doc);
    auto* ed = static_cast<HttpEditor*>(alive->target);
    ed->CallAfter([ed, doc, reindent, gen, edits, ms, src, head] {
      ed->swap_body_doc(doc, reindent, gen, edits, ms, src, head);
    });
  }).detach();
}

void HttpEditor::swap_body_doc(void* doc, bool reindent, uint64_t gen, uint64_t edits, double worker_ms,
                               std::shared_ptr<const std::string> src, size_t head) {
  const size_t bytes = src->size();
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
  ++st_swaps_;
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
  drop_redo_doc();
  const auto ti = Clock::now();
  install_doc(doc, true);
  st_install_ms_ = ms_between(ti, Clock::now());
  body_->ReleaseDocument(doc);  // the control holds its own reference now
  pending_body_.reset();
  if (!reindent && raw_on_load_) {
    raw_body_ = std::move(raw_on_load_);
    raw_edits_ = body_edits_;
    minify_->set_value(true);
    if (!minify_->IsShown()) {
      minify_->Show();
      fit_body_tools();
    }
    note(wxString::Format("One-line JSON body (%s) shown formatted (%.0f ms on a worker); sent and saved as it was "
                          "(Send compact).",
                          human_size(raw_body_->size()), worker_ms));
  }
  if (reindent) {
    ++body_edits_;
    if (raw_pending_) {
      raw_body_ = std::move(raw_pending_);
      raw_edits_ = body_edits_;
      minify_->set_value(true);
      if (!minify_->IsShown()) {
        minify_->Show();
        fit_body_tools();
      }
      note(wxString::Format("Pasted %s of one-line JSON: shown formatted (%s, %.0f ms on a worker, UI %.1f ms); "
                            "sent as pasted. Ctrl+Z undoes.",
                            human_size(raw_body_->size()), human_size(bytes), worker_ms, ms_between(t0, Clock::now())));
    } else if (!swap_note_.empty() && swap_note_gen_ == gen) {
      note(swap_note_);
      if (swap_caret_ >= 0) body_->GotoPos(std::min(swap_caret_, body_->GetLength()));
    } else {
      note(wxString::Format("Formatted %s in %.0f ms (UI %.1f ms)", human_size(bytes), worker_ms,
                            ms_between(t0, Clock::now())));
    }
    swap_note_.clear();
    swap_caret_ = -1;
    touched();
  }
  if (head < bytes) start_fill(std::move(src), head);
  st_swap_ms_ = ms_between(t0, Clock::now());
}

void HttpEditor::start_fill(std::shared_ptr<const std::string> src, size_t head) {
  fill_src_ = std::move(src);
  fill_off_ = head;
  fill_doc_ = body_->GetDocPointer();
  fill_ui_ms_ = fill_max_ms_ = 0;
  fill_slices_ = 0;
  // Makes Scintilla's per-line fold table now, for the head's lines only (hides nothing).
  body_->HideLines(1, 0);
  body_->SetUndoCollection(false);
  body_->SetReadOnly(true);
  // What store() sends meanwhile: the bytes as pasted / loaded when sent that way, else the text.
  pending_body_ = raw_body_ && raw_edits_ == body_edits_ && minify_->value() ? raw_body_ : fill_src_;
  fill_pending_ = pending_body_;
  lint_timer_.Stop();
  wxWakeUpIdle();
  fill_timer_.StartOnce(40);
}

// The fill and fold slices run here: wx sends idle events until no more are requested, and stops
// for any input or paint message in between, so a slice never holds back a keystroke for long.
void HttpEditor::update_crumb() {
  if (!crumb_) return;
  auto* tab = dynamic_cast<RequestTab*>(host_);
  if (!tab) return;
  const std::filesystem::path& file = tab->file();
  const bool dirty = tab->dirty();
  const auto fu = file.u8string();
  std::string sig(reinterpret_cast<const char*>(fu.data()), fu.size());
  sig += dirty ? "\x01" : "\x02";
  sig += base_.name;
  if (sig == crumb_sig_) return;
  crumb_sig_ = std::move(sig);
  // "users / admin / Get user by id": the folders under the workspace, then the name.
  std::string text;
  if (file.empty()) {
    text = "Unsaved request";
  } else if (file.parent_path().filename() == L"snapshots" && file.parent_path().parent_path().filename() == L".watt") {
    // A history snapshot: where it came from, not watt's internal folder.
    text = "History  /  ";
    std::string name = base_.name;
    if (name.empty()) {
      const auto u = file.stem().u8string();
      name.assign(reinterpret_cast<const char*>(u.c_str()), u.size());
    }
    text += name;
  } else {
    if (const Workspace* ws = host_->workspace()) {
      std::error_code ec;
      const auto rel = std::filesystem::relative(file.parent_path(), ws->root(), ec);
      if (!ec && !rel.empty() && rel.native() != L".")
        for (const auto& part : rel) {
          const auto u = part.u8string();
          text.append(reinterpret_cast<const char*>(u.c_str()), u.size());
          text += "  /  ";
        }
    }
    std::string name = base_.name;
    if (name.empty()) {
      const auto u = file.filename().u8string();
      name.assign(reinterpret_cast<const char*>(u.c_str()), u.size());
      if (const size_t dot = name.find(".req.json"); dot != std::string::npos) name.erase(dot);
    }
    text += name;
  }
  if (dirty) text += "  \xE2\x97\x8F";  // the tab's unsaved mark
  const wxString label = wxString::FromUTF8(text);
  crumb_->SetLabel(label);
  crumb_->SetToolTip(file.empty() ? wxString("Not saved to a file yet: Save (Ctrl+S) picks a folder and a name")
                                  : wxString(file.wstring()) + (dirty ? "\nUnsaved changes" : ""));
  crumb_->SetForegroundColour(dirty ? Theme::get().fg : Theme::get().dim);
  save_btn_->SetLabel(file.empty() ? wxString::FromUTF8("Save\xE2\x80\xA6") : dirty ? wxString("Save") : wxString("Saved"));
  save_btn_->set_text_colour(dirty || file.empty() ? Theme::get().accent : Theme::get().dim);
  save_btn_->GetContainingSizer()->Layout();
}

void HttpEditor::save_now() {
  // The frame's Ctrl+S command (whatever id its accelerator table gives it): it stores this
  // editor, asks for a name and folder the first time, and refreshes the tab and the tree.
  wxWindow* top = wxGetTopLevelParent(this);
  const wxAcceleratorTable* acc = top ? top->GetAcceleratorTable() : nullptr;
  int id = wxID_NONE;
  if (acc && acc->IsOk()) {
    const auto h = static_cast<HACCEL>(acc->GetHACCEL());
    const int n = ::CopyAcceleratorTableW(h, nullptr, 0);
    std::vector<ACCEL> v(static_cast<size_t>(std::max(n, 0)));
    if (n > 0 && ::CopyAcceleratorTableW(h, v.data(), n) == n)
      for (const ACCEL& a : v)
        if ((a.fVirt & FVIRTKEY) && (a.fVirt & FCONTROL) && !(a.fVirt & (FSHIFT | FALT)) && a.key == 'S') id = a.cmd;
  }
  if (id == wxID_NONE) {
    note("Save with Ctrl+S", false);
    return;
  }
  wxCommandEvent ev(wxEVT_MENU, id);
  ev.SetEventObject(top);
  top->GetEventHandler()->ProcessEvent(ev);
  crumb_sig_.clear();
  update_crumb();
}

void HttpEditor::on_idle(wxIdleEvent& e) {
  e.Skip();
  if (IsShownOnScreen()) update_crumb();
  if (fill_src_) run_fill();
  else if (!fold_ops_.empty()) run_fold_ops();
  if (fill_src_ || !fold_ops_.empty()) {
    e.RequestMore();
  } else if (prebuild_at_ != std::chrono::steady_clock::time_point{}) {
    if (std::chrono::steady_clock::now() >= prebuild_at_) {
      prebuild_at_ = {};
      prebuild_step();
    }
  } else if (!prebuild_done_ && (IsShownOnScreen() || parked())) {
    schedule_prebuild(400);  // a tab that was in the background when its steps came due
  }
}

bool HttpEditor::parked() const {
  // In the notebook (an open tab, shown or not): built when shown. Outside it (main_frame's
  // hidden spare): built while it waits, as long as the window itself is up.
  const wxWindow* top = wxGetTopLevelParent(const_cast<HttpEditor*>(this));
  if (!top || !top->IsShown()) return false;
  if (const auto* f = wxDynamicCast(top, wxTopLevelWindow); f && f->IsIconized()) return false;
  for (const wxWindow* w = GetParent(); w && w != top; w = w->GetParent())
    if (wxDynamicCast(w, wxAuiNotebook)) return false;
  return true;
}

void HttpEditor::finish_side_step() {
  if (torn_ || !tests_page_ || !script_tabs_) return;
  const size_t side = script_tabs_->selection();
  split_sides_ = true;
  bool more = false;
  if (side == 0) {
    if (!pre_) build_pre_side();
    else if (!pre_js_) build_pre_script();
    more = !pre_js_;
  } else {
    if (!asserts_) build_post_side();
    else if (!captures_) build_post_captures();
    else if (!post_js_) build_post_script();
    more = !post_js_;
  }
  split_sides_ = false;
  if (more) side_timer_.StartOnce(1);
}

void HttpEditor::schedule_prebuild(int ms) {
  prebuild_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  prebuild_timer_.StartOnce(ms + 5);
}

void HttpEditor::prebuild_step() {
  LASTINPUTINFO li{sizeof(li), 0};
  const bool typing = ::GetLastInputInfo(&li) && ::GetTickCount() - li.dwTime < 300;
  if (!IsShownOnScreen() && !parked()) return;  // on_idle starts again once the tab is shown (no polling)
  if (typing || fill_src_ || !fold_ops_.empty() || wxGetMouseState().LeftIsDown()) {
    schedule_prebuild(250);
    return;
  }
  const auto t0 = Clock::now();
  const char* what = nullptr;
  // A built page is laid out at the book's size too, so its first show only paints.
  auto lay_out = [this](wxPanel* page) {
    if (page->GetSize() != book_->GetClientSize()) page->SetSize(book_->GetClientSize());
    page->Layout();
  };
  if (!tests_page_) {
    want_side_ = 3;
    ensure_page(kTabTests);  // the shell; its prebuild_timer_ start brings the sides
    want_side_ = 2;
    what = "tests";
  } else if (!pre_) {
    split_sides_ = true;
    build_pre_side();
    split_sides_ = false;
    what = "pre";
  } else if (!pre_js_) {
    build_pre_script();
    what = "pre script";
  } else if (!asserts_) {
    split_sides_ = true;
    build_post_side();
    split_sides_ = false;
    what = "post";
  } else if (!captures_) {
    build_post_captures();
    what = "post captures";
  } else if (!post_js_) {
    build_post_script();
    what = "post script";
  } else if (!tests_laid_out_) {
    tests_laid_out_ = true;
    if (book_->GetSelection() != kTabTests) {
      lay_out(tests_holder_);
      layout_tests_page();
    }
    what = "tests layout";
  } else if (!timeout_) {
    defer_net_ = book_->GetSelection() != kTabOptions;
    ensure_page(kTabOptions);
    defer_net_ = false;
    if (book_->GetSelection() != kTabOptions) lay_out(opts_holder_);
    what = "options";
  } else if (!net_panel_) {
    build_net_panel(true);  // three steps: a section each
    if (book_->GetSelection() != kTabOptions) lay_out(opts_holder_);
    what = "options net";
  } else {
    prebuild_done_ = true;
    return;
  }
  prebuild_times_ += std::string(what) + " " + std::to_string(ms_between(t0, Clock::now())).substr(0, 4) + "  ";
  schedule_prebuild(60);
}

void HttpEditor::run_fill() {
  if (!fill_src_) return;
  if (body_->GetDocPointer() != fill_doc_ || pending_body_ != fill_pending_) {  // replaced meanwhile
    end_fill(false);
    return;
  }
  const auto t0 = Clock::now();
  const std::string& s = *fill_src_;
  size_t end = std::min(s.size(), fill_off_ + fill_step_);
  if (end < s.size()) {  // whole lines
    const void* nl = std::memchr(s.data() + end, '\n', s.size() - end);
    end = nl ? static_cast<size_t>(static_cast<const char*>(nl) - s.data()) + 1 : s.size();
  }
  // Not an edit: body_edits_ (and what is keyed on it) stays; end_fill counts the fill as one.
  const uint64_t edits = body_edits_;
  const bool was = loading_;
  loading_ = true;
  body_->SetReadOnly(false);
  body_->AppendTextRaw(s.data() + fill_off_, static_cast<int>(end - fill_off_));
  body_->SetReadOnly(true);
  loading_ = was;
  body_edits_ = edits;
  fill_off_ = end;
  const double ms = ms_between(t0, Clock::now());
  fill_ui_ms_ += ms;
  fill_max_ms_ = std::max(fill_max_ms_, ms);
  ++fill_slices_;
  // ~6 ms per slice (input and paints get in between), between 128 KB and 4 MB.
  const double next = static_cast<double>(fill_step_) * std::clamp(6.0 / std::max(ms, 0.25), 0.5, 1.5);
  fill_step_ = static_cast<size_t>(std::clamp(next, 128.0 * 1024, 4.0 * 1024 * 1024));
  if (fill_off_ >= s.size()) {
    end_fill(true);
  } else {
    wxWakeUpIdle();
    fill_timer_.StartOnce(40);  // when no idle time comes (a hidden tab)
  }
}

void HttpEditor::end_fill(bool done) {
  fill_timer_.Stop();
  if (!fill_src_) return;
  if (body_->GetDocPointer() == fill_doc_) {
    body_->SetUndoCollection(true);
    body_->EmptyUndoBuffer();
    body_->SetSavePoint();
  }
  if (done) {
    body_->SetReadOnly(false);
    if (pending_body_ == fill_pending_) pending_body_.reset();
    const bool raw_current = raw_body_ && raw_edits_ == body_edits_;
    ++body_edits_;  // the text grew: find snapshots and fold ops made meanwhile are stale
    if (raw_current) raw_edits_ = body_edits_;
    lint_timer_.StartOnce(50);
  }
  fill_src_.reset();
  fill_pending_.reset();
  fill_doc_ = nullptr;
}

bool HttpEditor::undo_swap() {
  if (!prev_doc_ || body_->CanUndo() || pending_body_) return false;
  void* cur = body_->GetDocPointer();
  body_->AddRefDocument(cur);
  void* back = prev_doc_;
  prev_doc_ = nullptr;
  drop_redo_doc();
  redo_doc_ = cur;  // kept with its undo / redo history for Ctrl+Y
  redo_minify_ = minify_->value();
  redo_raw_ = raw_body_ && raw_edits_ == body_edits_ ? raw_body_ : nullptr;
  install_doc(back, false);
  body_->ReleaseDocument(back);  // the control holds it now
  minify_->set_value(prev_minify_);
  raw_body_.reset();
  ++body_edits_;
  note("Undid the paste / format (Ctrl+Y redoes it)");
  touched();
  return true;
}

bool HttpEditor::redo_swap() {
  if (!redo_doc_ || body_->CanRedo() || pending_body_) return false;
  void* cur = body_->GetDocPointer();
  body_->AddRefDocument(cur);
  if (prev_doc_) release_doc_async(prev_doc_);
  prev_doc_ = cur;
  prev_minify_ = minify_->value();
  void* fwd = redo_doc_;
  redo_doc_ = nullptr;
  install_doc(fwd, false);
  body_->ReleaseDocument(fwd);
  minify_->set_value(redo_minify_);
  ++body_edits_;
  raw_body_ = std::move(redo_raw_);
  raw_edits_ = body_edits_;
  note("Redid the paste / format");
  touched();
  return true;
}

void HttpEditor::drop_redo_doc() {
  if (!redo_doc_) return;
  release_doc_async(redo_doc_);
  redo_doc_ = nullptr;
  redo_raw_.reset();
}

void HttpEditor::install_doc(void* doc, bool fresh) {
  end_fill(false);
  const bool was = loading_;
  loading_ = true;
  body_->SetReadOnly(false);
  body_->SetDocPointer(doc);
  body_->SetScrollWidth(1);
  if (fresh) {
    // A loader-built document has undo collection off: typing into it would leave nothing to
    // undo, and Ctrl+Z would throw the edits away with the paste.
    body_->SetUndoCollection(true);
    body_->EmptyUndoBuffer();
    body_->SetSavePoint();
  }
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
    std::string warning;
    const SavedRequest r = export_request(&warning);
    const std::string code = to_code(r, lang);
    if (wxTheClipboard->Open()) {
      wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(code)));
      wxTheClipboard->Close();
      if (!warning.empty()) note(wxString::FromUTF8(std::string("Copied ") + code_lang_name(lang) + ", but: " + warning), true);
      else note(wxString::Format("Copied %s, %zu chars", code_lang_name(lang), code.size()));
    } else {
      note("Could not open the clipboard", true);
    }
  }
}

void HttpEditor::copy_as_curl(CurlShell shell) {
  std::string warning;
  const SavedRequest r = export_request(&warning);
  const std::string cmd = to_curl_command(r, shell);
  std::string problem = curl_export_problem(r.http, r.form, shell);
  if (problem.empty()) problem = warning;
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
  const bool was_shown = note_->IsShown();
  const int old_h = note_->GetSize().y;
  note_->SetLabel(shown);
  if (!shown.empty()) note_->Wrap(std::max(FromDIP(200), GetClientSize().x - FromDIP(24)));
  note_->SetToolTip(text);
  if (was_shown != !text.empty()) note_->Show(!text.empty());
  if (was_shown != !text.empty() || (!text.empty() && note_->GetBestSize().y != old_h)) Layout();
  note_timer_.Stop();
  if (!text.empty()) note_timer_.StartOnce(warn ? 12000 : 3500);
}

// ------------------------------------------------------------------ {{vars}}

std::optional<std::string> HttpEditor::resolve(const std::string& name) const {
  if (pre_names_.count(name)) {  // set by a pre-request row right before the send
    const auto it = pre_preview_.find(name);
    if (it != pre_preview_.end() && it->second.ok) return it->second.text;
    return std::string("(computed before each send)");
  }
  const Workspace* ws = host_->workspace();
  if (!ws) return std::nullopt;
  const std::string ph = "{{" + name + "}}";
  std::string v = expand_span(*ws, ph);
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

// '}' / ']' typed on a blank line right above the closer that auto-close put there ("{" Enter,
// a member, Enter, "}"): the blank line goes and the caret steps over that closer, so the body
// keeps one closer at its indent (as Postman's Monaco does), not "  }" plus a stray "}".
bool HttpEditor::close_on_blank_line(int key) {
  if ((key != '}' && key != ']') || !body_->GetSelectionEmpty() || body_->GetSelections() != 1) return false;
  const int pos = body_->GetCurrentPos();
  const int line = body_->LineFromPosition(pos);
  if (line <= 0 || line + 1 >= body_->GetLineCount()) return false;
  const int ls = body_->PositionFromLine(line), le = body_->GetLineEndPosition(line);
  if (le - ls > 256) return false;
  for (int p = ls; p < le; ++p)
    if (const int c = body_->GetCharAt(p); c != ' ' && c != '\t') return false;
  const int q = body_->GetLineIndentPosition(line + 1);  // the next line's first non-blank
  if (body_->GetCharAt(q) != key || body_->IndicatorValueAt(kIndAutoClose, q) == 0) return false;
  if (body_->AutoCompActive()) body_->AutoCompCancel();
  body_->BeginUndoAction();
  const int from = body_->GetLineEndPosition(line - 1);
  body_->DeleteRange(from, le - from);  // the blank line, with the line break before it
  const int closer = q - (le - from);
  body_->SetIndicatorCurrent(kIndAutoClose);
  body_->IndicatorClearRange(closer, 1);
  body_->EndUndoAction();
  body_->GotoPos(closer + 1);
  return true;
}

// '}' / ']' typed as the first thing on its line: the line takes the indent of the line that
// opened the block (a closer is never left one level too deep).
void HttpEditor::dedent_closer() {
  const int pos = body_->GetCurrentPos();  // after the closer
  const int line = body_->LineFromPosition(pos);
  if (body_->GetLineIndentPosition(line) != pos - 1) return;
  int indent = -1;
  if (body_->GetLength() <= (2 << 20)) {  // the matching brace (a bounded scan on a big body)
    body_->Colourise(body_->PositionFromLine(line), pos);  // BraceMatch compares styles: style the new closer
    const int m = body_->BraceMatch(pos - 1);
    if (m >= 0 && m < pos - 1) indent = body_->GetLineIndentation(body_->LineFromPosition(m));
  }
  if (indent < 0) indent = std::max(0, body_->GetLineIndentation(line) - 2);
  if (indent == body_->GetLineIndentation(line)) return;
  body_->SetLineIndentation(line, indent);
  body_->GotoPos(body_->GetLineIndentPosition(line) + 1);
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
  show_var_list(s, {});
}

void HttpEditor::var_narrow(wxStyledTextCtrl* s) {
  const int pos = s->GetCurrentPos();
  auto var_char = [](int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                     c == '_' || c == '$' || c == '.' || c == '-'; };
  int a = pos;
  while (a > 0 && pos - a < 64 && var_char(s->GetCharAt(a - 1))) --a;
  if (a < 2 || a == pos || s->GetCharAt(a - 1) != '{' || s->GetCharAt(a - 2) != '{' ||
      (a >= 3 && s->GetCharAt(a - 3) == '{')) {
    if (s->AutoCompActive()) s->AutoCompCancel();
    return;
  }
  if (!host_->workspace()) return;
  show_var_list(s, s->GetTextRange(a, pos).utf8_string());
}

void HttpEditor::show_var_list(wxStyledTextCtrl* s, const std::string& typed) {
  auto lower = [](std::string v) {
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return v;
  };
  const std::string want = lower(typed);
  // Each entry shows the variable's value and where it comes from (Postman's popover):
  // "base   http://localhost:8080   environment". A pick inserts only the name (the
  // AUTOCOMP_SELECTION handler below); a secret's value is never shown.
  const Workspace* ws = host_->workspace();
  const Environment* env = nullptr;
  if (ws)
    for (const auto& e : const_cast<Workspace*>(ws)->environments())
      if (!ws->active_env.empty() && e.name == ws->active_env) env = &e;
  auto scope_of = [&](const std::string& n) -> std::string {
    if (pre_names_.count(n)) return "pre-request";
    if (!n.empty() && n[0] == '$') return "dynamic";
    if (!ws) return {};
    if (ws->runtime_vars.count(n)) return "runtime";
    for (const auto& [k, v] : inh_.vars)
      if (k == n) return "folder";
    if (env && env->vars.count(n)) return "env " + env->name;
    if (ws->globals.count(n)) return "globals";
    return {};
  };
  auto shown_value = [&](const std::string& n) -> std::string {
    if (!n.empty() && n[0] == '$') return n == "$guid" ? "a new UUID" : n == "$randomInt" ? "0..1000" : "now";
    if (env && env->secrets.count(n) && !ws->runtime_vars.count(n)) return "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2 (secret)";
    const auto v = resolve(n);
    if (!v) return "(empty)";
    std::string out;
    for (char c : *v) {
      if (out.size() >= 48) {
        out += "\xE2\x80\xA6";
        break;
      }
      out += static_cast<unsigned char>(c) < 32 ? ' ' : c;  // one line, no list separator
    }
    return out.empty() ? std::string("\"\"") : out;
  };
  std::string list;
  for (const auto& n : var_names()) {
    if (!want.empty() && lower(n).compare(0, want.size(), want) != 0) continue;
    std::string item = n + "    " + shown_value(n);
    if (const std::string sc = scope_of(n); !sc.empty()) item += "    \xC2\xB7 " + sc;
    list += (list.empty() ? "" : "\x1e") + item;
  }
  if (list.empty()) {
    if (s->AutoCompActive()) s->AutoCompCancel();
    return;
  }
  if (var_list_bound_.insert(s).second) {
    s->Bind(wxEVT_STC_AUTOCOMP_SELECTION, [this, s](wxStyledTextEvent& e) {
      const wxString item = e.GetText();
      const int sp = item.Find(' ');
      if (sp == wxNOT_FOUND || !s->AutoCompActive()) {
        e.Skip();
        return;
      }
      // Insert the name only (not the value and scope shown next to it): cancelling in this
      // handler stops Scintilla's own insertion.
      const wxString name = item.Left(static_cast<size_t>(sp));
      const int from = e.GetPosition(), to = s->GetCurrentPos();
      s->AutoCompCancel();
      s->SetTargetRange(from, to);
      s->ReplaceTarget(name);
      s->GotoPos(from + static_cast<int>(name.utf8_string().size()));
      CallAfter([this, s] { step_past_var_closer(s); });
    });
  }
  s->AutoCompSetIgnoreCase(true);
  // Scintilla finds the typed prefix by binary search: the "$..." entries come after the names,
  // so the list is unsorted and without this {{ba matched nothing and the list closed.
  s->AutoCompSetOrder(wxSTC_ORDER_PERFORMSORT);
  s->AutoCompSetAutoHide(true);
  s->AutoCompSetDropRestOfWord(false);
  s->AutoCompSetMaxHeight(10);
  s->AutoCompSetMaxWidth(0);
  s->AutoCompSetSeparator('\x1e');      // values may hold spaces
  s->AutoCompSetTypeSeparator('\x1f');  // and '?', Scintilla's default image marker
  s->AutoCompShow(static_cast<int>(typed.size()), wxString::FromUTF8(list));
  guard_autocomp(s);
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

// The "Not sent: {{name}} is not defined" banner. The shell's own check (inherited headers and
// auth, the bounced fast path) shows the same one, so one condition never has two UIs.
bool HttpEditor::show_unresolved(const std::vector<std::string>& missing, bool in_url,
                                 std::function<void()> send_anyway) {
  if (missing.empty()) return false;
  const Workspace* ws = host_->workspace();
  const std::string where = !ws || ws->active_env.empty() ? std::string("the globals (no environment is active)")
                                                          : "environment '" + ws->active_env + "' or the globals";
  std::string title = "Not sent: {{" + missing.front() + "}} is not defined in " + where;
  std::string text;
  if (missing.size() > 1) {
    text = "Also undefined:";
    for (size_t i = 1; i < missing.size(); ++i) text += " {{" + missing[i] + "}}";
    text += ". ";
  }
  // A URL with "{{name}}" in it is not a URL (no host, or curl refuses the braces): there is
  // nothing to send "as written", so the banner says why instead of offering a sure failure.
  if (in_url) send_anyway = nullptr;
  text += in_url ? "A URL with an undefined variable cannot be sent: define it in the environment, or remove it "
                   "from the URL."
                 : "Define it in the environment, or send it as written.";
  if (std::find(missing.begin(), missing.end(), "mock") != missing.end())
    text += " {{mock}} is the address of a running mock server: start it in its Mock tab (or right-click a "
            "folder > Start mock server).";
  // The usual case after a restart: a token a login request captured into a runtime variable.
  text += " Runtime variables (captures, scripts) last until watt closes: send the request that sets it first.";
  const Theme& t = Theme::get();
  std::function<void()> act;
  if (send_anyway)
    act = [this, f = std::move(send_anyway)] {
      hide_banner();
      f();
    };
  const wxString label = act ? wxString("Send as written") : wxString();  // before act is moved from
  show_info(wxString::FromUTF8(title), wxString::FromUTF8(text), label, std::move(act));
  banner_->set_colours(t.warn_bg, t.warn_border);
  banner_->Refresh();
  banner_missing_ = missing;
  return true;
}

void HttpEditor::do_send() {
  // Ctrl+Enter with a {{var}} list open: the list goes with the send (focus stays in the field).
  for (wxStyledTextCtrl* s : var_list_bound_)
    if (s->AutoCompActive()) s->AutoCompCancel();
  if (in_flight_) {
    host_->cancel();
    return;
  }
  if (pre_busy_ || minify_busy_) return;  // the send is on its way
  // A curl command typed (not pasted) into the URL bar: import it first, then send what it
  // describes (a paste imports at once, see url_inserted).
  if (const std::string typed = url_text(); looks_like_curl(typed)) {
    import_curl(std::string(trim(typed)));
    if (looks_like_curl(url_text())) return;  // not imported: the banner says why, nothing sent
  }
  // GraphQL variables that are not JSON: the server could not parse the body at all (it would
  // answer as if no variables were sent). Not sent; the lint under the editor says where.
  if (body_mode_ == BodyMode::GraphQL) {
    const std::string vars = gql_vars_text();
    if (!trim(vars).empty())
      if (const auto ji = check_json(vars)) {
        if (gql_vars_) {
          lint_gql_vars();
          gql_vars_->SetFocus();
          gql_vars_->GotoPos(static_cast<int>(std::min(ji->offset, vars.size())));
        }
        note(wxString::FromUTF8("Not sent: the GraphQL variables are not valid JSON (" + ji->message +
                                "). Fix them, or clear them to send none."),
             true);
        return;
      }
  }
  // A script that cannot parse fails in the sandbox anyway: say where now (the send goes on, and
  // its Log tab has the engine's own message).
  if (const wxString lint = script_lint_summary(); !lint.empty()) note(lint, true);
  url_->CallTipCancel();
  body_->CallTipCancel();
  // An edited big JSON body shown formatted is sent minified: minify it on a worker first (the
  // send follows when it is done), never 10 MB here.
  if (body_mode_ == BodyMode::Json && minify_ && minify_->value() && !pending_body_ &&
      body_->GetLength() >= 512 * 1024 && !(raw_body_ && raw_edits_ == body_edits_) && !minified_current()) {
    minify_then_send();
    return;
  }
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
    if (scheme_mode(auth_mode_)) {
      const HttpAuth& a = av_.adv;
      const OAuth2Config& o = a.oauth2;
      head += a.user + a.password + a.aws_access_key + a.aws_secret_key + a.aws_session_token + o.token_url +
              o.auth_url + o.client_id + o.client_secret + o.scope + o.audience + o.username + o.password;
    }
    const std::vector<PreVar> pre_rows = to_pre_vars(pre_now());
    for (const auto& pv : pre_rows) head += pv.expr;
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
      // A pre-request expression reading an OS environment variable: the same consent.
      if (env)
        for (auto& v : pre_env_refs(pre_rows, *ws))
          if (std::find(blocked_env.begin(), blocked_env.end(), v) == blocked_env.end()) blocked_env.push_back(std::move(v));
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
      show_unresolved(missing, in_url, [this, missing] {
        host_->accept_unresolved(missing);  // the shell's own check then lets them through too
        force_send_ = true;
        do_send();
        force_send_ = false;
      });
      // The response pane must not keep showing the last answer as if this one was sent.
      std::string names;
      for (const auto& n : missing) names += (names.empty() ? "{{" : ", {{") + n + "}}";
      host_->not_sent("Not sent: " + names + (missing.size() == 1 ? " is" : " are") + " not defined (" +
                      (ws->active_env.empty() ? std::string("no environment") : "environment " + ws->active_env) +
                      ").");
      return;
    }
  }
  // A {{var}} with a brace missing goes out literally: say where, before anything is sent.
  if (!force_send_) {
    if (const auto bad = brace_problems(); !bad.empty()) {
      std::string title = "Not sent: " + bad.front().first + " has a broken {{variable}}";
      std::string text = bad.front().second;
      if (bad.size() > 1) text += " (and " + std::to_string(bad.size() - 1) + " more)";
      text += ". Fix the braces, or send it as written.";
      const Theme& t = Theme::get();
      show_info(wxString::FromUTF8(title), wxString::FromUTF8(text), "Send as written", [this] {
        force_send_ = true;
        do_send();
        force_send_ = false;
      });
      banner_->set_colours(t.warn_bg, t.warn_border);
      banner_->Refresh();
      banner_missing_.clear();
      for (const auto& b : bad) banner_missing_.push_back("{}" + b.first);
      return;
    }
    // A header name that is not a token ("Authorization Bearer x", "Content-Type/json"): the
    // engine would squeeze it into some other name. Say so instead of sending it.
    for (const auto& row : header_rows()) {
      if (!row.enabled) continue;
      const std::string p = header_name_problem(row.key);
      if (p.empty()) continue;
      const Theme& t = Theme::get();
      show_info(wxString::FromUTF8("Not sent: header \"" + std::string(trim(row.key)) + "\" is not a valid name"),
                wxString::FromUTF8(p + "."), "Send anyway", [this] {
                  force_send_ = true;
                  do_send();
                  force_send_ = false;
                });
      banner_->set_colours(t.warn_bg, t.warn_border);
      banner_->Refresh();
      banner_missing_.clear();
      banner_missing_.push_back("{}header name " + row.key);
      return;
    }
  }
  // Pre-request variables: computed on a worker, stored, then this runs again to send.
  if (!pre_ready_ && ws) {
    auto rows = to_pre_vars(pre_now());
    if (std::any_of(rows.begin(), rows.end(), [](const PreVar& v) { return v.enabled && !trim(v.var).empty(); })) {
      run_pre_then_send(std::move(rows));
      return;
    }
  }
  hide_banner();
  if (body_mode_ == BodyMode::Json && lint_issue_)
    note(wxString::FromUTF8("Sent, but the JSON body is invalid: " + lint_issue_->message), true);
  sending_ = true;
  host_->send();
  sending_ = false;
  // The shell may ask to trust the scripts (asynchronously): the status is read back again when
  // the send starts and when the response arrives (set_in_flight), never left stale. The
  // response itself says when the scripts were not run.
  if (has_scripts()) refresh_trust();
}

void HttpEditor::show_upload_prompt(const std::vector<std::filesystem::path>& files,
                                    const std::vector<std::string>& env) {
  const Theme& t = Theme::get();
  ensure_banner();
  banner_missing_.clear();
  long_line_banner_ = false;
  banner_->set_colours(t.warn_bg, t.warn_border);
  std::string title, text;
  if (files.size() == 1 && env.empty()) {
    // The file name first (a long path cut at the end hid it); the full path is listed below.
    title = "This request uploads " + path_utf8(files[0].filename()) + " from outside the workspace";
  } else if (!files.empty()) {
    title = "This request uploads " + std::to_string(files.size()) + " files from outside the workspace";
  } else {
    title = env.size() == 1 ? "This request reads the environment variable " + env[0] + " from your system"
                            : "This request reads environment variables from your system";
  }
  if (!files.empty()) {
    for (const auto& f : files) text += "\xE2\x80\xA2 " + path_utf8(f.filename()) + "   " + path_utf8(f) + "\n";
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
  long_line_banner_ = false;
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
  long_line_banner_ = false;
  if (!banner_ || !banner_->IsShown()) return;
  banner_->Hide();
  banner_on_action_ = nullptr;
  Layout();
}

}  // namespace

// ------------------------------------------------------------------ tests (assert / capture)

wxWindow* HttpEditor::build_tests_page() {
  const Theme& t = Theme::get();
  auto* p = new wxPanel(tests_holder_);
  p->SetBackgroundColour(t.bg);
  auto* outer = new wxBoxSizer(wxVERTICAL);
  tests_notes_.clear();
  tests_wrap_ = -1;
  tests_page_ = p;
  // Pre-request | Post-response (Postman's Scripts tab, Bruno's Vars / Script / Assert).
  script_tabs_ = new TabStrip(p);
  script_tabs_->set_tabs({"Pre-request", "Post-response"});
  script_tabs_->Hide();  // the editor's Scripts / Tests tabs pick the side
  outer->Add(script_tabs_, 0, wxEXPAND | wxTOP, FromDIP(2));
  script_book_ = new wxSimplebook(p);
  script_book_->SetBackgroundColour(t.bg);
  outer->Add(script_book_, 1, wxEXPAND | wxTOP, FromDIP(6));
  // Each side is built when first shown, the other one a UI turn later: three tables and two
  // script editors at once cost ~50 ms.
  for (wxPanel** pg : {&pre_pg_, &post_pg_}) {
    *pg = new wxPanel(script_book_);
    (*pg)->SetBackgroundColour(t.bg);
  }
  script_book_->AddPage(pre_pg_, "pre");
  script_book_->AddPage(post_pg_, "post");
  p->SetSizer(outer);
  script_tabs_->on_select = [this](size_t i) {
    if (i == 0) build_pre_side();
    else build_post_side();
    script_book_->ChangeSelection(i);
    if (i == 0) preview_pre();
    layout_tests_page();
  };
  p->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
    e.Skip();
    CallAfter([this] { layout_tests_page(); });
  });
  // Open on the side that has something (pre-request when both or neither).
  const bool has_pre = !pre_vals_.empty() || !base_.pre_script.empty();
  const bool has_post = !assert_vals_.empty() || !capture_vals_.empty() || !base_.post_script.empty();
  // The side its tab asked for (Scripts / Tests), else the one with something in it; the other
  // side is built a UI turn later.
  const size_t side = want_side_ <= 1 ? want_side_ : has_post && !has_pre ? 1 : 0;
  const bool shell_only = want_side_ == 3;  // prebuild_step: the sides come in later steps
  want_side_ = 2;
  if (!shell_only) {
    if (side == 0) build_pre_side();
    else build_post_side();
  }
  script_tabs_->select(side);
  script_book_->ChangeSelection(side);
  // The other side a step later (each is 10-20 ms of control creation).
  schedule_prebuild(shell_only ? 60 : 30);
  update_script_badges();
  update_pre_names();
  update_tests_hint();
  layout_tests_page();
  refresh_trust();
  return p;
}

void HttpEditor::show_script_side(size_t side) {
  // At most the side's first table now; the rest in timer ticks (finish_side_step): the whole
  // side (two tables and a script editor) in one step was 40-55 ms on a first click.
  // A page built ahead (prebuild_step, also while the tab waits as a spare) is already laid out
  // at the book's size: then this only shows it.
  const void* had[2] = {pre_, asserts_};
  split_sides_ = true;
  want_side_ = side;
  ensure_page(kTabTests);
  want_side_ = 2;
  if (script_tabs_ && script_tabs_->selection() != side) {
    script_tabs_->select(side);
    script_book_->ChangeSelection(side);
  }
  if (side == 0) build_pre_side();
  else build_post_side();
  split_sides_ = false;
  book_->ChangeSelection(kTabTests);
  tabs_->select(side == 1 ? kStripTests : kStripScripts);
  if (had[side] != (side == 0 ? static_cast<const void*>(pre_) : static_cast<const void*>(asserts_))) layout_tests_page();
  if (side == 0 ? !pre_js_ : !post_js_) side_timer_.StartOnce(1);
  if (side == 0) preview_pre();
}


// Under each script: what it can do, when it runs, where its output shows, and the trust state.
void HttpEditor::add_trust_row(wxWindow* parent, wxSizer* s, int side) {
  const Theme& t = Theme::get();
  auto* row = new wxBoxSizer(wxHORIZONTAL);
  trust_text_[side] = new wxStaticText(parent, wxID_ANY, wxString::FromUTF8("Checking whether this workspace's scripts are trusted\xE2\x80\xA6"));
  trust_text_[side]->SetForegroundColour(t.dim);
  trust_btn_[side] = new FlatButton(parent, "Stop trusting", FlatButton::Style::Subtle);
  trust_btn_[side]->SetMinSize(trust_btn_[side]->GetBestSize());
  trust_btn_[side]->Hide();
  trust_btn_[side]->on_click = [this] { set_trust(trust_state_ != 1); };
  row->Add(trust_text_[side], 1, wxALIGN_CENTER_VERTICAL);
  row->Add(trust_btn_[side], 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
  s->Add(row, 0, wxEXPAND | wxTOP, FromDIP(4));
  if (trust_state_ >= 0) show_trust();
}

void HttpEditor::setup_tests_grid(KvGrid* g) {
  g->lookup = [this](const std::string& n) { return resolve(n); };
  g->var_names = [this] { return var_names(); };
  g->on_change = [this, g] {
    if (g == pre_) {
      update_pre_names();
      pre_timer_.StartOnce(250);
    }
    update_tests_hint();
    layout_tests_page();
    if (!loading_) touched();
  };
}

void HttpEditor::build_pre_side() {
  if (!pre_pg_) return;
  if (pre_) {
    if (!split_sides_) build_pre_script();
    return;
  }
  wxWindowUpdateLocker lock(pre_pg_);
  auto* ps = new wxBoxSizer(wxVERTICAL);
  // Pre-request: variables computed before the request is built, then the script.
  auto* pp = new KvPanel(pre_pg_, KvGrid::Kind::PreVar, "Variables");
  pre_ = pp->grid();
  pre_panel_ = pp;
  add_help(pp, "Pre-request variables",
           "Computed before each send, used as {{name}} in the URL, headers and body. Plain text works as is: "
           "{{userId}}-x\n\n"
           "Functions: hmacSha256(key, text[, \"base64\"]) hmacSha1 hmacSha512 sha256 sha1 sha512 md5 base64 base64url "
           "base64Decode hex urlencode urldecode lower upper trim replace substr length now() unix([+s]) iso([+s]) "
           "uuid() randomInt(lo, hi) randomHex(n) jsonPath({{json}}, \"$.a.b\")\n\n"
           "Inputs: 'text', {{var}}, req.method req.url req.body req.headers.<name>, joined with +\n\n"
           "Example: hmacSha256({{secret}}, req.method + req.url + {{ts}})");
  ps->Add(pp, 0, wxEXPAND);
  pre_sizer_ = ps;
  pre_pg_->SetSizer(ps);
  setup_tests_grid(pre_);
  pre_->check = [g = pre_](int col, const std::string& text) -> std::string {
    if (trim(text).empty()) return {};
    if (col == g->col_key()) return check_var_name(text);
    if (col == g->col_value()) return check_pre_expr(text);
    return {};
  };
  pre_->preview = [this](const std::string& var) -> std::optional<KvGrid::Preview> {
    const auto it = pre_preview_.find(var);
    if (it == pre_preview_.end()) return std::nullopt;
    return it->second;
  };
  const bool was = loading_;
  loading_ = true;
  std::vector<KvRow> pr;
  for (const auto& x : pre_vals_) pr.push_back({x.var, x.expr, x.enabled, false, false, {}, x.scope.empty() ? "runtime" : x.scope});
  pre_->set_rows(pr);
  loading_ = was;
  update_script_badges();
  update_pre_names();
  update_tests_hint();
  if (!split_sides_) build_pre_script();
  pre_pg_->Layout();
  layout_tests_page();
}

void HttpEditor::build_pre_script() {
  if (pre_js_ || !pre_sizer_) return;
  wxWindowUpdateLocker lock(pre_pg_);
  pre_sizer_->Add(make_script_label(pre_pg_, &pre_js_, false), 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(4));
  pre_js_ = make_script_editor(pre_pg_, "// pm. bru. req. complete as you type");
  setup_script_help(pre_js_, false);
  attach_find(pre_js_);
  pre_sizer_->Add(pre_js_, 1, wxEXPAND);
  add_trust_row(pre_pg_, pre_sizer_, 0);
  const bool was = loading_;
  loading_ = true;
  pre_js_->SetText(wxString::FromUTF8(base_.pre_script));
  pre_js_->EmptyUndoBuffer();
  loading_ = was;
  update_script_badges();
  pre_pg_->Layout();
  layout_tests_page();
}

void HttpEditor::build_post_side() {
  if (!post_pg_) return;
  if (asserts_) {
    if (!split_sides_) {
      build_post_captures();
      build_post_script();
    }
    return;
  }
  const Theme& t = Theme::get();
  wxWindowUpdateLocker lock(post_pg_);
  auto* qs = new wxBoxSizer(wxVERTICAL);
  // Post-response: assertions, captures, then the script.
  auto* ap = new KvPanel(post_pg_, KvGrid::Kind::Assert, "Assertions");
  asserts_ = ap->grid();
  assert_panel_ = ap;
  add_help(ap, "Assertions",
           "Checked on every response; pass / fail and the actual values show in the response's Tests tab.\n\n"
           "Targets: res.status, res.body.items[0].id, res.headers.content-type, res.responseTime, res.timing.ttfb, "
           "res.size");
  setup_tests_grid(asserts_);
  asserts_->check = [g = asserts_](int col, const std::string& text) -> std::string {
    if (col != g->col_key() || trim(text).empty()) return {};
    return check_target(std::string(trim(text)));
  };
  qs->Add(ap, 0, wxEXPAND | wxTOP, FromDIP(4));
  inh_asserts_box_ = new wxPanel(post_pg_);
  inh_asserts_box_->SetBackgroundColour(t.bg);
  inh_asserts_box_->SetSizer(new wxBoxSizer(wxVERTICAL));
  inh_asserts_box_->Hide();
  qs->Add(inh_asserts_box_, 0, wxEXPAND | wxTOP, FromDIP(2));
  // One line that keeps its row when empty, as the GraphQL lint: fixing an assertion never moves
  // the Captures table up under the mouse. The whole message is the tooltip.
  tests_hint_ = new wxStaticText(post_pg_, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                 wxST_NO_AUTORESIZE | wxST_ELLIPSIZE_END);
  tests_hint_->SetForegroundColour(t.var_bad);
  tests_hint_->SetMinSize(wxSize(-1, GetCharHeight()));
  qs->Add(tests_hint_, 0, wxEXPAND | wxTOP, FromDIP(4));
  post_sizer_ = qs;
  post_pg_->SetSizer(qs);
  const bool was = loading_;
  loading_ = true;
  std::vector<KvRow> ar;
  for (const auto& a : assert_vals_) ar.push_back({a.target, a.value, a.enabled, false, false, {}, a.op.empty() ? "eq" : a.op});
  asserts_->set_rows(ar);
  loading_ = was;
  update_script_badges();
  update_tests_hint();
  show_inherited_asserts();
  if (!split_sides_) {
    build_post_captures();
    build_post_script();
  }
  post_pg_->Layout();
  layout_tests_page();
}

void HttpEditor::build_post_captures() {
  if (captures_ || !post_sizer_) return;
  wxWindowUpdateLocker lock(post_pg_);
  auto* cp = new KvPanel(post_pg_, KvGrid::Kind::Capture, "Captures");
  captures_ = cp->grid();
  capture_panel_ = cp;
  add_help(cp, "Captures",
           "Copy a response value into a {{variable}} for later requests.\n\n"
           "Scope: runtime = this session only; environment / globals = saved there (a secret variable is never "
           "overwritten).");
  setup_tests_grid(captures_);
  captures_->check = [g = captures_](int col, const std::string& text) -> std::string {
    if (trim(text).empty()) return {};
    if (col == g->col_key()) return check_var_name(text);
    if (col == g->col_value()) return check_target(std::string(trim(text)));
    return {};
  };
  post_sizer_->Add(cp, 0, wxEXPAND | wxTOP, FromDIP(10));
  const bool was = loading_;
  loading_ = true;
  std::vector<KvRow> cr;
  for (const auto& c : capture_vals_) cr.push_back({c.var, c.target, c.enabled, false, false, {}, capture_scope_name(c.scope)});
  captures_->set_rows(cr);
  loading_ = was;
  update_script_badges();
  update_tests_hint();
  post_pg_->Layout();
  layout_tests_page();
}

void HttpEditor::build_post_script() {
  if (post_js_ || !post_sizer_) return;
  build_post_captures();  // above the script
  wxWindowUpdateLocker lock(post_pg_);
  post_sizer_->Add(make_script_label(post_pg_, &post_js_, true), 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(4));
  post_js_ = make_script_editor(post_pg_, "// pm.test('ok', () => pm.response.to.have.status(200))");
  setup_script_help(post_js_, true);
  attach_find(post_js_);
  post_sizer_->Add(post_js_, 1, wxEXPAND);
  add_trust_row(post_pg_, post_sizer_, 1);
  const bool was = loading_;
  loading_ = true;
  post_js_->SetText(wxString::FromUTF8(base_.post_script));
  post_js_->EmptyUndoBuffer();
  loading_ = was;
  update_script_badges();
  post_pg_->Layout();
  layout_tests_page();
}

void HttpEditor::show_inherited_asserts() {
  if (!inh_asserts_box_) return;
  std::vector<std::pair<std::string, std::string>> rows;
  for (const auto& c : inh_.asserts) {
    std::string text = c.assertion.target + " " + (c.assertion.op.empty() ? std::string("eq") : c.assertion.op);
    if (!c.assertion.value.empty()) text += " " + c.assertion.value;
    rows.emplace_back(std::move(text), c.from);
  }
  if (rows == inh_asserts_shown_) return;
  inh_asserts_shown_ = rows;
  const Theme& t = Theme::get();
  wxWindowUpdateLocker lock(inh_asserts_box_);
  inh_asserts_box_->DestroyChildren();
  wxSizer* s = inh_asserts_box_->GetSizer();
  s->Clear();
  for (size_t i = 0; i < rows.size(); ++i) {
    const auto& [text, from] = rows[i];
    // A fixed-size, ellipsized label: an auto-sized one would push the page's minimum width.
    auto* l = new wxStaticText(inh_asserts_box_, wxID_ANY,
                               wxString::FromUTF8("\xE2\x86\xB3 " + text + "   (from " + from + ")"),
                               wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE | wxST_ELLIPSIZE_END);
    l->SetMinSize(wxSize(FromDIP(80), l->GetBestSize().y));
    l->SetForegroundColour(t.var_ok);
    l->SetCursor(wxCursor(wxCURSOR_HAND));
    l->SetToolTip(wxString::FromUTF8("Inherited from " + from + ": checked on every response of this request, "
                                     "with the rows above. Click to edit the folder's assertions."));
    const std::filesystem::path folder = inh_.asserts[i].folder;
    l->Bind(wxEVT_LEFT_UP, [this, folder](wxMouseEvent&) {
      if (const Workspace* ws = host_->workspace()) edit_folder(folder.empty() ? ws->root() : folder);
    });
    s->Add(l, 0, wxEXPAND | wxLEFT, FromDIP(6));
  }
  inh_asserts_box_->Show(!rows.empty());
  if (post_pg_) post_pg_->Layout();
  layout_tests_page();
}

bool HttpEditor::has_scripts() const {
  const bool pre = pre_js_ ? pre_js_->GetLength() > 0 : !base_.pre_script.empty();
  const bool post = post_js_ ? post_js_->GetLength() > 0 : !base_.post_script.empty();
  return pre || post;
}

void HttpEditor::refresh_trust(bool after_send) {
  const Workspace* ws = host_->workspace();
  if (!ws) return;
  auto wsc = std::make_shared<Workspace>(*ws);
  const uint64_t gen = ++trust_gen_;
  std::thread([alive = alive_, wsc, gen, after_send] {
    const bool trusted = wsc->scripts_trusted();  // a small file read: never on the UI thread
    std::lock_guard lk(alive->m);
    if (!alive->target) return;
    auto* ed = static_cast<HttpEditor*>(alive->target);
    ed->CallAfter([ed, gen, trusted, after_send] {
      if (gen != ed->trust_gen_) return;
      ed->trust_state_ = trusted ? 1 : 0;
      ed->show_trust();
      if (after_send && !trusted && ed->has_scripts())
        ed->note("Scripts not run: this workspace's scripts are not trusted (Scripts tab: Trust scripts)", true);
    });
  }).detach();
}

void HttpEditor::show_trust() {
  const Theme& t = Theme::get();
  for (int i = 0; i < 2; ++i) {
    if (!trust_text_[i]) continue;
    wxString txt;
    if (trust_state_ == 1) {
      txt = "This workspace's scripts are trusted: they run on every send.";
    } else if (trust_state_ == 0) {
      txt = wxString::FromUTF8("Not trusted yet: the first send with a script asks; until then a send shows "
                               "\xE2\x80\x9Cscript not run\xE2\x80\x9D.");
    } else {
      continue;
    }
    trust_text_[i]->SetForegroundColour(trust_state_ == 1 ? t.dim : t.var_bad);
    if (trust_text_[i]->GetLabel() != txt) trust_text_[i]->SetLabel(txt);
    const int w = tests_wrap_ > 0 ? tests_wrap_ - trust_btn_[i]->GetBestSize().x - FromDIP(16) : FromDIP(600);
    trust_text_[i]->Wrap(std::max(w, FromDIP(200)));
    // Its full wrapped height: a min size kept from an earlier (shorter) label clipped the rest.
    trust_text_[i]->SetMinSize(wxSize(-1, trust_text_[i]->GetBestSize().y));
    trust_btn_[i]->SetLabel(trust_state_ == 1 ? "Stop trusting" : "Trust scripts");
    trust_btn_[i]->SetToolTip(trust_state_ == 1 ? "Stop running this workspace's scripts (kept in your user settings, outside the workspace)"
                                                : "Run this workspace's request scripts from now on (asks first)");
    trust_btn_[i]->Show();
    trust_text_[i]->GetParent()->Layout();
  }
}

void HttpEditor::set_trust(bool trusted) {
  const Workspace* ws = host_->workspace();
  if (!ws) return;
  if (trusted) {
    // The same question the first send asks (request_tab.cpp): scripts are code from whoever
    // wrote the workspace.
    const auto ans = wxMessageBox(
        wxString::Format("Run the request scripts of this workspace?\n\n%s\n\nScripts run in a sandbox (no file or "
                         "process access; network only through watt's own checks), but they can read and change "
                         "your requests and variables.",
                         ws->root().wstring()),
        "watt", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this);
    if (ans != wxYES) return;
  }
  auto wsc = std::make_shared<Workspace>(*ws);
  std::thread([alive = alive_, wsc, trusted] {
    std::string err;
    const bool ok = wsc->trust_scripts(trusted, &err);
    std::lock_guard lk(alive->m);
    if (!alive->target) return;
    auto* ed = static_cast<HttpEditor*>(alive->target);
    ed->CallAfter([ed, ok, err] {
      if (!ok) ed->note(wxString::FromUTF8("Could not save the script trust decision: " + err), true);
      ed->refresh_trust();
    });
  }).detach();
}

// A JavaScript editor (pre-request / post-response script): Scintilla's C++ lexer with JS
// keywords, themed like the body; edits mark the tab dirty.
wxStyledTextCtrl* HttpEditor::make_script_editor(wxWindow* parent, const char* placeholder) {
  const Theme& t = Theme::get();
  auto* s = new wxStyledTextCtrl(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(120)), wxBORDER_NONE);
  s->SetLexer(wxSTC_LEX_CPP);
  s->SetKeyWords(0, "async await break case catch class const continue default delete do else export extends "
                    "finally for function if in instanceof let new of return switch this throw try typeof var void "
                    "while yield true false null undefined");
  s->SetKeyWords(1, "pm bru req res console crypto CryptoJS expect test require JSON Math Date Object Array String "
                    "Number Boolean Promise setTimeout atob btoa");
  const wxFont f = mono_font(10);
  s->StyleSetFont(wxSTC_STYLE_DEFAULT, f);
  s->StyleSetBackground(wxSTC_STYLE_DEFAULT, t.field);
  s->StyleSetForeground(wxSTC_STYLE_DEFAULT, t.fg);
  s->StyleClearAll();
  s->StyleSetForeground(wxSTC_C_COMMENT, t.dim);
  s->StyleSetForeground(wxSTC_C_COMMENTLINE, t.dim);
  s->StyleSetForeground(wxSTC_C_COMMENTDOC, t.dim);
  s->StyleSetForeground(wxSTC_C_NUMBER, t.js_number);
  s->StyleSetForeground(wxSTC_C_STRING, t.js_string);
  s->StyleSetForeground(wxSTC_C_CHARACTER, t.js_string);
  s->StyleSetForeground(wxSTC_C_STRINGEOL, t.js_error);
  s->StyleSetForeground(wxSTC_C_WORD, t.js_keyword);
  s->StyleSetForeground(wxSTC_C_WORD2, t.js_key);
  s->StyleSetForeground(wxSTC_C_OPERATOR, t.js_punct);
  s->StyleSetForeground(wxSTC_C_REGEX, t.js_string);
  s->StyleSetForeground(wxSTC_STYLE_LINENUMBER, t.dim);
  s->StyleSetBackground(wxSTC_STYLE_LINENUMBER, t.field);
  s->SetMarginType(0, wxSTC_MARGIN_NUMBER);
  s->SetMarginWidth(0, s->TextWidth(wxSTC_STYLE_LINENUMBER, "_999"));
  for (int m = 1; m < 5; ++m) s->SetMarginWidth(m, 0);
  s->SetMarginLeft(FromDIP(4));
  s->SetCaretForeground(t.fg);
  s->SetSelBackground(true, t.dark ? wxColour(0x26, 0x4F, 0x78) : wxColour(0xAD, 0xD6, 0xFF));
  s->SetTabWidth(2);
  s->SetUseTabs(false);
  s->SetIndent(2);
  s->SetTabIndents(true);
  s->SetBackSpaceUnIndents(true);
  s->SetEOLMode(wxSTC_EOL_LF);  // Enter / paste give "\n": saved scripts have one kind of line end
  s->SetWrapMode(wxSTC_WRAP_WORD);  // no horizontal scroll bar on a short script editor
  s->SetUseHorizontalScrollBar(false);
  s->SetModEventMask(wxSTC_MOD_INSERTTEXT | wxSTC_MOD_DELETETEXT);
  const wxString ph = wxString::FromUTF8(placeholder);
  auto show_placeholder = [s, ph] {
    s->EOLAnnotationClearAll();
    if (s->GetLength() == 0) {
      s->EOLAnnotationSetText(0, ph);
      s->EOLAnnotationSetStyle(0, kStylePlaceholder);
    }
  };
  s->StyleSetForeground(kStylePlaceholder, t.dim);
  s->StyleSetBackground(kStylePlaceholder, t.field);
  s->StyleSetItalic(kStylePlaceholder, true);
  s->EOLAnnotationSetVisible(wxSTC_EOLANNOTATION_STANDARD);
  show_placeholder();
  s->Bind(wxEVT_STC_CHANGE, [this, show_placeholder](wxStyledTextEvent& e) {
    e.Skip();
    show_placeholder();
    if (loading_) return;
    update_script_badges();
    touched();
  });
  // Enter keeps the indentation of the line (Ctrl+Enter still sends).
  s->Bind(wxEVT_STC_CHARADDED, [s](wxStyledTextEvent& e) {
    e.Skip();
    if (e.GetKey() != '\n') return;
    const int line = s->GetCurrentLine();
    if (line <= 0) return;
    const int ind = s->GetLineIndentation(line - 1);
    if (ind <= 0) return;
    s->SetLineIndentation(line, ind);
    s->GotoPos(s->GetLineIndentPosition(line));
  });
  return s;
}

// "Script" with a Snippets button on the right (Postman's snippet list). `editor` is read when
// the button is used (the editor is created after this row).
wxWindow* HttpEditor::make_script_label(wxWindow* parent, wxStyledTextCtrl** editor, bool post) {
  const Theme& t = Theme::get();
  auto* row = new wxPanel(parent);
  auto* h = new wxBoxSizer(wxHORIZONTAL);
  auto* l = new wxStaticText(row, wxID_ANY, "Script");
  l->SetForegroundColour(t.dim);
  h->Add(l, 0, wxALIGN_CENTER_VERTICAL);
  h->Add(help_button(row, post ? "Post-response script" : "Pre-request script",
                     post ? "Runs on a worker thread once the response arrived, after the Captures above (so "
                            "pm.variables.get / bru.getVar see this response's captured values). "
                            "pm.test / test() results show next to "
                            "the assertions in the response's Tests tab, console output and errors in its Log tab; "
                            "pm.environment.set / bru.setVar values are kept like captures.\n\n"
                            "QuickJS sandbox (pm.* and bru.* APIs, crypto, CryptoJS, sendRequest): 1 s CPU, no files, "
                            "no OS access. pm. bru. res. complete as you type (Ctrl+Space); Snippets has examples."
                          : "Runs on Send, on a worker thread, after the variables above and before the request is "
                            "built: pm.request / req can change the method, URL, headers and body. console output and "
                            "errors go to the response's Log tab.\n\n"
                            "QuickJS sandbox (pm.* and bru.* APIs, crypto, CryptoJS, sendRequest): 1 s CPU, no files, "
                            "no OS access. pm. bru. req. complete as you type (Ctrl+Space); Snippets has examples."),
         0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
  h->AddStretchSpacer();
  auto* b = new FlatButton(row, "Snippets", FlatButton::Style::Subtle, true);
  b->SetToolTip(post ? "Insert a ready-made test or capture below the cursor's line"
                     : "Insert a ready-made pre-request step below the cursor's line");
  h->Add(b, 0, wxALIGN_CENTER_VERTICAL);
  row->SetSizer(h);
  b->on_click = [b, editor, post] {
    wxStyledTextCtrl* s = *editor;
    if (!s) return;
    const auto& list = js_snippets(post);
    wxMenu menu;
    for (size_t i = 0; i < list.size(); ++i) menu.Append(static_cast<int>(i) + 1, wxString::FromUTF8(list[i].title));
    const int sel = b->GetPopupMenuSelectionFromUser(menu, wxPoint(0, b->GetSize().y));
    if (sel < 1 || sel > static_cast<int>(list.size())) return;
    // After the caret's line when it has text, so a snippet never splits a statement.
    int pos = s->GetCurrentPos();
    const int line = s->LineFromPosition(pos);
    std::string code = list[static_cast<size_t>(sel - 1)].code;
    if (s->GetLineEndPosition(line) > s->PositionFromLine(line)) {
      pos = s->GetLineEndPosition(line);
      code = "\n" + code;
      code.pop_back();  // the line's own newline follows
    }
    s->InsertText(pos, wxString::FromUTF8(code));
    s->GotoPos(pos + static_cast<int>(code.size()));
    s->SetFocus();
  };
  return row;
}

FlatButton* HttpEditor::help_button(wxWindow* parent, const wxString& title, const wxString& text) {
  auto* b = new FlatButton(parent, "?", FlatButton::Style::Subtle);
  b->SetToolTip(title + ": reference");
  b->on_click = [this, b, title, text] {
    // Stays until Esc, a click elsewhere or the app losing focus (it is read, not glanced at); as
    // wide as the editor pane at most, below the button, never over the screen edge.
    const wxRect pane = GetScreenRect();
    const int width = std::clamp(pane.width - FromDIP(24), FromDIP(240), FromDIP(560));
    auto* pop = new HelpPopup(b, title, text, width);
    const wxSize sz = pop->GetSize();
    wxPoint at = b->ClientToScreen(wxPoint(0, b->GetSize().y + FromDIP(2)));
    at.x = std::max(pane.x + FromDIP(4), std::min(at.x, pane.GetRight() - sz.x - FromDIP(4)));
    if (const int d = wxDisplay::GetFromWindow(this); d != wxNOT_FOUND) {
      const wxRect area = wxDisplay(static_cast<unsigned>(d)).GetClientArea();
      if (at.y + sz.y > area.GetBottom()) at.y = std::max(area.y, b->GetScreenPosition().y - sz.y - FromDIP(2));
    }
    pop->Position(at, wxSize(0, 0));
    pop->Popup(pop->focus());
  };
  return b;
}

void HttpEditor::add_help(KvPanel* panel, const wxString& title, const wxString& text) {
  panel->title_row()->Insert(1, help_button(panel, title, text), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
}

void HttpEditor::setup_script_help(wxStyledTextCtrl* s, bool post) {
  const Theme& t = Theme::get();
  s->IndicatorSetStyle(kIndJsLint, wxSTC_INDIC_SQUIGGLE);
  s->IndicatorSetForeground(kIndJsLint, t.js_error);
  s->StyleSetForeground(kStyleJsLint, t.js_error);
  s->StyleSetBackground(kStyleJsLint, t.field);
  s->StyleSetFont(kStyleJsLint, GetFont());
  s->AnnotationSetVisible(wxSTC_ANNOTATION_STANDARD);
  s->AutoCompSetIgnoreCase(false);
  s->AutoCompSetAutoHide(true);
  s->AutoCompSetDropRestOfWord(false);
  s->AutoCompSetMaxHeight(10);
  s->AutoCompSetSeparator(' ');
  s->AutoCompSetOrder(wxSTC_ORDER_PERFORMSORT);
  s->Bind(wxEVT_STC_CHARADDED, [this, s, post](wxStyledTextEvent& e) {
    e.Skip();
    const int k = e.GetKey();
    const bool word_char = (k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9') || k == '_' || k == '$';
    if (k == '.') {
      script_completion(s, post, false);
    } else if (s->AutoCompActive() && word_char) {
      script_completion(s, post, true);  // the list narrows to what still matches
    } else if (!s->AutoCompActive() && ((k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z'))) {
      // A top-level name after two letters typed (pm, bru, req, res, console, ...).
      const int pos = s->GetCurrentPos();
      const int start = s->WordStartPosition(pos, true);
      if (pos - start == 2 && (start == 0 || s->GetCharAt(start - 1) != '.')) script_completion(s, post, false);
    }
  });
  s->Bind(wxEVT_KEY_DOWN, [this, s, post](wxKeyEvent& e) {
    if (e.GetKeyCode() == WXK_SPACE && e.GetModifiers() == wxMOD_CONTROL) {
      script_completion(s, post, true);
      return;
    }
    e.Skip();
  });
  // The lint runs 400 ms after the last edit (a script is small: microseconds).
  auto* timer = new wxTimer();
  timer->Bind(wxEVT_TIMER, [this, s](wxTimerEvent&) { lint_script(s); });
  s->Bind(wxEVT_DESTROY, [timer, s](wxWindowDestroyEvent& e) {
    e.Skip();
    if (e.GetEventObject() == s) {
      timer->Stop();
      delete timer;
    }
  });
  s->Bind(wxEVT_STC_CHANGE, [timer](wxStyledTextEvent& e) {
    e.Skip();
    timer->StartOnce(400);
  });
}

void HttpEditor::script_completion(wxStyledTextCtrl* s, bool post, bool explicit_request) {
  const int pos = s->GetCurrentPos();
  // Not inside a string or a comment.
  const int style = pos > 0 ? s->GetStyleAt(pos - 1) : 0;
  if (style == wxSTC_C_STRING || style == wxSTC_C_CHARACTER || style == wxSTC_C_COMMENT ||
      style == wxSTC_C_COMMENTLINE || style == wxSTC_C_COMMENTDOC || style == wxSTC_C_STRINGEOL)
    return;
  const int line_start = s->PositionFromLine(s->LineFromPosition(pos));
  const wxCharBuffer buf = s->GetTextRangeRaw(line_start, pos);
  const std::string_view text(buf.data(), buf.length());
  // "pm.res|": the word being typed, and the chain before its dot.
  size_t w = text.size();
  while (w > 0 && (std::isalnum(static_cast<unsigned char>(text[w - 1])) || text[w - 1] == '_' || text[w - 1] == '$')) --w;
  const size_t typed = text.size() - w;
  std::string chain;
  if (w > 0 && text[w - 1] == '.') {
    chain = js_chain_before(text, w - 1);
    if (chain.empty()) return;
  } else if (!explicit_request && typed < 2) {
    return;
  }
  const auto members = js_members(chain, post);
  if (members.empty()) return;
  const std::string_view word = text.substr(w);
  // Only the members that start with what is typed ("pm.request.he" lists headers alone).
  std::string list;
  for (const auto& m : members) {
    if (m.compare(0, word.size(), word) != 0) continue;
    list += (list.empty() ? "" : " ") + m;
  }
  if (list.empty() || list == word) {
    if (s->AutoCompActive()) s->AutoCompCancel();
    return;
  }
  s->AutoCompShow(static_cast<int>(typed), wxString::FromUTF8(list));
  guard_autocomp(s);
}

void HttpEditor::lint_script(wxStyledTextCtrl* s) {
  s->SetIndicatorCurrent(kIndJsLint);
  s->IndicatorClearRange(0, s->GetLength());
  s->AnnotationClearAll();
  if (s->GetLength() == 0 || s->GetLength() > 512 * 1024) return;
  const auto issue = js_lint(std::string_view(s->GetCharacterPointer(), static_cast<size_t>(s->GetLength())));
  if (!issue) return;
  const int pos = static_cast<int>(issue->pos);
  s->IndicatorFillRange(pos, 1);
  const int line = s->LineFromPosition(pos);
  s->AnnotationSetText(line, wxString::FromUTF8("\xE2\x9A\xA0 " + issue->message));
  s->AnnotationSetStyle(line, kStyleJsLint);
}

wxString HttpEditor::script_lint_summary() const {
  const auto check = [](const char* name, wxStyledTextCtrl* ed, const std::string& saved) -> wxString {
    const std::string_view src = ed ? std::string_view(ed->GetCharacterPointer(), static_cast<size_t>(ed->GetLength()))
                                    : std::string_view(saved);
    if (src.empty() || src.size() > 512 * 1024) return {};
    const auto issue = js_lint(src);
    if (!issue) return {};
    const auto line = 1 + std::count(src.begin(), src.begin() + static_cast<std::ptrdiff_t>(issue->pos), '\n');
    return wxString::Format("%s script, line %d: %s", name, static_cast<int>(line), wxString::FromUTF8(issue->message));
  };
  wxString w = check("Pre-request", pre_js_, base_.pre_script);
  if (w.empty()) w = check("Post-response", post_js_, base_.post_script);
  return w;
}

std::string HttpEditor::gql_query_text() const {
  if (!gql_query_) return gql_q_val_;
  return std::string(gql_query_->GetCharacterPointer(), static_cast<size_t>(gql_query_->GetLength()));
}

std::string HttpEditor::gql_vars_text() const {
  if (!gql_vars_) return gql_v_val_;
  return std::string(gql_vars_->GetCharacterPointer(), static_cast<size_t>(gql_vars_->GetLength()));
}

void HttpEditor::update_script_badges() {
  if (!script_tabs_) return;
  size_t npre = 0, npost = 0;
  if (pre_) {
    npre = pre_->enabled_count();
  } else {
    for (const auto& x : pre_vals_) npre += x.enabled && !x.var.empty();
  }
  npre += pre_js_ ? pre_js_->GetLength() > 0 : !base_.pre_script.empty();
  if (asserts_) {
    npost = asserts_->enabled_count();
    if (captures_) npost += captures_->enabled_count();
    else
      for (const auto& c : capture_vals_) npost += c.enabled && !c.var.empty();
  } else {
    for (const auto& a : assert_vals_) npost += a.enabled && !a.target.empty();
    for (const auto& c : capture_vals_) npost += c.enabled && !c.var.empty();
  }
  npost += post_js_ ? post_js_->GetLength() > 0 : !base_.post_script.empty();
  script_tabs_->set_badge(0, npre ? wxString::Format("%zu", npre) : wxString());
  script_tabs_->set_badge(1, npost ? wxString::Format("%zu", npost) : wxString(), tests_hint_ && !tests_hint_->GetLabel().empty());
}

// The tables as tall as their rows (capped), the problem line right under the assertions, the
// script editor taking what is left.
void HttpEditor::layout_tests_page() {
  if (!tests_page_) return;
  const wxSize cs = script_book_->GetClientSize();
  const int h = cs.y;
  if (h <= 0) return;
  if (cs.x > FromDIP(40) && cs.x != tests_wrap_) {
    tests_wrap_ = cs.x;
    for (auto& [n, text] : tests_notes_) {
      n->SetLabel(text);
      n->Wrap(cs.x - FromDIP(4));
    }
  }
  if (pre_panel_) pre_panel_->fit_rows(std::max(FromDIP(90), h * 2 / 5));
  if (assert_panel_ && capture_panel_) {
    // The two tables grow with their rows (no inner scroll bar while there is room) and share
    // what the script editor below does not need: it keeps a quarter of the page (at least
    // 120 px); the titles, the hint and the script label take about 90 px.
    const int script_min = std::max(FromDIP(120), h / 4);
    const int room = std::max(FromDIP(190), h - script_min - FromDIP(90));
    auto need = [](KvPanel* p) {
      KvGrid* g = p->grid();
      return g->GetColLabelSize() + g->GetNumberRows() * g->GetDefaultRowSize() + p->FromDIP(2);
    };
    const int na = need(assert_panel_), nc = need(capture_panel_);
    int ma = std::max(FromDIP(110), room * 3 / 5), mc = std::max(FromDIP(80), room - ma);
    if (na < ma) mc = std::max(mc, room - na);  // what the assertions leave
    if (nc < mc) ma = std::max(ma, room - nc);  // and the other way round
    assert_panel_->fit_rows(ma);
    capture_panel_->fit_rows(mc);
  }
  update_script_badges();
  for (size_t i = 0; i < script_book_->GetPageCount(); ++i) script_book_->GetPage(i)->Layout();
  tests_page_->Layout();
}

void HttpEditor::set_assert_rows(const std::vector<Assertion>& a) {
  if (!asserts_) return;
  std::vector<KvRow> rows;
  for (const auto& x : a) {
    const auto it = assert_descs_.find(x.target);
    rows.push_back({x.target, x.value, x.enabled, false, false, it == assert_descs_.end() ? std::string() : it->second,
                    x.op.empty() ? "eq" : x.op});
  }
  const bool was = loading_;
  loading_ = true;
  asserts_->set_rows(rows);
  loading_ = was;
  update_tests_hint();
  layout_tests_page();
}

std::vector<Assertion> HttpEditor::asserts_now() const {
  if (!asserts_) return assert_vals_;
  std::vector<Assertion> out;
  for (const auto& row : asserts_->rows()) {
    Assertion a;
    a.enabled = row.enabled;
    a.target = std::string(trim(row.key));
    a.op = row.type.empty() ? "eq" : row.type;
    a.value = assert_op_unary(a.op) ? std::string() : row.value;
    out.push_back(std::move(a));
  }
  return out;
}

std::vector<Capture> HttpEditor::captures_now() const {
  if (!captures_) return capture_vals_;
  std::vector<Capture> out;
  for (const auto& row : captures_->rows()) {
    Capture c;
    c.enabled = row.enabled;
    c.var = std::string(trim(row.key));
    c.target = std::string(trim(row.value));
    c.scope = capture_scope_from(row.type);
    out.push_back(std::move(c));
  }
  return out;
}

// The first problem of the tables, under the assertions (each bad cell is also drawn in the
// warning colour, with the message as its tooltip).
void HttpEditor::update_tests_hint() {
  if (!tests_hint_) return;
  wxString msg;
  int n = 0;
  auto scan = [&](KvGrid* g, const char* what) {
    if (!g) return;  // that side of the page is not built yet
    const auto rows = g->rows();
    // The cell being typed into is checked once its edit ends ("res.resp" is not an error yet).
    int live_col = -1;
    const int live = g->live_index(&live_col);
    for (size_t i = 0; i < rows.size(); ++i) {
      const bool typing = static_cast<int>(i) == live;
      std::string m;
      if (trim(rows[i].key).empty() && !typing) m = std::string(g == asserts_ ? "no target" : "no variable name") + ", not run";
      if (m.empty() && !(typing && live_col == g->col_key())) m = g->check(g->col_key(), rows[i].key);
      if (m.empty() && !(typing && live_col == g->col_value())) m = g->check(g->col_value(), rows[i].value);
      if (g == captures_ && m.empty() && !typing && trim(rows[i].value).empty()) m = "no response target to copy from";
      // "contains <empty>" always passes: an operator that compares needs an expected value.
      if (g == asserts_ && m.empty() && !typing && !trim(rows[i].key).empty() && rows[i].value.empty() &&
          !assert_op_unary(rows[i].type.empty() ? std::string_view("eq") : std::string_view(rows[i].type)))
        m = "no expected value (" + (rows[i].type.empty() ? std::string("eq") : rows[i].type) +
            " with nothing always passes or fails; isEmpty checks for an empty value)";
      if (g == pre_ && m.empty() && !typing && trim(rows[i].value).empty()) m = "no expression, not set";
      if (m.empty()) continue;
      if (++n == 1) msg = wxString::FromUTF8(std::string("\xE2\x9A\xA0 ") + what + " " + std::to_string(i + 1) + ": " + m);
    }
  };
  scan(pre_, "Pre-request");
  scan(asserts_, "Assertion");
  scan(captures_, "Capture");
  if (n > 1) msg += wxString::Format("  (+%d more)", n - 1);
  if (tests_hint_->GetLabel() == msg) return;
  tests_hint_->SetToolTip(msg);
  tests_hint_->SetLabel(msg);  // its row stays: nothing below it moves
}

// ------------------------------------------------------------------ cookies

wxWindow* HttpEditor::build_cookies_page() {
  const Theme& t = Theme::get();
  auto* p = new wxPanel(cookies_holder_);
  p->SetBackgroundColour(t.bg);
  auto* s = new wxBoxSizer(wxVERTICAL);
  auto* row = new wxBoxSizer(wxHORIZONTAL);
  cookie_head_ = new wxStaticText(p, wxID_ANY, "");
  cookie_head_->SetForegroundColour(t.dim);
  row->Add(cookie_head_, 1, wxALIGN_CENTER_VERTICAL);
  auto* add = new FlatButton(p, "Add cookie", FlatButton::Style::Subtle);
  add->SetToolTip("Add a cookie to the Cookie header (Headers tab)");
  row->Add(add, 0, wxALIGN_CENTER_VERTICAL);
  auto* manage = new FlatButton(p, wxString::FromUTF8("Manage cookies\xE2\x80\xA6"), FlatButton::Style::Outline);
  manage->SetToolTip("Every cookie in this workspace's jar: add, edit, delete, clear");
  row->Add(manage, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
  s->Add(row, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(6));
  jar_check_ = new CheckButton(p, "Use the cookie jar for this request");
  jar_check_->SetToolTip("Off: no jar cookies are sent with this request (Postman's \"Disable cookie jar\")");
  jar_check_->set_value(ov_.jar);
  jar_check_->on_change = [this] { set_use_jar(jar_check_->value()); };
  s->Add(jar_check_, 0, wxBOTTOM, FromDIP(8));
  manage->on_click = [this] {
    if (!jar_) {
      note("The cookie jar is still loading; try again in a moment.", false);
      refresh_jar();
      return;
    }
    // The app's one cookie manager (the sidebar's): modal, opened after the jar is loaded. Its
    // edits show here when the window is active again (below).
    sidebar::manage_cookies(wxGetTopLevelParent(this), jar_, url_host(sent_url()));
  };
  if (wxWindow* tlw = wxGetTopLevelParent(this)) {
    tlw->Bind(wxEVT_ACTIVATE, [this, w = wxWeakRef<wxWindow>(p)](wxActivateEvent& e) {
      e.Skip();
      // The frame is active again (the cookie manager or another window closed): the jar may have
      // changed. Only while this editor's Cookies tab is on screen.
      if (e.GetActive() && w && IsShownOnScreen() && book_->GetSelection() == kTabCookies) refresh_jar();
    });
  }
  auto* frame = new FramePanel(p, t.field, t.border);
  auto* fs = new wxBoxSizer(wxVERTICAL);
  cookie_list_ = new wxPanel(frame);
  cookie_list_->SetBackgroundColour(t.field);
  cookie_grid_ = new wxFlexGridSizer(3, FromDIP(6), FromDIP(20));
  cookie_grid_->AddGrowableCol(1, 1);
  cookie_list_->SetSizer(cookie_grid_);
  fs->Add(cookie_list_, 1, wxEXPAND | wxALL, FromDIP(10));
  frame->SetSizer(fs);
  s->Add(frame, 0, wxEXPAND);
  auto* foot = new wxStaticText(p, wxID_ANY,
                                "Jar cookies for this URL are added when it is sent; a name the Cookie header has keeps "
                                "the header's value.",
                                wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
  foot->SetForegroundColour(t.dim);
  foot->SetMinSize(wxSize(-1, GetCharHeight()));
  foot->SetToolTip(foot->GetLabel());
  s->Add(foot, 0, wxEXPAND | wxTOP, FromDIP(8));
  p->SetSizer(s);
  add->on_click = [this] {
    ensure_page(kTabHeaders);
    auto hrows = headers_->rows();
    int at = -1;
    for (size_t i = 0; i < hrows.size(); ++i)
      if (hrows[i].enabled && iequals(trim(hrows[i].key), "Cookie")) at = static_cast<int>(i);
    if (at < 0) {
      hrows.push_back({"Cookie", "", true, false, false});
      at = static_cast<int>(hrows.size()) - 1;
    } else if (!trim(hrows[static_cast<size_t>(at)].value).empty()) {
      hrows[static_cast<size_t>(at)].value = std::string(trim(hrows[static_cast<size_t>(at)].value)) + "; ";
    }
    headers_->set_rows(hrows);
    tabs_->select(strip_of(kTabHeaders));
    book_->ChangeSelection(kTabHeaders);
    update_auto_headers();
    touched();
    headers_->SetFocus();
    headers_->SetGridCursor(at, headers_->col_value());
    headers_->MakeCellVisible(at, headers_->col_value());
    headers_->CallAfter([g = headers_] {
      if (!g->IsCellEditControlEnabled() && g->CanEnableCellControl()) g->EnableCellEditControl();
    });
  };
  cookie_sig_.clear();
  return p;
}

// The cookies the request sends: each enabled Cookie header row split into its cookies
// ({{vars}} shown resolved), then the curl -b jars (read when sent, not here).
void HttpEditor::update_cookies() {
  if (!cookie_list_) return;
  const Workspace* ws = host_->workspace();
  std::vector<std::string> sig;
  struct Row {
    std::string name, value, source;
  };
  std::vector<Row> rows;
  for (const auto& r : header_rows()) {
    if (!r.enabled || !iequals(trim(r.key), "Cookie")) continue;
    for (auto& [n, v] : parse_cookie_header(r.value)) {
      std::string shown = v;
      if (ws && shown.find("{{") != std::string::npos) {
        const std::string x = ws->expand(shown);
        if (x != shown) shown += "  =  " + (x.size() > 120 ? x.substr(0, 120) + "\xE2\x80\xA6" : x);
      }
      rows.push_back({n, shown, "Cookie header"});
    }
  }
  for (const auto& f : base_.cookie_files) rows.push_back({"(cookie file)", f, "curl -b file, read when sent"});
  // The workspace jar's cookies for this URL (what Workspace::http_to_send adds).
  size_t jar_sent = 0, jar_off = 0;
  if (!jar_all_.empty()) {
    std::set<std::string> own;
    for (const auto& r : rows)
      if (r.source == "Cookie header") own.insert(r.name);
    const int64_t now = cookies::now();
    for (const auto& c : jar_matches()) {
      if (!ov_.jar) {
        ++jar_off;
        continue;
      }
      std::string src = "Cookie jar: " + std::string(c.host_only ? "" : ".") + c.domain + (c.path == "/" ? "" : c.path);
      src += ", " + expiry_text(c.expires, now);
      if (own.count(c.name)) src += " (not sent: the Cookie header has it)";
      else ++jar_sent;
      std::string v = c.value.size() > 200 ? c.value.substr(0, 200) + "\xE2\x80\xA6" : c.value;
      rows.push_back({c.name, v, src});
    }
  }
  sig.push_back(std::to_string(jar_off) + (ov_.jar ? "j" : "n"));
  for (const auto& r : rows) sig.push_back(r.name + '\x01' + r.value + '\x01' + r.source);
  if (sig == cookie_sig_ && cookie_grid_->GetItemCount() > 0) return;
  cookie_sig_ = sig;
  const Theme& t = Theme::get();
  wxWindowUpdateLocker lock(cookie_list_);
  cookie_grid_->Clear(true);
  auto head = [&](const char* text) {
    auto* l = new wxStaticText(cookie_list_, wxID_ANY, text);
    l->SetForegroundColour(t.dim);
    l->SetFont(l->GetFont().Bold());
    cookie_grid_->Add(l, 0);
  };
  if (rows.empty()) {
    auto* l = new wxStaticText(cookie_list_, wxID_ANY,
                               jar_off ? wxString::Format("No cookies are sent: the cookie jar is off for this request "
                                                          "(%zu jar cookie%s match this URL).",
                                                          jar_off, jar_off == 1 ? "" : "s")
                                       : wxString("No cookies are sent: add one to this request (Add cookie) or to "
                                                  "the workspace jar (Manage cookies)."));
    l->SetForegroundColour(t.dim);
    cookie_grid_->Add(l, 0);
    cookie_grid_->AddSpacer(0);
    cookie_grid_->AddSpacer(0);
  } else {
    head("Name");
    head("Value");
    head("From");
    const wxFont mono = mono_font(GetFont().GetPointSize());
    for (const auto& r : rows) {
      auto* n = new wxStaticText(cookie_list_, wxID_ANY, wxString::FromUTF8(r.name));
      n->SetForegroundColour(t.fg);
      n->SetFont(mono);
      auto* v = new wxStaticText(cookie_list_, wxID_ANY, wxString::FromUTF8(r.value), wxDefaultPosition, wxDefaultSize,
                                 wxST_ELLIPSIZE_END);
      v->SetForegroundColour(t.fg);
      v->SetFont(mono);
      v->SetToolTip(wxString::FromUTF8(r.value));
      auto* src = new wxStaticText(cookie_list_, wxID_ANY, wxString::FromUTF8(r.source));
      src->SetForegroundColour(t.dim);
      cookie_grid_->Add(n, 0, wxALIGN_CENTER_VERTICAL);
      cookie_grid_->Add(v, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
      cookie_grid_->Add(src, 0, wxALIGN_CENTER_VERTICAL);
    }
  }
  size_t nc = jar_sent;
  for (const auto& r : rows) nc += r.source == "Cookie header";
  wxString heading = rows.empty() ? wxString("Cookies sent with this request")
                               : wxString::Format("%zu cookie%s sent with this request%s", nc, nc == 1 ? "" : "s",
                                                  base_.cookie_files.empty() ? "" : ", plus the cookie files below");
  if (jar_sent) heading += wxString::Format(" (%zu from the jar)", jar_sent);
  if (jar_off && !rows.empty()) heading += wxString::Format(" (jar off: %zu not sent)", jar_off);
  cookie_head_->SetLabel(heading);
  if (jar_check_ && jar_check_->value() != ov_.jar) jar_check_->set_value(ov_.jar);
  cookie_list_->GetParent()->GetParent()->Layout();
}

std::vector<Cookie> HttpEditor::jar_matches() const {
  if (jar_all_.empty()) return {};
  return jar_cookies_for(jar_all_, sent_url(), cookies::now());
}

void HttpEditor::set_use_jar(bool on) {
  if (ov_.jar == on) return;
  ov_.jar = on;
  const bool was = loading_;
  loading_ = true;
  if (jar_opt_) jar_opt_->set_value(on);
  if (jar_check_) jar_check_->set_value(on);
  loading_ = was;
  touched();  // badges + the Cookies list
}

// Finds the workspace's jar (cookie_jar_for canonicalises the root path) and loads it (a file
// read + DPAPI unseal) on a worker; the cookies come back here as a copy.
void HttpEditor::refresh_jar() {
  const Workspace* ws = host_->workspace();
  if (!ws) return;
  std::filesystem::path root = ws->root();
  std::shared_ptr<CookieJar> jar = root == jar_root_ ? jar_ : nullptr;
  const uint64_t gen = ++jar_gen_;
  std::thread([alive = alive_, root = std::move(root), jar = std::move(jar), gen]() mutable {
    if (!jar) jar = cookie_jar_for(root);
    jar->preload();
    std::vector<Cookie> all = jar->list();
    std::lock_guard lk(alive->m);
    if (!alive->target) return;
    auto* ed = static_cast<HttpEditor*>(alive->target);
    ed->CallAfter([ed, gen, root = std::move(root), jar = std::move(jar), all = std::move(all)]() mutable {
      if (gen != ed->jar_gen_) return;
      const bool same = all == ed->jar_all_ && ed->jar_ == jar;
      ed->jar_ = std::move(jar);
      ed->jar_root_ = std::move(root);
      ed->jar_all_ = std::move(all);
      if (same) return;
      ed->update_badges();
      if (ed->book_->GetSelection() == kTabCookies) ed->update_cookies();
    });
  }).detach();
}

std::optional<size_t> HttpEditor::sent_body_size() const {
  const Workspace* ws = host_->workspace();
  constexpr size_t kMax = 1 << 20;  // above this the size is the template's (no expansion here)
  auto expanded = [&](std::string_view b) -> size_t {
    if (!ws || b.size() > kMax || b.find("{{") == std::string_view::npos) return b.size();
    return ws->expand(std::string(b)).size();
  };
  switch (body_mode_) {
    case BodyMode::None:
      return 0;
    case BodyMode::Json:
    case BodyMode::Text:
    case BodyMode::Xml: {
      if (pending_body_) return pending_body_->size();
      const std::string_view shown(body_->GetCharacterPointer(), static_cast<size_t>(body_->GetLength()));
      if (body_mode_ == BodyMode::Json && minify_->value()) {
        if (raw_body_ && raw_edits_ == body_edits_) return expanded(*raw_body_);
        if (minified_current()) return expanded(*minified_);
        if (shown.size() > kMax) return std::nullopt;
        return expanded(minify_json(shown));
      }
      return expanded(shown);
    }
    case BodyMode::UrlEncoded: {
      std::vector<KvItem> items;
      for (const auto& row : form_rows(0)) items.push_back({row.key, row.value, row.enabled, false});
      const std::string b = build_urlencoded(items);
      return b.find("{{") == std::string::npos ? b.size() : sent_form(b).size();
    }
    default:
      return std::nullopt;
  }
}

// ------------------------------------------------------------------ pre-request variables

std::vector<PreRow> HttpEditor::pre_now() const {
  if (!pre_) return pre_vals_;
  std::vector<PreRow> out;
  for (const auto& row : pre_->rows()) {
    PreRow x;
    x.var = std::string(trim(row.key));
    x.expr = row.value;
    x.scope = row.type.empty() ? "runtime" : row.type;
    x.enabled = row.enabled;
    out.push_back(std::move(x));
  }
  return out;
}

std::vector<PreVar> HttpEditor::to_pre_vars(const std::vector<PreRow>& rows) {
  std::vector<PreVar> out;
  for (const auto& x : rows) {
    PreVar v;
    v.enabled = x.enabled;
    v.var = x.var;
    v.expr = x.expr;
    v.scope = capture_scope_from(x.scope);
    out.push_back(std::move(v));
  }
  return out;
}

void HttpEditor::update_pre_names() {
  pre_names_.clear();
  for (const auto& x : pre_now())
    if (x.enabled && !x.var.empty() && !trim(x.expr).empty()) pre_names_.insert(x.var);
}

SavedRequest HttpEditor::eval_request(bool with_body, bool sending) const {
  SavedRequest r = base_;
  const auto stored = asserts_stored_;
  const SavedRequest* target = asserts_target_;
  auto* self = const_cast<HttpEditor*>(this);
  const bool was = sending_;
  self->sending_ = sending;
  store_no_body_ = !with_body;
  store(r);
  store_no_body_ = false;
  self->sending_ = was;
  asserts_stored_ = stored;
  asserts_target_ = target;
  if (!sending) r.http.url = with_path(r.http.url);
  return r;
}

SavedRequest HttpEditor::export_request(std::string* warning) const {
  SavedRequest r = eval_request(true, false);
  const Workspace* ws = host_->workspace();
  const std::filesystem::path own_file = r.file;
  if (std::filesystem::path f = inherit_from(); !f.empty()) r.file = f;  // its folders' headers / auth
  std::string warn;
  if (const auto empty = empty_path_vars(); !empty.empty())
    warn = "path variable :" + empty.front() + " has no value (Send refuses this request)";
  if (!resolve_vars_in_curl_ && ws) {
    r = ws->resolved(r);  // the folders' / collection's headers and auth, {{vars}} kept
  } else if (resolve_vars_in_curl_ && ws) {
    if (pre_names_.empty()) {
      r = ws->expanded(r);
    } else {
      // The pre-request values as last computed for the Value column (a timestamp or a nonce is
      // computed again by each send).
      Workspace w = *ws;
      size_t missing = 0;
      for (const auto& n : pre_names_) {
        const auto it = pre_preview_.find(n);
        if (it != pre_preview_.end() && it->second.ok) w.runtime_vars[n] = it->second.text;
        else ++missing;
      }
      r = w.expanded(r);
      if (warn.empty())
        warn = missing ? "pre-request variables not computed yet stay as {{name}}"
                       : "pre-request values are the ones shown on the Scripts tab (each send computes them again)";
    }
  }
  {
    // The redirect / URL / TLS options the send applies: this request's over its folders'.
    const HttpRequest eo = effective_options(ov_.net, inh_.opts);
    r.http.max_redirects = eo.max_redirects;
    r.http.keep_method_on_redirect = eo.keep_method_on_redirect;
    r.http.keep_auth_on_redirect = eo.keep_auth_on_redirect;
    r.http.encode_url = eo.encode_url;
    r.http.tls_min_version = eo.tls_min_version;
  }
  drop_shadowed_basic(r);  // an Authorization header that is sent beats Basic: never -H and -u both
  if (warning) *warning = std::move(warn);
  r.file = own_file;
  return r;
}

// The "Value now" column: the rows evaluated on a worker against a copy of the workspace.
void HttpEditor::preview_pre() {
  if (!pre_ || book_->GetSelection() != kTabTests) return;
  const Workspace* ws = host_->workspace();
  auto rows = to_pre_vars(pre_now());
  const uint64_t gen = ++pre_gen_;
  if (!ws || rows.empty()) {
    pre_preview_.clear();
    pre_->ForceRefresh();
    return;
  }
  // The body only when an expression reads it and it is small (a big one is read at send time).
  const bool wants_body = std::any_of(rows.begin(), rows.end(), [](const PreVar& v) { return v.expr.find("req.body") != std::string::npos; });
  const bool body = wants_body && (!text_mode(body_mode_) || body_->GetLength() < 1024 * 1024);
  auto req = std::make_shared<SavedRequest>(eval_request(body, false));
  auto wsc = std::make_shared<Workspace>(*ws);
  std::thread([alive = alive_, rows = std::move(rows), req, wsc, gen, wants_body, body] {
    auto res = evaluate_pre_vars(rows, *req, *wsc);
    std::map<std::string, KvGrid::Preview> out;
    for (const auto& x : res) {
      KvGrid::Preview pv{x.ok, x.ok ? x.value : x.message};
      if (x.ok && wants_body && !body && rows[x.index].expr.find("req.body") != std::string::npos)
        pv = {false, "the body is over 1 MB: computed when sent"};
      out[std::string(trim(rows[x.index].var))] = std::move(pv);
    }
    std::lock_guard lk(alive->m);
    if (!alive->target) return;
    auto* ed = static_cast<HttpEditor*>(alive->target);
    ed->CallAfter([ed, gen, out = std::move(out)] {
      if (gen != ed->pre_gen_ || !ed->pre_) return;
      ed->pre_preview_ = out;
      ed->pre_->ForceRefresh();
    });
  }).detach();
}

void HttpEditor::run_pre_then_send(std::vector<PreVar> rows) {
  const Workspace* ws = host_->workspace();
  if (!ws) return;
  const bool body = std::any_of(rows.begin(), rows.end(), [](const PreVar& v) { return v.expr.find("req.body") != std::string::npos; });
  auto req = std::make_shared<SavedRequest>(eval_request(body, true));  // as sent: path values, minified body
  auto wsc = std::make_shared<Workspace>(*ws);
  const uint64_t gen = ++pre_send_gen_;
  pre_busy_ = true;
  const auto t0 = Clock::now();
  std::thread([alive = alive_, rows = std::move(rows), req, wsc, gen, t0] {
    auto res = evaluate_pre_vars(rows, *req, *wsc);
    const double ms = ms_between(t0, Clock::now());
    std::lock_guard lk(alive->m);
    if (!alive->target) return;
    auto* ed = static_cast<HttpEditor*>(alive->target);
    ed->CallAfter([ed, gen, rows, res = std::move(res), ms] { ed->pre_done(gen, rows, res, ms); });
  }).detach();
}

void HttpEditor::pre_done(uint64_t gen, const std::vector<PreVar>& rows, const std::vector<PreVarResult>& res, double ms) {
  pre_busy_ = false;
  if (gen != pre_send_gen_) return;
  // The shell's workspace (as the response view's captures write to it): the values are runtime
  // variables of this session, and saved into the environment / globals when the row says so.
  auto* ws = const_cast<Workspace*>(host_->workspace());
  if (!ws) return;
  for (const auto& x : res) {
    const PreVar& pv = rows[x.index];
    const std::string name(trim(pv.var));
    pre_preview_[name] = {x.ok, x.ok ? x.value : x.message};
    if (x.ok) continue;
    const Theme& t = Theme::get();
    show_info(wxString::FromUTF8("Not sent: pre-request variable '" + name + "': " + x.message),
              "Fix the row on the Scripts tab (its Value column shows the problem).", "Show", [this] {
                hide_banner();
                show_script_side(0);  // the Pre-request side, where the row is
              });
    banner_->set_colours(t.warn_bg, t.warn_border);
    banner_->Refresh();
    banner_missing_.clear();
    if (pre_) pre_->ForceRefresh();
    return;
  }
  Environment* env = nullptr;
  for (auto& e : ws->environments())
    if (e.name == ws->active_env) env = &e;
  bool persist = false;
  std::string skipped;
  for (const auto& x : res) {
    const PreVar& pv = rows[x.index];
    const std::string name(trim(pv.var));
    ws->runtime_vars[name] = x.value;  // what this send expands (runtime vars come first)
    if (pv.scope == Capture::Environment) {
      if (!env) {
        skipped = name;
      } else if (env->vars[name] != x.value) {
        env->vars[name] = x.value;
        persist = true;
      }
    } else if (pv.scope == Capture::Globals && ws->globals[name] != x.value) {
      ws->globals[name] = x.value;
      persist = true;
    }
  }
  if (persist) {
    // Saved off the UI thread from a snapshot (the UI keeps changing the live workspace).
    auto snap = std::make_shared<Workspace>(*ws);
    std::thread([snap] { snap->save_settings(); }).detach();
  }
  if (pre_) pre_->ForceRefresh();
  pre_ready_ = true;
  do_send();
  pre_ready_ = false;
  if (!in_flight_) return;  // stopped by a later check (its banner says why)
  note(wxString::Format("Pre-request: %zu variable%s in %.2f ms, off the UI thread%s", res.size(), res.size() == 1 ? "" : "s", ms,
                        skipped.empty() ? wxString() : wxString::FromUTF8(" (no active environment for " + skipped + ": runtime only)")));
}

// ------------------------------------------------------------------ minified send of a big edited body

void HttpEditor::minify_then_send() {
  auto text = std::make_shared<const std::string>(body_->GetCharacterPointer(), static_cast<size_t>(body_->GetLength()));
  const uint64_t edits = body_edits_;
  minify_busy_ = true;
  const auto t0 = Clock::now();
  std::thread([alive = alive_, text, edits, t0] {
    auto mini = std::make_shared<const std::string>(minify_json(*text));
    const double ms = ms_between(t0, Clock::now());
    std::lock_guard lk(alive->m);
    if (!alive->target) return;
    auto* ed = static_cast<HttpEditor*>(alive->target);
    ed->CallAfter([ed, mini, edits, ms] {
      ed->minify_busy_ = false;
      if (edits != ed->body_edits_) {  // edited meanwhile: minify the new text
        ed->do_send();
        return;
      }
      ed->minified_ = mini;
      ed->minified_edits_ = edits;
      ed->do_send();
      (void)ms;
    });
  }).detach();
}

// ------------------------------------------------------------------ GraphQL self test (WATT_HTTPED_GQLTEST)

void HttpEditor::gql_self_test_tick() {
  using namespace std::chrono;
  const auto now = steady_clock::now();
  const double since = duration<double, std::milli>(now - gqlst_t0_).count();
  auto log = [this](const std::string& s) {
    gqlst_log_ += s + "\n";
    std::ofstream(gqlst_out_, std::ios::binary | std::ios::trunc) << gqlst_log_;
  };
  auto fmt = [](const char* f, auto... a) {
    char buf[768];
    std::snprintf(buf, sizeof buf, f, a...);
    return std::string(buf);
  };
  auto next = [&](int state) {
    gqlst_state_ = state;
    gqlst_t0_ = steady_clock::now();
  };
  auto timed = [](auto&& fn) {
    const auto a = steady_clock::now();
    fn();
    return duration<double, std::milli>(steady_clock::now() - a).count();
  };
  // Types text into the query as the keyboard does (CHARADDED runs gql_char_added).
  auto type = [this](const std::string& text) {
    for (char c : text) {
      const int at = gql_query_->GetCurrentPos();
      if (c == '\n') {
        if (gql_pop_) {
          gql_pop_->key(WXK_RETURN);  // Enter picks, as the key handler does
          continue;
        }
        gql_query_->NewLine();
      } else if ((c == ')' || c == '}' || c == ']') && gql_query_->GetCharAt(at) == c &&
                 gql_query_->IndicatorValueAt(kIndAutoClose, at) != 0) {
        gql_query_->GotoPos(at + 1);  // steps over the auto-closed one (gql_key)
        continue;
      } else {
        const char b[2] = {c, 0};
        gql_query_->AddTextRaw(b, 1);
      }
      gql_char_added(c);
    }
  };
  auto list = [this] {
    std::string l = gql_pop_ && gql_pop_->IsShown() ? "[shown] " : "[not shown] ";
    for (const auto& i : gql_items_) l += i.name + " ";
    return l;
  };
  auto lints = [this] {
    return "query lint: [" + (gql_lint_->IsShown() ? gql_lint_->GetLabel().utf8_string() : std::string()) +
           "] vars lint: [" + (gql_vars_lint_->IsShown() ? gql_vars_lint_->GetLabel().utf8_string() : std::string()) + "]";
  };
  switch (gqlst_state_) {
    case 0: {
      if (gqlst_t0_ == steady_clock::time_point{}) {
        gqlst_t0_ = now;
        return;
      }
      if (!IsShownOnScreen() || since < 2500 || !host_->workspace()) return;
      wxString url;
      if (!wxGetEnv("WATT_HTTPED_GQLURL", &url)) url = "http://127.0.0.1:18930/graphql";
      gqlst_probe_ = std::make_shared<StallProbe>();
      gqlst_probe_->hwnd = static_cast<HWND>(wxGetTopLevelParent(this)->GetHWND());
      gqlst_probe_->start();
      const double ms_url = timed([&] { set_url_text(url.utf8_string()); });
      const double ms_tab = timed([&] {
        tabs_->select(1);
        if (tabs_->on_select) tabs_->on_select(1);
      });
      const double ms_page = timed([&] { ensure_body_page(6); });
      const double ms = timed([&] { set_body_mode(BodyMode::GraphQL, true); });
      log(fmt("URL %.1f ms, Body tab %.1f ms, GraphQL page build %.1f ms, switch to GraphQL: %.1f ms", ms_url, ms_tab,
              ms_page, ms));
      log("prebuilt: " + prebuild_times_);
      const double ms2 = timed([&] {
        gql_query_->SetText("query Q($id: ID!) {\n  user(id: $id) {\n    id\n  }\n}\n");
        gql_vars_->SetText("{ \"id\": \"1\", \"idx\": 2 }");
      });
      log(fmt("set query + vars: %.1f ms; before the schema: %s", ms2, lints().c_str()));
      next(1);
      return;
    }
    case 1: {
      if (since < 600) return;
      log(fmt("idle: worst stall %.1f ms; chip [%s]", gqlst_probe_->take(), gql_chip_->GetLabel().utf8_string().c_str()));
      const double ms = timed([&] { gql_fetch(); });
      log(fmt("Fetch click: %.2f ms (worker from here)", ms));
      next(2);
      return;
    }
    case 2: {
      if (gql_busy_ && since < 20000) return;
      log(fmt("fetched after %.1f ms: chip [%s], worst UI stall while fetching %.1f ms", since,
              gql_chip_->GetLabel().utf8_string().c_str(), gqlst_probe_->take()));
      log("note: " + note_->GetLabel().utf8_string());
      log("after the schema: " + lints());
      // The caret at the end of "    id" (line 3), then a new field typed.
      if (auto* top = wxGetTopLevelParent(this)) {
        top->Raise();
        ::SetForegroundWindow(static_cast<HWND>(top->GetHWND()));
      }
      gql_query_->SetFocus();
      {
        // The pieces of one completion: the walk, then Scintilla's list (the first one creates
        // its window).
        const std::string doc = gql_query_->GetText().utf8_string();
        const double walk = timed([&] { (void)gql_complete(doc, doc.size() / 2, gql_schema_.get()); });
        const double val = timed([&] { (void)gql_validate(doc, gql_schema_.get()); });
        log(fmt("walk %.3f ms, validate %.3f ms (%zu bytes)", walk, val, doc.size()));
      }
      gql_query_->GotoPos(gql_query_->GetLineEndPosition(2));
      double worst = 0;
      for (const char* step : {"\n", "n", "a"}) {
        const double one = timed([&] { type(step); });
        worst = std::max(worst, one);
        log(fmt("  typed [%s] %.2f ms: %s", step[0] == '\n' ? "Enter" : step, one, list().c_str()));
      }
      log(fmt("typed '\\n na': worst step %.2f ms; list: %s", worst, list().c_str()));
      double ms = timed([&] {
        if (gql_pop_) {
          gql_pop_->key(WXK_RETURN);
        } else if (!gql_items_.empty()) {  // no focus (another window is in front): as the pick would
          const int p = gql_query_->GetCurrentPos(), from = gql_query_->WordStartPosition(p, true);
          gql_query_->SetTargetRange(from, p);
          gql_query_->ReplaceTarget(wxString::FromUTF8(gql_items_[0].name));
          gql_query_->GotoPos(from + static_cast<int>(gql_items_[0].name.size()));
        }
      });
      log(fmt("Enter picks: %.2f ms; line now [%s]", ms,
              gql_query_->GetLine(gql_query_->GetCurrentLine()).utf8_string().c_str()));
      ms = timed([&] { type("\nposts("); });
      log(fmt("typed 'posts(': %.2f ms; text after caret [%s]", ms,
              gql_query_->GetTextRange(gql_query_->GetCurrentPos(), gql_query_->GetCurrentPos() + 1).utf8_string().c_str()));
      ms = timed([&] { gql_completion(true); });
      log(fmt("Ctrl+Space in posts(): %.2f ms; list: %s", ms, list().c_str()));
      gql_close_pop();
      ms = timed([&] { type("first: $"); });
      log(fmt("typed 'first: $': %.2f ms; list: %s", ms, list().c_str()));
      gql_close_pop();
      type("id) {");
      ms = timed([&] { type("\n"); });
      log(fmt("Enter inside {}: %.2f ms", ms));
      next(3);
      return;
    }
    case 3: {
      if (since < 100) return;
      double ms = timed([&] { type("titl"); });
      log(fmt("typed 'titl': %.2f ms; list: %s", ms, list().c_str()));
      gql_close_pop();
      ms = timed([&] { type("e nope"); });
      gql_close_pop();
      log(fmt("typed 'e nope': %.2f ms; %s", ms, lints().c_str()));
      log("query now:\n" + gql_query_->GetText().utf8_string());
      // The big schema: parse on the worker, UI stall while it arrives.
      wxString big;
      if (wxGetEnv("WATT_HTTPED_GQLBIG", &big)) {
        next(6);
        return;
      }
      next(5);
      return;
    }
    case 6: {
      if (since < 500) return;
      wxString big;
      wxGetEnv("WATT_HTTPED_GQLBIG", &big);
      double ms = 0;
      log(fmt("idle before the big schema: worst stall %.1f ms", gqlst_probe_->take()));
      {
        set_url_text(big.utf8_string());
        gql_lookup();
        ms = timed([&] { gql_fetch(); });
        log(fmt("big Fetch click: %.2f ms", ms));
        next(4);
        return;
      }
    }
    case 4: {
      if (gql_busy_ && since < 30000) return;
      log(fmt("big schema after %.1f ms: chip [%s], worst UI stall %.1f ms; note: %s", since,
              gql_chip_->GetLabel().utf8_string().c_str(), gqlst_probe_->take(), note_->GetLabel().utf8_string().c_str()));
      const double ms = timed([&] { lint_gql(); });
      log(fmt("lint with the big schema: %.2f ms; %s", ms, lints().c_str()));
      next(5);
      return;
    }
    case 5: {
      if (since < 300) return;
      gqlst_timer_.Stop();
      log(fmt("done: worst stall %.1f ms", gqlst_probe_->take()));
      if (!wxGetEnv("WATT_HTTPED_SELFTEST_STAY", nullptr)) {
        gqlst_probe_->run = false;
        ::TerminateProcess(::GetCurrentProcess(), 0);
      }
      // Staying, for a screenshot: a completion list in the nested selection.
      gqlst_probe_->run = false;
      gql_query_->SetFocus();
      const int l = gql_query_->LineFromPosition(gql_query_->GetText().utf8_string().find("nope"));
      gql_query_->GotoPos(gql_query_->GetLineEndPosition(l));
      type("\n");
      type("c");
      return;
    }
    default:
      return;
  }
}

// ------------------------------------------------------------------ self test (WATT_HTTPED_SELFTEST)

void HttpEditor::self_test_tick() {
  using namespace std::chrono;
  const auto now = steady_clock::now();
  const double since = duration<double, std::milli>(now - st_t0_).count();
  auto log = [this](const std::string& s) { st_log_ += s + "\n"; };
  auto fmt = [](const char* f, auto... a) {
    char buf[512];
    std::snprintf(buf, sizeof buf, f, a...);
    return std::string(buf);
  };
  auto next = [&](int state) {
    st_state_ = state;
    st_t0_ = steady_clock::now();
  };
  switch (st_state_) {
    case 0: {
      if (st_t0_ == steady_clock::time_point{}) {
        st_t0_ = now;
        return;
      }
      if (!IsShownOnScreen() || since < 3000) return;
      wxString big;
      if (!wxGetEnv("WATT_HTTPED_BIG", &big)) big = "big.json";
      std::ifstream in(big.ToStdWstring(), std::ios::binary);
      auto text = std::make_shared<std::string>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      st_big_ = text;
      log("INFO ctor ms: " + ctor_times_);
      log("INFO prebuilt ms: " + prebuild_times_);
      {
        const auto a = steady_clock::now();
        show_script_side(1);
        const auto m = steady_clock::now();
        tabs_->select(strip_of(kTabOptions));
        if (tabs_->on_select) tabs_->on_select(strip_of(kTabOptions));
        log(fmt("INFO open Tests %.1f ms, Options %.1f ms (without paint)", duration<double, std::milli>(m - a).count(),
                duration<double, std::milli>(steady_clock::now() - m).count()));
      }
      log(fmt("INFO big %zu bytes, grow %s", text->size(), wxGetEnv("WATT_HTTPED_NOGROW", nullptr) ? "off" : "on"));
      set_body_mode(BodyMode::Json, true);
      tabs_->select(1);
      if (tabs_->on_select) tabs_->on_select(1);
      st_probe_ = std::make_shared<StallProbe>();
      st_probe_->hwnd = static_cast<HWND>(wxGetTopLevelParent(this)->GetHWND());
      st_probe_->start();
      next(10);
      return;
    }
    case 10: {  // a fresh paste
      // Rendered now (as another app's copy would be), not on the paste's GetClipboardData.
      const std::wstring w = wxString::FromUTF8(*st_big_).ToStdWstring();
      if (::OpenClipboard(static_cast<HWND>(GetHWND()))) {
        ::EmptyClipboard();
        if (HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t))) {
          std::memcpy(::GlobalLock(h), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
          ::GlobalUnlock(h);
          ::SetClipboardData(CF_UNICODETEXT, h);
        }
        ::CloseClipboard();
      }
      body_->SetFocus();
      body_->SelectAll();
      next(11);
      return;
    }
    case 11: {
      if (since < 400) return;
      log(fmt("R%d idle before paste: worst stall %.1f ms", st_round_, st_probe_->take()));
      st_swaps_at_ = st_swaps_;
      const auto t = steady_clock::now();
      const bool ok = paste_long_json();
      log(fmt("R%d paste call %.1f ms (%s)", st_round_, duration<double, std::milli>(steady_clock::now() - t).count(),
              ok ? "taken" : "NOT TAKEN"));
      if (!ok) log(fmt("  mode %d ro %d len %d", static_cast<int>(body_mode_), body_->GetReadOnly() ? 1 : 0, body_->GetLength()));
      if (!ok) {
        next(10);  // the clipboard was busy: again
        return;
      }
      next(12);
      return;
    }
    case 12: {
      if (!(st_swaps_ > st_swaps_at_ && raw_body_ && !pending_body_ && !fill_src_ && raw_edits_ == body_edits_)) {
        if (since > 20000) {
          log(fmt("FAIL paste never finished: swaps %d/%d raw %d pending %d fill %d edits %llu/%llu", st_swaps_,
                  st_swaps_at_, raw_body_ ? 1 : 0, pending_body_ ? 1 : 0, fill_src_ ? 1 : 0,
                  static_cast<unsigned long long>(raw_edits_), static_cast<unsigned long long>(body_edits_)));
          next(99);
        }
        return;
      }
      log(fmt("R%d paste done in %.0f ms: %d lines, worst stall %.1f ms, swap %.1f ms (install %.1f), fill %d slices %.1f ms UI (max %.1f)",
              st_round_, since, body_->GetLineCount(), st_probe_->take(), st_swap_ms_, st_install_ms_, fill_slices_, fill_ui_ms_,
              fill_max_ms_));
      next(13);
      return;
    }
    case 13:
    case 15:
    case 17: {  // fold / unfold / fold the biggest block near the top
      if (since < 600) return;
      log(fmt("R%d idle: worst stall %.1f ms", st_round_, st_probe_->take()));
      if (st_line_ < 0) {
        for (int l = 0; l < 50 && st_line_ < 0; ++l)
          if ((body_->GetFoldLevel(l) & wxSTC_FOLDLEVELHEADERFLAG) && fold_block_end(l) - l > 100000) st_line_ = l;
        if (st_line_ < 0) {
          log("FAIL no big block");
          next(99);
          return;
        }
      }
      const auto tb = steady_clock::now();
      const int bend = fold_block_end(st_line_);
      log(fmt("R%d   block end %d: %.1f ms", st_round_, bend + 1, duration<double, std::milli>(steady_clock::now() - tb).count()));
      const auto t = steady_clock::now();
      const bool big = big_fold(st_line_);
      log(fmt("R%d %s line %d: call %.1f ms%s", st_round_, st_state_ == 15 ? "unfold" : "fold", st_line_ + 1,
              duration<double, std::milli>(steady_clock::now() - t).count(), big ? "" : " (not big_fold)"));
      next(st_state_ + 1);
      return;
    }
    case 14:
    case 16:
    case 18: {
      if (!fold_ops_.empty()) return;
      log(fmt("R%d   done in %.0f ms, worst stall %.1f ms, longest slice %.1f ms (%d lines), line %d visible %d",
              st_round_, since, st_probe_->take(), fold_max_ms_, fold_lines_, st_line_ + 2,
              body_->GetLineVisible(st_line_ + 1) ? 1 : 0));
      fold_max_ms_ = 0;
      next(st_state_ == 18 ? 19 : st_state_ + 1);
      return;
    }
    case 19: {  // find a term inside the folded block, as the find bar does
      if (since < 600) return;
      log(fmt("R%d idle: worst stall %.1f ms", st_round_, st_probe_->take()));
      const auto t = steady_clock::now();
      auto snap = std::make_shared<const std::string>(body_->GetCharacterPointer(), static_cast<size_t>(body_->GetLength()));
      const double copy_ms = duration<double, std::milli>(steady_clock::now() - t).count();
      const std::string term = "\"item 140000\"";
      const size_t at = snap->find(term);  // the bar does this on a worker
      const auto t2 = steady_clock::now();
      if (at == std::string::npos) {
        log("FAIL term not found");
        next(99);
        return;
      }
      const int s = static_cast<int>(at), e = s + static_cast<int>(term.size());
      const int l0 = body_->LineFromPosition(s), l1 = body_->LineFromPosition(e);
      if (!body_->GetLineVisible(l0)) reveal_body(l0, l1);
      body_->SetSelection(s, e);
      body_->EnsureCaretVisible();
      body_->ScrollRange(e, s);
      log(fmt("R%d find line %d: copy %.1f ms, select+reveal %.1f ms", st_round_, l0 + 1, copy_ms,
              duration<double, std::milli>(steady_clock::now() - t2).count()));
      next(20);
      return;
    }
    case 20: {
      if (!fold_ops_.empty() || reveal_pending_) return;
      if (since < 300) return;
      const int l0 = body_->LineFromPosition(body_->GetSelectionStart());
      log(fmt("R%d   find done, worst stall %.1f ms, match visible %d, first visible line %d", st_round_,
              st_probe_->take(), body_->GetLineVisible(l0) ? 1 : 0, body_->GetFirstVisibleLine() + 1));
      st_line_ = -1;
      next(++st_round_ < 3 ? 10 : 99);
      return;
    }
    case 99: {
      st_timer_.Stop();
      if (st_probe_) st_probe_->run = false;
      std::ofstream(st_out_, std::ios::binary) << st_log_;
      if (!wxGetEnv("WATT_HTTPED_SELFTEST_STAY", nullptr)) ::TerminateProcess(::GetCurrentProcess(), 0);
      // Staying (to look at it): the big block folded, its header at the top.
      body_->GotoPos(0);
      for (int l = 1; l < 50; ++l) {
        if ((body_->GetFoldLevel(l) & wxSTC_FOLDLEVELHEADERFLAG) && fold_block_end(l) - l > 100000) {
          if (body_->GetFoldExpanded(l)) big_fold(l);
          break;
        }
      }
      return;
    }
    default:
      return;
  }
}

HttpEditorPanel* make_http_editor(wxWindow* parent, EditorHost* host) { return new HttpEditor(parent, host); }

}  // namespace watt::app
