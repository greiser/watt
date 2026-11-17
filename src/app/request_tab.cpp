#include "app/request_tab.h"

#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/splitter.h>
#include <wx/utils.h>
#include <wx/wupdlock.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <atomic>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include "app/history_summary.h"
#include "app/main_frame.h"        // startup::
#include "core/oauth2.h"
#include "app/response_widgets.h"  // rv::Theme (the placeholder's colour)

#include <windows.h>

namespace watt::app {

namespace {

// A tab opened from a history row is named "Protected @ 15:57" (the snapshot's send time). A
// fresh send from it shows a new response: the old time is dropped so the title never names a
// run that is not the one on screen. True when the name changed.
bool drop_snapshot_time(SavedRequest& q) {
  if (q.file.empty() || q.file.parent_path().filename() != "snapshots") return false;
  std::string& n = q.name;
  const size_t at = n.rfind(" @ ");
  if (at == std::string::npos || n.size() < at + 8 || n[n.size() - 3] != ':' ||
      !std::isdigit(static_cast<unsigned char>(n.back())))
    return false;
  n.resize(at);
  return true;
}

// Workspace::append_history from several tab workers: serialize the appends.
std::mutex& history_mutex() {
  static std::mutex m;
  return m;
}

std::string now_iso() {
  const auto t = std::chrono::system_clock::now();
  const std::time_t tt = std::chrono::system_clock::to_time_t(t);
  std::tm tm{};
  gmtime_s(&tm, &tt);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
  return s;
}

// A client / bidi body given as a JSON array: one message per element.
std::vector<std::string> split_messages(const std::string& body) {
  const auto t = trim(body);
  if (t.empty() || t.front() != '[') return {std::string(t)};
  rapidjson::Document d;
  d.Parse(t.data(), t.size());
  if (d.HasParseError() || !d.IsArray()) return {std::string(t)};
  std::vector<std::string> out;
  out.reserve(d.Size());
  for (auto& v : d.GetArray()) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    v.Accept(w);
    out.emplace_back(sb.GetString(), sb.GetSize());
  }
  return out;
}

// Whether sending r needs Workspace::http_to_send ({{vars}}, form parts, a body file, basic
// auth, disabled headers). Most re-fires need none of it, and skipping it saves ~0.2-0.4 ms.
bool needs_workspace(const SavedRequest& r) {
  // Any file reference must go through http_to_send: it is the only place the upload policy
  // (Workspace::blocked_files) is enforced and the only place these files are read.
  if (!r.form.empty() || !r.body_file.empty() || !r.body_data.empty() || !r.header_files.empty() ||
      !r.cookie_files.empty() || r.basic_auth.enabled || !r.disabled_headers.empty())
    return true;
  // CA / client certificate / proxy files are read (and checked) there too.
  if (!r.network.empty()) return true;
  // Digest/NTLM/Negotiate/SigV4/OAuth2 are applied (and the Windows-logon check enforced) only
  // in http_to_send; scripts may change the request or set the variables it uses.
  if (r.auth.type != HttpAuth::Type::None || !r.pre_script.empty() || !r.post_script.empty()) return true;
  auto has_var = [](const std::string& s) { return s.find("{{") != std::string::npos; };
  if (has_var(r.http.url) || has_var(r.http.body)) return true;
  for (const auto& [k, v] : r.http.headers)
    if (has_var(k) || has_var(v)) return true;
  return false;
}

// Asks once per OS environment variable the request would read from outside the workspace
// (Workspace::blocked_env_vars). Expansion never resolves a non-allowed one either way; this is
// the consent prompt so the request is not sent with a literal {{process.env.X}} instead.
bool confirm_env_vars(const Workspace& ws, const SavedRequest& r, wxWindow* parent, std::string* err) {
  for (const auto& name : ws.blocked_env_vars(r)) {
    const auto ans = wxMessageBox(
        wxString::Format("This request reads the environment variable\n\n%s\n\nfrom your Windows "
                         "environment, outside the workspace. Send its value with this request?",
                         wxString::FromUTF8(name)),
        "watt", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, parent);
    std::string e;
    if (ans != wxYES || !ws.allow_env_var(name, &e)) {
      *err = e.empty() ? "Environment variable not allowed: " + name : e;
      return false;
    }
  }
  return true;
}

// One workspace-sourced network item (Workspace::blocked_network_settings): says exactly what is
// trusted and why it matters. Default No.
bool confirm_network_item(const Workspace::NetworkItem& it, wxWindow* parent) {
  using K = Workspace::NetworkItem::Kind;
  wxString msg;
  const wxString where = wxString::FromUTF8(it.level);
  if (!it.file_blocked.empty()) {
    wxString files;
    for (const auto& f : it.file_blocked) files += wxString("    ") + wxString(f.wstring()) + "\n";
    msg = wxString::Format("The workspace settings (%s) want watt to read\n\n%s\nfrom outside the workspace as a %s. "
                           "Allow reading it?",
                           where, files, it.kind == K::Ca ? "CA certificate" : "client certificate / key");
  } else if (it.kind == K::Proxy) {
    msg = wxString::Format("The workspace settings (%s) send all requests in this workspace through the proxy\n\n"
                           "    %s%s\n\nThe proxy sees every plain HTTP request, including Authorization headers "
                           "and secrets, and every host you connect to. Only allow a proxy you trust. Allow it for "
                           "this workspace?",
                           where, wxString::FromUTF8(it.proxy_origin),
                           it.proxy_credentials ? "\n    (the proxy URL carries credentials)" : "");
  } else if (it.kind == K::Ca) {
    wxString who;
    for (const auto& s : it.subjects) who += "    " + wxString::FromUTF8(s) + "\n";
    if (who.empty()) who = "    (no certificate found in the file)\n";
    msg = wxString::Format("The workspace settings (%s) make watt trust certificates signed by\n\n%s\n"
                           "    file %s\n    SHA-256 %s\n\nWhoever holds this CA's key can read and change all "
                           "HTTPS traffic of this workspace, including Authorization headers and secrets. Only "
                           "allow a CA you know. Trust it for this workspace?",
                           where, who, it.file.wstring(), wxString::FromUTF8(it.fingerprint));
  } else {
    msg = wxString::Format("The workspace settings (%s) present the client certificate\n\n    %s%s\n\nto servers "
                           "that ask for one (it identifies you to them). Allow it for this workspace?",
                           where, it.file.wstring(),
                           it.key_file.empty() ? wxString() : wxString("\n    key ") + wxString(it.key_file.wstring()));
  }
  return wxMessageBox(msg, "watt", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, parent) == wxYES;
}

bool grpc_has_vars(const SavedRequest& r) {
  auto has_var = [](const std::string& s) { return s.find("{{") != std::string::npos; };
  if (has_var(r.grpc_target.address) || has_var(r.grpc_target.authority) || has_var(r.grpc_method) ||
      has_var(r.grpc_body))
    return true;
  for (const auto* md : {&r.grpc_opts.metadata, &r.grpc_opts.rpc_metadata, &r.grpc_opts.reflect_metadata})
    for (const auto& [k, v] : *md)
      if (has_var(k) || has_var(v)) return true;
  return false;
}

// gRPC-Web sent to a native gRPC (HTTP/2) port: the server answers with an HTTP/2 frame that
// libcurl reads as "HTTP/0.9". Said in plain words, with the likely gRPC-Web port.
std::string web_port_hint(const GrpcTarget& t, const std::string& error) {
  if (!t.web || error.find("HTTP/0.9") == std::string::npos) return {};
  std::string hint = ": " + t.address +
                     " looks like a native gRPC (HTTP/2) port, not a gRPC-Web endpoint. Turn gRPC-Web off to call it "
                     "directly, or use the server's gRPC-Web address";
  const size_t colon = t.address.rfind(':');
  if (colon != std::string::npos && t.address.find(']', colon) == std::string::npos) {
    const std::string port = t.address.substr(colon + 1);
    if (!port.empty() && port.size() <= 5 && port.find_first_not_of("0123456789") == std::string::npos) {
      const int p = std::stoi(port);
      if (p > 0 && p < 65535)
        hint += " (a proxy such as Envoy, or watt_testserver's " + t.address.substr(0, colon + 1) +
                std::to_string(p + 1) + ")";
    }
  }
  return hint + ".";
}

std::string url_tail(const std::string& url) {
  auto p = url.find("://");
  std::string s = p == std::string::npos ? url : url.substr(p + 3);
  if (s.size() > 40) s = "..." + s.substr(s.size() - 37);
  return s;
}

}  // namespace

// ------------------------------------------------------------------ last response

namespace {
using JWriter = rapidjson::Writer<rapidjson::StringBuffer>;

void put(JWriter& w, const char* k, const std::string& v) {
  w.Key(k);
  w.String(v.data(), static_cast<rapidjson::SizeType>(v.size()));
}
void put(JWriter& w, const char* k, double v) {
  w.Key(k);
  w.Double(v);
}
void put_timing(JWriter& w, const Timing& t) {
  w.Key("timing");
  w.StartObject();
  put(w, "queue", t.queue_ms);
  put(w, "dns", t.dns_ms);
  put(w, "connect", t.connect_ms);
  put(w, "tls", t.tls_ms);
  put(w, "send", t.send_ms);
  put(w, "ttfb", t.ttfb_ms);
  put(w, "download", t.download_ms);
  put(w, "total", t.total_ms);
  w.Key("reused");
  w.Bool(t.connection_reused);
  w.Key("sent");
  w.Uint64(t.bytes_sent);
  w.Key("received");
  w.Uint64(t.bytes_received);
  w.EndObject();
}
template <class Pairs>
void put_pairs(JWriter& w, const char* k, const Pairs& pairs) {
  w.Key(k);
  w.StartArray();
  for (const auto& [n, v] : pairs) {
    w.StartArray();
    w.String(n.data(), static_cast<rapidjson::SizeType>(n.size()));
    w.String(v.data(), static_cast<rapidjson::SizeType>(v.size()));
    w.EndArray();
  }
  w.EndArray();
}

std::string str(const rapidjson::Value& o, const char* k) {
  return o.HasMember(k) && o[k].IsString() ? std::string(o[k].GetString(), o[k].GetStringLength()) : std::string();
}
double num(const rapidjson::Value& o, const char* k) { return o.HasMember(k) && o[k].IsNumber() ? o[k].GetDouble() : 0; }
bool flag(const rapidjson::Value& o, const char* k) { return o.HasMember(k) && o[k].IsBool() && o[k].GetBool(); }
uint64_t u64(const rapidjson::Value& o, const char* k) { return o.HasMember(k) && o[k].IsUint64() ? o[k].GetUint64() : 0; }
Timing get_timing(const rapidjson::Value& o) {
  Timing t;
  if (!o.HasMember("timing") || !o["timing"].IsObject()) return t;
  const auto& v = o["timing"];
  t.queue_ms = num(v, "queue");
  t.dns_ms = num(v, "dns");
  t.connect_ms = num(v, "connect");
  t.tls_ms = num(v, "tls");
  t.send_ms = num(v, "send");
  t.ttfb_ms = num(v, "ttfb");
  t.download_ms = num(v, "download");
  t.total_ms = num(v, "total");
  t.connection_reused = flag(v, "reused");
  t.bytes_sent = u64(v, "sent");
  t.bytes_received = u64(v, "received");
  return t;
}
template <class F>
void get_pairs(const rapidjson::Value& o, const char* k, F&& add) {
  if (!o.HasMember(k) || !o[k].IsArray()) return;
  for (const auto& p : o[k].GetArray())
    if (p.IsArray() && p.Size() == 2 && p[0].IsString() && p[1].IsString())
      add(std::string(p[0].GetString(), p[0].GetStringLength()), std::string(p[1].GetString(), p[1].GetStringLength()));
}
}  // namespace

std::string last_response_to_json(const LastResponse& r) {
  rapidjson::StringBuffer sb;
  JWriter w(sb);
  w.StartObject();
  if (const HttpResponse* h = r.http.get()) {
    if (h->body.size() > LastResponse::kMaxBytes) return {};
    put(w, "kind", std::string("http"));
    w.Key("ok");
    w.Bool(h->ok);
    put(w, "error", h->error);
    w.Key("status");
    w.Int64(h->status);
    put(w, "status_text", h->status_text);
    put(w, "http_version", h->http_version);
    w.Key("headers");
    w.StartArray();
    for (const auto hv : h->headers) {
      w.StartArray();
      w.String(hv.first.data(), static_cast<rapidjson::SizeType>(hv.first.size()));
      w.String(hv.second.data(), static_cast<rapidjson::SizeType>(hv.second.size()));
      w.EndArray();
    }
    w.EndArray();
    put(w, "body", h->body);
    put(w, "effective_url", h->effective_url);
    put(w, "remote_ip", h->remote_ip);
    put(w, "remote_port", static_cast<double>(h->remote_port));
    put(w, "redirect_count", static_cast<double>(h->redirect_count));
    put(w, "prepare_ms", h->prepare_ms);
    put(w, "socket_ms", h->socket_ms);
    put(w, "redirect_ms", h->redirect_ms);
    put(w, "process_ms", h->process_ms);
    put_timing(w, h->timing);
    w.Key("sizes");
    w.StartArray();
    for (uint64_t v : {h->sizes.request_headers, h->sizes.request_body, h->sizes.response_headers,
                       h->sizes.response_body_wire, h->sizes.response_body})
      w.Uint64(v);
    w.EndArray();
    if (r.sent) {
      put(w, "sent_method", r.sent->request.method);
      put(w, "sent_url", r.sent->request.url);
    }
  } else if (const GrpcResponse* g = r.grpc.get()) {
    size_t bytes = 0;
    for (const auto& m : g->messages) bytes += m.json.size();
    if (bytes > LastResponse::kMaxBytes) return {};
    put(w, "kind", std::string("grpc"));
    w.Key("ok");
    w.Bool(g->ok);
    w.Key("code");
    w.Int(g->status_code);
    put(w, "status_name", g->status_name);
    put(w, "status_message", g->status_message);
    put(w, "error_details", g->error_details);
    put_pairs(w, "headers", g->headers);
    put_pairs(w, "trailers", g->trailers);
    w.Key("messages");
    w.StartArray();
    for (const auto& m : g->messages) {
      w.StartObject();
      put(w, "json", m.json);
      put(w, "at_ms", m.at_ms);
      put(w, "wire_bytes", static_cast<double>(m.wire_bytes));
      w.EndObject();
    }
    w.EndArray();
    put(w, "reflect_ms", g->reflect_ms);
    put(w, "peer", g->peer);
    put_timing(w, g->timing);
  } else {
    return {};
  }
  w.EndObject();
  return std::string(sb.GetString(), sb.GetSize());
}

LastResponse last_response_from_json(const std::string& json, const SavedRequest& req) {
  LastResponse out;
  rapidjson::Document d;
  d.Parse(json.data(), json.size());
  if (d.HasParseError() || !d.IsObject()) return out;
  const std::string kind = str(d, "kind");
  if (kind == "http" && req.kind == RequestKind::Http) {
    auto h = std::make_shared<HttpResponse>();
    h->ok = flag(d, "ok");
    h->error = str(d, "error");
    h->status = static_cast<long>(num(d, "status"));
    h->status_text = str(d, "status_text");
    h->http_version = str(d, "http_version");
    get_pairs(d, "headers", [&](const std::string& n, const std::string& v) { h->headers.add(n, v); });
    h->body = str(d, "body");
    h->effective_url = str(d, "effective_url");
    h->remote_ip = h->conn.remote_ip = str(d, "remote_ip");
    h->remote_port = h->conn.remote_port = static_cast<long>(num(d, "remote_port"));
    h->redirect_count = static_cast<long>(num(d, "redirect_count"));
    h->prepare_ms = num(d, "prepare_ms");
    h->socket_ms = num(d, "socket_ms");
    h->redirect_ms = num(d, "redirect_ms");
    h->process_ms = num(d, "process_ms");
    h->timing = get_timing(d);
    h->conn.reused = h->timing.connection_reused;
    if (d.HasMember("sizes") && d["sizes"].IsArray() && d["sizes"].Size() == 5) {
      const auto& z = d["sizes"];
      auto at = [&](unsigned i) { return z[i].IsUint64() ? z[i].GetUint64() : 0; };
      h->sizes = {at(0), at(1), at(2), at(3), at(4)};
    }
    auto sent = std::make_shared<SentHttp>();
    sent->request = req.http;
    if (d.HasMember("sent_url")) {
      sent->request.method = str(d, "sent_method");
      sent->request.url = str(d, "sent_url");
    }
    out.http = std::move(h);
    out.sent = std::move(sent);
  } else if (kind == "grpc" && req.kind == RequestKind::Grpc) {
    auto g = std::make_shared<GrpcResponse>();
    g->ok = flag(d, "ok");
    g->status_code = static_cast<int>(num(d, "code"));
    g->status_name = str(d, "status_name");
    g->status_message = str(d, "status_message");
    g->error_details = str(d, "error_details");
    get_pairs(d, "headers", [&](const std::string& n, const std::string& v) { g->headers.emplace_back(n, v); });
    get_pairs(d, "trailers", [&](const std::string& n, const std::string& v) { g->trailers.emplace_back(n, v); });
    if (d.HasMember("messages") && d["messages"].IsArray())
      for (const auto& m : d["messages"].GetArray()) {
        if (!m.IsObject()) continue;
        GrpcMessageEvent e;
        e.json = str(m, "json");
        e.at_ms = num(m, "at_ms");
        e.wire_bytes = static_cast<size_t>(num(m, "wire_bytes"));
        g->messages.push_back(std::move(e));
      }
    g->reflect_ms = num(d, "reflect_ms");
    g->peer = str(d, "peer");
    g->timing = get_timing(d);
    out.grpc = std::move(g);
  }
  return out;
}

std::string schema_to_json(const GrpcSchema& s) {
  if (!s.ok) return {};
  rapidjson::StringBuffer sb;
  JWriter w(sb);
  w.StartObject();
  put(w, "load_ms", s.load_ms);
  w.Key("methods");
  w.StartArray();
  for (const auto& m : s.methods) {
    w.StartObject();
    put(w, "service", m.service);
    put(w, "method", m.method);
    put(w, "path", m.full_path);
    put(w, "in", m.input_type);
    put(w, "out", m.output_type);
    w.Key("kind");
    w.Int(static_cast<int>(m.kind));
    put(w, "template", m.request_template);
    w.Key("notes");
    w.StartArray();
    for (const auto& n : m.template_notes) w.String(n.data(), static_cast<rapidjson::SizeType>(n.size()));
    w.EndArray();
    w.EndObject();
  }
  w.EndArray();
  w.EndObject();
  return std::string(sb.GetString(), sb.GetSize());
}

std::shared_ptr<const GrpcSchema> schema_from_json(const std::string& json) {
  rapidjson::Document d;
  d.Parse(json.data(), json.size());
  if (d.HasParseError() || !d.IsObject() || !d.HasMember("methods") || !d["methods"].IsArray()) return nullptr;
  auto s = std::make_shared<GrpcSchema>();
  s->ok = true;
  s->load_ms = num(d, "load_ms");
  for (const auto& v : d["methods"].GetArray()) {
    if (!v.IsObject()) continue;
    GrpcMethodInfo m;
    m.service = str(v, "service");
    m.method = str(v, "method");
    m.full_path = str(v, "path");
    m.input_type = str(v, "in");
    m.output_type = str(v, "out");
    const int k = static_cast<int>(num(v, "kind"));
    m.kind = k >= 0 && k <= 3 ? static_cast<GrpcCallKind>(k) : GrpcCallKind::Unary;
    m.request_template = str(v, "template");
    if (v.HasMember("notes") && v["notes"].IsArray())
      for (const auto& n : v["notes"].GetArray())
        if (n.IsString()) m.template_notes.emplace_back(n.GetString(), n.GetStringLength());
    if (!m.full_path.empty()) s->methods.push_back(std::move(m));
  }
  if (s->methods.empty()) return nullptr;
  return s;
}

namespace {
// Where a request's schema comes from: the same server (reflection) or the same file set.
std::string schema_origin(const SavedRequest& r) {
  if (r.proto_files.empty())
    return "r|" + r.grpc_target.address + (r.grpc_target.tls ? "|t" : "") + (r.grpc_target.web ? "|w" : "");
  std::string k = "p";
  for (const auto& f : r.proto_files) k += "|" + f;
  return k;
}
// The last schema that loaded for each origin in this process (UI thread), for the session: a
// restored tab shows its method, call kind and types before its server is asked again.
std::map<std::string, std::shared_ptr<const GrpcSchema>>& known_schemas() {
  static std::map<std::string, std::shared_ptr<const GrpcSchema>> m;
  return m;
}
// HTTP clients shared by every tab. A send borrows an idle one, preferably the one that last
// talked to the same origin, and gives it back after the response: its pooled connection, TLS
// session and DNS entry then serve the next send to that host from any tab (as Postman's and
// Bruno's shared agents do, and as the runner's single client does), instead of every new tab
// paying a fresh TCP + TLS handshake. Sends in flight at the same time each hold their own
// client; at most kIdle stay parked.
class SharedClients {
 public:
  static SharedClients& get() {
    static SharedClients s;
    return s;
  }
  std::unique_ptr<HttpClient> take(const std::string& origin) {
    std::lock_guard lk(m_);
    if (idle_.empty()) return std::make_unique<HttpClient>();
    size_t pick = idle_.size() - 1;  // the most recently used one
    for (size_t i = idle_.size(); i-- > 0;)
      if (idle_[i].first == origin) {
        pick = i;
        break;
      }
    auto c = std::move(idle_[pick].second);
    idle_.erase(idle_.begin() + static_cast<std::ptrdiff_t>(pick));
    return c;
  }
  void give(std::unique_ptr<HttpClient> c, const std::string& origin) {
    std::unique_ptr<HttpClient> drop;
    std::lock_guard lk(m_);
    idle_.emplace_back(origin, std::move(c));
    if (idle_.size() > kIdle) {
      drop = std::move(idle_.front().second);  // destroyed after the lock is released
      idle_.erase(idle_.begin());
    }
  }
  // Start-up: one client ready before the first send.
  void warm() {
    {
      std::lock_guard lk(m_);
      if (!idle_.empty()) return;
    }
    give(std::make_unique<HttpClient>(), std::string());
  }

 private:
  static constexpr size_t kIdle = 6;
  std::mutex m_;
  std::vector<std::pair<std::string, std::unique_ptr<HttpClient>>> idle_;
};

}  // namespace

// ------------------------------------------------------------------ worker

struct TabWorker : std::enable_shared_from_this<TabWorker> {
  std::mutex m;
  std::condition_variable cv;
  std::deque<std::function<void()>> jobs;
  bool stop = false;

  std::mutex post_m;
  RequestTab* tab = nullptr;  // null once the tab is gone

  std::mutex http_m;
  std::unique_ptr<HttpClient> http;  // borrowed from SharedClients for one send (worker thread)

  std::mutex call_m;
  std::shared_ptr<GrpcCall> call;
  std::atomic<uint64_t> cancelled_gen{0};

  std::mutex msg_m;
  std::vector<GrpcMessageEvent> msgs;
  bool msg_posted = false;
  uint64_t msg_gen = 0;  // messages of other generations are dropped

  void start() {
    std::thread([self = shared_from_this()] { self->run(); }).detach();
  }
  void run() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock lk(m);
        cv.wait(lk, [&] { return stop || !jobs.empty(); });
        if (stop) break;
        job = std::move(jobs.front());
        jobs.pop_front();
      }
      job();
    }
    std::lock_guard lk(http_m);
    http.reset();
  }
  void push(std::function<void()> job) {
    {
      std::lock_guard lk(m);
      jobs.push_back(std::move(job));
    }
    cv.notify_one();
  }
  void shutdown() {
    cancel_all();
    {
      std::lock_guard lk(post_m);
      tab = nullptr;
    }
    {
      std::lock_guard lk(m);
      stop = true;
      jobs.clear();
    }
    cv.notify_one();
  }
  // Runs fn(tab) on the UI thread if the tab still exists.
  template <class F>
  void post(F&& fn) {
    std::lock_guard lk(post_m);
    if (!tab) return;
    RequestTab* t = tab;
    t->CallAfter([t, fn = std::forward<F>(fn)]() mutable { fn(*t); });
  }
  // Borrows a shared client for one send; give_back() returns it (see SharedClients).
  HttpClient& lease_client(const std::string& origin) {
    auto c = SharedClients::get().take(origin);
    std::lock_guard lk(http_m);
    http = std::move(c);
    return *http;
  }
  void give_back(const std::string& origin) {
    std::unique_ptr<HttpClient> c;
    {
      std::lock_guard lk(http_m);
      c = std::move(http);
    }
    if (c) SharedClients::get().give(std::move(c), origin);
  }
  void cancel(uint64_t gen) {
    cancelled_gen.store(gen);
    {
      std::lock_guard lk(http_m);
      if (http) http->cancel();
    }
    std::lock_guard lk(call_m);
    if (call) call->cancel();
  }
  void cancel_all() { cancel(UINT64_MAX); }
  bool is_cancelled(uint64_t gen) const {
    const auto c = cancelled_gen.load();
    return c == UINT64_MAX || c >= gen;
  }
  // gRPC message from the engine thread: queue it, post at most one flush per UI turn.
  void on_message(uint64_t gen, const GrpcMessageEvent& e) {
    bool post_now = false;
    {
      std::lock_guard lk(msg_m);
      if (gen != msg_gen) return;
      msgs.push_back(e);
      if (!msg_posted) post_now = msg_posted = true;
    }
    if (post_now) post([gen](RequestTab& t) { t.on_grpc_messages(gen); });
  }
  std::vector<GrpcMessageEvent> take_messages() {
    std::lock_guard lk(msg_m);
    msg_posted = false;
    return std::exchange(msgs, {});
  }
};

int input_quiet_wait(int quiet_ms) {
  LASTINPUTINFO li{sizeof li, 0};
  if (!GetLastInputInfo(&li)) return 0;
  const DWORD since = GetTickCount() - li.dwTime;
  return since >= static_cast<DWORD>(quiet_ms) ? 0 : quiet_ms - static_cast<int>(since) + 20;
}

// ------------------------------------------------------------------ tab

RequestTab::RequestTab(wxWindow* parent, TabContext ctx, SavedRequest req, bool preview, bool lazy)
    : wxPanel(parent, wxID_ANY), ctx_(std::move(ctx)), req_(std::move(req)), preview_(preview) {
  // The worker must exist before ensure_editor(): GrpcEditor::load() calls back into
  // load_schema() right away for a request with an address, and its schema thread posts the
  // result through worker_ (a null one crashed in post_m's lock once reflection returned).
  worker_ = std::make_shared<TabWorker>();
  worker_->tab = this;
  worker_->start();
  resp_timer_.SetOwner(this);
  Bind(wxEVT_TIMER, [this](wxTimerEvent&) { on_resp_timer(); }, resp_timer_.GetId());
  // Both propagate up from the focused control: the first key or click in this tab wakes a
  // parked gRPC engine (park_grpc).
  Bind(wxEVT_CHAR_HOOK, &RequestTab::on_tab_key, this);
  Bind(wxEVT_CHILD_FOCUS, &RequestTab::on_tab_child_focus, this);
  if (!lazy) realize();
}

void RequestTab::on_tab_key(wxKeyEvent& e) {
  e.Skip();
  // Ctrl / Alt chords are the frame's (Ctrl+Tab away from this tab is no intent for it; Ctrl+Enter
  // wakes it through send()).
  if (grpc_parked_ && !(e.GetModifiers() & (wxMOD_CONTROL | wxMOD_ALT))) wake_grpc();
}

void RequestTab::on_tab_child_focus(wxChildFocusEvent& e) {
  e.Skip();
  // A click into the address, the body or the method tree. Focus set by the program (the start
  // tab's address field at the first activation) is no intent; keyboard focus moves come with a
  // key, which on_tab_key has seen.
  if (!grpc_parked_) return;
  const wxMouseState ms = wxGetMouseState();
  if (ms.LeftIsDown() || ms.RightIsDown() || ms.MiddleIsDown()) wake_grpc();
}

void RequestTab::wake_grpc() {
  if (!grpc_parked_) return;
  grpc_parked_ = false;
  if (wxWindow* st = held_status_.get()) {
    held_status_ = nullptr;
    static_cast<wxStaticText*>(st)->SetLabel(wxString::FromUTF8("Loading\xE2\x80\xA6"));
  }
  if (parked_schema_) {
    const auto r = std::move(parked_schema_);
    load_schema_now(*r);  // starts the engine on its thread, then reflects / parses
  } else if (grpc_ && ctx_.grpc) {
    std::thread([g = ctx_.grpc] { g(); }).detach();
  }
}

void RequestTab::realize() {
  if (split_) return;
  wxWindowUpdateLocker lock(this);
  split_ = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                wxSP_LIVE_UPDATE | wxSP_NOBORDER | wxSP_THIN_SASH);
  split_->SetSashGravity(0.5);
  split_->SetMinimumPaneSize(FromDIP(240));
  // The response view is not needed for the first paint: a pane in its colour stands in until the
  // tab's first idle after it is shown (see on_idle, 6-8 ms build), or until something has to be
  // displayed (response()).
  resp_ph_ = new wxPanel(split_, wxID_ANY);
  resp_ph_->SetBackgroundColour(rv::Theme::get().bg);
  // Until the real view is built (it waits for a quiet moment), the pane is never blank: it
  // shows the same hint the empty view shows, or that the last response is on its way.
  resp_ph_->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
    wxPaintDC dc(resp_ph_);
    const auto& th = rv::Theme::get();
    dc.SetBackground(wxBrush(th.bg));
    dc.Clear();
    dc.SetTextForeground(th.dim);
    dc.SetFont(resp_ph_->GetFont().Scaled(1.1f));
    const wxString msg = last_pending_ ? wxString::FromUTF8("Loading the last response\xE2\x80\xA6")
                                       : wxString("Send the request to see the response (Ctrl+Enter)");
    const wxSize ts = dc.GetTextExtent(msg), cs = resp_ph_->GetClientSize();
    dc.DrawText(msg, std::max(0, (cs.x - ts.x) / 2), std::max(0, cs.y / 3 - ts.y / 2));
  });
  resp_ph_->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
    resp_ph_->Refresh(false);
    e.Skip();
  });
  RequestEditorPanel* ed = ensure_editor(req_.kind);
  split_->SplitVertically(ed->window(), resp_ph_);
  auto* sz = new wxBoxSizer(wxVERTICAL);
  sz->Add(split_, 1, wxEXPAND);
  SetSizer(sz);
  // The split stays at 50% until the user drags the sash (SplitVertically before the first size
  // event uses 0, and the first sizes the tab sees while the frame is built are provisional).
  split_->Bind(wxEVT_SPLITTER_SASH_POS_CHANGED, [this](wxSplitterEvent& e) {
    e.Skip();
    user_sash_ = true;
  });
  // wxAuiNotebook sizes every page when the window is resized, the hidden ones too: laying out
  // each tab's editor and response view made one resize step O(tabs) (67-105 ms with 12-25
  // tabs). A hidden tab only notes it; it lays itself out when it is shown again.
  Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
    if (!IsShown()) {
      layout_pending_ = true;
      return;  // no Skip: no Layout() now
    }
    e.Skip();
    if (!user_sash_ && GetClientSize().x > 0) CallAfter([this] { center_sash(); });
  });
  Bind(wxEVT_SHOW, [this](wxShowEvent& e) {
    e.Skip();
    if (!e.IsShown() || !layout_pending_) return;
    layout_pending_ = false;
    Layout();
    if (!user_sash_) center_sash();
  });
  center_sash();  // realized after the page was laid out (a restored tab)
  Bind(wxEVT_IDLE, &RequestTab::on_idle, this);
}

void RequestTab::center_sash() {
  if (user_sash_ || !split_ || GetClientSize().x <= 0) return;
  Layout();
  const int want = split_->GetClientSize().x / 2;
  if (std::abs(split_->GetSashPosition() - want) > 1) split_->SetSashPosition(want);
}

void RequestTab::on_idle(wxIdleEvent& e) {
  e.Skip();
  if (resp_ || !IsShownOnScreen()) return;
  // Build the response view at the tab's first idle after it is shown: the first paint has
  // happened, and the build is cheap now (6-8 ms measured; its pages are built lazily), so there
  // is no reason to wait for a quiet moment and leave the pane empty. response() keeps the
  // keyboard focus where it is, so a URL being typed is not disturbed.
  Unbind(wxEVT_IDLE, &RequestTab::on_idle, this);
  response();
}

void RequestTab::on_resp_timer() {
  if (resp_) return;
  // Not while the user is typing or clicking: the build blocks the UI thread for ~60 ms, which
  // would delay the echo of a key. Wait for 1.5 s without any input (a typist's pauses between
  // words are shorter), as long as it takes: a send builds the view at once anyway, alongside
  // the request in flight.
  if (!IsShownOnScreen()) {
    resp_timer_.StartOnce(500);
    return;
  }
  // A stored last response the user opened this request to see: a short pause is enough (the
  // user is looking at the pane, not typing a URL), so it shows within ~0.2 s of the click.
  if (const int wait = input_quiet_wait(last_pending_ ? kQuietShortMs : kQuietMs); wait > 0) {
    resp_timer_.StartOnce(wait);
    return;
  }
  response();
}

ResponsePanel* RequestTab::response() {
  if (resp_) return resp_;
  realize();
  resp_timer_.Stop();
  // Building the view must never move the keyboard focus: it is built at the first idle after
  // the tab is shown, while the user may already be typing a URL. Whatever had the focus (inside
  // this window) keeps it, also against focus changes the new view's own deferred events make.
  wxWindow* const had = wxWindow::FindFocus();
  if (had && wxGetTopLevelParent(had) == wxGetTopLevelParent(this)) focus_keep_ = had;
  {
    wxWindowUpdateLocker lock(split_);
    resp_ = make_response_view(split_);
    split_->ReplaceWindow(resp_ph_, resp_->window());
    resp_ph_->Destroy();
    resp_ph_ = nullptr;
    resp_->clear();
    // Lay the view out at the size it has now. Built in a background tab (restored, never shown,
    // or prebuilt in an idle moment) it would otherwise keep the layout of its creation size,
    // too narrow, until something else re-laid it out (the first response).
    split_->UpdateSize();
    resp_->window()->Layout();
    if (last_pending_) {
      last_pending_ = false;
      show_last();
    }
  }
  startup::mark("response_view");
  Unbind(wxEVT_IDLE, &RequestTab::on_idle, this);  // false if on_idle already unbound itself
  if (focus_keep_) {
    restore_focus();
    // The view (or wx's container logic) may hand focus to it a little later; for a short while
    // focus that lands in the view without a click goes back to where the user was typing.
    resp_->window()->Bind(wxEVT_CHILD_FOCUS, &RequestTab::on_resp_focus, this);
    focus_guard_until_ = Clock::now() + std::chrono::milliseconds(600);
  }
  return resp_;
}

void RequestTab::restore_focus() {
  wxWindow* w = focus_keep_.get();
  if (w && w != wxWindow::FindFocus() && w->IsShownOnScreen()) w->SetFocus();
}

void RequestTab::on_resp_focus(wxChildFocusEvent& e) {
  e.Skip();
  const bool clicked = wxGetMouseState().LeftIsDown() || wxGetMouseState().RightIsDown() ||
                       wxGetMouseState().MiddleIsDown();
  if (clicked || Clock::now() > focus_guard_until_ || !focus_keep_) {
    resp_->window()->Unbind(wxEVT_CHILD_FOCUS, &RequestTab::on_resp_focus, this);
    focus_keep_ = nullptr;
    return;
  }
  CallAfter([this] { restore_focus(); });
}

RequestTab::~RequestTab() { worker_->shutdown(); }

RequestEditorPanel* RequestTab::editor() const {
  return req_.kind == RequestKind::Http ? static_cast<RequestEditorPanel*>(http_)
                                        : static_cast<RequestEditorPanel*>(grpc_);
}

RequestEditorPanel* RequestTab::ensure_editor(RequestKind kind) {
  RequestEditorPanel* ed;
  if (kind == RequestKind::Http) {
    if (!http_) http_ = make_http_editor(split_, this);
    ed = http_;
  } else {
    if (!grpc_) {
      // The gRPC engine (and its delay-loaded protobuf / gRPC DLLs, ~60 ms) starts now on a
      // thread of its own, while this editor is built and the user picks an address: never on
      // the UI thread at the first reflection or send.
      // A parked tab (restored from the session) waits for the user instead (wake_grpc).
      if (ctx_.grpc && !grpc_parked_) std::thread([g = ctx_.grpc] { g(); }).detach();
      grpc_ = make_grpc_editor(split_, this);
    }
    ed = grpc_;
  }
  loading_ = true;
  ed->load(req_);
  loading_ = false;
  return ed;
}

void RequestTab::show_editor(RequestKind kind) {
  wxWindow* old = editor()->window();
  req_.kind = kind;
  RequestEditorPanel* ed = ensure_editor(kind);
  if (old != ed->window()) {
    split_->ReplaceWindow(old, ed->window());
    old->Hide();
    ed->window()->Show();
  }
}

void RequestTab::store() const {
  if (RequestEditorPanel* ed = editor()) ed->store(req_);  // null: a lazy tab not shown yet
}

const SavedRequest& RequestTab::request() const {
  store();
  return req_;
}

std::wstring RequestTab::title() const {
  std::string t;
  // "Untitled" is what an unsaved tab used to be named when it was sent: a history entry or a
  // restored tab of it is titled by what it calls (Echo/Chat), not "gRPC Untitled".
  if (!req_.name.empty() && req_.name != "Untitled") {
    t = req_.kind == RequestKind::Grpc ? grpc_display_name(req_.name, req_.grpc_method) : req_.name;
  } else if (req_.kind == RequestKind::Http) {
    t = req_.http.url.empty() ? "Untitled" : url_tail(req_.http.url);
  } else {
    // "/watt.test.Echo/Chat" -> "Echo/Chat", as the method picker shows it.
    std::string m = req_.grpc_method;
    if (!m.empty() && m[0] == '/') m.erase(0, 1);
    if (const auto sl = m.find('/'); sl != std::string::npos)
      if (const auto dot = m.rfind('.', sl); dot != std::string::npos) m.erase(0, dot + 1);
    t = m.empty() ? "Untitled" : m;
  }
  std::string prefix = req_.kind == RequestKind::Http ? req_.http.method : std::string("gRPC");
  return wxString::FromUTF8(prefix + " " + t).ToStdWstring() + (dirty_ ? L" \u25CF" : L"");
}

void RequestTab::make_permanent() {
  if (!preview_) return;
  preview_ = false;
  if (on_title_changed) on_title_changed(this);
}

void RequestTab::set_request(SavedRequest req, bool preview) {
  if (in_flight_ || pending_send_) cancel();
  if (!split_) {  // not realized yet: realize() loads req_ when the tab is first shown
    req_ = std::move(req);
    preview_ = preview;
    dirty_ = false;
    if (on_title_changed) on_title_changed(this);
    return;
  }
  wxWindow* old = editor()->window();
  req_ = std::move(req);
  preview_ = preview;
  dirty_ = false;
  RequestEditorPanel* ed = ensure_editor(req_.kind);
  if (old != ed->window()) {
    split_->ReplaceWindow(old, ed->window());
    old->Hide();
    ed->window()->Show();
  }
  last_ = {};
  last_pending_ = false;
  if (resp_) resp_->clear();
  if (on_title_changed) on_title_changed(this);
}

std::string RequestTab::suggested_name() const {
  store();
  std::string s;
  if (req_.kind == RequestKind::Http) {
    s = req_.http.url;
    if (const auto p = s.find("://"); p != std::string::npos) s = s.substr(p + 3);
    s = s.substr(0, s.find_first_of("?#"));
  } else {
    s = req_.grpc_method;
  }
  // Drop {{variables}} ("{{base}}/echo" -> "/echo"), then take the last non-empty segment.
  for (size_t a; (a = s.find("{{")) != std::string::npos;) {
    const size_t b = s.find("}}", a + 2);
    s.erase(a, b == std::string::npos ? std::string::npos : b + 2 - a);
  }
  while (!s.empty() && s.back() == '/') s.pop_back();
  if (const auto p = s.rfind('/'); p != std::string::npos) {
    std::string last = s.substr(p + 1);
    // An id alone says nothing ("{{base}}/json/3" -> "3"): name it with the segment before it
    // ("json 3", "users 42", "orders :id").
    const bool id = !last.empty() && (last.find_first_not_of("0123456789") == std::string::npos || last[0] == ':' ||
                                      (last.size() >= 8 && last.find_first_not_of("0123456789abcdefABCDEF-") ==
                                                               std::string::npos));
    std::string head = s.substr(0, p);
    while (!head.empty() && head.back() == '/') head.pop_back();
    const auto q = head.rfind('/');
    std::string prev = q == std::string::npos ? std::string() : head.substr(q + 1);
    if (q == std::string::npos && req_.kind == RequestKind::Http) prev.clear();  // the host: not a name
    s = id && !prev.empty() ? prev + " " + last : last;
  }
  for (char& c : s)
    if (c == '\\' || c == ':' || c == '?' || c == '*' || c == '"' || c == '<' || c == '>' || c == '|' || c == '{' ||
        c == '}')
      c = '_';
  if (s.empty() && !req_.name.empty() && req_.name.find("{{") == std::string::npos) s = req_.name;
  return s.empty() ? std::string("request") : s;
}

bool RequestTab::save(Workspace& ws, std::string* err) {
  store();
  if (!save_name_.empty()) req_.name = save_name_;
  if (req_.name.empty()) req_.name = suggested_name();
  if (req_.file.empty()) {
    // A free file name in the chosen folder (never over an existing request); the name itself
    // keeps what the user typed, the file name drops {{variables}} and reserved characters.
    std::string base = req_.name;
    for (size_t a; (a = base.find("{{")) != std::string::npos;) {
      const size_t b = base.find("}}", a + 2);
      base.erase(a, b == std::string::npos ? std::string::npos : b + 2 - a);
    }
    if (base.find_first_not_of(" _-.") == std::string::npos) base = suggested_name();
    req_.file = ws.new_request(req_.kind, base, save_folder_.empty() ? ws.root() : save_folder_).file;
  }
  if (!ws.save_request(req_, err)) return false;
  save_name_.clear();
  dirty_ = false;
  preview_ = false;
  if (on_title_changed) on_title_changed(this);
  return true;
}

void RequestTab::focus_url() {
  realize();
  editor()->focus_url();
}

void RequestTab::restore_response(LastResponse r) {
  if (!r || in_flight_ || pending_send_) return;
  last_ = std::move(r);
  last_pending_ = true;
  if (resp_) {
    last_pending_ = false;
    show_last();
  }
}

void RequestTab::show_last() {
  resp_->set_history_when(last_.when_iso);
  if (last_.http) resp_->show_http(last_.http, last_.sent);
  else if (last_.grpc) resp_->show_grpc(last_.grpc);
  resp_->set_history_when({});
}

std::shared_ptr<const GrpcSchema> RequestTab::session_schema() const {
  if (req_.kind != RequestKind::Grpc) return nullptr;
  SavedRequest h = req_;
  if (grpc_) grpc_->store_head(h);
  if (h.grpc_method.empty()) return nullptr;
  if (restored_schema_ && schema_origin(h) == restored_origin_) return restored_schema_;  // not built yet
  const auto& m = known_schemas();
  const auto it = m.find(schema_origin(h));
  if (it == m.end()) return nullptr;
  for (const auto& mi : it->second->methods)
    if (mi.full_path == h.grpc_method) return it->second;
  return nullptr;
}

void RequestTab::restore_schema(std::shared_ptr<const GrpcSchema> s) {
  if (!s || req_.kind != RequestKind::Grpc || !grpc_parked_ || parked_schema_) return;
  restored_origin_ = schema_origin(req_);
  restored_schema_ = std::move(s);
}

void RequestTab::mark_dirty() {
  dirty_ = true;
  preview_ = false;
  if (on_title_changed) on_title_changed(this);
}

void RequestTab::warm_up() {
  auto w = worker_;
  w->push([] { SharedClients::get().warm(); });
}

void RequestTab::file_moved(const std::filesystem::path& to) {
  req_.file = to;
  // A rename in the sidebar also renames the request inside its file. Adopt that name, or the
  // next Ctrl+S would write the old one back and silently undo the rename. Only the name: any
  // unsaved edits in this tab are kept.
  if (!to.empty())
    if (const Workspace* ws = workspace())
      if (auto r = ws->load_request(to)) req_.name = r->name;
  if (on_title_changed) on_title_changed(this);
}

// ------------------------------------------------------------------ EditorHost

void RequestTab::changed() {
  if (loading_) return;
  editor()->store_head(req_);  // cheap: name, method, url, target (never the body)
  dirty_ = true;
  preview_ = false;
  if (on_title_changed) on_title_changed(this);
}

void RequestTab::switch_kind(RequestKind kind) {
  if (kind == req_.kind) return;
  realize();
  wake_grpc();
  if (in_flight_) cancel();
  store();
  show_editor(kind);
  last_ = {};
  last_pending_ = false;
  if (resp_) resp_->clear();
  dirty_ = true;
  if (on_title_changed) on_title_changed(this);
}

const Workspace* RequestTab::workspace() const { return ctx_.workspace ? ctx_.workspace() : nullptr; }

std::filesystem::path RequestTab::inherit_file() const {
  if (!req_.file.empty()) {
    if (ctx_.snapshot_origin)
      if (std::filesystem::path o = ctx_.snapshot_origin(req_); !o.empty()) return o;
    return req_.file;
  }
  // Unsaved: it inherits from the folder its first save goes to (as Bruno's new request does).
  if (!save_folder_.empty()) return save_folder_ / L"(unsaved).req.json";
  return {};
}

SavedRequest RequestTab::for_inheritance(const SavedRequest& req) const {
  SavedRequest r = req;
  r.file = inherit_file();
  return r;
}

void RequestTab::send() {
  const auto click = Clock::now();
  realize();
  wake_grpc();  // the schema loads alongside the call (the engine resolves the method itself)
  // Sent before the workspace has opened (it opens in the background at start-up, usually by
  // the first paint): wait for it, so the request gets its {{vars}}, the upload checks and a
  // history entry like any other. The response view shows "Sending..." meanwhile.
  if (!workspace() && ctx_.when_workspace) {
    if (pending_send_) return;
    wxWeakRef<RequestTab> self(this);
    if (ctx_.when_workspace([self, click] {
          if (self && self->pending_send_) {
            self->pending_send_ = false;
            self->send_now(click);
          }
        })) {
      pending_send_ = true;
      store();
      response()->show_sending(req_.kind);
      return;
    }
  }
  send_now(click);
}

void RequestTab::send_now(Clock::time_point click) {
  if (in_flight_) cancel();
  last_pending_ = false;
  store();
  if (req_.kind == RequestKind::Http)
    send_http(click);
  else
    send_grpc(click);
}

void RequestTab::send_http(Clock::time_point click) {
  Workspace* ws = ctx_.workspace ? ctx_.workspace() : nullptr;
  if (req_.http.url.empty()) {
    response()->show_error("Enter a URL first.");
    return;
  }
  // What is checked and sent: the tab's request, inheriting from its folders (a history snapshot
  // from the folders of the request it came from; an unsaved one from the folder it will be saved in).
  auto rq_p = std::make_shared<SavedRequest>(for_inheritance(req_));
  const SavedRequest& rq = *rq_p;
  // Files from outside the workspace and OS environment variables need the user's consent
  // (Workspace::blocked_files / blocked_env_vars). http_to_send() refuses them regardless, so
  // this is the prompt, not the gate. Only requests that go through http_to_send can reach them.
  // Time the user spends answering a consent prompt is not request time: after any prompt the
  // clock restarts, so queue_ms / total_ms (and asserts on res.responseTime) stay honest.
  bool prompted = false;
  // Folder / collection settings (folder.json) add headers, auth, vars and scripts that only
  // http_to_send applies: such a request never takes the fast path.
  const bool inherited = ws && ws->has_inherited_settings(rq);
  if (ws && (needs_workspace(rq) || inherited)) {
    prompted = !ws->blocked_env_vars(rq).empty();
    if (std::string err; !confirm_env_vars(*ws, rq, this, &err)) {
      response()->show_error(err);
      return;
    }
    for (const auto& p : ws->blocked_files(rq)) {
      prompted = true;
      const auto ans = wxMessageBox(
          wxString::Format("This request wants to upload\n\n%s\n\nfrom outside the workspace. Allow it?",
                           p.wstring()),
          "watt", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);
      std::string err;
      if (ans != wxYES || !ws->allow_file(p, &err)) {
        response()->show_error(err.empty() ? "Upload not allowed: " + p.string() : err);
        return;
      }
    }
    // NTLM / Negotiate with no user name signs in as the current Windows user: a cloned
    // workspace could aim that at a hostile host, so each host needs the user's consent.
    for (const auto& host : ws->blocked_sso_hosts(rq)) {
      prompted = true;
      const auto ans = wxMessageBox(
          wxString::Format("This request signs in to\n\n%s\n\nwith your Windows account (%s, no user name "
                           "given). Only allow this for servers you trust. Allow it for this workspace?",
                           wxString::FromUTF8(host),
                           rq.auth.type == HttpAuth::Type::Ntlm ? "NTLM" : "Negotiate"),
          "watt", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);
      std::string err;
      if (ans != wxYES || !ws->allow_sso_host(host, &err)) {
        response()->show_error(err.empty() ? "Windows sign-in not allowed for " + host : err);
        return;
      }
    }
    // Proxy / CA / client certificate from workspace files (a cloned repo could route every
    // request through its proxy and make watt trust its CA: a silent MITM of HTTPS). A CA file
    // outside the workspace is first allowed to be read, then its content is asked about: at
    // most a few rounds.
    for (int round = 0; round < 3; ++round) {
      const auto items = ws->blocked_network_settings(rq);
      if (items.empty()) break;
      for (const auto& it : items) {
        prompted = true;
        if (!confirm_network_item(it, this)) {
          response()->show_error("Not sent: " + it.text + " (set in the " + it.level + ") is not allowed.");
          return;
        }
        std::string err;
        if (!ws->allow_network_item(it, &err)) {
          response()->show_error(err.empty() ? "Not allowed: " + it.text : err);
          return;
        }
      }
    }
  }
  // Scripts are code from the workspace (possibly a cloned repo): ask once per workspace.
  bool run_scripts = false, scripts_skipped = false, has_pre = false, has_post = false;
  if (ws) {  // the request's own scripts plus those inherited from its folders / collection
    const auto eff = ws->effective_settings(rq);
    has_pre = !eff.pre_scripts.empty();
    has_post = !eff.post_scripts.empty();
  }
  if (ws && (has_pre || has_post)) {
    run_scripts = ws->scripts_trusted();
    if (!run_scripts) {
      prompted = true;
      const auto ans = wxMessageBox(
          wxString::Format("Requests in this workspace contain scripts:\n\n%s\n\nScripts run in a sandbox "
                           "(no file or process access; network only through watt's own checks), but they "
                           "can read and change your requests and variables. Run scripts in this workspace?",
                           ws->root().wstring()),
          "watt", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this);
      std::string err;
      run_scripts = ans == wxYES && ws->trust_scripts(true, &err);
      scripts_skipped = !run_scripts;  // sent without its scripts; the response says so
    }
  }
  if (prompted) click = Clock::now();
  const uint64_t gen = ++gen_;
  in_flight_ = true;
  sent_at_ = click;
  if (drop_snapshot_time(req_) && on_title_changed) on_title_changed(this);
  // The job is queued before any UI update, so the worker starts while the UI repaints.
  std::shared_ptr<const SavedRequest> saved = std::move(rq_p);
  auto wsc = ws ? std::make_shared<Workspace>(*ws) : nullptr;
  auto w = worker_;
  w->push([w, gen, click, saved, wsc, run_scripts, scripts_skipped, has_pre, has_post, inherited] {
    if (w->is_cancelled(gen)) return;
    // Every send that got as far as the network (or failed on its way there: an OAuth token step)
    // is a history row; status 0 is a failed one.
    auto record_history = [&](long status, double total_ms) {
      if (!wsc) return;
      HistoryEntry e;
      e.when_iso = now_iso();
      e.kind = RequestKind::Http;
      // What was sent (vars resolved now, on the worker, with the workspace copy the request
      // was built from), not the template alone: the history row must say where it went even
      // after the environment is switched (history_summary.h). The URL as written, expanded,
      // not the final request URL, so an OAuth / API key query parameter is not recorded.
      e.summary = make_history_summary(saved->http.method + " " + saved->http.url,
                                       wsc->expand(saved->http.url, *saved), wsc->active_env);
      e.status = status;
      e.total_ms = total_ms;
      e.request = *saved;
      {
        std::lock_guard lk(history_mutex());
        wsc->append_history(e);
      }
      w->post([e = std::move(e)](RequestTab& t) {
        if (t.ctx_.history_added) t.ctx_.history_added(e);
      });
    };
    auto sent = std::make_shared<SentHttp>();
    sent->scripts_skipped_untrusted = scripts_skipped;
    // Pre-request script: edits a copy of the request; its variable writes apply to this
    // worker's workspace copy (so this send sees them) and later to the real one (UI thread).
    auto toSend = std::make_shared<SavedRequest>(*saved);
    std::shared_ptr<ScriptRun> pre, post;
    if (run_scripts && has_pre) {
      pre = std::make_shared<ScriptRun>(run_pre_script(*wsc, *toSend));
      if (!pre->ok) {
        const std::string err = "Pre-request script failed: " + pre->error;
        w->post([gen, err](RequestTab& t) { t.on_error(gen, err); });
        return;
      }
      apply_script_vars(*wsc, *pre, false);
    }
    // OAuth 2.0: get the token here (cached, refreshed or fetched), timed, so the response shows
    // it as its own phase; http_to_send() below then finds it in the cache.
    if (wsc && (toSend->auth.type == HttpAuth::Type::OAuth2 || (inherited && toSend->auth.type == HttpAuth::Type::Inherit))) {
      const EffectiveSettings eff = wsc->effective_settings(*toSend);
      if (eff.auth.type == HttpAuth::Type::OAuth2) {
        auto step = std::make_shared<SentHttp::TokenStep>();
        const OAuth2Result t = oauth2_get_token(*wsc, *toSend);
        step->ok = t.ok;
        step->from_cache = t.from_cache;
        step->refreshed = t.refreshed;
        step->ms = t.elapsed_ms;
        step->error = t.error;
        const OAuth2Config& oc = eff.auth.oauth2;
        step->grant = oc.grant == OAuth2Config::Grant::ClientCredentials ? "client credentials"
                      : oc.grant == OAuth2Config::Grant::Password        ? "password"
                                                                         : "authorization code";
        step->token_url = wsc->expand(oc.token_url, *toSend);
        step->from = eff.auth_from.label;
        sent->token = step;
        if (!t.ok) {  // the same message http_to_send() would give, without a second token call
          const std::string err = "OAuth 2.0: " + t.error + ": request not sent.";
          w->post([gen, err](RequestTab& t) { t.on_error(gen, err); });
          record_history(0, ms_between(click, Clock::now()));
          return;
        }
      }
    }
    if (wsc && (needs_workspace(*toSend) || inherited)) {
      std::string err;
      auto r = wsc->http_to_send(*toSend, &err);
      if (!r) {
        w->post([gen, err](RequestTab& t) { t.on_error(gen, err); });
        return;
      }
      sent->request = std::move(*r);
    } else {
      sent->request = toSend->http;
    }
    if (w->is_cancelled(gen)) return;
    const std::string origin = Workspace::url_origin(sent->request.url);
    HttpClient& client = w->lease_client(origin);
    // Progress: at most every 50 ms.
    uint64_t got = 0;
    auto last = Clock::now();
    auto on_chunk = [&](const char*, size_t n) {
      got += n;
      const auto now = Clock::now();
      if (ms_between(last, now) >= 50) {
        last = now;
        w->post([gen, got](RequestTab& t) {
          if (t.gen_ == gen && t.in_flight_) t.response()->show_progress(got);
        });
      }
    };
    const auto start = Clock::now();
    const double ui_queue = ms_between(click, start);
    if (w->is_cancelled(gen)) {  // cancelled while the client was being borrowed
      w->give_back(origin);
      return;
    }
    auto resp = std::make_shared<HttpResponse>(client.send(sent->request, on_chunk));
    w->give_back(origin);
    resp->timing.queue_ms += ui_queue;
    resp->timing.total_ms += ui_queue;
    if (run_scripts && has_post && resp->ok) {
      ResponseData rd(*resp);
      post = std::make_shared<ScriptRun>(run_post_script(*wsc, *toSend, rd));
    }
    sent->pre_script = pre;
    sent->post_script = post;
    std::shared_ptr<const HttpResponse> cresp = resp;
    std::shared_ptr<const SentHttp> csent = sent;
    const std::filesystem::path script_root = wsc ? wsc->root() : std::filesystem::path{};
    w->post([gen, cresp, csent, pre, post, script_root](RequestTab& t) {
      // Script variable writes land in the real workspace on the UI thread (persisted for
      // environment/globals, in memory for runtime vars), even if the tab moved on, but only
      // in the workspace whose scripts produced them: if the user opened another workspace
      // meanwhile (or revoked trust), they are dropped.
      if (Workspace* real = t.ctx_.workspace ? t.ctx_.workspace() : nullptr;
          real && (pre || post) && real->root() == script_root && real->scripts_trusted()) {
        if (pre) apply_script_vars(*real, *pre, true);
        if (post && post->ok) apply_script_vars(*real, *post, true);
      }
      t.on_http_done(gen, cresp, csent);
    });

    if (!(resp->error == "cancelled")) record_history(resp->status, resp->timing.total_ms);
  });
  editor()->set_in_flight(true);
  response()->show_sending(RequestKind::Http);
}

void RequestTab::send_grpc(Clock::time_point click) {
  if (req_.grpc_method.empty() || req_.grpc_target.address.empty()) {
    response()->show_error("Choose a server address and a method first.");
    return;
  }
  Workspace* ws = ctx_.workspace ? ctx_.workspace() : nullptr;
  auto rq_p = std::make_shared<SavedRequest>(for_inheritance(req_));  // inherits as send_http's
  const SavedRequest& rq = *rq_p;
  const bool grpc_inherited = ws && ws->has_inherited_settings(rq);  // folder metadata / vars
  if (ws && (grpc_has_vars(rq) || grpc_inherited)) {
    const bool prompted = !ws->blocked_env_vars(rq).empty();
    if (std::string err; !confirm_env_vars(*ws, rq, this, &err)) {
      response()->show_error(err);
      return;
    }
    if (prompted) click = Clock::now();  // time spent in the prompt is not request time
  }
  const uint64_t gen = ++gen_;
  in_flight_ = true;
  sent_at_ = click;
  if (drop_snapshot_time(req_) && on_title_changed) on_title_changed(this);
  {
    std::lock_guard lk(worker_->msg_m);
    worker_->msgs.clear();
    worker_->msg_posted = false;
    worker_->msg_gen = gen;
  }

  std::shared_ptr<const SavedRequest> saved = std::move(rq_p);
  auto wsc = ws ? std::make_shared<Workspace>(*ws) : nullptr;
  const bool interactive = grpc_ && grpc_->interactive();
  auto w = worker_;
  w->push([w, gen, click, saved, wsc, interactive, grpc = ctx_.grpc, grpc_inherited] {
    if (w->is_cancelled(gen)) return;
    auto client = grpc();  // first use starts the engine: on the worker, not the UI thread
    const SavedRequest r = wsc && (grpc_has_vars(*saved) || grpc_inherited) ? wsc->expanded(*saved) : *saved;
    const auto start = Clock::now();
    const double ui_queue = ms_between(click, start);
    std::shared_ptr<GrpcCall> call(client->start_call(
        r.grpc_target, r.grpc_method, r.grpc_opts,
        [w, gen](const GrpcMessageEvent& e) { w->on_message(gen, e); }));
    {
      std::lock_guard lk(w->call_m);
      w->call = call;
      if (w->is_cancelled(gen)) call->cancel();
    }
    if (!call->done()) {
      if (interactive) {
        if (!trim(r.grpc_body).empty()) call->send(r.grpc_body);
      } else {
        for (const auto& m : split_messages(r.grpc_body)) {
          std::string err;
          if (!call->send(m, &err)) {
            call->cancel();
            w->post([gen, err](RequestTab& t) { t.on_error(gen, "Request message: " + err); });
            break;
          }
        }
        call->close_send();
      }
    }
    auto resp = std::make_shared<GrpcResponse>(call->wait());
    {
      std::lock_guard lk(w->call_m);
      w->call.reset();
    }
    resp->timing.queue_ms += ui_queue;
    resp->timing.total_ms += ui_queue;
    std::shared_ptr<const GrpcResponse> cresp = resp;
    w->post([gen, cresp](RequestTab& t) { t.on_grpc_done(gen, cresp); });

    if (wsc && resp->status_name != "CANCELLED") {
      HistoryEntry e;
      e.when_iso = now_iso();
      e.kind = RequestKind::Grpc;
      e.summary = make_history_summary(saved->grpc_method + " @ " + saved->grpc_target.address,
                                       r.grpc_target.address, wsc->active_env);
      e.status = resp->status_code;
      e.total_ms = resp->timing.total_ms;
      e.request = *saved;
      {
        std::lock_guard lk(history_mutex());
        wsc->append_history(e);
      }
      w->post([e = std::move(e)](RequestTab& t) {
        if (t.ctx_.history_added) t.ctx_.history_added(e);
      });
    }
  });
  editor()->set_in_flight(true);
  response()->show_sending(RequestKind::Grpc);
}

void RequestTab::cancel() {
  if (pending_send_) {
    pending_send_ = false;
    response()->show_cancelled(0);
    return;
  }
  if (!in_flight_) return;
  worker_->cancel(gen_);
  const double after = ms_between(sent_at_, Clock::now());
  finish(gen_);
  response()->show_cancelled(after);
}

void RequestTab::finish(uint64_t gen) {
  if (gen != gen_ || !in_flight_) return;
  in_flight_ = false;
  editor()->set_in_flight(false);
}

void RequestTab::on_http_done(uint64_t gen, std::shared_ptr<const HttpResponse> r,
                              std::shared_ptr<const SentHttp> sent) {
  if (gen != gen_ || !in_flight_) return;
  finish(gen);
  last_ = LastResponse{r, sent, nullptr};
  last_pending_ = false;
  response()->show_http(std::move(r), std::move(sent));
  if (on_response) on_response(this);
  // Start-up measurement: "shown" = after the view's own follow-up events ran and the window
  // has painted. No-op unless tracing.
  if (startup::awaiting_response()) CallAfter([this] {
    if (wxWindow* top = wxGetTopLevelParent(this))
      RedrawWindow(static_cast<HWND>(top->GetHWND()), nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
    startup::response_shown();
  });
}

void RequestTab::on_grpc_messages(uint64_t gen) {
  auto batch = worker_->take_messages();
  if (gen != gen_ || !in_flight_ || batch.empty()) return;
  response()->append_grpc_messages(batch);
}

void RequestTab::on_grpc_done(uint64_t gen, std::shared_ptr<const GrpcResponse> r) {
  if (gen != gen_ || !in_flight_) return;
  on_grpc_messages(gen);
  finish(gen);
  if (r && !r->ok)
    if (std::string hint = web_port_hint(req_.grpc_target, r->status_message + " " + r->error_details); !hint.empty()) {
      auto c = std::make_shared<GrpcResponse>(*r);
      c->status_message += hint;
      r = std::move(c);
    }
  last_ = LastResponse{nullptr, nullptr, r};
  last_pending_ = false;
  response()->show_grpc(std::move(r));
  if (on_response) on_response(this);
  if (startup::awaiting_response()) CallAfter([this] {
    if (wxWindow* top = wxGetTopLevelParent(this))
      RedrawWindow(static_cast<HWND>(top->GetHWND()), nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
    startup::response_shown();
  });
}

void RequestTab::on_error(uint64_t gen, const std::string& msg) {
  if (gen != gen_ || !in_flight_) return;
  finish(gen);
  response()->show_error(msg);
}

void RequestTab::load_schema(const SavedRequest& r) {
  if (grpc_parked_) {
    // The editor's own load when it was built: hold it. A second one while parked comes from the
    // user (reload, another address, schema source, a file opened into this tab): wake up.
    const bool first = !parked_schema_;
    parked_schema_ = std::make_unique<SavedRequest>(r);
    if (!first) {
      wake_grpc();
    } else if (auto rs = std::move(restored_schema_); rs && grpc_ && schema_origin(r) == restored_origin_) {
      // The schema this tab had in the last session: the editor shows the method, its call
      // kind and types and hides the methods pane now, exactly as it will once the server is
      // asked again on waking (which then changes nothing visible). Not an edit of the tab.
      known_schemas().try_emplace(restored_origin_, rs);
      const bool was = loading_;
      loading_ = true;
      grpc_->set_schema(rs);
      loading_ = was;
    } else {
      hold_schema_ui();
    }
    return;
  }
  load_schema_now(r);
}

// The editor started its "Reflecting <address> ... <elapsed>" clock when it asked for the load
// that is now held. While held, nothing is loading: hide that line and stop its 20 Hz clock
// (a repeating 50 ms timer of the editor; its one-shot timers are left alone) instead of showing
// a load that is not happening. The editor rewrites the line when the schema arrives.
void RequestTab::hold_schema_ui() {
  if (!grpc_) return;
  wxWindow* ed = grpc_->window();
  if (!timer_hook_) {
    timer_hook_ = true;
    // Bound after the editor's own handlers, so it is called before them.
    ed->Bind(wxEVT_TIMER, [this](wxTimerEvent& e) {
      if (parked_schema_ && grpc_parked_ && !e.GetTimer().IsOneShot() && e.GetInterval() == 50) {
        e.GetTimer().Stop();
        return;
      }
      e.Skip();
    });
  }
  std::function<wxWindow*(wxWindow*)> find = [&](wxWindow* w) -> wxWindow* {
    for (wxWindow* c : w->GetChildren()) {
      if (auto* st = dynamic_cast<wxStaticText*>(c)) {
        const wxString l = st->GetLabel();
        if (l.StartsWith("Reflecting ") || l.StartsWith("Parsing ")) return st;
      }
      if (wxWindow* f = find(c)) return f;
    }
    return nullptr;
  };
  if (wxWindow* w = find(ed)) {
    // Say what is true instead. The line re-wraps its own text on resize; while held it keeps
    // this one (one short line, so the height it already has fits).
    // A re-wrap the line queued before this (it re-wraps on the next turn after a resize) would
    // bring the old text back: set it again after those.
    auto* st = static_cast<wxStaticText*>(w);
    st->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      if (!grpc_parked_) e.Skip();
    });
    held_status_ = st;
    auto relabel = [this] {
      if (wxWindow* h = held_status_.get(); h && grpc_parked_) static_cast<wxStaticText*>(h)->SetLabel("Not loaded yet.");
    };
    relabel();
    CallAfter([this, relabel] {
      relabel();
      CallAfter(relabel);
    });
  }
}

void RequestTab::load_schema_now(const SavedRequest& r) {
  // Not on the tab worker: an interactive stream may keep it busy for minutes.
  wxASSERT_MSG(worker_, "load_schema() before the tab worker exists");
  auto w = worker_;
  Workspace* ws = ctx_.workspace ? ctx_.workspace() : nullptr;
  // The schema loads as soon as a saved gRPC request is opened, so a cloned workspace could
  // point it at \\attacker\share\x.proto (NTLM hash leak) or any local file. Paths outside the
  // workspace need the user's approval first; nothing outside is touched before that.
  auto req = std::make_shared<SavedRequest>(ws ? ws->expanded(r) : r);
  if (ws && !r.proto_files.empty()) {
    Workspace::SchemaPlan plan = ws->schema_plan(r);
    if (!plan.invalid.empty()) {
      auto cs = std::make_shared<GrpcSchema>();
      cs->error = "Schema not loaded: " + plan.invalid.front();
      if (grpc_) grpc_->set_schema(cs);
      return;
    }
    for (const auto& p : plan.blocked) {
      const auto ans = wxMessageBox(
          wxString::Format("This request loads its gRPC schema from\n\n%s\n\nwhich is outside the "
                           "workspace%s. Load it?",
                           p.wstring(), p.native().rfind(L"\\\\", 0) == 0 ? L" (a network share)" : L""),
          "watt", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);
      std::string err;
      if (ans != wxYES || !ws->allow_file(p, &err)) {
        auto cs = std::make_shared<GrpcSchema>();
        cs->error = err.empty() ? "Schema not loaded: " + p.string() + " is outside the workspace and was not allowed." : err;
        if (grpc_) grpc_->set_schema(cs);
        return;
      }
    }
    // Load exactly what was checked: root-anchored absolute paths, not the raw strings.
    req->proto_files = std::move(plan.proto_files);
    req->import_dirs = std::move(plan.import_dirs);
  }
  std::thread([w, grpc = ctx_.grpc, req, origin = schema_origin(r)] {
    auto client = grpc();
    GrpcSchema s;
    if (req->proto_files.empty()) {
      Metadata md = req->grpc_opts.metadata;
      md.insert(md.end(), req->grpc_opts.reflect_metadata.begin(), req->grpc_opts.reflect_metadata.end());
      s = client->load_via_reflection(req->grpc_target, 5000, md);
      if (!s.ok) s.error += web_port_hint(req->grpc_target, s.error);
    } else {
      s = client->load_proto_files(req->proto_files, req->import_dirs);
    }
    auto cs = std::make_shared<const GrpcSchema>(std::move(s));
    w->post([cs, origin](RequestTab& t) {
      if (cs->ok) known_schemas()[origin] = cs;
      if (t.grpc_) t.grpc_->set_schema(cs);
    });
  }).detach();
}

void RequestTab::preconnect(const GrpcTarget& target) {
  if (grpc_parked_) return;  // the held schema load connects once the tab wakes
  Workspace* ws = ctx_.workspace ? ctx_.workspace() : nullptr;
  GrpcTarget t = target;
  if (ws) t.address = ws->expand(t.address);
  std::thread([grpc = ctx_.grpc, t] { grpc()->preconnect(t); }).detach();
}

bool RequestTab::stream_send(const std::string& json, std::string* error) {
  std::shared_ptr<GrpcCall> call;
  {
    std::lock_guard lk(worker_->call_m);
    call = worker_->call;
  }
  if (!call || call->done()) {
    if (error) *error = "No stream is open.";
    return false;
  }
  return call->send(json, error);
}

void RequestTab::stream_close() {
  std::shared_ptr<GrpcCall> call;
  {
    std::lock_guard lk(worker_->call_m);
    call = worker_->call;
  }
  if (call) call->close_send();
}

}  // namespace watt::app
