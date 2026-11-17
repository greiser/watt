#include "app/response_tests_view.h"

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/listctrl.h>
#include <wx/menu.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "app/response_format.h"
#include "app/response_tests.h"
#include "app/response_widgets.h"

namespace watt::app::rv {
namespace {

wxString U(std::string_view s) { return wxString::FromUTF8(s.data(), s.size()); }

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

class TestList : public wxListCtrl {
 public:
  explicit TestList(TestsView* owner)
      : wxListCtrl(owner, wxID_ANY, wxDefaultPosition, wxDefaultSize,
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
      if (fit_pending_) return;
      fit_pending_ = true;
      CallAfter([this] {
        fit_pending_ = false;
        // Result fixed; the rest shares the width: target 30%, check 20%, actual 26%, note 24%.
        const int r = FromDIP(64);
        const int w = std::max(FromDIP(300), GetClientSize().x - r - 2);
        const int widths[] = {r, w * 30 / 100, w * 20 / 100, w * 26 / 100, w - w * 30 / 100 - w * 20 / 100 - w * 26 / 100};
        for (int c = 0; c < 5; ++c)
          if (GetColumnWidth(c) != widths[c]) SetColumnWidth(c, widths[c]);
      });
    });
  }
  void refresh_rows() {
    SetItemCount(static_cast<long>(v_->asserts_.size()));
    Refresh();
  }

 private:
  const TestRow* row(long i) const {
    return static_cast<size_t>(i) < v_->rows_.size() ? &v_->rows_[static_cast<size_t>(i)] : nullptr;
  }
  wxString OnGetItemText(long item, long col) const override {
    const Assertion& a = v_->asserts_[static_cast<size_t>(item)];
    const TestRow* r = row(item);
    switch (col) {
      case 0: {
        if (!a.enabled) return "off";
        if (!r) return v_->running_ ? wxString::FromUTF8("\xE2\x80\xA6") : wxString("\xE2\x80\x94");
        switch (r->state) {
          case TestRow::Pass: return wxString::FromUTF8("\xE2\x9C\x93 pass");
          case TestRow::Fail: return wxString::FromUTF8("\xE2\x9C\x97 FAIL");
          case TestRow::Off: return "off";
          default: return v_->running_ ? wxString::FromUTF8("\xE2\x80\xA6") : wxString::FromUTF8("\xE2\x80\x94");
        }
      }
      case 1: return U(a.target);
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
};

// ------------------------------------------------------------------ view

TestsView::TestsView(wxWindow* parent) : wxPanel(parent, wxID_ANY) {
  const Theme& th = Theme::get();
  SetBackgroundColour(th.bg);
  bar_ = new ChipBar(this, [this](int id) { on_chip(id); });
  bar_->add(kAdd, "+ Add", "Add a check (Insert). Checks are saved with the request and run on every response.", -1,
            true);
  bar_->add(kEdit, "Edit", "Change the selected check (F2 or double-click)", -1, true);
  bar_->add(kToggle, "On/Off", "Enable or disable the selected check (Space)", -1, true);
  bar_->add(kDelete, "Delete", "Delete the selected check (Del)", -1, true);
  bar_->add(kHintStatus, "+ status 2xx", "", -1, true);
  bar_->add(kHintTime, "+ time", "", -1, true);
  bar_->add(kHintType, "+ Content-Type", "", -1, true);
  bar_->add(kHintJson, "+ body is JSON", "res.body isJson", -1, true);

  editor_ = new wxPanel(this, wxID_ANY);
  editor_->SetBackgroundColour(th.bg);
  target_ = new wxComboBox(editor_, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(280, -1)), 0, nullptr,
                           wxTE_PROCESS_ENTER);
  target_->SetHint("res.body.items[0].id");
  wxArrayString ops;
  for (const OpDef& o : kOps) ops.Add(U(std::string(o.shown) + "   (" + o.id + ")"));
  op_ = new wxChoice(editor_, wxID_ANY, wxDefaultPosition, wxDefaultSize, ops);
  op_->SetSelection(0);
  value_ = new wxTextCtrl(editor_, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(200, -1)), wxTE_PROCESS_ENTER);
  value_->SetHint("200, \"text\", 10, 20 ... ({{var}} ok)");
  auto* save = new wxButton(editor_, wxID_ANY, "Save", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
  auto* cancel = new wxButton(editor_, wxID_ANY, "Cancel", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
  edit_err_ = new wxStaticText(editor_, wxID_ANY, "");
  edit_err_->SetForegroundColour(th.err);
  auto* es = new wxBoxSizer(wxHORIZONTAL);
  es->Add(target_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
  es->Add(op_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
  es->Add(value_, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
  es->Add(save, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
  es->Add(cancel, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(6));
  auto* ev = new wxBoxSizer(wxVERTICAL);
  ev->Add(es, 0, wxEXPAND | wxTOP, FromDIP(4));
  ev->Add(edit_err_, 0, wxLEFT | wxTOP | wxBOTTOM, FromDIP(8));
  editor_->SetSizer(ev);
  editor_->Hide();

  list_ = new TestList(this);
  empty_ = new wxStaticText(this, wxID_ANY, "");
  empty_->SetForegroundColour(th.dim);

  auto* sz = new wxBoxSizer(wxVERTICAL);
  sz->Add(bar_, 0, wxEXPAND);
  sz->Add(editor_, 0, wxEXPAND);
  sz->Add(empty_, 0, wxALL, FromDIP(12));
  sz->Add(list_, 1, wxEXPAND);
  SetSizer(sz);

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
  update_summary();
  update_chips();
}

int TestsView::selected() const { return static_cast<int>(list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)); }

void TestsView::set_asserts(const std::vector<Assertion>& a, bool editable, bool grpc) {
  const bool same = a == asserts_;
  editable_ = editable;
  grpc_ = grpc;
  if (!same) {
    asserts_ = a;
    rows_.clear();
    if (editing_ >= 0) cancel_edit();
  }
  // Target suggestions for this kind of request.
  wxArrayString t;
  for (const char* s : grpc ? std::initializer_list<const char*>{"res.status", "res.statusMessage", "res.responseTime",
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
  list_->refresh_rows();
  update_summary();
  update_chips();
}

void TestsView::set_running() {
  running_ = true;
  have_response_ = true;
  update_summary();
  list_->Refresh();
}

void TestsView::set_no_response() {
  running_ = false;
  have_response_ = false;
  rows_.clear();
  list_->refresh_rows();
  update_summary();
}

void TestsView::set_results(std::vector<TestRow> rows, double total_ms, double parse_ms) {
  running_ = false;
  have_response_ = true;
  rows_ = std::move(rows);
  total_ms_ = total_ms;
  parse_ms_ = parse_ms;
  list_->refresh_rows();
  update_summary();
}

void TestsView::set_hints(long status, double total_ms, const std::string& content_type, bool body_json) {
  hint_status_ = status;
  hint_ms_ = total_ms;
  hint_ctype_ = content_type;
  hint_json_ = body_json;
  update_chips();
}

void TestsView::update_summary() {
  passed_ = failed_ = 0;
  int off = 0;
  for (size_t i = 0; i < asserts_.size(); ++i) {
    if (!asserts_[i].enabled) {
      ++off;
      continue;
    }
    if (i < rows_.size()) {
      if (rows_[i].state == TestRow::Pass) ++passed_;
      else if (rows_[i].state == TestRow::Fail) ++failed_;
    }
  }
  const int on = static_cast<int>(asserts_.size()) - off;
  std::string note;
  Tone tone = Tone::Dim;
  if (asserts_.empty()) {
    note = "No checks yet";
  } else if (running_) {
    note = "checking " + std::to_string(on) + (on == 1 ? " assertion\xE2\x80\xA6" : " assertions\xE2\x80\xA6");
    tone = Tone::Accent;
  } else if (!have_response_ || rows_.empty()) {
    note = std::to_string(on) + (on == 1 ? " check" : " checks") + " \xC2\xB7 run on the next response";
  } else {
    note = std::to_string(passed_) + " of " + std::to_string(on) + " passed";
    if (failed_) note += " \xC2\xB7 " + std::to_string(failed_) + " failed";
    if (off) note += " \xC2\xB7 " + std::to_string(off) + " off";
    note += " \xC2\xB7 " + fmt::ms(total_ms_);
    if (parse_ms_ > 0.05) note += " (JSON parse " + fmt::ms(parse_ms_) + ")";
    note += ", off the UI thread";
    tone = failed_ ? Tone::Err : Tone::Ok;
  }
  if (!editable_) note += " \xC2\xB7 edit them in the request's Asserts tab";
  bar_->set_note(note, tone);
  const bool empty = asserts_.empty();
  if (empty) {
    empty_->SetLabel(editable_
                         ? wxString::FromUTF8(
                               "Checks run on every response, off the UI thread, and are saved with the request "
                               "(Ctrl+S).\nAdd one with + Add or a suggestion above, e.g.  res.status == 200,  "
                               "res.body.items[0].id exists,  res.responseTime < 500,  res.headers.content-type "
                               "contains json.")
                         : wxString("No checks for this request."));
  }
  empty_->Show(empty);
  list_->Show(!empty);
  Layout();
}

void TestsView::update_chips() {
  const int sel = selected();
  bar_->set_visible(kAdd, editable_);
  bar_->set_visible(kEdit, editable_ && sel >= 0);
  bar_->set_visible(kToggle, editable_ && sel >= 0);
  bar_->set_visible(kDelete, editable_ && sel >= 0);
  // Suggestions from the shown response, only those not already there.
  auto has = [&](const std::string& target) {
    return std::any_of(asserts_.begin(), asserts_.end(), [&](const Assertion& a) { return a.target == target; });
  };
  // Suggestions while the list is short (they would crowd out the summary otherwise).
  const bool hints = editable_ && have_response_ && sel < 0 && asserts_.size() < 3;
  bar_->set_visible(kHintStatus, hints && hint_status_ > 0 && !has("res.status"));
  bar_->set_label(kHintStatus, grpc_ ? "+ status OK" : "+ status " + std::to_string(hint_status_ / 100) + "xx");
  const double lim = hint_ms_ <= 0 ? 1000 : std::max(50.0, std::ceil(hint_ms_ * 3 / 50.0) * 50.0);
  bar_->set_visible(kHintTime, hints && hint_ms_ > 0 && !has("res.responseTime"));
  bar_->set_label(kHintTime, "+ time < " + std::to_string(static_cast<long>(lim)) + " ms");
  const std::string ct = hint_ctype_.substr(0, hint_ctype_.find(';'));
  bar_->set_visible(kHintType, hints && !grpc_ && !ct.empty() && !has("res.headers.content-type"));
  bar_->set_label(kHintType, "+ Content-Type " + ct);
  bar_->set_visible(kHintJson, hints && hint_json_ && !has("res.body"));
  bar_->Refresh();
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
