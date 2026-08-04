#include "actions/archive_config.hpp"
#include "actions/parsed_args.hpp"
#include "app_exception.hpp"
#include "doctest.h"

using namespace actions;

TEST_CASE("pack parses source and destination in transfer order") {
  ToBeParsedArgs args;
  args.action_name = "pack";
  args.args = {"root", "rootfs.tar.zst"};
  PackConfig config;
  config.parse(args);
  CHECK(config.root == "root");
  CHECK(config.archive == "rootfs.tar.zst");
}

TEST_CASE("unpack defaults new rootfs trees to rich mapping") {
  ToBeParsedArgs args;
  args.action_name = "unpack";
  args.args = {"rootfs.tar", "root"};
  UnpackConfig config;
  config.parse(args);
  CHECK(config.archive == "rootfs.tar");
  CHECK(config.root == "root");
  CHECK_FALSE(config.native);
}

TEST_CASE("unpack accepts explicit native ownership") {
  ToBeParsedArgs args;
  args.action_name = "unpack";
  args.args = {"input.tar", "root", "--native"};
  UnpackConfig config;
  config.parse(args);
  CHECK(config.native);
}

TEST_CASE("unpack no longer exposes ownership-shape selection") {
  ToBeParsedArgs args;
  args.action_name = "unpack";
  args.args = {"input.tar", "root", "--idmap=user"};
  UnpackConfig config;
  REQUIRE_THROWS_AS(config.parse(args), std::invalid_argument);
}
