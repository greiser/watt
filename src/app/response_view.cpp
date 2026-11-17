// Response view: status strip (status, total, client overhead, size, protocol, remote address),
// a timing waterfall with per-phase tooltips and a delta against the previous run of the same
// request (method + expanded URL + body; gRPC: target + method + body), and tabs for the body (response_body.*), headers, gRPC trailers, the live gRPC
// message list and the request as sent.
//
// Everything that scales with the response size (pretty-printing, hex, Scintilla line indexing,
// search counts) runs on the body view's worker; the UI thread only fills the strip, the
// waterfall and virtual lists (which ask for the rows they draw).
#include <wx/listctrl.h>
#include <wx/textctrl.h>
#include <wx/panel.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/clipbrd.h>
#include <wx/menu.h>
#include <wx/datetime.h>
#include <wx/app.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <thread>

#include "core/asserts.h"

#include "app/panels.h"
#include "app/request_tab.h"
#include "app/response_body.h"
#include "app/response_conn.h"
#include "app/response_format.h"
#include "app/response_tests.h"
#include "app/response_tests_view.h"
#include "app/response_widgets.h"

namespace watt::app {
namespace {

using namespace rv;
using KV = std::vector<std::pair<std::string, std::string>>;

wxString U(std::string_view s) { return wxString::FromUTF8(s.data(), s.size()); }

// ------------------------------------------------------------------ virtual lists

class KvList : public wxListCtrl {
 public:
  KvList(wxWindow* parent, const char* c1, const char* c2)
      : wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                   wxLC_REPORT | wxLC_VIRTUAL | wxBORDER_NONE) {
    AppendColumn(c1, wxLIST_FORMAT_LEFT, FromDIP(200));
    AppendColumn(c2, wxLIST_FORMAT_LEFT, FromDIP(400));
    // Last column fills the width. Deferred: setting a column width inside the size event can
    // toggle the scrollbars and re-enter the handler (unbounded recursion on some widths).
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      if (fit_pending_) return;
      fit_pending_ = true;
      CallAfter([this] {
        fit_pending_ = false;
        const int w = GetClientSize().x - GetColumnWidth(0) - 2;
        if (w > FromDIP(80) && w != GetColumnWidth(1)) SetColumnWidth(1, w);
      });
    });
    Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
      if (e.GetModifiers() == wxMOD_CONTROL && e.GetKeyCode() == 'C') return copy(false);
      if (e.GetModifiers() == wxMOD_CONTROL && e.GetKeyCode() == 'A') {
        for (long i = 0; i < GetItemCount(); ++i) SetItemState(i, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
        return;
      }
      e.Skip();
    });
    Bind(wxEVT_CONTEXT_MENU, [this](wxContextMenuEvent&) {
      wxMenu m;
      m.Append(1, "Copy selected\tCtrl+C");
      m.Append(2, "Copy all");
      m.Append(3, "Copy value");
      const int id = GetPopupMenuSelectionFromUser(m);
      if (id == 1) copy(false);
      else if (id == 2) copy(true);
      else if (id == 3) {
        const long i = GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (i >= 0) to_clipboard(U(rows_[static_cast<size_t>(i)].second));
      }
    });
  }
  // dim[i] != 0: row i is drawn dimmed (e.g. a header the HTTP stack added by itself).
  void set(KV rows, std::vector<char> dim = {}) {
    rows_ = std::move(rows);
    dim_ = std::move(dim);
    dim_.resize(rows_.size(), 0);
    SetItemCount(static_cast<long>(rows_.size()));
    Refresh();
  }
  size_t count() const { return rows_.size(); }
  const KV& rows() const { return rows_; }

 private:
  wxString OnGetItemText(long item, long col) const override {
    const auto& r = rows_[static_cast<size_t>(item)];
    return U(col == 0 ? r.first : r.second);
  }
  wxItemAttr* OnGetItemAttr(long item) const override {
    return dim_[static_cast<size_t>(item)] ? &dim_attr_ : nullptr;
  }
  static void to_clipboard(const wxString& s) {
    if (wxTheClipboard->Open()) {
      wxTheClipboard->SetData(new wxTextDataObject(s));
      wxTheClipboard->Close();
    }
  }
  void copy(bool all) {
    std::string s;
    for (size_t i = 0; i < rows_.size(); ++i)
      if (all || GetItemState(static_cast<long>(i), wxLIST_STATE_SELECTED))
        s += rows_[i].first + ": " + rows_[i].second + "\n";
    if (!s.empty()) to_clipboard(U(s));
  }
  KV rows_;
  std::vector<char> dim_;
  mutable wxItemAttr dim_attr_{Theme::get().dim, wxNullColour, wxNullFont};
  bool fit_pending_ = false;
};

struct MsgRow {
  double at_ms = 0;
  double gap_ms = 0;
  size_t bytes = 0;
  std::string preview;
  bool error = false;
};

class MsgList : public wxListCtrl {
 public:
  MsgList(wxWindow* parent, std::function<void(size_t)> on_open)
      : wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                   wxLC_REPORT | wxLC_VIRTUAL | wxLC_SINGLE_SEL | wxBORDER_NONE),
        on_open_(std::move(on_open)) {
    AppendColumn("#", wxLIST_FORMAT_RIGHT, FromDIP(52));
    AppendColumn("Offset", wxLIST_FORMAT_RIGHT, FromDIP(86));
    AppendColumn("Gap", wxLIST_FORMAT_RIGHT, FromDIP(80));
    AppendColumn("Size", wxLIST_FORMAT_RIGHT, FromDIP(70));
    AppendColumn("Message", wxLIST_FORMAT_LEFT, FromDIP(400));
    err_attr_.SetTextColour(Theme::get().err);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      if (fit_pending_) return;
      fit_pending_ = true;
      CallAfter([this] {
        fit_pending_ = false;
        int used = 0;
        for (int c = 0; c < 4; ++c) used += GetColumnWidth(c);
        const int w = GetClientSize().x - used - 2;
        if (w > FromDIP(80) && w != GetColumnWidth(4)) SetColumnWidth(4, w);
      });
    });
    Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent& e) {
      if (on_open_) on_open_(static_cast<size_t>(e.GetIndex()));
    });
  }
  void reset() {
    rows_.clear();
    SetItemCount(0);
    Refresh();
  }
  void append(const GrpcMessageEvent* m, size_t n) {
    // Follow the tail only while the user is looking at it.
    const long count = GetItemCount();
    const bool at_end = count == 0 || GetTopItem() + GetCountPerPage() >= count;
    for (size_t i = 0; i < n; ++i) {
      MsgRow r;
      r.at_ms = m[i].at_ms;
      r.gap_ms = rows_.empty() ? m[i].at_ms : m[i].at_ms - rows_.back().at_ms;
      r.bytes = m[i].wire_bytes;
      r.error = !m[i].decode_error.empty();
      const std::string& src = r.error ? m[i].decode_error : m[i].json;
      // One-line preview: whitespace runs collapsed, at most 160 characters.
      r.preview.reserve(std::min<size_t>(src.size(), 160));
      bool ws = false;
      for (char c : src) {
        if (r.preview.size() >= 160) {
          r.preview += "\xE2\x80\xA6";
          break;
        }
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
          ws = true;
          continue;
        }
        if (ws && !r.preview.empty()) r.preview += ' ';
        ws = false;
        r.preview += c;
      }
      if (r.error) r.preview = "decode error: " + r.preview;
      rows_.push_back(std::move(r));
    }
    SetItemCount(static_cast<long>(rows_.size()));
    // (Not while hidden: the page size is then meaningless and row #1 would scroll out of view.)
    if (at_end && count > 0 && IsShownOnScreen()) EnsureVisible(static_cast<long>(rows_.size()) - 1);
    Refresh();
  }
  size_t count() const { return rows_.size(); }

 private:
  wxString OnGetItemText(long item, long col) const override {
    const MsgRow& r = rows_[static_cast<size_t>(item)];
    switch (col) {
      case 0: return wxString::Format("%ld", item + 1);
      case 1: return U("+" + fmt::ms(r.at_ms));
      case 2: return U(fmt::ms(r.gap_ms));
      case 3: return U(fmt::human_bytes(r.bytes));
      default: return U(r.preview);
    }
  }
  wxItemAttr* OnGetItemAttr(long item) const override {
    return rows_[static_cast<size_t>(item)].error ? &err_attr_ : nullptr;
  }
  std::vector<MsgRow> rows_;
  bool fit_pending_ = false;
  mutable wxItemAttr err_attr_;
  std::function<void(size_t)> on_open_;
};

// ------------------------------------------------------------------ timing models

namespace tips {
const char* kPrepare =
    "watt's own request setup: libcurl options, header list, adding the transfer to the multi "
    "handle (send() -> request handed to libcurl).";
const char* kQueueHttp =
    "From the Send click to I/O starting: the UI-to-worker hand-off plus libcurl's internal queue "
    "(URL parse, connection-pool lookup and liveness check).";
const char* kDnsHttp =
    "Name resolution. watt's DNS phase ends when the winning socket opens (happy eyeballs), unlike "
    "curl's time_namelookup, which ends when the resolver answers. 0 for a reused connection.";
const char* kSocket = "Name resolved -> socket created and configured (Postman's \"Socket Initialization\").";
const char* kConnectHttp = "TCP handshake: SYN sent -> connection established.";
const char* kTlsHttp =
    "TLS handshake: TCP connected -> handshake complete (certificate check and ALPN included). "
    "n/a for plain http://.";
const char* kSendHttp = "Request upload: connection ready -> request line, headers and body fully written.";
const char* kWait =
    "Server wait (TTFB): request fully sent -> first response byte. Server processing plus one "
    "network round trip.";
const char* kDownloadHttp =
    "First response byte -> last byte received and decoded (headers, body, content decoding).";
const char* kProcess =
    "libcurl done -> send() returns: returning the connection to the pool and watt's result "
    "collection. curl -w time_total leaves this out; watt counts it so the total is real wall time.";
const char* kTotalHttp =
    "Send click -> complete response available: the wall time you waited. Exactly the sum of the phases.";
const char* kQueueGrpc = "From the Send click until the call started on the engine (UI-to-worker hand-off).";
const char* kDnsGrpc =
    "watt's own name lookup for a new channel. 0 for IP literals and for a channel that was already "
    "connected.";
const char* kConnectGrpc =
    "Channel setup + TCP connect this call waited for (plaintext: also the HTTP/2 SETTINGS exchange).";
const char* kTlsGrpc = "TCP connected -> channel READY: the TLS handshake and HTTP/2 SETTINGS. n/a for plaintext.";
const char* kReflect =
    "Server reflection before the call: channel READY -> schema resolved (only on the first call "
    "without .proto files).";
const char* kSendGrpc = "Request message(s) serialised and written to the stream.";
const char* kDownloadGrpc = "First response message -> trailers received (all messages decoded).";
const char* kOther = "Time inside the call not attributed to any phase above.";
const char* kTotalGrpc = "Send click -> final status received: the wall time you waited.";
}  // namespace tips

// Phase colours: setup greys, connection blues/purple, server wait green, transfer amber/blue.
struct Pal {
  wxColour setup, dns, socket, connect, tls, send, wait, down, redirect;
};
Pal palette() {
  const bool d = Theme::get().dark;
  return d ? Pal{wxColour(130, 130, 140), wxColour(40, 190, 190), wxColour(90, 160, 210), wxColour(70, 130, 240),
                 wxColour(170, 110, 240), wxColour(235, 175, 60), wxColour(80, 200, 120), wxColour(90, 170, 255),
                 wxColour(240, 140, 70)}
           : Pal{wxColour(150, 150, 160), wxColour(0, 150, 150), wxColour(60, 130, 190), wxColour(40, 100, 220),
                 wxColour(140, 80, 210), wxColour(215, 150, 20), wxColour(30, 160, 80), wxColour(40, 130, 230),
                 wxColour(220, 110, 40)};
}

TimingModel http_model(const HttpResponse& r, bool plain_http) {
  const Timing& t = r.timing;
  const Pal p = palette();
  const bool reused = t.connection_reused;
  TimingModel m;
  m.valid = true;
  m.kind = "http";
  m.reused = reused;
  m.conn_note = reused ? "reused connection" : "new connection";
  if (!r.tls_version.empty()) m.conn_note += " \xC2\xB7 " + r.tls_version;
  m.total = t.total_ms;
  m.server_wait = t.ttfb_ms;
  for (const RedirectHop& h : r.hops) m.server_wait += h.ttfb_ms;  // each hop's own server wait
  if (!r.hops.empty()) {
    m.server_tip = "Wait (TTFB) of the final request plus the wait of every redirect hop: server processing "
                   "plus one network round trip each.";
  }
  m.total_tip = tips::kTotalHttp;
  auto add = [&](const char* name, double v, const char* tip, wxColour c, bool conn_phase) {
    Phase ph{name, v, tip, c};
    ph.reused = conn_phase && reused && v <= 0;
    m.phases.push_back(std::move(ph));
  };
  add("Prepare", r.prepare_ms, tips::kPrepare, p.setup, false);
  add("Queue", t.queue_ms, tips::kQueueHttp, p.setup, false);
  add("DNS", t.dns_ms, tips::kDnsHttp, p.dns, true);
  add("Socket", r.socket_ms, tips::kSocket, p.socket, true);
  add("Connect", t.connect_ms, tips::kConnectHttp, p.connect, true);
  add("TLS", t.tls_ms, tips::kTlsHttp, p.tls, true);
  if (plain_http && t.tls_ms <= 0 && !reused) m.phases.back().has_value = false;
  add("Send", t.send_ms, tips::kSendHttp, p.send, false);
  add("Wait (TTFB)", t.ttfb_ms, tips::kWait, p.wait, false);
  add("Download", t.download_ms, tips::kDownloadHttp, p.down, false);
  if (r.redirect_count > 0 || r.redirect_ms > 0) {
    std::string tip = "Every followed redirect before the final request (" + std::to_string(r.redirect_count) +
                      (r.redirect_count == 1 ? " hop):" : " hops):");
    for (const RedirectHop& h : r.hops)
      tip += "\n  " + std::to_string(h.status) + " " + h.url + "  " + fmt::ms(h.total_ms) +
             (h.connection_reused ? " (reused)" : "");
    // Insert before Prepare's successors would mislead the waterfall order: redirects happen
    // before the final request's phases, so they go first after Prepare.
    Phase ph{"Redirects", r.redirect_ms, tip, p.redirect};
    m.phases.insert(m.phases.begin() + 1, std::move(ph));
  }
  add("Process", r.process_ms, tips::kProcess, p.setup, false);
  m.client = r.prepare_ms + t.queue_ms + r.process_ms;
  m.network = std::max(0.0, t.total_ms - m.server_wait - m.client);
  m.client_tip = "watt's own time: Prepare (request setup) + Queue (UI-to-worker hand-off, libcurl's queue) + "
                 "Process (result collection) = " + fmt::ms(r.prepare_ms) + " + " + fmt::ms(t.queue_ms) + " + " +
                 fmt::ms(r.process_ms) + ".";
  m.network_tip = "Time on the network: DNS, socket, TCP connect, TLS, upload and download";
  m.network_tip += r.hops.empty() ? std::string(".") : std::string(", plus all but the server wait of each redirect hop.");
  m.network_tip += " Neither watt nor the server thinking.";
  return m;
}

TimingModel grpc_model(const GrpcResponse& r) {
  const Timing& t = r.timing;
  const Pal p = palette();
  const bool reused = t.connection_reused;
  TimingModel m;
  m.valid = true;
  m.kind = "grpc";
  m.reused = reused;
  m.conn_note = reused ? "reused channel" : "new channel";
  m.total = t.total_ms;
  m.server_wait = t.ttfb_ms;
  if (r.messages.size() > 1) {
    // A stream's Download phase is paced by the server (first message -> trailers): server time.
    m.server_wait = t.ttfb_ms + t.download_ms;
    m.server_label = "server + stream";
    m.server_tip = "Wait (TTFB) + Download: for a stream the server decides when each message and the "
                   "trailers come, so the whole stream counts as server time. Client overhead is the rest "
                   "(queue, DNS, connect, TLS, reflection, send).";
  }
  m.total_tip = tips::kTotalGrpc;
  auto add = [&](const char* name, double v, const char* tip, wxColour c, bool conn_phase) {
    Phase ph{name, v, tip, c};
    ph.reused = conn_phase && reused && v <= 0;
    m.phases.push_back(std::move(ph));
  };
  add("Queue", t.queue_ms, tips::kQueueGrpc, p.setup, false);
  add("DNS", t.dns_ms, tips::kDnsGrpc, p.dns, true);
  add("Connect", t.connect_ms, tips::kConnectGrpc, p.connect, true);
  add("TLS", t.tls_ms, tips::kTlsGrpc, p.tls, true);
  if (t.tls_ms <= 0 && !reused) m.phases.back().has_value = false;
  if (r.reflect_ms > 0) add("Reflection", r.reflect_ms, tips::kReflect, p.redirect, false);
  add("Send", t.send_ms, tips::kSendGrpc, p.send, false);
  add("Wait (TTFB)", t.ttfb_ms, tips::kWait, p.wait, false);
  add("Download", t.download_ms, tips::kDownloadGrpc, p.down, false);
  double sum = 0;
  for (const Phase& ph : m.phases) sum += ph.ms;
  const double other = t.total_ms - sum > 0.005 ? t.total_ms - sum : 0;
  // Always a row (0 when nothing is left over), so a failed call and an OK one have the same
  // height and the tabs below do not jump when a stream finishes.
  add("Other", other, tips::kOther, p.setup, false);
  if (r.status_code == 14 && r.messages.empty() && t.ttfb_ms <= 0 && t.download_ms <= 0) {
    // The call ended before the server answered (e.g. UNAVAILABLE: nothing to connect to): the
    // phases it never reached are n/a, and the connection phase it stopped in is marked.
    for (Phase& ph : m.phases) {
      const bool unreached = ph.name == "Wait (TTFB)" || ph.name == "Download" || (ph.name == "Send" && ph.ms <= 0);
      if (unreached) {
        ph.has_value = false;
        ph.ms = 0;
      }
    }
    if (!reused)
      for (auto it = m.phases.rbegin(); it != m.phases.rend(); ++it)
        if ((it->name == "Connect" || it->name == "TLS" || it->name == "DNS") && it->ms > 0) {
          it->failed = true;
          break;
        }
    m.conn_note = reused ? "reused channel, no reply" : "no connection";
  }
  m.client = t.queue_ms + other;
  m.network = std::max(0.0, t.total_ms - m.server_wait - m.client);
  m.client_tip = "watt's own time: Queue (UI-to-worker hand-off) + Other (unattributed time inside the call).";
  m.network_tip = r.messages.size() > 1
                      ? "DNS, connect, TLS, reflection and sending the request: time on the network before the "
                        "server's stream started."
                      : "DNS, connect, TLS, reflection, sending the request and receiving the reply: time on the "
                        "network, neither watt nor the server thinking.";
  return m;
}

Tone http_tone(long s) {
  if (s >= 200 && s < 300) return Tone::Ok;
  if (s >= 300 && s < 400) return Tone::Redirect;
  if (s >= 400 && s < 500) return Tone::Warn;
  if (s >= 500) return Tone::Err;
  return Tone::Plain;
}

std::string host_port(const std::string& ip, long port) {
  if (ip.empty()) return {};
  std::string s = ip.find(':') != std::string::npos ? "[" + ip + "]" : ip;
  if (port) s += ":" + std::to_string(port);
  return s;
}

// "2026-01-15 23:59:59 UTC" -> whole days from now (negative: expired); false if unparsable.
bool days_left(const std::string& utc, long* days) {
  wxDateTime d;
  wxString::const_iterator end;
  const wxString s = wxString::FromUTF8(utc.substr(0, 19));
  if (!d.ParseFormat(s, "%Y-%m-%d %H:%M:%S", &end)) return false;
  d.MakeFromUTC();
  *days = static_cast<long>(std::floor((d - wxDateTime::Now()).GetSeconds().ToDouble() / 86400.0));
  return true;
}

// Rows of the connection popover for an HTTP response.
std::vector<InfoRow> http_conn_rows(const HttpResponse& r, const SentHttp* sent) {
  std::vector<InfoRow> rows;
  const std::string& url = r.effective_url.empty() && sent ? sent->request.url : r.effective_url;
  const bool https = url.rfind("https:", 0) == 0;
  rows.push_back({"Connection", {}, true});
  if (!r.ok) rows.push_back({"Error", r.error, false, true});
  if (!r.http_version.empty())
    rows.push_back({"Protocol", r.http_version + (https ? "  (negotiated via ALPN)" : "  (plain TCP)")});
  if (r.ok)
    rows.push_back({"Connection", r.timing.connection_reused ? "reused (opened by an earlier request; DNS, TCP and TLS skipped)"
                                                             : "new (opened for this request)"});
  if (const std::string a = host_port(r.remote_ip, r.remote_port); !a.empty()) rows.push_back({"Remote address", a});
  if (const std::string a = host_port(r.local_ip, r.local_port); !a.empty()) rows.push_back({"Local address", a});
  if (!url.empty()) rows.push_back({"URL", url});
  if (r.redirect_count > 0) rows.push_back({"Redirects", std::to_string(r.redirect_count) + " followed"});
  if (!https) {
    rows.push_back({"TLS", {}, true});
    rows.push_back({"TLS", "none: plain http://, nothing is encrypted"});
    return rows;
  }
  rows.push_back({"TLS", {}, true});
  if (r.tls_version.empty()) {
    rows.push_back({"TLS", r.ok ? "details not available" : "no TLS session was established", false, !r.ok});
    return rows;
  }
  rows.push_back({"Version", r.tls_version});
  if (!r.tls_cipher.empty()) rows.push_back({"Cipher", r.tls_cipher});
  if (!r.verify_result.empty())
    rows.push_back({"Verification", r.verify_result, false, r.verify_result.rfind("verified", 0) != 0});
  rows.push_back({"Certificate", {}, true});
  if (!r.cert_subject.empty()) rows.push_back({"Subject", r.cert_subject});
  if (!r.cert_san.empty()) rows.push_back({"Alt names", r.cert_san});
  if (!r.cert_issuer.empty()) rows.push_back({"Issuer", r.cert_issuer});
  if (!r.cert_not_after.empty()) {
    std::string v = r.cert_not_after;
    long d = 0;
    bool soon = false;
    if (days_left(r.cert_not_after, &d)) {
      v += d < 0 ? "  (expired " + std::to_string(-d) + " days ago)" : "  (" + std::to_string(d) + " days left)";
      soon = d < 14;
    }
    rows.push_back({"Valid until", v, false, soon});
  }
  if (!r.cert_chain.empty()) rows.push_back({"Chain", r.cert_chain});
  return rows;
}

// What the response view knows about a gRPC call's target (from the request as sent).
struct GrpcConnMeta {
  std::string address, authority, server_name;
  bool tls = false, mtls = false, web = false;
};

std::vector<InfoRow> grpc_conn_rows(const GrpcConnMeta& m, const GrpcResponse& r) {
  std::vector<InfoRow> rows;
  rows.push_back({"Channel", {}, true});
  rows.push_back({"Target", m.address.empty() ? std::string("\xE2\x80\x94") : m.address});
  if (!r.peer.empty()) {
    std::string p = r.peer;
    for (const char* pre : {"ipv4:", "ipv6:", "dns:///"})
      for (size_t at; (at = p.find(pre)) != std::string::npos;) p.erase(at, std::strlen(pre));
    std::string list;
    for (char c : p) list += c == ',' ? std::string(", ") : std::string(1, c);
    rows.push_back({"Resolved", list + "   (" + r.peer + ")"});
  } else if (m.web) {
    rows.push_back({"Resolved", "gRPC-Web goes over HTTP: see the HTTP engine"});
  }
  rows.push_back({"Protocol", m.web ? "gRPC-Web over HTTP/1.1 or HTTP/2" : (m.tls ? "gRPC over HTTP/2 (h2, TLS)" : "gRPC over HTTP/2 (h2c, plaintext)")});
  const bool no_conn = r.status_code == 14 && r.messages.empty();
  rows.push_back({"Channel", no_conn ? "not connected" : r.timing.connection_reused
                                                            ? "reused (already READY: no DNS, TCP or TLS)"
                                                            : "new (connected for this call)"});
  if (!m.authority.empty()) rows.push_back({":authority", m.authority});
  rows.push_back({"Security", {}, true});
  if (!m.tls) {
    rows.push_back({"TLS", "none: plaintext, nothing is encrypted"});
  } else {
    rows.push_back({"TLS", m.mtls ? "yes, mutual TLS (client certificate sent)" : "yes (server certificate verified by gRPC)"});
    if (!m.server_name.empty()) rows.push_back({"Server name", m.server_name});
    rows.push_back({"Details", "the gRPC engine does not report the TLS version, cipher or certificate", false, false});
  }
  return rows;
}

// Where a failed HTTP request stopped, from libcurl's error text first (a rejected URL or an
// unsupported scheme never resolves anything, whatever the timer says) and the timings second.
enum class FailAt { Url, Dns, Connect, Tls, Wait, Download, Other };
struct Failure {
  FailAt at;
  const char* text;
};
Failure classify_failure(const HttpResponse& r) {
  std::string e = r.error;
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  auto has = [&](const char* k) { return e.find(k) != std::string::npos; };
  const Timing& t = r.timing;
  if (has("resolv")) return {FailAt::Dns, "Failed during name resolution (DNS)."};
  if ((has("protocol") && (has("not supported") || has("unsupported"))) || has("url rejected") ||
      has("malformed") || has("bad/illegal") || has("no url") || has("port number"))
    return {FailAt::Url, "The URL was rejected before anything was resolved, connected or sent."};
  if (has("ssl") || has("tls") || has("schannel") || has("certificate"))
    return {FailAt::Tls, "Failed during the TLS handshake."};
  if (has("connection timed out") || has("connect")) return {FailAt::Connect, "Failed while connecting (TCP)."};
  if (t.download_ms > 0) return {FailAt::Download, "Failed while receiving the response."};
  if (t.ttfb_ms > 0 || t.send_ms > 0) return {FailAt::Wait, "Failed while sending or waiting for the server."};
  if (t.tls_ms > 0) return {FailAt::Tls, "Failed during the TLS handshake."};
  if (t.connect_ms > 0) return {FailAt::Connect, "Failed while connecting (TCP)."};
  if (t.dns_ms > 0) return {FailAt::Dns, "Failed during name resolution or connect."};
  return {FailAt::Other, "Failed before the request could be sent."};
}

// A failed request's waterfall: the phase it failed in is marked, the ones it never reached are
// n/a (no bar, no value) instead of whatever the timers held.
void mark_failure(TimingModel& m, const HttpResponse& r) {
  const Failure f = classify_failure(r);
  const char* failed = nullptr;
  switch (f.at) {
    case FailAt::Url: failed = ""; break;  // nothing on the network ran
    case FailAt::Dns: failed = "DNS"; break;
    case FailAt::Connect: failed = "Connect"; break;
    case FailAt::Tls: failed = "TLS"; break;
    case FailAt::Wait: failed = "Wait (TTFB)"; break;
    case FailAt::Download: failed = "Download"; break;
    case FailAt::Other: return;
  }
  static const char* const kNet[] = {"DNS", "Socket", "Connect", "TLS", "Send", "Wait (TTFB)", "Download"};
  bool after = *failed == 0;
  for (const char* name : kNet) {
    for (Phase& ph : m.phases) {
      if (ph.name != name) continue;
      if (after) {
        ph.has_value = false;
        ph.reused = false;
        ph.ms = 0;
      } else if (ph.name == failed) {
        ph.failed = true;
        ph.has_value = true;
        ph.reused = false;
      }
    }
    if (std::string_view(name) == failed) after = true;
  }
  m.conn_note = f.at == FailAt::Url ? "not sent" : "failed";
  m.reused = false;
  // The footer split follows: time in phases that never ran is watt's (client), not network.
  double net = 0;
  for (const Phase& ph : m.phases)
    for (const char* name : kNet)
      if (ph.has_value && ph.name == name && ph.name != "Wait (TTFB)") net += ph.ms;
  m.network = net;
  m.client = std::max(0.0, m.total - m.server_wait - net);
  m.client_tip = "watt's own time, including the time before the request failed that no phase above covers.";
}

// Cheap identity of a request body: its size plus a hash of the first and last 64 KB.
std::string body_tag(std::string_view b) {
  if (b.empty()) return {};
  const size_t k = 64 * 1024;
  size_t h = std::hash<std::string_view>()(b.substr(0, k));
  if (b.size() > k) h ^= std::hash<std::string_view>()(b.substr(b.size() - std::min(k, b.size() - k))) * 31;
  return " #" + std::to_string(b.size()) + ":" + std::to_string(h);
}

// "GET https://example.com/a/very/long/path" -> at most n characters, elided in the middle.
std::string elide_middle(const std::string& s, size_t n);

// Short name of the request a response belongs to, for the status strip: "GET /json/20",
// "Echo/Count" (full label in the tooltip).
std::string request_tag(const std::string& label) {
  std::string t = label;
  if (const size_t at = t.find(" @ "); at != std::string::npos) {  // gRPC "pkg.Svc/Method @ host"
    t.resize(at);
    const size_t slash = t.rfind('/');
    const size_t dot = t.rfind('.', slash == std::string::npos ? std::string::npos : slash);
    if (dot != std::string::npos) t = t.substr(dot + 1);
    return elide_middle(t, 40);
  }
  // HTTP "METHOD scheme://host/path?q": method + host + path (no scheme).
  const size_t sp = t.find(' ');
  const size_t sch = t.find("://");
  if (sp != std::string::npos && sch != std::string::npos) {
    const size_t path = t.find('/', sch + 3);
    t = t.substr(0, sp) + " " + t.substr(sch + 3) + (path == std::string::npos ? "/" : "");
  }
  return elide_middle(t, 40);
}

std::string elide_middle(const std::string& s, size_t n) {
  if (s.size() <= n) return s;
  return s.substr(0, n / 2) + "\xE2\x80\xA6" + s.substr(s.size() - (n - n / 2 - 1));
}

// ------------------------------------------------------------------ log (curl -v style)

// Path + query of a URL for the request line ("/a?b"; "/" when none).
std::string url_target(std::string_view u) {
  const size_t sch = u.find("://");
  if (sch != std::string_view::npos) u.remove_prefix(sch + 3);
  const size_t slash = u.find_first_of("/?");
  std::string t = slash == std::string_view::npos ? std::string("/") : std::string(u.substr(slash));
  if (const size_t h = t.find('#'); h != std::string::npos) t.resize(h);
  if (!t.empty() && t[0] == '?') t.insert(t.begin(), '/');
  return t;
}

// The whole exchange as one text, like curl -v: connection and TLS ("*"), the request as sent
// (">"), redirect hops, the response status and headers ("<") and the timing. Built from what the
// response already holds; for a bug report.
std::string http_transcript(const HttpResponse& r, const SentHttp* sent, const std::vector<std::pair<std::string, std::string>>& req_rows,
                            const std::string& when) {
  std::string o;
  auto line = [&](char p, const std::string& t) {
    o += p;
    o += ' ';
    for (char c : t) {
      o += c;
      if (c == '\n') o += std::string(1, p) + "   ";
    }
    o += '\n';
  };
  const std::string url = sent ? sent->request.url : r.effective_url;
  line('*', when + "  " + (sent ? sent->request.method + " " : std::string()) + url);
  for (const InfoRow& row : http_conn_rows(r, sent)) {
    if (row.section) continue;
    if (row.key == "URL") continue;
    line('*', row.key + ": " + row.value);
  }
  for (size_t i = 0; i < r.hops.size(); ++i) {
    const RedirectHop& h = r.hops[i];
    line('*', "");
    line('>', (sent ? sent->request.method : std::string("GET")) + " " + url_target(h.url) + "  (" + h.url + ")");
    line('<', std::to_string(h.status) + " redirect  \xC2\xB7 " + fmt::ms(h.total_ms) + " (wait " + fmt::ms(h.ttfb_ms) +
                  ")" + (h.connection_reused ? "  \xC2\xB7 reused connection" : "") +
                  (h.remote_ip.empty() ? std::string() : "  \xC2\xB7 " + host_port(h.remote_ip, h.remote_port)));
    line('*', "Following the redirect");
  }
  line('*', "");
  if (sent) {
    const std::string final_url = r.effective_url.empty() ? url : r.effective_url;
    line('>', sent->request.method + " " + url_target(final_url) + " " + (r.http_version.empty() ? "HTTP/1.1" : r.http_version));
    // Request rows: [0] is the request line, then headers, then "(body)" and redirect rows.
    for (size_t i = 1; i < req_rows.size(); ++i) {
      const auto& [k, v] = req_rows[i];
      if (k.rfind("(body)", 0) == 0) {
        line('>', "");
        if (v != "none") line('>', "[" + k.substr(7) + " body] " + v);
        break;
      }
      line('>', k + ": " + v);
    }
  }
  if (!r.ok) {
    line('*', "Failed: " + r.error);
  } else {
    line('<', (r.http_version.empty() ? std::string("HTTP/1.1") : r.http_version) + " " + std::to_string(r.status) +
                  (r.status_text.empty() ? std::string() : " " + r.status_text));
    for (const auto& [k, v] : r.headers) line('<', std::string(k) + ": " + std::string(v));
    line('<', "");
    const HttpSizes& z = r.sizes;
    std::string b = "Body " + fmt::human_bytes(z.response_body);
    if (z.response_body_wire != z.response_body) b += " (" + fmt::human_bytes(z.response_body_wire) + " on the wire)";
    line('*', b + ", headers " + fmt::human_bytes(z.response_headers));
  }
  const Timing& t = r.timing;
  std::string tm = "Timing: queue " + fmt::ms(t.queue_ms) + ", DNS " + fmt::ms(t.dns_ms) + ", connect " +
                   fmt::ms(t.connect_ms) + ", TLS " + fmt::ms(t.tls_ms) + ", send " + fmt::ms(t.send_ms) + ", wait " +
                   fmt::ms(t.ttfb_ms) + ", download " + fmt::ms(t.download_ms);
  if (r.redirect_ms > 0) tm += ", redirects " + fmt::ms(r.redirect_ms);
  line('*', tm + ", total " + fmt::ms(t.total_ms));
  line('*', "Bytes on the wire: " + std::to_string(t.bytes_sent) + " sent, " + std::to_string(t.bytes_received) + " received");
  return o;
}

// ------------------------------------------------------------------ view

enum Tab { kBody, kHeaders, kCookies, kTrailers, kMessages, kRequest, kTests, kLog };

class ResponseView : public wxPanel, public ResponsePanel {
 public:
  explicit ResponseView(wxWindow* parent) : wxPanel(parent, wxID_ANY) {
    SetBackgroundColour(Theme::get().bg);
    status_ = new StatusStrip(this);
    wf_ = new Waterfall(this);
    tabs_ = new TabStrip(this, [this](int i) {
      book_->SetSelection(static_cast<size_t>(i));
      if (i == kTests) refresh_asserts(true);
    });
    book_ = new wxSimplebook(this, wxID_ANY);
    book_->SetBackgroundColour(Theme::get().bg);
    body_ = new BodyView(book_);
    headers_ = new KvList(book_, "Header", "Value");
    cookies_ = new KvList(book_, "Cookie", "Value; attributes");
    trailers_ = new KvList(book_, "Trailer", "Value");
    msgs_ = new MsgList(book_, [this](size_t i) {
      select_tab(kBody);
      body_->goto_message(i);
    });
    request_ = new KvList(book_, "Request", "Value");
    tests_ = new TestsView(book_);
    tests_->on_edit = [this](const std::vector<Assertion>& a) { save_asserts(a); };
    tests_->on_reveal = [this](const Assertion& a) { reveal(a); };
    book_->AddPage(body_, "Body");
    book_->AddPage(headers_, "Headers");
    book_->AddPage(cookies_, "Cookies");
    book_->AddPage(trailers_, "Trailers");
    book_->AddPage(msgs_, "Messages");
    book_->AddPage(request_, "Request");
    book_->AddPage(tests_, "Tests");
    log_ = new wxTextCtrl(book_, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                          wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP | wxTE_RICH2 | wxBORDER_NONE);
    log_->SetFont(wxFont(wxFontInfo(9).Family(wxFONTFAMILY_TELETYPE).FaceName("Consolas")));
    log_->SetBackgroundColour(Theme::get().bg);
    log_->SetForegroundColour(Theme::get().fg);
    book_->AddPage(log_, "Log");
    tabs_->add("Body");
    tabs_->add("Headers");
    tabs_->add("Cookies");
    tabs_->add("Trailers");
    tabs_->add("Messages");
    tabs_->add("Request");
    tabs_->add("Tests");
    tabs_->add("Log");

    hist_ = new ChipBar(this, [this](int) { history_menu(); });
    hist_->add(1, "History", "Earlier responses of this tab, each labelled with its request: pick one to look at it "
                             "again (its delta compares with the run of the same request before it)", -1, true);
    hist_->set_visible(1, false);
    hist_->set_bottom_rule(true);  // the tab strip's rule runs to the pane's right edge
    hist_->SetMinSize(wxSize(FromDIP(130), -1));
    auto* tabrow = new wxBoxSizer(wxHORIZONTAL);
    tabrow->Add(tabs_, 1, wxEXPAND);
    tabrow->Add(hist_, 0, wxEXPAND);

    auto* sz = new wxBoxSizer(wxVERTICAL);
    sz->Add(status_, 0, wxEXPAND);
    sz->Add(wf_, 0, wxEXPAND | wxTOP, FromDIP(4));
    sz->Add(tabrow, 0, wxEXPAND | wxTOP, FromDIP(2));
    sz->Add(book_, 1, wxEXPAND);
    SetSizer(sz);
    wf_->set_on_conn_click([this](const wxRect& r) { open_conn(r); });
    status_->set_on_click([this](int, const wxRect& r) { open_conn(r); });

    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
      if (e.GetKeyCode() == 'F' && e.GetModifiers() == wxMOD_CONTROL) {
        select_tab(kBody);
        body_->show_find();
        return;
      }
      e.Skip();
    });
    // Esc anywhere in the tab (the URL field included) cancels the request in flight. Hooked on
    // the tab once it exists (after the constructor; the tab builds us first).
    CallAfter([this] {
      if (RequestTab* t = tab()) {
        t->Bind(wxEVT_CHAR_HOOK, [this, t](wxKeyEvent& e) {
          if (e.GetKeyCode() == WXK_ESCAPE && e.GetModifiers() == 0 && t->in_flight()) {
            t->cancel();
            return;
          }
          e.Skip();
        });
      }
      refresh_asserts(false);
    });
  }

  wxWindow* window() override { return this; }

  void clear() override {
    status_->set("", Tone::Plain, {{"Send a request to see the response here  (Ctrl+Enter)", "", Tone::Dim}});
    wf_->clear(false);
    shown_ident_.clear();
    body_->show_message("");
    headers_->set({});
    trailers_->set({});
    msgs_->reset();
    request_->set({});
    cookies_->set({});
    log_->Clear();
    set_kind_tabs(false, false);
    select_tab(kBody);
    runs_.clear();
    shown_run_ = 0;
    update_history_chip();
    ++test_seq_;
    tests_->set_no_response();
    // The tab may have loaded another request: read its checks once this turn is over (the tab
    // may still be building its editor).
    CallAfter([this] { refresh_asserts(false); });
  }

  void show_sending(RequestKind kind) override {
    kind_ = kind;
    live_ = false;
    status_->set_busy(kind == RequestKind::Grpc ? "Calling\xE2\x80\xA6" : "Sending\xE2\x80\xA6", Clock::now());
    // gRPC results carry no request: remember what this call is (target, method, body) for its
    // delta. HTTP results come with the request as sent.
    grpc_key_.clear();
    grpc_label_.clear();
    std::string ident;  // which request this is (as edited, before {{var}} expansion)
    for (wxWindow* w = GetParent(); w; w = w->GetParent())
      if (auto* tab = dynamic_cast<RequestTab*>(w)) {
        const SavedRequest& q = tab->request();
        if (kind == RequestKind::Grpc) {
          grpc_key_ = "grpc " + std::string(q.grpc_target.tls ? "tls " : "") + q.grpc_target.address + " " +
                      q.grpc_method + body_tag(q.grpc_body);
          grpc_label_ = q.grpc_method + " @ " + q.grpc_target.address;
          const GrpcTarget& gt = q.grpc_target;
          grpc_meta_ = GrpcConnMeta{gt.address, gt.authority, gt.server_name, gt.tls || gt.address.rfind("https:", 0) == 0,
                                    !gt.client_cert_pem.empty(), gt.web};
          ident ="grpc " + q.grpc_target.address + " " + q.grpc_method;
        } else {
          ident = "http " + q.http.method + " " + q.http.url;
        }
        set_asserts_from(q);
        break;
      }
    // The previous figures stay (dimmed, "previous run") only while the same request runs again:
    // Fail's timing must not sit above a Count stream.
    if (ident.empty() || ident != shown_ident_) wf_->clear();
    shown_ident_ = ident;
    wf_->reserve(kind == RequestKind::Grpc ? 8 : 10);
    wf_->set_stale(true);
    wf_->set_waiting(Clock::now());
    body_->set_stale(true);
    if (!tests_->asserts().empty()) {
      tests_->set_running();
      tabs_->set_label(kTests, "Tests", "\xE2\x80\xA6");
      tabs_->set_count_tone(kTests, Tone::Plain);
    }
  }

  void show_progress(uint64_t bytes) override {
    status_->set_busy_label("Receiving " + fmt::human_bytes(bytes) + "\xE2\x80\xA6");
  }

  void show_http(std::shared_ptr<const HttpResponse> r, std::shared_ptr<const SentHttp> sent) override {
    const bool plain = sent ? sent->request.url.rfind("http:", 0) == 0 : r->effective_url.rfind("http:", 0) == 0;
    Run run{r, sent, http_model(*r, plain), wxDateTime::Now()};
    if (!r->ok) mark_failure(run.model, *r);
    if (sent) {
      run.label = sent->request.method + " " + sent->request.url;
      run.key = "http " + run.label + body_tag(sent->request.body);
    }
    // The delta compares with the previous successful run of the same request only.
    if (const Base* b = base_for(run.key)) {
      run.base = b->model;
      run.base_at = b->at;
      run.has_base = true;
    }
    if (r->ok) remember(run.key, run.model);
    // Keep the last runs of this tab (at most kRuns, and at most ~64 MB of bodies) so an earlier
    // response can be looked at again and compared with the current one.
    runs_.push_back(std::move(run));
    size_t bytes = 0;
    for (const Run& x : runs_) bytes += x.resp->body.size();
    while (runs_.size() > kRuns || (runs_.size() > 1 && bytes > (64u << 20))) {
      bytes -= runs_.front().resp->body.size();
      runs_.erase(runs_.begin());
    }
    show_run(runs_.size() - 1);
  }

  // Renders run i of this tab's HTTP history (the newest one after a send). Deltas compare with
  // the run of the same request before it.
  void show_run(size_t i) {
    body_->set_stale(false);
    showing_grpc_ = false;
    shown_run_ = i;
    update_history_chip();
    const Run& run = runs_[i];
    std::shared_ptr<const HttpResponse> r = run.resp;
    const std::shared_ptr<const SentHttp>& sent = run.sent;
    const Timing& t = r->timing;
    const TimingModel& model = run.model;
    const TimingModel* before = (r->ok && run.has_base) ? &run.base : nullptr;
    std::vector<StatusSeg> segs;
    if (!r->ok) {
      const std::string where = classify_failure(*r).text;
      segs.push_back({fmt::ms(t.total_ms), "Time until the failure.", Tone::Plain, true});
      segs.push_back({r->error, where + "\n" + r->error, Tone::Err, false});
      status_->set("Failed", Tone::Err, std::move(segs));
      wf_->set(model, nullptr);
      body_->show_message("Request failed: " + r->error + "\n\n" + where + "\n");
      headers_->set({});
      tabs_->set_label(kHeaders, "Headers");
      set_kind_tabs(false, true);
      set_cookies(*r);
      set_request(sent.get(), r.get());
      set_log(run);
      run_tests();
      return;
    }
    std::string badge = std::to_string(r->status);
    if (!r->status_text.empty()) badge += " " + r->status_text;
    else if (r->status == 200) badge += " OK";
    segs.push_back({fmt::ms(t.total_ms), "Total: Send click -> complete response (the sum of all phases).",
                    Tone::Plain, true});
    segs.push_back({"server " + fmt::ms(model.server_wait), model.server_tip, Tone::Dim, false, 5});
    segs.push_back({"client " + fmt::ms(model.client),
                    model.client_tip + "\nNetwork (DNS, connect, TLS, transfer): " + fmt::ms(model.network) + ".",
                    Tone::Dim, false, 6});
    const HttpSizes& s = r->sizes;
    std::string size_tip = "Body " + fmt::human_bytes(s.response_body);
    if (s.response_body_wire != s.response_body)
      size_tip += " (" + fmt::human_bytes(s.response_body_wire) + " on the wire, " +
                  std::string(r->headers.get("content-encoding")) + ")";
    size_tip += ", headers " + fmt::human_bytes(s.response_headers) + ".\nSent: headers " +
                fmt::human_bytes(s.request_headers) + ", body " + fmt::human_bytes(s.request_body) +
                ".\nOn the wire in total: " + fmt::human_bytes(t.bytes_received) + " received, " +
                fmt::human_bytes(t.bytes_sent) + " sent.";
    std::string size = fmt::human_bytes(s.response_body);
    if (s.response_body_wire != s.response_body && s.response_body_wire)
      size += " (" + fmt::human_bytes(s.response_body_wire) + " wire)";
    segs.push_back({size, size_tip, Tone::Plain, false, 2});
    const bool earlier = i + 1 < runs_.size();
    if (earlier) {
      // An earlier run may be of another request than the tab shows now: say which, first.
      const std::string tag = run.label.empty() ? std::string("earlier run") : "earlier run: " + request_tag(run.label);
      segs.insert(segs.begin() + 1, StatusSeg{tag, "You are looking at an earlier response of this tab (History):\n" +
                                                       run.label, Tone::Warn, true, 0});
    }
    const std::string remote = host_port(r->remote_ip, r->remote_port);
    {
      std::string proto = r->http_version.empty() ? std::string("connection") : r->http_version;
      if (!r->tls_version.empty()) proto += " \xC2\xB7 " + r->tls_version;
      std::string tip = "Protocol of the final response";
      tip += r->tls_version.empty() ? "." : " (negotiated via ALPN), " + r->tls_version + " " + r->tls_cipher + ".";
      if (!remote.empty()) tip += "\nRemote " + remote;
      if (!r->local_ip.empty()) tip += ", local " + host_port(r->local_ip, r->local_port);
      tip += "\nClick for the connection and certificate details.";
      StatusSeg seg{proto + " \xE2\x96\xBE", tip, Tone::Info, false, 3};
      seg.click = 1;
      segs.push_back(std::move(seg));
    }
    if (!remote.empty())
      segs.push_back({remote, "Remote address that served the final response.\n" + r->effective_url, Tone::Dim,
                      false, 8});
    if (!run.label.empty() && !earlier)
      segs.push_back({request_tag(run.label), "This response belongs to:\n" + run.label, Tone::Dim, false, 7});
    if (r->redirect_count > 0)
      segs.push_back({std::to_string(r->redirect_count) + (r->redirect_count == 1 ? " redirect" : " redirects"),
                      "Followed redirects; final URL:\n" + r->effective_url, Tone::Redirect, false, 3});
    if (before) add_delta(segs, model, *before, run.label, run.base_at);
    status_->set(badge, http_tone(r->status), std::move(segs));
    wf_->set(model, before);

    KV h;
    h.reserve(r->headers.size());
    for (const auto& [k, v] : r->headers) h.emplace_back(std::string(k), std::string(v));
    headers_->set(std::move(h));
    tabs_->set_label(kHeaders, "Headers", std::to_string(r->headers.size()));
    set_kind_tabs(false, true);
    set_cookies(*r);
    set_request(sent.get(), r.get());
    set_log(run);
    body_->set_http(std::move(r));
    run_tests();
  }

  void append_grpc_messages(const std::vector<GrpcMessageEvent>& batch) override {
    if (batch.empty()) return;
    if (!live_) start_stream();
    body_->append_messages(batch.data(), batch.size(), msgs_->count());
    msgs_->append(batch.data(), batch.size());
    tabs_->set_label(kMessages, "Messages", std::to_string(msgs_->count()));
    status_->set_busy_label("Streaming \xC2\xB7 " + std::to_string(msgs_->count()) + " messages");
  }

  void show_grpc(std::shared_ptr<const GrpcResponse> r) override {
    const size_t shown = live_ ? msgs_->count() : 0;
    if (!live_) start_stream();
    if (r->messages.size() > shown) {
      const size_t n = r->messages.size() - shown;
      body_->append_messages(r->messages.data() + shown, n, shown);
      msgs_->append(r->messages.data() + shown, n);
    }
    live_ = false;
    showing_grpc_ = true;
    grpc_last_ = r;
    grpc_shown_meta_ = grpc_meta_;
    const Timing& t = r->timing;
    TimingModel model = grpc_model(*r);
    if (r->status_code != 14 || !r->messages.empty())
      model.conn_note += grpc_meta_.tls ? (grpc_meta_.mtls ? " \xC2\xB7 mTLS" : " \xC2\xB7 TLS") : " \xC2\xB7 plaintext";
    std::vector<StatusSeg> segs;
    segs.push_back({fmt::ms(t.total_ms), "Total: Send click -> final status (the sum of all phases).", Tone::Plain,
                    true});
    segs.push_back({"client " + fmt::ms(model.client),
                    model.client_tip + "\nServer: " + fmt::ms(model.server_wait) + ", network: " +
                        fmt::ms(model.network) + ".",
                    Tone::Dim, false, 4});
    uint64_t wire = 0;
    for (const auto& m : r->messages) wire += m.wire_bytes;
    const size_t n = r->messages.size();
    segs.push_back({std::to_string(n) + (n == 1 ? " message" : " messages"),
                    "Messages received; see the Messages tab for per-message offsets and sizes.", Tone::Plain, false,
                    3});
    segs.push_back({fmt::human_bytes(wire),
                    "Message payload bytes received (protobuf wire size). Total on the wire: " +
                        fmt::human_bytes(t.bytes_received) + " received, " + fmt::human_bytes(t.bytes_sent) + " sent.",
                    Tone::Plain, false, 6});
    if (!r->ok && !r->status_message.empty())
      segs.push_back({r->status_message, r->error_details.empty() ? r->status_message : r->error_details, Tone::Err,
                      false});
    {
      std::string text = grpc_meta_.web ? "gRPC-Web" : grpc_meta_.tls ? (grpc_meta_.mtls ? "h2 \xC2\xB7 mTLS" : "h2 \xC2\xB7 TLS")
                                                                       : "h2c";
      StatusSeg seg{text + " \xE2\x96\xBE",
                    "Target " + grpc_meta_.address + (r->peer.empty() ? std::string() : "\nResolved " + r->peer) +
                        "\nClick for the channel details.",
                    Tone::Info, false, 3};
      seg.click = 1;
      segs.push_back(std::move(seg));
    }
    // Same target, method, body and outcome only: a failing call is never compared with a
    // successful one, nor Count with Say.
    const std::string key =
        grpc_key_.empty() ? std::string() : grpc_key_ + " status " + std::to_string(r->status_code);
    const Base* base = base_for(key);
    if (!grpc_label_.empty())
      segs.push_back({request_tag(grpc_label_), "This response belongs to:\n" + grpc_label_, Tone::Dim, false, 7});
    if (base) add_delta(segs, model, base->model, grpc_label_, base->at);
    Tone tone = Tone::Ok;
    if (!r->ok) tone = (r->status_code == 1 || r->status_code == 4) ? Tone::Warn : Tone::Err;
    status_->set(r->status_name.empty() ? std::string("ERROR") : r->status_name, tone, std::move(segs));
    wf_->set(model, base ? &base->model : nullptr);
    // Not a transport failure (UNAVAILABLE, DEADLINE_EXCEEDED, CANCELLED): remember for the next delta.
    if (r->status_code != 14 && r->status_code != 4 && r->status_code != 1) remember(key, model);
    if (!r->ok) {
      std::string s = "status " + r->status_name + " (" + std::to_string(r->status_code) + ")";
      if (!r->status_message.empty()) s += ": " + r->status_message;
      if (n == 0) {
        body_->show_grpc_error("gRPC " + s, r->error_details);
      } else {
        body_->append_note(s);
      }
    } else if (n == 0) {
      body_->show_message("(no messages)");
    }
    body_->finish_grpc(r);
    headers_->set(r->headers);
    KV tr = r->trailers;
    for (auto& [k, v] : tr)
      if (k == "grpc-status-details-bin" && !r->error_details.empty()) {
        std::string one;
        v = "decoded: " + (fmt::compact_json(r->error_details, one, 4096) ? one : r->error_details);
      }
    trailers_->set(std::move(tr));
    tabs_->set_label(kHeaders, "Headers", std::to_string(r->headers.size()));
    tabs_->set_label(kTrailers, "Trailers", std::to_string(r->trailers.size()));
    tabs_->set_label(kMessages, "Messages", std::to_string(msgs_->count()));
    set_grpc_log(*r);
    run_tests();
  }

  void show_cancelled(double after_ms) override {
    live_ = false;
    ++test_seq_;
    tests_->set_no_response();
    update_tests_label();
    body_->set_stale(false);
    wf_->set_stale(false);
    if (!wf_->has_figures()) wf_->clear(false);  // nothing to show: no empty reserved block
    status_->set("Cancelled", Tone::Warn, {{"after " + fmt::ms(after_ms), "Cancelled by you (Esc or the Cancel button).", Tone::Plain, true}});
  }

  void show_error(const std::string& message) override {
    live_ = false;
    ++test_seq_;
    showing_grpc_ = false;
    tests_->set_no_response();
    update_tests_label();
    body_->set_stale(false);
    status_->set("Error", Tone::Err, {{message, message, Tone::Err, false}});
    wf_->clear(false);
    body_->show_message(message);
    headers_->set({});
    trailers_->set({});
    msgs_->reset();
    cookies_->set({});
    request_->set({});
    fill_log("* " + message + "\n");
    set_kind_tabs(false, false);
    tabs_->set_label(kRequest, "Request");
  }

 private:
  // ------------------------------------------------------------------ tests

  RequestTab* tab() const {
    for (wxWindow* w = GetParent(); w; w = w->GetParent())
      if (auto* t = dynamic_cast<RequestTab*>(w)) return t;
    return nullptr;
  }

  // Checks of the request q (as the tab holds it). Editable here unless the request editor owns
  // them (see save_asserts).
  void set_asserts_from(const SavedRequest& q) {
    tests_->set_asserts(q.asserts, !asserts_owned_elsewhere_, q.kind == RequestKind::Grpc);
    update_tests_label();
  }
  void refresh_asserts(bool rerun_if_changed) {
    RequestTab* t = tab();
    if (!t) return;
    const std::vector<Assertion> before = tests_->asserts();
    set_asserts_from(t->request());
    if (rerun_if_changed && before != tests_->asserts()) run_tests();
  }

  // The user edited the checks in the Tests tab: they go into the tab's request (saved with it on
  // Ctrl+S) and run again on the response shown.
  void save_asserts(const std::vector<Assertion>& a) {
    if (RequestTab* t = tab()) {
      // request() is the tab's own (mutable) copy, refreshed from the editor first.
      auto& q = const_cast<SavedRequest&>(t->request());
      q.asserts = a;
      t->changed();
      // An editor that stores the checks itself (an Asserts table) would overwrite them on its
      // next store(): then editing belongs there and this tab only shows them.
      if (t->request().asserts != a) {
        asserts_owned_elsewhere_ = true;
        set_asserts_from(t->request());
      }
    }
    run_tests();
  }

  void update_tests_label() {
    const auto& a = tests_->asserts();
    int on = 0;
    for (const Assertion& x : a) on += x.enabled && !x.target.empty();
    if (on == 0) {
      tabs_->set_label(kTests, "Tests", a.empty() ? std::string() : "off");
      tabs_->set_count_tone(kTests, Tone::Plain);
      return;
    }
    const int pass = tests_->passed(), fail = tests_->failed();
    if (pass + fail == 0) {
      tabs_->set_label(kTests, "Tests", std::to_string(on));
      tabs_->set_count_tone(kTests, Tone::Plain);
      return;
    }
    tabs_->set_label(kTests, "Tests", std::to_string(pass) + "/" + std::to_string(pass + fail));
    tabs_->set_count_tone(kTests, fail ? Tone::Err : Tone::Ok);
  }

  // Evaluates the checks against the response shown, on a worker thread (the body is parsed
  // there, once). {{vars}} in expected values are expanded here first, on the UI thread, so the
  // worker never touches the workspace.
  void run_tests() {
    const uint64_t seq = ++test_seq_;
    const std::vector<Assertion>& list = tests_->asserts();
    // Hints for the quick-add chips.
    if (showing_grpc_ && grpc_last_) {
      tests_->set_hints(grpc_last_->status_code == 0 ? 200 : 0, grpc_last_->timing.total_ms, {},
                        !grpc_last_->messages.empty());
    } else if (!runs_.empty() && shown_run_ < runs_.size()) {
      const HttpResponse& r = *runs_[shown_run_].resp;
      const std::string ct(r.headers.get("content-type"));
      tests_->set_hints(r.ok ? r.status : 0, r.timing.total_ms, ct,
                        ct.find("json") != std::string::npos);
    }
    bool any = false;
    for (const Assertion& x : list) any = any || (x.enabled && !x.target.empty());
    if (!any) {
      std::vector<TestRow> rows(list.size());
      for (size_t i = 0; i < list.size(); ++i) rows[i].state = list[i].enabled ? TestRow::NotRun : TestRow::Off;
      tests_->set_results(std::move(rows), 0, 0);
      update_tests_label();
      return;
    }
    auto req = std::make_shared<SavedRequest>();
    req->kind = showing_grpc_ ? RequestKind::Grpc : RequestKind::Http;
    req->asserts = list;
    if (RequestTab* t = tab())
      if (const Workspace* ws = t->workspace())
        for (Assertion& x : req->asserts)
          if (x.value.find("{{") != std::string::npos) x.value = ws->expand(x.value);
    std::shared_ptr<const HttpResponse> http;
    std::shared_ptr<const GrpcResponse> grpc;
    if (showing_grpc_ && grpc_last_) grpc = grpc_last_;
    else if (!runs_.empty() && shown_run_ < runs_.size()) http = runs_[shown_run_].resp;
    if (!http && !grpc) return;
    tests_->set_running();
    tabs_->set_label(kTests, "Tests", "\xE2\x80\xA6");
    tabs_->set_count_tone(kTests, Tone::Plain);
    std::thread([req, http, grpc, seq, alive = alive_, this] {
      const auto t0 = Clock::now();
      std::vector<AssertResult> res;
      double parse_us = 0;
      if (http) {
        ResponseData d(*http);
        res = evaluate_asserts(*req, d, nullptr);
        parse_us = d.parse_us();
      } else {
        ResponseData d(*grpc);
        res = evaluate_asserts(*req, d, nullptr);
        parse_us = d.parse_us();
      }
      const double ms = ms_between(t0, Clock::now());
      // Results are for the enabled checks with a target, in order: map them back to rows.
      std::vector<TestRow> rows(req->asserts.size());
      for (size_t i = 0; i < rows.size(); ++i)
        rows[i].state = req->asserts[i].enabled ? TestRow::NotRun : TestRow::Off;
      for (const AssertResult& r : res) {
        if (!r.a) continue;
        const size_t i = static_cast<size_t>(r.a - req->asserts.data());
        if (i >= rows.size()) continue;
        TestRow& row = rows[i];
        row.state = r.pass ? TestRow::Pass : TestRow::Fail;
        row.actual = r.actual;
        row.expected = r.expected;
        row.message = r.message;
        row.eval_us = r.eval_us;
      }
      if (!wxTheApp) return;
      wxTheApp->CallAfter([alive, this, seq, rows = std::move(rows), ms, parse_us]() mutable {
        if (!*alive || seq != test_seq_) return;
        tests_->set_results(std::move(rows), ms, parse_us / 1000.0);
        update_tests_label();
      });
    }).detach();
  }

  // A failing check was clicked: show what it looked at.
  void reveal(const Assertion& a) {
    std::vector<PathStep> path;
    long msg = -1;
    if (target_json_path(a.target, path, &msg)) {
      select_tab(kBody);
      body_->reveal_json(std::move(path), msg, a.target);
      return;
    }
    const std::string t = a.target;
    if (t.rfind("res.headers", 0) == 0 || t.rfind("res.metadata", 0) == 0) select_tab(kHeaders);
    else if (t.rfind("res.trailers", 0) == 0 && tabs_->tab_shown(kTrailers)) select_tab(kTrailers);
  }

  // ------------------------------------------------------------------ log

  struct Run;
  void set_log(const Run& run) {
    const std::string text = http_transcript(*run.resp, run.sent.get(), request_->rows(),
                                             run.at.Format("%Y-%m-%d %H:%M:%S").utf8_string());
    fill_log(text);
  }

  // One colour per kind of line: "*" info dim, ">" sent in the accent colour, "<" received plain.
  void fill_log(const std::string& text) {
    const Theme& th = Theme::get();
    log_->Freeze();
    log_->Clear();
    size_t at = 0;
    while (at < text.size()) {
      size_t e = text.find('\n', at);
      if (e == std::string::npos) e = text.size() - 1;
      const char p = text[at];
      log_->SetDefaultStyle(wxTextAttr(p == '>' ? th.accent : p == '<' ? th.fg : th.dim, th.bg));
      log_->AppendText(U(std::string_view(text).substr(at, e + 1 - at)));
      at = e + 1;
    }
    log_->ShowPosition(0);
    log_->Thaw();
  }

  void set_grpc_log(const GrpcResponse& r) {
    std::string o;
    auto line = [&](char p, const std::string& t) {
      o += p;
      o += ' ';
      o += t;
      o += '\n';
    };
    line('*', wxDateTime::Now().Format("%Y-%m-%d %H:%M:%S").utf8_string() + "  " + grpc_label_);
    for (const InfoRow& row : grpc_conn_rows(grpc_shown_meta_, r))
      if (!row.section) line('*', row.key + ": " + row.value);
    line('*', "");
    line('>', "POST " + (grpc_label_.empty() ? std::string("(method)") : grpc_label_.substr(0, grpc_label_.find(" @ "))) +
                  " HTTP/2");
    line('>', "content-type: application/grpc" + std::string(grpc_shown_meta_.web ? "-web" : ""));
    line('>', "");
    if (!r.headers.empty() || r.ok || !r.messages.empty()) {
      line('<', "HTTP/2 200");
      for (const auto& [k, v] : r.headers) line('<', k + ": " + v);
      line('<', "");
    }
    for (size_t i = 0; i < r.messages.size(); ++i) {
      const GrpcMessageEvent& m = r.messages[i];
      line('<', "message #" + std::to_string(i + 1) + "  +" + fmt::ms(m.at_ms) + "  " + fmt::human_bytes(m.wire_bytes) +
                    (m.decode_error.empty() ? std::string() : "  decode error: " + m.decode_error));
      if (i >= 199 && r.messages.size() > 200) {
        line('<', "\xE2\x80\xA6 " + std::to_string(r.messages.size() - 200) + " more (see Messages)");
        break;
      }
    }
    for (const auto& [k, v] : r.trailers) line('<', "trailer " + k + ": " + v);
    line('*', "Status " + std::to_string(r.status_code) + " " + r.status_name +
                  (r.status_message.empty() ? std::string() : ": " + r.status_message));
    const Timing& t = r.timing;
    line('*', "Timing: queue " + fmt::ms(t.queue_ms) + ", DNS " + fmt::ms(t.dns_ms) + ", connect " + fmt::ms(t.connect_ms) +
                  ", TLS " + fmt::ms(t.tls_ms) + (r.reflect_ms > 0 ? ", reflection " + fmt::ms(r.reflect_ms) : std::string()) +
                  ", send " + fmt::ms(t.send_ms) + ", wait " + fmt::ms(t.ttfb_ms) + ", download " + fmt::ms(t.download_ms) +
                  ", total " + fmt::ms(t.total_ms));
    fill_log(o);
  }

  // The connection popover for what is shown (the HTTP run or the last gRPC call).
  void open_conn(const wxRect& anchor) {
    if (showing_grpc_ && grpc_last_) {
      show_info_popup(this, anchor, "Channel \xC2\xB7 " + (grpc_shown_meta_.address.empty() ? std::string("gRPC") : grpc_shown_meta_.address),
                      grpc_conn_rows(grpc_shown_meta_, *grpc_last_));
      return;
    }
    if (runs_.empty() || shown_run_ >= runs_.size()) return;
    const Run& run = runs_[shown_run_];
    std::string title = "Connection";
    if (!run.label.empty()) title += " \xC2\xB7 " + request_tag(run.label);
    show_info_popup(this, anchor, title, http_conn_rows(*run.resp, run.sent.get()));
  }

  void start_stream() {
    body_->set_stale(false);
    live_ = true;
    body_->begin_stream();
    msgs_->reset();
    headers_->set({});
    trailers_->set({});
    set_kind_tabs(true, false);
  }

  void set_kind_tabs(bool grpc, bool http) {
    if (!http) tabs_->show_tab(kCookies, false);
    tabs_->show_tab(kTrailers, grpc);
    tabs_->show_tab(kMessages, grpc);
    tabs_->show_tab(kRequest, http);
    if (!grpc) {
      tabs_->set_label(kTrailers, "Trailers");
      tabs_->set_label(kMessages, "Messages");
    }
    if (!grpc && !http) tabs_->set_label(kHeaders, "Headers");
    if (!tabs_->tab_shown(tabs_->selection())) select_tab(kBody);
  }

  void select_tab(int i) {
    tabs_->select(i);
    book_->SetSelection(static_cast<size_t>(i));
    if (i == kTests) refresh_asserts(true);
  }

  void add_delta(std::vector<StatusSeg>& segs, const TimingModel& cur, const TimingModel& prev,
                 const std::string& label, const wxDateTime& at) {
    const double total = cur.total;
    const double d = total - prev.total;
    // Run-to-run jitter (tens of microseconds locally, a few percent) is not a regression.
    const bool noise = std::fabs(d) < std::max(0.1, 0.05 * std::max(total, prev.total));
    // One run on a fresh connection, the other on a reused one: the difference is mostly the
    // handshake, not the server. Said so, and not coloured as a regression / improvement.
    const bool conn_changed = cur.reused != prev.reused;
    std::string text = fmt::delta_ms(d) + " vs previous";
    if (conn_changed) text += cur.reused ? " (reused conn.)" : " (new conn.)";
    std::string tip = "Total compared with the previous run of the same request" +
                      (label.empty() ? std::string() : ":\n" + label) + "\n(" + fmt::ms(prev.total) + " at " +
                      at.Format("%H:%M:%S").utf8_string() +
                      "). The waterfall shows the difference per phase. Requests with another method, URL or "
                      "body are never compared.";
    if (conn_changed)
      tip += std::string("\n\nThis run ") + (cur.reused ? "reused an open connection" : "opened a new connection") +
             " and the previous one " + (prev.reused ? "reused one" : "opened a new one") +
             ", so most of the difference is connection setup. Server wait: " +
             fmt::delta_ms(cur.server_wait - prev.server_wait) + ".";
    segs.push_back({text, tip, noise || conn_changed ? Tone::Dim : (d < 0 ? Tone::Ok : Tone::Err), false, 4});
  }

  struct Base {
    TimingModel model;
    wxDateTime at;
  };
  const Base* base_for(const std::string& key) const {
    if (key.empty()) return nullptr;
    const auto it = bases_.find(key);
    return it == bases_.end() ? nullptr : &it->second;
  }
  void remember(const std::string& key, const TimingModel& m) {
    if (key.empty()) return;
    if (bases_.size() > 256) bases_.clear();
    bases_[key] = Base{m, wxDateTime::Now()};
  }

  void update_history_chip() {
    const bool any = runs_.size() > 1;
    hist_->set_visible(1, any);
    if (!any) return hist_->Refresh();
    const bool latest = shown_run_ + 1 == runs_.size();
    hist_->set_label(1, (latest ? std::string("Latest") : "Run " + std::to_string(shown_run_ + 1)) + " of " +
                            std::to_string(runs_.size()) + " \xE2\x96\xBE");
    hist_->set_on(1, !latest);
  }

  void history_menu() {
    if (runs_.size() < 2) return;
    wxMenu m;
    const wxDateTime now = wxDateTime::Now();
    for (size_t i = runs_.size(); i-- > 0;) {
      const Run& x = runs_[i];
      const long secs = (now - x.at).GetSeconds().ToLong();
      std::string when = secs < 10 ? "just now" : secs < 60 ? std::to_string(secs) + " s ago"
                                                : secs < 3600 ? std::to_string(secs / 60) + " min ago"
                                                              : x.at.Format("%H:%M:%S").utf8_string();
      std::string label = "#" + std::to_string(i + 1) + "   " + when + "   ";
      label += x.resp->ok ? std::to_string(x.resp->status) : std::string("failed");
      label += "   " + fmt::ms(x.resp->timing.total_ms) + "   " + fmt::human_bytes(x.resp->body.size());
      if (!x.label.empty()) label += "   \xC2\xB7   " + elide_middle(x.label, 80);
      m.AppendCheckItem(static_cast<int>(i) + 100, U(label))->Check(i == shown_run_);
    }
    const int id = GetPopupMenuSelectionFromUser(m, hist_->GetPosition() + wxPoint(0, hist_->GetSize().y));
    if (id >= 100 && static_cast<size_t>(id - 100) < runs_.size() && static_cast<size_t>(id - 100) != shown_run_)
      show_run(static_cast<size_t>(id - 100));
  }

  static bool has_header(const std::vector<Header>& h, std::string_view name) {
    for (const auto& [k, v] : h)
      if (k.size() == name.size() &&
          std::equal(k.begin(), k.end(), name.begin(), [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == y; }))
        return true;
    return false;
  }

  // Set-Cookie headers of the final response: name -> "value; Path=/; HttpOnly ...".
  void set_cookies(const HttpResponse& r) {
    KV rows;
    for (const auto& [k, v] : r.headers) {
      if (k.size() != 10) continue;
      std::string lk(k);
      for (char& c : lk) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (lk != "set-cookie") continue;
      const std::string_view sv = v;
      const size_t semi = sv.find(';');
      const std::string_view pair = sv.substr(0, semi);
      const size_t eq = pair.find('=');
      std::string name(pair.substr(0, eq));
      std::string value(eq == std::string_view::npos ? std::string_view() : pair.substr(eq + 1));
      if (semi != std::string_view::npos) {
        std::string_view attrs = sv.substr(semi + 1);
        while (!attrs.empty() && attrs.front() == ' ') attrs.remove_prefix(1);
        value += "  \xC2\xB7  ";
        value += attrs;
      }
      rows.emplace_back(std::move(name), std::move(value));
    }
    const size_t n = rows.size();
    cookies_->set(std::move(rows));
    tabs_->show_tab(kCookies, n > 0);
    tabs_->set_label(kCookies, "Cookies", n ? std::to_string(n) : std::string());
    if (!tabs_->tab_shown(tabs_->selection())) select_tab(kBody);
  }

  // What went out: request line, the headers as sent (the ones libcurl adds itself dimmed), the
  // body, then every redirect hop and the final URL.
  void set_request(const SentHttp* sent, const HttpResponse* r) {
    if (!sent) {
      request_->set({});
      return;
    }
    const HttpRequest& q = sent->request;
    KV rows;
    std::vector<char> dim;
    auto row = [&](std::string k, std::string v, bool auto_added) {
      rows.emplace_back(std::move(k), std::move(v));
      dim.push_back(auto_added ? 1 : 0);
    };
    row(q.method, q.url, false);
    if (!has_header(q.headers, "host")) {
      std::string_view u = q.url;
      const size_t sch = u.find("://");
      if (sch != std::string_view::npos) u.remove_prefix(sch + 3);
      u = u.substr(0, u.find_first_of("/?#"));
      if (const size_t at = u.rfind('@'); at != std::string_view::npos) u.remove_prefix(at + 1);
      if (!u.empty()) row("Host", std::string(u), true);
    }
    for (const auto& h : q.headers) row(h.first, h.second, false);
    if (!has_header(q.headers, "user-agent")) row("User-Agent", "watt/0.1", true);
    if (!has_header(q.headers, "accept")) row("Accept", "*/*", true);
    if (!has_header(q.headers, "accept-encoding")) row("Accept-Encoding", "deflate, gzip", true);
    if (!q.body.empty()) {
      if (!has_header(q.headers, "content-length")) row("Content-Length", std::to_string(q.body.size()), true);
      if (!has_header(q.headers, "content-type") && q.method != "GET" && q.method != "HEAD")
        row("Content-Type", "application/x-www-form-urlencoded", true);
    }
    const size_t nheaders = rows.size() - 1;
    if (q.body.empty()) {
      row("(body)", "none", true);
    } else {
      // First 2 KB of the body on one line (whitespace runs collapsed).
      std::string b;
      bool ws = false;
      for (size_t i = 0; i < q.body.size() && b.size() < 2048; ++i) {
        const char c = q.body[i];
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
          ws = true;
          continue;
        }
        if (ws && !b.empty()) b += ' ';
        ws = false;
        b += c;
      }
      if (b.size() < q.body.size()) b += "\xE2\x80\xA6";
      row("(body) " + fmt::human_bytes(q.body.size()), std::move(b), false);
    }
    if (r) {
      int i = 0;
      for (const RedirectHop& h : r->hops) {
        std::string v = std::to_string(h.status) + "  " + h.url + "  \xC2\xB7  " + fmt::ms(h.total_ms) + " (wait " +
                        fmt::ms(h.ttfb_ms) + ")";
        if (h.connection_reused) v += "  \xC2\xB7  reused connection";
        const std::string remote = host_port(h.remote_ip, h.remote_port);
        if (!remote.empty()) v += "  \xC2\xB7  " + remote;
        row("Redirect " + std::to_string(++i), std::move(v), false);
      }
      if (!r->hops.empty()) row("Final URL", r->effective_url, false);
    }
    request_->set(std::move(rows), std::move(dim));
    tabs_->set_label(kRequest, "Request", std::to_string(nheaders));
  }

  struct Run {
    std::shared_ptr<const HttpResponse> resp;
    std::shared_ptr<const SentHttp> sent;
    TimingModel model;
    wxDateTime at;
    std::string key;    // method + expanded URL + body tag: what the delta compares by
    std::string label;  // "GET https://..." for the history menu and tooltips
    TimingModel base;   // previous run of the same request, when there was one
    wxDateTime base_at;
    bool has_base = false;
  };
  static constexpr size_t kRuns = 10;
  std::vector<Run> runs_;
  size_t shown_run_ = 0;
  ChipBar* hist_ = nullptr;
  StatusStrip* status_;
  Waterfall* wf_;
  TabStrip* tabs_;
  wxSimplebook* book_;
  BodyView* body_;
  KvList* headers_;
  KvList* cookies_;
  KvList* trailers_;
  MsgList* msgs_;
  KvList* request_;
  RequestKind kind_ = RequestKind::Http;
  bool live_ = false;
  std::map<std::string, Base> bases_;  // last good run per request key (HTTP and gRPC)
  std::string grpc_key_, grpc_label_;  // the gRPC call in flight
  GrpcConnMeta grpc_meta_;             // ... and its target
  GrpcConnMeta grpc_shown_meta_;       // target of grpc_last_
  std::shared_ptr<const GrpcResponse> grpc_last_;
  bool showing_grpc_ = false;
  std::string shown_ident_;            // the request the waterfall's figures belong to
  TestsView* tests_ = nullptr;
  wxTextCtrl* log_ = nullptr;
  uint64_t test_seq_ = 0;              // newest evaluation; older results are dropped
  bool asserts_owned_elsewhere_ = false;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

 public:
  ~ResponseView() override { *alive_ = false; }
};

}  // namespace

ResponsePanel* make_response_view(wxWindow* parent) { return new ResponseView(parent); }

}  // namespace watt::app
