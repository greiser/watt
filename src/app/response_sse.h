#pragma once
// Server-Sent Events (text/event-stream) for the response view: an incremental parser (WHATWG
// HTML "event stream interpretation": id / event / data / retry fields, comments, a blank line
// dispatches, CR / LF / CRLF line ends, a leading BOM) and the HttpBodyTap the shell's worker
// feeds while the body arrives. wx-free (tests compile it in directly).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "app/panels.h"

namespace watt::app::rv {

struct SseEvent {
  double at_ms = -1;     // when its dispatching blank line arrived, ms after the send (-1: unknown)
  std::string type;      // "event:" field ("" = the default "message")
  std::string id;        // "id:" field of this event block ("" = none)
  std::string data;      // "data:" lines joined with '\n'
  long retry = -1;       // "retry:" reconnection time in ms (-1 = none)
  size_t bytes = 0;      // bytes of the event block in the stream (its lines and the blank line)
  bool truncated = false;  // a line or the data went past SseParser::kMaxField: the rest was dropped
};

class SseParser {
 public:
  // A line, or an event's joined data, never grows past this (a stream that never sends a line
  // end or a blank line must not take all memory): the excess is dropped and the event marked.
  static constexpr size_t kMaxField = 16u << 20;
  // Parses the next bytes of the stream; dispatched events are appended to out with at_ms.
  void feed(const char* p, size_t n, double at_ms, std::vector<SseEvent>& out);
  uint64_t comments() const { return comments_; }
  // Bytes of an event block still open (no blank line yet).
  size_t pending_bytes() const { return cur_bytes_ + line_.size(); }

 private:
  void line(std::string_view l, double at_ms, std::vector<SseEvent>& out);
  std::string line_;
  bool cr_ = false;         // the last byte was a CR (a following LF belongs to it)
  bool started_ = false;    // past a possible leading BOM
  bool has_data_ = false;
  SseEvent cur_;
  size_t cur_bytes_ = 0;
  uint64_t comments_ = 0;
  size_t pending_bytes_ = 0;  // bytes of the lines since the last event
};

// The whole stream at once (a restored response). Events get at_ms -1.
std::vector<SseEvent> parse_sse(std::string_view body);

// Does a body that starts with these bytes look like an event stream? 1 yes, 0 no, -1 not
// enough bytes to tell. Its first line (after a BOM and blank lines) must be a comment or one of
// the four fields.
int sniff_sse(std::string_view head);

// Content-Type is text/event-stream (parameters and case ignored).
bool is_event_stream(std::string_view content_type);

// One-line preview of an event's data: whitespace runs collapsed, at most max bytes (+ an ellipsis).
std::string one_line_preview(std::string_view src, size_t max = 160);

using SteadyClock = std::chrono::steady_clock;

// The tap of one HTTP send. feed() runs on the worker; it parses outside any lock, then hands
// the new events and bytes over under a short lock. notify (supplied by the view, called on the
// worker) posts one drain to the UI thread; it is called again only after drain() ran.
class SseTap : public HttpBodyTap {
 public:
  SseTap(SteadyClock::time_point t0, std::function<void()> notify, std::function<void()> on_head = {})
      : t0_(t0), notify_(std::move(notify)), on_head_(std::move(on_head)) {}
  void feed(const char* data, size_t len) override;
  // Worker: the response head (HeadFn). Kept, then on_head (supplied by the view) posts it.
  void head(std::shared_ptr<const HttpResponse> h) override;
  // Any thread: the latest head, null before it arrived; when it arrived (ms after t0).
  std::shared_ptr<const HttpResponse> head_response() const;
  double head_ms() const { return head_ms_.load(std::memory_order_acquire); }
  // Worker: what goes out (HttpBodyTap::sent); any thread: read back, null before the send.
  void sent(std::shared_ptr<const SentHttp> s) override;
  std::shared_ptr<const SentHttp> sent_request() const;

  struct Batch {
    std::vector<SseEvent> events;
    std::string raw;  // the stream's bytes since the last drain
    uint64_t comments = 0;
  };
  // UI thread: everything since the last drain; the next feed() notifies again.
  Batch drain();
  bool is_sse() const { return state_.load(std::memory_order_acquire) == 1; }
  bool is_off() const { return state_.load(std::memory_order_acquire) == 0; }
  // Any thread: body bytes fed so far, and when the first of them arrived (ms after t0, -1: none).
  uint64_t bytes() const { return bytes_.load(std::memory_order_acquire); }
  double first_ms() const {
    const int64_t us = first_us_.load(std::memory_order_acquire);
    return us < 0 ? -1.0 : static_cast<double>(us) / 1000.0;
  }
  // UI thread: the start of a body that is not an event stream (at most kPartialMax bytes), for
  // a request cancelled while it downloads: what arrived is shown, not thrown away.
  std::string partial() const;
  static constexpr size_t kPartialMax = 2u << 20;
  // Between two drains (the UI drains once per frame; these only matter if it stalls): the
  // newest events are kept (older ones counted in dropped()), the raw bytes are cut off.
  static constexpr size_t kMaxPendingEvents = 100000;
  static constexpr size_t kMaxPendingRaw = 64u << 20;
  uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

 private:
  void parse(const char* p, size_t n);
  void keep_partial(const char* p, size_t n);
  SteadyClock::time_point t0_;
  std::function<void()> notify_;
  std::function<void()> on_head_;
  std::shared_ptr<const HttpResponse> head_;  // under m_
  std::shared_ptr<const SentHttp> sent_;      // under m_
  std::atomic<double> head_ms_{-1};
  std::atomic<int> state_{-1};  // -1 undecided, 0 not an event stream (inert), 1 event stream
  std::atomic<uint64_t> bytes_{0};
  std::atomic<int64_t> first_us_{-1};
  std::atomic<bool> partial_full_{false};
  std::atomic<uint64_t> dropped_{0};
  // Worker only.
  std::string sniff_;
  SseParser parser_;
  std::vector<SseEvent> scratch_;
  // Handed over under m_.
  mutable std::mutex m_;
  std::string partial_;  // not an event stream: its first bytes
  std::vector<SseEvent> pending_;
  std::string raw_;
  uint64_t comments_ = 0;
  size_t pending_bytes_ = 0;  // bytes of the lines since the last event
  bool posted_ = false;
};

}  // namespace watt::app::rv
