#pragma once
// Pure helpers of the HTTP request editor (no wx): URL query <-> params, form-urlencoded bodies,
// {{var}} spans, and a JSON re-indenter that never parses values (so {{vars}}, big numbers,
// comments and odd formatting survive a Beautify byte for byte, and 10 MB re-indents in ~30 ms on
// a worker thread).
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
// The URL as written, for a request whose "Encode URL" option is off: only what no HTTP request
// line can carry is %XX-encoded (space and control characters, outside {{vars}}, after the
// authority). Everything else ('"', '|', '{', non-ASCII, a lone '%') goes out as typed.
std::string minimal_url(std::string_view url);

// {{var}} substitution as the request is sent. expand_var("{{name}}") returns the value, or its
// argument unchanged when the variable is not defined (then it stays as written).
using VarExpander = std::function<std::string(const std::string&)>;
// The URL as sent: expand first, then encode. A value inside the query (after the first '?',
// before '#') is query_encode()d, so "ann marie" is "ann%20marie" and "a&b=c d" stays one value;
// the whole result is then normalize_url()ed, so a value elsewhere ({{base}}, a path segment) is
// kept as written but made safe. The "Sent as" preview and the send both use this.
std::string expand_url(std::string_view tmpl, const VarExpander& expand_var);
// encode = false: {{var}} values go in as they are and the result is only minimal_url()ed.
std::string expand_url(std::string_view tmpl, const VarExpander& expand_var, bool encode);
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
// encode = false (the request's "Encode URL" is off): the values go in as written.
std::string with_path_values(std::string_view url, const std::function<std::string(const std::string&)>& value,
                             bool encode = true);
// An application/x-www-form-urlencoded body as sent: each {{var}} value form_encode()d.
std::string expand_form(std::string_view tmpl, const VarExpander& expand_var);

// Bulk edit text: one "key: value" line per item, a disabled item prefixed with "//".
std::string to_bulk(const std::vector<KvItem>& items);
// Parses bulk edit text: "key: value" (or "key=value" when the line has no ':'), "//" disables,
// blank lines and HTTP/2 pseudo-headers (":authority: x") are skipped.
std::vector<KvItem> parse_bulk(std::string_view text);
// Header name as sent: control characters and whitespace removed.
std::string clean_header_name(std::string_view s);
// Why `name` is not a valid header name (RFC 9110 token: no spaces, ':', '/', control or
// non-ASCII characters), "" when it is. {{variables}} in it are not checked (they expand later).
std::string header_name_problem(std::string_view name);
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
// A {{var}} typed wrong ("{{user", "{{d}", "{userId}}", "Id}}"): what is wrong, "" when the
// braces balance. Only brace pairs next to a name count, so JSON such as {"a":{"b":1}} passes.
std::string brace_problem(std::string_view s);

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

// The JSON body's own lexer (Scintilla's LexJSON styles the rest of a line as an error after an
// unquoted {{var}}): styles in LexJSON's numbering, so the colours and folding stay the same.
// {{var}} spans are plain text (their indicator colours them), a bare word other than
// true / false / null is an error up to its end only.
enum JsonStyle : char {
  kJsDefault = 0, kJsNumber = 1, kJsString = 2, kJsStringEol = 3, kJsProperty = 4, kJsEscape = 5,
  kJsLineComment = 6, kJsBlockComment = 7, kJsOperator = 8, kJsKeyword = 11, kJsError = 13,
};
struct JsonLexState {
  int depth = 0;            // open { / [ before the line
  bool in_comment = false;  // inside /* */ at the start of the line
};
// Styles `text` (whole lines from a line start); one style byte per input byte into *styles.
// For each line: lines->push_back({depth at its start, whether it opens a block}); `st` is
// updated to the state after the text (the start of the next line).
struct JsonLexLine {
  int depth;
  bool header;
  bool in_comment;
};
void lex_json(std::string_view text, JsonLexState& st, std::string* styles, std::vector<JsonLexLine>* lines);

// The offset of the bracket that closes the '{' / '[' at `opener` (strings, \ escapes, // and
// /* */ comments skipped), or npos when it is not closed. 16 bytes per step: ~3 ms per 18 MB.
size_t json_block_end(std::string_view text, size_t opener);

// The blocks of a JSON text spanning at least min_lines lines, as (line of the opener, line of
// its closer), sorted by opener line; for a line opening several, the outermost (the one a fold
// of that line hides). Strings, \ escapes, // and /* */ comments are skipped like
// json_block_end. One pass on a worker (~30 ms per 18 MB), so folding a 1M-line block needs no
// scan on the UI thread.
std::vector<std::pair<int, int>> json_big_blocks(std::string_view text, int min_lines);

// Find bar: how many times needle occurs in hay (non-overlapping, ASCII case folding unless
// match_case), and how many of them start before `before` (the "3 of 120" index). Stops after
// `limit` matches (the count is then a lower bound). One pass, ~10 ms per 10 MB. A set *stop
// ends the walk early (cancelled: a newer search replaced this one).
struct MatchCount {
  size_t total = 0;
  size_t before = 0;
  bool capped = false;
  bool cancelled = false;
};
MatchCount count_matches(std::string_view hay, std::string_view needle, bool match_case, size_t before,
                         size_t limit = 1000000, const std::atomic<bool>* stop = nullptr);
// The match the find bar goes to: forward, the first at or after `from`; backward, the last
// that starts before `from`; either wraps around the end. npos when there is none.
// npos too when *stop is set meanwhile.
size_t find_match(std::string_view hay, std::string_view needle, bool match_case, size_t from, bool forward,
                  const std::atomic<bool>* stop = nullptr);
// Every non-overlapping occurrence of needle in hay replaced by with (same rules as
// count_matches); *n is the number replaced.
std::string replace_all(std::string_view hay, std::string_view needle, std::string_view with, bool match_case,
                        size_t* n = nullptr);

// The Docs tab's preview: a small Markdown subset (# headings, paragraphs, - / 1. lists, > quotes,
// ``` fences, `code`, **bold**, *italic*, [text](https://link), ---) as HTML for wxHtmlWindow.
// Everything else is text: raw HTML is escaped (a cloned workspace's docs cannot inject markup),
// and only http(s) links become links.
std::string markdown_to_html(std::string_view md);

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

// Row descriptions (the Description column of Params and Headers): notes saved with the request
// and never sent. They travel in SavedRequest::docs as a trailing HTML comment, invisible when the
// Markdown is rendered and stripped from the Docs editor:
//   <!-- watt:descriptions
//   param<TAB>key<TAB>text        (a path variable's key is ":name")
//   header<TAB>Name<TAB>text
//   -->
// Fields escape backslash, tab, CR, LF and '>' as \ \t \r \n \g, so no text can end the comment.
//   pre<TAB>var<TAB>expression<TAB>scope<TAB>on|off   (a pre-request variable, http_editor_pre.h)
//   auth<TAB>field<TAB>value      (OAuth 2.0 settings: type, grant, token_url, client_id, ...)
// (SavedRequest has no field for the last two yet; they move to the model once it has.)
struct PreRow {
  std::string var, expr, scope;  // scope: "runtime" / "environment" / "globals"
  bool enabled = true;
  bool operator==(const PreRow&) const = default;
};
struct RowDocs {
  std::vector<std::pair<std::string, std::string>> params;
  std::vector<std::pair<std::string, std::string>> headers;
  std::vector<PreRow> pre;
  std::vector<std::pair<std::string, std::string>> auth;
  std::vector<std::pair<std::string, std::string>> asserts;  // assertion target -> description
  bool empty() const { return params.empty() && headers.empty() && pre.empty() && auth.empty() && asserts.empty(); }
  bool operator==(const RowDocs&) const = default;
};
// The docs as the user wrote them, and the descriptions found at their end (if any).
std::string split_row_docs(std::string_view docs, RowDocs& out);
// docs with the descriptions appended (docs unchanged when there are none).
std::string join_row_docs(std::string_view docs, const RowDocs& d);

// The cookies of a Cookie header value ("a=1; b=2"), in order; a part without '=' has an empty
// value.
std::vector<std::pair<std::string, std::string>> parse_cookie_header(std::string_view v);

// A curl import's URL without the "http://" curl's parser put in front of a URL that starts with
// a {{variable}} ("{{base}}/echo" -> "http://{{base}}/echo"): the variable holds the scheme, so
// the request would go to "http://http://host". Unchanged when the command itself wrote it.
std::string strip_curl_default_scheme(std::string_view url, std::string_view cmd);
// True when an expanded URL starts with two schemes ("http://https://host/..."), as a
// {{base}} that holds one behind a typed "http://" gives.
bool doubled_scheme(std::string_view url);

// GraphQL over HTTP (Postman / Insomnia / Bruno's GraphQL body): the JSON body
// {"query": "...", "variables": {...}} from the two editors. The variables text goes in as
// written (it may hold {{vars}} that make it JSON only once expanded); empty: no "variables".
std::string graphql_body(std::string_view query, std::string_view variables, std::string_view operation_name = {});
// The other way, for a saved body: true when it is a JSON object with a string "query" and at
// most "variables" (object or null) and "operationName" (string) besides; variables come back
// pretty printed (2 spaces), operation_name empty when there is none.
bool split_graphql_body(std::string_view body, std::string& query, std::string& variables,
                        std::string* operation_name = nullptr);

}  // namespace watt::app::http_editor
