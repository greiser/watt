#include "app/ws_view.h"

#include <wx/dcbuffer.h>
#include <wx/settings.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/stattext.h>
#include <wx/stc/stc.h>
#include <wx/textctrl.h>
#include <wx/vlbox.h>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#include <algorithm>
#include <cctype>
#include <set>

#include "app/autocomp_guard.h"
#include "app/http_editor_kv.h"
#include "app/http_editor_util.h"
#include "app/http_editor_widgets.h"
#include "app/response_format.h"
#include "app/response_widgets.h"

namespace watt::app {

namespace {

using http_editor::FlatButton;
using http_editor::KvGrid;
using http_editor::KvPanel;
using http_editor::KvRow;

constexpr int kIndOk = 8, kIndBad = 9;

std::string lower(std::string_view s) {
  std::string o(s);
  for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return o;
}

bool istarts(std::string_view s, std::string_view p) { return s.size() >= p.size() && lower(s.substr(0, p.size())) == p; }

// A bordered input area (the URL field's frame), its border in the accent colour while focused.
class Frame : public wxPanel {
 public:
  explicit Frame(wxWindow* parent) : wxPanel(parent, wxID_ANY) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
      wxAutoBufferedPaintDC dc(this);
      const auto& t = http_editor::Theme::get();
      dc.SetBackground(wxBrush(t.bg));
      dc.Clear();
      dc.SetBrush(wxBrush(t.field));
      dc.SetPen(wxPen(focus_ ? t.accent : t.border, FromDIP(1)));
      dc.DrawRoundedRectangle(GetClientRect(), FromDIP(4));
    });
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      Refresh(false);
      e.Skip();
    });
  }
  void set_focus(bool f) {
    focus_ = f;
    Refresh(false);
  }

 private:
  bool focus_ = false;
};

void style_stc(wxStyledTextCtrl* s, wxWindow* w, bool mono) {
  const auto& t = http_editor::Theme::get();
  wxFont f = mono ? wxFont(wxFontInfo(w->GetFont().GetPointSize()).Family(wxFONTFAMILY_TELETYPE).FaceName("Consolas"))
                  : w->GetFont();
  s->StyleSetFont(wxSTC_STYLE_DEFAULT, f);
  s->StyleSetForeground(wxSTC_STYLE_DEFAULT, t.fg);
  s->StyleSetBackground(wxSTC_STYLE_DEFAULT, t.field);
  s->StyleClearAll();
  s->SetCaretForeground(t.fg);
  s->SetSelBackground(true, t.dark ? wxColour(0x26, 0x4F, 0x78) : wxColour(0xAD, 0xD6, 0xFF));
  for (int m = 0; m < 5; ++m) s->SetMarginWidth(m, 0);
  s->SetEOLMode(wxSTC_EOL_LF);
}

void json_colours(wxStyledTextCtrl* s) {
  const auto& t = http_editor::Theme::get();
  s->SetLexer(wxSTC_LEX_JSON);
  s->StyleSetForeground(wxSTC_JSON_PROPERTYNAME, t.js_key);
  s->StyleSetForeground(wxSTC_JSON_STRING, t.js_string);
  s->StyleSetForeground(wxSTC_JSON_NUMBER, t.js_number);
  s->StyleSetForeground(wxSTC_JSON_KEYWORD, t.js_keyword);
  s->StyleSetForeground(wxSTC_JSON_OPERATOR, t.js_punct);
  s->StyleSetForeground(wxSTC_JSON_STRINGEOL, t.js_error);
  s->StyleSetForeground(wxSTC_JSON_ERROR, t.fg);
  s->StyleSetForeground(wxSTC_JSON_LINECOMMENT, t.dim);
  s->StyleSetForeground(wxSTC_JSON_BLOCKCOMMENT, t.dim);
  for (int st : {wxSTC_JSON_PROPERTYNAME, wxSTC_JSON_STRING, wxSTC_JSON_NUMBER, wxSTC_JSON_KEYWORD, wxSTC_JSON_OPERATOR,
                 wxSTC_JSON_STRINGEOL, wxSTC_JSON_ERROR, wxSTC_JSON_LINECOMMENT, wxSTC_JSON_BLOCKCOMMENT, wxSTC_JSON_DEFAULT})
    s->StyleSetBackground(st, t.field);
}

std::string ms_text(double v) { return fmt::ms(v); }

// "+1.234 ms", "+2.4105 s", "+3m 05s": the offset column of the message log (since Connect was
// clicked). 0.1 ms under a minute, so an echo's send and receive rows show its round trip.
std::string offset_text(double ms) {
  char b[48];
  if (ms < 1000) std::snprintf(b, sizeof b, "+%.3f ms", ms);
  else if (ms < 60000) std::snprintf(b, sizeof b, "+%.4f s", ms / 1000);
  else std::snprintf(b, sizeof b, "+%dm %02ds", static_cast<int>(ms / 60000), static_cast<int>(ms / 1000) % 60);
  return b;
}

}  // namespace

// ------------------------------------------------------------------ editor

namespace {

enum AuthChoice { kAuthInherit = 0, kAuthNone, kAuthBearer, kAuthBasic };

class WsEditor : public wxPanel, public WsEditorPanel {
 public:
  WsEditor(wxWindow* parent, EditorHost* host) : wxPanel(parent, wxID_ANY), host_(host) {
    const auto& t = http_editor::Theme::get();
    SetBackgroundColour(t.bg);
    auto* sz = new wxBoxSizer(wxVERTICAL);
    sz->AddSpacer(FromDIP(8));
    // Title row: what this tab is.
    // Fixed size (ellipsized): a long folder path must never widen the editor (the tab keeps its
    // sash at 50% and would chase a growing minimum width forever).
    title_ = new wxStaticText(this, wxID_ANY, "WebSocket", wxDefaultPosition, wxDefaultSize,
                              wxST_ELLIPSIZE_START | wxST_NO_AUTORESIZE);
    title_->SetMinSize(wxSize(FromDIP(80), GetCharHeight()));
    title_->SetForegroundColour(t.dim);
    sz->Add(title_, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sz->AddSpacer(FromDIP(4));
    // Address row.
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    box_ = new Frame(this);
    auto* inner = new wxBoxSizer(wxHORIZONTAL);
    badge_ = new wxStaticText(box_, wxID_ANY, "WS");
    badge_->SetFont(GetFont().Bold());
    badge_->SetForegroundColour(t.dark ? wxColour("#E8875A") : wxColour("#B04A12"));
    badge_->SetBackgroundColour(t.field);
    inner->Add(badge_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
    url_ = new wxStyledTextCtrl(box_, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxWANTS_CHARS);
    setup_url();
    inner->Add(url_, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(8));
    box_->SetSizer(inner);
    box_->SetMinSize(wxSize(-1, url_->TextHeight(0) + FromDIP(14)));
    row->Add(box_, 1, wxEXPAND | wxRIGHT, FromDIP(8));
    connect_ = new FlatButton(this, "Connect", FlatButton::Style::Primary);
    connect_->SetToolTip("Connect (Enter in the address, or Ctrl+Enter)");
    connect_->on_click = [this] {
      if (state_ == State::Closed) host_->send();
      else if (state_ == State::Connecting) host_->cancel();
      else host_->ws_close();
    };
    row->Add(connect_, 0, wxEXPAND);
    sz->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sent_as_ = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
    sent_as_->SetForegroundColour(t.dim);
    sent_as_->SetMinSize(wxSize(FromDIP(80), GetCharHeight()));
    sz->AddSpacer(FromDIP(4));
    sz->Add(sent_as_, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(14));
    sz->AddSpacer(FromDIP(6));
    // Pages.
    tabs_ = new http_editor::TabStrip(this);
    tabs_->set_tabs({"Message", "Headers", "Auth", "Docs"});
    tabs_->on_select = [this](size_t i) { book_->ChangeSelection(i); };
    sz->Add(tabs_, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    book_ = new wxSimplebook(this, wxID_ANY);
    book_->SetBackgroundColour(t.bg);
    build_message_page();
    build_headers_page();
    build_auth_page();
    build_docs_page();
    sz->Add(book_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    SetSizer(sz);
    set_state(State::Closed);
    // The title row ("folder / name") follows the tab's file: a first save or a rename in the
    // sidebar changes it without a load(). One path compare per idle.
    Bind(wxEVT_IDLE, [this](wxIdleEvent& e) {
      e.Skip();
      const std::filesystem::path f = host_->inherit_file();
      // The workspace opens after a restored tab's first idle: the crumb gains its folder then.
      const bool have_ws = host_->workspace() != nullptr;
      if (f == shown_file_ && have_ws == shown_ws_) return;
      shown_file_ = f;
      shown_ws_ = have_ws;
      std::string crumb = "WebSocket \xC2\xB7 unsaved";
      if (!f.empty() && f.filename() != L"(unsaved).req.json") {
        std::filesystem::path rel = f.filename();  // until the workspace is open
        if (const Workspace* ws = host_->workspace()) {
          std::error_code ec;
          const auto r = std::filesystem::relative(f, ws->root(), ec);
          if (!ec && !r.empty() && r.native().rfind(L"..", 0) != 0) rel = r;
        }
        const std::u8string u = rel.generic_u8string();
        std::string s(reinterpret_cast<const char*>(u.data()), u.size());
        if (s.size() > 9 && s.compare(s.size() - 9, 9, ".req.json") == 0) s.resize(s.size() - 9);
        for (size_t p = 0; (p = s.find('/', p)) != std::string::npos; p += 3) s.replace(p, 1, " / ");
        crumb = "WebSocket \xC2\xB7 " + s;
      }
      title_->SetLabel(wxString::FromUTF8(crumb));
    });
  }

  wxWindow* window() override { return this; }

  void load(const SavedRequest& r) override {
    loading_ = true;
    req_file_ = r.file;
    req_id_ = r.id;
    url_->SetText(wxString::FromUTF8(r.http.url));
    url_->EmptyUndoBuffer();
    body_->SetText(wxString::FromUTF8(r.http.body));
    body_->EmptyUndoBuffer();
    const bool json = r.http.body.empty() || http_editor::looks_like_json(r.http.body);
    mode_->select(json ? 0 : 1);
    apply_mode();
    // Headers: a Bearer header is the Auth page's.
    std::vector<KvRow> rows;
    std::string bearer;
    for (size_t k = 0; k < r.http.headers.size(); ++k) {
      const auto& [n, v] = r.http.headers[k];
      const bool on = !r.disabled_headers.count(k);
      if (on && bearer.empty() && lower(n) == "authorization" && istarts(v, "bearer ")) {
        bearer = v.substr(7);
        continue;
      }
      KvRow row;
      row.key = n;
      row.value = v;
      row.enabled = on;
      rows.push_back(std::move(row));
    }
    headers_->grid()->set_rows(rows);
    int mode = kAuthNone;
    if (!bearer.empty()) mode = kAuthBearer;
    else if (r.auth.type == HttpAuth::Type::Inherit) mode = kAuthInherit;
    else if (r.basic_auth.enabled) mode = kAuthBasic;
    auth_->set_selection(mode);
    bearer_->ChangeValue(wxString::FromUTF8(bearer));
    user_->ChangeValue(wxString::FromUTF8(r.basic_auth.user));
    pass_->ChangeValue(wxString::FromUTF8(r.basic_auth.password));
    other_auth_ = r.auth.type != HttpAuth::Type::None && r.auth.type != HttpAuth::Type::Inherit ? r.auth : HttpAuth{};
    apply_auth_mode();
    docs_->ChangeValue(wxString::FromUTF8(r.docs));
    shown_file_ = std::filesystem::path(L"?");  // the title row follows on the next idle
    loading_ = false;
    refresh_vars();
    update_badges();
  }

  void store(SavedRequest& r) const override {
    r.kind = RequestKind::WebSocket;
    r.http.method = "GET";
    store_head(r);
    r.http.body = body_->GetText().utf8_string();
    r.http.headers.clear();
    r.disabled_headers.clear();
    for (const KvRow& row : headers_->grid()->rows()) {
      if (row.key.empty()) continue;
      if (!row.enabled) r.disabled_headers.insert(r.http.headers.size());
      r.http.headers.emplace_back(row.key, row.value);
    }
    const int mode = auth_->selection();
    r.basic_auth = BasicAuth{};
    r.auth = HttpAuth{};
    switch (mode) {
      case kAuthInherit: r.auth.type = HttpAuth::Type::Inherit; break;
      case kAuthBearer: {
        const std::string tok = bearer_->GetValue().utf8_string();
        if (!tok.empty()) r.http.headers.emplace_back("Authorization", "Bearer " + tok);
        break;
      }
      case kAuthBasic:
        r.basic_auth.enabled = true;
        r.basic_auth.user = user_->GetValue().utf8_string();
        r.basic_auth.password = pass_->GetValue().utf8_string();
        break;
      default:
        if (other_auth_.type != HttpAuth::Type::None) r.auth = other_auth_;  // set elsewhere (OAuth 2.0 in the file)
        break;
    }
    r.docs = docs_->GetValue().utf8_string();
  }

  void store_head(SavedRequest& r) const override {
    r.kind = RequestKind::WebSocket;
    r.http.method = "GET";
    r.http.url = url_->GetText().utf8_string();
  }

  void set_in_flight(bool in_flight) override { set_state(in_flight ? State::Connecting : State::Closed); }

  void set_state(State s) override {
    state_ = s;
    connect_->SetLabel(s == State::Closed ? "Connect" : s == State::Connecting ? "Cancel" : "Disconnect");
    connect_->SetToolTip(s == State::Closed     ? "Connect (Enter in the address, or Ctrl+Enter)"
                         : s == State::Open     ? "Close the connection (close code 1000)"
                                                : "Stop connecting (Esc)");
    send_->Enable(s == State::Open);
    send_->SetToolTip(s == State::Open ? "Send this message (Ctrl+Enter)" : "Connect first (Ctrl+Enter connects)");
    hint_->SetLabel(s == State::Open ? "Ctrl+Enter sends" : "Ctrl+Enter connects");
    hint_->SetForegroundColour(http_editor::Theme::get().dim);
    hint_->UnsetToolTip();
    warned_text_.clear();
    hint_->GetParent()->Layout();
    Layout();
  }

  // JSON mode: a message that is not valid JSON ({{variables}} count as values) is held once with
  // the parse error in the composer's hint; sending it again unchanged sends it as written.
  bool confirm_message() override {
    if (mode_->selection() != 0) return true;
    const std::string text = body_->GetText().utf8_string();
    std::string probe;
    probe.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
      if (text.compare(i, 2, "{{") == 0)
        if (const size_t e = text.find("}}", i + 2); e != std::string::npos) {
          probe += '0';
          i = e + 2;
          continue;
        }
      probe += text[i++];
    }
    rapidjson::Document d;
    d.Parse(probe.data(), probe.size());
    if (!d.HasParseError() || text == warned_text_) {
      if (!warned_text_.empty()) set_state(state_);  // the hint back to normal
      return true;
    }
    warned_text_ = text;
    hint_->SetLabel(wxString::FromUTF8("Not valid JSON at " + std::to_string(d.GetErrorOffset()) + ": Send again sends it as is"));
    hint_->SetToolTip(wxString::FromUTF8(std::string(rapidjson::GetParseError_En(d.GetParseError())) + " (offset " +
                                         std::to_string(d.GetErrorOffset()) + ")"));
    hint_->SetForegroundColour(http_editor::Theme::get().var_bad);
    hint_->GetParent()->Layout();  // the message page's bar: room for the longer hint
    return false;
  }

  void focus_url() override {
    url_->SetFocus();
    url_->SelectAll();
  }

  void vars_changed() override { refresh_vars(); }

 private:
  void setup_url() {
    const auto& t = http_editor::Theme::get();
    wxStyledTextCtrl* s = url_;
    wxFont f = GetFont();
    f.SetPointSize(f.GetPointSize() + 1);
    s->StyleSetFont(wxSTC_STYLE_DEFAULT, f);
    s->StyleSetForeground(wxSTC_STYLE_DEFAULT, t.fg);
    s->StyleSetBackground(wxSTC_STYLE_DEFAULT, t.field);
    s->StyleClearAll();
    s->SetLexer(wxSTC_LEX_NULL);
    for (int m = 0; m < 5; ++m) s->SetMarginWidth(m, 0);
    s->SetMarginLeft(0);
    s->SetMarginRight(0);
    s->SetUseHorizontalScrollBar(false);
    s->SetUseVerticalScrollBar(false);
    s->SetWrapMode(wxSTC_WRAP_NONE);
    s->SetCaretForeground(t.fg);
    s->SetSelBackground(true, t.dark ? wxColour(0x26, 0x4F, 0x78) : wxColour(0xAD, 0xD6, 0xFF));
    s->SetModEventMask(wxSTC_MOD_INSERTTEXT | wxSTC_MOD_DELETETEXT);
    s->StyleSetFont(1, f);
    s->StyleSetForeground(1, t.dim);
    s->StyleSetBackground(1, t.field);
    s->EOLAnnotationSetVisible(wxSTC_EOLANNOTATION_STANDARD);
    s->IndicatorSetStyle(kIndOk, wxSTC_INDIC_TEXTFORE);
    s->IndicatorSetForeground(kIndOk, t.var_ok);
    s->IndicatorSetStyle(kIndBad, wxSTC_INDIC_TEXTFORE);
    s->IndicatorSetForeground(kIndBad, t.var_bad);
    s->IndicatorSetStyle(kIndOk + 2, wxSTC_INDIC_ROUNDBOX);
    s->IndicatorSetForeground(kIndOk + 2, t.var_ok);
    s->IndicatorSetAlpha(kIndOk + 2, 45);
    s->IndicatorSetOutlineAlpha(kIndOk + 2, 0);
    s->IndicatorSetUnder(kIndOk + 2, true);
    s->IndicatorSetStyle(kIndBad + 2, wxSTC_INDIC_ROUNDBOX);
    s->IndicatorSetForeground(kIndBad + 2, t.var_bad);
    s->IndicatorSetAlpha(kIndBad + 2, 45);
    s->IndicatorSetOutlineAlpha(kIndBad + 2, 0);
    s->IndicatorSetUnder(kIndBad + 2, true);
    const int h = s->TextHeight(0);
    s->SetMinSize(wxSize(FromDIP(100), h));
    s->SetMaxSize(wxSize(-1, h));
    s->AutoCompSetIgnoreCase(true);
    s->AutoCompSetAutoHide(true);
    s->AutoCompSetSeparator('\n');
    s->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
      const int k = e.GetKeyCode();
      if (url_->AutoCompActive()) {
        e.Skip();
        return;
      }
      if ((k == WXK_RETURN || k == WXK_NUMPAD_ENTER) && !e.ControlDown()) {
        if (state_ == State::Closed) host_->send();
      } else if (k == WXK_ESCAPE && state_ == State::Connecting) {
        host_->cancel();
      } else if (k == WXK_TAB && !e.ControlDown()) {
        url_->Navigate(e.ShiftDown() ? wxNavigationKeyEvent::IsBackward : wxNavigationKeyEvent::IsForward);
      } else if ((k == WXK_UP || k == WXK_DOWN || k == WXK_PAGEUP || k == WXK_PAGEDOWN) && !e.HasAnyModifiers()) {
      } else {
        e.Skip();
      }
    });
    s->Bind(wxEVT_STC_CHARADDED, [this](wxStyledTextEvent& e) {
      e.Skip();
      const int pos = url_->GetCurrentPos();
      if (e.GetKey() == '{' && pos >= 2 && url_->GetCharAt(pos - 2) == '{') show_var_list(0);
    });
    s->Bind(wxEVT_STC_AUTOCOMP_SELECTION, [this](wxStyledTextEvent& e) {
      // The pick replaces the typed name; "}}" follows unless it is already there.
      e.Skip();
      CallAfter([this] {
        const int pos = url_->GetCurrentPos();
        if (url_->GetTextRange(pos, pos + 2) != "}}") url_->InsertText(pos, "}}");
        url_->GotoPos(pos + 2);
      });
    });
    s->Bind(wxEVT_STC_CHANGE, [this](wxStyledTextEvent& e) {
      e.Skip();
      if (loading_) return;
      refresh_vars();
      host_->changed();
    });
    s->Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) {
      e.Skip();
      box_->set_focus(true);
    });
    s->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) {
      e.Skip();
      box_->set_focus(false);
    });
    s->SetMouseDwellTime(350);
    s->Bind(wxEVT_STC_DWELLSTART, [this](wxStyledTextEvent& e) { var_tip(e.GetPosition()); });
    s->Bind(wxEVT_STC_DWELLEND, [this](wxStyledTextEvent&) { url_->CallTipCancel(); });
  }

  std::string warned_text_;  // the message held by confirm_message()

  void build_message_page() {
    const auto& t = http_editor::Theme::get();
    auto* pg = new wxPanel(book_);
    pg->SetBackgroundColour(t.bg);
    auto* s = new wxBoxSizer(wxVERTICAL);
    auto* bar = new wxBoxSizer(wxHORIZONTAL);
    mode_ = new http_editor::RadioStrip(pg, {"JSON", "Text"});
    mode_->on_select = [this](int) {
      apply_mode();
      if (!loading_) host_->changed();
    };
    bar->Add(mode_, 0, wxALIGN_CENTER_VERTICAL);
    bar->AddStretchSpacer();
    hint_ = new wxStaticText(pg, wxID_ANY, "Ctrl+Enter connects");
    hint_->SetForegroundColour(t.dim);
    bar->Add(hint_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    send_ = new FlatButton(pg, "Send", FlatButton::Style::Outline);
    send_->on_click = [this] {
      if (state_ == State::Open) host_->send();
    };
    bar->Add(send_, 0, wxALIGN_CENTER_VERTICAL);
    s->AddSpacer(FromDIP(8));
    s->Add(bar, 0, wxEXPAND);
    s->AddSpacer(FromDIP(6));
    body_ = new wxStyledTextCtrl(pg, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    style_stc(body_, this, true);
    body_->SetWrapMode(wxSTC_WRAP_WORD);
    body_->SetMarginLeft(FromDIP(6));
    body_->SetTabWidth(2);
    body_->SetUseTabs(false);
    body_->SetModEventMask(wxSTC_MOD_INSERTTEXT | wxSTC_MOD_DELETETEXT);
    body_->Bind(wxEVT_STC_CHANGE, [this](wxStyledTextEvent& e) {
      e.Skip();
      if (!loading_) host_->changed();
    });
    body_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
      // Shift+Alt+F (as in the HTTP body): re-indent JSON.
      if (e.GetKeyCode() == 'F' && e.GetModifiers() == (wxMOD_SHIFT | wxMOD_ALT) && mode_->selection() == 0) {
        const std::string in = body_->GetText().utf8_string();
        body_->SetText(wxString::FromUTF8(http_editor::reindent_json(in)));
        return;
      }
      e.Skip();
    });
    s->Add(body_, 1, wxEXPAND);
    pg->SetSizer(s);
    book_->AddPage(pg, "Message");
  }

  void build_headers_page() {
    const auto& t = http_editor::Theme::get();
    auto* pg = new wxPanel(book_);
    pg->SetBackgroundColour(t.bg);
    auto* s = new wxBoxSizer(wxVERTICAL);
    headers_ = new KvPanel(pg, KvGrid::Kind::Headers, "Headers");
    KvGrid* g = headers_->grid();
    g->lookup = [this](const std::string& n) { return resolve(n); };
    g->var_names = [this] { return var_names(); };
    g->on_change = [this] {
      if (!loading_ && !KvGrid::echo()) host_->changed();
      update_badges();
    };
    s->AddSpacer(FromDIP(6));
    s->Add(headers_, 1, wxEXPAND);
    inherited_ = new wxStaticText(pg, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
    inherited_->SetMinSize(wxSize(FromDIP(80), GetCharHeight()));
    inherited_->SetForegroundColour(t.dim);
    s->Add(inherited_, 0, wxEXPAND | wxTOP, FromDIP(6));
    auto* note = new wxStaticText(pg, wxID_ANY,
                                  "Sent with the upgrade request (Host, Upgrade and Sec-WebSocket-* are watt's). "
                                  "Add Sec-WebSocket-Protocol to ask for a subprotocol.",
                                  wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
    note->SetMinSize(wxSize(FromDIP(80), GetCharHeight()));
    note->SetToolTip(note->GetLabel());
    note->SetForegroundColour(t.dim);
    s->Add(note, 0, wxEXPAND | wxTOP, FromDIP(6));
    pg->SetSizer(s);
    book_->AddPage(pg, "Headers");
  }

  void build_auth_page() {
    const auto& t = http_editor::Theme::get();
    auto* pg = new wxPanel(book_);
    pg->SetBackgroundColour(t.bg);
    auto* s = new wxBoxSizer(wxVERTICAL);
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto* lbl = new wxStaticText(pg, wxID_ANY, "Type");
    lbl->SetForegroundColour(t.fg);
    row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    auth_ = new http_editor::ChoiceButton(pg, {"Inherit from parent", "No auth", "Bearer token", "Basic auth"});
    auth_->on_select = [this](int) {
      apply_auth_mode();
      if (!loading_) host_->changed();
    };
    row->Add(auth_, 0, wxALIGN_CENTER_VERTICAL);
    s->AddSpacer(FromDIP(10));
    s->Add(row, 0);
    auto field = [&](const wxString& label, wxTextCtrl*& out, bool password) {
      auto* r = new wxBoxSizer(wxHORIZONTAL);
      auto* l = new wxStaticText(pg, wxID_ANY, label);
      l->SetForegroundColour(t.fg);
      l->SetMinSize(wxSize(FromDIP(70), -1));
      r->Add(l, 0, wxALIGN_CENTER_VERTICAL);
      out = new wxTextCtrl(pg, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, password ? wxTE_PASSWORD : 0);
      out->SetBackgroundColour(t.field);
      out->SetForegroundColour(t.fg);
      out->SetHint(password ? "{{password}}" : "{{token}}");
      out->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
        if (!loading_) host_->changed();
      });
      r->Add(out, 1, wxALIGN_CENTER_VERTICAL);
      s->Add(r, 0, wxEXPAND | wxTOP, FromDIP(8));
      return r;
    };
    bearer_row_ = field("Token", bearer_, false);
    user_row_ = field("User", user_, false);
    user_->SetHint("user or {{user}}");
    pass_row_ = field("Password", pass_, true);
    auth_info_ = new wxStaticText(pg, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
    auth_info_->SetMinSize(wxSize(FromDIP(80), GetCharHeight()));
    auth_info_->SetForegroundColour(t.dim);
    s->Add(auth_info_, 0, wxEXPAND | wxTOP, FromDIP(10));
    auth_page_ = pg;
    pg->SetSizer(s);
    book_->AddPage(pg, "Auth");
  }

  void build_docs_page() {
    const auto& t = http_editor::Theme::get();
    auto* pg = new wxPanel(book_);
    pg->SetBackgroundColour(t.bg);
    auto* s = new wxBoxSizer(wxVERTICAL);
    docs_ = new wxTextCtrl(pg, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxBORDER_SIMPLE);
    docs_->SetBackgroundColour(t.field);
    docs_->SetForegroundColour(t.fg);
    docs_->SetHint("Notes about this connection (Markdown)");
    docs_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
      if (!loading_) host_->changed();
    });
    s->AddSpacer(FromDIP(8));
    s->Add(docs_, 1, wxEXPAND);
    pg->SetSizer(s);
    book_->AddPage(pg, "Docs");
  }

  void apply_mode() {
    if (mode_->selection() == 0) {
      json_colours(body_);
    } else {
      body_->SetLexer(wxSTC_LEX_NULL);
      style_stc(body_, this, true);
    }
    body_->Colourise(0, -1);
  }

  void apply_auth_mode() {
    const int m = auth_->selection();
    auth_page_->GetSizer()->Show(bearer_row_, m == kAuthBearer, true);
    auth_page_->GetSizer()->Show(user_row_, m == kAuthBasic, true);
    auth_page_->GetSizer()->Show(pass_row_, m == kAuthBasic, true);
    std::string info;
    if (m == kAuthInherit) {
      info = "Uses the auth of the nearest folder / the collection that sets one";
      if (const Workspace* ws = host_->workspace()) {
        SavedRequest r;
        r.kind = RequestKind::Http;
        r.file = host_->inherit_file();
        r.auth.type = HttpAuth::Type::Inherit;
        const EffectiveSettings eff = ws->effective_settings(r);
        const std::string type = eff.auth.type == HttpAuth::Type::OAuth2 ? "OAuth 2.0"
                                 : eff.auth.type == HttpAuth::Type::None
                                     ? (eff.basic_auth.enabled ? "Basic auth" : "no auth")
                                     : auth_type_name(eff.auth.type);
        info += ": " + type + (eff.auth_from.label.empty() ? std::string() : " (from " + eff.auth_from.label + ")");
      }
      info += ".";
    } else if (m == kAuthBearer) {
      info = "Sent as  Authorization: Bearer <token>  with the upgrade request. Keep tokens in {{secret}} variables.";
    } else if (m == kAuthBasic) {
      info = "Sent as  Authorization: Basic base64(user:password)  with the upgrade request.";
    } else {
      info = other_auth_.type != HttpAuth::Type::None ? std::string("Set in the file: ") + auth_type_name(other_auth_.type)
                                                      : "No Authorization header (headers you add are still sent).";
    }
    auth_info_->SetLabel(wxString::FromUTF8(info));
    auth_info_->SetToolTip(wxString::FromUTF8(info));
    auth_page_->Layout();
    update_badges();
  }

  void update_badges() {
    size_t n = headers_ ? headers_->grid()->enabled_count() : 0;
    tabs_->set_badge(1, n ? wxString::Format("%zu", n) : wxString());
    const int m = auth_ ? auth_->selection() : 0;
    tabs_->set_badge(2, m == kAuthInherit ? wxString("inherit") : m == kAuthNone ? wxString() : wxString::FromUTF8("\xE2\x97\x8F"));
  }

  SavedRequest probe() const {
    SavedRequest r;
    r.kind = RequestKind::Http;
    r.file = host_->inherit_file();
    r.id = req_id_;
    return r;
  }

  std::optional<std::string> resolve(const std::string& name) const {
    const Workspace* ws = host_->workspace();
    if (!ws) return std::nullopt;
    const std::string span = "{{" + name + "}}";
    std::string v = ws->expand(span, probe());
    if (v == span) return std::nullopt;
    return v;
  }

  bool is_secret(const std::string& name) const {
    const Workspace* ws = host_->workspace();
    if (!ws) return false;
    for (const auto& env : const_cast<Workspace*>(ws)->environments())
      if (env.name == ws->active_env) return env.secrets.count(name) > 0;
    return false;
  }

  std::vector<std::string> var_names() const {
    std::vector<std::string> out;
    const Workspace* ws = host_->workspace();
    if (!ws) return out;
    std::set<std::string> names;
    for (const auto& env : const_cast<Workspace*>(ws)->environments())
      if (env.name == ws->active_env)
        for (const auto& [k, v] : env.vars) names.insert(k);
    for (const auto& [k, v] : ws->globals) names.insert(k);
    for (const auto& [k, v] : ws->runtime_vars) names.insert(k);
    for (const auto& row : ws->effective_settings(probe()).vars)
      if (row.row.enabled) names.insert(row.row.name);
    for (const auto& n : names)
      if (!n.empty()) out.push_back(n);
    return out;
  }

  void show_var_list(int typed) {
    const auto names = var_names();
    if (names.empty()) return;
    wxString list;
    for (const auto& n : names) list += wxString::FromUTF8(n) + "\n";
    list.RemoveLast();
    url_->AutoCompShow(typed, list);
    guard_autocomp(url_);
  }

  void var_tip(int pos) {
    if (pos < 0) return;
    const std::string text = url_->GetText().utf8_string();
    for (const auto& v : http_editor::find_vars(text)) {
      if (static_cast<size_t>(pos) < v.begin || static_cast<size_t>(pos) >= v.end) continue;
      const auto val = resolve(v.name);
      std::string tip = val ? (is_secret(v.name) ? std::string("\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2 (secret)") : *val)
                            : "not defined in the active environment";
      url_->CallTipShow(static_cast<int>(v.begin), wxString::FromUTF8(v.name + " = " + tip));
      return;
    }
  }

  // {{var}} colours in the address and the resolved "Sent as" line (secrets masked).
  void refresh_vars() {
    const std::string text = url_->GetText().utf8_string();
    for (int ind : {kIndOk, kIndBad, kIndOk + 2, kIndBad + 2}) {
      url_->SetIndicatorCurrent(ind);
      url_->IndicatorClearRange(0, url_->GetLength());
    }
    std::string sent;
    size_t at = 0;
    std::vector<std::string> missing;
    for (const auto& v : http_editor::find_vars(text)) {
      const auto val = resolve(v.name);
      for (int ind : {val ? kIndOk : kIndBad, (val ? kIndOk : kIndBad) + 2}) {
        url_->SetIndicatorCurrent(ind);
        url_->IndicatorFillRange(static_cast<int>(v.begin), static_cast<int>(v.end - v.begin));
      }
      sent += text.substr(at, v.begin - at);
      if (!val) {
        missing.push_back(v.name);
        sent += text.substr(v.begin, v.end - v.begin);
      } else {
        sent += is_secret(v.name) ? std::string("\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2") : *val;
      }
      at = v.end;
    }
    sent += text.substr(std::min(at, text.size()));
    if (text.empty()) {
      url_->EOLAnnotationSetText(0, "ws://host:port/path  or  {{baseWs}}/chat");
      url_->EOLAnnotationSetStyle(0, 1);
    } else {
      url_->EOLAnnotationClearAll();
    }
    const auto& t = http_editor::Theme::get();
    std::string line;
    if (!missing.empty()) {
      line = "Not defined: ";
      for (size_t i = 0; i < missing.size(); ++i) line += (i ? ", {{" : "{{") + missing[i] + "}}";
      sent_as_->SetForegroundColour(t.var_bad);
    } else if (!text.empty() && sent != text) {
      line = "Connects to " + sent;
      sent_as_->SetForegroundColour(t.dim);
    } else if (!text.empty() && ws_http_url(sent).empty()) {
      line = "Use ws:// or wss:// (http:// and https:// also work)";
      sent_as_->SetForegroundColour(t.var_bad);
    } else {
      sent_as_->SetForegroundColour(t.dim);
    }
    sent_as_->SetLabel(wxString::FromUTF8(line));
    sent_as_->SetToolTip(wxString::FromUTF8(line));
    // Inherited headers, as the HTTP editor lists them.
    if (const Workspace* ws = host_->workspace(); ws && inherited_) {
      std::string inh;
      for (const auto& h : ws->effective_settings(probe()).headers)
        if (h.from.level != SettingsOrigin::Level::Request && h.row.enabled && !h.overridden)
          inh += (inh.empty() ? "" : ", ") + h.row.name + " (" + h.from.label + ")";
      inherited_->SetLabel(inh.empty() ? wxString() : wxString::FromUTF8("Inherited: " + inh));
    }
  }

  EditorHost* host_;
  bool loading_ = false;
  State state_ = State::Closed;
  std::filesystem::path shown_file_ = L"?";
  bool shown_ws_ = false;
  std::filesystem::path req_file_;
  std::string req_id_;
  HttpAuth other_auth_;
  wxStaticText* title_ = nullptr;
  Frame* box_ = nullptr;
  wxStaticText* badge_ = nullptr;
  wxStyledTextCtrl* url_ = nullptr;
  FlatButton* connect_ = nullptr;
  wxStaticText* sent_as_ = nullptr;
  http_editor::TabStrip* tabs_ = nullptr;
  wxSimplebook* book_ = nullptr;
  http_editor::RadioStrip* mode_ = nullptr;
  wxStaticText* hint_ = nullptr;
  FlatButton* send_ = nullptr;
  wxStyledTextCtrl* body_ = nullptr;
  KvPanel* headers_ = nullptr;
  wxStaticText* inherited_ = nullptr;
  http_editor::ChoiceButton* auth_ = nullptr;
  wxPanel* auth_page_ = nullptr;
  wxSizer *bearer_row_ = nullptr, *user_row_ = nullptr, *pass_row_ = nullptr;
  wxTextCtrl *bearer_ = nullptr, *user_ = nullptr, *pass_ = nullptr;
  wxStaticText* auth_info_ = nullptr;
  wxTextCtrl* docs_ = nullptr;
};

}  // namespace

WsEditorPanel* make_ws_editor(wxWindow* parent, EditorHost* host) { return new WsEditor(parent, host); }

// ------------------------------------------------------------------ response: message log

std::shared_ptr<HttpResponse> ws_handshake_response(const WsHandshake& hs, double queue_ms) {
  auto r = std::make_shared<HttpResponse>();
  r->ok = hs.status > 0;
  r->error = hs.ok ? std::string() : hs.error;
  r->status = hs.status;
  r->status_text = hs.status_text;
  r->http_version = hs.status ? "HTTP/1.1" : "";
  for (const auto& [k, v] : hs.headers) r->headers.add(k, v);
  r->body = hs.body;
  r->effective_url = hs.url;
  r->remote_ip = hs.remote_ip;
  r->remote_port = hs.remote_port;
  r->local_ip = hs.local_ip;
  r->local_port = hs.local_port;
  r->tls_version = hs.tls_version;
  r->conn.remote_ip = hs.remote_ip;
  r->conn.remote_port = hs.remote_port;
  r->conn.local_ip = hs.local_ip;
  r->conn.local_port = hs.local_port;
  r->conn.tls = hs.tls;
  r->conn.proxy = hs.proxy;
  r->prepare_ms = hs.prepare_ms;
  Timing& t = r->timing;
  t.queue_ms = queue_ms;
  t.dns_ms = hs.dns_ms;
  t.connect_ms = hs.connect_ms;
  t.tls_ms = hs.tls_ms;
  t.send_ms = hs.send_ms;
  t.ttfb_ms = hs.upgrade_ms;
  t.bytes_sent = hs.bytes_sent;
  t.bytes_received = hs.bytes_received;
  const double sum = r->prepare_ms + t.queue_ms + t.dns_ms + t.connect_ms + t.tls_ms + t.send_ms + t.ttfb_ms;
  r->process_ms = std::max(0.0, queue_ms + hs.total_ms - sum);
  t.total_ms = sum + r->process_ms;
  r->sizes.request_headers = hs.bytes_sent;
  r->sizes.response_headers = hs.bytes_received;
  return r;
}

namespace {

struct LogRow {
  enum Kind : unsigned char { In, Out, Info, Good, Bad } kind = In;
  bool binary = false;
  double at_ms = 0;
  uint64_t size = 0;
  std::string preview;                       // one line
  std::shared_ptr<const std::string> data;   // the whole message (or the info text)
};

std::string preview_of(const std::string& d, bool binary) {
  std::string p;
  if (binary) {
    static const char* hx = "0123456789abcdef";
    for (size_t i = 0; i < d.size() && i < 24; ++i) {
      const auto c = static_cast<unsigned char>(d[i]);
      p += hx[c >> 4];
      p += hx[c & 15];
      p += ' ';
    }
    if (d.size() > 24) p += "\xE2\x80\xA6";
    return p;
  }
  const size_t n = std::min<size_t>(d.size(), 240);
  p.reserve(n);
  bool sp = false;
  for (size_t i = 0; i < n; ++i) {
    const char c = d[i];
    if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
      if (!sp && !p.empty()) p += ' ';
      sp = true;
      continue;
    }
    sp = false;
    p += static_cast<unsigned char>(c) < 32 ? '.' : c;
  }
  // Cut at a UTF-8 boundary.
  while (!p.empty() && (static_cast<unsigned char>(p.back()) & 0xC0) == 0x80) p.pop_back();
  if (!p.empty() && static_cast<unsigned char>(p.back()) >= 0xC0) p.pop_back();
  if (d.size() > n) p += "\xE2\x80\xA6";
  return p;
}

class LogList : public wxVListBox {
 public:
  explicit LogList(wxWindow* parent) : wxVListBox(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE) {
    const auto& t = rv::Theme::get();
    SetBackgroundColour(t.bg);
    SetSelectionBackground(t.dark ? wxColour(0x26, 0x4F, 0x78) : wxColour(0xCC, 0xE4, 0xFF));
    mono_ = wxFont(wxFontInfo(GetFont().GetPointSize()).Family(wxFONTFAMILY_TELETYPE).FaceName("Consolas"));
    row_h_ = std::max(GetCharHeight(), GetTextExtent("Ag").y) + FromDIP(8);
  }
  std::vector<LogRow> rows;
  std::vector<uint32_t> view;  // indices of rows shown (filter)
  std::string filter;          // lower case
  int dir = 0;                 // 0 all, 1 sent, 2 received

  bool matches(const LogRow& r) const {
    if (dir == 1 && r.kind != LogRow::Out) return false;
    if (dir == 2 && r.kind != LogRow::In) return false;
    if (filter.empty()) return true;
    if (!r.data) return false;
    const std::string& d = *r.data;
    if (d.size() > (4u << 20)) return lower(r.preview).find(filter) != std::string::npos;
    auto it = std::search(d.begin(), d.end(), filter.begin(), filter.end(),
                          [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
    return it != d.end();
  }
  void rebuild_view() {
    view.clear();
    for (size_t i = 0; i < rows.size(); ++i)
      if (matches(rows[i])) view.push_back(static_cast<uint32_t>(i));
    SetItemCount(view.size());
    Refresh();
  }
  // Appends rows[from..]: keeps following the end when it was showing it.
  void appended(size_t from) {
    const size_t before = view.size();
    const bool follow = before == 0 || GetVisibleRowsEnd() >= before;
    for (size_t i = from; i < rows.size(); ++i)
      if (matches(rows[i])) view.push_back(static_cast<uint32_t>(i));
    if (view.size() == before) return;
    SetItemCount(view.size());
    if (follow) ScrollToRow(view.size() - 1);
    RefreshRows(before, view.size());
  }
  const LogRow* row_at(int sel) const {
    return sel >= 0 && static_cast<size_t>(sel) < view.size() ? &rows[view[static_cast<size_t>(sel)]] : nullptr;
  }

 protected:
  wxCoord OnMeasureItem(size_t) const override { return row_h_; }
  void OnDrawBackground(wxDC& dc, const wxRect& rect, size_t n) const override {
    const auto& t = rv::Theme::get();
    const LogRow* r = row_at(static_cast<int>(n));
    wxColour bg = t.bg;
    if (r && (r->kind == LogRow::Info || r->kind == LogRow::Good || r->kind == LogRow::Bad))
      bg = t.dark ? wxColour(0x2A, 0x2A, 0x2E) : wxColour(0xF3, 0xF4, 0xF6);
    if (IsSelected(n)) bg = GetSelectionBackground();
    dc.SetBrush(wxBrush(bg));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(rect);
    dc.SetPen(wxPen(t.grid));
    dc.DrawLine(rect.GetLeft(), rect.GetBottom(), rect.GetRight() + 1, rect.GetBottom());
  }
  void OnDrawItem(wxDC& dc, const wxRect& rect, size_t n) const override {
    const LogRow* r = row_at(static_cast<int>(n));
    if (!r) return;
    const auto& t = rv::Theme::get();
    const int pad = FromDIP(8);
    int x = rect.x + pad;
    const int cy = rect.y + (rect.height - GetCharHeight()) / 2;
    dc.SetFont(GetFont());
    // Direction.
    const char* glyph = r->kind == LogRow::Out    ? "\xE2\x86\x91"   // up arrow: sent
                        : r->kind == LogRow::In   ? "\xE2\x86\x93"   // down arrow: received
                        : r->kind == LogRow::Good ? "\xE2\x97\x8F"   // dot
                        : r->kind == LogRow::Bad  ? "\xE2\x9C\x95"   // cross
                                                  : "\xE2\x80\xA2";  // bullet
    const wxColour gc = r->kind == LogRow::Out    ? t.ok
                        : r->kind == LogRow::In   ? t.info
                        : r->kind == LogRow::Good ? t.ok
                        : r->kind == LogRow::Bad  ? t.err
                                                  : t.dim;
    wxFont bold = GetFont().Bold();
    dc.SetFont(bold);
    dc.SetTextForeground(gc);
    dc.DrawText(wxString::FromUTF8(glyph), x, cy);
    x += FromDIP(18);
    dc.SetFont(GetFont());
    // Offset and size columns.
    dc.SetTextForeground(t.dim);
    const wxString off = wxString::FromUTF8(offset_text(r->at_ms));
    const int off_w = dc.GetTextExtent("+000.000 ms").x + FromDIP(8);
    dc.DrawText(off, x, cy);
    x += off_w;
    const bool msg = r->kind == LogRow::In || r->kind == LogRow::Out;
    const int size_w = dc.GetTextExtent("000.0 KB").x + FromDIP(10);
    if (msg) {
      const wxString sz = wxString::FromUTF8(fmt::human_bytes(r->size));
      dc.DrawText(sz, x + size_w - FromDIP(10) - dc.GetTextExtent(sz).x, cy);
    }
    x += msg ? size_w : 0;
    // Preview.
    const int avail = rect.GetRight() - pad - x;
    if (avail <= FromDIP(10)) return;
    if (msg) {
      dc.SetFont(mono_);
      dc.SetTextForeground(r->binary ? t.dim : t.fg);
    } else {
      dc.SetTextForeground(r->kind == LogRow::Bad ? t.err : r->kind == LogRow::Good ? t.fg : t.dim);
    }
    const wxString pv = wxControl::Ellipsize(wxString::FromUTF8(r->preview), dc, wxELLIPSIZE_END, avail, wxELLIPSIZE_FLAGS_EXPAND_TABS);
    const int py = rect.y + (rect.height - dc.GetCharHeight()) / 2;
    dc.DrawText(pv, x, py);
  }

 private:
  wxFont mono_;
  int row_h_ = 20;
};

constexpr int kChipAll = 1, kChipSent = 2, kChipRecv = 3, kChipClear = 4, kChipPretty = 5;

class WsView : public wxPanel, public WsResponsePanel {
 public:
  explicit WsView(wxWindow* parent) : wxPanel(parent, wxID_ANY) {
    const auto& t = rv::Theme::get();
    SetBackgroundColour(t.bg);
    auto* sz = new wxBoxSizer(wxVERTICAL);
    status_ = new rv::StatusStrip(this);
    sz->Add(status_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    wf_ = new rv::Waterfall(this);
    wf_->reserve(8);
    sz->Add(wf_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    tabs_ = new rv::TabStrip(this, [this](int i) { book_->ChangeSelection(static_cast<size_t>(i)); });
    tabs_->add("Messages");
    tabs_->add("Handshake");
    sz->Add(tabs_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    book_ = new wxSimplebook(this, wxID_ANY);
    book_->SetBackgroundColour(t.bg);
    // Messages.
    auto* mp = new wxPanel(book_);
    mp->SetBackgroundColour(t.bg);
    auto* ms = new wxBoxSizer(wxVERTICAL);
    auto* fr = new wxBoxSizer(wxHORIZONTAL);
    chips_ = new rv::ChipBar(mp, [this](int id) { chip(id); });
    chips_->add(kChipAll, "All", "Every message and connection event", 0);
    chips_->add(kChipSent, "Sent", "Messages this client sent", 0);
    chips_->add(kChipRecv, "Received", "Messages from the server", 0);
    chips_->add(kChipClear, "Clear", "Clear the message log", -1, true);
    chips_->set_on(kChipAll, true);
    fr->Add(chips_, 1, wxEXPAND);
    filter_ = new wxTextCtrl(mp, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(180), -1));
    filter_->SetHint("Filter messages");
    filter_->SetBackgroundColour(t.dark ? wxColour(0x25, 0x25, 0x28) : *wxWHITE);
    filter_->SetForegroundColour(t.fg);
    filter_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
      list_->filter = lower(filter_->GetValue().utf8_string());
      list_->rebuild_view();
      show_selected();
    });
    fr->Add(filter_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    ms->Add(fr, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(4));
    split_ = new wxSplitterWindow(mp, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSP_LIVE_UPDATE | wxSP_NOBORDER | wxSP_THIN_SASH);
    split_->SetSashGravity(0.6);
    split_->SetMinimumPaneSize(FromDIP(60));
    list_ = new LogList(split_);
    list_->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { show_selected(); });
    detail_ = new wxStyledTextCtrl(split_, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    style_stc(detail_, this, true);
    detail_->SetWrapMode(wxSTC_WRAP_WORD);
    detail_->SetMarginLeft(FromDIP(8));
    detail_->SetReadOnly(true);
    split_->SplitHorizontally(list_, detail_);
    ms->Add(split_, 1, wxEXPAND);
    mp->SetSizer(ms);
    book_->AddPage(mp, "Messages");
    // Handshake.
    auto* hp = new wxPanel(book_);
    hp->SetBackgroundColour(t.bg);
    auto* hs = new wxBoxSizer(wxVERTICAL);
    hs_text_ = new wxStyledTextCtrl(hp, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    style_stc(hs_text_, this, true);
    hs_text_->SetWrapMode(wxSTC_WRAP_WORD);
    hs_text_->SetMarginLeft(FromDIP(8));
    hs_text_->StyleSetForeground(1, t.accent);
    hs_text_->StyleSetBold(1, true);
    hs_text_->StyleSetFont(1, hs_text_->StyleGetFont(0).Bold());
    hs_text_->StyleSetBackground(1, http_editor::Theme::get().field);
    hs_text_->SetReadOnly(true);
    hs->Add(hs_text_, 1, wxEXPAND | wxTOP, FromDIP(4));
    hp->SetSizer(hs);
    book_->AddPage(hp, "Handshake");
    sz->Add(book_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    SetSizer(sz);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      if (!sash_set_ && split_->GetClientSize().y > FromDIP(200)) {
        sash_set_ = true;
        CallAfter([this] { split_->SetSashPosition(split_->GetClientSize().y * 3 / 5); });
      }
    });
    clear();
  }

  wxWindow* window() override { return this; }

  void clear() override {
    list_->rows.clear();
    list_->rebuild_view();
    set_detail({}, false);
    hs_text_set({});
    in_ = out_ = 0;
    bytes_in_ = bytes_out_ = 0;
    open_ = false;
    status_->set("", rv::Tone::Plain, {{"Not connected", "", rv::Tone::Dim}, {"Connect to open the WebSocket (Ctrl+Enter)", "", rv::Tone::Dim, false, 1}});
    wf_->clear(true);
    tabs_->set_label(0, "Messages");
  }

  void show_sending(RequestKind) override {
    connect_t0_ = Clock::now();
    open_off_ms_ = 0;
    status_->set_busy("Connecting\xE2\x80\xA6", connect_t0_);
    wf_->set_waiting(connect_t0_);
    add_info(LogRow::Info, "Connecting\xE2\x80\xA6");
  }

  void show_http(std::shared_ptr<const HttpResponse> resp, std::shared_ptr<const SentHttp> sent) override {
    // A restored session's handshake (the messages are not kept).
    if (!resp) return;
    WsHandshake hs;
    hs.ok = resp->status == 101;
    hs.status = resp->status;
    hs.status_text = resp->status_text;
    hs.url = resp->effective_url;
    hs.error = resp->error;
    hs.headers = resp->headers.to_vector();
    hs.remote_ip = resp->remote_ip;
    hs.remote_port = resp->remote_port;
    hs.tls_version = resp->tls_version;
    hs.prepare_ms = resp->prepare_ms;
    hs.dns_ms = resp->timing.dns_ms;
    hs.connect_ms = resp->timing.connect_ms;
    hs.tls_ms = resp->timing.tls_ms;
    hs.tls = resp->conn.tls;
    hs.send_ms = resp->timing.send_ms;
    hs.upgrade_ms = resp->timing.ttfb_ms;
    hs.total_ms = resp->timing.total_ms - resp->timing.queue_ms;
    restored_ = true;
    ws_handshake(std::make_shared<const WsHandshake>(std::move(hs)),
                 sent ? std::make_shared<const HttpRequest>(sent->request) : nullptr, resp->timing.queue_ms);
    open_ = false;
    restored_ = false;
    std::vector<rv::StatusSeg> segs = last_segs_;
    segs.insert(segs.begin(), rv::StatusSeg{"Closed \xC2\xB7 last session", "The connection of an earlier session", rv::Tone::Dim, true});
    status_->set(last_badge_, rv::Tone::Dim, segs);
  }

  void append_grpc_messages(const std::vector<GrpcMessageEvent>&) override {}
  void show_grpc(std::shared_ptr<const GrpcResponse>) override {}

  void show_cancelled(double after_ms) override {
    open_ = false;
    status_->set("", rv::Tone::Plain, {{"Cancelled", "", rv::Tone::Warn, true}, {"after " + ms_text(after_ms), "", rv::Tone::Dim}});
    wf_->set_cancelled(after_ms);
    add_info(LogRow::Bad, "Cancelled after " + ms_text(after_ms));
  }

  void show_error(const std::string& message) override {
    open_ = false;
    status_->set("Error", rv::Tone::Err, {{message, message, rv::Tone::Err}});
    wf_->clear(true);
    add_info(LogRow::Bad, message);
  }

  void ws_handshake(std::shared_ptr<const WsHandshake> hs, std::shared_ptr<const HttpRequest> sent, double queue_ms) override {
    if (!hs) return;
    hs_ = hs;
    // Waterfall: the handshake phases.
    TimingModel_set(*hs, queue_ms);
    std::string text;
    text += "\x01" "Request\n";
    text += hs->request_head.empty() ? std::string("(not sent)\n") : hs->request_head;
    if (hs->status) {
      text += "\x01" "Response\n";
      text += "HTTP/1.1 " + std::to_string(hs->status) + " " + hs->status_text + "\n";
      for (const auto& [k, v] : hs->headers) text += k + ": " + v + "\n";
      if (!hs->body.empty()) text += "\n" + hs->body + "\n";
    }
    text += "\x01" "Connection\n";
    text += "URL          " + hs->url + "\n";
    if (!hs->remote_ip.empty()) text += "Remote       " + hs->remote_ip + ":" + std::to_string(hs->remote_port) + "\n";
    if (!hs->local_ip.empty()) text += "Local        " + hs->local_ip + ":" + std::to_string(hs->local_port) + "\n";
    if (hs->tls) text += "TLS          " + (hs->tls_version.empty() ? std::string("yes") : hs->tls_version) + "\n";
    if (!hs->proxy.empty()) text += "Proxy        " + hs->proxy + "\n";
    if (!hs->subprotocol.empty()) text += "Subprotocol  " + hs->subprotocol + "\n";
    (void)sent;
    hs_text_set(text);
    const double total = queue_ms + hs->total_ms;
    std::vector<rv::StatusSeg> segs;
    segs.push_back({"handshake " + ms_text(total), "Click -> 101 received: queue, DNS, TCP, TLS and the upgrade", rv::Tone::Plain, true});
    segs.push_back({hs->url, "", rv::Tone::Dim, false, 3});
    if (hs->tls && !hs->tls_version.empty()) segs.push_back({hs->tls_version, "", rv::Tone::Dim, false, 4});
    if (!hs->subprotocol.empty()) segs.push_back({"protocol " + hs->subprotocol, "Sec-WebSocket-Protocol", rv::Tone::Dim, false, 2});
    last_segs_ = segs;
    if (hs->ok) {
      open_ = true;
      last_badge_ = "101 Switching Protocols";
      segs.insert(segs.begin(), rv::StatusSeg{"\xE2\x97\x8F Connected", "The connection is open", rv::Tone::Ok, true});
      status_->set(last_badge_, rv::Tone::Ok, segs);
      open_off_ms_ = total;  // message offsets count from the Connect click, like this row's
      if (!restored_) add_info(LogRow::Good, "Connected to " + hs->url + "  (101 Switching Protocols, " + ms_text(total) + ")", total);
    } else {
      open_ = false;
      last_badge_ = hs->status ? std::to_string(hs->status) + (hs->status_text.empty() ? "" : " " + hs->status_text) : "Failed";
      std::vector<rv::StatusSeg> fs{{hs->error, hs->error, rv::Tone::Err}};
      fs.insert(fs.end(), segs.begin() + 1, segs.end());
      status_->set(last_badge_, hs->status >= 400 || !hs->status ? rv::Tone::Err : rv::Tone::Warn, fs);
      if (!restored_) {
        add_info(LogRow::Bad, "Could not connect: " + hs->error);
        if (!hs->body.empty()) add_info(LogRow::Info, "Server said: " + hs->body.substr(0, 2000));
      }
      if (hs->status) tabs_->select(1), book_->ChangeSelection(1);
    }
  }

  void ws_messages(std::vector<WsMessage>&& batch) override {
    const size_t from = list_->rows.size();
    list_->rows.reserve(from + batch.size());
    for (WsMessage& m : batch) {
      LogRow r;
      r.kind = m.out ? LogRow::Out : LogRow::In;
      r.binary = m.binary;
      r.at_ms = open_off_ms_ + m.at_ms;
      r.size = m.data.size();
      r.preview = preview_of(m.data, m.binary);
      (m.out ? out_ : in_)++;
      (m.out ? bytes_out_ : bytes_in_) += m.data.size();
      r.data = std::make_shared<const std::string>(std::move(m.data));
      list_->rows.push_back(std::move(r));
    }
    list_->appended(from);
    counts();
  }

  void ws_closed(const WsClose& c) override {
    open_ = false;
    const char* name = ws_close_code_name(c.code);
    std::string what = "Disconnected  " + std::to_string(c.code) + (*name ? std::string(" ") + name : std::string());
    if (!c.reason.empty()) what += ": " + c.reason;
    what += c.by_client ? "  (closed by watt)" : c.code == 1006 ? "" : "  (closed by the server)";
    if (!c.error.empty() && c.error != "cancelled") what += "  \xC2\xB7 " + c.error;
    add_info(c.code == 1000 || c.code == 1001 || c.code == 1005 ? LogRow::Info : LogRow::Bad, what, open_off_ms_ + c.at_ms);
    std::vector<rv::StatusSeg> segs = last_segs_;
    std::string tag = "Closed " + std::to_string(c.code) + (*name ? std::string(" ") + name : std::string());
    if (!c.reason.empty()) tag += " \xC2\xB7 " + c.reason;
    segs.insert(segs.begin(), rv::StatusSeg{tag, c.error, c.code == 1000 ? rv::Tone::Dim : rv::Tone::Warn, true});
    segs.push_back({"open " + offset_text(c.at_ms).substr(1), "How long the connection was open", rv::Tone::Dim, false, 5});
    status_->set(last_badge_, rv::Tone::Dim, segs);
    counts();
  }

  void ws_info(const std::string& text, bool error) override { add_info(error ? LogRow::Bad : LogRow::Info, text); }

 private:
  void TimingModel_set(const WsHandshake& hs, double queue_ms) {
    const bool d = rv::Theme::get().dark;
    const wxColour setup = d ? wxColour(130, 130, 140) : wxColour(150, 150, 160);
    const wxColour dns = d ? wxColour(40, 190, 190) : wxColour(0, 150, 150);
    const wxColour con = d ? wxColour(70, 130, 240) : wxColour(40, 100, 220);
    const wxColour tls = d ? wxColour(170, 110, 240) : wxColour(140, 80, 210);
    const wxColour send = d ? wxColour(235, 175, 60) : wxColour(215, 150, 20);
    const wxColour wait = d ? wxColour(80, 200, 120) : wxColour(30, 160, 80);
    rv::TimingModel m;
    m.valid = true;
    // The waterfall keeps one row floor per kind ("grpc": 8, everything else: 10) so tabs of a
    // kind line up; the handshake's 8 rows share the gRPC floor instead of leaving an empty band.
    m.kind = "grpc";
    m.conn_note = hs.tls ? "new connection \xC2\xB7 " + (hs.tls_version.empty() ? std::string("TLS") : hs.tls_version)
                         : "new connection";
    const double other = std::max(0.0, hs.total_ms - hs.prepare_ms - hs.dns_ms - hs.connect_ms - hs.tls_ms - hs.send_ms - hs.upgrade_ms);
    auto add = [&](const char* n, double v, const char* tip, wxColour c, bool has = true) {
      rv::Phase p{n, v, tip, c};
      p.has_value = has;
      m.phases.push_back(std::move(p));
    };
    // Prepare before Queue, as in the HTTP waterfall.
    add("Prepare", hs.prepare_ms, "watt's connection setup: URL parse, libcurl options.", setup);
    add("Queue", queue_ms, "Click -> the tab's worker built the upgrade request (http_to_send: inherited headers, auth, {{vars}}, OAuth token).", setup);
    add("DNS", hs.dns_ms, "Name resolution.", dns);
    add("Connect", hs.connect_ms, "TCP handshake (through the proxy tunnel when there is one).", con);
    add("TLS", hs.tls_ms, "TLS handshake (wss://). n/a for ws://.", tls, hs.tls);
    add("Upgrade send", hs.send_ms, "Writing the HTTP/1.1 upgrade request (GET with Upgrade: websocket).", send);
    add("Upgrade (101)", hs.upgrade_ms, "Upgrade request sent -> 101 Switching Protocols received: server processing plus one round trip.", wait);
    add("Process", other, "Checking Sec-WebSocket-Accept and setting up the connection.", setup);
    if (!hs.ok) {
      for (auto& p : m.phases)
        if (p.ms <= 0 && &p != &m.phases.front()) p.has_value = false;
      // The phase it failed in: the first one without a value after one with a value.
      for (size_t i = 1; i < m.phases.size(); ++i)
        if (!m.phases[i].has_value && m.phases[i].name != "TLS") {
          m.phases[i].failed = true;
          break;
        }
    }
    m.total = queue_ms + hs.total_ms;
    m.server_wait = hs.upgrade_ms;
    m.server_label = "server (101)";
    m.server_tip = "Upgrade request sent -> 101 received.";
    m.client = queue_ms + hs.prepare_ms + other;
    m.network = std::max(0.0, m.total - m.server_wait - m.client);
    m.client_tip = "watt's own time: queue, request build, accept-key check.";
    m.network_tip = "DNS, TCP, TLS and writing the upgrade request.";
    m.total_tip = "Click -> connection open (101 received and checked): the wall time you waited.";
    wf_->set(m, has_prev_ ? &prev_ : nullptr);
    prev_ = m;
    has_prev_ = true;
  }

  void counts() {
    tabs_->set_label(0, "Messages", in_ + out_ ? std::to_string(in_ + out_) : std::string());
    std::string c = "\xE2\x86\x91 " + std::to_string(out_) + "  \xE2\x86\x93 " + std::to_string(in_) + "  \xC2\xB7  " +
                    fmt::human_bytes(bytes_in_ + bytes_out_);
    chips_->set_note(c, rv::Tone::Dim);
  }

  void add_info(LogRow::Kind k, const std::string& text, double at = -1) {
    LogRow r;
    r.kind = k;
    // Without a time of its own: now, since the Connect click (0 before any connect).
    r.at_ms = at >= 0 ? at : connect_t0_ == Clock::time_point{} ? 0 : ms_between(connect_t0_, Clock::now());
    r.preview = text;
    r.data = std::make_shared<const std::string>(text);
    const size_t from = list_->rows.size();
    list_->rows.push_back(std::move(r));
    list_->appended(from);
  }

  void chip(int id) {
    if (id == kChipClear) {
      list_->rows.clear();
      in_ = out_ = 0;
      bytes_in_ = bytes_out_ = 0;
      list_->rebuild_view();
      set_detail({}, false);
      counts();
      return;
    }
    chips_->set_on(kChipAll, id == kChipAll);
    chips_->set_on(kChipSent, id == kChipSent);
    chips_->set_on(kChipRecv, id == kChipRecv);
    list_->dir = id == kChipSent ? 1 : id == kChipRecv ? 2 : 0;
    list_->rebuild_view();
    show_selected();
  }

  void show_selected() {
    const LogRow* r = list_->row_at(list_->GetSelection());
    if (!r || !r->data) {
      set_detail({}, false);
      return;
    }
    const std::string& d = *r->data;
    if (r->binary) {
      std::string out;
      bool cut = false;
      fmt::hex_dump(d, out, 64 * 1024, &cut);
      if (cut) out += "\n(first 64 KB of " + fmt::human_bytes(d.size()) + ")";
      set_detail(out, false);
      return;
    }
    if ((r->kind == LogRow::In || r->kind == LogRow::Out) && d.size() < (8u << 20) && http_editor::looks_like_json(d)) {
      std::string pretty;
      if (fmt::pretty_json(d, pretty)) {
        set_detail(pretty, true);
        return;
      }
    }
    set_detail(d, false);
  }

  void set_detail(const std::string& text, bool json) {
    detail_->SetReadOnly(false);
    if (json) json_colours(detail_);
    else {
      detail_->SetLexer(wxSTC_LEX_NULL);
      style_stc(detail_, this, true);
    }
    detail_->SetText(wxString::FromUTF8(text));
    detail_->Colourise(0, -1);
    detail_->SetReadOnly(true);
    if (text.empty()) {
      detail_->EOLAnnotationSetVisible(wxSTC_EOLANNOTATION_STANDARD);
      detail_->StyleSetForeground(2, rv::Theme::get().dim);
      detail_->StyleSetBackground(2, http_editor::Theme::get().field);
      detail_->EOLAnnotationSetText(0, "Select a message to see it here (JSON pretty-printed)");
      detail_->EOLAnnotationSetStyle(0, 2);
    } else {
      detail_->EOLAnnotationClearAll();
    }
  }

  void hs_text_set(const std::string& text) {
    hs_text_->SetReadOnly(false);
    std::string plain;
    std::vector<std::pair<int, int>> heads;
    size_t i = 0;
    while (i < text.size()) {
      if (text[i] == '\x01') {
        const size_t e = text.find('\n', i);
        const std::string h = text.substr(i + 1, e - i - 1);
        while (plain.size() >= 2 && plain.back() == '\n' && plain[plain.size() - 2] == '\n') plain.pop_back();
        if (!plain.empty()) plain += "\n";
        heads.emplace_back(static_cast<int>(plain.size()), static_cast<int>(h.size()));
        plain += h + "\n";
        i = e == std::string::npos ? text.size() : e + 1;
        continue;
      }
      const size_t e = text.find('\x01', i);
      std::string chunk = text.substr(i, e == std::string::npos ? std::string::npos : e - i);
      // \r\n of the raw request head -> \n
      chunk.erase(std::remove(chunk.begin(), chunk.end(), '\r'), chunk.end());
      plain += chunk;
      i = e == std::string::npos ? text.size() : e;
    }
    hs_text_->SetText(wxString::FromUTF8(plain));
    for (const auto& [at, len] : heads) {
      hs_text_->StartStyling(at);
      hs_text_->SetStyling(len, 1);
    }
    hs_text_->SetReadOnly(true);
  }

  rv::StatusStrip* status_ = nullptr;
  rv::Waterfall* wf_ = nullptr;
  rv::TabStrip* tabs_ = nullptr;
  wxSimplebook* book_ = nullptr;
  rv::ChipBar* chips_ = nullptr;
  wxTextCtrl* filter_ = nullptr;
  wxSplitterWindow* split_ = nullptr;
  LogList* list_ = nullptr;
  wxStyledTextCtrl* detail_ = nullptr;
  wxStyledTextCtrl* hs_text_ = nullptr;
  bool sash_set_ = false;
  bool open_ = false;
  bool restored_ = false;
  std::shared_ptr<const WsHandshake> hs_;
  Clock::time_point connect_t0_{};
  double open_off_ms_ = 0;  // Connect click -> open: added to the engine's since-open offsets
  size_t in_ = 0, out_ = 0;
  uint64_t bytes_in_ = 0, bytes_out_ = 0;
  std::string last_badge_;
  std::vector<rv::StatusSeg> last_segs_;
  rv::TimingModel prev_;
  bool has_prev_ = false;
};

}  // namespace

WsResponsePanel* make_ws_view(wxWindow* parent) { return new WsView(parent); }

}  // namespace watt::app
