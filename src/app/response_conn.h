#pragma once
// Connection details popover of the response view (like Postman's network globe): protocol,
// local and remote address, TLS version, cipher, certificate and chain. Opened from the
// connection pill next to "Timing" and from the protocol segment of the status strip. Click a
// row to copy its value; "Copy all" copies every row as "Key: value" lines.
#include <wx/gdicmn.h>

#include <string>
#include <vector>

class wxWindow;

namespace watt::app::rv {

struct InfoRow {
  std::string key, value;
  bool section = false;  // a group heading ("TLS", "Certificate"); value unused
  bool warn = false;     // drawn in the warning color (e.g. verification off)
};

// Shows the popover under `anchor` (screen coordinates); it closes on any click outside / Esc.
void show_info_popup(wxWindow* owner, const wxRect& anchor, const std::string& title, std::vector<InfoRow> rows);

}  // namespace watt::app::rv
