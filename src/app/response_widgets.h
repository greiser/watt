#pragma once
// Custom-drawn pieces of the response view: the status strip, the timing waterfall and the tab
// strip. All painting is wxDC on the UI thread over a handful of rectangles: nothing here scales
// with the response size.
#include <wx/timer.h>
#include <wx/window.h>

#include <functional>
#include <string>
#include <vector>

#include "core/timing.h"

namespace watt::app::rv {

// Colours shared by the response view (light / dark follow the system appearance).
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
};

class StatusStrip : public wxWindow {
 public:
  explicit StatusStrip(wxWindow* parent);
  // badge: "200 OK" drawn as a coloured pill (empty = none); segments follow, separated by dots.
  void set(const std::string& badge, Tone badge_tone, std::vector<StatusSeg> segs);
  // Running clock: draws "label  12.3 ms" with the time since `since`, repainted ~30x/s until set().
  void set_busy(const std::string& label, Clock::time_point since);
  void set_busy_label(const std::string& label) { busy_label_ = label; Refresh(); }
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
  bool busy_ = false;
  std::string busy_label_;
  Clock::time_point since_{};
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
  bool failed = false;    // the phase the request failed in (drawn in the error colour)
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
  double server_wait = 0;  // the TTFB / wait phase
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
};

class Waterfall : public wxWindow {
 public:
  explicit Waterfall(wxWindow* parent);
  ~Waterfall() override;
  // Re-reads the global collapsed choice and re-lays out the parent.
  void relayout();
  // prev: the previous run of this tab (same kind) for the delta column; nullptr = none.
  void set(const TimingModel& cur, const TimingModel* prev);
  // keep_reserve: keep the reserved height (a request is about to run); false: the empty state.
  void clear(bool keep_reserve = true);
  // While a new request is in flight the old figures stay, dimmed and labelled "previous run".
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
  // The request was cancelled: the reserved rows stay (nothing below moves) and say so; the
  // figures of an earlier run are not shown under a cancelled request. Ends with set() / clear().
  void set_cancelled(double after_ms);
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
  int reserve_rows_ = 10;
  std::vector<std::pair<wxRect, std::string>> tips_;
  int hover_ = -2;
  wxRect pill_;  // connection pill as last drawn (empty: none)
  bool waiting_ = false;
  bool cancelled_ = false;
  double cancel_ms_ = 0;
  Clock::time_point wait_since_{};
  double live_first_ms_ = -1;
  uint64_t live_bytes_ = 0;
  size_t live_msgs_ = 0;
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
  // Colour of the tab's count (e.g. Tests "3/4": Ok when all pass, Err when one fails), drawn as
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
  std::vector<Tab> tabs_;
  int sel_ = 0;
  std::string note_;
  std::function<void(int)> on_select_;
};

// ------------------------------------------------------------------ chip bar

// A flat toolbar of text "chips": a left group (segmented, e.g. Pretty | Raw | Hex), a note in the
// middle and right-aligned action chips. Custom drawn so it looks the same in light and dark.
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
  wxSize DoGetBestClientSize() const override;

 private:
  bool bottom_rule_ = false;
  struct Chip {
    int id;
    std::string label, tip;
    int group;
    bool right, on = false, enabled = true, visible = true;
    bool folded = false;  // no room on the bar: listed under the "more" chip instead
    wxRect rect;
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
