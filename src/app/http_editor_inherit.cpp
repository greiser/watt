// Folder / collection inheritance helpers of the HTTP editor (see http_editor_inherit.h).
#include "app/http_editor_inherit.h"

#include <algorithm>

#include "app/http_editor_util.h"

namespace watt::app::http_editor {

namespace {

bool is_key_header(std::string_view name) {
  for (std::string_view n : {"x-api-key", "api-key", "apikey", "x-apikey", "x-api-token", "x-auth-token"})
    if (iequals(name, n)) return true;
  return false;
}

const char* scheme_label(HttpAuth::Type t) {
  switch (t) {
    case HttpAuth::Type::OAuth2: return "OAuth 2.0";
    case HttpAuth::Type::Digest: return "Digest";
    case HttpAuth::Type::Ntlm: return "NTLM";
    case HttpAuth::Type::Negotiate: return "Negotiate (Kerberos)";
    case HttpAuth::Type::AwsSigV4: return "AWS Signature v4";
    case HttpAuth::Type::None:
    case HttpAuth::Type::Inherit: break;
  }
  return "No auth";
}

FolderAuth form_of(HttpAuth::Type t) {
  switch (t) {
    case HttpAuth::Type::OAuth2: return FolderAuth::OAuth2;
    case HttpAuth::Type::Digest: return FolderAuth::Digest;
    case HttpAuth::Type::Ntlm: return FolderAuth::Ntlm;
    case HttpAuth::Type::Negotiate: return FolderAuth::Negotiate;
    case HttpAuth::Type::AwsSigV4: return FolderAuth::AwsSigV4;
    case HttpAuth::Type::Inherit: return FolderAuth::Inherit;
    case HttpAuth::Type::None: break;
  }
  return FolderAuth::None;
}

HttpAuth::Type type_of(FolderAuth a) {
  switch (a) {
    case FolderAuth::OAuth2: return HttpAuth::Type::OAuth2;
    case FolderAuth::Digest: return HttpAuth::Type::Digest;
    case FolderAuth::Ntlm: return HttpAuth::Type::Ntlm;
    case FolderAuth::Negotiate: return HttpAuth::Type::Negotiate;
    case FolderAuth::AwsSigV4: return HttpAuth::Type::AwsSigV4;
    case FolderAuth::Inherit: return HttpAuth::Type::Inherit;
    default: return HttpAuth::Type::None;
  }
}

}  // namespace

std::string mask_secret(std::string_view v) {
  v = trim(v);
  if (v.empty()) return "(empty)";
  if (v.find("{{") != std::string_view::npos) return std::string(v);
  if (v.size() <= 12) return std::string(std::min<size_t>(v.size(), 8), '*');
  return std::string(v.substr(0, 6)) + "\xE2\x80\xA6" + std::string(v.substr(v.size() - 4));
}

std::string InheritView::summary() const {
  if (!known) return "Reading the folder settings\xE2\x80\xA6";
  if (source.empty()) return "No folder above this request and no collection setting sets auth: sent with no auth.";
  std::string s = type + " from " + source;
  if (!detail.empty()) s += ": " + detail;
  return s;
}

namespace {

const char* proxy_label(NetworkSettings::Proxy p) {
  switch (p) {
    case NetworkSettings::Proxy::Env: return "environment variables";
    case NetworkSettings::Proxy::System: return "Windows system proxy";
    case NetworkSettings::Proxy::Off: return "none (direct)";
    case NetworkSettings::Proxy::Url: return "custom";
    default: return "inherit";
  }
}

std::string level_name(const SettingsOrigin& o) { return o.label.empty() ? std::string("default") : o.label; }

}  // namespace

std::string network_summary(const EffectiveSettings& e) {
  const NetworkSettings& n = e.network;
  std::vector<std::string> parts;
  if (!n.cert_file.empty())
    parts.push_back("client certificate " + n.cert_file + " (" + level_name(e.cert_from) + ")");
  if (!n.ca_file.empty()) parts.push_back("CA " + n.ca_file + " (" + level_name(e.ca_from) + ")");
  if (n.proxy == NetworkSettings::Proxy::Url)
    parts.push_back("proxy " + n.proxy_url + " (" + level_name(e.proxy_from) + ")");
  else if (n.proxy != NetworkSettings::Proxy::Env || !e.proxy_from.label.empty())
    parts.push_back(std::string("proxy: ") + proxy_label(n.proxy) + " (" + level_name(e.proxy_from) + ")");
  std::string out;
  for (const auto& p : parts) out += (out.empty() ? "" : " \xC2\xB7 ") + p;
  return out;
}

OptionsAbove options_above(const EffectiveSettings& e) {
  OptionsAbove o;
  o.n.max_redirects = e.network.max_redirects;
  o.n.keep_method = e.network.keep_method;
  o.n.keep_auth = e.network.keep_auth;
  o.n.encode_url = e.network.encode_url;
  o.n.tls_min = e.network.tls_min;
  auto from = [](bool set, const SettingsOrigin& s) { return set ? level_name(s) : std::string(); };
  o.max_redirects_from = from(o.n.max_redirects.has_value(), e.max_redirects_from);
  o.keep_method_from = from(o.n.keep_method.has_value(), e.keep_method_from);
  o.keep_auth_from = from(o.n.keep_auth.has_value(), e.keep_auth_from);
  o.encode_url_from = from(o.n.encode_url.has_value(), e.encode_url_from);
  o.tls_min_from = from(o.n.tls_min.has_value(), e.tls_min_from);
  return o;
}

HttpRequest effective_options(const NetworkSettings& own, const OptionsAbove& above) {
  HttpRequest h;
  apply_network_options(above.n, h);
  apply_network_options(own, h);
  return h;
}

InheritView make_inherit_view(const EffectiveSettings& eff) {
  InheritView v;
  v.known = true;
  v.opts = options_above(eff);
  v.errors = eff.errors;
  v.network = network_summary(eff);
  for (const auto& r : eff.headers) {
    if (r.from.level == SettingsOrigin::Level::Request || !r.row.enabled || r.overridden) continue;
    const std::string name = clean_header_name(r.row.name);
    if (name.empty()) continue;
    InheritView::Header h{name, r.row.value, r.from.label, false};
    v.headers.push_back(std::move(h));
  }
  for (const auto& r : eff.vars)
    if (r.from.level != SettingsOrigin::Level::Request && r.row.enabled && !r.overridden && !r.row.name.empty())
      v.vars.emplace_back(r.row.name, r.row.value);
  const bool from_level = eff.auth_from.level != SettingsOrigin::Level::Request;
  if (from_level) {
    v.source = eff.auth_from.label;
    v.source_folder = eff.auth_from.folder;
  }
  if (eff.auth.type != HttpAuth::Type::None && eff.auth.type != HttpAuth::Type::Inherit) {
    v.type = scheme_label(eff.auth.type);
    const HttpAuth& a = eff.auth;
    switch (a.type) {
      case HttpAuth::Type::OAuth2:
        v.detail = (a.oauth2.client_id.empty() ? std::string() : "client " + a.oauth2.client_id + ", ") +
                   (a.oauth2.token_url.empty() ? std::string("no token URL") : a.oauth2.token_url);
        break;
      case HttpAuth::Type::AwsSigV4:
        v.detail = "key " + mask_secret(a.aws_access_key) + ", " + a.aws_region + " / " + a.aws_service;
        break;
      default:
        v.detail = a.user.empty() ? std::string("your Windows sign-in") : "user " + a.user;
        break;
    }
  } else if (eff.basic_auth.enabled) {
    v.type = "Basic auth";
    v.detail = "user " + (eff.basic_auth.user.empty() ? std::string("(empty)") : eff.basic_auth.user);
  } else {
    v.type = "No auth";
  }
  // A folder's Bearer token / API key travels as its header.
  if (v.type == "No auth") {
    for (auto& h : v.headers) {
      if (iequals(h.name, "Authorization") && istarts_with(h.value, "Bearer ")) {
        h.auth = true;
        v.type = "Bearer token";
        v.detail = "token " + mask_secret(std::string_view(h.value).substr(7));
        if (!from_level || v.source.empty()) v.source = h.from;
        break;
      }
      if (is_key_header(h.name)) {
        h.auth = true;
        v.type = "API key";
        v.detail = "header " + h.name + " = " + mask_secret(h.value);
        if (!from_level || v.source.empty()) v.source = h.from;
        break;
      }
    }
  }
  return v;
}

FolderForm folder_form(const FolderSettings& s) {
  FolderForm f;
  f.auth = form_of(s.auth.type);
  f.adv = s.auth;
  f.user = s.basic_auth.user;
  f.pass = s.basic_auth.password;
  if (s.auth.type == HttpAuth::Type::None && s.basic_auth.enabled) f.auth = FolderAuth::Basic;
  bool taken = false;
  for (const auto& r : s.headers) {
    if (!taken && r.enabled && (f.auth == FolderAuth::None || f.auth == FolderAuth::Inherit)) {
      if (iequals(r.name, "Authorization") && istarts_with(r.value, "Bearer ")) {
        f.auth = FolderAuth::Bearer;
        f.token = r.value.substr(7);
        taken = true;
        continue;
      }
      if (is_key_header(r.name)) {
        f.auth = FolderAuth::ApiKey;
        f.key_name = r.name;
        f.key_value = r.value;
        taken = true;
        continue;
      }
    }
    f.headers.push_back(r);
  }
  return f;
}

void apply_folder_form(const FolderForm& f, FolderSettings& s) {
  s.headers.clear();
  s.basic_auth = {};
  s.auth = {};
  switch (f.auth) {
    case FolderAuth::Inherit: s.auth.type = HttpAuth::Type::Inherit; break;
    case FolderAuth::None: break;
    case FolderAuth::Bearer:
      if (!trim(f.token).empty()) s.headers.push_back({"Authorization", "Bearer " + clean_header_value(f.token), {}, true});
      break;
    case FolderAuth::Basic:
      s.basic_auth.enabled = true;
      s.basic_auth.user = f.user;
      s.basic_auth.password = f.pass;
      break;
    case FolderAuth::ApiKey: {
      const std::string name = clean_header_name(f.key_name);
      if (!name.empty()) s.headers.push_back({name, clean_header_value(f.key_value), {}, true});
      break;
    }
    default:
      s.auth = f.adv;
      s.auth.type = type_of(f.auth);
      break;
  }
  for (const auto& r : f.headers) {
    SettingsRow c = r;
    c.name = clean_header_name(r.name);
    c.value = clean_header_value(r.value);
    if (!c.name.empty()) s.headers.push_back(std::move(c));
  }
}

std::string rows_to_bulk(const std::vector<SettingsRow>& rows) {
  std::vector<KvItem> items;
  for (const auto& r : rows) items.push_back({r.name, r.value, r.enabled, false});
  return to_bulk(items);
}

std::vector<SettingsRow> rows_from_bulk(std::string_view text) {
  std::vector<SettingsRow> out;
  for (const auto& it : parse_bulk(text)) {
    SettingsRow r;
    r.name = clean_header_name(it.key);
    r.value = clean_header_value(it.value);
    r.enabled = it.enabled;
    if (!r.name.empty()) out.push_back(std::move(r));
  }
  return out;
}

}  // namespace watt::app::http_editor
