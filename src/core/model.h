#pragma once
// Workspace model. A workspace is a folder on disk (git-friendly, like Bruno):
//   <ws>/watt.json                   workspace settings (globals)
//   <ws>/environments/<name>.json      one file per environment (no merge conflicts between them)
//   <ws>/<folder>/.../<name>.req.json  one saved request per file
//   <ws>/.env                          KEY=value lines for {{process.env.KEY}} (keep out of git)
//   <ws>/.watt/                      personal, gitignored, owner-only (Windows: protected DACL for
//                                      the current user and SYSTEM; POSIX: 0700, files 0600):
//                                      history/ (append-only segments, capped), state.json (active
//                                      environment), secrets.json (secret values, DPAPI-sealed on
//                                      Windows)
// Per user, outside every workspace (a cloned repo cannot plant it): <config>/allowed_files.json,
// files a workspace may upload from outside its root, see Workspace::blocked_files().
// <config> = %APPDATA%\watt (POSIX: $XDG_CONFIG_HOME/watt or ~/.config/watt, 0700 / 0600).
// Variables: {{name}} substituted from the active environment, then workspace globals, then
// dynamic variables ({{$guid}}, {{process.env.X}} from .env or the OS environment, ...).
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/grpc_client.h"
#include "core/http_client.h"
#include "core/model_form.h"

namespace watt {

namespace detail {
struct HistoryHandles;
struct TreeIndex;
}

enum class RequestKind { Http, Grpc };

// HTTP basic auth (curl -u, Bruno's auth:basic, Postman's Auth tab). Kept as fields so user and
// password can be {{vars}} or secrets instead of a fixed base64 header in git. The header
// "Authorization: Basic base64(user:password)" is added at send time by
// Workspace::http_to_send(), unless the request sets its own Authorization header (as curl).
struct BasicAuth {
  bool enabled = false;
  std::string user;
  std::string password;
  bool operator==(const BasicAuth&) const = default;
};

struct SavedRequest {
  std::string id;                 // stable uuid
  std::string name;
  std::filesystem::path file;     // absolute path of the .req.json (empty if unsaved)
  RequestKind kind = RequestKind::Http;
  // HTTP
  HttpRequest http;
  // Indexes into http.headers of headers kept but not sent (an unchecked row, like Bruno's
  // ~Header or Postman's disabled header): saved in place in the .req.json, left out by
  // http_to_send() and to_curl_command(SavedRequest).
  std::set<size_t> disabled_headers;
  // multipart/form-data parts. When not empty they replace http.body: the body is built from
  // them (files read) at send time, see Workspace::http_to_send(). Stored in the .req.json with
  // file parts as path references.
  std::vector<FormPart> form;
  BasicAuth basic_auth;
  // A file sent as the raw body (curl -T), read at send time like form files (relative paths
  // against the workspace root; may hold {{vars}}). When set, http.body is not saved.
  std::string body_file;
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
  // Names of vars whose values are secret: environments/<name>.json lists only the name, the
  // value lives in .watt/secrets.json (like Bruno's vars:secret).
  std::set<std::string> secrets;
  // Secret values in .watt/secrets.json that could not be decrypted here (sealed by another
  // Windows user or machine, or corrupt): var name -> the stored value, as-is. Such a var is left
  // out of `vars` (so {{var}} stays unresolved) and the UI should flag it. save_settings() writes
  // the stored value back unchanged, so it is never lost; setting a value in `vars` replaces it.
  std::map<std::string, std::string> unreadable_secrets;
};

struct HistoryEntry {
  std::string when_iso;
  RequestKind kind = RequestKind::Http;
  std::string summary;            // "GET https://..." or "/pkg.Svc/Method @ host:port"
  long status = 0;                // HTTP status or grpc code
  double total_ms = 0;
  // What append_history() stores. history() leaves it empty (listing 10k entries must stay
  // fast); history_request() loads it when an entry is opened.
  SavedRequest request;
  uint64_t offset = UINT64_MAX;   // set by history(): where the entry is (segment, byte offset)
};

struct CollectionNode {           // folder tree for the sidebar
  std::string name;
  std::filesystem::path path;
  bool is_folder = false;
  // Requests only, for the sidebar badge: kind, and the HTTP method ("GET", ...; empty for
  // gRPC). They come from a small mtime-validated index (.watt/index.json, also kept up to
  // date by save_request), so tree() does not open every request file. `badge` is false when
  // tree()'s time budget ran out before this file could be read: call tree() again (e.g. with
  // no budget on a worker thread) to fill it.
  bool badge = false;
  RequestKind kind = RequestKind::Http;
  std::string method;
  std::vector<CollectionNode> children;
};

class Workspace {
 public:
  static std::optional<Workspace> open(const std::filesystem::path& root, std::string* err = nullptr);
  static Workspace create(const std::filesystem::path& root);

  const std::filesystem::path& root() const { return root_; }
  // Scans root for folders and *.req.json. Badges of new or changed files are read from the
  // first KiB of each file for at most `badge_budget_ms` (< 0: no limit); the rest are left
  // with badge == false. Right after a git checkout an antivirus scan makes each first open
  // slow, which is why the budget exists: with a budget, reads also stop at the first one
  // slower than 2 ms, and none are made when there is no index yet (fresh clone). The
  // environments/ folder at the root is not listed.
  CollectionNode tree(double badge_budget_ms = 25) const;

  bool save_request(SavedRequest& r, std::string* err = nullptr);  // assigns file if empty
  std::optional<SavedRequest> load_request(const std::filesystem::path& file, std::string* err = nullptr) const;
  bool remove(const std::filesystem::path& p);
  bool rename(const std::filesystem::path& from, const std::string& new_name);

  std::vector<Environment>& environments() { return envs_; }
  std::string active_env;        // saved in .watt/state.json: switching never dirties git
  std::map<std::string, std::string> globals;
  std::map<std::string, std::string> dotenv;  // <ws>/.env, read by open()
  bool save_settings(std::string* err = nullptr) const;

  void append_history(const HistoryEntry& e);
  // Newest first, list fields only (when, kind, summary, status, total_ms, offset).
  std::vector<HistoryEntry> history(size_t max = 500) const;
  // The full request of an entry returned by history(). nullopt if the file changed under it
  // (history was trimmed): list again.
  std::optional<SavedRequest> history_request(const HistoryEntry& e) const;

  // {{var}} substitution using active env then globals. Unknown vars are left as-is.
  std::string expand(const std::string& s) const;
  SavedRequest expanded(const SavedRequest& r) const;
  // What to hand to HttpClient::send(): expanded(r).http, plus the multipart body built from
  // r.form (files read now, relative paths against the workspace root only). nullopt if a form
  // file cannot be read, or if a form file or body_file is not allowed (see blocked_files()):
  // then *err names the path and the request must not be sent.
  std::optional<HttpRequest> http_to_send(const SavedRequest& r, std::string* err = nullptr) const;

  // Files a request uploads (form file parts, body_file) must be inside the workspace root once
  // {{vars}} are expanded and symlinks resolved (fs::weakly_canonical), and not in its personal
  // .watt/, .git/ or .env. Anything else is sent only if the user allowed it for this workspace
  // in the per-user allowlist <user_config_dir()>/allowed_files.json ({version, workspaces:
  // {resolved root: [resolved files]}}). Nothing under the workspace is trusted for this (a
  // <ws>/.watt/allowed_files.json is ignored), so a cloned workspace cannot make Send upload
  // e.g. ~/.ssh/id_rsa.
  // The files `r` would read on send that are not allowed, as resolved absolute paths: before
  // sending, the UI asks "this request wants to upload <path> from outside the workspace: allow?"
  // and calls allow_file() for each one the user accepts.
  std::vector<std::filesystem::path> blocked_files(const SavedRequest& r) const;
  // Whether `file` (relative paths against the root) may be uploaded: inside the workspace, or
  // allowlisted.
  bool is_file_allowed(const std::filesystem::path& file) const;
  // Adds / removes `file` (resolved as above) to / from the allowlist. allow_file() of a file
  // that needs no entry (inside the workspace) is a no-op that returns true.
  bool allow_file(const std::filesystem::path& file, std::string* err = nullptr) const;
  bool disallow_file(const std::filesystem::path& file, std::string* err = nullptr) const;
  // This workspace's allowlist entries (resolved absolute paths).
  std::vector<std::filesystem::path> allowed_files() const;
  // Where per-user watt settings live: %APPDATA%\watt, $XDG_CONFIG_HOME/watt or
  // ~/.config/watt. set_user_config_dir() overrides it for the process (tests; empty path:
  // back to the default).
  static std::filesystem::path user_config_dir();
  static void set_user_config_dir(const std::filesystem::path& dir);

 private:
  void note_saved(const std::filesystem::path& file, const SavedRequest& r) const;
  bool write_allowlist(const std::vector<std::filesystem::path>& files, std::string* err) const;

  // Sealed secret values as read or last written: "env\0var" -> {plain, stored}. A value that
  // did not change is written back with the same ciphertext (DPAPI output is randomized), so a
  // no-op save_settings() leaves secrets.json untouched.
  mutable std::map<std::string, std::pair<std::string, std::string>> sealed_;

  std::filesystem::path root_;
  std::vector<Environment> envs_;
  // tree() badge cache, shared by copies; written by tree() and when the last copy goes away.
  mutable std::shared_ptr<detail::TreeIndex> tree_index_;
  detail::TreeIndex& tree_index() const;
  // history segment files, opened once per process and root (see model_history_file.h)
  mutable std::shared_ptr<detail::HistoryHandles> hist_;
  detail::HistoryHandles& history_handles() const;
};

// Header value convention (matches libcurl, which the HTTP engine hands headers to):
//   ""            sends the header with an empty value      (curl -H 'Name;')
//   kRemoveHeader suppresses a header the engine would add (curl -H 'Name:'), e.g. Accept.
// A request imported from `curl -d ''` carries an explicit "Content-Length: 0" header so the
// empty body survives (HttpRequest has no separate has-body flag).
inline constexpr char kRemoveHeader[] = " ";

// Import/export helpers that make switching from other tools cheap.
//
// parse_curl() is the import: the request, its -F / --form-string parts (file parts as path
// references, see model_form.h) and warnings for the import UI where curl itself would send
// something other than what the command seems to say (e.g. -b next to -H 'Cookie: ...', -G
// next to --url-query, a form file that does not exist).
struct CurlImport {
  HttpRequest http;               // with form parts, http.body is empty
  std::vector<FormPart> form;
  BasicAuth basic_auth;           // -u user:password (not also in http.headers)
  // -T file: the file as given. http.body holds its content as read now; a SavedRequest keeps
  // the reference (SavedRequest::body_file) and reads the file at send time.
  std::string body_file;
  std::vector<std::string> warnings;
  // Browser "Copy as cURL (cmd)" doubles every backslash, but curl.exe (MS CRT argv rules)
  // halves them only before a '"'. This is how many more backslashes curl.exe sends than the
  // browser did (0: no difference). When > 0 a warning is added and the UI can offer
  // CmdBackslashes::AsBrowserMeant.
  size_t browser_doubled_backslashes = 0;
};
// How backslashes in a cmd.exe command are read. AsCurlExe (default): exactly the bytes curl.exe
// gets. AsBrowserMeant: for a browser's cmd export, each "\\" not before a quote counts as one
// "\" (what the browser sent); a lone backslash is always kept.
enum class CmdBackslashes { AsCurlExe, AsBrowserMeant };
std::optional<CurlImport> parse_curl(const std::string& cmd, std::string* err = nullptr,
                                     CmdBackslashes bs = CmdBackslashes::AsCurlExe);
// parse_curl() with the form already built into http.body (files read relative to the current
// directory): ready to send, but the parts are flattened. Prefer parse_curl() for saving.
std::optional<HttpRequest> parse_curl_command(const std::string& cmd, std::string* err = nullptr);
// Shell the exported command is written for: bash ('...' quoting, \ continuations), cmd.exe
// ("..." with ^ continuations) or PowerShell (curl.exe, '...' quoting, ` continuations).
enum class CurlShell { Bash, Cmd, PowerShell };
std::string to_curl_command(const HttpRequest& r, CurlShell shell = CurlShell::Bash);
// With form parts: each one becomes -F / --form-string (with ;type= and ;filename=) and
// r.body is not exported.
std::string to_curl_command(const HttpRequest& r, const std::vector<FormPart>& form,
                            CurlShell shell = CurlShell::Bash);
// With basic auth: exported as -u 'user:password'.
std::string to_curl_command(const HttpRequest& r, const std::vector<FormPart>& form, const BasicAuth& auth,
                            CurlShell shell = CurlShell::Bash);
// The whole saved request: with body_file set, the body is exported as -T file.
std::string to_curl_command(const SavedRequest& r, CurlShell shell = CurlShell::Bash);
// Why `shell` cannot carry this request exactly (empty if it can): cmd.exe has no way to pass
// a CR or a NUL and garbles non-ASCII text, Windows PowerShell 5.1 cannot pass a NUL, and no
// shell can pass a NUL in an argument. The UI warns and suggests another shell.
std::string curl_export_problem(const HttpRequest& r, const std::vector<FormPart>& form, CurlShell shell);
std::string to_grpcurl_command(const SavedRequest& r);

}  // namespace watt
