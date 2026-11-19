#include "app/sidebar_history.h"

#include <wx/clipbrd.h>
#include <wx/dc.h>
#include <wx/control.h>
#include <wx/menu.h>
#include <wx/time.h>

#include <algorithm>
#include <unordered_map>
#include <ctime>
#include <set>

#include "app/history_summary.h"
#include "app/sidebar_fuzzy.h"
#include "app/sidebar_style.h"
#include "core/model_history_store.h"

namespace watt::app::sidebar {

namespace {

// "GET https://host/x" -> "host/x"; "/pkg.Svc/M @ host:1" -> "pkg.Svc/M @ host:1".
std::string target_of(const HistoryEntry& e) {
  std::string s(history_template(e));
  if (e.kind == RequestKind::Http) {
    const size_t sp = s.find(' ');
    if (sp != std::string::npos) s.erase(0, sp + 1);
    for (const char* scheme : {"https://", "http://", "wss://", "ws://"})
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
  const std::string_view t = history_template(e);
  return std::string(t.substr(0, t.find(' ')));
}

// Repeats group by where they went (method + sent URL / address), so "GET {{base}}/echo" and a
// literal "GET http://127.0.0.1:18971/echo" sent to the same place fold into one run; entries
// from before the sent target was recorded group by their template.
std::string group_key(const HistoryEntry& e) {
  const std::string_view sent = history_sent(e);
  if (sent.empty()) return std::string(history_template(e));
  const std::string_view t = history_template(e);
  std::string head(e.kind == RequestKind::Grpc ? t.substr(0, t.find(" @ ")) : t.substr(0, t.find(' ')));
  return head + ' ' + std::string(sent);
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
  // The selected row's band follows the focus (OnDrawBackground).
  Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) {
    Refresh(false);
    e.Skip();
  });
  Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) {
    Refresh(false);
    e.Skip();
  });
  // A click opens the row as a preview (the host reuses the preview tab or activates the tab
  // that already shows it, so clicks never pile up tabs); a double click or Enter keeps it.
  // A click on a run's "x60" chip folds / unfolds the run instead.
  Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
    const int hit = VirtualHitTest(e.GetPosition().y);
    click_opened_ = -1;
    if (!entry(hit)) return;  // a day header or empty space: nothing to select
    if (on_chip(hit, e.GetPosition().x)) {
      SetSelection(hit);
      toggle_run(hit);
      return;
    }
    e.Skip();
    click_opened_ = hit;
    CallAfter([this, hit] { open_row(hit, false); });
  });
  Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent& e) {
    const int hit = VirtualHitTest(e.GetPosition().y);
    if (on_chip(hit, e.GetPosition().x)) {
      toggle_run(hit);  // the second click of a fast double click on the chip
      return;
    }
    if (entry(hit)) open_row(hit, true);  // the host makes the preview permanent (no second tab)
    click_opened_ = -1;
  });
  Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
    const int sel = GetSelection();
    const bool run = is_run(sel);
    switch (e.GetKeyCode()) {
      case WXK_RETURN:
      case WXK_NUMPAD_ENTER:
        open_row(sel, true);
        return;
      case WXK_DELETE:
      case WXK_NUMPAD_DELETE:
        if (entry(sel) && on_delete) on_delete(row_entries(sel));
        return;
      case WXK_RIGHT:
      case WXK_NUMPAD_RIGHT:
        if (run && !unfolded(sel)) toggle_run(sel);
        return;
      case WXK_LEFT:
      case WXK_NUMPAD_LEFT:
        if (run && unfolded(sel)) {
          toggle_run(sel);
        } else if (sel >= 0 && static_cast<size_t>(sel) < rows_.size() && rows_[static_cast<size_t>(sel)].child) {
          int r = sel;
          while (r > 0 && !is_run(r)) --r;
          SetSelection(r);
          toggle_run(r);
        }
        return;
      default:
        e.Skip();
    }
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
    const Row& row = rows_[static_cast<size_t>(hit)];
    const Label& l = label(static_cast<size_t>(row.entry));
    const std::string tmpl(history_template(*h));
    std::string tip = l.name.empty() ? tmpl : l.name + "\n" + tmpl;
    // Where it went at the time, and with which environment (not the one active now).
    if (const std::string_view sent = history_sent(*h); !sent.empty()) {
      if (tmpl.find(sent) == std::string::npos) tip += "\nSent to " + std::string(sent);
      if (const std::string_view env = history_env(*h); !env.empty()) tip += "  (env " + std::string(env) + ")";
      else if (tmpl.find("{{") != std::string::npos) tip += "  (no environment)";
    } else if (tmpl.find("{{") != std::string::npos) {
      tip += "\nSent before watt recorded the resolved target";
    }
    if (h->kind == RequestKind::Grpc && grpc_kind)
      if (const std::string k = grpc_kind_name(grpc_kind(*h)); !k.empty()) tip += "\n" + k;
    std::string when = h->when_iso;  // stored in UTC: shown in local time, like the rows
    if (std::tm lt{}; local_time(h->when_iso, &lt, nullptr)) {
      char buf[48];
      std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &lt);
      when = buf;
      if (const std::string ago = when_text(h->when_iso); !ago.empty() && ago.find(':') == std::string::npos)
        when += " (" + (ago == "now" ? std::string("just now") : ago + " ago") + ")";
    }
    tip += "\n" + status_text(h->kind, h->status) + "  \xC2\xB7  " + format_ms(h->total_ms) + "  \xC2\xB7  " + when;
    if (!row.run.empty()) {
      double lo = 1e300, hi = 0, sum = 0;
      for (int i : row.run) {
        const double ms = all_[static_cast<size_t>(i)].total_ms;
        lo = std::min(lo, ms);
        hi = std::max(hi, ms);
        sum += ms;
      }
      tip += "\n" + std::to_string(row.run.size()) + " sends in a row: min " + format_ms(lo) + ", avg " +
             format_ms(sum / static_cast<double>(row.run.size())) + ", max " + format_ms(hi) +
             "\nClick the count (or Right / Left) to show / fold them.";
    }
    SetToolTip(wxString::FromUTF8(tip));
  });
  Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) {
    e.Skip();
    const int old = hover_;
    hover_ = -1;
    if (old >= 0) RefreshRow(old);
  });
  // The menu opens on the right button's release itself, not on the WM_CONTEXTMENU that
  // follows it: a right click right after the pointer reached a row (its tooltip just being
  // replaced) lost that message, and the row was selected with no menu. The tooltip is
  // dropped first so it cannot cover the menu.
  Bind(wxEVT_RIGHT_DOWN, [this](wxMouseEvent& e) {
    right_row_ = VirtualHitTest(e.GetPosition().y);
    UnsetToolTip();
    if (entry(right_row_)) SetSelection(right_row_);
    SetFocus();
  });
  Bind(wxEVT_RIGHT_UP, [this](wxMouseEvent& e) {
    const int row = VirtualHitTest(e.GetPosition().y);
    const int down = right_row_;
    right_row_ = -1;
    if (row != down || !entry(row)) return;
    menu_shown_at_ = wxGetLocalTimeMillis();
    show_menu(row);
  });
  // When the user last scrolled (wheel, scrollbar): prepend() keeps their position then.
  Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& e) {
    e.Skip();
    user_scrolled_at_ = wxGetLocalTimeMillis();
  });
  auto scrolled = [this](wxScrollWinEvent& e) {
    e.Skip();
    user_scrolled_at_ = wxGetLocalTimeMillis();
  };
  Bind(wxEVT_SCROLLWIN_THUMBTRACK, scrolled);
  Bind(wxEVT_SCROLLWIN_LINEUP, scrolled);
  Bind(wxEVT_SCROLLWIN_LINEDOWN, scrolled);
  Bind(wxEVT_SCROLLWIN_PAGEUP, scrolled);
  Bind(wxEVT_SCROLLWIN_PAGEDOWN, scrolled);
  Bind(wxEVT_CONTEXT_MENU, [this](wxContextMenuEvent& e) {
    wxPoint p = e.GetPosition();
    // The one a right click sends after its RIGHT_UP: that click has its menu already.
    if (p != wxDefaultPosition && wxGetLocalTimeMillis() - menu_shown_at_ < 1000) return;
    int row = GetSelection();
    if (p != wxDefaultPosition) row = VirtualHitTest(ScreenToClient(p).y);
    show_menu(row);
  });
}

void HistoryList::show_menu(int row) {
  {
    const HistoryEntry* h = entry(row);
    if (!h) return;
    SetSelection(row);
    const HistoryEntry copy = *h;
    const Label l = label(static_cast<size_t>(rows_[static_cast<size_t>(row)].entry));
    const std::vector<HistoryEntry> members = row_entries(row);
    const bool run = is_run(row);
    const bool open = run && unfolded(row);
    enum { kOpen = 1, kCopyUrl, kCopyResolved, kSaved, kSaveAs, kCurl, kFold, kDelete, kClear };
    wxMenu m;
    m.Append(kOpen, "Open as sent\tEnter");
    m.Append(kSaved, "Open saved request")->Enable(!has_saved || has_saved(copy));
    m.Append(kSaveAs, wxString::FromUTF8("Save as request\xE2\x80\xA6"));
    m.AppendSeparator();
    m.Append(kCurl, copy.kind == RequestKind::Grpc ? "Copy as grpcurl" : "Copy as cURL");
    m.Append(kCopyUrl, "Copy URL / target");
    m.Append(kCopyResolved, "Copy resolved URL / target");
    m.AppendSeparator();
    if (run)
      m.Append(kFold, open ? wxString::Format("Fold the %zu repeats\tLeft", rows_[static_cast<size_t>(row)].run.size())
                           : wxString::Format("Show the %zu repeats\tRight", members.size()));
    m.Append(kDelete, members.size() > 1 ? wxString::Format("Delete %zu entries\tDel", members.size())
                                         : wxString("Delete entry\tDel"));
    m.Append(kClear, wxString::FromUTF8("Clear history\xE2\x80\xA6"));
    const int id = GetPopupMenuSelectionFromUser(m);
    switch (id) {
      case kOpen:
        if (on_open) on_open(copy, true, false);
        return;
      case kSaved:
        if (on_open) on_open(copy, false, true);
        return;
      case kSaveAs:
        if (on_save_as) on_save_as(copy);
        return;
      case kCurl:
        if (on_copy_curl) on_copy_curl(copy);
        return;
      case kFold:
        toggle_run(row);
        return;
      case kDelete:
        if (on_delete) on_delete(members);
        return;
      case kClear:
        if (on_clear) on_clear();
        return;
      default:
        break;
    }
    std::string s;
    if (id == kCopyUrl) {
      s = history_template(copy);
      if (copy.kind == RequestKind::Http) {
        const size_t sp = s.find(' ');
        if (sp != std::string::npos) s.erase(0, sp + 1);
      }
    } else if (id == kCopyResolved) {
      s = history_sent(copy);  // with its scheme, as sent
      if (s.empty()) s = l.target;
    }
    if (!s.empty() && wxTheClipboard->Open()) {
      wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(s)));
      wxTheClipboard->Close();
    }
  }
}

void HistoryList::open_row(int row, bool permanent) {
  if (const HistoryEntry* h = entry(row); h && on_open) {
    const HistoryEntry copy = *h;
    on_open(copy, permanent, false);
  }
}

bool HistoryList::is_run(int row) const {
  return row >= 0 && static_cast<size_t>(row) < rows_.size() && !rows_[static_cast<size_t>(row)].run.empty();
}

bool HistoryList::unfolded(int row) const {
  const std::string k = run_key(row);
  return !k.empty() && std::find(unfolded_.begin(), unfolded_.end(), k) != unfolded_.end();
}

bool HistoryList::on_chip(int row, int x) const {
  if (row < 0 || static_cast<size_t>(row) >= chip_x_.size() || !is_run(row)) return false;
  const auto [l, r] = chip_x_[static_cast<size_t>(row)];
  return r > l && x >= l - FromDIP(3) && x < r + FromDIP(3);
}

std::vector<HistoryEntry> HistoryList::row_entries(int row) const {
  std::vector<HistoryEntry> out;
  if (!entry(row)) return out;
  const Row& r = rows_[static_cast<size_t>(row)];
  if (is_run(row) && !unfolded(row)) {
    for (int i : r.run) out.push_back(all_[static_cast<size_t>(i)]);
  } else {
    out.push_back(all_[static_cast<size_t>(r.entry)]);
  }
  return out;
}

std::string HistoryList::run_key(int row) const {
  if (!is_run(row)) return {};
  const HistoryEntry& oldest = all_[static_cast<size_t>(rows_[static_cast<size_t>(row)].run.back())];
  return oldest.when_iso + "\x01" + oldest.summary;
}

void HistoryList::toggle_run(int row) {
  const std::string k = run_key(row);
  if (k.empty()) return;
  auto it = std::find(unfolded_.begin(), unfolded_.end(), k);
  if (it == unfolded_.end()) unfolded_.push_back(k);
  else unfolded_.erase(it);
  const size_t top = GetVisibleBegin();
  refilter();
  // The run row stays where it was (its entries come below it), and stays selected.
  for (size_t r = 0; r < rows_.size(); ++r)
    if (run_key(static_cast<int>(r)) == k) {
      SetSelection(static_cast<int>(r));
      break;
    }
  if (!rows_.empty()) ScrollToRow(std::min(top, rows_.size() - 1));
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
  const size_t top = GetVisibleBegin();
  refilter();
  // The new entry is in view: the list goes back to its top, unless the user is scrolling it
  // right now (then their position is kept). Sends after clicking an older row used to land
  // above the visible area with the panel looking unchanged.
  if (top > 0 && wxGetLocalTimeMillis() - user_scrolled_at_ > 1500) ScrollToRow(0);
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

void HistoryList::remove(const std::vector<HistoryEntry>& entries) {
  if (entries.empty()) return;
  std::set<std::string> keys;
  for (const auto& e : entries) keys.insert(history_entry_key(e));
  int sel = GetSelection();
  std::vector<HistoryEntry> keep;
  std::vector<Label> keep_labels;
  keep.reserve(all_.size());
  keep_labels.reserve(all_.size());
  for (size_t i = 0; i < all_.size(); ++i) {
    if (keys.count(history_entry_key(all_[i]))) continue;
    keep.push_back(std::move(all_[i]));
    keep_labels.push_back(std::move(labels_[i]));
  }
  all_ = std::move(keep);
  labels_ = std::move(keep_labels);
  refilter();
  // The row that took the deleted one's place is selected (Del, Del, Del walks down the list).
  const int n = static_cast<int>(rows_.size());
  if (n == 0) return;
  sel = std::min(std::max(sel, 0), n - 1);
  if (!entry(sel)) sel = sel + 1 < n ? sel + 1 : sel - 1;
  if (entry(sel)) SetSelection(sel);
}

void HistoryList::clear_all() {
  all_.clear();
  labels_.clear();
  unfolded_.clear();
  refilter();
}

void HistoryList::set_filter(const std::string& q) {
  if (q == filter_) return;
  // The highlight stays on its entry: the rows are renumbered, and the same row number is
  // another entry (after clearing the filter the first row was highlighted, not the open one).
  const HistoryEntry* sel = entry(GetSelection());
  const std::string when = sel ? sel->when_iso : std::string(), summary = sel ? sel->summary : std::string();
  filter_ = q;
  refilter();
  select_entry(when, summary);
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
  // The entries that pass the filter, with their day.
  std::vector<int> shown;
  std::vector<std::string> days;
  shown.reserve(all_.size());
  days.reserve(all_.size());
  for (size_t i = 0; i < all_.size(); ++i) {
    if (!filter_.empty()) {
      const Label& l = label(i);
      // The template, the sent target and its environment (all in summary), and the name.
      std::string hay = all_[i].summary;
      for (char& c : hay)
        if (c == kHistorySep) c = ' ';
      if (!words_match(filter_, hay + " " + l.name + " " + l.target)) continue;
    }
    shown.push_back(static_cast<int>(i));
    days.push_back(day_text(all_[i].when_iso));
  }
  // Repeats of one request within a day fold into one row at its newest send, also when other
  // sends fall in between (two Health checks 5 min apart group like four List users in a row):
  // same day, kind, target, status and request (its id, else its name).
  auto key_of = [&](size_t a) {
    const HistoryEntry& x = all_[static_cast<size_t>(shown[a])];
    std::string k = std::to_string(static_cast<int>(x.kind)) + '\x1f' + std::to_string(x.status) + '\x1f' + group_key(x);
    const std::string id = identity ? identity(x) : std::string();
    k += id.empty() ? "\x1fn" + label(static_cast<size_t>(shown[a])).name : "\x1fi" + id;
    return k;
  };
  std::vector<std::vector<int>> groups;  // the day's groups, in the order of their newest send
  std::unordered_map<std::string, size_t> group_at;
  auto flush = [&] {
    for (auto& g : groups) {
      if (g.size() == 1) {
        rows_.push_back(Row{g[0], {}, {}, false});
      } else {
        Row r{g[0], {}, {}, false};
        r.run = g;
        const HistoryEntry& oldest = all_[static_cast<size_t>(g.back())];
        const bool open =
            std::find(unfolded_.begin(), unfolded_.end(), oldest.when_iso + "\x01" + oldest.summary) != unfolded_.end();
        rows_.push_back(std::move(r));
        if (open)
          for (int m : g) rows_.push_back(Row{m, {}, {}, true});
      }
      shown_count_ += g.size();
    }
    groups.clear();
    group_at.clear();
  };
  std::string day;
  for (size_t k = 0; k < shown.size(); ++k) {
    if (rows_.empty() || days[k] != day) {
      flush();
      day = days[k];
      rows_.push_back(Row{-1, day, {}, false});
    }
    auto [it, fresh] = group_at.try_emplace(key_of(k), groups.size());
    if (fresh) groups.emplace_back();
    groups[it->second].push_back(shown[k]);
  }
  flush();
  hover_ = -1;
  click_opened_ = -1;
  chip_x_.assign(rows_.size(), {0, 0});
  SetItemCount(rows_.size());
  Refresh();
}

void HistoryList::select_entry(const std::string& when_iso, const std::string& summary) {
  int sel_row = wxNOT_FOUND;
  auto is_sel = [&](int e) {
    const HistoryEntry& x = all_[static_cast<size_t>(e)];
    return x.when_iso == when_iso && x.summary == summary;
  };
  if (!when_iso.empty())
    for (size_t r = 0; r < rows_.size() && sel_row == wxNOT_FOUND; ++r) {
      if (rows_[r].entry < 0) continue;
      if (is_sel(rows_[r].entry)) sel_row = static_cast<int>(r);
      else if (!rows_[r].child)
        for (int m : rows_[r].run)
          if (is_sel(m)) sel_row = static_cast<int>(r);  // folded into a run row
    }
  if (sel_row != GetSelection()) SetSelection(sel_row);
}

const HistoryEntry* HistoryList::entry(int row) const {
  if (row < 0 || static_cast<size_t>(row) >= rows_.size()) return nullptr;
  const int e = rows_[static_cast<size_t>(row)].entry;
  return e < 0 ? nullptr : &all_[static_cast<size_t>(e)];
}

int HistoryList::content_height(size_t max_rows) const {
  int h = 0;
  for (size_t i = 0; i < rows_.size() && i < max_rows; ++i) h += OnMeasureItem(i);
  return h;
}

wxCoord HistoryList::OnMeasureItem(size_t n) const {
  if (n < rows_.size() && (rows_[n].entry < 0 || rows_[n].child)) return FromDIP(24);
  return FromDIP(38);
}

void HistoryList::OnDrawBackground(wxDC& dc, const wxRect& rect, size_t n) const {
  if (n >= rows_.size() || rows_[n].entry < 0) return;
  wxColour bg;
  // The entry the shown tab holds: the accent band while the list has the keyboard, a quiet grey
  // one otherwise (as the tree's unfocused selection), so it never looks like the active control.
  if (IsSelected(n)) bg = HasFocus() ? highlight_colour() : mix(window_colour(), text_colour(), 0.10);
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
  if (n < chip_x_.size()) chip_x_[n] = {0, 0};
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
  const Row& row = rows_[n];
  const HistoryEntry& h = all_[static_cast<size_t>(row.entry)];
  const Label& l = label(static_cast<size_t>(row.entry));
  const wxFont base = GetFont();
  wxFont small = base;
  small.SetFractionalPointSize(small.GetFractionalPointSize() * 0.88);
  const int call_kind = h.kind == RequestKind::Grpc && grpc_kind ? grpc_kind(h) : -1;

  if (row.child) {
    // One entry of an unfolded run, on one line: a guide, the status, the time, when.
    dc.SetPen(wxPen(mix(window_colour(), text_colour(), 0.25), FromDIP(1)));
    const int gx = rect.x + pad + FromDIP(14);
    dc.DrawLine(gx, rect.y, gx, rect.GetBottom() + 1);
    dc.SetFont(small);
    const int y = rect.y + (rect.height - dc.GetCharHeight()) / 2;
    const wxString when = wxString::FromUTF8(history_when_label(h.when_iso));
    dc.SetTextForeground(dim_colour());
    dc.DrawText(when, right - dc.GetTextExtent(when).x, y);
    dc.SetFont(small.Bold());
    const wxString st = wxString::FromUTF8(status_text(h.kind, h.status));
    dc.SetTextForeground(status_colour(h.kind, h.status));
    const int tx = gx + FromDIP(10);
    dc.DrawText(st, tx, y);
    const int stw = dc.GetTextExtent(st).x;
    dc.SetFont(small);
    dc.SetTextForeground(mix(window_colour(), text_colour(), 0.8));
    dc.DrawText(wxString::FromUTF8(format_ms(h.total_ms)), tx + stw + FromDIP(12), y);
    return;
  }

  // Line 1: badge, name, (run count), status. Line 2 (dimmed, smaller): the target as sent,
  // elapsed (a run: the average), time.
  dc.SetFont(base);
  const int h1 = dc.GetCharHeight();
  dc.SetFont(small);
  const int h2 = dc.GetCharHeight();
  const int gap = FromDIP(1);
  const int y1 = rect.y + (rect.height - h1 - h2 - gap) / 2;
  const int y2 = y1 + h1 + gap;
  draw_badge(dc, this, h.kind, method_of(h), rect.x + pad, y1, h1, call_kind);
  const int tx = rect.x + pad + FromDIP(38);

  dc.SetFont(small.Bold());
  const wxString st = wxString::FromUTF8(status_text(h.kind, h.status));
  int stx = right - dc.GetTextExtent(st).x;
  dc.SetTextForeground(status_colour(h.kind, h.status));
  dc.DrawText(st, stx, y1 + (h1 - dc.GetCharHeight()) / 2);
  if (!row.run.empty()) {
    // "x60" chip left of the status (a triangle while unfolded): a click shows / folds the run.
    const wxString c = wxString::FromUTF8(unfolded(static_cast<int>(n)) ? "\xE2\x96\xBE " : "\xC3\x97") +
                       wxString::Format("%zu", row.run.size());
    const wxSize cs = dc.GetTextExtent(c);
    const int cw = cs.x + FromDIP(10), ch = cs.y + FromDIP(2);
    const int cx = stx - FromDIP(8) - cw;
    const int cy = y1 + (h1 - ch) / 2;
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(mix(window_colour(), text_colour(), IsSelected(n) ? 0.24 : 0.14)));
    dc.DrawRoundedRectangle(cx, cy, cw, ch, FromDIP(3));
    dc.SetTextForeground(mix(window_colour(), text_colour(), 0.9));
    dc.DrawText(c, cx + FromDIP(5), cy + FromDIP(1));
    if (n < chip_x_.size()) chip_x_[n] = {cx, cx + cw};
    stx = cx;
  }

  dc.SetFont(small);
  const wxString when = wxString::FromUTF8(when_text(h.when_iso));
  double ms = h.total_ms;
  if (!row.run.empty()) {
    double sum = 0;
    for (int i : row.run) sum += all_[static_cast<size_t>(i)].total_ms;
    ms = sum / static_cast<double>(row.run.size());
  }
  const wxString mss = wxString::FromUTF8((row.run.empty() ? std::string() : std::string("avg ")) + format_ms(ms));
  const wxString tail = mss + wxString::FromUTF8("  \xC2\xB7  ") + when;
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
        wxControl::Ellipsize(wxString::FromUTF8(path.size() > 1 || host.empty() ? path : t), dc, wxELLIPSIZE_END, avail, wxELLIPSIZE_FLAGS_EXPAND_TABS);
    dc.DrawText(ps, x, y);
    const int used = dc.GetTextExtent(ps).x + FromDIP(6);
    if (path.size() > 1 && !host.empty() && avail - used > FromDIP(28)) {
      dc.SetTextForeground(dim_colour());
      dc.DrawText(wxControl::Ellipsize(wxString::FromUTF8(host), dc, wxELLIPSIZE_END, avail - used, wxELLIPSIZE_FLAGS_EXPAND_TABS), x + used, y);
    }
  };
  // An unnamed HTTP entry has its path on line 1 already: line 2 names only the host.
  const bool path_on_top = l.name.empty() && h.kind == RequestKind::Http && path.size() > 1 && !host.empty();
  if (path_on_top) {
    const int avail = tailx - FromDIP(8) - tx;
    if (avail > FromDIP(12)) {
      dc.SetTextForeground(mix(window_colour(), text_colour(), 0.72));
      dc.DrawText(wxControl::Ellipsize(wxString::FromUTF8(host), dc, wxELLIPSIZE_END, avail, wxELLIPSIZE_FLAGS_EXPAND_TABS), tx, y2);
    }
  } else {
    draw_target(tx, y2, tailx - FromDIP(8) - tx, mix(window_colour(), text_colour(), 0.72));
  }

  dc.SetFont(base);
  const int avail1 = stx - FromDIP(8) - tx;
  if (avail1 > FromDIP(12)) {
    if (!l.name.empty()) {
      dc.SetTextForeground(text_colour());
      dc.DrawText(wxControl::Ellipsize(wxString::FromUTF8(l.name), dc, wxELLIPSIZE_END, avail1, wxELLIPSIZE_FLAGS_EXPAND_TABS), tx, y1);
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
      dc.DrawText(wxControl::Ellipsize(wxString::FromUTF8(top), dc, wxELLIPSIZE_END, avail1, wxELLIPSIZE_FLAGS_EXPAND_TABS), tx, y1);
    }
  }
}

}  // namespace watt::app::sidebar
