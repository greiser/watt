#pragma once
// Every response in watt carries a Timing. All values are milliseconds, measured with a
// monotonic high-resolution clock. A phase that did not happen (reused connection, plaintext)
// is 0 and its `*_reused`/flag says why. `total_ms` is wall time from send() to last byte.
#include <chrono>
#include <cstdint>

namespace watt {

using Clock = std::chrono::steady_clock;

inline double ms_between(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

struct Timing {
  double queue_ms = 0;     // from user click/send() until the engine actually started I/O
  double dns_ms = 0;       // name resolution
  double connect_ms = 0;   // TCP handshake
  double tls_ms = 0;       // TLS handshake
  double send_ms = 0;      // request upload (pretransfer -> request fully written)
  double ttfb_ms = 0;      // waiting for first response byte after request was sent
  double download_ms = 0;  // first byte -> last byte
  double total_ms = 0;     // send() -> complete response available
  bool connection_reused = false;
  uint64_t bytes_sent = 0;
  uint64_t bytes_received = 0;
};

}  // namespace watt
