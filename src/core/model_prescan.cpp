#include "core/model_prescan.h"

#ifdef _WIN32
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#endif

namespace watt::detail {

#ifdef _WIN32

namespace {

using clock = std::chrono::steady_clock;
// A file appended to in a burst is opened once, this long after its last write, and never later
// than kMaxDelay after its first pending write.
constexpr auto kDebounce = std::chrono::milliseconds(500);
constexpr auto kMaxDelay = std::chrono::seconds(10);

void open_once(const std::filesystem::path& p) {
  // The open is what the antivirus filter scans; nothing needs to be read.
  HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

// Opens the files on up to 8 threads (the scanner works on several files at once).
void open_all(const std::vector<std::filesystem::path>& files) {
  if (files.empty()) return;
  std::atomic<size_t> next{0};
  auto work = [&] {
    for (size_t k; (k = next.fetch_add(1)) < files.size();) open_once(files[k]);
  };
  std::vector<std::thread> pool;
  for (size_t k = 1; k < std::min<size_t>(files.size(), 8); ++k) pool.emplace_back(work);
  work();
  for (auto& t : pool) t.join();
}

class Prescanner {
 public:
  ~Prescanner() {
    std::vector<std::filesystem::path> left;
    {
      std::unique_lock<std::mutex> lock(mu_);
      stop_ = true;
      cv_.notify_all();
      // A worker in the middle of an open finishes it first.
      idle_cv_.wait(lock, [&] { return busy_ == 0; });
      for (const auto& [p, e] : pending_)
        if (e.before_exit) left.push_back(p);
      pending_.clear();
    }
    if (worker_.joinable()) worker_.join();
    open_all(left);
    gone().store(true);
  }

  static std::atomic<bool>& gone() {
    static std::atomic<bool> g{false};
    return g;
  }

  void add(const std::filesystem::path& p, Prescan when) {
    std::lock_guard<std::mutex> lock(mu_);
    if (stop_) {
      if (when == Prescan::BeforeExit) open_once(p);
      return;
    }
    const auto now = clock::now();
    auto [it, fresh] = pending_.try_emplace(p);
    if (fresh) it->second.first = now;
    it->second.due = std::min(now + kDebounce, it->second.first + kMaxDelay);
    it->second.before_exit = it->second.before_exit || when == Prescan::BeforeExit;
    if (!worker_.joinable()) worker_ = std::thread([this] { run(); });
    cv_.notify_all();
  }

  void flush() {
    std::vector<std::filesystem::path> now;
    {
      std::unique_lock<std::mutex> lock(mu_);
      idle_cv_.wait(lock, [&] { return busy_ == 0; });
      for (const auto& [p, e] : pending_) now.push_back(p);
      pending_.clear();
    }
    open_all(now);
  }

 private:
  struct Entry {
    clock::time_point first{}, due{};
    bool before_exit = false;
  };

  void run() {
    std::unique_lock<std::mutex> lock(mu_);
    while (!stop_) {
      if (pending_.empty()) {
        cv_.wait(lock);
        continue;
      }
      auto next = std::min_element(pending_.begin(), pending_.end(),
                                   [](const auto& a, const auto& b) { return a.second.due < b.second.due; });
      if (clock::now() < next->second.due) {
        cv_.wait_until(lock, next->second.due);
        continue;
      }
      const std::filesystem::path p = next->first;
      pending_.erase(next);
      ++busy_;
      lock.unlock();
      open_once(p);
      lock.lock();
      --busy_;
      idle_cv_.notify_all();
    }
  }

  std::mutex mu_;
  std::condition_variable cv_, idle_cv_;
  std::map<std::filesystem::path, Entry> pending_;
  std::thread worker_;
  int busy_ = 0;
  bool stop_ = false;
};

Prescanner& prescanner() {
  static Prescanner p;
  return p;
}

bool disabled() {
  // WATT_NO_PRESCAN=1 turns it off (to measure what the restart path costs without it).
  static const bool off = [] {
    wchar_t v[8];
    return GetEnvironmentVariableW(L"WATT_NO_PRESCAN", v, 8) > 0 && v[0] != L'0';
  }();
  return off;
}

}  // namespace

void prescan_init() { (void)prescanner(); }

void prescan_later(const std::filesystem::path& p, Prescan when) {
  if (disabled()) return;
  if (Prescanner::gone().load()) {  // after static destruction: do it now
    if (when == Prescan::BeforeExit) open_once(p);
    return;
  }
  prescanner().add(p, when);
}

void prescan_flush() {
  if (!Prescanner::gone().load()) prescanner().flush();
}

#else

void prescan_init() {}
void prescan_later(const std::filesystem::path&, Prescan) {}
void prescan_flush() {}

#endif

}  // namespace watt::detail
