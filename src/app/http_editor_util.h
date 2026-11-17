#pragma once
// Pure helpers of the HTTP request editor (no wx): URL query <-> params, form-urlencoded bodies,
// {{var}} spans, and a JSON re-indenter that never parses values (so {{vars}}, big numbers,
// comments and odd formatting survive a Beautify byte for byte, and 10 MB re-indents in ~30 ms on
// a worker thread).
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace watt::app::http_editor {

struct KvItem {
  std::string key;
  std::string value;
  bool enabled = true;
  bool bare = false;  // query param written without '=' ("?flag")
};

struct UrlParts {
  std::string_view base;      // before '?' (or before '#' when there is no query)
  std::string_view query;     // between '?' and '#', without both
  std::string_view fragment;  // from '#' on (with '#'), may be empty
  bool has_query = false;
};
UrlParts split_url(std::string_view url);

// Query items as written (not percent-decoded: the URL is the source of truth and must round-trip).
std::vector<KvItem> parse_query(std::string_view query);
// Query items percent-decoded for display (%XX only, '+' stays '+'; an item whose decoded text
// would not be valid UTF-8 or would hold a control character is kept as written).
std::vector<KvItem> parse_query_decoded(std::string_view query);
// RFC 3986 query component encoding of one key or value: unreserved and the sub-delims a query
// may carry stay literal, space is %20, '&' '=' '#' '+' '%' and everything else is %XX.
// {{vars}} are kept literal.
std::string query_encode(std::string_view s);
// %XX decoding (no '+' -> space). nullopt-free: invalid sequences stay as written.
std::string query_decode(std::string_view s);
// url with its query rebuilt from decoded (display) items: an item whose key and value equal the
// decoded form of an item already in the URL keeps that item's original spelling (so an
// untouched "a+b" or "%7e" is not rewritten), any other is query_encode()d.
std::string with_decoded_query(std::string_view url, const std::vector<KvItem>& items);
// The URL made safe to send: outside {{vars}}, in the path / query / fragment, a space becomes
// %20 and control characters, non-ASCII bytes, '"' '<' '>' '' '^' '`' '{' '|' '}' and a '%'
// not followed by two hex digits are %XX-encoded. Scheme and host are left alone.
std::string normalize_url(std::string_view url);

// {{var}} substitution as the request is sent. expand_var("{{name}}") returns the value, or its
// argument unchanged when the variable is not defined (then it stays as written).
using VarExpander = std::function<std::string(const std::string&)>;
// The URL as sent: expand first, then encode. A value inside the query (after the first '?',
// before '#') is query_encode()d, so "ann marie" is "ann%20marie" and "a&b=c d" stays one value;
// the whole result is then normalize_url()ed, so a value elsewhere ({{base}}, a path segment) is
// kept as written but made safe. The "Sent as" preview and the send both use this.
std::string expand_url(std::string_view tmpl, const VarExpander& expand_var);
// Path variables: "/:name" segments of the path (Postman, Insomnia, Bruno). The name is
// [A-Za-z0-9_-]+ right after "/:"; the scheme, the authority (a ":port"), the query, the
// fragment and {{vars}} are never read as one.
struct PathVar {
  size_t begin = 0;  // at ':'
  size_t end = 0;    // after the name
  std::string name;
};
std::vector<PathVar> find_path_vars(std::string_view url);
// Unique names in order of first appearance.
std::vector<std::string> path_var_names(std::string_view url);
// RFC 3986 path segment encoding: unreserved and sub-delims (and ':' '@') stay literal, '/' and
// everything else is %XX. {{vars}} are kept literal.
std::string path_encode(std::string_view s);
// url with each ":name" whose value is not empty replaced by path_encode(value); an empty or
// unknown one stays as written.
std::string with_path_values(std::string_view url, const std::function<std::string(const std::string&)>& value);
// An application/x-www-form-urlencoded body as sent: each {{var}} value form_encode()d.
std::string expand_form(std::string_view tmpl, const VarExpander& expand_var);

// Bulk edit text: one "key: value" line per item, a disabled item prefixed with "//".
std::string to_bulk(const std::vector<KvItem>& items);
// Parses bulk edit text: "key: value" (or "key=value" when the line has no ':'), "//" disables,
// blank lines and HTTP/2 pseudo-headers (":authority: x") are skipped.
std::vector<KvItem> parse_bulk(std::string_view text);
// Header name as sent: control characters and whitespace removed.
std::string clean_header_name(std::string_view s);
// Header value as sent: CR, LF and other control characters (except tab) removed.
std::string clean_header_value(std::string_view s);
// Enabled items joined with '&'.
std::string join_query(const std::vector<KvItem>& items);
// url with its query replaced by the enabled items (no '?' when there are none).
std::string with_query(std::string_view url, const std::vector<KvItem>& items);

// application/x-www-form-urlencoded. Encoding keeps {{vars}} literal.
std::string form_encode(std::string_view s);
std::string form_decode(std::string_view s);
std::vector<KvItem> parse_form_body(std::string_view body);  // decoded
std::string build_urlencoded(const std::vector<KvItem>& items);  // enabled only, encoded

struct VarSpan {
  size_t begin = 0;  // at "{{"
  size_t end = 0;    // after "}}"
  std::string name;  // trimmed
};
// {{name}} spans in s (same rules as Workspace::expand: "{{{" emits a literal brace).
std::vector<VarSpan> find_vars(std::string_view s);

// First syntax error of a JSON body, or nullopt when it is valid (or empty). Lenient where the
// editor is: // and /* */ comments are allowed, and a {{var}} may stand for a value or a key.
// Iterative (no recursion), ~10 ms per MB.
struct JsonIssue {
  size_t offset = 0;  // where the problem starts
  size_t length = 1;  // how much to underline
  std::string message;
};
std::optional<JsonIssue> check_json(std::string_view s);

// Re-indents JSON (2 spaces) token by token without parsing values.
std::string reindent_json(std::string_view in);
// Removes the whitespace between JSON tokens (strings, {{vars}} and comments kept as written; a
// line comment keeps its line break). minify_json(reindent_json(s)) == s for minified s.
std::string minify_json(std::string_view in);

std::string_view trim(std::string_view s);
bool iequals(std::string_view a, std::string_view b);
bool istarts_with(std::string_view s, std::string_view prefix);
// A curl command (curl / curl.exe, possibly after "$ " or with leading blanks).
bool looks_like_curl(std::string_view s);
// Length in bytes of the longest line (memchr speed: ~1 ms for 10 MB).
size_t longest_line(std::string_view s);
// First non-blank character is '{' or '['.
bool looks_like_json(std::string_view s);
// First non-blank character is '<' (an XML / SOAP body).
bool looks_like_xml(std::string_view s);

// {{ auto-close, shared by the URL bar and the body. After the user typed '{' (the caret is
// right after it): the closer to insert after the caret ("", "}" or "}}"). before / after are the
// text left / right of the caret (only the 3 / 2 nearest characters matter). "{{{" is a literal.
std::string var_closer(std::string_view before, std::string_view after);
// A typed closer steps over the same character right after the caret when the editor inserted
// that one itself (auto_closed), instead of adding another (Postman, VS Code).
inline bool steps_over(int typed, int next, bool next_auto_closed) {
  return next_auto_closed && typed == next && (typed == '}' || typed == ']' || typed == '"');
}
// The URL bar's typing rules on plain text, one key at a time (for tests and for reasoning):
// '{' completes "{{" with "}}", a typed '}' over an auto-inserted one moves past it.
struct TypingModel {
  std::string text;
  size_t caret = 0;
  std::vector<bool> auto_closed;  // parallel to text
  void type(char c);
  void type(std::string_view keys) {
    for (char c : keys) type(c);
  }
};

}  // namespace watt::app::http_editor
