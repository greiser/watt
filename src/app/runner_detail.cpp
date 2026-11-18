#include "app/runner_detail.h"

#include <wx/clipbrd.h>
#include <wx/dcbuffer.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stc/stc.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>
#include <thread>

#include <wx/app.h>

#include "app/response_body.h"
#include "app/response_format.h"
#include "app/response_widgets.h"
#include "app/sidebar_style.h"

namespace watt::app::runner_ui {

using runner::StepResult;
using StepPtr = std::shared_ptr<const StepResult>;

namespace {
wxString U(const std::string& s) { return wxString::FromUTF8(s); }
}  // namespace

std::string fmt_ms(double v) {
  char b[32];
  if (v < 0) v = 0;
  if (v < 1)
    std::snprintf(b, sizeof b, "%.2f ms", v);
  else if (v < 100)
    std::snprintf(b, sizeof b, "%.1f ms", v);
  else if (v < 1000)
    std::snprintf(b, sizeof b, "%.0f ms", v);
  else if (v < 600000)
    std::snprintf(b, sizeof b, "%.2f s", v / 1000);
  else
    std::snprintf(b, sizeof b, "%dm %02ds", static_cast<int>(v / 60000), static_cast<int>(std::fmod(v / 1000, 60)));
  return b;
}

std::string fmt_bytes(uint64_t n) {
  char b[32];
  if (n < 1024)
    std::snprintf(b, sizeof b, "%llu B", static_cast<unsigned long long>(n));
  else if (n < 1024 * 1024)
    std::snprintf(b, sizeof b, "%.1f KB", static_cast<double>(n) / 1024);
  else
    std::snprintf(b, sizeof b, "%.1f MB", static_cast<double>(n) / (1024.0 * 1024.0));
  return b;
}

wxColour mix(const wxColour& a, const wxColour& b, double t) {
  auto m = [t](int x, int y) { return static_cast<unsigned char>(std::lround(x + (y - x) * t)); };
  return wxColour(m(a.Red(), b.Red()), m(a.Green(), b.Green()), m(a.Blue(), b.Blue()));
}

wxColour status_colour(const StepResult& r) {
  const auto& th = rv::Theme::get();
  if (r.skipped) return th.dim;
  if (r.stopped()) return th.warn;
  if (!r.sent || (r.kind == RequestKind::Http && !r.transport_ok)) return th.err;
  if (r.kind == RequestKind::Grpc) return r.transport_ok ? th.ok : th.err;
  if (r.status >= 500) return th.err;
  if (r.status >= 400) return th.warn;
  if (r.status >= 300) return th.redirect;
  return th.ok;
}

std::string status_label(const StepResult& r) {
  if (r.skipped) return "skipped";
  if (r.stopped()) return "stopped";
  if (!r.sent) return r.client_error ? "error" : "not sent";
  if (r.kind == RequestKind::Http) {
    if (!r.transport_ok) return "error";
    return std::to_string(r.status) + (r.status_text.empty() ? "" : " " + r.status_text);
  }
  return r.status_text.empty() ? std::to_string(r.status) : r.status_text;
}

void hook_keys(wxWindow* w, const std::function<bool(wxKeyEvent&)>& f, std::set<wxWindow*>* seen) {
  if (!w) return;
  if (!seen || seen->insert(w).second)
    w->Bind(wxEVT_KEY_DOWN, [f](wxKeyEvent& e) {
      if (f && f(e)) return;
      e.Skip();
    });
  for (wxWindow* c : w->GetChildren()) hook_keys(c, f, seen);
}

wxColour method_colour(const std::string& method, bool grpc) {
  return sidebar::badge_colour(grpc ? RequestKind::Grpc : RequestKind::Http, method);
}

namespace {

enum Tab { kResponse, kHeaders, kRequest, kTiming, kTests, kCaptures, kConsole, kTabs };
enum Chip { kCopy = 1, kOpen = 2 };

// ------------------------------------------------------------------ header

class HeadView : public wxWindow {
 public:
  explicit HeadView(wxWindow* parent) : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    mono_ = wxFont(wxFontInfo(GetFont().GetPointSize()).Family(wxFONTFAMILY_TELETYPE).FaceName("Consolas"));
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      Refresh();
    });
  }
  void set(StepPtr s, const std::string& hint) {
    s_ = std::move(s);
    hint_ = hint;
    // The reason line's height is kept for every step, so the tabs below never move between
    // a failed row and a passed one while arrowing through the results.
    const int h = FromDIP(!s_ ? 40 : 82);
    if (GetMinSize().y != h) {
      SetMinSize(wxSize(-1, h));
      GetParent()->Layout();
    }
    Refresh();
  }

 private:
  void paint() {
    wxAutoBufferedPaintDC dc(this);
    const auto& th = rv::Theme::get();
    dc.SetBackground(wxBrush(th.bg));
    dc.Clear();
    const wxSize cs = GetClientSize();
    const int pad = FromDIP(16);
    int y = FromDIP(10);
    const wxFont base = GetFont();
    const wxFont bold = base.Bold();
    const wxFont small = base.Scaled(0.9f);
    if (!s_) {
      dc.SetFont(base);
      dc.SetTextForeground(th.dim);
      dc.DrawText(wxControl::Ellipsize(U(hint_), dc, wxELLIPSIZE_END, std::max(cs.x - 2 * pad, 40)), pad, y + FromDIP(2));
      return;
    }
    const StepResult& r = *s_;
    const int right = cs.x - pad;
    auto text = [&](const wxString& t, int x, const wxFont& f, const wxColour& c) {
      dc.SetFont(f);
      dc.SetTextForeground(c);
      dc.DrawText(t, x, y);
      return dc.GetTextExtent(t);
    };
    auto ell = [&](const std::string& s, int width, const wxFont& f) {
      dc.SetFont(f);
      return wxControl::Ellipsize(U(s), dc, wxELLIPSIZE_END, std::max(width, FromDIP(40)));
    };
    const wxSize a = text(wxString::Format("#%zu  ", r.seq + 1), pad, bold, th.dim);
    const std::string m = r.kind == RequestKind::Http ? r.method : "gRPC";
    const wxSize b = text(U(m), pad + a.x, bold, method_colour(m, r.kind == RequestKind::Grpc));
    // Status pill and time on the right.
    const wxString st = U(status_label(r));
    dc.SetFont(bold);
    const wxSize ss = dc.GetTextExtent(st);
    const wxString tm = r.sent ? U(fmt_ms(r.timing.total_ms) + "   " + fmt_bytes(r.size)) : wxString();
    const wxSize ts = dc.GetTextExtent(tm);
    int x = right - ts.x;
    if (!tm.empty()) text(tm, x, bold, th.fg);
    x -= ss.x + FromDIP(16) + (tm.empty() ? 0 : FromDIP(12));
    const wxColour sc = status_colour(r);
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(mix(sc, th.bg, 0.8)));
    dc.DrawRoundedRectangle(x, y - FromDIP(2), ss.x + FromDIP(16), ss.y + FromDIP(4), FromDIP(4));
    text(st, x + FromDIP(8), bold, sc);
    const int name_x = pad + a.x + b.x + FromDIP(8);
    text(ell(r.name.empty() ? r.rel : r.name, x - name_x - FromDIP(12), bold.Scaled(1.1f)), name_x, bold.Scaled(1.1f), th.fg);
    y += std::max(a.y, ss.y) + FromDIP(6);
    std::string sub = "iteration " + std::to_string(r.iteration + 1);
    if (r.data_row) {
      // Which data file row the iteration used, with its first values (all of them: Variables).
      sub += " (data row " + std::to_string(r.data_index + 1);
      size_t n = 0;
      for (const auto& [k, v] : *r.data_row) {
        if (++n > 3) {
          sub += ", ...";
          break;
        }
        sub += (n == 1 ? ": " : ", ") + k + "=" + (v.size() > 20 ? v.substr(0, 17) + "..." : v);
      }
      sub += ")";
    }
    sub += "  \xC2\xB7  " + r.rel;
    if (r.sent) {
      sub += "  \xC2\xB7  " + std::string(r.timing.connection_reused ? "reused connection" : "new connection");
      if (!r.http_version.empty()) sub += "  \xC2\xB7  " + r.http_version;
    }
    const wxSize su = text(ell(sub, (right - pad) / 2, small), pad, small, th.dim);
    text(ell(r.url, right - pad - su.x - FromDIP(16), mono_), pad + su.x + FromDIP(16), mono_, th.fg);
    y += FromDIP(20);
    if (!r.skipped && (!r.error.empty() || r.failed())) {
      std::string why = r.error.empty() ? r.reason() : r.error;
      if (r.failed() && r.asserts_failed > 1) why += "   (+" + std::to_string(r.asserts_failed - 1) + " more: Tests)";
      if (!r.unresolved.empty()) why += "   \xC2\xB7   unresolved {{" + r.unresolved.front() + "}}" + (r.unresolved.size() > 1 ? " ..." : "");
      text(ell(why, right - pad, base), pad, base, r.stopped() ? th.warn : th.err);
    } else if (r.sent && !r.unresolved.empty()) {
      std::string w = "\xE2\x9A\xA0  passed, but sent with unresolved ";
      for (size_t i = 0; i < r.unresolved.size(); ++i) w += (i ? ", {{" : "{{") + r.unresolved[i] + "}}";
      w += " (no variable of that name in the run's environment, globals, data row or captures: sent as written)";
      text(ell(w, right - pad, base), pad, base, th.warn);
    } else if (r.sent) {
      const int n = r.asserts_passed + r.asserts_failed;
      const std::string ok = n ? "\xE2\x9C\x93  " + std::to_string(n) + (n == 1 ? " check passed" : " checks passed")
                               : std::string("\xE2\x9C\x93  passed (no checks)");
      text(ell(ok, right - pad, base), pad, base, th.ok);
    }
  }
  StepPtr s_;
  std::string hint_;
  wxFont mono_;
};

// ------------------------------------------------------------------ text pane (Headers / Request)

// Styles: 0..13 = fmt::json_style (JSON body), 20 header name, 21 section title, 22 request line.
constexpr int kStName = 20, kStTitle = 21, kStLine = 22, kStDim = 23;

class TextPane : public wxStyledTextCtrl {
 public:
  explicit TextPane(wxWindow* parent) : wxStyledTextCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE) {
    const auto& th = rv::Theme::get();
    const bool dark = th.dark;
    const wxColour bg = th.bg;
    const wxColour fg = dark ? wxColour(212, 212, 212) : wxColour(30, 30, 30);
    wxFont mono(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE).FaceName("Consolas"));
    StyleSetFont(wxSTC_STYLE_DEFAULT, mono);
    StyleSetBackground(wxSTC_STYLE_DEFAULT, bg);
    StyleSetForeground(wxSTC_STYLE_DEFAULT, fg);
    StyleClearAll();
    auto set = [&](int s, wxColour d, wxColour l) { StyleSetForeground(s, dark ? d : l); };
    set(fmt::json_style::PropertyName, wxColour(156, 220, 254), wxColour(4, 81, 165));
    set(fmt::json_style::String, wxColour(206, 145, 120), wxColour(163, 21, 21));
    set(fmt::json_style::Number, wxColour(181, 206, 168), wxColour(9, 134, 88));
    set(fmt::json_style::Keyword, wxColour(86, 156, 214), wxColour(0, 0, 255));
    set(fmt::json_style::Operator, wxColour(150, 150, 150), wxColour(110, 110, 110));
    set(fmt::json_style::Escape, wxColour(215, 186, 125), wxColour(238, 0, 0));
    set(fmt::json_style::Error, wxColour(244, 71, 71), wxColour(205, 49, 49));
    set(kStName, wxColour(156, 220, 254), wxColour(4, 81, 165));
    set(kStTitle, th.dim, th.dim);
    StyleSetBold(kStTitle, true);
    set(kStLine, fg, fg);
    StyleSetBold(kStLine, true);
    set(kStDim, th.dim, th.dim);
    SetCaretForeground(fg);
    SetSelBackground(true, dark ? wxColour(38, 79, 120) : wxColour(173, 214, 255));
    SetMarginWidth(0, 0);
    SetMarginWidth(1, FromDIP(12));
    SetMarginType(1, wxSTC_MARGIN_BACK);
    StyleSetBackground(wxSTC_STYLE_LINENUMBER, bg);
    SetMarginWidth(2, 0);
    SetWrapMode(wxSTC_WRAP_WORD);
    SetWrapIndentMode(wxSTC_WRAPINDENT_INDENT);
    SetUndoCollection(false);
    SetLexer(wxSTC_LEX_CONTAINER);
    SetScrollWidth(1);
    SetScrollWidthTracking(true);
    UsePopUp(wxSTC_POPUP_ALL);
    SetReadOnly(true);
    Bind(wxEVT_STC_STYLENEEDED, [](wxStyledTextEvent&) {});  // pre-styled
  }
  void set(const std::string& text, const std::string& styles) {
    SetReadOnly(false);
    ClearAll();
    AddTextRaw(text.data(), static_cast<int>(text.size()));
    StartStyling(0);
    SetStyleBytes(static_cast<int>(styles.size()), const_cast<char*>(styles.data()));
    SetReadOnly(true);
    GotoPos(0);
    SetScrollWidth(1);
  }
};

// Text + one style byte per text byte.
struct Styled {
  std::string text, styles;
  void add(const std::string& s, int style) {
    text += s;
    styles.append(s.size(), static_cast<char>(style));
  }
  void title(const std::string& s) {
    if (!text.empty()) add("\n", 0);
    add(s + "\n", kStTitle);
  }
  void header(const std::string& k, const std::string& v) {
    add(k, kStName);
    add(": ", kStDim);
    add(v + "\n", 0);
  }
  // A body: pretty JSON when it parses (styled), else as is.
  void body(const std::string& b, const std::string& content_type) {
    std::string pretty;
    const auto kind = fmt::detect_kind(content_type, b);
    if (kind == fmt::BodyKind::Json && fmt::pretty_json(b, pretty)) {
      std::string st;
      std::vector<int> levels;
      fmt::style_json(pretty, st, levels);
      text += pretty;
      styles += st;
      if (!pretty.empty() && pretty.back() != '\n') add("\n", 0);
      return;
    }
    if (kind == fmt::BodyKind::Binary) {
      std::string hex;
      bool cut = false;
      fmt::hex_dump(b, hex, 4096, &cut);
      add(hex, 0);
      if (cut) add("...\n", kStDim);
      return;
    }
    add(b, 0);
    if (!b.empty() && b.back() != '\n') add("\n", 0);
  }
};

std::string header_value(const std::vector<Header>& h, const char* name) {
  for (const auto& [k, v] : h) {
    if (k.size() != std::strlen(name)) continue;
    bool eq = true;
    for (size_t i = 0; i < k.size() && eq; ++i) eq = std::tolower(static_cast<unsigned char>(k[i])) == name[i];
    if (eq) return v;
  }
  return {};
}

// ------------------------------------------------------------------ canvas (Timing / Tests / Captures)

class Canvas : public wxScrolledCanvas {
 public:
  explicit Canvas(wxWindow* parent) : wxScrolledCanvas(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxVSCROLL) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetScrollRate(0, FromDIP(12));
    mono_ = wxFont(wxFontInfo(GetFont().GetPointSize()).Family(wxFONTFAMILY_TELETYPE).FaceName("Consolas"));
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
      wxAutoBufferedPaintDC dc(this);
      DoPrepareDC(dc);
      const int h = draw(dc);
      if (h != height_) {
        height_ = h;
        CallAfter([this] { SetVirtualSize(-1, height_); });
      }
    });
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
      e.Skip();
      Refresh();
    });
  }
  void set(StepPtr s, int tab) {
    s_ = std::move(s);
    tab_ = tab;
    Scroll(0, 0);
    Refresh();
  }

 private:
  int draw(wxDC& dc) {
    const auto& th = rv::Theme::get();
    const wxSize cs = GetClientSize();
    const int vy = GetViewStart().y * FromDIP(12);
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(th.bg));
    dc.DrawRectangle(0, vy, cs.x, cs.y);
    const int pad = FromDIP(16);
    int y = FromDIP(12);
    const wxFont base = GetFont();
    const wxFont bold = base.Bold();
    const wxFont small = base.Scaled(0.9f);
    if (!s_) return y;
    const StepResult& r = *s_;
    const int right = cs.x - pad;
    auto text = [&](const wxString& t, int x, const wxFont& f, const wxColour& c) {
      dc.SetFont(f);
      dc.SetTextForeground(c);
      dc.DrawText(t, x, y);
      return dc.GetTextExtent(t);
    };
    auto ell = [&](const std::string& s, int width, const wxFont& f) {
      dc.SetFont(f);
      return wxControl::Ellipsize(U(s), dc, wxELLIPSIZE_END, std::max(width, FromDIP(40)));
    };
    auto empty = [&](const std::string& s) {
      text(U(s), pad, base, th.dim);
      return y + FromDIP(30);
    };
    if (tab_ == kTiming) {
      if (!r.sent) return empty(r.skipped ? "Not run." : "Not sent: no timing.");
      const std::string head = "Total " + fmt_ms(r.timing.total_ms) + "   \xC2\xB7   " + fmt_bytes(r.size) + " body, " +
                               fmt_bytes(r.bytes_received) + " received   \xC2\xB7   " +
                               (r.timing.connection_reused ? "reused connection" : "new connection") +
                               "   \xC2\xB7   started " + fmt_ms(r.started_ms) + " into the run";
      text(U(head), pad, small, th.dim);
      y += FromDIP(24);
      const int label_w = FromDIP(110), val_w = FromDIP(80);
      const int bar_x = pad + label_w, bar_w = std::max(FromDIP(60), right - val_w - bar_x - FromDIP(10));
      const double total = std::max(r.timing.total_ms, 1e-6);
      double at = 0;
      const wxColour cols[] = {th.dim, th.info, th.redirect, th.warn, th.accent, th.ok};
      int ci = 0;
      for (const auto& p : r.phases) {
        if (p.ms < 0.0005) {
          at += std::max(p.ms, 0.0);
          continue;
        }
        text(p.name, pad, small, th.fg);
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(th.grid));
        dc.DrawRectangle(bar_x, y + FromDIP(4), bar_w, FromDIP(10));
        const int x0 = bar_x + static_cast<int>(at / total * bar_w);
        const int w = std::max(FromDIP(2), static_cast<int>(p.ms / total * bar_w));
        dc.SetBrush(wxBrush(cols[ci++ % 6]));
        dc.DrawRectangle(std::min(x0, bar_x + bar_w - w), y + FromDIP(4), w, FromDIP(10));
        const wxString v = U(fmt_ms(p.ms));
        dc.SetFont(small);
        text(v, right - dc.GetTextExtent(v).x, small, th.fg);
        at += p.ms;
        y += FromDIP(20);
      }
      return y + FromDIP(12);
    }
    if (tab_ == kTests) {
      if (r.asserts.empty()) {
        if (!r.sent || r.skipped) return empty(r.error.empty() ? "Not sent." : r.error);
        const bool f = r.failed();
        text(wxString::FromUTF8(f ? "\xE2\x9C\x97" : "\xE2\x9C\x93"), pad, bold, f ? th.err : th.ok);
        text(f ? wxString("No assertions: failed because of its status (") + U(status_label(r)) + ")"
               : wxString("No assertions: passed (status below 400)"),
             pad + FromDIP(20), base, f ? th.err : th.dim);
        return y + FromDIP(30);
      }
      std::string extra = std::to_string(r.asserts_passed) + " passed";
      if (r.asserts_failed) extra += "  \xC2\xB7  " + std::to_string(r.asserts_failed) + " failed";
      text(U(extra), pad, small.Bold(), r.asserts_failed ? th.err : th.ok);
      y += FromDIP(24);
      bool script_title = false;
      for (const auto& a : r.asserts) {
        if (a.script && !script_title) {
          // Script tests (test() / pm.test) after the assert rows, under their own title.
          script_title = true;
          if (&a != &r.asserts.front()) y += FromDIP(6);
          text(r.scripts_ran ? "SCRIPT TESTS" : "SCRIPTS", pad, small.Bold(), th.dim);
          y += FromDIP(20);
        }
        text(wxString::FromUTF8(a.pass ? "\xE2\x9C\x93" : "\xE2\x9C\x97"), pad, bold, a.pass ? th.ok : th.err);
        const int cx = pad + FromDIP(20);
        if (a.script) {
          // A test's name in the proportional font, why it failed under it when long.
          const wxSize nsz = text(ell(a.target, (right - cx) * 3 / 5, base), cx, base, th.fg);
          if (!a.pass) {
            const std::string why = a.why();
            const int wx = cx + nsz.x + FromDIP(14);
            if (right - wx > FromDIP(160)) {
              text(ell(why, right - wx, small), wx, small, th.err);
            } else {
              y += FromDIP(20);
              text(ell(why, right - cx, small), cx, small, th.err);
            }
          }
          y += FromDIP(22);
          continue;
        }
        const std::string check = a.target + "  " + a.op + (a.expected.empty() ? "" : "  " + a.expected) +
                                  (a.from.empty() ? "" : "   \xC2\xB7 from " + a.from);
        const int half = (right - cx) * 2 / 5;
        text(ell(check, half - FromDIP(10), mono_), cx, mono_, th.fg);
        const std::string got = a.pass ? "= " + (a.actual.empty() ? std::string("(undefined)") : a.actual) : a.why();
        text(ell(got, right - cx - half, small), cx + half, small, a.pass ? th.dim : th.err);
        y += FromDIP(22);
      }
      return y + FromDIP(12);
    }
    // Variables: the iteration's data row, unresolved names, then what the request set.
    if (r.data_row) {
      text(U("DATA ROW " + std::to_string(r.data_index + 1) + "  \xC2\xB7  iteration " + std::to_string(r.iteration + 1)),
           pad, small.Bold(), th.dim);
      y += FromDIP(20);
      for (const auto& [k, v] : *r.data_row) {
        const wxSize a = text(U(k), pad, mono_.Bold(), th.info);
        text(ell("= " + v, right - pad - a.x - FromDIP(10), mono_), pad + a.x + FromDIP(10), mono_, th.fg);
        y += FromDIP(21);
      }
      y += FromDIP(8);
    }
    if (!r.unresolved.empty()) {
      text("UNRESOLVED", pad, small.Bold(), th.warn);
      y += FromDIP(20);
      for (const auto& n : r.unresolved) {
        const wxSize a = text(U("{{" + n + "}}"), pad, mono_.Bold(), th.warn);
        text("not defined: sent as written", pad + a.x + FromDIP(10), small, th.dim);
        y += FromDIP(21);
      }
      y += FromDIP(8);
    }
    if (r.captures.empty()) {
      if (r.data_row || !r.unresolved.empty()) {
        text("This request sets no variables (no captures, pre-request variables or script variables).", pad, small, th.dim);
        return y + FromDIP(30);
      }
      return empty("This request sets no variables (no captures, pre-request variables or script variables).");
    }
    if (r.data_row || !r.unresolved.empty()) {
      text("SET BY THIS REQUEST", pad, small.Bold(), th.dim);
      y += FromDIP(20);
    }
    for (const auto& c : r.captures) {
      const wxSize a = text(U(c.var), pad, mono_.Bold(), c.ok ? th.accent : th.err);
      const std::string from = c.script ? ", script" : "";
      const std::string v = !c.ok      ? c.message
                            : c.removed ? std::string("removed   (") + capture_scope_name(c.scope) + from + ")"
                                        : "= " + c.value + "   (" + capture_scope_name(c.scope) + from + ")";
      text(ell(v, right - pad - a.x - FromDIP(10), mono_), pad + a.x + FromDIP(10), mono_, c.ok ? th.fg : th.err);
      y += FromDIP(21);
    }
    return y + FromDIP(12);
  }

  StepPtr s_;
  int tab_ = kTiming;
  wxFont mono_;
  int height_ = 0;
};

}  // namespace

// ------------------------------------------------------------------ the pane

class DetailPane::Impl {
 public:
  explicit Impl(DetailPane* p) : pane(p) {
    const auto& th = rv::Theme::get();
    pane->SetBackgroundColour(th.bg);
    auto* v = new wxBoxSizer(wxVERTICAL);
    header = new HeadView(pane);
    v->Add(header, 0, wxEXPAND);
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    tabs = new rv::TabStrip(pane, [this](int i) { select(i, true); });
    for (const char* n : {"Response", "Headers", "Request", "Timing", "Tests", "Variables", "Console"}) tabs->add(n);
    row->Add(tabs, 1, wxEXPAND);
    chips = new rv::ChipBar(pane, [this](int id) {
      if (id == kCopy) copy();
      if (id == kOpen && pane->on_open) pane->on_open();
    });
    chips->add(kCopy, "Copy", "Copy this tab's text (Ctrl+C copies a selection)", -1, true);
    chips->add(kOpen, "Open request", "Open the saved request in an editor tab to edit it (double click or Enter on a "
                                      "row). This run's response, timing and tests stay here, in the runner.",
               -1, true);
    chips->set_bottom_rule(true);
    chips->SetMinSize(pane->FromDIP(wxSize(190, -1)));
    row->Add(chips, 0, wxEXPAND);
    v->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT, pane->FromDIP(8));
    body_host = new wxPanel(pane);
    body_host->SetBackgroundColour(th.bg);
    body_sizer = new wxBoxSizer(wxVERTICAL);
    body_host->SetSizer(body_sizer);
    // The text view (Scintilla) and the body viewer are created on first use: opening the runner
    // stays cheap.
    canvas = new Canvas(body_host);
    body_sizer->Add(canvas, 1, wxEXPAND);
    v->Add(body_host, 1, wxEXPAND);
    pane->SetSizer(v);
    canvas->Hide();
    tabs->select(kResponse);
    hook();
  }

  TextPane* text_pane() {
    if (!text) {
      text = new TextPane(body_host);
      body_sizer->Add(text, 1, wxEXPAND | wxLEFT, pane->FromDIP(4));
      text->Hide();
      hook();
    }
    return text;
  }

  rv::BodyView* body_view() {
    if (!body) {
      body = new rv::BodyView(body_host);
      body_sizer->Add(body, 1, wxEXPAND);
      body->Hide();
      hook();
    }
    return body;
  }
  // The whole body of a step whose kept body was cut: read on a worker, shown when it arrives
  // if the pane still shows that step (a later show() or select() bumps gen).
  void load_full(const StepPtr& s) {
    const int at = gen;
    std::weak_ptr<int> alive_w = alive;
    std::thread([s, at, alive_w, this] {
      auto resp = std::make_shared<HttpResponse>();
      std::string body;
      if (!s->body_file->read(&body)) return;  // deleted (budget) or unreadable: the preview stays
      resp->ok = true;
      resp->status = s->status;
      resp->status_text = s->status_text;
      if (const std::string ct = s->content_type(); !ct.empty()) resp->headers.add("Content-Type", ct);
      resp->body = std::move(body);
      resp->sizes.response_body = resp->body.size();
      wxTheApp->CallAfter([resp, at, alive_w, this, s] {
        if (alive_w.expired() || at != gen || pane->s_ != s || !this->body) return;
        this->body->set_http(resp);
      });
    }).detach();
  }

  void hook() {
    hook_keys(pane, [this](wxKeyEvent& e) { return pane->on_key && pane->on_key(e); }, &hooked);
  }

  void show(const StepPtr& s, const std::string& hint) {
    header->set(s, hint);
    gen++;
    if (!s) {
      if (text) text->Hide();
      canvas->Hide();
      if (body) body->Hide();
      tabs->Hide();
      chips->Hide();
      pane->Layout();
      return;
    }
    // From the hint / Running state the tab strip was hidden: lay the pane out again, or the
    // body below keeps its old place (over the tab strip).
    const bool relayout = !tabs->IsShown() || !chips->IsShown();
    tabs->Show();
    chips->Show();
    if (relayout) pane->Layout();
    const StepResult& r = *s;
    tabs->set_label(kHeaders, "Headers", r.resp_headers.empty() && r.resp_trailers.empty()
                                              ? std::string()
                                              : std::to_string(r.resp_headers.size() + r.resp_trailers.size()));
    if (!r.asserts.empty()) {
      tabs->set_label(kTests, "Tests", std::to_string(r.asserts_passed) + "/" + std::to_string(r.asserts.size()));
      tabs->set_count_tone(kTests, r.asserts_failed ? rv::Tone::Err : rv::Tone::Ok);
    } else {
      tabs->set_label(kTests, "Tests", r.failed() ? "!" : std::string());
      tabs->set_count_tone(kTests, r.failed() ? rv::Tone::Err : rv::Tone::Plain);
    }
    {
      const size_t nv = r.captures.size() + (r.data_row ? r.data_row->size() : 0);
      tabs->set_label(kCaptures, "Variables", nv ? std::to_string(nv) : r.unresolved.empty() ? std::string() : "!");
      tabs->set_count_tone(kCaptures, r.unresolved.empty() ? rv::Tone::Plain : rv::Tone::Warn);
    }
    tabs->set_label(kConsole, "Console", r.logs.empty() ? std::string() : std::to_string(r.logs.size()));
    bool log_err = false;
    for (const auto& l : r.logs) log_err = log_err || l.level == ScriptLog::Level::Error;
    tabs->set_count_tone(kConsole, log_err ? rv::Tone::Err : rv::Tone::Plain);
    tabs->set_label(kResponse, "Response", r.body_preview.empty() ? std::string() : fmt_bytes(r.size));
    built = -1;
    select(sel, false);
  }

  void select(int i, bool user) {
    sel = i;
    tabs->select(i);
    const StepPtr& s = pane->s_;
    if (!s) return;
    const StepResult& r = *s;
    if (user) built = -1;
    if (text) text->Hide();
    canvas->Hide();
    if (body) body->Hide();
    if (i == kResponse) {
      rv::BodyView* b = body_view();
      if (built != gen) {
        built = gen;
        if (r.body_file) load_full(s);
        const bool had_body = filter_open;
        filter_open = false;
        if (r.kind == RequestKind::Grpc && r.sent && !r.transport_ok && r.body_preview.empty()) {
          // The status and its google.rpc.Status details decoded, as the request tab shows them.
          b->show_grpc_error("gRPC " + (r.status_text.empty() ? std::to_string(r.status) : r.status_text) +
                                 (r.error.empty() || r.error == r.status_text ? "" : ": " + r.error),
                             r.grpc_details);
        } else if (r.body_preview.empty() && r.body_file) {
          b->show_message("Loading the " + fmt_bytes(r.body_file->size()) + " response...");
        } else if (r.body_preview.empty()) {
          b->show_message(r.skipped ? "Not run: the run ended before this request."
                          : !r.sent ? (r.error.empty() ? std::string("Not sent.") : r.error)
                          : r.kind == RequestKind::Http && !r.transport_ok && !r.stopped()
                              ? "No response: " + (r.error.empty() ? std::string("transport error") : r.error)
                          : r.stopped() ? std::string("Stopped before the response arrived.")
                          : r.size ? "The body of this step was not kept: a long run keeps the bodies of failed steps and of the first " +
                                         std::to_string(runner::kKeepPreviews) + " passing ones, within a memory budget."
                                   : std::string("Empty body."));
        } else {
          auto resp = std::make_shared<HttpResponse>();
          resp->ok = true;
          resp->status = r.status;
          resp->status_text = r.status_text;
          std::string ct = r.content_type();
          if (r.kind == RequestKind::Grpc) ct = "application/json";
          if (!ct.empty()) resp->headers.add("Content-Type", ct);
          resp->body = r.body_preview;
          resp->sizes.response_body = r.body_preview.size();
          b->set_http(resp);  // the whole body follows from its file (load_full)
          filter_open = true;  // a filter bar may be opened on this body
        }
        if (!filter_open && had_body) {
          // A JSONPath filter bar opened on an earlier step's body does not stay over a message
          // (set_filter("") alone keeps a bar whose text was never applied).
          b->set_filter(" ");
          b->set_filter("");
        }
      }
      b->Show();
    } else if (i == kHeaders || i == kRequest || i == kConsole) {
      Styled st;
      if (i == kConsole) {
        console_text(r, st);
      } else if (i == kHeaders) {
        if (r.kind == RequestKind::Http && r.sent && r.transport_ok)
          st.add((r.http_version.empty() ? std::string("HTTP") : r.http_version) + " " + status_label(r) + "\n", kStLine);
        if (r.resp_headers.empty() && r.resp_trailers.empty())
          st.add(r.sent ? "No response headers.\n" : "Not sent: no response.\n", kStDim);
        if (r.kind == RequestKind::Grpc && !r.resp_headers.empty()) st.title("INITIAL METADATA");
        for (const auto& [k, v] : r.resp_headers) st.header(k, v);
        if (!r.resp_trailers.empty()) {
          st.title("TRAILERS");
          for (const auto& [k, v] : r.resp_trailers) {
            if (k == "grpc-status-details-bin" && !r.grpc_details.empty()) {
              std::string one;
              st.header(k, "decoded: " + (fmt::compact_json(r.grpc_details, one, 4096) ? one : r.grpc_details));
            } else {
              st.header(k, v);
            }
          }
        }
      } else {
        st.add((r.kind == RequestKind::Http ? r.method : "gRPC " + r.method) + " " + r.url + "\n", kStLine);
        if (r.req_headers.empty() && r.req_body.empty() && !r.sent)
          st.add("\n" + (r.error.empty() ? std::string("Not sent.") : r.error) + "\n", kStDim);
        if (!r.req_headers.empty()) {
          st.title(r.kind == RequestKind::Http ? "HEADERS" : "METADATA");
          for (const auto& [k, v] : r.req_headers) st.header(k, v);
        }
        if (r.kind == RequestKind::Http && r.sent) {
          // What libcurl adds on its own (HTTP/1.1 form), unless the request set it.
          std::vector<Header> implied;
          auto has = [&](const char* n) { return !header_value(r.req_headers, n).empty(); };
          std::string host = r.url;
          if (const size_t at = host.find("://"); at != std::string::npos) host.erase(0, at + 3);
          host = host.substr(0, host.find_first_of("/?#"));
          if (const size_t at = host.rfind('@'); at != std::string::npos) host.erase(0, at + 1);
          if (!has("host")) implied.push_back({"Host", host});
          if (!has("user-agent")) implied.push_back({"User-Agent", "watt/0.1"});
          if (!has("accept")) implied.push_back({"Accept", "*/*"});
          if (!has("accept-encoding")) implied.push_back({"Accept-Encoding", "deflate, gzip"});
          if (r.req_body_size && !has("content-length")) implied.push_back({"Content-Length", std::to_string(r.req_body_size)});
          if (r.req_body_size && !has("content-type")) implied.push_back({"Content-Type", "application/x-www-form-urlencoded"});
          if (!implied.empty()) {
            st.title("ADDED BY THE HTTP ENGINE");
            for (const auto& [k, v] : implied) st.header(k, v);
          }
        }
        if (!r.req_body.empty()) {
          st.title("BODY  " + fmt_bytes(r.req_body_size) +
                   (r.req_body_truncated ? "  (first " + fmt_bytes(r.req_body.size()) + " shown)" : ""));
          st.body(r.req_body, r.kind == RequestKind::Grpc ? std::string("application/json")
                                                          : header_value(r.req_headers, "content-type"));
        }
      }
      text_pane()->set(st.text, st.styles);
      text->Show();
    } else {
      canvas->set(s, i);
      canvas->Show();
    }
    body_host->Layout();
  }

  static void console_text(const StepResult& r, Styled& st) {
    if (r.logs.empty()) {
      st.add(!r.has_scripts      ? "This request has no scripts.\n"
             : !r.scripts_ran    ? "Scripts not run (workspace not trusted).\n"
                                 : "The scripts printed nothing (console.log, console.warn...).\n",
             kStDim);
      return;
    }
    int at = -1;
    for (const auto& l : r.logs) {
      if (static_cast<int>(l.post) != at) {
        at = l.post;
        st.title(l.post ? "POST-RESPONSE SCRIPT" : "PRE-REQUEST SCRIPT");
      }
      const char* lv = l.level == ScriptLog::Level::Error ? "error " : l.level == ScriptLog::Level::Warn ? "warn  "
                       : l.level == ScriptLog::Level::Info ? "info  " : "";
      if (*lv) st.add(lv, l.level == ScriptLog::Level::Error ? fmt::json_style::Error : kStName);
      st.add(l.text + "\n", l.level == ScriptLog::Level::Error ? fmt::json_style::Error : 0);
    }
    if (r.logs_truncated) st.add("\n(more output was not kept)\n", kStDim);
  }

  // The current tab as text.
  std::string tab_text() {
    const StepPtr& s = pane->s_;
    if (!s) return {};
    const StepResult& r = *s;
    if (sel == kResponse) return r.body_preview;
    if (sel == kHeaders || sel == kRequest || sel == kConsole) return text ? text->GetText().ToStdString(wxConvUTF8) : std::string();
    std::string o;
    if (sel == kTiming) {
      o = "Total " + fmt_ms(r.timing.total_ms) + "\n";
      for (const auto& p : r.phases) o += std::string(p.name) + "\t" + fmt_ms(p.ms) + "\n";
    } else if (sel == kTests) {
      for (const auto& a : r.asserts)
        o += std::string(a.pass ? "PASS " : "FAIL ") + a.label() + (a.pass ? "" : "  (" + a.why() + ")") + "\n";
    } else {
      if (r.data_row)
        for (const auto& [k, v] : *r.data_row) o += k + " = " + v + "   (data row " + std::to_string(r.data_index + 1) + ")\n";
      for (const auto& n : r.unresolved) o += "{{" + n + "}} unresolved\n";
      for (const auto& c : r.captures) o += c.var + " = " + (c.ok ? c.value : c.message) + "\n";
    }
    return o;
  }

  void copy() {
    const std::string t = tab_text();
    if (t.empty()) return;
    if (wxTheClipboard->Open()) {
      wxTheClipboard->SetData(new wxTextDataObject(U(t)));
      wxTheClipboard->Close();
    }
  }

  DetailPane* pane;
  HeadView* header = nullptr;
  rv::TabStrip* tabs = nullptr;
  rv::ChipBar* chips = nullptr;
  wxPanel* body_host = nullptr;
  wxBoxSizer* body_sizer = nullptr;
  TextPane* text = nullptr;
  Canvas* canvas = nullptr;
  rv::BodyView* body = nullptr;
  int sel = kResponse;
  int gen = 0, built = -1;
  bool filter_open = false;  // a body was shown last (its filter bar may be open)
  std::set<wxWindow*> hooked;
  std::shared_ptr<int> alive = std::make_shared<int>(0);  // workers' CallAfter checks it
};

DetailPane::DetailPane(wxWindow* parent) : wxPanel(parent, wxID_ANY) { impl_ = std::make_unique<Impl>(this); }

DetailPane::~DetailPane() = default;

void DetailPane::show(StepPtr s, const std::string& hint) {
  s_ = std::move(s);
  if (impl_) impl_->show(s_, hint);
}

void DetailPane::set_can_open(bool on) {
  if (impl_) impl_->chips->set_enabled(kOpen, on);
}

void DetailPane::warm_up() {
  if (impl_) impl_->body_view();
}

}  // namespace watt::app::runner_ui
