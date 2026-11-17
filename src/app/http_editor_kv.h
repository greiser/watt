#pragma once
// Key / value table of the HTTP editor (query params, headers, form-urlencoded and multipart
// bodies): an enable checkbox per row, an always-present empty row at the bottom to type into,
// single-click editing, {{var}} highlighting with the resolved value as a tooltip, and
// autocompletion of common header names / values. Edits are reported live (while typing), so
// the URL follows the params table keystroke by keystroke.
#include <wx/grid.h>
#include <wx/panel.h>

class wxSimplebook;
class wxStaticText;
class wxTextCtrl;

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/http_editor_util.h"

namespace watt::app::http_editor {

// Resolved value of a {{name}}, or nullopt when it is not defined.
using VarLookup = std::function<std::optional<std::string>(const std::string& name)>;

struct KvRow {
  std::string key;
  std::string value;
  bool enabled = true;
  bool file = false;  // multipart: value is a file path
  bool bare = false;  // query: written without '='
};

// Header name as sent, in the canonical case of a well-known header ("x-request-id" ->
// "X-Request-ID"), without control characters or blanks.
std::string canonical_header_name(std::string_view name);

class KvGrid : public wxGrid {
 public:
  // Path: path variables, one row per ":name" of the URL (set by code), only the value editable,
  // no blank row, no checkbox and no delete column.
  enum class Kind { Params, Headers, UrlEncoded, Multipart, Path };
  KvGrid(wxWindow* parent, Kind kind);

  std::function<void()> on_change;  // any user edit, live while typing
  std::function<void()> on_rows_set;  // after set_rows (a change made by code)
  // After any change: how many rows have a value but no key (they are never sent).
  std::function<void(size_t orphans)> on_orphans;
  VarLookup lookup;
  // {{var}} names offered when "{{" is typed in a cell (read on the UI thread when an edit starts).
  std::function<std::vector<std::string>()> var_names;

  void set_rows(const std::vector<KvRow>& rows);  // does not call on_change
  std::vector<KvRow> rows() const;               // filled rows only, with the live editor text
  size_t enabled_count() const;  // rows that are sent (enabled, with a key)
  size_t orphan_count() const;   // rows with a value but no key
  // Multipart: pick a file for a row (the Value cell of a File row also opens the picker).
  void choose_file(int row);

  Kind kind() const { return kind_; }

 protected:
  void DrawColLabel(wxDC& dc, int col) override;

 private:
  friend class KvRenderer;
  int col_key() const { return 1; }
  int col_type() const { return kind_ == Kind::Multipart ? 2 : -1; }
  int col_value() const { return kind_ == Kind::Multipart ? 3 : 2; }
  int col_del() const { return kind_ == Kind::Multipart ? 4 : 3; }
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

  Kind kind_;
  std::shared_ptr<struct VarSnapshot> vars_;  // read by the cell editors' completer thread
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
  KvGrid* grid_ = nullptr;
  wxTextCtrl* text_ = nullptr;
  wxSimplebook* book_ = nullptr;
  FlatButton* toggle_ = nullptr;
  wxSizer* title_row_ = nullptr;
  wxStaticText* hint_ = nullptr;  // "a row has a value but no key"
  bool bulk_ = false;
  bool from_text_ = false;
};

}  // namespace watt::app::http_editor
