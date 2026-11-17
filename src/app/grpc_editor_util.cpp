#include "app/grpc_editor_util.h"

#include <wx/dcmemory.h>
#include <wx/graphics.h>
#include <wx/image.h>
#include <wx/settings.h>
#include <wx/stc/stc.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <set>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

namespace watt::app::grpced {

Theme theme() {
  Theme t;
  t.dark = wxSystemSettings::GetAppearance().IsDark();
  if (t.dark) {
    t.bg = wxColour(30, 30, 30);
    t.panel = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
    t.fg = wxColour(212, 212, 212);
    t.muted = wxColour(140, 140, 140);
    t.border = wxColour(60, 60, 60);
    t.accent = wxColour(0, 120, 212);
    t.accent_fg = *wxWHITE;
    t.error = wxColour(241, 112, 112);
    t.ok = wxColour(115, 201, 145);
  } else {
    t.bg = *wxWHITE;
    t.panel = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
    t.fg = wxColour(30, 30, 30);
    t.muted = wxColour(110, 110, 110);
    t.border = wxColour(220, 220, 220);
    t.accent = wxColour(0, 103, 192);
    t.accent_fg = *wxWHITE;
    t.error = wxColour(196, 43, 28);
    t.ok = wxColour(16, 124, 16);
  }
  return t;
}

const char* kind_short(GrpcCallKind k) {
  switch (k) {
    case GrpcCallKind::Unary: return "UNARY";
    case GrpcCallKind::ServerStreaming: return "SERVER";
    case GrpcCallKind::ClientStreaming: return "CLIENT";
    case GrpcCallKind::Bidi: return "BIDI";
  }
  return "";
}

const char* kind_long(GrpcCallKind k) {
  switch (k) {
    case GrpcCallKind::Unary: return "Unary";
    case GrpcCallKind::ServerStreaming: return "Server streaming";
    case GrpcCallKind::ClientStreaming: return "Client streaming";
    case GrpcCallKind::Bidi: return "Bidirectional streaming";
  }
  return "";
}

wxColour kind_colour(GrpcCallKind k, bool dark) {
  switch (k) {
    case GrpcCallKind::Unary: return dark ? wxColour(46, 160, 67) : wxColour(26, 127, 55);
    case GrpcCallKind::ServerStreaming: return dark ? wxColour(56, 132, 244) : wxColour(9, 105, 218);
    case GrpcCallKind::ClientStreaming: return dark ? wxColour(210, 140, 30) : wxColour(176, 106, 0);
    case GrpcCallKind::Bidi: return dark ? wxColour(163, 113, 247) : wxColour(130, 80, 223);
  }
  return *wxLIGHT_GREY;
}

wxBitmap make_badge(const wxString& text, const wxColour& fill, const wxColour& text_colour, int w, int h,
                    double font_px) {
  wxImage img(w, h);
  img.InitAlpha();
  unsigned char* a = img.GetAlpha();
  std::fill(a, a + static_cast<size_t>(w) * h, static_cast<unsigned char>(0));
  wxBitmap bmp(img, 32);
  {
    wxMemoryDC dc(bmp);
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (gc) {
      gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
      gc->SetBrush(wxBrush(fill));
      gc->SetPen(*wxTRANSPARENT_PEN);
      const double r = h * 0.25;
      gc->DrawRoundedRectangle(0.5, 1.5, w - 1.0, h - 3.0, r);
      wxFont f(wxFontInfo().Bold().FaceName("Segoe UI"));
      f.SetFractionalPointSize(font_px * 72.0 / 96.0);
      gc->SetFont(f, text_colour);
      double tw = 0, th = 0;
      gc->GetTextExtent(text, &tw, &th);
      gc->DrawText(text, (w - tw) / 2.0, (h - th) / 2.0);
    }
  }
  return bmp;
}

void setup_json_editor(wxStyledTextCtrl* stc, const Theme& t) {
  wxFont mono(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE).FaceName("Consolas"));
  stc->StyleSetFont(wxSTC_STYLE_DEFAULT, mono);
  stc->StyleSetBackground(wxSTC_STYLE_DEFAULT, t.bg);
  stc->StyleSetForeground(wxSTC_STYLE_DEFAULT, t.fg);
  stc->StyleClearAll();
  stc->StyleSetForeground(wxSTC_JSON_PROPERTYNAME, t.dark ? wxColour(156, 220, 254) : wxColour(4, 81, 165));
  stc->StyleSetForeground(wxSTC_JSON_STRING, t.dark ? wxColour(206, 145, 120) : wxColour(163, 21, 21));
  stc->StyleSetForeground(wxSTC_JSON_STRINGEOL, t.error);
  stc->StyleSetForeground(wxSTC_JSON_NUMBER, t.dark ? wxColour(181, 206, 168) : wxColour(9, 134, 88));
  stc->StyleSetForeground(wxSTC_JSON_KEYWORD, t.dark ? wxColour(86, 156, 214) : wxColour(0, 0, 255));
  stc->StyleSetForeground(wxSTC_JSON_OPERATOR, t.dark ? wxColour(180, 180, 180) : wxColour(60, 60, 60));
  stc->StyleSetForeground(wxSTC_JSON_ERROR, t.error);
  stc->StyleSetForeground(wxSTC_JSON_LINECOMMENT, t.muted);
  stc->StyleSetForeground(wxSTC_JSON_BLOCKCOMMENT, t.muted);
  stc->StyleSetBackground(wxSTC_STYLE_BRACELIGHT, t.dark ? wxColour(60, 60, 60) : wxColour(220, 230, 245));
  stc->StyleSetForeground(wxSTC_STYLE_BRACELIGHT, t.fg);
  stc->StyleSetBold(wxSTC_STYLE_BRACELIGHT, true);
  stc->StyleSetForeground(wxSTC_STYLE_BRACEBAD, t.error);
  stc->StyleSetBackground(wxSTC_STYLE_CALLTIP, t.dark ? wxColour(45, 45, 48) : wxColour(250, 250, 250));
  stc->StyleSetForeground(wxSTC_STYLE_CALLTIP, t.fg);
  stc->SetCaretForeground(t.fg);
  stc->SetSelBackground(true, t.dark ? wxColour(38, 79, 120) : wxColour(173, 214, 255));
  // margins: line numbers, folding
  stc->SetMarginType(0, wxSTC_MARGIN_NUMBER);
  stc->StyleSetBackground(wxSTC_STYLE_LINENUMBER, t.bg);
  stc->StyleSetForeground(wxSTC_STYLE_LINENUMBER, t.dark ? wxColour(110, 110, 110) : wxColour(160, 160, 160));
  stc->SetMarginType(1, wxSTC_MARGIN_SYMBOL);
  stc->SetMarginMask(1, wxSTC_MASK_FOLDERS);
  stc->SetMarginSensitive(1, true);
  stc->SetFoldMarginColour(true, t.bg);
  stc->SetFoldMarginHiColour(true, t.bg);
  const wxColour fm = t.dark ? wxColour(120, 120, 120) : wxColour(150, 150, 150);
  const int folds[][2] = {{wxSTC_MARKNUM_FOLDEROPEN, wxSTC_MARK_ARROWDOWN},
                          {wxSTC_MARKNUM_FOLDER, wxSTC_MARK_ARROW},
                          {wxSTC_MARKNUM_FOLDERSUB, wxSTC_MARK_EMPTY},
                          {wxSTC_MARKNUM_FOLDERTAIL, wxSTC_MARK_EMPTY},
                          {wxSTC_MARKNUM_FOLDEREND, wxSTC_MARK_ARROW},
                          {wxSTC_MARKNUM_FOLDEROPENMID, wxSTC_MARK_ARROWDOWN},
                          {wxSTC_MARKNUM_FOLDERMIDTAIL, wxSTC_MARK_EMPTY}};
  for (const auto& f : folds) stc->MarkerDefine(f[0], f[1], fm, fm);
  stc->SetAutomaticFold(wxSTC_AUTOMATICFOLD_CLICK | wxSTC_AUTOMATICFOLD_SHOW | wxSTC_AUTOMATICFOLD_CHANGE);
  stc->SetMarginLeft(stc->FromDIP(4));
  stc->SetTabWidth(2);
  stc->SetUseTabs(false);
  stc->SetIndent(2);
  stc->SetTabIndents(true);
  stc->SetBackSpaceUnIndents(true);
  stc->SetWrapMode(wxSTC_WRAP_NONE);
  stc->SetLayoutCache(wxSTC_CACHE_PAGE);
  stc->SetScrollWidthTracking(true);
  stc->SetScrollWidth(1);
  stc->SetEndAtLastLine(false);
  stc->SetCaretLineVisible(true);
  stc->SetCaretLineBackground(t.dark ? wxColour(40, 40, 40) : wxColour(246, 248, 250));
  stc->StyleSetForeground(wxSTC_STYLE_INDENTGUIDE, t.dark ? wxColour(55, 55, 55) : wxColour(225, 225, 225));
  stc->SetCodePage(wxSTC_CP_UTF8);
  stc->SetEOLMode(wxSTC_EOL_LF);
  stc->UsePopUp(wxSTC_POPUP_ALL);
  stc->AutoCompSetIgnoreCase(true);
  stc->AutoCompSetAutoHide(true);
  stc->AutoCompSetDropRestOfWord(false);
  stc->AutoCompSetMaxHeight(10);
  stc->AutoCompSetSeparator('\n');
  set_json_lexing(stc, true);
  fit_line_margin(stc);
}

void set_json_lexing(wxStyledTextCtrl* stc, bool on) {
  if (on) {
    stc->SetLexer(wxSTC_LEX_JSON);
    stc->SetProperty("fold", "1");
    stc->SetProperty("fold.compact", "0");
    stc->SetIdleStyling(wxSTC_IDLESTYLING_ALL);
    stc->SetMarginWidth(1, stc->FromDIP(14));
    stc->SetIndentationGuides(wxSTC_IV_LOOKBOTH);
  } else {
    stc->SetLexer(wxSTC_LEX_NULL);
    stc->SetProperty("fold", "0");
    stc->SetIdleStyling(wxSTC_IDLESTYLING_NONE);
    stc->SetMarginWidth(1, 0);
    stc->SetIndentationGuides(wxSTC_IV_NONE);
    if (!stc->GetAllLinesVisible()) stc->ShowLines(0, stc->GetLineCount() - 1);
    stc->ClearDocumentStyle();
  }
}

void fit_line_margin(wxStyledTextCtrl* stc) {
  int digits = 1;
  for (int n = stc->GetLineCount(); n >= 10; n /= 10) ++digits;
  digits = std::max(digits, 3);
  const int w = stc->TextWidth(wxSTC_STYLE_LINENUMBER, wxString('9', static_cast<size_t>(digits) + 1));
  if (stc->GetMarginWidth(0) != w) stc->SetMarginWidth(0, w);
}

namespace {

std::string pretty(const rapidjson::Value& v) {
  rapidjson::StringBuffer sb;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> w(sb);
  w.SetIndent(' ', 2);
  v.Accept(w);
  return std::string(sb.GetString(), sb.GetSize());
}

constexpr unsigned kParse = rapidjson::kParseFullPrecisionFlag;

}  // namespace

bool split_json_array(const std::string& body, std::vector<std::string>& out) {
  const size_t p = body.find_first_not_of(" \t\r\n");
  if (p == std::string::npos || body[p] != '[') return false;
  rapidjson::Document d;
  d.Parse<kParse>(body.data(), body.size());
  if (d.HasParseError() || !d.IsArray()) return false;
  out.clear();
  out.reserve(d.Size());
  for (const auto& v : d.GetArray()) out.push_back(pretty(v));
  return true;
}

std::string join_messages(const std::vector<std::string>& msgs) {
  if (msgs.empty()) return "[]";
  std::string out = "[\n";
  for (size_t i = 0; i < msgs.size(); ++i) {
    out += "  ";
    const std::string& m = msgs[i];
    size_t b = m.find_first_not_of(" \t\r\n");
    size_t e = m.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) {
      out += "{}";
    } else {
      for (size_t k = b; k <= e; ++k) {
        if (m[k] == '\r') continue;
        out += m[k];
        if (m[k] == '\n') out += "  ";
      }
    }
    out += i + 1 < msgs.size() ? ",\n" : "\n";
  }
  out += "]";
  return out;
}

std::string json_problem(const std::string& s, size_t* offset) {
  if (s.find_first_not_of(" \t\r\n") == std::string::npos) return "empty";
  rapidjson::Document d;
  d.Parse<kParse>(s.data(), s.size());
  if (!d.HasParseError()) return {};
  const size_t off = d.GetErrorOffset();
  if (offset) *offset = off;
  size_t line = 1, col = 1;
  for (size_t i = 0; i < off && i < s.size(); ++i) {
    if (s[i] == '\n') {
      ++line;
      col = 1;
    } else if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
      ++col;
    }
  }
  return "line " + std::to_string(line) + ", col " + std::to_string(col) + ": " +
         rapidjson::GetParseError_En(d.GetParseError());
}

std::string json_pretty(const std::string& s) {
  rapidjson::Document d;
  d.Parse<kParse>(s.data(), s.size());
  if (d.HasParseError()) return s;
  return pretty(d);
}

bool strip_scheme(std::string& address, bool* tls) {
  bool changed = false;
  struct Scheme {
    const char* prefix;
    bool tls;
  };
  static const Scheme kSchemes[] = {{"grpcs://", true}, {"https://", true}, {"grpc://", false}, {"http://", false}};
  for (const auto& s : kSchemes) {
    const size_t n = std::char_traits<char>::length(s.prefix);
    if (address.size() >= n && std::equal(s.prefix, s.prefix + n, address.begin(), [](char a, char b) {
          return a == static_cast<char>(std::tolower(static_cast<unsigned char>(b)));
        })) {
      address.erase(0, n);
      if (tls) *tls = s.tls;
      changed = true;
      break;
    }
  }
  while (!address.empty() && (address.back() == '/' || address.back() == ' ')) {
    address.pop_back();
    changed = true;
  }
  while (!address.empty() && address.front() == ' ') {
    address.erase(0, 1);
    changed = true;
  }
  return changed;
}

std::string one_line_preview(const std::string& json, size_t max_chars) {
  std::string out;
  bool in_str = false, esc = false, space = false;
  for (char c : json) {
    if (out.size() >= max_chars) {
      out += "...";
      break;
    }
    if (in_str) {
      out += c;
      if (esc) esc = false;
      else if (c == '\\') esc = true;
      else if (c == '"') in_str = false;
      continue;
    }
    if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
      space = true;
      continue;
    }
    if (space && !out.empty() && out.back() != '{' && out.back() != '[' && c != '}' && c != ']') out += ' ';
    space = false;
    out += c;
    if (c == '"') in_str = true;
  }
  return out;
}

namespace {

void collect_keys(const rapidjson::Value& v, std::set<std::string>& out) {
  if (v.IsObject()) {
    for (const auto& m : v.GetObject()) {
      out.emplace(m.name.GetString(), m.name.GetStringLength());
      collect_keys(m.value, out);
    }
  } else if (v.IsArray()) {
    for (const auto& e : v.GetArray()) collect_keys(e, out);
  }
}

// Shell words of a pasted command: '...', "..." (backslash escapes), bare words with backslash
// escapes; line continuations of bash (\), cmd (^) and PowerShell (`) count as whitespace.
std::vector<std::string> shell_words(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  bool have = false;
  auto flush = [&] {
    if (have) out.push_back(std::move(cur));
    cur.clear();
    have = false;
  };
  auto continuation = [&](size_t j) {
    size_t k = j + 1;
    if (k < s.size() && s[k] == '\r') ++k;
    return k < s.size() && s[k] == '\n' ? k : std::string::npos;
  };
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if ((c == '\\' || c == '^' || c == '`') && continuation(i) != std::string::npos) {
      i = continuation(i);
      flush();
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      flush();
      continue;
    }
    have = true;
    if (c == '\'') {
      for (++i; i < s.size() && s[i] != '\''; ++i) cur += s[i];
    } else if (c == '"') {
      for (++i; i < s.size() && s[i] != '"'; ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == '"' || s[i + 1] == '\\' || s[i + 1] == '$'))
          ++i;
        else if (s[i] == '`' && i + 1 < s.size() && s[i + 1] == '"')
          ++i;
        cur += s[i];
      }
    } else if (c == '\\' && i + 1 < s.size()) {
      cur += s[++i];
    } else {
      cur += c;
    }
  }
  flush();
  return out;
}

}  // namespace

size_t break_json_lines(const char* text, size_t n, size_t soft, std::string& out) {
  out.clear();
  out.reserve(n + n / soft + 16);
  size_t breaks = 0, line = 0;
  bool in_str = false, esc = false;
  for (size_t i = 0; i < n; ++i) {
    const char c = text[i];
    out += c;
    ++line;
    if (in_str) {
      if (esc) esc = false;
      else if (c == '\\') esc = true;
      else if (c == '"') in_str = false;
      continue;
    }
    if (c == '"') {
      in_str = true;
    } else if (c == '\n') {
      line = 0;
    } else if ((c == ',' || c == '{' || c == '[') && line >= soft) {
      out += '\n';
      line = 0;
      ++breaks;
    }
  }
  return breaks;
}

std::vector<std::string> json_keys(const std::string& json) {
  rapidjson::Document d;
  d.Parse<kParse>(json.data(), json.size());
  if (d.HasParseError()) return {};
  std::set<std::string> keys;
  collect_keys(d, keys);
  return {keys.begin(), keys.end()};
}

bool parse_grpcurl(const std::string& cmd, GrpcurlImport& out, std::string* error) {
  auto fail = [&](const std::string& e) {
    if (error) *error = e;
    return false;
  };
  const std::vector<std::string> w = shell_words(cmd);
  if (w.empty()) return fail("empty");
  {
    std::string first = w[0];
    for (auto& c : first) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const size_t slash = first.find_last_of("/\\");
    if (slash != std::string::npos) first = first.substr(slash + 1);
    if (first != "grpcurl" && first != "grpcurl.exe") return fail("not a grpcurl command");
  }
  out = GrpcurlImport{};
  static const std::set<std::string> kBool = {"plaintext",  "insecure", "v",          "vv",
                                              "emit-defaults", "use-reflection", "allow-unknown-fields",
                                              "msg-template", "format-error", "unix", "expand-headers",
                                              "alts", "help", "version"};
  std::vector<std::string> pos;
  for (size_t i = 1; i < w.size(); ++i) {
    const std::string& a = w[i];
    if (a.size() < 2 || a[0] != '-') {
      pos.push_back(a);
      continue;
    }
    std::string name = a.substr(a[1] == '-' ? 2 : 1), value;
    bool has_value = false;
    if (const size_t eq = name.find('='); eq != std::string::npos) {
      value = name.substr(eq + 1);
      name.resize(eq);
      has_value = true;
    }
    if (kBool.count(name)) {
      const bool on = !has_value || value == "true" || value == "1";
      if (name == "plaintext") out.tls = !on;
      else if (name == "insecure") out.insecure = on;
      continue;
    }
    if (!has_value) {
      if (i + 1 >= w.size()) return fail("-" + name + " needs a value");
      value = w[++i];
    }
    if (name == "H" || name == "rpc-header" || name == "reflect-header") {
      const size_t c = value.find(':');
      std::string k = value.substr(0, c), v = c == std::string::npos ? std::string() : value.substr(c + 1);
      while (!v.empty() && v.front() == ' ') v.erase(0, 1);
      while (!k.empty() && k.back() == ' ') k.pop_back();
      if (!k.empty()) out.metadata.emplace_back(k, v);
      if (name == "reflect-header") out.warnings.push_back("-reflect-header " + k + " is sent on every call");
    } else if (name == "d") {
      if (value == "@") {
        out.warnings.push_back("-d @ reads stdin; paste the message into the editor");
      } else {
        out.has_data = true;
        out.data = value;
      }
    } else if (name == "max-time") {
      char* end = nullptr;
      const double secs = std::strtod(value.c_str(), &end);
      if (end && *end == 0 && secs >= 0) out.deadline_ms = std::lround(secs * 1000.0);
      else out.warnings.push_back("bad -max-time " + value);
    } else if (name == "authority") {
      out.authority = value;
    } else if (name == "import-path") {
      out.import_paths.push_back(value);
    } else if (name == "proto" || name == "protoset") {
      out.protos.push_back(value);
    } else if (name != "connect-timeout" && name != "format" && name != "keepalive-time" && name != "max-msg-sz") {
      out.warnings.push_back("-" + name + " is not imported");
    }
  }
  if (pos.empty()) return fail("no address in the command");
  out.address = pos[0];
  if (pos.size() >= 2) {
    std::string m = pos[1];
    if (m == "list" || m == "describe") {
      out.warnings.push_back("grpcurl " + m + ": pick a method from the list");
    } else {
      if (!m.empty() && m[0] == '/') m.erase(0, 1);
      if (m.find('/') == std::string::npos) {
        const size_t dot = m.rfind('.');
        if (dot != std::string::npos) m[dot] = '/';
      }
      out.method = "/" + m;
    }
  }
  return true;
}

}  // namespace watt::app::grpced

