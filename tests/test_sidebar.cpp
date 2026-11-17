// Sidebar file operations (src/app/sidebar_files.*): rename / duplicate / new request keep the
// kebab-case file convention and the "name" field in step, and a tab that follows a rename (the
// shell re-reads the name on file_moved) saves the new name, not the old one.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>

// No wx dependency: compiled into this test directly.
#include "app/sidebar_files.cpp"
#include "app/sidebar_fuzzy.cpp"

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
  CHECK(!rename_item(t.ws, t.dir / "a.req.json", "b", &err));
  CHECK(!err.empty());
  CHECK(t.ws.load_request(t.dir / "b.req.json")->name == "B");
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

TEST_CASE("filter rank: exact name before folder path matches", "[sidebar]") {
  const int create_order = filter_rank("create order", "Create order", "orders", "POST {{base}}/orders");
  const int create_refund = filter_rank("create order", "Create refund", "orders/refunds", "POST {{base}}/refunds");
  CHECK(create_order > create_refund);
  CHECK(create_refund >= 0);
  CHECK(filter_rank("zzz", "Create order", "orders", "") < 0);
  CHECK(filter_rank("order", "List orders", "orders", "") > filter_rank("order", "Get user", "orders", ""));
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
