// Workspace folder watcher (see ws_watch.h).
#include "app/ws_watch.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cwctype>
#include <set>
#include <string>
#include <vector>

namespace watt::app {

namespace fs = std::filesystem;

namespace {

// ".watt\x", ".git\x" (any case): watt's own private state and git's internals.
bool ignored(const std::wstring& rel) {
  auto starts = [&](const wchar_t* dir) {
    const size_t n = std::wcslen(dir);
    if (rel.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
      if (std::towlower(rel[i]) != dir[i]) return false;
    return rel.size() == n || rel[n] == L'\\' || rel[n] == L'/';
  };
  return starts(L".watt") || starts(L".git");
}

}  // namespace

WorkspaceWatcher::WorkspaceWatcher(fs::path root, std::function<void(Batch)> on_batch)
    : root_(std::move(root)), on_batch_(std::move(on_batch)) {
  stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (stop_) thread_ = std::thread([this] { run(); });
}

WorkspaceWatcher::~WorkspaceWatcher() {
  if (stop_) SetEvent(stop_);
  if (thread_.joinable()) thread_.join();
  if (stop_) CloseHandle(stop_);
}

void WorkspaceWatcher::run() {
  HANDLE dir = CreateFileW(root_.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
  if (dir == INVALID_HANDLE_VALUE) return;
  OVERLAPPED ov{};
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::vector<DWORD> words(16 * 1024);  // 64 KB, DWORD-aligned as the API needs
  auto* buf = reinterpret_cast<unsigned char*>(words.data());
  const DWORD buf_size = static_cast<DWORD>(words.size() * sizeof(DWORD));
  const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                       FILE_NOTIFY_CHANGE_SIZE;
  std::set<std::wstring> pending;
  bool overflow = false;
  bool armed = false;
  for (;;) {
    if (!armed) {
      ResetEvent(ov.hEvent);
      if (!ReadDirectoryChangesW(dir, buf, buf_size, TRUE, filter, nullptr, &ov, nullptr)) break;
      armed = true;
    }
    const HANDLE waits[2] = {stop_, ov.hEvent};
    const bool quiet_wait = !pending.empty() || overflow;
    const DWORD w = WaitForMultipleObjects(2, waits, FALSE, quiet_wait ? 300 : INFINITE);
    if (w == WAIT_OBJECT_0) break;
    if (w == WAIT_TIMEOUT) {
      // 300 ms without another change: one batch.
      Batch b;
      if (overflow) b.emplace_back();
      for (const auto& p : pending) b.emplace_back(p);
      pending.clear();
      overflow = false;
      if (on_batch_) on_batch_(std::move(b));
      continue;
    }
    if (w != WAIT_OBJECT_0 + 1) break;
    armed = false;
    DWORD got = 0;
    if (!GetOverlappedResult(dir, &ov, &got, FALSE)) break;
    if (got == 0) {  // the system's buffer overflowed: we do not know what changed
      overflow = true;
      continue;
    }
    for (size_t at = 0;;) {
      const auto* fni = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buf + at);
      std::wstring rel(fni->FileName, fni->FileNameLength / sizeof(wchar_t));
      if (!rel.empty() && !ignored(rel)) pending.insert(std::move(rel));
      if (!fni->NextEntryOffset) break;
      at += fni->NextEntryOffset;
    }
  }
  if (armed) {
    CancelIoEx(dir, &ov);
    DWORD got = 0;
    GetOverlappedResult(dir, &ov, &got, TRUE);
  }
  CloseHandle(ov.hEvent);
  CloseHandle(dir);
}

bool same_request(const SavedRequest& a, const SavedRequest& b) {
  return a.name == b.name && a.docs == b.docs && a.kind == b.kind && a.http.method == b.http.method &&
         a.http.url == b.http.url && a.http.headers == b.http.headers && a.http.body == b.http.body &&
         a.http.timeout_ms == b.http.timeout_ms && a.http.follow_redirects == b.http.follow_redirects &&
         a.http.verify_tls == b.http.verify_tls && a.http.version == b.http.version &&
         a.http.fresh_connection == b.http.fresh_connection && a.disabled_headers == b.disabled_headers &&
         a.disabled_params == b.disabled_params && a.path_params == b.path_params && a.form == b.form &&
         a.basic_auth == b.basic_auth && a.body_file == b.body_file && a.body_data == b.body_data &&
         a.header_files == b.header_files && a.cookie_files == b.cookie_files && a.grpc_target == b.grpc_target &&
         a.grpc_method == b.grpc_method && a.grpc_body == b.grpc_body &&
         a.grpc_opts.deadline_ms == b.grpc_opts.deadline_ms && a.grpc_opts.metadata == b.grpc_opts.metadata &&
         a.proto_files == b.proto_files && a.import_dirs == b.import_dirs && a.asserts == b.asserts &&
         a.captures == b.captures && a.auth == b.auth && a.network == b.network && a.pre_script == b.pre_script &&
         a.post_script == b.post_script;
}

bool same_environments(Workspace& a, Workspace& b) {
  if (a.globals != b.globals || a.dotenv != b.dotenv) return false;
  const auto& x = a.environments();
  const auto& y = b.environments();
  if (x.size() != y.size()) return false;
  for (size_t i = 0; i < x.size(); ++i)
    if (x[i].name != y[i].name || x[i].vars != y[i].vars || x[i].secrets != y[i].secrets ||
        x[i].unreadable_secrets != y[i].unreadable_secrets)
      return false;
  return true;
}

}  // namespace watt::app
