#include "app/response_sse.h"

#include <cctype>

namespace watt::app::rv {

void SseParser::feed(const char* p, size_t n, double at_ms, std::vector<SseEvent>& out) {
  size_t i = 0;
  if (!started_) {
    // A leading UTF-8 BOM is not part of the first line (it may arrive split across chunks).
    static const char kBom[] = "\xEF\xBB\xBF";
    while (i < n && line_.size() < 3 && static_cast<unsigned char>(p[i]) == static_cast<unsigned char>(kBom[line_.size()]))
      line_ += p[i++];
    if (line_.size() == 3) {
      line_.clear();
      cur_bytes_ += 3;
      started_ = true;
    } else if (i < n || !line_.empty()) {
      if (i < n) started_ = true;  // not a BOM: those bytes start the first line
    }
    if (!started_) return;
  }
  while (i < n) {
    const char c = p[i];
    if (cr_) {
      cr_ = false;
      if (c == '\n') {  // the LF of a CRLF split across chunks or not: already ended the line
        ++cur_bytes_;
        ++i;
        continue;
      }
    }
    // Copy the run of bytes up to the next line end in one go.
    size_t e = i;
    while (e < n && p[e] != '\n' && p[e] != '\r') ++e;
    line_.append(p + i, e - i);
    if (e == n) break;
    cr_ = p[e] == '\r';
    cur_bytes_ += line_.size() + 1;
    line(line_, at_ms, out);
    line_.clear();
    i = e + 1;
  }
}

void SseParser::line(std::string_view l, double at_ms, std::vector<SseEvent>& out) {
  if (l.empty()) {  // dispatch
    if (has_data_) {
      if (!cur_.data.empty() && cur_.data.back() == '\n') cur_.data.pop_back();
      cur_.at_ms = at_ms;
      cur_.bytes = cur_bytes_;
      out.push_back(std::move(cur_));
    }
    cur_ = SseEvent{};
    has_data_ = false;
    cur_bytes_ = 0;
    return;
  }
  if (l[0] == ':') {
    ++comments_;
    return;
  }
  std::string_view name = l, value;
  if (const size_t colon = l.find(':'); colon != std::string_view::npos) {
    name = l.substr(0, colon);
    value = l.substr(colon + 1);
    if (!value.empty() && value[0] == ' ') value.remove_prefix(1);
  }
  if (name == "data") {
    cur_.data.append(value);
    cur_.data += '\n';
    has_data_ = true;
  } else if (name == "event") {
    cur_.type.assign(value);
  } else if (name == "id") {
    if (value.find('\0') == std::string_view::npos) cur_.id.assign(value);
  } else if (name == "retry") {
    bool digits = !value.empty() && value.size() < 10;
    long v = 0;
    for (char c : value) {
      if (c < '0' || c > '9') {
        digits = false;
        break;
      }
      v = v * 10 + (c - '0');
    }
    if (digits) cur_.retry = v;
  }
  // Any other field is ignored (as browsers do).
}

std::vector<SseEvent> parse_sse(std::string_view body) {
  std::vector<SseEvent> out;
  SseParser p;
  p.feed(body.data(), body.size(), -1, out);
  return out;
}

int sniff_sse(std::string_view h) {
  if (h.size() >= 3 && h.substr(0, 3) == "\xEF\xBB\xBF") h.remove_prefix(3);
  else if (!h.empty() && std::string_view("\xEF\xBB\xBF").substr(0, h.size()) == h) return -1;
  size_t i = 0;
  while (i < h.size() && (h[i] == '\n' || h[i] == '\r')) ++i;
  h.remove_prefix(i);
  if (h.empty()) return -1;
  if (h[0] == ':') return 1;
  size_t e = 0;
  while (e < h.size() && h[e] != ':' && h[e] != '\n' && h[e] != '\r') ++e;
  const std::string_view name = h.substr(0, e);
  static const std::string_view kFields[] = {"data", "event", "id", "retry"};
  if (e < h.size()) {
    for (std::string_view f : kFields)
      if (name == f) return 1;
    return 0;
  }
  for (std::string_view f : kFields)  // a field name still arriving
    if (f.substr(0, std::min(f.size(), name.size())) == name && name.size() <= f.size()) return -1;
  return 0;
}

bool is_event_stream(std::string_view ct) {
  while (!ct.empty() && (ct.front() == ' ' || ct.front() == '\t')) ct.remove_prefix(1);
  static constexpr std::string_view kT = "text/event-stream";
  if (ct.size() < kT.size()) return false;
  for (size_t i = 0; i < kT.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(ct[i])) != kT[i]) return false;
  return ct.size() == kT.size() || ct[kT.size()] == ';' || ct[kT.size()] == ' ' || ct[kT.size()] == '\t';
}

std::string one_line_preview(std::string_view src, size_t max) {
  std::string r;
  r.reserve(std::min(src.size(), max) + 3);
  bool ws = false;
  for (char c : src) {
    if (r.size() >= max) {
      // Never end inside a UTF-8 sequence.
      while (!r.empty() && (static_cast<unsigned char>(r.back()) & 0xC0) == 0x80) r.pop_back();
      if (!r.empty() && (static_cast<unsigned char>(r.back()) & 0x80)) r.pop_back();
      r += "\xE2\x80\xA6";
      return r;
    }
    if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
      ws = true;
      continue;
    }
    if (ws && !r.empty()) r += ' ';
    ws = false;
    r += c;
  }
  return r;
}

void SseTap::feed(const char* data, size_t len) {
  if (len == 0) return;
  if (first_us_.load(std::memory_order_relaxed) < 0)
    first_us_.store(std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now() - t0_).count(),
                    std::memory_order_release);
  bytes_.fetch_add(len, std::memory_order_acq_rel);
  const int st = state_.load(std::memory_order_relaxed);
  if (st == 0) {
    keep_partial(data, len);
    return;
  }
  if (st < 0) {
    // Undecided: the first bytes tell (no headers reach the tap). Anything but an event stream
    // turns the tap off for good: a JSON download costs this one check per chunk.
    // sniff_ holds every byte of the earlier (undecided, so tiny) chunks.
    const size_t held = sniff_.size();
    sniff_.append(data, std::min<size_t>(len, 256 - std::min<size_t>(256, held)));
    const int s = sniff_sse(sniff_);
    if (s == 0 || (s < 0 && sniff_.size() >= 256)) {
      state_.store(0, std::memory_order_release);
      std::string earlier = sniff_.substr(0, held);
      std::string().swap(sniff_);
      keep_partial(earlier.data(), earlier.size());
      keep_partial(data, len);
      return;
    }
    if (s < 0) return;  // wait for more (the bytes are held in sniff_)
    state_.store(1, std::memory_order_release);
    std::string earlier = sniff_.substr(0, held);
    std::string().swap(sniff_);
    if (!earlier.empty()) parse(earlier.data(), earlier.size());
  }
  parse(data, len);
}

void SseTap::parse(const char* p, size_t n) {
  const double at = std::chrono::duration<double, std::milli>(SteadyClock::now() - t0_).count();
  scratch_.clear();
  parser_.feed(p, n, at, scratch_);
  bool post = false;
  {
    std::lock_guard lk(m_);
    if (pending_.empty()) pending_.swap(scratch_);
    else
      for (SseEvent& e : scratch_) pending_.push_back(std::move(e));
    raw_.append(p, n);
    comments_ = parser_.comments();
    if (!posted_) post = posted_ = true;
  }
  if (post && notify_) notify_();
}

void SseTap::keep_partial(const char* p, size_t n) {
  if (n == 0 || partial_full_.load(std::memory_order_relaxed)) return;
  std::lock_guard lk(m_);
  const size_t room = kPartialMax - std::min(kPartialMax, partial_.size());
  partial_.append(p, std::min(room, n));
  if (partial_.size() >= kPartialMax) partial_full_.store(true, std::memory_order_relaxed);
}

void SseTap::head(std::shared_ptr<const HttpResponse> h) {
  if (!h) return;
  // Declared an event stream: no sniffing of the first bytes needed (head() runs on the worker
  // before the first feed()).
  if (state_.load(std::memory_order_relaxed) < 0 && is_event_stream(h->headers.get("content-type")))
    state_.store(1, std::memory_order_release);
  {
    std::lock_guard lk(m_);
    head_ = std::move(h);
  }
  head_ms_.store(std::chrono::duration<double, std::milli>(SteadyClock::now() - t0_).count(), std::memory_order_release);
  if (on_head_) on_head_();
}

std::shared_ptr<const HttpResponse> SseTap::head_response() const {
  std::lock_guard lk(m_);
  return head_;
}

std::string SseTap::partial() const {
  std::lock_guard lk(m_);
  return partial_;
}

SseTap::Batch SseTap::drain() {
  Batch b;
  std::lock_guard lk(m_);
  b.events.swap(pending_);
  b.raw.swap(raw_);
  b.comments = comments_;
  posted_ = false;
  return b;
}

}  // namespace watt::app::rv
