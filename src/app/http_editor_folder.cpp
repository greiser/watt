// Folder / collection settings dialog (see http_editor_folder.h).
#include "app/http_editor_folder.h"

#include <wx/app.h>
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/weakref.h>
#include <wx/wupdlock.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "app/http_editor_inherit.h"
#include "app/http_editor_kv.h"
#include "app/http_editor_network.h"
#include "app/http_editor_util.h"
#include "app/http_editor_widgets.h"
#include "core/model.h"

namespace watt::app::http_editor {

namespace {

namespace fs = std::filesystem;

// "Collection" for the root, else the folder relative to it ("api/users").
std::string folder_label(const Workspace& ws, const fs::path& folder) {
  const fs::path rel = folder.lexically_normal().lexically_relative(ws.root().lexically_normal());
  if (rel.empty() || rel == ".") return "Collection";
  const std::u8string g = rel.generic_u8string();
  return std::string(reinterpret_cast<const char*>(g.data()), g.size());
}

wxFont mono(int pt) {
  wxFont f(wxFontInfo(pt).Family(wxFONTFAMILY_TELETYPE).FaceName("Cascadia Mono"));
  if (!f.IsOk() || f.GetFaceName() != "Cascadia Mono") f = wxFont(wxFontInfo(pt).Family(wxFONTFAMILY_TELETYPE).FaceName("Consolas"));
  return f;
}

std::vector<KvRow> to_kv(const std::vector<SettingsRow>& rows) {
  std::vector<KvRow> out;
  for (const auto& r : rows) {
    KvRow k;
    k.key = r.name;
    k.value = r.value;
    k.enabled = r.enabled;
    k.desc = r.description;
    out.push_back(std::move(k));
  }
  return out;
}

std::vector<SettingsRow> from_kv(const std::vector<KvRow>& rows) {
  std::vector<SettingsRow> out;
  for (const auto& r : rows) {
    const std::string name(trim(r.key));
    if (name.empty()) continue;  // a value without a name is never sent
    out.push_back({name, r.value, r.desc, r.enabled});
  }
  return out;
}

// What a page of the dialog needs to resolve {{vars}} (chips and completion in its tables).
struct VarSource {
  VarLookup lookup;
  std::function<std::vector<std::string>()> names;
};

// The dialog builds what it shows: the auth fields of the picked type only, and each tab's page
// when it is first opened (a dialog with every field and page at once took 50-130 ms to open).
class FolderDialog : public wxDialog {
 public:
  FolderDialog(wxWindow* parent, const wxString& label, bool collection, const FolderSettings& s, VarSource vars,
               std::filesystem::path root, std::string passphrase)
      : wxDialog(parent, wxID_ANY, collection ? wxString("Collection settings") : "Folder settings: " + label,
                 wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
        root_(std::move(root)), passphrase_(std::move(passphrase)), collection_(collection), settings_(s),
        form_(folder_form(s)), vars_src_(std::move(vars)) {
    wxWindowUpdateLocker lock(this);
    const Theme& t = Theme::get();
    SetBackgroundColour(t.bg);
    SetForegroundColour(t.fg);
    auto* top = new wxBoxSizer(wxVERTICAL);
    const int pad = FromDIP(16);

    auto* title = new wxStaticText(this, wxID_ANY, collection ? wxString("Collection") : label);
    title->SetForegroundColour(t.fg);
    title->SetFont(GetFont().Bold().Scaled(1.15f));
    top->Add(title, 0, wxLEFT | wxRIGHT | wxTOP, pad);
    auto* hint = new wxStaticText(
        this, wxID_ANY,
        collection ? "Every request of the workspace whose auth is \"Inherit from parent\" uses this auth unless a "
                     "folder above it sets its own. Headers and assertions go with every request unless it (or a "
                     "folder) sets the same header; scripts run before the folders' and the request's own."
                   : "Requests below this folder whose auth is \"Inherit from parent\" use this auth (Inherit: the "
                     "parent folder's or the collection's). Headers and assertions go with every request below unless "
                     "it sets the same header; scripts run after the collection's and parent folders', before the "
                     "request's.");
    hint->SetForegroundColour(t.dim);
    hint->Wrap(FromDIP(680));
    top->Add(hint, 0, wxLEFT | wxRIGHT | wxTOP, pad);

    // Auth type + its fields.
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto* al = new wxStaticText(this, wxID_ANY, "Auth");
    al->SetForegroundColour(t.dim);
    row->Add(al, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    std::vector<wxString> labels(std::begin(kFolderAuthLabels), std::end(kFolderAuthLabels));
    if (collection) labels[0] = "Inherit (none: the collection is the top)";
    type_ = new ChoiceButton(this, labels);
    type_->SetToolTip("Pick the auth type (click, or Space / Enter, then type to filter)");
    type_->set_selection(static_cast<int>(form_.auth));
    type_->on_select = [this](int i) {
      form_.auth = static_cast<FolderAuth>(i);
      show_fields();
    };
    row->Add(type_, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(row, 0, wxLEFT | wxRIGHT | wxTOP, pad);

    grid_ = new wxFlexGridSizer(2, FromDIP(6), FromDIP(10));
    grid_->AddGrowableCol(1, 1);
    OAuth2Config& o = form_.adv.oauth2;
    add(&f_token_, "Token", &form_.token, "{{token}}", {FolderAuth::Bearer});
    add(&f_user_, "Username", &form_.user, "user", {FolderAuth::Basic});
    add(&f_pass_, "Password", &form_.pass, "", {FolderAuth::Basic}, true);
    add(&f_key_name_, "Header", &form_.key_name, "X-API-Key", {FolderAuth::ApiKey});
    add(&f_key_value_, "Value", &form_.key_value, "{{apiKey}}", {FolderAuth::ApiKey});
    add(&f_s_user_, "Username", &form_.adv.user, "DOMAIN\\user (empty: your Windows sign-in)",
        {FolderAuth::Digest, FolderAuth::Ntlm, FolderAuth::Negotiate});
    add(&f_s_pass_, "Password", &form_.adv.password, "", {FolderAuth::Digest, FolderAuth::Ntlm, FolderAuth::Negotiate}, true);
    add(&f_aws_key_, "Access key", &form_.adv.aws_access_key, "AKIA... or {{awsKey}}", {FolderAuth::AwsSigV4});
    add(&f_aws_secret_, "Secret key", &form_.adv.aws_secret_key, "", {FolderAuth::AwsSigV4}, true);
    add(&f_aws_region_, "Region", &form_.adv.aws_region, "eu-west-1", {FolderAuth::AwsSigV4});
    add(&f_aws_service_, "Service", &form_.adv.aws_service, "execute-api", {FolderAuth::AwsSigV4});
    add(&f_aws_token_, "Session token", &form_.adv.aws_session_token, "optional", {FolderAuth::AwsSigV4}, true);
    add(nullptr, "Grant", nullptr, "", {FolderAuth::OAuth2});  // the grant picker
    add(&f_o_token_url_, "Token URL", &o.token_url, "https://auth.example.com/oauth/token", {FolderAuth::OAuth2});
    add(&f_o_auth_url_, "Auth URL", &o.auth_url, "https://auth.example.com/authorize", {FolderAuth::OAuth2}, false, 2);
    add(&f_o_client_id_, "Client ID", &o.client_id, "{{clientId}}", {FolderAuth::OAuth2});
    add(&f_o_secret_, "Client secret", &o.client_secret, "{{clientSecret}}", {FolderAuth::OAuth2}, true);
    add(&f_o_scope_, "Scope", &o.scope, "read write", {FolderAuth::OAuth2});
    add(&f_o_user_, "Username", &o.username, "", {FolderAuth::OAuth2}, false, 1);
    add(&f_o_pass_, "Password", &o.password, "", {FolderAuth::OAuth2}, true, 1);
    top->Add(grid_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);
    auth_note_ = new wxStaticText(this, wxID_ANY, "");
    auth_note_->SetForegroundColour(t.dim);
    top->Add(auth_note_, 0, wxLEFT | wxRIGHT | wxTOP, pad);

    // Headers / Variables / Assertions / Pre-request / Post-response, each built when first shown.
    tabs_ = new TabStrip(this);
    tabs_->set_tabs({"Headers", "Variables", "Assertions", "Pre-request script", "Post-response script",
                     "Certificates & proxy"});
    top->Add(tabs_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);
    book_ = new wxSimplebook(this);
    book_->SetBackgroundColour(t.bg);
    for (size_t i = 0; i < kPages; ++i) {
      auto* holder = new wxPanel(book_);
      holder->SetBackgroundColour(t.bg);
      holder->SetSizer(new wxBoxSizer(wxVERTICAL));
      book_->AddPage(holder, "");
    }
    tabs_->on_select = [this](size_t i) {
      ensure_page(i);
      book_->ChangeSelection(i);
      if (wxWindow* f = page_focus(i)) f->SetFocus();
    };
    top->Add(book_, 1, wxEXPAND | wxLEFT | wxRIGHT, pad);

    auto* btns = new wxBoxSizer(wxHORIZONTAL);
    auto* where = new wxStaticText(this, wxID_ANY, collection ? wxString("Saved as folder.json at the workspace root")
                                                              : "Saved as " + label + "/folder.json");
    where->SetForegroundColour(t.dim);
    btns->Add(where, 1, wxALIGN_CENTER_VERTICAL);
    auto* cancel = new FlatButton(this, "Cancel", FlatButton::Style::Subtle);
    cancel->on_click = [this] { EndModal(wxID_CANCEL); };
    auto* save = new FlatButton(this, "Save", FlatButton::Style::Primary);
    save->SetToolTip("Save (Ctrl+S or Ctrl+Enter)");
    save->on_click = [this] { finish(); };
    btns->Add(cancel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    btns->Add(save, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(btns, 0, wxEXPAND | wxALL, pad);
    SetSizer(top);
    SetEscapeId(wxID_CANCEL);
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
      const int k = e.GetKeyCode();
      if (e.ControlDown() && (k == 'S' || k == WXK_RETURN || k == WXK_NUMPAD_ENTER)) {
        finish();
        return;
      }
      e.Skip();
    });
    // The first page with something in it (Headers when none has).
    size_t first = 0;
    if (form_.headers.empty()) {
      if (!settings_.vars.empty()) first = 1;
      else if (!settings_.asserts.empty()) first = 2;
      else if (!settings_.pre_script.empty()) first = 3;
      else if (!settings_.post_script.empty()) first = 4;
      else if (!settings_.network.empty()) first = 5;
    }
    ensure_page(first);
    tabs_->select(first);
    book_->ChangeSelection(first);
    badges();
    show_fields();
    SetMinSize(FromDIP(wxSize(760, 600)));
    SetSize(FromDIP(wxSize(760, 600)));
    CentreOnParent();
  }

  // The settings as edited (valid after ShowModal() == wxID_OK).
  const FolderSettings& settings() const { return settings_; }
  const std::string& passphrase() const { return passphrase_; }

 private:
  static constexpr size_t kPages = 6;
  struct Row {
    wxTextCtrl** out;  // nullptr: the OAuth 2.0 grant picker
    const char* label;
    std::string* value;
    const char* hint;
    std::vector<FolderAuth> modes;
    bool secret = false;
    int grant = -1;  // OAuth 2.0: only for this grant (1 password, 2 authorization code)
    wxWindow* label_w = nullptr;
    wxWindow* field = nullptr;  // created when its auth type is first picked
  };

  void add(wxTextCtrl** out, const char* label, std::string* value, const char* hint, std::vector<FolderAuth> modes,
           bool secret = false, int grant = -1) {
    rows_.push_back({out, label, value, hint, std::move(modes), secret, grant});
  }

  // Creates a row's label and field, at its place in the grid (rows are created in any order).
  void create_row(size_t index) {
    const Theme& t = Theme::get();
    Row& r = rows_[index];
    size_t at = 0;
    for (size_t i = 0; i < index; ++i)
      if (rows_[i].field) at += 2;
    auto* l = new wxStaticText(this, wxID_ANY, r.label);
    l->SetForegroundColour(t.fg);
    wxWindow* field = nullptr;
    if (!r.out) {
      grant_ = new ChoiceButton(this, {"Client credentials", "Password", "Authorization code"});
      grant_->set_selection(static_cast<int>(form_.adv.oauth2.grant));
      grant_->on_select = [this](int i) {
        form_.adv.oauth2.grant = static_cast<OAuth2Config::Grant>(std::clamp(i, 0, 2));
        show_fields();
      };
      field = grant_;
    } else {
      auto* f = new wxTextCtrl(this, wxID_ANY, wxString::FromUTF8(*r.value), wxDefaultPosition, wxDefaultSize,
                               wxBORDER_SIMPLE | (r.secret ? wxTE_PASSWORD : 0));
      f->SetBackgroundColour(t.field);
      f->SetForegroundColour(t.fg);
      f->SetFont(mono(9));
      if (*r.hint) f->SetHint(r.hint);
      f->SetMinSize(wxSize(FromDIP(420), -1));
      *r.out = f;
      field = f;
    }
    grid_->Insert(at, l, 0, wxALIGN_CENTER_VERTICAL);
    grid_->Insert(at + 1, field, r.out ? 1 : 0, r.out ? wxEXPAND : 0);
    r.label_w = l;
    r.field = field;
  }

  void ensure_page(size_t i) {
    if (i >= kPages || built_[i]) return;
    built_[i] = true;
    auto* holder = static_cast<wxPanel*>(book_->GetPage(i));
    wxWindowUpdateLocker lock(holder);
    wxWindow* page = nullptr;
    auto table = [&](KvGrid::Kind kind, const wxString& title, const std::vector<KvRow>& rows) {
      auto* p = new KvPanel(holder, kind, title);
      KvGrid* g = p->grid();
      g->lookup = [this, g](const std::string& name) -> std::optional<std::string> {
        // This folder's own variables (as edited here) first, then the environment / globals.
        if (vars_p_ && vars_p_->grid() != g)
          for (const auto& r : vars_p_->grid()->rows())
            if (r.enabled && trim(r.key) == name) return r.value;
        return vars_src_.lookup ? vars_src_.lookup(name) : std::nullopt;
      };
      g->var_names = [this] {
        std::vector<std::string> n = vars_src_.names ? vars_src_.names() : std::vector<std::string>();
        if (vars_p_)
          for (const auto& r : vars_p_->grid()->rows())
            if (!trim(r.key).empty()) n.emplace_back(trim(r.key));
        return n;
      };
      g->set_rows(rows);
      g->on_change = [this] { badges(); };
      return p;
    };
    switch (i) {
      case 0: headers_p_ = table(KvGrid::Kind::Headers, "Sent with every request below (a request's own header of the same name wins)", to_kv(form_.headers)); page = headers_p_; break;
      case 1: vars_p_ = table(KvGrid::Kind::Params, "Folder {{variables}}: above the environment's, below runtime ones", to_kv(settings_.vars)); page = vars_p_; break;
      case 2: {
        std::vector<KvRow> rows;
        for (const auto& a : settings_.asserts)
          rows.push_back({a.target, a.value, a.enabled, false, false, {}, a.op.empty() ? "eq" : a.op});
        asserts_p_ = table(KvGrid::Kind::Assert, "Checked on every response below, next to each request's own assertions", rows);
        page = asserts_p_;
        break;
      }
      case 3: pre_ = script_page(holder, "// JavaScript run before each request below (pm. / bru. APIs)", settings_.pre_script); page = pre_; break;
      case 4: post_ = script_page(holder, "// JavaScript run after each response below (pm.test, bru.setVar, ...)", settings_.post_script); page = post_; break;
      case 5:
        net_ = new NetworkPanel(holder, root_, collection_);
        net_->set(settings_.network, passphrase_);
        net_->on_change = [this] { badges(); };
        page = net_;
        break;
      default: break;
    }
    holder->GetSizer()->Add(page, 1, wxEXPAND | wxTOP, FromDIP(6));
    holder->Layout();
  }

  wxWindow* page_focus(size_t i) const {
    switch (i) {
      case 0: return headers_p_ ? static_cast<wxWindow*>(headers_p_->grid()) : nullptr;
      case 1: return vars_p_ ? static_cast<wxWindow*>(vars_p_->grid()) : nullptr;
      case 2: return asserts_p_ ? static_cast<wxWindow*>(asserts_p_->grid()) : nullptr;
      case 3: return pre_;
      case 4: return post_;
      case 5: return net_;
      default: return nullptr;
    }
  }

  wxTextCtrl* script_page(wxWindow* parent, const char* hint, const std::string& text) {
    const Theme& t = Theme::get();
    auto* f = new wxTextCtrl(parent, wxID_ANY, wxString::FromUTF8(text), wxDefaultPosition, wxDefaultSize,
                             wxBORDER_SIMPLE | wxTE_MULTILINE | wxTE_DONTWRAP | wxTE_PROCESS_TAB);
    f->SetBackgroundColour(t.field);
    f->SetForegroundColour(t.fg);
    f->SetFont(mono(9));
    f->SetHint(hint);
    f->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { badges(); });
    return f;
  }

  void badges() {
    auto num = [](size_t n) { return n ? wxString::Format("%zu", n) : wxString(); };
    auto enabled = [](const auto& rows) {
      size_t n = 0;
      for (const auto& r : rows) n += r.enabled ? 1 : 0;
      return n;
    };
    tabs_->set_badge(0, num(headers_p_ ? headers_p_->grid()->enabled_count() : enabled(form_.headers)));
    tabs_->set_badge(1, num(vars_p_ ? vars_p_->grid()->enabled_count() : enabled(settings_.vars)));
    tabs_->set_badge(2, num(asserts_p_ ? asserts_p_->grid()->enabled_count() : enabled(settings_.asserts)));
    const bool pre = pre_ ? !pre_->IsEmpty() : !settings_.pre_script.empty();
    const bool post = post_ ? !post_->IsEmpty() : !settings_.post_script.empty();
    tabs_->set_badge(3, pre ? wxString::FromUTF8("\xE2\x97\x8F") : wxString());
    tabs_->set_badge(4, post ? wxString::FromUTF8("\xE2\x97\x8F") : wxString());
    const bool net = net_ ? !net_->get().empty() : !settings_.network.empty();
    tabs_->set_badge(5, net ? wxString::FromUTF8("\xE2\x97\x8F") : wxString());
  }

  void show_fields() {
    Freeze();
    for (size_t i = 0; i < rows_.size(); ++i) {
      const Row& r = rows_[i];
      bool on = std::find(r.modes.begin(), r.modes.end(), form_.auth) != r.modes.end();
      if (on && r.grant >= 0) on = static_cast<int>(form_.adv.oauth2.grant) == r.grant;
      if (on && !r.field) create_row(i);
      if (rows_[i].field) {
        rows_[i].label_w->Show(on);
        rows_[i].field->Show(on);
      }
    }
    const char* note = "";
    switch (form_.auth) {
      case FolderAuth::Inherit: note = "Requests below use the parent folder's or the collection's auth."; break;
      case FolderAuth::None: note = "Requests below that inherit send no authorization."; break;
      case FolderAuth::Bearer: note = "Sent as  Authorization: Bearer <token>  with every request below that sets no Authorization header."; break;
      case FolderAuth::ApiKey: note = "Sent as this header with every request below that does not set it itself."; break;
      case FolderAuth::Basic: note = "Sent as  Authorization: Basic base64(user:password)."; break;
      case FolderAuth::OAuth2: note = "Tokens are fetched when a request below is sent (an authorization code grant: Get new token on a request first)."; break;
      default: break;
    }
    auth_note_->SetLabel(note);
    auth_note_->Wrap(FromDIP(680));
    Thaw();
    Layout();
  }

  void finish() {
    // A field never shown keeps the value it was read with.
    auto val = [](wxTextCtrl* f, const std::string& keep) { return f ? f->GetValue().utf8_string() : keep; };
    form_.token = val(f_token_, form_.token);
    form_.user = val(f_user_, form_.user);
    form_.pass = val(f_pass_, form_.pass);
    form_.key_name = val(f_key_name_, form_.key_name);
    form_.key_value = val(f_key_value_, form_.key_value);
    HttpAuth& a = form_.adv;
    a.user = val(f_s_user_, a.user);
    a.password = val(f_s_pass_, a.password);
    a.aws_access_key = val(f_aws_key_, a.aws_access_key);
    a.aws_secret_key = val(f_aws_secret_, a.aws_secret_key);
    a.aws_region = val(f_aws_region_, a.aws_region);
    a.aws_service = val(f_aws_service_, a.aws_service);
    a.aws_session_token = val(f_aws_token_, a.aws_session_token);
    OAuth2Config& o = a.oauth2;
    o.token_url = val(f_o_token_url_, o.token_url);
    o.auth_url = val(f_o_auth_url_, o.auth_url);
    o.client_id = val(f_o_client_id_, o.client_id);
    o.client_secret = val(f_o_secret_, o.client_secret);
    o.scope = val(f_o_scope_, o.scope);
    o.username = val(f_o_user_, o.username);
    o.password = val(f_o_pass_, o.password);
    if (form_.auth == FolderAuth::OAuth2 && trim(o.token_url).empty()) {
      auth_note_->SetLabel("OAuth 2.0 needs a token URL.");
      auth_note_->SetForegroundColour(Theme::get().js_error);
      if (f_o_token_url_) f_o_token_url_->SetFocus();
      Layout();
      return;
    }
    if (headers_p_) form_.headers = from_kv(headers_p_->grid()->rows());
    apply_folder_form(form_, settings_);
    if (vars_p_) {
      std::vector<SettingsRow> vars;
      for (auto& r : from_kv(vars_p_->grid()->rows()))
        if (r.name.find("{{") == std::string::npos && r.name.find("}}") == std::string::npos) vars.push_back(std::move(r));
      settings_.vars = std::move(vars);
    }
    if (asserts_p_) {
      settings_.asserts.clear();
      for (const auto& r : asserts_p_->grid()->rows()) {
        Assertion as;
        as.enabled = r.enabled;
        as.target = std::string(trim(r.key));
        if (as.target.empty()) continue;
        as.op = r.type.empty() ? "eq" : r.type;
        as.value = assert_op_unary(as.op) ? std::string() : r.value;
        settings_.asserts.push_back(std::move(as));
      }
    }
    if (pre_) settings_.pre_script = pre_->GetValue().utf8_string();
    if (post_) settings_.post_script = post_->GetValue().utf8_string();
    if (net_) {
      settings_.network = net_->get();
      passphrase_ = settings_.network.cert_file.empty() ? std::string() : net_->passphrase();
    }
    EndModal(wxID_OK);
  }

  std::filesystem::path root_;
  std::string passphrase_;
  bool collection_ = false;
  NetworkPanel* net_ = nullptr;
  FolderSettings settings_;
  FolderForm form_;
  VarSource vars_src_;
  ChoiceButton* type_ = nullptr;
  ChoiceButton* grant_ = nullptr;
  wxFlexGridSizer* grid_ = nullptr;
  wxStaticText* auth_note_ = nullptr;
  std::vector<Row> rows_;
  wxTextCtrl *f_token_ = nullptr, *f_user_ = nullptr, *f_pass_ = nullptr, *f_key_name_ = nullptr, *f_key_value_ = nullptr,
             *f_s_user_ = nullptr, *f_s_pass_ = nullptr, *f_aws_key_ = nullptr, *f_aws_secret_ = nullptr,
             *f_aws_region_ = nullptr, *f_aws_service_ = nullptr, *f_aws_token_ = nullptr, *f_o_token_url_ = nullptr,
             *f_o_auth_url_ = nullptr, *f_o_client_id_ = nullptr, *f_o_secret_ = nullptr, *f_o_scope_ = nullptr,
             *f_o_user_ = nullptr, *f_o_pass_ = nullptr;
  TabStrip* tabs_ = nullptr;
  wxSimplebook* book_ = nullptr;
  bool built_[kPages] = {};
  KvPanel *headers_p_ = nullptr, *vars_p_ = nullptr, *asserts_p_ = nullptr;
  wxTextCtrl *pre_ = nullptr, *post_ = nullptr;
};

}  // namespace

void edit_folder_settings(wxWindow* parent, const Workspace& ws, const fs::path& folder, std::function<void()> on_saved) {
  auto wsc = std::make_shared<Workspace>(ws);
  wxWeakRef<wxWindow> owner(parent);
  std::thread([wsc, folder, owner, on_saved = std::move(on_saved)]() mutable {
    std::string err;
    std::optional<FolderSettings> s = wsc->folder_settings(folder, &err);
    // The client certificate passphrase of this level (sealed in .watt/secrets.json).
    const std::string label = folder_label(*wsc, folder);
    const std::string pass_key = label == "Collection" ? std::string() : label;
    std::string pass = s ? wsc->cert_passphrase(pass_key) : std::string();
    // The variables the tables resolve (globals, the active environment, runtime values, in
    // increasing priority), gathered here: Workspace::expand may read files, never on the UI thread.
    auto known = std::make_shared<std::map<std::string, std::string>>();
    for (const auto& [k, v] : wsc->globals) (*known)[k] = v;
    for (const auto& env : wsc->environments())
      if (env.name == wsc->active_env)
        for (const auto& [k, v] : env.vars) (*known)[k] = v;
    for (const auto& [k, v] : wsc->runtime_vars) (*known)[k] = v;
    wxTheApp->CallAfter([wsc, folder, owner, s = std::move(s), err, known, pass_key, pass = std::move(pass),
                         on_saved = std::move(on_saved)]() mutable {
      if (!owner) return;
      if (!s) {
        wxMessageBox(wxString::FromUTF8("Cannot open the folder settings: " + err), "watt", wxOK | wxICON_WARNING,
                     owner.get());
        return;
      }
      const bool collection = folder_label(*wsc, folder) == "Collection";
      VarSource vars;
      vars.lookup = [known](const std::string& name) -> std::optional<std::string> {
        const auto it = known->find(name);
        if (it == known->end()) return std::nullopt;
        return it->second;
      };
      vars.names = [known] {
        std::vector<std::string> n;
        for (const auto& [k, v] : *known) n.push_back(k);
        return n;
      };
      FolderDialog dlg(wxGetTopLevelParent(owner.get()), wxString::FromUTF8(folder_label(*wsc, folder)), collection, *s,
                       std::move(vars), wsc->root(), pass);
      if (dlg.ShowModal() != wxID_OK || (dlg.settings() == *s && dlg.passphrase() == pass)) return;
      FolderSettings out = dlg.settings();
      std::optional<std::string> new_pass;
      if (dlg.passphrase() != pass) new_pass = dlg.passphrase();
      std::thread([wsc, folder, owner, out = std::move(out), pass_key, new_pass = std::move(new_pass),
                   on_saved = std::move(on_saved)]() mutable {
        std::string e;
        bool ok = wsc->save_folder_settings(folder, out, &e);
        if (ok && new_pass) ok = wsc->set_cert_passphrase(pass_key, *new_pass, &e);
        wxTheApp->CallAfter([owner, ok, e, on_saved = std::move(on_saved)] {
          if (!owner) return;
          if (!ok) {
            wxMessageBox(wxString::FromUTF8("The folder settings were not saved: " + e), "watt",
                         wxOK | wxICON_WARNING, owner.get());
            return;
          }
          if (on_saved) on_saved();
        });
      }).detach();
    });
  }).detach();
}

}  // namespace watt::app::http_editor
