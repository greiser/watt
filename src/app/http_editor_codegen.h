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

}  // namespace watt::app::http_editor
