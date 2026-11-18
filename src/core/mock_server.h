#pragma once
// Mock server (Bruno's Mock Servers, Insomnia's Mocks): a local HTTP/1.1 keep-alive listener
// that answers with the saved examples of a collection's or folder's requests. No wx.
//
// Routes: one per saved HTTP request, its method plus the path of its URL after {{vars}} are
// resolved ("/users/:id"). In a route path a ":name" segment, and a segment holding a {{var}}
// that did not resolve, match any one segment. A request matches the route with the most literal
// segments (ties: the first route). Each route serves one of its examples (status, headers, body,
// optional delay): the one named by the query parameter ?__example=<name> or the header
// X-Mock-Example: <name> (case-insensitive), else the first 2xx one, else the first. ?__delay=<ms> / X-Mock-Delay: <ms>
// replace the example's delay (at most 60 s). Unmatched requests get a 404 JSON body listing the
// routes. Responses carry X-Mock-Route and X-Mock-Example headers. The server never reads files:
// a request path is only compared with the routes, so nothing on disk can be reached through it.
//
// Binding: 127.0.0.1 (or ::1 / localhost) only, unless the user allowed another address in the
// per-user <Workspace::user_config_dir()>/allowed_mock_bind.json ({version, addresses: [...]};
// never read from a workspace, so a cloned repository cannot open a port to the network).
//
// Threads: start() returns once the socket listens; an accept thread and one thread per
// connection (at most 64) serve requests. on_request is called on those threads: hand the entry
// to the UI with CallAfter. stop() closes everything (in-flight delays end at once) and joins.
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace watt::mock {

struct Example {
  std::string name;
  long status = 200;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  uint32_t delay_ms = 0;
};

struct Route {
  std::string method;  // upper case; "*" (or empty) matches any method
  std::string path;    // template: "/users/:id", "/items/{{id}}"
  std::string source;  // what it comes from, for the log and the 404 listing ("users/Get user")
  std::vector<Example> examples;  // the first is the default
};

// The path of a request URL as a route template: scheme, host, query and fragment removed. A
// leading unresolved {{var}} (the base URL) counts as the host. Always starts with '/'; a
// trailing '/' is dropped (except for "/").
std::string route_path(std::string_view url);
// < 0: no match; else the number of literal segments (higher is a better match). HEAD matches GET.
int match_score(const Route& r, std::string_view method, std::string_view path);
// Index of the best route for method + path (path without query), or -1.
int find_route(const std::vector<Route>& routes, std::string_view method, std::string_view path);
// The example named `name` (case-insensitive; empty: the first 2xx one, else the first), or nullptr.
const Example* pick_example(const Route& r, std::string_view name);

// What the server sends for one request (also usable without sockets, e.g. in tests).
struct Reply {
  int status = 404;
  std::vector<std::pair<std::string, std::string>> headers;  // without Content-Length / Connection
  std::string body;
  uint32_t delay_ms = 0;
  int route = -1;       // index into routes, -1 when unmatched
  std::string example;  // name of the example served
};
// headers: the request's (name, value) pairs; target: "/path?query".
Reply reply_for(const std::vector<Route>& routes, std::string_view method, std::string_view target,
                const std::vector<std::pair<std::string, std::string>>& headers);

// Bind addresses: loopback ones need no consent.
bool is_loopback_address(std::string_view address);
bool bind_allowed(const std::string& address);
bool allow_bind(const std::string& address, std::string* err = nullptr);
bool disallow_bind(const std::string& address, std::string* err = nullptr);

struct LogEntry {
  std::chrono::system_clock::time_point when;
  std::string method;
  std::string target;  // path?query as received
  std::string route;   // "GET /users/:id" ("" when unmatched)
  std::string example;
  int status = 0;
  double served_ms = 0;  // from the request read to the last byte of the response written
};

class MockServer {
 public:
  struct Options {
    std::string address = "127.0.0.1";
    uint16_t port = 0;  // 0: any free port
    std::function<void(const LogEntry&)> on_request;
  };
  MockServer();
  ~MockServer();
  MockServer(const MockServer&) = delete;
  MockServer& operator=(const MockServer&) = delete;

  // false + *err when the address is not allowed, invalid, or the port cannot be bound.
  bool start(Options opts, std::vector<Route> routes, std::string* err = nullptr);
  // Replaces the routes; requests already being answered keep the old ones.
  void set_routes(std::vector<Route> routes);
  std::shared_ptr<const std::vector<Route>> routes() const;
  void stop();
  bool running() const;
  uint16_t port() const;
  std::string base_url() const;  // "http://127.0.0.1:54321" ("" when not running)
  uint64_t requests_served() const;

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace watt::mock
