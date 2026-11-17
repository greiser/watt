#pragma once
// Response body filter (Insomnia's and Bruno's JSONPath box under the response): a JSONPath
// query evaluated over the JSON body on the body view's worker thread. Pure, no wx.
//
//   $ (or res.body)   the whole body
//   .key  ['key']     a member            [2]  [-1]   an element (negative: from the end)
//   [1,3]             several elements    [1:4] [:2] [-2:]   a slice
//   .*  [*]           every member / element
//   ..key  ..*        every member named key / every value, at any depth below
//   .length           the size of an array or object, the length of a string (when no member
//                     of that name exists)
//   [?(expr)]         the elements (members) for which expr holds, with jsonpath-plus semantics
//                     (what Insomnia uses): @ is the element, $ the body; == != < <= > >= (also
//                     === / !==), =~ /re/i (RE2: linear time), && || ! ( ); a lone path is true
//                     when its value is truthy. $..[?(...)] filters every container below.
//
// Matches keep the response's own number text (0.00 stays 0.00, 1.50 stays 1.50).
//
// Safety: the body may come from anywhere. Parsing is iterative (no stack growth on deep input),
// every walk uses an explicit stack, matches are capped at kMaxMatches and output at max_out bytes,
// and nothing throws.
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace watt::app::rv {

struct FilterResult {
  bool ok = false;
  std::string error;     // why not (bad expression, body not JSON, nothing matched is not an error)
  std::string json;      // one match itself, several as a JSON array; pretty, 2-space indent
  size_t matches = 0;
  bool truncated = false;  // more than kMaxMatches matches, or the output was cut at max_out
};

inline constexpr size_t kMaxFilterMatches = 200000;

FilterResult json_filter(std::string_view body, std::string_view expr, size_t max_out = 64u << 20);

// Checks an expression without a body: "" when it parses, else why not (for the filter box).
std::string json_filter_check(std::string_view expr);

}  // namespace watt::app::rv

namespace watt::app::rv {

// Table view of a JSON value (Postman's Visualize without a script): an array of objects as rows
// with one column per member (in first-seen order), an array of scalars as one column, an object
// without such an array as key / value rows. Built on a worker; pure, no wx.
struct TableModel {
  std::string source;                      // which part of the body: "$", "$.items"
  std::vector<std::string> cols;           // without the leading "#" column
  std::vector<std::vector<std::string>> cells;  // [row][col]; nested values as compact JSON
  bool truncated = false;                  // rows or columns were capped
  std::string error;                       // not JSON / nothing tabular
};

inline constexpr size_t kMaxTableRows = 500000;
inline constexpr size_t kMaxTableCols = 64;

// json: the text to show (the body, or a filter's result). prefer_path: "$" when the caller
// already chose the part (a filter result), "" to find the largest array of objects.
TableModel json_table(std::string_view json, const std::string& prefer_path = {});

// Compares two cells for sorting: numbers numerically, otherwise text (case-insensitive ASCII).
bool table_cell_less(const std::string& a, const std::string& b);

}  // namespace watt::app::rv
