#pragma once
// Small owner-drawn controls of the HTTP editor: a colour theme that follows the Windows light /
// dark setting, a flat button (Send, method picker, Copy as curl) and a Postman-style text tab
// strip with an accent underline. All painting is double buffered and cheap (a few rectangles
// and strings), so none of these cost anything per keystroke.
#include <wx/control.h>
#include <wx/colour.h>

#include <functional>
#include <string>
#include <vector>

namespace watt::app::http_editor {

struct Theme {
  bool dark = false;
  wxColour bg;          // page background
  wxColour field;       // input background (URL bar, tables)
  wxColour fg;          // text
  wxColour dim;         // secondary text, placeholders
  wxColour border;
  wxColour hover;
  wxColour accent;      // Send button, active tab underline
  wxColour accent_fg;
  wxColour var_ok;      // resolved {{var}}
  wxColour var_ok_bg;
  wxColour var_bad;     // unresolved {{var}}
  wxColour var_bad_bg;
  wxColour warn_bg;     // security prompt
  wxColour warn_border;
  wxColour info_bg;
  // JSON
  wxColour js_key, js_string, js_number, js_keyword, js_punct, js_error;
  static const Theme& get();
};

wxColour method_colour(const std::string& method);

class FlatButton : public wxControl {
 public:
  enum class Style { Primary, Subtle, Method };
  FlatButton(wxWindow* parent, const wxString& label, Style style, bool dropdown = false);
  void SetLabel(const wxString& label) override;
  void set_text_colour(const wxColour& c);
  std::function<void()> on_click;

 protected:
  wxSize DoGetBestClientSize() const override;
  bool AcceptsFocus() const override { return true; }

 private:
  void paint();
  void fire();
  wxString label_;
  Style style_;
  bool dropdown_;
  bool hover_ = false;
  bool down_ = false;
  wxColour text_;
};

// A themed dropdown (Options' HTTP version, Auth's "Add to"): a FlatButton with an arrow that
// lists the choices in a popup menu; Up / Down step through them without opening it.
class ChoiceButton : public FlatButton {
 public:
  ChoiceButton(wxWindow* parent, std::vector<wxString> items);
  int selection() const { return sel_; }
  void set_selection(int index);  // no on_select
  std::function<void(int)> on_select;  // a user pick only

 private:
  void pick(int index);
  std::vector<wxString> items_;
  int sel_ = 0;
};

// A themed check box: drawn box (accent fill and a tick when on) and label. Space toggles it, a
// click anywhere on it too. on_change runs on a user toggle only.
class CheckButton : public wxControl {
 public:
  CheckButton(wxWindow* parent, const wxString& label);
  bool value() const { return on_; }
  void set_value(bool on);
  std::function<void()> on_change;

 protected:
  wxSize DoGetBestClientSize() const override;
  bool AcceptsFocus() const override { return true; }

 private:
  void paint();
  void toggle();
  wxString label_;
  bool on_ = false;
  bool hover_ = false;
};

class TabStrip : public wxControl {
 public:
  explicit TabStrip(wxWindow* parent);
  void set_tabs(const std::vector<wxString>& labels);
  // A small count / marker after a label ("Headers 3"); empty for none.
  // warn: drawn in the warning colour (something on that page is not sent as shown).
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
    wxRect rect;
    bool warn = false;
  };
  void paint();
  void layout_tabs(wxDC& dc);
  std::vector<Tab> tabs_;
  size_t sel_ = 0;
  int hover_ = -1;
};

// A row of themed radio choices (the body modes): owner drawn, so the dark theme never shows a
// native light focus rectangle. Left / Right (or Up / Down) pick the neighbour, Home / End the
// ends; a click picks one. on_select runs on a user pick only.
class RadioStrip : public wxControl {
 public:
  RadioStrip(wxWindow* parent, const std::vector<wxString>& labels);
  void select(int index);  // no on_select; -1 for none
  int selection() const { return sel_; }
  std::function<void(int)> on_select;

 protected:
  wxSize DoGetBestClientSize() const override;
  bool AcceptsFocus() const override { return true; }

 private:
  void paint();
  void layout(wxDC& dc);
  void pick(int index);
  std::vector<wxString> labels_;
  std::vector<wxRect> rects_;
  int sel_ = 0;
  int hover_ = -1;
};

}  // namespace watt::app::http_editor
