#pragma once
// Folder / collection settings dialog of the HTTP editor ("Edit on folder" on the Auth tab):
// the auth that requests set to "Inherit from parent" get, headers sent with every request below
// the folder, folder variables and pre-request / post-response scripts (core FolderSettings,
// <folder>/folder.json; the workspace root is the collection). Themed like the editor.
#include <filesystem>
#include <functional>

class wxWindow;

namespace watt {
class Workspace;
}

namespace watt::app::http_editor {

// Reads the settings of `folder` on a worker thread, then shows the dialog (modal) on the UI
// thread; Save writes them on a worker and calls on_saved on the UI thread once written. Nothing
// here blocks the UI thread on the disk. folder must be the workspace root or a folder inside it
// (core refuses anything else, and the dialog then says why).
void edit_folder_settings(wxWindow* parent, const Workspace& ws, const std::filesystem::path& folder,
                          std::function<void()> on_saved);

}  // namespace watt::app::http_editor
