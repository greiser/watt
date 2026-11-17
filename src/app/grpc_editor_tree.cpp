#include "app/grpc_editor_tree.h"

#include <wx/imaglist.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/treectrl.h>
#ifdef __WXMSW__
#include <wx/msw/wrapcctl.h>
#endif

#include <algorithm>
#include <cctype>
#include <map>
#include <vector>

#include "app/grpc_editor_util.h"

namespace watt::app::grpced {
namespace {

class MethodData : public wxTreeItemData {
 public:
  explicit MethodData(size_t i) : index(i) {}
  size_t index;
};

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Puts the focus back on a text control that had it when the native tree took it.
class KeepFocus {
 public:
  explicit KeepFocus(wxTextCtrl* t) : t_(wxWindow::FindFocus() == t ? t : nullptr) {
    if (t_) t_->GetSelection(&from_, &to_);
  }
  ~KeepFocus() {
    if (t_ && wxWindow::FindFocus() != t_) {
      t_->SetFocus();
      t_->SetSelection(from_, to_);
    }
  }
  KeepFocus(const KeepFocus&) = delete;
  KeepFocus& operator=(const KeepFocus&) = delete;

 private:
  wxTextCtrl* t_;
  long from_ = 0, to_ = 0;
};

enum Img { kImgUnary, kImgServer, kImgClient, kImgBidi, kImgService };

}  // namespace

MethodTree::MethodTree(wxWindow* parent) : wxPanel(parent, wxID_ANY) {
  const Theme t = theme();
  filter_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
  filter_->SetHint(wxString::FromUTF8("Filter methods  (Ctrl+F, ↑↓ Enter)"));
  tree_ = new wxTreeCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                         wxTR_HIDE_ROOT | wxTR_NO_LINES | wxTR_FULL_ROW_HIGHLIGHT | wxTR_SINGLE | wxBORDER_NONE);
#ifdef __WXMSW__
  // Services are always expanded (no expander buttons), and long method names never add a
  // horizontal scrollbar to the narrow pane (it can be widened instead).
  {
    const HWND hw = static_cast<HWND>(tree_->GetHWND());
    const LONG_PTR st = ::GetWindowLongPtr(hw, GWL_STYLE);
    ::SetWindowLongPtr(hw, GWL_STYLE, (st & ~static_cast<LONG_PTR>(TVS_HASBUTTONS | TVS_LINESATROOT)) | TVS_NOHSCROLL);
  }
#endif
  empty_ = new wxStaticText(this, wxID_ANY, "");
  empty_->SetForegroundColour(t.muted);

  // Badges at physical pixel size (the image list is not DPI-scaled for us).
  const int bw = FromDIP(44), bh = FromDIP(16);
  const double fpx = FromDIP(9);
  // The badges are drawn once per size and theme (every new gRPC tab builds a tree).
  static std::vector<wxBitmap> cache;
  static int cache_w = 0, cache_h = 0;
  static bool cache_dark = false;
  if (cache.empty() || cache_w != bw || cache_h != bh || cache_dark != t.dark) {
    cache.clear();
    for (auto k : {GrpcCallKind::Unary, GrpcCallKind::ServerStreaming, GrpcCallKind::ClientStreaming, GrpcCallKind::Bidi})
      cache.push_back(make_badge(kind_short(k), kind_colour(k, t.dark), *wxWHITE, bw, bh, fpx));
    cache.push_back(make_badge("SVC", t.dark ? wxColour(70, 70, 70) : wxColour(225, 225, 225),
                               t.dark ? wxColour(200, 200, 200) : wxColour(80, 80, 80), bw, bh, fpx));
    cache_w = bw;
    cache_h = bh;
    cache_dark = t.dark;
  }
  auto* il = new wxImageList(bw, bh, false);
  for (const wxBitmap& b : cache) il->Add(b);
  tree_->AssignImageList(il);
  // The smallest indent comctl allows (it clamps 0 to the image width + 3). Anything below that
  // minimum shifts the top level right by the difference: a 35 px blank strip before the
  // services. At the minimum, services sit flush left and each method's badge lines up under
  // its service's name.
  tree_->SetIndent(0);

  auto* sz = new wxBoxSizer(wxVERTICAL);
  sz->Add(filter_, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
  sz->Add(empty_, 0, wxEXPAND | wxALL, FromDIP(6));
  sz->Add(tree_, 1, wxEXPAND);
  SetSizer(sz);
  tree_->Hide();

  filter_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { rebuild(); });
  // Enter picks the highlighted method (the first match when nothing is highlighted); Up / Down
  // move the highlight while typing continues in the filter; Escape clears the filter.
  filter_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) {
    auto sel = tree_->GetSelection();
    if (!sel.IsOk() || !dynamic_cast<MethodData*>(tree_->GetItemData(sel))) move_highlight(1);
    pick_selected(true);
  });
  filter_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
    switch (e.GetKeyCode()) {
      case WXK_DOWN: case WXK_NUMPAD_DOWN: move_highlight(1); return;
      case WXK_UP: case WXK_NUMPAD_UP: move_highlight(-1); return;
      case WXK_PAGEDOWN: move_highlight(8); return;
      case WXK_PAGEUP: move_highlight(-8); return;
      case WXK_ESCAPE:
        if (!filter_->IsEmpty()) {
          filter_->ChangeValue("");
          rebuild();
          return;
        }
        break;
      default: break;
    }
    e.Skip();
  });
  Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
    e.Skip();
    if (empty_->IsShown()) CallAfter([this] { wrap_empty(); });
  });
  tree_->Bind(wxEVT_TREE_SEL_CHANGED, &MethodTree::on_sel, this);
  tree_->Bind(wxEVT_TREE_ITEM_ACTIVATED, [this](wxTreeEvent& e) {
    auto* d = dynamic_cast<MethodData*>(tree_->GetItemData(e.GetItem()));
    if (!d) {
      e.Skip();
      return;
    }
    pick_selected(true);
  });
}

void MethodTree::set_methods(const std::vector<GrpcMethodInfo>& methods) {
  entries_.clear();
  entries_.reserve(methods.size());
  for (const auto& m : methods)
    entries_.push_back({m.service, m.method, m.full_path, lower(m.full_path), m.kind});
  std::stable_sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
    return a.service < b.service;
  });
  rebuild();
}

void MethodTree::set_empty_text(const wxString& text) {
  empty_text_ = text;
  if (entries_.empty()) {
    empty_raw_ = text;
    wrap_empty();
  }
}

void MethodTree::wrap_empty() {
  empty_->SetLabel(empty_raw_);
  const int w = GetClientSize().x - FromDIP(12);
  if (w > FromDIP(40)) empty_->Wrap(w);
  empty_->SetMinSize(wxSize(FromDIP(40), empty_->GetBestSize().y));
  Layout();
}

void MethodTree::set_stale(bool stale) {
  if (stale == stale_) return;
  stale_ = stale;
  const Theme t = theme();
  tree_->SetForegroundColour(stale ? t.muted : t.fg);
  tree_->SetToolTip(stale ? "The last schema that loaded; reloading failed or is still running" : "");
  tree_->Refresh();
}

void MethodTree::set_web(bool web) {
  if (web == web_) return;
  web_ = web;
  rebuild();
}

void MethodTree::clear_filter() {
  if (filter_->IsEmpty()) return;
  filter_->ChangeValue("");
  rebuild();
}

void MethodTree::move_highlight(int delta) {
  std::vector<wxTreeItemId> items;
  wxTreeItemIdValue c1, c2;
  const auto root = tree_->GetRootItem();
  if (!root.IsOk()) return;
  for (auto s = tree_->GetFirstChild(root, c1); s.IsOk(); s = tree_->GetNextChild(root, c1))
    for (auto m = tree_->GetFirstChild(s, c2); m.IsOk(); m = tree_->GetNextChild(s, c2)) items.push_back(m);
  if (items.empty()) return;
  KeepFocus keep(filter_);
  int at = -1;
  const auto sel = tree_->GetSelection();
  for (size_t i = 0; i < items.size(); ++i)
    if (sel.IsOk() && items[i] == sel) at = static_cast<int>(i);
  int to = at < 0 ? (delta > 0 ? 0 : static_cast<int>(items.size()) - 1) : at + delta;
  to = std::clamp(to, 0, static_cast<int>(items.size()) - 1);
  quiet_ = true;
  tree_->SelectItem(items[static_cast<size_t>(to)]);
  tree_->EnsureVisible(items[static_cast<size_t>(to)]);
  quiet_ = false;
}

void MethodTree::focus_filter() {
  filter_->SetFocus();
  filter_->SelectAll();
}

void MethodTree::rebuild() {
  const std::string q = lower(filter_->GetValue().utf8_string());
  // Typing in the filter must keep the focus there: the native tree takes it when its selected
  // item is deleted / re-selected, and the rest of the word then went to its type-ahead.
  KeepFocus keep(filter_);
  if (wxWindow::FindFocus() == filter_) {
    // Also after any focus change the native tree posts for itself.
    CallAfter([this, at = filter_->GetInsertionPoint()] {
      if (wxWindow::FindFocus() == tree_) {
        filter_->SetFocus();
        filter_->SetInsertionPoint(at);
      }
    });
  }
  quiet_ = true;
  tree_->Freeze();
  // Drop the selection first, so deleting the items never moves the focus to the tree.
  tree_->UnselectAll();
  tree_->DeleteAllItems();
  const auto root = tree_->AddRoot("");

  // Which entries to show, in what order. Unfiltered: by service as loaded. Filtered: ranked,
  // so the method the engineer most likely means is first and is what Enter picks:
  // exact method name, then method-name prefix, then method name contains, then a match in
  // the service name only. Services are ordered by their best method, methods within a service
  // by rank (stable, so equal ranks keep the schema order).
  std::vector<std::pair<int, size_t>> order;  // (rank, entry index)
  order.reserve(entries_.size());
  for (size_t i = 0; i < entries_.size(); ++i) {
    const Entry& e = entries_[i];
    if (q.empty()) {
      order.push_back({0, i});
      continue;
    }
    if (e.search.find(q) == std::string::npos) continue;
    const std::string m = lower(e.method);
    int rank = 4;
    if (m == q) rank = 0;
    else if (m.rfind(q, 0) == 0) rank = 1;
    else if (m.find(q) != std::string::npos) rank = 2;
    else if (lower(e.service).find(q) != std::string::npos) rank = 3;
    order.push_back({rank, i});
  }
  if (!q.empty()) {
    std::map<std::string, int> best;  // service -> best rank
    for (const auto& [r, i] : order) {
      auto [it, fresh] = best.try_emplace(entries_[i].service, r);
      if (!fresh) it->second = std::min(it->second, r);
    }
    std::stable_sort(order.begin(), order.end(), [&](const auto& a, const auto& b) {
      const Entry& ea = entries_[a.second];
      const Entry& eb = entries_[b.second];
      const int ba = best[ea.service], bb = best[eb.service];
      if (ba != bb) return ba < bb;
      if (ea.service != eb.service) return a.second < b.second;  // first-seen order of services
      return a.first < b.first;
    });
    // Equal-best services keep their schema order: the comparator above compares entry index
    // across services, which is the schema order since entries_ is sorted by service.
  }

  std::string cur_service;
  wxTreeItemId svc, sel, first_method;
  size_t shown = 0;
  for (const auto& [rank, i] : order) {
    const Entry& e = entries_[i];
    if (!svc.IsOk() || e.service != cur_service) {
      cur_service = e.service;
      svc = tree_->AppendItem(root, wxString::FromUTF8(e.service), kImgService);
      tree_->SetItemBold(svc, true);
    }
    const auto item = tree_->AppendItem(svc, wxString::FromUTF8(e.method), static_cast<int>(e.kind), -1,
                                        new MethodData(i));
    if (web_ && (e.kind == GrpcCallKind::ClientStreaming || e.kind == GrpcCallKind::Bidi)) {
      tree_->SetItemText(item, wxString::FromUTF8(e.method + "  (no gRPC-Web)"));
      tree_->SetItemTextColour(item, theme().muted);
    }
    if (!first_method.IsOk()) first_method = item;
    if (e.full_path == selected_) sel = item;
    ++shown;
  }
  // While filtering, the best match is highlighted (Enter picks it, arrows move from it), not
  // the method that happened to be open before.
  if (!q.empty() && first_method.IsOk()) sel = first_method;
  tree_->ExpandAll();
  if (sel.IsOk()) {
    tree_->SelectItem(sel);
    tree_->EnsureVisible(sel);
  } else {
    wxTreeItemIdValue c;
    auto first = tree_->GetFirstChild(root, c);
    if (first.IsOk()) tree_->ScrollTo(first);
  }
  tree_->Thaw();
  quiet_ = false;
  const bool any = shown > 0;
  if (!any) empty_raw_ = entries_.empty() ? empty_text_ : "No method matches \"" + filter_->GetValue() + "\"";
  empty_->Show(!any);
  tree_->Show(any);
  Layout();
  if (!any) wrap_empty();
}

void MethodTree::select(const std::string& full_path) {
  selected_ = full_path;
  KeepFocus keep(filter_);
  quiet_ = true;
  wxTreeItemIdValue c1, c2;
  const auto root = tree_->GetRootItem();
  bool found = false;
  if (root.IsOk()) {
    for (auto s = tree_->GetFirstChild(root, c1); s.IsOk() && !found; s = tree_->GetNextChild(root, c1)) {
      for (auto m = tree_->GetFirstChild(s, c2); m.IsOk(); m = tree_->GetNextChild(s, c2)) {
        auto* d = dynamic_cast<MethodData*>(tree_->GetItemData(m));
        if (d && entries_[d->index].full_path == full_path) {
          tree_->SelectItem(m);
          tree_->EnsureVisible(m);
          found = true;
          break;
        }
      }
    }
    if (!found) tree_->UnselectAll();
  }
  quiet_ = false;
}

void MethodTree::on_sel(wxTreeEvent& e) {
  if (quiet_) return;
  auto* d = dynamic_cast<MethodData*>(tree_->GetItemData(e.GetItem()));
  if (!d) return;
  pick_selected(false);
}

void MethodTree::pick_selected(bool chosen) {
  const auto item = tree_->GetSelection();
  if (!item.IsOk()) return;
  auto* d = dynamic_cast<MethodData*>(tree_->GetItemData(item));
  if (!d) return;
  const std::string path = entries_[d->index].full_path;
  if (path == selected_ && !chosen) return;
  selected_ = path;
  if (on_pick) on_pick(path, chosen);
}

}  // namespace watt::app::grpced
