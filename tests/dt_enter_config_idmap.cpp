#include "doctest.h"
#include "actions/enter_config.hpp"
#include "actions/parsed_args.hpp"
#include "app_exception.hpp"

using namespace actions;

TEST_CASE("enter consumes a rootfs ownership mode from metadata") {
    ToBeParsedArgs args;
    args.action_name = "enter";
    args.args = {"/tmp/rootfs", "--", "/bin/true"};
    EnterConfig config;
    config.parse(args);

    CHECK(config.root == "/tmp/rootfs");
    CHECK_FALSE(config.single);
    CHECK_FALSE(config.native);
}

TEST_CASE("enter accepts explicit native ownership") {
    ToBeParsedArgs args;
    args.action_name = "enter";
    args.args = {"/tmp/rootfs", "--native", "--", "/bin/true"};
    EnterConfig config;
    config.parse(args);

    CHECK(config.native);
}

TEST_CASE("enter does not expose single or ID-map fallback options") {
    for (const auto& option : {"--rootless", "--idmap=user"}) {
        ToBeParsedArgs args;
        args.action_name = "enter";
        args.args = {"/tmp/rootfs", option, "--", "/bin/true"};
        EnterConfig config;
        REQUIRE_THROWS_AS(config.parse(args), std::invalid_argument);
    }
}

TEST_CASE("single has a distinct no-rootfs command surface") {
    ToBeParsedArgs args;
    args.action_name = "single";
    args.args = {"--cwd", "/tmp", "--env", "VALUE=ok", "--", "/bin/true"};
    SingleConfig config;
    config.parse(args);
    config.validate();

    CHECK(config.single);
    CHECK(config.root.empty());
    CHECK(config.cwdInRoot == "/tmp");
    REQUIRE(config.envVars.size() == 1);
    CHECK(config.envVars[0].first == "VALUE");
    CHECK(config.envVars[0].second == "ok");
}
