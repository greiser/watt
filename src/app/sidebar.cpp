// Sidebar: workspace switcher, environment switcher, filter box, the collection tree (folders,
// method / gRPC badges, drag-and-drop moves, rename, delete, new request / folder) and the
// history list; Ctrl+P quick open over every saved request.
//
// Performance: the folder scan (Workspace::tree) and the history listing run on worker threads
// with a copy of the Workspace (its badge index and history handles are shared and locked);
// results come back with CallAfter and are dropped if a newer scan started or the panel is gone.
// The tree is filled lazily: a folder's children are inserted when it is first expanded, so a
// rebuild costs only what is visible.
#include <wx/app.h>
#include <wx/aui/auibook.h>
#include <wx/aui/framemanager.h>
#include <wx/button.h>
#include <wx/clipbrd.h>
#include <wx/config.h>
#include <wx/dcclient.h>
#include <wx/graphics.h>
#include <wx/dirdlg.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/tokenzr.h>
#include <wx/toplevel.h>
#include <wx/treectrl.h>
#include <wx/utils.h>
#include <wx/msw/wrapwin.h>
#include <objbase.h>
#include <shellapi.h>
#include <uxtheme.h>
#pragma comment(lib, "uxtheme.lib")

#include <cctype>
#include <chrono>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <unordered_map>

#include "app/env_editor.h"
#include "app/panels.h"
#include "app/request_tab.h"
#include "app/sidebar_dialogs.h"
#include "app/sidebar_files.h"
#include "app/sidebar_fuzzy.h"
#include "app/sidebar_history.h"
#include "app/sidebar_names.h"
#include "app/sidebar_quickopen.h"
#include "app/sidebar_style.h"

namespace watt::app {
namespace {

namespace fs = std::filesystem;
using namespace sidebar;

constexpr char kReqExt[] = ".req.json";
constexpr size_t kMaxRecent = 8;

std::string u8(const fs::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}
fs::path from_u8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

bool is_under(const fs::path& p, const fs::path& folder) {
  const auto& a = p.native();
  const auto& b = folder.native();
  return a.size() > b.size() && a.compare(0, b.size(), b) == 0 && (a[b.size()] == L'\\' || a[b.size()] == L'/');
}

bool is_request_file(const fs::path& p) {
  const std::wstring n = p.filename().native();
  return n.size() > 9 && n.compare(n.size() - 9, 9, L".req.json") == 0;
}

// All request files under a node (or the node itself).
void collect_requests(const CollectionNode& n, std::vector<fs::path>& out) {
  if (!n.is_folder) {
    out.push_back(n.path);
    return;
  }
  for (const auto& c : n.children) collect_requests(c, out);
}

size_t count_requests(const CollectionNode& n) {
  if (!n.is_folder) return 1;
  size_t k = 0;
  for (const auto& c : n.children) k += count_requests(c);
  return k;
}

bool any_badge_missing(const CollectionNode& n) {
  if (!n.is_folder) return !n.badge;
  for (const auto& c : n.children)
    if (any_badge_missing(c)) return true;
  return false;
}

CollectionNode* find_node(CollectionNode& n, const fs::path& p) {
  if (n.path == p) return &n;
  if (!n.is_folder) return nullptr;
  for (auto& c : n.children)
    if (c.path == p || (c.is_folder && is_under(p, c.path))) return find_node(c, p);
  return nullptr;
}
const CollectionNode* find_node(const CollectionNode& n, const fs::path& p) {
  return find_node(const_cast<CollectionNode&>(n), p);
}

// Lower-case search text per node (folder path, name, method, URL / gRPC method, file name),
// built once per scan so a filter keystroke does no allocation or map lookup per request.
using SearchKeys = std::unordered_map<const CollectionNode*, std::string>;

void build_keys(const CollectionNode& n, const std::string& prefix, const MetaMap& meta, SearchKeys& out) {
  if (!n.is_folder) {
    std::string t = prefix + n.name + " " + (n.kind == RequestKind::Grpc ? std::string("grpc") : n.method);
    if (auto m = meta.find(n.path); m != meta.end()) t += " " + m->second.url;
    t += " " + u8(n.path.filename());
    out.emplace(&n, ascii_lower(t));
    return;
  }
  const std::string p = prefix.empty() && n.name.empty() ? "" : prefix + n.name + "/";
  if (!prefix.empty()) out.emplace(&n, ascii_lower(prefix + n.name));
  for (const auto& c : n.children) build_keys(c, p, meta, out);
}

// Every space-separated word of q (lower-case) is in t (lower-case).
bool words_in(std::string_view q, std::string_view t) {
  for (size_t p = 0; p < q.size();) {
    size_t e = q.find(' ', p);
    if (e == std::string_view::npos) e = q.size();
    if (e > p && t.find(q.substr(p, e - p)) == std::string_view::npos) return false;
    p = e + 1;
  }
  return true;
}

// Copy of n with only requests matching the filter words (and the folders that lead to them).
// A request matches on its folder path, name, method and URL (or gRPC method).
bool filter_node(const CollectionNode& n, const std::string& q, const SearchKeys& keys, CollectionNode& out) {
  auto key = [&](const CollectionNode& x) -> std::string_view {
    auto it = keys.find(&x);
    return it == keys.end() ? std::string_view() : std::string_view(it->second);
  };
  if (!n.is_folder) {
    if (!words_in(q, key(n))) return false;
    out = n;
    return true;
  }
  out.name = n.name;
  out.path = n.path;
  out.is_folder = true;
  for (const auto& c : n.children) {
    CollectionNode cc;
    if (filter_node(c, q, keys, cc)) out.children.push_back(std::move(cc));
  }
  const std::string_view k = key(n);
  return !out.children.empty() || (!k.empty() && words_in(q, k));
}

// Moves a file or folder to the Recycle Bin (undo with Restore there).
bool recycle(const fs::path& p) {
  std::wstring from = p.wstring();
  from.push_back(L'\0');  // double NUL terminated list
  SHFILEOPSTRUCTW op{};
  op.wFunc = FO_DELETE;
  op.pFrom = from.c_str();
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
  std::error_code ec;
  return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted && !fs::exists(p, ec);
}

// Per-workspace key for wxConfig entries.
wxString ws_key(const fs::path& root) {
  return wxString::Format("%016llx", static_cast<unsigned long long>(std::hash<std::wstring>{}(root.wstring())));
}

// "GET {{base}}/x" -> ("GET", "{{base}}/x"); "/pkg.Svc/M @ host" -> ("grpc", "/pkg.Svc/M").
std::string history_key(const HistoryEntry& e) {
  if (e.kind == RequestKind::Grpc) {
    const size_t at = e.summary.find(" @ ");
    return "grpc " + e.summary.substr(0, at);
  }
  const size_t sp = e.summary.find(' ');
  if (sp == std::string::npos) return e.summary;
  return ascii_lower(e.summary.substr(0, sp)) + " " + e.summary.substr(sp + 1);
}

std::string strip_scheme(std::string s) {
  for (const char* scheme : {"https://", "http://"})
    if (s.rfind(scheme, 0) == 0) return s.substr(std::char_traits<char>::length(scheme));
  return s;
}

// The collection tree. Its rename box is a native edit the tree creates itself, which neither
// wx's dark mode nor SetBackgroundColour reach: colour it here, where the tree (its parent)
// is asked for the edit's colours.
class CollectionTree : public wxTreeCtrl {
 public:
  using wxTreeCtrl::wxTreeCtrl;
  ~CollectionTree() override {
    if (brush_) DeleteObject(brush_);
  }

 protected:
  WXLRESULT MSWWindowProc(WXUINT msg, WXWPARAM wp, WXLPARAM lp) override {
    if (msg == WM_CTLCOLOREDIT && is_dark()) {
      const wxColour bg = mix(window_colour(), text_colour(), 0.10), fg = text_colour();
      if (!brush_ || bg != brush_colour_) {
        if (brush_) DeleteObject(brush_);
        brush_ = CreateSolidBrush(RGB(bg.Red(), bg.Green(), bg.Blue()));
        brush_colour_ = bg;
      }
      HDC dc = reinterpret_cast<HDC>(wp);
      SetTextColor(dc, RGB(fg.Red(), fg.Green(), fg.Blue()));
      SetBkColor(dc, RGB(bg.Red(), bg.Green(), bg.Blue()));
      return reinterpret_cast<WXLRESULT>(brush_);
    }
    return wxTreeCtrl::MSWWindowProc(msg, wp, lp);
  }

 private:
  HBRUSH brush_ = nullptr;
  wxColour brush_colour_;
};

struct ItemData : wxTreeItemData {
  explicit ItemData(const CollectionNode* n) : node(n) {}
  const CollectionNode* node;
  bool populated = false;
};

// Flat header button (label only, no border).
wxButton* flat_button(wxWindow* parent, const wxString& label, const wxString& tip) {
  auto* b = new wxButton(parent, wxID_ANY, label, wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT | wxBORDER_NONE);
  b->SetToolTip(tip);
  b->SetBackgroundColour(window_colour());
  b->SetForegroundColour(text_colour());
  return b;
}

wxStaticText* section_label(wxWindow* parent, const wxString& text) {
  auto* t = new wxStaticText(parent, wxID_ANY, text);
  wxFont f = t->GetFont();
  f.SetFractionalPointSize(f.GetFractionalPointSize() * 0.85);
  f.SetWeight(wxFONTWEIGHT_BOLD);
  t->SetFont(f);
  t->SetForegroundColour(dim_colour());
  return t;
}

class Sidebar : public wxPanel, public SidebarPanel {
 public:
  Sidebar(wxWindow* parent, SidebarHost* host) : wxPanel(parent, wxID_ANY), host_(host) {
    SetBackgroundColour(window_colour());
    build_ui();
    rebuild();
    update_env_choice();
    // Ctrl+P: quick open with fuzzy matching. The char hook runs before the frame's accelerator.
    if (wxWindow* tlw = wxGetTopLevelParent(this)) {
      tlw->Bind(wxEVT_CHAR_HOOK, [this, alive = std::weak_ptr<int>(alive_)](wxKeyEvent& e) {
        if (!alive.expired() && e.GetModifiers() == wxMOD_CONTROL && (e.GetKeyCode() == 'P' || e.GetKeyCode() == 'p')) {
          quick_open_dialog();
          return;
        }
        // Ctrl+B: hide / show the sidebar (as in VS Code, Bruno, Insomnia).
        if (!alive.expired() && e.GetModifiers() == wxMOD_CONTROL && (e.GetKeyCode() == 'B' || e.GetKeyCode() == 'b')) {
          set_collapsed(!collapsed_);
          return;
        }
        e.Skip();
      });
      tlw->Bind(wxEVT_CLOSE_WINDOW, [this, alive = std::weak_ptr<int>(alive_)](wxCloseEvent& e) {
        if (alive.expired()) e.Skip();
        else on_close_tlw(e);
      });
    }
    width_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
      if (wxConfigBase* c = wxConfigBase::Get()) {
        c->Write("Sidebar/Width", static_cast<long>(ToDIP(GetSize().x)));
        c->Flush();
      }
    });
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      width_changed();
    });
    // Once the frame has laid out its panes.
    CallAfter([this] {
      restore_width();
      width_restored_ = true;
    });
  }

  ~Sidebar() override { release_lock(); }

  wxWindow* window() override { return this; }

  void set_workspace(Workspace* ws) override {
    ws_ = ws;
    if (!ws_) {
      tree_root_ = CollectionNode{};
      search_keys_.clear();
      rebuild();
      return;
    }
    // Started without a folder argument: reopen the workspace used last time instead.
    if (first_ws_) {
      first_ws_ = false;
      const fs::path last = last_workspace();
      std::error_code ec;
      // Not when another watt window has it open (two windows saving the same files).
      if (wxTheApp && wxTheApp->argc <= 1 && !last.empty() && last != ws_->root() &&
          fs::exists(last / "watt.json", ec) && lock_owner(last) == 0) {
        switch_workspace(last, false);
        return;
      }
    }
    workspace_ready();
  }

  void refresh() override { rescan(); }

  void request_saved(const SavedRequest& r) override {
    if (r.file.empty()) return;
    if (promote_snapshot(r)) return;
    CollectionNode* n = find_node(tree_root_, r.file);
    if (!n) {
      rescan();  // a new file
      return;
    }
    const std::string url = r.kind == RequestKind::Grpc ? r.grpc_method : r.http.url;
    const std::string name = r.name.empty() ? n->name : r.name;
    for (CollectionNode* x : {n, find_node(view_root_, r.file)}) {
      if (!x) continue;
      x->badge = true;
      x->kind = r.kind;
      x->method = r.http.method;
      x->name = name;
    }
    if (name_cache_) name_cache_->note(r.file, r.name, url, r.id);
    meta_[r.file] = ReqMeta{r.name, url, r.id};
    meta_changed();
    if (wxTreeItemId id = item_for(r.file, false); id.IsOk()) {
      const int img = tree_image(r.kind, r.http.method);
      tree_->SetItemImage(id, img);
      tree_->SetItemImage(id, img, wxTreeItemIcon_Selected);
      if (tree_->GetItemText(id) != wxString::FromUTF8(name)) tree_->SetItemText(id, wxString::FromUTF8(name));
    }
  }

  void history_added(const HistoryEntry& e) override {
    // A tab of another workspace (kept hidden since a switch) finished a request: it was
    // appended to that workspace's history, not this one.
    if (ws_ && !e.request.file.empty() && !is_under(e.request.file, ws_->root())) return;
    history_->prepend(e);
    update_counts();
    update_history_pane();
  }

  void select_file(const fs::path& file_in) override {
    fs::path file = file_in;
    if (is_snapshot(file)) {  // a history snapshot: its saved request, if any
      auto it = snapshot_origin_.find(file);
      file = it == snapshot_origin_.end() ? fs::path() : it->second;
    }
    active_file_ = file;
    if (file.empty()) {
      // An unsaved tab is active: no request row is its own (a folder selection stays, it is
      // where "+" creates things).
      if (const auto* d = data(tree_->GetSelection()); d && !d->node->is_folder) {
        syncing_ = true;
        tree_->Unselect();
        syncing_ = false;
      }
      return;
    }
    touch_recent(file);
    syncing_ = true;
    wxTreeItemId id = item_for(file, true);
    if (id.IsOk() && tree_->GetSelection() != id) {
      tree_->SelectItem(id);
      centre_item(id);
    } else if (!id.IsOk() && !filter_q_.empty()) {
      // Hidden by the filter (Ctrl+P opened it): no other row may stay highlighted.
      if (const auto* d = data(tree_->GetSelection()); d && !d->node->is_folder) tree_->Unselect();
    }
    syncing_ = false;
    reveal_active_ = !id.IsOk();  // a file the tree does not show yet (first save): after the scan
  }

  void focus_filter() override {
    filter_->SetFocus();
    filter_->SelectAll();
  }

 private:
  // ---------------------------------------------------------------- UI

  void build_ui() {
    const int pad = FromDIP(8);
    auto* outer = new wxBoxSizer(wxHORIZONTAL);
    content_ = new wxPanel(this);
    content_->SetBackgroundColour(window_colour());
    rail_ = new wxPanel(this);
    rail_->SetBackgroundColour(window_colour());
    outer->Add(content_, 1, wxEXPAND);
    outer->Add(rail_, 1, wxEXPAND);
    // A visible divider on the edge the frame's sash drags (the sash itself is drawn flat).
    edge_ = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(1), -1));
    edge_->SetBackgroundColour(mix(window_colour(), text_colour(), 0.16));
    outer->Add(edge_, 0, wxEXPAND);
    SetSizer(outer);
    {
      auto* rs = new wxBoxSizer(wxVERTICAL);
      auto* show = flat_button(rail_, wxString::FromUTF8("\xC2\xBB"), "Show the sidebar (Ctrl+B)");
      wxFont f = show->GetFont();
      f.SetFractionalPointSize(f.GetFractionalPointSize() * 1.4);
      show->SetFont(f);
      show->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { set_collapsed(false); });
      rs->Add(show, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(6));
      // One icon per section (as in VS Code's activity bar): each opens the sidebar there.
      auto rail_btn = [&](const char* glyph, const wxString& tip, std::function<void()> fn) {
        auto* b = flat_button(rail_, wxString::FromUTF8(glyph), tip);
        wxFont bf = b->GetFont();
        bf.SetFractionalPointSize(bf.GetFractionalPointSize() * 1.25);
        b->SetFont(bf);
        b->SetForegroundColour(mix(window_colour(), text_colour(), 0.7));
        b->Bind(wxEVT_BUTTON, [this, fn](wxCommandEvent&) {
          set_collapsed(false);
          CallAfter(fn);
        });
        rs->Add(b, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(10));
      };
      rail_btn("\xF0\x9F\x97\x80", "Collection (Ctrl+B)", [this] { tree_->SetFocus(); });
      rail_btn("\xF0\x9F\x94\x8D", "Filter requests and history (Ctrl+Shift+F)", [this] { focus_filter(); });
      rail_btn("\xE2\x8F\xB1", "History", [this] {
        if (history_->IsShown()) history_->SetFocus();
      });
      rail_btn("\xE2\x9A\x99", "Environments", [this] { env_menu(); });
      rail_->SetSizer(rs);
      rail_->Hide();
    }
    wxWindow* const p = content_;
    auto* sz = new wxBoxSizer(wxVERTICAL);

    // Workspace switcher, Import, collapse.
    auto* head = new wxBoxSizer(wxHORIZONTAL);
    ws_btn_ = new wxButton(p, wxID_ANY, "Opening workspace...", wxDefaultPosition, wxDefaultSize,
                           wxBU_LEFT | wxBORDER_NONE);
    ws_btn_->SetBackgroundColour(window_colour());
    ws_btn_->SetForegroundColour(text_colour());
    {
      wxFont f = ws_btn_->GetFont();
      f.SetWeight(wxFONTWEIGHT_BOLD);
      f.SetFractionalPointSize(f.GetFractionalPointSize() * 1.08);
      ws_btn_->SetFont(f);
    }
    ws_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { workspace_menu(); });
    auto* import = flat_button(p, "Import", "Import a curl command as a saved request");
    import->SetForegroundColour(mix(window_colour(), text_colour(), 0.75));
    import->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { import_curl(target_folder(tree_->GetSelection())); });
    auto* hide = flat_button(p, wxString::FromUTF8("\xC2\xAB"), "Hide the sidebar (Ctrl+B)");
    {
      wxFont f = hide->GetFont();
      f.SetFractionalPointSize(f.GetFractionalPointSize() * 1.4);
      hide->SetFont(f);
    }
    hide->SetForegroundColour(dim_colour());
    hide->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { set_collapsed(true); });
    head->Add(ws_btn_, 1, wxALIGN_CENTER_VERTICAL);
    head->Add(import, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(2));
    head->Add(hide, 0, wxALIGN_CENTER_VERTICAL);
    sz->Add(head, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(4));

    // Environment switcher: a flat button with a menu (a native combo draws a light focus
    // rectangle in dark mode).
    auto* envrow = new wxBoxSizer(wxHORIZONTAL);
    env_menu_btn_ = new wxButton(p, wxID_ANY, "No environment", wxDefaultPosition, wxSize(-1, FromDIP(28)),
                                 wxBU_LEFT | wxBORDER_NONE);
    env_menu_btn_->SetBackgroundColour(mix(window_colour(), text_colour(), 0.07));
    env_menu_btn_->SetForegroundColour(text_colour());
    env_menu_btn_->SetToolTip("Active environment: {{variables}} are taken from it, then from Globals");
    env_menu_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { env_menu(); });
    env_btn_ = flat_button(p, wxString::FromUTF8("\xE2\x9A\x99"), "Edit environments and globals");
    {
      wxFont f = env_btn_->GetFont();
      f.SetFractionalPointSize(f.GetFractionalPointSize() * 1.25);
      env_btn_->SetFont(f);
    }
    envrow->Add(env_menu_btn_, 1, wxALIGN_CENTER_VERTICAL);
    envrow->Add(env_btn_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
    sz->Add(envrow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);
    env_warn_ = new wxStaticText(p, wxID_ANY, "");
    env_warn_->SetForegroundColour(is_dark() ? wxColour(0xF0, 0xA0, 0x3C) : wxColour(0xA0, 0x4F, 0x00));
    env_warn_->Hide();
    sz->Add(env_warn_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);
    env_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { edit_envs(); });
    env_warn_->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { edit_envs(); });

    // Filter.
    filter_ = new wxTextCtrl(p, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    filter_->SetHint("Filter by name, folder, method or URL");
    filter_->SetToolTip(
        "Filter requests and history (Ctrl+Shift+F). Down: best match, Enter: open it, Esc: clear.\n"
        "Ctrl+P opens any saved request by fuzzy name.");
    sz->Add(filter_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);
    filter_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { apply_filter(); });
    filter_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { filter_timer_.StartOnce(70); });
    filter_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
      const int k = e.GetKeyCode();
      if (k == WXK_ESCAPE && !filter_->IsEmpty()) {
        filter_->Clear();
        apply_filter();
        reveal_active();
        return;
      }
      if (k == WXK_DOWN || k == WXK_RETURN || k == WXK_NUMPAD_ENTER) {
        apply_filter();  // a pending debounce
        const bool enter = k != WXK_DOWN;
        // The best match (exact name first, as Ctrl+P ranks), never a folder.
        const fs::path best = best_match();
        wxTreeItemId first = best.empty() ? wxTreeItemId() : item_for(best, true);
        if (enter && first.IsOk()) {
          syncing_ = true;
          tree_->SelectItem(first);
          tree_->EnsureVisible(first);
          syncing_ = false;
          open_preview(best);
          return;
        }
        if (enter && !filter_q_.empty()) return;  // nothing matches
        ++tree_focus_req_;
        tree_->SetFocus();
        if (first.IsOk()) {
          tree_->SelectItem(first);  // opens the preview
          tree_->EnsureVisible(first);
        } else if (!tree_->GetSelection().IsOk()) {
          wxTreeItemIdValue cookie;
          wxTreeItemId top = tree_->GetFirstChild(tree_->GetRootItem(), cookie);
          if (top.IsOk()) tree_->SelectItem(top);
        }
        return;
      }
      e.Skip();
    });

    // Collection (top) and history (bottom).
    split_ = new wxSplitterWindow(p, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSP_LIVE_UPDATE | wxSP_NOBORDER);
    split_->SetBackgroundColour(window_colour());
    split_->SetSashGravity(1.0);  // the tree takes what the window gains; history keeps its height
    split_->SetMinimumPaneSize(FromDIP(64));

    auto* top = new wxPanel(split_);
    top->SetBackgroundColour(window_colour());
    auto* tsz = new wxBoxSizer(wxVERTICAL);
    auto* thead = new wxBoxSizer(wxHORIZONTAL);
    coll_label_ = section_label(top, "COLLECTION");
    auto* add = flat_button(top, "+", "New request or folder, import curl");
    {
      wxFont f = add->GetFont();
      f.SetFractionalPointSize(f.GetFractionalPointSize() * 1.3);
      add->SetFont(f);
    }
    add->Bind(wxEVT_BUTTON, [this, add](wxCommandEvent&) { new_menu(add); });
    thead->Add(coll_label_, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, pad);
    thead->Add(add, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    tsz->Add(thead, 0, wxEXPAND | wxTOP, FromDIP(6));
    tree_ = new CollectionTree(top, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           wxTR_HIDE_ROOT | wxTR_NO_BUTTONS | wxTR_NO_LINES | wxTR_FULL_ROW_HIGHLIGHT |
                               wxTR_SINGLE | wxTR_EDIT_LABELS | wxBORDER_NONE);
    // No native expand buttons: the folder images carry the chevron next to the icon, and the
    // ~20 px button column at the root is saved. A click on a folder row toggles it, as do
    // Left / Right / Enter.
    tree_->SetImages(tree_images(this));
    tree_->SetIndent(FromDIP(14));
    tsz->Add(tree_, 1, wxEXPAND | wxTOP, FromDIP(2));
    empty_ = new wxStaticText(top, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE);
    empty_->SetForegroundColour(dim_colour());
    tsz->Add(empty_, 1, wxEXPAND | wxALL, FromDIP(12));
    empty_->Hide();
    top->SetSizer(tsz);

    auto* bottom = new wxPanel(split_);
    bottom->SetBackgroundColour(window_colour());
    auto* bsz = new wxBoxSizer(wxVERTICAL);
    auto* sep = new wxPanel(bottom, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    sep->SetBackgroundColour(mix(window_colour(), text_colour(), 0.14));
    bsz->Add(sep, 0, wxEXPAND);
    hist_label_ = section_label(bottom, "HISTORY");
    // Expand: history takes the sidebar's full height (Postman's History tab), and back.
    hist_expand_ = flat_button(bottom, "Expand", "Show history at full height (double-click HISTORY)");
    hist_expand_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { toggle_history_full(); });
    hist_label_->Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent&) { toggle_history_full(); });
    auto* hhead = new wxBoxSizer(wxHORIZONTAL);
    hhead->Add(hist_label_, 1, wxALIGN_CENTER_VERTICAL);
    hhead->Add(hist_expand_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    bsz->Add(hhead, 0, wxEXPAND | wxLEFT | wxTOP, pad);
    history_ = new HistoryList(bottom);
    history_->on_open = [this](const HistoryEntry& e, bool permanent, bool open_saved) {
      open_history_entry(e, permanent, open_saved);
    };
    history_->has_saved = [this](const HistoryEntry& e) { return !history_file(e).empty(); };
    history_->describe = [this](const HistoryEntry& e, std::string* name, std::string* target) {
      describe_history(e, name, target);
    };
    bsz->Add(history_, 1, wxEXPAND | wxTOP, FromDIP(2));
    hist_empty_ = new wxStaticText(bottom, wxID_ANY, "Sent requests appear here.");
    hist_empty_->SetForegroundColour(dim_colour());
    bsz->Add(hist_empty_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);
    bottom->SetSizer(bsz);

    split_->SplitHorizontally(top, bottom, 0);
    top_pane_ = top;
    split_->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      if (sash_set_ && !user_sash_) CallAfter([this] { update_history_pane(); });
    });
    split_->Bind(wxEVT_SPLITTER_SASH_POS_CHANGED, [this](wxSplitterEvent& e) {
      e.Skip();
      // The user dragged the sash: keep the history height they chose. (Resizes and
      // SetSashPosition() can report here too; only a drag has the button down.)
      if (wxGetMouseState().LeftIsDown()) user_sash_ = true;
    });
    sz->Add(split_, 1, wxEXPAND | wxTOP, FromDIP(6));
    content_->SetSizer(sz);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      if (!sash_set_ && GetClientSize().y > FromDIP(200)) {
        sash_set_ = true;
        CallAfter([this] { update_history_pane(); });
      }
    });

    bind_tree();
    Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& e) {
      e.Skip();
      tree_->SetImages(tree_images(this));
    });
    Bind(wxEVT_SYS_COLOUR_CHANGED, [this](wxSysColourChangedEvent& e) {
      e.Skip();
      tree_->SetImages(tree_images(this));
      history_->Refresh();
    });
  }

  void toggle_history_full() {
    hist_full_ = !hist_full_;
    hist_expand_->SetLabel(hist_full_ ? "Restore" : "Expand");
    hist_expand_->SetToolTip(hist_full_ ? "Show the collection again" : "Show history at full height (double-click HISTORY)");
    hist_expand_->GetParent()->Layout();
    if (hist_full_) {
      hist_saved_sash_ = split_->GetSashPosition();
      hist_saved_user_ = user_sash_;
      user_sash_ = true;
      split_->Unsplit(split_->GetWindow1());  // the collection pane hides; history fills the sidebar
    } else {
      user_sash_ = hist_saved_user_;
      split_->SplitHorizontally(top_pane_, split_->GetWindow1(),
                                hist_saved_sash_ > 0 ? hist_saved_sash_ : split_->GetClientSize().y / 2);
      update_history_pane();
    }
  }

  // History pane height follows its content (an empty one is small), up to 45% of the
  // sidebar, until the user drags the sash.
  void update_history_pane() {
    const bool empty = history_->shown() == 0;
    const wxString hint = history_->total() ? "No history entry matches the filter." : "Sent requests appear here.";
    if (hist_empty_->GetLabel() != hint) hist_empty_->SetLabel(hint);
    if (hist_empty_->IsShown() != empty || history_->IsShown() == empty) {
      hist_empty_->Show(empty);
      history_->Show(!empty);
      hist_empty_->GetParent()->Layout();
    }
    if (user_sash_ || !sash_set_) return;
    const int h = split_->GetClientSize().y;
    if (h < FromDIP(160)) return;
    int want;
    if (history_->total() == 0) {
      want = FromDIP(64);
    } else {
      const int rows = static_cast<int>(std::min<size_t>(history_->total(), 10));
      want = FromDIP(44 + 24) + rows * FromDIP(38);
      want = std::clamp(want, FromDIP(120), h * 45 / 100);
    }
    const int pos = h - want - split_->GetSashSize();
    if (std::abs(split_->GetSashPosition() - pos) > 2) split_->SetSashPosition(pos);
  }

  // Hides everything but a narrow rail with a "show" button and shrinks the AUI pane.
  void set_collapsed(bool c) {
    if (c == collapsed_) return;
    wxAuiManager* m = wxAuiManager::GetManager(this);
    if (!m) return;
    wxAuiPaneInfo& pane = m->GetPane(this);
    if (!pane.IsOk()) return;
    if (c) {
      expanded_width_ = GetSize().x;
      if (!saved_min_.IsFullySpecified()) saved_min_ = pane.min_size;
    }
    collapsed_ = c;
    const int w = c ? FromDIP(30) : std::max(expanded_width_, FromDIP(200));
    // Re-docking makes AUI take the new best size (it keeps a dock's size while it exists).
    pane.Hide();
    m->Update();
    content_->Show(!c);
    rail_->Show(c);
    Layout();
    pane.Show().BestSize(wxSize(w, -1)).MinSize(c ? wxSize(w, -1) : saved_min_);
    if (!c) pane.MinSize(saved_min_.IsFullySpecified() ? saved_min_ : wxSize(FromDIP(160), -1));
    m->Update();
    if (!c) CallAfter([this] { update_history_pane(); });
  }

  void bind_tree() {
    tree_->Bind(wxEVT_TREE_ITEM_EXPANDING, [this](wxTreeEvent& e) {
      populate(e.GetItem());
      e.Skip();
    });
    tree_->Bind(wxEVT_TREE_ITEM_EXPANDED, [this](wxTreeEvent& e) {
      // Also during a rebuild: a folder opened to reveal the selection stays open on the next
      // rebuild (a new folder next to it must not collapse it).
      if (const auto* d = data(e.GetItem()); d && filter_q_.empty() && expanded_.insert(d->node->path).second)
        save_expanded();
      if (const auto* d = data(e.GetItem()); d && d->node->is_folder) folder_image(e.GetItem(), true);
    });
    tree_->Bind(wxEVT_TREE_ITEM_COLLAPSED, [this](wxTreeEvent& e) {
      if (const auto* d = data(e.GetItem()); d && !rebuilding_ && filter_q_.empty()) {
        expanded_.erase(d->node->path);
        save_expanded();
      }
      if (const auto* d = data(e.GetItem()); d && d->node->is_folder) folder_image(e.GetItem(), false);
    });
    // Single click: preview tab (like VS Code); double click / Enter: permanent tab.
    tree_->Bind(wxEVT_TREE_SEL_CHANGED, [this](wxTreeEvent& e) {
      if (syncing_ || rebuilding_) return;
      const auto* d = data(e.GetItem());
      if (!d || d->node->is_folder) return;
      host_->open_request(d->node->path, true);
      // A preview keeps the keyboard in the tree (arrows, F2, Del), as in VS Code; the tab may
      // have moved the focus to its URL field.
      CallAfter([this] {
        if (tree_->IsShownOnScreen()) tree_->SetFocus();
      });
    });
    tree_->Bind(wxEVT_TREE_ITEM_ACTIVATED, [this](wxTreeEvent& e) {
      const auto* d = data(e.GetItem());
      if (!d) return;
      if (d->node->is_folder) tree_->Toggle(e.GetItem());
      else host_->open_request(d->node->path, false);
    });
    // A click on a folder row toggles it (no need to hit the small twisty).
    tree_->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
      e.Skip();
      int flags = 0;
      const wxTreeItemId id = tree_->HitTest(e.GetPosition(), flags);
      if (!id.IsOk() || (flags & wxTREE_HITTEST_ONITEMBUTTON)) return;
      const auto* d = data(id);
      if (d && d->node->is_folder && (flags & (wxTREE_HITTEST_ONITEMLABEL | wxTREE_HITTEST_ONITEMICON |
                                               wxTREE_HITTEST_ONITEMRIGHT | wxTREE_HITTEST_ONITEMINDENT)))
        CallAfter([this, id] {
          if (id.IsOk()) tree_->Toggle(id);
        });
      // A click on the request that is already selected fires no SEL_CHANGED (after a
      // duplicate, a drop, a menu, or with another tab active): open it here.
      if (d && !d->node->is_folder && id == tree_->GetSelection()) {
        const fs::path p = d->node->path;
        CallAfter([this, p] {
          host_->open_request(p, true);
          if (tree_->IsShownOnScreen()) tree_->SetFocus();
        });
      }
    });
    tree_->Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent& e) {
      int flags = 0;
      const wxTreeItemId id = tree_->HitTest(e.GetPosition(), flags);
      const auto* d = id.IsOk() ? data(id) : nullptr;
      if (d && d->node->is_folder && !(flags & wxTREE_HITTEST_ONITEMBUTTON)) {
        tree_->Toggle(id);  // second click of a double click: toggle again, as a click would
        return;
      }
      e.Skip();
    });
    build_hover_button();
    tree_->Bind(wxEVT_TREE_KEY_DOWN, [this](wxTreeEvent& e) {
      const wxTreeItemId sel = tree_->GetSelection();
      switch (e.GetKeyCode()) {
        case WXK_F2:
          if (sel.IsOk()) edit_label(sel);
          return;
        case WXK_DELETE:
          if (sel.IsOk()) delete_item(sel);
          return;
        case 'D':  // Ctrl+D: duplicate (Bruno, Postman)
          if (e.GetKeyEvent().GetModifiers() == wxMOD_CONTROL) {
            if (const auto* d = data(sel); d && !d->node->is_folder) duplicate_item(d->node->path);
            return;
          }
          e.Skip();
          return;
        default:
          e.Skip();
      }
    });
    // Rename only on F2, the menu or a new item: the native slow second click on a selected
    // row would start it too, but a click opens (as in Postman and Bruno).
    tree_->Bind(wxEVT_TREE_BEGIN_LABEL_EDIT, [this](wxTreeEvent& e) {
      if (!data(e.GetItem()) || !edit_ok_) e.Veto();
    });
    tree_->Bind(wxEVT_TREE_END_LABEL_EDIT, [this](wxTreeEvent& e) {
      if (rebuild_after_edit_ || deferred_scan_) {
        rebuild_after_edit_ = false;
        CallAfter([this] {
          if (tree_->GetEditControl()) return;
          if (auto d = std::move(deferred_scan_)) {
            const bool fin = deferred_final_;
            deferred_final_ = false;
            apply_scan(std::move(d->first), std::move(d->second), fin);
          } else {
            rebuild();
          }
        });
      }
      const auto* d = data(e.GetItem());
      const fs::path from = d ? d->node->path : fs::path();
      // A new or duplicated request opens now that it has its name (or kept the proposed one).
      const bool open_after = !from.empty() && from == open_after_edit_;
      if (open_after) open_after_edit_.clear();
      const std::string name = e.IsEditCancelled() ? std::string() : e.GetLabel().Strip(wxString::both).utf8_string();
      if (!d || name.empty() || name == d->node->name) {
        if (!e.IsEditCancelled()) e.Veto();
        if (open_after) CallAfter([this, from] { open_tab_for_edit(from); });
        return;
      }
      e.Veto();  // the rescan relabels it
      CallAfter([this, from, name, open_after] {
        const fs::path to = rename_node(from, name);
        if (open_after) open_tab_for_edit(to.empty() ? from : to);
      });
    });
    tree_->Bind(wxEVT_TREE_ITEM_MENU, [this](wxTreeEvent& e) { item_menu(e.GetItem()); });
    tree_->Bind(wxEVT_CONTEXT_MENU, [this](wxContextMenuEvent& e) {
      // Right click on the empty area below the items.
      wxPoint p = tree_->ScreenToClient(e.GetPosition());
      int flags = 0;
      if (e.GetPosition() != wxDefaultPosition && !tree_->HitTest(p, flags).IsOk()) item_menu(wxTreeItemId());
      else e.Skip();
    });
    tree_->Bind(wxEVT_TREE_BEGIN_DRAG, [this](wxTreeEvent& e) {
      if (const auto* d = data(e.GetItem())) {
        drag_from_ = d->node->path;
        e.Allow();
      }
    });
    tree_->Bind(wxEVT_TREE_END_DRAG, [this](wxTreeEvent& e) {
      const fs::path from = std::move(drag_from_);
      drag_from_.clear();
      if (from.empty() || !ws_) return;
      fs::path dest = ws_->root();
      const auto* d = data(e.GetItem());
      // Onto a request of the same folder (a folder onto a sibling folder with Alt): reorder.
      if (d && d->node->path != from && d->node->path.parent_path() == from.parent_path() &&
          (!d->node->is_folder || wxGetKeyState(WXK_ALT))) {
        const fs::path onto = d->node->path;
        CallAfter([this, from, onto] { reorder_item(from, onto); });
        return;
      }
      if (d) dest = d->node->is_folder ? d->node->path : d->node->path.parent_path();
      CallAfter([this, from, dest] { move_item(from, dest); });
    });
  }

  // ---------------------------------------------------------------- tree

  void edit_label(const wxTreeItemId& id) {
    edit_ok_ = true;
    tree_->EditLabel(id);  // BEGIN_LABEL_EDIT is sent from in here
    edit_ok_ = false;
    // The native edit box is white; give it the tree's colours (a slightly raised field).
    if (wxTextCtrl* ed = tree_->GetEditControl()) {
      // Dark scroll bars / border for the native edit (its colours: CollectionTree).
      if (is_dark()) SetWindowTheme(static_cast<HWND>(ed->GetHWND()), L"DarkMode_CFD", nullptr);
      ed->Refresh();
    }
  }

  // The folder image carries the chevron: open / closed, also while the row is selected.
  void folder_image(const wxTreeItemId& id, bool open) {
    const int img = open ? kImgFolderOpen : kImgFolder;
    tree_->SetItemImage(id, img);
    tree_->SetItemImage(id, img, wxTreeItemIcon_Selected);
  }

  ItemData* data(const wxTreeItemId& id) const {
    return id.IsOk() ? static_cast<ItemData*>(tree_->GetItemData(id)) : nullptr;
  }

  wxTreeItemId add_item(const wxTreeItemId& parent, const CollectionNode& c) {
    const int img = c.is_folder ? kImgFolder : (c.badge ? tree_image(c.kind, c.method) : kImgUnknown);
    auto* d = new ItemData(&c);
    const wxTreeItemId id = tree_->AppendItem(parent, wxString::FromUTF8(c.name), img, img, d);
    if (c.is_folder && !c.children.empty()) tree_->SetItemHasChildren(id, true);
    return id;
  }

  // Inserts the children of a folder item (once).
  void populate(const wxTreeItemId& item) {
    const CollectionNode* node = &view_root_;
    ItemData* d = data(item);
    if (d) {
      if (d->populated) return;
      node = d->node;
      d->populated = true;
    } else if (item != tree_->GetRootItem() || root_populated_) {
      return;
    } else {
      root_populated_ = true;
    }
    std::vector<const CollectionNode*> kids;
    kids.reserve(node->children.size());
    for (const auto& c : node->children) kids.push_back(&c);
    sort_kids(node->path, kids);
    // While filtering, matched folders open until ~250 rows are in the tree: a filter that
    // matches 2000 requests then costs a few ms per key, not 70 (the rest open on demand).
    const bool filtering = !filter_q_.empty();
    for (const CollectionNode* c : kids) {
      const wxTreeItemId id = add_item(item, *c);
      if (!c->is_folder || c->children.empty()) continue;
      bool open = expanded_.count(c->path) != 0;
      if (filtering) {
        open = expand_budget_ > 0;
        if (open) expand_budget_ -= static_cast<int>(c->children.size());
      }
      if (open) {
        populate(id);
        tree_->Expand(id);
        folder_image(id, true);
      }
    }
  }

  // The item for a path; with `reveal`, folders on the way are populated and expanded.
  wxTreeItemId item_for(const fs::path& p, bool reveal) {
    wxTreeItemId cur = tree_->GetRootItem();
    if (!cur.IsOk()) return {};
    for (int depth = 0; depth < 256; ++depth) {
      if (reveal) populate(cur);
      wxTreeItemIdValue cookie;
      wxTreeItemId next;
      for (wxTreeItemId c = tree_->GetFirstChild(cur, cookie); c.IsOk(); c = tree_->GetNextChild(cur, cookie)) {
        const auto* d = data(c);
        if (!d) continue;
        if (d->node->path == p) return c;
        if (d->node->is_folder && is_under(p, d->node->path)) {
          next = c;
          break;
        }
      }
      if (!next.IsOk()) return {};
      if (reveal && !tree_->IsExpanded(next)) {
        populate(next);
        tree_->Expand(next);
      }
      cur = next;
    }
    return {};
  }

  void rebuild() {
    // A later scan pass (full badges, names) must not kill the rename editor of a new or
    // duplicated item: rebuild once the edit ends.
    if (tree_->GetEditControl()) {
      rebuild_after_edit_ = true;
      return;
    }
    // Remember what the user sees.
    fs::path sel_path, top_path;
    bool sel_folder = false;
    if (const auto* d = data(tree_->GetSelection())) {
      sel_path = d->node->path;
      sel_folder = d->node->is_folder;
    }
    // The highlight follows the active tab (after a save, delete or move the old row's
    // neighbour must not stay selected); a selected folder stays.
    if (!active_file_.empty() && (!sel_folder || reveal_active_)) sel_path = active_file_;
    if (const auto* d = data(tree_->GetFirstVisibleItem())) top_path = d->node->path;

    if (filter_q_.empty()) {
      view_root_ = tree_root_;
    } else {
      view_root_ = CollectionNode{};
      if (search_keys_.empty()) build_keys(tree_root_, "", meta_, search_keys_);
      filter_node(tree_root_, filter_q_, search_keys_, view_root_);
      view_root_.name = tree_root_.name;
      view_root_.path = tree_root_.path;
      view_root_.is_folder = true;
    }
    // Typing in the filter keeps the keyboard there. The native tree takes the focus when its
    // selected item is deleted (e.g. the active gRPC preview filtered out), and the rest of the
    // query and the Enter then went to the tree or the editor.
    const bool keep_filter = wxWindow::FindFocus() == filter_;
    long sel_from = 0, sel_to = 0;
    if (keep_filter) filter_->GetSelection(&sel_from, &sel_to);
    hide_hover();  // its item is about to be deleted
    rebuilding_ = true;
    tree_->Freeze();
    tree_->UnselectAll();  // deleting the selected item would move the focus to the tree
    tree_->DeleteAllItems();
    root_populated_ = false;
    expand_budget_ = 250;
    const wxTreeItemId root = tree_->AddRoot("root");
    populate(root);
    if (!sel_path.empty()) {
      if (wxTreeItemId id = item_for(sel_path, true); id.IsOk()) {
        tree_->SelectItem(id);
        if (sel_path == active_file_) reveal_active_ = false;
      }
    }
    if (!top_path.empty())
      if (wxTreeItemId id = item_for(top_path, false); id.IsOk()) tree_->ScrollTo(id);
    tree_->Thaw();
    rebuilding_ = false;
    if (keep_filter) {
      auto restore = [this, sel_from, sel_to] {
        if (wxWindow::FindFocus() == filter_) return;
        filter_->SetFocus();
        filter_->SetSelection(sel_from, sel_to);
      };
      restore();
      // Also after a focus change the native tree posts for itself.
      CallAfter([this, restore, req = tree_focus_req_] {
        if (wxWindow::FindFocus() == tree_ && req == tree_focus_req_) restore();
      });
    }

    const bool empty = view_root_.children.empty();
    wxString msg;
    if (!ws_) msg = "Opening workspace...";
    else if (!filter_q_.empty()) msg = "No saved request matches the filter.";
    else if (scanning_ && tree_root_.children.empty()) msg = "Loading...";
    else
      msg = "No saved requests yet.\n\nCtrl+S saves the current tab into this workspace; + creates a request or "
            "folder. Requests are plain .req.json files, so the folder works with git.";
    if (empty) {
      empty_->SetLabel(msg);
      empty_->Wrap(std::max(FromDIP(120), tree_->GetParent()->GetClientSize().x - FromDIP(24)));
    }
    if (empty_->IsShown() != empty || tree_->IsShown() == empty) {
      empty_->Show(empty);
      tree_->Show(!empty);
      tree_->GetParent()->Layout();
    }
    update_counts();
    if (!pending_edit_.empty() && !scanning_) {
      const fs::path p = std::move(pending_edit_);
      pending_edit_.clear();
      if (wxTreeItemId id = item_for(p, true); id.IsOk()) {
        syncing_ = true;  // the item is being named: no preview (it was opened by its creator)
        tree_->SelectItem(id);
        syncing_ = false;
        tree_->EnsureVisible(id);
        tree_->SetFocus();
        // After the tab that shows the new item has settled (it may take the focus once).
        wxTimer* t = new wxTimer;
        t->Bind(wxEVT_TIMER, [this, t, p, alive = std::weak_ptr<int>(alive_)](wxTimerEvent&) {
          if (!alive.expired()) {
            bool editing = false;
            if (wxTreeItemId it = item_for(p, false); it.IsOk() && !tree_->GetEditControl()) {
              tree_->SetFocus();
              edit_label(it);
              editing = tree_->GetEditControl() != nullptr;
            }
            // No rename editor after all: open the new request under its proposed name.
            if (!editing && open_after_edit_ == p) {
              open_after_edit_.clear();
              open_tab(p);
            }
          }
          wxTheApp->CallAfter([t] { delete t; });
        });
        t->StartOnce(150);
      } else if (open_after_edit_ == p) {
        open_after_edit_.clear();
        CallAfter([this, p] { open_tab(p); });
      }
    }
    if (select_after_scan_ && !scanning_) {
      select_after_scan_ = false;
      const fs::path p = std::move(pending_select_);
      pending_select_.clear();
      syncing_ = true;
      if (wxTreeItemId id = item_for(p, true); id.IsOk()) {
        tree_->SelectItem(id);
        tree_->EnsureVisible(id);
      }
      syncing_ = false;
    }
  }

  void update_counts() {
    const size_t n = count_requests(tree_root_) - (tree_root_.is_folder ? 0 : 1);
    wxString c = "COLLECTION";
    if (ws_ && n) {
      if (filter_q_.empty()) c += wxString::Format("   %zu", n);
      else c += wxString::Format("   %zu of %zu", count_requests(view_root_), n);
    }
    if (coll_label_->GetLabel() != c) coll_label_->SetLabel(c);
    wxString h = "HISTORY";
    if (history_->total()) {
      if (history_->shown() == history_->total()) h += wxString::Format("   %zu", history_->total());
      else h += wxString::Format("   %zu of %zu", history_->shown(), history_->total());
    }
    if (hist_label_->GetLabel() != h) hist_label_->SetLabel(h);
  }

  void apply_filter() {
    filter_timer_.Stop();
    const std::string q = ascii_lower(filter_->GetValue().Strip(wxString::both).utf8_string());
    if (q == filter_q_) return;
    filter_q_ = q;
    rebuild();
    history_->set_filter(q);
    update_counts();
    update_history_pane();
  }

  // The request that best matches the filter (see filter_rank); ties go to tree order.
  fs::path best_match() const {
    if (filter_q_.empty()) return {};
    fs::path best;
    int best_score = -1;
    std::function<void(const CollectionNode&, const std::string&)> walk = [&](const CollectionNode& n,
                                                                              const std::string& folder) {
      std::vector<const CollectionNode*> kids;
      kids.reserve(n.children.size());
      for (const auto& c : n.children) kids.push_back(&c);
      sort_kids(n.path, kids);
      for (const CollectionNode* c : kids) {
        if (c->is_folder) {
          walk(*c, folder.empty() ? c->name : folder + "/" + c->name);
          continue;
        }
        std::string rest = c->kind == RequestKind::Grpc ? std::string("grpc") : c->method;
        if (auto m = meta_.find(c->path); m != meta_.end()) rest += " " + m->second.url;
        rest += " " + u8(c->path.filename());
        const int s = filter_rank(filter_q_, c->name, folder, rest);
        if (s > best_score) {
          best_score = s;
          best = c->path;
        }
      }
    };
    walk(view_root_, "");
    return best;
  }

  // Selects and scrolls to the active tab's request (after the filter was cleared).
  void reveal_active() {
    if (active_file_.empty()) return;
    syncing_ = true;
    if (wxTreeItemId id = item_for(active_file_, true); id.IsOk()) {
      tree_->SelectItem(id);
      centre_item(id);
    }
    syncing_ = false;
  }

  // Scrolls so the item sits in the middle of the tree, unless it is already well inside
  // (EnsureVisible alone leaves it on the bottom edge with its folder scrolled off).
  void centre_item(const wxTreeItemId& id) {
    if (!id.IsOk()) return;
    tree_->EnsureVisible(id);
    wxRect r;
    if (!tree_->GetBoundingRect(id, r) || r.height <= 0) return;
    const int h = tree_->GetClientSize().y;
    if (r.y >= h / 5 && r.GetBottom() <= h * 4 / 5) return;
    int up = h / r.height / 2;
    wxTreeItemId top = id;
    while (up-- > 0) {
      const wxTreeItemId prev = tree_->GetPrevVisible(top);
      if (!prev.IsOk()) break;
      top = prev;
    }
    tree_->ScrollTo(top);
  }

  // Folder scan on a worker; the badge-less first pass shows at once, a full pass follows when
  // some badges were not read within the budget (fresh clone). Request names come from the
  // name cache (no file reads) and are then checked against the files; a changed name is
  // delivered as one more pass.
  void rescan() {
    if (!ws_) return;
    auto copy = std::make_shared<Workspace>(*ws_);
    auto names = name_cache_;
    const uint64_t gen = ++scan_gen_;
    scanning_ = true;
    std::weak_ptr<int> alive = alive_;
    std::thread([this, copy, names, gen, alive] {
      auto deliver = [this, gen, alive](CollectionNode node, MetaMap meta, bool final) {
        auto* holder = new std::pair<CollectionNode, MetaMap>(std::move(node), std::move(meta));
        if (wxAppConsole* app = wxTheApp) app->CallAfter([this, gen, alive, holder, final] {
          std::unique_ptr<std::pair<CollectionNode, MetaMap>> h(holder);
          if (alive.expired() || gen != scan_gen_) return;
          apply_scan(std::move(h->first), std::move(h->second), final);
        });
      };
      CollectionNode first = copy->tree(25);
      const bool missing = any_badge_missing(first);
      MetaMap meta = names ? names->apply_cached(first) : MetaMap{};
      CollectionNode latest = missing ? CollectionNode{} : first;
      deliver(std::move(first), std::move(meta), !missing);
      if (missing) {
        latest = copy->tree(-1);
        CollectionNode full = latest;
        MetaMap m2 = names ? names->apply_cached(full) : MetaMap{};
        deliver(std::move(full), std::move(m2), true);
      }
      if (!names) return;
      bool changed = false;
      MetaMap m3 = names->refresh(latest, &changed);
      if (changed) deliver(std::move(latest), std::move(m3), true);
    }).detach();
  }

  // A scan pass landed. While a label is being edited the items' data must stay valid (they
  // point into tree_root_), so the pass waits for the edit to end.
  void apply_scan(CollectionNode root, MetaMap meta, bool final) {
    if (tree_->GetEditControl()) {
      deferred_scan_ = std::make_unique<std::pair<CollectionNode, MetaMap>>(std::move(root), std::move(meta));
      deferred_final_ = deferred_final_ || final;
      return;
    }
    tree_root_ = std::move(root);
    meta_ = std::move(meta);
    if (final) scanning_ = false;
    meta_changed();
    rebuild();
  }

  void load_history() {
    if (!ws_) return;
    auto copy = std::make_shared<Workspace>(*ws_);
    const uint64_t gen = ++hist_gen_;
    std::weak_ptr<int> alive = alive_;
    std::thread([this, copy, gen, alive] {
      auto* list = new std::vector<HistoryEntry>(copy->history(500));
      // The list lines hold only "GET <url>": the name of the request each entry was sent as
      // comes from its stored request, so two saved requests with the same URL keep their own
      // names after a restart. Read here, on the worker.
      // Its id too: an entry sent with unsaved edits (another URL) still maps to its file.
      auto* names = new std::map<std::string, HistMeta>;
      for (const auto& e : *list)
        if (auto r = copy->history_request(e); r && (!r->name.empty() || !r->id.empty()))
          (*names)[e.when_iso + "\x01" + e.summary] = HistMeta{r->name, r->id};
      if (wxAppConsole* app = wxTheApp) app->CallAfter([this, gen, alive, list, names] {
        std::unique_ptr<std::vector<HistoryEntry>> h(list);
        std::unique_ptr<std::map<std::string, HistMeta>> nm(names);
        if (alive.expired() || gen != hist_gen_) return;
        hist_names_ = std::move(*nm);
        history_->set_entries(std::move(*h));
        update_counts();
        update_history_pane();
      });
    }).detach();
  }

  // ---------------------------------------------------------------- file operations

  fs::path target_folder(const wxTreeItemId& id) const {
    if (!ws_) return {};
    if (const auto* d = data(id)) return d->node->is_folder ? d->node->path : d->node->path.parent_path();
    return ws_->root();
  }

  // Like Bruno and Postman: the request is created in the folder at once ("New request",
  // new-request.req.json) with the rename editor open; its tab opens under the chosen name.
  void new_request_in(RequestKind kind, const fs::path& folder_in) {
    if (!ws_) return;
    const fs::path folder = folder_in.empty() ? ws_->root() : folder_in;
    std::string err;
    auto r = sidebar::create_request(*ws_, folder, kind, kind == RequestKind::Grpc ? "New gRPC request" : "New request",
                                     &err);
    if (!r) {  // read-only folder or similar: an unsaved tab whose first save goes there
      host_->new_request(kind, folder == ws_->root() ? fs::path() : folder);
      return;
    }
    if (folder != ws_->root()) expanded_.insert(folder);
    if (name_cache_) name_cache_->note(r->file, r->name, {}, r->id);
    if (!filter_q_.empty()) {  // the new item must be visible to be named
      filter_->ChangeValue("");
      apply_filter();
    }
    name_then_open(r->file);
  }

  void new_folder_in(const fs::path& folder) {
    if (!ws_ || folder.empty()) return;
    if (hist_full_) toggle_history_full();
    std::error_code ec;
    fs::path p = folder / L"New folder";
    for (int n = 2; fs::exists(p, ec); ++n) p = folder / (L"New folder " + std::to_wstring(n));
    if (!fs::create_directory(p, ec)) {
      notify(wxGetTopLevelParent(this), "Could not create the folder", wxString::FromUTF8(ec.message()));
      return;
    }
    if (folder != ws_->root()) expanded_.insert(folder);
    pending_edit_ = p;
    rescan();
  }

  // Requests under p (p itself if it is a request), from disk.
  std::vector<fs::path> requests_under(const fs::path& p) const {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(p, ec)) {
      out.push_back(p);
      return out;
    }
    for (fs::recursive_directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec))
      if (it->is_regular_file(ec) && is_request_file(it->path())) out.push_back(it->path());
    return out;
  }

  void remap_expanded(const fs::path& from, const fs::path& to) {
    std::set<fs::path> next;
    for (const auto& e : expanded_) {
      if (e == from) next.insert(to);
      else if (is_under(e, from)) next.insert(to / e.lexically_relative(from));
      else next.insert(e);
    }
    expanded_ = std::move(next);
  }

  // Renames a request (name field + kebab-case file name) or a folder. Every open tab of a
  // request under it follows, also when only the name changed and the file stayed: the shell's
  // file_moved() re-reads the name, so the tab title, its sends and its next Ctrl+S use it.
  // Returns the new path, or empty when it failed (the user was told).
  fs::path rename_node(const fs::path& from, const std::string& name) {
    if (!ws_) return {};
    const auto files = requests_under(from);
    std::string err;
    const auto to = sidebar::rename_item(*ws_, from, name, &err);
    if (!to) {
      notify(wxGetTopLevelParent(this), "Could not rename", wxString::FromUTF8("\"" + name + "\": " + err));
      return {};
    }
    for (const auto& f : files) host_->file_moved(f, f == from ? *to : *to / f.lexically_relative(from));
    if (*to != from) {
      remap_expanded(from, *to);
      save_expanded();
      remap_order(from, *to);
      if (active_file_ == from) active_file_ = *to;
    }
    if (is_request_file(*to) && name_cache_) {
      std::string url, id;
      if (auto m = meta_.find(from); m != meta_.end()) url = m->second.url, id = m->second.id;
      name_cache_->note(*to, name, url, id);
    }
    pending_select_ = *to;
    rescan_then_select();
    return *to;
  }

  // Filter Enter and Ctrl+P open like VS Code's quick open: in the preview tab (reused, a few
  // ms; the first edit makes it permanent), not in a new tab (~400 ms to build).
  void open_preview(const fs::path& p) {
    if (p.empty()) return;
    host_->open_request(p, true);
    select_file(p);
  }

  // Opens a request in a permanent tab. It goes through the preview first: an unmodified preview
  // tab is reused (a few ms) instead of building a new tab (~400 ms), then made permanent.
  void open_tab(const fs::path& p) {
    if (p.empty()) return;
    host_->open_request(p, true);
    host_->open_request(p, false);
    select_file(p);
  }

  // A new or duplicated request: open it and put the caret in its URL / address field, as Bruno
  // and Postman do, so typing goes there (not to the tree's type-ahead).
  void open_tab_for_edit(const fs::path& p) {
    open_tab(p);
    CallAfter([this, p] { focus_tab_url(p); });
  }

  void focus_tab_url(const fs::path& p) {
    auto* top = wxGetTopLevelParent(this);
    if (!top) return;
    std::function<wxAuiNotebook*(wxWindow*)> find = [&](wxWindow* w) -> wxAuiNotebook* {
      if (auto* nb = wxDynamicCast(w, wxAuiNotebook)) return nb;
      for (wxWindow* c : w->GetChildren())
        if (auto* nb = find(c)) return nb;
      return nullptr;
    };
    wxAuiNotebook* nb = find(top);
    if (!nb) return;
    auto* t = dynamic_cast<RequestTab*>(nb->GetCurrentPage());
    if (t && t->file() == p) t->focus_url();
  }

  void move_item(const fs::path& from, const fs::path& dest) {
    if (!ws_ || from.empty() || dest.empty()) return;
    if (from.parent_path() == dest || dest == from || is_under(dest, from)) return;
    std::error_code ec;
    const fs::path to = dest / from.filename();
    if (fs::exists(to, ec)) {
      notify(wxGetTopLevelParent(this), "Could not move",
             wxString::Format("\"%s\" already has an item named \"%s\".", dest.filename().wstring(),
                              from.filename().wstring()));
      return;
    }
    const auto files = requests_under(from);
    fs::rename(from, to, ec);
    if (ec) {
      notify(wxGetTopLevelParent(this), "Could not move", wxString::FromUTF8(ec.message()));
      return;
    }
    for (const auto& f : files) host_->file_moved(f, f == from ? to : to / f.lexically_relative(from));
    remap_expanded(from, to);
    remap_order(from, to);
    if (dest != ws_->root()) expanded_.insert(dest);
    pending_select_ = to;
    rescan_then_select();
  }

  // Rescans and selects pending_select_ once the scan has landed (see rebuild()).
  void rescan_then_select() {
    select_after_scan_ = true;
    rescan();
  }

  void delete_item(const wxTreeItemId& id) {
    const auto* d = data(id);
    if (!d || !ws_) return;
    const fs::path p = d->node->path;
    if (!is_under(p, ws_->root())) return;
    const auto files = requests_under(p);
    wxString title, q;
    if (d->node->is_folder) {
      title = wxString::Format("Delete the folder \"%s\"?", wxString::FromUTF8(d->node->name));
      q = wxString::Format("The folder and the %zu request%s in it go to the Recycle Bin; restore them from there.",
                           files.size(), files.size() == 1 ? "" : "s");
    } else {
      title = wxString::Format("Delete \"%s\"?", wxString::FromUTF8(d->node->name));
      q = wxString::Format("%s goes to the Recycle Bin; restore it from there.", p.filename().wstring());
    }
    if (!confirm(wxGetTopLevelParent(this), title, q, "Move to Recycle Bin", true)) return;
    // The Recycle Bin move runs on a worker: it takes ~0.2 s locally and seconds on a network
    // drive. The row goes at once; a failure puts it back with the rescan.
    if (wxTreeItemId it = item_for(p, false); it.IsOk()) {
      syncing_ = true;
      tree_->Delete(it);
      syncing_ = false;
    }
    std::weak_ptr<int> alive = alive_;
    std::thread([this, p, files, alive] {
      const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
      const bool ok = recycle(p);
      if (SUCCEEDED(co)) CoUninitialize();
      if (wxAppConsole* app = wxTheApp) app->CallAfter([this, p, files, alive, ok] {
        if (alive.expired() || !ws_) return;
        if (!ok && !ws_->remove(p)) {
          rescan();
          notify(wxGetTopLevelParent(this), "Could not delete", "It is in use, or outside the workspace.");
          return;
        }
        for (const auto& f : files) host_->file_moved(f, {});
        expanded_.erase(p);
        rescan();
      });
    }).detach();
  }

  // "Get order" -> "Get order copy" (get-order-copy.req.json) with its name in the rename
  // editor. The copy opens once it is named, so its tab shows the final name from the start.
  void duplicate_item(const fs::path& p) {
    if (!ws_) return;
    std::string err;
    auto r = sidebar::duplicate_request(*ws_, p, &err);
    if (!r) {
      notify(wxGetTopLevelParent(this), "Could not duplicate", wxString::FromUTF8(err));
      return;
    }
    // The tab of the request has unsaved edits: the copy is what the tab shows, not the file.
    if (RequestTab* t = tab_for(p); t && t->dirty()) {
      SavedRequest c = t->request();
      c.id = r->id;
      c.name = r->name;
      c.file = r->file;
      if (ws_->save_request(c, &err)) r = std::move(c);
    }
    // The scan's first pass takes names from the cache: put the copy's name there, so the
    // rename editor starts with "Get order copy", not the file name.
    if (name_cache_)
      name_cache_->note(r->file, r->name, r->kind == RequestKind::Grpc ? r->grpc_method : r->http.url, r->id);
    name_then_open(r->file);
  }

  // A new item (duplicate, new request) is selected with the rename editor open; its tab opens
  // when the edit ends, under the name the user gave it.
  void name_then_open(const fs::path& file) {
    if (hist_full_) toggle_history_full();  // the new item is named in the tree
    pending_edit_ = file;
    open_after_edit_ = file;
    rescan();
  }

  void reveal_in_explorer(const fs::path& p) {
    wxExecute(wxString::Format("explorer.exe /select,\"%s\"", p.wstring()), wxEXEC_ASYNC);
  }

  void copy_text(const wxString& s) {
    if (wxTheClipboard->Open()) {
      wxTheClipboard->SetData(new wxTextDataObject(s));
      wxTheClipboard->Close();
    }
  }

  // ---------------------------------------------------------------- menus

  void new_menu(wxWindow* anchor) {
    if (!ws_) return;
    const fs::path folder = target_folder(tree_->GetSelection());
    wxMenu m;
    m.Append(1, "New HTTP request");
    m.Append(2, "New gRPC request");
    m.AppendSeparator();
    m.Append(3, "New folder");
    m.AppendSeparator();
    m.Append(4, wxString::FromUTF8("Import cURL\xE2\x80\xA6"));
    const int id = anchor->GetPopupMenuSelectionFromUser(m, wxPoint(0, anchor->GetSize().y));
    if (id == 1) new_request_in(RequestKind::Http, folder);
    else if (id == 2) new_request_in(RequestKind::Grpc, folder);
    else if (id == 3) new_folder_in(folder);
    else if (id == 4) import_curl(folder);
  }

  // A "..." button on the row under the pointer (as in Bruno and Postman): the row's menu
  // without a right click. A small painted child of the tree, moved on mouse motion.
  void build_hover_button() {
    more_ = new wxWindow(tree_, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(22), FromDIP(16)), wxBORDER_NONE);
    more_->SetBackgroundStyle(wxBG_STYLE_PAINT);
    more_->SetCursor(wxCursor(wxCURSOR_HAND));
    more_->SetToolTip("Actions: open, new request, rename, duplicate, delete...");
    more_->Hide();
    more_->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
      wxPaintDC dc(more_);
      const wxSize sz = more_->GetClientSize();
      const bool sel = hover_item_.IsOk() && hover_item_ == tree_->GetSelection();
      // Blends into the row; a chip on the selected row and under the pointer.
      dc.SetBackground(wxBrush(more_hot_ ? mix(window_colour(), text_colour(), 0.24)
                                         : sel ? mix(window_colour(), text_colour(), 0.14) : window_colour()));
      dc.Clear();
      std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
      if (!gc) return;
      gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
      gc->SetPen(*wxTRANSPARENT_PEN);
      gc->SetBrush(wxBrush(mix(window_colour(), text_colour(), more_hot_ ? 0.95 : 0.75)));
      const double r = 1.6 * GetDPIScaleFactor(), gap = 5.0 * GetDPIScaleFactor();
      const double cx = sz.x / 2.0, cy = sz.y / 2.0;
      for (int i = -1; i <= 1; ++i) gc->DrawEllipse(cx + i * gap - r, cy - r, 2 * r, 2 * r);
    });
    more_->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) {
      more_hot_ = true;
      more_->Refresh();
    });
    more_->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
      more_hot_ = false;
      more_->Refresh();
      if (!tree_->GetScreenRect().Contains(wxGetMousePosition())) hide_hover();
    });
    more_->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
      const wxTreeItemId id = hover_item_;
      hide_hover();
      if (id.IsOk()) item_menu(id);
    });
    tree_->Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
      e.Skip();
      if (e.Dragging()) {
        hide_hover();
        return;
      }
      update_hover(e.GetPosition());
    });
    tree_->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) {
      e.Skip();
      if (!more_->GetScreenRect().Contains(wxGetMousePosition())) hide_hover();
    });
    tree_->Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& e) {
      e.Skip();
      hide_hover();
    });
    tree_->Bind(wxEVT_TREE_ITEM_EXPANDING, [this](wxTreeEvent& e) {
      e.Skip();
      hide_hover();
    });
    tree_->Bind(wxEVT_TREE_ITEM_COLLAPSING, [this](wxTreeEvent& e) {
      e.Skip();
      hide_hover();
    });
  }

  void update_hover(const wxPoint& pos) {
    int flags = 0;
    const wxTreeItemId id = tree_->HitTest(pos, flags);
    wxRect r;
    if (!id.IsOk() || !data(id) || tree_->GetEditControl() || !tree_->GetBoundingRect(id, r)) {
      hide_hover();
      return;
    }
    if (id == hover_item_ && more_->IsShown()) return;
    hover_item_ = id;
    const wxSize cs = tree_->GetClientSize();
    const int h = std::min(r.height, FromDIP(16));
    more_->SetSize(cs.x - FromDIP(26), r.y + (r.height - h) / 2, FromDIP(22), h);
    more_hot_ = false;
    more_->Show();
    more_->Refresh();
  }

  void hide_hover() {
    hover_item_ = wxTreeItemId();
    if (more_ && more_->IsShown()) more_->Hide();
  }

  void item_menu(const wxTreeItemId& id) {
    if (!ws_) return;
    const auto* d = data(id);
    // Highlight the row the menu is for without opening it (Delete / Duplicate from the menu
    // must not leave a preview tab behind).
    if (id.IsOk()) {
      syncing_ = true;
      tree_->SelectItem(id);
      syncing_ = false;
    }
    const fs::path path = d ? d->node->path : ws_->root();
    const bool folder = !d || d->node->is_folder;
    const fs::path in = folder ? path : path.parent_path();
    enum { kOpen = 1, kHttp, kGrpc, kFolder, kRename, kDup, kDelete, kCopyPath, kReveal, kCollapse, kCurl, kImport, kSortAz };
    wxMenu m;
    if (!folder) {
      m.Append(kOpen, "Open in tab\tEnter");
      m.AppendSeparator();
    }
    m.Append(kHttp, "New HTTP request");
    m.Append(kGrpc, "New gRPC request");
    m.Append(kFolder, "New folder");
    m.Append(kImport, wxString::FromUTF8("Import cURL here\xE2\x80\xA6"));
    if (d) {
      m.AppendSeparator();
      m.Append(kRename, "Rename\tF2");
      if (!folder) m.Append(kDup, "Duplicate\tCtrl+D");
      m.Append(kDelete, "Delete\tDel");
    }
    m.AppendSeparator();
    if (!folder) m.Append(kCurl, d && d->node->kind == RequestKind::Grpc ? "Copy as grpcurl" : "Copy as cURL");
    m.Append(kCopyPath, "Copy path");
    m.Append(kReveal, "Reveal in File Explorer");
    if (folder) m.Append(kCollapse, "Collapse all");
    if (folder && order_.count(path)) m.Append(kSortAz, "Sort A-Z (forget the dragged order)");
    const int choice = tree_->GetPopupMenuSelectionFromUser(m);
    // Nothing that changes the tree was chosen: give the highlight back to the active request.
    if (choice != kRename && choice != kDelete && choice != kDup && choice != kOpen && choice != kCollapse &&
        !active_file_.empty() &&
        active_file_ != path)
      reveal_active();
    switch (choice) {
      case kOpen: open_tab(path); break;
      case kHttp: new_request_in(RequestKind::Http, in); break;
      case kGrpc: new_request_in(RequestKind::Grpc, in); break;
      case kFolder: new_folder_in(in); break;
      case kRename: if (id.IsOk()) edit_label(id); break;
      case kDup: duplicate_item(path); break;
      case kDelete: delete_item(id); break;
      case kCopyPath: copy_text(path.wstring()); break;
      case kCurl: copy_curl(path); break;
      case kImport: import_curl(in); break;
      case kReveal: reveal_in_explorer(path); break;
      case kSortAz:
        order_.erase(path);
        save_order();
        rebuild();
        break;
      case kCollapse:
        expanded_.clear();
        save_expanded();
        rebuild();
        break;
      default: break;
    }
  }

  // ---------------------------------------------------------------- workspaces

  static fs::path last_workspace() {
    wxString s;
    if (wxConfigBase* c = wxConfigBase::Get(); c && c->Read("Workspace/Last", &s) && !s.empty())
      return fs::path(s.ToStdWstring());
    return {};
  }

  static std::vector<fs::path> recent_workspaces() {
    std::vector<fs::path> out;
    wxString s;
    if (wxConfigBase* c = wxConfigBase::Get(); c && c->Read("Workspace/Recent", &s)) {
      wxStringTokenizer tk(s, "\n");
      while (tk.HasMoreTokens()) {
        const wxString t = tk.GetNextToken();
        if (!t.empty()) out.push_back(fs::path(t.ToStdWstring()));
      }
    }
    return out;
  }

  static void remember_workspace(const fs::path& root) {
    wxConfigBase* c = wxConfigBase::Get();
    if (!c) return;
    auto recent = recent_workspaces();
    std::erase_if(recent, [&](const fs::path& p) { return p == root; });
    recent.insert(recent.begin(), root);
    if (recent.size() > kMaxRecent) recent.resize(kMaxRecent);
    wxString s;
    for (const auto& p : recent) s += wxString(p.wstring()) + "\n";
    c->Write("Workspace/Last", wxString(root.wstring()));
    c->Write("Workspace/Recent", s);
    c->Flush();
  }

  // "C:\...\scratchpad\cs5": the drive and the last folders, the middle elided.
  static wxString short_path(const wxString& p, size_t max) {
    if (p.length() <= max) return p;
    const size_t slash = p.find('\\');
    const size_t head = std::min<size_t>(slash == wxString::npos ? 3 : slash + 1, max / 3);
    return p.Left(head) + wxString::FromUTF8("\xE2\x80\xA6") + p.Right(max - head - 1);
  }

  static std::vector<wxString> distinct_labels(const std::vector<fs::path>& paths) {
    std::vector<std::vector<std::wstring>> parts;  // components, last first
    for (const auto& p : paths) {
      std::vector<std::wstring> v;
      for (fs::path q = p; q.has_relative_path() && q.has_filename(); q = q.parent_path()) v.push_back(q.filename().wstring());
      if (v.empty()) v.push_back(p.wstring());
      parts.push_back(std::move(v));
    }
    auto key = [&](size_t i, size_t depth) {
      std::wstring k;
      for (size_t d = 0; d <= depth && d < parts[i].size(); ++d) k += parts[i][d] + L"\\";
      return k;
    };
    std::vector<wxString> out;
    for (size_t i = 0; i < paths.size(); ++i) {
      size_t depth = 0;
      for (; depth + 1 < parts[i].size(); ++depth) {
        bool clash = false;
        for (size_t j = 0; j < paths.size() && !clash; ++j) clash = j != i && key(j, depth) == key(i, depth);
        if (!clash) break;
      }
      wxString label = parts[i][0];
      if (depth > 0) {
        wxString where;
        for (size_t d = depth; d >= 1; --d) where += wxString(parts[i][d]) + (d > 1 ? "\\" : "");
        label += wxString::FromUTF8("  \xC2\xB7  ") + where;
      }
      label.Replace("&", "&&");
      out.push_back(label);
    }
    return out;
  }

  void workspace_menu() {
    enum { kOpen = 1, kNew, kReveal, kReload, kRecent = 100 };
    wxMenu m;
    if (ws_) {
      m.Append(wxID_ANY, short_path(ws_->root().wstring(), 56))->Enable(false);
      m.AppendSeparator();
    }
    m.Append(kOpen, "Open workspace folder...");
    m.Append(kNew, "New workspace...");
    const auto recent = recent_workspaces();
    std::vector<fs::path> others;
    for (const auto& p : recent)
      if (!ws_ || p != ws_->root()) others.push_back(p);
    if (!others.empty()) {
      m.AppendSeparator();
      auto* hdr = m.Append(wxID_ANY, "Recent");
      hdr->Enable(false);
      // Several workspaces are often all called "ws": each label gets as many parent folders as
      // it takes to tell them apart ("ws  ·  cs5", "ws  ·  big"); the right column a short path.
      const auto labels = distinct_labels(others);
      for (size_t i = 0; i < others.size(); ++i)
        m.Append(kRecent + static_cast<int>(i),
                 wxString::Format("%s\t%s", labels[i], short_path(others[i].parent_path().wstring(), 40)));
    }
    if (ws_) {
      m.AppendSeparator();
      m.Append(kReveal, "Reveal workspace in File Explorer");
      m.Append(kReload, "Reload from disk");
    }
    const int id = ws_btn_->GetPopupMenuSelectionFromUser(m, wxPoint(0, ws_btn_->GetSize().y));
    if (id == kOpen || id == kNew) {
      const bool create = id == kNew;
      wxDirDialog dlg(this, create ? "Choose or create an empty folder for the new workspace" : "Open a workspace folder",
                      ws_ ? wxString(ws_->root().parent_path().wstring()) : wxString(),
                      wxDD_DEFAULT_STYLE | (create ? 0 : wxDD_DIR_MUST_EXIST));
      if (dlg.ShowModal() != wxID_OK) return;
      const fs::path dir(dlg.GetPath().ToStdWstring());
      std::error_code ec;
      if (!create && !fs::exists(dir / "watt.json", ec)) {
        if (!confirm(wxGetTopLevelParent(this), "Make this folder a workspace?",
                     wxString::Format("\"%s\" is not a watt workspace yet. This adds watt.json, .gitignore and "
                                      ".gitattributes; existing files are not touched.",
                                      dir.wstring()),
                     "Make workspace"))
          return;
      }
      switch_workspace(dir, true);
    } else if (id >= kRecent && id < kRecent + static_cast<int>(others.size())) {
      switch_workspace(others[static_cast<size_t>(id - kRecent)], true);
    } else if (id == kReveal && ws_) {
      wxExecute(wxString::Format("explorer.exe \"%s\"", ws_->root().wstring()), wxEXEC_ASYNC);
    } else if (id == kReload && ws_) {
      switch_workspace(ws_->root(), true);
    }
  }

  // Opens (or creates) dir on a worker and swaps it into the Workspace the frame owns: every
  // tab reads the workspace through that object at send time. Tabs of the old workspace's
  // files are closed (unsaved ones are kept, detached from their file).
  void switch_workspace(const fs::path& dir, bool user) {
    if (!ws_) return;
    ws_btn_->SetLabel(wxString::Format("Opening %s...", dir.filename().wstring()));
    std::weak_ptr<int> alive = alive_;
    const uint64_t gen = ++ws_gen_;
    std::thread([this, dir, alive, gen, user] {
      std::string err;
      std::unique_ptr<Workspace> ws;
      std::error_code ec;
      try {
        if (fs::exists(dir / "watt.json", ec)) {
          if (auto w = Workspace::open(dir, &err)) ws = std::make_unique<Workspace>(std::move(*w));
        } else {
          fs::create_directories(dir, ec);
          ws = std::make_unique<Workspace>(Workspace::create(dir));
        }
      } catch (const std::exception& e) {
        err = e.what();
      }
      auto* holder = ws.release();
      if (wxAppConsole* app = wxTheApp) app->CallAfter([this, alive, gen, holder, err, dir, user] {
        std::unique_ptr<Workspace> w(holder);
        if (alive.expired() || gen != ws_gen_ || !ws_) return;
        if (!w) {
          update_ws_button();
          if (user)
            notify(wxGetTopLevelParent(this), "Could not open the workspace",
                   wxString::FromUTF8(u8(dir) + ": " + err));
          return;
        }
        // The old workspace's tabs are put away (hidden, with their edits and responses) and
        // the new one's come back: no tab is built or destroyed, so this costs a few ms.
        const bool other = !w->root().empty() && w->root() != ws_->root();
        const fs::path old_root = ws_->root();
        *ws_ = std::move(*w);
        if (other) {
          swap_tabs(old_root, ws_->root());
          if (!filter_->IsEmpty()) {  // a filter typed for the old collection
            filter_->ChangeValue("");
            filter_q_.clear();
            history_->set_filter({});
          }
        }
        workspace_ready();
        host_->environment_changed();
      });
    }).detach();
  }

  // ---------------------------------------------------------------- tab sets per workspace

  struct TabSet {
    std::vector<std::pair<wxWindow*, wxString>> pages;  // in tab order, with their titles
    wxWindow* selected = nullptr;
    std::vector<fs::path> lost;  // files whose tab was reused for the other workspace
  };

  // Hides the tabs of old_root (every tab that is not an empty start tab) and shows the ones
  // new_root had when it was left. At least one tab stays: a clean old tab is reused (blanked)
  // when the new workspace has none, its file reopened when the old one comes back.
  void swap_tabs(const fs::path& old_root, const fs::path& new_root) {
    wxAuiNotebook* nb = notebook();
    if (!nb) return;
    nb->Freeze();
    TabSet old;
    const int sel = nb->GetSelection();
    for (size_t i = 0; i < nb->GetPageCount(); ++i) {
      auto* t = dynamic_cast<RequestTab*>(nb->GetPage(i));
      if (!t || t->pristine()) continue;
      old.pages.emplace_back(t, nb->GetPageText(i));
      if (static_cast<int>(i) == sel) old.selected = t;
    }
    TabSet next;
    if (auto it = tab_sets_.find(new_root); it != tab_sets_.end()) {
      next = std::move(it->second);
      tab_sets_.erase(it);
    }
    for (auto& [w, title] : next.pages) nb->AddPage(w, title, false);
    // Files of the new workspace whose tab was lent to the old one last time: lend a clean
    // tab back (a few ms) instead of building a new one (~200 ms each).
    std::vector<fs::path> reopen;
    for (const auto& f : next.lost) {
      std::error_code ec;
      if (!fs::exists(f, ec)) continue;
      RequestTab* loan = nullptr;
      for (int pass = 0; pass < 2 && !loan; ++pass)
        for (auto& pg : old.pages) {
          auto* t = static_cast<RequestTab*>(pg.first);
          if (!t->dirty() && !t->in_flight() && (pass == 1 || pg.first != old.selected)) {
            loan = t;
            break;
          }
        }
      std::optional<SavedRequest> r;
      if (loan) r = ws_->load_request(f);
      if (!r) {
        reopen.push_back(f);
        continue;
      }
      if (!loan->file().empty()) old.lost.push_back(loan->file());
      std::erase_if(old.pages, [&](const auto& pg) { return pg.first == loan; });
      if (old.selected == loan) old.selected = nullptr;
      loan->set_request(std::move(*r), false);
      if (const int i = nb->GetPageIndex(loan); i >= 0) nb->SetPageText(static_cast<size_t>(i), loan->title());
    }
    for (auto& [w, title] : old.pages) {
      const int i = nb->GetPageIndex(w);
      if (i >= 0) nb->RemovePage(static_cast<size_t>(i));
      w->Hide();
    }
    if (nb->GetPageCount() == 0) {
      // Nothing to show: reuse a clean tab of the old workspace as the start tab.
      RequestTab* carrier = nullptr;
      for (auto it = old.pages.rbegin(); it != old.pages.rend(); ++it) {
        auto* t = static_cast<RequestTab*>(it->first);
        if (!t->dirty() && !t->in_flight() && (it->first == old.selected || !carrier)) carrier = t;
      }
      if (carrier) {
        if (!carrier->file().empty()) old.lost.push_back(carrier->file());
        std::erase_if(old.pages, [&](const auto& p) { return p.first == carrier; });
        if (old.selected == carrier) old.selected = nullptr;
        carrier->set_request(SavedRequest{}, false);
        nb->AddPage(carrier, carrier->title(), true);
      }
    }
    if (next.selected) {
      if (const int i = nb->GetPageIndex(next.selected); i >= 0) nb->SetSelection(static_cast<size_t>(i));
    } else if (nb->GetPageCount() && nb->GetSelection() < 0) {
      nb->SetSelection(0);
    }
    nb->Thaw();
    if (!old.pages.empty() || !old.lost.empty()) tab_sets_[old_root] = std::move(old);
    if (nb->GetPageCount() == 0) host_->new_request(RequestKind::Http, {});
    // No clean tab to lend: open them (one per turn).
    for (const auto& f : reopen)
      CallAfter([this, f, root = new_root] {
        std::error_code ec;
        if (ws_ && ws_->root() == root && fs::exists(f, ec)) host_->open_request(f, false);
      });
    if (auto* t = dynamic_cast<RequestTab*>(nb->GetCurrentPage())) select_file(t->file());
  }

  // Quitting with unsaved tabs hidden in another workspace: ask, as the frame does for its own.
  void on_close_tlw(wxCloseEvent& e) {
    size_t dirty = 0;
    for (const auto& [root, set] : tab_sets_)
      for (const auto& p : set.pages) dirty += static_cast<RequestTab*>(p.first)->dirty();
    if (dirty && e.CanVeto()) {
      const auto a = wxMessageBox(
          wxString::Format("%zu request(s) in other workspaces have unsaved changes. Quit anyway?", dirty), "watt",
          wxYES_NO | wxICON_QUESTION, wxGetTopLevelParent(this));
      if (a != wxYES) {
        e.Veto();
        return;
      }
    }
    e.Skip();
  }

  // ---------------------------------------------------------------- one window per workspace

  // .watt/window.lock holds the process id of the watt window that has the workspace open.
  // The pid of another live watt process that holds root, else 0.
  static unsigned long lock_owner(const fs::path& root) {
    std::string text;
    {
      std::ifstream in(root / ".watt" / "window.lock", std::ios::binary);
      if (!in) return 0;
      std::getline(in, text);
    }
    const unsigned long pid = std::strtoul(text.c_str(), nullptr, 10);
    if (pid == 0 || pid == GetCurrentProcessId()) return 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!h) return 0;
    bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    if (alive) {  // a reused pid of another program does not count
      wchar_t name[MAX_PATH];
      DWORD n = MAX_PATH;
      alive = QueryFullProcessImageNameW(h, 0, name, &n) &&
              std::wstring(name, n).find(L"watt") != std::wstring::npos;
    }
    CloseHandle(h);
    return alive ? pid : 0;
  }

  void take_lock() {
    if (!ws_) return;
    const fs::path root = ws_->root();
    if (const unsigned long other = lock_owner(root)) {
      // After the workspace has been shown (the message box is modal).
      CallAfter([this, root, other] {
        notify(wxGetTopLevelParent(this), "Workspace open in another window",
               wxString::Format("%s is also open in another watt window (process %lu). Saving the same request "
                                "in both windows overwrites one with the other.",
                                root.filename().wstring(), other));
      });
      return;
    }
    std::error_code ec;
    if (!fs::is_directory(root / ".watt", ec)) return;
    std::ofstream out(root / ".watt" / "window.lock", std::ios::binary | std::ios::trunc);
    out << GetCurrentProcessId() << std::endl;
    locked_root_ = root;
  }

  void release_lock() {
    if (locked_root_.empty()) return;
    std::string text;
    {
      std::ifstream in(locked_root_ / ".watt" / "window.lock", std::ios::binary);
      if (in) std::getline(in, text);
    }
    std::error_code ec;
    if (std::strtoul(text.c_str(), nullptr, 10) == GetCurrentProcessId())
      fs::remove(locked_root_ / ".watt" / "window.lock", ec);
    locked_root_.clear();
  }

  // ---------------------------------------------------------------- sidebar width

  // The width the user dragged the sidebar to comes back after a restart (saved as it changes,
  // so a crash or kill keeps it too).
  void restore_width() {
    long w = 0;
    if (wxConfigBase* c = wxConfigBase::Get(); !c || !c->Read("Sidebar/Width", &w)) return;
    w = FromDIP(static_cast<int>(w));
    if (w < FromDIP(160) || std::abs(w - GetSize().x) < 4) return;
    wxAuiManager* m = wxAuiManager::GetManager(this);
    if (!m) return;
    wxAuiPaneInfo& pane = m->GetPane(this);
    if (!pane.IsOk()) return;
    restoring_width_ = true;
    pane.Hide();
    m->Update();
    pane.Show().BestSize(wxSize(static_cast<int>(w), -1));
    m->Update();
    restoring_width_ = false;
  }

  void width_changed() {
    if (collapsed_ || restoring_width_ || !width_restored_) return;
    const int w = GetSize().x;
    if (w < FromDIP(160)) return;
    width_timer_.StartOnce(400);
  }

  void workspace_ready() {
    release_lock();
    take_lock();
    remember_workspace(ws_->root());
    prune_snapshots();
    update_ws_button();
    if (wxWindow* tlw = wxGetTopLevelParent(this))
      static_cast<wxTopLevelWindow*>(tlw)->SetTitle(wxString::Format("watt - %s", ws_->root().wstring()));
    tree_root_ = CollectionNode{};
    search_keys_.clear();
    meta_.clear();
    name_cache_ = std::make_shared<NameCache>(ws_->root());
    load_expanded();
    load_order();
    history_->set_entries({});
    auto_select_env();
    update_env_choice();
    rescan();
    rebuild();
    load_history();
  }

  void update_ws_button() {
    if (!ws_) return;
    wxString name = ws_->root().filename().wstring();
    if (name.empty()) name = ws_->root().wstring();
    ws_btn_->SetLabel(name + wxString::FromUTF8("  \xE2\x96\xBE"));
    // No tooltip: a multi-line path covered the environment switcher and the filter whenever the
    // pointer rested on the header. The menu shows the full path instead.
    ws_btn_->UnsetToolTip();
    content_->Layout();
  }

  // ---------------------------------------------------------------- environments

  void update_env_choice() {
    env_names_.clear();
    size_t unreadable = 0;
    wxString label = "No environment";
    bool active = false;
    if (ws_) {
      for (const auto& e : ws_->environments()) {
        size_t bad = 0;
        for (const auto& [k, v] : e.unreadable_secrets) bad += e.vars.count(k) == 0;
        unreadable += bad;
        env_names_.push_back(e.name);
        if (e.name == ws_->active_env) {
          label = wxString::FromUTF8(e.name);
          active = true;
          if (bad) label += wxString::FromUTF8("  \xE2\x9A\xA0");
        }
      }
    }
    // A coloured dot marks an active environment, like the env pills of Postman / Insomnia.
    const wxString shown = (active ? wxString::FromUTF8("\xE2\x97\x8F  ") : wxString("    ")) + label +
                           wxString::FromUTF8("  \xE2\x96\xBE");
    if (env_menu_btn_->GetLabel() != shown) env_menu_btn_->SetLabel(shown);
    env_menu_btn_->SetForegroundColour(active ? text_colour() : dim_colour());
    env_menu_btn_->Enable(ws_ != nullptr);
    env_btn_->Enable(ws_ != nullptr);
    const wxString warn =
        unreadable ? wxString::Format(wxString::FromUTF8("\xE2\x9A\xA0 %zu secret%s cannot be decrypted here: re-enter"),
                                      unreadable, unreadable == 1 ? "" : "s")
                   : wxString();
    if (env_warn_->GetLabel() != warn || env_warn_->IsShown() != !warn.empty()) {
      env_warn_->SetLabel(warn);
      env_warn_->SetToolTip(
          "Secret values are encrypted for one Windows account on one machine (DPAPI). These were sealed by "
          "another user or PC. Click to open the environment editor and enter them again.");
      env_warn_->Show(!warn.empty());
      content_->Layout();
    }
  }

  void env_menu() {
    if (!ws_) return;
    wxMenu m;
    enum { kNone = 1, kManage = 2, kEnv = 100 };
    m.AppendRadioItem(kNone, "No environment")->Check(ws_->active_env.empty());
    for (size_t i = 0; i < env_names_.size(); ++i)
      m.AppendRadioItem(kEnv + static_cast<int>(i), wxString::FromUTF8(env_names_[i]))
          ->Check(env_names_[i] == ws_->active_env);
    m.AppendSeparator();
    m.Append(kManage, wxString::FromUTF8("Manage environments\xE2\x80\xA6"));
    const int id = env_menu_btn_->GetPopupMenuSelectionFromUser(m, wxPoint(0, env_menu_btn_->GetSize().y));
    if (id == kManage) edit_envs();
    else if (id == kNone) env_chosen("");
    else if (id >= kEnv && id < kEnv + static_cast<int>(env_names_.size()))
      env_chosen(env_names_[static_cast<size_t>(id - kEnv)]);
  }

  void env_chosen(const std::string& name) {
    if (!ws_ || ws_->active_env == name) return;
    const std::string old = ws_->active_env;
    ws_->active_env = name;
    std::string err;
    if (!ws_->save_settings(&err)) {
      ws_->active_env = old;
      update_env_choice();
      notify(wxGetTopLevelParent(this), "Could not save the active environment", wxString::FromUTF8(err));
      return;
    }
    if (wxConfigBase* c = wxConfigBase::Get()) c->Write("Sidebar/EnvChosen/" + ws_key(ws_->root()), true);
    update_env_choice();
    env_changed();
  }

  void env_changed() {
    history_->relabel();  // resolved targets
    host_->environment_changed();
  }

  // A workspace that defines environments but has none active: pick one once ("local" /
  // "dev" first), so {{vars}} resolve out of the box. An explicit "No environment" sticks.
  void auto_select_env() {
    if (!ws_ || !ws_->active_env.empty() || ws_->environments().empty()) return;
    wxConfigBase* c = wxConfigBase::Get();
    const wxString key = "Sidebar/EnvChosen/" + ws_key(ws_->root());
    if (!c || c->ReadBool(key, false)) return;
    auto choose = [this]() -> std::string {
      for (const char* pref : {"local", "dev", "development"})
        for (const auto& e : ws_->environments())
          if (ascii_lower(e.name) == pref) return e.name;
      return ws_->environments().front().name;
    };
    ws_->active_env = choose();
    if (!ws_->save_settings(nullptr)) {
      ws_->active_env.clear();
      return;
    }
    c->Write(key, true);
    CallAfter([this] { host_->environment_changed(); });
  }

  void edit_envs() {
    if (!ws_) return;
    if (edit_environments(wxGetTopLevelParent(this), *ws_)) env_changed();
    update_env_choice();
  }

  // ---------------------------------------------------------------- quick open

  void touch_recent(const fs::path& f) {
    std::erase_if(recent_files_, [&](const fs::path& p) { return p == f; });
    recent_files_.push_front(f);
    if (recent_files_.size() > 50) recent_files_.pop_back();
  }

  void gather(const CollectionNode& n, const std::string& folder, std::vector<QuickItem>& out) const {
    for (const auto& c : n.children) {
      if (c.is_folder) {
        gather(c, folder.empty() ? c.name : folder + "/" + c.name, out);
        continue;
      }
      QuickItem it;
      it.file = c.path;
      it.name = c.name;
      it.folder = folder;
      it.kind = c.kind;
      it.method = c.method;
      it.badge = c.badge;
      out.push_back(std::move(it));
    }
  }

  void quick_open_dialog() {
    if (!ws_ || quick_open_shown_) return;
    std::vector<QuickItem> items;
    gather(tree_root_, "", items);
    std::map<fs::path, int> rank;
    int k = 0;
    for (const auto& p : recent_files_) rank.emplace(p, k++);
    for (auto& it : items)
      if (auto r = rank.find(it.file); r != rank.end()) it.recent = r->second;
    quick_open_shown_ = true;
    const fs::path chosen = quick_open(this, std::move(items));
    quick_open_shown_ = false;
    if (chosen.empty()) return;
    open_preview(chosen);
  }

  // ---------------------------------------------------------------- names, history labels

  // meta_ changed: rebuild the method+URL -> name index that labels history rows.
  void meta_changed() {
    search_keys_.clear();
    name_by_key_.clear();
    path_by_key_.clear();
    paths_by_key_.clear();
    path_by_id_.clear();
    std::set<std::string> ambiguous, ambiguous_ids;
    for (const auto& [path, m] : meta_) {
      if (!m.id.empty() && !path_by_id_.emplace(m.id, path).second) ambiguous_ids.insert(m.id);
      if (m.url.empty()) continue;
      const CollectionNode* n = find_node(tree_root_, path);
      std::string key;
      if (n && n->kind == RequestKind::Grpc) key = "grpc " + m.url;
      else if (n && n->badge) key = ascii_lower(n->method.empty() ? std::string("GET") : n->method) + " " + m.url;
      else continue;
      const std::string name = m.name.empty() ? (n ? n->name : std::string()) : m.name;
      auto [it, inserted] = name_by_key_.emplace(key, name);
      if (!inserted && it->second != name) it->second.clear();  // two requests send the same thing
      if (!path_by_key_.emplace(key, path).second) ambiguous.insert(key);
      paths_by_key_.emplace(key, std::make_pair(name, path));
    }
    for (const auto& k : ambiguous) path_by_key_.erase(k);
    for (const auto& k : ambiguous_ids) path_by_id_.erase(k);  // a copied file kept the id
    history_->relabel();
  }

  // The saved request a history entry was sent from: the live entry's own file, else the file
  // with the request id the entry stored (so an entry sent with unsaved edits, or a resend from
  // its snapshot tab, still finds it), else the one saved request that sends the same method +
  // URL (gRPC: method). Empty when none or ambiguous.
  fs::path history_file(const HistoryEntry& e) const {
    if (!e.request.file.empty() && find_node(tree_root_, e.request.file)) return e.request.file;
    if (const std::string id = history_id(e); !id.empty())
      if (auto it = path_by_id_.find(id); it != path_by_id_.end()) return it->second;
    if (!e.request.file.empty())
      if (auto it = snapshot_origin_.find(e.request.file);
          it != snapshot_origin_.end() && !it->second.empty() && find_node(tree_root_, it->second))
        return it->second;
    const std::string key = history_key(e);
    if (auto it = path_by_key_.find(key); it != path_by_key_.end()) return it->second;
    // Several saved requests send this: the one with the entry's request name.
    const std::string name = history_name(e);
    if (name.empty()) return {};
    fs::path found;
    for (auto [it, end] = paths_by_key_.equal_range(key); it != end; ++it) {
      if (it->second.first != name) continue;
      if (!found.empty()) return {};  // same name too
      found = it->second.second;
    }
    return found;
  }

  // The name of the request an entry was sent as ("" when unknown), without a snapshot's
  // " @ 14:03".
  std::string history_name(const HistoryEntry& e) const {
    std::string n = e.request.name;
    if (n.empty())
      if (auto it = hist_names_.find(e.when_iso + "\x01" + e.summary); it != hist_names_.end()) n = it->second.name;
    return strip_snapshot_suffix(std::move(n));
  }

  std::string history_id(const HistoryEntry& e) const {
    if (!e.request.id.empty()) return e.request.id;
    if (auto it = hist_names_.find(e.when_iso + "\x01" + e.summary); it != hist_names_.end()) return it->second.id;
    return {};
  }

  fs::path snapshot_dir() const { return ws_ ? ws_->root() / ".watt" / "snapshots" : fs::path(); }
  bool is_snapshot(const fs::path& f) const { return ws_ && !f.empty() && f.parent_path() == snapshot_dir(); }
  // "Get user @ 14:03" / "Get user @ Nov 14 14:03" -> "Get user".
  static std::string strip_snapshot_suffix(std::string n) {
    const size_t at = n.rfind(" @ ");
    if (at != std::string::npos && n.size() >= at + 8 && n[n.size() - 3] == ':' &&
        std::isdigit(static_cast<unsigned char>(n.back())))
      n.resize(at);
    return n;
  }
  // One file per entry: the same row always opens (or focuses) the same snapshot.
  fs::path snapshot_file(const HistoryEntry& e) const {
    const std::string key = e.when_iso + "\x01" + e.summary;
    return snapshot_dir() /
           wxString::Format("%016llx.req.json", static_cast<unsigned long long>(std::hash<std::string>{}(key)))
               .ToStdWstring();
  }

  // History click: the request exactly as the entry sent it, in the preview tab (a single
  // click) or a kept tab (double click, Enter), titled "Get user @ 14:03" so it is never taken
  // for the saved request. Clicking the row again focuses the tab that shows it instead of
  // opening another. The saved request it came from is highlighted in the tree (not opened:
  // "Open saved request" in the row's menu does that). The snapshot is written once, on a
  // worker; after that a click costs what a tree click costs.
  void open_history_entry(const HistoryEntry& e, bool permanent, bool open_saved) {
    if (!ws_) return;
    const fs::path origin = history_file(e);
    if (open_saved) {
      if (origin.empty()) return;
      if (permanent) {
        open_tab(origin);
      } else {
        host_->open_request(origin, true);
        select_file(origin);
      }
      return;
    }
    reveal_in_tree(origin);
    const fs::path snap = snapshot_file(e);
    if (snapshot_origin_.count(snap)) {
      host_->open_request(snap, !permanent);
      return;
    }
    std::string base = origin.empty() ? std::string() : meta_name(origin);
    if (base.empty()) base = history_name(e);
    const std::string title = base.empty() ? std::string() : base + " @ " + history_when_label(e.when_iso);
    auto copy = std::make_shared<Workspace>(*ws_);
    std::weak_ptr<int> alive = alive_;
    const uint64_t gen = ++open_gen_;
    const bool loaded = !e.request.id.empty() || !e.request.http.url.empty() || !e.request.grpc_method.empty();
    std::thread([this, copy, e, alive, gen, snap, origin, title, loaded, permanent] {
      std::optional<SavedRequest> r;
      if (loaded) r = e.request;
      else r = copy->history_request(e);
      bool ok = false;
      if (r) {
        r->file = snap;
        if (!title.empty()) r->name = title;
        std::string err;
        ok = copy->save_request(*r, &err);
      }
      const bool found = r.has_value();
      if (wxAppConsole* app = wxTheApp) app->CallAfter([this, alive, gen, snap, origin, ok, found, permanent] {
        if (alive.expired() || gen != open_gen_) return;
        if (!found) {
          load_history();  // the segment was trimmed under the entry: list again
          return;
        }
        if (!ok) {
          notify(wxGetTopLevelParent(this), "Could not open the history entry",
                 wxString::FromUTF8("Writing " + u8(snap) + " failed."));
          return;
        }
        snapshot_origin_[snap] = origin;
        host_->open_request(snap, !permanent);
      });
    }).detach();
  }

  std::string meta_name(const fs::path& f) const {
    if (auto m = meta_.find(f); m != meta_.end() && !m->second.name.empty()) return m->second.name;
    if (const CollectionNode* n = find_node(tree_root_, f)) return n->name;
    return {};
  }

  // Highlights a saved request (scrolled into the middle of the tree) without opening it.
  void reveal_in_tree(const fs::path& f) {
    if (f.empty()) return;
    wxTreeItemId id = item_for(f, true);
    if (!id.IsOk()) return;
    syncing_ = true;
    if (tree_->GetSelection() != id) tree_->SelectItem(id);
    centre_item(id);
    syncing_ = false;
  }

  // A Ctrl+S in a snapshot tab saves a new request next to the one it came from ("Get user 2"),
  // never over the snapshot or the original: the tab moves to the new file.
  bool promote_snapshot(const SavedRequest& r) {
    if (!is_snapshot(r.file)) return false;
    fs::path origin;
    if (auto it = snapshot_origin_.find(r.file); it != snapshot_origin_.end()) origin = it->second;
    const fs::path folder = origin.empty() ? ws_->root() : origin.parent_path();
    std::string name = strip_snapshot_suffix(r.name);
    if (name.empty()) name = "Request";
    SavedRequest c = r;
    c.id.clear();
    c.file = free_request_file(folder, name, &c.name);
    std::string err;
    if (!ws_->save_request(c, &err)) {
      notify(wxGetTopLevelParent(this), "Could not save", wxString::FromUTF8(err));
      return true;
    }
    std::error_code ec;
    fs::remove(r.file, ec);
    snapshot_origin_.erase(r.file);
    if (RequestTab* t = tab_for(r.file)) t->set_request(c, false);
    if (name_cache_) name_cache_->note(c.file, c.name, c.kind == RequestKind::Grpc ? c.grpc_method : c.http.url, c.id);
    if (folder != ws_->root()) expanded_.insert(folder);
    pending_select_ = c.file;
    rescan_then_select();
    return true;
  }

  wxAuiNotebook* notebook() const {
    auto* top = wxGetTopLevelParent(const_cast<Sidebar*>(this));
    if (!top) return nullptr;
    std::function<wxAuiNotebook*(wxWindow*)> find = [&](wxWindow* w) -> wxAuiNotebook* {
      if (auto* nb = wxDynamicCast(w, wxAuiNotebook)) return nb;
      for (wxWindow* c : w->GetChildren())
        if (auto* nb = find(c)) return nb;
      return nullptr;
    };
    return find(top);
  }

  RequestTab* tab_for(const fs::path& f) const {
    wxAuiNotebook* nb = notebook();
    if (!nb) return nullptr;
    for (size_t i = 0; i < nb->GetPageCount(); ++i)
      if (auto* t = dynamic_cast<RequestTab*>(nb->GetPage(i)); t && t->file() == f) return t;
    return nullptr;
  }

  // Old snapshots (no tab is restored across a restart, so none is in use): removed on a worker.
  void prune_snapshots() {
    const fs::path dir = snapshot_dir();
    std::thread([dir] {
      std::error_code ec;
      const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24);
      for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2) && it->last_write_time(e2) < cutoff) fs::remove(it->path(), e2);
      }
    }).detach();
  }

  void describe_history(const HistoryEntry& e, std::string* name, std::string* target) {
    // The saved request's current name (it may have been renamed since), else the name it was
    // sent under.
    if (const fs::path f = history_file(e); !f.empty()) *name = meta_name(f);
    if (name->empty())
      if (auto it = name_by_key_.find(history_key(e)); it != name_by_key_.end()) *name = it->second;
    if (name->empty()) *name = history_name(e);
    if (!ws_) return;
    // The target with {{vars}} resolved by the active environment, without the scheme.
    std::string t = e.summary;
    if (e.kind == RequestKind::Http) {
      const size_t sp = t.find(' ');
      if (sp != std::string::npos) t.erase(0, sp + 1);
    } else if (!t.empty() && t[0] == '/') {
      t.erase(0, 1);
    }
    if (t.find("{{") != std::string::npos) t = ws_->expand(t);
    *target = strip_scheme(std::move(t));
  }

  // ---------------------------------------------------------------- manual order

  // Folders first, then requests. Within each, the order the user dragged them into (per
  // folder, by file name), the rest after it in natural order (case-insensitive, 2 before 10).
  void sort_kids(const fs::path& folder, std::vector<const CollectionNode*>& kids) const {
    const auto it = order_.find(folder);
    const std::vector<std::wstring>* ord = it == order_.end() ? nullptr : &it->second;
    auto rank = [&](const CollectionNode* n) -> size_t {
      if (!ord) return SIZE_MAX;
      const std::wstring f = n->path.filename().wstring();
      for (size_t i = 0; i < ord->size(); ++i)
        if ((*ord)[i] == f) return i;
      return SIZE_MAX;
    };
    std::vector<std::pair<size_t, const CollectionNode*>> v;
    v.reserve(kids.size());
    for (const CollectionNode* k : kids) v.emplace_back(rank(k), k);
    std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
      if (a.second->is_folder != b.second->is_folder) return a.second->is_folder;
      if (a.first != b.first) return a.first < b.first;
      return natural_less(a.second->name, b.second->name);
    });
    for (size_t i = 0; i < v.size(); ++i) kids[i] = v[i].second;
  }

  // Drag a request onto a sibling (a folder onto a sibling folder with Alt): it takes that
  // sibling's place.
  void reorder_item(const fs::path& from, const fs::path& onto) {
    const fs::path folder = from.parent_path();
    const CollectionNode* parent = find_folder(tree_root_, folder);
    if (!parent) return;
    std::vector<const CollectionNode*> kids;
    for (const auto& c : parent->children) kids.push_back(&c);
    sort_kids(folder, kids);
    std::vector<std::wstring> names;
    size_t from_i = SIZE_MAX, onto_i = SIZE_MAX;
    for (const CollectionNode* k : kids) {
      if (k->path == from) from_i = names.size();
      if (k->path == onto) onto_i = names.size();
      names.push_back(k->path.filename().wstring());
    }
    if (from_i == SIZE_MAX || onto_i == SIZE_MAX || from_i == onto_i) return;
    const std::wstring moved = names[from_i];
    names.erase(names.begin() + static_cast<std::ptrdiff_t>(from_i));
    names.insert(names.begin() + static_cast<std::ptrdiff_t>(onto_i), moved);
    order_[folder] = std::move(names);
    save_order();
    rebuild();
    syncing_ = true;
    if (wxTreeItemId id = item_for(from, true); id.IsOk()) {
      tree_->SelectItem(id);
      tree_->EnsureVisible(id);
    }
    syncing_ = false;
  }

  static const CollectionNode* find_folder(const CollectionNode& n, const fs::path& p) {
    if (n.path == p) return &n;
    for (const auto& c : n.children)
      if (c.is_folder && (c.path == p || is_under(p, c.path)))
        if (const CollectionNode* r = find_folder(c, p)) return r;
    return nullptr;
  }

  // A renamed / moved item keeps its place (renamed in its folder's list).
  void remap_order(const fs::path& from, const fs::path& to) {
    bool changed = false;
    if (auto it = order_.find(from.parent_path()); it != order_.end() && from.parent_path() == to.parent_path()) {
      for (auto& n : it->second)
        if (n == from.filename().wstring()) {
          n = to.filename().wstring();
          changed = true;
        }
    }
    std::map<fs::path, std::vector<std::wstring>> next;
    for (auto& [k, v] : order_) {
      if (k == from || is_under(k, from)) {
        next[to / k.lexically_relative(from)] = std::move(v);
        changed = true;
      } else {
        next[k] = std::move(v);
      }
    }
    order_ = std::move(next);
    if (changed) save_order();
  }

  void save_order() {
    wxConfigBase* c = wxConfigBase::Get();
    if (!c || !ws_) return;
    wxString s;
    for (const auto& [folder, names] : order_) {
      if (names.empty() || !(folder == ws_->root() || is_under(folder, ws_->root()))) continue;
      s += folder == ws_->root() ? wxString(".") : wxString(folder.lexically_relative(ws_->root()).wstring());
      for (const auto& n : names) s += "\t" + wxString(n);
      s += "\n";
    }
    c->Write("Sidebar/Order/" + ws_key(ws_->root()), s);
  }

  void load_order() {
    order_.clear();
    wxConfigBase* c = wxConfigBase::Get();
    wxString s;
    if (!c || !ws_ || !c->Read("Sidebar/Order/" + ws_key(ws_->root()), &s)) return;
    wxStringTokenizer lines(s, "\n");
    while (lines.HasMoreTokens()) {
      wxStringTokenizer f(lines.GetNextToken(), "\t");
      if (!f.HasMoreTokens()) continue;
      const wxString rel = f.GetNextToken();
      const fs::path folder = rel == "." ? ws_->root() : (ws_->root() / fs::path(rel.ToStdWstring())).lexically_normal();
      std::vector<std::wstring> names;
      while (f.HasMoreTokens()) names.push_back(f.GetNextToken().ToStdWstring());
      order_[folder] = std::move(names);
    }
  }

  // ---------------------------------------------------------------- expansion state

  void save_expanded() {
    wxConfigBase* c = wxConfigBase::Get();
    if (!c || !ws_) return;
    wxString s;
    for (const auto& p : expanded_)
      if (is_under(p, ws_->root())) s += wxString(p.lexically_relative(ws_->root()).wstring()) + "\n";
    c->Write("Sidebar/Expanded/" + ws_key(ws_->root()), s);
  }

  void load_expanded() {
    expanded_.clear();
    wxConfigBase* c = wxConfigBase::Get();
    wxString s;
    if (!c || !ws_ || !c->Read("Sidebar/Expanded/" + ws_key(ws_->root()), &s)) return;
    wxStringTokenizer tk(s, "\n");
    while (tk.HasMoreTokens()) {
      const wxString t = tk.GetNextToken();
      if (!t.empty()) expanded_.insert((ws_->root() / fs::path(t.ToStdWstring())).lexically_normal());
    }
  }

  // ---------------------------------------------------------------- import / export

  void import_curl(const fs::path& folder_in) {
    if (!ws_) return;
    const fs::path folder = folder_in.empty() ? ws_->root() : folder_in;
    std::string cmd, name;
    wxWindow* top = wxGetTopLevelParent(this);
    if (!ask_curl(top, &cmd, &name) || cmd.empty()) return;
    std::string err;
    auto imp = parse_curl(cmd, &err);
    if (!imp) {
      notify(top, "Could not import the curl command", wxString::FromUTF8(err));
      return;
    }
    SavedRequest r;
    r.kind = RequestKind::Http;
    r.http = imp->http;
    r.form = imp->form;
    r.basic_auth = imp->basic_auth;
    r.body_file = imp->body_file;
    r.body_data = imp->body_data;
    r.header_files = imp->header_files;
    r.cookie_files = imp->cookie_files;
    if (name.empty()) {
      // "orders" from "https://host/api/orders?x=1" (the badge shows the method).
      std::string u = r.http.url;
      if (const size_t q = u.find_first_of("?#"); q != std::string::npos) u.resize(q);
      while (!u.empty() && u.back() == '/') u.pop_back();
      const size_t slash = u.find_last_of('/');
      std::string last = slash == std::string::npos ? u : u.substr(slash + 1);
      if (last.empty() || last.find(':') != std::string::npos) last = "request";
      name = last;
    }
    const fs::path file = free_request_file(folder, name, &r.name);
    r.file = file;
    if (!ws_->save_request(r, &err)) {
      notify(top, "Could not save the imported request", wxString::FromUTF8(err));
      return;
    }
    if (folder != ws_->root()) expanded_.insert(folder);
    pending_select_ = file;
    rescan_then_select();
    open_tab(file);
    if (!imp->warnings.empty()) {
      std::string msg;
      for (const auto& w : imp->warnings) msg += "\xE2\x80\xA2 " + w + "\n";
      notify(top, "Imported, with notes", wxString::FromUTF8(msg));
    }
  }

  // Copies a saved request as a curl (gRPC: grpcurl) command with {{vars}} resolved.
  void copy_curl(const fs::path& p) {
    if (!ws_) return;
    std::string err;
    auto r = ws_->load_request(p, &err);
    if (!r) {
      notify(wxGetTopLevelParent(this), "Could not read the request", wxString::FromUTF8(err));
      return;
    }
    const SavedRequest x = ws_->expanded(*r);
    copy_text(wxString::FromUTF8(x.kind == RequestKind::Grpc ? to_grpcurl_command(x) : to_curl_command(x)));
  }

  // ---------------------------------------------------------------- state

  SidebarHost* host_;
  Workspace* ws_ = nullptr;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);
  bool first_ws_ = true;
  fs::path locked_root_;  // the workspace whose window.lock this window wrote
  bool restoring_width_ = false, width_restored_ = false;
  wxTimer width_timer_;
  uint64_t scan_gen_ = 0, hist_gen_ = 0, ws_gen_ = 0;
  bool scanning_ = false;

  CollectionNode tree_root_;  // last scan
  CollectionNode view_root_;  // what the tree shows (tree_root_ or its filtered copy)
  std::set<fs::path> expanded_;
  std::string filter_q_;
  bool rebuilding_ = false, syncing_ = false, root_populated_ = false, sash_set_ = false;
  bool quick_open_shown_ = false;
  bool edit_ok_ = false;
  bool rebuild_after_edit_ = false;
  wxWindow* more_ = nullptr;  // the hover "..." button
  wxTreeItemId hover_item_;
  bool more_hot_ = false;
  unsigned tree_focus_req_ = 0;  // bumped when the keyboard moves to the tree on purpose
  bool reveal_active_ = false;  // the active tab's file was not in the tree yet
  int expand_budget_ = 0;     // rows a filtered rebuild may still open folders for
  fs::path open_after_edit_;  // a new / duplicated request: opened when its rename ends
  struct HistMeta {
    std::string name, id;
  };
  std::map<std::string, HistMeta> hist_names_;  // persisted history entry -> request name and id
  std::map<std::string, fs::path> path_by_id_;  // request id -> its file (saved requests only)
  // History snapshots (.watt/snapshots/<entry>.req.json): the request as an entry sent it,
  // opened like a saved file so the host reuses the preview tab and focuses a tab that already
  // shows it. -> the saved request it came from (empty when none).
  std::map<fs::path, fs::path> snapshot_origin_;
  std::map<fs::path, TabSet> tab_sets_;  // hidden tabs of the workspaces switched away from
  std::unique_ptr<std::pair<CollectionNode, MetaMap>> deferred_scan_;  // a scan pass held during an edit
  bool deferred_final_ = false;  // a label edit was asked for (F2, menu, new item)
  bool select_after_scan_ = false;
  fs::path drag_from_, pending_edit_, pending_select_;
  std::map<fs::path, std::vector<std::wstring>> order_;  // folder -> file names in the dragged order
  std::deque<fs::path> recent_files_;
  std::vector<std::string> env_names_;
  fs::path active_file_;
  std::shared_ptr<NameCache> name_cache_;
  MetaMap meta_;                                   // by path: request name and URL
  SearchKeys search_keys_;                         // filter text per node of tree_root_ (lazy)
  std::map<std::string, std::string> name_by_key_;  // "get {{base}}/x" -> request name ("" if ambiguous)
  std::map<std::string, fs::path> path_by_key_;     // same key -> its file (only when unique)
  std::multimap<std::string, std::pair<std::string, fs::path>> paths_by_key_;  // key -> (name, file), all
  uint64_t open_gen_ = 0;
  bool collapsed_ = false, user_sash_ = false;
  int expanded_width_ = 0;
  wxSize saved_min_ = wxDefaultSize;

  wxPanel* content_ = nullptr;
  wxPanel* rail_ = nullptr;
  wxPanel* edge_ = nullptr;
  wxButton* ws_btn_ = nullptr;
  wxButton* env_menu_btn_ = nullptr;
  wxButton* env_btn_ = nullptr;
  wxStaticText* env_warn_ = nullptr;
  wxTextCtrl* filter_ = nullptr;
  wxTimer filter_timer_;
  wxSplitterWindow* split_ = nullptr;
  wxStaticText* coll_label_ = nullptr;
  wxStaticText* hist_label_ = nullptr;
  wxButton* hist_expand_ = nullptr;
  bool hist_full_ = false;
  int hist_saved_sash_ = 0;
  wxWindow* top_pane_ = nullptr;
  bool hist_saved_user_ = false;
  wxTreeCtrl* tree_ = nullptr;
  wxStaticText* empty_ = nullptr;
  HistoryList* history_ = nullptr;
  wxStaticText* hist_empty_ = nullptr;
};

}  // namespace

SidebarPanel* make_sidebar(wxWindow* parent, SidebarHost* host) { return new Sidebar(parent, host); }

}  // namespace watt::app
