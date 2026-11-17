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
  auto label = [&](const char* text) {
    auto* l = new wxStaticText(this, wxID_ANY, text);
    l->SetForegroundColour(t.fg);
    l->SetMinSize(wxSize(FromDIP(96), -1));  // one label column width for all three sections
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

  auto* h1 = new wxStaticText(this, wxID_ANY, "Client certificate (mutual TLS)");
  h1->SetForegroundColour(t.fg);
  h1->SetFont(GetFont().Bold());
  s->Add(h1, 0, wxTOP, FromDIP(4));
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
  return n;
}

std::string NetworkPanel::passphrase() const { return pass_->GetValue().utf8_string(); }

void NetworkPanel::set_inherited(const wxString& text) {
  inherited_->SetLabel(text.empty() ? wxString() : "Above this level: " + text);
  inherited_->Wrap(FromDIP(640));
  Layout();
}

}  // namespace watt::app::http_editor
