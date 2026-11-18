#pragma once
// WebSocket request tab pieces (RequestKind::WebSocket), like Postman's / Insomnia's / Bruno's
// WebSocket requests:
//   editor   ws(s):// address with {{var}} highlighting and the resolved "Sent as" line, Connect /
//            Disconnect, a message composer (JSON / Text, Ctrl+Enter sends; Ctrl+Enter connects
//            while closed), Headers and Auth (Inherit / None / Bearer / Basic, through the same
//            http_to_send() inheritance as an HTTP request), Docs.
//   response the connection's status strip (state, 101, handshake time, message counts), the
//            handshake phases on the waterfall (DNS / TCP / TLS / upgrade), and the message log:
//            one row per message with its direction, +ms since the connection opened, size and a
//            one-line preview; the selected one pretty-printed below. Messages arrive in batches
//            (WsClient, 50 ms), a row costs a few bytes of index and is drawn only when visible,
//            so a 10k-message burst stays a couple of milliseconds of UI work.
// UI thread only.
#include <memory>
#include <vector>

#include "app/panels.h"
#include "core/ws_client.h"

class wxWindow;

namespace watt::app {

class WsEditorPanel : public RequestEditorPanel {
 public:
  // The tab's connection state: connecting (Cancel), open (Disconnect, composer Send enabled) or
  // closed (Connect).
  enum class State { Closed, Connecting, Open };
  virtual void set_state(State s) = 0;
};

class WsResponsePanel : public ResponsePanel {
 public:
  // The handshake finished (ok or not). sent: the upgrade request as built by http_to_send().
  virtual void ws_handshake(std::shared_ptr<const WsHandshake> hs, std::shared_ptr<const HttpRequest> sent,
                            double queue_ms) = 0;
  virtual void ws_messages(std::vector<WsMessage>&& batch) = 0;
  virtual void ws_closed(const WsClose& c) = 0;
  // A line in the log that is not a message (a send that failed, ...).
  virtual void ws_info(const std::string& text, bool error) = 0;
};

WsEditorPanel* make_ws_editor(wxWindow* parent, EditorHost* host);
WsResponsePanel* make_ws_view(wxWindow* parent);

// The handshake as an HttpResponse (status 101, headers, phases; total = sum of the phases), for
// the session's last response and history.
std::shared_ptr<HttpResponse> ws_handshake_response(const WsHandshake& hs, double queue_ms);

}  // namespace watt::app
