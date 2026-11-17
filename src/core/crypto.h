#pragma once
// Small native crypto helpers on Windows CNG (BCrypt): digests, HMAC, random bytes, UUIDs and the
// hex / base64 codecs that go with them. Used by request scripts (core/script.h: crypto.*,
// CryptoJS shim), OAuth 2.0 PKCE (core/oauth2.h) and tests. Algorithm providers are opened once
// per process, on first use. Thread-safe; nothing throws.
#include <cstddef>
#include <string>
#include <string_view>

namespace watt::crypto {

enum class Hash { Md5, Sha1, Sha256, Sha384, Sha512 };

// "md5", "sha1", "sha256", "sha384", "sha512" (case-insensitive, "SHA-256" style dashes allowed).
bool hash_from_name(std::string_view name, Hash& out);
size_t digest_size(Hash h);

// Raw digest bytes (empty only if CNG fails).
std::string digest(Hash h, std::string_view data);
// Raw HMAC bytes.
std::string hmac(Hash h, std::string_view key, std::string_view data);

// Cryptographically random bytes (BCryptGenRandom, system RNG).
bool random_bytes(void* out, size_t n);
std::string random_bytes(size_t n);
// RFC 4122 version 4 UUID, lower case: "1b4e28ba-2fa1-41d2-883f-0016d3cca427".
std::string uuid_v4();

std::string hex(std::string_view bytes);                       // lower case
bool unhex(std::string_view text, std::string& out);           // even length, [0-9a-fA-F] only
// RFC 4648 base64; url = the URL-safe alphabet (-_), pad = '=' padding.
std::string base64(std::string_view bytes, bool url = false, bool pad = true);
// Accepts both alphabets, optional padding, ignores ASCII whitespace; false on anything else.
bool unbase64(std::string_view text, std::string& out);

}  // namespace watt::crypto
