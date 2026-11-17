#pragma once
// Helpers for the gRPC request editor (grpc_editor.cpp): call-kind badges, theme colours, the
// JSON message editor setup and the small JSON utilities the message list needs.
#include <wx/bitmap.h>
#include <wx/colour.h>

#include <string>
#include <vector>

#include "core/grpc_client.h"

class wxWindow;
class wxStyledTextCtrl;

namespace watt::app::grpced {

struct Theme {
  bool dark = false;
  wxColour bg, fg, muted, border, accent, accent_fg, error, ok, panel;
};
Theme theme();

const char* kind_short(GrpcCallKind k);  // "UNARY", "SERVER", "CLIENT", "BIDI"
const char* kind_long(GrpcCallKind k);   // "Unary", "Server streaming", ...
wxColour kind_colour(GrpcCallKind k, bool dark);
inline bool client_streams(GrpcCallKind k) {
  return k == GrpcCallKind::ClientStreaming || k == GrpcCallKind::Bidi;
}

// A rounded badge with `text` on `fill`, `w` x `h` physical pixels, transparent outside.
wxBitmap make_badge(const wxString& text, const wxColour& fill, const wxColour& text_colour, int w, int h,
                    double font_px);

// JSON message editor: colours, line numbers, folding margin; then set_json_lexing(stc, true).
void setup_json_editor(wxStyledTextCtrl* stc, const Theme& t);
// on: JSON lexer + folding, styled in idle chunks. off (huge documents): no lexer, no folding,
// so an edit never re-lexes megabytes. Lexer and fold properties belong to the document, so
// call it again after SetDocPointer().
void set_json_lexing(wxStyledTextCtrl* stc, bool on);
// Line number margin wide enough for the line count (at least 3 digits).
void fit_line_margin(wxStyledTextCtrl* stc);

// Copies text into out with a newline after a ',', '{' or '[' outside strings once a line has
// reached `soft` bytes, so minified JSON gets short lines (Scintilla lays out a whole line on
// every edit: a 10 MB line costs a second per keystroke). JSON whitespace between tokens is
// insignificant, so the message is unchanged. Returns the number of newlines inserted.
size_t break_json_lines(const char* text, size_t n, size_t soft, std::string& out);

// Every object key in json (nested too), sorted and unique: the message editor's completions.
std::vector<std::string> json_keys(const std::string& json);

// A pasted grpcurl command. tls follows grpcurl (on unless -plaintext).
struct GrpcurlImport {
  std::string address;
  std::string method;            // "/pkg.Svc/Method"
  bool tls = true;
  bool insecure = false;
  std::string authority;
  long deadline_ms = -1;         // -1: not given
  Metadata metadata;
  bool has_data = false;
  std::string data;
  std::vector<std::string> protos, import_paths;
  std::vector<std::string> warnings;
};
// Parses `grpcurl ...` (bash / cmd / PowerShell quoting and line continuations). False when the
// text does not start with grpcurl or has no address.
bool parse_grpcurl(const std::string& cmd, GrpcurlImport& out, std::string* error = nullptr);

// body is a JSON array: each element pretty-printed (2-space indent) into out. False when it is
// not a parseable array (out untouched).
bool split_json_array(const std::string& body, std::vector<std::string>& out);
// The messages as one JSON array (each message kept as written, indented by two spaces).
std::string join_messages(const std::vector<std::string>& msgs);
// Empty when s is valid JSON, else "line L, col C: what".
// *offset (optional) gets the byte offset of the problem.
std::string json_problem(const std::string& s, size_t* offset = nullptr);
// Pretty-printed s (2-space indent), or s unchanged if it is not valid JSON.
std::string json_pretty(const std::string& s);
// "grpcs://host:443/" -> "host:443" with *tls = true (grpc://, http:// -> false). False when
// the address had no scheme or trailing slash to strip.
bool strip_scheme(std::string& address, bool* tls);
// One line of the first message, for the stream message list ("{"text": "hi"}").
std::string one_line_preview(const std::string& json, size_t max_chars);

}  // namespace watt::app::grpced
