#pragma once
// Key / value table of the HTTP editor (query params, headers, form-urlencoded and multipart
// bodies): an enable checkbox per row, an always-present empty row at the bottom to type into,
// single-click editing, {{var}} highlighting with the resolved value as a tooltip, and
// autocompletion of common header names / values. Edits are reported live (while typing), so
// the URL follows the params table keystroke by keystroke.
#include <wx/grid.h>
#include <wx/panel.h>

class wxTextCtrl;

class wxSimplebook;
class wxStaticText;
class wxStyledTextCtrl;

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/http_editor_util.h"
#include "app/http_editor_widgets.h"

namespace watt::app::http_editor {

// Resolved value of a {{name}}, or nullopt when it is not defined.
using VarLookup = std::function<std::optional<std::string>(const std::string& name)>;

struct KvRow {
  std::string key;
  std::string value;
  bool enabled = true;
  bool file = false;  // multipart: value is a file path
  bool bare = false;  // query: written without '='
  std::string desc;   // Params / Headers / Path: a note kept in the request file, never sent
  std::string type;   // Assert: the operator ("eq"); Capture: the scope ("runtime")
};

// Operators of core/asserts.h in the order the Assert table offers them, and whether one takes
// no value (isEmpty, isNumber, ...).
const std::vector<std::string>& assert_ops();
bool assert_op_unary(std::string_view op);

// Header name as sent, in the canonical case of a well-known header ("x-request-id" ->
// "X-Request-ID"), without control characters or blanks.
std::string canonical_header_name(std::string_view name);


class KvGrid : public wxGrid {
 public:
  // Path: path variables, one row per ":name" of the URL (set by code), only the value editable,
  // no blank row, no checkbox and no delete column.
  // Assert: Target | Operator | Expected (core/asserts.h); Capture: Variable | From | Scope.
  // PreVar: Variable | Expression | Scope | Value (http_editor_pre.h; the value column is
  // read-only and drawn from preview()).
  // Params, Headers and Path have a Description column (saved, never sent).
  enum class Kind { Params, Headers, UrlEncoded, Multipart, Path, Assert, Capture, PreVar };
  KvGrid(wxWindow* parent, Kind kind);

  std::function<void()> on_change;  // any user edit, live while typing
  std::function<void()> on_rows_set;  // after set_rows (a change made by code)
  // After any change: how many rows have a value but no key (they are never sent).
  std::function<void(size_t orphans)> on_orphans;
  VarLookup lookup;
  // Problem with a cell's text ("" when fine), drawn in the warning colour with the message as
  // its tooltip (Assert / Capture targets and variable names).
  std::function<std::string(int col, const std::string& text)> check;
  // {{var}} names offered when "{{" is typed in a cell (read on the UI thread when an edit starts).
  std::function<std::vector<std::string>()> var_names;
  // PreVar: the value a row's variable would get now (ok == false: the problem), for the Value
  // column; nullopt while not computed.
  struct Preview {
    bool ok = false;
    std::string text;
  };
  std::function<std::optional<Preview>(const std::string& var)> preview;

  void set_rows(const std::vector<KvRow>& rows);  // does not call on_change
  std::vector<KvRow> rows() const;               // filled rows only, with the live editor text
  size_t enabled_count() const;  // rows that are sent (enabled, with a key)
  size_t orphan_count() const;   // rows with a value but no key
  // Multipart: pick a file for a row (the Value cell of a File row also opens the picker).
  void choose_file(int row);

  Kind kind() const { return kind_; }
  int col_key() const { return key_c_; }
  int col_type() const { return type_c_; }
  int col_value() const { return value_c_; }
  int col_desc() const { return desc_c_; }
  int col_del() const { return del_c_; }

 protected:
  void DrawColLabel(wxDC& dc, int col) override;

 private:
  friend class KvRenderer;
  std::string default_type() const;
  std::string cell(int row, int col) const;  // live editor text when that cell is being edited
  bool row_empty(int row) const;
  bool row_enabled(int row) const { return GetCellValue(row, 0) != "0"; }
  bool row_orphan(int row) const;  // a value (or a file) but no key: not sent
  void ensure_blank_row();
  void layout_columns();
  void changed();
  void update_tooltip(int row, int col);
  // Keyboard path: Key -> Value -> next row's Key (Tab also stops at Type in multipart), with
  // the editor opened; Shift goes back. The checkbox and delete columns are not on it.
  void nav_move(bool forward, bool enter);
  void go(int row, int col, bool edit);
  void on_key(wxKeyEvent& e);
  void delete_row(int row);
  void report_orphans();
  // The Operator / Scope / Type cell: a themed, filterable pick list (http_editor_widgets.h)
  // instead of a native combo box; first: a typed character that starts the filter.
  void open_picker(int row, wxChar first = 0);
  void close_picker();
  // Completion of the cell being edited, in a themed PickPopup that never takes the focus: "{{"
  // lists the {{var}} names, otherwise the column's fixed choices (header names / values). It
  // only changes the text on Tab, Enter or a click: nothing is appended while typing.
  struct Suggest {
    std::vector<PickItem> items;
    long from = 0, to = 0;  // the text replaced by a pick
    bool var = false;       // a pick is written as {{name}}
  };
  Suggest suggestions(int col, const wxString& text, long caret) const;
  void update_suggest(wxTextCtrl* tc);
  bool suggest_key(int key, bool shift);  // a key while the list is open: true when it used it
  bool accept_suggest(wxTextCtrl* tc);  // false: nothing to change (the key does its own thing)
  void insert_suggest(wxTextCtrl* tc, const wxString& value);
  void close_suggest();

  Kind kind_;
  PickPopup* pick_ = nullptr;
  int pick_row_ = -1;
  int key_c_ = 1, type_c_ = -1, value_c_ = 2, desc_c_ = -1, del_c_ = 3;
  PickPopup* suggest_ = nullptr;
  wxTextCtrl* suggest_tc_ = nullptr;  // the editor it completes
  bool inserting_ = false;
  wxArrayString key_choices_, value_choices_;
  wxArrayString var_list_;  // {{var}} names, read when an edit starts
  // The cell being edited and its current text (wxGrid only commits on Enter / focus loss).
  int live_row_ = -1, live_col_ = -1;
  wxString live_text_;
  int tip_row_ = -2, tip_col_ = -2;
  bool updating_ = false;
  size_t orphans_ = 0;
};

class FlatButton;

// A titled KvGrid with a "Bulk Edit" toggle (Postman / Insomnia / Bruno): the rows as editable
// "key: value" lines ("//" disables a row), so a block of headers copied from docs or devtools
// can be pasted in one go. Edits in either view are live.
class KvPanel : public wxPanel {
 public:
  KvPanel(wxWindow* parent, KvGrid::Kind kind, const wxString& title);
  KvGrid* grid() const { return grid_; }
  // Table height: as tall as its rows (at most max_px) instead of filling the panel; -1 fills.
  void fit_rows(int max_px);
  bool bulk() const { return bulk_; }
  void set_bulk(bool on);
  // Extra controls on the title row, left of the toggle.
  wxSizer* title_row() const { return title_row_; }
  // A window shown between the title row and the table (a child of this panel).
  void add_above(wxWindow* w);

 private:
  void text_changed();
  void refresh_text();
  std::string bulk_text() const;
  void dim_disabled_lines();
  KvGrid* grid_ = nullptr;
  wxStyledTextCtrl* text_ = nullptr;
  bool setting_text_ = false;
  wxSimplebook* book_ = nullptr;
  FlatButton* toggle_ = nullptr;
  wxSizer* title_row_ = nullptr;
  wxStaticText* hint_ = nullptr;  // "a row has a value but no key"
  bool bulk_ = false;
  bool from_text_ = false;
};

}  // namespace watt::app::http_editor
