#pragma once
// The HTTP editor's view of the workspace cookie jar (core/cookies.h): which jar cookies a URL
// gets (the same rules as CookieJar::cookie_header, without touching last_access), how an expiry
// and a cookie's attributes read in the Cookies tab. These helpers need no wx (unit tested).
// Editing the jar is the app's one cookie manager (sidebar_cookies.h), which the Cookies tab opens.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/cookies.h"

class wxWindow;

namespace watt::app::http_editor {

// The unexpired cookies of `all` that a request to url would carry from the jar, in the order
// they are sent (longer paths first, then older first). A URL without a scheme counts as http://
// (as it is sent); empty for one that is not http(s) or still holds a {{variable}}.
std::vector<Cookie> jar_cookies_for(const std::vector<Cookie>& all, std::string_view url, int64_t now);
// The host of an http(s) URL, lower case ("" when there is none).
std::string url_host(std::string_view url);
// "Session", "in 5 min", "in 3 h", "in 12 days", "2027-01-31" (UTC) or "expired".
std::string expiry_text(int64_t expires, int64_t now);
// "Secure HttpOnly Lax +subdomains" (the set attributes), "" when none.
std::string cookie_flags(const Cookie& c);

// Opens the app's cookie manager (sidebar::manage_cookies: loads the jar on a worker, then shows
// the dialog modally) on focus_host. Returns at once; on_changed is not called (the manager edits
// the live jar: re-read it when the window is active again). Implemented in
// http_editor_cookies_ui.cpp (needs wx).
void show_cookie_manager(wxWindow* parent, std::shared_ptr<CookieJar> jar, const std::string& focus_host,
                         const std::function<void()>& on_changed = {});

}  // namespace watt::app::http_editor
