// watt entry point. Per-monitor DPI v2 comes from the manifest in watt.rc; dark / light
// follows the Windows setting (wxApp::SetAppearance(System)).
//
// Usage: watt.exe [options] [workspace-dir]   (default: Documents\watt)
//   --trace-startup=<file>   append start-up milestones (ms since process creation) to <file>
//                            (also WATT_STARTUP_TRACE=<file>)
//   --first-request=<url>    send GET <url> (or grpc://host:port/pkg.Svc/Method with body {})
//                            from the first tab as soon as the window is up and report "time to
//                            first request": first_response_shown = launch -> response painted
//   --bench-exit             with --first-request: quit once the first response is shown
//   --new-instance           open a second window even if one already shows this workspace
//   --self-test=<file>       start-up self-test: types into the URL field 500 ms after launch
//                            and 150 / 500 ms after a new tab, checks nothing was lost, appends
//                            one JSON line to <file> and quits (see MainFrame::self_test)
//
// Single instance per workspace: a second launch on a workspace that is already open brings
// that window to the front and exits (a named mutex per workspace path; the window carries the
// same name as a window property). Tool runs (--trace-startup, --first-request, --self-test)
// and --new-instance always start their own.
//
// Start-up order (everything not needed for the first paint happens after it):
//   OnInit -> MainFrame (sidebar, one HTTP tab) -> Show -> first paint -> first_idle():
//   open the workspace (background thread), warm libcurl / Winsock / TLS (background thread),
//   pre-create the first tab's curl handle on its worker. The gRPC engine (and its DLLs, which
//   are delay-loaded) starts on the first gRPC use only.
#include <wx/app.h>
#include <wx/config.h>
#include <wx/stdpaths.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "app/main_frame.h"

#include <windows.h>

// Common controls v6 (themed controls) in the linker-generated manifest (see watt.rc).
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
// Per-monitor DPI awareness v2, set during static initialisation: before wx (or anything else)
// creates a window, exactly as the PerMonitorV2 manifest entry would.
const bool g_dpi_aware = [] {
  return SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
}();

struct Trace {
  std::mutex m;
  bool on = false;
  bool bench_exit = false;
  bool response_seen = false;
  std::wstring file;
  std::string first_url;
  std::wstring self_test;
  std::vector<std::pair<std::string, double>> marks;
};
Trace& trace() {
  static Trace t;
  return t;
}

double filetime_ms(const FILETIME& f) {
  ULARGE_INTEGER u;
  u.LowPart = f.dwLowDateTime;
  u.HighPart = f.dwHighDateTime;
  return static_cast<double>(u.QuadPart) / 10000.0;
}

void write_trace() {
  Trace& t = trace();
  std::string out;
  {
    std::lock_guard lk(t.m);
    if (t.file.empty()) return;
    char buf[160];
    out = "{\"watt_startup\":{";
    for (size_t i = 0; i < t.marks.size(); ++i) {
      std::snprintf(buf, sizeof buf, "%s\"%s\":%.1f", i ? "," : "", t.marks[i].first.c_str(), t.marks[i].second);
      out += buf;
    }
    out += "}}\n";
  }
  if (FILE* f = _wfopen(trace().file.c_str(), L"ab")) {
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
  }
}
// "watt.ws.<hash of the lower-case absolute workspace path>".
std::wstring instance_key(const std::filesystem::path& ws) {
  std::error_code ec;
  std::wstring p = std::filesystem::absolute(ws, ec).lexically_normal().wstring();
  while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
  CharLowerBuffW(p.data(), static_cast<DWORD>(p.size()));
  uint64_t h = 1469598103934665603ull;  // FNV-1a
  for (wchar_t c : p) {
    h ^= static_cast<uint64_t>(c);
    h *= 1099511628211ull;
  }
  wchar_t buf[40];
  swprintf(buf, std::size(buf), L"watt.ws.%016llx", static_cast<unsigned long long>(h));
  return buf;
}

// True if another watt already has this workspace open: its window is brought to the front.
bool activate_existing(const std::wstring& key) {
  // Held for the life of the process (never closed): the OS releases it on exit.
  CreateMutexW(nullptr, FALSE, (L"Local\\" + key).c_str());
  if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
  struct Find {
    const std::wstring* key;
    HWND found = nullptr;
  } f{&key};
  // The other instance may still be starting: its window gets the property right after it is
  // created. Give up after ~3 s (hung or exiting) and open a window of our own.
  for (int i = 0; i < 60 && !f.found; ++i) {
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
      auto* fp = reinterpret_cast<Find*>(l);
      if (!GetPropW(h, fp->key->c_str())) return TRUE;
      fp->found = h;
      return FALSE;
    }, reinterpret_cast<LPARAM>(&f));
    if (!f.found) Sleep(50);
  }
  if (!f.found) return false;
  if (IsIconic(f.found)) ShowWindow(f.found, SW_RESTORE);
  SetForegroundWindow(f.found);
  return true;
}
}  // namespace


namespace watt::app {

namespace startup {

double since_launch_ms() {
  FILETIME created, exited, kernel, user, now;
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
  GetSystemTimePreciseAsFileTime(&now);
  return filetime_ms(now) - filetime_ms(created);
}

void mark(const char* what) {
  Trace& t = trace();
  if (!t.on) return;
  const double ms = since_launch_ms();
  std::lock_guard lk(t.m);
  t.marks.emplace_back(what, ms);
}

void response_shown() {
  Trace& t = trace();
  if (!t.on) return;
  {
    std::lock_guard lk(t.m);
    if (t.response_seen) return;
    t.response_seen = true;
  }
  mark("first_response_shown");
  write_trace();
  if (t.bench_exit && wxTheApp)
    wxTheApp->CallAfter([] {
      if (wxWindow* w = wxTheApp->GetTopWindow()) w->Destroy();
    });
}

bool awaiting_response() { return trace().on && !trace().response_seen; }

const std::string& first_request_url() { return trace().first_url; }

const std::wstring& self_test_file() { return trace().self_test; }

void flush() {
  // With --first-request the report is written once, when the response is shown.
  if (trace().on && trace().first_url.empty()) write_trace();
}

}  // namespace startup

class App : public wxApp {
 public:
  bool OnInit() override {
    SetAppearance(Appearance::System);  // must run before the first window is created
    SetAppName("watt");

    Trace& t = trace();
    std::filesystem::path ws;
    bool new_instance = false;
    for (int i = 1; i < argc; ++i) {
      const std::wstring a = argv[i].ToStdWstring();
      auto opt = [&](const wchar_t* name, std::wstring* val) {
        const size_t n = wcslen(name);
        if (a.compare(0, n, name) != 0) return false;
        if (val) *val = a.size() > n && a[n] == L'=' ? a.substr(n + 1) : std::wstring();
        return a.size() == n || a[n] == L'=';
      };
      std::wstring v;
      if (opt(L"--trace-startup", &v)) {
        t.file = v;
      } else if (opt(L"--first-request", &v)) {
        t.first_url = wxString(v).utf8_string();
      } else if (opt(L"--self-test", &v)) {
        t.self_test = v;
      } else if (opt(L"--new-instance", nullptr)) {
        new_instance = true;
      } else if (opt(L"--bench-exit", nullptr)) {
        t.bench_exit = true;
      } else {
        ws = a;
      }
    }
    if (t.file.empty())
      if (const wchar_t* e = _wgetenv(L"WATT_STARTUP_TRACE"); e && *e) t.file = e;
    t.on = !t.file.empty() || !t.first_url.empty();
    startup::mark("oninit");
    if (ws.empty()) {
      // No folder given: the workspace used last (the sidebar records it), else Documents\watt.
      // Opening it directly saves the sidebar's switch from the default one after start-up (a
      // second workspace open plus a tab swap).
      wxString last;
      std::error_code ec;
      if (wxConfigBase* c = wxConfigBase::Get(); c && c->Read("Workspace/Last", &last) && !last.empty() &&
          std::filesystem::exists(std::filesystem::path(last.ToStdWstring()) / "watt.json", ec))
        ws = last.ToStdWstring();
      else
        ws = std::filesystem::path(wxStandardPaths::Get().GetDocumentsDir().ToStdWstring()) / L"watt";
    }

    const std::wstring key = instance_key(ws);
    if (!t.on && t.self_test.empty() && !new_instance && activate_existing(key)) {
      // Done, successfully: the other window is in front. Exit 0 (OnInit returning false would
      // make the process exit with 255, which scripts and launchers read as a failure).
      handed_off_ = true;
      return true;
    }

    auto* frame = new MainFrame(ws);
    SetPropW(static_cast<HWND>(frame->GetHWND()), key.c_str(), reinterpret_cast<HANDLE>(1));
    // Run what the panels queued with CallAfter while being built (the response view's Tests
    // list, the sidebar's width check, the tab's sash) before the window shows, not as a burst
    // right after its first paint.
    ProcessPendingEvents();
    startup::mark("frame_built");
    frame->Show();
    startup::mark("frame_shown");
    return true;
  }

  int OnRun() override { return handed_off_ ? 0 : wxApp::OnRun(); }

  int OnExit() override {
    // A trace without a first response (plain --trace-startup run): written on exit.
    Trace& t = trace();
    if (t.on && !t.response_seen && t.first_url.empty()) {
      startup::mark("exit");
      write_trace();
    }
    return wxApp::OnExit();
  }

 private:
  bool handed_off_ = false;  // another instance shows this workspace: run no event loop
};

}  // namespace watt::app

wxIMPLEMENT_APP(watt::app::App);
