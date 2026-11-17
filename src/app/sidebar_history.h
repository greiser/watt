#pragma once
// Sidebar history: a virtual list (only visible rows are drawn, 10k entries cost nothing),
// newest first, grouped by day ("Today", "Yesterday", "Mon, Nov 14"). Two lines per entry:
// method / gRPC badge, the saved request's name (else the target) and the status; under it the
// target that was sent, {{variables}} resolved, the elapsed time and the time of day. Two
// entries of one request that sent different URLs never look the same.
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
  // Lower-case words; every word must appear in the summary or the shown name. Empty: show all.
  void set_filter(const std::string& query_lower);
  // The names / resolved targets changed (environment switched, requests renamed).
  void relabel();
  size_t total() const { return all_.size(); }
  size_t shown() const { return shown_count_; }

  // Opens an entry: the request exactly as it was sent. permanent = false: a single click (the
  // host reuses the preview tab); true: a double click or Enter. open_saved: the context menu's
  // "Open saved request" (the saved request the entry came from) instead.
  std::function<void(const HistoryEntry&, bool permanent, bool open_saved)> on_open;
  // Whether an entry maps to a saved request (enables "Open saved request").
  std::function<bool(const HistoryEntry&)> has_saved;
  // Fills the label of an entry: *name = the saved request it was sent from ("" if none),
  // *target = what to show without a name (e.g. the URL with {{vars}} resolved).
  std::function<void(const HistoryEntry&, std::string* name, std::string* target)> describe;

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
    int entry = -1;      // index into all_, or -1 for a day header
    std::string header;  // day header text
  };
  void refilter();
  const HistoryEntry* entry(int row) const;
  const Label& label(size_t i) const;
  void open_row(int row, bool permanent);

  std::vector<HistoryEntry> all_;  // newest first
  mutable std::vector<Label> labels_;
  std::vector<Row> rows_;
  size_t shown_count_ = 0;
  std::string filter_;
  int hover_ = -1;
  int click_opened_ = -1;  // row the first click of a possible double click opened
  static constexpr size_t kMax = 500;
};

}  // namespace watt::app::sidebar
