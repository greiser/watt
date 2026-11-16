#pragma once
// Workspace model. A workspace is a folder on disk (git-friendly, like Bruno):
//   <ws>/watt.json                 workspace settings + environments
//   <ws>/<folder>/.../<name>.req.json  one saved request per file
//   <ws>/.watt/history.jsonl       append-only send history (capped)
// Variables: {{name}} substituted from the active environment, then workspace globals.
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/grpc_client.h"
#include "core/http_client.h"

namespace watt {

enum class RequestKind { Http, Grpc };

struct SavedRequest {
  std::string id;                 // stable uuid
  std::string name;
  std::filesystem::path file;     // absolute path of the .req.json (empty if unsaved)
  RequestKind kind = RequestKind::Http;
  // HTTP
  HttpRequest http;
  // gRPC
  GrpcTarget grpc_target;
  std::string grpc_method;        // full path "/pkg.Svc/Method"
  std::string grpc_body;          // JSON
  GrpcCallOptions grpc_opts;
  std::vector<std::string> proto_files;   // empty => use reflection
  std::vector<std::string> import_dirs;
};

struct Environment {
  std::string name;
  std::map<std::string, std::string> vars;
};

struct HistoryEntry {
  std::string when_iso;
  RequestKind kind = RequestKind::Http;
  std::string summary;            // "GET https://..." or "/pkg.Svc/Method @ host:port"
  long status = 0;                // HTTP status or grpc code
  double total_ms = 0;
  SavedRequest request;
};

struct CollectionNode {           // folder tree for the sidebar
  std::string name;
  std::filesystem::path path;
  bool is_folder = false;
  std::vector<CollectionNode> children;
};

class Workspace {
 public:
  static std::optional<Workspace> open(const std::filesystem::path& root, std::string* err = nullptr);
  static Workspace create(const std::filesystem::path& root);

  const std::filesystem::path& root() const { return root_; }
  CollectionNode tree() const;    // scans root for folders and *.req.json

  bool save_request(SavedRequest& r, std::string* err = nullptr);  // assigns file if empty
  std::optional<SavedRequest> load_request(const std::filesystem::path& file, std::string* err = nullptr) const;
  bool remove(const std::filesystem::path& p);
  bool rename(const std::filesystem::path& from, const std::string& new_name);

  std::vector<Environment>& environments() { return envs_; }
  std::string active_env;
  std::map<std::string, std::string> globals;
  bool save_settings(std::string* err = nullptr) const;

  void append_history(const HistoryEntry& e);
  std::vector<HistoryEntry> history(size_t max = 500) const;

  // {{var}} substitution using active env then globals. Unknown vars are left as-is.
  std::string expand(const std::string& s) const;
  SavedRequest expanded(const SavedRequest& r) const;

 private:
  std::filesystem::path root_;
  std::vector<Environment> envs_;
};

// Import/export helpers that make switching from other tools cheap.
std::optional<HttpRequest> parse_curl_command(const std::string& cmd, std::string* err = nullptr);
std::string to_curl_command(const HttpRequest& r);
std::string to_grpcurl_command(const SavedRequest& r);

}  // namespace watt
