#pragma once
// Internal helpers for the workspace model (not part of the public core contract).
// - UTF-8 <-> std::filesystem::path conversion (paths are wide on Windows).
// - A deterministic pretty printer: 2-space indent, members in insertion order, short scalar
//   pairs such as ["Accept", "application/json"] kept on one line, multi-line arrays with leading
//   commas (appending a header is a one-line diff), trailing newline. The same
//   value always prints to the same bytes, so saved requests diff cleanly in git.
// - Whole-file read and atomic (write temp + rename) write.
#include <cstdint>
#include <filesystem>
#include <string>

#include <rapidjson/document.h>

namespace watt::detail {

namespace fs = std::filesystem;

inline std::string to_utf8(const fs::path& p) {
  const std::u8string u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

inline fs::path from_utf8(const std::string& s) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string json_compact(const rapidjson::Value& v);
std::string json_pretty(const rapidjson::Value& v);

bool read_file(const fs::path& p, std::string& out);
// create_directories, but cheap when dir exists (MSVC's create_directories tries to create
// every component: ~0.6 ms even when all exist).
void ensure_dir(const fs::path& dir);
// The first n bytes (fewer if the file is shorter).
bool read_file_head(const fs::path& p, std::string& out, size_t n);
// Appends with one write call (atomic w.r.t. other appenders); returns the new file size.
bool append_file(const fs::path& p, const std::string& data, uint64_t* new_size = nullptr);
// private_file: created owner-only (POSIX 0600; files under a .watt directory always are).
bool write_file_atomic(const fs::path& p, const std::string& data, std::string* err = nullptr,
                       bool private_file = false);

std::string base64(const std::string& in);
// Stops at '='; false on any other character outside the base64 alphabet.
bool base64_decode(const std::string& in, std::string& out);
// Whether s is well-formed UTF-8 (what a JSON string must hold).
bool valid_utf8(const std::string& s);

}  // namespace watt::detail
