#include "app/sidebar_cookies.h"

#include <wx/app.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/clipbrd.h>
#include <wx/dc.h>
#include <wx/dialog.h>
#include <wx/grid.h>
#include <wx/menu.h>
#include <wx/sizer.h>
#include <wx/settings.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>
#include <wx/timer.h>
#include <wx/vlbox.h>
#include <wx/weakref.h>

#include <algorithm>
#include <optional>
#include <thread>
#include <vector>

#include "app/sidebar_cookies_text.h"
#include "app/sidebar_dialogs.h"
#include "app/sidebar_style.h"
#include "core/cookies.h"

namespace watt::app::sidebar {

namespace {

constexpr int kColDomain = 0, kColName = 1, kColValue = 2, kColPath = 3, kColExpires = 4, kColFlags = 5, kCols = 6;

wxString W(const std::string& s) { return wxString::FromUTF8(s); }
wxColour warn_colour() { return is_dark() ? wxColour(0xF0, 0xA0, 0x3C) : wxColour(0xA0, 0x4F, 0x00); }
wxColour ok_colour() { return is_dark() ? wxColour(0x6C, 0xC6, 0x8A) : wxColour(0x1E, 0x7A, 0x3C); }

// Flat column headers (the default one paints the current column in a light blue).
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

// The cookies of the selected domain(s), read-only (the editor below changes them).
class CookieTable : public wxGridTableBase {
 public:
  std::vector<Cookie> rows;
  int64_t now = 0;
  int GetNumberRows() override { return static_cast<int>(rows.size()); }
  int GetNumberCols() override { return kCols; }
  bool IsEmptyCell(int, int) override { return false; }
  void SetValue(int, int, const wxString&) override {}
  wxString GetColLabelValue(int c) override {
    static const char* const n[] = {"Domain", "Name", "Value", "Path", "Expires", "Flags"};
    return c >= 0 && c < kCols ? wxString(n[c]) : wxString();
  }
  wxString GetValue(int r, int c) override {
    if (r < 0 || r >= GetNumberRows()) return {};
    const Cookie& k = rows[static_cast<size_t>(r)];
    switch (c) {
      case kColDomain: return W(k.host_only ? k.domain : "." + k.domain);
      case kColName: return W(k.name);
      case kColValue: return W(k.value.size() > 300 ? k.value.substr(0, 300) + "\xE2\x80\xA6" : k.value);
      case kColPath: return W(k.path);
      case kColExpires: return W(cookie_expiry_label(k, now));
      case kColFlags: return W(cookie_flags_label(k));
      default: return {};
    }
  }
  // Replaces the rows, telling the grid how the row count changed.
  void reset(std::vector<Cookie> next) {
    const int before = GetNumberRows();
    rows = std::move(next);
    const int after = GetNumberRows();
    if (wxGrid* g = GetView()) {
      if (after < before) {
        wxGridTableMessage m(this, wxGRIDTABLE_NOTIFY_ROWS_DELETED, after, before - after);
        g->ProcessTableMessage(m);
      } else if (after > before) {
        wxGridTableMessage m(this, wxGRIDTABLE_NOTIFY_ROWS_APPENDED, after - before);
        g->ProcessTableMessage(m);
      }
      g->ForceRefresh();
    }
  }
};

// Domains with their cookie counts. Owner drawn (the native list box selection is unreadable in
// dark mode). Row 0 is "All domains".
class DomainList : public wxVListBox {
 public:
  explicit DomainList(wxWindow* parent, const wxSize& size)
      : wxVListBox(parent, wxID_ANY, wxDefaultPosition, size, wxBORDER_THEME | wxLB_SINGLE) {
    SetBackgroundColour(window_colour());
  }
  struct Item {
    wxString label;
    size_t count = 0;
  };
  void set(std::vector<Item> items) {
    items_ = std::move(items);
    SetItemCount(items_.size());
    Refresh();
  }
  size_t size() const { return items_.size(); }

 protected:
  wxCoord OnMeasureItem(size_t) const override { return FromDIP(26); }
  void OnDrawBackground(wxDC& dc, const wxRect& rect, size_t n) const override {
    if (!IsSelected(n)) return;
    dc.SetBrush(wxBrush(highlight_colour()));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(rect);
  }
  void OnDrawItem(wxDC& dc, const wxRect& rect, size_t n) const override {
    if (n >= items_.size()) return;
    const Item& it = items_[n];
    wxFont f = GetFont();
    if (n == 0) f.SetStyle(wxFONTSTYLE_ITALIC);
    dc.SetFont(f);
    const wxString count = wxString::Format("%zu", it.count);
    const int cw = dc.GetTextExtent(count).x;
    const int y = rect.y + (rect.height - dc.GetCharHeight()) / 2;
    const int x = rect.x + FromDIP(10);
    const int avail = rect.width - FromDIP(10) - cw - FromDIP(20);
    wxString label = it.label;
    if (dc.GetTextExtent(label).x > avail) {
      while (label.size() > 1 && dc.GetTextExtent(label + wxString::FromUTF8("\xE2\x80\xA6")).x > avail)
        label.RemoveLast();
      label += wxString::FromUTF8("\xE2\x80\xA6");
    }
    dc.SetTextForeground(text_colour());
    dc.DrawText(label, x, y);
    dc.SetTextForeground(dim_colour());
    dc.DrawText(count, rect.GetRight() - FromDIP(10) - cw, y);
  }

 private:
  std::vector<Item> items_;
};

class CookieDialog : public wxDialog {
 public:
  CookieDialog(wxWindow* parent, std::shared_ptr<CookieJar> jar, const std::string& domain)
      : wxDialog(parent, wxID_ANY, "Cookies", wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
        jar_(std::move(jar)),
        want_domain_(domain) {
    build_ui();
    reload();
    SetSize(FromDIP(wxSize(1120, 640)));
    CentreOnParent();
    CallAfter([this] {
      fit_columns();
      filter_->SetFocus();
    });
  }

 private:
  // ---------------------------------------------------------------- UI

  void build_ui() {
    SetBackgroundColour(window_colour());
    SetForegroundColour(text_colour());
    auto* top = new wxBoxSizer(wxVERTICAL);

    banner_ = new wxStaticText(this, wxID_ANY, "");
    banner_->SetForegroundColour(warn_colour());
    banner_->Hide();
    top->Add(banner_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    auto* head = new wxBoxSizer(wxHORIZONTAL);
    filter_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    filter_->SetHint("Filter by domain, name or value");
    head->Add(filter_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    count_ = new wxStaticText(this, wxID_ANY, "");
    count_->SetForegroundColour(dim_colour());
    head->Add(count_, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(head, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    auto* body = new wxBoxSizer(wxHORIZONTAL);
    auto* left = new wxBoxSizer(wxVERTICAL);
    domains_ = new DomainList(this, FromDIP(wxSize(230, -1)));
    left->Add(domains_, 1, wxEXPAND);
    del_domain_ = new wxButton(this, wxID_ANY, "Delete domain");
    del_domain_->SetToolTip("Delete every cookie of the selected domain and its subdomains");
    left->Add(del_domain_, 0, wxEXPAND | wxTOP, FromDIP(6));
    body->Add(left, 0, wxEXPAND | wxRIGHT, FromDIP(12));

    auto* right = new wxBoxSizer(wxVERTICAL);
    grid_ = new wxGrid(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_THEME);
    table_ = new CookieTable;
    grid_->AssignTable(table_, wxGrid::wxGridSelectRows);
    table_->SetAttrProvider(new FlatHeaderProvider);
    grid_->HideRowLabels();
    grid_->EnableEditing(false);
    grid_->SetColLabelAlignment(wxALIGN_LEFT, wxALIGN_CENTER);
    grid_->SetDefaultRowSize(FromDIP(24), true);
    grid_->SetColLabelSize(FromDIP(26));
    grid_->DisableDragRowSize();
    grid_->SetDefaultCellOverflow(false);
    grid_->SetDefaultCellBackgroundColour(window_colour());
    grid_->SetDefaultCellTextColour(text_colour());
    grid_->SetGridLineColour(mix(window_colour(), text_colour(), 0.12));
    grid_->SetLabelBackgroundColour(mix(window_colour(), text_colour(), 0.05));
    grid_->SetLabelTextColour(dim_colour());
    grid_->SetCellHighlightPenWidth(0);
    grid_->SetSelectionBackground(highlight_colour());
    grid_->SetSelectionForeground(text_colour());
    right->Add(grid_, 1, wxEXPAND);

    // Editor of one cookie.
    auto* form = new wxFlexGridSizer(4, FromDIP(6), FromDIP(10));
    form->AddGrowableCol(1, 1);
    form->AddGrowableCol(3, 2);
    auto label = [&](const char* t) {
      auto* l = new wxStaticText(this, wxID_ANY, t);
      l->SetForegroundColour(dim_colour());
      form->Add(l, 0, wxALIGN_CENTER_VERTICAL);
    };
    label("Name");
    name_ = new wxTextCtrl(this, wxID_ANY);
    form->Add(name_, 1, wxEXPAND);
    label("Value");
    value_ = new wxTextCtrl(this, wxID_ANY);
    form->Add(value_, 1, wxEXPAND);
    label("Domain");
    domain_ = new wxTextCtrl(this, wxID_ANY);
    domain_->SetHint("api.example.com");
    form->Add(domain_, 1, wxEXPAND);
    label("Path");
    path_ = new wxTextCtrl(this, wxID_ANY, "/");
    form->Add(path_, 1, wxEXPAND);
    label("Expires");
    expires_ = new wxTextCtrl(this, wxID_ANY);
    expires_->SetHint("session, 2026-12-31 23:59 (UTC), +7d");
    expires_->SetToolTip("Empty or \"session\": kept until cleared. A UTC date (2026-12-31 23:59), a cookie date "
                         "(Thu, 31 Dec 2026 23:59:00 GMT) or a time from now (+30m, +2h, +7d).");
    form->Add(expires_, 1, wxEXPAND);
    label("SameSite");
    {
      auto* row = new wxBoxSizer(wxHORIZONTAL);
      same_site_ = new wxChoice(this, wxID_ANY);
      same_site_->Append("(not set)");
      same_site_->Append("None");
      same_site_->Append("Lax");
      same_site_->Append("Strict");
      same_site_->SetSelection(0);
      row->Add(same_site_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(14));
      secure_ = new wxCheckBox(this, wxID_ANY, "Secure");
      secure_->SetToolTip("Sent over https only (and to loopback http)");
      http_only_ = new wxCheckBox(this, wxID_ANY, "HttpOnly");
      subdomains_ = new wxCheckBox(this, wxID_ANY, "Subdomains");
      subdomains_->SetToolTip("Also sent to subdomains of the domain (a Domain= attribute); off: this host only");
      for (wxCheckBox* c : {secure_, http_only_, subdomains_}) {
        c->SetForegroundColour(text_colour());
        row->Add(c, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
      }
      form->Add(row, 1, wxEXPAND);
    }
    right->Add(form, 0, wxEXPAND | wxTOP, FromDIP(10));

    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    new_btn_ = new wxButton(this, wxID_ANY, "New cookie");
    paste_btn_ = new wxButton(this, wxID_ANY, wxString::FromUTF8("Add from Set-Cookie\xE2\x80\xA6"));
    paste_btn_->SetToolTip("Paste a Set-Cookie header value for the domain above; the jar's rules apply to it");
    copy_btn_ = new wxButton(this, wxID_ANY, "Copy as Set-Cookie");
    del_btn_ = new wxButton(this, wxID_ANY, "Delete");
    save_btn_ = new wxButton(this, wxID_ANY, "Save cookie");
    actions->Add(new_btn_, 0, wxRIGHT, FromDIP(6));
    actions->Add(paste_btn_, 0, wxRIGHT, FromDIP(6));
    actions->Add(copy_btn_, 0, wxRIGHT, FromDIP(6));
    status_ = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    actions->Add(status_, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(8));
    actions->Add(del_btn_, 0, wxRIGHT, FromDIP(6));
    actions->Add(save_btn_, 0);
    right->Add(actions, 0, wxEXPAND | wxTOP, FromDIP(10));
    body->Add(right, 1, wxEXPAND);
    top->Add(body, 1, wxEXPAND | wxALL, FromDIP(12));

    auto* foot = new wxBoxSizer(wxHORIZONTAL);
    auto* hint = new wxStaticText(this, wxID_ANY,
                                  "Set by responses (Set-Cookie) and sent with matching requests unless a request "
                                  "turns the jar off. Kept per workspace in .watt/cookies.json, encrypted for your "
                                  "Windows account; never in git.");
    hint->SetForegroundColour(dim_colour());
    hint->Wrap(FromDIP(640));
    foot->Add(hint, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    clear_btn_ = new wxButton(this, wxID_ANY, "Clear all");
    clear_btn_->SetForegroundColour(is_dark() ? wxColour(0xF0, 0x70, 0x68) : wxColour(0xB0, 0x2A, 0x22));
    foot->Add(clear_btn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    auto* close = new wxButton(this, wxID_CANCEL, "Close");
    foot->Add(close, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(foot, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    SetSizer(top);
    SetEscapeId(wxID_CANCEL);

    if (std::string why = jar_->load_rejected(); !why.empty()) {
      banner_->SetLabel(W(why));
      banner_->Show();
    }

    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      CallAfter([this] { fit_columns(); });
    });
    filter_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { reload(); });
    filter_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { filter_timer_.StartOnce(60); });
    filter_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
      if (e.GetKeyCode() == WXK_DOWN && grid_->GetNumberRows() > 0) {
        grid_->SetFocus();
        select_row(0);
        return;
      }
      e.Skip();
    });
    domains_->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) {
      show_domain(domains_->GetSelection());
    });
    grid_->Bind(wxEVT_GRID_SELECT_CELL, [this](wxGridEvent& e) {
      e.Skip();
      fill_form(e.GetRow());
    });
    grid_->Bind(wxEVT_GRID_RANGE_SELECTED, [this](wxGridRangeSelectEvent& e) {
      e.Skip();
      if (e.Selecting()) fill_form(e.GetTopRow());
    });
    grid_->Bind(wxEVT_GRID_CELL_LEFT_DCLICK, [this](wxGridEvent& e) {
      fill_form(e.GetRow());
      value_->SetFocus();
      value_->SelectAll();
    });
    grid_->Bind(wxEVT_GRID_CELL_RIGHT_CLICK, [this](wxGridEvent& e) {
      select_row(e.GetRow());
      row_menu();
    });
    grid_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
      if (e.GetKeyCode() == WXK_DELETE) {
        delete_cookie();
        return;
      }
      if (e.GetKeyCode() == 'C' && e.ControlDown()) {
        copy_cookie();
        return;
      }
      e.Skip();
    });
    for (wxTextCtrl* t : {name_, value_, domain_, path_, expires_})
      t->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
        if (e.GetKeyCode() == WXK_RETURN || e.GetKeyCode() == WXK_NUMPAD_ENTER) {
          save_cookie();
          return;
        }
        e.Skip();
      });
    new_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { new_cookie(); });
    paste_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { paste_set_cookie(); });
    copy_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { copy_cookie(); });
    del_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { delete_cookie(); });
    save_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { save_cookie(); });
    del_domain_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { delete_domain(); });
    clear_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { clear_all(); });
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
      if (e.GetKeyCode() == 'F' && e.ControlDown()) {
        filter_->SetFocus();
        filter_->SelectAll();
        return;
      }
      if (e.GetKeyCode() == 'N' && e.ControlDown()) {
        new_cookie();
        return;
      }
      e.Skip();
    });
  }

  void fit_columns() {
    const int w = grid_->GetClientSize().x;
    if (w <= 0) return;
    const int dom = FromDIP(125), name = FromDIP(115), path = FromDIP(56), exp = FromDIP(176), flags = FromDIP(170);
    grid_->SetColSize(kColDomain, dom);
    grid_->SetColSize(kColName, name);
    grid_->SetColSize(kColPath, path);
    grid_->SetColSize(kColExpires, exp);
    grid_->SetColSize(kColFlags, flags);
    grid_->SetColSize(kColValue, std::max(FromDIP(120), w - dom - name - path - exp - flags - wxSystemSettings::GetMetric(wxSYS_VSCROLL_X, grid_) - 2));
  }

  // ---------------------------------------------------------------- model

  // Reads the jar again (it may have changed: a response set a cookie while the dialog is open)
  // and shows the domain that was selected (or the one asked for).
  void reload(const std::optional<Cookie>& select = std::nullopt) {
    now_ = cookies::now();
    all_ = jar_->list(now_);
    groups_ = group_cookies(all_, filter_->GetValue().utf8_string());
    size_t shown = 0;
    std::vector<DomainList::Item> items;
    for (const auto& g : groups_) shown += g.cookies.size();
    items.push_back({filter_->IsEmpty() ? "All domains" : "All matches", shown});
    for (const auto& g : groups_) items.push_back({W(g.domain), g.cookies.size()});
    domains_->set(std::move(items));
    std::string want = select ? select->domain : !want_domain_.empty() ? want_domain_ : cur_domain_;
    want_domain_.clear();
    int sel = 0;
    for (size_t i = 0; i < groups_.size(); ++i)
      if (groups_[i].domain == want) sel = static_cast<int>(i) + 1;
    domains_->SetSelection(sel);
    count_->SetLabel(all_.empty() ? wxString("No cookies yet")
                                  : wxString::Format("%zu cookie%s in %zu domain%s", all_.size(),
                                                     all_.size() == 1 ? "" : "s", count_domains(),
                                                     count_domains() == 1 ? "" : "s"));
    show_domain(sel, select);
    Layout();
  }

  size_t count_domains() const {
    size_t n = 0;
    std::string last;
    for (const auto& c : all_)
      if (n == 0 || c.domain != last) {
        ++n;
        last = c.domain;
      }
    return n;
  }

  void show_domain(int sel, const std::optional<Cookie>& select = std::nullopt) {
    std::vector<Cookie> rows;
    if (sel <= 0) {
      for (const auto& g : groups_)
        for (size_t i : g.cookies) rows.push_back(all_[i]);
      cur_domain_.clear();
    } else if (static_cast<size_t>(sel) <= groups_.size()) {
      const auto& g = groups_[static_cast<size_t>(sel) - 1];
      for (size_t i : g.cookies) rows.push_back(all_[i]);
      cur_domain_ = g.domain;
    }
    table_->now = now_;
    table_->reset(std::move(rows));
    del_domain_->Enable(!cur_domain_.empty());
    clear_btn_->Enable(!all_.empty());
    int row = -1;
    if (select)
      for (size_t i = 0; i < table_->rows.size(); ++i) {
        const Cookie& c = table_->rows[i];
        if (c.name == select->name && c.domain == select->domain && c.path == select->path) row = static_cast<int>(i);
      }
    if (row < 0 && !table_->rows.empty()) row = 0;
    if (row >= 0) {
      select_row(row);
    } else {
      grid_->ClearSelection();
      blank_form(cur_domain_);
    }
  }

  void select_row(int row) {
    if (row < 0 || row >= grid_->GetNumberRows()) return;
    grid_->SelectRow(row);
    grid_->SetGridCursor(row, 0);
    grid_->MakeCellVisible(row, 0);
    fill_form(row);
  }

  const Cookie* selected() const { return editing_ ? &*editing_ : nullptr; }

  void fill_form(int row) {
    if (row < 0 || row >= static_cast<int>(table_->rows.size())) return;
    const Cookie& c = table_->rows[static_cast<size_t>(row)];
    editing_ = c;
    name_->ChangeValue(W(c.name));
    value_->ChangeValue(W(c.value));
    domain_->ChangeValue(W(c.domain));
    path_->ChangeValue(W(c.path));
    expires_->ChangeValue(W(cookie_expiry_edit_text(c)));
    same_site_->SetSelection(static_cast<int>(c.same_site));
    secure_->SetValue(c.secure);
    http_only_->SetValue(c.http_only);
    subdomains_->SetValue(!c.host_only);
    del_btn_->Enable(true);
    copy_btn_->Enable(true);
    save_btn_->SetLabel("Save cookie");
  }

  void blank_form(const std::string& domain) {
    editing_.reset();
    name_->ChangeValue("");
    value_->ChangeValue("");
    domain_->ChangeValue(W(domain));
    path_->ChangeValue("/");
    expires_->ChangeValue("");
    same_site_->SetSelection(0);
    secure_->SetValue(false);
    http_only_->SetValue(false);
    subdomains_->SetValue(false);
    del_btn_->Enable(false);
    copy_btn_->Enable(false);
    save_btn_->SetLabel("Add cookie");
  }

  void say(const wxString& s, bool bad) {
    status_->SetForegroundColour(bad ? warn_colour() : ok_colour());
    status_->SetLabel(s);
    status_->SetToolTip(s);
  }

  // ---------------------------------------------------------------- actions

  void new_cookie() {
    grid_->ClearSelection();
    blank_form(cur_domain_);
    say("", false);
    (cur_domain_.empty() ? domain_ : name_)->SetFocus();
  }

  void save_cookie() {
    Cookie c;
    c.name = name_->GetValue().utf8_string();
    c.value = value_->GetValue().utf8_string();
    c.domain = domain_->GetValue().utf8_string();
    c.path = path_->GetValue().utf8_string();
    if (c.path.empty()) c.path = "/";
    while (!c.name.empty() && (c.name.back() == ' ' || c.name.back() == '\t')) c.name.pop_back();
    while (!c.name.empty() && (c.name.front() == ' ' || c.name.front() == '\t')) c.name.erase(0, 1);
    if (c.name.empty()) {
      say("A cookie needs a name", true);
      name_->SetFocus();
      return;
    }
    if (c.name.find_first_of("=;") != std::string::npos || c.value.find(';') != std::string::npos) {
      say("Names cannot hold '=' or ';', values cannot hold ';'", true);
      return;
    }
    if (!parse_cookie_expiry(expires_->GetValue().utf8_string(), cookies::now(), c.expires)) {
      say("Expires: use session, a UTC date (2026-12-31 23:59) or +7d", true);
      expires_->SetFocus();
      return;
    }
    c.same_site = static_cast<SameSite>(std::max(0, same_site_->GetSelection()));
    c.secure = secure_->GetValue();
    c.http_only = http_only_->GetValue();
    c.host_only = !subdomains_->GetValue();
    const std::optional<Cookie> before = editing_;
    std::string dom = c.domain;
    while (!dom.empty() && dom.front() == '.') dom.erase(0, 1);
    for (char& ch : dom)
      if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    const bool moved = before && (before->name != c.name || before->domain != dom || before->path != c.path);
    if (before) {
      c.created = before->created;
      if (moved) jar_->remove(before->domain, before->path, before->name);
    }
    std::string why;
    if (!jar_->upsert(c, &why)) {
      if (moved) jar_->upsert(*before);  // put the old one back
      say(W(why.empty() ? std::string("The jar refused this cookie") : why), true);
      return;
    }
    Cookie shown = c;
    shown.domain = dom;
    const bool expired = c.expires != 0 && c.expires <= cookies::now();
    say(expired ? wxString("Saved with an expiry in the past: the cookie is gone")
                : wxString::FromUTF8(before ? "Saved" : "Added"),
        expired);
    reload(shown);
  }

  void delete_cookie() {
    const Cookie* c = selected();
    if (!c) return;
    const Cookie gone = *c;
    jar_->remove(gone.domain, gone.path, gone.name);
    const int row = grid_->GetGridCursorRow();
    say(W("Deleted " + gone.name), false);
    reload();
    if (grid_->GetNumberRows() > 0) select_row(std::min(row, grid_->GetNumberRows() - 1));
  }

  void delete_domain() {
    if (cur_domain_.empty()) return;
    const std::string d = cur_domain_;
    if (!confirm(this, "Delete domain", W("Delete every cookie of " + d + " and its subdomains?"), "Delete", true))
      return;
    const size_t n = jar_->remove_domain(d);
    cur_domain_.clear();
    say(wxString::Format("Deleted %zu cookie%s", n, n == 1 ? "" : "s"), false);
    reload();
  }

  void clear_all() {
    if (all_.empty()) return;
    if (!confirm(this, "Clear all cookies",
                 wxString::Format("Delete all %zu cookies of this workspace?", all_.size()), "Clear all", true))
      return;
    jar_->clear();
    cur_domain_.clear();
    say("Cleared", false);
    reload();
  }

  void copy_cookie() {
    const Cookie* c = selected();
    if (!c) return;
    if (wxTheClipboard->Open()) {
      wxTheClipboard->SetData(new wxTextDataObject(W(set_cookie_line(*c))));
      wxTheClipboard->Close();
      say("Copied as Set-Cookie", false);
    }
  }

  void copy_value() {
    const Cookie* c = selected();
    if (!c) return;
    if (wxTheClipboard->Open()) {
      wxTheClipboard->SetData(new wxTextDataObject(W(c->value)));
      wxTheClipboard->Close();
      say("Copied the value", false);
    }
  }

  void row_menu() {
    if (!selected()) return;
    enum { kCopy = 1, kValue, kDelete };
    wxMenu m;
    m.Append(kCopy, "Copy as Set-Cookie\tCtrl+C");
    m.Append(kValue, "Copy value");
    m.AppendSeparator();
    m.Append(kDelete, "Delete\tDel");
    switch (grid_->GetPopupMenuSelectionFromUser(m)) {
      case kCopy: copy_cookie(); break;
      case kValue: copy_value(); break;
      case kDelete: delete_cookie(); break;
      default: break;
    }
  }

  // A Set-Cookie line as a server would send it for the domain in the form: recorded through
  // the jar's own Set-Cookie rules (public suffixes, __Host- / __Secure-, size limits).
  void paste_set_cookie() {
    std::string dom = domain_->GetValue().utf8_string();
    if (dom.empty()) dom = cur_domain_;
    const std::string url = cookie_origin_url(dom);
    if (url.empty()) {
      say("Type the domain first (api.example.com)", true);
      domain_->SetFocus();
      return;
    }
    wxString pre;
    if (wxTheClipboard->Open()) {
      if (wxTheClipboard->IsSupported(wxDF_UNICODETEXT)) {
        wxTextDataObject d;
        if (wxTheClipboard->GetData(d)) {
          wxString t = d.GetText().Strip(wxString::both);
          if (t.Lower().StartsWith("set-cookie:")) t = t.Mid(11).Strip(wxString::leading);
          if (t.size() < 4096 && t.Find('=') > 0 && t.Find('\n') == wxNOT_FOUND) pre = t;
        }
      }
      wxTheClipboard->Close();
    }
    wxTextEntryDialog dlg(this, W("Set-Cookie header value for " + url.substr(8, url.size() - 9) + ":"),
                          "Add from Set-Cookie", pre);
    dlg.SetSize(FromDIP(wxSize(640, -1)));
    if (dlg.ShowModal() != wxID_OK) return;
    std::string line = dlg.GetValue().Strip(wxString::both).utf8_string();
    if (line.size() >= 11 && (line.compare(0, 11, "Set-Cookie:") == 0 || line.compare(0, 11, "set-cookie:") == 0))
      line.erase(0, 11);
    std::string why;
    if (!jar_->set_from_header(url, line, &why)) {
      say(W(why.empty() ? std::string("Not stored") : "Not stored: " + why), true);
      return;
    }
    // Select what was stored: the name before '='.
    Cookie shown;
    const size_t eq = line.find('=');
    shown.name = line.substr(0, eq);
    while (!shown.name.empty() && shown.name.front() == ' ') shown.name.erase(0, 1);
    while (!shown.name.empty() && shown.name.back() == ' ') shown.name.pop_back();
    say(W("Stored " + shown.name), false);
    now_ = cookies::now();
    for (const auto& c : jar_->list(now_))
      if (c.name == shown.name && cookies::domain_match(url.substr(8, url.size() - 9), c.domain)) shown = c;
    reload(shown.domain.empty() ? std::nullopt : std::optional<Cookie>(shown));
  }

  std::shared_ptr<CookieJar> jar_;
  std::string want_domain_, cur_domain_;
  int64_t now_ = 0;
  std::vector<Cookie> all_;
  std::vector<CookieDomain> groups_;
  std::optional<Cookie> editing_;  // the cookie the form shows (none: a new one)
  wxTimer filter_timer_;

  wxStaticText* banner_ = nullptr;
  wxTextCtrl* filter_ = nullptr;
  wxStaticText* count_ = nullptr;
  DomainList* domains_ = nullptr;
  wxButton* del_domain_ = nullptr;
  wxGrid* grid_ = nullptr;
  CookieTable* table_ = nullptr;
  wxTextCtrl *name_ = nullptr, *value_ = nullptr, *domain_ = nullptr, *path_ = nullptr, *expires_ = nullptr;
  wxChoice* same_site_ = nullptr;
  wxCheckBox *secure_ = nullptr, *http_only_ = nullptr, *subdomains_ = nullptr;
  wxButton *new_btn_ = nullptr, *paste_btn_ = nullptr, *copy_btn_ = nullptr, *del_btn_ = nullptr,
           *save_btn_ = nullptr, *clear_btn_ = nullptr;
  wxStaticText* status_ = nullptr;
};

}  // namespace

void manage_cookies(wxWindow* parent, std::shared_ptr<CookieJar> jar, const std::string& domain) {
  if (!jar) return;
  // The weak reference is made and read on the UI thread only (the worker just carries it).
  auto p = std::make_shared<wxWeakRef<wxWindow>>(parent);
  std::thread([p, jar, domain] {
    jar->preload();  // one file read + one DPAPI call, off the UI thread
    if (wxAppConsole* app = wxTheApp)
      app->CallAfter([p, jar, domain] {
        if (!p->get()) return;
        // Built in one UI step, shown in the next: each stays inside the frame budget.
        auto* dlg = new CookieDialog(p->get(), jar, domain);
        wxWeakRef<wxDialog> d(dlg);
        if (wxAppConsole* a = wxTheApp)
          a->CallAfter([d] {
            if (!d) return;
            wxDialog* w = d.get();
            w->ShowModal();
            w->Destroy();
          });
      });
  }).detach();
}

}  // namespace watt::app::sidebar
