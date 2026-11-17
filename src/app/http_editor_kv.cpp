#include "app/http_editor_kv.h"

#include <wx/arrstr.h>
#include <wx/dc.h>
#include <wx/filedlg.h>
#include <wx/renderer.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textcompleter.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <mutex>

#include "app/http_editor_widgets.h"

namespace watt::app::http_editor {

namespace {

const char* const kHeaderNames[] = {
    "Accept", "Accept-Charset", "Accept-Encoding", "Accept-Language", "Authorization", "Cache-Control",
    "Connection", "Content-Disposition", "Content-Encoding", "Content-Language", "Content-Length",
    "Content-Type", "Cookie", "DNT", "Date", "Expect", "Forwarded", "From", "Host", "If-Match",
    "If-Modified-Since", "If-None-Match", "If-Range", "If-Unmodified-Since", "Idempotency-Key",
    "Keep-Alive", "Max-Forwards", "Origin", "Pragma", "Prefer", "Proxy-Authorization", "Range", "Referer",
    "TE", "Traceparent", "Tracestate", "Upgrade", "User-Agent", "Via", "X-API-Key", "X-Correlation-ID",
    "X-CSRF-Token", "X-Forwarded-For", "X-Forwarded-Host", "X-Forwarded-Proto", "X-HTTP-Method-Override",
    "X-Request-ID", "X-Requested-With"};

const char* const kHeaderValues[] = {
    "application/json", "application/json; charset=utf-8", "application/xml", "application/x-www-form-urlencoded",
    "application/octet-stream", "application/grpc-web+proto", "multipart/form-data", "text/plain", "text/html",
    "text/csv", "*/*", "gzip, deflate, br", "gzip", "identity", "no-cache", "no-store", "max-age=0",
    "keep-alive", "close", "Bearer ", "Basic ", "en-US,en;q=0.9", "XMLHttpRequest", "100-continue",
    "return=representation", "return=minimal"};

}  // namespace

std::string canonical_header_name(std::string_view name) {
  std::string n = clean_header_name(name);
  for (const char* known : kHeaderNames)
    if (iequals(n, known)) return known;
  return n;
}

// {{var}} names for the cell editors' completer, which MSW calls on a worker thread.
struct VarSnapshot {
  std::mutex m;
  wxArrayString names;
};

namespace {

// Completion of a cell: "{{" opens the {{var}} list (the text before it kept), otherwise the
// fixed choices (header names / values).
class CellCompleter : public wxTextCompleterSimple {
 public:
  CellCompleter(wxArrayString fixed, std::shared_ptr<VarSnapshot> vars) : fixed_(std::move(fixed)), vars_(std::move(vars)) {}
  void GetCompletions(const wxString& prefix, wxArrayString& res) override {
    const size_t open = prefix.rfind("{{");
    if (open != wxString::npos && prefix.find("}}", open) == wxString::npos) {
      wxArrayString names;
      if (vars_) {
        std::lock_guard<std::mutex> lock(vars_->m);
        names = vars_->names;
      }
      const wxString head = prefix.Left(open);
      const wxString typed = prefix.Mid(open + 2).Lower();
      for (const auto& n : names)
        if (n.Lower().StartsWith(typed)) res.Add(head + "{{" + n + "}}");
      return;
    }
    if (prefix.empty()) return;
    const wxString lp = prefix.Lower();
    for (const auto& c : fixed_)
      if (c.Lower().StartsWith(lp)) res.Add(c);
  }

 private:
  wxArrayString fixed_;
  std::shared_ptr<VarSnapshot> vars_;
};

wxArrayString to_array(const char* const* b, const char* const* e) {
  wxArrayString a;
  for (auto* p = b; p != e; ++p) a.Add(*p);
  return a;
}

// Text editor with inline autocompletion (wxTextEntry::AutoComplete).
class CompletingEditor : public wxGridCellTextEditor {
 public:
  CompletingEditor(wxArrayString choices, std::shared_ptr<VarSnapshot> vars)
      : choices_(std::move(choices)), vars_(std::move(vars)) {}
  void Create(wxWindow* parent, wxWindowID id, wxEvtHandler* handler) override {
    wxGridCellTextEditor::Create(parent, id, handler);
    Text()->AutoComplete(new CellCompleter(choices_, vars_));
  }
  wxGridCellEditor* Clone() const override { return new CompletingEditor(choices_, vars_); }
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

 private:
  wxArrayString choices_;
  std::shared_ptr<VarSnapshot> vars_;
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
    if (col == g_->col_type()) {
      if (blank) return;
      dc.SetTextForeground(t.dim);
      dc.DrawText(file_row ? "File" : "Text", x, ty);
      return;
    }
    if (s.empty()) {
      wxString ph;
      if (blank) {
        if (col == g_->col_key()) ph = g_->kind() == KvGrid::Kind::Headers ? "Header" : "Key";
        else ph = "Value";
      } else if (path && col == g_->col_value()) {
        // Sent as ":name" otherwise: the send is refused until it has a value.
        dc.SetTextForeground(t.var_bad);
        dc.DrawText(wxString::FromUTF8("\xE2\x9A\xA0 value required"), x, ty);
        return;
      } else if (col == g_->col_value() && file_row) {
        ph = wxString::FromUTF8("Choose a file\xE2\x80\xA6");
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
    const wxColour fg = enabled && !(path && col == g_->col_key()) ? t.fg : t.dim;
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
      kind_(kind),
      vars_(std::make_shared<VarSnapshot>()) {
  const Theme& t = Theme::get();
  const int ncols = kind == Kind::Multipart ? 5 : 4;
  CreateGrid(1, ncols);
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
  SetColLabelValue(col_key(), kind == Kind::Headers ? "Header" : "Key");
  SetColLabelValue(col_value(), "Value");
  SetColLabelValue(col_del(), "");
  if (col_type() >= 0) SetColLabelValue(col_type(), "Type");

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
    a->SetEditor(new CompletingEditor(kind == Kind::Headers ? to_array(std::begin(kHeaderNames), std::end(kHeaderNames))
                                                            : wxArrayString(),
                                      vars_));
    SetColAttr(col_key(), a);
  }
  {
    auto* a = new wxGridCellAttr;
    a->SetEditor(new CompletingEditor(kind == Kind::Headers
                                          ? to_array(std::begin(kHeaderValues), std::end(kHeaderValues))
                                          : wxArrayString(),
                                      vars_));
    SetColAttr(col_value(), a);
  }
  if (col_type() >= 0) {
    auto* a = new wxGridCellAttr;
    wxArrayString kinds;
    kinds.Add("Text");
    kinds.Add("File");
    a->SetEditor(new wxGridCellChoiceEditor(kinds));
    SetColAttr(col_type(), a);
  }

  Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
    e.Skip();
    layout_columns();
  });

  Bind(wxEVT_GRID_CELL_LEFT_CLICK, [this](wxGridEvent& e) {
    const int r = e.GetRow(), c = e.GetCol();
    if (kind_ == Kind::Path && (c == 0 || c == col_del() || c == col_key())) {
      go(r, col_value(), true);  // only the value is editable
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
    if (!var_names) return;
    wxArrayString names;
    for (const auto& n : var_names()) names.Add(wxString::FromUTF8(n));
    std::lock_guard<std::mutex> lock(vars_->m);
    vars_->names = std::move(names);
  });
  Bind(wxEVT_GRID_EDITOR_CREATED, [this](wxGridEditorCreatedEvent& e) {
    e.Skip();
    if (auto* tc = wxDynamicCast(e.GetControl(), wxTextCtrl)) {
      // Ahead of the MSW autocompletion, which would take Tab to accept a suggestion (leaving the
      // focus in the cell) or insert it as a character.
      // Same for Enter, which the autocompletion eats when its list is closed.
      tc->Bind(wxEVT_CHAR_HOOK, [this, tc](wxKeyEvent& ke) {
        const int k = ke.GetKeyCode();
        if (!ke.ControlDown() && !ke.AltDown()) {
          if (k == WXK_TAB || k == WXK_RETURN || k == WXK_NUMPAD_ENTER) {
            // The suggestion appended inline (selected, after the caret) is not taken by Tab /
            // Enter: "X" Tab stays "X", not "X-API-Key". Right / End accept it; a name picked
            // from the list with the arrows has no selection and is kept whole.
            long from = 0, to = 0;
            tc->GetSelection(&from, &to);
            if (from > 0 && to > from && to == tc->GetLastPosition()) tc->Remove(from, to);
          }
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
        live_row_ = GetGridCursorRow();
        live_col_ = GetGridCursorCol();
        live_text_ = tc->GetValue();
        // The new blank row is added when the edit ends: changing the table while the editor
        // is open would close it and lose what was typed.
        changed();
      });
    }
  });
  Bind(wxEVT_GRID_EDITOR_HIDDEN, [this](wxGridEvent& e) {
    e.Skip();
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
    live_row_ = live_col_ = -1;
    live_text_.clear();
    if (kind_ == Kind::Headers && (e.GetCol() == col_key() || e.GetCol() == col_value())) {
      const std::string v = GetCellValue(r, e.GetCol()).utf8_string();
      std::string c = e.GetCol() == col_key() ? canonical_header_name(v) : clean_header_value(v);
      if (c != v) SetCellValue(r, e.GetCol(), wxString::FromUTF8(c));
    }
    if (!row_empty(r) && GetCellValue(r, 0).empty()) SetCellValue(r, 0, "1");
    if (kind_ == Kind::Multipart && e.GetCol() == col_type()) SetCellValue(r, col_value(), "");
    ensure_blank_row();
    changed();
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
         !(col_type() >= 0 && GetCellValue(row, col_type()) == "File");
}

void KvGrid::ensure_blank_row() {
  if (kind_ == Kind::Path) return;
  const int n = GetNumberRows();
  if (n == 0 || !row_empty(n - 1)) {
    AppendRows(1);
    if (col_type() >= 0) SetCellValue(GetNumberRows() - 1, col_type(), "Text");
  }
}

void KvGrid::layout_columns() {
  const int w = GetClientSize().x;
  if (w <= 0) return;
  const int chk = FromDIP(34), del = FromDIP(34), type = col_type() >= 0 ? FromDIP(72) : 0;
  const int rest = std::max(FromDIP(120), w - chk - del - type);
  const int key = rest * 2 / 5;
  BeginBatch();
  SetColSize(0, chk);
  SetColSize(col_key(), key);
  if (col_type() >= 0) SetColSize(col_type(), type);
  SetColSize(col_value(), rest - key);
  SetColSize(col_del(), del);
  EndBatch();
}

void KvGrid::changed() {
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
      if (col_type() >= 0) SetCellValue(r, col_type(), kv.file ? "File" : "Text");
    } else {
      for (int c = 0; c < GetNumberCols(); ++c) SetCellValue(r, c, "");
      if (col_type() >= 0) SetCellValue(r, col_type(), "Text");
    }
  }
  EndBatch();
  updating_ = false;
  tip_row_ = tip_col_ = -2;
  if (GetNumberRows() > 0 && GetGridCursorCol() != col_key() && GetGridCursorCol() != col_value() &&
      GetGridCursorCol() != col_type())
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
    kv.file = col_type() >= 0 && GetCellValue(r, col_type()) == "File";
    out.push_back(std::move(kv));
  }
  return out;
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
  if (kind == KvGrid::Kind::Path) toggle_->Hide();  // the rows come from the URL
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
    const Theme& t = Theme::get();
    text_ = new wxTextCtrl(book_, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                           wxTE_MULTILINE | wxTE_DONTWRAP | wxTE_RICH2 | wxBORDER_SIMPLE);
    wxFont f(wxFontInfo(GetFont().GetPointSize()).Family(wxFONTFAMILY_TELETYPE).FaceName("Consolas"));
    text_->SetFont(f);
    text_->SetBackgroundColour(t.field);
    text_->SetForegroundColour(t.fg);
    text_->SetHint(grid_->kind() == KvGrid::Kind::Headers ? "Content-Type: application/json" : "key: value");
    book_->AddPage(text_, "bulk");
    text_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { text_changed(); });
  }
  if (on) refresh_text();
  book_->ChangeSelection(on ? 1 : 0);
  toggle_->SetLabel(on ? "Key-Value Edit" : "Bulk Edit");
  Layout();
  if (on) {
    text_->SetFocus();
    text_->SetInsertionPointEnd();
  } else {
    grid_->SetFocus();
  }
}

void KvPanel::refresh_text() {
  std::vector<KvItem> items;
  for (const auto& r : grid_->rows()) items.push_back({r.key, r.value, r.enabled, false});
  text_->ChangeValue(wxString::FromUTF8(to_bulk(items)));
}

void KvPanel::text_changed() {
  std::vector<KvRow> rows;
  for (auto& it : parse_bulk(text_->GetValue().utf8_string())) {
    if (grid_->kind() == KvGrid::Kind::Headers) {
      it.key = canonical_header_name(it.key);
      it.value = clean_header_value(it.value);
    }
    rows.push_back({std::move(it.key), std::move(it.value), it.enabled, false, false});
  }
  from_text_ = true;
  grid_->set_rows(rows);
  from_text_ = false;
  if (grid_->on_change) grid_->on_change();
}

void KvGrid::on_key(wxKeyEvent& e) {
  const int code = e.GetKeyCode();
  const bool editing = IsCellEditControlEnabled();
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
      if (c != col_key() && c != col_value() && c != col_type()) c = col_key();
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
  std::vector<int> path;
  if (kind_ != Kind::Path) path.push_back(col_key());
  if (!enter && col_type() >= 0) path.push_back(col_type());
  path.push_back(col_value());
  const int last = static_cast<int>(path.size()) - 1;
  const auto it = std::find(path.begin(), path.end(), c);
  int i;
  if (it != path.end()) i = static_cast<int>(it - path.begin());
  else if (c == col_type()) i = forward ? 0 : 1;  // Enter on Type: between Key and Value
  else if (c == 0 || c == col_key()) i = forward ? -1 : 0;  // the checkbox, or a Path key
  else i = last;                                   // the delete column: after Value
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
    } else if (col == col_key() || col == col_value()) {
      const std::string s = cell(row, col);
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
