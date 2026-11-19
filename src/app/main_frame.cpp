#include "app/main_frame.h"

#include <wx/accel.h>
#include <wx/app.h>
#include <wx/config.h>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/display.h>
#include <wx/iconbndl.h>
#include <wx/vlbox.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/wupdlock.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#include "app/history_summary.h"
#include "app/http_editor_widgets.h"  // themed field, dropdown and buttons of the Save dialog
#include "app/request_tab.h"
#include "app/response_widgets.h"  // rv::Theme (the "+" button)
#include "app/runner_view.h"
#include "app/mock_view.h"
#include "app/sidebar_quickopen.h"
#include "core/model_json.h"  // session file: read / atomic write, UTF-8 paths

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <windows.h>

namespace watt::app {

namespace {

enum {
  ID_SEND = MainFrame::kSendCommand,
  ID_NEW_TAB,
  ID_NEW_GRPC_TAB,
  ID_CLOSE_TAB,
  ID_FOCUS_URL,
  ID_SAVE,
  ID_QUICK_OPEN,
  ID_NEXT_TAB,
  ID_PREV_TAB,
  ID_FOCUS_FILTER,
  ID_NEW_WS_TAB,
};

// Closed tabs parked for teardown at most (beyond that the oldest is destroyed at once), and the
// USER object count above which no spare is built (Windows' limit is 10,000 per process; past it
// every CreateWindow fails and wx crashes on the null window).
constexpr size_t kMaxParked = 8;
constexpr DWORD kUserBusy = 7500;
// A spare taken is refilled after this long without input: one slice is at most ~30 ms, so a key
// pressed during it still shows well inside 50 ms, and tabs opened every 300 ms each find one.
constexpr int kRefillQuietMs = 60;
DWORD user_objects() { return ::GetGuiResources(::GetCurrentProcess(), GR_USEROBJECTS); }

// The frame, for background threads that finish after it is gone (cleared in ~MainFrame).
std::atomic<MainFrame*> g_frame{nullptr};

// WATT_FRAME_TRACE=<file>: appends "what.part ms" lines for the frame's own UI-thread steps
// (opening a request or history entry into a tab, prebuilding spares) so their cost can be
// measured in the running app. Off (one getenv) otherwise.
FILE* frame_trace_file() {
  static FILE* f = []() -> FILE* {
    const wchar_t* p = _wgetenv(L"WATT_FRAME_TRACE");
    return p && *p ? _wfopen(p, L"a") : nullptr;
  }();
  return f;
}
struct FrameLaps {
  const char* what;
  std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
  explicit FrameLaps(const char* w) : what(w) {}
  void lap(const char* part) {
    if (FILE* f = frame_trace_file()) {
      const auto now = std::chrono::steady_clock::now();
      std::fprintf(f, "%s.%s %.2f\n", what, part, std::chrono::duration<double, std::milli>(now - t).count());
      std::fflush(f);
      t = now;
    }
  }
};

// Reads the workspace's session on the opener thread for the frame (see the session section).
void prefetch_session(const Workspace& ws);
std::shared_ptr<MainFrame::Session> take_session(const std::filesystem::path& root);

// Every saved request, for Ctrl+P (the sidebar's quick-open dialog and item type).
void gather(const CollectionNode& n, const std::string& folder, std::vector<sidebar::QuickItem>& out) {
  for (const auto& c : n.children) {
    if (c.is_folder) {
      gather(c, folder.empty() ? c.name : folder + "/" + c.name, out);
      continue;
    }
    sidebar::QuickItem it;
    it.file = c.path;
    it.name = c.name;
    it.folder = folder;
    it.kind = c.kind;
    it.method = c.method;
    it.badge = c.badge;
    out.push_back(std::move(it));
  }
}


// The tab strip's "+" (new HTTP tab), drawn by the notebook's own tab art: wxAuiNotebook draws
// custom buttons only through its art provider, which knows just its own bitmaps.
constexpr int kPlusButton = 0x6E01;

int tab_list(wxWindow* wnd, const wxAuiNotebookPageArray& items, int active);

class PlusTabArt : public wxAuiFlatTabArt {
 public:
  wxAuiTabArt* Clone() override { return new PlusTabArt(); }
  int GetButtonRect(wxReadOnlyDC& dc, wxWindow* wnd, const wxRect& in, int id, int state, int orientation,
                    wxRect* out) override {
    if (id != kPlusButton) return wxAuiFlatTabArt::GetButtonRect(dc, wnd, in, id, state, orientation, out);
    const wxRect r = place(wnd, in, orientation);
    if (out) *out = r;
    return r.width;
  }
  void DrawButton(wxDC& dc, wxWindow* wnd, const wxRect& in, int id, int state, int orientation,
                  wxRect* out) override {
    if (id != kPlusButton) return wxAuiFlatTabArt::DrawButton(dc, wnd, in, id, state, orientation, out);
    const wxRect r = place(wnd, in, orientation);
    const auto& th = rv::Theme::get();
    if (state & (wxAUI_BUTTON_STATE_HOVER | wxAUI_BUTTON_STATE_PRESSED)) {
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.SetBrush(wxBrush(th.grid));
      dc.DrawRoundedRectangle(wxRect(r).Deflate(wnd->FromDIP(2)), wnd->FromDIP(3));
    }
    const int arm = wnd->FromDIP(5);
    const wxPoint c(r.x + r.width / 2, r.y + r.height / 2);
    dc.SetPen(wxPen(state & wxAUI_BUTTON_STATE_HOVER ? th.fg : th.dim, std::max(1, wnd->FromDIP(1))));
    dc.DrawLine(c.x - arm, c.y, c.x + arm + 1, c.y);
    dc.DrawLine(c.x, c.y - arm, c.x, c.y + arm + 1);
    if (out) *out = r;
  }

  // A preview tab (a single click in the tree or the history: the next one replaces it) has its
  // title in italics, as in VS Code and Postman, instead of a cryptic prefix; its tooltip says
  // what that means.
  int DrawPageTab(wxDC& dc, wxWindow* wnd, wxAuiNotebookPage& page, const wxRect& rect) override {
    Italic it(this, page);
    return wxAuiFlatTabArt::DrawPageTab(dc, wnd, page, rect);
  }
  wxSize GetPageTabSize(wxReadOnlyDC& dc, wxWindow* wnd, const wxAuiNotebookPage& page, int* x_extent) override {
    Italic it(this, page);
    return wxAuiFlatTabArt::GetPageTabSize(dc, wnd, page, x_extent);
  }
  // The window-list button: with many tabs a filterable list of at most ~18 rows instead of a
  // menu taller than the screen (150 tabs were a 150-row menu).
  int ShowDropDown(wxWindow* wnd, const wxAuiNotebookPageArray& items, int active) override {
    if (items.size() <= 16) return wxAuiFlatTabArt::ShowDropDown(wnd, items, active);
    return tab_list(wnd, items, active);
  }

 private:
  struct Italic {
    PlusTabArt* a;
    bool on;
    wxFont n, s, m;
    Italic(PlusTabArt* art, const wxAuiNotebookPage& page) : a(art) {
      const auto* t = dynamic_cast<const RequestTab*>(page.window);
      on = t && t->preview();
      if (!on) return;
      n = a->m_normalFont, s = a->m_selectedFont, m = a->m_measuringFont;
      a->m_normalFont = n.Italic(), a->m_selectedFont = s.Italic(), a->m_measuringFont = m.Italic();
    }
    ~Italic() {
      if (on) a->m_normalFont = n, a->m_selectedFont = s, a->m_measuringFont = m;
    }
  };
  static wxRect place(wxWindow* wnd, const wxRect& in, int orientation) {
    const int w = wnd->FromDIP(26), h = std::min(in.height, wnd->FromDIP(24));
    const int y = in.y + (in.height - h) / 2;
    return orientation == wxLEFT ? wxRect(in.x, y, w, h) : wxRect(in.x + in.width - w, y, w, h);
  }
};

// The window list for many tabs: a type-to-filter box over a list of the tabs' titles. Returns
// the chosen index into items, or -1.
class TabListBox : public wxVListBox {
 public:
  explicit TabListBox(wxWindow* parent) : wxVListBox(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE) {
    SetBackgroundColour(rv::Theme::get().bg);
  }
  std::vector<wxString> rows;
  void set_rows(std::vector<wxString> r) {
    rows = std::move(r);
    SetItemCount(rows.size());
    Refresh();
  }

 private:
  void OnDrawItem(wxDC& dc, const wxRect& rect, size_t n) const override {
    const auto& th = rv::Theme::get();
    dc.SetFont(GetFont());
    dc.SetTextForeground(IsSelected(n) ? th.fg : th.dim);
    dc.DrawText(rows[n], rect.x + FromDIP(8), rect.y + (rect.height - dc.GetCharHeight()) / 2);
  }
  void OnDrawBackground(wxDC& dc, const wxRect& rect, size_t n) const override {
    if (!IsSelected(n)) return;
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(rv::Theme::get().grid));
    dc.DrawRectangle(rect);
  }
  wxCoord OnMeasureItem(size_t) const override { return GetCharHeight() + FromDIP(8); }
};

int tab_list(wxWindow* wnd, const wxAuiNotebookPageArray& items, int active) {
  const auto& th = rv::Theme::get();
  wxDialog dlg(wnd, wxID_ANY, "Tabs", wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
  dlg.SetBackgroundColour(th.bg);
  auto* sz = new wxBoxSizer(wxVERTICAL);
  auto* filter = new wxTextCtrl(&dlg, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
  filter->SetHint(wxString::Format("Filter %zu tabs", items.size()));
  filter->SetBackgroundColour(th.bg);
  filter->SetForegroundColour(th.fg);
  auto* list = new TabListBox(&dlg);
  sz->Add(filter, 0, wxEXPAND | wxALL, dlg.FromDIP(6));
  sz->Add(list, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, dlg.FromDIP(6));
  std::vector<int> shown;  // list row -> index into items
  auto fill = [&] {
    const wxString f = filter->GetValue().Lower();
    std::vector<wxString> rows;
    shown.clear();
    int sel = 0;
    for (size_t i = 0; i < items.size(); ++i) {
      wxString c = items[i].caption;
      if (!f.empty() && c.Lower().Find(f) == wxNOT_FOUND) continue;
      if (static_cast<int>(i) == active) sel = static_cast<int>(shown.size());
      shown.push_back(static_cast<int>(i));
      rows.push_back(c);
    }
    list->set_rows(std::move(rows));
    if (!shown.empty()) {
      list->SetSelection(f.empty() ? sel : 0);
      list->ScrollToRow(static_cast<size_t>(list->GetSelection()));
    }
  };
  fill();
  int chosen = -1;
  auto pick = [&] {
    const int r = list->GetSelection();
    if (r >= 0 && r < static_cast<int>(shown.size())) chosen = shown[static_cast<size_t>(r)];
    dlg.EndModal(chosen >= 0 ? wxID_OK : wxID_CANCEL);
  };
  filter->Bind(wxEVT_TEXT, [&](wxCommandEvent&) { fill(); });
  filter->Bind(wxEVT_TEXT_ENTER, [&](wxCommandEvent&) { pick(); });
  list->Bind(wxEVT_LISTBOX_DCLICK, [&](wxCommandEvent&) { pick(); });
  list->Bind(wxEVT_LEFT_UP, [&](wxMouseEvent& e) {  // one click picks, as in a menu
    e.Skip();
    list->CallAfter([&] { pick(); });
  });
  dlg.Bind(wxEVT_CHAR_HOOK, [&](wxKeyEvent& e) {
    const int k = e.GetKeyCode();
    if (k == WXK_ESCAPE) {
      dlg.EndModal(wxID_CANCEL);
      return;
    }
    if ((k == WXK_DOWN || k == WXK_UP || k == WXK_PAGEDOWN || k == WXK_PAGEUP) && list->GetItemCount() > 0) {
      const int n = static_cast<int>(list->GetItemCount());
      const int step = k == WXK_DOWN ? 1 : k == WXK_UP ? -1 : k == WXK_PAGEDOWN ? 10 : -10;
      list->SetSelection(std::clamp(list->GetSelection() + step, 0, n - 1));
      list->ScrollToRow(static_cast<size_t>(list->GetSelection()));
      return;
    }
    if (k == WXK_RETURN || k == WXK_NUMPAD_ENTER) {
      pick();
      return;
    }
    e.Skip();
  });
  // Clicked outside: closed, as a menu would be.
  dlg.Bind(wxEVT_ACTIVATE, [&](wxActivateEvent& e) {
    e.Skip();
    if (!e.GetActive() && dlg.IsModal()) dlg.CallAfter([&dlg] {
        if (dlg.IsModal()) dlg.EndModal(wxID_CANCEL);
      });
  });
  dlg.SetSizer(sz);
  const int row = list->GetCharHeight() + dlg.FromDIP(8);
  dlg.SetSize(dlg.FromDIP(360), filter->GetBestSize().y + row * 18 + dlg.FromDIP(18));
  // Under the button (the right end of the tab strip), kept on the display.
  const wxRect strip = wnd->GetScreenRect();
  wxPoint at(strip.GetRight() - dlg.GetSize().x, strip.GetBottom());
  if (const int d = wxDisplay::GetFromWindow(wnd); d != wxNOT_FOUND) {
    const wxRect area = wxDisplay(static_cast<unsigned>(d)).GetClientArea();
    at.x = std::clamp(at.x, area.x, std::max(area.x, area.GetRight() - dlg.GetSize().x));
    at.y = std::clamp(at.y, area.y, std::max(area.y, area.GetBottom() - dlg.GetSize().y));
  }
  dlg.Move(at);
  filter->SetFocus();
  dlg.ShowModal();
  return chosen;
}

// Scrolls the tab strip to the selected page: wxAuiNotebook does that only when a page is
// selected through it, so a restored active tab (the 20th) stayed out of view.
void reveal_selected_tab(wxAuiNotebook* nb) {
  const int sel = nb->GetSelection();
  if (sel < 0) return;
  const wxAuiNotebookPosition pos = nb->GetPagePosition(static_cast<size_t>(sel));
  if (pos.tabCtrl && pos.tabIdx >= 0 && pos.tabCtrl->GetClientSize().x > 0) pos.tabCtrl->DoShowTab(pos.tabIdx);
}

// Runs fn on the UI thread after ms milliseconds (one-shot timer that deletes itself).
void after_ms(int ms, std::function<void()> fn) {
  auto* t = new wxTimer();
  t->Bind(wxEVT_TIMER, [t, fn = std::move(fn)](wxTimerEvent&) {
    fn();
    wxTheApp->CallAfter([t] { delete t; });
  });
  t->StartOnce(std::max(ms, 1));
}

// Types s the way the keyboard does: each character goes to whichever window has the focus at
// that moment (one WM_CHAR every 15 ms), so a focus change in between loses the rest.
void type_text(const std::wstring& s, std::function<void()> done, size_t i = 0) {
  if (i == s.size()) {
    after_ms(150, std::move(done));
    return;
  }
  if (HWND f = ::GetFocus()) ::PostMessageW(f, WM_CHAR, s[i], 1);
  after_ms(15, [s, done = std::move(done), i]() mutable { type_text(s, std::move(done), i + 1); });
}

// "Save changes?" for closing a tab with unsaved changes, in the app's theme (wxMessageBox is
// the system's light task dialog). wxID_YES (save), wxID_NO (discard) or wxID_CANCEL.
// count > 1: one question for a batch of tabs (what: their names), Save saves each.
int ask_save(wxWindow* parent, const wxString& what, size_t count = 1) {
  const auto& th = rv::Theme::get();
  wxDialog dlg(parent, wxID_ANY, "watt", wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX);
  dlg.SetBackgroundColour(th.bg);
  auto* sz = new wxBoxSizer(wxVERTICAL);
  auto* head = new wxStaticText(&dlg, wxID_ANY,
                                count > 1 ? wxString::Format(L"Save changes to %zu requests?", count)
                                          : wxString::Format(L"Save changes to “%s”?", what));
  head->SetForegroundColour(th.fg);
  head->SetFont(head->GetFont().Bold().Scaled(1.1f));
  auto* body = new wxStaticText(&dlg, wxID_ANY,
                                count > 1 ? what + L"\n\nTheir changes are lost if you close the tabs without saving."
                                          : wxString("Your changes are lost if you close the tab without saving."));
  body->SetForegroundColour(th.dim);
  sz->Add(head, 0, wxLEFT | wxRIGHT | wxTOP, dlg.FromDIP(18));
  sz->AddSpacer(dlg.FromDIP(6));
  sz->Add(body, 0, wxLEFT | wxRIGHT, dlg.FromDIP(18));
  auto* row = new wxBoxSizer(wxHORIZONTAL);
  // Alt+S / Alt+N (and S / N alone, as Windows' own save prompts) answer without the mouse.
  auto* save = new wxButton(&dlg, wxID_YES, "&Save");
  auto* discard = new wxButton(&dlg, wxID_NO, "Do&n't save");
  auto* cancel = new wxButton(&dlg, wxID_CANCEL, "Cancel");
  row->AddStretchSpacer();
  row->Add(save, 0, wxRIGHT, dlg.FromDIP(8));
  row->Add(discard, 0, wxRIGHT, dlg.FromDIP(8));
  row->Add(cancel);
  sz->Add(row, 0, wxEXPAND | wxALL, dlg.FromDIP(18));
  for (wxButton* b : {save, discard, cancel}) b->Bind(wxEVT_BUTTON, [&dlg](wxCommandEvent& e) { dlg.EndModal(e.GetId()); });
  save->SetDefault();
  save->SetFocus();
  dlg.SetEscapeId(wxID_CANCEL);
  dlg.Bind(wxEVT_CHAR_HOOK, [&dlg](wxKeyEvent& e) {
    const int k = e.GetKeyCode();
    if ((e.GetModifiers() == wxMOD_NONE || e.GetModifiers() == wxMOD_ALT) && (k == 'N' || k == 'S')) {
      dlg.EndModal(k == 'N' ? wxID_NO : wxID_YES);
      return;
    }
    e.Skip();
  });
  dlg.SetSizerAndFit(sz);
  dlg.SetMinSize(wxSize(dlg.FromDIP(380), -1));
  dlg.Fit();
  dlg.CentreOnParent();
  return dlg.ShowModal();
}

void gather_folders(const CollectionNode& n, const std::string& prefix, std::vector<std::pair<std::string, std::filesystem::path>>& out) {
  for (const auto& c : n.children) {
    if (!c.is_folder) continue;
    const std::string label = prefix.empty() ? c.name : prefix + " / " + c.name;
    out.emplace_back(label, c.path);
    gather_folders(c, label, out);
  }
}

// First save of an unsaved request (Postman's Save dialog): its name and the folder it goes to.
bool ask_save_target(wxWindow* parent, const Workspace& ws, const std::string& suggested,
                     const std::filesystem::path& folder, std::string* name, std::filesystem::path* chosen) {
  const auto& th = rv::Theme::get();
  wxDialog dlg(parent, wxID_ANY, "Save request", wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX);
  dlg.SetBackgroundColour(th.bg);
  auto* sz = new wxBoxSizer(wxVERTICAL);
  auto label = [&](const char* s) {
    auto* t = new wxStaticText(&dlg, wxID_ANY, s);
    t->SetForegroundColour(th.dim);
    sz->Add(t, 0, wxLEFT | wxRIGHT | wxTOP, dlg.FromDIP(18));
    sz->AddSpacer(dlg.FromDIP(4));
  };
  label("Name");
  // The editor's themed controls (a flat field, its dropdown and buttons): native ones draw light
  // outlines inside the dark theme.
  const auto& et = http_editor::Theme::get();
  auto* edit = new wxTextCtrl(&dlg, wxID_ANY, wxString::FromUTF8(suggested), wxDefaultPosition,
                              wxSize(dlg.FromDIP(360), -1), wxTE_PROCESS_ENTER | wxBORDER_SIMPLE);
  edit->SetBackgroundColour(et.field);
  edit->SetForegroundColour(et.fg);
  sz->Add(edit, 0, wxEXPAND | wxLEFT | wxRIGHT, dlg.FromDIP(18));
  label("Folder");
  std::vector<std::pair<std::string, std::filesystem::path>> folders{{"Collection root", ws.root()}};
  gather_folders(ws.tree(0), "", folders);
  std::vector<wxString> names;
  int sel = 0;
  for (size_t i = 0; i < folders.size(); ++i) {
    names.push_back(wxString::FromUTF8(folders[i].first));
    std::error_code ec;
    if (!folder.empty() && std::filesystem::equivalent(folders[i].second, folder, ec)) sel = static_cast<int>(i);
  }
  auto* pick = new http_editor::ChoiceButton(&dlg, names);
  pick->set_field_look(true);
  pick->set_selection(sel);
  sz->Add(pick, 0, wxEXPAND | wxLEFT | wxRIGHT, dlg.FromDIP(18));
  auto* row = new wxBoxSizer(wxHORIZONTAL);
  auto* save = new http_editor::FlatButton(&dlg, "Save", http_editor::FlatButton::Style::Primary);
  auto* cancel = new http_editor::FlatButton(&dlg, "Cancel", http_editor::FlatButton::Style::Subtle);
  row->AddStretchSpacer();
  row->Add(cancel, 0, wxRIGHT, dlg.FromDIP(8));
  row->Add(save);
  sz->Add(row, 0, wxEXPAND | wxALL, dlg.FromDIP(18));
  auto valid = [edit] { return !edit->GetValue().Strip(wxString::both).empty(); };
  edit->Bind(wxEVT_TEXT, [save, valid](wxCommandEvent& e) {
    e.Skip();
    save->Enable(valid());
  });
  save->Enable(valid());
  save->on_click = [&dlg, valid] {
    if (valid()) dlg.EndModal(wxID_OK);
  };
  cancel->on_click = [&dlg] { dlg.EndModal(wxID_CANCEL); };
  edit->Bind(wxEVT_TEXT_ENTER, [&dlg, valid](wxCommandEvent&) {
    if (valid()) dlg.EndModal(wxID_OK);
  });
  dlg.SetEscapeId(wxID_CANCEL);
  // Esc cancels (Cancel is a FlatButton, not a wxID_CANCEL button the default handling finds);
  // the folder list takes its own Esc first.
  dlg.Bind(wxEVT_CHAR_HOOK, [&dlg](wxKeyEvent& e) {
    if (e.GetKeyCode() == WXK_ESCAPE && !e.HasAnyModifiers()) return dlg.EndModal(wxID_CANCEL);
    e.Skip();
  });
  dlg.SetSizerAndFit(sz);
  dlg.CentreOnParent();
  edit->SetFocus();
  edit->SelectAll();
  if (dlg.ShowModal() != wxID_OK) return false;
  *name = edit->GetValue().Strip(wxString::both).utf8_string();
  const int s = pick->selection();
  *chosen = folders[static_cast<size_t>(s < 0 ? 0 : s)].second;
  return true;
}

std::string focus_desc() {
  wxWindow* f = wxWindow::FindFocus();
  if (!f) return "none";
  const wxRect r = f->GetScreenRect();
  return wxString::Format("%s %dx%d", f->GetClassInfo()->GetClassName(), r.width, r.height).utf8_string();
}

// wxWidgets 3.3.1: wxAuiTabCtrl::m_hoverButton points into the hovered tab's own close-button
// storage and is not reset when that page is removed. After a tab is closed with its X, the next
// mouse move clears a bit in the freed block (OnMotion / OnLeaveWindow), which corrupted whatever
// reused it: the notebook's layout sizers, AV in ~wxSizer from RemoveEmptyTabFrames on a later
// close. A leave event resets the pointer (and the hover state) while it is still valid: call this
// before every page removal.
void forget_tab_hover(wxAuiNotebook* nb) {
  if (!nb) return;
  for (wxAuiTabCtrl* tc : nb->GetAllTabCtrls()) {
    wxMouseEvent leave(wxEVT_LEAVE_WINDOW);
    leave.SetEventObject(tc);
    tc->GetEventHandler()->ProcessEvent(leave);
  }
}
}  // namespace

// ------------------------------------------------------------------ frame

std::shared_ptr<GrpcClient> MainFrame::grpc() {
  static std::once_flag once;
  static std::shared_ptr<GrpcClient> client;
  std::call_once(once, [] { client = std::make_shared<GrpcClient>(); });
  return client;
}

MainFrame::MainFrame(std::filesystem::path workspace_dir)
    : wxFrame(nullptr, wxID_ANY, "watt", wxDefaultPosition, wxDefaultSize), ws_dir_(std::move(workspace_dir)) {
  g_frame = this;
  SetIcons(wxIconBundle("AAA_WATT", nullptr));  // watt.rc: every size of the application icon
  open_workspace_async();  // file scan on a worker while the window is being built
  // Warm libcurl / Winsock / TLS on another core while the window is being built (the first
  // request is 12-21% slower without it); done long before the first paint.
  std::thread([] {
    HttpClient::global_init();
    startup::mark("http_warm");
  }).detach();
  restore_geometry();
  aui_.SetManagedWindow(this);

  sidebar_ = make_sidebar(this, this);
  // Ctrl+P before the sidebar can serve it: while the workspace is still opening (the sidebar
  // ignores the key then) and in the first seconds after, while the sidebar is still scanning
  // the collection (its dialog would list nothing). This frame's quick open waits for the
  // workspace and lists ws->tree() directly. Bound after the sidebar's hook, so it runs first.
  Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
    if (e.GetModifiers() == wxMOD_CONTROL && (e.GetKeyCode() == 'P' || e.GetKeyCode() == 'p') &&
        (!ws_ || ms_between(ws_opened_at_, Clock::now()) < 2000)) {
      quick_open();
      return;
    }
    e.Skip();
  });
  nb_ = new wxAuiNotebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                          wxAUI_NB_TOP | wxAUI_NB_TAB_MOVE | wxAUI_NB_SCROLL_BUTTONS | wxAUI_NB_CLOSE_ON_ALL_TABS |
                              wxAUI_NB_MIDDLE_CLICK_CLOSE | wxAUI_NB_WINDOWLIST_BUTTON | wxBORDER_NONE);
  // Prebuilt spare tabs live here, not as hidden non-page children of the notebook:
  // wxAuiNotebook walks its children when it removes empty tab frames, and stray RequestTab
  // children coincided with an AV in RemoveEmptyTabFrames during DeletePage. AddPage
  // reparents a spare into the notebook when it is used.
  spare_park_ = new wxWindow(this, wxID_ANY, wxDefaultPosition, wxSize(1, 1));
  spare_park_->Hide();
  aui_.AddPane(sidebar_->window(), wxAuiPaneInfo()
                                       .Name("sidebar")
                                       .Left()
                                       .CaptionVisible(false)
                                       .CloseButton(false)
                                       .PaneBorder(false)
                                       .BestSize(wxSize(sidebar_width(), -1))
                                       .MinSize(FromDIP(wxSize(160, -1))));
  nb_->SetArtProvider(new PlusTabArt());
  if (wxAuiTabCtrl* tc = nb_->GetMainTabCtrl()) tc->AddButton(kPlusButton, wxRIGHT);
  nb_->Bind(wxEVT_AUINOTEBOOK_BUTTON, [this](wxAuiNotebookEvent& e) {
    if (e.GetInt() == kPlusButton) {
      new_request(RequestKind::Http, {});
      return;
    }
    e.Skip();
  });
  aui_.AddPane(nb_, wxAuiPaneInfo().Name("tabs").CenterPane().PaneBorder(false));
  aui_.Update();
  // The start tab is built complete, response view included, before the window shows (~70 ms
  // before Show() instead of a block right after the first paint): the first frame is the
  // finished one, the URL field keeps its focus, and the first send is painted the moment its
  // response arrives. Later tabs build their response view shortly after they show.
  // A relaunch: the last session's tabs, if the opener thread has read them by now (it usually
  // has: the workspace opens while the sidebar is built). Restored here, the first frame shows
  // them instead of an empty tab that turns into them a moment after the first paint.
  if (auto s = take_session({})) apply_session(*s);
  if (nb_->GetPageCount() == 0) add_tab(SavedRequest{}, false)->build_response_view();
  // Start typing a URL right away: on first activation (focus set before that is lost, and wx
  // would otherwise give it to the first child, the sidebar).
  Bind(wxEVT_ACTIVATE, [this, first = true](wxActivateEvent& e) mutable {
    e.Skip();
    if (!e.GetActive() || !first) return;
    first = false;
    CallAfter([this] {
      if (auto* t = current_tab()) t->focus_url();
    });
  });

  wxAcceleratorEntry acc[] = {
      {wxACCEL_CTRL, WXK_RETURN, ID_SEND},
      {wxACCEL_CTRL, WXK_NUMPAD_ENTER, ID_SEND},
      {wxACCEL_CTRL, 'T', ID_NEW_TAB},
      {wxACCEL_CTRL, 'N', ID_NEW_TAB},  // as in Postman, Insomnia and Bruno
      {wxACCEL_CTRL | wxACCEL_SHIFT, 'T', ID_NEW_GRPC_TAB},
      {wxACCEL_CTRL | wxACCEL_ALT, 'T', ID_NEW_WS_TAB},
      {wxACCEL_CTRL, 'W', ID_CLOSE_TAB},
      {wxACCEL_CTRL, WXK_F4, ID_CLOSE_TAB},
      {wxACCEL_CTRL, 'L', ID_FOCUS_URL},
      {wxACCEL_CTRL, 'S', ID_SAVE},
      {wxACCEL_CTRL, 'P', ID_QUICK_OPEN},
      {wxACCEL_CTRL, WXK_TAB, ID_NEXT_TAB},
      {wxACCEL_CTRL | wxACCEL_SHIFT, WXK_TAB, ID_PREV_TAB},
      {wxACCEL_CTRL | wxACCEL_SHIFT, 'F', ID_FOCUS_FILTER},
  };
  SetAcceleratorTable(wxAcceleratorTable(static_cast<int>(std::size(acc)), acc));

  Bind(wxEVT_MENU, [this](wxCommandEvent&) {
    if (auto* t = current_tab()) t->send();
    else if (const int s = nb_->GetSelection(); s >= 0) runner_shortcut(nb_->GetPage(static_cast<size_t>(s)), false);
  }, ID_SEND);
  Bind(wxEVT_MENU, [this](wxCommandEvent&) { new_request(RequestKind::Http, {}); }, ID_NEW_TAB);
  Bind(wxEVT_MENU, [this](wxCommandEvent&) { new_request(RequestKind::Grpc, {}); }, ID_NEW_GRPC_TAB);
  Bind(wxEVT_MENU, [this](wxCommandEvent&) { new_request(RequestKind::WebSocket, {}); }, ID_NEW_WS_TAB);
  Bind(wxEVT_MENU, [this](wxCommandEvent&) { if (nb_->GetSelection() >= 0) close_tab(nb_->GetSelection()); }, ID_CLOSE_TAB);
  Bind(wxEVT_MENU, [this](wxCommandEvent&) { if (auto* t = current_tab()) t->focus_url(); }, ID_FOCUS_URL);
  Bind(wxEVT_MENU, [this](wxCommandEvent&) { save_current(); }, ID_SAVE);
  Bind(wxEVT_MENU, [this](wxCommandEvent&) { quick_open(); }, ID_QUICK_OPEN);
  Bind(wxEVT_MENU, [this](wxCommandEvent&) { nb_->AdvanceSelection(true); }, ID_NEXT_TAB);
  Bind(wxEVT_MENU, [this](wxCommandEvent&) { nb_->AdvanceSelection(false); }, ID_PREV_TAB);
  Bind(wxEVT_MENU, [this](wxCommandEvent&) { sidebar_->focus_filter(); }, ID_FOCUS_FILTER);

  // The tab's X: decide here, synchronously, and let wxAuiNotebook close the page itself.
  // Vetoing and deleting the page later from CallAfter crashed (AV in RemoveEmptyTabFrames ->
  // ~wxSizer) when the deferred DeletePage ran while wxAuiTabCtrl was still handling the mouse
  // over the closed tab's button; the captured index could also name another tab by then.
  nb_->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE, [this](wxAuiNotebookEvent& e) {
    forget_tab_hover(nb_);  // before the page (and its close button) goes
    const int idx = e.GetSelection();
    if (idx < 0 || idx >= static_cast<int>(nb_->GetPageCount())) return;
    auto* t = dynamic_cast<RequestTab*>(nb_->GetPage(idx));
    if (t && !t->dirty())  // the notebook deletes it: big documents are freed on workers first
      while (t->teardown_step()) {
      }
    if (!t || !t->dirty()) return;  // not vetoed: the notebook closes it (a runner tab too)
    nb_->SetSelection(idx);
    wxString what = t->title();
    what.Replace(L" ●", "");  // the unsaved-changes dot
    const int a = ask_save(this, what);
    if (a == wxID_CANCEL) {
      e.Veto();
      return;
    }
    if (a == wxID_YES) {
      save_current();
      if (t->dirty()) e.Veto();  // the save failed or was cancelled: keep the tab
    }
    if (e.IsAllowed())
      while (t->teardown_step()) {
      }
  });
  nb_->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSED, [this](wxAuiNotebookEvent& e) {
    e.Skip();
    // Adding a page is deferred too, until the notebook has finished closing this one.
    CallAfter([this] {
      if (nb_->GetPageCount() == 0) add_tab(SavedRequest{}, false);
      session_changed();
    });
  });
  nb_->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGED, [this](wxAuiNotebookEvent& e) {
    e.Skip();
    if (closing_) return;  // tabs torn down after the window closed (close_slice): nothing to show
    if (auto* t = current_tab()) {
      t->realize();  // a restored tab builds its editor when first shown
      if (!restoring_) t->wake_grpc();  // the user switched to a restored gRPC tab: load its schema
      sidebar_->select_file(t->file());
    }
    session_changed();
  });
  nb_->Bind(wxEVT_AUINOTEBOOK_DRAG_DONE, [this](wxAuiNotebookEvent& e) {
    e.Skip();
    session_changed();  // tabs reordered
  });
  nb_->Bind(wxEVT_AUINOTEBOOK_TAB_MIDDLE_UP, [this](wxAuiNotebookEvent& e) { close_tab(e.GetSelection()); });
  // Right click on a tab: Close / Close others / to the right / all, Duplicate, Reveal in sidebar.
  // After the notebook's own handling of the click (CallAfter), so the menu is not shown from
  // inside wxAuiTabCtrl's mouse handler.
  nb_->Bind(wxEVT_AUINOTEBOOK_TAB_RIGHT_UP, [this](wxAuiNotebookEvent& e) {
    const int sel = e.GetSelection();
    wxWindow* pg = sel >= 0 && sel < static_cast<int>(nb_->GetPageCount()) ? nb_->GetPage(static_cast<size_t>(sel)) : nullptr;
    if (!pg) return;
    CallAfter([this, pg] {
      if (const int i = nb_->GetPageIndex(pg); i != wxNOT_FOUND) tab_menu(i);
    });
  });
  // Double click on the empty tab strip: new tab (like browsers).
  nb_->Bind(wxEVT_AUINOTEBOOK_BG_DCLICK, [this](wxAuiNotebookEvent&) { new_request(RequestKind::Http, {}); });
  Bind(wxEVT_CLOSE_WINDOW, &MainFrame::on_close, this);
  // The un-maximized rectangle, for the next session (GetRect() of a maximized window is the
  // screen).
  auto track = [this](wxEvent& e) {
    e.Skip();
    if (IsShown() && !IsMaximized() && !IsIconized()) normal_rect_ = GetRect();
  };
  Bind(wxEVT_SIZE, track);
  Bind(wxEVT_MOVE, track);
  // Everything the first paint does not need runs after it: the first idle event comes once the
  // message queue (WM_PAINT included) is empty after Show().
  Bind(wxEVT_IDLE, [this, first = true](wxIdleEvent& e) mutable {
    e.Skip();
    if (!first) return;
    first = false;
    CallAfter([this] { first_idle(); });
  });
}

// The sidebar's saved width (the sidebar writes Sidebar/Width in DIPs when the user drags it).
// The pane starts at that width, so the sidebar's own restore (two AUI relayouts, ~30 ms after
// the first paint) has nothing to do.
int MainFrame::sidebar_width() const {
  long w = 260;
  if (wxConfigBase* c = wxConfigBase::Get()) c->Read("Sidebar/Width", &w);
  if (w < 160 || w > 2000) w = 260;
  return FromDIP(static_cast<int>(w));
}

// Window size, position and maximized state of the last session (Window/* in wxConfig), if
// that rectangle is still on a connected display; else 1400x860 (DIPs) at the default place.
void MainFrame::restore_geometry() {
  long x = 0, y = 0, w = 0, h = 0;
  bool max = false;
  wxConfigBase* c = wxConfigBase::Get();
  if (c && c->Read("Window/W", &w) && c->Read("Window/H", &h) && c->Read("Window/X", &x) && c->Read("Window/Y", &y) &&
      w >= 400 && h >= 300) {
    c->Read("Window/Maximized", &max, false);
    const wxRect r(static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), static_cast<int>(h));
    const int d = wxDisplay::GetFromPoint(wxPoint(r.x + std::min(r.width / 2, 200), r.y + 16));
    if (d != wxNOT_FOUND && wxDisplay(static_cast<unsigned>(d)).GetClientArea().Intersects(r)) {
      SetSize(r);
      normal_rect_ = r;
      if (max) Maximize(true);
      return;
    }
  }
  SetClientSize(FromDIP(wxSize(1400, 860)));
}

void MainFrame::save_geometry() {
  wxConfigBase* c = wxConfigBase::Get();
  if (!c) return;
  if (!IsMaximized() && !IsIconized()) normal_rect_ = GetRect();
  if (normal_rect_.IsEmpty()) return;
  c->Write("Window/X", static_cast<long>(normal_rect_.x));
  c->Write("Window/Y", static_cast<long>(normal_rect_.y));
  c->Write("Window/W", static_cast<long>(normal_rect_.width));
  c->Write("Window/H", static_cast<long>(normal_rect_.height));
  c->Write("Window/Maximized", IsMaximized());
  c->Flush();
}

void MainFrame::first_idle() {
  startup::mark("first_paint");
  startup::flush();
  painted_ = true;
  if (ws_pending_) {
    ws_pending_ = false;
    workspace_opened(std::move(pending_ws_), pending_err_);
  }
  reveal_selected_tab(nb_);  // a restored active tab far right in the strip
  // Give the first tab its curl handle (on its worker) so its first send does not create one.
  if (auto* t = current_tab()) t->warm_up();
  // A spare gRPC tab too (built last, in a quiet moment, its engine parked until used): the
  // first Ctrl+Shift+T or gRPC request / history entry opened in a session used to build the
  // gRPC editor on the spot (52-86 ms on the UI thread).
  want_grpc_spare_ = true;
  // Well after start-up has settled (the window is usable long before), in a quiet moment.
  prebuild_later(kQuietMs);
  if (!startup::self_test_file().empty()) self_test();
  const std::string& url = startup::first_request_url();
  if (!url.empty())
    if (auto* t = current_tab()) {
      SavedRequest r;
      if (url.rfind("grpc://", 0) == 0) {  // grpc://host:port/pkg.Service/Method, body {}
        const std::string rest = url.substr(7);
        const size_t slash = rest.find('/');
        r.kind = RequestKind::Grpc;
        r.grpc_target.address = rest.substr(0, slash);
        r.grpc_method = slash == std::string::npos ? std::string() : rest.substr(slash);
        r.grpc_body = "{}";
      } else {
        r.http.url = url;
      }
      t->set_request(std::move(r), false);
      startup::mark("first_request_sent");
      t->send();
    }
}

void MainFrame::self_test() {
  // 1. 500 ms after launch, type into the start tab, then send it while the workspace is still
  // opening (the self-test holds it back until 1.2 s): the send must wait for it and get its
  // history entry. 2. Ctrl+T, wait 150 ms, type. 3. Ctrl+T, wait 500 ms, type. Each time the
  // tab's URL must be exactly what was typed.
  struct Result {
    std::string json;
    int failed = 0;
  };
  auto res = std::make_shared<Result>();
  auto check = [this, res](const char* name, const std::wstring& want) {
    RequestTab* t = current_tab();
    const std::string got = t ? t->request().http.url : std::string("<no tab>");
    const bool ok = got == wxString(want).utf8_string();
    res->failed += !ok;
    std::string g;
    for (char c : got) g += c == '"' || c == '\\' ? '_' : c;
    res->json += wxString::Format("%s\"%s\":{\"ok\":%s,\"url\":\"%s\",\"focus\":\"%s\"}",
                                  res->json.empty() ? "" : ",", name, ok ? "true" : "false", g,
                                  focus_desc()).utf8_string();
  };
  auto finish = [this, res] {
    const std::string line = "{\"self_test\":{" + res->json + ",\"pass\":" + (res->failed ? "false" : "true") + "}}\n";
    if (FILE* f = _wfopen(startup::self_test_file().c_str(), L"ab")) {
      std::fwrite(line.data(), 1, line.size(), f);
      std::fclose(f);
    }
    Destroy();  // no close prompt for the edited tabs
  };
  auto new_tab_then = [this](int wait, std::function<void()> fn) {
    new_request(RequestKind::Http, {});  // exactly what Ctrl+T does
    after_ms(wait, std::move(fn));
  };
  const int start = std::max(1, 500 - static_cast<int>(startup::since_launch_ms()));
  after_ms(start, [=] {
    type_text(L"http://127.0.0.1:1/launch", [=] {
      check("launch_500ms", L"http://127.0.0.1:1/launch");
      const bool early = !ws_;
      if (RequestTab* t = current_tab()) t->send();  // what Ctrl+Enter does
      new_tab_then(150, [=] {
        type_text(L"http://127.0.0.1:1/t150", [=] {
          check("ctrl_t_150ms", L"http://127.0.0.1:1/t150");
          new_tab_then(500, [=] {
            type_text(L"http://127.0.0.1:1/t500", [=] {
              check("ctrl_t_500ms", L"http://127.0.0.1:1/t500");
              bool found = false;
              if (ws_)
                for (const auto& h : ws_->history(20)) found = found || history_template(h) == "GET http://127.0.0.1:1/launch";
              res->failed += !found || !early;
              res->json += wxString::Format(",\"send_before_workspace\":{\"ok\":%s,\"workspace_was_open\":%s}",
                                            found && early ? "true" : "false", early ? "false" : "true").utf8_string();
              finish();
            });
          });
        });
      });
    });
  });
}

MainFrame::~MainFrame() {
  g_frame = nullptr;
  aui_.UnInit();
}

void MainFrame::open_workspace_async() {
  std::thread([dir = ws_dir_] {
    // --self-test: hold the workspace back so a send can happen before it is there.
    if (!startup::self_test_file().empty()) Sleep(static_cast<DWORD>(std::max(0.0, 1200 - startup::since_launch_ms())));
    std::string err;
    std::unique_ptr<Workspace> ws;
    std::error_code ec;
    if (is_workspace_dir(dir)) {
      if (auto w = Workspace::open(dir, &err)) ws = std::make_unique<Workspace>(std::move(*w));
    } else {
      std::filesystem::create_directories(dir, ec);
      ws = std::make_unique<Workspace>(Workspace::create(dir));
    }
    if (ws) prefetch_session(*ws);
    auto* holder = new std::unique_ptr<Workspace>(std::move(ws));
    wxTheApp->CallAfter([holder, err] {
      std::unique_ptr<std::unique_ptr<Workspace>> h(holder);
      if (MainFrame* f = g_frame.load()) f->workspace_opened(std::move(*h), err);
    });
  }).detach();
}

void MainFrame::workspace_opened(std::unique_ptr<Workspace> ws, const std::string& err) {
  if (!painted_) {
    ws_pending_ = true;
    pending_ws_ = std::move(ws);
    pending_err_ = err;
    return;
  }
  startup::mark("workspace_opened");
  if (!ws) {
    SetTitle(wxString::FromUTF8("watt - workspace not opened: " + err));
    ws_failed_ = true;
  } else {
    ws_ = std::move(ws);
    ws_opened_at_ = Clock::now();
    SetTitle(wxString::Format("watt - %s", ws_->root().wstring()));  // shortened by SetTitle
    sidebar_->set_workspace(ws_.get());
    startup::mark("sidebar_workspace_set");
    restore_session();
    restore_special_later();  // runner / mock tabs a session restored before the workspace opened
    vars_changed();  // tabs restored before the workspace opened showed {{vars}} unresolved
    if (auto* t = current_tab()) sidebar_->select_file(t->file());
    startup::mark("session_restored");
    startup::flush();
  }
  // Whatever waited for the workspace (a send or Ctrl+P during start-up) runs now.
  auto waiting = std::move(ws_waiters_);
  ws_waiters_.clear();
  for (auto& fn : waiting) fn();
}

bool MainFrame::when_workspace(std::function<void()> fn) {
  if (ws_ || ws_failed_) return false;
  ws_waiters_.push_back(std::move(fn));
  return true;
}

// ------------------------------------------------------------------ session
//
// <ws>/.watt/session.json (personal, gitignored with the rest of .watt) lists the open tabs
// in order and the active one; a tab with unsaved changes (or never saved) also has its draft in
// .watt/session/tab<N>.req.json. Written shortly after every change (session_changed) and on
// close, read when the workspace opens: the active
// tab is restored into the start tab, the others are created lazily (no editor until shown).

namespace {
std::filesystem::path session_file(const Workspace& ws) { return ws.root() / ".watt" / "session.json"; }
std::filesystem::path session_dir(const Workspace& ws) { return ws.root() / ".watt" / "session"; }

// A tab's file as session.json stores it: relative to the root (the workspace can move or be
// cloned elsewhere), generic separators.
std::string session_path(const Workspace& ws, const std::filesystem::path& f) {
  if (f.empty()) return {};
  const std::filesystem::path rel = f.lexically_normal().lexically_relative(ws.root().lexically_normal());
  if (rel.empty() || *rel.begin() == "..") return detail::to_utf8(f);  // not below the root: load_request refuses it
  return detail::to_utf8(rel.generic_wstring());
}

// And back: a relative path without ".." below the root, or (older sessions) an absolute path
// below it. Anything else is ignored.
std::filesystem::path session_file_path(const Workspace& ws, const std::string& s) {
  namespace fs = std::filesystem;
  if (s.empty()) return {};
  const fs::path p = detail::from_utf8(s);
  const fs::path root = ws.root().lexically_normal();
  if (p.is_absolute()) {
    const fs::path rel = p.lexically_normal().lexically_relative(root);
    return rel.empty() || *rel.begin() == ".." ? fs::path() : p.lexically_normal();
  }
  if (p.has_root_name() || p.has_root_directory()) return {};
  for (const auto& part : p)
    if (part == "..") return {};
  return (root / p).lexically_normal();
}
}  // namespace

// What save_session writes, taken on the UI thread (cheap: only tabs with unsaved changes copy
// their request, responses are shared pointers) and written on any thread.
struct MainFrame::SessionSnapshot {
  struct Tab {
    std::filesystem::path file;
    std::optional<SavedRequest> draft;  // unsaved changes, or never saved
    bool dirty = false, preview = false, manual = false;
    LastResponse resp;
    std::shared_ptr<const GrpcSchema> schema;  // a gRPC tab's schema (RequestTab::session_schema)
    std::optional<std::filesystem::path> runner;  // a runner tab: its folder, relative to the root
    std::optional<std::filesystem::path> mock;    // a mock server tab: its folder, relative to the root
    unsigned mock_port = 0;
    bool mock_running = false;
  };
  uint64_t seq = 0;
  std::shared_ptr<Workspace> ws;  // a copy (the drafts are written through it)
  std::vector<Tab> tabs;
  int active = -1;
};

namespace {
std::mutex g_session_write_m;  // one writer at a time; a newer snapshot wins
uint64_t g_session_written = 0;
// Response and schema files already on disk, by path: unchanged ones are not written again.
std::map<std::filesystem::path, std::shared_ptr<const void>> g_resp_written;

// Writes dir/name from make() unless the same object (id) is already there. True if it is.
bool write_once(const std::filesystem::path& p, const std::shared_ptr<const void>& id,
                const std::function<std::string()>& make, std::map<std::filesystem::path, std::shared_ptr<const void>>& now) {
  std::error_code ec;
  bool have = false;
  if (auto it = g_resp_written.find(p); it != g_resp_written.end() && it->second == id && std::filesystem::exists(p, ec)) {
    have = true;
  } else if (const std::string json = make(); !json.empty()) {
    detail::ensure_dir(p.parent_path());
    have = detail::write_file_atomic(p, json, nullptr, true);
  }
  if (have) now[p] = id;
  return have;
}

bool write_session(const MainFrame::SessionSnapshot& s) {
  namespace fs = std::filesystem;
  std::lock_guard lk(g_session_write_m);
  if (s.seq < g_session_written) return true;  // a newer one is already on disk
  g_session_written = s.seq;
  Workspace& ws = *s.ws;
  const fs::path dir = session_dir(ws);
  std::set<fs::path> keep;
  std::map<fs::path, std::shared_ptr<const void>> resp_written;
  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> w(sb);
  w.StartObject();
  w.Key("version");
  w.Int(1);
  w.Key("tabs");
  w.StartArray();
  bool ok = true;
  int n = 0;
  for (const auto& t : s.tabs) {
    w.StartObject();
    if (t.runner) {
      w.Key("runner");
      w.String(detail::to_utf8(*t.runner).c_str());
      w.EndObject();
      ++n;
      continue;
    }
    if (t.mock) {
      w.Key("mock");
      w.String(detail::to_utf8(*t.mock).c_str());
      w.Key("port");
      w.Uint(t.mock_port);
      w.Key("running");
      w.Bool(t.mock_running);
      w.EndObject();
      ++n;
      continue;
    }
    w.Key("file");
    w.String(session_path(ws, t.file).c_str());
    if (t.draft) {
      SavedRequest d = *t.draft;
      const std::string draft = "tab" + std::to_string(n) + ".req.json";
      d.file = dir / draft;
      std::string err;
      if (ws.save_request(d, &err)) {  // unchanged content: the file is not touched
        keep.insert(dir / draft);
        w.Key("draft");
        w.String(draft.c_str());
        w.Key("dirty");
        w.Bool(t.dirty);
        if (t.draft->name.empty()) {
          w.Key("unnamed");
          w.Bool(true);
        }
      } else {
        ok = false;
      }
    }
    if (t.preview) {
      w.Key("preview");
      w.Bool(true);
    }
    if (t.manual) {  // a gRPC stream driven by hand (the Manual checkbox)
      w.Key("manual");
      w.Bool(true);
    }
    // The response it shows, so the restored tab shows it again.
    if (t.resp) {
      const std::string name = "tab" + std::to_string(n) + ".resp.json";
      // (A WebSocket tab's file changes with its message log, a new copy per change.)
      const std::shared_ptr<const void> id = t.resp.ws_log ? std::shared_ptr<const void>(t.resp.ws_log)
                                             : t.resp.http ? std::shared_ptr<const void>(t.resp.http)
                                                           : std::shared_ptr<const void>(t.resp.grpc);
      if (write_once(dir / name, id, [&] { return last_response_to_json(t.resp); }, resp_written)) {
        keep.insert(dir / name);
        w.Key("response");
        w.String(name.c_str());
      }
    }
    if (t.schema) {
      const std::string name = "tab" + std::to_string(n) + ".schema.json";
      if (write_once(dir / name, t.schema, [&] { return schema_to_json(*t.schema); }, resp_written)) {
        keep.insert(dir / name);
        w.Key("schema");
        w.String(name.c_str());
      }
    }
    w.EndObject();
    ++n;
  }
  w.EndArray();
  w.Key("active");
  w.Int(s.active);
  w.EndObject();
  g_resp_written = std::move(resp_written);
  ok = detail::write_file_atomic(session_file(ws), std::string(sb.GetString(), sb.GetSize()), nullptr, true) && ok;
  // What the new session.json no longer names (closed tabs), after it is written.
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.path().extension() == ".json" && !keep.count(e.path())) fs::remove(e.path(), ec);
  return ok;
}
}  // namespace

std::shared_ptr<MainFrame::SessionSnapshot> MainFrame::snapshot_session() {
  finish_swap();  // a swap shown in the next UI step: done first
  if (!ws_) return nullptr;
  static uint64_t seq = 0;
  auto s = std::make_shared<SessionSnapshot>();
  s->seq = ++seq;
  s->ws = std::make_shared<Workspace>(*ws_);
  const int sel = nb_->GetSelection();
  for (size_t i = 0; i < nb_->GetPageCount(); ++i) {
    std::filesystem::path folder;
    if (runner_folder(nb_->GetPage(i), &folder)) {
      // A runner tab: reopened (not rerun) after a restart, for its folder.
      std::filesystem::path rel = folder.empty() ? std::filesystem::path() : folder.lexically_relative(ws_->root());
      if (!folder.empty() && (rel.empty() || *rel.begin() == "..")) continue;
      if (rel == ".") rel.clear();
      SessionSnapshot::Tab st;
      st.runner = rel;
      if (static_cast<int>(i) == sel) s->active = static_cast<int>(s->tabs.size());
      s->tabs.push_back(std::move(st));
      continue;
    }
    unsigned mock_port = 0;
    bool mock_running = false;
    if (mock_tab_info(nb_->GetPage(i), &folder, &mock_port, &mock_running)) {
      // A mock server tab: reopened after a restart on the same port ({{mock}} keeps its URL).
      std::filesystem::path rel = folder.empty() ? std::filesystem::path() : folder.lexically_relative(ws_->root());
      if (!folder.empty() && (rel.empty() || *rel.begin() == "..")) continue;
      if (rel == ".") rel.clear();
      SessionSnapshot::Tab st;
      st.mock = rel;
      st.mock_port = mock_port;
      st.mock_running = mock_running;
      s->tabs.push_back(std::move(st));
      continue;
    }
    auto* t = dynamic_cast<RequestTab*>(nb_->GetPage(i));
    if (!t || t->pristine()) continue;
    SessionSnapshot::Tab st;
    st.file = t->file();
    st.dirty = t->dirty();
    st.preview = t->preview();
    st.manual = t->grpc_manual();
    if (t->dirty() || t->file().empty()) st.draft = t->request();
    st.resp = t->session_response();
    st.schema = t->session_schema();
    if (static_cast<int>(i) == sel) s->active = static_cast<int>(s->tabs.size());
    s->tabs.push_back(std::move(st));
  }
  return s;
}

bool MainFrame::save_session() {
  if (session_timer_) session_timer_->Stop();
  auto s = snapshot_session();
  return s && write_session(*s);  // waits for a background write still running
}

// Something the session holds changed (an edit, a response, a tab opened, closed or selected):
// write it ~1.5 s later on a background thread, so a crash or a hard kill keeps the drafts (as
// Postman and Insomnia do). Coalesced: typing keeps pushing the write back, up to 5 s.
void MainFrame::session_changed() {
  if (!ws_ || restoring_ || closing_) return;
  if (!session_timer_) {
    session_timer_ = new wxTimer(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
      session_due_ = false;
      if (auto s = snapshot_session()) std::thread([s] { write_session(*s); }).detach();
    }, session_timer_->GetId());
  }
  const auto now = Clock::now();
  if (!session_due_) {
    session_due_ = true;
    session_first_change_ = now;
  }
  const double waited = ms_between(session_first_change_, now);
  session_timer_->StartOnce(static_cast<int>(std::clamp(5000.0 - waited, 1.0, 1500.0)));
}

namespace {
// The parsed session of one workspace (read_session), restored by MainFrame::apply_session.
struct SessionTab {
  SavedRequest r;
  bool dirty = false, preview = false, manual = false;
  LastResponse resp;
  std::shared_ptr<const GrpcSchema> schema;
};
}  // namespace

struct MainFrame::Session {
  std::filesystem::path root;
  std::vector<SessionTab> tabs;
  int active = 0;
  // Runner tabs: folder (absolute, inside the root) and position among the restored tabs.
  struct Runner {
    std::filesystem::path folder;
    int pos = 0;
  };
  std::vector<Runner> runners;
  int active_runner = -1;  // the runner tab that was selected (index into runners), if one was
  // Mock server tabs: folder (absolute, inside the root), port, whether it was running.
  struct Mock {
    std::filesystem::path folder;
    unsigned port = 0;
    bool running = false;
    int pos = 0;  // as Runner::pos
  };
  std::vector<Mock> mocks;
};

namespace {
// Reads <root>/.watt/session.json and the drafts it names. Any thread (the workspace opener
// reads it right after opening, so the frame can restore it before the window shows).
std::shared_ptr<MainFrame::Session> read_session(const Workspace& ws) {
  namespace fs = std::filesystem;
  auto out = std::make_shared<MainFrame::Session>();
  out->root = ws.root();
  std::string text;
  if (!detail::read_file(session_file(ws), text)) return out;
  rapidjson::Document d;
  d.Parse(text.data(), text.size());
  if (d.HasParseError() || !d.IsObject() || !d.HasMember("tabs") || !d["tabs"].IsArray()) return out;
  const int saved_active = d.HasMember("active") && d["active"].IsInt() ? d["active"].GetInt() : 0;
  int active = 0, k = -1, pos = 0;
  for (const auto& v : d["tabs"].GetArray()) {
    ++k;
    if (!v.IsObject()) continue;
    auto str = [&](const char* k) { return v.HasMember(k) && v[k].IsString() ? std::string(v[k].GetString()) : std::string(); };
    auto flag = [&](const char* k) { return v.HasMember(k) && v[k].IsBool() && v[k].GetBool(); };
    if (v.HasMember("runner") && v["runner"].IsString()) {
      // Only a relative path below the root, without "..": the session must not open a runner
      // on a folder outside the workspace.
      const fs::path rel = detail::from_utf8(str("runner"));
      bool ok = !rel.is_absolute() && !rel.has_root_name() && !rel.has_root_directory();
      for (const auto& part : rel) ok = ok && part != "..";
      std::error_code ec;
      const fs::path p = rel.empty() ? fs::path() : ws.root() / rel;
      if (ok && (p.empty() || fs::exists(p, ec))) {
        if (k == saved_active) out->active_runner = static_cast<int>(out->runners.size());
        out->runners.push_back({p, pos++});
      }
      continue;
    }
    if (v.HasMember("mock") && v["mock"].IsString()) {
      // As a runner: only a folder below the root.
      const fs::path rel = detail::from_utf8(str("mock"));
      bool ok = !rel.is_absolute() && !rel.has_root_name() && !rel.has_root_directory();
      for (const auto& part : rel) ok = ok && part != "..";
      std::error_code ec;
      const fs::path p = rel.empty() ? fs::path() : ws.root() / rel;
      const unsigned port = v.HasMember("port") && v["port"].IsUint() && v["port"].GetUint() <= 65535 ? v["port"].GetUint() : 0;
      if (ok && (p.empty() || fs::exists(p, ec))) out->mocks.push_back({p, port, flag("running"), pos++});
      continue;
    }
    const fs::path file = session_file_path(ws, str("file"));
    const std::string draft = str("draft");
    SessionTab t;
    std::optional<SavedRequest> r;
    // Only a bare file name inside .watt/session: the session file must not point anywhere else.
    if (!draft.empty() && draft.find_first_of("/\\:") == std::string::npos) {
      r = ws.load_request(session_dir(ws) / detail::from_utf8(draft));
      if (r) {
        r->file = file;
        if (flag("unnamed")) r->name.clear();
        t.dirty = flag("dirty");
      }
    } else if (!file.empty()) {
      r = ws.load_request(file);
    }
    if (!r) continue;  // the file is gone (deleted or renamed outside watt)
    if (k <= saved_active) active = static_cast<int>(out->tabs.size());
    t.r = std::move(*r);
    t.preview = flag("preview");
    t.manual = flag("manual") && t.r.kind == RequestKind::Grpc;
    if (const std::string resp = str("response"); !resp.empty() && resp.find_first_of("/\\:") == std::string::npos) {
      std::string json;
      std::error_code ec;
      const fs::path rp = session_dir(ws) / detail::from_utf8(resp);
      if (fs::file_size(rp, ec) <= 2 * LastResponse::kMaxBytes && !ec && detail::read_file(rp, json))
        t.resp = last_response_from_json(json, t.r);
    }
    if (const std::string sf = str("schema"); !sf.empty() && sf.find_first_of("/\\:") == std::string::npos &&
                                              t.r.kind == RequestKind::Grpc) {
      std::string json;
      std::error_code ec;
      const fs::path sp = session_dir(ws) / detail::from_utf8(sf);
      if (fs::file_size(sp, ec) <= (8u << 20) && !ec && detail::read_file(sp, json)) t.schema = schema_from_json(json);
    }
    out->tabs.push_back(std::move(t));
    ++pos;
  }
  if (!out->tabs.empty()) out->active = std::clamp(active, 0, static_cast<int>(out->tabs.size()) - 1);
  return out;
}

// The opener thread's session, for the frame to take before it shows (or when the workspace
// arrives on the UI thread).
std::mutex g_session_m;
std::shared_ptr<MainFrame::Session> g_session;

void prefetch_session(const Workspace& ws) {
  auto s = read_session(ws);
  std::lock_guard lk(g_session_m);
  g_session = std::move(s);
}

std::shared_ptr<MainFrame::Session> take_session(const std::filesystem::path& root) {
  std::lock_guard lk(g_session_m);
  if (!g_session || (!root.empty() && g_session->root != root)) return nullptr;
  return std::exchange(g_session, nullptr);
}
}  // namespace

void MainFrame::restore_session() {
  if (restored_roots_.count(ws_->root())) return;
  auto s = take_session(ws_->root());
  if (!s) s = read_session(*ws_);
  apply_session(*s);
}

void MainFrame::restore_special_later() {
  if (specials_.empty()) return;
  if (!specials_timer_) {
    specials_timer_ = new wxTimer(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { restore_special_step(); }, specials_timer_->GetId());
  }
  // A timer, not CallAfter: CallAfter events, even posted from one another, all run in one pass.
  if (!specials_timer_->IsRunning()) specials_timer_->StartOnce(1);
}

void MainFrame::restore_special_step() {
  if (closing_) return specials_.clear();
  if (!ws_) {  // start-up: workspace_opened calls restore_special_later
    if (ws_failed_) specials_.clear();
    return;
  }
  if (specials_expect_ && nb_->GetPageCount() < specials_expect_ && ++specials_waits_ < 50) {
    specials_timer_->StartOnce(10);  // the last mock's page goes in on its next turn
    return;
  }
  specials_expect_ = 0;
  specials_waits_ = 0;
  while (!specials_.empty()) {
    SpecialTab t = std::move(specials_.front());
    specials_.pop_front();
    if (t.root != ws_->root()) continue;
    const size_t before = nb_->GetPageCount();
    FrameLaps laps("restore_special");
    t.open();
    laps.lap(t.mock ? "mock" : "runner");
    if (t.mock) specials_expect_ = before + 1;
    break;
  }
  if (!specials_.empty()) specials_timer_->StartOnce(1);
}

// Restores the tabs of s. Before the window shows (from the constructor, no start tab yet) the
// active tab is built complete and the rest lazily; later, the start tab takes the active one if
// the user has not touched it yet, else the restored tabs go after the user's, which stays
// selected. gRPC tabs are parked (RequestTab::park_grpc): no engine, reflection or connection
// until the user works in them.
void MainFrame::apply_session(Session& s) {
  if (!restored_roots_.insert(s.root).second) return;
  RequestTab* start = current_tab();
  const bool early = !start;
  const bool reuse = !s.tabs.empty() && start && nb_->GetPageCount() == 1 && start->pristine() && !start->in_flight();
  // Runner tabs come back at their place in the tab strip, one per turn of the message loop
  // after the window shows (each builds its whole panel), and never start a run. Behind the
  // user's own tabs when those stay.
  const int base = early || reuse ? 0 : static_cast<int>(nb_->GetPageCount());
  // Mock server tabs come back after the window shows, on their port, running if they ran (on
  // loopback); routes are read and the listener started on workers (mock_view.cpp). Runner and
  // mock tabs go in by place (both lists ascend), so each lands where it was.
  // One tab per timer step (restore_special_step; all at once they were one 140-185 ms step),
  // the selected runner first: it goes in at its place among the request tabs, and the tabs
  // before it then go in ascending, each pushing it one place on to where it was.
  {
    std::vector<SpecialTab> q;
    size_t sel = SIZE_MAX;
    for (size_t i = 0, mi = 0; i < s.runners.size() || mi < s.mocks.size();) {
      if (i == s.runners.size() || (mi < s.mocks.size() && s.mocks[mi].pos < s.runners[i].pos)) {
        q.push_back({s.root, [this, m = s.mocks[mi++], at = base] {
          open_mock(this, [this] { return ws_.get(); }, m.folder, [this] { vars_changed(); session_changed(); },
                    m.running, m.port, /*select=*/false, at + m.pos);
        }, true});
        continue;
      }
      const bool select = static_cast<int>(i) == s.active_runner && (early || reuse);
      if (select) sel = q.size();
      const int at = base + s.runners[i].pos - (select ? static_cast<int>(q.size()) : 0);
      q.push_back({s.root, [this, folder = s.runners[i].folder, at, select] {
        open_runner(this, [this] { return ws_.get(); }, folder, &MainFrame::grpc, select, at);
      }, false});
      ++i;
    }
    if (sel != SIZE_MAX) std::rotate(q.begin(), q.begin() + static_cast<std::ptrdiff_t>(sel), q.begin() + static_cast<std::ptrdiff_t>(sel) + 1);
    for (auto& t : q) specials_.push_back(std::move(t));
    restore_special_later();  // before the workspace has opened: when it has (workspace_opened)
  }
  if (s.tabs.empty()) return;
  const int active = s.active;
  wxWindowUpdateLocker lock(nb_);
  restoring_ = true;  // page changes made here are not the user's
  // Before the window shows, the active tab goes in first: the notebook selects the first page
  // it gets, and a lazy page selected even for a moment would be built.
  std::vector<int> order;
  if (early) order.push_back(active);
  for (int i = 0; i < static_cast<int>(s.tabs.size()); ++i)
    if (!early || i != active) order.push_back(i);
  for (const int i : order) {
    SessionTab& t = s.tabs[static_cast<size_t>(i)];
    RequestTab* tab;
    if (early && i == active) {
      tab = add_tab(std::move(t.r), t.preview, /*lazy=*/false, -1, /*park=*/true);
      tab->build_response_view();
    } else if (reuse && i == active) {
      start->park_grpc();
      start->set_request(std::move(t.r), t.preview);
      tab = start;
    } else {
      const bool before = (reuse || early) && i < active;
      tab = add_tab(std::move(t.r), t.preview, /*lazy=*/true, before ? i : -1, /*park=*/true);
    }
    tab->restore_schema(std::move(t.schema));
    if (t.manual) tab->restore_grpc_manual(true);
    if (t.dirty) tab->mark_dirty();
    tab->restore_response(std::move(t.resp));
  }
  // (No SetSelection for the early case: the active tab is already selected, and selecting the
  // current page again moves the keyboard focus to the tab strip instead of the URL field.)
  if (reuse) {
    sidebar_->select_file(start->file());
  }
  restoring_ = false;
  if (painted_) CallAfter([this] { reveal_selected_tab(nb_); });  // else first_idle() does it
}

TabContext MainFrame::tab_context() {
  TabContext ctx;
  ctx.workspace = [this] { return ws_.get(); };
  ctx.grpc = &MainFrame::grpc;
  ctx.history_added = [this](const HistoryEntry& e) { sidebar_->history_added(e); };
  ctx.when_workspace = [this](std::function<void()> fn) { return when_workspace(std::move(fn)); };
  ctx.snapshot_origin = [this](const SavedRequest& r) {
    return sidebar_ ? sidebar_->snapshot_origin(r.file, r.id) : std::filesystem::path();
  };
  return ctx;
}

RequestTab* MainFrame::add_tab(SavedRequest r, bool preview, bool lazy, int at, bool park) {
  finish_swap();  // a swap shown in the next UI step: done first
  // A tab shown now (a request or history entry opened, not a restored one) takes the spare of
  // its kind when there is one: its editor and response view are built already, so opening the
  // first gRPC request is a load into a finished editor, not a 60-85 ms build on the UI thread.
  // Not the start-up tab (before the first paint): its spare waits for a quiet moment like the
  // rest of the prebuild, so start-up settles first (the refill-soon path made it build ~0.2 s
  // after the first paint, which kept the CPU busy until ~0.8 s).
  if (painted_ && !lazy && !park && at < 0 && spare_slot(r.kind) < 2) spare_built_[spare_slot(r.kind)] = true;  // refill soon
  if (!lazy && !park && at < 0) promote_reserve(r.kind);
  if (!lazy && !park && at < 0)
    if (RequestTab*& spare = spare_[spare_slot(r.kind)]) {
      RequestTab* t = std::exchange(spare, nullptr);
      t->set_request(std::move(r), preview);
      t->wake_grpc();
      t->Reparent(nb_);
      nb_->AddPage(t, t->title(), true);
      t->Show();
      if (preview) update_title(t);
      t->focus_url();
      prebuild_later(kQuietMs);
      return t;
    }
  auto* t = new RequestTab(nb_, tab_context(), std::move(r), preview, /*lazy=*/true);
  if (park) t->park_grpc();
  if (!lazy) t->realize();
  hook_tab(t);
  wxString title = t->title();
  if (lazy) {  // a restored background tab: not selected, built when first shown
    if (at >= 0)
      nb_->InsertPage(static_cast<size_t>(at), t, title, false);
    else
      nb_->AddPage(t, title, false);
    if (preview) update_title(t);  // its tooltip
    return t;
  }
  nb_->AddPage(t, title, true);
  if (preview) update_title(t);
  t->focus_url();
  return t;
}

RequestTab* MainFrame::current_tab() const {
  const int s = nb_->GetSelection();
  return s >= 0 ? dynamic_cast<RequestTab*>(nb_->GetPage(s)) : nullptr;  // null: a runner tab
}

int MainFrame::tab_index(RequestTab* t) const { return nb_->GetPageIndex(t); }

void MainFrame::hook_tab(RequestTab* t) {
  t->on_title_changed = [this](RequestTab* tab) {
    update_title(tab);
    if (tab_index(tab) >= 0) session_changed();
  };
  t->on_response = [this](RequestTab* tab) {
    if (tab_index(tab) >= 0) session_changed();
  };
}

void MainFrame::update_title(RequestTab* t) {
  const int i = tab_index(t);
  if (i < 0) return;
  const wxString s = t->title();
  const wxString tip = t->preview() ? wxString::FromUTF8("Preview (italic): the next request you open replaces it. "
                                                         "Double-click, edit or send to keep it.")
                                    : wxString();
  if (nb_->GetPageText(i) != s) nb_->SetPageText(i, s);
  if (nb_->GetPageToolTip(i) != tip) {
    nb_->SetPageToolTip(i, tip);
    nb_->Refresh(false);  // italics on / off: the caption may be unchanged
  }
}

bool MainFrame::close_tab(int idx) {
  finish_swap();  // a swap shown in the next UI step: done first
  if (idx < 0 || idx >= static_cast<int>(nb_->GetPageCount())) return false;
  auto* t = dynamic_cast<RequestTab*>(nb_->GetPage(idx));
  if (t && t->dirty()) {
    nb_->SetSelection(idx);
    wxString what = t->title();
    what.Replace(L" ●", "");  // the unsaved-changes dot
    const int a = ask_save(this, what);
    if (a == wxID_CANCEL) return false;
    if (a == wxID_YES) {
      save_current();
      if (t->dirty()) return false;
    }
  }
  forget_tab_hover(nb_);
  // The last page: its successor first. A notebook left without pages drops its tab strip, and
  // the strip made for the next page has no "+" button.
  if (nb_->GetPageCount() == 1) new_request(RequestKind::Http, {});
  if (t && t->request_kind() == RequestKind::Http) {
    // Hidden and parked now, torn down in slices in quiet moments (prebuild_step): deleting a tab
    // that holds a 12 MB body and response took 50-65 ms in one UI step.
    {
      wxWindowUpdateLocker lock(nb_);
      nb_->RemovePage(idx);
      t->Hide();
      t->Reparent(spare_park_);
    }
    park_closed(t);
  } else {
    nb_->DeletePage(idx);
  }
  if (nb_->GetPageCount() == 0) add_tab(SavedRequest{}, false);
  session_changed();
  return true;
}

// A closed HTTP tab, already out of the notebook, hidden in spare_park_. A pristine built tab
// (Ctrl+T then Ctrl+W, nothing typed or sent) becomes the spare or reserve again, so opening and
// closing tabs in a row builds nothing and frees nothing. Any other tab stops its timers and idle
// work now (its editor would go on building pages while parked) and waits in closed_ for the
// reaper (reap_slice), which runs whether or not the user is typing; beyond kMaxParked the oldest
// is destroyed right away.
void MainFrame::park_closed(RequestTab* t) {
  FrameLaps laps("park_closed");
  if (t->request_kind() == RequestKind::Http && t->pristine() && !t->last_response() && t->realized() &&
      t->has_response_view() && !t->in_flight() && !t->hibernated() && (!spare_[0] || !reserve_[0]) && !closing_) {
    SavedRequest r;
    r.kind = RequestKind::Http;
    t->set_request(std::move(r), false);
    t->set_save_folder({});
    (spare_[0] ? reserve_[0] : spare_[0]) = t;  // a refill under way pauses (spare_slice)
    laps.lap("recycled");
    prebuild_later(kQuietMs);
    return;
  }
  if (!t->realized() || t->hibernated()) {  // no editor, no response view: a few windows
    delete t;
    laps.lap("delete_light");
    return;
  }
  t->teardown_step();  // the first step only stops things (in flight, timers, the editor's prebuild)
  closed_.push_back(t);
  if (closed_.size() > kMaxParked) {
    RequestTab* o = closed_.front();
    closed_.erase(closed_.begin());
    while (o->teardown_step()) {
    }
    delete o;
    laps.lap("delete_oldest");
  }
  reap_later();
}

void MainFrame::reap_later() {
  if (!reap_timer_) {
    reap_timer_ = new wxTimer(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
      if (closing_) return;  // close_slice tears everything down now
      if (reap_slice()) reap_timer_->StartOnce(16);
      else prebuild_later(kQuietMs);  // the spares paused while tabs waited for teardown
    }, reap_timer_->GetId());
  }
  if (!reap_timer_->IsRunning()) reap_timer_->StartOnce(16);
}

// One slice: teardown steps and deletes of parked tabs, oldest first, until ~6 ms are used (a
// step frees one heavy part: big documents on workers, the response view, then the tab).
bool MainFrame::reap_slice() {
  FrameLaps laps("reap");
  // Tabs replaced by a spare beyond the two that may become spares again: torn down too.
  while (retired_.size() > 2) {
    wxWindow* w = retired_.front();
    retired_.erase(retired_.begin());
    if (auto* t = dynamic_cast<RequestTab*>(w)) {
      t->teardown_step();
      closed_.push_back(t);
    } else {
      w->Destroy();
    }
  }
  const auto t0 = std::chrono::steady_clock::now();
  while (!closed_.empty()) {
    RequestTab* t = closed_.front();
    if (t->teardown_step()) {
      laps.lap("teardown_step");
    } else {
      closed_.erase(closed_.begin());
      delete t;
      laps.lap("delete");
    }
    if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(6)) break;
  }
  return !closed_.empty();
}

// The tab strip's context menu (right click on a tab), as in Postman, Insomnia and Bruno.
void MainFrame::tab_menu(int idx) {
  finish_swap();
  const int n = static_cast<int>(nb_->GetPageCount());
  if (idx < 0 || idx >= n) return;
  wxWindow* const pg = nb_->GetPage(static_cast<size_t>(idx));
  auto* t = dynamic_cast<RequestTab*>(pg);
  enum { M_CLOSE = wxID_HIGHEST + 900, M_OTHERS, M_RIGHT, M_ALL, M_SAVED, M_DUP, M_REVEAL };
  wxMenu m;
  m.Append(M_CLOSE, "Close\tCtrl+W");
  m.Append(M_OTHERS, "Close others")->Enable(n > 1);
  m.Append(M_RIGHT, "Close to the right")->Enable(idx < n - 1);
  m.Append(M_SAVED, "Close unchanged tabs");
  m.Append(M_ALL, "Close all");
  m.AppendSeparator();
  m.Append(M_DUP, "Duplicate")->Enable(t != nullptr);
  m.Append(M_REVEAL, "Reveal in sidebar")->Enable(t && !t->file().empty());
  const int c = nb_->GetPopupMenuSelectionFromUser(m);
  // The page may be gone by now (a send finished, the file was deleted): found by pointer again.
  const int at = nb_->GetPageIndex(pg);
  if (c == wxID_NONE || at == wxNOT_FOUND) return;
  std::vector<int> close;
  const int count = static_cast<int>(nb_->GetPageCount());
  switch (c) {
    case M_CLOSE: close_tab(at); return;
    case M_OTHERS:
      for (int i = 0; i < count; ++i)
        if (i != at) close.push_back(i);
      break;
    case M_RIGHT:
      for (int i = at + 1; i < count; ++i) close.push_back(i);
      break;
    case M_ALL:
      for (int i = 0; i < count; ++i) close.push_back(i);
      break;
    case M_SAVED:
      for (int i = 0; i < count; ++i)
        if (auto* rt = dynamic_cast<RequestTab*>(nb_->GetPage(static_cast<size_t>(i))); rt && !rt->dirty() && !rt->in_flight())
          close.push_back(i);
      break;
    case M_DUP:
      if (t) {
        SavedRequest r = t->request();
        r.file.clear();  // a copy: saving it makes a new file
        if (RequestTab* d = add_tab(std::move(r), false, false, at + 1)) {
          d->set_save_folder(t->save_folder());
          d->mark_dirty();
        }
      }
      return;
    case M_REVEAL:
      if (t) sidebar_->select_file(t->file());
      return;
    default: return;
  }
  close_tabs(std::move(close));
}

// Several pages closed as one batch: one question for all the dirty ones, one notebook update.
void MainFrame::close_tabs(std::vector<int> idx) {
  finish_swap();
  std::vector<wxWindow*> pages;
  std::vector<RequestTab*> dirty;
  for (int i : idx) {
    if (i < 0 || i >= static_cast<int>(nb_->GetPageCount())) continue;
    wxWindow* w = nb_->GetPage(static_cast<size_t>(i));
    pages.push_back(w);
    if (auto* t = dynamic_cast<RequestTab*>(w); t && t->dirty()) dirty.push_back(t);
  }
  if (pages.empty()) return;
  if (!dirty.empty()) {
    wxString names;
    for (size_t i = 0; i < dirty.size() && i < 8; ++i) {
      wxString what = dirty[i]->title();
      what.Replace(L" \u25CF", "");
      names += (i ? wxString(L"\n") : wxString()) + L"\u2022 " + what;
    }
    if (dirty.size() > 8) names += wxString::Format(L"\n\u2026 and %zu more", dirty.size() - 8);
    if (dirty.size() == 1) {
      names = dirty[0]->title();
      names.Replace(L" \u25CF", "");
    }
    const int a = ask_save(this, names, dirty.size());
    if (a == wxID_CANCEL) return;
    if (a == wxID_YES)
      for (RequestTab* t : dirty) {
        const int i = nb_->GetPageIndex(t);
        if (i == wxNOT_FOUND) continue;
        nb_->SetSelection(static_cast<size_t>(i));
        save_current();
        if (t->dirty()) return;  // a save failed or was cancelled: stop, the rest stays open
      }
  }
  FrameLaps laps("close_tabs");
  forget_tab_hover(nb_);
  if (pages.size() >= nb_->GetPageCount()) new_request(RequestKind::Http, {});  // see close_tab
  std::vector<RequestTab*> parked;
  {
    wxWindowUpdateLocker lock(nb_);
    // The selected page last: removing it first would select (show, lay out) another one each time.
    const int sel = nb_->GetSelection();
    wxWindow* const sel_pg = sel >= 0 ? nb_->GetPage(static_cast<size_t>(sel)) : nullptr;
    std::stable_partition(pages.begin(), pages.end(), [sel_pg](wxWindow* w) { return w != sel_pg; });
    for (wxWindow* w : pages) {
      const int i = nb_->GetPageIndex(w);
      if (i == wxNOT_FOUND) continue;
      auto* t = dynamic_cast<RequestTab*>(w);
      if (t && t->request_kind() == RequestKind::Http) {
        nb_->RemovePage(static_cast<size_t>(i));
        t->Hide();
        t->Reparent(spare_park_);
        parked.push_back(t);
      } else {
        if (t)
          while (t->teardown_step()) {
          }
        nb_->DeletePage(static_cast<size_t>(i));
      }
    }
  }
  laps.lap("remove");
  // Parked for the reaper without the cap: these tabs held their windows a moment ago, so the
  // count does not grow, and destroying dozens inside this click would stall it.
  for (RequestTab* t : parked) {
    if (!t->realized() || t->hibernated()) {
      delete t;
      continue;
    }
    t->teardown_step();
    closed_.push_back(t);
  }
  laps.lap("park");
  if (!closed_.empty()) reap_later();
  if (nb_->GetPageCount() == 0) add_tab(SavedRequest{}, false);
  session_changed();
}

void MainFrame::save_current() {
  RequestTab* t = current_tab();
  if (!t) return;
  if (!ws_) {
    wxMessageBox("The workspace is still opening; try again in a moment.", "watt", wxOK, this);
    return;
  }
  if (t->file().empty()) {
    std::string name;
    std::filesystem::path folder;
    if (!ask_save_target(this, *ws_, t->suggested_name(), t->save_folder(), &name, &folder)) return;
    t->set_save_target(name, folder);
  }
  std::string err;
  if (!t->save(*ws_, &err)) {
    wxMessageBox(wxString::FromUTF8("Could not save: " + err), "watt", wxOK | wxICON_ERROR, this);
    return;
  }
  sidebar_->request_saved(t->request());
  sidebar_->select_file(t->file());
}

void MainFrame::quick_open() {
  if (!ws_) {
    // Ctrl+P during start-up: open as soon as the workspace is there (once).
    if (!quick_open_pending_ && when_workspace([this] {
          quick_open_pending_ = false;
          quick_open();
        }))
      quick_open_pending_ = true;
    return;
  }
  if (quick_open_shown_) return;
  std::vector<sidebar::QuickItem> items;
  gather(ws_->tree(), "", items);
  quick_open_shown_ = true;
  const std::filesystem::path chosen = sidebar::quick_open(this, std::move(items));
  quick_open_shown_ = false;
  if (!chosen.empty()) open_request(chosen, false);
}

void MainFrame::on_close(wxCloseEvent& e) {
  finish_swap();  // a swap shown in the next UI step: done first
  if (closing_) {  // closed already, tearing down (close_slice): a forced close (log off) ends it now
    e.Skip();
    return;
  }
  save_geometry();
  // Unsaved changes are kept in the session (restored on the next start, like Postman's and
  // VS Code's unsaved tabs): ask only if that could not be written.
  const bool kept = save_session();
  size_t dirty = 0, shown_dirty = 0;
  for (size_t i = 0; i < nb_->GetPageCount(); ++i)
    if (auto* t = dynamic_cast<RequestTab*>(nb_->GetPage(i))) shown_dirty += t->dirty();
  // Tabs of other workspaces opened in this window (the sidebar keeps them as hidden children
  // of the notebook) are not in this workspace's session: those still need the question.
  for (wxWindow* w : nb_->GetChildren())
    if (auto* t = dynamic_cast<RequestTab*>(w); t && nb_->GetPageIndex(t) == wxNOT_FOUND) dirty += t->dirty();
  if (!kept) dirty += shown_dirty;
  if (dirty && e.CanVeto()) {
    const auto a = wxMessageBox(wxString::Format("%zu request(s) have unsaved changes. Quit anyway?", dirty), "watt",
                                wxYES_NO | wxICON_QUESTION, this);
    if (a != wxYES) {
      e.Veto();
      return;
    }
  }
  closing_ = true;
  if (e.CanVeto()) {
    // Gone for the user now; the windows are torn down in slices (a dozen tabs destroyed in one
    // step held the UI thread for ~100 ms), then the frame is destroyed and the app exits.
    e.Veto();
    Hide();
    // A timer per slice, not CallAfter: pending calls queued while the queue is processed run in
    // the same pass, so CallAfter slices would add up to one long step again.
    close_timer_ = new wxTimer(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { close_slice(); }, close_timer_->GetId());
    close_timer_->StartOnce(1);
    return;
  }
  e.Skip();
}

void MainFrame::close_slice() {
  FrameLaps laps("close_slice");
  if (const size_t n = nb_->GetPageCount(); n > 0) {
    // The selected page last: deleting it would select (show, lay out) another one.
    size_t i = n - 1;
    if (static_cast<int>(i) == nb_->GetSelection() && n > 1) --i;
    // One heavy part (an editor's documents, the response view) a slice, then the page.
    if (auto* t = dynamic_cast<RequestTab*>(nb_->GetPage(i)); t && t->teardown_step()) {  // big documents are freed on workers
      laps.lap("teardown_step");
      close_timer_->StartOnce(1);
      return;
    }
    forget_tab_hover(nb_);  // before the page (and its close button) goes
    nb_->DeletePage(i);
    laps.lap("delete_page");
    close_timer_->StartOnce(1);
    return;
  }
  // Then the tabs outside the notebook (the spares, replaced and closed tabs), the same way:
  // destroyed with the frame they were 50+ ms in one step.
  for (RequestTab*& s : spare_)
    if (s) closed_.push_back(std::exchange(s, nullptr));
  for (RequestTab** s : {&reserve_[0], &reserve_[1], &building_[0], &building_[1]})
    if (*s) closed_.push_back(std::exchange(*s, nullptr));
  for (wxWindow* w : std::exchange(retired_, {})) {
    if (auto* t = dynamic_cast<RequestTab*>(w)) closed_.push_back(t);
    else delete w;
  }
  if (!closed_.empty()) {
    RequestTab* t = closed_.back();
    if (!t->teardown_step()) {
      closed_.pop_back();
      delete t;
      laps.lap("delete_parked");
    } else {
      laps.lap("teardown_parked");
    }
    close_timer_->StartOnce(1);
    return;
  }
  close_timer_->Stop();
  Destroy();
}

// ------------------------------------------------------------------ SidebarHost

void MainFrame::open_request(const std::filesystem::path& file, bool preview) { open_impl(file, nullptr, preview); }

// A history snapshot the sidebar has just written on its worker: r is what it wrote, so the UI
// thread does not read the file again (the first read of a file just written took ~7 ms), and a
// swap of the preview tab for a tab of another kind is finished in the next UI step.
void MainFrame::open_loaded(SavedRequest r, bool preview) {
  if (r.file.empty()) return;
  if (r.file.is_relative() && ws_) r.file = ws_->root() / r.file;
  r.file = r.file.lexically_normal();
  const std::filesystem::path file = r.file;
  open_impl(file, &r, preview);
}

void MainFrame::open_impl(const std::filesystem::path& file, SavedRequest* loaded, bool preview) {
  if (closing_) return;  // the window is closed and its tabs are being torn down (close_slice)
  finish_swap();
  for (size_t i = 0; i < nb_->GetPageCount(); ++i) {
    auto* t = dynamic_cast<RequestTab*>(nb_->GetPage(i));
    if (t && t->file() == file) {
      nb_->SetSelection(i);
      if (!preview) t->make_permanent();
      return;
    }
  }
  if (!ws_) return;
  FrameLaps laps("open_request");
  std::optional<SavedRequest> r;
  if (loaded) {
    r = std::move(*loaded);
  } else {
    std::string err;
    r = ws_->load_request(file, &err);
    if (!r) {
      wxMessageBox(wxString::FromUTF8("Could not open: " + err), "watt", wxOK | wxICON_ERROR, this);
      return;
    }
  }
  laps.lap("load");
  const bool defer = loaded != nullptr;
  // The preview tab, or a pristine start tab, is reused only for a request of its own kind: a
  // tab of another kind would build that kind's editor and response view on the spot (58-169 ms
  // on the UI thread). The prebuilt spare of the request's kind takes its place instead.
  if (preview) {
    for (size_t i = 0; i < nb_->GetPageCount(); ++i) {
      auto* t = dynamic_cast<RequestTab*>(nb_->GetPage(i));
      if (t && t->preview() && !t->dirty() && !t->in_flight()) {
        if (t->request_kind() != r->kind && replace_with_spare(i, *r, true, defer)) return;
        t->set_request(std::move(*r), true);
        nb_->SetSelection(i);
        sidebar_->select_file(t->file());  // no page change event when the preview tab is the current one
        laps.lap("preview_set");
        return;
      }
    }
  }
  // Replace a pristine empty start tab instead of piling up "Untitled" tabs.
  if (RequestTab* cur = current_tab(); cur && cur->pristine() && !cur->in_flight()) {
    if (cur->request_kind() != r->kind &&
        replace_with_spare(static_cast<size_t>(nb_->GetSelection()), *r, preview, defer))
      return;
    cur->set_request(std::move(*r), preview);
    sidebar_->select_file(cur->file());  // no page change event when the start tab is reused
    return;
  }
  add_tab(std::move(*r), preview);
}

// The page at idx (a preview or start tab of another kind) gives its place to the spare of r's
// kind, loaded with r. The old tab is parked hidden and destroyed in a later quiet moment (a
// response view holding 10 MB is not torn down inside the click). False: no spare to take.
// defer: the spare is loaded now (hidden) and the pages are swapped in the next UI step (a posted
// message, so it runs before any input and before calls the caller posts after this one): the
// load and the swap with its paint were 45-70 ms together for a gRPC tab, 20-30 ms each apart.
RequestTab* MainFrame::replace_with_spare(size_t idx, SavedRequest& r, bool preview, bool defer) {
  if (idx >= nb_->GetPageCount()) return nullptr;
  FrameLaps laps(r.kind == RequestKind::Grpc ? "swap_grpc" : "swap");
  promote_reserve(r.kind);
  if (spare_slot(r.kind) < 2) spare_built_[spare_slot(r.kind)] = true;  // refill soon (and keep a reserve)
  RequestTab*& spare = spare_[spare_slot(r.kind)];
  RequestTab* t = std::exchange(spare, nullptr);
  if (!t)  // a tab of this kind replaced moments ago (not recycled yet): its editor is built
    for (auto it = retired_.begin(); it != retired_.end(); ++it)
      if (auto* rt = dynamic_cast<RequestTab*>(*it); rt && rt->realized() && rt->request_kind() == r.kind) {
        t = rt;
        retired_.erase(it);
        break;
      }
  if (!t) {
    laps.lap("no_spare");
    return nullptr;
  }
  swap_ = {t, nb_->GetPage(idx), preview};
  t->set_request(std::move(r), preview);
  laps.lap("set_request");
  if (defer) {
    ::PostMessageW(static_cast<HWND>(GetHWND()), kSwapMsg, 0, 0);
    return t;
  }
  finish_swap();
  return t;
}

// The second half of replace_with_spare: the loaded spare takes the old page's place.
void MainFrame::finish_swap() {
  const PendingSwap s = std::exchange(swap_, PendingSwap{});
  if (!s.t) return;
  FrameLaps laps("swap_pages");
  RequestTab* t = s.t;
  const int idx = s.old ? nb_->GetPageIndex(s.old) : wxNOT_FOUND;
  {
    wxWindowUpdateLocker lock(nb_);
    t->wake_grpc();
    t->Reparent(nb_);
    laps.lap("wake_reparent");
    if (idx == wxNOT_FOUND) {  // the old page is gone (closed in between): a tab of its own
      nb_->AddPage(t, t->title(), true);
    } else {
      nb_->InsertPage(static_cast<size_t>(idx), t, t->title(), true);  // selects it: the page-changed handler syncs the sidebar
    }
    laps.lap("insert");
    t->Show();
    if (idx != wxNOT_FOUND) {
      forget_tab_hover(nb_);
      nb_->RemovePage(static_cast<size_t>(idx) + 1);
      s.old->Hide();
      s.old->Reparent(spare_park_);
      retired_.push_back(s.old);
    }
    laps.lap("remove_old");
  }
  laps.lap("unlock");
  if (s.preview) update_title(t);  // a preview keeps the focus where the click was (tree, history)
  else t->focus_url();
  prebuild_later(kQuietMs);
  laps.lap("end");
  if (frame_trace_file())  // the paint and whatever else runs before the next idle
    CallAfter([laps]() mutable { laps.lap("until_idle"); });
}

WXLRESULT MainFrame::MSWWindowProc(WXUINT msg, WXWPARAM wp, WXLPARAM lp) {
  if (msg == kSwapMsg) {
    finish_swap();
    return 0;
  }
  // A resize step lays the notebook out once but painted the tab's windows two or three times:
  // wxAuiTabCtrl::DoApplyRect calls Update() in the middle of the layout, and the moves that
  // follow invalidate them again. With the notebook's drawing off for the layout, everything in
  // it is painted once, afterwards (WM_SETREDRAW directly: a wx Freeze() would make the
  // notebook skip sizing its pages).
  if (msg == WM_SIZE && wp != SIZE_MINIMIZED && nb_ && !nb_->IsFrozen()) {
    // The notebook sizes every page, the hidden ones too. A request tab only notes it (see
    // RequestTab::realize); the other tabs (runner, mock server) get the same: a hidden one is
    // laid out when it is shown again, not at every resize step.
    static std::set<wxWindow*> gated;
    for (size_t i = 0, n = nb_->GetPageCount(); i < n; ++i) {
      wxWindow* const pg = nb_->GetPage(i);
      if (!pg || dynamic_cast<RequestTab*>(pg) || !gated.insert(pg).second) continue;
      auto pending = std::make_shared<bool>(false);
      pg->Bind(wxEVT_SIZE, [pg, pending](wxSizeEvent& e) {
        if (!pg->IsShown()) {
          *pending = true;
          return;  // no Skip: no Layout() now
        }
        e.Skip();
      });
      pg->Bind(wxEVT_SHOW, [pg, pending](wxShowEvent& e) {
        e.Skip();
        if (!e.IsShown() || !*pending) return;
        *pending = false;
        pg->SendSizeEvent();  // its own size handlers too, not only Layout()
      });
      pg->Bind(wxEVT_DESTROY, [pg](wxWindowDestroyEvent& e) {
        e.Skip();
        if (e.GetEventObject() == pg) gated.erase(pg);
      });
    }
    const HWND nb = static_cast<HWND>(nb_->GetHWND());
    if (nb && ::IsWindowVisible(nb)) {
      ::SendMessageW(nb, WM_SETREDRAW, FALSE, 0);
      const WXLRESULT r = wxFrame::MSWWindowProc(msg, wp, lp);
      ::SendMessageW(nb, WM_SETREDRAW, TRUE, 0);
      ::RedrawWindow(nb, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
      return r;
    }
  }
  return wxFrame::MSWWindowProc(msg, wp, lp);
}

void MainFrame::open_history(const HistoryEntry& entry) {
  SavedRequest r;
  if (!entry.request.id.empty() || !entry.request.http.url.empty() || !entry.request.grpc_method.empty()) {
    r = entry.request;
  } else if (ws_) {
    auto h = ws_->history_request(entry);
    if (!h) return;
    r = std::move(*h);
  } else {
    return;
  }
  r.file.clear();  // a history entry is a copy: saving it makes a new file
  add_tab(std::move(r), false);
}

void MainFrame::new_request(RequestKind kind, const std::filesystem::path& folder) {
  finish_swap();  // a swap shown in the next UI step: done first
  SavedRequest r;
  r.kind = kind;
  if (kind == RequestKind::WebSocket) {
    r.auth.type = HttpAuth::Type::Inherit;
    r.http.body = "{\n  \"hello\": \"world\"\n}";
  }
  promote_reserve(kind);
  FrameLaps laps(kind == RequestKind::Grpc ? "new_grpc" : "new_tab");
  // No finished spare, but one being built (tabs opened faster than the refill): its last slice
  // or two now cost less than a whole tab built inline (~55 ms).
  if (const int k = spare_slot(kind); k < 2 && !spare_[k] && building_[k]) {
    RequestTab* b = std::exchange(building_[k], nullptr);
    if (!b->has_response_view()) b->build_response_view();
    spare_[k] = b;
    laps.lap("finish_building");
  }
  RequestTab*& spare = spare_[spare_slot(kind)];
  RequestTab* t;
  if (spare) {
    // Built in a quiet moment (prebuild_step): only the page is added now, so Ctrl+T shows a
    // finished tab at once instead of blocking ~20 ms (editor) plus ~60 ms (response view).
    t = std::exchange(spare, nullptr);
    const bool ready = spare_ready(t);
    t->set_request(std::move(r), false);  // the gRPC editor fills in the last address used
    laps.lap(ready ? "spare.set_request" : "spare(not laid out).set_request");
    t->wake_grpc();
    t->Reparent(nb_);  // from spare_park_ (AddPage would too; explicit for clarity)
    laps.lap("spare.wake_reparent");
    nb_->AddPage(t, t->title(), true);
    t->Show();
    laps.lap("spare.add_show");
    t->focus_url();
    laps.lap("spare.focus");
  } else {
    t = add_tab(std::move(r), false);
    laps.lap("inline");
  }
  if (kind == RequestKind::Grpc) want_grpc_spare_ = true;
  if (spare_slot(kind) < 2) spare_built_[spare_slot(kind)] = true;  // refill soon (and keep a reserve)
  prebuild_later(kQuietMs);
  if (!folder.empty()) t->set_save_folder(folder);
}

// After start-up, in moments without input, the frame builds what the next keystrokes are
// likely to need: the editors and response views of restored background tabs (so the first
// Ctrl+Tab into one is a plain switch), and a spare new tab (so Ctrl+T, and Ctrl+Shift+T once
// gRPC is in use, only add a page). One slice per timer step (a tab's editor, or one response
// view), each only after kQuietMs without any input, rechecked before every slice: the next slice
// waits for the next quiet moment as soon as the user touches a key or the mouse.
void MainFrame::prebuild_later(int ms) {
  if (!prebuild_timer_) {
    prebuild_timer_ = new wxTimer(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { prebuild_step(); }, prebuild_timer_->GetId());
  }
  // A spare was taken (a request opened from the tree, Ctrl+T / Ctrl+Shift+T): the finished
  // reserve takes its place at once, and the refill starts after a short pause (see spare_slice),
  // so tabs opened in quick succession each take a finished spare. A longer wait already set is
  // cut short.
  for (int k = 0; k < 2; ++k) {
    if (!spare_[k] && reserve_[k]) spare_[k] = std::exchange(reserve_[k], nullptr);
    if (spare_built_[k] && spare_slice(k, false)) {
      prebuild_timer_->StartOnce(std::min(ms, kRefillQuietMs));
      return;
    }
  }
  if (!prebuild_timer_->IsRunning()) prebuild_timer_->StartOnce(ms);
}

wxSize MainFrame::page_size() const {
  const int s = nb_->GetSelection();
  return s >= 0 ? nb_->GetPage(static_cast<size_t>(s))->GetSize() : wxSize();
}

bool MainFrame::spare_ready(RequestTab* t) const {
  const wxSize ps = page_size();
  return t && t->realized() && t->has_response_view() && (ps.x <= 0 || ps.y <= 0 || t->laid_out_at(ps));
}

// The HTTP (k = 0) and gRPC (k = 1) spares are refilled in slices of their own, each a separate
// timer step: the tab with its editor, then its response view, then its layout at the page size
// (one slice for all of it was 51-105 ms on the UI thread, and the layout left for the click made
// the open itself 55-100 ms). A tab is handed out only when finished. Once tabs of that kind are
// being opened, a second finished spare is kept, so a burst of opens (Ctrl+Shift+T twice in a
// row was 41-46 ms) never builds a tab inline. The first gRPC spare is built at low priority in
// prebuild_step (gRPC may never be used); from the first gRPC tab opened on, gRPC is refilled
// here. run=false: only says whether there is a slice to do.
bool MainFrame::spare_slice(int k, bool run) {
  if (k == 1 && !spare_built_[1]) return false;  // see above
  // Not while closed tabs wait for the reaper, nor close to the window quota: a spare is ~76 USER
  // objects (refilled once the reaper is done: reap_later).
  if (!closed_.empty() || user_objects() > kUserBusy) return false;
  const wxSize ps = page_size();
  const bool sized = ps.x > 0 && ps.y > 0;
  if (!spare_[k] && reserve_[k]) spare_[k] = std::exchange(reserve_[k], nullptr);
  // A finished spare after the window (or the sidebar) was resized: laid out again, one a slice.
  for (RequestTab* t : {spare_[k], reserve_[k]})
    if (t && sized && t->realized() && t->has_response_view() && !t->laid_out_at(ps)) {
      if (run) t->prelayout(ps);
      return true;
    }
  if (!building_[k] && spare_[k] && (reserve_[k] || !spare_built_[k])) return false;
  // Both slots refilled meanwhile (closed tabs recycled): a half-built tab waits, finished when
  // a slot is free again instead of built and thrown away.
  if (building_[k] && spare_[k] && reserve_[k]) return false;
  if (!run) return true;
  FrameLaps laps("spare_slice");
  if (!building_[k]) {
    SavedRequest r;
    r.kind = k == 0 ? RequestKind::Http : RequestKind::Grpc;
    auto* t = new RequestTab(spare_park_, tab_context(), std::move(r), false, /*lazy=*/true);
    t->Hide();
    hook_tab(t);
    if (k == 1) t->park_grpc();  // its engine waits until it is used
    t->realize();
    building_[k] = t;
    laps.lap("tab");
    return true;
  }
  if (!building_[k]->has_response_view()) {
    building_[k]->build_response_view();
    laps.lap("response_view");
    return true;
  }
  if (sized && !building_[k]->laid_out_at(ps)) {
    building_[k]->prelayout(ps);
    laps.lap("layout");
  }
  RequestTab* t = std::exchange(building_[k], nullptr);
  if (spare_[k] && reserve_[k]) {  // both refilled meanwhile (closed tabs recycled): not needed
    t->teardown_step();
    closed_.push_back(t);
    reap_later();
    return true;
  }
  (spare_[k] ? reserve_[k] : spare_[k]) = t;
  spare_built_[k] = true;  // from now on a spare taken is refilled after a short pause
  return true;
}

void MainFrame::prebuild_step() {
  finish_swap();  // a swap shown in the next UI step: done first
  // The HTTP spares first, then the gRPC ones once gRPC tabs are being opened, after a short
  // pause without input (rechecked before every slice: the next slice waits as soon as a key or
  // the mouse is used).
  if (IsShown() && !IsIconized())
    for (int k = 0; k < 2; ++k) {
      if (!spare_slice(k, false)) continue;
      if (const int w = input_quiet_wait(spare_built_[k] ? kRefillQuietMs : kQuietMs); w > 0) {
        prebuild_later(w);
        return;
      }
      spare_slice(k, true);
      prebuild_later(16);
      return;
    }
  if (const int wait = input_quiet_wait(kQuietMs); wait > 0) {
    prebuild_later(wait);
    return;
  }
  if (!IsShown() || IsIconized()) {
    prebuild_later(1000);
    return;
  }
  // Between slices: let paints, timers and the tabs' own deferred events run first.
  constexpr int kNext = 30;
  if (user_objects() > kUserBusy) {  // nothing built speculatively close to the window quota
    if (!closed_.empty()) reap_later();
    prebuild_later(2000);
    return;
  }
  if (!closed_.empty()) {  // closed tabs: one heavy part per slice, then the (light) tab itself
    RequestTab* t = closed_.back();
    if (!t->teardown_step()) {
      closed_.pop_back();
      delete t;
    }
    prebuild_later(kNext);
    return;
  }
  if (!retired_.empty()) {
    // A tab replaced by a spare: it becomes the spare of its kind when that slot is empty (its
    // editor and response view are built, so no new tab needs building), or the reserve of an
    // HTTP / gRPC spare, else it is torn down. One per slice.
    wxWindow* w = retired_.back();
    retired_.pop_back();
    auto* t = dynamic_cast<RequestTab*>(w);
    const int k = t ? spare_slot(t->request_kind()) : 0;
    const bool reserve_free = t && k < 2 && !reserve_[k] && t->has_response_view();
    if (t && t->realized() && !t->in_flight() && (!spare_[k] || reserve_free)) {
      SavedRequest r;
      r.kind = t->request_kind();
      t->set_request(std::move(r), false);
      t->set_save_folder({});
      if (k == 1) t->restore_grpc_manual(false);  // a spare opens as an ordinary call
      if (!spare_[k]) spare_[k] = t;
      else reserve_[k] = t;
    } else if (t) {
      t->teardown_step();
      closed_.push_back(t);  // torn down in slices like a closed tab
      reap_later();
    } else {
      delete w;
    }
    prebuild_later(kNext);
    return;
  }
  for (size_t i = 0; i < nb_->GetPageCount(); ++i) {
    auto* t = dynamic_cast<RequestTab*>(nb_->GetPage(i));
    if (!t || t->hibernated()) continue;  // gave its windows back (RequestTab::hibernate): built when shown
    if (!t->realized()) {
      t->realize();  // a parked gRPC tab stays parked: no engine until it is used
      prebuild_later(kNext);
      return;
    }
    if (!t->has_response_view()) {
      t->build_response_view();
      prebuild_later(kNext);
      return;
    }
  }
  // The first gRPC spare, and a WebSocket spare, last: opening a WebSocket request or history
  // entry, or Ctrl+Alt+T, used to build its editor and log on the spot (48-175 ms on the UI thread).
  for (int k = 1; k < 3; ++k) {  // HTTP: spare_slice
    if (k == 1 && (!want_grpc_spare_ || building_[1])) continue;
    if (!spare_[k]) {
      SavedRequest r;
      r.kind = k == 1 ? RequestKind::Grpc : RequestKind::WebSocket;
      // Parked outside the notebook (see spare_park_): sized and shown when new_request adds it.
      auto* t = new RequestTab(spare_park_, tab_context(), std::move(r), false, /*lazy=*/true);
      t->Hide();
      hook_tab(t);
      t->park_grpc();
      t->realize();
      spare_[k] = t;
      prebuild_later(kNext);
      return;
    }
    if (!spare_[k]->has_response_view()) {
      spare_[k]->build_response_view();
      prebuild_later(kNext);
      return;
    }
    const wxSize ps = page_size();
    if (ps.x > 0 && ps.y > 0 && !spare_[k]->laid_out_at(ps)) {  // laid out at the page size too
      spare_[k]->prelayout(ps);
      prebuild_later(kNext);
      return;
    }
  }
}

void MainFrame::file_moved(const std::filesystem::path& from, const std::filesystem::path& to) {
  finish_swap();  // a swap shown in the next UI step: done first
  for (size_t i = 0; i < nb_->GetPageCount();) {
    auto* t = dynamic_cast<RequestTab*>(nb_->GetPage(i));
    if (t && t->file() == from) {
      if (to.empty() && !t->dirty()) {
        forget_tab_hover(nb_);
        nb_->DeletePage(i);
        continue;
      }
      t->file_moved(to);
    }
    ++i;
  }
  if (nb_->GetPageCount() == 0) add_tab(SavedRequest{}, false);
}

void MainFrame::environment_changed() {
  // Nothing cached here: every send expands {{vars}} from the workspace at send time. The
  // sidebar also calls this after switching to another workspace: bring back that workspace's
  // last session the first time it is opened in this process.
  if (ws_ && !restored_roots_.count(ws_->root())) restore_session();
  vars_changed();
}

void MainFrame::vars_changed() {
  for (size_t i = 0; i < nb_->GetPageCount(); ++i)
    if (auto* t = dynamic_cast<RequestTab*>(nb_->GetPage(i))) t->vars_changed();
}

void MainFrame::run_folder(const std::filesystem::path& folder) {
  // The runner reads the folder and sends on its own worker threads; until the workspace has
  // opened (start-up) the request waits for it.
  auto open = [this, folder] { open_runner(this, [this] { return ws_.get(); }, folder, &MainFrame::grpc); };
  if (!when_workspace(open) && ws_) open();
}

void MainFrame::start_mock(const std::filesystem::path& folder) {
  // Routes are read and the listener started on worker threads (mock_view.cpp).
  auto open = [this, folder] {
    // {{mock}} changed: the editors recolour, and the session keeps the tab and its port.
    open_mock(this, [this] { return ws_.get(); }, folder, [this] { vars_changed(); session_changed(); });
  };
  if (!when_workspace(open) && ws_) open();
}

void MainFrame::SetTitle(const wxString& title) {
  // "watt - C:\Users\me\...\api" (the sidebar sets this) -> "api - watt": the full path is
  // noise in the task bar and shows the user's profile path in screen shares.
  wxString rest;
  if (title.StartsWith("watt - ", &rest) && std::filesystem::path(rest.ToStdWstring()).is_absolute()) {
    std::wstring name = std::filesystem::path(rest.ToStdWstring()).filename().wstring();
    if (name.empty()) name = rest.ToStdWstring();
    wxFrame::SetTitle(wxString(name) + " - watt");
    return;
  }
  wxFrame::SetTitle(title);
}

}  // namespace watt::app
