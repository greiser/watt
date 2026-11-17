#include "app/grpc_editor_widgets.h"

#include <wx/dcbuffer.h>
#include <wx/menu.h>

#include <algorithm>

namespace watt::app::grpced {

namespace {
wxColour hover_colour(const Theme& t) { return t.dark ? wxColour(55, 55, 58) : wxColour(232, 232, 235); }
}  // namespace

// ------------------------------------------------------------------ FlatButton

FlatButton::FlatButton(wxWindow* parent, const wxString& label, Style style, bool dropdown)
    : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxWANTS_CHARS),
      label_(label), style_(style), dropdown_(dropdown) {
  SetBackgroundStyle(wxBG_STYLE_PAINT);
  if (style_ == Style::Primary) SetFont(GetFont().Bold());
  Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
  Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) {
    hover_ = true;
    Refresh();
  });
  Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
    hover_ = down_ = false;
    Refresh();
  });
  Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
    if (!IsEnabled()) return;
    down_ = true;
    Refresh();
  });
  Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
    const bool was = down_;
    down_ = false;
    Refresh();
    if (was && hover_) fire();
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
  Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) {
    e.Skip();
    Refresh();
  });
  Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) {
    e.Skip();
    Refresh();
  });
}

void FlatButton::SetLabel(const wxString& label) {
  if (label == label_) return;
  label_ = label;
  InvalidateBestSize();
  Refresh();
}

void FlatButton::set_checked(bool on) {
  if (on == checked_) return;
  checked_ = on;
  Refresh();
}

void FlatButton::set_text_colour(const wxColour& c) {
  text_ = c;
  Refresh();
}

void FlatButton::set_width_labels(const std::vector<wxString>& labels) {
  width_labels_ = labels;
  InvalidateBestSize();
}

bool FlatButton::Enable(bool enable) {
  const bool r = wxControl::Enable(enable);
  Refresh();
  return r;
}

void FlatButton::fire() {
  if (IsEnabled() && on_click) on_click();
}

wxSize FlatButton::DoGetBestClientSize() const {
  wxClientDC dc(const_cast<FlatButton*>(this));
  dc.SetFont(GetFont());
  wxSize s = dc.GetTextExtent(label_);
  if (sidebar_glyph_) s.x = FromDIP(16);
  for (const auto& l : width_labels_) s.x = std::max(s.x, dc.GetTextExtent(l).x);
  int w = s.x + FromDIP(style_ == Style::Primary ? 28 : 16);
  if (dropdown_) w += FromDIP(14);
  return wxSize(w, s.y + FromDIP(style_ == Style::Primary ? 14 : 10));
}

void FlatButton::paint() {
  wxAutoBufferedPaintDC dc(this);
  const Theme t = theme();
  const wxSize sz = GetClientSize();
  const wxColour parent_bg = GetParent()->GetBackgroundColour();
  dc.SetBackground(wxBrush(parent_bg));
  dc.Clear();
  wxColour fill = parent_bg, fg = text_.IsOk() ? text_ : t.fg;
  wxPen pen = *wxTRANSPARENT_PEN;
  const bool on = IsEnabled();
  switch (style_) {
    case Style::Primary:
      fill = t.accent;
      if (on && hover_) fill = fill.ChangeLightness(down_ ? 90 : 112);
      fg = t.accent_fg;
      break;
    case Style::Subtle:
      if (on && hover_) fill = hover_colour(t);
      break;
    case Style::Outline:
      pen = wxPen(t.border, FromDIP(1));
      if (checked_) {
        fill = t.dark ? wxColour(20, 60, 100) : wxColour(220, 234, 250);
        pen = wxPen(t.accent, FromDIP(1));
      } else if (on && hover_) {
        fill = hover_colour(t);
      }
      break;
  }
  if (!on) fg = t.muted;
  if (HasFocus()) pen = style_ == Style::Primary ? wxPen(t.accent_fg, FromDIP(1), wxPENSTYLE_DOT) : wxPen(t.accent, FromDIP(1));
  dc.SetPen(pen);
  dc.SetBrush(wxBrush(fill));
  dc.DrawRoundedRectangle(0, 0, sz.x, sz.y, FromDIP(4));
  if (sidebar_glyph_) {
    const int gw = FromDIP(16), gh = FromDIP(12);
    const wxRect g((sz.x - gw) / 2, (sz.y - gh) / 2, gw, gh);
    dc.SetPen(wxPen(fg, FromDIP(1)));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRoundedRectangle(g, FromDIP(2));
    dc.SetBrush(wxBrush(checked_ ? fg : fill));
    dc.DrawRectangle(g.x, g.y, FromDIP(6), gh);
    return;
  }
  dc.SetFont(GetFont());
  dc.SetTextForeground(fg);
  const wxSize ts = dc.GetTextExtent(label_);
  const int x = std::max(FromDIP(4), (sz.x - ts.x - (dropdown_ ? FromDIP(14) : 0)) / 2);
  dc.SetClippingRegion(0, 0, sz.x - (dropdown_ ? FromDIP(14) : 0), sz.y);
  dc.DrawText(label_, x, (sz.y - ts.y) / 2);
  dc.DestroyClippingRegion();
  if (dropdown_) {
    const int a = FromDIP(4);
    const int ax = sz.x - FromDIP(16), ay = sz.y / 2 - a / 2;
    wxPoint pts[3] = {{ax, ay}, {ax + 2 * a, ay}, {ax + a, ay + a}};
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(style_ == Style::Primary ? fg : t.muted));
    dc.DrawPolygon(3, pts);
  }
}

// ------------------------------------------------------------------ CheckBox

CheckBox::CheckBox(wxWindow* parent, const wxString& label)
    : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxWANTS_CHARS), label_(label) {
  SetBackgroundStyle(wxBG_STYLE_PAINT);
  Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
  Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) {
    hover_ = true;
    Refresh();
  });
  Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
    hover_ = false;
    Refresh();
  });
  Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
    if (IsEnabled()) SetFocus();
  });
  Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
    if (hover_) toggle();
  });
  Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent&) { toggle(); });
  Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
    const int k = e.GetKeyCode();
    if (k == WXK_SPACE && !e.HasAnyModifiers()) {
      toggle();
    } else if (k == WXK_TAB) {
      Navigate(e.ShiftDown() ? wxNavigationKeyEvent::IsBackward : wxNavigationKeyEvent::IsForward);
    } else {
      e.Skip();
    }
  });
  Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) {
    e.Skip();
    Refresh();
  });
  Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) {
    e.Skip();
    Refresh();
  });
}

void CheckBox::SetValue(bool on) {
  if (on == value_) return;
  value_ = on;
  Refresh();
}

void CheckBox::SetLabel(const wxString& label) {
  if (label == label_) return;
  label_ = label;
  InvalidateBestSize();
  Refresh();
}

void CheckBox::toggle() {
  if (!IsEnabled()) return;
  value_ = !value_;
  Refresh();
  if (on_change) on_change();
}

wxSize CheckBox::DoGetBestClientSize() const {
  wxClientDC dc(const_cast<CheckBox*>(this));
  dc.SetFont(GetFont());
  const wxSize s = dc.GetTextExtent(label_);
  return wxSize(FromDIP(20) + s.x + FromDIP(4), std::max(s.y, FromDIP(14)) + FromDIP(6));
}

void CheckBox::paint() {
  wxAutoBufferedPaintDC dc(this);
  const Theme t = theme();
  const wxSize sz = GetClientSize();
  dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
  dc.Clear();
  const int b = FromDIP(14);
  const wxRect box(FromDIP(1), (sz.y - b) / 2, b, b);
  const bool on = IsEnabled();
  wxColour edge = HasFocus() || (hover_ && on) ? t.accent : t.border;
  if (t.dark && edge == t.border) edge = wxColour(110, 110, 115);
  dc.SetPen(wxPen(edge, FromDIP(1)));
  dc.SetBrush(wxBrush(value_ ? (on ? t.accent : t.muted) : t.bg));
  dc.DrawRoundedRectangle(box, FromDIP(3));
  if (value_) {
    dc.SetPen(wxPen(t.accent_fg, FromDIP(2)));
    const wxPoint p1(box.x + b * 3 / 14, box.y + b * 7 / 14), p2(box.x + b * 6 / 14, box.y + b * 10 / 14),
        p3(box.x + b * 11 / 14, box.y + b * 4 / 14);
    dc.DrawLine(p1, p2);
    dc.DrawLine(p2, p3);
  }
  dc.SetFont(GetFont());
  dc.SetTextForeground(on ? t.fg : t.muted);
  const wxSize ts = dc.GetTextExtent(label_);
  dc.DrawText(label_, FromDIP(20), (sz.y - ts.y) / 2);
}

// ------------------------------------------------------------------ ChoiceButton

ChoiceButton::ChoiceButton(wxWindow* parent, const std::vector<wxString>& items)
    : FlatButton(parent, items.empty() ? wxString() : items.front(), Style::Outline, true), items_(items) {
  set_width_labels(items_);
  on_click = [this] {
    wxMenu menu;
    for (size_t i = 0; i < items_.size(); ++i)
      menu.AppendCheckItem(static_cast<int>(1 + i), items_[i])->Check(static_cast<int>(i) == sel_);
    const int id = GetPopupMenuSelectionFromUser(menu, wxPoint(0, GetSize().y));
    if (id >= 1 && static_cast<size_t>(id) <= items_.size() && id - 1 != sel_) {
      SetSelection(id - 1);
      if (on_change) on_change();
    }
  };
}

void ChoiceButton::SetSelection(int i) {
  if (i < 0 || static_cast<size_t>(i) >= items_.size()) return;
  sel_ = i;
  SetLabel(items_[static_cast<size_t>(i)]);
}

// ------------------------------------------------------------------ TabStrip

TabStrip::TabStrip(wxWindow* parent) : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE) {
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
  for (const auto& l : labels) tabs_.push_back({l, {}, false, {}});
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
  return wxSize(FromDIP(200), dc.GetCharHeight() + FromDIP(14));
}

void TabStrip::layout_tabs(wxDC& dc) {
  int x = 0;
  const int h = GetClientSize().y;
  const wxFont bold = GetFont().Bold();
  for (auto& t : tabs_) {
    dc.SetFont(bold);
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
  const Theme th = theme();
  dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
  dc.Clear();
  layout_tabs(dc);
  const wxSize sz = GetClientSize();
  dc.SetPen(wxPen(th.border, 1));
  dc.DrawLine(0, sz.y - 1, sz.x, sz.y - 1);
  for (size_t i = 0; i < tabs_.size(); ++i) {
    const auto& t = tabs_[i];
    const bool sel = i == sel_;
    dc.SetFont(sel ? GetFont().Bold() : GetFont());
    dc.SetTextForeground(sel || static_cast<int>(i) == hover_ ? th.fg : th.muted);
    const int ty = (sz.y - dc.GetCharHeight()) / 2;
    int x = t.rect.x + FromDIP(10);
    dc.DrawText(t.label, x, ty);
    x += dc.GetTextExtent(t.label).x + FromDIP(5);
    if (!t.badge.empty()) {
      dc.SetFont(GetFont().Smaller());
      dc.SetTextForeground(t.warn ? (th.dark ? wxColour(230, 180, 80) : wxColour(170, 110, 0)) : th.ok);
      dc.DrawText(t.badge, x, ty + FromDIP(1));
    }
    if (sel) {
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.SetBrush(wxBrush(th.accent));
      dc.DrawRectangle(t.rect.x + FromDIP(8), sz.y - FromDIP(2), t.rect.width - FromDIP(16), FromDIP(2));
    }
  }
}

}  // namespace watt::app::grpced
