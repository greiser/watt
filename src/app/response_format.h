#pragma once
// Response body formatting for the response view. Pure functions, no wx: they run on the
// response view's worker thread (pretty-printing a 10 MB body never touches the UI thread).
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace watt::app::fmt {

enum class BodyKind { Empty, Json, Xml, Html, Text, Binary };

// From the Content-Type header when it says so, else sniffed from the first bytes.
BodyKind detect_kind(std::string_view content_type, std::string_view body);
const char* kind_name(BodyKind k);

// Re-indents JSON (2 spaces) without building a tree: one pass over the bytes, strings copied
// verbatim. Returns false (out unspecified, *error_at = byte offset) when the input is not
// well-formed enough to indent (unbalanced brackets, unterminated string, trailing garbage).
bool pretty_json(std::string_view in, std::string& out, size_t* error_at = nullptr);

// Indents XML when it arrives on few lines; returns false when it does not look like XML.
bool pretty_xml(std::string_view in, std::string& out);

// Indents HTML (2 spaces): block elements on their own lines, inline elements and text kept
// together on one line (whitespace runs collapsed), void elements (<meta>, <br>, ...) and
// unquoted attributes understood, unclosed <p> / <li> / <td> ... closed implicitly. The contents of
// <script> and <style> are re-indented as a block (line structure kept), <pre> / <textarea> are
// copied byte for byte. Never fails on tag soup; returns false only for input with no tags.
bool pretty_html(std::string_view in, std::string& out);

// Readable text of an HTML page for the Preview: title, headings, paragraphs and list items on
// their own lines, links as "text <url>", script / style / head dropped, entities decoded.
void html_to_text(std::string_view in, std::string& out);

// The page for the rendered Preview: <script>, <style>, <noscript>, <iframe>, <object>, <embed>,
// <template> and <svg> elements removed with their contents (nothing runs, nothing is fetched).
void html_for_preview(std::string_view in, std::string& out);

// One-line form of a small JSON value ({"a": 1, "b": [1, 2]}): whitespace outside strings
// collapsed. False (out unspecified) when the result would be longer than max.
bool compact_json(std::string_view in, std::string& out, size_t max);

// Classic hex dump, 16 bytes a line: "00000000  7b 22 61 ... |{"a...|". At most max_bytes of
// input are dumped; *truncated tells whether the input was longer.
void hex_dump(std::string_view in, std::string& out, size_t max_bytes, bool* truncated);

// Scintilla styling computed off the UI thread (the Lexilla JSON lexer manages ~6 MB/s on the
// UI thread; this runs at memory speed on the worker). Style ids are Lexilla's SCE_JSON_* so the
// view's JSON colors apply. styles gets one byte per input byte; levels one Scintilla fold
// level per line (base 0x400 + depth, 0x2000 on lines that open a container).
namespace json_style {
enum : char { Default = 0, Number = 1, String = 2, PropertyName = 4, Escape = 5, LineComment = 6,
              BlockComment = 7, Operator = 8, Keyword = 11, Error = 13 };
}
void style_json(std::string_view text, std::string& styles, std::vector<int>& levels);
// Hex dump styling: offset column = 1, ASCII column = 2, hex bytes = 0.
void style_hex(std::string_view text, std::string& styles);

// "Fold all" of a very large pretty-printed JSON document (levels from style_json): every value
// one level below the root is joined onto its first line ({"id": 1, "tags": ["a"]},), cut at
// max_line bytes with an ellipsis. map[i] = the line of `pretty` that output line i starts at.
// Returns how many values were folded.
size_t fold_outline(std::string_view pretty, const std::vector<int>& levels, std::string& out, std::vector<int>& map,
                    size_t max_line);

// Width of the longest line in characters (UTF-8 continuation bytes not counted, a tab = 4)
// for the first `limit` bytes of text.
size_t longest_line_chars(std::string_view text, size_t limit);

// Length of the longest line (bytes between newlines).
size_t longest_line(std::string_view text);
// Copies text, inserting newlines so no line exceeds max bytes: after the last "}," / "]," in the
// final 1 KB before the limit, else the last ',' / space / '>' / ';', else at the limit.
// Returns how many breaks were inserted.
// breaks (optional): the offset in `text` before which each line break was inserted, ascending.
size_t break_long_lines(std::string_view text, std::string& out, size_t max, std::vector<size_t>* breaks = nullptr);
// One pass over `text`: its line count (newlines + 1) and the longest line in characters (as
// longest_line_chars counts them).
struct LineStats {
  size_t lines = 0;
  size_t longest = 0;
};
LineStats line_stats(std::string_view text);
// Styles and fold levels of `src` (style_json) carried over to the copy break_long_lines made
// with `breaks`: an inserted break takes the style of the byte before it (a string stays a string
// on its continuation line), and the pieces of a broken line share its fold level, with the
// header flag on the last one.
void expand_styles_for_breaks(std::string_view src, const std::vector<size_t>& breaks, std::string& styles,
                              std::vector<int>& levels);

// "512 B", "1.5 KB", "9.87 MB".
std::string human_bytes(uint64_t b);
// "0.123 ms", "12.34 ms", "123.4 ms", "1234 ms" (one precision rule everywhere in the view).
std::string ms(double v);
// Signed delta: "+0.12 ms" / "-3.4 ms".
std::string delta_ms(double v);
// A run-to-run difference too small to color as better / worse: under 2 ms, or under 25% of
// the larger of the two (CDN and scheduler jitter on a ~10 ms request is not a regression).
bool delta_is_noise(double d, double scale);

}  // namespace watt::app::fmt
