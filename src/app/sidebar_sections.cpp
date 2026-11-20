#include "app/sidebar_sections.h"

#include <wx/app.h>
#include <wx/aui/auibook.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/listctrl.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/stattext.h>
#include <wx/stc/stc.h>
#include <wx/weakref.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <thread>

#include "app/http_editor_widgets.h"  // FlatButton
#include "app/sidebar_import.h"
#include "app/sidebar_specs.h"
#include "app/sidebar_style.h"

namespace watt::app::sidebar {

namespace fs = std::filesystem;

namespace {
wxString U(const std::string& s) { return wxString::FromUTF8(s); }
}  // namespace

// ------------------------------------------------------------------ SectionPane

SectionPane::SectionPane(wxWindow* parent, const wxString& title, const wxString& add_tip, const wxString& empty_text)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxFULL_REPAINT_ON_RESIZE),
      title_(title),
      add_tip_(add_tip),
      empty_text_(empty_text) {
  SetBackgroundStyle(wxBG_STYLE_PAINT);
  Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
  Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
    const size_t r = row_at(e.GetPosition());
    const bool plus = on_plus(e.GetPosition());
    const bool head = e.GetPosition().y < header_h() && !plus;
    if (r != hot_ || plus != plus_hot_ || head != head_hot_) {
      hot_ = r;
      plus_hot_ = plus;
      head_hot_ = head;
      wxString tip;
      if (plus) tip = add_tip_;
      else if (head) tip = collapsed_ ? wxString("Show " + title_.Lower()) : wxString("Collapse to this header");
      else if (r != npos) tip = U(rows_[r].tip);
      if (GetToolTipText() != tip) SetToolTip(tip);
      SetCursor(plus || head || r != npos ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
      Refresh(false);
    }
  });
  Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
    if (hot_ != npos || plus_hot_ || head_hot_) {
      hot_ = npos;
      plus_hot_ = head_hot_ = false;
      Refresh(false);
    }
  });
  Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) {
    const wxPoint p = e.GetPosition();
    if (on_plus(p)) {
      if (on_add) on_add(wxPoint(GetClientSize().x - FromDIP(26), header_h()));
      return;
    }
    if (p.y < header_h()) {
      set_collapsed(!collapsed_);
      if (on_toggle) on_toggle();
      return;
    }
    if (const size_t r = row_at(p); r != npos && on_click) on_click(r);
  });
  Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent& e) {
    const wxPoint p = e.GetPosition();
    if (on_plus(p) || p.y < header_h()) return;  // the first click did it
    if (const size_t r = row_at(p); r != npos && on_dclick) on_dclick(r);
  });
  Bind(wxEVT_RIGHT_UP, [this](wxMouseEvent& e) {
    if (on_menu) on_menu(row_at(e.GetPosition()));
  });
  Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& e) {
    const int max_top = std::max(0, static_cast<int>(rows_.size()) - visible_rows());
    const int t = std::clamp(top_ + (e.GetWheelRotation() > 0 ? -1 : 1), 0, max_top);
    if (t != top_) {
      top_ = t;
      Refresh(false);
    }
  });
}

int SectionPane::header_h() const { return FromDIP(22); }
int SectionPane::row_h() const { return FromDIP(20); }

int SectionPane::visible_rows() const {
  // Empty: just the header line (its hint is drawn in it), so the history below gets the rows.
  if (collapsed_ || rows_.empty()) return 0;
  return std::min(static_cast<int>(rows_.size()), max_rows_);
}

wxSize SectionPane::DoGetBestClientSize() const {
  return wxSize(FromDIP(120), header_h() + visible_rows() * row_h() + (collapsed_ || rows_.empty() ? 0 : FromDIP(2)));
}

size_t SectionPane::row_at(const wxPoint& p) const {
  if (collapsed_ || p.y < header_h()) return npos;
  const int i = (p.y - header_h()) / row_h();
  if (i >= visible_rows()) return npos;
  const size_t r = static_cast<size_t>(top_ + i);
  return r < rows_.size() ? r : npos;
}

bool SectionPane::on_plus(const wxPoint& p) const {
  return p.y < header_h() && p.x >= GetClientSize().x - FromDIP(28);
}

void SectionPane::relayout() {
  InvalidateBestSize();
  // From DoGetBestClientSize, not GetBestSize (that one keeps the larger min size set before,
  // so a collapse would not shrink the pane).
  const int want = DoGetBestClientSize().y;
  if (GetMinSize().y != want || GetSize().y != want) {
    SetMinSize(wxSize(-1, want));
    if (wxWindow* p = GetParent()) {
      std::vector<std::pair<wxWindow*, wxRect>> before;
      for (wxWindow* c : p->GetChildren())
        if (c->IsShown()) before.emplace_back(c, c->GetRect());
      p->Layout();
      // Every place a moved or resized sibling left or took, children included (the tree grows
      // into the band the panes leave): a strip that kept a pane header's old pixels showed a
      // dead "+" above the next header.
      wxRect band;
      for (const auto& [c, r] : before)
        if (c->GetRect() != r) band = band.IsEmpty() ? r.Union(c->GetRect()) : band.Union(r).Union(c->GetRect());
      if (!band.IsEmpty()) {
        band.x = 0;
        band.width = p->GetClientSize().x;
        p->RefreshRect(band, true);
      }
    }
  }
  Refresh(false);
}

void SectionPane::set_rows(std::vector<SectionRow> rows) {
  const size_t before = rows_.size();
  rows_ = std::move(rows);
  top_ = std::clamp(top_, 0, std::max(0, static_cast<int>(rows_.size()) - max_rows_));
  if (hot_ != npos && hot_ >= rows_.size()) hot_ = npos;
  if (std::min<size_t>(before, max_rows_) != std::min<size_t>(rows_.size(), max_rows_) ||
      (before == 0) != rows_.empty())
    relayout();
  else
    Refresh(false);
}

void SectionPane::set_collapsed(bool c) {
  if (c == collapsed_) return;
  collapsed_ = c;
  hot_ = npos;
  relayout();
}

void SectionPane::set_max_rows(int n) {
  max_rows_ = std::max(1, n);
  relayout();
}

void SectionPane::paint() {
  wxAutoBufferedPaintDC dc(this);
  const wxSize sz = GetClientSize();
  const wxColour bg = window_colour(), fg = text_colour(), dim = dim_colour();
  dc.SetBackground(wxBrush(bg));
  dc.Clear();
  // Separator above the header (as above HISTORY).
  dc.SetPen(wxPen(mix(bg, fg, 0.14)));
  dc.DrawLine(0, 0, sz.x, 0);
  const int pad = FromDIP(8);
  const int hh = header_h();
  if (head_hot_) {
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(mix(bg, fg, 0.05)));
    dc.DrawRectangle(0, 1, sz.x, hh - 1);
  }
  // Header: chevron, TITLE (bold, small, dim; COLLECTION's font), count, "+".
  wxFont hf = GetFont();
  hf.SetFractionalPointSize(hf.GetFractionalPointSize() * 0.85);
  hf.SetWeight(wxFONTWEIGHT_BOLD);
  dc.SetFont(hf);
  dc.SetTextForeground(dim);
  {
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (gc) {
      gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
      gc->SetPen(wxPen(dim, std::max(1, FromDIP(1))));
      const double cx = pad + FromDIP(3), cy = hh / 2.0, s = FromDIP(3);
      wxGraphicsPath path = gc->CreatePath();
      if (collapsed_) {  // >
        path.MoveToPoint(cx - s / 2, cy - s);
        path.AddLineToPoint(cx + s / 2, cy);
        path.AddLineToPoint(cx - s / 2, cy + s);
      } else {  // v
        path.MoveToPoint(cx - s, cy - s / 2);
        path.AddLineToPoint(cx, cy + s / 2);
        path.AddLineToPoint(cx + s, cy - s / 2);
      }
      gc->StrokePath(path);
    }
  }
  wxString head = title_;
  if (!rows_.empty()) head += wxString::Format("   %zu", rows_.size());
  const int tx = pad + FromDIP(12);
  const wxSize th = dc.GetTextExtent(head);
  dc.DrawText(head, tx, (hh - th.y) / 2);
  if (rows_.empty() && !collapsed_) {  // the empty hint, in the header line
    wxFont nf = GetFont();
    nf.SetFractionalPointSize(nf.GetFractionalPointSize() * 0.9);
    dc.SetFont(nf);
    dc.SetTextForeground(mix(bg, dim, 0.8));
    const int hx = tx + th.x + FromDIP(10);
    const int room = sz.x - FromDIP(30) - hx;
    if (room > FromDIP(30)) {
      const wxString t = wxControl::Ellipsize(empty_text_, dc, wxELLIPSIZE_END, room);
      dc.DrawText(t, hx, (hh - dc.GetCharHeight()) / 2);
    }
    dc.SetFont(hf);
  }
  {
    wxFont pf = GetFont();
    pf.SetFractionalPointSize(pf.GetFractionalPointSize() * 1.3);
    dc.SetFont(pf);
    const wxSize ps = dc.GetTextExtent("+");
    const int px = sz.x - FromDIP(26);
    if (plus_hot_) {
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.SetBrush(wxBrush(mix(bg, fg, 0.14)));
      dc.DrawRoundedRectangle(px, FromDIP(2), FromDIP(22), hh - FromDIP(4), FromDIP(3));
    }
    dc.SetTextForeground(fg);
    dc.DrawText("+", px + (FromDIP(22) - ps.x) / 2, (hh - ps.y) / 2 - FromDIP(1));
  }
  if (collapsed_) return;
  // Rows.
  dc.SetFont(GetFont());
  const int rh = row_h();
  if (rows_.empty()) return;
  wxFont small = GetFont();
  small.SetFractionalPointSize(small.GetFractionalPointSize() * 0.85);
  for (int i = 0; i < visible_rows(); ++i) {
    const size_t r = static_cast<size_t>(top_ + i);
    if (r >= rows_.size()) break;
    const SectionRow& row = rows_[r];
    const int y = hh + i * rh;
    if (r == hot_) {
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.SetBrush(wxBrush(mix(bg, fg, 0.08)));
      dc.DrawRectangle(0, y, sz.x, rh);
    }
    int right = sz.x - pad;
    // Chip at the right edge.
    if (!row.chip.empty()) {
      dc.SetFont(small);
      const wxString c = U(row.chip);
      const wxSize cs = dc.GetTextExtent(c);
      const int cw = cs.x + FromDIP(10), ch = std::min(rh - FromDIP(4), cs.y + FromDIP(4));
      const int cx = right - cw;
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.SetBrush(wxBrush(mix(bg, fg, r == hot_ ? 0.16 : 0.10)));
      dc.DrawRoundedRectangle(cx, y + (rh - ch) / 2, cw, ch, FromDIP(4));
      dc.SetTextForeground(row.dim ? dim : fg);
      dc.DrawText(c, cx + FromDIP(5), y + (rh - cs.y) / 2);
      right = cx - FromDIP(6);
      dc.SetFont(GetFont());
    }
    // Status dot.
    if (row.dot.IsOk()) {
      std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
      if (gc) {
        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(row.dot));
        const double d = FromDIP(7);
        gc->DrawEllipse(pad + FromDIP(3) - d / 2 + 0.5, y + rh / 2.0 - d / 2, d, d);
      }
    }
    // Label, then the dimmed detail in what is left.
    const int avail = right - tx;
    if (avail <= 0) continue;
    dc.SetTextForeground(row.dim ? dim : fg);
    const wxString label = wxControl::Ellipsize(U(row.label), dc, wxELLIPSIZE_END, avail);
    const wxSize ls = dc.GetTextExtent(label);
    dc.DrawText(label, tx, y + (rh - ls.y) / 2);
    if (!row.detail.empty() && ls.x + FromDIP(8) < avail) {
      dc.SetTextForeground(dim);
      const wxString d = wxControl::Ellipsize(U(row.detail), dc, wxELLIPSIZE_END, avail - ls.x - FromDIP(8));
      if (d.length() > 1) dc.DrawText(d, tx + ls.x + FromDIP(8), y + (rh - ls.y) / 2);
    }
  }
  // More rows than shown: a thin scroll hint at the right edge.
  if (static_cast<int>(rows_.size()) > visible_rows()) {
    const int total = static_cast<int>(rows_.size()), vis = visible_rows();
    const int track = vis * rh;
    const int len = std::max(FromDIP(8), track * vis / total);
    const int pos = hh + (track - len) * top_ / std::max(1, total - vis);
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(mix(bg, fg, 0.25)));
    dc.DrawRoundedRectangle(sz.x - FromDIP(3), pos, FromDIP(2), len, FromDIP(1));
  }
}

// ------------------------------------------------------------------ spec preview

namespace {

struct Op {
  std::string method, path, name, tag;
};

void collect_ops(const ImportFolder& f, const std::string& tag, std::vector<Op>& out) {
  for (const auto& r : f.requests) {
    Op op;
    op.method = r.http.method.empty() ? "GET" : r.http.method;
    op.path = spec_op_path(r.http.url);
    op.name = r.name;
    op.tag = tag;
    out.push_back(std::move(op));
  }
  for (const auto& sub : f.folders) collect_ops(sub, sub.name, out);
}

class OpList : public wxListCtrl {
 public:
  explicit OpList(wxWindow* parent)
      : wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                   wxLC_REPORT | wxLC_VIRTUAL | wxLC_SINGLE_SEL | wxBORDER_NONE) {
    AppendColumn("Method", wxLIST_FORMAT_LEFT, FromDIP(64));
    AppendColumn("Path", wxLIST_FORMAT_LEFT, FromDIP(220));
    AppendColumn("Operation", wxLIST_FORMAT_LEFT, FromDIP(200));
    AppendColumn("Tag", wxLIST_FORMAT_LEFT, FromDIP(90));
  }
  std::vector<Op> ops;

 private:
  wxString OnGetItemText(long item, long col) const override {
    if (item < 0 || static_cast<size_t>(item) >= ops.size()) return {};
    const Op& o = ops[static_cast<size_t>(item)];
    switch (col) {
      case 0: return U(o.method);
      case 1: return U(o.path);
      case 2: return U(o.name);
      case 3: return U(o.tag);
      default: return {};
    }
  }
};

std::map<std::wstring, wxWindow*>& open_specs() {
  static std::map<std::wstring, wxWindow*> m;
  return m;
}

std::wstring spec_key(const fs::path& p) {
  std::wstring k = p.lexically_normal().native();
  for (auto& c : k) c = static_cast<wchar_t>(towlower(c));
  return k;
}

constexpr size_t kPreviewText = 4u << 20;  // text shown (a bigger spec is listed whole, shown in part)

class SpecPreview : public wxPanel {
 public:
  SpecPreview(wxWindow* parent, fs::path file, std::function<void()> sync, std::function<void()> reveal)
      : wxPanel(parent, wxID_ANY), file_(std::move(file)), sync_(std::move(sync)), reveal_(std::move(reveal)) {
    key_ = spec_key(file_);
    build();
    load();
  }
  ~SpecPreview() override { open_specs().erase(key_); }

  wxString tab_title() const { return wxString::FromUTF8("Spec: ") + wxString(file_.filename().wstring()); }
  void set_target(const std::string& t) {
    target_->SetLabel(U(t));
    Layout();
  }
  std::function<void(const std::string&, const std::string&)> open_op;

 private:
  void build() {
    const bool dark = is_dark();
    const wxColour bg = window_colour(), fg = text_colour();
    SetBackgroundColour(bg);
    SetForegroundColour(fg);
    auto* top = new wxBoxSizer(wxVERTICAL);
    const int gap = FromDIP(6);
    auto* bar = new wxBoxSizer(wxHORIZONTAL);
    title_ = new wxStaticText(this, wxID_ANY, wxString(file_.filename().wstring()), wxDefaultPosition, wxDefaultSize,
                              wxST_NO_AUTORESIZE | wxST_ELLIPSIZE_END);
    title_->SetFont(GetFont().Bold().Scaled(1.15f));
    bar->Add(title_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(14));
    using http_editor::FlatButton;
    auto* sync = new FlatButton(this, "Sync requests", FlatButton::Style::Primary);
    sync->SetToolTip(
        "Import the spec's operations again into its folder: new operations are added, requests you edited are kept");
    sync->on_click = [this] {
      if (sync_) sync_();
    };
    bar->Add(sync, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    auto* reload = new FlatButton(this, "Reload", FlatButton::Style::Outline);
    reload->SetToolTip("Read the file again");
    reload->on_click = [this] { load(); };
    bar->Add(reload, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    auto* reveal = new FlatButton(this, "Reveal", FlatButton::Style::Outline);
    reveal->SetToolTip("Show the file in Explorer");
    reveal->on_click = [this] {
      if (reveal_) reveal_();
    };
    bar->Add(reveal, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(bar, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    info_ = new wxStaticText(this, wxID_ANY, wxString::FromUTF8("Reading\xE2\x80\xA6"), wxDefaultPosition, wxDefaultSize,
                             wxST_NO_AUTORESIZE | wxST_ELLIPSIZE_MIDDLE);
    info_->SetForegroundColour(dim_colour());
    top->Add(info_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    target_ = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE | wxST_ELLIPSIZE_END);
    target_->SetForegroundColour(dim_colour());
    top->Add(target_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    top->AddSpacer(FromDIP(2));

    auto* split = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSP_LIVE_UPDATE | wxSP_NOBORDER);
    split->SetBackgroundColour(bg);
    split->SetMinimumPaneSize(FromDIP(120));
    split->SetSashGravity(0.45);
    ops_ = new OpList(split);
    ops_->SetBackgroundColour(bg);
    ops_->SetForegroundColour(fg);
    text_ = new wxStyledTextCtrl(split, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    const wxFont mono(wxFontInfo(GetFont().GetPointSize()).Family(wxFONTFAMILY_TELETYPE).FaceName("Consolas"));
    text_->StyleSetFont(wxSTC_STYLE_DEFAULT, mono);
    text_->StyleSetBackground(wxSTC_STYLE_DEFAULT, bg);
    text_->StyleSetForeground(wxSTC_STYLE_DEFAULT, fg);
    text_->StyleClearAll();
    text_->SetCaretForeground(fg);
    text_->SetSelBackground(true, dark ? wxColour(0x26, 0x4F, 0x78) : wxColour(0xAD, 0xD6, 0xFF));
    text_->SetMarginWidth(0, FromDIP(44));
    text_->SetMarginType(0, wxSTC_MARGIN_NUMBER);
    text_->StyleSetBackground(wxSTC_STYLE_LINENUMBER, bg);
    text_->StyleSetForeground(wxSTC_STYLE_LINENUMBER, dim_colour());
    text_->SetMarginWidth(1, 0);
    text_->SetWrapMode(wxSTC_WRAP_NONE);
    text_->SetReadOnly(true);
    // Split once the tab has its size (made at the panel's initial size, the sash landed at the
    // right edge): the operations take 45%, the text the rest.
    split->Initialize(ops_);
    text_->Hide();
    split->Bind(wxEVT_SIZE, [this, split](wxSizeEvent& e) {
      e.Skip();
      if (split->IsSplit() || split->GetClientSize().x < FromDIP(300)) return;
      const int pos = split->GetClientSize().x * 45 / 100;
      split->CallAfter([split, this, pos] {
        if (!split->IsSplit()) split->SplitVertically(ops_, text_, pos);
      });
    });
    top->Add(split, 1, wxEXPAND | wxTOP, FromDIP(8));
    SetSizer(top);
    ops_->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent& e) { show_op(e.GetIndex()); });
    // Double-click / Enter: the request synced from that operation opens (Bruno, Insomnia).
    ops_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent& e) {
      const long i = e.GetIndex();
      if (!open_op || i < 0 || static_cast<size_t>(i) >= ops_->ops.size()) return;
      const Op& o = ops_->ops[static_cast<size_t>(i)];
      open_op(o.method, o.path);
    });
    ops_->SetToolTip("Click: show the operation in the document. Double-click or Enter: open its request");
  }

  void style_for(bool yaml) {
    const bool dark = is_dark();
    // Keys, values, numbers and keywords each in a clearly different hue (a pale blue next to
    // light gray text read as plain white): VS Code's YAML / JSON palette.
    const wxColour key = dark ? wxColour(0x56, 0x9C, 0xD6) : wxColour(0x04, 0x51, 0xA5);
    const wxColour str = dark ? wxColour(0xCE, 0x91, 0x78) : wxColour(0xA3, 0x15, 0x15);
    const wxColour num = dark ? wxColour(0xB5, 0xCE, 0xA8) : wxColour(0x09, 0x86, 0x58);
    const wxColour kw = dark ? wxColour(0xC5, 0x86, 0xC0) : wxColour(0xAF, 0x00, 0xDB);
    const wxColour com = dark ? wxColour(0x6A, 0x99, 0x55) : wxColour(0x00, 0x80, 0x00);
    const wxColour punct = dim_colour();
    if (yaml) {
      text_->SetLexer(wxSTC_LEX_YAML);
      text_->StyleSetForeground(wxSTC_YAML_DEFAULT, str);  // plain scalars: values
      text_->StyleSetForeground(wxSTC_YAML_TEXT, str);
      text_->StyleSetForeground(wxSTC_YAML_IDENTIFIER, key);
      text_->StyleSetForeground(wxSTC_YAML_COMMENT, com);
      text_->StyleSetForeground(wxSTC_YAML_NUMBER, num);
      text_->StyleSetForeground(wxSTC_YAML_KEYWORD, kw);
      text_->StyleSetForeground(wxSTC_YAML_REFERENCE, kw);
      text_->StyleSetForeground(wxSTC_YAML_DOCUMENT, com);
      text_->StyleSetForeground(wxSTC_YAML_OPERATOR, punct);
    } else {
      text_->SetLexer(wxSTC_LEX_JSON);
      text_->StyleSetForeground(wxSTC_JSON_PROPERTYNAME, key);
      text_->StyleSetForeground(wxSTC_JSON_STRING, str);
      text_->StyleSetForeground(wxSTC_JSON_NUMBER, num);
      text_->StyleSetForeground(wxSTC_JSON_KEYWORD, kw);
      text_->StyleSetForeground(wxSTC_JSON_OPERATOR, punct);
    }
  }

  void load() {
    info_->SetLabel(wxString::FromUTF8("Reading\xE2\x80\xA6"));
    wxWeakRef<SpecPreview> self(this);
    std::thread([self, file = file_] {
      const auto t0 = std::chrono::steady_clock::now();
      auto text = std::make_shared<std::string>();
      auto err = std::make_shared<std::string>();
      auto ops = std::make_shared<std::vector<Op>>();
      auto kind = std::make_shared<std::string>();
      uint64_t size = 0;
      {
        std::error_code ec;
        size = fs::file_size(file, ec);
        if (ec) *err = "Cannot read the file (" + ec.message() + ")";
        else if (size > (64u << 20)) *err = "The file is larger than 64 MB";
      }
      if (err->empty()) {
        std::ifstream in(file, std::ios::binary);
        text->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        std::string perr;
        if (auto r = parse_collection_text(*text, &perr)) {
          *kind = r->format + (r->root.name.empty() ? "" : " \xC2\xB7 " + r->root.name);
          collect_ops(r->root, "", *ops);
        } else {
          *err = "Not an OpenAPI document this version can read: " + perr;
        }
      }
      const bool cut = text->size() > kPreviewText;
      if (cut) {
        size_t n = kPreviewText;
        while (n > 0 && (static_cast<unsigned char>((*text)[n]) & 0xC0) == 0x80) --n;
        text->resize(n);
      }
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      if (wxTheApp)
        wxTheApp->CallAfter([self, text, err, ops, kind, size, cut, ms] {
          if (!self) return;
          self->loaded(*text, *err, std::move(*ops), *kind, size, cut, ms);
        });
    }).detach();
  }

  void loaded(const std::string& text, const std::string& err, std::vector<Op> ops, const std::string& kind,
              uint64_t size, bool cut, double ms) {
    const std::string ext = [&] {
      std::string e = file_.extension().string();
      for (auto& c : e) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
      return e;
    }();
    size_t first = text.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    const bool yaml = !(first != std::string::npos && (text[first] == '{' || text[first] == '['));
    style_for(yaml && ext != ".json");
    text_->SetReadOnly(false);
    // Off the paint path: one SetText (a 4 MB document is ~20 ms in Scintilla).
    text_->SetText(wxString::FromUTF8(text.data(), text.size()));
    text_->SetReadOnly(true);
    text_->EmptyUndoBuffer();
    char sz[64];
    if (size >= (1u << 20)) std::snprintf(sz, sizeof sz, "%.1f MB", size / 1048576.0);
    else std::snprintf(sz, sizeof sz, "%.1f KB", size / 1024.0);
    std::string line = err.empty() ? kind : err;
    line += " \xC2\xB7 " + std::to_string(ops.size()) + (ops.size() == 1 ? " operation" : " operations") + " \xC2\xB7 " +
            sz;
    char t[48];
    std::snprintf(t, sizeof t, " \xC2\xB7 read in %.0f ms", ms);
    line += t;
    if (cut) line += " \xC2\xB7 first 4 MB shown";
    info_->SetLabel(U(line));
    info_->SetToolTip(wxString(file_.wstring()));
    info_->SetForegroundColour(err.empty() ? dim_colour() : wxColour(0xD0, 0x40, 0x40));
    ops_->ops = std::move(ops);
    ops_->SetItemCount(static_cast<long>(ops_->ops.size()));
    ops_->Refresh();
    Layout();
  }

  // The operation's line in the text: its path key, then the method under it.
  void show_op(long i) {
    if (i < 0 || static_cast<size_t>(i) >= ops_->ops.size()) return;
    const Op& o = ops_->ops[static_cast<size_t>(i)];
    const int len = text_->GetLength();
    std::string m = o.method;
    for (auto& c : m) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    int at = -1;
    for (const std::string& pat : {o.path + ":", "\"" + o.path + "\""}) {
      at = text_->FindText(0, len, U(pat), wxSTC_FIND_MATCHCASE);
      if (at >= 0) break;
    }
    if (at < 0) return;
    int mat = -1;
    for (const std::string& pat : {m + ":", "\"" + m + "\""}) {
      mat = text_->FindText(at, std::min(len, at + 200000), U(pat), wxSTC_FIND_MATCHCASE);
      if (mat >= 0) break;
    }
    const int pos = mat >= 0 ? mat : at;
    const int line = text_->LineFromPosition(pos);
    text_->SetFirstVisibleLine(std::max(0, line - 3));
    text_->SetSelection(text_->PositionFromLine(line), text_->GetLineEndPosition(line));
  }

  fs::path file_;
  std::wstring key_;
  std::function<void()> sync_, reveal_;
  wxStaticText* title_ = nullptr;
  wxStaticText* info_ = nullptr;
  wxStaticText* target_ = nullptr;
  OpList* ops_ = nullptr;
  wxStyledTextCtrl* text_ = nullptr;
};

}  // namespace

wxWindow* open_spec_preview(wxAuiNotebook* nb, const fs::path& file, const std::string& target,
                            std::function<void()> sync, std::function<void()> reveal) {
  if (!nb) return nullptr;
  auto& open = open_specs();
  const std::wstring key = spec_key(file);
  if (auto it = open.find(key); it != open.end()) {
    if (const int i = nb->GetPageIndex(it->second); i != wxNOT_FOUND) {
      static_cast<SpecPreview*>(it->second)->set_target(target);
      return it->second;
    }
    open.erase(it);
  }
  auto* p = new SpecPreview(nb, file, std::move(sync), std::move(reveal));
  p->set_target(target);
  open[key] = p;
  // Added unselected: showing it (the notebook's layout of the page) is the caller's next UI
  // step, so building the tab and showing it never add up in one step.
  nb->AddPage(p, p->tab_title(), false);
  return p;
}

wxWindow* spec_preview_for(const fs::path& file) {
  auto& open = open_specs();
  auto it = open.find(spec_key(file));
  return it == open.end() ? nullptr : it->second;
}

void spec_preview_set_target(wxWindow* preview, const std::string& target) {
  if (auto* p = dynamic_cast<SpecPreview*>(preview)) p->set_target(target);
}

bool is_spec_preview(wxWindow* w) { return dynamic_cast<SpecPreview*>(w) != nullptr; }

void spec_preview_set_open_op(wxWindow* preview, std::function<void(const std::string&, const std::string&)> fn) {
  if (auto* p = dynamic_cast<SpecPreview*>(preview)) p->open_op = std::move(fn);
}

}  // namespace watt::app::sidebar
