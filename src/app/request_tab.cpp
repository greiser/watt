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

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "app/history_summary.h"
#include "app/main_frame.h"        // startup::
#include "core/oauth2.h"
#include "core/ws_client.h"
#include "app/response_widgets.h"  // rv::Theme (the placeholder's colour)
#include "app/ws_view.h"

#include <windows.h>

namespace watt::app {

namespace {

// protobuf's JSON parse errors pad their parts with runs of spaces ("invalid JSON  in  x,   near
// 1:6"): one space each, as the rest of the UI writes them.
std::string one_space(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (const char c : s)
    if (!(c == ' ' && !o.empty() && o.back() == ' ')) o += c;
  return o;
}

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
  } else if (it.kind == K::KeepAuth) {
    msg = wxString::Format("The workspace settings (%s) send the Authorization header, Basic / Digest credentials "
                           "and cookies on to ANY host a server redirects to (keep_auth_on_redirect).\n\n"
                           "A redirect from a server you trust to one you do not would hand it your token or "
                           "password. Only allow this if every server this workspace talks to is yours. Allow it "
                           "for this workspace?",
                           where);
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

// {{names}} still in `r` after expansion with the active environment (and its folders' vars):
// they would go out literally ("Authorization: Bearer {{token}}"). The URL, the headers (own and
// inherited, enabled), Basic / scheme credentials, the OAuth 2.0 fields and a small body are
// checked; {{process.env.X}} is left to the environment-variable consent prompt.
std::vector<std::string> unresolved_vars(const Workspace& ws, const SavedRequest& rq, bool* in_url = nullptr) {
  const SavedRequest r = ws.resolved(rq);
  if (in_url) *in_url = false;
  std::string text = r.http.url;
  for (size_t k = 0; k < r.http.headers.size(); ++k)
    if (!r.disabled_headers.count(k)) text += "\n" + r.http.headers[k].first + ": " + r.http.headers[k].second;
  if (r.basic_auth.enabled) text += "\n" + r.basic_auth.user + "\n" + r.basic_auth.password;
  if (r.auth.type != HttpAuth::Type::None) {
    text += "\n" + r.auth.user + "\n" + r.auth.password;
    if (r.auth.type == HttpAuth::Type::OAuth2) {
      const OAuth2Config& o = r.auth.oauth2;
      for (const std::string* s : {&o.token_url, &o.client_id, &o.client_secret, &o.scope, &o.username, &o.password})
        text += "\n" + *s;
    }
    if (r.auth.type == HttpAuth::Type::AwsSigV4) text += "\n" + r.auth.aws_access_key + "\n" + r.auth.aws_secret_key;
  }
  if (r.http.body.size() <= (64u << 10) && r.form.empty() && r.body_file.empty()) text += "\n" + r.http.body;
  if (text.find("{{") == std::string::npos) return {};
  const std::string x = ws.expand(text, rq);
  std::vector<std::string> out;
  for (size_t a = x.find("{{"); a != std::string::npos; a = x.find("{{", a + 2)) {
    if (a + 2 < x.size() && x[a + 2] == '{') continue;  // "{{{" is a literal brace
    const size_t b = x.find("}}", a + 2);
    if (b == std::string::npos) break;
    std::string n(trim(std::string_view(x).substr(a + 2, b - a - 2)));
    if (n.empty() || n.size() > 64 || n.find_first_of("{}\n\" ") != std::string::npos) continue;
    if (n.rfind("process.env.", 0) == 0 || n.rfind("$env.", 0) == 0) continue;
    if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(std::move(n));
  }
  if (in_url && !out.empty() && r.http.url.find("{{") != std::string::npos)
    *in_url = ws.expand(r.http.url, rq).find("{{") != std::string::npos;
  return out;
}

// {{names}} left in a request as it would go out (after http_to_send): URL, headers, a small body.
std::vector<std::string> unresolved_in(const HttpRequest& q) {
  std::vector<std::string> out;
  auto scan = [&](std::string_view x) {
    for (size_t a = x.find("{{"); a != std::string_view::npos; a = x.find("{{", a + 2)) {
      if (a + 2 < x.size() && x[a + 2] == '{') continue;
      const size_t b = x.find("}}", a + 2);
      if (b == std::string_view::npos) return;
      std::string n(trim(x.substr(a + 2, b - a - 2)));
      if (n.empty() || n.size() > 64 || n.find_first_of("{}\n\" ") != std::string::npos) continue;
      if (n.rfind("process.env.", 0) == 0 || n.rfind("$env.", 0) == 0) continue;
      if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(std::move(n));
    }
  };
  scan(q.url);
  for (const auto& [k, v] : q.headers) {
    scan(k);
    scan(v);
  }
  if (q.body.size() <= (64u << 10)) scan(q.body);
  return out;
}

// An unsaved tab's title: the path without the host and the query ("{{base}}/echo?u={{user}}"
// -> "/echo"), as Postman and Bruno keep tab titles short; the host when there is no path.
std::string url_tail(const std::string& url) {
  auto p = url.find("://");
  std::string s = p == std::string::npos ? url : url.substr(p + 3);
  if (const auto q = s.find_first_of("?#"); q != std::string::npos && q > 0) s.erase(q);
  if (const auto sl = s.find('/'); sl != std::string::npos && sl > 0 && sl + 1 < s.size()) s.erase(0, sl);
  if (s.size() > 32) {
    size_t from = s.size() - 29;
    while (from < s.size() && (static_cast<unsigned char>(s[from]) & 0xC0) == 0x80) ++from;  // whole UTF-8 characters
    s = "..." + s.substr(from);
  }
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

bool has_header_named(const std::vector<Header>& hs, const std::string& name) {
  return std::any_of(hs.begin(), hs.end(), [&](const Header& x) {
    return x.first.size() == name.size() &&
           std::equal(x.first.begin(), x.first.end(), name.begin(), [](char a, char b) {
             return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           });
  });
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
// The last run's script results (pass / fail, error, time; not the logs): a restored tab shows
// the script tests it ran, not only the assertions it can evaluate again.
void put_script(JWriter& w, const char* k, const std::shared_ptr<const ScriptRun>& r) {
  if (!r || !r->ran) return;
  constexpr size_t kMaxTests = 500, kMaxText = 2000;
  auto cut = [&](const std::string& t) { return t.size() > kMaxText ? t.substr(0, kMaxText) : t; };
  w.Key(k);
  w.StartObject();
  w.Key("ok");
  w.Bool(r->ok);
  put(w, "error", cut(r->error));
  put(w, "elapsed_us", r->elapsed_us);
  w.Key("tests");
  w.StartArray();
  for (size_t i = 0; i < r->tests.size() && i < kMaxTests; ++i) {
    w.StartObject();
    put(w, "name", cut(r->tests[i].name));
    w.Key("pass");
    w.Bool(r->tests[i].pass);
    put(w, "message", cut(r->tests[i].message));
    w.EndObject();
  }
  w.EndArray();
  w.EndObject();
}
std::shared_ptr<ScriptRun> get_script(const rapidjson::Value& o, const char* k) {
  if (!o.HasMember(k) || !o[k].IsObject()) return nullptr;
  const auto& v = o[k];
  auto r = std::make_shared<ScriptRun>();
  r->ran = true;
  r->ok = flag(v, "ok");
  r->error = str(v, "error");
  r->elapsed_us = num(v, "elapsed_us");
  if (v.HasMember("tests") && v["tests"].IsArray())
    for (const auto& t : v["tests"].GetArray())
      if (t.IsObject()) r->tests.push_back(ScriptTest{str(t, "name"), flag(t, "pass"), str(t, "message")});
  return r;
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
      put_script(w, "pre_script", r.sent->pre_script);
      put_script(w, "post_script", r.sent->post_script);
    }
    if (!r.inherited_headers.empty()) put_pairs(w, "inherited_headers", r.inherited_headers);
    if (r.ws_log && !r.ws_log->empty()) {
      w.Key("ws_messages");
      w.StartArray();
      for (const WsMessage& m : *r.ws_log) {
        w.StartObject();
        w.Key("out");
        w.Bool(m.out);
        if (m.binary) {
          w.Key("binary");
          w.Bool(true);
        }
        put(w, "at_ms", m.at_ms);
        put(w, "data", m.data);
        w.EndObject();
      }
      w.EndArray();
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
    if (g->pacing.on) {
      w.Key("pacing");
      w.StartObject();
      put(w, "sent", static_cast<double>(g->pacing.sent));
      put(w, "first_send", g->pacing.first_send_ms);
      put(w, "last_send", g->pacing.last_send_ms);
      put(w, "end", g->pacing.end_ms);
      put(w, "tail", g->pacing.tail_ms);
      w.EndObject();
    }
  } else {
    return {};
  }
  // When it was received: a restored tab says "last run at 00:35" like one opened from history,
  // instead of "earlier session" for some tabs and a time for others.
  if (!r.when_iso.empty()) put(w, "when", r.when_iso);
  w.EndObject();
  return std::string(sb.GetString(), sb.GetSize());
}

LastResponse last_response_from_json(const std::string& json, const SavedRequest& req) {
  LastResponse out;
  rapidjson::Document d;
  d.Parse(json.data(), json.size());
  if (d.HasParseError() || !d.IsObject()) return out;
  const std::string kind = str(d, "kind");
  if (kind == "http" && req.kind != RequestKind::Grpc) {  // a WebSocket tab: its handshake
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
    sent->pre_script = get_script(d, "pre_script");
    sent->post_script = get_script(d, "post_script");
    get_pairs(d, "inherited_headers", [&](const std::string& n, const std::string& v) {
      if (has_header_named(sent->request.headers, n)) return;
      sent->request.headers.emplace_back(n, v);
      out.inherited_headers.emplace_back(n, v);
    });
    out.http = std::move(h);
    out.sent = std::move(sent);
    if (req.kind == RequestKind::WebSocket && d.HasMember("ws_messages") && d["ws_messages"].IsArray()) {
      auto log = std::make_shared<std::vector<WsMessage>>();
      for (const auto& m : d["ws_messages"].GetArray()) {
        if (!m.IsObject()) continue;
        if (log->size() >= LastResponse::kLogMessages) break;
        WsMessage e;
        e.out = flag(m, "out");
        e.binary = flag(m, "binary");
        e.at_ms = num(m, "at_ms");
        e.data = str(m, "data");
        if (e.data.size() > LastResponse::kLogMessageBytes) e.data.resize(LastResponse::kLogMessageBytes);
        log->push_back(std::move(e));
      }
      if (!log->empty()) out.ws_log = std::move(log);
    }
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
    if (d.HasMember("pacing") && d["pacing"].IsObject()) {
      const auto& p = d["pacing"];
      auto off = [&](const char* k) { return p.HasMember(k) && p[k].IsNumber() ? p[k].GetDouble() : -1.0; };
      g->pacing.on = true;
      g->pacing.sent = static_cast<int>(std::clamp(num(p, "sent"), 0.0, 1e9));
      g->pacing.first_send_ms = off("first_send");
      g->pacing.last_send_ms = off("last_send");
      g->pacing.end_ms = off("end");
      g->pacing.tail_ms = off("tail");
    }
    out.grpc = std::move(g);
  }
  if (out) out.when_iso = str(d, "when");
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
  std::shared_ptr<WsClient> ws;  // the WebSocket connection of this tab (under call_m)
  // A Manual (interactive) stream, under call_m: when its first and last messages went out and
  // when End half-closed it. The time between is the user's pace, not the call's.
  Clock::time_point first_send{}, last_send{}, end_sent{};
  int sends = 0;
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
    if (ws) ws->cancel();
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
  hibernated_ = false;
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
  // Only a drag of the sash is the user's choice: wxSplitterWindow::OnSize sends CHANGED too
  // when it moves the sash for a new size, and a drag always sends CHANGING first.
  // (Splitter events propagate: the editor's and the response view's own splitters send theirs
  // through split_ too, so each handler looks at split_'s events only.)
  auto dragging = std::make_shared<bool>(false);
  split_->Bind(wxEVT_SPLITTER_SASH_POS_CHANGING, [this, dragging](wxSplitterEvent& e) {
    e.Skip();
    if (e.GetEventObject() == split_) *dragging = true;
  });
  split_->Bind(wxEVT_SPLITTER_SASH_POS_CHANGED, [this, dragging](wxSplitterEvent& e) {
    e.Skip();
    if (e.GetEventObject() != split_) return;
    if (*dragging) user_sash_ = true;
    *dragging = false;
  });
  // A resize keeps the split at 50% in the splitter's own size handler, so each resize step lays
  // the editor and the response view out once (a later SetSashPosition was a second layout).
  split_->Bind(wxEVT_SPLITTER_SASH_POS_RESIZE, [this](wxSplitterEvent& e) {
    if (e.GetEventObject() != split_ || user_sash_ || e.GetNewSize() <= 0) return e.Skip();
    e.SetSashPosition(e.GetNewSize() / 2);
  });
  // wxAuiNotebook sizes every page when the window is resized, the hidden ones too: laying out
  // each tab's editor and response view made one resize step O(tabs) (67-105 ms with 12-25
  // tabs). A hidden tab only notes it; it lays itself out when it is shown again.
  // Bound once: a tab realized again after hibernate() keeps these.
  if (!size_bound_) {
  size_bound_ = true;
  Bind(wxEVT_SIZE, [this, posted = std::make_shared<bool>(false)](wxSizeEvent& e) {
    if (!IsShown()) {
      layout_pending_ = true;
      return;  // no Skip: no Layout() now
    }
    if (update_split_mode(e.GetSize())) {
      center_sash();  // the new orientation, at 50%
      return;
    }
    e.Skip();  // one Layout(); the splitter keeps the 50% (SASH_POS_RESIZE above)
    // A sash position the splitter still holds as requested (set before it had its size) skips
    // SASH_POS_RESIZE: put it at 50% once afterwards. Nothing to do (no layout) when it is there.
    if (!user_sash_ && !*posted) {
      *posted = true;
      CallAfter([this, posted] {
        *posted = false;
        if (!split_ || user_sash_) return;
        const wxSize cs = split_->GetClientSize();
        const int axis = split_->GetSplitMode() == wxSPLIT_HORIZONTAL ? cs.y : cs.x;
        if (std::abs(split_->GetSashPosition() - axis / 2) > 1) center_sash();
      });
    }
  });
  Bind(wxEVT_SHOW, [this](wxShowEvent& e) {
    e.Skip();
    if (!e.IsShown() || !layout_pending_) return;
    layout_pending_ = false;
    update_split_mode(GetSize());
    Layout();
    if (!user_sash_) center_sash();
  });
  }
  update_split_mode(GetClientSize());
  center_sash();  // realized after the page was laid out (a restored tab)
  Bind(wxEVT_IDLE, &RequestTab::on_idle, this);
}

bool RequestTab::hibernate() {
  if (!split_) {
    hibernated_ = true;
    return false;
  }
  if (IsShown() || dirty_ || in_flight_ || pending_send_ || ws_open_ || req_.kind == RequestKind::Grpc) return false;
  store();  // what the editor shows (a reload of the file could differ from it)
  if (RequestEditorPanel* ed = editor())
    while (ed->teardown_step()) {  // big documents freed on a worker, timers stopped
    }
  resp_timer_.Stop();
  Unbind(wxEVT_IDLE, &RequestTab::on_idle, this);  // false when response() unbound it already
  if (last_) last_pending_ = true;  // shown again once the response view is built again
  wxWindow* const old = split_;
  split_ = nullptr;
  http_ = nullptr;
  grpc_ = nullptr;
  ws_ = nullptr;
  resp_ = nullptr;
  resp_ph_ = nullptr;
  resp_ws_ = false;
  focus_keep_ = nullptr;
  held_status_ = nullptr;
  SetSizer(nullptr);  // deletes the sizer that held old
  old->Destroy();
  hibernated_ = true;
  return true;
}

void RequestTab::prelayout(const wxSize& sz) {
  if (!split_ || sz.x <= 0 || sz.y <= 0) return;
  if (GetSize() != sz) SetSize(sz);  // hidden: the size handler only notes layout_pending_
  layout_pending_ = false;
  update_split_mode(sz);
  if (user_sash_)
    Layout();
  else
    center_sash();  // Layout() + the 50% sash
  if (resp_) resp_->window()->Layout();
}

void RequestTab::center_sash() {
  if (user_sash_ || !split_ || GetClientSize().x <= 0) return;
  Layout();
  const wxSize cs = split_->GetClientSize();
  const int want = (split_->GetSplitMode() == wxSPLIT_HORIZONTAL ? cs.y : cs.x) / 2;
  if (std::abs(split_->GetSashPosition() - want) > 1) split_->SetSashPosition(want);
}

bool RequestTab::update_split_mode(const wxSize& sz) {
  if (!split_ || !split_->IsSplit() || sz.x <= 0 || sz.y <= 0) return false;
  const bool stacked = split_->GetSplitMode() == wxSPLIT_HORIZONTAL;
  // Two side-by-side panes under ~500 px each lose their tab strips and toolbars (a Windows Snap
  // half of a 1920 screen); stacked, each gets the full width. A little hysteresis, so a drag
  // across the threshold does not flip back and forth.
  // Stacked only when tall enough for two usable panes (not on a 768 px screen: there the tab
  // strips' overflow buttons keep everything reachable side by side).
  const bool want = stacked ? sz.x < FromDIP(1040) && sz.y >= FromDIP(720)
                            : sz.x < FromDIP(1000) && sz.y >= FromDIP(760);
  if (want == stacked) return false;
  split_->SetSplitMode(want ? wxSPLIT_HORIZONTAL : wxSPLIT_VERTICAL);
  user_sash_ = false;  // a sash dragged in the other orientation means nothing in this one
  return true;
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
    resp_ws_ = req_.kind == RequestKind::WebSocket;
    resp_ = resp_ws_ ? static_cast<ResponsePanel*>(make_ws_view(split_)) : make_response_view(split_);
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

bool RequestTab::teardown_step() {
  if (in_flight_) worker_->cancel(gen_);
  in_flight_ = false;
  pending_send_ = false;
  ++gen_;  // a late result is dropped
  resp_timer_.Stop();
  if (RequestEditorPanel* ed = editor(); ed && ed->teardown_step()) return true;
  if (resp_ && split_ && last_ && !torn_resp_cleared_) {  // its documents first, then its windows
    torn_resp_cleared_ = true;
    resp_->clear();
    return true;
  }
  if (resp_ && split_) {  // the response view (its documents) in a step of its own
    wxWindow* w = resp_->window();
    resp_ = nullptr;
    resp_ph_ = new wxPanel(split_);  // response() is never called again, but keep the invariant
    resp_ph_->Hide();
    split_->ReplaceWindow(w, resp_ph_);
    w->Destroy();
    return true;
  }
  return false;
}

RequestEditorPanel* RequestTab::editor() const {
  if (req_.kind == RequestKind::WebSocket) return ws_;
  return req_.kind == RequestKind::Http ? static_cast<RequestEditorPanel*>(http_)
                                        : static_cast<RequestEditorPanel*>(grpc_);
}

void RequestTab::match_response_kind() {
  if (!resp_ || resp_ws_ == (req_.kind == RequestKind::WebSocket)) return;
  wxWindowUpdateLocker lock(split_);
  wxWindow* old = resp_->window();
  resp_ws_ = req_.kind == RequestKind::WebSocket;
  resp_ = resp_ws_ ? static_cast<ResponsePanel*>(make_ws_view(split_)) : make_response_view(split_);
  split_->ReplaceWindow(old, resp_->window());
  old->Destroy();
  resp_->clear();
  split_->UpdateSize();
  resp_->window()->Layout();
}

WsResponsePanel* RequestTab::ws_view() {
  match_response_kind();
  return resp_ws_ ? dynamic_cast<WsResponsePanel*>(response()) : nullptr;
}

RequestEditorPanel* RequestTab::ensure_editor(RequestKind kind) {
  RequestEditorPanel* ed;
  if (kind == RequestKind::Http) {
    if (!http_) http_ = make_http_editor(split_, this);
    ed = http_;
  } else if (kind == RequestKind::WebSocket) {
    if (!ws_) ws_ = make_ws_editor(split_, this);
    ed = ws_;
  } else {
    if (!grpc_) {
      // The gRPC engine (and its delay-loaded protobuf / gRPC DLLs, ~60 ms) starts now on a
      // thread of its own, while this editor is built and the user picks an address: never on
      // the UI thread at the first reflection or send.
      // A parked tab (restored from the session) waits for the user instead (wake_grpc).
      if (ctx_.grpc && !grpc_parked_) std::thread([g = ctx_.grpc] { g(); }).detach();
      grpc_ = make_grpc_editor(split_, this);
      if (manual_pending_) grpc_->set_manual(true);  // a restored session's Manual stream
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
  match_response_kind();
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
  } else if (req_.kind != RequestKind::Grpc) {
    t = req_.http.url.empty() ? "Untitled" : url_tail(req_.http.url);
  } else {
    // "/watt.test.Echo/Chat" -> "Echo/Chat", as the method picker shows it.
    std::string m = req_.grpc_method;
    if (!m.empty() && m[0] == '/') m.erase(0, 1);
    if (const auto sl = m.find('/'); sl != std::string::npos)
      if (const auto dot = m.rfind('.', sl); dot != std::string::npos) m.erase(0, dot + 1);
    t = m.empty() ? "Untitled" : m;
  }
  std::string prefix = req_.kind == RequestKind::Http        ? req_.http.method
                       : req_.kind == RequestKind::WebSocket ? std::string("WS")
                                                             : std::string("gRPC");
  // A name that already starts with the kind ("WS echo", "Get order") is not prefixed twice: its
  // first word becomes the method as every other tab shows it ("GET order"), so the strip does
  // not mix labels with and without a method.
  const bool named_with_kind = t.size() > prefix.size() && t[prefix.size()] == ' ' &&
                               wxString::FromUTF8(t.substr(0, prefix.size())).IsSameAs(wxString::FromUTF8(prefix), false);
  return wxString::FromUTF8(prefix + (named_with_kind ? t.substr(prefix.size()) : " " + t)).ToStdWstring() +
         (dirty_ ? L" \u25CF" : L"");
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
  match_response_kind();
  if (resp_) resp_->clear();
  if (on_title_changed) on_title_changed(this);
}

std::string RequestTab::suggested_name() const {
  store();
  std::string s;
  if (req_.kind != RequestKind::Grpc) {
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
    if (q == std::string::npos && req_.kind != RequestKind::Grpc) prev.clear();  // the host: not a name
    s = id && !prev.empty() ? prev + " " + last : last;
  }
  for (char& c : s)
    if (c == '\\' || c == ':' || c == '?' || c == '*' || c == '"' || c == '<' || c == '>' || c == '|' || c == '{' ||
        c == '}')
      c = '_';
  if (s.empty() && !req_.name.empty() && req_.name.find("{{") == std::string::npos) s = req_.name;
  // Nothing but variables or a bare host ("{{base}}", "{{base}}/"): the URL itself, as Postman
  // names an unsaved request, rather than a generic "request".
  if (s.empty() && req_.kind != RequestKind::Grpc) {
    for (char c : req_.http.url.substr(0, req_.http.url.find_first_of("?#"))) {
      if (c == '{' || c == '}') continue;
      if (c == '\\' || c == ':' || c == '*' || c == '"' || c == '<' || c == '>' || c == '|' || c == '/') c = ' ';
      if (c != ' ' || (!s.empty() && s.back() != ' ')) s += c;
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
  }
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
  if (last_.ws_log) ws_log_ = std::make_shared<std::vector<WsMessage>>(*last_.ws_log);
  shown_method_ = last_.grpc ? req_.grpc_method : std::string();
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
  // A WebSocket tab's restored message log, under its "last session" handshake.
  if (last_.http && last_.ws_log && resp_ws_)
    if (WsResponsePanel* v = ws_view()) v->ws_messages(std::vector<WsMessage>(*last_.ws_log));
}

bool RequestTab::grpc_manual() const {
  if (req_.kind != RequestKind::Grpc) return false;
  return grpc_ ? grpc_->manual_wanted() : manual_pending_;
}

void RequestTab::restore_grpc_manual(bool on) {
  manual_pending_ = on;
  if (grpc_) grpc_->set_manual(on);
}

LastResponse RequestTab::session_response() {
  if (req_.kind == RequestKind::WebSocket && last_.http && ws_log_ && ws_log_changed_) {
    last_.ws_log = std::make_shared<const std::vector<WsMessage>>(*ws_log_);
    ws_log_changed_ = false;
  }
  return last_;
}

void RequestTab::ws_log_add(const std::vector<WsMessage>& batch) {
  if (!ws_log_) ws_log_ = std::make_shared<std::vector<WsMessage>>();
  for (const WsMessage& m : batch) {
    WsMessage c;
    c.out = m.out;
    c.binary = m.binary;
    c.at_ms = m.at_ms;
    c.data = m.data.size() > LastResponse::kLogMessageBytes ? m.data.substr(0, LastResponse::kLogMessageBytes) : m.data;
    ws_log_->push_back(std::move(c));
  }
  if (ws_log_->size() > LastResponse::kLogMessages)
    ws_log_->erase(ws_log_->begin(), ws_log_->begin() + static_cast<std::ptrdiff_t>(ws_log_->size() - LastResponse::kLogMessages));
  ws_log_changed_ = true;
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
      if (auto r = ws->load_request(to)) {
        req_.name = r->name;
        // The editor shows the name too (the HTTP breadcrumb): tell it, or reload a clean one.
        RequestEditorPanel* ed = realized() ? editor() : nullptr;
        if (ed && !ed->adopt_name(req_.name) && !dirty_ && req_.kind == RequestKind::Http) {
          loading_ = true;
          ed->load(req_);
          loading_ = false;
        }
      }
  if (on_title_changed) on_title_changed(this);
}

// ------------------------------------------------------------------ EditorHost

void RequestTab::changed() {
  if (loading_) return;
  editor()->store_head(req_);  // cheap: name, method, url, target (never the body)
  // Another gRPC method picked: the response and timing on screen belong to the old one, so
  // they go (as switching the request would) instead of standing under the new method.
  if (req_.kind == RequestKind::Grpc && !in_flight_ && last_.grpc && !shown_method_.empty()) {
    auto bare = [](const std::string& m) { return m.empty() || m[0] != '/' ? m : m.substr(1); };
    if (bare(req_.grpc_method) != bare(shown_method_)) {
      shown_method_.clear();
      last_ = {};
      last_pending_ = false;
      if (resp_) resp_->clear();
    }
  }
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
  if (req_.kind == RequestKind::WebSocket && ws_open_) {
    // Ctrl+Enter / Send while connected: the composer's message on the open connection.
    store();
    ws_send_message();
    return;
  }
  if (in_flight_) cancel();
  last_pending_ = false;
  store();
  // A preview tab that was sent holds a response the user is looking at: the next single click in
  // the tree or history must not replace it (VS Code / Postman keep a tab once it is used).
  make_permanent();
  if (req_.kind == RequestKind::Http)
    send_http(click);
  else if (req_.kind == RequestKind::WebSocket)
    send_ws(click);
  else
    send_grpc(click);
}

// ------------------------------------------------------------------ WebSocket

void RequestTab::send_ws(Clock::time_point click) {
  Workspace* ws = ctx_.workspace ? ctx_.workspace() : nullptr;
  if (req_.http.url.empty()) {
    response()->show_error("Enter a ws:// or wss:// address first.");
    return;
  }
  // The upgrade request is an HTTP GET as far as inheritance, {{vars}}, auth and every consent
  // check go: it is built exactly like one (http_to_send), from a copy with kind Http and the
  // ws(s):// scheme written as http(s):// (a scheme inside a {{var}} is mapped by WsClient).
  auto rq_p = std::make_shared<SavedRequest>(for_inheritance(req_));
  SavedRequest& rq = *rq_p;
  rq.kind = RequestKind::Http;
  rq.http.method = "GET";
  rq.http.body.clear();
  rq.form.clear();
  rq.body_file.clear();
  rq.body_data.clear();
  if (std::string u = ws_http_url(rq.http.url); !u.empty()) rq.http.url = std::move(u);
  bool prompted = false;
  if (ws && !confirm_http_consents(*ws, rq, &prompted)) return;
  if (ws && !confirm_unresolved(*ws, rq, &prompted)) return;
  if (prompted) click = Clock::now();
  const uint64_t gen = ++gen_;
  in_flight_ = true;
  ws_open_ = false;
  sent_at_ = click;
  if (drop_snapshot_time(req_) && on_title_changed) on_title_changed(this);
  std::shared_ptr<const SavedRequest> h = std::move(rq_p);
  auto saved = std::make_shared<const SavedRequest>(req_);  // as written, for the history row
  auto wsc = ws ? std::make_shared<Workspace>(*ws) : nullptr;
  auto w = worker_;
  w->push([w, gen, click, h, saved, wsc] {
    if (w->is_cancelled(gen)) return;
    std::string err;
    std::optional<HttpRequest> r;
    if (wsc) r = wsc->http_to_send(*h, &err);
    else r = h->http;
    if (!r) {
      w->post([gen, err](RequestTab& t) { t.on_error(gen, err); });
      return;
    }
    auto client = std::make_shared<WsClient>();
    {
      std::lock_guard lk(w->call_m);
      w->ws = client;
      if (w->is_cancelled(gen)) client->cancel();
    }
    const double ui_queue = ms_between(click, Clock::now());
    auto hs = std::make_shared<const WsHandshake>(client->connect(*r));
    auto sent = std::make_shared<const HttpRequest>(std::move(*r));
    w->post([gen, hs, sent, ui_queue](RequestTab& t) { t.on_ws_handshake(gen, hs, sent, ui_queue); });
    if (wsc && hs->error != "cancelled") {
      HistoryEntry e;
      e.when_iso = now_iso();
      e.kind = RequestKind::Http;  // listed like an HTTP row ("WS" badge, 101); the request is a WebSocket one
      e.summary = make_history_summary("WS " + saved->http.url, ws_display_url(sent->url), wsc->active_env);
      e.status = hs->status;
      e.total_ms = ui_queue + hs->total_ms;
      e.request = *saved;
      {
        std::lock_guard lk(history_mutex());
        wsc->append_history(e);
      }
      w->post([e = std::move(e)](RequestTab& t) {
        if (t.ctx_.history_added) t.ctx_.history_added(e);
      });
    }
    if (hs->ok) {
      // Batches of at most one per 50 ms (WsClient::run), so a burst never floods the UI queue.
      auto c = std::make_shared<const WsClose>(client->run([w, gen](std::vector<WsMessage>&& b) {
        auto p = std::make_shared<std::vector<WsMessage>>(std::move(b));
        w->post([gen, p](RequestTab& t) { t.on_ws_batch(gen, p); });
      }));
      w->post([gen, c](RequestTab& t) { t.on_ws_closed(gen, c); });
    }
    std::lock_guard lk(w->call_m);
    if (w->ws == client) w->ws.reset();
  });
  editor()->set_in_flight(true);
  if (WsResponsePanel* v = ws_view()) v->show_sending(RequestKind::WebSocket);
}

void RequestTab::on_ws_handshake(uint64_t gen, std::shared_ptr<const WsHandshake> hs,
                                 std::shared_ptr<const HttpRequest> sent, double queue_ms) {
  if (gen != gen_ || !in_flight_) return;
  WsResponsePanel* v = ws_view();
  if (!v) return;
  if (hs->error == "cancelled") return;  // cancel() showed it already
  v->ws_handshake(hs, sent, queue_ms);
  auto s = std::make_shared<SentHttp>();
  s->request = *sent;
  last_ = LastResponse{ws_handshake_response(*hs, queue_ms), s, nullptr};
  last_.when_iso = now_iso();
  ws_log_changed_ = ws_log_ != nullptr;  // the log of earlier connections stays with the new handshake
  last_pending_ = false;
  if (hs->ok) {
    ws_open_ = true;
    if (ws_) ws_->set_state(WsEditorPanel::State::Open);
  } else {
    finish(gen);
  }
  if (on_response) on_response(this);
}

void RequestTab::on_ws_batch(uint64_t gen, std::shared_ptr<std::vector<WsMessage>> batch) {
  if (gen != gen_ || !ws_open_) return;
  ws_log_add(*batch);
  if (on_response) on_response(this);  // the session keeps the log
  if (WsResponsePanel* v = ws_view()) v->ws_messages(std::move(*batch));
}

void RequestTab::on_ws_closed(uint64_t gen, std::shared_ptr<const WsClose> c) {
  if (gen != gen_ || !in_flight_) return;
  ws_open_ = false;
  finish(gen);
  if (WsResponsePanel* v = ws_view()) v->ws_closed(*c);
}

void RequestTab::ws_send_message() {
  std::shared_ptr<WsClient> c;
  {
    std::lock_guard lk(worker_->call_m);
    c = worker_->ws;
  }
  WsResponsePanel* v = ws_view();
  if (ws_ && c && !ws_->confirm_message()) return;
  std::string text = req_.http.body;
  if (const Workspace* ws = workspace(); ws && text.find("{{") != std::string::npos)
    text = ws->expand(text, for_inheritance(req_));  // {{vars}} in a message, as Postman and Insomnia do
  std::string err;
  if (!c || !c->send(std::move(text), false, &err)) {
    if (v) v->ws_info(err.empty() ? "Not connected." : err, true);
  }
}

void RequestTab::ws_close() {
  std::shared_ptr<WsClient> c;
  {
    std::lock_guard lk(worker_->call_m);
    c = worker_->ws;
  }
  if (c && ws_open_) c->close(1000, {});
  else cancel();
}

void RequestTab::send_http(Clock::time_point click, bool checked) {
  Workspace* ws = ctx_.workspace ? ctx_.workspace() : nullptr;
  if (req_.http.url.empty()) {
    response()->show_error("Enter a URL first.");
    return;
  }
  // What is checked and sent: the tab's request, inheriting from its folders (a history snapshot
  // from the folders of the request it came from; an unsaved one from the folder it will be saved in).
  auto rq_p = std::make_shared<SavedRequest>(for_inheritance(req_));
  const SavedRequest& rq = *rq_p;
  // Fast path: the folder chain, the consent lookups and the variable check below cost 0.3-1 ms
  // on the UI thread for every send, and they almost never find anything to ask. So the worker
  // builds the request right away: http_to_send() is the gate for every consent (it refuses what
  // is not allowed) and the worker comes back here (on_send_bounced -> checked) whenever
  // something needs the user, which then asks exactly as before.
  if (!checked && ws) {
    send_http_job(click, std::move(rq_p), ws, false, false, false, false, false, /*fast=*/true);
    return;
  }
  // Files from outside the workspace and OS environment variables need the user's consent
  // (Workspace::blocked_files / blocked_env_vars). http_to_send() refuses them regardless, so
  // this is the prompt, not the gate. Only requests that go through http_to_send can reach them.
  // Time the user spends answering a consent prompt is not request time: after any prompt the
  // clock restarts, so queue_ms / total_ms (and asserts on res.responseTime) stay honest.
  bool prompted = false;
  // Folder / collection settings (folder.json) add headers, auth, vars and scripts that only
  // http_to_send applies: such a request never takes the fast path.
  const bool inherited = ws && ws->has_inherited_settings(rq);
  if (ws && (needs_workspace(rq) || inherited) && !confirm_http_consents(*ws, rq, &prompted)) return;
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
      before_prompt("run this workspace's scripts?");
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
  // A pre-request script may define the variables itself: then nothing is known to be missing.
  if (ws && !has_pre && (needs_workspace(rq) || inherited) && !confirm_unresolved(*ws, rq, &prompted)) return;
  if (prompted) click = Clock::now();
  send_http_job(click, std::move(rq_p), ws, inherited, run_scripts, scripts_skipped, has_pre, has_post, false);
}

void RequestTab::on_send_bounced(uint64_t gen) {
  if (gen != gen_ || !in_flight_) return;  // cancelled or superseded meanwhile
  in_flight_ = false;
  // Not in flight until the checked path sends again (send_http_job sets it back): a prompt
  // declined or a "Not sent" banner leaves Send a Send button, never a Cancel that cancels nothing.
  editor()->set_in_flight(false);
  send_http(sent_at_, /*checked=*/true);  // the click time stays unless a prompt is answered
}

void RequestTab::not_sent(const std::string& why) {
  if (!in_flight_) response()->show_error(why + " See the banner above the request.");
}

void RequestTab::before_prompt(const std::string& what) {
  if (!in_flight_) response()->show_waiting_for_user(what);
}

bool RequestTab::confirm_unresolved(const Workspace& ws, const SavedRequest& rq, bool* prompted) {
  bool in_url = false;
  const std::vector<std::string> miss = unresolved_vars(ws, rq, &in_url);
  if (miss.empty()) return true;
  // "Send as written" was chosen for each of these in this tab.
  if (std::all_of(miss.begin(), miss.end(), [&](const std::string& n) { return unresolved_ok_.count(n) > 0; }))
    return true;
  std::string names;
  for (const auto& n : miss) names += (names.empty() ? "{{" : ", {{") + n + "}}";
  const std::string not_sent =
      "Not sent: " + names + (miss.size() == 1 ? " is" : " are") + " not defined (" +
      (ws.active_env.empty() ? std::string("no environment") : "environment " + ws.active_env) + ").";
  // The same "Not sent" banner the editor shows for its own check: one condition, one UI. Its
  // "Send as written" resends with these names accepted (the click time is the new click).
  if (editor()->show_unresolved(miss, in_url, [this, miss] {
        unresolved_ok_.insert(miss.begin(), miss.end());
        send();
      })) {
    response()->show_error(not_sent + " See the banner above the request.");
    return false;
  }
  *prompted = true;
  before_prompt("send undefined variables?");
  wxString list;
  for (const auto& n : miss) list += "    {{" + wxString::FromUTF8(n) + "}}\n";
  const wxString env = ws.active_env.empty() ? wxString("no environment is active")
                                             : "the environment '" + wxString::FromUTF8(ws.active_env) + "'";
  wxMessageDialog dlg(this,
                      wxString::Format("%s not defined in %s, its folders or the globals:\n\n%s\nThe request would "
                                       "send %s literally. Send anyway?",
                                       miss.size() == 1 ? "This variable is" : "These variables are", env, list,
                                       miss.size() == 1 ? "it" : "them"),
                      "watt", wxOK | wxCANCEL | wxCANCEL_DEFAULT | wxICON_WARNING);
  dlg.SetOKCancelLabels("&Send anyway", "Cancel");  // a real Cancel: Esc declines
  if (dlg.ShowModal() != wxID_OK) {
    response()->show_error(not_sent + " Define it in the environment, or send anyway.");
    return false;
  }
  unresolved_ok_.insert(miss.begin(), miss.end());
  return true;
}

bool RequestTab::confirm_http_consents(Workspace& wsr, const SavedRequest& rq, bool* prompted_out) {
  Workspace* ws = &wsr;
  bool prompted = false;
  struct Out {
    bool* p;
    bool* v;
    ~Out() { *p = *p || *v; }
  } out{prompted_out, &prompted};
  {
    prompted = !ws->blocked_env_vars(rq).empty();
    if (prompted) before_prompt("read an environment variable?");
    if (std::string err; !confirm_env_vars(*ws, rq, this, &err)) {
      response()->show_error(err);
      return false;
    }
    for (const auto& p : ws->blocked_files(rq)) {
      prompted = true;
      before_prompt("upload a file from outside the workspace?");
      const auto ans = wxMessageBox(
          wxString::Format("This request wants to upload\n\n%s\n\nfrom outside the workspace. Allow it?",
                           p.wstring()),
          "watt", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);
      std::string err;
      if (ans != wxYES || !ws->allow_file(p, &err)) {
        response()->show_error(err.empty() ? "Upload not allowed: " + p.string() : err);
        return false;
      }
    }
    // NTLM / Negotiate with no user name signs in as the current Windows user: a cloned
    // workspace could aim that at a hostile host, so each host needs the user's consent.
    for (const auto& host : ws->blocked_sso_hosts(rq)) {
      prompted = true;
      before_prompt("sign in with your Windows account?");
      const auto ans = wxMessageBox(
          wxString::Format("This request signs in to\n\n%s\n\nwith your Windows account (%s, no user name "
                           "given). Only allow this for servers you trust. Allow it for this workspace?",
                           wxString::FromUTF8(host),
                           rq.auth.type == HttpAuth::Type::Ntlm ? "NTLM" : "Negotiate"),
          "watt", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);
      std::string err;
      if (ans != wxYES || !ws->allow_sso_host(host, &err)) {
        response()->show_error(err.empty() ? "Windows sign-in not allowed for " + host : err);
        return false;
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
        before_prompt("trust a network setting?");
        if (!confirm_network_item(it, this)) {
          response()->show_error("Not sent: " + it.text + " (set in the " + it.level + ") is not allowed.");
          return false;
        }
        std::string err;
        if (!ws->allow_network_item(it, &err)) {
          response()->show_error(err.empty() ? "Not allowed: " + it.text : err);
          return false;
        }
      }
    }
  }
  return true;
}

void RequestTab::send_http_job(Clock::time_point click, std::shared_ptr<SavedRequest> rq_p, Workspace* ws,
                               bool inherited, bool run_scripts, bool scripts_skipped, bool has_pre, bool has_post,
                               bool fast) {
  const uint64_t gen = ++gen_;
  in_flight_ = true;
  sent_at_ = click;
  if (drop_snapshot_time(req_) && on_title_changed) on_title_changed(this);
  // The job is queued before any UI update, so the worker starts while the UI repaints.
  std::shared_ptr<const SavedRequest> saved = std::move(rq_p);
  auto wsc = ws ? std::make_shared<Workspace>(*ws) : nullptr;
  auto w = worker_;
  std::shared_ptr<HttpBodyTap> tap = response()->body_tap();  // live SSE events (UI thread, by value)
  const std::set<std::string> acked = unresolved_ok_;
  w->push([w, gen, click, saved, wsc, run_scripts, scripts_skipped, has_pre, has_post, inherited, tap, fast,
           acked]() mutable {
    if (w->is_cancelled(gen)) return;
    // Fast path (see send_http): nothing was checked on the UI thread. Anything that may need
    // the user (a consent, an untrusted script, a variable that is not defined) sends the request
    // back to the UI thread's checked path instead of going out.
    auto bounce = [&] { w->post([gen](RequestTab& t) { t.on_send_bounced(gen); }); };
    if (fast && wsc) {
      inherited = wsc->has_inherited_settings(*saved);
      const EffectiveSettings eff = wsc->effective_settings(*saved);
      has_pre = !eff.pre_scripts.empty();
      has_post = !eff.post_scripts.empty();
      // A pre-request script runs before the request is built: consents are asked before it.
      if (has_pre || (has_post && !wsc->scripts_trusted())) return bounce();
      run_scripts = has_post;
    }
    // The request a pre-request script may edit (below); history records it as it went out.
    auto toSend = std::make_shared<SavedRequest>(*saved);
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
      // After the pre-request script (its URL edits), without the editor's encode marks.
      e.summary = make_history_summary(saved->http.method + " " + strip_var_marks(saved->http.url),
                                       wsc->expand(toSend->http.url, *toSend), wsc->active_env);
      e.status = status;
      e.total_ms = total_ms;
      e.request = *saved;
      e.request.http.url = strip_var_marks(e.request.http.url);
      e.request.http.body = strip_var_marks(e.request.http.body);
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
    // Pre-request script: edits a copy of the request (toSend); its variable writes apply to this
    // worker's workspace copy (so this send sees them) and later to the real one (UI thread).
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
          if (fast) return bounce();  // perhaps a consent (an OS environment variable) is missing
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
        // http_to_send() is the gate for every consent: the checked path asks the user (or shows
        // this error again when there is nothing to ask).
        if (fast) return bounce();
        w->post([gen, err](RequestTab& t) { t.on_error(gen, err); });
        return;
      }
      sent->request = std::move(*r);
      if (fast && !has_pre) {
        const std::vector<std::string> miss = unresolved_in(sent->request);
        if (std::any_of(miss.begin(), miss.end(), [&](const std::string& n) { return !acked.count(n); }))
          return bounce();
      }
    } else {
      sent->request = toSend->http;
    }
    if (w->is_cancelled(gen)) return;
    const std::string origin = Workspace::url_origin(sent->request.url);
    HttpClient& client = w->lease_client(origin);
    // Progress: at most every 50 ms.
    uint64_t got = 0;
    auto last = Clock::now();
    auto on_chunk = [&](const char* data, size_t n) {
      if (tap) tap->feed(data, n);
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
    // The head (status, headers, phases to the first byte) as soon as it arrives: a stream that
    // never ends, or a send cancelled mid-body, still shows them.
    HeadFn on_head;
    if (tap)
      on_head = [&](const HttpResponse& h) {
        auto c = std::make_shared<HttpResponse>(h);
        c->timing.queue_ms += ui_queue;
        c->timing.total_ms += ui_queue;
        tap->head(std::move(c));
      };
    if (tap) {
      auto copy = std::make_shared<SentHttp>(*sent);
      copy->pre_script = pre;
      tap->sent(std::move(copy));
    }
    auto resp = std::make_shared<HttpResponse>(client.send(sent->request, on_chunk, on_head));
    w->give_back(origin);
    resp->timing.queue_ms += ui_queue;
    resp->timing.total_ms += ui_queue;
    if (run_scripts && has_post && resp->ok) {
      ResponseData rd(*resp);
      // Captures first (into this worker's workspace copy, not saved): the post-response script
      // reads this response's captured values, not the previous run's.
      apply_captures(*toSend, rd, *wsc, false);
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

    // A cancelled send is a history row too, so it can be opened and sent again: what arrived
    // and its time when the head was in, else status -1 ("cancelled" in the history list).
    record_history(resp->error == "cancelled" && resp->status <= 0 ? -1 : resp->status, resp->timing.total_ms);
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
  shown_method_ = req_.grpc_method;
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
      w->first_send = w->last_send = w->end_sent = Clock::time_point{};
      w->sends = 0;
      if (w->is_cancelled(gen)) call->cancel();
    }
    if (!call->done()) {
      // Manual: nothing goes out until the user clicks Send (RequestTab::stream_send).
      if (!interactive) {
        for (const auto& m : split_messages(r.grpc_body)) {
          std::string err;
          if (!call->send(m, &err)) {
            call->cancel();
            w->post([gen, err](RequestTab& t) { t.on_error(gen, "Request message: " + one_space(err)); });
            break;
          }
        }
        call->close_send();
      }
    }
    auto resp = std::make_shared<GrpcResponse>(call->wait());
    Clock::time_point first_send, last_send, end_sent;
    int sends = 0;
    {
      std::lock_guard lk(w->call_m);
      w->call.reset();
      first_send = w->first_send;
      last_send = w->last_send;
      end_sent = w->end_sent;
      sends = w->sends;
    }
    // A Manual stream: one clock from the stream opening (as the message offsets), with the
    // user's pace recorded beside it: what Timing and res.responseTime make of it is in
    // GrpcResponse::pacing.
    if (interactive) {
      auto& pc = resp->pacing;
      const double engine_total = resp->timing.total_ms;
      auto at = [&](Clock::time_point t) {
        return t == Clock::time_point{} ? -1.0 : std::clamp(ms_between(start, t), 0.0, engine_total);
      };
      pc.on = true;
      pc.sent = sends;
      pc.first_send_ms = at(first_send);
      pc.last_send_ms = at(last_send);
      pc.end_ms = at(end_sent);
      pc.tail_ms = std::max(0.0, engine_total - std::max({0.0, pc.last_send_ms, pc.end_ms}));
    }
    resp->timing.queue_ms += ui_queue;
    resp->timing.total_ms += ui_queue;
    std::shared_ptr<const GrpcResponse> cresp = resp;
    w->post([gen, cresp](RequestTab& t) { t.on_grpc_done(gen, cresp); });

    if (wsc) {  // a cancelled call too (CANCELLED in the history list), so it can be sent again
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
  if (interactive) response()->show_paced(0, false);
}

void RequestTab::cancel() {
  if (pending_send_) {
    pending_send_ = false;
    response()->show_cancelled(0);
    return;
  }
  if (!in_flight_) {
    editor()->set_in_flight(false);  // never a Cancel button with nothing to cancel
    return;
  }
  worker_->cancel(gen_);
  ws_open_ = false;
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
  if (gen != gen_) return;
  if (!in_flight_) {
    // Cancelled mid-stream (cancel() already showed what arrived): the session still keeps it.
    if (r && r->error == "cancelled" && r->status > 0) {
      last_ = LastResponse{r, sent, nullptr};
      last_.when_iso = now_iso();
      last_pending_ = false;
      if (on_response) on_response(this);
    }
    return;
  }
  finish(gen);
  last_ = LastResponse{r, sent, nullptr};
  // The headers it inherited from folders / the collection, as templates (never expanded: no
  // secret is stored): the session restores them into the sent request (X-Team: core).
  if (const Workspace* ws = workspace(); ws && sent && ws->has_inherited_settings(for_inheritance(req_))) {
    for (const auto& h : ws->resolved(for_inheritance(req_)).http.headers)
      if (!has_header_named(req_.http.headers, h.first)) last_.inherited_headers.push_back(h);
  }
  last_.when_iso = now_iso();
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
  last_.when_iso = now_iso();
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
  if (!call->send(json, error)) return false;
  int sent = 0;
  bool ended = false;
  {
    std::lock_guard lk(worker_->call_m);
    if (worker_->call == call) {
      const auto now = Clock::now();
      if (worker_->first_send == Clock::time_point{}) worker_->first_send = now;
      worker_->last_send = now;
      sent = ++worker_->sends;
      ended = worker_->end_sent != Clock::time_point{};
    }
  }
  if (sent > 0) response()->show_paced(sent, ended);
  return true;
}

void RequestTab::stream_close() {
  std::shared_ptr<GrpcCall> call;
  int sent = 0;
  bool first_end = false;
  {
    std::lock_guard lk(worker_->call_m);
    call = worker_->call;
    if (call && worker_->end_sent == Clock::time_point{}) {
      worker_->end_sent = Clock::now();
      first_end = true;
    }
    sent = worker_->sends;
  }
  if (call) call->close_send();
  if (first_end) response()->show_paced(sent, true);
}

}  // namespace watt::app
