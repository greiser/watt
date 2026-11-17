#include "app/sidebar_probe.h"

#include <windows.h>

namespace watt::app::sidebar {

StallProbe::StallProbe(void* hwnd) : hwnd_(hwnd) {
  th_ = std::thread([this] {
    for (;;) {
      const auto t0 = Clock::now();
      DWORD_PTR r = 0;
      SendMessageTimeoutW(static_cast<HWND>(hwnd_), WM_NULL, 0, 0, SMTO_NORMAL, 5000, &r);
      const long long us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
      long long cur = max_us_.load();
      while (us > cur && !max_us_.compare_exchange_weak(cur, us)) {
      }
      if (us > 20000) {
        std::lock_guard<std::mutex> lock(mu_);
        long_.emplace_back(t0, us / 1000.0);
      }
      // Not Sleep(2): that waits a whole 15.6 ms timer tick and hides the start of a stall.
      const auto next = Clock::now() + std::chrono::milliseconds(2);
      while (Clock::now() < next) std::this_thread::yield();
    }
  });
  th_.detach();
}

void StallProbe::reset() {
  max_us_.store(0);
  std::lock_guard<std::mutex> lock(mu_);
  long_.clear();
}

std::vector<std::pair<StallProbe::Clock::time_point, double>> StallProbe::long_stalls() {
  std::lock_guard<std::mutex> lock(mu_);
  return long_;
}

}  // namespace watt::app::sidebar
