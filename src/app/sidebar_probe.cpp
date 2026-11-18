#include "app/sidebar_probe.h"

#include <windows.h>

namespace watt::app::sidebar {

StallProbe::StallProbe(void* hwnd) : hwnd_(hwnd) {
  th_ = std::thread([this] {
    for (;;) {
      const auto t0 = Clock::now();
      DWORD_PTR r = 0;
      SendMessageTimeoutW(static_cast<HWND>(hwnd_), WM_NULL, 0, 0, SMTO_NORMAL, 5000, &r);
      const long long us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
      long long cur = max_us_.load();
      while (us > cur && !max_us_.compare_exchange_weak(cur, us)) {
      }
      if (us > 20000) {
        std::lock_guard<std::mutex> lock(mu_);
        long_.emplace_back(t0, us / 1000.0);
      }
      // Not Sleep(2): that waits a whole 15.6 ms timer tick and hides the start of a stall.
      const auto next = Clock::now() + std::chrono::milliseconds(2);
      while (Clock::now() < next) std::this_thread::yield();
    }
  });
  th_.detach();
}

void StallProbe::reset() {
  max_us_.store(0);
  std::lock_guard<std::mutex> lock(mu_);
  long_.clear();
}

std::vector<std::pair<StallProbe::Clock::time_point, double>> StallProbe::long_stalls() {
  std::lock_guard<std::mutex> lock(mu_);
  return long_;
}

}  // namespace watt::app::sidebar

#include <dbghelp.h>

#include <cstdio>
#include <string>

namespace watt::app::sidebar {
namespace {

std::wstring g_crash_path;

LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, g_crash_path.c_str(), L"ab") != 0 || !f) return EXCEPTION_CONTINUE_SEARCH;
  std::fprintf(f, "CRASH code %08lx at %p\n", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
  HMODULE dh = ::LoadLibraryW(L"dbghelp.dll");
  using SymInit = BOOL(WINAPI*)(HANDLE, PCSTR, BOOL);
  using SymOpts = DWORD(WINAPI*)(DWORD);
  using Walk = BOOL(WINAPI*)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID, PREAD_PROCESS_MEMORY_ROUTINE64,
                             PFUNCTION_TABLE_ACCESS_ROUTINE64, PGET_MODULE_BASE_ROUTINE64, PTRANSLATE_ADDRESS_ROUTINE64);
  using FromAddr = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
  using LineAddr = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);
  auto sym_init = dh ? reinterpret_cast<SymInit>(::GetProcAddress(dh, "SymInitialize")) : nullptr;
  auto sym_opts = dh ? reinterpret_cast<SymOpts>(::GetProcAddress(dh, "SymSetOptions")) : nullptr;
  auto walk = dh ? reinterpret_cast<Walk>(::GetProcAddress(dh, "StackWalk64")) : nullptr;
  auto from_addr = dh ? reinterpret_cast<FromAddr>(::GetProcAddress(dh, "SymFromAddr")) : nullptr;
  auto line_addr = dh ? reinterpret_cast<LineAddr>(::GetProcAddress(dh, "SymGetLineFromAddr64")) : nullptr;
  auto table = dh ? reinterpret_cast<PFUNCTION_TABLE_ACCESS_ROUTINE64>(::GetProcAddress(dh, "SymFunctionTableAccess64")) : nullptr;
  auto base = dh ? reinterpret_cast<PGET_MODULE_BASE_ROUTINE64>(::GetProcAddress(dh, "SymGetModuleBase64")) : nullptr;
  if (sym_init && walk && from_addr && table && base) {
    HANDLE proc = ::GetCurrentProcess();
    if (sym_opts) sym_opts(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    sym_init(proc, nullptr, TRUE);
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 sf{};
    sf.AddrPC.Offset = ctx.Rip;
    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp;
    sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp;
    sf.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 64; ++i) {
      if (!walk(IMAGE_FILE_MACHINE_AMD64, proc, ::GetCurrentThread(), &sf, &ctx, nullptr, table, base, nullptr)) break;
      if (!sf.AddrPC.Offset) break;
      alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512] = {};
      auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
      si->SizeOfStruct = sizeof(SYMBOL_INFO);
      si->MaxNameLen = 511;
      DWORD64 disp = 0;
      const bool named = from_addr(proc, sf.AddrPC.Offset, &disp, si) != FALSE;
      IMAGEHLP_LINE64 ln{};
      ln.SizeOfStruct = sizeof(ln);
      DWORD ldisp = 0;
      const bool lined = line_addr && line_addr(proc, sf.AddrPC.Offset, &ldisp, &ln) != FALSE;
      std::fprintf(f, "  #%02d %p %s+0x%llx %s:%lu\n", i, reinterpret_cast<void*>(sf.AddrPC.Offset),
                   named ? si->Name : "?", static_cast<unsigned long long>(disp), lined ? ln.FileName : "",
                   lined ? ln.LineNumber : 0UL);
    }
  }
  std::fclose(f);
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void install_crash_log() {
  wchar_t p[MAX_PATH] = {};
  const DWORD n = ::GetEnvironmentVariableW(L"WATT_CRASHLOG", p, MAX_PATH);
  if (n == 0 || n >= MAX_PATH || !g_crash_path.empty()) return;
  g_crash_path = p;
  ::SetUnhandledExceptionFilter(crash_filter);
}

}  // namespace watt::app::sidebar
