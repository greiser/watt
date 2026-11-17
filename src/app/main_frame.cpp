#include "app/main_frame.h"

#include <wx/accel.h>
#include <wx/app.h>
#include <wx/config.h>
#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/display.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/timer.h>
#include <wx/wupdlock.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#include "app/request_tab.h"
#include "app/response_widgets.h"  // rv::Theme (the "+" button)
#include "app/runner_view.h"
#include "app/sidebar_quickopen.h"
#include "core/model_json.h"  // session file: read / atomic write, UTF-8 paths

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <windows.h>

namespace watt::app {

namespace {

enum {
  ID_SEND = wxID_HIGHEST + 100,
  ID_NEW_TAB,
  ID_NEW_GRPC_TAB,
  ID_CLOSE_TAB,
  ID_FOCUS_URL,
  ID_SAVE,
  ID_QUICK_OPEN,
  ID_NEXT_TAB,
  ID_PREV_TAB,
  ID_FOCUS_FILTER,
};

// The frame, for background threads that finish after it is gone (cleared in ~MainFrame).
std::atomic<MainFrame*> g_frame{nullptr};

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

 private:
  static wxRect place(wxWindow* wnd, const wxRect& in, int orientation) {
    const int w = wnd->FromDIP(26), h = std::min(in.height, wnd->FromDIP(24));
    const int y = in.y + (in.height - h) / 2;
    return orientation == wxLEFT ? wxRect(in.x, y, w, h) : wxRect(in.x + in.width - w, y, w, h);
  }
};

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
int ask_save(wxWindow* parent, const wxString& what) {
  const auto& th = rv::Theme::get();
  wxDialog dlg(parent, wxID_ANY, "watt", wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX);
  dlg.SetBackgroundColour(th.bg);
  auto* sz = new wxBoxSizer(wxVERTICAL);
  auto* head = new wxStaticText(&dlg, wxID_ANY, wxString::Format(L"Save changes to “%s”?", what));
  head->SetForegroundColour(th.fg);
  head->SetFont(head->GetFont().Bold().Scaled(1.1f));
  auto* body = new wxStaticText(&dlg, wxID_ANY, "Your changes are lost if you close the tab without saving.");
  body->SetForegroundColour(th.dim);
  sz->Add(head, 0, wxLEFT | wxRIGHT | wxTOP, dlg.FromDIP(18));
  sz->AddSpacer(dlg.FromDIP(6));
  sz->Add(body, 0, wxLEFT | wxRIGHT, dlg.FromDIP(18));
  auto* row = new wxBoxSizer(wxHORIZONTAL);
  auto* save = new wxButton(&dlg, wxID_YES, "Save");
  auto* discard = new wxButton(&dlg, wxID_NO, "Don't save");
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
  dlg.SetSizerAndFit(sz);
  dlg.SetMinSize(wxSize(dlg.FromDIP(380), -1));
  dlg.Fit();
  dlg.CentreOnParent();
  return dlg.ShowModal();
}

std::string focus_desc() {
  wxWindow* f = wxWindow::FindFocus();
  if (!f) return "none";
  const wxRect r = f->GetScreenRect();
  return wxString::Format("%s %dx%d", f->GetClassInfo()->GetClassName(), r.width, r.height).utf8_string();
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
      {wxACCEL_CTRL | wxACCEL_SHIFT, 'T', ID_NEW_GRPC_TAB},
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

  Bind(wxEVT_MENU, [this](wxCommandEvent&) { if (auto* t = current_tab()) t->send(); }, ID_SEND);
  Bind(wxEVT_MENU, [this](wxCommandEvent&) { new_request(RequestKind::Http, {}); }, ID_NEW_TAB);
  Bind(wxEVT_MENU, [this](wxCommandEvent&) { new_request(RequestKind::Grpc, {}); }, ID_NEW_GRPC_TAB);
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
    const int idx = e.GetSelection();
    if (idx < 0 || idx >= static_cast<int>(nb_->GetPageCount())) return;
    auto* t = static_cast<RequestTab*>(nb_->GetPage(idx));
    if (!t->dirty()) return;  // not vetoed: the notebook closes it
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
  // Give the first tab its curl handle (on its worker) so its first send does not create one.
  if (auto* t = current_tab()) t->warm_up();
  for (size_t i = 0; i < nb_->GetPageCount(); ++i)
    if (static_cast<RequestTab*>(nb_->GetPage(i))->request_kind() == RequestKind::Grpc) want_grpc_spare_ = true;
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
                for (const auto& h : ws_->history(20)) found = found || h.summary == "GET http://127.0.0.1:1/launch";
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
    if (std::filesystem::exists(dir / "watt.json", ec)) {
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
}  // namespace

// What save_session writes, taken on the UI thread (cheap: only tabs with unsaved changes copy
// their request, responses are shared pointers) and written on any thread.
struct MainFrame::SessionSnapshot {
  struct Tab {
    std::filesystem::path file;
    std::optional<SavedRequest> draft;  // unsaved changes, or never saved
    bool dirty = false, preview = false;
    LastResponse resp;
    std::shared_ptr<const GrpcSchema> schema;  // a gRPC tab's schema (RequestTab::session_schema)
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
    w.Key("file");
    w.String(detail::to_utf8(t.file).c_str());
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
    // The response it shows, so the restored tab shows it again.
    if (t.resp) {
      const std::string name = "tab" + std::to_string(n) + ".resp.json";
      const std::shared_ptr<const void> id = t.resp.http ? std::shared_ptr<const void>(t.resp.http)
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
  if (!ws_) return nullptr;
  static uint64_t seq = 0;
  auto s = std::make_shared<SessionSnapshot>();
  s->seq = ++seq;
  s->ws = std::make_shared<Workspace>(*ws_);
  const int sel = nb_->GetSelection();
  for (size_t i = 0; i < nb_->GetPageCount(); ++i) {
    auto* t = static_cast<RequestTab*>(nb_->GetPage(i));
    if (t->pristine()) continue;
    SessionSnapshot::Tab st;
    st.file = t->file();
    st.dirty = t->dirty();
    st.preview = t->preview();
    if (t->dirty() || t->file().empty()) st.draft = t->request();
    st.resp = t->last_response();
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
  bool dirty = false, preview = false;
  LastResponse resp;
  std::shared_ptr<const GrpcSchema> schema;
};
}  // namespace

struct MainFrame::Session {
  std::filesystem::path root;
  std::vector<SessionTab> tabs;
  int active = 0;
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
  int active = 0, k = -1;
  for (const auto& v : d["tabs"].GetArray()) {
    ++k;
    if (!v.IsObject()) continue;
    auto str = [&](const char* k) { return v.HasMember(k) && v[k].IsString() ? std::string(v[k].GetString()) : std::string(); };
    auto flag = [&](const char* k) { return v.HasMember(k) && v[k].IsBool() && v[k].GetBool(); };
    const fs::path file = detail::from_utf8(str("file"));
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

// Restores the tabs of s. Before the window shows (from the constructor, no start tab yet) the
// active tab is built complete and the rest lazily; later, the start tab takes the active one if
// the user has not touched it yet, else the restored tabs go after the user's, which stays
// selected. gRPC tabs are parked (RequestTab::park_grpc): no engine, reflection or connection
// until the user works in them.
void MainFrame::apply_session(Session& s) {
  if (!restored_roots_.insert(s.root).second || s.tabs.empty()) return;
  RequestTab* start = current_tab();
  const bool early = !start;
  const bool reuse = start && nb_->GetPageCount() == 1 && start->pristine() && !start->in_flight();
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
    if (t.dirty) tab->mark_dirty();
    tab->restore_response(std::move(t.resp));
  }
  // (No SetSelection for the early case: the active tab is already selected, and selecting the
  // current page again moves the keyboard focus to the tab strip instead of the URL field.)
  if (reuse) {
    sidebar_->select_file(start->file());
  }
  restoring_ = false;
}

TabContext MainFrame::tab_context() {
  TabContext ctx;
  ctx.workspace = [this] { return ws_.get(); };
  ctx.grpc = &MainFrame::grpc;
  ctx.history_added = [this](const HistoryEntry& e) { sidebar_->history_added(e); };
  ctx.when_workspace = [this](std::function<void()> fn) { return when_workspace(std::move(fn)); };
  return ctx;
}

RequestTab* MainFrame::add_tab(SavedRequest r, bool preview, bool lazy, int at, bool park) {
  auto* t = new RequestTab(nb_, tab_context(), std::move(r), preview, /*lazy=*/true);
  if (park) t->park_grpc();
  if (!lazy) t->realize();
  hook_tab(t);
  wxString title = t->title();
  if (preview) title = "~ " + title;
  if (lazy) {  // a restored background tab: not selected, built when first shown
    if (at >= 0)
      nb_->InsertPage(static_cast<size_t>(at), t, title, false);
    else
      nb_->AddPage(t, title, false);
    return t;
  }
  nb_->AddPage(t, title, true);
  t->focus_url();
  return t;
}

RequestTab* MainFrame::current_tab() const {
  const int s = nb_->GetSelection();
  return s >= 0 ? static_cast<RequestTab*>(nb_->GetPage(s)) : nullptr;
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
  wxString s = t->title();
  if (t->preview()) s = "~ " + s;
  if (nb_->GetPageText(i) != s) nb_->SetPageText(i, s);
}

bool MainFrame::close_tab(int idx) {
  if (idx < 0 || idx >= static_cast<int>(nb_->GetPageCount())) return false;
  auto* t = static_cast<RequestTab*>(nb_->GetPage(idx));
  if (t->dirty()) {
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
  nb_->DeletePage(idx);
  if (nb_->GetPageCount() == 0) add_tab(SavedRequest{}, false);
  session_changed();
  return true;
}

void MainFrame::save_current() {
  RequestTab* t = current_tab();
  if (!t) return;
  if (!ws_) {
    wxMessageBox("The workspace is still opening; try again in a moment.", "watt", wxOK, this);
    return;
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
  save_geometry();
  // Unsaved changes are kept in the session (restored on the next start, like Postman's and
  // VS Code's unsaved tabs): ask only if that could not be written.
  const bool kept = save_session();
  size_t dirty = 0, shown_dirty = 0;
  for (size_t i = 0; i < nb_->GetPageCount(); ++i) shown_dirty += static_cast<RequestTab*>(nb_->GetPage(i))->dirty();
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
  e.Skip();
}

// ------------------------------------------------------------------ SidebarHost

void MainFrame::open_request(const std::filesystem::path& file, bool preview) {
  for (size_t i = 0; i < nb_->GetPageCount(); ++i) {
    auto* t = static_cast<RequestTab*>(nb_->GetPage(i));
    if (t->file() == file) {
      nb_->SetSelection(i);
      if (!preview) t->make_permanent();
      return;
    }
  }
  if (!ws_) return;
  std::string err;
  auto r = ws_->load_request(file, &err);
  if (!r) {
    wxMessageBox(wxString::FromUTF8("Could not open: " + err), "watt", wxOK | wxICON_ERROR, this);
    return;
  }
  if (preview) {
    for (size_t i = 0; i < nb_->GetPageCount(); ++i) {
      auto* t = static_cast<RequestTab*>(nb_->GetPage(i));
      if (t->preview() && !t->dirty() && !t->in_flight()) {
        t->set_request(std::move(*r), true);
        nb_->SetSelection(i);
        return;
      }
    }
  }
  // Replace a pristine empty start tab instead of piling up "Untitled" tabs.
  if (RequestTab* cur = current_tab(); cur && cur->pristine() && !cur->in_flight()) {
    cur->set_request(std::move(*r), preview);
    sidebar_->select_file(cur->file());  // no page change event when the start tab is reused
    return;
  }
  add_tab(std::move(*r), preview);
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
  SavedRequest r;
  r.kind = kind;
  RequestTab*& spare = spare_[kind == RequestKind::Http ? 0 : 1];
  RequestTab* t;
  if (spare) {
    // Built in a quiet moment (prebuild_step): only the page is added now, so Ctrl+T shows a
    // finished tab at once instead of blocking ~20 ms (editor) plus ~60 ms (response view).
    t = std::exchange(spare, nullptr);
    t->set_request(std::move(r), false);  // the gRPC editor fills in the last address used
    t->wake_grpc();
    t->Reparent(nb_);  // from spare_park_ (AddPage would too; explicit for clarity)
    nb_->AddPage(t, t->title(), true);
    t->Show();
    t->focus_url();
  } else {
    t = add_tab(std::move(r), false);
  }
  if (kind == RequestKind::Grpc) want_grpc_spare_ = true;
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
  if (!prebuild_timer_->IsRunning()) prebuild_timer_->StartOnce(ms);
}

void MainFrame::prebuild_step() {
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
  for (size_t i = 0; i < nb_->GetPageCount(); ++i) {
    auto* t = static_cast<RequestTab*>(nb_->GetPage(i));
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
  for (int k = 0; k < 2; ++k) {
    if (k == 1 && !want_grpc_spare_) continue;
    if (!spare_[k]) {
      SavedRequest r;
      r.kind = k == 0 ? RequestKind::Http : RequestKind::Grpc;
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
  }
}

void MainFrame::file_moved(const std::filesystem::path& from, const std::filesystem::path& to) {
  for (size_t i = 0; i < nb_->GetPageCount();) {
    auto* t = static_cast<RequestTab*>(nb_->GetPage(i));
    if (t->file() == from) {
      if (to.empty() && !t->dirty()) {
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
}

void MainFrame::run_folder(const std::filesystem::path& folder) {
  // The runner reads the folder and sends on its own worker threads; until the workspace has
  // opened (start-up) the request waits for it.
  auto open = [this, folder] { open_runner(this, [this] { return ws_.get(); }, folder, &MainFrame::grpc); };
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
