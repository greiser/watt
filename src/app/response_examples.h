#pragma once
// Saved example responses of a request (Postman's "Save as example"): a response kept with its
// timing, headers and body, to look at again or compare with later. Stored per user, never in the
// shared workspace files: <ws>/.watt/examples/<request id>/<yyyymmdd-hhmmss>-<status>-<name>.json
// (.watt is the workspace's personal, owner-only folder; a response may hold tokens).
//
// All functions do file I/O: call them on a worker thread, never on the UI thread.
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "core/http_client.h"

namespace watt::app::rv {

struct ExampleEntry {
  std::filesystem::path file;
  std::string name;      // as the user typed it (from the file name)
  std::string saved_at;  // "2026-11-17 08:30:12"
  long status = 0;
};

// The examples folder of a request: inside ws_root/.watt/examples, named after the request id
// (only [A-Za-z0-9_-] kept, so a hostile id cannot leave the folder). Empty when there is no id.
std::filesystem::path examples_dir(const std::filesystem::path& ws_root, const std::string& request_id);

// Writes r (and the method / URL it was sent with) as a new example. false + *err on failure.
bool save_example(const std::filesystem::path& dir, const std::string& name, const HttpResponse& r,
                  const std::string& method, const std::string& url, std::string* err);

// Newest first; at most 200. Only regular files directly in dir with the example name pattern.
std::vector<ExampleEntry> list_examples(const std::filesystem::path& dir);

// Reads an example back (nullptr + *err when unreadable, too big or not an example).
std::shared_ptr<HttpResponse> load_example(const std::filesystem::path& file, std::string* method, std::string* url,
                                           std::string* err);

}  // namespace watt::app::rv
