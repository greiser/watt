#include "core/http_client.h"

// libcurl-backed REST engine.
//
// Design notes (performance first):
//  * One persistent easy handle per client, driven through a private multi handle so that
//    cancel() can wake the transfer loop instantly (curl_multi_wakeup) instead of waiting for
//    curl's ~1s idle progress tick. The atomic flag is also checked in the xferinfo callback.
//  * The easy handle is never curl_easy_reset(): options that never change (share, callbacks,
//    buffers, socket options) are set once when the client is built, and send() sets every
//    per-request option to an explicit value. A reset plus re-applying everything cost about
//    10 us per warm request; an unchanged header list is reused as is.
//  * A share handle holds DNS, TLS session and connection caches, so re-firing a request skips
//    DNS/TCP/TLS entirely.
//  * The response body is appended straight from curl's receive buffer into
//    HttpResponse::body (reserved from Content-Length): no intermediate copies.
//  * Loopback connects never retransmit SYNs (SIO_TCP_INITIAL_RTO). Windows retries a SYN to a
//    refused loopback port instead of failing at once, so "localhost" against an IPv4-only dev
//    server used to sit on ::1 until curl's 200 ms happy-eyeballs timer fired. Real networks
//    keep the normal SYN retry behaviour.
//  * Busy-poll (HttpRequest::busy_poll_ms): for the first 20 ms of a transfer the loop calls
//    curl_multi_perform again after a few pause instructions instead of sleeping in
//    curl_multi_poll. A thread that sleeps through the server wait wakes on an idle,
//    down-clocked core, and that wake-up plus everything after the last byte ran ~3x slower:
//    in-process A/B against a bare libcurl loop showed no option, callback or buffer size
//    made a measurable difference, only that.
//  * libcurl is pinned in the repo (ports/curl, 8.22, linked statically; see build.ps1): 8.22
//    skips the per-reuse connection liveness poll when the connection was checked < 1 s ago.
//  * Timing comes from curl's per-hop microsecond marks plus our own steady_clock marks
//    around the transfer; see the end of send() for how they map to phases.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <timeapi.h>
#include <wincrypt.h>
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <security.h>
#include <schannel.h>
#include <ncrypt.h>
#include <winhttp.h>

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <string_view>

#include "core/loopback_connect.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ncrypt.lib")
#pragma comment(lib, "winhttp.lib")

namespace watt {

namespace {

struct CurlGlobal {
  CurlGlobal() {
    // libcurl 8.20 + Schannel: schannel_init() asks "Windows >= 8.1?" (the ALPN gate) before
    // curl's win32 init has loaded RtlVerifyVersionInfo, so it falls back to VerifyVersionInfo,
    // which reports Windows 8 to any exe without a supportedOS manifest. ALPN then stays off for
    // the whole process and HTTP/2 over TLS is never offered. A second init, after a cleanup,
    // re-runs schannel_init with the pointer already loaded (it is never cleared). The second
    // init costs about 0.35 ms, so it is skipped when VerifyVersionInfo already tells the truth
    // (the exe declares Windows 8.1+ in its manifest, as the GUI does): curl's fallback then
    // reaches the same answer on the first init.
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (!version_info_reports_81()) {
      curl_global_cleanup();
      curl_global_init(CURL_GLOBAL_DEFAULT);
    }
  }
  static bool version_info_reports_81() {
    OSVERSIONINFOEXW v{};
    v.dwOSVersionInfoSize = sizeof(v);
    v.dwMajorVersion = 6;
    v.dwMinorVersion = 3;
    DWORDLONG mask = 0;
    mask = VerSetConditionMask(mask, VER_MAJORVERSION, VER_GREATER_EQUAL);
    mask = VerSetConditionMask(mask, VER_MINORVERSION, VER_GREATER_EQUAL);
    return VerifyVersionInfoW(&v, VER_MAJORVERSION | VER_MINORVERSION, mask) != FALSE;
  }
  ~CurlGlobal() { curl_global_cleanup(); }
};

void ensure_global_init() { static CurlGlobal g; }

// Spare multi handles. curl_multi_init costs about 0.5 ms (its wakeup socket pair is a
// loopback listen/connect/accept), which dwarfs a warm request, so HttpClient takes one from
// here and gives it back when destroyed. A multi with no easy handle attached holds no
// connections (those live in each client's share handle), so a recycled one carries no state
// into the next client. Destroyed at exit before CurlGlobal (constructed after it).
class MultiPool {
 public:
  static MultiPool& get() {
    ensure_global_init();
    static MultiPool p;
    return p;
  }
  CURLM* take() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (!spare_.empty()) {
        CURLM* m = spare_.back();
        spare_.pop_back();
        return m;
      }
    }
    return curl_multi_init();
  }
  void give(CURLM* m) {
    if (!m) return;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (spare_.size() < kMax) {
        spare_.push_back(m);
        return;
      }
    }
    curl_multi_cleanup(m);
  }
  void prefill(size_t n) {
    for (;;) {
      {
        std::lock_guard<std::mutex> lk(mu_);
        if (spare_.size() >= n) return;
      }
      CURLM* m = curl_multi_init();  // outside the lock: it is the slow part
      if (!m) return;
      give(m);
    }
  }
  ~MultiPool() {
    for (CURLM* m : spare_) curl_multi_cleanup(m);
  }

 private:
  static constexpr size_t kMax = 8;
  std::mutex mu_;
  std::vector<CURLM*> spare_;
};

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() &&
         (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
    s.remove_suffix(1);
  return s;
}

// Upper bound for reserving from Content-Length, so a lying server cannot make us allocate
// absurd amounts up front. Larger bodies still work; they just grow geometrically.
constexpr size_t kMaxReserve = size_t{256} * 1024 * 1024;

curl_off_t info_off(CURL* h, CURLINFO what) {
  curl_off_t v = 0;
  if (curl_easy_getinfo(h, what, &v) != CURLE_OK) return 0;
  return v < 0 ? 0 : v;
}

curl_off_t info_long(CURL* h, CURLINFO what) {
  long v = 0;
  if (curl_easy_getinfo(h, what, &v) != CURLE_OK) return 0;
  return v < 0 ? 0 : static_cast<curl_off_t>(v);
}

// curl's per-hop time marks (microseconds since the hop started) and its byte / connection
// counters. curl accumulates all of them across redirect hops, so one hop's values are the
// difference of two snapshots.
struct Marks {
  curl_off_t q = 0, n = 0, c = 0, a = 0, p = 0, pt = 0, st = 0;
  curl_off_t req = 0, up = 0, conns = 0;
  // `connect_marks` false skips the DNS / connect / TLS marks: the caller knows that no
  // connection was opened during this send() (they stay 0, as curl reports for a reuse).
  static Marks read(CURL* h, bool connect_marks = true) {
    Marks m;
    m.q = info_off(h, CURLINFO_QUEUE_TIME_T);
    if (connect_marks) {
      m.n = info_off(h, CURLINFO_NAMELOOKUP_TIME_T);
      m.c = info_off(h, CURLINFO_CONNECT_TIME_T);
      m.a = info_off(h, CURLINFO_APPCONNECT_TIME_T);
    }
    m.p = info_off(h, CURLINFO_PRETRANSFER_TIME_T);
    m.pt = info_off(h, CURLINFO_POSTTRANSFER_TIME_T);
    m.st = info_off(h, CURLINFO_STARTTRANSFER_TIME_T);
    m.req = info_long(h, CURLINFO_REQUEST_SIZE);
    m.up = info_off(h, CURLINFO_SIZE_UPLOAD_T);
    m.conns = info_long(h, CURLINFO_NUM_CONNECTS);
    return m;
  }
  Marks minus(const Marks& o) const {
    auto d = [](curl_off_t x, curl_off_t y) { return x > y ? x - y : curl_off_t{0}; };
    Marks r;
    r.q = d(q, o.q);
    r.n = d(n, o.n);
    r.c = d(c, o.c);
    r.a = d(a, o.a);
    r.p = d(p, o.p);
    r.pt = d(pt, o.pt);
    r.st = d(st, o.st);
    r.req = d(req, o.req);
    r.up = d(up, o.up);
    r.conns = d(conns, o.conns);
    return r;
  }
};

struct SocketOpen {
  Clock::time_point at;     // curl asked for the socket
  Clock::time_point ready;  // socket created and configured
  sockaddr_storage addr;
};

// A redirect hop as captured while it happened; turned into a RedirectHop after the transfer.
struct RawHop {
  Marks at_end;              // curl's accumulated marks when the 3xx header block ended
  curl_off_t redirect_end = -1;  // curl's accumulated redirect time once it moved on
  curl_off_t dl_after = 0;   // accumulated wire body bytes once it moved on
  uint64_t header_bytes = 0;
  long status = 0;
  long port = 0;
  std::string url, ip;
};

std::string narrow(const wchar_t* w, int n = -1) {
  if (!w) return {};
  const int len = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
  if (len <= 0) return {};
  std::string s(static_cast<size_t>(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, n, s.data(), len, nullptr, nullptr);
  if (n < 0 && !s.empty() && s.back() == '\0') s.pop_back();
  return s;
}

std::string x500(const CERT_NAME_BLOB* name) {
  wchar_t buf[1024];
  const DWORD n = CertNameToStrW(X509_ASN_ENCODING, const_cast<CERT_NAME_BLOB*>(name),
                                 CERT_X500_NAME_STR | CERT_NAME_STR_REVERSE_FLAG, buf, 1024);
  return n > 1 ? narrow(buf) : std::string();
}

std::string simple_name(PCCERT_CONTEXT c, bool issuer) {
  wchar_t buf[512];
  const DWORD n = CertGetNameStringW(c, CERT_NAME_SIMPLE_DISPLAY_TYPE,
                                     issuer ? CERT_NAME_ISSUER_FLAG : 0, nullptr, buf, 512);
  return n > 1 ? narrow(buf) : std::string();
}

std::string iso_utc(const FILETIME& ft) {
  SYSTEMTIME st{};
  if (!FileTimeToSystemTime(&ft, &st)) return {};
  char b[32];
  snprintf(b, sizeof b, "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour,
           st.wMinute, st.wSecond);
  return b;
}

// "2026-01-15T23:59:59Z" -> "2026-01-15 23:59:59 UTC" (HttpResponse::cert_not_after).
std::string display_utc(const std::string& iso) {
  if (iso.size() != 20) return iso;
  std::string s = iso;
  s[10] = ' ';
  s.pop_back();
  return s + " UTC";
}

std::string hex_bytes(const BYTE* b, size_t n, bool reverse, bool colons) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string s;
  s.reserve(n * 3);
  for (size_t i = 0; i < n; ++i) {
    const BYTE v = b[reverse ? n - 1 - i : i];
    if (colons && i) s.push_back(':');
    s.push_back(kHex[v >> 4]);
    s.push_back(kHex[v & 15]);
  }
  return s;
}

std::vector<std::string> alt_names(PCCERT_CONTEXT c) {
  std::vector<std::string> out;
  PCERT_EXTENSION ext = CertFindExtension(szOID_SUBJECT_ALT_NAME2, c->pCertInfo->cExtension,
                                          c->pCertInfo->rgExtension);
  if (!ext) return out;
  CERT_ALT_NAME_INFO* info = nullptr;
  DWORD sz = 0;
  if (!CryptDecodeObjectEx(X509_ASN_ENCODING, X509_ALTERNATE_NAME, ext->Value.pbData,
                           ext->Value.cbData, CRYPT_DECODE_ALLOC_FLAG, nullptr, &info, &sz) ||
      !info)
    return out;
  out.reserve(std::min<DWORD>(info->cAltEntry, 200));
  for (DWORD i = 0; i < info->cAltEntry && i < 200; ++i) {
    const CERT_ALT_NAME_ENTRY& e = info->rgAltEntry[i];
    char buf[64] = {};
    if (e.dwAltNameChoice == CERT_ALT_NAME_DNS_NAME) {
      if (std::string n = narrow(e.pwszDNSName); !n.empty()) out.push_back(std::move(n));
    } else if (e.dwAltNameChoice == CERT_ALT_NAME_IP_ADDRESS &&
               (e.IPAddress.cbData == 4 || e.IPAddress.cbData == 16)) {
      if (inet_ntop(e.IPAddress.cbData == 4 ? AF_INET : AF_INET6, e.IPAddress.pbData, buf,
                    sizeof buf))
        out.emplace_back(buf);
    }
  }
  LocalFree(info);
  return out;
}

const char* tls_version_name(DWORD p) {
  if (p & 0x00003000) return "TLSv1.3";  // SP_PROT_TLS1_3_{SERVER,CLIENT}
  if (p & SP_PROT_TLS1_2) return "TLSv1.2";
  if (p & SP_PROT_TLS1_1) return "TLSv1.1";
  if (p & SP_PROT_TLS1_0) return "TLSv1.0";
  if (p & SP_PROT_SSL3) return "SSLv3";
  return p ? "unknown" : "";
}

// How the request asked for the peer to be checked. Read when a connection is first seen; curl
// only reuses a connection for a request with the same TLS settings.
struct VerifyMode {
  bool verify = true;
  bool own_ca = false;
};

// Reads version, cipher, ALPN and the server's certificate from the connection's Schannel
// context. Runs once per connection (~0.1 ms, next to a handshake of a millisecond or more;
// global_init pre-loads crypt32's name tables, which would add ~5 ms to the first one), from
// the pre-request callback (on_prereq): see there for why then.
std::shared_ptr<const TlsInfo> read_tls(curl_tlssessioninfo* si, CURL* easy, VerifyMode vm) {
  if (!si || !si->internals || si->backend != CURLSSLBACKEND_SCHANNEL) return nullptr;
  const auto t0 = Clock::now();
  auto* ctx = static_cast<CtxtHandle*>(si->internals);
  auto t = std::make_shared<TlsInfo>();
  SecPkgContext_ConnectionInfo ci{};
  if (QueryContextAttributesW(ctx, SECPKG_ATTR_CONNECTION_INFO, &ci) == SEC_E_OK)
    t->version = tls_version_name(ci.dwProtocol);
  SecPkgContext_CipherInfo ki{};
  ki.dwVersion = SECPKGCONTEXT_CIPHERINFO_V1;
  if (QueryContextAttributesW(ctx, SECPKG_ATTR_CIPHER_INFO, &ki) == SEC_E_OK)
    t->cipher = narrow(ki.szCipherSuite);
  if (t->cipher.empty() && ci.dwCipherStrength)
    t->cipher = "ALG 0x" + std::to_string(ci.aiCipher) + ", " +
                std::to_string(ci.dwCipherStrength) + "-bit";
  SecPkgContext_ApplicationProtocol ap{};
  if (QueryContextAttributesW(ctx, SECPKG_ATTR_APPLICATION_PROTOCOL, &ap) == SEC_E_OK &&
      ap.ProtoNegoStatus == SecApplicationProtocolNegotiationStatus_Success)
    t->alpn.assign(reinterpret_cast<const char*>(ap.ProtocolId),
                   std::min<size_t>(ap.ProtocolIdSize, sizeof ap.ProtocolId));
  long vr = 0;
  curl_easy_getinfo(easy, CURLINFO_SSL_VERIFYRESULT, &vr);
  if (!vm.verify)
    t->verify_result = "not verified (certificate verification is off for this request)";
  else if (vr != 0)
    t->verify_result = "failed (verify result " + std::to_string(vr) + ")";
  else
    t->verify_result = vm.own_ca
                           ? "verified: chain and host name, against the request's CA certificate"
                           : "verified: chain and host name, against the Windows trust store";
  PCCERT_CONTEXT leaf = nullptr;
  if (QueryContextAttributesW(ctx, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &leaf) == SEC_E_OK && leaf) {
    TlsCertificate& c = t->leaf;
    const CERT_INFO* in = leaf->pCertInfo;
    c.subject = x500(&in->Subject);
    c.issuer = x500(&in->Issuer);
    c.subject_cn = simple_name(leaf, false);
    c.issuer_cn = simple_name(leaf, true);
    c.subject_alt_names = alt_names(leaf);
    c.not_before = iso_utc(in->NotBefore);
    c.not_after = iso_utc(in->NotAfter);
    c.serial = hex_bytes(in->SerialNumber.pbData, in->SerialNumber.cbData, true, false);
    BYTE sha[32];
    DWORD n = sizeof sha;
    if (CertGetCertificateContextProperty(leaf, CERT_SHA256_HASH_PROP_ID, sha, &n) && n == 32)
      c.sha256_fingerprint = hex_bytes(sha, 32, false, true);
    // The chain as the server sent it (the leaf's store holds what came over the wire): follow
    // issuer names from the leaf through the certificates it included.
    PCCERT_CONTEXT cur = CertDuplicateCertificateContext(leaf);
    for (int depth = 0; cur && depth < 10; ++depth) {
      ++t->chain_length;
      if (!t->chain.empty()) t->chain += "\n";
      t->chain += simple_name(cur, false) + "  (issued by " + simple_name(cur, true) +
                  ", until " + iso_utc(cur->pCertInfo->NotAfter).substr(0, 10) + ")";
      if (CertCompareCertificateName(X509_ASN_ENCODING, &cur->pCertInfo->Subject,
                                     &cur->pCertInfo->Issuer))
        break;  // self-signed: the root
      PCCERT_CONTEXT up =
          leaf->hCertStore
              ? CertFindCertificateInStore(leaf->hCertStore, X509_ASN_ENCODING, 0,
                                           CERT_FIND_SUBJECT_NAME, &cur->pCertInfo->Issuer, nullptr)
              : nullptr;
      CertFreeCertificateContext(cur);
      cur = up;
    }
    if (cur) CertFreeCertificateContext(cur);
    CertFreeCertificateContext(leaf);
  }
  // The client certificate Schannel presented (mutual TLS), if the server asked for one.
  PCCERT_CONTEXT mine = nullptr;
  if (QueryContextAttributesW(ctx, SECPKG_ATTR_LOCAL_CERT_CONTEXT, &mine) == SEC_E_OK && mine) {
    t->client_cert_subject = simple_name(mine, false);
    t->client_cert_issuer = simple_name(mine, true);
    CertFreeCertificateContext(mine);
  }
  t->read_ms = ms_between(t0, Clock::now());
  return t;
}

// ------------------------------------------------------------------ client certificates
// Schannel takes a client certificate from memory only as PKCS#12, so a PEM certificate + key
// is converted once: the key is imported into an ephemeral (never persisted) CNG key, attached
// to the certificate in a memory store and exported as a PKCS#12 protected by kPfxPass.

constexpr wchar_t kPfxPassW[] = L"watt-mtls";
constexpr char kPfxPass[] = "watt-mtls";

struct PemBlock {
  std::string label;  // "CERTIFICATE", "PRIVATE KEY", ...
  std::string der;
};

std::vector<PemBlock> pem_blocks(const std::string& text) {
  std::vector<PemBlock> out;
  size_t at = 0;
  while ((at = text.find("-----BEGIN ", at)) != std::string::npos) {
    const size_t lab = at + 11;
    const size_t lab_end = text.find("-----", lab);
    if (lab_end == std::string::npos) break;
    const std::string label = text.substr(lab, lab_end - lab);
    const std::string end_mark = "-----END " + label + "-----";
    const size_t end = text.find(end_mark, lab_end);
    if (end == std::string::npos) break;
    const size_t body = lab_end + 5;
    const std::string b64 = text.substr(body, end - body);
    DWORD n = 0;
    PemBlock b;
    b.label = label;
    if (CryptStringToBinaryA(b64.data(), static_cast<DWORD>(b64.size()), CRYPT_STRING_BASE64, nullptr, &n,
                             nullptr, nullptr) && n) {
      b.der.resize(n);
      if (CryptStringToBinaryA(b64.data(), static_cast<DWORD>(b64.size()), CRYPT_STRING_BASE64,
                               reinterpret_cast<BYTE*>(b.der.data()), &n, nullptr, nullptr)) {
        b.der.resize(n);
        out.push_back(std::move(b));
      }
    }
    at = end + end_mark.size();
  }
  return out;
}

// DER of a PKCS#8 PrivateKeyInfo wrapping a PKCS#1 RSA key or a SEC1 EC key.
bool wrap_pkcs8(const PemBlock& k, std::string& out) {
  CRYPT_PRIVATE_KEY_INFO pki{};
  BYTE rsa_null[] = {0x05, 0x00};
  std::string params;
  std::string oid;
  if (k.label == "RSA PRIVATE KEY") {
    oid = szOID_RSA_RSA;
    pki.Algorithm.Parameters.pbData = rsa_null;
    pki.Algorithm.Parameters.cbData = sizeof rsa_null;
  } else {  // EC PRIVATE KEY: the curve comes from the SEC1 structure
    CRYPT_ECC_PRIVATE_KEY_INFO* ec = nullptr;
    DWORD n = 0;
    if (!CryptDecodeObjectEx(X509_ASN_ENCODING, X509_ECC_PRIVATE_KEY, reinterpret_cast<const BYTE*>(k.der.data()),
                             static_cast<DWORD>(k.der.size()), CRYPT_DECODE_ALLOC_FLAG, nullptr, &ec, &n) ||
        !ec)
      return false;
    const std::string curve = ec->szCurveOid ? ec->szCurveOid : "";
    LocalFree(ec);
    if (curve.empty()) return false;
    LPCSTR c = curve.c_str();
    DWORD pn = 0;
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, X509_OBJECT_IDENTIFIER, &c, 0, nullptr, nullptr, &pn)) return false;
    params.resize(pn);
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, X509_OBJECT_IDENTIFIER, &c, 0, nullptr,
                             reinterpret_cast<BYTE*>(params.data()), &pn))
      return false;
    params.resize(pn);
    oid = szOID_ECC_PUBLIC_KEY;
    pki.Algorithm.Parameters.pbData = reinterpret_cast<BYTE*>(params.data());
    pki.Algorithm.Parameters.cbData = static_cast<DWORD>(params.size());
  }
  pki.Algorithm.pszObjId = oid.data();
  pki.PrivateKey.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(k.der.data()));
  pki.PrivateKey.cbData = static_cast<DWORD>(k.der.size());
  DWORD n = 0;
  if (!CryptEncodeObjectEx(X509_ASN_ENCODING, PKCS_PRIVATE_KEY_INFO, &pki, 0, nullptr, nullptr, &n)) return false;
  out.resize(n);
  if (!CryptEncodeObjectEx(X509_ASN_ENCODING, PKCS_PRIVATE_KEY_INFO, &pki, 0, nullptr,
                           reinterpret_cast<BYTE*>(out.data()), &n))
    return false;
  out.resize(n);
  return true;
}

std::wstring widen_utf8(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L' ');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string hr_text(long st) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "0x%08lx", static_cast<unsigned long>(st));
  return buf;
}

// PEM certificate (+ chain) and private key -> PKCS#12 (password kPfxPass). false + err.
bool pem_to_pfx(const std::string& cert_pem, const std::string& key_pem, const std::string& pass, std::string& pfx,
                std::string& err) {
  std::vector<PemBlock> certs, keys;
  for (const std::string* t : {&cert_pem, &key_pem})
    for (auto& b : pem_blocks(*t)) {
      if (b.label == "CERTIFICATE" || b.label == "TRUSTED CERTIFICATE")
        certs.push_back(std::move(b));
      else if (b.label.find("PRIVATE KEY") != std::string::npos)
        keys.push_back(std::move(b));
    }
  if (certs.empty()) {
    err = "client certificate: no PEM \"BEGIN CERTIFICATE\" block found";
    return false;
  }
  if (keys.empty()) {
    err = "client certificate: no private key: pick a PEM key file (BEGIN PRIVATE KEY) or use a .pfx / .p12";
    return false;
  }
  const PemBlock& k = keys.front();
  std::string pkcs8;
  const bool encrypted = k.label == "ENCRYPTED PRIVATE KEY";
  if (k.label == "PRIVATE KEY" || encrypted) {
    pkcs8 = k.der;
  } else if (k.label == "RSA PRIVATE KEY" || k.label == "EC PRIVATE KEY") {
    if (!wrap_pkcs8(k, pkcs8)) {
      err = "client certificate: cannot read the " + k.label +
            " (an encrypted legacy PEM key is not supported: convert it with openssl pkcs8 -topk8)";
      return false;
    }
  } else {
    err = "client certificate: unsupported key type \"" + k.label + "\"";
    return false;
  }
  if (encrypted && pass.empty()) {
    err = "client certificate: the private key is encrypted: enter its passphrase";
    return false;
  }
  NCRYPT_PROV_HANDLE prov = 0;
  SECURITY_STATUS st = NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER, 0);
  if (st != ERROR_SUCCESS) {
    err = "client certificate: no key storage provider (" + hr_text(st) + ")";
    return false;
  }
  NCRYPT_KEY_HANDLE key = 0;
  std::wstring wpass = widen_utf8(pass);
  NCryptBuffer nb{static_cast<ULONG>((wpass.size() + 1) * sizeof(wchar_t)), NCRYPTBUFFER_PKCS_SECRET, wpass.data()};
  NCryptBufferDesc desc{NCRYPTBUFFER_VERSION, 1, &nb};
  st = NCryptImportKey(prov, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, encrypted ? &desc : nullptr, &key,
                       reinterpret_cast<BYTE*>(pkcs8.data()), static_cast<DWORD>(pkcs8.size()),
                       NCRYPT_DO_NOT_FINALIZE_FLAG);
  SecureZeroMemory(wpass.data(), wpass.size() * sizeof(wchar_t));
  NCryptFreeObject(prov);
  if (st != ERROR_SUCCESS) {
    err = encrypted ? "client certificate: cannot decrypt the private key: wrong passphrase? (" + hr_text(st) + ")"
                    : "client certificate: cannot read the private key (" + hr_text(st) + ")";
    return false;
  }
  DWORD policy = NCRYPT_ALLOW_EXPORT_FLAG | NCRYPT_ALLOW_PLAINTEXT_EXPORT_FLAG;
  NCryptSetProperty(key, NCRYPT_EXPORT_POLICY_PROPERTY, reinterpret_cast<BYTE*>(&policy), sizeof policy, 0);
  if ((st = NCryptFinalizeKey(key, 0)) != ERROR_SUCCESS) {
    NCryptFreeObject(key);
    err = "client certificate: cannot use the private key (" + hr_text(st) + ")";
    return false;
  }
  // The key's public half, to find the certificate it belongs to (the PEM may list the chain first).
  DWORD pn = 0;
  std::string pub;
  if (CryptExportPublicKeyInfo(key, 0, X509_ASN_ENCODING, nullptr, &pn)) {
    pub.resize(pn);
    if (!CryptExportPublicKeyInfo(key, 0, X509_ASN_ENCODING, reinterpret_cast<CERT_PUBLIC_KEY_INFO*>(pub.data()), &pn))
      pub.clear();
  }
  HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr);
  if (!store) {
    NCryptFreeObject(key);
    err = "client certificate: cannot create a certificate store";
    return false;
  }
  PCCERT_CONTEXT leaf = nullptr;
  for (const auto& c : certs) {
    PCCERT_CONTEXT cx = nullptr;
    if (!CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, reinterpret_cast<const BYTE*>(c.der.data()),
                                          static_cast<DWORD>(c.der.size()), CERT_STORE_ADD_ALWAYS, &cx) ||
        !cx)
      continue;
    const bool match =
        !pub.empty() && CertComparePublicKeyInfo(X509_ASN_ENCODING, &cx->pCertInfo->SubjectPublicKeyInfo,
                                                 reinterpret_cast<CERT_PUBLIC_KEY_INFO*>(pub.data()));
    if (!leaf && match)
      leaf = cx;
    else
      CertFreeCertificateContext(cx);
  }
  bool ok = false;
  if (!leaf) {
    NCryptFreeObject(key);
    err = "client certificate: the private key does not belong to any certificate in the PEM";
  } else if (CERT_KEY_CONTEXT kc{sizeof kc, {key}, CERT_NCRYPT_KEY_SPEC};
             !CertSetCertificateContextProperty(leaf, CERT_KEY_CONTEXT_PROP_ID, 0, &kc)) {
    NCryptFreeObject(key);  // once the property is set, the certificate context owns the key
    err = "client certificate: cannot attach the private key";
  } else {
    CRYPT_DATA_BLOB blob{};
    const DWORD flags = EXPORT_PRIVATE_KEYS | REPORT_NO_PRIVATE_KEY | REPORT_NOT_ABLE_TO_EXPORT_PRIVATE_KEY;
    if (PFXExportCertStoreEx(store, &blob, kPfxPassW, nullptr, flags) && blob.cbData) {
      pfx.resize(blob.cbData);
      blob.pbData = reinterpret_cast<BYTE*>(pfx.data());
      ok = PFXExportCertStoreEx(store, &blob, kPfxPassW, nullptr, flags) != FALSE;
      pfx.resize(blob.cbData);
    }
    if (!ok)
      err = "client certificate: cannot package the certificate and key (" +
            hr_text(static_cast<long>(GetLastError())) + ")";
  }
  if (leaf) CertFreeCertificateContext(leaf);
  CertCloseStore(store, 0);
  return ok;
}

// Schannel signs the TLS handshake in LSASS, which cannot reach a key that lives only in this
// process (an ephemeral / PKCS12_NO_PERSIST_KEY import fails with SEC_E_UNKNOWN_CREDENTIALS or
// SEC_E_INTERNAL_ERROR). So a client certificate from a file is imported once into the current
// user's "watt" certificate store (certmgr.msc shows it), its key into the user's Windows key
// store (DPAPI-protected, like any imported .pfx), and libcurl is pointed at it by thumbprint
// ("CurrentUser\watt\<sha1>"). An already imported certificate with a usable key is reused.
// "store:CurrentUser\MY\<sha1>" (HttpRequest::client_cert) names a certificate that is already
// in a Windows store (smart card, enterprise enrolment) and is used as it is.
std::string install_client_cert(const std::string& pfx, const std::string& pass, std::string& err) {
  CRYPT_DATA_BLOB b{static_cast<DWORD>(pfx.size()), reinterpret_cast<BYTE*>(const_cast<char*>(pfx.data()))};
  if (!PFXIsPFXBlob(&b)) {
    err = "client certificate: the file is neither PEM (BEGIN CERTIFICATE) nor PKCS#12 (.pfx / .p12)";
    return {};
  }
  std::wstring wpass = widen_utf8(pass);
  auto wipe = [&] { SecureZeroMemory(wpass.data(), wpass.size() * sizeof(wchar_t)); };
  // Read the certificate (no key persisted) to learn its thumbprint.
  HCERTSTORE tmp = PFXImportCertStore(&b, wpass.c_str(), PKCS12_NO_PERSIST_KEY);
  if (!tmp) {
    const DWORD e = GetLastError();
    wipe();
    err = e == ERROR_INVALID_PASSWORD ? "client certificate: wrong passphrase for the PKCS#12 file"
                                      : "client certificate: cannot read the PKCS#12 file (" + hr_text(static_cast<long>(e)) + ")";
    return {};
  }
  BYTE sha1[20];
  DWORD n = sizeof sha1;
  PCCERT_CONTEXT c = CertFindCertificateInStore(tmp, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                                                CERT_FIND_HAS_PRIVATE_KEY, nullptr, nullptr);
  const bool have = c && CertGetCertificateContextProperty(c, CERT_SHA1_HASH_PROP_ID, sha1, &n) && n == 20;
  if (c) CertFreeCertificateContext(c);
  CertCloseStore(tmp, 0);
  if (!have) {
    wipe();
    err = "client certificate: the PKCS#12 file holds no certificate with a private key";
    return {};
  }
  const std::string path = "CurrentUser\\watt\\" + hex_bytes(sha1, 20, false, false);
  HCERTSTORE gs = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, L"watt");
  if (!gs) {
    wipe();
    err = "client certificate: cannot open the current user's watt certificate store";
    return {};
  }
  CRYPT_HASH_BLOB hb{20, sha1};
  bool ok = false;
  if (PCCERT_CONTEXT ex = CertFindCertificateInStore(gs, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                                                     CERT_FIND_SHA1_HASH, &hb, nullptr)) {
    HCRYPTPROV_OR_NCRYPT_KEY_HANDLE k = 0;
    DWORD spec = 0;
    BOOL must_free = FALSE;
    ok = CryptAcquireCertificatePrivateKey(ex, CRYPT_ACQUIRE_SILENT_FLAG | CRYPT_ACQUIRE_PREFER_NCRYPT_KEY_FLAG, nullptr,
                                           &k, &spec, &must_free) != FALSE;
    if (ok && must_free) {
      if (spec == CERT_NCRYPT_KEY_SPEC)
        NCryptFreeObject(k);
      else
        CryptReleaseContext(k, 0);
    }
    if (!ok) CertDeleteCertificateFromStore(CertDuplicateCertificateContext(ex));  // its key is gone: re-import
    CertFreeCertificateContext(ex);
  }
  if (!ok) {
    HCERTSTORE ps = PFXImportCertStore(&b, wpass.c_str(), CRYPT_USER_KEYSET | PKCS12_ALWAYS_CNG_KSP);
    if (ps) {
      if (PCCERT_CONTEXT pc = CertFindCertificateInStore(ps, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                                                         CERT_FIND_SHA1_HASH, &hb, nullptr)) {
        ok = CertAddCertificateContextToStore(gs, pc, CERT_STORE_ADD_REPLACE_EXISTING, nullptr) != FALSE;
        CertFreeCertificateContext(pc);
      }
      CertCloseStore(ps, 0);
    }
    if (!ok) err = "client certificate: cannot import it into the Windows key store (" + hr_text(static_cast<long>(GetLastError())) + ")";
  }
  wipe();
  CertCloseStore(gs, 0);
  return ok ? path : std::string();
}

// What CURLOPT_SSLCERT gets for a request's client certificate ("" + err when it cannot be used).
// Process-wide cache: every HttpClient (tab, runner worker) pays the conversion and the store
// lookup once per distinct certificate.
std::string client_cert_location(const HttpRequest& req, std::string& err) {
  // client_cert is always certificate DATA (PEM or PKCS#12), never a reference to a certificate
  // in the user's store: it is read from workspace files, so a "store:..." shortcut would let a
  // cloned repo present the user's own personal certificate to its server.
  const std::string& c = req.client_cert;
  static std::mutex mu;
  static std::vector<std::pair<std::string, std::string>> cache;  // cert \0 key \0 pass -> location
  std::string id = c;
  id += '\0';
  id += req.client_key;
  id += '\0';
  id += req.client_key_passphrase;
  {
    std::lock_guard<std::mutex> lock(mu);
    for (const auto& [k, v] : cache)
      if (k == id) return v;
  }
  std::string loc;
  if (c.find("-----BEGIN") != std::string::npos) {
    std::string pfx;
    if (pem_to_pfx(c, req.client_key, req.client_key_passphrase, pfx, err)) loc = install_client_cert(pfx, kPfxPass, err);
    SecureZeroMemory(pfx.data(), pfx.size());
  } else {
    loc = install_client_cert(c, req.client_key_passphrase, err);
  }
  if (!loc.empty()) {
    std::lock_guard<std::mutex> lock(mu);
    if (cache.size() >= 16) cache.erase(cache.begin());
    cache.emplace_back(std::move(id), loc);
  }
  return loc;
}

// ------------------------------------------------------------------ proxy

// "scheme://user:pw@host:port/x" -> "scheme://host:port/x" (for display: never show credentials).
std::string redact_proxy(std::string u) {
  const size_t s = u.find("://");
  const size_t from = s == std::string::npos ? 0 : s + 3;
  const size_t slash = u.find('/', from);
  const size_t at = u.rfind('@', slash == std::string::npos ? std::string::npos : slash);
  if (at != std::string::npos && at >= from) u.erase(from, at + 1 - from);
  return u;
}

std::vector<std::string> split_any(const std::string& s, std::string_view seps) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    const size_t j = s.find_first_of(seps, i);
    std::string part = s.substr(i, j == std::string::npos ? std::string::npos : j - i);
    if (!part.empty()) out.push_back(std::move(part));
    if (j == std::string::npos) break;
    i = j + 1;
  }
  return out;
}

// The current user's Windows proxy (Settings > Network > Proxy, Internet Options), read at most
// every 10 s: static servers and the bypass list in curl's NOPROXY syntax.
struct SystemProxy {
  std::string http, https;  // "http://host:port" or ""
  std::string bypass;       // curl NOPROXY list (loopback always included)
};

SystemProxy system_proxy() {
  static std::mutex mu;
  static SystemProxy cached;
  static Clock::time_point at{};
  static bool have = false;
  std::lock_guard<std::mutex> lock(mu);
  const auto now = Clock::now();
  if (have && now - at < std::chrono::seconds(10)) return cached;
  SystemProxy p;
  p.bypass = "localhost,127.0.0.1,::1";
  WINHTTP_CURRENT_USER_IE_PROXY_CONFIG cfg{};
  if (WinHttpGetIEProxyConfigForCurrentUser(&cfg)) {
    const std::string list = cfg.lpszProxy ? narrow(cfg.lpszProxy) : "";
    const std::string bypass = cfg.lpszProxyBypass ? narrow(cfg.lpszProxyBypass) : "";
    if (cfg.lpszProxy) GlobalFree(cfg.lpszProxy);
    if (cfg.lpszProxyBypass) GlobalFree(cfg.lpszProxyBypass);
    if (cfg.lpszAutoConfigUrl) GlobalFree(cfg.lpszAutoConfigUrl);
    auto url = [](const std::string& v) { return v.find("://") == std::string::npos ? "http://" + v : v; };
    for (const auto& e : split_any(list, "; \t")) {
      const size_t eq = e.find('=');
      if (eq == std::string::npos) {
        p.http = p.https = url(e);
        continue;
      }
      std::string k = e.substr(0, eq);
      std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      const std::string v = e.substr(eq + 1);
      if (v.empty()) continue;
      if (k == "http")
        p.http = url(v);
      else if (k == "https")
        p.https = url(v);
      else if (k == "socks" && p.http.empty() && p.https.empty())
        p.http = p.https = "socks4://" + v;
    }
    for (auto e : split_any(bypass, "; ,\t")) {
      if (e == "<local>") continue;  // dotless names: curl has no syntax for them; loopback is listed
      if (e.rfind("*.", 0) == 0) e.erase(0, 1);  // *.corp.com -> .corp.com (curl: domain and below)
      if (e.size() > 2 && e.compare(e.size() - 2, 2, ".*") == 0) {  // 10.* / 192.168.* -> CIDR
        std::string pre = e.substr(0, e.size() - 2);
        int parts = 1;
        for (char c : pre) parts += c == '.';
        if (parts > 3 || pre.find_first_not_of("0123456789.") != std::string::npos) continue;
        for (int i = parts; i < 4; ++i) pre += ".0";
        e = pre + "/" + std::to_string(parts * 8);
      } else if (e.find('*') != std::string::npos) {
        continue;  // other wildcards cannot be expressed in curl's list
      }
      p.bypass += "," + e;
    }
  }
  cached = p;
  at = now;
  have = true;
  return p;
}

bool is_https_url(const std::string& url) {
  return url.size() >= 6 && (url[0] == 'h' || url[0] == 'H') && (url[4] == 's' || url[4] == 'S') && url[5] == ':';
}

// The proxy an Env-mode request uses, as libcurl picks it (https_proxy / http_proxy / all_proxy).
std::string env_proxy(const std::string& url) {
  auto get = [](const char* n) -> std::string {
    char buf[1024];
    const DWORD k = GetEnvironmentVariableA(n, buf, sizeof buf);
    return k > 0 && k < sizeof buf ? std::string(buf, k) : std::string();
  };
  const bool https = is_https_url(url);
  std::string v = https ? get("https_proxy") : get("http_proxy");
  if (v.empty() && https) v = get("HTTPS_PROXY");
  if (v.empty()) v = get("all_proxy");
  if (v.empty()) v = get("ALL_PROXY");
  return v;
}

// One connection as this client saw it, plus the HttpResponse mirror strings ready to copy.
struct ConnEntry {
  ConnectionInfo info;
  std::string cert_san, cert_not_after;  // joined / display forms of tls_info's fields
};

// Connections this client has delivered responses on, keyed by libcurl's connection id (ids
// are never reused within a share handle). A few entries cover alternating hosts (redirects);
// a stale entry for a closed connection is harmless and gets overwritten.
struct ConnCache {
  static constexpr size_t kSize = 4;
  std::array<ConnEntry, kSize> e;
  size_t next = 0;
  const ConnEntry* find(curl_off_t id) const {
    if (id < 0) return nullptr;
    for (const ConnEntry& c : e)
      if (c.info.id == id) return &c;
    return nullptr;
  }
  ConnEntry& insert() {
    ConnEntry& c = e[next];
    next = (next + 1) % kSize;
    c = ConnEntry{};
    return c;
  }
};

// Reads the connection the transfer is on right now (called from on_prereq).
void read_connection(CURL* easy, curl_off_t id, VerifyMode vm, ConnEntry& out) {
  ConnectionInfo& ci = out.info;
  ci.id = id;
  char* s = nullptr;
  if (curl_easy_getinfo(easy, CURLINFO_LOCAL_IP, &s) == CURLE_OK && s) ci.local_ip = s;
  curl_easy_getinfo(easy, CURLINFO_LOCAL_PORT, &ci.local_port);
  s = nullptr;
  if (curl_easy_getinfo(easy, CURLINFO_PRIMARY_IP, &s) == CURLE_OK && s) ci.remote_ip = s;
  curl_easy_getinfo(easy, CURLINFO_PRIMARY_PORT, &ci.remote_port);
  curl_tlssessioninfo* si = nullptr;
  if (curl_easy_getinfo(easy, CURLINFO_TLS_SSL_PTR, &si) != CURLE_OK) si = nullptr;
  ci.tls = si && si->internals;
  if (!ci.tls) return;
  ci.tls_info = read_tls(si, easy, vm);
  if (!ci.tls_info) return;
  for (const std::string& n : ci.tls_info->leaf.subject_alt_names) {
    if (!out.cert_san.empty()) out.cert_san += ", ";
    out.cert_san += n;
  }
  out.cert_not_after = display_utc(ci.tls_info->leaf.not_after);
}

struct TransferCtx {
  HttpResponse* resp = nullptr;
  const BodyChunkFn* on_chunk = nullptr;
  bool has_chunk = false;
  std::atomic<bool>* cancel = nullptr;
  CURL* easy = nullptr;
  bool keep_body = true;
  bool follow = false;
  bool hop_open = false;      // a followed 3xx ended; the next status line starts a new hop
  long hop_status = 0;
  bool have_location = false;
  uint64_t hop_header_bytes = 0;  // response header bytes of the current hop (incl. 1xx)
  uint64_t hop_body_bytes = 0;    // decoded body bytes of the current hop
  std::vector<RawHop> hops;
  // Response headers go straight into HttpResponse::headers (one text buffer + offsets),
  // reserved from what the previous response needed, so a re-fired request allocates exactly
  // twice for all its headers and nothing is converted after the transfer.
  size_t last_hdr_bytes = 512, last_hdr_count = 16;
  const char* hop_version = "";  // protocol of the latest status line ("HTTP/1.1", "HTTP/2")
  std::array<SocketOpen, 8> opens;  // sockets curl opened during this send(), in order
  size_t n_opens = 0;
  curl_socket_t preconnected = CURL_SOCKET_BAD;  // on_opensocket connected it (loopback)
  // Connection details (HttpResponse::conn), read at a status line on a connection this client
  // has not seen yet and kept across sends; a reused connection costs one CONN_ID lookup.
  ConnCache conns;
  VerifyMode verify_mode;
  // Head hook (HeadFn): fired once per final header block; t0 / t_io as in send().
  const HeadFn* on_head = nullptr;
  bool head_sent = false;
  bool client_cert = false;
  // The final head said text/event-stream: send() lifts the total timeout (a stream is open
  // until one side closes it, as in Postman / Insomnia) and keeps an idle timeout instead.
  bool event_stream = false;
  Clock::time_point t0{}, t_io{};

  void begin(HttpResponse* r, const BodyChunkFn* chunk, const HttpRequest& req) {
    resp = r;
    on_chunk = chunk;
    has_chunk = chunk && *chunk;
    head_sent = false;
    event_stream = false;
    keep_body = req.keep_body;
    follow = req.follow_redirects;
    hop_open = false;
    hop_status = 0;
    have_location = false;
    hop_header_bytes = 0;
    hop_body_bytes = 0;
    hops.clear();
    hop_version = "";
    n_opens = 0;
    verify_mode.verify = req.verify_tls;
    verify_mode.own_ca = !req.ca_pem.empty();
    r->headers.reserve(last_hdr_bytes, last_hdr_count);
  }
  // curl has moved on from the last followed 3xx: close that hop.
  void close_hop() {
    RawHop& h = hops.back();
    h.redirect_end = info_off(easy, CURLINFO_REDIRECT_TIME_T);
    h.dl_after = info_off(easy, CURLINFO_SIZE_DOWNLOAD_T);
    hop_open = false;
    hop_header_bytes = 0;
    hop_body_bytes = 0;
  }
};

// Protocol token of a status line as a static string (no allocation when assigned).
const char* version_name(std::string_view v) {
  if (v == "HTTP/1.1") return "HTTP/1.1";
  if (v == "HTTP/2" || v == "HTTP/2.0") return "HTTP/2";
  if (v == "HTTP/1.0") return "HTTP/1.0";
  if (v == "HTTP/3") return "HTTP/3";
  return "";
}

bool is_redirect_code(long s) { return s == 301 || s == 302 || s == 303 || s == 307 || s == 308; }

void fire_head(TransferCtx* ctx);  // below split_hop

size_t on_header(char* buf, size_t size, size_t nitems, void* userdata) {
  auto* ctx = static_cast<TransferCtx*>(userdata);
  const size_t n = size * nitems;
  std::string_view line(buf, n);
  HttpResponse& r = *ctx->resp;

  if (line.size() >= 5 && line.compare(0, 5, "HTTP/") == 0) {
    // New response (1xx, redirect hop or final): keep only the latest one.
    if (ctx->hop_open) ctx->close_hop();
    ctx->hop_header_bytes += n;
    r.headers.clear();
    r.body.clear();
    r.status_text.clear();
    ctx->hop_status = 0;
    ctx->have_location = false;
    ctx->head_sent = false;
    std::string_view rest = trim(line);
    size_t sp = rest.find(' ');
    if (sp != std::string_view::npos) {
      ctx->hop_version = version_name(rest.substr(0, sp));
      rest.remove_prefix(sp + 1);
      long code = 0;
      for (size_t i = 0; i < rest.size() && i < 3 && rest[i] >= '0' && rest[i] <= '9'; ++i)
        code = code * 10 + (rest[i] - '0');
      ctx->hop_status = code;
      size_t sp2 = rest.find(' ');
      if (sp2 != std::string_view::npos) r.status_text = std::string(trim(rest.substr(sp2 + 1)));
    }
    return n;
  }
  ctx->hop_header_bytes += n;
  std::string_view t = trim(line);
  if (t.empty()) {
    // End of a header block. If curl is about to follow this one, remember where the hop
    // stands: everything after this belongs to the next hop.
    if (ctx->hop_status >= 200 && ctx->hop_status < 300 && !ctx->event_stream) {
      const std::string_view ct = r.headers.get("content-type");
      ctx->event_stream = ct.size() >= 17 && iequals(ct.substr(0, 17), "text/event-stream");
    }
    if (ctx->follow && ctx->have_location && is_redirect_code(ctx->hop_status)) {
      RawHop& h = ctx->hops.emplace_back();
      h.at_end = Marks::read(ctx->easy);
      h.header_bytes = ctx->hop_header_bytes;
      h.status = ctx->hop_status;
      char* s = nullptr;
      if (curl_easy_getinfo(ctx->easy, CURLINFO_EFFECTIVE_URL, &s) == CURLE_OK && s) h.url = s;
      s = nullptr;
      if (curl_easy_getinfo(ctx->easy, CURLINFO_PRIMARY_IP, &s) == CURLE_OK && s) h.ip = s;
      curl_easy_getinfo(ctx->easy, CURLINFO_PRIMARY_PORT, &h.port);
      ctx->hop_open = true;
    } else if (ctx->hop_status >= 200 && !ctx->head_sent && ctx->on_head && *ctx->on_head) {
      // The final response's head (trailers after the body end with a blank line too: once).
      ctx->head_sent = true;
      fire_head(ctx);
    }
    return n;
  }
  size_t colon = t.find(':');
  if (colon == std::string_view::npos) return n;
  std::string_view name = trim(t.substr(0, colon));
  std::string_view value = trim(t.substr(colon + 1));
  if (iequals(name, "content-length")) {
    size_t len = 0;
    bool valid = !value.empty();
    for (char c : value) {
      if (c < '0' || c > '9') { valid = false; break; }
      len = len * 10 + static_cast<size_t>(c - '0');
      if (len > kMaxReserve) { len = kMaxReserve; break; }
    }
    if (ctx->keep_body && valid && len > r.body.capacity()) r.body.reserve(len);
  } else if (iequals(name, "location") && !value.empty()) {
    ctx->have_location = true;
  }
  r.headers.add(name, value);
  return n;
}

// libcurl calls this once per request (and per redirect hop) when the connection is ready,
// after any TLS handshake and before the request goes out. A connection this client has not
// seen yet gets its addresses and TLS session read here: Schannel's negotiated ALPN is only
// readable until the first post-handshake records (TLS 1.3 session tickets) are decrypted,
// and CURLINFO_TLS_SSL_PTR only while the connection is attached. A known connection costs
// one CONN_ID lookup and a compare.
int on_prereq(void* userdata, char*, char*, int, int) {
  auto* ctx = static_cast<TransferCtx*>(userdata);
  curl_off_t cid = -1;
  if (curl_easy_getinfo(ctx->easy, CURLINFO_CONN_ID, &cid) == CURLE_OK && cid >= 0 &&
      !ctx->conns.find(cid))
    read_connection(ctx->easy, cid, ctx->verify_mode, ctx->conns.insert());
  return CURL_PREREQFUNC_OK;
}

size_t on_write(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* ctx = static_cast<TransferCtx*>(userdata);
  const size_t n = size * nmemb;
  if (ctx->cancel->load(std::memory_order_relaxed)) return CURL_WRITEFUNC_ERROR;
  ctx->hop_body_bytes += n;
  if (ctx->keep_body) ctx->resp->body.append(ptr, n);
  if (ctx->has_chunk) {
    try {
      (*ctx->on_chunk)(ptr, n);
    } catch (...) {  // never let an exception unwind through libcurl's C frames
      return CURL_WRITEFUNC_ERROR;
    }
  }
  return n;
}

bool is_loopback(const sockaddr* sa) {
  if (sa->sa_family == AF_INET) {
    const auto* in = reinterpret_cast<const sockaddr_in*>(sa);
    return (ntohl(in->sin_addr.s_addr) >> 24) == 127;
  }
  if (sa->sa_family == AF_INET6) {
    const auto* in6 = reinterpret_cast<const sockaddr_in6*>(sa);
    const unsigned char* b = in6->sin6_addr.s6_addr;
    static constexpr unsigned char kLoop[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    if (std::memcmp(b, kLoop, 16) == 0) return true;
    static constexpr unsigned char kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    return std::memcmp(b, kMapped, 12) == 0 && b[12] == 127;  // ::ffff:127.x.y.z
  }
  return false;
}

curl_socket_t on_opensocket(void* userdata, curlsocktype purpose, curl_sockaddr* address) {
  auto* ctx = static_cast<TransferCtx*>(userdata);
  const auto at = Clock::now();
  curl_socket_t s = socket(address->family, address->socktype, address->protocol);
  if (s == CURL_SOCKET_BAD) return s;
  const auto* sa = reinterpret_cast<const sockaddr*>(&address->addr);
  const bool loop_tcp =
      purpose == CURLSOCKTYPE_IPCXN && address->socktype == SOCK_STREAM && is_loopback(sa);
  // A refused loopback port answers with RST immediately; without SYN retransmissions the
  // connect fails at once and curl moves on to the next address (::1 -> 127.0.0.1).
  auto no_syn_retries = [](curl_socket_t fd) {
    TCP_INITIAL_RTO_PARAMETERS rto{};
    rto.Rtt = TCP_INITIAL_RTO_UNSPECIFIED_RTT;
    rto.MaxSynRetransmissions = TCP_INITIAL_RTO_NO_SYN_RETRANSMISSIONS;
    DWORD bytes = 0;
    WSAIoctl(fd, SIO_TCP_INITIAL_RTO, &rto, sizeof(rto), nullptr, 0, &bytes, nullptr, nullptr);
  };
  if (loop_tcp) no_syn_retries(s);
  if (ctx->n_opens < ctx->opens.size()) {
    SocketOpen& o = ctx->opens[ctx->n_opens++];
    o.at = at;
    std::memset(&o.addr, 0, sizeof(o.addr));
    std::memcpy(&o.addr, sa,
                std::min<size_t>(sizeof(o.addr), static_cast<size_t>(address->addrlen)));
    o.ready = Clock::now();  // the handshake below counts as Connect, not socket set-up
  }
  // Loopback: connect here and let on_sockopt report it connected, so curl skips its own
  // connect check (see core/loopback_connect.h: ~10 ms on every other fresh connection).
  ctx->preconnected = CURL_SOCKET_BAD;
  if (loop_tcp) {
    switch (detail::loopback_preconnect(s, sa, static_cast<int>(address->addrlen))) {
      case detail::PreConnect::Connected:
        ctx->preconnected = s;
        break;
      case detail::PreConnect::Refused:
        // curl moves on to the next address, or fails with its usual "Failed to connect".
        closesocket(s);
        WSASetLastError(WSAECONNREFUSED);
        return CURL_SOCKET_BAD;
      case detail::PreConnect::Unknown:
        // Slow: curl connects a fresh socket itself and reports the outcome as usual.
        closesocket(s);
        s = socket(address->family, address->socktype, address->protocol);
        if (s != CURL_SOCKET_BAD) no_syn_retries(s);
        break;
    }
  }
  return s;
}

int on_sockopt(void* userdata, curl_socket_t s, curlsocktype purpose) {
  const auto* ctx = static_cast<const TransferCtx*>(userdata);
  return purpose == CURLSOCKTYPE_IPCXN && s != CURL_SOCKET_BAD && s == ctx->preconnected
             ? CURL_SOCKOPT_ALREADY_CONNECTED
             : CURL_SOCKOPT_OK;
}

bool same_ip(const sockaddr_storage& ss, const std::string& ip) {
  if (ip.empty()) return false;
  char buf[INET6_ADDRSTRLEN] = {};
  const void* src = nullptr;
  if (ss.ss_family == AF_INET)
    src = &reinterpret_cast<const sockaddr_in*>(&ss)->sin_addr;
  else if (ss.ss_family == AF_INET6)
    src = &reinterpret_cast<const sockaddr_in6*>(&ss)->sin6_addr;
  else
    return false;
  if (!inet_ntop(ss.ss_family, src, buf, sizeof(buf))) return false;
  return ip == buf;
}

const char* version_string(long v) {
  switch (v) {
    case CURL_HTTP_VERSION_1_0: return "HTTP/1.0";
    case CURL_HTTP_VERSION_1_1: return "HTTP/1.1";
    case CURL_HTTP_VERSION_2_0: return "HTTP/2";
    case CURL_HTTP_VERSION_3: return "HTTP/3";
    default: return "";
  }
}

// One hop's phases in microseconds; consecutive, so they add up to the hop's duration.
struct Phases {
  curl_off_t queue = 0, dns = 0, socket = 0, connect = 0, tls = 0, send = 0, ttfb = 0,
             download = 0;
};

// f: the hop's own marks; dur: hop duration; hop_start: the hop's time zero on our clock.
Phases split_hop(const Marks& f, curl_off_t dur, bool reused, Clock::time_point hop_start,
                 const TransferCtx& ctx, const std::string& ip) {
  // Clamp the marks into a monotonic sequence (a skipped phase collapses to 0).
  const curl_off_t q = f.q;
  curl_off_t n = std::max(q, f.n);
  curl_off_t c = std::max(n, f.c);
  curl_off_t sock = n;
  if (!reused) {
    // Winning socket of this hop: the one opened to the address curl reports (else the last).
    const SocketOpen* win = nullptr;
    const auto hop_end = hop_start + std::chrono::microseconds(dur);
    for (size_t i = 0; i < ctx.n_opens; ++i) {
      const SocketOpen& o = ctx.opens[i];
      if (o.at < hop_start || o.at > hop_end) continue;
      if (!win || same_ip(o.addr, ip) || !same_ip(win->addr, ip)) win = &o;
    }
    if (win) {
      auto rel = [&](Clock::time_point tp) {
        return static_cast<curl_off_t>(
            std::chrono::duration<double, std::micro>(tp - hop_start).count());
      };
      // When curl starts connecting before name resolution is fully done (happy eyeballs on
      // the first answer), DNS ends when the winning socket was opened instead of swallowing
      // the TCP handshake.
      const curl_off_t at = rel(win->at);
      if (at >= q && at < n) n = at;
      // Socket Initialization is creating + configuring the winning socket; time spent on
      // addresses that failed first (::1 before 127.0.0.1) stays in connect. A connect that
      // never completed (refused, timed out) still had its socket set up: it ends at the hop.
      const curl_off_t sock_end = f.c > 0 ? c : std::max(n, dur);
      sock = n + std::clamp<curl_off_t>(rel(win->ready) - at, 0, sock_end - n);
      c = std::max(c, sock);
    }
  }
  const curl_off_t a = std::max(c, f.a);
  const curl_off_t p = std::max(a, f.p);
  const curl_off_t pt = std::max(p, f.pt);
  const curl_off_t st = std::max(pt, f.st);
  const curl_off_t end = std::max(dur, st);
  Phases ph;
  ph.queue = q;
  ph.dns = n - q;
  ph.socket = sock - n;
  ph.connect = c - sock;
  ph.tls = a - c;
  ph.send = pt - a;  // protocol setup (pretransfer) + request upload
  ph.ttfb = st - pt;
  ph.download = end - st;
  return ph;
}

double us_ms(curl_off_t v) { return static_cast<double>(v) / 1000.0; }

curl_off_t us_between(Clock::time_point a, Clock::time_point b) {
  return static_cast<curl_off_t>(std::chrono::duration<double, std::micro>(b - a).count());
}

// The final response's head as HeadFn hands it over (see http_client.h): what the header
// callback has parsed, the connection as read at pre-request time, and the phases up to the
// first response byte, split like send() splits them (download and process are still running).
void fire_head(TransferCtx* ctx) {
  const auto now = Clock::now();
  CURL* e = ctx->easy;
  const HttpResponse& r = *ctx->resp;
  HttpResponse h;
  h.ok = true;
  h.status = ctx->hop_status;
  h.status_text = r.status_text;
  h.http_version = ctx->hop_version;
  h.headers = r.headers;
  char* s = nullptr;
  if (curl_easy_getinfo(e, CURLINFO_EFFECTIVE_URL, &s) == CURLE_OK && s) h.effective_url = s;
  s = nullptr;
  if (curl_easy_getinfo(e, CURLINFO_PRIMARY_IP, &s) == CURLE_OK && s) h.remote_ip = s;
  curl_easy_getinfo(e, CURLINFO_PRIMARY_PORT, &h.remote_port);
  curl_off_t cid = -1;
  curl_easy_getinfo(e, CURLINFO_CONN_ID, &cid);
  if (const ConnEntry* ce = ctx->conns.find(cid)) {
    h.conn = ce->info;
    h.local_ip = ce->info.local_ip;
    h.local_port = ce->info.local_port;
    if (const TlsInfo* t = ce->info.tls_info.get()) {
      h.tls_version = t->version;
      h.tls_cipher = t->cipher;
      h.cert_subject = t->leaf.subject;
      h.cert_san = ce->cert_san;
      h.cert_issuer = t->leaf.issuer;
      h.cert_not_after = ce->cert_not_after;
      h.cert_chain = t->chain;
      h.verify_result = t->verify_result;
    }
  }
  h.conn.client_cert = ctx->client_cert;
  h.redirect_count = static_cast<long>(ctx->hops.size());
  h.sizes.response_headers = ctx->hop_header_bytes;
  // Every earlier hop is closed (its successor's status line closed it): R is where this one
  // started on curl's clock.
  Marks base;
  curl_off_t R = 0;
  if (!ctx->hops.empty()) {
    base = ctx->hops.back().at_end;
    R = std::max<curl_off_t>(ctx->hops.back().redirect_end, 0);
  }
  Marks f = Marks::read(e).minus(base);
  const auto final_start = ctx->t_io + std::chrono::microseconds(R);
  bool opened = false;
  for (size_t i = 0; i < ctx->n_opens && !opened; ++i) opened = ctx->opens[i].at >= final_start;
  const bool reused = f.conns == 0 && !opened;
  // The first byte is here: a first-byte mark curl has not stamped yet is now.
  const curl_off_t so_far = std::max<curl_off_t>(us_between(final_start, now), 0);
  if (f.st <= 0) f.st = so_far;
  const curl_off_t st = std::max({f.q, f.n, f.c, f.a, f.p, f.pt, f.st});
  const Phases ph = split_hop(f, st, reused, final_start, *ctx, h.remote_ip);
  Timing& tm = h.timing;
  tm.connection_reused = reused;
  h.conn.reused = reused;
  tm.queue_ms = us_ms(ph.queue);
  tm.dns_ms = us_ms(ph.dns);
  h.socket_ms = us_ms(ph.socket);
  tm.connect_ms = us_ms(ph.connect);
  tm.tls_ms = us_ms(ph.tls);
  tm.send_ms = us_ms(ph.send);
  tm.ttfb_ms = us_ms(ph.ttfb);
  tm.download_ms = 0;
  h.redirect_ms = us_ms(R);
  h.prepare_ms = ms_between(ctx->t0, ctx->t_io);
  tm.bytes_sent = static_cast<uint64_t>(Marks::read(e).req);
  tm.bytes_received = ctx->hop_header_bytes;
  tm.total_ms = h.prepare_ms + h.redirect_ms + us_ms(ph.queue + ph.dns + ph.socket + ph.connect + ph.tls + ph.send + ph.ttfb);
  try {
    (*ctx->on_head)(h);
  } catch (...) {  // never let an exception unwind through libcurl's C frames
  }
}

constexpr const char* kUserAgent = "watt/" WATT_VERSION;

}  // namespace

std::vector<std::string> pem_certificate_subjects(std::string_view pem) {
  std::vector<std::string> out;
  constexpr std::string_view kBegin = "-----BEGIN CERTIFICATE-----", kEnd = "-----END CERTIFICATE-----";
  for (size_t at = pem.find(kBegin); at != std::string_view::npos; at = pem.find(kBegin, at)) {
    const size_t end = pem.find(kEnd, at);
    if (end == std::string_view::npos) break;
    const std::string block(pem.substr(at, end + kEnd.size() - at));
    at = end + kEnd.size();
    DWORD n = 0;
    if (!CryptStringToBinaryA(block.c_str(), static_cast<DWORD>(block.size()), CRYPT_STRING_BASE64HEADER, nullptr, &n,
                              nullptr, nullptr) || n == 0)
      continue;
    std::string der(n, char{});
    if (!CryptStringToBinaryA(block.c_str(), static_cast<DWORD>(block.size()), CRYPT_STRING_BASE64HEADER,
                              reinterpret_cast<BYTE*>(der.data()), &n, nullptr, nullptr))
      continue;
    PCCERT_CONTEXT c = CertCreateCertificateContext(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                                    reinterpret_cast<const BYTE*>(der.data()), n);
    if (!c) continue;
    std::string name = simple_name(c, false);
    CertFreeCertificateContext(c);
    out.push_back(name.empty() ? "(unnamed certificate)" : std::move(name));
  }
  return out;
}

struct HttpClient::Impl {
  CURL* easy = nullptr;
  CURLM* multi = nullptr;
  CURLSH* share = nullptr;
  std::atomic<bool> cancel_flag{false};
  char errbuf[CURL_ERROR_SIZE] = {};
  TransferCtx ctx;
  curl_slist* hdrs = nullptr;        // request header list, reused while unchanged
  std::vector<Header> hdrs_for;      // the request headers `hdrs` was built from
  bool hdrs_built = false;
  bool fine_timer = false;           // this client holds a 1 ms timeBeginPeriod
  curl_off_t last_conn_id = -1;      // the connection curl used last, and its peer
  std::string last_ip;
  long last_port = 0;

  Impl() {
    ensure_global_init();
    share = curl_share_init();
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
    multi = MultiPool::get().take();
    easy = curl_easy_init();
    ctx.cancel = &cancel_flag;
    ctx.easy = easy;
    ctx.hops.reserve(4);
    if (!easy) return;
    // Options that never change between requests.
    CURL* h = easy;
    curl_easy_setopt(h, CURLOPT_SHARE, share);
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(h, CURLOPT_BUFFERSIZE, 256L * 1024L);
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");  // all decoders curl was built with
    curl_easy_setopt(h, CURLOPT_OPENSOCKETFUNCTION, &on_opensocket);
    curl_easy_setopt(h, CURLOPT_OPENSOCKETDATA, &ctx);
    curl_easy_setopt(h, CURLOPT_SOCKOPTFUNCTION, &on_sockopt);
    curl_easy_setopt(h, CURLOPT_SOCKOPTDATA, &ctx);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, &on_header);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(h, CURLOPT_PREREQFUNCTION, &on_prereq);
    curl_easy_setopt(h, CURLOPT_PREREQDATA, &ctx);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, &on_write);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &ctx);
    // No progress callback: cancel() wakes the transfer loop, which checks the flag after
    // every curl_multi_perform, and the write callback checks it per body chunk. A progress
    // callback would only add calls on every perform.
    curl_easy_setopt(h, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 30L);
    // Only HTTP(S), for the request and for every redirect hop. libcurl is built with other
    // protocols (file://, smb://, ...); a URL or a Location header must never reach them
    // (local file reads, UNC/SMB NTLM credential leaks).
    curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    // Credentials (Basic / Digest / NTLM / Negotiate / SigV4, including implicit Windows SSO)
    // go to the original host only, never to a redirect target on another host or port.
    curl_easy_setopt(h, CURLOPT_UNRESTRICTED_AUTH, 0L);
    // Like curl (curl/x) and Postman (PostmanRuntime/x), every request names its client: some
    // APIs refuse requests without one (api.github.com answers 403). A User-Agent header in the
    // request replaces it; an empty one ("User-Agent" with no value) sends it empty.
    curl_easy_setopt(h, CURLOPT_USERAGENT, kUserAgent);
  }
  ~Impl() {
    if (fine_timer) timeEndPeriod(1);
    if (easy) curl_easy_cleanup(easy);
    MultiPool::get().give(multi);  // the easy handle is never left attached after send()
    if (share) curl_share_cleanup(share);
    curl_slist_free_all(hdrs);
  }

  // Per-request options as last applied to the easy handle.
  struct Applied {
    bool valid = false;
    std::string url;
    long timeout = 0, version = 0, follow = 0;
    long max_redirs = 30, post_redir = 0, unrestricted = 0, as_is = 0, tls_min = CURL_SSLVERSION_DEFAULT;
    bool verify = false, fresh = false;
    std::string ca_pem;
    int kind = 0;  // 0 GET-like, 1 HEAD, 2 with body
    std::string custom;
    HttpRequest::Auth auth = HttpRequest::Auth::None;
    std::string auth_user, auth_password, aws_sigv4;
    std::string cert, key, pass;  // client certificate as given (HttpRequest)
    bool proxy_null = true;       // CURLOPT_PROXY unset (Env): libcurl reads the environment
    std::string proxy, no_proxy;
  } last;
  std::string cert_path;    // CURLOPT_SSLCERT for last.cert / key / pass ("CurrentUser\\watt\\<sha1>")
  std::string cert_error;   // why that client certificate cannot be used ("" = fine)
  std::string proxy_shown;  // the proxy applied for the current request, credentials removed

  // Client certificate (mTLS): converted (PEM) or taken as is (PKCS#12) once per distinct value.
  void apply_cert(const HttpRequest& req, bool all) {
    CURL* h = easy;
    // Only TLS uses it: a plain http:// request never pays for the conversion / store lookup.
    static const std::string kNone;
    const std::string& cert = is_https_url(req.url) ? req.client_cert : kNone;
    if (!all && cert == last.cert && req.client_key == last.key && req.client_key_passphrase == last.pass)
      return;
    if (all && cert.empty()) return;
    last.cert = cert;
    last.key = req.client_key;
    last.pass = req.client_key_passphrase;
    cert_error.clear();
    cert_path = cert.empty() ? std::string() : client_cert_location(req, cert_error);
    curl_easy_setopt(h, CURLOPT_SSLCERT, cert_path.empty() ? nullptr : cert_path.c_str());
  }

  // Proxy: CURLOPT_PROXY / NOPROXY, set only when they change.
  void apply_proxy(const HttpRequest& req, bool all) {
    bool null = false;
    std::string px, np;
    switch (req.proxy) {
      case HttpRequest::Proxy::Env: null = true; break;
      case HttpRequest::Proxy::Off: break;  // "" = never a proxy, environment included
      case HttpRequest::Proxy::Url:
        px = req.proxy_url;
        np = req.no_proxy;
        break;
      case HttpRequest::Proxy::System: {
        const SystemProxy sp = system_proxy();
        px = is_https_url(req.url) ? sp.https : sp.http;
        np = sp.bypass;
        if (!req.no_proxy.empty()) np += "," + req.no_proxy;
        break;
      }
    }
    proxy_shown = null ? std::string() : redact_proxy(px);
    if (!all && null == last.proxy_null && px == last.proxy && np == last.no_proxy) return;
    if (all && null) {
      last.proxy_null = true;
      return;
    }
    curl_easy_setopt(easy, CURLOPT_PROXY, null ? nullptr : px.c_str());
    curl_easy_setopt(easy, CURLOPT_NOPROXY, null || np.empty() ? nullptr : np.c_str());
    last.proxy_null = null;
    last.proxy = std::move(px);
    last.no_proxy = std::move(np);
  }

  // Authentication options: only touched when they differ from the last request, so a request
  // without auth (the common case) costs four comparisons.
  void apply_auth(const HttpRequest& req, bool all) {
    CURL* h = easy;
    const bool sig = !req.aws_sigv4.empty();
    const bool none = req.auth == HttpRequest::Auth::None && !sig;
    if (all && none) return;  // a fresh handle has curl's defaults already
    if (!all && req.auth == last.auth && req.auth_user == last.auth_user && req.auth_password == last.auth_password &&
        req.aws_sigv4 == last.aws_sigv4)
      return;
    long mask = CURLAUTH_BASIC;
    switch (req.auth) {
      case HttpRequest::Auth::None:
      case HttpRequest::Auth::Basic: mask = CURLAUTH_BASIC; break;
      case HttpRequest::Auth::Digest: mask = CURLAUTH_DIGEST; break;
      case HttpRequest::Auth::Ntlm: mask = CURLAUTH_NTLM; break;
      case HttpRequest::Auth::Negotiate: mask = CURLAUTH_NEGOTIATE; break;
    }
    if (sig) mask = CURLAUTH_AWS_SIGV4;
    curl_easy_setopt(h, CURLOPT_HTTPAUTH, mask);
    curl_easy_setopt(h, CURLOPT_AWS_SIGV4, sig ? req.aws_sigv4.c_str() : nullptr);
    if (none) {
      curl_easy_setopt(h, CURLOPT_USERNAME, nullptr);
      curl_easy_setopt(h, CURLOPT_PASSWORD, nullptr);
    } else {
      // Empty strings (not null): with SSPI, NTLM / Negotiate then use the current Windows logon.
      curl_easy_setopt(h, CURLOPT_USERNAME, req.auth_user.c_str());
      curl_easy_setopt(h, CURLOPT_PASSWORD, req.auth_password.c_str());
    }
    last.auth = req.auth;
    last.auth_user = req.auth_user;
    last.auth_password = req.auth_password;
    last.aws_sigv4 = req.aws_sigv4;
  }

  // Redirect limit / method / credentials, URL as written, lowest TLS version: set when changed.
  void apply_redirect_url_tls(const HttpRequest& req, bool all) {
    CURL* h = easy;
    const long redirs = std::clamp<long>(req.max_redirects, 0L, 100L);
    if (all ? redirs != 30L : redirs != last.max_redirs) curl_easy_setopt(h, CURLOPT_MAXREDIRS, redirs);
    last.max_redirs = redirs;
    const long post = req.keep_method_on_redirect ? CURL_REDIR_POST_ALL : 0L;
    if (all ? post != 0 : post != last.post_redir) curl_easy_setopt(h, CURLOPT_POSTREDIR, post);
    last.post_redir = post;
    // Windows SSO (NTLM / Negotiate, SSPI: possibly the current logon) never goes to another host.
    const bool sspi = req.auth == HttpRequest::Auth::Ntlm || req.auth == HttpRequest::Auth::Negotiate;
    const long unres = req.keep_auth_on_redirect && !sspi ? 1L : 0L;
    if (all ? unres != 0 : unres != last.unrestricted) curl_easy_setopt(h, CURLOPT_UNRESTRICTED_AUTH, unres);
    last.unrestricted = unres;
    const long as_is = req.encode_url ? 0L : 1L;
    if (all ? as_is != 0 : as_is != last.as_is) curl_easy_setopt(h, CURLOPT_PATH_AS_IS, as_is);
    last.as_is = as_is;
    long tls = CURL_SSLVERSION_DEFAULT;
    switch (req.tls_min_version) {
      case TlsVersion::Default: tls = CURL_SSLVERSION_DEFAULT; break;
      case TlsVersion::Tls1_0: tls = CURL_SSLVERSION_TLSv1_0; break;
      case TlsVersion::Tls1_1: tls = CURL_SSLVERSION_TLSv1_1; break;
      case TlsVersion::Tls1_2: tls = CURL_SSLVERSION_TLSv1_2; break;
      case TlsVersion::Tls1_3: tls = CURL_SSLVERSION_TLSv1_3; break;
    }
    if (all ? tls != CURL_SSLVERSION_DEFAULT : tls != last.tls_min) curl_easy_setopt(h, CURLOPT_SSLVERSION, tls);
    last.tls_min = tls;
  }

  void apply(const HttpRequest& req) {
    CURL* h = easy;
    const bool all = !last.valid;
    if (all || req.url != last.url) {
      curl_easy_setopt(h, CURLOPT_URL, req.url.c_str());
      last.url = req.url;
    }
    const long timeout = req.timeout_ms > 0 ? req.timeout_ms : 0L;
    if (all || timeout != last.timeout) curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, timeout);
    last.timeout = timeout;

    long ver = CURL_HTTP_VERSION_2TLS;  // h2 offered via ALPN over TLS, HTTP/1.1 in plaintext
    switch (req.version) {
      case HttpVersion::Auto: ver = CURL_HTTP_VERSION_2TLS; break;
      case HttpVersion::Http1_1: ver = CURL_HTTP_VERSION_1_1; break;
      case HttpVersion::Http2: ver = CURL_HTTP_VERSION_2_0; break;
      case HttpVersion::Http2PriorKnowledge: ver = CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE; break;
    }
    if (all || ver != last.version) curl_easy_setopt(h, CURLOPT_HTTP_VERSION, ver);
    last.version = ver;
    if (all || req.verify_tls != last.verify) {
      curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, req.verify_tls ? 1L : 0L);
      curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, req.verify_tls ? 2L : 0L);
    }
    last.verify = req.verify_tls;
    if ((all && !req.ca_pem.empty()) || req.ca_pem != last.ca_pem) {
      if (req.ca_pem.empty()) {
        curl_easy_setopt(h, CURLOPT_CAINFO_BLOB, nullptr);
        curl_easy_setopt(h, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
      } else {
        curl_blob blob{const_cast<char*>(req.ca_pem.data()), req.ca_pem.size(), CURL_BLOB_COPY};
        curl_easy_setopt(h, CURLOPT_CAINFO_BLOB, &blob);
        curl_easy_setopt(h, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NO_REVOKE));
      }
      last.ca_pem = req.ca_pem;
    }
    if (all || req.fresh_connection != last.fresh) {
      curl_easy_setopt(h, CURLOPT_FRESH_CONNECT, req.fresh_connection ? 1L : 0L);
      curl_easy_setopt(h, CURLOPT_SSL_SESSIONID_CACHE, req.fresh_connection ? 0L : 1L);
      curl_easy_setopt(h, CURLOPT_DNS_CACHE_TIMEOUT, req.fresh_connection ? 0L : 60L);
    }
    last.fresh = req.fresh_connection;

    // Method + body. The body is handed to curl by pointer: no copy (so it is set on every
    // request that has one). POSTFIELDS switches curl to POST and HTTPGET back to GET (clearing
    // NOBODY), so the order below gives a clean state.
    const std::string& m = req.method;
    const bool custom = !(m == "GET" || m == "POST" || m == "HEAD");
    const bool send_body = !req.body.empty() || m == "POST" || m == "PUT" || m == "PATCH";
    const int kind = m == "HEAD" ? 1 : send_body ? 2 : 0;
    const bool need_custom = kind == 2 ? m != "POST" : custom;
    if (kind == 2) {
      curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(req.body.size()));
      curl_easy_setopt(h, CURLOPT_POSTFIELDS, req.body.data());
    } else if (all || kind != last.kind) {
      curl_easy_setopt(h, CURLOPT_POSTFIELDS, nullptr);
      curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, curl_off_t{-1});
      curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);
      if (kind == 1) curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
    }
    last.kind = kind;
    const char* want_custom = need_custom ? m.c_str() : "";
    if (all || last.custom != want_custom) {
      curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, need_custom ? m.c_str() : nullptr);
      last.custom = want_custom;
    }
    // OBEYCODE: a custom method is still switched to GET on 301/302/303 like a browser would.
    // keep_method_on_redirect: the method (custom or POST) and body go to every hop.
    const long follow = !req.follow_redirects                                   ? 0L
                        : (custom || send_body) && !req.keep_method_on_redirect ? CURLFOLLOW_OBEYCODE
                                                                                : CURLFOLLOW_ALL;
    if (all || follow != last.follow) curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, follow);
    last.follow = follow;
    apply_redirect_url_tls(req, all);
    apply_auth(req, all);
    apply_cert(req, all);
    apply_proxy(req, all);
    last.valid = true;
  }

  void set_headers(const std::vector<Header>& headers) {
    if (hdrs_built && headers == hdrs_for) return;
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, nullptr);
    curl_slist_free_all(hdrs);
    hdrs = nullptr;
    bool has_expect = false;
    std::string line;
    for (const auto& [k, v] : headers) {
      if (k.empty()) continue;
      if (iequals(k, "expect")) has_expect = true;
      line.clear();
      line.reserve(k.size() + v.size() + 2);
      line.append(k);
      if (v.empty()) {
        line.push_back(';');  // curl syntax for "send this header with an empty value"
      } else {
        line.append(": ");
        line.append(v);
      }
      hdrs = curl_slist_append(hdrs, line.c_str());
    }
    if (!has_expect) hdrs = curl_slist_append(hdrs, "Expect:");  // never stall on 100-continue
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, hdrs);
    hdrs_for = headers;
    hdrs_built = true;
  }
};

HttpClient::HttpClient() : impl_(std::make_unique<Impl>()) {}
HttpClient::~HttpClient() = default;

namespace {

// One throwaway HTTP/1.1 exchange over loopback with a responder thread. The first request in a
// process otherwise pays for loading the Winsock providers (first socket ~0.2 ms), the TCP
// stack's first connect (~0.3 ms) and paging in libcurl's and our own request / response code:
// a cold local request measured 1.15 -> 0.55 ms after it. Its client is destroyed afterwards,
// so no connection, DNS entry or TLS session carries over: real clients still start cold.
void warm_up_transfer() {
  SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (l == INVALID_SOCKET) return;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int alen = sizeof(a);
  if (bind(l, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(l, 1) != 0 ||
      getsockname(l, reinterpret_cast<sockaddr*>(&a), &alen) != 0) {
    closesocket(l);
    return;
  }
  SOCKET v6 = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);  // "localhost" tries ::1 first
  if (v6 != INVALID_SOCKET) closesocket(v6);
  std::thread responder([l] {
    SOCKET c = accept(l, nullptr, nullptr);
    if (c == INVALID_SOCKET) return;
    DWORD tmo = 2000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
    std::string got;
    char buf[2048];
    while (got.find("\r\n\r\n") == std::string::npos) {
      const int n = recv(c, buf, sizeof(buf), 0);
      if (n <= 0) break;
      got.append(buf, static_cast<size_t>(n));
    }
    static constexpr char kReply[] =
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\nok";
    send(c, kReply, static_cast<int>(sizeof(kReply) - 1), 0);
    shutdown(c, SD_SEND);
    closesocket(c);
  });
  {
    HttpClient warm;
    HttpRequest req;
    req.url = "http://127.0.0.1:" + std::to_string(ntohs(a.sin_port)) + "/";
    req.timeout_ms = 2000;
    req.busy_poll_ms = 0;
    (void)warm.send(req);
  }
  closesocket(l);  // unblocks accept() if curl never connected
  responder.join();
}

// crypt32's first name formatting in a process loads its OID tables (~5 ms); the first TLS
// connection's certificate read (read_tls) would pay that. Round-trip a name with the usual
// attributes once instead.
void warm_up_cert_names() {
  BYTE enc[256];
  DWORD n = sizeof enc;
  if (!CertStrToNameW(X509_ASN_ENCODING, L"CN=a, OU=b, O=c, L=d, S=e, C=US", CERT_X500_NAME_STR,
                      nullptr, enc, &n, nullptr))
    return;
  CERT_NAME_BLOB blob{n, enc};
  (void)x500(&blob);
}

}  // namespace

void HttpClient::global_init() {
  static std::once_flag once;
  std::call_once(once, [] {
    // Runs next to the rest (it only touches crypt32), so it adds nothing to the wall time.
    std::thread certs(warm_up_cert_names);
    MultiPool::get().prefill(1);
    // The first timeBeginPeriod in a process costs ~0.05 ms (winmm start-up); later ones ~5 us.
    timeBeginPeriod(1);
    timeEndPeriod(1);
    warm_up_transfer();
    certs.join();
  });
}

bool ResponseHeaders::find(std::string_view name, std::string_view* value) const {
  for (size_t i = 0; i < ranges_.size(); ++i) {
    const HeaderView h = (*this)[i];
    if (iequals(h.first, name)) {
      if (value) *value = h.second;
      return true;
    }
  }
  return false;
}

std::vector<Header> ResponseHeaders::to_vector() const {
  std::vector<Header> out;
  out.reserve(ranges_.size());
  for (const HeaderView h : *this) out.emplace_back(std::string(h.first), std::string(h.second));
  return out;
}

void HttpClient::cancel() {
  impl_->cancel_flag.store(true, std::memory_order_relaxed);
  if (impl_->multi) curl_multi_wakeup(impl_->multi);
}

HttpResponse HttpClient::send(const HttpRequest& req, const BodyChunkFn& on_chunk) {
  return send(req, on_chunk, HeadFn{});
}

HttpResponse HttpClient::send(const HttpRequest& req, const BodyChunkFn& on_chunk, const HeadFn& on_head) {
  const auto t0 = Clock::now();
  HttpResponse resp;
  Impl& im = *impl_;
  im.cancel_flag.store(false, std::memory_order_relaxed);

  if (!im.easy || !im.multi || !im.share) {
    resp.error = "libcurl initialisation failed";
    resp.timing.total_ms = resp.prepare_ms = ms_between(t0, Clock::now());
    return resp;
  }
  // The method goes into the request line verbatim (CURLOPT_CUSTOMREQUEST): it must be an
  // RFC 9110 token, or a saved request could smuggle CR/LF, spaces or a second request line.
  if (!is_http_token(req.method)) {
    resp.error = "invalid HTTP method \"" + req.method +
                 "\": use letters, digits or !#$%&'*+-.^_`|~ only (no spaces or line breaks)";
    resp.timing.total_ms = resp.prepare_ms = ms_between(t0, Clock::now());
    return resp;
  }

  CURL* h = im.easy;
  im.errbuf[0] = '\0';
  TransferCtx& ctx = im.ctx;
  ctx.begin(&resp, &on_chunk, req);
  ctx.on_head = on_head ? &on_head : nullptr;
  ctx.client_cert = !req.client_cert.empty() && is_https_url(req.url);
  ctx.t0 = t0;

  // While a client is in use the process asks for 1 ms timer resolution (as Chromium-based
  // tools do). With Windows' default 15.6 ms tick the CPU drops into deep idle during every
  // server wait, and the work right after the response (and before the next request) runs on
  // a cold, down-clocked core: warm HTTPS re-fires measured 0.198 -> 0.163 ms client-side
  // (p50, 10/10 interleaved process runs). Taken once per client on its first send, so that
  // request pays the call (~5 us after global_init) in prepare_ms; released when the client
  // is destroyed. Toggling it per request costs ~0.13 ms, more than it saves.
  if (!im.fine_timer) im.fine_timer = timeBeginPeriod(1) == TIMERR_NOERROR;
  // Every per-request option gets an explicit value (no curl_easy_reset, see the notes above);
  // curl never changes them itself, so only what differs from the last request is applied.
  im.apply(req);
  if (!im.cert_error.empty()) {
    resp.error = im.cert_error;
    resp.conn.client_cert = true;
    resp.timing.total_ms = resp.prepare_ms = ms_between(t0, Clock::now());
    return resp;
  }
  im.set_headers(req.headers);

  // Drive the transfer. curl starts its clock inside the first curl_multi_perform, so t_io
  // (taken right before it) is curl's time zero on our clock. t_curl is taken as each
  // curl_multi_perform returns: after the last one it is the moment curl handed the finished
  // transfer back, so everything after it is our own result collection (process_ms).
  CURLcode result = CURLE_OK;
  bool cancelled = false;
  std::string multi_error;
  CURLMcode mc = curl_multi_add_handle(im.multi, h);
  const bool added = mc == CURLM_OK;
  const auto t_io = Clock::now();
  ctx.t_io = t_io;
  auto t_curl = t_io;
  // See HttpRequest::busy_poll_ms: for the first busy_poll_ms of the transfer this thread
  // stays on the CPU and re-polls curl instead of sleeping until the network wakes it.
  const bool spin = req.busy_poll_ms > 0;
  const auto spin_until = t_io + std::chrono::milliseconds(spin ? req.busy_poll_ms : 0);
  bool connected = false;
  bool timeout_lifted = false;
  if (!added) {
    multi_error = curl_multi_strerror(mc);
  } else {
    for (;;) {
      int running = 0;
      mc = curl_multi_perform(im.multi, &running);
      t_curl = Clock::now();
      if (mc != CURLM_OK) {
        multi_error = curl_multi_strerror(mc);
        break;
      }
      if (running == 0) break;
      if (im.cancel_flag.load(std::memory_order_relaxed)) {
        cancelled = true;
        break;
      }
      if (ctx.event_stream && !timeout_lifted && req.timeout_ms > 0) {
        // An event stream: no total timeout once its head arrived; it fails only when nothing
        // at all arrives for max(timeout, 5 min) (keep-alive comments count).
        timeout_lifted = true;
        curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 0L);
        curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, std::max(300L, req.timeout_ms / 1000));
        im.last.timeout = -1;  // apply() sets it again for the next request
      }
      // Busy-poll: let curl look at its sockets again shortly instead of sleeping in the
      // kernel; a few pause instructions keep the loop from hammering the socket calls.
      // While a connection is still being set up the loop sleeps in 1 ms polls instead:
      // spinning there now and then left curl blind to a completed loopback connect until
      // its next ~10 ms timer.
      const bool in_window = spin && t_curl < spin_until;
      if (in_window &&
          (connected || (connected = info_off(h, CURLINFO_CONNECT_TIME_T) > 0 ||
                                     info_off(h, CURLINFO_PRETRANSFER_TIME_T) > 0))) {
        for (int k = 0; k < 64; ++k) YieldProcessor();
        continue;
      }
      mc = curl_multi_poll(im.multi, nullptr, 0, in_window ? 1 : 1000, nullptr);
      if (mc != CURLM_OK) {
        t_curl = Clock::now();
        multi_error = curl_multi_strerror(mc);
        break;
      }
    }
    int left = 0;
    while (CURLMsg* msg = curl_multi_info_read(im.multi, &left)) {
      if (msg->msg == CURLMSG_DONE && msg->easy_handle == h) result = msg->data.result;
    }
    curl_multi_remove_handle(im.multi, h);  // curl's marks stay readable after removal
    if (timeout_lifted) {
      curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 0L);
      curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 0L);
    }
  }
  if (im.cancel_flag.load(std::memory_order_relaxed) &&
      (result == CURLE_ABORTED_BY_CALLBACK || result == CURLE_WRITE_ERROR))
    cancelled = true;

  // Collect results.
  if (cancelled) {
    resp.ok = false;
    resp.error = "cancelled";
  } else if (!multi_error.empty()) {
    resp.ok = false;
    resp.error = multi_error;
  } else if (result != CURLE_OK) {
    resp.ok = false;
    resp.error = im.errbuf[0] ? std::string(im.errbuf) : std::string(curl_easy_strerror(result));
  } else {
    resp.ok = true;
  }

  // Status and protocol come from the status line the header callback already parsed; libcurl
  // is asked only when there was none it could parse.
  resp.status = ctx.hop_status;
  if (resp.status == 0) curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &resp.status);
  if (*ctx.hop_version) {
    resp.http_version = ctx.hop_version;
  } else {
    long hv = 0;
    curl_easy_getinfo(h, CURLINFO_HTTP_VERSION, &hv);
    resp.http_version = version_string(hv);
  }
  curl_easy_getinfo(h, CURLINFO_REDIRECT_COUNT, &resp.redirect_count);
  char* s = nullptr;
  if (curl_easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &s) == CURLE_OK && s) resp.effective_url = s;
  // The peer belongs to the connection: re-firing on the same one keeps the last address.
  const bool any_open = ctx.n_opens > 0;
  curl_off_t conn_id = -1;
  curl_easy_getinfo(h, CURLINFO_CONN_ID, &conn_id);
  if (any_open || conn_id < 0 || conn_id != im.last_conn_id) {
    s = nullptr;
    if (curl_easy_getinfo(h, CURLINFO_PRIMARY_IP, &s) == CURLE_OK && s) resp.remote_ip = s;
    curl_easy_getinfo(h, CURLINFO_PRIMARY_PORT, &resp.remote_port);
    im.last_conn_id = conn_id;
    im.last_ip = resp.remote_ip;
    im.last_port = resp.remote_port;
  } else {
    resp.remote_ip = im.last_ip;
    resp.remote_port = im.last_port;
  }
  // Connection details: a copy of what was read when this connection delivered its first
  // response (two short strings and a shared_ptr), plus the mirror fields.
  if (const ConnEntry* ce = ctx.conns.find(conn_id)) {
    resp.conn = ce->info;
    resp.local_ip = ce->info.local_ip;
    resp.local_port = ce->info.local_port;
    if (const TlsInfo* t = ce->info.tls_info.get()) {
      resp.tls_version = t->version;
      resp.tls_cipher = t->cipher;
      resp.cert_subject = t->leaf.subject;
      resp.cert_san = ce->cert_san;
      resp.cert_issuer = t->leaf.issuer;
      resp.cert_not_after = ce->cert_not_after;
      resp.cert_chain = t->chain;
      resp.verify_result = t->verify_result;
    }
  } else if (conn_id >= 0) {
    // curl never got as far as sending the request (refused connect, TLS failure, ...): only
    // what curl kept about the attempt.
    resp.conn.id = conn_id;
    s = nullptr;
    if (curl_easy_getinfo(h, CURLINFO_LOCAL_IP, &s) == CURLE_OK && s) resp.conn.local_ip = s;
    curl_easy_getinfo(h, CURLINFO_LOCAL_PORT, &resp.conn.local_port);
    resp.conn.remote_ip = resp.remote_ip;
    resp.conn.remote_port = resp.remote_port;
    resp.local_ip = resp.conn.local_ip;
    resp.local_port = resp.conn.local_port;
  }
  // Proxy and client certificate (mTLS) of this request, for the connection view.
  resp.conn.client_cert = !req.client_cert.empty() && is_https_url(req.url);
  if (req.proxy != HttpRequest::Proxy::Off) {
    long used = 0;
    if (curl_easy_getinfo(h, CURLINFO_USED_PROXY, &used) == CURLE_OK && used)
      resp.conn.proxy = req.proxy == HttpRequest::Proxy::Env ? redact_proxy(env_proxy(req.url)) : im.proxy_shown;
  }
  if (resp.status != 0) {
    ctx.last_hdr_bytes = std::max<size_t>(resp.headers.text_bytes(), 64);
    ctx.last_hdr_count = std::max<size_t>(resp.headers.size(), 4);
  }
  if (!resp.ok && resp.status == 0) resp.headers.clear();
  if (!resp.ok) resp.body.clear();

  // Redirect hops. A 3xx that curl did not follow (redirect limit) is the final response; a
  // followed one whose successor never produced a status line (refused, timed out) closes now.
  // Without a socket opened in this send() curl has no DNS / connect / TLS marks to give.
  const Marks all = Marks::read(h, any_open || !ctx.hops.empty());
  const auto followed = static_cast<size_t>(std::max<long>(resp.redirect_count, 0));
  if (ctx.hops.size() > followed) {
    ctx.hops.resize(followed);
    ctx.hop_open = false;
  }
  if (ctx.hop_open) ctx.close_hop();
  while (!ctx.hops.empty() && ctx.hops.back().redirect_end < 0) ctx.hops.pop_back();

  // Timeline (one monotonic clock; curl's zero is t_io):
  //   t0 .. t_io            our request setup                        -> prepare_ms
  //   0 .. R                every redirect hop (curl time_redirect)  -> hops / redirect_ms
  //   R + [0 .. q]          curl's own queue for the final hop       -> queue
  //   R + [q .. st]         dns / socket / connect / tls / send / ttfb of the final hop
  //   R + st .. t_curl      first byte -> curl returned the transfer -> download
  //   t_curl .. t_end       result collection                        -> process_ms
  // Each phase is its own measured delta (see split_hop for DNS vs socket vs connect).
  Timing& tm = resp.timing;
  Marks base;
  curl_off_t R = 0, dl_base = 0;
  resp.hops.reserve(ctx.hops.size());
  for (const RawHop& rh : ctx.hops) {
    const Marks d = rh.at_end.minus(base);
    const curl_off_t dur = std::max<curl_off_t>(rh.redirect_end - R, 0);
    const bool reused = d.conns == 0;
    const Phases ph = split_hop(d, dur, reused, t_io + std::chrono::microseconds(R), ctx, rh.ip);
    RedirectHop& hop = resp.hops.emplace_back();
    hop.url = rh.url;
    hop.status = rh.status;
    hop.remote_ip = rh.ip;
    hop.remote_port = rh.port;
    hop.connection_reused = reused;
    hop.queue_ms = us_ms(ph.queue);
    hop.dns_ms = us_ms(ph.dns);
    hop.socket_ms = us_ms(ph.socket);
    hop.connect_ms = us_ms(ph.connect);
    hop.tls_ms = us_ms(ph.tls);
    hop.send_ms = us_ms(ph.send);
    hop.ttfb_ms = us_ms(ph.ttfb);
    hop.download_ms = us_ms(ph.download);
    const curl_off_t hop_us = ph.queue + ph.dns + ph.socket + ph.connect + ph.tls + ph.send +
                              ph.ttfb + ph.download;  // == max(dur, the hop's last mark)
    hop.total_ms = us_ms(hop_us);
    hop.bytes_sent = static_cast<uint64_t>(d.req);
    hop.bytes_received =
        rh.header_bytes + static_cast<uint64_t>(std::max<curl_off_t>(rh.dl_after - dl_base, 0));
    base = rh.at_end;
    R += hop_us;
    dl_base = rh.dl_after;
  }

  Marks f = all.minus(base);
  // The final hop reused a connection when curl made none and no socket was opened for it.
  // That holds for a request that then failed (timeout, cancel, no answer) too, as long as the
  // request went out: a DNS failure opens nothing and sends nothing.
  const auto final_start = t_io + std::chrono::microseconds(R);
  bool opened_final = false;
  for (size_t i = 0; i < ctx.n_opens && !opened_final; ++i)
    opened_final = ctx.opens[i].at >= final_start;
  const bool no_new_conn = f.conns == 0 && !opened_final;
  const bool failed_early = !resp.ok && resp.status == 0;  // no response line at all
  tm.connection_reused = no_new_conn && (resp.status != 0 || f.req > 0);
  resp.conn.reused = tm.connection_reused;
  if (failed_early) {
    // When a transfer fails before it has a response, curl stamps the transfer marks it never
    // reached (pretransfer, posttransfer, first byte) with the failure time; only those it
    // really passed count. Nothing written means it never got to pretransfer.
    f.st = 0;
    if (f.req == 0) f.p = f.pt = 0;
  }
  const curl_off_t st_f = std::max({f.q, f.n, f.c, f.a, f.p, f.pt, f.st});
  const curl_off_t T = std::max(R + st_f, us_between(t_io, t_curl));
  Phases ph = split_hop(f, T - R, no_new_conn, final_start, ctx, resp.remote_ip);
  // The phase that ran until curl handed the transfer back: download, or for a transfer that
  // failed before any response byte the phase that was in progress (refused or timed-out
  // connect, TLS failure, a server that never answers, a reused connection gone silent).
  curl_off_t* last = &ph.download;
  if (failed_early) {
    const bool tls = resp.effective_url.compare(0, 6, "https:") == 0 ||
                     req.url.compare(0, 6, "https:") == 0;
    if (f.req > 0)  // the request went out (all of it, unless the body upload was cut short)
      last = f.up < static_cast<curl_off_t>(req.body.size()) ? &ph.send : &ph.ttfb;
    else if (no_new_conn) last = &ph.dns;  // never got as far as a connection
    else if (f.c == 0) last = &ph.connect;
    else if (tls && f.a == 0) last = &ph.tls;
    else last = &ph.send;
    *last += ph.download;
    ph.download = 0;
  }

  // Sizes of the final request / response, and totals over all hops.
  const curl_off_t up = std::min(f.up, f.req);
  resp.sizes.request_body = static_cast<uint64_t>(up);
  resp.sizes.request_headers = static_cast<uint64_t>(f.req - up);
  resp.sizes.response_headers = ctx.hop_header_bytes;
  const curl_off_t dl_all = info_off(h, CURLINFO_SIZE_DOWNLOAD_T);
  resp.sizes.response_body_wire = static_cast<uint64_t>(std::max<curl_off_t>(dl_all - dl_base, 0));
  resp.sizes.response_body = ctx.hop_body_bytes;
  tm.bytes_sent = static_cast<uint64_t>(all.req);
  tm.bytes_received = static_cast<uint64_t>(info_long(h, CURLINFO_HEADER_SIZE) + dl_all);

  resp.redirect_ms = us_ms(R);
  tm.queue_ms = us_ms(ph.queue);
  tm.dns_ms = us_ms(ph.dns);
  resp.socket_ms = us_ms(ph.socket);
  tm.connect_ms = us_ms(ph.connect);
  tm.tls_ms = us_ms(ph.tls);
  tm.send_ms = us_ms(ph.send);
  tm.ttfb_ms = us_ms(ph.ttfb);
  tm.download_ms = us_ms(ph.download);
  // curl's marks are whole microseconds; the phase that ran until t_curl also gets the
  // sub-microsecond rest, so curl's phases cover exactly t_io .. t_curl.
  double* last_ms = last == &ph.download  ? &tm.download_ms
                    : last == &ph.ttfb    ? &tm.ttfb_ms
                    : last == &ph.send    ? &tm.send_ms
                    : last == &ph.tls     ? &tm.tls_ms
                    : last == &ph.connect ? &tm.connect_ms
                                          : &tm.dns_ms;
  *last_ms += ms_between(t_io, t_curl) - us_ms(T);
  double overshoot = 0;  // curl's last mark landed a hair after t_curl (its clock rounds up)
  if (*last_ms < 0) {
    overshoot = -*last_ms;
    *last_ms = 0;
  }

  const auto t_end = Clock::now();
  tm.total_ms = ms_between(t0, t_end);
  resp.prepare_ms = ms_between(t0, t_io);
  resp.process_ms = std::max(0.0, ms_between(t_curl, t_end) - overshoot);
  return resp;
}

}  // namespace watt
