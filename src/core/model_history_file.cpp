#include "core/model_history_file.h"
#include "core/model_json.h"

#include <algorithm>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace watt::detail {

#ifdef _WIN32

HistoryFile::~HistoryFile() { close(); }

bool HistoryFile::is_open() const { return h_ != INVALID_HANDLE_VALUE; }

void HistoryFile::close() {
  if (is_open()) CloseHandle(h_);
  h_ = INVALID_HANDLE_VALUE;
}

bool HistoryFile::open(const std::filesystem::path& p, bool create, bool write) {
  write = write || create;
  if (is_open() && (writable_ || !write)) return true;
  close();
  const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
  const DWORD access = write ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
  h_ = CreateFileW(p.c_str(), access, share, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h_ == INVALID_HANDLE_VALUE && create) {
    detail::ensure_dir(p.parent_path());  // .watt: owner-only DACL, inherited (model_secure.h)
    h_ = CreateFileW(p.c_str(), access, share, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  }
  writable_ = is_open() && write;
  return is_open();
}

uint64_t HistoryFile::size() const {
  LARGE_INTEGER s{};
  if (!is_open() || !GetFileSizeEx(h_, &s)) return 0;
  return static_cast<uint64_t>(s.QuadPart);
}

bool HistoryFile::append(const std::string& data, uint64_t* end) {
  if (!is_open()) return false;
  OVERLAPPED ov{};
  ov.Offset = 0xFFFFFFFF;  // both 0xFFFFFFFF: write at the current end of file, atomically
  ov.OffsetHigh = 0xFFFFFFFF;
  DWORD written = 0;
  const BOOL ok = WriteFile(h_, data.data(), static_cast<DWORD>(data.size()), &written, &ov);
  if (end) *end = size();
  return ok && written == data.size();
}

bool HistoryFile::read_at(uint64_t offset, size_t n, std::string& out) const {
  out.resize(n);
  if (!is_open()) return false;
  size_t got = 0;
  while (got < n) {
    OVERLAPPED ov{};
    const uint64_t pos = offset + got;
    ov.Offset = static_cast<DWORD>(pos & 0xFFFFFFFFu);
    ov.OffsetHigh = static_cast<DWORD>(pos >> 32);
    DWORD r = 0;
    const DWORD want = static_cast<DWORD>(std::min<size_t>(n - got, 1u << 30));
    if (!ReadFile(h_, out.data() + got, want, &r, &ov) || r == 0) break;
    got += r;
  }
  out.resize(got);
  return true;
}

bool HistoryFile::rewrite(const std::string& data) {
  if (!is_open()) return false;
  OVERLAPPED ov{};
  DWORD written = 0;
  if (!data.empty() && (!WriteFile(h_, data.data(), static_cast<DWORD>(data.size()), &written, &ov) || written != data.size()))
    return false;
  LARGE_INTEGER pos{};
  pos.QuadPart = static_cast<LONGLONG>(data.size());
  return SetFilePointerEx(h_, pos, nullptr, FILE_BEGIN) && SetEndOfFile(h_);
}

#else

HistoryFile::~HistoryFile() { close(); }

bool HistoryFile::is_open() const { return fd_ >= 0; }

void HistoryFile::close() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

bool HistoryFile::open(const std::filesystem::path& p, bool create, bool write) {
  write = write || create;
  if (is_open() && (writable_ || !write)) return true;
  close();
  fd_ = ::open(p.c_str(), (write ? O_RDWR : O_RDONLY) | O_CLOEXEC);
  if (fd_ < 0 && create) {
    detail::ensure_dir(p.parent_path());  // .watt is created 0700 (model_secure.h)
    fd_ = ::open(p.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);  // personal: owner only
  }
  writable_ = is_open() && write;
  return is_open();
}

uint64_t HistoryFile::size() const {
  struct stat st {};
  if (!is_open() || fstat(fd_, &st) != 0) return 0;
  return static_cast<uint64_t>(st.st_size);
}

bool HistoryFile::append(const std::string& data, uint64_t* end) {
  if (!is_open()) return false;
  // End of file at the time of the write (not atomic across processes; history is per user).
  const off_t at = lseek(fd_, 0, SEEK_END);
  const bool ok = at >= 0 && ::pwrite(fd_, data.data(), data.size(), at) == static_cast<ssize_t>(data.size());
  if (end) *end = size();
  return ok;
}

bool HistoryFile::read_at(uint64_t offset, size_t n, std::string& out) const {
  out.resize(n);
  if (!is_open()) return false;
  size_t got = 0;
  while (got < n) {
    const ssize_t r = ::pread(fd_, out.data() + got, n - got, static_cast<off_t>(offset + got));
    if (r <= 0) break;
    got += static_cast<size_t>(r);
  }
  out.resize(got);
  return true;
}

bool HistoryFile::rewrite(const std::string& data) {
  if (!is_open()) return false;
  if (::pwrite(fd_, data.data(), data.size(), 0) != static_cast<ssize_t>(data.size())) return false;
  return ftruncate(fd_, static_cast<off_t>(data.size())) == 0;
}

#endif

}  // namespace watt::detail
