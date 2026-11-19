#pragma once
// The sidebar's pinned sections under the collection tree (Bruno's "API Specs" and "Mock
// Servers"): a 22 px header styled like COLLECTION (bold dim caps, a count, a "+") that collapses
// the section to itself, and a few dense custom-drawn rows (a status dot, a name, dim detail and
// a chip at the right edge). Also the read-only API spec preview tab. UI thread only; the spec
// preview reads and parses its file on a worker.
#include <wx/window.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class wxAuiNotebook;

namespace watt::app::sidebar {

struct SectionRow {
  std::string label;
  std::string detail;  // dimmed, after the label (ellipsized first)
  std::string chip;    // a small rounded tag at the right edge (":18431", "3.0.3")
  wxColour dot;        // a status dot before the label; !IsOk(): none
  std::string tip;
  bool dim = false;    // drawn dimmed (a spec whose file is gone)
};

class SectionPane : public wxWindow {
 public:
  static constexpr size_t npos = static_cast<size_t>(-1);
  SectionPane(wxWindow* parent, const wxString& title, const wxString& add_tip, const wxString& empty_text);

  void set_rows(std::vector<SectionRow> rows);
  const std::vector<SectionRow>& rows() const { return rows_; }
  void set_collapsed(bool c);
  bool collapsed() const { return collapsed_; }
  // At most this many rows show at once (the rest scroll with the wheel).
  void set_max_rows(int n);

  std::function<void(size_t row)> on_click;
  std::function<void(size_t row)> on_dclick;
  // Right-click (or the context-menu key) on a row, or npos on the header / empty area.
  std::function<void(size_t row)> on_menu;
  // "+": `at` is the "+"'s bottom-left corner in this window's coordinates (for a popup menu).
  std::function<void(const wxPoint& at)> on_add;
  // The header was clicked: collapsed() is already the new state (save it, lay out the parent).
  std::function<void()> on_toggle;

 protected:
  wxSize DoGetBestClientSize() const override;

 private:
  void paint();
  int header_h() const;
  int row_h() const;
  int visible_rows() const;
  size_t row_at(const wxPoint& p) const;
  bool on_plus(const wxPoint& p) const;
  void relayout();

  wxString title_, add_tip_, empty_text_;
  std::vector<SectionRow> rows_;
  bool collapsed_ = false;
  int max_rows_ = 4;
  int top_ = 0;  // first row shown (wheel scroll)
  size_t hot_ = npos;
  bool plus_hot_ = false;
  bool head_hot_ = false;
};

// A read-only preview tab of an OpenAPI / Swagger file in nb (added unselected; the caller
// selects the returned page in its next UI step), or the one already open for it: the
// document's title, version and operations (method, path, summary; read and parsed on a
// worker) next to its text. sync: "Sync requests" (re-runs the import into its folder); target:
// where they go ("Requests in api/Pet Store"), shown under the title. reveal: Explorer.
wxWindow* open_spec_preview(wxAuiNotebook* nb, const std::filesystem::path& file, const std::string& target,
                       std::function<void()> sync, std::function<void()> reveal);
// The preview tab of file (nullptr: none open) and a refresh of its target line.
wxWindow* spec_preview_for(const std::filesystem::path& file);
void spec_preview_set_target(wxWindow* preview, const std::string& target);
// True for a spec preview tab (it is not a request tab).
bool is_spec_preview(wxWindow* w);
// A double-click (or Enter) on an operation row calls fn(method, path), the path as the document
// writes it ("/pets/{petId}"), to open the request synced from it.
void spec_preview_set_open_op(wxWindow* preview, std::function<void(const std::string&, const std::string&)> fn);

}  // namespace watt::app::sidebar
