#pragma once
// Request display names and URLs for the sidebar. The folder scan (Workspace::tree) gives file
// names ("get-order"); tabs show the request's "name" field ("Get order"). This cache reads the
// first 2 KiB of each request file (where the serializer puts "name" and the URL) on a worker,
// keyed by path and validated by mtime + size, and keeps a copy in .watt/sidebar-names.tsv so a
// restart shows the real names at once.
//
// Threading: apply_cached() and refresh() run on worker threads; the cache is locked inside.
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "core/model.h"

namespace watt::app::sidebar {

struct ReqMeta {
  std::string name;  // the request's name field ("" when the head did not have one)
  std::string url;   // HTTP url (unexpanded) or the gRPC method
  std::string id;    // the request's id (history entries store it: they map back by id)
};

using MetaMap = std::map<std::filesystem::path, ReqMeta>;

class NameCache {
 public:
  explicit NameCache(std::filesystem::path root);

  // Fast pass, no file access beyond the cache file (loaded once): sets the name of every
  // request in `tree` that has a cached entry and returns the cached metadata by path.
  MetaMap apply_cached(CollectionNode& tree);
  // Slow pass: stats every request file and reads the heads of new / changed ones. Sets the
  // names in `tree` and returns the metadata; *changed is true when anything differs from what
  // apply_cached() returned. Writes the cache file when something changed.
  MetaMap refresh(CollectionNode& tree, bool* changed);
  // A request was saved or renamed by this app (UI thread is fine: two stats, no reads).
  void note(const std::filesystem::path& file, const std::string& name, const std::string& url,
            const std::string& id = {});

 private:
  struct Entry {
    int64_t mtime = 0;
    uint64_t size = 0;
    ReqMeta meta;
  };
  void load_locked();
  void save_locked();

  std::filesystem::path root_;
  std::mutex mu_;
  bool loaded_ = false;
  std::map<std::filesystem::path, Entry> entries_;
};

// Reads "name" and the URL / gRPC method from the start of a request file.
bool read_request_meta(const std::filesystem::path& file, ReqMeta* out);

}  // namespace watt::app::sidebar
