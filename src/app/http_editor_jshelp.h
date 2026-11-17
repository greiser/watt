#pragma once
// Help for the request script editors (pure, no wx): member completion for the pm. / bru. /
// req. / res. APIs of core/script.h, the snippet list (Postman's "Snippets", Insomnia's snippet
// menu), and a quick syntax check (brackets, strings, template literals, comments, regex
// literals) shown while typing and before a send, so a missing '}' is seen without a round trip
// through the sandbox.
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace watt::app::http_editor {

// The dotted identifier chain that ends right before `pos` ("pm.response" for "x = pm.response"
// with pos at its end); empty when the text before pos is not an identifier chain.
std::string js_chain_before(std::string_view text, size_t pos);

// Members to offer after "<chain>." ("" = the top-level names); sorted, empty when the chain is
// unknown. `post`: the post-response script (res / pm.response exist only there).
std::vector<std::string> js_members(std::string_view chain, bool post);

struct JsSnippet {
  const char* title;
  const char* code;  // inserted as is, ends with a newline
};
const std::vector<JsSnippet>& js_snippets(bool post);

struct JsIssue {
  size_t pos = 0;  // byte offset of the problem
  std::string message;
};
// The first structural error of a script, if any. Never a false alarm on valid JavaScript that
// uses only ordinary syntax; it does not check more than structure.
std::optional<JsIssue> js_lint(std::string_view src);

}  // namespace watt::app::http_editor
