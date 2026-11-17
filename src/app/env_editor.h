#pragma once
// Environment editor dialog: workspace globals plus every environment, each a table of
// variables. Secret variables are masked (and edited in a password field); their values are
// stored DPAPI-sealed in <ws>/.watt/secrets.json by Workspace::save_settings(), never in the
// git-tracked environments/*.json. Variables listed in Environment::unreadable_secrets (sealed by
// another Windows user or machine) are flagged "cannot decrypt on this machine, re-enter value";
// their stored value is kept untouched until a new value is entered.
#include <string>

class wxWindow;

namespace watt {
class Workspace;
}

namespace watt::app {

// Modal. Edits ws in place and calls ws.save_settings() on Save. Returns true if anything was
// saved. `select` names the environment to show first (empty: the active one, else Globals).
bool edit_environments(wxWindow* parent, Workspace& ws, const std::string& select = {});

// Number of variables, over all environments, whose secret value cannot be decrypted here.
size_t unreadable_secret_count(Workspace& ws);

}  // namespace watt::app
