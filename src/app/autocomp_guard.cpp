#include "autocomp_guard.h"

#include <wx/stc/stc.h>
#include <wx/timer.h>
#include <wx/weakref.h>

#include <memory>
#include <unordered_map>

namespace watt::app {

namespace {

bool lost(wxStyledTextCtrl* s) { return !s->IsShownOnScreen() || wxWindow::FindFocus() != s; }

class Watch : public wxTimer {
 public:
  explicit Watch(wxStyledTextCtrl* s) : stc_(s) {}
  void Notify() override {
    wxStyledTextCtrl* s = stc_.get();
    if (!s || !s->AutoCompActive()) {
      Stop();
      return;
    }
    if (lost(s)) {
      s->AutoCompCancel();
      Stop();
    }
  }

 private:
  wxWeakRef<wxStyledTextCtrl> stc_;
};

// One watch per field (UI thread only), dropped when the field is destroyed.
std::unordered_map<wxStyledTextCtrl*, std::unique_ptr<Watch>>& watches() {
  static auto* m = new std::unordered_map<wxStyledTextCtrl*, std::unique_ptr<Watch>>();
  return *m;
}

}  // namespace

void guard_autocomp(wxStyledTextCtrl* s) {
  if (!s) return;
  auto& m = watches();
  auto it = m.find(s);
  if (it == m.end()) {
    it = m.emplace(s, std::make_unique<Watch>(s)).first;
    // Focus left the field: close at once (after the focus change settles, so a click on the
    // list itself, which hands focus straight back, keeps it open).
    s->Bind(wxEVT_KILL_FOCUS, [s](wxFocusEvent& e) {
      e.Skip();
      s->CallAfter([s] {
        if (s->AutoCompActive() && lost(s)) s->AutoCompCancel();
      });
    });
    s->Bind(wxEVT_DESTROY, [s](wxWindowDestroyEvent& e) {
      e.Skip();
      if (e.GetEventObject() == s) watches().erase(s);
    });
  }
  if (!it->second->IsRunning()) it->second->Start(100);
}

}  // namespace watt::app
