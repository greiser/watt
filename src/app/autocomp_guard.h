#pragma once
// Keeps a Scintilla autocompletion list from outliving its field. wxSTC shows the list in its own
// popup window, and nothing closes that popup when the field loses focus, its tab is switched
// away, parked or recycled: the list then floats over whatever is shown next. Call this right
// after every AutoCompShow: the list is canceled as soon as focus leaves the field or the field
// is no longer on screen (a cheap 100 ms check that runs only while a list is open).
class wxStyledTextCtrl;

namespace watt::app {

void guard_autocomp(wxStyledTextCtrl* stc);

}  // namespace watt::app
