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
#include <wx/dnd.h>
#include <wx/eventfilter.h>
#include <wx/filedlg.h>
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
#include <wx/log.h>
#include <wx/utils.h>
#include <wx/weakref.h>
#include <wx/wupdlock.h>
#include <wx/msw/wrapwin.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <uxtheme.h>
#pragma comment(lib, "uxtheme.lib")

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "app/env_editor.h"
#include "app/history_summary.h"
#include "app/http_editor_folder.h"
#include "app/main_frame.h"
#include "app/panels.h"
#include "app/request_tab.h"
#include "app/sidebar_cookies.h"
#include "app/sidebar_dialogs.h"
#include "app/sidebar_files.h"
#include "app/http_editor_codegen.h"
#include "app/sidebar_fuzzy.h"
#include "app/sidebar_history.h"
#include "app/sidebar_import.h"
#include "app/sidebar_names.h"
#include "app/sidebar_probe.h"
#include "app/sidebar_quickopen.h"
#include "app/sidebar_style.h"
#include "app/ws_watch.h"
#include "core/http_client.h"
#include "core/model_history_store.h"

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

// Equal paths. The native strings first: fs::path's operator== compares element by element
// (it parses both), which made a 2000-request lookup loop cost ~60 ms.
bool same_path(const fs::path& a, const fs::path& b) {
  const auto& x = a.native();
  const auto& y = b.native();
  if (x.size() != y.size()) return false;
  for (size_t i = 0; i < x.size(); ++i)
    if (x[i] != y[i] && !((x[i] == L'/' || x[i] == L'\\') && (y[i] == L'/' || y[i] == L'\\'))) return false;
  return true;  // "a/b" and "a\b" too
}

CollectionNode* find_node(CollectionNode& n, const fs::path& p) {
  if (same_path(n.path, p)) return &n;
  if (!n.is_folder) return nullptr;
  for (auto& c : n.children)
    if (same_path(c.path, p) || (c.is_folder && is_under(p, c.path))) return find_node(c, p);
  return nullptr;
}
const CollectionNode* find_node(const CollectionNode& n, const fs::path& p) {
  return find_node(const_cast<CollectionNode&>(n), p);
}

// What labels history rows with request names and maps entries back to their files: a pure
// function of a scan (the tree and the names), so the scan worker builds it (~30 ms at 10k
// requests) and the UI thread only takes it over.
struct NameIndex {
  std::map<std::string, std::string> name_by_key;  // "get {{base}}/x" -> request name ("" if ambiguous)
  std::map<std::string, fs::path> path_by_key;     // same key -> its file (only when unique)
  std::multimap<std::string, std::pair<std::string, fs::path>> paths_by_key;  // key -> (name, file), all
  std::map<std::string, fs::path> path_by_id;      // request id -> its file (saved requests only)
};

NameIndex build_name_index(const CollectionNode& tree, const MetaMap& meta) {
  NameIndex x;
  std::set<std::string> ambiguous, ambiguous_ids;
  // Requests by path in one walk (a find_node() per request was a tree walk each).
  std::unordered_map<std::wstring, const CollectionNode*> by_path;
  by_path.reserve(meta.size());
  std::function<void(const CollectionNode&)> index = [&](const CollectionNode& n) {
    for (const auto& c : n.children) {
      if (c.is_folder) index(c);
      else by_path.emplace(c.path.native(), &c);
    }
  };
  index(tree);
  for (const auto& [path, m] : meta) {
    if (!m.id.empty() && !x.path_by_id.emplace(m.id, path).second) ambiguous_ids.insert(m.id);
    if (m.url.empty()) continue;
    const auto found = by_path.find(path.native());
    const CollectionNode* n = found == by_path.end() ? nullptr : found->second;
    std::string key;
    if (n && n->kind == RequestKind::Grpc) key = "grpc " + m.url;
    else if (n && n->badge) key = ascii_lower(n->method.empty() ? std::string("GET") : n->method) + " " + m.url;
    else continue;
    const std::string name = m.name.empty() ? n->name : m.name;
    auto [it, inserted] = x.name_by_key.emplace(key, name);
    if (!inserted && it->second != name) it->second.clear();  // two requests send the same thing
    if (!x.path_by_key.emplace(key, path).second) ambiguous.insert(key);
    x.paths_by_key.emplace(key, std::make_pair(name, path));
  }
  for (const auto& k : ambiguous) x.path_by_key.erase(k);
  for (const auto& k : ambiguous_ids) x.path_by_id.erase(k);  // a copied file kept the id
  return x;
}

// Lower-case search text per node (folder path, name, method, URL / gRPC method, file name),
// built once per scan so a filter keystroke does no allocation or map lookup per request.
using SearchKeys = std::unordered_map<const CollectionNode*, std::string>;

// A request's key is "<own text>\x01<context>": its name, URL and file name, then its folder
// path and method. Every filter word must be in one of them, and at least one in its own text,
// so "get user" lists "Get user" but not "List admins" in Users/ (method and folder only).
void build_keys(const CollectionNode& n, const std::string& prefix, const MetaMap& meta, SearchKeys& out) {
  if (!n.is_folder) {
    std::string t = n.name;
    if (auto m = meta.find(n.path); m != meta.end()) t += " " + url_search_text(m->second.url);
    t += " " + u8(n.path.filename());
    t += '\x01';
    t += prefix + " " + (n.kind == RequestKind::Grpc ? std::string("grpc") : n.method);
    out.emplace(&n, ascii_lower(t));
    return;
  }
  const std::string p = prefix.empty() && n.name.empty() ? "" : prefix + n.name + "/";
  if (!p.empty()) out.emplace(&n, ascii_lower(prefix + n.name));  // every folder but the root
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

// Some space-separated word of q is in t.
bool any_word_in(std::string_view q, std::string_view t) {
  for (size_t p = 0; p < q.size();) {
    size_t e = q.find(' ', p);
    if (e == std::string_view::npos) e = q.size();
    if (e > p && t.find(q.substr(p, e - p)) != std::string_view::npos) return true;
    p = e + 1;
  }
  return false;
}

// A query that names only the method ("patch", "grpc") or only a folder ("bulk 07") matches on
// those fields alone: every word is the request's method, or the whole query is in its folder
// path. A mix of a method word and a folder word ("get user") still needs a name / URL hit, so it
// does not list every GET request of a Users folder.
bool context_only_match(std::string_view q, std::string_view ctx) {
  const size_t sp = ctx.rfind(' ');
  const std::string_view method = sp == std::string_view::npos ? std::string_view() : ctx.substr(sp + 1);
  const std::string_view folder = sp == std::string_view::npos ? ctx : ctx.substr(0, sp);
  if (!folder.empty() && folder.find(q) != std::string_view::npos) return true;
  if (method.empty()) return false;
  for (size_t p = 0; p < q.size();) {
    size_t e = q.find(' ', p);
    if (e == std::string_view::npos) e = q.size();
    if (e > p && q.substr(p, e - p) != method) return false;
    p = e + 1;
  }
  return true;
}

// The last path segment of a folder key ("users/admin" -> "admin").
std::string_view folder_name_of(std::string_view key) {
  const size_t s = key.rfind('/');
  return s == std::string_view::npos ? key : key.substr(s + 1);
}

// Whether a multi-word query is the name (or part of the name) of some folder: then the filter
// shows that folder, not every request that has one word in its name and the other in its path.
bool phrase_names_folder(const CollectionNode& n, std::string_view q, const SearchKeys& keys) {
  if (q.find(' ') == std::string_view::npos) return false;
  for (const auto& c : n.children) {
    if (!c.is_folder) continue;
    if (auto it = keys.find(&c); it != keys.end() && folder_name_of(it->second).find(q) != std::string_view::npos)
      return true;
    if (phrase_names_folder(c, q, keys)) return true;
  }
  return false;
}

void collect_hits(const CollectionNode& n, std::vector<const CollectionNode*>* hits) {
  if (!hits) return;
  for (const auto& c : n.children) {
    if (c.is_folder) collect_hits(c, hits);
    else hits->push_back(&c);
  }
}

// Copy of n with only requests matching the filter (and the folders that lead to them). A request
// matches when every word is in its name, URL, file name, folder path or method and at least one
// is in its own text (name, URL, file name), or when the query names only its method or folder
// (context_only_match). A folder whose own name holds every word is shown whole. In phrase mode
// (the query is a folder name, phrase_names_folder) only that folder and requests whose own text
// holds the whole query match.
bool filter_node(const CollectionNode& n, const std::string& q, const SearchKeys& keys, CollectionNode& out,
                 std::vector<const CollectionNode*>* hits, bool phrase) {
  auto key = [&](const CollectionNode& x) -> std::string_view {
    auto it = keys.find(&x);
    return it == keys.end() ? std::string_view() : std::string_view(it->second);
  };
  if (!n.is_folder) {
    const std::string_view k = key(n);
    const size_t sep = k.find('\x01');
    const std::string_view own = k.substr(0, sep);
    if (phrase) {
      if (own.find(q) == std::string_view::npos) return false;
    } else {
      if (!words_in(q, k)) return false;
      if (!any_word_in(q, own) &&
          !(sep != std::string_view::npos && context_only_match(q, k.substr(sep + 1))))
        return false;
    }
    out = n;
    if (hits) hits->push_back(&n);
    return true;
  }
  const std::string_view k = key(n);
  if (!k.empty()) {
    const std::string_view name = folder_name_of(k);
    if (phrase ? name.find(q) != std::string_view::npos : words_in(q, name)) {
      out = n;
      collect_hits(n, hits);
      return true;
    }
  }
  out.name = n.name;
  out.path = n.path;
  out.is_folder = true;
  for (const auto& c : n.children) {
    CollectionNode cc;
    if (filter_node(c, q, keys, cc, hits, phrase)) out.children.push_back(std::move(cc));
  }
  return !out.children.empty();
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

// The docks of an AUI manager. A dock keeps its size across Update() (best_size is used only
// when a dock is created), so a new sidebar width used to need two relayouts of the whole frame
// (hide the pane, Update, show it, Update: 92-229 ms with a few tabs open). Setting the dock's
// size makes it one.
struct AuiDocks : wxAuiManager {
  static wxAuiDockInfoArray& of(wxAuiManager* m) { return static_cast<AuiDocks*>(m)->m_docks; }
  static bool resizing(wxAuiManager* m) { return static_cast<AuiDocks*>(m)->m_action == actionResize; }
  static void hide_resize_hint(wxAuiManager* m) { static_cast<AuiDocks*>(m)->m_overlay.Reset(); }
};

// wxAuiManager::Update() without wxAUI_MGR_LIVE_RESIZE freezes the whole frame with
// wxWindowUpdateLocker, and wxWindow::Freeze()/Thaw() recurse into every child window, the
// hidden notebook pages' too: with 8 request tabs (1500 windows) that alone was 65-105 ms of
// each Ctrl+B, sash release and tab removal, against 9 ms for the update itself. Updates run
// with the flag on (no lock; nothing paints before the update returns anyway), and a sash
// drag keeps it off (it moves a hint line, see Sidebar()).
void aui_update(wxAuiManager* m) {
  const unsigned flags = m->GetFlags();
  m->SetFlags(flags | wxAUI_MGR_LIVE_RESIZE);
  m->Update();
  m->SetFlags(flags);
}

// Paints a window and its children once, when it goes: WM_SETREDRAW on that one window, where
// wxWindowUpdateLocker / Freeze() send it to every descendant (see aui_update()).
class RedrawLock {
 public:
  explicit RedrawLock(wxWindow* w) : hwnd_(w ? static_cast<HWND>(w->GetHWND()) : nullptr) {
    if (hwnd_ && ::IsWindowVisible(hwnd_)) ::SendMessageW(hwnd_, WM_SETREDRAW, FALSE, 0);
    else hwnd_ = nullptr;
  }
  ~RedrawLock() { release(hwnd_); }
  RedrawLock(const RedrawLock&) = delete;
  RedrawLock& operator=(const RedrawLock&) = delete;
  static void release(HWND h) {
    if (!h || !::IsWindow(h)) return;
    ::SendMessageW(h, WM_SETREDRAW, TRUE, 0);
    ::RedrawWindow(h, nullptr, nullptr, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
  }
  HWND take() { return std::exchange(hwnd_, nullptr); }

 private:
  HWND hwnd_;
};

// The release of an AUI sash drag runs the update with live resize on (above): the hint line
// goes first, then the dock is resized and the frame laid out without the recursive lock.
class AuiSashRelease : public wxEventFilter {
 public:
  explicit AuiSashRelease(wxWindow* frame) : frame_(frame) { wxEvtHandler::AddFilter(this); }
  ~AuiSashRelease() override { wxEvtHandler::RemoveFilter(this); }
  int FilterEvent(wxEvent& e) override {
    if (e.GetEventType() != wxEVT_LEFT_UP || e.GetEventObject() != frame_) return Event_Skip;
    wxAuiManager* m = wxAuiManager::GetManager(frame_);
    if (!m || !AuiDocks::resizing(m) || m->HasLiveResize()) return Event_Skip;
    AuiDocks::hide_resize_hint(m);
    m->SetFlags(m->GetFlags() | wxAUI_MGR_LIVE_RESIZE);
    HWND locked = RedrawLock(frame_).take();  // painted once, with the new layout
    frame_->CallAfter([f = wxWeakRef<wxWindow>(frame_), locked] {  // after the manager handled the release
      RedrawLock::release(locked);
      if (wxAuiManager* mm = f ? wxAuiManager::GetManager(f.get()) : nullptr)
        mm->SetFlags(mm->GetFlags() & ~wxAUI_MGR_LIVE_RESIZE);
    });
    return Event_Skip;
  }

 private:
  wxWindow* frame_;
};

// Per-workspace key for wxConfig entries.
wxString ws_key(const fs::path& root) {
  return wxString::Format("%016llx", static_cast<unsigned long long>(std::hash<std::wstring>{}(root.wstring())));
}

// "GET {{base}}/x" -> ("GET", "{{base}}/x"); "/pkg.Svc/M @ host" -> ("grpc", "/pkg.Svc/M").
std::string history_key(const HistoryEntry& e) {
  const std::string t(history_template(e));
  if (e.kind == RequestKind::Grpc) {
    const size_t at = t.find(" @ ");
    return "grpc " + t.substr(0, at);
  }
  const size_t sp = t.find(' ');
  if (sp == std::string::npos) return t;
  return ascii_lower(t.substr(0, sp)) + " " + t.substr(sp + 1);
}

// Folder runs belong to the runner piece (SidebarHost::run_folder). Until the host has it, the
// menu shows "Run folder" disabled; once it exists this calls it, with no change here.
template <class H>
concept HostRunsFolders = requires(H* h, const fs::path& p) { h->run_folder(p); };
template <class H>
constexpr bool host_runs_folders(H*) {
  return HostRunsFolders<H>;
}
template <class H>
void host_run_folder(H* h, const fs::path& p) {
  if constexpr (HostRunsFolders<H>) {
    h->run_folder(p);
  } else {
    (void)h;
    (void)p;
  }
}

// The list fields of an entry only (what history_entry_key() and the hidden list need): an
// entry added live carries its whole request, which a worker need not copy.
HistoryEntry slim(const HistoryEntry& e) {
  HistoryEntry s;
  s.when_iso = e.when_iso;
  s.kind = e.kind;
  s.summary = e.summary;
  s.status = e.status;
  s.total_ms = e.total_ms;
  s.offset = e.offset;
  return s;
}

// Small state file under .watt (written whole, then renamed over the old one). Any thread.
void write_text_file(const fs::path& file, const std::string& text) {
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  fs::path tmp = file;
  tmp += L".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << text;
    if (!out) return;
  }
  fs::rename(tmp, file, ec);
  if (ec) fs::remove(tmp, ec);
}

std::string size_text(uint64_t n) {
  char buf[32];
  if (n >= (1u << 20)) std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(n) / (1u << 20));
  else std::snprintf(buf, sizeof buf, "%.0f KB", static_cast<double>(n) / 1024.0);
  return buf;
}

// Re-indents a JSON text that may be cut anywhere (2 spaces, one member / element per line,
// strings untouched). False (body unchanged) when it does not start like JSON or already has
// line breaks (it was pretty-printed by the server).
bool indent_json_prefix(std::string& body) {
  size_t i = 0;
  while (i < body.size() && (body[i] == ' ' || body[i] == '\t' || body[i] == '\r' || body[i] == '\n')) ++i;
  if (i >= body.size() || (body[i] != '{' && body[i] != '[')) return false;
  if (body.find('\n', i) != std::string::npos) return false;
  std::string out;
  out.reserve(body.size() + body.size() / 2);
  int depth = 0;
  bool in_str = false, esc = false;
  auto newline = [&] {
    out += '\n';
    out.append(static_cast<size_t>(std::max(depth, 0)) * 2, ' ');
  };
  for (; i < body.size(); ++i) {
    const char c = body[i];
    if (in_str) {
      out += c;
      if (esc) esc = false;
      else if (c == '\\') esc = true;
      else if (c == '"') in_str = false;
      continue;
    }
    switch (c) {
      case '"':
        in_str = true;
        out += c;
        break;
      case '{':
      case '[': {
        out += c;
        // An empty object / array stays on its line.
        size_t j = i + 1;
        while (j < body.size() && (body[j] == ' ' || body[j] == '\t')) ++j;
        if (j < body.size() && (body[j] == '}' || body[j] == ']')) {
          out += body[j];
          i = j;
          break;
        }
        ++depth;
        newline();
        break;
      }
      case '}':
      case ']':
        --depth;
        newline();
        out += c;
        break;
      case ',':
        out += c;
        newline();
        break;
      case ':':
        out += ": ";
        break;
      case ' ':
      case '\t':
      case '\r':
      case '\n':
        break;
      default:
        out += c;
    }
  }
  body = std::move(out);
  return true;
}

// A response as it is kept with its history entry: the HTTP body cut to kHistoryResponseCap
// (at a UTF-8 boundary), gRPC messages up to that many bytes; the cut is said in the status line.
LastResponse capped_response(const LastResponse& r) {
  LastResponse out = r;
  if (r.http && r.http->body.size() > kHistoryResponseCap) {
    auto h = std::make_shared<HttpResponse>(*r.http);
    size_t n = kHistoryResponseCap;
    while (n > 0 && (static_cast<unsigned char>(h->body[n]) & 0xC0) == 0x80) --n;
    h->body.resize(n);
    // A cut JSON body no longer parses, so the view would show it raw: one 64 KB line for a
    // minified API response. Indented here, it reads like the pretty view (tolerant of the cut).
    const bool json = indent_json_prefix(h->body);
    h->status_text += (h->status_text.empty() ? "" : " ") + std::string("(body cut: history keeps the first ") +
                      size_text(n) + " of " + size_text(r.http->body.size()) + (json ? ", indented)" : ")");
    out.http = std::move(h);
  }
  if (r.grpc) {
    size_t bytes = 0, keep = 0;
    for (const auto& m : r.grpc->messages) {
      if (bytes + m.json.size() > kHistoryResponseCap && keep > 0) break;
      bytes += m.json.size();
      ++keep;
    }
    if (keep < r.grpc->messages.size()) {
      auto g = std::make_shared<GrpcResponse>(*r.grpc);
      g->messages.resize(keep);
      g->status_message += (g->status_message.empty() ? "" : " ") + std::string("(history keeps the first ") +
                           std::to_string(keep) + " of " + std::to_string(r.grpc->messages.size()) + " messages)";
      out.grpc = std::move(g);
    }
  }
  return out;
}

// "/pkg.Svc/M @ host:1" -> "/pkg.Svc/M".
std::string grpc_method_of(const HistoryEntry& e) {
  if (e.kind != RequestKind::Grpc) return {};
  const std::string_view t = history_template(e);
  return std::string(t.substr(0, t.find(" @ ")));
}

std::string strip_scheme(std::string s) {
  for (const char* scheme : {"https://", "http://", "wss://", "ws://"})
    if (s.rfind(scheme, 0) == 0) return s.substr(std::char_traits<char>::length(scheme));
  return s;
}

// The collection tree. Its rename box is a native edit the tree creates itself, which neither
// wx's dark mode nor SetBackgroundColour reach: colour it here, where the tree (its parent)
// is asked for the edit's colours.
// Files dropped from Explorer: a collection file (or a Bruno folder) to import.
class FileDrop : public wxFileDropTarget {
 public:
  using Fn = std::function<bool(wxCoord, wxCoord, const wxArrayString&)>;
  explicit FileDrop(Fn fn) : fn_(std::move(fn)) {}
  bool OnDropFiles(wxCoord x, wxCoord y, const wxArrayString& files) override { return fn_(x, y, files); }

 private:
  Fn fn_;
};

class CollectionTree : public wxTreeCtrl {
 public:
  using wxTreeCtrl::wxTreeCtrl;
  ~CollectionTree() override {
    if (brush_) DeleteObject(brush_);
  }
  // While a row is dragged: where a drop would reorder (0: not a reorder; 1: before `hit`,
  // 2: after it). The native insert mark then shows the line it lands on (as in Bruno), and
  // the target row is not highlighted as a drop folder.
  std::function<int(const wxTreeItemId& hit)> drag_mark;
  bool dragging = false;
  // A folder row's request count, drawn dimmed at the row's right edge (as Insomnia shows it);
  // 0: none. Only the rows on screen are asked, after each native paint.
  std::function<size_t(const wxTreeItemId&)> folder_count;
  wxColour count_colour;
  // Faint vertical lines under each open folder's chevron (VS Code's indent guides; easier to
  // follow at 4 levels) and the selected row's accent bar at its left edge (the theme's grey
  // band alone was hard to see on a folder row).
  wxColour guide_colour, accent_colour;
  // The sidebar filter (lower-case, space separated words; empty: none): the characters of each
  // row's name that a word matches are underlined, so it is plain why a row is listed.
  std::function<const std::string&()> filter_query;
  wxColour match_colour;

  void clear_drag_mark() {
    SendMessage(static_cast<HWND>(GetHWND()), TVM_SETINSERTMARK, FALSE, 0);
  }

 protected:
  WXLRESULT MSWWindowProc(WXUINT msg, WXWPARAM wp, WXLPARAM lp) override {
    if (dragging && msg == WM_MOUSEMOVE && drag_mark) {
      const WXLRESULT r = wxTreeCtrl::MSWWindowProc(msg, wp, lp);
      const HWND hw = static_cast<HWND>(GetHWND());
      TVHITTESTINFO ht{};
      ht.pt.x = static_cast<short>(LOWORD(lp));  // not windowsx.h: its macros clash with wx
      ht.pt.y = static_cast<short>(HIWORD(lp));
      const auto hit = reinterpret_cast<HTREEITEM>(SendMessage(hw, TVM_HITTEST, 0, reinterpret_cast<LPARAM>(&ht)));
      const int m = hit ? drag_mark(wxTreeItemId(hit)) : 0;
      ImageList_DragShowNolock(FALSE);  // painting under the drag image corrupts it
      if (m) {
        SendMessage(hw, TVM_SELECTITEM, TVGN_DROPHILITE, 0);
        SendMessage(hw, TVM_SETINSERTMARK, m == 2, reinterpret_cast<LPARAM>(hit));
      } else {
        SendMessage(hw, TVM_SETINSERTMARK, FALSE, 0);
      }
      UpdateWindow(hw);
      ImageList_DragShowNolock(TRUE);
      return r;
    }
    if (dragging && (msg == WM_LBUTTONUP || msg == WM_CAPTURECHANGED || msg == WM_CANCELMODE)) {
      clear_drag_mark();
      if (msg != WM_LBUTTONUP) dragging = false;  // cancelled: no END_DRAG follows
    }
    if (msg == WM_PAINT && (folder_count || filter_query)) {
      const WXLRESULT r = wxTreeCtrl::MSWWindowProc(msg, wp, lp);
      paint_counts();
      return r;
    }
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
  void paint_counts() {
    const HWND hw = static_cast<HWND>(GetHWND());
    RECT client{};
    ::GetClientRect(hw, &client);
    HDC dc = ::GetDC(hw);
    if (!dc) return;
    const HGDIOBJ old_font = ::SelectObject(dc, reinterpret_cast<HFONT>(SendMessage(hw, WM_GETFONT, 0, 0)));
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, RGB(count_colour.Red(), count_colour.Green(), count_colour.Blue()));
    const int pad = FromDIP(8);
    // Filter words (split once per paint, not per row).
    std::vector<std::string> words;
    if (filter_query) {
      const std::string& q = filter_query();
      for (size_t i = 0; i < q.size();) {
        const size_t e = std::min(q.find(' ', i), q.size());
        if (e > i) words.push_back(q.substr(i, e - i));
        i = e + 1;
      }
    }
    HFONT bold_font = nullptr;
    HPEN pen = nullptr;
    if (!words.empty()) {
      LOGFONTW lf{};
      ::GetObjectW(reinterpret_cast<HFONT>(SendMessage(hw, WM_GETFONT, 0, 0)), sizeof lf, &lf);
      lf.lfWeight = FW_BOLD;
      bold_font = ::CreateFontIndirectW(&lf);
      pen = ::CreatePen(PS_SOLID, std::max(1, FromDIP(2)), RGB(match_colour.Red(), match_colour.Green(), match_colour.Blue()));
    }
    int img_w = FromDIP(28);
    if (auto il = reinterpret_cast<HIMAGELIST>(SendMessage(hw, TVM_GETIMAGELIST, TVSIL_NORMAL, 0))) {
      int cx = 0, cy = 0;
      if (ImageList_GetIconSize(il, &cx, &cy) && cx > 0) img_w = cx;
    }
    const int indent = static_cast<int>(SendMessage(hw, TVM_GETINDENT, 0, 0));
    const HTREEITEM caret = reinterpret_cast<HTREEITEM>(SendMessage(hw, TVM_GETNEXTITEM, TVGN_CARET, 0));
    const HTREEITEM root = reinterpret_cast<HTREEITEM>(SendMessage(hw, TVM_GETNEXTITEM, TVGN_ROOT, 0));
    HBRUSH guide = guide_colour.IsOk() ? ::CreateSolidBrush(RGB(guide_colour.Red(), guide_colour.Green(), guide_colour.Blue())) : nullptr;
    auto item = reinterpret_cast<HTREEITEM>(SendMessage(hw, TVM_GETNEXTITEM, TVGN_FIRSTVISIBLE, 0));
    for (int guard = 0; item && guard < 400; ++guard) {
      RECT rc{};
      *reinterpret_cast<HTREEITEM*>(&rc) = item;
      if (!SendMessage(hw, TVM_GETITEMRECT, TRUE, reinterpret_cast<LPARAM>(&rc)) || rc.top >= client.bottom) break;
      if (guide && indent > 0) {
        int depth = 0;  // 0: a top-level row (the root is hidden)
        for (HTREEITEM p = reinterpret_cast<HTREEITEM>(SendMessage(hw, TVM_GETNEXTITEM, TVGN_PARENT, reinterpret_cast<LPARAM>(item)));
             p && p != root && depth < 64;
             p = reinterpret_cast<HTREEITEM>(SendMessage(hw, TVM_GETNEXTITEM, TVGN_PARENT, reinterpret_cast<LPARAM>(p))))
          ++depth;
        // Under each ancestor's chevron (the left part of its image).
        const int w = std::max(1, FromDIP(1));
        for (int k = 0; k < depth; ++k) {
          const int x = rc.left - (depth - k) * indent - img_w + FromDIP(11);
          if (x < 0) continue;
          RECT line{x, rc.top, x + w, rc.bottom};
          ::FillRect(dc, &line, guide);
        }
      }
      if (item == caret && accent_colour.IsOk()) {
        RECT full{};
        *reinterpret_cast<HTREEITEM*>(&full) = item;
        if (SendMessage(hw, TVM_GETITEMRECT, FALSE, reinterpret_cast<LPARAM>(&full))) {
          HBRUSH b = ::CreateSolidBrush(RGB(accent_colour.Red(), accent_colour.Green(), accent_colour.Blue()));
          RECT bar{0, full.top, std::max(2, FromDIP(3)), full.bottom};
          ::FillRect(dc, &bar, b);
          ::DeleteObject(b);
        }
      }
      if (!words.empty()) underline_matches(dc, item, rc, words, bold_font, pen);
      if (const size_t n = folder_count ? folder_count(wxTreeItemId(item)) : 0) {
        wchar_t buf[24];
        const int len = std::swprintf(buf, 24, L"%zu", n);
        SIZE sz{};
        ::GetTextExtentPoint32W(dc, buf, len, &sz);
        const int x = client.right - pad - sz.cx;
        // Never over the name: a long name keeps its row to itself.
        if (x > rc.right + pad) ::TextOutW(dc, x, rc.top + (rc.bottom - rc.top - sz.cy) / 2, buf, len);
      }
      item = reinterpret_cast<HTREEITEM>(SendMessage(hw, TVM_GETNEXTITEM, TVGN_NEXTVISIBLE, reinterpret_cast<LPARAM>(item)));
    }
    ::SelectObject(dc, old_font);
    if (guide) ::DeleteObject(guide);
    if (bold_font) ::DeleteObject(bold_font);
    if (pen) ::DeleteObject(pen);
    ::ReleaseDC(hw, dc);
  }

  // Underlines, under the row's name, each span a filter word matches (case-insensitive, every
  // occurrence). The native control draws the name centred in rc with its own small margin.
  void underline_matches(HDC dc, HTREEITEM item, const RECT& rc, const std::vector<std::string>& words,
                         HFONT bold_font, HPEN pen) {
    wchar_t buf[260];
    TVITEMW it{};
    it.mask = TVIF_TEXT | TVIF_STATE;
    it.hItem = item;
    it.stateMask = TVIS_BOLD;
    it.pszText = buf;
    it.cchTextMax = 260;
    if (!SendMessageW(static_cast<HWND>(GetHWND()), TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&it)) || !buf[0]) return;
    const std::wstring label(buf);
    // Lower-case per UTF-16 unit for ASCII only (as the filter does): offsets stay aligned.
    std::wstring low = label;
    for (auto& c : low)
      if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    const HGDIOBJ prev_font = (it.state & TVIS_BOLD) && bold_font ? ::SelectObject(dc, bold_font) : nullptr;
    SIZE full{};
    ::GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &full);
    const int x0 = rc.left + std::max<int>(0, (rc.right - rc.left - full.cx) / 2);
    const int y = rc.bottom - std::max(1, FromDIP(2));
    const HGDIOBJ prev_pen = ::SelectObject(dc, pen);
    for (const auto& w8 : words) {
      const std::wstring w = wxString::FromUTF8(w8).ToStdWstring();
      if (w.empty()) continue;
      for (size_t at = low.find(w); at != std::wstring::npos; at = low.find(w, at + w.size())) {
        SIZE a{}, b{};
        ::GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(at), &a);
        ::GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(at + w.size()), &b);
        if (at == 0) a.cx = 0;
        ::MoveToEx(dc, x0 + a.cx, y, nullptr);
        ::LineTo(dc, std::min<int>(x0 + b.cx, rc.right), y);
      }
    }
    ::SelectObject(dc, prev_pen);
    if (prev_font) ::SelectObject(dc, prev_font);
  }

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
    hist_focus_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { hist_focus_tick(); });
    preview_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
      wxAuiNotebook* nb = notebook();
      auto* t = nb ? dynamic_cast<RequestTab*>(nb->GetCurrentPage()) : nullptr;
      if (!t || !t->realized()) return;
      // Its folder variables have landed by now: the editor colours {{baseUrl}} with them (not
      // red as undefined) when it shows the request again. Not while the user types in the tab
      // (the caret would move), nor in a tab with a large body (a reload would cost a frame) or
      // a send in flight.
      bool typing = false;
      for (wxWindow* f = wxWindow::FindFocus(); f; f = f->GetParent())
        if (f == t) typing = true;
      if (!typing && !t->in_flight() && t->request_kind() == RequestKind::Http && t->file() == preview_file_) {
        const SavedRequest& r = t->request();
        if (r.http.url.find("{{") != std::string::npos && r.http.body.size() < (256u << 10) &&
            recoloured_.insert({t, t->file()}).second)  // once per tab and file
          reload_editor(t);
      }
      t->vars_changed();
    });
    inh_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
      if (host_) host_->folder_settings_changed();  // the "Sent as" lines, with what was read
      if (--inh_ticks_ > 0) {
        inh_timer_.StartOnce(450);
      } else if (pending_inherit_set_) {
        pending_inherit_set_ = false;
        refresh_inherit_tabs(pending_inherit_);
      }
    });
    learn_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
      RequestTab* t = tab_for(learn_file_);
      if (!t || learn_grpc_kinds(t) || ++learn_tries_ > 120) learn_timer_.Stop();
    });
    width_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
      if (wxConfigBase* c = wxConfigBase::Get()) {
        // Per workspace (a window on another workspace keeps its own width); the plain key is
        // the default for a workspace never resized, and what the frame starts with.
        const long w = static_cast<long>(ToDIP(GetSize().x));
        if (ws_) c->Write("Sidebar/Width/" + ws_key(ws_->root()), w);
        c->Write("Sidebar/Width", w);
        c->Flush();
      }
    });
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      width_changed();
    });
    // Once the frame has laid out its panes.
    CallAfter([this] {
      // The sidebar's sash: AUI's live resize relays out and repaints the whole frame (every
      // tab's editor and response) on each mouse step, 40-121 ms each. Without it the drag
      // moves a resize line (no layout) and the frame is laid out once, on release.
      // Its release lays the frame out without freezing every window (AuiSashRelease).
      if (wxAuiManager* m = wxAuiManager::GetManager(this)) {
        m->SetFlags(m->GetFlags() & ~wxAUI_MGR_LIVE_RESIZE);
        sash_release_ = std::make_unique<AuiSashRelease>(m->GetManagedWindow());
      }
      // The notebook's own manager (its tab frames) updates on every page added or removed:
      // without live resize each of those froze and thawed every page's windows (67 ms of a tab
      // close with 8 tabs). It has no drag to keep cheap: tab frames are not split here.
      if (wxAuiNotebook* nb = notebook()) {
        auto& nm = const_cast<wxAuiManager&>(nb->GetAuiManager());
        nm.SetFlags(nm.GetFlags() | wxAUI_MGR_LIVE_RESIZE);
      }
      // Muted page events (see swap_tabs()). Bound after the frame's handlers, so it runs first.
      if (wxAuiNotebook* nb = notebook()) {
        auto mute = [this](wxAuiNotebookEvent& e) {
          if (!mute_page_events_) e.Skip();
        };
        nb->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGED, mute);
        nb->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGING, mute);
      }
      dormant_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { dormant_tick(); });
      heal_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { heal_stale_tabs(); });
      dormant_timer_.Start(400);
      restore_width();
      width_restored_ = true;
      selftest_start();
    });
  }

  ~Sidebar() override { release_lock(); }

  // Runs fn as its own UI step. CallAfter() does not split work: wx runs every pending
  // CallAfter (also ones queued from inside one) in the same idle pass, so a chain of them is a
  // single stall. A posted message goes through the message loop, which handles input, sent
  // messages and the next steps' messages in between.
  static constexpr UINT kLaterMsg = WM_APP + 0x4C;
  void later(std::function<void()> fn) {
    later_q_.push_back(std::move(fn));
    ::PostMessageW(static_cast<HWND>(GetHWND()), kLaterMsg, 0, 0);
  }
  WXLRESULT MSWWindowProc(WXUINT msg, WXWPARAM wp, WXLPARAM lp) override {
    if (msg == kLaterMsg) {
      if (!later_q_.empty()) {
        auto fn = std::move(later_q_.front());
        later_q_.pop_front();
        fn();
      }
      return 0;
    }
    return wxPanel::MSWWindowProc(msg, wp, lp);
  }

  // ---------------------------------------------------------------- self test
  // WATT_SIDEBAR_SELFTEST=<file>: once the workspace is scanned, runs the critic's sidebar
  // actions with a WM_NULL stall probe and writes "<step>\t<sync ms>\t<max stall ms>" lines,
  // then quits (unless WATT_SIDEBAR_SELFTEST_STAY is set). Unset: nothing happens.
  struct TestStep {
    std::string name;
    std::function<void()> fn;
    int wait_ms = 800;
  };

  void selftest_start() {
    wxString out;
    if (!wxGetEnv("WATT_SIDEBAR_SELFTEST", &out) || out.empty()) return;
    st_out_ = out.ToStdWstring();
    st_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { selftest_tick(); });
    st_timer_.StartOnce(500);
  }

  fs::path st_find(const std::string& name) const {
    fs::path found;
    std::function<void(const CollectionNode&)> walk = [&](const CollectionNode& n) {
      for (const auto& c : n.children) {
        if (!found.empty()) return;
        if (c.is_folder) walk(c);
        else if (c.name == name) found = c.path;
      }
    };
    walk(tree_root_);
    return found;
  }

  // "nb 3 tabs, parked 2 (in notebook 0)": tabs of other workspaces must never stay children of
  // the notebook.
  std::string st_tab_state() {
    wxAuiNotebook* nb = notebook();
    size_t parked = 0, in_nb = 0;
    for (const auto& [root, set] : tab_sets_)
      for (const auto& pg : set.pages) {
        ++parked;
        in_nb += pg.first->GetParent() == nb;
      }
    return "nb " + std::to_string(nb ? nb->GetPageCount() : 0) + " tabs, parked " + std::to_string(parked) +
           " (in notebook " + std::to_string(in_nb) + ")";
  }

  void st_collapse_all() {
    syncing_ = true;
    tree_->Freeze();
    wxTreeItemIdValue cookie;
    for (wxTreeItemId c = tree_->GetFirstChild(tree_->GetRootItem(), cookie); c.IsOk();
         c = tree_->GetNextChild(tree_->GetRootItem(), cookie))
      if (tree_->IsExpanded(c)) tree_->CollapseAllChildren(c);
    tree_->Thaw();
    syncing_ = false;
  }

  void st_build() {
    auto add = [this](std::string n, std::function<void()> fn, int wait = 800) {
      st_steps_.push_back(TestStep{std::move(n), std::move(fn), wait});
    };
    add("idle", [] {}, 1500);
    wxString only;
    if (wxGetEnv("WATT_SIDEBAR_SELFTEST_ONLY", &only) && only == "dup") {
      st_build_dup();
      return;
    }
    if (only == "big") {
      st_build_big();
      return;
    }
    // Many open tabs with responses (the critic's round 19 case): Ctrl+B, a sash drag, a
    // workspace round trip (WATT_SIDEBAR_SELFTEST_WS2) and deleting a request with an open
    // tab must each stay under 50 ms.
    if (only == "tabs") {
      st_build_tabs();
      return;
    }
    // Copy / paste between folders, a paste next to the original and Ctrl+D: the copies' rows.
    if (only == "paste") {
      auto rows = [this](const fs::path& folder) {
        std::string out;
        if (const CollectionNode* n = find_node(tree_root_, folder)) {
          std::vector<const CollectionNode*> kids;
          for (const auto& c : n->children) kids.push_back(&c);
          sort_kids(folder, kids);
          for (const auto* k : kids) out += k->name + " | ";
        }
        return out;
      };
      add("copy", [this] { copy_item(st_find("Get order v2")); }, 300);
      add("paste into users", [this] { paste_into(st_find("Get order v2").parent_path().parent_path() / "users"); }, 1500);
      add("paste next to it", [this] { paste_into(st_find("Get order v2").parent_path()); }, 1500);
      add("ctrl+d", [this] { duplicate_item(st_find("List orders")); }, 1500);
      add("esc", [this] {
        if (tree_->GetEditControl()) tree_->EndEditLabel(tree_->GetFocusedItem(), true);
      }, 1500);
      add("rows", [this, rows] {
        const fs::path orders = st_find("Get order v2").parent_path();
        st_log_ += "# orders: " + rows(orders) + "\n# users: " + rows(orders.parent_path() / "users") + "\n";
      }, 50);
      return;
    }
    // A broad filter on a big workspace (2000 requests): typing, narrowing, widening, clearing.
    // Each step logs its synchronous time and the longest UI stall (the probe) after it.
    if (only == "filter") {
      auto type = [this](const char* q) {
        filter_->ChangeValue(wxString::FromUTF8(q));
        apply_filter();
      };
      for (int round = 0; round < 3; ++round) {
        add("filter e", [this, type] {
          focus_filter();
          type("e");
        }, 1500);
        add("rows e", [this] {
          st_log_ += wxString::Format("# rows %u\n", tree_->GetCount()).utf8_string();
          st_log_ += "# focus: " + st_focus() + "\n";
        }, 50);
        add("clear e", [type] { type(""); }, 1500);
        add("focus after clear", [this] { st_log_ += "# focus: " + st_focus() + "\n"; }, 50);
        add("filter p", [type] { type("p"); }, 1500);
        add("p->po", [type] { type("po"); }, 1500);
        add("po->p", [type] { type("p"); }, 1500);
        add("clear p", [type] { type(""); }, 1500);
        add("check rows", [this] { st_check_rows(); }, 50);
      }
      if (wxString ws2; wxGetEnv("WATT_SIDEBAR_SELFTEST_WS2", &ws2)) {
        const fs::path a = ws_->root(), b = fs::path(ws2.ToStdWstring());
        add("filter e (switch)", [type] { type("e"); }, 1500);
        add("switch with filter", [this, b] { switch_workspace(b, true); }, 2500);
        add("switch back", [this, a] { switch_workspace(a, true); }, 2500);
        add("state", [this] { st_log_ += wxString::Format("# expanded %zu, rows %u\n", expanded_.size(), tree_->GetCount()).utf8_string(); }, 50);
      }
      return;
    }
    // Cookie manager: a few cookies seeded through the jar's Set-Cookie rules, then the dialog
    // (modal: the next steps run inside it).
    if (only == "cookies") {
      add("seed cookies", [this] {
        auto jar = ws_->cookie_jar();
        jar->preload();
        if (jar->size() > 0) return;
        const char* const lines[][2] = {
            {"http://127.0.0.1:18080/login", "session=7f3a9c21e0b4; Path=/; HttpOnly; SameSite=Lax"},
            {"http://127.0.0.1:18080/login", "csrftoken=Jq8xV2mN4pLs; Path=/; Max-Age=86400"},
            {"https://api.example.com/v1/users", "auth_token=eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjMifQ; Path=/v1; "
                                                 "Secure; HttpOnly; Max-Age=3600"},
            {"https://api.example.com/", "__Host-prefs=theme%3Ddark; Path=/; Secure; SameSite=Strict"},
            {"https://shop.example.org/", "cart_id=c-5521; Domain=example.org; Path=/; Max-Age=2592000"},
            {"https://shop.example.org/", "ab_test=variant-b; Path=/; Expires=Thu, 31 Dec 2026 23:59:59 GMT"},
            {"https://auth.internal.test/oauth", "oidc_state=s8d7f6; Path=/oauth; Secure; HttpOnly; SameSite=None"},
        };
        for (const auto& l : lines) jar->set_from_header(l[0], l[1]);
        st_log_ += "# seeded " + std::to_string(jar->size()) + " cookies\n";
      }, 300);
      add("open cookies", [this] { open_cookies(); }, 1500);
      return;
    }
    // History keeps the folder: "List" (api/) is sent, then its file is deleted; its history
    // entry still opens with api/ as the folder it inherits from (logged).
    if (only == "origin") {
      add("send List", [this] {
        open_tab(st_find("List"));
        if (RequestTab* t = tab_for(st_find("List"))) t->send();
      }, 2000);
      add("delete List", [this] {
        const fs::path f = st_find("List");
        st_log_ += "# deleting " + u8(f.lexically_relative(ws_->root())) + "\n";
        std::error_code ec;
        fs::remove(f, ec);
        rescan();
      }, 1500);
      add("open history", [this] {
        if (history_->entries().empty()) {
          st_log_ += "# no history\n";
          return;
        }
        open_history_entry(history_->entries().front(), true, false);
      }, 1500);
      add("origin", [this] {
        if (history_->entries().empty()) return;
        const fs::path snap = snapshot_file(history_->entries().front());
        const auto it = snapshot_origin_.find(snap);
        st_log_ += "# snapshot inherits from " +
                   (it == snapshot_origin_.end() ? std::string("<none>") : u8(it->second.lexically_relative(ws_->root()))) +
                   "\n";
      }, 50);
      return;
    }
    // Workspace switching with open tabs: hidden tabs are parked outside the notebook and come
    // back (WATT_SIDEBAR_SELFTEST_WS2=<other workspace>), several round trips.
    if (wxString ws2; only == "switch" && wxGetEnv("WATT_SIDEBAR_SELFTEST_WS2", &ws2)) {
      const fs::path a = ws_->root(), b = fs::path(ws2.ToStdWstring());
      add("open 3 tabs", [this] {
        std::vector<fs::path> reqs;
        collect_requests(tree_root_, reqs);
        for (size_t i = 0; i < reqs.size() && i < 3; ++i) open_tab(reqs[i]);
      }, 1500);
      for (int round = 0; round < 6; ++round) {
        add("switch to ws2", [this, b] { switch_workspace(b, true); }, 1500);
        add("parked", [this] { st_log_ += "# " + st_tab_state() + "\n"; }, 50);
        add("switch back", [this, a] { switch_workspace(a, true); }, 1500);
        add("back", [this] { st_log_ += "# " + st_tab_state() + "\n"; }, 50);
      }
      return;
    }
    // A relaunch on a saved session: the tab active at launch must have what it inherits from
    // folder.json (variables, headers, auth), so a send goes where its folder says.
    if (only == "restart") {
      add("restart heal", [this] { st_log_ += "# " + heal_log_; }, 50);
      add("restart send", [this] {
        auto* t = notebook() ? dynamic_cast<RequestTab*>(notebook()->GetCurrentPage()) : nullptr;
        st_log_ += "# active " + (t ? u8(t->file().filename()) + " name '" + t->request().name + "'" : std::string("none")) + "\n";
        if (t) t->send();
      }, 2000);
      add("restart result", [this] {
        auto* t = notebook() ? dynamic_cast<RequestTab*>(notebook()->GetCurrentPage()) : nullptr;
        const auto* h = t ? t->last_response().http.get() : nullptr;
        const auto* sent = t ? t->last_response().sent.get() : nullptr;
        std::string auth = "<none>";
        if (sent)
          for (const auto& [k, v] : sent->request.headers)
            if (_stricmp(k.c_str(), "Authorization") == 0) auth = v;
        st_log_ += "# " + (h ? "status " + std::to_string(h->status) + " " + h->error + " url " + h->effective_url
                             : std::string("no response")) +
                   " auth " + auth + "\n";
      }, 50);
      return;
    }
    // WATT_SIDEBAR_SELFTEST_IMPORT=<collection file>: a clean import ends in a notice.
    if (wxString src; only == "import" && wxGetEnv("WATT_SIDEBAR_SELFTEST_IMPORT", &src)) {
      add("import", [this, src] { start_import(fs::path(src.ToStdWstring()), ws_->root()); }, 2500);
      add("import notice", [this] {
        st_log_ += "# notice: " + (note_ && note_->IsShown() ? note_->GetLabel().utf8_string() : std::string("<none>")) +
                   "\n";
      }, 50);
      // WATT_SIDEBAR_SELFTEST_REQ=<imported request>: open and send it, log status and URL.
      if (wxString rq; wxGetEnv("WATT_SIDEBAR_SELFTEST_REQ", &rq)) {
        const std::string rn = rq.utf8_string();
        add("import open " + rn, [this, rn] { open_preview(st_find(rn)); }, 1200);
        add("import send", [this, rn] {
          if (RequestTab* t = tab_for(st_find(rn))) t->send();
          else st_log_ += "# no tab for " + rn + "\n";
        }, 2000);
        add("import result", [this, rn] {
          RequestTab* t = tab_for(st_find(rn));
          const auto* h = t ? t->last_response().http.get() : nullptr;
          st_log_ += "# env " + ws_->active_env + "; " +
                     (h ? "status " + std::to_string(h->status) + " " + h->error + " url " + h->effective_url
                        : std::string("no response")) +
                     "\n";
        }, 50);
      }
      return;
    }
    // Ctrl+B with one tab open (the frame relayout grows with every open tab's editor).
    for (int i = 0; i < 2; ++i) add(i % 2 ? "ctrl+b show (1 tab)" : "ctrl+b hide (1 tab)", [this] {
      if (wxAuiNotebook* nb = notebook()) st_log_ += "# tabs " + std::to_string(nb->GetPageCount()) + "\n";
      set_collapsed(!collapsed_);
    }, 900);
    for (const char* t : {"0777", "0555", "1234", "1555", "0333", "1999"}) {
      add(std::string("collapse"), [this] { st_collapse_all(); }, 600);
      add(std::string("quickopen ep") + t, [this, t] {
        const fs::path p = st_find(std::string("Endpoint ") + t);
        if (p.empty()) st_log_ += "# missing Endpoint " + std::string(t) + "\n";
        open_preview(p);
      }, 1200);
    }
    for (const char* q : {"e", "en", "end", "endp", "endpo", "endpoi", "point", "0", "07", "0", "group 1", "", "users",
                          "user", "get", ""}) {
      add(std::string("filter '") + q + "'", [this, q] {
        filter_->ChangeValue(wxString::FromUTF8(q));
        apply_filter();
      }, 600);
      add("check rows", [this] { st_check_rows(); }, 50);
    }
    add("grpc open Say", [this] {
      const fs::path p = st_find("Say");
      if (wxTreeItemId id = item_for(p, true); id.IsOk()) tree_->SelectItem(id);
    }, 4000);
    add("grpc open Count", [this] {
      const fs::path p = st_find("Count");
      if (wxTreeItemId id = item_for(p, true); id.IsOk()) tree_->SelectItem(id);
    }, 4000);
    add("grpc kinds learned", [this] {
      st_log_ += "# grpc kinds known: " + std::to_string(grpc_kinds_.size()) + "\n";
      std::error_code ec;
      st_log_ += std::string("# tsv written: ") +
                 (fs::exists(ws_->root() / ".watt" / "sidebar-grpc-kinds.tsv", ec) ? "yes" : "no") + "\n";
    }, 300);
    add("new request", [this] {
      new_request_in(RequestKind::Http, st_find("Get user").parent_path());
    }, 1200);
    add("name + Enter", [this] {
      if (wxTextCtrl* ed = tree_->GetEditControl()) {
        ed->SetValue(wxString::Format("Probe %ld", static_cast<long>(wxGetLocalTime() % 100000)));
        tree_->EndEditLabel(tree_->GetFocusedItem(), false);
      } else {
        st_log_ += "# no rename editor\n";
      }
    }, 1500);
    add("focus after name+Enter", [this] { st_log_ += "# focus: " + st_focus() + "\n"; }, 50);
    add("ctrl+d", [this] { duplicate_item(st_find("Get user")); }, 1200);
    add("enter", [this] {
      if (tree_->GetEditControl()) tree_->EndEditLabel(tree_->GetFocusedItem(), false);
      else st_log_ += "# no rename editor\n";
    }, 1500);
    add("focus after ctrl+d Enter", [this] { st_log_ += "# focus: " + st_focus() + "\n"; }, 50);
    add("ctrl+d (2)", [this] { duplicate_item(st_find("Get user")); }, 1200);
    add("esc", [this] {
      if (tree_->GetEditControl()) tree_->EndEditLabel(tree_->GetFocusedItem(), true);
      else st_log_ += "# no rename editor\n";
    }, 1500);
    add("focus after ctrl+d Esc", [this] { st_log_ += "# focus: " + st_focus() + "\n"; }, 50);
    add("delete open tab", [this] {
      const fs::path p = active_file_;
      st_log_ += "# deleting " + u8(p.filename()) + (tab_for(p) ? " (tab open)" : " (no tab)") + "\n";
      if (!p.empty()) delete_confirmed(p, requests_under(p));
    }, 2000);
    add("selection after delete", [this] {
      const auto* d = data(tree_->GetSelection());
      st_log_ += "# selected after delete: " + (d ? d->node->name : std::string("<none>")) + ", active " +
                 u8(active_file_.filename()) + "\n";
    }, 50);
    for (int i = 0; i < 4; ++i) add(i % 2 ? "ctrl+b show" : "ctrl+b hide", [this] {
      if (wxAuiNotebook* nb = notebook()) st_log_ += "# tabs " + std::to_string(nb->GetPageCount()) + "\n";
      set_collapsed(!collapsed_);
    }, 900);
    // What one step of a sash drag costs now (live resize is off: a drag only moves a line, the
    // release lays out once): a release at +40 px, then back.
    for (int dw : {40, -40}) add("sash release " + std::to_string(dw), [this, dw] {
      wxAuiManager* m = wxAuiManager::GetManager(this);
      st_log_ += std::string("# live resize: ") + (m && m->HasLiveResize() ? "on" : "off") + "\n";
      set_pane_width(GetSize().x + FromDIP(dw), m ? m->GetPane(this).min_size : wxDefaultSize);
    }, 900);
    for (const char* q : {"patch", "grpc", "bulk 07", "get user"}) {
      add(std::string("filter '") + q + "'", [this, q] {
        filter_->ChangeValue(wxString::FromUTF8(q));
        apply_filter();
        size_t n = 0;
        std::function<void(const CollectionNode&)> count = [&](const CollectionNode& x) {
          for (const auto& c : x.children) c.is_folder ? count(c) : void(++n);
        };
        count(view_root_);
        st_log_ += "# filter '" + std::string(q) + "': " + std::to_string(n) + " requests\n";
      }, 600);
    }
    add("filter clear", [this] {
      filter_->ChangeValue("");
      apply_filter();
    }, 600);
    add("grpc kinds later", [this] {
      st_log_ += "# grpc kinds known: " + std::to_string(grpc_kinds_.size()) + "\n";
    }, 300);
    for (const char* n : {"Get user", "List users", "Health 1", "Health 1", "Health 1", "Get user", "Create user"}) {
      add(std::string("send ") + n, [this, n] {
        const fs::path p = st_find(n);
        if (p.empty()) {
          st_log_ += "# missing " + std::string(n) + "\n";
          return;
        }
        host_->open_request(p, true);
        if (RequestTab* t = tab_for(p)) t->send();
        else st_log_ += "# no tab for " + std::string(n) + "\n";
      }, 700);
    }
    add("history pane", [this] {
      st_log_ += wxString::Format("# history rows %zu, pane height %d px, tree height %d px\n", history_->shown(),
                                  history_->GetSize().y, tree_->GetSize().y)
                     .utf8_string();
    }, 300);
    add("history open", [this] {
      if (history_->entries().empty()) {
        st_log_ += "# no history\n";
        return;
      }
      open_history_entry(history_->entries().front(), false, false);
    }, 1500);
    add("history open 2", [this] {
      if (history_->entries().size() > 1) open_history_entry(history_->entries()[1], false, false);
    }, 1500);
    // A right click right after the pointer reached a history row: its menu opens.
    add("history right click", [this] {
      const HWND hw = static_cast<HWND>(history_->GetHWND());
      const LPARAM at = MAKELPARAM(FromDIP(60), FromDIP(24 + 12));  // the first entry, under "Today"
      ::PostMessageW(hw, WM_MOUSEMOVE, 0, at);
      ::PostMessageW(hw, WM_RBUTTONDOWN, MK_RBUTTON, at);
      ::PostMessageW(hw, WM_RBUTTONUP, 0, at);
    }, 500);
    add("history menu shown", [this] {
      const HWND m = ::FindWindowW(L"#32768", nullptr);
      DWORD pid = 0;
      if (m) ::GetWindowThreadProcessId(m, &pid);
      const bool shown = m && ::IsWindowVisible(m) && pid == ::GetCurrentProcessId();
      st_log_ += std::string("# history menu: ") + (shown ? "shown" : "NOT shown") + "\n";
      if (shown) ::EndMenu();
    }, 500);
    add("collapse", [this] { st_collapse_all(); }, 600);
    add("quickopen ep0777 (final)", [this] { open_preview(st_find("Endpoint 0777")); }, 1500);
    // The reorder line while a row is dragged (Endpoint 0777 over Endpoint 0780: after it).
    add("drag mark", [this] {
      const wxTreeItemId over = item_for(st_find("Endpoint 0780"), false);
      wxRect r;
      if (!over.IsOk() || !tree_->GetBoundingRect(over, r)) {
        st_log_ += "# drag: no row\n";
        return;
      }
      drag_from_ = st_find("Endpoint 0777");
      ctree_->dragging = true;
      ::SendMessageW(static_cast<HWND>(tree_->GetHWND()), WM_MOUSEMOVE, MK_LBUTTON,
                     MAKELPARAM(r.x + r.width / 2, r.y + r.height / 2));
      st_log_ += std::string("# drag mark: ") + (ctree_->drag_mark(over) == 2 ? "after 0780" : "WRONG") + "\n";
      ctree_->dragging = false;  // the mark stays drawn for a screenshot
      drag_from_.clear();
    }, 500);
    st_build_dup();
  }

  void st_build_dup() {
    auto add = [this](std::string n, std::function<void()> fn, int wait = 800) {
      st_steps_.push_back(TestStep{std::move(n), std::move(fn), wait});
    };
    wxString req_name = "Get user";
    wxGetEnv("WATT_SIDEBAR_SELFTEST_REQ", &req_name);
    const std::string rn = req_name.utf8_string();
    // Regression: Ctrl+D, Enter on a request with a run. The copy was never sent: its tab
    // shows no response (not the original's, kept in the preview tab it reused).
    // WATT_SIDEBAR_SELFTEST_NOSEND: the run is from an earlier session (only in the history).
    const bool send = !wxGetEnv("WATT_SIDEBAR_SELFTEST_NOSEND", nullptr);
    if (send) add("dup-resp send", [this, rn] {
      const fs::path p = st_find(rn);
      host_->open_request(p, true);
      if (RequestTab* t = tab_for(p)) t->send();
    }, 1500);
    add("dup-resp preview", [this, rn] {
      const fs::path p = st_find(rn);
      host_->open_request(p, true);
      RequestTab* t = tab_for(p);
      st_log_ += std::string("# original has response: ") + (t && t->last_response() ? "yes" : "no") +
                 (t && t->preview() ? " (preview)" : " (kept)") + "\n";
    }, send ? 600 : 150);
    add("dup-resp ctrl+d", [this, rn] { duplicate_item(st_find(rn)); }, 1200);
    add("dup-resp enter", [this] {
      if (tree_->GetEditControl()) tree_->EndEditLabel(tree_->GetFocusedItem(), false);
      else st_log_ += "# no rename editor\n";
    }, 2500);
    add("dup-resp check", [this] {
      RequestTab* t = active_file_.empty() ? nullptr : tab_for(active_file_);
      const bool is_copy = t && u8(active_file_.filename()).find("copy") != std::string::npos;
      st_log_ += "# copy tab " + (t ? u8(active_file_.filename()) : std::string("<none>")) + ": " +
                 (!is_copy ? "WRONG TAB" : t->last_response() ? "FAIL inherited a response" : "ok, no response") +
                 "\n";
    }, 50);
  }

  void st_build_tabs() {
    auto add = [this](std::string n, std::function<void()> fn, int wait = 800) {
      st_steps_.push_back(TestStep{std::move(n), std::move(fn), wait});
    };
    auto reqs = std::make_shared<std::vector<fs::path>>();
    add("open 8 tabs", [this, reqs] {
      std::vector<fs::path> all;
      collect_requests(tree_root_, all);
      for (const auto& p : all) {
        if (reqs->size() >= 8) break;
        if (u8(p).find("Shop API") == std::string::npos) continue;
        reqs->push_back(p);
        open_tab(p);
      }
      st_log_ += "# opened " + std::to_string(reqs->size()) + "\n";
    }, 2500);
    for (size_t i = 0; i < 8; ++i)
      add("send " + std::to_string(i), [this, reqs, i] {
        if (i >= reqs->size()) return;
        host_->open_request((*reqs)[i], false);
        if (RequestTab* t = tab_for((*reqs)[i])) t->send();
      }, 1200);
    for (int i = 0; i < 6; ++i)
      add("switch tab " + std::to_string(i), [this, reqs, i] {
        if (static_cast<size_t>(i) < reqs->size()) host_->open_request((*reqs)[static_cast<size_t>(i)], false);
      }, 900);
    for (int i = 0; i < 6; ++i)
      add(i % 2 ? "ctrl+b show" : "ctrl+b hide", [this] { set_collapsed(!collapsed_); }, 900);
    // A sash drag through the frame's own mouse messages (AUI handles the sash on the frame).
    for (int i = 0; i < 4; ++i)
      add("sash release", [this, i] {
        HWND f = static_cast<HWND>(wxGetTopLevelParent(this)->GetHWND());
        const wxPoint at = wxGetTopLevelParent(this)->ScreenToClient(ClientToScreen(wxPoint(GetSize().x, 40)));
        const int x0 = at.x + 2, x1 = x0 + (i % 2 ? -60 : 60);
        ::SendMessageW(f, WM_MOUSEMOVE, 0, MAKELPARAM(x0, at.y));
        ::SendMessageW(f, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x0, at.y));
        ::SendMessageW(f, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM((x0 + x1) / 2, at.y));
        ::SendMessageW(f, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x1, at.y));
        const auto t0 = std::chrono::steady_clock::now();
        ::SendMessageW(f, WM_LBUTTONUP, 0, MAKELPARAM(x1, at.y));
        st_log_ += wxString::Format("#   release %.1f ms, width %d\n",
                                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
                                    GetSize().x)
                       .utf8_string();
      }, 900);
    if (wxString ws2; wxGetEnv("WATT_SIDEBAR_SELFTEST_WS2", &ws2)) {
      const fs::path a = ws_->root(), b = fs::path(ws2.ToStdWstring());
      for (int round = 0; round < 3; ++round) {
        add("switch to ws2", [this, b] { switch_workspace(b, true); }, 2000);
        add("switch back", [this, a] { switch_workspace(a, true); }, 2000);
        add("state", [this] { st_log_ += "# " + st_tab_state() + "\n"; }, 50);
      }
    }
    add("select tab 2", [this, reqs] {
      if (reqs->size() > 2) host_->open_request((*reqs)[2], false);
    }, 900);
    add("delete open tab", [this, reqs] {
      if (reqs->size() < 3) return;
      const fs::path p = (*reqs)[2];
      RequestTab* t = tab_for(p);
      st_log_ += "# deleting " + u8(p.filename()) + (t ? (t->last_response() ? " (tab, response)" : " (tab)") : " (no tab)") + "\n";
      delete_confirmed(p, requests_under(p));
    }, 1500);
    add("delete other tab", [this, reqs] {
      if (reqs->size() < 5) return;
      const fs::path p = (*reqs)[4];
      delete_confirmed(p, requests_under(p));
    }, 1500);
    add("select a folder row", [this, reqs] {
      if (reqs->empty()) return;
      const fs::path folder = reqs->back().parent_path();
      if (wxTreeItemId id = item_for(folder, true); id.IsOk()) tree_->SelectItem(id);
    }, 600);
  }

  // A 10 MB response: its history row and its tree row (last run from the history) reopened.
  void st_build_big() {
    auto add = [this](std::string n, std::function<void()> fn, int wait = 800) {
      st_steps_.push_back(TestStep{std::move(n), std::move(fn), wait});
    };
    add("big send", [this] {
      const fs::path p = st_find("Big payload");
      host_->open_request(p, true);
      if (RequestTab* t = tab_for(p)) t->send();
    }, 6000);
    for (int i = 0; i < 3; ++i) {
      add("other row", [this] { open_preview(st_find("Health")); }, 1500);
      add("big history row", [this] {
        for (const auto& e : history_->entries())
          if (e.summary.find("json/130000") != std::string::npos) {
            open_history_entry(e, false, false);
            return;
          }
        st_log_ += "# no big entry\n";
      }, 3000);
      add("other row", [this] { open_preview(st_find("Health")); }, 1500);
      add("big tree row", [this] { open_preview(st_find("Big payload")); }, 3000);
    }
  }

  std::string st_focus() {
    wxWindow* f = wxWindow::FindFocus();
    if (!f) return "<none>";
    std::string where = wxString(f->GetClassInfo()->GetClassName()).utf8_string();
    for (wxWindow* w = f; w; w = w->GetParent())
      if (auto* t = dynamic_cast<RequestTab*>(w)) {
        where += " in tab " + u8(t->file().filename());
        break;
      }
    return where;
  }

  // Every filled folder row has exactly its view node's children, in order (sync_tree).
  void st_check_rows() {
    int bad = 0, rows = 0;
    std::function<void(const wxTreeItemId&, const CollectionNode&)> check = [&](const wxTreeItemId& item,
                                                                                const CollectionNode& node) {
      std::vector<const CollectionNode*> kids;
      for (const auto& c : node.children) kids.push_back(&c);
      sort_kids(node.path, kids);
      size_t i = 0;
      wxTreeItemIdValue cookie;
      for (wxTreeItemId c = tree_->GetFirstChild(item, cookie); c.IsOk(); c = tree_->GetNextChild(item, cookie), ++i) {
        ++rows;
        const auto* d = data(c);
        if (!d || i >= kids.size() || d->node != kids[i] ||
            tree_->GetItemText(c) != wxString::FromUTF8(kids[i]->name)) {
          ++bad;
          continue;
        }
        if (d->node->is_folder && d->populated) check(c, *d->node);
      }
      if (i != kids.size()) ++bad;
    };
    if (tree_->GetRootItem().IsOk() && root_populated_) check(tree_->GetRootItem(), view_root_);
    st_log_ += wxString::Format("# rows %d, %s\n", rows, bad ? wxString::Format("MISMATCH %d", bad) : wxString("ok"))
                   .utf8_string();
  }

  void selftest_tick() {
    if (!st_probe_) {
      if (!ws_ || scanning_ || tree_root_.children.empty()) {
        st_timer_.StartOnce(200);
        return;
      }
      st_probe_ = new StallProbe(wxGetTopLevelParent(this)->GetHWND());  // lives until exit
      st_build();
      st_timer_.StartOnce(500);
      return;
    }
    if (st_i_ > 0) {
      const TestStep& s = st_steps_[st_i_ - 1];
      for (const auto& [at, ms] : st_probe_->long_stalls())
        st_log_ += wxString::Format("#   STALL at %.1f for %.1f ms\n",
                                    std::chrono::duration<double, std::milli>(at - st_t0_).count(), ms)
                       .utf8_string();
      st_log_ += wxString::Format("%s\t%.1f\t%.1f\n", s.name, st_sync_, st_probe_->max_ms()).utf8_string();
    }
    if (st_i_ >= st_steps_.size()) {
      std::ofstream(st_out_, std::ios::binary) << st_log_;
      if (!wxGetEnv("WATT_SIDEBAR_SELFTEST_STAY", nullptr)) ::TerminateProcess(::GetCurrentProcess(), 0);
      return;
    }
    const TestStep& s = st_steps_[st_i_++];
    st_probe_->reset();
    const auto t0 = std::chrono::steady_clock::now();
    st_t0_ = t0;
    s.fn();
    st_sync_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    st_timer_.StartOnce(s.wait_ms);
  }

  wxWindow* window() override { return this; }

  void set_workspace(Workspace* ws) override {
    // Tabs built before the first workspace arrived (the session's active tab is restored into
    // the window before it shows): their editors loaded the request without the workspace, so
    // nothing they inherit from folder.json (variables, headers, auth) is known to them.
    if (ws && !ws_seen_) {
      ws_seen_ = true;
      if (wxAuiNotebook* nb = notebook())
        for (size_t i = 0; i < nb->GetPageCount(); ++i)
          if (auto* t = dynamic_cast<RequestTab*>(nb->GetPage(i)); t && t->realized())
            stale_tabs_.emplace_back(t);
      heal_log_ += "stale tabs at first workspace: " + std::to_string(stale_tabs_.size()) + "\n";
      if (!stale_tabs_.empty()) later([this] { heal_stale_tabs(); });
    }
    ws_ = ws;
    session_resp_.clear();  // another workspace's history
    session_resp_bytes_ = 0;
    if (!ws_) {
      tree_root_ = CollectionNode{};
      search_keys_.clear();
      rank_keys_.clear();
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
      if (r.kind == RequestKind::Grpc)
        if (RequestTab* t = tab_for(r.file)) learn_grpc_kinds(t);
      const int img = r.kind == RequestKind::Grpc ? grpc_tree_image(grpc_kind_of(r.grpc_method))
                                                  : tree_image(r.kind, r.http.method);
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
    keep_response(e);
  }

  // A gRPC tab shows this file: take the call kinds of its schema for the tree's arrows. The
  // host reuses the preview tab without a page change (no select_file), so every open from
  // the sidebar calls this too.
  void learn_later(const fs::path& file) {
    if (file.empty()) return;
    if (RequestTab* t = tab_for(file); t && t->request_kind() == RequestKind::Grpc && !learn_grpc_kinds(t)) {
      // Its schema is still loading: look again a few times (a pointer compare each).
      learn_file_ = file;
      learn_tries_ = 0;
      learn_timer_.Start(1000);
    }
  }

  fs::path snapshot_origin(const fs::path& file, const std::string& id) const override {
    if (!is_snapshot(file)) return {};
    if (auto it = snapshot_origin_.find(file); it != snapshot_origin_.end() && !it->second.empty()) return it->second;
    // A snapshot restored from the last session: the map is gone, the request id is not.
    if (!id.empty())
      if (auto it = path_by_id_.find(id); it != path_by_id_.end()) return it->second;
    return {};
  }

  void select_file(const fs::path& file_in) override {
    learn_later(file_in);
    if (file_in != active_file_) {
      sticky_sel_.clear();
      // The editor of a tab just opened reads its folder variables on a worker: its "Sent as"
      // line shows them once that has landed.
      if (!file_in.empty()) {
        preview_file_ = file_in;
        preview_timer_.StartOnce(200);
      }
    }
    // Ctrl+P opened a request the filter hides: clear the filter so the tree can show it.
    if (!file_in.empty() && !filter_q_.empty() && sidebar::take_quick_pick(file_in)) {
      filter_->ChangeValue("");
      apply_filter();
    }
    fs::path file = file_in;
    // The history list highlights the entry the active tab shows, and no other row.
    if (auto it = snapshot_entry_.find(file_in); it != snapshot_entry_.end())
      history_->select_entry(it->second.first, it->second.second);
    else
      history_->select_entry({}, {});
    if (is_snapshot(file)) {  // a history snapshot: its saved request, if any
      auto it = snapshot_origin_.find(file);
      file = it == snapshot_origin_.end() ? fs::path() : it->second;
      if (!file.empty() && !find_node(tree_root_, file)) file.clear();  // sent unsaved from a folder
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
    // A row already on screen: just its highlight. Turning the redraw off and on again costs
    // the native tree a re-measure of every row (15-27 ms with 1000 rows open), so that is
    // only for a reveal (folders to open, a scroll).
    wxTreeItemId id = item_for(file, false);
    if (id.IsOk() && tree_->IsVisible(id)) {
      if (tree_->GetSelection() != id) st_timed("  select (on screen)", [&] { tree_->SelectItem(id); });
      syncing_ = false;
      reveal_active_ = false;
      return;
    }
    redraw_off();  // expanding folders and the centring scroll: one repaint, no smooth-scroll animation
    st_timed("  reveal", [&] { id = item_for(file, true); });
    if (id.IsOk() && tree_->GetSelection() != id) {
      st_timed("  select", [&] { tree_->SelectItem(id); });
      st_timed("  centre", [&] { centre_item(id); });
    } else if (id.IsOk()) {
      centre_item(id);  // selected but scrolled away
    } else if (!filter_q_.empty()) {
      // Hidden by the filter (Ctrl+P opened it): no other row may stay highlighted.
      if (const auto* d = data(tree_->GetSelection()); d && !d->node->is_folder) tree_->Unselect();
    }
    st_timed("  redraw", [&] { redraw_on(); });
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
      rail_btn("\xF0\x9F\x8D\xAA", "Cookies", [this] { open_cookies(); });
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
    auto* import = flat_button(p, "Import",
                               "Import a Postman, Insomnia, Bruno or OpenAPI collection, a HAR file or a curl command");
    import->SetForegroundColour(mix(window_colour(), text_colour(), 0.75));
    // No folder: a collection lands at the root, a single curl request where "+" would put it.
    import->Bind(wxEVT_BUTTON, [this, import](wxCommandEvent&) { import_menu(import, fs::path()); });
    import_btn_ = import;
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
    // Cookies of the workspace's jar (Postman's "Cookies" link, Insomnia's Cookie Manager).
    cookie_btn_ = flat_button(p, wxString::FromUTF8("\xF0\x9F\x8D\xAA"), "Cookies: what responses set and requests send");
    {
      wxFont f = cookie_btn_->GetFont();
      f.SetFractionalPointSize(f.GetFractionalPointSize() * 1.1);
      cookie_btn_->SetFont(f);
    }
    cookie_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { open_cookies(); });
    envrow->Add(env_menu_btn_, 1, wxALIGN_CENTER_VERTICAL);
    envrow->Add(cookie_btn_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
    envrow->Add(env_btn_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(2));
    sz->Add(envrow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);
    env_warn_ = new wxStaticText(p, wxID_ANY, "");
    env_warn_->SetForegroundColour(is_dark() ? wxColour(0xF0, 0xA0, 0x3C) : wxColour(0xA0, 0x4F, 0x00));
    env_warn_->Hide();
    sz->Add(env_warn_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);
    env_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { edit_envs(); });
    env_warn_->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
      if (!env_bad_.empty()) fix_env_files();
      else edit_envs();
    });

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
        redraw_off();
        wxTreeItemId first = best.empty() ? wxTreeItemId() : item_for(best, true);
        redraw_on();
        if (enter && first.IsOk()) {
          syncing_ = true;
          redraw_off();
          tree_->SelectItem(first);
          centre_item(first);
          redraw_on();
          syncing_ = false;
          later([this, best] {  // its own UI step
            open_preview(best);
            keep_keyboard(best);
          });
          return;
        }
        if (enter && !filter_q_.empty()) return;  // nothing matches
        ++tree_focus_req_;
        tree_->SetFocus();
        if (first.IsOk()) {
          tree_->SelectItem(first);  // opens the preview
          centre_item(first);
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
    // Right-click on the COLLECTION header: the collection's menu (Run collection, settings), as
    // on the empty tree area below the items.
    coll_label_->SetToolTip("Right-click: run the collection, collection settings");
    coll_label_->Bind(wxEVT_RIGHT_UP, [this](wxMouseEvent&) { item_menu(wxTreeItemId()); });
    thead->Add(coll_label_, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, pad);
    // A passing notice ("Imported 4 requests ..."): one ellipsized line in place of the
    // COLLECTION label for a few seconds (the tree below never moves), never modal (a dialog
    // would take the next clicks). A click runs its action (if any) and dismisses it.
    note_ = new wxStaticText(top, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                             wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
    note_->SetForegroundColour(is_dark() ? wxColour(0x6C, 0xC6, 0x8A) : wxColour(0x1E, 0x7A, 0x3C));
    note_->SetCursor(wxCursor(wxCURSOR_HAND));
    note_->SetFont(note_->GetFont().Bold());
    note_->Hide();
    thead->Add(note_, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, pad);
    note_->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
      auto act = std::move(note_action_);
      note_action_ = nullptr;
      hide_note();
      if (act) act();
    });
    note_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { hide_note(); });
    thead->Add(add, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    tsz->Add(thead, 0, wxEXPAND | wxTOP, FromDIP(6));
    tree_ = ctree_ = new CollectionTree(top, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           wxTR_HIDE_ROOT | wxTR_NO_BUTTONS | wxTR_NO_LINES | wxTR_FULL_ROW_HIGHLIGHT |
                               wxTR_SINGLE | wxTR_EDIT_LABELS | wxBORDER_NONE);
    // No native expand buttons: the folder images carry the chevron next to the icon, and the
    // ~20 px button column at the root is saved. A click on a folder row toggles it, as do
    // Left / Right / Enter.
    tree_->SetImages(tree_images(this));
    tree_->SetIndent(FromDIP(14));
    ctree_->count_colour = dim_colour();
    ctree_->guide_colour = mix(window_colour(), text_colour(), is_dark() ? 0.14 : 0.16);
    ctree_->accent_colour = is_dark() ? wxColour(0x4C, 0x9A, 0xFF) : wxColour(0x1A, 0x73, 0xE8);
    ctree_->match_colour = is_dark() ? wxColour(0xE8, 0xA3, 0x3D) : wxColour(0xD9, 0x7A, 0x00);
    ctree_->filter_query = [this]() -> const std::string& { return filter_q_; };
    // No horizontal scroll bar (a sidebar row is clipped at the edge, as in Bruno). Without it the
    // native tree also stops measuring the widest open row after every expand, collapse and
    // insert: that made each step of a filter cost more the more rows the tree already had.
    {
      const HWND hw = static_cast<HWND>(tree_->GetHWND());
      SetWindowLongPtr(hw, GWL_STYLE, GetWindowLongPtr(hw, GWL_STYLE) | TVS_NOHSCROLL);
    }
    ctree_->folder_count = [this](const wxTreeItemId& id) -> size_t {
      const auto* d = data(id);
      return d && d->node->is_folder ? count_requests(*d->node) : 0;
    };
    // A collection file dropped on a folder row is imported into that folder, anywhere else into
    // the collection root (one file per drop; parsing runs on a worker).
    tree_->SetDropTarget(new FileDrop([this](wxCoord x, wxCoord y, const wxArrayString& files) {
      if (!ws_ || files.empty()) return false;
      int flags = 0;
      const wxTreeItemId id = tree_->HitTest(wxPoint(x, y), flags);
      const auto* d = data(id);
      const fs::path dest = d && d->node->is_folder ? d->node->path : ws_->root();
      drop_import(files, dest);
      return true;
    }));
    tsz->Add(tree_, 1, wxEXPAND | wxTOP, FromDIP(2));
    // Empty collection: a short note and the first steps as buttons (Bruno's Create / Open /
    // Import, Postman's "Create a Collection").
    empty_box_ = new wxPanel(top);
    empty_box_->SetBackgroundColour(window_colour());
    {
      auto* es = new wxBoxSizer(wxVERTICAL);
      empty_ = new wxStaticText(empty_box_, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE);
      empty_->SetForegroundColour(dim_colour());
      es->Add(empty_, 0, wxEXPAND | wxBOTTOM, FromDIP(10));
      empty_btns_ = new wxPanel(empty_box_);
      empty_btns_->SetBackgroundColour(window_colour());
      auto* bs = new wxBoxSizer(wxVERTICAL);
      auto step = [&](const wxString& label, const wxString& tip, bool primary, std::function<void()> fn) {
        auto* b = new wxButton(empty_btns_, wxID_ANY, label, wxDefaultPosition, wxSize(-1, FromDIP(30)),
                               wxBU_LEFT | wxBORDER_NONE);
        b->SetToolTip(tip);
        b->SetBackgroundColour(primary ? highlight_colour() : mix(window_colour(), text_colour(), 0.08));
        b->SetForegroundColour(primary ? highlight_text_colour() : text_colour());
        b->Bind(wxEVT_BUTTON, [fn](wxCommandEvent&) { fn(); });
        bs->Add(b, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
      };
      step("  +   New HTTP request", "A request in this workspace, named in the tree (Ctrl+N for an unsaved tab)", true,
           [this] { new_request_in(RequestKind::Http, ws_ ? ws_->root() : fs::path()); });
      step("  +   New gRPC request", "A gRPC request in this workspace", false,
           [this] { new_request_in(RequestKind::Grpc, ws_ ? ws_->root() : fs::path()); });
      step("  +   New WebSocket", "A ws:// / wss:// connection in this workspace (Ctrl+Alt+T for an unsaved tab)", false,
           [this] { new_request_in(RequestKind::WebSocket, ws_ ? ws_->root() : fs::path()); });
      step("  +   New folder", "A folder to group requests (a plain folder on disk)", false,
           [this] { if (ws_) new_folder_in(ws_->root()); });
      step(wxString::FromUTF8("  \xE2\x86\x93   Import from Postman\xE2\x80\xA6"),
           "A Postman collection (v2.0 / v2.1 export): folders, requests, auth, scripts and variables", false,
           [this] { import_kind(kImportPostman, ws_ ? ws_->root() : fs::path()); });
      step(wxString::FromUTF8("  \xE2\x86\x93   Import Insomnia, Bruno, OpenAPI, HAR, cURL\xE2\x80\xA6"),
           "Insomnia v4 export, Bruno collection folder, OpenAPI 3 / Swagger, HAR file, or a curl command", false,
           [this] {
             wxWindow* b = wxWindow::FindFocus();
             import_menu(b && b->GetParent() == empty_btns_ ? b : empty_btns_, ws_ ? ws_->root() : fs::path());
           });
      step(wxString::FromUTF8("  \xE2\x86\x97   Open folder\xE2\x80\xA6"), "Open another workspace folder (or make one)", false,
           [this] { open_workspace_dialog(false); });
      empty_btns_->SetSizer(bs);
      es->Add(empty_btns_, 0, wxEXPAND);
      empty_box_->SetSizer(es);
    }
    tsz->Add(empty_box_, 1, wxEXPAND | wxALL, FromDIP(12));
    empty_box_->Hide();
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
    history_->grpc_kind = [this](const HistoryEntry& e) { return grpc_kind_of(grpc_method_of(e)); };
    history_->on_delete = [this](const std::vector<HistoryEntry>& v) { delete_history(v); };
    history_->on_clear = [this] { clear_history_all(); };
    history_->on_save_as = [this](const HistoryEntry& e) { save_history_as(e); };
    history_->on_copy_curl = [this](const HistoryEntry& e) { copy_history_curl(e); };
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
    {
      long hh = 0;
      if (wxConfigBase* c = wxConfigBase::Get(); c && c->Read("Sidebar/HistoryHeight", &hh) && hh > 0) {
        user_sash_ = true;
        user_hist_h_ = FromDIP(static_cast<int>(hh));
      }
    }
    // Double click on the divider: back to sizing the history to its rows.
    split_->Bind(wxEVT_SPLITTER_DOUBLECLICKED, [this](wxSplitterEvent& e) {
      e.Veto();  // no unsplit
      user_sash_ = false;
      user_hist_h_ = 0;
      if (wxConfigBase* c = wxConfigBase::Get()) c->DeleteEntry("Sidebar/HistoryHeight");
      update_history_pane();
    });
    split_->SetToolTip("Drag to size the history; double-click to fit it to its rows again");
    split_->Bind(wxEVT_SPLITTER_SASH_POS_CHANGED, [this](wxSplitterEvent& e) {
      e.Skip();
      // The user dragged the sash: keep the history height they chose. (Resizes and
      // SetSashPosition() can report here too; only a drag has the button down.)
      if (wxGetMouseState().LeftIsDown() && !hist_full_) {
        user_sash_ = true;
        user_hist_h_ = split_->GetClientSize().y - e.GetSashPosition() - split_->GetSashSize();
        if (wxConfigBase* c = wxConfigBase::Get()) c->Write("Sidebar/HistoryHeight", static_cast<long>(ToDIP(user_hist_h_)));
      }
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
      ctree_->count_colour = dim_colour();
      ctree_->match_colour = is_dark() ? wxColour(0xE8, 0xA3, 0x3D) : wxColour(0xD9, 0x7A, 0x00);
      history_->Refresh();
    });
  }

  void toggle_history_full() {
    hist_full_ = !hist_full_;
    hist_expand_->SetLabel(hist_full_ ? "Restore" : "Expand");
    hist_expand_->SetToolTip(hist_full_ ? "Show the collection again" : "Show history at full height (double-click HISTORY)");
    hist_expand_->GetParent()->Layout();
    // The filter box stays above the full-height history and filters it (Postman's History
    // search): its hint says so.
    filter_->SetHint(hist_full_ ? "Filter history by request, URL or method" : "Filter by name, folder, method or URL");
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
    if (!sash_set_ || !split_->IsSplit()) return;
    const int h = split_->GetClientSize().y;
    if (h < FromDIP(160)) return;
    int want;
    if (user_sash_) {
      // The height the user dragged it to (kept across restarts), within the sidebar.
      if (user_hist_h_ <= 0) return;
      want = std::clamp(user_hist_h_, FromDIP(64), h - FromDIP(64));
    } else if (history_->total() == 0) {
      want = FromDIP(64);
    } else {
      // The rows it shows (day headers included) plus its header, no empty space under them,
      // up to 40% of the sidebar: the collection keeps the rest.
      want = FromDIP(34) + history_->content_height(12) + FromDIP(4);
      want = std::clamp(want, FromDIP(96), std::max(FromDIP(96), h * 40 / 100));
    }
    const int pos = h - want - split_->GetSashSize();
    if (std::abs(split_->GetSashPosition() - pos) > 2) split_->SetSashPosition(pos);
  }

  // Gives the sidebar's AUI pane width w with one frame relayout (see AuiDocks), painted once:
  // the frame is frozen until the editors and the response have their new size.
  void set_pane_width(int w, const wxSize& min_size) {
    wxAuiManager* m = wxAuiManager::GetManager(this);
    if (!m) return;
    wxAuiPaneInfo& pane = m->GetPane(this);
    if (!pane.IsOk()) return;
    RedrawLock lock(wxGetTopLevelParent(this));
    pane.Show().BestSize(wxSize(w, -1)).MinSize(min_size);
    bool sized = false;
    for (auto& dock : AuiDocks::of(m))
      for (wxAuiPaneInfo* pi : dock.panes)
        if (pi && pi->window == this) {
          dock.size = w;
          sized = true;
        }
    if (!sized) {  // not docked yet: re-dock so AUI takes the best size
      pane.Hide();
      aui_update(m);
      pane.Show();
    }
    aui_update(m);
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
    st_timed("collapse: own layout", [&] {
      wxWindowUpdateLocker lock(this);
      content_->Show(!c);
      rail_->Show(c);
      Layout();
    });
    st_timed("collapse: aui", [&] { set_pane_width(w, c ? wxSize(w, -1) : (saved_min_.IsFullySpecified() ? saved_min_ : wxSize(FromDIP(160), -1))); });
    // Shown again: the keyboard goes to the tree (the filter while it holds a query), so the arrow
    // keys work at once instead of landing on the frame.
    if (!c) {
      auto focus_in = [this] {
        if (!filter_->GetValue().empty()) filter_->SetFocus();
        else tree_->SetFocus();
        // A tree with no selection takes the focus but the arrow keys have no row to move from.
        if (filter_->GetValue().empty() && !tree_->GetSelection().IsOk()) {
          wxTreeItemIdValue cookie;
          if (wxTreeItemId first = tree_->GetFirstChild(tree_->GetRootItem(), cookie); first.IsOk())
            tree_->SetFocusedItem(first);
        }
      };
      focus_in();
      // The frame's AUI update and the key's own WM_CHAR can still move the focus back to the
      // frame after this returns: check again once they are handled.
      later([this, focus_in] {
        if (collapsed_) return;
        wxWindow* f = wxWindow::FindFocus();
        bool inside = false;
        for (wxWindow* w = f; w && !inside; w = w->GetParent()) inside = w == this;
        if (!inside) focus_in();
      });
      later([this] { update_history_pane(); });
    }
  }

  void bind_tree() {
    {
      // A bright accent, not the selection colour (too dark for a 2 px line in dark mode).
      const wxColour c = is_dark() ? wxColour(0x4C, 0x9A, 0xFF) : wxColour(0x1A, 0x73, 0xE8);
      SendMessage(static_cast<HWND>(tree_->GetHWND()), TVM_SETINSERTMARKCOLOR, 0,
                  static_cast<LPARAM>(RGB(c.Red(), c.Green(), c.Blue())));
    }
    // The drop of a row onto a sibling reorders (see END_DRAG): it takes the sibling's place,
    // so the line goes after it when moving down and before it when moving up.
    ctree_->drag_mark = [this](const wxTreeItemId& hit) -> int {
      const auto* d = data(hit);
      if (!d || drag_from_.empty() || same_path(d->node->path, drag_from_)) return 0;
      if (!same_path(d->node->path.parent_path(), drag_from_.parent_path())) return 0;
      if (d->node->is_folder && !wxGetKeyState(WXK_ALT)) return 0;
      const wxTreeItemId parent = tree_->GetItemParent(hit);
      int from_i = -1, hit_i = -1, i = 0;
      wxTreeItemIdValue cookie;
      for (wxTreeItemId c = tree_->GetFirstChild(parent, cookie); c.IsOk(); c = tree_->GetNextChild(parent, cookie), ++i) {
        if (c == hit) hit_i = i;
        else if (const auto* cd = data(c); cd && same_path(cd->node->path, drag_from_)) from_i = i;
      }
      if (from_i < 0 || hit_i < 0) return 0;
      return hit_i > from_i ? 2 : 1;
    };
    // Every deleted row sends one of these, and as a command event it would travel up through
    // the sidebar and the frame looking for a handler: stopped here (no one needs it).
    tree_->Bind(wxEVT_TREE_DELETE_ITEM, [](wxTreeEvent&) {});
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
      sticky_sel_.clear();
      // The user picked a row here: the history list no longer marks a current entry (the
      // preview tab is reused without a page change, so no select_file clears it).
      history_->select_entry({}, {});
      const auto* d = data(e.GetItem());
      if (!d || d->node->is_folder) return;
      host_->open_request(d->node->path, true);
      learn_later(d->node->path);
      // A preview keeps the keyboard in the tree (arrows, F2, Del), as in VS Code; the tab may
      // have moved the focus to its URL field.
      CallAfter([this] {
        if (tree_->IsShownOnScreen()) tree_->SetFocus();
      });
    });
    tree_->Bind(wxEVT_TREE_ITEM_ACTIVATED, [this](wxTreeEvent& e) {
      const auto* d = data(e.GetItem());
      if (!d) return;
      if (d->node->is_folder) {
        tree_->Toggle(e.GetItem());
        return;
      }
      const fs::path p = d->node->path;
      host_->open_request(p, false);
      learn_later(p);
      keep_keyboard(p);
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
          learn_later(p);
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
        case WXK_ESCAPE:  // as in the filter box: clear the filter, the row stays selected
          if (!filter_->IsEmpty()) {
            filter_->Clear();
            apply_filter();
            reveal_active();
            return;
          }
          e.Skip();
          return;
        case WXK_DELETE:
        case WXK_NUMPAD_DELETE:
          if (sel.IsOk()) delete_item(sel);
          return;
        case 'C':  // Ctrl+C / Ctrl+V: copy a request or folder, paste it into another folder (Bruno)
          if (e.GetKeyEvent().GetModifiers() == wxMOD_CONTROL) {
            if (const auto* d = data(sel)) copy_item(d->node->path);
            return;
          }
          e.Skip();
          return;
        case 'V':
          if (e.GetKeyEvent().GetModifiers() == wxMOD_CONTROL) {
            const auto* d = data(sel);
            if (ws_) paste_into(!d ? ws_->root() : d->node->is_folder ? d->node->path : d->node->path.parent_path());
            return;
          }
          e.Skip();
          return;
        case 'D':  // Ctrl+D: duplicate (Bruno, Postman)
          if (e.GetKeyEvent().GetModifiers() == wxMOD_CONTROL) {
            if (const auto* d = data(sel)) {
              if (d->node->is_folder) duplicate_folder(d->node->path);
              else duplicate_item(d->node->path);
            }
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
        // Enter or Esc on the proposed name: the item keeps it and opens with the caret in its
        // URL, so what is typed next goes there.
        // A posted step of its own: a CallAfter would run in the same idle pass as the deferred
        // scan above (one stall of both).
        if (open_after) later([this, from] { open_tab_for_edit(from); });
        return;
      }
      e.Veto();  // the rescan relabels it
      CallAfter([this, from, name, open_after] {
        fs::path to;
        st_timed("rename", [&] { to = rename_node(from, name); });
        // The tab opens in a later step: the rename (a file move, the tabs following it) and the
        // tab (built or swapped by the host) stay separate UI steps.
        if (open_after) later([this, p = to.empty() ? from : to] { open_tab_for_edit(p); });
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
        ctree_->dragging = true;
      }
    });
    tree_->Bind(wxEVT_TREE_END_DRAG, [this](wxTreeEvent& e) {
      ctree_->dragging = false;
      ctree_->clear_drag_mark();
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
    const auto* d = data(id);
    const int img = d && d->node->children.empty() ? kImgFolderEmpty : open ? kImgFolderOpen : kImgFolder;
    if (tree_->GetItemImage(id) == img) return;
    tree_->SetItemImage(id, img);
    tree_->SetItemImage(id, img, wxTreeItemIcon_Selected);
  }

  ItemData* data(const wxTreeItemId& id) const {
    return id.IsOk() ? static_cast<ItemData*>(tree_->GetItemData(id)) : nullptr;
  }

  wxTreeItemId add_item(const wxTreeItemId& parent, const CollectionNode& c) {
    const int img = c.is_folder ? (c.children.empty() ? kImgFolderEmpty : kImgFolder)
                                : (c.badge ? node_image(c) : kImgUnknown);
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
    // While filtering, matched folders open until ~120 rows are in the tree (a screenful and a
    // half); expand_more() opens the rest in later steps of ~80 rows, up to ~300.
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
        if (same_path(d->node->path, p)) return c;
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
    // A scan pass or a filter keystroke updates the rows in place (sync_tree); anything else
    // (collapse all, a new order, a workspace switch) rebuilds them.
    if (purging_) {  // the old rows go first (purge_step), then this rebuild
      rebuild_after_purge_ = true;
      sync_ok_ = false;
      return;
    }
    const bool try_sync = sync_ok_;
    sync_ok_ = false;
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
    // A row just moved or reordered stays selected through the rescans that follow.
    if (!sticky_sel_.empty()) sel_path = sticky_sel_;
    if (const auto* d = data(tree_->GetFirstVisibleItem())) top_path = d->node->path;
    const bool query_changed = filter_q_ != rebuilt_q_;
    // A cleared filter closes the folders it opened (also those a step still has to update when
    // a rescan comes first).
    unfilter_ = filter_q_.empty() && ((query_changed && !rebuilt_q_.empty()) || (unfilter_ && !sync_queue_.empty()));
    rebuilt_q_ = filter_q_;

    // The items point into view_root_: the old view lives until they point into the new one.
    CollectionNode old_view = std::move(view_root_);
    if (filter_q_.empty()) {
      view_root_ = tree_root_;
    } else {
      view_root_ = CollectionNode{};
      if (search_keys_.empty()) build_keys(tree_root_, "", meta_, search_keys_);
      std::vector<const CollectionNode*> hits;
      filter_node(tree_root_, filter_q_, search_keys_, view_root_, &hits,
                  phrase_names_folder(tree_root_, filter_q_, search_keys_));
      rank_hits(hits);
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
    redraw_off();
    if (best_item_.IsOk()) tree_->SetItemBold(best_item_, false);  // before it may be deleted
    best_item_ = wxTreeItemId();
    const uint64_t gen = ++rebuild_gen_;
    bool synced = false;
    if (try_sync) {
      synced = sync_tree();
      // The highlight follows the active tab (also when its row was not in the tree before).
      if (synced && !sel_path.empty()) {
        const auto* d = data(tree_->GetSelection());
        if (!d || !same_path(d->node->path, sel_path))
          if (wxTreeItemId id = item_for(sel_path, filter_q_.empty()); id.IsOk()) {
            tree_->SelectItem(id);
            if (sel_path == active_file_) reveal_active_ = false;
          }
      }
    }
    if (!synced) {
      tree_->UnselectAll();  // deleting the selected item would move the focus to the tree
      delete_all_rows();
      root_populated_ = false;
      expand_budget_ = 0;  // a filter's matched folders open breadth first, below
      const wxTreeItemId root = tree_->AddRoot("root");
      populate(root);
      if (!filter_q_.empty()) expand_more(gen, 0);  // the first ~100 rows in this step
      // While filtering, the active request is highlighted when its row is in the tree; its
      // folder is not opened for it (that is up to 100 more rows per keystroke).
      if (!sel_path.empty()) {
        if (wxTreeItemId id = item_for(sel_path, filter_q_.empty()); id.IsOk()) {
          tree_->SelectItem(id);
          if (sel_path == active_file_) reveal_active_ = false;
        }
      }
      if (!query_changed && !top_path.empty())
        if (wxTreeItemId id = item_for(top_path, false); id.IsOk()) set_first_visible(id);
    }
    if (!filter_q_.empty()) later([this, gen] { expand_more(gen, 1); });
    mark_best_match();
    // A new query shows its results from the first row (the best match is ranked into view);
    // the old view's scroll position belongs to another, longer list. A rescan with the same
    // query keeps what the user sees.
    if (query_changed && filter_q_.empty() && tree_->GetSelection().IsOk()) {
      centre_item(tree_->GetSelection());  // a cleared filter returns to the active row
    } else if (query_changed) {
      wxTreeItemIdValue cookie;
      if (wxTreeItemId first = tree_->GetFirstChild(tree_->GetRootItem(), cookie); first.IsOk())
        set_first_visible(first);
      if (best_item_.IsOk() && !row_in_view(best_item_)) centre_item(best_item_);
    }
    redraw_on();
    // Rows of folders still queued for an update point into the old view: it lives until the
    // queue is done. Otherwise no row points into it: freed on a worker (10k nodes: ~5 ms).
    stale_views_.push_back(std::move(old_view));
    if (sync_queue_.empty()) free_stale_views();
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
    bool steps = false;
    if (!ws_) msg = "Opening workspace...";
    else if (!filter_q_.empty()) msg = "No saved request matches the filter.";
    else if (scanning_ && tree_root_.children.empty()) msg = "Loading...";
    else {
      msg = "No saved requests yet. Requests are plain .req.json files in this folder, so it works with git; "
            "Ctrl+S saves the current tab here.";
      steps = true;
    }
    if (empty) {
      empty_->SetLabel(msg);
      empty_->Wrap(std::max(FromDIP(120), tree_->GetParent()->GetClientSize().x - FromDIP(24)));
      empty_->InvalidateBestSize();
      empty_->SetMinSize(wxSize(-1, empty_->GetBestSize().y));
      if (empty_btns_->IsShown() != steps) empty_btns_->Show(steps);
      empty_box_->Layout();
    }
    if (empty_box_->IsShown() != empty || tree_->IsShown() == empty) {
      empty_box_->Show(empty);
      tree_->Show(!empty);
      tree_->GetParent()->Layout();
    }
    update_counts();
    if (!pending_edit_.empty() && !scanning_) {
      const fs::path p = std::move(pending_edit_);
      pending_edit_.clear();
      // Selecting, scrolling to and focusing the new row: the next UI step (the scan that
      // brought it in already took one).
      later([this, p] { name_new_item(p); });
    }
    if (select_after_scan_ && !scanning_) {
      select_after_scan_ = false;
      const fs::path p = std::move(pending_select_);
      pending_select_.clear();
      syncing_ = true;
      redraw_off();
      if (wxTreeItemId id = item_for(p, true); id.IsOk()) {
        tree_->SelectItem(id);
        centre_item(id);
      }
      redraw_on();
      syncing_ = false;
      sticky_sel_ = p;
      // A pasted copy: the preview tab shows it (not the original it was copied from).
      if (preview_after_scan_) {
        preview_after_scan_ = false;
        later([this, p] { open_preview(p); });
      }
    }
  }

  // ---------------------------------------------------------------- row updates
  //
  // Native tree costs at 2000 open rows: inserting or deleting a leaf row ~20 us, but opening,
  // closing or deleting a folder that has open rows below it ~0.4 ms each (the tree walks every
  // open row). So rows are deleted leaves first, and a big update is split: the top folders are
  // synced one by one in time-boxed UI steps (those on screen first), never 200 in one step.

  struct SyncWork {
    wxTreeItemId row;
    const CollectionNode* node;  // null: the row (a folder with rows) left the view
  };

  HWND tree_hwnd() const { return static_cast<HWND>(tree_->GetHWND()); }
  static HTREEITEM tv_next(HWND hw, HTREEITEM it, WPARAM how) {
    return reinterpret_cast<HTREEITEM>(SendMessage(hw, TVM_GETNEXTITEM, how, reinterpret_cast<LPARAM>(it)));
  }
  static HTREEITEM tv_top(HWND hw, HTREEITEM it) {
    while (it)
      if (HTREEITEM p = tv_next(hw, it, TVGN_PARENT)) it = p;
      else break;
    return it;
  }
  int page_rows() const {
    const int h = std::max(1, static_cast<int>(SendMessage(tree_hwnd(), TVM_GETITEMHEIGHT, 0, 0)));
    return tree_->GetClientSize().y / h + 1;
  }

  // Deletes every row below `parent` (not parent itself), deepest and last rows first.
  void delete_descendants(HTREEITEM parent) {
    const HWND hw = tree_hwnd();
    std::vector<HTREEITEM> kids;
    for (HTREEITEM c = tv_next(hw, parent, TVGN_CHILD); c; c = tv_next(hw, c, TVGN_NEXT)) kids.push_back(c);
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
      if (tv_next(hw, *it, TVGN_CHILD)) delete_descendants(*it);
      SendMessage(hw, TVM_DELETEITEM, 0, reinterpret_cast<LPARAM>(*it));
    }
  }

  // Every row (the caller has unselected): ~45 ms for 2200 open rows instead of ~100-230.
  void delete_all_rows() {
    const HWND hw = tree_hwnd();
    std::vector<HTREEITEM> tops;
    for (HTREEITEM c = tv_next(hw, nullptr, TVGN_ROOT); c; c = tv_next(hw, c, TVGN_NEXT)) tops.push_back(c);
    for (auto it = tops.rbegin(); it != tops.rend(); ++it)
      if (tv_next(hw, *it, TVGN_CHILD)) delete_descendants(*it);
    tree_->DeleteAllItems();  // the (now childless) top rows and wx's hidden root
    sync_queue_.clear();
  }

  // Another workspace's rows (a switch): deleted in steps of ~10 ms while the tree is hidden
  // behind "Loading...", instead of 40-60 ms in the switch's own step. rebuild() waits for it.
  void purge_rows_async() {
    if (purging_) return;
    purging_ = true;
    sync_queue_.clear();
    ++rebuild_gen_;  // queued updates and filter steps of the old rows stop
    hide_hover();
    syncing_ = true;
    tree_->UnselectAll();
    syncing_ = false;
    if (best_item_.IsOk()) tree_->SetItemBold(best_item_, false);
    best_item_ = wxTreeItemId();
    empty_->SetLabel("Loading...");
    if (empty_btns_->IsShown()) empty_btns_->Show(false);
    empty_box_->Show();
    tree_->Hide();
    tree_->GetParent()->Layout();
    purge_step();
  }
  void purge_step() {
    const auto t0 = std::chrono::steady_clock::now();
    const HWND hw = tree_hwnd();
    rebuilding_ = true;
    for (;;) {
      HTREEITEM last = nullptr;
      for (HTREEITEM c = tv_next(hw, nullptr, TVGN_ROOT); c; c = tv_next(hw, c, TVGN_NEXT)) last = c;
      if (!last) break;
      // The last top row's rows, its last folder's first, then the row itself.
      HTREEITEM victim = last;
      while (HTREEITEM c = tv_next(hw, victim, TVGN_CHILD)) {
        HTREEITEM l = c;
        while (HTREEITEM n = tv_next(hw, l, TVGN_NEXT)) l = n;
        victim = l;
      }
      SendMessage(hw, TVM_DELETEITEM, 0, reinterpret_cast<LPARAM>(victim));
      if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() >= 10.0) {
        rebuilding_ = false;
        later([this] { purge_step(); });
        return;
      }
    }
    tree_->DeleteAllItems();
    root_populated_ = false;
    rebuilding_ = false;
    purging_ = false;
    if (sync_queue_.empty()) free_stale_views();
    if (rebuild_after_purge_) {
      rebuild_after_purge_ = false;
      rebuild();
    }
  }

  // A folder row back to closed and not filled (its rows are inserted when it is opened).
  void reset_row(const wxTreeItemId& id, const CollectionNode& n) {
    const auto hi = static_cast<HTREEITEM>(id.GetID());
    delete_descendants(hi);
    TVITEMW tv{};
    tv.mask = TVIF_HANDLE | TVIF_STATE;
    tv.hItem = hi;
    tv.stateMask = TVIS_EXPANDED | TVIS_EXPANDEDONCE;
    SendMessage(tree_hwnd(), TVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&tv));
    if (ItemData* d = data(id)) d->populated = false;
    tree_->SetItemHasChildren(id, !n.children.empty());
  }

  void open_row(const wxTreeItemId& id) {
    populate(id);
    tree_->Expand(id);
    folder_image(id, true);
  }

  bool holds_selection(const wxTreeItemId& it) const {
    for (wxTreeItemId p = tree_->GetSelection(); p.IsOk(); p = tree_->GetItemParent(p))
      if (p == it) return true;
    return false;
  }

  void free_stale_views() {
    if (stale_views_.empty()) return;
    std::thread([o = std::move(stale_views_)] {}).detach();  // 10k nodes: ~5 ms, off the UI thread
    stale_views_.clear();
  }

  // The rows follow the new view_root_ in place: rows whose request or folder is still in the
  // view are kept (renamed / re-badged), the others deleted, new ones inserted where they
  // belong. A scan that changed one file or a filter keystroke costs a few rows, not all of
  // them. Folders are updated one at a time (sync_queue_): those on screen now, the rest
  // within ~8 ms per later UI step. False when there is no tree yet.
  bool sync_tree() {
    const wxTreeItemId root = tree_->GetRootItem();
    if (!root.IsOk() || !root_populated_) return false;
    const int saved = expand_budget_;
    expand_budget_ = 0;  // a filter's closed folders are opened by expand_more()
    sync_queue_.clear();
    deferring_ = true;
    sync_children(root, view_root_);
    deferring_ = false;
    expand_budget_ = saved;
    run_sync_queue(rebuild_gen_, 8.0);
    return true;
  }

  // A filled folder row whose node may have changed: its rows follow the node. A folder that a
  // filter opened is closed again once the filter is cleared, unless the user had it open.
  void sync_folder_row(const wxTreeItemId& id, const CollectionNode& n) {
    ItemData* d = data(id);
    if (!d) return;
    if (!d->populated) {  // a new row of a folder the user had open
      if (filter_q_.empty() && expanded_.count(n.path)) open_row(id);
      return;
    }
    if (unfilter_ && !expanded_.count(n.path) && !holds_selection(id)) reset_row(id, n);
    else sync_children(id, n);
    folder_image(id, tree_->IsExpanded(id));
  }

  // Whether a row is (at least partly) inside the tree's client area.
  bool row_on_screen(const wxTreeItemId& id) const {
    RECT rc{};
    *reinterpret_cast<HTREEITEM*>(&rc) = static_cast<HTREEITEM>(id.GetID());
    if (!SendMessage(tree_hwnd(), TVM_GETITEMRECT, FALSE, reinterpret_cast<LPARAM>(&rc))) return false;
    return rc.bottom > 0 && rc.top < tree_->GetClientSize().y;
  }

  void run_sync_work(const SyncWork& w) {
    if (w.node) {
      sync_folder_row(w.row, *w.node);
      return;
    }
    const auto hi = static_cast<HTREEITEM>(w.row.GetID());  // a folder row that left the view
    delete_descendants(hi);
    SendMessage(tree_hwnd(), TVM_DELETEITEM, 0, reinterpret_cast<LPARAM>(hi));
  }

  // Runs queued folder updates: every one whose row is on screen (also those that come into
  // view as rows above close), then others for up to budget_ms; the rest in later steps. What
  // is on screen stays put: the selected row keeps its place when it is on screen, otherwise
  // the first visible row's top folder stays the first row.
  void run_sync_queue(uint64_t gen, double budget_ms) {
    if (gen != rebuild_gen_) return;
    if (sync_queue_.empty()) {
      free_stale_views();
      return;
    }
    if (tree_->GetEditControl()) {  // a row is being renamed: the rows are rebuilt after that
      rebuild_after_edit_ = true;
      return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
    const HWND hw = tree_hwnd();
    const bool nested = rebuilding_;  // inside rebuild(): it keeps the view itself
    HTREEITEM anchor = nullptr;
    int offset = 0;
    if (!nested) {
      hide_hover();
      rebuilding_ = true;
      const auto sel = static_cast<HTREEITEM>(tree_->GetSelection().GetID());
      HTREEITEM v = tv_next(hw, nullptr, TVGN_FIRSTVISIBLE);
      for (int i = 0, n = page_rows(); sel && v && i < n; ++i, v = tv_next(hw, v, TVGN_NEXTVISIBLE))
        if (v == sel) {
          anchor = sel;
          offset = i;
          break;
        }
      if (!anchor) anchor = tv_top(hw, tv_next(hw, nullptr, TVGN_FIRSTVISIBLE));
    }
    redraw_off();
    deferring_ = true;  // folders below a synced one are queued too (one folder's rows per item)
    // On screen first (a pass can bring more rows into view; at most ~24 ms for all of them).
    for (bool any = true; any && elapsed() < 24.0;) {
      any = false;
      for (size_t i = 0; i < sync_queue_.size() && elapsed() < 24.0;) {
        if (!row_on_screen(sync_queue_[i].row)) {
          ++i;
          continue;
        }
        const SyncWork w = sync_queue_[i];
        sync_queue_.erase(sync_queue_.begin() + static_cast<std::ptrdiff_t>(i));
        run_sync_work(w);
        any = true;
      }
    }
    while (!sync_queue_.empty() && elapsed() < budget_ms) {
      const SyncWork w = sync_queue_.front();
      sync_queue_.pop_front();
      run_sync_work(w);
    }
    deferring_ = false;
    if (anchor) {
      HTREEITEM top = anchor;
      for (int i = 0; i < offset; ++i)
        if (HTREEITEM p = tv_next(hw, top, TVGN_PREVIOUSVISIBLE)) top = p;
      if (tv_next(hw, nullptr, TVGN_FIRSTVISIBLE) != top)
        SendMessage(hw, TVM_SELECTITEM, TVGN_FIRSTVISIBLE, reinterpret_cast<LPARAM>(top));
    }
    redraw_on();
    if (!nested) rebuilding_ = false;
    if (sync_queue_.empty()) free_stale_views();
    else later([this, gen] { run_sync_queue(gen, 8.0); });
  }

  // The rows of `item` follow node's children. While deferring_, the folders below it (kept,
  // dropped with rows, or new ones to open) are queued for run_sync_queue instead.
  void sync_children(const wxTreeItemId& item, const CollectionNode& node) {
    std::vector<const CollectionNode*> kids;
    kids.reserve(node.children.size());
    for (const auto& c : node.children) kids.push_back(&c);
    sort_kids(node.path, kids);
    // Rows and kids are both in tree order: a row is kept when its node comes later in kids
    // than the last kept one (names are unique among siblings).
    auto key = [](const CollectionNode& n) {
      std::wstring s = n.path.native();
      std::replace(s.begin(), s.end(), L'/', L'\\');
      s += n.is_folder ? L"|d" : L"|f";
      return s;
    };
    std::unordered_map<std::wstring, size_t> index;
    index.reserve(kids.size() * 2);
    for (size_t i = 0; i < kids.size(); ++i) index.emplace(key(*kids[i]), i);
    std::vector<wxTreeItemId> row_for(kids.size());
    std::vector<wxTreeItemId> drop;
    size_t next = 0;
    wxTreeItemIdValue cookie;
    for (wxTreeItemId c = tree_->GetFirstChild(item, cookie); c.IsOk(); c = tree_->GetNextChild(item, cookie)) {
      const auto* d = data(c);
      const auto it = d ? index.find(key(*d->node)) : index.end();
      if (it != index.end() && it->second >= next) {
        row_for[it->second] = c;
        next = it->second + 1;
      } else {
        drop.push_back(c);
      }
    }
    for (const wxTreeItemId& it : drop)
      if (holds_selection(it)) {
        tree_->UnselectAll();  // deleting the selected row would move the focus to the tree
        break;
      }
    const HWND hw = tree_hwnd();
    for (auto it = drop.rbegin(); it != drop.rend(); ++it) {
      const auto hi = static_cast<HTREEITEM>(it->GetID());
      if (deferring_ && tv_next(hw, hi, TVGN_CHILD)) {
        sync_queue_.push_back({*it, nullptr});
        continue;
      }
      delete_descendants(hi);
      SendMessage(hw, TVM_DELETEITEM, 0, reinterpret_cast<LPARAM>(hi));
    }
    const bool filtering = !filter_q_.empty();
    wxTreeItemId prev;
    for (size_t i = 0; i < kids.size(); ++i) {
      const CollectionNode* n = kids[i];
      wxTreeItemId row = row_for[i];
      if (!row.IsOk()) {  // new: after the previous row, closed (a filter's expand_more opens it)
        const int img = n->is_folder ? (n->children.empty() ? kImgFolderEmpty : kImgFolder)
                                     : (n->badge ? node_image(*n) : kImgUnknown);
        auto* d = new ItemData(n);
        row = prev.IsOk() ? tree_->InsertItem(item, prev, wxString::FromUTF8(n->name), img, img, d)
                          : tree_->PrependItem(item, wxString::FromUTF8(n->name), img, img, d);
        if (n->is_folder && !n->children.empty()) {
          tree_->SetItemHasChildren(row, true);
          if (!filtering && expanded_.count(n->path)) {  // the user had it open
            if (deferring_) sync_queue_.push_back({row, n});
            else open_row(row);
          }
        }
        prev = row;
        continue;
      }
      prev = row;
      ItemData* d = data(row);
      d->node = n;
      if (const wxString name = wxString::FromUTF8(n->name); tree_->GetItemText(row) != name) tree_->SetItemText(row, name);
      if (n->is_folder) {
        if (!d->populated) {
          tree_->SetItemHasChildren(row, !n->children.empty());
          folder_image(row, tree_->IsExpanded(row));  // gained or lost its chevron
        } else if (deferring_) {
          sync_queue_.push_back({row, n});
        } else {
          sync_folder_row(row, *n);
        }
        continue;
      }
      const int img = n->badge ? node_image(*n) : kImgUnknown;
      if (tree_->GetItemImage(row) != img) {
        tree_->SetItemImage(row, img);
        tree_->SetItemImage(row, img, wxTreeItemIcon_Selected);
      }
    }
  }

  // A new or duplicated item is in the tree: select it and open the rename editor on it.
  void name_new_item(const fs::path& p) {
    redraw_off();  // the reveal and its scroll: one repaint, no smooth-scroll animation
    syncing_ = true;  // the item is being named: no preview (it was opened by its creator)
    const wxTreeItemId id = item_for(p, true);
    if (id.IsOk()) {
      tree_->SelectItem(id);
      centre_item(id);
    }
    syncing_ = false;
    redraw_on();
    if (!id.IsOk()) {
      if (open_after_edit_ == p) {
        open_after_edit_.clear();
        CallAfter([this, p] { open_tab(p); });
      }
      return;
    }
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
  }

  // A filter keystroke inserts only ~120 rows (deleting and inserting rows is what a rebuild
  // costs: ~25 us each). The matched folders it left closed open here, ~100 rows per UI step
  // (each step a few ms), breadth first: every matching top folder is open before any deeper
  // one, so a broad query shows all its folders, not the first three fully. Up to kFilterRows
  // rows; a newer rebuild (the next keystroke) cancels it. A folder the user closed stays closed.
  static constexpr unsigned kFilterRows = 2500;
  void expand_more(uint64_t gen, int step) {
    if (gen != rebuild_gen_ || filter_q_.empty() || tree_->GetEditControl() || step > 64) return;
    if (tree_->GetCount() >= kFilterRows) return;
    if (!sync_queue_.empty()) {  // the rows are still being updated: after them
      later([this, gen, step] { expand_more(gen, step); });
      return;
    }
    int budget = 100;
    bool more = false;
    const int saved_budget = expand_budget_;
    expand_budget_ = 0;  // populate() opens no nested folder by itself: the walk does
    redraw_off();
    // Opening folders must not move the view (the first rows, or wherever the user scrolled).
    const wxTreeItemId top = tree_->GetFirstVisibleItem();
    std::vector<wxTreeItemId> level{tree_->GetRootItem()}, next;
    while (!level.empty() && !more) {
      next.clear();
      for (const wxTreeItemId& parent : level) {
        wxTreeItemIdValue cookie;
        for (wxTreeItemId c = tree_->GetFirstChild(parent, cookie); c.IsOk(); c = tree_->GetNextChild(parent, cookie)) {
          ItemData* d = data(c);
          if (!d || !d->node->is_folder || d->node->children.empty()) continue;
          if (!d->populated) {
            if (budget <= 0 || tree_->GetCount() >= kFilterRows) {
              more = budget <= 0 && tree_->GetCount() < kFilterRows;
              break;
            }
            populate(c);
            tree_->Expand(c);
            folder_image(c, true);
            budget -= static_cast<int>(d->node->children.size());
          }
          if (tree_->IsExpanded(c)) next.push_back(c);
        }
        if (more) break;
      }
      level.swap(next);
    }
    if (top.IsOk()) set_first_visible(top);
    redraw_on();
    expand_budget_ = saved_budget;
    if (more) later([this, gen, step] { expand_more(gen, step + 1); });
  }

  // While filtering, the request Enter opens is shown in bold.
  void mark_best_match() {
    best_item_ = wxTreeItemId();  // the rebuild deleted the old one
    if (filter_q_.empty()) return;
    const fs::path best = best_match();
    if (best.empty()) return;
    if (wxTreeItemId id = item_for(best, false); id.IsOk()) {
      tree_->SetItemBold(id, true);
      best_item_ = id;
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
    // From one filter to another the rows are updated in place; starting or clearing a filter
    // rebuilds them (the folders a filter opened must not stay open, the user's must come back).
    // Clearing one updates them in place too (the folders it opened are closed in steps).
    sync_ok_ = !filter_q_.empty();
    filter_q_ = q;
    rebuild();
    history_->set_filter(q);
    update_counts();
    update_history_pane();
  }

  // The request that best matches the filter (see filter_rank); ties go to tree order.
  // Computed by rebuild() from the filter's hits.
  fs::path best_match() const { return filter_q_.empty() ? fs::path() : best_path_; }

  // Lower-case rank text and tree position per request of tree_root_ (lazy, once per scan or
  // order change): ranking 2000 hits then takes no allocation, sort or map lookup by path.
  void build_rank_keys() {
    rank_keys_.clear();
    size_t order = 0;
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
        RankKey k;
        k.name = ascii_lower(c->name);
        k.folder = ascii_lower(folder);
        k.rest = c->kind == RequestKind::Grpc ? std::string("grpc") : c->method;
        if (auto m = meta_.find(c->path); m != meta_.end()) k.rest += " " + url_search_text(m->second.url);
        k.rest += " " + u8(c->path.filename());
        k.rest = ascii_lower(k.rest);
        k.order = order++;
        rank_keys_.emplace(c, std::move(k));
      }
    };
    walk(tree_root_, "");
  }

  void rank_hits(const std::vector<const CollectionNode*>& hits) {
    best_path_.clear();
    if (filter_q_.empty() || hits.empty()) return;
    if (rank_keys_.empty()) build_rank_keys();
    int best_score = -1;
    size_t best_order = SIZE_MAX;
    for (const CollectionNode* c : hits) {
      auto it = rank_keys_.find(c);
      if (it == rank_keys_.end()) continue;
      const RankKey& k = it->second;
      const int s = filter_rank_lower(filter_q_, k.name, k.folder, k.rest);
      if (s > best_score || (s == best_score && s >= 0 && k.order < best_order)) {
        best_score = s;
        best_order = k.order;
        best_path_ = c->path;
      }
    }
    if (best_score < 0) best_path_.clear();
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

  // One TVGN_FIRSTVISIBLE jump (no smooth-scroll animation, unlike ScrollTo/EnsureVisible).
  void set_first_visible(const wxTreeItemId& id) {
    if (!id.IsOk()) return;
    SendMessage(static_cast<HWND>(tree_->GetHWND()), TVM_SELECTITEM, TVGN_FIRSTVISIBLE,
                reinterpret_cast<LPARAM>(static_cast<HTREEITEM>(id.GetID())));
  }
  // Whether the whole row is inside the tree's client area.
  bool row_in_view(const wxTreeItemId& id) const {
    RECT rc{};
    *reinterpret_cast<HTREEITEM*>(&rc) = static_cast<HTREEITEM>(id.GetID());
    if (!SendMessage(static_cast<HWND>(tree_->GetHWND()), TVM_GETITEMRECT, FALSE, reinterpret_cast<LPARAM>(&rc)))
      return false;
    return rc.top >= 0 && rc.bottom <= tree_->GetClientSize().y;
  }

  // Scrolls so the item sits in the middle of the tree, unless it is already well inside
  // (EnsureVisible alone leaves it on the bottom edge with its folder scrolled off).
  void centre_item(const wxTreeItemId& id) {
    if (!id.IsOk()) return;
    const HWND hw = static_cast<HWND>(tree_->GetHWND());
    const HTREEITEM hi = static_cast<HTREEITEM>(id.GetID());
    RECT rc{};
    *reinterpret_cast<HTREEITEM*>(&rc) = hi;
    const int h = tree_->GetClientSize().y;
    const bool got = SendMessage(hw, TVM_GETITEMRECT, FALSE, reinterpret_cast<LPARAM>(&rc)) != 0;
    if (got && rc.top >= h / 5 && rc.bottom <= h * 4 / 5) return;
    // The row lands in the middle: the row half a page above it becomes the first visible one
    // (the native view clamps at the end). One TVGN_FIRSTVISIBLE jump, not EnsureVisible plus
    // line scrolls: those animate (smooth scrolling) and cost ~100 ms on a just expanded folder.
    // The walk is over the logical rows (TVM_GETNEXTITEM), so it needs no painted layout.
    const int row = std::max(1, static_cast<int>(SendMessage(hw, TVM_GETITEMHEIGHT, 0, 0)));
    HTREEITEM top = hi;
    for (int i = 0; i < h / row / 2; ++i) {
      const auto prev = reinterpret_cast<HTREEITEM>(
          SendMessage(hw, TVM_GETNEXTITEM, TVGN_PREVIOUSVISIBLE, reinterpret_cast<LPARAM>(top)));
      if (!prev) break;
      top = prev;
    }
    SendMessage(hw, TVM_SELECTITEM, TVGN_FIRSTVISIBLE, reinterpret_cast<LPARAM>(top));
  }

  // Redraw off without wxTreeCtrl::Freeze(): that also toggles TVS_NOSCROLL (a style change
  // makes the native tree re-measure every row on Thaw, ~25 ms at 600 rows) and defers
  // EnsureVisible() to Thaw. Nested calls count.
  void redraw_off() {
    if (redraw_off_++ == 0) {
      redraw_sent_ = tree_->IsShown();  // TRUE on a hidden window would show it
      if (redraw_sent_) SendMessage(static_cast<HWND>(tree_->GetHWND()), WM_SETREDRAW, FALSE, 0);
    }
  }
  void redraw_on() {
    if (redraw_off_ <= 0 || --redraw_off_ > 0 || !redraw_sent_) return;
    const HWND hw = static_cast<HWND>(tree_->GetHWND());
    SendMessage(hw, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hw, nullptr, TRUE);
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
        auto idx = std::make_shared<NameIndex>(build_name_index(node, meta));
        auto* holder = new std::pair<CollectionNode, MetaMap>(std::move(node), std::move(meta));
        if (wxAppConsole* app = wxTheApp) app->CallAfter([this, gen, alive, holder, final, idx] {
          std::unique_ptr<std::pair<CollectionNode, MetaMap>> h(holder);
          if (alive.expired() || gen != scan_gen_) return;
          apply_scan(std::move(h->first), std::move(h->second), final, idx.get());
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
  void apply_scan(CollectionNode root, MetaMap meta, bool final, NameIndex* idx = nullptr) {
    if (tree_->GetEditControl()) {
      deferred_scan_ = std::make_unique<std::pair<CollectionNode, MetaMap>>(std::move(root), std::move(meta));
      deferred_final_ = deferred_final_ || final;
      return;
    }
    // The old tree and names are freed on a worker (10k nodes: ~5 ms). Nothing points into
    // them: the rows point into view_root_, the keys that point into tree_root_ are cleared
    // below.
    auto old = std::make_shared<std::pair<CollectionNode, MetaMap>>(std::move(tree_root_), std::move(meta_));
    tree_root_ = std::move(root);
    meta_ = std::move(meta);
    if (final) scanning_ = false;
    meta_changed(idx);
    std::thread([o = std::move(old)] {}).detach();
    sync_ok_ = true;
    rebuild();
    // The filter's search and rank keys, built ahead in their own UI steps: the first
    // keystroke then only filters (each is ~5 ms per 2000 requests).
    if (final) {
      later([this] {
        if (search_keys_.empty()) build_keys(tree_root_, "", meta_, search_keys_);
      });
      later([this] {
        if (rank_keys_.empty()) build_rank_keys();
      });
      warm_grpc();
    }
  }

  // The gRPC engine (its delay-loaded DLLs, runtime and protobuf machinery: ~90 ms) starts on a
  // worker as soon as the collection holds a gRPC request, not on the UI thread when its row is
  // first clicked. Once per process.
  void warm_grpc() {
    static std::atomic<bool> started{false};
    if (started.load()) return;
    std::function<bool(const CollectionNode&)> has = [&](const CollectionNode& n) {
      for (const auto& c : n.children)
        if (c.is_folder ? has(c) : c.kind == RequestKind::Grpc) return true;
      return false;
    };
    if (!has(tree_root_) || started.exchange(true)) return;
    std::thread([] { (void)MainFrame::grpc(); }).detach();
  }

  void load_history() {
    if (!ws_) return;
    auto copy = std::make_shared<Workspace>(*ws_);
    const uint64_t gen = ++hist_gen_;
    std::weak_ptr<int> alive = alive_;
    std::thread([this, copy, gen, alive] {
      // Entries the user deleted or cleared are filtered out here (the segments are append-only).
      const HistoryHidden hidden = load_history_hidden(copy->root());
      auto* list = new std::vector<HistoryEntry>(copy->history(std::min<size_t>(10000, 500 + hidden.keys.size())));
      filter_hidden_history(hidden, *list);
      if (list->size() > 500) list->resize(500);
      // The list lines hold only "GET <url>": the name of the request each entry was sent as
      // comes from its stored request, so two saved requests with the same URL keep their own
      // names after a restart. Read here, on the worker.
      // Its id too: an entry sent with unsaved edits (another URL) still maps to its file.
      auto* names = new std::map<std::string, HistMeta>;
      for (const auto& e : *list)
        if (auto r = copy->history_request(e); r && (!r->name.empty() || !r->id.empty() || !r->file.empty()))
          (*names)[e.when_iso + "\x01" + e.summary] = HistMeta{r->name, r->id, r->file};
      // Where snapshot tabs restored from the last session came from (their folder settings).
      auto* origins = new std::map<std::string, std::string>(load_snapshot_origins(copy->root()));
      if (wxAppConsole* app = wxTheApp) app->CallAfter([this, gen, alive, list, names, origins] {
        std::unique_ptr<std::vector<HistoryEntry>> h(list);
        std::unique_ptr<std::map<std::string, HistMeta>> nm(names);
        std::unique_ptr<std::map<std::string, std::string>> og(origins);
        if (alive.expired() || gen != hist_gen_) return;
        hist_names_ = std::move(*nm);
        for (const auto& [snap, rel] : *og)
          snapshot_origin_.emplace(snapshot_dir() / from_u8(snap), ws_->root() / from_u8(rel));
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
    auto r = sidebar::create_request(*ws_, folder, kind,
                                     kind == RequestKind::Grpc        ? "New gRPC request"
                                     : kind == RequestKind::WebSocket ? "New WebSocket"
                                                                      : "New request",
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
    for (const auto& f : files) {
      const fs::path g = f == from ? *to : *to / f.lexically_relative(from);
      note_own_write(g);  // the watcher reports it: not "changed outside watt"
      host_->file_moved(f, g);
    }
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
    // Its row (a reveal of a collapsed folder, the selection, the scroll): the next UI step.
    later([this, p] {
      select_file(p);
    });
  }

  // Opens a request in a permanent tab. It goes through the preview first: an unmodified preview
  // tab is reused (a few ms) instead of building a new tab (~400 ms), then made permanent.
  // `then` runs once the tab is permanent and its row revealed, in a step of its own.
  void open_tab(const fs::path& p, std::function<void()> then = {}) {
    if (p.empty()) return;
    st_timed("open preview", [&] { host_->open_request(p, true); });
    // Permanent, and its row revealed, in the next step (each under the 50 ms budget).
    later([this, p, then = std::move(then)]() mutable {
      st_timed("make permanent", [&] { host_->open_request(p, false); });
      // The row in a step of its own: the tab swap and the tree's reveal / repaint each stay
      // under the budget (together they were 50-65 ms on name + Enter).
      later([this, p, then = std::move(then)]() mutable {
        st_timed("select_file", [&] { select_file(p); });
        if (then) later(std::move(then));
      });
    });
  }

  // Self test only: how long fn took on the UI thread, in the log.
  template <class F>
  void st_timed(const char* what, F&& fn) {
    if (!st_probe_) return fn();
    const auto t0 = std::chrono::steady_clock::now();
    fn();
    st_log_ += wxString::Format("#   %s %.1f ms\n", what,
                                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count())
                   .utf8_string();
  }

  // A new or duplicated request: open it and put the caret in its URL / address field, as Bruno
  // and Postman do, so typing goes there (not to the tree's type-ahead). After the tab is final:
  // focused before, the caret was lost when the preview tab became permanent and the keyboard
  // landed on the tab strip (a URL typed next went nowhere).
  void open_tab_for_edit(const fs::path& p) {
    open_tab(p, [this, p] {
      focus_tab_url(p);
      // The host may still move the focus once (tab activation is posted): check again.
      CallAfter([this, p] {
        if (!focus_in_tab(p)) focus_tab_url(p);
      });
    });
  }

  // Opening a tab from the keyboard (Enter) can leave the focus on the notebook's tab strip,
  // where the next arrow key switches tabs and typed text is lost. Keep it in the tree (the
  // user is browsing with the keyboard) unless it went into the request's tab.
  void keep_keyboard(const fs::path& p) {
    auto fix = [this, p] {
      wxWindow* f = wxWindow::FindFocus();
      if (f == tree_ || focus_in_tab(p)) return;
      for (wxWindow* w = f; w; w = w->GetParent())
        if (w == this) return;  // the filter, a rename editor
      if (tree_->IsShownOnScreen()) tree_->SetFocus();
    };
    // The host activates the tab in posted steps: check after them.
    CallAfter([this, fix] { later(fix); });
  }

  // Whether the keyboard focus is inside the tab of p.
  bool focus_in_tab(const fs::path& p) {
    RequestTab* t = tab_for(p);
    if (!t) return false;
    for (wxWindow* w = wxWindow::FindFocus(); w; w = w->GetParent())
      if (w == t) return true;
    return false;
  }

  void focus_tab_url(const fs::path& p) {
    if (RequestTab* t = tab_for(p)) {
      if (t->IsShownOnScreen()) {
        t->focus_url();
        return;
      }
    }
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
    for (const auto& f : files) {
      const fs::path g = f == from ? to : to / f.lexically_relative(from);
      note_own_write(g);
      host_->file_moved(f, g);
    }
    remap_expanded(from, to);
    remap_order(from, to);
    if (dest != ws_->root()) expanded_.insert(dest);
    pending_select_ = to;
    sticky_sel_ = to;
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
    // A tab with unsaved edits is not closed with its file: say what happens to it.
    std::vector<std::string> dirty;
    for (const auto& f : files)
      if (RequestTab* t = tab_for(f); t && t->dirty()) dirty.push_back(meta_name(f).empty() ? u8(f.filename()) : meta_name(f));
    if (!dirty.empty()) {
      std::string names;
      for (size_t i = 0; i < dirty.size() && i < 3; ++i) names += (i ? ", \"" : "\"") + dirty[i] + "\"";
      if (dirty.size() > 3) names += " and " + std::to_string(dirty.size() - 3) + " more";
      q += wxString::FromUTF8("\n\n" + names + (dirty.size() == 1 ? " is" : " are") +
                              " open with unsaved changes: the tab stays open as an unsaved request "
                              "(Ctrl+S saves it again, Ctrl+W closes it).");
    }
    if (!confirm(wxGetTopLevelParent(this), title, q, "Move to Recycle Bin", true)) return;
    delete_confirmed(p, files);
  }

  // The row to select once p is gone: the next sibling, else the previous one, else the folder.
  fs::path neighbour_of(const fs::path& p) {
    const wxTreeItemId it = item_for(p, false);
    if (!it.IsOk()) return {};
    wxTreeItemId n = tree_->GetNextSibling(it);
    if (!n.IsOk()) n = tree_->GetPrevSibling(it);
    if (!n.IsOk()) n = tree_->GetItemParent(it);
    const auto* d = n.IsOk() ? data(n) : nullptr;
    return d ? d->node->path : fs::path();
  }

  void delete_confirmed(const fs::path& p, const std::vector<fs::path>& files) {
    // The Recycle Bin move runs on a worker: it takes ~0.2 s locally and seconds on a network
    // drive. The row goes at once; a failure puts it back with the rescan.
    // Afterwards: the tab that was active before the deleted one (if the deleted one was
    // active), else the deleted row's neighbour; never whatever tab the notebook falls back to.
    const bool active_deleted =
        !active_file_.empty() && (active_file_ == p || is_under(active_file_, p));
    fs::path back_to;
    if (active_deleted)
      for (const auto& f : recent_files_)
        if (f != p && !is_under(f, p) && tab_for(f)) {
          back_to = f;
          break;
        }
    const fs::path neighbour = neighbour_of(p);
    if (wxTreeItemId it = item_for(p, false); it.IsOk()) {
      syncing_ = true;
      if (wxTreeItemId n = neighbour.empty() ? wxTreeItemId() : item_for(neighbour, false); n.IsOk()) tree_->SelectItem(n);
      tree_->Delete(it);
      syncing_ = false;
    }
    std::weak_ptr<int> alive = alive_;
    std::thread([this, p, files, alive, back_to, neighbour, active_deleted] {
      const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
      const bool ok = recycle(p);
      if (SUCCEEDED(co)) CoUninitialize();
      if (wxAppConsole* app = wxTheApp) app->CallAfter([this, p, files, alive, ok, back_to, neighbour, active_deleted] {
        if (alive.expired() || !ws_) return;
        if (!ok && !ws_->remove(p)) {
          rescan();
          notify(wxGetTopLevelParent(this), "Could not delete", "It is in use, or outside the workspace.");
          return;
        }
        expanded_.erase(p);
        // The user keeps their place: the deleted request's neighbour row (a request) is shown,
        // in the preview tab (the deleted request's own tab when it was the preview: no tab is
        // closed or built then); else the tab that was active before it. That happens before
        // its tab closes, so the notebook never falls back to whatever tab it picks (the first
        // one). Showing the neighbour, closing the tab and the rescan are separate UI steps.
        // The neighbour only where showing it is cheap (the deleted request's tab is the preview
        // and is reused, a few ms) or there is no earlier tab; a permanent tab gives way to the
        // tab that was active before it (activating it builds nothing; a new tab is ~70 ms).
        fs::path show;
        if (active_deleted) {
          const bool neighbour_req = !neighbour.empty() && is_request_file(neighbour);
          const bool have_back = !back_to.empty() && tab_for(back_to);
          RequestTab* gone = tab_for(active_file_);
          const bool reusable = gone && gone->preview() && !gone->dirty();
          if (neighbour_req && (reusable || !have_back)) show = neighbour;
          else if (have_back) show = back_to;
        }
        if (!show.empty()) st_timed("show neighbour", [&] { host_->open_request(show, true); });
        later([this, files, show, neighbour] {
          st_timed("close tab", [&] {
            for (const auto& f : files) {
              detach_closed_tab(f);
              host_->file_moved(f, {});
            }
          });
          // The row of the tab the editor really shows now (the notebook may have kept or
          // picked another one than `show`): Enter / Ctrl+Enter must act on what is visible.
          fs::path shown = show;
          if (wxAuiNotebook* nb = notebook(); nb && nb->GetSelection() != wxNOT_FOUND)
            if (auto* t = dynamic_cast<RequestTab*>(nb->GetPage(static_cast<size_t>(nb->GetSelection()))))
              shown = t->file();
          if (!shown.empty() || !show.empty()) {
            select_file(shown);
          } else if (!neighbour.empty()) {
            pending_select_ = neighbour;
            select_after_scan_ = true;
          }
          // The keyboard stays in the tree (Del again, arrows), not on the tab strip the closed
          // tab left it on.
          ++tree_focus_req_;
          if (tree_->IsShownOnScreen()) tree_->SetFocus();
          later([this] { rescan(); });
        });
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
    place_after(r->file, p);
    name_then_open(r->file);
  }

  // The copy goes directly under its original (as in Bruno and Postman), not at the end of a
  // folder with its own order or after whatever sorts between them by name.
  void place_after(const fs::path& copy, const fs::path& original) {
    const fs::path folder = original.parent_path();
    if (copy.parent_path() != folder) return;
    std::vector<std::wstring> names;
    if (auto it = order_.find(folder); it != order_.end()) {
      names = it->second;
    } else if (const CollectionNode* parent = find_folder(tree_root_, folder)) {
      std::vector<const CollectionNode*> kids;
      for (const auto& c : parent->children) kids.push_back(&c);
      sort_kids(folder, kids);
      for (const CollectionNode* k : kids) names.push_back(k->path.filename().wstring());
    }
    const std::wstring o = original.filename().wstring(), c = copy.filename().wstring();
    std::erase(names, c);
    auto at = std::find(names.begin(), names.end(), o);
    if (at == names.end()) return;
    names.insert(at + 1, c);
    order_[folder] = std::move(names);
    save_order();
  }

  // A new item (duplicate, new request) is selected with the rename editor open; its tab opens
  // when the edit ends, under the name the user gave it.
  void name_then_open(const fs::path& file) {
    if (hist_full_) toggle_history_full();  // the new item is named in the tree
    pending_edit_ = file;
    open_after_edit_ = file;
    rescan();
  }

  // "Payments" -> "Payments copy" next to it, every request in it with a new id (so history
  // entries of the originals keep mapping to the originals). Copied on a worker; the copy is
  // then named in the tree.
  // into: where the copy goes (Ctrl+V); empty: next to the original, as "<name> copy".
  void duplicate_folder(const fs::path& p, const fs::path& into = {}) {
    if (!ws_ || p.empty() || p == ws_->root() || !is_under(p, ws_->root())) return;
    if (!into.empty() && (!(into == ws_->root() || is_under(into, ws_->root())) || into == p || is_under(into, p))) {
      notify(wxGetTopLevelParent(this), "Could not paste", "A folder cannot be pasted into itself.");
      return;
    }
    std::error_code ec;
    const fs::path dir = into.empty() ? p.parent_path() : into;
    fs::path to = dir / (into.empty() ? p.filename().wstring() + L" copy" : p.filename().wstring());
    for (int n = 2; fs::exists(to, ec); ++n) to = dir / (p.filename().wstring() + L" copy " + std::to_wstring(n));
    auto copy = std::make_shared<Workspace>(*ws_);
    std::weak_ptr<int> alive = alive_;
    std::thread([this, copy, p, to, alive] {
      std::error_code e2;
      fs::copy(p, to, fs::copy_options::recursive, e2);
      std::string err = e2 ? e2.message() : std::string();
      if (!e2)
        for (fs::recursive_directory_iterator it(to, e2), end; !e2 && it != end; it.increment(e2)) {
          if (!is_request_file(it->path())) continue;
          if (auto r = copy->load_request(it->path())) {
            r->id.clear();
            copy->save_request(*r);
          }
        }
      if (wxAppConsole* app = wxTheApp) app->CallAfter([this, alive, p, to, err] {
        if (alive.expired() || !ws_) return;
        if (!err.empty()) {
          notify(wxGetTopLevelParent(this), "Could not duplicate the folder", wxString::FromUTF8(err));
          rescan();
          return;
        }
        if (auto it = order_.find(p); it != order_.end()) {
          order_[to] = it->second;
          save_order();
        }
        if (expanded_.count(p)) expanded_.insert(to);
        if (hist_full_) toggle_history_full();
        pending_edit_ = to;
        rescan();
      });
    }).detach();
  }

  // Folder settings: rename, the order of its items, what is in it, Run.
  void folder_settings_dialog(const fs::path& p) {
    if (!ws_) return;
    const CollectionNode* n = find_node(tree_root_, p);
    if (!n || !n->is_folder) return;
    size_t http = 0, grpc = 0, folders = 0;
    std::function<void(const CollectionNode&)> count = [&](const CollectionNode& c) {
      for (const auto& k : c.children) {
        if (k.is_folder) {
          ++folders;
          count(k);
        } else if (k.kind == RequestKind::Grpc) {
          ++grpc;
        } else {
          ++http;
        }
      }
    };
    count(*n);
    sidebar::FolderSettings fs_;
    fs_.name = n->name;
    fs_.has_dragged_order = order_.count(p) != 0;
    fs_.dragged_order = fs_.has_dragged_order;
    fs_.path = p.wstring();
    fs_.can_run = host_runs_folders(host_) && http + grpc > 0;
    fs_.contents = wxString::Format("%zu request%s (%zu HTTP, %zu gRPC)", http + grpc, http + grpc == 1 ? "" : "s",
                                    http, grpc);
    if (folders) fs_.contents += wxString::Format(" in %zu subfolder%s and this folder", folders, folders == 1 ? "" : "s");
    const int rc = sidebar::folder_settings(wxGetTopLevelParent(this), fs_);
    if (rc == wxID_CANCEL) return;
    if (fs_.has_dragged_order && !fs_.dragged_order) {
      order_.erase(p);
      save_order();
      rebuild();
    }
    fs::path now = p;
    if (!fs_.name.empty() && fs_.name != n->name) {
      const fs::path to = rename_node(p, fs_.name);
      if (!to.empty()) now = to;
    }
    if (rc == sidebar::kFolderRun) host_run_folder(host_, now);
  }

  void reveal_in_explorer(const fs::path& p) {
    wxExecute(wxString::Format("explorer.exe /select,\"%s\"", p.wstring()), wxEXEC_ASYNC);
  }

  // A terminal in `dir` (Bruno's "Open in Terminal"): Windows Terminal when it is installed,
  // else a command prompt. Only folders of this workspace.
  void open_terminal(const fs::path& dir) {
    if (!ws_ || !(dir == ws_->root() || is_under(dir, ws_->root()))) return;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    wxLogNull quiet;  // a missing wt.exe is not an error dialog
    wxExecuteEnv env;
    env.cwd = dir.wstring();
    if (wxExecute("wt.exe -d .", wxEXEC_ASYNC, nullptr, &env) > 0) return;  // "." is env.cwd
    wxExecute("cmd.exe", wxEXEC_ASYNC | wxEXEC_SHOW_CONSOLE, nullptr, &env);
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
    m.Append(1, "New &HTTP request");
    m.Append(2, "New &gRPC request");
    m.Append(4, "New &WebSocket");
    m.AppendSeparator();
    m.Append(3, "New &folder");
    m.AppendSeparator();
    auto* imp = new wxMenu;
    append_import_items(*imp, 10);
    m.AppendSubMenu(imp, "&Import");
    const int id = anchor->GetPopupMenuSelectionFromUser(m, wxPoint(0, anchor->GetSize().y));
    if (id == 1) new_request_in(RequestKind::Http, folder);
    else if (id == 2) new_request_in(RequestKind::Grpc, folder);
    else if (id == 4) new_request_in(RequestKind::WebSocket, folder);
    else if (id == 3) new_folder_in(folder);
    else if (id >= 10 && id < 10 + kImportKinds) import_kind(id - 10, folder);
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
    enum {
      kOpen = 1, kHttp, kGrpc, kFolder, kRename, kDup, kDelete, kCopyPath, kReveal, kCollapse, kCurl, kCurlAll, kExport, kSortAz,
      kRun, kSettings, kOrder, kCopy, kPaste, kInfo, kTerminal, kWs, kCodeBase = 900
    };
    const bool can_run = host_runs_folders(host_);
    wxMenu m;
    if (!folder) {
      m.Append(kOpen, "&Open in tab\tEnter");
      m.AppendSeparator();
    } else {
      // Run every request in the folder (the runner); disabled until the host can run folders.
      wxMenuItem* run = m.Append(kRun, d ? "R&un folder" : "R&un collection");
      run->Enable(can_run && !requests_under(path).empty());
      if (!can_run) run->SetHelp("Runs need the collection runner");
      m.AppendSeparator();
    }
    m.Append(kHttp, "New &HTTP request");
    m.Append(kGrpc, "New &gRPC request");
    m.Append(kWs, "New &WebSocket");
    m.Append(kFolder, "New &folder");
    {
      auto* imp = new wxMenu;
      append_import_items(*imp, kImportBase);
      m.AppendSubMenu(imp, "&Import here");
    }
    if (d) {
      m.AppendSeparator();
      m.Append(kRename, "&Rename\tF2");
      m.Append(kDup, "&Duplicate\tCtrl+D");
      m.Append(kCopy, "Cop&y\tCtrl+C");
    }
    if (folder) {
      wxMenuItem* paste = m.Append(kPaste, clip_.empty() ? wxString("&Paste\tCtrl+V")
                                                         : wxString::Format("&Paste \"%s\"\tCtrl+V", clip_name()));
      paste->Enable(clip_ok());
    }
    if (d) m.Append(kDelete, "D&elete\tDel");
    // The settings requests below inherit (auth, headers, variables, assertions, scripts): the
    // same editor as the request's Auth tab "Edit on <folder>".
    if (folder)
      m.Append(kSettings, wxString::FromUTF8(d ? "Folder &settings (auth, headers, vars, asserts)\xE2\x80\xA6"
                                               : "Collection &settings (auth, headers, vars, asserts)\xE2\x80\xA6"));
    if (d && folder) m.Append(kOrder, wxString::FromUTF8("Rena&me, order and contents\xE2\x80\xA6"));
    m.AppendSeparator();
    if (!folder) m.Append(kCurl, d && d->node->kind == RequestKind::Grpc ? "&Copy as grpcurl" : "&Copy as cURL");
    if (!folder && d && d->node->kind == RequestKind::Http) {
      auto* code = new wxMenu;
      for (int i = 0; i < http_editor::kCodeLangCount; ++i)
        code->Append(kCodeBase + i, wxString::FromUTF8(http_editor::code_lang_name(static_cast<http_editor::CodeLang>(i))));
      m.AppendSubMenu(code, "&Generate code (copy)");
    }
    if (folder) {
      m.Append(kCurlAll, d ? "&Copy folder as cURL" : "&Copy collection as cURL");
      m.Append(kExport, wxString::FromUTF8("E&xport as Postman collection\xE2\x80\xA6"));
    }
    if (d) m.Append(kInfo, wxString::FromUTF8("I&nfo\xE2\x80\xA6"));
    m.Append(kCopyPath, "Copy &path");
    m.Append(kReveal, "Reveal in File Exp&lorer");
    m.Append(kTerminal, folder ? "Open in &Terminal" : "Open folder in &Terminal");
    if (folder) m.Append(kCollapse, "Coll&apse all");
    if (folder && order_.count(path)) m.Append(kSortAz, "Sort &by name (forget the dragged order)");
    const int choice = tree_->GetPopupMenuSelectionFromUser(m);
    if (choice >= kImportBase && choice < kImportBase + kImportKinds) {
      import_kind(choice - kImportBase, in);
      return;
    }
    if (choice >= kCodeBase && choice < kCodeBase + http_editor::kCodeLangCount) {
      copy_code(path, static_cast<http_editor::CodeLang>(choice - kCodeBase));
      return;
    }
    // Nothing that changes the tree was chosen: give the highlight back to the active request.
    if (choice != kRename && choice != kDelete && choice != kDup && choice != kOpen && choice != kCollapse &&
        choice != kSettings && choice != kOrder &&
        !active_file_.empty() &&
        active_file_ != path)
      reveal_active();
    switch (choice) {
      case kOpen: open_tab(path); break;
      case kHttp: new_request_in(RequestKind::Http, in); break;
      case kGrpc: new_request_in(RequestKind::Grpc, in); break;
      case kWs: new_request_in(RequestKind::WebSocket, in); break;
      case kFolder: new_folder_in(in); break;
      case kRename: if (id.IsOk()) edit_label(id); break;
      case kDup:
        if (folder) duplicate_folder(path);
        else duplicate_item(path);
        break;
      case kRun: host_run_folder(host_, d ? path : fs::path()); break;  // empty: the whole workspace
      case kSettings: open_folder_settings(path); break;
      case kOrder: folder_settings_dialog(path); break;
      case kDelete: delete_item(id); break;
      case kCopyPath: copy_text(path.wstring()); break;
      case kCopy: copy_item(path); break;
      case kPaste: paste_into(path); break;
      case kInfo: show_info(path); break;
      case kCurl: copy_curl(path); break;
      case kCurlAll: export_folder(path, true); break;
      case kExport: export_folder(path, false); break;
      case kReveal: reveal_in_explorer(path); break;
      case kTerminal: open_terminal(in); break;
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

  // The Recent list as this window shows it. The config list is shared by every watt process
  // and reorders whenever any of them opens a workspace, so the menu keeps this process's own
  // order (read once, then only moved by this window's switches); workspaces another instance
  // added since are appended at the end, never inserted above the entry being aimed at.
  static std::vector<fs::path>& own_recent() {
    static std::vector<fs::path> v = recent_workspaces();
    return v;
  }
  static std::vector<fs::path> menu_recent() {
    std::vector<fs::path> out = own_recent();
    for (const auto& p : recent_workspaces())
      if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    // A workspace deleted since (another instance's scratch folders) is not offered. A few stats
    // of local folders; a UNC path is kept as it is (never touched from the UI thread).
    std::erase_if(out, [](const fs::path& p) {
      const std::wstring w = p.wstring();
      auto sep = [](wchar_t c) { return c == L'\\' || c == L'/'; };
      if (w.size() >= 2 && sep(w[0]) && sep(w[1])) return false;
      std::error_code ec;
      return !fs::exists(p / "watt.json", ec);
    });
    if (out.size() > kMaxRecent) out.resize(kMaxRecent);
    return out;
  }

  static void remember_workspace(const fs::path& root) {
    {
      auto& own = own_recent();
      std::erase_if(own, [&](const fs::path& p) { return p == root; });
      own.insert(own.begin(), root);
      if (own.size() > kMaxRecent) own.resize(kMaxRecent);
    }
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

  // Open (or make) a workspace folder: the workspace menu and the empty collection's button.
  void open_workspace_dialog(bool create) {
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
  }

  // A folder's (the workspace root: the collection's) settings dialog; a save refreshes open tabs.
  void open_folder_settings(const fs::path& path) {
    if (!ws_) return;
    http_editor::edit_folder_settings(wxGetTopLevelParent(this), *ws_, path, [this, path, alive = std::weak_ptr<int>(alive_)] {
      if (alive.expired()) return;
      refresh_inherit_tabs(path);
      inh_saved_ = path;
      inh_saved_at_ = std::chrono::steady_clock::now();
      if (host_) host_->folder_settings_changed();
    });
  }

  void workspace_menu() {
    enum { kOpen = 1, kNew, kReveal, kReload, kCookies, kRecent = 100 };
    wxMenu m;
    if (ws_) {
      m.Append(wxID_ANY, short_path(ws_->root().wstring(), 56))->Enable(false);
      m.AppendSeparator();
    }
    m.Append(kOpen, "&Open workspace folder...");
    m.Append(kNew, "&New workspace...");
    const auto recent = menu_recent();
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
      m.Append(kCookies, wxString::FromUTF8("&Cookies\xE2\x80\xA6"));
      m.Append(kReveal, "Reveal workspace in File Exp&lorer");
      m.Append(kReload, "&Reload from disk");
    }
    const int id = ws_btn_->GetPopupMenuSelectionFromUser(m, wxPoint(0, ws_btn_->GetSize().y));
    if (id == kOpen || id == kNew) {
      open_workspace_dialog(id == kNew);
    } else if (id >= kRecent && id < kRecent + static_cast<int>(others.size())) {
      switch_workspace(others[static_cast<size_t>(id - kRecent)], true);
    } else if (id == kReveal && ws_) {
      wxExecute(wxString::Format("explorer.exe \"%s\"", ws_->root().wstring()), wxEXEC_ASYNC);
    } else if (id == kReload && ws_) {
      switch_workspace(ws_->root(), true);
    } else if (id == kCookies) {
      open_cookies();
    }
  }

  // The cookie manager of this workspace's jar (loaded on a worker first).
  void open_cookies(const std::string& domain = {}) {
    if (!ws_) return;
    manage_cookies(wxGetTopLevelParent(this), ws_->cookie_jar(), domain);
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
      // A posted step of its own (later()), not in the idle pass that runs every pending
      // CallAfter: the tab swap must not add up with other pieces' queued work in one stall.
      auto w = std::shared_ptr<Workspace>(holder);
      if (wxAppConsole* app = wxTheApp) app->CallAfter([this, alive, gen, w, err, dir, user] {
        if (!alive.expired()) later([this, alive, gen, w, err, dir, user] { workspace_opened(alive, gen, w, err, dir, user); });
      });
    }).detach();
  }

  void workspace_opened(const std::weak_ptr<int>& alive, uint64_t gen, const std::shared_ptr<Workspace>& w,
                        const std::string& err, const fs::path& dir, bool user) {
    {
      {
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
          st_timed("  swap_tabs", [&] { swap_tabs(old_root, ws_->root()); });
          if (!filter_->IsEmpty()) {  // a filter typed for the old collection
            filter_->ChangeValue("");
            filter_q_.clear();
            history_->set_filter({});
          }
        }
        // Tabs, tree and the tabs' {{var}} previews each cost 20-40 ms on a big workspace: one
        // step each, so the window repaints between them and no single UI step passes 50 ms.
        // (later(): a posted window message, so input and other messages get in between; nested
        // CallAfter calls would all run in the same pending-event pass.)
        later([this, alive, gen] {
          if (alive.expired() || gen != ws_gen_ || !ws_) return;
          st_timed("  workspace_ready", [&] { workspace_ready(); });
          later([this, alive, gen] {
            if (alive.expired() || gen != ws_gen_ || !ws_) return;
            st_timed("  env_changed", [&] { host_->environment_changed(); });
          });
        });
      }
    }
  }

  // ---------------------------------------------------------------- tab sets per workspace

  // A file whose tab was lent to the other workspace: where it stood and the response it showed,
  // so it comes back as it was left (not with the other workspace's response, or an older one).
  struct LostTab {
    fs::path file;
    LastResponse last;
    int index = -1;       // its tab position (-1: append)
    bool selected = false;
  };
  struct TabSet {
    std::vector<std::pair<wxWindow*, wxString>> pages;  // in tab order, with their titles
    wxWindow* selected = nullptr;
    std::vector<LostTab> lost;  // files whose tab was reused for the other workspace
  };

  // A tab may be lent only when it holds nothing a reload would lose: realized (an unrealized
  // tab keeps its pending restored response through set_request), clean and idle.
  static bool lendable(RequestTab* t) { return t->realized() && !t->dirty() && !t->in_flight(); }

  // Remembers t's file (with its response and position) before t is reused for another file.
  static void remember_lent(TabSet& set, RequestTab* t, int index, bool selected) {
    if (t->file().empty()) return;
    set.lost.push_back({t->file(), t->last_response(), index, selected});
  }

  // Hides the tabs of old_root (every tab that is not an empty start tab) and shows the ones
  // new_root had when it was left. At least one tab stays: a clean old tab is reused (blanked)
  // when the new workspace has none, its file reopened when the old one comes back.
  void swap_tabs(const fs::path& old_root, const fs::path& new_root) {
    wxAuiNotebook* nb = notebook();
    if (!nb) return;
    // Frozen once for all the pages added and removed (each RemovePage() freezes the notebook
    // too, and lays its pages out): with the hidden tabs dormant (dormant_tick()) this walks
    // the shown tab only.
    nb->Freeze();
    TabSet old;
    std::map<wxWindow*, int> old_index;  // tab positions in the old workspace
    const int sel = nb->GetSelection();
    for (size_t i = 0; i < nb->GetPageCount(); ++i) {
      auto* t = dynamic_cast<RequestTab*>(nb->GetPage(i));
      if (!t || t->pristine()) continue;
      old.pages.emplace_back(t, nb->GetPageText(i));
      old_index[t] = static_cast<int>(i);
      if (static_cast<int>(i) == sel) old.selected = t;
    }
    TabSet next;
    if (auto it = tab_sets_.find(new_root); it != tab_sets_.end()) {
      next = std::move(it->second);
      tab_sets_.erase(it);
    }
    st_timed("   add back", [&] {
      for (auto& [w, title] : next.pages) {
        w->Reparent(nb);  // from the park (see tab_park())
        nb->AddPage(w, title, false);
      }
    });
    // Select the tab coming back before the old ones go: removing the selected page makes the
    // notebook show (and lay out) a neighbour first, ~25 ms for nothing.
    st_timed("   select", [&] {
      if (wxWindow* first = next.selected ? next.selected : (next.pages.empty() ? nullptr : next.pages.front().first))
        if (const int i = nb->GetPageIndex(first); i >= 0) nb->SetSelection(static_cast<size_t>(i));
    });
    const auto st_lend0 = std::chrono::steady_clock::now();
    // Files of the new workspace whose tab was lent to the old one last time: lend a clean
    // tab back (a few ms) instead of building a new one (~200 ms each). Each comes back with the
    // response it showed and at the position it had.
    std::vector<fs::path> reopen;
    std::vector<std::pair<RequestTab*, int>> placed;  // lent tabs and the position they go to
    RequestTab* lent_selected = nullptr;
    std::sort(next.lost.begin(), next.lost.end(), [](const LostTab& a, const LostTab& b) {
      return static_cast<unsigned>(a.index) < static_cast<unsigned>(b.index);  // -1 (append) last
    });
    for (auto& lost : next.lost) {
      std::error_code ec;
      if (!fs::exists(lost.file, ec)) continue;
      RequestTab* loan = nullptr;
      for (int pass = 0; pass < 2 && !loan; ++pass)
        for (auto& pg : old.pages) {
          auto* t = static_cast<RequestTab*>(pg.first);
          if (lendable(t) && (pass == 1 || pg.first != old.selected)) {
            loan = t;
            break;
          }
        }
      std::optional<SavedRequest> r;
      if (loan) r = ws_->load_request(lost.file);
      if (!r) {
        reopen.push_back(lost.file);
        continue;
      }
      remember_lent(old, loan, old_index.count(loan) ? old_index[loan] : -1, loan == old.selected);
      std::erase_if(old.pages, [&](const auto& pg) { return pg.first == loan; });
      if (old.selected == loan) old.selected = nullptr;
      loan->set_request(std::move(*r), false);  // clears the old workspace's response
      if (lost.last)  // shown in a later turn: no single UI step pays the editor and the response view
        later([alive = std::weak_ptr<int>(alive_), w = wxWeakRef<RequestTab>(loan), file = lost.file,
               last = std::move(lost.last)]() mutable {
          if (!alive.expired() && w && w->file() == file && !w->in_flight() && !w->last_response())
            w->restore_response(std::move(last));
        });
      if (const int i = nb->GetPageIndex(loan); i >= 0) nb->SetPageText(static_cast<size_t>(i), loan->title());
      placed.emplace_back(loan, lost.index);
      if (lost.selected && !next.selected) lent_selected = loan;
    }
    if (st_probe_)
      st_log_ += wxString::Format("#      lend %.1f ms (%zu lost)\n",
                                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - st_lend0).count(),
                                  next.lost.size()).utf8_string();
    // Parked outside the notebook: a removed page left as a hidden child of the notebook is what
    // coincided with the access violation in wxAuiNotebook::RemoveEmptyTabFrames (the frame's
    // spare tabs are parked the same way).
    wxWindow* park = tab_park();
    // Each removed page makes wxAuiNotebook select the current page again, with a page change
    // event the frame answers (realize, select in the tree, save the session): 10-15 ms per
    // parked tab for nothing. Muted while the tabs go; one event at the end if the page changed.
    wxWindow* const shown_before = nb->GetCurrentPage();
    mute_page_events_ = true;
    st_timed("   park", [&] {
      for (auto& [w, title] : old.pages) {
        const int i = nb->GetPageIndex(w);
        if (i >= 0) nb->RemovePage(static_cast<size_t>(i));
        w->Hide();
        if (park) w->Reparent(park);
      }
    });
    // Lent tabs back where they stood (in increasing order, so earlier moves stay put).
    for (const auto& [t, want] : placed) {
      const int cur = nb->GetPageIndex(t);
      const int count = static_cast<int>(nb->GetPageCount());
      const int to = want < 0 ? count - 1 : std::min(want, count - 1);
      if (cur < 0 || cur == to) continue;
      const wxString text = nb->GetPageText(static_cast<size_t>(cur));
      nb->RemovePage(static_cast<size_t>(cur));
      nb->InsertPage(static_cast<size_t>(to), t, text, false);
    }
    mute_page_events_ = false;
    if (nb->GetCurrentPage() != shown_before && nb->GetSelection() >= 0) {
      wxAuiNotebookEvent ev(wxEVT_AUINOTEBOOK_PAGE_CHANGED, nb->GetId());
      ev.SetSelection(nb->GetSelection());
      ev.SetOldSelection(wxNOT_FOUND);
      ev.SetEventObject(nb);
      nb->GetEventHandler()->ProcessEvent(ev);
    }
    if (nb->GetPageCount() == 0) {
      // Nothing to show: reuse a clean tab of the old workspace as the start tab.
      RequestTab* carrier = nullptr;
      for (auto it = old.pages.rbegin(); it != old.pages.rend(); ++it) {
        auto* t = static_cast<RequestTab*>(it->first);
        if (lendable(t) && (it->first == old.selected || !carrier)) carrier = t;
      }
      if (carrier) {
        remember_lent(old, carrier, old_index.count(carrier) ? old_index[carrier] : -1, carrier == old.selected);
        std::erase_if(old.pages, [&](const auto& p) { return p.first == carrier; });
        if (old.selected == carrier) old.selected = nullptr;
        carrier->set_request(SavedRequest{}, false);  // a blank start tab: no response either
        carrier->Reparent(nb);
        nb->AddPage(carrier, carrier->title(), true);
      }
    }
    if (wxWindow* s = next.selected ? next.selected : lent_selected) {
      if (const int i = nb->GetPageIndex(s); i >= 0) nb->SetSelection(static_cast<size_t>(i));
    } else if (nb->GetPageCount() && nb->GetSelection() < 0) {
      nb->SetSelection(0);
    }
    st_timed("   plus", [&] { ensure_plus_button(nb); });
    st_timed("   thaw", [&] { nb->Thaw(); });
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

  // The frame's "+" (new tab) lives on the notebook's main tab control, which wxAuiNotebook
  // destroys and builds again when its last page goes: give the new one the button too. Same id as
  // main_frame.cpp's kPlusButton (its art draws it, its handler opens the tab).
  static void ensure_plus_button(wxAuiNotebook* nb) {
    constexpr int kPlusButton = 0x6E01;
    if (wxAuiTabCtrl* tc = nb->GetMainTabCtrl()) {
      tc->RemoveButton(kPlusButton);  // never twice
      tc->AddButton(kPlusButton, wxRIGHT);
      tc->Refresh();
    }
  }

  // Folder / collection settings were saved: the HTTP editors of open tabs under that folder
  // read what they inherit (folder variables: the URL they send) again, so a send never goes to
  // the old server. The editor reads it on load(); the tab's request, unsaved edits and response
  // stay as they are (load() gets the tab's current request back). Tabs not built yet read it
  // when they are; a tab with a send in flight is done once that send has finished.
  void refresh_inherit_tabs(const fs::path& folder) {
    wxAuiNotebook* nb = notebook();
    if (!nb || !ws_) return;
    const bool all = folder.empty() || folder == ws_->root();
    bool waiting = false;
    for (size_t i = 0; i < nb->GetPageCount(); ++i) {
      auto* t = dynamic_cast<RequestTab*>(nb->GetPage(i));
      if (!t || !t->realized() || t->request_kind() != RequestKind::Http) continue;
      if (!all) {
        const fs::path f = t->inherit_file();
        if (f.empty() || !is_under(f, folder)) continue;
      }
      if (t->in_flight()) {
        waiting = true;
        continue;
      }
      reload_editor(t);
    }
    // The editors read the folders on a worker: show the "Sent as" line with the new values once
    // that has landed (and once more for a slow disk).
    inh_ticks_ = 2;
    inh_timer_.StartOnce(150);
    if (waiting) {
      pending_inherit_ = folder;
      pending_inherit_set_ = true;
    }
  }

  // The HTTP editor shows t's current request again (its own state stored first, so unsaved
  // edits stay, and so does the dirty mark); the response is not touched. load() is what makes
  // the editor read its folders' settings again and colour the {{vars}} with them.
  static void reload_editor(RequestTab* t) {
    HttpEditorPanel* ed = find_http_editor(t);
    if (!ed) return;
    const bool was_dirty = t->dirty();
    SavedRequest r = t->request();  // stores the editor's state first
    ed->load(r);
    if (was_dirty && !t->dirty()) t->mark_dirty();
  }

  static bool is_under(const fs::path& file, const fs::path& folder) {
    const fs::path f = file.lexically_normal(), d = folder.lexically_normal();
    auto fi = f.begin();
    for (auto di = d.begin(); di != d.end(); ++di, ++fi) {
      if (di->empty()) continue;  // a trailing separator
      if (fi == f.end() || _wcsicmp(fi->c_str(), di->c_str()) != 0) return false;
    }
    return true;
  }

  static HttpEditorPanel* find_http_editor(wxWindow* w) {
    for (wxWindow* c : w->GetChildren()) {
      if (auto* ed = dynamic_cast<HttpEditorPanel*>(c)) return ed;
      if (dynamic_cast<RequestTab*>(c)) continue;
      if (HttpEditorPanel* ed = find_http_editor(c)) return ed;
    }
    return nullptr;
  }

  // Hidden holder for the tabs of other workspaces: a child of the frame, never of the notebook.
  // The clean tab of a deleted request leaves the notebook now and is destroyed in a later UI
  // step: removing the page and destroying its ~170 windows were 35-45 ms in one step. (The
  // frame's file_moved() then finds no tab; a dirty tab stays, as the frame keeps it unsaved.)
  void detach_closed_tab(const fs::path& f) {
    wxAuiNotebook* nb = notebook();
    if (!nb || nb->GetPageCount() < 2) return;  // the last tab: the frame puts a start tab in
    for (size_t i = 0; i < nb->GetPageCount(); ++i) {
      auto* t = dynamic_cast<RequestTab*>(nb->GetPage(i));
      if (!t || t->file() != f || t->dirty()) continue;
      nb->RemovePage(i);
      t->Hide();
      if (wxWindow* park = tab_park()) t->Reparent(park);
      later([w = wxWeakRef<wxWindow>(t)] {
        if (w) w->Destroy();
      });
      return;
    }
  }

  wxWindow* tab_park() {
    if (!tab_park_) {
      wxWindow* top = wxGetTopLevelParent(this);
      if (!top) return nullptr;
      tab_park_ = new wxWindow(top, wxID_ANY, wxDefaultPosition, wxSize(1, 1));
      tab_park_->Hide();
    }
    return tab_park_;
  }

  // Quitting with unsaved tabs hidden in another workspace: ask, as the frame does for its own.
  void on_close_tlw(wxCloseEvent& e) {
    size_t dirty = 0;
    for (const auto& [root, set] : tab_sets_)
      for (const auto& p : set.pages) dirty += static_cast<RequestTab*>(p.first)->dirty();
    if (dirty && e.CanVeto()) {
      // Names the workspaces, in the app's colours (a native message box stays light in dark mode).
      std::string where;
      size_t n = 0;
      for (const auto& [root, set] : tab_sets_) {
        size_t k = 0;
        for (const auto& p : set.pages) k += static_cast<RequestTab*>(p.first)->dirty();
        if (!k) continue;
        if (n++ < 3) where += (where.empty() ? "" : ", ") + u8(root.filename()) + " (" + std::to_string(k) + ")";
      }
      if (n > 3) where += ", ...";
      if (!confirm(wxGetTopLevelParent(this),
                   wxString::Format("Quit with %zu unsaved request%s?", dirty, dirty == 1 ? "" : "s"),
                   wxString::FromUTF8("Unsaved changes in other workspaces of this window: " + where +
                                      ". They are lost if you quit now; switch to the workspace and press Ctrl+S to keep them."),
                   "Quit anyway", true)) {
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
    // This workspace's width, else the last one used, else about Bruno's width (250 px), which
    // leaves the request editor the room.
    wxConfigBase* c = wxConfigBase::Get();
    if (!c || !((ws_ && c->Read("Sidebar/Width/" + ws_key(ws_->root()), &w)) || c->Read("Sidebar/Width", &w))) w = 250;
    w = FromDIP(static_cast<int>(w));
    if (collapsed_ || w < FromDIP(160) || std::abs(w - GetSize().x) < 4) return;
    wxAuiManager* m = wxAuiManager::GetManager(this);
    if (!m) return;
    wxAuiPaneInfo& pane = m->GetPane(this);
    if (!pane.IsOk()) return;
    restoring_width_ = true;
    set_pane_width(static_cast<int>(w), pane.min_size);
    restoring_width_ = false;
  }

  void width_changed() {
    if (collapsed_ || restoring_width_ || !width_restored_) return;
    const int w = GetSize().x;
    if (w < FromDIP(160)) return;
    width_timer_.StartOnce(400);
  }

  // Reloads each tab of stale_tabs_ (see set_workspace) in the notebook, the active one first and
  // one per UI step, so its editor reads the folder settings with the workspace there: the
  // inherited variables, headers and auth then show and are sent. Edits, the unsaved mark, the
  // preview state and the shown response are kept. Tabs of another workspace (parked by a
  // switch) wait until they are shown again; a tab with a send in flight is tried later.
  void heal_stale_tabs() {
    wxAuiNotebook* nb = notebook();
    if (!nb || !ws_) return;
    stale_tabs_.erase(std::remove_if(stale_tabs_.begin(), stale_tabs_.end(),
                                     [](const wxWeakRef<RequestTab>& w) { return !w; }),
                      stale_tabs_.end());
    RequestTab* cur = dynamic_cast<RequestTab*>(nb->GetCurrentPage());
    auto pick = stale_tabs_.end();
    for (auto it = stale_tabs_.begin(); it != stale_tabs_.end(); ++it) {
      if (nb->GetPageIndex(it->get()) == wxNOT_FOUND || (*it)->in_flight()) continue;
      if (pick == stale_tabs_.end() || it->get() == cur) pick = it;
      if (it->get() == cur) break;
    }
    if (pick == stale_tabs_.end()) {
      // In flight, or parked with another workspace: look again later.
      if (std::any_of(stale_tabs_.begin(), stale_tabs_.end(),
                      [nb](const wxWeakRef<RequestTab>& w) { return nb->GetPageIndex(w.get()) != wxNOT_FOUND; }))
        heal_timer_.StartOnce(500);
      return;
    }
    RequestTab* t = pick->get();
    stale_tabs_.erase(pick);
    if (t->request_kind() == RequestKind::Http && !t->pristine()) {
      // The editor's title row (breadcrumb) is redrawn only when the file, name or unsaved mark
      // differ from what it last showed, and it last showed them without the folders: the shown
      // tab is loaded once with a trailing space on its name (invisible), and a moment later
      // (after an idle pass redrew the row) with the real name.
      const bool shown = t == cur && t->IsShownOnScreen();
      const std::string name = t->request().name;
      // The tree's highlight follows the active tab (its row may not have been there yet when
      // the frame first said which file is active).
      if (t == cur && !t->file().empty()) later([this, w = wxWeakRef<RequestTab>(t)] {
        if (w && notebook() && notebook()->GetCurrentPage() == w.get()) select_file(w->file());
      });
      heal_log_ += "heal " + u8(t->file().filename()) + (shown ? " (shown)\n" : "\n");
      st_timed("  heal tab", [&] { reload_tab(t, shown ? std::optional<std::string>(name + " ") : std::nullopt); });
      if (shown) {
        wxWeakRef<RequestTab> w(t);
        // After two idle passes of the tab (its editor, a child, gets the same passes and redraws
        // the title row in them); not on a timer, as start-up work can hold idle back for long.
        auto passes = std::make_shared<int>(0);
        t->Bind(wxEVT_IDLE, [this, w, name, passes](wxIdleEvent& e) {
          e.Skip();
          if (*passes < 0) return;  // done (the handler stays bound: one int compare per idle)
          if (++*passes < 2) {
            e.RequestMore();
            return;
          }
          *passes = -1;
          CallAfter([this, w, name] {
            const bool again = w && !w->in_flight() && w->request().name == name + " ";
            heal_log_ += std::string("heal name step: ") + (again ? "reloaded" : "skipped") + "\n";
            if (again) st_timed("  heal tab (name)", [&] { reload_tab(w.get(), name); });
          });
        });
      }
    }
    if (!stale_tabs_.empty()) later([this] { heal_stale_tabs(); });
  }

  // Loads t's request into its editor again (edits, unsaved mark, preview state and the shown
  // response kept), with `name` in place of its name if given.
  static void reload_tab(RequestTab* t, const std::optional<std::string>& name) {
    const bool dirty = t->dirty(), preview = t->preview();
    SavedRequest r = t->request();
    if (name) r.name = *name;
    LastResponse last = t->last_response();
    t->set_request(std::move(r), preview);
    if (dirty) t->mark_dirty();
    t->restore_response(std::move(last));
  }

  void workspace_ready() {
    if (!stale_tabs_.empty()) later([this] { heal_stale_tabs(); });  // tabs a switch brought back
    release_lock();
    take_lock();
    if (width_restored_) later([this] { restore_width(); });
    remember_workspace(ws_->root());
    prune_snapshots();
    update_ws_button();
    if (wxWindow* tlw = wxGetTopLevelParent(this))
      static_cast<wxTopLevelWindow*>(tlw)->SetTitle(wxString::Format("watt - %s", ws_->root().wstring()));
    // Many rows of the old workspace: they go in steps while "Loading..." shows.
    if (tree_->GetCount() > 300) purge_rows_async();
    tree_root_ = CollectionNode{};
    search_keys_.clear();
    rank_keys_.clear();
    meta_.clear();
    name_cache_ = std::make_shared<NameCache>(ws_->root());
    load_expanded();
    load_order();
    history_->set_entries({});
    load_grpc_kinds();
    auto_select_env();
    update_env_choice();
    check_env_files();
    rescan();
    rebuild();
    load_history();
    watch_workspace();
  }

  // ---------------------------------------------------------------- changes made on disk

  // Files changed outside watt (git pull, an editor) are picked up like Bruno does, without
  // Workspace > Reload from disk: the tree is rescanned, environments / globals / .env are
  // re-read (runtime values and the active environment stay), and an open tab of a changed
  // request follows the file when it has no unsaved edits, or says it changed when it has.
  // watt's own saves come back here too and change nothing (same content).
  void watch_workspace() {
    watcher_.reset();
    if (!ws_ || ws_->root().empty()) return;
    std::weak_ptr<int> alive = alive_;
    const fs::path root = ws_->root();
    watcher_ = std::make_unique<WorkspaceWatcher>(root, [this, alive, root](WorkspaceWatcher::Batch b) {
      if (wxAppConsole* app = wxTheApp)
        app->CallAfter([this, alive, root, b = std::move(b)]() mutable {
          if (alive.expired() || !ws_ || ws_->root() != root) return;
          for (auto& p : b) disk_pending_.insert(std::move(p));
          apply_disk_changes();
        });
    });
  }

  static bool modal_open() {
    for (wxWindow* w : wxTopLevelWindows)
      if (auto* d = dynamic_cast<wxDialog*>(w); d && d->IsModal()) return true;
    return false;
  }

  void apply_disk_changes() {
    if (disk_pending_.empty() || !ws_) return;
    // The environment editor (modal) edits the workspace in place: wait until it is closed.
    if (modal_open()) {
      if (disk_timer_.GetOwner() != this) {
        disk_timer_.SetOwner(this, wxWindow::NewControlId());
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { apply_disk_changes(); }, disk_timer_.GetId());
      }
      disk_timer_.StartOnce(500);
      return;
    }
    std::set<fs::path> changed = std::move(disk_pending_);
    disk_pending_.clear();
    bool envs = false, tree = false, all = false;
    std::vector<fs::path> reqs;
    std::vector<fs::path> folders;  // whose folder.json changed (git pull, an editor)
    const fs::path root = ws_->root();
    for (const fs::path& rel : changed) {
      if (rel.empty()) {
        all = true;
        continue;
      }
      const std::wstring first = rel.begin()->wstring();
      const std::wstring name = rel.filename().wstring();
      if (_wcsicmp(first.c_str(), L"environments") == 0 || _wcsicmp(name.c_str(), L"watt.json") == 0 ||
          (_wcsicmp(name.c_str(), L".env") == 0 && rel.has_parent_path() == false)) {
        envs = true;
      } else if (name.size() > 9 && _wcsicmp(name.c_str() + name.size() - 9, L".req.json") == 0) {
        reqs.push_back(root / rel);
        tree = true;  // a new, removed or renamed request, or its name
      } else if (_wcsicmp(name.c_str(), L"folder.json") != 0) {
        tree = true;  // folders; folder.json itself is re-read when used (cached by its stat)
      } else {
        folders.push_back((root / rel).parent_path());
      }
    }
    // Open tabs read what they inherit again (the nearest common folder is enough for one save).
    if (!folders.empty() && !all) {
      fs::path common = folders.front();
      for (const auto& f : folders)
        while (!common.empty() && !is_under(f, common) && common != root) common = common.parent_path();
      // Our own save (refresh_inherit_tabs ran for it already) comes back through the watcher.
      const bool own = !inh_saved_.empty() && is_under(common, inh_saved_) &&
                       std::chrono::steady_clock::now() - inh_saved_at_ < std::chrono::seconds(3);
      if (!own) refresh_inherit_tabs(common);
    } else if (all) {
      refresh_inherit_tabs(root);
    }
    if (all) {
      envs = tree = true;
      if (wxAuiNotebook* nb = notebook())
        for (size_t i = 0; i < nb->GetPageCount(); ++i)
          if (auto* t = dynamic_cast<RequestTab*>(nb->GetPage(i)); t && !t->file().empty()) reqs.push_back(t->file());
    }
    if (tree) rescan();
    if (envs) reload_environments();
    // Only the requests open in a tab are read (on a worker).
    std::vector<fs::path> open;
    for (const auto& f : reqs)
      if (tab_for(f)) open.push_back(f);
    if (!open.empty()) reload_open_requests(std::move(open));
  }

  void reload_environments() {
    std::weak_ptr<int> alive = alive_;
    const fs::path root = ws_->root();
    std::thread([this, alive, root] {
      std::string err;
      auto w = Workspace::open(root, &err);
      if (!w) return;
      auto fresh = std::make_shared<Workspace>(std::move(*w));
      if (wxAppConsole* app = wxTheApp) app->CallAfter([this, alive, root, fresh] {
        if (alive.expired() || !ws_ || ws_->root() != root) return;
        if (modal_open()) {  // edited in place by a dialog right now: try again when it closes
          disk_pending_.insert(fs::path("environments"));
          apply_disk_changes();
          return;
        }
        if (same_environments(*ws_, *fresh)) return;  // watt's own save
        ws_->environments() = std::move(fresh->environments());
        ws_->globals = std::move(fresh->globals);
        ws_->dotenv = std::move(fresh->dotenv);
        bool active = false;
        for (const auto& e : ws_->environments()) active = active || e.name == ws_->active_env;
        if (!active) ws_->active_env = fresh->active_env;
        update_env_choice();
        check_env_files();
        host_->environment_changed();
      });
    }).detach();
  }

  // A request file watt itself renamed, moved or rewrote (a rename writes the new name into
  // it): its write time, so the watcher's report of it is not taken for an outside change. A tab
  // with unsaved edits compared its buffer with that file and claimed it "was changed outside
  // watt".
  void note_own_write(const fs::path& f) {
    std::error_code ec;
    const auto t = fs::last_write_time(f, ec);
    if (ec) return;
    if (own_writes_.size() > 256) own_writes_.clear();
    own_writes_[f] = t;
  }

  void reload_open_requests(std::vector<fs::path> files) {
    auto copy = std::make_shared<Workspace>(*ws_);
    std::weak_ptr<int> alive = alive_;
    std::map<fs::path, fs::file_time_type> own;
    for (const auto& f : files)
      if (auto it = own_writes_.find(f); it != own_writes_.end()) own.insert(*it);
    std::thread([this, alive, copy, files = std::move(files), own = std::move(own)] {
      auto loaded = std::make_shared<std::vector<std::pair<fs::path, SavedRequest>>>();
      for (const auto& f : files) {
        if (auto it = own.find(f); it != own.end()) {
          std::error_code ec;
          if (fs::last_write_time(f, ec) == it->second && !ec) continue;  // still as watt left it
        }
        if (auto r = copy->load_request(f)) loaded->emplace_back(f, std::move(*r));
      }
      if (wxAppConsole* app = wxTheApp) app->CallAfter([this, alive, loaded] {
        if (alive.expired()) return;
        for (auto& [f, r] : *loaded) {
          RequestTab* t = tab_for(f);
          if (!t || t->in_flight()) continue;
          if (same_request(t->request(), r)) continue;  // watt's own save, or no real change
          if (!t->dirty()) {
            t->set_request(std::move(r), t->preview());
            continue;
          }
          notify(wxGetTopLevelParent(this), "Changed on disk",
                 wxString::FromUTF8(u8(f.filename()) + " was changed outside watt, and its tab has unsaved edits.\n\n"
                                    "Ctrl+S keeps your version (it overwrites the file); close the tab without "
                                    "saving to take the one on disk."));
        }
      });
    }).detach();
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
    wxString warn =
        unreadable ? wxString::Format(wxString::FromUTF8("\xE2\x9A\xA0 %zu secret%s cannot be decrypted here: re-enter"),
                                      unreadable, unreadable == 1 ? "" : "s")
                   : wxString();
    wxString tip =
        "Secret values are encrypted for one Windows account on one machine (DPAPI). These were sealed by "
        "another user or PC. Click to open the environment editor and enter them again.";
    // An environment file the workspace skipped: said first (its variables are missing).
    if (!env_bad_.empty()) {
      const auto& f = env_bad_.front();
      const bool all_bom = std::all_of(env_bad_.begin(), env_bad_.end(), [](const auto& u) { return u.bom; });
      warn = wxString::FromUTF8("\xE2\x9A\xA0 environments/") + wxString(f.file.filename().wstring()) +
             (env_bad_.size() > 1 ? wxString::Format(" and %zu more", env_bad_.size() - 1) : wxString()) +
             (all_bom ? wxString(" skipped: UTF-8 BOM, click to fix") : wxString(" skipped: not valid JSON"));
      tip = all_bom ? "These environment files start with a UTF-8 byte order mark (Windows PowerShell 5 "
                      "Set-Content -Encoding utf8, Notepad's \"UTF-8 with BOM\"), which JSON does not allow, so "
                      "their variables are not used.\nClick to remove the mark and reload the workspace."
                    : "These environment files could not be parsed as JSON, so their variables are not used. "
                      "Fix them in an editor, then Workspace menu > Reload from disk. Click to show the file.";
    }
    if (env_warn_->GetLabel() != warn || env_warn_->IsShown() != !warn.empty()) {
      env_warn_->SetLabel(warn);
      env_warn_->SetToolTip(tip);
      env_warn_->Show(!warn.empty());
      content_->Layout();
    }
  }

  // Environment files the workspace could not read (a BOM, broken JSON): checked on a worker
  // whenever the workspace is (re)opened.
  void check_env_files() {
    if (!ws_) return;
    std::weak_ptr<int> alive = alive_;
    const fs::path root = ws_->root();
    std::thread([this, alive, root] {
      auto bad = sidebar::unreadable_env_files(root);
      if (wxAppConsole* app = wxTheApp) app->CallAfter([this, alive, root, bad = std::move(bad)]() mutable {
        if (alive.expired() || !ws_ || ws_->root() != root) return;
        if (bad.empty() && env_bad_.empty()) return;
        env_bad_ = std::move(bad);
        update_env_choice();
      });
    }).detach();
  }

  // The warning was clicked: the BOMs are removed (only those files, only that mark) and the
  // workspace is read again, so the variables apply at once. Broken JSON: the file is shown.
  void fix_env_files() {
    if (!ws_ || env_bad_.empty()) return;
    std::vector<fs::path> files;
    for (const auto& u : env_bad_)
      if (u.bom && is_under(u.file, ws_->root())) files.push_back(u.file);
    if (files.empty()) {
      reveal_in_explorer(env_bad_.front().file);
      return;
    }
    std::weak_ptr<int> alive = alive_;
    std::thread([this, alive, files] {
      std::string err;
      bool ok = true;
      for (const auto& f : files) ok = sidebar::remove_utf8_bom(f, &err) && ok;
      if (wxAppConsole* app = wxTheApp) app->CallAfter([this, alive, ok, err] {
        if (alive.expired() || !ws_) return;
        if (!ok) notify(wxGetTopLevelParent(this), "Could not fix the environment file", wxString::FromUTF8(err));
        switch_workspace(ws_->root(), true);
      });
    }).detach();
  }

  void env_menu() {
    if (!ws_) return;
    wxMenu m;
    enum { kNone = 1, kManage = 2, kCookies = 3, kEnv = 100 };
    m.AppendRadioItem(kNone, "&No environment")->Check(ws_->active_env.empty());
    for (size_t i = 0; i < env_names_.size(); ++i)
      m.AppendRadioItem(kEnv + static_cast<int>(i), wxString::FromUTF8(env_names_[i]))
          ->Check(env_names_[i] == ws_->active_env);
    m.AppendSeparator();
    m.Append(kManage, wxString::FromUTF8("&Manage environments\xE2\x80\xA6"));
    m.Append(kCookies, wxString::FromUTF8("&Cookies\xE2\x80\xA6"));
    const int id = env_menu_btn_->GetPopupMenuSelectionFromUser(m, wxPoint(0, env_menu_btn_->GetSize().y));
    if (id == kCookies) open_cookies();
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
    later([this, chosen] { open_preview(chosen); });  // the dialog closes first
  }

  // ---------------------------------------------------------------- names, history labels

  // meta_ changed: rebuild the method+URL -> name index that labels history rows.
  // `prebuilt`: the index the scan worker built for exactly tree_root_ and meta_.
  void meta_changed(NameIndex* prebuilt = nullptr) {
    search_keys_.clear();
    rank_keys_.clear();
    NameIndex x = prebuilt ? std::move(*prebuilt) : build_name_index(tree_root_, meta_);
    name_by_key_ = std::move(x.name_by_key);
    path_by_key_ = std::move(x.path_by_key);
    paths_by_key_ = std::move(x.paths_by_key);
    path_by_id_ = std::move(x.path_by_id);
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

  // The file whose folder / collection settings a re-send of the entry inherits: the saved
  // request it came from, else the file it was sent as (a request sent unsaved from a folder,
  // or one deleted since, still inherits that folder's headers, auth, vars and scripts).
  fs::path inherit_origin(const HistoryEntry& e) const {
    if (fs::path f = history_file(e); !f.empty()) return f;
    fs::path f = e.request.file;
    if (f.empty())
      if (auto it = hist_names_.find(e.when_iso + "\x01" + e.summary); it != hist_names_.end()) f = it->second.file;
    return folder_origin(f);
  }
  // f when its folder is the workspace root or a folder of the tree (a snapshot: its origin).
  fs::path folder_origin(const fs::path& f) const {
    if (!ws_ || f.empty()) return {};
    if (is_snapshot(f)) {
      auto it = snapshot_origin_.find(f);
      return it == snapshot_origin_.end() ? fs::path() : it->second;
    }
    if (!is_under(f, ws_->root()) || is_under(f, ws_->root() / ".watt")) return {};
    const fs::path dir = f.parent_path();
    if (!same_path(dir, ws_->root()) && !find_node(tree_root_, dir)) return {};
    return f;
  }
  // Writes the snapshot -> origin map (.watt/snapshots/origins.json) on a worker; the newest
  // map wins when two writes overlap.
  void persist_snapshot_origins() {
    if (!ws_) return;
    auto m = std::make_shared<std::map<std::string, std::string>>();
    const fs::path root = ws_->root();
    for (const auto& [snap, origin] : snapshot_origin_) {
      if (origin.empty() || !is_under(origin, root) || snap.parent_path() != snapshot_dir()) continue;
      std::string v = u8(fs::path(origin.lexically_relative(root).generic_wstring()));
      if (safe_relative_origin(v)) (*m)[u8(snap.filename())] = std::move(v);
    }
    static std::atomic<uint64_t> latest{0};
    static std::mutex mu;
    static uint64_t written = 0;
    const uint64_t g = ++latest;
    std::thread([m, root, g] {
      std::lock_guard lk(mu);
      if (g < written || g != latest.load()) return;  // a newer map is on its way
      written = g;
      save_snapshot_origins(root, *m);
    }).detach();
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
    const fs::path origin = history_file(e);  // the saved request (open / reveal)
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
    const fs::path inherit = inherit_origin(e);
    const fs::path snap = snapshot_file(e);
    snapshot_entry_[snap] = {e.when_iso, e.summary};
    if (snapshot_origin_.count(snap) && tab_for(snap)) {
      host_->open_request(snap, !permanent);  // the tab that shows it (with its response)
      keep_history_focus(permanent, snap);
      return;
    }
    std::string base = origin.empty() ? std::string() : meta_name(origin);
    if (base.empty()) base = history_name(e);
    const std::string title = base.empty() ? std::string() : base + " @ " + history_when_label(e.when_iso);
    auto copy = std::make_shared<Workspace>(*ws_);
    std::weak_ptr<int> alive = alive_;
    const uint64_t gen = ++open_gen_;
    const bool loaded = !e.request.id.empty() || !e.request.http.url.empty() || !e.request.grpc_method.empty();
    // Sent in this session: its full response from memory, not the capped copy on disk.
    std::shared_ptr<LastResponse> session = session_response(e);
    std::thread([this, copy, e, alive, gen, snap, inherit, title, loaded, permanent, session] {
      std::optional<SavedRequest> r;
      fs::path sent_as;
      if (loaded) r = e.request;
      else r = copy->history_request(e);
      bool ok = false;
      // The response the entry got, with its timing: sent in this session, the full one from
      // memory (the response view formats big bodies on its workers, as for a live response);
      // otherwise the capped copy on disk (the status line says so).
      std::shared_ptr<LastResponse> resp = session ? std::make_shared<LastResponse>(*session) : nullptr;
      if (r) {
        if (auto j = resp ? std::nullopt : load_history_response(copy->root(), e))
          if (LastResponse lr = last_response_from_json(*j, *r)) resp = std::make_shared<LastResponse>(std::move(lr));
        if (resp) resp->when_iso = e.when_iso;
        sent_as = r->file;
        r->file = snap;
        if (!title.empty()) r->name = title;
        std::string err;
        ok = copy->save_request(*r, &err);
      }
      const bool found = r.has_value();
      if (wxAppConsole* app = wxTheApp) app->CallAfter([this, alive, gen, snap, inherit, sent_as, ok, found, permanent, resp] {
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
        // Inherits the folder it was sent from (also when it was sent unsaved or the saved
        // request is gone), kept across restarts for a restored snapshot tab.
        snapshot_origin_[snap] = inherit.empty() ? folder_origin(sent_as) : inherit;
        persist_snapshot_origins();
        host_->open_request(snap, !permanent);
        keep_history_focus(permanent, snap);
        // Its stored response (body and timing) in the next UI step: the tab swap and the
        // response view each stay under the budget.
        if (resp)
          later([this, alive, snap, resp] {
            if (alive.expired()) return;
            RequestTab* t = tab_for(snap);
            if (!t || t->in_flight()) return;
            // A tab of this snapshot restored from the last session may hold only the head of a
            // large response (status, timing, no body): the history's copy has the body (cut)
            // with the same timing, so it replaces that.
            const LastResponse& have = t->last_response();
            const bool head_only = have.http && have.http->body.empty() && resp->http && !resp->http->body.empty();
            if (!have || head_only) t->restore_response(*resp);
          });
      });
    }).detach();
  }

  // A single click previews an entry and keeps the keyboard in the history list (arrows, Del,
  // Right / Left), as a tree click keeps it in the tree; Enter / double click go to the tab.
  void keep_history_focus(bool permanent, const fs::path& snap) {
    if (permanent) return;
    auto grab = [this] {
      if (history_->IsShownOnScreen() && wxWindow::FindFocus() != history_) history_->SetFocus();
    };
    CallAfter(grab);
    // The tab moves the focus to its editor while it builds itself (a lazy tab realizes on its
    // first show, its response view ~300 ms later restores the focus it had): take it back for
    // a short while, unless the pointer is over the tab (the user clicked into it).
    hist_focus_snap_ = snap;
    hist_focus_ticks_ = 0;
    hist_focus_timer_.Start(100);
  }

  void hist_focus_tick() {
    if (++hist_focus_ticks_ > 9) {
      hist_focus_timer_.Stop();
      return;
    }
    RequestTab* tab = tab_for(hist_focus_snap_);
    if (!tab || !history_->IsShownOnScreen()) {
      hist_focus_timer_.Stop();
      return;
    }
    if (tab->GetScreenRect().Contains(wxGetMousePosition())) {
      hist_focus_timer_.Stop();
      return;
    }
    for (wxWindow* f = wxWindow::FindFocus(); f; f = f->GetParent())
      if (f == tab) {
        history_->SetFocus();
        break;
      }
  }

  // ---------------------------------------------------------------- history responses, actions

  // A request finished (history_added): keep the response its tab now shows with the entry
  // (capped, on a worker), so reopening the entry later shows it with its timing.
  void keep_response(const HistoryEntry& e) {
    if (!ws_) return;
    LastResponse lr;
    if (wxAuiNotebook* nb = notebook())
      for (size_t i = 0; i < nb->GetPageCount() && !lr; ++i) {
        auto* t = dynamic_cast<RequestTab*>(nb->GetPage(i));
        if (!t) continue;
        const LastResponse& l = t->last_response();
        if (e.kind == RequestKind::Http && l.http && l.http->timing.total_ms == e.total_ms && l.http->status == e.status)
          lr = l;
        else if (e.kind == RequestKind::Grpc && l.grpc && l.grpc->timing.total_ms == e.total_ms &&
                 l.grpc->status_code == e.status)
          lr = l;
        if (lr && e.kind == RequestKind::Grpc) learn_grpc_kinds(t);
      }
    if (!lr) return;
    remember_session_response(e, lr);
    const fs::path root = ws_->root();
    const bool prune = (resp_saves_++ % 64) == 0;
    std::thread([root, k = slim(e), lr, prune] {
      save_history_response(root, k, last_response_to_json(capped_response(lr)));
      if (prune) prune_history_responses(root);
    }).detach();
  }

  static std::string session_key(const HistoryEntry& e) {
    return e.when_iso + '\t' + e.summary + '\t' + std::to_string(e.status);
  }
  static size_t response_bytes(const LastResponse& r) {
    size_t n = r.http ? r.http->body.size() : 0;
    if (r.grpc)
      for (const auto& m : r.grpc->messages) n += m.json.size();
    return n;
  }
  void remember_session_response(const HistoryEntry& e, const LastResponse& lr) {
    constexpr size_t kMaxEntries = 64, kMaxBytes = size_t{128} << 20;
    session_resp_bytes_ += response_bytes(lr);
    session_resp_.emplace_back(session_key(e), lr);
    while (session_resp_.size() > 1 && (session_resp_.size() > kMaxEntries || session_resp_bytes_ > kMaxBytes)) {
      session_resp_bytes_ -= response_bytes(session_resp_.front().second);
      session_resp_.pop_front();
    }
  }
  std::shared_ptr<LastResponse> session_response(const HistoryEntry& e) const {
    const std::string k = session_key(e);
    for (auto it = session_resp_.rbegin(); it != session_resp_.rend(); ++it)
      if (it->first == k) return std::make_shared<LastResponse>(it->second);
    return nullptr;
  }

  void delete_history(const std::vector<HistoryEntry>& v) {
    if (!ws_ || v.empty()) return;
    std::vector<HistoryEntry> keys;
    keys.reserve(v.size());
    for (const auto& e : v) keys.push_back(slim(e));
    history_->remove(keys);
    update_counts();
    update_history_pane();
    const fs::path root = ws_->root();
    std::thread([root, keys] { hide_history_entries(root, keys); }).detach();
  }

  void clear_history_all() {
    if (!ws_ || !history_->total()) return;
    const size_t n = history_->total();
    if (!confirm(wxGetTopLevelParent(this), "Clear history?",
                 wxString::Format("The %zu entr%s of this workspace's history and their kept responses are removed. "
                                  "Saved requests are not touched.",
                                  n, n == 1 ? "y" : "ies"),
                 "Clear history", true))
      return;
    std::vector<HistoryEntry> keys;
    keys.reserve(n);
    for (const auto& e : history_->entries()) keys.push_back(slim(e));
    history_->clear_all();
    update_counts();
    update_history_pane();
    const fs::path root = ws_->root();
    std::thread([root, keys] { clear_history(root, keys); }).detach();
  }

  // Loads an entry's request on a worker, then runs fn on the UI thread with it.
  void with_history_request(const HistoryEntry& e, std::function<void(SavedRequest)> fn) {
    if (!ws_) return;
    const bool loaded = !e.request.id.empty() || !e.request.http.url.empty() || !e.request.grpc_method.empty();
    if (loaded) {
      fn(e.request);
      return;
    }
    auto copy = std::make_shared<Workspace>(*ws_);
    std::weak_ptr<int> alive = alive_;
    std::thread([this, copy, k = slim(e), alive, fn] {
      auto r = std::make_shared<std::optional<SavedRequest>>(copy->history_request(k));
      if (wxAppConsole* app = wxTheApp) app->CallAfter([this, alive, r, fn] {
        if (alive.expired()) return;
        if (!*r) {
          load_history();  // trimmed under the entry
          return;
        }
        fn(std::move(**r));
      });
    }).detach();
  }

  // "Save as request": the request as the entry sent it becomes a new saved request next to
  // the one it came from (else at the root), named in the tree like a duplicate.
  void save_history_as(const HistoryEntry& e) {
    const fs::path origin = history_file(e);
    std::string name = history_name(e);
    if (name.empty()) {
      const std::string tmpl(history_template(e));
      std::string t = e.kind == RequestKind::Grpc ? grpc_method_of(e) : tmpl;
      if (const size_t q = t.find('?'); q != std::string::npos) t.resize(q);
      while (!t.empty() && t.back() == '/') t.pop_back();
      const size_t sl = t.rfind('/');
      name = sl == std::string::npos ? t : t.substr(sl + 1);
      if (e.kind == RequestKind::Http) {
        const size_t sp = tmpl.find(' ');
        name = (sp == std::string::npos ? std::string("GET") : tmpl.substr(0, sp)) + " " + name;
      }
    }
    with_history_request(e, [this, origin, name](SavedRequest r) {
      if (!ws_) return;
      const fs::path folder = origin.empty() ? ws_->root() : origin.parent_path();
      r.id.clear();
      r.file = free_request_file(folder, strip_snapshot_suffix(name.empty() ? std::string("Request") : name), &r.name);
      std::string err;
      if (!ws_->save_request(r, &err)) {
        notify(wxGetTopLevelParent(this), "Could not save", wxString::FromUTF8(err));
        return;
      }
      if (name_cache_) name_cache_->note(r.file, r.name, r.kind == RequestKind::Grpc ? r.grpc_method : r.http.url, r.id);
      if (folder != ws_->root()) expanded_.insert(folder);
      if (!filter_q_.empty()) {
        filter_->ChangeValue("");
        apply_filter();
      }
      name_then_open(r.file);
    });
  }

  void copy_history_curl(const HistoryEntry& e) {
    with_history_request(e, [this](SavedRequest r) {
      if (!ws_) return;
      const SavedRequest x = ws_->expanded(r);
      copy_text(wxString::FromUTF8(x.kind == RequestKind::Grpc ? to_grpcurl_command(x) : to_curl_command(x)));
    });
  }

  // ---------------------------------------------------------------- gRPC call kinds

  // The call kind of a gRPC method (GrpcCallKind as int), learned from the schemas the tabs
  // loaded and kept in .watt/sidebar-grpc-kinds.tsv; -1 when not known yet.
  int grpc_kind_of(const std::string& method) const {
    if (method.empty()) return -1;
    auto it = grpc_kinds_.find(method);
    return it == grpc_kinds_.end() ? -1 : it->second;
  }

  int node_image(const CollectionNode& c) const {
    if (c.kind != RequestKind::Grpc) return tree_image(c.kind, c.method);
    if (grpc_kinds_.empty()) return kImgGrpc;
    auto m = meta_.find(c.path);
    return grpc_tree_image(m == meta_.end() ? -1 : grpc_kind_of(m->second.url));
  }

  // False while the tab has no schema yet.
  bool learn_grpc_kinds(RequestTab* t) {
    const auto schema = t->session_schema();
    if (!schema) return false;
    if (schema == last_learned_) return true;
    last_learned_ = schema;
    bool changed = false;
    for (const auto& m : schema->methods) {
      const int k = static_cast<int>(m.kind);
      auto [it, inserted] = grpc_kinds_.try_emplace(m.full_path, k);
      if (!inserted && it->second == k) continue;
      it->second = k;
      changed = true;
    }
    if (!changed) return true;
    grpc_kinds_changed();
    if (!ws_) return true;
    std::string text;
    for (const auto& [m, k] : grpc_kinds_) text += m + "\t" + std::to_string(k) + "\n";
    const fs::path file = ws_->root() / ".watt" / "sidebar-grpc-kinds.tsv";
    std::thread([file, text] { write_text_file(file, text); }).detach();
    return true;
  }

  void load_grpc_kinds() {
    grpc_kinds_.clear();
    last_learned_.reset();
    if (!ws_) return;
    const fs::path file = ws_->root() / ".watt" / "sidebar-grpc-kinds.tsv";
    std::weak_ptr<int> alive = alive_;
    const uint64_t gen = ws_gen_;
    std::thread([this, file, alive, gen] {
      auto* kinds = new std::map<std::string, int>;
      std::ifstream in(file, std::ios::binary);
      std::string line;
      while (std::getline(in, line)) {
        const size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0 || tab + 2 != line.size() || line[tab + 1] < '0' || line[tab + 1] > '3')
          continue;
        (*kinds)[line.substr(0, tab)] = line[tab + 1] - '0';
      }
      if (wxAppConsole* app = wxTheApp) app->CallAfter([this, alive, gen, kinds] {
        std::unique_ptr<std::map<std::string, int>> k(kinds);
        if (alive.expired() || gen != ws_gen_ || k->empty()) return;
        for (auto& [m, v] : *k) grpc_kinds_.try_emplace(m, v);  // what tabs taught meanwhile wins
        grpc_kinds_changed();
      });
    }).detach();
  }

  // New kinds: the gRPC rows of the tree and the history get their arrows.
  void grpc_kinds_changed() {
    std::function<void(const wxTreeItemId&)> walk = [&](const wxTreeItemId& parent) {
      wxTreeItemIdValue cookie;
      for (wxTreeItemId c = tree_->GetFirstChild(parent, cookie); c.IsOk(); c = tree_->GetNextChild(parent, cookie)) {
        const auto* d = data(c);
        if (!d) continue;
        if (d->node->is_folder) {
          walk(c);
        } else if (d->node->badge && d->node->kind == RequestKind::Grpc) {
          const int img = node_image(*d->node);
          if (tree_->GetItemImage(c) != img) {
            tree_->SetItemImage(c, img);
            tree_->SetItemImage(c, img, wxTreeItemIcon_Selected);
          }
        }
      }
    };
    if (tree_->GetRootItem().IsOk()) walk(tree_->GetRootItem());
    history_->Refresh();
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
    if (nb_cache_) return nb_cache_.get();
    auto* top = wxGetTopLevelParent(const_cast<Sidebar*>(this));
    if (!top) return nullptr;
    std::function<wxAuiNotebook*(wxWindow*)> find = [&](wxWindow* w) -> wxAuiNotebook* {
      if (auto* nb = wxDynamicCast(w, wxAuiNotebook)) return nb;
      for (wxWindow* c : w->GetChildren())
        if (auto* nb = find(c)) return nb;
      return nullptr;
    };
    nb_cache_ = find(top);
    return nb_cache_.get();
  }

  // ---------------------------------------------------------------- dormant tabs

  // A tab that is not shown is kept frozen. wxWindow::Freeze() recurses into every descendant
  // unless the window is frozen already, and wxAuiNotebook freezes itself on every page removed
  // (DoRemovePage), as does the frame on each AUI update without live resize: with 8 tabs that
  // walked 1500 windows twice (60-100 ms of a tab close, a workspace switch, a Ctrl+B). A frozen
  // hidden page stops that walk at itself, so those cost the shown tab only. Each hidden page is
  // frozen once, a little after it was hidden (one page per step, ~5 ms), and thawed as it is
  // shown (wxEVT_SHOW comes before its first paint).
  void dormant_tick() {
    wxAuiNotebook* nb = notebook();
    if (!nb) return;
    const auto t0 = std::chrono::steady_clock::now();
    auto budget_left = [&] { return std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(8); };
    for (size_t i = 0; i < nb->GetPageCount(); ++i) {
      wxWindow* w = nb->GetPage(i);
      if (!w) continue;
      if (w->IsShown()) {
        dormant_wake(w);  // shown without a show event: never stays frozen
        continue;
      }
      if (static_cast<int>(i) != nb->GetSelection() && dormant_freeze(w) && !budget_left()) return;  // the rest next tick
    }
    // Other workspaces' tabs (parked, see swap_tabs()): they come back without a walk too.
    for (const auto& [root, set] : tab_sets_)
      for (const auto& pg : set.pages)
        if (!pg.first->IsShown() && dormant_freeze(pg.first) && !budget_left()) return;
  }

  // Freezes a hidden tab; false when it was frozen already.
  bool dormant_freeze(wxWindow* w) {
    if (dormant_.count(w)) return false;
    if (dormant_bound_.insert(w).second) {
      w->Bind(wxEVT_SHOW, [this, w, alive = std::weak_ptr<int>(alive_)](wxShowEvent& e) {
        e.Skip();
        if (!alive.expired() && e.GetEventObject() == w && e.IsShown()) dormant_wake(w);
      });
      w->Bind(wxEVT_DESTROY, [this, w, alive = std::weak_ptr<int>(alive_)](wxWindowDestroyEvent& e) {
        e.Skip();
        if (alive.expired() || e.GetEventObject() != w) return;
        dormant_.erase(w);
        dormant_bound_.erase(w);
      });
    }
    w->Freeze();
    dormant_.insert(w);
    return true;
  }

  void dormant_wake(wxWindow* w) {
    if (!dormant_.erase(w)) return;
    w->Thaw();
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
    if (e.kind == RequestKind::Grpc) *name = grpc_display_name(*name, grpc_method_of(e));
    // Where it went, as recorded at send time (history_summary.h), without the scheme. Never
    // re-expanded with the environment that is active now: after switching local -> staging a
    // row sent to 127.0.0.1:18971 must still say so. Entries older than that record show their
    // {{template}} as written.
    std::string t(history_template(e));
    const std::string_view sent = history_sent(e);
    if (e.kind == RequestKind::Http) {
      if (!sent.empty()) t = sent;
      else if (const size_t sp = t.find(' '); sp != std::string::npos) t.erase(0, sp + 1);
    } else {
      if (!sent.empty()) t = grpc_method_of(e) + " @ " + std::string(sent);
      if (!t.empty() && t[0] == '/') t.erase(0, 1);
    }
    *target = strip_scheme(std::move(t));
  }

  // ---------------------------------------------------------------- manual order

  // Folders first, then requests. Within each, the order the user dragged them into (per
  // folder, by file name), the rest after it in natural order (case-insensitive, 2 before 10)
  // of their file names, not the display names: "01-login", "02-items" order a folder the way
  // Run folder and watt --run run it (runner_engine flatten()), whatever the requests are called.
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
    struct Key {
      size_t rank;
      std::string file;  // file name without .req.json (what Workspace::tree() names the node)
      const CollectionNode* n;
    };
    std::vector<Key> v;
    v.reserve(kids.size());
    for (const CollectionNode* k : kids) {
      const auto u = k->path.filename().u8string();
      std::string f(u.begin(), u.end());
      if (!k->is_folder && f.size() > 9 && f.compare(f.size() - 9, 9, ".req.json") == 0) f.resize(f.size() - 9);
      v.push_back({rank(k), std::move(f), k});
    }
    std::stable_sort(v.begin(), v.end(), [](const Key& a, const Key& b) {
      if (a.n->is_folder != b.n->is_folder) return a.n->is_folder;
      if (a.rank != b.rank) return a.rank < b.rank;
      return natural_less(a.file, b.file);
    });
    for (size_t i = 0; i < v.size(); ++i) kids[i] = v[i].n;
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
      centre_item(id);
    }
    syncing_ = false;
    sticky_sel_ = from;
  }

  static const CollectionNode* find_folder(const CollectionNode& n, const fs::path& p) {
    if (same_path(n.path, p)) return &n;
    for (const auto& c : n.children)
      if (c.is_folder && (same_path(c.path, p) || is_under(p, c.path)))
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
    rank_keys_.clear();  // tree order changed
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

  void import_curl(const fs::path& folder_in, const std::string& pasted = {}) {
    if (!ws_) return;
    // One request: into the folder of the selected row (where "+" creates one), not the root.
    const fs::path folder = folder_in.empty() ? target_folder(tree_->GetSelection()) : folder_in;
    std::string cmd = pasted, name;
    wxWindow* top = wxGetTopLevelParent(this);
    if (cmd.empty() && (!ask_curl(top, &cmd, &name) || cmd.empty())) return;
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
    if (name_cache_) name_cache_->note(file, r.name, r.http.url, r.id);
    if (!filter_q_.empty()) {  // the new item must be visible to be named
      filter_->ChangeValue("");
      apply_filter();
    }
    // Named in the tree like a new request (Enter keeps the proposed name), then opened.
    name_then_open(file);
    if (!imp->warnings.empty()) {
      std::string msg;
      for (const auto& w : imp->warnings) msg += "\xE2\x80\xA2 " + w + "\n";
      notify(top, "Imported, with notes", wxString::FromUTF8(msg));
    }
  }

  // Collection import. The file dialog is the only UI-thread step; parsing and writing the
  // .req.json files run on a worker with a copy of the workspace, then one rescan shows them.
  enum { kImportCurl, kImportPostman, kImportInsomnia, kImportBruno, kImportOpenApi, kImportHar, kImportText, kImportKinds };
  static constexpr int kImportBase = 200;

  static void append_import_items(wxMenu& m, int base) {
    m.Append(base + kImportPostman, wxString::FromUTF8("&Postman collection (v2.0 / v2.1)\xE2\x80\xA6"));
    m.Append(base + kImportInsomnia, wxString::FromUTF8("&Insomnia export (v4 JSON)\xE2\x80\xA6"));
    m.Append(base + kImportBruno, wxString::FromUTF8("&Bruno collection folder\xE2\x80\xA6"));
    m.Append(base + kImportOpenApi, wxString::FromUTF8("&OpenAPI 3 / Swagger (JSON or YAML)\xE2\x80\xA6"));
    m.Append(base + kImportHar, wxString::FromUTF8("H&AR file\xE2\x80\xA6"));
    m.AppendSeparator();
    m.Append(base + kImportCurl, wxString::FromUTF8("c&URL command\xE2\x80\xA6"));
    m.Append(base + kImportText, wxString::FromUTF8("Paste &text or URL\xE2\x80\xA6"));
  }

  void import_menu(wxWindow* anchor, const fs::path& folder) {
    if (!ws_ || !anchor) return;
    wxMenu m;
    append_import_items(m, 1);
    const int id = anchor->GetPopupMenuSelectionFromUser(m, wxPoint(0, anchor->GetSize().y));
    if (id >= 1 && id < 1 + kImportKinds) import_kind(id - 1, folder);
  }

  void import_kind(int kind, const fs::path& folder) {
    if (kind == kImportCurl) return import_curl(folder);
    if (kind == kImportText) return import_text(folder);
    if (!ws_) return;
    wxWindow* top = wxGetTopLevelParent(this);
    if (importing_) {
      notify(top, "An import is running", "Wait for it to finish, then import the next collection.");
      return;
    }
    if (picking_) return;  // a file dialog is open (its owner, this window, is disabled meanwhile)
    const fs::path dest = folder.empty() ? ws_->root() : folder;
    std::wstring title;
    std::vector<std::pair<std::wstring, std::wstring>> filters;
    switch (kind) {
      case kImportBruno:
        title = L"Choose a Bruno collection folder (the one with bruno.json)";
        break;
      case kImportPostman:
        title = L"Import a Postman collection";
        filters = {{L"Postman collection or environment (*.json)", L"*.json"}};
        break;
      case kImportInsomnia:
        title = L"Import an Insomnia export";
        filters = {{L"Insomnia v4 export (*.json)", L"*.json"}};
        break;
      case kImportOpenApi:
        title = L"Import an OpenAPI / Swagger document";
        filters = {{L"OpenAPI (*.json;*.yaml;*.yml)", L"*.json;*.yaml;*.yml"}};
        break;
      default:
        title = L"Import a HAR file";
        filters = {{L"HAR (*.har;*.json)", L"*.har;*.json"}};
    }
    if (!filters.empty()) filters.emplace_back(L"All files (*.*)", L"*.*");
    // On a thread of its own: closing the OS dialog costs 30-40 ms of shell work, which then
    // never holds up a UI step (the import that follows is on a worker too).
    picking_ = true;
    std::weak_ptr<int> alive = alive_;
    const fs::path root = ws_->root();
    pick_file_async(top, title, filters, kind == kImportBruno, [this, alive, root, dest](const fs::path& src) {
      if (alive.expired()) return;
      picking_ = false;
      focus_tree_after_dialog();
      if (src.empty() || !ws_ || ws_->root() != root) return;
      later([this, alive, src, dest] {
        if (!alive.expired()) start_import(src, dest);
      });
    });
  }

  // A modal OS dialog hands the focus back to the first control of the frame (the sidebar's
  // hide button: Space / Enter would then collapse the sidebar). The tree takes it instead.
  void focus_tree_after_dialog() {
    ++tree_focus_req_;
    tree_->SetFocus();
    CallAfter([this, req = tree_focus_req_] {
      if (req == tree_focus_req_ && wxWindow::FindFocus() != tree_) tree_->SetFocus();
    });
  }

  // Pasted text, a curl command or an http(s) URL (fetched on the worker, then parsed like a file).
  void import_text(const fs::path& folder_in) {
    if (!ws_) return;
    wxWindow* top = wxGetTopLevelParent(this);
    if (importing_) {
      notify(top, "An import is running", "Wait for it to finish, then import the next collection.");
      return;
    }
    const fs::path dest = folder_in.empty() ? ws_->root() : folder_in;
    std::string text;
    if (!ask_import_text(top, &text)) return;
    if (ascii_lower(text.substr(0, 5)) == "curl ") return import_curl(folder_in, text);
    const std::string head = ascii_lower(text.substr(0, 8));
    const bool url = (head.rfind("http://", 0) == 0 || head.rfind("https://", 0) == 0) &&
                     text.find_first_of(" \t\r\n") == std::string::npos;
    if (url) {
      start_import_with(dest, [text](std::string* err) -> std::optional<ImportResult> {
        HttpClient c;
        HttpRequest rq;
        rq.method = "GET";
        rq.url = text;
        rq.timeout_ms = 30000;
        rq.busy_poll_ms = 0;
        rq.headers.push_back({"Accept", "application/json, application/yaml, text/yaml, */*"});
        const HttpResponse r = c.send(rq);
        if (!r.ok) {
          *err = "Could not fetch " + text + ": " + r.error;
          return std::nullopt;
        }
        if (r.status < 200 || r.status >= 300) {
          *err = "Could not fetch " + text + ": HTTP " + std::to_string(r.status);
          return std::nullopt;
        }
        if (r.body.size() > (size_t{64} << 20)) {
          *err = "The document at " + text + " is larger than 64 MB.";
          return std::nullopt;
        }
        return parse_collection_text(r.body, err);
      });
      return;
    }
    start_import_with(dest, [t = std::move(text)](std::string* err) { return parse_collection_text(t, err); });
  }

  void drop_import(const wxArrayString& files, const fs::path& dest) {
    wxWindow* top = wxGetTopLevelParent(this);
    if (importing_) {
      notify(top, "An import is running", "Wait for it to finish, then import the next collection.");
      return;
    }
    const fs::path src(files[0].ToStdWstring());
    // After the drop returns (the OLE drag source is still waiting for it).
    CallAfter([this, src, dest, more = files.size() > 1] {
      start_import(src, dest);
      if (more)
        notify(wxGetTopLevelParent(this), "One collection per drop",
               wxString::FromUTF8("Importing " + u8(src.filename()) + "; drop the other files one at a time."));
    });
  }

  void start_import(const fs::path& src, const fs::path& dest) {
    start_import_with(dest, [src](std::string* err) {
      return parse_collection_file(src, err);
    });
  }

  // Parses on a worker (`parse` runs there), writes the requests, then one rescan shows them.
  void start_import_with(const fs::path& dest, std::function<std::optional<ImportResult>(std::string*)> parse) {
    if (!ws_ || importing_) return;
    importing_ = true;
    if (import_btn_) {
      import_btn_->SetLabel(wxString::FromUTF8("Importing\xE2\x80\xA6"));
      import_btn_->Disable();
      import_btn_->GetParent()->Layout();
    }
    auto copy = std::make_shared<Workspace>(*ws_);
    std::weak_ptr<int> alive = alive_;
    const fs::path root = ws_->root();
    std::thread([this, copy, parse = std::move(parse), dest, alive, root] {
      const auto t0 = std::chrono::steady_clock::now();
      auto err = std::make_shared<std::string>();
      auto parsed = std::make_shared<std::optional<ImportResult>>(parse(err.get()));
      auto written = std::make_shared<ImportWritten>();
      if (*parsed) *written = write_import(*copy, dest, **parsed);
      if (*parsed && written->requests) {
        // What the imported requests can resolve: the workspace's variables and what the folders
        // above the destination give (folder.json, read here on the worker).
        std::set<std::string> defined;
        for (const auto& e : copy->environments())
          if (e.name == copy->active_env)
            for (const auto& [k, v] : e.vars) defined.insert(k);
        for (const auto& [k, v] : copy->globals) defined.insert(k);
        for (const auto& [k, v] : copy->runtime_vars) defined.insert(k);
        SavedRequest probe;
        probe.file = (written->top.empty() ? dest : written->top) / L"probe.req.json";
        for (const auto& row : copy->effective_settings(probe).vars)
          if (row.row.enabled) defined.insert(row.row.name);
        written->undefined = undefined_import_vars(**parsed, defined);
      }
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      if (wxAppConsole* app = wxTheApp) app->CallAfter([this, alive, root, dest, parsed, written, err, ms] {
        if (alive.expired()) return;
        import_done(root, dest, *parsed, *written, *err, ms);
      });
    }).detach();
  }

  void import_done(const fs::path& root, const fs::path& dest, std::optional<ImportResult>& parsed, ImportWritten& w,
                   const std::string& err, double ms) {
    importing_ = false;
    if (import_btn_) {
      import_btn_->SetLabel("Import");
      import_btn_->Enable();
      import_btn_->GetParent()->Layout();
    }
    wxWindow* top = wxGetTopLevelParent(this);
    if (!parsed) {
      notify(top, "Could not import", wxString::FromUTF8(err));
      return;
    }
    if (!ws_ || ws_->root() != root) return;  // the window switched workspaces meanwhile
    const ImportResult& r = *parsed;
    // Collection variables went into the imported folder's folder.json (they resolve under any
    // environment); r.envs are real environments (a Postman environment file, Insomnia
    // sub-environments, Bruno environments/*.bru). With none active the first one is switched
    // on; otherwise the notice says it is not active and a click switches to it.
    std::string inactive_env;
    if (!r.envs.empty() && merge_import_envs(ws_->environments(), r.envs)) {
      if (ws_->active_env.empty()) ws_->active_env = r.envs.front().name;
      auto copy = std::make_shared<Workspace>(*ws_);
      std::thread([copy] { copy->save_settings(); }).detach();  // disk off the UI thread
      update_env_choice();
      env_changed();
    }
    if (!r.envs.empty() && std::none_of(r.envs.begin(), r.envs.end(), [&](const Environment& e) { return e.name == ws_->active_env; }))
      inactive_env = r.envs.front().name;
    if (!w.top.empty()) {
      for (auto& [dir, names] : w.order)
        if (!names.empty()) order_[dir] = std::move(names);
      if (auto it = order_.find(dest); it != order_.end()) it->second.push_back(w.top.filename().wstring());
      save_order();
      expanded_.insert(w.top);
      if (dest != root) expanded_.insert(dest);
      save_expanded();
      if (hist_full_) toggle_history_full();
      pending_select_ = w.top;
      rescan_then_select();
      // The keyboard goes to the imported folder, unless the user went on elsewhere meanwhile.
      if (wxWindow* f = wxWindow::FindFocus(); !f || IsDescendant(f)) {
        ++tree_focus_req_;
        tree_->SetFocus();
      }
    }
    std::string msg = import_summary(r, w);
    char t[64];
    std::snprintf(t, sizeof t, " in %.0f ms", ms);
    msg.insert(msg.size() - 1, t);
    for (size_t i = 0; i < w.skipped.size() && i < 8; ++i)
      msg += (i ? "\n" : "\n\nSkipped:\n") + std::string("\xE2\x80\xA2 ") + w.skipped[i];
    if (w.skipped.size() > 8) msg += "\n\xE2\x80\xA2 and " + std::to_string(w.skipped.size() - 8) + " more";
    for (size_t i = 0; i < r.notes.size() && i < 6; ++i)
      msg += (i ? "\n" : "\n\nNotes:\n") + std::string("\xE2\x80\xA2 ") + r.notes[i];
    if (w.vars)
      msg += "\n\n" + std::to_string(w.vars) + (w.vars == 1 ? " collection variable is" : " collection variables are") +
             " in the folder settings of " + u8(w.top.filename()) + " (they apply under any environment).";
    std::string undef_line;  // "{{trace}} is not defined" / "3 variables are not defined"
    if (!w.undefined.empty()) {
      std::string names;
      for (size_t i = 0; i < w.undefined.size() && i < 8; ++i) names += (i ? ", {{" : "{{") + w.undefined[i] + "}}";
      if (w.undefined.size() > 8) names += ", ...";
      undef_line = w.undefined.size() == 1 ? "{{" + w.undefined[0] + "}} is not defined"
                                           : std::to_string(w.undefined.size()) + " variables are not defined";
      msg += "\n\nNot defined anywhere (a request that uses one is not sent until it is): " + names +
             ".\nAdd them in the folder settings of " + u8(w.top.filename()) + " or in an environment.";
    }
    if (!r.envs.empty()) {
      msg += "\n\nEnvironments: ";
      for (size_t i = 0; i < r.envs.size(); ++i) msg += (i ? ", " : "") + r.envs[i].name;
      if (!inactive_env.empty())
        msg += "\nThe active environment is still " + (ws_->active_env.empty() ? std::string("none") : ws_->active_env) +
               ": choose " + inactive_env + " in the environment menu to use the imported one.";
    }
    if (w.skipped.empty() && r.notes.empty()) {
      // A clean import: the tree shows the result (selected, expanded); the summary is a
      // notice, not a dialog that blocks the next clicks.
      std::string line = import_summary(r, w);
      if (!line.empty() && line.back() == '.') line.pop_back();
      if (const size_t z = line.find(", 0 skipped"); z != std::string::npos) line.erase(z, 11);
      // One line in the header row: the format goes to the tooltip.
      if (const size_t z = line.rfind(" from "); z != std::string::npos) line.resize(z);
      std::snprintf(t, sizeof t, " \xC2\xB7 %.0f ms", ms);
      if (!inactive_env.empty()) {
        // Says so up front, and a click switches (the summary is in the tooltip).
        show_note(wxString::FromUTF8("\xE2\x9C\x93 Imported. Env " + inactive_env + " is not active: click to use it"),
                  wxString::FromUTF8(line + t + "\n\nClick to make " + inactive_env + " the active environment."), 15000,
                  [this, name = inactive_env] { env_chosen(name); });
        return;
      }
      if (!undef_line.empty()) {
        // The warning first (the header row ellipsizes the end), the count after it. It stays
        // until clicked or replaced (a send would stop at it); a click opens the variables.
        show_note(wxString::FromUTF8("\xE2\x9A\xA0 " + undef_line + ": click to define \xC2\xB7 " + line + t),
                  wxString::FromUTF8(msg + "\n\nClick to open the folder settings of " + u8(w.top.filename()) + "."), 0,
                  [this, top_dir = w.top] { open_folder_settings(top_dir); });
        return;
      }
      show_note(wxString::FromUTF8("\xE2\x9C\x93 " + line + t), wxString::FromUTF8(msg));
      return;
    }
    if (!inactive_env.empty()) {
      msg += "\n\nSwitch to " + inactive_env + " now?";
      if (wxMessageBox(wxString::FromUTF8(msg), "Import finished", wxYES_NO | wxICON_INFORMATION, top) == wxYES)
        env_chosen(inactive_env);
      return;
    }
    notify(top, "Import finished", wxString::FromUTF8(msg));
  }

  // `action`: what a click on the notice does (then it is dismissed); none: a click dismisses it.
  void show_note(const wxString& text, const wxString& tip, int ms = 8000, std::function<void()> action = {}) {
    if (!note_) return;
    note_action_ = std::move(action);
    // A tinted band in bold, green for done and amber for a warning: plain green text in place
    // of the COLLECTION label went unnoticed.
    const bool warn = text.StartsWith(wxString::FromUTF8("\xE2\x9A\xA0"));
    const bool dark = is_dark();
    note_->SetForegroundColour(warn ? (dark ? wxColour(0xF0, 0xC0, 0x60) : wxColour(0x8A, 0x5A, 0x00))
                                    : (dark ? wxColour(0x7E, 0xD8, 0x9B) : wxColour(0x1E, 0x7A, 0x3C)));
    note_->SetBackgroundColour(warn ? (dark ? wxColour(0x3A, 0x30, 0x18) : wxColour(0xFB, 0xF0, 0xD6))
                                    : (dark ? wxColour(0x1C, 0x36, 0x26) : wxColour(0xE2, 0xF4, 0xE7)));
    note_->SetLabel(" " + text);
    note_->SetToolTip(tip + (note_action_ ? "" : "\n\nClick to dismiss."));
    if (!note_->IsShown()) {
      // Same row, same height: only the header row is laid out again, the tree stays put.
      coll_label_->Hide();
      note_->Show();
      note_->GetContainingSizer()->Layout();
    }
    note_timer_.Stop();
    if (ms > 0) note_timer_.StartOnce(ms);  // 0: until clicked or replaced
  }

  void hide_note() {
    note_timer_.Stop();
    note_action_ = nullptr;
    if (!note_ || !note_->IsShown()) return;
    note_->Hide();
    coll_label_->Show();
    note_->GetContainingSizer()->Layout();
  }

  // A folder (or the whole workspace) as a Postman v2.1 collection file, or as curl commands
  // on the clipboard ({{vars}} resolved, one "# folder/name" line above each). Files are read
  // and written on a worker.
  void export_folder(const fs::path& folder, bool curl) {
    if (!ws_ || folder.empty()) return;
    wxWindow* top = wxGetTopLevelParent(this);
    const std::string name = u8(folder == ws_->root() ? ws_->root().filename() : folder.filename());
    fs::path out;
    if (!curl) {
      wxFileDialog dlg(top, "Export as a Postman collection", "", wxString::FromUTF8(name + ".postman_collection.json"),
                       "Postman collection v2.1 (*.json)|*.json", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
      if (dlg.ShowModal() != wxID_OK) return;
      out = fs::path(dlg.GetPath().ToStdWstring());
    }
    auto copy = std::make_shared<Workspace>(*ws_);
    auto order = std::make_shared<std::map<fs::path, std::vector<std::wstring>>>(order_);
    std::weak_ptr<int> alive = alive_;
    std::thread([this, copy, order, folder, name, out, curl, alive] {
      auto res = std::make_shared<ExportResult>(curl ? export_curl(*copy, folder, CurlShell::Bash, order.get())
                                                     : export_postman(*copy, folder, name, order.get()));
      std::string err;
      if (!curl) {
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        if (!(f << res->text) || !(f.flush())) err = "Could not write " + u8(out);
      }
      if (wxAppConsole* app = wxTheApp) app->CallAfter([this, res, err, out, curl, alive] {
        if (alive.expired()) return;
        wxWindow* top = wxGetTopLevelParent(this);
        if (!err.empty()) {
          notify(top, "Could not export", wxString::FromUTF8(err));
          return;
        }
        auto plural = [](size_t n, const char* one) { return std::to_string(n) + " " + one + (n == 1 ? "" : "s"); };
        std::string msg = (curl ? "Copied " + plural(res->requests, "curl command") : "Exported " + plural(res->requests, "request") + ", " + plural(res->folders, "folder") + " to " + u8(out)) +
                          (res->skipped.empty() ? "." : ", " + std::to_string(res->skipped.size()) + " skipped:");
        for (size_t i = 0; i < res->skipped.size() && i < 8; ++i) msg += "\n\xE2\x80\xA2 " + res->skipped[i];
        if (curl) copy_text(wxString::FromUTF8(res->text));
        if (!curl || !res->skipped.empty()) notify(top, curl ? "Copied as cURL" : "Exported", wxString::FromUTF8(msg));
      });
    }).detach();
  }

  // Copies a saved request as a curl (gRPC: grpcurl) command with {{vars}} resolved.
  // The request as it would be sent, as code in another language (on the clipboard).
  void copy_code(const fs::path& p, http_editor::CodeLang lang) {
    if (!ws_) return;
    std::string err;
    auto r = ws_->load_request(p, &err);
    if (!r) {
      notify(wxGetTopLevelParent(this), "Could not read the request", wxString::FromUTF8(err));
      return;
    }
    copy_text(wxString::FromUTF8(http_editor::to_code(ws_->expanded(*r), lang)));
    show_note(wxString::FromUTF8(std::string("\xE2\x9C\x93 Copied as ") + http_editor::code_lang_name(lang)),
              "The request as it would be sent, with {{variables}} resolved.", 4000);
  }

  // ---------------------------------------------------------------- copy / paste

  // Ctrl+C on a row: remembered, not put on the clipboard as text. The pasted copy gets a new
  // id, as Ctrl+D's does.
  void copy_item(const fs::path& p) {
    if (p.empty()) return;
    clip_ = p;
    show_note(wxString::FromUTF8("\xE2\x9C\x93 Copied \"") + clip_name() +
                  wxString::FromUTF8("\": Ctrl+V on a folder pastes it"),
              "Select a folder (or a request in it) and press Ctrl+V.", 5000);
  }

  wxString clip_name() {
    if (clip_.empty()) return {};
    if (const CollectionNode* n = find_node(tree_root_, clip_)) return wxString::FromUTF8(n->name);
    return clip_.filename().wstring();
  }

  bool clip_ok() const {
    std::error_code ec;
    return ws_ && !clip_.empty() && fs::exists(clip_, ec) && is_under(clip_, ws_->root());
  }

  void paste_into(const fs::path& folder) {
    if (!ws_ || !clip_ok()) return;
    if (!(folder == ws_->root() || is_under(folder, ws_->root()))) return;
    std::error_code ec;
    if (fs::is_directory(clip_, ec)) {
      duplicate_folder(clip_, folder);
      return;
    }
    std::string err;
    auto r = ws_->load_request(clip_, &err);
    if (!r) {
      notify(wxGetTopLevelParent(this), "Could not paste", wxString::FromUTF8(err));
      return;
    }
    // The same name where it is free (another folder), "<name> copy" next to the original.
    const bool same = folder == clip_.parent_path();
    std::string base = r->name;
    if (base.empty()) {
      base = u8(clip_.filename());
      if (base.size() > 9) base.resize(base.size() - 9);  // ".req.json"
    }
    std::string name;
    r->file = sidebar::free_request_file(folder, same ? base + " copy" : base, &name);
    r->name = name;
    r->id.clear();
    if (!ws_->save_request(*r, &err)) {
      notify(wxGetTopLevelParent(this), "Could not paste", wxString::FromUTF8(err));
      return;
    }
    if (name_cache_)
      name_cache_->note(r->file, r->name, r->kind == RequestKind::Grpc ? r->grpc_method : r->http.url, r->id);
    if (same) place_after(r->file, clip_);
    if (folder != ws_->root()) expanded_.insert(folder);
    pending_select_ = r->file;
    preview_after_scan_ = true;
    const std::string where = folder == ws_->root() ? std::string("the collection") : u8(folder.filename());
    show_note(wxString::FromUTF8("\xE2\x9C\x93 Pasted \"" + name + "\" into " + where),
              wxString::FromUTF8("A copy of " + u8(clip_.lexically_relative(ws_->root())) + " with a new id, saved as " +
                                 u8(r->file.lexically_relative(ws_->root())) + "."),
              4000);
    rescan_then_select();
  }

  // What a row is: its file, size, last change, and for a request its kind, URL and id.
  void show_info(const fs::path& p) {
    if (!ws_) return;
    const CollectionNode* n = find_node(tree_root_, p);
    std::error_code ec;
    std::string s;
    const bool dir = fs::is_directory(p, ec);
    s += std::string(dir ? "Folder" : "Request") + "\n";
    s += "File: " + u8(p.lexically_relative(ws_->root())) + "\n";
    if (!dir) {
      if (const auto sz = fs::file_size(p, ec); !ec) s += "Size: " + std::to_string(sz) + " bytes\n";
    } else {
      s += "Requests: " + std::to_string(requests_under(p).size()) + "\n";
    }
    if (const auto t = fs::last_write_time(p, ec); !ec) {
      const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
      const wxDateTime dt(static_cast<time_t>(std::chrono::system_clock::to_time_t(sys)));
      s += "Modified: " + dt.FormatISOCombined(' ').utf8_string() + "\n";
    }
    if (!dir)
      if (auto r = ws_->load_request(p)) {
        if (r->kind == RequestKind::Grpc) s += "gRPC: " + r->grpc_method + "\n";
        else s += "HTTP: " + r->http.method + " " + r->http.url + "\n";
        if (!r->id.empty()) s += "Id: " + r->id + "\n";
      }
    notify(wxGetTopLevelParent(this), wxString::FromUTF8(n ? n->name : u8(p.filename())), wxString::FromUTF8(s));
  }

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
  std::unique_ptr<WorkspaceWatcher> watcher_;  // changes made outside watt (watch_workspace)
  std::set<fs::path> disk_pending_;
  wxTimer disk_timer_;
  bool first_ws_ = true;
  bool ws_seen_ = false;                           // set_workspace had a workspace once
  std::string heal_log_;                           // what heal_stale_tabs did (self test)
  std::vector<wxWeakRef<RequestTab>> stale_tabs_;  // heal_stale_tabs()
  wxTimer heal_timer_;
  fs::path locked_root_;  // the workspace whose window.lock this window wrote
  bool restoring_width_ = false, width_restored_ = false;
  wxTimer width_timer_;
  std::unique_ptr<AuiSashRelease> sash_release_;
  uint64_t scan_gen_ = 0, hist_gen_ = 0, ws_gen_ = 0;
  bool scanning_ = false;

  CollectionNode tree_root_;  // last scan
  CollectionNode view_root_;  // what the tree shows (tree_root_ or its filtered copy)
  std::set<fs::path> expanded_;
  std::string filter_q_;
  std::string rebuilt_q_;  // the filter the tree's rows were last built for
  bool rebuilding_ = false, syncing_ = false, root_populated_ = false, sash_set_ = false;
  bool quick_open_shown_ = false;
  bool edit_ok_ = false;
  bool rebuild_after_edit_ = false;
  wxWindow* more_ = nullptr;  // the hover "..." button
  wxTreeItemId hover_item_;
  bool more_hot_ = false;
  unsigned tree_focus_req_ = 0;  // bumped when the keyboard moves to the tree on purpose
  bool reveal_active_ = false;  // the active tab's file was not in the tree yet
  bool sync_ok_ = false;  // the next rebuild() may update the rows in place (sync_tree)
  uint64_t rebuild_gen_ = 0;  // bumped by every rebuild (cancels expand_more)
  int expand_budget_ = 0;
  // Top folders whose rows are still to be updated (sync_tree), and the views their rows may
  // still point into.
  std::deque<SyncWork> sync_queue_;
  bool deferring_ = false;  // sync_children() queues the folders below instead of syncing them
  std::vector<CollectionNode> stale_views_;
  bool purging_ = false;            // purge_step() is deleting another workspace's rows
  bool rebuild_after_purge_ = false;
  bool unfilter_ = false;  // the current update closes the folders a cleared filter opened     // rows a filtered rebuild may still open folders for
  fs::path open_after_edit_;  // a new / duplicated request: opened when its rename ends
  struct HistMeta {
    std::string name, id;
    fs::path file;  // the file the entry was sent as (its folder's settings applied)
  };
  std::map<std::string, HistMeta> hist_names_;  // persisted history entry -> request name and id
  std::map<std::string, fs::path> path_by_id_;  // request id -> its file (saved requests only)
  // History snapshots (.watt/snapshots/<entry>.req.json): the request as an entry sent it,
  // opened like a saved file so the host reuses the preview tab and focuses a tab that already
  // shows it. -> the saved request it came from (empty when none).
  std::map<fs::path, fs::path> snapshot_origin_;
  // History snapshot file -> the entry it shows (when_iso, summary): the active snapshot tab's
  // row is the one highlighted in the history list.
  std::map<fs::path, std::pair<std::string, std::string>> snapshot_entry_;
  std::map<fs::path, TabSet> tab_sets_;  // hidden tabs of the workspaces switched away from
  wxTimer inh_timer_;            // see refresh_inherit_tabs()
  wxTimer preview_timer_;        // see select_file()
  mutable wxWeakRef<wxAuiNotebook> nb_cache_;
  fs::path clip_;                           // Ctrl+C (copy_item())
  std::map<fs::path, fs::file_time_type> own_writes_;  // see note_own_write()
  bool mute_page_events_ = false;           // see swap_tabs()
  wxTimer dormant_timer_;                   // see dormant_tick()
  std::set<wxWindow*> dormant_;             // hidden tabs frozen by dormant_tick()
  std::set<wxWindow*> dormant_bound_;       // tabs with the wake / destroy handlers
  fs::path preview_file_;
  std::set<std::pair<const void*, fs::path>> recoloured_;  // tabs whose editor preview_timer_ reloaded
  int inh_ticks_ = 0;
  fs::path pending_inherit_;     // a folder saved while a tab under it was sending
  bool pending_inherit_set_ = false;
  fs::path inh_saved_;           // the folder last saved from here, and when
  std::chrono::steady_clock::time_point inh_saved_at_{};
  wxWindow* tab_park_ = nullptr;         // their parent while hidden (see tab_park())
  wxButton* cookie_btn_ = nullptr;
  std::unique_ptr<std::pair<CollectionNode, MetaMap>> deferred_scan_;  // a scan pass held during an edit
  bool deferred_final_ = false;  // a label edit was asked for (F2, menu, new item)
  bool select_after_scan_ = false;
  bool preview_after_scan_ = false;
  bool picking_ = false;  // an import file dialog is open (pick_file_async)  // open pending_select_ in the preview tab too (a paste)
  fs::path drag_from_, pending_edit_, pending_select_;
  std::map<fs::path, std::vector<std::wstring>> order_;  // folder -> file names in the dragged order
  std::deque<fs::path> recent_files_;
  std::vector<std::string> env_names_;
  fs::path active_file_;
  std::shared_ptr<NameCache> name_cache_;
  MetaMap meta_;                                   // by path: request name and URL
  struct RankKey {
    std::string name, folder, rest;  // lower-case
    size_t order = 0;                // position in the unfiltered tree
  };
  std::unordered_map<const CollectionNode*, RankKey> rank_keys_;  // see build_rank_keys()
  fs::path best_path_;                             // the filter's best hit (rebuild)
  SearchKeys search_keys_;                         // filter text per node of tree_root_ (lazy)
  std::map<std::string, std::string> name_by_key_;  // "get {{base}}/x" -> request name ("" if ambiguous)
  std::map<std::string, fs::path> path_by_key_;     // same key -> its file (only when unique)
  std::multimap<std::string, std::pair<std::string, fs::path>> paths_by_key_;  // key -> (name, file), all
  uint64_t open_gen_ = 0;
  uint64_t resp_saves_ = 0;                        // responses kept this session (prune every 64)
  // This session's responses in full (shared with the tabs that got them), newest last, for
  // history restores: the disk copy keeps only a capped body. Bounded by count and body bytes.
  std::deque<std::pair<std::string, LastResponse>> session_resp_;
  size_t session_resp_bytes_ = 0;
  std::map<std::string, int> grpc_kinds_;          // "/pkg.Svc/M" -> GrpcCallKind
  std::shared_ptr<const GrpcSchema> last_learned_;  // the schema grpc_kinds_ last took in
  wxTimer hist_focus_timer_;                       // see keep_history_focus()
  fs::path hist_focus_snap_;
  int hist_focus_ticks_ = 0;
  wxTimer learn_timer_;                            // retries learn_grpc_kinds for learn_file_
  fs::path learn_file_;
  int learn_tries_ = 0;
  fs::path sticky_sel_;                            // a moved row: stays selected over rescans
  wxTreeItemId best_item_;                         // the filter's Enter target (bold)
  bool collapsed_ = false, user_sash_ = false;
  int expanded_width_ = 0;
  wxSize saved_min_ = wxDefaultSize;

  wxPanel* content_ = nullptr;
  wxPanel* rail_ = nullptr;
  wxPanel* edge_ = nullptr;
  wxButton* ws_btn_ = nullptr;
  wxButton* import_btn_ = nullptr;
  bool importing_ = false;
  wxButton* env_menu_btn_ = nullptr;
  wxButton* env_btn_ = nullptr;
  wxStaticText* env_warn_ = nullptr;
  wxStaticText* note_ = nullptr;  // show_note()
  std::function<void()> note_action_;
  wxTimer note_timer_;
  std::vector<sidebar::UnreadableEnv> env_bad_;  // environment files the workspace skipped
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
  CollectionTree* ctree_ = nullptr;  // tree_, for its drag insert mark
  wxStaticText* empty_ = nullptr;
  wxPanel* empty_box_ = nullptr;   // empty_ and the first-step buttons
  wxPanel* empty_btns_ = nullptr;
  HistoryList* history_ = nullptr;
  wxStaticText* hist_empty_ = nullptr;

  // Self test (WATT_SIDEBAR_SELFTEST).
  wxTimer st_timer_;
  StallProbe* st_probe_ = nullptr;
  std::vector<TestStep> st_steps_;
  size_t st_i_ = 0;
  double st_sync_ = 0;
  std::deque<std::function<void()>> later_q_;  // see later()
  int redraw_off_ = 0;
  int user_hist_h_ = 0;  // history pane height the user dragged (px), 0: fit to its rows
  bool redraw_sent_ = false;
  std::chrono::steady_clock::time_point st_t0_;
  std::string st_log_;
  fs::path st_out_;
};

}  // namespace

SidebarPanel* make_sidebar(wxWindow* parent, SidebarHost* host) { return new Sidebar(parent, host); }

}  // namespace watt::app
