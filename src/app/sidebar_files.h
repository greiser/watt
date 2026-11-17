#pragma once
// File operations behind the sidebar's tree (no wx: unit tested in tests/test_sidebar.cpp).
//
// Request files use the workspace's kebab-case convention: "Get order copy" is stored as
// get-order-copy.req.json with "name": "Get order copy". Renames, duplicates and new requests
// all follow it, so a workspace never mixes "Get item seven.req.json" and get-item-seven.req.json.
// Folders keep the name as typed (made file-system safe).
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "core/model.h"

namespace watt::app::sidebar {

// "Get order copy" -> "get-order-copy": lower-case ASCII letters and digits, other runs -> '-'
// (UTF-8 letters are kept). Never empty ("request").
std::string slug(std::string_view name);

// A file / folder name without the characters Windows refuses ("a/b" -> "a_b").
std::string file_safe(std::string_view name);

// folder / "<slug(name)>.req.json", or with " 2", " 3" ... appended to the name until the
// file does not exist. *final_name (optional) receives the name that goes with the file.
std::filesystem::path free_request_file(const std::filesystem::path& folder, const std::string& name,
                                        std::string* final_name = nullptr);

// Renames a request file or a folder. A request gets the new "name" field and the kebab-case
// file name; a folder is renamed to file_safe(name). Returns the new path (== from when only
// the name field changed), or nullopt with *err (the target exists, or a write failed).
std::optional<std::filesystem::path> rename_item(Workspace& ws, const std::filesystem::path& from,
                                                 const std::string& name, std::string* err);

// Writes a copy of `from` next to it, named "<name> copy" (or "copy 2" ...), with a new id.
// Returns the saved copy (file and name set) or nullopt with *err.
std::optional<SavedRequest> duplicate_request(Workspace& ws, const std::filesystem::path& from, std::string* err);

// A new, empty request of `kind` named `name` (made unique) in `folder`, saved at once so it
// shows in the tree and can be renamed like any other.
std::optional<SavedRequest> create_request(Workspace& ws, const std::filesystem::path& folder, RequestKind kind,
                                           const std::string& name, std::string* err);

// How well a saved request matches the sidebar filter (higher is better; < 0: no match).
// Exact name > name prefix > every word in the name > words spread over the folder path /
// URL. Used to pick the request Enter opens.
int filter_rank(std::string_view query_lower, std::string_view name, std::string_view folder_path,
                std::string_view rest);

}  // namespace watt::app::sidebar
