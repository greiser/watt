#include "app/runner_view.h"

#include <wx/app.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/config.h>
#include <wx/tokenzr.h>
#include <wx/dialog.h>
#include <wx/dcbuffer.h>
#include <wx/filedlg.h>
#include <wx/aui/auibook.h>
#include <wx/frame.h>
#include <wx/listctrl.h>
#include <wx/menu.h>
#include <wx/module.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/vlbox.h>
#include <wx/weakref.h>
#include <wx/wupdlock.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#ifdef small
#undef small  // rpcndr.h: "#define small char"
#endif

#include "app/panels.h"            // SidebarHost
#include <rapidjson/document.h>    // Open request: a gRPC stream's kept messages
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include "app/request_tab.h"       // RequestTab::restore_response (Open request shows the run's response)
#include "app/response_widgets.h"  // rv::Theme, ChipBar
#include "app/http_editor_widgets.h"  // http_editor::Theme: Run uses the Send button's accent
#include "app/runner_detail.h"
#include "app/runner_engine.h"
#include "app/ws_watch.h"          // WorkspaceWatcher: an open runner follows edits on disk
#include "core/timing.h"

namespace watt::app {

namespace {

using runner::RunItem;
using runner::RunSummary;
using runner::StepResult;
using runner_ui::fmt_bytes;
using runner_ui::fmt_ms;
using runner_ui::method_colour;
using runner_ui::mix;
using runner_ui::status_colour;
using runner_ui::status_label;
using StepPtr = std::shared_ptr<const StepResult>;
namespace fs = std::filesystem;

wxString U(const std::string& s) { return wxString::FromUTF8(s); }
std::string u8(const std::filesystem::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}
const char* const kDot = "  \xC2\xB7  ";

// A URL cut to w pixels without losing what tells runs apart: the scheme, host and port stay
// whole, then as much of the end (the query with a data row's values) as fits, the middle of
// the path giving way first: "http://127.0.0.1:18861/…/search?q=ann&n=1".
wxString elide_url(wxDC& dc, const wxString& url, int w) {
  if (w <= 0 || dc.GetTextExtent(url).x <= w) return url;
  const wxString dots = wxString::FromUTF8("\xE2\x80\xA6");
  size_t head_end = url.find("://");
  head_end = head_end == wxString::npos ? 0 : head_end + 3;
  const size_t slash = url.find_first_of("/?#", head_end);
  if (slash == wxString::npos || slash == 0) return wxControl::Ellipsize(url, dc, wxELLIPSIZE_END, w, wxELLIPSIZE_FLAGS_EXPAND_TABS);
  wxString head = url.substr(0, slash);
  int room = w - dc.GetTextExtent(head + dots).x;
  if (room < dc.GetTextExtent("/a").x && head_end > 0) {
    // Narrow: the scheme goes before the port and the path's end do.
    head = url.substr(head_end, slash - head_end);
    room = w - dc.GetTextExtent(head + dots).x;
  }
  if (room < dc.GetTextExtent("/a").x) return wxControl::Ellipsize(url, dc, wxELLIPSIZE_END, w, wxELLIPSIZE_FLAGS_EXPAND_TABS);
  // The longest end of the rest that fits (binary search on its length).
  const wxString rest = url.substr(slash);
  size_t lo = 0, hi = rest.size();
  while (lo < hi) {
    const size_t mid = (lo + hi + 1) / 2;
    if (dc.GetTextExtent(rest.Right(mid)).x <= room) lo = mid;
    else hi = mid - 1;
  }
  return head + dots + rest.Right(lo);
}

// Runs f on a later turn of the message loop (a one-shot timer), after the window messages
// already queued: unlike CallAfter, whose events wx runs back to back in one go, two steps posted
// this way are two UI-thread steps with input and paints answered in between.
void next_turn(std::function<void()> f, int ms = 1) {
  auto* t = new wxTimer();
  t->Bind(wxEVT_TIMER, [t, f = std::move(f)](wxTimerEvent&) {
    f();
    wxTheApp->CallAfter([t] { delete t; });
  });
  t->StartOnce(ms);
}

// wxWidgets 3.3.1: wxAuiTabCtrl::m_hoverButton points into the hovered tab's close-button storage
// and is not reset when that page is removed; the next mouse move then writes into the freed block
// (it corrupted the notebook's layout sizers: AV in ~wxSizer on a later tab close). A leave event
// resets it while it is still valid (MainFrame does the same before its removals).
void forget_tab_hover(wxAuiNotebook* nb) {
  if (!nb) return;
  for (wxAuiTabCtrl* tc : nb->GetAllTabCtrls()) {
    wxMouseEvent leave(wxEVT_LEAVE_WINDOW);
    leave.SetEventObject(tc);
    tc->GetEventHandler()->ProcessEvent(leave);
  }
}

// Characters (UTF-8 code points) in s: the width of a monospace cell, "über" is 4 wide.
size_t u8_width(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s) n += (c & 0xC0) != 0x80;
  return n;
}

// The first n characters of s (never a split UTF-8 sequence).
std::string u8_head(const std::string& s, size_t n) {
  size_t i = 0;
  for (size_t k = 0; i < s.size(); ++i) {
    if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80 && k++ == n) break;
  }
  return s.substr(0, i);
}

// "2026-11-17T15:42:51Z" (UTC) as local time, "2026-11-17 16:42:51" (date_too false: "16:42:51").
std::string iso_local(const std::string& iso, bool date_too = true) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
  if (sscanf_s(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return iso;
  std::tm t{};
  t.tm_year = y - 1900;
  t.tm_mon = mo - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min = mi;
  t.tm_sec = se;
  const std::time_t tt = _mkgmtime(&t);
  std::tm lt{};
  if (tt == static_cast<std::time_t>(-1) || localtime_s(&lt, &tt) != 0) return iso;
  char b[32];
  std::strftime(b, sizeof b, date_too ? "%Y-%m-%d %H:%M:%S" : "%H:%M:%S", &lt);
  return b;
}

// ------------------------------------------------------------------ shared run state

class RunnerFrame;

// Lives as long as any worker thread or queued CallAfter needs it. frame is set and cleared on
// the UI thread only, and only read there.
struct Link {
  RunnerFrame* frame = nullptr;
  std::atomic<int> saving{0};  // run-order.json writes in flight (a reload reads the file after them)
};

struct RunState {
  runner::RunControl ctl;
  std::mutex m;
  std::vector<StepPtr> pending;  // finished steps not drained by the UI yet
};

// A finished run, kept for this process (History, comparisons) after its window closes.
struct RunRecord {
  std::string started_iso;
  std::string environment;
  std::string label;  // "rerun failed", "data: users.csv"
  int iterations = 1;
  size_t planned = 0;
  RunSummary summary;  // with per_request
  std::shared_ptr<const std::vector<StepPtr>> steps;
  std::shared_ptr<const std::vector<RunItem>> items;
  std::shared_ptr<const std::vector<size_t>> order;  // the run's order (indexes into items)
  // What was run ("9 requests x 300 iterations, 8 parallel"): By request's change column
  // compares only runs with the same plan.
  std::string plan;
};

std::map<std::wstring, std::deque<std::shared_ptr<const RunRecord>>>& run_history() {
  static std::map<std::wstring, std::deque<std::shared_ptr<const RunRecord>>> m;
  return m;
}
// Runners whose results were cleared (Reset) since their last run: reopened, they show none.
std::set<std::wstring>& cleared_runners() {
  static std::set<std::wstring> s;
  return s;
}
constexpr size_t kHistoryRuns = 12;
// Runners whose saved runs (runner::load_history) were read in this process.
std::set<std::wstring>& history_read() {
  static std::set<std::wstring> s;
  return s;
}
constexpr size_t kAllChecksRun = 100;  // runs up to this many sends list passed checks too

// ------------------------------------------------------------------ accent button

class AccentButton : public wxWindow {
 public:
  AccentButton(wxWindow* parent, const wxString& label)
      : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxWANTS_CHARS), label_(label) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetCursor(wxCursor(wxCURSOR_HAND));
    SetFont(GetFont().Bold().Scaled(1.05f));
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
    Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { hover_ = true; Refresh(); });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { hover_ = false; Refresh(); });
    Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) { e.Skip(); Refresh(); });
    Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) { e.Skip(); Refresh(); });
    Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { fire(); });
    Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
      if ((e.GetKeyCode() == WXK_SPACE || e.GetKeyCode() == WXK_RETURN) && e.GetModifiers() == 0) return fire();
      if (on_key && on_key(e)) return;
      e.Skip();
    });
  }
  std::function<bool(wxKeyEvent&)> on_key;  // window-wide keys (wxWANTS_CHARS keeps them from the char hook)
  void set(const wxString& label, bool danger) {
    label_ = label;
    danger_ = danger;
    Refresh();
  }
  std::function<void()> on_click;
  wxSize DoGetBestClientSize() const override { return FromDIP(wxSize(120, 34)); }
  bool AcceptsFocus() const override { return true; }

 private:
  void fire() {
    if (IsEnabled() && on_click) on_click();
  }
  void paint() {
    wxAutoBufferedPaintDC dc(this);
    const auto& th = rv::Theme::get();
    dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
    dc.Clear();
    // The primary action's color everywhere (the editors' Send / Connect), not the view accent.
    wxColour c = danger_ ? th.err : http_editor::Theme::get().accent;
    if (!IsEnabled()) c = mix(c, th.bg, 0.55);
    else if (hover_) c = mix(c, *wxWHITE, 0.12);
    dc.SetPen(HasFocus() ? wxPen(mix(c, *wxWHITE, 0.6), FromDIP(2)) : *wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(c));
    wxRect r = GetClientRect();
    if (HasFocus()) r.Deflate(FromDIP(1));
    dc.DrawRoundedRectangle(r, FromDIP(5));
    dc.SetFont(GetFont());
    dc.SetTextForeground(*wxWHITE);
    const wxSize ts = dc.GetTextExtent(label_);
    const wxSize cs = GetClientSize();
    dc.DrawText(label_, (cs.x - ts.x) / 2, (cs.y - ts.y) / 2);
  }
  wxString label_;
  bool hover_ = false;
  bool danger_ = false;
};

// ------------------------------------------------------------------ number field

// A whole number in [lo, hi]: an edit box (a native spin control costs ~4 ms to create, three
// of them a sixth of the open budget). Up / Down (PgUp / PgDn: 10) and the mouse wheel step it;
// a value out of range is clamped when read and when the field loses focus.
class NumberField : public wxTextCtrl {
 public:
  NumberField(wxWindow* parent, int initial, int lo, int hi, const wxSize& size)
      : wxTextCtrl(parent, wxID_ANY, wxString::Format("%d", initial), wxDefaultPosition, size, wxTE_RIGHT | wxBORDER_THEME),
        lo_(lo), hi_(hi) {
    Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
      const int k = e.GetKeyCode();
      if (e.GetModifiers() == 0 && (k == WXK_UP || k == WXK_DOWN || k == WXK_PAGEUP || k == WXK_PAGEDOWN)) {
        const int step = k == WXK_PAGEUP || k == WXK_PAGEDOWN ? 10 : 1;
        bump(k == WXK_UP || k == WXK_PAGEUP ? step : -step);
        return;
      }
      e.Skip();
    });
    Bind(wxEVT_CHAR, [](wxKeyEvent& e) {
      const int c = e.GetUnicodeKey();
      if (c != WXK_NONE && c >= 32 && !(c >= '0' && c <= '9') && !e.HasAnyModifiers()) return;  // digits only
      e.Skip();
    });
    Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& e) {
      if (!HasFocus()) return e.Skip();
      bump(e.GetWheelRotation() > 0 ? 1 : -1);
    });
    Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) {
      e.Skip();
      const int v = value();
      if (wxTextCtrl::GetValue() != wxString::Format("%d", v)) ChangeValue(wxString::Format("%d", v));
    });
  }
  int value() const {
    long v = 0;
    if (!wxTextCtrl::GetValue().ToLong(&v)) return lo_;
    return static_cast<int>(std::clamp<long>(v, lo_, hi_));
  }
  // Sets the text without a text event.
  void set_value(int v) { ChangeValue(wxString::Format("%d", std::clamp(v, lo_, hi_))); }

 private:
  void bump(int d) {
    const long v = std::clamp<long>(static_cast<long>(value()) + d, lo_, hi_);
    wxTextCtrl::SetValue(wxString::Format("%ld", v));  // a text event: the plan line follows
    SetInsertionPointEnd();
  }
  int lo_, hi_;
};

// ------------------------------------------------------------------ consent dialog

// A three-way question in the runner's own theme (a native message box stays light inside the
// dark runner). Returns wxID_YES, wxID_NO or wxID_CANCEL (Esc, the close box). yes_default:
// Enter answers yes, and yes is drawn as the primary button; otherwise Enter answers no.
int ask_three(wxWindow* parent, const wxString& title, const wxString& heading, const wxString& msg,
              const wxString& yes, const wxString& no, const wxString& cancel, bool yes_default) {
  const auto& th = rv::Theme::get();
  wxDialog dlg(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX);
  dlg.SetBackgroundColour(mix(th.bg, th.fg, 0.04));
  dlg.SetForegroundColour(th.fg);
  auto* sz = new wxBoxSizer(wxVERTICAL);
  auto* h = new wxStaticText(&dlg, wxID_ANY, heading);
  h->SetFont(dlg.GetFont().Bold().Scaled(1.15f));
  h->SetForegroundColour(th.fg);
  sz->Add(h, 0, wxLEFT | wxRIGHT | wxTOP, dlg.FromDIP(20));
  auto* t = new wxStaticText(&dlg, wxID_ANY, msg);
  t->SetForegroundColour(th.fg);
  t->Wrap(dlg.FromDIP(520));
  sz->Add(t, 0, wxALL, dlg.FromDIP(20));
  auto* bs = new wxBoxSizer(wxHORIZONTAL);
  bs->AddStretchSpacer();
  auto button = [&](wxWindowID id, const wxString& label, bool primary) {
    auto* b = new wxButton(&dlg, id, label, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    b->SetBackgroundColour(primary ? th.accent : mix(th.bg, th.fg, 0.14));
    b->SetForegroundColour(primary ? *wxWHITE : th.fg);
    if (primary) b->SetFont(b->GetFont().Bold());
    const wxSize best = b->GetBestSize();
    b->SetMinSize(wxSize(std::max(best.x + dlg.FromDIP(24), dlg.FromDIP(96)), dlg.FromDIP(30)));
    b->Bind(wxEVT_BUTTON, [&dlg, id](wxCommandEvent&) { dlg.EndModal(id); });
    bs->Add(b, 0, wxLEFT, dlg.FromDIP(8));
    return b;
  };
  button(wxID_CANCEL, cancel, false);
  wxButton* n = button(wxID_NO, no, !yes_default);
  wxButton* y = button(wxID_YES, yes, yes_default);
  wxButton* def = yes_default ? y : n;
  def->SetDefault();
  dlg.SetEscapeId(wxID_CANCEL);
  sz->Add(bs, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, dlg.FromDIP(16));
  dlg.SetSizerAndFit(sz);
  dlg.CentreOnParent();
  dlg.CallAfter([def] { def->SetFocus(); });
  return dlg.ShowModal();
}

// ------------------------------------------------------------------ summary strip

class SummaryStrip : public wxWindow {
 public:
  explicit SummaryStrip(wxWindow* parent) : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { e.Skip(); Refresh(); });
  }
  void set(const RunSummary& s, const std::string& state, bool running) {
    s_ = s;
    state_ = state;
    running_ = running;
    Refresh();
  }
  void set_thresholds(std::vector<runner::ThresholdResult> t) {
    thr_ = std::move(t);
    Refresh();
  }
  wxSize DoGetBestClientSize() const override { return FromDIP(wxSize(400, 92)); }

 private:
  void paint() {
    wxAutoBufferedPaintDC dc(this);
    const auto& th = rv::Theme::get();
    dc.SetBackground(wxBrush(th.bg));
    dc.Clear();
    const wxSize cs = GetClientSize();
    const int pad = FromDIP(16);
    const wxFont small = GetFont().Scaled(0.85f);
    const wxFont big = GetFont().Bold().Scaled(1.45f);
    struct Tile {
      std::string label, value;
      wxColour col;
    };
    const bool any = s_.total > s_.stopped_steps;
    const size_t skipped = s_.skipped + s_.stopped_steps;
    std::vector<Tile> tiles = {
        {"PASSED", std::to_string(s_.passed), s_.passed ? th.ok : th.dim},
        {"FAILED", std::to_string(s_.failed), s_.failed ? th.err : th.dim},
    };
    if (skipped) tiles.push_back({"SKIPPED", std::to_string(skipped), th.warn});
    tiles.push_back({"REQUESTS", std::to_string(s_.total) + (s_.planned ? " / " + std::to_string(s_.planned) : ""), th.fg});
    tiles.push_back({"DURATION", any || running_ ? fmt_ms(s_.wall_ms) : "-", th.fg});
    // A spread and a rate need two samples: one send shows "-" there, not "0.00 ms" / "729/s".
    const bool spread = any && s_.samples > 1;
    tiles.push_back({"AVG", any ? fmt_ms(s_.avg_ms) : "-", th.fg});
    tiles.push_back({"P50", any ? fmt_ms(s_.p50_ms) : "-", th.fg});
    tiles.push_back({"P90", spread ? fmt_ms(s_.p90_ms) : "-", th.fg});
    tiles.push_back({"P95", spread ? fmt_ms(s_.p95_ms) : "-", th.fg});
    tiles.push_back({"P99", spread ? fmt_ms(s_.p99_ms) : "-", th.fg});
    tiles.push_back({"MIN", any ? fmt_ms(s_.min_ms) : "-", th.fg});
    tiles.push_back({"MAX", any ? fmt_ms(s_.max_ms) : "-", th.fg});
    tiles.push_back({"STD DEV", spread ? fmt_ms(s_.stddev_ms) : "-", th.fg});
    // The times above are response times (Newman's): scripts and token requests are apart.
    if (s_.scripted) tiles.push_back({"SCRIPTS AVG", fmt_ms(s_.avg_script_ms), th.dim});
    {
      // Throughput over the wall time (load mode reads it first).
      const size_t sent = s_.total - s_.stopped_steps;
      char b[32] = "-";
      if (spread && s_.wall_ms > 0) std::snprintf(b, sizeof b, sent * 1000.0 / s_.wall_ms >= 100 ? "%.0f/s" : "%.1f/s", sent * 1000.0 / s_.wall_ms);
      tiles.push_back({"THROUGHPUT", b, th.fg});
    }
    tiles.push_back({"ASSERTS", std::to_string(s_.asserts_passed) + " / " + std::to_string(s_.asserts_passed + s_.asserts_failed),
                     s_.asserts_failed ? th.err : (s_.asserts_passed ? th.ok : th.dim)});
    tiles.push_back({"RECEIVED", any ? fmt_bytes(s_.bytes_received) : "-", th.fg});
    dc.SetFont(GetFont().Bold());
    const wxSize sz = dc.GetTextExtent(U(state_));
    int limit = cs.x - pad - sz.x - FromDIP(16);
    const int y = FromDIP(8);
    bool state_below = false;  // the state moved to the second line's right end (no room above)
    // Every tile fits: the gaps shrink first, then the values' font; only on a very narrow
    // window are the least read tiles (STD DEV, MIN, MAX) left out, never the last ones.
    wxFont vfont = big;
    auto widths = [&](const wxFont& f) {
      std::vector<int> w;
      for (const auto& t : tiles) {
        dc.SetFont(small);
        const int lw = dc.GetTextExtent(U(t.label)).x;
        dc.SetFont(f);
        w.push_back(std::max({lw, dc.GetTextExtent(U(t.value)).x, FromDIP(40)}));
      }
      return w;
    };
    auto sum = [](const std::vector<int>& w) {
      int n = 0;
      for (int v : w) n += v;
      return n;
    };
    int avail = limit - pad;
    std::vector<int> w = widths(vfont);
    int gap = FromDIP(22);
    const int min_gap = FromDIP(8);
    auto fits = [&] { return w.empty() || sum(w) + gap * static_cast<int>(w.size() - 1) <= avail; };
    auto regap = [&] {
      if (w.size() > 1) gap = std::clamp((avail - sum(w)) / static_cast<int>(w.size() - 1), min_gap, FromDIP(22));
    };
    regap();
    // The same tiles in every run (a load run's "1500 / 1500" is wider than a 9-send run's
    // "9 / 9"): the state moves under the tiles, "a / b" values lose their spaces and the values'
    // font gets smaller before any tile is left out.
    for (int step = 0; step < 6 && !fits(); ++step) {
      if (step == 2 && !state_below) {
        state_below = true;
        limit = cs.x - pad;
        avail = limit - pad;
      } else if (step == 3) {
        for (auto& t : tiles)
          if (const size_t at = t.value.find(" / "); at != std::string::npos) t.value.replace(at, 3, "/");
      } else {
        static const float ks[] = {1.3f, 1.15f, 1.15f, 1.15f, 1.0f, 0.92f};
        vfont = GetFont().Bold().Scaled(ks[step]);
      }
      w = widths(vfont);
      regap();
    }
    for (const char* drop : {"SCRIPTS AVG", "STD DEV", "P90", "P99", "RECEIVED", "THROUGHPUT", "P95", "DURATION"}) {
      if (fits()) break;
      for (size_t i = 0; i < tiles.size(); ++i)
        if (tiles[i].label == drop) {
          tiles.erase(tiles.begin() + static_cast<std::ptrdiff_t>(i));
          w.erase(w.begin() + static_cast<std::ptrdiff_t>(i));
          break;
        }
    }
    int x = pad;
    for (size_t i = 0; i < tiles.size(); ++i) {
      const auto& t = tiles[i];
      if (x + w[i] > limit) break;  // only on a window too narrow for the core tiles
      dc.SetFont(small);
      dc.SetTextForeground(th.dim);
      dc.DrawText(U(t.label), x, y);
      dc.SetFont(vfont);
      dc.SetTextForeground(t.col);
      dc.DrawText(U(t.value), x, y + FromDIP(15));
      x += w[i] + gap;
    }
    // State on the right ("Running · iteration 2 of 5", "Stopped", "Done").
    dc.SetFont(GetFont().Bold());
    dc.SetTextForeground(running_ ? th.accent : (s_.stopped ? th.warn : th.dim));
    dc.DrawText(U(state_), cs.x - pad - sz.x, y + FromDIP(state_below ? 47 : 18));
    const int line_end = state_below ? cs.x - pad - sz.x - FromDIP(16) : cs.x - pad;  // the second line's room
    // Second line: where the time goes on average.
    if (any) {
      std::string l = "avg  TTFB " + fmt_ms(s_.avg_ttfb_ms);
      if (s_.new_connections) {
        l += kDot + std::string("DNS ") + fmt_ms(s_.avg_dns_ms) + kDot + "connect " + fmt_ms(s_.avg_connect_ms);
        if (s_.avg_tls_ms > 0) l += kDot + std::string("TLS ") + fmt_ms(s_.avg_tls_ms);
        l += "  (" + std::to_string(s_.new_connections) + " new connection" + (s_.new_connections == 1 ? ")" : "s)");
      } else {
        l += kDot + std::string("every connection reused");
      }
      // watt's own setup before the first I/O: not in any time above (Newman's "prepare").
      if (s_.avg_prep_ms > 0) l += kDot + std::string("setup ") + fmt_ms(s_.avg_prep_ms) + " (not in the times)";
      if (skipped) {
        // The same number as the SKIPPED tile and the Skipped filter, with its parts.
        l += kDot + std::to_string(skipped) + " skipped";
        if (s_.stopped_steps && s_.skipped)
          l += " (" + std::to_string(s_.stopped_steps) + " stopped, " + std::to_string(s_.skipped) + " not run)";
        else if (s_.stopped_steps)
          l += " (stopped in flight)";
        else
          l += " (not run)";
      }
      if (s_.errors) l += kDot + std::to_string(s_.errors) + " not sent / transport error" + (s_.errors == 1 ? "" : "s");
      dc.SetFont(small);
      dc.SetTextForeground(th.dim);
      dc.DrawText(wxControl::Ellipsize(U(l), dc, wxELLIPSIZE_END, std::max(FromDIP(60), line_end - pad), wxELLIPSIZE_FLAGS_EXPAND_TABS), pad, y + FromDIP(48));
      // Thresholds (Postman's performance pass / fail criteria), each green or red with its actual.
      if (!thr_.empty()) {
        int tx = pad + dc.GetTextExtent(U(l)).x + FromDIP(18);
        auto put = [&](const std::string& t, const wxColour& c, const wxFont& f) {
          dc.SetFont(f);
          dc.SetTextForeground(c);
          dc.DrawText(U(t), tx, y + FromDIP(48));
          tx += dc.GetTextExtent(U(t)).x;
        };
        bool all = true;
        for (const auto& t : thr_) all = all && t.pass;
        put(all ? "THRESHOLDS MET  " : "THRESHOLDS MISSED  ", all ? th.ok : th.err, small.Bold());
        for (const auto& t : thr_) {
          if (tx > line_end - FromDIP(60)) break;
          put((t.pass ? "\xE2\x9C\x93 " : "\xE2\x9C\x97 ") + t.t.text, t.pass ? th.ok : th.err, small);
          put(" (" + t.actual_text + ")   ", th.dim, small);
        }
      }
    }
    // Progress bar along the bottom: passed green, failed red, stopped / skipped amber.
    const int bh = FromDIP(4);
    const int by = cs.y - bh - FromDIP(4);
    const int bw = cs.x - 2 * pad;
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(th.grid));
    dc.DrawRoundedRectangle(pad, by, bw, bh, bh / 2.0);
    if (s_.planned) {
      const double k = static_cast<double>(bw) / static_cast<double>(s_.planned);
      int at = pad;
      for (const auto& [n, c] : {std::pair<size_t, wxColour>{s_.passed, th.ok}, {s_.failed, th.err}, {skipped, th.warn}}) {
        const int seg = static_cast<int>(std::lround(k * static_cast<double>(n)));
        if (!seg) continue;
        dc.SetBrush(wxBrush(c));
        dc.DrawRectangle(at, by, seg, bh);
        at += seg;
      }
    }
  }
  RunSummary s_;
  std::vector<runner::ThresholdResult> thr_;
  std::string state_ = "Ready";
  bool running_ = false;
};

// ------------------------------------------------------------------ results table

// Owner-drawn (wxVListBox): per-column colors, which a dark-mode wxListCtrl cannot do (it
// paints a whole row with one attribute), and only the visible rows are ever drawn. Columns
// are resized by dragging their edge in the header and sorted by clicking their title.
constexpr int kCols = 8;
const char* const kColNames[kCols] = {"#", "Iter", "Request", "Status", "Time", "Script", "Size", "Asserts"};
constexpr bool kColRight[kCols] = {true, true, false, false, true, true, true, false};
constexpr int kFlex = 2;

struct ColModel {
  int w[kCols] = {52, 44, 0, 136, 90, 78, 86, 110};  // DIP; w[kFlex] unused
  // Some step had script time: the Script column shows (otherwise it takes no room).
  bool scripts = false;
  int sort = -1;                                 // column, -1: run order
  bool desc = false;
  struct X {
    int x[kCols + 1];
  };
  // On a narrow pane the columns give way so Asserts (pass / fail counts) stays in view: Status
  // narrows, then the request column, then Size is left out, then Time and the numbers narrow.
  X layout(const wxWindow* win, int width) const {
    int ww[kCols];
    int sum = 0;
    for (int i = 0; i < kCols; ++i) {
      ww[i] = i == kFlex || (i == 5 && !scripts) ? 0 : win->FromDIP(w[i]);
      sum += ww[i];
    }
    int flex_min = win->FromDIP(160);
    int over = sum + flex_min - width;
    auto give = [&](int& v, int floor) {
      if (over <= 0 || v <= floor) return;
      const int cut = std::min(over, v - floor);
      v -= cut;
      over -= cut;
    };
    give(ww[3], win->FromDIP(96));
    give(flex_min, win->FromDIP(120));
    if (over > 0) {
      over -= ww[6];
      ww[6] = 0;
    }
    give(ww[5], win->FromDIP(56));
    give(ww[4], win->FromDIP(64));
    give(ww[0], win->FromDIP(36));
    give(ww[1], win->FromDIP(32));
    give(ww[7], win->FromDIP(76));
    sum = 0;
    for (int i = 0; i < kCols; ++i) sum += ww[i];
    const int flex = std::max(flex_min, width - sum);
    X c;
    int x = 0;
    for (int i = 0; i < kCols; ++i) {
      c.x[i] = x;
      x += i == kFlex ? flex : ww[i];
    }
    c.x[kCols] = x;
    return c;
  }
};

class GridHeader : public wxWindow {
 public:
  GridHeader(wxWindow* parent, ColModel* cols) : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE), cols_(cols) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { e.Skip(); Refresh(); });
    Bind(wxEVT_MOTION, [this](wxMouseEvent& e) { motion(e); });
    Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
      drag_ = edge_at(e.GetX());
      if (drag_ >= 0) {
        drag_x_ = e.GetX();
        drag_w_ = cols_->w[drag_];
        CaptureMouse();
      }
    });
    Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) {
      if (drag_ >= 0) {
        drag_ = -1;
        if (HasCapture()) ReleaseMouse();
        return;
      }
      const auto c = cols_->layout(this, GetClientSize().x - scrollbar_);
      for (int i = 0; i < kCols; ++i)
        if (e.GetX() >= c.x[i] && e.GetX() < c.x[i + 1]) {
          // Ascending -> descending -> run order.
          if (cols_->sort != i) {
            cols_->sort = i;
            cols_->desc = i == 4 || i == 5 || i == 6;  // slowest / biggest first
          } else if (cols_->desc != (i == 4 || i == 5 || i == 6)) {
            cols_->sort = -1;
          } else {
            cols_->desc = !cols_->desc;
          }
          Refresh();
          if (on_sort) on_sort();
          return;
        }
    });
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { drag_ = -1; });
    SetToolTip("Click a title to sort; drag a column edge to resize");
  }
  void set_scrollbar(int w) {
    if (w != scrollbar_) {
      scrollbar_ = w;
      Refresh();
    }
  }
  wxSize DoGetBestClientSize() const override { return wxSize(-1, FromDIP(26)); }
  std::function<void()> on_sort, on_resize;

 private:
  // The column whose width the edge under x changes: left of the flex column a column's right
  // edge, right of it a column's left edge. -1: none.
  int edge_at(int x) const {
    const auto c = cols_->layout(this, GetClientSize().x - scrollbar_);
    const int tol = FromDIP(4);
    for (int i = 0; i < kFlex; ++i)
      if (std::abs(x - c.x[i + 1]) <= tol) return i;
    for (int i = kFlex + 1; i < kCols; ++i)
      if (std::abs(x - c.x[i]) <= tol) return i;
    return -1;
  }
  void motion(wxMouseEvent& e) {
    if (drag_ >= 0 && e.Dragging()) {
      const int dx = ToDIP(e.GetX() - drag_x_);
      const int w = drag_ < kFlex ? drag_w_ + dx : drag_w_ - dx;
      cols_->w[drag_] = std::clamp(w, 28, 600);
      Refresh();
      if (on_resize) on_resize();
      return;
    }
    SetCursor(edge_at(e.GetX()) >= 0 ? wxCursor(wxCURSOR_SIZEWE) : wxCursor(wxCURSOR_HAND));
  }
  void paint() {
    wxAutoBufferedPaintDC dc(this);
    const auto& th = rv::Theme::get();
    dc.SetBackground(wxBrush(th.bg));
    dc.Clear();
    const wxSize cs = GetClientSize();
    const auto c = cols_->layout(this, cs.x - scrollbar_);
    const wxFont f = GetFont().Scaled(0.85f).Bold();
    const int pad = FromDIP(8);
    for (int i = 0; i < kCols; ++i) {
      if (c.x[i + 1] - c.x[i] < FromDIP(12)) continue;  // left out on a narrow pane
      wxString t = kColNames[i];
      const bool sorted = cols_->sort == i;
      if (sorted) t += wxString::FromUTF8(cols_->desc ? " \xE2\x96\xBC" : " \xE2\x96\xB2");
      dc.SetFont(f);
      dc.SetTextForeground(sorted ? th.fg : th.dim);
      const wxSize ts = dc.GetTextExtent(t);
      const int x = kColRight[i] ? c.x[i + 1] - pad - ts.x : c.x[i] + pad;
      dc.DrawText(t, x, (cs.y - ts.y) / 2);
      if (i > 0) {
        dc.SetPen(wxPen(mix(th.bg, th.grid, 0.7)));
        dc.DrawLine(c.x[i], FromDIP(6), c.x[i], cs.y - FromDIP(6));
      }
    }
    dc.SetPen(wxPen(th.grid));
    dc.DrawLine(0, cs.y - 1, cs.x, cs.y - 1);
  }
  ColModel* cols_;
  int scrollbar_ = 0;
  int drag_ = -1, drag_x_ = 0, drag_w_ = 0;
};

class ResultsList : public wxVListBox {
 public:
  ResultsList(wxWindow* parent, const std::vector<StepPtr>* steps, const ColModel* cols)
      : wxVListBox(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxWANTS_CHARS), steps_(steps), cols_(cols) {
    const auto& th = rv::Theme::get();
    SetBackgroundColour(th.bg);
    bold_ = GetFont().Bold();
    small_ = GetFont().Scaled(0.92f);
    mono_ = wxFont(wxFontInfo(GetFont().GetPointSize()).Family(wxFONTFAMILY_TELETYPE).FaceName("Consolas")).Scaled(0.92f);
  }
  void refresh_rows() {
    SetItemCount(steps_->size());
    Refresh();
  }
  // Scrolls just enough to show row n.
  void ensure_visible(size_t n) {
    if (n >= GetItemCount()) return;
    const size_t first = GetVisibleRowsBegin(), end = GetVisibleRowsEnd();
    const size_t vis = end > first ? end - first : 1;
    if (n < first)
      ScrollToRow(n);
    else if (n + 1 >= end)
      ScrollToRow(n + 2 > vis ? n + 2 - vis : 0);
  }
  size_t hover_ = static_cast<size_t>(-1);

  // Checks listed under a row (Bruno lists every failed assert with expected / actual): every
  // failed one first, its full "expected ... got ..." line, then, with all_checks (a small run,
  // where every check is quick to scan), the passed ones, in their order. At most kMaxSub lines
  // (then "+ n more").
  static constexpr size_t kMaxSub = 6;
  size_t sub_lines(const StepResult& r) const {
    if (r.skipped || r.stopped() || r.errored()) return 0;
    size_t failed = 0;
    for (const auto& a : r.asserts) failed += a.pass ? 0 : 1;
    const size_t more = failed + (all_checks_ ? r.asserts.size() - failed : 0);
    return more > kMaxSub ? kMaxSub + 1 : more;
  }
  void set_all_checks(bool on) {
    if (on == all_checks_) return;
    all_checks_ = on;
    RefreshAll();
  }

 private:
  wxCoord OnMeasureItem(size_t n) const override {
    const size_t extra = n < steps_->size() ? sub_lines(*(*steps_)[n]) : 0;
    return FromDIP(25) + static_cast<int>(extra) * FromDIP(18);
  }
  void OnDrawBackground(wxDC& dc, const wxRect& rect, size_t n) const override {
    const auto& th = rv::Theme::get();
    wxColour c = th.bg;
    if (IsSelected(n))
      c = mix(th.bg, th.info, HasFocus() ? 0.32 : 0.2);
    else if (n == hover_)
      c = mix(th.bg, th.fg, 0.05);
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(c));
    dc.DrawRectangle(rect);
  }
  void OnDrawItem(wxDC& dc, const wxRect& row_rect, size_t n) const override {
    if (n >= steps_->size()) return;
    const StepResult& r = *(*steps_)[n];
    const auto& th = rv::Theme::get();
    const wxRect rect(row_rect.x, row_rect.y, row_rect.width, FromDIP(25));  // the row's own line; failed checks below
    const auto c = cols_->layout(this, rect.width);
    const int pad = FromDIP(8);
    const bool faded = r.skipped;
    auto cell = [&](int col, const wxString& t, const wxFont& f, const wxColour& colour) {
      dc.SetFont(f);
      dc.SetTextForeground(faded ? th.dim : colour);
      const int w = c.x[col + 1] - c.x[col] - 2 * pad;
      if (w < 6) return;
      const wxString s = dc.GetTextExtent(t).x > w ? wxControl::Ellipsize(t, dc, wxELLIPSIZE_END, std::max(w, 8), wxELLIPSIZE_FLAGS_EXPAND_TABS) : t;
      const wxSize ts = dc.GetTextExtent(s);
      const int x = rect.x + (kColRight[col] ? c.x[col + 1] - pad - ts.x : c.x[col] + pad);
      dc.DrawText(s, x, rect.y + (rect.height - ts.y) / 2);
    };
    const wxFont& base = GetFont();
    cell(0, wxString::Format("%zu", r.seq + 1), small_, th.dim);
    cell(1, wxString::Format("%d", r.iteration + 1), small_, th.dim);
    {
      // Method in its color, the name, then the URL as sent (dim) in the room that is left.
      const bool grpc = r.kind == RequestKind::Grpc;
      const std::string ms = grpc ? "gRPC" : r.method;
      const wxString m = U(ms);
      dc.SetFont(bold_);
      const wxSize msz = dc.GetTextExtent(m);
      const int mw = std::max(msz.x, FromDIP(44));
      dc.SetTextForeground(faded ? th.dim : method_colour(ms, grpc));
      dc.DrawText(m, rect.x + c.x[2] + pad, rect.y + (rect.height - msz.y) / 2);
      dc.SetFont(base);
      const int x0 = c.x[2] + pad + mw + FromDIP(6);
      const int w = c.x[3] - x0 - pad;
      const wxString full = U(r.name.empty() ? r.rel : r.name);
      const int nw = dc.GetTextExtent(full).x;
      const wxString s = nw > w ? wxControl::Ellipsize(full, dc, wxELLIPSIZE_END, std::max(w, 8), wxELLIPSIZE_FLAGS_EXPAND_TABS) : full;
      dc.SetTextForeground(faded ? th.dim : th.fg);
      const wxSize ns = dc.GetTextExtent(s);
      dc.DrawText(s, rect.x + x0, rect.y + (rect.height - ns.y) / 2);
      const int ux = x0 + ns.x + FromDIP(14);
      const int uw = c.x[3] - ux - pad;
      // Why it failed, unless its failed checks are listed in full below the row.
      const bool listed = r.asserts_failed > 0 && sub_lines(r) > 0;
      const std::string why = !faded && r.failed() && !listed ? r.reason() : std::string();
      if (uw > FromDIP(60) && !why.empty()) {
        // Why it failed, readable without a click (Bruno lists it under the request).
        dc.SetFont(small_);
        dc.SetTextForeground(mix(th.bg, th.err, 0.9));
        const wxString us = wxControl::Ellipsize(U(why), dc, wxELLIPSIZE_END, uw, wxELLIPSIZE_FLAGS_EXPAND_TABS);
        const wxSize uz = dc.GetTextExtent(us);
        dc.DrawText(us, rect.x + ux, rect.y + (rect.height - uz.y) / 2);
      } else if (uw > FromDIP(60) && !faded && !r.unresolved.empty()) {
        // Sent with a {{name}} nothing defines: it passed, but probably not as meant.
        std::string warn = "unresolved ";
        for (size_t i = 0; i < r.unresolved.size(); ++i) warn += (i ? ", {{" : "{{") + r.unresolved[i] + "}}";
        warn += " sent as written";
        dc.SetFont(small_);
        dc.SetTextForeground(th.warn);
        const wxString us = wxControl::Ellipsize(U(warn), dc, wxELLIPSIZE_END, uw, wxELLIPSIZE_FLAGS_EXPAND_TABS);
        dc.DrawText(us, rect.x + ux, rect.y + (rect.height - dc.GetTextExtent(us).y) / 2);
      } else if (uw > FromDIP(60)) {
        int dx = ux;
        if (r.data_row && !r.data_row->empty()) {
          // The iteration's data row (Postman lists it per iteration): "id=1 q=c d", then the URL.
          std::string d;
          for (const auto& [k, v] : *r.data_row) {
            if (d.size() > 400) break;
            d += (d.empty() ? "" : "  ") + k + "=" + (v.size() > 120 ? v.substr(0, 120) + "\xE2\x80\xA6" : v);
          }
          // Both in full when they fit. Else the URL (host, port, path: what was sent) keeps its
          // width and the data values give way first, down to a third of the room (all of them
          // are in the Variables tab); a still too long URL loses its middle.
          dc.SetFont(mono_);
          const int ufull = r.url.empty() ? 0 : dc.GetTextExtent(U(r.url)).x;
          dc.SetFont(small_);
          const int dfull = dc.GetTextExtent(U(d)).x;
          const int gap = FromDIP(12);
          int dw = uw;
          if (!r.url.empty() && dfull + gap + ufull > uw)
            dw = std::min(dfull, std::max(uw - gap - ufull, std::max((uw - gap) / 3, FromDIP(60))));
          dw = std::max(std::min(dw, uw), FromDIP(40));
          dc.SetTextForeground(faded ? th.dim : mix(th.bg, th.info, 0.85));
          const wxString ds = wxControl::Ellipsize(U(d), dc, wxELLIPSIZE_END, dw, wxELLIPSIZE_FLAGS_EXPAND_TABS);
          const wxSize dz = dc.GetTextExtent(ds);
          dc.DrawText(ds, rect.x + dx, rect.y + (rect.height - dz.y) / 2);
          dx += dz.x + FromDIP(12);
        }
        const int rw = c.x[3] - dx - pad;
        if (rw > FromDIP(40) && !r.url.empty()) {
          dc.SetFont(mono_);
          dc.SetTextForeground(mix(th.bg, th.dim, 0.8));
          const wxString us = elide_url(dc, U(r.url), rw);
          const wxSize uz = dc.GetTextExtent(us);
          dc.DrawText(us, rect.x + dx, rect.y + (rect.height - uz.y) / 2);
        }
      }
    }
    cell(3, U(status_label(r)), bold_, status_colour(r));
    cell(4, r.sent && !r.skipped ? U(fmt_ms(r.timing.total_ms)) : wxString("-"), base, th.fg);
    // Script time, outside the response time (Time): "-" for a step without any.
    cell(5, r.sent && !r.skipped && r.script_ms > 0 ? U(fmt_ms(r.script_ms)) : wxString("-"), small_, th.dim);
    cell(6, r.sent && !r.skipped ? U(fmt_bytes(r.size)) : wxString("-"), small_, th.dim);
    if (r.skipped || r.stopped())
      cell(7, wxString::FromUTF8("\xE2\x80\x94"), bold_, th.dim);
    else if (r.asserts.empty())
      // No checks: why it failed (a gRPC status other than OK, as the Failures view says).
      cell(7,
           !r.failed()                                              ? wxString::FromUTF8("\xE2\x80\x94")
           : r.kind == RequestKind::Grpc && r.sent && r.status != 0 ? wxString::FromUTF8("\xE2\x9C\x97 status not OK")
           : r.client_error                                         ? wxString::FromUTF8("\xE2\x9C\x97 error")
           : !r.sent                                                ? wxString::FromUTF8("\xE2\x9C\x97 not sent")
                                                                    : wxString::FromUTF8("\xE2\x9C\x97 failed"),
           bold_, r.failed() ? th.err : th.dim);
    else if (r.errored())
      cell(7, wxString::FromUTF8(r.sent || r.client_error ? "\xE2\x9C\x97 error" : "\xE2\x9C\x97 not sent"), bold_,
           th.err);
    else if (!r.asserts_failed && r.status_failed())
      // Every (inherited) check passed, but the status is an error the request does not expect.
      cell(7,
           wxString::Format(wxString::FromUTF8("\xE2\x9C\x97 status \xC2\xB7 %d / %zu"), r.asserts_passed,
                            r.asserts.size()),
           bold_, th.err);
    else
      cell(7,
           wxString::Format(wxString::FromUTF8(r.asserts_failed ? "\xE2\x9C\x97 %d / %zu" : "\xE2\x9C\x93 %d / %zu"),
                            r.asserts_passed, r.asserts.size()),
           bold_, r.asserts_failed ? th.err : th.ok);
    if (const size_t extra = sub_lines(r)) {
      // The checks, one per line under the request name: failed ones first, with expected / actual.
      const int x0 = rect.x + c.x[2] + pad + FromDIP(50);
      const int w = std::max(FromDIP(80), rect.x + c.x[kCols] - pad - x0);
      dc.SetFont(small_);
      int y = rect.GetBottom() - FromDIP(3);
      size_t shown = 0, listable = 0;
      for (int pass = 0; pass < 2; ++pass) {
        for (const auto& a : r.asserts) {
          if (a.pass != (pass == 1) || (a.pass && !all_checks_)) continue;
          ++listable;
          if (shown == kMaxSub) continue;
          ++shown;
          dc.SetTextForeground(a.pass ? mix(th.bg, th.ok, 0.85) : th.err);
          dc.DrawText(wxString::FromUTF8(a.pass ? "\xE2\x9C\x93" : "\xE2\x9C\x97"), x0 - FromDIP(14), y);
          dc.SetTextForeground(a.pass ? mix(th.bg, th.fg, 0.75) : mix(th.bg, th.err, 0.92));
          dc.DrawText(wxControl::Ellipsize(U(a.pass ? a.label() : a.text()), dc, wxELLIPSIZE_END, w, wxELLIPSIZE_FLAGS_EXPAND_TABS), x0, y);
          y += FromDIP(18);
        }
      }
      if (extra > kMaxSub) {
        dc.SetTextForeground(th.dim);
        dc.DrawText(wxString::Format("+ %zu more check%s (Tests tab)", listable - kMaxSub, listable - kMaxSub == 1 ? "" : "s"), x0, y);
      }
    }
  }

  const std::vector<StepPtr>* steps_;
  const ColModel* cols_;
  bool all_checks_ = false;
  wxFont bold_, small_, mono_;
};

// ------------------------------------------------------------------ per-request table

class StatsList : public wxVListBox {
 public:
  explicit StatsList(wxWindow* parent) : wxVListBox(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_THEME | wxWANTS_CHARS) {
    SetBackgroundColour(rv::Theme::get().bg);
    bold_ = GetFont().Bold();
    small_ = GetFont().Scaled(0.85f).Bold();
  }
  // base: the per-request numbers of the run compared with (empty: no delta column).
  // top: per request (item), its most frequent failure and how many kinds of failure it had.
  struct Top {
    std::string message;
    size_t count = 0, causes = 0;
  };
  void set(std::vector<runner::RequestStats> rows, std::map<std::string, double> base, std::string base_label,
           std::map<size_t, Top> top = {}) {
    top_ = std::move(top);
    rows_ = std::move(rows);
    base_ = std::move(base);
    base_label_ = std::move(base_label);
    SetItemCount(rows_.size() + 1);  // row 0: column titles
    Refresh();
  }
  const runner::RequestStats* row(size_t n) const { return n >= 1 && n <= rows_.size() ? &rows_[n - 1] : nullptr; }

 private:
  wxCoord OnMeasureItem(size_t) const override { return FromDIP(25); }
  void OnDrawBackground(wxDC& dc, const wxRect& rect, size_t n) const override {
    const auto& th = rv::Theme::get();
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(n > 0 && IsSelected(n) ? mix(th.bg, th.info, HasFocus() ? 0.32 : 0.2) : th.bg));
    dc.DrawRectangle(rect);
    if (n == 0) {
      dc.SetPen(wxPen(th.grid));
      dc.DrawLine(rect.x, rect.GetBottom(), rect.GetRight(), rect.GetBottom());
    }
  }
  void OnDrawItem(wxDC& dc, const wxRect& rect, size_t n) const override {
    const auto& th = rv::Theme::get();
    const int pad = FromDIP(8);
    const int num = FromDIP(84);
    static const char* const heads[] = {"Count", "Failed", "Avg", "Min", "P50", "P95", "Max"};
    const bool delta = !base_.empty();
    const int ncols = 7 + (delta ? 1 : 0);
    const int name_w = std::max(FromDIP(200), rect.width - ncols * num);
    auto right = [&](int col, const wxString& t, const wxFont& f, const wxColour& c) {
      dc.SetFont(f);
      dc.SetTextForeground(c);
      const wxSize ts = dc.GetTextExtent(t);
      dc.DrawText(t, rect.x + name_w + (col + 1) * num - pad - ts.x, rect.y + (rect.height - ts.y) / 2);
    };
    if (n == 0) {
      dc.SetFont(small_);
      dc.SetTextForeground(th.dim);
      dc.DrawText("Request", rect.x + pad, rect.y + (rect.height - dc.GetCharHeight()) / 2);
      for (int i = 0; i < 7; ++i) right(i, heads[i], small_, th.dim);
      // Named: the run it is compared with (the latest earlier run with the same plan).
      if (delta) right(7, wxString::FromUTF8("\xCE\x94 P50") + (base_label_.size() >= 5 ? U(" vs " + base_label_.substr(0, 5)) : wxString()), small_, th.dim);
      return;
    }
    const runner::RequestStats& q = rows_[n - 1];
    const bool grpc = q.kind == RequestKind::Grpc;
    const std::string ms = grpc ? "gRPC" : q.method;
    dc.SetFont(bold_);
    dc.SetTextForeground(method_colour(ms, grpc));
    const wxSize msz = dc.GetTextExtent(U(ms));
    dc.DrawText(U(ms), rect.x + pad, rect.y + (rect.height - msz.y) / 2);
    dc.SetFont(GetFont());
    dc.SetTextForeground(th.fg);
    const int x0 = pad + std::max(msz.x, FromDIP(44)) + FromDIP(6);
    // The request's name as the results show it, its folder (below the run's folder) dim first.
    int nx = rect.x + x0;
    int room = std::max(name_w - x0 - pad, 20);
    if (const size_t sl = q.rel.rfind('/'); sl != std::string::npos) {
      dc.SetFont(small_);
      dc.SetTextForeground(th.dim);
      const wxString f = wxControl::Ellipsize(U(q.rel.substr(0, sl) + " /"), dc, wxELLIPSIZE_START, std::max(room / 3, 20), wxELLIPSIZE_FLAGS_EXPAND_TABS);
      dc.DrawText(f, nx, rect.y + (rect.height - dc.GetCharHeight()) / 2);
      nx += dc.GetTextExtent(f).x + FromDIP(5);
      room -= dc.GetTextExtent(f).x + FromDIP(5);
      dc.SetFont(GetFont());
      dc.SetTextForeground(th.fg);
    }
    const std::string shown = q.name.empty() ? q.rel : q.name;
    const wxString nm = wxControl::Ellipsize(U(shown), dc, wxELLIPSIZE_END, std::max(room, 20), wxELLIPSIZE_FLAGS_EXPAND_TABS);
    dc.DrawText(nm, nx, rect.y + (rect.height - dc.GetCharHeight()) / 2);
    if (auto t = top_.find(q.item); t != top_.end() && q.failures) {
      // Why it failed, without a click: the most frequent cause (and how many others there are).
      const int tx = nx + dc.GetTextExtent(nm).x + FromDIP(14);
      const int tw = rect.x + name_w - pad - tx;
      if (tw > FromDIP(60)) {
        std::string why = t->second.message;
        if (t->second.count != q.failures || t->second.causes > 1)
          why = std::to_string(t->second.count) + "x " + why +
                (t->second.causes > 1 ? "  (+" + std::to_string(t->second.causes - 1) + " more cause" + (t->second.causes > 2 ? "s)" : ")") : "");
        dc.SetFont(GetFont().Scaled(0.92f));
        dc.SetTextForeground(mix(th.bg, th.err, 0.9));
        const wxString ws = wxControl::Ellipsize(U(why), dc, wxELLIPSIZE_END, tw, wxELLIPSIZE_FLAGS_EXPAND_TABS);
        dc.DrawText(ws, tx, rect.y + (rect.height - dc.GetTextExtent(ws).y) / 2);
      }
    }
    const wxFont& f = GetFont();
    right(0, wxString::Format("%zu", q.count), f, th.fg);
    right(1, wxString::Format("%zu", q.failures), bold_, q.failures ? th.err : th.dim);
    right(2, U(fmt_ms(q.avg_ms)), f, th.fg);
    right(3, U(fmt_ms(q.min_ms)), f, th.dim);
    right(4, U(fmt_ms(q.p50_ms)), bold_, th.fg);
    right(5, U(fmt_ms(q.p95_ms)), f, th.fg);
    right(6, U(fmt_ms(q.max_ms)), f, th.fg);
    if (delta) {
      auto it = base_.find(q.rel);
      if (it == base_.end() || it->second <= 0) {
        right(7, "new", f, th.dim);
      } else {
        const double d = q.p50_ms - it->second;
        const double pct = d / it->second * 100;
        char b[48];
        std::snprintf(b, sizeof b, "%+.0f%%", pct);
        // Only a change that is both relative and absolute is colored (noise stays dim).
        const bool big = std::abs(pct) >= 20 && std::abs(d) >= 0.5;
        right(7, b, big ? bold_ : f, !big ? th.dim : d > 0 ? th.err : th.ok);
      }
    }
  }
  std::vector<runner::RequestStats> rows_;
  std::map<size_t, Top> top_;
  std::map<std::string, double> base_;
  std::string base_label_;
  wxFont bold_, small_;
};

// ------------------------------------------------------------------ failures by cause

// Postman's performance view groups errors by type: one row per request + check + message (its
// numbers normalized), with how many steps failed that way, of how many sends of that request,
// and the first and last iteration. A click (or Enter) filters Results to exactly those steps.
class FailList : public wxVListBox {
 public:
  explicit FailList(wxWindow* parent) : wxVListBox(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_THEME | wxWANTS_CHARS) {
    SetBackgroundColour(rv::Theme::get().bg);
    bold_ = GetFont().Bold();
    small_ = GetFont().Scaled(0.85f).Bold();
    mono_ = wxFont(wxFontInfo(GetFont().GetPointSize()).Family(wxFONTFAMILY_TELETYPE).FaceName("Consolas")).Scaled(0.92f);
    Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) {
      e.Skip();
      const int h = VirtualHitTest(e.GetY());
      if (h > 0 && on_pick) {
        const size_t i = static_cast<size_t>(h) - 1;
        CallAfter([this, i] {
          if (on_pick && i < rows_.size()) on_pick(rows_[i]);
        });
      }
    });
  }
  // sends: finished steps per request (item), for "1000 of 1000".
  // iterations: the iterations the run reached, for "1000 of 1000 iterations".
  void set(std::vector<runner::FailureGroup> rows, std::map<size_t, size_t> sends, int iterations) {
    rows_ = std::move(rows);
    sends_ = std::move(sends);
    iterations_ = iterations;
    SetItemCount(rows_.size() + 1);  // row 0: column titles (or the empty note)
    Refresh();
  }
  const runner::FailureGroup* row(size_t n) const { return n >= 1 && n <= rows_.size() ? &rows_[n - 1] : nullptr; }
  std::function<void(const runner::FailureGroup&)> on_pick;
  size_t groups() const { return rows_.size(); }

 private:
  wxCoord OnMeasureItem(size_t n) const override { return FromDIP(n == 0 ? 26 : 42); }
  void OnDrawBackground(wxDC& dc, const wxRect& rect, size_t n) const override {
    const auto& th = rv::Theme::get();
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(n > 0 && IsSelected(n) ? mix(th.bg, th.info, HasFocus() ? 0.32 : 0.2) : th.bg));
    dc.DrawRectangle(rect);
    dc.SetPen(wxPen(n == 0 ? th.grid : mix(th.bg, th.grid, 0.5)));
    dc.DrawLine(rect.x, rect.GetBottom(), rect.GetRight(), rect.GetBottom());
  }
  void OnDrawItem(wxDC& dc, const wxRect& rect, size_t n) const override {
    const auto& th = rv::Theme::get();
    const int pad = FromDIP(8);
    const int count_w = FromDIP(110), iter_w = FromDIP(180);
    const int x_req = rect.x + count_w;
    const int right = rect.GetRight() - pad;
    if (n == 0) {
      dc.SetFont(small_);
      dc.SetTextForeground(th.dim);
      const int ty = rect.y + (rect.height - dc.GetCharHeight()) / 2;
      if (rows_.empty()) {
        dc.DrawText("No failures in this run.", rect.x + pad, ty);
        return;
      }
      const wxString c = "Failed";
      dc.DrawText(c, rect.x + count_w - pad - dc.GetTextExtent(c).x, ty);
      dc.DrawText(wxString::Format(wxString::FromUTF8("Request  \xC2\xB7  check  \xC2\xB7  message   (%zu cause%s; click one to list its results)"),
                                   rows_.size(), rows_.size() == 1 ? "" : "s"),
                  x_req + pad, ty);
      const wxString it = "Iterations";
      dc.DrawText(it, right - dc.GetTextExtent(it).x, ty);
      return;
    }
    const runner::FailureGroup& g = rows_[n - 1];
    const int y1 = rect.y + FromDIP(4), y2 = rect.y + FromDIP(22);
    // Count, and the share of that request's sends.
    dc.SetFont(bold_.Scaled(1.1f));
    dc.SetTextForeground(th.err);
    const wxString cnt = wxString::Format("%zu", g.count);
    dc.DrawText(cnt, rect.x + count_w - pad - dc.GetTextExtent(cnt).x, y1);
    if (auto it = sends_.find(g.item); it != sends_.end() && it->second) {
      dc.SetFont(GetFont().Scaled(0.85f));
      dc.SetTextForeground(th.dim);
      const wxString of = wxString::Format("of %zu sends", it->second);
      dc.DrawText(of, rect.x + count_w - pad - dc.GetTextExtent(of).x, y2);
    }
    // Iterations: lowest - highest failing one, and how many of them failed.
    dc.SetFont(GetFont());
    dc.SetTextForeground(th.fg);
    const wxString its = g.first_iteration == g.last_iteration ? wxString::Format("%d", g.first_iteration + 1)
                                                               : wxString::Format(wxString::FromUTF8("%d \xE2\x80\x93 %d"), g.first_iteration + 1, g.last_iteration + 1);
    dc.DrawText(its, right - dc.GetTextExtent(its).x, y1);
    dc.SetFont(GetFont().Scaled(0.85f));
    dc.SetTextForeground(th.dim);
    const wxString rng = iterations_ > 1 ? wxString::Format("%zu of %d iterations", g.iterations, iterations_)
                                         : wxString::Format("%zu iteration%s", g.iterations, g.iterations == 1 ? "" : "s");
    dc.DrawText(rng, right - dc.GetTextExtent(rng).x, y2);
    // Request (method color), check (mono) on the first line; the message on the second.
    const bool grpc = g.kind == RequestKind::Grpc;
    const std::string ms = grpc ? "gRPC" : g.method;
    int x = x_req + pad;
    const int room_end = right - iter_w;
    dc.SetFont(bold_);
    dc.SetTextForeground(method_colour(ms, grpc));
    dc.DrawText(U(ms), x, y1);
    x += std::max(dc.GetTextExtent(U(ms)).x, FromDIP(44)) + FromDIP(6);
    dc.SetFont(GetFont());
    dc.SetTextForeground(th.fg);
    const wxString nm = wxControl::Ellipsize(U(g.name.empty() ? g.rel : g.name), dc, wxELLIPSIZE_END, std::max(FromDIP(40), (room_end - x) / 2), wxELLIPSIZE_FLAGS_EXPAND_TABS);
    dc.DrawText(nm, x, y1);
    x += dc.GetTextExtent(nm).x + FromDIP(14);
    if (room_end - x > FromDIP(40)) {
      dc.SetFont(mono_);
      dc.SetTextForeground(th.dim);
      dc.DrawText(wxControl::Ellipsize(U(g.check), dc, wxELLIPSIZE_END, room_end - x, wxELLIPSIZE_FLAGS_EXPAND_TABS), x, y1 + FromDIP(1));
    }
    std::string msg = g.message;
    if (g.variants > 1) {
      // The numbers differ between occurrences: the first and the latest message.
      msg += "   \xE2\x80\xA6   latest: " + g.last_message;
      if (g.variants > 2) msg += "   (" + std::to_string(g.variants) + (g.variants >= 1000 ? "+" : "") + " values)";
    }
    dc.SetFont(GetFont().Scaled(0.95f));
    int mx = x_req + pad;
    if (g.unresolved) {
      // Sent with an undefined {{name}}: likely why it failed, so it leads the line.
      std::string u = "\xE2\x9A\xA0 ";
      for (size_t i = 0; i < g.unresolved_names.size(); ++i) u += (i ? ", {{" : "{{") + g.unresolved_names[i] + "}}";
      u += " unresolved";
      if (g.unresolved < g.count) u += " in " + std::to_string(g.unresolved) + " of " + std::to_string(g.count);
      const wxString us = wxControl::Ellipsize(U(u), dc, wxELLIPSIZE_END, std::max(FromDIP(60), (room_end - mx) / 2), wxELLIPSIZE_FLAGS_EXPAND_TABS);
      dc.SetTextForeground(th.warn);
      dc.DrawText(us, mx, y2);
      mx += dc.GetTextExtent(us).x + FromDIP(12);
    }
    dc.SetTextForeground(mix(th.bg, th.err, 0.92));
    dc.DrawText(wxControl::Ellipsize(U(msg), dc, wxELLIPSIZE_END, std::max(FromDIP(60), room_end - mx), wxELLIPSIZE_FLAGS_EXPAND_TABS), mx, y2);
  }
  std::vector<runner::FailureGroup> rows_;
  std::map<size_t, size_t> sends_;
  int iterations_ = 0;
  wxFont bold_, small_, mono_;
};

// ------------------------------------------------------------------ over time (load mode)

// Postman's performance view, in two panels over the run's wall time that share the time axis:
// above, the p50 / p95 response time (lines, smoothed over neighboring buckets); below, the
// sends finished per second (bars, failed part red) and the active workers. One quantity per
// axis, so no line has to be read through the legend.
class TimeChart : public wxWindow {
 public:
  explicit TimeChart(wxWindow* parent) : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      Refresh();
    });
    Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
      const int b = bucket_at(e.GetX());
      if (b != hover_) {
        hover_ = b;
        Refresh();
      }
    });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
      hover_ = -1;
      Refresh();
    });
  }
  // Buckets the finished, sent steps by when they finished. O(n log n) in the steps: called at
  // most a few times a second while this view is shown.
  void set(const std::vector<StepPtr>* steps, double wall_ms) {
    double end = std::max(wall_ms, 1.0);
    for (const auto& p : *steps)
      if (p->sent && !p->skipped) end = std::max(end, p->started_ms + p->timing.total_ms);
    static const double nice[] = {100, 200, 250, 500, 1000, 2000, 5000, 10000, 15000, 30000, 60000, 120000, 300000, 600000};
    bucket_ms_ = nice[std::size(nice) - 1];
    for (double n : nice)
      if (end / n <= 60) {
        bucket_ms_ = n;
        break;
      }
    const size_t nb = static_cast<size_t>(std::ceil(end / bucket_ms_));
    std::vector<std::vector<double>> t(nb);
    b_.assign(nb, {});
    total_ = failed_ = 0;
    for (const auto& p : *steps) {
      const StepResult& r = *p;
      if (!r.sent || r.skipped || r.stopped()) continue;
      const size_t i = std::min(nb - 1, static_cast<size_t>((r.started_ms + r.timing.total_ms) / bucket_ms_));
      t[i].push_back(r.timing.total_ms);
      ++b_[i].n;
      ++total_;
      if (r.failed()) {
        ++b_[i].failed;
        ++failed_;
      }
    }
    for (size_t i = 0; i < nb; ++i)
      if (!t[i].empty()) {
        b_[i].p50 = runner::percentile(t[i], 50);
        b_[i].p95 = runner::percentile(t[i], 95);
      }
    // The p50 and p95 lines are drawn over a window of neighboring buckets (until it holds 100
    // sends, two buckets either side at most): with a slow request in the mix (one send in nine
    // at 120 ms, the rest at 2 ms) a single bucket's p95 (and, in a bucket where mostly the slow
    // sends finished, its p50) flips between the two on every bucket. A bucket whose own p95 is
    // far off that line is marked as a dot of its own.
    std::vector<double> win;
    for (size_t i = 0; i < nb; ++i) {
      if (t[i].empty()) continue;
      win.assign(t[i].begin(), t[i].end());
      for (size_t r = 1; r <= 2 && win.size() < 100; ++r) {
        if (i >= r) win.insert(win.end(), t[i - r].begin(), t[i - r].end());
        if (i + r < nb) win.insert(win.end(), t[i + r].begin(), t[i + r].end());
      }
      b_[i].p95s = runner::percentile(win, 95);
      b_[i].p50s = runner::percentile(win, 50);
    }
    // Active workers (k6's VUs, Postman's virtual users): the sends in flight, averaged over
    // each bucket (each send adds the share of the bucket it overlaps).
    max_active_ = 0;
    for (const auto& p : *steps) {
      const StepResult& r = *p;
      if (!r.sent || r.skipped || r.stopped()) continue;
      const double a = r.started_ms, z = r.started_ms + r.timing.total_ms;
      const size_t i0 = std::min(nb - 1, static_cast<size_t>(std::max(0.0, a) / bucket_ms_));
      const size_t i1 = std::min(nb - 1, static_cast<size_t>(std::max(0.0, z) / bucket_ms_));
      for (size_t i = i0; i <= i1; ++i) {
        const double lo = std::max(a, bucket_ms_ * static_cast<double>(i)), hi = std::min(z, bucket_ms_ * static_cast<double>(i + 1));
        if (hi > lo) b_[i].busy_ms += hi - lo;
      }
    }
    for (size_t i = 0; i < nb; ++i) {
      // The last bucket is partly past the end: rates and the average over the time it covers.
      const double span = std::min(bucket_ms_, end - bucket_ms_ * static_cast<double>(i));
      b_[i].span_ms = std::max(span, 0.0);
      b_[i].partial = span < bucket_ms_ * 0.999;
      b_[i].active = span > 0 ? b_[i].busy_ms / span : 0;
      max_active_ = std::max(max_active_, b_[i].active);
    }
    end_ms_ = end;
    Refresh();
  }

 private:
  struct B {
    size_t n = 0, failed = 0;
    double p50 = 0, p95 = 0;
    double p95s = 0;  // p95 over the bucket and its neighbors (the drawn line)
    double p50s = 0;  // p50 likewise
    double busy_ms = 0, active = 0;  // send time in the bucket; active = busy_ms / bucket length
    double span_ms = 0;              // the run time it covers (less than a bucket for the last one)
    bool partial = false;            // the last bucket, cut by the run's end: few sends, drawn hatched,
                                     // no p50 / p95 point (its tail would read as a latency collapse)
    double rate(double bucket_ms) const { return span_ms > 0 ? static_cast<double>(n) * 1000.0 / std::max(span_ms, bucket_ms * 0.25) : 0; }
    double fail_rate(double bucket_ms) const { return n ? rate(bucket_ms) * static_cast<double>(failed) / static_cast<double>(n) : 0; }
  };
  static bool outlier(const B& b) { return b.p95s > 0 && (b.p95 > b.p95s * 2 || b.p95 < b.p95s / 2); }
  wxRect plot() const {
    const wxSize cs = GetClientSize();
    return wxRect(FromDIP(64), FromDIP(72), std::max(10, cs.x - FromDIP(64 + 64)), std::max(10, cs.y - FromDIP(72 + 30)));
  }
  // Time runs left to right over the run (end_ms_), so the cut last bucket is drawn as narrow as
  // the time it covers.
  // The two panels inside plot(): response time above, throughput below.
  std::pair<wxRect, wxRect> panels() const {
    const wxRect pr = plot();
    const int gap = FromDIP(30);
    const int th = std::max(10, (pr.height - gap) * 52 / 100);
    return {wxRect(pr.x, pr.y, pr.width, th), wxRect(pr.x, pr.y + th + gap, pr.width, std::max(10, pr.height - th - gap))};
  }
  int bucket_at(int x) const {
    const wxRect pr = plot();
    if (b_.empty() || x < pr.x || x >= pr.GetRight() || end_ms_ <= 0) return -1;
    const double t = end_ms_ * static_cast<double>(x - pr.x) / std::max(1, pr.width);
    return std::min(static_cast<int>(b_.size()) - 1, static_cast<int>(t / bucket_ms_));
  }
  void paint() {
    wxAutoBufferedPaintDC dc(this);
    const auto& th = rv::Theme::get();
    dc.SetBackground(wxBrush(th.bg));
    dc.Clear();
    const wxFont small = GetFont().Scaled(0.85f);
    const wxRect pr = plot();
    dc.SetFont(small);
    if (b_.empty() || total_ == 0) {
      dc.SetTextForeground(th.dim);
      dc.DrawText("No sends yet. Run the requests (Parallel and Duration set a load profile) to see throughput and "
                  "response time over time.",
                  FromDIP(16), FromDIP(16));
      return;
    }
    double max_rps = 0, max_ms = 0;
    for (const auto& b : b_) {
      if (b.partial && b.span_ms < bucket_ms_ * 0.5 && b_.size() > 1) continue;  // a sliver sets no scale
      max_rps = std::max(max_rps, b.rate(bucket_ms_));
      if (!b.partial || b_.size() == 1) max_ms = std::max({max_ms, b.p95, b.p95s});
    }
    max_rps = std::max(max_rps, 1.0) * 1.1;
    max_ms = std::max(max_ms, 0.01) * 1.1;
    const auto [tp, bp] = panels();
    // When p50 sits far below p95 (0.66 vs 130 ms) a linear axis puts it on the floor: the
    // times then use a log axis, so both lines stay readable.
    double min_ms = max_ms;
    for (const auto& b : b_)
      if (b.n && b.p50s > 0 && (!b.partial || b_.size() == 1)) min_ms = std::min(min_ms, b.p50s);
    const bool log_axis = max_ms / std::max(min_ms, 1e-6) > 20;
    const double lo_ms = log_axis ? std::max(min_ms / 1.5, 1e-3) : 0;
    auto y_of_ms = [&](double v) {
      if (!log_axis) return static_cast<int>(tp.height * v / max_ms);
      const double t = (std::log(std::max(v, lo_ms)) - std::log(lo_ms)) / (std::log(max_ms) - std::log(lo_ms));
      return static_cast<int>(tp.height * std::clamp(t, 0.0, 1.0));
    };
    // Totals.
    char head[256];
    std::snprintf(head, sizeof head, "%zu sends in %s  \xC2\xB7  %.1f / s average  \xC2\xB7  %.2f%% failed  \xC2\xB7  bucket %s",
                  total_, fmt_ms(end_ms_).c_str(), static_cast<double>(total_) * 1000.0 / end_ms_,
                  100.0 * static_cast<double>(failed_) / static_cast<double>(total_), fmt_ms(bucket_ms_).c_str());
    dc.SetTextForeground(th.dim);
    dc.DrawText(U(head), FromDIP(16), FromDIP(10));
    const wxColour bar = mix(th.bg, th.info, 0.55), fail = th.err, c50 = th.ok, c95 = th.warn;
    const wxColour cact = mix(th.fg, wxColour(190, 140, 255), 0.7);
    // Each panel's title above its left edge, its series' keys after it (the line or box in its
    // color): what a line is reads next to the panel it is in.
    struct Key {
      wxString text;
      wxColour colour;
      int kind;  // 0 box, 1 line, 2 dashed line
    };
    auto title = [&](const wxRect& r, const wxString& t, const std::vector<Key>& keys) {
      int x = r.x;
      const int y = r.y - FromDIP(20);
      dc.SetTextForeground(th.fg);
      dc.DrawText(t, x, y);
      x += dc.GetTextExtent(t).x + FromDIP(16);
      for (const Key& k : keys) {
        dc.SetBrush(wxBrush(k.colour));
        dc.SetPen(k.kind ? wxPen(k.colour, FromDIP(2), k.kind == 2 ? wxPENSTYLE_SHORT_DASH : wxPENSTYLE_SOLID)
                         : *wxTRANSPARENT_PEN);
        if (k.kind) dc.DrawLine(x, y + FromDIP(8), x + FromDIP(12), y + FromDIP(8));
        else dc.DrawRectangle(x, y + FromDIP(3), FromDIP(12), FromDIP(10));
        x += FromDIP(16);
        dc.SetTextForeground(th.dim);
        dc.DrawText(k.text, x, y);
        x += dc.GetTextExtent(k.text).x + FromDIP(14);
      }
    };
    const double act_top = std::max(1.0, std::ceil(max_active_)) * 1.1;
    {
      char at[64];
      std::snprintf(at, sizeof at, "active workers (max %.1f, right axis)", max_active_);
      title(tp, log_axis ? wxString("Response time (log axis)") : wxString("Response time"),
            {{"p50", c50, 1}, {"p95", c95, 1}});
      title(bp, "Throughput", {{"sends / s", bar, 0}, {"failed / s", fail, 0}, {U(at), cact, 2}});
    }
    // Grids and axes: ms left of the top panel, sends / s left and workers right of the bottom one.
    const wxPen grid(mix(th.bg, th.grid, 0.6));
    for (int g = 0; g <= 4; ++g) {
      const int y = tp.GetBottom() - tp.height * g / 4;
      dc.SetPen(grid);
      dc.DrawLine(tp.x, y, tp.GetRight(), y);
      const wxString l = U(fmt_ms(log_axis ? lo_ms * std::pow(max_ms / lo_ms, g / 4.0) : max_ms * g / 4));
      dc.SetTextForeground(th.dim);
      const wxSize ls = dc.GetTextExtent(l);
      dc.DrawText(l, tp.x - ls.x - FromDIP(6), y - ls.y / 2);
    }
    for (int g = 0; g <= 2; ++g) {
      const int y = bp.GetBottom() - bp.height * g / 2;
      dc.SetPen(grid);
      dc.DrawLine(bp.x, y, bp.GetRight(), y);
      char l[32], r[32];
      std::snprintf(l, sizeof l, max_rps * g / 2 >= 10 ? "%.0f/s" : "%.1f/s", max_rps * g / 2);
      std::snprintf(r, sizeof r, act_top * g / 2 >= 10 ? "%.0f" : "%.1f", act_top * g / 2);
      dc.SetTextForeground(th.dim);
      const wxSize ls = dc.GetTextExtent(l);
      dc.DrawText(l, bp.x - ls.x - FromDIP(6), y - ls.y / 2);
      dc.SetTextForeground(cact);
      dc.DrawText(r, bp.GetRight() + FromDIP(6), y - ls.y / 2);
    }
    const size_t nb = b_.size();
    auto xat = [&](size_t i) {
      const double t = std::min(end_ms_, bucket_ms_ * static_cast<double>(i));
      return tp.x + static_cast<int>(static_cast<double>(tp.width) * t / std::max(end_ms_, 1e-9));
    };
    dc.SetTextForeground(th.dim);
    for (int g = 0; g <= 4; ++g) {
      // Every tick in the same unit ("0 s", "4.5 s", "9 s"; ms for a run under a second).
      const double v = end_ms_ * g / 4;
      char tb[32];
      if (end_ms_ >= 1000)
        std::snprintf(tb, sizeof tb, end_ms_ >= 20000 || v == 0 ? "%.0f s" : "%.1f s", v / 1000);
      else
        std::snprintf(tb, sizeof tb, "%.0f ms", v);
      const wxString t = tb;
      const int x = bp.x + bp.width * g / 4 - (g == 4 ? dc.GetTextExtent(t).x : g == 0 ? 0 : dc.GetTextExtent(t).x / 2);
      dc.DrawText(t, x, bp.GetBottom() + FromDIP(6));
    }
    // The hovered bucket: a band across both panels.
    if (hover_ >= 0 && static_cast<size_t>(hover_) < nb) {
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.SetBrush(wxBrush(mix(th.bg, th.fg, th.dark ? 0.07 : 0.05)));
      const int x0 = xat(static_cast<size_t>(hover_)), x1 = std::max(x0 + 2, xat(static_cast<size_t>(hover_) + 1));
      dc.DrawRectangle(x0, tp.y, x1 - x0, tp.height + 1);
      dc.DrawRectangle(x0, bp.y, x1 - x0, bp.height + 1);
    }
    // Bars.
    dc.SetPen(*wxTRANSPARENT_PEN);
    for (size_t i = 0; i < nb; ++i) {
      const auto& b = b_[i];
      if (!b.n) continue;
      const int x0 = xat(i) + 1, x1 = std::max(x0 + 1, xat(i + 1) - 1);
      const int h = std::min(bp.height, static_cast<int>(bp.height * (b.rate(bucket_ms_) / max_rps)));
      const int hf = std::min(h, static_cast<int>(bp.height * (b.fail_rate(bucket_ms_) / max_rps)));
      const wxColour bc = static_cast<int>(i) == hover_ ? mix(bar, th.fg, 0.25) : bar;
      dc.SetBrush(b.partial && nb > 1 ? wxBrush(bc, wxBRUSHSTYLE_BDIAGONAL_HATCH) : wxBrush(bc));
      if (b.partial && nb > 1) dc.SetPen(wxPen(bc));
      dc.DrawRectangle(x0, bp.GetBottom() - h, x1 - x0, h - hf);
      if (hf) {
        dc.SetBrush(wxBrush(fail));
        dc.DrawRectangle(x0, bp.GetBottom() - hf, x1 - x0, hf);
      }
      dc.SetPen(*wxTRANSPARENT_PEN);
    }
    // Active workers: a stepped dashed line over the bars, on the right axis.
    {
      dc.SetPen(wxPen(cact, FromDIP(2), wxPENSTYLE_SHORT_DASH));
      int prev_y = -1;
      for (size_t i = 0; i < nb; ++i) {
        const int y = bp.GetBottom() - static_cast<int>(bp.height * (b_[i].active / act_top));
        const int x0 = xat(i), x1 = xat(i + 1);
        if (prev_y >= 0 && prev_y != y) dc.DrawLine(x0, prev_y, x0, y);
        dc.DrawLine(x0, y, x1, y);
        prev_y = y;
      }
    }
    // Response time lines (smoothed), joined across buckets in which nothing finished; the cut
    // last bucket has no point.
    for (int k = 0; k < 2; ++k) {
      dc.SetPen(wxPen(k ? c95 : c50, FromDIP(2)));
      wxPoint prev(-1, -1);
      for (size_t i = 0; i < nb; ++i) {
        const auto& b = b_[i];
        if (!b.n) continue;
        if (b.partial && nb > 1) break;
        const double v = k ? b.p95s : b.p50s;
        const wxPoint pt((xat(i) + xat(i + 1)) / 2, tp.GetBottom() - y_of_ms(v));
        if (prev.x >= 0) dc.DrawLine(prev, pt);
        else dc.DrawCircle(pt, FromDIP(1));
        prev = pt;
      }
    }
    // Buckets whose own p95 is far from the line (a burst of slow sends, or none of them): a
    // hollow dot at their own value.
    dc.SetBrush(wxBrush(th.bg));
    dc.SetPen(wxPen(c95, FromDIP(1)));
    for (size_t i = 0; i < nb; ++i) {
      const auto& b = b_[i];
      if (!b.n || (b.partial && nb > 1) || !outlier(b)) continue;
      dc.DrawCircle(wxPoint((xat(i) + xat(i + 1)) / 2, tp.GetBottom() - y_of_ms(b.p95)), FromDIP(3));
    }
    // Hovered bucket.
    if (hover_ >= 0 && static_cast<size_t>(hover_) < nb) {
      const auto& b = b_[static_cast<size_t>(hover_)];
      char t[256];
      std::snprintf(t, sizeof t, "%s - %s:  %zu sends (%.1f / s), %zu failed, p50 %s, p95 %s (smoothed %s / %s), %.1f active workers%s",
                    fmt_ms(bucket_ms_ * hover_).c_str(), fmt_ms(bucket_ms_ * hover_ + b.span_ms).c_str(), b.n,
                    b.rate(bucket_ms_), b.failed, fmt_ms(b.p50).c_str(), fmt_ms(b.p95).c_str(), fmt_ms(b.p50s).c_str(),
                    fmt_ms(b.p95s).c_str(), b.active,
                    b.partial && nb > 1 ? "  (partial bucket at the run's end: no p50 / p95 point)" : "");
      dc.SetTextForeground(th.fg);
      dc.DrawText(U(t), FromDIP(16), FromDIP(28));
    }
  }
  std::vector<B> b_;
  double bucket_ms_ = 1000, end_ms_ = 0, max_active_ = 0;
  size_t total_ = 0, failed_ = 0;
  int hover_ = -1;
};

// ------------------------------------------------------------------ request checklist

// The requests to run: a checkbox, the method in its sidebar color, the path. Space toggles,
// Alt+Up / Alt+Down move, a click on the box toggles, a double click opens the request.
class RequestList : public wxVListBox {
 public:
  struct Row {
    std::string method, name, folder, tags;  // name: the request's name (as the results show it)
    bool grpc = false;
    bool checked = true;
  };
  explicit RequestList(wxWindow* parent) : wxVListBox(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_THEME | wxWANTS_CHARS) {
    SetBackgroundColour(rv::Theme::get().bg);
    bold_ = GetFont().Bold().Scaled(0.9f);
    small_ = GetFont().Scaled(0.85f);
    Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
      const int h = VirtualHitTest(e.GetY());
      if (h != wxNOT_FOUND && e.GetX() < FromDIP(30)) {
        SetSelection(h);
        toggle(static_cast<size_t>(h));
        SetFocus();
        return;
      }
      // A press on a row may start a drag (Bruno's drag handles): it moves once past a few px.
      press_ = h;
      press_y_ = e.GetY();
      e.Skip();
    });
    Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
      if (press_ != wxNOT_FOUND && e.LeftIsDown() && !dragging_ && std::abs(e.GetY() - press_y_) > FromDIP(5) && on_drop) {
        dragging_ = true;
        CaptureMouse();
        SetCursor(wxCursor(wxCURSOR_SIZENS));
      }
      if (dragging_) {
        const int t = drop_at(e.GetY());
        if (t != drop_) {
          drop_ = t;
          Refresh();
        }
        return;
      }
      e.Skip();
    });
    Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) {
      if (!dragging_) {
        press_ = wxNOT_FOUND;
        e.Skip();
        return;
      }
      const int from = press_, to = drop_;
      end_drag();
      if (from != wxNOT_FOUND && to >= 0 && to != from && to != from + 1 && on_drop) on_drop(static_cast<size_t>(from), static_cast<size_t>(to));
    });
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { end_drag(); });
    Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
      const int sel = GetSelection();
      if (e.GetKeyCode() == WXK_SPACE && e.GetModifiers() == 0 && sel != wxNOT_FOUND) return toggle(static_cast<size_t>(sel));
      if (e.GetModifiers() == wxMOD_ALT && (e.GetKeyCode() == WXK_UP || e.GetKeyCode() == WXK_DOWN)) {
        if (on_move && sel != wxNOT_FOUND) on_move(static_cast<size_t>(sel), e.GetKeyCode() == WXK_UP ? -1 : 1);
        return;
      }
      e.Skip();
    });
  }
  std::vector<Row> rows;
  std::function<void(size_t)> on_toggle;
  std::function<void(size_t, int)> on_move;
  // Drag and drop: row `from` goes before row `before` (rows.size(): to the end).
  std::function<void(size_t, size_t)> on_drop;
  void refresh_rows() {
    SetItemCount(rows.size());
    Refresh();
  }

 private:
  // The gap a drop at y lands in: 0 .. rows.size().
  int drop_at(int y) const {
    const int rh = FromDIP(24);
    const int vy = y + static_cast<int>(GetVisibleRowsBegin()) * rh;
    return std::clamp((vy + rh / 2) / rh, 0, static_cast<int>(rows.size()));
  }
  void end_drag() {
    if (HasCapture()) ReleaseMouse();
    dragging_ = false;
    press_ = wxNOT_FOUND;
    drop_ = -1;
    SetCursor(wxNullCursor);
    Refresh();
  }
  void OnDrawSeparator(wxDC& dc, wxRect& rect, size_t n) const override {
    // The drop line while dragging: above row n (or below the last row).
    if (!dragging_ || drop_ < 0) return;
    const auto& th = rv::Theme::get();
    dc.SetPen(wxPen(th.accent, FromDIP(2)));
    if (static_cast<int>(n) == drop_) dc.DrawLine(rect.x, rect.y + 1, rect.GetRight(), rect.y + 1);
    if (n + 1 == rows.size() && drop_ == static_cast<int>(rows.size()))
      dc.DrawLine(rect.x, rect.GetBottom() - 1, rect.GetRight(), rect.GetBottom() - 1);
  }
  int press_ = wxNOT_FOUND, press_y_ = 0, drop_ = -1;
  bool dragging_ = false;
  void toggle(size_t n) {
    if (n >= rows.size()) return;
    rows[n].checked = !rows[n].checked;
    RefreshRow(n);
    if (on_toggle) on_toggle(n);
  }
  wxCoord OnMeasureItem(size_t) const override { return FromDIP(24); }
  void OnDrawBackground(wxDC& dc, const wxRect& rect, size_t n) const override {
    const auto& th = rv::Theme::get();
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(IsSelected(n) ? mix(th.bg, th.info, HasFocus() ? 0.3 : 0.16) : th.bg));
    dc.DrawRectangle(rect);
  }
  void OnDrawItem(wxDC& dc, const wxRect& rect, size_t n) const override {
    if (n >= rows.size()) return;
    const Row& r = rows[n];
    const auto& th = rv::Theme::get();
    const int box = FromDIP(14);
    const int bx = rect.x + FromDIP(8), by = rect.y + (rect.height - box) / 2;
    dc.SetPen(wxPen(r.checked ? th.accent : th.dim));
    dc.SetBrush(r.checked ? wxBrush(th.accent) : *wxTRANSPARENT_BRUSH);
    dc.DrawRoundedRectangle(bx, by, box, box, FromDIP(3));
    if (r.checked) {
      dc.SetPen(wxPen(*wxWHITE, FromDIP(2)));
      dc.DrawLine(bx + box * 22 / 100, by + box / 2, bx + box * 42 / 100, by + box * 72 / 100);
      dc.DrawLine(bx + box * 42 / 100, by + box * 72 / 100, bx + box * 78 / 100, by + box * 28 / 100);
    }
    int x = bx + box + FromDIP(8);
    dc.SetFont(bold_);
    dc.SetTextForeground(r.checked ? method_colour(r.method, r.grpc) : th.dim);
    const std::string m = r.grpc ? "gRPC" : r.method;
    const wxSize ms = dc.GetTextExtent(U(m));
    dc.DrawText(U(m), x, rect.y + (rect.height - ms.y) / 2);
    x += std::max(ms.x, FromDIP(40)) + FromDIP(6);
    dc.SetFont(GetFont());
    dc.SetTextForeground(r.checked ? th.fg : th.dim);
    int tw = 0;
    if (!r.tags.empty()) {
      dc.SetFont(small_);
      tw = std::min(dc.GetTextExtent(U(r.tags)).x, (rect.width - x) / 3);
      dc.SetFont(GetFont());
    }
    int room = std::max(rect.width - x - tw - FromDIP(14), 20);
    if (!r.folder.empty()) {
      // The folder below the run's folder, dim, before the name ("orders / Create order").
      dc.SetFont(small_);
      dc.SetTextForeground(th.dim);
      const wxString f = wxControl::Ellipsize(U(r.folder + " /"), dc, wxELLIPSIZE_START, std::max(room / 3, 20), wxELLIPSIZE_FLAGS_EXPAND_TABS);
      const wxSize fs = dc.GetTextExtent(f);
      dc.DrawText(f, x, rect.y + (rect.height - fs.y) / 2);
      x += fs.x + FromDIP(5);
      room -= fs.x + FromDIP(5);
      dc.SetFont(GetFont());
      dc.SetTextForeground(r.checked ? th.fg : th.dim);
    }
    const wxString t = wxControl::Ellipsize(U(r.name), dc, wxELLIPSIZE_END, std::max(room, 20), wxELLIPSIZE_FLAGS_EXPAND_TABS);
    const wxSize ts = dc.GetTextExtent(t);
    dc.DrawText(t, x, rect.y + (rect.height - ts.y) / 2);
    if (tw) {
      dc.SetFont(small_);
      dc.SetTextForeground(th.dim);
      const wxString tg = wxControl::Ellipsize(U(r.tags), dc, wxELLIPSIZE_END, tw, wxELLIPSIZE_FLAGS_EXPAND_TABS);
      dc.DrawText(tg, rect.GetRight() - FromDIP(8) - dc.GetTextExtent(tg).x, rect.y + (rect.height - dc.GetCharHeight()) / 2);
    }
  }
  wxFont bold_, small_;
};

// ------------------------------------------------------------------ the window

std::map<std::wstring, wxWeakRef<wxWindow>>& open_runners() {
  static std::map<std::wstring, wxWeakRef<wxWindow>> m;
  return m;
}

std::wstring runner_key(const fs::path& root, const fs::path& folder) {
  std::wstring k = (folder.empty() ? root : folder).lexically_normal().wstring();
  for (auto& c : k) c = static_cast<wchar_t>(towlower(c));
  while (k.size() > 3 && (k.back() == L'\\' || k.back() == L'/')) k.pop_back();
  return k;
}

enum ChipId {
  // left
  kAll = 1, kNone, kUp, kDown, kDataPick, kDataClear, kDataPreview, kEnvPick,
  // load profile (Postman's performance runner): fixed, ramp up, peak, spike
  kProfFixed = 12, kProfRamp, kProfPeak, kProfSpike, kOrderReset,
  // results bar
  kFAll = 20, kFPassed, kFFailed, kFSkipped, kVResults, kVRequests, kVTime, kVFail, kScope,
  kRunAgain = 40, kRerunFailed, kHistory, kHtml, kJson, kJunit, kReset,
};

// A tab of the main window's notebook (as Postman's and Bruno's runners are), next to the
// request tabs it runs.
class RunnerFrame : public wxPanel {
 public:
  RunnerFrame(wxWindow* parent, std::function<Workspace*()> workspace, const Workspace& ws, fs::path folder,
              std::function<std::shared_ptr<GrpcClient>()> grpc)
      : wxPanel(parent, wxID_ANY),
        workspace_(std::move(workspace)),
        grpc_(std::move(grpc)),
        root_(ws.root()),
        folder_(std::move(folder)) {
    link_ = std::make_shared<Link>();
    link_->frame = this;
    key_ = runner_key(root_, folder_);
    const auto& th = rv::Theme::get();
    {
      const fs::path& p = folder_.empty() || key_ == runner_key(root_, {}) ? root_ : folder_;
      name_s_ = runner::strip_ext(u8(p.filename()));
    }
    SetBackgroundColour(th.bg);
    env_ = ws.active_env;
    build_shell();
    // The window opens empty and fills in on the next timer ticks (the left side, then the
    // results side): each step stays short and the UI thread answers in between.
    // Five steps of 10-25 ms each (the request list, the two halves of the settings, the results
    // side without its detail pane, then the detail pane), never one of 50.
    stage_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
      if (left_stage_ < kLeftBuilt) {
        wxWindowUpdateLocker lock(left_);  // children made on a visible window: draw once, at the end
        if (left_stage_ == 0) build();
        else build_settings(left_stage_ - 1);
        ++left_stage_;
        relayout_left();
        if (left_stage_ == kLeftBuilt) fit_list_height();  // the settings are there: the list's room
        stage_.StartOnce(1);
        return;
      }
      {
        wxWindowUpdateLocker lock(right_);
        if (right_stage_ == 0) {
          build_right();
          right_->Layout();
          right_stage_ = 1;
          stage_.StartOnce(1);
          return;
        }
        build_detail();
      }
      if (pending_items_) {
        auto items = std::move(pending_items_);
        loaded(std::move(items), pending_errs_);
      }
    });
    stage_.StartOnce(1);
    tick_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); });
    save_order_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { flush_order(); });
    reload_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { reload(0); });
    restore_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { restore_step(); });
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) { if (!on_key(e)) e.Skip(); });
    load(ws);
    watch();
    start_layout_check();
    start_open_check();
  }
  ~RunnerFrame() override {
    watcher_.reset();  // joins the watcher thread before the window goes
    flush_order();  // a reorder just before closing is still saved
    save_settings();
    link_->frame = nullptr;
    if (run_) run_->ctl.stop();
  }

  // "Runner: api" for the tab.
  wxString tab_title() const { return U("Runner: " + name_s_); }
  const fs::path& folder() const { return folder_; }
  // Ctrl+Enter / Ctrl+Shift+Enter from the main window's accelerators while this tab is shown.
  void run_from_shell(bool failed_only) { on_run(failed_only); }

  // Closes this tab (Ctrl+W, the tab's close button goes through the notebook itself).
  void close_tab() {
    wxWindow* w = GetParent();
    while (w && !dynamic_cast<wxAuiNotebook*>(w)) w = w->GetParent();
    auto* nb = static_cast<wxAuiNotebook*>(w);
    if (!nb) return;
    wxWeakRef<wxWindow> self(this);
    wxWeakRef<wxAuiNotebook> book(nb);
    wxTheApp->CallAfter([book, self] {
      if (!self || !book) return;
      const int i = book->GetPageIndex(self.get());
      if (i == wxNOT_FOUND) return;
      forget_tab_hover(book.get());
      book->DeletePage(static_cast<size_t>(i));
      if (book->GetPageCount() > 0) book->SetSelection(std::min<size_t>(static_cast<size_t>(i), book->GetPageCount() - 1));
    });
  }

 private:
  // WATT_RUNNER_LAYOUT_CHECK=<file> (with WATT_RUN_FOLDER; tests/test_runner.cpp): drives the
  // runner through everything that shows or hides the "Plan changed" note and checks after each
  // step that Iterations, Parallel and Run are where they were before (as drawn, and after the
  // layout the next keystroke would do): a run, a filter typed and cleared, a request unchecked
  // and checked again, a request edited on disk (the test edits one once it sees
  // {"check":"ran"}), and a second run that clears the note. Writes one JSON line per step and
  // ends the process. Unset: one getenv per runner opened.
  void start_layout_check() {
    wxString out;
    if (!wxGetEnv("WATT_RUNNER_LAYOUT_CHECK", &out) || out.empty()) return;
    check_out_ = out.ToStdWstring();
    check_started_ = Clock::now();
    check_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { layout_check_step(); });
    check_timer_.Start(50);
  }
  void check_write(const std::string& line) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, check_out_.c_str(), L"ab") == 0 && f) {
      std::fputs((line + "\n").c_str(), f);
      std::fclose(f);
    }
  }
  std::array<int, 3> check_ys() const {
    return {iters_->GetPosition().y, par_->GetPosition().y, run_btn_->GetPosition().y};
  }
  // How far the fields moved from where they were first drawn: as drawn now, then after a layout.
  void check_measure(const char* step) {
    int moved = 0;
    for (int pass = 0; pass < 2; ++pass) {
      if (pass) relayout_left();
      const auto ys = check_ys();
      for (size_t i = 0; i < ys.size(); ++i) moved = std::max(moved, std::abs(ys[i] - check_base_[i]));
    }
    check_moved_ = std::max(check_moved_, moved);
    check_write(std::string("{\"check\":\"step\",\"step\":\"") + step + "\",\"moved\":" + std::to_string(moved) +
                ",\"note\":" + (changed_text_.empty() ? "false" : "true") + "}");
  }
  void layout_check_step() {
    const bool idle = items_ && detail_ && !run_ && !preparing_ && left_stage_ >= kLeftBuilt;
    const size_t runs = run_history()[key_].size();
    if (std::chrono::duration<double>(Clock::now() - check_started_).count() > 60) {
      check_write("{\"check\":\"timeout\",\"phase\":" + std::to_string(check_phase_) + "}");
      TerminateProcess(GetCurrentProcess(), 3);
    }
    if (!idle) return;
    switch (check_phase_) {
      case 0:
        if (items_->size() < 2) return;
        relayout_left();
        check_base_ = check_ys();
        check_runs_ = runs;
        on_run(false);
        check_phase_ = 1;
        break;
      case 1:
        if (runs <= check_runs_) return;
        check_measure("run");
        check_write("{\"check\":\"ran\"}");
        check_phase_ = 2;
        break;
      case 2: {  // filter out the last request, as typed
        const RunItem& last = (*items_)[order_.back()];
        filter_->SetValue("-" + U(last.req.name));
        check_phase_ = 3;
        break;
      }
      case 3:
        check_note_filter_ = !changed_text_.empty();
        check_measure("filter");
        filter_->SetValue("");
        check_phase_ = 4;
        break;
      case 4:
        check_measure("filter_cleared");
        if (!list_->rows.empty()) {
          list_->rows[0].checked = false;
          list_->on_toggle(0);
          list_->Refresh();
        }
        check_phase_ = 5;
        break;
      case 5:
        check_note_uncheck_ = !changed_text_.empty();
        check_measure("uncheck");
        if (!list_->rows.empty()) {
          list_->rows[0].checked = true;
          list_->on_toggle(0);
          list_->Refresh();
        }
        check_phase_ = 6;
        break;
      case 6:  // the test edits a request on disk: the note says so
        if (changed_text_.Find("edited") == wxNOT_FOUND) return;
        check_measure("disk_edit");
        check_runs_ = runs;
        on_run(false);
        check_phase_ = 7;
        break;
      case 7: {
        if (runs <= check_runs_) return;
        check_measure("run_again");
        update_counts();  // what the first keystroke in a field does
        check_measure("keystroke");
        check_note_after_ = !changed_text_.empty();
        // The run-order chips and the request list: where they are in the sidebar's order, with
        // the capture-order hint shown (the last request captures what the first reads).
        check_chips_ = chips_now();
        check_hint_ = order_hint_ && order_hint_->IsShown() && order_hint_->GetForegroundColour() == rv::Theme::get().warn;
        check_write("{\"check\":\"chips\",\"step\":\"sidebar_order\",\"up\":" + std::to_string(check_chips_[0]) +
                    ",\"down\":" + std::to_string(check_chips_[2]) + ",\"list\":" + std::to_string(check_chips_[4]) +
                    ",\"hint\":" + (check_hint_ ? "true" : "false") + "}");
        // The capturing request one step up, as the up chip does: the hint clears and the
        // reset chip becomes active.
        if (!visible_.empty()) list_->SetSelection(static_cast<int>(visible_.size() - 1));
        on_chip(kUp);
        check_phase_ = 8;
        break;
      }
      case 8:
        if (!saved_order_) return;
        chips_measure("reordered");
        check_hint_after_ = order_hint_ && order_hint_->IsShown() && order_hint_->GetForegroundColour() == rv::Theme::get().warn;
        reset_order();
        check_phase_ = 9;
        break;
      case 9: {
        if (saved_order_) return;
        check_timer_.Stop();
        chips_measure("reset");
        check_write(std::string("{\"check\":\"done\",\"moved\":") + std::to_string(check_moved_) +
                    ",\"chips_moved\":" + std::to_string(check_chips_moved_) +
                    ",\"note_filter\":" + (check_note_filter_ ? "true" : "false") +
                    ",\"note_uncheck\":" + (check_note_uncheck_ ? "true" : "false") +
                    ",\"note_after\":" + (check_note_after_ ? "true" : "false") +
                    ",\"hint_before\":" + (check_hint_ ? "true" : "false") +
                    ",\"hint_after\":" + (check_hint_after_ ? "true" : "false") + "}");
        TerminateProcess(GetCurrentProcess(), 0);
        break;
      }
      default:
        break;
    }
  }
  // Where a click lands on the up / down chips (first and last x of each, in the left panel, from
  // real hit tests on the chip bar) and the request list's top: {up0, up1, down0, down1, list}.
  std::array<int, 5> chips_now() {
    std::array<int, 5> r{-1, -1, -1, -1, -1};
    relayout_left();
    left_chips_->Refresh(false);
    left_chips_->Update();  // the chip bar lays its chips out when it paints
    const wxSize sz = left_chips_->GetClientSize();
    const int x0 = left_chips_->GetPosition().x;
    probing_ = true;
    for (int x = 0; x < sz.x; ++x) {
      wxMouseEvent e(wxEVT_LEFT_DOWN);
      e.SetPosition(wxPoint(x, sz.y / 2));
      e.SetEventObject(left_chips_);
      probe_hit_ = -1;
      left_chips_->GetEventHandler()->ProcessEvent(e);
      for (int k = 0; k < 2; ++k)
        if (probe_hit_ == (k ? kDown : kUp)) {
          if (r[k * 2] < 0) r[k * 2] = x0 + x;
          r[k * 2 + 1] = x0 + x;
        }
    }
    probing_ = false;
    r[4] = list_->GetPosition().y;
    return r;
  }
  void chips_measure(const char* step) {
    const auto now = chips_now();
    int moved = 0;
    for (size_t i = 0; i < now.size(); ++i) moved = std::max(moved, std::abs(now[i] - check_chips_[i]));
    if (now[0] < 0 || now[2] < 0) moved = std::max(moved, 9999);  // a chip could not be hit at all
    check_chips_moved_ = std::max(check_chips_moved_, moved);
    check_write(std::string("{\"check\":\"chips\",\"step\":\"") + step + "\",\"up\":" + std::to_string(now[0]) +
                ",\"down\":" + std::to_string(now[2]) + ",\"list\":" + std::to_string(now[4]) +
                ",\"moved\":" + std::to_string(moved) + "}");
  }
  bool probing_ = false;
  int probe_hit_ = -1;
  std::array<int, 5> check_chips_{};
  int check_chips_moved_ = 0;
  bool check_hint_ = false, check_hint_after_ = false, check_note_after_ = false;
  wxTimer check_timer_;
  std::wstring check_out_;
  Clock::time_point check_started_;
  int check_phase_ = 0, check_moved_ = 0;
  std::array<int, 3> check_base_{};
  bool check_note_filter_ = false, check_note_uncheck_ = false;
  size_t check_runs_ = 0;

  // WATT_RUNNER_OPEN_CHECK=<file> (with WATT_RUN_FOLDER; tests/test_runner.cpp): a 300 x 8 run,
  // then Open request on a result, close that tab with its X (a hover, press and release on the
  // close button, as the mouse does), Open request again and close it again. After each close the
  // tab strip may keep no pointer to the closed tab's button (wx 3.3.1 kept one and the next mouse
  // move wrote into freed memory: the notebook's sizers, AV in ~wxSizer on the next close). Writes
  // one JSON line per close and ends the process.
  struct HoverPeek : wxAuiTabCtrl {
    static const void* hover(wxAuiTabCtrl* t) { return static_cast<HoverPeek*>(t)->m_hoverButton; }
  };
  void start_open_check() {
    wxString out;
    if (!wxGetEnv("WATT_RUNNER_OPEN_CHECK", &out) || out.empty()) return;
    check_out_ = out.ToStdWstring();
    check_started_ = Clock::now();
    check_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { open_check_step(); });
    check_timer_.Start(50);
  }
  wxAuiNotebook* book() const {
    wxWindow* w = GetParent();
    while (w && !dynamic_cast<wxAuiNotebook*>(w)) w = w->GetParent();
    return dynamic_cast<wxAuiNotebook*>(w);
  }
  // Closes page with the mouse on its X: motion onto the button, left down, left up, then the
  // mouse moves on. Returns {hovered before, hover pointer left after, page gone}.
  std::array<bool, 3> close_with_x(wxAuiNotebook* nb, wxWindow* page) {
    std::array<bool, 3> r{false, true, false};
    wxAuiTabCtrl* tc = nullptr;
    int idx = -1;
    if (!nb->FindTab(page, &tc, &idx) || !tc) return r;
    wxRect rc;
    for (const auto& b : tc->GetPage(static_cast<size_t>(idx)).buttons)
      if (b.id == wxAUI_BUTTON_CLOSE) rc = b.rect;
    if (rc.IsEmpty()) return r;
    const wxPoint at(rc.x + rc.width / 2, rc.y + rc.height / 2);
    const auto send = [tc](wxEventType type, wxPoint p) {
      wxMouseEvent e(type);
      e.SetPosition(p);
      e.m_leftDown = type == wxEVT_LEFT_DOWN;
      e.SetEventObject(tc);
      tc->GetEventHandler()->ProcessEvent(e);
    };
    send(wxEVT_MOTION, at);
    r[0] = HoverPeek::hover(tc) != nullptr;
    send(wxEVT_LEFT_DOWN, at);
    send(wxEVT_LEFT_UP, at);
    r[1] = HoverPeek::hover(tc) != nullptr;
    r[2] = nb->GetPageIndex(page) == wxNOT_FOUND;
    send(wxEVT_MOTION, wxPoint(at.x + 1, at.y));  // the next move (wrote into the freed button)
    send(wxEVT_MOTION, wxPoint(2, 2));
    return r;
  }
  RequestTab* tab_of(wxAuiNotebook* nb, const fs::path& f) const {
    for (size_t i = 0; i < nb->GetPageCount(); ++i)
      if (auto* t = dynamic_cast<RequestTab*>(nb->GetPage(i)); t && t->file() == f) return t;
    return nullptr;
  }
  void open_check_step() {
    const bool idle = items_ && detail_ && !run_ && !preparing_ && left_stage_ >= kLeftBuilt;
    if (std::chrono::duration<double>(Clock::now() - check_started_).count() > 60) {
      check_write("{\"check\":\"timeout\",\"phase\":" + std::to_string(check_phase_) + "}");
      TerminateProcess(GetCurrentProcess(), 3);
    }
    if (!idle) return;
    wxAuiNotebook* nb = book();
    if (!nb) return;
    switch (check_phase_) {
      case 0:
        if (items_->empty()) return;
        iters_->set_value(300);
        par_->set_value(8);
        check_runs_ = run_history()[key_].size();
        on_run(false);
        check_phase_ = 1;
        break;
      case 1:
      case 4: {  // the run is done (phase 1), or back from the first close: Open request on a result
        if (check_phase_ == 1 && run_history()[key_].size() <= check_runs_) return;
        if (check_phase_ == 1)
          check_write("{\"check\":\"ran\",\"steps\":" + std::to_string(steps_.size()) + "}");
        StepPtr pick;
        for (auto it = steps_.rbegin(); it != steps_.rend() && !pick; ++it)
          if ((*it)->sent && !file_of(**it).empty()) pick = *it;
        if (!pick) return;
        check_file_ = file_of(*pick);
        nb->SetSelection(static_cast<size_t>(nb->GetPageIndex(page_window())));
        open_step(pick);
        check_wait_ = 0;
        check_phase_ = check_phase_ == 1 ? 2 : 5;
        break;
      }
      case 2:
      case 5: {  // the tab is there with the run's response: close it with its X
        RequestTab* t = tab_of(nb, check_file_);
        if (!t || !t->has_response_view() || ++check_wait_ < 20) return;
        const auto r = close_with_x(nb, t);
        check_write(std::string("{\"check\":\"close\",\"n\":") + (check_phase_ == 2 ? "1" : "2") +
                    ",\"hovered\":" + (r[0] ? "true" : "false") + ",\"hover_left\":" + (r[1] ? "true" : "false") +
                    ",\"closed\":" + (r[2] ? "true" : "false") + "}");
        check_phase_ = check_phase_ == 2 ? 3 : 6;
        break;
      }
      case 3:  // back to the runner, as the user clicks its tab
        nb->SetSelection(static_cast<size_t>(nb->GetPageIndex(page_window())));
        check_phase_ = 4;
        break;
      case 6:
        check_timer_.Stop();
        check_write("{\"check\":\"done\",\"pages\":" + std::to_string(nb->GetPageCount()) + "}");
        TerminateProcess(GetCurrentProcess(), 0);
        break;
      default:
        break;
    }
  }
  // This runner's page in the notebook (the notebook's child that holds it).
  wxWindow* page_window() {
    wxWindow* w = this;
    while (w->GetParent() && !dynamic_cast<wxAuiNotebook*>(w->GetParent())) w = w->GetParent();
    return w;
  }
  fs::path check_file_;
  int check_wait_ = 0;

  // Window-wide keys (also called from the lists, whose wxWANTS_CHARS keeps some keys from the
  // frame's char hook). True when handled.
  bool on_key(wxKeyEvent& e) {
    const int k = e.GetKeyCode();
    const int mods = e.GetModifiers();
    const bool enter = k == WXK_RETURN || k == WXK_NUMPAD_ENTER;
    if (k == WXK_ESCAPE && mods == 0 && run_) {
      stop_run();
      return true;
    }
    if (k == WXK_ESCAPE && mods == 0 && (scope_item_ != kNoScope || !scope_key_.empty()) && FindFocus() == table_) {
      clear_scope();
      return true;
    }
    if (mods == wxMOD_CONTROL && enter) {
      on_run(false);
      return true;
    }
    if (mods == (wxMOD_CONTROL | wxMOD_SHIFT) && enter) {
      on_run(true);
      return true;
    }
    if (mods == wxMOD_CONTROL && k == 'W') {
      close_tab();
      return true;
    }
    if (mods == wxMOD_CONTROL && k == 'A') {
      // Ctrl+A in a number field selects its text (the native spin control does not).
      wxWindow* f = FindFocus();
      for (NumberField* s : {iters_, delay_, par_, dur_, ramp_})
        if (s && f && f == s) {
          s->SetSelection(-1, -1);
          return true;
        }
    }
    return false;
  }

  // The two sides, empty: build() fills the left one, build_right() the right one.
  void build_shell() {
    const auto& th = rv::Theme::get();
    auto* root = new wxBoxSizer(wxHORIZONTAL);
    // The settings scroll when the window is too short for them (1280 x 800), so the request
    // list keeps a few rows instead of one.
    auto* sc = new wxScrolled<wxPanel>(this);
    sc->SetScrollRate(0, FromDIP(12));
    sc->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_DEFAULT);
    left_ = sc;
    left_->SetBackgroundColour(th.bg);
    left_->SetMinSize(FromDIP(wxSize(340, -1)));
    root->Add(left_, 0, wxEXPAND);
    // Collapsed: a narrow rail with the button that opens the settings again.
    rail_ = new rv::ChipBar(this, [this](int) { set_collapsed(false); });
    rail_->SetBackgroundColour(th.bg);
    rail_->add(1, "\xC2\xBB", "Show the run settings", -1);
    rail_->SetMinSize(FromDIP(wxSize(44, 26)));
    rail_->SetMaxSize(FromDIP(wxSize(44, 26)));
    rail_->Hide();
    root->Add(rail_, 0, wxTOP, FromDIP(8));
    auto* sep = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(1, -1));
    sep->SetBackgroundColour(th.grid);
    root->Add(sep, 0, wxEXPAND);
    right_ = new wxPanel(this);
    right_->SetBackgroundColour(th.bg);
    root->Add(right_, 1, wxEXPAND);
    SetSizer(root);
    {
      wxConfigBase* c = wxConfigBase::Get();
      bool col = false;
      if (c && c->Read("Runner/Collapsed", &col, false) && col) set_collapsed(true, false);
    }
    // A narrow window gives the settings less room (they keep at least 304 px).
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      const int w = GetClientSize().x;
      const int want = w >= FromDIP(1150) ? FromDIP(340) : std::clamp(w * 31 / 100, FromDIP(304), FromDIP(340));
      CallAfter([this] { fit_list_height(); });  // the panel's height: room for the request list
      if (left_->GetMinSize().x != want) {
        left_->SetMinSize(wxSize(want, -1));
        if (order_hint_ && order_hint_->IsShown()) {
          hint_for_.clear();  // laid out again for the new width
          CallAfter([this] { update_order_hint(); });
        }
      }
    });
  }

  void set_collapsed(bool on, bool remember = true) {
    collapsed_ = on;
    left_->Show(!on);
    rail_->Show(on);
    Layout();
    if (remember)
      if (wxConfigBase* c = wxConfigBase::Get()) c->Write("Runner/Collapsed", on);
    if (on && run_btn_ && FindFocus() && left_->IsDescendant(FindFocus()) && table_) table_->SetFocus();
  }

  // The request list is tall enough for every request of the folder (up to 10 rows; at least 5)
  // when the settings below it still fit the panel, else as tall as fits. A filter does not
  // change it (it depends on the folder and the panel's height), so nothing below the list moves
  // while typing.
  void fit_list_height() {
    if (!list_ || !left_sizer_) return;
    const int full = FromDIP(24 * static_cast<int>(std::clamp<size_t>(order_.size(), 5, 10)) + 4);
    const int least = FromDIP(24 * 5 + 4);
    const int cur = list_->GetMinSize().y;
    // The rest of the panel's content at its smallest, without the list.
    const int others = left_sizer_->GetMinSize().y - std::max(cur, 0);
    // The panel scrolls in steps of 12 px: its content rounded up to a step must fit, else a
    // scroll bar shows (and narrows every field) for a few pixels.
    const int step = FromDIP(12);
    const int room = left_->GetSize().y / step * step - others;
    const int want = std::max(least, std::min(full, room));
    if (want != cur) {
      list_->SetMinSize(wxSize(-1, want));
      relayout_left();
    }
    // Still taller than the panel (wrapped labels measured at the new width): shrink by that much.
    if (const int over = (left_sizer_->GetMinSize().y + step - 1) / step * step - left_->GetSize().y;
        over > 0 && list_->GetMinSize().y > least) {
      list_->SetMinSize(wxSize(-1, std::max(least, list_->GetMinSize().y - over)));
      relayout_left();
    }
  }

  void relayout_left() {
    static_cast<wxScrolled<wxPanel>*>(left_)->FitInside();
    left_->Layout();
  }

  // ---- left: what to run
  // The left side in two steps (two timer ticks, each short): the title and the request list,
  // then the run settings below it.
  void build() {
    const auto& th = rv::Theme::get();
    const wxColour lbg = th.bg;  // the chip bars and lists paint the theme background
    auto* ls = new wxBoxSizer(wxVERTICAL);
    left_sizer_ = ls;
    const int pad = FromDIP(16);
    auto label = [&](const wxString& s, bool dim = true, float scale = 0.9f, bool bold = true) {
      auto* t = new wxStaticText(left_, wxID_ANY, s);
      wxFont f = t->GetFont().Scaled(scale);
      if (bold) f = f.Bold();
      t->SetFont(f);
      t->SetForegroundColour(dim ? th.dim : th.fg);
      return t;
    };
    {
      auto* top = new wxBoxSizer(wxHORIZONTAL);
      top->Add(label("COLLECTION RUNNER", true, 0.85f), 1, wxALIGN_CENTER_VERTICAL);
      auto* hide = new rv::ChipBar(left_, [this](int) { set_collapsed(true); });
      hide->SetBackgroundColour(lbg);
      hide->add(1, "\xC2\xAB", "Hide the run settings (more room for the results); Ctrl+Enter still runs", -1, true);
      hide->SetMinSize(FromDIP(wxSize(44, 26)));
      hide->SetMaxSize(FromDIP(wxSize(44, 26)));
      top->Add(hide, 0, wxALIGN_CENTER_VERTICAL);
      ls->Add(top, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad - FromDIP(4));
    }
    ls->Add(label(U(name_s_), false, 1.35f), 0, wxLEFT | wxRIGHT | wxTOP, pad);
    ls->AddSpacer(FromDIP(4));
    // The environment of the run (Postman's runner dropdown, Bruno's selector on the runner tab):
    // the run's own choice; the workspace's active environment is not changed.
    env_chips_ = new rv::ChipBar(left_, [this](int id) { on_chip(id); });
    env_chips_->SetBackgroundColour(lbg);
    env_chips_->add(kEnvPick, "", "Environment for this run (the workspace's active one is not changed)", -1, false);
    ls->Add(env_chips_, 0, wxEXPAND | wxLEFT | wxRIGHT, pad - FromDIP(4));
    update_env_label();
    ls->AddSpacer(FromDIP(8));

    auto* reqrow = new wxBoxSizer(wxHORIZONTAL);
    req_title_ = label("REQUESTS", true, 0.85f);
    reqrow->Add(req_title_, 0, wxALIGN_CENTER_VERTICAL);
    left_chips_ = new rv::ChipBar(left_, [this](int id) { on_chip(id); });
    left_chips_->SetBackgroundColour(lbg);
    left_chips_->add(kAll, "All", "Check every request shown", -1, true);
    left_chips_->add(kNone, "None", "Uncheck every request shown", -1, true);
    left_chips_->add(kUp, "\xE2\x96\xB2", "Move the selected request up (Alt+Up)", -1, true);
    left_chips_->add(kDown, "\xE2\x96\xBC", "Move the selected request down (Alt+Down)", -1, true);
    left_chips_->add(kOrderReset, "\xE2\x86\xBA", "Run in the sidebar's order again (removes this folder's entries from run-order.json); "
                     "dimmed while the runner already follows the sidebar",
                     -1, true);
    // Always laid out (faint and disabled while the folder follows the sidebar order): the row is
    // right-aligned, so showing it after the first move would shift the arrows under the mouse.
    left_chips_->set_enabled(kOrderReset, saved_order_);
    // Quick clicks on the arrows arrive as a double click in place of the second press: each one
    // still moves the request a step (four clicks, four places).
    left_chips_->Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent& e) {
      wxMouseEvent down(e);
      down.SetEventType(wxEVT_LEFT_DOWN);
      left_chips_->GetEventHandler()->ProcessEvent(down);
    });
    reqrow->Add(left_chips_, 1, wxEXPAND | wxLEFT, FromDIP(6));
    ls->Add(reqrow, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);
    ls->AddSpacer(FromDIP(4));
    filter_ = new wxTextCtrl(left_, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxBORDER_THEME);
    filter_->SetHint("Filter by tag or name: smoke -slow");
    filter_->SetToolTip("Tags are #words in a request's Docs and its folder names. A word must match; -word excludes.");
    filter_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { fill_list(); });
    ls->Add(filter_, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);
    ls->AddSpacer(FromDIP(6));
    // A request using {{var}} before the request that captures var (Postman / Bruno just send it
    // unresolved): said before the run, not found afterwards in a failed row. Above the list, so
    // showing or hiding it changes the list's height and nothing below it moves.
    order_hint_ = label("", false, 0.9f, false);
    order_hint_->SetForegroundColour(th.warn);
    order_hint_->Hide();
    ls->Add(order_hint_, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);
    ls->AddSpacer(FromDIP(6));
    list_ = new RequestList(left_);
    list_->SetMinSize(wxSize(-1, FromDIP(24 * 5 + 4)));
    list_->on_toggle = [this](size_t n) {
      if (n < visible_.size()) checked_[visible_[n]] = list_->rows[n].checked;
      update_counts();
    };
    list_->on_move = [this](size_t n, int d) { move_sel(n, d); };
    list_->on_drop = [this](size_t from, size_t before) { drop_row(from, before); };
    list_->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) {
      const int s = list_->GetSelection();
      if (s != wxNOT_FOUND && static_cast<size_t>(s) < visible_.size() && items_)
        open_file((*items_)[order_[visible_[static_cast<size_t>(s)]]].req.file);
    });
    ls->Add(list_, 1, wxEXPAND | wxLEFT | wxRIGHT, pad);
    // Said under the list when this folder has a saved run order, which the sidebar does not
    // follow (Bruno shows one order in both places; here they can differ).
    ls->AddSpacer(FromDIP(4));
    // Always there, one line high (empty without a saved order): the note appearing after the
    // first move must not move the settings below it under the cursor.
    order_note_ = label("", true, 0.85f, false);
    order_note_->SetMinSize(wxSize(-1, lines_height(order_note_, 1)));
    ls->Add(order_note_, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);
    set_saved_order(saved_order_);
    left_->SetSizer(ls);
  }

  void edge_note_done() {
    if (!order_note_) return;
    order_note_->SetForegroundColour(rv::Theme::get().dim);
    set_saved_order(saved_order_);
  }

  // Whether this runner's folder has entries in run-order.json (a worker reads it).
  void set_saved_order(bool on) {
    saved_order_ = on;
    if (!order_note_) return;
    if (left_chips_) left_chips_->set_enabled(kOrderReset, on);
    set_fixed_text(order_note_,
                   on ? wxString::FromUTF8("Custom run order, not the sidebar's \xC2\xB7 \xE2\x86\xBA above goes back to it") : wxString(), 1,
                   on ? wxString("Move a request with Alt+Up / Alt+Down, the arrows above or by dragging. The order is "
                                 "kept for this runner and the command line runner (run-order.json); the sidebar keeps "
                                 "its own. The circular arrow above removes it: the runner then runs in the sidebar's order.")
                      : wxString());
  }
  static bool has_saved_order(const fs::path& root, const fs::path& folder) {
    const runner::FolderOrder o = runner::load_run_order(root);
    std::error_code ec;
    const fs::path base = folder.empty() || fs::equivalent(folder, root, ec) ? root
                          : fs::is_directory(folder, ec)                   ? folder
                                                                            : folder.parent_path();
    for (const auto& [dir, names] : o) {
      if (names.empty()) continue;
      const fs::path rel = dir.lexically_normal().lexically_relative(base.lexically_normal());
      if (!rel.empty() && *rel.begin() != "..") return true;
    }
    return false;
  }

  // part 0: the fields grid; part 1: the data file, check boxes, Run and the plan lines.
  void build_settings(int part) {
    const auto& th = rv::Theme::get();
    const wxColour lbg = th.bg;
    wxBoxSizer* ls = left_sizer_;
    const int pad = FromDIP(16);
    auto label = [&](const wxString& s, bool dim = true, float scale = 0.9f, bool bold = true) {
      auto* t = new wxStaticText(left_, wxID_ANY, s);
      wxFont f = t->GetFont().Scaled(scale);
      if (bold) f = f.Bold();
      t->SetFont(f);
      t->SetForegroundColour(dim ? th.dim : th.fg);
      return t;
    };
    auto field = [&](const wxString& s, const wxString& tip) {
      auto* t = new wxStaticText(left_, wxID_ANY, s);
      t->SetForegroundColour(th.fg);
      t->SetToolTip(tip);
      return t;
    };
    if (part == 1) {
      build_settings_rest(label, field, pad, lbg);
      return;
    }
    ls->AddSpacer(FromDIP(8));
    auto* grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(12));
    grid->AddGrowableCol(1);
    iters_ = new NumberField(left_, 1, 1, 1000000, FromDIP(wxSize(110, -1)));
    delay_ = new NumberField(left_, 0, 0, 600000, FromDIP(wxSize(110, -1)));
    par_ = new NumberField(left_, 1, 1, static_cast<int>(runner::RunControl::kSlots), FromDIP(wxSize(110, -1)));
    dur_ = new NumberField(left_, 0, 0, 86400, FromDIP(wxSize(110, -1)));
    ramp_ = new NumberField(left_, 0, 0, 3600, FromDIP(wxSize(110, -1)));
    iters_->SetToolTip("Up / Down or the mouse wheel change it");
    for (NumberField* s : {iters_, par_, dur_, ramp_}) s->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { update_counts(); });
    grid->Add(field("Iterations", "How many times the requests run (with a data file: one per row by default)"), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(iters_, 0, wxALIGN_RIGHT);
    grid->Add(field("Delay (ms)", "Pause before every request but the first"), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(delay_, 0, wxALIGN_RIGHT);
    grid->Add(field("Parallel", "Iterations run on this many workers at once (load mode). Captures chain within an "
                                "iteration; 1 runs everything in order."),
              0, wxALIGN_CENTER_VERTICAL);
    grid->Add(par_, 0, wxALIGN_RIGHT);
    grid->Add(field("Duration (s)", "Load profile: keep running iterations for this long (0: run the iterations above). "
                                    "Iterations above 1 then cap the run."),
              0, wxALIGN_CENTER_VERTICAL);
    grid->Add(dur_, 0, wxALIGN_RIGHT);
    grid->Add(field("Profile", "Load profile with Parallel > 1 (Postman's performance runner). Fixed: every worker from "
                               "the start. Ramp up: the workers join one by one over Ramp (s). Peak: ramp up, hold, "
                               "and ramp down over the last Ramp (s) of the Duration. Spike: a quarter of the workers "
                               "all through, all of them at once in the middle fifth of the Duration."),
              0, wxALIGN_CENTER_VERTICAL);
    prof_chips_ = new rv::ChipBar(left_, [this](int id) { set_profile(id - kProfFixed, true); });
    prof_chips_->SetBackgroundColour(lbg);
    prof_chips_->add(kProfFixed, "Fixed", "Every worker from the start", 0);
    prof_chips_->add(kProfRamp, "Ramp", "Ramp up: the workers join one by one over Ramp (s)", 0);
    prof_chips_->add(kProfPeak, "Peak", "Ramp up, hold, ramp down over the last Ramp (s) of the Duration", 0);
    prof_chips_->add(kProfSpike, "Spike", "Base load, then every worker at once in the middle fifth of the Duration", 0);
    prof_chips_->set_on(kProfFixed, true);
    grid->Add(prof_chips_, 0, wxEXPAND);
    grid->Add(field("Ramp (s)", "Load profile: the parallel workers start one after another over this time, so the load "
                                "grows linearly (Peak: and falls the same way at the end)"),
              0, wxALIGN_CENTER_VERTICAL);
    grid->Add(ramp_, 0, wxALIGN_RIGHT);
    ramp_->Bind(wxEVT_TEXT, [this](wxCommandEvent& e) {
      e.Skip();
      // A ramp typed while Fixed is a ramp-up profile (as the field alone meant before).
      if (profile_ == 0 && ramp_->value() > 0) set_profile(1, false);
    });
    thresh_ = new wxTextCtrl(left_, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(170, -1)), wxBORDER_THEME);
    thresh_->SetHint("p95<200 err<1%");
    thresh_->SetToolTip("Pass / fail criteria for the whole run (Postman's performance thresholds): avg, min, max, p50, "
                        "p90, p95, p99 in ms (or 2s), err in % of the sends, rps in sends per second. "
                        "Example: p95<200 p99<500 err<1% rps>=50");
    grid->Add(field("Thresholds", "Pass / fail criteria for the run, checked live and at the end"), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(thresh_, 0, wxALIGN_RIGHT);
    ls->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);
  }

  template <class Label, class Field>
  void build_settings_rest(Label& label, Field& field, int pad, const wxColour& lbg) {
    const auto& th = rv::Theme::get();
    wxBoxSizer* ls = left_sizer_;
    ls->AddSpacer(FromDIP(8));
    auto* drow = new wxBoxSizer(wxHORIZONTAL);
    auto* dl = field("Data file", "CSV with a header row, or a JSON array of objects: each row's columns become {{vars}} for one iteration");
    drow->Add(dl, 0, wxALIGN_CENTER_VERTICAL);
    data_chips_ = new rv::ChipBar(left_, [this](int id) { on_chip(id); });
    data_chips_->SetBackgroundColour(lbg);
    data_chips_->add(kDataPick, "Choose CSV / JSON...", "Pick a data file", -1, true);
    data_chips_->add(kDataClear, "\xC3\x97", "No data file", -1, true);
    data_chips_->add(kDataPreview, "Preview", "Show the rows of the data file", -1, true);
    data_chips_->set_visible(kDataClear, false);
    data_chips_->set_visible(kDataPreview, false);
    drow->Add(data_chips_, 1, wxEXPAND | wxLEFT, FromDIP(6));
    ls->Add(drow, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);
    data_label_ = label("", true, 0.95f, false);
    // One line, always (empty without a data file): choosing a file moves nothing below.
    data_label_->SetMinSize(wxSize(-1, lines_height(data_label_, 1)));
    ls->AddSpacer(FromDIP(2));
    ls->Add(data_label_, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);
    ls->AddSpacer(FromDIP(8));
    bail_ = new wxCheckBox(left_, wxID_ANY, "Stop on first failure");
    keep_ = new wxCheckBox(left_, wxID_ANY, "Keep variable values");
    keep_->SetValue(true);
    keep_->SetToolTip("After the run, captured values stay in the workspace (runtime vars, and environment / globals "
                      "captures are saved), so the next request you send uses them.");
    bail_->SetToolTip("End the run at the first request that fails (Bruno's bail).");
    cookies_ = new wxCheckBox(left_, wxID_ANY, "Carry cookies between requests");
    cookies_->SetValue(true);
    cookies_->SetToolTip("A Set-Cookie from one response is sent by the later requests of the run (Postman's runner "
                         "does the same). Off: each request sends only its own Cookie header. Stored cookies are never "
                         "used, and the run's cookies are dropped when it ends.");
    for (auto* c : {bail_, cookies_, keep_}) {
      c->SetForegroundColour(th.fg);
      ls->Add(c, 0, wxLEFT | wxRIGHT, pad);
      ls->AddSpacer(FromDIP(10));
    }
    ls->AddSpacer(FromDIP(8));
    run_btn_ = new AccentButton(left_, "Run");
    run_btn_->on_click = [this] { on_run(false); };
    run_btn_->Disable();
    run_btn_->SetToolTip("Run (Ctrl+Enter); Esc stops; Ctrl+Shift+Enter reruns the failed requests");
    ls->Add(run_btn_, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);
    ls->AddSpacer(FromDIP(6));
    // The plan line and the "Plan changed" note under it have fixed heights (two lines each, empty
    // or not; longer text is cut, its whole in the tooltip): what they say changes with every
    // keystroke in a field or the filter, a check box and a request edited on disk, and the list
    // above takes what the panel has left, so any change in their height would move every field.
    plan_label_ = label("", true, 0.9f, false);
    plan_label_->SetMinSize(wxSize(-1, lines_height(plan_label_, 2)));
    ls->Add(plan_label_, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);
    ls->AddSpacer(FromDIP(4));
    changed_label_ = label("", false, 0.9f, false);
    changed_label_->SetForegroundColour(th.warn);
    changed_label_->SetMinSize(wxSize(-1, lines_height(changed_label_, 2)));
    ls->Add(changed_label_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, pad);
    restore_settings();
  }

  void build_right() {
    const auto& th = rv::Theme::get();
    wxPanel* right = right_;
    auto* rs = new wxBoxSizer(wxVERTICAL);
    summary_ = new SummaryStrip(right);
    rs->Add(summary_, 0, wxEXPAND | wxTOP, FromDIP(6));
    auto* bar = new wxBoxSizer(wxHORIZONTAL);
    bar_ = new rv::ChipBar(right, [this](int id) { on_chip(id); });
    bar_->add(kFAll, "All", "Every result", 0);
    bar_->add(kFPassed, "Passed", "Passed results", 0);
    bar_->add(kFFailed, "Failed", "Failed results", 0);
    bar_->add(kFSkipped, "Skipped", "Stopped, or not run because the run ended first", 0);
    bar_->add(kVResults, "Results", "Every step in run order", 1);
    bar_->add(kVRequests, "By request", "Per request across iterations: count, failures, avg, min, p50, p95, max, and the "
                                        "change against the previous run",
              1);
    bar_->add(kVTime, "Over time", "Throughput, failures and p50 / p95 response time over the run (load mode)", 1);
    bar_->add(kVFail, "Failures", "Failed checks grouped by request, check and message (numbers ignored), with counts "
                                  "and first / last iteration; click a group to list its results",
              1);
    bar_->set_on(kFAll, true);
    bar_->set_on(kVResults, true);
    bar_->add(kRunAgain, "Run again", "Run the same requests again (Ctrl+Enter)", -1, true);
    bar_->add(kRerunFailed, "Rerun failed", "Run the failed requests again: each iteration that had a failure, with its data row, plus the "
                                          "requests whose captures they use (Ctrl+Shift+Enter)", -1, true);
    bar_->add(kHistory, "History \xE2\x96\xBE", "Earlier runs of this folder in this session", -1, true);
    // One menu for the reports (HTML, JSON, JUnit): the bar keeps Rerun failed and History in
    // view with a filter or cause chip shown.
    bar_->add(kHtml, "Export \xE2\x96\xBE", "Save the results: an HTML report, JSON, or JUnit XML for CI", -1, true);
    bar_->add(kJson, "JSON", "Export every result as JSON", -1, true);
    bar_->add(kJunit, "JUnit", "Export JUnit XML for CI", -1, true);
    bar_->add(kReset, "Reset", "Clear the results and show the run's plan again (the run stays in History)", -1, true);
    for (int id : {kRunAgain, kRerunFailed, kHistory, kHtml, kJson, kJunit, kReset}) bar_->set_enabled(id, false);
    bar_->set_visible(kJson, false);
    bar_->set_visible(kJunit, false);
    bar->Add(bar_, 1, wxEXPAND);
    rs->Add(bar, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));
    // Always one line high (empty or not): a message never moves the rows under the cursor.
    // The status line, with the scope chip ("x Fail lookup: expected gRPC OK") at its start while
    // results are narrowed to one request or failure: on its own row, so the bar above keeps
    // every action in view. One height with or without the chip: nothing below moves.
    scope_row_ = new wxBoxSizer(wxHORIZONTAL);
    scope_row_->Add(0, FromDIP(24));
    scope_bar_ = new rv::ChipBar(right, [this](int id) { on_chip(id); });
    scope_bar_->SetBackgroundColour(th.bg);
    scope_bar_->add(kScope, "", "Results are narrowed to this request or failure: click to show every result again", -1);
    scope_bar_->Hide();
    scope_row_->Add(scope_bar_, 0, wxALIGN_CENTER_VERTICAL);
    scope_row_->AddSpacer(FromDIP(8));
    status_ = new wxStaticText(right, wxID_ANY, " ", wxDefaultPosition, wxDefaultSize,
                               wxST_NO_AUTORESIZE | wxST_ELLIPSIZE_END);
    status_->SetForegroundColour(th.dim);
    status_->SetMinSize(wxSize(-1, status_->GetCharHeight()));
    scope_row_->Add(status_, 1, wxALIGN_CENTER_VERTICAL);
    rs->Add(scope_row_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    split_ = new wxSplitterWindow(right, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSP_LIVE_UPDATE | wxSP_NOBORDER | wxSP_THIN_SASH);
    split_->SetBackgroundColour(th.grid);
    tpanel_ = new wxPanel(split_);
    tpanel_->SetBackgroundColour(th.bg);
    auto* tsz = new wxBoxSizer(wxVERTICAL);
    header_ = new GridHeader(tpanel_, &cols_);
    table_ = new ResultsList(tpanel_, &view_, &cols_);
    stats_ = new StatsList(tpanel_);
    tsz->Add(header_, 0, wxEXPAND);
    tsz->Add(table_, 1, wxEXPAND);
    tsz->Add(stats_, 1, wxEXPAND);
    stats_->Hide();
    chart_ = new TimeChart(tpanel_);
    tsz->Add(chart_, 1, wxEXPAND);
    chart_->Hide();
    fails_ = new FailList(tpanel_);
    tsz->Add(fails_, 1, wxEXPAND);
    fails_->Hide();
    fails_->on_pick = [this](const runner::FailureGroup& g) { scope_to_failure(g); };
    fails_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
      const bool enter = e.GetKeyCode() == WXK_RETURN || e.GetKeyCode() == WXK_NUMPAD_ENTER;
      const int sel = fails_->GetSelection();
      if (enter && e.GetModifiers() == 0 && sel != wxNOT_FOUND) {
        if (const runner::FailureGroup* g = fails_->row(static_cast<size_t>(sel))) {
          const runner::FailureGroup copy = *g;
          scope_to_failure(copy);
        }
        return;
      }
      if (on_key(e)) return;
      e.Skip();
    });
    tpanel_->SetSizer(tsz);
    header_->on_sort = [this] { apply_sort(true); };
    header_->on_resize = [this] { table_->Refresh(); };
    table_->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      header_->set_scrollbar(table_->GetSize().x - table_->GetClientSize().x);
      table_->Refresh();
    });
    table_->Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
      e.Skip();
      const int h = table_->VirtualHitTest(e.GetPosition().y);
      const size_t n = h == wxNOT_FOUND ? static_cast<size_t>(-1) : static_cast<size_t>(h);
      if (n != table_->hover_) {
        table_->hover_ = n;
        table_->Refresh();
      }
    });
    table_->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) {
      e.Skip();
      table_->hover_ = static_cast<size_t>(-1);
      table_->Refresh();
    });
    // wxWANTS_CHARS: Enter and Ctrl+Enter reach the list itself, not the frame's char hook.
    table_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
      const bool enter = e.GetKeyCode() == WXK_RETURN || e.GetKeyCode() == WXK_NUMPAD_ENTER;
      if (enter && e.GetModifiers() == 0) return open_step(shown_);
      if (on_key(e)) return;
      e.Skip();
    });
    table_->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) { open_step(shown_); });
    table_->Bind(wxEVT_CONTEXT_MENU, [this](wxContextMenuEvent& e) { row_menu(e.GetPosition()); });
    stats_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
      const bool enter = e.GetKeyCode() == WXK_RETURN || e.GetKeyCode() == WXK_NUMPAD_ENTER;
      if (enter && e.GetModifiers() == 0 && stats_->GetSelection() > 0) {
        scope_to_stats_row(static_cast<size_t>(stats_->GetSelection()));
        return;
      }
      if (on_key(e)) return;
      e.Skip();
    });
    // A click (or Enter) on a request row: Results narrowed to that request.
    stats_->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) {
      e.Skip();
      const int h = stats_->VirtualHitTest(e.GetY());
      if (h > 0) CallAfter([this, h] { scope_to_stats_row(static_cast<size_t>(h)); });
    });
    list_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
      if (on_key(e)) return;
      e.Skip();
    });
    run_btn_->on_key = [this](wxKeyEvent& e) { return on_key(e); };
    split_->Initialize(tpanel_);  // the detail pane joins on the next step (build_detail)
    split_->SetSashGravity(0.6);
    split_->SetMinimumPaneSize(FromDIP(120));
    rs->Add(split_, 1, wxEXPAND | wxLEFT, FromDIP(1));
    right->SetSizer(rs);
    table_->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) {
      follow_ = false;
      if (detail_) show_selected();
    });
    split_->Bind(wxEVT_SPLITTER_SASH_POS_CHANGED, [this](wxSplitterEvent& e) {
      e.Skip();
      const int h = split_->GetClientSize().y;
      if (h > 0 && e.GetSashPosition() > 0)
        if (wxConfigBase* c = wxConfigBase::Get())
          c->Write("Runner/SplitPct", std::clamp(static_cast<long>(e.GetSashPosition()) * 100 / h, 15L, 85L));
      CallAfter([this] {
        const int sel = table_->GetSelection();
        if (sel != wxNOT_FOUND) table_->ensure_visible(static_cast<size_t>(sel));
      });
    });
    update_filter_chips();
  }

  // The results side's detail pane (Response / Headers / Request / Timing / Tests ...), one step
  // after the rest of that side: the runner shows its list and summary first.
  void build_detail() {
    detail_ = new runner_ui::DetailPane(split_);
    detail_->on_open = [this] { open_step(shown_); };
    detail_->on_run_again = [this] { on_run(false); };
    detail_->on_key = [this](wxKeyEvent& e) { return on_key(e); };
    split_->SplitHorizontally(tpanel_, detail_, 0);
    // The divider where the user last left it (a share of the height, for every runner), else
    // ~22 result rows at 1600 x 1000. A drag keeps the selected row in view.
    const int h = split_->GetClientSize().y;
    long pct = 58;
    if (wxConfigBase* c = wxConfigBase::Get()) c->Read("Runner/SplitPct", &pct, 58L);
    pct = std::clamp(pct, 15L, 85L);
    if (h > 0) split_->SetSashPosition(static_cast<int>(h * pct / 100));
    else
      CallAfter([this, pct] {
        if (const int ch = split_->GetClientSize().y; ch > 0) split_->SetSashPosition(static_cast<int>(ch * pct / 100));
      });
  }

  void update_env_label() {
    env_chips_->set_label(kEnvPick, (env_.empty() ? std::string("No environment") : "Environment: " + env_) + "  \xE2\x96\xBE");
    env_chips_->Refresh();
  }

  void env_menu() {
    Workspace* ws = workspace_ ? workspace_() : nullptr;
    if (!ws || run_) return;
    wxMenu m;
    m.AppendCheckItem(1000, "No environment")->Check(env_.empty());
    int id = 1001;
    std::vector<std::string> names;
    for (const auto& e : ws->environments()) {
      names.push_back(e.name);
      m.AppendCheckItem(id++, U(e.name + (e.name == ws->active_env ? "   (active in the workspace)" : "")))->Check(e.name == env_);
    }
    const int got = GetPopupMenuSelectionFromUser(m);
    if (got == 1000) env_.clear();
    else if (got > 1000 && static_cast<size_t>(got - 1001) < names.size()) env_ = names[static_cast<size_t>(got - 1001)];
    else return;
    update_env_label();
    relayout_left();
  }

  // ---- loading the folder (worker)
  // The run order is run-order.json only (what `watt --run` follows), never the sidebar's
  // per-user drag order: one order for the runner, its re-reads and the CLI.
  void load(const Workspace& ws) {
    auto wsc = std::make_shared<Workspace>(ws);
    auto link = link_;
    const fs::path folder = folder_;
    std::thread([wsc, link, folder] {
      auto errs = std::make_shared<std::vector<std::string>>();
      auto items = std::make_shared<std::vector<RunItem>>(runner::collect(*wsc, folder, errs.get()));
      const bool saved_order = has_saved_order(wsc->root(), folder);
      wxTheApp->CallAfter([link, items, errs, saved_order] {
        if (!link->frame) return;
        link->frame->set_saved_order(saved_order);
        link->frame->loaded(items, *errs);
      });
    }).detach();
  }

  // Watches the workspace on disk (an editor save, a file added / deleted / renamed, git pull,
  // run-order.json): a change under this runner's folder reloads its list.
  void watch() {
    auto link = link_;
    try {
      watcher_ = std::make_unique<WorkspaceWatcher>(root_, [link](WorkspaceWatcher::Batch b) {
        auto batch = std::make_shared<WorkspaceWatcher::Batch>(std::move(b));
        wxTheApp->CallAfter([link, batch] {
          if (link->frame) link->frame->files_changed(*batch);
        });
      });
    } catch (...) {
      watcher_.reset();
    }
  }

 public:
  void files_changed(const WorkspaceWatcher::Batch& batch) {
    const std::wstring base = run_base().lexically_relative(root_).generic_wstring();
    const bool whole = base.empty() || base == L".";
    bool hit = false;
    for (const auto& p : batch) {
      const std::wstring w = p.generic_wstring();
      if (w.empty() || whole || w == L"run-order.json" || w.ends_with(L"folder.json")) {
        hit = true;  // empty: too many changes to list
        break;
      }
      // Under the folder, the folder itself, or one of its parents (renamed / deleted).
      if ((w.size() > base.size() && w.compare(0, base.size(), base) == 0 && w[base.size()] == L'/') || w == base ||
          (base.size() > w.size() && base.compare(0, w.size(), w) == 0 && base[w.size()] == L'/')) {
        hit = true;
        break;
      }
    }
    if (!hit) return;
    if (run_ || preparing_) {
      reload_due_ = true;  // after the run: it sends what it started with
      return;
    }
    reload_timer_.StartOnce(120);
  }
  // Raising an already open runner (Run folder again) re-reads its requests.
  void refresh() {
    if (!run_ && !preparing_ && items_) reload_timer_.StartOnce(1);
  }

 private:
  // Re-reads the requests of the folder on a worker (a drag order not yet written is written
  // first), then merges them into the list: checks are kept per file. then: 0 only refresh, 1
  // start a run, 2 rerun the failed requests, from the fresh list.
  void reload(int then) {
    reload_timer_.Stop();
    reload_due_ = false;
    Workspace* ws = workspace_ ? workspace_() : nullptr;
    if (!ws || runner_key(ws->root(), {}) != runner_key(root_, {})) {
      if (then) {
        preparing_ = false;
        set_running(false);
        summary_->set(summary_now(false), "Not run", false);
        set_status_w("The workspace was closed or switched: open the runner again from the sidebar.");
      }
      return;
    }
    const uint64_t seq = ++reload_seq_;
    if (then) reload_then_ = then;
    auto wsc = std::make_shared<Workspace>(*ws);
    auto link = link_;
    const fs::path folder = folder_;
    const fs::path root = root_;
    std::shared_ptr<runner::FolderOrder> changed;
    if (!order_dirty_.empty()) {
      changed = std::make_shared<runner::FolderOrder>(std::move(order_dirty_));
      order_dirty_.clear();
      save_order_.Stop();
    }
    const uint64_t oseq = changed ? ++order_seq() : 0;
    std::thread([wsc, link, folder, root, changed, oseq, seq] {
      std::string oerr;
      bool saved = true;
      if (changed) saved = runner::save_run_order(root, *changed, oseq, &oerr);
      // An order write started just before (the debounce timer) lands first.
      for (int i = 0; i < 2000 && link->saving.load() > 0; ++i) Sleep(1);
      auto errs = std::make_shared<std::vector<std::string>>();
      auto items = std::make_shared<std::vector<RunItem>>(runner::collect(*wsc, folder, errs.get()));
      const bool saved_order = has_saved_order(root, folder);
      wxTheApp->CallAfter([link, items, errs, seq, saved, oerr, saved_order] {
        if (!link->frame) return;
        link->frame->set_saved_order(saved_order);
        if (!saved) link->frame->set_status_w(U("Could not save the run order: " + oerr));
        link->frame->reloaded(seq, items, *errs);
      });
    }).detach();
  }

  static std::atomic<uint64_t>& order_seq() {
    static std::atomic<uint64_t> seq{0};
    return seq;
  }

 public:
  void reloaded(uint64_t seq, std::shared_ptr<std::vector<RunItem>> items, const std::vector<std::string>& errs) {
    if (seq != reload_seq_) return;  // a newer reload is on its way
    if (!detail_) {
      pending_items_ = std::move(items);
      pending_errs_ = errs;
      return;
    }
    const int then = reload_then_;
    reload_then_ = 0;
    merge_items(std::move(items), errs);
    if (then) {
      preparing_ = false;
      begin_run(then == 2);
    }
  }

 private:
  // The fresh list from disk replaces the shown one: checks (and the selected row) follow the
  // file, a new request comes checked, a deleted one goes; the order is the saved run order.
  void merge_items(std::shared_ptr<std::vector<RunItem>> fresh, const std::vector<std::string>& errs) {
    std::map<fs::path, char> was;
    fs::path sel_file;
    if (items_) {
      for (size_t k = 0; k < order_.size() && k < checked_.size(); ++k) was[(*items_)[order_[k]].req.file] = checked_[k];
      const int sel = list_->GetSelection();
      if (sel != wxNOT_FOUND && static_cast<size_t>(sel) < visible_.size())
        sel_file = (*items_)[order_[visible_[static_cast<size_t>(sel)]]].req.file;
    }
    items_ = std::move(fresh);
    order_.clear();
    tags_.clear();
    checked_.clear();
    for (size_t i = 0; i < items_->size(); ++i) {
      order_.push_back(i);
      std::string t;
      for (const auto& g : runner::tags_of((*items_)[i])) t += (t.empty() ? "#" : " #") + g;
      tags_.push_back(std::move(t));
      const auto it = was.find((*items_)[i].req.file);
      checked_.push_back(it == was.end() ? 1 : it->second);
    }
    fill_list();
    for (size_t n = 0; n < visible_.size() && !sel_file.empty(); ++n)
      if ((*items_)[order_[visible_[n]]].req.file == sel_file) {
        list_->SetSelection(static_cast<int>(n));
        break;
      }
    if (!errs.empty()) set_status_w(U(std::to_string(errs.size()) + " file(s) could not be read: " + errs.front()));
  }

  // What changed between the plan of the run shown and what Run would send now ("" when
  // nothing did): requests edited on disk, added, removed or reordered.
  std::string plan_change() const {
    if (!items_ || !run_items_ || !run_order_ || steps_.empty() || run_) return {};
    const std::vector<size_t> now = chosen();
    if (run_items_.get() == items_.get() && now == *run_order_) return {};
    std::map<fs::path, const SavedRequest*> last;
    std::vector<fs::path> last_seq, now_seq;
    for (size_t i : *run_order_)
      if (i < run_items_->size()) {
        last[(*run_items_)[i].req.file] = &(*run_items_)[i].req;
        last_seq.push_back((*run_items_)[i].req.file);
      }
    std::vector<std::string> edited, added;
    size_t removed = 0;
    std::map<fs::path, bool> seen;
    for (size_t i : now) {
      const SavedRequest& r = (*items_)[i].req;
      now_seq.push_back(r.file);
      seen[r.file] = true;
      const auto it = last.find(r.file);
      const std::string name = r.name.empty() ? (*items_)[i].rel : r.name;
      if (it == last.end()) added.push_back(name);
      else if (!same_request(*it->second, r)) edited.push_back(name);
    }
    for (const auto& [f, r] : last)
      if (!seen.count(f)) ++removed;
    auto names = [](const std::vector<std::string>& v) {
      std::string o;
      for (size_t i = 0; i < v.size() && i < 2; ++i) o += (i ? ", " : "") + v[i];
      if (v.size() > 2) o += " +" + std::to_string(v.size() - 2);
      return o;
    };
    std::string o;
    auto add = [&](const std::string& t) { o += (o.empty() ? "" : "; ") + t; };
    if (!edited.empty()) add("edited: " + names(edited));
    if (!added.empty()) add("new: " + names(added));
    if (removed) add(std::to_string(removed) + " no longer in the run");
    if (o.empty() && now_seq != last_seq) add("new order");
    return o.empty() ? o : "Plan changed since the last run (" + o + ")";
  }

  // Shows or hides the "Plan changed" note. Whenever its text or visibility changes the settings
  // panel is laid out again (unless the caller does that next): a note that appears or goes away
  // without a layout leaves Iterations / Delay / Parallel drawn where they no longer are, and the
  // next keystroke's layout moves them under the cursor.
  bool update_plan_change(bool layout = true) {
    if (!changed_label_) return false;
    const std::string t = plan_change();
    if (U(t) == changed_text_) return false;
    changed_text_ = U(t);
    set_fixed_text(changed_label_, changed_text_, 2,
                   t.empty() ? wxString()
                             : wxString("Run sends the requests as saved now; the results shown are from the earlier plan."));
    (void)layout;  // a fixed height: nothing to lay out again
    return true;
  }

 public:
  void loaded(std::shared_ptr<std::vector<RunItem>> items, const std::vector<std::string>& errs) {
    if (!detail_) {  // the results side is not built yet: it calls this when it is
      pending_items_ = std::move(items);
      pending_errs_ = errs;
      return;
    }
    merge_items(std::move(items), errs);
    if (items_->empty()) {
      detail_->show(nullptr, errs.empty() ? "This folder has no requests." : errs.front());
    } else {
      detail_->show(nullptr, "Press Run (Ctrl+Enter) to send " + std::to_string(items_->size()) +
                                 " request(s) in order. Select a result to see its response, headers, request, timing and tests.");
    }
    // The last run of this folder in this session, if the window was closed since: shown in
    // steps on the next timer ticks (the table, then the summary and views, then the detail),
    // each short, so reopening a runner never holds the UI thread.
    auto& h = run_history()[key_];
    if (!h.empty() && steps_.empty() && !cleared_runners().count(key_)) {
      restore_rec_ = h.front();
      restore_phase_ = 0;
      restore_.StartOnce(1);
    }
    update_history_chips();
    // Only when this runner is on screen: focusing a child of a hidden page makes the notebook
    // select that page (a restored runner loading behind the active tab took the selection).
    if (IsShownOnScreen()) run_btn_->SetFocus();
    load_saved_history();
  }

  // The runs saved by earlier sessions (per user): read and parsed on a worker once per process
  // and runner, then put behind this session's runs in History; the newest is shown when the
  // runner has nothing to show yet (Postman keeps past runs per collection).
  void load_saved_history() {
    if (!history_read().insert(key_).second) return;
    auto rels = std::make_shared<std::map<std::string, size_t>>();
    for (size_t i = 0; i < items_->size(); ++i) rels->try_emplace((*items_)[i].rel, i);
    std::shared_ptr<const std::vector<RunItem>> items = items_;
    auto link = link_;
    const std::wstring key = key_;
    std::thread([link, key, rels, items] {
      auto recs = std::make_shared<std::vector<std::shared_ptr<const RunRecord>>>();
      for (const std::string& text : runner::load_history(key, kHistoryRuns)) {
        runner::LoadedRun lr;
        if (!runner::from_json(text, lr)) continue;
        // The file's request order onto this runner's items (a request since removed or renamed
        // gets an index past them: it is listed, just not openable).
        std::map<size_t, size_t> remap;
        size_t extra = items->size();
        auto map_item = [&](size_t old, const std::string& rel) {
          auto [it, fresh] = remap.try_emplace(old, 0);
          if (fresh) {
            const auto f = rels->find(rel);
            it->second = f != rels->end() ? f->second : extra++;
          }
          return it->second;
        };
        auto steps = std::make_shared<std::vector<StepPtr>>();
        steps->reserve(lr.steps.size());
        for (auto& p : lr.steps) {
          auto r = std::const_pointer_cast<StepResult>(p);  // ours alone until published below
          r->item = map_item(r->item, r->rel);
          steps->push_back(std::move(r));
        }
        for (auto& q : lr.summary.per_request) q.item = map_item(q.item, q.rel);
        auto rec = std::make_shared<RunRecord>();
        rec->started_iso = lr.meta.started_iso;
        rec->environment = lr.meta.environment;
        rec->label = lr.meta.label;
        if (lr.partial) rec->label += std::string(rec->label.empty() ? "" : ", ") + "failures only";
        rec->iterations = lr.meta.iterations;
        rec->planned = lr.summary.planned;
        rec->summary = std::move(lr.summary);
        rec->steps = std::move(steps);
        rec->items = items;
        rec->plan = lr.meta.plan;
        recs->push_back(std::move(rec));
      }
      if (recs->empty()) return;
      wxTheApp->CallAfter([link, recs] {
        if (link->frame) link->frame->saved_history(*recs);
      });
    }).detach();
  }

  void saved_history(const std::vector<std::shared_ptr<const RunRecord>>& recs) {
    auto& h = run_history()[key_];
    const bool was_empty = h.empty();
    for (const auto& r : recs) {
      if (h.size() >= kHistoryRuns) break;
      const bool dup = std::any_of(h.begin(), h.end(), [&](const std::shared_ptr<const RunRecord>& x) {
        return x->started_iso == r->started_iso && x->planned == r->planned && x->summary.total == r->summary.total;
      });
      if (!dup) h.push_back(r);  // older than anything this session ran
    }
    if (was_empty && !h.empty() && steps_.empty() && !run_ && !preparing_ && !restore_rec_ &&
        !cleared_runners().count(key_)) {
      restore_rec_ = h.front();
      restore_phase_ = 0;
      restore_.StartOnce(1);
    }
    update_history_chips();
  }

  void restore_step() {
    if (!restore_rec_ || run_) {
      restore_rec_.reset();
      return;
    }
    if (restore_phase_ == 0) {
      show_record(restore_rec_, true, 1);
      restore_phase_ = 1;
      restore_.StartOnce(1);
      return;
    }
    auto rec = std::move(restore_rec_);
    restore_rec_.reset();
    show_record(rec, true, 2);
  }

  void drain(const std::shared_ptr<RunState>& st) {
    if (st != run_) return;
    std::vector<StepPtr> got;
    {
      std::lock_guard lk(st->m);
      got.swap(st->pending);
    }
    if (got.empty()) return;
    size_t first_fail = static_cast<size_t>(-1);
    for (auto& s : got) {
      steps_.push_back(s);
      if (s->script_ms > 0 && !cols_.scripts) show_script_column(true);
      count(*s);
      if (matches(*s)) {
        if (first_fail == static_cast<size_t>(-1) && s->failed()) first_fail = view_.size();
        view_.push_back(s);
      }
    }
    dirty_ = true;
    if (cols_.sort >= 0) apply_sort(false);
    table_->refresh_rows();
    if (first_fail != static_cast<size_t>(-1) && follow_ && cols_.sort < 0 && !pinned_fail_) {
      // The first failure stays selected (and in the detail pane) while the run goes on, instead
      // of the selection racing after the newest row: triage starts at once.
      pinned_fail_ = true;
      pinned_step_ = view_[first_fail];
      follow_ = false;
      table_->SetSelection(static_cast<int>(first_fail));
      table_->ensure_visible(first_fail);
      detail_at_ = Clock::now();
      detail_due_ = false;
      show_selected(false);
    } else if (!view_.empty() && follow_ && cols_.sort < 0) {
      const size_t last = view_.size() - 1;
      table_->SetSelection(static_cast<int>(last));
      table_->ensure_visible(last);
      // The detail pane follows at most ~4x a second: building a response view per result
      // (thousands a second) would keep the UI thread busy.
      if (ms_between(detail_at_, Clock::now()) >= 250) {
        detail_at_ = Clock::now();
        show_selected(true, true);
      } else {
        detail_due_ = true;
      }
    }
  }

  void preflight_done(const std::shared_ptr<RunState>& st, std::shared_ptr<Workspace> wsc,
                      std::shared_ptr<const std::vector<size_t>> order, runner::Consent consent) {
    if (st != run_) return;
    bool allow = false;
    auto cancelled = [&] {
      run_.reset();
      set_running(false);
      summary_->set(summary_now(false), "Canceled", false);
    };
    // While a question is open nothing is sent yet: the runner says it waits, and Run is not a
    // red Stop behind the dialog. Once answered, the run starts its clock from there.
    bool waited = false;
    auto waiting = [&] {
      if (waited) return;
      waited = true;
      summary_->set(RunSummary{0, planned_}, "Waiting for your answer", false);
      detail_->show(nullptr, "Nothing is sent until you answer the question.");
      run_btn_->set("Run", false);
      run_btn_->Disable();
    };
    // Scripts are code from the workspace (possibly a cloned repo): asked once, as the request
    // tab asks. "Run without scripts" sends those requests without them; they do not pass.
    int scripts = consent.scripts_trusted && consent.script_requests ? 1 : 0;  // 1 run, 2 trust first, 0 no
    if (consent.scripts_need_trust()) {
      waiting();
      const int ans = ask_three(
          this, "watt runner", "Run the scripts of this workspace?",
          wxString::Format("%zu request%s of this run contain scripts (pre-request / post-response) in\n%s\n\n"
                           "Scripts run in a sandbox (no file or process access; network only through watt's own "
                           "checks), but they can read and change your requests and variables.\n\nWithout them, "
                           "those requests are sent without their scripts and do not pass.",
                           consent.script_requests, consent.script_requests == 1 ? "" : "s", wxString(root_.wstring())),
          "Trust and run", "Run without scripts", "Cancel", true);
      if (ans == wxID_CANCEL || st != run_) return cancelled();
      scripts = ans == wxID_YES ? 2 : 0;
    }
    // Proxy / CA / client certificate from workspace files (a cloned repo could route the run
    // through its proxy or make watt trust its CA): asked separately, saying what each means.
    if (!consent.network.empty()) {
      waiting();
      wxString msg = "The workspace settings of this run want watt to use:\n\n";
      bool proxy = false, ca = false;
      for (const auto& it : consent.network) {
        using K = Workspace::NetworkItem::Kind;
        proxy = proxy || (it.kind == K::Proxy && it.file_blocked.empty());
        ca = ca || it.kind == K::Ca;
        msg += "    " + U(it.text) + "  (" + U(it.level) + ")\n";
      }
      msg += "\n";
      if (proxy)
        msg += "All requests of this run will go through the proxy above: it sees every plain HTTP request, "
               "including Authorization headers and secrets.\n";
      if (ca)
        msg += "watt will trust certificates signed by the CA above: whoever holds its key can read and change "
               "the HTTPS traffic of this workspace, secrets included.\n";
      msg += "\nOnly allow what you trust. Requests that need anything you do not allow are not sent.";
      const int ans = ask_three(this, "watt runner", "Trust these network settings?", msg, "Allow and run",
                                "Run without them", "Cancel", false);
      if (ans == wxID_CANCEL || st != run_) return cancelled();
      if (ans != wxID_YES) consent.network.clear();
    }
    if (!consent.files.empty() || !consent.env_vars.empty()) {
      waiting();
      wxString msg = "This run reads from outside the workspace:\n\n";
      size_t n = 0;
      for (const auto& f : consent.files) {
        if (++n > 12) break;
        msg += wxString("    upload / schema file  ") + wxString(f.wstring()) + "\n";
      }
      for (const auto& v : consent.env_vars) {
        if (++n > 12) break;
        msg += "    environment variable  " + U(v) + "\n";
      }
      if (n > 12) msg += wxString::Format("    ... and %zu more\n", consent.files.size() + consent.env_vars.size() - 12);
      msg += "\nAllow these for this workspace? Requests that need anything you do not allow are not sent.";
      // Enter keeps the safe answer here: nothing outside the workspace without a deliberate click.
      const int ans = ask_three(this, "watt runner", "Read from outside the workspace?", msg, "Allow and run",
                                "Run without them", "Cancel", false);
      if (ans == wxID_CANCEL || st != run_) return cancelled();
      allow = ans == wxID_YES;
    }
    if (!allow) {
      consent.files.clear();
      consent.env_vars.clear();
    }
    if (waited) {
      started_ = Clock::now();
      set_running(true);
      summary_->set(RunSummary{0, planned_}, "Preparing", true);
      detail_->show(nullptr, "Running...");
    }
    start_run(st, std::move(wsc), std::move(order), std::move(consent), scripts);
  }

  void run_done(const std::shared_ptr<RunState>& st, RunSummary s, std::shared_ptr<Workspace> wsc) {
    if (st != run_) return;
    drain(st);
    run_.reset();
    tick_.Stop();
    if (reload_due_) reload_timer_.StartOnce(50);
    // Planned steps that never ran (Stop, Esc, stop on failure) are listed as Skipped.
    if (s.stopped && run_order_ && steps_.size() < planned_ && run_label_ != "rerun failed") {
      std::vector<bool> done(planned_);
      for (const auto& p : steps_) {
        const size_t at = static_cast<size_t>(p->iteration) * run_order_->size() + p->pos;
        if (at < done.size()) done[at] = true;
      }
      auto sk = runner::skipped_steps(*run_items_, *run_order_, iterations_, done, steps_.size(), 50000);
      for (auto& x : sk) {
        steps_.push_back(x);
        count(*x);
        if (matches(*x)) view_.push_back(x);
      }
      table_->refresh_rows();
    }
    if (run_parallel_) {
      // Parallel workers finish out of order. The list keeps the order the results arrived in
      // (the # column; the Iter column sorts into plan order), so nothing jumps at the end;
      // steps_ (exports, history) is put in plan order.
      std::stable_sort(steps_.begin(), steps_.end(), [](const StepPtr& a, const StepPtr& b) {
        if (a->iteration != b->iteration) return a->iteration < b->iteration;
        if (a->pos != b->pos) return a->pos < b->pos;
        return a->seq < b->seq;
      });
    }
    // The row shown stays shown, picked by the user or not; with none yet, the first failure.
    if (!shown_ && !view_.empty()) {
      int sel = 0;
      for (size_t i = 0; i < view_.size(); ++i)
        if (view_[i]->failed()) {
          sel = static_cast<int>(i);
          break;
        }
      follow_ = false;
      table_->SetSelection(sel);
      table_->ensure_visible(static_cast<size_t>(sel));
      show_selected(false);
    }
    final_ = runner::summarize(steps_, planned_, s.wall_ms, s.stopped, true);
    have_final_ = true;
    // History: this run, for this session (survives the window), newest first.
    auto rec = std::make_shared<RunRecord>();
    rec->started_iso = started_iso_;
    rec->environment = env_;
    rec->label = run_label_;
    rec->iterations = iterations_;
    rec->planned = planned_;
    rec->summary = final_;
    rec->steps = std::make_shared<const std::vector<StepPtr>>(steps_);
    rec->items = run_items_;
    rec->order = run_order_;
    rec->plan = run_plan_;
    auto& h = run_history()[key_];
    h.push_front(rec);
    while (h.size() > kHistoryRuns) h.pop_back();
    {
      // Kept for the next session too (per user, written on a worker).
      runner::RunMeta meta;
      meta.name = name_s_;
      meta.environment = env_;
      meta.started_iso = started_iso_;
      meta.iterations = iterations_;
      meta.label = run_label_;
      meta.plan = run_plan_;
      std::thread([rec, meta, key = key_] {
        runner::save_history(key, meta.started_iso, runner::history_json(meta, rec->summary, *rec->steps), kHistoryRuns);
      }).detach();
    }
    shown_record_ = rec;
    detail_due_ = false;
    if (follow_) show_selected();
    set_running(false);
    if (run_label_ == "rerun failed") update_counts();  // the plan line is the next Run's again
    eval_thresholds(final_);
    refresh_fails();
    summary_->set(final_, state_text(final_), false);
    update_filter_chips();
    if (view_mode_ == 1) refresh_stats();
    if (view_mode_ == 2) chart_->set(&steps_, final_.wall_ms);
    if (keep_->GetValue()) keep_values(*wsc);
  }

  void data_loaded(uint64_t gen, std::shared_ptr<std::vector<runner::DataRow>> rows, std::string name, std::string err) {
    if (gen != data_gen_) return;
    struct Reset {
      int& r;
      ~Reset() { r = -1; }
    } reset{restore_iters_};
    if (!err.empty()) {
      data_label_->SetLabel(U(name + ": " + err));
      data_label_->SetForegroundColour(rv::Theme::get().err);
      data_chips_->set_label(kDataPick, "Choose CSV / JSON...");
      data_.reset();
    } else {
      data_ = std::move(rows);
      data_name_ = name;
      std::string cols;
      if (!data_->empty())
        for (const auto& [k, v] : data_->front()) cols += (cols.empty() ? "" : ", ") + k;
      if (cols.size() > 40) cols = cols.substr(0, 37) + "...";
      // The file and its size on the button itself (it picks another), the columns below it.
      std::string shown = name.size() > 28 ? name.substr(0, 26) + "\xE2\x80\xA6" : name;
      data_chips_->set_label(kDataPick, shown + kDot + std::to_string(data_->size()) + " row" + (data_->size() == 1 ? "" : "s"));
      data_label_->SetLabel(U("Columns {{" + cols + "}}, one row per iteration"));
      data_label_->SetForegroundColour(rv::Theme::get().fg);
      // Iterations follow the file (one per row); removing the file brings back what they were.
      if (restore_iters_ <= 0 && iters_auto_ < 0) iters_before_data_ = iters_->value();
      iters_->set_value(restore_iters_ > 0 ? restore_iters_ : static_cast<int>(std::min<size_t>(data_->size(), 1000000)));
      iters_auto_ = iters_->value();
      data_chips_->set_visible(kDataClear, true);
      data_chips_->set_visible(kDataPreview, true);
    }
    update_counts();
  }

  void set_status(const std::string& s) { set_status_w(U(s)); }

  // ---- the run's settings, remembered per runner (folder) for this user
  wxString settings_key() const {
    return wxString::Format("Runner/Settings/%016llx", static_cast<unsigned long long>(std::hash<std::wstring>{}(key_)));
  }
  void save_settings() {
    wxConfigBase* c = wxConfigBase::Get();
    if (!c || left_stage_ < kLeftBuilt || !iters_) return;
    wxString th = thresh_->GetValue();
    th.Replace("\t", " ");
    th.Replace("\n", " ");
    const wxString v = wxString::Format("v1\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t", iters_->value(), delay_->value(),
                                        par_->value(), dur_->value(), ramp_->value(), profile_, bail_->GetValue() ? 1 : 0,
                                        cookies_->GetValue() ? 1 : 0, keep_->GetValue() ? 1 : 0) +
                       th + "\t" + wxString(data_path_.wstring());
    c->Write(settings_key(), v);
  }
  void restore_settings() {
    wxConfigBase* c = wxConfigBase::Get();
    wxString v;
    if (!c || !c->Read(settings_key(), &v)) return;
    wxArrayString f = wxSplit(v, '\t', 0);
    if (f.size() < 12 || f[0] != "v1") return;
    auto num = [&](size_t i) {
      long n = 0;
      return f[i].ToLong(&n) ? static_cast<int>(n) : 0;
    };
    iters_->set_value(num(1));
    delay_->set_value(num(2));
    par_->set_value(num(3));
    dur_->set_value(num(4));
    ramp_->set_value(num(5));
    bail_->SetValue(num(7) != 0);
    cookies_->SetValue(num(8) != 0);
    keep_->SetValue(num(9) != 0);
    thresh_->ChangeValue(f[10]);
    set_profile(std::clamp(num(6), 0, 3), false);
    if (!f[11].empty()) {
      // The data file picked last time (the user's own pick, kept per user): read again on a worker.
      restore_iters_ = num(1);
      data_picked(fs::path(f[11].ToStdWstring()));
    }
  }

 private:
  void set_status_w(const wxString& s) {
    wxString one = s;
    one.Replace("\n", "  ");
    status_->SetLabel(one);
    status_->SetToolTip(s.empty() ? wxString() : s);
  }

  std::string state_text(const RunSummary& s) const {
    if (!thr_ok_) return s.failed ? "Done \xC2\xB7 " + std::to_string(s.failed) + " failed \xC2\xB7 thresholds missed"
                                 : "Done \xC2\xB7 thresholds missed";
    if (s.stopped) return s.failed ? "Stopped \xC2\xB7 " + std::to_string(s.failed) + " failed" : "Stopped";
    return s.failed ? "Done \xC2\xB7 " + std::to_string(s.failed) + " failed" : "Done \xC2\xB7 all passed";
  }

  // ---- filters, counts
  // Results narrowed to one request (By request, a row's menu) or one failure group (Failures).
  bool in_scope(const StepResult& s) const {
    if (scope_item_ != kNoScope && s.item != scope_item_) return false;
    return scope_key_.empty() || runner::has_failure(s, scope_key_);
  }
  void count(const StepResult& s) {
    if (!in_scope(s)) return;
    if (s.skipped || s.stopped())
      ++n_skipped_;
    else if (s.failed())
      ++n_failed_;
    else
      ++n_passed_;
  }
  bool matches(const StepResult& s) const {
    if (!in_scope(s)) return false;
    switch (filter_mode_) {
      case 1: return s.passed();
      case 2: return s.failed();
      case 3: return s.skipped || s.stopped();
      default: return true;
    }
  }
  void set_scope(size_t item, std::string key, const std::string& label, int mode) {
    scope_item_ = item;
    scope_key_ = std::move(key);
    const bool on = scope_item_ != kNoScope || !scope_key_.empty();
    show_scope_chip(on, label);
    n_passed_ = n_failed_ = n_skipped_ = 0;
    for (const auto& s : steps_) count(*s);
    if (mode >= 0) {
      filter_mode_ = mode;
      for (int c = kFAll; c <= kFSkipped; ++c) bar_->set_on(c, c == kFAll + mode);
    }
    update_filter_chips();
    show_mode(0);
    rebuild_view();
    // The table was hidden: scroll again once it has its size (as the filter chips do).
    CallAfter([this] {
      if (view_.empty() || view_mode_ != 0) return;
      table_->SetSelection(0);
      table_->ScrollToRow(0);
      show_selected();
    });
    table_->SetFocus();
    bar_->Refresh();
    if (on)
      set_status_w(U(std::to_string(view_.size()) + " result" + (view_.size() == 1 ? "" : "s") + " shown" + kDot +
                     "click the \xC3\x97 chip (or Esc) to show every result"));
    else
      set_status_w("");
  }
  void clear_scope() {
    if (scope_item_ != kNoScope || !scope_key_.empty()) set_scope(kNoScope, {}, "", -1);
  }
  void show_scope_chip(bool on, const std::string& label) {
    if (!scope_bar_) return;
    if (!on && !scope_bar_->IsShown()) return;
    std::string chip;
    if (on) {
      chip = "\xC3\x97  " + (u8_width(label) > 34 ? u8_head(label, 33) + "\xE2\x80\xA6" : label);
      wxClientDC dc(scope_bar_);
      dc.SetFont(scope_bar_->GetFont());
      scope_bar_->SetMinSize(wxSize(dc.GetTextExtent(U(chip)).x + FromDIP(8 + 16 + 4), FromDIP(26)));
    }
    scope_bar_->set_label(kScope, chip);
    scope_bar_->Show(on);
    scope_row_->Layout();
  }

  // Reset (Bruno's): the results go, the plan shows again. The run stays in History, and a
  // reopened runner shows no results either until the next run.
  void reset_results() {
    if (run_ || preparing_ || !detail_) return;
    steps_.clear();
    view_.clear();
    show_script_column(false);
    scope_item_ = kNoScope;
    scope_key_.clear();
    show_scope_chip(false, {});
    n_fail_groups_ = 0;
    pinned_fail_ = false;
    pinned_step_.reset();
    n_passed_ = n_failed_ = n_skipped_ = 0;
    shown_.reset();
    shown_record_.reset();
    have_final_ = false;
    final_ = RunSummary{};
    restore_rec_.reset();
    table_->refresh_rows();
    refresh_fails();
    if (view_mode_ != 0) show_mode(0);
    summary_->set_thresholds({});
    summary_->set(RunSummary{}, "Ready", false);
    detail_->show(nullptr, items_ && !items_->empty()
                               ? "Press Run (Ctrl+Enter) to send " + std::to_string(items_->size()) +
                                     " request(s) in order. Select a result to see its response, headers, request, timing and tests."
                               : std::string());
    update_filter_chips();
    update_plan_change();
    cleared_runners().insert(key_);
    set_status_w("Results cleared. The run is still in History.");
    if (run_btn_) run_btn_->SetFocus();
  }

  // Back to the sidebar's order: this folder's entries (and its subfolders') leave run-order.json.
  void reset_order() {
    if (run_ || !saved_order_) return;
    save_order_.Stop();
    order_dirty_.clear();
    const uint64_t s = ++order_seq();
    const fs::path root = root_, base = run_base();
    auto link = link_;
    ++link->saving;
    std::thread([root, base, s, link] {
      runner::FolderOrder changed;
      for (const auto& [dir, names] : runner::load_run_order(root)) {
        const fs::path rel = dir.lexically_normal().lexically_relative(base.lexically_normal());
        if (!rel.empty() && *rel.begin() != "..") changed[dir] = {};
      }
      std::string err;
      const bool ok = changed.empty() || runner::save_run_order(root, changed, s, &err);
      --link->saving;
      wxTheApp->CallAfter([link, ok, err] {
        if (!link->frame) return;
        link->frame->set_status_w(ok ? U(std::string("Runs in the sidebar's order again (removed from ") + runner::kRunOrderFile + ")")
                                     : U("Could not save the run order: " + err));
        if (ok) link->frame->reload(0);
      });
    }).detach();
  }
  std::string request_label(size_t item) const {
    if (run_items_ && item < run_items_->size()) {
      const RunItem& it = (*run_items_)[item];
      return it.req.name.empty() ? it.rel : it.req.name;
    }
    return "request";
  }
  void scope_to_request(size_t item) { set_scope(item, {}, request_label(item), -1); }
  void scope_to_stats_row(size_t row) {
    if (const runner::RequestStats* q = stats_->row(row)) scope_to_request(q->item);
  }
  void scope_to_failure(const runner::FailureGroup& g) {
    std::string label = (g.name.empty() ? g.rel : g.name) + ": " + g.check;
    if (label.size() > 70) label = label.substr(0, 67) + "...";
    set_scope(kNoScope, g.key, label + "  (" + std::to_string(g.count) + ")", 0);
  }
  // Right click on a result: narrow to its request, or to one of its failures.
  void row_menu(const wxPoint& screen) {
    wxPoint at = screen == wxDefaultPosition ? wxPoint(FromDIP(40), FromDIP(40)) : table_->ScreenToClient(screen);
    if (screen != wxDefaultPosition) {
      const int h = table_->VirtualHitTest(at.y);
      if (h != wxNOT_FOUND) {
        table_->SetSelection(h);
        follow_ = false;
        show_selected();
      }
    }
    if (!shown_) return;
    const StepPtr s = shown_;
    wxMenu m;
    m.Append(1, U("Only " + request_label(s->item)));
    const auto fails = runner::failures_of(*s);
    for (size_t i = 0; i < fails.size() && i < 10; ++i) {
      std::string t = "Only this failure: " + fails[i].message;
      if (t.size() > 90) t = t.substr(0, 87) + "...";
      m.Append(10 + static_cast<int>(i), U(t));
    }
    if (scope_item_ != kNoScope || !scope_key_.empty()) {
      m.AppendSeparator();
      m.Append(2, "Show every result");
    }
    m.AppendSeparator();
    m.Append(3, "Open request in a tab\tEnter");
    const int id = table_->GetPopupMenuSelectionFromUser(m, at);
    if (id == 1) scope_to_request(s->item);
    else if (id == 2) clear_scope();
    else if (id == 3) open_step(s);
    else if (id >= 10 && static_cast<size_t>(id - 10) < fails.size()) {
      const std::string key = runner::failure_key(*s, fails[static_cast<size_t>(id - 10)]);
      for (const auto& g : runner::group_failures(steps_))
        if (g.key == key) return scope_to_failure(g);
    }
  }
  void update_filter_chips() {
    const size_t all = n_passed_ + n_failed_ + n_skipped_;
    bar_->set_label(kFAll, "All  " + std::to_string(all));
    bar_->set_label(kFPassed, "Passed  " + std::to_string(n_passed_));
    bar_->set_label(kFFailed, "Failed  " + std::to_string(n_failed_));
    bar_->set_label(kFSkipped, "Skipped  " + std::to_string(n_skipped_));
    bar_->set_label(kVFail, n_fail_groups_ ? "Failures  " + std::to_string(n_fail_groups_) : std::string("Failures"));
    const bool idle = !run_;
    const bool any = !steps_.empty();
    bar_->set_enabled(kRunAgain, idle && items_ && !chosen().empty());
    bar_->set_enabled(kRerunFailed, idle && n_failed_ > 0 && shown_is_latest());
    for (int id : {kHtml, kJson, kJunit, kReset}) bar_->set_enabled(id, idle && any);
    update_history_chips();
  }
  void update_history_chips() {
    const auto& h = run_history()[key_];
    bar_->set_enabled(kHistory, !run_ && !h.empty());
    bar_->set_label(kHistory, "History (" + std::to_string(h.size()) + ") \xE2\x96\xBE");
  }
  bool shown_is_latest() const {
    const auto& h = run_history()[key_];
    return !shown_record_ || h.empty() || h.front() == shown_record_;
  }

  // may_follow: selecting the last row (by the user) resumes following new results.
  // live: the selection follows a run's newest result (not picked by the user): a big body
  // waits for a click (DetailPane::set_defer_big).
  void show_selected(bool may_follow = true, bool live = false) {
    const int i = table_->GetSelection();
    if (i == wxNOT_FOUND || static_cast<size_t>(i) >= view_.size()) return;
    if (may_follow && cols_.sort < 0 && static_cast<size_t>(i) + 1 == view_.size()) follow_ = true;
    if (shown_ != view_[static_cast<size_t>(i)]) {
      detail_->set_defer_big(live, false);
      shown_ = view_[static_cast<size_t>(i)];
      detail_->show(shown_, "");
      detail_->set_can_open(!file_of(*shown_).empty());
    } else if (!live) {
      detail_->set_defer_big(false);  // the user picked the row shown: its body now
    }
  }

  fs::path file_of(const StepResult& s) const {
    if (run_items_ && s.item < run_items_->size()) return (*run_items_)[s.item].req.file;
    return {};
  }

  // Opens the step's saved request with the response this run got in its response view (Postman's
  // runner opens a result the same way), not an empty one: the HTTP response (a spilled large body
  // is read back), the HTTP transport error, or the gRPC call's status, google.rpc.Status details,
  // messages and trailers. Put together on a worker.
  void open_step(const StepPtr& s) {
    if (!s) return;
    const fs::path f = file_of(*s);
    if (f.empty()) return;
    if (!s->sent || s->skipped || s->stopped()) {
      open_file(f);
      return;
    }
    // A run read back from the history keeps no bodies: the response view would check the
    // request's asserts again against an empty body ("tests now 2/4" in red while the run said
    // 4/4). Open the request alone and say why its response is not shown.
    if (s->from_history && s->kind != RequestKind::Grpc && s->transport_ok && s->size > 0 && s->body_preview.empty()) {
      open_file(f);
      set_status_w(U("Opened " + request_label(s->item) + ": this run was read back from the history without its "
                     "response bodies, so its response is not shown (the run's checks: " +
                     std::to_string(s->asserts_passed) + "/" + std::to_string(s->asserts_passed + s->asserts_failed) +
                     " passed). Send it again for a live response."));
      return;
    }
    auto link = link_;
    const std::string when = started_iso_;
    std::thread([s, link, f, when] {
      LastResponse last;
      last.when_iso = when;
      if (s->kind == RequestKind::Grpc) last.grpc = grpc_response_of(*s);
      else last.http = http_response_of(*s);
      wxTheApp->CallAfter([link, f, last = std::move(last)]() mutable {
        if (link->frame) link->frame->open_with_response(f, std::move(last));
      });
    }).detach();
  }

  static std::shared_ptr<HttpResponse> http_response_of(const StepResult& s) {
    auto r = std::make_shared<HttpResponse>();
    r->ok = s.transport_ok;
    r->effective_url = s.url;
    r->timing = s.timing;
    // The editor's response view shows the setup phases inside its total, as for its own sends.
    r->timing.total_ms += s.prep_ms;
    for (const auto& p : s.phases)
      if (std::strcmp(p.name, "Prepare") == 0) r->prepare_ms = p.ms;
    r->sizes.request_body = s.req_body_size;
    if (!s.transport_ok) {
      r->error = s.error.empty() ? "No response" : s.error;
      return r;
    }
    r->status = s.status;
    r->status_text = s.status_text;
    r->http_version = s.http_version;
    for (const auto& [k, v] : s.resp_headers) r->headers.add(k, v);
    std::string full;
    if (s.body_truncated && s.body_file && s.body_file->read(&full)) r->body = std::move(full);
    else r->body = s.body_preview;
    r->sizes.response_body = s.size;
    r->sizes.response_body_wire = s.size;
    if (s.bytes_received > s.size) r->sizes.response_headers = s.bytes_received - s.size;
    return r;
  }

  // The run keeps a stream's messages as one JSON array (a single message is an object): split
  // back into its messages when it was kept whole.
  static std::shared_ptr<GrpcResponse> grpc_response_of(const StepResult& s) {
    auto g = std::make_shared<GrpcResponse>();
    g->ok = s.transport_ok;
    g->status_code = static_cast<int>(s.status);
    g->status_name = s.status_text;
    if (!s.transport_ok) g->status_message = s.error;
    g->error_details = s.grpc_details;
    g->headers.assign(s.resp_headers.begin(), s.resp_headers.end());
    g->trailers.assign(s.resp_trailers.begin(), s.resp_trailers.end());
    g->timing = s.timing;
    g->timing.total_ms += s.prep_ms;  // Queue inside the total, as the editor's own calls show it
    const std::string& b = s.body_preview;
    const size_t at = b.find_first_not_of(" \t\r\n");
    bool split = false;
    if (at != std::string::npos && b[at] == '[' && !s.body_truncated) {
      rapidjson::Document d;
      d.Parse(b.data(), b.size());
      if (!d.HasParseError() && d.IsArray() && d.Size() > 0) {
        split = true;
        const size_t each = static_cast<size_t>(s.size / d.Size());
        for (const auto& v : d.GetArray()) {
          rapidjson::StringBuffer sb;
          rapidjson::PrettyWriter<rapidjson::StringBuffer> w(sb);
          w.SetIndent(' ', 2);
          v.Accept(w);
          GrpcMessageEvent m;
          m.json.assign(sb.GetString(), sb.GetSize());
          m.wire_bytes = each;
          g->messages.push_back(std::move(m));
        }
      }
    }
    if (!split && at != std::string::npos) {
      GrpcMessageEvent m;
      m.json = b;
      m.wire_bytes = static_cast<size_t>(s.size);
      g->messages.push_back(std::move(m));
    }
    return g;
  }

 public:
  void open_with_response(const fs::path& f, LastResponse last) {
    open_file(f);
    wxWindow* w = GetParent();
    while (w && !dynamic_cast<wxAuiNotebook*>(w)) w = w->GetParent();
    auto* nb = dynamic_cast<wxAuiNotebook*>(w);
    if (!nb) return;
    // The tab that holds this request now (opened or brought forward), not just the selected page:
    // a request that could not be opened must not put this response into another tab.
    RequestTab* tab = nullptr;
    for (size_t i = 0; i < nb->GetPageCount() && !tab; ++i)
      if (auto* t = dynamic_cast<RequestTab*>(nb->GetPage(i)); t && t->file() == f) tab = t;
    if (!tab) return;
    // Three short UI steps, each on its own turn of the message loop (the tab shows, with its
    // editor, first): build the tab's response view when it has none yet, then fill it.
    wxWeakRef<RequestTab> weak(tab);
    auto held = std::make_shared<LastResponse>(std::move(last));
    auto fill = [weak, held] {
      if (weak) weak->restore_response(std::move(*held));
    };
    if (tab->has_response_view()) {
      next_turn(fill);
      return;
    }
    next_turn([weak, fill] {
      if (!weak) return;
      if (!weak->has_response_view()) weak->build_response_view();
      next_turn(fill);
    });
  }

 private:

  void open_file(const fs::path& file) {
    if (file.empty()) return;
    auto* host = dynamic_cast<SidebarHost*>(wxGetTopLevelParent(this));
    if (!host) {
      set_status_w("The main window is not available.");
      return;
    }
    host->open_request(file, false);
  }

  // ---- the request list
  void fill_list() {
    if (!items_) return;
    const std::string f = filter_->GetValue().ToStdString(wxConvUTF8);
    visible_.clear();
    list_->rows.clear();
    for (size_t k = 0; k < order_.size(); ++k) {
      const RunItem& it = (*items_)[order_[k]];
      if (!f.empty() && !runner::tag_filter_match(it, f)) continue;
      visible_.push_back(k);
      RequestList::Row r;
      r.grpc = it.req.kind == RequestKind::Grpc;
      r.method = r.grpc ? "gRPC" : (it.req.http.method.empty() ? "GET" : it.req.http.method);
      r.name = it.req.name.empty() ? it.rel.substr(it.rel.rfind('/') + 1) : it.req.name;
      if (const size_t sl = it.rel.rfind('/'); sl != std::string::npos) r.folder = it.rel.substr(0, sl);
      r.tags = tags_[order_[k]];
      r.checked = checked_[k] != 0;
      list_->rows.push_back(std::move(r));
    }
    list_->refresh_rows();
    fit_list_height();
    update_counts();
  }

  void check_all(bool on) {
    for (size_t n = 0; n < visible_.size(); ++n) {
      checked_[visible_[n]] = on;
      list_->rows[n].checked = on;
    }
    list_->Refresh();
    update_counts();
  }

  // Reordering: the list is the folder tree flattened, so a step moves a whole block (a request
  // within its folder, or a subfolder with its requests; at the edge of its folder the folder
  // moves). The new order is saved to <ws>/run-order.json on a worker (debounced), which the
  // next open of this runner and `watt --run` follow.
  fs::path run_base() const {
    std::error_code ec;
    if (folder_.empty() || key_ == runner_key(root_, {})) return root_;
    return fs::is_directory(folder_, ec) ? folder_ : folder_.parent_path();
  }

  // Applies a reorder of order_ (old position -> item) to checked_, refreshes, selects item.
  void reordered(const std::vector<size_t>& before, size_t item, const runner::FolderOrder& changed) {
    std::vector<char> chk(items_->size(), 1);
    for (size_t k = 0; k < before.size(); ++k) chk[before[k]] = checked_[k];
    for (size_t k = 0; k < order_.size(); ++k) checked_[k] = chk[order_[k]];
    for (const auto& [dir, names] : changed) order_dirty_[dir] = names;
    set_saved_order(true);
    fill_list();
    for (size_t n = 0; n < visible_.size(); ++n)
      if (order_[visible_[n]] == item) {
        list_->SetSelection(static_cast<int>(n));
        break;
      }
    save_order_.StartOnce(250);
  }

  void flush_order() {
    if (order_dirty_.empty()) return;
    auto changed = std::make_shared<runner::FolderOrder>(std::move(order_dirty_));
    order_dirty_.clear();
    const uint64_t s = ++order_seq();
    const fs::path root = root_;
    auto link = link_;
    ++link->saving;
    std::thread([root, changed, s, link] {
      std::string err;
      const bool ok = runner::save_run_order(root, *changed, s, &err);
      --link->saving;
      wxTheApp->CallAfter([link, ok, err] {
        if (!link->frame) return;
        link->frame->set_status_w(ok ? U(std::string("Run order saved to ") + runner::kRunOrderFile +
                                         " (watt --run follows it)")
                                     : U("Could not save the run order: " + err));
      });
    }).detach();
  }

  void move_sel(size_t n, int d) {
    if (n >= visible_.size() || !items_) return;
    const std::vector<size_t> before = order_;
    const size_t item = order_[visible_[n]];
    runner::FolderOrder changed;
    // With a filter, keep stepping until the shown order changes (or it cannot move).
    const std::vector<size_t> vis0 = visible_order(order_);
    for (int guard = 0; guard < 4096; ++guard) {
      const size_t pos = static_cast<size_t>(std::find(order_.begin(), order_.end(), item) - order_.begin());
      if (!runner::move_block(*items_, order_, run_base(), pos, d, &changed)) break;
      if (visible_order(order_) != vis0) break;
    }
    if (order_ == before) {
      // Said under the request list, where the click was (not in the results' status line).
      set_fixed_text(order_note_, wxString::FromUTF8("Already at the edge: a request stays in its folder."), 1,
                     "A request moves within its folder only; it is already first / last there.");
      order_note_->SetForegroundColour(rv::Theme::get().warn);
      auto link = link_;
      next_turn([link] {
        if (link->frame) link->frame->edge_note_done();
      }, 3000);
      return;
    }
    reordered(before, item, changed);
  }

  // Drag and drop in the list: the request at visible row `from` moves before visible row
  // `before` (visible_.size(): after the last one shown), block by block.
  void drop_row(size_t from, size_t before) {
    if (!items_ || from >= visible_.size() || before > visible_.size() || before == from || before == from + 1) return;
    const std::vector<size_t> prev = order_;
    const size_t a = visible_[from];
    const size_t at = before == visible_.size() ? visible_.back() + 1 : visible_[before];
    const size_t item = order_[a];
    runner::FolderOrder changed;
    if (!runner::drop_block(*items_, order_, run_base(), a, at, &changed)) return;
    reordered(prev, item, changed);
  }

  // The visible (filtered) items in order, for an order.
  std::vector<size_t> visible_order(const std::vector<size_t>& order) const {
    const std::string f = filter_->GetValue().ToStdString(wxConvUTF8);
    std::vector<size_t> v;
    for (size_t i : order)
      if (f.empty() || runner::tag_filter_match((*items_)[i], f)) v.push_back(i);
    return v;
  }
  std::vector<size_t> visible_items() const { return visible_order(order_); }

  // What Run sends: checked requests that pass the filter, in list order.
  std::vector<size_t> chosen() const {
    std::vector<size_t> o;
    if (!items_) return o;
    for (size_t k : visible_)
      if (checked_[k]) o.push_back(order_[k]);
    return o;
  }

  void update_counts() {
    if (!items_) return;
    const size_t n = chosen().size();
    const int it = std::max(1, iters_->value());
    const int par = std::max(1, par_->value());
    req_title_->SetLabel(wxString::Format("REQUESTS  %zu / %zu", n, order_.size()));
    const size_t sends = n * static_cast<size_t>(it);
    wxString plan = n ? wxString::Format("%zu request%s x %d iteration%s = %zu send%s", n, n == 1 ? "" : "s", it,
                                         it == 1 ? "" : "s", sends, sends == 1 ? "" : "s")
                      : wxString("Select at least one request.");
    if (const int d = dur_->value(); n && d > 0) {
      plan = wxString::Format("%zu request%s, repeated for %d s", n, n == 1 ? "" : "s", d);
      if (it > 1) plan += wxString::Format(" (at most %d iterations)", it);
      if (par > 1) plan += wxString::Format(", %d at a time", par);
    } else if (n && par > 1 && it < par) {
      // Parallel workers each run whole iterations (requests in one iteration stay in order).
      plan += it == 1 ? wxString::Format(", sequential: Parallel %d applies across iterations", par)
                      : wxString::Format(", %d at a time (Parallel %d applies across iterations)", it, par);
    } else if (n && par > 1) {
      plan += wxString::Format(", %d at a time", par);
    }
    if (n && par > 1) {
      const int d = dur_->value(), r = ramp_->value();
      if (profile_ == 1 && r > 0) plan += wxString::Format(", ramp-up %d s", r);
      else if (profile_ == 2 && r > 0) plan += d > 0 ? wxString::Format(", peak: ramp %d s up and down", r)
                                                     : wxString::Format(", ramp-up %d s (peak needs a Duration)", r);
      else if (profile_ == 3) plan += d > 0 ? wxString::Format(", spike %d to %d", spike_base(par), par)
                                            : wxString(" (spike needs a Duration)");
    }
    // More iterations than the data file has rows: the later ones reuse its last row (Postman
    // and Newman do the same); said before the run, not only in the Preview.
    bool past_data = false;
    if (n && data_ && !data_->empty() && static_cast<size_t>(it) > data_->size() && dur_->value() <= 0) {
      past_data = true;
      plan += wxString::Format("\nIterations %zu-%d reuse the last data row (the file has %zu)", data_->size() + 1, it,
                               data_->size());
    }
    set_plan_text(plan);
    plan_label_->SetForegroundColour(past_data ? rv::Theme::get().warn : rv::Theme::get().dim);
    update_order_hint(false);
    update_plan_change(false);
    if (!run_) run_btn_->Enable(n > 0);
    if (!run_ && bar_) bar_->set_enabled(kRunAgain, n > 0 && !steps_.empty());
    relayout_left();
  }

  // The plan line under Run, wrapped to the panel once it has its width; at least two lines
  // high, so a plan that wraps while a field is typed into does not move the fields above.
  void set_plan_text(const wxString& t) { set_fixed_text(plan_label_, t, 2, wxString()); }

  // The height of `lines` lines in a label's font.
  static int lines_height(wxStaticText* l, int lines) {
    int w = 0, h = 0;
    l->GetTextExtent("Ag", &w, &h);
    return h * lines;
  }

  // A label of a fixed height: `t` wrapped to the panel and cut to `lines` lines (an ellipsis
  // ends the last one, and the tooltip has the whole text; else the tooltip is `tip`). The
  // label's size never changes, so nothing around it moves and no layout is needed.
  void set_fixed_text(wxStaticText* l, const wxString& t, int lines, const wxString& tip) {
    const int width = std::max(FromDIP(120), l->GetSize().x > FromDIP(60) ? l->GetSize().x - FromDIP(4)
                                                                          : left_->GetClientSize().x - FromDIP(36));
    wxString out;
    bool cut = false;
    if (!t.empty()) {
      wxClientDC dc(l);
      dc.SetFont(l->GetFont());
      std::vector<wxString> rows;
      for (const wxString& para : wxSplit(t, '\n', 0)) {
        wxString cur;
        for (const wxString& word : wxSplit(para, ' ', 0)) {
          const wxString next = cur.empty() ? word : cur + " " + word;
          if (!cur.empty() && dc.GetTextExtent(next).x > width) {
            rows.push_back(cur);
            cur = word;
          } else {
            cur = next;
          }
        }
        rows.push_back(cur);
      }
      if (rows.size() > static_cast<size_t>(lines)) {
        cut = true;
        rows.resize(static_cast<size_t>(lines));
        wxString& last = rows.back();
        const wxString ell = wxString::FromUTF8("\xE2\x80\xA6");
        while (!last.empty() && dc.GetTextExtent(last + ell).x > width) last.RemoveLast();
        last += ell;
      }
      for (size_t i = 0; i < rows.size(); ++i) out += (i ? "\n" : "") + rows[i];
    }
    if (l->GetLabel() != out) {
      const wxSize keep = l->GetSize();
      l->SetLabel(out);
      if (keep.y > 0) l->SetSize(keep);  // the sizer's size, not the text's
      l->Refresh();
    }
    const wxString want = cut ? t : tip;
    if (l->GetToolTipText() != want) {
      if (want.empty()) l->UnsetToolTip();
      else l->SetToolTip(want);
    }
  }

  // The order hint for the chosen requests; recomputed only when the choice or order changes.
  // Like update_plan_change(), lays the panel out again when the hint changes (unless told not to).
  bool update_order_hint(bool layout = true) {
    if (!order_hint_ || !items_) return false;
    std::vector<size_t> o = chosen();
    if (o == hint_for_ && hint_items_ == items_.get()) return false;
    hint_items_ = items_.get();
    hint_for_ = o;
    const auto hints = runner::capture_order_hints(*items_, o, 3);
    // At most two lines, so the request list keeps its rows: the users of one capture share a
    // sentence ("Get order, Create order use {{firstId}} before List items captures it"); the
    // tooltip has every hint.
    wxString full;
    std::vector<std::pair<std::string, std::string>> groups;  // (" use {{v}} before X captures it", users)
    for (const auto& h : hints) {
      const std::string& user = (*items_)[o[h.user]].req.name;
      const std::string& cap = (*items_)[o[h.capture]].req.name;
      full += wxString::FromUTF8((full.empty() ? "" : "\n") + user + " uses {{" + h.var + "}} before " + cap + " captures it");
      const std::string tail = "{{" + h.var + "}} before " + cap + " captures it";
      auto g = std::find_if(groups.begin(), groups.end(), [&](const auto& x) { return x.first == tail; });
      if (g == groups.end()) groups.emplace_back(tail, user);
      else g->second += ", " + user;
    }
    wxString text;
    for (const auto& [tail, users] : groups)
      text += wxString::FromUTF8((text.empty() ? "" : "; ") + users + (users.find(", ") == std::string::npos ? " uses " : " use ") + tail);
    if (!hints.empty()) {
      const std::string mv = "Move " + (*items_)[o[hints.front().capture]].req.name + " up";
      text += wxString::FromUTF8(" \xC2\xB7 " + mv + ".");
      full += wxString::FromUTF8("\n" + mv + " (drag or the arrows; the order is saved for the next run and --run).");
    }
    // A folder with captures keeps the hint's two lines whether or not a hint applies (a calm
    // line when the order is right): the list must not jump up under the mouse when a reorder
    // clears the hint, as the Plan changed note keeps its line.
    bool reserve = false;
    for (const RunItem& it : *items_)
      for (const auto& c : it.req.captures) reserve = reserve || (c.enabled && !c.var.empty());
    const auto& th = rv::Theme::get();
    wxString shown = text;
    if (shown.empty() && reserve)
      shown = wxString::FromUTF8("Captures run before the requests that use them \xE2\x9C\x93");
    order_hint_->SetForegroundColour(text.empty() ? th.dim : th.warn);
    // Two lines whether it warns or not: the request list's top (and so the chips a click aims
    // at) does not move when a reorder clears the warning.
    const int hint_h = reserve ? lines_height(order_hint_, 2) : -1;
    const bool grew = order_hint_->GetMinSize().y != hint_h;
    order_hint_->SetMinSize(wxSize(-1, hint_h));
    set_fixed_text(order_hint_, shown, 2, text.empty() && reserve ? wxString("Every {{var}} a request reads is captured by a request before it.") : full);
    order_hint_->InvalidateBestSize();
    const bool was = order_hint_->IsShown(), changed = was != !shown.empty() || (was && !reserve);
    order_hint_->Show(!shown.empty());
    if (changed && layout) relayout_left();
    if (changed || grew) fit_list_height();  // the hint above the list takes its room from the list
    return changed;
  }

  void set_running(bool on) {
    if (!on && detail_) detail_->set_defer_big(false);  // a body deferred while following: shown now
    run_btn_->set(on ? "Stop" : "Run", on);
    run_btn_->Enable(on || !chosen().empty());
    // Focus must not stay on a control about to be disabled: keys (Esc) would then reach no
    // window at all. The Run / Stop button keeps it.
    if (on) {
      wxWindow* f = FindFocus();
      for (wxWindow* w : std::initializer_list<wxWindow*>{iters_, delay_, par_, dur_, ramp_, thresh_, bail_, cookies_, keep_, filter_, list_})
        if (f && (f == w || f->GetParent() == w)) {
          run_btn_->SetFocus();
          break;
        }
      if (!f || !IsDescendant(f)) run_btn_->SetFocus();
    }
    for (wxWindow* w : std::initializer_list<wxWindow*>{iters_, delay_, par_, dur_, ramp_, thresh_, bail_, cookies_, keep_}) w->Enable(!on);
    for (int id = kProfFixed; id <= kProfSpike; ++id) prof_chips_->set_enabled(id, !on);
    data_chips_->set_enabled(kDataPick, !on);
    data_chips_->set_enabled(kDataClear, !on);
    update_filter_chips();
  }

  RunSummary summary_now(bool running) {
    return runner::summarize(steps_, planned_, running ? ms_between(started_, Clock::now()) : final_.wall_ms, false);
  }

  void tick() {
    if (!run_) return;
    drain(run_);
    if (++ticks_ % 3 != 0 && !first_tick_) return;  // the summary 10x a second
    first_tick_ = false;
    if (detail_due_ && ms_between(detail_at_, Clock::now()) >= 250) {
      detail_due_ = false;
      detail_at_ = Clock::now();
      if (follow_) show_selected(true, true);
    }
    // Percentiles over every finished step at most 10x a second, not per result.
    if (dirty_) {
      last_summary_ = runner::summarize(steps_, planned_, 0, false, view_mode_ == 1);
      dirty_ = false;
      update_filter_chips();
      if (view_mode_ == 1) refresh_stats(&last_summary_);
    }
    RunSummary s = last_summary_;
    s.wall_ms = ms_between(started_, Clock::now());
    if (!thresholds_.empty() && ticks_ % 15 == 0) eval_thresholds(s);
    if (view_mode_ == 3 && ++fail_ticks_ % 10 == 0) refresh_fails();  // ~1x a second while shown
    const int it = steps_.empty() ? 1 : steps_.back()->iteration + 1;
    std::string state = "Running";
    if (duration_ms_ > 0)
      state += " \xC2\xB7 " + fmt_ms(s.wall_ms) + " of " + fmt_ms(duration_ms_);
    else if (iterations_ > 1)
      state += " \xC2\xB7 iteration " + std::to_string(it) + " of " + std::to_string(iterations_);
    summary_->set(s, state, true);
    if (view_mode_ == 2 && ++chart_ticks_ % 3 == 0) chart_->set(&steps_, s.wall_ms);
  }

  void stop_run() {
    if (!run_) return;
    run_->ctl.stop();
    run_btn_->set("Stopping...", true);
    run_btn_->Disable();
  }

  // Run, Run again and Rerun failed send the requests as saved now: the folder is read again on
  // a worker first (an edit, a new or deleted file, a new order), then the run starts from that.
  void on_run(bool failed_only) {
    if (run_) {
      stop_run();
      return;
    }
    if (!items_ || preparing_) return;
    if (failed_only && !has_failed_to_rerun()) return;
    if (!failed_only && chosen().empty()) return;
    preparing_ = true;
    run_btn_->set("Reading...", true);
    run_btn_->Disable();
    reload(failed_only ? 2 : 1);
  }

  bool has_failed_to_rerun() const {
    if (!shown_is_latest() || !run_items_ || !run_order_) return false;
    for (const auto& s : steps_)
      if (s->failed() && s->pos < run_order_->size()) return true;
    return false;
  }

  void begin_run(bool failed_only) {
    run_btn_->set("Run", false);
    run_btn_->Enable(!chosen().empty());
    if (!items_ || run_) return;
    save_settings();
    Workspace* ws = workspace_ ? workspace_() : nullptr;
    if (!ws || runner_key(ws->root(), {}) != runner_key(root_, {})) {
      set_status_w("The workspace was closed or switched: open the runner again from the sidebar.");
      return;
    }
    std::vector<size_t> pick;
    int iterations = std::max(1, iters_->value());
    std::shared_ptr<const std::vector<RunItem>> items = items_;
    run_label_.clear();
    std::string rerun_note;
    if (failed_only) {
      // Every failed step of the run shown, once: per failed iteration, with that iteration's
      // data row and number, only the requests that failed in it (Postman's "Run failed"),
      // plus the requests before them whose captures they read (Login for its token), so a
      // rerun does not reuse values left from the last run. Matched to the fresh list by file.
      if (!shown_is_latest() || !run_items_ || !run_order_) return;
      std::map<fs::path, size_t> idx;
      for (size_t i = 0; i < items_->size(); ++i) idx.emplace((*items_)[i].req.file, i);
      std::vector<size_t> new_pos(run_order_->size(), kNoScope);
      for (size_t k = 0; k < run_order_->size(); ++k) {
        const size_t old = (*run_order_)[k];
        if (old >= run_items_->size()) continue;
        const auto it = idx.find((*run_items_)[old].req.file);
        if (it == idx.end()) continue;
        new_pos[k] = pick.size();
        pick.push_back(it->second);
      }
      std::map<int, std::vector<size_t>> per;
      size_t gone = 0;
      for (const auto& s : steps_)
        if (s->failed() && s->pos < run_order_->size()) {
          if (new_pos[s->pos] == kNoScope) ++gone;
          else per[s->iteration].push_back(new_pos[s->pos]);
        }
      if (per.empty()) {
        set_status_w(gone ? "The failed requests are no longer in this folder." : "");
        return;
      }
      retry_.clear();
      std::set<size_t> deps;
      size_t n_failed = 0;
      for (auto& [it, pos] : per) {
        std::sort(pos.begin(), pos.end());
        pos.erase(std::unique(pos.begin(), pos.end()), pos.end());
        n_failed += pos.size();
        std::vector<size_t> all = runner::with_capture_sources(*items_, pick, pos);
        for (size_t q : all)
          if (!std::binary_search(pos.begin(), pos.end(), q)) deps.insert(pick[q]);
        retry_.push_back({it, std::move(all)});
      }
      if (!deps.empty()) {
        std::string names;
        size_t shown = 0;
        for (size_t i : deps) {
          if (shown++ == 3) {
            names += " +" + std::to_string(deps.size() - 3);
            break;
          }
          const RunItem& r = (*items_)[i];
          names += (names.empty() ? "" : ", ") + (r.req.name.empty() ? r.rel : r.req.name);
        }
        rerun_note = ", with " + names + " first (their captures feed the failed ones)";
      } else {
        rerun_note = ", no other request first (variables from earlier runs are used as they are)";
      }
      if (gone) rerun_note += "; " + std::to_string(gone) + " failed no longer in the folder";
      items = items_;
      iterations = static_cast<int>(retry_.size());
      run_label_ = "rerun failed";
    } else {
      pick = chosen();
      retry_.clear();
      run_data_ = data_ && !data_->empty() ? data_ : nullptr;  // a later rerun uses the same rows
      if (run_data_) run_label_ = "data: " + data_name_;
    }
    auto order = std::make_shared<const std::vector<size_t>>(std::move(pick));
    if (order->empty()) return;
    {
      // The environment picked here, if it still exists (else the workspace's active one).
      bool found = env_.empty();
      for (const auto& e : ws->environments()) found = found || e.name == env_;
      if (!found) env_ = ws->active_env;
      update_env_label();
    }
    auto wsc = std::make_shared<Workspace>(*ws);  // the run's private copy: captures go here
    wsc->active_env = env_;
    std::vector<runner::Threshold> thresholds;
    {
      std::string terr;
      if (!runner::parse_thresholds(thresh_->GetValue().ToStdString(wxConvUTF8), &thresholds, &terr)) {
        set_status_w(U("Thresholds: " + terr));
        thresh_->SetFocus();
        return;
      }
    }
    thresholds_ = std::move(thresholds);
    summary_->set_thresholds({});
    run_ = std::make_shared<RunState>();
    steps_.clear();
    view_.clear();
    show_script_column(false);
    scope_item_ = kNoScope;
    scope_key_.clear();
    show_scope_chip(false, {});
    n_fail_groups_ = 0;
    pinned_fail_ = false;
    pinned_step_.reset();
    n_passed_ = n_failed_ = n_skipped_ = 0;
    table_->refresh_rows();
    shown_.reset();
    shown_record_.reset();
    detail_->show(nullptr, "Running...");
    have_final_ = false;
    iterations_ = iterations;
    run_order_ = order;
    run_items_ = items;
    duration_ms_ = failed_only ? 0 : dur_->value() * 1000;
    planned_ = duration_ms_ > 0 ? 0 : order->size() * static_cast<size_t>(iterations_);
    // A small run lists every check under its request, passed ones too (as Bruno does).
    table_->set_all_checks(planned_ > 0 && planned_ <= kAllChecksRun);
    if (failed_only) {
      planned_ = 0;
      for (const auto& r : retry_) planned_ += r.positions.size();
    }
    started_iso_ = runner::now_iso();
    cleared_runners().erase(key_);
    started_ = Clock::now();
    follow_ = true;
    set_running(true);
    if (failed_only) {
      // Said where it stays after the run (the status line), not only while it runs.
      const wxString what = wxString::Format("Rerun failed: %zu send%s in %zu iteration%s", planned_,
                                             planned_ == 1 ? "" : "s", retry_.size(), retry_.size() == 1 ? "" : "s") +
                            U(rerun_note);
      set_status_w(what);
      set_plan_text(what);
      relayout_left();
    } else {
      set_status_w("");
    }
    update_plan_change();
    summary_->set(RunSummary{0, planned_}, "Preparing", true);
    // Consent is judged on a worker (it resolves paths and reads the allowlists), then asked
    // here once for the whole run.
    auto st = run_;
    auto link = link_;
    std::thread([st, link, items, order, wsc] {
      runner::Consent c = runner::consent_needed(*wsc, *items, *order);
      wxTheApp->CallAfter([st, link, wsc, order, c = std::move(c)]() mutable {
        if (link->frame) link->frame->preflight_done(st, wsc, order, std::move(c));
      });
    }).detach();
  }

  // scripts: 0 do not run them, 1 run (trusted), 2 trust the workspace's scripts first.
  void start_run(const std::shared_ptr<RunState>& st, std::shared_ptr<Workspace> wsc,
                 std::shared_ptr<const std::vector<size_t>> order, runner::Consent approved, int scripts) {
    runner::RunOptions o;
    o.run_scripts = scripts != 0;
    o.iterations = iterations_;
    o.delay_ms = delay_->value();
    o.stop_on_failure = bail_->GetValue();
    o.concurrency = std::max(1, par_->value());
    run_parallel_ = o.concurrency > 1;
    {
      // The plan as By request names it when it compares this run with an earlier one.
      std::string p = std::to_string(order->size()) + " request" + (order->size() == 1 ? "" : "s");
      if (duration_ms_ > 0) p += " for " + std::to_string(duration_ms_ / 1000) + " s";
      else p += " x " + std::to_string(o.iterations) + " iteration" + (o.iterations == 1 ? "" : "s");
      if (o.concurrency > 1) p += ", " + std::to_string(o.concurrency) + " parallel";
      if (o.delay_ms > 0) p += ", delay " + std::to_string(o.delay_ms) + " ms";
      if (o.ramp_up_ms > 0 || o.spike_base > 0) p += profile_ == 3 ? ", spike" : profile_ == 2 ? ", peak" : ", ramp";
      if (!run_label_.empty()) p += ", " + run_label_;
      run_plan_ = std::move(p);
    }
    o.duration_ms = duration_ms_;
    o.cookies = cookies_->GetValue();
    o.spill_bodies = true;  // the full response of every step stays viewable (a temp file per big body)
    if (run_label_ != "rerun failed") {
      const int ramp_ms = ramp_->value() * 1000;
      if (profile_ == 1 || profile_ == 2) o.ramp_up_ms = ramp_ms;
      if (profile_ == 2 && duration_ms_ > 0) o.ramp_down_ms = std::min(ramp_ms, duration_ms_ / 2);
      if (profile_ == 3 && duration_ms_ > 0 && o.concurrency > 1) o.spike_base = spike_base(o.concurrency);
    }
    if (duration_ms_ > 0 && o.concurrency > 1 && o.iterations <= 1) o.iterations = runner::kMaxIterations;
    if (run_data_) o.data = *run_data_;
    if (run_label_ == "rerun failed") o.retry = retry_;
    started_ = Clock::now();
    first_tick_ = true;
    tick_.Start(33);
    tick();
    auto link = link_;
    auto items = run_items_;
    auto grpc = grpc_;
    std::thread([st, link, items, order, wsc, o = std::move(o), grpc, approved = std::move(approved), scripts]() mutable {
      for (const auto& f : approved.files) wsc->allow_file(f);
      for (const auto& v : approved.env_vars) wsc->allow_env_var(v);
      for (const auto& it : approved.network) wsc->allow_network_item(it);
      if (scripts == 2 && !wsc->trust_scripts(true)) o.run_scripts = false;  // the trust file is on disk: here
      // The UI collects finished steps on its timer (~30x a second): a post per step would make
      // the UI thread repaint thousands of times a second and slow the run down.
      auto on_step = [st](StepPtr s) {
        std::lock_guard lk(st->m);
        st->pending.push_back(std::move(s));
      };
      const RunSummary sum = runner::run_plan(*items, *order, o, *wsc, grpc, st->ctl, on_step, false);
      wxTheApp->CallAfter([st, link, sum, wsc] {
        if (link->frame) link->frame->run_done(st, sum, wsc);
      });
    }).detach();
  }

  // "Keep variable values": what the run captured goes back into the live workspace.
  void keep_values(const Workspace& run_ws) {
    Workspace* ws = workspace_ ? workspace_() : nullptr;
    if (!ws || runner_key(ws->root(), {}) != runner_key(root_, {})) return;
    for (const auto& [k, v] : run_ws.runtime_vars) ws->runtime_vars[k] = v;
    bool settings = false;
    for (const auto& s : steps_)
      for (const auto& c : s->captures) {
        if (!c.ok) continue;
        if (c.scope == Capture::Runtime) {
          if (c.removed) ws->runtime_vars.erase(c.var);
          continue;
        }
        if (c.scope == Capture::Globals) {
          if (c.removed) ws->globals.erase(c.var);
          else ws->globals[c.var] = c.value;
          settings = true;
        } else {
          for (auto& e : ws->environments())
            if (e.name == env_ && !e.secrets.count(c.var)) {  // never a secret
              if (c.removed) e.vars.erase(c.var);
              else e.vars[c.var] = c.value;
              settings = true;
            }
        }
      }
    if (settings) {
      auto copy = std::make_shared<Workspace>(*ws);
      std::thread([copy] { copy->save_settings(); }).detach();  // disk off the UI thread
    }
  }

  // ---- results view
  void rebuild_view(bool detail = true) {
    view_.clear();
    for (const auto& s : steps_)
      if (matches(*s)) view_.push_back(s);
    apply_sort(false);
    table_->refresh_rows();
    follow_ = false;
    if (!view_.empty()) {
      table_->SetSelection(0);
      table_->ScrollToRow(0);
      if (detail) show_selected();
    } else {
      table_->SetSelection(wxNOT_FOUND);
      shown_.reset();
      detail_->show(nullptr, steps_.empty() ? "" : "No results match this filter.");
    }
  }

  // The Script column (script time, apart from Time) takes room only when a step has any.
  void show_script_column(bool on) {
    if (cols_.scripts == on) return;
    cols_.scripts = on;
    if (!on && cols_.sort == 5) cols_.sort = -1;
    if (header_) header_->Refresh();
    if (table_) table_->Refresh();
  }

  void apply_sort(bool user) {
    auto key = [this](const StepResult& r) -> double {
      switch (cols_.sort) {
        case 1: return r.iteration;
        case 3: return r.skipped ? 1e9 : r.stopped() ? 1e8 : !r.sent ? 1e7 : r.status;
        case 4: return r.sent && !r.skipped ? r.timing.total_ms : -1;
        case 5: return r.sent && !r.skipped ? r.script_ms : -1;
        case 6: return static_cast<double>(r.size);
        case 7: return r.failed() ? 1000.0 + r.asserts_failed : r.asserts.empty() ? 0 : 1;
        default: return static_cast<double>(r.seq);
      }
    };
    const StepPtr keep = user ? shown_ : nullptr;
    if (cols_.sort < 0) {
      // No column picked: the # order (when each result arrived; plan order for a sequential
      // run), during the run and after it, so rows never jump when a parallel run ends.
      // steps_ is in plan order once a parallel run is done (exports, history).
      const auto by_seq = [](const StepPtr& a, const StepPtr& b) { return a->seq < b->seq; };
      if (!std::is_sorted(view_.begin(), view_.end(), by_seq)) std::stable_sort(view_.begin(), view_.end(), by_seq);
    } else if (cols_.sort == 1) {
      // Iteration, then the request's place in it: plan order.
      std::stable_sort(view_.begin(), view_.end(), [this](const StepPtr& a, const StepPtr& b) {
        if (a->iteration != b->iteration) return cols_.desc ? a->iteration > b->iteration : a->iteration < b->iteration;
        if (a->pos != b->pos) return a->pos < b->pos;
        return a->seq < b->seq;
      });
    } else if (cols_.sort == 2) {
      std::stable_sort(view_.begin(), view_.end(), [this](const StepPtr& a, const StepPtr& b) {
        const int c = a->rel.compare(b->rel);
        return cols_.desc ? c > 0 : c < 0 || (c == 0 && a->seq < b->seq);
      });
    } else {
      std::stable_sort(view_.begin(), view_.end(), [&](const StepPtr& a, const StepPtr& b) {
        const double x = key(*a), y = key(*b);
        if (x != y) return cols_.desc ? x > y : x < y;
        return a->seq < b->seq;
      });
    }
    if (!user) return;
    table_->refresh_rows();
    follow_ = false;
    if (keep)
      for (size_t i = 0; i < view_.size(); ++i)
        if (view_[i] == keep) {
          table_->SetSelection(static_cast<int>(i));
          table_->ensure_visible(i);
          return;
        }
    if (!view_.empty()) {
      table_->SetSelection(0);
      table_->ScrollToRow(0);
      show_selected();
    }
  }

  // 0 results, 1 by request, 2 over time, 3 failures.
  void show_mode(int mode) {
    view_mode_ = mode;
    bar_->set_on(kVResults, mode == 0);
    bar_->set_on(kVRequests, mode == 1);
    bar_->set_on(kVTime, mode == 2);
    bar_->set_on(kVFail, mode == 3);
    table_->Show(mode == 0);
    header_->Show(mode == 0);
    stats_->Show(mode == 1);
    chart_->Show(mode == 2);
    fails_->Show(mode == 3);
    if (mode == 1) refresh_stats();
    if (mode == 3) refresh_fails();
    if (mode == 2) chart_->set(&steps_, run_ ? ms_between(started_, Clock::now()) : final_.wall_ms);
    tpanel_->Layout();
  }

  void refresh_stats(const RunSummary* live = nullptr) {
    // A cause chip, a request scope or a Passed / Failed filter narrows this view as it narrows
    // Results: the counts and times are of the results that match.
    const bool narrowed = scope_item_ != kNoScope || !scope_key_.empty() || filter_mode_ != 0;
    std::vector<StepPtr> picked;
    if (narrowed) {
      picked.reserve(steps_.size());
      for (const auto& st : steps_)
        if (matches(*st)) picked.push_back(st);
    }
    const std::vector<StepPtr>& src = narrowed ? picked : steps_;
    RunSummary s = narrowed ? runner::summarize(src, 0, 0, false, true)
                   : live ? *live : have_final_ ? final_ : runner::summarize(steps_, planned_, 0, false, true);
    if (s.per_request.empty() && !src.empty()) s = runner::summarize(src, narrowed ? 0 : planned_, 0, false, true);
    // Compared with the latest run before the one shown that had the same plan (same folder,
    // this session): a 300 x 8 load run is never set against a single sequential pass.
    std::map<std::string, double> base;
    const auto& h = run_history()[key_];
    std::shared_ptr<const RunRecord> prev;
    const std::string plan = shown_record_ ? shown_record_->plan : run_plan_;
    bool after = !shown_record_;  // a live run: every record is an earlier one
    bool other_plans = false;
    for (const auto& r : h) {
      if (after) {
        if (r->plan == plan) {
          prev = r;
          break;
        }
        other_plans = true;
      }
      if (r == shown_record_) after = true;
    }
    if (prev)
      for (const auto& q : prev->summary.per_request) base[q.rel] = q.p50_ms;
    std::string base_tip;
    if (prev)
      base_tip = "\xCE\x94 P50: the change against the run of " + iso_local(prev->started_iso, false) +
                 ", the latest earlier run with the same plan (" + plan + ")";
    else if (other_plans)
      base_tip = "No \xCE\x94 P50 column: no earlier run in this session had the same plan (" + plan + ")";
    if (stats_->GetToolTipText() != U(base_tip)) {
      if (base_tip.empty()) stats_->UnsetToolTip();
      else stats_->SetToolTip(U(base_tip));
    }
    // Why each request failed: its most frequent cause (groups are sorted most frequent first).
    std::map<size_t, StatsList::Top> top;
    if (s.failed || n_failed_)
      for (const auto& g : runner::group_failures(src)) {
        auto [it, fresh] = top.try_emplace(g.item);
        if (fresh) {
          it->second.message = g.message;
          it->second.count = g.count;
        }
        ++it->second.causes;
      }
    stats_->set(s.per_request, std::move(base), prev ? iso_local(prev->started_iso, false) : std::string(), std::move(top));
  }

  void refresh_fails() {
    auto groups = runner::group_failures(steps_);
    n_fail_groups_ = groups.size();
    std::map<size_t, size_t> sends;
    int reached = 0;
    for (const auto& s : steps_)
      if (!s->skipped && !s->stopped()) {
        ++sends[s->item];
        reached = std::max(reached, s->iteration + 1);
      }
    if (fails_) fails_->set(std::move(groups), std::move(sends), reached);
    if (bar_) bar_->set_label(kVFail, n_fail_groups_ ? "Failures  " + std::to_string(n_fail_groups_) : std::string("Failures"));
  }

  void eval_thresholds(const RunSummary& s) {
    thr_ok_ = true;
    if (thresholds_.empty()) {
      summary_->set_thresholds({});
      return;
    }
    auto r = runner::check_thresholds(thresholds_, s);
    for (const auto& t : r) thr_ok_ = thr_ok_ && t.pass;
    summary_->set_thresholds(std::move(r));
  }

  // part: 0 all at once; 1 the results table only, 2 the rest (summary, views, the detail of
  // the first row) of a record part 1 put up: a restore spread over two timer ticks.
  void show_record(const std::shared_ptr<const RunRecord>& rec, bool announce, int part = 0) {
    if (run_ || !rec) return;
    if (part == 2) {
      if (shown_record_ != rec) return;
      finish_record(rec, announce);
      if (!view_.empty() && table_->GetSelection() == 0) show_selected();
      return;
    }
    shown_record_ = rec;
    steps_ = *rec->steps;
    show_script_column(std::any_of(steps_.begin(), steps_.end(), [](const StepPtr& p) { return p->script_ms > 0; }));
    run_items_ = rec->items;
    if (rec->order) run_order_ = rec->order;
    planned_ = rec->planned;
    iterations_ = rec->iterations;
    table_->set_all_checks(planned_ > 0 && planned_ <= kAllChecksRun);
    final_ = rec->summary;
    have_final_ = true;
    started_iso_ = rec->started_iso;
    scope_item_ = kNoScope;
    scope_key_.clear();
    show_scope_chip(false, {});
    n_passed_ = n_failed_ = n_skipped_ = 0;
    for (const auto& s : steps_) count(*s);
    shown_.reset();
    rebuild_view(part != 1);
    if (part == 1) {
      update_plan_change();
      return;
    }
    finish_record(rec, announce);
  }

  void finish_record(const std::shared_ptr<const RunRecord>& rec, bool announce) {
    refresh_fails();
    eval_thresholds(final_);
    summary_->set(final_, state_text(final_), false);
    update_filter_chips();
    if (view_mode_ == 1) refresh_stats();
    if (view_mode_ == 2) chart_->set(&steps_, final_.wall_ms);
    if (announce) {
      const auto& h = run_history()[key_];
      size_t idx = 0;
      while (idx < h.size() && h[idx] != rec) ++idx;
      // Its own plan, not the settings on the left (a rerun of 64 failed sends is not "9 x 1").
      set_status_w(U("Showing the run of " + iso_local(rec->started_iso) + (idx == 0 ? " (latest)" : " (" + std::to_string(idx) + " before the latest)") +
                     (rec->label.empty() ? "" : kDot + rec->label) + kDot + std::to_string(rec->planned) + " send" +
                     (rec->planned == 1 ? "" : "s") + ", " + std::to_string(rec->iterations) + " iteration" +
                     (rec->iterations == 1 ? "" : "s") + (rec->environment.empty() ? "" : std::string(kDot) + "env " + rec->environment)));
    }
  }

  void history_menu() {
    const auto& h = run_history()[key_];
    if (h.empty()) return;
    wxMenu m;
    for (size_t i = 0; i < h.size(); ++i) {
      const RunRecord& r = *h[i];
      std::string t = iso_local(r.started_iso, false);
      t += "   " + std::to_string(r.summary.passed) + " passed, " + std::to_string(r.summary.failed) + " failed" + "   " +
           fmt_ms(r.summary.wall_ms) + "   p50 " + fmt_ms(r.summary.p50_ms);
      if (!r.label.empty()) t += "   (" + r.label + ")";
      wxMenuItem* it = m.AppendCheckItem(1000 + static_cast<int>(i), U(t));
      it->Check(h[i] == shown_record_);
    }
    m.AppendSeparator();
    m.Append(999, "Compare: By request shows the change against the last run with the same plan");
    m.Enable(999, false);
    const int id = GetPopupMenuSelectionFromUser(m);
    if (id >= 1000 && static_cast<size_t>(id - 1000) < h.size()) {
      cleared_runners().erase(key_);
      show_record(h[static_cast<size_t>(id - 1000)], true);
    }
  }

  // The OS file dialog runs on its own thread (owned by this window, which it disables while it
  // is open): creating and closing it takes 50-200 ms that the UI thread would otherwise stall.
  void pick_data() {
    if (picking_) return;
    picking_ = true;
    auto link = link_;
    const HWND owner = static_cast<HWND>(GetHWND());
    std::thread([link, owner] {
      fs::path got;
      if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        IFileOpenDialog* d = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) {
          const COMDLG_FILTERSPEC types[] = {{L"Data files (*.csv; *.json)", L"*.csv;*.json"}, {L"All files (*.*)", L"*.*"}};
          d->SetFileTypes(2, types);
          d->SetTitle(L"Data file for the iterations");
          FILEOPENDIALOGOPTIONS fo = 0;
          if (SUCCEEDED(d->GetOptions(&fo))) d->SetOptions(fo | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
          if (SUCCEEDED(d->Show(IsWindow(owner) ? owner : nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(d->GetResult(&item))) {
              PWSTR path = nullptr;
              if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                got = path;
                CoTaskMemFree(path);
              }
              item->Release();
            }
          }
          d->Release();
        }
        CoUninitialize();
      }
      wxTheApp->CallAfter([link, got] {
        if (link->frame) link->frame->data_picked(got);
      });
    }).detach();
  }

 public:
  void data_picked(const fs::path& p) {
    picking_ = false;
    if (p.empty() || run_) return;
    data_path_ = p;
    const uint64_t gen = ++data_gen_;
    data_label_->SetLabel("Reading " + wxString(p.filename().wstring()) + "...");
    data_label_->SetForegroundColour(rv::Theme::get().dim);
    relayout_left();
    auto link = link_;
    std::thread([link, p, gen] {
      auto rows = std::make_shared<std::vector<runner::DataRow>>();
      std::string err;
      runner::load_data_file(p, rows.get(), &err);
      const std::string name = u8(p.filename());
      wxTheApp->CallAfter([link, gen, rows, name, err] {
        if (link->frame) link->frame->data_loaded(gen, rows, name, err);
      });
    }).detach();
  }

 private:

  // Postman's / Bruno's data file Preview: the rows as a table with a header row, iteration by
  // iteration. A virtual list: a 100 000-row file opens as fast as a 5-row one.
  void preview_data() {
    if (!data_ || data_->empty()) return;
    auto rows = data_;  // kept alive by the list while the dialog is open
    // Columns: every key of the first rows, in first-seen order (JSON rows may differ).
    std::vector<std::string> cols;
    for (size_t i = 0; i < std::min<size_t>(rows->size(), 1000); ++i)
      for (const auto& [k, v] : (*rows)[i])
        if (std::find(cols.begin(), cols.end(), k) == cols.end()) cols.push_back(k);
    auto cell = [](const runner::DataRow& r, size_t c, const std::string& key) {
      const std::string* v = nullptr;
      if (c < r.size() && r[c].first == key) v = &r[c].second;
      else
        for (const auto& kv : r)
          if (kv.first == key) {
            v = &kv.second;
            break;
          }
      if (!v) return std::string();
      std::string o = *v;
      for (auto& ch : o)
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
      if (u8_width(o) > 200) o = u8_head(o, 197) + "...";
      return o;
    };
    class Table : public wxListCtrl {
     public:
      Table(wxWindow* p, std::shared_ptr<std::vector<runner::DataRow>> r, std::vector<std::string> c,
            std::function<std::string(const runner::DataRow&, size_t, const std::string&)> f)
          : wxListCtrl(p, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_VIRTUAL | wxLC_HRULES | wxBORDER_NONE),
            rows_(std::move(r)), cols_(std::move(c)), cell_(std::move(f)) {}
      wxString OnGetItemText(long item, long col) const override {
        if (item < 0 || static_cast<size_t>(item) >= rows_->size()) return {};
        if (col == 0) return wxString::Format("%ld", item + 1);
        const size_t c = static_cast<size_t>(col - 1);
        return c < cols_.size() ? U(cell_((*rows_)[static_cast<size_t>(item)], c, cols_[c])) : wxString();
      }
      std::shared_ptr<std::vector<runner::DataRow>> rows_;
      std::vector<std::string> cols_;
      std::function<std::string(const runner::DataRow&, size_t, const std::string&)> cell_;
    };
    const auto& th = rv::Theme::get();
    wxDialog dlg(this, wxID_ANY, U(data_name_ + kDot + std::to_string(rows->size()) + " rows"), wxDefaultPosition,
                 FromDIP(wxSize(760, 460)), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    dlg.SetBackgroundColour(th.bg);
    auto* sz = new wxBoxSizer(wxVERTICAL);
    auto* tc = new Table(&dlg, rows, cols, cell);
    tc->SetBackgroundColour(th.bg);
    tc->SetForegroundColour(th.fg);
    // Widths from the header and the first rows' text (at most 320 px a column).
    {
      wxClientDC dc(tc);
      dc.SetFont(tc->GetFont());
      const int padw = FromDIP(18);
      tc->AppendColumn("Iteration", wxLIST_FORMAT_RIGHT,
                       std::max(dc.GetTextExtent("Iteration").x, dc.GetTextExtent(std::to_string(rows->size())).x) + padw);
      for (size_t c = 0; c < cols.size(); ++c) {
        int w = dc.GetTextExtent(U(cols[c])).x;
        for (size_t i = 0; i < std::min<size_t>(rows->size(), 200); ++i)
          w = std::max(w, dc.GetTextExtent(U(cell((*rows)[i], c, cols[c]))).x);
        tc->AppendColumn(U(cols[c]), wxLIST_FORMAT_LEFT, std::min(w + padw, FromDIP(320)));
      }
    }
    tc->SetItemCount(static_cast<long>(rows->size()));
    auto* hint = new wxStaticText(&dlg, wxID_ANY, "Iteration i uses row i (the last row again when there are more iterations than rows).");
    hint->SetForegroundColour(th.dim);
    sz->Add(tc, 1, wxEXPAND | wxALL, FromDIP(10));
    sz->Add(hint, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    dlg.SetSizer(sz);
    dlg.Bind(wxEVT_CHAR_HOOK, [&dlg](wxKeyEvent& e) {
      if (e.GetKeyCode() == WXK_ESCAPE) dlg.EndModal(wxID_CANCEL);
      else e.Skip();
    });
    tc->SetFocus();
    dlg.ShowModal();
  }

  void on_chip(int id) {
    if (probing_) {  // the layout check's hit test (chips_now)
      probe_hit_ = id;
      return;
    }
    switch (id) {
      case kAll: return check_all(true);
      case kNone: return check_all(false);
      case kUp:
      case kDown: {
        const int s = list_->GetSelection();
        if (s != wxNOT_FOUND) move_sel(static_cast<size_t>(s), id == kUp ? -1 : 1);
        return;
      }
      case kDataPick: return pick_data();
      case kDataPreview: return preview_data();
      case kEnvPick: return env_menu();
      case kDataClear:
        ++data_gen_;
        data_path_.clear();
        data_.reset();
        data_name_.clear();
        data_label_->SetLabel("");
        data_chips_->set_label(kDataPick, "Choose CSV / JSON...");
        data_chips_->set_visible(kDataClear, false);
        data_chips_->set_visible(kDataPreview, false);
        // Iterations set by the file (not changed since) go back to what they were before it.
        if (iters_auto_ > 0 && iters_->value() == iters_auto_)
          iters_->set_value(iters_before_data_ > 0 ? iters_before_data_ : 1);
        iters_auto_ = iters_before_data_ = -1;
        update_counts();
        relayout_left();
        return;
      case kFAll:
      case kFPassed:
      case kFFailed:
      case kFSkipped: {
        filter_mode_ = id - kFAll;
        for (int c = kFAll; c <= kFSkipped; ++c) bar_->set_on(c, c == id);
        const bool was_hidden = view_mode_ != 0;
        if (was_hidden) show_mode(0);
        rebuild_view();
        if (was_hidden && !view_.empty()) {
          // The table was hidden (By request / Over time): scroll again once it has its size,
          // so the first match is the row in view and selected, not an old scroll position.
          CallAfter([this] {
            if (view_.empty() || view_mode_ != 0) return;
            table_->SetSelection(0);
            table_->ScrollToRow(0);
            show_selected();
          });
        }
        table_->SetFocus();  // PgDn / arrows go to the results, not to the chip bar
        return;
      }
      case kVResults:
      case kVRequests:
      case kVTime:
      case kVFail:
        show_mode(id - kVResults);
        if (id == kVRequests) stats_->SetFocus();
        else if (id == kVResults) table_->SetFocus();
        else if (id == kVFail) fails_->SetFocus();
        return;
      case kScope: return clear_scope();
      case kReset: return reset_results();
      case kOrderReset: return reset_order();
      case kRunAgain: return on_run(false);
      case kRerunFailed: return on_run(true);
      case kHistory: return history_menu();
      case kHtml: {
        wxMenu m;
        m.Append(1, "HTML report...");
        m.Append(2, "JSON...");
        m.Append(3, "JUnit XML (CI)...");
        const int got = GetPopupMenuSelectionFromUser(m);
        if (got == 1) export_as(2);
        else if (got == 2) export_as(0);
        else if (got == 3) export_as(1);
        return;
      }
      case kJson: return export_as(0);
      case kJunit: return export_as(1);
      default: return;
    }
  }

  void export_as(int kind) {  // 0 JSON, 1 JUnit, 2 HTML
    if (steps_.empty() || run_) return;
    std::string base = name_s_.empty() ? "run" : name_s_;
    for (auto& c : base)
      if (std::string_view("\\/:*?\"<>|").find(c) != std::string_view::npos) c = '_';
    static const char* const titles[] = {"Export JSON", "Export JUnit XML", "Save HTML report"};
    static const char* const names[] = {"-run.json", "-junit.xml", "-report.html"};
    static const char* const wild[] = {"JSON (*.json)|*.json", "JUnit XML (*.xml)|*.xml", "HTML (*.html)|*.html"};
    wxFileDialog dlg(this, titles[kind], "", U(base + names[kind]), wild[kind], wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return;
    const fs::path out(dlg.GetPath().ToStdWstring());
    runner::RunMeta meta;
    meta.name = name_s_;
    meta.environment = shown_record_ ? shown_record_->environment : env_;
    meta.started_iso = started_iso_;
    meta.iterations = iterations_;
    auto steps = std::make_shared<std::vector<StepPtr>>(steps_);  // pointers only
    const RunSummary s = have_final_ ? final_ : runner::summarize(steps_, planned_, 0, true);
    set_status_w("Exporting...");
    auto link = link_;
    std::thread([link, steps, s, meta, out, kind] {
      const std::string text = kind == 1 ? runner::to_junit(meta, s, *steps)
                               : kind == 2 ? runner::to_html(meta, s, *steps)
                                           : runner::to_json(meta, s, *steps);
      std::ofstream f(out, std::ios::binary | std::ios::trunc);
      f.write(text.data(), static_cast<std::streamsize>(text.size()));
      const bool ok = static_cast<bool>(f);
      f.close();
      const std::string msg = ok ? "Saved " + std::to_string(steps->size()) + " results to " + u8(out.filename())
                                 : "Could not write " + u8(out);
      wxTheApp->CallAfter([link, msg] {
        if (link->frame) link->frame->set_status(msg);
      });
    }).detach();
  }

  std::function<Workspace*()> workspace_;
  std::function<std::shared_ptr<GrpcClient>()> grpc_;
  fs::path root_, folder_;
  std::wstring key_;
  std::string name_s_;
  std::string env_;
  std::shared_ptr<Link> link_;
  std::shared_ptr<std::vector<RunItem>> items_;
  std::vector<std::string> tags_;  // per item: "#a #b"
  std::vector<size_t> order_;      // display order: indexes into items_
  std::vector<char> checked_;      // per display position
  std::vector<size_t> visible_;    // display positions that pass the filter (list rows)
  std::shared_ptr<RunState> run_;
  std::shared_ptr<const std::vector<size_t>> run_order_;
  std::shared_ptr<const std::vector<RunItem>> run_items_;  // items of the run shown
  std::string run_label_;
  std::string run_plan_;  // the running / latest run's plan (RunRecord::plan)
  std::vector<StepPtr> steps_;  // every step of the run shown
  std::vector<StepPtr> view_;   // what the table shows (filtered, sorted)
  size_t n_passed_ = 0, n_failed_ = 0, n_skipped_ = 0;
  int filter_mode_ = 0;         // 0 all, 1 passed, 2 failed, 3 skipped
  int view_mode_ = 0;           // 0 results, 1 by request, 2 over time, 3 failures
  static constexpr size_t kNoScope = static_cast<size_t>(-1);
  size_t scope_item_ = kNoScope;  // Results narrowed to this request (item)...
  std::string scope_key_;         // ... or to this failure group (runner::failure_key)
  size_t n_fail_groups_ = 0;
  StepPtr pinned_step_;           // the row pinned_fail_ selected (a user's pick differs)
  bool pinned_fail_ = false;      // the run's first failure was selected (following stopped)
  unsigned fail_ticks_ = 0;
  std::vector<runner::Threshold> thresholds_;  // of the run shown
  bool thr_ok_ = true;
  ColModel cols_;
  size_t planned_ = 0;
  int iterations_ = 1;
  int duration_ms_ = 0;  // the run's load profile duration (0: a fixed number of iterations)
  Clock::time_point started_{};
  std::string started_iso_;
  RunSummary final_, last_summary_;
  bool have_final_ = false;
  bool run_parallel_ = false;  // the latest run had more than one worker
  bool dirty_ = false;
  bool follow_ = true;
  bool detail_due_ = false;
  unsigned ticks_ = 0;
  bool first_tick_ = true;          // the followed row changed since the detail was drawn
  Clock::time_point detail_at_{};    // when the detail pane was last updated by following
  std::shared_ptr<const RunRecord> shown_record_;
  std::shared_ptr<std::vector<runner::DataRow>> data_;
  std::shared_ptr<std::vector<runner::DataRow>> run_data_;  // the rows of the run shown (live or last)
  std::vector<runner::RunOptions::Retry> retry_;           // a rerun's failed steps per iteration
  std::string data_name_;
  uint64_t data_gen_ = 0;
  wxTimer tick_;

  wxPanel* left_ = nullptr;   // a wxScrolled<wxPanel>
  rv::ChipBar* rail_ = nullptr;
  bool collapsed_ = false;
  wxPanel* right_ = nullptr;
  wxTimer stage_;
  fs::path data_path_;   // the data file picked (remembered with the settings)
  int restore_iters_ = -1;  // iterations restored with a data file (its load keeps them)
  int iters_auto_ = -1;         // iterations the data file set (-1: none)
  int iters_before_data_ = -1;  // iterations before the data file set them
  wxTimer save_order_;                 // debounces writing run-order.json after a reorder
  wxTimer reload_timer_;               // debounces re-reading the folder after a change on disk
  wxTimer restore_;                    // shows a restored last run in steps
  std::shared_ptr<const RunRecord> restore_rec_;
  int restore_phase_ = 0;
  std::unique_ptr<WorkspaceWatcher> watcher_;
  uint64_t reload_seq_ = 0;
  int reload_then_ = 0;   // what the pending reload starts: 0 nothing, 1 a run, 2 rerun failed
  bool preparing_ = false;  // Run pressed: the folder is being read again
  bool reload_due_ = false; // the folder changed during a run: read it again after it
  wxStaticText* changed_label_ = nullptr;
  runner::FolderOrder order_dirty_;    // folders reordered since the last write
  wxStaticText* order_note_ = nullptr; // "Run order: run-order.json ... the sidebar keeps its own"
  bool saved_order_ = false;           // this folder has a saved run order
  static constexpr int kLeftBuilt = 3;
  int left_stage_ = 0;   // 0 nothing built, 1 the list, 2 the first settings, 3 (kLeftBuilt) all of them
  int right_stage_ = 0;  // 0 nothing, 1 everything but the detail pane (detail_ set: all of it)
  wxBoxSizer* left_sizer_ = nullptr;
  std::shared_ptr<std::vector<RunItem>> pending_items_;
  std::vector<std::string> pending_errs_;
  rv::ChipBar* env_chips_ = nullptr;
  wxStaticText* req_title_ = nullptr;
  wxStaticText* plan_label_ = nullptr;
  wxString changed_text_;  // the "Plan changed" note's whole text (empty: none)
  wxStaticText* order_hint_ = nullptr;
  std::vector<size_t> hint_for_;
  const void* hint_items_ = nullptr;
  wxStaticText* status_ = nullptr;
  wxStaticText* data_label_ = nullptr;
  wxTextCtrl* filter_ = nullptr;
  RequestList* list_ = nullptr;
  NumberField* iters_ = nullptr;
  NumberField* delay_ = nullptr;
  NumberField* par_ = nullptr;
  NumberField* dur_ = nullptr;   // load profile: seconds, 0 = off
  NumberField* ramp_ = nullptr;  // load profile: ramp-up seconds
  rv::ChipBar* prof_chips_ = nullptr;
  int profile_ = 0;  // 0 fixed, 1 ramp up, 2 peak, 3 spike

  static int spike_base(int workers) { return std::max(1, workers / 4); }
  void set_profile(int p, bool user) {
    if (p < 0 || p > 3) return;
    profile_ = p;
    for (int i = 0; i < 4; ++i) prof_chips_->set_on(kProfFixed + i, i == p);
    if (user && (p == 1 || p == 2) && ramp_->value() == 0) ramp_->SetFocus();
    update_counts();
  }
  wxCheckBox* bail_ = nullptr;
  wxCheckBox* keep_ = nullptr;
  wxCheckBox* cookies_ = nullptr;
  AccentButton* run_btn_ = nullptr;
  rv::ChipBar* left_chips_ = nullptr;
  rv::ChipBar* scope_bar_ = nullptr;  // the scope chip, at the status line's start
  wxBoxSizer* scope_row_ = nullptr;
  rv::ChipBar* data_chips_ = nullptr;
  rv::ChipBar* bar_ = nullptr;
  SummaryStrip* summary_ = nullptr;
  wxSplitterWindow* split_ = nullptr;
  wxPanel* tpanel_ = nullptr;
  ResultsList* table_ = nullptr;
  StatsList* stats_ = nullptr;
  TimeChart* chart_ = nullptr;
  FailList* fails_ = nullptr;
  wxTextCtrl* thresh_ = nullptr;
  bool picking_ = false;  // the data file dialog is open (on its own thread)
  unsigned chart_ticks_ = 0;
  GridHeader* header_ = nullptr;
  StepPtr shown_;
  runner_ui::DetailPane* detail_ = nullptr;
};

}  // namespace

void open_runner(wxWindow* parent, std::function<Workspace*()> workspace, const fs::path& folder,
                 std::function<std::shared_ptr<GrpcClient>()> grpc, bool select, int at) {
  Workspace* ws = workspace ? workspace() : nullptr;
  if (!ws) return;
  const std::wstring key = runner_key(ws->root(), folder);
  // The main window's tab notebook: the runner opens as a tab next to the requests.
  std::function<wxAuiNotebook*(wxWindow*)> find_nb = [&](wxWindow* w) -> wxAuiNotebook* {
    if (auto* nb = dynamic_cast<wxAuiNotebook*>(w)) return nb;
    for (wxWindow* c : w->GetChildren())
      if (auto* nb = find_nb(c)) return nb;
    return nullptr;
  };
  wxAuiNotebook* nb = parent ? find_nb(parent) : nullptr;
  if (!nb) return;
  auto& open = open_runners();
  if (auto it = open.find(key); it != open.end()) {
    if (wxWindow* f = it->second.get()) {
      if (const int i = nb->GetPageIndex(f); i != wxNOT_FOUND) {
        if (!select) return;
        nb->SetSelection(static_cast<size_t>(i));
        if (auto* r = dynamic_cast<RunnerFrame*>(f)) r->refresh();
        return;
      }
    }
    open.erase(it);
  }
  auto* f = new RunnerFrame(nb, std::move(workspace), *ws, folder, std::move(grpc));
  open[key] = f;
  // Added, then selected on the next turn of the message loop: with many tabs open, the tab
  // strip's layout and the page switch are 15-40 ms each, two steps rather than one.
  if (at >= 0 && static_cast<size_t>(at) < nb->GetPageCount()) nb->InsertPage(static_cast<size_t>(at), f, f->tab_title(), false);
  else nb->AddPage(f, f->tab_title(), false);
  if (!select) return;
  wxWeakRef<wxAuiNotebook> book(nb);
  wxWeakRef<wxWindow> page(f);
  next_turn([book, page] {
    if (!book || !page) return;
    if (const int i = book->GetPageIndex(page.get()); i != wxNOT_FOUND) book->SetSelection(static_cast<size_t>(i));
  });
}

bool runner_folder(wxWindow* page, fs::path* folder) {
  auto* r = dynamic_cast<RunnerFrame*>(page);
  if (!r) return false;
  if (folder) *folder = r->folder();
  return true;
}

bool runner_shortcut(wxWindow* page, bool failed_only) {
  auto* r = dynamic_cast<RunnerFrame*>(page);
  if (!r) return false;
  r->run_from_shell(failed_only);
  return true;
}

}  // namespace watt::app

// ------------------------------------------------------------------ dev hook and CLI
// WATT_RUN_FOLDER=<folder> opens the runner for that folder once the main window and its
// workspace are up (benchmarks, screenshots). Unset: nothing happens (one getenv at start-up).
//
// watt.exe --run <folder> [options] (runner_cli.cpp): a headless run for CI. It runs here,
// before the app creates any window, writes its text to the inherited stdout (a pipe or file) or
// the parent's console, and ends the process with the run's exit code.
namespace watt::app {
namespace {

void cli_write(const std::string& text) {
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode = 0;
  if (h && h != INVALID_HANDLE_VALUE && !GetConsoleMode(h, &mode)) {
    DWORD n = 0;
    WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);  // pipe / file: UTF-8
    return;
  }
  if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
  h = GetStdHandle(STD_OUTPUT_HANDLE);
  HANDLE con = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  const wxString w = wxString::FromUTF8(text);
  DWORD n = 0;
  if (con != INVALID_HANDLE_VALUE) {
    WriteConsoleW(con, w.wc_str(), static_cast<DWORD>(w.length()), &n, nullptr);
    CloseHandle(con);
  }
}

// The command line when it asks for a headless run (empty otherwise).
std::vector<std::string> cli_args() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::string> out;
  if (!argv) return out;
  bool run = false;
  for (int i = 1; i < argc; ++i) {
    const wxString a(argv[i]);
    run = run || a == "--run";
    out.push_back(a.ToStdString(wxConvUTF8));
  }
  LocalFree(argv);
  if (!run) out.clear();
  return out;
}

// watt --help / -h / /? / --version (any position).
bool help_asked() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return false;
  bool help = false;
  for (int i = 1; i < argc && !help; ++i) {
    const std::wstring a = argv[i];
    help = a == L"--help" || a == L"-h" || a == L"/?" || a == L"-?" || a == L"--version";
  }
  LocalFree(argv);
  return help;
}

class RunnerDevHook : public wxModule {
 public:
  bool OnInit() override {
    if (help_asked()) {
      // watt --help: usage on the console, no window (and no workspace folder named "--help").
      cli_write("usage: watt [<workspace folder>] [--new-instance]\n"
                "       watt --run <folder or .req.json> [--workspace <dir>] [--env <name>] [--iterations <n>]\n"
                "              [--delay <ms>] [--data <file.csv|file.json>] [--filter \"<tags>\"] [--parallel <n>]\n"
                "              [--duration <s>] [--ramp-up <s>] [--threshold \"p95<200 err<1%\"] [--bail] [--no-cookies]\n"
                "              [--trust-scripts] [--reporter-junit <file>] [--reporter-json <file>]\n"
                "              [--reporter-html <file>] [--quiet]\n\n"
                "--run runs the requests of a folder headless (CI) and exits 0 when all passed, 1 on failures,\n"
                "2 on a usage or setup error. A relative folder is looked up below --workspace.\n");
      TerminateProcess(GetCurrentProcess(), 0);
    }
    if (auto args = cli_args(); !args.empty()) {
      std::string out;
      const int code = runner::run_cli(args, [] { return std::make_shared<GrpcClient>(); }, &out);
      cli_write(out);
      // No window was ever created: end here (the gRPC / curl threads need no orderly shutdown).
      TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    }
    wxString dir;
    if (!wxGetEnv("WATT_RUN_FOLDER", &dir) || dir.empty()) return true;
    // Modules start before the app's OnInit: wait for the event loop, then for the main frame.
    if (wxTheApp) wxTheApp->CallAfter([dir] { poll(dir, 0); });
    return true;
  }
  void OnExit() override {}

 private:
  static void poll(const wxString& dir, int tries) {
    auto* t = new wxTimer();
    t->Bind(wxEVT_TIMER, [t, dir, tries](wxTimerEvent&) {
      auto* host = dynamic_cast<SidebarHost*>(wxTheApp->GetTopWindow());
      if (host) host->run_folder(std::filesystem::path(dir.ToStdWstring()));
      else if (tries < 100) poll(dir, tries + 1);
      wxTheApp->CallAfter([t] { delete t; });
    });
    t->StartOnce(300);
  }
  wxDECLARE_DYNAMIC_CLASS(RunnerDevHook);
};
wxIMPLEMENT_DYNAMIC_CLASS(RunnerDevHook, wxModule);
}  // namespace
}  // namespace watt::app
