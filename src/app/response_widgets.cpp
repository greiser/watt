#include "app/response_widgets.h"

#include <wx/config.h>
#include <wx/dcbuffer.h>
#include <wx/menu.h>
#include <wx/control.h>
#include <wx/settings.h>
#include <wx/tooltip.h>

#include <algorithm>
#include <cmath>

#include "app/response_format.h"

namespace watt::app::rv {
namespace {

// Removes the window's tooltip and takes a tooltip that is on screen down at once (a popover
// opened by the same click must not sit under it).
void hide_tooltip_now(wxWindow* w) {
  w->UnsetToolTip();
  wxToolTip::Enable(false);
  wxToolTip::Enable(true);
}

wxColour mix(const wxColour& a, const wxColour& b, double t) {
  auto m = [t](unsigned char x, unsigned char y) {
    return static_cast<unsigned char>(std::lround(x + (y - x) * t));
  };
  return wxColour(m(a.Red(), b.Red()), m(a.Green(), b.Green()), m(a.Blue(), b.Blue()));
}

wxString U(const std::string& s) { return wxString::FromUTF8(s); }

// Text drawn left-aligned and vertically centred in r; returns its width.
int draw_text(wxDC& dc, const wxString& s, int x, const wxRect& r) {
  const wxSize e = dc.GetTextExtent(s);
  dc.DrawText(s, x, r.y + (r.height - e.y) / 2);
  return e.x;
}

void draw_text_right(wxDC& dc, const wxString& s, int right, const wxRect& r) {
  const wxSize e = dc.GetTextExtent(s);
  dc.DrawText(s, right - e.x, r.y + (r.height - e.y) / 2);
}

void update_tip(wxWindow* w, int& hover, int idx, const std::string& tip) {
  if (idx == hover) return;
  hover = idx;
  if (idx < 0 || tip.empty()) w->UnsetToolTip();
  else w->SetToolTip(U(tip));
}

// Collapsed or expanded waterfall: one choice for every tab, remembered across runs.
bool& collapsed_pref() {
  static bool v = [] {
    bool c = false;
    if (wxConfigBase* cfg = wxConfigBase::Get()) cfg->Read("Response/TimingCollapsed", &c, false);
    return c;
  }();
  return v;
}

// Every live waterfall: the collapsed choice is global, so a toggle re-lays out all of them (the
// hidden tabs too), not just the one clicked.
std::vector<Waterfall*>& all_waterfalls() {
  static std::vector<Waterfall*> v;
  return v;
}

}  // namespace

const Theme& Theme::get() {
  static Theme t = [] {
    Theme th;
    th.dark = wxSystemSettings::GetAppearance().IsDark();
    th.bg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
    th.fg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    if (th.dark && th.bg.GetLuminance() > 0.5) {  // system colours not themed: pick our own
      th.bg = wxColour(32, 32, 32);
      th.fg = wxColour(220, 220, 220);
    }
    th.dim = mix(th.fg, th.bg, 0.45);
    th.faint = mix(th.fg, th.bg, 0.82);
    th.grid = mix(th.fg, th.bg, 0.90);
    if (th.dark) {
      th.ok = wxColour(80, 200, 120);
      th.redirect = wxColour(100, 170, 240);
      th.warn = wxColour(240, 170, 70);
      th.err = wxColour(245, 100, 100);
      th.info = wxColour(120, 180, 255);
      th.accent = wxColour(255, 140, 60);
    } else {
      th.ok = wxColour(20, 135, 60);
      th.redirect = wxColour(0, 100, 200);
      th.warn = wxColour(185, 105, 0);
      th.err = wxColour(200, 40, 40);
      th.info = wxColour(0, 95, 200);
      th.accent = wxColour(230, 100, 20);
    }
    return th;
  }();
  return t;
}

wxColour tone_colour(Tone t) {
  const Theme& th = Theme::get();
  switch (t) {
    case Tone::Plain: return th.fg;
    case Tone::Dim: return th.dim;
    case Tone::Ok: return th.ok;
    case Tone::Redirect: return th.redirect;
    case Tone::Warn: return th.warn;
    case Tone::Err: return th.err;
    case Tone::Info: return th.info;
    case Tone::Accent: return th.accent;
  }
  return th.fg;
}

// ------------------------------------------------------------------ status strip

StatusStrip::StatusStrip(wxWindow* parent) : wxWindow(parent, wxID_ANY), timer_(this) {
  SetBackgroundStyle(wxBG_STYLE_PAINT);
  SetBackgroundColour(Theme::get().bg);
  Bind(wxEVT_PAINT, [this](wxPaintEvent&) { on_paint(); });
  Bind(wxEVT_MOTION, [this](wxMouseEvent& e) { on_motion(e.GetPosition()); });
  Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { update_tip(this, hover_, -1, {}); });
  Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
    for (size_t i = 0; i < hit_.size(); ++i)
      if (segs_[i].click && hit_[i].Contains(e.GetPosition()) && on_click_) {
        hide_tooltip_now(this);
        hover_ = -2;
        const wxRect r = hit_[i];
        on_click_(segs_[i].click, wxRect(ClientToScreen(r.GetTopLeft()), r.GetSize()));
        return;
      }
    e.Skip();
  });
  Bind(wxEVT_TIMER, [this](wxTimerEvent&) { Refresh(false); });
  Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { Refresh(false); e.Skip(); });
}

wxSize StatusStrip::DoGetBestClientSize() const { return wxSize(FromDIP(200), FromDIP(34)); }

void StatusStrip::set(const std::string& badge, Tone badge_tone, std::vector<StatusSeg> segs) {
  busy_ = false;
  timer_.Stop();
  badge_ = badge;
  badge_tone_ = badge_tone;
  segs_ = std::move(segs);
  // The strip is one line: an error text with line breaks (gRPC status messages, WSA errors)
  // would otherwise be drawn over the timing header below. Breaks and runs of blanks become one
  // space; the full text stays in the tooltip.
  for (StatusSeg& s : segs_) {
    std::string one;
    one.reserve(s.text.size());
    for (char c : s.text) {
      const bool blank = c == '\n' || c == '\r' || c == '\t' || c == ' ';
      if (blank && (one.empty() || one.back() == ' ')) continue;
      one += blank ? ' ' : c;
    }
    while (!one.empty() && one.back() == ' ') one.pop_back();
    s.text = std::move(one);
  }
  hover_ = -2;
  UnsetToolTip();
  Refresh(false);
  Update();  // paint now: the status is the first thing the user looks for
}

void StatusStrip::set_busy(const std::string& label, Clock::time_point since) {
  busy_ = true;
  busy_label_ = label;
  since_ = since;
  badge_.clear();
  segs_.clear();
  hit_.clear();
  UnsetToolTip();
  timer_.Start(33);
  Refresh(false);
}

void StatusStrip::on_paint() {
  wxAutoBufferedPaintDC dc(this);
  const Theme& th = Theme::get();
  const wxRect cr = GetClientRect();
  dc.SetBackground(wxBrush(th.bg));
  dc.Clear();
  dc.SetPen(wxPen(th.grid));
  dc.DrawLine(0, cr.height - 1, cr.width, cr.height - 1);
  const int pad = FromDIP(10);
  int x = pad;
  wxFont base = GetFont();
  wxFont bold = base.Bold();
  wxFont big = bold.Larger();
  hit_.assign(segs_.size(), wxRect());
  if (busy_) {
    dc.SetFont(base);
    dc.SetTextForeground(th.dim);
    x += draw_text(dc, U(busy_label_), x, cr) + FromDIP(10);
    dc.SetFont(big);
    dc.SetTextForeground(th.fg);
    draw_text(dc, U(fmt::ms(ms_between(since_, Clock::now()))), x, cr);
    return;
  }
  if (!badge_.empty()) {
    dc.SetFont(big);
    const wxSize e = dc.GetTextExtent(U(badge_));
    const wxColour c = tone_colour(badge_tone_);
    const int h = e.y + FromDIP(6);
    const wxRect pill(x, (cr.height - h) / 2, e.x + FromDIP(16), h);
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(mix(th.bg, c, th.dark ? 0.22 : 0.14)));
    dc.DrawRoundedRectangle(pill, FromDIP(4));
    dc.SetTextForeground(c);
    dc.DrawText(U(badge_), pill.x + FromDIP(8), pill.y + (pill.height - e.y) / 2);
    x = pill.GetRight() + FromDIP(12);
  }
  // Fit: drop the least important segments (highest `drop`) until the rest fits, then elide the
  // widest remaining one. Nothing is cut off at the edge.
  const int gap = FromDIP(19);  // divider + spacing between two segments
  const int avail = cr.width - x - pad;
  std::vector<int> w(segs_.size());
  std::vector<char> keep(segs_.size(), 1);
  for (size_t i = 0; i < segs_.size(); ++i) {
    dc.SetFont(segs_[i].bold ? bold : base);
    w[i] = dc.GetTextExtent(U(segs_[i].text)).x;
  }
  auto used = [&] {
    int u = 0, n = 0;
    for (size_t i = 0; i < segs_.size(); ++i)
      if (keep[i]) u += w[i] + (n++ ? gap : 0);
    return u;
  };
  while (used() > avail) {
    int worst = -1;
    for (size_t i = 0; i < segs_.size(); ++i)
      if (keep[i] && segs_[i].drop > 0 && (worst < 0 || segs_[i].drop >= segs_[static_cast<size_t>(worst)].drop))
        worst = static_cast<int>(i);
    if (worst < 0) break;
    keep[static_cast<size_t>(worst)] = 0;
  }
  std::vector<wxString> text(segs_.size());
  for (size_t i = 0; i < segs_.size(); ++i) text[i] = U(segs_[i].text);
  if (const int over = used() - avail; over > 0) {
    int widest = -1;
    for (size_t i = 0; i < segs_.size(); ++i)
      if (keep[i] && (widest < 0 || w[i] > w[static_cast<size_t>(widest)])) widest = static_cast<int>(i);
    if (widest >= 0) {
      const size_t k = static_cast<size_t>(widest);
      dc.SetFont(segs_[k].bold ? bold : base);
      text[k] = wxControl::Ellipsize(text[k], dc, wxELLIPSIZE_END, std::max(FromDIP(24), w[k] - over));
      w[k] = dc.GetTextExtent(text[k]).x;
    }
  }
  dropped_tip_.clear();
  bool first = true;
  for (size_t i = 0; i < segs_.size(); ++i) {
    const StatusSeg& s = segs_[i];
    if (!keep[i]) {
      dropped_tip_ += (dropped_tip_.empty() ? "" : "\n") + s.text;
      continue;
    }
    if (!first) {
      // Thin divider between segments.
      x += FromDIP(9);
      dc.SetPen(wxPen(th.faint));
      dc.DrawLine(x, cr.height / 2 - FromDIP(6), x, cr.height / 2 + FromDIP(6));
      x += FromDIP(10);
    }
    first = false;
    if (x >= cr.width - pad) break;
    dc.SetFont(s.bold ? bold : base);
    dc.SetTextForeground(tone_colour(s.tone));
    const wxSize e = dc.GetTextExtent(text[i]);
    dc.DrawText(text[i], x, (cr.height - e.y) / 2);
    hit_[i] = wxRect(x - FromDIP(4), 0, e.x + FromDIP(8), cr.height);
    x += e.x;
  }
  if (!dropped_tip_.empty()) dropped_tip_ = "Not shown (pane too narrow):\n" + dropped_tip_;
}

void StatusStrip::on_motion(const wxPoint& p) {
  for (size_t i = 0; i < hit_.size(); ++i)
    if (hit_[i].Contains(p)) {
      SetCursor(segs_[i].click ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
      return update_tip(this, hover_, static_cast<int>(i), segs_[i].tip);
    }
  SetCursor(wxNullCursor);
  if (!dropped_tip_.empty()) return update_tip(this, hover_, 1000, dropped_tip_);
  update_tip(this, hover_, -1, {});
}

// ------------------------------------------------------------------ waterfall

Waterfall::Waterfall(wxWindow* parent) : wxWindow(parent, wxID_ANY) {
  all_waterfalls().push_back(this);
  laid_collapsed_ = collapsed_pref();
  SetBackgroundStyle(wxBG_STYLE_PAINT);
  SetBackgroundColour(Theme::get().bg);
  Bind(wxEVT_PAINT, [this](wxPaintEvent&) { on_paint(); });
  Bind(wxEVT_MOTION, [this](wxMouseEvent& e) { on_motion(e.GetPosition()); });
  Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { update_tip(this, hover_, -1, {}); });
  Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
    if (on_conn_ && !pill_.IsEmpty() && pill_.Contains(e.GetPosition())) {
      hide_tooltip_now(this);
      hover_ = -2;
      on_conn_(wxRect(ClientToScreen(pill_.GetTopLeft()), pill_.GetSize()));
      return;
    }
    // A phase with parts (Redirects): its row expands / collapses them.
    if (cur_.valid && !collapsed_pref() && e.GetPosition().y >= header_h()) {
      int row = (e.GetPosition().y - header_h()) / row_h();
      for (const Phase& p : cur_.phases) {
        if (row == 0) {
          if (!p.sub.empty()) {
            expand_sub_ = !expand_sub_;
            hover_ = -2;
            UnsetToolTip();
            InvalidateBestSize();
            GetParent()->Layout();
            Refresh(false);
          }
          break;
        }
        --row;
        if (expand_sub_) row -= static_cast<int>(p.sub.size());
        if (row < 0) break;
      }
    }
    if (e.GetPosition().y < header_h()) {
      bool& c = collapsed_pref();
      c = !c;
      if (wxConfigBase* cfg = wxConfigBase::Get()) cfg->Write("Response/TimingCollapsed", c);
      for (Waterfall* w : all_waterfalls()) w->relayout();
    }
  });

  Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { Refresh(false); e.Skip(); });
  wait_timer_.SetOwner(this);
  Bind(wxEVT_TIMER, [this](wxTimerEvent&) { Refresh(false); }, wait_timer_.GetId());
}

void Waterfall::set_live(double first_ms, uint64_t bytes, size_t msgs) {
  live_first_ms_ = first_ms;
  live_bytes_ = bytes;
  live_msgs_ = msgs;
  if (waiting_) Refresh(false);
}

void Waterfall::set_waiting(Clock::time_point since) {
  waiting_ = true;
  cancelled_ = false;
  wait_since_ = since;
  live_first_ms_ = -1;
  live_bytes_ = 0;
  live_msgs_ = 0;
  if (!wait_timer_.IsRunning()) wait_timer_.Start(66);
  Refresh(false);
}

void Waterfall::set_cancelled(double after_ms) {
  const bool had_rows = cur_.valid;
  const int rows = had_rows ? static_cast<int>(cur_.phases.size()) : reserve_rows_;
  cur_ = TimingModel{};
  has_prev_ = false;
  stale_ = false;
  waiting_ = false;
  wait_timer_.Stop();
  cancelled_ = true;
  cancel_ms_ = after_ms;
  hover_ = -2;
  UnsetToolTip();
  const bool resize = !reserved_ || rows != reserve_rows_;
  reserved_ = true;
  reserve_rows_ = rows;
  if (resize) {
    InvalidateBestSize();
    GetParent()->Layout();
  }
  Refresh(false);
}

void Waterfall::paint_waiting(wxDC& dc) {
  const Theme& th = Theme::get();
  const wxRect cr = GetClientRect();
  const double el = ms_between(wait_since_, Clock::now());
  const int pad = FromDIP(10);
  dc.SetFont(GetFont().Bold());
  dc.SetTextForeground(th.fg);
  const wxString title = wxString::FromUTF8(collapsed_pref() ? "\xE2\x96\xB8 Timing" : "\xE2\x96\xBE Timing");
  const wxSize te = dc.GetTextExtent(title);
  dc.DrawText(title, pad, (header_h() - te.y) / 2);
  dc.SetFont(GetFont());
  dc.SetTextForeground(cancelled_ ? th.warn : th.accent);
  const bool grpc = reserve_rows_ == 8;
  const bool got = !cancelled_ && live_first_ms_ >= 0;
  auto el_text = [](double v) {
    char b[32];
    if (v < 1000) std::snprintf(b, sizeof b, "%.0f ms", v);
    else std::snprintf(b, sizeof b, "%.2f s", v / 1000.0);
    return std::string(b);
  };
  std::string head;
  if (cancelled_) {
    head = "cancelled after " + fmt::ms(cancel_ms_) + " \xC2\xB7 no response, so no phase breakdown";
  } else if (!got) {
    head = "waiting for the response \xC2\xB7 " + el_text(el);
  } else if (grpc) {
    head = "streaming \xC2\xB7 " + std::to_string(live_msgs_) + (live_msgs_ == 1 ? " message \xC2\xB7 " : " messages \xC2\xB7 ") +
           el_text(el);
  } else {
    head = "receiving \xC2\xB7 " + fmt::human_bytes(live_bytes_) + " \xC2\xB7 " + el_text(el);
  }
  dc.DrawText(wxString::FromUTF8(head), pad + te.x + FromDIP(12), (header_h() - dc.GetCharHeight()) / 2);
  if (collapsed_pref()) return;
  static const char* const kHttp[] = {"Prepare", "Queue", "DNS", "Socket", "Connect", "TLS", "Send", "Wait (TTFB)",
                                      "Download", "Process"};
  static const char* const kGrpc[] = {"Queue", "DNS", "Connect", "TLS", "Send", "Wait (TTFB)", "Download", "Other"};
  const int n = grpc ? 8 : std::min(reserve_rows_, 10);
  const int label_w = FromDIP(84);
  const int value_w = FromDIP(76);
  const int bar_x = pad + label_w;
  const int bar_w = std::max(FromDIP(40), cr.width - bar_x - value_w - pad - FromDIP(8));
  const int value_r = bar_x + bar_w + FromDIP(8) + value_w;
  const int rh = row_h();
  if (cancelled_) {
    // Same rows, frozen: the phases the request did not finish.
    for (int i = 0; i < n; ++i) {
      const int y = header_h() + i * rh;
      dc.SetTextForeground(th.faint);
      dc.DrawText(wxString::FromUTF8(grpc ? kGrpc[i] : kHttp[i]), pad, y + (rh - dc.GetCharHeight()) / 2);
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.SetBrush(wxBrush(th.grid));
      dc.DrawRectangle(wxRect(bar_x, y + rh / 2 - FromDIP(1), bar_w, FromDIP(2)));
    }
    dc.SetTextForeground(th.dim);
    dc.SetFont(GetFont().Smaller());
    dc.DrawText(wxString::FromUTF8("cancelled by you (Esc or Cancel): the engine reports the phases only with a response"),
                pad, header_h() + n * rh + (footer_h() - dc.GetCharHeight()) / 2);
    return;
  }
  // The time axis is the elapsed time so far. The engine times every phase but reports them with
  // the response, so what is known now is drawn as such: the phases up to the first byte as one
  // block (running, or done at the first byte), then the transfer (Download) as it runs.
  const int down = grpc ? 6 : 8;  // index of the Download row
  const double span = std::max(el, 1.0);
  auto xt = [&](double t) { return bar_x + static_cast<int>(std::lround(std::clamp(t / span, 0.0, 1.0) * bar_w)); };
  const wxColour wait_c = th.dark ? wxColour(80, 200, 120) : wxColour(30, 160, 80);
  const wxColour down_c = th.dark ? wxColour(90, 170, 255) : wxColour(40, 130, 230);
  for (int i = 0; i < n; ++i) {
    const int y = header_h() + i * rh;
    const bool pre = i < down;
    const bool done = pre && got;
    const bool active = got ? i == down : pre;
    dc.SetFont(GetFont());
    dc.SetTextForeground(done ? th.dim : active ? th.fg : th.faint);
    dc.DrawText(wxString::FromUTF8(grpc ? kGrpc[i] : kHttp[i]), pad, y + (rh - dc.GetCharHeight()) / 2);
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(th.grid));
    dc.DrawRectangle(wxRect(bar_x, y + rh / 2 - FromDIP(1), bar_w, FromDIP(2)));
    if (i == down && got) {
      // The transfer so far: first byte -> now, growing.
      const int x0 = xt(live_first_ms_);
      dc.SetBrush(wxBrush(down_c));
      dc.DrawRoundedRectangle(wxRect(x0, y + (rh - FromDIP(10)) / 2, std::max(FromDIP(2), bar_x + bar_w - x0), FromDIP(10)),
                              FromDIP(2));
      dc.SetFont(GetFont().Smaller());
      dc.SetTextForeground(th.accent);
      const std::string v = grpc ? std::to_string(live_msgs_) + " msgs" : fmt::human_bytes(live_bytes_);
      const wxString t = wxString::FromUTF8(v + " \xE2\x80\xA6");
      dc.DrawText(t, value_r - dc.GetTextExtent(t).x, y + (rh - dc.GetCharHeight()) / 2);
    } else if (!pre) {
      dc.SetFont(GetFont().Smaller());
      dc.SetTextForeground(th.faint);
      const wxString t = "pending";
      dc.DrawText(t, value_r - dc.GetTextExtent(t).x, y + (rh - dc.GetCharHeight()) / 2);
    }
  }
  // The phases before the first byte (DNS ... server wait). Still running: one block over their
  // rows (accent, a sweep moving inside it). Done: a marker, not a bar (their split is not known
  // yet): a thin rule at the first byte across those rows and a thin line on the Wait row.
  const int top = header_h() + (grpc ? 1 : 2) * rh + FromDIP(2);
  const int bottom = header_h() + down * rh - FromDIP(2);
  const int x1 = got ? xt(live_first_ms_) : bar_x + bar_w;
  const wxRect block(bar_x, top, std::max(FromDIP(3), x1 - bar_x), bottom - top);
  if (!got) {
    dc.SetPen(wxPen(mix(th.bg, th.accent, 0.6)));
    dc.SetBrush(wxBrush(mix(th.bg, th.accent, 0.14)));
    dc.DrawRoundedRectangle(block, FromDIP(3));
    const int sweep = std::max(FromDIP(30), block.width / 6);
    const int ph = static_cast<int>(std::fmod(el / 1400.0, 1.0) * (block.width + sweep)) - sweep;
    const int sx = std::max(block.x + 1, block.x + ph), ex = std::min(block.GetRight() - 1, block.x + ph + sweep);
    if (ex > sx) {
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.SetBrush(wxBrush(mix(th.bg, th.accent, 0.35)));
      dc.DrawRectangle(wxRect(sx, block.y + 1, ex - sx, block.height - 2));
    }
  } else {
    dc.SetPen(wxPen(mix(th.bg, wait_c, 0.7), 1, wxPENSTYLE_SHORT_DASH));
    dc.DrawLine(x1, top, x1, bottom);
    const int wy = header_h() + (down - 1) * rh + rh / 2;
    dc.SetPen(wxPen(mix(th.bg, wait_c, 0.8), FromDIP(2)));
    dc.DrawLine(bar_x, wy, x1, wy);
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(wait_c));
    dc.DrawCircle(x1, wy, FromDIP(3));
  }
  {
    dc.SetFont(GetFont().Smaller());
    dc.SetTextForeground(th.dim);
    const std::string what = grpc ? "connect, send, server wait" : "DNS, connect, TLS, send, server wait";
    const std::string t = got ? "\xE2\x9C\x93 first " + std::string(grpc ? "message" : "byte") + " at +" + fmt::ms(live_first_ms_) +
                                    " (" + what + " done; split with the response)"
                              : what + ": in progress";
    const wxString wt = wxString::FromUTF8(t);
    const wxSize e = dc.GetTextExtent(wt);
    int tx = block.x + FromDIP(8);
    int ty = block.y + (block.height - e.y) / 2;
    if (got) {
      // Next to the rule, on the row above Wait; left of it when there is no room on the right.
      ty = header_h() + (down - 2) * rh + (rh - e.y) / 2;
      tx = x1 + FromDIP(8);
      if (tx + e.x > bar_x + bar_w) tx = std::max(bar_x + FromDIP(4), x1 - FromDIP(8) - e.x);
    }
    dc.DrawText(wt, std::max(bar_x + FromDIP(4), tx), ty);
    if (got) {
      const wxString v = wxString::FromUTF8("+" + fmt::ms(live_first_ms_));
      dc.SetTextForeground(th.dim);
      dc.DrawText(v, value_r - dc.GetTextExtent(v).x, header_h() + (down - 1) * rh + (rh - dc.GetCharHeight()) / 2);
    }
  }
  dc.SetTextForeground(th.dim);
  dc.SetFont(GetFont().Smaller());
  dc.DrawText(wxString::FromUTF8(got ? (grpc ? "per-phase split with the trailers, when the stream ends"
                                            : "per-phase split when the download completes")
                                     : "the engine times each phase and reports the split with the response"),
              pad, header_h() + n * rh + (footer_h() - dc.GetCharHeight()) / 2);
}

Waterfall::~Waterfall() {
  auto& v = all_waterfalls();
  v.erase(std::remove(v.begin(), v.end(), this), v.end());
}

void Waterfall::relayout() {
  laid_collapsed_ = collapsed_pref();
  InvalidateBestSize();
  GetParent()->Layout();
  Refresh(false);
}

int Waterfall::row_count(const TimingModel& m) const {
  int n = static_cast<int>(m.phases.size());
  if (expand_sub_)
    for (const Phase& p : m.phases) n += static_cast<int>(p.sub.size());
  return n;
}

wxSize Waterfall::DoGetBestClientSize() const {
  laid_collapsed_ = collapsed_pref();
  if (!cur_.valid && !reserved_) return wxSize(FromDIP(200), 0);
  // Collapsed: the header line alone (with the stacked bar and the total in it).
  if (collapsed_pref()) return wxSize(FromDIP(200), header_h() + FromDIP(2));
  const int rows = cur_.valid ? row_count(cur_) : reserve_rows_;
  return wxSize(FromDIP(200), header_h() + rows * row_h() + footer_h() + FromDIP(6));
}

void Waterfall::reserve(int rows) {
  if (reserved_ && (cur_.valid || rows == reserve_rows_)) return;
  reserved_ = true;
  reserve_rows_ = rows;
  InvalidateBestSize();
  GetParent()->Layout();
  Refresh(false);
}

void Waterfall::set(const TimingModel& cur, const TimingModel* prev) {
  const int rows_before = cur_.valid ? row_count(cur_) : reserved_ ? reserve_rows_ : -1;
  const bool resize = rows_before != row_count(cur);
  reserved_ = true;
  reserve_rows_ = static_cast<int>(cur.phases.size());
  cur_ = cur;
  waiting_ = false;
  cancelled_ = false;
  wait_timer_.Stop();
  has_prev_ = prev && prev->valid && prev->kind == cur.kind;
  if (has_prev_) prev_ = *prev;
  stale_ = false;
  hover_ = -2;
  UnsetToolTip();
  if (resize) {
    InvalidateBestSize();
    GetParent()->Layout();
  }
  Refresh(false);
}

void Waterfall::clear(bool keep_reserve) {
  const bool had_rows = cur_.valid;
  const bool had_reserve = reserved_;
  cur_ = TimingModel{};
  has_prev_ = false;
  stale_ = false;
  waiting_ = false;
  cancelled_ = false;
  wait_timer_.Stop();
  if (!keep_reserve) reserved_ = false;
  if (!reserved_ || had_rows || had_reserve != reserved_) {
    InvalidateBestSize();
    GetParent()->Layout();
  }
  Refresh(false);
}

void Waterfall::set_stale(bool stale) {
  if (!stale && waiting_) {
    waiting_ = false;
    wait_timer_.Stop();
    Refresh(false);
  }
  if (stale_ == stale) return;
  stale_ = stale;
  Refresh(false);
}

void Waterfall::on_paint() {
  wxAutoBufferedPaintDC dc(this);
  // Safety net: a layout made before the collapsed choice changed (e.g. while this tab was
  // hidden) would leave a gap or overlap; fix it on the first paint.
  if (laid_collapsed_ != collapsed_pref()) CallAfter([this] { relayout(); });
  const Theme& th = Theme::get();
  const wxRect cr = GetClientRect();
  dc.SetBackground(wxBrush(th.bg));
  dc.Clear();
  tips_.clear();
  pill_ = wxRect();
  if (!cur_.valid) {
    if (reserved_ && (waiting_ || cancelled_)) return paint_waiting(dc);
    if (reserved_) {
      dc.SetFont(GetFont());
      dc.SetTextForeground(th.dim);
      const char* msg = stale_ ? "Timing \xE2\x80\x94 waiting for the response\xE2\x80\xA6"
                                  : "Timing \xE2\x80\x94 the phase breakdown appears here when a response arrives";
      dc.DrawText(wxString::FromUTF8(msg), FromDIP(10), (header_h() - dc.GetCharHeight()) / 2);
    }
    return;
  }
  const wxColour fg = stale_ ? th.dim : th.fg;
  const wxColour dim = stale_ ? th.faint : th.dim;
  wxFont base = GetFont();
  wxFont small = base.Smaller();
  wxFont bold = base.Bold();
  const int pad = FromDIP(10);
  const int label_w = FromDIP(84);
  const int value_w = FromDIP(76);
  const int delta_w = has_prev_ ? FromDIP(72) : 0;
  const int bar_x = pad + label_w;
  const int bar_w = std::max(FromDIP(40), cr.width - bar_x - value_w - delta_w - pad - FromDIP(8));
  const int value_r = bar_x + bar_w + FromDIP(8) + value_w;
  const int delta_r = value_r + delta_w;
  const double total = std::max(cur_.total + cur_.after, 1e-9);
  auto px = [&](double v) { return static_cast<int>(std::lround(v / total * bar_w)); };

  // Header: title, connection marker, delta column title. Click toggles collapsed.
  wxRect hr(0, 0, cr.width, header_h());
  dc.SetFont(bold);
  dc.SetTextForeground(fg);
  int x = pad;
  x += draw_text(dc, collapsed_pref() ? wxString::FromUTF8("\xE2\x96\xB8 Timing") : wxString::FromUTF8("\xE2\x96\xBE Timing"), x, hr);
  x += FromDIP(10);
  if (stale_) {
    dc.SetFont(small);
    dc.SetTextForeground(dim);
    x += draw_text(dc, "previous run", x, hr) + FromDIP(10);
  }
  pill_ = wxRect();
  if (!cur_.conn_note.empty()) {
    dc.SetFont(small);
    const wxString note = U(cur_.conn_note) + (on_conn_ ? wxString::FromUTF8("  \xE2\x96\xBE") : wxString());
    const wxSize e = dc.GetTextExtent(note);
    const wxColour c = cur_.reused ? th.ok : th.info;
    const wxRect pill(x, (hr.height - e.y - FromDIP(4)) / 2, e.x + FromDIP(10), e.y + FromDIP(4));
    dc.SetPen(on_conn_ ? wxPen(mix(th.bg, c, 0.45)) : *wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(mix(th.bg, c, th.dark ? 0.22 : 0.14)));
    dc.DrawRoundedRectangle(pill, FromDIP(3));
    dc.SetTextForeground(stale_ ? th.dim : c);
    dc.DrawText(note, pill.x + FromDIP(5), pill.y + FromDIP(2));
    pill_ = pill;
    std::string tip = cur_.reused ? "The request went out on a connection that was already open: no DNS, "
                                    "TCP or TLS handshake was needed."
                                  : "A new connection was opened for this request (DNS, TCP and TLS as "
                                    "applicable are included in the total).";
    if (on_conn_) tip += "\nClick for the connection details (addresses, protocol, TLS, certificate).";
    tips_.push_back({pill, tip});
    x = pill.GetRight() + FromDIP(12);
  }
  if (collapsed_pref()) {
    // Compact: one line. Every phase as a segment of one stacked bar, the total at the right;
    // the bar's tooltip lists the whole breakdown (Postman's timing popover), each segment its
    // own phase.
    dc.SetFont(bold);
    std::string tot = "Total " + fmt::ms(cur_.total);
    if (has_prev_) tot += "  " + fmt::delta_ms(cur_.total - prev_.total);
    // The post-response script runs after the clock stops but has its own bar on this scale:
    // the headline names it, so a bar longer than the total is never unexplained.
    if (cur_.after > 0) tot += " + post-script " + fmt::ms(cur_.after);
    const wxString twx = U(tot);
    const int tw = dc.GetTextExtent(twx).x;
    dc.SetTextForeground(fg);
    draw_text_right(dc, twx, cr.width - pad, hr);
    tips_.push_back({wxRect(cr.width - pad - tw, 0, tw, hr.height), cur_.total_tip});
    const int bx = x, bw = cr.width - pad - tw - FromDIP(14) - bx;
    if (bw > FromDIP(40)) {
      const int bh = FromDIP(10);
      const int by = (hr.height - bh) / 2;
      std::string all = "Timing (click the title for every phase as a row):\n";
      const double sum = std::max(cur_.total + cur_.after, 1e-9);
      double off = 0;
      for (const Phase& ph : cur_.phases) {
        all += "  " + ph.name + "  " + (ph.reused ? std::string("reused") : ph.has_value ? fmt::ms(ph.ms) : "n/a") + "\n";
        if (ph.ms <= 0) continue;
        const int x0 = bx + static_cast<int>(std::lround(off / sum * bw));
        const int x1 = bx + static_cast<int>(std::lround((off + ph.ms) / sum * bw));
        const wxColour pc = ph.failed ? th.err : ph.colour;
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(stale_ ? mix(pc, th.bg, 0.6) : pc));
        dc.DrawRectangle(x0, by, std::max(1, x1 - x0), bh);
        off += ph.ms;
      }
      all += "Total " + fmt::ms(cur_.total) + "  \xC2\xB7  " + cur_.server_label + " " + fmt::ms(cur_.server_wait) +
             ", network " + fmt::ms(cur_.network) + ", client " + fmt::ms(cur_.client);
      tips_.push_back({wxRect(bx, 0, bw, hr.height), all});
      off = 0;
      for (const Phase& ph : cur_.phases) {
        if (ph.ms <= 0) continue;
        const int x0 = bx + static_cast<int>(std::lround(off / sum * bw));
        const int x1 = bx + static_cast<int>(std::lround((off + ph.ms) / sum * bw));
        if (x1 - x0 >= FromDIP(3))
          tips_.push_back({wxRect(x0, 0, x1 - x0, hr.height), ph.name + "  " + fmt::ms(ph.ms) + "\n" + ph.tip + "\n\n" + all});
        off += ph.ms;
      }
    }
    hover_ = -2;
    return;
  }
  dc.SetFont(small);
  dc.SetTextForeground(dim);
  if (has_prev_) {
    draw_text_right(dc, "vs previous", delta_r, hr);
    tips_.push_back({wxRect(value_r, 0, delta_w, hr.height),
                     "Difference to the previous run of the same request (same method, URL and body), per "
                     "phase (negative = faster). Not shown when this request has not been sent before."});
  }

  // Faint grid in the bar area.
  const int rows_top = header_h();
  const int nrows = collapsed_pref() ? 1 : row_count(cur_);
  const int rows_bottom = rows_top + nrows * row_h();
  dc.SetPen(wxPen(th.grid));
  for (int q = 0; q <= 4; ++q) {
    const int gx = bar_x + bar_w * q / 4;
    dc.DrawLine(gx, rows_top, gx, rows_bottom);
  }

  auto delta_text = [&](double d, const wxRect& r, double scale) {
    if (!has_prev_) return;
    // Jitter, not a change (fmt::delta_is_noise).
    const bool noise = fmt::delta_is_noise(d, scale);
    dc.SetTextForeground(noise ? dim : (d < 0 ? th.ok : th.err));
    if (stale_) dc.SetTextForeground(dim);
    draw_text_right(dc, U(fmt::delta_ms(d)), delta_r, r);
  };
  auto prev_of = [&](const std::string& name, double* v) {
    for (const Phase& p : prev_.phases)
      if (p.name == name) {
        *v = p.ms;
        return true;
      }
    return false;
  };

  const int bar_h = FromDIP(10);
  if (collapsed_pref()) {
    const wxRect r(0, rows_top, cr.width, row_h());
    double off = 0;
    for (const Phase& p : cur_.phases) {
      if (p.ms <= 0) continue;
      const int x0 = bar_x + px(off);
      const int w = std::max(1, px(off + p.ms) - px(off));
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.SetBrush(wxBrush(stale_ ? mix(p.colour, th.bg, 0.6) : p.colour));
      dc.DrawRectangle(x0, r.y + (r.height - bar_h) / 2, w, bar_h);
      tips_.push_back({wxRect(x0, r.y, w, r.height), p.name + "  " + fmt::ms(p.ms) + "\n" + p.tip});
      off += p.ms;
    }
    dc.SetFont(base);
    dc.SetTextForeground(dim);
    draw_text(dc, "phases", pad, r);
  } else {
    // The rows: every phase, and under a phase with parts (Redirects) its parts when expanded,
    // each drawn where it ran inside the phase.
    struct RowRef {
      const Phase* p;
      bool sub;
      double off;
    };
    std::vector<RowRef> rows;
    {
      double o = 0;
      for (const Phase& p : cur_.phases) {
        rows.push_back({&p, false, o});
        if (expand_sub_) {
          double so = o;
          for (const Phase& sp : p.sub) {
            rows.push_back({&sp, true, so});
            so += sp.ms;
          }
        }
        o += p.ms;
      }
    }
    for (size_t i = 0; i < rows.size(); ++i) {
      const Phase& p = *rows[i].p;
      const bool is_sub = rows[i].sub;
      const double off = rows[i].off;
      const wxRect r(0, rows_top + static_cast<int>(i) * row_h(), cr.width, row_h());
      dc.SetFont(is_sub ? small : base);
      dc.SetTextForeground(p.failed && !stale_ ? th.err : p.ms > 0 ? fg : dim);
      wxString label = U(p.name);
      if (!p.sub.empty())
        label = wxString::FromUTF8(expand_sub_ ? "\xE2\x96\xBE " : "\xE2\x96\xB8 ") + label;
      draw_text(dc, label, is_sub ? pad + FromDIP(12) : pad, r);
      if (p.reused || !p.has_value) {
        dc.SetFont(small);
        dc.SetTextForeground(p.reused ? (stale_ ? dim : th.ok) : dim);
        draw_text(dc, p.reused ? "reused" : "n/a", bar_x + px(off) + FromDIP(4), r);
        dc.SetFont(base);
        dc.SetTextForeground(dim);
        draw_text_right(dc, wxString::FromUTF8("\xE2\x80\x94"), value_r, r);
      } else {
        const int x0 = bar_x + px(off);
        const int w = std::max(1, px(off + p.ms) - px(off));
        dc.SetPen(*wxTRANSPARENT_PEN);
        const wxColour pc = p.failed ? th.err : p.colour;
        dc.SetBrush(wxBrush(stale_ ? mix(pc, th.bg, 0.6) : pc));
        dc.DrawRoundedRectangle(wxRect(std::min(x0, bar_x + bar_w - 1), r.y + (r.height - bar_h) / 2, w, bar_h),
                                w > 4 ? FromDIP(2) : 0);
        dc.SetFont(base);
        dc.SetTextForeground(p.ms > 0 ? fg : dim);
        draw_text_right(dc, U(fmt::ms(p.ms)), value_r, r);
      }
      double pv = 0;
      if (!is_sub && has_prev_ && prev_of(p.name, &pv)) {
        dc.SetFont(small);
        delta_text(p.ms - pv, r, std::max(p.ms, pv));
      }
      std::string tip = p.name + ": " + (p.reused ? std::string("reused (0 ms)") : fmt::ms(p.ms)) + "\n" + p.tip;
      if (!p.sub.empty()) tip += expand_sub_ ? "\nClick to hide the hops." : "\nClick to show each hop as its own row.";
      tips_.push_back({r, tip});
    }
  }

  // Footer: total, client overhead, server wait.
  const wxRect fr(0, rows_bottom + FromDIP(4), cr.width, footer_h());
  dc.SetPen(wxPen(th.grid));
  dc.DrawLine(pad, fr.y, cr.width - pad, fr.y);
  x = pad;
  dc.SetFont(bold);
  dc.SetTextForeground(fg);
  wxString tot = "Total " + U(fmt::ms(cur_.total));
  if (cur_.after > 0) tot += U(" + post-script " + fmt::ms(cur_.after));
  const int tw = draw_text(dc, tot, x, fr);
  tips_.push_back({wxRect(x, fr.y, tw, fr.height), cur_.total_tip});
  x += tw + FromDIP(16);
  // total = server + network + client, each with a swatch in its phase colour family.
  auto part = [&](const wxString& label, double v, const std::string& tip, const wxColour& c) {
    const int sw = FromDIP(8);
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(stale_ ? mix(c, th.bg, 0.6) : c));
    dc.DrawRoundedRectangle(wxRect(x, fr.y + (fr.height - sw) / 2, sw, sw), FromDIP(2));
    dc.SetFont(base);
    dc.SetTextForeground(fg);
    const int w = draw_text(dc, label + " " + U(fmt::ms(v)), x + sw + FromDIP(5), fr) + sw + FromDIP(5);
    tips_.push_back({wxRect(x, fr.y, w, fr.height), tip});
    x += w + FromDIP(16);
  };
  part(U(cur_.server_label), cur_.server_wait, cur_.server_tip, th.dark ? wxColour(80, 200, 120) : wxColour(30, 160, 80));
  part("network", cur_.network, cur_.network_tip, th.dark ? wxColour(70, 130, 240) : wxColour(40, 100, 220));
  part("client", cur_.client, cur_.client_tip, th.dark ? wxColour(130, 130, 140) : wxColour(150, 150, 160));
  if (has_prev_) {
    dc.SetFont(bold);
    delta_text(cur_.total - prev_.total, fr, std::max(cur_.total, prev_.total));
  }
  hover_ = -2;
}

void Waterfall::on_motion(const wxPoint& p) {
  SetCursor(on_conn_ && !pill_.IsEmpty() && pill_.Contains(p) ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
  // Later entries are more specific (footer pieces, bars), search from the back.
  for (size_t i = tips_.size(); i-- > 0;)
    if (tips_[i].first.Contains(p)) return update_tip(this, hover_, static_cast<int>(i), tips_[i].second);
  update_tip(this, hover_, -1, {});
}

// ------------------------------------------------------------------ tab strip

TabStrip::TabStrip(wxWindow* parent, std::function<void(int)> on_select)
    : wxWindow(parent, wxID_ANY), on_select_(std::move(on_select)) {
  SetBackgroundStyle(wxBG_STYLE_PAINT);
  SetBackgroundColour(Theme::get().bg);
  Bind(wxEVT_PAINT, [this](wxPaintEvent&) { on_paint(); });
  Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
    for (size_t i = 0; i < tabs_.size(); ++i)
      if (tabs_[i].shown && tabs_[i].rect.Contains(e.GetPosition())) {
        select(static_cast<int>(i));
        if (on_select_) on_select_(static_cast<int>(i));
        return;
      }
  });
  Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { Refresh(false); e.Skip(); });
  SetCursor(wxCursor(wxCURSOR_HAND));
}

wxSize TabStrip::DoGetBestClientSize() const { return wxSize(FromDIP(200), FromDIP(30)); }

int TabStrip::add(const std::string& label) {
  tabs_.push_back({label, {}, true, {}, Tone::Plain});
  Refresh(false);
  return static_cast<int>(tabs_.size()) - 1;
}

void TabStrip::set_label(int i, const std::string& label, const std::string& count) {
  Tab& t = tabs_[static_cast<size_t>(i)];
  if (t.label == label && t.count == count) return;
  t.label = label;
  t.count = count;
  Refresh(false);
}

void TabStrip::set_count_tone(int i, Tone t) {
  Tab& tab = tabs_[static_cast<size_t>(i)];
  if (tab.tone == t) return;
  tab.tone = t;
  Refresh(false);
}

void TabStrip::show_tab(int i, bool show) {
  if (tabs_[static_cast<size_t>(i)].shown == show) return;
  tabs_[static_cast<size_t>(i)].shown = show;
  Refresh(false);
}

void TabStrip::select(int i) {
  sel_ = i;
  Refresh(false);
}

void TabStrip::set_note(const std::string& note) {
  if (note_ == note) return;
  note_ = note;
  Refresh(false);
}

void TabStrip::on_paint() {
  wxAutoBufferedPaintDC dc(this);
  const Theme& th = Theme::get();
  const wxRect cr = GetClientRect();
  dc.SetBackground(wxBrush(th.bg));
  dc.Clear();
  dc.SetPen(wxPen(th.grid));
  dc.DrawLine(0, cr.height - 1, cr.width, cr.height - 1);
  const wxFont base = GetFont();
  const wxFont small = base.Smaller();
  int x = FromDIP(10);
  for (size_t i = 0; i < tabs_.size(); ++i) {
    Tab& t = tabs_[i];
    if (!t.shown) {
      t.rect = wxRect();
      continue;
    }
    const bool sel = static_cast<int>(i) == sel_;
    dc.SetFont(sel ? base.Bold() : base);
    dc.SetTextForeground(sel ? th.fg : th.dim);
    const wxString l = U(t.label);
    const wxSize e = dc.GetTextExtent(l);
    const int x0 = x;
    dc.DrawText(l, x, (cr.height - e.y) / 2);
    x += e.x;
    if (!t.count.empty() && t.tone != Tone::Plain) {
      // A result pill: white text on the tone colour.
      dc.SetFont(small.Bold());
      const wxString c = U(t.count);
      const wxSize ce = dc.GetTextExtent(c);
      x += FromDIP(6);
      const int ph = ce.y + FromDIP(2);
      const wxRect pill(x, (cr.height - ph) / 2, ce.x + FromDIP(10), ph);
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.SetBrush(wxBrush(tone_colour(t.tone)));
      dc.DrawRoundedRectangle(pill, ph / 2.0);
      dc.SetTextForeground(*wxWHITE);
      dc.DrawText(c, pill.x + FromDIP(5), pill.y + (ph - ce.y) / 2);
      x += pill.width;
    } else if (!t.count.empty()) {
      dc.SetFont(small);
      dc.SetTextForeground(sel ? th.ok : th.dim);
      const wxString c = U(t.count);
      const wxSize ce = dc.GetTextExtent(c);
      x += FromDIP(4);
      dc.DrawText(c, x, (cr.height - e.y) / 2 + (e.y - ce.y) / 2);
      x += ce.x;
    }
    t.rect = wxRect(x0 - FromDIP(6), 0, x - x0 + FromDIP(12), cr.height);
    if (sel) {
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.SetBrush(wxBrush(th.accent));
      dc.DrawRectangle(x0, cr.height - FromDIP(2), x - x0, FromDIP(2));
    }
    x += FromDIP(18);
  }
  if (!note_.empty()) {
    dc.SetFont(small);
    dc.SetTextForeground(th.dim);
    const wxString n = U(note_);
    const wxSize e = dc.GetTextExtent(n);
    if (cr.width - FromDIP(10) - e.x > x) dc.DrawText(n, cr.width - FromDIP(10) - e.x, (cr.height - e.y) / 2);
  }
}

// ------------------------------------------------------------------ chip bar

ChipBar::ChipBar(wxWindow* parent, std::function<void(int)> on_click)
    : wxWindow(parent, wxID_ANY), on_click_(std::move(on_click)) {
  SetBackgroundStyle(wxBG_STYLE_PAINT);
  SetBackgroundColour(Theme::get().bg);
  Bind(wxEVT_PAINT, [this](wxPaintEvent&) { on_paint(); });
  Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
    int h = -1;
    for (size_t i = 0; i < chips_.size(); ++i)
      if (chips_[i].visible && !chips_[i].folded && chips_[i].rect.Contains(e.GetPosition())) h = static_cast<int>(i);
    if (h < 0 && more_rect_.Contains(e.GetPosition())) {
      SetCursor(wxCursor(wxCURSOR_HAND));
      return update_tip(this, tip_, 10001, "More actions");
    }
    if (h != hover_) {
      hover_ = h;
      Refresh(false);
      SetCursor(h >= 0 && chips_[static_cast<size_t>(h)].enabled ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
    }
    if (h < 0 && note_cut_ && note_rect_.Contains(e.GetPosition())) return update_tip(this, tip_, 10000, note_);
    update_tip(this, tip_, h, h >= 0 ? chips_[static_cast<size_t>(h)].tip : std::string());
  });
  Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
    hover_ = -1;
    update_tip(this, tip_, -1, {});
    Refresh(false);
  });
  Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
    if (more_rect_.Contains(e.GetPosition())) return more_menu();
    for (const Chip& c : chips_)
      if (c.visible && c.enabled && !c.folded && c.rect.Contains(e.GetPosition())) {
        if (on_click_) on_click_(c.id);
        return;
      }
  });
  Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { Refresh(false); e.Skip(); });
}

void ChipBar::more_menu() {
  wxMenu menu;
  for (const Chip& c : chips_) {
    if (!c.folded || !c.visible) continue;
    wxMenuItem* it = menu.AppendCheckItem(c.id + 1000, U(c.label), U(c.tip));
    it->Check(c.on);
    it->Enable(c.enabled);
  }
  if (!menu.GetMenuItemCount()) return;
  const int id = GetPopupMenuSelectionFromUser(menu, more_rect_.GetBottomLeft());
  if (id >= 1000 && on_click_) on_click_(id - 1000);
}

wxSize ChipBar::DoGetBestClientSize() const { return wxSize(FromDIP(200), FromDIP(30)); }

ChipBar::Chip* ChipBar::find(int id) {
  for (Chip& c : chips_)
    if (c.id == id) return &c;
  return nullptr;
}

void ChipBar::add(int id, const std::string& label, const std::string& tip, int group, bool right) {
  chips_.push_back({id, label, tip, group, right});
  Refresh(false);
}

void ChipBar::set_on(int id, bool on) {
  Chip* c = find(id);
  if (!c) return;
  if (on && c->group >= 0)
    for (Chip& o : chips_)
      if (o.group == c->group) o.on = false;
  c->on = on;
  Refresh(false);
}

void ChipBar::set_enabled(int id, bool en) {
  if (Chip* c = find(id); c && c->enabled != en) {
    c->enabled = en;
    Refresh(false);
  }
}

void ChipBar::set_visible(int id, bool vis) {
  if (Chip* c = find(id); c && c->visible != vis) {
    c->visible = vis;
    Refresh(false);
  }
}

void ChipBar::set_label(int id, const std::string& label) {
  if (Chip* c = find(id); c && c->label != label) {
    c->label = label;
    Refresh(false);
  }
}

void ChipBar::set_note(const std::string& note, Tone tone) {
  if (note_ == note && note_tone_ == tone) return;
  note_ = note;
  note_tone_ = tone;
  Refresh(false);
}

void ChipBar::on_paint() {
  wxAutoBufferedPaintDC dc(this);
  const Theme& th = Theme::get();
  const wxRect cr = GetClientRect();
  dc.SetBackground(wxBrush(th.bg));
  dc.Clear();
  if (bottom_rule_) {
    dc.SetPen(wxPen(th.grid));
    dc.DrawLine(0, cr.height - 1, cr.width, cr.height - 1);
  }
  const wxFont base = GetFont();
  dc.SetFont(base);
  const int ch = cr.height - FromDIP(8);
  // Fitting: full padding, then tighter chips, then right chips (from the last) folded into a
  // "more" chip, until the left chips, the note's first part (the size) and the right chips fit
  // side by side; chips never overlap.
  static const wxString more_label = wxString::FromUTF8("\xE2\x8B\xAF");  // midline ellipsis
  static const wxString note_sep = wxString::FromUTF8(" \xC2\xB7 ");
  int note_min = 0;
  if (!note_.empty()) {
    dc.SetFont(base.Smaller());
    wxString first = U(note_);
    if (const size_t cut = first.find(note_sep); cut != wxString::npos) first.Truncate(cut);
    note_min = dc.GetTextExtent(first).x + FromDIP(32);
    dc.SetFont(base);
  }
  auto need = [&](int pad, size_t keep) {
    int w = FromDIP(16), last = -2;
    size_t seen = 0;
    for (const Chip& c : chips_) {
      if (!c.visible) continue;
      if (c.right) {
        if (seen++ >= keep) continue;
        w += dc.GetTextExtent(U(c.label)).x + 2 * pad + FromDIP(2);
        continue;
      }
      if (last != -2 && (c.group != last || c.group < 0)) w += FromDIP(6);
      w += dc.GetTextExtent(U(c.label)).x + 2 * pad;
      last = c.group;
    }
    if (seen > keep) w += dc.GetTextExtent(more_label).x + 2 * pad + FromDIP(2);
    return w + note_min;
  };
  size_t n_right = 0;
  for (const Chip& c : chips_) n_right += c.right && c.visible;
  int padx = FromDIP(8);
  size_t keep = n_right;
  if (need(padx, keep) > cr.width) padx = FromDIP(5);
  while (keep > 0 && need(padx, keep) > cr.width) --keep;
  {
    size_t i = 0;
    for (Chip& c : chips_) {
      c.folded = false;
      if (c.right && c.visible) c.folded = i++ >= keep;
      if (c.folded) c.rect = wxRect();
    }
  }
  auto layout = [&](Chip& c, int x) {
    const wxSize e = dc.GetTextExtent(U(c.label));
    c.rect = wxRect(x, (cr.height - ch) / 2, e.x + 2 * padx, ch);
    return c.rect.width;
  };
  // Left chips (groups drawn as one segmented box).
  int x = FromDIP(8);
  int last_group = -2;
  for (Chip& c : chips_) {
    if (c.right || !c.visible) continue;
    if (last_group != -2 && (c.group != last_group || c.group < 0)) x += FromDIP(6);
    x += layout(c, x);
    last_group = c.group;
  }
  const int left_end = x;
  int rx = cr.width - FromDIP(8);
  more_rect_ = wxRect();
  if (keep < n_right) {
    const wxSize e = dc.GetTextExtent(more_label);
    rx -= e.x + 2 * padx;
    more_rect_ = wxRect(rx, (cr.height - ch) / 2, e.x + 2 * padx, ch);
    rx -= FromDIP(2);
  }
  for (size_t i = chips_.size(); i-- > 0;) {
    Chip& c = chips_[i];
    if (!c.right || !c.visible || c.folded) continue;
    const wxSize e = dc.GetTextExtent(U(c.label));
    rx -= e.x + 2 * padx;
    c.rect = wxRect(rx, (cr.height - ch) / 2, e.x + 2 * padx, ch);
    rx -= FromDIP(2);
  }
  if (!more_rect_.IsEmpty()) {
    dc.SetTextForeground(th.dim);
    const wxSize e = dc.GetTextExtent(more_label);
    dc.DrawText(more_label, more_rect_.x + (more_rect_.width - e.x) / 2, more_rect_.y + (more_rect_.height - e.y) / 2);
  }
  // Segmented group backgrounds.
  for (size_t i = 0; i < chips_.size(); ++i) {
    Chip& c = chips_[i];
    if (!c.visible || c.folded) continue;
    const bool hover = static_cast<int>(i) == hover_ && c.enabled;
    // A disabled chip never shows its "on" fill: faint text on the fill read as an empty pill.
    if (c.on && c.enabled) {
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.SetBrush(wxBrush(mix(th.bg, th.fg, th.dark ? 0.16 : 0.10)));
      dc.DrawRoundedRectangle(c.rect, FromDIP(3));
    } else if (hover) {
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.SetBrush(wxBrush(mix(th.bg, th.fg, th.dark ? 0.08 : 0.05)));
      dc.DrawRoundedRectangle(c.rect, FromDIP(3));
    }
    dc.SetTextForeground(!c.enabled ? th.faint : (c.on ? th.fg : th.dim));
    if (hover && !c.on) dc.SetTextForeground(th.fg);
    const wxSize e = dc.GetTextExtent(U(c.label));
    dc.DrawText(U(c.label), c.rect.x + (c.rect.width - e.x) / 2, c.rect.y + (c.rect.height - e.y) / 2);
  }
  if (!note_.empty()) {
    const wxFont small = base.Smaller();
    dc.SetFont(small);
    dc.SetTextForeground(tone_colour(note_tone_));
    wxString n = U(note_);
    const int avail = rx - left_end - FromDIP(20);
    note_rect_ = wxRect();
    note_cut_ = false;
    if (avail > FromDIP(30)) {
      // Too wide: drop the " \xB7 " parts from the end (least important last) before eliding, so
      // what is shown is always whole; the full note is the tooltip.
      const wxString& sep = note_sep;
      while (dc.GetTextExtent(n).x > avail) {
        const size_t cut = n.rfind(sep);
        if (cut == wxString::npos || cut == 0) break;
        n.Truncate(cut);
        note_cut_ = true;
      }
      if (dc.GetTextExtent(n).x > avail) {
        n = wxControl::Ellipsize(n, dc, wxELLIPSIZE_END, avail);
        note_cut_ = true;
      }
      const wxSize e = dc.GetTextExtent(n);
      note_rect_ = wxRect(left_end + FromDIP(12), 0, e.x, cr.height);
      dc.DrawText(n, left_end + FromDIP(12), (cr.height - e.y) / 2);
    }
  }
}

}  // namespace watt::app::rv
