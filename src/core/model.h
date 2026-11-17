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
// files a workspace may upload from outside its root, see Workspace::blocked_files(), and
// <config>/allowed_env.json, OS environment variables it may expand, see blocked_env_vars().
// <config> = %APPDATA%\watt (POSIX: $XDG_CONFIG_HOME/watt or ~/.config/watt, 0700 / 0600).
// Variables: {{name}} substituted from the active environment, then workspace globals, then
// dynamic variables ({{$guid}}, {{process.env.X}} from .env, or from the OS environment only for
// names the user allowed for this workspace, ...).
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/asserts.h"
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

// OAuth 2.0 settings of a request (Postman's / Bruno's OAuth 2.0 auth). Every string may hold
// {{vars}}, so client secrets and passwords can live in secret environment variables (sealed with
// DPAPI in .watt/secrets.json) instead of the request file. Tokens are fetched and cached by
// core/oauth2.h (in memory, per workspace, environment, request and settings, with expiry);
// Workspace::http_to_send() adds the cached token, refreshing or fetching it first for the
// non-interactive grants.
struct OAuth2Config {
  enum class Grant { ClientCredentials, Password, AuthorizationCode };
  Grant grant = Grant::ClientCredentials;
  std::string token_url;      // http(s) token endpoint
  std::string auth_url;       // authorization endpoint (AuthorizationCode)
  std::string client_id;
  std::string client_secret;  // may be empty (public client with PKCE)
  std::string scope;
  std::string audience;       // optional "audience" parameter (Auth0 and others)
  std::string username;       // Password grant
  std::string password;
  // AuthorizationCode: where the browser is sent back to. Empty: http://127.0.0.1:<free port>/callback.
  // Only loopback http URLs (127.0.0.1, [::1], localhost) can be received; the port may be fixed.
  std::string redirect_uri;
  bool pkce = true;  // AuthorizationCode: S256 code challenge (RFC 7636)
  // How the client authenticates to the token endpoint: HTTP Basic header or form fields.
  enum class ClientAuth { BasicHeader, Body };
  ClientAuth client_auth = ClientAuth::BasicHeader;
  // Where the access token goes: "Authorization: <header_prefix> <token>" or ?<query_param>=<token>.
  enum class AddTo { Header, Query };
  AddTo add_to = AddTo::Header;
  std::string header_prefix = "Bearer";
  std::string query_param = "access_token";
  bool auto_refresh = true;  // use the refresh token when the access token expired
  bool operator==(const OAuth2Config&) const = default;
};

// Authentication schemes beyond Basic (which stays in SavedRequest::basic_auth). When type is not
// None it replaces basic_auth. Strings may hold {{vars}} (see OAuth2Config).
struct HttpAuth {
  enum class Type { None, Digest, Ntlm, Negotiate, AwsSigV4, OAuth2 };
  Type type = Type::None;
  // Digest / NTLM / Negotiate. NTLM and Negotiate with an empty user use the current Windows logon.
  std::string user;
  std::string password;
  // AWS Signature Version 4 (curl --aws-sigv4 "aws:amz:<region>:<service>").
  std::string aws_access_key;
  std::string aws_secret_key;
  std::string aws_session_token;  // optional: sent as X-Amz-Security-Token (signed)
  std::string aws_region;         // "eu-west-1"
  std::string aws_service;        // "execute-api", "s3", ...
  OAuth2Config oauth2;
  bool operator==(const HttpAuth&) const = default;
};
const char* auth_type_name(HttpAuth::Type t);  // "none", "digest", "ntlm", "negotiate", "awsv4", "oauth2"
HttpAuth::Type auth_type_from(const std::string& s);

// One curl -d family argument kept as written because it reads a file (-d @f, --data-binary @f,
// --json @f, --data-urlencode name@f): nothing is read when a command is imported, the file is
// read when the request is sent (Workspace::http_to_send(), under the upload policy of
// blocked_files()) and processed as curl does (-d strips CR and LF, --data-urlencode encodes).
// A request keeps all its -d family arguments this way, in order, when any of them is a file.
struct CurlData {
  enum class Kind {
    Data,       // -d / --data / --data-ascii: "@file" = the file without CR and LF, else the text
    Binary,     // --data-binary: "@file" = the file as it is
    Raw,        // --data-raw: the text, never a file
    UrlEncode,  // --data-urlencode: content | =content | name=content | @file | name@file
    Json,       // --json: "@file" = the file as it is; concatenated, not joined with '&'
  };
  Kind kind = Kind::Data;
  std::string value;  // as written ({{vars}} allowed; relative paths against the workspace root)
  bool operator==(const CurlData&) const = default;
};

struct SavedRequest {
  std::string id;                 // stable uuid
  std::string name;
  // Free-form notes about the request (Bruno's / Insomnia's Docs tab), Markdown by convention.
  std::string docs;
  std::filesystem::path file;     // absolute path of the .req.json (empty if unsaved)
  RequestKind kind = RequestKind::Http;
  // HTTP
  HttpRequest http;
  // Indexes into http.headers of headers kept but not sent (an unchecked row, like Bruno's
  // ~Header or Postman's disabled header): saved in place in the .req.json, left out by
  // http_to_send() and to_curl_command(SavedRequest).
  std::set<size_t> disabled_headers;
  // Query params kept but not sent (an unchecked row in the Params table, like Postman's disabled
  // param or Bruno's ~param): decoded key / value as shown. http.url holds only the sent ones, so
  // nothing on the send path reads these.
  std::vector<std::pair<std::string, std::string>> disabled_params;
  // Path variables (Postman's Path Variables, Insomnia / Bruno's path params): the value of each
  // "/:name" segment of http.url, which keeps the ":name" template. The editor substitutes the
  // URL-encoded values when it sends or exports the request.
  std::vector<std::pair<std::string, std::string>> path_params;
  // multipart/form-data parts. When not empty they replace http.body: the body is built from
  // them (files read) at send time, see Workspace::http_to_send(). Stored in the .req.json with
  // file parts as path references.
  std::vector<FormPart> form;
  BasicAuth basic_auth;
  // A file sent as the raw body (curl -T), read at send time like form files (relative paths
  // against the workspace root; may hold {{vars}}). When set, http.body is not saved.
  std::string body_file;
  // curl -d family arguments read at send time (see CurlData). When not empty (and there is no
  // form and no body_file) the body is built from them and http.body is not saved.
  std::vector<CurlData> body_data;
  // curl -H @file: header lines read from these files at send time (a header from a file
  // replaces a header of the same name). curl -b file: Netscape cookie jars read at send time;
  // their cookies for the URL go first in the Cookie header. Same file policy as body_file.
  std::vector<std::string> header_files;
  std::vector<std::string> cookie_files;
  // gRPC
  GrpcTarget grpc_target;
  std::string grpc_method;        // full path "/pkg.Svc/Method"
  std::string grpc_body;          // JSON
  GrpcCallOptions grpc_opts;
  std::vector<std::string> proto_files;   // empty => use reflection
  std::vector<std::string> import_dirs;
  // Response checks (Bruno's Assert tab / Postman's Tests) and post-response captures (Bruno's
  // vars:post-response), both kinds of request; see core/asserts.h. Saved only when not empty.
  std::vector<Assertion> asserts;
  std::vector<Capture> captures;
  // Digest / NTLM / Negotiate / AWS SigV4 / OAuth 2.0 (HTTP only); saved as "http.auth_scheme"
  // only when type is not None.
  HttpAuth auth;
  // JavaScript run before the request is sent / after the response arrived (Postman's
  // Pre-request Script and Tests, Bruno's script:pre-request / script:post-response), in a
  // sandbox; see core/script.h. They run only once the user trusted this workspace's scripts
  // (Workspace::scripts_trusted()). Saved last, only when not empty.
  std::string pre_script, post_script;
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
  // Runtime variables (Bruno's bru.setVar): set by Capture::Runtime captures (apply_captures),
  // looked up by expand() before the active environment, never saved.
  std::map<std::string, std::string> runtime_vars;
  bool save_settings(std::string* err = nullptr) const;

  void append_history(const HistoryEntry& e);
  // Newest first, list fields only (when, kind, summary, status, total_ms, offset).
  std::vector<HistoryEntry> history(size_t max = 500) const;
  // The full request of an entry returned by history(). nullopt if the file changed under it
  // (history was trimmed): list again.
  std::optional<SavedRequest> history_request(const HistoryEntry& e) const;

  // {{var}} substitution using runtime_vars, then the active env, then globals. Unknown vars are
  // left as-is.
  std::string expand(const std::string& s) const;
  SavedRequest expanded(const SavedRequest& r) const;
  // What to hand to HttpClient::send(): expanded(r).http, plus the multipart body built from
  // r.form, body_file or body_data, and the headers / cookies of header_files / cookie_files
  // (files read now, relative paths against the workspace root only). nullopt if one of these
  // files cannot be read or is not allowed (see blocked_files()): then *err names the path and
  // the request must not be sent.
  // Authentication: SavedRequest::auth (when not None) replaces basic_auth: Digest / NTLM /
  // Negotiate / AWS SigV4 become HttpRequest auth options (libcurl does the handshake / signing),
  // OAuth 2.0 adds the cached access token (header or query), first fetching or refreshing it for
  // the client credentials / password grants (a blocking token request: call this off the UI
  // thread). An authorization code grant is never started here (it opens a browser): without a
  // cached or refreshable token the send fails with *err saying to authorize first (oauth2.h).
  std::optional<HttpRequest> http_to_send(const SavedRequest& r, std::string* err = nullptr) const;

  // Files a request uploads (form file parts, body_file, body_data, header_files, cookie_files)
  // must be inside the workspace root once
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
  // Proto files / import dirs of a gRPC request outside the workspace (UNC paths always count):
  // the schema must not be loaded until each is approved with allow_file(). Paths are judged
  // from their text and never touched before approval (no SMB session to a planted host).
  std::vector<std::filesystem::path> blocked_schema_paths(const SavedRequest& r) const;
  // What a schema load may read, as absolute paths anchored at the workspace root (relative
  // proto names that are not files under the root stay virtual names, looked up only in the
  // import dirs). Pass these to GrpcClient::load_proto_files, never the raw request strings:
  // the loader would resolve those against the process cwd. Load only if blocked and invalid
  // are both empty (after the user approved each blocked path).
  struct SchemaPlan {
    std::vector<std::string> proto_files;
    std::vector<std::string> import_dirs;
    std::vector<std::filesystem::path> blocked;  // outside the workspace, not yet allowed
    std::vector<std::string> invalid;            // refused forms ("\foo", "C:foo", "../x" names)
  };
  SchemaPlan schema_plan(const SavedRequest& r) const;
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
  // {{process.env.X}} / {{$env.X}} take X from <ws>/.env ({{process.env.X}} only; meant for this
  // workspace's requests), else from the OS environment, but only if the user allowed X for this
  // workspace in the per-user <user_config_dir()>/allowed_env.json ({version, workspaces:
  // {resolved root: [names]}}; never read from the workspace, so a cloned one cannot make Send
  // leak e.g. AWS_SECRET_ACCESS_KEY). Otherwise the placeholder is left as-is.
  // The OS environment variables `r` references (after expansion) that are set but not allowed:
  // before sending, the UI asks "this request wants to read the environment variable X: allow?"
  // and calls allow_env_var() for each one accepted. http_to_send() refuses to send while any is
  // left, naming it.
  std::vector<std::string> blocked_env_vars(const SavedRequest& r) const;
  bool is_env_var_allowed(const std::string& name) const;
  bool allow_env_var(const std::string& name, std::string* err = nullptr) const;
  bool disallow_env_var(const std::string& name, std::string* err = nullptr) const;
  std::vector<std::string> allowed_env_vars() const;
  // Single sign-on: NTLM / Negotiate auth with an empty user name signs in with the current
  // Windows logon (SSPI). A cloned workspace could point such a request at its own host and
  // collect an NTLM challenge-response for the user, so implicit credentials go only to origins
  // ("scheme://host:port", lower case, default port filled in) the user allowed for this
  // workspace, in the per-user <user_config_dir()>/allowed_sso.json ({version, workspaces:
  // {resolved root: [origins]}}; never read from the workspace). Explicit user + password needs
  // no approval. The origins `r` would send implicit credentials to (after {{var}} expansion)
  // that are not allowed: before sending, the UI asks "this request wants to sign in to <origin>
  // with your Windows account: allow?" and calls allow_sso_host() for each one accepted.
  // http_to_send() refuses to send while any is left, naming it. libcurl never sends credentials
  // to a redirect target on another host (CURLOPT_UNRESTRICTED_AUTH stays off).
  std::vector<std::string> blocked_sso_hosts(const SavedRequest& r) const;
  bool is_sso_host_allowed(const std::string& origin_or_url) const;
  bool allow_sso_host(const std::string& origin_or_url, std::string* err = nullptr) const;
  bool disallow_sso_host(const std::string& origin_or_url, std::string* err = nullptr) const;
  std::vector<std::string> allowed_sso_hosts() const;
  // "scheme://host:port" of an http(s) URL (or of an origin), lower case with the default port;
  // "" if it is not one.
  static std::string url_origin(const std::string& url);
  // Request scripts (SavedRequest::pre_script / post_script) are code from whoever wrote the
  // workspace, often a cloned repository: they run only after the user trusted this workspace.
  // The decision is stored per user, outside every workspace, in
  // <user_config_dir()>/trusted_scripts.json ({version, workspaces: {resolved root: ["scripts"]}}),
  // keyed by the canonical root like the allowlists above: nothing inside a workspace (a planted
  // .watt/trusted_scripts.json or a watt.json key) can trust it. Until trusted, the run
  // functions in core/script.h return without running and say why (the UI asks once).
  bool scripts_trusted() const;
  bool trust_scripts(bool trusted, std::string* err = nullptr) const;
  static std::filesystem::path user_config_dir();
  static void set_user_config_dir(const std::filesystem::path& dir);

 private:
  void note_saved(const std::filesystem::path& file, const SavedRequest& r) const;
  bool write_allowlist(const std::vector<std::filesystem::path>& files, std::string* err) const;
  std::vector<std::string> env_refs_left(const SavedRequest& expanded_request) const;
  bool apply_http_auth(const SavedRequest& r, SavedRequest& expanded_request, std::string* err) const;

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
// No local file is ever read by the import (a pasted `curl -d @~/.ssh/id_rsa ...` must not copy
// the key into a request that may be committed): every file the command names is kept as a
// reference (body_file, body_data, header_files, cookie_files, form file parts) and read at send
// time by Workspace::http_to_send(), where blocked_files() asks the user about files outside the
// workspace. Only data piped into the command itself (@- after `printf ... |`) is inlined.
struct CurlImport {
  HttpRequest http;               // with form parts, http.body is empty
  std::vector<FormPart> form;
  BasicAuth basic_auth;           // -u user:password (not also in http.headers)
  // -T file, or a single --data-binary @file: the file as given (http.body is empty); a
  // SavedRequest keeps the reference (SavedRequest::body_file) and reads the file at send time.
  std::string body_file;
  // The -d family arguments when any of them reads a file (http.body is empty then); see
  // SavedRequest::body_data. -H @file and -b file references, see SavedRequest.
  std::vector<CurlData> body_data;
  std::vector<std::string> header_files;
  std::vector<std::string> cookie_files;
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
// parse_curl() with the form already built into http.body and basic auth as a header. It reads
// no file either: file form parts are sent empty and body_data / header_files / cookie_files /
// body_file are left out. Prefer parse_curl() (and Workspace::http_to_send()).
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
// The whole saved request: with body_file set, the body is exported as -T file (or
// --data-binary @file); body_data, header_files and cookie_files as the -d family / -H @file /
// -b file arguments they came from.
std::string to_curl_command(const SavedRequest& r, CurlShell shell = CurlShell::Bash);
// Why `shell` cannot carry this request exactly (empty if it can): cmd.exe has no way to pass
// a CR or a NUL and garbles non-ASCII text, Windows PowerShell 5.1 cannot pass a NUL, and no
// shell can pass a NUL in an argument. The UI warns and suggests another shell.
std::string curl_export_problem(const HttpRequest& r, const std::vector<FormPart>& form, CurlShell shell);
std::string to_grpcurl_command(const SavedRequest& r);

// Send-time helpers for curl file references (used by Workspace::http_to_send()).
// The body curl sends for these -d family arguments; `read(path, out)` reads a file reference
// (false: *err names it). curl's rules: --json values are concatenated (then the first other
// value is appended), the rest joined with '&'.
bool build_curl_data_body(const std::vector<CurlData>& data,
                          const std::function<bool(const std::string& path, std::string& out)>& read,
                          std::string& body);
// The file a -d family argument reads ("" if none).
std::string curl_data_file(const CurlData& d);
// Adds the header lines of a curl -H @file (one header per line, blank lines skipped, curl's
// "Name:" / "Name;" conventions). A header replaces earlier headers of the same name.
void add_curl_header_file(std::vector<Header>& headers, const std::string& content);
// The cookies of a Netscape cookie file (curl -b file) that curl sends to `url`, "a=1; b=2".
std::string curl_jar_cookies(const std::string& jar, const std::string& url);

}  // namespace watt
