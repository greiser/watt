#pragma once
// Shared look of the sidebar pieces: method / kind badge colours and labels, the badge images
// of the collection tree, small fonts, status colours. UI thread only.
#include <wx/bmpbndl.h>
#include <wx/colour.h>
#include <wx/font.h>

#include <string>
#include <string_view>
#include <vector>

#include "core/model.h"

class wxWindow;
class wxDC;

namespace watt::app::sidebar {

bool is_dark();

// "GET" -> "GET", "DELETE" -> "DEL", "OPTIONS" -> "OPT"; gRPC -> "gRPC".
std::string badge_label(RequestKind kind, std::string_view method);
wxColour badge_colour(RequestKind kind, std::string_view method);

// Colour for a history status: HTTP 2xx green, 3xx blue, 4xx orange, 5xx / failure red;
// gRPC 0 green, else red.
wxColour status_colour(RequestKind kind, long status);
// "200", "failed", "OK", "UNAVAILABLE"...
std::string status_text(RequestKind kind, long status);
// "0.31 ms", "12.4 ms", "1.24 s"
std::string format_ms(double ms);

// Text colour helpers (follow the system light / dark appearance).
wxColour text_colour();
wxColour dim_colour();
wxColour window_colour();
wxColour highlight_colour();       // selection background
wxColour highlight_text_colour();
// a mix of a and b, t in [0,1] (0 = a)
wxColour mix(const wxColour& a, const wxColour& b, double t);

// Bold small font for badges, at the window's DPI.
wxFont badge_font(const wxWindow* w);

// Images for the collection tree, in this order (all the same size).
enum TreeImage {
  kImgFolder,
  kImgFolderOpen,
  kImgGet,
  kImgPost,
  kImgPut,
  kImgPatch,
  kImgDelete,
  kImgHead,
  kImgOptions,
  kImgOther,
  kImgGrpc,
  kImgUnknown,  // badge not read yet
  kImgCount
};
int tree_image(RequestKind kind, std::string_view method);
// Bundles for wxTreeCtrl::SetImages(), rendered for w's current DPI scale.
std::vector<wxBitmapBundle> tree_images(const wxWindow* w);

// Draws the badge text at (x, y) (top-left of a box of height h), left aligned. Returns width used.
int draw_badge(wxDC& dc, const wxWindow* w, RequestKind kind, std::string_view method, int x, int y, int h);

}  // namespace watt::app::sidebar
