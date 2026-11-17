#pragma once
// Private, per-user storage for <ws>/.watt (internal to the workspace model).
//
// - <ws>/.watt is created owner-only: on Windows with a protected DACL that grants full control
//   to the current user and SYSTEM only (files created inside inherit it); on POSIX mode 0700, and
//   files inside are created 0600 with O_CLOEXEC.
// - Secret values in .watt/secrets.json are sealed: on Windows with DPAPI (current user scope,
//   CRYPTPROTECT_UI_FORBIDDEN, a watt-specific entropy), stored as "dpapi1:" + base64. Elsewhere
//   they stay plaintext (the 0600 file is the protection).
#include <filesystem>
#include <string>

namespace watt::detail {

// Whether p is inside a ".watt" directory (or is one): files there are personal.
bool is_private_path(const std::filesystem::path& p);
// Creates the single directory `dir` (its parent must exist) owner-only. True if it exists after.
bool create_private_dir(const std::filesystem::path& dir);

inline constexpr char kSealedPrefix[] = "dpapi1:";
// Windows: stored = "dpapi1:" + base64(DPAPI blob). Elsewhere: stored = plain. False on failure.
bool seal_secret(const std::string& plain, std::string& stored);
enum class Unseal {
  Plain,   // not sealed (legacy plaintext value, or a non-Windows file): plain = stored
  Ok,      // decrypted into plain
  Failed,  // sealed, but not for this user/machine (or corrupt): plain untouched
};
Unseal unseal_secret(const std::string& stored, std::string& plain);

}  // namespace watt::detail
