// Find / replace bar of the HTTP editor's text editors (see http_editor_find.h).
#include "app/http_editor_find.h"

#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/stc/stc.h>
#include <wx/textctrl.h>
#include <wx/timer.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "app/http_editor_util.h"
#include "app/http_editor_widgets.h"

namespace watt::app::http_editor {

namespace {

class FindBar;

// Cancel tokens of the running workers (UI thread only; each worker reads its own flag). A newer
// search, closing the bar or an edit of the document stops the older walks.
struct Stops {
  std::shared_ptr<std::atomic<bool>> find, count;
  static std::shared_ptr<std::atomic<bool>> renew(std::shared_ptr<std::atomic<bool>>& slot) {
    if (slot) slot->store(true);
    slot = std::make_shared<std::atomic<bool>>(false);
    return slot;
  }
  void all() {
    if (find) find->store(true);
    if (count) count->store(true);
  }
};

// Lifetime token for the counting worker (the bar may go away while it runs).
struct BarAlive {
  std::mutex m;
  FindBar* bar = nullptr;
};

class FindBar : public wxPanel {
 public:
  FindBar(wxStyledTextCtrl* stc, std::function<void(int, int)> reveal)
      : wxPanel(stc->GetParent()), stc_(stc), reveal_(std::move(reveal)), count_timer_(this) {
    const Theme& t = Theme::get();
    alive_->bar = this;
    SetBackgroundColour(t.bg);
    auto* col = new wxBoxSizer(wxVERTICAL);
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    find_ = field("Find  (Enter next, Shift+Enter previous)");
    row->Add(find_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    case_ = new CheckButton(this, "Aa");
    case_->SetToolTip("Match case");
    case_->on_change = [this] { term_changed(); };
    row->Add(case_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    count_ = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(96), -1), wxST_NO_AUTORESIZE);
    count_->SetForegroundColour(t.dim);
    row->Add(count_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    auto* prev = new FlatButton(this, wxString::FromUTF8("\xE2\x86\x91"), FlatButton::Style::Subtle);
    prev->SetToolTip("Previous match (Shift+Enter, Shift+F3)");
    prev->on_click = [this] { step(false); };
    auto* next = new FlatButton(this, wxString::FromUTF8("\xE2\x86\x93"), FlatButton::Style::Subtle);
    next->SetToolTip("Next match (Enter, F3)");
    next->on_click = [this] { step(true); };
    toggle_ = new FlatButton(this, "Replace", FlatButton::Style::Subtle);
    toggle_->SetToolTip("Show the replace row (Ctrl+H)");
    toggle_->on_click = [this] { show_replace(!replace_row_shown_); };
    auto* close = new FlatButton(this, wxString::FromUTF8("\xC3\x97"), FlatButton::Style::Subtle);
    close->SetToolTip("Close (Esc)");
    close->on_click = [this] { hide_bar(); };
    for (wxWindow* w : {static_cast<wxWindow*>(prev), static_cast<wxWindow*>(next), static_cast<wxWindow*>(toggle_)})
      row->Add(w, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(2));
    row->Add(close, 0, wxALIGN_CENTER_VERTICAL);
    col->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(4));
    repl_row_ = new wxBoxSizer(wxHORIZONTAL);
    repl_ = field("Replace with");
    repl_row_->Add(repl_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    auto* one = new FlatButton(this, "Replace", FlatButton::Style::Subtle);
    one->SetToolTip("Replace this match and go to the next (Enter in the replace box)");
    one->on_click = [this] { replace_one(); };
    auto* all = new FlatButton(this, "All", FlatButton::Style::Subtle);
    all->SetToolTip("Replace every match (one undo step)");
    all->on_click = [this] { replace_every(); };
    repl_row_->Add(one, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(2));
    repl_row_->Add(all, 0, wxALIGN_CENTER_VERTICAL);
    col->Add(repl_row_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(4));
    col->AddSpacer(FromDIP(4));
    SetSizer(col);
    col->Show(repl_row_, false);

    find_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { term_changed(); });
    find_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { step(!wxGetKeyState(WXK_SHIFT)); });
    repl_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { replace_one(); });
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
      const int k = e.GetKeyCode();
      const int m = e.GetModifiers();
      if (k == WXK_ESCAPE && m == 0) {
        hide_bar();
        return;
      }
      if (k == WXK_F3) {
        step(m != wxMOD_SHIFT);
        return;
      }
      if (m == wxMOD_CONTROL && (k == 'H' || k == 'h')) {
        show_replace(true);
        return;
      }
      if (m == wxMOD_CONTROL && (k == 'F' || k == 'f')) {
        find_->SetFocus();
        find_->SelectAll();
        return;
      }
      e.Skip();
    });
    count_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { start_count(); });
    // Edits of the document invalidate the snapshot the workers search (no `this`: the editor
    // may outlive the bar).
    stc_->Bind(wxEVT_STC_MODIFIED, [edits = edits_, stops = stops_](wxStyledTextEvent& e) {
      e.Skip();
      if (e.GetModificationType() & (wxSTC_MOD_INSERTTEXT | wxSTC_MOD_DELETETEXT)) {
        ++*edits;
        stops->all();
      }
    });
    Hide();
  }

  ~FindBar() override {
    stops_->all();
    std::lock_guard lk(alive_->m);
    alive_->bar = nullptr;
  }

  // Ctrl+F / Ctrl+H from the editor.
  void open(bool replace) {
    if (wxSizer* sz = stc_->GetContainingSizer(); sz && !placed_) {
      // Above the editor, in its own sizer (the editor shrinks by one row while the bar is shown).
      int at = 0;
      for (auto* item : sz->GetChildren()) {
        if (item->GetWindow() == stc_) break;
        ++at;
      }
      sz->Insert(static_cast<size_t>(at), this, 0, wxEXPAND);
      placed_ = true;
    }
    const wxString sel = stc_->GetSelectedText();
    if (!sel.empty() && sel.find('\n') == wxString::npos && sel.size() < 200) {
      find_->ChangeValue(sel);
      anchor_ = stc_->GetSelectionStart();
    } else {
      anchor_ = stc_->GetCurrentPos();
    }
    show_replace(replace);
    if (!IsShown()) {
      Show();
      GetParent()->Layout();
    }
    find_->SetFocus();
    find_->SelectAll();
    schedule_count();
  }

  void step(bool forward) {
    const std::string term = find_->GetValue().utf8_string();
    if (term.empty()) return;
    if (big()) {
      // Scintilla's search of a 10 MB document runs ~100-270 ms on the UI thread: a worker
      // searches a snapshot instead. Presses while it runs are queued, not dropped.
      if (finding_) {
        ++queued_;
        queued_forward_ = forward;
        return;
      }
      find_async(term, static_cast<size_t>(forward ? stc_->GetSelectionEnd() : stc_->GetSelectionStart()), forward,
                 true);
      return;
    }
    const int len = stc_->GetLength();
    int from = forward ? stc_->GetSelectionEnd() : stc_->GetSelectionStart();
    if (!search(term, from, forward ? len : 0)) {
      if (!search(term, forward ? 0 : len, forward ? len : 0)) {  // wrap around
        not_found();
        return;
      }
    }
    anchor_ = stc_->GetSelectionStart();
    schedule_count();
  }

 private:
  wxTextCtrl* field(const char* hint) {
    const Theme& t = Theme::get();
    auto* f = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE | wxTE_PROCESS_ENTER);
    f->SetBackgroundColour(t.field);
    f->SetForegroundColour(t.fg);
    f->SetHint(hint);
    return f;
  }

  int flags() const { return case_->value() ? wxSTC_FIND_MATCHCASE : 0; }

  // Selects the first match in [from, to) (backwards when to < from).
  bool search(const std::string& term, int from, int to) {
    stc_->SetSearchFlags(flags());
    stc_->SetTargetStart(from);
    stc_->SetTargetEnd(to);
    const int at = stc_->SearchInTarget(wxString::FromUTF8(term));
    if (at < 0) return false;
    const int end = stc_->GetTargetEnd();
    reveal(at, end);
    stc_->SetSelection(at, end);
    scroll_to_match(stc_, at, end);
    return true;
  }

  // Unfolds the lines of [s, e) so a match inside a folded region is shown, not selected hidden.
  void reveal(int s, int e) {
    const int l0 = stc_->LineFromPosition(s), l1 = stc_->LineFromPosition(e);
    bool hidden = false;
    for (int l = l0; l <= l1 && !hidden; ++l) hidden = !stc_->GetLineVisible(l);
    if (!hidden) return;
    if (reveal_) {
      reveal_(l0, l1);
      return;
    }
    for (int l = l0; l <= l1; ++l)
      if (!stc_->GetLineVisible(l)) stc_->EnsureVisible(l);
  }

  void not_found() {
    // The last prefix match ("item 9599" for "item 95999") must not stay selected: it is not a
    // match of what is typed now. The caret goes back to where the search started.
    if (stc_->GetSelectionStart() != stc_->GetSelectionEnd()) {
      const int a = std::clamp(anchor_, 0, stc_->GetLength());
      stc_->SetSelection(a, a);
    }
    count_->SetLabel("No results");
    count_->SetForegroundColour(Theme::get().js_error);
  }

  // Typing in the find box: the first match from where the search started (incremental).
  void term_changed() {
    const std::string term = find_->GetValue().utf8_string();
    if (term.empty()) {
      count_->SetLabel("");
      stc_->SetSelection(anchor_, anchor_);
      return;
    }
    if (big()) {
      queued_ = 0;
      find_async(term, static_cast<size_t>(anchor_), true, false);
      return;
    }
    const int len = stc_->GetLength();
    if (!search(term, anchor_, len) && !search(term, 0, len)) {
      not_found();
      return;
    }
    schedule_count();
  }

  bool big() const { return stc_->GetLength() >= 512 * 1024; }

  // The document as the workers read it, copied once per edit (a few ms for 10 MB).
  std::shared_ptr<const std::string> snapshot() {
    if (!snap_ || snap_edits_ != *edits_) {
      snap_ = std::make_shared<const std::string>(stc_->GetCharacterPointer(), static_cast<size_t>(stc_->GetLength()));
      snap_edits_ = *edits_;
    }
    return snap_;
  }

  // The match from `from` (with its "n of m") found on a worker; found() selects it.
  void find_async(const std::string& term, size_t from, bool forward, bool stepping) {
    auto text = snapshot();
    const bool mc = case_->value();
    const uint64_t gen = ++find_gen_;
    ++count_gen_;  // this search brings its own count
    count_timer_.Stop();
    finding_ = true;
    if (stops_->count) stops_->count->store(true);
    auto stop = Stops::renew(stops_->find);
    std::thread([alive = alive_, text, term, mc, from, forward, stepping, gen, edits = snap_edits_, stop] {
      size_t at = find_match(*text, term, mc, from, forward, stop.get());
      MatchCount m;
      if (at != std::string_view::npos) m = count_matches(*text, term, mc, at + 1, 1000000, stop.get());
      if (stop->load()) at = std::string_view::npos;  // superseded: found() drops it
      std::lock_guard lk(alive->m);
      if (FindBar* b = alive->bar)
        b->CallAfter([b, at, n = term.size(), m, stepping, gen, edits] { b->found(gen, edits, at, n, m, stepping); });
    }).detach();
  }

  void found(uint64_t gen, uint64_t edits, size_t at, size_t n, const MatchCount& m, bool stepping) {
    if (gen != find_gen_) return;  // a newer search is on its way
    finding_ = false;
    if (!IsShown()) return;
    const int len = stc_->GetLength();
    if (edits != *edits_ || (at != std::string_view::npos && at + n > static_cast<size_t>(len))) {
      if (!stepping) term_changed();  // edited meanwhile: search again
      return;
    }
    if (at == std::string_view::npos) {
      queued_ = 0;
      not_found();
      return;
    }
    const int s = static_cast<int>(at), e = static_cast<int>(at + n);
    reveal(s, e);
    stc_->SetSelection(s, e);
    scroll_to_match(stc_, s, e);
    if (stepping) anchor_ = s;
    show_count(m);
    if (queued_ > 0) {
      --queued_;
      step_again();
    }
  }

  void step_again() { step(queued_forward_); }

  void schedule_count() {
    // Small documents at once, big ones after a pause in typing.
    if (stc_->GetLength() < 512 * 1024) start_count();
    else count_timer_.StartOnce(150);
  }

  void start_count() {
    const std::string term = find_->GetValue().utf8_string();
    if (term.empty()) return;
    const size_t at = static_cast<size_t>(stc_->GetSelectionStart());
    const size_t len = static_cast<size_t>(stc_->GetLength());
    const bool mc = case_->value();
    const uint64_t gen = ++count_gen_;
    if (len < 512 * 1024) {
      show_count(count_matches(std::string_view(stc_->GetCharacterPointer(), len), term, mc, at + 1));
      return;
    }
    // A snapshot so the worker never reads the live buffer.
    auto text = snapshot();
    auto stop = Stops::renew(stops_->count);
    std::thread([alive = alive_, text, term, mc, at, gen, stop] {
      const MatchCount m = count_matches(*text, term, mc, at + 1, 1000000, stop.get());
      if (m.cancelled || stop->load()) return;
      std::lock_guard lk(alive->m);
      if (FindBar* b = alive->bar)
        b->CallAfter([b, m, gen] {
          if (gen == b->count_gen_) b->show_count(m);
        });
    }).detach();
  }

  void show_count(const MatchCount& m) {
    count_->SetForegroundColour(m.total ? Theme::get().dim : Theme::get().js_error);
    if (!m.total) count_->SetLabel("No results");
    else count_->SetLabel(wxString::Format("%zu of %zu%s", std::max<size_t>(m.before, 1), m.total, m.capped ? "+" : ""));
  }

  void show_replace(bool on) {
    if (on == replace_row_shown_) {
      if (on) repl_->SetFocus();
      return;
    }
    replace_row_shown_ = on;
    GetSizer()->Show(repl_row_, on);
    toggle_->SetLabel(on ? "Find only" : "Replace");
    Layout();
    GetParent()->Layout();
    if (on && IsShown()) repl_->SetFocus();
  }

  void replace_one() {
    if (stc_->GetReadOnly()) return;
    const std::string term = find_->GetValue().utf8_string();
    if (term.empty()) return;
    const std::string with = repl_->GetValue().utf8_string();
    // The selection is a match: replace it, then go on to the next one.
    const int s = stc_->GetSelectionStart(), e = stc_->GetSelectionEnd();
    const wxCharBuffer selb = stc_->GetTextRangeRaw(s, e);
    const std::string sel(selb.data(), selb.length());
    const bool match = case_->value() ? sel == term : iequals(sel, term);
    if (match && e > s) {
      stc_->SetTargetStart(s);
      stc_->SetTargetEnd(e);
      stc_->ReplaceTargetRaw(with.data(), static_cast<int>(with.size()));
      stc_->SetSelection(s + static_cast<int>(with.size()), s + static_cast<int>(with.size()));
    }
    step(true);
  }

  void replace_every() {
    if (stc_->GetReadOnly()) return;
    const std::string term = find_->GetValue().utf8_string();
    if (term.empty()) return;
    const std::string with = repl_->GetValue().utf8_string();
    const size_t len = static_cast<size_t>(stc_->GetLength());
    size_t n = 0;
    const std::string out = replace_all(std::string_view(stc_->GetCharacterPointer(), len), term, with, case_->value(), &n);
    if (n == 0) {
      not_found();
      return;
    }
    const int pos = stc_->GetCurrentPos();
    stc_->BeginUndoAction();
    stc_->SetTargetStart(0);
    stc_->SetTargetEnd(static_cast<int>(len));
    stc_->ReplaceTargetRaw(out.data(), static_cast<int>(out.size()));
    stc_->EndUndoAction();
    stc_->GotoPos(std::min(pos, stc_->GetLength()));
    count_->SetForegroundColour(Theme::get().dim);
    count_->SetLabel(wxString::Format("Replaced %zu", n));
  }

  void hide_bar() {
    stops_->all();
    ++count_gen_;
    ++find_gen_;
    finding_ = false;
    queued_ = 0;
    snap_.reset();  // 10 MB less while the bar is closed
    count_timer_.Stop();
    Hide();
    GetParent()->Layout();
    stc_->SetFocus();
  }

  wxStyledTextCtrl* stc_;
  std::function<void(int, int)> reveal_;
  std::shared_ptr<BarAlive> alive_ = std::make_shared<BarAlive>();
  std::shared_ptr<Stops> stops_ = std::make_shared<Stops>();
  wxTextCtrl* find_ = nullptr;
  wxTextCtrl* repl_ = nullptr;
  CheckButton* case_ = nullptr;
  wxStaticText* count_ = nullptr;
  FlatButton* toggle_ = nullptr;
  wxBoxSizer* repl_row_ = nullptr;
  bool replace_row_shown_ = false;
  bool placed_ = false;
  int anchor_ = 0;
  uint64_t count_gen_ = 0;
  uint64_t find_gen_ = 0;
  bool finding_ = false;
  int queued_ = 0;  // Enter / F3 presses while a search runs
  bool queued_forward_ = true;
  std::shared_ptr<std::atomic<uint64_t>> edits_ = std::make_shared<std::atomic<uint64_t>>(0);
  std::shared_ptr<const std::string> snap_;
  uint64_t snap_edits_ = 0;
  wxTimer count_timer_;
};

}  // namespace

void scroll_to_match(wxStyledTextCtrl* stc, int from, int to) {
  const int line = stc->LineFromPosition(from);
  const int vis = stc->VisibleFromDocLine(line);
  const int first = stc->GetFirstVisibleLine(), n = std::max(stc->LinesOnScreen(), 1);
  const int margin = std::min(3, n / 4);
  if (vis < first + margin || vis > first + n - 1 - margin) stc->SetFirstVisibleLine(std::max(0, vis - n / 2));
  stc->ScrollRange(to, from);  // horizontally
}

void attach_find(wxStyledTextCtrl* stc, std::function<void(int first, int last)> reveal) {
  // Scintilla's own Ctrl+F / Ctrl+H do nothing useful here (Ctrl+H would type a backspace).
  stc->CmdKeyClear('F', wxSTC_KEYMOD_CTRL);
  stc->CmdKeyClear('H', wxSTC_KEYMOD_CTRL);
  auto bar = std::make_shared<FindBar*>(nullptr);
  stc->Bind(wxEVT_CHAR_HOOK, [stc, bar, reveal = std::move(reveal)](wxKeyEvent& e) {
    const int k = e.GetKeyCode();
    const int m = e.GetModifiers();
    const bool find = m == wxMOD_CONTROL && (k == 'F' || k == 'f');
    const bool repl = m == wxMOD_CONTROL && (k == 'H' || k == 'h');
    const bool f3 = k == WXK_F3 && (m == 0 || m == wxMOD_SHIFT);
    if (!find && !repl && !f3) {
      e.Skip();
      return;
    }
    if (!*bar) *bar = new FindBar(stc, reveal);
    if (f3 && (*bar)->IsShown()) (*bar)->step(m == 0);
    else (*bar)->open(repl);
  });
}

}  // namespace watt::app::http_editor
