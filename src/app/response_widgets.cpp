#include "app/response_widgets.h"

#include <wx/config.h>
#include <wx/dcbuffer.h>
#include <wx/control.h>
#include <wx/settings.h>

#include <algorithm>
#include <cmath>

#include "app/response_format.h"

namespace watt::app::rv {
namespace {

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
        UnsetToolTip();
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
      UnsetToolTip();
      hover_ = -2;
      on_conn_(wxRect(ClientToScreen(pill_.GetTopLeft()), pill_.GetSize()));
      return;
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

void Waterfall::set_waiting(Clock::time_point since) {
  waiting_ = true;
  wait_since_ = since;
  if (!wait_timer_.IsRunning()) wait_timer_.Start(66);
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
  dc.SetTextForeground(th.accent);
  char buf[96];
  if (el < 1000) std::snprintf(buf, sizeof buf, "waiting for the response \xC2\xB7 %.0f ms", el);
  else std::snprintf(buf, sizeof buf, "waiting for the response \xC2\xB7 %.2f s", el / 1000.0);
  const wxString w = wxString::FromUTF8(buf);
  dc.DrawText(w, pad + te.x + FromDIP(12), (header_h() - dc.GetCharHeight()) / 2);
  if (collapsed_pref()) return;
  // The phases to come, faint, each with an empty track; one indeterminate sweep runs over the
  // tracks (the engine reports the phases with the response, not while it runs).
  static const char* const kHttp[] = {"Prepare", "Queue", "DNS", "Socket", "Connect", "TLS", "Send", "Wait (TTFB)",
                                      "Download", "Process"};
  static const char* const kGrpc[] = {"Queue", "DNS", "Connect", "TLS", "Send", "Wait (TTFB)", "Download", "Other"};
  const bool grpc = reserve_rows_ == 8;
  const int n = grpc ? 8 : std::min(reserve_rows_, 10);
  const int label_w = FromDIP(84);
  const int bar_x = pad + label_w;
  const int bar_w = std::max(FromDIP(40), cr.width - bar_x - FromDIP(76) - pad - FromDIP(8));
  const int rh = row_h();
  const int sweep = std::max(FromDIP(30), bar_w / 6);
  const int phase = static_cast<int>(std::fmod(el / 1400.0, 1.0) * (bar_w + sweep)) - sweep;
  for (int i = 0; i < n; ++i) {
    const int y = header_h() + i * rh;
    dc.SetTextForeground(th.faint);
    dc.DrawText(wxString::FromUTF8(grpc ? kGrpc[i] : kHttp[i]), pad, y + (rh - dc.GetCharHeight()) / 2);
    const wxRect track(bar_x, y + rh / 2 - FromDIP(2), bar_w, FromDIP(4));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(th.grid));
    dc.DrawRectangle(track);
    const int sx = std::max(bar_x, bar_x + phase);
    const int ex = std::min(bar_x + bar_w, bar_x + phase + sweep);
    if (ex > sx) {
      dc.SetBrush(wxBrush(th.accent));
      dc.DrawRectangle(wxRect(sx, track.y, ex - sx, track.height));
    }
  }
  dc.SetTextForeground(th.dim);
  dc.SetFont(GetFont().Smaller());
  dc.DrawText(wxString::FromUTF8("phase breakdown when the response arrives"), pad,
              header_h() + n * rh + (footer_h() - dc.GetCharHeight()) / 2);
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

wxSize Waterfall::DoGetBestClientSize() const {
  laid_collapsed_ = collapsed_pref();
  if (!cur_.valid && !reserved_) return wxSize(FromDIP(200), 0);
  const int rows = collapsed_pref() ? 1 : cur_.valid ? static_cast<int>(cur_.phases.size()) : reserve_rows_;
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
  const int rows_before = cur_.valid ? static_cast<int>(cur_.phases.size()) : reserved_ ? reserve_rows_ : -1;
  const bool resize = rows_before != static_cast<int>(cur.phases.size());
  reserved_ = true;
  reserve_rows_ = static_cast<int>(cur.phases.size());
  cur_ = cur;
  waiting_ = false;
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
    if (reserved_ && waiting_) return paint_waiting(dc);
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
  const double total = std::max(cur_.total, 1e-9);
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
    dc.SetTextForeground(stale_ ? dim : c);
    dc.DrawText(note, pill.x + FromDIP(5), pill.y + FromDIP(2));
    pill_ = pill;
    std::string tip = cur_.reused ? "The request went out on a connection that was already open: no DNS, "
                                    "TCP or TLS handshake was needed."
                                  : "A new connection was opened for this request (DNS, TCP and TLS as "
                                    "applicable are included in the total).";
    if (on_conn_) tip += "\nClick for the connection details (addresses, protocol, TLS, certificate).";
    tips_.push_back({pill, tip});
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
  const int nrows = collapsed_pref() ? 1 : static_cast<int>(cur_.phases.size());
  const int rows_bottom = rows_top + nrows * row_h();
  dc.SetPen(wxPen(th.grid));
  for (int q = 0; q <= 4; ++q) {
    const int gx = bar_x + bar_w * q / 4;
    dc.DrawLine(gx, rows_top, gx, rows_bottom);
  }

  auto delta_text = [&](double d, const wxRect& r, double scale) {
    if (!has_prev_) return;
    const bool noise = std::fabs(d) < std::max(0.05, 0.05 * scale);  // jitter, not a change
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
    double off = 0;
    for (size_t i = 0; i < cur_.phases.size(); ++i) {
      const Phase& p = cur_.phases[i];
      const wxRect r(0, rows_top + static_cast<int>(i) * row_h(), cr.width, row_h());
      dc.SetFont(base);
      dc.SetTextForeground(p.failed && !stale_ ? th.err : p.ms > 0 ? fg : dim);
      draw_text(dc, U(p.name), pad, r);
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
      if (has_prev_ && prev_of(p.name, &pv)) {
        dc.SetFont(small);
        delta_text(p.ms - pv, r, std::max(p.ms, pv));
      }
      tips_.push_back({r, p.name + ": " + (p.reused ? std::string("reused (0 ms)") : fmt::ms(p.ms)) + "\n" + p.tip});
      off += p.ms;
    }
  }

  // Footer: total, client overhead, server wait.
  const wxRect fr(0, rows_bottom + FromDIP(4), cr.width, footer_h());
  dc.SetPen(wxPen(th.grid));
  dc.DrawLine(pad, fr.y, cr.width - pad, fr.y);
  x = pad;
  dc.SetFont(bold);
  dc.SetTextForeground(fg);
  const wxString tot = "Total " + U(fmt::ms(cur_.total));
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
      if (chips_[i].visible && chips_[i].rect.Contains(e.GetPosition())) h = static_cast<int>(i);
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
    for (const Chip& c : chips_)
      if (c.visible && c.enabled && c.rect.Contains(e.GetPosition())) {
        if (on_click_) on_click_(c.id);
        return;
      }
  });
  Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { Refresh(false); e.Skip(); });
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
  const int padx = FromDIP(8);
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
  for (size_t i = chips_.size(); i-- > 0;) {
    Chip& c = chips_[i];
    if (!c.right || !c.visible) continue;
    const wxSize e = dc.GetTextExtent(U(c.label));
    rx -= e.x + 2 * padx;
    c.rect = wxRect(rx, (cr.height - ch) / 2, e.x + 2 * padx, ch);
    rx -= FromDIP(2);
  }
  // Segmented group backgrounds.
  for (size_t i = 0; i < chips_.size(); ++i) {
    Chip& c = chips_[i];
    if (!c.visible) continue;
    const bool hover = static_cast<int>(i) == hover_ && c.enabled;
    if (c.on) {
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
      static const wxString sep = wxString::FromUTF8(" \xC2\xB7 ");
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
