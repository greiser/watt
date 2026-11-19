#pragma once
// API specs for the sidebar's API SPECS section (Bruno's "API Specs"): the OpenAPI / Swagger
// files under <ws>/specs (*.yaml, *.yml, *.json) plus specs that were imported from elsewhere
// (recorded with their source path and the folder their requests went to). No wx: unit tested in
// tests/test_sidebar.cpp. Reads the disk: worker thread only.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace watt::app::sidebar {

struct SpecInfo {
  std::filesystem::path file;
  std::string kind;     // "OpenAPI 3.0.3", "Swagger 2.0"; empty: not an OpenAPI document
  std::string version;  // "3.0.3" (the openapi / swagger value)
  std::string title;    // info.title
  uint64_t size = 0;
  bool missing = false;   // a recorded spec whose file is gone
  bool imported = false;  // recorded by an import (outside <ws>/specs)
};

// Reads the first 16 KB of file and finds the openapi / swagger key and info.title (JSON or
// YAML). Cheap: a list of 200 specs costs a few ms.
SpecInfo peek_spec(const std::filesystem::path& file);

// The specs folder of a workspace: <root>/specs.
std::filesystem::path specs_dir(const std::filesystem::path& root);

// The OpenAPI files in <root>/specs (one level of sub-folders, at most 200 files, sorted by
// name, non-OpenAPI JSON / YAML left out) followed by `recorded` files not already listed
// (missing ones flagged, kept so the user can forget them).
std::vector<SpecInfo> scan_specs(const std::filesystem::path& root, const std::vector<std::filesystem::path>& recorded);

// True when p is root or inside it (lexically normalized, '/' and '\' alike, case-insensitive as
// on NTFS): "ws1" never matches "ws10", and "ws1/../ws2" is ws2.
bool in_workspace(const std::filesystem::path& p, const std::filesystem::path& root);

// Whether a spec preview's Sync may write: only into the workspace the tab was opened for
// (opened_for; empty: the current one), and only for a spec of that workspace (inside it, or
// recorded for it). A tab left over from another workspace never writes into this one.
bool spec_sync_allowed(const std::filesystem::path& spec, const std::filesystem::path& opened_for,
                       const std::filesystem::path& current_root, bool recorded);

// A request URL as an operation path: base URL / {{base}} and query cut, ":id" and "{{id}}" as
// "{id}", no trailing slash ("{{base}}/pets/:petId?x=1" -> "/pets/{petId}").
std::string spec_op_path(const std::string& url);

}  // namespace watt::app::sidebar
