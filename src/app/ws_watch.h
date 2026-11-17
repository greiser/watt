#pragma once
// Watches a workspace folder for changes made outside watt (git pull, an editor, a script),
// like Bruno's collection watcher: ReadDirectoryChangesW on the root (recursive) on its own
// thread, changes under .watt/ and .git/ ignored, collected until 300 ms pass without another
// one, then handed over as one batch. The UI decides what to reload (sidebar.cpp). No wx.
#include <atomic>
#include <filesystem>
#include <functional>
#include <thread>
#include <vector>

#include "core/model.h"

namespace watt::app {

class WorkspaceWatcher {
 public:
  // Paths relative to the root. An empty path means "too many changes to list" (the system's
  // buffer overflowed): reload everything.
  using Batch = std::vector<std::filesystem::path>;
  // on_batch runs on the watcher thread (post to the UI from it).
  WorkspaceWatcher(std::filesystem::path root, std::function<void(Batch)> on_batch);
  ~WorkspaceWatcher();  // stops the thread (it wakes at once) and joins it
  WorkspaceWatcher(const WorkspaceWatcher&) = delete;
  WorkspaceWatcher& operator=(const WorkspaceWatcher&) = delete;

 private:
  void run();
  std::filesystem::path root_;
  std::function<void(Batch)> on_batch_;
  void* stop_ = nullptr;  // event handle
  std::thread thread_;
};

// Whether two requests hold the same content (what a .req.json stores), to tell a change made
// elsewhere from watt's own save of what a tab shows.
bool same_request(const SavedRequest& a, const SavedRequest& b);
// Same for the environments, globals and .env values of two workspaces of one root.
bool same_environments(Workspace& a, Workspace& b);

}  // namespace watt::app
