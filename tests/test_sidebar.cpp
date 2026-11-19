// Sidebar file operations (src/app/sidebar_files.*): rename / duplicate / new request keep the
// kebab-case file convention and the "name" field in step, and a tab that follows a rename (the
// shell re-reads the name on file_moved) saves the new name, not the old one.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <thread>
#include <fstream>
#include <set>
#include <filesystem>
#include <algorithm>
#include <string>
#include <vector>

// No wx dependency: compiled into this test directly.
#include "app/sidebar_files.cpp"
#include "app/sidebar_fuzzy.cpp"
#include "app/sidebar_import.cpp"
#include "app/sidebar_names.cpp"
#include "app/sidebar_specs.cpp"
#include "../tools/httpserver/http_server.h"
#include "core/http_client.h"

namespace fs = std::filesystem;
using namespace watt;
using namespace watt::app::sidebar;

namespace {

struct TempWs {
  fs::path dir;
  Workspace ws;
  TempWs()
      : dir(fs::temp_directory_path() /
            ("watt_sidebar_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))),
        ws(Workspace::create(dir)) {}
  ~TempWs() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

SavedRequest saved(Workspace& ws, const fs::path& file, const std::string& name) {
  SavedRequest r;
  r.name = name;
  r.file = file;
  r.http.method = "GET";
  r.http.url = "http://127.0.0.1:18080/echo";
  REQUIRE(ws.save_request(r));
  return r;
}

size_t request_files(const fs::path& dir) {
  size_t n = 0;
  for (const auto& e : fs::recursive_directory_iterator(dir))
    if (e.is_regular_file() && e.path().filename().string().ends_with(".req.json")) ++n;
  return n;
}

}  // namespace

TEST_CASE("slug and file names", "[sidebar]") {
  CHECK(slug("Get order copy") == "get-order-copy");
  CHECK(slug("  {{base}}} ") == "base");
  CHECK(slug("***") == "request");
  CHECK(file_safe("a/b:c") == "a_b_c");
}

TEST_CASE("rename while open, then Ctrl+S keeps the new name and file", "[sidebar]") {
  TempWs t;
  fs::create_directories(t.dir / "users");
  // Saved from a new tab: spaced file name, odd name.
  SavedRequest tab = saved(t.ws, t.dir / "users" / "{{base}}}.req.json", "{{base}}}");
  const fs::path from = tab.file;

  auto to = rename_item(t.ws, from, "Get item seven", nullptr);
  REQUIRE(to);
  CHECK(*to == t.dir / "users" / "get-item-seven.req.json");
  CHECK(!fs::exists(from));
  auto on_disk = t.ws.load_request(*to);
  REQUIRE(on_disk);
  CHECK(on_disk->name == "Get item seven");

  // The tab follows (MainFrame::file_moved -> RequestTab::file_moved: new path, name re-read).
  tab.file = *to;
  tab.name = t.ws.load_request(*to)->name;
  // Ctrl+S in that tab.
  REQUIRE(t.ws.save_request(tab));
  CHECK(tab.file == *to);
  auto after = t.ws.load_request(*to);
  REQUIRE(after);
  CHECK(after->name == "Get item seven");
  CHECK(request_files(t.dir) == 1);
}

TEST_CASE("a rename that keeps the file only changes the name", "[sidebar]") {
  TempWs t;
  saved(t.ws, t.dir / "get-user.req.json", "Get user");
  auto to = rename_item(t.ws, t.dir / "get-user.req.json", "GET user", nullptr);
  REQUIRE(to);
  CHECK(*to == t.dir / "get-user.req.json");
  CHECK(t.ws.load_request(*to)->name == "GET user");
}

TEST_CASE("rename never overwrites another request", "[sidebar]") {
  TempWs t;
  saved(t.ws, t.dir / "a.req.json", "A");
  saved(t.ws, t.dir / "b.req.json", "B");
  std::string err;
  // The name is taken as typed (duplicates allowed, as in Postman); the file gets a suffix.
  const auto to = rename_item(t.ws, t.dir / "a.req.json", "b", &err);
  REQUIRE(to);
  CHECK(to->filename() == "b-2.req.json");
  CHECK(t.ws.load_request(*to)->name == "b");
  CHECK(!fs::exists(t.dir / "a.req.json"));
  CHECK(t.ws.load_request(t.dir / "b.req.json")->name == "B");
  // Renaming it to the same name again keeps its file (no b-3).
  const auto again = rename_item(t.ws, *to, "b", &err);
  REQUIRE(again);
  CHECK(again->filename() == "b-2.req.json");
  // A folder name that is taken still fails (folders are paths), without touching either.
  fs::create_directories(t.dir / "f1");
  fs::create_directories(t.dir / "f2");
  CHECK(!rename_item(t.ws, t.dir / "f1", "f2", &err));
  CHECK(!err.empty());
  CHECK(fs::exists(t.dir / "f1"));
}

TEST_CASE("folder rename", "[sidebar]") {
  TempWs t;
  fs::create_directories(t.dir / "orders");
  saved(t.ws, t.dir / "orders" / "get-order.req.json", "Get order");
  auto to = rename_item(t.ws, t.dir / "orders", "Orders v2", nullptr);
  REQUIRE(to);
  CHECK(*to == t.dir / "Orders v2");
  CHECK(fs::exists(*to / "get-order.req.json"));
}

TEST_CASE("duplicate and new request use kebab file names", "[sidebar]") {
  TempWs t;
  saved(t.ws, t.dir / "get-user.req.json", "Get user");
  auto c1 = duplicate_request(t.ws, t.dir / "get-user.req.json", nullptr);
  REQUIRE(c1);
  CHECK(c1->name == "Get user copy");
  CHECK(c1->file == t.dir / "get-user-copy.req.json");
  auto c2 = duplicate_request(t.ws, t.dir / "get-user.req.json", nullptr);
  REQUIRE(c2);
  CHECK(c2->name == "Get user copy 2");
  CHECK(c2->file == t.dir / "get-user-copy-2.req.json");
  CHECK(c1->id != c2->id);

  auto n = create_request(t.ws, t.dir, RequestKind::Http, "New request", nullptr);
  REQUIRE(n);
  CHECK(n->file == t.dir / "new-request.req.json");
  CHECK(t.ws.load_request(n->file)->name == "New request");
  // Named right away (Duplicate -> rename): the tab opens with this name.
  auto r = rename_item(t.ws, c1->file, "Get user by id", nullptr);
  REQUIRE(r);
  CHECK(*r == t.dir / "get-user-by-id.req.json");
}

// The open tab of a renamed (then moved) request re-reads its file (RequestTab::file_moved) and
// hands that name to its editor, whose breadcrumb is "<folders>  /  <name>". The name must be the
// new one after each step, and the id must stay (history groups repeats by it).
TEST_CASE("rename then move: the tab's breadcrumb name and the request id follow", "[sidebar]") {
  TempWs t;
  fs::create_directories(t.dir / "Shop API" / "catalog");
  auto n = create_request(t.ws, t.dir / "Shop API" / "catalog", RequestKind::Http, "New request", nullptr);
  REQUIRE(n);
  const std::string id = t.ws.load_request(n->file)->id;
  REQUIRE(!id.empty());
  auto crumb = [&](const fs::path& file) {
    // What the tab's editor shows after file_moved(): the folders, then the re-read name.
    std::string s;
    for (const auto& part : fs::relative(file.parent_path(), t.dir)) s += part.string() + "  /  ";
    return s + t.ws.load_request(file)->name;
  };
  auto to = rename_item(t.ws, n->file, "Product stock", nullptr);
  REQUIRE(to);
  CHECK(*to == t.dir / "Shop API" / "catalog" / "product-stock.req.json");
  CHECK(crumb(*to).ends_with("Product stock"));
  CHECK(crumb(*to) == "Shop API  /  catalog  /  Product stock");
  CHECK(t.ws.load_request(*to)->id == id);
  // Drag to the collection root (a plain file move, as the sidebar's move_item does).
  const fs::path moved = t.dir / "Shop API" / to->filename();
  fs::rename(*to, moved);
  CHECK(crumb(moved) == "Shop API  /  Product stock");
  auto again = rename_item(t.ws, moved, "Stock level", nullptr);
  REQUIRE(again);
  CHECK(crumb(*again).ends_with("Stock level"));
  CHECK(t.ws.load_request(*again)->id == id);
}

TEST_CASE("filter rank: exact name before folder path matches", "[sidebar]") {
  const int create_order = filter_rank("create order", "Create order", "orders", "POST {{base}}/orders");
  const int create_refund = filter_rank("create order", "Create refund", "orders/refunds", "POST {{base}}/refunds");
  CHECK(create_order > create_refund);
  CHECK(create_refund >= 0);
  CHECK(filter_rank("zzz", "Create order", "orders", "") < 0);
  CHECK(filter_rank("order", "List orders", "orders", "") > filter_rank("order", "Get user", "orders", ""));
}

TEST_CASE("filter rank on lower-case keys matches filter_rank and stays cheap", "[sidebar]") {
  // The sidebar ranks every hit of a keystroke with pre-lowered keys (no allocation).
  CHECK(filter_rank_lower("create order", "create order", "orders", "post {{base}}/orders") ==
        filter_rank("create order", "Create order", "orders", "POST {{base}}/orders"));
  CHECK(filter_rank_lower("ref  order", "create refund", "orders/refunds", "post") ==
        filter_rank("ref  order", "Create refund", "orders/refunds", "POST"));
  CHECK(filter_rank_lower("", "a", "b", "c") < 0);
  CHECK(filter_rank_lower("   ", "a", "b", "c") < 0);
  CHECK(filter_rank_lower("zzz", "a", "b", "c") < 0);
  // 10k hits (a keystroke in a 10k-request workspace) in a few ms.
  std::vector<std::string> names;
  for (int i = 0; i < 10000; ++i) names.push_back("endpoint " + std::to_string(i));
  const auto t0 = std::chrono::steady_clock::now();
  int best = -1;
  for (const auto& n : names) best = std::max(best, filter_rank_lower("endpoint 77", n, "bulk/group 07", "get /json/77"));
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  CHECK(best >= 10000);  // the exact name wins
  CHECK(ms < 25);
}

TEST_CASE("natural order: numbers by value, case-insensitive", "[sidebar]") {
  CHECK(natural_less("endpoint 17-2", "endpoint 17-10"));
  CHECK_FALSE(natural_less("endpoint 17-10", "endpoint 17-2"));
  CHECK(natural_less("item 9", "item 10"));
  CHECK(natural_less("apple", "Banana"));
  CHECK(natural_less("a", "a1"));
  CHECK(natural_less("v007", "v8"));
  CHECK_FALSE(natural_less("same", "same"));
}

// ------------------------------------------------------------------ history store

#include "core/model_history_store.h"

namespace {
HistoryEntry entry(const std::string& when, const std::string& summary, long status, double ms) {
  HistoryEntry e;
  e.when_iso = when;
  e.summary = summary;
  e.status = status;
  e.total_ms = ms;
  return e;
}
}  // namespace

TEST_CASE("history: a kept response is found again from the listed entry", "[sidebar][history]") {
  TempWs t;
  HistoryEntry live = entry("2026-11-17T10:00:00Z", "GET http://127.0.0.1:18080/json", 200, 1.5412345678901234);
  live.request.name = "Get json";
  live.request.http.url = "http://127.0.0.1:18080/json";
  t.ws.append_history(live);
  REQUIRE(save_history_response(t.dir, live, "{\"kind\":\"http\",\"status\":200}"));
  // What Workspace::history() reads back (a restart) maps to the same file.
  auto listed = t.ws.history(10);
  REQUIRE(listed.size() == 1);
  CHECK(history_entry_key(listed[0]) == history_entry_key(live));
  auto j = load_history_response(t.dir, listed[0]);
  REQUIRE(j);
  CHECK(*j == "{\"kind\":\"http\",\"status\":200}");
  // Another send of the same request in the same second is another entry.
  CHECK(history_entry_key(entry(live.when_iso, live.summary, 200, 1.6)) != history_entry_key(live));
  // Kept under .watt only.
  CHECK(history_response_file(t.dir, live).parent_path() == t.dir / ".watt" / "responses");
}

TEST_CASE("history: delete and clear hide entries and drop their responses", "[sidebar][history]") {
  TempWs t;
  std::vector<HistoryEntry> all;
  for (int i = 0; i < 5; ++i) {
    HistoryEntry e = entry("2026-11-17T10:00:0" + std::to_string(i) + "Z", "GET http://h/health", 200, 1.0 + i);
    t.ws.append_history(e);
    save_history_response(t.dir, e, "{}");
    all.insert(all.begin(), e);  // newest first
  }
  REQUIRE(hide_history_entries(t.dir, {all[1]}));
  CHECK(!load_history_response(t.dir, all[1]));
  CHECK(load_history_response(t.dir, all[0]));
  auto listed = t.ws.history(100);
  filter_hidden_history(load_history_hidden(t.dir), listed);
  CHECK(listed.size() == 4);

  REQUIRE(clear_history(t.dir, all));
  CHECK(!load_history_response(t.dir, all[0]));
  listed = t.ws.history(100);
  filter_hidden_history(load_history_hidden(t.dir), listed);
  CHECK(listed.empty());
  // A send after the clear (same second as the newest cleared one) shows.
  HistoryEntry later = entry(all[0].when_iso, "GET http://h/health", 200, 9.0);
  t.ws.append_history(later);
  listed = t.ws.history(100);
  filter_hidden_history(load_history_hidden(t.dir), listed);
  REQUIRE(listed.size() == 1);
  CHECK(listed[0].total_ms == 9.0);
}

TEST_CASE("history: hidden filter keeps listing 10k entries fast", "[sidebar][history]") {
  TempWs t;
  std::vector<HistoryEntry> hide;
  for (int i = 0; i < 10000; ++i) {
    char when[32];
    std::snprintf(when, sizeof when, "2026-11-17T%02d:%02d:%02dZ", i / 3600, (i / 60) % 60, i % 60);
    HistoryEntry e = entry(when, "GET http://127.0.0.1:18080/json/" + std::to_string(i), 200, 0.5 + i * 0.001);
    t.ws.append_history(e);
    if (i % 10 == 0) hide.push_back(e);
  }
  REQUIRE(hide_history_entries(t.dir, hide));
  t.ws.history(10);  // warm the segment handles, as the app's first listing does
  const auto t0 = std::chrono::steady_clock::now();
  auto listed = t.ws.history(10000);
  const auto t1 = std::chrono::steady_clock::now();
  const HistoryHidden h = load_history_hidden(t.dir);
  filter_hidden_history(h, listed);
  const auto t2 = std::chrono::steady_clock::now();
  CHECK(listed.size() == 9000);
  const double list_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  const double filter_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
  CHECK(filter_ms < 20.0);  // the sidebar filters ~1500 entries, not 10k
  WARN("list 10k: " << list_ms << " ms, hidden filter (1000 keys): " << filter_ms << " ms");
  // What the sidebar lists: the newest 500 plus room for the hidden ones.
  const auto t3 = std::chrono::steady_clock::now();
  auto shown = t.ws.history(500 + h.keys.size());
  filter_hidden_history(load_history_hidden(t.dir), shown);
  const double side_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t3).count();
  CHECK(shown.size() >= 500);
  CHECK(side_ms < 30.0);
  WARN("sidebar listing (1500 read, filtered): " << side_ms << " ms");
}

TEST_CASE("history: prune keeps the newest response files", "[sidebar][history]") {
  TempWs t;
  for (int i = 0; i < 10; ++i) save_history_response(t.dir, entry("2026-11-17T10:00:00Z", "x", i, 1), std::string(1000, 'a'));
  prune_history_responses(t.dir, 4, 1ull << 30);
  size_t n = 0;
  for (const auto& e : fs::directory_iterator(t.dir / ".watt" / "responses")) n += e.is_regular_file();
  CHECK(n == 4);
}

// ------------------------------------------------------------------ collection import

namespace {

// A Postman v2.1 export as the app writes it (Collection > Export > v2.1): ids, the schema URL,
// folder auth and scripts, disabled headers / params, every body mode, collection variables.
const char* kPostmanV21 = R"PM({
	"info": {
		"_postman_id": "8c3e2b5f-1f0a-4c7d-9a43-2d1f6b8e9c10",
		"name": "Shop API",
		"description": "Orders and users",
		"schema": "https://schema.getpostman.com/json/collection/v2.1.0/collection.json",
		"_exporter_id": "31415926"
	},
	"item": [
		{
			"name": "Users",
			"item": [
				{
					"name": "Get user",
					"event": [
						{
							"listen": "test",
							"script": {
								"exec": [
									"pm.test(\"status 200\", function () {",
									"    pm.response.to.have.status(200);",
									"});"
								],
								"type": "text/javascript",
								"packages": {}
							}
						}
					],
					"request": {
						"method": "GET",
						"header": [
							{ "key": "Accept", "value": "application/json", "type": "text" },
							{ "key": "X-Debug", "value": "1", "type": "text", "disabled": true }
						],
						"url": {
							"raw": "{{baseUrl}}/users/:id?expand=orders",
							"host": [ "{{baseUrl}}" ],
							"path": [ "users", ":id" ],
							"query": [
								{ "key": "expand", "value": "orders" },
								{ "key": "trace", "value": "true", "disabled": true }
							],
							"variable": [ { "key": "id", "value": "42" } ]
						}
					},
					"response": []
				},
				{
					"name": "Admin",
					"item": [
						{
							"name": "Create user",
							"request": {
								"auth": {
									"type": "basic",
									"basic": [
										{ "key": "password", "value": "{{adminPass}}", "type": "string" },
										{ "key": "username", "value": "admin", "type": "string" }
									]
								},
								"method": "POST",
								"header": [],
								"body": {
									"mode": "raw",
									"raw": "{\n    \"name\": \"Ada\"\n}",
									"options": { "raw": { "language": "json" } }
								},
								"url": {
									"raw": "{{baseUrl}}/users",
									"host": [ "{{baseUrl}}" ],
									"path": [ "users" ]
								}
							},
							"response": []
						}
					]
				}
			],
			"auth": {
				"type": "bearer",
				"bearer": [ { "key": "token", "value": "{{token}}", "type": "string" } ]
			},
			"event": [
				{ "listen": "prerequest", "script": { "type": "text/javascript", "exec": [ "console.log('users folder');" ] } }
			],
			"variable": [ { "key": "userId", "value": "42" } ]
		},
		{
			"name": "Login (form)",
			"request": {
				"auth": { "type": "noauth" },
				"method": "POST",
				"header": [],
				"body": {
					"mode": "urlencoded",
					"urlencoded": [
						{ "key": "user", "value": "ada lovelace", "type": "text" },
						{ "key": "debug", "value": "1", "type": "text", "disabled": true }
					]
				},
				"url": { "raw": "https://shop.example.com/login", "protocol": "https", "host": [ "shop", "example", "com" ], "path": [ "login" ] }
			},
			"response": []
		},
		{
			"name": "Upload avatar",
			"request": {
				"method": "PUT",
				"header": [],
				"body": {
					"mode": "formdata",
					"formdata": [
						{ "key": "file", "type": "file", "src": "/Users/ada/avatar.png" },
						{ "key": "caption", "value": "me", "type": "text" }
					]
				},
				"url": { "raw": "{{baseUrl}}/avatar", "host": [ "{{baseUrl}}" ], "path": [ "avatar" ] }
			},
			"response": []
		},
		{
			"name": "Legacy FTP",
			"request": { "method": "GET", "header": [], "url": { "raw": "ftp://files.example.com/a.txt" } },
			"response": []
		}
	],
	"auth": {
		"type": "apikey",
		"apikey": [
			{ "key": "value", "value": "{{apiKey}}", "type": "string" },
			{ "key": "key", "value": "X-Api-Key", "type": "string" }
		]
	},
	"event": [
		{ "listen": "prerequest", "script": { "type": "text/javascript", "packages": {}, "exec": [ "" ] } },
		{ "listen": "test", "script": { "type": "text/javascript", "packages": {}, "exec": [ "" ] } }
	],
	"variable": [
		{ "key": "baseUrl", "value": "https://shop.example.com/api", "type": "string" },
		{ "key": "token", "value": "", "type": "string" }
	]
})PM";

const SavedRequest* find_req(const ImportFolder& f, const std::string& name) {
  for (const auto& r : f.requests)
    if (r.name == name) return &r;
  for (const auto& sub : f.folders)
    if (const SavedRequest* r = find_req(sub, name)) return r;
  return nullptr;
}

std::string header(const SavedRequest& r, const std::string& name) {
  for (const auto& h : r.http.headers)
    if (h.first == name) return h.second;
  return "<none>";
}

void put_file(const fs::path& p, const std::string& s) { std::ofstream(p, std::ios::binary) << s; }

std::string folder_var(const ImportFolder& f, const std::string& name) {
  for (const auto& v : f.vars)
    if (v.name == name) return v.enabled ? v.value : "<disabled>";
  return "<none>";
}

}  // namespace

TEST_CASE("import: a Postman v2.1 export becomes folders, requests and collection variables", "[sidebar][import]") {
  std::string err;
  auto r = parse_collection_text(kPostmanV21, &err);
  REQUIRE(r);
  CHECK(r->format == "Postman Collection v2.1");
  CHECK(r->root.name == "Shop API");
  REQUIRE(r->root.folders.size() == 1);
  CHECK(r->root.folders[0].name == "Users");
  REQUIRE(r->root.folders[0].folders.size() == 1);
  CHECK(r->root.folders[0].folders[0].name == "Admin");

  const SavedRequest* get = find_req(r->root, "Get user");
  REQUIRE(get);
  CHECK(get->http.method == "GET");
  CHECK(get->http.url == "{{baseUrl}}/users/:id?expand=orders");
  CHECK(get->disabled_params == std::vector<std::pair<std::string, std::string>>{{"trace", "true"}});
  CHECK(get->path_params == std::vector<std::pair<std::string, std::string>>{{"id", "42"}});
  CHECK(header(*get, "Accept") == "application/json");
  REQUIRE(get->http.headers.size() == 3);  // Accept, X-Debug (disabled), Authorization (folder bearer)
  CHECK(get->disabled_headers == std::set<size_t>{1});
  CHECK(header(*get, "Authorization") == "Bearer {{token}}");  // folder auth wins over collection apikey
  CHECK(header(*get, "X-Api-Key") == "<none>");
  CHECK(get->pre_script == "console.log('users folder');");
  CHECK(get->post_script.find("pm.response.to.have.status(200)") != std::string::npos);

  const SavedRequest* create = find_req(r->root, "Create user");
  REQUIRE(create);
  CHECK(create->http.method == "POST");
  CHECK(create->basic_auth.enabled);
  CHECK(create->basic_auth.user == "admin");
  CHECK(create->basic_auth.password == "{{adminPass}}");
  CHECK(header(*create, "Content-Type") == "application/json");
  CHECK(create->http.body.find("\"Ada\"") != std::string::npos);

  const SavedRequest* login = find_req(r->root, "Login (form)");
  REQUIRE(login);
  CHECK(login->http.body == "user=ada%20lovelace");
  CHECK(header(*login, "Content-Type") == "application/x-www-form-urlencoded");
  CHECK(header(*login, "X-Api-Key") == "<none>");  // noauth

  const SavedRequest* up = find_req(r->root, "Upload avatar");
  REQUIRE(up);
  REQUIRE(up->form.size() == 2);
  CHECK(up->form[0].kind == FormPart::Kind::File);
  CHECK(up->form[0].value == "/Users/ada/avatar.png");  // a reference, read (with consent) at send
  CHECK(header(*up, "X-Api-Key") == "{{apiKey}}");       // collection auth inherited

  CHECK(find_req(r->root, "Legacy FTP") == nullptr);
  REQUIRE(r->skipped.size() == 1);
  CHECK(r->skipped[0].find("ftp://") != std::string::npos);

  // Collection variables are the collection folder's variables, folder ones the folder's: they
  // resolve under whatever environment is active (no new, inactive environment).
  CHECK(r->envs.empty());
  CHECK(folder_var(r->root, "baseUrl") == "https://shop.example.com/api");
  CHECK(folder_var(r->root, "token") == "");
  CHECK(folder_var(r->root.folders[0], "userId") == "42");

  // Written into the workspace: one folder per collection folder, .req.json per request.
  TempWs t;
  const fs::path dest = t.dir / "imports";
  fs::create_directories(dest);
  auto w = write_import(t.ws, dest, *r);
  CHECK(w.requests == 4);
  CHECK(w.folders == 3);
  CHECK(w.top == dest / "Shop API");
  CHECK(fs::exists(dest / "Shop API" / "Users" / "Admin" / "create-user.req.json"));
  CHECK(fs::exists(dest / "Shop API" / "Users" / "get-user.req.json"));
  auto back = t.ws.load_request(dest / "Shop API" / "Users" / "get-user.req.json");
  REQUIRE(back);
  CHECK(back->http.url == "{{baseUrl}}/users/:id?expand=orders");
  CHECK(back->disabled_headers == std::set<size_t>{1});
  CHECK(back->post_script == get->post_script);
  CHECK(w.vars == 3);
  {
    auto fs_top = t.ws.folder_settings(dest / "Shop API");
    REQUIRE(fs_top);
    REQUIRE(fs_top->vars.size() == 2);
    CHECK(fs_top->vars[0].name == "baseUrl");
    auto fs_users = t.ws.folder_settings(dest / "Shop API" / "Users");
    REQUIRE(fs_users);
    REQUIRE(fs_users->vars.size() == 1);
    // An unrelated active environment does not hide them.
    Environment local;
    local.name = "local";
    local.vars["other"] = "1";
    t.ws.environments().push_back(local);
    t.ws.active_env = "local";
    CHECK(t.ws.expand(back->http.url, *back) == "https://shop.example.com/api/users/:id?expand=orders");
    CHECK(t.ws.expand("{{userId}}", *back) == "42");
  }
  REQUIRE(w.order.count(dest / "Shop API"));
  CHECK(w.order[dest / "Shop API"].front() == L"Users");
  const std::string summary = import_summary(*r, w);
  INFO(summary);
  CHECK(summary.find("Imported 4 requests, 3 folders, 1 skipped (1 ftp:// URL") == 0);

  // A second import does not overwrite the first.
  auto r2 = parse_collection_text(kPostmanV21, &err);
  REQUIRE(r2);
  auto w2 = write_import(t.ws, dest, *r2);
  CHECK(w2.top == dest / "Shop API 2");

  // Environments merge: a new one is added, an existing one only gains the vars it lacks.
  std::vector<Environment> envs(1);
  envs[0].name = "Shop API";
  envs[0].vars["baseUrl"] = "http://localhost:8080";
  std::vector<Environment> imported(1);
  imported[0].name = "Shop API";
  imported[0].vars = {{"baseUrl", "https://shop.example.com/api"}, {"userId", "42"}};
  CHECK(merge_import_envs(envs, imported) == 1);
  CHECK(envs[0].vars["baseUrl"] == "http://localhost:8080");
  CHECK(envs[0].vars["userId"] == "42");
}

TEST_CASE("import: hostile names stay inside the workspace", "[sidebar][import]") {
  const char* coll = R"({"info":{"name":"..","schema":"https://schema.getpostman.com/json/collection/v2.1.0/collection.json"},
    "item":[{"name":"../../escape","item":[{"name":"..\\..\\x","request":{"method":"GET","url":"http://a/"}}]},
            {"name":"CON","item":[{"name":"r","request":{"method":"G T","url":"http://a/"}}]},
            {"name":"file","request":{"method":"GET","url":"file:///c:/windows/win.ini"}}]})";
  std::string err;
  auto r = parse_collection_text(coll, &err);
  REQUIRE(r);
  CHECK(r->skipped.size() == 2);  // the method with a space, the file: URL
  TempWs t;
  auto w = write_import(t.ws, t.dir, *r);
  CHECK(w.requests == 1);
  for (fs::recursive_directory_iterator it(t.dir), end; it != end; ++it) {
    const std::string rel = it->path().lexically_relative(t.dir).generic_string();
    CHECK(rel.find("..") != 0);
  }
  CHECK(fs::exists(w.top));
  CHECK(w.top.parent_path() == t.dir);
  // Outside the workspace: nothing is written.
  auto w2 = write_import(t.ws, t.dir.parent_path(), *r);
  CHECK(w2.requests == 0);
}

TEST_CASE("import: Insomnia v4 export", "[sidebar][import]") {
  const char* ins = R"({"_type":"export","__export_format":4,"__export_source":"insomnia.desktop.app:v8.6.1","resources":[
    {"_id":"wrk_1","parentId":null,"name":"Petstore","_type":"workspace"},
    {"_id":"fld_1","parentId":"wrk_1","name":"Pets","_type":"request_group","metaSortKey":-1},
    {"_id":"req_1","parentId":"fld_1","name":"List pets","method":"GET","url":"{{ _.base_url }}/pets",
     "headers":[{"name":"Accept","value":"application/json"},{"name":"X-Off","value":"1","disabled":true}],
     "parameters":[{"name":"limit","value":"10"},{"name":"tag","value":"dog","disabled":true}],
     "authentication":{"type":"bearer","token":"{{ _.token }}"},"body":{},"_type":"request","metaSortKey":-2},
    {"_id":"req_2","parentId":"wrk_1","name":"Add pet","method":"POST","url":"{{base_url}}/pets",
     "body":{"mimeType":"application/json","text":"{\"name\":\"Rex\"}"},"headers":[],"authentication":{},"_type":"request"},
    {"_id":"grpc_1","parentId":"wrk_1","name":"Say hello","url":"grpcs://localhost:50051","protoMethodName":"/hello.Greeter/SayHello",
     "body":{"text":"{\"name\":\"x\"}"},"metadata":[],"_type":"grpc_request"},
    {"_id":"env_base","parentId":"wrk_1","name":"Base Environment","data":{"base_url":"http://localhost:3000","auth":{"user":"a"}},"_type":"environment"},
    {"_id":"env_prod","parentId":"env_base","name":"Production","data":{"base_url":"https://petstore.example.com"},"_type":"environment"}]})";
  std::string err;
  auto r = parse_collection_text(ins, &err);
  REQUIRE(r);
  CHECK(r->format == "Insomnia v4");
  CHECK(r->root.name == "Petstore");
  REQUIRE(r->root.folders.size() == 1);
  CHECK(r->root.folders[0].name == "Pets");
  const SavedRequest* list = find_req(r->root, "List pets");
  REQUIRE(list);
  CHECK(list->http.url == "{{base_url}}/pets?limit=10");
  CHECK(list->disabled_params.size() == 1);
  CHECK(header(*list, "Authorization") == "Bearer {{token}}");
  CHECK(list->disabled_headers == std::set<size_t>{1});
  const SavedRequest* add = find_req(r->root, "Add pet");
  REQUIRE(add);
  CHECK(header(*add, "Content-Type") == "application/json");
  const SavedRequest* hello = find_req(r->root, "Say hello");
  REQUIRE(hello);
  CHECK(hello->kind == RequestKind::Grpc);
  CHECK(hello->grpc_target.tls);
  CHECK(hello->grpc_target.address == "localhost:50051");
  // The base environment: the workspace folder's variables, except base_url, which the
  // Production sub-environment overrides (an environment ranks below folder variables).
  CHECK(folder_var(r->root, "auth.user") == "a");
  CHECK(folder_var(r->root, "base_url") == "<none>");
  REQUIRE(r->envs.size() == 1);
  CHECK(r->envs[0].name == "Production");
  CHECK(r->envs[0].vars.at("base_url") == "https://petstore.example.com");
  CHECK(r->envs[0].vars.count("auth.user") == 0);
}

TEST_CASE("import: OpenAPI 3 in YAML, one request per operation, tags as folders", "[sidebar][import]") {
  const char* yaml = R"(openapi: 3.0.3
info:
  title: Pet Store   # comment
  version: "1.0"
servers:
  - url: https://{env}.example.com/v1
    variables:
      env:
        default: api
tags:
  - name: pets
    description: Pets
security:
  - bearerAuth: []
paths:
  /pets:
    get:
      tags: [pets]
      summary: List pets
      parameters:
        - name: limit
          in: query
          required: false
          schema: { type: integer, default: 20 }
        - $ref: '#/components/parameters/TraceId'
      responses:
        '200':
          description: OK
    post:
      tags:
      - pets
      operationId: createPet
      requestBody:
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/Pet'
      responses:
        '201': { description: Created }
  /pets/{petId}:
    parameters:
      - name: petId
        in: path
        required: true
        schema:
          type: string
          example: "007"
    delete:
      summary: >
        Delete a
        pet
      responses: {}
  /health:
    get:
      security: []
      description: |
        Liveness.
        Second line.
      responses: {}
components:
  parameters:
    TraceId:
      name: X-Trace
      in: header
      required: true
      schema: { type: string, example: abc }
  schemas:
    Pet:
      type: object
      required: [name]
      properties:
        name: { type: string, example: Rex }
        age: { type: integer }
        tags:
          type: array
          items: { type: string }
  securitySchemes:
    bearerAuth:
      type: http
      scheme: bearer
)";
  std::string err;
  auto r = parse_collection_text(yaml, &err);
  INFO(err);
  REQUIRE(r);
  CHECK(r->format == "OpenAPI 3.0.3");
  CHECK(r->root.name == "Pet Store");
  REQUIRE(r->root.folders.size() == 1);
  CHECK(r->root.folders[0].name == "pets");
  CHECK(r->root.folders[0].requests.size() == 2);
  CHECK(r->root.requests.size() == 2);  // untagged: delete, health
  const SavedRequest* list = find_req(r->root, "List pets");
  REQUIRE(list);
  CHECK(list->http.url == "{{baseUrl}}/pets");
  CHECK(list->disabled_params == std::vector<std::pair<std::string, std::string>>{{"limit", "20"}});
  CHECK(header(*list, "X-Trace") == "abc");
  CHECK(header(*list, "Authorization") == "Bearer {{token}}");
  const SavedRequest* create = find_req(r->root, "createPet");
  REQUIRE(create);
  CHECK(create->http.method == "POST");
  CHECK(create->http.body.find("\"name\": \"Rex\"") != std::string::npos);
  CHECK(create->http.body.find("\"tags\": [") != std::string::npos);
  const SavedRequest* del = find_req(r->root, "Delete a pet");
  REQUIRE(del);
  CHECK(del->http.url == "{{baseUrl}}/pets/:petId");
  CHECK(del->path_params == std::vector<std::pair<std::string, std::string>>{{"petId", "007"}});
  const SavedRequest* health = find_req(r->root, "GET /health");
  REQUIRE(health);
  CHECK(header(*health, "Authorization") == "<none>");
  CHECK(health->docs == "Liveness.\nSecond line.\n");
  CHECK(folder_var(r->root, "baseUrl") == "https://api.example.com/v1");
  REQUIRE(r->envs.size() == 1);  // the credential placeholders
  CHECK(r->envs[0].vars.count("baseUrl") == 0);
  CHECK(r->envs[0].secrets.count("token"));
}

TEST_CASE("import: a Bruno collection folder", "[sidebar][import]") {
  TempWs src;
  const fs::path c = src.dir / "bruno-coll";
  fs::create_directories(c / "Users");
  fs::create_directories(c / "environments");
  put_file(c / "bruno.json", R"({"version":"1","name":"My API","type":"collection"})");
  put_file(c / "collection.bru",
           "headers {\n  X-Client: watt\n}\n\nauth {\n  mode: bearer\n}\n\nauth:bearer {\n  token: {{token}}\n}\n\n"
           "vars:pre-request {\n  apiVersion: v2\n  baseUrl: http://collection\n}\n");
  put_file(c / "Users" / "folder.bru", "meta {\n  name: People\n  seq: 1\n}\n\nvars:pre-request {\n  team: core\n}\n");
  put_file(c / "Users" / "create.bru", R"(meta {
  name: Create user
  type: http
  seq: 2
}

post {
  url: {{baseUrl}}/users?x=1
  body: json
  auth: inherit
}

params:query {
  x: 1
  ~debug: true
}

headers {
  ~X-Off: 1
}

body:json {
  {
    "name": "Ada"
  }
}

assert {
  res.status: eq 201
}

vars:post-response {
  userId: res.body.id
}

script:post-response {
  bru.setVar("x", 1);
}

tests {
  test("ok", function() {});
}
)");
  put_file(c / "Users" / "get.bru",
           "meta {\n  name: Get user\n  seq: 1\n}\n\nget {\n  url: {{baseUrl}}/users/:id\n  body: none\n  auth: basic\n}\n\n"
           "params:path {\n  id: 7\n}\n\nauth:basic {\n  username: u\n  password: p\n}\n");
  put_file(c / "environments" / "Local.bru", "vars {\n  baseUrl: http://localhost:3000\n  ~off: 1\n}\nvars:secret [\n  token\n]\n");
  std::string err;
  auto r = parse_collection_file(c, &err);
  INFO(err);
  REQUIRE(r);
  CHECK(r->format == "Bruno");
  CHECK(r->root.name == "My API");
  REQUIRE(r->root.folders.size() == 1);
  CHECK(r->root.folders[0].name == "People");
  REQUIRE(r->root.folders[0].requests.size() == 2);
  CHECK(r->root.folders[0].requests[0].name == "Get user");  // seq order
  const SavedRequest& cr = r->root.folders[0].requests[1];
  CHECK(cr.http.method == "POST");
  CHECK(cr.http.url == "{{baseUrl}}/users?x=1");
  CHECK(cr.disabled_params.size() == 1);
  CHECK(cr.http.body == "{\n  \"name\": \"Ada\"\n}");
  CHECK(header(cr, "Authorization") == "Bearer {{token}}");
  CHECK(header(cr, "X-Client") == "watt");
  CHECK(header(cr, "Content-Type") == "application/json");
  REQUIRE(cr.asserts.size() == 1);
  CHECK(cr.asserts[0].target == "res.status");
  CHECK(cr.asserts[0].op == "eq");
  CHECK(cr.asserts[0].value == "201");
  REQUIRE(cr.captures.size() == 1);
  CHECK(cr.captures[0].var == "userId");
  CHECK(cr.post_script.find("bru.setVar") != std::string::npos);
  CHECK(cr.post_script.find("test(\"ok\"") != std::string::npos);
  const SavedRequest& g = r->root.folders[0].requests[0];
  CHECK(g.basic_auth.enabled);
  CHECK(g.path_params == std::vector<std::pair<std::string, std::string>>{{"id", "7"}});
  CHECK(folder_var(r->root, "apiVersion") == "v2");
  CHECK(folder_var(r->root, "baseUrl") == "<none>");  // Local defines it: the environment keeps winning
  CHECK(folder_var(r->root.folders[0], "team") == "core");
  REQUIRE(r->envs.size() == 1);
  CHECK(r->envs[0].name == "Local");
  CHECK(r->envs[0].vars.at("baseUrl") == "http://localhost:3000");
  CHECK(r->envs[0].vars.count("off") == 0);
  CHECK(r->envs[0].secrets.count("token"));
}

TEST_CASE("import: a 2000-request Postman collection parses fast", "[sidebar][import]") {
  std::string coll = R"({"info":{"name":"Big","schema":"https://schema.getpostman.com/json/collection/v2.1.0/collection.json"},"item":[)";
  for (int f = 0; f < 20; ++f) {
    coll += std::string(f ? "," : "") + R"({"name":"Folder )" + std::to_string(f) + R"(","item":[)";
    for (int i = 0; i < 100; ++i)
      coll += std::string(i ? "," : "") + R"({"name":"Req )" + std::to_string(i) +
              R"(","request":{"method":"PATCH","url":{"raw":"{{base}}/items/)" + std::to_string(i) + R"("}}})";
    coll += "]}";
  }
  coll += "]}";
  const auto t0 = std::chrono::steady_clock::now();
  std::string err;
  auto r = parse_collection_text(coll, &err);
  REQUIRE(r);
  const double parse_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  CHECK(parse_ms < 200);
  TempWs t;
  const auto t1 = std::chrono::steady_clock::now();
  auto w = write_import(t.ws, t.dir, *r);
  const double write_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
  WARN("2000 requests written in " << write_ms << " ms");
  CHECK(w.requests == 2000);
  CHECK(w.folders == 21);
  // Every file is there once, in collection order.
  const fs::path f0 = w.top / "Folder 0";
  REQUIRE(w.order.count(f0));
  REQUIRE(w.order[f0].size() == 100);
  CHECK(w.order[f0][0] == L"req-0.req.json");
  CHECK(w.order[f0][99] == L"req-99.req.json");
  size_t files = 0;
  size_t orders = 0;
  for (fs::recursive_directory_iterator it(w.top), end; it != end; ++it) {
    files += it->path().filename().string().ends_with(".req.json");
    orders += it->path().filename() == "folder.json";
  }
  CHECK(files == 2000);
  CHECK(orders == 0);  // in natural name order already: no folder.json needed for it
}

TEST_CASE("import: YAML subset", "[sidebar][import]") {
  std::string err;
  auto j = yaml_to_json("a: 1\nb:\n  - x\n  - y: 2\n    z: 'it''s'\nc: \"q\\n\"\nd: {e: [1, 2], f: null}\ng: 007\n", &err);
  INFO(err);
  REQUIRE(j);
  CHECK(*j == R"({"a":1,"b":["x",{"y":2,"z":"it's"}],"c":"q\n","d":{"e":[1,2],"f":null},"g":"007"})");
  CHECK_FALSE(parse_collection_text("just: yaml\n", &err));
  CHECK_FALSE(parse_collection_text("{\"not\": \"a collection\"}", &err));
}

TEST_CASE("export: a folder as a Postman v2.1 collection imports back the same requests", "[sidebar][import]") {
  std::string err;
  auto r = parse_collection_text(kPostmanV21, &err);
  REQUIRE(r);
  TempWs t;
  auto w = write_import(t.ws, t.dir, *r);
  REQUIRE(w.requests == 4);
  const auto ex = export_postman(t.ws, w.top, "Shop API", &w.order);
  CHECK(ex.requests == 4);
  CHECK(ex.folders == 2);
  auto back = parse_collection_text(ex.text, &err);
  INFO(err);
  REQUIRE(back);
  CHECK(back->format == "Postman Collection v2.1");
  REQUIRE(back->root.folders.size() == 1);
  CHECK(back->root.folders[0].name == "Users");
  CHECK(back->root.requests.size() == 2);
  CHECK(back->root.requests[0].name == "Login (form)");  // collection order kept
  for (const char* name : {"Get user", "Create user", "Login (form)", "Upload avatar"}) {
    const SavedRequest* a = find_req(r->root, name);
    const SavedRequest* b = find_req(back->root, name);
    REQUIRE(a);
    REQUIRE(b);
    INFO(name);
    CHECK(b->http.method == a->http.method);
    CHECK(b->http.url == a->http.url);
    CHECK(b->http.headers == a->http.headers);
    CHECK(b->disabled_headers == a->disabled_headers);
    CHECK(b->disabled_params == a->disabled_params);
    CHECK(b->path_params == a->path_params);
    CHECK(b->http.body == a->http.body);
    CHECK(b->form == a->form);
    CHECK(b->basic_auth == a->basic_auth);
    CHECK(b->pre_script == a->pre_script);
    CHECK(b->post_script == a->post_script);
  }
  // As curl commands, each under its folder / name.
  const auto cu = export_curl(t.ws, w.top, CurlShell::Bash, &w.order);
  CHECK(cu.requests == 4);
  CHECK(cu.text.find("# Users/Get user\n") != std::string::npos);
  CHECK(cu.text.find("curl") != std::string::npos);
}

TEST_CASE("environment files: a UTF-8 BOM is read, broken JSON is reported", "[sidebar][env]") {
  TempWs t;
  fs::create_directories(t.dir / "environments");
  const std::string body = R"({"name":"local","seq":0,"vars":{"grpc":"127.0.0.1:50999"}})";
  std::ofstream(t.dir / "environments" / "ok.json", std::ios::binary) << R"({"name":"ok","vars":{}})";
  // Windows PowerShell 5 Set-Content -Encoding utf8 writes a BOM: the workspace reads it anyway.
  std::ofstream(t.dir / "environments" / "local.json", std::ios::binary) << "\xEF\xBB\xBF" << body;
  std::ofstream(t.dir / "environments" / "broken.json", std::ios::binary) << "{\"name\": ";
  auto bad = unreadable_env_files(t.dir);
  REQUIRE(bad.size() == 1);
  CHECK(bad[0].file.filename() == "broken.json");
  CHECK(!bad[0].bom);
  auto ws = Workspace::open(t.dir);
  REQUIRE(ws);
  bool found = false;
  for (const auto& e : ws->environments())
    if (e.name == "local") found = e.vars.count("grpc") && e.vars.at("grpc") == "127.0.0.1:50999";
  CHECK(found);
  // remove_utf8_bom drops only the mark.
  REQUIRE(remove_utf8_bom(t.dir / "environments" / "local.json", nullptr));
  std::ifstream in(t.dir / "environments" / "local.json", std::ios::binary);
  const std::string after((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(after == body);
  CHECK(remove_utf8_bom(t.dir / "environments" / "ok.json", nullptr));
}

// ---------------------------------------------------------------- cookie manager text helpers
#include "app/sidebar_cookies_text.cpp"

TEST_CASE("cookies: expiry parsing and labels", "[sidebar][cookies]") {
  const int64_t now = 1795000000;  // 2026-11-18 ...
  int64_t t = -1;
  CHECK(parse_cookie_expiry("", now, t));
  CHECK(t == 0);
  CHECK(parse_cookie_expiry("  Session ", now, t));
  CHECK(t == 0);
  CHECK(parse_cookie_expiry("2026-12-31", now, t));
  CHECK(t == 1798675200);
  CHECK(parse_cookie_expiry("2026-12-31 23:59", now, t));
  CHECK(t == 1798675200 + 23 * 3600 + 59 * 60);
  CHECK(parse_cookie_expiry("2026-12-31T23:59:05Z", now, t));
  CHECK(t == 1798675200 + 23 * 3600 + 59 * 60 + 5);
  CHECK(parse_cookie_expiry("Thu, 31 Dec 2026 23:59:05 GMT", now, t));
  CHECK(t == 1798675200 + 23 * 3600 + 59 * 60 + 5);
  CHECK(parse_cookie_expiry("+2h", now, t));
  CHECK(t == now + 7200);
  CHECK(parse_cookie_expiry("+7d", now, t));
  CHECK(t == now + 7 * 86400);
  CHECK_FALSE(parse_cookie_expiry("tomorrow", now, t));
  CHECK_FALSE(parse_cookie_expiry("2026-13-01", now, t));
  CHECK_FALSE(parse_cookie_expiry("+h", now, t));
  CHECK_FALSE(parse_cookie_expiry("+5w", now, t));

  Cookie c;
  c.name = "sid";
  c.value = "abc";
  c.domain = "api.example.com";
  CHECK(cookie_expiry_label(c, now) == "Session");
  CHECK(cookie_expiry_edit_text(c).empty());
  c.expires = 1798675200 + 23 * 3600 + 59 * 60 + 5;
  CHECK(cookie_expiry_edit_text(c) == "2026-12-31 23:59:05");
  CHECK(cookie_expiry_label(c, now).rfind("2026-12-31 23:59 UTC (in ", 0) == 0);
  CHECK(cookie_expiry_label(c, c.expires + 1).find("(expired)") != std::string::npos);
  // The edit text parses back to the same instant.
  CHECK(parse_cookie_expiry(cookie_expiry_edit_text(c), now, t));
  CHECK(t == c.expires);
}

TEST_CASE("cookies: Set-Cookie line, flags and grouping", "[sidebar][cookies]") {
  Cookie c;
  c.name = "sid";
  c.value = "abc";
  c.domain = "example.com";
  c.path = "/api";
  c.host_only = false;
  c.secure = true;
  c.http_only = true;
  c.same_site = SameSite::Lax;
  c.expires = 1798761599;  // Thu, 31 Dec 2026 23:59:59 GMT
  CHECK(set_cookie_line(c) ==
        "sid=abc; Domain=example.com; Path=/api; Expires=Thu, 31 Dec 2026 23:59:59 GMT; Secure; HttpOnly; SameSite=Lax");
  CHECK(cookie_flags_label(c) == "Secure  HttpOnly  SameSite=Lax");
  // The line goes through the jar's own rules and comes back as the same cookie.
  CookieJar jar;
  REQUIRE(jar.set_from_header(cookie_origin_url("example.com"), set_cookie_line(c), nullptr, 1795000000));
  const auto got = jar.list(1795000000);
  REQUIRE(got.size() == 1);
  CHECK(got[0].name == "sid");
  CHECK(got[0].path == "/api");
  CHECK(got[0].expires == c.expires);
  CHECK(got[0].secure);
  CHECK(got[0].http_only);
  CHECK_FALSE(got[0].host_only);
  CHECK(got[0].same_site == SameSite::Lax);

  CHECK(cookie_origin_url(".Example.COM") == "https://example.com/");
  CHECK(cookie_origin_url("") == "");
  CHECK(cookie_origin_url("evil.com/path") == "");
  CHECK(cookie_origin_url("a b") == "");

  std::vector<Cookie> list(3);
  list[0].domain = "a.test";
  list[0].name = "x";
  list[1].domain = "b.test";
  list[1].name = "Token";
  list[1].value = "zzz";
  list[2].domain = "a.test";
  list[2].name = "y";
  auto g = group_cookies(list, "");
  REQUIRE(g.size() == 2);
  CHECK(g[0].domain == "a.test");
  CHECK(g[0].cookies == std::vector<size_t>{0, 2});
  g = group_cookies(list, "token");
  REQUIRE(g.size() == 1);
  CHECK(g[0].domain == "b.test");
  g = group_cookies(list, " ZZ ");
  REQUIRE(g.size() == 1);
  g = group_cookies(list, "A.TE");
  REQUIRE(g.size() == 1);
  CHECK(g[0].cookies.size() == 2);
}

TEST_CASE("history: snapshot origins round-trip and stay inside the workspace", "[sidebar][history]") {
  TempWs t;
  std::map<std::string, std::string> m;
  m["0123456789abcdef.req.json"] = "api/users/Get user.req.json";
  m["fedcba9876543210.req.json"] = "api/(unsaved).req.json";
  m["1111111111111111.req.json"] = "../outside.req.json";
  m["2222222222222222.req.json"] = "C:/Windows/x.req.json";
  m["3333333333333333.req.json"] = ".watt/snapshots/x.req.json";
  m["4444444444444444.req.json"] = "/abs.req.json";
  m["..\\evil.req.json"] = "api/x.req.json";
  REQUIRE(save_snapshot_origins(t.dir, m));
  const auto back = load_snapshot_origins(t.dir);
  CHECK(back.size() == 2);
  CHECK(back.at("0123456789abcdef.req.json") == "api/users/Get user.req.json");
  CHECK(back.at("fedcba9876543210.req.json") == "api/(unsaved).req.json");
  CHECK(fs::exists(t.dir / ".watt" / "snapshots" / "origins.json"));
  // A planted file with hostile entries: only the safe ones are read.
  {
    std::ofstream out(t.dir / ".watt" / "snapshots" / "origins.json", std::ios::binary | std::ios::trunc);
    out << R"({"version":1,"origins":{"aaaaaaaaaaaaaaaa.req.json":"a/b.req.json","bbbbbbbbbbbbbbbb.req.json":"a/../../x","c":5}})";
  }
  const auto planted = load_snapshot_origins(t.dir);
  CHECK(planted.size() == 1);
  CHECK(planted.count("aaaaaaaaaaaaaaaa.req.json") == 1);
  CHECK(safe_relative_origin("a/b/c.req.json"));
  CHECK_FALSE(safe_relative_origin("a//b"));
  CHECK_FALSE(safe_relative_origin("\\\\server\\share\\x"));
  CHECK_FALSE(safe_relative_origin(".git/config"));
}

// Folder settings saved from the sidebar (the dialog writes through a copy of the workspace on a
// worker): the next send from an open tab resolves the new folder variable at once, also when
// the file keeps its size (":18915" -> ":18916") and is rewritten within the same clock tick.
TEST_CASE("folder settings: a saved folder variable is what the next send uses", "[sidebar][folders]") {
  TempWs t;
  const fs::path folder = t.dir / "petstore";
  fs::create_directories(folder);
  SavedRequest r = saved(t.ws, folder / "get-pet.req.json", "Get pet");
  r.http.url = "{{baseUrl}}/json/1";
  REQUIRE(t.ws.save_request(r));
  auto set_base = [&](const std::string& v) {
    FolderSettings s = t.ws.folder_settings(folder).value_or(FolderSettings{});
    s.vars = {SettingsRow{"baseUrl", v, "", true}};
    const Workspace copy(t.ws);  // as edit_folder_settings saves it
    std::string err;
    REQUIRE(copy.save_folder_settings(folder, s, &err));
  };
  set_base("http://127.0.0.1:18915");
  CHECK(t.ws.expanded(r).http.url == "http://127.0.0.1:18915/json/1");  // read once: now cached
  set_base("http://127.0.0.1:18916");
  CHECK(t.ws.expanded(r).http.url == "http://127.0.0.1:18916/json/1");
  set_base("http://localhost:18916");
  CHECK(t.ws.expanded(r).http.url == "http://localhost:18916/json/1");
  const EffectiveSettings eff = t.ws.effective_settings(r);
  bool found = false;
  for (const auto& v : eff.vars)
    if (v.row.name == "baseUrl" && !v.overridden) found = v.row.value == "http://localhost:18916";
  CHECK(found);
}

TEST_CASE("import: variables the requests use but nothing defines") {
  ImportResult r;
  r.format = "Postman Collection v2.1";
  r.root.name = "Petstore";
  r.root.vars.push_back({"baseUrl", "http://127.0.0.1:18917", "", true});
  r.root.vars.push_back({"off", "1", "", false});  // disabled: does not define
  SavedRequest a;
  a.http.url = "{{baseUrl}}/json/{{ id }}?t={{$timestamp}}&e={{process.env.HOME}}";
  a.http.headers.push_back({"X-Trace", "{{trace}}"});
  a.http.headers.push_back({"X-Off", "{{disabledOnly}}"});
  a.disabled_headers.insert(1);
  a.http.body = "{\"o\":\"{{off}}\",\"g\":\"{{fromGlobals}}\"}";
  r.root.requests.push_back(a);
  ImportFolder sub;
  sub.name = "users";
  sub.vars.push_back({"id", "7", "", true});  // only defined below this folder
  SavedRequest b;
  b.http.url = "{{baseUrl}}/users/{{id}}";
  b.basic_auth.user = "{{envUser}}";
  sub.requests.push_back(b);
  r.root.folders.push_back(sub);
  Environment e;
  e.name = "dev";
  e.vars["envUser"] = "u";
  r.envs.push_back(e);
  const auto u = undefined_import_vars(r, {"fromGlobals"});
  CHECK(u == std::vector<std::string>{"id", "trace", "off"});
}

TEST_CASE("import: Postman folder auth becomes the folder's setting, inherited by its requests", "[sidebar][import]") {
  const std::string pm = R"PM({
  "info": { "name": "Shop", "schema": "https://schema.getpostman.com/json/collection/v2.1.0/collection.json" },
  "item": [
    { "name": "users",
      "auth": { "type": "bearer", "bearer": [ { "key": "token", "value": "{{token}}" } ] },
      "item": [
        { "name": "List users", "request": { "method": "GET", "url": "{{baseUrl}}/users" } },
        { "name": "Get user", "request": { "method": "GET", "url": "{{baseUrl}}/users/1", "auth": { "type": "inherit" } } }
      ] },
    { "name": "orders",
      "auth": { "type": "bearer", "bearer": [ { "key": "token", "value": "{{orderToken}}" } ] },
      "item": [
        { "name": "List orders", "request": { "method": "GET", "url": "{{baseUrl}}/orders" } },
        { "name": "Public order", "request": { "method": "GET", "url": "{{baseUrl}}/orders/p", "auth": { "type": "noauth" } } }
      ] }
  ]
})PM";
  std::string err;
  auto r = parse_collection_text(pm, &err);
  REQUIRE(r);
  REQUIRE(r->root.folders.size() == 2);
  const ImportFolder& users = r->root.folders[0];
  REQUIRE(users.headers.size() == 1);
  CHECK(users.headers[0].name == "Authorization");
  CHECK(users.headers[0].value == "Bearer {{token}}");
  const SavedRequest* list = find_req(r->root, "List users");
  REQUIRE(list);
  CHECK(header(*list, "Authorization") == "<none>");  // not a copy: inherited
  CHECK(list->auth.type == HttpAuth::Type::Inherit);
  // A request under "orders" says "no auth": the folder's token stays on each other request.
  const ImportFolder& orders = r->root.folders[1];
  CHECK(orders.headers.empty());
  const SavedRequest* lo = find_req(r->root, "List orders");
  REQUIRE(lo);
  CHECK(header(*lo, "Authorization") == "Bearer {{orderToken}}");
  const SavedRequest* pub = find_req(r->root, "Public order");
  REQUIRE(pub);
  CHECK(header(*pub, "Authorization") == "<none>");

  // Written: users/folder.json holds the header; a request there resolves with it, and a later
  // change of the folder's token reaches every request.
  TempWs t;
  auto w = write_import(t.ws, t.dir, *r);
  CHECK(w.requests == 4);
  const fs::path ufolder = w.top / "users";
  auto s = t.ws.folder_settings(ufolder);
  REQUIRE(s);
  REQUIRE(s->headers.size() == 1);
  CHECK(s->headers[0].value == "Bearer {{token}}");
  auto back = t.ws.load_request(ufolder / "list-users.req.json");
  REQUIRE(back);
  CHECK(header(t.ws.resolved(*back), "Authorization") == "Bearer {{token}}");
  s->headers[0].value = "Bearer {{other}}";
  REQUIRE(t.ws.save_folder_settings(ufolder, *s, &err));
  Workspace again = *Workspace::open(t.dir, &err);
  auto back2 = again.load_request(ufolder / "get-user.req.json");
  REQUIRE(back2);
  CHECK(header(again.resolved(*back2), "Authorization") == "Bearer {{other}}");
}

TEST_CASE("quick open: a number in the query matches unbroken", "[sidebar][fuzzy]") {
  FuzzyQuery q("2 svc150");
  int s = 0;
  const std::string scattered = "service-152/delete resource 0 of svc152";
  CHECK_FALSE(q.match(scattered, 12, &s, nullptr));  // 1,5 from "152", 0 from "resource 0"
  CHECK(FuzzyQuery("2 svc152").match("service-152/delete resource 2 of svc152", 12, &s, nullptr));
  CHECK(FuzzyQuery("svc150").match("service-150/delete resource 5 of svc150", 12, &s, nullptr));
  CHECK(FuzzyQuery("ep0777").match("group 7/Endpoint 0777", 8, &s, nullptr));
  CHECK(numbers_present("v2", "get order v2"));
  CHECK_FALSE(numbers_present("150", "svc152 0"));
  CHECK(numbers_present("a1b2", "x"));  // single digits may still be scattered
}

TEST_CASE("filter: a numeric URL host and its port are not searched", "[sidebar][fuzzy]") {
  CHECK(url_search_text("http://127.0.0.1:18080/echo") == "http:///echo");
  CHECK(url_search_text("https://[::1]:8443/v1/users?id=7") == "https:///v1/users?id=7");
  CHECK(url_search_text("http://localhost:8080/json/7") == "http://localhost/json/7");
  CHECK(url_search_text("https://api.github.com/repos") == "https://api.github.com/repos");
  CHECK(url_search_text("{{baseUrl}}/orders/7") == "{{baseUrl}}/orders/7");
  CHECK(url_search_text("http://{{host}}:{{port}}/x") == "http://{{host}}:{{port}}/x");
  CHECK(url_search_text("127.0.0.1:8080/items") == "/items");
  CHECK(url_search_text("users/42") == "users/42");
  CHECK(url_search_text("42/items") == "42/items");
  CHECK(url_search_text("") == "");
  // "7" no longer matches every request on 127.0.0.1, but still one with 7 in its path.
  CHECK(url_search_text("http://127.0.0.1:18080/echo").find('7') == std::string::npos);
  CHECK(url_search_text("http://127.0.0.1:18080/json/7").find('7') != std::string::npos);
}

// The critic's restart case at the model level: a request whose host, port and Authorization
// come only from folder.json, saved, then the workspace opened again from disk (a relaunch: no
// state carried over) and sent.
TEST_CASE("restart: a request whose URL host comes only from folder.json is sent there", "[sidebar][folders]") {
  static httpserver::Server srv;
  static const bool up = srv.start(0);
  REQUIRE(up);
  TempWs t;
  const fs::path api = t.dir / "Shop API", orders = api / "orders";
  fs::create_directories(orders);
  {
    FolderSettings top;
    top.vars = {SettingsRow{"baseUrl", "http://127.0.0.1:" + std::to_string(srv.port()), "", true},
                SettingsRow{"token", "dev-token-123", "", true}};
    REQUIRE(t.ws.save_folder_settings(api, top));
    FolderSettings o;
    o.headers = {SettingsRow{"Authorization", "Bearer {{token}}", "", true}};
    REQUIRE(t.ws.save_folder_settings(orders, o));
    SavedRequest r = saved(t.ws, orders / "create-order.req.json", "Create order");
    r.http.method = "POST";
    r.http.url = "{{baseUrl}}/echo";
    r.http.body = "{\"items\":[1,2]}";
    REQUIRE(t.ws.save_request(r));
  }
  // Relaunch: a fresh Workspace from disk.
  std::string err;
  auto ws = Workspace::open(t.dir, &err);
  REQUIRE(ws);
  auto r = ws->load_request(orders / "create-order.req.json", &err);
  REQUIRE(r);
  const EffectiveSettings eff = ws->effective_settings(*r);
  bool base = false, header = false;
  for (const auto& v : eff.vars) base = base || v.row.name == "baseUrl";
  for (const auto& h : eff.headers) header = header || h.row.name == "Authorization";
  CHECK(base);
  CHECK(header);
  auto req = ws->http_to_send(*r, &err);
  REQUIRE(req);
  CHECK(req->url == "http://127.0.0.1:" + std::to_string(srv.port()) + "/echo");
  HttpClient c;
  const HttpResponse resp = c.send(*req);
  REQUIRE(resp.ok);
  CHECK(resp.status == 200);
  CHECK(std::string(resp.body).find("Bearer dev-token-123") != std::string::npos);
  CHECK(std::string(resp.body).find("POST") != std::string::npos);
}

namespace {
// Requests whose shown name is still the file slug ("req-7" for "req-7.req.json").
size_t slug_named(const CollectionNode& n) {
  size_t k = 0;
  for (const auto& c : n.children) {
    if (c.is_folder) {
      k += slug_named(c);
      continue;
    }
    std::string stem = c.path.filename().string();
    if (stem.ends_with(".req.json")) stem.resize(stem.size() - 9);
    k += c.name == stem;
  }
  return k;
}
}  // namespace

TEST_CASE("names: overlapping scans after a 2000-request import end with the request names", "[sidebar][names]") {
  std::string coll = R"({"info":{"name":"Big","schema":"https://schema.getpostman.com/json/collection/v2.1.0/collection.json"},"item":[)";
  for (int f = 0; f < 20; ++f) {
    coll += std::string(f ? "," : "") + R"({"name":"resource)" + std::to_string(f) + R"(","item":[)";
    for (int i = 0; i < 100; ++i)
      coll += std::string(i ? "," : "") + R"({"name":"resource)" + std::to_string(f) + " op" + std::to_string(i) +
              R"( update","request":{"method":"PATCH","url":{"raw":"{{base}}/items/)" + std::to_string(i) + R"("}}})";
    coll += "]}";
  }
  coll += "]}";
  std::string err;
  auto r = parse_collection_text(coll, &err);
  REQUIRE(r);
  TempWs t;
  REQUIRE(write_import(t.ws, t.dir, *r).requests == 2000);
  NameCache cache(t.dir);

  // Scan B (the newer one, the only one whose deliveries the sidebar applies) reads the empty
  // cache, then waits inside its first delivery while scan A reads every file and saves the
  // cache. B's own refresh then finds nothing new in the cache.
  std::promise<void> b_delivering, a_done;
  auto a_done_f = a_done.get_future();
  CollectionNode b_last;
  size_t b_deliveries = 0, b_first_slugs = 0;
  std::thread b([&] {
    scan_tree(t.ws, &cache, [&](CollectionNode node, MetaMap, bool) {
      if (b_deliveries++ == 0) {
        b_first_slugs = slug_named(node);
        b_delivering.set_value();
        a_done_f.wait();
      }
      b_last = std::move(node);
    });
  });
  b_delivering.get_future().wait();
  scan_tree(t.ws, &cache, [](CollectionNode, MetaMap, bool) {});
  a_done.set_value();
  b.join();
  CHECK(b_first_slugs == 2000);  // the race really happened: B first showed the slugs
  CHECK(b_deliveries >= 2);
  CHECK(slug_named(b_last) == 0);

  // A later scan with the saved cache shows the names at once, ids included (no file re-read).
  NameCache again(t.dir);
  CollectionNode tree = t.ws.tree(-1);
  MetaMap m = again.apply_cached(tree);
  CHECK(m.size() == 2000);
  CHECK(slug_named(tree) == 0);
  CHECK(std::all_of(m.begin(), m.end(), [](const auto& kv) { return !kv.second.id.empty(); }));
}

// The critic's folder-auth case: Basic auth set on a folder after the fact reaches a request the
// sidebar creates there and a Postman request imported without its own auth (both start on
// Inherit, as in Postman / Insomnia / Bruno); only an explicit "noauth" stays without auth.
TEST_CASE("folder auth: Basic on a folder reaches sidebar-created and imported requests", "[sidebar][folders][auth]") {
  static httpserver::Server srv;
  static const bool up = srv.start(0);
  REQUIRE(up);
  const std::string base = "http://127.0.0.1:" + std::to_string(srv.port());
  const std::string pm = R"PM({
  "info": { "name": "Shop", "schema": "https://schema.getpostman.com/json/collection/v2.1.0/collection.json" },
  "item": [
    { "name": "users", "item": [
        { "name": "List users", "request": { "method": "GET", "url": ")PM" + base + R"PM(/echo" } },
        { "name": "Public", "request": { "method": "GET", "url": ")PM" + base + R"PM(/echo", "auth": { "type": "noauth" } } }
      ] }
  ]
})PM";
  std::string err;
  auto imp = parse_collection_text(pm, &err);
  REQUIRE(imp);
  const SavedRequest* list = find_req(imp->root, "List users");
  REQUIRE(list);
  CHECK(list->auth.type == HttpAuth::Type::Inherit);
  const SavedRequest* pub = find_req(imp->root, "Public");
  REQUIRE(pub);
  CHECK(pub->auth.type == HttpAuth::Type::None);

  TempWs t;
  auto w = write_import(t.ws, t.dir, *imp);
  REQUIRE(w.requests == 2);
  const fs::path users = w.top / "users";
  FolderSettings fs_users;
  if (auto s = t.ws.folder_settings(users)) fs_users = *s;
  fs_users.auth.type = HttpAuth::Type::None;  // None + basic_auth.enabled: Basic at this level
  fs_users.basic_auth = BasicAuth{true, "alice", "pw"};
  REQUIRE(t.ws.save_folder_settings(users, fs_users, &err));

  auto created = create_request(t.ws, users, RequestKind::Http, "New request", &err);
  REQUIRE(created);
  CHECK(created->auth.type == HttpAuth::Type::Inherit);
  for (RequestKind k : {RequestKind::Grpc, RequestKind::WebSocket}) {
    auto other = create_request(t.ws, users, k, "Other", &err);
    REQUIRE(other);
    CHECK(other->auth.type == HttpAuth::Type::Inherit);
  }
  created->http.url = base + "/echo";
  REQUIRE(t.ws.save_request(*created, &err));

  Workspace ws = *Workspace::open(t.dir, &err);  // as after a restart: everything from disk
  HttpClient c;
  auto sent_auth = [&](const fs::path& file, bool expect_basic) {
    INFO(file.string());
    auto r = ws.load_request(file, &err);
    REQUIRE(r);
    const EffectiveSettings eff = ws.effective_settings(*r);
    CHECK(eff.basic_auth.enabled == expect_basic);
    if (expect_basic) {
      CHECK(eff.basic_auth.user == "alice");
      CHECK(eff.auth_from.level == SettingsOrigin::Level::Folder);
    }
    auto req = ws.http_to_send(*r, &err);
    REQUIRE(req);
    const HttpResponse resp = c.send(*req);
    REQUIRE(resp.ok);
    const bool has = std::string(resp.body).find("Basic YWxpY2U6cHc=") != std::string::npos;  // alice:pw
    CHECK(has == expect_basic);
  };
  sent_auth(created->file, true);
  sent_auth(users / "list-users.req.json", true);
  sent_auth(users / "public.req.json", false);
}

TEST_CASE("import: Insomnia and Bruno requests without auth inherit", "[sidebar][import][auth]") {
  const std::string ins = R"({"_type":"export","__export_format":4,"resources":[
    {"_id":"wrk_1","name":"W","_type":"workspace"},
    {"_id":"fld_1","parentId":"wrk_1","name":"users","_type":"request_group"},
    {"_id":"req_1","parentId":"fld_1","name":"List","method":"GET","url":"http://127.0.0.1:1/x","_type":"request"},
    {"_id":"req_2","parentId":"fld_1","name":"Own","method":"GET","url":"http://127.0.0.1:1/y",
     "authentication":{"type":"bearer","token":"t"},"_type":"request"}]})";
  std::string err;
  auto r = parse_collection_text(ins, &err);
  REQUIRE(r);
  const SavedRequest* l = find_req(r->root, "List");
  REQUIRE(l);
  CHECK(l->auth.type == HttpAuth::Type::Inherit);
  const SavedRequest* o = find_req(r->root, "Own");
  REQUIRE(o);
  CHECK(o->auth.type == HttpAuth::Type::None);
  CHECK(header(*o, "Authorization") == "Bearer t");
}

namespace {
void write_file(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << text;
}

const char* kSpecV1 = R"(openapi: 3.0.3
info:
  title: Shop API
  version: "1.0"
servers:
  - url: https://shop.example.com
paths:
  /orders:
    get:
      tags: [orders]
      summary: List orders
      responses: { '200': { description: OK } }
    post:
      tags: [orders]
      summary: Create order
      responses: { '201': { description: Created } }
  /orders/{id}:
    get:
      tags: [orders]
      summary: Get order
      parameters:
        - { name: id, in: path, required: true, schema: { type: string } }
      responses: { '200': { description: OK } }
)";
}  // namespace

TEST_CASE("specs: peek finds OpenAPI / Swagger, title and version; scan lists specs/ only", "[sidebar][specs]") {
  TempWs t;
  write_file(t.dir / "specs" / "shop.yaml", kSpecV1);
  write_file(t.dir / "specs" / "pets.json",
             R"({"swagger": "2.0", "info": {"version": "1", "title": "Pet \"Store\""}, "paths": {}})");
  write_file(t.dir / "specs" / "package.json", R"({"name": "not a spec"})");
  write_file(t.dir / "specs" / "notes.txt", "openapi: 3.0.0\n");
  const SpecInfo y = peek_spec(t.dir / "specs" / "shop.yaml");
  CHECK(y.kind == "OpenAPI 3.0.3");
  CHECK(y.version == "3.0.3");
  CHECK(y.title == "Shop API");
  const SpecInfo j = peek_spec(t.dir / "specs" / "pets.json");
  CHECK(j.kind == "Swagger 2.0");
  CHECK(j.title == "Pet \"Store\"");
  CHECK(peek_spec(t.dir / "specs" / "package.json").kind.empty());
  CHECK(peek_spec(t.dir / "nope.yaml").missing);

  const fs::path outside = t.dir / "elsewhere.yaml";
  const auto list = scan_specs(t.dir, {t.dir / "specs" / "shop.yaml", outside});
  REQUIRE(list.size() == 3);  // pets.json, shop.yaml, then the recorded (missing) one; no duplicate
  CHECK(list[0].file.filename() == "pets.json");
  CHECK(list[1].file.filename() == "shop.yaml");
  CHECK(!list[1].imported);
  CHECK(list[2].imported);
  CHECK(list[2].missing);
}

TEST_CASE("specs: sync adds new operations and keeps edited requests", "[sidebar][specs][import]") {
  TempWs t;
  std::string err;
  auto r1 = parse_collection_text(kSpecV1, &err);
  REQUIRE(r1);
  ImportWritten w = write_import(t.ws, t.dir, *r1);
  REQUIRE(w.requests == 3);
  const fs::path top = w.top;
  CHECK(top.filename() == "Shop API");
  // The user edits one request (a header and a new name) and moves another into a sub-folder.
  fs::path edited;
  for (const auto& e : fs::recursive_directory_iterator(top))
    if (e.path().filename() == "list-orders.req.json") edited = e.path();
  REQUIRE(!edited.empty());
  auto req = t.ws.load_request(edited);
  REQUIRE(req);
  req->name = "All orders (mine)";
  req->http.headers.push_back({"X-Mine", "1"});
  REQUIRE(t.ws.save_request(*req));
  // Same operations again plus one new one and one removed (POST /orders).
  std::string v2 = kSpecV1;
  const size_t post = v2.find("    post:");
  const size_t post_end = v2.find("  /orders/{id}:");
  v2.erase(post, post_end - post);
  v2 += R"(  /orders/{id}/cancel:
    post:
      tags: [orders]
      summary: Cancel order
      responses: { '200': { description: OK } }
)";
  auto r2 = parse_collection_text(v2, &err);
  REQUIRE(r2);
  SyncWritten s = sync_import(t.ws, top, *r2);
  CHECK(s.added == 1);
  CHECK(s.kept == 2);
  REQUIRE(s.stale.size() == 1);
  CHECK(s.stale[0].rfind("POST ", 0) == 0);
  CHECK(s.first_added.filename() == "cancel-order.req.json");
  CHECK(request_files(top) == 4);  // nothing deleted
  auto after = t.ws.load_request(edited);
  REQUIRE(after);
  CHECK(after->name == "All orders (mine)");
  REQUIRE(!after->http.headers.empty());
  CHECK(after->http.headers.back().first == "X-Mine");
  // A second sync of the same document adds nothing.
  auto r3 = parse_collection_text(v2, &err);
  REQUIRE(r3);
  SyncWritten s2 = sync_import(t.ws, top, *r3);
  CHECK(s2.added == 0);
  CHECK(s2.kept == 3);
  // Never outside the workspace.
  auto r4 = parse_collection_text(v2, &err);
  SyncWritten bad = sync_import(t.ws, t.dir.parent_path(), *r4);
  CHECK(bad.added == 0);
  CHECK(!bad.skipped.empty());
}

TEST_CASE("specs: an operation row finds the request synced from it", "[sidebar][specs]") {
  // What a double-click on an operation of the spec preview matches against request URLs.
  CHECK(spec_op_path("{{base}}/pets/:petId?x=1") == "/pets/{petId}");
  CHECK(spec_op_path("{{baseUrl}}/pets/{{petId}}") == "/pets/{petId}");
  CHECK(spec_op_path("https://api.example.com/v1/orders/") == "/v1/orders");
  CHECK(spec_op_path("http://127.0.0.1:8080") == "/");
  CHECK(spec_op_path("{{base}}") == "/");
  CHECK(spec_op_path("/orders/{id}#frag") == "/orders/{id}");
  // Every operation of a document, once imported, maps back to its own path.
  TempWs t;
  std::string err;
  auto parsed = parse_collection_text(kSpecV1, &err);
  REQUIRE(parsed);
  const fs::path top = t.dir / "Shop API";
  sync_import(t.ws, top, *parsed);
  std::set<std::string> paths;
  for (fs::recursive_directory_iterator it(top), end; it != end; ++it)
    if (it->path().string().ends_with(".req.json"))
      if (auto r = t.ws.load_request(it->path())) paths.insert(r->http.method + " " + spec_op_path(r->http.url));
  CHECK(paths == std::set<std::string>{"GET /orders", "POST /orders", "GET /orders/{id}"});
}

TEST_CASE("specs: a spec tab opened in one workspace never syncs into another", "[sidebar][specs][workspace]") {
  TempWs ws1, ws2;
  write_file(ws2.dir / "specs" / "petstore.yaml", kSpecV1);
  const fs::path spec = ws2.dir / "specs" / "petstore.yaml";
  // Its own workspace: allowed.
  CHECK(spec_sync_allowed(spec, ws2.dir, ws2.dir, false));
  CHECK(spec_sync_allowed(spec, {}, ws2.dir, false));
  // The tab was opened in ws2 and ws1 is open now (the critic's repro): refused, whatever is
  // recorded, and ws1 gets no file.
  CHECK_FALSE(spec_sync_allowed(spec, ws2.dir, ws1.dir, false));
  CHECK_FALSE(spec_sync_allowed(spec, ws2.dir, ws1.dir, true));
  // A spec of another workspace that ws1 never recorded: refused even without a tab root.
  CHECK_FALSE(spec_sync_allowed(spec, {}, ws1.dir, false));
  CHECK(spec_sync_allowed(fs::path("C:/elsewhere/api.yaml"), ws1.dir, ws1.dir, true));  // recorded import
  const size_t before = request_files(ws1.dir);
  if (spec_sync_allowed(spec, ws2.dir, ws1.dir, false)) {  // what the sidebar's sync path does
    std::string err;
    auto parsed = parse_collection_file(spec, &err);
    REQUIRE(parsed);
    write_import(ws1.ws, ws1.dir, *parsed);
  }
  CHECK(request_files(ws1.dir) == before);
  CHECK_FALSE(fs::exists(ws1.dir / "Shop API"));

  // Mock rows: only folders under the open workspace.
  CHECK(in_workspace(ws1.dir, ws1.dir));
  CHECK(in_workspace(ws1.dir / "Pet Store", ws1.dir));
  CHECK_FALSE(in_workspace(ws2.dir / "Pet Store", ws1.dir));
  CHECK_FALSE(in_workspace(fs::path(ws1.dir.native() + L"0") / "x", ws1.dir));  // ws1 vs ws10
  CHECK_FALSE(in_workspace(ws1.dir / ".." / ws2.dir.filename() / "Pet Store", ws1.dir));
  CHECK(in_workspace(fs::path(ws1.dir.generic_wstring() + L"/A/b"), ws1.dir));
  CHECK_FALSE(in_workspace({}, ws1.dir));
}

// The collection's order is kept in the workspace (folder.json "order"), not on this machine:
// a copy (a git clone, a teammate, a moved folder) lists it as imported, and so does every
// reader of Workspace::tree() (the runner flattens it depth-first).
TEST_CASE("import order is stored in the workspace and survives a copy", "[sidebar][order]") {
  const std::string coll = R"({
    "info": { "name": "Shop API", "schema": "https://schema.getpostman.com/json/collection/v2.1.0/collection.json" },
    "item": [
      { "name": "auth", "item": [ { "name": "Login", "request": { "method": "POST", "url": "https://x.test/login" } } ] },
      { "name": "users", "item": [ { "name": "List users", "request": { "method": "GET", "url": "https://x.test/users" } } ] },
      { "name": "orders", "item": [
        { "name": "List orders", "request": { "method": "GET", "url": "https://x.test/orders" } },
        { "name": "Get order", "request": { "method": "GET", "url": "https://x.test/orders/1" } },
        { "name": "Create order", "request": { "method": "POST", "url": "https://x.test/orders" } },
        { "name": "Cancel order", "request": { "method": "POST", "url": "https://x.test/orders/1/cancel" } },
        { "name": "Refund order", "request": { "method": "POST", "url": "https://x.test/orders/1/refund" } },
        { "name": "Ship order", "request": { "method": "POST", "url": "https://x.test/orders/1/ship" } } ] },
      { "name": "catalog", "item": [ { "name": "Products", "request": { "method": "GET", "url": "https://x.test/p" } } ] }
    ] })";
  std::string err;
  auto r = parse_collection_text(coll, &err);
  REQUIRE(r);
  TempWs t;
  auto w = write_import(t.ws, t.dir, *r);
  REQUIRE(w.requests == 9);
  CHECK(w.skipped.empty());
  const fs::path top = t.dir / "Shop API";
  CHECK(fs::exists(top / "folder.json"));
  CHECK(t.ws.folder_order(top) == std::vector<std::string>{"auth", "users", "orders", "catalog"});
  // Settings saved later keep the order; the order alone is not inherited settings.
  FolderSettings fs_;
  fs_.headers.push_back(SettingsRow{"X-Team", "core"});
  REQUIRE(t.ws.save_folder_settings(top / "orders", fs_));
  CHECK(t.ws.folder_order(top / "orders").size() == 6);
  REQUIRE(t.ws.save_folder_settings(top / "orders", FolderSettings{}));
  CHECK(fs::exists(top / "orders" / "folder.json"));
  CHECK(t.ws.folder_order(top / "orders").size() == 6);
  SavedRequest probe;
  probe.file = top / "users" / "list-users.req.json";
  CHECK_FALSE(t.ws.has_inherited_settings(probe));

  // A copy elsewhere, opened fresh.
  const fs::path dir2 = t.dir.parent_path() / (t.dir.filename().string() + "_copy");
  fs::remove_all(dir2);
  fs::copy(t.dir, dir2, fs::copy_options::recursive);
  std::error_code ec;
  fs::remove_all(dir2 / ".watt", ec);  // what git does not carry
  {
    auto ws2 = Workspace::open(dir2);
    REQUIRE(ws2);
    const CollectionNode root = ws2->tree(-1);
    const CollectionNode* shop = nullptr;
    for (const auto& c : root.children)
      if (c.name == "Shop API") shop = &c;
    REQUIRE(shop);
    std::vector<std::string> folders;
    for (const auto& c : shop->children) folders.push_back(c.name);
    CHECK(folders == std::vector<std::string>{"auth", "users", "orders", "catalog"});
    const CollectionNode* orders = &shop->children[2];
    std::vector<std::string> names;
    for (const auto& c : orders->children) names.push_back(c.name);
    CHECK(names == std::vector<std::string>{"list-orders", "get-order", "create-order", "cancel-order", "refund-order",
                                            "ship-order"});
    // Depth first over tree() (what Run folder does): the tree's order.
    std::vector<std::string> run;
    auto flat = [&](auto& self, const CollectionNode& n) -> void {
      for (const auto& c : n.children)
        if (c.is_folder) self(self, c);
        else run.push_back(c.name);
    };
    flat(flat, *shop);
    CHECK(run == std::vector<std::string>{"login", "list-users", "list-orders", "get-order", "create-order", "cancel-order",
                                          "refund-order", "ship-order", "products"});
    // The export follows it too (no sidebar order handed in).
    const ExportResult ex = export_postman(*ws2, dir2 / "Shop API", "Shop API", nullptr);
    CHECK(ex.text.find("\"auth\"") < ex.text.find("\"users\""));
    CHECK(ex.text.find("\"users\"") < ex.text.find("\"orders\""));
    CHECK(ex.text.find("\"orders\"") < ex.text.find("\"catalog\""));
    CHECK(ex.text.find("List orders") < ex.text.find("Cancel order"));

    // A dragged order saved into the copy is read back; names that are paths are refused.
    REQUIRE(ws2->save_folder_order(dir2 / "Shop API" / "orders",
                                   {"ship-order.req.json", "..\\evil", "a/b", "..", "list-orders.req.json"}));
    CHECK(ws2->folder_order(dir2 / "Shop API" / "orders") ==
          std::vector<std::string>{"ship-order.req.json", "list-orders.req.json"});
    const CollectionNode again = ws2->tree(-1);
    for (const auto& c : again.children)
      if (c.name == "Shop API") {
        REQUIRE(c.children.size() == 4);
        REQUIRE(c.children[2].children.size() == 6);
        CHECK(c.children[2].children[0].name == "ship-order");
        CHECK(c.children[2].children[1].name == "list-orders");
        CHECK(c.children[2].children[2].name == "cancel-order");  // the rest by name
      }
    // An order of a folder that is gone does not re-create it.
    CHECK_FALSE(ws2->save_folder_order(dir2 / "nope", {"x.req.json"}));
    CHECK_FALSE(fs::exists(dir2 / "nope"));
    CHECK(ws2->save_folder_order(dir2 / "nope", {}));
  }
  fs::remove_all(dir2, ec);
}
