#include "app/http_editor_widgets.h"

#include <wx/dcbuffer.h>
#include <wx/menu.h>
#include <wx/settings.h>

#include <algorithm>

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
      fill = hover_ ? t.hover : parent_bg;
      fg = text_.IsOk() ? text_ : t.fg;
      break;
  }
  if (!IsEnabled()) fg = t.dim;
  dc.SetPen(HasFocus() && style_ != Style::Primary ? wxPen(t.accent, FromDIP(1)) : *wxTRANSPARENT_PEN);
  if (HasFocus() && style_ == Style::Primary) dc.SetPen(wxPen(t.accent_fg, FromDIP(1), wxPENSTYLE_DOT));
  dc.SetBrush(wxBrush(fill));
  dc.DrawRoundedRectangle(0, 0, sz.x, sz.y, FromDIP(4));
  dc.SetFont(GetFont());
  dc.SetTextForeground(fg);
  const wxSize ts = dc.GetTextExtent(label_);
  int x = style_ == Style::Method ? FromDIP(12) : (sz.x - ts.x - (dropdown_ ? FromDIP(14) : 0)) / 2;
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
    wxMenu menu;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i)
      menu.AppendRadioItem(i + 1, items_[static_cast<size_t>(i)])->Check(i == sel_);
    const int s = GetPopupMenuSelectionFromUser(menu, wxPoint(0, GetSize().y));
    if (s >= 1 && s <= static_cast<int>(items_.size())) pick(s - 1);
  };
  Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
    const int k = e.GetKeyCode();
    const int n = static_cast<int>(items_.size());
    const bool up = k == WXK_UP || k == WXK_NUMPAD_UP, down = k == WXK_DOWN || k == WXK_NUMPAD_DOWN;
    if ((up || down) && !e.HasAnyModifiers() && n > 0) {
      pick(std::clamp(sel_ + (down ? 1 : -1), 0, n - 1));
      return;
    }
    e.Skip();  // FlatButton's own handler: Space / Enter open the list, Tab moves on
  });
}

void ChoiceButton::set_selection(int index) {
  if (index < 0 || index >= static_cast<int>(items_.size())) return;
  sel_ = index;
  SetLabel(items_[static_cast<size_t>(index)]);
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
  Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
  Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
    int h = -1;
    for (size_t i = 0; i < tabs_.size(); ++i)
      if (tabs_[i].rect.Contains(e.GetPosition())) h = static_cast<int>(i);
    if (h != hover_) {
      hover_ = h;
      SetCursor(h >= 0 ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
      Refresh();
    }
  });
  Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
    hover_ = -1;
    Refresh();
  });
  Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
    for (size_t i = 0; i < tabs_.size(); ++i)
      if (tabs_[i].rect.Contains(e.GetPosition())) {
        select(i);
        if (on_select) on_select(i);
      }
  });
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
  const int h = GetClientSize().y;
  const wxFont bold = GetFont().Bold();
  for (auto& t : tabs_) {
    dc.SetFont(bold);  // width of the bold label, so selecting a tab does not shift the others
    int w = dc.GetTextExtent(t.label).x;
    if (!t.badge.empty()) {
      dc.SetFont(GetFont().Smaller());
      w += FromDIP(5) + dc.GetTextExtent(t.badge).x;
    }
    t.rect = wxRect(x, 0, w + FromDIP(20), h);
    x += t.rect.width;
  }
}

void TabStrip::paint() {
  wxAutoBufferedPaintDC dc(this);
  const Theme& th = Theme::get();
  dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
  dc.Clear();
  layout_tabs(dc);
  const wxSize sz = GetClientSize();
  for (size_t i = 0; i < tabs_.size(); ++i) {
    const auto& t = tabs_[i];
    const bool sel = i == sel_;
    dc.SetFont(sel ? GetFont().Bold() : GetFont());
    dc.SetTextForeground(sel || static_cast<int>(i) == hover_ ? th.fg : th.dim);
    const int ty = (sz.y - dc.GetCharHeight()) / 2;
    int x = t.rect.x + FromDIP(10);
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
      dc.DrawRectangle(t.rect.x + FromDIP(8), sz.y - FromDIP(2), t.rect.width - FromDIP(16), FromDIP(2));
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

}  // namespace watt::app::http_editor
