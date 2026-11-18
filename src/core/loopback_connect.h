#pragma once
// Loopback TCP connects without libcurl's Windows connect check (internal to src/core).
//
// libcurl confirms every connect with verifyconnect(): SleepEx(0) plus getsockopt(SO_ERROR).
// Right after a loopback handshake that getsockopt blocks for one scheduler tick (~10 ms)
// on many fresh connections once the thread has been idle (measured: 3/8 to 14/20 at 300 ms
// gaps; the 10 ms then showed up as "Connect" in the waterfall, while the handshake itself
// takes ~0.3 ms inside connect()). For a loopback peer the handshake completes inside
// connect(), so the open-socket callback connects the socket itself, confirms it with WSAPoll
// alone (POLLWRNORM without POLLERR/POLLHUP, never SO_ERROR), and the socket-option callback
// tells libcurl it is already connected (CURL_SOCKOPT_ALREADY_CONNECTED), which skips
// verifyconnect. A refused port (RST: POLLERR/POLLHUP) is reported to libcurl as a socket it
// could not open, so it moves on to the next address (::1 -> 127.0.0.1) without touching
// SO_ERROR either. Anything else (not done within the budget) gets a fresh, unconnected
// socket so libcurl connects and reports the outcome exactly as before.
#include <winsock2.h>
#include <ws2tcpip.h>

namespace watt::detail {

enum class PreConnect { Connected, Refused, Unknown };

// 127.0.0.0/8, ::1 or ::ffff:127.x.y.z.
inline bool loopback_sockaddr(const sockaddr* sa) {
  if (sa->sa_family == AF_INET)
    return (ntohl(reinterpret_cast<const sockaddr_in*>(sa)->sin_addr.s_addr) >> 24) == 127;
  if (sa->sa_family != AF_INET6) return false;
  const unsigned char* b = reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr.s6_addr;
  bool zero10 = true;
  for (int i = 0; i < 10; ++i) zero10 = zero10 && b[i] == 0;
  if (!zero10) return false;
  if (b[10] == 0 && b[11] == 0) {  // ::1
    for (int i = 12; i < 15; ++i)
      if (b[i] != 0) return false;
    return b[15] == 1;
  }
  return b[10] == 0xff && b[11] == 0xff && b[12] == 127;  // ::ffff:127.x.y.z
}

// Connects `s` (a fresh TCP socket) to the loopback address `sa`. Unless Connected, the socket
// is in an unknown state and must be closed.
inline PreConnect loopback_preconnect(SOCKET s, const sockaddr* sa, int len) {
  u_long nb = 1;
  if (ioctlsocket(s, FIONBIO, &nb) != 0) return PreConnect::Unknown;
  if (connect(s, sa, len) == 0) return PreConnect::Connected;
  const int err = WSAGetLastError();
  if (err == WSAECONNREFUSED) return PreConnect::Refused;
  if (err != WSAEWOULDBLOCK) return PreConnect::Unknown;
  // The handshake normally completes inside connect(); a few ms covers a loaded machine.
  WSAPOLLFD p{};
  p.fd = s;
  p.events = POLLWRNORM;
  if (WSAPoll(&p, 1, 5) != 1 || (p.revents & POLLNVAL)) return PreConnect::Unknown;
  if (p.revents & (POLLERR | POLLHUP)) return PreConnect::Refused;
  return (p.revents & POLLWRNORM) ? PreConnect::Connected : PreConnect::Unknown;
}

}  // namespace watt::detail
