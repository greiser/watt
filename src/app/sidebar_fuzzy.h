#pragma once
// Fuzzy matching for Ctrl+P quick open (VS Code / fzf style): every query character must appear
// in order; matches at word starts (after '/', ' ', '-', '_', '.', camelCase), consecutive runs
// and matches in the request name (after the last '/') score higher. Space separated query words
// are matched independently (all must match). ASCII case-insensitive; UTF-8 passes through.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace watt::app::sidebar {

class FuzzyQuery {
 public:
  explicit FuzzyQuery(std::string_view query);
  bool empty() const { return words_.empty(); }
  // target: what is shown ("folder/sub/name"); name_start: byte offset of the name part.
  // On a match returns true, *score (higher is better) and, if pos != null, the matched byte
  // offsets in ascending order (for highlighting).
  bool match(std::string_view target, size_t name_start, int* score, std::vector<uint16_t>* pos) const;
  const std::vector<std::string>& words() const { return words_; }

 private:
  std::vector<std::string> words_;  // lower-case
};

// A number of two or more digits in query word w (lower-case) must appear unbroken in target.
bool numbers_present(const std::string& w, std::string_view target);
// Each query word must be a case-insensitive substring of target (the sidebar filter box).
bool words_match(std::string_view query_lower, std::string_view target);
std::string ascii_lower(std::string_view s);
// A request URL as the filter searches it: a numeric host (127.0.0.1, [::1]) and any port are
// left out, so typing "7" or "80" finds requests named or pathed so, not every request on
// 127.0.0.1:8080. Named hosts and {{variables}} are kept ("api.github", "{{base}}").
std::string url_search_text(std::string_view url);
// Natural, case-insensitive order: digit runs compare by value ("endpoint 2" < "endpoint 10").
bool natural_less(std::string_view a, std::string_view b);

// The sidebar filter's context match. ctx is a request's "<folder path> <method>" (lower-case):
// the query (lower-case words) names only its method ("patch"), only its folder ("bulk 07"), or a
// folder and a method ("svc33 get", "orders delete": each word is the method or starts a word of
// the folder path, one of them the method).
bool context_only_match(std::string_view q, std::string_view ctx);
// Some word of q is the method of ctx (as above): a context hit came from the method.
bool names_method(std::string_view q, std::string_view ctx);

}  // namespace watt::app::sidebar
