#include "app/response_sse_view.h"

#include <wx/clipbrd.h>
#include <wx/dcbuffer.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/stc/stc.h>
#include <wx/textctrl.h>
#include <wx/wupdlock.h>

#include <algorithm>

#include "app/response_format.h"
#include "app/response_widgets.h"

namespace watt::app::rv {

namespace {

wxString U(std::string_view s) { return wxString::FromUTF8(s.data(), s.size()); }

enum { kChipEvents = 1, kChipRaw = 2, kChipCopy = 3 };

// Pretty JSON of one event's data is formatted on the UI thread: only up to this size.
constexpr size_t kPrettyMax = 512u << 10;
// The detail pane shows at most this much of one event's data.
constexpr size_t kDetailMax = 4u << 20;

std::string at_text(double at_ms) { return at_ms < 0 ? std::string("\xE2\x80\x94") : "+" + fmt::ms(at_ms); }

}  // namespace

// ------------------------------------------------------------------ list

class SseList : public wxWindow {
 public:
  explicit SseList(SsePage* page)
      : wxWindow(page->split_, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxWANTS_CHARS | wxBORDER_NONE),
        page_(page) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    const Theme& th = Theme::get();
    bg_ = th.dark ? wxColour(30, 30, 30) : *wxWHITE;
    SetBackgroundColour(bg_);
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      sync(follow_);
    });
    Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& e) {
      const int lines = e.GetWheelRotation() / std::max(1, e.GetWheelDelta()) * std::max(1, e.GetLinesPerAction());
      scroll_to(top_ - lines);
    });
    auto on_scroll = [this](wxScrollWinEvent& e) {
      const wxEventType t = e.GetEventType();
      long pos = top_;
      const long page = std::max(1, visible_rows() - 1);
      if (t == wxEVT_SCROLLWIN_LINEUP) pos -= 1;
      else if (t == wxEVT_SCROLLWIN_LINEDOWN) pos += 1;
      else if (t == wxEVT_SCROLLWIN_PAGEUP) pos -= page;
      else if (t == wxEVT_SCROLLWIN_PAGEDOWN) pos += page;
      else if (t == wxEVT_SCROLLWIN_TOP) pos = 0;
      else if (t == wxEVT_SCROLLWIN_BOTTOM) pos = count();
      else pos = e.GetPosition();
      scroll_to(pos);
    };
    for (wxEventType t : {wxEVT_SCROLLWIN_TOP, wxEVT_SCROLLWIN_BOTTOM, wxEVT_SCROLLWIN_LINEUP, wxEVT_SCROLLWIN_LINEDOWN,
                          wxEVT_SCROLLWIN_PAGEUP, wxEVT_SCROLLWIN_PAGEDOWN, wxEVT_SCROLLWIN_THUMBTRACK,
                          wxEVT_SCROLLWIN_THUMBRELEASE})
      Bind(wxEventTypeTag<wxScrollWinEvent>(t), on_scroll);
    Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
      SetFocus();
      if (const long r = row_at(e.GetPosition().y); r >= 0) pick(r);
    });
    Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
      const long r = row_at(e.GetPosition().y);
      if (r == tip_row_) return;
      tip_row_ = r;
      if (r < 0) return UnsetToolTip();
      const size_t ei = page_->event_of(r);
      const SseEvent& ev = page_->events_[ei];
      std::string tip = "#" + std::to_string(ei + 1) + "  " + at_text(ev.at_ms) + "  " +
                        (ev.type.empty() ? std::string("message") : ev.type);
      if (!ev.id.empty()) tip += "  id " + ev.id;
      if (ev.retry >= 0) tip += "  retry " + std::to_string(ev.retry) + " ms";
      tip += "  " + fmt::human_bytes(ev.data.size()) + "\n" + one_line_preview(ev.data, 400) +
             "\n\nClick: the full data below. Ctrl+C copies it.";
      SetToolTip(U(tip));
    });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) {
      e.Skip();
      tip_row_ = -2;
    });
    Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
      const int k = e.GetKeyCode();
      const long n = count();
      if (e.GetModifiers() == wxMOD_CONTROL && k == 'C') return page_->copy_data();
      if (e.GetModifiers() != 0 || n == 0) return e.Skip();
      const long page = std::max(1, visible_rows() - 1);
      const long s = sel_ < 0 ? n - 1 : sel_;
      switch (k) {
        case WXK_UP: return pick(std::max(0L, s - 1));
        case WXK_DOWN: return pick(std::min(n - 1, s + 1));
        case WXK_PAGEUP: return pick(std::max(0L, s - page));
        case WXK_PAGEDOWN: return pick(std::min(n - 1, s + page));
        case WXK_HOME: return pick(0);
        case WXK_END:
          // Back to following the stream.
          page_->user_picked_ = false;
          follow_ = true;
          sel_ = -1;
          sync(true);
          page_->show_detail(n - 1);
          return;
        case WXK_ESCAPE: return e.Skip();
        default: e.Skip();
      }
    });
  }
  bool at_end() const {
    const long n = count();
    return n == 0 || top_ + visible_rows() >= n;
  }
  void sync(bool follow) {
    const long n = count();
    if (sel_ >= n) sel_ = -1;
    follow_ = follow;
    if (follow && n > 0) top_ = std::max(0L, n - visible_rows());
    top_ = std::clamp(top_, 0L, std::max(0L, n - visible_rows()));
    SetScrollbar(wxVERTICAL, static_cast<int>(top_), visible_rows(), static_cast<int>(n));
    Refresh(false);
  }
  void reset() {
    top_ = 0;
    sel_ = -1;
    follow_ = true;
    sync(true);
  }
  long selection() const { return sel_; }
  void select_row(long r) { pick(r); }

 private:
  long count() const { return page_->rows(); }
  int row_h() const { return GetCharHeight() + FromDIP(5); }
  int head_h() const { return row_h() + FromDIP(2); }
  int visible_rows() const { return std::max(1, (GetClientSize().y - head_h()) / row_h()); }
  long row_at(int y) const {
    if (y < head_h()) return -1;
    const long r = top_ + (y - head_h()) / row_h();
    return r < count() ? r : -1;
  }
  void pick(long r) {
    sel_ = r;
    page_->user_picked_ = true;
    follow_ = false;
    if (sel_ < top_) scroll_to(sel_);
    else if (sel_ >= top_ + visible_rows()) scroll_to(sel_ - visible_rows() + 1);
    else Refresh(false);
    page_->show_detail(r);
  }
  void scroll_to(long pos) {
    pos = std::clamp(pos, 0L, std::max(0L, count() - visible_rows()));
    follow_ = pos + visible_rows() >= count();
    if (pos == top_) return Refresh(false);
    top_ = pos;
    SetScrollbar(wxVERTICAL, static_cast<int>(top_), visible_rows(), static_cast<int>(count()));
    Refresh(false);
  }
  void paint() {
    wxAutoBufferedPaintDC dc(this);
    const Theme& th = Theme::get();
    const wxSize cs = GetClientSize();
    dc.SetBackground(wxBrush(bg_));
    dc.Clear();
    const wxColour head_bg = th.dark ? wxColour(40, 40, 44) : wxColour(244, 244, 247);
    const wxColour sel = th.dark ? wxColour(38, 79, 120) : wxColour(205, 228, 255);
    const int rh = row_h(), hh = head_h(), pad = FromDIP(6);
    const long n = count();
    const long last = std::min<long>(n, top_ + visible_rows() + 1);
    // #, Offset and Size right-aligned; Event and ID left-aligned; the data fills the rest.
    const int w[5] = {FromDIP(52), FromDIP(86), FromDIP(110), FromDIP(90), FromDIP(70)};
    const bool right[5] = {true, true, false, false, true};
    dc.SetFont(GetFont());
    const int ch = dc.GetCharHeight();
    // The row whose data is shown below: selected, or the newest one while following.
    const long shown = sel_ >= 0 ? sel_ : (page_->user_picked_ ? -1 : n - 1);
    if (shown >= top_ && shown < last) {
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.SetBrush(wxBrush(sel_ >= 0 ? sel : (th.dark ? wxColour(44, 52, 62) : wxColour(232, 240, 252))));
      dc.DrawRectangle(0, hh + static_cast<int>(shown - top_) * rh, cs.x, rh);
    }
    for (long i = top_; i < last; ++i) {
      const size_t ei = page_->event_of(i);
      const SseEvent& ev = page_->events_[ei];
      const int y = hh + static_cast<int>(i - top_) * rh + (rh - ch) / 2;
      const wxString cells[5] = {wxString::Format("%zu", ei + 1), U(at_text(ev.at_ms)),
                                 U(ev.type.empty() ? std::string("message") : ev.type), U(ev.id),
                                 U(fmt::human_bytes(ev.data.size()))};
      int x = 0;
      for (int c = 0; c < 5; ++c) {
        dc.SetTextForeground(c == 0 || (c == 2 && ev.type.empty()) || c == 3 ? th.dim : c == 2 ? th.accent : th.fg);
        dc.SetClippingRegion(x, hh, w[c] - FromDIP(2), std::max(0, cs.y - hh));
        if (right[c]) dc.DrawText(cells[c], x + w[c] - pad - dc.GetTextExtent(cells[c]).x, y);
        else dc.DrawText(cells[c], x + pad, y);
        dc.DestroyClippingRegion();
        x += w[c];
      }
      dc.SetTextForeground(th.fg);
      dc.SetClippingRegion(x, hh, std::max(0, cs.x - x), std::max(0, cs.y - hh));
      const std::string& p = page_->previews_[ei];
      const size_t fit = static_cast<size_t>(std::max(16, (cs.x - x) / FromDIP(4)));
      dc.DrawText(wxString::FromUTF8(p.data(), std::min(p.size(), fit)), x + pad, y);
      dc.DestroyClippingRegion();
    }
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(head_bg));
    dc.DrawRectangle(0, 0, cs.x, hh);
    dc.SetPen(wxPen(th.grid));
    dc.DrawLine(0, hh - 1, cs.x, hh - 1);
    dc.SetFont(GetFont().Bold());
    dc.SetTextForeground(th.fg);
    static const char* kHead[] = {"#", "Offset", "Event", "ID", "Size"};
    int x = 0;
    const int hy = (hh - dc.GetCharHeight()) / 2;
    for (int c = 0; c < 5; ++c) {
      if (right[c]) dc.DrawText(kHead[c], x + w[c] - pad - dc.GetTextExtent(kHead[c]).x, hy);
      else dc.DrawText(kHead[c], x + pad, hy);
      x += w[c];
      dc.DrawLine(x - 1, FromDIP(4), x - 1, hh - FromDIP(4));
    }
    dc.DrawText("Data", x + pad, hy);
    if (n == 0) {
      dc.SetFont(GetFont());
      dc.SetTextForeground(th.dim);
      dc.DrawText(page_->filter_.empty() || page_->events_.empty()
                      ? wxString("No events yet.")
                      : wxString::Format("No event matches the filter (%zu events in all).", page_->events_.size()),
                  pad, hh + pad);
    }
  }
  SsePage* page_;
  wxColour bg_;
  long top_ = 0;
  long sel_ = -1;
  long tip_row_ = -2;
  bool follow_ = true;
};

// ------------------------------------------------------------------ page

SsePage::SsePage(wxWindow* parent) : wxPanel(parent, wxID_ANY) { SetBackgroundColour(Theme::get().bg); }

void SsePage::ensure() {
  if (bar_) return;
  const Theme& th = Theme::get();
  wxWindowUpdateLocker lock(this);
  bar_ = new ChipBar(this, [this](int id) {
    if (id == kChipRaw && on_raw) on_raw();
    else if (id == kChipCopy) copy_data();
  });
  bar_->add(kChipEvents, "Events", "One row per server-sent event, live while the stream runs", 0);
  bar_->add(kChipRaw, "Raw", "The stream exactly as received (find, copy, save)", 0);
  bar_->set_on(kChipEvents, true);
  bar_->add(kChipCopy, "Copy data", "Copy the data of the event shown below (Ctrl+C in the list)", -1, true);
  bar_->set_note(caption_text_, caption_tone_);
  find_ = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(210), -1),
                         wxBORDER_SIMPLE | wxTE_PROCESS_ENTER);
  find_->SetHint("Filter events: name, id or text");
  find_->SetToolTip("Shows only the events whose name, id or data contains this text (case-insensitive).\n"
                    "event:<name> matches the event name exactly. Esc clears it.");
  find_->SetBackgroundColour(th.dark ? wxColour(37, 37, 38) : *wxWHITE);
  find_->SetForegroundColour(th.fg);
  find_timer_.SetOwner(this);
  Bind(wxEVT_TIMER, [this](wxTimerEvent&) { apply_filter(); }, find_timer_.GetId());
  // A short pause after typing: a 100k event stream is scanned once per pause, not per key.
  find_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { find_timer_.StartOnce(events_.size() > 5000 ? 150 : 40); });
  find_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) {
    find_timer_.Stop();
    apply_filter();
  });
  find_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
    if (e.GetKeyCode() == WXK_ESCAPE && !find_->IsEmpty()) {
      find_->Clear();
      find_timer_.Stop();
      apply_filter();
      return;
    }
    if (e.GetKeyCode() == WXK_DOWN && list_) return list_->SetFocus();
    e.Skip();
  });

  split_ = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                wxSP_LIVE_UPDATE | wxSP_NOBORDER | wxSP_THIN_SASH);
  split_->SetBackgroundColour(th.grid);
  split_->SetMinimumPaneSize(FromDIP(60));
  split_->SetSashGravity(0.55);
  list_ = new SseList(this);
  detail_ = new wxPanel(split_, wxID_ANY);
  detail_->SetBackgroundColour(th.bg);
  detail_head_ = new ChipBar(detail_, [](int) {});
  stc_ = new wxStyledTextCtrl(detail_, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
  const wxFont mono(wxFontInfo(9).Family(wxFONTFAMILY_TELETYPE).FaceName("Consolas"));
  stc_->StyleSetFont(wxSTC_STYLE_DEFAULT, mono);
  stc_->StyleSetBackground(wxSTC_STYLE_DEFAULT, th.bg);
  stc_->StyleSetForeground(wxSTC_STYLE_DEFAULT, th.fg);
  stc_->StyleClearAll();
  const bool dark = th.dark;
  auto set = [&](int s, wxColour d, wxColour l) { stc_->StyleSetForeground(s, dark ? d : l); };
  set(wxSTC_JSON_PROPERTYNAME, wxColour(156, 220, 254), wxColour(4, 81, 165));
  set(wxSTC_JSON_STRING, wxColour(206, 145, 120), wxColour(163, 21, 21));
  set(wxSTC_JSON_STRINGEOL, wxColour(206, 145, 120), wxColour(163, 21, 21));
  set(wxSTC_JSON_NUMBER, wxColour(181, 206, 168), wxColour(9, 134, 88));
  set(wxSTC_JSON_KEYWORD, wxColour(86, 156, 214), wxColour(0, 0, 255));
  set(wxSTC_JSON_OPERATOR, wxColour(150, 150, 150), wxColour(110, 110, 110));
  stc_->SetCaretForeground(th.fg);
  stc_->SetSelBackground(true, th.dark ? wxColour(38, 79, 120) : wxColour(173, 214, 255));
  for (int m = 0; m < 5; ++m) stc_->SetMarginWidth(m, 0);
  stc_->SetMarginLeft(FromDIP(8));
  stc_->SetUndoCollection(false);
  stc_->SetWrapMode(wxSTC_WRAP_WORD);
  stc_->SetWrapVisualFlags(wxSTC_WRAPVISUALFLAG_END);
  stc_->SetReadOnly(true);
  auto* ds = new wxBoxSizer(wxVERTICAL);
  ds->Add(detail_head_, 0, wxEXPAND);
  ds->Add(stc_, 1, wxEXPAND);
  detail_->SetSizer(ds);
  split_->SplitHorizontally(list_, detail_);
  auto* top = new wxBoxSizer(wxHORIZONTAL);
  top->Add(bar_, 1, wxEXPAND);
  top->Add(find_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(6));
  auto* s = new wxBoxSizer(wxVERTICAL);
  s->Add(top, 0, wxEXPAND);
  s->Add(split_, 1, wxEXPAND);
  SetSizer(s);
  Layout();
  // Half of the pane for the list once the page has its size.
  CallAfter([this] {
    if (split_ && split_->GetSize().y > FromDIP(160)) split_->SetSashPosition(split_->GetSize().y * 55 / 100);
  });
  list_->sync(true);
  detail_row_ = -2;
  show_detail(rows() - 1);
}

bool SsePage::matches(const SseEvent& ev, const std::string& f) {
  if (f.empty()) return true;
  auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
  auto contains = [&](std::string_view hay) {
    if (f.size() > hay.size()) return false;
    const auto it = std::search(hay.begin(), hay.end(), f.begin(), f.end(),
                                [&](char a, char b) { return lower(a) == b; });
    return it != hay.end();
  };
  const std::string_view type = ev.type.empty() ? std::string_view("message") : std::string_view(ev.type);
  if (f.rfind("event:", 0) == 0) {
    std::string_view want = std::string_view(f).substr(6);
    while (!want.empty() && want.front() == ' ') want.remove_prefix(1);
    if (want.size() != type.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
      if (lower(type[i]) != want[i]) return false;
    return true;
  }
  return contains(type) || contains(ev.id) || contains(ev.data);
}

void SsePage::apply_filter() {
  if (!find_) return;
  std::string f = find_->GetValue().utf8_string();
  for (char& c : f)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  while (!f.empty() && f.back() == ' ') f.pop_back();
  if (f == filter_) return;
  // The event shown below stays shown (and selected) when it still matches.
  const long sel = list_ ? list_->selection() : -1;
  const long shown_ev = sel >= 0 ? static_cast<long>(event_of(sel)) : -1;
  filter_ = f;
  view_.clear();
  if (!filter_.empty())
    for (size_t i = 0; i < events_.size(); ++i)
      if (matches(events_[i], filter_)) view_.push_back(i);
  find_->SetForegroundColour(!filter_.empty() && view_.empty() ? Theme::get().err : Theme::get().fg);
  find_->Refresh();
  user_picked_ = false;
  detail_row_ = -2;
  list_->reset();
  long keep = -1;
  if (shown_ev >= 0) {
    if (filter_.empty()) keep = shown_ev;
    else if (auto it = std::lower_bound(view_.begin(), view_.end(), static_cast<size_t>(shown_ev));
             it != view_.end() && *it == static_cast<size_t>(shown_ev))
      keep = static_cast<long>(it - view_.begin());
  }
  if (keep >= 0) list_->select_row(keep);
  else show_detail(rows() - 1);
}

void SsePage::reset() {
  events_.clear();
  previews_.clear();
  dropped_ = 0;
  user_picked_ = false;
  detail_row_ = -2;
  detail_size_ = 0;
  caption_text_.clear();
  view_.clear();
  if (!bar_) return;
  bar_->set_note({});
  list_->reset();
  show_detail(-1);
}

void SsePage::append(std::vector<SseEvent>&& evs) {
  if (evs.empty()) return;
  const bool follow = !list_ || list_->at_end();
  const size_t room = kMaxRows - std::min(kMaxRows, events_.size());
  const size_t take = std::min(room, evs.size());
  dropped_ += evs.size() - take;
  events_.reserve(events_.size() + take);
  previews_.reserve(previews_.size() + take);
  for (size_t i = 0; i < take; ++i) {
    previews_.push_back(one_line_preview(evs[i].data, 200));
    if (!filter_.empty() && matches(evs[i], filter_)) view_.push_back(events_.size());
    events_.push_back(std::move(evs[i]));
  }
  if (!list_) return;
  list_->sync(follow && !user_picked_);
  if (!user_picked_) show_detail(rows() - 1);
}

void SsePage::set_caption(const std::string& text, Tone tone) {
  caption_text_ = text;
  caption_tone_ = tone;
  if (bar_) bar_->set_note(text, tone);
}

void SsePage::show_detail(long row) {
  if (!stc_) return;
  if (row >= rows()) row = -1;
  if (row == detail_row_ && (row < 0 || events_[event_of(row)].data.size() == detail_size_)) return;
  detail_row_ = row;
  stc_->SetReadOnly(false);
  stc_->ClearAll();
  if (row < 0) {
    detail_size_ = 0;
    detail_head_->set_note(events_.empty()       ? "Events appear here as they arrive."
                           : rows() == 0 ? "No event matches the filter."
                                         : "Pick an event to see its data.",
                           Tone::Dim);
    stc_->SetLexer(wxSTC_LEX_NULL);
    stc_->SetReadOnly(true);
    return;
  }
  const size_t ei = event_of(row);
  const SseEvent& ev = events_[ei];
  detail_size_ = ev.data.size();
  std::string head = "#" + std::to_string(ei + 1) + " \xC2\xB7 " + (ev.type.empty() ? std::string("message") : ev.type);
  if (!ev.id.empty()) head += " \xC2\xB7 id " + ev.id;
  head += " \xC2\xB7 " + at_text(ev.at_ms);
  if (ei > 0 && ev.at_ms >= 0 && events_[ei - 1].at_ms >= 0)
    head += " (gap " + fmt::ms(ev.at_ms - events_[ei - 1].at_ms) + ")";
  if (ev.retry >= 0) head += " \xC2\xB7 retry " + std::to_string(ev.retry) + " ms";
  head += " \xC2\xB7 " + fmt::human_bytes(ev.data.size());
  std::string_view data = ev.data;
  std::string pretty;
  bool json = false;
  if (data.size() <= kPrettyMax) {
    size_t i = 0;
    while (i < data.size() && (data[i] == ' ' || data[i] == '\n' || data[i] == '\r' || data[i] == '\t')) ++i;
    if (i < data.size() && (data[i] == '{' || data[i] == '[') && fmt::pretty_json(data, pretty)) {
      json = true;
      data = pretty;
      head += " \xC2\xB7 JSON";
    }
  }
  if (data.size() > kDetailMax) {
    data = data.substr(0, kDetailMax);
    head += " \xC2\xB7 first " + fmt::human_bytes(kDetailMax) + " shown";
  }
  if (!user_picked_ && ei + 1 == events_.size()) head += " \xC2\xB7 newest";
  detail_head_->set_note(head, Tone::Plain);
  stc_->SetLexer(json ? wxSTC_LEX_JSON : wxSTC_LEX_NULL);
  stc_->AddTextRaw(data.data(), static_cast<int>(data.size()));
  if (json) stc_->Colourise(0, -1);
  stc_->SetReadOnly(true);
  stc_->GotoPos(0);
}

void SsePage::copy_data() {
  long r = list_ ? list_->selection() : -1;
  if (r < 0) r = rows() - 1;
  if (r < 0 || r >= rows()) return;
  if (wxTheClipboard->Open()) {
    wxTheClipboard->SetData(new wxTextDataObject(U(events_[event_of(r)].data)));
    wxTheClipboard->Close();
  }
}

}  // namespace watt::app::rv
