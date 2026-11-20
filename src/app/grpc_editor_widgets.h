#pragma once
// Owner-drawn controls of the gRPC editor, so the dark theme has no light native chrome: a flat
// button (primary / subtle / toggle) and a text tab strip with an accent underline that matches
// the HTTP editor's tabs. Painting is a few rectangles and strings, double buffered.
#include <wx/control.h>

#include <functional>
#include <vector>

#include "app/grpc_editor_util.h"

namespace watt::app::grpced {

class FlatButton : public wxControl {
 public:
  enum class Style { Primary, Subtle, Outline };
  FlatButton(wxWindow* parent, const wxString& label, Style style = Style::Subtle, bool dropdown = false);
  void SetLabel(const wxString& label) override;
  wxString GetLabel() const override { return label_; }
  // Toggle look (Outline style drawn filled with the accent when on).
  void set_checked(bool on);
  bool checked() const { return checked_; }
  void set_text_colour(const wxColour& c);
  // Width never shrinks below the widest of these labels (Send <-> Cancel).
  void set_width_labels(const std::vector<wxString>& labels);
  bool Enable(bool enable = true) override;
  // Draws a "sidebar" pictogram instead of the label (the label stays the accessible name).
  void set_sidebar_glyph(bool on) { sidebar_glyph_ = on; }
  std::function<void()> on_click;

 protected:
  wxSize DoGetBestClientSize() const override;
  bool AcceptsFocus() const override { return IsEnabled(); }

 private:
  void paint();
  void fire();
  wxString label_;
  Style style_;
  bool dropdown_;
  bool hover_ = false;
  bool down_ = false;
  bool checked_ = false;
  bool sidebar_glyph_ = false;
  wxColour text_;
  std::vector<wxString> width_labels_;
};

// A check box drawn in the theme's colors (the native one stays light on the dark theme).
class CheckBox : public wxControl {
 public:
  CheckBox(wxWindow* parent, const wxString& label);
  bool GetValue() const { return value_; }
  void SetValue(bool on);  // no on_change
  void SetLabel(const wxString& label) override;
  std::function<void()> on_change;  // user toggled it

 protected:
  wxSize DoGetBestClientSize() const override;
  bool AcceptsFocus() const override { return IsEnabled(); }

 private:
  void paint();
  void toggle();
  wxString label_;
  bool value_ = false;
  bool hover_ = false;
};

// A drop-down choice drawn as a flat button that opens a menu (wxChoice draws a light combo).
class ChoiceButton : public FlatButton {
 public:
  ChoiceButton(wxWindow* parent, const std::vector<wxString>& items);
  int GetSelection() const { return sel_; }
  void SetSelection(int i);  // no on_change
  std::function<void()> on_change;  // user picked another item

 private:
  std::vector<wxString> items_;
  int sel_ = 0;
};

class TabStrip : public wxControl {
 public:
  explicit TabStrip(wxWindow* parent);
  void set_tabs(const std::vector<wxString>& labels);
  // A small marker after a label ("Metadata 2", "Options 0.5 s"); empty for none.
  void set_badge(size_t index, const wxString& badge, bool warn = false);
  void select(size_t index);
  size_t selection() const { return sel_; }
  std::function<void(size_t)> on_select;

 protected:
  wxSize DoGetBestClientSize() const override;

 private:
  struct Tab {
    wxString label;
    wxString badge;
    bool warn = false;
    wxRect rect;
  };
  void paint();
  void layout_tabs(wxDC& dc);
  void more_menu();
  std::vector<Tab> tabs_;
  size_t sel_ = 0;
  int hover_ = -1;  // -2: the overflow button
  // The tabs that do not fit (never the selected one) are listed under this "»" button.
  wxRect more_rect_;
};

}  // namespace watt::app::grpced
