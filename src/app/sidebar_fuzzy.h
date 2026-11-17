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

 private:
  std::vector<std::string> words_;  // lower-case
};

// Each query word must be a case-insensitive substring of target (the sidebar filter box).
bool words_match(std::string_view query_lower, std::string_view target);
std::string ascii_lower(std::string_view s);
// Natural, case-insensitive order: digit runs compare by value ("endpoint 2" < "endpoint 10").
bool natural_less(std::string_view a, std::string_view b);

}  // namespace watt::app::sidebar
