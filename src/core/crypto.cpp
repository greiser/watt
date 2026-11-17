#include "core/crypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstdint>
#include <mutex>

#pragma comment(lib, "bcrypt.lib")

namespace watt::crypto {

namespace {

const wchar_t* alg_id(Hash h) {
  switch (h) {
    case Hash::Md5: return BCRYPT_MD5_ALGORITHM;
    case Hash::Sha1: return BCRYPT_SHA1_ALGORITHM;
    case Hash::Sha256: return BCRYPT_SHA256_ALGORITHM;
    case Hash::Sha384: return BCRYPT_SHA384_ALGORITHM;
    case Hash::Sha512: return BCRYPT_SHA512_ALGORITHM;
  }
  return BCRYPT_SHA256_ALGORITHM;
}

// Providers opened once per process: [hash][0 = plain, 1 = HMAC]. Opening one costs tens of
// microseconds; a cached handle is safe to use from several threads (CNG documents it).
struct Providers {
  std::array<std::array<BCRYPT_ALG_HANDLE, 2>, 5> h{};
  std::array<std::array<std::once_flag, 2>, 5> once;
};
Providers& providers() {
  static Providers p;
  return p;
}

BCRYPT_ALG_HANDLE provider(Hash h, bool mac) {
  auto& p = providers();
  const auto i = static_cast<size_t>(h);
  const size_t k = mac ? 1 : 0;
  std::call_once(p.once[i][k], [&] {
    BCRYPT_ALG_HANDLE a = nullptr;
    if (BCryptOpenAlgorithmProvider(&a, alg_id(h), nullptr, mac ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0) == 0)
      p.h[i][k] = a;
  });
  return p.h[i][k];
}

std::string run(Hash h, bool mac, std::string_view key, std::string_view data) {
  BCRYPT_ALG_HANDLE a = provider(h, mac);
  if (!a) return {};
  std::string out(digest_size(h), '\0');
  // BCryptHash: one-shot hash / HMAC (Windows 10+). An empty HMAC key is valid (RFC 2104).
  static const UCHAR kEmpty = 0;
  const NTSTATUS st = BCryptHash(
      a, mac ? (key.empty() ? const_cast<PUCHAR>(&kEmpty) : reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())))
             : nullptr,
      mac ? static_cast<ULONG>(key.size()) : 0,
      data.empty() ? const_cast<PUCHAR>(&kEmpty) : reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
      static_cast<ULONG>(data.size()), reinterpret_cast<PUCHAR>(out.data()), static_cast<ULONG>(out.size()));
  if (st != 0) return {};
  return out;
}

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

bool hash_from_name(std::string_view name, Hash& out) {
  std::string n;
  for (char c : name)
    if (c != '-' && c != '_') n.push_back(lower(c));
  if (n == "md5") out = Hash::Md5;
  else if (n == "sha1") out = Hash::Sha1;
  else if (n == "sha256") out = Hash::Sha256;
  else if (n == "sha384") out = Hash::Sha384;
  else if (n == "sha512") out = Hash::Sha512;
  else return false;
  return true;
}

size_t digest_size(Hash h) {
  switch (h) {
    case Hash::Md5: return 16;
    case Hash::Sha1: return 20;
    case Hash::Sha256: return 32;
    case Hash::Sha384: return 48;
    case Hash::Sha512: return 64;
  }
  return 32;
}

std::string digest(Hash h, std::string_view data) { return run(h, false, {}, data); }
std::string hmac(Hash h, std::string_view key, std::string_view data) { return run(h, true, key, data); }

bool random_bytes(void* out, size_t n) {
  if (n == 0) return true;
  if (n > 0xFFFFFFFFu) return false;
  return BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

std::string random_bytes(size_t n) {
  std::string s(n, '\0');
  if (!random_bytes(s.data(), n)) return {};
  return s;
}

std::string uuid_v4() {
  unsigned char b[16];
  if (!random_bytes(b, sizeof(b))) return {};
  b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40);
  b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80);
  static const char* hx = "0123456789abcdef";
  std::string s;
  s.reserve(36);
  for (int i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) s.push_back('-');
    s.push_back(hx[b[i] >> 4]);
    s.push_back(hx[b[i] & 15]);
  }
  return s;
}

std::string hex(std::string_view bytes) {
  static const char* hx = "0123456789abcdef";
  std::string s;
  s.reserve(bytes.size() * 2);
  for (char ch : bytes) {
    const auto c = static_cast<unsigned char>(ch);
    s.push_back(hx[c >> 4]);
    s.push_back(hx[c & 15]);
  }
  return s;
}

bool unhex(std::string_view text, std::string& out) {
  if (text.size() % 2) return false;
  std::string o;
  o.reserve(text.size() / 2);
  for (size_t i = 0; i < text.size(); i += 2) {
    const int a = hexval(text[i]), b = hexval(text[i + 1]);
    if (a < 0 || b < 0) return false;
    o.push_back(static_cast<char>(a * 16 + b));
  }
  out = std::move(o);
  return true;
}

std::string base64(std::string_view bytes, bool url, bool pad) {
  const char* tbl = url ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
                        : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  size_t i = 0;
  const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
  for (; i + 3 <= bytes.size(); i += 3) {
    const uint32_t v = (uint32_t{p[i]} << 16) | (uint32_t{p[i + 1]} << 8) | p[i + 2];
    out.push_back(tbl[(v >> 18) & 63]);
    out.push_back(tbl[(v >> 12) & 63]);
    out.push_back(tbl[(v >> 6) & 63]);
    out.push_back(tbl[v & 63]);
  }
  const size_t rest = bytes.size() - i;
  if (rest == 1) {
    const uint32_t v = uint32_t{p[i]} << 16;
    out.push_back(tbl[(v >> 18) & 63]);
    out.push_back(tbl[(v >> 12) & 63]);
    if (pad) out += "==";
  } else if (rest == 2) {
    const uint32_t v = (uint32_t{p[i]} << 16) | (uint32_t{p[i + 1]} << 8);
    out.push_back(tbl[(v >> 18) & 63]);
    out.push_back(tbl[(v >> 12) & 63]);
    out.push_back(tbl[(v >> 6) & 63]);
    if (pad) out.push_back('=');
  }
  return out;
}

bool unbase64(std::string_view text, std::string& out) {
  std::string o;
  o.reserve(text.size() / 4 * 3 + 3);
  uint32_t acc = 0;
  int bits = 0;
  bool padding = false;
  for (char c : text) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
    if (c == '=') {
      padding = true;
      continue;
    }
    if (padding) return false;  // data after '='
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+' || c == '-') v = 62;
    else if (c == '/' || c == '_') v = 63;
    else return false;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      o.push_back(static_cast<char>((acc >> bits) & 0xFF));
    }
  }
  out = std::move(o);
  return true;
}

}  // namespace watt::crypto
