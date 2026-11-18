// Response view: status strip (status, total, client overhead, size, protocol, remote address),
// a timing waterfall with per-phase tooltips and a delta against the previous run of the same
// request (method + expanded URL + body; gRPC: target + method + body), and tabs for the body (response_body.*), headers, gRPC trailers, the live gRPC
// message list and the request as sent.
//
// Everything that scales with the response size (pretty-printing, hex, Scintilla line indexing,
// search counts) runs on the body view's worker; the UI thread only fills the strip, the
// waterfall and virtual lists (which ask for the rows they draw).
#include <wx/listctrl.h>
#include <wx/dcbuffer.h>
#include <wx/panel.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/clipbrd.h>
#include <wx/menu.h>
#include <wx/datetime.h>
#include <wx/app.h>
#include <wx/wupdlock.h>
#include <wx/textentry.h>
#include <wx/textdlg.h>
#include <wx/stc/stc.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <memory>
#include <thread>

#include "app/history_summary.h"
#include "core/asserts.h"
#include "core/model_history_store.h"
#include "core/script.h"

#include "app/http_editor_folder.h"
#include "app/panels.h"
#include "app/request_tab.h"
#include "app/response_body.h"
#include "app/response_conn.h"
#include "app/response_cookies.h"
#include "app/http_editor_cookies.h"
#include "app/mock_view.h"  // mock_examples_changed
#include "app/response_examples.h"
#include "app/response_format.h"
#include "app/response_keep.h"
#include "app/response_sse.h"
#include "app/response_sse_view.h"
#include "app/response_tests.h"
#include "app/response_tests_view.h"
#include "app/response_widgets.h"

namespace watt::app {
namespace {

using namespace rv;
using KV = std::vector<std::pair<std::string, std::string>>;

wxString U(std::string_view s) { return wxString::FromUTF8(s.data(), s.size()); }

// WATT_RV_TRACE=<file>: appends "label ms" lines for the view's own UI-thread steps (build,
// clear, show) so their cost can be measured in the running app. Off (one getenv) otherwise.
FILE* trace_file() {
  static FILE* f = [] () -> FILE* {
    const wchar_t* p = _wgetenv(L"WATT_RV_TRACE");
    return p && *p ? _wfopen(p, L"a") : nullptr;
  }();
  return f;
}
// Lap timer for one step split in parts: "what.part ms" lines (only when tracing is on).
struct TraceLaps {
  const char* what;
  Clock::time_point t = Clock::now();
  explicit TraceLaps(const char* w) : what(w) {}
  void lap(const char* part) {
    if (FILE* f = trace_file()) {
      const auto now = Clock::now();
      std::fprintf(f, "%s.%s %.2f\n", what, part, std::chrono::duration<double, std::milli>(now - t).count());
      std::fflush(f);
      t = now;
    }
  }
};
struct TraceScope {
  const char* what;
  Clock::time_point t0 = Clock::now();
  explicit TraceScope(const char* w) : what(w) {}
  ~TraceScope() {
    if (FILE* f = trace_file()) {
      std::fprintf(f, "%s %.2f\n", what, std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
      std::fflush(f);
    }
  }
};

// ------------------------------------------------------------------ lazy pages
//
// Every page but Body is created the first time it is shown (or in a short idle slice after the
// view is up): a list control, the Tests editor or the Log editor each cost several ms to create,
// and the view is built synchronously by the first send of a tab. A page keeps its data itself,
// so filling it while it is not built costs nothing but the copy.

class LazyPage : public wxPanel {
 public:
  explicit LazyPage(wxWindow* parent) : wxPanel(parent, wxID_ANY) { SetBackgroundColour(Theme::get().bg); }
  bool built() const { return built_; }
  void ensure() {
    if (built_) return;
    built_ = true;
    wxWindow* w = make();
    auto* s = new wxBoxSizer(wxVERTICAL);
    s->Add(w, 1, wxEXPAND);
    SetSizer(s);
    Layout();
    after_make();
  }

 protected:
  virtual wxWindow* make() = 0;
  virtual void after_make() {}

 private:
  bool built_ = false;
};

void to_clipboard(const wxString& s) {
  if (wxTheClipboard->Open()) {
    wxTheClipboard->SetData(new wxTextDataObject(s));
    wxTheClipboard->Close();
  }
}

struct KvData {
  KV rows;
  std::vector<char> dim;  // dim[i] != 0: row i is drawn dimmed (a header the HTTP stack added)
};

class KvList : public FastList {
 public:
  KvList(wxWindow* parent, const char* c1, const char* c2, const KvData* d)
      : FastList(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                   wxLC_REPORT | wxLC_VIRTUAL | wxBORDER_NONE),
        d_(d) {
    AppendColumn(c1, wxLIST_FORMAT_LEFT, FromDIP(200));
    AppendColumn(c2, wxLIST_FORMAT_LEFT, FromDIP(400));
    // Last column fills the width. Deferred: setting a column width inside the size event can
    // toggle the scrollbars and re-enter the handler (unbounded recursion on some widths).
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      if (fit_pending_) return;
      fit_pending_ = true;
      CallAfter([this] {
        fit_pending_ = false;
        const int w = GetClientSize().x - GetColumnWidth(0) - 2;
        if (w > FromDIP(80) && w != GetColumnWidth(1)) {
          Freeze();  // (a width change repaints the list at once otherwise)
          SetColumnWidth(1, w);
          Thaw();
        }
      });
    });
    Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
      if (e.GetModifiers() == wxMOD_CONTROL && e.GetKeyCode() == 'C') return copy(false);
      if (e.GetModifiers() == wxMOD_CONTROL && e.GetKeyCode() == 'A') {
        for (long i = 0; i < GetItemCount(); ++i) SetItemState(i, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
        return;
      }
      e.Skip();
    });
    Bind(wxEVT_CONTEXT_MENU, [this](wxContextMenuEvent&) {
      wxMenu m;
      m.Append(1, "Copy selected\tCtrl+C");
      m.Append(2, "Copy all");
      m.Append(3, "Copy value");
      const int id = GetPopupMenuSelectionFromUser(m);
      if (id == 1) copy(false);
      else if (id == 2) copy(true);
      else if (id == 3) {
        const long i = GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (i >= 0 && static_cast<size_t>(i) < d_->rows.size()) to_clipboard(U(d_->rows[static_cast<size_t>(i)].second));
      }
    });
    sync();
  }
  void sync() {
    SetItemCount(static_cast<long>(d_->rows.size()));
    Refresh();
  }

 private:
  wxString OnGetItemText(long item, long col) const override {
    if (static_cast<size_t>(item) >= d_->rows.size()) return {};
    const auto& r = d_->rows[static_cast<size_t>(item)];
    return U(col == 0 ? r.first : r.second);
  }
  wxItemAttr* OnGetItemAttr(long item) const override {
    return static_cast<size_t>(item) < d_->dim.size() && d_->dim[static_cast<size_t>(item)] ? &dim_attr_ : nullptr;
  }
  void copy(bool all) {
    std::string s;
    for (size_t i = 0; i < d_->rows.size(); ++i)
      if (all || GetItemState(static_cast<long>(i), wxLIST_STATE_SELECTED))
        s += d_->rows[i].first + ": " + d_->rows[i].second + "\n";
    if (!s.empty()) to_clipboard(U(s));
  }
  const KvData* d_;
  mutable wxItemAttr dim_attr_{Theme::get().dim, wxNullColour, wxNullFont};
  bool fit_pending_ = false;
};

// Headers, Cookies, Trailers, Request: two columns of text.
class KvPage : public LazyPage {
 public:
  KvPage(wxWindow* parent, const char* c1, const char* c2) : LazyPage(parent), c1_(c1), c2_(c2) {}
  void set(KV rows, std::vector<char> dim = {}) {
    d_.rows = std::move(rows);
    d_.dim = std::move(dim);
    d_.dim.resize(d_.rows.size(), 0);
    if (list_) list_->sync();
  }
  size_t count() const { return d_.rows.size(); }
  const KV& rows() const { return d_.rows; }

 private:
  wxWindow* make() override { return list_ = new KvList(this, c1_, c2_, &d_); }
  const char* c1_;
  const char* c2_;
  KvData d_;
  KvList* list_ = nullptr;
};

// The Cookies page: every Set-Cookie of the response as Postman / Insomnia show them (name,
// value, domain, path, expiry, flags) plus what the workspace jar did with it, under a line
// that says where they went and opens the jar ("Manage cookies").
class CookieList : public FastList {
 public:
  enum Col { kName, kValue, kDomain, kPath, kExpires, kFlags, kJar, kCols };
  CookieList(wxWindow* parent, const std::vector<rv::SetCookieRow>* rows)
      : FastList(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_VIRTUAL | wxBORDER_NONE),
        rows_(rows) {
    const char* names[kCols] = {"Name", "Value", "Domain", "Path", "Expires", "Flags", "Jar"};
    const int widths[kCols] = {140, 240, 150, 70, 100, 140, 180};
    for (int c = 0; c < kCols; ++c) AppendColumn(names[c], wxLIST_FORMAT_LEFT, FromDIP(widths[c]));
    // Refit after a resize, deferred (a width change inside the size event can re-enter it).
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      if (fit_pending_) return;
      fit_pending_ = true;
      CallAfter([this] {
        fit_pending_ = false;
        fit_columns();
      });
    });
    Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
      if (e.GetModifiers() == wxMOD_CONTROL && e.GetKeyCode() == 'C') return copy(false, false);
      if (e.GetModifiers() == wxMOD_CONTROL && e.GetKeyCode() == 'A') {
        for (long i = 0; i < GetItemCount(); ++i) SetItemState(i, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
        return;
      }
      e.Skip();
    });
    // The whole cell under the pointer as a tooltip (a long value, the jar's full reason).
    Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
      e.Skip();
      int flags = 0;
      long col = -1;
      const long item = HitTest(e.GetPosition(), flags, &col);
      if (item == tip_item_ && col == tip_col_) return;
      tip_item_ = item;
      tip_col_ = col;
      wxString t;
      if (item >= 0 && (col == kJar || col == kValue)) {
        t = OnGetItemText(item, col);
        wxClientDC dc(this);
        dc.SetFont(GetFont());
        if (dc.GetTextExtent(t).x + FromDIP(12) <= GetColumnWidth(static_cast<int>(col))) t.clear();
      }
      if (t.empty()) UnsetToolTip();
      else SetToolTip(t);
    });
    Bind(wxEVT_CONTEXT_MENU, [this](wxContextMenuEvent&) {
      wxMenu m;
      m.Append(1, "Copy value\tCtrl+C");
      m.Append(2, "Copy name=value");
      m.Append(3, "Copy Set-Cookie header");
      m.Append(4, "Copy all as Set-Cookie headers");
      const int id = GetPopupMenuSelectionFromUser(m);
      if (id == 1) copy(false, false);
      else if (id == 2) copy(false, true);
      else if (id == 3) copy_raw(false);
      else if (id == 4) copy_raw(true);
    });
    sync();
  }
  void sync() {
    SetItemCount(static_cast<long>(rows_->size()));
    fit_columns();
    Refresh();
  }

 private:
  // Each column as wide as its widest cell (the first 200 rows, capped), so the seven columns
  // fit a narrow response pane; Value may grow to the right edge.
  void fit_columns() {
    if (GetClientSize().x <= 0) return;
    static const int caps[kCols] = {200, 320, 200, 140, 120, 170, 520};
    wxClientDC dc(this);
    dc.SetFont(GetFont());
    int w[kCols];
    for (int c = 0; c < kCols; ++c) {
      wxListItem it;
      it.SetMask(wxLIST_MASK_TEXT);
      GetColumn(c, it);
      w[c] = dc.GetTextExtent(it.GetText()).x;
    }
    const size_t n = std::min<size_t>(rows_->size(), 200);
    for (size_t i = 0; i < n; ++i)
      for (int c = 0; c < kCols; ++c)
        w[c] = std::max(w[c], dc.GetTextExtent(OnGetItemText(static_cast<long>(i), c)).x);
    int cw[kCols];
    int used = 0;
    for (int c = 0; c < kCols; ++c) {
      cw[c] = std::min(w[c] + FromDIP(18), FromDIP(caps[c]));
      used += cw[c];
    }
    // No horizontal scrolling at the pane width: Value gives way first, then the Jar reason (its
    // full text is the row's tooltip), down to a readable minimum each.
    const int room = GetClientSize().x - 4;
    if (room > 0 && used > room) {
      const int v = std::max(FromDIP(80), cw[kValue] - (used - room));
      used -= cw[kValue] - v;
      cw[kValue] = v;
    }
    if (room > 0 && used > room) {
      const int j = std::max(FromDIP(110), cw[kJar] - (used - room));
      used -= cw[kJar] - j;
      cw[kJar] = j;
    }
    if (room > 0 && used > room) {  // a very narrow pane: every column in proportion
      int sum = 0;
      for (int c = 0; c < kCols; ++c) sum += (cw[c] = std::max(FromDIP(36), cw[c] * room / used));
      used = sum;
    }
    Freeze();
    for (int c = 0; c < kCols; ++c) SetColumnWidth(c, cw[c]);
    const int spare = room - used;
    if (spare > 0) SetColumnWidth(kValue, cw[kValue] + spare);
    Thaw();
  }
  wxString OnGetItemText(long item, long col) const override {
    if (item < 0 || static_cast<size_t>(item) >= rows_->size()) return {};
    const rv::SetCookieRow& r = (*rows_)[static_cast<size_t>(item)];
    switch (col) {
      case kName: return U(r.name.empty() ? std::string("(no name)") : r.name);
      case kValue: return U(r.value.size() > 512 ? r.value.substr(0, 512) + "\xE2\x80\xA6" : r.value);
      case kDomain: return U(r.host_only ? r.domain : "." + r.domain);
      case kPath: return U(r.path);
      case kExpires: return U(r.expires);
      case kFlags: return U(rv::cookie_flags_text(r));
      case kJar: return U(rv::cookie_fate_text(r));
      default: return {};
    }
  }
  wxItemAttr* OnGetItemAttr(long item) const override {
    if (item < 0 || static_cast<size_t>(item) >= rows_->size()) return nullptr;
    switch ((*rows_)[static_cast<size_t>(item)].fate) {
      case rv::SetCookieRow::Fate::Ignored: return &warn_attr_;
      case rv::SetCookieRow::Fate::Deletes: return &dim_attr_;
      default: return nullptr;
    }
  }
  void copy(bool all, bool pair) {
    std::string s;
    for (size_t i = 0; i < rows_->size(); ++i)
      if (all || GetItemState(static_cast<long>(i), wxLIST_STATE_SELECTED)) {
        const rv::SetCookieRow& r = (*rows_)[i];
        s += pair ? r.name + "=" + r.value : r.value;
        s += '\n';
      }
    if (!s.empty()) s.pop_back();
    if (!s.empty()) to_clipboard(U(s));
  }
  void copy_raw(bool all) {
    std::string s;
    for (size_t i = 0; i < rows_->size(); ++i)
      if (all || GetItemState(static_cast<long>(i), wxLIST_STATE_SELECTED)) s += "Set-Cookie: " + (*rows_)[i].raw + "\n";
    if (!s.empty()) to_clipboard(U(s));
  }
  const std::vector<rv::SetCookieRow>* rows_;
  bool fit_pending_ = false;
  long tip_item_ = -2, tip_col_ = -2;
  mutable wxItemAttr warn_attr_{Theme::get().warn, wxNullColour, wxNullFont};
  mutable wxItemAttr dim_attr_{Theme::get().dim, wxNullColour, wxNullFont};
};

class CookiePage : public LazyPage {
 public:
  // on_manage: opens the workspace cookie jar on the given host's cookies.
  CookiePage(wxWindow* parent, std::function<void(const std::string&)> on_manage)
      : LazyPage(parent), on_manage_(std::move(on_manage)) {}
  void set(std::vector<rv::SetCookieRow> rows, std::string note = {}, Tone tone = Tone::Dim) {
    rows_ = std::move(rows);
    note_ = std::move(note);
    tone_ = tone;
    if (list_) {
      list_->sync();
      bar_->set_note(note_, tone_);
      bar_->set_enabled(1, !rows_.empty());
    }
  }
  size_t count() const { return rows_.size(); }

 private:
  wxWindow* make() override {
    auto* p = new wxPanel(this);
    p->SetBackgroundColour(Theme::get().bg);
    bar_ = new ChipBar(p, [this](int id) {
      if (id == 1 && on_manage_) on_manage_(rows_.empty() ? std::string() : rows_.front().domain);
    });
    bar_->add(1, "Manage cookies\xE2\x80\xA6",
              "Open the workspace cookie jar on this host's cookies: every cookie later requests send, "
              "to edit, delete or clear.",
              -1, true);
    bar_->set_note(note_, tone_);
    bar_->set_enabled(1, !rows_.empty());
    list_ = new CookieList(p, &rows_);
    auto* s = new wxBoxSizer(wxVERTICAL);
    s->Add(bar_, 0, wxEXPAND);
    s->Add(list_, 1, wxEXPAND);
    p->SetSizer(s);
    return p;
  }
  std::function<void(const std::string&)> on_manage_;
  std::vector<rv::SetCookieRow> rows_;
  std::string note_;
  Tone tone_ = Tone::Dim;
  ChipBar* bar_ = nullptr;
  CookieList* list_ = nullptr;
};

struct MsgRow {
  double at_ms = 0;
  double gap_ms = 0;
  size_t bytes = 0;
  std::string preview;
  bool error = false;
};

// The Messages table: owner-drawn (a native list view costs 40-100 ms of UI time to paint a page
// of rows in this theme, every time the tab is shown). Only the visible rows are drawn.
class MsgList : public wxWindow {
 public:
  MsgList(wxWindow* parent, const std::vector<MsgRow>* rows, std::function<void(size_t)> on_open)
      : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxWANTS_CHARS | wxBORDER_NONE),
        rows_(rows),
        on_open_(std::move(on_open)) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    const Theme& th = Theme::get();
    bg_ = th.dark ? wxColour(30, 30, 30) : *wxWHITE;
    SetBackgroundColour(bg_);
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      update_scrollbar();
      Refresh(false);
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
      if (const long r = row_at(e.GetPosition().y); r >= 0) select(r);
    });
    Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent& e) {
      if (const long r = row_at(e.GetPosition().y); r >= 0 && on_open_) on_open_(static_cast<size_t>(r));
    });
    Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
      const long r = row_at(e.GetPosition().y);
      if (r == tip_row_) return;
      tip_row_ = r;
      if (r < 0) return UnsetToolTip();
      const MsgRow& m = (*rows_)[static_cast<size_t>(r)];
      SetToolTip(U("#" + std::to_string(r + 1) + "  +" + fmt::ms(m.at_ms) + "  " + fmt::human_bytes(m.bytes) + "\n" +
                   m.preview +
                   "\n\nOffset: from the call start (Send click), the clock of the Timing total; the "
                   "Timing's Wait (TTFB) ends earlier, at the response headers. Gap: since the "
                   "previous message.\nDouble-click or Enter: show it in the Body."));
    });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) {
      e.Skip();
      tip_row_ = -2;
    });
    Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
      const int k = e.GetKeyCode();
      const long n = count();
      if (e.GetModifiers() == wxMOD_CONTROL && k == 'C') {
        if (sel_ >= 0 && sel_ < n) to_clipboard(U((*rows_)[static_cast<size_t>(sel_)].preview));
        return;
      }
      if (e.GetModifiers() != 0 || n == 0) return e.Skip();
      const long page = std::max(1, visible_rows() - 1);
      switch (k) {
        case WXK_UP: return select(std::max(0L, sel_ - 1));
        case WXK_DOWN: return select(std::min(n - 1, sel_ + 1));
        case WXK_PAGEUP: return select(std::max(0L, sel_ - page));
        case WXK_PAGEDOWN: return select(std::min(n - 1, sel_ + page));
        case WXK_HOME: return select(0);
        case WXK_END: return select(n - 1);
        case WXK_RETURN:
        case WXK_NUMPAD_ENTER:
          if (sel_ >= 0 && on_open_) on_open_(static_cast<size_t>(sel_));
          return;
        default: e.Skip();
      }
    });
  }
  // Before rows are appended: is the user looking at the tail?
  bool at_end() const {
    const long n = count();
    return n == 0 || top_ + visible_rows() >= n;
  }
  void sync(bool follow) {
    const long n = count();
    if (sel_ >= n) sel_ = -1;
    if (follow && n > 0) top_ = std::max(0L, n - visible_rows());
    top_ = std::clamp(top_, 0L, std::max(0L, n - visible_rows()));
    update_scrollbar();
    Refresh(false);
  }

 private:
  long count() const { return static_cast<long>(rows_->size()); }
  int row_h() const { return GetCharHeight() + FromDIP(5); }
  int head_h() const { return row_h() + FromDIP(2); }
  int visible_rows() const { return std::max(1, (GetClientSize().y - head_h()) / row_h()); }
  long row_at(int y) const {
    if (y < head_h()) return -1;
    const long r = top_ + (y - head_h()) / row_h();
    return r < count() ? r : -1;
  }
  void select(long r) {
    sel_ = r;
    if (sel_ < top_) scroll_to(sel_);
    else if (sel_ >= top_ + visible_rows()) scroll_to(sel_ - visible_rows() + 1);
    else Refresh(false);
  }
  void scroll_to(long pos) {
    pos = std::clamp(pos, 0L, std::max(0L, count() - visible_rows()));
    if (pos == top_) return Refresh(false);
    top_ = pos;
    update_scrollbar();
    Refresh(false);
  }
  void update_scrollbar() {
    SetScrollbar(wxVERTICAL, static_cast<int>(top_), visible_rows(), static_cast<int>(count()));
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
    // #, Offset, Gap, Size right-aligned; the message fills the rest.
    const int w[4] = {FromDIP(52), FromDIP(86), FromDIP(80), FromDIP(70)};
    dc.SetFont(GetFont());
    const int ch = dc.GetCharHeight();
    if (sel_ >= top_ && sel_ < last) {
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.SetBrush(wxBrush(sel));
      dc.DrawRectangle(0, hh + static_cast<int>(sel_ - top_) * rh, cs.x, rh);
    }
    for (long i = top_; i < last; ++i) {
      const MsgRow& m = (*rows_)[static_cast<size_t>(i)];
      const int y = hh + static_cast<int>(i - top_) * rh + (rh - ch) / 2;
      const wxString cells[4] = {wxString::Format("%ld", i + 1), U("+" + fmt::ms(m.at_ms)), U(fmt::ms(m.gap_ms)),
                                 U(fmt::human_bytes(m.bytes))};
      int x = 0;
      for (int c = 0; c < 4; ++c) {
        dc.SetTextForeground(c == 0 ? th.dim : th.fg);
        dc.DrawText(cells[c], x + w[c] - pad - dc.GetTextExtent(cells[c]).x, y);
        x += w[c];
      }
      dc.SetTextForeground(m.error ? th.err : th.fg);
      dc.SetClippingRegion(x, hh, std::max(0, cs.x - x), std::max(0, cs.y - hh));
      // At most what can fit (a character is at least ~4 px wide).
      const size_t fit = static_cast<size_t>(std::max(16, (cs.x - x) / FromDIP(4)));
      dc.DrawText(wxString::FromUTF8(m.preview.data(), std::min(m.preview.size(), fit)), x + pad, y);
      dc.DestroyClippingRegion();
    }
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(head_bg));
    dc.DrawRectangle(0, 0, cs.x, hh);
    dc.SetPen(wxPen(th.grid));
    dc.DrawLine(0, hh - 1, cs.x, hh - 1);
    dc.SetFont(GetFont().Bold());
    dc.SetTextForeground(th.fg);
    static const char* kHead[] = {"#", "Offset", "Gap", "Size"};
    int x = 0;
    const int hy = (hh - dc.GetCharHeight()) / 2;
    for (int c = 0; c < 4; ++c) {
      dc.DrawText(kHead[c], x + w[c] - pad - dc.GetTextExtent(kHead[c]).x, hy);
      x += w[c];
      dc.DrawLine(x - 1, FromDIP(4), x - 1, hh - FromDIP(4));
    }
    dc.DrawText("Message", x + pad, hy);
    if (n == 0) {
      dc.SetFont(GetFont());
      dc.SetTextForeground(th.dim);
      dc.DrawText("No messages.", pad, hh + pad);
    }
  }
  const std::vector<MsgRow>* rows_;
  std::function<void(size_t)> on_open_;
  wxColour bg_;
  long top_ = 0;
  long sel_ = -1;
  long tip_row_ = -2;
};

class MsgPage : public LazyPage {
 public:
  MsgPage(wxWindow* parent, std::function<void(size_t)> on_open) : LazyPage(parent), on_open_(std::move(on_open)) {}
  void reset() {
    rows_.clear();
    if (list_) list_->sync(false);
  }
  void append(const GrpcMessageEvent* m, size_t n) {
    // Follow the tail only while the user is looking at it.
    const bool follow = list_ && list_->at_end();
    rows_.reserve(rows_.size() + n);
    for (size_t i = 0; i < n; ++i) {
      MsgRow r;
      r.at_ms = m[i].at_ms;
      r.gap_ms = rows_.empty() ? m[i].at_ms : m[i].at_ms - rows_.back().at_ms;
      r.bytes = m[i].wire_bytes;
      r.error = !m[i].decode_error.empty();
      const std::string& src = r.error ? m[i].decode_error : m[i].json;
      // One-line preview: whitespace runs collapsed, at most 160 characters.
      r.preview.reserve(std::min<size_t>(src.size(), 160));
      bool ws = false;
      for (char c : src) {
        if (r.preview.size() >= 160) {
          r.preview += "\xE2\x80\xA6";
          break;
        }
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
          ws = true;
          continue;
        }
        if (ws && !r.preview.empty()) r.preview += ' ';
        ws = false;
        r.preview += c;
      }
      if (r.error) r.preview = "decode error: " + r.preview;
      rows_.push_back(std::move(r));
    }
    if (list_) list_->sync(follow);
  }
  size_t count() const { return rows_.size(); }

 private:
  wxWindow* make() override { return list_ = new MsgList(this, &rows_, on_open_); }
  std::vector<MsgRow> rows_;
  MsgList* list_ = nullptr;
  std::function<void(size_t)> on_open_;
};

// The Log: a curl -v style transcript in a light read-only Scintilla control (a RichEdit text
// control costs ~20 ms to create), one style per kind of line.
class LogPage : public LazyPage {
 public:
  explicit LogPage(wxWindow* parent) : LazyPage(parent) {}
  void set(std::string text) {
    text_ = std::move(text);
    tail_.clear();
    if (stc_) fill();
  }
  // Lines known only after the checks ran (the captures' variable writes), after the transcript.
  void set_tail(std::string tail) {
    if (tail == tail_) return;
    tail_ = std::move(tail);
    if (stc_) fill();
  }

 private:
  enum { kInfo = 1, kSent = 2, kRecv = 3, kConsole = 4, kAlert = 5 };
  wxWindow* make() override {
    const Theme& th = Theme::get();
    stc_ = new wxStyledTextCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    const wxFont mono(wxFontInfo(9).Family(wxFONTFAMILY_TELETYPE).FaceName("Consolas"));
    stc_->StyleSetFont(wxSTC_STYLE_DEFAULT, mono);
    stc_->StyleSetBackground(wxSTC_STYLE_DEFAULT, th.bg);
    stc_->StyleSetForeground(wxSTC_STYLE_DEFAULT, th.fg);
    stc_->StyleClearAll();
    stc_->StyleSetForeground(kInfo, th.dim);
    stc_->StyleSetForeground(kSent, th.accent);
    stc_->StyleSetForeground(kRecv, th.fg);
    stc_->StyleSetForeground(kConsole, th.fg);
    stc_->StyleSetForeground(kAlert, th.err);
    stc_->SetCaretForeground(th.fg);
    stc_->SetSelBackground(true, th.dark ? wxColour(38, 79, 120) : wxColour(173, 214, 255));
    for (int m = 0; m < 5; ++m) stc_->SetMarginWidth(m, 0);
    stc_->SetMarginLeft(FromDIP(6));
    stc_->SetUndoCollection(false);
    stc_->SetScrollWidthTracking(true);
    stc_->SetScrollWidth(1);
    // A transcript line (the Timing summary, a long header) wraps under its own indent instead
    // of running off the right edge.
    stc_->SetWrapVisualFlags(wxSTC_WRAPVISUALFLAG_END);
    stc_->SetWrapIndentMode(wxSTC_WRAPINDENT_INDENT);
    stc_->SetLexer(wxSTC_LEX_CONTAINER);  // styled by fill(), never by a lexer
    stc_->Bind(wxEVT_STC_STYLENEEDED, [](wxStyledTextEvent&) {});
    stc_->SetReadOnly(true);
    return stc_;
  }
  void after_make() override { fill(); }
  void fill() {
    stc_->SetReadOnly(false);
    stc_->ClearAll();
    // Scintilla wraps on the UI thread: only a transcript of a usual size (a huge script log
    // keeps its horizontal scroll bar).
    stc_->SetWrapMode(text_.size() < (512u << 10) ? wxSTC_WRAP_WORD : wxSTC_WRAP_NONE);
    const std::string all = tail_.empty() ? std::string() : text_ + tail_;
    const std::string& text = tail_.empty() ? text_ : all;
    stc_->AddTextRaw(text.data(), static_cast<int>(text.size()));
    // One style per line, from its first character: "*" info dim, ">" sent, "<" received,
    // "|" script console output, "!" a warning / error.
    stc_->StartStyling(0);
    size_t at = 0;
    while (at < text.size()) {
      size_t e = text.find('\n', at);
      e = e == std::string::npos ? text.size() : e + 1;
      const char p = text[at];
      stc_->SetStyling(static_cast<int>(e - at), p == '>'   ? kSent
                                                 : p == '<' ? kRecv
                                                 : p == '|' ? kConsole
                                                 : p == '!' ? kAlert
                                                            : kInfo);
      at = e;
    }
    stc_->SetReadOnly(true);
    stc_->GotoPos(0);
  }
  std::string text_;
  std::string tail_;
  wxStyledTextCtrl* stc_ = nullptr;
};

// ------------------------------------------------------------------ timing models

namespace tips {
const char* kPrepare =
    "watt's own request setup: libcurl options, header list, adding the transfer to the multi "
    "handle (send() -> request handed to libcurl).";
const char* kQueueHttp =
    "From the Send click to I/O starting: the UI-to-worker hand-off plus libcurl's internal queue "
    "(URL parse, connection-pool lookup and liveness check).";
const char* kDnsHttp =
    "Name resolution. watt's DNS phase ends when the winning socket opens (happy eyeballs), unlike "
    "curl's time_namelookup, which ends when the resolver answers. 0 for a reused connection.";
const char* kSocket = "Name resolved -> socket created and configured (Postman's \"Socket Initialization\").";
const char* kConnectHttp = "TCP handshake: SYN sent -> connection established.";
const char* kTlsHttp =
    "TLS handshake: TCP connected -> handshake complete (certificate check and ALPN included). "
    "n/a for plain http://.";
const char* kSendHttp = "Request upload: connection ready -> request line, headers and body fully written.";
const char* kWait =
    "Server wait (TTFB): request fully sent -> first response byte. Server processing plus one "
    "network round trip.";
const char* kDownloadHttp =
    "First response byte -> last byte received and decoded (headers, body, content decoding).";
const char* kProcess =
    "libcurl done -> send() returns: returning the connection to the pool and watt's result "
    "collection. curl -w time_total leaves this out; watt counts it so the total is real wall time.";
const char* kTotalHttp =
    "Send click -> complete response available: the wall time you waited. Exactly the sum of the phases.";
const char* kQueueGrpc = "From the Send click until the call started on the engine (UI-to-worker hand-off).";
const char* kDnsGrpc =
    "watt's own name lookup for a new channel. 0 for IP literals and for a channel that was already "
    "connected.";
const char* kConnectGrpc =
    "Channel setup + TCP connect this call waited for (plaintext: also the HTTP/2 SETTINGS exchange).";
const char* kTlsGrpc = "TCP connected -> channel READY: the TLS handshake and HTTP/2 SETTINGS. n/a for plaintext.";
const char* kReflect =
    "Server reflection before the call: channel READY -> schema resolved (only on the first call "
    "without .proto files).";
const char* kSendGrpc = "Request message(s) serialised and written to the stream.";
const char* kDownloadGrpc = "First response message -> trailers received (all messages decoded).";
const char* kWaitGrpc =
    "Server wait (TTFB): request sent -> response headers (the first HTTP/2 HEADERS frame). The "
    "first message comes after them; the Messages tab's offsets count from the call start (Send "
    "click), so the first offset is Queue + DNS + Connect + TLS + Send + Wait and the time to "
    "that message.";
const char* kOther = "Time inside the call not attributed to any phase above.";
const char* kTotalGrpc = "Send click -> final status received: the wall time you waited.";
}  // namespace tips

// Phase colours: setup greys, connection blues/purple, server wait green, transfer amber/blue.
struct Pal {
  wxColour setup, dns, socket, connect, tls, send, wait, down, redirect;
};
Pal palette() {
  const bool d = Theme::get().dark;
  return d ? Pal{wxColour(130, 130, 140), wxColour(40, 190, 190), wxColour(90, 160, 210), wxColour(70, 130, 240),
                 wxColour(170, 110, 240), wxColour(235, 175, 60), wxColour(80, 200, 120), wxColour(90, 170, 255),
                 wxColour(240, 140, 70)}
           : Pal{wxColour(150, 150, 160), wxColour(0, 150, 150), wxColour(60, 130, 190), wxColour(40, 100, 220),
                 wxColour(140, 80, 210), wxColour(215, 150, 20), wxColour(30, 160, 80), wxColour(40, 130, 230),
                 wxColour(220, 110, 40)};
}

std::string host_port(const std::string& ip, long port);

// pre: the request's pre-request script, which ran on the worker inside the shell's Queue time:
// shown as its own row and taken out of Queue.
TimingModel http_model(const HttpResponse& r, bool plain_http, const ScriptRun* pre = nullptr,
                       const ScriptRun* post = nullptr, const SentHttp::TokenStep* token = nullptr) {
  const Timing& t = r.timing;
  const double pre_ms = pre && pre->ran ? std::clamp(pre->elapsed_us / 1000.0, 0.0, std::max(0.0, t.queue_ms)) : 0.0;
  // The OAuth 2.0 token step ran on the worker inside Queue too: its own row, out of Queue.
  const double tok_ms = token ? std::clamp(token->ms, 0.0, std::max(0.0, t.queue_ms - pre_ms)) : 0.0;
  const double queue_ms = t.queue_ms - pre_ms - tok_ms;
  const Pal p = palette();
  const bool reused = t.connection_reused;
  TimingModel m;
  m.valid = true;
  m.kind = "http";
  m.reused = reused;
  m.conn_note = reused ? "reused connection" : "new connection";
  if (!r.tls_version.empty()) m.conn_note += " \xC2\xB7 " + r.tls_version;
  m.total = t.total_ms;
  m.server_wait = t.ttfb_ms;
  for (const RedirectHop& h : r.hops) m.server_wait += h.ttfb_ms;  // each hop's own server wait
  if (!r.hops.empty()) {
    m.server_tip = "Wait (TTFB) of the final request plus the wait of every redirect hop: server processing "
                   "plus one network round trip each.";
  }
  m.total_tip = tips::kTotalHttp;
  auto add = [&](const char* name, double v, const char* tip, wxColour c, bool conn_phase) {
    Phase ph{name, v, tip, c};
    ph.reused = conn_phase && reused && v <= 0;
    m.phases.push_back(std::move(ph));
  };
  add("Prepare", r.prepare_ms, tips::kPrepare, p.setup, false);
  add("Queue", queue_ms, tips::kQueueHttp, p.setup, false);
  add("DNS", t.dns_ms, tips::kDnsHttp, p.dns, true);
  add("Socket", r.socket_ms, tips::kSocket, p.socket, true);
  add("Connect", t.connect_ms, tips::kConnectHttp, p.connect, true);
  add("TLS", t.tls_ms, tips::kTlsHttp, p.tls, true);
  if (plain_http && t.tls_ms <= 0 && !reused) m.phases.back().has_value = false;
  add("Send", t.send_ms, tips::kSendHttp, p.send, false);
  add("Wait (TTFB)", t.ttfb_ms, tips::kWait, p.wait, false);
  add("Download", t.download_ms, tips::kDownloadHttp, p.down, false);
  if (r.redirect_count > 0 || r.redirect_ms > 0) {
    std::string tip = "Every followed redirect before the final request (" + std::to_string(r.redirect_count) +
                      (r.redirect_count == 1 ? " hop):" : " hops):");
    for (const RedirectHop& h : r.hops)
      tip += "\n  " + std::to_string(h.status) + " " + h.url + "  " + fmt::ms(h.total_ms) +
             (h.connection_reused ? " (reused)" : "");
    // Insert before Prepare's successors would mislead the waterfall order: redirects happen
    // before the final request's phases, so they go first after Prepare.
    // To keep the waterfall at its 10 reserved rows (nothing below jumps when a redirecting
    // response arrives), Prepare and Queue share one row then: both are watt's own setup time.
    Phase& prep = m.phases[0];
    prep.name = "Prep+queue";
    prep.ms = r.prepare_ms + queue_ms;
    prep.tip = std::string("Prepare (request setup) ") + fmt::ms(r.prepare_ms) + " + Queue (UI-to-worker hand-off) " +
               fmt::ms(queue_ms) + ".\n" + tips::kPrepare;
    m.phases.erase(m.phases.begin() + 1);
    Phase ph{"Redirects", r.redirect_ms, tip, p.redirect};
    // One row per hop when expanded (click the Redirects row).
    for (size_t i = 0; i < r.hops.size(); ++i) {
      const RedirectHop& h = r.hops[i];
      std::string htip = std::to_string(h.status) + " " + h.url + "\n";
      htip += h.connection_reused ? "reused connection" : "DNS " + fmt::ms(h.dns_ms) + ", connect " + fmt::ms(h.connect_ms) +
                                                             ", TLS " + fmt::ms(h.tls_ms);
      htip += ", send " + fmt::ms(h.send_ms) + ", wait " + fmt::ms(h.ttfb_ms) + ", download " + fmt::ms(h.download_ms);
      if (!h.remote_ip.empty()) htip += "\n" + host_port(h.remote_ip, h.remote_port);
      std::string path = h.url;
      if (const size_t sch = path.find("://"); sch != std::string::npos) {
        const size_t sl = path.find('/', sch + 3);
        path = sl == std::string::npos ? "/" : path.substr(sl);
      }
      if (path.size() > 18) path = path.substr(0, 17) + "\xE2\x80\xA6";
      Phase hp{std::to_string(h.status) + " " + path, h.total_ms, htip, p.redirect};
      hp.reused = false;
      ph.sub.push_back(std::move(hp));
    }
    m.phases.insert(m.phases.begin() + 1, std::move(ph));
  }
  add("Process", r.process_ms, tips::kProcess, p.setup, false);
  if (pre_ms > 0) {
    // Its own row, first (it runs before the request is built). Prepare and Queue share a row
    // then (as with redirects) so the waterfall keeps its reserved height.
    if (m.phases.size() > 1 && m.phases[1].name == "Queue") {
      Phase& prep = m.phases[0];
      prep.name = "Prep+queue";
      prep.ms = r.prepare_ms + queue_ms;
      prep.tip = std::string("Prepare (request setup) ") + fmt::ms(r.prepare_ms) + " + Queue (UI-to-worker hand-off) " +
                 fmt::ms(queue_ms) + ".\n" + tips::kPrepare;
      m.phases.erase(m.phases.begin() + 1);
    }
    std::string tip = "The request's pre-request script (QuickJS sandbox, on the worker thread, before the request "
                      "is built): " + fmt::ms(pre_ms) + " in all";
    if (pre->setup_us > 0) tip += ", of which sandbox setup " + fmt::ms(pre->setup_us / 1000.0);
    if (pre->wait_us > 0) tip += ", waiting in sendRequest / sleep " + fmt::ms(pre->wait_us / 1000.0);
    tip += ". Its console output and tests are in the Log and Tests tabs.";
    m.phases.insert(m.phases.begin(), Phase{"Pre-script", pre_ms, tip, wxColour(170, 110, 190)});
    // Not part of the response time (as the post-response script is not): Total is the request's
    // own time and the script is named next to it, as Postman and Bruno leave both out.
    m.before = pre_ms;
    m.total = t.total_ms - pre_ms;
  }
  if (token) {
    if (m.phases.size() > 1 && m.phases[1].name == "Queue") {  // keep the reserved row count
      Phase& prep = m.phases[0];
      prep.name = "Prep+queue";
      prep.ms = r.prepare_ms + queue_ms;
      prep.tip = std::string("Prepare (request setup) ") + fmt::ms(r.prepare_ms) + " + Queue (UI-to-worker hand-off) " +
                 fmt::ms(queue_ms) + ".\n" + tips::kPrepare;
      m.phases.erase(m.phases.begin() + 1);
    }
    std::string tip = "OAuth 2.0 (" + token->grant + (token->from.empty() ? std::string() : ", auth from " + token->from) +
                      "): ";
    tip += token->from_cache ? "the cached token, no request: " + fmt::ms(tok_ms)
           : token->refreshed ? "refreshed the token at " + token->token_url + ": " + fmt::ms(tok_ms)
                              : "token request to " + token->token_url + ": " + fmt::ms(tok_ms);
    tip += ", before the request was built. Details in the Log tab.";
    // After a pre-script row (the script runs first), else first.
    const size_t at = !m.phases.empty() && m.phases[0].name == "Pre-script" ? 1 : 0;
    m.phases.insert(m.phases.begin() + static_cast<std::ptrdiff_t>(at),
                    Phase{token->from_cache ? "Auth (cached)" : "Auth / token", tok_ms, tip, wxColour(200, 150, 60)});
  }
  if (post && post->ran && post->elapsed_us > 0) {
    // After the total: the response was complete when the post-response script started.
    if (m.phases.size() > 1 && m.phases[1].name == "Queue") {  // keep the reserved row count
      Phase& prep = m.phases[0];
      prep.name = "Prep+queue";
      prep.ms = r.prepare_ms + queue_ms;
      prep.tip = std::string("Prepare (request setup) ") + fmt::ms(r.prepare_ms) + " + Queue (UI-to-worker hand-off) " +
                 fmt::ms(queue_ms) + ".\n" + tips::kPrepare;
      m.phases.erase(m.phases.begin() + 1);
    }
    const double post_ms = post->elapsed_us / 1000.0;
    std::string tip = "The request's post-response script (QuickJS sandbox, on the worker thread), run once the "
                      "response was complete: " + fmt::ms(post_ms) + ", not part of the response time (Total)";
    if (post->setup_us > 0) tip += "; sandbox setup " + fmt::ms(post->setup_us / 1000.0);
    tip += ". Its tests and console output are in the Tests and Log tabs.";
    m.phases.push_back(Phase{"Post-script", post_ms, tip, wxColour(170, 110, 190)});
    m.after = post_ms;
  }
  // The token step is network time to another endpoint, not watt's: its own figure.
  m.client = std::max(0.0, r.prepare_ms + (t.queue_ms - pre_ms - tok_ms) + r.process_ms);
  if (token) {
    m.auth = tok_ms;
    m.auth_tip = token->from_cache ? "OAuth 2.0: the cached access token was used, no token request: " + fmt::ms(tok_ms) + "."
                                   : "OAuth 2.0 token step before the request (" +
                                         std::string(token->refreshed ? "refresh at " : "token request to ") +
                                         token->token_url + "): " + fmt::ms(tok_ms) + ", not counted as client time.";
  }
  m.network = std::max(0.0, m.total - m.server_wait - m.client - std::max(0.0, m.auth));
  m.client_tip = "watt's own time: Prepare (request setup) + Queue (UI-to-worker hand-off, libcurl's queue) + "
                 "Process (result collection) = " + fmt::ms(r.prepare_ms) + " + " + fmt::ms(queue_ms) + " + " +
                 fmt::ms(r.process_ms) + ".";
  if (pre_ms > 0) m.client_tip += " Before the request: the pre-request script, " + fmt::ms(pre_ms) + " (not in the total).";
  if (token) m.client_tip += " Without the OAuth 2.0 token step (" + fmt::ms(tok_ms) + ", shown as auth).";
  if (m.after > 0)
    m.client_tip += " After the response: the post-response script, " + fmt::ms(m.after) + " (not in the total).";
  m.network_tip = "Time on the network: DNS, socket, TCP connect, TLS, upload and download";
  m.network_tip += r.hops.empty() ? std::string(".") : std::string(", plus all but the server wait of each redirect hop.");
  m.network_tip += " Neither watt nor the server thinking.";
  return m;
}

// UNAVAILABLE with nothing at all from a server (no messages, no response headers, no
// trailers): the call never reached one.
bool grpc_never_answered(const GrpcResponse& r) {
  return r.status_code == 14 && r.messages.empty() && r.headers.empty() && r.trailers.empty();
}

// A user-paced (Manual) stream: one clock from the stream opening, as the message offsets. The
// time it sat open at the user's pace is its own span (idle before #1, then the user's sends
// until End), never Send or server time; the server's part is End (or the last send) -> trailers.
TimingModel grpc_paced_model(const GrpcResponse& r) {
  const Timing& t = r.timing;
  const GrpcResponse::Pacing& pc = r.pacing;
  const Pal p = palette();
  const bool reused = t.connection_reused;
  TimingModel m;
  m.valid = true;
  m.kind = "grpc-paced";
  m.reused = reused;
  m.conn_note = reused ? "reused channel" : "new channel";
  m.conn_note += " \xC2\xB7 user-paced";
  m.total = t.total_ms;
  const double setup = t.queue_ms + t.dns_ms + t.connect_ms + t.tls_ms + r.reflect_ms;
  const double tail = std::clamp(pc.tail_ms, 0.0, std::max(0.0, t.total_ms - setup));
  const double open_end = std::max(setup, t.total_ms - tail);  // the client's last action
  const double first = pc.first_send_ms >= 0 ? pc.first_send_ms : open_end;
  const double idle = std::clamp(first - setup, 0.0, open_end - setup);
  const double sends = std::max(0.0, open_end - setup - idle);
  auto add = [&](const char* name, double v, const std::string& tip, wxColour c, bool conn_phase) {
    Phase ph{name, v, tip, c};
    ph.reused = conn_phase && reused && v <= 0;
    m.phases.push_back(std::move(ph));
  };
  add("Queue", t.queue_ms, tips::kQueueGrpc, p.setup, false);
  add("DNS", t.dns_ms, tips::kDnsGrpc, p.dns, true);
  add("Connect", t.connect_ms, tips::kConnectGrpc, p.connect, true);
  add("TLS", t.tls_ms, tips::kTlsGrpc, p.tls, true);
  if (t.tls_ms <= 0 && !reused) m.phases.back().has_value = false;
  if (r.reflect_ms > 0) add("Reflection", r.reflect_ms, tips::kReflect, p.redirect, false);
  const wxColour paced_c = Theme::get().dark ? wxColour(190, 150, 70) : wxColour(190, 130, 30);
  add("Open, idle", idle,
      "Manual stream: open and idle, waiting for you to send the first message (" +
          (pc.first_send_ms >= 0 ? "#1 went out at +" + fmt::ms(pc.first_send_ms) : std::string("none was sent")) +
          "). Your pace, not call time.",
      Theme::get().dark ? wxColour(130, 110, 70) : wxColour(225, 195, 140), false);
  m.phases.back().has_value = true;
  add("User-paced", sends,
      std::to_string(pc.sent) + (pc.sent == 1 ? " message" : " messages") +
          " sent by you, from #1 to " + (pc.end_ms >= 0 ? std::string("End") : std::string("the last one")) +
          ": the stream is open at your pace while replies arrive (see the Messages tab for each "
          "reply's offset). Not server time.",
      paced_c, false);
  const std::string tail_name = pc.end_ms >= 0 ? "End \xE2\x86\x92 trailers" : "Last \xE2\x86\x92 trailers";
  add(tail_name.c_str(), tail,
      (pc.end_ms >= 0 ? std::string("End (half-close) at +") + fmt::ms(pc.end_ms)
                      : pc.last_send_ms >= 0 ? "Your last message at +" + fmt::ms(pc.last_send_ms)
                                             : std::string("The stream opening")) +
          " -> the server's final status: the part of the call you did not pace. res.responseTime "
          "evaluates this for a Manual stream.",
      p.wait, false);
  double sum = 0;
  for (const Phase& ph : m.phases) sum += ph.ms;
  const double other = t.total_ms - sum > 0.005 ? t.total_ms - sum : 0;
  add("Other", other, tips::kOther, p.setup, false);
  m.server_wait = tail;
  m.server_label = "server";
  m.server_tip = "End (or your last message) -> trailers: the server finishing the stream once you stopped "
                 "pacing it.";
  m.paced = idle + sends;
  m.paced_tip = "The stream sat open at your pace: idle before #1 (" + fmt::ms(idle) + ") plus your sends until " +
                (pc.end_ms >= 0 ? std::string("End") : std::string("the last one")) + " (" + fmt::ms(sends) +
                "). Neither the network nor the server.";
  m.client = t.queue_ms + other;
  m.network = t.dns_ms + t.connect_ms + t.tls_ms + r.reflect_ms;
  m.client_tip = "watt's own time: Queue (UI-to-worker hand-off) + Other (unattributed time inside the call).";
  m.network_tip = "DNS, connect, TLS and reflection: opening the stream.";
  m.total_tip = "Manual stream: stream opened (Ctrl+Enter) -> final status, the same zero as the message "
                "offsets. Most of it is your pace (user-paced).";
  return m;
}

TimingModel grpc_model(const GrpcResponse& r) {
  if (r.pacing.on && !grpc_never_answered(r)) return grpc_paced_model(r);
  const Timing& t = r.timing;
  const Pal p = palette();
  const bool reused = t.connection_reused;
  TimingModel m;
  m.valid = true;
  m.kind = "grpc";
  m.reused = reused;
  m.conn_note = reused ? "reused channel" : "new channel";
  m.total = t.total_ms;
  m.server_wait = t.ttfb_ms;
  if (r.messages.size() > 1) {
    // A stream's Download phase is paced by the server (first message -> trailers): server time.
    m.server_wait = t.ttfb_ms + t.download_ms;
    m.server_label = "server + stream";
    m.server_tip = "Wait (TTFB) + Download: for a stream the server decides when each message and the "
                   "trailers come, so the whole stream counts as server time. Client overhead is the rest "
                   "(queue, DNS, connect, TLS, reflection, send).";
  }
  m.total_tip = tips::kTotalGrpc;
  auto add = [&](const char* name, double v, const char* tip, wxColour c, bool conn_phase) {
    Phase ph{name, v, tip, c};
    ph.reused = conn_phase && reused && v <= 0;
    m.phases.push_back(std::move(ph));
  };
  add("Queue", t.queue_ms, tips::kQueueGrpc, p.setup, false);
  add("DNS", t.dns_ms, tips::kDnsGrpc, p.dns, true);
  add("Connect", t.connect_ms, tips::kConnectGrpc, p.connect, true);
  add("TLS", t.tls_ms, tips::kTlsGrpc, p.tls, true);
  if (t.tls_ms <= 0 && !reused) m.phases.back().has_value = false;
  if (r.reflect_ms > 0) add("Reflection", r.reflect_ms, tips::kReflect, p.redirect, false);
  add("Send", t.send_ms, tips::kSendGrpc, p.send, false);
  add("Wait (TTFB)", t.ttfb_ms, tips::kWaitGrpc, p.wait, false);
  add("Download", t.download_ms, tips::kDownloadGrpc, p.down, false);
  double sum = 0;
  for (const Phase& ph : m.phases) sum += ph.ms;
  double other = t.total_ms - sum > 0.005 ? t.total_ms - sum : 0;
  // Always a row (0 when nothing is left over), so a failed call and an OK one have the same
  // height and the tabs below do not jump when a stream finishes.
  add("Other", other, tips::kOther, p.setup, false);
  if (grpc_never_answered(r)) {
    // The call ended before any server answered (UNAVAILABLE with no response headers: nothing
    // to connect to, WSA 10049/10061, ...). gRPC connects lazily when the call starts, so the
    // engine's "send" / "wait" timers really measured the connection attempt: that time goes to
    // the connection phase the call stopped in (marked as failed), and the phases it never reached
    // are n/a. Nothing of it is server time.
    double moved = 0;
    for (Phase& ph : m.phases) {
      const bool unreached = ph.name == "Wait (TTFB)" || ph.name == "Download" || ph.name == "Send";
      if (unreached) {
        moved += ph.ms;
        ph.has_value = false;
        ph.reused = false;
        ph.ms = 0;
      }
    }
    if (!reused) {
      Phase* fail = nullptr;
      for (auto it = m.phases.rbegin(); it != m.phases.rend() && !fail; ++it)
        if ((it->name == "Connect" || it->name == "TLS" || it->name == "DNS") && it->ms > 0) fail = &*it;
      if (!fail)
        for (Phase& ph : m.phases)
          if (ph.name == "Connect") fail = &ph;
      if (fail) {
        fail->failed = true;
        fail->has_value = true;
        fail->reused = false;
        fail->ms += moved;
        fail->tip = std::string(fail->tip) + "\nThe call failed here: no server answered (the time includes the "
                                             "connection attempt the engine timed as send / wait).";
      }
    } else {
      for (Phase& ph : m.phases)
        if (ph.name == "Other") ph.ms += moved;
      other += moved;
    }
    m.server_wait = 0;
    m.server_label = "server";
    m.server_tip = "No server answered this call: no server time.";
    m.conn_note = reused ? "reused channel, no reply" : "no connection";
  }
  m.client = t.queue_ms + other;
  m.network = std::max(0.0, t.total_ms - m.server_wait - m.client);
  m.client_tip = "watt's own time: Queue (UI-to-worker hand-off) + Other (unattributed time inside the call).";
  m.network_tip = r.messages.size() > 1
                      ? "DNS, connect, TLS, reflection and sending the request: time on the network before the "
                        "server's stream started."
                      : "DNS, connect, TLS, reflection, sending the request and receiving the reply: time on the "
                        "network, neither watt nor the server thinking.";
  return m;
}

Tone http_tone(long s) {
  if (s >= 200 && s < 300) return Tone::Ok;
  if (s >= 300 && s < 400) return Tone::Redirect;
  if (s >= 400 && s < 500) return Tone::Warn;
  if (s >= 500) return Tone::Err;
  return Tone::Plain;
}

std::string host_port(const std::string& ip, long port) {
  if (ip.empty()) return {};
  std::string s = ip.find(':') != std::string::npos ? "[" + ip + "]" : ip;
  if (port) s += ":" + std::to_string(port);
  return s;
}

// "2026-01-15 23:59:59 UTC" or ISO "2026-01-15T23:59:59Z" -> whole days from now (negative:
// expired); false if unparsable.
bool days_left(const std::string& utc, long* days) {
  std::string t = utc.substr(0, 19);
  if (t.size() > 10 && t[10] == 'T') t[10] = ' ';
  wxDateTime d;
  wxString::const_iterator end;
  const wxString s = wxString::FromUTF8(t);
  if (!d.ParseFormat(s, "%Y-%m-%d %H:%M:%S", &end)) return false;
  d.MakeFromUTC();
  *days = static_cast<long>(std::floor((d - wxDateTime::Now()).GetSeconds().ToDouble() / 86400.0));
  return true;
}

// "2026-01-15T08:00:00Z" -> "2026-01-15 08:00 UTC" (anything else unchanged).
std::string iso_short(const std::string& iso) {
  if (iso.size() >= 16 && iso[10] == 'T') return iso.substr(0, 10) + " " + iso.substr(11, 5) + " UTC";
  return iso;
}

// Rows of the connection popover for an HTTP response: HttpResponse::conn (the connection that
// carried the final request, with its TLS session and leaf certificate), then the redirect chain.
std::vector<InfoRow> http_conn_rows(const HttpResponse& r, const SentHttp* sent) {
  std::vector<InfoRow> rows;
  const ConnectionInfo& c = r.conn;
  const std::string& url = r.effective_url.empty() && sent ? sent->request.url : r.effective_url;
  const bool https = url.rfind("https:", 0) == 0;
  rows.push_back({"Connection", {}, true});
  if (!r.ok) rows.push_back({"Error", r.error, false, true});
  if (!r.http_version.empty())
    rows.push_back({"Protocol", r.http_version + (https ? "  (negotiated via ALPN)" : "  (plain TCP)")});
  if (r.ok) {
    std::string v = c.reused ? "reused (opened by an earlier request; DNS, TCP and TLS skipped)"
                             : "new (opened for this request)";
    if (c.id >= 0) v += "  \xC2\xB7  #" + std::to_string(c.id);
    rows.push_back({"Connection", v});
  }
  const std::string remote = host_port(c.remote_ip.empty() ? r.remote_ip : c.remote_ip,
                                       c.remote_ip.empty() ? r.remote_port : c.remote_port);
  const std::string local = host_port(c.local_ip.empty() ? r.local_ip : c.local_ip,
                                      c.local_ip.empty() ? r.local_port : c.local_port);
  if (!c.proxy.empty())
    rows.push_back({"Proxy", c.proxy + (https ? "  (HTTP CONNECT tunnel: TLS runs end to end through it)"
                                              : "  (the request went to the proxy, which forwarded it)")});
  if (!remote.empty()) rows.push_back({c.proxy.empty() ? "Remote address" : "Remote (proxy)", remote});
  if (!local.empty()) rows.push_back({"Local address", local});
  if (!url.empty()) rows.push_back({"URL", url});
  if (!https) {
    rows.push_back({"TLS", {}, true});
    rows.push_back({"TLS", "none: plain http://, nothing is encrypted"});
  } else {
    rows.push_back({"TLS", {}, true});
    const TlsInfo* ti = c.tls_info.get();
    if (!ti && r.tls_version.empty()) {
      rows.push_back({"TLS", r.ok ? "details not available" : "no TLS session was established", false, !r.ok});
    } else if (!ti) {
      // Older engine path: only the flat mirror fields.
      rows.push_back({"Version", r.tls_version});
      if (!r.tls_cipher.empty()) rows.push_back({"Cipher", r.tls_cipher});
      if (!r.verify_result.empty())
        rows.push_back({"Verification", r.verify_result, false, r.verify_result.rfind("verified", 0) != 0});
      if (!r.cert_subject.empty()) rows.push_back({"Subject", r.cert_subject});
      if (!r.cert_issuer.empty()) rows.push_back({"Issuer", r.cert_issuer});
    } else {
      rows.push_back({"Version", ti->version});
      if (!ti->cipher.empty()) rows.push_back({"Cipher", ti->cipher});
      rows.push_back({"ALPN", ti->alpn.empty() ? std::string("none (server did not negotiate ALPN)") : ti->alpn});
      if (!ti->verify_result.empty())
        rows.push_back({"Verification", ti->verify_result, false, ti->verify_result.rfind("verified", 0) != 0});
      if (c.reused) rows.push_back({"Handshake", "earlier, on this reused connection"});
      if (!ti->client_cert_subject.empty())
        rows.push_back({"Client certificate", ti->client_cert_subject + "  (issued by " + ti->client_cert_issuer +
                                                  "; sent: mutual TLS)"});
      else if (c.client_cert)
        rows.push_back({"Client certificate", "set, but the server did not ask for one (not sent)"});
      const TlsCertificate& L = ti->leaf;
      if (!L.subject.empty() || !L.sha256_fingerprint.empty()) {
        rows.push_back({"Certificate", {}, true});
        if (!L.subject.empty()) rows.push_back({"Subject", L.subject});
        if (!L.subject_alt_names.empty()) {
          std::string sans;
          for (size_t i = 0; i < L.subject_alt_names.size(); ++i) {
            if (i == 24) {
              sans += ", \xE2\x80\xA6 " + std::to_string(L.subject_alt_names.size() - 24) + " more";
              break;
            }
            if (i) sans += ", ";
            sans += L.subject_alt_names[i];
          }
          rows.push_back({"Alt names (" + std::to_string(L.subject_alt_names.size()) + ")", sans});
        }
        if (!L.issuer.empty()) rows.push_back({"Issuer", L.issuer});
        if (!L.not_before.empty()) rows.push_back({"Valid from", iso_short(L.not_before)});
        if (!L.not_after.empty()) {
          std::string v = iso_short(L.not_after);
          long d = 0;
          bool soon = false;
          if (days_left(L.not_after, &d)) {
            v += d < 0 ? "  (expired " + std::to_string(-d) + " days ago)" : "  (" + std::to_string(d) + " days left)";
            soon = d < 14;
          }
          rows.push_back({"Valid until", v, false, soon});
        }
        if (!L.serial.empty()) rows.push_back({"Serial", L.serial});
        if (!L.sha256_fingerprint.empty()) rows.push_back({"SHA-256", L.sha256_fingerprint});
      }
      if (!ti->chain.empty()) {
        rows.push_back({"Chain (" + std::to_string(ti->chain_length) + ")", {}, true});
        size_t at = 0;
        int n = 0;
        while (at < ti->chain.size()) {
          size_t e = ti->chain.find('\n', at);
          if (e == std::string::npos) e = ti->chain.size();
          if (e > at) rows.push_back({"#" + std::to_string(++n), ti->chain.substr(at, e - at)});
          at = e + 1;
        }
      }
    }
  }
  if (!r.hops.empty()) {
    rows.push_back({"Redirects (" + std::to_string(r.hops.size()) + ")", {}, true});
    for (size_t i = 0; i < r.hops.size(); ++i) {
      const RedirectHop& h = r.hops[i];
      std::string v = std::to_string(h.status) + "  " + h.url + "  \xC2\xB7  " + fmt::ms(h.total_ms);
      if (const std::string a = host_port(h.remote_ip, h.remote_port); !a.empty()) v += "  \xC2\xB7  " + a;
      if (h.connection_reused) v += "  \xC2\xB7  reused conn.";
      rows.push_back({"#" + std::to_string(i + 1), v});
    }
    rows.push_back({"Final", std::to_string(r.status) + "  " + r.effective_url});
  }
  return rows;
}

// What the response view knows about a gRPC call's target (from the request as sent).
struct GrpcConnMeta {
  std::string address, authority, server_name;
  bool tls = false, mtls = false, web = false;
};

std::vector<InfoRow> grpc_conn_rows(const GrpcConnMeta& m, const GrpcResponse& r) {
  std::vector<InfoRow> rows;
  rows.push_back({"Channel", {}, true});
  rows.push_back({"Target", m.address.empty() ? std::string("\xE2\x80\x94") : m.address});
  if (!r.peer.empty()) {
    std::string p = r.peer;
    for (const char* pre : {"ipv4:", "ipv6:", "dns:///"})
      for (size_t at; (at = p.find(pre)) != std::string::npos;) p.erase(at, std::strlen(pre));
    std::string list;
    for (char c : p) list += c == ',' ? std::string(", ") : std::string(1, c);
    rows.push_back({"Resolved", list + "   (" + r.peer + ")"});
  } else if (m.web) {
    rows.push_back({"Resolved", "gRPC-Web goes over HTTP: see the HTTP engine"});
  }
  rows.push_back({"Protocol", m.web ? "gRPC-Web over HTTP/1.1 or HTTP/2" : (m.tls ? "gRPC over HTTP/2 (h2, TLS)" : "gRPC over HTTP/2 (h2c, plaintext)")});
  const bool no_conn = r.status_code == 14 && r.messages.empty();
  rows.push_back({"Channel", no_conn ? "not connected" : r.timing.connection_reused
                                                            ? "reused (already READY: no DNS, TCP or TLS)"
                                                            : "new (connected for this call)"});
  if (!m.authority.empty()) rows.push_back({":authority", m.authority});
  rows.push_back({"Security", {}, true});
  if (!m.tls) {
    rows.push_back({"TLS", "none: plaintext, nothing is encrypted"});
  } else {
    rows.push_back({"TLS", m.mtls ? "yes, mutual TLS (client certificate sent)" : "yes (server certificate verified by gRPC)"});
    if (!m.server_name.empty()) rows.push_back({"Server name", m.server_name});
    rows.push_back({"Details", "the gRPC engine does not report the TLS version, cipher or certificate", false, false});
  }
  return rows;
}

// Where a failed HTTP request stopped, from libcurl's error text first (a rejected URL or an
// unsupported scheme never resolves anything, whatever the timer says) and the timings second.
enum class FailAt { Url, Dns, Connect, Tls, Wait, Download, Other };
struct Failure {
  FailAt at;
  const char* text;
};
Failure classify_failure(const HttpResponse& r) {
  std::string e = r.error;
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  auto has = [&](const char* k) { return e.find(k) != std::string::npos; };
  const Timing& t = r.timing;
  if (has("resolv")) return {FailAt::Dns, "Failed during name resolution (DNS)."};
  if ((has("protocol") && (has("not supported") || has("unsupported"))) || has("url rejected") ||
      has("malformed") || has("bad/illegal") || has("no url") || has("port number"))
    return {FailAt::Url, "The URL was rejected before anything was resolved, connected or sent."};
  if (has("ssl") || has("tls") || has("schannel") || has("certificate"))
    return {FailAt::Tls, "Failed during the TLS handshake."};
  if (has("connection timed out") || has("connect")) return {FailAt::Connect, "Failed while connecting (TCP)."};
  if (t.download_ms > 0) return {FailAt::Download, "Failed while receiving the response."};
  if (t.ttfb_ms > 0 || t.send_ms > 0) return {FailAt::Wait, "Failed while sending or waiting for the server."};
  if (t.tls_ms > 0) return {FailAt::Tls, "Failed during the TLS handshake."};
  if (t.connect_ms > 0) return {FailAt::Connect, "Failed while connecting (TCP)."};
  if (t.dns_ms > 0) return {FailAt::Dns, "Failed during name resolution or connect."};
  return {FailAt::Other, "Failed before the request could be sent."};
}

// A failed request's waterfall: the phase it failed in is marked, the ones it never reached are
// n/a (no bar, no value) instead of whatever the timers held.
void mark_failure(TimingModel& m, const HttpResponse& r) {
  const Failure f = classify_failure(r);
  const char* failed = nullptr;
  switch (f.at) {
    case FailAt::Url: failed = ""; break;  // nothing on the network ran
    case FailAt::Dns: failed = "DNS"; break;
    case FailAt::Connect: failed = "Connect"; break;
    case FailAt::Tls: failed = "TLS"; break;
    case FailAt::Wait: failed = "Wait (TTFB)"; break;
    case FailAt::Download: failed = "Download"; break;
    case FailAt::Other: return;
  }
  static const char* const kNet[] = {"DNS", "Socket", "Connect", "TLS", "Send", "Wait (TTFB)", "Download"};
  bool after = *failed == 0;
  for (const char* name : kNet) {
    for (Phase& ph : m.phases) {
      if (ph.name != name) continue;
      if (after) {
        ph.has_value = false;
        ph.reused = false;
        ph.ms = 0;
      } else if (ph.name == failed) {
        ph.failed = true;
        ph.has_value = true;
        ph.reused = false;
      }
    }
    if (std::string_view(name) == failed) after = true;
  }
  m.conn_note = f.at == FailAt::Url ? "not sent" : "failed";
  m.reused = false;
  // The footer split follows: time in phases that never ran is watt's (client), not network.
  double net = 0;
  for (const Phase& ph : m.phases)
    for (const char* name : kNet)
      if (ph.has_value && ph.name == name && ph.name != "Wait (TTFB)") net += ph.ms;
  // Failed before the request was sent (DNS, connect, TLS): no server time at all, "n/a" (not
  // "server wait 0.000 ms", which reads as an instant answer).
  if (f.at == FailAt::Url || f.at == FailAt::Dns || f.at == FailAt::Connect || f.at == FailAt::Tls) {
    m.server_wait = 0;
    m.server_na = true;
    m.server_tip = "The request failed before it was sent: no server was reached, so there is no server time.";
  }
  m.network = net;
  m.client = std::max(0.0, m.total - m.server_wait - net);
  m.client_tip = "watt's own time, including the time before the request failed that no phase above covers.";
}

// curl's message counts whole milliseconds from its own start ("Failed to connect to 127.0.0.1
// port 9 after 0 ms: ..."), next to a total of 1.17 ms: the view says the total it shows.
std::string shown_error(const HttpResponse& r) {
  std::string e = r.error;
  const size_t a = e.find(" after ");
  if (a == std::string::npos) return e;
  size_t k = a + 7;
  const size_t digits = k;
  while (k < e.size() && e[k] >= '0' && e[k] <= '9') ++k;
  if (k == digits || e.compare(k, 3, " ms") != 0) return e;
  e.replace(a, k + 3 - a, " after " + fmt::ms(r.timing.total_ms));
  return e;
}

// Cheap identity of a request body: its size plus a hash of the first and last 64 KB.
std::string body_tag(std::string_view b) {
  if (b.empty()) return {};
  const size_t k = 64 * 1024;
  size_t h = std::hash<std::string_view>()(b.substr(0, k));
  if (b.size() > k) h ^= std::hash<std::string_view>()(b.substr(b.size() - std::min(k, b.size() - k))) * 31;
  return " #" + std::to_string(b.size()) + ":" + std::to_string(h);
}

// "GET https://example.com/a/very/long/path" -> at most n characters, elided in the middle.
std::string elide_middle(const std::string& s, size_t n);

// Short name of the request a response belongs to, for the status strip: "GET /json/20",
// "Echo/Count" (full label in the tooltip).
std::string request_tag(const std::string& label) {
  std::string t = label;
  if (const size_t at = t.find(" @ "); at != std::string::npos) {  // gRPC "pkg.Svc/Method @ host"
    t.resize(at);
    const size_t slash = t.rfind('/');
    const size_t dot = t.rfind('.', slash == std::string::npos ? std::string::npos : slash);
    if (dot != std::string::npos) t = t.substr(dot + 1);
    return elide_middle(t, 40);
  }
  // HTTP "METHOD scheme://host/path?q": method + host + path (no scheme).
  const size_t sp = t.find(' ');
  const size_t sch = t.find("://");
  if (sp != std::string::npos && sch != std::string::npos) {
    const size_t path = t.find('/', sch + 3);
    t = t.substr(0, sp) + " " + t.substr(sch + 3) + (path == std::string::npos ? "/" : "");
  }
  return elide_middle(t, 40);
}

// Line breaks (and the indentation after them) of an error text joined into one line.
std::string one_line(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  bool brk = false;
  for (char c : s) {
    if (c == '\r' || c == '\n') {
      brk = true;
      continue;
    }
    if (brk && (c == ' ' || c == '\t')) continue;
    if (brk && !o.empty() && o.back() != ' ') o += ' ';
    brk = false;
    o += c;
  }
  return o;
}

std::string elide_middle(const std::string& s, size_t n) {
  if (s.size() <= n) return s;
  return s.substr(0, n / 2) + "\xE2\x80\xA6" + s.substr(s.size() - (n - n / 2 - 1));
}

// ------------------------------------------------------------------ log (curl -v style)

// Path + query of a URL for the request line ("/a?b"; "/" when none).
std::string url_target(std::string_view u) {
  const size_t sch = u.find("://");
  if (sch != std::string_view::npos) u.remove_prefix(sch + 3);
  const size_t slash = u.find_first_of("/?");
  std::string t = slash == std::string_view::npos ? std::string("/") : std::string(u.substr(slash));
  if (const size_t h = t.find('#'); h != std::string::npos) t.resize(h);
  if (!t.empty() && t[0] == '?') t.insert(t.begin(), '/');
  return t;
}

// The whole exchange as one text, like curl -v: connection and TLS ("*"), the request as sent
// (">"), redirect hops, the response status and headers ("<") and the timing. Built from what the
// response already holds; for a bug report.
std::string http_transcript(const HttpResponse& r, const SentHttp* sent, const std::vector<std::pair<std::string, std::string>>& req_rows,
                            const std::string& when) {
  std::string o;
  auto line = [&](char p, const std::string& t) {
    o += p;
    o += ' ';
    for (char c : t) {
      o += c;
      if (c == '\n') o += std::string(1, p) + "   ";
    }
    o += '\n';
  };
  const std::string url = sent ? sent->request.url : r.effective_url;
  line('*', when + "  " + (sent ? sent->request.method + " " : std::string()) + url);
  for (const InfoRow& row : http_conn_rows(r, sent)) {
    if (row.section) continue;
    if (row.key == "URL") continue;
    line('*', row.key + ": " + row.value);
  }
  for (size_t i = 0; i < r.hops.size(); ++i) {
    const RedirectHop& h = r.hops[i];
    line('*', "");
    line('>', (sent ? sent->request.method : std::string("GET")) + " " + url_target(h.url) + "  (" + h.url + ")");
    line('<', std::to_string(h.status) + " redirect  \xC2\xB7 " + fmt::ms(h.total_ms) + " (wait " + fmt::ms(h.ttfb_ms) +
                  ")" + (h.connection_reused ? "  \xC2\xB7 reused connection" : "") +
                  (h.remote_ip.empty() ? std::string() : "  \xC2\xB7 " + host_port(h.remote_ip, h.remote_port)));
    line('*', "Following the redirect");
  }
  line('*', "");
  if (sent) {
    const std::string final_url = r.effective_url.empty() ? url : r.effective_url;
    line('>', sent->request.method + " " + url_target(final_url) + " " + (r.http_version.empty() ? "HTTP/1.1" : r.http_version));
    // Request rows: [0] is the request line, then headers, then "(body)" and redirect rows.
    for (size_t i = 1; i < req_rows.size(); ++i) {
      const auto& [k, v] = req_rows[i];
      if (k.rfind("(body)", 0) == 0) {
        line('>', "");
        if (v != "none") line('>', "[" + k.substr(7) + " body] " + v);
        break;
      }
      line('>', k + ": " + v);
    }
  }
  if (!r.ok) {
    line('*', "Failed: " + r.error);
  } else {
    line('<', (r.http_version.empty() ? std::string("HTTP/1.1") : r.http_version) + " " + std::to_string(r.status) +
                  (r.status_text.empty() ? std::string() : " " + r.status_text));
    for (const auto& [k, v] : r.headers) line('<', std::string(k) + ": " + std::string(v));
    line('<', "");
    const HttpSizes& z = r.sizes;
    std::string b = "Body " + fmt::human_bytes(z.response_body);
    if (z.response_body_wire != z.response_body) b += " (" + fmt::human_bytes(z.response_body_wire) + " on the wire)";
    line('*', b + ", headers " + fmt::human_bytes(z.response_headers));
  }
  const Timing& t = r.timing;
  // The pre-request script ran inside Queue: split out as in the status line and the waterfall
  // (Total is the request's own time, the script is named next to it).
  const ScriptRun* pre = sent && sent->pre_script && sent->pre_script->ran ? sent->pre_script.get() : nullptr;
  const double pre_ms = pre ? std::min(t.queue_ms, pre->elapsed_us / 1000.0) : 0.0;
  std::string tm = "Timing: queue " + fmt::ms(t.queue_ms - pre_ms) + ", DNS " + fmt::ms(t.dns_ms) + ", connect " +
                   fmt::ms(t.connect_ms) + ", TLS " + fmt::ms(t.tls_ms) + ", send " + fmt::ms(t.send_ms) + ", wait " +
                   fmt::ms(t.ttfb_ms) + ", download " + fmt::ms(t.download_ms);
  if (r.redirect_ms > 0) tm += ", redirects " + fmt::ms(r.redirect_ms);
  line('*', tm + ", total " + fmt::ms(t.total_ms - pre_ms) +
                (pre_ms > 0 ? " (+ pre-request script " + fmt::ms(pre_ms) + ")" : std::string()));
  line('*', "Bytes on the wire: " + std::to_string(t.bytes_sent) + " sent, " + std::to_string(t.bytes_received) + " received");
  return o;
}

// ------------------------------------------------------------------ view

enum Tab { kBody, kHeaders, kCookies, kTrailers, kMessages, kRequest, kTests, kLog };
// Book page of an event stream's Body tab (no tab of its own: the Body tab shows it).
constexpr int kSsePage = 8;

// The body pane before the first response: what the view will show, so its features are found.
const char* const kEmptyHelp =
    "Send the request (Ctrl+Enter) to see its response here.\n\n"
    "Timing    every phase from click to last byte, vs the last run\n"
    "Protocol  click the HTTP / TLS chip: connection, certificate\n"
    "Body      Pretty, Raw, Table, Preview; Ctrl+F finds\n"
    "Filter    Ctrl+Shift+F, e.g. $.items[?(@.price < 10)].name\n"
    "Tests     assertions and script test() results, with actuals\n"
    "Log       curl -v style transcript and script console output\n"
    "Examples  save a response; compare later runs with it\n\n"
    "Esc cancels a request in flight.";
// The same for a gRPC tab (its body is the response messages as JSON: no HTML / table modes).
const char* const kEmptyHelpGrpc =
    "Send the call (Ctrl+Enter) to see its response here.\n\n"
    "Timing    every phase from click to the trailers, vs the last call\n"
    "Body      the response message(s) as JSON; Ctrl+F finds\n"
    "Messages  every streamed message with its time offset\n"
    "Headers   response metadata; Trailers: status, message, details\n"
    "Tests     assertions (res.status, res.messages[0].field) with actuals\n"
    "Log       the call as sent and received, with its timing\n\n"
    "Esc cancels a call in flight.";

class ResponseView : public wxPanel, public ResponsePanel {
 public:
  explicit ResponseView(wxWindow* parent) : wxPanel(parent, wxID_ANY) {
    SetBackgroundColour(Theme::get().bg);
    TraceLaps tl("build");
    status_ = new StatusStrip(this);
    wf_ = new Waterfall(this);
    tabs_ = new TabStrip(this, [this](int i) { show_page(i); });
    book_ = new wxSimplebook(this, wxID_ANY);
    book_->SetBackgroundColour(Theme::get().bg);
    // (The default handler lays the view out; this one does the same, timed when tracing.)
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      if (!GetSizer()) return e.Skip();
      TraceScope ts("size.layout");
      Layout();
    });
    tl.lap("strips");
    body_ = new BodyView(book_);
    body_->on_visualize_chosen = [this](bool on) {
      const std::string k = vis_key();
      if (k.empty()) return;
      auto& m = vis_chosen_by_request();
      if (!on) m.erase(k);
      else if (m.size() < 4096) m.insert(k);
      // Kept with the run shown, so the request reopens (after a restart too) in this view.
      if (showing_grpc_ || shown_run_ >= runs_.size()) return;
      const Run& run = runs_[shown_run_];
      const RequestTab* t = tab();
      if (!t || t->request().kind != RequestKind::Http) return;
      const SavedRequest& q = t->request();
      const std::string key =
          rv::run_key(q.http.method + " " + q.http.url, run.resp->status, run.resp->timing.total_ms + run.prompt_ms);
      if (auto x = rv::find_extras_cached(key); x && x->visualize != on) {
        x->visualize = on;
        const Workspace* ws = t->workspace();
        rv::keep_extras(ws ? ws->root() : std::filesystem::path(), key, std::move(*x));
      }
    };
    tl.lap("body");
    // The other pages are cheap panels until first shown (see LazyPage).
    headers_ = new KvPage(book_, "Header", "Value");
    cookies_ = new CookiePage(book_, [this](const std::string& host) { manage_cookies(host); });
    trailers_ = new KvPage(book_, "Trailer", "Value");
    msgs_ = new MsgPage(book_, [this](size_t i) {
      select_tab(kBody);
      body_->goto_message(i);
    });
    request_ = new KvPage(book_, "Request", "Value");
    tests_ = new TestsView(book_);
    tests_->on_edit = [this](const std::vector<Assertion>& a) { save_asserts(a); };
    tests_->on_reveal = [this](const Assertion& a) { reveal(a); };
    // The body's right-click menu: assert or capture the JSON value under the mouse.
    body_->on_add_assert = [this](const std::string& target, const std::string& op, const std::string& value) {
      if (!tab() || showing_grpc_) return;
      std::vector<Assertion> a = tests_->asserts();
      Assertion x;
      x.target = target;
      x.op = op;
      x.value = value;
      if (std::find(a.begin(), a.end(), x) != a.end() || tests_->find_check(target, op, value))
        return body_->note("already asserted: " + target + " " + op + (value.empty() ? "" : " " + value), Tone::Dim);
      a.push_back(std::move(x));
      save_asserts(a);
      refresh_asserts(true);  // the Tests list shows (and checks) the new row now, not when it is next opened
      body_->note("assertion added (Tests): " + target + " " + op + (value.empty() ? "" : " " + value), Tone::Ok);
    };
    body_->asserted = [this](const std::string& target, const std::string& op, const std::string& value) {
      if (!tab() || showing_grpc_) return std::string();
      std::string from;
      const TestRow* r = tests_->find_check(target, op, value, &from);
      if (!r) return std::string();
      const char* st = r->state == TestRow::Pass  ? "pass"
                       : r->state == TestRow::Fail ? "fail"
                       : r->state == TestRow::Off  ? "disabled"
                                                   : "not run";
      return from.empty() ? std::string(st) : "inherited from " + from + ", " + st;
    };
    // The request editor owns the Capture table: a capture added here sticks only once the editor
    // adopts captures changed elsewhere (as it does asserts). Probed once per process on a built
    // editor (append a row, store, see whether it stayed, take it out again): until then the menu
    // does not offer an item that would ask for a name and then do nothing.
    body_->capture_ok = [this]() -> bool {
      static int known = -1;
      if (known >= 0) return known == 1;
      RequestTab* t = tab();
      if (!t || showing_grpc_) return false;
      if (!t->realized()) return true;  // no editor yet: the row is loaded into it when it is built
      Capture probe;
      probe.var = "__watt_capture_probe";
      probe.target = "res.status";
      probe.scope = Capture::Runtime;
      auto& q = const_cast<SavedRequest&>(t->request());
      q.captures.push_back(probe);
      auto has = [&] {
        const auto& now = t->request().captures;
        return std::find(now.begin(), now.end(), probe) != now.end();
      };
      known = has() ? 1 : 0;
      if (known == 1) {
        auto& q2 = const_cast<SavedRequest&>(t->request());
        q2.captures.erase(std::remove(q2.captures.begin(), q2.captures.end(), probe), q2.captures.end());
        (void)t->request();  // the editor adopts the removal
      }
      return known == 1;
    };
    body_->on_add_capture = [this](const std::string& target, const std::string& suggested) {
      RequestTab* t = tab();
      if (!t || showing_grpc_) return;
      wxTextEntryDialog dlg(this, wxString::FromUTF8("After each response, store the value of\n" + target +
                                                     "\nin this runtime variable (use it as {{name}}):"),
                            "Capture into variable", wxString::FromUTF8(suggested));
      if (dlg.ShowModal() != wxID_OK) return;
      std::string var = dlg.GetValue().utf8_string();
      while (!var.empty() && var.back() == ' ') var.pop_back();
      while (!var.empty() && var.front() == ' ') var.erase(var.begin());
      if (const std::string why = capture_var_problem(var, kMaxVarName); !why.empty())
        return body_->note("not captured: " + why, Tone::Err);
      Capture c;
      c.var = var;
      c.target = target;
      c.scope = Capture::Runtime;
      auto& q = const_cast<SavedRequest&>(t->request());
      q.captures.push_back(c);
      t->changed();
      const auto& now = t->request().captures;
      if (std::find(now.begin(), now.end(), c) == now.end()) {
        // The request editor's Capture table owns the list: the row goes there by hand.
        if (wxTheClipboard->Open()) {
          wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(target)));
          wxTheClipboard->Close();
        }
        return body_->note("add it in the request's Tests > Capture table: " + var + " \xE2\x86\x90 " + target +
                               " (path copied)",
                           Tone::Warn);
      }
      body_->note("captured on every response: {{" + var + "}} \xE2\x86\x90 " + target, Tone::Ok);
      run_tests();
    };
    // An inherited (folder / collection) check: edited in that folder's settings, not here.
    tests_->on_edit_folder = [this](const std::filesystem::path& folder) {
      RequestTab* t = tab();
      const Workspace* ws = t ? t->workspace() : nullptr;
      if (!ws || folder.empty()) return;
      wxWeakRef<wxWindow> self(this);
      http_editor::edit_folder_settings(this, *ws, folder, [self, this] {
        if (self) refresh_asserts(true);
      });
    };
    log_ = new LogPage(book_);
    sse_ = new SsePage(book_);
    sse_->on_raw = [this] {
      sse_raw_ = true;
      show_raw_stream();
    };
    book_->AddPage(body_, "Body");
    book_->AddPage(headers_, "Headers");
    book_->AddPage(cookies_, "Cookies");
    book_->AddPage(trailers_, "Trailers");
    book_->AddPage(msgs_, "Messages");
    book_->AddPage(request_, "Request");
    book_->AddPage(tests_, "Tests");
    book_->AddPage(log_, "Log");
    book_->AddPage(sse_, "Events");
    tabs_->add("Body");
    tabs_->add("Headers");
    tabs_->add("Cookies");
    tabs_->add("Trailers");
    tabs_->add("Messages");
    tabs_->add("Request");
    tabs_->add("Tests");
    tabs_->add("Log");
    tl.lap("pages");

    hist_ = new ChipBar(this, [this](int id) { id == 2 ? examples_menu() : history_menu(); });
    hist_->add(2, "Examples \xE2\x96\xBE",
               "Save this response as an example of the request (Postman's Save Response), or open a saved one to "
               "look at it or check it again. Examples are kept per user, in the workspace's private .watt "
               "folder (a response may hold tokens).", -1, true);
    hist_->set_visible(2, false);
    hist_->add(1, "History", "Earlier responses of this tab, each labelled with its request: pick one to look at it "
                             "again (its delta compares with the run of the same request before it)", -1, true);
    hist_->set_visible(1, false);
    hist_->set_bottom_rule(true);  // the tab strip's rule runs to the pane's right edge
    hist_->SetMinSize(wxSize(FromDIP(220), -1));
    auto* tabrow = new wxBoxSizer(wxHORIZONTAL);
    tabrow->Add(tabs_, 1, wxEXPAND);
    tabrow->Add(hist_, 0, wxEXPAND);

    auto* sz = new wxBoxSizer(wxVERTICAL);
    sz->Add(status_, 0, wxEXPAND);
    sz->Add(wf_, 0, wxEXPAND | wxTOP, FromDIP(4));
    sz->Add(tabrow, 0, wxEXPAND | wxTOP, FromDIP(2));
    sz->Add(book_, 1, wxEXPAND);
    SetSizer(sz);
    tl.lap("sizer");
    wf_->set_on_conn_click([this](const wxRect& r) { open_conn(r); });
    status_->set_on_click([this](int id, const wxRect& r) {
      if (id == kClickTests) return select_tab(kTests);
      if (id == kClickLog) return select_tab(kLog);
      open_conn(r);
    });

    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
      if (e.GetKeyCode() == 'F' && e.GetModifiers() == wxMOD_CONTROL) {
        select_tab(kBody);
        body_->show_find();
        return;
      }
      e.Skip();
    });
    // Esc anywhere in the tab (the URL field included) cancels the request in flight. Hooked on
    // the tab once it exists (after the constructor; the tab builds us first).
    CallAfter([this] {
      if (RequestTab* t = tab()) {
        t->Bind(wxEVT_CHAR_HOOK, [this, t](wxKeyEvent& e) {
          if (e.GetKeyCode() == WXK_ESCAPE && e.GetModifiers() == 0 && t->in_flight()) {
            t->cancel();
            return;
          }
          e.Skip();
        });
      }
      TraceScope ts("ctor_after");
      // A restored response shown right after the view was built (a session's tab) was checked
      // before the checks were known: they run now.
      refresh_asserts(showing_grpc_ ? grpc_last_ != nullptr : !runs_.empty());
      refresh_examples();
      // Esc with the focus outside the tab (the sidebar after a double-click + Ctrl+Enter) cancels
      // the request of the tab on screen too.
      top_ = wxGetTopLevelParent(this);
      if (top_) top_->Bind(wxEVT_CHAR_HOOK, &ResponseView::on_top_key, this);
    });
  }

  void on_top_key(wxKeyEvent& e) {
    e.Skip();
    if (e.GetKeyCode() != WXK_ESCAPE || e.GetModifiers() != 0) return;
    RequestTab* t = tab();
    if (!t || !t->in_flight() || !t->IsShownOnScreen()) return;
    // A text field outside the tab that holds text keeps Esc (e.g. clearing the sidebar filter).
    if (wxWindow* f = wxWindow::FindFocus()) {
      bool inside = false;
      for (wxWindow* w = f; w && !inside; w = w->GetParent()) inside = w == t;
      if (!inside)
        if (auto* te = dynamic_cast<wxTextEntry*>(f); te && !te->IsEmpty()) return;
    }
    e.Skip(false);
    t->cancel();
  }

  wxWindow* window() override { return this; }

  void clear() override {
    TraceScope ts("clear");
    stop_live_head();
    tap_.reset();
    ++tap_seq_;
    tap_armed_ = false;
    sse_timer_.Stop();
    sse_live_ = false;
    sse_off();
    status_->set("", Tone::Plain, {{"Send a request to see the response here  (Ctrl+Enter)", "", Tone::Dim}});
    wf_->clear(false);
    switch_filter({});
    shown_ident_.clear();
    {
      const RequestTab* t = tab();
      body_->show_message(t && t->request().kind == RequestKind::Grpc ? kEmptyHelpGrpc : kEmptyHelp);
    }
    headers_->set({});
    trailers_->set({});
    msgs_->reset();
    request_->set({});
    cookies_->set({});
    log_->set({});
    set_kind_tabs(false, false);
    select_tab(kBody);
    runs_.clear();
    shown_run_ = 0;
    update_history_chip();
    ++test_seq_;
    tests_->set_no_response();
    tests_->set_script({});
    awaiting_ = false;
    // The tab may have loaded another request: read its checks once this turn is over (the tab
    // may still be building its editor).
    const uint64_t seq = ++view_seq_;
    help_seq_ = seq;
    grpc_history_when_.clear();
    // The tab may now hold a request of the other kind: an HTTP request opened after a gRPC
    // response must not be treated as "a gRPC response is on screen".
    showing_grpc_ = false;
    grpc_last_.reset();
    CallAfter([this, seq] {
      TraceScope ts("clear_after");
      // A response restored meanwhile (a session's gRPC tab shows its last call before this turn)
      // was checked against no checks: run them now that they are known.
      refresh_asserts(seq == view_seq_ ? false : (showing_grpc_ ? grpc_last_ != nullptr : !runs_.empty()));
      refresh_examples();
      load_last_from_history(seq);
    });
  }

  // A request opened in a tab shows its last response from the workspace history (Insomnia
  // keeps one per request), read on a worker thread, unless something else was shown first (a
  // send, the tab's own restored response). History snapshot tabs get theirs from the sidebar.
  void load_last_from_history(uint64_t seq, int tries = 0) {
    if (seq != view_seq_) return;
    RequestTab* t = tab();
    if (!t || t->in_flight() || t->last_response()) return;
    const Workspace* ws = t->workspace();
    if (!ws || ws->root().empty()) {
      // A tab restored at start-up is built before the workspace has finished opening.
      if (tries < 40) {
        if (hist_timer_.GetOwner() != this) {
          hist_timer_.SetOwner(this, wxWindow::NewControlId());
          Bind(wxEVT_TIMER, [this](wxTimerEvent&) { load_last_from_history(hist_seq_, hist_tries_); },
               hist_timer_.GetId());
        }
        hist_seq_ = seq;
        hist_tries_ = tries + 1;
        hist_timer_.StartOnce(250);
      }
      return;
    }
    const std::filesystem::path& file = t->file();
    if (file.empty() || file.parent_path() == ws->root() / ".watt" / "snapshots") return;
    // A tab in the background (its view prebuilt in a quiet moment, a restored session) reads
    // its last response when it is first shown: a 10 MB history body is not held for a tab
    // nobody looks at.
    if (!t->IsShown()) {
      hist_seq_ = seq;
      if (!hist_show_bound_) {
        hist_show_bound_ = true;
        t->Bind(wxEVT_SHOW, [this, alive = std::weak_ptr<bool>(alive_)](wxShowEvent& e) {
          e.Skip();
          if (alive.expired() || !e.IsShown()) return;
          const uint64_t seq = std::exchange(hist_seq_, 0);  // once per deferred lookup
          if (!seq) return;
          CallAfter([this, alive, seq] {
            if (!alive.expired() && seq == view_seq_) load_last_from_history(seq);
          });
        });
      }
      return;
    }
    SavedRequest q = t->request();
    const RequestKind kind = q.kind;
    const std::string summary = kind == RequestKind::Grpc ? q.grpc_method + " @ " + q.grpc_target.address
                                                          : q.http.method + " " + q.http.url;
    if (kind == RequestKind::Grpc ? q.grpc_method.empty() : q.http.url.empty()) return;
    auto copy = std::make_shared<Workspace>(*ws);
    std::thread([copy, q = std::move(q), kind, summary, seq, alive = alive_, this]() mutable {
      std::vector<HistoryEntry> h = copy->history(200);
      filter_hidden_history(load_history_hidden(copy->root()), h);
      keep_runs_of(*copy, q, summary, h);
      auto lr = std::make_shared<LastResponse>();
      auto ex = std::make_shared<std::optional<rv::RunExtras>>();
      auto entry = std::make_shared<HistoryEntry>();
      std::string when;
      if (kind == RequestKind::Http) {
        // The newest run of this request decides: its kept response (body capped by the history)
        // with what this view kept of it (dialog time, scripts, TLS, hops), or that alone when
        // the body was too large to keep, or just its status and time from the history list.
        for (const HistoryEntry& e : h) {
          if (e.kind != kind || history_template(e) != summary) continue;
          *entry = e;
          when = e.when_iso;
          *ex = rv::find_extras(copy->root(), rv::run_key(summary, e.status, e.total_ms));
          if (auto j = load_history_response(copy->root(), e)) *lr = last_response_from_json(*j, q);
          break;
        }
        if (when.empty() || !wxTheApp) return;
      } else {
        // The newest run of this request that has its response kept (not every entry does).
        int looked = 0;
        for (const HistoryEntry& e : h) {
          if (e.kind != kind || history_template(e) != summary) continue;
          if (++looked > 8) break;
          if (auto j = load_history_response(copy->root(), e)) {
            *lr = last_response_from_json(*j, q);
            when = e.when_iso;
            if (*lr) break;
          }
        }
        if (!*lr || !wxTheApp) return;
      }
      wxTheApp->CallAfter([alive, this, seq, lr, ex, entry, when] {
        TraceScope ts("history_show");
        if (!*alive || seq != view_seq_ || !runs_.empty()) return;
        if (RequestTab* tt = tab(); !tt || tt->in_flight() || tt->last_response()) return;
        if (lr->grpc) {
          grpc_history_when_ = when;
          show_grpc(lr->grpc);
          return;
        }
        history_when_ = when;
        if (lr->http && ex->has_value()) {
          auto [r, sent] = with_extras(*lr->http, lr->sent, **ex);
          extras_applied_ = true;
          show_http(std::move(r), std::move(sent));
        } else if (lr->http) {
          extras_applied_ = true;  // none were kept for this run (an older version, another tool)
          show_http(lr->http, lr->sent);
        } else if (ex->has_value() && (*ex)->head) {
          auto [r, sent] = with_extras_head(**ex);
          extras_applied_ = true;
          body_not_kept_ = true;
          not_kept_size_ = (*ex)->body_size;
          show_http(std::move(r), std::move(sent));
        } else {
          show_not_kept(*entry);
        }
        extras_applied_ = false;
        body_not_kept_ = false;
        history_when_.clear();
      });
    }).detach();
  }

  // ------------------------------------------------------------------ event streams
 private:
  struct Run;

 public:
  //
  // The shell's worker feeds every body chunk of an HTTP send to the tap. A text/event-stream
  // body (sniffed from its first bytes: no headers reach the tap) turns the Body tab into the
  // event list as soon as the first event is complete; batches reach the UI thread at most every
  // kSseBatchMs, so a fast stream never floods it.
  std::shared_ptr<HttpBodyTap> body_tap() override {
    const uint64_t seq = ++tap_seq_;
    tap_armed_ = true;
    std::weak_ptr<bool> alive = alive_;
    tap_t0_ = Clock::now();
    tap_ = std::make_shared<SseTap>(
        tap_t0_,
        [alive, this, seq] {
          // Worker thread: one post per drain (the tap does not call again until drain() ran).
          if (!wxTheApp) return;
          wxTheApp->CallAfter([alive, this, seq] {
            if (auto a = alive.lock(); a && *a && seq == tap_seq_) sse_kick();
          });
        },
        [alive, this, seq] {
          // Worker thread: the response head is in (status, headers, phases to the first byte).
          if (!wxTheApp) return;
          wxTheApp->CallAfter([alive, this, seq] {
            if (auto a = alive.lock(); a && *a && seq == tap_seq_) on_tap_head();
          });
        });
    return tap_;
  }

  void sse_kick() {
    if (!tap_ || sse_timer_.IsRunning()) return;
    const double since = sse_drained_at_ == Clock::time_point{} ? 1e9 : ms_between(sse_drained_at_, Clock::now());
    if (since >= kSseBatchMs) return sse_drain();
    if (sse_timer_.GetOwner() != this) {
      sse_timer_.SetOwner(this, wxWindow::NewControlId());
      Bind(wxEVT_TIMER, [this](wxTimerEvent&) { sse_drain(); }, sse_timer_.GetId());
    }
    sse_timer_.StartOnce(std::max(1, static_cast<int>(kSseBatchMs - since)));
  }

  // UI thread: the events and bytes since the last drain onto the list.
  void sse_drain() {
    TraceScope ts("sse_drain");
    sse_drained_at_ = Clock::now();
    if (!tap_ || !tap_->is_sse()) return;
    SseTap::Batch b = tap_->drain();
    if (!awaiting_) return;  // the result (or a cancel) already took the stream over
    if (!sse_live_) sse_begin_live();
    sse_comments_ = b.comments;
    if (sse_raw_bytes_.size() < kSseRawMax) sse_raw_bytes_.append(b.raw, 0, kSseRawMax - sse_raw_bytes_.size());
    sse_->append(std::move(b.events));
    sse_live_caption(false);
    if (first_ms_ < 0) first_ms_ = tap_->first_ms();
    status_->set_busy_label(live_label());
    wf_->set_live(first_ms_, tap_->bytes(), 0);
    if (head_shown_) live_tick();
  }

  // ------------------------------------------------------------------ live head
  //
  // The engine hands the final response's head over as soon as its header block ends (HeadFn):
  // status, headers, connection and the phases up to the first byte. A response that is still
  // arriving after kHeadDelayMs (an event stream, a slow download) shows them then, with only
  // Download running; a fast one goes straight to show_http (no extra repaint).
  static constexpr int kHeadDelayMs = 40;
  static constexpr int kLiveTickMs = 100;

  void on_tap_head() {
    if (!awaiting_ || kind_ != RequestKind::Http || !tap_ || head_shown_) return;
    // An event stream: the event list's controls are built now, in a UI turn of their own,
    // not in the same turn as the first batch of events (the two were 25 ms together).
    if (tap_->is_sse()) {
      TraceScope ts("sse_ensure");
      sse_->ensure();
    }
    if (head_timer_.GetOwner() != this) {
      head_timer_.SetOwner(this, wxWindow::NewControlId());
      Bind(wxEVT_TIMER, [this](wxTimerEvent&) { show_live_head(); }, head_timer_.GetId());
    }
    if (!head_timer_.IsRunning()) head_timer_.StartOnce(kHeadDelayMs);
  }

  // Body bytes and events per second since the first body byte ("" until there is half a second).
  std::string live_rate() const {
    if (!tap_ || tap_->first_ms() < 0) return {};
    const double span = (ms_between(tap_t0_, Clock::now()) - tap_->first_ms()) / 1000.0;
    if (span < 0.5) return {};
    std::string t = fmt::human_bytes(static_cast<uint64_t>(static_cast<double>(tap_->bytes()) / span)) + "/s";
    if (sse_live_ && sse_) {
      char b[32];
      std::snprintf(b, sizeof b, "%.1f events/s", static_cast<double>(sse_->count()) / span);
      t += std::string(" \xC2\xB7 ") + b;
    }
    return t;
  }

  // The busy status line while a response arrives: protocol, what arrived, throughput.
  std::string live_label() const {
    std::string t;
    if (head_shown_) {
      t = head_shown_->http_version.empty() ? std::string("HTTP") : head_shown_->http_version;
      if (!head_shown_->tls_version.empty()) t += " \xC2\xB7 " + head_shown_->tls_version;
      t += "  \xC2\xB7  ";
    }
    const uint64_t got = std::max<uint64_t>(tap_ ? tap_->bytes() : 0, live_bytes_);
    if (sse_live_ && sse_) {
      t += "Streaming \xC2\xB7 " + std::to_string(sse_->count()) + (sse_->count() == 1 ? " event" : " events") +
           " \xC2\xB7 " + fmt::human_bytes(got);
    } else {
      t += "Receiving " + fmt::human_bytes(got);
    }
    if (const std::string rate = live_rate(); !rate.empty()) t += " \xC2\xB7 " + rate;
    return t;
  }

  std::string live_wf_head() const {
    const uint64_t got = std::max<uint64_t>(tap_ ? tap_->bytes() : 0, live_bytes_);
    std::string t = std::string(sse_live_ ? "streaming \xC2\xB7 " : "receiving \xC2\xB7 ") + fmt::human_bytes(got);
    if (const std::string rate = live_rate(); !rate.empty()) t += " \xC2\xB7 " + rate;
    return t;
  }

  static KV header_rows(const HttpResponse& r) {
    KV h;
    h.reserve(r.headers.size());
    for (const auto& [k, v] : r.headers) h.emplace_back(std::string(k), std::string(v));
    return h;
  }

  // The head of the response in flight: status pill, Headers, protocol, and the waterfall with
  // every phase up to Wait (TTFB) as measured; Download runs (live_tick).
  void show_live_head() {
    if (!awaiting_ || kind_ != RequestKind::Http || !tap_) return;
    std::shared_ptr<const HttpResponse> h = tap_->head_response();
    if (!h || h == head_shown_) return;
    TraceScope ts("live_head");
    head_shown_ = h;
    head_at_ms_ = tap_->head_ms();
    const bool plain = h->effective_url.rfind("http:", 0) == 0;
    TimingModel m = http_model(*h, plain);
    for (size_t i = 0; i < m.phases.size(); ++i)
      if (m.phases[i].name == "Download") {
        m.live_row = static_cast<int>(i);
        m.phases[i].tip = "First response byte -> now: the body is still arriving. Every phase before it is final "
                          "(the engine reported them with the response head).";
      }
    for (Phase& ph : m.phases)
      if (ph.name == "Process") ph.has_value = false;  // runs once the body is complete
    m.total_tip = "So far: Send click -> now. Process and the final total come with the complete response.";
    m.live_head = live_wf_head();
    wf_->set(m, nullptr);
    std::string badge = std::to_string(h->status);
    if (!h->status_text.empty()) badge += " " + h->status_text;
    else if (h->status == 200) badge += " OK";
    status_->set_busy_badge(badge, http_tone(h->status));
    headers_->set(header_rows(*h));
    tabs_->set_label(kHeaders, "Headers", std::to_string(h->headers.size()));
    live_tick();
    if (live_timer_.GetOwner() != this) {
      live_timer_.SetOwner(this, wxWindow::NewControlId());
      Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        if (!awaiting_ || !head_shown_) return live_timer_.Stop();
        live_tick();
      }, live_timer_.GetId());
    }
    live_timer_.Start(kLiveTickMs);
  }

  void live_tick() {
    if (!head_shown_ || !tap_) return;
    const double dl = std::max(0.0, ms_between(tap_t0_, Clock::now()) - head_at_ms_);
    // One figure for both: the status clock and the waterfall's Total read the same.
    const double total = head_shown_->timing.total_ms + dl;
    wf_->live_tick(dl, total, live_wf_head());
    status_->set_busy_label(live_label());
    status_->set_busy_ms(total);
  }

  void stop_live_head() {
    head_timer_.Stop();
    live_timer_.Stop();
    head_shown_.reset();
    head_at_ms_ = -1;
  }

  // The first event of a send arrived: the Body tab becomes the event list.
  void sse_begin_live() {
    sse_live_ = true;
    sse_raw_bytes_.clear();
    sse_comments_ = 0;
    sse_resp_.reset();
    stale_timer_.Stop();
    body_->set_stale(false);
    waiting_msg_ = true;
    sse_->reset();
    sse_show(true);
  }

  // The Body tab shows the event list (true) or the plain body (false).
  void sse_show(bool on) {
    if (on == sse_on_) return;
    sse_on_ = on;
    sse_raw_ = false;
    if (on) {
      sse_->ensure();
      body_->set_events_chip([this] {
        sse_raw_ = false;
        if (tabs_->selection() == kBody) show_page(kBody);
      });
    } else {
      body_->set_events_chip({});
    }
    tabs_->set_label(kBody, on ? "Events" : "Body", on && sse_->count() ? std::to_string(sse_->count()) : std::string());
    if (tabs_->selection() == kBody) show_page(kBody);
  }

  void sse_live_caption(bool final_batch) {
    const size_t n = sse_->count();
    std::string c = std::string(final_batch ? "" : "live \xC2\xB7 ") + std::to_string(n) + (n == 1 ? " event" : " events");
    if (sse_comments_) c += " \xC2\xB7 " + std::to_string(sse_comments_) + (sse_comments_ == 1 ? " comment" : " comments");
    if (tap_) c += " \xC2\xB7 " + fmt::human_bytes(tap_->bytes());
    if (tap_) sse_->set_tap_dropped(tap_->dropped());
    const auto& ev = sse_->events();
    if (!ev.empty() && ev.back().at_ms >= 0) c += " \xC2\xB7 last at +" + fmt::ms(ev.back().at_ms);
    sse_->set_caption(c, final_batch ? Tone::Dim : Tone::Accent);
    tabs_->set_label(kBody, "Events", std::to_string(n));
  }

  // Raw chip of the event list: the stream as received in the body view (the stream so far
  // while it runs).
  void show_raw_stream() {
    if (sse_live_ && awaiting_) {
      auto r = std::make_shared<HttpResponse>();
      r->ok = true;
      r->body = sse_raw_bytes_;
      r->headers.add("Content-Type", "text/event-stream");
      body_->set_origin_note("stream so far (" + fmt::human_bytes(r->body.size()) + "), as of +" +
                             fmt::ms(ms_between(send_t0_, Clock::now())));
      body_->set_http(std::move(r));
      body_->set_events_chip([this] {
        sse_raw_ = false;
        if (tabs_->selection() == kBody) show_page(kBody);
      });
    }
    if (tabs_->selection() == kBody) show_page(kBody);
  }

  // The run's body is an event stream: the list for it (the live one when this run streamed
  // here, else parsed from the body, with the offsets kept for the run when there are any).
  void sse_for_run(const Run& run) {
    const std::shared_ptr<const HttpResponse>& r = run.resp;
    if (sse_resp_ == r) return sse_show(true);
    sse_resp_ = r;
    if (run.sse_live) {
      // The rest of the stream (events after the last batch) is on the list already: show_http
      // drained it before this run was made.
      sse_show(true);
      std::string c = std::to_string(sse_->count()) + (sse_->count() == 1 ? " event" : " events");
      if (sse_comments_) c += " \xC2\xB7 " + std::to_string(sse_comments_) + " comments";
      if (run.cancelled_ms >= 0) {
        c += " \xC2\xB7 " + (run.cut_label.empty() ? "cancelled by you after " + fmt::ms(run.cancelled_ms)
                                                    : "stream ended: " + run.cut_label);
        sse_->set_caption(c, run.cut_label.empty() ? Tone::Warn : Tone::Err);
      } else {
        c += " \xC2\xB7 stream closed after " + fmt::ms(r->timing.total_ms);
        sse_->set_caption(c, Tone::Dim);
      }
      tabs_->set_label(kBody, "Events", std::to_string(sse_->count()));
      return;
    }
    sse_->reset();
    sse_show(true);
    auto at = run.sse_at;
    auto fill = [this, at](std::vector<SseEvent> evs) {
      if (at)
        for (size_t i = 0; i < evs.size() && i < at->size(); ++i) evs[i].at_ms = (*at)[i];
      sse_comments_ = 0;
      sse_->append(std::move(evs));
      const size_t n = sse_->count();
      std::string c = std::to_string(n) + (n == 1 ? " event" : " events");
      c += at && !at->empty() ? " \xC2\xB7 offsets as received" : " \xC2\xB7 parsed from the body (no offsets kept)";
      sse_->set_caption(c, Tone::Dim);
      tabs_->set_label(kBody, "Events", std::to_string(n));
    };
    // A restored stream is parsed on a worker whatever its size: it is shown as a tab opens.
    if (!run.restored && r->body.size() <= kSseSyncParse) return fill(parse_sse(r->body));
    sse_->set_caption("reading events\xE2\x80\xA6", Tone::Accent);
    std::thread([r, fill, alive = alive_, this] {
      auto evs = std::make_shared<std::vector<SseEvent>>(parse_sse(r->body));
      if (!wxTheApp) return;
      wxTheApp->CallAfter([alive, this, r, evs, fill] {
        if (*alive && sse_resp_ == r) fill(std::move(*evs));
      });
    }).detach();
  }

  // The run on screen is not an event stream.
  void sse_off() {
    sse_resp_.reset();
    sse_show(false);
  }

  static bool is_sse_response(const HttpResponse& r) {
    const std::string_view ct = r.headers.get("content-type");
    if (is_event_stream(ct)) return true;
    return ct.empty() && sniff_sse(std::string_view(r.body).substr(0, 256)) == 1;
  }

  void set_history_when(const std::string& when_iso) override {
    history_when_ = when_iso;
    if (!when_iso.empty()) grpc_history_when_ = when_iso;
  }

  void show_sending(RequestKind kind) override {
    const bool help_shown = help_seq_ == view_seq_ && runs_.empty() && !showing_grpc_;
    ++view_seq_;
    grpc_history_when_.clear();
    kind_ = kind;
    live_ = false;
    // The tap handed out right before this send is its own; any other one is of an earlier send.
    if (kind != RequestKind::Http || !tap_armed_) {
      tap_.reset();
      ++tap_seq_;
    }
    tap_armed_ = false;
    sse_timer_.Stop();
    stop_live_head();
    sse_drained_at_ = {};
    sse_live_ = false;
    // The previous run's event list leaves the Body tab (its body stays, faded, as for any run).
    sse_off();
    send_t0_ = Clock::now();
    sent_here_ = true;
    awaiting_ = true;
    status_->set_busy(kind == RequestKind::Grpc ? "Calling\xE2\x80\xA6" : "Sending\xE2\x80\xA6", send_t0_);
    // gRPC results carry no request: remember what this call is (target, method, body) for its
    // delta. HTTP results come with the request as sent.
    grpc_key_.clear();
    grpc_label_.clear();
    std::string ident;  // which request this is (as edited, before {{var}} expansion)
    for (wxWindow* w = GetParent(); w; w = w->GetParent())
      if (auto* tab = dynamic_cast<RequestTab*>(w)) {
        const SavedRequest& q = tab->request();
        if (kind == RequestKind::Grpc) {
          grpc_key_ = owner_tag() + "grpc " + std::string(q.grpc_target.tls ? "tls " : "") + q.grpc_target.address + " " +
                      q.grpc_method + body_tag(q.grpc_body);
          grpc_label_ = q.grpc_method + " @ " + q.grpc_target.address;
          const GrpcTarget& gt = q.grpc_target;
          grpc_meta_ = GrpcConnMeta{gt.address, gt.authority, gt.server_name, gt.tls || gt.address.rfind("https:", 0) == 0,
                                    !gt.client_cert_pem.empty(), gt.web};
          ident ="grpc " + q.grpc_target.address + " " + q.grpc_method;
        } else {
          ident = "http " + q.http.method + " " + q.http.url;
        }
        set_asserts_from(q);
        // The request may have been loaded into this tab after the view was built.
        if (const Workspace* ws = tab->workspace(); ws && !q.file.empty()) {
          if (examples_dir(ws->root(), q.id) != examples_dir_) CallAfter([this] { refresh_examples(); });
        }
        break;
      }
    // The previous figures stay (dimmed, "previous run") only while the same request runs again:
    // Fail's timing must not sit above a Count stream.
    if (ident.empty() || ident != shown_ident_) {
      wf_->clear();
      switch_filter(ident);
    }
    shown_ident_ = ident;
    wf_->reserve(kind == RequestKind::Grpc ? 8 : 10);
    wf_->set_stale(true);
    first_ms_ = -1;
    wf_->set_waiting(send_t0_);
    // No earlier response: the empty-state help is not a "previous response" to fade.
    waiting_msg_ = help_shown;
    progress_shown_at_ = {};
    live_bytes_ = 0;
    if (help_shown) {
      body_->show_message("Waiting for the response.");
    } else {
      // The previous body stays (faded, labelled) only for a short moment, so a fast resend does
      // not flash an empty pane; a request still out after that clears it (Bruno does the same):
      // the body pane then shows the request in flight, never an old answer under a small label.
      body_->set_stale(true);
      if (stale_timer_.GetOwner() != this) {
        stale_timer_.SetOwner(this, wxWindow::NewControlId());
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { show_in_flight(stale_seq_); }, stale_timer_.GetId());
      }
      stale_seq_ = view_seq_;
      stale_timer_.StartOnce(static_cast<int>(kStaleBodyMs));
    }
    if (!tests_->asserts().empty()) {
      tests_->set_running();
      tabs_->set_label(kTests, "Tests", "\xE2\x80\xA6");
      tabs_->set_count_tone(kTests, Tone::Plain);
    }
  }

  // The request is still out kStaleBodyMs after the send: the previous body leaves the pane, which
  // shows the request in flight instead (bytes so far once they arrive).
  void show_in_flight(uint64_t seq) {
    // A gRPC stream that started shows its messages in the body: nothing to replace.
    if (seq != view_seq_ || !awaiting_ || waiting_msg_ || live_ || sse_live_) return;
    waiting_msg_ = true;
    body_->set_stale(false);
    body_->show_message(
        live_bytes_ > 0 ? in_flight_text(live_bytes_)
                        : "Waiting for the response\xE2\x80\xA6 no byte yet.\n\nEsc cancels.");
  }
  std::string in_flight_text(uint64_t bytes) const {
    return "Receiving the response: " + fmt::human_bytes(bytes) + " so far (body data since +" +
           fmt::ms(first_ms_ >= 0 ? first_ms_ : ms_between(send_t0_, Clock::now())) +
           ").\n\nThe body is shown when it is complete. Esc cancels.";
  }

  void show_progress(uint64_t bytes) override {
    // The tap saw the first body bytes exactly (progress reports come at most every 50 ms).
    if (tap_ && tap_->first_ms() >= 0) first_ms_ = tap_->first_ms();
    if (first_ms_ < 0) first_ms_ = ms_between(send_t0_, Clock::now());
    live_bytes_ = bytes;
    if (sse_live_) {  // the event list says what arrives (sse_drain)
      wf_->set_live(first_ms_, bytes, 0);
      return;
    }
    status_->set_busy_label(live_label() + "\xE2\x80\xA6");
    // A download still running after kStaleBodyMs: the old body goes (see show_in_flight).
    if (!waiting_msg_ && awaiting_ && ms_between(send_t0_, Clock::now()) >= kStaleBodyMs) show_in_flight(view_seq_);
    // The body area says what is arriving (not "Waiting"), a few times a second at most.
    if (waiting_msg_ && awaiting_) {
      const auto now = Clock::now();
      if (progress_shown_at_ == Clock::time_point{} || ms_between(progress_shown_at_, now) > 250) {
        progress_shown_at_ = now;
        body_->show_message(in_flight_text(bytes));
      }
    }
    wf_->set_live(first_ms_, bytes, 0);
  }

  void show_http(std::shared_ptr<const HttpResponse> r, std::shared_ptr<const SentHttp> sent) override {
    // A restored response (no show_sending() before it) is shown in a UI turn of its own: the tab
    // builds this view and restores its last run in the same turn, and the two together were
    // 20-90 ms of UI time on a busy machine. Shown one turn later (and in two parts, see
    // show_run), no step is longer than either alone. A send or clear in between drops it.
    if (!awaiting_ && !restore_now_ && r) {
      RestoreArgs a{std::move(r),      std::move(sent),   history_when_,  extras_applied_, extras_prompt_ms_,
                    extras_cut_size_, body_not_kept_,    not_kept_size_, view_seq_, extras_body_key_,
                    extras_sse_at_};
      // Taken over by the deferred call: the caller resets its own flags after this returns.
      extras_prompt_ms_ = 0;
      extras_cut_size_ = 0;
      extras_body_key_.clear();
      extras_sse_at_.reset();
      CallAfter([this, a = std::move(a), alive = alive_]() mutable {
        if (!*alive || a.seq != view_seq_ || awaiting_) return;
        restore_now_ = true;
        history_when_ = a.history_when;
        extras_applied_ = a.extras_applied;
        extras_prompt_ms_ = a.prompt_ms;
        extras_cut_size_ = a.cut_size;
        extras_body_key_ = std::move(a.body_key);
        extras_sse_at_ = std::move(a.sse_at);
        body_not_kept_ = a.body_not_kept;
        not_kept_size_ = a.not_kept_size;
        show_http(std::move(a.r), std::move(a.sent));
        restore_now_ = false;
        history_when_.clear();
        extras_applied_ = false;
        body_not_kept_ = false;
        not_kept_size_ = 0;
      });
      return;
    }
    TraceScope ts("show_http");
    // A response without show_sending() before it is a restored one (the tab's last response of
    // an earlier session, or one read from the history): never "sent now".
    const bool restored = !awaiting_;
    // A transfer that failed after its head arrived (an idle stream timing out, the server going
    // away mid-body): the status, headers, phases to TTFB and what arrived stay, as on a cancel.
    if (!restored && r && !r->ok && r->error != "cancelled" && kind_ == RequestKind::Http && tap_)
      if (std::shared_ptr<const HttpResponse> h = tap_->head_response()) {
        ++view_seq_;
        return show_cancelled_with_head(r->timing.total_ms, std::move(h), std::move(sent), r.get());
      }
    // An event stream listed live: the events after the last batch go on the list now, and their
    // offsets are kept with the run (a restored stream lists them as they came).
    std::shared_ptr<std::vector<double>> live_sse_at;
    if (!restored && tap_ && tap_->is_sse()) {
      sse_timer_.Stop();
      sse_drain();
      if (sse_live_) {
        live_sse_at = std::make_shared<std::vector<double>>();
        live_sse_at->reserve(sse_->events().size());
        for (const SseEvent& e : sse_->events()) live_sse_at->push_back(e.at_ms);
      }
    }
    const bool live_sse = live_sse_at != nullptr;
    sse_live_ = false;
    if (!restored) stop_live_head();
    if (restored && !extras_applied_ && r) {
      // The tab's own last response: what the view kept of that run (dialog time, scripts, TLS)
      // is in memory when it ran in this process, else on disk (read on a worker, then shown
      // again with it).
      const std::string key = extras_key(*r);
      if (auto x = rv::find_extras_cached(key)) {
        auto [rr, ss] = with_extras(*r, sent, *x);
        extras_applied_ = true;
        show_http(std::move(rr), std::move(ss));
        extras_applied_ = false;
        return;
      }
      if (!key.empty()) fetch_extras(key, r, sent, history_when_, 0);
    }
    // A body the history cut carries a note in its status text ("OK (body cut: history keeps the
    // first 64 KB of 10.2 MB)"): the status chip shows the status, the "first ... kept" segment
    // says the rest (from the size as received).
    uint64_t hist_cut = 0;
    if (restored && r) {
      const size_t at = r->status_text.find("(body cut: history keeps the first ");
      if (at != std::string::npos && r->sizes.response_body > r->body.size()) {
        auto c = std::make_shared<HttpResponse>(*r);
        c->status_text.resize(at);
        while (!c->status_text.empty() && c->status_text.back() == ' ') c->status_text.pop_back();
        hist_cut = c->sizes.response_body;
        r = std::move(c);
      }
    }
    const bool plain = sent ? sent->request.url.rfind("http:", 0) == 0 : r->effective_url.rfind("http:", 0) == 0;
    const ScriptRun* pre = sent && sent->pre_script ? sent->pre_script.get() : nullptr;
    // The shell starts its clock at the Send click, before any confirmation dialog (script trust,
    // Windows sign-in, environment variables): the time a dialog was open is not the request's.
    // It shows up as the shell's total exceeding the wall time since show_sending() (which comes
    // right after the dialogs): that excess is taken out of Queue and the total, and said so.
    double prompt_ms = 0;
    if (awaiting_ && send_t0_ != Clock::time_point{}) {
      const double wall = ms_between(send_t0_, Clock::now());
      const double pre_ms = pre && pre->ran ? pre->elapsed_us / 1000.0 : 0.0;
      // The post-response script ran after the shell's clock stopped, inside the wall time.
      const ScriptRun* post = sent && sent->post_script ? sent->post_script.get() : nullptr;
      const double post_ms = post && post->ran ? post->elapsed_us / 1000.0 : 0.0;
      const double excess = r->timing.total_ms - std::max(0.0, wall - post_ms);
      if (excess > kPromptMinMs) prompt_ms = std::min(excess, r->timing.queue_ms - pre_ms);
      if (prompt_ms <= kPromptMinMs) prompt_ms = 0;
    }
    awaiting_ = false;
    if (!restored) {
      // Kept with this run so a restore shows it as it is shown now (see response_keep.h).
      rv::RunExtras x;
      x.prompt_ms = prompt_ms;
      if (sent) {
        x.pre = sent->pre_script;
        x.post = sent->post_script;
        x.scripts_skipped_untrusted = sent->scripts_skipped_untrusted;
      }
      x.head = rv::response_head(*r);
      x.body_size = r->body.size();
      x.visualize = vis_chosen_by_request().count(vis_key()) > 0;
      if (live_sse_at)
        x.sse_at.assign(live_sse_at->begin(),
                        live_sse_at->begin() + static_cast<std::ptrdiff_t>(std::min(live_sse_at->size(), rv::kKeptSseTimes)));
      const std::string key = extras_key(*r);
      if (!key.empty()) {
        const RequestTab* t = tab();
        const Workspace* ws = t ? t->workspace() : nullptr;
        rv::keep_extras(ws ? ws->root() : std::filesystem::path(), key, std::move(x), r);
      }
      record_cookies(*r, sent.get());
    }
    if (prompt_ms > 0) {
      auto c = std::make_shared<HttpResponse>(*r);  // rare (a dialog was shown): the copy is fine
      c->timing.queue_ms -= prompt_ms;
      c->timing.total_ms -= prompt_ms;
      r = std::move(c);
    }
    ++view_seq_;
    Run run{r, sent,
            http_model(*r, plain, pre, sent ? sent->post_script.get() : nullptr, sent ? sent->token.get() : nullptr),
            wxDateTime::Now()};
    run.prompt_ms = prompt_ms;
    run.restored = restored;
    run.sse_live = live_sse;
    run.sse_at = live_sse_at;
    run.from_history = history_when_;
    // A run read from the history happened when the history says (UTC there): its Log line and
    // the History menu's "n min ago" show that time, not the moment the tab was opened.
    if (wxDateTime dt; history_when_.size() >= 19 && dt.ParseISOCombined(wxString(history_when_.substr(0, 19))))
      run.at = dt.FromTimezone(wxDateTime::UTC);
    run.body_not_kept = restored && body_not_kept_;
    run.not_kept_size = not_kept_size_;
    if (restored) {
      // The dialog time with_extras() took out: said so again, as it was live.
      run.prompt_ms = extras_applied_ && extras_prompt_ms_ > kPromptMinMs ? extras_prompt_ms_ : 0;
      run.cut_size = extras_applied_ ? extras_cut_size_ : 0;
      if (run.cut_size) run.body_key = extras_body_key_;
      if (!run.cut_size) run.cut_size = hist_cut;
      if (extras_applied_ && extras_sse_at_ && !extras_sse_at_->empty()) run.sse_at = extras_sse_at_;
      extras_sse_at_.reset();
      extras_prompt_ms_ = 0;
      extras_cut_size_ = 0;
      extras_body_key_.clear();
    }
    if (restored && !r->ok && r->status >= 100 && r->headers.size() > 0 && r->error != "cancelled") {
      // A transfer that failed after its head (a timeout mid-body), restored: shown as it was
      // live (status, headers, what arrived, "timed out after ..."), not as a bare failure.
      auto c = std::make_shared<HttpResponse>(*r);
      c->ok = true;
      run.resp = c;
      run.model = http_model(*c, plain, pre, sent ? sent->post_script.get() : nullptr, sent ? sent->token.get() : nullptr);
      for (size_t i = 0; i < run.model.phases.size(); ++i)
        if (run.model.phases[i].name == "Download") run.model.live_row = static_cast<int>(i);
      run.model.live_cut = true;
      for (Phase& ph : run.model.phases)
        if (ph.name == "Process") ph.has_value = false;
      run.cancelled_ms = r->timing.total_ms;
      run.cancelled_bytes = std::max<uint64_t>(r->sizes.response_body, r->body.size());
      run.cut_error = r->error;
      run.cut_label = cut_label_for(*r, r->timing.total_ms);
      run.model.live_head = run.cut_label + " \xC2\xB7 " + fmt::human_bytes(run.cancelled_bytes) + " received";
    } else if (!r->ok) {
      mark_failure(run.model, *r);
    }
    if (run.restored && r->tls_version.empty() && !plain && r->ok) run.model.conn_note += " \xC2\xB7 TLS";
    if (run.restored && r->timing.connection_reused) run.model.conn_note += " (at the time)";
    if (sent) {
      run.label = sent->request.method + " " + sent->request.url;
      run.key = owner_tag() + "http " + run.label + body_tag(sent->request.body);
    }
    // The delta compares with the previous successful run of the same request only, and only for
    // a response received now (a restored one may be that very run again: "+-0 vs previous").
    if (const Base* b = run.restored ? nullptr : base_for(run.key)) {
      run.base = b->model;
      run.base_at = b->at;
      run.has_base = true;
    }
    // A restored response from an earlier session was timed by the shell, confirmation dialogs
    // included (they cannot be told apart afterwards): a Queue that long is a dialog, not a
    // baseline worth comparing the next run with.
    const bool dialog_in_it = run.restored && r->timing.queue_ms > 250;
    if (r->ok && !dialog_in_it) remember(run.key, run.model);
    // Its captures are applied (not when an earlier run is re-shown, nor for a restored one:
    // opening a request must not change variables).
    fresh_response_ = !run.restored;
    // Keep the last runs of this tab (at most kRuns, and at most ~64 MB of bodies) so an earlier
    // response can be looked at again and compared with the current one.
    runs_.push_back(std::move(run));
    size_t bytes = 0;
    for (const Run& x : runs_) bytes += x.resp->body.size();
    while (runs_.size() > kRuns || (runs_.size() > 1 && bytes > (64u << 20))) {
      bytes -= runs_.front().resp->body.size();
      runs_.erase(runs_.begin());
    }
    show_run(runs_.size() - 1);
  }

  // Renders run i of this tab's HTTP history (the newest one after a send). Deltas compare with
  // the run of the same request before it.
  void show_run(size_t i) {
    body_->set_stale(false);
    showing_grpc_ = false;
    shown_run_ = i;
    update_history_chip();
    const Run& run = runs_[i];
    std::shared_ptr<const HttpResponse> r = run.resp;
    const std::shared_ptr<const SentHttp>& sent = run.sent;
    const Timing& t = r->timing;
    const TimingModel& model = run.model;
    const TimingModel* before = (r->ok && run.has_base) ? &run.base : nullptr;
    std::vector<StatusSeg> segs;
    if (!r->ok) {
      const std::string where = classify_failure(*r).text;
      segs.push_back({fmt::ms(t.total_ms), "Time until the failure.", Tone::Plain, true});
      segs.push_back({shown_error(*r), where + "\n" + r->error, Tone::Err, false});
      add_script_segs(segs, run);
      status_->set("Failed", Tone::Err, std::move(segs));
      wf_->set(model, nullptr);
      body_->show_message("Request failed: " + shown_error(*r) + "\n\n" + where + "\n");
      if (run.sse_live && sse_->count() > 0) {
        // An event stream cut by an error (a timeout, the server going away): its events stay.
        sse_resp_ = r;
        sse_show(true);
        sse_->set_caption("stream ended: " + shown_error(*r) + " after " + fmt::ms(t.total_ms) + " \xC2\xB7 " +
                              std::to_string(sse_->count()) + " events",
                          Tone::Err);
        tabs_->set_label(kBody, "Events", std::to_string(sse_->count()));
      } else {
        sse_off();
      }
      headers_->set({});
      tabs_->set_label(kHeaders, "Headers");
      set_kind_tabs(false, true);
      set_cookies(*r, run);
      set_request(sent.get(), r.get());
      set_log(run);
      set_script_results(sent.get());
      // No response: no check runs against the failed transport's figures (its responseTime is
      // the time until the failure). watt --run and the runner count the request failed.
      ++test_seq_;
      tests_->set_no_response("request failed, no response");
      update_tests_label();
      return;
    }
    std::string badge = std::to_string(r->status);
    if (!r->status_text.empty()) badge += " " + r->status_text;
    else if (r->status == 200) badge += " OK";
    segs.push_back({fmt::ms(model.total),
                    model.before > 0 ? "Total: request start -> complete response (the sum of the request's phases; "
                                       "the pre-request script before it, " + fmt::ms(model.before) + ", is not counted)."
                                     : std::string("Total: Send click -> complete response (the sum of all phases)."),
                    Tone::Plain, true});
    // Server / client split: one segment, kept longer than the "vs previous" delta (the
    // waterfall's footer has the delta too), so the headline reads the same on every send.
    segs.push_back({"server " + fmt::ms(model.server_wait) +
                        (model.auth >= 0 ? " \xC2\xB7 auth " + fmt::ms(model.auth) : std::string()) +
                        " \xC2\xB7 client " + fmt::ms(model.client),
                    model.server_tip + (model.auth >= 0 ? "\n\n" + model.auth_tip : std::string()) + "\n\n" +
                        model.client_tip + "\nNetwork (DNS, connect, TLS, transfer): " + fmt::ms(model.network) + ".",
                    Tone::Dim, false, 2});
    const HttpSizes& s = r->sizes;
    std::string size_tip = "Body " + fmt::human_bytes(s.response_body);
    if (s.response_body_wire != s.response_body)
      size_tip += " (" + fmt::human_bytes(s.response_body_wire) + " on the wire, " +
                  std::string(r->headers.get("content-encoding")) + ")";
    size_tip += ", headers " + fmt::human_bytes(s.response_headers) + ".\nSent: headers " +
                fmt::human_bytes(s.request_headers) + ", body " + fmt::human_bytes(s.request_body) +
                ".\nOn the wire in total: " + fmt::human_bytes(t.bytes_received) + " received, " +
                fmt::human_bytes(t.bytes_sent) + " sent.";
    std::string size = fmt::human_bytes(s.response_body);
    if (s.response_body_wire != s.response_body && s.response_body_wire)
      size += " (" + fmt::human_bytes(s.response_body_wire) + " wire)";
    segs.push_back({size, size_tip, Tone::Plain, false, 2});
    const bool earlier = i + 1 < runs_.size();
    if (earlier) {
      // An earlier run may be of another request than the tab shows now: say which, first.
      const std::string tag = run.label.empty() ? std::string("earlier run") : "earlier run: " + request_tag(run.label);
      segs.insert(segs.begin() + 1, StatusSeg{tag, "You are looking at an earlier response of this tab (History):\n" +
                                                       run.label, Tone::Warn, true, 0});
    }
    const std::string remote = host_port(r->remote_ip, r->remote_port);
    {
      std::string proto = r->http_version.empty() ? std::string("connection") : r->http_version;
      if (!r->tls_version.empty()) proto += " \xC2\xB7 " + r->tls_version;
      else if (run.restored && r->effective_url.rfind("https:", 0) == 0) proto += " \xC2\xB7 TLS";
      const TlsInfo* cti = r->conn.tls_info.get();
      const bool mtls = cti && !cti->client_cert_subject.empty();
      if (mtls) proto += " \xC2\xB7 mTLS";
      if (!r->conn.proxy.empty()) proto += " \xC2\xB7 via proxy";
      std::string tip = "Protocol of the final response";
      tip += r->tls_version.empty() ? "." : " (negotiated via ALPN), " + r->tls_version + " " + r->tls_cipher + ".";
      if (mtls) tip += "\nMutual TLS: sent the client certificate " + cti->client_cert_subject + ".";
      if (!r->conn.proxy.empty()) tip += "\nThrough the proxy " + r->conn.proxy + ".";
      if (!remote.empty()) tip += "\nRemote " + remote;
      if (!r->local_ip.empty()) tip += ", local " + host_port(r->local_ip, r->local_port);
      tip += "\nClick for the connection and certificate details.";
      StatusSeg seg{proto + " \xE2\x96\xBE", tip, Tone::Info, false, 3};
      seg.click = 1;
      segs.push_back(std::move(seg));
    }
    if (!remote.empty())
      segs.push_back({remote, "Remote address that served the final response.\n" + r->effective_url, Tone::Dim,
                      false, 8});
    if (!run.label.empty() && !earlier)
      segs.push_back({request_tag(run.label), "This response belongs to:\n" + run.label, Tone::Dim, false, 7});
    if (r->redirect_count > 0)
    {
      StatusSeg seg{std::to_string(r->redirect_count) + (r->redirect_count == 1 ? " redirect \xE2\x96\xBE" : " redirects \xE2\x96\xBE"),
                    "Followed redirects; final URL:\n" + r->effective_url + "\nClick for each hop (status, URL, time).",
                    Tone::Redirect, false, 3};
      seg.click = 2;
      segs.push_back(std::move(seg));
    }
    if (before) add_delta(segs, model, *before, run.label, run.base_at);
    if (run.cancelled_ms >= 0) {
      if (run.cut_label.empty())
        segs.insert(segs.begin(), StatusSeg{"cancelled after " + fmt::ms(run.cancelled_ms),
                                            "Cancelled by you (Esc or the Cancel button) while the body arrived: the "
                                            "status, headers and phases up to Wait (TTFB) are as received, the body is "
                                            "what arrived before the cancel.\nCtrl+Enter sends it again.",
                                            Tone::Warn, true, 0});
      else
        segs.insert(segs.begin(), StatusSeg{run.cut_label,
                                            "The transfer failed while the body arrived: " + run.cut_error +
                                                "\nThe status, headers and phases up to Wait (TTFB) are as received, "
                                                "the body is what arrived before it.\nCtrl+Enter sends it again.",
                                            Tone::Err, true, 0});
      const char* when = run.cut_label.empty() ? "the cancel" : "the failure";
      for (StatusSeg& sg : segs)
        if (sg.text == fmt::human_bytes(s.response_body)) {
          sg.text += " received";
          sg.tip = std::string("Body bytes that arrived before ") + when + " (the response was not complete).\n" + sg.tip;
        }
    }
    add_script_segs(segs, run);
    if (run.restored) segs.insert(segs.begin() + 1, run.from_history.empty() ? restored_seg(r.get()) : history_seg(run.from_history));
    if (run.cut_size > 0)
      segs.insert(segs.begin() + 2, StatusSeg{"first " + fmt::human_bytes(r->body.size()) + " of " +
                                                  fmt::human_bytes(run.cut_size) + " kept",
                                              "The history keeps the start of a large body only; status, headers and "
                                              "timing are as received.\nCtrl+Enter sends the request again.",
                                              Tone::Dim, false, 2});
    if (run.body_not_kept)
      segs.insert(segs.begin() + 2, StatusSeg{"body not kept (" + fmt::human_bytes(run.not_kept_size) + ")",
                                              "Responses over the history's size limit are kept without their body.",
                                              Tone::Dim, false, 2});
    if (!run.example.empty())
      segs.insert(segs.begin() + 1, StatusSeg{"example \xC2\xB7 " + run.example,
                                              "A saved example of this request, not a live response.\nCtrl+Enter "
                                              "sends the request; Examples (right) lists the saved ones.",
                                              Tone::Info, true, 0});
    TraceLaps tl("run");
    status_->set(badge, http_tone(r->status), std::move(segs));
    tl.lap("status");
    wf_->set(model, before);
    tl.lap("wf");
    if (run.restored) {
      // The rest (pages, body, checks) in the next UI turn: a restored run is shown as the tab
      // opens, when the UI thread is busiest (see show_http).
      CallAfter([this, i, resp = run.resp, seq = view_seq_, alive = alive_] {
        if (!*alive || seq != view_seq_ || shown_run_ != i || i >= runs_.size() || runs_[i].resp != resp) return;
        show_run_rest(i);
      });
      return;
    }
    show_run_rest(i);
  }

  // The extras of a restored response read from disk on a worker, then the run shown again with
  // them, as long as it is still this view's newest and shown run. At start-up a tab can restore
  // its response before it has its workspace: retried a few times (200 ms apart) until it has.
  void fetch_extras(const std::string& key, std::shared_ptr<const HttpResponse> r,
                    std::shared_ptr<const SentHttp> sent, const std::string& hw, int tries) {
    const RequestTab* t = tab();
    const Workspace* ws = t ? t->workspace() : nullptr;
    const bool ready = ws && !ws->root().empty();
    if (!ready && tries >= 15) return;
    std::thread([root = ready ? ws->root() : std::filesystem::path(), ready, key, r, sent, hw, tries,
                 alive = alive_, this] {
      std::optional<rv::RunExtras> x;
      if (ready) x = rv::find_extras(root, key);
      else std::this_thread::sleep_for(std::chrono::milliseconds(200));
      if ((ready && !x) || !wxTheApp) return;
      wxTheApp->CallAfter([alive, this, ready, key, r, sent, x, hw, tries] {
        if (!*alive || runs_.empty() || runs_.back().resp != r || shown_run_ + 1 != runs_.size() || showing_grpc_)
          return;
        if (!ready) return fetch_extras(key, r, sent, hw, tries + 1);
        runs_.pop_back();
        auto [rr, ss] = with_extras(*r, sent, *x);
        extras_applied_ = true;
        history_when_ = hw;
        show_http(std::move(rr), std::move(ss));
        history_when_.clear();
        extras_applied_ = false;
      });
    }).detach();
  }

  // Run i was restored with the start of its body only: reads the whole body response_keep kept
  // for it (worker), then replaces the run's response and shows it again (the body is formatted
  // on the body view's worker like a live one: one short UI step).
  void load_whole_body(size_t i) {
    Run& run = runs_[i];
    const std::string key = std::move(run.body_key);
    run.body_key.clear();
    const RequestTab* t = tab();
    const Workspace* ws = t ? t->workspace() : nullptr;
    if (!ws || ws->root().empty()) return;
    std::thread([root = ws->root(), key, size = run.cut_size, resp = run.resp, alive = alive_, this] {
      std::shared_ptr<std::string> body = rv::load_kept_body(root, key, size);
      if (!body || !wxTheApp) return;
      auto full = std::make_shared<HttpResponse>(*resp);
      full->body = std::move(*body);
      wxTheApp->CallAfter([alive, this, resp, full = std::shared_ptr<const HttpResponse>(std::move(full))] {
        if (!*alive) return;
        for (size_t k = 0; k < runs_.size(); ++k) {
          if (runs_[k].resp != resp) continue;
          runs_[k].resp = full;
          runs_[k].cut_size = 0;
          if (shown_run_ == k && !showing_grpc_) show_run(k);
          return;
        }
      });
    }).detach();
  }

  // Everything below the status line and the waterfall of run i.
  void show_run_rest(size_t i) {
    TraceLaps tl("run");
    const Run& run = runs_[i];
    std::shared_ptr<const HttpResponse> r = run.resp;
    const std::shared_ptr<const SentHttp>& sent = run.sent;
    KV h;
    h.reserve(r->headers.size());
    for (const auto& [k, v] : r->headers) h.emplace_back(std::string(k), std::string(v));
    headers_->set(std::move(h));
    tabs_->set_label(kHeaders, "Headers", std::to_string(r->headers.size()));
    tl.lap("headers");
    set_kind_tabs(false, true);
    tl.lap("kind_tabs");
    set_cookies(*r, run);
    tl.lap("cookies");
    set_request(sent.get(), r.get());
    tl.lap("request");
    set_log(run);
    tl.lap("log");
    set_script_results(sent.get());
    tl.lap("scripts");
    if (!run.body_not_kept && (run.sse_live || is_sse_response(*r))) sse_for_run(run);
    else sse_off();
    tl.lap("sse");
    if (run.body_not_kept) {
      body_->show_message("The body of this response (" + fmt::human_bytes(run.not_kept_size) +
                          ") was too large to keep with the history: status, headers and timing are as received."
                          "\n\nCtrl+Enter sends the request again.");
      ++test_seq_;
      tests_->set_no_response("the body of this run was not kept: send again to check");
      update_tests_label();
      return;
    }
    // A body the history cut: the whole body, when this view kept it with the run, is read on a
    // worker and the run shown again with it (the cut copy meanwhile).
    if (run.cut_size > 0 && r->body.size() < run.cut_size && !run.body_key.empty()) load_whole_body(i);
    tl.lap("whole");
    // A body the history cut (and indented, for JSON): Raw / Hex show the kept bytes, and say so.
    if (run.cut_size > 0 && r->body.size() < run.cut_size) {
      const std::string_view b = r->body;
      const bool indented = b.size() > 2 && (b[0] == '{' || b[0] == '[') && (b[1] == '\n' || b[1] == '\r');
      body_->set_origin_note(indented ? "history copy, indented" : "history copy");
    } else {
      body_->set_origin_note({});
    }
    // pm.visualizer.set() of this run (live, or kept with a restored run): the Visualize chip, open
    // when it was the view last picked for this request.
    body_->set_visualize_chosen(vis_chosen_by_request().count(vis_key()) > 0);
    tl.lap("vis_key");
    body_->set_visualizer(sent ? sent->post_script : nullptr);
    tl.lap("vis");
    if (run.cancelled_ms >= 0 && r->body.empty() && run.cancelled_bytes > 0 && !run.sse_live) {
      body_->show_message("The " + fmt::human_bytes(run.cancelled_bytes) + " of the body that arrived before it was cut (" +
                          (run.cut_label.empty() ? "cancelled by you" : run.cut_label) +
                          ") were not kept with this run.\n\nCtrl+Enter sends the request again.");
      ++test_seq_;
      tests_->set_no_response("incomplete response: not checked");
      update_tests_label();
      return;
    }
    if (run.cancelled_ms >= 0) {
      const bool cut = r->body.size() < run.cancelled_bytes;
      body_->set_origin_note((run.cut_label.empty() ? "cancelled after " + fmt::ms(run.cancelled_ms) : run.cut_label) + ": " +
                             (cut ? "the first " + fmt::human_bytes(r->body.size()) + " of " +
                                        fmt::human_bytes(run.cancelled_bytes)
                                  : fmt::human_bytes(run.cancelled_bytes) + " received, incomplete"));
    }
    body_->set_http(std::move(r));
    tl.lap("body");
    if (run.cancelled_ms >= 0) {
      ++test_seq_;
      tests_->set_no_response(run.cut_label.empty() ? std::string("cancelled while the response arrived: not checked")
                                                    : run.cut_label + " while the response arrived: not checked");
      tests_->set_script({});
      update_tests_label();
      return;
    }
    run_tests();
    tl.lap("tests");
  }

  void append_grpc_messages(const std::vector<GrpcMessageEvent>& batch) override {
    if (batch.empty()) return;
    if (!live_) start_stream();
    if (first_ms_ < 0) first_ms_ = batch.front().at_ms > 0 ? batch.front().at_ms : ms_between(send_t0_, Clock::now());
    body_->append_messages(batch.data(), batch.size(), msgs_->count());
    msgs_->append(batch.data(), batch.size());
    wf_->set_live(first_ms_, 0, msgs_->count());
    tabs_->set_label(kMessages, "Messages", std::to_string(msgs_->count()));
    status_->set_busy_label("Streaming \xC2\xB7 " + std::to_string(msgs_->count()) +
                            (msgs_->count() == 1 ? " message" : " messages"));
  }

  void show_grpc(std::shared_ptr<const GrpcResponse> r) override {
    // Without show_sending() before it: restored (earlier session / history), never "sent now".
    const bool restored = !awaiting_;
    awaiting_ = false;
    ++view_seq_;
    sse_off();
    // A restored call was not sent from this view: its target is the request's (the chip of a
    // restored gRPC-Web call reads gRPC-Web, not the last call's h2c).
    if (restored)
      if (const RequestTab* t = tab(); t && t->request().kind == RequestKind::Grpc) {
        const GrpcTarget& gt = t->request().grpc_target;
        grpc_meta_ = GrpcConnMeta{gt.address, gt.authority, gt.server_name, gt.tls || gt.address.rfind("https:", 0) == 0,
                                  !gt.client_cert_pem.empty(), gt.web};
      }
    const size_t shown = live_ ? msgs_->count() : 0;
    if (!live_) start_stream();
    if (r->messages.size() > shown) {
      const size_t n = r->messages.size() - shown;
      body_->append_messages(r->messages.data() + shown, n, shown);
      msgs_->append(r->messages.data() + shown, n);
    }
    live_ = false;
    showing_grpc_ = true;
    fresh_response_ = !restored;
    grpc_last_ = r;
    grpc_shown_meta_ = grpc_meta_;
    const Timing& t = r->timing;
    TimingModel model = grpc_model(*r);
    if (r->status_code != 14 || !r->messages.empty())
      model.conn_note += grpc_meta_.tls ? (grpc_meta_.mtls ? " \xC2\xB7 mTLS" : " \xC2\xB7 TLS") : " \xC2\xB7 plaintext";
    std::vector<StatusSeg> segs;
    const bool paced = r->pacing.on && model.paced >= 0;
    segs.push_back({fmt::ms(t.total_ms),
                    paced ? "Total: " + model.total_tip : std::string("Total: Send click -> final status (the sum of all phases)."),
                    Tone::Plain, true});
    if (paced)
      segs.push_back({"server " + fmt::ms(model.server_wait) + " after " +
                          (r->pacing.end_ms >= 0 ? std::string("End") : std::string("your last message")),
                      model.server_tip + "\nUser-paced (open, idle and your sends): " + fmt::ms(model.paced) +
                          ". res.responseTime evaluates the server part for a Manual stream.",
                      Tone::Dim, false, 2});
    segs.push_back({"client " + fmt::ms(model.client),
                    model.client_tip + "\nServer: " + fmt::ms(model.server_wait) + ", network: " +
                        fmt::ms(model.network) + ".",
                    Tone::Dim, false, 4});
    uint64_t wire = 0;
    for (const auto& m : r->messages) wire += m.wire_bytes;
    const size_t n = r->messages.size();
    segs.push_back({std::to_string(n) + (n == 1 ? " message" : " messages"),
                    "Messages received; see the Messages tab for per-message offsets and sizes.", Tone::Plain, false,
                    3});
    segs.push_back({fmt::human_bytes(wire),
                    "Message payload bytes received (protobuf wire size). Total on the wire: " +
                        fmt::human_bytes(t.bytes_received) + " received, " + fmt::human_bytes(t.bytes_sent) + " sent.",
                    Tone::Plain, false, 6});
    const std::string status_msg = one_line(r->status_message);
    if (!r->ok && !status_msg.empty())
      segs.push_back({status_msg, r->error_details.empty() ? r->status_message : r->error_details, Tone::Err, false});
    if (restored) segs.insert(segs.begin() + 1, grpc_history_when_.empty() ? restored_seg() : history_seg(grpc_history_when_));
    {
      std::string text = grpc_meta_.web ? "gRPC-Web" : grpc_meta_.tls ? (grpc_meta_.mtls ? "h2 \xC2\xB7 mTLS" : "h2 \xC2\xB7 TLS")
                                                                       : "h2c";
      StatusSeg seg{text + " \xE2\x96\xBE",
                    "Target " + grpc_meta_.address + (r->peer.empty() ? std::string() : "\nResolved " + r->peer) +
                        "\nClick for the channel details.",
                    Tone::Info, false, 3};
      seg.click = 1;
      segs.push_back(std::move(seg));
    }
    // Same target, method, body and outcome only: a failing call is never compared with a
    // successful one, nor Count with Say.
    // A restored response is never compared (grpc_key_ is of the last call sent, if any).
    const std::string key = restored || grpc_key_.empty() ? std::string()
                                                          : grpc_key_ + " status " + std::to_string(r->status_code) +
                                                                (paced ? " paced" : "");
    const Base* base = base_for(key);
    if (!grpc_label_.empty() && !restored)
      segs.push_back({request_tag(grpc_label_), "This response belongs to:\n" + grpc_label_, Tone::Dim, false, 7});
    if (base) add_delta(segs, model, base->model, grpc_label_, base->at);
    Tone tone = Tone::Ok;
    if (!r->ok) tone = (r->status_code == 1 || r->status_code == 4) ? Tone::Warn : Tone::Err;
    // The finished timing before the status strip: no frame shows "OK, 200 messages" over a
    // Timing line still reading "streaming, 199 messages".
    wf_->set(model, base ? &base->model : nullptr);
    status_->set(r->status_name.empty() ? std::string("ERROR") : r->status_name, tone, std::move(segs));
    // Not a transport failure (UNAVAILABLE, DEADLINE_EXCEEDED, CANCELLED): remember for the next delta.
    if (r->status_code != 14 && r->status_code != 4 && r->status_code != 1) remember(key, model);
    if (!r->ok) {
      std::string s = "status " + r->status_name + " (" + std::to_string(r->status_code) + ")";
      if (!status_msg.empty()) s += ": " + status_msg;
      if (n == 0) {
        body_->show_grpc_error("gRPC " + s, r->error_details);
      } else {
        body_->append_note(s);
      }
    } else if (n == 0) {
      body_->show_message("(no messages)");
    }
    body_->finish_grpc(r);
    headers_->set(r->headers);
    KV tr = r->trailers;
    for (auto& [k, v] : tr)
      if (k == "grpc-status-details-bin" && !r->error_details.empty()) {
        std::string one;
        v = "decoded: " + (fmt::compact_json(r->error_details, one, 4096) ? one : r->error_details);
      }
    trailers_->set(std::move(tr));
    tabs_->set_label(kHeaders, "Headers", std::to_string(r->headers.size()));
    tabs_->set_label(kTrailers, "Trailers", std::to_string(r->trailers.size()));
    tabs_->set_label(kMessages, "Messages", std::to_string(msgs_->count()));
    set_grpc_log(*r);
    tests_->set_script({});
    run_tests();
  }

  void show_cancelled(double after_ms) override {
    ++view_seq_;
    // HTTP: what arrived before the cancel (an event stream's events, the start of a body) stays.
    if (kind_ == RequestKind::Http && awaiting_ && tap_ && tap_->head_response())
      return show_cancelled_with_head(after_ms, tap_->head_response(), tap_->sent_request(), nullptr);
    stop_live_head();
    if (kind_ == RequestKind::Http && awaiting_ && tap_ && tap_->bytes() > 0) return show_cancelled_partial(after_ms);
    sse_timer_.Stop();
    sse_live_ = false;
    sse_off();
    const bool streamed = live_ && msgs_->count() > 0;
    live_ = false;
    awaiting_ = false;
    ++test_seq_;
    tests_->set_no_response("cancelled, no response");
    tests_->set_script({});
    update_tests_label();
    body_->set_stale(false);
    // Nothing of an earlier run is shown under "Cancelled": the timing keeps its height (the body
    // does not jump) and says the request was cancelled; the earlier responses stay one click away
    // in the History chip.
    wf_->set_cancelled(after_ms);
    std::vector<StatusSeg> segs;
    segs.push_back({"after " + fmt::ms(after_ms), "Cancelled by you (Esc or the Cancel button).", Tone::Plain, true});
    if (streamed) {
      // Messages of this very call arrived before the cancel: they stay, with a note.
      segs.push_back({std::to_string(msgs_->count()) + " messages before the cancel",
                      "These messages arrived before you cancelled the call.", Tone::Dim, false, 3});
      body_->append_note("cancelled by you after " + fmt::ms(after_ms));
    } else {
      showing_grpc_ = false;
      grpc_last_.reset();
      body_->show_message("Cancelled after " + fmt::ms(after_ms) + ": no response was received.\n\n" +
                          (runs_.empty() ? std::string("Ctrl+Enter sends it again.")
                                         : "Ctrl+Enter sends it again. Earlier responses: \"Earlier runs\" (right)."));
      headers_->set({});
      trailers_->set({});
      msgs_->reset();
      cookies_->set({});
      request_->set({});
      fill_log("* Cancelled by you after " + fmt::ms(after_ms) + ": no response.\n");
      set_kind_tabs(false, false);
      tabs_->set_label(kRequest, "Request");
      shown_run_ = runs_.size();  // none of the kept runs is on screen
      update_history_chip();
    }
    if (!runs_.empty() && !streamed)
      segs.push_back({"previous response not shown", "The last response is kept: open it from \"Earlier runs\".",
                      Tone::Dim, false, 4});
    status_->set("Cancelled", Tone::Warn, std::move(segs));
  }

  // An HTTP request cancelled after its head arrived (an event stream that never ends, a slow
  // download): a run of its own like any response, with the status, headers, connection and the
  // phases up to Wait (TTFB) as measured, Download cut at the cancel, and the events or body
  // bytes that arrived. It stays in "Earlier runs" to look at again.
  // `failed`: not a cancel but a transfer error after the head (a timeout, a reset): the run is
  // the same, labelled with the error instead of "cancelled".
  void show_cancelled_with_head(double after_ms, std::shared_ptr<const HttpResponse> h,
                                std::shared_ptr<const SentHttp> sent, const HttpResponse* failed) {
    TraceScope ts("cancel_head");
    sse_timer_.Stop();
    stop_live_head();
    const bool sse = tap_->is_sse();
    if (sse) sse_drain();
    live_ = false;
    awaiting_ = false;
    sse_live_ = false;
    stale_timer_.Stop();
    const uint64_t got = tap_->bytes();
    auto r = std::make_shared<HttpResponse>(*h);
    r->ok = true;
    if (sse) {
      r->body = std::move(sse_raw_bytes_);
      sse_raw_bytes_.clear();
    } else if (failed && !failed->body.empty()) {
      r->body = failed->body;
    } else {
      r->body = tap_->partial();
    }
    r->sizes.response_body = got;
    r->sizes.response_body_wire = got;
    const double dl = std::max(0.0, after_ms - h->timing.total_ms);
    r->timing.download_ms = dl;
    r->timing.total_ms = h->timing.total_ms + dl;
    r->timing.bytes_received += got;
    const bool plain = r->effective_url.rfind("http:", 0) == 0;
    Run run{r, std::move(sent), http_model(*r, plain), wxDateTime::Now()};
    run.cancelled_ms = after_ms;
    run.cancelled_bytes = got;
    if (failed) {
      run.cut_error = failed->error.empty() ? std::string("the transfer failed") : failed->error;
      run.cut_label = cut_label_for(*failed, after_ms);
    }
    const std::string why = run.cut_label.empty() ? std::string("your cancel") : run.cut_label;
    for (size_t i = 0; i < run.model.phases.size(); ++i)
      if (run.model.phases[i].name == "Download") {
        run.model.live_row = static_cast<int>(i);
        run.model.phases[i].tip = "First response byte -> " + why + ": the transfer was cut there (" +
                                  fmt::human_bytes(got) + " of the body had arrived). Every phase before it is "
                                  "as measured.";
      }
    run.model.live_cut = true;
    for (Phase& ph : run.model.phases)
      if (ph.name == "Process") ph.has_value = false;  // never ran: the transfer was cut
    run.model.live_head = (run.cut_label.empty() ? std::string("cancelled by you") : run.cut_label) + " \xC2\xB7 " +
                          fmt::human_bytes(got) + " received";
    run.model.total_tip = "Send click -> " + why + " (the response was not complete).";
    if (sse && sse_->count() > 0) {
      run.sse_live = true;
      auto at = std::make_shared<std::vector<double>>();
      at->reserve(sse_->events().size());
      for (const SseEvent& e : sse_->events()) at->push_back(e.at_ms);
      run.sse_at = std::move(at);
    }
    if (run.sent) {
      run.label = run.sent->request.method + " " + run.sent->request.url;
      run.key = owner_tag() + "http " + run.label + body_tag(run.sent->request.body);
    } else if (const RequestTab* t = tab()) {
      run.label = t->request().http.method + " " + r->effective_url;
    }
    fresh_response_ = false;
    runs_.push_back(std::move(run));
    size_t bytes = 0;
    for (const Run& x : runs_) bytes += x.resp->body.size();
    while (runs_.size() > kRuns || (runs_.size() > 1 && bytes > (64u << 20))) {
      bytes -= runs_.front().resp->body.size();
      runs_.erase(runs_.begin());
    }
    show_run(runs_.size() - 1);
  }

  // "timed out after 30000 ms" (curl: "Operation timed out after 30000 milliseconds with ..."),
  // else "failed after 1.2 s".
  static std::string cut_label_for(const HttpResponse& f, double after_ms) {
    const std::string& e = f.error;
    if (e.find("timed out") != std::string::npos || e.find("Timeout") != std::string::npos) {
      const size_t a = e.find(" after ");
      if (a != std::string::npos) {
        size_t k = a + 7;
        long v = 0;
        while (k < e.size() && e[k] >= '0' && e[k] <= '9' && v < 100000000) v = v * 10 + (e[k++] - '0');
        if (k > a + 7 && e.compare(k, 13, " milliseconds") == 0) return "timed out after " + std::to_string(v) + " ms";
      }
      return "timed out after " + fmt::ms(after_ms);
    }
    return "failed after " + fmt::ms(after_ms);
  }

  // An HTTP request cancelled while its body arrived: the events or the bytes so far, the time to
  // the first of them and the partial transfer in the waterfall, marked cancelled. The engine
  // hands the status line and headers over only with a complete response, so they are not shown.
  void show_cancelled_partial(double after_ms) {
    sse_timer_.Stop();
    const bool sse = tap_->is_sse();
    if (sse) sse_drain();
    live_ = false;
    awaiting_ = false;
    sse_live_ = false;
    const uint64_t got = tap_->bytes();
    const double fb = tap_->first_ms();
    const size_t events = sse ? sse_->count() : 0;
    ++test_seq_;
    tests_->set_no_response("cancelled while the response arrived: not checked");
    tests_->set_script({});
    update_tests_label();
    stale_timer_.Stop();
    body_->set_stale(false);
    wf_->set_live(fb, got, 0);
    wf_->set_cancelled(after_ms);
    std::vector<StatusSeg> segs;
    segs.push_back({"after " + fmt::ms(after_ms), "Cancelled by you (Esc or the Cancel button) while the response arrived.",
                    Tone::Plain, true});
    if (sse)
      segs.push_back({std::to_string(events) + (events == 1 ? " event" : " events"),
                      "Events that arrived before the cancel: they stay listed in the Body tab.", Tone::Plain, false, 2});
    segs.push_back({fmt::human_bytes(got) + " received",
                    "Body bytes that arrived before the cancel (the response was not complete).", Tone::Plain, false, 3});
    if (fb >= 0)
      segs.push_back({"first body byte +" + fmt::ms(fb),
                      "The first body bytes arrived then (the status line and headers came before them).", Tone::Dim,
                      false, 4});
    segs.push_back({"headers not kept",
                    "A cancelled transfer ends without its status line, headers and per-phase split (the engine hands "
                    "them over with a complete response only). What arrived of the body is shown.\nCtrl+Enter sends "
                    "it again.",
                    Tone::Dim, false, 6});
    status_->set("Cancelled", Tone::Warn, std::move(segs));
    headers_->set({});
    trailers_->set({});
    msgs_->reset();
    cookies_->set({});
    request_->set({});
    std::string log = "* Cancelled by you after " + fmt::ms(after_ms) + ": " + fmt::human_bytes(got) +
                      " of the body had arrived";
    if (fb >= 0) log += " (the first at +" + fmt::ms(fb) + ")";
    if (sse) log += ", " + std::to_string(events) + (events == 1 ? " event" : " events");
    log += ".\n* The status line and headers are not kept for a cancelled transfer.\n";
    fill_log(std::move(log));
    set_kind_tabs(false, false);
    tabs_->set_label(kRequest, "Request");
    shown_run_ = runs_.size();  // none of the kept runs is on screen
    update_history_chip();
    auto r = std::make_shared<HttpResponse>();
    r->ok = true;
    if (sse) {
      r->body = std::move(sse_raw_bytes_);
      sse_raw_bytes_.clear();
      r->headers.add("Content-Type", "text/event-stream");
    } else {
      r->body = tap_->partial();
    }
    const bool cut = r->body.size() < got;
    body_->set_origin_note("cancelled after " + fmt::ms(after_ms) + ": " +
                           (cut ? "the first " + fmt::human_bytes(r->body.size()) + " of " + fmt::human_bytes(got)
                                : fmt::human_bytes(got) + " received, incomplete"));
    body_->set_visualizer(nullptr);
    body_->set_http(r);
    if (sse) {
      sse_resp_ = r;
      sse_show(true);
      sse_->set_caption("cancelled by you after " + fmt::ms(after_ms) + " \xC2\xB7 " + std::to_string(events) +
                            (events == 1 ? " event" : " events") + " \xC2\xB7 " + fmt::human_bytes(got),
                        Tone::Warn);
      tabs_->set_label(kBody, "Events", std::to_string(events));
    } else {
      sse_off();
    }
  }

  void show_error(const std::string& message) override {
    ++view_seq_;
    sse_timer_.Stop();
    sse_live_ = false;
    sse_off();
    live_ = false;
    awaiting_ = false;
    ++test_seq_;
    showing_grpc_ = false;
    tests_->set_no_response("not sent: " + (message.size() > 160 ? message.substr(0, 160) + "\xE2\x80\xA6" : message));
    tests_->set_script({});
    update_tests_label();
    body_->set_stale(false);
    status_->set("Error", Tone::Err, {{message, message, Tone::Err, false}});
    wf_->clear(false);
    body_->show_message(message);
    headers_->set({});
    trailers_->set({});
    msgs_->reset();
    cookies_->set({});
    request_->set({});
    fill_log("* " + message + "\n");
    set_kind_tabs(false, false);
    tabs_->set_label(kRequest, "Request");
  }

  void show_paced(int sent, bool ended) override {
    if (!awaiting_ || kind_ != RequestKind::Grpc) return;
    wf_->set_paced(sent, ended);
    if (live_) return;  // replies are arriving: the strip counts them
    waiting_msg_ = true;  // the body says what the stream waits for, not "waiting for the response"
    body_->set_stale(false);
    body_->show_message(sent == 0 ? (ended ? std::string("Stream ended by you before any message: waiting for the server's "
                                                         "final status.")
                                           : std::string("Stream open (Manual). Nothing is sent yet: Send #1 when you are "
                                                         "ready, or End to half-close.\n\nThe server's replies appear "
                                                         "here as they arrive, each with its offset from the stream "
                                                         "opening. Esc cancels."))
                                  : std::to_string(sent) + (sent == 1 ? " message" : " messages") +
                                        " sent: waiting for the server's reply\xE2\x80\xA6\n\nEsc cancels.");
    status_->set_busy_label(sent == 0 ? (ended ? std::string("Ended \xC2\xB7 waiting for the trailers")
                                               : std::string("Stream open \xC2\xB7 nothing sent yet"))
                                      : "Waiting for the reply \xC2\xB7 " + std::to_string(sent) + " sent" +
                                            (ended ? ", ended" : ""));
  }

  void show_waiting_for_user(const std::string& what) override {
    if (!awaiting_) return;
    ++view_seq_;  // the stale-body timer of this send does nothing now
    stale_timer_.Stop();
    stop_live_head();
    const std::string msg = "Not sent yet: waiting for your answer (" + what +
                            ").\n\nThe time you take is not counted: the clock starts when the request goes out.";
    status_->set("Waiting", Tone::Info, {{"for your answer: " + what, msg, Tone::Dim, false}});
    wf_->clear();
    body_->set_stale(false);
    body_->show_message(msg);
    waiting_msg_ = true;
  }

 private:
  // ------------------------------------------------------------------ tests

  RequestTab* tab() const {
    for (wxWindow* w = GetParent(); w; w = w->GetParent())
      if (auto* t = dynamic_cast<RequestTab*>(w)) return t;
    return nullptr;
  }

  // Requests whose body view is Visualize (for this process, every tab): by request id, or by
  // method + URL as edited for a request without one.
  static std::set<std::string>& vis_chosen_by_request() {
    static std::set<std::string> s;
    return s;
  }
  // A restored run that was shown as Visualize: this request opens in it again.
  void remember_visualize() {
    const std::string k = vis_key();
    if (!k.empty() && vis_chosen_by_request().size() < 4096) vis_chosen_by_request().insert(k);
  }
  std::string vis_key() const {
    const RequestTab* t = tab();
    if (!t) return {};
    // A saved request by its file: no store of the editor's state on every run shown.
    if (!t->file().empty()) {
      const std::u8string f = t->file().generic_u8string();
      return "file " + std::string(f.begin(), f.end());
    }
    const SavedRequest& q = t->request();
    if (!q.id.empty()) return "id " + q.id;
    return "http " + q.http.method + " " + q.http.url;
  }

  // Which saved request a run belongs to, for the delta's base: two requests with the same URL
  // (02 Items and 19 Json) never compare with each other's runs. Empty for an unsaved tab, whose
  // runs compare by method + URL + body alone.
  std::string owner_tag() const {
    const RequestTab* t = tab();
    if (!t || t->file().empty()) return {};
    return "file " + wxString(t->file().wstring()).utf8_string() + "\n";
  }

  // Checks of the request q (as the tab holds it). Editable here unless the request editor owns
  // them (see save_asserts).
  void set_asserts_from(const SavedRequest& q) {
    tests_->set_asserts(q.asserts, !asserts_owned_elsewhere_, q.kind == RequestKind::Grpc);
    tests_->set_inherited(inherited_checks(q));
    update_tests_label();
  }
  // The checks q gets from its folders and the collection (folder.json asserts), with their origin:
  // they run with its own on every response (as the runner and watt --run do). One cached stat
  // per folder level (Workspace::has_inherited_settings), so this is cheap on the UI thread.
  std::vector<InheritedCheck> inherited_checks(const SavedRequest& q) {
    std::vector<InheritedCheck> out;
    const RequestTab* t = tab();
    const Workspace* ws = t ? t->workspace() : nullptr;
    if (!ws || q.settings_merged) return out;
    const SavedRequest probe = [&] {  // only what decides the inheritance: the file (its folders)
      SavedRequest p;
      p.kind = q.kind;
      p.file = t->inherit_file();
      return p;
    }();
    if (!ws->has_inherited_settings(probe)) return out;
    for (const auto& c : ws->effective_settings(probe).asserts)
      if (c.from.level != SettingsOrigin::Level::Request) out.push_back({c.assertion, c.from.label, c.from.folder});
    return out;
  }
  void refresh_asserts(bool rerun_if_changed) {
    RequestTab* t = tab();
    if (!t) return;
    const std::vector<Assertion> before = tests_->asserts();
    const std::vector<InheritedCheck> inh_before = tests_->inherited();
    set_asserts_from(t->request());
    if (rerun_if_changed && (before != tests_->asserts() || inh_before != tests_->inherited())) run_tests();
  }

  // The user edited the checks in the Tests tab: they go into the tab's request (saved with it on
  // Ctrl+S) and run again on the response shown.
  void save_asserts(const std::vector<Assertion>& a) {
    if (RequestTab* t = tab()) {
      // request() is the tab's own (mutable) copy, refreshed from the editor first.
      auto& q = const_cast<SavedRequest&>(t->request());
      q.asserts = a;
      t->changed();
      // An editor that stores the checks itself (an Asserts table) would overwrite them on its
      // next store(): then editing belongs there and this tab only shows them.
      if (t->request().asserts != a) {
        asserts_owned_elsewhere_ = true;
        set_asserts_from(t->request());
      }
    }
    run_tests();
  }

  void update_tests_label() {
    const auto& a = tests_->asserts();
    int on = 0;
    for (const Assertion& x : a) on += x.enabled && !x.target.empty();
    // Script test() results count with the assertions (Postman / Insomnia / Bruno show one total).
    for (const InheritedCheck& x : tests_->inherited()) on += x.a.enabled && !x.a.target.empty();
    // Script test() results count with the assertions (Postman / Insomnia / Bruno show one total),
    // and so do the folders' / collection's checks.
    const int sp = tests_->script_passed() + tests_->inherited_passed(),
              sf = tests_->script_failed() + tests_->inherited_failed();
    // The send failed before any response: the checks did not run, which counts as a failure.
    if (!tests_->no_response_reason().empty() && on > 0) {
      // A send that failed or was cancelled fails its checks (as watt --run and the runner
      // count it); a restored run without its body only skips them.
      const std::string& why = tests_->no_response_reason();
      const bool skipped = why.rfind("the ", 0) == 0;
      tabs_->set_label(kTests, "Tests", skipped ? "skipped" : "not run");
      tabs_->set_count_tone(kTests, skipped ? Tone::Warn : Tone::Err);
      return;
    }
    if (on == 0 && sp + sf == 0) {
      tabs_->set_label(kTests, "Tests", tests_->script_skipped() ? std::string("not run")
                                        : a.empty()               ? std::string()
                                                                  : std::string("off"));
      tabs_->set_count_tone(kTests, tests_->script_skipped() ? Tone::Warn : Tone::Plain);
      return;
    }
    const int pass = tests_->passed() + sp, fail = tests_->failed() + sf;
    if (on > 0 && tests_->passed() + tests_->failed() == 0 && sp + sf == 0) {
      tabs_->set_label(kTests, "Tests", std::to_string(on));
      tabs_->set_count_tone(kTests, Tone::Plain);
      return;
    }
    // A restored run's checks were evaluated now, not when it was sent: the count says so (the
    // Tests page spells it out).
    const bool re = tests_->rechecked() && tests_->passed() + tests_->failed() > 0;
    const std::string count = (re ? std::string("now ") : std::string()) + std::to_string(pass) + "/" +
                              std::to_string(pass + fail);
    tabs_->set_label(kTests, "Tests", count);
    tabs_->set_count_tone(kTests, fail ? Tone::Err : Tone::Ok);
    // The status strip says the same total as the Tab (assertions, folder checks and script tests
    // together), once the assertions have been evaluated.
    if (tests_->passed() + tests_->failed() + tests_->inherited_passed() + tests_->inherited_failed() > 0) {
      std::string tip = std::to_string(pass) + " of " + std::to_string(pass + fail) + " checks passed: assertions " +
                        std::to_string(tests_->passed()) + "/" + std::to_string(tests_->passed() + tests_->failed());
      if (tests_->inherited_passed() + tests_->inherited_failed() > 0)
        tip += ", folder checks " + std::to_string(tests_->inherited_passed()) + "/" +
               std::to_string(tests_->inherited_passed() + tests_->inherited_failed());
      if (tests_->script_passed() + tests_->script_failed() > 0)
        tip += ", script tests " + std::to_string(tests_->script_passed()) + "/" +
               std::to_string(tests_->script_passed() + tests_->script_failed()) +
               " (a script that threw counts as one failed check)";
      StatusSeg seg{"tests " + count, tip + ".\nClick for the Tests tab.", fail ? Tone::Err : Tone::Ok, false, 2};
      seg.click = kClickTests;
      seg.key = kSegChecks;
      status_->put(kSegChecks, std::move(seg));
    }
  }

  // Evaluates the checks against the response shown, on a worker thread (the body is parsed
  // there, once). {{vars}} in expected values are expanded here first, on the UI thread, so the
  // worker never touches the workspace.
  void run_tests() {
    const uint64_t seq = ++test_seq_;
    const bool fresh = fresh_response_;
    fresh_response_ = false;
    {
      bool re = false;
      if (showing_grpc_) re = !grpc_history_when_.empty();
      else if (shown_run_ < runs_.size()) re = runs_[shown_run_].restored || !runs_[shown_run_].example.empty();
      tests_->set_rechecked(re);
    }
    const std::vector<Assertion>& list = tests_->asserts();
    // The request's captures: resolved on the worker with the same parsed body, stored on the UI
    // thread (only for a response that just arrived).
    auto caps = std::make_shared<std::vector<Capture>>();
    if (RequestTab* t = tab()) *caps = t->request().captures;
    // Hints for the quick-add chips.
    if (showing_grpc_ && grpc_last_) {
      // A Manual stream's responseTime is its server part (core/asserts.h), not open -> trailers.
      const GrpcResponse::Pacing& pc = grpc_last_->pacing;
      tests_->set_hints(grpc_last_->status_code == 0 ? 200 : 0,
                        pc.on && pc.tail_ms >= 0 ? pc.tail_ms : grpc_last_->timing.total_ms, {},
                        !grpc_last_->messages.empty());
    } else if (!runs_.empty() && shown_run_ < runs_.size()) {
      const HttpResponse& r = *runs_[shown_run_].resp;
      const std::string ct(r.headers.get("content-type"));
      tests_->set_hints(r.ok ? r.status : 0, r.timing.total_ms, ct,
                        ct.find("json") != std::string::npos);
    }
    // A failed send has no response to check (a re-run after the checks were edited, too).
    if (!showing_grpc_ && shown_run_ < runs_.size() && !runs_[shown_run_].resp->ok) {
      tests_->set_no_response("request failed, no response");
      update_tests_label();
      return;
    }
    // The same for a gRPC call that never reached a server (connection refused, DNS, TLS): status
    // UNAVAILABLE with no metadata and no message from anyone. Its checks do not run either,
    // as for a failed HTTP request.
    if (showing_grpc_ && grpc_last_ && grpc_last_->status_code == 14 && grpc_last_->headers.empty() &&
        grpc_last_->trailers.empty() && grpc_last_->messages.empty()) {
      tests_->set_no_response("call failed, no response");
      update_tests_label();
      return;
    }
    const std::vector<InheritedCheck>& inh = tests_->inherited();
    bool any = false;
    for (const Assertion& x : list) any = any || (x.enabled && !x.target.empty());
    for (const InheritedCheck& x : inh) any = any || (x.a.enabled && !x.a.target.empty());
    for (const Capture& c : *caps) any = any || (c.enabled && !c.var.empty());
    if (caps->empty()) tests_->set_captures({});
    if (!any) {
      std::vector<TestRow> rows(list.size());
      for (size_t i = 0; i < list.size(); ++i) rows[i].state = list[i].enabled ? TestRow::NotRun : TestRow::Off;
      tests_->set_results(std::move(rows), 0, 0);
      tests_->set_inherited_results({});
      update_tests_label();
      if (!list.empty() || tests_->script_passed() + tests_->script_failed() > 0 || tests_->script_skipped())
        prebuild_tests();
      return;
    }
    auto req = std::make_shared<SavedRequest>();
    req->kind = showing_grpc_ ? RequestKind::Grpc : RequestKind::Http;
    // The request's own checks first, then the inherited ones (rows own_n.. of the results).
    const size_t own_n = list.size();
    req->asserts = list;
    for (const InheritedCheck& x : inh) req->asserts.push_back(x.a);
    // Per check: the {{variables}} its expected value still holds after expansion (not set by
    // the environment, globals, folders or this session's captures), for the failure message:
    // a runtime variable a capture set is gone after a restart, and "expected {{token}}" alone
    // did not say why a check that passed before now fails.
    auto unset = std::make_shared<std::vector<std::string>>(req->asserts.size());
    if (RequestTab* t = tab())
      if (const Workspace* ws = t->workspace())
        for (size_t i = 0; i < req->asserts.size(); ++i) {
          Assertion& x = req->asserts[i];
          if (x.value.find("{{") == std::string::npos) continue;
          x.value = ws->expand(x.value);
          for (size_t a = x.value.find("{{"); a != std::string::npos; a = x.value.find("{{", a + 2)) {
            const size_t b = x.value.find("}}", a + 2);
            if (b == std::string::npos) break;
            const std::string name = x.value.substr(a, b + 2 - a);
            if ((*unset)[i].find(name) == std::string::npos) (*unset)[i] += ((*unset)[i].empty() ? "" : ", ") + name;
          }
        }
    std::shared_ptr<const HttpResponse> http;
    std::shared_ptr<const GrpcResponse> grpc;
    if (showing_grpc_ && grpc_last_) grpc = grpc_last_;
    else if (!runs_.empty() && shown_run_ < runs_.size()) http = runs_[shown_run_].resp;
    if (!http && !grpc) {  // nothing on screen to check: never keep an earlier response's results
      tests_->set_no_response(tests_->no_response_reason());
      update_tests_label();
      return;
    }
    tests_->set_running();
    tabs_->set_label(kTests, "Tests", "\xE2\x80\xA6");
    tabs_->set_count_tone(kTests, Tone::Plain);
    std::thread([req, http, grpc, seq, caps, fresh, own_n, unset, alive = alive_, this] {
      const auto t0 = Clock::now();
      std::vector<AssertResult> res;
      double parse_us = 0;
      // Per capture: resolved value, or why not.
      std::vector<std::pair<bool, std::string>> cvals(caps->size());
      {
        std::unique_ptr<ResponseData> d = http ? std::make_unique<ResponseData>(*http) : std::make_unique<ResponseData>(*grpc);
        res = evaluate_asserts(*req, *d, nullptr);
        for (size_t i = 0; i < caps->size(); ++i) {
          const Capture& c = (*caps)[i];
          if (!c.enabled || c.var.empty()) continue;
          std::string v, err;
          try {
            cvals[i].first = resolve_target(c.target, *d, v, &err);
          } catch (...) {
            err = "internal error";
          }
          cvals[i].second = cvals[i].first ? std::move(v) : (err.empty() ? std::string("undefined") : err);
          if (!cvals[i].first) {
            std::vector<PathStep> path;
            long msg = -1;
            if (target_json_path(c.target, path, &msg) && !path.empty()) {
              std::string why;
              if (http) why = json_path_miss(http->body, path);
              else if (grpc && !grpc->messages.empty() && static_cast<size_t>(msg < 0 ? 0 : msg) < grpc->messages.size())
                why = json_path_miss(grpc->messages[static_cast<size_t>(msg < 0 ? 0 : msg)].json, path);
              if (!why.empty()) cvals[i].second = why;
            }
          }
        }
        parse_us = d->parse_us();
      }
      const double ms = ms_between(t0, Clock::now());
      // Results are for the enabled checks with a target, in order: map them back to rows.
      std::vector<TestRow> rows(req->asserts.size());
      for (size_t i = 0; i < rows.size(); ++i)
        rows[i].state = req->asserts[i].enabled ? TestRow::NotRun : TestRow::Off;
      for (const AssertResult& r : res) {
        if (!r.a) continue;
        const size_t i = static_cast<size_t>(r.a - req->asserts.data());
        if (i >= rows.size()) continue;
        TestRow& row = rows[i];
        row.state = r.pass ? TestRow::Pass : TestRow::Fail;
        row.actual = r.actual;
        row.expected = r.expected;
        row.message = r.message;
        row.eval_us = r.eval_us;
        if (!r.pass && i < unset->size() && !(*unset)[i].empty())
          row.message = (*unset)[i] + " is not set (no environment, global or runtime variable has it; values captured " +
                        "at run time are kept in memory only and are gone after a restart: send the request that " +
                        "captures it again)" + (row.message.empty() ? std::string() : " \xC2\xB7 " + row.message);
        // A path into the body that is not there: say what is there instead, first (the column
        // and the reveal box show the start of the note).
        if (!r.pass && (r.actual.empty() || r.actual == "undefined")) {
          std::vector<PathStep> path;
          long msg = -1;
          if (target_json_path(r.a->target, path, &msg) && !path.empty()) {
            std::string why;
            if (http) {
              why = json_path_miss(http->body, path);
            } else if (grpc && !grpc->messages.empty()) {
              const size_t mi = msg < 0 ? 0 : static_cast<size_t>(msg);
              if (mi < grpc->messages.size())
                why = json_path_miss(grpc->messages[mi].json, path,
                                     msg < 0 ? std::string("res.body") : "res.messages[" + std::to_string(mi) + "]");
            }
            if (!why.empty()) row.message = why + (row.message.empty() ? std::string() : " \xC2\xB7 " + row.message);
          }
        }
      }
      if (!wxTheApp) return;
      std::vector<TestRow> inh_rows;
      if (rows.size() > own_n) {
        inh_rows.assign(std::make_move_iterator(rows.begin() + static_cast<std::ptrdiff_t>(own_n)),
                        std::make_move_iterator(rows.end()));
        rows.resize(own_n);
      }
      wxTheApp->CallAfter([alive, this, seq, rows = std::move(rows), inh_rows = std::move(inh_rows), ms, parse_us, caps,
                           cvals = std::move(cvals), fresh]() mutable {
        if (!*alive || seq != test_seq_) return;
        tests_->set_inherited_results(std::move(inh_rows));
        tests_->set_results(std::move(rows), ms, parse_us / 1000.0);
        if (!caps->empty()) {
          std::vector<CaptureRow> crows = store_captures(*caps, cvals, fresh);
          log_->set_tail(capture_log(crows, fresh));
          tests_->set_captures(std::move(crows));
        } else {
          log_->set_tail({});
        }
        update_tests_label();
        prebuild_tests();
      });
    }).detach();
  }

  // The Tests page (three lists, two chip rows, the column fitting) cost 60-85 ms to build and
  // lay out on its first show. Once a response with checks is on screen it is built ahead, one
  // slice (a few ms) per timer tick: a timer message only comes when no input is waiting, so the
  // slices never delay a click or a key. Its first show then only swaps the page in.
  void prebuild_tests() {
    if (tests_->complete()) return;
    if (tests_timer_.GetOwner() != this) {
      tests_timer_.SetOwner(this, wxWindow::NewControlId());
      Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        // Only for the view on screen (restored background tabs build theirs when shown).
        if (tests_->complete() || !IsShownOnScreen()) return;
        TraceScope ts("tests.prebuild.slice");
        if (!tests_->build_step()) tests_timer_.StartOnce(tests_fast_ ? 1 : 15);
      }, tests_timer_.GetId());
    }
    if (!tests_timer_.IsRunning()) tests_timer_.StartOnce(30);
  }

  // The captures of the run for the Log, next to the script's variable writes ("set runtime cnt").
  static std::string capture_log(const std::vector<CaptureRow>& rows, bool fresh) {
    std::string o;
    for (const CaptureRow& r : rows) {
      if (!r.enabled) continue;
      if (o.empty()) o = "*\n* captures" + std::string(fresh ? "" : " (shown again: nothing written)") + "\n";
      if (r.state == CaptureRow::Failed) {
        o += "! capture {{" + r.var + "}} from " + r.target + ": " + one_line(r.message) + "\n";
        continue;
      }
      std::string v = r.value.size() > 160 ? r.value.substr(0, 160) + "\xE2\x80\xA6" : r.value;
      for (char& c : v)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
      o += std::string("* ") + (r.state == CaptureRow::Stored && fresh ? "set " : "would set ") + r.scope + " " +
           r.var + " = " + v + "  (capture from " + r.target + ")\n";
    }
    return o;
  }

  // Stores the resolved capture values into the workspace (UI thread: memory only; a changed
  // environment / globals value is saved by a worker from a copy), with the same rules as
  // core apply_captures: valid name, no "{{" in the value, never a secret. fresh = false (an
  // earlier run shown again): nothing is stored, the values are only shown.
  std::vector<CaptureRow> store_captures(const std::vector<Capture>& caps,
                                         const std::vector<std::pair<bool, std::string>>& vals, bool fresh) {
    std::vector<CaptureRow> out;
    Workspace* ws = nullptr;
    if (RequestTab* t = tab()) ws = const_cast<Workspace*>(t->workspace());  // the frame's own workspace
    Environment* env = nullptr;
    if (ws)
      for (Environment& e : ws->environments())
        if (!ws->active_env.empty() && e.name == ws->active_env) env = &e;
    bool persist = false;
    for (size_t i = 0; i < caps.size(); ++i) {
      const Capture& c = caps[i];
      CaptureRow r;
      r.var = c.var;
      r.target = c.target;
      r.scope = capture_scope_name(c.scope);
      r.enabled = c.enabled && !c.var.empty();
      if (!r.enabled || i >= vals.size()) {
        out.push_back(std::move(r));
        continue;
      }
      auto fail = [&](std::string m) {
        r.state = CaptureRow::Failed;
        r.message = std::move(m);
      };
      const std::string name_problem = capture_var_problem(c.var, kMaxVarName);
      const bool secret = env && (env->secrets.count(c.var) || env->unreadable_secrets.count(c.var));
      if (!vals[i].first) fail(vals[i].second);
      else if (!name_problem.empty()) fail(name_problem);
      else if (vals[i].second.find("{{") != std::string::npos)
        fail("the value contains '{{', which would be expanded as a variable when sent; not captured");
      else if (secret && c.scope != Capture::Globals)
        fail("'" + c.var + "' is a secret variable of environment '" + env->name + "': captures never write secrets");
      else if (!fresh) {
        // Checks re-run on the same response (an edit), or an earlier run shown again: nothing is
        // written. A variable that already holds this value was set by this response.
        const std::map<std::string, std::string>* where =
            !ws ? nullptr
                : c.scope == Capture::Environment ? (env ? &env->vars : nullptr)
                : c.scope == Capture::Globals     ? &ws->globals
                                                  : &ws->runtime_vars;
        const auto it = where ? where->find(c.var) : std::map<std::string, std::string>::const_iterator{};
        r.state = where && it != where->end() && it->second == vals[i].second ? CaptureRow::Stored
                                                                              : CaptureRow::Previewed;
        r.value = vals[i].second;
      } else if (!ws) fail("the workspace is not open yet");
      else if (c.scope == Capture::Environment && !env) fail("no active environment to capture into");
      else {
        std::string& slot = c.scope == Capture::Environment ? env->vars[c.var]
                            : c.scope == Capture::Globals   ? ws->globals[c.var]
                                                            : ws->runtime_vars[c.var];
        if (c.scope != Capture::Runtime && slot != vals[i].second) persist = true;
        slot = vals[i].second;
        r.state = CaptureRow::Stored;
        r.value = vals[i].second;
      }
      out.push_back(std::move(r));
    }
    if (persist && ws) {
      // Saved off the UI thread from a snapshot (the UI keeps changing the live workspace).
      auto snap = std::make_shared<Workspace>(*ws);
      std::thread([snap] {
        try {
          snap->save_settings();
        } catch (...) {
        }
      }).detach();
    }
    return out;
  }

  // A failing check was clicked: show what it looked at.
  void reveal(const Assertion& a) {
    std::vector<PathStep> path;
    long msg = -1;
    if (target_json_path(a.target, path, &msg)) {
      select_tab(kBody);
      body_->reveal_json(std::move(path), msg, a.target);
      return;
    }
    const std::string t = a.target;
    if (t.rfind("res.headers", 0) == 0 || t.rfind("res.metadata", 0) == 0) select_tab(kHeaders);
    else if (t.rfind("res.trailers", 0) == 0 && tabs_->tab_shown(kTrailers)) select_tab(kTrailers);
  }

  // ------------------------------------------------------------------ restore

  // The key response_keep stores a run's extras by: the request as saved in this tab, the status
  // and the shell's exact total (what the history entry and a kept last response carry).
  std::string extras_key(const HttpResponse& r) const {
    const RequestTab* t = tab();
    if (!t) return {};
    const SavedRequest& q = t->request();
    if (q.kind != RequestKind::Http || q.http.url.empty()) return {};
    return rv::run_key(q.http.method + " " + q.http.url, r.status, r.timing.total_ms);
  }

  // A kept response with what this view kept of its run.
  std::pair<std::shared_ptr<const HttpResponse>, std::shared_ptr<const SentHttp>> with_extras(
      const HttpResponse& kept, const std::shared_ptr<const SentHttp>& sent, const rv::RunExtras& x) {
    std::shared_ptr<const HttpResponse> r = rv::restore_response(&kept, x);
    extras_prompt_ms_ = x.prompt_ms <= kept.timing.queue_ms ? x.prompt_ms : 0;
    extras_cut_size_ = x.head && kept.body.size() < x.body_size ? x.body_size : 0;
    extras_body_key_ = x.key;
    extras_sse_at_ = x.sse_at.empty() ? nullptr : std::make_shared<const std::vector<double>>(x.sse_at);
    auto s = sent ? std::make_shared<SentHttp>(*sent) : std::make_shared<SentHttp>();
    if (!sent) {
      s->request.method = "GET";
      s->request.url = kept.effective_url;
    }
    if (!s->pre_script) s->pre_script = x.pre;
    if (!s->post_script) s->post_script = x.post;
    s->scripts_skipped_untrusted = s->scripts_skipped_untrusted || x.scripts_skipped_untrusted;
    if (x.visualize) remember_visualize();
    return {std::move(r), std::move(s)};
  }
  std::pair<std::shared_ptr<const HttpResponse>, std::shared_ptr<const SentHttp>> with_extras_head(
      const rv::RunExtras& x) {
    std::shared_ptr<const HttpResponse> r = rv::restore_response(nullptr, x);
    extras_prompt_ms_ = x.head && x.prompt_ms <= x.head->timing.queue_ms ? x.prompt_ms : 0;
    extras_sse_at_.reset();
    auto s = std::make_shared<SentHttp>();
    if (const RequestTab* t = tab()) s->request = t->request().http;
    s->pre_script = x.pre;
    s->post_script = x.post;
    s->scripts_skipped_untrusted = x.scripts_skipped_untrusted;
    if (x.visualize) remember_visualize();
    return {std::move(r), std::move(s)};
  }

  // The newest history entry of the request has no response kept and nothing else of it is
  // known (an older version kept none, or it was too large): its status and time, not a blank.
  void show_not_kept(const HistoryEntry& e) {
    ++view_seq_;
    std::vector<StatusSeg> segs;
    segs.push_back({"last run " + fmt::ms(e.total_ms) + " \xC2\xB7 body not kept",
                    "The last run of this request, from the workspace history: its response was not kept (too "
                    "large, or kept by an older version).\nThe time is as the history recorded it.\nCtrl+Enter "
                    "sends the request again.",
                    Tone::Plain, true});
    segs.push_back(history_seg(e.when_iso));
    if (e.status > 0)
      status_->set(std::to_string(e.status), http_tone(e.status), std::move(segs));
    else
      status_->set("Failed", Tone::Err, std::move(segs));
    wf_->clear(false);
    body_->show_message("The last response of this request was not kept with the history (" + fmt::ms(e.total_ms) +
                        ", status " + (e.status > 0 ? std::to_string(e.status) : std::string("none")) +
                        ").\n\nCtrl+Enter sends the request again.");
    // Nothing to check: the checks say so instead of a bare count that reads like a result.
    ++test_seq_;
    tests_->set_no_response("the last response was not kept: send again to check");
    update_tests_label();
  }

  // ------------------------------------------------------------------ log

  // The tab's last response from an earlier session: said so (it is not fresh, and it carries
  // less than a live one: no TLS details, no certificate, no request as sent).
  // The request's last response, read from the workspace history when the tab opened.
  static StatusSeg history_seg(const std::string& when_iso) {
    // Stored in UTC: shown in local time.
    std::string when = when_iso;
    wxDateTime dt;
    if (when.size() >= 19 && dt.ParseISOCombined(wxString(when.substr(0, 19)))) {
      const wxDateTime local = dt.FromTimezone(wxDateTime::UTC);
      // Today: the time alone (the strip keeps room for the timing split).
      when = local.Format(local.IsSameDate(wxDateTime::Now()) ? "%H:%M" : "%Y-%m-%d %H:%M").utf8_string();
    }
    return {"last run \xC2\xB7 " + when,
            "The last response of this request, from the workspace history (" + when_iso +
                "): it was not sent now.\nIts timing is as measured then (time in confirmation dialogs left out); "
                "the connection details are those of that run.\nCtrl+Enter sends the request again.",
            Tone::Warn, false, 1};
  }

  // Labelled like a run read from the history ("last run · 21:18"): the same state reads the same
  // on every tab. The time is the response's Date header when it has one.
  static StatusSeg restored_seg(const HttpResponse* r = nullptr) {
    std::string when = "earlier session";
    if (r)
      if (const std::string_view d = r->headers.get("date"); !d.empty()) {
        wxDateTime dt;
        wxString::const_iterator end;
        const wxString ds = wxString::FromUTF8(d.data(), d.size());
        if (dt.ParseRfc822Date(ds, &end)) {
          const wxDateTime local = dt;  // parsed as an absolute time, shown in local time
          when = local.Format(local.IsSameDate(wxDateTime::Now()) ? "%H:%M" : "%Y-%m-%d %H:%M").utf8_string();
        }
      }
    return {"last run \xC2\xB7 " + when,
            "This response was received in an earlier session and restored with the tab: it is not fresh.\n"
            "Its timing is as measured then (time in confirmation dialogs left out); the connection details are "
            "those of that run.\nCtrl+Enter sends the request again.",
            Tone::Warn, false, 1};
  }

  void set_log(const Run& run) {
    std::string text = http_transcript(*run.resp, run.sent.get(), request_->rows(),
                                       run.at.Format("%Y-%m-%d %H:%M:%S").utf8_string());
    if (run.cancelled_ms >= 0)
      text += (run.cut_label.empty() ? "* Cancelled by you after " + fmt::ms(run.cancelled_ms)
                                     : "! " + run.cut_label + ": " + run.cut_error) +
              " \xC2\xB7 " + fmt::human_bytes(run.cancelled_bytes) + " of the body had arrived\n";
    if (run.prompt_ms > 0)
      text += "* " + fmt::ms(run.prompt_ms) + " in a confirmation dialog before the send: not counted in the timing\n";
    if (const SentHttp* s = run.sent.get()) {
      if (s->scripts_skipped_untrusted)
        text += "! Scripts not run: this workspace's scripts are not trusted (you answered No). Send again to be asked "
                "again.\n";
      if (const SentHttp::TokenStep* tk = s->token.get()) {
        text += "* OAuth 2.0 " + tk->grant + (tk->from.empty() ? std::string() : " (auth from " + tk->from + ")") + ": ";
        if (tk->from_cache) text += "cached token used, no token request (" + fmt::ms(tk->ms) + ")\n";
        else
          text += std::string(tk->refreshed ? "token refreshed" : "token requested") + " from POST " + tk->token_url +
                  " in " + fmt::ms(tk->ms) + (tk->ok ? std::string(", OK") : ": " + tk->error) +
                  "; the access token was added to the request and is kept in memory only\n";
      }
      append_script_log(text, "pre-request", s->pre_script.get());
      append_script_log(text, "post-response", s->post_script.get());
    }
    fill_log(std::move(text));
  }

  // One script's console output, variable writes and error, for the Log ("|" console line, "!"
  // warning / error, "*" info).
  static void append_script_log(std::string& o, const char* phase, const ScriptRun* r) {
    if (!r || !r->ran) return;
    o += "*\n* " + std::string(phase) + " script \xC2\xB7 " + fmt::ms(r->elapsed_us / 1000.0);
    if (r->setup_us > 0) o += " (sandbox setup " + fmt::ms(r->setup_us / 1000.0) + ")";
    if (r->requests_sent > 0) o += " \xC2\xB7 " + std::to_string(r->requests_sent) + " sendRequest";
    if (!r->tests.empty() || !r->ok) {
      // Counted as the Tests tab and the status strip count: a script that threw is one failed
      // check of its own.
      int pass = 0;
      for (const ScriptTest& t : r->tests) pass += t.pass;
      const size_t n = r->tests.size() + (r->ok ? 0 : 1);
      o += " \xC2\xB7 script tests " + std::to_string(pass) + "/" + std::to_string(n) +
           (r->ok ? std::string() : std::string(" (the error counts as a failed check)"));
    }
    o += '\n';
    auto put = [&](char prefix, const std::string& label, const std::string& text) {
      // One Log line per text line, each with the prefix (so the colours stay right).
      size_t at = 0;
      bool first = true;
      do {
        size_t e = text.find('\n', at);
        if (e == std::string::npos) e = text.size();
        o += prefix;
        o += ' ';
        o += first ? label : std::string(label.size(), ' ');
        o.append(text, at, e - at);
        o += '\n';
        first = false;
        at = e + 1;
      } while (at < text.size());
    };
    static const char* kLevel[] = {"log   ", "info  ", "warn  ", "error "};
    for (const ScriptLog& l : r->logs) {
      const int lv = static_cast<int>(l.level);
      put(lv >= 2 ? '!' : '|', kLevel[std::clamp(lv, 0, 3)], l.text.empty() ? std::string(" ") : l.text);
    }
    if (r->logs_truncated) o += "! (console output cut at the log limit)\n";
    for (const ScriptTest& t : r->tests)
      o += (t.pass ? "* test \xE2\x9C\x93 " : "! test \xE2\x9C\x97 ") + t.name +
           (t.pass || t.message.empty() ? std::string() : ": " + t.message) + "\n";
    static const char* kScope[] = {"runtime", "environment", "globals"};
    for (const ScriptVar& v : r->vars_set) {
      std::string val = v.removed ? std::string("(removed)") : v.value;
      if (val.size() > 200) val = val.substr(0, 200) + "\xE2\x80\xA6";
      for (char& c : val)
        if (c == '\n' || c == '\r') c = ' ';
      const std::string what = std::string(kScope[std::clamp(static_cast<int>(v.scope), 0, 2)]) + " " + v.name +
                               (v.removed ? " removed" : " = " + val);
      if (v.ok) o += "* set " + what + "\n";
      else o += "! not set: " + what + (v.message.empty() ? std::string() : " (" + v.message + ")") + "\n";
    }
    if (!r->ok) o += "! error: " + script_error(*r) + "\n";
  }

  // "line 3:14: ReferenceError: x is not defined" (the position first, when known).
  static std::string script_error(const ScriptRun& r) {
    std::string e = r.error.empty() ? std::string("script failed") : r.error;
    if (r.line > 0) {
      const std::string pos = std::to_string(r.line) + ":" + std::to_string(r.column);
      if (e.rfind(pos, 0) == 0) e = "line " + e;
      else e = "line " + pos + ": " + e;
    }
    return e;
  }

  // The Tests tab's Script group for the run on screen.
  void set_script_results(const SentHttp* s) {
    ScriptResults out;
    if (s) {
      out.skipped_untrusted = s->scripts_skipped_untrusted;
      std::string note;
      for (const auto& [phase, run] : {std::pair<const char*, const ScriptRun*>{"pre-request", s->pre_script.get()},
                                       std::pair<const char*, const ScriptRun*>{"post-response", s->post_script.get()}}) {
        if (!run || !run->ran) continue;
        out.any_script = true;
        for (const ScriptTest& t : run->tests)
          out.rows.push_back({phase, t.name.empty() ? std::string("(unnamed test)") : t.name,
                              t.pass ? ScriptTestRow::Pass : ScriptTestRow::Fail, t.message});
        if (!run->ok) out.rows.push_back({phase, std::string(phase) + " script", ScriptTestRow::Error, script_error(*run)});
        if (!note.empty()) note += ", ";
        note += std::string(phase) + " " + fmt::ms(run->elapsed_us / 1000.0);
      }
      out.note = note;
    }
    tests_->set_script(std::move(out));
    update_tests_label();
  }

  // Status strip: a dialog's time left out, scripts that did not run, script failures.
  void add_script_segs(std::vector<StatusSeg>& segs, const Run& run) {
    if (run.prompt_ms > 0) {
      segs.push_back({fmt::ms(run.prompt_ms) + " in dialog, not counted",
                      "A confirmation dialog (script trust, Windows sign-in or environment variables) was open for " +
                          fmt::ms(run.prompt_ms) + " after the Send click. That time is left out of Queue and the "
                          "total: it was you deciding, not the request.",
                      Tone::Dim, false, 4});
    }
    const SentHttp* s = run.sent.get();
    if (!s) return;
    if (s->scripts_skipped_untrusted) {
      StatusSeg seg{"scripts not run (workspace not trusted)",
                    "The request has scripts, but you did not trust this workspace's scripts, so it was sent without "
                    "them (no pre-request changes, no script tests). Send again to be asked again.",
                    Tone::Warn, false, 2};
      seg.click = kClickTests;
      segs.push_back(std::move(seg));
      return;
    }
    // Counted as the Tests tab counts them: every test() call, plus one failed row per script
    // that threw (its error is a failure too, not just a note).
    int pass = 0, fail = 0, errors = 0;
    std::string err;
    for (const ScriptRun* r : {s->pre_script.get(), s->post_script.get()}) {
      if (!r || !r->ran) continue;
      for (const ScriptTest& t : r->tests) (t.pass ? pass : fail)++;
      if (!r->ok) {
        ++fail;
        ++errors;
        if (err.empty()) err = (r == s->pre_script.get() ? "pre-request: " : "post-response: ") + script_error(*r);
      }
    }
    if (!err.empty()) {
      StatusSeg seg{"script error", err + "\nClick for the Log.", Tone::Err, false, 2};
      seg.click = kClickLog;
      segs.push_back(std::move(seg));
    }
    if (pass + fail > 0) {
      // Until the assertions are evaluated (run_tests); update_tests_label then puts the one total
      // the Tests tab shows in its place.
      StatusSeg seg{"script tests " + std::to_string(pass) + "/" + std::to_string(pass + fail),
                    std::to_string(pass) + " of " + std::to_string(pass + fail) + " script checks passed" +
                        (errors ? " (a script that threw counts as one failed check)." : ".") +
                        "\nClick for the Tests tab.",
                    fail ? Tone::Err : Tone::Ok, false, 2};
      seg.click = kClickTests;
      seg.key = kSegChecks;
      segs.push_back(std::move(seg));
    }
  }

  // The Log page colours each line by its first character ("*" info, ">" sent, "<" received).
  void fill_log(std::string text) { log_->set(std::move(text)); }

  void set_grpc_log(const GrpcResponse& r) {
    std::string o;
    auto line = [&](char p, const std::string& t) {
      o += p;
      o += ' ';
      o += t;
      o += '\n';
    };
    line('*', wxDateTime::Now().Format("%Y-%m-%d %H:%M:%S").utf8_string() + "  " + grpc_label_);
    for (const InfoRow& row : grpc_conn_rows(grpc_shown_meta_, r))
      if (!row.section) line('*', row.key + ": " + row.value);
    line('*', "");
    line('>', "POST " + (grpc_label_.empty() ? std::string("(method)") : grpc_label_.substr(0, grpc_label_.find(" @ "))) +
                  " HTTP/2");
    line('>', "content-type: application/grpc" + std::string(grpc_shown_meta_.web ? "-web" : ""));
    line('>', "");
    if (!r.headers.empty() || r.ok || !r.messages.empty()) {
      line('<', "HTTP/2 200");
      for (const auto& [k, v] : r.headers) line('<', k + ": " + v);
      line('<', "");
    }
    for (size_t i = 0; i < r.messages.size(); ++i) {
      const GrpcMessageEvent& m = r.messages[i];
      line('<', "message #" + std::to_string(i + 1) + "  +" + fmt::ms(m.at_ms) + "  " + fmt::human_bytes(m.wire_bytes) +
                    (m.decode_error.empty() ? std::string() : "  decode error: " + m.decode_error));
      if (i >= 199 && r.messages.size() > 200) {
        line('<', "\xE2\x80\xA6 " + std::to_string(r.messages.size() - 200) + " more (see Messages)");
        break;
      }
    }
    for (const auto& [k, v] : r.trailers) line('<', "trailer " + k + ": " + v);
    line('*', "Status " + std::to_string(r.status_code) + " " + r.status_name +
                  (r.status_message.empty() ? std::string() : ": " + r.status_message));
    const Timing& t = r.timing;
    line('*', "Timing: queue " + fmt::ms(t.queue_ms) + ", DNS " + fmt::ms(t.dns_ms) + ", connect " + fmt::ms(t.connect_ms) +
                  ", TLS " + fmt::ms(t.tls_ms) + (r.reflect_ms > 0 ? ", reflection " + fmt::ms(r.reflect_ms) : std::string()) +
                  ", send " + fmt::ms(t.send_ms) + ", wait " + fmt::ms(t.ttfb_ms) + ", download " + fmt::ms(t.download_ms) +
                  ", total " + fmt::ms(t.total_ms));
    fill_log(o);
  }

  // The connection popover for what is shown (the HTTP run or the last gRPC call).
  void open_conn(const wxRect& anchor) {
    if (showing_grpc_ && grpc_last_) {
      show_info_popup(this, anchor, "Channel \xC2\xB7 " + (grpc_shown_meta_.address.empty() ? std::string("gRPC") : grpc_shown_meta_.address),
                      grpc_conn_rows(grpc_shown_meta_, *grpc_last_));
      return;
    }
    if (awaiting_ && head_shown_) {  // the response in flight (its head is in)
      show_info_popup(this, anchor, "Connection \xC2\xB7 in flight", http_conn_rows(*head_shown_, nullptr));
      return;
    }
    if (runs_.empty() || shown_run_ >= runs_.size()) return;
    const Run& run = runs_[shown_run_];
    std::string title = "Connection";
    if (!run.label.empty()) title += " \xC2\xB7 " + request_tag(run.label);
    show_info_popup(this, anchor, title, http_conn_rows(*run.resp, run.sent.get()));
  }

  void start_stream() {
    sse_off();
    body_->set_stale(false);
    live_ = true;
    body_->begin_stream();
    msgs_->reset();
    headers_->set({});
    trailers_->set({});
    set_kind_tabs(true, false);
  }

  void set_kind_tabs(bool grpc, bool http) {
    if (!http) tabs_->show_tab(kCookies, false);
    tabs_->show_tab(kTrailers, grpc);
    tabs_->show_tab(kMessages, grpc);
    tabs_->show_tab(kRequest, http);
    if (!grpc) {
      tabs_->set_label(kTrailers, "Trailers");
      tabs_->set_label(kMessages, "Messages");
    }
    if (!grpc && !http) tabs_->set_label(kHeaders, "Headers");
    if (!tabs_->tab_shown(tabs_->selection())) select_tab(kBody);
  }

  void select_tab(int i) {
    tabs_->select(i);
    show_page(i);
  }

  // Page i becomes visible: created now if it never was.
  void show_page(int i) {
    if (i < 0 || static_cast<size_t>(i) >= book_->GetPageCount()) return;
    if (i == kBody && sse_on_ && !sse_raw_) {
      sse_->ensure();
      book_->SetSelection(static_cast<size_t>(kSsePage));
      return;
    }
    wxWindow* w = book_->GetPage(static_cast<size_t>(i));
    TraceLaps tl(i == kTests ? "tests.show" : "page.show");
    if (auto* lp = dynamic_cast<LazyPage*>(w)) {
      if (!lp->built()) {
        wxWindowUpdateLocker lock(w);
        lp->ensure();
      }
    } else if (i == kTests) {
      // Not prebuilt yet (clicked right after a restore): one slice now, the rest on the next
      // turns of the message loop, so the first click never pays the whole page (~30 ms).
      const bool was = tests_->complete();
      if (!was && !tests_->build_step()) {
        tests_fast_ = true;
        prebuild_tests();
        tests_timer_.StartOnce(1);
      }
      tl.lap(was ? "ready" : "build");
    }
    book_->SetSelection(static_cast<size_t>(i));
    tl.lap("select");
    if (i == kTests) {
      refresh_asserts(true);
      tl.lap("refresh");
      // (Tracing only: the first paint of the page, synchronously, so its cost is in the trace.)
      if (trace_file()) {
        for (wxWindow* c : w->GetChildren()) {
          if (!c->IsShown()) continue;
          c->Update();
          tl.lap(c->GetClassInfo()->GetClassName() ? wxString(c->GetClassInfo()->GetClassName()).utf8_string().c_str() : "child");
        }
        w->Update();
        tl.lap("paint");
      }
    }
  }

  void add_delta(std::vector<StatusSeg>& segs, const TimingModel& cur, const TimingModel& prev,
                 const std::string& label, const wxDateTime& at) {
    const double total = cur.total;
    const double d = total - prev.total;
    // Run-to-run jitter (tens of microseconds locally, a few percent) is not a regression.
    // Under 1 ms or under 10% of the total is not coloured as a change.
    const bool noise = fmt::delta_is_noise(d, std::max(total, prev.total));
    // One run on a fresh connection, the other on a reused one: the difference is mostly the
    // handshake, not the server. Said so, and not coloured as a regression / improvement.
    const bool conn_changed = cur.reused != prev.reused;
    std::string text = fmt::delta_ms(d) + " vs previous";
    if (conn_changed) text += cur.reused ? " (reused conn.)" : " (new conn.)";
    std::string tip = "Total compared with the previous run of the same request" +
                      (label.empty() ? std::string() : ":\n" + label) + "\n(" + fmt::ms(prev.total) + " at " +
                      at.Format("%H:%M:%S").utf8_string() +
                      "). The waterfall shows the difference per phase. Requests with another method, URL or "
                      "body are never compared.";
    if (conn_changed)
      tip += std::string("\n\nThis run ") + (cur.reused ? "reused an open connection" : "opened a new connection") +
             " and the previous one " + (prev.reused ? "reused one" : "opened a new one") +
             ", so most of the difference is connection setup. Server wait: " +
             fmt::delta_ms(cur.server_wait - prev.server_wait) + ".";
    segs.push_back({text, tip, noise || conn_changed ? Tone::Dim : (d < 0 ? Tone::Ok : Tone::Err), false, 5});
  }

  struct Base {
    TimingModel model;
    wxDateTime at;
  };
  const Base* base_for(const std::string& key) const {
    if (key.empty()) return nullptr;
    const auto it = bases_.find(key);
    return it == bases_.end() ? nullptr : &it->second;
  }
  void remember(const std::string& key, const TimingModel& m) {
    if (key.empty()) return;
    if (bases_.size() > 256) bases_.clear();
    bases_[key] = Base{m, wxDateTime::Now()};
  }

  void update_history_chip() {
    const bool none = shown_run_ >= runs_.size();  // e.g. cancelled: no kept run is on screen
    const bool any = runs_.size() > 1 || (none && !runs_.empty());
    hist_->set_visible(1, any);
    if (!any) return hist_->Refresh();
    if (none) {
      hist_->set_label(1, "Earlier runs (" + std::to_string(runs_.size()) + ") \xE2\x96\xBE");
      hist_->set_on(1, false);
      return;
    }
    const bool latest = shown_run_ + 1 == runs_.size();
    hist_->set_label(1, (latest ? std::string("Latest") : "Run " + std::to_string(shown_run_ + 1)) + " of " +
                            std::to_string(runs_.size()) + " \xE2\x96\xBE");
    hist_->set_on(1, !latest);
  }

  // ------------------------------------------------------------------ examples

  std::filesystem::path examples_folder() const {
    RequestTab* t = tab();
    const Workspace* ws = t ? t->workspace() : nullptr;
    if (!ws || t->request().file.empty()) return {};
    return examples_dir(ws->root(), t->request().id);
  }

  // Lists the request's examples on a worker (disk), then updates the chip.
  void refresh_examples() {
    const std::filesystem::path dir = examples_folder();
    hist_->set_visible(2, !dir.empty());
    hist_->Refresh();
    examples_dir_ = dir;
    if (dir.empty()) {
      examples_.clear();
      return;
    }
    const uint64_t seq = ++examples_seq_;
    std::thread([dir, seq, alive = alive_, this] {
      auto list = std::make_shared<std::vector<ExampleEntry>>(list_examples(dir));
      if (!wxTheApp) return;
      wxTheApp->CallAfter([alive, this, seq, list] {
        if (!*alive || seq != examples_seq_) return;
        examples_ = std::move(*list);
        hist_->set_label(2, examples_.empty() ? std::string("Examples \xE2\x96\xBE")
                                              : "Examples (" + std::to_string(examples_.size()) + ") \xE2\x96\xBE");
        hist_->Refresh();
      });
    }).detach();
  }

  // Where a chip menu opens: under the chips, moved left so that it ends inside the pane (a menu
  // opened at the chips' left edge ran past the window's right edge at 1600 px).
  wxPoint menu_pos(const wxMenu& m) const {
    int w = 0;
    for (const wxMenuItem* it : m.GetMenuItems())
      if (!it->IsSeparator()) w = std::max(w, GetTextExtent(it->GetItemLabelText()).x);
    // The menu font is a little larger than the pane's; then the check mark column, the
    // submenu-arrow column, padding and the border.
    w = w * 115 / 100 + FromDIP(96);
    wxPoint p = hist_->GetPosition() + wxPoint(0, hist_->GetSize().y);
    p.x = std::max(0, std::min(p.x, GetClientSize().x - w));
    return p;
  }

  void examples_menu() {
    wxMenu m;
    const Run* run = !showing_grpc_ && shown_run_ < runs_.size() ? &runs_[shown_run_] : nullptr;
    const bool can_save = run && run->resp->ok && run->example.empty() && !examples_dir_.empty();
    m.Append(1, wxString::FromUTF8("Save this response as example\xE2\x80\xA6"))->Enable(can_save);
    if (!examples_.empty()) {
      m.AppendSeparator();
      for (size_t i = 0; i < examples_.size() && i < 40; ++i) {
        const ExampleEntry& e = examples_[i];
        m.Append(static_cast<int>(i) + 100,
                 U(std::to_string(e.status) + "   " + elide_middle(e.name, 50) + "   \xC2\xB7   " + e.saved_at));
      }
    } else {
      m.Append(2, showing_grpc_ ? "(examples are for HTTP responses)" : "(no examples saved yet)")->Enable(false);
    }
    const int id = GetPopupMenuSelectionFromUser(m, menu_pos(m));
    if (id == 1 && can_save) return save_example_of(*run);
    if (id >= 100 && static_cast<size_t>(id - 100) < examples_.size()) open_example(examples_[static_cast<size_t>(id - 100)]);
  }

  void save_example_of(const Run& run) {
    const HttpResponse& r = *run.resp;
    std::string def = std::to_string(r.status) + (r.status_text.empty() ? std::string() : " " + r.status_text);
    wxTextEntryDialog dlg(this, "Name of the example (saved with this request, for you only):", "Save as example",
                          U(def));
    if (dlg.ShowModal() != wxID_OK) return;
    const std::string name = dlg.GetValue().utf8_string();
    const std::filesystem::path dir = examples_dir_;
    auto resp = run.resp;
    const std::string method = run.sent ? run.sent->request.method : std::string();
    const std::string url = run.sent ? run.sent->request.url : r.effective_url;
    status_note("saving the example\xE2\x80\xA6", Tone::Accent);
    std::thread([dir, name, resp, method, url, alive = alive_, this] {
      std::string err;
      const bool ok = save_example(dir, name, *resp, method, url, &err);
      if (!wxTheApp) return;
      wxTheApp->CallAfter([alive, this, ok, err, name] {
        if (!*alive) return;
        status_note(ok ? "saved as example \"" + name + "\"" : "could not save the example: " + err,
                    ok ? Tone::Ok : Tone::Err);
        refresh_examples();
        if (ok) mock_examples_changed();  // a running mock serves it at once
      });
    }).detach();
  }

  void open_example(const ExampleEntry& e) {
    const std::filesystem::path file = e.file;
    const std::string label = e.name + ", saved " + e.saved_at;
    std::thread([file, label, alive = alive_, this] {
      std::string method, url, err;
      std::shared_ptr<HttpResponse> r = load_example(file, &method, &url, &err);
      if (!wxTheApp) return;
      wxTheApp->CallAfter([alive, this, r, method, url, err, label] {
        if (!*alive) return;
        if (!r) return status_note(err, Tone::Err);
        show_example(r, method, url, label);
      });
    }).detach();
  }

  // An example joins the tab's runs (History) like a response, marked as an example: no captures
  // are stored from it and it is never the base of a delta.
  void show_example(std::shared_ptr<const HttpResponse> r, const std::string& method, const std::string& url,
                    const std::string& label) {
    auto sent = std::make_shared<SentHttp>();
    sent->request.method = method.empty() ? std::string("GET") : method;
    sent->request.url = url;
    Run run{r, sent, http_model(*r, url.rfind("http:", 0) == 0), wxDateTime::Now()};
    run.label = sent->request.method + " " + url;
    run.example = label;
    fresh_response_ = false;
    runs_.push_back(std::move(run));
    while (runs_.size() > kRuns) runs_.erase(runs_.begin());
    show_run(runs_.size() - 1);
    fresh_response_ = false;
  }

  // A short message in the status strip's place for notes (the body bar).
  void status_note(const std::string& text, Tone tone) { body_->note(text, tone); }

  void history_menu() {
    if (runs_.empty() || (runs_.size() < 2 && shown_run_ < runs_.size())) return;
    wxMenu m;
    const wxDateTime now = wxDateTime::Now();
    for (size_t i = runs_.size(); i-- > 0;) {
      const Run& x = runs_[i];
      const long secs = (now - x.at).GetSeconds().ToLong();
      std::string when = secs < 10 ? "just now" : secs < 60 ? std::to_string(secs) + " s ago"
                                                : secs < 3600 ? std::to_string(secs / 60) + " min ago"
                                                              : x.at.Format("%H:%M:%S").utf8_string();
      std::string label = "#" + std::to_string(i + 1) + "   " + when + "   ";
      label += x.resp->ok ? std::to_string(x.resp->status) : std::string("failed");
      label += "   " + fmt::ms(x.model.total) + "   " + fmt::human_bytes(x.resp->body.size());  // as the status line
      if (!x.example.empty()) label += "   \xC2\xB7   example " + elide_middle(x.example, 40);
      else if (!x.label.empty()) label += "   \xC2\xB7   " + elide_middle(x.label, 80);
      m.AppendCheckItem(static_cast<int>(i) + 100, U(label))->Check(i == shown_run_);
    }
    const int id = GetPopupMenuSelectionFromUser(m, menu_pos(m));
    if (id >= 100 && static_cast<size_t>(id - 100) < runs_.size() && static_cast<size_t>(id - 100) != shown_run_)
      show_run(static_cast<size_t>(id - 100));
  }

  static bool has_header(const std::vector<Header>& h, std::string_view name) {
    for (const auto& [k, v] : h)
      if (k.size() == name.size() &&
          std::equal(k.begin(), k.end(), name.begin(), [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == y; }))
        return true;
    return false;
  }

  // Set-Cookie headers of the final response, parsed as the workspace jar reads them (and what
  // the jar did with each). The tab stays for every HTTP response (empty: says none were set).
  void set_cookies(const HttpResponse& r, const Run& run) {
    std::vector<std::pair<std::string, std::string>> sc;
    for (const auto& [k, v] : r.headers)
      if (k.size() == 10 && wxString::FromUTF8(k.data(), k.size()).IsSameAs("set-cookie", false))
        sc.emplace_back(std::string(k), std::string(v));
    const int64_t at = run.at.IsValid() ? static_cast<int64_t>(run.at.GetTicks()) : static_cast<int64_t>(wxDateTime::Now().GetTicks());
    std::string url = r.effective_url;
    if (url.empty() && run.sent) url = run.sent->request.url;
    std::vector<rv::SetCookieRow> rows = rv::set_cookie_rows(sc, url, at);
    const size_t n = rows.size();
    size_t stored = 0, deleted = 0, ignored = 0;
    for (const auto& x : rows)
      (x.fate == rv::SetCookieRow::Fate::Stored ? stored : x.fate == rv::SetCookieRow::Fate::Deletes ? deleted : ignored)++;
    bool jar_off = false;
    if (const RequestTab* t = tab()) jar_off = t->request().no_cookie_jar;
    std::string note;
    Tone tone = Tone::Dim;
    if (n == 0) {
      note = r.ok ? "This response set no cookies (no Set-Cookie header)." : "No response, no cookies.";
    } else {
      // Short parts, most important first: the bar drops whole " · " parts from the end when it
      // is narrow (the full text is its tooltip).
      note = std::to_string(n) + (n == 1 ? " cookie" : " cookies") + " set";
      if (jar_off) {
        note += " \xC2\xB7 cookie jar off for this request: none stored";
        tone = Tone::Warn;
      } else {
        // Not sent now (restored, an example): what the jar's rules made of them then.
        const bool then = run.restored || !run.example.empty();
        if (stored) note += " \xC2\xB7 " + std::to_string(stored) + " went to the workspace jar";
        if (deleted) note += " \xC2\xB7 " + std::to_string(deleted) + " deleted";
        if (ignored) note += " \xC2\xB7 " + std::to_string(ignored) + " ignored (see Jar)";
        if (stored) note += " \xC2\xB7 later requests that match send them";
        if (then) note += " \xC2\xB7 as of that run";
        if (ignored) tone = Tone::Warn;
      }
    }
    if (r.redirect_count > 0)
      note += " \xC2\xB7 cookies set by the " + std::to_string(r.redirect_count) +
              (r.redirect_count == 1 ? " redirect" : " redirects") + " are not seen";
    cookies_->set(std::move(rows), std::move(note), tone);
    tabs_->show_tab(kCookies, true);
    tabs_->set_label(kCookies, "Cookies", n ? std::to_string(n) : std::string());
    if (!tabs_->tab_shown(tabs_->selection())) select_tab(kBody);
  }

  // A response received now: its Set-Cookie headers go into the workspace jar (Workspace::
  // record_cookies' rules, the URL the response came from), unless the request turned the jar
  // off. On a worker: the jar may still have to be read from disk; its writes are its own.
  void record_cookies(const HttpResponse& r, const SentHttp* sent) {
    RequestTab* t = tab();
    const Workspace* ws = t ? t->workspace() : nullptr;
    if (!ws || ws->root().empty() || t->request().no_cookie_jar) return;
    std::vector<std::pair<std::string, std::string>> sc;
    for (const auto& [k, v] : r.headers)
      if (k.size() == 10 && wxString::FromUTF8(k.data(), k.size()).IsSameAs("set-cookie", false))
        sc.emplace_back(std::string(k), std::string(v));
    if (sc.empty()) return;
    std::string url = r.effective_url;
    if (url.empty() && sent) url = sent->request.url;
    std::shared_ptr<CookieJar> jar = ws->cookie_jar();
    if (!jar || url.empty()) return;
    std::thread([jar, url = std::move(url), sc = std::move(sc)] { jar->record(url, sc); }).detach();
  }

  // "Manage cookies...": the workspace jar's editor, on this host. The jar is read from disk on
  // a worker first (the editor needs it in memory; the UI thread never waits on the disk).
  void manage_cookies(const std::string& host) {
    RequestTab* t = tab();
    const Workspace* ws = t ? t->workspace() : nullptr;
    if (!ws) return;
    std::shared_ptr<CookieJar> jar = ws->cookie_jar();
    if (!jar) return;
    std::thread([jar, host, alive = alive_, this] {
      jar->preload();
      if (!wxTheApp) return;
      wxTheApp->CallAfter([jar, host, alive, this] {
        if (!*alive) return;
        // Centred on the window, not on this pane (the dialog is wider than the pane).
        wxWindow* top = wxGetTopLevelParent(this);
        http_editor::show_cookie_manager(top ? top : this, jar, host, [] {});
      });
    }).detach();
  }

  // What went out: request line, the headers as sent (the ones libcurl adds itself dimmed), the
  // body, then every redirect hop and the final URL.
  void set_request(const SentHttp* sent, const HttpResponse* r) {
    if (!sent) {
      request_->set({});
      return;
    }
    const HttpRequest& q = sent->request;
    KV rows;
    std::vector<char> dim;
    auto row = [&](std::string k, std::string v, bool auto_added) {
      rows.emplace_back(std::move(k), std::move(v));
      dim.push_back(auto_added ? 1 : 0);
    };
    row(q.method, q.url, false);
    if (!has_header(q.headers, "host")) {
      std::string_view u = q.url;
      const size_t sch = u.find("://");
      if (sch != std::string_view::npos) u.remove_prefix(sch + 3);
      u = u.substr(0, u.find_first_of("/?#"));
      if (const size_t at = u.rfind('@'); at != std::string_view::npos) u.remove_prefix(at + 1);
      if (!u.empty()) row("Host", std::string(u), true);
    }
    for (const auto& h : q.headers) row(h.first, h.second, false);
    if (!has_header(q.headers, "user-agent")) row("User-Agent", "watt/0.1", true);
    if (!has_header(q.headers, "accept")) row("Accept", "*/*", true);
    if (!has_header(q.headers, "accept-encoding")) row("Accept-Encoding", "deflate, gzip", true);
    if (!q.body.empty()) {
      if (!has_header(q.headers, "content-length")) row("Content-Length", std::to_string(q.body.size()), true);
      if (!has_header(q.headers, "content-type") && q.method != "GET" && q.method != "HEAD")
        row("Content-Type", "application/x-www-form-urlencoded", true);
    }
    const size_t nheaders = rows.size() - 1;
    if (q.body.empty()) {
      row("(body)", "none", true);
    } else {
      // First 2 KB of the body on one line (whitespace runs collapsed).
      std::string b;
      bool ws = false;
      for (size_t i = 0; i < q.body.size() && b.size() < 2048; ++i) {
        const char c = q.body[i];
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
          ws = true;
          continue;
        }
        if (ws && !b.empty()) b += ' ';
        ws = false;
        b += c;
      }
      if (b.size() < q.body.size()) b += "\xE2\x80\xA6";
      row("(body) " + fmt::human_bytes(q.body.size()), std::move(b), false);
    }
    if (r) {
      int i = 0;
      for (const RedirectHop& h : r->hops) {
        std::string v = std::to_string(h.status) + "  " + h.url + "  \xC2\xB7  " + fmt::ms(h.total_ms) + " (wait " +
                        fmt::ms(h.ttfb_ms) + ")";
        if (h.connection_reused) v += "  \xC2\xB7  reused connection";
        const std::string remote = host_port(h.remote_ip, h.remote_port);
        if (!remote.empty()) v += "  \xC2\xB7  " + remote;
        row("Redirect " + std::to_string(++i), std::move(v), false);
      }
      if (!r->hops.empty()) row("Final URL", r->effective_url, false);
    }
    request_->set(std::move(rows), std::move(dim));
    tabs_->set_label(kRequest, "Request", std::to_string(nheaders));
  }

  struct Run {
    std::shared_ptr<const HttpResponse> resp;
    std::shared_ptr<const SentHttp> sent;
    TimingModel model;
    wxDateTime at;
    std::string key;    // method + expanded URL + body tag: what the delta compares by
    std::string label;  // "GET https://..." for the history menu and tooltips
    TimingModel base;   // previous run of the same request, when there was one
    wxDateTime base_at;
    bool has_base = false;
    bool restored = false;  // the tab's last response from an earlier session (not sent now)
    std::string from_history;  // restored from the workspace history: when it was received
    std::string example;    // a saved example ("name, saved 2026-11-17 08:30"), not a live response
    double prompt_ms = 0;   // a confirmation dialog's time, taken out of the timing
    bool body_not_kept = false;  // restored without its body (too large to keep)
    uint64_t cut_size = 0;       // restored with the start of its body only: the size received
    uint64_t not_kept_size = 0;
    std::string body_key;        // cut: the run key its whole body may be kept under (response_keep)
    bool sse_live = false;       // an event stream listed live while it arrived (this send)
    double cancelled_ms = -1;    // cut after its head arrived (ms after the send): cancelled by the user, or
    std::string cut_error;       // ... the transfer failed then (a timeout, the server went away): the error
    std::string cut_label;       // "timed out after 30000 ms" (empty: cancelled by you)
    uint64_t cancelled_bytes = 0;  // ... with this much of the body
    std::shared_ptr<const std::vector<double>> sse_at;  // event offsets (live, or kept with the run)
  };
  static constexpr double kPromptMinMs = 50;  // less is UI work, not a dialog someone answered
  static constexpr int kClickTests = 3, kClickLog = 4;  // status strip segment ids (1 conn, 2 redirects)
  static constexpr int kSegChecks = 1;  // StatusSeg::key of the tests total
  static constexpr size_t kRuns = 10;
  std::vector<Run> runs_;
  size_t shown_run_ = 0;
  ChipBar* hist_ = nullptr;
  StatusStrip* status_;
  Waterfall* wf_;
  TabStrip* tabs_;
  wxSimplebook* book_;
  BodyView* body_;
  KvPage* headers_;
  CookiePage* cookies_;
  KvPage* trailers_;
  MsgPage* msgs_;
  KvPage* request_;
  RequestKind kind_ = RequestKind::Http;
  bool live_ = false;
  std::map<std::string, Base> bases_;  // last good run per request key (HTTP and gRPC)
  std::string grpc_key_, grpc_label_;  // the gRPC call in flight
  std::vector<ExampleEntry> examples_;  // the request's saved examples (newest first)
  std::filesystem::path examples_dir_;
  uint64_t examples_seq_ = 0;
  GrpcConnMeta grpc_meta_;             // ... and its target
  GrpcConnMeta grpc_shown_meta_;       // target of grpc_last_
  std::shared_ptr<const GrpcResponse> grpc_last_;
  bool showing_grpc_ = false;
  std::string shown_ident_;            // the request the waterfall's figures belong to
  TestsView* tests_ = nullptr;
  LogPage* log_ = nullptr;
  bool fresh_response_ = false;  // the next run_tests() is for a response that just arrived
  uint64_t test_seq_ = 0;              // newest evaluation; older results are dropped
  bool asserts_owned_elsewhere_ = false;
  Clock::time_point send_t0_{};
  wxWindow* top_ = nullptr;  // frame whose key hook we added (Esc cancels)
  double first_ms_ = -1;      // first byte / message of the request in flight (ms after the send)
  bool sent_here_ = false;    // a request was sent from this view (else a result is a restored one)
  bool awaiting_ = false;     // show_sending() was called and its result has not arrived yet
  bool waiting_msg_ = false;  // the body shows "Waiting for the response" (no earlier response to fade)
  Clock::time_point progress_shown_at_{};  // when show_progress() last rewrote that message
  // A previous body stays faded on screen at most this long after a send (then show_in_flight).
  static constexpr double kStaleBodyMs = 300;
  wxTimer stale_timer_;
  uint64_t stale_seq_ = 0;
  uint64_t live_bytes_ = 0;  // body bytes of the request in flight so far
  uint64_t view_seq_ = 0;
  uint64_t help_seq_ = ~0ull;
  wxTimer hist_timer_;  // retries the history lookup until the workspace is open
  wxTimer tests_timer_;  // builds the Tests page ahead of its first show, one slice per tick
  bool tests_fast_ = false;  // the page is on screen half built: the next slices without a pause
  uint64_t hist_seq_ = 0;
  bool hist_show_bound_ = false;  // the tab's wxEVT_SHOW reads a deferred history response
  int hist_tries_ = 0;  // == view_seq_ while the empty-state help is shown     // bumps on clear / send / result: a history lookup is dropped after it
  std::string history_when_;  // show_* of a response read from history: when it was received
  // A restored show_http, deferred to a turn of its own with the state its caller set for it.
  struct RestoreArgs {
    std::shared_ptr<const HttpResponse> r;
    std::shared_ptr<const SentHttp> sent;
    std::string history_when;
    bool extras_applied = false;
    double prompt_ms = 0;
    uint64_t cut_size = 0;
    bool body_not_kept = false;
    uint64_t not_kept_size = 0;
    uint64_t seq = 0;
    std::string body_key;
    std::shared_ptr<const std::vector<double>> sse_at;
  };
  bool restore_now_ = false;  // inside the deferred restored show_http (or one it calls)
  std::string grpc_history_when_;  // ... the gRPC response shown
  bool extras_applied_ = false;    // show_http of a restored response already merged with its RunExtras
  double extras_prompt_ms_ = 0;    // ... the dialog time with_extras() took out of it
  uint64_t extras_cut_size_ = 0;   // ... its body as received, when only the start of it was kept
  std::string extras_body_key_;    // ... and the run key its whole body may be kept under
  std::shared_ptr<const std::vector<double>> extras_sse_at_;  // ... its event offsets
  bool body_not_kept_ = false;     // ... rebuilt without its body (too large to keep)
  uint64_t not_kept_size_ = 0;     // ... which was this large
  std::map<std::string, std::string> filters_;  // JSONPath filter per request (by ident)

  // The tab moved to another request (ident): the body filter of the old one is kept for it and
  // the new one's (usually none) applied, so 03 Big's filter never hides 04 Redirect's body.
  void switch_filter(const std::string& ident) {
    if (ident == shown_ident_) return;
    const std::string cur = body_->filter_text();
    if (!shown_ident_.empty()) {
      if (cur.empty()) filters_.erase(shown_ident_);
      else if (filters_.size() < 512) filters_[shown_ident_] = cur;
    }
    const auto it = ident.empty() ? filters_.end() : filters_.find(ident);
    body_->set_filter(it == filters_.end() ? std::string() : it->second);
  }
  // Event streams (see body_tap).
  static constexpr double kSseBatchMs = 50;          // at most one batch onto the list this often
  static constexpr size_t kSseRawMax = 64u << 20;    // the stream's bytes kept for Raw while live
  static constexpr size_t kSseSyncParse = 1u << 20;  // a restored stream up to this is parsed inline
  SsePage* sse_ = nullptr;
  std::shared_ptr<SseTap> tap_;  // the tap of the HTTP send in flight (or the last one)
  Clock::time_point tap_t0_{};   // when it was made (its clock's zero)
  std::shared_ptr<const HttpResponse> head_shown_;  // the in-flight response's head on screen
  double head_at_ms_ = -1;       // ... which arrived then (ms on the tap's clock)
  wxTimer head_timer_;           // shows the head kHeadDelayMs after it arrived (unless done by then)
  wxTimer live_timer_;           // Download / rates of a response with its head shown
  uint64_t tap_seq_ = 0;         // bumps with every tap: a late post of an old one drops out
  bool tap_armed_ = false;       // body_tap() ran and the show_sending() of its send has not
  wxTimer sse_timer_;
  Clock::time_point sse_drained_at_{};
  bool sse_live_ = false;   // the send in flight is an event stream listed live
  bool sse_on_ = false;     // the Body tab shows the event list
  bool sse_raw_ = false;    // ... switched to Raw by the user
  std::string sse_raw_bytes_;  // the live stream's bytes (Raw while it runs)
  uint64_t sse_comments_ = 0;
  std::shared_ptr<const HttpResponse> sse_resp_;  // the run the list was made for
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

 public:
  ~ResponseView() override {
    *alive_ = false;
    if (top_) top_->Unbind(wxEVT_CHAR_HOOK, &ResponseView::on_top_key, this);
  }
};

}  // namespace

ResponsePanel* make_response_view(wxWindow* parent) {
  TraceScope ts("build");
  return new ResponseView(parent);
}

}  // namespace watt::app
