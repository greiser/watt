#include "app/env_editor.h"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dc.h>
#include <wx/dialog.h>
#include <wx/grid.h>
#include <wx/vlbox.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <set>
#include <vector>

#include "app/sidebar_style.h"
#include "core/model.h"

namespace watt::app {

namespace {

using sidebar::dim_colour;
using sidebar::is_dark;
using sidebar::mix;
using sidebar::text_colour;
using sidebar::window_colour;

constexpr char kUnreadableText[] = "cannot decrypt on this machine, re-enter value";
constexpr int kColSecret = 0, kColName = 1, kColValue = 2;

wxColour warn_colour() { return is_dark() ? wxColour(0xF0, 0xA0, 0x3C) : wxColour(0xA0, 0x4F, 0x00); }

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
  return s.substr(b, e - b);
}

struct VarRow {
  std::string name, value;
  bool secret = false;
  bool unreadable = false;  // value could not be decrypted here; `stored` is the sealed value
  std::string stored;
};

struct EnvModel {
  std::string name;
  std::string orig_name;  // empty for a new environment
  bool globals = false;
  // The session's runtime variables (Workspace::runtime_vars: captures, scripts, pre-request
  // variables). Memory only: Save sets them, nothing is written to disk.
  bool runtime = false;
  bool fixed() const { return globals || runtime; }  // no name, no delete, no secrets
  std::vector<VarRow> vars;
  size_t unreadable() const {
    size_t n = 0;
    for (const auto& v : vars) n += v.unreadable && v.value.empty();
    return n;
  }
};

class EnvDialog;

// Flat column headers. The default renderer paints the current cell's column header in a light
// "highlighted" blue, which in the dark theme hides the dim label text.
class FlatHeader : public wxGridColumnHeaderRenderer {
 public:
  void DrawBorder(const wxGrid& grid, wxDC& dc, wxRect& rect) const override {
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(grid.GetLabelBackgroundColour()));
    dc.DrawRectangle(rect);
    dc.SetPen(wxPen(grid.GetGridLineColour()));
    dc.DrawLine(rect.GetLeft(), rect.GetBottom(), rect.GetRight() + 1, rect.GetBottom());
    dc.DrawLine(rect.GetRight(), rect.GetTop(), rect.GetRight(), rect.GetBottom());
    rect.Deflate(grid.FromDIP(6), 0);
  }
  void DrawHighlighted(const wxGrid& grid, wxDC& dc, wxRect& rect, int, int) const override {
    DrawBorder(grid, dc, rect);
  }
};

class FlatHeaderProvider : public wxGridCellAttrProvider {
 public:
  const wxGridColumnHeaderRenderer& GetColumnHeaderRenderer(int) override { return header_; }

 private:
  FlatHeader header_;
};

// Value column: secret values as bullets (unless "Show values"), unreadable ones as a warning.
class ValueRenderer : public wxGridCellStringRenderer {
 public:
  explicit ValueRenderer(const EnvDialog* d) : d_(d) {}
  void Draw(wxGrid& grid, wxGridCellAttr& attr, wxDC& dc, const wxRect& rect, int row, int col,
            bool selected) override;
  wxGridCellRenderer* Clone() const override { return new ValueRenderer(d_); }

 private:
  const EnvDialog* d_;
};

// Text editor in a password field, for secret values.
class SecretEditor : public wxGridCellTextEditor {
 public:
  void Create(wxWindow* parent, wxWindowID id, wxEvtHandler* evt) override { DoCreate(parent, id, evt, wxTE_PASSWORD); }
  wxGridCellEditor* Clone() const override { return new SecretEditor; }
};

// Environment list. Owner drawn: the native list box draws its selection unreadably in dark mode.
class EnvList : public wxVListBox {
 public:
  explicit EnvList(wxWindow* parent, const wxSize& size)
      : wxVListBox(parent, wxID_ANY, wxDefaultPosition, size, wxBORDER_THEME | wxLB_SINGLE) {
    SetBackgroundColour(window_colour());
  }
  void Clear() {
    items_.clear();
    SetItemCount(0);
    Refresh();
  }
  void Append(const wxString& s) {
    items_.push_back(s);
    SetItemCount(items_.size());
    Refresh();
  }
  wxString GetString(int i) const { return i >= 0 && i < static_cast<int>(items_.size()) ? items_[i] : wxString(); }
  void SetString(int i, const wxString& s) {
    if (i < 0 || i >= static_cast<int>(items_.size())) return;
    items_[i] = s;
    RefreshRow(i);
  }
  void Delete(int i) {
    if (i < 0 || i >= static_cast<int>(items_.size())) return;
    items_.erase(items_.begin() + i);
    SetItemCount(items_.size());
    Refresh();
  }

 protected:
  wxCoord OnMeasureItem(size_t) const override { return FromDIP(28); }
  void OnDrawBackground(wxDC& dc, const wxRect& rect, size_t n) const override {
    if (!IsSelected(n)) return;
    dc.SetBrush(wxBrush(sidebar::highlight_colour()));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(rect);
  }
  void OnDrawItem(wxDC& dc, const wxRect& rect, size_t n) const override {
    wxString s = items_[n];
    const wxString mark = wxString::FromUTF8("  \xE2\x9A\xA0");
    const bool warn = s.EndsWith(mark);
    if (warn) s.RemoveLast(mark.size());
    wxFont f = GetFont();
    if (n == 0) f.SetStyle(wxFONTSTYLE_ITALIC);
    dc.SetFont(f);
    dc.SetTextForeground(text_colour());
    const int x = rect.x + FromDIP(10);
    const int y = rect.y + (rect.height - dc.GetCharHeight()) / 2;
    dc.DrawText(s, x, y);
    if (warn) {
      dc.SetTextForeground(warn_colour());
      dc.DrawText(wxString::FromUTF8("\xE2\x9A\xA0"), x + dc.GetTextExtent(s).x + FromDIP(8), y);
    }
  }

 private:
  std::vector<wxString> items_;
};

class EnvDialog : public wxDialog {
 public:
  EnvDialog(wxWindow* parent, Workspace& ws, const std::string& select)
      : wxDialog(parent, wxID_ANY, "Environments", wxDefaultPosition, wxDefaultSize,
                 wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
        ws_(ws) {
    load_models();
    build_ui();
    int first = 0;
    const std::string want = select.empty() ? ws_.active_env : select;
    for (size_t i = 0; i < envs_.size(); ++i)
      if (!envs_[i].fixed() && envs_[i].name == want) first = static_cast<int>(i);
    refresh_list();
    list_->SetSelection(first);
    show_env(first);
    SetSize(FromDIP(wxSize(920, 580)));
    CentreOnParent();
  }

  bool show_values() const { return show_->GetValue(); }
  bool row_secret(int row) const { return row >= 0 && row < static_cast<int>(secret_.size()) && secret_[row]; }
  bool row_unreadable(int row) const {
    return row >= 0 && row < static_cast<int>(unreadable_.size()) && unreadable_[row];
  }

  bool saved = false;

 private:
  // ---------------------------------------------------------------- model

  void load_models() {
    EnvModel g;
    g.name = "Globals";
    g.globals = true;
    for (const auto& [k, v] : ws_.globals) g.vars.push_back({k, v, false, false, {}});
    envs_.push_back(std::move(g));
    EnvModel rt;
    rt.name = "Runtime";
    rt.runtime = true;
    for (const auto& [k, v] : ws_.runtime_vars) rt.vars.push_back({k, v, false, false, {}});
    envs_.push_back(std::move(rt));
    for (const auto& e : ws_.environments()) {
      EnvModel m;
      m.name = m.orig_name = e.name;
      std::set<std::string> seen;
      for (const auto& [k, v] : e.vars) {
        m.vars.push_back({k, v, e.secrets.count(k) > 0, false, {}});
        seen.insert(k);
      }
      for (const auto& [k, stored] : e.unreadable_secrets)
        if (!seen.count(k)) {
          m.vars.push_back({k, "", true, true, stored});
          seen.insert(k);
        }
      for (const auto& k : e.secrets)
        if (!seen.count(k)) m.vars.push_back({k, "", true, false, {}});
      std::stable_sort(m.vars.begin(), m.vars.end(), [](const VarRow& a, const VarRow& b) { return a.name < b.name; });
      envs_.push_back(std::move(m));
    }
  }

  // ---------------------------------------------------------------- UI

  void build_ui() {
    SetBackgroundColour(window_colour());
    auto* top = new wxBoxSizer(wxVERTICAL);

    banner_ = new wxStaticText(this, wxID_ANY, "");
    banner_->SetForegroundColour(warn_colour());
    top->Add(banner_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    auto* body = new wxBoxSizer(wxHORIZONTAL);
    // Left: environment list.
    auto* left = new wxBoxSizer(wxVERTICAL);
    list_ = new EnvList(this, FromDIP(wxSize(200, -1)));
    left->Add(list_, 1, wxEXPAND);
    auto* lbtn = new wxBoxSizer(wxHORIZONTAL);
    auto* add = new wxButton(this, wxID_ANY, "New", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    auto* dup = new wxButton(this, wxID_ANY, "Duplicate", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    del_env_ = new wxButton(this, wxID_ANY, "Delete", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    lbtn->Add(add, 1, wxRIGHT, FromDIP(4));
    lbtn->Add(dup, 1, wxRIGHT, FromDIP(4));
    lbtn->Add(del_env_, 1);
    left->Add(lbtn, 0, wxEXPAND | wxTOP, FromDIP(6));
    body->Add(left, 0, wxEXPAND | wxRIGHT, FromDIP(12));

    // Right: name + variables.
    auto* right = new wxBoxSizer(wxVERTICAL);
    auto* namerow = new wxBoxSizer(wxHORIZONTAL);
    name_label_ = new wxStaticText(this, wxID_ANY, "Name");
    name_ = new wxTextCtrl(this, wxID_ANY);
    show_ = new wxCheckBox(this, wxID_ANY, "Show secret values");
    namerow->Add(name_label_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    namerow->Add(name_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(16));
    namerow->Add(show_, 0, wxALIGN_CENTER_VERTICAL);
    right->Add(namerow, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    runtime_note_ = new wxStaticText(this, wxID_ANY,
                                     "Set this session by captures, scripts and pre-request variables (e.g. a captured "
                                     "{{tok}}). Highest precedence, above the environment. Kept in memory only: "
                                     "never written to disk; edits here apply until watt closes.");
    runtime_note_->SetForegroundColour(dim_colour());
    runtime_note_->Wrap(FromDIP(560));
    runtime_note_->Hide();
    right->Add(runtime_note_, 0, wxEXPAND | wxBOTTOM, FromDIP(8));

    grid_ = new wxGrid(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_THEME);
    grid_->CreateGrid(0, 3);
    grid_->GetTable()->SetAttrProvider(new FlatHeaderProvider);
    grid_->HideRowLabels();
    grid_->SetColLabelValue(kColSecret, "Secret");
    grid_->SetColLabelValue(kColName, "Variable");
    grid_->SetColLabelValue(kColValue, "Value");
    grid_->SetColLabelAlignment(wxALIGN_LEFT, wxALIGN_CENTER);
    grid_->SetDefaultRowSize(FromDIP(26), true);
    grid_->SetColLabelSize(FromDIP(26));
    grid_->DisableDragRowSize();
    grid_->SetSelectionMode(wxGrid::wxGridSelectRows);
    grid_->SetDefaultCellBackgroundColour(window_colour());
    grid_->SetDefaultCellTextColour(text_colour());
    grid_->SetGridLineColour(mix(window_colour(), text_colour(), 0.12));
    grid_->SetLabelBackgroundColour(mix(window_colour(), text_colour(), 0.05));
    grid_->SetLabelTextColour(dim_colour());
    grid_->SetCellHighlightColour(mix(window_colour(), text_colour(), 0.4));
    grid_->SetSelectionBackground(sidebar::highlight_colour());
    grid_->SetSelectionForeground(text_colour());
    {
      auto* a = new wxGridCellAttr;
      a->SetRenderer(new wxGridCellBoolRenderer);
      a->SetEditor(new wxGridCellBoolEditor);
      a->SetAlignment(wxALIGN_CENTER, wxALIGN_CENTER);
      grid_->SetColAttr(kColSecret, a);
      auto* v = new wxGridCellAttr;
      v->SetRenderer(new ValueRenderer(this));
      grid_->SetColAttr(kColValue, v);
    }
    grid_->SetColSize(kColSecret, FromDIP(56));
    grid_->SetColSize(kColName, FromDIP(200));
    right->Add(grid_, 1, wxEXPAND);

    hint_ = new wxStaticText(this, wxID_ANY,
                             "Use as {{name}}. Secret values are encrypted for your Windows account (DPAPI) in "
                             ".watt/secrets.json, never in environments/*.json, so they stay out of git.");
    hint_->SetForegroundColour(dim_colour());
    right->Add(hint_, 0, wxEXPAND | wxTOP, FromDIP(8));
    body->Add(right, 1, wxEXPAND);
    top->Add(body, 1, wxEXPAND | wxALL, FromDIP(12));

    auto* btns = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
    if (auto* ok = FindWindow(wxID_OK)) static_cast<wxButton*>(ok)->SetLabel("Save");
    top->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    SetSizer(top);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      CallAfter([this] {
        fit_columns();
        const int w = grid_->GetSize().x;
        if (w > 0 && w != hint_wrap_) {
          hint_wrap_ = w;
          hint_->SetLabel("Use as {{name}}. Secret values are encrypted for your Windows account (DPAPI) in "
                          ".watt/secrets.json, never in environments/*.json, so they stay out of git.");
          hint_->Wrap(w);
          Layout();
        }
      });
    });

    list_->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) {
      const int s = list_->GetSelection();
      if (s == wxNOT_FOUND || s == cur_) return;
      store_env();
      show_env(s);
    });
    add->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { new_env(false); });
    dup->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { new_env(true); });
    del_env_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { delete_env(); });
    name_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
      if (cur_ <= 0 || loading_) return;
      envs_[cur_].name = name_->GetValue().utf8_string();
      update_list_label(cur_);
    });
    show_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
      grid_->DisableCellEditControl();
      apply_row_editors();
      grid_->ForceRefresh();
    });
    // One click toggles "Secret" (the default needs a click to select, another to edit).
    grid_->Bind(wxEVT_GRID_CELL_LEFT_CLICK, [this](wxGridEvent& e) {
      if (e.GetCol() != kColSecret || envs_[cur_].fixed()) {
        e.Skip();
        return;
      }
      const int r = e.GetRow();
      grid_->SetGridCursor(r, kColSecret);
      set_secret(r, !row_secret(r));
    });
    grid_->Bind(wxEVT_GRID_EDITOR_SHOWN, [this](wxGridEvent& e) {
      if (e.GetCol() == kColSecret) {
        e.Veto();
        if (!envs_[cur_].fixed()) set_secret(e.GetRow(), !row_secret(e.GetRow()));
      } else {
        e.Skip();
      }
    });
    grid_->Bind(wxEVT_GRID_CELL_CHANGED, [this](wxGridEvent& e) {
      const int r = e.GetRow();
      if (e.GetCol() == kColValue && row_unreadable(r) && !grid_->GetCellValue(r, kColValue).empty()) {
        unreadable_[r] = false;  // a new value replaces the one that could not be decrypted
        update_banner();
      }
      ensure_blank_row();
    });
    grid_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
      if ((e.GetKeyCode() == WXK_DELETE || e.GetKeyCode() == WXK_BACK) && !grid_->IsCellEditControlShown() &&
          grid_->GetSelectedRows().size() > 0) {
        delete_rows();
        return;
      }
      if (e.GetKeyCode() == WXK_SPACE && grid_->GetGridCursorCol() == kColSecret && !envs_[cur_].fixed()) {
        const int r = grid_->GetGridCursorRow();
        set_secret(r, !row_secret(r));
        return;
      }
      e.Skip();
    });
    grid_->Bind(wxEVT_GRID_CELL_RIGHT_CLICK, [this](wxGridEvent& e) {
      const int r = e.GetRow();
      if (!grid_->IsInSelection(r, 0)) grid_->SelectRow(r);
      wxMenu m;
      if (!envs_[cur_].fixed()) m.Append(1, row_secret(r) ? "Make not secret" : "Make secret");
      m.Append(2, "Delete variable");
      const int id = GetPopupMenuSelectionFromUser(m);
      if (id == 1) set_secret(r, !row_secret(r));
      else if (id == 2) delete_rows();
    });
    Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
      if (save()) e.Skip();
    }, wxID_OK);
  }

  void fit_columns() {
    const int w = grid_->GetClientSize().x - grid_->GetColSize(kColSecret) - grid_->GetColSize(kColName);
    if (w > FromDIP(80)) grid_->SetColSize(kColValue, w);
  }

  wxString list_label(const EnvModel& m) const {
    wxString s = wxString::FromUTF8(m.globals    ? std::string("Globals")
                                    : m.runtime ? "Runtime \xC2\xB7 " + std::to_string(m.vars.size()) + " (session)"
                                                : (m.name.empty() ? std::string("(unnamed)") : m.name));
    if (m.unreadable()) s += wxString::FromUTF8("  \xE2\x9A\xA0");
    return s;
  }

  void refresh_list() {
    list_->Clear();
    for (const auto& m : envs_) list_->Append(list_label(m));
  }

  void update_list_label(int i) {
    const wxString s = list_label(envs_[i]);
    if (list_->GetString(i) != s) list_->SetString(i, s);
  }

  void show_env(int i) {
    cur_ = i;
    loading_ = true;
    const EnvModel& m = envs_[i];
    name_->ChangeValue(wxString::FromUTF8(m.globals ? "Globals" : m.runtime ? "Runtime (this session only)" : m.name));
    name_->Enable(!m.fixed());
    del_env_->Enable(!m.fixed());
    grid_->ShowCol(kColSecret);
    if (m.fixed()) grid_->HideCol(kColSecret);
    if (runtime_note_) {
      runtime_note_->Show(m.runtime);
      Layout();
    }
    grid_->ClearGrid();
    if (grid_->GetNumberRows() > 0) grid_->DeleteRows(0, grid_->GetNumberRows());
    grid_->AppendRows(static_cast<int>(m.vars.size()) + 1);
    secret_.assign(m.vars.size() + 1, false);
    unreadable_.assign(m.vars.size() + 1, false);
    stored_.assign(m.vars.size() + 1, {});
    for (size_t r = 0; r < m.vars.size(); ++r) {
      const auto& v = m.vars[r];
      const int row = static_cast<int>(r);
      grid_->SetCellValue(row, kColName, wxString::FromUTF8(v.name));
      grid_->SetCellValue(row, kColValue, wxString::FromUTF8(v.value));
      secret_[r] = v.secret;
      unreadable_[r] = v.unreadable && v.value.empty();
      stored_[r] = v.stored;
      grid_->SetCellValue(row, kColSecret, v.secret ? "1" : "");
    }
    apply_row_editors();
    grid_->SetGridCursor(0, kColName);
    update_banner();
    loading_ = false;
    fit_columns();
    grid_->ForceRefresh();
  }

  void apply_row_editors() {
    for (int r = 0; r < grid_->GetNumberRows(); ++r) {
      if (row_secret(r) && !show_values()) grid_->SetCellEditor(r, kColValue, new SecretEditor);
      else grid_->SetCellEditor(r, kColValue, new wxGridCellTextEditor);
    }
  }

  void set_secret(int r, bool on) {
    if (r < 0 || r >= grid_->GetNumberRows()) return;
    grid_->DisableCellEditControl();
    secret_[r] = on;
    if (!on) unreadable_[r] = false;  // dropping the secret flag drops the undecryptable value too
    grid_->SetCellValue(r, kColSecret, on ? "1" : "");
    apply_row_editors();
    ensure_blank_row();
    update_banner();
    grid_->ForceRefresh();
  }

  void ensure_blank_row() {
    const int n = grid_->GetNumberRows();
    if (n == 0 || !grid_->GetCellValue(n - 1, kColName).empty() || !grid_->GetCellValue(n - 1, kColValue).empty()) {
      grid_->AppendRows(1);
      secret_.push_back(false);
      unreadable_.push_back(false);
      stored_.emplace_back();
      grid_->SetCellEditor(n, kColValue, new wxGridCellTextEditor);
    }
  }

  void delete_rows() {
    grid_->DisableCellEditControl();
    store_env();
    wxArrayInt rows = grid_->GetSelectedRows();
    std::sort(rows.begin(), rows.end());
    auto& vars = envs_[cur_].vars;
    for (size_t k = rows.size(); k-- > 0;)
      if (rows[k] >= 0 && static_cast<size_t>(rows[k]) < vars.size()) vars.erase(vars.begin() + rows[k]);
    show_env(cur_);
    update_list_label(cur_);
  }

  // Grid -> model for the environment shown.
  void store_env() {
    if (cur_ < 0) return;
    if (grid_->IsCellEditControlShown()) {
      grid_->SaveEditControlValue();
      grid_->HideCellEditControl();
    }
    EnvModel& m = envs_[cur_];
    m.vars.clear();
    for (int r = 0; r < grid_->GetNumberRows(); ++r) {
      VarRow v;
      v.name = trim(grid_->GetCellValue(r, kColName).utf8_string());
      v.value = grid_->GetCellValue(r, kColValue).utf8_string();
      v.secret = !m.fixed() && row_secret(r);
      v.unreadable = row_unreadable(r) && v.value.empty();
      if (v.unreadable) v.stored = stored_[r];
      if (v.name.empty() && v.value.empty() && !v.unreadable) continue;
      m.vars.push_back(std::move(v));
    }
  }

  void update_banner() {
    size_t n = 0, envs = 0;
    for (size_t i = 0; i < envs_.size(); ++i) {
      size_t k = 0;
      if (static_cast<int>(i) == cur_) {
        for (int r = 0; r < grid_->GetNumberRows(); ++r) k += row_unreadable(r);
      } else {
        k = envs_[i].unreadable();
      }
      n += k;
      envs += k > 0;
    }
    const wxString msg =
        n == 0 ? wxString()
               : wxString::Format(wxString::FromUTF8("\xE2\x9A\xA0  %zu secret value%s in %zu environment%s cannot be "
                                                     "decrypted on this machine (sealed by another Windows user or "
                                                     "PC). Re-enter %s; the old value is kept until you do."),
                                  n, n == 1 ? "" : "s", envs, envs == 1 ? "" : "s", n == 1 ? "it" : "them");
    if (banner_->GetLabel() != msg) {
      banner_->SetLabel(msg);
      banner_->Show(!msg.empty());
      Layout();
    }
    if (cur_ >= 0) {
      // keep the list's warning mark in step with the grid
      size_t k = 0;
      for (int r = 0; r < grid_->GetNumberRows(); ++r) k += row_unreadable(r);
      wxString s = list_label(envs_[cur_]);
      const wxString mark = wxString::FromUTF8("  \xE2\x9A\xA0");
      if (s.EndsWith(mark) && k == 0) s.RemoveLast(mark.size());
      else if (!s.EndsWith(mark) && k > 0) s += mark;
      if (list_->GetString(cur_) != s) list_->SetString(cur_, s);
    }
  }

  void new_env(bool duplicate) {
    store_env();
    EnvModel m;
    if (duplicate && cur_ >= 0) {
      m = envs_[cur_];
      m.globals = false;
      m.orig_name.clear();
      // Undecryptable values cannot be copied meaningfully: drop them from the copy.
      std::erase_if(m.vars, [](const VarRow& v) { return v.unreadable; });
    }
    const std::string base = duplicate && cur_ > 0 ? envs_[cur_].name + " copy" : "New environment";
    std::string name = base;
    for (int n = 2;; ++n) {
      bool clash = false;
      for (const auto& e : envs_) clash = clash || (!e.fixed() && e.name == name);
      if (!clash) break;
      name = base + " " + std::to_string(n);
    }
    m.name = name;
    envs_.push_back(std::move(m));
    list_->Append(list_label(envs_.back()));
    const int i = static_cast<int>(envs_.size()) - 1;
    list_->SetSelection(i);
    show_env(i);
    name_->SetFocus();
    name_->SelectAll();
  }

  void delete_env() {
    if (cur_ <= 0) return;
    const wxString q = wxString::Format("Delete the environment \"%s\" and its variables?", list_->GetString(cur_));
    if (wxMessageBox(q, "Environments", wxYES_NO | wxICON_QUESTION, this) != wxYES) return;
    envs_.erase(envs_.begin() + cur_);
    list_->Delete(cur_);
    const int next = std::min(cur_, static_cast<int>(envs_.size()) - 1);
    cur_ = -1;
    list_->SetSelection(next);
    show_env(next);
  }

  bool fail(int env, const wxString& msg) {
    if (env != cur_) {
      store_env();
      list_->SetSelection(env);
      show_env(env);
    }
    wxMessageBox(msg, "Environments", wxOK | wxICON_WARNING, this);
    return false;
  }

  bool save() {
    store_env();
    std::set<std::string> names;
    for (size_t i = 0; i < envs_.size(); ++i) {
      const auto& m = envs_[i];
      if (!m.fixed()) {
        const std::string n = trim(m.name);
        if (n.empty()) return fail(static_cast<int>(i), "An environment needs a name.");
        if (!names.insert(n).second)
          return fail(static_cast<int>(i), wxString::FromUTF8("Two environments are named \"" + n + "\"."));
      }
      std::set<std::string> vars;
      for (const auto& v : m.vars) {
        if (v.name.empty()) return fail(static_cast<int>(i), "A variable with a value has no name.");
        if (!vars.insert(v.name).second)
          return fail(static_cast<int>(i), wxString::FromUTF8("The variable \"" + v.name + "\" is defined twice."));
      }
    }
    std::map<std::string, std::string> globals, runtime;
    std::vector<Environment> out;
    std::string active = ws_.active_env;
    bool active_found = active.empty();
    for (const auto& m : envs_) {
      if (m.globals) {
        for (const auto& v : m.vars) globals[v.name] = v.value;
        continue;
      }
      if (m.runtime) {
        for (const auto& v : m.vars) runtime[v.name] = v.value;
        continue;
      }
      Environment e;
      e.name = trim(m.name);
      for (const auto& v : m.vars) {
        if (v.unreadable) {
          e.secrets.insert(v.name);
          e.unreadable_secrets[v.name] = v.stored;
          continue;
        }
        e.vars[v.name] = v.value;
        if (v.secret) e.secrets.insert(v.name);
      }
      if (!active.empty() && m.orig_name == ws_.active_env) {
        active = e.name;
        active_found = true;
      }
      out.push_back(std::move(e));
    }
    if (!active_found) active.clear();
    // Keep the old state to roll back if writing fails.
    auto old_envs = ws_.environments();
    auto old_globals = ws_.globals;
    auto old_active = ws_.active_env;
    ws_.environments() = std::move(out);
    ws_.globals = std::move(globals);
    ws_.active_env = active;
    std::string err;
    wxBusyCursor busy;
    if (!ws_.save_settings(&err)) {
      ws_.environments() = std::move(old_envs);
      ws_.globals = std::move(old_globals);
      ws_.active_env = std::move(old_active);
      wxMessageBox(wxString::FromUTF8("Could not save the environments: " + err), "Environments",
                   wxOK | wxICON_ERROR, this);
      return false;
    }
    ws_.runtime_vars = std::move(runtime);  // memory only
    saved = true;
    return true;
  }

  Workspace& ws_;
  std::vector<EnvModel> envs_;
  int cur_ = -1;
  bool loading_ = false;
  // Per grid row of the environment shown.
  std::vector<bool> secret_, unreadable_;
  std::vector<std::string> stored_;

  wxStaticText* banner_ = nullptr;
  EnvList* list_ = nullptr;
  wxStaticText* runtime_note_ = nullptr;
  wxButton* del_env_ = nullptr;
  wxStaticText* name_label_ = nullptr;
  wxTextCtrl* name_ = nullptr;
  wxCheckBox* show_ = nullptr;
  wxGrid* grid_ = nullptr;
  wxStaticText* hint_ = nullptr;
  int hint_wrap_ = 0;
};

void ValueRenderer::Draw(wxGrid& grid, wxGridCellAttr& attr, wxDC& dc, const wxRect& rect, int row, int col,
                         bool selected) {
  const bool secret = d_->row_secret(row) && !d_->show_values();
  const bool unreadable = d_->row_unreadable(row) && grid.GetCellValue(row, col).empty();
  if (!secret && !unreadable) {
    wxGridCellStringRenderer::Draw(grid, attr, dc, rect, row, col, selected);
    return;
  }
  wxGridCellRenderer::Draw(grid, attr, dc, rect, row, col, selected);  // background
  wxRect r = rect;
  r.Deflate(grid.FromDIP(4), 0);
  dc.SetBackgroundMode(wxBRUSHSTYLE_TRANSPARENT);
  if (unreadable) {
    wxFont f = attr.GetFont();
    f.SetStyle(wxFONTSTYLE_ITALIC);
    dc.SetFont(f);
    dc.SetTextForeground(warn_colour());
    dc.DrawLabel(wxString::FromUTF8("\xE2\x9A\xA0 ") + kUnreadableText, r, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);
    return;
  }
  const size_t n = grid.GetCellValue(row, col).length();
  if (n == 0) return;
  dc.SetFont(attr.GetFont());
  dc.SetTextForeground(dim_colour());
  wxString dots;
  for (size_t i = 0; i < std::min<size_t>(n, 12); ++i) dots += wxString::FromUTF8("\xE2\x80\xA2");
  dc.DrawLabel(dots, r, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);
}

}  // namespace

bool edit_environments(wxWindow* parent, Workspace& ws, const std::string& select) {
  EnvDialog dlg(parent, ws, select);
  dlg.ShowModal();
  return dlg.saved;
}

size_t unreadable_secret_count(Workspace& ws) {
  size_t n = 0;
  for (const auto& e : ws.environments())
    for (const auto& [k, v] : e.unreadable_secrets) n += e.vars.count(k) == 0;
  return n;
}

}  // namespace watt::app
