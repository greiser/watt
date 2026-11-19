#include "app/sidebar_quickopen.h"

#include <chrono>

#include <wx/dc.h>
#include <wx/dialog.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/vlbox.h>

#include <algorithm>
#include <cctype>
#include <numeric>

#include "app/sidebar_fuzzy.h"
#include "app/sidebar_style.h"

namespace watt::app::sidebar {

namespace {

struct Row {
  size_t item;
  int score;
  std::vector<uint16_t> pos;  // byte offsets into the item's target ("folder/name")
};

bool is_method_word(const std::string& w, RequestKind* kind, std::string* method) {
  static const char* http[] = {"get", "post", "put", "patch", "delete", "head", "options"};
  for (const char* m : http)
    if (w == m) {
      *kind = RequestKind::Http;
      *method = ascii_lower(m);
      return true;
    }
  if (w == "del" || w == "opt") {  // the badge abbreviations
    *kind = RequestKind::Http;
    *method = w == "del" ? "delete" : "options";
    return true;
  }
  if (w == "grpc") {
    *kind = RequestKind::Grpc;
    method->clear();
    return true;
  }
  return false;
}

// Draws utf8[b, e) at x with the bytes listed in `pos` highlighted; clips at max_x with an
// ellipsis. Returns the x after the text.
int draw_marked(wxDC& dc, const std::string& s, size_t b, size_t e, const std::vector<uint16_t>& pos, int x, int y,
                int max_x, const wxFont& normal, const wxFont& bold, const wxColour& fg, const wxColour& hl) {
  size_t i = b;
  auto marked = [&](size_t k) { return std::binary_search(pos.begin(), pos.end(), static_cast<uint16_t>(k)); };
  while (i < e) {
    const bool m = marked(i);
    size_t j = i + 1;
    while (j < e && (marked(j) == m || (static_cast<unsigned char>(s[j]) & 0xC0) == 0x80)) ++j;
    const wxString part = wxString::FromUTF8(s.data() + i, j - i);
    dc.SetFont(m ? bold : normal);
    dc.SetTextForeground(m ? hl : fg);
    const int w = dc.GetTextExtent(part).x;
    if (x + w > max_x) {
      // Fit what we can, then an ellipsis.
      const wxString ell = wxString::FromUTF8("\xE2\x80\xA6");
      const int ew = dc.GetTextExtent(ell).x;
      wxString fit = part;
      while (!fit.empty() && x + dc.GetTextExtent(fit).x + ew > max_x) fit.RemoveLast();
      dc.DrawText(fit, x, y);
      x += dc.GetTextExtent(fit).x;
      dc.SetFont(normal);
      dc.SetTextForeground(fg);
      dc.DrawText(ell, x, y);
      return x + ew;
    }
    dc.DrawText(part, x, y);
    x += w;
    i = j;
  }
  return x;
}

class ResultList : public wxVListBox {
 public:
  ResultList(wxWindow* parent, const std::vector<QuickItem>& items, const std::vector<std::string>& targets,
             const std::vector<Row>& rows)
      : wxVListBox(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxLB_SINGLE),
        items_(items), targets_(targets), rows_(rows) {
    SetBackgroundColour(window_colour());
  }

 protected:
  wxCoord OnMeasureItem(size_t) const override { return FromDIP(30); }

  void OnDrawBackground(wxDC& dc, const wxRect& rect, size_t n) const override {
    if (IsSelected(n)) {
      dc.SetBrush(wxBrush(highlight_colour()));
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.DrawRectangle(rect);
    }
  }

  void OnDrawItem(wxDC& dc, const wxRect& rect, size_t n) const override {
    const Row& r = rows_[n];
    const QuickItem& it = items_[r.item];
    const std::string& t = targets_[r.item];
    const int pad = FromDIP(12);
    const int lh = dc.GetCharHeight();
    const int y = rect.y + (rect.height - lh) / 2;
    if (it.badge || it.kind == RequestKind::Grpc) draw_badge(dc, this, it.kind, it.method, rect.x + pad, rect.y, rect.height);
    int x = rect.x + pad + FromDIP(44);
    int right = rect.GetRight() - pad;
    wxFont normal = GetFont();
    // The URL at the right (requests that differ only by URL), up to 40% of the row, ellipsized
    // at its start: the path end is what tells them apart.
    if (!it.url.empty()) {
      wxFont small = normal;
      small.SetFractionalPointSize(normal.GetFractionalPointSize() * 0.92);
      dc.SetFont(small);
      wxString u = wxString::FromUTF8(it.url);
      const int max_w = (right - x) * 2 / 5;
      if (dc.GetTextExtent(u).x > max_w) {
        const wxString ell = wxString::FromUTF8("\xE2\x80\xA6");
        size_t lo = 1, hi = u.length();  // the fewest leading chars to drop (binary search)
        while (lo < hi) {
          const size_t mid = (lo + hi) / 2;
          if (dc.GetTextExtent(ell + u.Mid(mid)).x > max_w) lo = mid + 1;
          else hi = mid;
        }
        u = ell + u.Mid(lo);
      }
      const int uw = dc.GetTextExtent(u).x;
      dc.SetTextForeground(dim_colour());
      dc.DrawText(u, right - uw, rect.y + (rect.height - dc.GetCharHeight()) / 2);
      right -= uw + FromDIP(16);
    }
    dc.SetFont(normal);
    wxFont bold = normal.Bold();
    const wxColour hl = is_dark() ? wxColour(0x6C, 0xB6, 0xFF) : wxColour(0x0B, 0x5C, 0xB8);
    const size_t ns = it.folder.empty() ? 0 : it.folder.size() + 1;
    x = draw_marked(dc, t, ns, t.size(), r.pos, x, y, right, normal, bold, text_colour(), hl);
    if (!it.folder.empty() && x + FromDIP(30) < right) {
      wxFont small = normal;
      small.SetFractionalPointSize(normal.GetFractionalPointSize() * 0.92);
      wxFont small_bold = small.Bold();
      dc.SetFont(small);
      const int sy = rect.y + (rect.height - dc.GetCharHeight()) / 2;
      draw_marked(dc, t, 0, it.folder.size(), r.pos, x + FromDIP(12), sy, right, small, small_bold, dim_colour(), hl);
    }
    dc.SetFont(normal);
  }

 private:
  const std::vector<QuickItem>& items_;
  const std::vector<std::string>& targets_;
  const std::vector<Row>& rows_;
};

class QuickOpenDialog : public wxDialog {
 public:
  QuickOpenDialog(wxWindow* parent, std::vector<QuickItem> items)
      : wxDialog(parent, wxID_ANY, "Open request", wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE),
        items_(std::move(items)) {
    SetBackgroundColour(window_colour());
    targets_.reserve(items_.size());
    for (const auto& it : items_) targets_.push_back(it.folder.empty() ? it.name : it.folder + "/" + it.name);

    filter_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER | wxBORDER_THEME);
    filter_->SetHint("Search saved requests by name or folder (prefix GET / POST / gRPC to narrow)");
    list_ = new ResultList(this, items_, targets_, rows_);
    count_ = new wxStaticText(this, wxID_ANY, "");
    count_->SetForegroundColour(dim_colour());
    auto* sz = new wxBoxSizer(wxVERTICAL);
    sz->Add(filter_, 0, wxEXPAND | wxALL, FromDIP(8));
    sz->Add(list_, 1, wxEXPAND);
    sz->Add(count_, 0, wxEXPAND | wxALL, FromDIP(6));
    SetSizer(sz);
    const wxSize size = FromDIP(wxSize(680, 440));
    SetSize(size);
    if (parent) {
      const wxRect pr = wxGetTopLevelParent(parent)->GetScreenRect();
      SetPosition(wxPoint(pr.x + (pr.width - size.x) / 2, pr.y + FromDIP(72)));
    }

    filter_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { refill(); });
    filter_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { accept(); });
    filter_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
      const int n = static_cast<int>(rows_.size());
      int s = list_->GetSelection();
      const int page = std::max(1, list_->GetClientSize().y / FromDIP(30) - 1);
      switch (e.GetKeyCode()) {
        case WXK_DOWN: s = n ? (s + 1) % n : -1; break;
        case WXK_UP: s = n ? (s - 1 + n) % n : -1; break;
        case WXK_PAGEDOWN: s = std::min(n - 1, s + page); break;
        case WXK_PAGEUP: s = std::max(0, s - page); break;
        case WXK_ESCAPE: EndModal(wxID_CANCEL); return;
        default: e.Skip(); return;
      }
      if (s >= 0 && s < n) {
        list_->SetSelection(s);
      }
    });
    list_->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) { accept(); });
    list_->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) {
      e.Skip();
      const int hit = list_->VirtualHitTest(e.GetPosition().y);
      if (hit != wxNOT_FOUND) {
        list_->SetSelection(hit);
        CallAfter([this] { accept(); });
      }
    });
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
      if (e.GetKeyCode() == WXK_ESCAPE) EndModal(wxID_CANCEL);
      else e.Skip();
    });
    // Click outside: close, like VS Code.
    Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& e) {
      e.Skip();
      if (!e.GetActive() && IsModal() && ready_ && !accepted_) CallAfter([this] {
        if (IsModal() && !accepted_) EndModal(wxID_CANCEL);
      });
    });
    refill();
    filter_->SetFocus();
    CallAfter([this] { ready_ = true; });
  }

  std::filesystem::path chosen;

 private:
  void refill() {
    const std::string q = filter_->GetValue().utf8_string();
    // Method words ("get", "post", "grpc") narrow by badge instead of matching text.
    std::string rest;
    bool have_kind = false;
    RequestKind want_kind = RequestKind::Http;
    std::string want_method;
    for (size_t p = 0; p < q.size();) {
      size_t e = q.find(' ', p);
      if (e == std::string::npos) e = q.size();
      const std::string w = ascii_lower(q.substr(p, e - p));
      RequestKind k;
      std::string m;
      if (!have_kind && !w.empty() && is_method_word(w, &k, &m)) {
        have_kind = true;
        want_kind = k;
        want_method = m;
      } else if (!w.empty()) {
        rest += (rest.empty() ? "" : " ") + q.substr(p, e - p);
      }
      p = e + 1;
    }
    const FuzzyQuery fq(rest);
    // A method word is also text: "get" finds "Get user" by its name too, and such name matches
    // come first (the GET badge alone matches half the collection), with their letters marked.
    const FuzzyQuery fq_all(have_kind ? q : std::string());
    constexpr int kNameTier = 1 << 20;
    rows_.clear();
    for (size_t i = 0; i < items_.size(); ++i) {
      const QuickItem& it = items_[i];
      const size_t ns = it.folder.empty() ? 0 : it.folder.size() + 1;
      Row r{i, 0, {}};
      if (!fq_all.empty()) {
        // Only the name part counts here: a folder called "get" is not a name match.
        int sc = 0;
        std::vector<uint16_t> pos;
        const std::string name = ascii_lower(std::string_view(targets_[i]).substr(std::min(ns, targets_[i].size())));
        const bool in_name = std::all_of(fq_all.words().begin(), fq_all.words().end(),
                                         [&](const std::string& w) { return name.find(w) != std::string::npos; });
        if (in_name && fq_all.match(targets_[i], ns, &sc, &pos) && !pos.empty() && pos.front() >= ns) {
          r.score = kNameTier + sc;
          r.pos = std::move(pos);
          rows_.push_back(std::move(r));
          continue;
        }
      }
      if (have_kind) {
        if (it.kind != want_kind) continue;
        if (want_kind == RequestKind::Http && (!it.badge || ascii_lower(it.method) != want_method)) continue;
      }
      if (!fq.empty() && !fq.match(targets_[i], ns, &r.score, &r.pos)) continue;
      rows_.push_back(std::move(r));
    }
    const bool name_hits = std::any_of(rows_.begin(), rows_.end(), [&](const Row& r) { return r.score >= kNameTier; });
    // Nothing matches every word: the closest, where a word may match only by its start ("svc150"
    // as "svc15": a number typed one digit off), fewest characters dropped first.
    near_ = false;
    if (rows_.empty() && !fq.empty()) near_fill(fq.words(), have_kind, want_kind, want_method);
    if (fq.empty() && !name_hits) {
      // Recently used first, then by path.
      std::stable_sort(rows_.begin(), rows_.end(), [this](const Row& a, const Row& b) {
        const int ra = items_[a.item].recent, rb = items_[b.item].recent;
        if ((ra >= 0) != (rb >= 0)) return ra >= 0;
        if (ra >= 0) return ra < rb;
        return targets_[a.item] < targets_[b.item];
      });
    } else if (!near_) {  // near_fill() ordered its rows
      std::stable_sort(rows_.begin(), rows_.end(), [this](const Row& a, const Row& b) {
        if (a.score != b.score) return a.score > b.score;
        const int ra = items_[a.item].recent, rb = items_[b.item].recent;
        if ((ra >= 0) != (rb >= 0)) return ra >= 0;
        return targets_[a.item].size() < targets_[b.item].size();
      });
    }
    list_->SetItemCount(rows_.size());
    if (!rows_.empty()) list_->SetSelection(0);
    list_->Refresh();
    if (items_.empty()) count_->SetLabel("No saved requests in this workspace yet (Ctrl+S saves the current tab)");
    else if (near_) count_->SetLabel(wxString::Format("No exact match: %zu close", rows_.size()) +
                                     wxString::FromUTF8("   ↑↓ move   Enter open   Esc close"));
    else count_->SetLabel(wxString::Format("%zu of %zu requests", rows_.size(), items_.size()) +
                          wxString::FromUTF8("   ↑↓ move   Enter open   Esc close"));
  }

  void near_fill(const std::vector<std::string>& words, bool have_kind, RequestKind want_kind,
                 const std::string& want_method) {
    // Per word, its prefixes from the longest down to 60% of it (at least 2 characters).
    std::vector<std::vector<std::pair<FuzzyQuery, size_t>>> pre(words.size());
    for (size_t w = 0; w < words.size(); ++w)
      for (size_t len = words[w].size(); len >= 2 && len * 5 >= words[w].size() * 3; --len)
        pre[w].emplace_back(FuzzyQuery(words[w].substr(0, len)), words[w].size() - len);
    struct Near {
      Row row;
      size_t dropped;
    };
    std::vector<Near> found;
    for (size_t i = 0; i < items_.size(); ++i) {
      const QuickItem& it = items_[i];
      if (have_kind) {
        if (it.kind != want_kind) continue;
        if (want_kind == RequestKind::Http && (!it.badge || ascii_lower(it.method) != want_method)) continue;
      }
      const size_t ns = it.folder.empty() ? 0 : it.folder.size() + 1;
      Near n{Row{i, 0, {}}, 0};
      bool ok = true;
      for (size_t w = 0; w < pre.size() && ok; ++w) {
        ok = false;
        for (const auto& [q, dropped] : pre[w]) {
          int s = 0;
          std::vector<uint16_t> pos;
          if (!q.match(targets_[i], ns, &s, &pos)) continue;
          n.row.score += s;
          n.dropped += dropped;
          // The dropped tail found later in the target ("op4712": "12" in "op 47-012") wins it
          // back, most when it ends a word there.
          std::string lower_t = targets_[i];
          for (char& ch : lower_t) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
          const std::string_view t = lower_t;
          const size_t after = pos.empty() ? ns : static_cast<size_t>(*std::max_element(pos.begin(), pos.end())) + 1;
          const std::string_view tail = std::string_view(words[w]).substr(words[w].size() - dropped);
          for (size_t l = tail.size(); l > 0 && after < t.size(); --l) {
            const std::string_view piece = tail.substr(tail.size() - l);
            size_t best = std::string_view::npos;
            for (size_t at = t.find(piece, after); at != std::string_view::npos; at = t.find(piece, at + 1)) {
              best = at;
              const size_t end = at + l;
              if (end == t.size() || !std::isalnum(static_cast<unsigned char>(t[end]))) break;
            }
            if (best == std::string_view::npos) continue;
            const size_t end = best + l;
            n.dropped -= l;
            n.row.score += end == t.size() || !std::isalnum(static_cast<unsigned char>(t[end])) ? 40 : 10;
            for (size_t k = best; k < end; ++k) pos.push_back(static_cast<uint16_t>(k));
            break;
          }
          n.row.pos.insert(n.row.pos.end(), pos.begin(), pos.end());
          ok = true;
          break;
        }
      }
      if (!ok) continue;
      std::sort(n.row.pos.begin(), n.row.pos.end());
      n.row.pos.erase(std::unique(n.row.pos.begin(), n.row.pos.end()), n.row.pos.end());
      found.push_back(std::move(n));
    }
    std::stable_sort(found.begin(), found.end(), [](const Near& a, const Near& b) {
      if (a.dropped != b.dropped) return a.dropped < b.dropped;
      return a.row.score > b.row.score;
    });
    for (auto& n : found) rows_.push_back(std::move(n.row));
    near_ = !rows_.empty();
  }

  void accept() {
    const int s = list_->GetSelection();
    if (s < 0 || static_cast<size_t>(s) >= rows_.size()) return;
    if (accepted_) return;
    accepted_ = true;
    chosen = items_[rows_[static_cast<size_t>(s)].item].file;
    if (IsModal()) EndModal(wxID_OK);
  }

  std::vector<QuickItem> items_;
  std::vector<std::string> targets_;
  std::vector<Row> rows_;
  wxTextCtrl* filter_;
  ResultList* list_;
  wxStaticText* count_;
  bool ready_ = false;
  bool near_ = false;  // the rows are near matches (near_fill())
  bool accepted_ = false;  // accept() ran: ignore the deactivation that EndModal itself causes
};

}  // namespace

namespace {
struct QuickPick {
  std::filesystem::path file;
  std::chrono::steady_clock::time_point at{};
};
QuickPick& quick_pick() {
  static QuickPick p;  // UI thread only
  return p;
}
}  // namespace

bool take_quick_pick(const std::filesystem::path& file) {
  QuickPick& p = quick_pick();
  if (p.file.empty() || p.file != file || std::chrono::steady_clock::now() - p.at > std::chrono::seconds(5)) return false;
  p.file.clear();
  return true;
}

std::filesystem::path quick_open(wxWindow* parent, std::vector<QuickItem> items) {
  QuickOpenDialog dlg(parent, std::move(items));
  // The chosen file wins over the return code: hiding the dialog deactivates it, and a queued
  // "click outside" cancel must never discard a choice the user already made.
  dlg.ShowModal();
  quick_pick() = {dlg.chosen, std::chrono::steady_clock::now()};
  return dlg.chosen;
}

}  // namespace watt::app::sidebar
