#pragma once
// Visualize tab of the response view: Postman's pm.visualizer.set(template, data) rendered like
// Postman renders it (Handlebars), but here, in C++, on the body view's worker thread, into a
// static page shown with scripts off and every network request refused (response_body.cpp).
//
// Handlebars subset (what Postman visualizer templates use):
//   {{path}} (HTML-escaped), {{{path}}} / {{& path}} (raw), {{!comment}}, {{!-- comment --}},
//   \{{ (literal), whitespace control {{~ ~}}, paths a.b.[0].c, a/b, this, ., this.x, ../x,
//   @root, @index, @key, @first, @last, block params (#each xs as |x i|), string / number /
//   true / false / null literals, {{lookup obj key}}, {{#each}} (arrays and objects),
//   {{#if}} / {{#unless}} (Handlebars truthiness: false, null, "", 0 and [] are falsy),
//   {{else}} / {{^}} / {{else if x}} chains, {{#with}}, mustache sections {{#name}} / {{^name}}.
//   Partials, sub-expressions and custom helpers render nothing and are listed in `warnings`.
// No wx: unit tested in tests/test_response_asserts.cpp. Any thread.
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace watt::app::rv {

struct VisualizeResult {
  bool ok = false;
  std::string html;                   // the rendered template
  std::string error;                  // template syntax / data JSON error ("line 3: unclosed {{#each}}")
  std::vector<std::string> warnings;  // unsupported constructs (first few)
  bool truncated = false;             // the output reached max_out and was cut
};

// Renders `tpl` with the JSON `data_json` as its context. The output stops at max_out bytes.
VisualizeResult render_handlebars(std::string_view tpl, std::string_view data_json,
                                  size_t max_out = 1536u << 10);

// The page the Visualize tab loads: a Content-Security-Policy that allows inline styles and
// data: images only (no scripts, no fetches), a readable default style, then `body_html`.
// dark: the base style follows the app's dark theme (background, text, tables, links, inputs);
// the template's own styles still win.
std::string visualize_page(std::string_view body_html, bool dark = false);

}  // namespace watt::app::rv
