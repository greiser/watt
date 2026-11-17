#pragma once
// Tests tab of the response view: the request's assertions (SavedRequest::asserts, evaluated by
// core/asserts.h off the UI thread) with expected, actual and pass / fail per row, plus a small
// editor to add, change, enable / disable and delete them. Clicking a failing row whose target
// is a path into the body (res.body..., res.messages[i]...) reveals that value in the Body view.
#include <wx/panel.h>

#include <functional>
#include <string>
#include <vector>

#include "core/asserts.h"

class wxComboBox;
class wxChoice;
class wxTextCtrl;
class wxStaticText;

namespace watt::app::rv {

class ChipBar;
class TestList;

struct TestRow {
  enum State { NotRun, Pass, Fail, Off } state = NotRun;
  std::string actual, expected, message;
  double eval_us = 0;
};

class TestsView : public wxPanel {
 public:
  explicit TestsView(wxWindow* parent);

  // The user changed the list (add / edit / delete / toggle): the owner saves it into the
  // request and re-runs the checks.
  std::function<void(const std::vector<Assertion>&)> on_edit;
  // A failing row with a body target was clicked.
  std::function<void(const Assertion&)> on_reveal;

  // The request's assertions (rows reset to "not run" when they changed). editable = false:
  // another panel owns editing (the request editor's Asserts table): the list is read-only here.
  void set_asserts(const std::vector<Assertion>& a, bool editable, bool grpc);
  const std::vector<Assertion>& asserts() const { return asserts_; }
  // A response is being checked / no response to check yet.
  void set_running();
  void set_no_response();
  // One row per assertion of the list (same order). parse_ms: time to parse the body.
  void set_results(std::vector<TestRow> rows, double total_ms, double parse_ms);
  // Quick-add suggestions taken from the shown response (status, time, content type).
  void set_hints(long status, double total_ms, const std::string& content_type, bool body_json);
  int passed() const { return passed_; }
  int failed() const { return failed_; }

 private:
  friend class TestList;
  void on_chip(int id);
  void begin_edit(int row);  // -1: a new assertion
  void commit_edit();
  void cancel_edit();
  void delete_selected();
  void toggle_selected();
  void move_selected(int by);
  void update_summary();
  void update_chips();
  void row_clicked(int row);
  int selected() const;
  void edited();  // list changed by the user

  ChipBar* bar_ = nullptr;
  wxPanel* editor_ = nullptr;
  wxComboBox* target_ = nullptr;
  wxChoice* op_ = nullptr;
  wxTextCtrl* value_ = nullptr;
  wxStaticText* edit_err_ = nullptr;
  TestList* list_ = nullptr;
  wxStaticText* empty_ = nullptr;
  std::vector<Assertion> asserts_;
  std::vector<TestRow> rows_;
  int editing_ = -2;  // -2: no editor open, -1: new, else row
  long tip_row_ = -2;  // row whose tooltip is shown
  bool editable_ = true;
  bool grpc_ = false;
  bool running_ = false;
  bool have_response_ = false;
  double total_ms_ = 0, parse_ms_ = 0;
  int passed_ = 0, failed_ = 0;
  // hints
  long hint_status_ = 0;
  double hint_ms_ = 0;
  std::string hint_ctype_;
  bool hint_json_ = false;
};

}  // namespace watt::app::rv
