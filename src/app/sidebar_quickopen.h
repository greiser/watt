#pragma once
// Ctrl+P quick open: a VS Code style popup over every saved request with fuzzy matching and
// highlighted matches. Matching runs on the UI thread per keystroke (10k requests < 10 ms);
// the list is virtual, so only visible rows are drawn.
#include <filesystem>
#include <string>
#include <vector>

#include "core/model.h"

class wxWindow;

namespace watt::app::sidebar {

struct QuickItem {
  std::filesystem::path file;
  std::string name;    // request name (UTF-8)
  std::string folder;  // "folder/sub" relative to the workspace root ("" at the root)
  RequestKind kind = RequestKind::Http;
  std::string method;
  bool badge = false;
  int recent = -1;     // MRU rank (0 = most recent) or -1
};

// Modal; returns the chosen file or an empty path.
std::filesystem::path quick_open(wxWindow* parent, std::vector<QuickItem> items);

}  // namespace watt::app::sidebar
