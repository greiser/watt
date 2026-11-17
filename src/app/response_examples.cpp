#include "app/response_examples.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>

namespace watt::app::rv {
namespace fs = std::filesystem;

namespace {

constexpr uintmax_t kMaxExampleBytes = 80ull << 20;

std::string clean(const std::string& s, size_t max) {
  std::string o;
  for (char c : s) {
    if (o.size() >= max) break;
    const auto u = static_cast<unsigned char>(c);
    if (std::isalnum(u) || c == '-' || c == '_') o += c;
    else if (c == ' ' || c == '.') o += '_';
  }
  return o;
}

using JWriter = rapidjson::Writer<rapidjson::StringBuffer>;
void put(JWriter& w, const char* k, const std::string& v) {
  w.Key(k);
  w.String(v.data(), static_cast<rapidjson::SizeType>(v.size()));
}
void put(JWriter& w, const char* k, double v) {
  w.Key(k);
  w.Double(v);
}
std::string str(const rapidjson::Value& o, const char* k) {
  if (!o.IsObject()) return {};
  const auto it = o.FindMember(k);
  return it != o.MemberEnd() && it->value.IsString() ? std::string(it->value.GetString(), it->value.GetStringLength())
                                                     : std::string();
}
double num(const rapidjson::Value& o, const char* k) {
  if (!o.IsObject()) return 0;
  const auto it = o.FindMember(k);
  return it != o.MemberEnd() && it->value.IsNumber() ? it->value.GetDouble() : 0;
}

}  // namespace

fs::path examples_dir(const fs::path& ws_root, const std::string& request_id) {
  std::string id = clean(request_id, 64);
  if (id.empty() || ws_root.empty()) return {};
  return ws_root / ".watt" / "examples" / id;
}

bool save_example(const fs::path& dir, const std::string& name, const HttpResponse& r, const std::string& method,
                  const std::string& url, std::string* err) {
  try {
    if (dir.empty()) {
      if (err) *err = "the request is not saved in the workspace yet";
      return false;
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
      if (err) *err = "cannot create " + dir.string() + ": " + ec.message();
      return false;
    }
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char stamp[32], when[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);
    std::string slug = clean(name, 60);
    if (slug.empty()) slug = "example";
    fs::path file = dir / (std::string(stamp) + "-" + std::to_string(r.status) + "-" + slug + ".json");
    for (int i = 2; fs::exists(file, ec) && i < 100; ++i)
      file = dir / (std::string(stamp) + "-" + std::to_string(r.status) + "-" + slug + "_" + std::to_string(i) + ".json");

    rapidjson::StringBuffer sb;
    JWriter w(sb);
    w.StartObject();
    w.Key("version");
    w.Int(1);
    put(w, "name", name);
    put(w, "saved_at", std::string(when));
    put(w, "method", method);
    put(w, "url", url);
    w.Key("status");
    w.Int64(r.status);
    put(w, "status_text", r.status_text);
    put(w, "http_version", r.http_version);
    put(w, "effective_url", r.effective_url);
    put(w, "remote_ip", r.remote_ip);
    put(w, "remote_port", static_cast<double>(r.remote_port));
    put(w, "tls_version", r.tls_version);
    put(w, "tls_cipher", r.tls_cipher);
    put(w, "redirect_count", static_cast<double>(r.redirect_count));
    w.Key("headers");
    w.StartArray();
    for (const auto h : r.headers) {
      w.StartArray();
      w.String(h.first.data(), static_cast<rapidjson::SizeType>(h.first.size()));
      w.String(h.second.data(), static_cast<rapidjson::SizeType>(h.second.size()));
      w.EndArray();
    }
    w.EndArray();
    const Timing& t = r.timing;
    w.Key("timing");
    w.StartObject();
    put(w, "prepare", r.prepare_ms);
    put(w, "queue", t.queue_ms);
    put(w, "dns", t.dns_ms);
    put(w, "socket", r.socket_ms);
    put(w, "connect", t.connect_ms);
    put(w, "tls", t.tls_ms);
    put(w, "send", t.send_ms);
    put(w, "ttfb", t.ttfb_ms);
    put(w, "download", t.download_ms);
    put(w, "redirect", r.redirect_ms);
    put(w, "process", r.process_ms);
    put(w, "total", t.total_ms);
    w.Key("reused");
    w.Bool(t.connection_reused);
    put(w, "sent", static_cast<double>(t.bytes_sent));
    put(w, "received", static_cast<double>(t.bytes_received));
    w.EndObject();
    w.Key("sizes");
    w.StartArray();
    for (uint64_t v : {r.sizes.request_headers, r.sizes.request_body, r.sizes.response_headers,
                       r.sizes.response_body_wire, r.sizes.response_body})
      w.Uint64(v);
    w.EndArray();
    put(w, "body", r.body);
    w.EndObject();

    // Written next to the target and renamed into place: a reader never sees half a file.
    const fs::path tmp = file.string() + ".tmp";
    {
      std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
      o.write(sb.GetString(), static_cast<std::streamsize>(sb.GetSize()));
      if (!o) {
        if (err) *err = "cannot write " + tmp.string();
        return false;
      }
    }
    fs::rename(tmp, file, ec);
    if (ec) {
      fs::remove(tmp, ec);
      if (err) *err = "cannot write " + file.string();
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    if (err) *err = e.what();
    return false;
  }
}

std::vector<ExampleEntry> list_examples(const fs::path& dir) {
  std::vector<ExampleEntry> out;
  try {
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return out;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
      if (!it->is_regular_file(ec) || it->is_symlink(ec)) continue;
      const fs::path& p = it->path();
      if (p.extension() != ".json") continue;
      // yyyymmdd-hhmmss-<status>-<name>.json
      const std::string stem = p.stem().string();
      if (stem.size() < 18 || stem[8] != '-' || stem[15] != '-') continue;
      const size_t dash = stem.find('-', 16);
      if (dash == std::string::npos) continue;
      ExampleEntry e;
      e.file = p;
      e.status = std::strtol(stem.substr(16, dash - 16).c_str(), nullptr, 10);
      e.name = stem.substr(dash + 1);
      std::replace(e.name.begin(), e.name.end(), '_', ' ');
      e.saved_at = stem.substr(0, 4) + "-" + stem.substr(4, 2) + "-" + stem.substr(6, 2) + " " + stem.substr(9, 2) + ":" +
                   stem.substr(11, 2) + ":" + stem.substr(13, 2);
      out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const ExampleEntry& a, const ExampleEntry& b) { return a.file > b.file; });
    if (out.size() > 200) out.resize(200);
  } catch (...) {
  }
  return out;
}

std::shared_ptr<HttpResponse> load_example(const fs::path& file, std::string* method, std::string* url,
                                           std::string* err) {
  try {
    std::error_code ec;
    const uintmax_t size = fs::file_size(file, ec);
    if (ec || size > kMaxExampleBytes) {
      if (err) *err = ec ? "cannot read " + file.string() : "example too big";
      return nullptr;
    }
    std::ifstream in(file, std::ios::binary);
    std::string text(static_cast<size_t>(size), '\0');
    in.read(text.data(), static_cast<std::streamsize>(size));
    if (!in) {
      if (err) *err = "cannot read " + file.string();
      return nullptr;
    }
    rapidjson::Document d;
    d.Parse<rapidjson::kParseIterativeFlag>(text.data(), text.size());
    if (d.HasParseError() || !d.IsObject() || num(d, "version") != 1) {
      if (err) *err = "not a watt example: " + file.filename().string();
      return nullptr;
    }
    auto r = std::make_shared<HttpResponse>();
    r->ok = true;
    r->status = static_cast<long>(num(d, "status"));
    r->status_text = str(d, "status_text");
    r->http_version = str(d, "http_version");
    r->effective_url = str(d, "effective_url");
    r->remote_ip = r->conn.remote_ip = str(d, "remote_ip");
    r->remote_port = r->conn.remote_port = static_cast<long>(num(d, "remote_port"));
    r->tls_version = str(d, "tls_version");
    r->tls_cipher = str(d, "tls_cipher");
    r->redirect_count = static_cast<long>(num(d, "redirect_count"));
    if (d.HasMember("headers") && d["headers"].IsArray())
      for (const auto& h : d["headers"].GetArray())
        if (h.IsArray() && h.Size() == 2 && h[0].IsString() && h[1].IsString())
          r->headers.add(std::string_view(h[0].GetString(), h[0].GetStringLength()),
                         std::string_view(h[1].GetString(), h[1].GetStringLength()));
    if (d.HasMember("timing")) {
      const auto& t = d["timing"];
      r->prepare_ms = num(t, "prepare");
      r->timing.queue_ms = num(t, "queue");
      r->timing.dns_ms = num(t, "dns");
      r->socket_ms = num(t, "socket");
      r->timing.connect_ms = num(t, "connect");
      r->timing.tls_ms = num(t, "tls");
      r->timing.send_ms = num(t, "send");
      r->timing.ttfb_ms = num(t, "ttfb");
      r->timing.download_ms = num(t, "download");
      r->redirect_ms = num(t, "redirect");
      r->process_ms = num(t, "process");
      r->timing.total_ms = num(t, "total");
      r->timing.connection_reused = t.IsObject() && t.HasMember("reused") && t["reused"].IsBool() && t["reused"].GetBool();
      r->timing.bytes_sent = static_cast<uint64_t>(num(t, "sent"));
      r->timing.bytes_received = static_cast<uint64_t>(num(t, "received"));
      r->conn.reused = r->timing.connection_reused;
    }
    if (d.HasMember("sizes") && d["sizes"].IsArray() && d["sizes"].Size() == 5) {
      const auto& z = d["sizes"];
      auto at = [&](unsigned i) { return z[i].IsUint64() ? z[i].GetUint64() : 0; };
      r->sizes = {at(0), at(1), at(2), at(3), at(4)};
    }
    r->body = str(d, "body");
    if (method) *method = str(d, "method");
    if (url) *url = str(d, "url");
    return r;
  } catch (const std::exception& e) {
    if (err) *err = e.what();
    return nullptr;
  }
}

}  // namespace watt::app::rv
