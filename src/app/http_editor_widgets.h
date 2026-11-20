#pragma once
// Small owner-drawn controls of the HTTP editor: a color theme that follows the Windows light /
// dark setting, a flat button (Send, method picker, Copy as curl) and a Postman-style text tab
// strip with an accent underline. All painting is double buffered and cheap (a few rectangles
// and strings), so none of these cost anything per keystroke.
#include <wx/control.h>
#include <wx/colour.h>
#include <wx/popupwin.h>

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
  // Outline: a secondary action that must read as a button (a thin border around it).
  enum class Style { Primary, Subtle, Method, Outline };
  FlatButton(wxWindow* parent, const wxString& label, Style style, bool dropdown = false);
  void SetLabel(const wxString& label) override;
  void set_text_colour(const wxColour& c);
  // Drawn like a text field (its fill and border, the label left-aligned): a dropdown in a
  // settings column of fields lines up with them.
  void set_field_look(bool on);
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
  bool field_look_ = false;
  bool hover_ = false;
  bool down_ = false;
  wxColour text_;
};

class PickPopup;

// A themed dropdown (Options' HTTP version, Auth's "Add to", the folder dialog's auth type): a
// FlatButton with an arrow that lists the choices in a themed PickPopup (typing filters it,
// Enter / a click picks, Esc closes); Up / Down step through them without opening it.
class ChoiceButton : public FlatButton {
 public:
  ChoiceButton(wxWindow* parent, std::vector<wxString> items);
  int selection() const { return sel_; }
  void set_selection(int index);  // no on_select
  // Relabels one choice (e.g. "Inherit (on, from api)"); grows the button when it is wider.
  void set_item(int index, const wxString& text);
  std::function<void(int)> on_select;  // a user pick only

 private:
  void pick(int index);
  void close_list();
  std::vector<wxString> items_;
  int sel_ = 0;
  PickPopup* list_ = nullptr;
};

// A themed check box: drawn box (accent fill and a tick when on) and label. Space toggles it, a
// click anywhere on it too. on_change runs on a user toggle only.
class CheckButton : public wxControl {
 public:
  CheckButton(wxWindow* parent, const wxString& label);
  bool value() const { return on_; }
  void set_value(bool on);
  void set_label(const wxString& label);
  const wxString& label() const { return label_; }
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
  // warn: drawn in the warning color (something on that page is not sent as shown).
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
  void more_menu();
  std::vector<Tab> tabs_;
  size_t sel_ = 0;
  int hover_ = -1;  // -2: the overflow button
  int pad_ = 0;  // space around each label (less when the strip is narrow)
  // The overflow button: the tabs that do not fit (never the selected one) are listed in its
  // menu instead of being cut at the strip's edge. Empty when every tab fits.
  wxRect more_rect_;
};

// A row of themed radio choices (the body modes): owner drawn, so the dark theme never shows a
// native light focus rectangle. Left / Right (or Up / Down) pick the neighbor, Home / End the
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


// A themed, filterable pick list (the Assert table's Operator, the Scope / Type cells): a popup
// that never takes the focus. Its owner keeps the focus and forwards keys with key() / chr():
// typing filters (prefix matches first), Up / Down / PgUp / PgDn / Home / End move, Enter or a
// click picks, Esc closes. Items are grouped under their group name while no filter is typed.
struct PickItem {
  wxString value;  // what is picked ("contains")
  wxString desc;   // drawn dim after it ("substring / element / key")
  wxString group;  // header ("String"); "" for none
  wxString alias;  // other spellings, space separated, matched like the value ("== ===" for eq)
};

class PickPopup : public wxPopupWindow {
 public:
  PickPopup(wxWindow* owner, std::vector<PickItem> items, const wxString& current);
  // Below (or, without room, above) the screen rectangle `cell`, at least as wide as it.
  void show_below(const wxRect& cell);
  void set_filter(const wxString& f);
  const wxString& filter() const { return filter_; }
  // A key from the owner: true when the list used it (Enter / Esc close it).
  bool key(int code);
  bool chr(wxChar c);  // a typed character: added to the filter
  void pick_selected();
  std::function<void(const wxString&)> on_pick;  // then closed
  std::function<void()> on_close;                // closed, picked or not
  // The key that picked (WXK_TAB / WXK_RETURN; 0: a click), readable in on_pick: a grid moves on
  // to the next cell after a Tab pick and stays after Enter.
  int picked_with = 0;
  // Non-empty: Enter with a filter that matches nothing picks the typed text itself, and this
  // hint replaces "no match" (the method list: "Enter: custom method").
  wxString free_hint;
  // Shown without activation (wxPopupWindow::Show makes it the foreground window, which takes
  // the focus from the owner: a cell editor would lose the keys typed after the list opened).
  bool Show(bool show = true) override;
#ifdef __WXMSW__
  WXLRESULT MSWWindowProc(WXUINT msg, WXWPARAM wp, WXLPARAM lp) override;  // a click does not activate it either
#endif
  // Compact: no filter line (a completion list whose filter is the text typed in the owner).
  bool compact = false;
  // Nothing highlighted until Up / Down (a completion list: Enter / Tab then keep what was typed
  // instead of taking the first entry). The mouse only hovers; a click picks.
  bool no_preselect = false;
  // New items (the selection goes to the first match / `current`); the size follows on the
  // next show_below.
  void set_items(std::vector<PickItem> items, const wxString& current);
  // The highlighted item's value, "" when none.
  wxString selected_value() const;
  void close();
  // Kept for reuse: close() only hides it (callbacks dropped), reopen() arms it again. Building
  // the window and measuring its items again on every open is what made opening it slow.
  bool keep = false;
  void reopen(const wxString& current);
  bool is_closed() const { return closed_; }

 private:
  struct Row {
    int item = -1;  // -1: a group header
    wxString group;
  };
  void rebuild();
  void paint();
  void ensure_visible();
  int row_h() const;
  int header_h() const;
  int visible_rows() const;
  int row_at(int y) const;
  void move(int delta);
  std::vector<PickItem> items_;
  std::vector<Row> rows_;
  wxString filter_, current_;
  int sel_ = -1;  // index into rows_
  int hover_ = -1;  // the row under the mouse (no_preselect)
  int top_ = 0;   // first row drawn
  bool closed_ = false;
  wxRect anchor_;  // the screen rectangle it was shown under
  int width_ = -1;  // the widest item's row width (show_below), -1: measure again
};

}  // namespace watt::app::http_editor
