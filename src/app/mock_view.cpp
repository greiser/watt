#include "app/mock_view.h"

#include <wx/app.h>
#include <wx/aui/auibook.h>
#include <wx/button.h>
#include <wx/clipbrd.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/toplevel.h>
#include <wx/utils.h>
#include <wx/weakref.h>

#include <algorithm>
#include <chrono>
#include <share.h>

#include <cstdio>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include "app/http_editor_widgets.h"  // FlatButton
#include "app/response_examples.h"
#include "app/response_widgets.h"  // rv::Theme
#include "app/runner_engine.h"     // runner::collect

namespace watt::app {
namespace fs = std::filesystem;

namespace {

// WATT_FRAME_TRACE=<file>: "mock.<step> ms" lines for the tab's UI-thread steps (as main_frame).
FILE* trace_file() {
  static FILE* f = []() -> FILE* {
    wxString p;
    FILE* out = nullptr;
    if (wxGetEnv("WATT_FRAME_TRACE", &p) && !p.empty()) out = _wfsopen(p.wc_str(), L"a", _SH_DENYNO);
    return out;
  }();
  return f;
}
struct Laps {
  const char* what;
  std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
  explicit Laps(const char* w) : what(w) {}
  void lap(const char* part) {
    if (FILE* f = trace_file()) {
      const auto now = std::chrono::steady_clock::now();
      const long long wall = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch()).count();
      std::fprintf(f, "%s.%s %.2f @%lld\n", what, part, std::chrono::duration<double, std::milli>(now - t).count(), wall);
      std::fflush(f);
      t = now;
    }
  }
};

// Runs f on a later turn of the message loop (a one-shot timer), after the window messages
// already queued: CallAfter events posted from CallAfter run back to back in one go (as
// runner_view.cpp's next_turn).
void next_turn(std::function<void()> f) {
  auto* t = new wxTimer();
  t->Bind(wxEVT_TIMER, [t, f = std::move(f)](wxTimerEvent&) {
    f();
    wxTheApp->CallAfter([t] { delete t; });
  });
  t->StartOnce(1);
}

constexpr size_t kMaxLogRows = 20000;
constexpr size_t kMaxExamplesPerRoute = 20;

wxString U(const std::string& s) { return wxString::FromUTF8(s.data(), s.size()); }
std::string u8(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

std::wstring mock_key(const fs::path& root, const fs::path& folder) {
  fs::path p = folder.empty() ? root : folder;
  std::wstring k = p.lexically_normal().wstring();
  while (k.size() > 3 && (k.back() == L'\\' || k.back() == L'/')) k.pop_back();
  std::transform(k.begin(), k.end(), k.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
  return k;
}

// Lives as long as any worker or queued CallAfter needs it; frame is set / cleared on the UI
// thread only and only read there.
class MockFrame;
struct Link {
  MockFrame* frame = nullptr;
  std::mutex m;
  std::vector<mock::LogEntry> pending;  // log rows not drained by the UI yet
  bool posted = false;
};

std::map<std::wstring, wxWindow*>& open_mocks() {
  static std::map<std::wstring, wxWindow*> m;
  return m;
}

class LogList : public wxListCtrl {
 public:
  LogList(wxWindow* parent, const std::deque<mock::LogEntry>* rows)
      : wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_VIRTUAL | wxBORDER_NONE),
        rows_(rows) {
    const auto& th = rv::Theme::get();
    ok_.SetTextColour(th.ok);
    warn_.SetTextColour(th.warn);
    err_.SetTextColour(th.err);
    AppendColumn("Time", wxLIST_FORMAT_LEFT, FromDIP(92));
    AppendColumn("Method", wxLIST_FORMAT_LEFT, FromDIP(64));
    AppendColumn("Path", wxLIST_FORMAT_LEFT, FromDIP(330));
    AppendColumn("Matched route", wxLIST_FORMAT_LEFT, FromDIP(260));
    AppendColumn("Example", wxLIST_FORMAT_LEFT, FromDIP(150));
    AppendColumn("Status", wxLIST_FORMAT_RIGHT, FromDIP(56));
    AppendColumn("Served", wxLIST_FORMAT_RIGHT, FromDIP(84));
  }

 private:
  wxString OnGetItemText(long item, long col) const override {
    if (item < 0 || static_cast<size_t>(item) >= rows_->size()) return {};
    const mock::LogEntry& e = (*rows_)[static_cast<size_t>(item)];
    switch (col) {
      case 0: {
        const std::time_t t = std::chrono::system_clock::to_time_t(e.when);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(e.when.time_since_epoch()).count() % 1000;
        std::tm tm{};
        localtime_s(&tm, &t);
        return wxString::Format("%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
      }
      case 1: return U(e.method);
      case 2: return U(e.target);
      case 3: return e.route.empty() ? wxString::FromUTF8("\xE2\x80\x94 no match") : U(e.route);
      case 4: return U(e.example);
      case 5: return wxString::Format("%d", e.status);
      case 6:
        return e.served_ms < 10 ? wxString::Format("%.3f ms", e.served_ms) : wxString::Format("%.1f ms", e.served_ms);
      default: return {};
    }
  }
  wxItemAttr* OnGetItemAttr(long item) const override {
    if (item < 0 || static_cast<size_t>(item) >= rows_->size()) return nullptr;
    const int s = (*rows_)[static_cast<size_t>(item)].status;
    return s >= 500 ? &err_ : s >= 400 ? &warn_ : s >= 200 && s < 300 ? &ok_ : nullptr;
  }
  const std::deque<mock::LogEntry>* rows_;
  mutable wxItemAttr ok_, warn_, err_;
};

class MockFrame : public wxPanel {
 public:
  MockFrame(wxWindow* parent, std::function<Workspace*()> workspace, const Workspace& ws, fs::path folder,
            std::function<void()> vars_changed)
      : workspace_(std::move(workspace)),
        vars_changed_(std::move(vars_changed)),
        root_(ws.root()),
        folder_(std::move(folder)),
        server_(std::make_shared<mock::MockServer>()) {
    // Created hidden: child controls made under a shown window cost several ms each (the
    // notebook shows the page when it is selected).
    Hide();
    Create(parent, wxID_ANY);
    link_ = std::make_shared<Link>();
    link_->frame = this;
    key_ = mock_key(root_, folder_);
    const fs::path& p = folder_.empty() || key_ == mock_key(root_, {}) ? root_ : folder_;
    name_ = u8(p.filename());
    build_bar();
  }

  // The rest of the tab, one step per turn of the message loop (each native control costs
  // 0.5-2 ms, each list several; built at once with the bar they were one 50-80 ms step): the
  // page goes into the notebook (add_page), the Address / Port / Delay fields (then start()
  // when start), the routes list, the log list, the splitter, then done() (the selection).
  void build_body(std::function<void()> add_page, bool start, std::function<void()> done) {
    auto steps = std::make_shared<std::vector<std::pair<const char*, std::function<void()>>>>();
    steps->emplace_back("add_page", [this, add_page = std::move(add_page)] {
      add_page();
      adding_ = false;
    });
    steps->emplace_back("fields", [this, start] {
      build_fields();
      if (start) this->start();
    });
    steps->emplace_back("routes", [this] { build_routes(); });
    steps->emplace_back("log", [this] { build_log(); });
    steps->emplace_back("split", [this] {
      split_->SplitHorizontally(routes_pane_, log_pane_, FromDIP(190));
      GetSizer()->Add(split_, 1, wxEXPAND | wxALL, FromDIP(10));
      split_->Show();
      show();
    });
    if (done) steps->emplace_back("select", std::move(done));
    run_step(std::move(steps), 0);
  }

  ~MockFrame() override {
    link_->frame = nullptr;
    open_mocks().erase(key_);
    clear_var();
    // stop() joins the listener's threads: off the UI thread.
    std::thread([s = std::move(server_)] { s->stop(); }).detach();
  }

  bool adding() const { return adding_; }  // not in the notebook yet (build_body's first step)
  wxString tab_title() const { return U("Mock: " + name_); }
  const fs::path& folder() const { return folder_; }
  bool running() const { return running_; }
  // The port to keep for a restart: the one it listens on, else the Port field (0: any).
  unsigned port() const {
    if (!port_) return pending_port_;  // the fields come a turn after the tab (build_body)
    unsigned long p = 0;
    if (!port_->GetValue().Trim().ToULong(&p) || p > 65535) p = 0;
    return static_cast<unsigned>(p);
  }
  void set_port(unsigned p) {
    if (port_) port_->ChangeValue(wxString::Format("%u", p));
    else pending_port_ = p;
  }
  // Only a loopback mock comes back running after a restart (any other address asked first).
  bool loopback() const {
    if (!addr_) return true;  // not built yet: the default, 127.0.0.1
    std::string a = addr_->GetValue().Trim().Trim(false).utf8_string();
    return a.empty() || mock::is_loopback_address(a);
  }
  // Save as example: read the routes again (after the current start / reload when busy).
  void examples_changed() {
    if (busy_) reload_pending_ = true;
    else if (running_) reload();
  }

  void start() {
    Workspace* ws = workspace_ ? workspace_() : nullptr;
    if (!ws || busy_ || !addr_) return;
    std::string addr = addr_->GetValue().Trim().Trim(false).utf8_string();
    if (addr.empty()) addr = "127.0.0.1";
    unsigned long port = 0;
    if (!port_->GetValue().Trim().IsEmpty() && (!port_->GetValue().Trim().ToULong(&port) || port > 65535)) {
      set_state("Port must be 0-65535", true);
      return;
    }
    unsigned long delay = 0;
    if (!delay_->GetValue().Trim().IsEmpty() && (!delay_->GetValue().Trim().ToULong(&delay) || delay > 60000)) {
      set_state("Delay must be 0-60000 ms", true);
      return;
    }
    // Any other address than loopback opens the port to the network: ask, and keep the answer
    // per user (core/mock_server.h); the check itself reads that file on the worker.
    bool allow = false;
    if (!mock::is_loopback_address(addr)) {
      const int a = wxMessageBox(U("Bind the mock server to " + addr +
                                   "?\n\nOther computers on your network will be able to reach it and read every "
                                   "saved example it serves. watt remembers this address for your user account."),
                                 "Expose mock server", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);
      if (a != wxYES) return;
      allow = true;
    }
    busy_ = true;
    set_state("Starting\xE2\x80\xA6", false);
    Laps laps("mock");
    auto copy = std::make_shared<Workspace>(*ws);
    laps.lap("ws_copy");
    run_worker([copy, folder = folder_, srv = server_, link = link_, addr, port, delay, allow] {
      std::vector<std::string> errors;
      auto routes = collect_mock_routes(*copy, folder, &errors);
      for (auto& r : routes)
        for (auto& e : r.examples)
          if (!e.delay_ms) e.delay_ms = static_cast<uint32_t>(delay);
      std::string err;
      bool ok = true;
      if (allow && !mock::allow_bind(addr, &err)) ok = false;
      if (ok) {
        mock::MockServer::Options o;
        o.address = addr;
        o.port = static_cast<uint16_t>(port);
        o.on_request = [link](const mock::LogEntry& e) {
          std::lock_guard lk(link->m);
          if (link->pending.size() < kMaxLogRows) link->pending.push_back(e);
          if (link->posted) return;
          link->posted = true;
          if (wxTheApp) wxTheApp->CallAfter([link] {
              if (link->frame) link->frame->drain();
              else {
                std::lock_guard lk2(link->m);
                link->pending.clear();
                link->posted = false;
              }
            });
        };
        ok = srv->start(std::move(o), std::move(routes), &err);
      }
      const std::string base = ok ? srv->base_url() : std::string();
      if (wxTheApp) wxTheApp->CallAfter([link, ok, err, base, errors] {
          if (link->frame) link->frame->started(ok, err, base, errors);
        });
    });
  }

  void stop() {
    if (busy_ || !running_) return;
    busy_ = true;
    clear_var();
    set_state("Stopping\xE2\x80\xA6", false);
    run_worker([srv = server_, link = link_] {
      srv->stop();
      if (wxTheApp) wxTheApp->CallAfter([link] {
          if (!link->frame) return;
          link->frame->busy_ = false;
          link->frame->running_ = false;
          link->frame->base_.clear();
          link->frame->show();
        });
    });
  }

  void reload() {
    Workspace* ws = workspace_ ? workspace_() : nullptr;
    if (!ws || busy_) return;
    if (!running_) {
      start();
      return;
    }
    busy_ = true;
    set_state("Reloading routes\xE2\x80\xA6", false);
    auto copy = std::make_shared<Workspace>(*ws);
    unsigned long delay = 0;
    delay_->GetValue().Trim().ToULong(&delay);
    delay = std::min<unsigned long>(delay, 60000);
    run_worker([copy, folder = folder_, srv = server_, link = link_, delay] {
      std::vector<std::string> errors;
      auto routes = collect_mock_routes(*copy, folder, &errors);
      for (auto& r : routes)
        for (auto& e : r.examples)
          if (!e.delay_ms) e.delay_ms = static_cast<uint32_t>(delay);
      srv->set_routes(std::move(routes));
      if (wxTheApp) wxTheApp->CallAfter([link, errors] {
          if (!link->frame) return;
          link->frame->busy_ = false;
          link->frame->errors_ = errors;
          link->frame->show();
        });
    });
  }

  void drain() {
    std::vector<mock::LogEntry> got;
    {
      std::lock_guard lk(link_->m);
      got.swap(link_->pending);
      link_->posted = false;
    }
    if (got.empty()) return;
    const long count = log_ ? log_->GetItemCount() : 0;
    const bool at_end = count == 0 || log_->GetTopItem() + log_->GetCountPerPage() >= count;
    for (auto& e : got) rows_.push_back(std::move(e));
    while (rows_.size() > kMaxLogRows) rows_.pop_front();
    if (!log_) return;  // the log list is not built yet: it shows rows_ when it is
    log_->SetItemCount(static_cast<long>(rows_.size()));
    if (at_end) log_->EnsureVisible(static_cast<long>(rows_.size()) - 1);
    log_->Refresh(false);
    log_title_->SetLabel(wxString::Format("Request log  (%zu)", rows_.size()));
  }

 private:
  void run_step(std::shared_ptr<std::vector<std::pair<const char*, std::function<void()>>>> steps, size_t i) {
    if (i >= steps->size()) return;
    wxWeakRef<wxWindow> self(this);
    next_turn([this, self, steps = std::move(steps), i]() mutable {
      if (!self) return;
      Laps laps("mock");
      (*steps)[i].second();
      laps.lap((*steps)[i].first);
      run_step(std::move(steps), i + 1);
    });
  }

  template <class F>
  void run_worker(F&& f) {
    std::thread(std::forward<F>(f)).detach();
  }

  void started(bool ok, const std::string& err, const std::string& base, const std::vector<std::string>& errors) {
    busy_ = false;
    errors_ = errors;
    running_ = ok;
    base_ = base;
    if (!ok) {
      show();
      set_state("Not running: " + err, true);
      return;
    }
    // The port it really listens on (0 picks one): shown, and kept for the next Start / restart.
    if (const size_t c = base_.rfind(':'); c != std::string::npos && c + 1 < base_.size())
      port_->ChangeValue(U(base_.substr(c + 1)));
    Laps laps("mock");
    show();
    laps.lap("started_show");
    // {{mock}} recolours every editor (MainFrame::vars_changed): a turn of its own.
    wxWeakRef<wxWindow> self(this);
    next_turn([this, self] {
      if (!self || !running_) return;
      Laps laps2("mock");
      set_var();
      laps2.lap("set_var");
    });
  }

  // {{mock}}: the newest running mock server's base URL (runtime variables are never saved).
  void set_var() {
    Workspace* ws = workspace_ ? workspace_() : nullptr;
    if (!ws || base_.empty() || ws->root() != root_) return;
    ws->runtime_vars["mock"] = base_;
    var_set_ = true;
    if (vars_changed_) vars_changed_();
  }
  void clear_var() {
    if (!var_set_) return;
    var_set_ = false;
    // While the main window is being destroyed its workspace may already be gone.
    wxWindow* top = wxGetTopLevelParent(this);
    if (!top || top->IsBeingDeleted()) return;
    Workspace* ws = workspace_ ? workspace_() : nullptr;
    if (!ws || ws->root() != root_) return;
    auto it = ws->runtime_vars.find("mock");
    if (it != ws->runtime_vars.end() && it->second == base_) {
      ws->runtime_vars.erase(it);
      if (vars_changed_) vars_changed_();
    }
  }

  void set_state(const std::string& text, bool error) {
    const auto& th = rv::Theme::get();
    state_->SetForegroundColour(error ? th.err : th.dim);
    state_->SetLabel(U(text));
    Layout();
  }

  void build_bar() {
    const auto& th = rv::Theme::get();
    SetBackgroundColour(th.bg);
    SetForegroundColour(th.fg);
    auto* top = new wxBoxSizer(wxVERTICAL);
    const int gap = FromDIP(6);

    auto* bar = new wxBoxSizer(wxHORIZONTAL);
    auto* title = new wxStaticText(this, wxID_ANY, U("Mock server \xC2\xB7 " + name_));
    title->SetFont(GetFont().Bold().Scaled(1.15f));
    bar->Add(title, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(14));
    dot_ = new wxStaticText(this, wxID_ANY, wxString::FromUTF8("\xE2\x97\x8F"));
    bar->Add(dot_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    status_ = new wxStaticText(this, wxID_ANY, "Stopped");
    status_->SetFont(GetFont().Bold());
    bar->Add(status_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    url_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(250), -1), wxTE_READONLY);
    bar->Add(url_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    using http_editor::FlatButton;
    copy_ = new FlatButton(this, "Copy URL", FlatButton::Style::Outline);
    copy_->on_click = [this] {
      if (base_.empty() || !wxTheClipboard->Open()) return;
      wxTheClipboard->SetData(new wxTextDataObject(U(base_)));
      wxTheClipboard->Close();
      set_state("Copied " + base_, false);
    };
    bar->Add(copy_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    toggle_ = new FlatButton(this, "Start", FlatButton::Style::Primary);
    toggle_->on_click = [this] { running_ ? stop() : start(); };
    bar->Add(toggle_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    reload_ = new FlatButton(this, "Reload routes", FlatButton::Style::Outline);
    reload_->SetToolTip("Read the requests and their saved examples again (Save as example reloads by itself)");
    reload_->on_click = [this] { reload(); };
    bar->Add(reload_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(14));
    bar_ = bar;
    top->Add(bar, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    hint_ = new wxStaticText(this, wxID_ANY, "");
    hint_->SetForegroundColour(th.dim);
    top->Add(hint_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    state_ = new wxStaticText(this, wxID_ANY, "");
    state_->SetForegroundColour(th.dim);
    top->Add(state_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(4));
    SetSizer(top);
    show();
  }

  // Address, Port, Delay: the bar's right end (build_body's first step).
  void build_fields() {
    const int gap = FromDIP(6);
    auto label = [this](const char* t) {
      auto* s = new wxStaticText(this, wxID_ANY, t);
      s->SetForegroundColour(rv::Theme::get().dim);
      return s;
    };
    bar_->Add(label("Address"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    addr_ = new wxTextCtrl(this, wxID_ANY, "127.0.0.1", wxDefaultPosition, wxSize(FromDIP(96), -1));
    addr_->SetToolTip("127.0.0.1 only by default; any other address asks first (stored per user)");
    bar_->Add(addr_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    bar_->Add(label("Port"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    port_ = new wxTextCtrl(this, wxID_ANY, wxString::Format("%u", pending_port_), wxDefaultPosition, wxSize(FromDIP(54), -1));
    port_->SetToolTip("0 picks a free port");
    bar_->Add(port_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    bar_->Add(label("Delay"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    delay_ = new wxTextCtrl(this, wxID_ANY, "0", wxDefaultPosition, wxSize(FromDIP(54), -1));
    delay_->SetToolTip("Milliseconds before each response (?__delay=ms or X-Mock-Delay: ms per request)");
    bar_->Add(delay_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    bar_->Add(label("ms"), 0, wxALIGN_CENTER_VERTICAL);
    show();
  }

  wxPanel* pane(wxWindow* parent, wxStaticText** title_out, const wxString& text) {
    const auto& th = rv::Theme::get();
    auto* p = new wxPanel(parent);
    p->SetBackgroundColour(th.bg);
    auto* s = new wxBoxSizer(wxVERTICAL);
    auto* t = new wxStaticText(p, wxID_ANY, text);
    t->SetFont(p->GetFont().Bold());
    s->Add(t, 0, wxLEFT | wxTOP | wxBOTTOM, FromDIP(4));
    *title_out = t;
    p->SetSizer(s);
    return p;
  }

  void build_routes() {
    auto* split = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSP_LIVE_UPDATE | wxSP_THIN_SASH);
    split_ = split;
    split->SetBackgroundColour(rv::Theme::get().bg);
    split->SetMinimumPaneSize(FromDIP(60));
    split->SetSashGravity(0.0);
    auto* rp = pane(split, &routes_title_, "Routes");
    routes_pane_ = rp;
    routes_ = new wxListCtrl(rp, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_NONE);
    routes_->AppendColumn("Method", wxLIST_FORMAT_LEFT, FromDIP(64));
    routes_->AppendColumn("Path", wxLIST_FORMAT_LEFT, FromDIP(330));
    routes_->AppendColumn("Examples", wxLIST_FORMAT_LEFT, FromDIP(360));
    routes_->AppendColumn("Request", wxLIST_FORMAT_LEFT, FromDIP(300));
    routes_->SetToolTip("Double-click: copy the route's mock URL");
    routes_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent& e) {
      auto rt = server_->routes();
      const long i = e.GetIndex();
      if (base_.empty() || !rt || i < 0 || static_cast<size_t>(i) >= rt->size() || !wxTheClipboard->Open()) return;
      const std::string u = base_ + (*rt)[static_cast<size_t>(i)].path;
      wxTheClipboard->SetData(new wxTextDataObject(U(u)));
      wxTheClipboard->Close();
      set_state("Copied " + u, false);
    });
    rp->GetSizer()->Add(routes_, 1, wxEXPAND);
    split->Hide();  // shown with both panes (build_body's last step)
  }

  void build_log() {
    auto* lp = pane(split_, &log_title_, wxString::Format("Request log  (%zu)", rows_.size()));
    log_pane_ = lp;
    log_ = new LogList(lp, &rows_);
    log_->SetItemCount(static_cast<long>(rows_.size()));
    lp->GetSizer()->Add(log_, 1, wxEXPAND);
    auto* clear = new http_editor::FlatButton(lp, "Clear log", http_editor::FlatButton::Style::Outline);
    clear->on_click = [this] {
      rows_.clear();
      log_->SetItemCount(0);
      log_->Refresh();
      log_title_->SetLabel("Request log  (0)");
    };
    lp->GetSizer()->Add(clear, 0, wxALIGN_RIGHT | wxTOP, FromDIP(4));
  }

  void show() {
    const auto& th = rv::Theme::get();
    dot_->SetForegroundColour(running_ ? th.ok : th.dim);
    status_->SetLabel(running_ ? "Running" : busy_ ? "\xE2\x80\xA6" : "Stopped");
    status_->SetForegroundColour(running_ ? th.ok : th.dim);
    url_->ChangeValue(U(base_));
    toggle_->SetLabel(running_ ? "Stop" : "Start");
    copy_->Enable(running_);
    if (addr_) addr_->Enable(!running_);
    if (port_) port_->Enable(!running_);
    hint_->SetLabel(U(running_ ? "{{mock}} = " + base_ +
                                     "   \xC2\xB7   pick an example with ?__example=<name> or X-Mock-Example: <name>   "
                                     "\xC2\xB7   unmatched paths return 404 with the route list"
                               : "Serves the saved examples (Save as example in the response view) of the HTTP requests in " +
                                     name_ + ". While running, {{mock}} is its base URL."));
    auto rt = server_->routes();
    size_t with = 0;
    if (routes_) {
      routes_->Freeze();
      routes_->DeleteAllItems();
    }
    if (routes_ && running_ && rt) {
      long i = 0;
      for (const auto& r : *rt) {
        routes_->InsertItem(i, U(r.method));
        routes_->SetItem(i, 1, U(r.path));
        std::string ex;
        for (const auto& e : r.examples) {
          if (!ex.empty()) ex += ", ";
          ex += e.name + " (" + std::to_string(e.status) + ")";
          if (ex.size() > 200) break;
        }
        if (r.examples.empty()) {
          ex = "none: answers 501 until an example is saved";
          routes_->SetItemTextColour(i, th.dim);
        } else {
          ++with;
        }
        routes_->SetItem(i, 2, U(ex));
        routes_->SetItem(i, 3, U(r.source));
        ++i;
      }
    }
    if (routes_) {  // built a few turns after the bar (build_body)
      routes_->Thaw();
      routes_title_->SetLabel(rt && running_ ? wxString::Format("Routes  (%zu, %zu with examples)", rt->size(), with)
                                             : wxString("Routes"));
    }
    std::string st;
    if (!errors_.empty()) {
      st = std::to_string(errors_.size()) + " request(s) could not be read: " + errors_.front();
      set_state(st, true);
    } else if (running_) {
      set_state("Listening on its own threads; " + std::to_string(rt ? rt->size() : 0) + " route(s).", false);
    } else if (!busy_) {
      set_state("", false);
    }
    toggle_->Enable(!busy_);
    reload_->Enable(!busy_);
    for (wxWindow* b : {static_cast<wxWindow*>(copy_), static_cast<wxWindow*>(toggle_), static_cast<wxWindow*>(reload_)})
      b->Refresh();
    Layout();
    if (!busy_ && reload_pending_) {  // examples were saved while it was busy
      reload_pending_ = false;
      if (running_) reload();
    }
  }

  std::function<Workspace*()> workspace_;
  std::function<void()> vars_changed_;
  fs::path root_, folder_;
  std::wstring key_;
  std::string name_;
  std::shared_ptr<mock::MockServer> server_;
  std::shared_ptr<Link> link_;
  std::deque<mock::LogEntry> rows_;
  std::vector<std::string> errors_;
  std::string base_;
  bool running_ = false, busy_ = false, var_set_ = false, reload_pending_ = false;
  wxStaticText *dot_ = nullptr, *status_ = nullptr, *hint_ = nullptr, *state_ = nullptr;
  wxStaticText *routes_title_ = nullptr, *log_title_ = nullptr;
  wxTextCtrl *url_ = nullptr, *addr_ = nullptr, *port_ = nullptr, *delay_ = nullptr;
  http_editor::FlatButton *copy_ = nullptr, *toggle_ = nullptr, *reload_ = nullptr;
  wxListCtrl* routes_ = nullptr;
  LogList* log_ = nullptr;
  wxSplitterWindow* split_ = nullptr;
  wxBoxSizer* bar_ = nullptr;
  unsigned pending_port_ = 0;
  bool adding_ = true;
  wxPanel *routes_pane_ = nullptr, *log_pane_ = nullptr;
};

}  // namespace

std::vector<mock::Route> collect_mock_routes(const Workspace& ws, const fs::path& folder, std::vector<std::string>* errors) {
  std::vector<mock::Route> out;
  const auto items = runner::collect(ws, folder, errors);
  for (const auto& it : items) {
    const SavedRequest& r = it.req;
    if (r.kind != RequestKind::Http) continue;
    mock::Route route;
    route.method = r.http.method.empty() ? std::string("GET") : r.http.method;
    for (char& c : route.method) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    route.path = mock::route_path(ws.expand(r.http.url, r));
    route.source = r.name.empty() ? it.rel : r.name;
    std::set<std::string> names;
    for (const auto& e : rv::list_examples(rv::examples_dir(ws.root(), r.id))) {  // newest first
      if (route.examples.size() >= kMaxExamplesPerRoute) break;
      std::string lname = e.name;
      for (char& c : lname) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
      if (!names.insert(lname).second) continue;  // an older example of the same name
      std::string err;
      auto resp = rv::load_example(e.file, nullptr, nullptr, &err);
      if (!resp) {
        if (errors) errors->push_back(it.rel + ": " + err);
        continue;
      }
      mock::Example ex;
      ex.name = e.name;
      ex.status = resp->status;
      for (const auto h : resp->headers) ex.headers.emplace_back(std::string(h.first), std::string(h.second));
      ex.body = std::move(resp->body);
      route.examples.push_back(std::move(ex));
    }
    out.push_back(std::move(route));
  }
  return out;
}

void open_mock(wxWindow* parent, std::function<Workspace*()> workspace, const fs::path& folder,
               std::function<void()> vars_changed, bool start, unsigned port, bool select, int at) {
  Laps laps("mock");
  Workspace* ws = workspace ? workspace() : nullptr;
  if (!ws) return;
  std::function<wxAuiNotebook*(wxWindow*)> find_nb = [&](wxWindow* w) -> wxAuiNotebook* {
    if (auto* nb = dynamic_cast<wxAuiNotebook*>(w)) return nb;
    for (wxWindow* c : w->GetChildren())
      if (auto* nb = find_nb(c)) return nb;
    return nullptr;
  };
  wxAuiNotebook* nb = parent ? find_nb(parent) : nullptr;
  if (!nb) return;
  const std::wstring key = mock_key(ws->root(), folder);
  auto& open = open_mocks();
  if (auto it = open.find(key); it != open.end()) {
    if (const int i = nb->GetPageIndex(it->second); i != wxNOT_FOUND) {
      nb->SetSelection(static_cast<size_t>(i));
      return;
    }
    if (auto* f = dynamic_cast<MockFrame*>(it->second); f && f->adding()) return;  // added on the next turn
    open.erase(it);
  }
  laps.lap("find");
  // One step per turn of the message loop: the page with its bar (title, URL, Start) first, then
  // the tab, the Address / Port fields, the routes list, the log list and the splitter
  // (build_body), then the selection.
  auto* f = new MockFrame(nb, std::move(workspace), *ws, folder, std::move(vars_changed));
  open[key] = f;
  laps.lap("frame");
  if (port) f->set_port(port);
  wxWeakRef<wxAuiNotebook> book(nb);
  wxWeakRef<wxWindow> page(f);
  auto add = [book, f, at] {
    if (!book) return;
    if (at >= 0 && static_cast<size_t>(at) < book->GetPageCount())
      book->InsertPage(static_cast<size_t>(at), f, f->tab_title(), false);
    else
      book->AddPage(f, f->tab_title(), false);
  };
  std::function<void()> sel;  // restored after a restart: the user's tab stays selected
  if (select)
    sel = [book, page] {
      if (!book || !page) return;
      if (const int i = book->GetPageIndex(page.get()); i != wxNOT_FOUND) book->SetSelection(static_cast<size_t>(i));
    };
  f->build_body(std::move(add), start, std::move(sel));
}

bool mock_tab_info(wxWindow* w, fs::path* folder, unsigned* port, bool* running) {
  auto* f = dynamic_cast<MockFrame*>(w);
  if (!f) return false;
  if (folder) *folder = f->folder();
  if (port) *port = f->port();
  if (running) *running = f->running() && f->loopback();
  return true;
}

void mock_examples_changed() {
  for (auto& [key, w] : open_mocks())
    if (auto* f = dynamic_cast<MockFrame*>(w)) f->examples_changed();
}

}  // namespace watt::app
