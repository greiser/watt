#include "app/sidebar_dialogs.h"

#include <wx/app.h>
#include <wx/button.h>
#include <wx/clipbrd.h>
#include <wx/dialog.h>
#include <wx/radiobut.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <thread>

#include <wx/msw/wrapwin.h>
#include <shobjidl.h>

#include "app/sidebar_style.h"

namespace watt::app::sidebar {

namespace {

wxButton* make_button(wxWindow* parent, wxWindowID id, const wxString& label, bool primary, bool danger) {
  auto* b = new wxButton(parent, id, label, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
  if (primary) {
    b->SetBackgroundColour(danger ? wxColour(0xC4, 0x32, 0x2B) : wxColour(0x1F, 0x6F, 0xEB));
    b->SetForegroundColour(*wxWHITE);
    wxFont f = b->GetFont();
    f.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    b->SetFont(f);
  } else {
    b->SetBackgroundColour(mix(window_colour(), text_colour(), 0.12));
    b->SetForegroundColour(text_colour());
  }
  const wxSize best = b->GetBestSize();
  b->SetMinSize(wxSize(std::max(best.x + parent->FromDIP(24), parent->FromDIP(88)), parent->FromDIP(30)));
  return b;
}

class Base : public wxDialog {
 public:
  Base(wxWindow* parent, const wxString& caption)
      : wxDialog(parent, wxID_ANY, caption, wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX) {
    SetBackgroundColour(mix(window_colour(), text_colour(), 0.03));
    SetForegroundColour(text_colour());
    body_ = new wxBoxSizer(wxVERTICAL);
    buttons_ = new wxBoxSizer(wxHORIZONTAL);
    buttons_->AddStretchSpacer();
    auto* sz = new wxBoxSizer(wxVERTICAL);
    sz->Add(body_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(18));
    sz->Add(buttons_, 0, wxEXPAND | wxALL, FromDIP(14));
    SetSizer(sz);
  }

  void add_button(wxWindowID id, const wxString& label, bool primary, bool danger = false) {
    wxButton* b = make_button(this, id, label, primary, danger);
    buttons_->Add(b, 0, wxLEFT, FromDIP(8));
    if (primary) {
      // The primary button is the default (Enter) and also gets the focus, so Enter and the
      // look agree: Delete, then Enter, deletes.
      b->SetDefault();
      SetAffirmativeId(id);
      primary_ = b;
    }
    b->Bind(wxEVT_BUTTON, [this, id](wxCommandEvent&) { EndModal(id); });
  }

  void finish(int min_width) {
    GetSizer()->SetMinSize(wxSize(FromDIP(min_width), -1));
    Fit();
    CentreOnParent();
    if (primary_ && !focus_bound_) {
      focus_bound_ = true;
      Bind(wxEVT_INIT_DIALOG, [this](wxInitDialogEvent& e) {
        e.Skip();
        CallAfter([this] {
          if (primary_ && !has_input_focus()) primary_->SetFocus();
        });
      });
    }
  }

  // A text field (curl paste) keeps the focus; plain confirmations focus the primary button.
  bool has_input_focus() const {
    const wxWindow* f = wxWindow::FindFocus();
    return f && f != primary_ && dynamic_cast<const wxTextCtrl*>(f) != nullptr;
  }

  wxButton* primary_ = nullptr;
  bool focus_bound_ = false;

  wxBoxSizer* body_;
  wxBoxSizer* buttons_;
};

wxStaticText* text(wxWindow* parent, const wxString& s, int wrap) {
  auto* t = new wxStaticText(parent, wxID_ANY, s);
  t->SetForegroundColour(text_colour());
  t->Wrap(wrap);
  return t;
}

}  // namespace

bool confirm(wxWindow* parent, const wxString& title, const wxString& message, const wxString& action, bool danger) {
  Base dlg(parent, "watt");
  auto* head = text(&dlg, title, dlg.FromDIP(400));
  wxFont f = head->GetFont();
  f.SetWeight(wxFONTWEIGHT_BOLD);
  f.SetFractionalPointSize(f.GetFractionalPointSize() * 1.1);
  head->SetFont(f);
  dlg.body_->Add(head, 0, wxBOTTOM, dlg.FromDIP(8));
  auto* msg = text(&dlg, message, dlg.FromDIP(400));
  msg->SetForegroundColour(mix(window_colour(), text_colour(), 0.8));
  dlg.body_->Add(msg, 0, wxEXPAND);
  dlg.add_button(wxID_CANCEL, "Cancel", false);
  dlg.add_button(wxID_OK, action, true, danger);
  dlg.SetEscapeId(wxID_CANCEL);
  dlg.finish(420);
  return dlg.ShowModal() == wxID_OK;
}

void notify(wxWindow* parent, const wxString& title, const wxString& message) {
  Base dlg(parent, title);
  dlg.body_->Add(text(&dlg, message, dlg.FromDIP(400)), 0, wxEXPAND);
  dlg.add_button(wxID_OK, "OK", true);
  dlg.SetEscapeId(wxID_OK);
  dlg.finish(380);
  dlg.ShowModal();
}

bool ask_curl(wxWindow* parent, std::string* command, std::string* name) {
  Base dlg(parent, "Import cURL");
  dlg.body_->Add(text(&dlg, "Paste a curl command (bash, cmd or PowerShell). It is saved as a request here.", dlg.FromDIP(520)), 0,
                 wxBOTTOM, dlg.FromDIP(8));
  wxString initial;
  if (wxTheClipboard->Open()) {
    if (wxTheClipboard->IsSupported(wxDF_UNICODETEXT)) {
      wxTextDataObject d;
      wxTheClipboard->GetData(d);
      const wxString s = d.GetText().Strip(wxString::both);
      if (s.Lower().StartsWith("curl")) initial = s;
    }
    wxTheClipboard->Close();
  }
  auto* cmd = new wxTextCtrl(&dlg, wxID_ANY, initial, wxDefaultPosition, dlg.FromDIP(wxSize(540, 150)),
                             wxTE_MULTILINE | wxTE_BESTWRAP | wxBORDER_SIMPLE);
  cmd->SetBackgroundColour(window_colour());
  cmd->SetForegroundColour(text_colour());
  wxFont mono(wxFontInfo(9).Family(wxFONTFAMILY_TELETYPE).FaceName("Consolas"));
  cmd->SetFont(mono);
  dlg.body_->Add(cmd, 1, wxEXPAND | wxBOTTOM, dlg.FromDIP(10));
  dlg.body_->Add(text(&dlg, "Name (optional)", 400), 0, wxBOTTOM, dlg.FromDIP(4));
  auto* nm = new wxTextCtrl(&dlg, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
  nm->SetBackgroundColour(window_colour());
  nm->SetForegroundColour(text_colour());
  nm->SetHint("e.g. Create order (default: the last path segment)");
  dlg.body_->Add(nm, 0, wxEXPAND);
  dlg.add_button(wxID_CANCEL, "Cancel", false);
  dlg.add_button(wxID_OK, "Import", true);
  dlg.SetEscapeId(wxID_CANCEL);
  dlg.finish(560);
  cmd->SetFocus();
  cmd->SetInsertionPointEnd();
  if (dlg.ShowModal() != wxID_OK) return false;
  *command = cmd->GetValue().utf8_string();
  *name = nm->GetValue().Strip(wxString::both).utf8_string();
  return true;
}

bool ask_import_text(wxWindow* parent, std::string* out) {
  Base dlg(parent, "Import from text or URL");
  dlg.body_->Add(text(&dlg,
                      "Paste a Postman, Insomnia, OpenAPI (JSON or YAML) or HAR document, a curl command, "
                      "or an http(s) URL to fetch one from. Ctrl+Enter imports.",
                      dlg.FromDIP(520)),
                 0, wxBOTTOM, dlg.FromDIP(8));
  wxString initial;
  if (wxTheClipboard->Open()) {
    if (wxTheClipboard->IsSupported(wxDF_UNICODETEXT)) {
      wxTextDataObject d;
      wxTheClipboard->GetData(d);
      const wxString s = d.GetText().Strip(wxString::both);
      const wxString lo = s.Left(16).Lower();
      const bool looks = s.StartsWith("{") || s.StartsWith("[") || lo.StartsWith("curl") ||
                         lo.StartsWith("openapi:") || lo.StartsWith("swagger:") ||
                         ((lo.StartsWith("http://") || lo.StartsWith("https://")) && s.Find('\n') == wxNOT_FOUND);
      if (looks && s.length() <= (1u << 20)) initial = s;
    }
    wxTheClipboard->Close();
  }
  auto* box = new wxTextCtrl(&dlg, wxID_ANY, initial, wxDefaultPosition, dlg.FromDIP(wxSize(540, 220)),
                             wxTE_MULTILINE | wxTE_RICH2 | wxTE_DONTWRAP | wxBORDER_SIMPLE);
  box->SetBackgroundColour(window_colour());
  box->SetForegroundColour(text_colour());
  wxFont mono(wxFontInfo(9).Family(wxFONTFAMILY_TELETYPE).FaceName("Consolas"));
  box->SetFont(mono);
  box->SetHint("https://example.com/openapi.yaml");
  box->Bind(wxEVT_CHAR_HOOK, [&dlg](wxKeyEvent& e) {
    if (e.GetKeyCode() == WXK_RETURN && e.ControlDown()) dlg.EndModal(wxID_OK);
    else e.Skip();
  });
  dlg.body_->Add(box, 1, wxEXPAND);
  dlg.add_button(wxID_CANCEL, "Cancel", false);
  dlg.add_button(wxID_OK, "Import", true);
  dlg.SetEscapeId(wxID_CANCEL);
  dlg.finish(560);
  box->SetFocus();
  box->SetSelection(-1, -1);
  if (dlg.ShowModal() != wxID_OK) return false;
  *out = box->GetValue().Strip(wxString::both).utf8_string();
  return !out->empty();
}

int folder_settings(wxWindow* parent, FolderSettings& s) {
  Base dlg(parent, "Folder settings");
  auto label = [&](const wxString& t) {
    auto* l = text(&dlg, t, dlg.FromDIP(440));
    l->SetForegroundColour(mix(window_colour(), text_colour(), 0.7));
    dlg.body_->Add(l, 0, wxBOTTOM, dlg.FromDIP(4));
  };
  label("Name");
  auto* nm = new wxTextCtrl(&dlg, wxID_ANY, wxString::FromUTF8(s.name), wxDefaultPosition, wxDefaultSize,
                            wxBORDER_SIMPLE);
  nm->SetBackgroundColour(window_colour());
  nm->SetForegroundColour(text_colour());
  dlg.body_->Add(nm, 0, wxEXPAND | wxBOTTOM, dlg.FromDIP(12));
  label("Order of the requests and folders in it");
  auto* dragged = new wxRadioButton(&dlg, wxID_ANY, "As dragged in the sidebar", wxDefaultPosition, wxDefaultSize,
                                    wxRB_GROUP);
  auto* az = new wxRadioButton(&dlg, wxID_ANY, "A-Z (folders first, 2 before 10)");
  for (auto* r : {dragged, az}) r->SetForegroundColour(text_colour());
  dragged->SetValue(s.dragged_order);
  az->SetValue(!s.dragged_order);
  dragged->Enable(s.has_dragged_order);
  if (!s.has_dragged_order) dragged->SetToolTip("Drag a request onto another one in this folder to order it by hand.");
  dlg.body_->Add(dragged, 0, wxBOTTOM, dlg.FromDIP(2));
  dlg.body_->Add(az, 0, wxBOTTOM, dlg.FromDIP(12));
  label("Contents");
  dlg.body_->Add(text(&dlg, s.contents, dlg.FromDIP(440)), 0, wxBOTTOM, dlg.FromDIP(12));
  label("Location");
  auto* path = new wxTextCtrl(&dlg, wxID_ANY, s.path, wxDefaultPosition, wxDefaultSize, wxTE_READONLY | wxBORDER_NONE);
  path->SetBackgroundColour(dlg.GetBackgroundColour());
  path->SetForegroundColour(text_colour());
  dlg.body_->Add(path, 0, wxEXPAND);
  if (s.can_run) {
    wxButton* run = make_button(&dlg, kFolderRun, "Run folder", false, false);
    dlg.buttons_->Insert(0, run, 0, wxRIGHT, dlg.FromDIP(8));
    run->Bind(wxEVT_BUTTON, [&dlg](wxCommandEvent&) { dlg.EndModal(kFolderRun); });
  }
  dlg.add_button(wxID_CANCEL, "Cancel", false);
  dlg.add_button(wxID_OK, "Save", true);
  dlg.SetEscapeId(wxID_CANCEL);
  dlg.finish(460);
  nm->SetFocus();
  nm->SelectAll();
  const int rc = dlg.ShowModal();
  if (rc == wxID_CANCEL) return rc;
  s.name = nm->GetValue().Strip(wxString::both).utf8_string();
  s.dragged_order = dragged->GetValue();
  return rc;
}

int define_vars(wxWindow* parent, const wxString& where, const std::vector<std::string>& names,
                std::vector<std::string>* values) {
  Base dlg(parent, "Define variables");
  auto* head = text(&dlg,
                    wxString::Format("%zu variable%s used by the imported requests %s not defined. The values are saved "
                                     "as variables of the folder %s (they apply under any environment; an "
                                     "environment value of the same name wins).",
                                     names.size(), names.size() == 1 ? "" : "s", names.size() == 1 ? "is" : "are", where),
                    dlg.FromDIP(460));
  head->SetForegroundColour(mix(window_colour(), text_colour(), 0.8));
  dlg.body_->Add(head, 0, wxBOTTOM, dlg.FromDIP(12));
  auto* grid = new wxFlexGridSizer(2, dlg.FromDIP(6), dlg.FromDIP(10));
  grid->AddGrowableCol(1, 1);
  std::vector<wxTextCtrl*> fields;
  wxFont mono(wxFontInfo(9).Family(wxFONTFAMILY_TELETYPE).FaceName("Consolas"));
  for (size_t i = 0; i < names.size() && i < 24; ++i) {
    auto* k = new wxStaticText(&dlg, wxID_ANY, wxString::FromUTF8(names[i]));
    k->SetFont(mono);
    k->SetForegroundColour(text_colour());
    grid->Add(k, 0, wxALIGN_CENTER_VERTICAL);
    auto* v = new wxTextCtrl(&dlg, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    v->SetBackgroundColour(window_colour());
    v->SetForegroundColour(text_colour());
    v->SetFont(mono);
    v->SetHint(names[i] == "base" || names[i].find("url") != std::string::npos || names[i].find("host") != std::string::npos
                   ? wxString("e.g. http://127.0.0.1:8080")
                   : wxString("value"));
    grid->Add(v, 1, wxEXPAND);
    fields.push_back(v);
  }
  dlg.body_->Add(grid, 0, wxEXPAND);
  if (names.size() > 24) dlg.body_->Add(text(&dlg, wxString::Format("and %zu more: in the folder settings", names.size() - 24), 400), 0, wxTOP, dlg.FromDIP(6));
  wxButton* more = make_button(&dlg, kDefineVarsSettings, wxString::FromUTF8("Folder settings\xE2\x80\xA6"), false, false);
  more->SetToolTip("Open every setting of the folder (headers, auth, variables, scripts) instead");
  dlg.buttons_->Insert(0, more, 0, wxRIGHT, dlg.FromDIP(8));
  more->Bind(wxEVT_BUTTON, [&dlg](wxCommandEvent&) { dlg.EndModal(kDefineVarsSettings); });
  dlg.add_button(wxID_CANCEL, "Cancel", false);
  dlg.add_button(wxID_OK, "Save", true);
  dlg.SetEscapeId(wxID_CANCEL);
  dlg.finish(480);
  if (!fields.empty()) fields.front()->SetFocus();
  const int rc = dlg.ShowModal();
  if (rc != wxID_OK) return rc;
  values->clear();
  for (size_t i = 0; i < names.size(); ++i)
    values->push_back(i < fields.size() ? fields[i]->GetValue().Strip(wxString::both).utf8_string() : std::string());
  return rc;
}

}  // namespace watt::app::sidebar

// ------------------------------------------------------------------ OS file dialog on a thread

namespace watt::app::sidebar {

namespace {

// IFileOpenDialog / IFileSaveDialog on a thread of their own (see pick_file_async).
void run_file_dialog(wxWindow* owner, std::wstring title, std::vector<std::pair<std::wstring, std::wstring>> filters,
                     bool folders, bool save, std::filesystem::path start, std::wstring name,
                     std::function<void(const std::filesystem::path&)> done) {
  const HWND hwnd = owner ? static_cast<HWND>(owner->GetHWND()) : nullptr;
  std::thread([hwnd, title = std::move(title), filters = std::move(filters), folders, save, start = std::move(start),
               name = std::move(name), done = std::move(done)]() mutable {
    std::filesystem::path chosen;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileDialog* dlg = nullptr;
    const HRESULT made = save ? CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(
                                                     reinterpret_cast<IFileSaveDialog**>(&dlg)))
                              : CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(
                                                     reinterpret_cast<IFileOpenDialog**>(&dlg)));
    if (SUCCEEDED(made) && dlg) {
      FILEOPENDIALOGOPTIONS opts = 0;
      dlg->GetOptions(&opts);
      opts |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
      if (save) opts |= FOS_OVERWRITEPROMPT;
      else opts |= folders ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST;
      dlg->SetOptions(opts);
      dlg->SetTitle(title.c_str());
      std::vector<COMDLG_FILTERSPEC> spec;
      for (const auto& [label, pattern] : filters) spec.push_back({label.c_str(), pattern.c_str()});
      if (!folders && !spec.empty()) dlg->SetFileTypes(static_cast<UINT>(spec.size()), spec.data());
      if (save) dlg->SetDefaultExtension(L"json");
      if (!name.empty()) dlg->SetFileName(name.c_str());
      if (!start.empty()) {
        IShellItem* at = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&at))) && at) {
          dlg->SetFolder(at);
          at->Release();
        }
      }
      if (SUCCEEDED(dlg->Show(hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
          PWSTR path = nullptr;
          if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
            chosen = path;
            CoTaskMemFree(path);
          }
          item->Release();
        }
      }
      dlg->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    if (wxAppConsole* app = wxTheApp)
      app->CallAfter([done = std::move(done), chosen = std::move(chosen)] { done(chosen); });
  }).detach();
}

}  // namespace

void pick_file_async(wxWindow* owner, const std::wstring& title,
                     const std::vector<std::pair<std::wstring, std::wstring>>& filters, bool folders,
                     std::function<void(const std::filesystem::path&)> done, const std::filesystem::path& start) {
  run_file_dialog(owner, title, filters, folders, false, start, {}, std::move(done));
}

void save_file_async(wxWindow* owner, const std::wstring& title,
                     const std::vector<std::pair<std::wstring, std::wstring>>& filters, const std::wstring& name,
                     std::function<void(const std::filesystem::path&)> done, const std::filesystem::path& start) {
  run_file_dialog(owner, title, filters, false, true, start, name, std::move(done));
}

}  // namespace watt::app::sidebar
