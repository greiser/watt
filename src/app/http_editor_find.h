#pragma once
// Find / replace bar for the HTTP editor's text editors (body, scripts, docs), like the Monaco /
// CodeMirror search of Postman, Insomnia and Bruno: Ctrl+F opens it (with the selection as the
// term), Ctrl+H with the replace row, Enter / F3 next, Shift+Enter / Shift+F3 previous, Esc
// closes it and returns to the text. It sits above the editor (in the editor's sizer).
//
// Performance on 10 MB bodies: finding the next match is Scintilla's own search from the caret
// (it stops at the first hit); the "3 of 120" count runs on a worker over a copy of the text,
// debounced while typing, so no keystroke in the bar waits on a scan of the whole document.
#include <functional>

class wxStyledTextCtrl;

namespace watt::app::http_editor {

// Adds Ctrl+F / Ctrl+H / F3 / Shift+F3 to stc. The bar is created on first use. A match inside
// a folded block is unfolded first: by `reveal` (lines first..last) when given, which may do it
// over several frames and scroll to the selection afterwards, else by Scintilla's EnsureVisible.
void attach_find(wxStyledTextCtrl* stc, std::function<void(int first, int last)> reveal = {});

// Scrolls so the selected match [from, to) shows: a match already well inside the view stays
// put; otherwise its line goes to the middle of the view (as Monaco / CodeMirror do), not to the
// last visible line.
void scroll_to_match(wxStyledTextCtrl* stc, int from, int to);

}  // namespace watt::app::http_editor
