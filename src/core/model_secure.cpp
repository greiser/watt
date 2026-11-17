// Owner-only .watt directory and DPAPI-sealed secret values (see model_secure.h).
#include "core/model_secure.h"

#include <cstring>
#include <system_error>
#include <vector>

#include "core/model_json.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dpapi.h>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "crypt32.lib")
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace watt::detail {

namespace fs = std::filesystem;

bool is_private_path(const fs::path& p) {
  for (const auto& c : p)
    if (c == ".watt") return true;
  return false;
}

#ifdef _WIN32

bool create_private_dir(const fs::path& dir) {
  std::error_code ec;
  HANDLE tok = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
  DWORD n = 0;
  GetTokenInformation(tok, TokenUser, nullptr, 0, &n);
  std::vector<BYTE> tu(n ? n : 1);
  const BOOL got = GetTokenInformation(tok, TokenUser, tu.data(), n, &n);
  CloseHandle(tok);
  if (!got) return false;
  PSID user = reinterpret_cast<TOKEN_USER*>(tu.data())->User.Sid;
  BYTE sys_buf[SECURITY_MAX_SID_SIZE];
  DWORD sys_size = sizeof sys_buf;
  if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, sys_buf, &sys_size)) return false;
  PSID sys = sys_buf;

  const DWORD acl_size = sizeof(ACL) + 2 * (sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD)) + GetLengthSid(user) +
                         GetLengthSid(sys);
  std::vector<BYTE> acl_buf(acl_size);
  PACL acl = reinterpret_cast<PACL>(acl_buf.data());
  const DWORD inherit = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
  if (!InitializeAcl(acl, acl_size, ACL_REVISION) ||
      !AddAccessAllowedAceEx(acl, ACL_REVISION, inherit, FILE_ALL_ACCESS, user) ||
      !AddAccessAllowedAceEx(acl, ACL_REVISION, inherit, FILE_ALL_ACCESS, sys))
    return false;
  SECURITY_DESCRIPTOR sd;
  // Protected: the parent's inheritable ACEs (Users, Authenticated Users, ...) are not merged in.
  if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) ||
      !SetSecurityDescriptorOwner(&sd, user, FALSE) || !SetSecurityDescriptorDacl(&sd, TRUE, acl, FALSE) ||
      !SetSecurityDescriptorControl(&sd, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
    return false;
  SECURITY_ATTRIBUTES sa{sizeof sa, &sd, FALSE};
  if (CreateDirectoryW(dir.c_str(), &sa)) return true;
  if (GetLastError() == ERROR_INVALID_OWNER) {  // owner not assignable here: keep the default owner
    SetSecurityDescriptorOwner(&sd, nullptr, FALSE);
    if (CreateDirectoryW(dir.c_str(), &sa)) return true;
  }
  return fs::is_directory(dir, ec);
}

namespace {

// Ties the blobs to watt: another program running as the same user cannot unseal them by
// calling CryptUnprotectData without knowing this.
constexpr char kEntropy[] = "watt/.watt/secrets.json/v1:7f3c2a91-5d4e-4b8a-9e61-c0d2b7a4f815";

DATA_BLOB entropy_blob() {
  DATA_BLOB e;
  e.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy));
  e.cbData = static_cast<DWORD>(sizeof kEntropy - 1);
  return e;
}

}  // namespace

bool seal_secret(const std::string& plain, std::string& stored) {
  DATA_BLOB in;
  in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()));
  in.cbData = static_cast<DWORD>(plain.size());
  DATA_BLOB ent = entropy_blob();
  DATA_BLOB out{};
  if (!CryptProtectData(&in, L"watt secret", &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
  stored = kSealedPrefix + base64(std::string(reinterpret_cast<const char*>(out.pbData), out.cbData));
  SecureZeroMemory(out.pbData, out.cbData);
  LocalFree(out.pbData);
  return true;
}

Unseal unseal_secret(const std::string& stored, std::string& plain) {
  const size_t pre = sizeof kSealedPrefix - 1;
  if (stored.compare(0, pre, kSealedPrefix) != 0) {
    plain = stored;
    return Unseal::Plain;
  }
  std::string blob;
  if (!base64_decode(stored.substr(pre), blob) || blob.empty()) return Unseal::Failed;
  DATA_BLOB in;
  in.pbData = reinterpret_cast<BYTE*>(blob.data());
  in.cbData = static_cast<DWORD>(blob.size());
  DATA_BLOB ent = entropy_blob();
  DATA_BLOB out{};
  if (!CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return Unseal::Failed;
  plain.assign(reinterpret_cast<const char*>(out.pbData), out.cbData);
  SecureZeroMemory(out.pbData, out.cbData);
  LocalFree(out.pbData);
  return Unseal::Ok;
}

#else

bool create_private_dir(const fs::path& dir) {
  std::error_code ec;
  if (::mkdir(dir.c_str(), 0700) == 0) return true;
  return fs::is_directory(dir, ec);
}

bool seal_secret(const std::string& plain, std::string& stored) {
  stored = plain;  // no DPAPI here: the 0600 file in a 0700 directory is the protection
  return true;
}

Unseal unseal_secret(const std::string& stored, std::string& plain) {
  if (stored.compare(0, sizeof kSealedPrefix - 1, kSealedPrefix) == 0) return Unseal::Failed;
  plain = stored;
  return Unseal::Plain;
}

#endif

}  // namespace watt::detail
