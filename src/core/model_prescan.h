#pragma once
// Antivirus pre-scan of files watt just wrote (internal to the workspace model).
//
// Why: Windows real-time antivirus (Defender) scans a file when it is first opened after a
// modification, and blocks that open for the scan: 20-50 ms for a JSON file over ~8 KiB, almost
// independent of its size. The verdict is cached until the file changes again, for every
// process. So the first open of history, index.json or an environment in the next watt start
// (the restart path) paid that scan for every file the previous run wrote.
//
// prescan_later() re-opens such a file shortly after the write (debounced: a burst of appends
// costs one scan) on a background thread, so the scan runs while nobody waits and the next
// start opens it warm. Files marked BeforeExit that are still pending when the process exits
// (a CLI run, or a write just before quitting) are opened then, in parallel. Background ones
// are dropped at exit. Elsewhere than Windows these are no-ops; WATT_NO_PRESCAN=1 disables it.
#include <filesystem>

namespace watt::detail {

enum class Prescan {
  Background,  // nice to have (a saved request): skipped if still pending at exit
  BeforeExit,  // read by the next start (history, index, settings): done at exit if pending
};

void prescan_later(const std::filesystem::path& p, Prescan when = Prescan::BeforeExit);
// Opens every pending file now and waits (tests, benchmarks).
void prescan_flush();
// Makes sure the pre-scanner outlives objects created after this call (their destructors may
// call prescan_later at exit).
void prescan_init();

}  // namespace watt::detail
