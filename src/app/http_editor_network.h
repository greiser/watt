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

#include "core/model.h"

class wxStaticText;
class wxTextCtrl;

namespace watt::app::http_editor {

class ChoiceButton;
class CheckButton;

class NetworkPanel : public wxPanel {
 public:
  // top: the collection (nothing above it to inherit from: "Inherit" reads as the default).
  NetworkPanel(wxWindow* parent, const std::filesystem::path& root, bool top);
  void set(const NetworkSettings& n, const std::string& passphrase);
  NetworkSettings get() const;
  std::string passphrase() const;
  bool passphrase_changed() const { return passphrase() != passphrase0_; }
  // "Inherited: client certificate certs/a.pem from Collection, proxy: system from api" (or "").
  void set_inherited(const wxString& text);
  std::function<void()> on_change;  // a user edit

 private:
  void sync();
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
  wxStaticText* inherited_ = nullptr;
  wxWindow *proxy_url_row_ = nullptr, *no_proxy_row_ = nullptr, *proxy_url_label_ = nullptr,
           *no_proxy_label_ = nullptr;
};

}  // namespace watt::app::http_editor
