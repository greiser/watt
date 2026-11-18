// The wx side of http_editor_cookies.h (the pure helpers stay testable without wx).
#include <wx/window.h>

#include "app/http_editor_cookies.h"
#include "app/sidebar_cookies.h"

namespace watt::app::http_editor {

void show_cookie_manager(wxWindow* parent, std::shared_ptr<CookieJar> jar, const std::string& focus_host,
                         const std::function<void()>& on_changed) {
  (void)on_changed;
  if (!jar) return;
  sidebar::manage_cookies(parent ? wxGetTopLevelParent(parent) : nullptr, std::move(jar), focus_host);
}

}  // namespace watt::app::http_editor