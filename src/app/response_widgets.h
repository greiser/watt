#pragma once
// Custom-drawn pieces of the response view: the status strip, the timing waterfall and the tab
// strip. All painting is wxDC on the UI thread over a handful of rectangles: nothing here scales
// with the response size.
#include <wx/listctrl.h>
#include <wx/timer.h>
#include <wx/window.h>

#include <functional>
#include <string>
#include <vector>

#include "core/timing.h"

namespace watt::app::rv {

// Colors shared by the response view (light / dark follow the system appearance).
struct Theme {
  bool dark = false;
  wxColour bg, fg, dim, faint, grid, accent;
  wxColour ok, redirect, warn, err, info;
  static const Theme& get();
};

enum class Tone { Plain, Dim, Ok, Redirect, Warn, Err, Info, Accent };
wxColour tone_colour(Tone t);

// ------------------------------------------------------------------ status strip

struct StatusSeg {
  std::string text;
  std::string tip;  // tooltip
  Tone tone = Tone::Plain;
  bool bold = false;
  // When the strip is too narrow, segments are dropped highest `drop` first (0: never dropped,
  // elided with "..." as a last resort). Dropped segments are listed in the strip's tooltip.
  int drop = 0;
  int click = 0;  // != 0: clickable (hand cursor); StatusStrip's on_click gets this id
  int key = 0;    // != 0: names the segment for StatusStrip::put
};

class StatusStrip : public wxWindow {
 public:
  explicit StatusStrip(wxWindow* parent);
  // badge: "200 OK" drawn as a colored pill (empty = none); segments follow, separated by dots.
  void set(const std::string& badge, Tone badge_tone, std::vector<StatusSeg> segs);
  // Running clock: draws "label  12.3 ms" with the time since `since`, repainted ~30x/s until set().
  void set_busy(const std::string& label, Clock::time_point since);
  void set_busy_label(const std::string& label) { busy_label_ = label; Refresh(); }
  // While the clock runs: the time shown is `ms`, set by the caller on its own tick (so it reads
  // the same as the waterfall's live total next to it) instead of the strip's own clock.
  void set_busy_ms(double ms) {
    busy_ms_ = ms;
    timer_.Stop();
    Refresh(false);
  }
  // While the clock runs: the response's status pill ("200 OK") once its head arrived, drawn
  // before the label (empty: none). Cleared by set() / set_busy().
  void set_busy_badge(const std::string& badge, Tone tone) {
    busy_badge_ = badge;
    busy_badge_tone_ = tone;
    Refresh(false);
  }
  // Replaces the segment with seg.key (or appends it): a count that is known only later. Ignored
  // while the busy clock runs (the segments are about to be replaced).
  void put(int key, StatusSeg seg);
  // A clickable segment was clicked: its id and its rectangle in screen coordinates.
  void set_on_click(std::function<void(int, const wxRect&)> f) { on_click_ = std::move(f); }
  wxSize DoGetBestClientSize() const override;

 private:
  void on_paint();
  void on_motion(const wxPoint& p);
  std::function<void(int, const wxRect&)> on_click_;
  std::string badge_;
  Tone badge_tone_ = Tone::Plain;
  std::vector<StatusSeg> segs_;
  std::vector<wxRect> hit_;  // per segment, for tooltips (empty: dropped for lack of room)
  std::string dropped_tip_;  // what did not fit, shown when hovering the strip outside a segment
  int hover_ = -2;
  wxRect mute_;  // a clicked spot whose popover is open: no tooltip there until the mouse leaves it
  bool busy_ = false;
  std::string busy_label_;
  std::string busy_badge_;
  Tone busy_badge_tone_ = Tone::Plain;
  Clock::time_point since_{};
  double busy_ms_ = -1;  // set_busy_ms(): the time shown (-1: the strip's own clock)
  wxTimer timer_;
};

// ------------------------------------------------------------------ waterfall

struct Phase {
  std::string name;
  double ms = 0;
  std::string tip;  // what exactly the phase measures
  wxColour colour;
  bool reused = false;  // 0 because the connection was reused (drawn as "reused", not a bar)
  bool has_value = true;  // false: "n/a" (e.g. TLS on plaintext, a phase a failed request never reached)
  bool failed = false;    // the phase the request failed in (drawn in the error color)
  // Parts of this phase (the hops of "Redirects"): shown as rows under it when the user expands
  // it (click the row), each bar where it ran inside the phase.
  std::vector<Phase> sub;
};

struct TimingModel {
  bool valid = false;
  std::string kind;  // "http" / "grpc": deltas only compare like with like
  std::vector<Phase> phases;
  double total = 0;
  // Phases drawn after the total (the post-response script: it runs once the response is
  // complete, so it is not part of the response time). The last phases add up to it.
  double after = 0;
  // Phases drawn before the total (the pre-request script: it runs before the request is built,
  // so it is not part of the response time either). The first phases add up to it.
  double before = 0;
  double server_wait = 0;  // the TTFB / wait phase
  bool server_na = false;  // the request failed before any response was awaited: "n/a", not 0 ms
  // total = server_wait + network + client: network is DNS, connect, TLS, upload, download (and
  // the non-server part of redirects), client is watt's own time (queue, prepare, process).
  double network = 0;
  double client = 0;
  std::string network_tip;
  std::string client_tip;
  bool reused = false;
  std::string conn_note;  // e.g. "new connection" / "reused connection"
  std::string total_tip;
  std::string server_label = "server wait";  // footer label for server_wait
  std::string server_tip =
      "Request fully sent -> first response byte: server processing plus one network round trip.";
  // A user-paced (Manual) stream: the time the stream sat open at the user's pace (idle before
  // the first message plus the user's sends until End). Its own footer part, so total =
  // server_wait + network + client + paced; < 0: not user-paced.
  double paced = -1;
  std::string paced_tip;
  // The OAuth 2.0 token step (fetch, refresh or cache lookup) that ran before the request: its
  // own footer / header part, never counted as client (watt) time; total = server_wait +
  // network + client + auth. < 0: no token step.
  double auth = -1;
  std::string auth_tip;
  // A response whose head is in and whose body still arrives (or was cut by a cancel): the phase
  // at live_row (Download) runs, live_head says what is arriving; live_cut: canceled.
  int live_row = -1;
  bool live_cut = false;
  std::string live_head;
};

class Waterfall : public wxWindow {
 public:
  explicit Waterfall(wxWindow* parent);
  ~Waterfall() override;
  // Re-reads the global collapsed choice and re-lays out the parent.
  void relayout();
  // The response pane's height, from its size handler before it lays out: a short pane (under
  // ~760 px: a 768 px screen, request over response) shows the one-line bar.
  void pane_height(int h);
  // prev: the previous run of this tab (same kind) for the delta column; nullptr = none.
  void set(const TimingModel& cur, const TimingModel* prev);
  // keep_reserve: keep the reserved height (a request is about to run); false: the empty state.
  void clear(bool keep_reserve = true);
  // While a new request is in flight the old figures stay, dimmed and labeled "previous run".
  void set_stale(bool stale);
  // Keep the height of `rows` phase rows from now on, also while no timing is shown, so the tabs
  // and body below do not move when a response arrives or a stream starts (HTTP: 10, gRPC: 8).
  void reserve(int rows);
  // A request is in flight and no earlier figures are shown: the reserved rows show the phases
  // to come (faint) with a live elapsed clock and a moving bar, instead of empty space. Ends
  // with set() / clear().
  void set_waiting(Clock::time_point since);
  // What is known while waiting, before the engine's breakdown arrives: the first byte / message
  // came at first_ms after the send (< 0: nothing yet), with `bytes` body bytes (HTTP) or `msgs`
  // messages (gRPC) so far. The phases before it are drawn as done, the transfer as running.
  void set_live(double first_ms, uint64_t bytes, size_t msgs);
  // While waiting on a user-paced (Manual) stream: `sent` messages went out, `ended`: End was
  // clicked. Before the first one the rows say the stream is open and idle, not "waiting for the
  // response". Reset by set_waiting().
  void set_paced(int sent, bool ended);
  // The request was canceled: the reserved rows stay (nothing below moves) and say so; the
  // figures of an earlier run are not shown under a canceled request. Ends with set() / clear().
  void set_cancelled(double after_ms);
  // A live model (TimingModel::live_row) moves on: the running phase's time, the total so far and
  // the header note. Repaints only (no layout).
  void live_tick(double live_ms, double total_ms, const std::string& head);
  bool has_figures() const { return cur_.valid; }
  // The connection pill in the header was clicked (screen rectangle of the pill). Set: the pill
  // is drawn with a drop-down mark and a hand cursor.
  void set_on_conn_click(std::function<void(const wxRect&)> f) { on_conn_ = std::move(f); }
  wxSize DoGetBestClientSize() const override;

 private:
  void on_paint();
  void on_motion(const wxPoint& p);
  int row_h() const { return FromDIP(18); }
  int row_count(const TimingModel& m) const;  // phase rows, plus the sub rows when expanded
  bool expand_sub_ = false;                   // sub rows (redirect hops) shown
  int header_h() const { return FromDIP(22); }
  int footer_h() const { return FromDIP(24); }
  TimingModel cur_;
  TimingModel prev_;
  bool has_prev_ = false;
  bool stale_ = false;
  bool reserved_ = false;
  mutable bool laid_collapsed_ = false;  // the collapsed state the last layout used
  // A short response pane (a 768 px screen, request over response): the one-line bar whatever
  // the remembered choice, so the body keeps its room; a click on the header still opens it
  // (short_open_, this view only).
  bool short_ = false, short_open_ = false;
  bool collapsed() const;
  int reserve_rows_ = 10;
  std::vector<std::pair<wxRect, std::string>> tips_;
  int hover_ = -2;
  wxRect mute_;  // a clicked spot whose popover is open: no tooltip there until the mouse leaves it
  wxRect pill_;  // connection pill as last drawn (empty: none)
  bool waiting_ = false;
  bool cancelled_ = false;
  double cancel_ms_ = 0;
  Clock::time_point wait_since_{};
  double live_first_ms_ = -1;
  uint64_t live_bytes_ = 0;
  size_t live_msgs_ = 0;
  int paced_sent_ = -1;  // set_paced(); -1: not a user-paced stream
  bool paced_ended_ = false;
  wxTimer wait_timer_;
  void paint_waiting(wxDC& dc);
  std::function<void(const wxRect&)> on_conn_;
};

// ------------------------------------------------------------------ tab strip

class TabStrip : public wxWindow {
 public:
  TabStrip(wxWindow* parent, std::function<void(int)> on_select);
  int add(const std::string& label);
  void set_label(int i, const std::string& label, const std::string& count = {});
  // Color of the tab's count (e.g. Tests "3/4": Ok when all pass, Err when one fails), drawn as
  // a filled pill so it reads at a glance whether the tab is selected or not. Plain: default.
  void set_count_tone(int i, Tone t);
  void show_tab(int i, bool show);
  bool tab_shown(int i) const { return tabs_[static_cast<size_t>(i)].shown; }
  void select(int i);
  int selection() const { return sel_; }
  // Right-aligned free text (e.g. "JSON, 9.87 MB").
  void set_note(const std::string& note);
  wxSize DoGetBestClientSize() const override;

 private:
  struct Tab {
    std::string label, count;
    bool shown = true;
    wxRect rect;
    Tone tone = Tone::Plain;
  };
  void on_paint();
  void more_menu();
  std::vector<Tab> tabs_;
  int sel_ = 0;
  std::string note_;
  std::function<void(int)> on_select_;
  // The "»" button: shown tabs that do not fit (never the selected one) are listed in its menu
  // instead of running past the strip's edge. Empty when every tab fits.
  wxRect more_rect_;
  bool more_hover_ = false;
};

// ------------------------------------------------------------------ chip bar

// A flat toolbar of text "chips": a left group (segmented, e.g. Pretty | Raw | Hex), a note in the
// middle and right-aligned action chips. Custom drawn so it looks the same in light and dark.
// A report-mode list that paints each row once. Under dark mode wx draws a whole row itself on
// every sub-item notification (columns x columns text draws per row: 15-45 ms for an 8 x 5
// list's first paint), and the row ends up in the last column's colors. This one draws the row
// on its item notification, every cell with its own color and font.
class FastList : public wxListCtrl {
 public:
  using wxListCtrl::wxListCtrl;
#ifdef __WXMSW__
  bool MSWOnNotify(int id, WXLPARAM lp, WXLPARAM* result) override;

 private:
  void paint_row(WXHDC dc, long item);
#endif
};

class ChipBar : public wxWindow {
 public:
  ChipBar(wxWindow* parent, std::function<void(int)> on_click);
  // group >= 0: segmented, one chip of the group is "on" (set_on). right: aligned to the right.
  void add(int id, const std::string& label, const std::string& tip, int group = -1, bool right = false);
  void set_on(int id, bool on);
  void set_enabled(int id, bool en);
  void set_visible(int id, bool vis);
  void set_label(int id, const std::string& label);
  void set_note(const std::string& note, Tone tone = Tone::Dim);
  // Draw the tab strip's bottom rule too (a chip bar that sits at the end of a TabStrip row).
  void set_bottom_rule(bool on) { bottom_rule_ = on; Refresh(false); }
  // The bar's minimum width follows its visible chips (none: no width), so a bar next to a tab
  // strip takes only the room it needs and the parent is laid out again when that changes.
  void set_fit_width(bool on) { fit_ = on; refit(); }
  wxSize DoGetBestClientSize() const override;

 private:
  void refit();
  bool fit_ = false;
  bool bottom_rule_ = false;
  struct Chip {
    int id;
    std::string label, tip;
    int group;
    bool right, on = false, enabled = true, visible = true;
    bool folded = false;  // no room on the bar: listed under the "more" chip instead
    wxRect rect;
    wxSize ext;  // the label's text extent in the bar's font (measured on first paint)
  };
  wxRect more_rect_;  // the "more" chip (right chips that did not fit), empty when all fit
  void more_menu();
  Chip* find(int id);
  void on_paint();
  std::vector<Chip> chips_;
  std::string note_;
  Tone note_tone_ = Tone::Dim;
  wxRect note_rect_;       // where the note was drawn
  bool note_cut_ = false;  // parts of it were dropped for lack of room (full note = tooltip)
  int hover_ = -1;
  int tip_ = -2;
  std::function<void(int)> on_click_;
};

}  // namespace watt::app::rv
