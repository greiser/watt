#include "app/response_tests_view.h"

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/clipbrd.h>
#include <wx/combobox.h>
#include <wx/dcclient.h>
#include <wx/listctrl.h>
#include <wx/menu.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/wupdlock.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstdlib>

#include "app/response_format.h"
#include "app/response_tests.h"
#include "app/response_widgets.h"

namespace watt::app::rv {
namespace {

wxString U(std::string_view s) { return wxString::FromUTF8(s.data(), s.size()); }

// WATT_RV_TRACE=<file> (as in response_view.cpp): "tests.build.<slice> ms" lines for the page's
// build slices, so its first show can be measured in the running app.
FILE* trace_file() {
  static FILE* f = []() -> FILE* {
    const wchar_t* p = _wgetenv(L"WATT_RV_TRACE");
    return p && *p ? _wfopen(p, L"a") : nullptr;
  }();
  return f;
}

enum Chip { kAdd = 1, kEdit, kToggle, kDelete, kHintStatus = 20, kHintTime, kHintType, kHintJson };

struct OpDef {
  const char* id;
  const char* shown;
  bool value;
};
// The operators of core/asserts.h, in the order the editor offers them.
const OpDef kOps[] = {
    {"eq", "==", true},           {"neq", "!=", true},          {"lt", "<", true},
    {"lte", "<=", true},          {"gt", ">", true},            {"gte", ">=", true},
    {"between", "between", true}, {"in", "in", true},           {"notIn", "not in", true},
    {"contains", "contains", true}, {"notContains", "not contains", true}, {"startsWith", "starts with", true},
    {"endsWith", "ends with", true}, {"matches", "matches", true}, {"notMatches", "not matches", true},
    {"length", "length", true},   {"isDefined", "exists", false}, {"isUndefined", "not exists", false},
    {"isEmpty", "is empty", false}, {"isNotEmpty", "is not empty", false}, {"isNull", "is null", false},
    {"isTruthy", "is truthy", false}, {"isFalsy", "is falsy", false}, {"isJson", "is JSON", false},
    {"isNumber", "is number", false}, {"isString", "is string", false}, {"isBoolean", "is boolean", false},
    {"isArray", "is array", false},
};

int op_index(const std::string& op) {
  std::string o = op.empty() ? std::string("eq") : op;
  static const std::pair<const char*, const char*> kAlias[] = {
      {"==", "eq"}, {"!=", "neq"}, {"ne", "neq"}, {">", "gt"}, {">=", "gte"}, {"ge", "gte"}, {"<", "lt"},
      {"<=", "lte"}, {"le", "lte"}, {"exists", "isDefined"}, {"not_exists", "isUndefined"}};
  for (const auto& [a, b] : kAlias)
    if (o == a) o = b;
  for (size_t i = 0; i < std::size(kOps); ++i) {
    std::string x = kOps[i].id, y = o;
    std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(y.begin(), y.end(), y.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (x == y) return static_cast<int>(i);
  }
  return -1;
}

std::string op_label(const std::string& op) {
  const int i = op_index(op);
  return i < 0 ? op : kOps[i].shown;
}

}  // namespace

// ------------------------------------------------------------------ list

class TestList : public FastList {
 public:
  explicit TestList(TestsView* owner)
      : FastList(owner, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                   wxLC_REPORT | wxLC_VIRTUAL | wxLC_SINGLE_SEL | wxBORDER_NONE),
        v_(owner) {
    AppendColumn("Result", wxLIST_FORMAT_LEFT, FromDIP(70));
    AppendColumn("Target", wxLIST_FORMAT_LEFT, FromDIP(200));
    AppendColumn("Check", wxLIST_FORMAT_LEFT, FromDIP(150));
    AppendColumn("Actual", wxLIST_FORMAT_LEFT, FromDIP(220));
    AppendColumn("Note", wxLIST_FORMAT_LEFT, FromDIP(200));
    const Theme& th = Theme::get();
    pass_.SetTextColour(th.ok);
    fail_.SetTextColour(th.err);
    off_.SetTextColour(th.dim);
    pass_.SetFont(GetFont().Bold());
    fail_.SetFont(GetFont().Bold());
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      schedule_fit();
    });
  }
  void refresh_rows() {
    SetItemCount(static_cast<long>(v_->asserts_.size()));
    ++gen_;
    Refresh();
    schedule_fit();  // the columns follow what the rows hold
  }
  // The column widths for the current rows and width, now (computed once per change of either).
  void fit_now() { fit(); }
  // The rows' texts changed without a change of rows (the Result column while checking).
  void texts_changed() {
    ++gen_;
    Refresh();
  }

 private:
  void schedule_fit() {
    if (fit_pending_) return;
    fit_pending_ = true;
    CallAfter([this] {
      fit_pending_ = false;
      fit();
    });
  }
  // Result fixed. Target, Check and Actual get what their text needs (measured over the first
  // rows), as long as the Note keeps a sixth of the width (when any row has a note); when they
  // need more, the room is shared fairly (water-filling). The Note gets the rest.
  void fit() {
    const int r = FromDIP(64);
    const int w = std::max(FromDIP(300), GetClientSize().x - r - 2);
    // Same rows, same width: the widths computed last time still hold (no text measuring again).
    if (w == fit_w_ && gen_ == fit_gen_) return;
    fit_w_ = w;
    fit_gen_ = gen_;
    const int pad = FromDIP(16);
    static const char* const kHead[] = {"", "Target", "Check", "Actual"};
    int need[4] = {0, 0, 0, 0};
    for (int c = 1; c <= 3; ++c) need[c] = GetTextExtent(kHead[c]).x + pad;
    const long n = std::min<long>(GetItemCount(), 200);
    for (long i = 0; i < n; ++i)
      for (int c = 1; c <= 3; ++c) {
        // The Target as written, not as elided to the current width.
        const wxString t = c == 1 ? U(v_->asserts_[static_cast<size_t>(i)].target) : OnGetItemText(i, c);
        if (t.empty()) continue;
        // Long values are cut anyway: no need to measure more than ~120 characters.
        need[c] = std::max(need[c], GetTextExtent(t.length() > 120 ? t.Left(120) : t).x + pad);
      }
    const int lo = w * 10 / 100;  // never narrower than this
    for (int c = 1; c <= 3; ++c) need[c] = std::max(need[c], lo);
    // The Note keeps a quarter of the width only when some row has one (a failure's reason).
    bool any_note = false;
    for (long i = 0; i < n && !any_note; ++i) any_note = !OnGetItemText(i, 4).empty();
    const int avail = w - (any_note ? w * 18 / 100 : w * 6 / 100);
    // Water-filling: a column that needs less than an equal share gets all it needs, the rest
    // share what is left equally (a long Actual no longer squeezes a short Target to its floor).
    int cols[4] = {r, need[1], need[2], need[3]};
    {
      bool done[4] = {true, false, false, false};
      int left = avail, open = 3;
      for (bool changed = true; changed && open > 0;) {
        changed = false;
        const int share = left / open;
        for (int c = 1; c <= 3; ++c)
          if (!done[c] && need[c] <= share) {
            cols[c] = need[c];
            left -= need[c];
            done[c] = true;
            --open;
            changed = true;
          }
      }
      for (int c = 1; c <= 3; ++c)
        if (!done[c]) cols[c] = std::max(lo, left / std::max(open, 1));
    }
    const int note = std::max(any_note ? w * 14 / 100 : w * 6 / 100, w - cols[1] - cols[2] - cols[3]);
    const int widths[] = {cols[0], cols[1], cols[2], cols[3], note};
    // Frozen: every SetColumnWidth repaints the list at once otherwise (~50 ms each).
    Freeze();
    for (int c = 0; c < 5; ++c)
      if (GetColumnWidth(c) != widths[c]) SetColumnWidth(c, widths[c]);
    Thaw();
  }
  const TestRow* row(long i) const {
    return static_cast<size_t>(i) < v_->rows_.size() ? &v_->rows_[static_cast<size_t>(i)] : nullptr;
  }
  // The list asks for every cell's text several times per paint (and fit() measures them): the
  // texts are made once per change of rows or of the Target column's width, with one DC for the
  // elided targets, and served from here.
  wxString OnGetItemText(long item, long col) const override {
    if (item < 0 || static_cast<size_t>(item) >= v_->asserts_.size() || col < 0 || col > 4) return {};
    const int room = GetColumnWidth(1) - FromDIP(14);
    if (cache_gen_ != gen_ || cache_room_ != room || cache_.size() != v_->asserts_.size()) {
      cache_gen_ = gen_;
      cache_room_ = room;
      cache_.assign(v_->asserts_.size(), {});
      wxClientDC dc(const_cast<TestList*>(this));
      dc.SetFont(GetFont());
      for (size_t i = 0; i < cache_.size(); ++i)
        for (long c = 0; c < 5; ++c) cache_[i][static_cast<size_t>(c)] = make_text(static_cast<long>(i), c, dc, room);
    }
    return cache_[static_cast<size_t>(item)][static_cast<size_t>(col)];
  }
  wxString make_text(long item, long col, wxDC& dc, int room) const {
    const Assertion& a = v_->asserts_[static_cast<size_t>(item)];
    const TestRow* r = row(item);
    switch (col) {
      case 0: {
        if (!a.enabled) return "off";
        if (!r) return v_->running_ ? wxString::FromUTF8("\xE2\x80\xA6") : wxString::FromUTF8("\xE2\x80\x94");
        switch (r->state) {
          case TestRow::Pass: return wxString::FromUTF8("\xE2\x9C\x93 pass");
          case TestRow::Fail: return wxString::FromUTF8("\xE2\x9C\x97 FAIL");
          case TestRow::Off: return "off";
          default: return v_->running_ ? wxString::FromUTF8("\xE2\x80\xA6") : wxString::FromUTF8("\xE2\x80\x94");
        }
      }
      case 1: {
        // A long target keeps its end, the part that tells rows apart: "...items[2].owner.email".
        const wxString t = U(a.target);
        if (room <= 0 || dc.GetTextExtent(t).x <= room) return t;
        return wxControl::Ellipsize(t, dc, wxELLIPSIZE_START, room, wxELLIPSIZE_FLAGS_EXPAND_TABS);
      }
      case 2: {
        std::string s = op_label(a.op);
        const std::string& exp = r && !r->expected.empty() ? r->expected : a.value;
        if (!exp.empty()) s += " " + exp;
        return U(s);
      }
      case 3: {
        if (!r || r->state == TestRow::NotRun || r->state == TestRow::Off) return {};
        if (r->actual.empty()) return "(undefined)";
        // Times read as times: "1.1999" -> "1.20 ms".
        if (a.target.rfind("res.responseTime", 0) == 0 || a.target.rfind("res.timing.", 0) == 0) {
          char* end = nullptr;
          const double v = std::strtod(r->actual.c_str(), &end);
          if (end && *end == 0) return U(fmt::ms(v));
        }
        return U(r->actual);
      }
      default:
        if (!r) return {};
        // (Per-check evaluation time is not repeated here: the summary has the total, and the
        // column's room goes to Target and Actual.)
        // "expected res.body[3].name to equal nope, got item 3": the target is the row's own
        // Target column, so the note starts after it ("to equal nope, got item 3") and fits.
        if (const std::string head = "expected " + a.target + " ";
            r->state == TestRow::Fail && r->message.size() > head.size() && r->message.compare(0, head.size(), head) == 0)
          return U(r->message.substr(head.size()));
        return U(r->message);
    }
  }
  wxItemAttr* OnGetItemColumnAttr(long item, long col) const override {
    const Assertion& a = v_->asserts_[static_cast<size_t>(item)];
    const TestRow* r = row(item);
    if (!a.enabled || (r && r->state == TestRow::Off)) return &off_;
    if (!r) return nullptr;
    if (col == 0 || col == 3 || col == 4) {
      if (r->state == TestRow::Pass) return col == 0 ? &pass_ : nullptr;
      if (r->state == TestRow::Fail) return &fail_;
    }
    return nullptr;
  }
  wxItemAttr* OnGetItemAttr(long) const override { return nullptr; }
  TestsView* v_;
  mutable wxItemAttr pass_, fail_, off_;
  bool fit_pending_ = false;
  mutable std::vector<std::array<wxString, 5>> cache_;
  mutable unsigned cache_gen_ = ~0u;
  mutable int cache_room_ = -1;
  unsigned gen_ = 0, fit_gen_ = ~0u;  // rows changed / rows the widths were computed for
  int fit_w_ = -1;                     // width they were computed for
};

// ------------------------------------------------------------------ view

TestsView::TestsView(wxWindow* parent) : wxPanel(parent, wxID_ANY) { SetBackgroundColour(Theme::get().bg); }

void TestsView::ensure_built() {
  if (complete()) return;
  wxWindowUpdateLocker lock(this);
  while (!build_step()) {
  }
}

bool TestsView::build_step() {
  if (complete()) return true;
  const auto t0 = std::chrono::steady_clock::now();
  const int s = stage_++;  // (the apply_* below check stage_: the slice running counts as done)
  static const char* const kName[kStages] = {"chips", "list", "script", "captures", "inherited", "layout"};
  switch (s) {
    case 0: {
      bar_ = new ChipBar(this, [this](int id) { on_chip(id); });
  bar_->add(kAdd, "+ Add", "Add a check (Insert). Checks are saved with the request and run on every response.", -1,
            true);
  bar_->add(kEdit, "Edit", "Change the selected check (F2 or double-click)", -1, true);
  bar_->add(kToggle, "On/Off", "Enable or disable the selected check (Space)", -1, true);
  bar_->add(kDelete, "Delete", "Delete the selected check (Del)", -1, true);
  // Suggestions sit on their own row, left-aligned, and a used one is grayed out rather than
  // removed: the chip under the cursor never moves after a click.
  hints_ = new ChipBar(this, [this](int id) { on_chip(id); });
  hints_->add(kHintStatus, "+ status 2xx", "Add: the status is in this response's class");
  hints_->add(kHintTime, "+ time", "Add: response time below ~3x this response's");
  hints_->add(kHintType, "+ Content-Type", "Add: the Content-Type contains this response's type");
  hints_->add(kHintJson, "+ body is JSON", "Add: res.body isJson");
  hints_->Hide();
      break;
    }
    case 1: build_core(); break;
    case 2:
      if (script_.any_script || script_.skipped_untrusted || !script_.rows.empty()) apply_script();
      break;
    case 3:
      if (!cap_rows_.empty()) apply_captures();
      break;
    case 4:
      if (!inh_.empty()) apply_inherited();
      break;
    default: {
      // At the page's size (a hidden page of the book may never have been sized), with the
      // columns fitted now: the first show only paints.
      if (wxWindow* p = GetParent()) {
        const wxSize ps = p->GetClientSize();
        if (ps.x > 0 && ps.y > 0 && GetSize() != ps) SetSize(ps);
      }
      update_summary();
      Layout();
      list_->fit_now();
      if (script_list_) fit_script_columns();
      break;
    }
  }
  if (FILE* f = trace_file()) {
    std::fprintf(f, "tests.build.%s %.2f\n", kName[s],
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    std::fflush(f);
  }
  return complete();
}

// The assertion list, the empty-state text and the captures title, with their handlers.
void TestsView::build_core() {
  const Theme& th = Theme::get();
  list_ = new TestList(this);
  empty_ = new wxStaticText(this, wxID_ANY, "");
  empty_->SetForegroundColour(th.dim);
  caps_title_ = new wxStaticText(this, wxID_ANY, "Captures");
  caps_title_->SetForegroundColour(th.dim);
  caps_title_->SetFont(caps_title_->GetFont().Bold());
  caps_title_->Hide();

  auto* sz = new wxBoxSizer(wxVERTICAL);
  sz->Add(bar_, 0, wxEXPAND);
  sz->Add(hints_, 0, wxEXPAND);
  sz->Add(empty_, 0, wxALL, FromDIP(12));
  sz->Add(list_, 1, wxEXPAND);
  sz->Add(caps_title_, 0, wxLEFT | wxTOP | wxBOTTOM, FromDIP(8));
  SetSizer(sz);
  Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
    e.Skip();
    if (empty_->IsShown()) CallAfter([this] { wrap_empty(); });
  });

  list_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent& e) {
    const int i = static_cast<int>(e.GetIndex());
    const TestRow* r = static_cast<size_t>(i) < rows_.size() ? &rows_[static_cast<size_t>(i)] : nullptr;
    if (r && r->state == TestRow::Fail && on_reveal) return row_clicked(i);
    if (editable_) begin_edit(i);
  });
  list_->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {  // (the native list swallows LEFT_UP)
    e.Skip();
    int flags = 0;
    const long i = list_->HitTest(e.GetPosition(), flags);
    // One click on a failing check of a value in the body jumps to it; other rows just select
    // (double-click / Enter: edit, or show the headers a failing header check looked at).
    if (i < 0 || !(flags & wxLIST_HITTEST_ONITEM) || static_cast<size_t>(i) >= asserts_.size()) return;
    std::vector<PathStep> path;
    long msg = 0;
    if (!target_json_path(asserts_[static_cast<size_t>(i)].target, path, &msg)) return;
    CallAfter([this, i] { row_clicked(static_cast<int>(i)); });
  });
  // The full actual value and message of the row under the mouse (the columns cut them).
  list_->Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
    e.Skip();
    int flags = 0;
    const long i = list_->HitTest(e.GetPosition(), flags);
    if (i == tip_row_) return;
    tip_row_ = i;
    if (i < 0 || static_cast<size_t>(i) >= asserts_.size()) return list_->UnsetToolTip();
    const Assertion& a = asserts_[static_cast<size_t>(i)];
    std::string t = a.target + " " + op_label(a.op) + (a.value.empty() ? std::string() : " " + a.value);
    if (static_cast<size_t>(i) < rows_.size()) {
      const TestRow& r = rows_[static_cast<size_t>(i)];
      if (r.state == TestRow::Pass || r.state == TestRow::Fail) {
        t += std::string(r.state == TestRow::Pass ? "\npassed" : "\nFAILED") + "\nactual: " +
             (r.actual.empty() ? std::string("(undefined)") : r.actual);
        if (!r.message.empty()) t += "\n" + r.message;
        if (r.state == TestRow::Fail) t += "\n\nClick to show it in the Body.";
      }
    }
    list_->SetToolTip(U(t));
  });
  list_->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) {
    e.Skip();
    tip_row_ = -2;
  });
  list_->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent&) { update_chips(); });
  list_->Bind(wxEVT_LIST_ITEM_DESELECTED, [this](wxListEvent&) { update_chips(); });
  list_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
    const int k = e.GetKeyCode();
    if (!editable_) return e.Skip();
    if (k == WXK_DELETE) return delete_selected();
    if (k == WXK_SPACE) return toggle_selected();
    if (k == WXK_F2) return begin_edit(selected());
    if (k == WXK_INSERT) return begin_edit(-1);
    if (e.GetModifiers() == wxMOD_ALT && k == WXK_UP) return move_selected(-1);
    if (e.GetModifiers() == wxMOD_ALT && k == WXK_DOWN) return move_selected(1);
    e.Skip();
  });
  list_->Bind(wxEVT_CONTEXT_MENU, [this](wxContextMenuEvent&) {
    const int i = selected();
    wxMenu m;
    if (i >= 0 && static_cast<size_t>(i) < rows_.size() && rows_[static_cast<size_t>(i)].state == TestRow::Fail)
      m.Append(5, "Show in Body");
    if (editable_) {
      m.Append(1, "Add check\tIns");
      if (i >= 0) {
        m.Append(2, "Edit\tF2");
        m.Append(3, asserts_[static_cast<size_t>(i)].enabled ? "Disable\tSpace" : "Enable\tSpace");
        m.Append(6, "Move up\tAlt+Up");
        m.Append(7, "Move down\tAlt+Down");
        m.Append(4, "Delete\tDel");
      }
    }
    const int id = GetPopupMenuSelectionFromUser(m);
    if (id == 1) begin_edit(-1);
    else if (id == 2) begin_edit(i);
    else if (id == 3) toggle_selected();
    else if (id == 4) delete_selected();
    else if (id == 5) row_clicked(i);
    else if (id == 6) move_selected(-1);
    else if (id == 7) move_selected(1);
  });
  list_->refresh_rows();
  update_summary();
  update_chips();
}

// The add / edit form: created the first time a check is added or edited.
void TestsView::build_editor() {
  if (editor_) return;
  const Theme& th = Theme::get();
  editor_ = new wxPanel(this, wxID_ANY);
  editor_->SetBackgroundColour(th.bg);
  target_ = new wxComboBox(editor_, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(160, -1)), 0, nullptr,
                           wxTE_PROCESS_ENTER);
  target_->SetHint("res.body.items[0].id");
  wxArrayString ops;
  for (const OpDef& o : kOps) ops.Add(U(std::string(o.shown) + "   (" + o.id + ")"));
  op_ = new wxChoice(editor_, wxID_ANY, wxDefaultPosition, wxDefaultSize, ops);
  op_->SetSelection(0);
  value_ = new wxTextCtrl(editor_, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(160, -1)), wxTE_PROCESS_ENTER);
  value_->SetHint("200, \"text\", 10, 20 ... ({{var}} ok)");
  auto* save = new wxButton(editor_, wxID_ANY, "Save", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
  auto* cancel = new wxButton(editor_, wxID_ANY, "Cancel", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
  edit_err_ = new wxStaticText(editor_, wxID_ANY, "");
  edit_err_->SetForegroundColour(th.err);
  // Two rows, so each field keeps a usable width in a narrow response pane:
  //   Target   [res.body.items[0].name        ] [equals (eq)  v]
  //   Expected ["item 9"                      ] [Save] [Cancel]
  auto label = [&](const char* t) {
    auto* l = new wxStaticText(editor_, wxID_ANY, t);
    l->SetForegroundColour(th.dim);
    return l;
  };
  auto* lt = label("Target");
  auto* lv = label("Expected");
  const int lw = std::max(lt->GetBestSize().x, lv->GetBestSize().x);
  lt->SetMinSize(wxSize(lw, -1));
  lv->SetMinSize(wxSize(lw, -1));
  auto* es = new wxBoxSizer(wxHORIZONTAL);
  es->Add(lt, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
  es->Add(target_, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
  es->Add(op_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(6));
  auto* es2 = new wxBoxSizer(wxHORIZONTAL);
  es2->Add(lv, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
  es2->Add(value_, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
  es2->Add(save, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
  es2->Add(cancel, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(6));
  auto* ev = new wxBoxSizer(wxVERTICAL);
  ev->Add(es, 0, wxEXPAND | wxTOP, FromDIP(4));
  ev->Add(es2, 0, wxEXPAND | wxTOP, FromDIP(4));
  ev->Add(edit_err_, 0, wxLEFT | wxTOP | wxBOTTOM, FromDIP(8));
  editor_->SetSizer(ev);
  editor_->Hide();
  GetSizer()->Insert(2, editor_, 0, wxEXPAND);

  save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { commit_edit(); });
  cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { cancel_edit(); });
  target_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { commit_edit(); });
  value_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { commit_edit(); });
  target_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { edit_err_->SetLabel(""); });
  op_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
    const int i = op_->GetSelection();
    value_->Enable(i < 0 || kOps[i].value);
  });
  editor_->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
    if (e.GetKeyCode() == WXK_ESCAPE) return cancel_edit();
    e.Skip();
  });
  fill_targets();
}

// The capture table: created with the first capture to show.
void TestsView::apply_captures() {
  if (stage_ < 4) return;  // (built by its slice)
  const Theme& th = Theme::get();
  if (!caps_) {
    caps_ = new FastList(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_NONE);
    caps_->AppendColumn("Result", wxLIST_FORMAT_LEFT, FromDIP(64));
    caps_->AppendColumn("Variable", wxLIST_FORMAT_LEFT, FromDIP(120));
    caps_->AppendColumn("Scope", wxLIST_FORMAT_LEFT, FromDIP(80));
    caps_->AppendColumn("From", wxLIST_FORMAT_LEFT, FromDIP(160));
    caps_->AppendColumn("Value / why not", wxLIST_FORMAT_LEFT, FromDIP(220));
    caps_->SetBackgroundColour(th.bg);
    caps_->SetForegroundColour(th.fg);
    caps_->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      if (caps_fit_pending_) return;
      caps_fit_pending_ = true;
      CallAfter([this] {
        caps_fit_pending_ = false;
        const int r = FromDIP(64);
        const int w = std::max(FromDIP(300), caps_->GetClientSize().x - r - 2);
        const int widths[] = {r, w * 22 / 100, w * 13 / 100, w * 28 / 100, w - w * 22 / 100 - w * 13 / 100 - w * 28 / 100};
        caps_->Freeze();  // (each width change would repaint the list at once)
        for (int i = 0; i < 5; ++i)
          if (caps_->GetColumnWidth(i) != widths[i]) caps_->SetColumnWidth(i, widths[i]);
        caps_->Thaw();
      });
    });
    // Ctrl+C / double-click copies the selected capture's value.
    auto copy_cap = [this] {
      const long i = caps_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
      if (i < 0) return;
      const wxString v = caps_->GetItemText(i, 4);
      if (wxTheClipboard->Open()) {
        wxTheClipboard->SetData(new wxTextDataObject(v));
        wxTheClipboard->Close();
      }
    };
    caps_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [copy_cap](wxListEvent&) { copy_cap(); });
    caps_->Bind(wxEVT_KEY_DOWN, [copy_cap](wxKeyEvent& e) {
      if (e.GetModifiers() == wxMOD_CONTROL && e.GetKeyCode() == 'C') return copy_cap();
      e.Skip();
    });
    GetSizer()->Add(caps_, 0, wxEXPAND);
  }
  const std::vector<CaptureRow>& rows = cap_rows_;
  caps_->Freeze();
  caps_->DeleteAllItems();
  int stored = 0, failed = 0;
  for (size_t i = 0; i < rows.size(); ++i) {
    const CaptureRow& r = rows[i];
    const long at = static_cast<long>(i);
    std::string res = !r.enabled ? "off"
                      : r.state == CaptureRow::Stored    ? "\xE2\x9C\x93 set"
                      : r.state == CaptureRow::Failed    ? "\xE2\x9C\x97 not set"
                      : r.state == CaptureRow::Previewed ? "preview"
                                                         : "\xE2\x80\x94";
    caps_->InsertItem(at, U(res));
    caps_->SetItem(at, 1, U("{{" + r.var + "}}"));
    caps_->SetItem(at, 2, U(r.scope));
    caps_->SetItem(at, 3, U(r.target));
    std::string v = r.state == CaptureRow::Failed ? r.message : r.value;
    if (r.state == CaptureRow::Previewed) v += "   (earlier run: not stored)";
    if (v.size() > 300) v = v.substr(0, 300) + "\xE2\x80\xA6";
    caps_->SetItem(at, 4, U(v));
    const wxColour c = !r.enabled ? th.dim
                       : r.state == CaptureRow::Stored ? th.ok
                       : r.state == CaptureRow::Failed ? th.err
                                                       : th.dim;
    caps_->SetItemTextColour(at, c);
    stored += r.state == CaptureRow::Stored;
    failed += r.state == CaptureRow::Failed;
  }
  caps_->Thaw();
  const bool show = !rows.empty();
  std::string title = "Captures";
  if (stored || failed) {
    title += "  \xC2\xB7  " + std::to_string(stored) + " set";
    if (failed) title += ", " + std::to_string(failed) + " not set";
  }
  caps_title_->SetLabel(U(title));
  caps_title_->SetToolTip("Captures copy response values into {{variables}} for the next requests.\n"
                          "Double-click a row or press Ctrl+C to copy its value.");
  caps_title_->SetForegroundColour(failed ? th.warn : th.dim);
  const int rh = FromDIP(22);
  caps_->SetMinSize(wxSize(-1, rh * (std::min<int>(static_cast<int>(rows.size()), 6) + 1) + FromDIP(4)));
  caps_title_->Show(show);
  caps_->Show(show);
  Layout();
}

// The Script group: created with the first script result to show, between the assertions and
// the captures.
void TestsView::apply_script() {
  if (stage_ < 3) return;  // (built by its slice)
  const Theme& th = Theme::get();
  const ScriptResults& sr = script_;
  const bool show = sr.any_script || sr.skipped_untrusted || !sr.rows.empty();
  if (!show && !script_list_) return;
  if (!script_title_) {
    script_title_ = new wxStaticText(this, wxID_ANY, "Script");
    script_title_->SetFont(script_title_->GetFont().Bold());
    script_title_->SetToolTip("Results of the request's scripts: test() / pm.test() calls and uncaught errors "
                              "(Scripts tab of the request). Their console output is in the Log tab.");
    script_list_ = new FastList(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                  wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_NONE);
    script_list_->AppendColumn("Result", wxLIST_FORMAT_LEFT, FromDIP(64));
    script_list_->AppendColumn("Test", wxLIST_FORMAT_LEFT, FromDIP(240));
    script_list_->AppendColumn("Script", wxLIST_FORMAT_LEFT, FromDIP(100));
    script_list_->AppendColumn("Message", wxLIST_FORMAT_LEFT, FromDIP(300));
    script_list_->SetBackgroundColour(th.bg);
    script_list_->SetForegroundColour(th.fg);
    script_list_->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      if (script_fit_pending_) return;
      script_fit_pending_ = true;
      CallAfter([this] {
        script_fit_pending_ = false;
        fit_script_columns();
      });
    });
    // The full message of the row under the mouse (the column cuts it); Ctrl+C copies it.
    script_list_->Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
      e.Skip();
      int flags = 0;
      const long i = script_list_->HitTest(e.GetPosition(), flags);
      if (i < 0 || static_cast<size_t>(i) >= script_.rows.size()) return script_list_->UnsetToolTip();
      const ScriptTestRow& r = script_.rows[static_cast<size_t>(i)];
      script_list_->SetToolTip(
          U(r.name + " (" + r.phase + " script)" + (r.message.empty() ? std::string() : "\n" + r.message)));
    });
    script_list_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
      if (e.GetModifiers() == wxMOD_CONTROL && e.GetKeyCode() == 'C') {
        const long i = script_list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (i >= 0 && static_cast<size_t>(i) < script_.rows.size() && wxTheClipboard->Open()) {
          const ScriptTestRow& r = script_.rows[static_cast<size_t>(i)];
          wxTheClipboard->SetData(new wxTextDataObject(U(r.name + (r.message.empty() ? "" : ": " + r.message))));
          wxTheClipboard->Close();
        }
        return;
      }
      e.Skip();
    });
    // After the assertion list, before the captures.
    wxSizer* sz = GetSizer();
    size_t at = sz->GetItemCount();
    for (size_t i = 0; i < sz->GetItemCount(); ++i)
      if (sz->GetItem(i)->GetWindow() == caps_title_) at = i;
    script_err_ = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE);
    script_err_->SetForegroundColour(th.err);
    script_err_->Hide();
    sz->Insert(at, script_err_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    sz->Insert(at, script_list_, 0, wxEXPAND);
    sz->Insert(at, script_title_, 0, wxLEFT | wxTOP | wxBOTTOM, FromDIP(8));
    // Re-wrapped when the pane width changes (after the layout, once per burst of size events).
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      if (script_err_ && script_err_->IsShown()) CallAfter([this] { wrap_script_err(); });
    });
  }
  {
    // A script error or failed test message too long for the list's Message column (or on several
    // lines) in full under the list, one per line ("post-response: line 3:1: ReferenceError: ...").
    // One that fits is said once, in its row.
    std::string t;
    size_t shown = 0;
    for (const ScriptTestRow& r : sr.rows) {
      if (r.state == ScriptTestRow::Pass || r.message.empty()) continue;
      if (r.message.size() <= 90 && r.message.find('\n') == std::string::npos) continue;
      if (++shown > 5) {
        t += "\xE2\x80\xA6";
        break;
      }
      std::string msg = r.message.size() > 600 ? r.message.substr(0, 600) + "\xE2\x80\xA6" : r.message;
      for (char& c : msg)
        if (c == '\r' || c == '\t') c = ' ';
      if (!t.empty()) t += '\n';
      t += "\xE2\x9C\x97 " + (r.state == ScriptTestRow::Error ? r.phase + " script" : r.name) + ": " + msg;
    }
    script_err_text_ = std::move(t);
    script_err_->Show(!script_err_text_.empty());
    if (!script_err_text_.empty()) wrap_script_err();
  }
  script_list_->Freeze();
  script_list_->DeleteAllItems();
  const size_t n = std::min<size_t>(sr.rows.size(), 1000);
  for (size_t i = 0; i < n; ++i) {
    const ScriptTestRow& r = sr.rows[i];
    const long at = static_cast<long>(i);
    script_list_->InsertItem(at, U(r.state == ScriptTestRow::Pass   ? "\xE2\x9C\x93 pass"
                                   : r.state == ScriptTestRow::Fail ? "\xE2\x9C\x97 FAIL"
                                                                    : "\xE2\x9C\x97 error"));
    std::string msg = r.message;
    if (msg.size() > 400) msg = msg.substr(0, 400) + "\xE2\x80\xA6";
    for (char& c : msg)
      if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    script_list_->SetItem(at, 1, U(r.name));
    script_list_->SetItem(at, 2, U(r.phase));
    script_list_->SetItem(at, 3, U(msg));
    script_list_->SetItemTextColour(at, r.state == ScriptTestRow::Pass ? th.fg : th.err);
    if (r.state != ScriptTestRow::Pass) script_list_->SetItemFont(at, script_list_->GetFont().Bold());
  }
  script_list_->Thaw();
  std::string title = "Script";
  wxColour colour = th.dim;
  if (sr.skipped_untrusted) {
    title += "  \xC2\xB7  not run (workspace not trusted)";
    colour = th.warn;
  } else if (sr.rows.empty()) {
    title += "  \xC2\xB7  no test() calls";
  } else {
    title += "  \xC2\xB7  " + std::to_string(script_pass_) + " of " + std::to_string(script_pass_ + script_fail_) +
             " passed";
    if (script_fail_) title += ", " + std::to_string(script_fail_) + " failed";
    colour = script_fail_ ? th.err : th.ok;
  }
  if (!sr.rows.empty()) {
    bool one_phase = true;
    for (const ScriptTestRow& r : sr.rows)
      if (r.phase != sr.rows.front().phase) one_phase = false;
    // (The note names the script with its time already when it ran.)
    if (one_phase && sr.note.find(sr.rows.front().phase) == std::string::npos)
      title += "  \xC2\xB7  " + sr.rows.front().phase + " script";
  }
  if (!sr.note.empty()) title += "  \xC2\xB7  " + sr.note;
  script_title_->SetLabel(U(title));
  fit_script_columns();
  script_title_->SetForegroundColour(colour);
  const int rh = FromDIP(22);
  const int visible = std::clamp<int>(static_cast<int>(n), 1, 10);
  script_list_->SetMinSize(wxSize(-1, rh * (visible + 1) + FromDIP(4)));
  script_title_->Show(show);
  script_list_->Show(show && n > 0);
  update_summary();
}

// Result | Test | Script | Message. The Script column only when the rows come from both scripts
// (the title names the one script otherwise: "post-response" on every row says nothing).
void TestsView::fit_script_columns() {
  if (!script_list_) return;
  bool one_phase = true;
  for (const ScriptTestRow& r : script_.rows)
    if (r.phase != script_.rows.front().phase) one_phase = false;
  const int r = FromDIP(64);
  const int w = std::max(FromDIP(300), script_list_->GetClientSize().x - r - 2);
  const int ph = one_phase ? 0 : FromDIP(104);  // "post-response"
  const int rest = std::max(FromDIP(200), w - ph);
  const int widths[] = {r, rest * 40 / 100, ph, rest - rest * 40 / 100};
  script_list_->Freeze();  // (each width change would repaint the list at once)
  for (int i = 0; i < 4; ++i)
    if (script_list_->GetColumnWidth(i) != widths[i]) script_list_->SetColumnWidth(i, widths[i]);
  script_list_->Thaw();
}

void TestsView::wrap_script_err() {
  if (!script_err_ || script_err_text_.empty()) return;
  const int w = std::max(FromDIP(200), GetClientSize().x - FromDIP(16));
  if (w == script_err_w_ && script_err_text_ == script_err_shown_) return;
  script_err_w_ = w;
  script_err_shown_ = script_err_text_;
  script_err_->SetLabel(U(script_err_text_));
  script_err_->Wrap(w);
  const int lines = static_cast<int>(script_err_->GetLabel().Freq('\n')) + 1;
  script_err_->SetMinSize(wxSize(w, lines * script_err_->GetCharHeight() + FromDIP(2)));
  Layout();
}

// The inherited group: the folders' / collection's checks, read-only, with their origin. Created
// with the first inherited check, right under the request's own list.
void TestsView::apply_inherited() {
  if (stage_ < 5) return;  // (built by its slice)
  const Theme& th = Theme::get();
  const bool show = !inh_.empty();
  if (!show && !inh_list_) return;
  if (!inh_list_) {
    inh_title_ = new wxStaticText(this, wxID_ANY, "From folders");
    inh_title_->SetFont(inh_title_->GetFont().Bold());
    inh_title_->SetToolTip("Checks set on a folder or the collection (folder.json). They run on every response "
                           "of the requests below it, next to the request's own checks, in the runner and "
                           "watt --run too. Edit them in the folder's settings.");
    inh_link_ = new wxStaticText(this, wxID_ANY, wxString::FromUTF8("Edit folder settings\xE2\x80\xA6"));
    inh_link_->SetForegroundColour(th.accent);
    inh_link_->SetCursor(wxCursor(wxCURSOR_HAND));
    inh_link_->SetToolTip("Open the settings of the folder these checks come from (auth, headers, variables, "
                          "assertions, scripts)");
    inh_link_->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
      if (inh_.empty() || !on_edit_folder) return;
      long i = inh_list_ ? inh_list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED) : -1;
      if (i < 0 || static_cast<size_t>(i) >= inh_.size()) i = static_cast<long>(inh_.size()) - 1;  // nearest folder
      const std::filesystem::path f = inh_[static_cast<size_t>(i)].folder;
      CallAfter([this, f] {
        if (on_edit_folder) on_edit_folder(f);
      });
    });
    inh_list_ = new FastList(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                               wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_NONE);
    inh_list_->AppendColumn("Result", wxLIST_FORMAT_LEFT, FromDIP(64));
    inh_list_->AppendColumn("Check", wxLIST_FORMAT_LEFT, FromDIP(260));
    inh_list_->AppendColumn("Actual", wxLIST_FORMAT_LEFT, FromDIP(200));
    inh_list_->AppendColumn("From", wxLIST_FORMAT_LEFT, FromDIP(120));
    inh_list_->SetBackgroundColour(th.bg);
    inh_list_->SetForegroundColour(th.fg);
    inh_list_->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      if (inh_fit_pending_) return;
      inh_fit_pending_ = true;
      CallAfter([this] {
        inh_fit_pending_ = false;
        const int r = FromDIP(64);
        const int w = std::max(FromDIP(300), inh_list_->GetClientSize().x - r - 2);
        const int widths[] = {r, w * 42 / 100, w * 38 / 100, w - w * 42 / 100 - w * 38 / 100};
        inh_list_->Freeze();
        for (int i = 0; i < 4; ++i)
          if (inh_list_->GetColumnWidth(i) != widths[i]) inh_list_->SetColumnWidth(i, widths[i]);
        inh_list_->Thaw();
      });
    });
    inh_list_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent& e) {
      const long i = e.GetIndex();
      if (i < 0 || static_cast<size_t>(i) >= inh_.size() || !on_edit_folder) return;
      const std::filesystem::path f = inh_[static_cast<size_t>(i)].folder;
      CallAfter([this, f] {
        if (on_edit_folder) on_edit_folder(f);
      });
    });
    inh_list_->Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
      e.Skip();
      int flags = 0;
      const long i = inh_list_->HitTest(e.GetPosition(), flags);
      if (i < 0 || static_cast<size_t>(i) >= inh_.size()) return inh_list_->UnsetToolTip();
      const InheritedCheck& c = inh_[static_cast<size_t>(i)];
      std::string t = c.a.target + " " + op_label(c.a.op) + (c.a.value.empty() ? std::string() : " " + c.a.value) +
                      "\nfrom " + c.from + " (read-only here: double-click to edit the folder's settings)";
      if (static_cast<size_t>(i) < inh_rows_.size()) {
        const TestRow& r = inh_rows_[static_cast<size_t>(i)];
        if (r.state == TestRow::Pass || r.state == TestRow::Fail)
          t += std::string(r.state == TestRow::Pass ? "\npassed" : "\nFAILED") + "\nactual: " +
               (r.actual.empty() ? std::string("(undefined)") : r.actual) + (r.message.empty() ? "" : "\n" + r.message);
      }
      inh_list_->SetToolTip(U(t));
    });
    // Right under the request's own list.
    wxSizer* sz = GetSizer();
    size_t at = sz->GetItemCount();
    for (size_t i = 0; i < sz->GetItemCount(); ++i)
      if (sz->GetItem(i)->GetWindow() == list_) at = i + 1;
    auto* head = new wxBoxSizer(wxHORIZONTAL);
    head->Add(inh_title_, 0, wxALIGN_CENTER_VERTICAL);
    head->AddSpacer(FromDIP(14));
    head->Add(inh_link_, 0, wxALIGN_CENTER_VERTICAL);
    sz->Insert(at, inh_list_, 0, wxEXPAND);
    sz->Insert(at, head, 0, wxLEFT | wxTOP | wxBOTTOM, FromDIP(8));
  }
  inh_list_->Freeze();
  inh_list_->DeleteAllItems();
  for (size_t i = 0; i < inh_.size(); ++i) {
    const InheritedCheck& c = inh_[i];
    const TestRow* r = i < inh_rows_.size() ? &inh_rows_[i] : nullptr;
    const long at = static_cast<long>(i);
    const bool on = c.a.enabled && !c.a.target.empty();
    const char* res = !on                                 ? "off"
                      : running_                          ? "\xE2\x80\xA6"
                      : !r || r->state == TestRow::NotRun ? "\xE2\x80\x94"
                      : r->state == TestRow::Pass         ? "\xE2\x9C\x93 pass"
                                                          : "\xE2\x9C\x97 FAIL";
    inh_list_->InsertItem(at, U(res));
    inh_list_->SetItem(at, 1, U(c.a.target + "  " + op_label(c.a.op) + (c.a.value.empty() ? "" : "  " + c.a.value)));
    std::string act;
    if (r && (r->state == TestRow::Pass || r->state == TestRow::Fail)) {
      act = r->state == TestRow::Fail && !r->message.empty() ? r->message
                                                              : (r->actual.empty() ? std::string("(undefined)") : r->actual);
      // Times read as times, as in the own checks: "0.66669999" -> "0.667 ms".
      if (act == r->actual && (c.a.target.rfind("res.responseTime", 0) == 0 || c.a.target.rfind("res.timing.", 0) == 0)) {
        char* end = nullptr;
        const double v = std::strtod(act.c_str(), &end);
        if (end && *end == 0 && !act.empty()) act = fmt::ms(v);
      }
      if (act.size() > 300) act = act.substr(0, 300) + "\xE2\x80\xA6";
      for (char& ch : act)
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    }
    inh_list_->SetItem(at, 2, U(act));
    inh_list_->SetItem(at, 3, U(c.from));
    const bool fail = on && r && r->state == TestRow::Fail;
    inh_list_->SetItemTextColour(at, !on ? th.dim : fail ? th.err : th.fg);
    if (fail) inh_list_->SetItemFont(at, inh_list_->GetFont().Bold());
  }
  inh_list_->Thaw();
  std::string title = "From folders";
  wxColour colour = th.dim;
  if (inh_pass_ + inh_fail_ > 0) {
    title += "  \xC2\xB7  " + std::to_string(inh_pass_) + " of " + std::to_string(inh_pass_ + inh_fail_) + " passed";
    if (inh_fail_) title += ", " + std::to_string(inh_fail_) + " failed";
    colour = inh_fail_ ? th.err : th.ok;
  } else {
    title += "  \xC2\xB7  " + std::to_string(inh_.size()) + (inh_.size() == 1 ? " check" : " checks") + ", read-only";
  }
  inh_title_->SetLabel(U(title));
  inh_title_->SetForegroundColour(colour);
  const int rh = FromDIP(22);
  const int visible = std::clamp<int>(static_cast<int>(inh_.size()), 1, 8);
  inh_list_->SetMinSize(wxSize(-1, rh * (visible + 1) + FromDIP(4)));
  inh_title_->Show(show);
  inh_link_->Show(show && static_cast<bool>(on_edit_folder));
  inh_list_->Show(show);
  update_summary();
}

void TestsView::set_inherited(std::vector<InheritedCheck> c) {
  if (c == inh_) return;
  inh_ = std::move(c);
  inh_rows_.clear();
  inh_pass_ = inh_fail_ = 0;
  apply_inherited();
}

void TestsView::set_inherited_results(std::vector<TestRow> rows) {
  inh_rows_ = std::move(rows);
  inh_pass_ = inh_fail_ = 0;
  for (size_t i = 0; i < inh_.size() && i < inh_rows_.size(); ++i) {
    if (!inh_[i].a.enabled) continue;
    if (inh_rows_[i].state == TestRow::Pass) ++inh_pass_;
    else if (inh_rows_[i].state == TestRow::Fail) ++inh_fail_;
  }
  apply_inherited();
}

void TestsView::set_script(ScriptResults s) {
  script_pass_ = script_fail_ = 0;
  for (const ScriptTestRow& r : s.rows) (r.state == ScriptTestRow::Pass ? script_pass_ : script_fail_)++;
  const bool was = script_.any_script || script_.skipped_untrusted || !script_.rows.empty();
  script_ = std::move(s);
  const bool now = script_.any_script || script_.skipped_untrusted || !script_.rows.empty();
  if (!was && !now) return;
  apply_script();
}

int TestsView::selected() const {
  return list_ ? static_cast<int>(list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) : -1;
}

// Target suggestions for this kind of request.
void TestsView::fill_targets() {
  if (!target_) return;
  wxArrayString t;
  for (const char* s : grpc_ ? std::initializer_list<const char*>{"res.status", "res.statusMessage", "res.responseTime",
                                                                  "res.timing.ttfb", "res.body", "res.body.",
                                                                  "res.messages.length", "res.messages[0].",
                                                                  "res.trailers.", "res.metadata."}
                             : std::initializer_list<const char*>{"res.status", "res.responseTime", "res.timing.ttfb",
                                                                  "res.timing.connect", "res.headers.content-type",
                                                                  "res.headers.", "res.body", "res.body.",
                                                                  "res.body.length", "res.size", "res.url"})
    t.Add(s);
  const wxString keep = target_->GetValue();
  target_->Set(t);
  target_->ChangeValue(keep);
}

void TestsView::set_asserts(const std::vector<Assertion>& a, bool editable, bool grpc) {
  const bool same = a == asserts_;
  const bool kind_changed = grpc != grpc_;
  editable_ = editable;
  grpc_ = grpc;
  if (!same) {
    asserts_ = a;
    rows_.clear();
    if (editing_ >= 0) cancel_edit();
  }
  if (kind_changed) fill_targets();
  count_results();
  if (!list_) return;
  list_->refresh_rows();
  update_summary();
  update_chips();
}

void TestsView::set_running() {
  running_ = true;
  have_response_ = true;
  no_resp_reason_.clear();
  count_results();
  if (!list_) return;
  update_summary();
  list_->texts_changed();
}

void TestsView::set_no_response(std::string reason) {
  running_ = false;
  have_response_ = false;
  no_resp_reason_ = std::move(reason);
  rows_.clear();
  // Inherited results belong to the response they ran on: never keep them past it.
  const bool had_inh = !inh_rows_.empty() || inh_pass_ + inh_fail_ > 0;
  inh_rows_.clear();
  inh_pass_ = inh_fail_ = 0;
  count_results();
  if (had_inh) apply_inherited();
  if (!list_) return;
  list_->refresh_rows();
  update_summary();
}

const TestRow* TestsView::find_check(const std::string& target, const std::string& op, const std::string& value,
                                     std::string* inherited_from) const {
  auto unq = [](std::string_view v) {
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    return v;
  };
  auto same = [&](const Assertion& a) { return a.target == target && a.op == op && unq(a.value) == unq(value); };
  static const TestRow kNotRun{};
  for (size_t i = 0; i < asserts_.size(); ++i)
    if (same(asserts_[i])) {
      if (inherited_from) inherited_from->clear();
      return i < rows_.size() ? &rows_[i] : &kNotRun;
    }
  for (size_t i = 0; i < inh_.size(); ++i)
    if (same(inh_[i].a)) {
      if (inherited_from) *inherited_from = inh_[i].from;
      return i < inh_rows_.size() ? &inh_rows_[i] : &kNotRun;
    }
  return nullptr;
}

void TestsView::set_results(std::vector<TestRow> rows, double total_ms, double parse_ms) {
  running_ = false;
  have_response_ = true;
  no_resp_reason_.clear();
  rows_ = std::move(rows);
  total_ms_ = total_ms;
  parse_ms_ = parse_ms;
  count_results();
  // The inherited rows were filled while running_ still read "checking": redraw their Result.
  if (!inh_.empty()) apply_inherited();
  if (!list_) return;
  list_->refresh_rows();
  update_summary();
}

void TestsView::set_captures(std::vector<CaptureRow> rows) {
  if (rows.empty() && cap_rows_.empty()) return;
  cap_rows_ = std::move(rows);
  apply_captures();
}

void TestsView::set_hints(long status, double total_ms, const std::string& content_type, bool body_json) {
  hint_status_ = status;
  hint_ms_ = total_ms;
  hint_ctype_ = content_type;
  hint_json_ = body_json;
  if (list_) update_chips();
}

void TestsView::count_results() {
  passed_ = failed_ = 0;
  for (size_t i = 0; i < asserts_.size() && i < rows_.size(); ++i) {
    if (!asserts_[i].enabled) continue;
    if (rows_[i].state == TestRow::Pass) ++passed_;
    else if (rows_[i].state == TestRow::Fail) ++failed_;
  }
}

void TestsView::update_summary() {
  count_results();
  if (!list_) return;
  int off = 0;
  for (const Assertion& a : asserts_) off += !a.enabled;
  const int on = static_cast<int>(asserts_.size()) - off;
  std::string note;
  Tone tone = Tone::Dim;
  const bool scripted = !script_.rows.empty() || script_.skipped_untrusted;
  const bool inherited = !inh_.empty();
  const int all_on = on + static_cast<int>(std::count_if(inh_.begin(), inh_.end(), [](const InheritedCheck& c) {
                       return c.a.enabled && !c.a.target.empty();
                     }));
  if (!running_ && !no_resp_reason_.empty() && all_on > 0) {
    // The send failed before a response: no check ran (watt --run and the runner count it failed).
    note = std::to_string(all_on) + (all_on == 1 ? " check" : " checks") + " not run \xC2\xB7 " + no_resp_reason_;
    tone = Tone::Err;
  } else if (asserts_.empty() && inherited && !scripted) {
    note = running_ ? std::string("checking\xE2\x80\xA6")
           : inh_pass_ + inh_fail_ == 0
               ? std::to_string(inh_.size()) + " inherited " + (inh_.size() == 1 ? "check" : "checks")
               : "No own checks \xC2\xB7 folders " + std::to_string(inh_pass_) + " of " +
                     std::to_string(inh_pass_ + inh_fail_) + " passed";
    tone = running_ ? Tone::Accent : inh_fail_ ? Tone::Err : inh_pass_ ? Tone::Ok : Tone::Dim;
  } else if (asserts_.empty() && scripted) {
    note = script_.skipped_untrusted ? std::string("No assertion rows \xC2\xB7 scripts not run (workspace not trusted)")
                                     : "No assertion rows \xC2\xB7 script " + std::to_string(script_pass_) + " of " +
                                           std::to_string(script_pass_ + script_fail_) + " passed";
    tone = script_.skipped_untrusted ? Tone::Warn : script_fail_ ? Tone::Err : Tone::Ok;
  } else if (asserts_.empty()) {
    note = "No checks yet";
  } else if (running_) {
    note = "checking " + std::to_string(on) + (on == 1 ? " assertion\xE2\x80\xA6" : " assertions\xE2\x80\xA6");
    tone = Tone::Accent;
  } else if (!have_response_ || rows_.empty()) {
    note = std::to_string(on) + (on == 1 ? " check" : " checks") + " \xC2\xB7 run on the next response";
  } else {
    // One total first (the same as the tab's badge and the status line: assertions, folder checks
    // and script tests together), then the split.
    const int all_pass = passed_ + inh_pass_ + script_pass_;
    const int all = passed_ + failed_ + inh_pass_ + inh_fail_ + script_pass_ + script_fail_;
    const bool split = inh_pass_ + inh_fail_ + script_pass_ + script_fail_ > 0;
    note = std::to_string(all_pass) + " of " + std::to_string(all) + " passed";
    if (rechecked_) note = "checked now against the restored run: " + note;
    if (split) note += " \xC2\xB7 assertions " + std::to_string(passed_) + "/" + std::to_string(passed_ + failed_);
    else if (failed_) note += " \xC2\xB7 " + std::to_string(failed_) + " failed";
    if (off) note += " \xC2\xB7 " + std::to_string(off) + " off";
    if (inh_pass_ + inh_fail_ > 0)
      note += " \xC2\xB7 folders " + std::to_string(inh_pass_) + "/" + std::to_string(inh_pass_ + inh_fail_);
    if (script_pass_ + script_fail_ > 0)
      note += " \xC2\xB7 script " + std::to_string(script_pass_) + "/" + std::to_string(script_pass_ + script_fail_);
    note += " \xC2\xB7 " + fmt::ms(total_ms_);
    if (parse_ms_ > 0.05) note += " (JSON parse " + fmt::ms(parse_ms_) + ")";
    note += ", off the UI thread";
    tone = failed_ || script_fail_ || inh_fail_ ? Tone::Err : Tone::Ok;
  }
  if (!editable_) note += " \xC2\xB7 edit them in the request's Asserts tab";
  bar_->set_note(note, tone);
  const bool empty = asserts_.empty();
  // No assertion rows but script results: the long hint would push them down; one short line.
  if (empty && inherited) {
    empty_text_ = editable_ ? "No own checks (+ Add, or a suggestion above). Inherited from folders:"
                            : "No own checks. Inherited from folders:";
  } else if (empty && scripted) {
    empty_text_ = editable_ ? "No assertion rows (+ Add, or a suggestion above). Script results:" : "Script results:";
  } else if (empty) {
    empty_text_ = editable_ ? "Checks run on every response, off the UI thread, and are saved with the request "
                              "(Ctrl+S).\nAdd one with + Add or a suggestion above, e.g.  res.status == 200,  "
                              "res.body.items[0].id exists,  res.responseTime < 500,  res.headers.content-type "
                              "contains json."
                            : "No checks for this request.";
  }
  empty_->Show(empty);
  list_->Show(!empty);
  fit_list();
  if (empty) wrap_empty();
  else Layout();
}

// With a Script group below, the assertion list is only as tall as its rows (the script results
// sit right under it, not at the bottom of the pane); alone it takes the whole height.
void TestsView::fit_list() {
  wxSizerItem* it = list_ ? GetSizer()->GetItem(list_) : nullptr;
  if (!it) return;
  const bool below = (script_title_ && script_title_->IsShown()) || !inh_.empty();
  if (below) {
    const int rows = std::clamp(static_cast<int>(asserts_.size()), 1, 12);
    list_->SetMinSize(wxSize(-1, FromDIP(19) * rows + FromDIP(30)));
    it->SetProportion(0);
  } else {
    list_->SetMinSize(wxSize(-1, -1));
    it->SetProportion(1);
  }
}

// The empty-state text wraps at the pane width (a static text does not wrap by itself).
void TestsView::wrap_empty() {
  const int w = GetClientSize().x - FromDIP(24);
  empty_->SetLabel(U(empty_text_));
  if (w >= FromDIP(80)) empty_->Wrap(w);
  Layout();
}

void TestsView::update_chips() {
  if (!list_) return;
  const int sel = selected();
  // Always the same chips while editable (grayed without a selection): selecting a row never
  // moves the chip under the cursor.
  bar_->set_visible(kAdd, editable_);
  for (int id : {kEdit, kToggle, kDelete}) {
    bar_->set_visible(id, editable_);
    bar_->set_enabled(id, editable_ && sel >= 0);
  }
  bar_->Refresh();
  // Suggestions from the shown response. Which ones apply depends on the response only; one
  // already in the list is grayed (and ticked) in place.
  auto has = [&](const std::string& target) {
    return std::any_of(asserts_.begin(), asserts_.end(), [&](const Assertion& a) { return a.target == target; });
  };
  const std::string ct = hint_ctype_.substr(0, hint_ctype_.find(';'));
  const bool s_ok = hint_status_ > 0, t_ok = hint_ms_ > 0, c_ok = !grpc_ && !ct.empty(), j_ok = hint_json_;
  const bool row = editable_ && have_response_ && (s_ok || t_ok || c_ok || j_ok);
  auto chip = [&](int id, bool applies, bool used, const std::string& text) {
    hints_->set_visible(id, applies);
    hints_->set_enabled(id, !used);
    hints_->set_label(id, (used ? "\xE2\x9C\x93 " : "+ ") + text);
  };
  chip(kHintStatus, s_ok, has("res.status"), grpc_ ? "status OK" : "status " + std::to_string(hint_status_ / 100) + "xx");
  const double lim = hint_ms_ <= 0 ? 1000 : std::max(50.0, std::ceil(hint_ms_ * 3 / 50.0) * 50.0);
  chip(kHintTime, t_ok, has("res.responseTime"), "time < " + std::to_string(static_cast<long>(lim)) + " ms");
  chip(kHintType, c_ok, has("res.headers.content-type"), "Content-Type " + ct);
  chip(kHintJson, j_ok, has("res.body"), "body is JSON");
  hints_->set_note("suggested");
  if (hints_->IsShown() != row) {
    hints_->Show(row);
    Layout();
  }
  hints_->Refresh();
}

void TestsView::on_chip(int id) {
  auto add = [&](Assertion a) {
    asserts_.push_back(std::move(a));
    edited();
  };
  switch (id) {
    case kAdd: begin_edit(-1); break;
    case kEdit: begin_edit(selected()); break;
    case kToggle: toggle_selected(); break;
    case kDelete: delete_selected(); break;
    case kHintStatus:
      if (grpc_) add({true, "res.status", "eq", "OK"});
      else add({true, "res.status", "between", std::to_string(hint_status_ / 100 * 100) + ", " +
                                                  std::to_string(hint_status_ / 100 * 100 + 99)});
      break;
    case kHintTime: {
      const double lim = std::max(50.0, std::ceil(hint_ms_ * 3 / 50.0) * 50.0);
      add({true, "res.responseTime", "lt", std::to_string(static_cast<long>(lim))});
      break;
    }
    case kHintType: add({true, "res.headers.content-type", "contains", hint_ctype_.substr(0, hint_ctype_.find(';'))}); break;
    case kHintJson: add({true, "res.body", "isJson", ""}); break;
    default: break;
  }
}

void TestsView::begin_edit(int row) {
  if (!editable_) return;
  ensure_built();
  build_editor();
  if (row >= static_cast<int>(asserts_.size())) row = -1;
  editing_ = row;
  if (row >= 0) {
    const Assertion& a = asserts_[static_cast<size_t>(row)];
    target_->ChangeValue(U(a.target));
    const int oi = op_index(a.op);
    op_->SetSelection(oi < 0 ? 0 : oi);
    value_->ChangeValue(U(a.value));
  } else {
    // A new check: a path into the body is the common case for JSON, the status otherwise.
    target_->ChangeValue(hint_json_ ? "res.body." : "res.status");
    op_->SetSelection(0);
    value_->ChangeValue("");
  }
  value_->Enable(kOps[op_->GetSelection()].value);
  edit_err_->SetLabel("");
  edit_err_->Hide();
  editor_->Show();
  Layout();
  target_->SetFocus();
  target_->SetInsertionPointEnd();
}

void TestsView::commit_edit() {
  if (editing_ < -1) return;
  Assertion a;
  a.target = target_->GetValue().utf8_string();
  while (!a.target.empty() && a.target.back() == ' ') a.target.pop_back();
  while (!a.target.empty() && a.target.front() == ' ') a.target.erase(a.target.begin());
  const int oi = std::max(0, op_->GetSelection());
  a.op = kOps[oi].id;
  if (kOps[oi].value) a.value = value_->GetValue().utf8_string();
  std::string err = a.target.empty() ? std::string("Enter a target, e.g. res.status") : check_target(a.target);
  if (!err.empty()) {
    edit_err_->SetLabel(U(err));
    edit_err_->Show();
    Layout();
    target_->SetFocus();
    return;
  }
  if (editing_ >= 0 && static_cast<size_t>(editing_) < asserts_.size()) {
    a.enabled = asserts_[static_cast<size_t>(editing_)].enabled;
    asserts_[static_cast<size_t>(editing_)] = a;
  } else {
    asserts_.push_back(a);
  }
  const int sel = editing_ >= 0 ? editing_ : static_cast<int>(asserts_.size()) - 1;
  editing_ = -2;
  editor_->Hide();
  Layout();
  edited();
  list_->SetItemState(sel, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
  list_->SetFocus();
}

void TestsView::cancel_edit() {
  editing_ = -2;
  if (!editor_) return;
  editor_->Hide();
  Layout();
  list_->SetFocus();
}

void TestsView::delete_selected() {
  const int i = selected();
  if (i < 0 || !editable_) return;
  asserts_.erase(asserts_.begin() + i);
  if (static_cast<size_t>(i) < rows_.size()) rows_.erase(rows_.begin() + i);
  edited();
  if (!asserts_.empty()) {
    const long n = std::min<long>(i, static_cast<long>(asserts_.size()) - 1);
    list_->SetItemState(n, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
  }
}

void TestsView::toggle_selected() {
  const int i = selected();
  if (i < 0 || !editable_) return;
  asserts_[static_cast<size_t>(i)].enabled = !asserts_[static_cast<size_t>(i)].enabled;
  edited();
}

void TestsView::move_selected(int by) {
  const int i = selected();
  const int j = i + by;
  if (i < 0 || j < 0 || j >= static_cast<int>(asserts_.size()) || !editable_) return;
  std::swap(asserts_[static_cast<size_t>(i)], asserts_[static_cast<size_t>(j)]);
  if (static_cast<size_t>(std::max(i, j)) < rows_.size()) std::swap(rows_[static_cast<size_t>(i)], rows_[static_cast<size_t>(j)]);
  edited();
  list_->SetItemState(j, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
}

void TestsView::edited() {
  if (!list_) return;
  list_->refresh_rows();
  update_summary();
  update_chips();
  if (on_edit) on_edit(asserts_);
}

void TestsView::row_clicked(int i) {
  if (i < 0 || static_cast<size_t>(i) >= rows_.size() || static_cast<size_t>(i) >= asserts_.size()) return;
  if (rows_[static_cast<size_t>(i)].state != TestRow::Fail || !on_reveal) return;
  on_reveal(asserts_[static_cast<size_t>(i)]);
}

}  // namespace watt::app::rv
