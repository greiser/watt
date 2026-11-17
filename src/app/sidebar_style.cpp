#include "app/sidebar_style.h"

#include <wx/dc.h>
#include <wx/graphics.h>
#include <wx/image.h>
#include <wx/settings.h>
#include <wx/window.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace watt::app::sidebar {

namespace {

constexpr int kBadgeW = 36;  // DIP
constexpr int kBadgeH = 16;

wxColour pick(const char* dark, const char* light) { return wxColour(is_dark() ? dark : light); }

std::string upper(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

// Renders one tree image at `scale` (pixels per DIP).
wxBitmap render(int which, double scale) {
  const int w = static_cast<int>(std::lround(kBadgeW * scale));
  const int h = static_cast<int>(std::lround(kBadgeH * scale));
  wxImage img(w, h);
  img.InitAlpha();
  unsigned char* a = img.GetAlpha();
  std::fill(a, a + static_cast<size_t>(w) * h, static_cast<unsigned char>(0));
  {
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(img));
    if (!gc) return wxBitmap(img);
    // The context draws into img when it is destroyed (end of this scope / reset()).
    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
    if (which == kImgFolder || which == kImgFolderOpen) {
      // A quiet outline in the text colour (not a yellow Explorer folder), as in Bruno / VS Code.
      const wxColour c = mix(window_colour(), text_colour(), 0.62);
      const double s = scale;
      const double off = (kBadgeW - 16) * s;  // right-aligned like the badges, next to the name
      const double x0 = off + 1.5 * s, y0 = 3.0 * s, x1 = off + 15.5 * s, y1 = 13.5 * s;
      wxGraphicsPath p = gc->CreatePath();
      p.MoveToPoint(x0, y0 + 1 * s);
      p.AddLineToPoint(x0 + 4.5 * s, y0 + 1 * s);
      p.AddLineToPoint(x0 + 6 * s, y0 + 2.5 * s);
      p.AddLineToPoint(x1, y0 + 2.5 * s);
      p.AddLineToPoint(x1, y1);
      p.AddLineToPoint(x0, y1);
      p.CloseSubpath();
      gc->SetPen(wxPen(c, std::max(1, static_cast<int>(std::lround(1.2 * s)))));
      gc->SetBrush(wxBrush(wxColour(c.Red(), c.Green(), c.Blue(), which == kImgFolderOpen ? 70 : 0)));
      gc->DrawPath(p);
      // The expand chevron is part of the image (the tree has no native buttons): it sits
      // right next to the folder instead of a button column ~40 px to the left.
      {
        const wxColour cc = mix(window_colour(), text_colour(), 0.6);
        gc->SetPen(wxPen(cc, std::max(1, static_cast<int>(std::lround(1.3 * s)))));
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        wxGraphicsPath v = gc->CreatePath();
        const double cx = off - 6.5 * s, cy = 8.0 * s;
        if (which == kImgFolderOpen) {
          v.MoveToPoint(cx - 3.5 * s, cy - 1.8 * s);
          v.AddLineToPoint(cx, cy + 1.8 * s);
          v.AddLineToPoint(cx + 3.5 * s, cy - 1.8 * s);
        } else {
          v.MoveToPoint(cx - 1.8 * s, cy - 3.5 * s);
          v.AddLineToPoint(cx + 1.8 * s, cy);
          v.AddLineToPoint(cx - 1.8 * s, cy + 3.5 * s);
        }
        gc->StrokePath(v);
      }
      gc.reset();  // flushes into img
      return wxBitmap(img);
    }
    static const struct {
      RequestKind kind;
      const char* method;
    } kinds[] = {
        {RequestKind::Http, "GET"},   {RequestKind::Http, "POST"},  {RequestKind::Http, "PUT"},
        {RequestKind::Http, "PATCH"}, {RequestKind::Http, "DELETE"}, {RequestKind::Http, "HEAD"},
        {RequestKind::Http, "OPTIONS"}, {RequestKind::Http, "TRACE"}, {RequestKind::Grpc, ""},
    };
    std::string label;
    wxColour col;
    if (which == kImgUnknown) {
      label = "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2";  // three bullets: badge not read yet
      col = dim_colour();
    } else {
      const auto& k = kinds[which - kImgGet];
      label = badge_label(k.kind, k.method);
      col = badge_colour(k.kind, k.method);
    }
    wxGraphicsFont f = gc->CreateFont(9.6 * scale, "Segoe UI", wxFONTFLAG_BOLD, col);
    gc->SetFont(f);
    double tw = 0, th = 0, desc = 0, lead = 0;
    const wxString s = wxString::FromUTF8(label);
    gc->GetTextExtent(s, &tw, &th, &desc, &lead);
    // Right-aligned on a dark (light) chip: the name follows the badge closely, and the label
    // stays readable on the tree's grey selection fill.
    const double pw = std::min<double>(w - 1.0 * scale, tw + 6.0 * scale);
    const double px = w - pw - 0.5 * scale, ph = h - 2.0 * scale, py = 1.0 * scale;
    if (which != kImgUnknown) {
      gc->SetPen(*wxTRANSPARENT_PEN);
      gc->SetBrush(wxBrush(is_dark() ? wxColour(0, 0, 0, 105) : wxColour(col.Red(), col.Green(), col.Blue(), 30)));
      gc->DrawRoundedRectangle(px, py, pw, ph, 3.0 * scale);
    }
    gc->DrawText(s, px + (pw - tw) / 2.0, (h - th) / 2.0);
  }
  return wxBitmap(img);
}

}  // namespace

bool is_dark() { return wxSystemSettings::GetAppearance().IsDark(); }

std::string badge_label(RequestKind kind, std::string_view method) {
  if (kind == RequestKind::Grpc) return "gRPC";
  std::string m = upper(method);
  if (m.empty()) return "GET";
  if (m == "DELETE") return "DEL";
  if (m == "OPTIONS") return "OPT";
  if (m.size() > 5) m.resize(5);
  return m;
}

wxColour badge_colour(RequestKind kind, std::string_view method) {
  if (kind == RequestKind::Grpc) return pick("#2EC4CF", "#077C86");
  const std::string m = upper(method);
  if (m == "GET" || m.empty()) return pick("#3FCF73", "#127A3A");
  if (m == "POST") return pick("#F0B43C", "#9A6500");
  if (m == "PUT") return pick("#4E9EF5", "#0B5CB8");
  if (m == "PATCH") return pick("#B08CF0", "#6A3BA8");
  if (m == "DELETE") return pick("#F26B6B", "#B42318");
  if (m == "HEAD") return pick("#62C99A", "#2B7A55");
  if (m == "OPTIONS") return pick("#F27EC2", "#A3266E");
  return dim_colour();
}

wxColour status_colour(RequestKind kind, long status) {
  if (kind == RequestKind::Grpc) return status == 0 ? pick("#3FCF73", "#127A3A") : pick("#F26B6B", "#B42318");
  if (status >= 200 && status < 300) return pick("#3FCF73", "#127A3A");
  if (status >= 300 && status < 400) return pick("#4E9EF5", "#0B5CB8");
  if (status >= 400 && status < 500) return pick("#F0A03C", "#A04F00");
  return pick("#F26B6B", "#B42318");
}

std::string status_text(RequestKind kind, long status) {
  if (kind == RequestKind::Grpc) {
    static const char* names[] = {"OK",          "CANCELLED",          "UNKNOWN",         "INVALID_ARGUMENT",
                                  "DEADLINE_EXCEEDED", "NOT_FOUND",    "ALREADY_EXISTS",  "PERMISSION_DENIED",
                                  "RESOURCE_EXHAUSTED", "FAILED_PRECONDITION", "ABORTED", "OUT_OF_RANGE",
                                  "UNIMPLEMENTED", "INTERNAL",          "UNAVAILABLE",     "DATA_LOSS",
                                  "UNAUTHENTICATED"};
    if (status >= 0 && status < static_cast<long>(std::size(names))) return names[status];
    return "code " + std::to_string(status);
  }
  if (status <= 0) return "failed";
  return std::to_string(status);
}

std::string format_ms(double ms) {
  char buf[32];
  if (ms < 0) ms = 0;
  if (ms < 10) std::snprintf(buf, sizeof buf, "%.2f ms", ms);
  else if (ms < 1000) std::snprintf(buf, sizeof buf, "%.0f ms", ms);
  else if (ms < 60000) std::snprintf(buf, sizeof buf, "%.2f s", ms / 1000);
  else std::snprintf(buf, sizeof buf, "%.0f s", ms / 1000);
  return buf;
}

wxColour text_colour() { return wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT); }
wxColour window_colour() { return wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW); }
wxColour dim_colour() { return mix(window_colour(), text_colour(), 0.55); }
wxColour highlight_colour() {
  return is_dark() ? mix(window_colour(), wxColour(0x4E, 0x9E, 0xF5), 0.35)
                   : mix(window_colour(), wxColour(0x0B, 0x5C, 0xB8), 0.16);
}
wxColour highlight_text_colour() { return text_colour(); }

wxColour mix(const wxColour& a, const wxColour& b, double t) {
  auto m = [t](unsigned char x, unsigned char y) {
    return static_cast<unsigned char>(std::lround(x + (static_cast<double>(y) - x) * t));
  };
  return wxColour(m(a.Red(), b.Red()), m(a.Green(), b.Green()), m(a.Blue(), b.Blue()));
}

wxFont badge_font(const wxWindow* w) {
  wxFont f = w->GetFont();
  f.SetFractionalPointSize(f.GetFractionalPointSize() * 0.8);
  f.SetWeight(wxFONTWEIGHT_BOLD);
  return f;
}

int tree_image(RequestKind kind, std::string_view method) {
  if (kind == RequestKind::Grpc) return kImgGrpc;
  const std::string m = upper(method);
  if (m == "GET" || m.empty()) return kImgGet;
  if (m == "POST") return kImgPost;
  if (m == "PUT") return kImgPut;
  if (m == "PATCH") return kImgPatch;
  if (m == "DELETE") return kImgDelete;
  if (m == "HEAD") return kImgHead;
  if (m == "OPTIONS") return kImgOptions;
  return kImgOther;
}

std::vector<wxBitmapBundle> tree_images(const wxWindow* w) {
  const double scale = w->GetDPIScaleFactor();
  std::vector<wxBitmapBundle> out;
  out.reserve(kImgCount);
  for (int i = 0; i < kImgCount; ++i) {
    wxVector<wxBitmap> bmps;
    bmps.push_back(render(i, 1.0));
    if (std::abs(scale - 1.0) > 0.01) bmps.push_back(render(i, scale));
    out.push_back(wxBitmapBundle::FromBitmaps(bmps));
  }
  return out;
}

int draw_badge(wxDC& dc, const wxWindow* w, RequestKind kind, std::string_view method, int x, int y, int h) {
  const wxFont old = dc.GetFont();
  dc.SetFont(badge_font(w));
  dc.SetTextForeground(badge_colour(kind, method));
  const wxString s = wxString::FromUTF8(badge_label(kind, method));
  const wxSize sz = dc.GetTextExtent(s);
  dc.DrawText(s, x, y + (h - sz.y) / 2);
  dc.SetFont(old);
  return sz.x;
}

}  // namespace watt::app::sidebar
