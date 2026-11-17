#pragma once
// Find / replace bar for the HTTP editor's text editors (body, scripts, docs), like the Monaco /
// CodeMirror search of Postman, Insomnia and Bruno: Ctrl+F opens it (with the selection as the
// term), Ctrl+H with the replace row, Enter / F3 next, Shift+Enter / Shift+F3 previous, Esc
// closes it and returns to the text. It sits above the editor (in the editor's sizer).
//
// Performance on 10 MB bodies: finding the next match is Scintilla's own search from the caret
// (it stops at the first hit); the "3 of 120" count runs on a worker over a copy of the text,
// debounced while typing, so no keystroke in the bar waits on a scan of the whole document.
class wxStyledTextCtrl;

namespace watt::app::http_editor {

// Adds Ctrl+F / Ctrl+H / F3 / Shift+F3 to stc. The bar is created on first use.
void attach_find(wxStyledTextCtrl* stc);

}  // namespace watt::app::http_editor
