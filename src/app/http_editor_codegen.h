#pragma once
// "Copy as code" for the HTTP editor beyond curl: Python (requests), JavaScript (fetch), HTTPie,
// Node.js (axios), Go (net/http), Java (java.net.http), C# (HttpClient) and PowerShell
// (Invoke-RestMethod), generated from the request as it would be sent (disabled headers left out). Pure
// functions, no wx.
#include <string>

#include "core/model.h"

namespace watt::app::http_editor {

enum class CodeLang { PythonRequests, JavaScriptFetch, HTTPie, NodeAxios, Go, Java, CSharp, PowerShell };
constexpr int kCodeLangCount = 8;

const char* code_lang_name(CodeLang lang);
std::string to_code(const SavedRequest& r, CodeLang lang);
// A resolved request (Workspace::resolved / expanded) as an export should carry it: Basic auth is
// dropped when an enabled Authorization header is sent anyway (it wins on the wire, see
// Workspace::http_to_send), so curl / code never carries both -H Authorization and -u.
void drop_shadowed_basic(SavedRequest& r);

}  // namespace watt::app::http_editor
