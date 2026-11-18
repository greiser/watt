// "Certificates & proxy" fields (see http_editor_network.h).
#include "app/http_editor_network.h"

#include <wx/filedlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <vector>

#include "app/http_editor_widgets.h"

namespace watt::app::http_editor {

namespace {

namespace fs = std::filesystem;

std::string u8(const fs::path& p) {
  const std::u8string s = p.generic_u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

wxFont mono(int pt) {
  wxFont f(wxFontInfo(pt).Family(wxFONTFAMILY_TELETYPE).FaceName("Cascadia Mono"));
  if (!f.IsOk() || f.GetFaceName() != "Cascadia Mono")
    f = wxFont(wxFontInfo(pt).Family(wxFONTFAMILY_TELETYPE).FaceName("Consolas"));
  return f;
}

}  // namespace

NetworkPanel::NetworkPanel(wxWindow* parent, const fs::path& root, bool top)
    : wxPanel(parent), root_(root.lexically_normal()), top_(top) {
  const Theme& t = Theme::get();
  SetBackgroundColour(t.bg);
  auto* s = new wxBoxSizer(wxVERTICAL);
  auto* grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(10));
  grid->AddGrowableCol(1, 1);
  // One label column width for every section (the longest label is in the redirect one), so
  // the fields and choices of all sections start on one left edge.
  const int label_w = std::max(FromDIP(96), GetTextExtent("Credentials on redirect").x + FromDIP(4));
  auto label = [&](const char* text) {
    auto* l = new wxStaticText(this, wxID_ANY, text);
    l->SetForegroundColour(t.fg);
    l->SetMinSize(wxSize(label_w, -1));
    grid->Add(l, 0, wxALIGN_CENTER_VERTICAL);
    return l;
  };
  auto file_row = [&](wxTextCtrl* f, const wxString& title, const wxString& wildcard) {
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(f, 1, wxALIGN_CENTER_VERTICAL);
    auto* b = new FlatButton(this, wxString::FromUTF8("Browse\xE2\x80\xA6"), FlatButton::Style::Subtle);
    b->on_click = [this, f, title, wildcard] { browse(f, title, wildcard); };
    row->Add(b, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    grid->Add(row, 1, wxEXPAND);
  };

  {
    // Redirects, URL encoding, TLS version (Postman's / Bruno's / Insomnia's request settings).
    auto* h0 = new wxStaticText(this, wxID_ANY, "Redirects, URL and TLS");
    h0->SetForegroundColour(t.fg);
    h0->SetFont(GetFont().Bold());
    s->Add(h0, 0, wxTOP, FromDIP(4));
    auto* g = new wxFlexGridSizer(2, FromDIP(6), FromDIP(10));
    g->AddGrowableCol(1, 1);
    auto lab = [&](const char* text, const char* tip) {
      auto* l = new wxStaticText(this, wxID_ANY, text);
      l->SetForegroundColour(t.fg);
      l->SetToolTip(tip);
      l->SetMinSize(wxSize(label_w, -1));
      g->Add(l, 0, wxALIGN_CENTER_VERTICAL);
      return l;
    };
    const char* inh = top ? "Default" : "Inherit";
    auto* l1 = lab("Max redirects", "Hops followed before the request fails (curl --max-redirs). 0 to 100.");
    max_redirs_ = field("");
    max_redirs_->SetMinSize(wxSize(FromDIP(240), -1));
    max_redirs_->SetMaxLength(3);
    max_redirs_->Bind(wxEVT_CHAR, [](wxKeyEvent& e) {
      const int k = e.GetKeyCode();
      if (k >= 32 && k < 127 && !(k >= '0' && k <= '9') && !e.ControlDown()) return;  // digits only
      e.Skip();
    });
    g->Add(max_redirs_, 0);
    auto* l2 = lab("Method on redirect",
                   "301 / 302 / 303: browsers (and curl) switch a POST to GET and drop the body. Keep sends the same "
                   "method and body to the new location (curl --post301 --post302 --post303). 307 / 308 always keep it.");
    keep_method_ = new ChoiceButton(this, {inh, "Keep the method and body", "Switch to GET on 301 / 302 / 303 (like a browser)"});
    g->Add(keep_method_, 0);
    auto* l3 = lab("Credentials on redirect",
                   "Authorization and Cookie headers, Basic / Digest / AWS credentials: sent to the original host "
                   "only, or also to a redirect target on another host (curl --location-trusted). Windows SSO "
                   "(NTLM / Negotiate) never goes to another host.");
    keep_auth_ = new ChoiceButton(this, {inh, "Original host only", "Also to other hosts (Authorization follows the redirect)"});
    g->Add(keep_auth_, 0);
    lab("Encode URL",
        "On: characters HTTP does not allow in a URL (space, quotes, |, {}, non-ASCII) and {{variable}} values in "
        "the query are percent-encoded. Off: the path and query go out as typed (curl --path-as-is; only spaces "
        "and control characters, which no request line can carry, are encoded).");
    encode_url_ = new ChoiceButton(this, {inh, "Percent-encode what a URL may not hold", "Send the path and query as typed"});
    g->Add(encode_url_, 0);
    lab("Minimum TLS", "The lowest TLS version offered to the server (curl --tlsv1.x).");
    tls_min_ = new ChoiceButton(this, {inh, "Windows default (TLS 1.2 and 1.3)", "TLS 1.0", "TLS 1.1", "TLS 1.2", "TLS 1.3"});
    g->Add(tls_min_, 0);
    s->Add(g, 0, wxEXPAND | wxTOP, FromDIP(6));
    redirect_rows_ = {l1, max_redirs_, l2, keep_method_, l3, keep_auth_};
    max_redirs_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
      if (!loading_ && on_change) on_change();
    });
    for (ChoiceButton* c : {keep_method_, keep_auth_, encode_url_, tls_min_})
      c->on_select = [this](int) {
        if (on_change) on_change();
      };
    // A number, not a path: the UI font like the choices next to it, which are drawn as fields
    // with their text on the same left edge.
    max_redirs_->SetFont(GetFont());
    for (ChoiceButton* c : {keep_method_, keep_auth_, encode_url_, tls_min_}) c->set_field_look(true);
    label_inherit();
    // One width for the four choices (and the field), so they line up as a column.
    int w = max_redirs_->GetMinSize().x;
    for (ChoiceButton* c : {keep_method_, keep_auth_, encode_url_, tls_min_}) w = std::max(w, c->GetMinSize().x);
    const int fh = max_redirs_->GetBestSize().y;  // and one height: the field's
    max_redirs_->SetMinSize(wxSize(w, -1));
    for (ChoiceButton* c : {keep_method_, keep_auth_, encode_url_, tls_min_})
      c->SetMinSize(wxSize(w, std::max(fh, c->GetBestSize().y)));
  }

  auto* h1 = new wxStaticText(this, wxID_ANY, "Client certificate (mutual TLS)");
  h1->SetForegroundColour(t.fg);
  h1->SetFont(GetFont().Bold());
  s->Add(h1, 0, wxTOP, FromDIP(14));
  label("Certificate");
  cert_ = field(top ? "certs/client.pem or client.pfx (empty: none)"
                    : "certs/client.pem or client.pfx (empty: inherit)");
  file_row(cert_, "Client certificate",
           "Certificates (*.pem;*.crt;*.cer;*.pfx;*.p12)|*.pem;*.crt;*.cer;*.pfx;*.p12|All files (*.*)|*.*");
  label("Private key");
  key_ = field("certs/client.key (PEM; empty with a .pfx or a PEM holding the key)");
  file_row(key_, "Private key", "Keys (*.key;*.pem)|*.key;*.pem|All files (*.*)|*.*");
  label("Passphrase");
  pass_ = field("only for an encrypted key or a .pfx (kept DPAPI-sealed in .watt, never in git)", true);
  grid->Add(pass_, 1, wxEXPAND);
  s->Add(grid, 0, wxEXPAND | wxTOP, FromDIP(6));
  cert_off_ = new CheckButton(this, "No client certificate for these requests (even if a level above sets one)");
  s->Add(cert_off_, 0, wxTOP, FromDIP(6));
  cert_off_->Show(!top);

  auto* h2 = new wxStaticText(this, wxID_ANY, "Server certificate");
  h2->SetForegroundColour(t.fg);
  h2->SetFont(GetFont().Bold());
  s->Add(h2, 0, wxTOP, FromDIP(14));
  grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(10));
  grid->AddGrowableCol(1, 1);
  label("CA certificate");
  ca_ = field(top ? "certs/ca.pem: trust this CA instead of the Windows roots (empty: Windows roots)"
                  : "certs/ca.pem (empty: inherit; the Windows roots at the top)");
  file_row(ca_, "CA certificate (PEM)", "PEM certificates (*.pem;*.crt;*.cer)|*.pem;*.crt;*.cer|All files (*.*)|*.*");
  s->Add(grid, 0, wxEXPAND | wxTOP, FromDIP(6));
  ca_off_ = new CheckButton(this, "Use the Windows trust store here (ignore a CA set above)");
  s->Add(ca_off_, 0, wxTOP, FromDIP(6));
  ca_off_->Show(!top);

  auto* h3 = new wxStaticText(this, wxID_ANY, "Proxy");
  h3->SetForegroundColour(t.fg);
  h3->SetFont(GetFont().Bold());
  s->Add(h3, 0, wxTOP, FromDIP(14));
  grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(10));
  grid->AddGrowableCol(1, 1);
  label("Proxy");
  proxy_ = new ChoiceButton(this, {top ? wxString("Default (HTTPS_PROXY / NO_PROXY variables)")
                                       : wxString("Inherit from parent"),
                                   "Environment variables (HTTPS_PROXY / NO_PROXY)", "Windows system proxy",
                                   "No proxy (direct)", "Custom proxy URL"});
  proxy_->set_field_look(true);
  proxy_->SetMinSize(wxSize(proxy_->GetBestSize().x, std::max(proxy_->GetBestSize().y, ca_->GetBestSize().y)));
  grid->Add(proxy_, 0);
  proxy_url_label_ = label("Proxy URL");
  proxy_url_ = field("http://proxy.corp:8080  (http, https, socks5h; user:{{pw}}@ for credentials)");
  grid->Add(proxy_url_, 1, wxEXPAND);
  proxy_url_row_ = proxy_url_;
  no_proxy_label_ = label("Bypass");
  no_proxy_ = field("localhost, .internal.corp, 10.0.0.0/8  (hosts that go direct)");
  grid->Add(no_proxy_, 1, wxEXPAND);
  no_proxy_row_ = no_proxy_;
  s->Add(grid, 0, wxEXPAND | wxTOP, FromDIP(6));

  inherited_ = new wxStaticText(this, wxID_ANY, "");
  inherited_->SetForegroundColour(t.dim);
  s->Add(inherited_, 0, wxEXPAND | wxTOP, FromDIP(12));
  SetSizer(s);

  for (wxTextCtrl* f : {ca_, cert_, key_, pass_, proxy_url_, no_proxy_})
    f->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
      if (!loading_ && on_change) on_change();
    });
  for (CheckButton* c : {ca_off_, cert_off_})
    c->on_change = [this] {
      sync();
      if (on_change) on_change();
    };
  proxy_->on_select = [this](int) {
    sync();
    if (on_change) on_change();
  };
  sync();
}

wxTextCtrl* NetworkPanel::field(const char* hint, bool secret) {
  const Theme& t = Theme::get();
  auto* f = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                           wxBORDER_SIMPLE | (secret ? wxTE_PASSWORD : 0));
  f->SetBackgroundColour(t.field);
  f->SetForegroundColour(t.fg);
  f->SetFont(mono(9));
  f->SetHint(hint);
  f->SetMinSize(wxSize(FromDIP(160), -1));
  return f;
}

void NetworkPanel::browse(wxTextCtrl* f, const wxString& title, const wxString& wildcard) {
  wxFileDialog dlg(this, title, wxString(root_.wstring()), "", wildcard, wxFD_OPEN | wxFD_FILE_MUST_EXIST);
  if (dlg.ShowModal() != wxID_OK) return;
  const fs::path p = fs::path(dlg.GetPath().ToStdWstring()).lexically_normal();
  const fs::path rel = p.lexically_relative(root_);
  const bool inside = !rel.empty() && *rel.begin() != "..";
  f->SetValue(wxString::FromUTF8(inside ? u8(rel) : u8(p)));
}

void NetworkPanel::sync() {
  const bool url = proxy_->selection() == static_cast<int>(NetworkSettings::Proxy::Url);
  const bool sys = proxy_->selection() == static_cast<int>(NetworkSettings::Proxy::System);
  proxy_url_label_->Show(url);
  proxy_url_row_->Show(url);
  no_proxy_label_->Show(url || sys);
  no_proxy_row_->Show(url || sys);
  for (wxTextCtrl* f : {cert_, key_, pass_}) f->Enable(!cert_off_->value());
  ca_->Enable(!ca_off_->value());
  Layout();
  if (GetParent()) GetParent()->Layout();
}

void NetworkPanel::set(const NetworkSettings& n, const std::string& passphrase) {
  loading_ = true;
  ca_->ChangeValue(wxString::FromUTF8(n.ca_file));
  cert_->ChangeValue(wxString::FromUTF8(n.cert_file));
  key_->ChangeValue(wxString::FromUTF8(n.key_file));
  pass_->ChangeValue(wxString::FromUTF8(passphrase));
  passphrase0_ = passphrase;
  proxy_url_->ChangeValue(wxString::FromUTF8(n.proxy_url));
  no_proxy_->ChangeValue(wxString::FromUTF8(n.no_proxy));
  ca_off_->set_value(n.ca_off);
  cert_off_->set_value(n.cert_off);
  proxy_->set_selection(static_cast<int>(n.proxy));
  max_redirs_->ChangeValue(n.max_redirects ? wxString::Format("%ld", *n.max_redirects) : wxString());
  keep_method_->set_selection(!n.keep_method ? 0 : *n.keep_method ? 1 : 2);
  keep_auth_->set_selection(!n.keep_auth ? 0 : *n.keep_auth ? 2 : 1);
  encode_url_->set_selection(!n.encode_url ? 0 : *n.encode_url ? 1 : 2);
  tls_min_->set_selection(n.tls_min ? 1 + static_cast<int>(*n.tls_min) : 0);
  loading_ = false;
  sync();
}

NetworkSettings NetworkPanel::get() const {
  NetworkSettings n;
  auto v = [](wxTextCtrl* f) {
    std::string s = f->GetValue().utf8_string();
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    // A path pasted with quotes (Explorer's "Copy as path").
    s = s.substr(i);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return s;
  };
  n.ca_off = !top_ && ca_off_->value();
  n.cert_off = !top_ && cert_off_->value();
  if (!n.ca_off) n.ca_file = v(ca_);
  if (!n.cert_off) {
    n.cert_file = v(cert_);
    n.key_file = v(key_);
  }
  n.proxy = static_cast<NetworkSettings::Proxy>(std::clamp(proxy_->selection(), 0, 4));
  if (n.proxy == NetworkSettings::Proxy::Url) n.proxy_url = v(proxy_url_);
  if (n.proxy == NetworkSettings::Proxy::Url || n.proxy == NetworkSettings::Proxy::System) n.no_proxy = v(no_proxy_);
  if (long r = 0; v(max_redirs_).size() && max_redirs_->GetValue().ToLong(&r)) n.max_redirects = std::clamp<long>(r, 0, 100);
  if (const int k = keep_method_->selection(); k > 0) n.keep_method = k == 1;
  if (const int k = keep_auth_->selection(); k > 0) n.keep_auth = k == 2;
  if (const int k = encode_url_->selection(); k > 0) n.encode_url = k == 1;
  if (const int k = tls_min_->selection(); k > 0) n.tls_min = static_cast<TlsVersion>(std::clamp(k - 1, 0, 4));
  return n;
}

std::string NetworkPanel::passphrase() const { return pass_->GetValue().utf8_string(); }

void NetworkPanel::set_above(const OptionsAbove& above) {
  if (above_known_ && above == above_) return;
  above_known_ = true;
  above_ = above;
  label_inherit();
}

void NetworkPanel::set_follow(bool on) {
  for (wxWindow* w : redirect_rows_) w->Enable(on);
}

// "Inherit: 30 (default)", "Inherit: keep (api/users)": the value an unset option gets here.
void NetworkPanel::label_inherit() {
  const HttpRequest def;
  const NetworkSettings& n = above_.n;
  if (!top_ && !above_known_) {  // a folder: what its parents set is not looked up here
    max_redirs_->SetHint("Inherit from parent");
    for (ChoiceButton* c : {keep_method_, keep_auth_, encode_url_, tls_min_}) c->set_item(0, "Inherit from parent");
    return;
  }
  auto from = [this](const std::string& f) {
    if (top_ && f.empty()) return std::string();
    return " (" + (f.empty() ? std::string("default") : f) + ")";
  };
  const std::string head = top_ ? "Default: " : "Inherit: ";
  const long redirs = n.max_redirects.value_or(def.max_redirects);
  max_redirs_->SetHint(wxString::FromUTF8(head + std::to_string(redirs) + from(above_.max_redirects_from)));
  keep_method_->set_item(0, wxString::FromUTF8(head + (n.keep_method.value_or(false) ? "keep" : "switch to GET") +
                                               from(above_.keep_method_from)));
  keep_auth_->set_item(0, wxString::FromUTF8(head + (n.keep_auth.value_or(false) ? "also other hosts" : "original host only") +
                                             from(above_.keep_auth_from)));
  encode_url_->set_item(0, wxString::FromUTF8(head + (n.encode_url.value_or(true) ? "encode" : "as typed") +
                                              from(above_.encode_url_from)));
  static const char* kTls[] = {"Windows default", "TLS 1.0", "TLS 1.1", "TLS 1.2", "TLS 1.3"};
  tls_min_->set_item(0, wxString::FromUTF8(head + kTls[std::clamp(static_cast<int>(n.tls_min.value_or(TlsVersion::Default)), 0, 4)] +
                                           from(above_.tls_min_from)));
}

void NetworkPanel::set_inherited(const wxString& text) {
  inherited_->SetLabel(text.empty() ? wxString() : "Above this level: " + text);
  inherited_->Wrap(FromDIP(640));
  Layout();
}

}  // namespace watt::app::http_editor
