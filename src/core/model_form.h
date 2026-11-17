#pragma once
// Multipart form parts (curl -F / --form-string, Postman form-data, Bruno multipartForm).
//
// A saved request keeps its form as structured parts, never as a flattened body: file parts are
// path references (like Bruno's @file(path)), so a .req.json never holds file bytes, a binary
// file survives every curl export, and an edited file is picked up on the next send.
// build_form_body() turns the parts into the body at send time, in exactly the format libcurl's
// curl_mime produces for `curl -F` (Content-Disposition escaping, content types inferred from
// the file extension, one CRLF per line).
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/http_client.h"

namespace watt {

struct FormPart {
  enum class Kind {
    Text,         // name=value
    File,         // name=@path   upload: filename = base name of path unless `filename` is set
    FileContent,  // name=<path   the file's bytes as a plain field value (no filename)
  };
  std::string name;
  std::string value;     // Text: the value. File/FileContent: the path ({{vars}} allowed;
                         // relative paths resolve against the workspace root; see
                         // Workspace::blocked_files() for what a workspace may upload)
  Kind kind = Kind::Text;
  bool literal = false;  // Text from --form-string: exported as --form-string, no ;type= parsing
  std::string filename;  // ;filename= (File: overrides the base name; Text: makes it a file part)
  std::string type;      // ;type= Content-Type of the part; empty = curl's default (see below)
  bool enabled = true;   // false: kept in the request, not sent (UI checkbox)

  bool operator==(const FormPart&) const = default;
};

// curl's content-type table for file names (lib/mime.c): .gif .jpg .jpeg .png .svg .txt .htm
// .html .pdf .xml, case-insensitive. Empty when the extension is not in it.
std::string_view mime_type_for_filename(std::string_view filename);

// The Content-Type curl sends for a part when `type` is empty: File -> from `filename` (or the
// path), else application/octet-stream; Text with a filename -> from the filename, else none;
// FileContent -> none.
std::string form_part_content_type(const FormPart& p);

// Reads the files and writes the multipart body into r.body and its Content-Type (with
// boundary) into r.headers. A Content-Type header the user set (e.g. multipart/mixed) is kept
// and gets the boundary appended, as curl does. Relative paths resolve against `base_dir` when it
// is not empty (never the current directory then), else against the current directory (a pasted
// curl command, as curl itself). Returns false (and r unchanged) if a file cannot be read. No
// access policy here: Workspace::http_to_send() checks the paths first.
bool build_form_body(const std::vector<FormPart>& parts, HttpRequest& r,
                     const std::filesystem::path& base_dir = {}, std::string* err = nullptr);

// One part as a curl argument, without shell quoting: {"-F", "name=@path;type=x"} or
// {"--form-string", "name=value"}.
std::pair<std::string, std::string> form_part_curl_arg(const FormPart& p);

}  // namespace watt
