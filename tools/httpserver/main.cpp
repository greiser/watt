// watt_httpserver [port] [--any]
// Local HTTP/1.1 keep-alive server for watt tests and benchmarks (default port 18080).
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "http_server.h"

namespace {
HANDLE g_quit = nullptr;
BOOL WINAPI on_ctrl(DWORD) {
  if (g_quit) SetEvent(g_quit);
  return TRUE;
}
}  // namespace

int main(int argc, char** argv) {
  unsigned long port = 18080;
  bool any = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--any") == 0) {
      any = true;
    } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
      std::printf("usage: watt_httpserver [port=18080] [--any]\n");
      return 0;
    } else {
      char* end = nullptr;
      port = std::strtoul(argv[i], &end, 10);
      if (!end || *end || port > 65535) {
        std::fprintf(stderr, "invalid port: %s\n", argv[i]);
        return 2;
      }
    }
  }
  g_quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  SetConsoleCtrlHandler(on_ctrl, TRUE);

  watt::httpserver::Server server;
  if (!server.start(static_cast<uint16_t>(port), any)) {
    std::fprintf(stderr, "watt_httpserver: cannot listen on port %lu\n", port);
    return 1;
  }
  std::printf("watt_httpserver listening on http://%s:%u\n", any ? "0.0.0.0" : "127.0.0.1",
              static_cast<unsigned>(server.port()));
  std::fflush(stdout);
  WaitForSingleObject(g_quit, INFINITE);
  server.stop();
  std::printf("served %llu requests on %llu connections\n",
              static_cast<unsigned long long>(server.requests_served()),
              static_cast<unsigned long long>(server.connections_accepted()));
  CloseHandle(g_quit);
  return 0;
}
