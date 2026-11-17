#pragma once
// Tests tab of the response view: the request's assertions (SavedRequest::asserts, evaluated by
// core/asserts.h off the UI thread) with expected, actual and pass / fail per row, plus a small
// editor to add, change, enable / disable and delete them. Clicking a failing row whose target
// is a path into the body (res.body..., res.messages[i]...) reveals that value in the Body view.
#include <wx/panel.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "core/asserts.h"

class wxComboBox;
class wxChoice;
class wxTextCtrl;
class wxStaticText;
class wxListCtrl;

namespace watt::app::rv {

class ChipBar;
class TestList;

struct TestRow {
  enum State { NotRun, Pass, Fail, Off } state = NotRun;
  std::string actual, expected, message;
  double eval_us = 0;
};

// One capture of the request (SavedRequest::captures) and what it did with the shown response.
struct CaptureRow {
  std::string var, target, scope;  // scope: "runtime" / "environment" / "globals"
  bool enabled = true;
  enum State { NotRun, Stored, Failed, Previewed } state = NotRun;
  std::string value, message;  // value stored (or that would be, for an earlier run); why not
};

// What the request's scripts reported for the shown response (core/script.h ScriptRun): their
// test() / pm.test() results and uncaught errors, listed under the assertions as a "Script" group.
struct ScriptTestRow {
  std::string phase;  // "pre-request" / "post-response"
  std::string name;
  enum State { Pass, Fail, Error } state = Pass;
  std::string message;  // why it failed; for Error the script error with line:col
};
struct ScriptResults {
  std::vector<ScriptTestRow> rows;
  bool any_script = false;          // the request has scripts that ran (or would have)
  bool skipped_untrusted = false;   // not run: the workspace's scripts are not trusted
  std::string note;                 // e.g. "post-response 0.84 ms"
};

// A check the request inherits from a folder / the collection (folder.json asserts): run with
// the request's own on every response, listed read-only here with where it comes from.
struct InheritedCheck {
  Assertion a;
  std::string from;               // "api", "Collection"
  std::filesystem::path folder;   // the folder whose settings hold it (the root for Collection)
  bool operator==(const InheritedCheck&) const = default;
};

class TestsView : public wxPanel {
 public:
  // Cheap: the controls are created by ensure_built() (the first time the tab is shown); until
  // then every setter only records its state.
  explicit TestsView(wxWindow* parent);
  void ensure_built();
  bool built() const { return list_ != nullptr; }

  // The user changed the list (add / edit / delete / toggle): the owner saves it into the
  // request and re-runs the checks.
  std::function<void(const std::vector<Assertion>&)> on_edit;
  // A failing row with a body target was clicked.
  std::function<void(const Assertion&)> on_reveal;

  // The request's assertions (rows reset to "not run" when they changed). editable = false:
  // another panel owns editing (the request editor's Asserts table): the list is read-only here.
  void set_asserts(const std::vector<Assertion>& a, bool editable, bool grpc);
  const std::vector<Assertion>& asserts() const { return asserts_; }
  // The checks inherited from folders / the collection (read-only; results reset when changed),
  // and their results (same order).
  void set_inherited(std::vector<InheritedCheck> c);
  const std::vector<InheritedCheck>& inherited() const { return inh_; }
  void set_inherited_results(std::vector<TestRow> rows);
  int inherited_passed() const { return inh_pass_; }
  int inherited_failed() const { return inh_fail_; }
  // "Edit folder settings" (or a double-click on an inherited row): open that folder's editor.
  std::function<void(const std::filesystem::path&)> on_edit_folder;
  // A response is being checked / no response to check yet.
  void set_running();
  // No response to check: nothing sent yet, or the send failed before a response (reason says
  // why: a transport error, the OAuth token step). Every check, own and inherited, is "not run";
  // results of an earlier response are dropped.
  void set_no_response(std::string reason = {});
  const std::string& no_response_reason() const { return no_resp_reason_; }
  // One row per assertion of the list (same order). parse_ms: time to parse the body.
  void set_results(std::vector<TestRow> rows, double total_ms, double parse_ms);
  // Quick-add suggestions taken from the shown response (status, time, content type).
  void set_hints(long status, double total_ms, const std::string& content_type, bool body_json);
  // The request's captures and their outcome (empty: the captures section is hidden).
  void set_captures(std::vector<CaptureRow> rows);
  // The scripts' results for the shown response (empty: the Script group is hidden).
  void set_script(ScriptResults s);
  // The response shown was not sent now (restored from history or the session, an example): its
  // checks are the request's current ones run against it now, not results recorded at the time.
  void set_rechecked(bool on) {
    if (rechecked_ == on) return;
    rechecked_ = on;
    update_summary();
  }
  bool rechecked() const { return rechecked_; }
  int passed() const { return passed_; }
  int failed() const { return failed_; }
  int script_passed() const { return script_pass_; }
  int script_failed() const { return script_fail_; }  // failed tests and script errors
  bool script_skipped() const { return script_.skipped_untrusted; }

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
  void count_results();  // passed_ / failed_ from rows_ (no controls needed)
  void build_editor();
  void fill_targets();
  void apply_captures();
  void apply_script();
  void apply_inherited();
  void fit_list();
  void update_chips();
  void row_clicked(int row);
  int selected() const;
  void edited();  // list changed by the user

  ChipBar* bar_ = nullptr;
  ChipBar* hints_ = nullptr;
  wxStaticText* caps_title_ = nullptr;
  wxListCtrl* caps_ = nullptr;  // quick-add suggestions from the shown response (own row: nothing shifts)
  std::string empty_text_;    // empty_'s text before wrapping
  void wrap_empty();
  wxPanel* editor_ = nullptr;
  wxComboBox* target_ = nullptr;
  wxChoice* op_ = nullptr;
  wxTextCtrl* value_ = nullptr;
  wxStaticText* edit_err_ = nullptr;
  TestList* list_ = nullptr;
  wxStaticText* empty_ = nullptr;
  std::vector<Assertion> asserts_;
  std::vector<TestRow> rows_;
  std::vector<CaptureRow> cap_rows_;
  wxStaticText* script_title_ = nullptr;
  wxListCtrl* script_list_ = nullptr;
  ScriptResults script_;
  wxStaticText* inh_title_ = nullptr;
  wxStaticText* inh_link_ = nullptr;
  wxListCtrl* inh_list_ = nullptr;
  std::vector<InheritedCheck> inh_;
  std::vector<TestRow> inh_rows_;
  int inh_pass_ = 0, inh_fail_ = 0;
  bool inh_fit_pending_ = false;
  int script_pass_ = 0, script_fail_ = 0;
  bool script_fit_pending_ = false;
  bool caps_fit_pending_ = false;
  int editing_ = -2;  // -2: no editor open, -1: new, else row
  long tip_row_ = -2;  // row whose tooltip is shown
  bool editable_ = true;
  bool grpc_ = false;
  bool running_ = false;
  bool have_response_ = false;
  std::string no_resp_reason_;  // set_no_response(reason): why no check ran
  bool rechecked_ = false;
  double total_ms_ = 0, parse_ms_ = 0;
  int passed_ = 0, failed_ = 0;
  // hints
  long hint_status_ = 0;
  double hint_ms_ = 0;
  std::string hint_ctype_;
  bool hint_json_ = false;
};

}  // namespace watt::app::rv
