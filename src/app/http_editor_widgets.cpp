#include "app/http_editor_widgets.h"

#include <wx/app.h>
#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#endif
#include <wx/dcbuffer.h>
#include <wx/display.h>
#include <wx/arrstr.h>
#include <wx/menu.h>
#include <wx/settings.h>

#include <algorithm>

#include "app/http_editor_util.h"

namespace watt::app::http_editor {

const Theme& Theme::get() {
  static const Theme theme = [] {
    Theme x;
    x.dark = wxSystemSettings::GetAppearance().IsDark();
    x.bg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
    if (x.dark) {
      x.field = x.bg;
      x.fg = wxColour(0xE6, 0xE6, 0xE6);
      x.dim = wxColour(0x8A, 0x8A, 0x8A);
      x.border = wxColour(0x3C, 0x3C, 0x3C);
      x.hover = wxColour(0x33, 0x33, 0x33);
      x.accent = wxColour(0x1F, 0x6F, 0xEB);
      x.accent_fg = *wxWHITE;
      x.var_ok = wxColour(0x6C, 0xCB, 0x8B);
      x.var_ok_bg = wxColour(0x1C, 0x3A, 0x28);
      x.var_bad = wxColour(0xF2, 0x7A, 0x6E);
      x.var_bad_bg = wxColour(0x46, 0x22, 0x20);
      x.warn_bg = wxColour(0x3D, 0x2E, 0x10);
      x.warn_border = wxColour(0xC8, 0x8A, 0x1E);
      x.info_bg = wxColour(0x1E, 0x2B, 0x3C);
      x.js_key = wxColour(0x9C, 0xDC, 0xFE);
      x.js_string = wxColour(0xCE, 0x91, 0x78);
      x.js_number = wxColour(0xB5, 0xCE, 0xA8);
      x.js_keyword = wxColour(0x56, 0x9C, 0xD6);
      x.js_punct = wxColour(0xC8, 0xC8, 0xC8);
      x.js_error = wxColour(0xF4, 0x47, 0x47);
    } else {
      x.field = *wxWHITE;
      x.fg = wxColour(0x21, 0x21, 0x21);
      x.dim = wxColour(0x80, 0x80, 0x80);
      x.border = wxColour(0xDA, 0xDA, 0xDA);
      x.hover = wxColour(0xEE, 0xEE, 0xEE);
      x.accent = wxColour(0x0B, 0x63, 0xD8);
      x.accent_fg = *wxWHITE;
      x.var_ok = wxColour(0x1A, 0x7F, 0x37);
      x.var_ok_bg = wxColour(0xDD, 0xF4, 0xE4);
      x.var_bad = wxColour(0xC2, 0x2B, 0x1E);
      x.var_bad_bg = wxColour(0xFC, 0xE4, 0xE1);
      x.warn_bg = wxColour(0xFF, 0xF4, 0xD6);
      x.warn_border = wxColour(0xD9, 0x9A, 0x16);
      x.info_bg = wxColour(0xE8, 0xF1, 0xFD);
      x.js_key = wxColour(0x04, 0x51, 0xA5);
      x.js_string = wxColour(0xA3, 0x15, 0x15);
      x.js_number = wxColour(0x09, 0x86, 0x58);
      x.js_keyword = wxColour(0x00, 0x00, 0xFF);
      x.js_punct = wxColour(0x44, 0x44, 0x44);
      x.js_error = wxColour(0xE0, 0x00, 0x00);
    }
    return x;
  }();
  return theme;
}

wxColour method_colour(const std::string& m) {
  const bool dark = Theme::get().dark;
  if (m == "GET") return dark ? wxColour(0x6B, 0xDD, 0x9A) : wxColour(0x00, 0x7F, 0x31);
  if (m == "POST") return dark ? wxColour(0xFF, 0xE4, 0x7E) : wxColour(0xAD, 0x7A, 0x03);
  if (m == "PUT") return dark ? wxColour(0x74, 0xAE, 0xF6) : wxColour(0x09, 0x5A, 0xB5);
  if (m == "PATCH") return dark ? wxColour(0xC0, 0xA8, 0xE1) : wxColour(0x62, 0x3E, 0xBA);
  if (m == "DELETE") return dark ? wxColour(0xF7, 0x9A, 0x8E) : wxColour(0x8E, 0x1A, 0x10);
  return dark ? wxColour(0xC0, 0xC0, 0xC0) : wxColour(0x50, 0x50, 0x50);
}

// ------------------------------------------------------------------ FlatButton

FlatButton::FlatButton(wxWindow* parent, const wxString& label, Style style, bool dropdown)
    : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxWANTS_CHARS),
      label_(label), style_(style), dropdown_(dropdown) {
  SetBackgroundStyle(wxBG_STYLE_PAINT);
  wxControl::SetLabel(label);
  if (style_ == Style::Method) SetFont(GetFont().Bold());
  if (style_ == Style::Primary) SetFont(GetFont().Bold());
  Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
  Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { hover_ = true; Refresh(); });
  Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { hover_ = down_ = false; Refresh(); });
  Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
    down_ = true;
    SetFocus();
    Refresh();
  });
  Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) {
    const bool was = down_;
    down_ = false;
    Refresh();
    // Where the button is released, not hover_: a click without a mouse move before it (a
    // tablet tap, a synthesized click) never sends ENTER_WINDOW.
    if (was && GetClientRect().Contains(e.GetPosition())) fire();
  });
  Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent&) { fire(); });
  Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
    const int k = e.GetKeyCode();
    if ((k == WXK_SPACE || k == WXK_RETURN || k == WXK_NUMPAD_ENTER) && !e.HasAnyModifiers()) {
      fire();
    } else if (k == WXK_TAB) {
      Navigate(e.ShiftDown() ? wxNavigationKeyEvent::IsBackward : wxNavigationKeyEvent::IsForward);
    } else {
      e.Skip();
    }
  });
  Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) { e.Skip(); Refresh(); });
  Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) { e.Skip(); Refresh(); });
}

void FlatButton::SetLabel(const wxString& label) {
  if (label == label_) return;
  label_ = label;
  wxControl::SetLabel(label);  // so GetLabel() (and accessibility) reflect it
  InvalidateBestSize();
  Refresh();
}

void FlatButton::set_text_colour(const wxColour& c) {
  text_ = c;
  Refresh();
}

void FlatButton::fire() {
  if (on_click) on_click();
}

wxSize FlatButton::DoGetBestClientSize() const {
  wxClientDC dc(const_cast<FlatButton*>(this));
  dc.SetFont(GetFont());
  wxSize s = dc.GetTextExtent(label_);
  // Primary / method buttons keep their width when the label changes (Send <-> Cancel).
  if (style_ == Style::Primary) s.x = std::max(s.x, dc.GetTextExtent("Cancel").x);
  if (style_ == Style::Method) s.x = std::max(s.x, dc.GetTextExtent("OPTIONS").x);
  int w = s.x + FromDIP(style_ == Style::Subtle ? 16 : 32);
  if (dropdown_) w += FromDIP(14);
  return wxSize(w, s.y + FromDIP(style_ == Style::Subtle ? 10 : 16));
}

void FlatButton::paint() {
  wxAutoBufferedPaintDC dc(this);
  const Theme& t = Theme::get();
  const wxSize sz = GetClientSize();
  wxColour parent_bg = GetParent()->GetBackgroundColour();
  dc.SetBackground(wxBrush(parent_bg));
  dc.Clear();
  wxColour fill, fg;
  switch (style_) {
    case Style::Primary:
      fill = t.accent;
      if (hover_) fill = fill.ChangeLightness(down_ ? 90 : 112);
      fg = t.accent_fg;
      break;
    case Style::Subtle:
    case Style::Method:
    case Style::Outline:
      fill = hover_ ? t.hover : field_look_ ? t.field : parent_bg;
      fg = text_.IsOk() ? text_ : t.fg;
      break;
  }
  if (!IsEnabled()) fg = t.dim;
  dc.SetPen(HasFocus() && style_ != Style::Primary ? wxPen(t.accent, FromDIP(1))
            : style_ == Style::Outline || field_look_ ? wxPen(hover_ ? t.dim : t.border, FromDIP(1))
                                                 : *wxTRANSPARENT_PEN);
  if (HasFocus() && style_ == Style::Primary) dc.SetPen(wxPen(t.accent_fg, FromDIP(1), wxPENSTYLE_DOT));
  dc.SetBrush(wxBrush(fill));
  if (field_look_) dc.DrawRectangle(0, 0, sz.x, sz.y);  // square, like the wxBORDER_SIMPLE fields
  else dc.DrawRoundedRectangle(0, 0, sz.x, sz.y, FromDIP(4));
  dc.SetFont(GetFont());
  dc.SetTextForeground(fg);
  const wxSize ts = dc.GetTextExtent(label_);
  int x = style_ == Style::Method ? FromDIP(12)
          : field_look_            ? FromDIP(4)
                                   : (sz.x - ts.x - (dropdown_ ? FromDIP(14) : 0)) / 2;
  dc.DrawText(label_, x, (sz.y - ts.y) / 2);
  if (dropdown_) {
    const int ax = sz.x - FromDIP(16), ay = sz.y / 2 - FromDIP(2);
    const int a = FromDIP(4);
    wxPoint pts[3] = {{ax, ay}, {ax + 2 * a, ay}, {ax + a, ay + a}};
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(style_ == Style::Primary ? fg : t.dim));
    dc.DrawPolygon(3, pts);
  }
}

void FlatButton::set_field_look(bool on) {
  if (field_look_ == on) return;
  field_look_ = on;
  Refresh(false);
}

// ------------------------------------------------------------------ ChoiceButton

ChoiceButton::ChoiceButton(wxWindow* parent, std::vector<wxString> items)
    : FlatButton(parent, items.empty() ? wxString() : items[0], Style::Subtle, true), items_(std::move(items)) {
  // As wide as the widest choice, so the button does not jump when the choice changes.
  int w = 0;
  for (const auto& it : items_) {
    SetLabel(it);
    w = std::max(w, GetBestSize().x);
  }
  if (!items_.empty()) SetLabel(items_[0]);
  SetMinSize(wxSize(w, -1));
  on_click = [this] {
    if (list_) {
      close_list();  // a second click closes it, like a combo box
      return;
    }
    std::vector<PickItem> items;
    for (const auto& it : items_) items.push_back({it, "", ""});
    auto* p = new PickPopup(this, std::move(items), sel_ >= 0 && sel_ < static_cast<int>(items_.size())
                                                        ? items_[static_cast<size_t>(sel_)]
                                                        : wxString());
    list_ = p;
    p->on_close = [this, p] {
      if (list_ == p) list_ = nullptr;
    };
    p->on_pick = [this](const wxString& v) {
      for (size_t i = 0; i < items_.size(); ++i)
        if (items_[i] == v) {
          pick(static_cast<int>(i));
          break;
        }
    };
    SetFocus();
    p->show_below(GetScreenRect());
  };
  // The open list takes the keys (the button keeps the focus): ahead of the dialog's Esc.
  Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
    if (list_ && e.GetKeyCode() == WXK_ESCAPE) {
      close_list();
      return;
    }
    e.Skip();
  });
  Bind(wxEVT_CHAR, [this](wxKeyEvent& e) {
    const wxChar c = e.GetUnicodeKey();
    if (list_ && c != WXK_NONE && !e.ControlDown() && !e.AltDown() && list_->chr(c)) return;
    e.Skip();
  });
  Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) {
    e.Skip();
    if (list_)
      CallAfter([this] {
        if (!list_) return;
        // A press on the list itself picks there.
        if (list_->GetScreenRect().Contains(wxGetMousePosition()) && wxGetMouseState().LeftIsDown()) return;
        close_list();
      });
  });
  Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
    const int k = e.GetKeyCode();
    if (list_ && !e.ControlDown() && !e.AltDown()) {
      if (k == WXK_SHIFT) return;
      if (list_->key(k)) return;  // Up / Down / Enter / Tab / Esc / Backspace
      e.Skip();                   // a character: filtered on wxEVT_CHAR
      return;
    }
    const int n = static_cast<int>(items_.size());
    const bool up = k == WXK_UP || k == WXK_NUMPAD_UP, down = k == WXK_DOWN || k == WXK_NUMPAD_DOWN;
    if ((up || down) && !e.HasAnyModifiers() && n > 0) {
      pick(std::clamp(sel_ + (down ? 1 : -1), 0, n - 1));
      return;
    }
    e.Skip();  // FlatButton's own handler: Space / Enter open the list, Tab moves on
  });
}

void ChoiceButton::close_list() {
  if (list_) list_->close();  // its on_close clears list_
}

void ChoiceButton::set_selection(int index) {
  if (index < 0 || index >= static_cast<int>(items_.size())) return;
  sel_ = index;
  SetLabel(items_[static_cast<size_t>(index)]);
}

void ChoiceButton::set_item(int index, const wxString& text) {
  if (index < 0 || index >= static_cast<int>(items_.size()) || items_[static_cast<size_t>(index)] == text) return;
  items_[static_cast<size_t>(index)] = text;
  if (index == sel_) SetLabel(text);
  const wxString shown = GetLabel();
  SetLabel(text);
  const int w = GetBestSize().x;
  SetLabel(shown);
  if (w > GetMinSize().x) {
    SetMinSize(wxSize(w, GetMinSize().y));  // a height set by the owner stays
    if (GetParent()) GetParent()->Layout();
  }
}

void ChoiceButton::pick(int index) {
  if (index == sel_) return;
  set_selection(index);
  if (on_select) on_select(index);
}

// ------------------------------------------------------------------ CheckButton

CheckButton::CheckButton(wxWindow* parent, const wxString& label)
    : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxWANTS_CHARS), label_(label) {
  SetBackgroundStyle(wxBG_STYLE_PAINT);
  wxControl::SetLabel(label);
  Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
  Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { hover_ = true; Refresh(); });
  Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { hover_ = false; Refresh(); });
  Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
    SetFocus();
    toggle();
  });
  Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent&) { toggle(); });
  Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
    const int k = e.GetKeyCode();
    if (k == WXK_SPACE && !e.HasAnyModifiers()) toggle();
    else if (k == WXK_TAB) Navigate(e.ShiftDown() ? wxNavigationKeyEvent::IsBackward : wxNavigationKeyEvent::IsForward);
    else e.Skip();
  });
  Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) { e.Skip(); Refresh(); });
  Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) { e.Skip(); Refresh(); });
}

void CheckButton::set_value(bool on) {
  if (on == on_) return;
  on_ = on;
  Refresh();
}

void CheckButton::set_label(const wxString& label) {
  if (label == label_) return;
  label_ = label;
  InvalidateBestSize();
  Refresh();
}

void CheckButton::toggle() {
  on_ = !on_;
  Refresh();
  if (on_change) on_change();
}

wxSize CheckButton::DoGetBestClientSize() const {
  wxClientDC dc(const_cast<CheckButton*>(this));
  dc.SetFont(GetFont());
  const wxSize s = dc.GetTextExtent(label_);
  return wxSize(FromDIP(16 + 8 + 4) + s.x, std::max(s.y, FromDIP(16)) + FromDIP(6));
}

void CheckButton::paint() {
  wxAutoBufferedPaintDC dc(this);
  const Theme& t = Theme::get();
  const wxSize sz = GetClientSize();
  dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
  dc.Clear();
  const int b = FromDIP(16);
  const wxRect box(FromDIP(1), (sz.y - b) / 2, b, b);
  dc.SetPen(wxPen(on_ ? t.accent : (hover_ || HasFocus() ? t.dim : t.border), FromDIP(1)));
  dc.SetBrush(wxBrush(on_ ? t.accent : t.field));
  dc.DrawRoundedRectangle(box, FromDIP(3));
  if (on_) {
    dc.SetPen(wxPen(t.accent_fg, FromDIP(2)));
    const int x = box.x, y = box.y;
    dc.DrawLine(x + FromDIP(4), y + FromDIP(8), x + FromDIP(7), y + FromDIP(11));
    dc.DrawLine(x + FromDIP(7), y + FromDIP(11), x + FromDIP(12), y + FromDIP(5));
  }
  dc.SetFont(GetFont());
  dc.SetTextForeground(IsEnabled() ? t.fg : t.dim);
  const wxSize ts = dc.GetTextExtent(label_);
  const int tx = box.GetRight() + FromDIP(9);
  dc.DrawText(label_, tx, (sz.y - ts.y) / 2);
  if (HasFocus()) {
    dc.SetPen(wxPen(t.accent, FromDIP(1)));
    dc.DrawLine(tx, (sz.y + ts.y) / 2, tx + ts.x, (sz.y + ts.y) / 2);
  }
}

// ------------------------------------------------------------------ TabStrip

TabStrip::TabStrip(wxWindow* parent)
    : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE) {
  SetBackgroundStyle(wxBG_STYLE_PAINT);
  // Shrinks with the pane (the tabs that do not fit go under the overflow button), so the
  // controls next to it ("Copy as") keep their room instead of being drawn over the tabs.
  SetMinSize(wxSize(FromDIP(48), -1));
  Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
  Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
    Refresh(false);
    e.Skip();
  });
  Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
    int h = more_rect_.Contains(e.GetPosition()) ? -2 : -1;
    for (size_t i = 0; i < tabs_.size(); ++i)
      if (tabs_[i].rect.Contains(e.GetPosition())) h = static_cast<int>(i);
    if (h != hover_) {
      hover_ = h;
      SetCursor(h != -1 ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
      if (h == -2) {
        wxString hidden;
        for (const auto& t : tabs_)
          if (t.rect.IsEmpty()) hidden += (hidden.empty() ? "" : ", ") + t.label;
        SetToolTip("More tabs: " + hidden);
      } else {
        UnsetToolTip();
      }
      Refresh();
    }
  });
  Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
    hover_ = -1;
    Refresh();
  });
  Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
    if (more_rect_.Contains(e.GetPosition())) return more_menu();
    for (size_t i = 0; i < tabs_.size(); ++i)
      if (tabs_[i].rect.Contains(e.GetPosition())) {
        select(i);
        if (on_select) on_select(i);
        return;
      }
  });
}

void TabStrip::more_menu() {
  wxMenu menu;
  for (size_t i = 0; i < tabs_.size(); ++i) {
    if (!tabs_[i].rect.IsEmpty()) continue;
    const wxString l = tabs_[i].badge.empty() ? tabs_[i].label : tabs_[i].label + "   " + tabs_[i].badge;
    menu.Append(static_cast<int>(1000 + i), l);
  }
  if (!menu.GetMenuItemCount()) return;
  const int id = GetPopupMenuSelectionFromUser(menu, more_rect_.GetBottomLeft());
  if (id < 1000 || static_cast<size_t>(id - 1000) >= tabs_.size()) return;
  const size_t i = static_cast<size_t>(id - 1000);
  select(i);
  if (on_select) on_select(i);
}

void TabStrip::set_tabs(const std::vector<wxString>& labels) {
  tabs_.clear();
  for (const auto& l : labels) tabs_.push_back({l, {}, {}, false});
  InvalidateBestSize();
  Refresh();
}

void TabStrip::set_badge(size_t index, const wxString& badge, bool warn) {
  if (index >= tabs_.size() || (tabs_[index].badge == badge && tabs_[index].warn == warn)) return;
  tabs_[index].badge = badge;
  tabs_[index].warn = warn;
  Refresh();
}

void TabStrip::select(size_t index) {
  if (index >= tabs_.size() || index == sel_) return;
  sel_ = index;
  Refresh();
}

wxSize TabStrip::DoGetBestClientSize() const {
  wxClientDC dc(const_cast<TabStrip*>(this));
  dc.SetFont(GetFont());
  return wxSize(FromDIP(300), dc.GetCharHeight() + FromDIP(16));
}

void TabStrip::layout_tabs(wxDC& dc) {
  int x = 0;
  const wxSize cs = GetClientSize();
  const wxFont bold = GetFont().Bold();
  std::vector<int> widths;
  int sum = 0;
  for (auto& t : tabs_) {
    dc.SetFont(bold);  // width of the bold label, so selecting a tab does not shift the others
    int w = dc.GetTextExtent(t.label).x;
    if (!t.badge.empty()) {
      dc.SetFont(GetFont().Smaller());
      w += FromDIP(5) + dc.GetTextExtent(t.badge).x;
    }
    widths.push_back(w);
    sum += w;
  }
  // Narrow (badges shown, a narrow window): less space between the tabs first; when even that
  // does not fit, the selected tab and the ones before it that fit stay, the rest go under a
  // "»" button at the end (never a label cut or drawn past the strip).
  const int n = static_cast<int>(tabs_.size());
  pad_ = FromDIP(20);
  if (n && sum + pad_ * n > cs.x) pad_ = std::max(FromDIP(8), (cs.x - sum) / n);
  std::vector<char> shown(tabs_.size(), 1);
  more_rect_ = wxRect();
  if (n && sum + pad_ * n > cs.x) {
    pad_ = FromDIP(12);
    const int more_w = FromDIP(28);
    int used = sel_ < tabs_.size() ? widths[sel_] + pad_ : 0;
    bool full = false;
    for (size_t i = 0; i < tabs_.size(); ++i) {
      if (i == sel_) continue;
      if (!full && used + widths[i] + pad_ <= cs.x - more_w) used += widths[i] + pad_;
      else full = true, shown[i] = 0;
    }
  }
  for (size_t i = 0; i < tabs_.size(); ++i) {
    if (!shown[i]) {
      tabs_[i].rect = wxRect();
      continue;
    }
    tabs_[i].rect = wxRect(x, 0, widths[i] + pad_, cs.y);
    x += tabs_[i].rect.width;
  }
  if (std::find(shown.begin(), shown.end(), 0) != shown.end())
    more_rect_ = wxRect(std::min(x, cs.x - FromDIP(28)), 0, FromDIP(28), cs.y);
}

void TabStrip::paint() {
  wxAutoBufferedPaintDC dc(this);
  const Theme& th = Theme::get();
  dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
  dc.Clear();
  layout_tabs(dc);
  const wxSize sz = GetClientSize();
  if (!more_rect_.IsEmpty()) {
    // The overflow button; in the warning colour when a hidden tab has a warning badge.
    bool warn = false;
    for (const auto& t : tabs_) warn = warn || (t.rect.IsEmpty() && t.warn);
    if (hover_ == -2) {
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.SetBrush(wxBrush(th.hover));
      dc.DrawRoundedRectangle(more_rect_.Deflate(FromDIP(2), FromDIP(5)), FromDIP(3));
    }
    dc.SetFont(GetFont().Bold());
    dc.SetTextForeground(warn ? th.var_bad : hover_ == -2 ? th.fg : th.dim);
    const wxString m = wxString::FromUTF8("\xC2\xBB");
    const wxSize e = dc.GetTextExtent(m);
    dc.DrawText(m, more_rect_.x + (more_rect_.width - e.x) / 2, (sz.y - e.y) / 2);
  }
  for (size_t i = 0; i < tabs_.size(); ++i) {
    const auto& t = tabs_[i];
    if (t.rect.IsEmpty()) continue;
    const bool sel = i == sel_;
    dc.SetFont(sel ? GetFont().Bold() : GetFont());
    dc.SetTextForeground(sel || static_cast<int>(i) == hover_ ? th.fg : th.dim);
    const int ty = (sz.y - dc.GetCharHeight()) / 2;
    int x = t.rect.x + pad_ / 2;
    dc.DrawText(t.label, x, ty);
    x += dc.GetTextExtent(t.label).x + FromDIP(5);
    if (!t.badge.empty()) {
      dc.SetFont(GetFont().Smaller());
      dc.SetTextForeground(t.warn ? th.var_bad : th.var_ok);
      dc.DrawText(t.badge, x, ty + FromDIP(1));
    }
    if (sel) {
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.SetBrush(wxBrush(th.accent));
      const int inset = std::min(FromDIP(8), pad_ / 2);
      dc.DrawRectangle(t.rect.x + inset, sz.y - FromDIP(2), t.rect.width - 2 * inset, FromDIP(2));
    }
  }
}

// ------------------------------------------------------------------ RadioStrip

RadioStrip::RadioStrip(wxWindow* parent, const std::vector<wxString>& labels)
    : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxWANTS_CHARS), labels_(labels) {
  SetBackgroundStyle(wxBG_STYLE_PAINT);
  Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
  Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
    int h = -1;
    for (size_t i = 0; i < rects_.size(); ++i)
      if (rects_[i].Contains(e.GetPosition())) h = static_cast<int>(i);
    if (h != hover_) {
      hover_ = h;
      Refresh();
    }
  });
  Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
    hover_ = -1;
    Refresh();
  });
  Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
    SetFocus();
    for (size_t i = 0; i < rects_.size(); ++i)
      if (rects_[i].Contains(e.GetPosition())) pick(static_cast<int>(i));
  });
  Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
    const int n = static_cast<int>(labels_.size());
    if (e.HasAnyModifiers() && e.GetModifiers() != wxMOD_SHIFT) {
      e.Skip();
      return;
    }
    switch (e.GetKeyCode()) {
      case WXK_LEFT: case WXK_UP: case WXK_NUMPAD_LEFT: case WXK_NUMPAD_UP:
        if (sel_ > 0) pick(sel_ - 1);
        return;
      case WXK_RIGHT: case WXK_DOWN: case WXK_NUMPAD_RIGHT: case WXK_NUMPAD_DOWN:
        if (sel_ + 1 < n) pick(sel_ + 1);
        return;
      case WXK_HOME: pick(0); return;
      case WXK_END: pick(n - 1); return;
      case WXK_TAB:
        Navigate(e.ShiftDown() ? wxNavigationKeyEvent::IsBackward : wxNavigationKeyEvent::IsForward);
        return;
      default: e.Skip();
    }
  });
  Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) { e.Skip(); Refresh(); });
  Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) { e.Skip(); Refresh(); });
}

void RadioStrip::select(int index) {
  if (index >= static_cast<int>(labels_.size()) || index == sel_) return;
  sel_ = index < 0 ? -1 : index;  // -1: none
  Refresh();
}

void RadioStrip::pick(int index) {
  if (index < 0 || index >= static_cast<int>(labels_.size())) return;
  const bool changed = index != sel_;
  sel_ = index;
  Refresh();
  if (changed && on_select) on_select(index);
}

wxSize RadioStrip::DoGetBestClientSize() const {
  wxClientDC dc(const_cast<RadioStrip*>(this));
  dc.SetFont(GetFont());
  int w = 0;
  for (const auto& l : labels_) w += dc.GetTextExtent(l).x + FromDIP(14 + 6 + 16);
  return wxSize(w, dc.GetCharHeight() + FromDIP(10));
}

void RadioStrip::layout(wxDC& dc) {
  rects_.clear();
  int x = 0;
  const int h = GetClientSize().y;
  for (const auto& l : labels_) {
    const int w = FromDIP(14 + 6) + dc.GetTextExtent(l).x + FromDIP(8);
    rects_.emplace_back(x, 0, w, h);
    x += w + FromDIP(8);
  }
}

void RadioStrip::paint() {
  wxAutoBufferedPaintDC dc(this);
  const Theme& th = Theme::get();
  dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
  dc.Clear();
  dc.SetFont(GetFont());
  layout(dc);
  const int h = GetClientSize().y;
  const int d = FromDIP(14);
  for (size_t i = 0; i < labels_.size(); ++i) {
    const wxRect& r = rects_[i];
    const bool sel = static_cast<int>(i) == sel_;
    const bool hov = static_cast<int>(i) == hover_;
    const int cx = r.x + FromDIP(2) + d / 2, cy = h / 2;
    dc.SetPen(wxPen(sel ? th.accent : (hov ? th.fg : th.dim), FromDIP(1)));
    dc.SetBrush(wxBrush(th.field));
    dc.DrawCircle(cx, cy, d / 2);
    if (sel) {
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.SetBrush(wxBrush(th.accent));
      dc.DrawCircle(cx, cy, d / 2 - FromDIP(3));
    }
    dc.SetTextForeground(sel || hov ? th.fg : th.dim);
    const wxSize ts = dc.GetTextExtent(labels_[i]);
    dc.DrawText(labels_[i], r.x + FromDIP(2) + d + FromDIP(6), (h - ts.y) / 2);
    if (sel && HasFocus()) {  // keyboard focus: an accent outline, never a dotted native one
      dc.SetPen(wxPen(th.accent, FromDIP(1)));
      dc.SetBrush(*wxTRANSPARENT_BRUSH);
      dc.DrawRoundedRectangle(r.x - FromDIP(2), FromDIP(1), r.width + FromDIP(2), h - FromDIP(2), FromDIP(3));
    }
  }
}


// ------------------------------------------------------------------ PickPopup

PickPopup::PickPopup(wxWindow* owner, std::vector<PickItem> items, const wxString& current)
    : wxPopupWindow(owner, wxBORDER_NONE), items_(std::move(items)), current_(current) {
  SetBackgroundStyle(wxBG_STYLE_PAINT);
  SetFont(owner->GetFont());
  Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
  Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
    const int r = row_at(e.GetPosition().y);
    const bool item = r >= 0 && rows_[static_cast<size_t>(r)].item >= 0;
    if (no_preselect) {  // hover only: a key choice stays the user's own
      const int h = item ? r : -1;
      if (h != hover_) {
        hover_ = h;
        Refresh();
      }
      return;
    }
    if (item && r != sel_) {
      sel_ = r;
      Refresh();
    }
  });
  Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
    if (hover_ >= 0) {
      hover_ = -1;
      Refresh();
    }
  });
  // Picked on the press: the release may land after the popup is gone (and a press that moves
  // the focus would otherwise close it first).
  Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
    const int r = row_at(e.GetPosition().y);
    if (r >= 0 && rows_[static_cast<size_t>(r)].item >= 0) {
      sel_ = r;
      pick_selected();
    }
  });
  Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& e) {
    const int n = static_cast<int>(rows_.size()), vis = visible_rows();
    top_ = std::clamp(top_ - (e.GetWheelRotation() > 0 ? 3 : -3), 0, std::max(0, n - vis));
    Refresh();
  });
  rebuild();
}

int PickPopup::row_h() const { return GetCharHeight() + FromDIP(9); }
int PickPopup::header_h() const { return compact ? FromDIP(3) : GetCharHeight() + FromDIP(12); }  // the filter line
int PickPopup::visible_rows() const { return std::max(1, (GetClientSize().y - header_h() - FromDIP(4)) / row_h()); }

int PickPopup::row_at(int y) const {
  y -= header_h();
  if (y < 0) return -1;
  const int r = top_ + y / row_h();
  return r < static_cast<int>(rows_.size()) ? r : -1;
}

void PickPopup::rebuild() {
  rows_.clear();
  const wxString f = filter_.Lower();
  if (f.empty()) {
    wxString group;
    bool first = true;
    for (size_t i = 0; i < items_.size(); ++i) {
      if (first || items_[i].group != group) {
        group = items_[i].group;
        if (!group.empty()) rows_.push_back({-1, group});
        first = false;
      }
      rows_.push_back({static_cast<int>(i), items_[i].group});
    }
  } else {
    // an exact value or alias first ("<" is lt, not neq through its "<>"), then a prefix, then
    // inside the value, then in the description (rank_pick_matches, tested on its own)
    std::vector<PickMatch> m;
    m.reserve(items_.size());
    for (const auto& it : items_)
      m.push_back({it.value.utf8_string(), it.alias.utf8_string(), it.desc.utf8_string()});
    for (const size_t i : rank_pick_matches(m, filter_.utf8_string()))
      rows_.push_back({static_cast<int>(i), items_[i].group});
  }
  sel_ = -1;
  hover_ = -1;
  for (size_t r = 0; r < rows_.size(); ++r) {
    if (rows_[r].item < 0) continue;
    if (sel_ < 0 && !no_preselect) sel_ = static_cast<int>(r);  // the best match
    if (f.empty() && items_[static_cast<size_t>(rows_[r].item)].value == current_) {
      sel_ = static_cast<int>(r);
      break;
    }
  }
  top_ = 0;
  ensure_visible();
  // Shown: as tall as what the filter left (a list of 10 cut to 1 by typing shrinks).
  if (IsShown() && !anchor_.IsEmpty()) show_below(anchor_);
  Refresh();
}

void PickPopup::ensure_visible() {
  if (sel_ < 0) return;
  const int vis = visible_rows();
  int want = sel_;
  if (want > 0 && rows_[static_cast<size_t>(want - 1)].item < 0) --want;  // keep its header in view
  if (want < top_) top_ = want;
  if (sel_ >= top_ + vis) top_ = sel_ - vis + 1;
  top_ = std::clamp(top_, 0, std::max(0, static_cast<int>(rows_.size()) - vis));
}

void PickPopup::show_below(const wxRect& cell) {
  if (closed_) return;  // closed (destroyed at the next idle time): never shown again
  anchor_ = cell;
  if (width_ < 0) {  // measured once per item list (a filter keystroke re-shows it)
    wxClientDC dc(this);
    dc.SetFont(GetFont());
    width_ = FromDIP(220);
    // room for "value  description · group" (the group is added while filtering)
    for (const auto& it : items_)
      width_ = std::max(width_, dc.GetTextExtent(it.value).x + dc.GetTextExtent(it.desc).x +
                                    (it.group.empty() ? 0 : dc.GetTextExtent(it.group).x + FromDIP(14)) + FromDIP(52));
  }
  const int w = std::max(cell.width, width_);
  const int rows = std::max(1, std::min<int>(static_cast<int>(rows_.size()), compact ? 10 : 16));
  const int h = header_h() + rows * row_h() + FromDIP(4);
  const int di = wxDisplay::GetFromPoint(cell.GetBottomLeft());
  const wxRect scr = wxDisplay(di == wxNOT_FOUND ? 0u : static_cast<unsigned>(di)).GetClientArea();
  const int x = std::max(scr.x, std::min(cell.x, scr.GetRight() - w));
  int y = cell.GetBottom() + 1;
  if (y + h > scr.GetBottom() && cell.y - h >= scr.y) y = cell.y - h;
  SetSize(x, y, w, h);
  ensure_visible();
  Show();
}

bool PickPopup::Show(bool show) {
#ifdef __WXMSW__
  if (show) {
    if (!wxWindowBase::Show(true)) return false;  // the shown flag; the window below
    ::SetWindowPos(static_cast<HWND>(GetHWND()), HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
  }
#endif
  return wxPopupWindow::Show(show);
}

#ifdef __WXMSW__
WXLRESULT PickPopup::MSWWindowProc(WXUINT msg, WXWPARAM wp, WXLPARAM lp) {
  if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
  return wxPopupWindow::MSWWindowProc(msg, wp, lp);
}
#endif

void PickPopup::set_items(std::vector<PickItem> items, const wxString& current) {
  items_ = std::move(items);
  current_ = current;
  width_ = -1;
  rebuild();
}

void PickPopup::reopen(const wxString& current) {
  closed_ = false;
  picked_with = 0;
  filter_.clear();
  current_ = current;
  anchor_ = {};
  rebuild();
}

wxString PickPopup::selected_value() const {
  if (sel_ < 0 || sel_ >= static_cast<int>(rows_.size()) || rows_[static_cast<size_t>(sel_)].item < 0) return {};
  return items_[static_cast<size_t>(rows_[static_cast<size_t>(sel_)].item)].value;
}

void PickPopup::set_filter(const wxString& f) {
  if (f == filter_) return;
  filter_ = f;
  rebuild();
}

void PickPopup::move(int delta) {
  if (rows_.empty()) return;
  const int n = static_cast<int>(rows_.size());
  int r = sel_ < 0 ? (delta > 0 ? -1 : n) : sel_;
  const int step = delta > 0 ? 1 : -1;
  for (int left = std::abs(delta); left > 0;) {
    int nr = r + step;
    while (nr >= 0 && nr < n && rows_[static_cast<size_t>(nr)].item < 0) nr += step;
    if (nr < 0 || nr >= n) break;
    r = nr;
    --left;
  }
  if (r >= 0 && r < n && rows_[static_cast<size_t>(r)].item >= 0) sel_ = r;
  ensure_visible();
  Refresh();
}

bool PickPopup::key(int code) {
  if (closed_) return false;
  switch (code) {
    case WXK_UP: case WXK_NUMPAD_UP: move(-1); return true;
    case WXK_DOWN: case WXK_NUMPAD_DOWN: move(1); return true;
    case WXK_PAGEUP: case WXK_NUMPAD_PAGEUP: move(-visible_rows()); return true;
    case WXK_PAGEDOWN: case WXK_NUMPAD_PAGEDOWN: move(visible_rows()); return true;
    case WXK_HOME: case WXK_NUMPAD_HOME: move(-1000); return true;
    case WXK_END: case WXK_NUMPAD_END: move(1000); return true;
    case WXK_RETURN: case WXK_NUMPAD_ENTER: case WXK_TAB:
      picked_with = code == WXK_NUMPAD_ENTER ? WXK_RETURN : code;
      pick_selected();
      return true;
    case WXK_ESCAPE: close(); return true;
    case WXK_BACK:
      if (!filter_.empty()) set_filter(filter_.Left(filter_.length() - 1));
      return true;
    default: return false;
  }
}

bool PickPopup::chr(wxChar c) {
  if (closed_) return false;
  if (c < 32 || c == 127) return false;
  set_filter(filter_ + c);
  return true;
}

void PickPopup::pick_selected() {
  if (closed_) return;
  wxString v;
  if (sel_ < 0 || sel_ >= static_cast<int>(rows_.size()) || rows_[static_cast<size_t>(sel_)].item < 0) {
    if (free_hint.empty() || filter_.empty()) {
      close();  // nothing matches: the old value stays
      return;
    }
    v = filter_;  // free text: the typed value itself
  } else {
    v = items_[static_cast<size_t>(rows_[static_cast<size_t>(sel_)].item)].value;
  }
  auto cb = on_pick;
  close();
  if (cb) cb(v);
}

void PickPopup::close() {
  if (closed_) return;
  closed_ = true;
  Hide();
  auto cb = std::move(on_close);
  on_close = nullptr;
  on_pick = nullptr;
  if (cb) cb();
  if (keep) return;  // hidden, ready for reopen()
  // Deleted at the next idle time, not here: a popup is not a wxTopLevelWindow, so Destroy()
  // would delete it at once, and close() can run inside one of its own calls (a pick, a resize
  // that moved the focus) whose caller still uses it.
  if (wxTheApp) wxTheApp->ScheduleForDestruction(this);
  else Destroy();
}

void PickPopup::paint() {
  wxAutoBufferedPaintDC dc(this);
  const Theme& t = Theme::get();
  const wxSize sz = GetClientSize();
  const wxColour bg = t.dark ? wxColour(0x25, 0x25, 0x26) : t.field;
  dc.SetBackground(wxBrush(bg));
  dc.Clear();
  dc.SetFont(GetFont());
  const int pad = FromDIP(10), rh = row_h(), hh = header_h();
  if (!compact) {  // the filter line
    const int ty = (hh - dc.GetCharHeight()) / 2;
    if (filter_.empty()) {
      dc.SetTextForeground(t.dim);
      dc.DrawText("Type to filter", pad, ty);
    } else {
      dc.SetTextForeground(t.fg);
      dc.DrawText(filter_, pad, ty);
      const int cx = pad + dc.GetTextExtent(filter_).x + FromDIP(1);
      dc.SetPen(wxPen(t.accent, FromDIP(1)));
      dc.DrawLine(cx, ty, cx, ty + dc.GetCharHeight());
      if (rows_.empty()) {
        const wxString note = free_hint.empty() ? wxString("no match") : free_hint;
        dc.SetTextForeground(free_hint.empty() ? t.var_bad : t.dim);
        dc.DrawText(note, sz.x - pad - dc.GetTextExtent(note).x, ty);
      }
    }
    dc.SetPen(wxPen(t.border));
    dc.DrawLine(0, hh - 1, sz.x, hh - 1);
  }
  const int vis = visible_rows();
  wxFont small = GetFont();
  small.SetPointSize(std::max(7, small.GetPointSize() - 1));
  small.MakeBold();
  for (int i = 0; i < vis && top_ + i < static_cast<int>(rows_.size()); ++i) {
    const int r = top_ + i;
    const Row& row = rows_[static_cast<size_t>(r)];
    const int y = hh + i * rh;
    const int ty = y + (rh - dc.GetCharHeight()) / 2;
    if (row.item < 0) {
      dc.SetFont(small);
      dc.SetTextForeground(t.dim);
      dc.DrawText(row.group.Upper(), pad, y + (rh - dc.GetCharHeight()) / 2 + FromDIP(1));
      dc.SetFont(GetFont());
      continue;
    }
    const PickItem& it = items_[static_cast<size_t>(row.item)];
    const bool sel = r == sel_;
    if (sel) {
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.SetBrush(wxBrush(t.accent));
      dc.DrawRoundedRectangle(FromDIP(4), y + FromDIP(1), sz.x - FromDIP(8), rh - FromDIP(2), FromDIP(3));
    } else if (r == hover_) {
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.SetBrush(wxBrush(t.dark ? wxColour(0x37, 0x37, 0x3A) : wxColour(0xE8, 0xEC, 0xF2)));
      dc.DrawRoundedRectangle(FromDIP(4), y + FromDIP(1), sz.x - FromDIP(8), rh - FromDIP(2), FromDIP(3));
    }
    const int ind = filter_.empty() && !row.group.empty() ? FromDIP(8) : 0;
    dc.SetTextForeground(sel ? t.accent_fg : t.fg);
    dc.DrawText(it.value, pad + ind, ty);
    const int x = pad + ind + dc.GetTextExtent(it.value).x + FromDIP(12);
    wxString d = it.desc;
    if (!filter_.empty() && !it.group.empty()) d = d.empty() ? it.group : d + wxString::FromUTF8(" \xC2\xB7 ") + it.group;
    if (!d.empty()) {
      dc.SetTextForeground(sel ? wxColour(0xDD, 0xE8, 0xFA) : t.dim);
      dc.DrawText(d, x, ty);
    }
    if (it.value == current_) {
      dc.SetTextForeground(sel ? t.accent_fg : t.accent);
      const wxString tick = wxString::FromUTF8("\xE2\x9C\x93");
      dc.DrawText(tick, sz.x - pad - dc.GetTextExtent(tick).x, ty);
    }
  }
  const int n = static_cast<int>(rows_.size());
  if (n > vis) {  // where the visible rows are in the list
    const int track = sz.y - hh - FromDIP(4);
    const int th = std::max(FromDIP(16), track * vis / n);
    const int y0 = hh + (track - th) * top_ / std::max(1, n - vis);
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(t.border));
    dc.DrawRoundedRectangle(sz.x - FromDIP(5), y0 + FromDIP(2), FromDIP(3), th, FromDIP(1));
  }
  dc.SetPen(wxPen(t.dark ? wxColour(0x55, 0x55, 0x55) : t.border));
  dc.SetBrush(*wxTRANSPARENT_BRUSH);
  dc.DrawRectangle(0, 0, sz.x, sz.y);
}

}  // namespace watt::app::http_editor
