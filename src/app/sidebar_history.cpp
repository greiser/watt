#include "app/sidebar_history.h"

#include <wx/clipbrd.h>
#include <wx/dc.h>
#include <wx/control.h>
#include <wx/menu.h>

#include <ctime>

#include "app/sidebar_fuzzy.h"
#include "app/sidebar_style.h"

namespace watt::app::sidebar {

namespace {

// "GET https://host/x" -> "host/x"; "/pkg.Svc/M @ host:1" -> "pkg.Svc/M @ host:1".
std::string target_of(const HistoryEntry& e) {
  std::string s = e.summary;
  if (e.kind == RequestKind::Http) {
    const size_t sp = s.find(' ');
    if (sp != std::string::npos) s.erase(0, sp + 1);
    for (const char* scheme : {"https://", "http://"})
      if (s.rfind(scheme, 0) == 0) {
        s.erase(0, std::char_traits<char>::length(scheme));
        break;
      }
  } else if (!s.empty() && s[0] == '/') {
    s.erase(0, 1);
  }
  return s;
}

std::string method_of(const HistoryEntry& e) {
  if (e.kind != RequestKind::Http) return {};
  const size_t sp = e.summary.find(' ');
  return sp == std::string::npos ? e.summary : e.summary.substr(0, sp);
}

// Local time of an ISO UTC timestamp; false when it does not parse.
bool local_time(const std::string& iso, std::tm* out, std::time_t* t_out) {
  std::tm tm{};
  if (iso.size() < 19 || std::sscanf(iso.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                                     &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
    return false;
  tm.tm_year -= 1900;
  tm.tm_mon -= 1;
  const std::time_t t = _mkgmtime(&tm);
  if (t == -1) return false;
  localtime_s(out, &t);
  if (t_out) *t_out = t;
  return true;
}

// "just now", "4 min", else "14:05" (the day is in the group header).
std::string when_text(const std::string& iso) {
  std::tm lt{};
  std::time_t t = 0;
  if (!local_time(iso, &lt, &t)) return {};
  const long ago = static_cast<long>(std::time(nullptr) - t);
  char buf[32];
  if (ago >= 0 && ago < 60) return "now";
  if (ago >= 0 && ago < 3600) {
    std::snprintf(buf, sizeof buf, "%ld min", ago / 60);
    return buf;
  }
  std::strftime(buf, sizeof buf, "%H:%M", &lt);
  return buf;
}

// "Today", "Yesterday", "Mon, Nov 14" (with the year when it is not this year).
std::string day_text(const std::string& iso) {
  std::tm lt{};
  if (!local_time(iso, &lt, nullptr)) return "Earlier";
  const std::time_t n = std::time(nullptr);
  std::tm now{}, yt{};
  localtime_s(&now, &n);
  const std::time_t y = n - 86400;
  localtime_s(&yt, &y);
  if (lt.tm_year == now.tm_year && lt.tm_yday == now.tm_yday) return "Today";
  if (lt.tm_year == yt.tm_year && lt.tm_yday == yt.tm_yday) return "Yesterday";
  char buf[48];
  std::strftime(buf, sizeof buf, lt.tm_year == now.tm_year ? "%a, %b %d" : "%a, %b %d %Y", &lt);
  return buf;
}

}  // namespace

std::string history_when_label(const std::string& iso) {
  std::tm lt{};
  if (!local_time(iso, &lt, nullptr)) return iso;
  const std::time_t n = std::time(nullptr);
  std::tm now{};
  localtime_s(&now, &n);
  char buf[48];
  const bool today = lt.tm_year == now.tm_year && lt.tm_yday == now.tm_yday;
  std::strftime(buf, sizeof buf, today ? "%H:%M" : "%b %d %H:%M", &lt);
  return buf;
}

HistoryList::HistoryList(wxWindow* parent)
    : wxVListBox(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxLB_SINGLE) {
  SetBackgroundColour(window_colour());
  // A click opens the row as a preview (the host reuses the preview tab or activates the tab
  // that already shows it, so clicks never pile up tabs); a double click or Enter keeps it.
  Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
    const int hit = VirtualHitTest(e.GetPosition().y);
    click_opened_ = -1;
    if (!entry(hit)) return;  // a day header or empty space: nothing to select
    e.Skip();
    click_opened_ = hit;
    CallAfter([this, hit] { open_row(hit, false); });
  });
  Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent& e) {
    const int hit = VirtualHitTest(e.GetPosition().y);
    if (entry(hit)) open_row(hit, true);  // the host makes the preview permanent (no second tab)
    click_opened_ = -1;
    (void)e;
  });
  Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
    if (e.GetKeyCode() == WXK_RETURN || e.GetKeyCode() == WXK_NUMPAD_ENTER) {
      open_row(GetSelection(), true);
      return;
    }
    e.Skip();
  });
  // Day headers are not selectable (keyboard): step over them.
  Bind(wxEVT_LISTBOX, [this](wxCommandEvent& e) {
    e.Skip();
    const int s = GetSelection();
    if (s >= 0 && !entry(s)) {
      const int n = static_cast<int>(rows_.size());
      if (s + 1 < n) SetSelection(s + 1);
      else if (s > 0) SetSelection(s - 1);
    }
  });
  Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
    e.Skip();
    int hit = VirtualHitTest(e.GetPosition().y);
    if (!entry(hit)) hit = -1;
    if (hit == hover_) return;
    const int old = hover_;
    hover_ = hit;
    if (old >= 0) RefreshRow(old);
    if (hit >= 0) RefreshRow(hit);
    const HistoryEntry* h = entry(hit);
    if (!h) {
      UnsetToolTip();
      return;
    }
    const Label& l = label(static_cast<size_t>(rows_[static_cast<size_t>(hit)].entry));
    std::string tip = l.name.empty() ? h->summary : l.name + "\n" + h->summary;
    if (!l.target.empty() && h->summary.find(l.target) == std::string::npos) tip += "\n= " + l.target;
    tip += "\n" + status_text(h->kind, h->status) + "  \xC2\xB7  " + format_ms(h->total_ms) + "  \xC2\xB7  " +
           h->when_iso;
    SetToolTip(wxString::FromUTF8(tip));
  });
  Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) {
    e.Skip();
    const int old = hover_;
    hover_ = -1;
    if (old >= 0) RefreshRow(old);
  });
  Bind(wxEVT_CONTEXT_MENU, [this](wxContextMenuEvent& e) {
    wxPoint p = e.GetPosition();
    int row = GetSelection();
    if (p != wxDefaultPosition) row = VirtualHitTest(ScreenToClient(p).y);
    const HistoryEntry* h = entry(row);
    if (!h) return;
    SetSelection(row);
    const HistoryEntry copy = *h;
    const Label l = label(static_cast<size_t>(rows_[static_cast<size_t>(row)].entry));
    wxMenu m;
    m.Append(1, "Open as sent\tEnter");
    m.Append(4, "Open saved request")->Enable(!has_saved || has_saved(copy));
    m.AppendSeparator();
    m.Append(2, "Copy URL / target");
    m.Append(3, "Copy resolved URL / target");
    const int id = GetPopupMenuSelectionFromUser(m);
    if (id == 1 && on_open) on_open(copy, true, false);
    if (id == 4 && on_open) on_open(copy, false, true);
    std::string s;
    if (id == 2) {
      s = copy.summary;
      if (copy.kind == RequestKind::Http) {
        const size_t sp = s.find(' ');
        if (sp != std::string::npos) s.erase(0, sp + 1);
      }
    } else if (id == 3) {
      s = l.target;
    }
    if (!s.empty() && wxTheClipboard->Open()) {
      wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(s)));
      wxTheClipboard->Close();
    }
  });
}

void HistoryList::open_row(int row, bool permanent) {
  if (const HistoryEntry* h = entry(row); h && on_open) {
    const HistoryEntry copy = *h;
    on_open(copy, permanent, false);
  }
}

void HistoryList::set_entries(std::vector<HistoryEntry> newest_first) {
  all_ = std::move(newest_first);
  if (all_.size() > kMax) all_.resize(kMax);
  labels_.assign(all_.size(), Label{});
  refilter();
}

void HistoryList::prepend(const HistoryEntry& e) {
  // Entries added live have no offset: they keep their request so open_history() can use it.
  const HistoryEntry* sel = entry(GetSelection());
  const std::string sel_key = sel ? sel->when_iso + "\x01" + sel->summary : std::string();
  all_.insert(all_.begin(), e);
  labels_.insert(labels_.begin(), Label{});
  if (all_.size() > kMax) {
    all_.pop_back();
    labels_.pop_back();
  }
  refilter();
  if (sel_key.empty()) return;
  for (size_t r = 0; r < rows_.size(); ++r) {
    if (rows_[r].entry < 0) continue;
    const HistoryEntry& h = all_[static_cast<size_t>(rows_[r].entry)];
    if (h.when_iso + "\x01" + h.summary == sel_key) {
      SetSelection(static_cast<int>(r));
      break;
    }
  }
}

void HistoryList::set_filter(const std::string& q) {
  if (q == filter_) return;
  filter_ = q;
  refilter();
}

void HistoryList::relabel() {
  for (auto& l : labels_) l.ready = false;
  if (!filter_.empty()) refilter();
  else Refresh();
}

const HistoryList::Label& HistoryList::label(size_t i) const {
  Label& l = labels_[i];
  if (!l.ready) {
    l.ready = true;
    l.name.clear();
    l.target.clear();
    if (describe) describe(all_[i], &l.name, &l.target);
    if (l.target.empty()) l.target = target_of(all_[i]);
  }
  return l;
}

void HistoryList::refilter() {
  rows_.clear();
  rows_.reserve(all_.size() + 8);
  shown_count_ = 0;
  std::string day;
  for (size_t i = 0; i < all_.size(); ++i) {
    if (!filter_.empty()) {
      const Label& l = label(i);
      if (!words_match(filter_, all_[i].summary + " " + l.name + " " + l.target)) continue;
    }
    std::string d = day_text(all_[i].when_iso);
    if (rows_.empty() || d != day) {
      day = d;
      rows_.push_back(Row{-1, std::move(d)});
    }
    rows_.push_back(Row{static_cast<int>(i), {}});
    ++shown_count_;
  }
  hover_ = -1;
  click_opened_ = -1;
  SetItemCount(rows_.size());
  Refresh();
}

const HistoryEntry* HistoryList::entry(int row) const {
  if (row < 0 || static_cast<size_t>(row) >= rows_.size()) return nullptr;
  const int e = rows_[static_cast<size_t>(row)].entry;
  return e < 0 ? nullptr : &all_[static_cast<size_t>(e)];
}

wxCoord HistoryList::OnMeasureItem(size_t n) const {
  return FromDIP(n < rows_.size() && rows_[n].entry < 0 ? 24 : 38);
}

void HistoryList::OnDrawBackground(wxDC& dc, const wxRect& rect, size_t n) const {
  if (n >= rows_.size() || rows_[n].entry < 0) return;
  wxColour bg;
  if (IsSelected(n)) bg = highlight_colour();
  else if (static_cast<int>(n) == hover_) bg = mix(window_colour(), text_colour(), 0.06);
  else return;
  dc.SetBrush(wxBrush(bg));
  dc.SetPen(*wxTRANSPARENT_PEN);
  dc.DrawRectangle(rect);
}

void HistoryList::OnDrawItem(wxDC& dc, const wxRect& rect, size_t n) const {
  if (n >= rows_.size()) return;
  const int pad = FromDIP(10);
  const int right = rect.GetRight() - pad;
  if (rows_[n].entry < 0) {
    wxFont f = GetFont();
    f.SetFractionalPointSize(f.GetFractionalPointSize() * 0.85);
    f.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    dc.SetFont(f);
    dc.SetTextForeground(dim_colour());
    dc.DrawText(wxString::FromUTF8(rows_[n].header), rect.x + pad,
                rect.y + rect.height - dc.GetCharHeight() - FromDIP(3));
    dc.SetFont(GetFont());
    return;
  }
  const HistoryEntry& h = all_[static_cast<size_t>(rows_[n].entry)];
  const Label& l = label(static_cast<size_t>(rows_[n].entry));
  const wxFont base = GetFont();
  wxFont small = base;
  small.SetFractionalPointSize(small.GetFractionalPointSize() * 0.88);
  // Line 1: badge, name, status. Line 2 (dimmed, smaller): the target as sent, elapsed, time.
  dc.SetFont(base);
  const int h1 = dc.GetCharHeight();
  dc.SetFont(small);
  const int h2 = dc.GetCharHeight();
  const int gap = FromDIP(1);
  const int y1 = rect.y + (rect.height - h1 - h2 - gap) / 2;
  const int y2 = y1 + h1 + gap;
  draw_badge(dc, this, h.kind, method_of(h), rect.x + pad, y1, h1);
  const int tx = rect.x + pad + FromDIP(38);

  dc.SetFont(small.Bold());
  const wxString st = wxString::FromUTF8(status_text(h.kind, h.status));
  const int stx = right - dc.GetTextExtent(st).x;
  dc.SetTextForeground(status_colour(h.kind, h.status));
  dc.DrawText(st, stx, y1 + (h1 - dc.GetCharHeight()) / 2);

  dc.SetFont(small);
  const wxString when = wxString::FromUTF8(when_text(h.when_iso));
  const wxString ms = wxString::FromUTF8(format_ms(h.total_ms));
  const wxString tail = ms + wxString::FromUTF8("  \xC2\xB7  ") + when;
  const int tailx = right - dc.GetTextExtent(tail).x;
  dc.SetTextForeground(dim_colour());
  dc.DrawText(tail, tailx, y2);

  // The target: the path first ("/json?x=1"), then the host if it fits, so a narrow sidebar
  // does not cut every row to "127.0.0....8080/js".
  const std::string& t = l.target;
  const size_t slash = h.kind == RequestKind::Http ? t.find('/') : std::string::npos;
  const std::string path = slash == std::string::npos ? t : t.substr(slash);
  const std::string host = slash == std::string::npos ? std::string() : t.substr(0, slash);
  auto draw_target = [&](int x, int y, int avail, const wxColour& path_col) {
    if (avail <= FromDIP(12)) return;
    dc.SetTextForeground(path_col);
    const wxString ps =
        wxControl::Ellipsize(wxString::FromUTF8(path.size() > 1 || host.empty() ? path : t), dc, wxELLIPSIZE_END, avail);
    dc.DrawText(ps, x, y);
    const int used = dc.GetTextExtent(ps).x + FromDIP(6);
    if (path.size() > 1 && !host.empty() && avail - used > FromDIP(28)) {
      dc.SetTextForeground(dim_colour());
      dc.DrawText(wxControl::Ellipsize(wxString::FromUTF8(host), dc, wxELLIPSIZE_END, avail - used), x + used, y);
    }
  };
  draw_target(tx, y2, tailx - FromDIP(8) - tx, mix(window_colour(), text_colour(), 0.72));

  dc.SetFont(base);
  const int avail1 = stx - FromDIP(8) - tx;
  if (avail1 > FromDIP(12)) {
    if (!l.name.empty()) {
      dc.SetTextForeground(text_colour());
      dc.DrawText(wxControl::Ellipsize(wxString::FromUTF8(l.name), dc, wxELLIPSIZE_END, avail1), tx, y1);
    } else {
      // No saved request: the method / gRPC method name on top.
      std::string top = t;
      if (h.kind == RequestKind::Grpc) {
        const size_t at = top.find(" @ ");
        top = top.substr(0, at);
        if (const size_t sl = top.rfind('/'); sl != std::string::npos) top = top.substr(sl + 1);
      } else {
        top = path.size() > 1 ? path : t;
      }
      dc.SetTextForeground(mix(window_colour(), text_colour(), 0.88));
      dc.DrawText(wxControl::Ellipsize(wxString::FromUTF8(top), dc, wxELLIPSIZE_END, avail1), tx, y1);
    }
  }
}

}  // namespace watt::app::sidebar
