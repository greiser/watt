// UI-thread stall probe for the sidebar self test (WATT_SIDEBAR_SELFTEST=<file>): a worker
// sends WM_NULL to the main window every 2 ms with SendMessageTimeout and keeps the longest
// wait, the same measurement the critics use from outside the process.
#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace watt::app::sidebar {

class StallProbe {
 public:
  using Clock = std::chrono::steady_clock;
  explicit StallProbe(void* hwnd);
  // Never destroyed while the app runs (the worker may be waiting on the UI thread).
  StallProbe(const StallProbe&) = delete;
  StallProbe& operator=(const StallProbe&) = delete;

  // Longest stall (ms) since the last reset.
  double max_ms() const { return max_us_.load() / 1000.0; }
  void reset();
  // Stalls over 20 ms since the last reset: (start, ms).
  std::vector<std::pair<Clock::time_point, double>> long_stalls();

 private:
  void* hwnd_;
  std::atomic<long long> max_us_{0};
  std::mutex mu_;
  std::vector<std::pair<Clock::time_point, double>> long_;
  std::thread th_;
};

}  // namespace watt::app::sidebar

namespace watt::app::sidebar {
// WATT_CRASHLOG=<file> (self tests and critics): an unhandled exception writes its code, the
// faulting address and a symbolized stack of the crashing thread to <file> before the process
// dies (dbghelp, loaded only then). No effect when the variable is unset.
void install_crash_log();
}  // namespace watt::app::sidebar
