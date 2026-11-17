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
#include <wx/menu.h>
#include <wx/module.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
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
#include <cmath>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#ifdef small
#undef small  // rpcndr.h: "#define small char"
#endif

#include "app/panels.h"            // SidebarHost
#include "app/response_widgets.h"  // rv::Theme, ChipBar
#include "app/runner_detail.h"
#include "app/runner_engine.h"
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
};

std::map<std::wstring, std::deque<std::shared_ptr<const RunRecord>>>& run_history() {
  static std::map<std::wstring, std::deque<std::shared_ptr<const RunRecord>>> m;
  return m;
}
constexpr size_t kHistoryRuns = 12;
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
    wxColour c = danger_ ? th.err : th.accent;
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
    const int limit = cs.x - pad - sz.x - FromDIP(16);
    const int y = FromDIP(8);
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
    const int avail = limit - pad;
    std::vector<int> w = widths(vfont);
    int gap = FromDIP(22);
    const int min_gap = FromDIP(10);
    auto fits = [&] { return w.empty() || sum(w) + gap * static_cast<int>(w.size() - 1) <= avail; };
    if (!fits() && w.size() > 1) gap = std::max(min_gap, (avail - sum(w)) / static_cast<int>(w.size() - 1));
    for (float k : {1.3f, 1.15f, 1.0f}) {
      if (fits()) break;
      vfont = GetFont().Bold().Scaled(k);
      w = widths(vfont);
      if (w.size() > 1) gap = std::clamp((avail - sum(w)) / static_cast<int>(w.size() - 1), min_gap, FromDIP(22));
    }
    for (const char* drop : {"P90", "STD DEV", "MIN", "MAX", "AVG"}) {
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
    // State on the right ("Running Â· iteration 2 of 5", "Stopped", "Done").
    dc.SetFont(GetFont().Bold());
    dc.SetTextForeground(running_ ? th.accent : (s_.stopped ? th.warn : th.dim));
    dc.DrawText(U(state_), cs.x - pad - sz.x, y + FromDIP(18));
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
      dc.DrawText(U(l), pad, y + FromDIP(48));
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
          if (tx > cs.x - pad - FromDIP(60)) break;
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

// Owner-drawn (wxVListBox): per-column colours, which a dark-mode wxListCtrl cannot do (it
// paints a whole row with one attribute), and only the visible rows are ever drawn. Columns
// are resized by dragging their edge in the header and sorted by clicking their title.
constexpr int kCols = 7;
const char* const kColNames[kCols] = {"#", "Iter", "Request", "Status", "Time", "Size", "Asserts"};
constexpr bool kColRight[kCols] = {true, true, false, false, true, true, false};
constexpr int kFlex = 2;

struct ColModel {
  int w[kCols] = {52, 44, 0, 170, 90, 86, 110};  // DIP; w[kFlex] unused
  int sort = -1;                                 // column, -1: run order
  bool desc = false;
  struct X {
    int x[kCols + 1];
  };
  X layout(const wxWindow* win, int width) const {
    int sum = 0;
    for (int i = 0; i < kCols; ++i)
      if (i != kFlex) sum += win->FromDIP(w[i]);
    const int flex = std::max(win->FromDIP(160), width - sum);
    X c;
    int x = 0;
    for (int i = 0; i < kCols; ++i) {
      c.x[i] = x;
      x += i == kFlex ? flex : win->FromDIP(w[i]);
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
            cols_->desc = i == 4 || i == 5;  // slowest / biggest first
          } else if (cols_->desc != (i == 4 || i == 5)) {
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

  // Failed checks listed under a row (Bruno lists every one under its request): the first is
  // on the row itself, the others on lines below, at most kMaxSub (then "+ n more"). With
  // all_checks (a small run, where every check is quick to scan) the passed ones are listed
  // too, in their order.
  static constexpr size_t kMaxSub = 6;
  size_t sub_lines(const StepResult& r) const {
    if (r.skipped || r.stopped() || r.errored()) return 0;
    size_t more = 0;
    if (all_checks_) more = r.asserts.size() - (r.asserts_failed > 0 ? 1 : 0);
    else if (r.asserts_failed >= 2) more = static_cast<size_t>(r.asserts_failed) - 1;
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
      const wxString s = dc.GetTextExtent(t).x > w ? wxControl::Ellipsize(t, dc, wxELLIPSIZE_END, std::max(w, 8)) : t;
      const wxSize ts = dc.GetTextExtent(s);
      const int x = rect.x + (kColRight[col] ? c.x[col + 1] - pad - ts.x : c.x[col] + pad);
      dc.DrawText(s, x, rect.y + (rect.height - ts.y) / 2);
    };
    const wxFont& base = GetFont();
    cell(0, wxString::Format("%zu", r.seq + 1), small_, th.dim);
    cell(1, wxString::Format("%d", r.iteration + 1), small_, th.dim);
    {
      // Method in its colour, the name, then the URL as sent (dim) in the room that is left.
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
      const wxString s = nw > w ? wxControl::Ellipsize(full, dc, wxELLIPSIZE_END, std::max(w, 8)) : full;
      dc.SetTextForeground(faded ? th.dim : th.fg);
      const wxSize ns = dc.GetTextExtent(s);
      dc.DrawText(s, rect.x + x0, rect.y + (rect.height - ns.y) / 2);
      const int ux = x0 + ns.x + FromDIP(14);
      const int uw = c.x[3] - ux - pad;
      const std::string why = !faded && r.failed() ? r.reason() : std::string();
      if (uw > FromDIP(60) && !why.empty()) {
        // Why it failed, readable without a click (Bruno lists it under the request).
        dc.SetFont(small_);
        dc.SetTextForeground(mix(th.bg, th.err, 0.9));
        const wxString us = wxControl::Ellipsize(U(why), dc, wxELLIPSIZE_END, uw);
        const wxSize uz = dc.GetTextExtent(us);
        dc.DrawText(us, rect.x + ux, rect.y + (rect.height - uz.y) / 2);
      } else if (uw > FromDIP(60) && !faded && !r.unresolved.empty()) {
        // Sent with a {{name}} nothing defines: it passed, but probably not as meant.
        std::string warn = "unresolved ";
        for (size_t i = 0; i < r.unresolved.size(); ++i) warn += (i ? ", {{" : "{{") + r.unresolved[i] + "}}";
        warn += " sent as written";
        dc.SetFont(small_);
        dc.SetTextForeground(th.warn);
        const wxString us = wxControl::Ellipsize(U(warn), dc, wxELLIPSIZE_END, uw);
        dc.DrawText(us, rect.x + ux, rect.y + (rect.height - dc.GetTextExtent(us).y) / 2);
      } else if (uw > FromDIP(60) && !r.url.empty()) {
        dc.SetFont(mono_);
        dc.SetTextForeground(mix(th.bg, th.dim, 0.8));
        const wxString us = wxControl::Ellipsize(U(r.url), dc, wxELLIPSIZE_MIDDLE, uw);
        const wxSize uz = dc.GetTextExtent(us);
        dc.DrawText(us, rect.x + ux, rect.y + (rect.height - uz.y) / 2);
      }
    }
    cell(3, U(status_label(r)), bold_, status_colour(r));
    cell(4, r.sent && !r.skipped ? U(fmt_ms(r.timing.total_ms)) : wxString("-"), base, th.fg);
    cell(5, r.sent && !r.skipped ? U(fmt_bytes(r.size)) : wxString("-"), small_, th.dim);
    if (r.skipped || r.stopped())
      cell(6, wxString::FromUTF8("\xE2\x80\x94"), bold_, th.dim);
    else if (r.asserts.empty())
      cell(6, r.failed() ? wxString::FromUTF8("\xE2\x9C\x97 failed") : wxString::FromUTF8("\xE2\x80\x94"), bold_,
           r.failed() ? th.err : th.dim);
    else
      cell(6,
           wxString::Format(wxString::FromUTF8(r.asserts_failed ? "\xE2\x9C\x97 %d / %zu" : "\xE2\x9C\x93 %d / %zu"),
                            r.asserts_passed, r.asserts.size()),
           bold_, r.asserts_failed ? th.err : th.ok);
    if (const size_t extra = sub_lines(r)) {
      // The other failed checks, one per line under the request name, with expected / actual.
      const int x0 = rect.x + c.x[2] + pad + FromDIP(50);
      const int w = std::max(FromDIP(80), rect.x + c.x[kCols] - pad - x0);
      dc.SetFont(small_);
      int y = rect.GetBottom() - FromDIP(3);
      size_t shown = 0, skipped_first = 0, listable = 0;
      for (const auto& a : r.asserts) {
        if (a.pass && !all_checks_) continue;
        if (!a.pass && !skipped_first++) continue;  // the first failed one is on the row itself
        ++listable;
        if (shown == kMaxSub) continue;
        ++shown;
        dc.SetTextForeground(a.pass ? mix(th.bg, th.ok, 0.85) : mix(th.bg, th.err, 0.9));
        dc.DrawText(wxString::FromUTF8(a.pass ? "\xE2\x9C\x93" : "\xE2\x9C\x97"), x0 - FromDIP(14), y);
        dc.SetTextForeground(a.pass ? mix(th.bg, th.fg, 0.75) : mix(th.bg, th.err, 0.9));
        dc.DrawText(wxControl::Ellipsize(U(a.pass ? a.label() : a.text()), dc, wxELLIPSIZE_END, w), x0, y);
        y += FromDIP(18);
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
  explicit StatsList(wxWindow* parent) : wxVListBox(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxWANTS_CHARS) {
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
      if (delta) right(7, wxString::FromUTF8("\xCE\x94 P50"), small_, th.dim);
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
      const wxString f = wxControl::Ellipsize(U(q.rel.substr(0, sl) + " /"), dc, wxELLIPSIZE_START, std::max(room / 3, 20));
      dc.DrawText(f, nx, rect.y + (rect.height - dc.GetCharHeight()) / 2);
      nx += dc.GetTextExtent(f).x + FromDIP(5);
      room -= dc.GetTextExtent(f).x + FromDIP(5);
      dc.SetFont(GetFont());
      dc.SetTextForeground(th.fg);
    }
    const std::string shown = q.name.empty() ? q.rel : q.name;
    const wxString nm = wxControl::Ellipsize(U(shown), dc, wxELLIPSIZE_END, std::max(room, 20));
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
        const wxString ws = wxControl::Ellipsize(U(why), dc, wxELLIPSIZE_END, tw);
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
        // Only a change that is both relative and absolute is coloured (noise stays dim).
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
// numbers normalised), with how many steps failed that way, of how many sends of that request,
// and the first and last iteration. A click (or Enter) filters Results to exactly those steps.
class FailList : public wxVListBox {
 public:
  explicit FailList(wxWindow* parent) : wxVListBox(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxWANTS_CHARS) {
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
    // Request (method colour), check (mono) on the first line; the message on the second.
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
    const wxString nm = wxControl::Ellipsize(U(g.name.empty() ? g.rel : g.name), dc, wxELLIPSIZE_END, std::max(FromDIP(40), (room_end - x) / 2));
    dc.DrawText(nm, x, y1);
    x += dc.GetTextExtent(nm).x + FromDIP(14);
    if (room_end - x > FromDIP(40)) {
      dc.SetFont(mono_);
      dc.SetTextForeground(th.dim);
      dc.DrawText(wxControl::Ellipsize(U(g.check), dc, wxELLIPSIZE_END, room_end - x), x, y1 + FromDIP(1));
    }
    std::string msg = g.message;
    if (g.variants > 1) {
      // The numbers differ between occurrences: the first and the latest message.
      msg += "   \xE2\x80\xA6   latest: " + g.last_message;
      if (g.variants > 2) msg += "   (" + std::to_string(g.variants) + (g.variants >= 1000 ? "+" : "") + " values)";
    }
    dc.SetFont(GetFont().Scaled(0.95f));
    dc.SetTextForeground(mix(th.bg, th.err, 0.92));
    dc.DrawText(wxControl::Ellipsize(U(msg), dc, wxELLIPSIZE_END, std::max(FromDIP(60), room_end - x_req - pad)), x_req + pad, y2);
  }
  std::vector<runner::FailureGroup> rows_;
  std::map<size_t, size_t> sends_;
  int iterations_ = 0;
  wxFont bold_, small_, mono_;
};

// ------------------------------------------------------------------ over time (load mode)

// Postman's performance view: per time bucket, the sends finished (bars, failed part red, as
// sends per second) and the p50 / p95 response time (lines), over the run's wall time.
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
      // The last bucket is partly past the end: average over the time it covers.
      const double span = std::min(bucket_ms_, end - bucket_ms_ * static_cast<double>(i));
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
    double busy_ms = 0, active = 0;  // send time in the bucket; active = busy_ms / bucket length
  };
  wxRect plot() const {
    const wxSize cs = GetClientSize();
    return wxRect(FromDIP(64), FromDIP(40), std::max(10, cs.x - FromDIP(64 + 64)), std::max(10, cs.y - FromDIP(40 + 30)));
  }
  int bucket_at(int x) const {
    const wxRect pr = plot();
    if (b_.empty() || x < pr.x || x >= pr.GetRight()) return -1;
    return std::min(static_cast<int>(b_.size()) - 1, (x - pr.x) * static_cast<int>(b_.size()) / std::max(1, pr.width));
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
    const double per_s = 1000.0 / bucket_ms_;
    double max_rps = 0, max_ms = 0;
    for (const auto& b : b_) {
      max_rps = std::max(max_rps, static_cast<double>(b.n) * per_s);
      max_ms = std::max(max_ms, b.p95);
    }
    max_rps = std::max(max_rps, 1.0) * 1.1;
    max_ms = std::max(max_ms, 0.01) * 1.1;
    // When p50 sits far below p95 (0.66 vs 130 ms) a linear axis puts it on the floor, under
    // the bars: the times then use a log axis, so both lines stay readable.
    double min_ms = max_ms;
    for (const auto& b : b_)
      if (b.n && b.p50 > 0) min_ms = std::min(min_ms, b.p50);
    const bool log_axis = max_ms / std::max(min_ms, 1e-6) > 20;
    const double lo_ms = log_axis ? std::max(min_ms / 1.5, 1e-3) : 0;
    auto y_of_ms = [&](double v) {
      if (!log_axis) return static_cast<int>(pr.height * v / max_ms);
      const double t = (std::log(std::max(v, lo_ms)) - std::log(lo_ms)) / (std::log(max_ms) - std::log(lo_ms));
      return static_cast<int>(pr.height * std::clamp(t, 0.0, 1.0));
    };
    // Legend / totals.
    char head[256];
    std::snprintf(head, sizeof head, "%zu sends in %s  \xC2\xB7  %.1f / s average  \xC2\xB7  %.2f%% failed  \xC2\xB7  bucket %s",
                  total_, fmt_ms(end_ms_).c_str(), static_cast<double>(total_) * 1000.0 / end_ms_,
                  100.0 * static_cast<double>(failed_) / static_cast<double>(total_), fmt_ms(bucket_ms_).c_str());
    dc.SetTextForeground(th.dim);
    dc.DrawText(U(head), FromDIP(16), FromDIP(10));
    int lx = pr.GetRight();
    auto legend = [&](const wxString& t, const wxColour& c, bool line) {
      const int tw = dc.GetTextExtent(t).x;
      lx -= tw;
      dc.SetTextForeground(th.dim);
      dc.DrawText(t, lx, FromDIP(10));
      lx -= FromDIP(18);
      dc.SetPen(line ? wxPen(c, FromDIP(2)) : *wxTRANSPARENT_PEN);
      dc.SetBrush(wxBrush(c));
      if (line) dc.DrawLine(lx, FromDIP(18), lx + FromDIP(12), FromDIP(18));
      else dc.DrawRectangle(lx, FromDIP(13), FromDIP(12), FromDIP(10));
      lx -= FromDIP(14);
    };
    const wxColour bar = mix(th.bg, th.info, 0.55), fail = th.err, c50 = th.ok, c95 = th.warn;
    const wxColour cact = mix(th.fg, wxColour(190, 140, 255), 0.7);
    // Active workers on their own scale: the line's top is the busiest bucket (labelled).
    const double act_top = std::max(1.0, std::ceil(max_active_)) * 1.1;
    {
      char at[64];
      std::snprintf(at, sizeof at, "active workers (max %.1f)", max_active_);
      legend(at, cact, true);
    }
    legend(log_axis ? "p95 ms (log axis)" : "p95 ms", c95, true);
    legend("p50 ms", c50, true);
    legend("failed / s", fail, false);
    legend("sends / s", bar, false);
    // Grid and axes.
    dc.SetPen(wxPen(mix(th.bg, th.grid, 0.6)));
    for (int g = 0; g <= 4; ++g) {
      const int y = pr.GetBottom() - pr.height * g / 4;
      dc.DrawLine(pr.x, y, pr.GetRight(), y);
      char l[32], r[32];
      std::snprintf(l, sizeof l, max_rps * g / 4 >= 10 ? "%.0f/s" : "%.1f/s", max_rps * g / 4);
      std::snprintf(r, sizeof r, "%s", fmt_ms(log_axis ? lo_ms * std::pow(max_ms / lo_ms, g / 4.0) : max_ms * g / 4).c_str());
      dc.SetTextForeground(th.dim);
      const wxSize ls = dc.GetTextExtent(l);
      dc.DrawText(l, pr.x - ls.x - FromDIP(6), y - ls.y / 2);
      dc.DrawText(r, pr.GetRight() + FromDIP(6), y - ls.y / 2);
    }
    const size_t nb = b_.size();
    auto xat = [&](size_t i) { return pr.x + static_cast<int>(static_cast<double>(pr.width) * static_cast<double>(i) / static_cast<double>(nb)); };
    for (int g = 0; g <= 4; ++g) {
      // Every tick in the same unit ("0 s", "4.5 s", "9 s"; ms for a run under a second).
      const double v = end_ms_ * g / 4;
      char tb[32];
      if (end_ms_ >= 1000)
        std::snprintf(tb, sizeof tb, end_ms_ >= 20000 || v == 0 ? "%.0f s" : "%.1f s", v / 1000);
      else
        std::snprintf(tb, sizeof tb, "%.0f ms", v);
      const wxString t = tb;
      const int x = pr.x + pr.width * g / 4 - (g == 4 ? dc.GetTextExtent(t).x : g == 0 ? 0 : dc.GetTextExtent(t).x / 2);
      dc.DrawText(t, x, pr.GetBottom() + FromDIP(6));
    }
    // Bars.
    dc.SetPen(*wxTRANSPARENT_PEN);
    for (size_t i = 0; i < nb; ++i) {
      const auto& b = b_[i];
      if (!b.n) continue;
      const int x0 = xat(i) + 1, x1 = std::max(x0 + 1, xat(i + 1) - 1);
      const int h = static_cast<int>(pr.height * (static_cast<double>(b.n) * per_s / max_rps));
      const int hf = static_cast<int>(pr.height * (static_cast<double>(b.failed) * per_s / max_rps));
      dc.SetBrush(wxBrush(static_cast<int>(i) == hover_ ? mix(bar, th.fg, 0.25) : bar));
      dc.DrawRectangle(x0, pr.GetBottom() - h, x1 - x0, h - hf);
      if (hf) {
        dc.SetBrush(wxBrush(fail));
        dc.DrawRectangle(x0, pr.GetBottom() - hf, x1 - x0, hf);
      }
    }
    // Lines, each on a background-coloured halo so it reads over the bars.
    for (int pass = 0; pass < 4; ++pass) {
      const int k = pass / 2;
      const bool halo = pass % 2 == 0;
      dc.SetPen(halo ? wxPen(th.bg, FromDIP(5)) : wxPen(k ? c95 : c50, FromDIP(2)));
      wxPoint prev(-1, -1);
      for (size_t i = 0; i < nb; ++i) {
        const auto& b = b_[i];
        if (!b.n) {
          prev = wxPoint(-1, -1);
          continue;
        }
        const double v = k ? b.p95 : b.p50;
        const wxPoint pt((xat(i) + xat(i + 1)) / 2, pr.GetBottom() - y_of_ms(v));
        if (prev.x >= 0) dc.DrawLine(prev, pt);
        else dc.DrawCircle(pt, FromDIP(1));
        prev = pt;
      }
    }
    // Active workers: a stepped line (dashed) over the bars.
    {
      wxPen pen(cact, FromDIP(2), wxPENSTYLE_SHORT_DASH);
      dc.SetPen(pen);
      int prev_y = -1;
      for (size_t i = 0; i < nb; ++i) {
        const int y = pr.GetBottom() - static_cast<int>(pr.height * (b_[i].active / act_top));
        const int x0 = xat(i), x1 = xat(i + 1);
        if (prev_y >= 0 && prev_y != y) dc.DrawLine(x0, prev_y, x0, y);
        dc.DrawLine(x0, y, x1, y);
        prev_y = y;
      }
    }
    // Hovered bucket.
    if (hover_ >= 0 && static_cast<size_t>(hover_) < nb) {
      const auto& b = b_[static_cast<size_t>(hover_)];
      char t[256];
      std::snprintf(t, sizeof t, "%s - %s:  %zu sends (%.1f / s), %zu failed, p50 %s, p95 %s, %.1f active workers",
                    fmt_ms(bucket_ms_ * hover_).c_str(), fmt_ms(bucket_ms_ * (hover_ + 1)).c_str(), b.n,
                    static_cast<double>(b.n) * per_s, b.failed, fmt_ms(b.p50).c_str(), fmt_ms(b.p95).c_str(), b.active);
      dc.SetTextForeground(th.fg);
      dc.DrawText(U(t), FromDIP(16), FromDIP(24));
    }
  }
  std::vector<B> b_;
  double bucket_ms_ = 1000, end_ms_ = 0, max_active_ = 0;
  size_t total_ = 0, failed_ = 0;
  int hover_ = -1;
};

// ------------------------------------------------------------------ request checklist

// The requests to run: a checkbox, the method in its sidebar colour, the path. Space toggles,
// Alt+Up / Alt+Down move, a click on the box toggles, a double click opens the request.
class RequestList : public wxVListBox {
 public:
  struct Row {
    std::string method, name, folder, tags;  // name: the request's name (as the results show it)
    bool grpc = false;
    bool checked = true;
  };
  explicit RequestList(wxWindow* parent) : wxVListBox(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxWANTS_CHARS) {
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
      const wxString f = wxControl::Ellipsize(U(r.folder + " /"), dc, wxELLIPSIZE_START, std::max(room / 3, 20));
      const wxSize fs = dc.GetTextExtent(f);
      dc.DrawText(f, x, rect.y + (rect.height - fs.y) / 2);
      x += fs.x + FromDIP(5);
      room -= fs.x + FromDIP(5);
      dc.SetFont(GetFont());
      dc.SetTextForeground(r.checked ? th.fg : th.dim);
    }
    const wxString t = wxControl::Ellipsize(U(r.name), dc, wxELLIPSIZE_END, std::max(room, 20));
    const wxSize ts = dc.GetTextExtent(t);
    dc.DrawText(t, x, rect.y + (rect.height - ts.y) / 2);
    if (tw) {
      dc.SetFont(small_);
      dc.SetTextForeground(th.dim);
      const wxString tg = wxControl::Ellipsize(U(r.tags), dc, wxELLIPSIZE_END, tw);
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
  kProfFixed = 12, kProfRamp, kProfPeak, kProfSpike,
  // results bar
  kFAll = 20, kFPassed, kFFailed, kFSkipped, kVResults, kVRequests, kVTime, kVFail, kScope,
  kRunAgain = 40, kRerunFailed, kHistory, kHtml, kJson, kJunit,
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
    stage_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
      if (!left_built_) {
        left_built_ = true;
        wxWindowUpdateLocker lock(left_);  // children made on a visible window: draw once, at the end
        build();
        left_->Layout();
        stage_.StartOnce(1);
        return;
      }
      {
        wxWindowUpdateLocker lock(right_);
        build_right();
        Layout();
      }
      if (pending_items_) {
        auto items = std::move(pending_items_);
        loaded(std::move(items), pending_errs_);
      }
    });
    stage_.StartOnce(1);
    tick_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); });
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) { if (!on_key(e)) e.Skip(); });
    load(ws);
  }
  ~RunnerFrame() override {
    link_->frame = nullptr;
    if (run_) run_->ctl.stop();
  }

  // "Runner: api" for the tab.
  wxString tab_title() const { return U("Runner: " + name_s_); }
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
      book->DeletePage(static_cast<size_t>(i));
      if (book->GetPageCount() > 0) book->SetSelection(std::min<size_t>(static_cast<size_t>(i), book->GetPageCount() - 1));
    });
  }

 private:
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
    left_ = new wxPanel(this);
    left_->SetBackgroundColour(th.bg);
    left_->SetMinSize(FromDIP(wxSize(340, -1)));
    root->Add(left_, 0, wxEXPAND);
    auto* sep = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(1, -1));
    sep->SetBackgroundColour(th.grid);
    root->Add(sep, 0, wxEXPAND);
    right_ = new wxPanel(this);
    right_->SetBackgroundColour(th.bg);
    root->Add(right_, 1, wxEXPAND);
    SetSizer(root);
  }

  // ---- left: what to run
  void build() {
    const auto& th = rv::Theme::get();
    const wxColour lbg = th.bg;  // the chip bars and lists paint the theme background
    auto* ls = new wxBoxSizer(wxVERTICAL);
    const int pad = FromDIP(16);
    auto label = [&](const wxString& s, bool dim = true, float scale = 0.9f, bool bold = true) {
      auto* t = new wxStaticText(left_, wxID_ANY, s);
      wxFont f = t->GetFont().Scaled(scale);
      if (bold) f = f.Bold();
      t->SetFont(f);
      t->SetForegroundColour(dim ? th.dim : th.fg);
      return t;
    };
    ls->Add(label("COLLECTION RUNNER", true, 0.85f), 0, wxLEFT | wxRIGHT | wxTOP, pad);
    ls->Add(label(U(name_s_), false, 1.35f), 0, wxLEFT | wxRIGHT | wxTOP, pad);
    ls->AddSpacer(FromDIP(4));
    // The environment of the run (Postman's runner dropdown, Bruno's selector on the runner tab):
    // the run's own choice; the workspace's active environment is not changed.
    env_chips_ = new rv::ChipBar(left_, [this](int id) { on_chip(id); });
    env_chips_->SetBackgroundColour(lbg);
    env_chips_->add(kEnvPick, "", "Environment for this run (the workspace's active one is not changed)", -1, false);
    ls->Add(env_chips_, 0, wxEXPAND | wxLEFT | wxRIGHT, pad - FromDIP(4));
    update_env_label();
    ls->AddSpacer(FromDIP(12));

    auto* reqrow = new wxBoxSizer(wxHORIZONTAL);
    req_title_ = label("REQUESTS", true, 0.85f);
    reqrow->Add(req_title_, 0, wxALIGN_CENTER_VERTICAL);
    left_chips_ = new rv::ChipBar(left_, [this](int id) { on_chip(id); });
    left_chips_->SetBackgroundColour(lbg);
    left_chips_->add(kAll, "All", "Check every request shown", -1, true);
    left_chips_->add(kNone, "None", "Uncheck every request shown", -1, true);
    left_chips_->add(kUp, "\xE2\x96\xB2", "Move the selected request up (Alt+Up)", -1, true);
    left_chips_->add(kDown, "\xE2\x96\xBC", "Move the selected request down (Alt+Down)", -1, true);
    reqrow->Add(left_chips_, 1, wxEXPAND | wxLEFT, FromDIP(6));
    ls->Add(reqrow, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);
    ls->AddSpacer(FromDIP(4));
    filter_ = new wxTextCtrl(left_, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxBORDER_THEME);
    filter_->SetHint("Filter by tag or name: smoke -slow");
    filter_->SetToolTip("Tags are #words in a request's Docs and its folder names. A word must match; -word excludes.");
    filter_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { fill_list(); });
    ls->Add(filter_, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);
    ls->AddSpacer(FromDIP(6));
    list_ = new RequestList(left_);
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

    ls->AddSpacer(FromDIP(14));
    auto* grid = new wxFlexGridSizer(2, FromDIP(8), FromDIP(12));
    grid->AddGrowableCol(1);
    auto field = [&](const wxString& s, const wxString& tip) {
      auto* t = new wxStaticText(left_, wxID_ANY, s);
      t->SetForegroundColour(th.fg);
      t->SetToolTip(tip);
      return t;
    };
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
    prof_chips_->add(kProfRamp, "Ramp up", "The workers join one by one over Ramp (s)", 0);
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
    data_label_ = label("", true, 0.85f, false);
    ls->Add(data_label_, 0, wxLEFT | wxRIGHT, pad);
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
      ls->Add(c, 0, wxLEFT | wxRIGHT | wxBOTTOM, pad);
    }
    ls->AddSpacer(FromDIP(8));
    run_btn_ = new AccentButton(left_, "Run");
    run_btn_->on_click = [this] { on_run(false); };
    run_btn_->Disable();
    run_btn_->SetToolTip("Run (Ctrl+Enter); Esc stops; Ctrl+Shift+Enter reruns the failed requests");
    ls->Add(run_btn_, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);
    ls->AddSpacer(FromDIP(6));
    plan_label_ = label("", true, 0.9f, false);
    ls->Add(plan_label_, 0, wxLEFT | wxRIGHT | wxBOTTOM, pad);
    left_->SetSizer(ls);
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
    bar_->add(kScope, "", "Results are narrowed to this request or failure: click to show every result again", -1);
    bar_->set_visible(kScope, false);
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
    bar_->add(kRerunFailed, "Rerun failed", "Run only the requests that failed, once (Ctrl+Shift+Enter)", -1, true);
    bar_->add(kHistory, "History \xE2\x96\xBE", "Earlier runs of this folder in this session", -1, true);
    bar_->add(kHtml, "HTML report", "Save a self-contained HTML report", -1, true);
    bar_->add(kJson, "JSON", "Export every result as JSON", -1, true);
    bar_->add(kJunit, "JUnit", "Export JUnit XML for CI", -1, true);
    for (int id : {kRunAgain, kRerunFailed, kHistory, kHtml, kJson, kJunit}) bar_->set_enabled(id, false);
    bar->Add(bar_, 1, wxEXPAND);
    rs->Add(bar, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));
    status_ = new wxStaticText(right, wxID_ANY, "");
    status_->SetForegroundColour(th.dim);
    rs->Add(status_, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    status_->Hide();

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
    detail_ = new runner_ui::DetailPane(split_);
    detail_->on_open = [this] { open_step(shown_); };
    detail_->on_key = [this](wxKeyEvent& e) { return on_key(e); };
    run_btn_->on_key = [this](wxKeyEvent& e) { return on_key(e); };
    split_->SplitHorizontally(tpanel_, detail_, 0);
    split_->SetSashGravity(0.6);
    split_->SetMinimumPaneSize(FromDIP(120));
    rs->Add(split_, 1, wxEXPAND | wxLEFT, FromDIP(1));
    right->SetSizer(rs);
    table_->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) {
      follow_ = false;
      show_selected();
    });
    CallAfter([this] {
      const int h = split_->GetClientSize().y;
      if (h > 0) split_->SetSashPosition(h * 58 / 100);  // ~22 result rows at 1600 x 1000
    });
    update_filter_chips();
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
    left_->Layout();
  }

  // ---- loading the folder (worker)
  // The order the user dragged items into in the sidebar (stored by the sidebar, per workspace,
  // under Sidebar/Order/<hash of the root>): the runner runs what the tree lists, top to bottom.
  static runner::FolderOrder sidebar_order(const fs::path& root) {
    runner::FolderOrder out;
    wxConfigBase* c = wxConfigBase::Get();
    wxString s;
    const wxString key = wxString::Format("%016llx", static_cast<unsigned long long>(std::hash<std::wstring>{}(root.wstring())));
    if (!c || !c->Read("Sidebar/Order/" + key, &s)) return out;
    wxStringTokenizer lines(s, "\n");
    while (lines.HasMoreTokens()) {
      wxStringTokenizer f(lines.GetNextToken(), "\t");
      if (!f.HasMoreTokens()) continue;
      const wxString rel = f.GetNextToken();
      const fs::path dir = rel == "." ? root : (root / fs::path(rel.ToStdWstring())).lexically_normal();
      std::vector<std::wstring> names;
      while (f.HasMoreTokens()) names.push_back(f.GetNextToken().ToStdWstring());
      out[dir] = std::move(names);
    }
    return out;
  }

  void load(const Workspace& ws) {
    auto wsc = std::make_shared<Workspace>(ws);
    auto link = link_;
    const fs::path folder = folder_;
    auto order = std::make_shared<runner::FolderOrder>(sidebar_order(ws.root()));
    std::thread([wsc, link, folder, order] {
      auto errs = std::make_shared<std::vector<std::string>>();
      auto items = std::make_shared<std::vector<RunItem>>(runner::collect(*wsc, folder, errs.get(), order.get()));
      wxTheApp->CallAfter([link, items, errs] {
        if (link->frame) link->frame->loaded(items, *errs);
      });
    }).detach();
  }

 public:
  void loaded(std::shared_ptr<std::vector<RunItem>> items, const std::vector<std::string>& errs) {
    if (!detail_) {  // the results side is not built yet: it calls this when it is
      pending_items_ = std::move(items);
      pending_errs_ = errs;
      return;
    }
    items_ = std::move(items);
    order_.clear();
    tags_.clear();
    for (size_t i = 0; i < items_->size(); ++i) {
      order_.push_back(i);
      std::string t;
      for (const auto& g : runner::tags_of((*items_)[i])) t += (t.empty() ? "#" : " #") + g;
      tags_.push_back(std::move(t));
    }
    checked_.assign(items_->size(), 1);
    fill_list();
    if (!errs.empty()) set_status_w(U(std::to_string(errs.size()) + " file(s) could not be read: " + errs.front()));
    if (items_->empty()) {
      detail_->show(nullptr, errs.empty() ? "This folder has no requests." : errs.front());
    } else {
      detail_->show(nullptr, "Press Run (Ctrl+Enter) to send " + std::to_string(items_->size()) +
                                 " request(s) in order. Select a result to see its response, headers, request, timing and tests.");
    }
    // The last run of this folder in this session, if the window was closed since.
    auto& h = run_history()[key_];
    if (!h.empty() && steps_.empty()) show_record(h.front(), false);
    update_history_chips();
    run_btn_->SetFocus();
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
        show_selected();
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
      summary_->set(summary_now(false), "Cancelled", false);
    };
    // Scripts are code from the workspace (possibly a cloned repo): asked once, as the request
    // tab asks. "Run without scripts" sends those requests without them; they do not pass.
    int scripts = consent.scripts_trusted && consent.script_requests ? 1 : 0;  // 1 run, 2 trust first, 0 no
    if (consent.scripts_need_trust()) {
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
    start_run(st, std::move(wsc), std::move(order), std::move(consent), scripts);
  }

  void run_done(const std::shared_ptr<RunState>& st, RunSummary s, std::shared_ptr<Workspace> wsc) {
    if (st != run_) return;
    drain(st);
    run_.reset();
    tick_.Stop();
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
      // Parallel workers finish out of order: once the run is done the results read in plan
      // order (iteration, then the request's place), as a sequential run lists them; the
      // finishing order stays one click away (the # column).
      std::stable_sort(steps_.begin(), steps_.end(), [](const StepPtr& a, const StepPtr& b) {
        if (a->iteration != b->iteration) return a->iteration < b->iteration;
        if (a->pos != b->pos) return a->pos < b->pos;
        return a->seq < b->seq;
      });
      view_.clear();
      for (const auto& x : steps_)
        if (matches(*x)) view_.push_back(x);
      if (cols_.sort >= 0) apply_sort(false);
      table_->refresh_rows();
      // A row the user picked stays picked; the automatic choice (the first failure, or the
      // latest row while following) becomes the first failure in plan order, else the top row.
      const bool auto_pick = follow_ || !shown_ || shown_ == pinned_step_;
      int sel = view_.empty() ? wxNOT_FOUND : 0;
      for (size_t i = 0; i < view_.size(); ++i)
        if (auto_pick ? view_[i]->failed() : view_[i] == shown_) {
          sel = static_cast<int>(i);
          break;
        }
      follow_ = false;
      table_->SetSelection(sel);
      if (sel != wxNOT_FOUND) {
        if (sel == 0) table_->ScrollToRow(0);
        else table_->ensure_visible(static_cast<size_t>(sel));
        if (view_[static_cast<size_t>(sel)] != shown_) show_selected();
      }
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
    auto& h = run_history()[key_];
    h.push_front(rec);
    while (h.size() > kHistoryRuns) h.pop_back();
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
    if (!err.empty()) {
      data_label_->SetLabel(U(name + ": " + err));
      data_label_->SetForegroundColour(rv::Theme::get().err);
      data_.reset();
    } else {
      data_ = std::move(rows);
      data_name_ = name;
      std::string cols;
      if (!data_->empty())
        for (const auto& [k, v] : data_->front()) cols += (cols.empty() ? "" : ", ") + k;
      if (cols.size() > 40) cols = cols.substr(0, 37) + "...";
      data_label_->SetLabel(U(name + kDot + std::to_string(data_->size()) + " row" + (data_->size() == 1 ? "" : "s") +
                              kDot + "{{" + cols + "}}"));
      data_label_->SetForegroundColour(rv::Theme::get().dim);
      iters_->set_value(static_cast<int>(std::min<size_t>(data_->size(), 1000000)));
      data_chips_->set_visible(kDataClear, true);
      data_chips_->set_visible(kDataPreview, true);
    }
    update_counts();
  }

  void set_status(const std::string& s) { set_status_w(U(s)); }

 private:
  void set_status_w(const wxString& s) {
    status_->SetLabel(s);
    status_->Show(!s.empty());
    status_->GetParent()->Layout();
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
    bar_->set_visible(kScope, on);
    bar_->set_label(kScope, on ? "\xC3\x97  " + label : std::string());
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
      set_status_w(U(std::to_string(view_.size()) + " result" + (view_.size() == 1 ? "" : "s") + " of " + label +
                     kDot + "click the \xC3\x97 chip (or Esc) to show every result"));
    else
      set_status_w("");
  }
  void clear_scope() {
    if (scope_item_ != kNoScope || !scope_key_.empty()) set_scope(kNoScope, {}, "", -1);
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
    for (int id : {kHtml, kJson, kJunit}) bar_->set_enabled(id, idle && any);
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
  void show_selected(bool may_follow = true) {
    const int i = table_->GetSelection();
    if (i == wxNOT_FOUND || static_cast<size_t>(i) >= view_.size()) return;
    if (may_follow && cols_.sort < 0 && static_cast<size_t>(i) + 1 == view_.size()) follow_ = true;
    if (shown_ != view_[static_cast<size_t>(i)]) {
      shown_ = view_[static_cast<size_t>(i)];
      detail_->show(shown_, "");
      detail_->set_can_open(!file_of(*shown_).empty());
    }
  }

  fs::path file_of(const StepResult& s) const {
    if (run_items_ && s.item < run_items_->size()) return (*run_items_)[s.item].req.file;
    return {};
  }

  void open_step(const StepPtr& s) {
    if (!s) return;
    open_file(file_of(*s));
  }

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

  void move_sel(size_t n, int d) {
    if (n >= visible_.size()) return;
    const long t = static_cast<long>(n) + d;
    if (t < 0 || t >= static_cast<long>(visible_.size())) return;
    const size_t a = visible_[n], b = visible_[static_cast<size_t>(t)];
    std::swap(order_[a], order_[b]);
    std::swap(checked_[a], checked_[b]);
    fill_list();
    list_->SetSelection(static_cast<int>(t));
  }

  // Drag and drop in the list: the request at visible row `from` moves before visible row
  // `before` (visible_.size(): after the last one shown); hidden (filtered) rows keep their place.
  void drop_row(size_t from, size_t before) {
    if (from >= visible_.size() || before > visible_.size() || before == from || before == from + 1) return;
    const size_t a = visible_[from];
    const size_t at = before == visible_.size() ? visible_.back() + 1 : visible_[before];
    const size_t item = order_[a];
    const char chk = checked_[a];
    order_.erase(order_.begin() + static_cast<long>(a));
    checked_.erase(checked_.begin() + static_cast<long>(a));
    const size_t ins = at > a ? at - 1 : at;
    order_.insert(order_.begin() + static_cast<long>(ins), item);
    checked_.insert(checked_.begin() + static_cast<long>(ins), chk);
    fill_list();
    for (size_t n = 0; n < visible_.size(); ++n)
      if (visible_[n] == ins) list_->SetSelection(static_cast<int>(n));
  }

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
    wxString plan = n ? wxString::Format("%zu request%s x %d iteration%s = %zu sends", n, n == 1 ? "" : "s", it,
                                         it == 1 ? "" : "s", n * static_cast<size_t>(it))
                      : wxString("Select at least one request.");
    if (const int d = dur_->value(); n && d > 0) {
      plan = wxString::Format("%zu request%s, repeated for %d s", n, n == 1 ? "" : "s", d);
      if (it > 1) plan += wxString::Format(" (at most %d iterations)", it);
      if (par > 1) plan += wxString::Format(", %d at a time", par);
    } else if (n && par > 1) {
      plan += wxString::Format(", %d at a time", std::min(par, it));
    }
    if (n && par > 1) {
      const int d = dur_->value(), r = ramp_->value();
      if (profile_ == 1 && r > 0) plan += wxString::Format(", ramp-up %d s", r);
      else if (profile_ == 2 && r > 0) plan += d > 0 ? wxString::Format(", peak: ramp %d s up and down", r)
                                                     : wxString::Format(", ramp-up %d s (peak needs a Duration)", r);
      else if (profile_ == 3) plan += d > 0 ? wxString::Format(", spike %d to %d", spike_base(par), par)
                                            : wxString(" (spike needs a Duration)");
    }
    plan_label_->SetLabel(plan);
    if (!run_) run_btn_->Enable(n > 0);
    if (!run_ && bar_) bar_->set_enabled(kRunAgain, n > 0 && !steps_.empty());
    left_->Layout();
  }

  void set_running(bool on) {
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
      if (follow_) show_selected();
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

  void on_run(bool failed_only) {
    if (run_) {
      stop_run();
      return;
    }
    if (!items_) return;
    Workspace* ws = workspace_ ? workspace_() : nullptr;
    if (!ws || runner_key(ws->root(), {}) != runner_key(root_, {})) {
      set_status_w("The workspace was closed or switched: open the runner again from the sidebar.");
      return;
    }
    std::vector<size_t> pick;
    int iterations = std::max(1, iters_->value());
    std::shared_ptr<const std::vector<RunItem>> items = items_;
    run_label_.clear();
    if (failed_only) {
      // Every failed step of the run shown, once: per failed iteration, with that iteration's
      // data row and number, only the requests that failed in it (Postman's "Run failed").
      if (!shown_is_latest() || !run_items_ || !run_order_) return;
      std::map<int, std::vector<size_t>> per;
      for (const auto& s : steps_)
        if (s->failed() && s->pos < run_order_->size()) per[s->iteration].push_back(s->pos);
      if (per.empty()) return;
      retry_.clear();
      for (auto& [it, pos] : per) {
        std::sort(pos.begin(), pos.end());
        pos.erase(std::unique(pos.begin(), pos.end()), pos.end());
        retry_.push_back({it, std::move(pos)});
      }
      pick = *run_order_;
      items = run_items_;
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
    scope_item_ = kNoScope;
    scope_key_.clear();
    bar_->set_visible(kScope, false);
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
    started_ = Clock::now();
    follow_ = true;
    set_status_w("");
    set_running(true);
    if (failed_only)
      plan_label_->SetLabel(wxString::Format("Rerun failed: %zu send%s in %zu iteration%s, each with its own data row",
                                             planned_, planned_ == 1 ? "" : "s", retry_.size(),
                                             retry_.size() == 1 ? "" : "s"));
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
  void rebuild_view() {
    view_.clear();
    for (const auto& s : steps_)
      if (matches(*s)) view_.push_back(s);
    apply_sort(false);
    table_->refresh_rows();
    follow_ = false;
    if (!view_.empty()) {
      table_->SetSelection(0);
      table_->ScrollToRow(0);
      show_selected();
    } else {
      table_->SetSelection(wxNOT_FOUND);
      shown_.reset();
      detail_->show(nullptr, steps_.empty() ? "" : "No results match this filter.");
    }
  }

  void apply_sort(bool user) {
    auto key = [this](const StepResult& r) -> double {
      switch (cols_.sort) {
        case 1: return r.iteration;
        case 3: return r.skipped ? 1e9 : r.stopped() ? 1e8 : !r.sent ? 1e7 : r.status;
        case 4: return r.sent && !r.skipped ? r.timing.total_ms : -1;
        case 5: return static_cast<double>(r.size);
        case 6: return r.failed() ? 1000.0 + r.asserts_failed : r.asserts.empty() ? 0 : 1;
        default: return static_cast<double>(r.seq);
      }
    };
    const StepPtr keep = user ? shown_ : nullptr;
    if (cols_.sort == 2) {
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
    RunSummary s = live ? *live : have_final_ ? final_ : runner::summarize(steps_, planned_, 0, false, true);
    if (s.per_request.empty() && !steps_.empty()) s = runner::summarize(steps_, planned_, 0, false, true);
    // Compared with the run before the one shown (same folder, this session).
    std::map<std::string, double> base;
    const auto& h = run_history()[key_];
    std::shared_ptr<const RunRecord> prev;
    bool after = !shown_record_;  // a live run: the newest record is the previous one
    for (const auto& r : h) {
      if (after) {
        prev = r;
        break;
      }
      if (r == shown_record_) after = true;
    }
    if (prev)
      for (const auto& q : prev->summary.per_request) base[q.rel] = q.p50_ms;
    // Why each request failed: its most frequent cause (groups are sorted most frequent first).
    std::map<size_t, StatsList::Top> top;
    if (s.failed || n_failed_)
      for (const auto& g : runner::group_failures(steps_)) {
        auto [it, fresh] = top.try_emplace(g.item);
        if (fresh) {
          it->second.message = g.message;
          it->second.count = g.count;
        }
        ++it->second.causes;
      }
    stats_->set(s.per_request, std::move(base), prev ? prev->started_iso : std::string(), std::move(top));
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

  void show_record(const std::shared_ptr<const RunRecord>& rec, bool announce) {
    if (run_ || !rec) return;
    shown_record_ = rec;
    steps_ = *rec->steps;
    run_items_ = rec->items;
    planned_ = rec->planned;
    iterations_ = rec->iterations;
    table_->set_all_checks(planned_ > 0 && planned_ <= kAllChecksRun);
    final_ = rec->summary;
    have_final_ = true;
    started_iso_ = rec->started_iso;
    scope_item_ = kNoScope;
    scope_key_.clear();
    bar_->set_visible(kScope, false);
    n_passed_ = n_failed_ = n_skipped_ = 0;
    for (const auto& s : steps_) count(*s);
    refresh_fails();
    eval_thresholds(final_);
    shown_.reset();
    rebuild_view();
    summary_->set(final_, state_text(final_), false);
    update_filter_chips();
    if (view_mode_ == 1) refresh_stats();
    if (view_mode_ == 2) chart_->set(&steps_, final_.wall_ms);
    if (announce) {
      const auto& h = run_history()[key_];
      size_t idx = 0;
      while (idx < h.size() && h[idx] != rec) ++idx;
      set_status_w(U("Showing the run of " + iso_local(rec->started_iso) + (idx == 0 ? " (latest)" : " (" + std::to_string(idx) + " before the latest)") +
                     (rec->label.empty() ? "" : kDot + rec->label)));
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
    m.Append(999, "Compare: By request shows the change against the run before");
    m.Enable(999, false);
    const int id = GetPopupMenuSelectionFromUser(m);
    if (id >= 1000 && static_cast<size_t>(id - 1000) < h.size()) show_record(h[static_cast<size_t>(id - 1000)], true);
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
    const uint64_t gen = ++data_gen_;
    data_label_->SetLabel("Reading " + wxString(p.filename().wstring()) + "...");
    data_label_->SetForegroundColour(rv::Theme::get().dim);
    left_->Layout();
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

  // Postman's data file Preview: the rows as a table (the first 500), iteration by iteration.
  void preview_data() {
    if (!data_ || data_->empty()) return;
    const auto& rows = *data_;
    std::vector<std::string> cols;
    for (const auto& [k, v] : rows.front()) cols.push_back(k);
    std::vector<size_t> w(cols.size() + 1, 4);
    const size_t n = std::min<size_t>(rows.size(), 500);
    auto cell = [](std::string v) {
      for (auto& c : v)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
      if (u8_width(v) > 60) v = u8_head(v, 57) + "...";
      return v;
    };
    w[0] = std::max<size_t>(4, std::to_string(n).size());
    for (size_t c = 0; c < cols.size(); ++c) w[c + 1] = std::max(w[c + 1], u8_width(cols[c]));
    for (size_t i = 0; i < n; ++i)
      for (size_t c = 0; c < cols.size() && c < rows[i].size(); ++c) w[c + 1] = std::max(w[c + 1], u8_width(cell(rows[i][c].second)));
    auto padded = [](std::string v, size_t width) {
      if (const size_t vw = u8_width(v); vw < width) v.append(width - vw, ' ');
      return v;
    };
    std::string t = padded("iter", w[0]);
    for (size_t c = 0; c < cols.size(); ++c) t += "   " + padded(cols[c], w[c + 1]);
    t += "\n";
    for (size_t i = 0; i < n; ++i) {
      t += padded(std::to_string(i + 1), w[0]);
      for (size_t c = 0; c < cols.size(); ++c) t += "   " + padded(c < rows[i].size() ? cell(rows[i][c].second) : "", w[c + 1]);
      t += "\n";
    }
    if (rows.size() > n) t += "... " + std::to_string(rows.size() - n) + " more rows\n";
    const auto& th = rv::Theme::get();
    wxDialog dlg(this, wxID_ANY, U(data_name_ + kDot + std::to_string(rows.size()) + " rows"), wxDefaultPosition,
                 FromDIP(wxSize(760, 460)), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    dlg.SetBackgroundColour(th.bg);
    auto* sz = new wxBoxSizer(wxVERTICAL);
    auto* tc = new wxTextCtrl(&dlg, wxID_ANY, U(t), wxDefaultPosition, wxDefaultSize,
                              wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP | wxBORDER_NONE);
    tc->SetFont(wxFont(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE).FaceName("Consolas")));
    tc->SetBackgroundColour(th.bg);
    tc->SetForegroundColour(th.fg);
    auto* hint = new wxStaticText(&dlg, wxID_ANY, "Iteration i uses row i (the last row again when there are more iterations than rows).");
    hint->SetForegroundColour(th.dim);
    sz->Add(tc, 1, wxEXPAND | wxALL, FromDIP(10));
    sz->Add(hint, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    dlg.SetSizer(sz);
    dlg.Bind(wxEVT_CHAR_HOOK, [&dlg](wxKeyEvent& e) {
      if (e.GetKeyCode() == WXK_ESCAPE) dlg.EndModal(wxID_CANCEL);
      else e.Skip();
    });
    dlg.ShowModal();
  }

  void on_chip(int id) {
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
        data_.reset();
        data_name_.clear();
        data_label_->SetLabel("");
        data_chips_->set_visible(kDataClear, false);
        data_chips_->set_visible(kDataPreview, false);
        left_->Layout();
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
      case kRunAgain: return on_run(false);
      case kRerunFailed: return on_run(true);
      case kHistory: return history_menu();
      case kHtml: return export_as(2);
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

  wxPanel* left_ = nullptr;
  wxPanel* right_ = nullptr;
  wxTimer stage_;
  bool left_built_ = false;
  std::shared_ptr<std::vector<RunItem>> pending_items_;
  std::vector<std::string> pending_errs_;
  rv::ChipBar* env_chips_ = nullptr;
  wxStaticText* req_title_ = nullptr;
  wxStaticText* plan_label_ = nullptr;
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
                 std::function<std::shared_ptr<GrpcClient>()> grpc) {
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
        nb->SetSelection(static_cast<size_t>(i));
        return;
      }
    }
    open.erase(it);
  }
  auto* f = new RunnerFrame(nb, std::move(workspace), *ws, folder, std::move(grpc));
  open[key] = f;
  nb->AddPage(f, f->tab_title(), true);
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
