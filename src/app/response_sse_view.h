#pragma once
// The Body tab of a text/event-stream response: one row per event (offset since the request
// started, event name, id, size, a one-line data preview), live while the stream runs, and the
// selected event's full data below it, pretty-printed when it is JSON (Bruno and Postman list
// SSE events the same way). The list is owner-drawn (only the visible rows are painted), so a
// stream of 100k events stays responsive; the detail pane formats one event at a time.
#include <wx/panel.h>
#include <wx/timer.h>

#include <functional>
#include <string>
#include <vector>

#include "app/response_sse.h"

class wxSplitterWindow;
class wxStyledTextCtrl;
class wxTextCtrl;

namespace watt::app::rv {

class SseList;
class ChipBar;
enum class Tone;

class SsePage : public wxPanel {
 public:
  explicit SsePage(wxWindow* parent);
  // Children are created on first use (the page costs nothing until an event stream is shown).
  void ensure();
  void reset();
  // Appends events (rows follow the tail while the user looks at it; the newest one is shown
  // below until the user picks a row).
  void append(std::vector<SseEvent>&& evs);
  size_t count() const { return events_.size() + dropped_; }
  const std::vector<SseEvent>& events() const { return events_; }
  // The note in the bar above the list ("live · 12 events · 3 keep-alive comments").
  void set_caption(const std::string& text, Tone tone);
  // The Raw chip: the stream's bytes as received, in the body view.
  std::function<void()> on_raw;

  static constexpr size_t kMaxRows = 200000;
  // Does the event match a filter text (lower case)? Its name, id or data contains it,
  // ASCII case-insensitive; "event:<name>" matches the event name exactly.
  static bool matches(const SseEvent& ev, const std::string& lower_filter);

 private:
  friend class SseList;
  // Rows of the list: every event, or (while the filter field has text) the matching ones.
  long rows() const { return filter_.empty() ? static_cast<long>(events_.size()) : static_cast<long>(view_.size()); }
  size_t event_of(long row) const { return filter_.empty() ? static_cast<size_t>(row) : view_[static_cast<size_t>(row)]; }
  void apply_filter();
  void show_detail(long row);
  void copy_data();
  std::string filter_;          // lower case; empty: no filter
  std::vector<size_t> view_;    // matching events (indices into events_) while filtered
  wxTextCtrl* find_ = nullptr;
  wxTimer find_timer_;
  std::vector<SseEvent> events_;
  std::vector<std::string> previews_;
  size_t dropped_ = 0;  // events past kMaxRows (counted, not listed)
  long detail_row_ = -2;
  size_t detail_size_ = 0;  // data size of the event shown when it was formatted (the live tail)
  bool user_picked_ = false;
  ChipBar* bar_ = nullptr;
  wxSplitterWindow* split_ = nullptr;
  SseList* list_ = nullptr;
  wxPanel* detail_ = nullptr;
  ChipBar* detail_head_ = nullptr;
  wxStyledTextCtrl* stc_ = nullptr;
  std::string caption_text_;
  Tone caption_tone_{};
};

}  // namespace watt::app::rv
