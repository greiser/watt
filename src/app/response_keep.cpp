#include "app/response_keep.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

#include "app/history_summary.h"
#include "core/model_json.h"

namespace watt::app::rv {

namespace fs = std::filesystem;

namespace {

using W = rapidjson::Writer<rapidjson::StringBuffer>;
using V = rapidjson::Value;

void key(W& w, const char* k) { w.Key(k); }
void put(W& w, const char* k, std::string_view v) {
  w.Key(k);
  w.String(v.data(), static_cast<rapidjson::SizeType>(v.size()));
}
void put(W& w, const char* k, double v) {
  w.Key(k);
  w.Double(std::isfinite(v) ? v : 0.0);
}
void put_i(W& w, const char* k, long long v) {
  w.Key(k);
  w.Int64(v);
}
void put_u(W& w, const char* k, uint64_t v) {
  w.Key(k);
  w.Uint64(v);
}
void put_b(W& w, const char* k, bool v) {
  w.Key(k);
  w.Bool(v);
}

std::string str(const V& o, const char* k) {
  const auto it = o.FindMember(k);
  return it != o.MemberEnd() && it->value.IsString() ? std::string(it->value.GetString(), it->value.GetStringLength())
                                                     : std::string();
}
double num(const V& o, const char* k) {
  const auto it = o.FindMember(k);
  return it != o.MemberEnd() && it->value.IsNumber() ? it->value.GetDouble() : 0.0;
}
long long i64(const V& o, const char* k) {
  const auto it = o.FindMember(k);
  return it != o.MemberEnd() && it->value.IsInt64() ? it->value.GetInt64() : 0;
}
uint64_t u64(const V& o, const char* k) {
  const auto it = o.FindMember(k);
  return it != o.MemberEnd() && it->value.IsUint64() ? it->value.GetUint64() : 0;
}
bool flag(const V& o, const char* k) {
  const auto it = o.FindMember(k);
  return it != o.MemberEnd() && it->value.IsBool() && it->value.GetBool();
}
const V* obj(const V& o, const char* k) {
  const auto it = o.FindMember(k);
  return it != o.MemberEnd() && it->value.IsObject() ? &it->value : nullptr;
}
const V* arr(const V& o, const char* k) {
  const auto it = o.FindMember(k);
  return it != o.MemberEnd() && it->value.IsArray() ? &it->value : nullptr;
}

// ------------------------------------------------------------------ scripts

void put_script(W& w, const char* k, const ScriptRun& r) {
  key(w, k);
  w.StartObject();
  put_b(w, "ran", r.ran);
  put_b(w, "untrusted", r.untrusted);
  put_b(w, "ok", r.ok);
  put(w, "error", r.error);
  put_i(w, "line", r.line);
  put_i(w, "column", r.column);
  key(w, "logs");
  w.StartArray();
  for (const ScriptLog& l : r.logs) {
    w.StartArray();
    w.Int(static_cast<int>(l.level));
    w.String(l.text.data(), static_cast<rapidjson::SizeType>(l.text.size()));
    w.EndArray();
  }
  w.EndArray();
  put_b(w, "logs_truncated", r.logs_truncated);
  key(w, "tests");
  w.StartArray();
  for (const ScriptTest& t : r.tests) {
    w.StartObject();
    put(w, "name", t.name);
    put_b(w, "pass", t.pass);
    put(w, "message", t.message);
    w.EndObject();
  }
  w.EndArray();
  // Variable writes: names and outcome only. Values may be secrets (tokens a script fetched);
  // the Log shows them, the disk does not need to.
  key(w, "vars");
  w.StartArray();
  for (const ScriptVar& v : r.vars_set) {
    w.StartObject();
    put_i(w, "scope", static_cast<int>(v.scope));
    put(w, "name", v.name);
    put_b(w, "removed", v.removed);
    put_b(w, "ok", v.ok);
    put(w, "message", v.message);
    w.EndObject();
  }
  w.EndArray();
  put_b(w, "request_changed", r.request_changed);
  put_i(w, "requests_sent", r.requests_sent);
  put(w, "elapsed_us", r.elapsed_us);
  put(w, "setup_us", r.setup_us);
  put(w, "wait_us", r.wait_us);
  w.EndObject();
}

std::shared_ptr<const ScriptRun> get_script(const V& o) {
  auto r = std::make_shared<ScriptRun>();
  r->ran = flag(o, "ran");
  r->untrusted = flag(o, "untrusted");
  r->ok = o.HasMember("ok") ? flag(o, "ok") : true;
  r->error = str(o, "error");
  r->line = static_cast<int>(i64(o, "line"));
  r->column = static_cast<int>(i64(o, "column"));
  if (const V* a = arr(o, "logs"))
    for (const V& l : a->GetArray())
      if (l.IsArray() && l.Size() == 2 && l[0].IsInt() && l[1].IsString()) {
        ScriptLog x;
        x.level = static_cast<ScriptLog::Level>(std::clamp(l[0].GetInt(), 0, 3));
        x.text.assign(l[1].GetString(), l[1].GetStringLength());
        r->logs.push_back(std::move(x));
      }
  r->logs_truncated = flag(o, "logs_truncated");
  if (const V* a = arr(o, "tests"))
    for (const V& t : a->GetArray())
      if (t.IsObject()) r->tests.push_back({str(t, "name"), flag(t, "pass"), str(t, "message")});
  if (const V* a = arr(o, "vars"))
    for (const V& v : a->GetArray())
      if (v.IsObject()) {
        ScriptVar x;
        x.scope = static_cast<ScriptVar::Scope>(std::clamp(static_cast<int>(i64(v, "scope")), 0, 2));
        x.name = str(v, "name");
        x.value = "(value not kept)";
        x.removed = flag(v, "removed");
        x.ok = flag(v, "ok");
        x.message = str(v, "message");
        r->vars_set.push_back(std::move(x));
      }
  r->request_changed = flag(o, "request_changed");
  r->requests_sent = static_cast<int>(i64(o, "requests_sent"));
  r->elapsed_us = num(o, "elapsed_us");
  r->setup_us = num(o, "setup_us");
  r->wait_us = num(o, "wait_us");
  return r;
}

// ------------------------------------------------------------------ response head

void put_head(W& w, const HttpResponse& h) {
  key(w, "head");
  w.StartObject();
  put_b(w, "ok", h.ok);
  put(w, "error", h.error);
  put_i(w, "status", h.status);
  put(w, "status_text", h.status_text);
  put(w, "http_version", h.http_version);
  // Headers: the first 64 KB (a few KB in practice).
  key(w, "headers");
  w.StartArray();
  size_t bytes = 0;
  for (const auto hv : h.headers) {
    bytes += hv.first.size() + hv.second.size();
    if (bytes > (64u << 10)) break;
    w.StartArray();
    w.String(hv.first.data(), static_cast<rapidjson::SizeType>(hv.first.size()));
    w.String(hv.second.data(), static_cast<rapidjson::SizeType>(hv.second.size()));
    w.EndArray();
  }
  w.EndArray();
  put(w, "effective_url", h.effective_url);
  put(w, "remote_ip", h.remote_ip);
  put_i(w, "remote_port", h.remote_port);
  put(w, "local_ip", h.local_ip);
  put_i(w, "local_port", h.local_port);
  put_i(w, "redirect_count", h.redirect_count);
  put(w, "prepare_ms", h.prepare_ms);
  put(w, "socket_ms", h.socket_ms);
  put(w, "redirect_ms", h.redirect_ms);
  put(w, "process_ms", h.process_ms);
  const Timing& t = h.timing;
  key(w, "timing");
  w.StartObject();
  put(w, "queue", t.queue_ms);
  put(w, "dns", t.dns_ms);
  put(w, "connect", t.connect_ms);
  put(w, "tls", t.tls_ms);
  put(w, "send", t.send_ms);
  put(w, "ttfb", t.ttfb_ms);
  put(w, "download", t.download_ms);
  put(w, "total", t.total_ms);
  put_b(w, "reused", t.connection_reused);
  put_u(w, "sent", t.bytes_sent);
  put_u(w, "received", t.bytes_received);
  w.EndObject();
  key(w, "sizes");
  w.StartArray();
  for (uint64_t v : {h.sizes.request_headers, h.sizes.request_body, h.sizes.response_headers,
                     h.sizes.response_body_wire, h.sizes.response_body})
    w.Uint64(v);
  w.EndArray();
  key(w, "hops");
  w.StartArray();
  for (const RedirectHop& x : h.hops) {
    w.StartObject();
    put(w, "url", x.url);
    put_i(w, "status", x.status);
    put(w, "remote_ip", x.remote_ip);
    put_i(w, "remote_port", x.remote_port);
    put_b(w, "reused", x.connection_reused);
    put(w, "queue", x.queue_ms);
    put(w, "dns", x.dns_ms);
    put(w, "socket", x.socket_ms);
    put(w, "connect", x.connect_ms);
    put(w, "tls", x.tls_ms);
    put(w, "send", x.send_ms);
    put(w, "ttfb", x.ttfb_ms);
    put(w, "download", x.download_ms);
    put(w, "total", x.total_ms);
    put_u(w, "sent", x.bytes_sent);
    put_u(w, "received", x.bytes_received);
    w.EndObject();
  }
  w.EndArray();
  const ConnectionInfo& c = h.conn;
  key(w, "conn");
  w.StartObject();
  put_i(w, "id", c.id);
  put_b(w, "reused", c.reused);
  put(w, "local_ip", c.local_ip);
  put_i(w, "local_port", c.local_port);
  put(w, "remote_ip", c.remote_ip);
  put_i(w, "remote_port", c.remote_port);
  put_b(w, "tls", c.tls);
  if (!c.proxy.empty()) put(w, "proxy", c.proxy);
  if (c.client_cert) put_b(w, "client_cert", true);
  if (const TlsInfo* s = c.tls_info.get()) {
    key(w, "tls_info");
    w.StartObject();
    put(w, "version", s->version);
    put(w, "cipher", s->cipher);
    put(w, "alpn", s->alpn);
    put(w, "verify", s->verify_result);
    put_i(w, "chain_length", s->chain_length);
    put(w, "chain", s->chain);
    put(w, "read_ms", s->read_ms);
    if (!s->client_cert_subject.empty()) {
      put(w, "client_subject", s->client_cert_subject);
      put(w, "client_issuer", s->client_cert_issuer);
    }
    const TlsCertificate& l = s->leaf;
    key(w, "leaf");
    w.StartObject();
    put(w, "subject", l.subject);
    put(w, "subject_cn", l.subject_cn);
    put(w, "issuer", l.issuer);
    put(w, "issuer_cn", l.issuer_cn);
    key(w, "san");
    w.StartArray();
    for (const std::string& n : l.subject_alt_names) w.String(n.data(), static_cast<rapidjson::SizeType>(n.size()));
    w.EndArray();
    put(w, "not_before", l.not_before);
    put(w, "not_after", l.not_after);
    put(w, "serial", l.serial);
    put(w, "sha256", l.sha256_fingerprint);
    w.EndObject();
    w.EndObject();
  }
  w.EndObject();
  // The flat mirrors that are not derived from conn.tls_info by restore_response().
  put(w, "cert_not_after", h.cert_not_after);
  w.EndObject();
}

std::shared_ptr<HttpResponse> get_head(const V& o) {
  auto h = std::make_shared<HttpResponse>();
  h->ok = flag(o, "ok");
  h->error = str(o, "error");
  h->status = static_cast<long>(i64(o, "status"));
  h->status_text = str(o, "status_text");
  h->http_version = str(o, "http_version");
  if (const V* a = arr(o, "headers"))
    for (const V& p : a->GetArray())
      if (p.IsArray() && p.Size() == 2 && p[0].IsString() && p[1].IsString())
        h->headers.add(std::string_view(p[0].GetString(), p[0].GetStringLength()),
                       std::string_view(p[1].GetString(), p[1].GetStringLength()));
  h->effective_url = str(o, "effective_url");
  h->remote_ip = str(o, "remote_ip");
  h->remote_port = static_cast<long>(i64(o, "remote_port"));
  h->local_ip = str(o, "local_ip");
  h->local_port = static_cast<long>(i64(o, "local_port"));
  h->redirect_count = static_cast<long>(i64(o, "redirect_count"));
  h->prepare_ms = num(o, "prepare_ms");
  h->socket_ms = num(o, "socket_ms");
  h->redirect_ms = num(o, "redirect_ms");
  h->process_ms = num(o, "process_ms");
  if (const V* t = obj(o, "timing")) {
    Timing& x = h->timing;
    x.queue_ms = num(*t, "queue");
    x.dns_ms = num(*t, "dns");
    x.connect_ms = num(*t, "connect");
    x.tls_ms = num(*t, "tls");
    x.send_ms = num(*t, "send");
    x.ttfb_ms = num(*t, "ttfb");
    x.download_ms = num(*t, "download");
    x.total_ms = num(*t, "total");
    x.connection_reused = flag(*t, "reused");
    x.bytes_sent = u64(*t, "sent");
    x.bytes_received = u64(*t, "received");
  }
  if (const V* z = arr(o, "sizes"); z && z->Size() == 5) {
    auto at = [&](unsigned i) { return (*z)[i].IsUint64() ? (*z)[i].GetUint64() : 0; };
    h->sizes = {at(0), at(1), at(2), at(3), at(4)};
  }
  if (const V* a = arr(o, "hops"))
    for (const V& x : a->GetArray()) {
      if (!x.IsObject()) continue;
      RedirectHop p;
      p.url = str(x, "url");
      p.status = static_cast<long>(i64(x, "status"));
      p.remote_ip = str(x, "remote_ip");
      p.remote_port = static_cast<long>(i64(x, "remote_port"));
      p.connection_reused = flag(x, "reused");
      p.queue_ms = num(x, "queue");
      p.dns_ms = num(x, "dns");
      p.socket_ms = num(x, "socket");
      p.connect_ms = num(x, "connect");
      p.tls_ms = num(x, "tls");
      p.send_ms = num(x, "send");
      p.ttfb_ms = num(x, "ttfb");
      p.download_ms = num(x, "download");
      p.total_ms = num(x, "total");
      p.bytes_sent = u64(x, "sent");
      p.bytes_received = u64(x, "received");
      h->hops.push_back(std::move(p));
    }
  if (const V* c = obj(o, "conn")) {
    ConnectionInfo& x = h->conn;
    x.id = i64(*c, "id");
    x.reused = flag(*c, "reused");
    x.local_ip = str(*c, "local_ip");
    x.local_port = static_cast<long>(i64(*c, "local_port"));
    x.remote_ip = str(*c, "remote_ip");
    x.remote_port = static_cast<long>(i64(*c, "remote_port"));
    x.tls = flag(*c, "tls");
    x.proxy = str(*c, "proxy");
    x.client_cert = flag(*c, "client_cert");
    if (const V* s = obj(*c, "tls_info")) {
      auto t = std::make_shared<TlsInfo>();
      t->version = str(*s, "version");
      t->cipher = str(*s, "cipher");
      t->alpn = str(*s, "alpn");
      t->verify_result = str(*s, "verify");
      t->chain_length = static_cast<int>(i64(*s, "chain_length"));
      t->chain = str(*s, "chain");
      t->read_ms = num(*s, "read_ms");
      t->client_cert_subject = str(*s, "client_subject");
      t->client_cert_issuer = str(*s, "client_issuer");
      if (const V* l = obj(*s, "leaf")) {
        TlsCertificate& x2 = t->leaf;
        x2.subject = str(*l, "subject");
        x2.subject_cn = str(*l, "subject_cn");
        x2.issuer = str(*l, "issuer");
        x2.issuer_cn = str(*l, "issuer_cn");
        if (const V* sa = arr(*l, "san"))
          for (const V& n : sa->GetArray())
            if (n.IsString()) x2.subject_alt_names.emplace_back(n.GetString(), n.GetStringLength());
        x2.not_before = str(*l, "not_before");
        x2.not_after = str(*l, "not_after");
        x2.serial = str(*l, "serial");
        x2.sha256_fingerprint = str(*l, "sha256");
      }
      x.tls_info = std::move(t);
    }
  }
  h->cert_not_after = str(o, "cert_not_after");
  return h;
}

// ------------------------------------------------------------------ store

struct Cache {
  std::mutex mu;
  std::map<std::string, RunExtras> m;
};
Cache& cache() {
  static Cache c;
  return c;
}

fs::path runs_dir(const fs::path& root) { return root / ".watt" / "response-runs"; }

// Only hex keys reach the file system (run_key's output).
bool safe_key(const std::string& k) {
  if (k.empty() || k.size() > 32) return false;
  for (char c : k)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

// Keeps the newest kMaxFiles files.
void prune(const fs::path& dir) {
  constexpr size_t kMaxFiles = 1500;
  std::error_code ec;
  std::vector<std::pair<fs::file_time_type, fs::path>> files;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    files.emplace_back(it->last_write_time(ec), it->path());
  }
  if (files.size() <= kMaxFiles) return;
  std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  for (size_t i = kMaxFiles; i < files.size(); ++i) fs::remove(files[i].second, ec);
}

}  // namespace

std::string run_key(const std::string& summary, long status, double raw_total_ms) {
  const long long us = std::llround(raw_total_ms * 1000.0);
  char nums[48];
  const int n = std::snprintf(nums, sizeof nums, "\n%ld\n%lld", status, us);
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](std::string_view s) {
    for (unsigned char c : s) {
      h ^= c;
      h *= 1099511628211ull;
    }
  };
  mix(summary);
  mix(std::string_view(nums, n > 0 ? static_cast<size_t>(n) : 0));
  static const char hex[] = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i, h >>= 4) out[static_cast<size_t>(i)] = hex[h & 15];
  return out;
}

std::shared_ptr<const HttpResponse> response_head(const HttpResponse& r) {
  auto h = std::make_shared<HttpResponse>();
  h->ok = r.ok;
  h->error = r.error;
  h->status = r.status;
  h->status_text = r.status_text;
  h->http_version = r.http_version;
  h->headers = r.headers;
  h->effective_url = r.effective_url;
  h->remote_ip = r.remote_ip;
  h->remote_port = r.remote_port;
  h->local_ip = r.local_ip;
  h->local_port = r.local_port;
  h->tls_version = r.tls_version;
  h->tls_cipher = r.tls_cipher;
  h->cert_subject = r.cert_subject;
  h->cert_san = r.cert_san;
  h->cert_issuer = r.cert_issuer;
  h->cert_not_after = r.cert_not_after;
  h->cert_chain = r.cert_chain;
  h->verify_result = r.verify_result;
  h->timing = r.timing;
  h->redirect_count = r.redirect_count;
  h->sizes = r.sizes;
  h->hops = r.hops;
  h->prepare_ms = r.prepare_ms;
  h->socket_ms = r.socket_ms;
  h->redirect_ms = r.redirect_ms;
  h->process_ms = r.process_ms;
  h->conn = r.conn;
  return h;
}

std::string extras_to_json(const RunExtras& x) {
  rapidjson::StringBuffer sb;
  W w(sb);
  w.StartObject();
  put_i(w, "v", 1);
  put(w, "prompt_ms", x.prompt_ms);
  put_b(w, "untrusted", x.scripts_skipped_untrusted);
  put_u(w, "body_size", x.body_size);
  if (x.pre) put_script(w, "pre", *x.pre);
  if (x.post) put_script(w, "post", *x.post);
  if (x.head) put_head(w, *x.head);
  w.EndObject();
  return std::string(sb.GetString(), sb.GetSize());
}

std::optional<RunExtras> extras_from_json(const std::string& json) {
  rapidjson::Document d;
  d.Parse(json.data(), json.size());
  if (d.HasParseError() || !d.IsObject() || i64(d, "v") != 1) return std::nullopt;
  RunExtras x;
  x.prompt_ms = std::max(0.0, num(d, "prompt_ms"));
  x.scripts_skipped_untrusted = flag(d, "untrusted");
  x.body_size = u64(d, "body_size");
  if (const V* p = obj(d, "pre")) x.pre = get_script(*p);
  if (const V* p = obj(d, "post")) x.post = get_script(*p);
  if (const V* h = obj(d, "head")) x.head = get_head(*h);
  return x;
}

std::shared_ptr<HttpResponse> restore_response(const HttpResponse* kept, const RunExtras& x) {
  std::shared_ptr<HttpResponse> r;
  if (kept) {
    r = std::make_shared<HttpResponse>(*kept);
    if (const HttpResponse* h = x.head.get()) {
      // What the history leaves out; the kept copy's own fields (its capped body, a note in the
      // status text) stay.
      r->conn = h->conn;
      r->hops = h->hops;
      r->local_ip = h->local_ip;
      r->local_port = h->local_port;
      r->cert_not_after = h->cert_not_after;
      if (r->redirect_count == 0) r->redirect_count = h->redirect_count;
      // The history notes a capped body in the status text ("OK (history keeps the first ...)"):
      // the view says that itself, from the sizes as received.
      r->status_text = h->status_text;
      r->sizes = h->sizes;
    }
  } else if (x.head) {
    r = std::make_shared<HttpResponse>(*x.head);
  } else {
    return nullptr;
  }
  if (const TlsInfo* t = r->conn.tls_info.get()) {
    r->tls_version = t->version;
    r->tls_cipher = t->cipher;
    r->cert_subject = t->leaf.subject;
    r->cert_issuer = t->leaf.issuer;
    r->cert_chain = t->chain;
    r->verify_result = t->verify_result;
    r->cert_san.clear();
    for (const std::string& n : t->leaf.subject_alt_names) r->cert_san += (r->cert_san.empty() ? "" : ", ") + n;
  }
  r->conn.reused = r->timing.connection_reused;
  if (x.prompt_ms > 0 && x.prompt_ms <= r->timing.queue_ms) {
    r->timing.queue_ms -= x.prompt_ms;
    r->timing.total_ms -= x.prompt_ms;
  }
  return r;
}

void keep_extras(const fs::path& root, const std::string& key, RunExtras x) {
  if (!safe_key(key)) return;
  {
    Cache& c = cache();
    std::lock_guard lock(c.mu);
    if (c.m.size() > 512) c.m.clear();
    c.m[key] = x;
  }
  if (root.empty()) return;
  static std::atomic<unsigned> writes{0};
  const bool do_prune = (writes++ % 64) == 0;
  std::thread([root, key, x = std::move(x), do_prune] {
    const std::string json = extras_to_json(x);
    const fs::path dir = runs_dir(root);
    detail::ensure_dir(dir);
    detail::write_file_atomic(dir / (key + ".json"), json, nullptr, true);
    if (do_prune) prune(dir);
  }).detach();
}

std::optional<RunExtras> find_extras_cached(const std::string& key) {
  Cache& c = cache();
  std::lock_guard lock(c.mu);
  const auto it = c.m.find(key);
  if (it == c.m.end()) return std::nullopt;
  return it->second;
}

void keep_runs_of(const Workspace& ws, const SavedRequest& q, const std::string& summary,
                  std::vector<HistoryEntry>& h) {
  std::vector<HistoryEntry> mine;
  int looked = 0;
  for (HistoryEntry& e : h) {
    if (e.kind != q.kind || history_template(e) != summary) continue;
    if (++looked > 48) break;
    const std::optional<SavedRequest> r = ws.history_request(e);
    if (!r) continue;
    const bool same = q.id.empty() ? r->id.empty() && !q.name.empty() && r->name == q.name : r->id == q.id;
    if (same) mine.push_back(std::move(e));
  }
  h = std::move(mine);
}

std::optional<RunExtras> find_extras(const fs::path& root, const std::string& key) {
  if (auto x = find_extras_cached(key)) return x;
  if (root.empty() || !safe_key(key)) return std::nullopt;
  std::string json;
  if (!detail::read_file(runs_dir(root) / (key + ".json"), json) || json.empty()) return std::nullopt;
  auto x = extras_from_json(json);
  if (x) {
    Cache& c = cache();
    std::lock_guard lock(c.mu);
    c.m.emplace(key, *x);
  }
  return x;
}

}  // namespace watt::app::rv
