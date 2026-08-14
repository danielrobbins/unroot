#include "actions/archive_config.hpp"
#include "actions/parsed_args.hpp"
#include "app_exception.hpp"
#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace actions;

namespace {

class TempArchive {
public:
  TempArchive() {
    char pattern[] = "/tmp/unroot-archive-config-XXXXXX";
    int fd = ::mkstemp(pattern);
    REQUIRE(fd >= 0);
    REQUIRE(::close(fd) == 0);
    path = pattern;
  }

  ~TempArchive() { std::filesystem::remove(path); }

  std::filesystem::path path;
};

}  // namespace

TEST_CASE("pack parses source and destination in transfer order") {
  ToBeParsedArgs args;
  args.action_name = "pack";
  args.args = {"root", "rootfs.tar.zst"};
  PackConfig config;
  config.parse(args);
  CHECK(config.root == "root");
  CHECK(config.archive == "rootfs.tar.zst");
}

TEST_CASE("pack accepts explicit metadata-loss override") {
  ToBeParsedArgs args;
  args.action_name = "pack";
  args.args = {"root", "rootfs.tar.zst", "--force"};
  PackConfig config;
  config.parse(args);
  CHECK(config.force);
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
  CHECK(config.idCount == 65535);
  CHECK_FALSE(config.idCountSpecified);
}

TEST_CASE("unpack accepts a custom rich ID count") {
  ToBeParsedArgs args;
  args.action_name = "unpack";
  args.args = {"input.tar", "root", "--id-count=100000"};
  UnpackConfig config;
  config.parse(args);
  CHECK(config.idCount == 100000);
  CHECK(config.idCountSpecified);
}

TEST_CASE("unpack accepts the maximum mappable rich ID count") {
  ToBeParsedArgs args;
  args.action_name = "unpack";
  args.args = {"input.tar", "root", "--id-count=4294967294"};
  UnpackConfig config;
  config.parse(args);
  CHECK(config.idCount == 4294967294U);
  CHECK(config.idCountSpecified);
}

TEST_CASE("unpack rejects invalid rich ID counts") {
  for (const std::string value : {"0", "-1", "+1", "invalid",
                                  "4294967295", "4294967296"}) {
    ToBeParsedArgs args;
    args.action_name = "unpack";
    args.args = {"input.tar", "root", "--id-count", value};
    UnpackConfig config;
    REQUIRE_THROWS_AS(config.parse(args), AppException);
  }
}

TEST_CASE("unpack accepts explicit native ownership") {
  ToBeParsedArgs args;
  args.action_name = "unpack";
  args.args = {"input.tar", "root", "--native"};
  UnpackConfig config;
  config.parse(args);
  CHECK(config.native);
}

TEST_CASE("unpack rejects a rich ID count with native ownership") {
  TempArchive archive;
  ToBeParsedArgs args;
  args.action_name = "unpack";
  args.args = {archive.path.string(), "root", "--native", "--id-count",
               "100000"};
  UnpackConfig config;
  config.parse(args);
  bool rejected = false;
  try {
    config.validate();
  } catch (const AppException& error) {
    rejected = true;
    CHECK(std::string(error.what()).find(
              "--id-count cannot be combined with --native") !=
          std::string::npos);
  }
  CHECK(rejected);
}

TEST_CASE("unpack accepts explicit metadata-loss override") {
  ToBeParsedArgs args;
  args.action_name = "unpack";
  args.args = {"input.tar", "root", "--force"};
  UnpackConfig config;
  config.parse(args);
  CHECK(config.force);
}

TEST_CASE("unpack accepts subtractive default injection controls") {
  ToBeParsedArgs args;
  args.action_name = "unpack";
  args.args = {"input.tar", "root", "--inject=-hosts,-resolv.conf"};
  UnpackConfig config;
  config.parse(args);
  CHECK((config.disabledInjections ==
         std::vector<std::string>{"hosts", "resolv.conf"}));

  ToBeParsedArgs allArgs;
  allArgs.action_name = "unpack";
  allArgs.args = {"input.tar", "root", "--inject=-*"};
  UnpackConfig all;
  all.parse(allArgs);
  CHECK(all.disabledInjections == std::vector<std::string>{"*"});
}

TEST_CASE("unpack directs positive injections to the inject action") {
  for (const std::string value : {"hosts", "/host/file:/etc/file"}) {
    ToBeParsedArgs args;
    args.action_name = "unpack";
    args.args = {"input.tar", "root", "--inject", value};
    UnpackConfig config;
    REQUIRE_THROWS_AS(config.parse(args), AppException);
  }
}

TEST_CASE("unpack no longer exposes ownership-shape selection") {
  ToBeParsedArgs args;
  args.action_name = "unpack";
  args.args = {"input.tar", "root", "--idmap=user"};
  UnpackConfig config;
  REQUIRE_THROWS_AS(config.parse(args), std::invalid_argument);
}
