#include "core/model_json.h"

#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "core/model_secure.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace watt::detail {

namespace {

void put_scalar(std::string& out, const rapidjson::Value& v) {
  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> w(sb);
  v.Accept(w);
  out.append(sb.GetString(), sb.GetSize());
}

void put_key(std::string& out, const rapidjson::Value& k) {
  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> w(sb);
  w.String(k.GetString(), k.GetStringLength());
  out.append(sb.GetString(), sb.GetSize());
}

void indent(std::string& out, int depth) { out.append(static_cast<size_t>(depth) * 2, ' '); }

bool is_scalar(const rapidjson::Value& v) { return !v.IsObject() && !v.IsArray(); }

void pretty(std::string& out, const rapidjson::Value& v, int depth) {
  if (v.IsObject()) {
    if (v.MemberCount() == 0) {
      out += "{}";
      return;
    }
    out += "{\n";
    bool first = true;
    for (auto it = v.MemberBegin(); it != v.MemberEnd(); ++it) {
      if (!first) out += ",\n";
      first = false;
      indent(out, depth + 1);
      put_key(out, it->name);
      out += ": ";
      pretty(out, it->value, depth + 1);
    }
    out += '\n';
    indent(out, depth);
    out += '}';
  } else if (v.IsArray()) {
    if (v.Empty()) {
      out += "[]";
      return;
    }
    // [k, v], or [k, v, false] (a disabled header) on one line.
    bool inline_pair = v.Size() <= 2 || (v.Size() == 3 && v[2].IsBool());
    for (auto& e : v.GetArray()) inline_pair = inline_pair && is_scalar(e);
    if (inline_pair) {
      out += '[';
      for (rapidjson::SizeType i = 0; i < v.Size(); ++i) {
        if (i) out += ", ";
        put_scalar(out, v[i]);
      }
      out += ']';
      return;
    }
    // Leading commas (still JSON): appending or removing the last element (a header, a form
    // part, a body line) is a one-line git diff, as in .bru files.
    out += "[\n";
    for (rapidjson::SizeType i = 0; i < v.Size(); ++i) {
      if (i) {
        out += '\n';
        indent(out, depth);
        out += ", ";
      } else {
        indent(out, depth + 1);
      }
      pretty(out, v[i], depth + 1);
    }
    out += '\n';
    indent(out, depth);
    out += ']';
  } else {
    put_scalar(out, v);
  }
}

}  // namespace

std::string json_compact(const rapidjson::Value& v) {
  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> w(sb);
  v.Accept(w);
  return std::string(sb.GetString(), sb.GetSize());
}

std::string json_pretty(const rapidjson::Value& v) {
  std::string out;
  out.reserve(1024);
  pretty(out, v, 0);
  out += '\n';
  return out;
}

bool read_file(const fs::path& p, std::string& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  f.seekg(0, std::ios::end);
  const auto size = f.tellg();
  if (size < 0) return false;
  f.seekg(0, std::ios::beg);
  out.resize(static_cast<size_t>(size));
  if (size > 0) f.read(out.data(), static_cast<std::streamsize>(size));
  return static_cast<bool>(f) || f.eof();
}

bool stat_path(const fs::path& p, PathStat& st) {
  st = PathStat{};
#ifdef _WIN32
  // Attributes, size and time in one call that opens nothing (no antivirus scan, no handle).
  WIN32_FILE_ATTRIBUTE_DATA a;
  if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return false;
  st.exists = true;
  st.is_link = (a.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
  st.is_dir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  st.size = (static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
  st.mtime = static_cast<int64_t>((static_cast<uint64_t>(a.ftLastWriteTime.dwHighDateTime) << 32) |
                                  a.ftLastWriteTime.dwLowDateTime);
#else
  struct stat s;
  if (lstat(p.c_str(), &s) != 0) return false;
  st.exists = true;
  st.is_link = S_ISLNK(s.st_mode);
  st.is_dir = S_ISDIR(s.st_mode);
  st.size = static_cast<uint64_t>(s.st_size);
  st.mtime = static_cast<int64_t>(s.st_mtim.tv_sec) * 1000000000 + s.st_mtim.tv_nsec;
#endif
  return true;
}

void ensure_dir(const fs::path& dir) {
  std::error_code ec;
  if (dir.empty() || fs::is_directory(dir, ec)) return;
  // <ws>/.watt (secrets, state, allowlist, history) is created owner-only; what is created
  // inside it inherits that (Windows DACL) or sits behind its 0700 (POSIX).
  fs::path priv;
  for (const auto& c : dir) {
    priv /= c;
    if (c == ".watt") break;
  }
  if (priv.filename() == ".watt" && !fs::is_directory(priv, ec)) {
    ensure_dir(priv.parent_path());
    create_private_dir(priv);
  }
  fs::create_directories(dir, ec);
}

bool read_file_head(const fs::path& p, std::string& out, size_t n) {
#ifdef _WIN32
  // Plain Win32: no stream buffer or locale per open (tree() reads 2000 of these).
  HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  out.resize(n);
  DWORD got = 0;
  const BOOL ok = ReadFile(h, out.data(), static_cast<DWORD>(n), &got, nullptr);
  CloseHandle(h);
  out.resize(ok ? got : 0);
  return ok != FALSE;
#else
  std::FILE* f = std::fopen(p.c_str(), "rb");
  if (!f) return false;
  out.resize(n);
  out.resize(std::fread(out.data(), 1, n, f));
  std::fclose(f);
  return true;
#endif
}

bool append_file(const fs::path& p, const std::string& data, uint64_t* new_size) {
#ifdef _WIN32
  HANDLE h = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const BOOL ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
  LARGE_INTEGER size{};
  if (new_size && GetFileSizeEx(h, &size)) *new_size = static_cast<uint64_t>(size.QuadPart);
  CloseHandle(h);
  return ok && written == data.size();
#else
  const int fd = ::open(p.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, is_private_path(p) ? 0600 : 0666);
  if (fd < 0) return false;
  const bool ok = ::write(fd, data.data(), data.size()) == static_cast<ssize_t>(data.size());
  struct stat st {};
  if (new_size && fstat(fd, &st) == 0) *new_size = static_cast<uint64_t>(st.st_size);
  ::close(fd);
  return ok;
#endif
}

namespace {

// Creates or truncates p and writes data. Files under .watt are created 0600 on POSIX (on
// Windows they inherit the directory's owner-only DACL).
bool write_whole(const fs::path& p, const std::string& data, bool priv) {
#ifdef _WIN32
  (void)priv;  // the owner-only DACL comes from the directory
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (f) {
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    f.close();
  }
  return static_cast<bool>(f);
#else
  const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                        priv || is_private_path(p) ? 0600 : 0666);
  if (fd < 0) return false;
  size_t done = 0;
  while (done < data.size()) {
    const ssize_t w = ::write(fd, data.data() + done, data.size() - done);
    if (w <= 0) break;
    done += static_cast<size_t>(w);
  }
  return ::close(fd) == 0 && done == data.size();
#endif
}

}  // namespace

bool write_file_atomic(const fs::path& p, const std::string& data, std::string* err, bool private_file) {
  fs::path tmp = p;
  tmp += ".tmp~";
  if (!write_whole(tmp, data, private_file)) {
    std::error_code ec;
    fs::remove(tmp, ec);
    if (err) *err = "cannot write " + to_utf8(tmp);
    return false;
  }
  std::error_code ec;
  fs::rename(tmp, p, ec);
  if (!ec) return true;
  // Target locked (e.g. open in an editor that blocks rename): fall back to an in-place write.
  fs::remove(tmp, ec);
  if (!write_whole(p, data, private_file)) {
    if (err) *err = "cannot write " + to_utf8(p);
    return false;
  }
  return true;
}

std::string base64(const std::string& in) {
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const uint32_t v = (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8) | static_cast<uint8_t>(in[i + 2]);
    out += tbl[(v >> 18) & 63];
    out += tbl[(v >> 12) & 63];
    out += tbl[(v >> 6) & 63];
    out += tbl[v & 63];
  }
  if (i + 1 == in.size()) {
    const uint32_t v = static_cast<uint8_t>(in[i]) << 16;
    out += tbl[(v >> 18) & 63];
    out += tbl[(v >> 12) & 63];
    out += "==";
  } else if (i + 2 == in.size()) {
    const uint32_t v = (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8);
    out += tbl[(v >> 18) & 63];
    out += tbl[(v >> 12) & 63];
    out += tbl[(v >> 6) & 63];
    out += '=';
  }
  return out;
}

bool base64_decode(const std::string& in, std::string& out) {
  out.clear();
  uint32_t acc = 0;
  int bits = 0;
  for (char c : in) {
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+') v = 62;
    else if (c == '/') v = 63;
    else if (c == '=') break;
    else return false;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += static_cast<char>((acc >> bits) & 0xFF);
    }
  }
  return true;
}

bool valid_utf8(const std::string& s) {
  const auto* p = reinterpret_cast<const unsigned char*>(s.data());
  const size_t n = s.size();
  for (size_t i = 0; i < n;) {
    const unsigned char c = p[i];
    if (c < 0x80) {
      ++i;
      continue;
    }
    size_t len;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) len = 2, cp = c & 0x1F;
    else if ((c & 0xF0) == 0xE0) len = 3, cp = c & 0x0F;
    else if ((c & 0xF8) == 0xF0) len = 4, cp = c & 0x07;
    else return false;
    if (i + len > n) return false;
    for (size_t k = 1; k < len; ++k) {
      if ((p[i + k] & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (p[i + k] & 0x3F);
    }
    // Overlong forms, UTF-16 surrogates and values past U+10FFFF are not UTF-8.
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF))
      return false;
    i += len;
  }
  return true;
}

}  // namespace watt::detail
