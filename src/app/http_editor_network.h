#pragma once
// "Certificates & proxy" fields (core NetworkSettings) shared by the folder / collection settings
// dialog and the HTTP editor's Options tab: CA bundle, client certificate (PEM certificate + key,
// or a .pfx / .p12) with its passphrase, and the proxy. Each part can inherit from the level
// above (Inherit / empty), be set here, or be switched off here. File paths inside the workspace
// are stored relative to its root. Nothing here touches the disk: the passphrase is read and
// written by the caller on a worker (Workspace::cert_passphrase / set_cert_passphrase).
#include <wx/panel.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "app/http_editor_inherit.h"
#include "core/model.h"

class wxStaticText;
class wxTextCtrl;

class wxFlexGridSizer;
class wxBoxSizer;

namespace watt::app::http_editor {

class ChoiceButton;
class CheckButton;

class NetworkPanel : public wxPanel {
 public:
  // top: the collection (nothing above it to inherit from: "Inherit" reads as the default).
  // staged: only the first section now, the rest in build_step() calls (idle steps of ~10 ms
  // each instead of one ~40 ms step); set() / get() and the rest need complete().
  NetworkPanel(wxWindow* parent, const std::filesystem::path& root, bool top, bool staged = false);
  bool build_step();  // builds the next section; true once the panel is complete
  bool complete() const { return stage_ >= 3; }
  void set(const NetworkSettings& n, const std::string& passphrase);
  NetworkSettings get() const;
  std::string passphrase() const;
  bool passphrase_changed() const { return passphrase() != passphrase0_; }
  // "Inherited: client certificate certs/a.pem from Collection, proxy: system from api" (or "").
  void set_inherited(const wxString& text);
  // What the levels above set for the redirect / URL / TLS options: the "Inherit" choices name
  // the value that applies and where it comes from ("Inherit: on (api)").
  void set_above(const OptionsAbove& above);
  // Follow redirects is off (the request's own check box): the redirect rows are dimmed.
  void set_follow(bool on);
  std::function<void()> on_change;  // a user edit

 private:
  void sync();
  void label_inherit();
  void build_certs();     // stage 1: client certificate
  void build_ca_proxy();  // stage 2: server certificate, proxy, the bindings
  wxStaticText* add_label(wxFlexGridSizer* grid, const char* text);
  void add_file_row(wxFlexGridSizer* grid, wxTextCtrl* f, const wxString& title, const wxString& wildcard);
  int stage_ = 0;
  int label_w_ = 0;
  wxBoxSizer* sizer_ = nullptr;
  wxTextCtrl* field(const char* hint, bool secret = false);
  void browse(wxTextCtrl* f, const wxString& title, const wxString& wildcard);

  std::filesystem::path root_;
  bool top_;
  bool loading_ = false;
  std::string passphrase0_;
  wxTextCtrl *ca_ = nullptr, *cert_ = nullptr, *key_ = nullptr, *pass_ = nullptr, *proxy_url_ = nullptr,
             *no_proxy_ = nullptr;
  CheckButton *ca_off_ = nullptr, *cert_off_ = nullptr;
  ChoiceButton* proxy_ = nullptr;
  // Redirect / URL / TLS options (NetworkSettings' optional ones): index 0 = inherit.
  wxTextCtrl* max_redirs_ = nullptr;
  ChoiceButton *keep_method_ = nullptr, *keep_auth_ = nullptr, *encode_url_ = nullptr, *tls_min_ = nullptr;
  std::vector<wxWindow*> redirect_rows_;
  OptionsAbove above_;
  bool above_known_ = false;  // set_above() was called (the request's Options tab)
  wxStaticText* inherited_ = nullptr;
  wxWindow *proxy_url_row_ = nullptr, *no_proxy_row_ = nullptr, *proxy_url_label_ = nullptr,
           *no_proxy_label_ = nullptr;
};

}  // namespace watt::app::http_editor
