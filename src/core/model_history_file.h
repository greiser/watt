#pragma once
// A history file kept open for the life of a Workspace (internal to the workspace model).
//
// Why: on Windows, real-time antivirus scans a file when it is opened after a modification.
// Opening history.jsonl again after every append made the next listing cost ~30 ms on a 10k
// entry history (1.5 MB) instead of ~0.6 ms. One handle per file, used for appends
// (atomic end-of-file writes) and positional reads, opens it once per process.
// Handles are opened with FILE_SHARE_DELETE, so a trim in another process can delete an old
// segment that is open here.
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace watt::detail {

class HistoryFile {
 public:
  HistoryFile() = default;
  ~HistoryFile();
  HistoryFile(const HistoryFile&) = delete;
  HistoryFile& operator=(const HistoryFile&) = delete;

  // Opens (creating the file and its directory if `create`) for reading, and for writing if
  // `write` (a read-only handle is reopened). Idempotent while open.
  bool open(const std::filesystem::path& p, bool create, bool write = true);
  bool is_open() const;
  void close();

  uint64_t size() const;
  // Appends at the end in one write; *end = file size right after it.
  bool append(const std::string& data, uint64_t* end);
  // Reads up to n bytes at `offset` into out (resized to what was read).
  bool read_at(uint64_t offset, size_t n, std::string& out) const;
  // Replaces the whole content.
  bool rewrite(const std::string& data);

 private:
  bool writable_ = false;
#ifdef _WIN32
  void* h_ = reinterpret_cast<void*>(static_cast<intptr_t>(-1));
#else
  int fd_ = -1;
#endif
};

// History is kept in segments under .watt/history/: <seq>.jsonl (the list) and <seq>.req.jsonl
// (the request records), at most 1000 entries each. Only the newest segment is appended to; the
// others never change again, so after a restart only that one small file can need an antivirus
// scan, and trimming deletes whole old segments instead of rewriting anything. Segment 0 is the
// pre-segment layout (.watt/history.jsonl + history.req.jsonl), read until it is migrated.
struct HistorySegment {
  HistoryFile list;
  HistoryFile recs;
};

struct HistoryHandles {
  std::map<uint32_t, std::unique_ptr<HistorySegment>> segs;  // opened lazily, by seq
  uint32_t head = 0;        // segment appends go to (0: not known yet)
  int64_t head_count = -1;  // its entries, as of the last append from this process
  bool checked = false;     // first append in this process: migration and trim done
  std::mutex mu;            // one append / read at a time (Workspace copies on several threads)
};

}  // namespace watt::detail
