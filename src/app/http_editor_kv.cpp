#include "app/http_editor_kv.h"

#include <wx/arrstr.h>
#include <wx/dc.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/filedlg.h>
#include <wx/generic/grideditors.h>
#include <wx/renderer.h>
#include <wx/toplevel.h>
#include <wx/weakref.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/stc/stc.h>
#include <wx/textctrl.h>

#include <algorithm>

#include "app/http_editor_widgets.h"

namespace watt::app::http_editor {

namespace {

const char* const kAssertTargets[] = {
    "res.status", "res.statusText", "res.responseTime", "res.size", "res.headers.", "res.headers.content-type",
    "res.headersSize", "res.url", "res.body", "res.body.", "res.body[0]", "res.body.length",
    "res.timing.total", "res.timing.ttfb", "res.timing.dns", "res.timing.connect", "res.timing.tls",
    "res.timing.download"};

const char* const kPreFunctions[] = {
    "hmacSha256(", "hmacSha1(", "hmacSha512(", "sha256(", "sha1(", "sha512(", "md5(", "base64(", "base64url(",
    "base64Decode(", "hex(", "urlencode(", "urldecode(", "lower(", "upper(", "trim(", "replace(", "substr(",
    "length(", "concat(", "now()", "unix()", "iso()", "uuid()", "randomInt(", "randomHex(", "jsonPath(",
    "req.method", "req.url", "req.body", "req.headers."};

}  // namespace

const std::vector<std::string>& assert_ops() {
  static const std::vector<std::string> ops = {
      "eq", "neq", "gt", "gte", "lt", "lte", "contains", "notContains", "startsWith", "endsWith", "matches",
      "notMatches", "in", "notIn", "between", "length", "isEmpty", "isNotEmpty", "isNull", "isUndefined",
      "isDefined", "isTruthy", "isFalsy", "isJson", "isNumber", "isString", "isBoolean", "isArray"};
  return ops;
}

bool assert_op_unary(std::string_view op) { return istarts_with(op, "is"); }

std::string canonical_header_name(std::string_view name) {
  std::string n = clean_header_name(name);
  for (const auto& known : header_name_choices())
    if (iequals(n, known)) return known;
  return n;
}

namespace {

wxArrayString to_array(const char* const* b, const char* const* e) {
  wxArrayString a;
  for (auto* p = b; p != e; ++p) a.Add(*p);
  return a;
}

wxArrayString to_array(const std::vector<std::string>& v) {
  wxArrayString a;
  for (const auto& x : v) a.Add(wxString::FromUTF8(x));
  return a;
}

// The cell text editor. Its completion list is the grid's own themed popup (KvGrid::update_suggest),
// not the native IAutoComplete: that one appends the rest of a suggestion as selected text, so
// typing on after a pause replaced it ("{{user", pause, "Id}}" -> "Id}}").
class CompletingEditor : public wxGridCellTextEditor {
 public:
  CompletingEditor() = default;
  wxGridCellEditor* Clone() const override { return new CompletingEditor(); }
  // The open editor of a blank row keeps its placeholder ("Header" in the new row after Tab).
  void BeginEdit(int row, int col, wxGrid* grid) override {
    wxGridCellTextEditor::BeginEdit(row, col, grid);
    auto* g = dynamic_cast<KvGrid*>(grid);
    Text()->SetHint(g && g->row_empty(row) ? g->blank_hint(col) : wxString());
  }
  // A character typed on a cell that is not being edited opens it and goes at the end of its
  // text (as in an input box), instead of replacing what the cell holds (the spreadsheet way,
  // which lost a value typed before an app switch or an Esc).
  void StartingKey(wxKeyEvent& event) override {
    const int ch = static_cast<int>(event.GetUnicodeKey());
    if (ch != WXK_NONE && ch != WXK_BACK && ch != WXK_DELETE) {
      Text()->SetInsertionPointEnd();
      Text()->WriteText(static_cast<wxChar>(ch));
      return;
    }
    wxGridCellTextEditor::StartingKey(event);
  }
  // Single-line editor centred in the row, aligned with the rendered text.
  void SetSize(const wxRect& rect) override {
    wxRect r = rect;
    const int h = Text()->GetBestSize().y;
    if (h < r.height) {
      r.y += (r.height - h) / 2;
      r.height = h;
    }
    r.x += Text()->FromDIP(4);
    r.width -= Text()->FromDIP(4);
    wxGridCellTextEditor::SetSize(r);
  }
};

}  // namespace

// Draws a cell: checkbox / delete glyph / placeholder / text with {{var}} chips.
class KvRenderer : public wxGridCellStringRenderer {
 public:
  explicit KvRenderer(KvGrid* g) : g_(g) {}
  void Draw(wxGrid& grid, wxGridCellAttr& attr, wxDC& dc, const wxRect& rect, int row, int col,
            bool) override {
    wxGridCellRenderer::Draw(grid, attr, dc, rect, row, col, false);
    const Theme& t = Theme::get();
    // The cursor cell gets an accent frame while the grid has the focus (the grid's own highlight
    // is off): visible on the Operator / Scope choice cells too, not only in an open editor.
    if (row == grid.GetGridCursorRow() && col == grid.GetGridCursorCol() && col != 0 && col != g_->col_del() &&
        !grid.IsCellEditControlShown()) {
      const wxWindow* f = wxWindow::FindFocus();
      if (f && (f == &grid || grid.IsDescendant(const_cast<wxWindow*>(f)))) {
        wxDCPenChanger pen(dc, wxPen(t.accent, grid.FromDIP(2)));
        wxDCBrushChanger brush(dc, *wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(wxRect(rect).Deflate(grid.FromDIP(1)));
      }
    }
    const bool blank = g_->row_empty(row);
    const bool enabled = g_->row_enabled(row);
    dc.SetFont(grid.GetDefaultCellFont());
    const int ch = dc.GetCharHeight();
    const int ty = rect.y + (rect.height - ch) / 2;
    const bool path = g_->kind() == KvGrid::Kind::Path;
    if (col == 0) {
      if (blank || path) return;
      const int s = grid.FromDIP(14);
      wxRect box(rect.x + (rect.width - s) / 2, rect.y + (rect.height - s) / 2, s, s);
      wxRendererNative::Get().DrawCheckBox(&grid, dc, box, enabled ? wxCONTROL_CHECKED : 0);
      return;
    }
    if (col == g_->col_del()) {
      if (blank || path) return;
      dc.SetTextForeground(t.dim);
      const wxString x = wxString::FromUTF8("\xC3\x97");  // multiplication sign
      const wxSize xs = dc.GetTextExtent(x);
      dc.DrawText(x, rect.x + (rect.width - xs.x) / 2, rect.y + (rect.height - xs.y) / 2);
      return;
    }
    wxDCClipper clip(dc, rect);
    const int pad = grid.FromDIP(8);
    int x = rect.x + pad;
    std::string s = g_->cell(row, col);
    const bool file_row = g_->kind() == KvGrid::Kind::Multipart && g_->GetCellValue(row, g_->col_type()) == "File";
    const KvGrid::Kind kind = g_->kind();
    if (col == g_->col_type()) {
      // the blank row shows a type only once one was picked for it
      if (blank && (kind == KvGrid::Kind::Multipart || s.empty() || s == g_->default_type())) return;
      dc.SetTextForeground(kind == KvGrid::Kind::Multipart || !enabled || blank ? t.dim : t.accent);
      if (kind == KvGrid::Kind::Multipart) s = file_row ? "File" : "Text";
      if (s.empty()) s = g_->default_type();  // what an unset operator / scope means when sent
      dc.DrawText(wxString::FromUTF8(s), x, ty);
      // a chevron: this cell is a choice
      const int cx = rect.GetRight() - grid.FromDIP(12), cy = rect.y + rect.height / 2;
      dc.SetPen(wxPen(t.dim, grid.FromDIP(1)));
      dc.DrawLine(cx - grid.FromDIP(3), cy - grid.FromDIP(2), cx, cy + grid.FromDIP(1));
      dc.DrawLine(cx, cy + grid.FromDIP(1), cx + grid.FromDIP(3) + 1, cy - grid.FromDIP(2) - 1);
      return;
    }
    if (col == g_->col_desc() && kind == KvGrid::Kind::PreVar) {
      if (blank) return;
      const std::string var = std::string(trim(g_->cell(row, g_->col_key())));
      const auto pv = g_->preview && !var.empty() ? g_->preview(var) : std::nullopt;
      wxString txt;
      if (!enabled) txt = "(off)";
      else if (!var.empty() && !pv) txt = wxString::FromUTF8("\xE2\x80\xA6");
      else if (pv) txt = wxString::FromUTF8(pv->ok ? pv->text.substr(0, 400) : "\xE2\x9A\xA0 " + pv->text);
      txt.Replace("\n", " ");
      txt.Replace("\r", " ");
      dc.SetTextForeground(pv && !pv->ok && enabled ? t.var_bad : t.dim);
      dc.DrawText(txt, x, ty);
      return;
    }
    if (col == g_->col_desc()) {
      dc.SetTextForeground(t.dim);
      if (!s.empty()) {
        wxFont f = grid.GetDefaultCellFont();
        f.SetStyle(wxFONTSTYLE_ITALIC);
        dc.SetFont(f);
        dc.DrawText(wxString::FromUTF8(s), x, ty);
      } else if (blank) {
        dc.DrawText("Description", x, ty);
      }
      return;
    }
    if (s.empty()) {
      wxString ph;
      if (blank) {
        ph = g_->blank_hint(col);
      } else if (kind == KvGrid::Kind::Assert && col == g_->col_value() &&
                 assert_op_unary(g_->GetCellValue(row, g_->col_type()).utf8_string())) {
        ph = "(no value)";
      } else if (path && col == g_->col_value()) {
        // Sent as ":name" otherwise: the send is refused until it has a value.
        dc.SetTextForeground(t.var_bad);
        dc.DrawText(wxString::FromUTF8("\xE2\x9A\xA0 value required"), x, ty);
        return;
      } else if (col == g_->col_value() && file_row) {
        ph = wxString::FromUTF8("Choose a file\xE2\x80\xA6");
      } else if (col == g_->col_value() && kind == KvGrid::Kind::Headers) {
        // Legal (an empty field value), but rarely meant: say what goes on the wire.
        dc.SetTextForeground(enabled ? t.warn_border : t.dim);
        dc.DrawText("(empty value: sent as \"Name:\")", x, ty);
        return;
      } else if (col == g_->col_key()) {
        // A value with no key is never sent: say so on the row itself.
        dc.SetTextForeground(t.var_bad);
        dc.DrawText(wxString::FromUTF8("\xE2\x9A\xA0 key required, not sent"), x, ty);
        return;
      }
      if (!ph.empty()) {
        dc.SetTextForeground(t.dim);
        dc.DrawText(ph, x, ty);
      }
      return;
    }
    if (path && col == g_->col_key()) s = ":" + s;
    if (file_row && col == g_->col_value() && s.find("{{") == std::string::npos) {
      // A chip with the file name (a long path cut at the end hid it), then its folder, dimmed.
      const size_t cut = s.find_last_of("/\\");
      const wxString name = wxString::FromUTF8(cut == std::string::npos ? s : s.substr(cut + 1));
      const wxString dir = cut == std::string::npos ? wxString() : wxString::FromUTF8(s.substr(0, cut + 1));
      const wxSize ns = dc.GetTextExtent(name);
      const int cp = grid.FromDIP(6);
      dc.SetPen(wxPen(t.border, 1));
      dc.SetBrush(wxBrush(t.hover));
      dc.DrawRoundedRectangle(x - 2, ty - 2, ns.x + 2 * cp, ch + 4, grid.FromDIP(4));
      dc.SetTextForeground(enabled ? t.fg : t.dim);
      dc.DrawText(name, x - 2 + cp, ty);
      x += ns.x + 2 * cp + grid.FromDIP(6);
      if (!dir.empty() && x < rect.GetRight()) {
        dc.SetTextForeground(t.dim);
        dc.DrawText(dir, x, ty);
      }
      return;
    }
    std::string problem = g_->check ? g_->check(col, g_->cell(row, col)) : std::string();
    if (problem.empty() && kind != KvGrid::Kind::Capture) problem = brace_problem(g_->cell(row, col));
    const wxColour fg = !problem.empty() ? t.var_bad : enabled && !(path && col == g_->col_key()) ? t.fg : t.dim;
    if (!problem.empty()) {  // a wavy underline under the text
      const int w = std::min(dc.GetTextExtent(wxString::FromUTF8(s)).x, rect.GetRight() - x);
      dc.SetPen(wxPen(t.var_bad, 1));
      const int uy = ty + ch;
      for (int i = 0; i + 2 <= w; i += 2) dc.DrawLine(x + i, uy - (i / 2) % 2, x + i + 2, uy - (i / 2 + 1) % 2);
    }
    auto draw_plain = [&](std::string_view part) {
      if (part.empty()) return;
      const wxString w = wxString::FromUTF8(part.data(), part.size());
      dc.SetTextForeground(fg);
      dc.DrawText(w, x, ty);
      x += dc.GetTextExtent(w).x;
    };
    const std::string_view sv(s);
    size_t pos = 0;
    for (const auto& v : find_vars(sv)) {
      if (x > rect.GetRight()) break;
      draw_plain(sv.substr(pos, v.begin - pos));
      const bool ok = g_->lookup && g_->lookup(v.name).has_value();
      const wxString w = wxString::FromUTF8(sv.data() + v.begin, v.end - v.begin);
      const wxSize ws = dc.GetTextExtent(w);
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.SetBrush(wxBrush(ok ? t.var_ok_bg : t.var_bad_bg));
      dc.DrawRoundedRectangle(x - 2, ty - 1, ws.x + 4, ch + 2, 3);
      dc.SetTextForeground(ok ? t.var_ok : t.var_bad);
      dc.DrawText(w, x, ty);
      x += ws.x;
      pos = v.end;
    }
    if (x <= rect.GetRight()) draw_plain(sv.substr(pos));
  }
  wxGridCellRenderer* Clone() const override { return new KvRenderer(g_); }

 private:
  KvGrid* g_;
};

KvGrid::KvGrid(wxWindow* parent, Kind kind)
    : wxGrid(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS | wxBORDER_NONE),
      kind_(kind) {
  const Theme& t = Theme::get();
  switch (kind) {
    case Kind::Params: case Kind::Headers: case Kind::Path: key_c_ = 1; value_c_ = 2; desc_c_ = 3; del_c_ = 4; break;
    case Kind::UrlEncoded: key_c_ = 1; value_c_ = 2; del_c_ = 3; break;
    case Kind::Multipart: key_c_ = 1; type_c_ = 2; value_c_ = 3; del_c_ = 4; break;
    case Kind::Assert: key_c_ = 1; type_c_ = 2; value_c_ = 3; desc_c_ = 4; del_c_ = 5; break;
    case Kind::Capture: key_c_ = 1; value_c_ = 2; type_c_ = 3; del_c_ = 4; break;
    case Kind::PreVar: key_c_ = 1; value_c_ = 2; type_c_ = 3; desc_c_ = 4; del_c_ = 5; break;
  }
  CreateGrid(1, del_c_ + 1);
  SetRowLabelSize(0);
  SetColLabelSize(FromDIP(28));
  SetDefaultRowSize(FromDIP(30), true);
  DisableDragRowSize();
  DisableDragColMove();
  DisableDragGridSize();
  SetMargins(0, 0);
  SetCellHighlightPenWidth(0);
  SetCellHighlightROPenWidth(0);
  SetSelectionBackground(t.field);
  SetSelectionForeground(t.fg);
  SetDefaultCellBackgroundColour(t.field);
  SetDefaultCellTextColour(t.fg);
  SetGridLineColour(t.border);
  SetLabelBackgroundColour(t.bg);
  SetLabelTextColour(t.dim);
  SetLabelFont(GetFont().Bold());
  SetBackgroundColour(t.bg);
  GetGridWindow()->SetBackgroundColour(t.bg);
  SetColLabelAlignment(wxALIGN_LEFT, wxALIGN_CENTRE);
  ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_DEFAULT);
  SetTabBehaviour(wxGrid::Tab_Wrap);

  SetColLabelValue(0, "");
  SetColLabelValue(col_del(), "");
  switch (kind) {
    case Kind::Assert:
      SetColLabelValue(col_key(), "Target");
      SetColLabelValue(col_type(), "Operator");
      SetColLabelValue(col_value(), "Expected");
      SetColLabelValue(col_desc(), "Description");
      break;
    case Kind::Capture:
      SetColLabelValue(col_key(), "Variable");
      SetColLabelValue(col_value(), "From (response)");
      SetColLabelValue(col_type(), "Scope");
      break;
    case Kind::PreVar:
      SetColLabelValue(col_key(), "Variable");
      SetColLabelValue(col_value(), "Expression");
      SetColLabelValue(col_type(), "Scope");
      SetColLabelValue(col_desc(), "Value now");
      break;
    default:
      SetColLabelValue(col_key(), kind == Kind::Headers ? "Header" : "Key");
      SetColLabelValue(col_value(), "Value");
      if (col_type() >= 0) SetColLabelValue(col_type(), "Type");
      if (col_desc() >= 0) SetColLabelValue(col_desc(), "Description");
      break;
  }

  SetDefaultRenderer(new KvRenderer(this));
  auto ro = [&](int col) {
    auto* a = new wxGridCellAttr;
    a->SetReadOnly();
    SetColAttr(col, a);
  };
  ro(0);
  ro(col_del());
  if (kind == Kind::Path) {
    ro(col_key());  // the name comes from the URL
  } else {
    auto* a = new wxGridCellAttr;
    a->SetEditor(new CompletingEditor());
    SetColAttr(col_key(), a);
  }
  key_choices_ = kind == Kind::Headers  ? to_array(header_name_choices())
                 : kind == Kind::Assert ? to_array(std::begin(kAssertTargets), std::end(kAssertTargets))
                                        : wxArrayString();
  // A header's values follow its name (suggestions()): none here.
  value_choices_ = kind == Kind::Capture   ? to_array(std::begin(kAssertTargets), std::end(kAssertTargets))
                   : kind == Kind::PreVar  ? to_array(std::begin(kPreFunctions), std::end(kPreFunctions))
                                           : wxArrayString();
  {
    auto* a = new wxGridCellAttr;
    a->SetEditor(new CompletingEditor());
    SetColAttr(col_value(), a);
  }
  if (kind == Kind::PreVar) {
    ro(col_desc());  // computed
  } else if (col_desc() >= 0) {
    auto* a = new wxGridCellAttr;
    a->SetEditor(new CompletingEditor());
    SetColAttr(col_desc(), a);
  }
  // The type column has no cell editor: a click, Enter / F2 / Space / Alt+Down or a typed
  // character opens the themed pick list (open_picker).
  if (col_type() >= 0) ro(col_type());

  Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
    e.Skip();
    close_picker();
    close_suggest();
    layout_columns();
  });

  Bind(wxEVT_GRID_CELL_LEFT_CLICK, [this](wxGridEvent& e) {
    const int r = e.GetRow(), c = e.GetCol();
    if (kind_ == Kind::Path && (c == 0 || c == col_del() || c == col_key())) {
      go(r, col_value(), true);  // only the value (and the description) are editable
      return;
    }
    if (c == 0) {
      if (!row_empty(r)) {
        SetCellValue(r, 0, row_enabled(r) ? "0" : "1");
        changed();
      }
      return;
    }
    if (c == col_del()) {
      delete_row(r);
      return;
    }
    if (c == col_type() && kind_ != Kind::Path) {
      if (pick_ && pick_row_ == r) {
        close_picker();  // a second click closes it, like a combo box
        return;
      }
      SetGridCursor(r, c);
      SetFocus();
      open_picker(r);
      return;
    }
    if (c == col_value() && kind_ == Kind::Multipart && GetCellValue(r, col_type()) == "File") {
      SetGridCursor(r, c);
      choose_file(r);
      return;
    }
    e.Skip();
    CallAfter([this, r, c] {
      if (GetGridCursorRow() == r && GetGridCursorCol() == c && !IsCellEditControlEnabled() && CanEnableCellControl())
        EnableCellEditControl();
    });
  });

  // Tab / Enter (Shift: back): commit the cell and edit the next one (Key -> Value -> next row's
  // Key). The key handler sees keys from the grid window and from the open cell editor (wxGrid
  // forwards both to its own handler, where dynamic handlers run first).
  Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) { on_key(e); });
  Bind(wxEVT_CHAR, [this](wxKeyEvent& e) {
    const wxChar ch = e.GetUnicodeKey();
    if (pick_ && !e.ControlDown() && !e.AltDown()) {
      if (ch != WXK_NONE && pick_->chr(ch)) return;
    } else if (!IsCellEditControlEnabled() && col_type() >= 0 && GetGridCursorCol() == col_type() && !e.ControlDown() &&
               !e.AltDown() && ch != WXK_NONE && ch > 32 && ch != 127) {
      open_picker(GetGridCursorRow(), ch);  // typing on the cell filters the list
      return;
    }
    e.Skip();
  });
  GetGridWindow()->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) {
    e.Skip();
    if (!pick_) return;
    CallAfter([this] {
      if (!pick_) return;
      // A press on the list itself (if it took the focus) picks there: keep it open for that.
      if (pick_->GetScreenRect().Contains(wxGetMousePosition()) && wxGetMouseState().LeftIsDown()) {
        SetFocus();
        return;
      }
      wxWindow* f = wxWindow::FindFocus();
      if (f && (f == this || IsDescendant(f))) return;
      close_picker();
    });
  });
  Bind(wxEVT_GRID_SELECT_CELL, [this](wxGridEvent& e) {
    e.Skip();
    if (pick_ && (e.GetRow() != pick_row_ || e.GetCol() != col_type())) close_picker();
    // The renderer frames the cursor cell: repaint the old and the new one.
    const int r0 = GetGridCursorRow(), c0 = GetGridCursorCol();
    if (r0 >= 0 && c0 >= 0) RefreshBlock(r0, c0, r0, c0);
    CallAfter([this] {
      const int r = GetGridCursorRow(), c = GetGridCursorCol();
      if (r >= 0 && c >= 0 && r < GetNumberRows()) RefreshBlock(r, c, r, c);
    });
  });
  auto refresh_cursor = [this](wxFocusEvent& e) {
    e.Skip();
    CallAfter([this] {
      const int r = GetGridCursorRow(), c = GetGridCursorCol();
      if (r >= 0 && c >= 0 && r < GetNumberRows()) RefreshBlock(r, c, r, c);
    });
  };
  GetGridWindow()->Bind(wxEVT_SET_FOCUS, refresh_cursor);
  GetGridWindow()->Bind(wxEVT_KILL_FOCUS, refresh_cursor);
  Bind(wxEVT_SCROLLWIN_THUMBTRACK, [this](wxScrollWinEvent& e) {
    e.Skip();
    close_picker();
    close_suggest();
  });
  Bind(wxEVT_GRID_TABBING, [this](wxGridEvent& e) { nav_move(!e.ShiftDown(), false); });
  // The delete column is mouse only: the cursor never lands there.
  Bind(wxEVT_GRID_SELECT_CELL, [this](wxGridEvent& e) {
    if (e.GetCol() == col_del()) {
      e.Veto();
      return;
    }
    e.Skip();
  });

  Bind(wxEVT_GRID_EDITOR_SHOWN, [this](wxGridEvent& e) {
    e.Skip();
    var_list_.clear();
    if (!var_names || kind_ == Kind::Capture) return;
    for (const auto& n : var_names()) var_list_.Add(wxString::FromUTF8(n));
  });
  Bind(wxEVT_GRID_EDITOR_CREATED, [this](wxGridEditorCreatedEvent& e) {
    e.Skip();
    if (auto* tc = wxDynamicCast(e.GetControl(), wxTextCtrl)) {
      // Tab / Enter move to the next cell here (ahead of the text control, which would insert
      // a tab or eat the Enter). With the completion list open, the arrows move in it and Tab,
      // Enter or a click insert the highlighted entry; Esc closes it (the edit stays open).
      // wxGrid closes the editor when it loses the focus. Not when the focus goes to another app
      // (or another window of this one): the typed text, the caret and the undo stay as they
      // were, and on_activate gives the editor the focus back (copy a token in a browser, come
      // back, paste). The grid's handler (pushed on the control) skips a focus loss while its
      // "in set focus" flag is up: raise it for this one event.
      if (!activate_bound_) {
        if (wxWindow* tlw = wxGetTopLevelParent(this)) {
          tlw->Bind(wxEVT_ACTIVATE, &KvGrid::on_activate, this);
          activate_bound_ = true;
        }
      }
      wxEvtHandler* gh = tc->GetEventHandler();
      if (gh != tc && gh->GetClassInfo() && wxString(gh->GetClassInfo()->GetClassName()) == "wxGridCellEditorEvtHandler") {
        gh->Bind(wxEVT_KILL_FOCUS, [this, tc, gh](wxFocusEvent& fe) {
          fe.Skip();
          wxWindow* to = fe.GetWindow();
          wxWindow* tlw = wxGetTopLevelParent(this);
          const bool same_tlw = to && tlw && wxGetTopLevelParent(to) == tlw;
          if (same_tlw || !tlw) return;  // within this window: the edit ends as usual
          auto* eh = static_cast<wxGridCellEditorEvtHandler*>(gh);
          eh->SetInSetFocus(true);
          tc->GetSelection(&kept_from_, &kept_to_);
          kept_open_ = true;
          close_suggest();
          CallAfter([this, w = wxWeakRef<wxEvtHandler>(gh)] {
            if (w) static_cast<wxGridCellEditorEvtHandler*>(w.get())->SetInSetFocus(false);
            // A native child of this window (WebView2) took the focus, not another app: end the
            // edit as a focus loss inside the window would.
            auto* top = dynamic_cast<wxTopLevelWindow*>(wxGetTopLevelParent(this));
            if (kept_open_ && top && top->IsActive()) {
              wxWindow* f = wxWindow::FindFocus();
              if (!f || !IsDescendant(f)) {
                kept_open_ = false;
                if (IsCellEditControlEnabled()) DisableCellEditControl();
              }
            }
          });
        });
      }
      tc->Bind(wxEVT_CHAR_HOOK, [this, tc](wxKeyEvent& ke) {
        const int k = ke.GetKeyCode();
        if (suggest_ && !ke.ControlDown() && !ke.AltDown() && suggest_key(k, ke.ShiftDown())) return;
        // Ctrl+Z / Ctrl+Y in a cell where nothing was typed yet (Enter moved here after a commit):
        // the table's undo, as if the edit had not been opened. With typed text the text
        // control's own undo applies.
        const int mods = ke.GetModifiers();
        const bool undo = mods == wxMOD_CONTROL && k == 'Z';
        const bool redo = (mods == wxMOD_CONTROL && k == 'Y') || (mods == (wxMOD_CONTROL | wxMOD_SHIFT) && k == 'Z');
        if (undo || redo) {
          const int r = GetGridCursorRow(), c = GetGridCursorCol();
          if (r >= 0 && c >= 0 && r < GetNumberRows() && tc->GetValue() == GetCellValue(r, c) &&
              !(redo ? redo_ : undo_).empty()) {
            DisableCellEditControl();
            undo_step(redo);
            return;
          }
        }
        if (k == WXK_ESCAPE && !ke.HasAnyModifiers()) {
          commit_edit();  // no list to close: the edit ends, keeping what was typed
          return;
        }
        if (!ke.ControlDown() && !ke.AltDown()) {
          if (k == WXK_TAB) {
            nav_move(!ke.ShiftDown(), false);
            return;
          }
          if (k == WXK_RETURN || k == WXK_NUMPAD_ENTER) {
            nav_move(!ke.ShiftDown(), true);
            return;
          }
        }
        ke.Skip();
      });
      tc->Bind(wxEVT_TEXT, [this, tc](wxCommandEvent& te) {
        te.Skip();
        if (updating_) return;
        const int r = GetGridCursorRow(), c = GetGridCursorCol();
        const wxString now = tc->GetValue();
        // The editor fills itself with the cell's text when the edit starts (a click, F2,
        // Enter from the previous cell): that is not a change. The cell keeps its old text
        // until the edit ends, so it is what the edit began with.
        const bool same = r >= 0 && c >= 0 && r < GetNumberRows() && now == GetCellValue(r, c);
        if (same && !(live_row_ == r && live_col_ == c)) {
          if (!inserting_) update_suggest(tc);
          return;
        }
        live_row_ = r;
        live_col_ = c;
        live_text_ = now;
        // The new blank row is added when the edit ends: changing the table while the editor
        // is open would close it and lose what was typed.
        changed();
        if (!inserting_) update_suggest(tc);
      });
      // Keys that bypass the hook (posted ones, some IMEs): the arrows still move in the list.
      tc->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& ke) {
        const int k = ke.GetKeyCode();
        if (suggest_ && !ke.ControlDown() && !ke.AltDown() && k != WXK_TAB && k != WXK_RETURN &&
            k != WXK_NUMPAD_ENTER && suggest_key(k, ke.ShiftDown()))
          return;
        ke.Skip();
      });
      tc->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& fe) {
        fe.Skip();
        close_suggest();
      });
      tc->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& me) {
        me.Skip();
        close_suggest();  // the caret moves: the list was for the old place
      });
    }
  });
  Bind(wxEVT_GRID_EDITOR_HIDDEN, [this](wxGridEvent& e) {
    e.Skip();
    kept_open_ = false;
    close_suggest();
    CallAfter([this] {
      if (live_row_ < 0) return;
      live_row_ = live_col_ = -1;
      live_text_.clear();
      ensure_blank_row();
      changed();
    });
  });
  Bind(wxEVT_GRID_CELL_CHANGED, [this](wxGridEvent& e) {
    e.Skip();
    const int r = e.GetRow();
    // The committed text is what was already reported live while typing (a save in between
    // stored it): an echo, not a new edit, unless the clean-up below changes something.
    bool echo = live_row_ == r && live_col_ == e.GetCol() && live_text_ == GetCellValue(r, e.GetCol());
    live_row_ = live_col_ = -1;
    live_text_.clear();
    // The tooltip was for the old text (an error since fixed): drop it, the next mouse move
    // computes it again for what the cell holds now.
    tip_row_ = tip_col_ = -2;
    GetGridWindow()->UnsetToolTip();
    if (kind_ == Kind::Headers && (e.GetCol() == col_key() || e.GetCol() == col_value())) {
      const std::string v = GetCellValue(r, e.GetCol()).utf8_string();
      // A name with blanks inside ("Authorization Bearer x") is kept as typed (trimmed) so its
      // lint shows; squeezing the blanks out would hide the mistake ("AuthorizationBearerx").
      const std::string tk(trim(v));
      const bool blanks = tk.find_first_of(" \t") != std::string::npos;
      std::string c = e.GetCol() != col_key() ? clean_header_value(v) : blanks ? tk : canonical_header_name(v);
      if (c != v) {
        SetCellValue(r, e.GetCol(), wxString::FromUTF8(c));
        echo = false;
      }
    }
    if (!row_empty(r) && GetCellValue(r, 0).empty()) SetCellValue(r, 0, "1");  // rows(): enabled either way
    if (col_type() >= 0 && GetCellValue(r, col_type()).empty()) {
      SetCellValue(r, col_type(), wxString::FromUTF8(default_type()));
      echo = false;
    }
    if (kind_ == Kind::Multipart && e.GetCol() == col_type()) {
      SetCellValue(r, col_value(), "");
      echo = false;
    }
    ensure_blank_row();
    record_undo();  // the cell's commit (the edit control may still count as enabled here)
    echo_ = echo;
    changed();
    echo_ = false;
  });

  GetGridWindow()->Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
    e.Skip();
    const wxPoint p = CalcUnscrolledPosition(e.GetPosition());
    update_tooltip(YToRow(p.y), XToCol(p.x));
  });
  SetGridCursor(0, col_key());
  layout_columns();
}

void KvGrid::DrawColLabel(wxDC& dc, int col) {
  const Theme& t = Theme::get();
  const int x = GetColLeft(col), w = GetColWidth(col), h = GetColLabelSize();
  dc.SetPen(*wxTRANSPARENT_PEN);
  dc.SetBrush(wxBrush(t.bg));
  dc.DrawRectangle(x, 0, w, h);
  dc.SetPen(wxPen(t.border));
  dc.DrawLine(x, 0, x + w, 0);
  dc.DrawLine(x, h - 1, x + w, h - 1);
  if (col == 0) dc.DrawLine(x, 0, x, h);
  dc.DrawLine(x + w - 1, 0, x + w - 1, h);
  const wxString label = GetColLabelValue(col);
  if (label.empty()) return;
  dc.SetFont(GetLabelFont());
  dc.SetTextForeground(t.dim);
  const wxSize ts = dc.GetTextExtent(label);
  dc.DrawText(label, x + FromDIP(8), (h - ts.y) / 2);
}

std::string KvGrid::cell(int row, int col) const {
  if (row == live_row_ && col == live_col_) return live_text_.utf8_string();
  return GetCellValue(row, col).utf8_string();
}

bool KvGrid::row_empty(int row) const {
  if (row < 0 || row >= GetNumberRows()) return true;
  if (kind_ == Kind::Path) return GetCellValue(row, col_key()).empty();
  return cell(row, col_key()).empty() && cell(row, col_value()).empty() &&
         (col_desc() < 0 || cell(row, col_desc()).empty()) &&
         !(kind_ == Kind::Multipart && GetCellValue(row, col_type()) == "File");
}

std::string KvGrid::default_type() const {
  switch (kind_) {
    case Kind::Multipart: return "Text";
    case Kind::Assert: return "eq";
    case Kind::Capture: case Kind::PreVar: return "runtime";
    default: return {};
  }
}

void KvGrid::ensure_blank_row() {
  if (kind_ == Kind::Path) return;
  const int n = GetNumberRows();
  if (n == 0 || !row_empty(n - 1)) {
    AppendRows(1);
    if (col_type() >= 0) SetCellValue(GetNumberRows() - 1, col_type(), wxString::FromUTF8(default_type()));
  }
}

void KvGrid::layout_columns() {
  const int w = GetClientSize().x;
  if (w <= 0) return;
  int type = 0;
  if (col_type() >= 0) type = FromDIP(kind_ == Kind::Multipart ? 72 : kind_ == Kind::Assert ? 128 : 116);
  const int chk = FromDIP(34), del = FromDIP(34);
  const int rest = std::max(FromDIP(160), w - chk - del - type);
  // Key 40 % / Value 60 %; with a description 30 / 42 / 28.
  int key = rest * 2 / 5, desc = 0;
  if (kind_ == Kind::PreVar) {
    key = rest * 18 / 100;
    desc = rest * 30 / 100;
  } else if (kind_ == Kind::Assert) {
    key = rest * 36 / 100;
    desc = rest * 30 / 100;
  } else if (col_desc() >= 0) {
    key = rest * 30 / 100;
    desc = rest * 28 / 100;
  } else if (kind_ == Kind::Capture) {
    key = rest * 30 / 100;
  }
  BeginBatch();
  SetColSize(0, chk);
  SetColSize(col_key(), key);
  if (col_type() >= 0) SetColSize(col_type(), type);
  if (col_desc() >= 0) SetColSize(col_desc(), desc);
  SetColSize(col_value(), rest - key - desc);
  SetColSize(col_del(), del);
  EndBatch();
}

namespace {
bool same_rows(const std::vector<KvRow>& a, const std::vector<KvRow>& b) {
  return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const KvRow& x, const KvRow& y) {
    return x.key == y.key && x.value == y.value && x.enabled == y.enabled && x.file == y.file && x.bare == y.bare &&
           x.desc == y.desc && x.type == y.type;
  });
}
}  // namespace

bool KvGrid::undo_step(bool redo) {
  auto& from = redo ? redo_ : undo_;
  auto& to = redo ? undo_ : redo_;
  if (from.empty()) return false;
  std::vector<KvRow> rows_now = rows();
  std::vector<KvRow> back = std::move(from.back());
  from.pop_back();
  to.push_back(std::move(rows_now));
  auto keep_undo = std::move(undo_);
  auto keep_redo = std::move(redo_);
  const int r = GetGridCursorRow(), c = GetGridCursorCol();
  set_rows(back);  // resets the history: put it back
  undo_ = std::move(keep_undo);
  redo_ = std::move(keep_redo);
  undo_base_ = std::move(back);
  if (r >= 0 && r < GetNumberRows() && c >= 0) SetGridCursor(r, c);
  ForceRefresh();
  report_orphans();
  if (on_change) on_change();
  return true;
}

void KvGrid::record_undo() {
  if (updating_) return;
  std::vector<KvRow> now = rows();
  if (same_rows(now, undo_base_)) return;
  undo_.push_back(std::move(undo_base_));
  if (undo_.size() > 100) undo_.erase(undo_.begin());
  redo_.clear();
  undo_base_ = std::move(now);
}

void KvGrid::changed() {
  // A committed change (not a keystroke inside a cell): the rows before it can be restored.
  if (!IsCellEditControlEnabled()) record_undo();
  ForceRefresh();
  report_orphans();
  if (on_change) on_change();
}

void KvGrid::report_orphans() {
  const size_t n = orphan_count();
  if (n == orphans_) return;
  orphans_ = n;
  if (on_orphans) on_orphans(n);
}

bool KvGrid::row_orphan(int row) const {
  if (kind_ == Kind::Path || row_empty(row)) return false;
  return trim(cell(row, col_key())).empty();
}

size_t KvGrid::orphan_count() const {
  size_t n = 0;
  for (int r = 0; r < GetNumberRows(); ++r)
    if (row_orphan(r)) ++n;
  return n;
}

void KvGrid::delete_row(int r) {
  if (kind_ == Kind::Path) return;
  if (r < 0 || r >= GetNumberRows() - 1 || row_empty(r)) return;
  if (IsCellEditControlEnabled()) DisableCellEditControl();
  live_row_ = live_col_ = -1;
  live_text_.clear();
  DeleteRows(r);
  ensure_blank_row();
  changed();
}

void KvGrid::set_rows(const std::vector<KvRow>& rows) {
  updating_ = true;
  if (IsCellEditControlEnabled()) DisableCellEditControl();
  live_row_ = live_col_ = -1;
  live_text_.clear();
  BeginBatch();
  const int want = static_cast<int>(rows.size()) + (kind_ == Kind::Path ? 0 : 1);
  const int have = GetNumberRows();
  if (have < want) AppendRows(want - have);
  else if (have > want) DeleteRows(want, have - want);
  for (int r = 0; r < want; ++r) {
    if (r < static_cast<int>(rows.size())) {
      const auto& kv = rows[static_cast<size_t>(r)];
      SetCellValue(r, 0, kv.enabled ? "1" : "0");
      SetCellValue(r, col_key(), wxString::FromUTF8(kv.key));
      SetCellValue(r, col_value(), wxString::FromUTF8(kv.value));
      if (col_desc() >= 0) SetCellValue(r, col_desc(), wxString::FromUTF8(kv.desc));
      if (kind_ == Kind::Multipart) SetCellValue(r, col_type(), kv.file ? "File" : "Text");
      else if (col_type() >= 0) SetCellValue(r, col_type(), wxString::FromUTF8(kv.type.empty() ? default_type() : kv.type));
    } else {
      for (int c = 0; c < GetNumberCols(); ++c) SetCellValue(r, c, "");
      if (col_type() >= 0) SetCellValue(r, col_type(), wxString::FromUTF8(default_type()));
    }
  }
  EndBatch();
  updating_ = false;
  undo_.clear();
  redo_.clear();
  undo_base_ = this->rows();
  tip_row_ = tip_col_ = -2;
  if (GetNumberRows() > 0 && GetGridCursorCol() != col_key() && GetGridCursorCol() != col_value() &&
      GetGridCursorCol() != col_type() && GetGridCursorCol() != col_desc())
    SetGridCursor(std::clamp(GetGridCursorRow(), 0, GetNumberRows() - 1), kind_ == Kind::Path ? col_value() : col_key());
  ForceRefresh();
  report_orphans();
  if (on_rows_set) on_rows_set();
}

std::vector<KvRow> KvGrid::rows() const {
  std::vector<KvRow> out;
  for (int r = 0; r < GetNumberRows(); ++r) {
    if (row_empty(r)) continue;
    KvRow kv;
    kv.key = cell(r, col_key());
    kv.value = cell(r, col_value());
    kv.enabled = GetCellValue(r, 0) != "0";
    kv.file = kind_ == Kind::Multipart && GetCellValue(r, col_type()) == "File";
    if (col_desc() >= 0) kv.desc = cell(r, col_desc());
    if (kind_ != Kind::Multipart && col_type() >= 0) kv.type = GetCellValue(r, col_type()).utf8_string();
    out.push_back(std::move(kv));
  }
  return out;
}

int KvGrid::live_index(int* col) const {
  if (live_row_ < 0 || live_row_ >= GetNumberRows() || row_empty(live_row_)) return -1;
  int n = 0;
  for (int r = 0; r < live_row_; ++r) n += !row_empty(r);
  if (col) *col = live_col_;
  return n;
}

size_t KvGrid::enabled_count() const {
  size_t n = 0;
  for (int r = 0; r < GetNumberRows(); ++r)
    if (!row_empty(r) && GetCellValue(r, 0) != "0" && !row_orphan(r)) ++n;
  return n;
}

void KvGrid::choose_file(int row) {
  wxFileDialog dlg(this, "Choose a file to upload", "", "", "All files (*.*)|*.*", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
  if (dlg.ShowModal() != wxID_OK) return;
  if (row < 0 || row >= GetNumberRows()) return;
  if (col_type() >= 0) SetCellValue(row, col_type(), "File");
  SetCellValue(row, col_value(), dlg.GetPath());
  SetCellValue(row, 0, "1");
  ensure_blank_row();
  changed();
}

// ------------------------------------------------------------------ KvPanel

KvPanel::KvPanel(wxWindow* parent, KvGrid::Kind kind, const wxString& title) : wxPanel(parent) {
  const Theme& t = Theme::get();
  SetBackgroundColour(t.bg);
  auto* s = new wxBoxSizer(wxVERTICAL);
  title_row_ = new wxBoxSizer(wxHORIZONTAL);
  auto* l = new wxStaticText(this, wxID_ANY, title);
  l->SetForegroundColour(t.dim);
  title_row_->Add(l, 0, wxALIGN_CENTER_VERTICAL);
  title_row_->AddStretchSpacer();
  toggle_ = new FlatButton(this, "Bulk Edit", FlatButton::Style::Subtle);
  toggle_->SetToolTip("Edit the rows as text, one \"key: value\" per line (a line starting with // is not sent)");
  title_row_->Add(toggle_, 0, wxALIGN_CENTER_VERTICAL);
  // Path: the rows come from the URL. Assert / Capture: three columns, not "key: value" lines.
  if (kind == KvGrid::Kind::Path || kind == KvGrid::Kind::Assert || kind == KvGrid::Kind::Capture ||
      kind == KvGrid::Kind::PreVar)
    toggle_->Hide();
  s->Add(title_row_, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(4));
  book_ = new wxSimplebook(this);
  book_->SetBackgroundColour(t.bg);
  grid_ = new KvGrid(book_, kind);
  book_->AddPage(grid_, "table");
  // The bulk text box (a rich edit control) is created on first use: a new tab does not pay for it.
  // Right above the table, where the eye is while typing (a note at the bottom of a tall page
  // goes unseen).
  hint_ = new wxStaticText(this, wxID_ANY, "");
  hint_->SetForegroundColour(t.var_bad);
  hint_->Hide();
  s->Add(hint_, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
  s->Add(book_, 1, wxEXPAND);
  SetSizer(s);
  grid_->on_orphans = [this](size_t n) {
    const char* what = grid_->kind() == KvGrid::Kind::Headers ? "name" : "key";
    if (n)
      hint_->SetLabel(wxString::Format(wxString::FromUTF8("\xE2\x9A\xA0 %zu row%s with a value but no %s %s not sent. "
                                                           "Type a %s or delete the row."),
                                       n, n == 1 ? "" : "s", what, n == 1 ? "is" : "are", what));
    if (hint_->IsShown() != (n > 0)) {
      hint_->Show(n > 0);
      Layout();
    }
  };
  toggle_->on_click = [this] { set_bulk(!bulk_); };
  grid_->on_rows_set = [this] {
    if (bulk_ && !from_text_) refresh_text();
  };
}

void KvPanel::fit_rows(int max_px) {
  int h = -1;
  if (max_px >= 0) {
    const int min_h = grid_->GetColLabelSize() + grid_->GetDefaultRowSize();
    h = grid_->GetColLabelSize() + grid_->GetNumberRows() * grid_->GetDefaultRowSize() + FromDIP(2);
    h = std::min(h, std::max(max_px, min_h));
  }
  if (book_->GetMinSize().y == h) return;
  book_->SetMinSize(wxSize(-1, h));
  grid_->SetMinSize(wxSize(-1, h));
  InvalidateBestSize();
  Layout();
}

void KvPanel::add_above(wxWindow* w) {
  GetSizer()->Insert(1, w, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
}

void KvPanel::set_bulk(bool on) {
  if (on == bulk_) return;
  if (grid_->IsCellEditControlEnabled()) grid_->DisableCellEditControl();
  bulk_ = on;
  if (on && !text_) {
    // A themed Scintilla editor (a native multi-line box is light, with light scroll bars, in
    // the dark theme), framed like the table.
    const Theme& t = Theme::get();
    // In a 1 px frame of the border colour, so it reads as a box to type in.
    auto* frame = new wxPanel(book_);
    frame->SetBackgroundColour(t.border);
    text_ = new wxStyledTextCtrl(frame, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    auto* fs = new wxBoxSizer(wxVERTICAL);
    fs->Add(text_, 1, wxEXPAND | wxALL, 1);
    frame->SetSizer(fs);
    wxFont f(wxFontInfo(GetFont().GetPointSize()).Family(wxFONTFAMILY_TELETYPE).FaceName("Cascadia Mono"));
    if (!f.IsOk() || f.GetFaceName() != "Cascadia Mono")
      f = wxFont(wxFontInfo(GetFont().GetPointSize()).Family(wxFONTFAMILY_TELETYPE).FaceName("Consolas"));
    text_->StyleSetFont(wxSTC_STYLE_DEFAULT, f);
    text_->StyleSetForeground(wxSTC_STYLE_DEFAULT, t.fg);
    text_->StyleSetBackground(wxSTC_STYLE_DEFAULT, t.field);
    text_->StyleClearAll();
    text_->StyleSetForeground(1, t.dim);  // "//" lines: not sent
    text_->SetCodePage(wxSTC_CP_UTF8);
    text_->SetEOLMode(wxSTC_EOL_LF);
    text_->SetMarginWidth(0, 0);
    text_->SetMarginWidth(1, 0);
    text_->SetMarginLeft(FromDIP(8));
    text_->SetCaretForeground(t.fg);
    text_->SetSelBackground(true, t.dark ? wxColour(0x26, 0x4F, 0x78) : wxColour(0xAD, 0xD6, 0xFF));
    text_->SetCaretLineVisible(false);
    text_->SetScrollWidth(1);
    text_->SetScrollWidthTracking(true);
    text_->SetUseHorizontalScrollBar(true);
    text_->SetExtraAscent(FromDIP(2));
    text_->SetExtraDescent(FromDIP(2));
    text_->SetToolTip(grid_->kind() == KvGrid::Kind::Headers
                          ? "One header per line, \"Name: value\"; a line starting with // is kept but not sent"
                          : "One row per line, \"key: value\"; a line starting with // is kept but not sent");
    // Placeholder: an annotation under the empty first line, in the dim colour.
    text_->StyleSetForeground(2, t.dim);
    text_->StyleSetBackground(2, t.field);
    text_->StyleSetFont(2, f);
    text_->AnnotationSetVisible(wxSTC_ANNOTATION_STANDARD);
    book_->AddPage(frame, "bulk");
    text_->Bind(wxEVT_STC_CHANGE, [this](wxStyledTextEvent& e) {
      e.Skip();
      if (!setting_text_) text_changed();
      dim_disabled_lines();
      show_bulk_hint();
    });
  }
  if (on) {
    refresh_text();
    show_bulk_hint();
  }
  book_->ChangeSelection(on ? 1 : 0);
  toggle_->SetLabel(on ? "Key-Value Edit" : "Bulk Edit");
  Layout();
  if (on) {
    text_->SetFocus();
    text_->DocumentEnd();
  } else {
    grid_->SetFocus();
  }
}

void KvPanel::refresh_text() {
  std::vector<KvItem> items;
  for (const auto& r : grid_->rows()) items.push_back({r.key, r.value, r.enabled, false});
  const std::string want = to_bulk(items);
  if (bulk_text() == want) return;
  setting_text_ = true;
  text_->SetText(wxString::FromUTF8(want));
  text_->EmptyUndoBuffer();
  setting_text_ = false;
  dim_disabled_lines();
}

void KvPanel::show_bulk_hint() {
  if (!text_) return;
  const bool empty = text_->GetLength() == 0;
  if (empty == bulk_hint_) return;
  bulk_hint_ = empty;
  // The annotation is no edit, though Scintilla may report it as a change.
  struct Guard {
    bool& b;
    bool was;
    ~Guard() { b = was; }
  } guard{setting_text_, setting_text_};
  setting_text_ = true;
  if (!empty) {
    text_->AnnotationClearAll();
    return;
  }
  const bool headers = grid_->kind() == KvGrid::Kind::Headers;
  text_->AnnotationSetText(0, headers ? "One per line:  Name: value\n"
                                        "Content-Type: application/json\n"
                                        "Authorization: Bearer {{token}}\n"
                                        "// X-Debug: 1      (a // line is kept, not sent)"
                                      : "One per line:  key: value\n"
                                        "page: 1\n"
                                        "q: {{search}}\n"
                                        "// sort: asc      (a // line is kept, not sent)");
  text_->AnnotationSetStyle(0, 2);
}

std::string KvPanel::bulk_text() const {
  if (!text_) return {};
  return std::string(text_->GetCharacterPointer(), static_cast<size_t>(text_->GetLength()));
}

void KvPanel::dim_disabled_lines() {
  if (!text_ || text_->GetLength() > 256 * 1024) return;
  const int n = text_->GetLineCount();
  for (int l = 0; l < n; ++l) {
    const int a = text_->PositionFromLine(l), b = text_->GetLineEndPosition(l);
    int p = a;
    while (p < b && (text_->GetCharAt(p) == ' ' || text_->GetCharAt(p) == '\t')) ++p;
    const bool off = p + 1 < b && text_->GetCharAt(p) == '/' && text_->GetCharAt(p + 1) == '/';
    text_->StartStyling(a);
    text_->SetStyling(b - a, off ? 1 : 0);
  }
}

void KvPanel::text_changed() {
  // Descriptions are not in the text: a row keeps the one of the first earlier row with its key.
  std::vector<KvRow> before = grid_->rows();
  std::vector<KvRow> rows;
  for (auto& it : parse_bulk(bulk_text())) {
    if (grid_->kind() == KvGrid::Kind::Headers) {
      it.key = canonical_header_name(it.key);
      it.value = clean_header_value(it.value);
    }
    KvRow row{std::move(it.key), std::move(it.value), it.enabled, false, false};
    for (auto& b : before)
      if (!b.desc.empty() && b.key == row.key) {
        row.desc = std::move(b.desc);
        b.desc.clear();
        break;
      }
    rows.push_back(std::move(row));
  }
  from_text_ = true;
  grid_->set_rows(rows);
  from_text_ = false;
  if (grid_->on_change) grid_->on_change();
}

void KvGrid::on_key(wxKeyEvent& e) {
  const int code = e.GetKeyCode();
  const bool editing = IsCellEditControlEnabled();
  if (suggest_ && editing && !e.ControlDown() && !e.AltDown() && suggest_key(code, e.ShiftDown())) return;
  if (pick_) {
    if (code == WXK_ALT || code == WXK_CONTROL || code == WXK_SHIFT || code == WXK_WINDOWS_LEFT ||
        code == WXK_WINDOWS_RIGHT) {
      e.Skip();  // a modifier alone keeps the list open
      return;
    }
    if (e.ControlDown() || e.AltDown() || code == WXK_LEFT || code == WXK_RIGHT || code == WXK_NUMPAD_LEFT ||
        code == WXK_NUMPAD_RIGHT) {
      close_picker();  // and the key does what it does on the cell
    } else {
      if (!pick_->key(code)) e.Skip();  // a character: filtered on wxEVT_CHAR
      return;
    }
  }
  if (!editing && col_type() >= 0 && GetGridCursorCol() == col_type() && kind_ != Kind::Path && !e.ControlDown()) {
    const bool alt_down = e.AltDown() && (code == WXK_DOWN || code == WXK_NUMPAD_DOWN);
    if (alt_down || (!e.AltDown() && !e.ShiftDown() &&
                     (code == WXK_RETURN || code == WXK_NUMPAD_ENTER || code == WXK_F2 || code == WXK_SPACE))) {
      open_picker(std::max(GetGridCursorRow(), 0));
      return;
    }
  }
  if (!editing && !e.AltDown() &&
      ((e.GetModifiers() == wxMOD_CONTROL && code == 'V') ||
       (e.GetModifiers() == wxMOD_SHIFT && (code == WXK_INSERT || code == WXK_NUMPAD_INSERT))) &&
      paste_into_cell())
    return;
  if (!editing && !e.AltDown() && code == 'Z' &&
      (e.GetModifiers() == wxMOD_CONTROL || e.GetModifiers() == (wxMOD_CONTROL | wxMOD_SHIFT))) {
    undo_step(e.ShiftDown());
    return;
  }
  if (!editing && e.GetModifiers() == wxMOD_CONTROL && code == 'Y') {
    undo_step(true);
    return;
  }
  if (e.ControlDown() && !(code == WXK_BACK && !editing)) {
    e.Skip();  // Ctrl+Enter sends, Ctrl+C copies, ...
    return;
  }
  if (kind_ == Kind::Path && !editing && !e.AltDown() &&
      (code == WXK_DELETE || code == WXK_NUMPAD_DELETE || code == WXK_SPACE || code == WXK_BACK)) {
    return;  // rows follow the URL: nothing to delete or toggle
  }
  if ((code == WXK_RETURN || code == WXK_NUMPAD_ENTER) && !e.AltDown()) {
    if (!editing && !e.ShiftDown()) {
      int r = std::max(GetGridCursorRow(), 0), c = GetGridCursorCol();
      if (kind_ == Kind::Path) {
        if (r < GetNumberRows()) go(r, col_value(), true);
        return;
      }
      if (c == 0) {  // Enter on the checkbox: toggle it
        if (!row_empty(r)) {
          SetCellValue(r, 0, row_enabled(r) ? "0" : "1");
          changed();
        }
        return;
      }
      if (c != col_key() && c != col_value() && c != col_type() && c != col_desc()) c = col_key();
      go(r, c, true);
      return;
    }
    nav_move(!e.ShiftDown(), true);
    return;
  }
  if (code == WXK_TAB && !e.AltDown()) {
    nav_move(!e.ShiftDown(), false);
    return;
  }
  if (!editing && !e.AltDown()) {
    const int r = GetGridCursorRow(), c = GetGridCursorCol();
    if (code == WXK_DELETE || code == WXK_NUMPAD_DELETE || (code == WXK_BACK && e.ControlDown())) {
      if (r >= 0 && r < GetNumberRows() - 1 && !row_empty(r)) {
        delete_row(r);
        go(std::min(r, GetNumberRows() - 1), c == col_value() ? col_value() : col_key(), false);
      }
      return;
    }
    if (code == WXK_SPACE && c == 0) {
      if (!row_empty(r)) {
        SetCellValue(r, 0, row_enabled(r) ? "0" : "1");
        changed();
      }
      return;
    }
    // Arrows move the cursor here: wxGrid first offers its keys to the parent chain, where the
    // shell's handlers could otherwise take them.
    const bool ext = e.ShiftDown();
    switch (code) {
      case WXK_UP: case WXK_NUMPAD_UP: MoveCursorUp(ext); return;
      case WXK_DOWN: case WXK_NUMPAD_DOWN: MoveCursorDown(ext); return;
      case WXK_LEFT: case WXK_NUMPAD_LEFT: MoveCursorLeft(ext); return;
      case WXK_RIGHT: case WXK_NUMPAD_RIGHT:
        if (c + 1 < col_del()) MoveCursorRight(ext);
        return;
      case WXK_F2:
        if (c > 0 && c < col_del()) go(std::max(r, 0), c, true);
        return;
      default: break;
    }
  }
  e.Skip();
}

void KvGrid::nav_move(bool forward, bool enter) {
  int r = std::max(GetGridCursorRow(), 0);
  const int c = GetGridCursorCol();
  // Commit first: this fires CELL_CHANGED, which appends the next blank row when needed.
  if (IsCellEditControlEnabled()) DisableCellEditControl();
  const int rows = GetNumberRows();
  if (r >= rows) r = rows - 1;
  if (rows == 0) return;
  // Tab visits every editable column in order; Enter only Key and Value.
  std::vector<int> path;
  for (int col = 1; col < col_del(); ++col) {
    if (kind_ == Kind::Path && col == col_key()) continue;
    if (enter && (col == col_type() || col == col_desc())) continue;
    path.push_back(col);
  }
  const int last = static_cast<int>(path.size()) - 1;
  const auto it = std::find(path.begin(), path.end(), c);
  int i;
  if (it != path.end()) {
    i = static_cast<int>(it - path.begin());
  } else {  // a column off the path (checkbox, Type on Enter, delete): between its neighbours
    const int before = static_cast<int>(std::count_if(path.begin(), path.end(), [c](int col) { return col < c; }));
    i = forward ? before - 1 : before;
  }
  if (forward) {
    if (enter && c == col_key() && row_empty(r) && kind_ != Kind::Path) {
      go(r, col_key(), true);  // Enter on the empty last row: nothing to commit, keep typing here
      return;
    }
    if (++i > last) {
      if (r + 1 < rows) {
        ++r;
        i = 0;
      } else {
        if (!enter) Navigate(wxNavigationKeyEvent::IsForward);  // Tab past the last (blank) row
        return;
      }
    }
  } else if (--i < 0) {
    if (r > 0) {
      --r;
      i = last;
    } else {
      if (!enter) Navigate(wxNavigationKeyEvent::IsBackward);
      return;
    }
  }
  const int nc = path[static_cast<size_t>(i)];
  go(r, nc, nc != col_type());
}

wxString KvGrid::blank_hint(int col) const {
  if (col == col_key()) {
    switch (kind_) {
      case Kind::Headers: return "Header";
      case Kind::Assert: return "res.status";
      case Kind::Capture: return "token";
      case Kind::PreVar: return "signature";
      default: return "Key";
    }
  }
  if (col == col_desc()) return kind_ == Kind::PreVar ? wxString() : wxString("Description");
  return kind_ == Kind::Assert    ? "200"
         : kind_ == Kind::Capture ? "res.body.access_token"
         : kind_ == Kind::PreVar  ? "hmacSha256({{secret}}, req.url)"
                                  : "Value";
}

void KvGrid::go(int r, int c, bool edit) {
  if (r < 0 || r >= GetNumberRows()) return;
  SetGridCursor(r, c);
  MakeCellVisible(r, c);
  if (!edit) return;
  // Open the editor once the old one has finished handling its key. A key typed before that
  // starts the editor itself (wxGrid opens it with that character), so nothing is lost.
  CallAfter([this, r, c] {
    if (IsCellEditControlEnabled()) return;
    if (GetGridCursorRow() != r || GetGridCursorCol() != c) return;
    wxWindow* f = wxWindow::FindFocus();
    if (!f || (f != this && !IsDescendant(f))) return;
    if (CanEnableCellControl()) EnableCellEditControl();
  });
}

void KvGrid::on_activate(wxActivateEvent& e) {
  e.Skip();
  if (!e.GetActive() || !kept_open_) return;
  // After the window has put the focus back where it was (its own activation handling).
  CallAfter([this] {
    if (!kept_open_) return;
    kept_open_ = false;
    if (!IsCellEditControlShown()) return;
    wxGridCellEditor* ed = GetCellEditor(GetGridCursorRow(), GetGridCursorCol());
    auto* tc = ed ? wxDynamicCast(ed->GetWindow(), wxTextCtrl) : nullptr;
    if (ed) ed->DecRef();
    if (!tc) return;
    wxWindow* f = wxWindow::FindFocus();
    if (f == tc) return;  // Windows gave it back, caret and all
    // The click that activated the window went to another control: the edit ends there.
    if (f && f != this && !IsDescendant(f) && f != wxGetTopLevelParent(this)) {
      DisableCellEditControl();
      return;
    }
    tc->SetFocus();
    const long n = tc->GetLastPosition();
    if (kept_from_ >= 0 && kept_to_ >= kept_from_ && kept_to_ <= n) tc->SetSelection(kept_from_, kept_to_);
    else tc->SetInsertionPointEnd();
  });
}

bool KvGrid::paste_into_cell() {
  const int r = GetGridCursorRow(), c = GetGridCursorCol();
  if (r < 0 || r >= GetNumberRows()) return false;
  if (c != col_key() && c != col_value() && c != col_desc()) return false;
  if (kind_ == Kind::Multipart && c == col_value() && GetCellValue(r, col_type()) == "File") return false;
  if (!CanEnableCellControl()) return false;
  wxString text;
  if (wxTheClipboard->Open()) {
    if (wxTheClipboard->IsSupported(wxDF_UNICODETEXT) || wxTheClipboard->IsSupported(wxDF_TEXT)) {
      wxTextDataObject d;
      if (wxTheClipboard->GetData(d)) text = d.GetText();
    }
    wxTheClipboard->Close();
  }
  if (text.empty()) return false;
  // A cell holds one line: a pasted block keeps its first line (as an <input> would join it,
  // the trailing newline of a copied token is dropped).
  text = text.BeforeFirst('\n');
  text.Trim(true);
  text.Replace("\r", "");
  EnableCellEditControl();
  wxGridCellEditor* ed = GetCellEditor(r, c);
  auto* tc = ed ? wxDynamicCast(ed->GetWindow(), wxTextCtrl) : nullptr;
  if (ed) ed->DecRef();
  if (!tc || !IsCellEditControlShown()) return false;
  tc->SetInsertionPointEnd();
  tc->WriteText(text);  // wxEVT_TEXT: the live text and the URL follow
  return true;
}

void KvGrid::commit_edit() {
  close_suggest();
  if (IsCellEditControlEnabled()) DisableCellEditControl();
}

void KvGrid::close_picker() {
  if (pick_) pick_->close();  // its on_close clears pick_
}

KvGrid::Suggest KvGrid::suggestions(int col, const wxString& text, long caret) const {
  Suggest out;
  if (caret < 0 || caret > static_cast<long>(text.length())) return out;
  const wxString before = text.Left(static_cast<size_t>(caret));
  const bool vars_here = (col == col_key() || col == col_value()) && kind_ != Kind::Capture;
  const size_t open = before.rfind("{{");
  if (vars_here && open != wxString::npos && before.find("}}", open) == wxString::npos) {
    wxString typed = before.Mid(open + 2);
    typed.Trim(false);
    if (typed.find_first_of(" {}\t") != wxString::npos) return out;  // not a name being typed
    const wxString lt = typed.Lower();
    std::vector<PickItem> pre, in;
    for (const auto& n : var_list_) {
      const wxString ln = n.Lower();
      wxString desc;
      if (lookup) {
        if (const auto v = lookup(n.utf8_string())) {
          desc = wxString::FromUTF8(v->substr(0, 40));
          desc.Replace("\n", " ");
          if (v->size() > 40) desc += wxString::FromUTF8("\xE2\x80\xA6");
        }
      }
      if (ln.StartsWith(lt)) pre.push_back({n, desc, ""});
      else if (!lt.empty() && ln.Find(lt) != wxNOT_FOUND) in.push_back({n, desc, ""});
    }
    out.items = std::move(pre);
    out.items.insert(out.items.end(), in.begin(), in.end());
    out.from = static_cast<long>(open);
    out.to = caret;
    if (text.Mid(static_cast<size_t>(caret)).StartsWith("}}")) out.to += 2;  // "{{us|}}": the closing braces stay one pair
    out.var = true;
    return out;
  }
  if (before.empty() || caret != static_cast<long>(text.length())) return out;
  // A header's values are keyed by its name (Content-Type: media types, Expect: 100-continue);
  // an unknown or X-* header gets none, so a typed "1" is never offered "100-continue".
  if (kind_ == Kind::Headers && col == col_value()) {
    const int row = GetGridCursorRow();
    if (row < 0 || row >= GetNumberRows()) return out;
    const std::string name = GetCellValue(row, col_key()).utf8_string();
    for (const auto& c : completions_for(header_value_choices(name), before.utf8_string()))
      out.items.push_back({wxString::FromUTF8(c), "", ""});
  } else {
    static const wxArrayString none;
    const wxArrayString& fixed = col == col_key() ? key_choices_ : col == col_value() ? value_choices_ : none;
    const wxString lp = before.Lower();
    for (const auto& c : fixed)  // strictly longer: what is typed is already there
      if (c.length() > lp.length() && c.Lower().StartsWith(lp)) out.items.push_back({c, "", ""});
  }
  if (out.items.empty()) return out;
  out.from = 0;
  out.to = static_cast<long>(text.length());
  return out;
}

void KvGrid::update_suggest(wxTextCtrl* tc) {
  if (!tc || !IsCellEditControlEnabled()) return close_suggest();
  const int row = GetGridCursorRow(), col = GetGridCursorCol();
  Suggest s = suggestions(col, tc->GetValue(), tc->GetInsertionPoint());
  if (s.items.empty()) return close_suggest();
  if (s.items.size() > 60) s.items.resize(60);
  PickPopup* p = suggest_;
  if (!p) {
    p = new PickPopup(this, {}, "");
    p->compact = true;
    suggest_ = p;
    p->on_close = [this, p] {
      if (suggest_ == p) suggest_ = nullptr;
    };
  }
  suggest_tc_ = tc;
  // A whole-cell list highlights nothing until Up / Down: Enter / Tab keep what was typed
  // ("X-Api-Version: 1" stays 1). A {{variable}} list takes its best match.
  p->no_preselect = !s.var;
  // A click picks (the popup never takes the focus, so the edit is still open).
  p->on_pick = [this, tc](const wxString& v) { insert_suggest(tc, v); };
  // Showing / resizing it can move the focus (and so close it, which clears suggest_): the
  // popup object itself stays valid until the next idle time, but is not used once closed.
  p->set_items(std::move(s.items), "");
  if (suggest_ != p) return;
  wxRect r = CellToRect(row, col);
  r.SetPosition(GetGridWindow()->ClientToScreen(CalcScrolledPosition(r.GetPosition())));
  p->show_below(r);
}

bool KvGrid::suggest_key(int k, bool shift) {
  if (!suggest_) return false;
  switch (k) {
    case WXK_UP: case WXK_NUMPAD_UP: case WXK_DOWN: case WXK_NUMPAD_DOWN:
    case WXK_PAGEUP: case WXK_NUMPAD_PAGEUP: case WXK_PAGEDOWN: case WXK_NUMPAD_PAGEDOWN:
      suggest_->key(k);
      return true;
    case WXK_ESCAPE:
      close_suggest();
      return true;
    case WXK_TAB: case WXK_RETURN: case WXK_NUMPAD_ENTER: {
      bool whole = false;
      wxString picked;
      if (!shift && accept_suggest(suggest_tc_, &whole, &picked)) {
        // A whole-cell entry the user chose with Up / Down: in, and the key commits the cell and
        // moves on as it would without the list (Enter: the next row's name after a value). One
        // that continues ("Bearer ") keeps the caret for the rest, as a {{variable}} does.
        if (whole && !choice_continues(picked.utf8_string())) nav_move(true, k != WXK_TAB);
        return true;
      }
      close_suggest();
      return false;  // nothing chosen: the cell keeps what was typed and the key moves on
    }
    case WXK_LEFT: case WXK_RIGHT: case WXK_NUMPAD_LEFT: case WXK_NUMPAD_RIGHT:
    case WXK_HOME: case WXK_END: case WXK_NUMPAD_HOME: case WXK_NUMPAD_END:
      close_suggest();
      return false;
    default: return false;
  }
}

bool KvGrid::accept_suggest(wxTextCtrl* tc, bool* whole, wxString* picked) {
  if (!tc) return false;
  if (!suggest_) return false;
  const wxString v = suggest_->selected_value();
  if (picked) *picked = v;
  if (v.empty()) return false;
  const Suggest s = suggestions(GetGridCursorCol(), tc->GetValue(), tc->GetInsertionPoint());
  const wxString text = tc->GetValue();
  if (s.items.empty() || s.to > static_cast<long>(text.length())) return false;
  const wxString repl = s.var ? "{{" + v + "}}" : v;
  const wxString after = text.Left(static_cast<size_t>(s.from)) + repl + text.Mid(static_cast<size_t>(s.to));
  if (after == text) return false;  // already that: Enter / Tab commit and move on
  if (whole) *whole = !s.var;
  insert_suggest(tc, v);
  return true;
}

void KvGrid::insert_suggest(wxTextCtrl* tc, const wxString& value) {
  close_suggest();
  if (!tc || !IsCellEditControlEnabled() || !tc->IsShown()) return;
  const Suggest s = suggestions(GetGridCursorCol(), tc->GetValue(), tc->GetInsertionPoint());
  if (s.items.empty() && !s.var) return;  // the text changed under the list: leave it alone
  const wxString repl = s.var ? "{{" + value + "}}" : value;
  inserting_ = true;
  tc->Replace(s.from, s.to, repl);
  tc->SetInsertionPoint(s.from + static_cast<long>(repl.length()));
  inserting_ = false;
  tc->SetFocus();
}

void KvGrid::close_suggest() {
  if (suggest_) suggest_->close();  // its on_close clears suggest_
}

void KvGrid::open_picker(int row, wxChar first) {
  if (col_type() < 0 || row < 0 || row >= GetNumberRows()) return;
  close_picker();
  if (IsCellEditControlEnabled()) DisableCellEditControl();
  std::vector<PickItem> items;
  const wxString cur_raw = GetCellValue(row, col_type());
  if (kind_ == Kind::Assert) {
    static const char* const ops[][3] = {
        {"eq", "equals (numbers numerically)", "Compare"}, {"neq", "not equal", "Compare"},
        {"gt", "greater than", "Compare"}, {"gte", "greater or equal", "Compare"}, {"lt", "less than", "Compare"},
        {"lte", "less or equal", "Compare"}, {"between", "lo, hi (inclusive)", "Compare"},
        {"in", "one of a, b, c", "Compare"}, {"notIn", "none of a, b, c", "Compare"},
        {"contains", "substring / element / key", "String"}, {"notContains", "does not contain", "String"},
        {"startsWith", "begins with", "String"}, {"endsWith", "ends with", "String"},
        {"matches", "regular expression", "String"}, {"notMatches", "regex does not match", "String"},
        {"length", "string / array / object size", "String"},
        {"isString", "", "Type"}, {"isNumber", "", "Type"}, {"isBoolean", "", "Type"}, {"isArray", "", "Type"},
        {"isJson", "parses as JSON", "Type"},
        {"isDefined", "present", "Existence"}, {"isUndefined", "missing", "Existence"},
        {"isNull", "null", "Existence"}, {"isEmpty", "\"\", [] or {}", "Existence"},
        {"isNotEmpty", "", "Existence"}, {"isTruthy", "", "Existence"},
        {"isFalsy", "false, 0, \"\", null", "Existence"}};
    for (const auto& o : ops) items.push_back({o[0], o[1], o[2]});
    // an operator this list does not know (from a file) is still offered
    if (!cur_raw.empty() && std::none_of(items.begin(), items.end(), [&](const PickItem& i) { return i.value == cur_raw; }))
      items.push_back({cur_raw, "", "Other"});
  } else if (kind_ == Kind::Capture || kind_ == Kind::PreVar) {
    items = {{"runtime", "this session only", ""},
             {"environment", "saved in the active environment", ""},
             {"globals", "saved in globals", ""}};
  } else {
    items = {{"Text", "a text value", ""}, {"File", "a file, read when sent", ""}};
  }
  wxString cur = kind_ == Kind::Multipart ? wxString(cur_raw == "File" ? "File" : "Text") : cur_raw;
  if (cur.empty()) cur = wxString::FromUTF8(default_type());
  auto* p = new PickPopup(this, std::move(items), cur);
  pick_ = p;
  pick_row_ = row;
  p->on_close = [this, p] {
    if (pick_ != p) return;
    pick_ = nullptr;
    pick_row_ = -1;
  };
  p->on_pick = [this, row, p](const wxString& v) {
    if (row >= GetNumberRows()) return;
    const bool tab = p->picked_with == WXK_TAB;
    const wxString old = GetCellValue(row, col_type());
    SetCellValue(row, col_type(), v);
    if (kind_ == Kind::Multipart && v != old) SetCellValue(row, col_value(), "");
    if (!row_empty(row) && GetCellValue(row, 0).empty()) SetCellValue(row, 0, "1");
    ensure_blank_row();
    if (v != old) changed();  // the same entry picked again: nothing changed
    SetFocus();
    if (kind_ == Kind::Multipart && v == "File") {
      CallAfter([this, row] { choose_file(row); });
    } else {
      // Tab picks and moves on to the expected value, as Tab in Target does ("lt, Tab, 9" types 9
      // into Expected). Enter picks and stays (moving on as well made "cont, Enter, Tab" land in
      // Description).
      if (tab && col_value() > col_type()) go(row, col_value(), true);
      else go(row, col_type(), false);
    }
  };
  wxRect r = CellToRect(row, col_type());
  r.SetPosition(GetGridWindow()->ClientToScreen(CalcScrolledPosition(r.GetPosition())));
  if (first) p->chr(first);
  p->show_below(r);
}

void KvGrid::update_tooltip(int row, int col) {
  if (row == tip_row_ && col == tip_col_) return;
  tip_row_ = row;
  tip_col_ = col;
  wxString tip;
  if (row >= 0 && col >= 0) {
    if (kind_ == Kind::Path && col == col_key() && !row_empty(row)) {
      tip = wxString::FromUTF8(":" + cell(row, col) + " in the URL is replaced by the value (URL-encoded)");
    } else if (kind_ == Kind::Path && (col == 0 || col == col_del())) {
      // no checkbox, no delete: the rows follow the URL
    } else if (col == col_del() && !row_empty(row)) {
      tip = "Remove this row";
    } else if (col == 0 && !row_empty(row)) {
      tip = row_enabled(row) ? "Sent (click to disable)" : "Not sent (click to enable)";
    } else if (col == col_desc() && kind_ == Kind::PreVar && !row_empty(row)) {
      const std::string var = std::string(trim(cell(row, col_key())));
      const auto pv = preview && !var.empty() ? preview(var) : std::nullopt;
      if (!pv) {
        // no preview (yet): the cell text, when it is cut
        const std::string s = cell(row, col);
        if (!s.empty() && GetTextExtent(wxString::FromUTF8(s)).x > GetColSize(col) - FromDIP(16))
          tip = wxString::FromUTF8(s.size() > 600 ? s.substr(0, 600) + "..." : s);
      } else {
        std::string shown = pv->text;
        if (shown.size() > 600) shown = shown.substr(0, 600) + "...";
        tip = wxString::FromUTF8(pv->ok ? "{{" + var + "}} = " + shown + "\n(computed now; the send computes it again)"
                                        : shown);
      }
    } else if (col == col_desc() && !row_empty(row)) {
      const std::string s = cell(row, col);
      tip = s.empty() ? wxString("A note about this row, saved with the request and never sent") : wxString::FromUTF8(s);
    } else if (col == col_key() || col == col_value()) {
      const std::string s = cell(row, col);
      if (check && !row_empty(row))
        if (const std::string p = check(col, s); !p.empty()) tip = wxString::FromUTF8(p);
      if (tip.empty() && kind_ != Kind::Capture)
        if (const std::string p = brace_problem(s); !p.empty()) tip = wxString::FromUTF8(p);
      // A text cut in the cell: the whole of it in the tooltip (above a warning about it).
      if (!s.empty() && GetTextExtent(wxString::FromUTF8(s)).x > GetColSize(col) - FromDIP(16)) {
        const wxString whole = wxString::FromUTF8(s.size() > 600 ? s.substr(0, 600) + "..." : s);
        tip = tip.empty() ? whole : whole + "\n" + tip;
      }
      for (const auto& v : find_vars(s)) {
        if (!tip.empty()) tip += "\n";
        const auto val = lookup ? lookup(v.name) : std::nullopt;
        if (val) {
          std::string shown = *val;
          if (shown.size() > 200) shown = shown.substr(0, 200) + "...";
          tip += wxString::FromUTF8("{{" + v.name + "}} = " + shown);
        } else {
          tip += wxString::FromUTF8("{{" + v.name + "}} is not defined in the active environment or globals");
        }
      }
    }
  }
  if (tip.empty()) GetGridWindow()->UnsetToolTip();
  else GetGridWindow()->SetToolTip(tip);
}

}  // namespace watt::app::http_editor
