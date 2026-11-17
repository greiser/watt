#pragma once
// Sidebar history: a virtual list (only visible rows are drawn, 10k entries cost nothing),
// newest first, grouped by day ("Today", "Yesterday", "Mon, Nov 14"). Two lines per entry:
// method / gRPC badge, the saved request's name (else the target) and the status; under it the
// target that was sent (recorded at send time, never re-resolved with the environment active
// now: history_summary.h), the elapsed time and the time of day. Two entries of one request
// that sent different URLs never look the same.
// Repeats (consecutive entries of one day with the same sent target and status, e.g. 60 health
// checks) fold into one row with a "x60" chip and the min / avg / max time; a click on the chip,
// Right / Left or the menu expands / folds it.
#include <wx/vlbox.h>

#include <functional>
#include <string>
#include <vector>

#include "core/model.h"

namespace watt::app::sidebar {

// "14:03" for an ISO UTC time of today (local time), else "Nov 14 14:03".
std::string history_when_label(const std::string& iso);

class HistoryList : public wxVListBox {
 public:
  explicit HistoryList(wxWindow* parent);

  void set_entries(std::vector<HistoryEntry> newest_first);
  void prepend(const HistoryEntry& e);
  // Drops these entries from the list (matched by when + summary + status + total_ms).
  void remove(const std::vector<HistoryEntry>& entries);
  void clear_all();
  const std::vector<HistoryEntry>& entries() const { return all_; }
  // Lower-case words; every word must appear in the summary or the shown name. Empty: show all.
  void set_filter(const std::string& query_lower);
  // Highlights the row of the entry (its run row when folded); none when it is not shown.
  void select_entry(const std::string& when_iso, const std::string& summary);
  // The names / resolved targets changed (environment switched, requests renamed).
  void relabel();
  size_t total() const { return all_.size(); }
  size_t shown() const { return shown_count_; }
  // Height of the first max_rows rows (day headers included): sizes the pane to its content.
  int content_height(size_t max_rows) const;

  // Opens an entry: the request exactly as it was sent. permanent = false: a single click (the
  // host reuses the preview tab); true: a double click or Enter. open_saved: the context menu's
  // "Open saved request" (the saved request the entry came from) instead.
  std::function<void(const HistoryEntry&, bool permanent, bool open_saved)> on_open;
  // Whether an entry maps to a saved request (enables "Open saved request").
  std::function<bool(const HistoryEntry&)> has_saved;
  // Fills the label of an entry: *name = the saved request it was sent from ("" if none),
  // *target = what to show without a name (e.g. the URL with {{vars}} resolved).
  std::function<void(const HistoryEntry&, std::string* name, std::string* target)> describe;
  // gRPC call kind of an entry (GrpcCallKind as int, < 0 unknown), for its badge arrow.
  std::function<int(const HistoryEntry&)> grpc_kind;
  // Context menu / Del: remove entries (a folded run removes all of its entries), clear the
  // whole history, save an entry as a new saved request, copy it as cURL / grpcurl.
  std::function<void(const std::vector<HistoryEntry>&)> on_delete;
  std::function<void()> on_clear;
  std::function<void(const HistoryEntry&)> on_save_as;
  std::function<void(const HistoryEntry&)> on_copy_curl;

 protected:
  wxCoord OnMeasureItem(size_t n) const override;
  void OnDrawBackground(wxDC& dc, const wxRect& rect, size_t n) const override;
  void OnDrawItem(wxDC& dc, const wxRect& rect, size_t n) const override;

 private:
  struct Label {
    bool ready = false;
    std::string name, target;
  };
  struct Row {
    int entry = -1;      // index into all_ (a run: its newest entry), or -1 for a day header
    std::string header;  // day header text
    std::vector<int> run;  // a run row: every entry of the run (newest first, size > 1)
    bool child = false;    // an entry of an unfolded run
  };
  void refilter();
  // The entries a row stands for (a run row: all of the run).
  std::vector<HistoryEntry> row_entries(int row) const;
  void toggle_run(int row);
  std::string run_key(int row) const;
  bool is_run(int row) const;
  bool unfolded(int row) const;  // a run row the user unfolded
  bool on_chip(int row, int x) const;
  const HistoryEntry* entry(int row) const;
  const Label& label(size_t i) const;
  void open_row(int row, bool permanent);
  void show_menu(int row);
  int right_row_ = -1;           // row under the right button press
  wxLongLong menu_shown_at_ = 0;  // when a right button release opened the menu
  wxLongLong user_scrolled_at_ = 0;  // last wheel / scrollbar use (prepend() keeps the position then)

  std::vector<HistoryEntry> all_;  // newest first
  mutable std::vector<Label> labels_;
  std::vector<Row> rows_;
  size_t shown_count_ = 0;
  std::string filter_;
  int hover_ = -1;
  int click_opened_ = -1;  // row the first click of a possible double click opened
  std::vector<std::string> unfolded_;  // run keys (oldest entry) the user unfolded
  mutable std::vector<std::pair<int, int>> chip_x_;  // per row: [left, right) of its drawn run chip
  static constexpr size_t kMax = 500;
};

}  // namespace watt::app::sidebar
