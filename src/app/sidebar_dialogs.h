#pragma once
// Small modal dialogs drawn in the app's colours (native message boxes stay light in dark mode).
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

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

// Import from text: one box for a pasted collection (Postman, Insomnia, OpenAPI JSON / YAML,
// HAR), a curl command, or an http(s) URL to fetch one from. Prefilled from the clipboard when
// it holds something like that (up to 1 MB). False when cancelled or empty.
bool ask_import_text(wxWindow* parent, std::string* text);

// Folder settings: its name (renames the folder), how its items are ordered, and what is in it.
struct FolderSettings {
  std::string name;
  bool dragged_order = false;  // the folder keeps a dragged order (false: A-Z)
  bool has_dragged_order = false;  // there is a dragged order to keep (else A-Z only)
  wxString path;               // shown, copyable
  wxString contents;           // "12 requests (9 HTTP, 3 gRPC) in 2 folders"
  bool can_run = false;        // "Run folder" is offered (the host runs folders)
};
// What the user chose: wxID_OK (apply name / order), wxID_CANCEL, or kFolderRun (OK + run it).
inline constexpr int kFolderRun = 7001;
int folder_settings(wxWindow* parent, FolderSettings& s);

}  // namespace watt::app::sidebar

namespace watt::app::sidebar {

// The OS "Open" dialog (IFileOpenDialog), run on a thread of its own and modal to `owner` (the
// owner is disabled while it is open). The UI thread keeps painting meanwhile, and the dialog's
// teardown (30-40 ms of shell work on close) happens on that thread, not in a UI step. `filters`
// are (label, "*.json;*.yaml") pairs; `done` runs on the UI thread with the chosen file, or an
// empty path when cancelled. `folders`: pick a folder instead of a file.
void pick_file_async(wxWindow* owner, const std::wstring& title,
                     const std::vector<std::pair<std::wstring, std::wstring>>& filters, bool folders,
                     std::function<void(const std::filesystem::path&)> done,
                     const std::filesystem::path& start = {});
// The OS "Save as" dialog the same way (asks before overwriting; `name` is the suggested file
// name, `start` the folder it opens in).
void save_file_async(wxWindow* owner, const std::wstring& title,
                     const std::vector<std::pair<std::wstring, std::wstring>>& filters, const std::wstring& name,
                     std::function<void(const std::filesystem::path&)> done,
                     const std::filesystem::path& start = {});

}  // namespace watt::app::sidebar
