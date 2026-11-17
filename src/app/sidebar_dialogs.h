#pragma once
// Small modal dialogs drawn in the app's colours (native message boxes stay light in dark mode).
#include <string>

#include <wx/string.h>

class wxWindow;

namespace watt::app::sidebar {

// A question with a primary action button (Enter) and Cancel (Esc). `danger` paints the action
// red. True when the action was chosen.
bool confirm(wxWindow* parent, const wxString& title, const wxString& message, const wxString& action,
             bool danger = false);

// A notice with an OK button.
void notify(wxWindow* parent, const wxString& title, const wxString& message);

// Import a curl command: a multi-line box (prefilled from the clipboard when it holds one) and
// an optional name. False when cancelled.
bool ask_curl(wxWindow* parent, std::string* command, std::string* name);

}  // namespace watt::app::sidebar
