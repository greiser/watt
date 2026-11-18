#pragma once
// Cookie manager of the workspace's cookie jar (core/cookies.h): Postman's "Manage Cookies",
// Insomnia's "Cookie Manager", Bruno's Cookies. Domains on the left (with counts), the domain's
// cookies in a table (name, value, path, expiry, flags), and an editor for one cookie below
// (name, value, domain, path, expiry, Secure, HttpOnly, SameSite, subdomains). Also: filter by
// domain / name / value, add one from a pasted Set-Cookie line (the jar's own rules apply), copy
// as Set-Cookie, delete one, a domain, or all. Changes go to the live jar at once (it writes
// .watt/cookies.json on its own thread); the jar is loaded on a worker before the dialog opens.
#include <memory>
#include <string>

class wxWindow;

namespace watt {
class CookieJar;
}

namespace watt::app::sidebar {

// Loads the jar on a worker, then shows the dialog (modal) on the UI thread. `domain`: the domain
// to select first (empty: all). Returns at once.
void manage_cookies(wxWindow* parent, std::shared_ptr<CookieJar> jar, const std::string& domain = {});

}  // namespace watt::app::sidebar
