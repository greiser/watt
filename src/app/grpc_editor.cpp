// gRPC request editor.
//
//   [◧] [Plaintext] [host:port ..................................] [Send] [grpcurl ▾]
//   +-------------------------+------------------------------------------------------+
//   | Schema: [Reflection v] ↻ | UNARY  Echo / Say ▾   EchoRequest -> EchoResponse  {}  |
//   | (proto files + imports) | Message   Metadata 1   Options 0.5 s                   |
//   | 7 methods · 3.1 ms      |  (client/bidi: message list + manual stream bar)       |
//   | [filter.............]   |  JSON editor (wxStyledTextCtrl, JSON lexer)            |
//   | ▾ watt.test.Echo      |                                                        |
//   |   UNARY  Say ...        |  ✓ JSON   notes                      Format  Template  |
//   +-------------------------+------------------------------------------------------+
//
// Nothing here waits on the network: schemas load through EditorHost::load_schema() (a worker
// thread; a running clock shows how long it has taken), and sends go through the shell.
//
// Large bodies stay responsive:
//  - texts of 256 KB and more (loading a request, switching messages, Format, a big paste) are
//    built into a Scintilla document on a worker (ILoader) and swapped in, so megabytes never
//    pass through the UI thread;
//  - above 1 MB, or with a line over 64 KB, the editor drops the JSON lexer and folding, so a
//    keystroke never re-lexes the document and Ctrl+End does not style a million lines; smaller
//    documents are lexed in idle chunks (SC_IDLESTYLING_ALL);
//  - JSON validation is debounced and skipped above 2 MB, brace matching is off in plain mode,
//    and nothing runs per keystroke beyond Scintilla's own work and changed().
#include <wx/app.h>
#include <wx/utils.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/clipbrd.h>
#include <wx/dialog.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/grid.h>
#include <wx/listbox.h>
#include <wx/menu.h>
#include <wx/panel.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/splitter.h>
#include <wx/stattext.h>
#include <wx/stc/stc.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/vlbox.h>
#include <wx/scrolwin.h>
#include <wx/wrapsizer.h>
#include <wx/wupdlock.h>
#include <wx/msgdlg.h>
#include <wx/msw/wrapwin.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "app/grpc_editor_tree.h"
#include "app/grpc_editor_util.h"
#include "app/grpc_editor_widgets.h"
#include "app/panels.h"
#include "core/timing.h"

namespace watt::app {
namespace {

using namespace grpced;

constexpr size_t kValidateMax = 2u << 20;      // JSON check on idle up to 2 MB
constexpr size_t kSplitMax = 8u << 20;         // a stream body above this stays one text
constexpr size_t kAsyncDoc = 256u << 10;       // texts this big are built into a document on a worker
constexpr size_t kAsyncPaste = 64u << 10;      // pastes this big too
constexpr size_t kBreakAt = 1000;              // minified JSON is shown with lines about this long
constexpr size_t kRememberMax = 1u << 20;      // per-method body memory (switching methods)
constexpr size_t kPlainBytes = 1u << 20;       // above this: no lexer, no folding
constexpr size_t kPlainLine = 64u << 10;       // ... or with a line longer than this
constexpr size_t kLexBytes = 768u << 10;       // back to the lexer below these (hysteresis)
constexpr size_t kLexLine = 48u << 10;
constexpr int kErrIndicator = 9;               // squiggle under a JSON syntax error
constexpr int kSourceReflection = 0;
constexpr int kSourceProto = 1;
constexpr int kMdDeleteCol = 2;

// Column labels that never "highlight" the current column: wxGrid's overlay selection paints the
// current column's label in a lightened selection colour, which leaves the light label text
// unreadable in the dark theme.
class FlatColHeaderRenderer : public wxGridColumnHeaderRendererDefault {
 public:
  void DrawHighlighted(const wxGrid& grid, wxDC& dc, wxRect& rect, int, int) const override {
    DrawBorder(grid, dc, rect);
  }
};

class FlatHeaderAttrProvider : public wxGridCellAttrProvider {
 public:
  const wxGridColumnHeaderRenderer& GetColumnHeaderRenderer(int) override { return renderer_; }

 private:
  FlatColHeaderRenderer renderer_;
};

// Metadata key cell: an empty key next to a value is drawn as a red "key required" note, so a
// header that will not be sent never disappears silently.
class MdKeyRenderer : public wxGridCellStringRenderer {
 public:
  explicit MdKeyRenderer(wxColour warn) : warn_(warn) {}
  void Draw(wxGrid& grid, wxGridCellAttr& attr, wxDC& dc, const wxRect& rect, int row, int col,
            bool sel) override {
    wxGridCellStringRenderer::Draw(grid, attr, dc, rect, row, col, sel);
    if (!grid.GetCellValue(row, col).Trim().Trim(false).IsEmpty() || grid.GetCellValue(row, 1).IsEmpty()) return;
    dc.SetFont(attr.GetFont());
    dc.SetTextForeground(warn_);
    dc.SetBackgroundMode(wxBRUSHSTYLE_TRANSPARENT);
    wxRect r = rect;
    r.Deflate(grid.FromDIP(4), 0);
    dc.DrawLabel(wxString::FromUTF8("\xE2\x9A\xA0 key required, not sent"), r, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);
  }
  wxGridCellRenderer* Clone() const override { return new MdKeyRenderer(warn_); }

 private:
  wxColour warn_;
};

// Scintilla's ILoader (include/ILoader.h, stable ABI): a document filled on any thread.
// CreateLoader() hands out the Document's ILoader; ConvertToDocument() returns the document for
// SetDocPointer() and keeps the reference the loader held.
class SciLoader {
 public:
  virtual int __stdcall Release() = 0;
  virtual int __stdcall AddData(const char* data, ptrdiff_t length) = 0;
  virtual void* __stdcall ConvertToDocument() = 0;

 protected:
  ~SciLoader() = default;
};

// A document kept for undo / redo across a whole-document swap (paste, format), with its view.
struct UndoDoc {
  void* doc = nullptr;
  bool plain = false;
  int caret = 0, anchor = 0, first_line = 0;
};

size_t longest_line(const char* p, size_t n, size_t stop_at) {
  size_t best = 0;
  const char* end = p + n;
  while (p < end) {
    const char* nl = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(end - p)));
    const size_t len = static_cast<size_t>((nl ? nl : end) - p);
    best = std::max(best, len);
    if (best > stop_at) return best;
    if (!nl) break;
    p = nl + 1;
  }
  return best;
}

bool wants_plain(const char* p, size_t n) { return n > kPlainBytes || longest_line(p, n, kPlainLine) > kPlainLine; }

std::string short_type(const std::string& t) {
  const size_t p = t.rfind('.');
  return p == std::string::npos ? t : t.substr(p + 1);
}

std::string human_size(size_t b) {
  char buf[32];
  if (b < 1024) std::snprintf(buf, sizeof buf, "%zu B", b);
  else if (b < (1u << 20)) std::snprintf(buf, sizeof buf, "%.1f KB", static_cast<double>(b) / 1024.0);
  else std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(b) / (1024.0 * 1024.0));
  return buf;
}

std::string elapsed_text(double ms) {
  char buf[32];
  if (ms < 10) std::snprintf(buf, sizeof buf, "%.2f ms", ms);
  else if (ms < 1000) std::snprintf(buf, sizeof buf, "%.1f ms", ms);
  else std::snprintf(buf, sizeof buf, "%.2f s", ms / 1000.0);
  return buf;
}

std::string lower_camel_to_snake(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c >= 'A' && c <= 'Z') {
      out += '_';
      out += static_cast<char>(c - 'A' + 'a');
    } else {
      out += c;
    }
  }
  return out;
}

std::filesystem::path fs_path(const std::string& utf8) {
  return std::filesystem::path(wxString::FromUTF8(utf8).ToStdWstring());
}

wxString file_label(const std::string& path) { return wxString(fs_path(path).filename().wstring()); }

wxString dir_label(const std::string& path) {
  const std::filesystem::path p = fs_path(path);
  const wxString name(p.filename().wstring()), parent(p.parent_path().filename().wstring());
  if (parent.empty()) return wxString::FromUTF8(path);
  return wxString::FromUTF8("\xE2\x80\xA6\\") + parent + "\\" + name;
}

// What a new tab starts with: the last target / schema source that loaded a schema, so trying
// several methods in several tabs needs no retyping (BloomRPC keeps its protos global too).
struct LastTarget {
  bool set = false;
  std::string address, authority;
  bool tls = false, skip_verify = false;
  std::vector<std::string> files, dirs;
  // The schema that loaded for `key` (GrpcEditor::schema_key()): a new tab on the same target
  // shows its methods at once instead of reflecting again.
  std::shared_ptr<const GrpcSchema> schema;
  std::string key;
  GrpcTarget target;  // the rest of the channel settings (certificates, timeouts)
};
LastTarget& last_target() {
  static LastTarget t;
  return t;
}

// A label that re-wraps to its width (hints that must never be cut off).
class WrapLabel : public wxStaticText {
 public:
  // wxST_NO_AUTORESIZE: the sizer alone sets the width. Without it SetLabel() resizes the control,
  // which re-triggers the wrap on every size event (an endless loop).
  WrapLabel(wxWindow* parent, const wxString& text)
      : wxStaticText(parent, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE), raw_(text) {
    SetMinSize(wxSize(FromDIP(60), GetBestSize().y));
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      const int w = GetClientSize().x;
      if (w == width_ || w <= FromDIP(40)) return;
      width_ = w;
      if (!queued_) {
        queued_ = true;
        CallAfter([this] {
          queued_ = false;
          rewrap();
        });
      }
    });
  }
  void set_text(const wxString& t) {
    if (t == raw_) return;
    raw_ = t;
    rewrap();
  }

 private:
  void rewrap() {
    const int w = width_ > 0 ? width_ : GetClientSize().x;
    SetLabel(raw_);
    if (w > FromDIP(40)) Wrap(w);
    // Height of the wrapped text (the width stays whatever the sizer gives).
    wxClientDC dc(this);
    dc.SetFont(GetFont());
    int tw = 0, th = 0;
    dc.GetMultiLineTextExtent(GetLabel(), &tw, &th);
    if (th != height_) {
      height_ = th;
      SetMinSize(wxSize(FromDIP(60), th));
      if (GetParent()) GetParent()->Layout();
    }
    Refresh();
  }
  wxString raw_;
  int width_ = -1;
  int height_ = -1;
  bool queued_ = false;
};

// The client-stream message list: owner-drawn so selection and dark mode look right (a native
// list box draws its selection white-on-white in dark mode).
class MessageList : public wxVListBox {
 public:
  MessageList(wxWindow* parent, const Theme& t, const wxSize& size)
      : wxVListBox(parent, wxID_ANY, wxDefaultPosition, size, wxBORDER_NONE), th_(t) {
    SetBackgroundColour(t.dark ? wxColour(37, 37, 38) : wxColour(246, 247, 249));
    SetSelectionBackground(t.dark ? wxColour(4, 57, 94) : wxColour(204, 228, 247));
  }
  unsigned GetCount() const { return static_cast<unsigned>(labels_.size()); }
  void Clear() {
    labels_.clear();
    SetItemCount(0);
  }
  void Append(const wxString& s) {
    labels_.push_back(s);
    SetItemCount(labels_.size());
  }
  wxString GetString(unsigned i) const { return labels_[i]; }
  void SetString(unsigned i, const wxString& s) {
    labels_[i] = s;
    RefreshRow(i);
  }

 protected:
  void OnDrawItem(wxDC& dc, const wxRect& rect, size_t n) const override {
    const wxString& l = labels_[n];
    const bool sent = l.StartsWith(wxString::FromUTF8("✓"));
    dc.SetFont(GetFont());
    dc.SetTextForeground(sent ? th_.ok : th_.fg);
    const int pad = FromDIP(6);
    dc.SetClippingRegion(rect);
    dc.DrawText(l, rect.x + pad, rect.y + (rect.height - dc.GetCharHeight()) / 2);
    dc.DestroyClippingRegion();
  }
  wxCoord OnMeasureItem(size_t) const override { return FromDIP(24); }

 private:
  Theme th_;
  std::vector<wxString> labels_;
};

// Documents built on workers and not yet swapped in, released if the editor goes away first.
struct DocJobs {
  std::mutex m;
  wxEvtHandler* target = nullptr;
  std::vector<void*> docs;
};

enum class DocKind { Load, Format, Paste };

class GrpcEditor : public wxPanel, public GrpcEditorPanel {
 public:
  GrpcEditor(wxWindow* parent, EditorHost* host)
      : wxPanel(parent, wxID_ANY),
        host_(host),
        th_(theme()),
        load_timer_(this, wxWindow::NewControlId()),
        json_timer_(this, wxWindow::NewControlId()),
        pump_timer_(this, wxWindow::NewControlId()),
        flash_timer_(this, wxWindow::NewControlId()) {
    jobs_->target = this;
    const auto t0 = Clock::now();
    Hide();  // children are created in a hidden window: no size / paint traffic per child
    build_top_bar();
    const auto t1 = Clock::now();
    split_ = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                  wxSP_LIVE_UPDATE | wxSP_THIN_SASH | wxSP_NOBORDER);
    split_->SetMinimumPaneSize(FromDIP(170));
    split_->SetSashGravity(0.0);
    build_schema_pane();
    const auto t2 = Clock::now();
    build_request_pane();
    const auto t3 = Clock::now();
    split_->SplitVertically(left_, right_, FromDIP(230));
    // Documents for the ILoader are created on a hidden control: CreateLoader() must not touch
    // the visible editor's view state.
    // The hidden control that builds documents for big texts is created on first use.

    auto* sz = new wxBoxSizer(wxVERTICAL);
    sz->Add(top_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    sz->Add(split_, 1, wxEXPAND | wxTOP, FromDIP(6));
    SetSizer(sz);

    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick_load(); }, load_timer_.GetId());
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { check_json(); }, json_timer_.GetId());
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { pump_sends(); }, pump_timer_.GetId());
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { grpcurl_->SetLabel("grpcurl"); }, flash_timer_.GetId());
    Bind(wxEVT_CHAR_HOOK, &GrpcEditor::on_char_hook, this);

    update_header();
    update_stream_ui();
    update_send_label();
    update_options_tab();
    show_schema_hint();
    Show();
    ctor_times_ = "ctor top " + elapsed_text(ms_between(t0, t1)) + ", schema pane " + elapsed_text(ms_between(t1, t2)) +
                  ", request pane " + elapsed_text(ms_between(t2, t3)) + ", total " + elapsed_text(ms_between(t0, Clock::now()));
    maybe_self_test();
    maybe_type_test();
  }

  ~GrpcEditor() override {
    *alive_ = 0;
    std::lock_guard lk(jobs_->m);
    jobs_->target = nullptr;
    for (UndoDoc* u : {&undo_doc_, &redo_doc_})
      if (u->doc) body_->ReleaseDocument(std::exchange(u->doc, nullptr));
    if (factory_)
      for (void* doc : jobs_->docs) factory_->ReleaseDocument(doc);
    jobs_->docs.clear();
  }

  wxWindow* window() override { return this; }

  // ------------------------------------------------------------------ contract

  void load(const SavedRequest& r) override {
    const auto t_load = Clock::now();
    struct Done {
      std::string& out;
      Clock::time_point t;
      ~Done() { out = "load " + elapsed_text(ms_between(t, Clock::now())); }
    } done{load_times_, t_load};
    loading_ = true;
    pane_auto_hidden_ = false;
    base_ = r;
    base_.grpc_body.clear();
    base_.grpc_body.shrink_to_fit();
    const LastTarget& lt = last_target();
    const bool fresh = r.grpc_target.address.empty() && r.proto_files.empty() && r.grpc_method.empty() &&
                       trim_empty(r.grpc_body) && lt.set;
    addr_->ChangeValue(wxString::FromUTF8(fresh ? lt.address : r.grpc_target.address));
    tls_->set_checked(fresh ? lt.tls : r.grpc_target.tls);
    update_tls_label();
    opt_t_ = fresh ? lt.target : r.grpc_target;
    web_->set_checked(opt_t_.web);
    if (fresh) {
      opt_t_.authority = lt.authority;
      opt_t_.insecure_skip_verify = lt.skip_verify;
    }
    opt_o_ = r.grpc_opts;
    for (Pem& pm : pem_) pm.from.clear();
    push_options();
    set_metadata(r.grpc_opts.metadata);

    files_full_ = fresh ? lt.files : r.proto_files;
    dirs_full_ = fresh ? lt.dirs : r.import_dirs;
    source_->SetSelection(files_full_.empty() ? kSourceReflection : kSourceProto);
    refresh_proto_lists();
    update_source_ui();

    method_ = r.grpc_method;
    body_by_method_.clear();
    last_template_.clear();
    keys_method_.clear();
    const GrpcMethodInfo* m = find_method(method_);
    if (m) stream_mode_ = client_streams(m->kind);
    else stream_mode_ = r.grpc_body.size() <= kSplitMax && r.grpc_body.find_first_not_of(" \t\r\n") != std::string::npos &&
                        r.grpc_body[r.grpc_body.find_first_not_of(" \t\r\n")] == '[';
    set_body(r.grpc_body);
    tree_->select(method_);
    update_web_ui();
    update_header();
    update_stream_ui();
    update_send_label();
    update_options_tab();
    // Never start a schema load from inside load(): it runs from the tab's constructor, before
    // the tab is fully built. A target whose schema already loaded this session reuses it (the
    // shared engine has it cached too); anything else loads on the next event-loop turn.
    if (lt.schema && lt.schema->ok && !lt.key.empty() && schema_key() == lt.key) {
      schema_key_ = lt.key;
      set_schema(lt.schema);  // still loading_: filling the template does not dirty the tab
      loading_ = false;
    } else {
      loading_ = false;
      CallAfter([this, alive = alive_] {
        if (*alive) maybe_load_schema(false);
      });
    }
  }

  void store_head(SavedRequest& r) const override {
    r.grpc_target.address = addr_->GetValue().utf8_string();
    r.grpc_target.tls = tls_->checked();
    r.grpc_target.web = web_->checked();
    r.grpc_method = method_;
  }

  void store(SavedRequest& r) const override {
    store_fields(r);
    r.grpc_body = compose_body();
  }

  void set_in_flight(bool f) override {
    in_flight_ = f;
    if (f && interactive()) {
      std::fill(sent_.begin(), sent_.end(), static_cast<char>(0));
      refresh_message_list();
    }
    if (!f) {
      pending_sends_.clear();
      pump_timer_.Stop();
      // The stream is over: an untouched draft that send_messages() queued up after the last
      // sent message (an exact copy of it) is dropped, so it does not end up in the request.
      if (stream_mode_ && messages_.size() >= 2 && sent_.size() == messages_.size()) {
        sync_current();
        const size_t n = messages_.size();
        if (!sent_[n - 1] && sent_[n - 2] && messages_[n - 1] == messages_[n - 2]) {
          messages_.pop_back();
          sent_.pop_back();
          if (cur_ >= static_cast<int>(n - 1)) select_message(static_cast<int>(n - 2), true);
          else refresh_message_list();
        }
      }
    }
    update_send_label();
    update_stream_ui();
  }

  void focus_url() override {
    addr_->SetFocus();
    addr_->SelectAll();
  }

  bool interactive() const override { return stream_mode_ && manual_ && manual_->GetValue(); }

  void set_schema(std::shared_ptr<const GrpcSchema> s) override {
    if (pending_loads_ > 0) --pending_loads_;
    if (pending_loads_ == 0) load_timer_.Stop();
    const double wall = ms_between(load_start_, Clock::now());
    reload_->Enable();
    if (!s->ok) {
      // Keep the last schema (greyed) only while it is still the same server / file set: methods
      // of another server would be easy to pick by mistake.
      const bool keep = schema_ && schema_->ok && loaded_origin_ == schema_origin();
      std::string hint;
      const bool reflect = source_->GetSelection() == kSourceReflection;
      if (reflect && tls_->checked() &&
          (s->error.find("wrong version number") != std::string::npos ||
           s->error.find("WRONG_VERSION_NUMBER") != std::string::npos || s->error.find("packet length too long") != std::string::npos))
        hint = "\nThe server answered the TLS handshake in plaintext: switch the address bar's TLS to Plaintext.";
      else if (reflect && !tls_->checked() &&
               (s->error.find("Socket closed") != std::string::npos ||
                s->error.find("Connection reset") != std::string::npos))
        hint = "\nIf this server uses TLS, switch Plaintext to TLS.";
      status_err_ = true;
      schema_status_->SetForegroundColour(th_.error);
      schema_status_->set_text(wxString::FromUTF8("Could not load schema (" + elapsed_text(wall) + ")\n" + s->error + hint +
                                                  (keep ? "\nShowing the last schema that loaded." : "")));
      schema_status_->SetToolTip(wxString::FromUTF8(s->error));
      if (keep) {
        tree_->set_stale(true);
      } else {
        schema_ = std::move(s);
        tree_->set_methods({});
        tree_->set_empty_text(source_->GetSelection() == kSourceReflection
                                  ? "Does the server have reflection enabled? You can also load .proto files instead."
                                  : "Check the files and import paths.");
      }
      left_->Layout();
      update_header();
      return;
    }
    schema_ = std::move(s);
    loaded_origin_ = schema_origin();
    status_err_ = false;
    tree_->set_stale(false);
    {
      LastTarget& lt = last_target();
      SavedRequest r;
      store_fields(r);
      lt.set = true;
      lt.address = r.grpc_target.address;
      lt.tls = r.grpc_target.tls;
      lt.authority = r.grpc_target.authority;
      lt.skip_verify = r.grpc_target.insecure_skip_verify;
      lt.files = r.proto_files;
      lt.dirs = r.import_dirs;
      lt.target = r.grpc_target;
      lt.schema = schema_;
      lt.key = schema_key_.empty() ? schema_key() : schema_key_;
    }
    keys_method_.clear();
    std::map<std::string, int> services;
    for (const auto& m : schema_->methods) ++services[m.service];
    schema_status_->SetForegroundColour(th_.muted);
    schema_status_->set_text(wxString::Format(L"%zu method%s in %zu service%s  ·  %s", schema_->methods.size(),
                                              schema_->methods.size() == 1 ? "" : "s", services.size(),
                                              services.size() == 1 ? "" : "s",
                                              wxString::FromUTF8(elapsed_text(schema_->load_ms))));
    schema_status_->SetToolTip(wxString::Format("Schema loaded in %s (%s including the hand-off to this window)",
                                                wxString::FromUTF8(elapsed_text(schema_->load_ms)),
                                                wxString::FromUTF8(elapsed_text(wall))));
    tree_->set_empty_text("The schema has no services.");
    tree_->set_methods(schema_->methods);
    tree_->select(method_);
    left_->Layout();
    if (const GrpcMethodInfo* m = find_method(method_)) {
      // A request that already has its method opens with the editor at full width.
      if (!pane_auto_hidden_) {
        pane_auto_hidden_ = true;
        hide_schema_pane();
      }
      apply_kind(m->kind);
      if (trim_empty(compose_body())) fill_template(*m);
      else show_notes(*m);
    }
    update_header();
    update_stream_ui();
    update_web_ui();
  }

 private:
  // ------------------------------------------------------------------ building

  void build_top_bar() {
    top_ = new wxPanel(this);
    schema_btn_ = new FlatButton(top_, "Schema", FlatButton::Style::Subtle);
    schema_btn_->set_sidebar_glyph(true);
    schema_btn_->SetToolTip("Show or hide the schema and method list");
    schema_btn_->set_checked(true);
    tls_ = new FlatButton(top_, "Plaintext", FlatButton::Style::Outline);
    tls_->set_width_labels({wxString::FromUTF8("\U0001F512 TLS"), "Plaintext"});
    tls_->SetToolTip("Use TLS for this connection (grpcs). Paste grpcs://host:port to set it automatically.");
    web_ = new FlatButton(top_, "gRPC-Web", FlatButton::Style::Outline);
    web_->SetToolTip("Call a gRPC-Web endpoint (Envoy, grpcwebproxy, Connect, tonic-web) over HTTP instead of a gRPC "
                     "channel. Unary and server streaming only; the address may carry a scheme and path prefix.");
    addr_ = new wxTextCtrl(top_, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    addr_->SetHint("host:port  or a grpcurl command  (Ctrl+L)");
    wxFont af = addr_->GetFont();
    af.SetFractionalPointSize(af.GetFractionalPointSize() * 1.1);
    addr_->SetFont(af);
    send_ = new FlatButton(top_, "Send", FlatButton::Style::Primary);
    send_->set_width_labels({"Send", "Cancel", "Open stream"});
    send_->SetToolTip("Send the request (Ctrl+Enter)");
    grpcurl_ = new FlatButton(top_, "grpcurl", FlatButton::Style::Subtle, true);
    grpcurl_->set_width_labels({wxString::FromUTF8("✓ Copied"), "grpcurl"});
    grpcurl_->SetToolTip("Copy as grpcurl, or import a grpcurl command");

    auto* bar = new wxBoxSizer(wxHORIZONTAL);
    const int h = std::max(addr_->GetBestSize().y, send_->GetBestSize().y);
    for (wxWindow* w : std::initializer_list<wxWindow*>{schema_btn_, tls_, web_, send_, grpcurl_})
      w->SetMinSize(wxSize(w->GetBestSize().x, h));
    bar->Add(schema_btn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    bar->Add(tls_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    bar->Add(web_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    bar->Add(addr_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    bar->Add(send_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    bar->Add(grpcurl_, 0, wxALIGN_CENTER_VERTICAL);
    top_->SetSizer(bar);

    schema_btn_->on_click = [this] { toggle_schema_pane(); };
    tls_->on_click = [this] {
      tls_->set_checked(!tls_->checked());
      update_tls_label();
      update_options_tab();
      changed();
      maybe_load_schema(false);
    };
    web_->on_click = [this] {
      web_->set_checked(!web_->checked());
      update_web_ui();
      update_options_tab();
      changed();
      maybe_load_schema(false);
    };
    addr_->Bind(wxEVT_TEXT, [this](wxCommandEvent& e) {
      e.Skip();
      if (loading_) return;
      std::string a = addr_->GetValue().utf8_string();
      if (looks_like_grpcurl(a)) {
        import_grpcurl(a);
        return;
      }
      // A pasted grpcs://host:443/ becomes host:443 with TLS on.
      if (a.find("://") != std::string::npos) {
        bool tls = tls_->checked();
        if (strip_scheme(a, &tls)) {
          loading_ = true;
          addr_->ChangeValue(wxString::FromUTF8(a));
          addr_->SetInsertionPointEnd();
          tls_->set_checked(tls);
          update_tls_label();
          loading_ = false;
        }
      }
      changed();
    });
    // A multi-line grpcurl command would lose its continuation lines in a single-line field.
    addr_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
      const bool paste = (e.GetKeyCode() == 'V' && e.GetModifiers() == wxMOD_CONTROL) ||
                         (e.GetKeyCode() == WXK_INSERT && e.GetModifiers() == wxMOD_SHIFT);
      if (paste) {
        const std::string clip = clipboard_text(64u << 10);
        if (looks_like_grpcurl(clip)) {
          import_grpcurl(clip);
          return;
        }
      }
      e.Skip();
    });
    addr_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) {
      if (!method_.empty() && schema_key() == schema_key_ && schema_ && schema_->ok) host_->send();
      else maybe_load_schema(true);
    });
    addr_->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) {
      e.Skip();
      std::string a = addr_->GetValue().utf8_string();
      bool tls = tls_->checked();
      if (strip_scheme(a, &tls)) {
        addr_->ChangeValue(wxString::FromUTF8(a));
        tls_->set_checked(tls);
        update_tls_label();
        changed();
      }
      if (a.empty()) return;
      SavedRequest r;
      store_fields(r);
      host_->preconnect(r.grpc_target);
      CallAfter([this] { maybe_load_schema(false); });
    });
    send_->on_click = [this] { in_flight_ ? host_->cancel() : host_->send(); };
    grpcurl_->on_click = [this] { show_grpcurl_menu(); };
  }

  void ensure_proto_box() {
    if (files_) return;
    auto* psz = new wxBoxSizer(wxVERTICAL);
    files_ = new wxListBox(proto_box_, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(62)), 0, nullptr,
                           wxLB_EXTENDED | wxBORDER_NONE);
    dirs_ = new wxListBox(proto_box_, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(46)), 0, nullptr,
                          wxLB_EXTENDED | wxBORDER_NONE);
    for (wxListBox* l : {files_, dirs_}) {
      l->SetBackgroundColour(th_.dark ? wxColour(37, 37, 38) : wxColour(246, 247, 249));
      l->SetForegroundColour(th_.fg);
    }
    auto list_row = [&](const wxString& title, wxListBox* list, std::vector<std::string>* full, const wxString& add_tip,
                        std::function<void()> add) {
      auto* row = new wxBoxSizer(wxHORIZONTAL);
      auto* t = new wxStaticText(proto_box_, wxID_ANY, title);
      t->SetForegroundColour(th_.muted);
      auto* addb = new FlatButton(proto_box_, "Add", FlatButton::Style::Subtle);
      addb->SetToolTip(add_tip);
      auto* delb = new FlatButton(proto_box_, wxString::FromUTF8("−"), FlatButton::Style::Subtle);
      delb->SetToolTip("Remove the selected entries");
      row->Add(t, 1, wxALIGN_CENTER_VERTICAL);
      row->Add(addb, 0, wxRIGHT, FromDIP(2));
      row->Add(delb, 0);
      psz->Add(row, 0, wxEXPAND | wxTOP, FromDIP(4));
      psz->Add(list, 0, wxEXPAND | wxTOP, FromDIP(2));
      addb->on_click = add;
      delb->on_click = [this, list, full] {
        wxArrayInt sel;
        list->GetSelections(sel);
        for (size_t i = sel.size(); i-- > 0;)
          if (static_cast<size_t>(sel[i]) < full->size()) full->erase(full->begin() + sel[i]);
        refresh_proto_lists();
        changed();
        maybe_load_schema(false);
      };
      list->Bind(wxEVT_MOTION, [list, full](wxMouseEvent& e) {
        e.Skip();
        const int i = list->HitTest(e.GetPosition());
        const wxString tip = i >= 0 && static_cast<size_t>(i) < full->size() ? wxString::FromUTF8((*full)[static_cast<size_t>(i)]) : wxString();
        if (list->GetToolTipText() != tip) list->SetToolTip(tip);
      });
    };
    list_row("Files", files_, &files_full_, "Add .proto or .protoset files", [this] { add_files(); });
    list_row("Import paths", dirs_, &dirs_full_, "Add a directory that imports resolve against", [this] { add_dir(); });
    proto_box_->SetSizer(psz);
  }

  void build_schema_pane() {
    left_ = new wxPanel(split_);
    auto* lsz = new wxBoxSizer(wxVERTICAL);

    auto* head = new wxBoxSizer(wxHORIZONTAL);
    auto* lbl = new wxStaticText(left_, wxID_ANY, "Schema");
    lbl->SetFont(lbl->GetFont().Bold());
    source_ = new ChoiceButton(left_, {"Server reflection", ".proto files"});
    source_->SetSelection(kSourceReflection);
    source_->SetToolTip("Where the service definitions come from. Remembered with the request.");
    reload_ = new FlatButton(left_, wxString::FromUTF8("↻"), FlatButton::Style::Subtle);
    reload_->SetToolTip("Reload the schema (Enter in the address field)");
    head->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    head->Add(source_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    head->Add(reload_, 0, wxEXPAND);
    lsz->Add(head, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));

    // The .proto file lists are built when that source is first chosen (ensure_proto_box()).
    proto_box_ = new wxPanel(left_);
    proto_box_->Hide();
    lsz->Add(proto_box_, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));

    schema_status_ = new WrapLabel(left_, "");
    schema_status_->SetForegroundColour(th_.muted);
    lsz->Add(schema_status_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));

    tree_ = new MethodTree(left_);
    tree_->on_pick = [this](const std::string& p, bool chosen) {
      pick_method(p);
      // Done picking (Enter / double-click): give the message editor the room, BloomRPC's
      // width. The header's method menu, Ctrl+F and the Schema button bring the list back.
      if (chosen && find_method(method_)) {
        hide_schema_pane();
        focus_body();
      }
    };
    lsz->Add(tree_, 1, wxEXPAND | wxALL, FromDIP(8));
    left_->SetSizer(lsz);

    source_->on_change = [this] {
      update_source_ui();
      changed();
      maybe_load_schema(false);
    };
    reload_->on_click = [this] { maybe_load_schema(true); };
  }

  void build_request_pane() {
    right_ = new wxPanel(split_);
    auto* rsz = new wxBoxSizer(wxVERTICAL);

    // header: badge + method picker + types + definition
    auto* head = new wxBoxSizer(wxHORIZONTAL);
    badge_ = new wxStaticText(right_, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
    wxFont small = badge_->GetFont().Bold();
    small.SetFractionalPointSize(small.GetFractionalPointSize() * 0.85);
    badge_->SetFont(small);
    badge_->SetForegroundColour(*wxWHITE);
    method_btn_ = new FlatButton(right_, "", FlatButton::Style::Subtle, true);
    wxFont mf = method_btn_->GetFont().Bold();
    mf.SetFractionalPointSize(mf.GetFractionalPointSize() * 1.15);
    method_btn_->SetFont(mf);
    method_btn_->SetToolTip("Pick a method (Ctrl+F filters the list on the left)");
    io_lbl_ = new wxStaticText(right_, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    io_lbl_->SetForegroundColour(th_.muted);
    io_lbl_->SetMinSize(wxSize(FromDIP(40), -1));
    def_btn_ = new FlatButton(right_, "View proto", FlatButton::Style::Outline);
    def_btn_->SetToolTip("Show the .proto definition of the method and its messages");
    head->Add(badge_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    head->Add(method_btn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    head->Add(io_lbl_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    head->Add(def_btn_, 0, wxALIGN_CENTER_VERTICAL);
    rsz->Add(head, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));

    tabs_ = new TabStrip(right_);
    tabs_->set_tabs({"Message", "Metadata", "Options"});
    book_ = new wxSimplebook(right_, wxID_ANY);
    book_->AddPage(build_message_page(book_), "Message", true);
    md_holder_ = new wxPanel(book_);
    md_holder_->SetSizer(new wxBoxSizer(wxVERTICAL));
    opt_holder_ = new wxPanel(book_);
    opt_holder_->SetSizer(new wxBoxSizer(wxVERTICAL));
    book_->AddPage(md_holder_, "Metadata");
    book_->AddPage(opt_holder_, "Options");
    rsz->Add(tabs_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(6));
    rsz->Add(book_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(6));
    right_->SetSizer(rsz);

    tabs_->on_select = [this](size_t i) { show_page(i); };
    method_btn_->on_click = [this] { show_method_menu(); };
    def_btn_->on_click = [this] { show_definition(); };
  }

  // Builds the client / bidi stream controls the first time a streaming method needs them.
  void ensure_stream_ui() {
    if (stream_bar_) return;
    wxWindow* page = msg_page_;
    wxWindowUpdateLocker lock(page);
    // client / bidi stream bar
    stream_bar_ = new wxPanel(page);
    auto* sb = new wxBoxSizer(wxHORIZONTAL);
    manual_ = new CheckBox(stream_bar_, "Manual");
    manual_->SetToolTip("Manual stream: open the stream and send messages one at a time while responses arrive.\n"
                        "Off: Send streams every message in the list, then closes the send side.");
    stream_hint_ = new wxStaticText(stream_bar_, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    stream_hint_->SetForegroundColour(th_.muted);
    stream_hint_->SetMinSize(wxSize(FromDIP(30), -1));
    send_one_ = new FlatButton(stream_bar_, "Send #1", FlatButton::Style::Outline);
    send_one_->set_width_labels({"Send #00", "Resend #00"});
    send_one_->SetToolTip("Send the selected message on the open stream (Ctrl+Shift+Enter); opens the stream if needed");
    send_all_ = new FlatButton(stream_bar_, "Send all", FlatButton::Style::Outline);
    send_all_->SetToolTip("Send every message not sent yet, in order");
    end_ = new FlatButton(stream_bar_, "End", FlatButton::Style::Outline);
    end_->SetToolTip("End stream (half-close): tell the server no more messages follow");
    sb->Add(manual_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    sb->Add(stream_hint_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    sb->Add(send_one_, 0, wxRIGHT, FromDIP(4));
    sb->Add(send_all_, 0, wxRIGHT, FromDIP(4));
    sb->Add(end_, 0);
    stream_bar_->SetSizer(sb);
    page_sz_->Insert(0, stream_bar_, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(6));

    msg_col_ = new wxPanel(page);
    auto* mc = new wxBoxSizer(wxVERTICAL);
    msgs_ = new MessageList(msg_col_, th_, wxSize(FromDIP(180), -1));
    auto* mb = new wxBoxSizer(wxHORIZONTAL);
    auto mk = [&](const char* utf8, const char* tip) {
      auto* b = new FlatButton(msg_col_, wxString::FromUTF8(utf8), FlatButton::Style::Subtle);
      b->SetMinSize(wxSize(FromDIP(18), -1));
      b->SetToolTip(tip);
      mb->Add(b, 1, 0);
      return b;
    };
    add_msg_ = mk("+", "Add a message (a copy of the template)");
    dup_msg_ = mk("⧉", "Duplicate the selected message");
    del_msg_ = mk("−", "Remove the selected message");
    up_msg_ = mk("↑", "Move up");
    down_msg_ = mk("↓", "Move down");
    mc->Add(msgs_, 1, wxEXPAND);
    mc->Add(mb, 0, wxEXPAND | wxTOP, FromDIP(2));
    msg_col_->SetSizer(mc);
    mid_sz_->Insert(0, msg_col_, 0, wxEXPAND | wxBOTTOM, FromDIP(4));

    manual_->on_change = [this] {
      update_stream_ui();
      update_send_label();
    };
    send_one_->on_click = [this] { send_messages(false); };
    send_all_->on_click = [this] { send_messages(true); };
    end_->on_click = [this] { host_->stream_close(); };
    msgs_->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { select_message(msgs_->GetSelection()); });
    add_msg_->on_click = [this] {
      sync_current();
      std::string t = template_for(method_);
      messages_.push_back(t.empty() ? "{}" : t);
      sent_.push_back(0);
      select_message(static_cast<int>(messages_.size()) - 1, true);
      changed();
    };
    dup_msg_->on_click = [this] {
      sync_current();
      messages_.insert(messages_.begin() + cur_ + 1, messages_[static_cast<size_t>(cur_)]);
      sent_.insert(sent_.begin() + cur_ + 1, 0);
      select_message(cur_ + 1, true);
      changed();
    };
    del_msg_->on_click = [this] {
      if (messages_.size() <= 1) {
        set_editor_text("");
        messages_[0].clear();
        refresh_message_list();
      } else {
        messages_.erase(messages_.begin() + cur_);
        sent_.erase(sent_.begin() + cur_);
        const int next = std::min<int>(cur_, static_cast<int>(messages_.size()) - 1);
        cur_ = -1;
        select_message(next, true);
      }
      changed();
    };
    auto move = [this](int d) {
      sync_current();
      const int to = cur_ + d;
      if (to < 0 || to >= static_cast<int>(messages_.size())) return;
      std::swap(messages_[static_cast<size_t>(cur_)], messages_[static_cast<size_t>(to)]);
      std::swap(sent_[static_cast<size_t>(cur_)], sent_[static_cast<size_t>(to)]);
      cur_ = to;
      refresh_message_list();
      changed();
    };
    up_msg_->on_click = [move] { move(-1); };
    down_msg_->on_click = [move] { move(1); };
    page->Layout();
  }

  wxWindow* build_message_page(wxWindow* parent) {
    auto* page = new wxPanel(parent);
    auto* sz = new wxBoxSizer(wxVERTICAL);

    // The stream bar and the message list (client / bidi streams) are built when first needed:
    // ensure_stream_ui() puts them into these sizers.
    page_sz_ = sz;
    auto* mid = new wxBoxSizer(wxHORIZONTAL);
    mid_sz_ = mid;
    msg_page_ = page;

    body_ = new wxStyledTextCtrl(page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    setup_json_editor(body_, th_);
    body_->IndicatorSetStyle(kErrIndicator, wxSTC_INDIC_SQUIGGLE);
    body_->IndicatorSetForeground(kErrIndicator, th_.error);
    body_->IndicatorSetUnder(kErrIndicator, true);
    body_->SetModEventMask(wxSTC_MOD_INSERTTEXT | wxSTC_MOD_DELETETEXT | wxSTC_PERFORMED_USER |
                           wxSTC_PERFORMED_UNDO | wxSTC_PERFORMED_REDO);
    mid->Add(body_, 1, wxEXPAND | wxLEFT, FromDIP(4));
    sz->Add(mid, 1, wxEXPAND | wxTOP, FromDIP(4));

    auto* foot = new wxBoxSizer(wxHORIZONTAL);
    json_status_ = new wxStaticText(page, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    json_status_->SetMinSize(wxSize(FromDIP(40), -1));
    json_status_->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
      if (error_at_ < 0) return;
      focus_body();
      body_->GotoPos(error_at_);
      body_->EnsureCaretVisible();
    });
    notes_ = new wxStaticText(page, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    notes_->SetForegroundColour(th_.muted);
    notes_->SetMinSize(wxSize(FromDIP(20), -1));
    format_ = new FlatButton(page, "Format", FlatButton::Style::Subtle);
    format_->SetToolTip("Pretty-print the JSON (Shift+Alt+F)");
    template_ = new FlatButton(page, "Template", FlatButton::Style::Subtle);
    template_->SetToolTip("Replace the message with an example for the method's request type");
    foot->Add(json_status_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    foot->Add(notes_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    foot->Add(format_, 0, wxRIGHT, FromDIP(2));
    foot->Add(template_, 0);
    sz->Add(foot, 0, wxEXPAND | wxTOP, FromDIP(4));
    page->SetSizer(sz);
    page->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      const int w = e.GetSize().x;
      if (w == foot_w_) return;
      foot_w_ = w;
      CallAfter([this] { fit_status(); });
    });

    body_->Bind(wxEVT_STC_CHANGE, [this](wxStyledTextEvent& e) {
      e.Skip();
      // Programmatic changes run with loading_ set. (Not GetModify(): an undo back to the save
      // point is an edit too, and must re-validate.)
      if (loading_) return;
      ++body_gen_;
      if (stream_mode_ && cur_ >= 0 && static_cast<size_t>(cur_) < sent_.size() && sent_[static_cast<size_t>(cur_)])
        edit_sent_message();
      json_timer_.StartOnce(300);
      changed();
    });
    body_->Bind(wxEVT_STC_MODIFIED, [this](wxStyledTextEvent& e) {
      e.Skip();
      if (e.GetLinesAdded() != 0) margin_dirty_ = true;
      // A new edit after going back past a paste / format: the redo document is gone, as
      // Scintilla drops its own redo history.
      if (!loading_ && redo_doc_.doc && (e.GetModificationType() & wxSTC_PERFORMED_USER) &&
          (e.GetModificationType() & (wxSTC_MOD_INSERTTEXT | wxSTC_MOD_DELETETEXT)))
        drop_doc(redo_doc_);
      // "Replaced your text with the example" is about that replacement only: the user's next
      // edit makes it stale, the method's own note comes back.
      if (!loading_ && replaced_note_ && (e.GetModificationType() & wxSTC_PERFORMED_USER) &&
          (e.GetModificationType() & (wxSTC_MOD_INSERTTEXT | wxSTC_MOD_DELETETEXT))) {
        replaced_note_ = false;
        notes_->SetLabel(replaced_before_);
      }
      if (plain_ || loading_) return;
      // A big insert (a context-menu paste, drag and drop): drop the lexer before Scintilla
      // styles the new text for the caret line.
      const size_t len = static_cast<size_t>(body_->GetLength());
      const size_t ins = (e.GetModificationType() & wxSTC_MOD_INSERTTEXT) ? static_cast<size_t>(e.GetLength()) : 0;
      if (len > kPlainBytes ||
          (ins > kPlainLine && ins / (static_cast<size_t>(std::abs(e.GetLinesAdded())) + 1) > kPlainLine))
        set_plain(true);
    });
    body_->Bind(wxEVT_STC_UPDATEUI, [this](wxStyledTextEvent& e) {
      e.Skip();
      if (margin_dirty_) {
        margin_dirty_ = false;
        fit_line_margin(body_);
      }
      fit_vscroll();
      if (!plain_) match_braces();
    });
    body_->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      CallAfter([this] { fit_vscroll(); });
    });
    body_->Bind(wxEVT_STC_CHARADDED, [this](wxStyledTextEvent& e) {
      e.Skip();
      const int k = e.GetKey();
      if (k == '\n') auto_indent();
      if (plain_) return;
      if (body_->CallTipActive()) body_->CallTipCancel();
      if (k == '{' || k == '[' || k == '"') auto_close(k);
      if (k == '"' && at_key_position(body_->GetCurrentPos() - 1)) show_completion(false);
    });
    // Typing a closer right before the same closer steps over it (the pair auto_close()
    // inserted), as Ace / VS Code do. No modifier check: '"' and '}' are Shift+' and Shift+]
    // on a US keyboard (Shift+2 / AltGr+0 on a German one). A closer typed while the key list
    // is open ends the key: the list closes first, so it never takes or completes the key.
    body_->Bind(wxEVT_CHAR, [this](wxKeyEvent& e) {
      const int k = static_cast<int>(e.GetUnicodeKey());
      if (!plain_ && (k == '}' || k == ']' || k == '"')) {
        if (body_->AutoCompActive()) body_->AutoCompCancel();
      }
      if (!plain_ && (k == '}' || k == ']' || k == '"') && body_->GetSelectionStart() == body_->GetSelectionEnd()) {
        const int pos = body_->GetCurrentPos();
        if (body_->GetCharAt(pos) == k) {
          const int in = string_state(pos);
          if (in >= 0 && ((k == '"') == (in == 1))) {
            body_->GotoPos(pos + 1);
            return;
          }
        }
      }
      e.Skip();
    });
    body_->Bind(wxEVT_STC_AUTOCOMP_COMPLETED, [this](wxStyledTextEvent& e) {
      e.Skip();
      CallAfter([this, word = e.GetText().utf8_string()] { after_completion(word); });
    });
    body_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
      const int k = e.GetKeyCode();
      const int mods = e.GetModifiers();
      if ((k == 'V' && mods == wxMOD_CONTROL) || (k == WXK_INSERT && mods == wxMOD_SHIFT)) {
        if (doc_pending_ || try_big_paste()) return;
      }
      if (k == 'Z' && mods == wxMOD_CONTROL && swap_history(true)) return;
      if (((k == 'Y' && mods == wxMOD_CONTROL) || (k == 'Z' && mods == (wxMOD_CONTROL | wxMOD_SHIFT))) &&
          swap_history(false))
        return;
      if (k == WXK_SPACE && mods == wxMOD_CONTROL) {
        show_completion(true);
        return;
      }
      // Backspace inside an empty pair ({|}, [|], "|") removes both halves.
      if (k == WXK_BACK && mods == wxMOD_NONE && !plain_ && !body_->AutoCompActive() &&
          body_->GetSelectionStart() == body_->GetSelectionEnd()) {
        const int pos = body_->GetCurrentPos();
        const int a = pos > 0 ? body_->GetCharAt(pos - 1) : 0, b = body_->GetCharAt(pos);
        if (((a == '{' && b == '}') || (a == '[' && b == ']') || (a == '"' && b == '"')) &&
            string_state(pos - 1) == 0) {
          body_->SetTargetRange(pos - 1, pos + 1);
          body_->ReplaceTarget("");
          return;
        }
      }
      e.Skip();
    });
    format_->on_click = [this] { format_body(); };
    template_->on_click = [this] {
      if (const GrpcMethodInfo* m = find_method(method_)) {
        if (stream_mode_) {
          const std::string t = template_for(method_);
          set_editor_text(t);
          messages_[static_cast<size_t>(cur_)] = t;
          refresh_message_list();
          changed();
        } else {
          fill_template(*m);
          changed();
        }
      }
    };
    return page;
  }

  wxWindow* build_metadata_page(wxWindow* parent) {
    auto* page = new wxPanel(parent);
    auto* sz = new wxBoxSizer(wxVERTICAL);
    md_ = new wxGrid(page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    md_->CreateGrid(1, 3);
    md_->GetTable()->SetAttrProvider(new FlatHeaderAttrProvider);
    md_->SetRowLabelSize(0);
    md_->SetColLabelValue(0, "Key");
    md_->SetColLabelValue(1, "Value");
    md_->SetColLabelValue(kMdDeleteCol, "");
    md_->SetColLabelAlignment(wxALIGN_LEFT, wxALIGN_CENTRE);
    md_->SetColLabelSize(FromDIP(26));
    md_->SetDefaultRowSize(FromDIP(26), true);
    md_->SetDefaultCellBackgroundColour(th_.bg);
    md_->SetDefaultCellTextColour(th_.fg);
    md_->SetGridLineColour(th_.border);
    md_->SetLabelBackgroundColour(th_.dark ? wxColour(37, 37, 38) : wxColour(246, 246, 246));
    md_->SetLabelTextColour(th_.fg);
    // The column label of the current cell is drawn on a lightened selection colour: keep it a
    // neutral grey so the label text stays readable.
    md_->SetSelectionBackground(th_.dark ? wxColour(52, 52, 56) : wxColour(214, 222, 232));
    md_->SetSelectionForeground(th_.fg);
    md_->SetCellHighlightColour(th_.accent);
    md_->DisableDragRowSize();
    md_->EnableEditing(true);
    // Tab / Enter are handled in on_md_key(): they walk Key -> Value -> next row's Key.
    md_->SetTabBehaviour(wxGrid::Tab_Leave);
    {
      auto* attr = new wxGridCellAttr;
      attr->SetReadOnly(true);
      attr->SetAlignment(wxALIGN_CENTRE, wxALIGN_CENTRE);
      attr->SetTextColour(th_.muted);
      md_->SetColAttr(kMdDeleteCol, attr);
      auto* key = new wxGridCellAttr;
      key->SetRenderer(new MdKeyRenderer(th_.error));
      md_->SetColAttr(0, key);
    }
    md_hint_ = new WrapLabel(page, md_hint_text());
    md_hint_->SetForegroundColour(th_.muted);
    sz->Add(md_, 1, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(6));
    sz->Add(md_hint_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(6));
    page->SetSizer(sz);

    md_->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      const int w = md_->GetClientSize().x;
      if (w <= 0) return;
      const int del = FromDIP(28);
      md_->SetColSize(0, std::max(FromDIP(100), (w - del) * 35 / 100));
      md_->SetColSize(1, std::max(FromDIP(100), w - del - md_->GetColSize(0)));
      md_->SetColSize(kMdDeleteCol, del);
    });
    md_->Bind(wxEVT_GRID_CELL_CHANGED, [this](wxGridEvent& e) {
      e.Skip();
      ensure_blank_row();
      update_md_tab();
      changed();
    });
    // The key handler sees keys from the grid window and from the open cell editor (both are
    // forwarded to the grid's handler; dynamic handlers run before wxGrid's own OnKeyDown).
    md_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) { on_md_key(e); });
    md_->Bind(wxEVT_GRID_TABBING, [this](wxGridEvent& e) { md_move(!e.ShiftDown()); });
    // The x column is mouse only: the cursor never lands there (arrows, Home/End...).
    md_->Bind(wxEVT_GRID_SELECT_CELL, [this](wxGridEvent& e) {
      if (e.GetCol() == kMdDeleteCol) {
        e.Veto();
        return;
      }
      e.Skip();
    });
    md_->Bind(wxEVT_GRID_CELL_LEFT_CLICK, [this](wxGridEvent& e) {
      if (e.GetCol() == kMdDeleteCol && e.GetRow() < md_->GetNumberRows() - 1) {
        delete_md_row(e.GetRow());
        return;
      }
      e.Skip();
    });
    md_->Bind(wxEVT_GRID_CELL_RIGHT_CLICK, [this](wxGridEvent& e) {
      const int row = e.GetRow();
      if (row < 0 || row >= md_->GetNumberRows() - 1) return;
      wxMenu menu;
      menu.Append(1, "Delete row");
      if (md_->GetPopupMenuSelectionFromUser(menu) == 1) delete_md_row(row);
    });
    return page;
  }

  // Model <-> Options page widgets (no-ops until the page is built).
  void pull_options() {
    if (!opt_page_built_) return;
    GrpcTarget& t = opt_t_;
    t.authority = authority_->GetValue().utf8_string();
    t.insecure_skip_verify = skip_verify_->GetValue();
    t.server_name = server_name_->GetValue().utf8_string();
    t.user_agent = user_agent_->GetValue().utf8_string();
    t.connect_timeout_ms = std::lround(connect_timeout_->GetValue() * 1000.0);
    t.keepalive_ms = std::lround(keepalive_->GetValue() * 1000.0);
    t.max_message_bytes = static_cast<int>(std::lround(max_msg_->GetValue())) << 20;
    opt_o_.proto_field_names = proto_names_->GetValue();
    opt_o_.emit_defaults = emit_defaults_->GetValue();
    opt_o_.allow_unknown_fields = allow_unknown_->GetValue();
    opt_o_.deadline_ms = std::lround(deadline_->GetValue() * 1000.0);
  }

  void push_options() {
    if (opt_page_built_) {
      const GrpcTarget& t = opt_t_;
      authority_->ChangeValue(wxString::FromUTF8(t.authority));
      skip_verify_->SetValue(t.insecure_skip_verify);
      server_name_->ChangeValue(wxString::FromUTF8(t.server_name));
      user_agent_->ChangeValue(wxString::FromUTF8(t.user_agent));
      connect_timeout_->SetValue(static_cast<double>(t.connect_timeout_ms) / 1000.0);
      keepalive_->SetValue(static_cast<double>(t.keepalive_ms) / 1000.0);
      max_msg_->SetValue(static_cast<double>(t.max_message_bytes >> 20));
      proto_names_->SetValue(opt_o_.proto_field_names);
      emit_defaults_->SetValue(opt_o_.emit_defaults);
      allow_unknown_->SetValue(opt_o_.allow_unknown_fields);
      deadline_->SetValue(static_cast<double>(opt_o_.deadline_ms) / 1000.0);
      update_pem_ui();
    }
    update_options_tab();
  }

  // Shows page i (0 Message, 1 Metadata, 2 Options) in both the tab strip and the book.
  void show_page(size_t i) {
    ensure_page(i);
    tabs_->select(i);
    if (book_->GetSelection() != static_cast<int>(i)) book_->ChangeSelection(i);
  }

  // Keyboard focus to the message editor. Never focus it while it is hidden behind the
  // Metadata or Options page: switch to the Message page first, so what the engineer types
  // next lands where they can see it.
  void focus_body() {
    show_page(0);
    body_->SetFocus();
  }

  void ensure_page(size_t i) {
    if (i == 1 && !md_) {
      wxWindowUpdateLocker lock(md_holder_);
      md_holder_->GetSizer()->Add(build_metadata_page(md_holder_), 1, wxEXPAND);
      set_metadata(md_model_);
      md_holder_->Layout();
    } else if (i == 2 && !opt_page_built_) {
      wxWindowUpdateLocker lock(opt_holder_);
      opt_holder_->GetSizer()->Add(build_options_page(opt_holder_), 1, wxEXPAND);
      opt_page_built_ = true;
      push_options();
      opt_holder_->Layout();
    }
  }

  wxWindow* build_options_page(wxWindow* parent) {
    auto* page = new wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxBORDER_NONE);
    page->SetScrollRate(0, FromDIP(12));
    auto* g = new wxFlexGridSizer(2, FromDIP(7), FromDIP(12));
    g->AddGrowableCol(1);
    auto label = [&](const char* s) {
      g->Add(new wxStaticText(page, wxID_ANY, s), 0, wxALIGN_CENTER_VERTICAL);
    };
    auto section = [&](const char* s) {
      auto* t = new wxStaticText(page, wxID_ANY, s);
      t->SetFont(t->GetFont().Bold());
      g->Add(t, 0, wxTOP, FromDIP(6));
      g->AddSpacer(0);
    };
    auto spin = [&](double max, double init, int digits, const char* unit, const char* tip) {
      auto* row = new wxBoxSizer(wxHORIZONTAL);
      auto* sp = new wxSpinCtrlDouble(page, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(100), -1), wxSP_ARROW_KEYS,
                                      0, max, init, 1);
      sp->SetDigits(digits);
      sp->SetToolTip(tip);
      auto* h = new wxStaticText(page, wxID_ANY, unit);
      h->SetForegroundColour(th_.muted);
      row->Add(sp, 0, wxRIGHT, FromDIP(8));
      row->Add(h, 0, wxALIGN_CENTER_VERTICAL);
      g->Add(row, 0, wxEXPAND);
      sp->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) {
        update_options_tab();
        changed();
      });
      return sp;
    };
    auto text = [&](const char* hint, bool reload) {
      auto* t = new wxTextCtrl(page, wxID_ANY);
      t->SetHint(hint);
      g->Add(t, 0, wxEXPAND);
      t->Bind(wxEVT_TEXT, [this](wxCommandEvent& e) {
        e.Skip();
        update_options_tab();
        changed();
      });
      // A channel setting the schema depends on: reload once the field is left.
      if (reload)
        t->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) {
          e.Skip();
          CallAfter([this] { maybe_load_schema(false); });
        });
      return t;
    };

    section("Call");
    label("Deadline");
    deadline_ = spin(86400, 30, 1, "seconds  (0 = none)",
                     "The call fails with DEADLINE_EXCEEDED after this long (grpcurl -max-time). 0 = none.");
    label("Connect timeout");
    connect_timeout_ =
        spin(3600, 0, 1, "seconds  (0 = 20 s)", "Give up connecting after this long (grpcurl -connect-timeout)");
    label("Response JSON");
    // One per line: a wrap sizer in the (vertically) scrolled page never wraps, it clips.
    auto* jrow = new wxBoxSizer(wxVERTICAL);
    proto_names_ = new CheckBox(page, "proto field names");
    proto_names_->SetToolTip("Keys as written in the .proto (received_at) instead of lowerCamelCase (receivedAt)");
    emit_defaults_ = new CheckBox(page, "show default values");
    emit_defaults_->SetToolTip("Print fields that are at their default value (grpcurl -emit-defaults)");
    emit_defaults_->SetValue(true);
    allow_unknown_ = new CheckBox(page, "drop unknown request fields");
    allow_unknown_->SetToolTip("Request fields the schema does not know are dropped instead of failing the send");
    for (CheckBox* c : {proto_names_, emit_defaults_, allow_unknown_}) {
      jrow->Add(c, 0, wxBOTTOM, FromDIP(4));
      c->on_change = [this] {
        update_options_tab();
        changed();
      };
    }
    g->Add(jrow, 0, wxEXPAND);

    section("Connection");
    label("Authority");
    authority_ = text(":authority override", true);
    authority_->SetToolTip(":authority header override; with TLS it is also the server name (grpcurl -authority)");
    label("User agent");
    user_agent_ = text("user-agent prefix", false);
    user_agent_->SetToolTip("Prepended to the gRPC user-agent (grpcurl -user-agent)");
    label("Keepalive");
    keepalive_ =
        spin(86400, 0, 0, "seconds between pings  (0 = off)", "HTTP/2 keepalive ping interval (grpcurl -keepalive-time)");
    label("Max message");
    max_msg_ = spin(2047, 0, 0, "MB  (0 = unlimited)", "Largest message sent or received (grpcurl -max-msg-sz)");

    section("TLS  (address bar set to TLS)");
    label("Verification");
    skip_verify_ = new CheckBox(page, "Skip certificate verification (-insecure)");
    g->Add(skip_verify_, 0, wxEXPAND);
    skip_verify_->on_change = [this] {
      update_options_tab();
      changed();
      maybe_load_schema(false);
    };
    label("Server name");
    server_name_ = text("SNI / certificate name", true);
    server_name_->SetToolTip("Name for SNI and the certificate check (grpcurl -servername)");
    const char* names[3] = {"CA certificate", "Client certificate", "Client key"};
    const char* tips[3] = {"PEM file with the CA certificates that sign the server certificate (grpcurl -cacert)",
                           "PEM file with the client certificate chain for mutual TLS (grpcurl -cert)",
                           "PEM file with the private key of the client certificate (grpcurl -key)"};
    for (int i = 0; i < 3; ++i) {
      label(names[i]);
      Pem& pm = pem_[i];
      auto* row = new wxBoxSizer(wxHORIZONTAL);
      pm.pick = new FlatButton(page, wxString::FromUTF8("Choose\xE2\x80\xA6"), FlatButton::Style::Outline);
      pm.pick->SetToolTip(tips[i]);
      pm.clear = new FlatButton(page, "Clear", FlatButton::Style::Subtle);
      pm.info = new wxStaticText(page, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_MIDDLE);
      pm.info->SetMinSize(wxSize(FromDIP(40), -1));
      row->Add(pm.pick, 0, wxRIGHT, FromDIP(6));
      row->Add(pm.clear, 0, wxRIGHT, FromDIP(8));
      row->Add(pm.info, 1, wxALIGN_CENTER_VERTICAL);
      g->Add(row, 0, wxEXPAND);
      pm.pick->on_click = [this, i] { pick_pem(i); };
      pm.clear->on_click = [this, i] {
        pull_options();
        pem_text(i)->clear();
        pem_[i].from.clear();
        update_pem_ui();
        update_options_tab();
        changed();
        maybe_load_schema(false);
      };
    }
    auto* sz = new wxBoxSizer(wxVERTICAL);
    sz->Add(g, 0, wxEXPAND | wxALL, FromDIP(10));
    page->SetSizer(sz);
    return page;
  }

  // A PEM file for the CA / client certificate / client key: read once when picked (a few KB);
  // the request keeps the PEM text, as GrpcTarget does.
  void pick_pem(int i) {
    wxFileDialog dlg(this, i == 2 ? "Client private key (PEM)" : "Certificate (PEM)", "", "",
                     "PEM files (*.pem;*.crt;*.cer;*.key)|*.pem;*.crt;*.cer;*.key|All files (*.*)|*.*",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return;
    const std::filesystem::path path(dlg.GetPath().ToStdWstring());
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    std::string pem;
    if (!ec && size <= (1u << 20)) {
      if (FILE* f = _wfopen(path.c_str(), L"rb")) {
        pem.resize(static_cast<size_t>(size));
        pem.resize(std::fread(pem.data(), 1, pem.size(), f));
        std::fclose(f);
      }
    }
    if (pem.find("-----BEGIN ") == std::string::npos) {
      wxMessageBox(ec                  ? wxString("Could not read the file.")
                   : size > (1u << 20) ? wxString("The file is larger than 1 MB, so it is not a PEM file.")
                                       : wxString("The file has no PEM block (-----BEGIN ...-----)."),
                   "watt", wxOK | wxICON_WARNING, this);
      return;
    }
    pull_options();
    *pem_text(i) = std::move(pem);
    pem_[i].from = path.filename().wstring();
    update_pem_ui();
    update_options_tab();
    changed();
    maybe_load_schema(false);
  }

  std::string* pem_text(int i) {
    return i == 0 ? &opt_t_.root_cert_pem : i == 1 ? &opt_t_.client_cert_pem : &opt_t_.client_key_pem;
  }

  void update_pem_ui() {
    if (!opt_page_built_) return;
    for (int i = 0; i < 3; ++i) {
      Pem& pm = pem_[i];
      const std::string& pem = *pem_text(i);
      pm.clear->Show(!pem.empty());
      wxString t;
      if (pem.empty()) {
        t = i == 0 ? "system roots" : i == 1 ? "none (no mutual TLS)" : "none";
        pm.info->SetForegroundColour(th_.muted);
      } else {
        size_t certs = 0;
        for (size_t at = pem.find("-----BEGIN CERTIFICATE"); at != std::string::npos;
             at = pem.find("-----BEGIN CERTIFICATE", at + 1))
          ++certs;
        t = pm.from.empty() ? wxString("from the request") : wxString(pm.from);
        if (i < 2)
          t += wxString::Format(L"  ·  %zu certificate%s", certs, certs == 1 ? "" : "s");
        else
          t += pem.find("PRIVATE KEY") != std::string::npos ? wxString(L"  ·  private key")
                                                               : wxString(L"  ·  no private key found");
        pm.info->SetForegroundColour(th_.fg);
      }
      pm.info->SetLabel(t);
      pm.info->SetToolTip(t);
    }
    if (auto* p = pem_[0].info->GetParent()) p->Layout();
  }

  // Documents for the ILoader are created on a hidden control: CreateLoader() must not touch
  // the visible editor's view state.
  wxStyledTextCtrl* factory() {
    if (!factory_) {
      factory_ = new wxStyledTextCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(1, 1));
      factory_->Hide();
    }
    return factory_;
  }

  // ------------------------------------------------------------------ state <-> UI

  void changed() {
    if (!loading_) host_->changed();
  }

  // Everything but the body.
  void store_fields(SavedRequest& r) const {
    store_head(r);
    const GrpcTarget& t = opt_t_;
    r.grpc_target.authority = t.authority;
    r.grpc_target.insecure_skip_verify = t.insecure_skip_verify;
    r.grpc_target.server_name = t.server_name;
    r.grpc_target.user_agent = t.user_agent;
    r.grpc_target.root_cert_pem = t.root_cert_pem;
    r.grpc_target.client_cert_pem = t.client_cert_pem;
    r.grpc_target.client_key_pem = t.client_key_pem;
    r.grpc_target.connect_timeout_ms = t.connect_timeout_ms;
    r.grpc_target.keepalive_ms = t.keepalive_ms;
    r.grpc_target.max_message_bytes = t.max_message_bytes;
    r.grpc_target.web_text = t.web_text;
    r.grpc_opts.proto_field_names = opt_o_.proto_field_names;
    r.grpc_opts.emit_defaults = opt_o_.emit_defaults;
    r.grpc_opts.allow_unknown_fields = opt_o_.allow_unknown_fields;
    r.grpc_opts.deadline_ms = opt_o_.deadline_ms;
    r.grpc_opts.metadata = metadata();
    r.proto_files.clear();
    r.import_dirs.clear();
    if (source_->GetSelection() == kSourceProto) {
      r.proto_files = files_full_;
      r.import_dirs = dirs_full_;
    }
  }

  // The editor text (the text being loaded while a document is built on a worker). One copy.
  std::string body_text() const {
    if (pending_text_) return *pending_text_;
    const int n = body_->GetLength();
    if (n == 0) return {};
    return std::string(body_->GetCharacterPointer(), static_cast<size_t>(n));
  }

  std::string compose_body() const {
    if (!stream_mode_) return body_text();
    std::vector<std::string> all = messages_;
    if (cur_ >= 0 && static_cast<size_t>(cur_) < all.size()) all[static_cast<size_t>(cur_)] = body_text();
    return join_messages(all);
  }

  static bool trim_empty(const std::string& s) { return s.find_first_not_of(" \t\r\n") == std::string::npos; }

  void set_plain(bool p) {
    if (p == plain_) return;
    plain_ = p;
    set_json_lexing(body_, !p);
    if (p && brace_lit_) {
      body_->BraceHighlight(wxSTC_INVALID_POSITION, wxSTC_INVALID_POSITION);
      brace_lit_ = false;
    }
    if (p && body_->AutoCompActive()) body_->AutoCompCancel();
  }

  // Document-level settings travel with the document: set them again after SetDocPointer.
  void doc_settings(bool plain) {
    body_->SetCodePage(wxSTC_CP_UTF8);
    body_->SetEOLMode(wxSTC_EOL_LF);
    body_->SetTabWidth(2);
    body_->SetUseTabs(false);
    body_->SetIndent(2);
    plain_ = plain;
    set_json_lexing(body_, !plain);
  }

  void set_editor_text(const std::string& s) {
    ++load_gen_;  // supersedes a document still being built
    drop_doc(undo_doc_);
    drop_doc(redo_doc_);
    const bool was = loading_;
    loading_ = true;
    body_->SetReadOnly(false);
    body_->SetScrollWidth(1);
    if (s.size() >= kAsyncDoc) {
      // Swap in an empty document now (frees the old one), build the real one on a worker.
      pending_text_ = std::make_shared<const std::string>(s);
      doc_pending_ = true;
      swap_in(nullptr);
      doc_settings(true);
      body_->SetReadOnly(true);
      json_status_->SetForegroundColour(th_.muted);
      set_status_label(wxString::FromUTF8("Loading " + human_size(s.size()) + "\xE2\x80\xA6"));
      start_doc_job(DocKind::Load, pending_text_, {}, {}, {}, 0, -1);
      loading_ = was;
      return;
    }
    pending_text_.reset();
    doc_pending_ = false;
    std::string broken;
    const std::string* src = &s;
    if (longest_line(s.data(), s.size(), kPlainLine) > kPlainLine) {
      break_json_lines(s.data(), s.size(), kBreakAt, broken);
      src = &broken;
    }
    set_plain(wants_plain(src->data(), src->size()));
    body_->SetTextRaw(src->c_str());
    body_->EmptyUndoBuffer();
    body_->SetSavePoint();
    body_->GotoPos(0);
    fit_line_margin(body_);
    loading_ = was;
    ++body_gen_;
    json_timer_.StartOnce(50);
  }

  // Builds a document on a worker: Load (text as is), Format (text pretty-printed) or Paste
  // (prefix + pasted + suffix). The result is swapped in by swap_doc() unless superseded.
  void start_doc_job(DocKind kind, std::shared_ptr<const std::string> text, std::wstring paste, std::string prefix,
                     std::string suffix, int caret, int first_line) {
    const uint64_t gen = load_gen_;
    size_t hint = text ? text->size() * (kind == DocKind::Format ? 2 : 1) : prefix.size() + suffix.size() + paste.size() * 2;
    hint = std::min<size_t>(hint, 0x7FFFFFF0);
    auto* loader = static_cast<SciLoader*>(factory()->CreateLoader(static_cast<int>(hint)));
    if (!loader) return;
    std::thread([jobs = jobs_, kind, text = std::move(text), paste = std::move(paste), prefix = std::move(prefix),
                 suffix = std::move(suffix), caret, first_line, gen, loader]() mutable {
      const auto t0 = Clock::now();
      std::string out;
      const std::string* src = text.get();
      bool unchanged = false;
      int caret_at = caret;
      if (kind == DocKind::Format) {
        out = json_pretty(*text);
        unchanged = out == *text;
        src = &out;
      } else if (kind == DocKind::Paste) {
        std::string p;
        if (!paste.empty()) {
          const int wn = static_cast<int>(std::min<size_t>(paste.size(), 0x7FFFFFFF));
          const int un = ::WideCharToMultiByte(CP_UTF8, 0, paste.data(), wn, nullptr, 0, nullptr, nullptr);
          p.resize(static_cast<size_t>(un));
          ::WideCharToMultiByte(CP_UTF8, 0, paste.data(), wn, p.data(), un, nullptr, nullptr);
        }
        std::wstring().swap(paste);
        p.erase(std::remove(p.begin(), p.end(), '\r'), p.end());
        out.reserve(prefix.size() + p.size() + suffix.size());
        out += prefix;
        out += p;
        out += suffix;
        caret_at = static_cast<int>(prefix.size() + p.size());
        src = &out;
      }
      // Minified JSON: short lines (see break_json_lines), the caret kept after the pasted text.
      size_t breaks = 0;
      std::string broken;
      if (!unchanged && longest_line(src->data(), src->size(), kPlainLine) > kPlainLine) {
        if (kind == DocKind::Paste) {
          std::string tail;
          const size_t at = static_cast<size_t>(caret_at);
          breaks += break_json_lines(src->data(), at, kBreakAt, broken);
          breaks += break_json_lines(src->data() + at, src->size() - at, kBreakAt, tail);
          caret_at = static_cast<int>(broken.size());
          broken += tail;
        } else {
          breaks = break_json_lines(src->data(), src->size(), kBreakAt, broken);
        }
        src = &broken;
      }
      const bool plain = wants_plain(src->data(), src->size());
      if (!unchanged)
        for (size_t off = 0; off < src->size(); off += 4u << 20)
          loader->AddData(src->data() + off, static_cast<ptrdiff_t>(std::min(src->size() - off, size_t{4} << 20)));
      const double worker_ms = ms_between(t0, Clock::now());
      const size_t bytes = src->size();
      std::lock_guard lk(jobs->m);
      if (!jobs->target || unchanged) {
        loader->Release();
        if (jobs->target) {
          auto* ed = static_cast<GrpcEditor*>(jobs->target);
          ed->CallAfter([ed, gen] { ed->swap_doc(nullptr, gen, false, DocKind::Format, 0, -1, 0, 0, 0); });
        }
        return;
      }
      void* doc = loader->ConvertToDocument();
      jobs->docs.push_back(doc);
      auto* ed = static_cast<GrpcEditor*>(jobs->target);
      ed->CallAfter([ed, doc, gen, plain, kind, caret_at, first_line, worker_ms, bytes, breaks] {
        ed->swap_doc(doc, gen, plain, kind, caret_at, first_line, worker_ms, bytes, breaks);
      });
    }).detach();
  }

  void swap_doc(void* doc, uint64_t gen, bool plain, DocKind kind, int caret, int first_line, double worker_ms,
                size_t bytes, size_t breaks) {
    if (doc) {
      std::lock_guard lk(jobs_->m);
      auto& d = jobs_->docs;
      d.erase(std::remove(d.begin(), d.end(), doc), d.end());
    }
    if (gen != load_gen_) {  // superseded by a newer load / edit
      if (doc) body_->ReleaseDocument(doc);
      return;
    }
    doc_pending_ = false;
    pending_text_.reset();
    body_->SetReadOnly(false);
    if (!doc) {  // Format: nothing to change
      json_status_->SetForegroundColour(th_.muted);
      set_status_label("Already formatted (or not valid JSON)");
      return;
    }
    const auto t0 = Clock::now();
    const bool was = loading_;
    loading_ = true;
    // Paste / Format replace the whole document: the document they replace is kept, so Ctrl+Z
    // past the start of the new one's history brings it back (one undo step, like an edit).
    drop_doc(redo_doc_);
    drop_doc(undo_doc_);
    if (kind != DocKind::Load) undo_doc_ = keep_current();
    swap_in(doc);
    doc_settings(plain);
    // SCI_CREATELOADER builds the document with undo collection off; without turning it back on
    // nothing the user types is recorded (no undo, GetModify() never true).
    body_->SetUndoCollection(true);
    body_->EmptyUndoBuffer();
    body_->SetSavePoint();
    body_->SetScrollWidth(1);
    if (kind == DocKind::Load) {
      body_->GotoPos(0);
    } else {
      body_->GotoPos(std::min(caret, body_->GetLength()));
      if (first_line >= 0) body_->SetFirstVisibleLine(first_line);
      body_->SetXOffset(0);
    }
    fit_line_margin(body_);
    loading_ = was;
    ++body_gen_;
    const double ui_ms = ms_between(t0, Clock::now());
    if (kind != DocKind::Load) {
      if (stream_mode_ && cur_ >= 0 && static_cast<size_t>(cur_) < messages_.size()) refresh_message_list();
      changed();
    }
    check_json();
    if (kind != DocKind::Load) {
      json_status_->SetToolTip(wxString::Format("%s %s in %s (UI %s)%s", kind == DocKind::Format ? "Formatted" : "Pasted",
                                                wxString::FromUTF8(human_size(bytes)),
                                                wxString::FromUTF8(elapsed_text(worker_ms)),
                                                wxString::FromUTF8(elapsed_text(ui_ms)),
                                                plain ? "; large document: no syntax colouring or folding" : ""));
    }
    if (breaks > 0) {
      set_status_label(json_status_->GetLabel() + wxString::FromUTF8(" · long lines split (same JSON)"));
      json_status_->SetToolTip(json_status_->GetToolTipText() +
                               wxString::Format("\nThe minified JSON was split into %zu shorter lines after commas "
                                                "and brackets (whitespace only), so editing stays instant.",
                                                breaks));
    }
  }

  // ------------------------------------------------------------------ undo across documents

  // The editor's current document with its caret, held by an extra reference.
  UndoDoc keep_current() {
    UndoDoc u;
    u.doc = body_->GetDocPointer();
    body_->AddRefDocument(u.doc);
    u.plain = plain_;
    u.caret = body_->GetCurrentPos();
    u.anchor = body_->GetAnchor();
    u.first_line = body_->GetFirstVisibleLine();
    return u;
  }

  // Releases a kept document (on a worker: freeing megabytes of text is not UI work).
  void drop_doc(UndoDoc& u) {
    if (!u.doc) return;
    void* d = std::exchange(u.doc, nullptr);
    factory();
    std::thread([jobs = jobs_, d] {
      std::lock_guard lk(jobs->m);
      if (jobs->target) static_cast<GrpcEditor*>(jobs->target)->factory_->ReleaseDocument(d);
    }).detach();
  }

  // Ctrl+Z at the start of a pasted / formatted document's history (back = true) brings back
  // the document it replaced; Ctrl+Y at the end of that one's redo history goes forward again.
  bool swap_history(bool back) {
    if (doc_pending_) return false;
    UndoDoc& from = back ? undo_doc_ : redo_doc_;
    UndoDoc& to = back ? redo_doc_ : undo_doc_;
    if (!from.doc || (back ? body_->CanUndo() : body_->CanRedo())) return false;
    drop_doc(to);
    to = keep_current();
    const UndoDoc u = std::exchange(from, UndoDoc{});
    const bool was = loading_;
    loading_ = true;
    body_->SetDocPointer(u.doc);
    body_->ReleaseDocument(u.doc);  // the control holds its own reference now
    doc_settings(u.plain);
    body_->SetScrollWidth(1);
    // (not SetSelection(): it goes through SetSelectionStart / End, which drop a backwards selection)
    body_->GotoPos(std::min(u.caret, body_->GetLength()));
    body_->SetAnchor(std::min(u.anchor, body_->GetLength()));
    body_->SetFirstVisibleLine(u.first_line);
    body_->EnsureCaretVisible();
    fit_line_margin(body_);
    loading_ = was;
    ++body_gen_;
    if (stream_mode_ && cur_ >= 0 && static_cast<size_t>(cur_) < messages_.size()) refresh_message_list();
    changed();
    check_json();
    return true;
  }

  // Makes doc (nullptr: a new empty one) the editor's document. Freeing a big old document
  // (megabytes of text, a million line starts) happens on a worker, not here.
  void swap_in(void* doc) {
    void* old = nullptr;
    if (body_->GetLength() > (1 << 20)) {
      old = body_->GetDocPointer();
      body_->AddRefDocument(old);
    }
    body_->SetDocPointer(doc);
    if (doc) body_->ReleaseDocument(doc);  // the control holds its own reference now
    if (!old) return;
    factory();  // the worker releases through it
    std::thread([jobs = jobs_, old] {
      std::lock_guard lk(jobs->m);
      if (jobs->target) static_cast<GrpcEditor*>(jobs->target)->factory_->ReleaseDocument(old);
    }).detach();
  }

  // Ctrl+V of a big clipboard text: build the result on a worker instead of Scintilla's paste.
  bool try_big_paste() {
    if (body_->GetReadOnly()) return false;
    // Straight from the clipboard handle: one copy of the UTF-16 text, converted on the worker.
    std::wstring clip;
    if (!::OpenClipboard(GetHWND())) return false;
    if (HANDLE h = ::GetClipboardData(CF_UNICODETEXT)) {
      if (const auto* w = static_cast<const wchar_t*>(::GlobalLock(h))) {
        const size_t n = wcsnlen(w, ::GlobalSize(h) / sizeof(wchar_t));
        if (n >= kAsyncPaste) clip.assign(w, n);
        ::GlobalUnlock(h);
      }
    }
    ::CloseClipboard();
    if (clip.empty()) return false;
    start_paste(std::move(clip));
    return true;
  }

  // Replaces the selection with clip, the new document built on a worker.
  void start_paste(std::wstring clip) {
    const int a = body_->GetSelectionStart(), b = body_->GetSelectionEnd();
    const char* p = body_->GetCharacterPointer();
    const int n = body_->GetLength();
    std::string prefix(p, static_cast<size_t>(std::min(a, n)));
    std::string suffix(p + std::min(b, n), static_cast<size_t>(n - std::min(b, n)));
    ++load_gen_;
    doc_pending_ = true;
    body_->SetReadOnly(true);
    json_status_->SetForegroundColour(th_.muted);
    set_status_label(wxString::FromUTF8("Pasting " + human_size(clip.length()) + "\xE2\x80\xA6"));
    start_doc_job(DocKind::Paste, nullptr, std::move(clip), std::move(prefix), std::move(suffix), 0, -1);
  }

  // Shows body in the current mode (a message list for client / bidi streams).
  void set_body(const std::string& body) {
    if (stream_mode_) {
      messages_.clear();
      if (trim_empty(body)) messages_.push_back("");
      else if (body.size() > kSplitMax || !split_json_array(body, messages_)) messages_ = {body};
      if (messages_.empty()) messages_.push_back("");
      sent_.assign(messages_.size(), 0);
      cur_ = -1;
      select_message(0, true);
    } else {
      messages_.clear();
      sent_.clear();
      cur_ = 0;
      set_editor_text(body);
    }
  }

  // The user started editing a message that was already sent. While the stream is open the
  // slot keeps exactly what went out (and its tick) and the edit becomes a new message at the
  // end of the list; once the call is over the slot is simply no longer marked as sent.
  void edit_sent_message() {
    const size_t i = static_cast<size_t>(cur_);
    if (in_flight_ && interactive()) {
      messages_.push_back(std::string());  // holds the editor's text from the next sync on
      sent_.push_back(0);
      cur_ = static_cast<int>(messages_.size()) - 1;
      stream_hint_->SetLabel(wxString::Format("#%zu kept as sent; editing new #%d", i + 1, cur_ + 1));
      stream_hint_->SetToolTip("A message that was sent keeps exactly what went out; the edit is a new message");
    } else {
      sent_[i] = 0;
    }
    refresh_message_list();
  }

  void sync_current() {
    if (stream_mode_ && cur_ >= 0 && static_cast<size_t>(cur_) < messages_.size())
      messages_[static_cast<size_t>(cur_)] = body_text();
  }

  void select_message(int i, bool force = false) {
    if (i < 0 || static_cast<size_t>(i) >= messages_.size()) i = 0;
    if (i == cur_ && !force) return;
    if (cur_ != i) sync_current();
    cur_ = i;
    set_editor_text(messages_[static_cast<size_t>(i)]);
    refresh_message_list();
  }

  wxString message_label(size_t i) const {
    std::string text = static_cast<int>(i) == cur_ && stream_mode_ ? text_of_head() : messages_[i];
    std::string pv = one_line_preview(text, 40);
    if (pv.empty()) pv = "(empty)";
    return wxString::Format("%s%zu  %s", sent_.size() > i && sent_[i] ? wxString::FromUTF8("✓ ") : wxString(""),
                            i + 1, wxString::FromUTF8(pv));
  }

  // The start of the editor text (enough for a list preview, without copying a big body).
  std::string text_of_head() const {
    if (pending_text_) return pending_text_->substr(0, 400);
    const int n = std::min(body_->GetLength(), 400);
    const wxCharBuffer b = body_->GetTextRangeRaw(0, n);
    return std::string(b.data(), b.length());
  }

  void refresh_message_list() {
    if (!stream_mode_) return;
    ensure_stream_ui();
    msgs_->Freeze();
    if (msgs_->GetCount() != messages_.size()) {
      msgs_->Clear();
      for (size_t i = 0; i < messages_.size(); ++i) msgs_->Append(message_label(i));
    } else {
      for (size_t i = 0; i < messages_.size(); ++i) {
        const wxString l = message_label(i);
        if (msgs_->GetString(static_cast<unsigned>(i)) != l) msgs_->SetString(static_cast<unsigned>(i), l);
      }
    }
    if (cur_ >= 0) msgs_->SetSelection(cur_);
    msgs_->Thaw();
    del_msg_->Enable(true);
    up_msg_->Enable(cur_ > 0);
    down_msg_->Enable(cur_ + 1 < static_cast<int>(messages_.size()));
    // After a stream ended the ticks show what went out on it; sending a ticked message again
    // opens a new stream, and the button says so instead of offering "Send" for a sent message.
    const bool resend = !in_flight_ && cur_ >= 0 && static_cast<size_t>(cur_) < sent_.size() && sent_[static_cast<size_t>(cur_)];
    send_one_->SetLabel(wxString::Format(resend ? "Resend #%d" : "Send #%d", cur_ + 1));
    send_one_->SetToolTip(resend ? "Open a new stream and send this message again (Ctrl+Shift+Enter)"
                                 : "Send the selected message on the open stream (Ctrl+Shift+Enter); opens the stream if needed");
    stream_bar_->Layout();
  }

  // Switches between a single message and a message list when the method's kind requires it.
  void apply_kind(GrpcCallKind k) {
    const bool want = client_streams(k);
    if (want == stream_mode_) return;
    const bool was = loading_;
    loading_ = true;
    if (want) {
      const std::string one = body_text();
      stream_mode_ = true;
      messages_ = {one};
      sent_.assign(1, 0);
      cur_ = -1;
      select_message(0, true);
    } else {
      sync_current();
      const std::string first = messages_.empty() ? std::string() : messages_[0];
      stream_mode_ = false;
      messages_.clear();
      sent_.clear();
      cur_ = 0;
      set_editor_text(first);
    }
    loading_ = was;
    update_stream_ui();
  }

  void update_stream_ui() {
    const bool show = stream_mode_;
    if (show) ensure_stream_ui();
    if (!stream_bar_) {
      if (auto* p = body_->GetParent()) p->Layout();
      return;
    }
    const bool manual = interactive();
    stream_bar_->Show(show);
    msg_col_->Show(show);
    if (show) {
      const size_t n = messages_.size();
      send_one_->Show(manual);
      send_all_->Show(manual);
      end_->Show(manual);
      end_->Enable(in_flight_);
      if (manual) {
        const bool ended = !in_flight_ && std::find(sent_.begin(), sent_.end(), static_cast<char>(1)) != sent_.end();
        stream_hint_->SetLabel(in_flight_ ? "Stream open" : ended ? "Stream ended; sending opens a new one" : "Opens on first send");
        stream_hint_->SetToolTip(in_flight_ ? "Send messages one at a time, then End to half-close"
                                 : ended    ? "Ticks mark what went out on the last stream. Send opens a new stream."
                                            : "The stream opens with the first message you send");
      } else {
        stream_hint_->SetLabel(wxString::Format("Sends %zu message%s, then closes", n, n == 1 ? "" : "s"));
        stream_hint_->SetToolTip("Send streams every message in the list in order, then half-closes");
      }
      refresh_message_list();
    }
    if (auto* p = body_->GetParent()) p->Layout();
  }

  void update_send_label() {
    if (in_flight_) send_->SetLabel("Cancel");
    else send_->SetLabel(interactive() ? "Open stream" : "Send");
    const bool blocked = !in_flight_ && web_blocks();
    send_->Enable(!blocked);
    send_->SetToolTip(blocked ? "gRPC-Web has no client or bidi streaming: switch gRPC-Web off to call this method"
                              : "Send the request (Ctrl+Enter)");
    if (send_one_) send_one_->Enable(!blocked);
    if (send_all_) send_all_->Enable(!blocked);
    if (manual_) manual_->Enable(!blocked);
    top_->Layout();
  }

  // gRPC-Web cannot carry client or bidi streams (the engine answers UNIMPLEMENTED).
  bool web_blocks() const {
    if (!web_->checked()) return false;
    const GrpcMethodInfo* m = find_method(method_);
    return m && client_streams(m->kind);
  }

  void update_web_ui() {
    tree_->set_web(web_->checked());
    update_send_label();
    if (web_blocks()) {
      replaced_note_ = false;
      notes_->SetForegroundColour(th_.error);
      notes_->SetLabel("gRPC-Web has no client or bidi streaming: switch gRPC-Web off to call this method");
    } else if (notes_->GetForegroundColour() == th_.error) {
      notes_->SetForegroundColour(th_.muted);
      if (const GrpcMethodInfo* m = find_method(method_)) show_notes(*m);
      else notes_->SetLabel("");
    }
    if (auto* p = notes_->GetParent()) p->Layout();
  }

  void update_tls_label() {
    tls_->SetLabel(tls_->checked() ? wxString::FromUTF8("\U0001F512 TLS") : wxString::FromUTF8("Plaintext"));
  }

  void update_source_ui() {
    const bool proto = source_->GetSelection() == kSourceProto;
    if (proto) {
      ensure_proto_box();
      refresh_proto_lists();
    }
    proto_box_->Show(proto);
    left_->Layout();
  }

  void refresh_proto_lists() {
    if (!files_) return;
    files_->Clear();
    for (const auto& f : files_full_) files_->Append(file_label(f));
    dirs_->Clear();
    for (const auto& d : dirs_full_) dirs_->Append(dir_label(d));
  }

  void update_options_tab() {
    pull_options();
    const GrpcTarget& t = opt_t_;
    const double dl = static_cast<double>(opt_o_.deadline_ms) / 1000.0;
    wxString badge;
    bool warn = false;
    if (std::lround(dl * 1000.0) != 30000) {
      badge = dl <= 0 ? wxString("no deadline") : wxString::Format("%g s", dl);
      warn = dl > 0 && dl < 5;
    }
    auto add = [&](const char* s) { badge += badge.empty() ? wxString(s) : wxString(", ") + s; };
    // TLS settings only count (and only show) while TLS is on.
    const bool tls = tls_->checked();
    if (tls && t.insecure_skip_verify) add("insecure");
    if (tls && (!t.client_cert_pem.empty() || !t.client_key_pem.empty())) add("mTLS");
    else if (tls && !t.root_cert_pem.empty()) add("CA");
    if (!t.authority.empty()) add("authority");
    if (opt_o_.proto_field_names || !opt_o_.emit_defaults || opt_o_.allow_unknown_fields) add("JSON");
    if (t.connect_timeout_ms > 0 || t.keepalive_ms > 0 || t.max_message_bytes > 0 || !t.user_agent.empty() ||
        !t.server_name.empty())
      add("channel");
    tabs_->set_badge(2, badge, warn || (tls && t.insecure_skip_verify));
  }

  void hide_schema_pane() {
    if (split_->IsSplit()) toggle_schema_pane();
  }

  void toggle_schema_pane() {
    if (split_->IsSplit()) {
      sash_ = split_->GetSashPosition();
      split_->Unsplit(left_);
      schema_btn_->set_checked(false);
      // Reopening the pane shows every method, not the one a filter narrowed it to (later: this
      // runs from inside the tree's own pick event).
      CallAfter([this, alive = alive_] {
        if (*alive && !split_->IsSplit()) tree_->clear_filter();
      });
    } else {
      split_->SplitVertically(left_, right_, sash_ > 0 ? sash_ : FromDIP(230));
      schema_btn_->set_checked(true);
    }
  }

  void update_header() {
    const GrpcMethodInfo* m = find_method(method_);
    def_btn_->Enable(m != nullptr);
    if (method_.empty()) {
      badge_->Hide();
      method_btn_->set_text_colour(th_.muted);
      method_btn_->SetLabel("Select a method");
      io_lbl_->SetLabel(schema_ && schema_->ok ? "pick one here or on the left" : "load a schema first");
    } else {
      std::string svc, name = method_;
      if (const size_t p = method_.rfind('/'); p != std::string::npos && p > 0) {
        name = method_.substr(p + 1);
        svc = short_type(method_.substr(method_[0] == '/' ? 1 : 0, p - (method_[0] == '/' ? 1 : 0)));
      }
      method_btn_->set_text_colour(th_.fg);
      method_btn_->SetLabel(wxString::FromUTF8(svc.empty() ? name : svc + " / " + name));
      method_btn_->SetToolTip(wxString::FromUTF8(method_ + "\nClick to pick another method"));
      if (m) {
        badge_->SetLabel(wxString::Format("  %s  ", kind_short(m->kind)));
        badge_->SetBackgroundColour(kind_colour(m->kind, th_.dark));
        badge_->SetToolTip(kind_long(m->kind));
        badge_->Show();
        io_lbl_->SetLabel(wxString::FromUTF8(short_type(m->input_type) + "  →  " + short_type(m->output_type)));
        io_lbl_->SetToolTip(wxString::FromUTF8(m->input_type + "  →  " + m->output_type));
      } else {
        badge_->Hide();
        io_lbl_->SetLabel(schema_ && schema_->ok ? "not in the loaded schema" : "");
      }
    }
    right_->Layout();
  }

  // ------------------------------------------------------------------ methods + templates

  const GrpcMethodInfo* find_method(const std::string& path) const {
    if (!schema_ || !schema_->ok || path.empty()) return nullptr;
    for (const auto& m : schema_->methods)
      if (m.full_path == path) return &m;
    return nullptr;
  }

  std::string template_for(const std::string& path) {
    const GrpcMethodInfo* m = find_method(path);
    if (!m) return {};
    std::string ex = host_->grpc_client().example_request(path);
    return ex.empty() ? m->request_template : ex;
  }

  void show_notes(const GrpcMethodInfo& m) {
    replaced_note_ = false;
    std::vector<std::string> notes;
    host_->grpc_client().example_request(m.full_path, &notes);
    if (notes.empty()) notes = m.template_notes;
    std::string all;
    for (const auto& n : notes) all += (all.empty() ? "" : "\n") + n;
    std::string first = notes.empty() ? std::string() : notes.front();
    if (notes.size() > 1) first += "  (+" + std::to_string(notes.size() - 1) + " more)";
    notes_->SetLabel(wxString::FromUTF8(first));
    notes_->SetToolTip(wxString::FromUTF8(all));
  }

  void fill_template(const GrpcMethodInfo& m) {
    const std::string t = template_for(m.full_path);
    const std::string before = last_template_;
    last_template_ = t;
    // Text the user typed (not an earlier example) is replaced as one undoable edit: Ctrl+Z
    // gets it back.
    const int len = body_->GetLength();
    if (!stream_mode_ && !doc_pending_ && len > 0 && static_cast<size_t>(len) < kAsyncDoc && t.size() < kAsyncDoc &&
        !wants_plain(t.data(), t.size())) {
      const std::string cur = body_text();
      if (!trim_empty(cur) && cur != before && cur != t) {
        const bool was = loading_;
        loading_ = true;
        set_plain(false);
        body_->SetTargetRange(0, len);
        body_->ReplaceTarget(wxString::FromUTF8(t));
        body_->SetSavePoint();
        body_->GotoPos(0);
        fit_line_margin(body_);
        loading_ = was;
        ++body_gen_;
        json_timer_.StartOnce(50);
        show_notes(m);
        replaced_before_ = notes_->GetLabel();
        replaced_note_ = true;
        notes_->SetLabel(wxString::FromUTF8("Replaced your text with the example  (Ctrl+Z restores it)"));
        return;
      }
    }
    if (stream_mode_) {
      messages_ = {t};
      sent_.assign(1, 0);
      cur_ = -1;
      select_message(0, true);
    } else {
      set_editor_text(t);
    }
    show_notes(m);
  }

  void pick_method(const std::string& path) {
    if (path == method_) return;
    // Remember what was typed for the old method, so switching back restores it.
    if (!method_.empty()) {
      std::string cur = compose_body();
      if (cur.size() <= kRememberMax && !trim_empty(cur)) body_by_method_[method_] = std::move(cur);
    }
    method_ = path;
    keys_method_.clear();
    if (const GrpcMethodInfo* m = find_method(path)) {
      stream_mode_ = client_streams(m->kind);
      auto it = body_by_method_.find(path);
      if (it != body_by_method_.end()) {
        set_body(it->second);
        show_notes(*m);
      } else {
        fill_template(*m);
      }
    }
    tree_->select(method_);
    update_header();
    update_stream_ui();
    update_web_ui();
    changed();
  }

  void show_method_menu() {
    if (!schema_ || !schema_->ok || schema_->methods.empty()) {
      if (!split_->IsSplit()) toggle_schema_pane();
      focus_url();
      return;
    }
    wxMenu menu;
    std::map<std::string, std::vector<const GrpcMethodInfo*>> by_service;
    for (const auto& m : schema_->methods) by_service[m.service].push_back(&m);
    std::vector<const GrpcMethodInfo*> ids;
    const bool sub = schema_->methods.size() > 30;
    for (const auto& [svc, list] : by_service) {
      wxMenu* target = &menu;
      if (sub) {
        target = new wxMenu;
        menu.AppendSubMenu(target, wxString::FromUTF8(svc));
      } else {
        if (menu.GetMenuItemCount()) menu.AppendSeparator();
        menu.Append(wxID_ANY, wxString::FromUTF8(svc))->Enable(false);
      }
      for (const GrpcMethodInfo* m : list) {
        const int id = 1000 + static_cast<int>(ids.size());
        ids.push_back(m);
        auto* item = target->AppendCheckItem(id, wxString::FromUTF8("    " + m->method + "    " +
                                                                    std::string(kind_long(m->kind))));
        item->Check(m->full_path == method_);
      }
    }
    const wxPoint at = method_btn_->GetPosition() + wxPoint(0, method_btn_->GetSize().y);
    const int sel = right_->GetPopupMenuSelectionFromUser(menu, at);
    if (sel >= 1000 && static_cast<size_t>(sel - 1000) < ids.size()) pick_method(ids[static_cast<size_t>(sel - 1000)]->full_path);
  }

  // View proto: the method and its request / response messages as .proto source.
  void show_definition() {
    const GrpcMethodInfo* m = find_method(method_);
    if (!m) return;
    const GrpcClient& c = host_->grpc_client();
    std::string text = c.describe(m->full_path);
    if (text.empty()) text = c.describe(m->service);
    for (const std::string* t : {&m->input_type, &m->output_type}) {
      const std::string d = c.describe(*t);
      if (!d.empty() && text.find(d) == std::string::npos) text += "\n\n" + d;
    }
    if (text.empty()) text = "// The loaded schema has no definition for " + m->full_path;
    auto* dlg = new wxDialog(this, wxID_ANY, wxString::FromUTF8(m->service + "." + m->method + "  -  definition"),
                             wxDefaultPosition, wxSize(FromDIP(640), FromDIP(560)),
                             wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    dlg->SetBackgroundColour(th_.bg);
    auto* stc = new wxStyledTextCtrl(dlg, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    setup_json_editor(stc, th_);
    stc->SetLexer(wxSTC_LEX_CPP);
    stc->SetKeyWords(0, "syntax package import option message service rpc returns stream enum oneof repeated "
                        "map optional required reserved extensions extend public weak to max edition");
    stc->SetKeyWords(1, "double float int32 int64 uint32 uint64 sint32 sint64 fixed32 fixed64 sfixed32 sfixed64 "
                        "bool string bytes");
    const wxColour kw = th_.dark ? wxColour(86, 156, 214) : wxColour(0, 0, 255);
    const wxColour ty = th_.dark ? wxColour(78, 201, 176) : wxColour(38, 127, 153);
    stc->StyleSetForeground(wxSTC_C_WORD, kw);
    stc->StyleSetForeground(wxSTC_C_WORD2, ty);
    stc->StyleSetForeground(wxSTC_C_COMMENT, th_.muted);
    stc->StyleSetForeground(wxSTC_C_COMMENTLINE, th_.muted);
    stc->StyleSetForeground(wxSTC_C_COMMENTDOC, th_.muted);
    stc->StyleSetForeground(wxSTC_C_STRING, th_.dark ? wxColour(206, 145, 120) : wxColour(163, 21, 21));
    stc->StyleSetForeground(wxSTC_C_NUMBER, th_.dark ? wxColour(181, 206, 168) : wxColour(9, 134, 88));
    stc->StyleSetForeground(wxSTC_C_OPERATOR, th_.dark ? wxColour(180, 180, 180) : wxColour(60, 60, 60));
    stc->SetMarginWidth(1, 0);
    stc->SetTextRaw(text.c_str());
    stc->SetReadOnly(true);
    fit_line_margin(stc);
    auto* sz = new wxBoxSizer(wxVERTICAL);
    sz->Add(stc, 1, wxEXPAND | wxALL, FromDIP(6));
    dlg->SetSizer(sz);
    dlg->Bind(wxEVT_CLOSE_WINDOW, [dlg](wxCloseEvent&) { dlg->Destroy(); });
    dlg->Bind(wxEVT_CHAR_HOOK, [dlg](wxKeyEvent& e) {
      if (e.GetKeyCode() == WXK_ESCAPE) dlg->Close();
      else e.Skip();
    });
    dlg->CentreOnParent();
    dlg->Show();
    stc->SetFocus();
  }

  // ------------------------------------------------------------------ completion

  void ensure_keys() {
    if (keys_method_ == method_ && !method_.empty()) return;
    keys_method_ = method_;
    keys_.clear();
    input_def_.clear();
    const GrpcMethodInfo* m = find_method(method_);
    if (!m) return;
    keys_ = json_keys(template_for(method_));
    input_def_ = host_->grpc_client().describe(m->input_type);
  }

  // pos is a '"' just typed: does it open an object key (after '{' or ',')?
  bool at_key_position(int pos) {
    for (int p = pos - 1, n = 0; p >= 0 && n < 256; --p, ++n) {
      const int c = body_->GetCharAt(p);
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
      return c == '{' || c == ',';
    }
    return false;
  }

  void show_completion(bool explicit_request) {
    if (plain_) return;
    ensure_keys();
    if (keys_.empty()) return;
    const int pos = body_->GetCurrentPos();
    int start = pos;
    while (start > 0) {
      const int c = body_->GetCharAt(start - 1);
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')) break;
      --start;
    }
    if (explicit_request && !(start > 0 && body_->GetCharAt(start - 1) == '"') && !at_key_position(start)) {
      // Ctrl+Space outside a key: insert the opening quote so the completion is a key.
      if (!at_key_position(pos)) return;
      body_->InsertText(pos, "\"");
      body_->GotoPos(pos + 1);
      start = pos + 1;
    }
    std::string list;
    for (const auto& k : keys_) {
      if (!list.empty()) list += '\n';
      list += k;
    }
    body_->AutoCompShow(body_->GetCurrentPos() - start, wxString::FromUTF8(list));
  }

  void after_completion(const std::string& word) {
    int pos = body_->GetCurrentPos();
    // Step over (or add) the closing quote, then ": " unless the key already has its colon.
    if (body_->GetCharAt(pos) != '"') body_->InsertText(pos, "\"");
    ++pos;
    int q = pos;
    while (q < pos + 64 && (body_->GetCharAt(q) == ' ' || body_->GetCharAt(q) == '\t')) ++q;
    if (body_->GetCharAt(q) != ':') {
      body_->InsertText(pos, ": ");
      pos += 2;
    }
    body_->GotoPos(pos);
    // Calltip: the field's line from the request message definition.
    const std::string snake = lower_camel_to_snake(word);
    for (const std::string& name : {word, snake}) {
      const std::string needle = " " + name + " = ";
      const size_t at = input_def_.find(needle);
      if (at == std::string::npos) continue;
      size_t b = input_def_.rfind('\n', at);
      b = b == std::string::npos ? 0 : b + 1;
      size_t e = input_def_.find('\n', at);
      std::string line = input_def_.substr(b, e == std::string::npos ? std::string::npos : e - b);
      line.erase(0, line.find_first_not_of(" \t"));
      body_->CallTipShow(body_->GetCurrentPos(), wxString::FromUTF8(line));
      break;
    }
  }

  // ------------------------------------------------------------------ schema loading

  std::string schema_key() const {
    std::string k;
    if (source_->GetSelection() == kSourceReflection) {
      k = "r|" + addr_->GetValue().utf8_string() + "|" + (tls_->checked() ? "1" : "0") + "|" +
          (opt_t_.insecure_skip_verify ? "1" : "0") + "|" + opt_t_.authority + "|" + opt_t_.server_name + "|" +
          (web_->checked() ? (opt_t_.web_text ? "wt" : "w") : "g");
      // Certificates change the channel: key on their size and a cheap hash.
      for (const std::string* pem : {&opt_t_.root_cert_pem, &opt_t_.client_cert_pem, &opt_t_.client_key_pem})
        k += "|" + std::to_string(pem->size()) + ":" + std::to_string(std::hash<std::string>{}(*pem));
    } else {
      k = "p";
      for (const auto& f : files_full_) k += "|f" + f;
      for (const auto& d : dirs_full_) k += "|d" + d;
    }
    return k;
  }

  // What a schema was loaded from, coarser than schema_key(): the server address, or the file set.
  std::string schema_origin() const {
    if (source_->GetSelection() == kSourceReflection)
      return "r|" + addr_->GetValue().utf8_string() + (web_->checked() ? "|w" : "");
    std::string k = "p";
    for (const auto& f : files_full_) k += "|" + f;
    return k;
  }

  void show_schema_hint() {
    status_err_ = false;
    schema_status_->SetForegroundColour(th_.muted);
    schema_status_->set_text("");
    if (source_->GetSelection() == kSourceReflection)
      tree_->set_empty_text("Enter the server address above and press Enter to list its methods (server reflection).");
    else
      tree_->set_empty_text("Add .proto or .protoset files, and the import paths they need.");
    left_->Layout();
  }

  void maybe_load_schema(bool force) {
    const std::string key = schema_key();
    if (!force && key == schema_key_) return;
    const bool reflect = source_->GetSelection() == kSourceReflection;
    if ((reflect && addr_->GetValue().IsEmpty()) || (!reflect && files_full_.empty())) {
      schema_key_.clear();
      if (!schema_ || !schema_->ok) {
        show_schema_hint();
      } else if (status_err_) {
        // The error belonged to the other source / address: do not leave it up.
        status_err_ = false;
        schema_status_->SetForegroundColour(th_.muted);
        schema_status_->set_text("");
        left_->Layout();
      }
      return;
    }
    schema_key_ = key;
    SavedRequest r = base_;
    store_fields(r);
    ++pending_loads_;
    load_start_ = Clock::now();
    load_what_ = reflect ? "Reflecting " + r.grpc_target.address : "Parsing " + std::to_string(r.proto_files.size()) +
                                                                       (r.proto_files.size() == 1 ? " file" : " files");
    reload_->Enable(false);
    tick_load();
    load_timer_.Start(50);
    host_->load_schema(r);
  }

  void tick_load() {
    const double ms = ms_between(load_start_, Clock::now());
    schema_status_->SetForegroundColour(th_.muted);
    schema_status_->set_text(wxString::FromUTF8(load_what_ + "  …  " + elapsed_text(ms)));
  }

  void add_files() {
    wxFileDialog dlg(this, "Add .proto files", "", "",
                     "Protocol Buffers (*.proto;*.protoset;*.pb;*.desc;*.binpb)|*.proto;*.protoset;*.pb;*.desc;*.binpb|"
                     "All files (*.*)|*.*",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);
    if (dlg.ShowModal() != wxID_OK) return;
    wxArrayString paths;
    dlg.GetPaths(paths);
    for (const auto& p : paths) {
      const std::string u = p.utf8_string();
      if (std::find(files_full_.begin(), files_full_.end(), u) == files_full_.end()) files_full_.push_back(u);
      // The file's own directory is almost always an import root; add it once for convenience.
      const std::string dir = wxString(std::filesystem::path(p.ToStdWstring()).parent_path().wstring()).utf8_string();
      if (dirs_full_.empty() && p.Lower().EndsWith(".proto")) dirs_full_.push_back(dir);
      // The user just picked these in the dialog: approve them (and the import root added for
      // them) up front, so the schema load does not ask "outside the workspace, load it?" again.
      approve_path(u);
      if (!dirs_full_.empty() && dirs_full_.back() == dir) approve_path(dir);
    }
    refresh_proto_lists();
    changed();
    maybe_load_schema(false);
  }

  void approve_path(const std::string& utf8) {
    const Workspace* ws = host_->workspace();
    if (!ws) return;
    std::string err;
    ws->allow_file(std::filesystem::path(wxString::FromUTF8(utf8).ToStdWstring()), &err);
  }

  void add_dir() {
    wxDirDialog dlg(this, "Add import path", "", wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return;
    const std::string d = dlg.GetPath().utf8_string();
    if (std::find(dirs_full_.begin(), dirs_full_.end(), d) == dirs_full_.end()) dirs_full_.push_back(d);
    approve_path(d);
    refresh_proto_lists();
    changed();
    maybe_load_schema(false);
  }

  // ------------------------------------------------------------------ metadata

  Metadata metadata() const {
    if (!md_) return md_model_;
    Metadata md;
    for (int r = 0; r < md_->GetNumberRows(); ++r) {
      std::string k = md_->GetCellValue(r, 0).Trim().Trim(false).utf8_string();
      if (k.empty()) continue;
      md.emplace_back(std::move(k), md_->GetCellValue(r, 1).utf8_string());
    }
    return md;
  }

  void set_metadata(const Metadata& md) {
    md_model_ = md;
    if (!md_) {
      tabs_->set_badge(1, md.empty() ? wxString() : wxString::Format("%zu", md.size()));
      return;
    }
    if (md_->GetNumberRows() > 0) md_->DeleteRows(0, md_->GetNumberRows());
    md_->AppendRows(static_cast<int>(md.size()) + 1);
    for (size_t i = 0; i < md.size(); ++i) {
      md_->SetCellValue(static_cast<int>(i), 0, wxString::FromUTF8(md[i].first));
      md_->SetCellValue(static_cast<int>(i), 1, wxString::FromUTF8(md[i].second));
    }
    update_md_tab();
  }

  void delete_md_row(int row) {
    md_->DeleteRows(row);
    ensure_blank_row();
    update_md_tab();
    changed();
  }

  void ensure_blank_row() {
    if (!md_) return;
    const int n = md_->GetNumberRows();
    if (n == 0 || !md_->GetCellValue(n - 1, 0).IsEmpty() || !md_->GetCellValue(n - 1, 1).IsEmpty())
      md_->AppendRows(1);
    for (int r = 0; r < md_->GetNumberRows(); ++r)
      md_->SetCellValue(r, kMdDeleteCol, r + 1 < md_->GetNumberRows() ? wxString::FromUTF8("\xC3\x97") : wxString());
  }

  void update_md_tab() {
    ensure_blank_row_labels();
    const size_t n = metadata().size();
    int orphans = 0;
    if (md_) {
      for (int r = 0; r < md_->GetNumberRows(); ++r)
        if (md_->GetCellValue(r, 0).Trim().Trim(false).IsEmpty() && !md_->GetCellValue(r, 1).IsEmpty()) ++orphans;
      md_hint_->SetForegroundColour(orphans ? th_.error : th_.muted);
      md_hint_->set_text(orphans ? wxString::Format(wxString::FromUTF8("\xE2\x9A\xA0 %d row%s with a value but no key "
                                                                       "%s not sent. Type a key or delete the row."),
                                                    orphans, orphans == 1 ? "" : "s", orphans == 1 ? "is" : "are")
                                 : md_hint_text());
      md_hint_->Refresh();
    }
    tabs_->set_badge(1, n || orphans ? wxString::Format("%zu", n) : wxString(), orphans > 0);
  }

  static wxString md_hint_text() {
    return wxString::FromUTF8("Sent as request headers on every call and on reflection. Keys ending in -bin take "
                              "base64 values. Tab / Enter move to the next field; Delete, Ctrl+Backspace or "
                              "\xC3\x97 removes a row.");
  }

  // Keyboard path through the grid: Key -> Value -> next row's Key (Tab and Enter), backwards
  // with Shift. The x column is never on it.
  void on_md_key(wxKeyEvent& e) {
    const int code = e.GetKeyCode();
    const bool enter = code == WXK_RETURN || code == WXK_NUMPAD_ENTER;
    if (enter && !e.ControlDown() && !e.AltDown()) {
      if (!md_->IsCellEditControlEnabled() && !e.ShiftDown()) {
        md_fix_cursor();
        md_->EnableCellEditControl();
        return;
      }
      md_move(!e.ShiftDown(), /*enter=*/true);
      return;
    }
    if (code == WXK_TAB && !e.ControlDown() && !e.AltDown()) {
      md_move(!e.ShiftDown());
      return;
    }
    if (!md_->IsCellEditControlEnabled() &&
        (code == WXK_DELETE || code == WXK_NUMPAD_DELETE || (code == WXK_BACK && e.ControlDown()))) {
      const int r = md_->GetGridCursorRow();
      if (r >= 0 && r + 1 < md_->GetNumberRows()) {
        const int c = std::min(md_->GetGridCursorCol(), 1);
        delete_md_row(r);
        md_go(std::min(r, md_->GetNumberRows() - 1), std::max(c, 0), false);
      }
      return;
    }
    // Arrows move the cursor here: wxGrid first offers its keys to the parent chain, where the
    // shell's handlers would otherwise take them.
    if (!md_->IsCellEditControlEnabled() && !e.ControlDown() && !e.AltDown()) {
      const bool ext = e.ShiftDown();
      switch (code) {
        case WXK_UP: case WXK_NUMPAD_UP: md_->MoveCursorUp(ext); return;
        case WXK_DOWN: case WXK_NUMPAD_DOWN: md_->MoveCursorDown(ext); return;
        case WXK_LEFT: case WXK_NUMPAD_LEFT: md_->MoveCursorLeft(ext); return;
        case WXK_RIGHT: case WXK_NUMPAD_RIGHT:
          if (md_->GetGridCursorCol() < 1) md_->MoveCursorRight(ext);
          return;
        case WXK_F2: md_fix_cursor(); md_->EnableCellEditControl(); return;
        default: break;
      }
    }
    e.Skip();
  }

  void md_fix_cursor() {
    if (md_->GetGridCursorCol() == kMdDeleteCol) md_->SetGridCursor(std::max(md_->GetGridCursorRow(), 0), 1);
  }

  void md_move(bool forward, bool enter = false) {
    int r = std::max(md_->GetGridCursorRow(), 0);
    int c = std::min(std::max(md_->GetGridCursorCol(), 0), 1);
    // Commit first: this fires CELL_CHANGED, which appends the next blank row when needed.
    if (md_->IsCellEditControlEnabled()) md_->DisableCellEditControl();
    const int rows = md_->GetNumberRows();
    const bool blank = md_->GetCellValue(r, 0).IsEmpty() && md_->GetCellValue(r, 1).IsEmpty();
    if (forward) {
      if (c == 0) {
        if (enter && blank) return;  // Enter on the empty last row: nothing to commit, stay
        c = 1;
      } else if (r + 1 < rows) {
        ++r;
        c = 0;
      } else {
        if (enter) return;
        md_->Navigate(wxNavigationKeyEvent::IsForward);  // Tab past the last (blank) row
        return;
      }
    } else {
      if (c == 1) {
        c = 0;
      } else if (r > 0) {
        --r;
        c = 1;
      } else {
        if (!enter) md_->Navigate(wxNavigationKeyEvent::IsBackward);
        return;
      }
    }
    md_go(r, c, true);
  }

  void md_go(int r, int c, bool edit) {
    md_->SetGridCursor(r, c);
    md_->MakeCellVisible(r, c);
    if (!edit) return;
    // Open the editor once the old one has finished handling its key.
    CallAfter([this, r, c] {
      if (!md_ || md_->IsCellEditControlEnabled()) return;
      if (md_->GetGridCursorRow() != r || md_->GetGridCursorCol() != c) return;
      wxWindow* f = wxWindow::FindFocus();
      if (!f || (f != md_ && !md_->IsDescendant(f))) return;
      md_->EnableCellEditControl();
    });
  }

  void ensure_blank_row_labels() {
    if (!md_) return;
    for (int r = 0; r < md_->GetNumberRows(); ++r) {
      const bool last = r + 1 == md_->GetNumberRows();
      const wxString want = last ? wxString() : wxString::FromUTF8("\xC3\x97");
      if (md_->GetCellValue(r, kMdDeleteCol) != want) md_->SetCellValue(r, kMdDeleteCol, want);
    }
  }

  // ------------------------------------------------------------------ editor helpers

  // The JSON status keeps its full width while it fits next to the buttons (then it ellipsizes),
  // so the notes give way first and Format / Template are never clipped, however narrow the
  // page is (schema pane open) or long the status (a 12 MB body, a JSON error).
  void set_status_label(const wxString& text) {
    json_status_->SetLabel(text);
    fit_status();
  }

  void fit_status() {
    wxWindow* page = json_status_->GetParent();
    const int w = page->GetClientSize().x;
    if (w <= 0) return;
    // Measured from the text itself: the control's best size is unreliable for an ellipsized
    // label that was last laid out narrower.
    const wxString label = json_status_->GetLabel();
    const int best = label.empty() ? 0 : json_status_->GetTextExtent(label).x + FromDIP(4);
    const int fixed = format_->GetBestSize().x + template_->GetBestSize().x + FromDIP(20) /* notes min */ +
                      FromDIP(10 + 6 + 2) /* borders */;
    const int cap = std::max(FromDIP(40), std::min(w - fixed, w * 7 / 10));
    json_status_->SetMinSize(wxSize(std::min(best, cap), -1));
    json_status_->SetSize(wxSize(std::min(best, cap), -1));
    page->Layout();
  }

  // Squiggles the JSON syntax error at byte offset `at` (npos clears); a click on the status
  // label jumps there.
  void mark_error(size_t at) {
    const int len = body_->GetLength();
    body_->SetIndicatorCurrent(kErrIndicator);
    if (error_at_ >= 0) body_->IndicatorClearRange(0, len);
    error_at_ = -1;
    if (at == std::string::npos) {
      json_status_->SetCursor(wxNullCursor);
      return;
    }
    const int pos = std::min(static_cast<int>(std::min<size_t>(at, static_cast<size_t>(len))), len);
    int from = pos, to = std::min(len, pos + 1);
    if (from == len && len > 0) from = len - 1;  // unexpected end: mark the last character
    if (to <= from) to = std::min(len, from + 1);
    body_->IndicatorFillRange(from, to - from);
    error_at_ = pos;
    json_status_->SetCursor(wxCursor(wxCURSOR_HAND));
  }

  void check_json() {
    if (doc_pending_) return;
    if (stream_mode_ && cur_ >= 0) {
      // keep the list preview of the message being edited current (cheap: first 400 bytes)
      const unsigned i = static_cast<unsigned>(cur_);
      if (msgs_ && i < msgs_->GetCount()) {
        const wxString l = message_label(i);
        if (msgs_->GetString(i) != l) msgs_->SetString(i, l);
      }
    }
    const size_t len = static_cast<size_t>(body_->GetLength());
    // Scroll width tracking only ever grows: after most of the text went, measure again (no
    // leftover horizontal scrollbar under a three-line message).
    if (len * 2 < checked_len_) body_->SetScrollWidth(1);
    checked_len_ = len;
    // Back to colouring once a huge document has shrunk (hysteresis, see kLexBytes).
    if (plain_ && len < kLexBytes && longest_line(body_->GetCharacterPointer(), len, kLexLine) < kLexLine)
      set_plain(false);
    if (len > kValidateMax) {
      json_status_->SetForegroundColour(th_.muted);
      mark_error(std::string::npos);
      set_status_label(wxString::FromUTF8(human_size(len) + " · checked on send"));
      json_status_->SetToolTip("Large message: JSON is checked when it is sent; syntax colouring and folding are "
                               "off above 1 MB so editing stays instant.");
    } else {
      const std::string text = body_text();
      size_t at = std::string::npos;
      const std::string p = json_problem(text, &at);
      mark_error(p.empty() || p == "empty" ? std::string::npos : at);
      if (p.empty()) {
        json_status_->SetForegroundColour(th_.ok);
        set_status_label(wxString::FromUTF8("✓ JSON" + std::string(len > (64u << 10) ? "  " + human_size(len) : "")));
        json_status_->SetToolTip(wxString::FromUTF8(human_size(len)));
      } else if (p == "empty") {
        json_status_->SetForegroundColour(th_.muted);
        set_status_label(stream_mode_ ? "empty message" : "empty (sends the default message)");
        json_status_->SetToolTip("");
      } else {
        json_status_->SetForegroundColour(th_.error);
        set_status_label(wxString::FromUTF8("✗ " + p));
        json_status_->SetToolTip(wxString::FromUTF8(p));
      }
    }
    if (auto* p = json_status_->GetParent()) p->Layout();
  }

  void match_braces() {
    const int pos = body_->GetCurrentPos();
    auto is_brace = [](int c) { return c == '{' || c == '}' || c == '[' || c == ']'; };
    int at = -1;
    if (pos > 0 && is_brace(body_->GetCharAt(pos - 1))) at = pos - 1;
    else if (is_brace(body_->GetCharAt(pos))) at = pos;
    if (at < 0) {
      if (brace_lit_) body_->BraceHighlight(wxSTC_INVALID_POSITION, wxSTC_INVALID_POSITION);
      brace_lit_ = false;
      return;
    }
    const int other = body_->BraceMatch(at);
    if (other == wxSTC_INVALID_POSITION) body_->BraceBadLight(at);
    else body_->BraceHighlight(at, other);
    brace_lit_ = true;
  }

  // The vertical scrollbar only while there is something to scroll: a 4-line message has no
  // scroll thumb running down the whole editor.
  void fit_vscroll() {
    const bool need = body_->GetFirstVisibleLine() > 0 || body_->GetLineCount() > body_->LinesOnScreen();
    if (need == vscroll_on_) return;
    vscroll_on_ = need;
    body_->SetUseVerticalScrollBar(need);
  }

  // Is pos inside a JSON string on its line? 1 inside, 0 outside, -1 unknown (a long line is
  // never scanned: no per-character work on a 10 MB single-line body).
  int string_state(int pos) {
    const int begin = body_->PositionFromLine(body_->LineFromPosition(pos));
    if (pos - begin > 4096) return -1;
    if (pos <= begin) return 0;
    const wxCharBuffer buf = body_->GetTextRangeRaw(begin, pos);
    bool in = false;
    for (const char* c = buf.data(); c && *c; ++c) {
      if (in && *c == '\\') {
        if (c[1]) ++c;
      } else if (*c == '"') {
        in = !in;
      }
    }
    return in ? 1 : 0;
  }

  // ch ({ [ ") was just typed: add its closer after the caret when that reads as the start of
  // a value or key, not inside a string or right before a word.
  void auto_close(int ch) {
    const int pos = body_->GetCurrentPos();
    const int next = body_->GetCharAt(pos);
    const bool free_after = next == 0 || next == ' ' || next == '\t' || next == '\r' || next == '\n' ||
                            next == ',' || next == '}' || next == ']' || next == ':';
    if (!free_after) return;
    if (string_state(pos - 1) != 0) return;  // before the typed character
    if (ch == '"') {
      const int prev = pos >= 2 ? body_->GetCharAt(pos - 2) : 0;
      if ((prev >= 0 && prev < 128 && std::isalnum(prev)) || prev == '"' || prev == '_') return;
    }
    const char closer = ch == '{' ? '}' : ch == '[' ? ']' : '"';
    body_->InsertText(pos, wxString(closer));
  }

  void auto_indent() {
    const int line = body_->GetCurrentLine();
    if (line <= 0) return;
    int indent = body_->GetLineIndentation(line - 1);
    const int end = body_->GetLineEndPosition(line - 1);
    const int begin = body_->PositionFromLine(line - 1);
    int p = end - 1;
    // Only the tail of the previous line matters (a 10 MB line is never walked).
    while (p >= begin && end - p < 256 && (body_->GetCharAt(p) == ' ' || body_->GetCharAt(p) == '\t')) --p;
    const int last = p >= begin ? body_->GetCharAt(p) : 0;
    if (last == '{' || last == '[') indent += 2;
    // "{|}" + Enter: put the closing brace on its own line
    const int next = body_->GetCharAt(body_->GetCurrentPos());
    body_->SetLineIndentation(line, indent);
    body_->GotoPos(body_->GetLineIndentPosition(line));
    if ((last == '{' && next == '}') || (last == '[' && next == ']')) {
      const int here = body_->GetCurrentPos();
      body_->InsertText(here, "\n");
      body_->SetLineIndentation(line + 1, std::max(0, indent - 2));
      body_->GotoPos(here);
    }
  }

  void format_body() {
    if (doc_pending_) return;
    const size_t len = static_cast<size_t>(body_->GetLength());
    if (len == 0) return;
    if (len < kAsyncDoc) {
      const std::string text = body_text();
      if (trim_empty(text)) return;
      const std::string pretty = json_pretty(text);
      if (pretty == text) return;
      const int line = body_->GetFirstVisibleLine();
      set_plain(wants_plain(pretty.data(), pretty.size()));
      body_->BeginUndoAction();
      body_->SetTextRaw(pretty.c_str());
      body_->EndUndoAction();
      body_->SetFirstVisibleLine(line);
      body_->SetXOffset(0);
      fit_line_margin(body_);
      check_json();
      return;
    }
    auto text = std::make_shared<const std::string>(body_text());
    ++load_gen_;
    doc_pending_ = true;
    body_->SetReadOnly(true);
    json_status_->SetForegroundColour(th_.muted);
    set_status_label(wxString::FromUTF8("Formatting " + human_size(len) + "\xE2\x80\xA6"));
    start_doc_job(DocKind::Format, std::move(text), {}, {}, {}, 0, body_->GetFirstVisibleLine());
  }

  // ------------------------------------------------------------------ streaming by hand

  void send_messages(bool all) {
    sync_current();
    if (!in_flight_) host_->send();  // opens the stream; messages wait until it is open
    if (!in_flight_) return;         // could not start (no address / method)
    if (all) {
      for (size_t i = 0; i < messages_.size(); ++i)
        if (!sent_[i] && std::find(pending_sends_.begin(), pending_sends_.end(), static_cast<int>(i)) == pending_sends_.end())
          pending_sends_.push_back(static_cast<int>(i));
    } else {
      pending_sends_.push_back(cur_);
    }
    pump_started_ = Clock::now();
    pump_sends();
    if (!all && cur_ + 1 < static_cast<int>(messages_.size())) {
      select_message(cur_ + 1);
    } else if (!all && in_flight_ && interactive() && cur_ >= 0) {
      // The last message went out: move on to a new draft (a copy, ready to edit and send) so
      // the button never offers to resend what was just sent.
      const int sent_no = cur_ + 1;
      messages_.push_back(messages_[static_cast<size_t>(cur_)]);
      sent_.push_back(0);
      select_message(static_cast<int>(messages_.size()) - 1);
      stream_hint_->SetLabel(wxString::Format("#%d sent; editing new #%d", sent_no, cur_ + 1));
      stream_hint_->SetToolTip("Edit the draft and send it, or End the stream");
      stream_bar_->Layout();
    }
  }

  void pump_sends() {
    while (!pending_sends_.empty()) {
      const int i = pending_sends_.front();
      if (i < 0 || static_cast<size_t>(i) >= messages_.size()) {
        pending_sends_.pop_front();
        continue;
      }
      if (i == cur_) sync_current();
      std::string err;
      if (host_->stream_send(messages_[static_cast<size_t>(i)], &err)) {
        sent_[static_cast<size_t>(i)] = 1;
        pending_sends_.pop_front();
        refresh_message_list();
        continue;
      }
      // The call is still connecting on the tab's worker: try again shortly.
      if (in_flight_ && err == "No stream is open." && ms_between(pump_started_, Clock::now()) < 20000) {
        if (!pump_timer_.IsRunning()) pump_timer_.Start(10);
        return;
      }
      json_status_->SetForegroundColour(th_.error);
      set_status_label(wxString::Format("Message %d not sent: %s", i + 1, wxString::FromUTF8(err)));
      json_status_->SetToolTip(wxString::FromUTF8(err));
      json_status_->GetParent()->Layout();
      pending_sends_.clear();
    }
    pump_timer_.Stop();
  }

  // ------------------------------------------------------------------ grpcurl

  static std::string clipboard_text(size_t max_chars) {
    std::string out;
    if (!wxTheClipboard->Open()) return out;
    if (wxTheClipboard->IsSupported(wxDF_UNICODETEXT)) {
      wxTextDataObject data;
      if (wxTheClipboard->GetData(data)) {
        const wxString t = data.GetText();
        if (t.length() <= max_chars) out = t.utf8_string();
      }
    }
    wxTheClipboard->Close();
    return out;
  }

  static bool looks_like_grpcurl(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return false;
    std::string head = s.substr(b, 12);
    for (auto& c : head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return head.rfind("grpcurl ", 0) == 0 || head.rfind("grpcurl.exe ", 0) == 0;
  }

  void show_grpcurl_menu() {
    wxMenu menu;
    menu.Append(1, "Copy as grpcurl");
    menu.Append(2, "Import grpcurl command from clipboard");
    const wxPoint at = grpcurl_->GetPosition() + wxPoint(0, grpcurl_->GetSize().y);
    const int sel = top_->GetPopupMenuSelectionFromUser(menu, at);
    if (sel == 1) copy_grpcurl();
    else if (sel == 2) {
      const std::string clip = clipboard_text(64u << 20);
      if (!import_grpcurl(clip)) {
        json_status_->SetForegroundColour(th_.error);
        set_status_label("The clipboard has no grpcurl command");
      }
    }
  }

  bool import_grpcurl(const std::string& cmd) {
    GrpcurlImport g;
    std::string err;
    if (!parse_grpcurl(cmd, g, &err)) {
      if (looks_like_grpcurl(cmd)) {
        json_status_->SetForegroundColour(th_.error);
        set_status_label(wxString::FromUTF8("grpcurl: " + err));
      }
      return false;
    }
    loading_ = true;
    std::string a = g.address;
    bool tls = g.tls;
    strip_scheme(a, &tls);
    addr_->ChangeValue(wxString::FromUTF8(a));
    addr_->SetInsertionPointEnd();
    tls_->set_checked(tls);
    update_tls_label();
    pull_options();
    opt_t_.insecure_skip_verify = g.insecure;
    opt_t_.authority = g.authority;
    if (g.deadline_ms >= 0) opt_o_.deadline_ms = g.deadline_ms;
    push_options();
    set_metadata(g.metadata);
    ensure_blank_row();
    files_full_ = g.protos;
    dirs_full_ = g.import_paths;
    source_->SetSelection(files_full_.empty() ? kSourceReflection : kSourceProto);
    refresh_proto_lists();
    update_source_ui();
    if (!g.method.empty() && g.method != method_) {
      method_ = g.method;
      keys_method_.clear();
      if (const GrpcMethodInfo* m = find_method(method_)) stream_mode_ = client_streams(m->kind);
    }
    if (g.has_data) set_body(g.data);
    loading_ = false;
    tree_->select(method_);
    update_header();
    update_stream_ui();
    update_send_label();
    update_options_tab();
    std::string note = "Imported grpcurl command";
    if (!g.warnings.empty()) note += " (" + std::to_string(g.warnings.size()) + " note" + (g.warnings.size() == 1 ? "" : "s") + ")";
    std::string all;
    for (const auto& w : g.warnings) all += (all.empty() ? "" : "\n") + w;
    json_status_->SetForegroundColour(th_.muted);
    set_status_label(wxString::FromUTF8(note));
    json_status_->SetToolTip(wxString::FromUTF8(all));
    changed();
    maybe_load_schema(true);
    return true;
  }

  // ------------------------------------------------------------------ misc

  void copy_grpcurl() {
    SavedRequest r = base_;
    store(r);
    if (const Workspace* ws = host_->workspace()) {
      if (std::filesystem::path f = host_->inherit_file(); !f.empty()) r.file = f;  // its folders' metadata / vars
      r = ws->expanded(r);
    }
    const std::string cmd = to_grpcurl_command(r);
    if (wxTheClipboard->Open()) {
      wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(cmd)));
      wxTheClipboard->Close();
      grpcurl_->SetLabel(wxString::FromUTF8("✓ Copied"));
      grpcurl_->SetToolTip(wxString::FromUTF8(cmd.size() > 4000 ? cmd.substr(0, 4000) + "..." : cmd));
      flash_timer_.StartOnce(1500);
    }
  }

  // ------------------------------------------------------------------ self test
  // WATT_GRPCED_SELFTEST=<file>: the first gRPC editor runs the worker-built document checks
  // (paste / format of big texts, then edit, validate, undo across the swap), writes PASS / FAIL
  // lines to <file> and quits the app. Drives the same code paths as the keyboard does.

  void maybe_self_test() {
    static bool ran = false;
    wxString out;
    if (ran || !wxGetEnv("WATT_GRPCED_SELFTEST", &out) || out.empty()) return;
    ran = true;
    st_out_ = out.ToStdWstring();
    st_log_ = "INFO " + ctor_times_ + "\n";
    st_fail_ = 0;
    CallAfter([this] { st_step(0); });
  }

  void st_check(bool ok, const std::string& what) {
    st_log_ += (ok ? "PASS " : "FAIL ") + what + "\n";
    if (!ok) ++st_fail_;
  }

  void st_key(int code, int mods) {
    wxKeyEvent ev(wxEVT_KEY_DOWN);
    ev.m_keyCode = code;
    ev.m_controlDown = (mods & wxMOD_CONTROL) != 0;
    ev.m_shiftDown = (mods & wxMOD_SHIFT) != 0;
    ev.SetEventObject(body_);
    body_->GetEventHandler()->ProcessEvent(ev);
  }

  std::string st_status() const { return json_status_->GetLabel().utf8_string(); }

  // Runs step `i` once no document is being built.
  void st_next(int i) {
    CallAfter([this, i, alive = alive_] {
      if (!*alive) return;
      if (doc_pending_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        st_next(i);
        return;
      }
      st_step(i);
    });
  }

  void st_step(int i) {
    switch (i) {
      case 0: {
        set_editor_text("{\"a\": 1}");
        std::string big = "{\"items\": [\n";
        for (int n = 0; big.size() < 100u << 10; ++n)
          big += std::string(n ? ",\n" : "") + "  {\"id\": " + std::to_string(n) + ", \"name\": \"item number " +
                 std::to_string(n) + "\"}";
        big += "\n]}";
        st_big_ = big;
        body_->SelectAll();
        start_paste(wxString::FromUTF8(big).ToStdWstring());
        st_check(doc_pending_, "a 100 KB paste is built on a worker");
        st_next(1);
        return;
      }
      case 1: {
        st_check(body_text() == st_big_, "pasted text is in the editor");
        st_check(!body_->GetReadOnly(), "editor writable after the paste");
        st_check(!body_->CanUndo(), "new document starts with an empty undo history");
        st_check(st_status().find("JSON") != std::string::npos, "footer validates the paste: " + st_status());
        const uint64_t gen = body_gen_;
        body_->SelectAll();
        body_->ReplaceSelection("{\"message\": \"a\"} ZZ");
        st_check(body_gen_ != gen, "the edit after the paste is seen (change handler)");
        st_check(body_->GetModify(), "GetModify() after the edit");
        st_check(body_->CanUndo(), "CanUndo() after the edit");
        st_check(json_timer_.IsRunning(), "validation scheduled after the edit");
        check_json();
        st_check(st_status().find("\xE2\x9C\x97") != std::string::npos, "invalid body is flagged: " + st_status());
        st_key('Z', wxMOD_CONTROL);
        st_check(body_text() == st_big_, "Ctrl+Z undoes the edit");
        check_json();
        st_check(st_status().find("\xE2\x9C\x93") != std::string::npos, "valid again after undo: " + st_status());
        st_key('Z', wxMOD_CONTROL);
        st_check(body_text() == "{\"a\": 1}", "Ctrl+Z again undoes the paste itself: " + body_text().substr(0, 40));
        st_check(body_->GetSelectionStart() == 0 && body_->GetSelectionEnd() == body_->GetLength(),
                 "selection before the paste restored: " + std::to_string(body_->GetSelectionStart()) + "-" +
                     std::to_string(body_->GetSelectionEnd()));
        st_key('Y', wxMOD_CONTROL);
        st_check(body_text() == st_big_, "Ctrl+Y redoes the paste");
        st_key('Z', wxMOD_CONTROL);
        body_->AddText("x");
        st_check(!redo_doc_.doc, "a new edit drops the redo document");
        // Format of a big minified body (worker-built too).
        std::string mini = "[";
        for (int n = 0; mini.size() < 300u << 10; ++n) mini += std::string(n ? "," : "") + "{\"k\":" + std::to_string(n) + "}";
        mini += "]";
        st_big_ = mini;
        set_editor_text(mini);
        st_next(2);
        return;
      }
      case 2: {
        st_check(body_->GetLength() > 0 && !body_->GetReadOnly(), "300 KB load finished");
        format_body();
        st_check(doc_pending_, "format of 300 KB runs on a worker");
        st_next(3);
        return;
      }
      case 3: {
        const std::string pretty = body_text();
        st_check(pretty.size() > st_big_.size() && pretty.find("\n  {") != std::string::npos, "formatted");
        body_->GotoPos(body_->GetLength());
        body_->AddText(" ZZ");
        st_check(body_->GetModify() && body_->CanUndo(), "edit after format is recorded");
        check_json();
        st_check(st_status().find("\xE2\x9C\x97") != std::string::npos, "invalid after format + edit: " + st_status());
        st_key('Z', wxMOD_CONTROL);
        st_check(body_text() == pretty, "Ctrl+Z undoes the edit after format");
        st_key('Z', wxMOD_CONTROL);
        std::string back = body_text();
        back.erase(std::remove_if(back.begin(), back.end(), [](char c) { return c == '\n' || c == ' '; }), back.end());
        st_check(back == st_big_, "Ctrl+Z again restores the unformatted body");
        st_finish();
        return;
      }
      default:
        st_finish();
    }
  }

  // WATT_GRPCED_TYPETEST=<file>: real keystrokes (SendInput, Shift held for '"', '{', '}', ':',
  // '_') type {"count": 5, "interval_ms": 50} into the message at 50, 90 and 200 ms per key,
  // through auto-close, step-over and the key list; the buffer must equal it exactly and parse.
  // Run it on a workspace whose first gRPC tab has a method with a loaded schema (so the key list
  // opens) and keep the window in the foreground. PASS / FAIL lines to <file>, then exit 0 / 1.
  void maybe_type_test() {
    wxString out;
    if (!wxGetEnv("WATT_GRPCED_TYPETEST", &out) || out.empty()) return;
    st_out_ = out.ToStdWstring();
    st_log_.clear();
    st_fail_ = 0;
    tt_wait_ = 0;
    tt_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tt_tick(); });
    tt_timer_.StartOnce(300);
  }

  static void tt_send_key(int ch) {
    const SHORT s = ::VkKeyScanW(static_cast<wchar_t>(ch));
    const bool shift = (s >> 8) & 1;
    const WORD vk = static_cast<WORD>(s & 0xff);
    INPUT in[4]{};
    int n = 0;
    auto add = [&](WORD k, bool up) {
      in[n].type = INPUT_KEYBOARD;
      in[n].ki.wVk = k;
      in[n].ki.wScan = static_cast<WORD>(::MapVirtualKeyW(k, MAPVK_VK_TO_VSC));
      in[n].ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
      ++n;
    };
    if (shift) add(VK_SHIFT, false);
    add(vk, false);
    add(vk, true);
    if (shift) add(VK_SHIFT, true);
    ::SendInput(static_cast<UINT>(n), in, sizeof(INPUT));
  }

  void tt_tick() {
    static const std::string want = "{\"count\": 5, \"interval_ms\": 50}";
    static const int speeds[] = {50, 90, 200};
    // Every gRPC editor waits; the first one on screen with a loaded method (the key list needs
    // its fields) runs the test.
    static bool claimed = false;
    if (tt_run_ < 0) {
      if (claimed) return;
      const bool ready = IsShownOnScreen() && find_method(method_);
      if (!ready && ++tt_wait_ < 150) {
        tt_timer_.StartOnce(100);
        return;
      }
      claimed = true;
      st_check(ready, "a gRPC tab on screen with a loaded method: " + method_);
      tt_run_ = 0;
      tt_pos_ = -1;
    }
    if (tt_run_ >= static_cast<int>(std::size(speeds))) {
      st_finish();
      return;
    }
    if (tt_pos_ < 0) {  // start a run: foreground, focus, empty message
      if (wxWindow* top = wxGetTopLevelParent(this)) {
        const HWND h = static_cast<HWND>(top->GetHWND());
        if (::GetForegroundWindow() != h && ++tt_wait_ < 200) {  // SendInput goes to the foreground
          // An Alt tap lets SetForegroundWindow succeed while another app is in front.
          INPUT alt[2]{};
          alt[0].type = alt[1].type = INPUT_KEYBOARD;
          alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
          alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
          ::SendInput(2, alt, sizeof(INPUT));
          top->Raise();
          ::SetForegroundWindow(h);
          tt_timer_.StartOnce(100);
          return;
        }
      }
      body_->SetFocus();
      body_->SelectAll();
      body_->Clear();
      tt_popup_ = false;
      tt_pos_ = 0;
      tt_timer_.StartOnce(150);
      return;
    }
    if (body_->AutoCompActive()) tt_popup_ = true;
    if (tt_pos_ < static_cast<int>(want.size())) {
      tt_send_key(static_cast<unsigned char>(want[static_cast<size_t>(tt_pos_++)]));
      tt_timer_.StartOnce(speeds[tt_run_]);
      return;
    }
    if (tt_pos_ == static_cast<int>(want.size())) {  // let the last keys land
      ++tt_pos_;
      tt_timer_.StartOnce(400);
      return;
    }
    const std::string got = body_text();
    const std::string ms = std::to_string(speeds[tt_run_]) + " ms/key";
    st_check(got == want, ms + ": typed text exact: " + got);
    size_t at = std::string::npos;
    st_check(json_problem(got, &at).empty(), ms + ": parses");
    st_check(tt_popup_, ms + ": key list opened while typing");
    ++tt_run_;
    tt_pos_ = -1;
    tt_timer_.StartOnce(100);
  }

  void st_finish() {
    st_log_ += "INFO " + load_times_ + "\n";
    st_log_ += st_fail_ ? "RESULT FAIL\n" : "RESULT PASS\n";
    {
      std::ofstream f(std::filesystem::path(st_out_), std::ios::binary);
      f << st_log_;
    }
    // Exit code 0 / 1 for scripts (a test run: no save prompts, no shutdown of the other tabs).
    if (!wxGetEnv("WATT_GRPCED_SELFTEST_STAY", nullptr)) ::TerminateProcess(::GetCurrentProcess(), st_fail_ ? 1 : 0);
  }

  void on_char_hook(wxKeyEvent& e) {
    const int k = e.GetKeyCode();
    if (k == 'F' && e.GetModifiers() == wxMOD_CONTROL) {
      if (!split_->IsSplit()) toggle_schema_pane();
      tree_->focus_filter();
      return;
    }
    if (k == 'F' && e.GetModifiers() == (wxMOD_SHIFT | wxMOD_ALT)) {
      format_body();
      return;
    }
    if ((k == WXK_RETURN || k == WXK_NUMPAD_ENTER) && (e.GetModifiers() & wxMOD_CONTROL) && web_blocks()) {
      update_web_ui();
      json_status_->SetForegroundColour(th_.error);
      set_status_label("Not sent: gRPC-Web has no client or bidi streaming");
      return;
    }
    if ((k == WXK_RETURN || k == WXK_NUMPAD_ENTER) && e.GetModifiers() == (wxMOD_CONTROL | wxMOD_SHIFT) &&
        interactive()) {
      send_messages(false);
      return;
    }
    e.Skip();
  }

  EditorHost* host_;
  Theme th_;
  std::shared_ptr<int> alive_ = std::make_shared<int>(1);
  std::shared_ptr<DocJobs> jobs_ = std::make_shared<DocJobs>();

  wxPanel* top_ = nullptr;
  FlatButton* schema_btn_ = nullptr;
  FlatButton* tls_ = nullptr;
  FlatButton* web_ = nullptr;
  wxTextCtrl* addr_ = nullptr;
  FlatButton* send_ = nullptr;
  FlatButton* grpcurl_ = nullptr;
  wxSplitterWindow* split_ = nullptr;
  int sash_ = 0;
  bool pane_auto_hidden_ = false;

  wxPanel* left_ = nullptr;
  ChoiceButton* source_ = nullptr;
  FlatButton* reload_ = nullptr;
  wxPanel* proto_box_ = nullptr;
  wxListBox* files_ = nullptr;
  wxListBox* dirs_ = nullptr;
  std::vector<std::string> files_full_, dirs_full_;
  WrapLabel* schema_status_ = nullptr;
  grpced::MethodTree* tree_ = nullptr;

  wxPanel* right_ = nullptr;
  wxStaticText* badge_ = nullptr;
  FlatButton* method_btn_ = nullptr;
  wxStaticText* io_lbl_ = nullptr;
  FlatButton* def_btn_ = nullptr;
  TabStrip* tabs_ = nullptr;
  wxSimplebook* book_ = nullptr;
  wxPanel* stream_bar_ = nullptr;  // null until ensure_stream_ui()
  wxWindow* msg_page_ = nullptr;
  wxBoxSizer* page_sz_ = nullptr;
  wxBoxSizer* mid_sz_ = nullptr;
  CheckBox* manual_ = nullptr;
  wxStaticText* stream_hint_ = nullptr;
  FlatButton* send_one_ = nullptr;
  FlatButton* send_all_ = nullptr;
  FlatButton* end_ = nullptr;
  wxPanel* msg_col_ = nullptr;
  MessageList* msgs_ = nullptr;
  FlatButton *add_msg_ = nullptr, *dup_msg_ = nullptr, *del_msg_ = nullptr, *up_msg_ = nullptr, *down_msg_ = nullptr;
  wxStyledTextCtrl* body_ = nullptr;
  wxStyledTextCtrl* factory_ = nullptr;  // see factory()
  wxStaticText* json_status_ = nullptr;
  int error_at_ = -1;  // position of the squiggled JSON error, -1 none
  wxStaticText* notes_ = nullptr;
  FlatButton* format_ = nullptr;
  FlatButton* template_ = nullptr;
  wxGrid* md_ = nullptr;
  WrapLabel* md_hint_ = nullptr;
  wxSpinCtrlDouble* deadline_ = nullptr;
  wxTextCtrl* authority_ = nullptr;
  CheckBox* skip_verify_ = nullptr;
  wxSpinCtrlDouble* connect_timeout_ = nullptr;
  wxSpinCtrlDouble* keepalive_ = nullptr;
  wxSpinCtrlDouble* max_msg_ = nullptr;
  wxTextCtrl* user_agent_ = nullptr;
  wxTextCtrl* server_name_ = nullptr;
  CheckBox* proto_names_ = nullptr;
  CheckBox* emit_defaults_ = nullptr;
  CheckBox* allow_unknown_ = nullptr;
  // Options and metadata live here; their pages are built when first shown (opening a tab
  // stays cheap) and read back into the model on every change.
  GrpcTarget opt_t_;
  GrpcCallOptions opt_o_;
  Metadata md_model_;
  bool opt_page_built_ = false;
  wxPanel* opt_holder_ = nullptr;
  wxPanel* md_holder_ = nullptr;
  struct Pem {
    std::wstring from;  // file name it was read from (empty: came with the request)
    FlatButton* pick = nullptr;
    FlatButton* clear = nullptr;
    wxStaticText* info = nullptr;
  };
  Pem pem_[3];  // CA, client certificate, client key
  std::string loaded_origin_;  // schema_origin() of schema_
  bool status_err_ = false;    // schema_status_ shows a load error

  SavedRequest base_;  // fields without UI here (certificates, channel options); body cleared
  std::string method_;
  std::shared_ptr<const GrpcSchema> schema_;  // the last schema that loaded (kept when a reload fails)
  std::string schema_key_;
  int pending_loads_ = 0;
  Clock::time_point load_start_;
  std::string load_what_;

  bool stream_mode_ = false;
  std::vector<std::string> messages_;  // stream mode: the list; messages_[cur_] is stale while editing
  std::vector<char> sent_;
  int cur_ = 0;
  std::map<std::string, std::string> body_by_method_;
  std::string last_template_;
  bool replaced_note_ = false;  // notes_ says "Replaced your text with the example"
  wxString replaced_before_;    // the method's note it replaced
  uint64_t body_gen_ = 0;
  bool brace_lit_ = false;

  // big documents
  bool plain_ = false;                               // no lexer / folding (huge document)
  bool margin_dirty_ = false;
  bool vscroll_on_ = true;
  int foot_w_ = -1;
  bool doc_pending_ = false;                         // a document is being built on a worker
  uint64_t load_gen_ = 0;
  std::shared_ptr<const std::string> pending_text_;  // the text being loaded (DocKind::Load)

  // completion
  std::string keys_method_;
  std::vector<std::string> keys_;
  std::string input_def_;

  std::deque<int> pending_sends_;
  Clock::time_point pump_started_;
  bool in_flight_ = false;
  bool loading_ = false;
  UndoDoc undo_doc_, redo_doc_;  // see swap_history()
  size_t checked_len_ = 0;       // body length at the last check_json()
  std::wstring st_out_;
  std::string st_log_, st_big_;  // self test (WATT_GRPCED_SELFTEST)
  int st_fail_ = 0;
  std::string ctor_times_, load_times_;  // construction / load() cost (self test INFO lines)

  wxTimer load_timer_, json_timer_, pump_timer_, flash_timer_;
  wxTimer tt_timer_;  // typing test (WATT_GRPCED_TYPETEST)
  int tt_run_ = -1, tt_pos_ = -1, tt_wait_ = 0;
  bool tt_popup_ = false;
};

}  // namespace

GrpcEditorPanel* make_grpc_editor(wxWindow* parent, EditorHost* host) {
  return new GrpcEditor(parent, host);
}

}  // namespace watt::app
