#include "app/response_body.h"
#include "app/response_filter.h"

#include <wx/app.h>
#include <wx/dcbuffer.h>
#include <wx/filedlg.h>
#include <wx/html/htmlwin.h>
#include <wx/menu.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/sizer.h>
#include <wx/stc/stc.h>
#include <wx/textctrl.h>
#include <wx/wupdlock.h>

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>

#include "app/response_widgets.h"

#ifdef _WIN32
#include <windows.h>
#endif

// wxHtmlWindow (the rendered HTML Preview) lives in wx's html library, which the watt target does
// not list (CMakeLists.txt: wx::core wx::base wx::stc wx::aui); link it from here.
#if defined(_MSC_VER)
#ifdef _DEBUG
#pragma comment(lib, "C:/vcpkg/installed/x64-windows/debug/lib/wxmsw33ud_html.lib")
#else
#pragma comment(lib, "C:/vcpkg/installed/x64-windows/lib/wxmsw33u_html.lib")
#endif
#endif

// The rendered Preview uses wxWebView's Edge (WebView2) backend, from wx's webview library (also
// not in the target's list). wx loads WebView2Loader.dll itself at run time; watt links it too
// (preview_runtime_present() calls into it) so the exe imports it and the build deploys the DLL
// next to watt.exe. Without the Edge runtime the Preview falls back to wxHtmlWindow.
#if defined(_WIN32) && wxUSE_WEBVIEW && wxUSE_WEBVIEW_EDGE
#define WATT_EDGE_PREVIEW 1
#include <wx/webview.h>
#include <objbase.h>
#include <ole2.h>
#include <WebView2.h>
#if defined(_MSC_VER)
#ifdef _DEBUG
#pragma comment(lib, "C:/vcpkg/installed/x64-windows/debug/lib/wxmsw33ud_webview.lib")
#pragma comment(lib, "C:/vcpkg/installed/x64-windows/debug/lib/WebView2Loader.dll.lib")
#else
#pragma comment(lib, "C:/vcpkg/installed/x64-windows/lib/wxmsw33u_webview.lib")
#pragma comment(lib, "C:/vcpkg/installed/x64-windows/lib/WebView2Loader.dll.lib")
#endif
#endif
#else
#define WATT_EDGE_PREVIEW 0
#endif

namespace watt::app::rv {
namespace {

// Scintilla's ILoader (Scintilla 5, include/ILoader.h): a document being filled from any thread.
// CreateLoader() hands out the Document's ILoader sub-object; ConvertToDocument() returns the
// document pointer for SetDocPointer() and keeps the one reference the loader held.
struct SciLoader {
  virtual int __stdcall Release() = 0;
  virtual int __stdcall AddData(const char* data, ptrdiff_t length) = 0;
  virtual void* __stdcall ConvertToDocument() = 0;
};

// Scintilla's IDocument (include/ILexer.h), the interface lexers style a document through.
// Document derives from PerLine, IDocument and ILoader in that order, each just a vtable pointer,
// so the IDocument sub-object sits between the document pointer and its ILoader (checked before
// use: the ILoader must be exactly two pointers past the document).
struct SciDocument {
  virtual int __stdcall Version() const = 0;
  virtual void __stdcall SetErrorStatus(int status) = 0;
  virtual ptrdiff_t __stdcall Length() const = 0;
  virtual void __stdcall GetCharRange(char* buffer, ptrdiff_t position, ptrdiff_t length) const = 0;
  virtual char __stdcall StyleAt(ptrdiff_t position) const = 0;
  virtual ptrdiff_t __stdcall LineFromPosition(ptrdiff_t position) const = 0;
  virtual ptrdiff_t __stdcall LineStart(ptrdiff_t line) const = 0;
  virtual int __stdcall GetLevel(ptrdiff_t line) const = 0;
  virtual int __stdcall SetLevel(ptrdiff_t line, int level) = 0;
  virtual int __stdcall GetLineState(ptrdiff_t line) const = 0;
  virtual int __stdcall SetLineState(ptrdiff_t line, int state) = 0;
  virtual void __stdcall StartStyling(ptrdiff_t position) = 0;
  virtual bool __stdcall SetStyleFor(ptrdiff_t length, char style) = 0;
  virtual bool __stdcall SetStyles(ptrdiff_t length, const char* styles) = 0;
};

SciDocument* document_interface(void* doc, SciLoader* ldr, size_t expect_len) {
  if (reinterpret_cast<char*>(ldr) - static_cast<char*>(doc) != 2 * static_cast<ptrdiff_t>(sizeof(void*)))
    return nullptr;
  auto* d = reinterpret_cast<SciDocument*>(static_cast<char*>(doc) + sizeof(void*));
  if (d->Version() != 2 || d->Length() != static_cast<ptrdiff_t>(expect_len)) return nullptr;
  return d;
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

#if WATT_EDGE_PREVIEW
// Is the Edge WebView2 runtime installed? Asked once, on the first Preview click (never at start).
bool preview_runtime_present() {
  static const int v = [] {
    LPWSTR ver = nullptr;
    const HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &ver);
    const bool ok = SUCCEEDED(hr) && ver != nullptr;
    if (ver) CoTaskMemFree(ver);
    return ok && wxWebView::IsBackendAvailable(wxWebViewBackendEdge) ? 1 : 0;
  }();
  return v == 1;
}

// Answers every resource request of the Preview (stylesheets, images, fonts, frames, fetches)
// with a local 403: the Preview never touches the network. data: URIs are left alone.
class ResourceBlocker final : public ICoreWebView2WebResourceRequestedEventHandler {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (riid == IID_IUnknown || riid == IID_ICoreWebView2WebResourceRequestedEventHandler) {
      *out = this;
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(++refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const long r = --refs_;
    if (r == 0) delete this;
    return static_cast<ULONG>(r);
  }
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender, ICoreWebView2WebResourceRequestedEventArgs* args) override {
    ICoreWebView2WebResourceRequest* req = nullptr;
    if (SUCCEEDED(args->get_Request(&req)) && req) {
      LPWSTR uri = nullptr;
      const bool data = SUCCEEDED(req->get_Uri(&uri)) && uri && wcsncmp(uri, L"data:", 5) == 0;
      if (uri) CoTaskMemFree(uri);
      req->Release();
      if (data) return S_OK;
    }
    ICoreWebView2_2* wv2 = nullptr;
    if (FAILED(sender->QueryInterface(IID_ICoreWebView2_2, reinterpret_cast<void**>(&wv2))) || !wv2) return S_OK;
    ICoreWebView2Environment* env = nullptr;
    if (SUCCEEDED(wv2->get_Environment(&env)) && env) {
      ICoreWebView2WebResourceResponse* resp = nullptr;
      if (SUCCEEDED(env->CreateWebResourceResponse(nullptr, 403, L"Not fetched by the watt Preview", L"", &resp)) &&
          resp) {
        args->put_Response(resp);
        resp->Release();
      }
      env->Release();
    }
    wv2->Release();
    return S_OK;
  }

 private:
  std::atomic<long> refs_{1};
};

// The page as the Preview loads it: a Content-Security-Policy that refuses every fetch and all
// script (on top of the WebView2 settings and the request filter), and a <base> so relative
// links name their real target, both placed at the start of <head>.
std::string preview_page(std::string_view body, const std::string& base_url) {
  std::string inject =
      "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; style-src 'unsafe-inline'; "
      "img-src data:; font-src data:; media-src data:\">";
  if (!base_url.empty() && base_url.find_first_of("\"<>") == std::string::npos)
    inject += "<base href=\"" + base_url + "\">";
  auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
  const size_t scan = std::min<size_t>(body.size(), 8192);
  auto find_tag = [&](std::string_view name) -> size_t {  // end of "<name ...>" (after '>'), or npos
    for (size_t i = 0; i + name.size() + 1 < scan; ++i) {
      if (body[i] != '<') continue;
      size_t k = 0;
      while (k < name.size() && lower(body[i + 1 + k]) == name[k]) ++k;
      if (k != name.size()) continue;
      const char e = body[i + 1 + k];
      if (e != '>' && e != ' ' && e != '\t' && e != '\n' && e != '\r') continue;
      const size_t gt = body.find('>', i);
      return gt == std::string_view::npos ? std::string_view::npos : gt + 1;
    }
    return std::string_view::npos;
  };
  size_t at = find_tag("head");
  if (at == std::string_view::npos) at = find_tag("html");
  if (at == std::string_view::npos) at = find_tag("!doctype");
  if (at == std::string_view::npos) at = 0;
  std::string out;
  out.reserve(body.size() + inject.size());
  out.append(body.substr(0, at));
  out.append(inject);
  out.append(body.substr(at));
  return out;
}
#endif

constexpr int kLexHex = 10000;  // pseudo lexer id: our own hex dump styles (apply_styles only)

constexpr size_t kSyncBytes = 256 * 1024;
// Longest line shown unbroken: Scintilla lays out a whole line to paint any of it, so a page of
// 4 KB lines costs ~60 ms per paint (the Raw toggle on a minified 10 MB body); 1 KB lines ~15 ms.
constexpr size_t kDisplayLine = 1024;
constexpr size_t kWrapNative = 1u << 20;   // Raw bodies above this wrap via a worker-built document
constexpr size_t kAutoWrapMax = 64 * 1024; // wrap is switched on by itself only below this size
constexpr size_t kHexMax = 64u << 20;      // hex view covers at most the first 64 MB
constexpr size_t kChunk = 4u << 20;        // AddData chunk (lets a superseded job stop early)

// Releases the loader / document unless the UI took ownership (l == nullptr).
struct LoaderBox {
  SciLoader* l = nullptr;
  LoaderBox() = default;
  LoaderBox(const LoaderBox&) = delete;
  LoaderBox& operator=(const LoaderBox&) = delete;
  ~LoaderBox() {
    if (l) l->Release();
  }
};

void post_ui(const std::shared_ptr<bool>& alive, std::function<void()> fn) {
  if (!wxTheApp) return;
  wxTheApp->CallAfter([alive, fn = std::move(fn)] {
    if (*alive) fn();
  });
}

enum ChipId { kPretty = 1, kRaw, kHex, kPreview, kTable, kViewAs = 6, kWrap = 10, kFold, kUnfold, kFind, kCopy, kSave,
              kFilter, kCase = 20, kPrev, kNext, kClose, kFilterClose = 30, kFilterHelp };

int lexer_for(fmt::BodyKind k) {
  switch (k) {
    case fmt::BodyKind::Json: return wxSTC_LEX_JSON;
    case fmt::BodyKind::Xml: return wxSTC_LEX_XML;
    case fmt::BodyKind::Html: return wxSTC_LEX_HTML;
    default: return wxSTC_LEX_NULL;
  }
}

// Case-(in)sensitive occurrence count of q in t; *before = matches starting before `pos`.
size_t count_matches(std::string_view t, std::string_view q, bool match_case, size_t pos, size_t* before) {
  size_t n = 0, b = 0;
  if (q.empty() || q.size() > t.size()) {
    *before = 0;
    return 0;
  }
  auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
  auto count = [&](auto searcher) {
    auto it = t.begin();
    while (n < 10'000'000) {
      auto r = searcher(it, t.end());
      if (r.first == t.end()) break;
      if (static_cast<size_t>(r.first - t.begin()) < pos) ++b;
      ++n;
      it = r.second;
    }
  };
  if (match_case) {
    count(std::boyer_moore_horspool_searcher(q.begin(), q.end()));
  } else {
    struct Hash {
      size_t operator()(char c) const {
        return std::hash<char>()((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
      }
    };
    count(std::boyer_moore_horspool_searcher(q.begin(), q.end(), Hash{},
                                             [lower](char a, char c) { return lower(a) == lower(c); }));
  }
  *before = b;
  return n;
}

// Next match of q at or after `from` (forward, wrapping to the start) or the last match starting
// before `from` (backward, wrapping to the end). npos when there is none.
size_t find_next(std::string_view t, std::string_view q, bool match_case, size_t from, bool forward) {
  if (q.empty() || q.size() > t.size()) return std::string_view::npos;
  auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
  struct Hash {
    size_t operator()(char c) const {
      return std::hash<char>()((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
  };
  // First (or, last_one, last) match starting in [a, b - q.size()]; npos if none.
  auto scan = [&](size_t a, size_t b, bool last_one) -> size_t {
    if (b > t.size()) b = t.size();
    if (a >= b) return std::string_view::npos;
    const std::string_view r = t.substr(a, b - a);
    size_t found = std::string_view::npos;
    auto run = [&](auto searcher) {
      auto it = r.begin();
      for (;;) {
        auto m = searcher(it, r.end());
        if (m.first == r.end()) break;
        found = a + static_cast<size_t>(m.first - r.begin());
        if (!last_one) break;
        it = m.first + 1;
      }
    };
    if (match_case) {
      run(std::boyer_moore_horspool_searcher(q.begin(), q.end()));
    } else {
      run(std::boyer_moore_horspool_searcher(q.begin(), q.end(), Hash{},
                                             [lower](char x, char y) { return lower(x) == lower(y); }));
    }
    return found;
  };
  from = std::min(from, t.size());
  if (forward) {
    const size_t f = scan(from, t.size(), false);
    return f != std::string_view::npos ? f : scan(0, from + q.size() - 1, false);
  }
  // Matches starting before `from` (they may extend past it).
  const size_t f = scan(0, from + q.size() - 1, true);
  if (f != std::string_view::npos && f < from) return f;
  return scan(from, t.size(), true);
}

}  // namespace

// ------------------------------------------------------------------ worker

// One background thread per body view, FIFO. Jobs never touch wx objects; they post results back
// with post_ui(). Dropping the view detaches the thread: queued jobs are discarded and a running
// one finishes into the void (its result checks `alive`).
class BodyWorker {
 public:
  BodyWorker() : st_(std::make_shared<State>()) {
    std::thread([st = st_] {
      for (;;) {
        std::function<void()> f;
        {
          std::unique_lock lk(st->m);
          st->cv.wait(lk, [&] { return st->stop || !st->q.empty(); });
          if (st->stop) return;
          f = std::move(st->q.front());
          st->q.pop_front();
        }
        f();
      }
    }).detach();
  }
  ~BodyWorker() {
    std::deque<std::function<void()>> drop;
    {
      std::lock_guard lk(st_->m);
      st_->stop = true;
      drop.swap(st_->q);
    }
    st_->cv.notify_all();
  }
  void post(std::function<void()> f) {
    {
      std::lock_guard lk(st_->m);
      st_->q.push_back(std::move(f));
    }
    st_->cv.notify_one();
  }

 private:
  struct State {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::function<void()>> q;
    bool stop = false;
  };
  std::shared_ptr<State> st_;
};

struct BodyView::Built {
  std::shared_ptr<LoaderBox> box;
  void* doc = nullptr;
  std::shared_ptr<const void> owner;
  std::string_view text;
  int lexer = wxSTC_LEX_NULL;
  bool prestyled = false;  // styles and fold levels already in the document: no Scintilla lexer
  bool display_breaks = false;
  std::shared_ptr<const std::vector<int>> levels;
  std::shared_ptr<const std::vector<int>> line_map;
  std::string note;
  uint64_t fseq = 0;  // Filter: the expression this was built for
  uint64_t wseq = 0;  // Wrapped / RawWrap: wrap_seq_ when the build started
  bool heavy = false;
  size_t longest = 0;
  std::shared_ptr<const std::vector<size_t>> breaks;
  std::shared_ptr<const void> full_owner;  // the text before the breaks were inserted
  std::string_view full;
};

namespace {
// Position p of a text -> its position once `breaks` (offsets in that text) were inserted.
int past_breaks(const std::vector<size_t>& b, int p) {
  if (p < 0) return p;
  return p + static_cast<int>(std::upper_bound(b.begin(), b.end(), static_cast<size_t>(p)) - b.begin());
}

// The inverse: position w of the text with the breaks -> position in the text without them.
int before_breaks(const std::vector<size_t>& b, int w) {
  if (w < 0) return w;
  // Break i sits at offset b[i] + i of the broken text; count those before w.
  size_t lo = 0, hi = b.size();
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (b[mid] + mid < static_cast<size_t>(w)) lo = mid + 1;
    else hi = mid;
  }
  return w - static_cast<int>(lo);
}

// A document this big gets no Scintilla wrap (see BodyView::Mode::Wrapped).
constexpr size_t kHeavyBytes = 1u << 20;
constexpr size_t kHeavyLines = 100000;
}  // namespace

// ------------------------------------------------------------------ view

BodyView::BodyView(wxWindow* parent)
    : wxPanel(parent, wxID_ANY),
      find_timer_(this),
      style_timer_(this),
      filter_timer_(this),
      worker_(std::make_unique<BodyWorker>()),
      gen_(std::make_shared<std::atomic<uint64_t>>(0)),
      alive_(std::make_shared<bool>(true)) {
  const Theme& th = Theme::get();
  SetBackgroundColour(th.bg);
  bar_ = new ChipBar(this, [this](int id) { on_chip(id); });
  bar_->add(kPretty, "Pretty", "Indented and highlighted (JSON, XML, HTML), formatted off the UI thread", 0);
  bar_->add(kRaw, "Raw", "Exactly the bytes received (after content decoding)", 0);
  bar_->add(kHex, "Hex", "Hex dump of the body", 0);
  bar_->add(kPreview, "Preview", "The page rendered like a browser (Edge WebView2): no scripts run and nothing is fetched (images, stylesheets and links stay local)", 0);
  bar_->add(kTable, "Table", "Visualize the JSON as a table: an array of objects as rows and columns (the largest "
                            "array in the body, or the Filter's result). Click a column header to sort.", 0);
  bar_->add(kViewAs, "JSON \xE2\x96\xBE",
            "View as: the format the body is shown in, detected from Content-Type and the content. Click to show it "
            "as JSON, XML, HTML or plain text instead (for a wrong or missing Content-Type).");
  bar_->add(kWrap, "Wrap", "Wrap long lines (on by itself for small bodies whose lines do not fit)", -1, true);
  bar_->add(kFold, "Fold", "Collapse everything below the top level", -1, true);
  bar_->add(kUnfold, "Unfold", "Expand all folds", -1, true);
  bar_->add(kFilter, "Filter", "Filter the JSON body with a JSONPath query, e.g. $.items[*].name or $..id "
                              "(Ctrl+Shift+F). Copy and Save then take the result.", -1, true);
  bar_->add(kFind, "Find", "Search the body (Ctrl+F, F3 / Shift+F3)", -1, true);
  bar_->add(kCopy, "Copy", "Copy the whole body (Raw: the bytes exactly as received; gRPC: the message JSON without "
                          "the #n annotations, a stream as a JSON array). Ctrl+C copies a selection.", -1, true);
  bar_->add(kSave, "Save", "Save the response body to a file, byte for byte as received", -1, true);

  findbar_ = new wxPanel(this, wxID_ANY);
  findbar_->SetBackgroundColour(th.bg);
  find_text_ = new wxTextCtrl(findbar_, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(260, -1)),
                              wxTE_PROCESS_ENTER);
  find_text_->SetHint("Find in body");
  find_chips_ = new ChipBar(findbar_, [this](int id) { on_chip(id); });
  find_chips_->add(kCase, "Aa", "Match case", 1);
  find_chips_->add(kPrev, "Prev", "Previous match (Shift+Enter, Shift+F3)", -1, true);
  find_chips_->add(kNext, "Next", "Next match (Enter, F3)", -1, true);
  find_chips_->add(kClose, "Close", "Close (Esc)", -1, true);
  auto* fs = new wxBoxSizer(wxHORIZONTAL);
  fs->Add(find_text_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
  fs->Add(find_chips_, 1, wxEXPAND);
  findbar_->SetSizer(fs);
  findbar_->Hide();

  stc_ = new wxStyledTextCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
  setup_stc();
  // The document factory is created in an idle moment after the view is up (or by the first
  // body that needs it, whichever comes first): not part of the view's build.
  CallAfter([this] { factory(); });

  auto* sz = new wxBoxSizer(wxVERTICAL);
  sz->Add(bar_, 0, wxEXPAND);
  sz->Add(findbar_, 0, wxEXPAND | wxBOTTOM, FromDIP(2));
  sz->Add(stc_, 1, wxEXPAND);
  SetSizer(sz);

  find_text_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { find_timer_.StartOnce(90); });
  find_text_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) {
    find_from(!wxGetKeyState(WXK_SHIFT), false);
  });
  Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
    find_from(true, true);
  }, find_timer_.GetId());
  Bind(wxEVT_TIMER, [this](wxTimerEvent&) { style_slice(); }, style_timer_.GetId());
  Bind(wxEVT_TIMER, [this](wxTimerEvent&) { apply_filter(); }, filter_timer_.GetId());
  // Wrap of a heavy document follows the pane width once a resize settles (a worker rebuild).
  wrap_timer_.SetOwner(this);
  Bind(wxEVT_TIMER, [this](wxTimerEvent&) { sync_wrap(); }, wrap_timer_.GetId());
  stc_->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
    e.Skip();
    if (!wrap_ || !resp_ || grpc_) return;
    const int cur = on_screen();
    if (cur >= 0 && docs_[cur].heavy) wrap_timer_.StartOnce(150);
  });
  Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
    const int k = e.GetKeyCode();
    if (k == 'F' && e.GetModifiers() == (wxMOD_CONTROL | wxMOD_SHIFT)) return show_filter();
    if (k == 'F' && e.GetModifiers() == wxMOD_CONTROL) return show_find();
    if (k == WXK_ESCAPE && filterbar_ && filterbar_->IsShown() && wxWindow::FindFocus() == filter_text_)
      return close_filter();
    if (k == WXK_F3) {
      if (!findbar_->IsShown()) return show_find();
      find_from(!e.ShiftDown(), false);
      return;
    }
    if (k == WXK_ESCAPE && findbar_->IsShown()) return close_find();
    e.Skip();
  });
  stc_->Bind(wxEVT_STC_MARGINCLICK, [this](wxStyledTextEvent& e) { on_margin_click(e); });
  stc_->Bind(wxEVT_STC_DOUBLECLICK, [this](wxStyledTextEvent& e) {
    if (shown_mode() != Outline) return e.Skip();
    open_outline_line(stc_->LineFromPosition(stc_->GetCurrentPos()));
  });
  stc_->Bind(wxEVT_STC_UPDATEUI, [this](wxStyledTextEvent& e) {
    e.Skip();
    if (findbar_->IsShown()) highlight_visible();
  });

  set_mode_chips();
}

wxStyledTextCtrl* BodyView::factory() {
  if (!factory_) {
    factory_ = new wxStyledTextCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(1, 1), wxBORDER_NONE);
    factory_->Hide();
  }
  return factory_;
}

BodyView::~BodyView() {
  *alive_ = false;
  ++*gen_;
  worker_.reset();
  release_docs();
}

void BodyView::setup_stc() {
  const Theme& th = Theme::get();
  const bool dark = th.dark;
  const wxColour bg = dark ? wxColour(30, 30, 30) : *wxWHITE;
  const wxColour fg = dark ? wxColour(212, 212, 212) : wxColour(30, 30, 30);
  wxFont mono(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE).FaceName("Consolas"));
  stc_->StyleSetFont(wxSTC_STYLE_DEFAULT, mono);
  stc_->StyleSetBackground(wxSTC_STYLE_DEFAULT, bg);
  stc_->StyleSetForeground(wxSTC_STYLE_DEFAULT, fg);
  stc_->StyleClearAll();
  stc_->SetCaretForeground(fg);
  stc_->SetSelBackground(true, dark ? wxColour(38, 79, 120) : wxColour(173, 214, 255));
  stc_->SetMarginType(0, wxSTC_MARGIN_NUMBER);
  stc_->StyleSetBackground(wxSTC_STYLE_LINENUMBER, bg);
  stc_->StyleSetForeground(wxSTC_STYLE_LINENUMBER, dark ? wxColour(110, 110, 110) : wxColour(150, 150, 150));
  stc_->SetMarginWidth(1, 0);
  // Fold margin.
  stc_->SetMarginType(2, wxSTC_MARGIN_SYMBOL);
  stc_->SetMarginMask(2, wxSTC_MASK_FOLDERS);
  stc_->SetMarginWidth(2, FromDIP(14));
  stc_->SetMarginSensitive(2, true);
  stc_->SetFoldMarginColour(true, bg);
  stc_->SetFoldMarginHiColour(true, bg);
  const wxColour mk = dark ? wxColour(130, 130, 130) : wxColour(140, 140, 140);
  stc_->MarkerDefine(wxSTC_MARKNUM_FOLDEROPEN, wxSTC_MARK_BOXMINUS, bg, mk);
  stc_->MarkerDefine(wxSTC_MARKNUM_FOLDER, wxSTC_MARK_BOXPLUS, bg, mk);
  stc_->MarkerDefine(wxSTC_MARKNUM_FOLDERSUB, wxSTC_MARK_VLINE, bg, mk);
  stc_->MarkerDefine(wxSTC_MARKNUM_FOLDERTAIL, wxSTC_MARK_LCORNER, bg, mk);
  stc_->MarkerDefine(wxSTC_MARKNUM_FOLDEREND, wxSTC_MARK_BOXPLUSCONNECTED, bg, mk);
  stc_->MarkerDefine(wxSTC_MARKNUM_FOLDEROPENMID, wxSTC_MARK_BOXMINUSCONNECTED, bg, mk);
  stc_->MarkerDefine(wxSTC_MARKNUM_FOLDERMIDTAIL, wxSTC_MARK_TCORNER, bg, mk);
  // Margin clicks are ours (on_margin_click): a folded header then shows what it hides.
  stc_->SetAutomaticFold(wxSTC_AUTOMATICFOLD_SHOW | wxSTC_AUTOMATICFOLD_CHANGE);
  // A folded line ends in a dim, unboxed "..." (a box is taller than the line and overlaps the
  // next row) instead of a full-width rule.
  stc_->SetFoldFlags(0);
  stc_->SetDefaultFoldDisplayText(wxString::FromUTF8("\xE2\x80\xA6"));
  stc_->FoldDisplayTextSetStyle(wxSTC_FOLDDISPLAYTEXT_STANDARD);
  stc_->StyleSetForeground(wxSTC_STYLE_FOLDDISPLAYTEXT, mk);
  stc_->StyleSetBackground(wxSTC_STYLE_FOLDDISPLAYTEXT, dark ? wxColour(48, 48, 52) : wxColour(238, 238, 242));
  stc_->StyleSetItalic(wxSTC_STYLE_FOLDDISPLAYTEXT, true);
  // Indent guides every 2 columns (the pretty printers indent by 2).
  stc_->SetIndent(2);
  stc_->SetTabWidth(4);
  stc_->SetIndentationGuides(wxSTC_IV_LOOKBOTH);
  stc_->StyleSetForeground(wxSTC_STYLE_INDENTGUIDE, dark ? wxColour(70, 70, 70) : wxColour(210, 210, 210));
  stc_->StyleSetBackground(wxSTC_STYLE_INDENTGUIDE, bg);
  // Search highlight.
  stc_->IndicatorSetStyle(8, wxSTC_INDIC_ROUNDBOX);
  stc_->IndicatorSetForeground(8, dark ? wxColour(230, 180, 60) : wxColour(240, 170, 0));
  stc_->IndicatorSetAlpha(8, 90);
  stc_->IndicatorSetOutlineAlpha(8, 200);
  // Big documents: no undo, page layout cache, styling in idle time instead of before paint,
  // scroll width grows with what has been seen (no full-document width scan).
  stc_->SetWrapMode(wxSTC_WRAP_NONE);
  stc_->SetWrapIndentMode(wxSTC_WRAPINDENT_INDENT);  // continuation lines keep the indentation
  stc_->SetUndoCollection(false);
  stc_->SetLayoutCache(wxSTC_CACHE_PAGE);
  // Styling: Scintilla styles what it paints; the rest of the document is styled by our own
  // timer in ~4 ms slices (Scintilla's own idle styling works in 20 ms slices, and without it a
  // jump to the end of a 1M-line document styles everything at once).
  stc_->SetIdleStyling(wxSTC_IDLESTYLING_NONE);
  stc_->SetScrollWidth(1);
  stc_->SetScrollWidthTracking(true);
  stc_->SetReadOnly(true);
  stc_->UsePopUp(wxSTC_POPUP_ALL);
  cur_lexer_ = -1;
}

void BodyView::apply_styles(int lexer) {
  if (lexer == cur_lexer_) return;
  const bool was_preview = cur_lexer_ == wxSTC_LEX_MARKDOWN;
  cur_lexer_ = lexer;
  const bool dark = Theme::get().dark;
  const bool preview = lexer == wxSTC_LEX_MARKDOWN;
  // Reset every lexer style to default, then colour the ones this lexer uses. The Preview reads
  // as a page: proportional font; everything else is monospaced.
  const wxColour fg = stc_->StyleGetForeground(wxSTC_STYLE_DEFAULT);
  const wxFont mono = stc_->StyleGetFont(wxSTC_STYLE_DEFAULT);
  const wxFont prop(wxFontInfo(10).Family(wxFONTFAMILY_SWISS).FaceName("Segoe UI"));
  for (int s = 0; s < wxSTC_STYLE_DEFAULT; ++s) {
    if (preview || was_preview) stc_->StyleSetFont(s, preview ? prop : mono);
    stc_->StyleSetForeground(s, fg);
    stc_->StyleSetBold(s, false);
    stc_->StyleSetItalic(s, false);
  }
  auto set = [&](int s, wxColour d, wxColour l) { stc_->StyleSetForeground(s, dark ? d : l); };
  const wxColour key(156, 220, 254), keyL(4, 81, 165);
  const wxColour str(206, 145, 120), strL(163, 21, 21);
  const wxColour num(181, 206, 168), numL(9, 134, 88);
  const wxColour kw(86, 156, 214), kwL(0, 0, 255);
  const wxColour com(106, 153, 85), comL(0, 128, 0);
  const wxColour op(150, 150, 150), opL(110, 110, 110);
  if (lexer == kLexHex) {
    set(1, op, opL);
    set(2, str, strL);
  } else if (preview) {
    const int heads[] = {wxSTC_MARKDOWN_HEADER1, wxSTC_MARKDOWN_HEADER2, wxSTC_MARKDOWN_HEADER3,
                         wxSTC_MARKDOWN_HEADER4, wxSTC_MARKDOWN_HEADER5, wxSTC_MARKDOWN_HEADER6};
    for (int i = 0; i < 6; ++i) {
      stc_->StyleSetBold(heads[i], true);
      stc_->StyleSetSize(heads[i], i == 0 ? 15 : i == 1 ? 13 : 11);
      set(heads[i], wxColour(230, 230, 230), wxColour(20, 20, 20));
    }
    set(wxSTC_MARKDOWN_ULIST_ITEM, op, opL);
    set(wxSTC_MARKDOWN_HRULE, op, opL);
    set(wxSTC_MARKDOWN_LINK, kw, kwL);
    set(wxSTC_MARKDOWN_CODE, str, strL);
  } else if (lexer == wxSTC_LEX_JSON) {
    set(wxSTC_JSON_PROPERTYNAME, key, keyL);
    set(wxSTC_JSON_STRING, str, strL);
    set(wxSTC_JSON_STRINGEOL, str, strL);
    set(wxSTC_JSON_NUMBER, num, numL);
    set(wxSTC_JSON_KEYWORD, kw, kwL);
    set(wxSTC_JSON_LDKEYWORD, kw, kwL);
    set(wxSTC_JSON_ESCAPESEQUENCE, wxColour(215, 186, 125), wxColour(238, 0, 0));
    set(wxSTC_JSON_OPERATOR, op, opL);
    set(wxSTC_JSON_LINECOMMENT, com, comL);
    set(wxSTC_JSON_BLOCKCOMMENT, com, comL);
    set(wxSTC_JSON_ERROR, wxColour(244, 71, 71), wxColour(205, 49, 49));
    set(wxSTC_JSON_URI, str, strL);
    set(wxSTC_JSON_COMPACTIRI, key, keyL);
  } else if (lexer == wxSTC_LEX_XML || lexer == wxSTC_LEX_HTML) {
    set(wxSTC_H_TAG, kw, wxColour(128, 0, 0));
    set(wxSTC_H_TAGUNKNOWN, kw, wxColour(128, 0, 0));
    set(wxSTC_H_TAGEND, kw, wxColour(128, 0, 0));
    set(wxSTC_H_ATTRIBUTE, key, wxColour(229, 0, 0));
    set(wxSTC_H_ATTRIBUTEUNKNOWN, key, wxColour(229, 0, 0));
    set(wxSTC_H_DOUBLESTRING, str, wxColour(0, 0, 255));
    set(wxSTC_H_SINGLESTRING, str, wxColour(0, 0, 255));
    set(wxSTC_H_NUMBER, num, numL);
    set(wxSTC_H_COMMENT, com, comL);
    set(wxSTC_H_ENTITY, wxColour(215, 186, 125), wxColour(160, 80, 0));
    set(wxSTC_H_XMLSTART, op, opL);
    set(wxSTC_H_XMLEND, op, opL);
    set(wxSTC_H_CDATA, str, strL);
    set(wxSTC_H_VALUE, str, strL);
    set(wxSTC_H_SGML_DEFAULT, op, opL);
    set(wxSTC_H_DOUBLESTRING, str, wxColour(0, 0, 255));
    // Embedded CSS / JavaScript inside <style> / <script> (the HTML lexer's sub-languages).
    set(wxSTC_HJ_KEYWORD, kw, kwL);
    set(wxSTC_HJ_WORD, fg, fg);
    set(wxSTC_HJ_DOUBLESTRING, str, strL);
    set(wxSTC_HJ_SINGLESTRING, str, strL);
    set(wxSTC_HJ_NUMBER, num, numL);
    set(wxSTC_HJ_COMMENT, com, comL);
    set(wxSTC_HJ_COMMENTLINE, com, comL);
    set(wxSTC_HJ_SYMBOLS, op, opL);
  }
}

void BodyView::release_docs() {
  keep_ = Keep{};
  ++wrap_seq_;
  for (Doc& d : docs_) {
    if (d.doc) stc_->ReleaseDocument(d.doc);
    d = Doc{};
  }
}

int BodyView::base_mode() const {
  if (mode_ == Pretty && !filter_.empty() && resp_ && kind_ == fmt::BodyKind::Json) return Filter;
  if (mode_ == Pretty && outline_) return Outline;
  if (mode_ == Raw && wrap_ && resp_ && resp_->body.size() > kWrapNative) return RawWrap;
  return mode_;
}

int BodyView::shown_mode() const {
  const int k = base_mode();
  if (wrap_ && resp_ && wrap_cols_ > 0 && (k == Pretty || k == Filter || k == Preview || k == Raw)) {
    const Doc& s = docs_[k];
    if (s.doc && s.heavy && s.longest > static_cast<size_t>(wrap_cols_)) return Wrapped;
  }
  return k;
}

int BodyView::on_screen() const {
  void* p = stc_->GetDocPointer();
  if (!p) return -1;
  for (int i = 0; i < kModes; ++i)
    if (docs_[i].doc == p) return i;
  return -1;
}

int BodyView::pane_cols() const {
  const int cw = std::max(1, stc_->TextWidth(wxSTC_STYLE_DEFAULT, "0"));
  const int w = stc_->GetClientSize().x;
  if (w < FromDIP(120)) return 0;
  const int text_w = w - stc_->GetMarginWidth(0) - stc_->GetMarginWidth(2) - stc_->GetMarginLeft() - FromDIP(16);
  return std::max(40, text_w / cw);
}

bool BodyView::wrap_stale(int k) const {
  const Doc& d = docs_[k];
  if (!d.doc && !d.building) return false;
  // A few columns of slack: the line-number margin grows with the line count of the wrapped
  // document, which would otherwise rebuild it once more for one column.
  const int want = std::max(wrap_cols_, 40);
  if (d.cols > want || d.cols + 3 < want) return true;
  if (k == Wrapped) return d.src != base_mode() || d.src_doc != docs_[d.src].doc;
  return false;
}

// The Wrapped document is built from its source's full text (without the source's own display
// breaks): positions go source document -> full text -> wrapped document, and back.
int BodyView::wrapped_pos(int p) const {
  const Doc& w = docs_[Wrapped];
  if (w.src < 0) return p;
  const Doc& s = docs_[w.src];
  if (s.full_owner && s.breaks) p = before_breaks(*s.breaks, p);
  return w.breaks ? past_breaks(*w.breaks, p) : p;
}

int BodyView::source_pos(int p) const {
  const Doc& w = docs_[Wrapped];
  if (w.src < 0) return p;
  if (w.breaks) p = before_breaks(*w.breaks, p);
  const Doc& s = docs_[w.src];
  return s.full_owner && s.breaks ? past_breaks(*s.breaks, p) : p;
}

void BodyView::sync_wrap() {
  if (!resp_ || grpc_ || !stc_->IsShown()) return;
  const int base = base_mode();
  if (base == Hex || base == Table || base == Outline) return;
  if (const int c = pane_cols(); c > 0) wrap_cols_ = c;
  const int want = shown_mode();
  const int cur = on_screen();
  if ((want == Wrapped || want == RawWrap) && wrap_stale(want)) {
    // Rebuilt for the new width: keep the place (in source positions) for when it is swapped in.
    if (cur == Wrapped && keep_.src < 0) {
      const int top = stc_->PositionFromLine(stc_->DocLineFromVisible(stc_->GetFirstVisibleLine()));
      keep_ = Keep{};
      keep_.src = docs_[Wrapped].src;
      keep_.top = source_pos(top);
      keep_.caret = source_pos(stc_->GetCurrentPos());
      keep_.anchor = source_pos(stc_->GetAnchor());
    } else if (cur == RawWrap && keep_.src < 0) {
      keep_ = Keep{};
      keep_.src = Raw;
      keep_.ratio = static_cast<double>(stc_->GetFirstVisibleLine()) / std::max(1, stc_->GetLineCount());
    }
    Doc& d = docs_[want];
    if (d.doc) stc_->ReleaseDocument(d.doc);  // the control keeps its own reference while shown
    d = Doc{};
    ++wrap_seq_;
  }
  if (cur != want || !docs_[want].doc) {
    if (!docs_[want].building) request_mode(mode_);
  } else {
    update_margin();
    update_note();
  }
}

void BodyView::show_message(const std::string& text) {
  reset_find();
  ++*gen_;
  hide_html_preview();
  resp_.reset();
  grpc_resp_.reset();
  grpc_ = false;
  outline_ = false;
  kind_ = fmt::BodyKind::Empty;
  msg_pos_.clear();
  mode_ = Pretty;
  wrap_chosen_ = false;
  wrap_ = true;  // messages are prose (errors): always wrapped
  stc_->SetDocPointer(nullptr);  // fresh empty document owned by the control
  stc_->SetScrollWidth(1);  // tracking only grows it: start again from this document's lines
  release_docs();
  stc_->SetLexer(wxSTC_LEX_NULL);
  apply_styles(wxSTC_LEX_NULL);
  stc_->SetReadOnly(false);
  stc_->SetText(wxString::FromUTF8(text));
  stc_->SetReadOnly(true);
  update_margin();
  set_mode_chips();
  bar_->set_note("");
}

void BodyView::set_http(std::shared_ptr<const HttpResponse> r) {
  if (!r) return show_message("");
  if (r->body.empty()) {
    show_message("");
    bar_->set_note("No body", Tone::Dim);
    return;
  }
  reset_find();
  ++*gen_;
  grpc_ = false;
  grpc_resp_.reset();
  outline_ = false;
  msg_pos_.clear();
  resp_ = std::move(r);
  kind_forced_ = false;
  apply_kind(fmt::detect_kind(resp_->headers.get("content-type"), resp_->body));
}

void BodyView::apply_kind(fmt::BodyKind k) {
  ++*gen_;  // views being built for the old kind drop out
  hide_html_preview();
  outline_ = false;
  kind_ = k;
  wrap_chosen_ = false;
  wrap_ = false;
  wrap_cols_ = 0;
  int m = Raw;
  if (kind_ == fmt::BodyKind::Binary) m = Hex;
  else if (kind_ == fmt::BodyKind::Json || kind_ == fmt::BodyKind::Xml || kind_ == fmt::BodyKind::Html) m = Pretty;
  // A body built on the worker: the previous document stays on screen, faded and labelled, until
  // the new one is swapped in (no flash of an empty pane). A small body replaces it in this turn.
  const bool async = resp_->body.size() > kSyncBytes && stc_->GetLength() > 0;
  if (async) {
    holding_ = true;
    fade_styles();
  } else {
    stc_->SetDocPointer(nullptr);
  }
  release_docs();  // our references; a held document lives on in the control until the swap
  stc_->SetReadOnly(true);
  request_mode(m);
  // Large non-raw view: build Raw in the background too, so the toggle is instant.
  if (m != Raw && resp_->body.size() > kSyncBytes) build(Raw);
}

void BodyView::view_as_menu() {
  if (!resp_) return;
  using K = fmt::BodyKind;
  const K detected = fmt::detect_kind(resp_->headers.get("content-type"), resp_->body);
  wxMenu menu;
  menu.AppendCheckItem(100, wxString::FromUTF8(std::string("Auto (") + fmt::kind_name(detected) + ")"))
      ->Check(!kind_forced_);
  menu.AppendSeparator();
  const K kinds[] = {K::Json, K::Xml, K::Html, K::Text};
  for (int i = 0; i < 4; ++i)
    menu.AppendCheckItem(101 + i, fmt::kind_name(kinds[i]))->Check(kind_forced_ && kind_ == kinds[i]);
  const int id = GetPopupMenuSelectionFromUser(menu, bar_->GetPosition() + wxPoint(0, bar_->GetSize().y));
  if (id == 100) {
    if (!kind_forced_) return;
    kind_forced_ = false;
    apply_kind(detected);
  } else if (id >= 101 && id <= 104) {
    const K k = kinds[id - 101];
    if (kind_forced_ && k == kind_) return;
    kind_forced_ = true;
    apply_kind(k);
  }
}

bool BodyView::show_web_preview(const std::string& page) {
#if WATT_EDGE_PREVIEW
  if (!preview_runtime_present()) return false;
  if (!web_) {
    web_ = wxWebView::New(this, wxID_ANY, "about:blank", wxDefaultPosition, wxDefaultSize, wxWebViewBackendEdge,
                          wxBORDER_NONE);
    if (!web_) return false;
    web_->EnableContextMenu(false);
    web_->EnableAccessToDevTools(false);
    web_->EnableBrowserAcceleratorKeys(false);
    web_->Bind(wxEVT_WEBVIEW_CREATED, [this](wxWebViewEvent&) {
      if (auto* wv = static_cast<ICoreWebView2*>(web_->GetNativeBackend())) {
        ICoreWebView2Settings* st = nullptr;
        if (SUCCEEDED(wv->get_Settings(&st)) && st) {
          st->put_IsScriptEnabled(FALSE);
          st->put_AreDefaultContextMenusEnabled(FALSE);
          st->put_AreDevToolsEnabled(FALSE);
          st->put_AreDefaultScriptDialogsEnabled(FALSE);
          st->put_IsStatusBarEnabled(FALSE);
          st->put_AreHostObjectsAllowed(FALSE);
          st->put_IsZoomControlEnabled(TRUE);
          st->Release();
        }
        wv->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        auto* blocker = new ResourceBlocker();
        EventRegistrationToken tok{};
        wv->add_WebResourceRequested(blocker, &tok);
        blocker->Release();  // the WebView holds it now
      }
      web_ready_ = true;
      if (!web_pending_.empty()) {
        std::string p;
        p.swap(web_pending_);
        web_load(p);
      }
    });
    // Only our own SetPage navigates: links, forms, meta refresh and frames are refused (the
    // target is shown in the note instead).
    web_->Bind(wxEVT_WEBVIEW_NAVIGATING, [this](wxWebViewEvent& e) {
      const std::string url = e.GetURL().utf8_string();
      if (url == "about:blank") return;  // the empty start page (and NavigateToString on some runtimes)
      if (web_allow_nav_ && url.rfind("data:", 0) == 0) {
        web_allow_nav_ = false;
        return;
      }
      e.Veto();
      bar_->set_note("link not followed in the Preview: " + url, Tone::Dim);
    });
    web_->Bind(wxEVT_WEBVIEW_NEWWINDOW, [this](wxWebViewEvent& e) {
      bar_->set_note("new window not opened by the Preview: " + e.GetURL().utf8_string(), Tone::Dim);
    });
    GetSizer()->Add(web_, 1, wxEXPAND);
  }
  web_load(page);
  if (html_) html_->Hide();
  stc_->Hide();
  web_->Show();
  Layout();
  bar_->set_note(std::string(fmt::kind_name(kind_)) + " \xC2\xB7 " + fmt::human_bytes(resp_->body.size()) +
                     " \xC2\xB7 rendered (Edge): scripts off, nothing fetched",
                 Tone::Dim);
  return true;
#else
  (void)page;
  return false;
#endif
}

void BodyView::web_load(const std::string& page) {
#if WATT_EDGE_PREVIEW
  if (!web_ready_) {
    web_pending_ = page;
    if (web_pending_.empty()) web_pending_ = " ";
    return;
  }
  wxString html = wxString::FromUTF8(page);
  if (html.empty() && !page.empty()) html = wxString::From8BitData(page.data(), page.size());
  web_allow_nav_ = true;
  web_->SetPage(html, wxString::FromUTF8(web_base_));
#else
  (void)page;
#endif
}

bool BodyView::show_html_preview() {
  if (!resp_) return false;
  // Edge renders like a browser (CSS layout) in its own process: pages up to NavigateToString's
  // ~2 MB limit. Scripts are off and nothing is fetched.
#if WATT_EDGE_PREVIEW
  if (resp_->body.size() <= (1536u << 10)) {
    web_base_ = resp_->effective_url;
    if (show_web_preview(preview_page(resp_->body, web_base_))) return true;
  }
#endif
  // wxHtmlWindow lays the page out on the UI thread: big pages keep the text preview.
  if (resp_->body.size() > (512u << 10)) return false;
  std::string page;
  fmt::html_for_preview(resp_->body, page);
  wxString html = wxString::FromUTF8(page);
  if (html.empty() && !page.empty()) html = wxString::From8BitData(page.data(), page.size());
  if (!html_) {
    html_ = new wxHtmlWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxHW_SCROLLBAR_AUTO | wxBORDER_NONE);
    html_->SetBorders(FromDIP(12));
    html_->SetStandardFonts(FromDIP(10), "Segoe UI", "Consolas");
    // Links are not followed (the Preview never fetches anything); hovering shows the target.
    html_->Bind(wxEVT_HTML_LINK_CLICKED, [this](wxHtmlLinkEvent& e) {
      bar_->set_note("link (not followed in the Preview): " + e.GetLinkInfo().GetHref().utf8_string(), Tone::Dim);
    });
    GetSizer()->Add(html_, 1, wxEXPAND);
  }
  html_->SetPage(html);
  stc_->Hide();
  if (web_) web_->Hide();
  html_->Show();
  Layout();
  bar_->set_note(std::string(fmt::kind_name(kind_)) + " \xC2\xB7 " + fmt::human_bytes(resp_->body.size()) +
                     " \xC2\xB7 rendered: no scripts, no CSS, nothing fetched",
                 Tone::Dim);
  return true;
}

void BodyView::hide_html_preview() {
  const bool html = html_ && html_->IsShown();
  const bool web = web_ && web_->IsShown();
  const bool table = table_win_ && table_win_->IsShown();
  if (!html && !web && !table) return;
  if (html) html_->Hide();
  if (web) web_->Hide();
  if (table) table_win_->Hide();
  stc_->Show();
  Layout();
}

void BodyView::request_mode(int m) {
  if (m != Pretty) outline_ = false;
  if (m == Table) reset_find();  // the table has its own Filter; the find bar searches the text views
  mode_ = m;
  set_mode_chips();
  if (m == Preview && show_html_preview()) return;
  if (m == Table && resp_) return show_table();
  hide_html_preview();
  const int k = shown_mode();
  if ((k == Wrapped || k == RawWrap) && wrap_stale(k)) {
    Doc& w = docs_[k];
    if (w.doc) stc_->ReleaseDocument(w.doc);
    w = Doc{};
    ++wrap_seq_;
  }
  Doc& d = docs_[k];
  if (d.doc) return show_doc(k);
  if (d.building || !resp_) return update_note();
  build(k);
}

void BodyView::build(int m) {
  Doc& d = docs_[m];
  if (d.doc || d.building || !resp_) return;
  const auto r = resp_;
  const size_t n = r->body.size();
  // No size hint: preallocating would zero-fill the buffer here on the UI thread (~5 ms for
  // 16 MB); the worker's AddData grows it instead.
  auto box = std::make_shared<LoaderBox>();
  box->l = static_cast<SciLoader*>(factory()->CreateLoader(0));
  if (!box->l) return show_message("Could not allocate a document for this body.");
  const uint64_t my = *gen_;
  const fmt::BodyKind kind = kind_;
  auto gen = gen_;
  const size_t cols = static_cast<size_t>(std::max(wrap_cols_, 40));
  // Outline is made from the Pretty document (its text and fold levels), already built.
  std::shared_ptr<const void> pre_owner;
  std::string_view pre_text;
  std::shared_ptr<const std::vector<int>> pre_levels;
  if (m == Outline) {
    pre_owner = docs_[Pretty].owner;
    pre_text = docs_[Pretty].text;
    pre_levels = docs_[Pretty].levels;
    if (!pre_owner || !pre_levels) return;
  }
  int src_lexer = 0;
  if (m == Wrapped) {
    const int src = base_mode();
    const Doc& s = docs_[src];
    if (!s.doc || !s.owner) return;
    pre_owner = s.full_owner ? s.full_owner : s.owner;
    pre_text = s.full_owner ? s.full : s.text;
    src_lexer = s.lexer;
    d.src = src;
    d.src_doc = s.doc;
  }
  if (m == Wrapped || m == RawWrap) d.cols = static_cast<int>(cols);
  const std::string filter = filter_;
  const uint64_t fseq = filter_seq_;
  const uint64_t wseq = wrap_seq_;
  auto job = [box, r, m, kind, gen, my, cols, pre_owner, pre_text, pre_levels, filter, fseq, wseq,
              src_lexer]() -> std::shared_ptr<Built> {
    if (*gen != my) return nullptr;
    auto b = std::make_shared<Built>();
    b->box = box;
    b->fseq = fseq;
    b->wseq = wseq;
    const std::string_view body(r->body);
    const auto t0 = Clock::now();
    b->owner = r;
    b->text = body;
    b->lexer = kind == fmt::BodyKind::Binary ? wxSTC_LEX_NULL : lexer_for(kind);
    auto formatted = [&](std::shared_ptr<std::string> s, const char* what) {
      b->owner = s;
      b->text = *s;
      b->note = std::string(what) + " " + fmt::ms(ms_between(t0, Clock::now()));
    };
    if (m == Pretty && kind == fmt::BodyKind::Json) {
      auto s = std::make_shared<std::string>();
      size_t err = 0;
      if (fmt::pretty_json(body, *s, &err)) formatted(s, "pretty");
      else b->note = "not valid JSON near byte " + std::to_string(err) + ": shown as received";
    } else if (m == Pretty && kind == fmt::BodyKind::Xml) {
      auto s = std::make_shared<std::string>();
      if (fmt::pretty_xml(body, *s)) formatted(s, "pretty");
      else b->note = "XML not indentable: shown as received";
    } else if (m == Pretty && kind == fmt::BodyKind::Html) {
      auto s = std::make_shared<std::string>();
      if (fmt::pretty_html(body, *s)) formatted(s, "pretty");
      else b->note = "no markup found: shown as received";
    } else if (m == Filter) {
      FilterResult fr = json_filter(body, filter);
      auto s = std::make_shared<std::string>();
      std::string note;
      if (!fr.ok) {
        *s = "// " + fr.error + "\n";
        note = "filter: " + fr.error;
      } else if (fr.matches == 0) {
        *s = "// nothing matches " + filter + "\n";
        note = "filter: no match";
      } else {
        *s = std::move(fr.json);
        note = std::to_string(fr.matches) + (fr.matches == 1 ? " match" : " matches") +
               (fr.truncated ? " (cut: too many)" : "");
      }
      formatted(s, "filtered in");
      b->note = note + " \xC2\xB7 " + fmt::ms(ms_between(t0, Clock::now()));
      b->lexer = wxSTC_LEX_JSON;
    } else if (m == Preview) {
      auto s = std::make_shared<std::string>();
      fmt::html_to_text(body, *s);
      formatted(s, "preview");
      b->note = "text preview, no scripts run \xC2\xB7 " + b->note;
      b->lexer = wxSTC_LEX_MARKDOWN;
    } else if (m == Hex) {
      auto s = std::make_shared<std::string>();
      bool trunc = false;
      fmt::hex_dump(body, *s, kHexMax, &trunc);
      b->owner = s;
      b->text = *s;
      b->lexer = wxSTC_LEX_NULL;
      if (trunc) b->note = "first " + fmt::human_bytes(kHexMax) + " shown";
    } else if (m == Outline) {
      auto s = std::make_shared<std::string>();
      auto map = std::make_shared<std::vector<int>>();
      const size_t n = fmt::fold_outline(pre_text, *pre_levels, *s, *map, kDisplayLine);
      b->owner = s;
      b->text = *s;
      b->line_map = map;
      b->note = "folded: " + std::to_string(n) + " values, one per line \xC2\xB7 double-click one to open it \xC2\xB7 " +
                fmt::ms(ms_between(t0, Clock::now()));
    } else if (m == RawWrap) {
      auto s = std::make_shared<std::string>();
      auto brk = std::make_shared<std::vector<size_t>>();
      fmt::break_long_lines(body, *s, cols, brk.get());
      b->owner = s;
      b->text = *s;
      b->breaks = brk;
      b->display_breaks = true;
      b->note = "wrapped at " + std::to_string(cols) + " columns";
    } else if (m == Wrapped) {
      auto s = std::make_shared<std::string>();
      auto brk = std::make_shared<std::vector<size_t>>();
      fmt::break_long_lines(pre_text, *s, cols, brk.get());
      b->owner = s;
      b->text = *s;
      b->breaks = brk;
      if (src_lexer != wxSTC_LEX_JSON) b->lexer = src_lexer;
      b->note = "wrapped at " + std::to_string(cols) + " columns on the worker \xC2\xB7 " +
                fmt::ms(ms_between(t0, Clock::now()));
    }
    // Scintilla lays out a whole line to paint any of it (a 10 MB minified line: ~0.9 s per
    // layout), so text with a line over kDisplayLine bytes (as received, or a long string in the
    // pretty form) gets display line breaks every kDisplayLine bytes.
    if (m == Wrapped) {
      b->full_owner = pre_owner;
      b->full = pre_text;
    } else if (m == RawWrap) {
      b->full_owner = r;
      b->full = body;
    } else if (m != Hex && m != Outline && fmt::longest_line(b->text) > kDisplayLine) {
      b->full_owner = b->owner;
      b->full = b->text;
      auto s = std::make_shared<std::string>();
      auto brk = std::make_shared<std::vector<size_t>>();
      const size_t breaks = fmt::break_long_lines(b->full, *s, kDisplayLine, brk.get());
      b->owner = s;
      b->text = *s;
      b->breaks = brk;
      b->display_breaks = true;
      if (!b->note.empty()) b->note += "; ";
      b->note += "lines over " + fmt::human_bytes(kDisplayLine) + " broken for display (" + std::to_string(breaks) +
                 "x, Copy / Save keep the exact bytes)";
    }
    // Heavy documents get no Scintilla wrap: Wrap on them is a Wrapped document when a line is
    // wider than the pane (one pass here, off the UI thread).
    if (b->text.size() > kHeavyLines) {  // fewer bytes cannot hold that many lines
      const fmt::LineStats st = fmt::line_stats(b->text);
      b->heavy = b->text.size() > kHeavyBytes || st.lines > kHeavyLines;
      b->longest = st.longest;
    }
    const auto t_fmt = Clock::now();
    // Style JSON and hex here instead of with a Scintilla lexer on the UI thread. The styles are
    // computed on a second thread while this one fills the document (they only need the text).
    const bool json = (kind == fmt::BodyKind::Json && m != Hex) || m == Filter;
    std::string styles;
    std::vector<int> levels;
    std::thread styler;
    const bool want_style = json || m == Hex;
    // A wrapped JSON document is styled as its source (the breaks may fall inside strings, which
    // would read as unterminated), then the styles are carried over to the inserted breaks.
    const bool expand = json && b->breaks && b->full_owner;
    if (want_style) {
      const std::string_view text = expand ? b->full : b->text;
      styler = std::thread([&styles, &levels, text, json] {
        if (json) fmt::style_json(text, styles, levels);
        else fmt::style_hex(text, styles);
      });
    }
    // Fill the document (Scintilla indexes lines as it goes) in chunks.
    for (size_t off = 0; off < b->text.size(); off += kChunk) {
      if (*gen != my) break;
      const size_t k = std::min(kChunk, b->text.size() - off);
      if (box->l->AddData(b->text.data() + off, static_cast<ptrdiff_t>(k)) != 0) {
        b->note = "out of memory loading the body";
        b->text = b->text.substr(0, off);
        break;
      }
    }
    if (styler.joinable()) styler.join();
    if (expand) fmt::expand_styles_for_breaks(b->full, *b->breaks, styles, levels);
    if (*gen != my) return nullptr;
    b->doc = box->l->ConvertToDocument();
    const auto t_fill = Clock::now();
    if (body.size() > kSyncBytes && !b->note.empty() && b->note.find("not ") == std::string::npos)
      b->note += " \xC2\xB7 load " + fmt::ms(ms_between(t_fmt, t_fill));
    if (want_style && styles.size() == b->text.size() && *gen == my) {
      if (SciDocument* sd = document_interface(b->doc, box->l, b->text.size())) {
        b->levels = std::make_shared<std::vector<int>>(std::move(levels));
        sd->StartStyling(0);
        for (size_t off = 0; off < styles.size(); off += kChunk) {
          if (*gen != my) return nullptr;
          sd->SetStyles(static_cast<ptrdiff_t>(std::min(kChunk, styles.size() - off)), styles.data() + off);
        }
        for (size_t l = 0; l < b->levels->size(); ++l)
          if ((*b->levels)[l] != 0x400) sd->SetLevel(static_cast<ptrdiff_t>(l), (*b->levels)[l]);
        b->prestyled = true;
        b->lexer = json ? wxSTC_LEX_JSON : kLexHex;
        if (body.size() > kSyncBytes && !b->note.empty() && b->note.find("not ") == std::string::npos)
          b->note += " \xC2\xB7 style " + fmt::ms(ms_between(t_fill, Clock::now()));
      }
    }
    return b;
  };
  if (n <= kSyncBytes) {
    if (auto b = job()) apply_built(my, m, *b);
    return;
  }
  d.building = true;
  update_note();
  worker_->post([job, alive = alive_, this, my, m] {
    auto b = job();
    if (!b) return;
    post_ui(alive, [this, b, my, m] { apply_built(my, m, *b); });
  });
}

void BodyView::apply_built(uint64_t gen, int m, Built& b) {
  if (*gen_ != gen) return;  // a newer body replaced this one; b.box releases the document
  if (m == Filter && b.fseq != filter_seq_) return;  // the filter changed meanwhile
  if ((m == Wrapped || m == RawWrap) && b.wseq != wrap_seq_) return;  // built for an older width / source
  Doc& d = docs_[m];
  d.building = false;
  d.heavy = b.heavy || m == Wrapped || m == RawWrap;
  d.longest = b.longest;
  d.breaks = std::move(b.breaks);
  d.full_owner = std::move(b.full_owner);
  d.full = d.full_owner ? b.full : std::string_view();
  d.doc = b.doc;
  b.box->l = nullptr;  // the reference is ours now (released in release_docs)
  d.owner = std::move(b.owner);
  d.text = b.text;
  d.lexer = b.lexer;
  d.prestyled = b.prestyled;
  d.display_breaks = b.display_breaks;
  d.levels = std::move(b.levels);
  d.line_map = std::move(b.line_map);
  d.note = std::move(b.note);
  d.lexer_set = false;
  if (m == Filter && filter_chips_) filter_chips_->set_note(d.note, d.note.rfind("filter:", 0) == 0 ? Tone::Warn : Tone::Dim);
  if (shown_mode() == m) show_doc(m);
  if (pending_reveal_ && pending_reveal_->mode == m && pending_reveal_->gen == *gen_) do_reveal(m);
}

void BodyView::show_doc(int m) {
  Doc& d = docs_[m];
  // Wrap on / off and width changes of a heavy document swap between it and its Wrapped form:
  // keep the place (positions of the source document; Raw <-> RawWrap by share of the lines).
  if (const int prev = on_screen(); keep_.src < 0 && prev >= 0 && prev != m) {
    const int top = stc_->PositionFromLine(stc_->DocLineFromVisible(stc_->GetFirstVisibleLine()));
    keep_ = Keep{};
    if (prev == RawWrap || (prev == Raw && m == RawWrap)) {
      keep_.src = Raw;
      keep_.ratio = static_cast<double>(stc_->GetFirstVisibleLine()) / std::max(1, stc_->GetLineCount());
    } else if (prev == Wrapped) {
      keep_.src = docs_[Wrapped].src;
      keep_.top = source_pos(top);
      keep_.caret = source_pos(stc_->GetCurrentPos());
      keep_.anchor = source_pos(stc_->GetAnchor());
    } else {
      keep_.src = prev;
      keep_.top = top;
      keep_.caret = stc_->GetCurrentPos();
      keep_.anchor = stc_->GetAnchor();
    }
  }
  const Keep kp = keep_;
  keep_ = Keep{};
  stc_->SetDocPointer(d.doc);
  stc_->SetScrollWidth(1);
  if (holding_) {
    holding_ = false;
    if (!stale_) cur_lexer_ = -2;  // undo the fade (apply_styles below recolours)
  }
  if (!d.lexer_set && !d.prestyled) {
    stc_->SetLexer(d.lexer);
    stc_->SetProperty("fold", "1");
    stc_->SetProperty("fold.compact", "0");
    stc_->SetProperty("fold.html", "1");
    stc_->SetProperty("lexer.json.allow.comments", "1");
    stc_->SetProperty("lexer.json.escape.sequence", "1");
    if (d.lexer == wxSTC_LEX_JSON) stc_->SetKeyWords(0, "false true null");
    d.lexer_set = true;
  }
  apply_styles(d.lexer);
  stc_->SetReadOnly(true);
  stc_->GotoPos(0);
  if (m != Wrapped && m != RawWrap) auto_wrap(d);
  update_margin();
  if (const int same = m == Wrapped ? d.src : m == RawWrap ? Raw : m; kp.src >= 0 && kp.src == same) {
    if (kp.ratio >= 0) {
      stc_->SetFirstVisibleLine(static_cast<int>(kp.ratio * stc_->GetLineCount()));
    } else {
      auto to = [&](int p) { return std::clamp(m == Wrapped ? wrapped_pos(p) : p, 0, stc_->GetLength()); };
      stc_->SetSelection(to(kp.anchor), to(kp.caret));
      stc_->SetFirstVisibleLine(stc_->LineFromPosition(to(kp.top)));
    }
  }
  display_break_note(d);
  update_note();
  if (findbar_->IsShown()) {
    schedule_count();
    highlight_visible();
  }
  start_styling();
  if (resp_ && (wrap_ || m == Wrapped || m == RawWrap)) sync_wrap();
}

void BodyView::auto_wrap(const Doc& d) {
  if (wrap_chosen_ || grpc_) return;
  bool w = false;
  if (d.lexer == wxSTC_LEX_MARKDOWN) {
    w = true;  // the Preview is prose
  } else if (&d != &docs_[Hex] && !d.text.empty() && d.text.size() <= kAutoWrapMax) {
    // Wrap when the longest line does not fit the pane (a one-line HTML page, a long string).
    const int cw = std::max(1, stc_->TextWidth(wxSTC_STYLE_DEFAULT, "0"));
    int avail = stc_->GetClientSize().x;
    if (avail < FromDIP(100) && GetParent()) avail = GetParent()->GetClientSize().x;
    avail -= FromDIP(70);  // line numbers + fold margin
    w = static_cast<long long>(fmt::longest_line_chars(d.text, kAutoWrapMax)) * cw > std::max(avail, FromDIP(200));
  }
  wrap_ = w;
  bar_->set_on(kWrap, wrap_);
}

void BodyView::start_styling() {
  if (!style_timer_.IsRunning() && stc_->GetEndStyled() < stc_->GetLength()) style_timer_.Start(1);
}

void BodyView::style_slice() {
  const int len = stc_->GetLength();
  const int from = stc_->GetEndStyled();
  if (from >= len || stc_->GetLexer() == wxSTC_LEX_NULL) {
    style_timer_.Stop();
    return;
  }
  const auto t0 = Clock::now();
  stc_->Colourise(from, std::min(len, from + style_chunk_));
  const double took = ms_between(t0, Clock::now());
  // Aim for ~4 ms per slice whatever the lexer's speed on this content.
  const double scale = std::clamp(4.0 / std::max(took, 0.05), 0.25, 4.0);
  style_chunk_ = std::clamp(static_cast<int>(style_chunk_ * scale), 16 << 10, 4 << 20);
}

void BodyView::update_margin() {
  const bool preview = cur_lexer_ == wxSTC_LEX_MARKDOWN && resp_;
  const int lines = stc_->GetLineCount();
  int digits = 1;
  for (int v = lines; v >= 10; v /= 10) ++digits;
  const bool hex = cur_lexer_ == kLexHex;
  const bool message = !resp_ && !grpc_;  // empty state / error text: no editor chrome
  if (preview || message) {
    stc_->SetMarginWidth(0, 0);
    stc_->SetMarginWidth(2, 0);
    stc_->SetMarginLeft(FromDIP(14));
  } else if (hex) {
    // The hex dump carries its own offsets: no line numbers, no fold column.
    stc_->SetMarginWidth(0, 0);
    stc_->SetMarginWidth(2, 0);
    stc_->SetMarginLeft(FromDIP(8));
  } else {
    stc_->SetMarginWidth(0, stc_->TextWidth(wxSTC_STYLE_LINENUMBER, "_" + wxString(std::max(digits, 3), '9')));
    stc_->SetMarginWidth(2, FromDIP(14));
    stc_->SetMarginLeft(FromDIP(2));
  }
  // Scintilla wraps on the UI thread, laying out every line up to the one asked for (Ctrl+End on
  // 1.3M lines: 170 ms, again after every width change): heavy documents never get its wrap.
  // Wrap on them shows a Wrapped / RawWrap document built on the worker, or nothing to do when
  // every line fits the pane.
  const int cur = on_screen();
  const bool heavy = cur >= 0 && docs_[cur].heavy;
  bar_->set_enabled(kWrap, !(heavy && (cur == Outline || cur == Hex)));
  stc_->SetWrapMode(wrap_ && !heavy ? wxSTC_WRAP_WORD : wxSTC_WRAP_NONE);
  stc_->SetWrapVisualFlags(wrap_ && !preview ? wxSTC_WRAPVISUALFLAG_END : wxSTC_WRAPVISUALFLAG_NONE);
}

void BodyView::set_mode_chips() {
  const bool http = resp_ != nullptr;
  bar_->set_visible(kPretty, http);
  bar_->set_visible(kRaw, http);
  bar_->set_visible(kHex, http);
  bar_->set_visible(kPreview, http && kind_ == fmt::BodyKind::Html);
  bar_->set_visible(kViewAs, http && kind_ != fmt::BodyKind::Binary);
  bar_->set_label(kViewAs, std::string(kind_forced_ ? "as " : "") + fmt::kind_name(kind_) + " \xE2\x96\xBE");
  bar_->set_on(kViewAs, kind_forced_);
  bar_->set_enabled(kPretty,
                    kind_ == fmt::BodyKind::Json || kind_ == fmt::BodyKind::Xml || kind_ == fmt::BodyKind::Html);
  bar_->set_on(kPretty, mode_ == Pretty);
  bar_->set_on(kRaw, mode_ == Raw);
  bar_->set_on(kHex, mode_ == Hex);
  bar_->set_on(kPreview, mode_ == Preview);
  bar_->set_visible(kTable, http && kind_ == fmt::BodyKind::Json);
  bar_->set_on(kTable, mode_ == Table);
  const bool has_doc = http || grpc_;
  bar_->set_on(kWrap, wrap_ && has_doc);
  bar_->set_on(kFold, outline_);
  bar_->set_enabled(kFold, has_doc && mode_ != Hex && mode_ != Preview);
  bar_->set_enabled(kUnfold, has_doc && mode_ != Hex && mode_ != Preview);
  bar_->set_enabled(kCopy, has_doc);
  bar_->set_visible(kSave, has_doc);
  // Empty state and error text: no editor chrome at all.
  for (int id : {kWrap, kFold, kUnfold, kFind, kCopy}) bar_->set_visible(id, has_doc);
  bar_->set_visible(kFilter, http && kind_ == fmt::BodyKind::Json);
  bar_->set_on(kFilter, !filter_.empty());
  bar_->set_label(kFilter, filter_.empty() ? std::string("Filter") : "Filter \xE2\x9C\x93");
  if (mode_ == Preview) bar_->set_visible(kWrap, false);  // the rendered page lays itself out
  // The table is a grid with its own columns: wrapping, folding and finding text do not apply to
  // it (Filter narrows its rows).
  if (mode_ == Table)
    for (int id : {kWrap, kFold, kUnfold, kFind}) bar_->set_visible(id, false);
}

void BodyView::update_note() {
  if (grpc_) {
    bar_->set_note(std::to_string(msg_pos_.size()) + (msg_pos_.size() == 1 ? " message" : " messages"));
    return;
  }
  if (!resp_) return;
  // The View-as chip already names the format: the note starts with the size.
  std::string n = kind_ == fmt::BodyKind::Binary
                      ? std::string(fmt::kind_name(kind_)) + " \xC2\xB7 " + fmt::human_bytes(resp_->body.size())
                      : fmt::human_bytes(resp_->body.size());
  const int k = shown_mode();
  const Doc& d = docs_[k];
  Tone tone = Tone::Dim;
  if (d.building) {
    n += k == Hex       ? " \xC2\xB7 building hex dump\xE2\x80\xA6"
         : k == Pretty  ? " \xC2\xB7 pretty-printing\xE2\x80\xA6"
         : k == Preview ? " \xC2\xB7 rendering preview\xE2\x80\xA6"
         : k == RawWrap || k == Wrapped ? " \xC2\xB7 wrapping\xE2\x80\xA6"
         : k == Outline ? " \xC2\xB7 folding\xE2\x80\xA6"
                        : " \xC2\xB7 loading\xE2\x80\xA6";
    if (holding_) n += " (previous body shown faded)";
    tone = Tone::Accent;
  } else if (!d.note.empty()) {
    n += " \xC2\xB7 " + d.note;
    if (d.note.find("not ") != std::string::npos) tone = Tone::Warn;
  }
  if (wrap_ && !d.building && d.heavy && k != Wrapped && k != RawWrap && (k == Pretty || k == Filter || k == Raw))
    n += " \xC2\xB7 wrap: every line fits the pane";
  if (d.doc && !d.building) n += " \xC2\xB7 " + std::to_string(stc_->GetLineCount()) + " lines";
  bar_->set_note(n, tone);
}

void BodyView::on_chip(int id) {
  switch (id) {
    case kPretty: request_mode(Pretty); break;
    case kRaw: request_mode(Raw); break;
    case kHex: request_mode(Hex); break;
    case kPreview: request_mode(Preview); break;
    case kTable: request_mode(Table); break;
    case kViewAs: view_as_menu(); break;
    case kWrap:
      wrap_ = !wrap_;
      wrap_chosen_ = true;
      bar_->set_on(kWrap, wrap_);
      // Heavy documents: the wrapped form is its own document, built on the worker at the pane
      // width (sync_wrap); small ones are wrapped by Scintilla.
      update_margin();
      sync_wrap();
      break;
    case kFold: fold_level1(); break;
    case kUnfold: unfold_all(); break;
    case kFind: findbar_->IsShown() ? close_find() : show_find(); break;
    case kFilter: (filterbar_ && filterbar_->IsShown()) ? close_filter() : show_filter(); break;
    case kFilterClose: close_filter(); break;
    case kFilterHelp:
      filter_chips_->set_note("$ body \xC2\xB7 .key ['k'] \xC2\xB7 [0] [-1] [0,2] [1:3] \xC2\xB7 [*] .* \xC2\xB7 ..key \xC2\xB7 .length · [?(@.a > 1 && @.b == 'x')]",
                              Tone::Dim);
      break;
    case kCopy: copy_all(); break;
    case kSave: save_body(); break;
    case kCase:
      match_case_ = !match_case_;
      find_chips_->set_on(kCase, match_case_);
      find_from(true, true);
      break;
    case kPrev:
    case kNext:
      find_from(id == kNext, false);
      break;
    case kClose: close_find(); break;
    default: break;
  }
  // A mode chip hides the control that had the keyboard focus (Pretty -> Table): the focus then
  // falls out of the body, and the body's own keys (Ctrl+Shift+F filter, Ctrl+F find) would go to
  // the frame's (the sidebar filter). The content now shown takes it.
  if (id == kPretty || id == kRaw || id == kHex || id == kTable || id == kWrap || id == kFold || id == kUnfold ||
      id == kCopy)
    CallAfter([this, alive = alive_] {
      if (!*alive) return;
      wxWindow* const foc = wxWindow::FindFocus();
      bool inside = false;
      for (wxWindow* f = foc; f && !inside; f = f->GetParent()) inside = f == this;
      if (inside && foc != bar_) return;
      if (table_win_ && table_win_->IsShown()) table_win_->SetFocus();
      else if (stc_->IsShown()) stc_->SetFocus();
    });
}

namespace {
#ifdef _WIN32
// UTF-16 copy of text in clipboard-ready global memory (GlobalAlloc is thread-safe: built on
// the worker, handed to the clipboard on the UI thread). Freed unless taken.
struct GlobalText {
  HGLOBAL h = nullptr;
  GlobalText() = default;
  GlobalText(const GlobalText&) = delete;
  GlobalText& operator=(const GlobalText&) = delete;
  ~GlobalText() {
    if (h) GlobalFree(h);
  }
  void fill(std::string_view text) {
    const int n = static_cast<int>(std::min<size_t>(text.size(), 0x7fffffff));
    const int wn = n ? MultiByteToWideChar(CP_UTF8, 0, text.data(), n, nullptr, 0) : 0;
    h = GlobalAlloc(GMEM_MOVEABLE, (static_cast<size_t>(wn) + 1) * sizeof(wchar_t));
    if (!h) return;
    auto* w = static_cast<wchar_t*>(GlobalLock(h));
    if (wn) MultiByteToWideChar(CP_UTF8, 0, text.data(), n, w, wn);
    w[wn] = 0;
    GlobalUnlock(h);
  }
  bool put(HWND owner) {
    if (!h || !OpenClipboard(owner)) return false;
    EmptyClipboard();
    const bool ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
    CloseClipboard();
    if (ok) h = nullptr;  // the clipboard owns it now
    return ok;
  }
};
#endif
}  // namespace

namespace {
// What Copy / Save give for a gRPC call: the messages' JSON, without the display annotations.
std::string grpc_clean_text(const GrpcResponse& r) {
  std::vector<const std::string*> js;
  for (const GrpcMessageEvent& m : r.messages)
    if (m.decode_error.empty()) js.push_back(&m.json);
  auto trim_end = [](std::string_view v) {
    while (!v.empty() && is_space(v.back())) v.remove_suffix(1);
    return v;
  };
  std::string out;
  if (js.empty()) {
    if (!r.error_details.empty() && r.error_details.rfind("hex:", 0) != 0 && fmt::pretty_json(r.error_details, out))
      return out;
    return r.error_details;
  }
  if (js.size() == 1) {
    out = trim_end(*js[0]);
    out += '\n';
    return out;
  }
  bool json = true;
  for (const std::string* j : js) {
    const size_t a = j->find_first_not_of(" \t\r\n");
    if (a == std::string::npos || ((*j)[a] != '{' && (*j)[a] != '[')) json = false;
  }
  size_t est = 8;
  for (const std::string* j : js) est += j->size() + 4;
  out.reserve(est);
  out += json ? "[\n" : "";
  for (size_t i = 0; i < js.size(); ++i) {
    out += trim_end(*js[i]);
    out += json ? (i + 1 < js.size() ? ",\n" : "\n]\n") : "\n";
  }
  return out;
}

// The live gRPC document without its annotations: "// ..." lines dropped, "/* #n ... */ " prefixes cut.
std::string strip_annotations(std::string_view t) {
  std::string out;
  out.reserve(t.size());
  while (!t.empty()) {
    const size_t nl = t.find('\n');
    std::string_view line = t.substr(0, nl == std::string_view::npos ? t.size() : nl + 1);
    t.remove_prefix(line.size());
    if (line.substr(0, 3) == "// ") continue;
    if (line.substr(0, 4) == "/* #") {
      const size_t e = line.find("*/ ");
      if (e != std::string_view::npos) line.remove_prefix(e + 3);
    }
    if (line == "\n" && (out.empty() || out.back() == '\n')) continue;
    out += line;
  }
  return out;
}
}  // namespace

std::pair<std::shared_ptr<const void>, std::string_view> BodyView::whole_text() {
  if (grpc_) {
    std::shared_ptr<std::string> clean;
    if (grpc_resp_) {
      clean = std::make_shared<std::string>(grpc_clean_text(*grpc_resp_));
    } else {
      const char* p = stc_->GetCharacterPointer();
      clean = std::make_shared<std::string>(
          strip_annotations(std::string_view(p ? p : "", p ? static_cast<size_t>(stc_->GetLength()) : 0)));
    }
    const std::string_view v = *clean;
    return {clean, v};
  }
  int k = shown_mode();
  if (k == Outline) k = Pretty;  // Copy / Save of the folded view: the whole pretty text
  const bool wrapped = k == Wrapped && docs_[Wrapped].src >= 0;
  if (wrapped) k = docs_[Wrapped].src;  // the wrapped view copies as its source, without the breaks
  const Doc& d = docs_[k];
  // The body exactly as received for Raw (also when broken for display) and while a view is
  // still being built; otherwise exactly what the view shows (pretty / hex / preview text).
  if (d.display_breaks && d.full_owner) return {d.full_owner, d.full};  // without the display breaks
  if (resp_ && (k == Raw || k == RawWrap || d.display_breaks || !d.doc)) return {resp_, resp_->body};
  if (d.owner && d.doc && (wrapped || static_cast<int>(d.text.size()) == stc_->GetLength())) return {d.owner, d.text};
  const char* p = stc_->GetCharacterPointer();
  auto snap = std::make_shared<std::string>(p ? p : "", static_cast<size_t>(stc_->GetLength()));
  const std::string_view v = *snap;
  return {snap, v};
}

void BodyView::copy_all() {
  const auto [owner, text] = whole_text();
  const std::string what = fmt::human_bytes(text.size());
  auto done = [this, what](bool ok) {
    if (ok) bar_->set_note("Copied " + what + (grpc_ ? " (message JSON)" : " (whole body)"), Tone::Ok);
    else bar_->set_note("Copy failed: the clipboard is in use by another program", Tone::Err);
  };
#ifdef _WIN32
  // UTF-8 -> UTF-16 of 19M characters takes ~200 ms: done on the worker for big bodies.
  if (text.size() <= kSyncBytes) {
    GlobalText g;
    g.fill(text);
    return done(g.put(GetHWND()));
  }
  bar_->set_note("Copying " + what + "\xE2\x80\xA6", Tone::Accent);
  worker_->post([owner = owner, text = text, alive = alive_, this, done] {
    auto g = std::make_shared<GlobalText>();
    g->fill(text);
    post_ui(alive, [this, g, done] { done(g->put(GetHWND())); });
  });
#else
  if (wxTheClipboard->Open()) {
    wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(text.data(), text.size())));
    wxTheClipboard->Close();
    done(true);
  } else {
    done(false);
  }
#endif
}

void BodyView::save_body() {
  std::shared_ptr<const void> owner;
  std::string_view text;
  std::string name = grpc_ ? "messages" : "response";
  if (resp_) {
    owner = resp_;
    text = resp_->body;
    // File name from the last path segment of the URL.
    std::string_view u = resp_->effective_url;
    u = u.substr(0, std::min(u.find('?'), u.find('#')));
    const size_t scheme = u.find("://");
    if (scheme != std::string_view::npos) u.remove_prefix(scheme + 3);
    const size_t slash = u.rfind('/');
    std::string seg(slash == std::string_view::npos ? std::string_view() : u.substr(slash + 1));
    for (char& c : seg)
      if (std::strchr("\\/:*?\"<>|", c) || static_cast<unsigned char>(c) < 0x20) c = '_';
    if (!seg.empty() && seg.find_first_not_of("._") != std::string::npos) name = seg;
  } else {
    const auto w = whole_text();
    owner = w.first;
    text = w.second;
  }
  if (name.find('.') == std::string::npos) {
    switch (kind_) {
      case fmt::BodyKind::Json: name += ".json"; break;
      case fmt::BodyKind::Xml: name += ".xml"; break;
      case fmt::BodyKind::Html: name += ".html"; break;
      case fmt::BodyKind::Binary: name += ".bin"; break;
      default: name += ".txt"; break;
    }
  }
  wxFileDialog dlg(this, "Save response body", wxEmptyString, wxString::FromUTF8(name), "All files (*.*)|*.*",
                   wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
  if (dlg.ShowModal() != wxID_OK) return;
  const std::wstring path = dlg.GetPath().ToStdWstring();
  const std::string fname = dlg.GetFilename().utf8_string();
  bar_->set_note("Saving " + fmt::human_bytes(text.size()) + "\xE2\x80\xA6", Tone::Accent);
  worker_->post([owner, text, path, fname, alive = alive_, this] {
    bool ok = false;
    {
      std::ofstream f(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
      if (f) {
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.close();
        ok = !f.fail();
      }
    }
    post_ui(alive, [this, ok, fname, n = text.size()] {
      if (ok) bar_->set_note("Saved " + fmt::human_bytes(n) + " to " + fname, Tone::Ok);
      else bar_->set_note("Could not write " + fname, Tone::Err);
    });
  });
}

void BodyView::on_margin_click(wxStyledTextEvent& e) {
  if (e.GetMargin() != 2) {
    e.Skip();
    return;
  }
  const int line = stc_->LineFromPosition(e.GetPosition());
  const int lvl = stc_->GetFoldLevel(line);
  if (!(lvl & wxSTC_FOLDLEVELHEADERFLAG)) return;
  if (!stc_->GetFoldExpanded(line)) {
    stc_->FoldLine(line, wxSTC_FOLDACTION_EXPAND);
    return;
  }
  // Folding: the line then ends in what it hides, "...}  3 keys" / "...]  120 items" / "...</div>".
  const int ls = stc_->PositionFromLine(line);
  const int le = stc_->GetLineEndPosition(line);
  std::string head = stc_->GetTextRange(std::max(ls, le - 512), le).utf8_string();
  while (!head.empty() && is_space(head.back())) head.pop_back();
  std::string text = "\xE2\x80\xA6";
  const char last = head.empty() ? 0 : head.back();
  if (last == '{' || last == '[') {
    text += last == '{' ? '}' : ']';
    const int depth = lvl & wxSTC_FOLDLEVELNUMBERMASK;
    const int end = stc_->GetLastChild(line, -1);
    const auto& lv = docs_[shown_mode()].levels;
    const bool vec = !grpc_ && lv && lv->size() == static_cast<size_t>(stc_->GetLineCount());
    if (vec || end - line <= 200000) {
      size_t count = 0;
      for (int l = line + 1; l <= end; ++l) {
        const int v = vec ? (*lv)[static_cast<size_t>(l)] : stc_->GetFoldLevel(l);
        if ((v & wxSTC_FOLDLEVELNUMBERMASK) == depth + 1) ++count;
      }
      if (count > 0) --count;  // the container's own closing line sits at that depth too
      text += "  " + std::to_string(count) +
              (last == '{' ? (count == 1 ? " key" : " keys") : (count == 1 ? " item" : " items"));
    }
  } else if (const size_t lt = head.rfind('<'); lt != std::string::npos && lt + 1 < head.size() && head[lt + 1] != '/') {
    size_t j = lt + 1;
    while (j < head.size() && (std::isalnum(static_cast<unsigned char>(head[j])) || head[j] == '-' || head[j] == ':'))
      ++j;
    if (j > lt + 1) text += "</" + head.substr(lt + 1, j - lt - 1) + ">";
  }
  stc_->ToggleFoldShowText(line, wxString::FromUTF8(text));
}

void BodyView::unfold_all() {
  if (outline_) {
    // Back to the full document, at the value that was at the top of the folded view.
    const auto& map = docs_[Outline].line_map;
    const int top = stc_->GetFirstVisibleLine();
    const int target = map && top >= 0 && static_cast<size_t>(top) < map->size() ? (*map)[static_cast<size_t>(top)] : 0;
    outline_ = false;
    request_mode(Pretty);
    if (stc_->GetDocPointer() == docs_[Pretty].doc) stc_->SetFirstVisibleLine(target);
    return;
  }
  void* doc = docs_[shown_mode()].doc;
  if (doc && doc == stc_->GetDocPointer()) {
    // Re-attaching the document resets Scintilla's fold state to "everything shown" in O(1);
    // FoldAll(EXPAND) instead marks each of the (hundreds of thousands of) headers one by one,
    // invalidating the margin every time (~0.8 s on a 10 MB JSON).
    const int first = stc_->DocLineFromVisible(stc_->GetFirstVisibleLine());
    const int pos = stc_->GetCurrentPos(), anchor = stc_->GetAnchor();
    stc_->SetDocPointer(doc);
    stc_->SetScrollWidth(1);
    stc_->SetReadOnly(true);
    stc_->SetSelection(anchor, pos);
    stc_->SetFirstVisibleLine(first);
    if (findbar_->IsShown()) highlight_visible();
  } else {
    wxBusyCursor busy;
    stc_->Hide();  // invalidating a hidden window is free
    stc_->FoldAll(wxSTC_FOLDACTION_EXPAND);
    stc_->Show();
  }
}

void BodyView::open_outline_line(int line) {
  const auto& map = docs_[Outline].line_map;
  if (!map || line < 0 || static_cast<size_t>(line) >= map->size()) return;
  const int target = (*map)[static_cast<size_t>(line)];
  outline_ = false;
  request_mode(Pretty);
  if (stc_->GetDocPointer() != docs_[Pretty].doc) return;
  stc_->SetFirstVisibleLine(std::max(0, target - 2));
  stc_->GotoLine(target);
  stc_->SetFocus();
}

void BodyView::fold_level1() {
  if (grpc_) {
    // A gRPC stream's messages are top-level values: one line per message.
    wxBusyCursor busy;
    stc_->FoldAll(wxSTC_FOLDACTION_CONTRACT);
    return;
  }
  if (outline_) return;
  const Doc& d = docs_[shown_mode()];
  const int lines = stc_->GetLineCount();
  // Very large JSON: Scintilla's fold state costs ~85 ns a line to set up the first time (112 ms
  // for 1.3M lines, in one block) and every fold call on a visible control 0.2-0.8 ms of
  // invalidation, so folding 130k values is seconds of UI time. The folded view is a document of
  // its own instead, built on the worker: one line per value, no Scintilla folds at all.
  if (mode_ == Pretty && kind_ == fmt::BodyKind::Json && d.levels && lines > 60000) {
    outline_ = true;
    request_mode(Pretty);
    return;
  }
  // Every level-1 header is marked contracted (a flag), then each root is contracted and
  // re-expanded once: Scintilla recomputes visibility in one pass (it skips contracted children).
  // All with the control hidden, where a fold call costs ~1 us instead of ~200 us.
  std::vector<int> heads, roots;
  auto visit = [&](int line, int lvl) {
    if (!(lvl & wxSTC_FOLDLEVELHEADERFLAG)) return;
    const int num = lvl & wxSTC_FOLDLEVELNUMBERMASK;
    if (num == wxSTC_FOLDLEVELBASE) roots.push_back(line);
    else if (num == wxSTC_FOLDLEVELBASE + 1) heads.push_back(line);
  };
  if (d.levels && d.levels->size() == static_cast<size_t>(lines)) {
    for (size_t l = 0; l < d.levels->size(); ++l) visit(static_cast<int>(l), (*d.levels)[l]);
  } else {
    stc_->Colourise(0, -1);
    for (int l = 0; l < lines; ++l) visit(l, stc_->GetFoldLevel(l));
  }
  if (heads.empty()) return;
  wxBusyCursor busy;
  const bool focused = stc_->HasFocus();
  stc_->Hide();
  for (int l : heads) stc_->SetFoldExpanded(l, false);
  for (int r : roots) stc_->FoldLine(r, wxSTC_FOLDACTION_CONTRACT);
  for (int r : roots) stc_->FoldLine(r, wxSTC_FOLDACTION_EXPAND);
  if (roots.empty())
    for (int l : heads) stc_->FoldLine(l, wxSTC_FOLDACTION_CONTRACT);
  stc_->Show();
  if (focused) stc_->SetFocus();
}

// ------------------------------------------------------------------ gRPC stream

void BodyView::begin_stream() {
  reset_find();
  ++*gen_;
  hide_html_preview();
  resp_.reset();
  grpc_resp_.reset();
  kind_ = fmt::BodyKind::Json;
  outline_ = false;
  grpc_ = true;
  msg_pos_.clear();
  mode_ = Pretty;
  wrap_ = false;
  wrap_chosen_ = false;
  follow_ = true;
  auto_first_ = 0;
  stc_->SetDocPointer(nullptr);
  stc_->SetScrollWidth(1);
  release_docs();
  Doc& d = docs_[Pretty];
  d.doc = factory()->CreateDocument();
  d.lexer = wxSTC_LEX_JSON;
  show_doc(Pretty);
  set_mode_chips();
  update_note();
}

void BodyView::append_raw(const std::string& s) {
  // Follow the newest message while the user has not scrolled away (or has come back to the end).
  const int first = stc_->GetFirstVisibleLine();
  const int rows = std::max(1, stc_->LinesOnScreen());
  const bool end_visible = stc_->VisibleFromDocLine(stc_->GetLineCount() - 1) < first + rows;
  if (first != auto_first_) follow_ = end_visible;
  stc_->SetReadOnly(false);
  stc_->AppendTextRaw(s.data(), static_cast<int>(s.size()));
  stc_->SetReadOnly(true);
  if (follow_ && msg_pos_.size() > 1) {
    const int last_vis = stc_->VisibleFromDocLine(stc_->GetLineCount() - 1);
    const int top = std::max(0, last_vis - rows + 1);
    if (top > first) stc_->SetFirstVisibleLine(top);
  }
  auto_first_ = stc_->GetFirstVisibleLine();
}

void BodyView::append_messages(const GrpcMessageEvent* m, size_t n, size_t first_index) {
  if (!grpc_) begin_stream();
  std::string s;
  size_t est = 0;
  for (size_t i = 0; i < n; ++i) est += m[i].json.size() + 64;
  s.reserve(est);
  const int base = stc_->GetLength();
  char head[160];
  std::string one;
  for (size_t i = 0; i < n; ++i) {
    const GrpcMessageEvent& e = m[i];
    msg_pos_.push_back(base + static_cast<int>(s.size()));
    const std::string at = "+" + fmt::ms(e.at_ms);
    const std::string size = fmt::human_bytes(e.wire_bytes);
    // A small message is one line: "/* #12  +360.4 ms  7 B */ {"n": 12}"; a big one gets a
    // header comment line, then its indented JSON and a blank line.
    if (e.decode_error.empty() && e.notes.empty() && fmt::compact_json(e.json, one, 160)) {
      std::snprintf(head, sizeof head, "/* #%-4zu %10s %8s */ ", first_index + i + 1, at.c_str(), size.c_str());
      s += head;
      s += one;
      s += '\n';
      continue;
    }
    std::snprintf(head, sizeof head, "// #%zu  %s  %s\n", first_index + i + 1, at.c_str(), size.c_str());
    s += head;
    for (const auto& note : e.notes) s += "// note: " + note + "\n";
    if (!e.decode_error.empty()) {
      s += "// decode error: " + e.decode_error + "\n";
      if (!e.raw_hex.empty()) s += "// raw: " + e.raw_hex + "\n";
    } else {
      s += e.json;
      if (s.empty() || s.back() != '\n') s += '\n';
    }
    s += '\n';
  }
  append_raw(s);
  update_margin();
  update_note();
  start_styling();
}

namespace {
// "// " before every line of text: a multi-line status message stays one comment.
std::string comment_lines(const std::string& text) {
  std::string s = "// ";
  for (char c : text) {
    if (c == '\r') continue;
    s += c;
    if (c == '\n') s += "// ";
  }
  while (s.size() > 3 && s.compare(s.size() - 3, 3, "// ") == 0 && s[s.size() - 4] == '\n') s.resize(s.size() - 4);
  s += '\n';
  return s;
}
}  // namespace

void BodyView::append_note(const std::string& text) {
  if (!grpc_) begin_stream();
  append_raw(comment_lines(text));
  update_margin();
  start_styling();
}

void BodyView::finish_grpc(std::shared_ptr<const GrpcResponse> r) {
  if (!grpc_ || !r) return;
  grpc_resp_ = std::move(r);
  const auto& ms = grpc_resp_->messages;
  if (!grpc_resp_->ok || ms.size() != 1 || msg_pos_.size() != 1 || !ms[0].decode_error.empty() ||
      !ms[0].notes.empty() || ms[0].json.size() > (4u << 20))
    return;
  // Unary: the plain JSON, nothing else (offset and size are in the status strip and Messages tab).
  std::string j = ms[0].json;
  while (!j.empty() && is_space(j.back())) j.pop_back();
  j += '\n';
  stc_->SetReadOnly(false);
  stc_->ClearAll();
  stc_->AppendTextRaw(j.data(), static_cast<int>(j.size()));
  stc_->SetReadOnly(true);
  stc_->GotoPos(0);
  msg_pos_.assign(1, 0);
  update_margin();
  update_note();
  start_styling();
}

void BodyView::show_grpc_error(const std::string& status_line, const std::string& details) {
  begin_stream();
  std::string s = comment_lines(status_line);
  std::string pretty;
  if (!details.empty()) {
    if (details.rfind("hex:", 0) != 0 && fmt::pretty_json(details, pretty)) s += "// details (google.rpc.Status)\n" + pretty;
    else s += comment_lines("details: " + details);
  }
  append_raw(s);
  wrap_ = true;  // an error status is prose: never cut off at the pane edge
  bar_->set_on(kWrap, true);
  update_margin();
  start_styling();
  bar_->set_note(details.empty() ? "error status" : "error status \xC2\xB7 details decoded", Tone::Err);
}

void BodyView::goto_message(size_t i) {
  if (i >= msg_pos_.size()) return;
  const int pos = msg_pos_[i];
  const int line = stc_->LineFromPosition(pos);
  stc_->EnsureVisible(line);
  stc_->SetFirstVisibleLine(stc_->VisibleFromDocLine(line));
  stc_->SetSelection(pos, stc_->GetLineEndPosition(line));
}

// ------------------------------------------------------------------ reveal (Tests tab)

void BodyView::reveal_json(std::vector<PathStep> path, long message, const std::string& label) {
  pending_reveal_.reset();
  if (grpc_) {
    if (msg_pos_.empty()) return bar_->set_note("no message to show " + label + " in", Tone::Warn);
    if (message == -2) return goto_message(0);
    const size_t i = message < 0 ? 0 : static_cast<size_t>(message);
    if (i >= msg_pos_.size())
      return bar_->set_note("message #" + std::to_string(i + 1) + " was not received", Tone::Warn);
    const int from = msg_pos_[i];
    const int to = i + 1 < msg_pos_.size() ? msg_pos_[i + 1] : stc_->GetLength();
    const wxCharBuffer buf = stc_->GetTextRangeRaw(from, to);
    const std::string_view text(buf.data(), buf.length());
    // Skip the "/* #n +ms size */" or "// #n ..." annotation of the message.
    size_t off = 0;
    for (;;) {
      while (off < text.size() && is_space(text[off])) ++off;
      if (text.substr(off, 2) == "/*") {
        const size_t e = text.find("*/", off + 2);
        off = e == std::string_view::npos ? text.size() : e + 2;
      } else if (text.substr(off, 2) == "//") {
        const size_t e = text.find('\n', off);
        off = e == std::string_view::npos ? text.size() : e + 1;
      } else {
        break;
      }
    }
    size_t len = 0, deep = 0;
    const size_t at = json_locate(text.substr(off), path, &len, &deep);
    const bool exact = at != std::string_view::npos;
    const std::string why =
        exact ? std::string() : json_path_miss(text.substr(off), path, message < 0 ? std::string("res.body")
                                                                           : "res.messages[" + std::to_string(i) + "]");
    select_found(from + static_cast<int>(off + (exact ? at : deep)), exact ? static_cast<int>(len) : 0, exact, label,
                 path_string(path), why);
    return;
  }
  if (!resp_) return;
  const int k = kind_ == fmt::BodyKind::Json ? Pretty : Raw;
  pending_reveal_ = std::make_unique<Reveal>(Reveal{std::move(path), label, k, *gen_});
  if (shown_mode() != k) {
    outline_ = false;
    request_mode(k);  // may build it on the worker: do_reveal runs from apply_built then
  }
  if (docs_[k].doc && pending_reveal_) do_reveal(k);
}

void BodyView::do_reveal(int k) {
  if (!pending_reveal_) return;
  Reveal rv = std::move(*pending_reveal_);
  pending_reveal_.reset();
  const Doc& d = docs_[k];
  if (!d.doc) return;
  // Looked up in the text without display breaks (a break may cut a string), then mapped.
  auto owner = d.full_owner ? d.full_owner : d.owner;
  const std::string_view text = d.full_owner ? d.full : d.text;
  const std::string what = path_string(rv.path);
  auto apply = [this, rv, what, k](uint64_t gen, size_t at, size_t len, size_t deep, const std::string& why) {
    if (gen != *gen_) return;
    const bool exact = at != std::string_view::npos;
    int from = static_cast<int>(exact ? at : deep);
    int n = exact ? static_cast<int>(len) : 0;
    if (const Doc& dk = docs_[k]; dk.full_owner && dk.breaks) {
      const int to = past_breaks(*dk.breaks, from + n);
      from = past_breaks(*dk.breaks, from);
      n = to - from;
    }
    if (on_screen() == Wrapped && docs_[Wrapped].src == k) {  // shown wrapped: its own positions
      const int to = wrapped_pos(from + n);
      from = wrapped_pos(from);
      n = to - from;
    }
    select_found(from, n, exact, rv.label, what, why);
  };
  if (text.size() <= kSyncBytes) {
    size_t len = 0, deep = 0;
    const size_t at = json_locate(text, rv.path, &len, &deep);
    return apply(*gen_, at, len, deep, at == std::string_view::npos ? json_path_miss(text, rv.path) : std::string());
  }
  // A big document: walk it on the worker (memory speed, but not on the UI thread).
  const uint64_t my = *gen_;
  bar_->set_note("finding " + what + "\xE2\x80\xA6", Tone::Accent);
  worker_->post([owner, text, path = rv.path, my, apply, alive = alive_]() {
    size_t len = 0, deep = 0;
    const size_t at = json_locate(text, path, &len, &deep);
    std::string why = at == std::string_view::npos ? json_path_miss(text, path) : std::string();
    post_ui(alive, [apply, my, at, len, deep, why = std::move(why)] { apply(my, at, len, deep, why); });
  });
}

void BodyView::select_found(int at, int len, bool exact, const std::string& label, const std::string& what,
                            const std::string& why) {
  const int n = stc_->GetLength();
  at = std::clamp(at, 0, n);
  len = std::clamp(len, 0, n - at);
  const int line = stc_->LineFromPosition(at);
  stc_->EnsureVisible(line);  // opens folds around it
  const int vis = stc_->VisibleFromDocLine(line);
  stc_->SetFirstVisibleLine(std::max(0, vis - stc_->LinesOnScreen() / 3));
  if (len > 0) {
    // The whole value (an object or array too), unless it is huge: then its first line.
    const int end = len <= (64 << 10) ? at + len : std::min(at + len, stc_->GetLineEndPosition(line));
    stc_->SetSelection(at, std::max(end, at + 1));
  } else {
    stc_->SetSelection(at, stc_->GetLineEndPosition(line));
  }
  stc_->SetFocus();
  const std::string head = label.empty() ? what : label;
  // An earlier explanation box goes (it belongs to another check).
  if (miss_line_ >= 0 && miss_doc_ == stc_->GetDocPointer() && miss_line_ < stc_->GetLineCount())
    stc_->AnnotationSetText(miss_line_, wxEmptyString);
  miss_line_ = -1;
  if (exact) {
    bar_->set_note("line " + std::to_string(line + 1) + ": " + head, Tone::Accent);
    return;
  }
  // The chip bar has room for a short note only: the full reason goes into a box right under the
  // line, where it cannot be cut off.
  bar_->set_note(what + " not found (line " + std::to_string(line + 1) + ")", Tone::Warn);
  const Theme& th = Theme::get();
  stc_->StyleSetForeground(wxSTC_STYLE_CALLTIP, th.warn);
  stc_->StyleSetBackground(wxSTC_STYLE_CALLTIP, stc_->StyleGetBackground(wxSTC_STYLE_DEFAULT));
  stc_->StyleSetFont(wxSTC_STYLE_CALLTIP, stc_->StyleGetFont(wxSTC_STYLE_DEFAULT));
  const std::string box = "\xE2\x86\x91 " + head + " not found: the deepest part that exists is selected." +
                          (why.empty() ? std::string() : "\n  " + why);
  stc_->AnnotationSetText(line, wxString::FromUTF8(box));
  stc_->AnnotationSetStyle(line, wxSTC_STYLE_CALLTIP);
  stc_->AnnotationSetVisible(wxSTC_ANNOTATION_BOXED);
  miss_line_ = line;
  miss_doc_ = stc_->GetDocPointer();
}

void BodyView::display_break_note(const Doc& d) {
  // Display line breaks in a body with very long lines: the line numbers are display lines, not
  // lines of the body. Said once, as a boxed annotation above the first line.
  if (!d.display_breaks || &d == &docs_[RawWrap]) return;
  const Theme& th = Theme::get();
  stc_->StyleSetForeground(wxSTC_STYLE_CALLTIP, th.warn);
  stc_->StyleSetBackground(wxSTC_STYLE_CALLTIP, stc_->StyleGetBackground(wxSTC_STYLE_DEFAULT));
  stc_->StyleSetFont(wxSTC_STYLE_CALLTIP, stc_->StyleGetFont(wxSTC_STYLE_DEFAULT));
  bool one_line = false;
  if (resp_ && (d.full.empty() || d.full.data() == resp_->body.data())) {
    const std::string_view b = resp_->body;
    // Newlines in the first 64 KB only (no per-byte work over a 10 MB body here).
    const std::string_view head = b.substr(0, 1u << 16);
    one_line = head.find('\n') == std::string_view::npos || head.find('\n') + 1 >= b.size();
  }
  const std::string text = std::string(one_line ? "One line of " + fmt::human_bytes(resp_ ? resp_->body.size() : 0)
                                                : "Lines longer than " + fmt::human_bytes(kDisplayLine)) +
                           ", broken every ~" + fmt::human_bytes(kDisplayLine) +
                           " for display only.\nLine numbers count display lines; Copy and Save keep the exact bytes.";
  stc_->AnnotationSetText(0, wxString::FromUTF8(text));
  stc_->AnnotationSetStyle(0, wxSTC_STYLE_CALLTIP);
  stc_->AnnotationSetVisible(wxSTC_ANNOTATION_BOXED);
}

void BodyView::fade_styles() {
  // Every style faded towards the background: still readable, plainly not the current answer.
  const wxColour bg = stc_->StyleGetBackground(wxSTC_STYLE_DEFAULT);
  const wxColour fg = stc_->StyleGetForeground(wxSTC_STYLE_DEFAULT);
  auto m = [](unsigned char a, unsigned char b) { return static_cast<unsigned char>(a + (b - a) * 38 / 100); };
  const wxColour faded(m(bg.Red(), fg.Red()), m(bg.Green(), fg.Green()), m(bg.Blue(), fg.Blue()));
  for (int st = 0; st < wxSTC_STYLE_DEFAULT; ++st) stc_->StyleSetForeground(st, faded);
}

void BodyView::set_stale(bool stale) {
  // Nothing shown: nothing to mistake for the answer (a message such as an error does count).
  if (stale && !resp_ && !grpc_ && stc_->GetLength() == 0) return;
  if (stale == stale_) return;
  stale_ = stale;
  if (stale) {
    fade_styles();
    bar_->set_note("previous response \xC2\xB7 waiting for the new one\xE2\x80\xA6", Tone::Warn);
  } else {
    const int lexer = cur_lexer_;
    cur_lexer_ = -2;  // force apply_styles to recolour
    apply_styles(lexer);
    if (resp_ || grpc_) update_note();
    else bar_->set_note("");
  }
  if (html_) html_->Enable(!stale);
}

// ------------------------------------------------------------------ search

// ------------------------------------------------------------------ table

// A custom-drawn virtual grid: only the visible cells are drawn (a few hundred DrawText calls per
// paint), unlike a native list view, whose paint asks for every cell several times over.
class JsonTable : public wxWindow {
 public:
  explicit JsonTable(wxWindow* parent)
      : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxHSCROLL | wxWANTS_CHARS | wxBORDER_NONE),
        alive_(std::make_shared<bool>(true)) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    const Theme& th = Theme::get();
    bg_ = th.dark ? wxColour(30, 30, 30) : *wxWHITE;
    SetBackgroundColour(bg_);
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      update_scrollbars();
      Refresh(false);
    });
    Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& e) {
      const int lines = e.GetWheelRotation() / std::max(1, e.GetWheelDelta()) * std::max(1, e.GetLinesPerAction());
      if (e.GetWheelAxis() == wxMOUSE_WHEEL_HORIZONTAL) scroll_to(top_, left_ + lines * FromDIP(40));
      else scroll_to(top_ - lines, left_);
    });
    auto on_scroll = [this](wxScrollWinEvent& e) {
      const bool v = e.GetOrientation() == wxVERTICAL;
      const wxEventType t = e.GetEventType();
      int pos = v ? top_ : left_;
      const int line = v ? 1 : FromDIP(40);
      const int page = v ? std::max(1, visible_rows() - 1) : std::max(1, GetClientSize().x - FromDIP(40));
      if (t == wxEVT_SCROLLWIN_LINEUP) pos -= line;
      else if (t == wxEVT_SCROLLWIN_LINEDOWN) pos += line;
      else if (t == wxEVT_SCROLLWIN_PAGEUP) pos -= page;
      else if (t == wxEVT_SCROLLWIN_PAGEDOWN) pos += page;
      else if (t == wxEVT_SCROLLWIN_TOP) pos = 0;
      else if (t == wxEVT_SCROLLWIN_BOTTOM) pos = 1 << 30;
      else pos = e.GetPosition();
      if (v) scroll_to(pos, left_);
      else scroll_to(top_, pos);
    };
    for (wxEventType t : {wxEVT_SCROLLWIN_TOP, wxEVT_SCROLLWIN_BOTTOM, wxEVT_SCROLLWIN_LINEUP, wxEVT_SCROLLWIN_LINEDOWN,
                          wxEVT_SCROLLWIN_PAGEUP, wxEVT_SCROLLWIN_PAGEDOWN, wxEVT_SCROLLWIN_THUMBTRACK,
                          wxEVT_SCROLLWIN_THUMBRELEASE})
      Bind(wxEventTypeTag<wxScrollWinEvent>(t), on_scroll);
    Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
      SetFocus();
      const wxPoint p = e.GetPosition();
      if (p.y < head_h()) {
        if (const int c = col_at(p.x); c >= 0) sort_by(c);
        return;
      }
      const long r = top_ + (p.y - head_h()) / row_h();
      if (m_ && r >= 0 && static_cast<size_t>(r) < m_->cells.size()) {
        if (e.ShiftDown() && sel_a_ >= 0) sel_b_ = r;
        else sel_a_ = sel_b_ = r;
        Refresh(false);
      }
    });
    Bind(wxEVT_MOTION, [this](wxMouseEvent& e) { hover_tip(e.GetPosition()); });
    Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
      const int k = e.GetKeyCode();
      if (e.GetModifiers() == wxMOD_CONTROL && k == 'C') return copy_selected();
      if (e.GetModifiers() == wxMOD_CONTROL && k == 'A' && m_) {
        sel_a_ = 0;
        sel_b_ = static_cast<long>(m_->cells.size()) - 1;
        return Refresh(false);
      }
      const int page = std::max(1, visible_rows() - 1);
      long cur = sel_b_ < 0 ? top_ : sel_b_;
      if (k == WXK_DOWN) cur += 1;
      else if (k == WXK_UP) cur -= 1;
      else if (k == WXK_PAGEDOWN) cur += page;
      else if (k == WXK_PAGEUP) cur -= page;
      else if (k == WXK_HOME && e.ControlDown()) cur = 0;
      else if (k == WXK_END && e.ControlDown()) cur = 1L << 30;
      else if (k == WXK_LEFT) return scroll_to(top_, left_ - FromDIP(40));
      else if (k == WXK_RIGHT) return scroll_to(top_, left_ + FromDIP(40));
      else return e.Skip();
      if (!m_ || m_->cells.empty()) return;
      cur = std::clamp<long>(cur, 0, static_cast<long>(m_->cells.size()) - 1);
      if (e.ShiftDown() && sel_a_ >= 0) sel_b_ = cur;
      else sel_a_ = sel_b_ = cur;
      if (cur < top_) scroll_to(cur, left_);
      else if (cur >= top_ + visible_rows()) scroll_to(cur - visible_rows() + 1, left_);
      Refresh(false);
    });
  }
  ~JsonTable() override { *alive_ = false; }
  std::function<void(const std::string&, Tone)> on_note;

  void set(std::shared_ptr<const TableModel> m) {
    m_ = std::move(m);
    ++sort_seq_;
    sort_col_ = 0;
    asc_ = true;
    perm_.clear();
    top_ = left_ = 0;
    sel_a_ = sel_b_ = -1;
    // Column widths from the text lengths of the header and the first rows (character count x the
    // average character width: no text measuring per cell).
    const int cw = std::max(1, GetCharWidth());
    widths_.clear();
    size_t digits = std::to_string(m_->cells.size()).size();
    widths_.push_back(static_cast<int>(digits + 2) * cw + FromDIP(8));
    for (size_t c = 0; c < m_->cols.size(); ++c) {
      size_t chars = m_->cols[c].size() + 2;
      for (size_t r = 0; r < m_->cells.size() && r < 400; ++r)
        if (c < m_->cells[r].size()) chars = std::max(chars, m_->cells[r][c].size());
      widths_.push_back(std::clamp(static_cast<int>(chars) * cw + FromDIP(16), FromDIP(48), FromDIP(420)));
    }
    update_scrollbars();
    Refresh(false);
  }

 private:
  int row_h() const { return GetCharHeight() + FromDIP(5); }
  int head_h() const { return row_h() + FromDIP(2); }
  int visible_rows() const { return std::max(1, (GetClientSize().y - head_h()) / row_h()); }
  int total_w() const {
    int w = 0;
    for (int x : widths_) w += x;
    return w;
  }
  int col_at(int x) const {
    int cx = -left_;
    for (size_t c = 0; c < widths_.size(); ++c) {
      if (x >= cx && x < cx + widths_[c]) return static_cast<int>(c);
      cx += widths_[c];
    }
    return -1;
  }
  size_t row_at(long item) const {
    return perm_.empty() ? static_cast<size_t>(item) : perm_[static_cast<size_t>(item)];
  }
  const std::string& cell(size_t r, size_t c) const {
    static const std::string none;
    const auto& row = m_->cells[r];
    return c < row.size() ? row[c] : none;
  }
  void update_scrollbars() {
    const long rows = m_ ? static_cast<long>(m_->cells.size()) : 0;
    const int vis = visible_rows();
    top_ = std::clamp<long>(top_, 0, std::max<long>(0, rows - vis));
    SetScrollbar(wxVERTICAL, static_cast<int>(top_), vis, static_cast<int>(std::max<long>(rows, 1)));
    const int cw = GetClientSize().x, tw = total_w();
    left_ = std::clamp(left_, 0, std::max(0, tw - cw));
    SetScrollbar(wxHORIZONTAL, left_, cw, std::max(tw, 1));
  }
  void scroll_to(long top, int left) {
    const long rows = m_ ? static_cast<long>(m_->cells.size()) : 0;
    top = std::clamp<long>(top, 0, std::max<long>(0, rows - visible_rows()));
    left = std::clamp(left, 0, std::max(0, total_w() - GetClientSize().x));
    if (top == top_ && left == left_) return;
    top_ = top;
    left_ = left;
    SetScrollPos(wxVERTICAL, static_cast<int>(top_));
    SetScrollPos(wxHORIZONTAL, left_);
    Refresh(false);
  }
  void paint() {
    wxAutoBufferedPaintDC dc(this);
    const Theme& th = Theme::get();
    const wxSize cs = GetClientSize();
    dc.SetBackground(wxBrush(bg_));
    dc.Clear();
    if (!m_) return;
    const wxColour head_bg = th.dark ? wxColour(40, 40, 44) : wxColour(244, 244, 247);
    const wxColour zebra = th.dark ? wxColour(35, 35, 38) : wxColour(249, 249, 251);
    const wxColour sel = th.dark ? wxColour(38, 79, 120) : wxColour(205, 228, 255);
    const int rh = row_h(), hh = head_h(), pad = FromDIP(6);
    const long n = static_cast<long>(m_->cells.size());
    const long last = std::min<long>(n, top_ + visible_rows() + 1);
    const long s0 = std::min(sel_a_, sel_b_), s1 = std::max(sel_a_, sel_b_);
    dc.SetPen(*wxTRANSPARENT_PEN);
    for (long i = top_; i < last; ++i) {
      const int y = hh + static_cast<int>(i - top_) * rh;
      const bool selected = sel_a_ >= 0 && i >= s0 && i <= s1;
      if (selected || (i % 2)) {
        dc.SetBrush(wxBrush(selected ? sel : zebra));
        dc.DrawRectangle(0, y, cs.x, rh);
      }
    }
    dc.SetFont(GetFont());
    int x = -left_;
    for (size_t c = 0; c < widths_.size(); ++c) {
      const int w = widths_[c];
      if (x + w > 0 && x < cs.x) {
        dc.SetClippingRegion(x, hh, w - FromDIP(2), cs.y - hh);
        dc.SetTextForeground(c == 0 ? th.dim : th.fg);
        for (long i = top_; i < last; ++i) {
          const size_t r = row_at(i);
          const int y = hh + static_cast<int>(i - top_) * rh + (rh - dc.GetCharHeight()) / 2;
          if (c == 0) {
            const wxString t = wxString::Format("%zu", r);
            dc.DrawText(t, x + w - pad - dc.GetTextExtent(t).x, y);
          } else {
            const std::string& v = cell(r, c - 1);
            if (!v.empty()) dc.DrawText(wxString::FromUTF8(v.data(), std::min<size_t>(v.size(), 300)), x + pad, y);
          }
        }
        dc.DestroyClippingRegion();
      }
      x += w;
    }
    // Header (drawn last: always on top), with the sort mark.
    dc.SetBrush(wxBrush(head_bg));
    dc.DrawRectangle(0, 0, cs.x, hh);
    dc.SetPen(wxPen(th.grid));
    dc.DrawLine(0, hh - 1, cs.x, hh - 1);
    wxFont bold = GetFont().Bold();
    dc.SetFont(bold);
    x = -left_;
    for (size_t c = 0; c < widths_.size(); ++c) {
      const int w = widths_[c];
      if (x + w > 0 && x < cs.x) {
        wxString t = c == 0 ? wxString("#") : wxString::FromUTF8(m_->cols[c - 1]);
        if (static_cast<int>(c) == sort_col_ && (c > 0 || !asc_))
          t += wxString::FromUTF8(asc_ ? " \xE2\x96\xB2" : " \xE2\x96\xBC");
        dc.SetClippingRegion(x, 0, w - FromDIP(2), hh);
        dc.SetTextForeground(th.fg);
        dc.DrawText(t, c == 0 ? x + w - pad - dc.GetTextExtent(t).x : x + pad, (hh - dc.GetCharHeight()) / 2);
        dc.DestroyClippingRegion();
        dc.SetPen(wxPen(th.grid));
        dc.DrawLine(x + w - 1, FromDIP(4), x + w - 1, hh - FromDIP(4));
      }
      x += w;
    }
  }
  void hover_tip(const wxPoint& p) {
    if (!m_) return;
    const int c = col_at(p.x);
    const long i = p.y < head_h() ? -1 : top_ + (p.y - head_h()) / row_h();
    const long key = c < 0 ? -2 : i * 1000 + c;
    if (key == tip_key_) return;
    tip_key_ = key;
    if (c <= 0 || i < -1 || (i >= 0 && static_cast<size_t>(i) >= m_->cells.size())) return UnsetToolTip();
    if (i < 0) return SetToolTip(wxString::FromUTF8(m_->cols[static_cast<size_t>(c - 1)] + "\nClick to sort by it."));
    const std::string& v = cell(row_at(i), static_cast<size_t>(c - 1));
    if (v.size() * static_cast<size_t>(GetCharWidth()) + FromDIP(12) > static_cast<size_t>(widths_[static_cast<size_t>(c)]))
      SetToolTip(wxString::FromUTF8(v));
    else
      UnsetToolTip();
  }
  void sort_by(int col) {
    if (!m_) return;
    if (col == sort_col_) asc_ = !asc_;
    else {
      sort_col_ = col;
      asc_ = true;
    }
    Refresh(false);
    const uint64_t seq = ++sort_seq_;
    auto m = m_;
    const bool asc = asc_;
    auto work = [m, col, asc] {
      std::vector<uint32_t> p(m->cells.size());
      for (size_t i = 0; i < p.size(); ++i) p[i] = static_cast<uint32_t>(i);
      if (col > 0) {
        const size_t c = static_cast<size_t>(col - 1);
        static const std::string none;
        auto at = [&](uint32_t r) -> const std::string& { return c < m->cells[r].size() ? m->cells[r][c] : none; };
        std::stable_sort(p.begin(), p.end(), [&](uint32_t a, uint32_t b) {
          return asc ? table_cell_less(at(a), at(b)) : table_cell_less(at(b), at(a));
        });
      } else if (!asc) {
        std::reverse(p.begin(), p.end());
      }
      return p;
    };
    if (m->cells.size() <= 20000) {
      perm_ = work();
      sel_a_ = sel_b_ = -1;
      Refresh(false);
      return;
    }
    // A big table sorts off the UI thread.
    if (on_note) on_note("sorting " + std::to_string(m->cells.size()) + " rows\xE2\x80\xA6", Tone::Accent);
    std::thread([work, seq, alive = alive_, this] {
      auto p = std::make_shared<std::vector<uint32_t>>(work());
      if (!wxTheApp) return;
      wxTheApp->CallAfter([alive, this, seq, p] {
        if (!*alive || seq != sort_seq_) return;
        perm_ = std::move(*p);
        sel_a_ = sel_b_ = -1;
        Refresh(false);
        if (on_note) on_note("sorted", Tone::Dim);
      });
    }).detach();
  }
  void copy_selected() {
    if (!m_ || sel_a_ < 0) return;
    std::string s;
    for (size_t c = 0; c < m_->cols.size(); ++c) s += (c ? "\t" : "") + m_->cols[c];
    s += "\n";
    const long a = std::min(sel_a_, sel_b_), b = std::max(sel_a_, sel_b_);
    for (long i = a; i <= b && static_cast<size_t>(i) < m_->cells.size(); ++i) {
      const auto& row = m_->cells[row_at(i)];
      for (size_t c = 0; c < row.size(); ++c) s += (c ? "\t" : "") + row[c];
      s += "\n";
    }
    if (wxTheClipboard->Open()) {
      wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(s)));
      wxTheClipboard->Close();
      if (on_note) on_note("copied " + std::to_string(b - a + 1) + (b == a ? " row" : " rows") + " (tab-separated)", Tone::Ok);
    }
  }
  std::shared_ptr<const TableModel> m_;
  std::vector<uint32_t> perm_;
  std::vector<int> widths_;  // "#" first
  wxColour bg_;
  long top_ = 0;
  int left_ = 0;
  long sel_a_ = -1, sel_b_ = -1;
  long tip_key_ = -3;
  int sort_col_ = 0;
  bool asc_ = true;
  uint64_t sort_seq_ = 0;
  std::shared_ptr<bool> alive_;
};

void BodyView::show_table() {
  if (!table_) {
    table_ = new JsonTable(this);
    table_win_ = table_;
    table_->on_note = [this](const std::string& n, Tone t) { bar_->set_note(n, t); };
    table_->Hide();
    GetSizer()->Add(table_, 1, wxEXPAND);
  }
  if (html_) html_->Hide();
  if (web_) web_->Hide();
  stc_->Hide();
  table_->Show();
  Layout();
  if (table_gen_ == *gen_ && table_fseq_ == filter_seq_) return;
  table_gen_ = *gen_;
  table_fseq_ = filter_seq_;
  table_->set(std::make_shared<TableModel>());
  bar_->set_note("building the table\xE2\x80\xA6", Tone::Accent);
  auto r = resp_;
  const std::string filter = filter_;
  const uint64_t gen = *gen_, fseq = filter_seq_;
  worker_->post([r, filter, gen, fseq, alive = alive_, this] {
    const auto t0 = Clock::now();
    auto m = std::make_shared<TableModel>();
    if (filter.empty()) {
      *m = json_table(r->body);
    } else {
      FilterResult f = json_filter(r->body, filter);
      if (!f.ok) m->error = "filter: " + f.error;
      else if (f.matches == 0) m->error = "the filter matches nothing";
      else {
        *m = json_table(f.json, "$");
        m->source = filter;
      }
    }
    const double ms = ms_between(t0, Clock::now());
    post_ui(alive, [this, m, gen, fseq, ms] {
      if (gen != *gen_ || fseq != filter_seq_ || !table_) return;
      table_->set(m);
      if (filter_chips_ && fseq == filter_seq_ && !filter_.empty())
        filter_chips_->set_note(m->error.empty() ? std::to_string(m->cells.size()) + " rows in the table" : m->error,
                                m->error.empty() ? Tone::Dim : Tone::Warn);
      if (!m->error.empty()) return bar_->set_note("Table: " + m->error, Tone::Warn);
      std::string n = m->source + " \xC2\xB7 " + std::to_string(m->cells.size()) +
                      (m->cells.size() == 1 ? " row" : " rows") + " \xC3\x97 " + std::to_string(m->cols.size()) +
                      (m->cols.size() == 1 ? " column" : " columns") + (m->truncated ? " (capped)" : "") + " \xC2\xB7 " +
                      fmt::ms(ms) + " \xC2\xB7 click a header to sort, Ctrl+C copies rows";
      bar_->set_note(n, Tone::Dim);
    });
  });
}

void BodyView::note(const std::string& text, Tone tone) { bar_->set_note(text, tone); }

void BodyView::show_filter() {
  show_filter_row();
  if (filter_text_->IsEmpty()) filter_text_->ChangeValue("$.");
  filter_text_->SetFocus();
  // The "$." prefix is selected: typing "$[0]" or ".items" replaces it ("$.$[0]" otherwise).
  if (filter_text_->GetValue() == "$.") filter_text_->SelectAll();
  else filter_text_->SetInsertionPointEnd();
}

void BodyView::show_filter_row() {
  if (!filterbar_) {
    const Theme& th = Theme::get();
    filterbar_ = new wxPanel(this, wxID_ANY);
    filterbar_->SetBackgroundColour(th.bg);
    filter_text_ = new wxTextCtrl(filterbar_, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(300, -1)),
                                  wxTE_PROCESS_ENTER);
    filter_text_->SetHint("$.items[*].name   (JSONPath)");
    filter_chips_ = new ChipBar(filterbar_, [this](int id) { on_chip(id); });
    filter_chips_->add(kFilterHelp, "?", "JSONPath: $ is the body; .key or ['key'] a member; [0], [-1], [0,2] elements; "
                                         "[1:3] a slice; [*] or .* everything; ..key at any depth; .length a size; "
                                         "[?(@.price < 10 && @.name =~ /^a/i)] the elements a condition holds for "
                                         "(@ the element, $ the body; == != < <= > >= && || !).", -1,
                       true);
    filter_chips_->add(kFilterClose, "Close", "Remove the filter and show the whole body (Esc)", -1, true);
    auto* fs = new wxBoxSizer(wxHORIZONTAL);
    fs->Add(filter_text_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    fs->Add(filter_chips_, 1, wxEXPAND);
    filterbar_->SetSizer(fs);
    filterbar_->Hide();
    GetSizer()->Insert(1, filterbar_, 0, wxEXPAND | wxBOTTOM, FromDIP(2));
    filter_text_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { filter_timer_.StartOnce(250); });
    filter_text_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) {
      filter_timer_.Stop();
      apply_filter();
    });
  }
  if (!filterbar_->IsShown()) {
    filterbar_->Show();
    Layout();
  }
}

void BodyView::set_filter(const std::string& expr) {
  if (expr == filter_) return;
  filter_timer_.Stop();
  if (expr.empty()) {
    if (filterbar_ && filterbar_->IsShown()) {
      filterbar_->Hide();
      Layout();
    }
    if (filter_text_) filter_text_->ChangeValue("");
  } else {
    show_filter_row();
    filter_text_->ChangeValue(wxString::FromUTF8(expr));
  }
  filter_ = expr;
  ++filter_seq_;
  Doc& d = docs_[Filter];
  if (d.doc) stc_->ReleaseDocument(d.doc);
  d = Doc{};
  if (filter_chips_) filter_chips_->set_note("");
  set_mode_chips();
  // The response on screen is the old request's (stale) until the next one arrives: it is not
  // re-rendered for the new filter; the next set_http() builds with it.
}

void BodyView::close_filter() {
  filter_timer_.Stop();
  if (filterbar_) filterbar_->Hide();
  if (filter_text_) filter_text_->ChangeValue("");
  Layout();
  if (!filter_.empty()) {
    filter_.clear();
    ++filter_seq_;
    if (resp_) request_mode(mode_);
  }
  set_mode_chips();
  stc_->SetFocus();
}

void BodyView::apply_filter() {
  if (!filter_text_) return;
  std::string e = filter_text_->GetValue().utf8_string();
  while (!e.empty() && e.back() == ' ') e.pop_back();
  // "$" or "$." alone: no filter yet.
  const bool none = e.empty() || e == "$" || e == "$." || e == "res.body";
  if (!none) {
    if (const std::string err = json_filter_check(e); !err.empty()) {
      filter_chips_->set_note(err, Tone::Err);
      return;
    }
  }
  const std::string want = none ? std::string() : e;
  if (want == filter_) return;
  filter_ = want;
  ++filter_seq_;
  Doc& d = docs_[Filter];
  if (d.doc) {
    stc_->ReleaseDocument(d.doc);
  }
  d = Doc{};
  set_mode_chips();
  if (!resp_ || kind_ != fmt::BodyKind::Json) {
    filter_chips_->set_note(resp_ ? "the body is not JSON: nothing to filter" : "no body", Tone::Warn);
    return;
  }
  filter_chips_->set_note(filter_.empty() ? "" : "filtering\xE2\x80\xA6", Tone::Accent);
  outline_ = false;
  if (mode_ == Table) return request_mode(Table);  // the table follows the filter
  if (mode_ != Pretty) mode_ = Pretty;
  request_mode(Pretty);
}

void BodyView::show_find() {
  if (mode_ == Table) {
    bar_->set_note("Find searches Pretty / Raw: use Filter to narrow the table", Tone::Dim);
    return;
  }
  if (!findbar_->IsShown()) {
    findbar_->Show();
    Layout();
  }
  const int a = stc_->GetSelectionStart(), b = stc_->GetSelectionEnd();
  if (b > a && b - a < 200 && stc_->LineFromPosition(a) == stc_->LineFromPosition(b))
    find_text_->ChangeValue(stc_->GetTextRange(a, b));
  find_text_->SetFocus();
  find_text_->SelectAll();
  schedule_count();
  highlight_visible();
}

void BodyView::reset_find() {
  ++find_seq_;  // a search still running on the worker reports nothing
  find_chips_->set_note("");
  if (!findbar_->IsShown()) return;
  findbar_->Hide();
  Layout();
  stc_->SetIndicatorCurrent(8);
  stc_->IndicatorClearRange(0, stc_->GetLength());
  hl_range_ = {0, 0};
}

void BodyView::close_find() {
  findbar_->Hide();
  Layout();
  stc_->SetIndicatorCurrent(8);
  stc_->IndicatorClearRange(0, stc_->GetLength());
  hl_range_ = {0, 0};
  stc_->SetFocus();
}

void BodyView::find_from(bool forward, bool from_sel_start) {
  const wxString q = find_text_->GetValue();
  if (q.empty()) {
    find_chips_->set_note("");
    return;
  }
  const int start = forward ? (from_sel_start ? stc_->GetSelectionStart() : stc_->GetSelectionEnd())
                            : stc_->GetSelectionStart();
  if (stc_->GetLength() > static_cast<int>(kSyncBytes)) {
    // Big document: scan on the worker (Scintilla's search walks the document on the UI thread,
    // ~200 ms for a far match in 18 MB) and select when the answer comes back.
    const auto [owner, text] = search_text();
    const std::string qs = q.utf8_string();
    const bool mc = match_case_;
    const uint64_t seq = ++find_seq_;
    const uint64_t my = *gen_;
    worker_->post([owner = owner, text = text, qs, mc, start, forward, seq, my, gen = gen_, alive = alive_, this] {
      if (*gen != my) return;
      const size_t at = find_next(text, qs, mc, static_cast<size_t>(start), forward);
      post_ui(alive, [this, at, n = qs.size(), seq, my] {
        if (seq != find_seq_ || *gen_ != my) return;  // superseded by a newer search
        show_match(at == std::string_view::npos ? -1 : static_cast<int>(at), static_cast<int>(n));
      });
    });
    return;
  }
  stc_->SetSearchFlags(match_case_ ? wxSTC_FIND_MATCHCASE : 0);
  const int len = stc_->GetLength();
  int found = -1;
  if (forward) {
    stc_->SetTargetRange(start, len);
    found = stc_->SearchInTarget(q);
    if (found < 0 && start > 0) {
      stc_->SetTargetRange(0, start);
      found = stc_->SearchInTarget(q);
    }
  } else {
    stc_->SetTargetRange(start, 0);
    found = stc_->SearchInTarget(q);
    if (found < 0 && start < len) {
      stc_->SetTargetRange(len, start);
      found = stc_->SearchInTarget(q);
    }
  }
  if (found >= 0) show_match(stc_->GetTargetStart(), stc_->GetTargetEnd() - stc_->GetTargetStart());
  else show_match(-1, 0);
}

void BodyView::show_match(int at, int len) {
  if (at >= 0) {
    const int doc_line = stc_->LineFromPosition(at);
    stc_->EnsureVisible(doc_line);
    stc_->SetSelection(at, at + len);
    // Keep the match away from the edges: a third of the way down the view.
    const int vis = stc_->VisibleFromDocLine(doc_line);
    const int first = stc_->GetFirstVisibleLine(), rows = stc_->LinesOnScreen();
    if (vis < first + 2 || vis > first + rows - 3) stc_->SetFirstVisibleLine(std::max(0, vis - rows / 3));
    stc_->ScrollRange(at, at + len);
    find_text_->SetForegroundColour(wxNullColour);
    schedule_count();
  } else {
    find_text_->SetForegroundColour(Theme::get().err);
    find_chips_->set_note("no matches", Tone::Err);
  }
  find_text_->Refresh();
  highlight_visible();
}

std::pair<std::shared_ptr<const void>, std::string_view> BodyView::search_text() {
  // The document's own source when we have it, else a snapshot (gRPC stream).
  std::shared_ptr<const void> owner = docs_[shown_mode()].owner;
  std::string_view text = docs_[shown_mode()].text;
  if (!owner || static_cast<int>(text.size()) != stc_->GetLength()) {
    const char* p = stc_->GetCharacterPointer();
    auto snap = std::make_shared<std::string>(p ? p : "", static_cast<size_t>(stc_->GetLength()));
    text = *snap;
    owner = snap;
  }
  return {owner, text};
}

void BodyView::schedule_count() {
  const std::string q = find_text_->GetValue().utf8_string();
  if (q.empty()) {
    find_chips_->set_note("");
    return;
  }
  const auto [owner, text] = search_text();
  const size_t pos = static_cast<size_t>(stc_->GetSelectionStart());
  const bool mc = match_case_;
  const uint64_t my = *gen_;
  auto gen = gen_;
  const auto run = [owner, text, q, pos, mc]() {
    size_t before = 0;
    const size_t n = count_matches(text, q, mc, pos, &before);
    return std::pair<size_t, size_t>(n, before);
  };
  auto show = [this, q](std::pair<size_t, size_t> r) {
    if (find_text_->GetValue().utf8_string() != q) return;
    if (r.first == 0) find_chips_->set_note("no matches", Tone::Err);
    else
      find_chips_->set_note(std::to_string(std::min(r.second + 1, r.first)) + " of " + std::to_string(r.first),
                            Tone::Plain);
  };
  if (text.size() <= kSyncBytes) return show(run());
  worker_->post([run, show, alive = alive_, gen, my] {
    if (*gen != my) return;
    const auto r = run();
    post_ui(alive, [show, r] { show(r); });
  });
}

void BodyView::highlight_visible() {
  stc_->SetIndicatorCurrent(8);
  if (hl_range_.second > hl_range_.first) {
    const int len = stc_->GetLength();
    const int a = std::min(hl_range_.first, len);
    stc_->IndicatorClearRange(a, std::min(hl_range_.second, len) - a);
  }
  hl_range_ = {0, 0};
  const wxString q = findbar_->IsShown() ? find_text_->GetValue() : wxString();
  if (q.empty()) return;
  const wxSize cs = stc_->GetClientSize();
  int a = stc_->PositionFromPoint(wxPoint(0, 0));
  int b = stc_->PositionFromPoint(wxPoint(cs.x, cs.y));
  a = stc_->PositionFromLine(stc_->LineFromPosition(a)) > a - 4096 ? stc_->PositionFromLine(stc_->LineFromPosition(a)) : a - 4096;
  b = std::min(stc_->GetLength(), std::max(b, stc_->GetLineEndPosition(stc_->LineFromPosition(b))));
  if (b - a > (1 << 20)) b = a + (1 << 20);
  a = std::max(a, 0);
  stc_->SetSearchFlags(match_case_ ? wxSTC_FIND_MATCHCASE : 0);
  int from = a;
  for (int i = 0; i < 2000 && from < b; ++i) {
    stc_->SetTargetRange(from, b);
    if (stc_->SearchInTarget(q) < 0) break;
    const int s = stc_->GetTargetStart(), e = stc_->GetTargetEnd();
    stc_->IndicatorFillRange(s, e - s);
    from = std::max(e, s + 1);
  }
  hl_range_ = {a, b};
}

}  // namespace watt::app::rv
