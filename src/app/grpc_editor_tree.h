#pragma once
// Searchable service / method tree for the gRPC editor: a filter box over a tree of services
// with their methods, each method carrying a call-kind badge (UNARY / SERVER / CLIENT / BIDI).
#include <wx/panel.h>

#include <functional>
#include <string>
#include <vector>

#include "core/grpc_client.h"

class wxTextCtrl;
class wxTreeCtrl;
class wxTreeEvent;
class wxStaticText;

namespace watt::app::grpced {

class MethodTree : public wxPanel {
 public:
  explicit MethodTree(wxWindow* parent);

  // A method was picked: chosen = true for Enter / double-click (done picking), false while
  // browsing (single click, arrow keys in the tree).
  std::function<void(const std::string& full_path, bool chosen)> on_pick;

  void set_methods(const std::vector<GrpcMethodInfo>& methods);
  // Highlight full_path without firing on_pick (empty clears the selection).
  void select(const std::string& full_path);
  void focus_filter();
  // Shown instead of the tree when there are no methods (e.g. "Load a schema ...").
  void set_empty_text(const wxString& text);
  // Greyed out: the methods of the last good schema while a reload failed or is running.
  void set_stale(bool stale);
  // gRPC-Web: client and bidi streaming methods are drawn greyed with a note (they cannot run).
  void set_web(bool web);
  // Empties the filter (the pane was hidden after a pick: reopening it shows every method).
  void clear_filter();
  size_t method_count() const { return entries_.size(); }

 private:
  struct Entry {
    std::string service, method, full_path, search;  // search: lower-case full path
    GrpcCallKind kind;
  };
  void rebuild();
  void pick_selected(bool chosen);
  // Moves the highlight by delta methods (skipping service rows) without picking.
  void move_highlight(int delta);
  void wrap_empty();
  void on_sel(wxTreeEvent& e);

  wxTextCtrl* filter_;
  wxTreeCtrl* tree_;
  wxStaticText* empty_;
  std::vector<Entry> entries_;
  std::string selected_;
  bool quiet_ = false;
  bool stale_ = false;
  bool web_ = false;
  wxString empty_text_, empty_raw_;
};

}  // namespace watt::app::grpced
