#include "app/response_conn.h"

#include <wx/clipbrd.h>
#include <wx/dcbuffer.h>
#include <wx/popupwin.h>
#include <wx/timer.h>

#include <algorithm>
#include <cmath>

#include "app/response_widgets.h"

namespace watt::app::rv {
namespace {

wxColour blend(const wxColour& a, const wxColour& b, double t) {
  auto m = [t](unsigned char x, unsigned char y) { return static_cast<unsigned char>(std::lround(x + (y - x) * t)); };
  return wxColour(m(a.Red(), b.Red()), m(a.Green(), b.Green()), m(a.Blue(), b.Blue()));
}

wxString W(const std::string& s) { return wxString::FromUTF8(s); }

void to_clipboard(const std::string& s) {
  if (wxTheClipboard->Open()) {
    wxTheClipboard->SetData(new wxTextDataObject(W(s)));
    wxTheClipboard->Close();
  }
}

class InfoPopup : public wxPopupTransientWindow {
 public:
  InfoPopup(wxWindow* owner, const std::string& title, std::vector<InfoRow> rows)
      : wxPopupTransientWindow(owner, wxBORDER_SIMPLE), title_(title), rows_(std::move(rows)), flash_(this) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(Theme::get().bg);
    layout();
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
    Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
      const int h = hit(e.GetPosition());
      const bool on_copy = copy_rect_.Contains(e.GetPosition());
      SetCursor(h >= 0 || on_copy ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
      if (h != hover_ || on_copy != hover_copy_) {
        hover_ = h;
        hover_copy_ = on_copy;
        Refresh(false);
      }
    });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
      hover_ = -1;
      hover_copy_ = false;
      Refresh(false);
    });
    Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
      if (copy_rect_.Contains(e.GetPosition())) return copy_all();
      const int h = hit(e.GetPosition());
      if (h < 0) return;
      to_clipboard(rows_[static_cast<size_t>(h)].value);
      flash("Copied " + rows_[static_cast<size_t>(h)].key);
    });
    Bind(wxEVT_CHAR_HOOK, &InfoPopup::on_key, this);
    // The popup may not get the keyboard focus (it opens from a click whose window keeps it):
    // its keys are caught in the app window too while it is open.
    top_ = wxGetTopLevelParent(owner);
    if (top_) top_->Bind(wxEVT_CHAR_HOOK, &InfoPopup::on_key, this);
    flash_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
      note_.clear();
      Refresh(false);
    });
  }

 private:
  struct Line {
    int row;  // index into rows_, -1: title / footer
    int y, h;
    std::vector<wxString> text;  // wrapped value lines
  };

  void copy_all() {
    std::string s = title_ + "\n";
    for (const InfoRow& r : rows_) {
      if (r.section) s += "\n[" + r.key + "]\n";
      else s += r.key + ": " + r.value + "\n";
    }
    to_clipboard(s);
    flash("Copied all");
  }
  void flash(const std::string& s) {
    note_ = s;
    Refresh(false);
    flash_.StartOnce(1500);
  }

  // Greedy word wrap of one value into lines at most `w` pixels wide (at most 10 lines).
  std::vector<wxString> wrap(wxDC& dc, const std::string& value, int w) {
    std::vector<wxString> out;
    wxString all = W(value);
    if (all.empty()) all = wxString::FromUTF8("\xE2\x80\x94");
    for (const wxString& para : wxSplit(all, '\n', 0)) {
      wxString line;
      size_t i = 0;
      while (i < para.size()) {
        size_t j = para.find(' ', i);
        if (j == wxString::npos) j = para.size();
        else ++j;
        wxString word = para.substr(i, j - i);
        i = j;
        if (dc.GetTextExtent(line + word).x <= w || line.empty()) {
          line += word;
          // A single word wider than the column: break it by characters.
          while (dc.GetTextExtent(line).x > w && line.size() > 1) {
            size_t k = line.size() - 1;
            while (k > 1 && dc.GetTextExtent(line.substr(0, k)).x > w) --k;
            out.push_back(line.substr(0, k));
            line = line.substr(k);
          }
        } else {
          out.push_back(line.Strip(wxString::trailing));
          line = word;
        }
      }
      out.push_back(line.Strip(wxString::trailing));
    }
    if (out.size() > 10) {
      out.resize(10);
      out.back() += wxString::FromUTF8(" \xE2\x80\xA6 (click to copy all)");
    }
    return out;
  }

  void layout() {
    wxClientDC dc(this);
    dc.SetFont(GetFont());
    const int ch = dc.GetCharHeight();
    pad_ = FromDIP(12);
    key_w_ = FromDIP(112);
    width_ = FromDIP(500);
    const int val_w = width_ - pad_ * 2 - key_w_;
    int y = pad_ + ch + FromDIP(10);  // title row
    lines_.clear();
    for (size_t i = 0; i < rows_.size(); ++i) {
      const InfoRow& r = rows_[i];
      Line l{static_cast<int>(i), y, 0, {}};
      if (r.section) {
        if (i > 0) y += FromDIP(6);
        l.y = y;
        l.h = ch + FromDIP(6);
      } else {
        l.text = wrap(dc, r.value, val_w);
        l.h = static_cast<int>(l.text.size()) * ch + FromDIP(5);
      }
      y += l.h;
      lines_.push_back(std::move(l));
    }
    height_ = y + FromDIP(8) + ch + pad_ / 2;  // footer
    SetClientSize(width_, height_);
  }

  int hit(const wxPoint& p) const {
    for (const Line& l : lines_)
      if (!rows_[static_cast<size_t>(l.row)].section && p.y >= l.y && p.y < l.y + l.h && p.x > FromDIP(4) &&
          p.x < width_ - FromDIP(4))
        return l.row;
    return -1;
  }

  void paint() {
    wxAutoBufferedPaintDC dc(this);
    const Theme& th = Theme::get();
    const wxColour panel = th.dark ? blend(th.bg, th.fg, 0.06) : th.bg;
    dc.SetBackground(wxBrush(panel));
    dc.Clear();
    wxFont base = GetFont();
    wxFont bold = base.Bold();
    wxFont small = base.Smaller();
    const int ch = dc.GetCharHeight();
    // Title + "Copy all".
    dc.SetFont(bold);
    dc.SetTextForeground(th.fg);
    dc.DrawText(W(title_), pad_, pad_);
    dc.SetFont(base);
    const wxString copy = "Copy all";
    const wxSize ce = dc.GetTextExtent(copy);
    copy_rect_ = wxRect(width_ - pad_ - ce.x - FromDIP(12), pad_ - FromDIP(3), ce.x + FromDIP(12), ce.y + FromDIP(6));
    dc.SetPen(wxPen(blend(panel, th.accent, 0.5)));
    dc.SetBrush(wxBrush(hover_copy_ ? blend(panel, th.accent, 0.18) : panel));
    dc.DrawRoundedRectangle(copy_rect_, FromDIP(3));
    dc.SetTextForeground(th.accent);
    dc.DrawText(copy, copy_rect_.x + FromDIP(6), copy_rect_.y + FromDIP(3));
    dc.SetPen(wxPen(th.grid));
    dc.DrawLine(pad_, pad_ + ch + FromDIP(5), width_ - pad_, pad_ + ch + FromDIP(5));

    for (const Line& l : lines_) {
      const InfoRow& r = rows_[static_cast<size_t>(l.row)];
      if (r.section) {
        dc.SetFont(small.Bold());
        dc.SetTextForeground(th.dim);
        wxString t = W(r.key);
        t.MakeUpper();
        dc.DrawText(t, pad_, l.y + FromDIP(4));
        continue;
      }
      if (l.row == hover_) {
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(blend(panel, th.fg, 0.07)));
        dc.DrawRoundedRectangle(wxRect(FromDIP(4), l.y, width_ - FromDIP(8), l.h), FromDIP(3));
      }
      dc.SetFont(base);
      dc.SetTextForeground(th.dim);
      dc.DrawText(W(r.key), pad_, l.y + FromDIP(2));
      dc.SetTextForeground(r.warn ? th.warn : th.fg);
      int y = l.y + FromDIP(2);
      for (const wxString& s : l.text) {
        dc.DrawText(s, pad_ + key_w_, y);
        y += ch;
      }
    }
    // Footer: hint or the last copy.
    dc.SetFont(small);
    dc.SetTextForeground(note_.empty() ? th.dim : th.ok);
    dc.DrawText(note_.empty() ? wxString::FromUTF8("Click or \xE2\x86\x91\xE2\x86\x93 Enter copies a row \xC2\xB7 Ctrl+C copies all \xC2\xB7 Esc closes")
                              : W(note_),
                pad_, height_ - pad_ / 2 - ch);
  }

  void on_key(wxKeyEvent& e) {
    if (e.GetKeyCode() == WXK_ESCAPE && e.GetModifiers() == 0) {
      unhook();
      Dismiss();
      CallAfter([this] { Destroy(); });
      return;
    }
    if (e.GetModifiers() == wxMOD_CONTROL && (e.GetKeyCode() == 'C' || e.GetKeyCode() == WXK_INSERT)) return copy_all();
    // Keyboard row cursor (the same highlight as the mouse): Up/Down/Home/End move it, Enter or
    // Space copies that row's value.
    const int k = e.GetKeyCode();
    if (e.GetModifiers() == 0) {
      if (k == WXK_DOWN || k == WXK_UP || k == WXK_HOME || k == WXK_END || k == WXK_TAB) {
        move_cursor(k == WXK_UP ? -1 : k == WXK_HOME ? -2 : k == WXK_END ? 2 : 1);
        return;
      }
      if ((k == WXK_RETURN || k == WXK_NUMPAD_ENTER || k == WXK_SPACE) && hover_ >= 0) {
        to_clipboard(rows_[static_cast<size_t>(hover_)].value);
        flash("Copied " + rows_[static_cast<size_t>(hover_)].key);
        return;
      }
    }
    if (e.GetModifiers() == wxMOD_SHIFT && k == WXK_TAB) return move_cursor(-1);
    e.Skip();
  }
  // by: +1 / -1 next / previous value row, -2 first, +2 last.
  void move_cursor(int by) {
    std::vector<int> rows;
    for (size_t i = 0; i < rows_.size(); ++i)
      if (!rows_[i].section) rows.push_back(static_cast<int>(i));
    if (rows.empty()) return;
    auto at = std::find(rows.begin(), rows.end(), hover_);
    int i = at == rows.end() ? -1 : static_cast<int>(at - rows.begin());
    const int n = static_cast<int>(rows.size());
    if (by == -2) i = 0;
    else if (by == 2) i = n - 1;
    else if (i < 0) i = by > 0 ? 0 : n - 1;
    else i = (i + by + n) % n;
    hover_ = rows[static_cast<size_t>(i)];
    hover_copy_ = false;
    Refresh(false);
  }
  void unhook() {
    if (top_) top_->Unbind(wxEVT_CHAR_HOOK, &InfoPopup::on_key, this);
    top_ = nullptr;
  }
  ~InfoPopup() override { unhook(); }

  void OnDismiss() override {
    unhook();
    CallAfter([this] { Destroy(); });
  }
  wxWindow* top_ = nullptr;

 public:
  void popup() {
    Popup();
    // Take the keyboard once the click that opened it is over.
    CallAfter([this] { SetFocus(); });
  }

 private:

  std::string title_;
  std::vector<InfoRow> rows_;
  std::vector<Line> lines_;
  int pad_ = 0, key_w_ = 0, width_ = 0, height_ = 0;
  int hover_ = -1;
  bool hover_copy_ = false;
  wxRect copy_rect_;
  std::string note_;
  wxTimer flash_;
};

}  // namespace

void show_info_popup(wxWindow* owner, const wxRect& anchor, const std::string& title, std::vector<InfoRow> rows) {
  auto* p = new InfoPopup(owner, title, std::move(rows));
  // Under the anchor, kept inside the app window (right-aligned to it when it would stick out).
  const wxSize sz = p->GetSize();
  wxWindow* top = wxGetTopLevelParent(owner);
  const wxRect win = top ? top->GetScreenRect() : wxRect(anchor.GetTopLeft(), sz);
  int x = std::min(anchor.x, win.GetRight() - sz.x - 8);
  x = std::max(x, win.x + 8);
  int y = anchor.GetBottom() + 3;
  if (y + sz.y > win.GetBottom() - 8 && anchor.y - sz.y - 3 > win.y) y = anchor.y - sz.y - 3;
  p->SetPosition(wxPoint(x, y));
  p->popup();
}

}  // namespace watt::app::rv
