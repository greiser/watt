#pragma once
// Folder / collection inheritance as the HTTP editor shows and edits it (no wx).
//   - InheritView: what a request whose auth is "Inherit from parent" gets from its folders and
//     the collection (Workspace::effective_settings), condensed for the Auth tab ("Inherit from
//     api/users: Bearer token abc123...f9e0") and the Auto headers list (each inherited header
//     with the folder it comes from).
//   - FolderForm: the folder settings dialog's auth choice over core's FolderSettings, which has
//     no Bearer / API key type: those are kept as the folder's Authorization / API key header
//     (sent with every request below that does not set the same header), as Bruno and Postman
//     send a folder's bearer token.
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/model.h"

namespace watt::app::http_editor {

// A secret shown on screen: "{{var}}" as written, a short literal as dots, a long one cut to its
// first 6 and last 4 characters.
std::string mask_secret(std::string_view v);

struct InheritView {
  bool known = false;                  // filled from effective_settings (else: still loading)
  std::string source;                  // "Collection", "api/users"; "" when no level sets auth
  std::filesystem::path source_folder;  // the folder whose auth is used (empty: none)
  std::string type;                    // "Bearer token", "Basic auth", "No auth", "OAuth 2.0", ...
  std::string detail;                  // "token abc123...f9e0", "user alice", "header X-API-Key"
  struct Header {
    std::string name, value, from;
    bool auth = false;  // the header carries the inherited auth (Bearer / API key)
  };
  std::vector<Header> headers;         // inherited, enabled, not replaced by a nearer folder
  std::vector<std::string> errors;     // folder.json files that could not be used
  // Folder / collection variables, nearest level first (name, value as written), enabled only.
  std::vector<std::pair<std::string, std::string>> vars;
  std::string network;  // network_summary() of what the levels above set
  // One line for the Auth tab: "Bearer token from api/users: token abc123...f9e0".
  std::string summary() const;
};
InheritView make_inherit_view(const EffectiveSettings& eff);
// One line naming the CA / client certificate / proxy that apply and where each comes from:
// "client certificate certs/client.pem (Collection) · proxy: Windows system proxy (api)".
// "" when everything is at its default.
std::string network_summary(const EffectiveSettings& e);

enum class FolderAuth { Inherit = 0, None, Bearer, Basic, ApiKey, OAuth2, Digest, Ntlm, Negotiate, AwsSigV4 };
inline constexpr const char* kFolderAuthLabels[] = {"Inherit from parent", "No auth", "Bearer token", "Basic auth",
                                                    "API key", "OAuth 2.0", "Digest", "NTLM",
                                                    "Negotiate (Kerberos)", "AWS Signature v4"};
inline constexpr int kFolderAuthCount = 10;

struct FolderForm {
  FolderAuth auth = FolderAuth::Inherit;
  std::string token;                    // Bearer
  std::string key_name = "X-API-Key";   // API key header
  std::string key_value;
  std::string user, pass;               // Basic
  HttpAuth adv;                         // OAuth 2.0 / Digest / NTLM / Negotiate / SigV4 (type set on apply)
  std::vector<SettingsRow> headers;     // the other headers
};
FolderForm folder_form(const FolderSettings& s);
// s's auth and headers from the form; vars, asserts and scripts are left as they are.
void apply_folder_form(const FolderForm& f, FolderSettings& s);

// Header rows <-> bulk text ("Name: value" per line, "//" disables, "# text" after the value is
// not special). Names and values are cleaned of control characters.
std::string rows_to_bulk(const std::vector<SettingsRow>& rows);
std::vector<SettingsRow> rows_from_bulk(std::string_view text);

}  // namespace watt::app::http_editor
