#pragma once
// Workspace model. A workspace is a folder on disk (git-friendly, like Bruno):
//   <ws>/watt.json                   workspace settings (globals)
//   <ws>/environments/<name>.json      one file per environment (no merge conflicts between them)
//   <ws>/<folder>/.../<name>.req.json  one saved request per file
//   <ws>/folder.json                   collection settings (headers, auth, vars, scripts, asserts
//                                      inherited by every request; see FolderSettings)
//   <ws>/<folder>/.../folder.json      folder settings, same format, inherited by the requests below
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
// Variables: {{name}} substituted, first match wins, from (Bruno's order):
//   runtime vars > folder vars (nearest folder first) > active environment > collection vars
//   (<ws>/folder.json) > workspace globals > dynamic variables ({{$guid}}, {{process.env.X}} from
//   .env, or from the OS environment only for names the user allowed for this workspace, ...).
// Folder vars apply only where a request is known (expanded(r), http_to_send(r), expand(s, r),
// scripts); expand(s) alone uses runtime, environment, collection and globals.
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
struct FolderCache;
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
// Type::Inherit: use the auth of the nearest parent folder (folder.json) or the collection
// (<ws>/folder.json) whose auth is not Inherit; None (or Basic) at a level ends the walk there.
// A request saved without an auth scheme keeps None (no inheritance); Workspace::new_request()
// starts new requests with Inherit, as Postman, Insomnia and Bruno do.
struct HttpAuth {
  enum class Type { None, Digest, Ntlm, Negotiate, AwsSigV4, OAuth2, Inherit };
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
const char* auth_type_name(HttpAuth::Type t);  // "none", "digest", "ntlm", "negotiate", "awsv4", "oauth2", "inherit"
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
  // Set on the copy returned by Workspace::resolved() / expanded(): folder and collection
  // settings are merged in, the first inherited_headers entries of http.headers (HTTP) or
  // inherited_metadata entries of grpc_opts.metadata (gRPC) are inherited ones, and auth /
  // basic_auth are the resolved ones (never Inherit). Never saved; resolving again is a no-op.
  bool settings_merged = false;
  size_t inherited_headers = 0;
  size_t inherited_metadata = 0;
};

// One row of a folder's / the collection's headers or variables (Bruno's folder.bru headers /
// vars, Postman's collection variables). Disabled rows are kept but not sent / not defined.
struct SettingsRow {
  std::string name;
  std::string value;  // may hold {{vars}}
  std::string description;
  bool enabled = true;
  bool operator==(const SettingsRow&) const = default;
};

// Settings of a folder (<folder>/folder.json) or of the whole collection (<ws>/folder.json),
// inherited by every request below it (Bruno's folder.bru / collection.bru, Postman's
// collection and folder Authorization / Pre-request Script / Tests / Variables).
//   headers   sent with every request below (gRPC: as metadata); a request header (or a nearer
//             folder's) with the same name, case-insensitive, replaces it
//   auth      used by requests whose auth is Inherit: auth.type Inherit (the default) passes on to
//             the parent; None with basic_auth.enabled is Basic auth; None otherwise is "no auth"
//             (HTTP only: gRPC requests do not use auth)
//   vars      {{name}} values, see the precedence at the top of this file
//   scripts   pre: collection, then folders outermost first, then the request; post: request,
//             then folders nearest first, then the collection (core/script.h)
//   asserts   checked in addition to the request's own
// The file is written only while something is set (save_folder_settings() removes it when the
// settings are empty), so workspaces that do not use it never get one.
struct FolderSettings {
  std::vector<SettingsRow> headers;
  HttpAuth auth{HttpAuth::Type::Inherit};
  BasicAuth basic_auth;
  std::vector<SettingsRow> vars;
  std::vector<Assertion> asserts;
  std::string pre_script, post_script;
  bool empty() const { return *this == FolderSettings{}; }
  bool operator==(const FolderSettings&) const = default;
};

// Where an effective setting comes from (Workspace::effective_settings()).
struct SettingsOrigin {
  enum class Level { Collection, Folder, Request };
  Level level = Level::Request;
  std::filesystem::path folder;  // the folder (the workspace root for Collection); empty for Request
  std::string label;             // "Collection", the folder path relative to the root ("api/users"), "Request"
  bool operator==(const SettingsOrigin&) const = default;
};

// What a request gets from its folders and the collection, with the origin of each item, for
// the editor ("inherited from folder api/users") and for the send path.
struct EffectiveSettings {
  struct Row {
    SettingsRow row;
    SettingsOrigin from;
    bool overridden = false;  // replaced by a nearer level (same name): not sent / not used
  };
  struct Script {
    std::string code;
    SettingsOrigin from;
  };
  struct Check {
    Assertion assertion;
    SettingsOrigin from;
  };
  // Headers in send order: collection, folders outermost first, then the request's own (gRPC:
  // its metadata). Disabled rows are listed (row.enabled = false) and never sent.
  std::vector<Row> headers;
  // Variables, highest precedence first: runtime vars (level Request), folders nearest first,
  // the collection. (Environment and globals come between / after; see the top of this file.)
  std::vector<Row> vars;
  // The resolved auth (type never Inherit) and the level it was taken from.
  HttpAuth auth;
  BasicAuth basic_auth;
  SettingsOrigin auth_from;
  std::vector<Script> pre_scripts;   // run order
  std::vector<Script> post_scripts;  // run order
  std::vector<Check> asserts;        // the request's own, then collection, then folders outermost first
  // folder.json files that could not be used (unreadable, invalid JSON, outside the workspace):
  // "<path>: <why>". http_to_send() refuses to send while there is one.
  std::vector<std::string> errors;
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
  // A new request as the UI should create it: new id, auth Inherit (uses the folder's /
  // collection's auth, like Postman / Insomnia / Bruno), otherwise SavedRequest's defaults. With
  // `folder` (a directory inside the workspace), file = <folder>/<free name>.req.json, so
  // save_request() puts it there (and its folder settings apply before it is saved).
  SavedRequest new_request(RequestKind kind = RequestKind::Http, const std::string& name = "New Request",
                           const std::filesystem::path& folder = {}) const;

  // Folder / collection settings (see FolderSettings). `folder` is a directory inside the
  // workspace (relative paths against the root), or the root itself for the collection. Load:
  // empty settings when there is no folder.json; nullopt (and *err) when the folder is outside
  // the workspace or the file is not valid JSON. Save: writes <folder>/folder.json (pretty,
  // diff-friendly, unchanged content leaves the file untouched), or removes it when `s` is empty.
  // Files are cached: changes made outside watt (git pull, an editor) are seen after open() or
  // tree(), and otherwise at most 100 ms later.
  std::optional<FolderSettings> folder_settings(const std::filesystem::path& folder, std::string* err = nullptr) const;
  bool save_folder_settings(const std::filesystem::path& folder, const FolderSettings& s, std::string* err = nullptr) const;
  FolderSettings collection_settings() const;  // <ws>/folder.json ({} if none or invalid)
  bool save_collection_settings(const FolderSettings& s, std::string* err = nullptr) const {
    return save_folder_settings(root_, s, err);
  }
  // The settings r inherits (from the folders of r.file up to the root; only the collection for
  // an unsaved request), merged with r's own, each with its origin.
  EffectiveSettings effective_settings(const SavedRequest& r) const;
  // Whether any non-empty folder.json applies to r (cheap: one cached stat per level). A send
  // path that skips http_to_send() for plain requests must not skip it when this is true.
  bool has_inherited_settings(const SavedRequest& r) const;
  // r with its folders' and the collection's headers (gRPC: metadata), auth (when Inherit) and
  // asserts merged in, not expanded. expanded(), http_to_send(), blocked_*() and schema_plan()
  // resolve on their own; use this for evaluate_asserts() (inherited asserts) and for display.
  SavedRequest resolved(const SavedRequest& r) const;
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

  // {{var}} substitution using runtime_vars, then the active env, then the collection vars, then
  // globals. Unknown vars are left as-is. The second form adds r's folder vars (above the
  // environment, nearest folder first).
  std::string expand(const std::string& s) const;
  std::string expand(const std::string& s, const SavedRequest& r) const;
  // resolved(r) with every field expanded (folder vars included). Inherited headers / metadata
  // whose expanded name or value holds CR, LF or NUL are dropped.
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
  // Folder / collection settings are merged first (resolved()): inherited headers, auth, vars.
  // A header (own or inherited) whose name or value holds CR, LF or NUL after expansion, or a
  // folder.json that cannot be read (EffectiveSettings::errors), fails the send.
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
  struct FolderChain;
  FolderChain folder_chain(const SavedRequest& r) const;
  SavedRequest resolve_with(const SavedRequest& r, const FolderChain& c) const;
  SavedRequest expanded_with(const SavedRequest& r, const FolderChain& c) const;
  std::string expand_in(const std::string& s, const FolderChain* c) const;
  void note_saved(const std::filesystem::path& file, const SavedRequest& r) const;
  bool write_allowlist(const std::vector<std::filesystem::path>& files, std::string* err) const;
  std::vector<std::string> env_refs_left(const SavedRequest& expanded_request, const FolderChain* c) const;
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
  // folder.json cache (validated by one stat per file), shared by every Workspace of the root.
  mutable std::shared_ptr<detail::FolderCache> folder_cache_;
  detail::FolderCache& folder_cache() const;
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
