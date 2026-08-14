#include "actions/injection_config.hpp"
#include "actions/parsed_args.hpp"
#include "app_exception.hpp"
#include "doctest.h"

using namespace actions;

TEST_CASE("inject parses an operation, root, and ordered items") {
  InjectionConfig config;
  ToBeParsedArgs args;
  args.args = {"add", "/rootfs", "hosts", "/host/file:/etc/file"};

  config.parse(args);

  CHECK(config.operation == "add");
  CHECK(config.root == "/rootfs");
  CHECK((config.items ==
         std::vector<std::string>{"hosts", "/host/file:/etc/file"}));
}

TEST_CASE("inject list accepts JSON output") {
  InjectionConfig config;
  ToBeParsedArgs args;
  args.args = {"list", "/rootfs", "--json"};

  config.parse(args);

  CHECK(config.operation == "list");
  CHECK(config.json);
  CHECK(config.items.empty());
}

TEST_CASE("inject validates operation-specific arguments") {
  InjectionConfig add;
  add.operation = "add";
  add.root = "/tmp";
  REQUIRE_THROWS_AS(add.validate(), AppException);

  InjectionConfig clear;
  clear.operation = "clear";
  clear.root = "/tmp";
  clear.items = {"hosts"};
  REQUIRE_THROWS_AS(clear.validate(), AppException);

  InjectionConfig json;
  json.operation = "remove";
  json.root = "/tmp";
  json.items = {"hosts"};
  json.json = true;
  REQUIRE_THROWS_AS(json.validate(), AppException);
}
