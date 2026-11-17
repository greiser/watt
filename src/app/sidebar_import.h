#pragma once
// Collection import for the sidebar's Import menu (no wx: unit tested in tests/test_sidebar.cpp).
//
// Formats: Postman Collection v2.0 / v2.1 (JSON), Insomnia v4 export (JSON), a Bruno collection
// folder (.bru files), OpenAPI 3 / Swagger 2 (JSON or YAML, one request per operation, the first
// tag is its folder) and HAR 1.2. parse_*() only reads the given file / folder; write_import()
// writes .req.json files under one new folder (named after the collection) of the chosen folder.
// Both are meant for a worker thread; the UI then rescans once.
//
// Safety: an imported collection is untrusted input. Requests whose URL has a scheme other than
// http / https, or whose method is not an HTTP token, are skipped (with the reason); folder and
// file names are made file-system safe and every file goes through Workspace::save_request(),
// which refuses paths outside the workspace. Files a request uploads (Postman form-data file
// parts, Bruno @file(...)) stay path references, read only at send time under the workspace's
// upload consent (Workspace::blocked_files()). Scripts are imported as code but run only once the
// user trusts the workspace's scripts.
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/model.h"

namespace watt::app::sidebar {

struct ImportFolder {
  std::string name;
  // Collection / folder variables (Postman collection and folder "variable", the Insomnia base
  // environment, Bruno collection.bru / folder.bru vars, the OpenAPI baseUrl). write_import()
  // saves them as this folder's FolderSettings::vars (<folder>/folder.json), so they resolve under
  // any active environment, the way Postman and Bruno treat collection variables.
  std::vector<SettingsRow> vars;
  std::vector<SavedRequest> requests;  // in collection order
  std::vector<ImportFolder> folders;
};

struct ImportResult {
  std::string format;  // "Postman Collection v2.1", "Insomnia v4", "Bruno", "OpenAPI 3.0.3", "HAR"
  ImportFolder root;   // root.name: the collection's name (the folder it is written to)
  // Real environments only: a Postman environment file, Insomnia sub-environments, Bruno
  // environments/*.bru, OpenAPI credential placeholders. Collection variables are root.vars.
  std::vector<Environment> envs;
  std::vector<std::string> skipped;  // "<name>: <why>"
  std::vector<std::string> notes;    // imported, but not exactly (e.g. an auth type watt lacks)
};

// Detects the format of a file's text (JSON, or YAML for OpenAPI) and parses it. nullopt with
// *err when it is none of the formats above (or malformed).
std::optional<ImportResult> parse_collection_text(const std::string& text, std::string* err);
// Reads `file` (at most 64 MB) and parses it with parse_collection_text(). A folder is read as a
// Bruno collection.
std::optional<ImportResult> parse_collection_file(const std::filesystem::path& file, std::string* err);
std::optional<ImportResult> parse_bruno_folder(const std::filesystem::path& dir, std::string* err);

// The YAML subset OpenAPI documents use (block maps / sequences, flow [] {}, quoted, plain and
// | > block scalars, comments) converted to JSON text. nullopt with *err on what it cannot read.
std::optional<std::string> yaml_to_json(const std::string& yaml, std::string* err);

struct ImportWritten {
  size_t requests = 0;
  size_t folders = 0;                // created, the collection's own folder included
  size_t vars = 0;                   // collection / folder variables written to folder.json files
  std::vector<std::string> skipped;  // parse skips, then write failures
  std::filesystem::path top;         // the collection's folder
  std::filesystem::path first_file;  // the first request written (to select)
  // Collection order per folder (file names), for the sidebar's manual order.
  std::map<std::filesystem::path, std::vector<std::wstring>> order;
};
// Writes r under dest / <collection name> (" 2", " 3" ... when that exists). Does not touch the
// workspace's environments: merge r.envs on the UI thread (merge_import_envs).
ImportWritten write_import(Workspace& ws, const std::filesystem::path& dest, ImportResult& r);

// Adds the imported environments to `envs`: a new name is appended, an existing one gets the
// variables it lacks (its own values win). Returns how many environments were added or changed.
size_t merge_import_envs(std::vector<Environment>& envs, const std::vector<Environment>& imported);

// "Imported 12 requests, 3 folders, 2 skipped (ftp:// URL; ...)" for the import notice.
std::string import_summary(const ImportResult& r, const ImportWritten& w);

// Export: a folder (or the whole workspace) as a Postman Collection v2.1 (folders, requests,
// disabled headers / params, path variables, bodies, auth, scripts as events, docs; gRPC
// requests are skipped, Postman collections cannot hold them), or as a list of curl / grpcurl
// commands with {{vars}} resolved, each under a "# folder/name" comment. Reads the request
// files: call on a worker (with a copy of the workspace). `order`: the sidebar's dragged order
// per folder (file names), else natural name order.
struct ExportResult {
  std::string text;
  size_t requests = 0, folders = 0;
  std::vector<std::string> skipped;  // "<name>: <why>"
};
ExportResult export_postman(const Workspace& ws, const std::filesystem::path& folder, const std::string& name,
                            const std::map<std::filesystem::path, std::vector<std::wstring>>* order = nullptr);
ExportResult export_curl(const Workspace& ws, const std::filesystem::path& folder, CurlShell shell = CurlShell::Bash,
                         const std::map<std::filesystem::path, std::vector<std::wstring>>* order = nullptr);

}  // namespace watt::app::sidebar
