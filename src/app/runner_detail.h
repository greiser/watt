#pragma once
// Detail pane of the collection runner for one step (Bruno's Response / Headers / Timeline /
// Tests tabs): a header (request, status, time, URL, error), then tabs
//   Response  the body (pretty JSON, wrap, find, copy, save) in the response view's BodyView
//   Headers   response headers (gRPC: initial metadata and trailers), selectable text
//   Request   method, URL, headers and body as sent (variables expanded), selectable text
//   Timing    the phase waterfall (script phases included)
//   Tests     assertions, then script tests (test() / pm.test) and a script's uncaught error
//   Variables captured values, pre-request variables and variables set by scripts
//   Console   the scripts' console output
// Everything is built from the StepResult the engine kept (bodies up to runner::kBodyKeep), so
// nothing here scales with the original response size. UI thread only.
#include <wx/event.h>
#include <wx/panel.h>

#include <functional>
#include <memory>
#include <set>
#include <string>

#include "app/runner_engine.h"

namespace watt::app::runner_ui {

class DetailPane : public wxPanel {
 public:
  explicit DetailPane(wxWindow* parent);
  ~DetailPane() override;
  // s null: shows hint instead.
  void show(std::shared_ptr<const runner::StepResult> s, const std::string& hint);
  const std::shared_ptr<const runner::StepResult>& shown() const { return s_; }
  // "Open in tab" (also double click / Enter on a result row).
  std::function<void()> on_open;
  void set_can_open(bool on);
  // "Run again" on a restored run's Response tab (its bodies were not kept).
  std::function<void()> on_run_again;
  // While a run streams in and the selection follows it: big response bodies wait for a click.
  // rebuild: build a body deferred so far now (when on turns false).
  void set_defer_big(bool on, bool rebuild = true);
  // Creates the body viewer now (it is otherwise created on first use).
  void warm_up();
  // Window-wide keys (Ctrl+Enter, Esc): offered first by every control in the pane, since
  // controls that want all keys (lists, editors) keep some from the frame's char hook.
  std::function<bool(wxKeyEvent&)> on_key;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;  // its controls are children of this panel
  std::shared_ptr<const runner::StepResult> s_;
};

// "1.23 ms", "456 ms", "1.20 s"
std::string fmt_ms(double v);
// "512 B", "1.5 KB", "9.9 MB"
std::string fmt_bytes(uint64_t n);
wxColour mix(const wxColour& a, const wxColour& b, double t);
wxColour status_colour(const runner::StepResult& r);
std::string status_label(const runner::StepResult& r);
// Binds KEY_DOWN on w and every window below it: f first, the control's own handling when f
// returns false. Windows in *seen are skipped (and new ones added).
void hook_keys(wxWindow* w, const std::function<bool(wxKeyEvent&)>& f, std::set<wxWindow*>* seen);

// Method colours as the request tabs draw them (GET green, POST orange, ...; gRPC teal).
wxColour method_colour(const std::string& method, bool grpc);

}  // namespace watt::app::runner_ui
